// EmotionHelper.exe: reads the feeling in a voice for MEC Cinematic Tools (GAME > Pose window > Voice > "From the
// voice"). The tools start it at idle priority, outside the game's process, with a job file; it writes what it heard
// in each stretch of the voice to a result file and ends. Nothing is played or recorded; it reads only the files it is
// given and the model next to it.
//
//   EmotionHelper.exe <job file> <result file> [progress file]
//
// The model: emotion_feel.onnx next to this program (Cinematic Tools\EmotionModel\, the optional download): audeering's
// wav2vec2-large-robust-12 emotion model (arousal, dominance, valence; CC BY-NC-SA 4.0) converted to ONNX, with a small
// head that gives the chances of 8 feelings (neutral, calm, happy, sad, angry, fearful, disgust, surprised), trained on
// the RAVDESS recordings (CC BY-NC-SA 4.0). It is run by Windows' own Windows ML on the CPU (2 threads), nothing
// installed. Measured on the user's PC: about 1.2 s to start, then about 1.5 - 1.9 s of 2 CPU threads for each 10 s it
// hears with the game closed (up to 2.9 s with it running), and about 1.7 GB of memory while it runs, whatever the
// voice's length (one stretch at a time).
//
// Job file (little-endian): "MECEMO1\0", uint32 rate (8000 - 200000), uint32 channels (1 or 2), uint64 frames,
// int16 samples[frames * channels] (interleaved), uint32 stretches, then per stretch two doubles: where to read from
// and to (seconds). It is deleted as soon as it has been read (also if this program is ended while reading it).
// Result file (UTF-8 text, written whole then renamed into place): "MECEMO1 ok <stretches>", then per stretch
// "<index> <arousal> <dominance> <valence> <8 chances>"; or "MECEMO1 error <why>".
// Progress file: "<stretches done> <stretches>", rewritten after each one.
#include <windows.h>
#include <unknwn.h>
#include <windows.ai.machinelearning.native.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.Metadata.h>
#include <winrt/Windows.AI.MachineLearning.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <new>
#include <numeric>
#include <string>
#include <vector>

using namespace winrt;
using namespace Windows::AI::MachineLearning;

namespace
{
  const int ModelRate = 16000;
  const int Feelings = 8;
  const int Threads = 2;
  const double MaxSeconds = 600.0;          // as voice::MaxSeconds: the tools never read more of a file
  const unsigned MaxStretches = 100000;
  const double ShortestRead = 0.1;          // s: a stretch read shorter than this gets no answer ("-")
  // The model's own answer for one second of silence (its model card's reference, [[0.5460754 0.6062266 0.40431657]]):
  // a check that the file next to this program is that model
  const float SilenceAdv[3] = { 0.5460754f, 0.6062266f, 0.40431657f };
  const float SilenceTolerance = 2e-3f;

  // ---- The resampler: to 16 kHz, band-limited. A windowed sinc (Kaiser, beta 8.6), cut-off 0.475 x the lower of
  // the two rates, 32 zero crossings each side: the same kernel as the offline reference (resample.py). The output's
  // positions are on the exact grid (rate / 16000 = M / L) when L <= MaxPhases, else rounded to 1 / MaxPhases of an
  // input sample (a rate like 44,057 Hz).
  const int Zeros = 32;
  const double Beta = 8.6;
  const double Rolloff = 0.95;
  const long long MaxPhases = 4096;

  double BesselI0(double x)
  {
    double sum = 1.0, term = 1.0, q = x * x / 4.0;
    for (int k = 1; k < 200; ++k)
    {
      term *= q / ((double)k * (double)k);
      sum += term;
      if (term < sum * 1e-17)
        break;
    }
    return sum;
  }

  bool To16k(std::vector<float> const& x, int rate, std::vector<float>& out)
  {
    out.clear();
    if (rate == ModelRate)
    {
      out = x;
      return true;
    }
    long long g = std::gcd((long long)rate, (long long)ModelRate);
    long long M = rate / g, L = ModelRate / g;
    long long P = L <= MaxPhases ? L : MaxPhases;
    double cut = 0.5 * (std::min)(rate, ModelRate) * Rolloff;
    double step = rate / (2.0 * cut);            // input samples between the sinc's zero crossings
    double half = Zeros * step;                  // the kernel's half width in input samples
    double gain = 2.0 * cut / rate;
    double i0Beta = BesselI0(Beta);
    // Each phase's taps: offsets j from the output's whole input position q, kernel at d = phase / P - j
    std::vector<long long> first((size_t)P);
    std::vector<size_t> start((size_t)P + 1);
    std::vector<double> taps;
    for (long long ph = 0; ph < P; ++ph)
    {
      double f = (double)ph / (double)P;
      long long lo = (long long)std::ceil(f - half), hi = (long long)std::floor(f + half);
      first[(size_t)ph] = lo;
      start[(size_t)ph] = taps.size();
      for (long long j = lo; j <= hi; ++j)
      {
        double d = f - (double)j;
        double h = 0.0;
        if (std::fabs(d) < half)
        {
          double u = d / half, s = d / step;
          double sinc = s == 0.0 ? 1.0 : std::sin(3.14159265358979323846 * s) / (3.14159265358979323846 * s);
          h = gain * sinc * BesselI0(Beta * std::sqrt((std::max)(0.0, 1.0 - u * u))) / i0Beta;
        }
        taps.push_back(h);
      }
    }
    start[(size_t)P] = taps.size();
    long long n = (long long)x.size();
    long long count = n * L / M;                 // floor(n * 16000 / rate)
    out.resize((size_t)count);
    for (long long o = 0; o < count; ++o)
    {
      long long num = o * M;
      long long q = num / L, r = num % L, ph = r;
      if (P != L)
      {
        ph = (long long)std::llround((double)r * (double)P / (double)L);
        if (ph >= P)
        {
          ph -= P;
          ++q;
        }
      }
      size_t a = start[(size_t)ph], b = start[(size_t)ph + 1];
      long long k = q + first[(size_t)ph];
      double sum = 0.0;
      for (size_t t = a; t < b; ++t, ++k)
        if (k >= 0 && k < n)
          sum += taps[t] * (double)x[(size_t)k];
      out[(size_t)o] = (float)sum;
    }
    return true;
  }

  // ---- Files
  std::wstring g_Result, g_Progress;

  bool WriteWhole(std::wstring const& path, std::string const& text)
  {
    std::wstring tmp = path + L".tmp";
    HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE)
      return false;
    DWORD wrote = 0;
    bool ok = WriteFile(f, text.data(), (DWORD)text.size(), &wrote, nullptr) && wrote == text.size();
    CloseHandle(f);
    if (!ok || !MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING))
    {
      DeleteFileW(tmp.c_str());
      return false;
    }
    return true;
  }

  int Fail(int code, std::string why)
  {
    for (char& c : why)
      if (c == '\r' || c == '\n')
        c = ' ';
    if (!g_Result.empty())
      WriteWhole(g_Result, "MECEMO1 error " + why + "\n");
    return code;
  }

  void Progress(unsigned done, unsigned total)
  {
    if (g_Progress.empty())
      return;
    char line[64];
    _snprintf_s(line, sizeof(line), _TRUNCATE, "%u %u\n", done, total);
    WriteWhole(g_Progress, line);
  }

  std::string Narrow(std::wstring const& w)
  {
    if (w.empty())
      return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s((size_t)(n > 0 ? n : 0), '\0');
    if (n > 0)
      WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
  }

  struct Job
  {
    int Rate = 0, Channels = 0;
    std::vector<float> Mono;                     // -1 .. 1
    std::vector<double> From, To;
  };

  // The job file, every field checked
  bool ReadJob(std::wstring const& path, Job& job, std::string& why)
  {
    // (deleted when this handle closes: as soon as it is read, or if this program is ended meanwhile; the tools have
    // closed theirs before starting it)
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ | DELETE, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
      FILE_FLAG_DELETE_ON_CLOSE | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (f == INVALID_HANDLE_VALUE)
    {
      why = "the job file can't be opened";
      return false;
    }
    LARGE_INTEGER size{};
    GetFileSizeEx(f, &size);
    auto read = [&](void* to, unsigned long long bytes) -> bool
    {
      unsigned char* p = (unsigned char*)to;
      while (bytes > 0)
      {
        DWORD chunk = bytes > (1u << 30) ? (1u << 30) : (DWORD)bytes, got = 0;
        if (!ReadFile(f, p, chunk, &got, nullptr) || got != chunk)
          return false;
        p += got;
        bytes -= got;
      }
      return true;
    };
    char magic[8];
    unsigned int rate = 0, channels = 0, stretches = 0;
    unsigned long long frames = 0;
    bool ok = read(magic, 8) && memcmp(magic, "MECEMO1\0", 8) == 0 && read(&rate, 4) && read(&channels, 4) && read(&frames, 8);
    if (!ok || rate < 8000 || rate > 200000 || (channels != 1 && channels != 2) || frames == 0 ||
      (double)frames > MaxSeconds * rate + 1.0 ||
      (unsigned long long)size.QuadPart < 24 + frames * channels * 2 + 4)
    {
      CloseHandle(f);
      why = "the job file isn't one this helper reads";
      return false;
    }
    std::vector<short> pcm((size_t)(frames * channels));
    ok = read(pcm.data(), (unsigned long long)pcm.size() * 2) && read(&stretches, 4) && stretches <= MaxStretches &&
      (unsigned long long)size.QuadPart == 24 + frames * channels * 2 + 4 + (unsigned long long)stretches * 16;
    if (ok)
    {
      job.From.resize(stretches);
      job.To.resize(stretches);
      double seconds = (double)frames / rate;
      for (unsigned s = 0; s < stretches && ok; ++s)
      {
        double ab[2];
        ok = read(ab, 16) && std::isfinite(ab[0]) && std::isfinite(ab[1]) && ab[0] >= 0.0 && ab[1] >= ab[0] &&
          ab[1] <= seconds + 1.0;
        job.From[s] = ab[0];
        job.To[s] = ab[1];
      }
    }
    CloseHandle(f);
    if (!ok)
    {
      why = "the job file is cut short or its stretches are out of the sound";
      return false;
    }
    job.Rate = (int)rate;
    job.Channels = (int)channels;
    job.Mono.resize((size_t)frames);
    for (size_t i = 0; i < (size_t)frames; ++i)
      job.Mono[i] = channels == 1 ? pcm[i] / 32768.f : (pcm[2 * i] + pcm[2 * i + 1]) / 65536.f;
    return true;
  }

  bool Evaluate(LearningModelSession& session, float const* x, size_t n, float adv[3], float feel[Feelings])
  {
    LearningModelBinding binding(session);
    binding.Bind(L"wav", TensorFloat::CreateFromArray({ 1, (int64_t)n }, array_view<float const>(x, x + n)));
    auto result = session.Evaluate(binding, L"");
    auto a = result.Outputs().Lookup(L"adv").as<TensorFloat>().GetAsVectorView();
    auto p = result.Outputs().Lookup(L"feel").as<TensorFloat>().GetAsVectorView();
    if (a.Size() != 3 || p.Size() != Feelings)
      return false;
    for (int i = 0; i < 3; ++i)
      adv[i] = a.GetAt(i);
    for (int i = 0; i < Feelings; ++i)
      feel[i] = p.GetAt(i);
    return true;
  }
}

int wmain(int argc, wchar_t** argv)
{
  if (argc < 3)
  {
    printf("usage: EmotionHelper.exe <job file> <result file> [progress file]\n");
    return 2;
  }
  g_Result = argv[2];
  if (argc > 3)
    g_Progress = argv[3];
  // Errors only in the result file, never a box
  SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
  try
  {
    Job job;
    std::string why;
    if (!ReadJob(argv[1], job, why))
      return Fail(3, why);
    std::vector<float> voice;
    To16k(job.Mono, job.Rate, voice);
    job.Mono = std::vector<float>();

    wchar_t self[MAX_PATH * 4];
    DWORD len = GetModuleFileNameW(nullptr, self, (DWORD)(sizeof(self) / sizeof(self[0])));
    if (len == 0 || len >= sizeof(self) / sizeof(self[0]))
      return Fail(4, "this program's own folder isn't known");
    std::wstring model(self, len);
    model = model.substr(0, model.find_last_of(L"\\/") + 1) + L"emotion_feel.onnx";
    if (GetFileAttributesW(model.c_str()) == INVALID_FILE_ATTRIBUTES)
      return Fail(4, "emotion_feel.onnx isn't next to EmotionHelper.exe");

    init_apartment(apartment_type::multi_threaded);
    LearningModel loaded = LearningModel::LoadFromFilePath(model);
    bool hasWav = false, hasAdv = false, hasFeel = false;
    for (auto const& f : loaded.InputFeatures())
      hasWav = hasWav || f.Name() == L"wav";
    for (auto const& f : loaded.OutputFeatures())
    {
      hasAdv = hasAdv || f.Name() == L"adv";
      hasFeel = hasFeel || f.Name() == L"feel";
    }
    if (!hasWav || !hasAdv || !hasFeel)
      return Fail(4, "emotion_feel.onnx isn't the emotion model (no wav / adv / feel)");
    // [FIX CHECK] Session options only where this Windows ML has them (its contract 2; closing the model's own copy:
    // contract 3, Windows 10 2004; 2 threads: where the native interface is there). An older Windows ML makes a plain
    // session (whether it reads this model at all was tested only on Windows 11 build 26300).
    LearningModelDevice device(LearningModelDeviceKind::Cpu);
    LearningModelSession session = [&]
    {
      if (!Windows::Foundation::Metadata::ApiInformation::IsApiContractPresent(
        L"Windows.AI.MachineLearning.MachineLearningContract", 2))
        return LearningModelSession(loaded, device);
      LearningModelSessionOptions options;
      if (auto close = options.try_as<ILearningModelSessionOptions2>())
        close.CloseModelOnSessionCreation(true);
      if (auto native = options.try_as<ILearningModelSessionOptionsNative>())
        native->SetIntraOpNumThreadsOverride(Threads);
      return LearningModelSession(loaded, device, options);
    }();

    // The check: one second of silence gives the model card's answer
    {
      std::vector<float> silence(ModelRate, 0.f);
      float adv[3], feel[Feelings];
      if (!Evaluate(session, silence.data(), silence.size(), adv, feel))
        return Fail(4, "the model's outputs aren't 3 and 8 values");
      for (int i = 0; i < 3; ++i)
        if (!(std::fabs(adv[i] - SilenceAdv[i]) <= SilenceTolerance))
        {
          char line[200];
          _snprintf_s(line, sizeof(line), _TRUNCATE, "emotion_feel.onnx isn't the expected model (silence gives %.4f %.4f "
            "%.4f, not 0.5461 0.6062 0.4043)", adv[0], adv[1], adv[2]);
          return Fail(4, line);
        }
    }

    unsigned total = (unsigned)job.From.size();
    std::string out = "MECEMO1 ok " + std::to_string(total) + "\n";
    Progress(0, total);
    for (unsigned s = 0; s < total; ++s)
    {
      long long n = (long long)voice.size();
      long long a = (long long)(job.From[s] * ModelRate), b = (long long)(job.To[s] * ModelRate);
      a = a < 0 ? 0 : (a > n ? n : a);
      b = b < a ? a : (b > n ? n : b);
      char line[400];
      float adv[3], feel[Feelings];
      if ((double)(b - a) < ShortestRead * ModelRate)
        _snprintf_s(line, sizeof(line), _TRUNCATE, "%u -\n", s);
      else
      {
        if (!Evaluate(session, voice.data() + a, (size_t)(b - a), adv, feel))
          return Fail(5, "the model's outputs aren't 3 and 8 values");
        int k = _snprintf_s(line, sizeof(line), _TRUNCATE, "%u %.6f %.6f %.6f", s, adv[0], adv[1], adv[2]);
        for (int i = 0; i < Feelings && k > 0; ++i)
          k += _snprintf_s(line + k, sizeof(line) - k, _TRUNCATE, " %.6f", feel[i]);
        strcat_s(line, "\n");
      }
      out += line;
      Progress(s + 1, total);
    }
    if (!WriteWhole(g_Result, out))
      return 6;
    return 0;
  }
  catch (hresult_error const& e)
  {
    char code[16];
    _snprintf_s(code, sizeof(code), _TRUNCATE, "0x%08X", (unsigned)e.code());
    return Fail(5, std::string("Windows ML: ") + code + " " + Narrow(std::wstring(e.message().c_str())));
  }
  catch (std::bad_alloc const&)
  {
    return Fail(5, "not enough memory");
  }
  catch (...)
  {
    return Fail(5, "an unexpected error");
  }
}
