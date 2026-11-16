#include "VoiceEmotion.h"
#include "Voice.h"
#include "../Util/Util.h"
#include <Windows.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <locale>
#include <mutex>
#include <sstream>
#include <thread>

namespace voice_emotion
{
  const char* const ClassNames[ClassCount] = { "neutral", "calm", "happy", "sad", "angry", "fearful", "disgust", "surprised" };

  int PresetOf(int cls)
  {
    switch (cls)
    {
    case Calm: return voice_feel::Calm;
    case Happy: return voice_feel::Happy;
    case Sad: return voice_feel::Sad;
    case Angry: return voice_feel::Angry;
    case Fearful: return voice_feel::Afraid;
    case Disgust: return voice_feel::Disgusted;
    case Surprised: return voice_feel::Surprised;
    default: return -1;   // neutral (and anything else): the feeling picked
    }
  }

  std::vector<Stretch> StretchesOf(std::vector<double> const& phraseFrom, std::vector<double> const& phraseTo, double seconds)
  {
    std::vector<Stretch> out;
    if (!(seconds > 0.0) || !std::isfinite(seconds))
      return out;
    std::vector<std::pair<double, double>> phrases;
    size_t n = (std::min)(phraseFrom.size(), phraseTo.size());
    for (size_t i = 0; i < n; ++i)
    {
      double a = phraseFrom[i], b = phraseTo[i];
      if (!std::isfinite(a) || !std::isfinite(b))
        continue;
      a = a < 0.0 ? 0.0 : (a > seconds ? seconds : a);
      b = b < 0.0 ? 0.0 : (b > seconds ? seconds : b);
      if (b > a)
        phrases.push_back(std::make_pair(a, b));
    }
    std::sort(phrases.begin(), phrases.end());
    // What a span [a, b] (a phrase, or phrases heard together) is heard as: itself, or ShortestHeard centred on it,
    // kept within the voice
    auto window = [seconds](double a, double b, double& from, double& to)
    {
      double middle = (a + b) * 0.5, half = (std::max)((b - a) * 0.5, ShortestHeard * 0.5);
      from = middle - half;
      to = middle + half;
      if (from < 0.0)
      {
        to = (std::min)(seconds, to - from);
        from = 0.0;
      }
      if (to > seconds)
      {
        from = (std::max)(0.0, from - (to - seconds));
        to = seconds;
      }
    };
    for (size_t p = 0; p < phrases.size(); ++p)
    {
      double a = phrases[p].first, b = phrases[p].second, length = b - a;
      if (length > LongPhrase)
      {
        int parts = (int)std::ceil(length / PartLength);
        for (int i = 0; i < parts; ++i)
        {
          Stretch s;
          s.From = s.HeardFrom = a + length * i / parts;
          s.To = s.HeardTo = i + 1 == parts ? b : a + length * (i + 1) / parts;
          s.Phrase = (int)p;
          s.Part = i;
          s.Parts = parts;
          out.push_back(s);
        }
        continue;
      }
      // [HEARD TOGETHER] A short phrase takes the short phrases after it that begin inside what it is heard as (the
      // group's window growing with each), up to LongPhrase from its start; a phrase alone is heard exactly as before
      size_t last = p;
      double groupTo = b, from = 0.0, to = 0.0;
      window(a, groupTo, from, to);
      while (groupTo - a < ShortestHeard && last + 1 < phrases.size())
      {
        double nextFrom = phrases[last + 1].first, nextTo = phrases[last + 1].second;
        // ([FIX CHECK] "shorter" by more than a microsecond: a phrase of 3.00 s on the analysis' 10 ms grid can come out
        // 2.9999999999999996 s long, and was taken in though it is heard by itself)
        if (!(nextFrom < to) || !(nextTo - nextFrom < ShortestHeard - 1e-6) ||
          !((std::max)(groupTo, nextTo) - a <= LongPhrase))
          break;
        ++last;
        groupTo = (std::max)(groupTo, nextTo);
        window(a, groupTo, from, to);
      }
      for (size_t q = p; q <= last; ++q)
      {
        Stretch s;
        s.From = phrases[q].first;
        s.To = phrases[q].second;
        s.Phrase = (int)q;
        s.HeardFrom = from;
        s.HeardTo = to;
        out.push_back(s);
      }
      p = last;
    }
    return out;
  }

  void WindowsOf(std::vector<Stretch> const& stretches, std::vector<std::pair<double, double>>& windows,
    std::vector<size_t>& windowOf)
  {
    windows.clear();
    windowOf.assign(stretches.size(), 0);
    for (size_t i = 0; i < stretches.size(); ++i)
    {
      Stretch const& s = stretches[i];
      if (windows.empty() || windows.back().first != s.HeardFrom || windows.back().second != s.HeardTo)
        windows.push_back(std::make_pair(s.HeardFrom, s.HeardTo));
      windowOf[i] = windows.size() - 1;
    }
  }

  static float Smooth(float edge0, float edge1, float x)
  {
    float t = (x - edge0) / (edge1 - edge0);
    t = !(t > 0.f) ? 0.f : (t > 1.f ? 1.f : t);
    return t * t * (3.f - 2.f * t);
  }

  Look LookOf(Reading const* reading, int setting, float const* picked, float sure)
  {
    const int count = LookSlots;
    Look look;
    // The feeling picked, as a mix that sums to 1 (none, if it holds nothing)
    float base[count] = {};
    float sum = 0.f;
    for (int f = 0; f < count; ++f)
    {
      base[f] = picked && picked[f] > 0.f && std::isfinite(picked[f]) ? picked[f] : 0.f;
      sum += base[f];
    }
    if (!(sum > 0.f))
    {
      base[0] = 1.f;
      sum = 1.f;
    }
    for (int f = 0; f < count; ++f)
      base[f] /= sum;
    if (setting == NoFeeling)
    {
      look.Weights[0] = look.Units[0] = 1.f;
      look.Share = 1.f;
      look.ByHand = true;
      return look;
    }
    if (setting >= 0 && setting < voice_feel::FeelingCount)
    {
      look.Weights[setting + 1] = look.Units[setting + 1] = 1.f;
      look.Shown = setting;
      look.Share = 1.f;
      look.ByHand = true;
      return look;
    }
    for (int f = 0; f < count; ++f)
      look.Weights[f] = look.Units[f] = base[f];
    look.ByHand = setting == AsPicked;
    if (setting == AsPicked || !reading || !reading->Ok)
      return look;
    // From the voice: its likeliest feeling, as far as the model is sure of it
    int top = 0;
    for (int c = 1; c < ClassCount; ++c)
      if (reading->Chance[c] > reading->Chance[top])
        top = c;
    float chance = reading->Chance[top];
    int preset = PresetOf(top);
    if (preset < 0 || !(chance >= 0.f && chance <= 1.001f))
      return look;
    sure = !(sure >= SureLeast) ? SureLeast : (sure > SureMost ? SureMost : sure);
    float top1 = sure + SureSpan < 1.f ? sure + SureSpan : 1.f;   // (a chance of 1 always shows it all)
    float share = Smooth(sure, top1, chance);
    // A lively feeling's own part a little stronger or weaker by the voice's arousal (the feeling picked as it is)
    float k = 1.f;
    bool lively = preset == voice_feel::Happy || preset == voice_feel::Angry || preset == voice_feel::Afraid ||
      preset == voice_feel::Surprised || preset == voice_feel::Disgusted;
    if (lively && std::isfinite(reading->Arousal))
      k = StrengthLeast + (StrengthMost - StrengthLeast) * Smooth(ArousalLow, ArousalHigh, reading->Arousal);
    for (int f = 0; f < count; ++f)
    {
      look.Weights[f] *= 1.f - share;
      look.Units[f] *= 1.f - share;
    }
    look.Weights[preset + 1] += share;
    look.Units[preset + 1] += share * k;
    look.Shown = preset;
    look.Share = share;
    look.Scale = k;
    return look;
  }

  Why WhyOf(Reading const* reading, Look const& look)
  {
    if (look.ByHand)
      return WhyByHand;
    if (!reading || !reading->Ok)
      return WhyTooShort;
    int top = 0;   // (LookOf's own rule: the first of equal chances)
    for (int c = 1; c < ClassCount; ++c)
      if (reading->Chance[c] > reading->Chance[top])
        top = c;
    if (PresetOf(top) < 0)
      return WhyNeutral;
    if (look.Shown >= 0 && look.Share >= 0.5f)
      return WhyShown;
    if (look.Shown >= 0 && look.Share > 0.f)
      return WhyPartly;
    return WhyNotSure;
  }

  void ChancesInOrder(Reading const& reading, int order[ClassCount])
  {
    for (int c = 0; c < ClassCount; ++c)
      order[c] = c;
    // (a stable insertion sort: equal chances stay in the model's order; a chance that isn't a number goes last)
    auto above = [&](int a, int b)
    {
      float x = reading.Chance[a], y = reading.Chance[b];
      bool xn = x == x, yn = y == y;
      return xn && (!yn || x > y);
    };
    for (int i = 1; i < ClassCount; ++i)
      for (int j = i; j > 0 && above(order[j], order[j - 1]); --j)
      {
        int t = order[j];
        order[j] = order[j - 1];
        order[j - 1] = t;
      }
  }

  bool MixAt(std::vector<Stretch> const& stretches, std::vector<Look> const& looks, double at, double changeSeconds,
    double loopLength, float* weights, float* units, float& share, int& stretch, float& stretchWeight)
  {
    const int count = LookSlots;
    size_t n = (std::min)(stretches.size(), looks.size());
    stretch = -1;
    share = 0.f;
    stretchWeight = 0.f;
    if (n == 0)
      return false;
    if (!std::isfinite(at))
      at = 0.0;
    // (a voice shorter than one change isn't blended across its loop's end)
    double halfChange = changeSeconds > 0.0 ? changeSeconds * 0.5 : 0.0;
    bool loop = loopLength > 0.0 && std::isfinite(loopLength) && n >= 2 && loopLength >= 2.0 * halfChange;
    if (loop)
    {
      at = fmod(at, loopLength);
      if (at < 0.0)
        at += loopLength;
    }
    // How far a change centred at a boundary has gone at a time (0 before it, 1 after)
    auto through = [&](double time, double boundary)
    {
      if (!(changeSeconds > 0.0))
        return time >= boundary ? 1.0 : 0.0;
      double t = (time - boundary) / changeSeconds + 0.5;
      return !(t > 0.0) ? 0.0 : (t > 1.0 ? 1.0 : t);
    };
    auto boundaryAfter = [&](size_t i, double lastBoundary)
    {
      double next = stretches[i + 1].From, end = stretches[i].To;
      double boundary = (std::max)(next - Lead, (end + next) * 0.5);
      // [FIX CHECK] Looping: within the loop, half a change from either end (a change cut by the loop's end would
      // jump there: one side of it seen before the end, the other after the start)
      if (loop)
        boundary = (std::min)((std::max)(boundary, halfChange), loopLength - halfChange);
      return (std::max)(boundary, lastBoundary);   // (never before the one before it)
    };
    // Looping: the change from the last stretch into the first, centred at its own boundary across the loop's end
    // (the first stretch's start one loop later); it moves the last's weight to the first at the end, and the first's
    // to the last at the start, by the same ramp, so the face is the same on both sides of the loop
    double toFirstAtEnd = 0.0, keptByFirst = 1.0;
    if (loop)
    {
      double lastBoundary = -1e300;
      for (size_t i = 0; i + 1 < n; ++i)
        lastBoundary = boundaryAfter(i, lastBoundary);
      double next = stretches[0].From + loopLength, end = stretches[n - 1].To;
      double wrap = (std::max)((std::max)(next - Lead, (end + next) * 0.5), lastBoundary);
      toFirstAtEnd = through(at, wrap);
      keptByFirst = through(at + loopLength, wrap);
    }
    for (int f = 0; f < count; ++f)
      weights[f] = units[f] = 0.f;
    double before = 1.0;       // the change into this stretch, done
    double lastBoundary = -1e300;
    float most = -1.f;
    double firstMoved = 0.0, lastMoved = 0.0, firstW = 0.0, lastW = 0.0;
    auto add = [&](size_t i, double w)
    {
      if (!(w > 0.0))
        return;
      float fw = (float)w;
      for (int f = 0; f < count; ++f)
      {
        weights[f] += fw * looks[i].Weights[f];
        units[f] += fw * looks[i].Units[f];
      }
      share += fw * looks[i].Share;
    };
    for (size_t i = 0; i < n; ++i)
    {
      double after = 0.0;      // the change out of it, done
      if (i + 1 < n)
      {
        lastBoundary = boundaryAfter(i, lastBoundary);
        after = through(at, lastBoundary);
      }
      double w = before - after;
      before = after;
      if (loop && i == 0)
      {
        firstMoved = w * (1.0 - keptByFirst);   // to the last, at the loop's start
        w -= firstMoved;
      }
      if (loop && i == n - 1)
      {
        lastMoved = w * toFirstAtEnd;           // to the first, at the loop's end
        w -= lastMoved;
      }
      add(i, w);
      if (i == 0)
        firstW = w;
      else if (i == n - 1)
        lastW = w;
      else if ((float)w > most)
      {
        most = (float)w;
        stretch = (int)i;
      }
    }
    if (loop)
    {
      add(n - 1, firstMoved);
      add(0, lastMoved);
      firstW += lastMoved;
      lastW += firstMoved;
    }
    if ((float)firstW > most)
    {
      most = (float)firstW;
      stretch = 0;
    }
    if (n > 1 && (float)lastW > most)
    {
      most = (float)lastW;
      stretch = (int)n - 1;
    }
    stretchWeight = most > 1.f ? 1.f : (most < 0.f ? 0.f : most);
    return true;
  }

  bool ParseResult(std::string const& text, size_t stretches, std::vector<Reading>& readings, std::string& why)
  {
    readings.clear();
    std::istringstream in(text);
    in.imbue(std::locale::classic());
    std::string line;
    if (!std::getline(in, line))
    {
      why = "the helper's answer is empty";
      return false;
    }
    if (line.compare(0, 14, "MECEMO1 error ") == 0)
    {
      why = line.substr(14);
      return false;
    }
    {
      std::istringstream head(line);
      head.imbue(std::locale::classic());
      std::string magic, ok;
      size_t count = 0;
      if (!(head >> magic >> ok >> count) || magic != "MECEMO1" || ok != "ok" || count != stretches)
      {
        why = "the helper's answer isn't for this voice";
        return false;
      }
    }
    readings.resize(stretches);
    for (size_t s = 0; s < stretches; ++s)
    {
      if (!std::getline(in, line))
      {
        why = "the helper's answer is cut short";
        readings.clear();
        return false;
      }
      std::istringstream row(line);
      row.imbue(std::locale::classic());
      size_t index = 0;
      if (!(row >> index) || index != s)
      {
        why = "the helper's answer is out of order";
        readings.clear();
        return false;
      }
      std::string rest;
      std::getline(row, rest);
      if (rest == " -")
        continue;
      std::istringstream values(rest);
      values.imbue(std::locale::classic());
      float v[3 + ClassCount];
      bool ok = true;
      float sum = 0.f;
      for (int i = 0; i < 3 + ClassCount && ok; ++i)
        ok = (bool)(values >> v[i]) && std::isfinite(v[i]);
      std::string extra;
      if (ok && (values >> extra))
        ok = false;
      for (int c = 0; c < ClassCount && ok; ++c)
      {
        ok = v[3 + c] >= 0.f && v[3 + c] <= 1.001f;
        sum += v[3 + c];
      }
      if (!ok || !(sum > 0.98f && sum < 1.02f))
      {
        why = "the helper's answer has a value out of its range";
        readings.clear();
        return false;
      }
      Reading& r = readings[s];
      r.Ok = true;
      r.Arousal = v[0];
      r.Dominance = v[1];
      r.Valence = v[2];
      for (int c = 0; c < ClassCount; ++c)
        r.Chance[c] = v[3 + c];
    }
    return true;
  }

  // ---- The helper
  const wchar_t* const Folder = L"Cinematic Tools\\EmotionModel";
  const wchar_t* const HelperFile = L"EmotionHelper.exe";
  const wchar_t* const ModelFile = L"emotion_feel.onnx";
  const ULONGLONG AvailableRecheckMs = 2000;
  const double WaitLeastSeconds = 120.0;      // [d] the helper is given this long, plus
  const double WaitPerHeardSecond = 3.0;      // [d] ... this for each second it hears (measured at idle priority on 2
                                              // threads: 0.15 - 0.29 s, the game closed or running)
  const DWORD PollMs = 250;
  const long long MaxHelperBytes = 64ll << 20; // the helper's own file, hashed into the cache's key (it is about 0.8 MB)

  struct Request
  {
    mutable std::mutex Mutex;
    std::shared_ptr<voice::Sound const> Sound;   // let go once its job is written, or at once when cancelled waiting
                                                 // (its thread's own hold: once the 8 MB chunk it is hashing is done)
    std::vector<Stretch> Stretches;
    std::string File;                            // [LOG] the sound's file (set by Start, never changed)
    State Now = Waiting;
    int Done = 0, Total = 0;
    bool FromCache = false;
    bool CanRetry = true;
    double Seconds = 0.0;
    std::string Message;
    std::shared_ptr<std::vector<Reading> const> Readings;
    ULONGLONG RunningAt = 0;                     // [PROGRESS] when it began to run (0: not yet)
    bool ModelReady = false;                     // [PROGRESS] the helper's first progress was read
    std::atomic<bool> CancelWanted{ false };
  };

  // [MEMORY] Windows' commit left to give (the tests replace it)
  namespace
  {
    unsigned long long CommitLeftNow()
    {
      MEMORYSTATUSEX m{};
      m.dwLength = sizeof(m);
      return GlobalMemoryStatusEx(&m) ? m.ullAvailPageFile : ~0ull;
    }
  }
  unsigned long long (*CommitLeft)() = CommitLeftNow;

  namespace
  {
    std::mutex g_AvailableMutex;
    ULONGLONG g_AvailableAt = 0;
    bool g_AvailableWas = false;
    std::string g_AvailableWhy;
    std::timed_mutex g_OneAtATime;            // one helper at a time (it holds about 1.7 GB while it runs)
    std::mutex g_JobMutex;
    HANDLE g_KillOnClose = nullptr;           // the helpers die with the game (never closed: it closes with the process)
    std::once_flag g_CleanedUp;

    bool IsFile(std::wstring const& path)
    {
      DWORD a = GetFileAttributesW(path.c_str());
      return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
    }

    std::wstring Full(std::wstring const& path)
    {
      wchar_t buffer[2048];
      DWORD n = GetFullPathNameW(path.c_str(), 2048, buffer, nullptr);
      return n > 0 && n < 2048 ? std::wstring(buffer, n) : path;
    }

    unsigned long long Fnv(unsigned long long h, void const* data, size_t bytes)
    {
      unsigned char const* p = (unsigned char const*)data;
      for (size_t i = 0; i < bytes; ++i)
      {
        h ^= p[i];
        h *= 1099511628211ull;
      }
      return h;
    }

    bool ReadText(std::wstring const& path, std::string& text, size_t most)
    {
      text.clear();
      HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
      if (f == INVALID_HANDLE_VALUE)
        return false;
      LARGE_INTEGER size{};
      bool ok = GetFileSizeEx(f, &size) && size.QuadPart >= 0 && (unsigned long long)size.QuadPart <= most;
      if (ok)
      {
        text.resize((size_t)size.QuadPart);
        DWORD got = 0;
        ok = text.empty() || (ReadFile(f, &text[0], (DWORD)text.size(), &got, nullptr) && got == text.size());
      }
      CloseHandle(f);
      if (!ok)
        text.clear();
      return ok;
    }

    // The helper's progress file ("<done> <total>") with no allocation at all (it is read every PollMs while the helper
    // runs: an exception there would leave the helper unwatched)
    bool ReadProgress(std::wstring const& path, int& done)
    {
      HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
      if (f == INVALID_HANDLE_VALUE)
        return false;
      char line[64];
      DWORD got = 0;
      bool ok = ReadFile(f, line, sizeof(line) - 1, &got, nullptr) != 0;
      CloseHandle(f);
      if (!ok)
        return false;
      line[got] = 0;
      int total = 0;
      return sscanf_s(line, "%d %d", &done, &total) == 2;
    }

    // A file's own bytes hashed in (false: it couldn't be read whole)
    bool HashFile(std::wstring const& path, unsigned long long& h)
    {
      HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN,
        nullptr);
      if (f == INVALID_HANDLE_VALUE)
        return false;
      LARGE_INTEGER size{};
      bool ok = GetFileSizeEx(f, &size) && size.QuadPart > 0 && size.QuadPart <= MaxHelperBytes;
      h = Fnv(h, &size.QuadPart, sizeof(size.QuadPart));
      std::vector<unsigned char> chunk(ok ? 65536 : 0);
      long long left = ok ? size.QuadPart : 0;
      while (ok && left > 0)
      {
        DWORD want = left > (long long)chunk.size() ? (DWORD)chunk.size() : (DWORD)left, got = 0;
        ok = ReadFile(f, chunk.data(), want, &got, nullptr) && got == want;
        h = Fnv(h, chunk.data(), got);
        left -= got;
      }
      CloseHandle(f);
      return ok;
    }

    bool WriteAll(HANDLE f, void const* data, unsigned long long bytes)
    {
      unsigned char const* p = (unsigned char const*)data;
      while (bytes > 0)
      {
        DWORD chunk = bytes > (1u << 24) ? (1u << 24) : (DWORD)bytes, wrote = 0;
        if (!WriteFile(f, p, chunk, &wrote, nullptr) || wrote != chunk)
          return false;
        p += chunk;
        bytes -= chunk;
      }
      return true;
    }

    // What a game that ended while a helper ran left behind (a job file holds the whole sound: up to about 256 MB).
    // Once a session, before its first reading (Available's first call, or Run's).
    void CleanUp()
    {
      std::call_once(g_CleanedUp, []
      {
        std::wstring cache = std::wstring(Folder) + L"\\cache";
        for (const wchar_t* pattern : { L"\\*.job", L"\\*.progress", L"\\*.tmp" })
        {
          WIN32_FIND_DATAW found;
          HANDLE h = FindFirstFileW((cache + pattern).c_str(), &found);
          if (h == INVALID_HANDLE_VALUE)
            continue;
          do
            if (!(found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
              DeleteFileW((cache + L"\\" + found.cFileName).c_str());
          while (FindNextFileW(h, &found));
          FindClose(h);
        }
      });
    }

    HANDLE KillOnClose()
    {
      std::lock_guard<std::mutex> lock(g_JobMutex);
      if (!g_KillOnClose)
      {
        HANDLE job = CreateJobObjectW(nullptr, nullptr);
        if (job)
        {
          JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
          limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
          if (SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits)))
            g_KillOnClose = job;
          else
            CloseHandle(job);
        }
      }
      return g_KillOnClose;
    }

    void Finish(Request& r, State state, std::string const& message, std::shared_ptr<std::vector<Reading> const> readings,
      bool fromCache, double seconds, bool canRetry = true)
    {
      std::lock_guard<std::mutex> lock(r.Mutex);
      r.Now = state;
      r.Message = message;
      r.Readings = readings;
      r.FromCache = fromCache;
      r.Seconds = seconds;
      r.CanRetry = canRetry;
      r.Sound.reset();
      if (state == Done)
        r.Done = r.Total;
    }

    // [LOG] Failed: the state, then one warning (written after the request's lock is let go: the game's thread asks for
    // the state every frame)
    void Fail(Request& r, std::string const& message, double seconds, bool canRetry = true)
    {
      Finish(r, Failed, message, nullptr, false, seconds, canRetry);
      util::log::Warning("Pose: the voice %s: its feeling couldn't be read: %s", r.File.c_str(), message.c_str());
    }

    // [LOG] Stopped (Cancel: another voice, Remove, Let go, Stop): before the helper began, or after it ran
    void Stop(Request& r, bool began, double seconds, int done)
    {
      Finish(r, Cancelled, "", nullptr, false, seconds);
      if (began)
        util::log::Write("Pose: the voice %s: its feeling's reading stopped after %.1f s (%d of %d stretches read)",
          r.File.c_str(), seconds, done, r.Total);
      else
        util::log::Write("Pose: the voice %s: its feeling's reading stopped before the helper began", r.File.c_str());
    }

    // [LOG] Done: one line with its cache file, then what each stretch was heard as (diagnostic lines, at most 100)
    void SayDone(Request const& r, std::vector<Reading> const& readings, wchar_t const* key, bool fromCache,
      double seconds, double heard, double waited)
    {
      if (fromCache)
        util::log::Write("Pose: the voice %s: its feeling, %u stretches, kept from an earlier reading (cache\\%ls.txt)",
          r.File.c_str(), (unsigned)r.Stretches.size(), key);
      else
      {
        // ([REVIEW] its own time, from its turn; the wait for another voice's reading apart)
        char wait[64] = "";
        if (waited >= 0.5)
          _snprintf_s(wait, sizeof(wait), _TRUNCATE, ", after waiting %.1f s for another voice's reading", waited);
        util::log::Write("Pose: the voice %s: its feeling read by EmotionHelper.exe, %u stretches (%.1f s heard) in %.1f s%s "
          "(cache\\%ls.txt)", r.File.c_str(), (unsigned)r.Stretches.size(), heard, seconds, wait, key);
      }
      // (the stretch-by-stretch lines are research: only with [Debug] Diagnostics on)
      if (!util::DiagnosticsOn())
        return;
      const size_t most = 100;
      size_t n = (std::min)(readings.size(), r.Stretches.size());
      for (size_t i = 0; i < n && i < most; ++i)
      {
        Stretch const& s = r.Stretches[i];
        Reading const& g = readings[i];
        if (!g.Ok)
        {
          util::log::Write("Pose: the voice %s: its feeling (diagnostic): stretch %u of %u, %.2f - %.2f s: too short to "
            "hear", r.File.c_str(), (unsigned)(i + 1), (unsigned)n, s.From, s.To);
          continue;
        }
        int order[ClassCount];
        ChancesInOrder(g, order);
        util::log::Write("Pose: the voice %s: its feeling (diagnostic): stretch %u of %u, %.2f - %.2f s (heard %.2f - %.2f "
          "s): %s %.1f %%, %s %.1f %%, %s %.1f %%; arousal %.3f, dominance %.3f, valence %.3f", r.File.c_str(),
          (unsigned)(i + 1), (unsigned)n, s.From, s.To, s.HeardFrom, s.HeardTo, ClassNames[order[0]],
          100.f * g.Chance[order[0]], ClassNames[order[1]], 100.f * g.Chance[order[1]], ClassNames[order[2]],
          100.f * g.Chance[order[2]], g.Arousal, g.Dominance, g.Valence);
      }
      if (n > most)
        util::log::Write("Pose: the voice %s: its feeling (diagnostic): %u stretches more (all of them in cache\\%ls.txt, "
          "a line for each window heard)", r.File.c_str(), (unsigned)(n - most), key);
    }

    // A helper started: ended (if it hasn't ended by itself) and its handle closed on every way out of Run, its job and
    // progress files deleted
    struct HelperProcess
    {
      HANDLE Process = nullptr;
      std::wstring const* Job = nullptr;
      std::wstring const* Progress = nullptr;
      ~HelperProcess()
      {
        if (Process)
        {
          if (WaitForSingleObject(Process, 0) == WAIT_TIMEOUT)
          {
            TerminateProcess(Process, 1);
            WaitForSingleObject(Process, 5000);
          }
          CloseHandle(Process);
        }
        if (Job)
          DeleteFileW(Job->c_str());
        if (Progress)
          DeleteFileW(Progress->c_str());
      }
    };

    void Run(std::shared_ptr<Request> request)
    {
      Request& r = *request;
      SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
      ULONGLONG started = GetTickCount64();
      auto since = [started] { return (GetTickCount64() - started) / 1000.0; };
      try
      {
        std::shared_ptr<voice::Sound const> held;
        {
          std::lock_guard<std::mutex> lock(r.Mutex);
          if (!r.CancelWanted)
            held = r.Sound;
        }
        if (!held)
        {
          Stop(r, false, 0.0, 0);
          return;
        }
        std::wstring folder = Full(Folder), cache = folder + L"\\cache";
        std::wstring helper = folder + L"\\" + HelperFile, model = folder + L"\\" + ModelFile;
        if (!IsFile(helper) || !IsFile(model))
        {
          Fail(r, "the emotion model's files aren't in Cinematic Tools\\EmotionModel", 0.0);
          return;
        }
        CreateDirectoryW(cache.c_str(), nullptr);
        CleanUp();
        unsigned int rate = (unsigned int)held->Rate, channels = (unsigned int)held->Channels;
        if (held->Rate < RateLeast || held->Rate > RateMost || (channels != 1 && channels != 2) || !held->Pcm ||
          held->Pcm->size() < channels)
        {
          char message[200];
          _snprintf_s(message, sizeof(message), _TRUNCATE, "the voice is %d Hz with %d channel%s; its feeling is read at "
            "%d - %d Hz, one or two channels (save it again at such a rate; how right it is on a voice under 16 kHz isn't "
            "known)", held->Rate, held->Channels,
            held->Channels == 1 ? "" : "s", RateLeast, RateMost);
          Fail(r, message, 0.0, false);
          return;
        }
        // [HEARD TOGETHER] What the helper hears: each window once (its progress counts windows: doneAt[k] is how many
        // stretches are read once k windows are)
        std::vector<std::pair<double, double>> windows;
        std::vector<size_t> windowOf;
        WindowsOf(r.Stretches, windows, windowOf);
        double heard = 0.0;
        for (auto const& w : windows)
          heard += w.second - w.first;
        std::vector<int> doneAt(windows.size() + 1, 0);
        for (size_t k = 0, i = 0; k <= windows.size(); ++k)
        {
          while (i < windowOf.size() && windowOf[i] < k)
            ++i;
          doneAt[k] = (int)i;
        }
        // The answers are kept by what was heard and what heard it: the sound, the windows, the helper's own bytes
        // and the model file's size and time (a voice whose stretches are each heard alone keeps its earlier key)
        unsigned long long frames = held->Pcm->size() / channels;
        unsigned long long h = 14695981039346656037ull;
        h = Fnv(h, "MECEMO1 key 2", 13);
        h = Fnv(h, &rate, 4);
        h = Fnv(h, &channels, 4);
        h = Fnv(h, &frames, 8);
        // [REVIEW] In chunks, a Stop looked at between them (the same key: FNV runs byte by byte): a voice at the
        // decoder's cap takes about 0.2 s to hash, and a Stop meanwhile lets its sound go within a chunk's time
        {
          unsigned char const* bytes = (unsigned char const*)held->Pcm->data();
          size_t left = (size_t)(frames * channels) * sizeof(short);
          const size_t chunk = 8u << 20;
          while (left > 0)
          {
            if (r.CancelWanted)
            {
              held.reset();
              Stop(r, false, since(), 0);
              return;
            }
            size_t n = left < chunk ? left : chunk;
            h = Fnv(h, bytes, n);
            bytes += n;
            left -= n;
          }
        }
        for (auto const& w : windows)
        {
          h = Fnv(h, &w.first, 8);
          h = Fnv(h, &w.second, 8);
        }
        if (!HashFile(helper, h))
        {
          Fail(r, "Cinematic Tools\\EmotionModel\\EmotionHelper.exe couldn't be read", 0.0);
          return;
        }
        WIN32_FILE_ATTRIBUTE_DATA modelData{};
        if (GetFileAttributesExW(model.c_str(), GetFileExInfoStandard, &modelData))
        {
          h = Fnv(h, &modelData.nFileSizeLow, 4);
          h = Fnv(h, &modelData.nFileSizeHigh, 4);
          h = Fnv(h, &modelData.ftLastWriteTime, sizeof(FILETIME));
        }
        wchar_t key[17];
        swprintf_s(key, L"%016llx", h);
        std::wstring result = cache + L"\\" + key + L".txt", job = cache + L"\\" + key + L".job",
          progress = cache + L"\\" + key + L".progress";
        std::string text, why;
        std::vector<Reading> readings;
        // One reading per stretch from one per window
        auto spread = [&](std::vector<Reading> const& perWindow)
        {
          auto out = std::make_shared<std::vector<Reading>>(r.Stretches.size());
          for (size_t i = 0; i < r.Stretches.size(); ++i)
            (*out)[i] = perWindow[windowOf[i]];
          return std::shared_ptr<std::vector<Reading> const>(out);
        };
        auto kept = [&]()
        {
          if (!(ReadText(result, text, 16u << 20) && ParseResult(text, windows.size(), readings, why)))
            return false;
          std::shared_ptr<std::vector<Reading> const> out = spread(readings);
          Finish(r, Done, "", out, true, since());
          try
          {
            SayDone(r, *out, key, true, 0.0, heard, 0.0);
          }
          catch (...)
          {
          }
          return true;
        };
        // [CACHE FIRST] Kept from an earlier reading: taken at once, not after another voice's helper has run (unless
        // it was stopped meanwhile: [REVIEW] a Stop during the hash isn't lost)
        if (r.CancelWanted)
        {
          held.reset();
          Stop(r, false, since(), 0);
          return;
        }
        if (kept())
          return;
        // One helper at a time; the sound is let go while this waits (a reading cancelled meanwhile lets it go at
        // once, and ends within PollMs)
        held.reset();
        std::unique_lock<std::timed_mutex> one(g_OneAtATime, std::defer_lock);
        ULONGLONG waitFrom = GetTickCount64();   // ([FIX CHECK] the wait for another reading alone: not its own hashing)
        while (!r.CancelWanted && !one.try_lock_for(std::chrono::milliseconds(PollMs)))
          ;
        // [REVIEW] Its own time counts from here (the lines in the log: not the wait for another voice's reading)
        ULONGLONG turnAt = GetTickCount64();
        {
          std::lock_guard<std::mutex> lock(r.Mutex);
          if (!r.CancelWanted && one.owns_lock() && r.Sound)
          {
            r.Now = Running;
            r.RunningAt = turnAt;
            held = r.Sound;
          }
        }
        if (!held)
        {
          Stop(r, false, since(), 0);
          return;
        }
        double waited = (turnAt - waitFrom) / 1000.0;
        auto ranFor = [turnAt] { return (GetTickCount64() - turnAt) / 1000.0; };
        // Another reading of the same voice may have written it meanwhile (the same line on two characters); a result
        // that can't be read is deleted only now, while no helper of this key can be writing it
        if (r.CancelWanted)
        {
          held.reset();
          Stop(r, false, since(), 0);
          return;
        }
        if (kept())
          return;
        DeleteFileW(result.c_str());
        // [MEMORY] Not started with too little memory left to give ([REVIEW] the figures rounded down, and the rule
        // said: a figure rounded up to the limit itself read as enough)
        unsigned long long left = CommitLeft ? CommitLeft() : ~0ull;
        if (left < HelperCommitBytes + StartReserveBytes)
        {
          char message[320];
          _snprintf_s(message, sizeof(message), _TRUNCATE, "not started: Windows has %.2f GB of memory left to give, and "
            "a reading needs about %.1f GB of it (about %.1f GB for the reading, %.0f GB kept for the game, which ends itself "
            "when Windows has none left): close other programs, then Try again",
            floor(left / 1073741824.0 * 100.0) / 100.0, (HelperCommitBytes + StartReserveBytes) / 1073741824.0,
            HelperCommitBytes / 1073741824.0, StartReserveBytes / 1073741824.0);
          Fail(r, message, since());
          return;
        }
        // [B2] Stopped meanwhile: no job, no helper
        if (r.CancelWanted)
        {
          Stop(r, false, since(), 0);
          return;
        }
        // The job (the helper deletes it as soon as it has read it)
        {
          HANDLE f = CreateFileW(job.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
          if (f == INVALID_HANDLE_VALUE)
          {
            Fail(r, "the job file couldn't be written in Cinematic Tools\\EmotionModel\\cache", since());
            return;
          }
          unsigned int count = (unsigned int)windows.size();
          bool ok = WriteAll(f, "MECEMO1\0", 8) && WriteAll(f, &rate, 4) && WriteAll(f, &channels, 4) &&
            WriteAll(f, &frames, 8) && WriteAll(f, held->Pcm->data(), frames * channels * sizeof(short)) &&
            WriteAll(f, &count, 4);
          for (auto const& w : windows)
            ok = ok && WriteAll(f, &w.first, 8) && WriteAll(f, &w.second, 8);
          CloseHandle(f);
          if (!ok)
          {
            DeleteFileW(job.c_str());
            Fail(r, "the job file couldn't be written (is the disk full?)", since());
            return;
          }
        }
        // The sound itself isn't needed any more (the job holds it). [REVIEW] Taken out under the request's lock and
        // let go after it: if these are the last of its samples (up to about 256 MB), they are freed here, not while the
        // game's thread waits on that lock (it asks for the state every frame)
        {
          std::shared_ptr<voice::Sound const> drop;
          {
            std::lock_guard<std::mutex> lock(r.Mutex);
            drop.swap(r.Sound);
          }
          held.reset();
        }
        if (r.CancelWanted)
        {
          DeleteFileW(job.c_str());
          Stop(r, false, since(), 0);
          return;
        }
        // The helper: idle priority, no window, dies with the game
        std::wstring command = L"\"" + helper + L"\" \"" + job + L"\" \"" + result + L"\" \"" + progress + L"\"";
        std::vector<wchar_t> line(command.begin(), command.end());
        line.push_back(0);
        double most = WaitLeastSeconds + WaitPerHeardSecond * heard;
        char leftText[48] = "not known";
        if (left != ~0ull)
          _snprintf_s(leftText, sizeof(leftText), _TRUNCATE, "%.1f GB", left / 1073741824.0);
        util::log::Write("Pose: the voice %s: reading its feeling with EmotionHelper.exe at idle priority: %u stretches, "
          "%u windows heard, %.1f s in all; Windows' memory left to give: %s (cache\\%ls.txt)", r.File.c_str(),
          (unsigned)r.Stretches.size(), (unsigned)windows.size(), heard, leftText, key);
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi{};
        if (!CreateProcessW(helper.c_str(), line.data(), nullptr, nullptr, FALSE,
          IDLE_PRIORITY_CLASS | CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, folder.c_str(), &si, &pi))
        {
          DWORD error = GetLastError();
          DeleteFileW(job.c_str());
          char message[160];
          _snprintf_s(message, sizeof(message), _TRUNCATE, "EmotionHelper.exe couldn't be started (error %lu)", error);
          Fail(r, message, since());
          return;
        }
        HelperProcess guard;
        guard.Process = pi.hProcess;
        guard.Job = &job;
        guard.Progress = &progress;
        HANDLE killer = KillOnClose();
        if (killer)
          AssignProcessToJobObject(killer, pi.hProcess);
        ResumeThread(pi.hThread);
        CloseHandle(pi.hThread);
        ULONGLONG deadline = GetTickCount64() + (ULONGLONG)(most * 1000.0);
        bool cancelled = false, late = false, low = false;
        unsigned long long lowLeft = 0;
        // (nothing in this loop allocates)
        while (WaitForSingleObject(pi.hProcess, PollMs) == WAIT_TIMEOUT)
        {
          bool wanted = r.CancelWanted, past = GetTickCount64() > deadline;
          unsigned long long nowLeft = CommitLeft ? CommitLeft() : ~0ull;
          if (wanted || past || nowLeft < RunFloorBytes)
          {
            cancelled = wanted;
            late = !wanted && past;
            low = !wanted && !past;
            lowLeft = nowLeft;
            TerminateProcess(pi.hProcess, 1);
            WaitForSingleObject(pi.hProcess, 5000);
            break;
          }
          int done = 0;
          if (ReadProgress(progress, done))
          {
            int k = done < 0 ? 0 : (done > (int)windows.size() ? (int)windows.size() : done);
            std::lock_guard<std::mutex> lock(r.Mutex);
            r.Done = doneAt[k];
            r.ModelReady = true;
          }
        }
        DWORD code = 0;
        GetExitCodeProcess(pi.hProcess, &code);
        DeleteFileW((result + L".tmp").c_str());
        double seconds = ranFor();
        int doneNow = 0;
        {
          std::lock_guard<std::mutex> lock(r.Mutex);
          doneNow = r.Done;
        }
        if (cancelled || late || low)
        {
          DeleteFileW(result.c_str());
          if (cancelled)
            Stop(r, true, seconds, doneNow);
          else
          {
            char message[300];
            if (late)
              _snprintf_s(message, sizeof(message), _TRUNCATE, "the helper took longer than %.0f s and was stopped", most);
            else
              _snprintf_s(message, sizeof(message), _TRUNCATE, "stopped: Windows' memory left to give fell to %.2f GB, under "
                "the %.0f GB kept for the game (which ends itself when Windows has none left): close other programs, then "
                "Try again", floor(lowLeft / 1073741824.0 * 100.0) / 100.0, RunFloorBytes / 1073741824.0);
            Fail(r, message, seconds);
          }
          return;
        }
        readings.clear();
        bool read = ReadText(result, text, 16u << 20);
        if (read && ParseResult(text, windows.size(), readings, why))
        {
          std::shared_ptr<std::vector<Reading> const> out = spread(readings);
          Finish(r, Done, "", out, false, seconds);
          try
          {
            SayDone(r, *out, key, false, seconds, heard, waited);
          }
          catch (...)
          {
            // ([REVIEW] the log failing never turns a finished reading into a failed one)
          }
          return;
        }
        DeleteFileW(result.c_str());
        char message[400];
        // [MEMORY] Out of memory (Windows' own codes for it, or the helper's answer): Try again, it may fit later.
        // [FIX CHECK] A crash (other 0xC... codes) or Windows ML refusing: the same will happen again (no Try again)
        bool memory = code == 0xC0000017u || code == 0xC000012Du || (read && (why == "not enough memory" ||
          why.find("0x8007000E") != std::string::npos || why.find("0x80070008") != std::string::npos ||
          why.find("0x800705AF") != std::string::npos));
        bool retry = memory || (code < 0xC0000000u && !(read && why.compare(0, 11, "Windows ML:") == 0));
        if (memory)
          _snprintf_s(message, sizeof(message), _TRUNCATE, "EmotionHelper.exe ran out of memory (%s): close other "
            "programs, then Try again", read ? why.c_str() : (code == 0xC0000017u ? "code 0xC0000017" : "code 0xC000012D"));
        else if (code == 0xC0000135u)
          _snprintf_s(message, sizeof(message), _TRUNCATE, "EmotionHelper.exe couldn't start: a Windows DLL it needs is "
            "missing (Windows ML comes with Windows 10 version 1809 and later)");
        else if (read)
          _snprintf_s(message, sizeof(message), _TRUNCATE, "%s%s", why.c_str(), retry ? "" : " (this Windows' Windows ML "
            "may not run the model)");
        else
          _snprintf_s(message, sizeof(message), _TRUNCATE, "EmotionHelper.exe ended (code 0x%08lX) without an answer%s", code,
            retry ? "" : " (this Windows' Windows ML may not run the model)");
        Fail(r, message, seconds, retry);
      }
      catch (...)
      {
        // (a helper started is ended by its guard; nothing here may throw again)
        try
        {
          Finish(r, Failed, "couldn't be read (out of memory?)", nullptr, false, (GetTickCount64() - started) / 1000.0);
          util::log::Warning("Pose: the voice %s: its feeling couldn't be read (out of memory?)", r.File.c_str());
        }
        catch (...)
        {
          try
          {
            std::lock_guard<std::mutex> lock(r.Mutex);
            r.Now = Failed;
          }
          catch (...)
          {
          }
        }
      }
    }
  }

  bool Available(std::string* why)
  {
    CleanUp();
    std::lock_guard<std::mutex> lock(g_AvailableMutex);
    ULONGLONG now = GetTickCount64();
    if (g_AvailableAt == 0 || now - g_AvailableAt >= AvailableRecheckMs)
    {
      g_AvailableAt = now;
      std::wstring folder(Folder);
      bool helper = IsFile(folder + L"\\" + HelperFile), model = IsFile(folder + L"\\" + ModelFile);
      // Windows ML in the box (Windows 10 version 1809 and later): without it the helper can't run at all
      wchar_t system[MAX_PATH];
      UINT n = GetSystemDirectoryW(system, MAX_PATH);
      bool winml = n > 0 && n < MAX_PATH && IsFile(std::wstring(system, n) + L"\\windows.ai.machinelearning.dll");
      g_AvailableWas = helper && model && winml;
      const char* download = ": it is an optional download (see the (?))";
      g_AvailableWhy = g_AvailableWas ? "" : (!helper && !model ? std::string("The emotion model isn't in Cinematic "
        "Tools\\EmotionModel") + download : (!helper ? std::string("Cinematic Tools\\EmotionModel\\EmotionHelper.exe isn't "
        "there") + download : (!model ? std::string("Cinematic Tools\\EmotionModel\\emotion_feel.onnx isn't there") +
        download : std::string("This Windows has no Windows ML, which the emotion model needs (it comes with Windows 10 "
        "version 1809 and later)"))));
    }
    if (why)
      *why = g_AvailableWhy;
    return g_AvailableWas;
  }

  std::shared_ptr<Request> Start(std::shared_ptr<voice::Sound const> const& sound, std::vector<Stretch> const& stretches)
  {
    auto request = std::make_shared<Request>();
    request->Sound = sound;
    request->Stretches = stretches;
    request->File = sound ? sound->File : std::string();
    request->Total = (int)stretches.size();
    if (!sound || !sound->Pcm || stretches.empty())
    {
      request->Now = Failed;
      request->CanRetry = false;
      request->Sound.reset();
      request->Message = !sound || !sound->Pcm ? "no sound" : "no speech found in it";
      return request;
    }
    try
    {
      std::thread([request] { Run(request); }).detach();
    }
    catch (...)
    {
      request->Now = Failed;
      request->Sound.reset();
      request->Message = "its reading couldn't be started (no thread)";
      util::log::Warning("Pose: the voice %s: its feeling couldn't be read: %s", request->File.c_str(),
        request->Message.c_str());
    }
    return request;
  }

  void Cancel(std::shared_ptr<Request> const& request)
  {
    if (!request)
      return;
    std::shared_ptr<voice::Sound const> drop;   // (let go after the lock: [REVIEW] never freed under it)
    {
      std::lock_guard<std::mutex> lock(request->Mutex);
      request->CancelWanted = true;
      if (request->Now == Waiting)
        drop.swap(request->Sound);   // (its thread ends within PollMs; the sound's memory goes now, or once the
                                     // chunk of it being hashed is done)
    }
  }

  Status StatusOf(Request const& request)
  {
    std::lock_guard<std::mutex> lock(request.Mutex);
    Status s;
    s.Now = request.Now;
    s.Done = request.Done;
    s.Total = request.Total;
    s.FromCache = request.FromCache;
    s.CanRetry = request.CanRetry;
    s.Seconds = request.Seconds;
    s.Message = request.Message;
    s.Readings = request.Readings;
    s.ModelReady = request.ModelReady;
    s.Elapsed = request.Now == Running && request.RunningAt ? (GetTickCount64() - request.RunningAt) / 1000.0 :
      request.Seconds;
    return s;
  }
}
