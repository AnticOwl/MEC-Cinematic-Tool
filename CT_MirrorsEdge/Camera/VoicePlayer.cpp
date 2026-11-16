#include "VoicePlayer.h"
#include <Windows.h>
#include <xaudio2.h>
#include <process.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <type_traits>

// Sounds that follow a clock set from outside, through XAudio2 (xaudio2_9.dll, Windows' own, loaded when first
// needed), on a thread of the player's own.
//
// How it's driven: every frame the caller says for each sound where it should be (Want: position, rate, volume, and
// the moment that position is for). Want only writes that down; the thread carries the position on from that moment
// at the rate given, so it doesn't matter when in a frame Want is called or how even the frames are, and when the
// calls stop (StaleSeconds) the sound stops by itself.
//
// How a voice is chased (all of it measured):
// - XAudio2 processes 10 ms at a time (a pass). Calls on a voice (Start, Stop, SetVolume, SetFrequencyRatio, a new
//   buffer) take effect at the next pass. The thread is woken at the end of every pass (the engine's own callback),
//   reads each voice and gives its commands right then, so they're all in before the next pass.
// - Where a voice is: IXAudio2SourceVoice::GetState's SamplesPlayed counts the samples of the SOURCE the voice has
//   taken in (441 a pass at 44.1 kHz and ratio 1, twice that at ratio 2). Stop, Start, FlushSourceBuffers, a new
//   buffer and its PlayBegin neither reset nor move it; only a buffer flagged END_OF_STREAM that plays to its end
//   resets it to 0. So the position is the buffer's PlayBegin + (SamplesPlayed - SamplesPlayed when it was started).
// - What a pass makes reaches the speakers later, by the delay XAudio2 reports
//   (XAUDIO2_PERFORMANCE_DATA::CurrentLatencyInSamples, 39.9 ms here; the middle one of its last 9 readings is used,
//   since a single reading is a whole pass off now and then). So the position the next pass starts at is held
//   against the position wanted at the time that pass is heard (when the pass is due + the delay): the sound runs
//   ahead of the clock by the output delay and is heard in step with it. When a pass is due is kept as a steady line
//   of 10 ms a pass pulled gently to the starts seen (they jitter).
// - A small error (under RestartSeconds of real time) is taken out by playing a little faster or slower (the
//   frequency ratio = the wanted rate, plus or minus up to MaxNudge, more while the rate itself is changing): a
//   ratio change takes effect at a pass's start with no break in the sound. The error is smoothed before it's acted
//   on, for longer while the rate isn't changing (SmoothSecondsSteady), so that noise in the clock said doesn't
//   become a wobble of the pitch. A large error (a jump, a scrub, a loop) starts the sound again from the right
//   place, at most once every RestartApartSeconds.
// - No clicks: XAudio2 ramps a volume change over 1 ms (48 samples at 48 kHz) at the next pass, but only on a voice
//   that has already run. So before a voice is stopped or moved its volume goes to 0 for one pass, and it comes in
//   with the ramp; each voice is run for one pass at volume 0 when it's made, for that. (A start while the rate was
//   under 1 was measured with a shorter ramp, 29 - 32 samples.) A voice is moved (Stop, FlushSourceBuffers, a new
//   buffer with PlayBegin, Start) only while it's stopped, so its count is sure. The one stop with no fade is a
//   sound's own end: its last samples are played as they are, so the caller hands in a sound that ends at zero.
// - Slower than MinRate or faster than MaxRate: silence (the pitch goes with the rate, as on a tape; nothing here
//   keeps the pitch).
//
// No audio device, or the device lost (the engine's OnCriticalError): the sounds stay silent, Want and Heard go on
// working, and the engine is tried again every RetrySeconds. A change of Windows' default output should need nothing
// here: XAudio2 2.9 is said to follow it by itself (its mastering voice is made on the default device without
// XAUDIO2_NO_VIRTUAL_AUDIO_CLIENT). That was not tried with a real device.

namespace
{
  using voice_player::MaxSounds;
  using voice_player::MinRate;
  using voice_player::MaxRate;
  using voice_player::StaleSeconds;

  const double QuantumSeconds = 0.010;         // a processing pass
  const double ActWithinSeconds = 0.007;       // commands are given only this soon after a pass began (before the next)
  const double RestartSeconds = 0.080;         // further off than this (real time): started again from the right place
  const double RestartApartSeconds = 0.12;     // not more often than this while it plays (a scrub gives snippets)
  const double NudgeSeconds = 0.5;             // a small error is taken out over about this long
  const double MaxNudge = 0.05;                // by at most this much of the rate (5 % = 0.84 of a semitone)
  const double NudgeSecondsChanging = 0.2;     // the same two while the rate is changing (see Chase)
  const double MaxNudgeChanging = 0.15;
  const double RateMemorySeconds = 0.25;       // "changing": against the rate smoothed over this long
  const double NudgeStep = 0.002;              // in steps of this
  const double DeadBandSeconds = 0.003;        // an error under this is left alone
  const double SmoothSeconds = 0.06;           // the error is smoothed over this before it's acted on
  const double SmoothSecondsSteady = 0.25;     // the same while the rate isn't changing (noise in the clock taken out)
  const float RateSlack = 0.08f;               // a playing sound goes on this far outside MinRate - MaxRate
  const float MaxVoiceRatio = 4.f;             // the voices' MaxFrequencyRatio (MaxRate, its slack, the nudge and room)
  const double RetrySeconds = 3.;              // between tries to open the audio
  const double IdleEngineSeconds = 2.;         // with no sound open for this long the engine is stopped
  const double FocusFadeSeconds = 0.15;        // the pace of the fade when the focus window loses or gets the front
                                               // (an easing; measured: half after 0.11 s, all of it after 0.67 s)
  const double NoPassSeconds = 1.;             // a running engine with no pass for this long is taken for lost
  const double StartTailSeconds = 0.05;        // a sound isn't started for less than this of its end
  const double StampAheadSeconds = 0.1;        // the moment given to Want may be this far after the call,
  const double StampBehindSeconds = 0.5;       // or this far before it: outside that the call's own time is taken
  const int PieceCount = 64;

  typedef HRESULT(__stdcall* tXAudio2Create)(IXAudio2**, UINT32, XAUDIO2_PROCESSOR);

  std::atomic<double> g_SecondsPerTick{ 0. };

  // Seconds on Windows' performance counter: the count times one over the frequency (the same clock for Want's
  // stamps, the passes' times and the thread; the moment a caller gives to Want is on it too)
  double Clock()
  {
    double perTick = g_SecondsPerTick.load(std::memory_order_relaxed);
    if (perTick == 0.)
    {
      LARGE_INTEGER f;
      QueryPerformanceFrequency(&f);
      perTick = 1. / (double)f.QuadPart;
      g_SecondsPerTick.store(perTick, std::memory_order_relaxed);
    }
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * perTick;
  }

  template <typename T> T Clamp(T value, T low, T high)
  {
    return value < low ? low : value > high ? high : value;
  }

  // A value handed from one thread to others with nobody waiting: the writer
  // marks it odd while it writes, a reader reads again when it changed under it
  template <typename T> class Handover
  {
    static_assert(std::is_trivially_copyable<T>::value, "plain data only");
    static const size_t Words = (sizeof(T) + 7) / 8;
    std::atomic<unsigned> m_Sequence{ 0 };
    std::atomic<unsigned long long> m_Data[(sizeof(T) + 7) / 8];

  public:
    Handover()
    {
      for (size_t i = 0; i < Words; i++)
        m_Data[i].store(0, std::memory_order_relaxed);
    }

    // False when another thread is writing it right now (nothing written)
    bool Write(T const& value)
    {
      unsigned long long words[(sizeof(T) + 7) / 8] = {};
      memcpy(words, &value, sizeof(T));
      unsigned s = m_Sequence.load();
      if ((s & 1) || !m_Sequence.compare_exchange_strong(s, s + 1))
        return false;
      for (size_t i = 0; i < Words; i++)
        m_Data[i].store(words[i], std::memory_order_relaxed);
      m_Sequence.store(s + 2);
      return true;
    }

    // False when it kept changing (value left as it was)
    bool Read(T& value) const
    {
      for (int tries = 0; tries < 200; tries++)
      {
        unsigned s = m_Sequence.load();
        if (s & 1)
        {
          YieldProcessor();
          continue;
        }
        unsigned long long words[(sizeof(T) + 7) / 8];
        for (size_t i = 0; i < Words; i++)
          words[i] = m_Data[i].load(std::memory_order_relaxed);
        if (m_Sequence.load() == s)
        {
          memcpy(&value, words, sizeof(T));
          return true;
        }
      }
      return false;
    }
  };

  // What Want said
  struct Wanted
  {
    double Seconds;
    double Stamp;     // the moment it's for on the clock (the one given to Want, or else when it was said)
    float Rate;
    float Volume;
    int Playing;
    int Pad;
  };

  // What the thread tells of a sound
  struct Told
  {
    voice_player::Stats Stats;
    // What reaches the speakers from PieceTime on: PiecePos + PieceSlope x the
    // time since (seconds of the sound), while PieceSound
    double PieceTime;
    double PiecePos;
    double PieceSlope;
    double Duration;
    int PieceSound;
    int Pad;
  };

  enum SlotState
  {
    SlotFree,
    SlotClaimed,    // Open is filling it in
    SlotOpen,
    SlotClosing     // Close was called; the thread lets it go
  };

  struct Slot
  {
    std::atomic<int> State{ SlotFree };
    std::atomic<unsigned> Generation{ 0 };
    // Written by Open while claimed, read by the thread once open, cleared by
    // the thread when it lets the slot go
    std::shared_ptr<const std::vector<short>> Pcm;
    int Rate = 0;
    int Channels = 0;
    Handover<Wanted> Asked;
    Handover<Told> Said;
  };

  enum VoiceMode
  {
    NoVoice,     // no XAudio2 voice (no engine yet, or it failed)
    Priming,     // made, running its one pass at volume 0
    Stopped,
    Running,
    Fading       // its volume set to 0, to be stopped once that pass is done
  };

  // From Time on (when it reaches the speakers): at Pos, moving at Slope
  struct Piece
  {
    double Time;
    double Pos;
    double Slope;
    bool Sound;
  };

  // The thread's own side of a sound
  struct Voice
  {
    bool Used = false;
    bool Closing = false;
    std::shared_ptr<const std::vector<short>> Pcm;
    int Rate = 0;
    int Channels = 0;
    long long Frames = 0;
    IXAudio2SourceVoice* Source = nullptr;
    VoiceMode Mode = NoVoice;
    bool Known = false;             // stopped with its buffer still on it: Begin and Base say where
    bool Unsure = false;            // its count may be a pass off (a start done again, see Chase)
    bool RestartAfterFade = false;
    long long Begin = 0;            // the buffer's first frame
    unsigned long long Base = 0;    // SamplesPlayed when that buffer was started
    unsigned MarkStarts = 0;        // passes begun when the fade (or the priming) was asked for
    double MarkTime = 0.;
    double NextMake = 0.;
    float RatioSet = 1.f;
    float VolumeSet = 0.f;
    double LastSeek = -1e9;
    double Smooth = 0.;
    bool Correcting = false;
    double LastControl = 0.;
    double ChaseError = 0.;
    double RateBefore = 0.;         // the wanted rate smoothed over RateMemorySeconds
    double RateTime = 0.;
    Wanted Last{};
    Piece Pieces[PieceCount] = {};
    int PieceEnd = 0;               // pieces pushed so far
    voice_player::Stats Stats{};
  };

  struct Engine
  {
    IXAudio2* Xaudio = nullptr;
    IXAudio2MasteringVoice* Master = nullptr;
    bool Running = false;
    double Started = 0.;
    double NextTry = 0.;
    double Latency = 0.04;       // the output delay used (the middle one of the last readings)
    double Readings[9] = {};
    int ReadingCount = 0;
    int PassesRead = 0;          // passes seen since the engine was made or started
    float VolumeSet = -1.f;
    float Focus = 1.f;
    long LoggedError = 0;
    voice_player::EngineInfo Info{};
  };

  struct Pass
  {
    unsigned Starts;
    unsigned Ends;
    double Start;    // when the last one began
    double Next;     // when the next one is due
  };

  Slot g_Slots[MaxSounds];
  std::atomic<HANDLE> g_Wake{ nullptr };
  std::atomic<HANDLE> g_Thread{ nullptr };
  std::atomic<int> g_ThreadState{ 0 };      // 0 none, 1 running, 2 being stopped (left so if it didn't end)
  std::atomic<bool> g_Stop{ false };
  std::atomic<float> g_MasterVolume{ 1.f };
  std::atomic<HWND> g_FocusWindow{ nullptr };
  std::atomic<double> g_LatencyTrim{ 0. };
  std::atomic<void (*)(const char*)> g_Log{ nullptr };
  std::atomic<int> g_TestNoDevice{ 0 };
  std::atomic<bool> g_TestRetry{ false };
  std::atomic<double> g_PassStart{ 0. };
  std::atomic<unsigned> g_PassStarts{ 0 };
  std::atomic<unsigned> g_PassEnds{ 0 };
  std::atomic<bool> g_Critical{ false };
  std::atomic<long> g_CriticalCode{ 0 };
  Handover<voice_player::EngineInfo> g_Info;
  tXAudio2Create g_Create = nullptr;        // the thread's only
  SRWLOCK g_ThreadLock = SRWLOCK_INIT;      // the thread is started and stopped under it

  // g_ThreadLock held to the end of the block
  struct ThreadLocked
  {
    ThreadLocked()
    {
      AcquireSRWLockExclusive(&g_ThreadLock);
    }

    ~ThreadLocked()
    {
      ReleaseSRWLockExclusive(&g_ThreadLock);
    }

    ThreadLocked(ThreadLocked const&) = delete;
    ThreadLocked& operator=(ThreadLocked const&) = delete;
  };

  void Log(const char* format, ...)
  {
    void (*write)(const char*) = g_Log.load();
    if (!write)
      return;
    char text[300];
    va_list args;
    va_start(args, format);
    _vsnprintf_s(text, sizeof(text), _TRUNCATE, format, args);
    va_end(args);
    write(text);
  }

  void Wake()
  {
    HANDLE wake = g_Wake.load();
    if (wake)
      SetEvent(wake);
  }

  // The engine's own thread tells when its passes begin and end
  struct EngineEvents : public IXAudio2EngineCallback
  {
    void STDMETHODCALLTYPE OnProcessingPassStart() override
    {
      // (the count first: a reader that finds the count unchanged around its
      // read of the time has the time that goes with it)
      double now = Clock();
      g_PassStarts.fetch_add(1);
      g_PassStart.store(now);
    }

    void STDMETHODCALLTYPE OnProcessingPassEnd() override
    {
      g_PassEnds.fetch_add(1);
      Wake();
    }

    void STDMETHODCALLTYPE OnCriticalError(HRESULT error) override
    {
      g_CriticalCode.store((long)error);
      g_Critical.store(true);
      Wake();
    }
  };
  EngineEvents g_Events;

  int MakeHandle(int index, unsigned generation)
  {
    return (int)(((generation & 0x3FFFFFu) << 8) | (unsigned)index);
  }

  // The slot a handle stands for, while it's open
  Slot* SlotOf(int handle)
  {
    if (handle < 0)
      return nullptr;
    int index = handle & 0xFF;
    if (index >= MaxSounds)
      return nullptr;
    Slot& slot = g_Slots[index];
    if (slot.State.load() != SlotOpen || MakeHandle(index, slot.Generation.load()) != handle)
      return nullptr;
    return &slot;
  }

  Told NothingTold()
  {
    Told told{};
    told.Stats.Heard = -1.;
    told.Stats.Wanted = -1.;
    told.Stats.Ratio = 1.f;
    return told;
  }

  XAUDIO2_BUFFER BufferFrom(Voice const& v, long long begin)
  {
    XAUDIO2_BUFFER buffer{};
    unsigned long long bytes = (unsigned long long)v.Frames * (unsigned)v.Channels * sizeof(short);
    buffer.Flags = XAUDIO2_END_OF_STREAM;
    buffer.AudioBytes = (UINT32)bytes;
    buffer.pAudioData = (const BYTE*)v.Pcm->data();
    buffer.PlayBegin = (UINT32)begin;
    return buffer;
  }

  // A voice made and set off on its one pass at volume 0 (see the top)
  bool MakeSource(Voice& v, Engine& e)
  {
    WAVEFORMATEX format{};
    format.wFormatTag = WAVE_FORMAT_PCM;
    format.nChannels = (WORD)v.Channels;
    format.nSamplesPerSec = (DWORD)v.Rate;
    format.wBitsPerSample = 16;
    format.nBlockAlign = (WORD)(v.Channels * 2);
    format.nAvgBytesPerSec = (DWORD)(v.Rate * v.Channels * 2);
    HRESULT hr = e.Xaudio->CreateSourceVoice(&v.Source, &format, 0, MaxVoiceRatio);
    if (FAILED(hr))
    {
      v.Source = nullptr;
      Log("Voice: a sound's voice couldn't be made (%d Hz, %d channels, HRESULT 0x%08X)", v.Rate, v.Channels,
        (unsigned)hr);
      return false;
    }
    v.Source->SetVolume(0.f);
    XAUDIO2_BUFFER buffer = BufferFrom(v, 0);
    if (FAILED(v.Source->SubmitSourceBuffer(&buffer)) || FAILED(v.Source->Start(0)))
    {
      v.Source->DestroyVoice();
      v.Source = nullptr;
      return false;
    }
    v.Begin = 0;
    v.Base = 0;
    v.RatioSet = 1.f;
    v.VolumeSet = 0.f;
    return true;
  }

  // The stopped voice set to play from the frame begin; played is its
  // SamplesPlayed now (it doesn't move while the voice is stopped)
  bool Seek(Voice& v, long long begin, float ratio, float volume, unsigned long long played)
  {
    XAUDIO2_BUFFER buffer = BufferFrom(v, begin);
    v.Source->Stop(0);
    v.Source->FlushSourceBuffers();
    if (FAILED(v.Source->SubmitSourceBuffer(&buffer)))
      return false;
    v.Source->SetFrequencyRatio(ratio);
    v.Source->SetVolume(volume);
    if (FAILED(v.Source->Start(0)))
      return false;
    v.Begin = begin;
    v.Base = played;
    v.RatioSet = ratio;
    v.VolumeSet = volume;
    return true;
  }

  // The second of the sound the voice's next pass starts at: the buffer's first frame + the count, less 2 samples.
  // Measured into the 48 kHz output, the next pass's first sample is the count less 2 for a 48 kHz sound at ratios
  // 0.5 and 2, less 1 for 44.1 kHz at ratio 1 (between the two at ratio 1.03), and the count itself for 48 kHz at
  // ratio 1. So this is right or up to 2 samples early, by the rates (0.04 ms at 48 kHz).
  double Position(Voice const& v, unsigned long long played)
  {
    long long taken = (long long)(played - v.Base) - 2;
    return ((double)v.Begin + (double)(std::max)(taken, 0ll)) / (double)v.Rate;
  }

  void Fade(Voice& v, unsigned starts, double now, bool restart)
  {
    v.Source->SetVolume(0.f);
    v.VolumeSet = 0.f;
    v.Mode = Fading;
    v.RestartAfterFade = restart;
    v.MarkStarts = starts;
    v.MarkTime = now;
    v.Stats.Fades++;
  }

  // What reaches the speakers from time on. A piece for the same pass
  // replaces the last one; silence after silence is left out
  void Push(Voice& v, double time, double pos, double slope, bool sound)
  {
    if (v.PieceEnd > 0)
    {
      Piece& last = v.Pieces[(v.PieceEnd - 1) % PieceCount];
      if (time <= last.Time + 0.002)
      {
        last = { last.Time, pos, slope, sound };
        return;
      }
      if (!sound && !last.Sound)
        return;
    }
    v.Pieces[v.PieceEnd % PieceCount] = { time, pos, slope, sound };
    v.PieceEnd++;
  }

  // The piece that reaches the speakers at time now, or null (nothing yet)
  Piece const* Active(Voice const& v, double now)
  {
    int oldest = (std::max)(0, v.PieceEnd - PieceCount);
    for (int i = v.PieceEnd - 1; i >= oldest; i--)
      if (v.Pieces[i % PieceCount].Time <= now)
        return &v.Pieces[i % PieceCount];
    return nullptr;
  }

  void DropSource(Voice& v)
  {
    if (v.Source)
      v.Source->DestroyVoice();
    v.Source = nullptr;
    v.Mode = NoVoice;
    v.Known = false;
    // (nothing of it reaches the speakers any more)
    v.PieceEnd = 0;
  }

  // One voice's chase, once after each pass (see the top)
  void Chase(Voice& v, Engine& e, Pass const& pass, double now)
  {
    Wanted const& w = v.Last;
    double heardAt = pass.Next + e.Latency + g_LatencyTrim.load();
    if (v.Mode == NoVoice)
    {
      if (now >= v.NextMake)
      {
        if (MakeSource(v, e))
        {
          v.Mode = Priming;
          v.MarkStarts = pass.Starts;
          v.MarkTime = now;
        }
        else
          v.NextMake = now + RetrySeconds;
      }
      Push(v, heardAt, 0., 0., false);
      return;
    }
    double rate = w.Rate;
    bool alive = w.Playing != 0 && now - w.Stamp <= StaleSeconds;
    double wantedThen = w.Seconds + rate * (heardAt - w.Stamp);
    double duration = (double)v.Frames / (double)v.Rate;
    // A playing sound goes on a little outside the rates it may start at, and
    // plays to its own end (it isn't cut when the clock gets there a moment
    // before it); a sound isn't started for less than its last StartTailSeconds
    bool mayGoOn = alive && rate >= MinRate * (1. - RateSlack) && rate <= MaxRate * (1. + RateSlack) &&
      wantedThen > -QuantumSeconds * rate && wantedThen < duration + RestartSeconds * rate;
    bool mayStart = alive && rate >= MinRate && rate <= MaxRate && wantedThen > -QuantumSeconds * rate &&
      wantedThen < duration - StartTailSeconds;
    // While the rate of a playing sound is changing (against what it was over
    // the last quarter second) the pitch is moving anyway, and what's on its
    // way to the speakers was made at the old rate: errors are taken out
    // faster then
    double sinceRate = now - v.RateTime;
    v.RateTime = now;
    if (v.Mode != Running || sinceRate >= RateMemorySeconds)
      v.RateBefore = rate;
    else
      v.RateBefore += (rate - v.RateBefore) * sinceRate / RateMemorySeconds;
    bool changing = fabs(rate - v.RateBefore) > 0.03 * rate;
    float volume = Clamp(w.Volume, 0.f, 2.f);
    float ratio = (float)rate;
    XAUDIO2_VOICE_STATE state{};
    v.Source->GetState(&state, 0);
    bool stoppedNow = false, sound = false;
    double pos = 0.;
    v.ChaseError = 0.;

    if (v.Mode == Priming)
    {
      if (pass.Ends < v.MarkStarts + 1)
      {
        Push(v, heardAt, 0., 0., false);
        return;
      }
      v.Source->Stop(0);
      v.Mode = Stopped;
      v.Known = false;
      stoppedNow = true;
    }
    if (v.Mode == Running)
    {
      if (state.BuffersQueued == 0)
      {
        // Played to its end
        v.Source->Stop(0);
        v.Mode = Stopped;
        v.Known = false;
        v.Stats.Ends++;
      }
      else
      {
        pos = Position(v, state.SamplesPlayed);
        double off = mayGoOn ? (pos - wantedThen) / rate : 0.;
        v.ChaseError = off;
        if (!mayGoOn)
          Fade(v, pass.Starts, now, false);
        else if (fabs(off) > RestartSeconds && now - v.LastSeek >= RestartApartSeconds)
          Fade(v, pass.Starts, now, true);
        else
        {
          // A small error: a little faster or slower. Smoothed first, for longer while the rate isn't changing,
          // so that noise in the position said moves the ratio less. Measured over 15 s at 144 frames a second,
          // the position said off by up to 5 ms either way (new every frame): 0 to 8 ratio changes, where the
          // short smoothing alone gave 17; up to 10 ms: 6 to 12 against 328; up to 20 ms: 105 to 182 against 894
          double step = now - v.LastControl;
          v.LastControl = now;
          double over = changing ? SmoothSeconds : SmoothSecondsSteady;
          v.Smooth += (off - v.Smooth) * (step >= over ? 1. : step / over);
          if (!v.Correcting && fabs(v.Smooth) > DeadBandSeconds)
            v.Correcting = true;
          else if (v.Correcting && fabs(v.Smooth) < DeadBandSeconds / 3.)
            v.Correcting = false;
          double most = changing ? MaxNudgeChanging : MaxNudge;
          double nudge = v.Correcting ?
            Clamp(-v.Smooth / (changing ? NudgeSecondsChanging : NudgeSeconds), -most, most) : 0.;
          nudge = floor(nudge / NudgeStep + 0.5) * NudgeStep;
          float set = Clamp((float)(rate * (1. + nudge)), XAUDIO2_MIN_FREQ_RATIO, MaxVoiceRatio);
          if (set != v.RatioSet)
          {
            v.Source->SetFrequencyRatio(set);
            v.RatioSet = set;
            v.Stats.RatioChanges++;
          }
          if (volume != v.VolumeSet)
          {
            v.Source->SetVolume(volume);
            v.VolumeSet = volume;
          }
          sound = true;
        }
      }
    }
    else if (v.Mode == Fading)
    {
      if (pass.Ends >= v.MarkStarts + 1)
      {
        // The pass at volume 0 is done
        v.Source->Stop(0);
        v.Known = state.BuffersQueued != 0 && !v.Unsure;
        v.Unsure = false;
        v.Mode = Stopped;
        stoppedNow = true;
      }
    }
    bool restart = v.RestartAfterFade;
    if (v.Mode == Stopped)
      v.RestartAfterFade = false;
    if (v.Mode == Stopped && mayStart)
    {
      double at = v.Known ? Position(v, state.SamplesPlayed) : 0.;
      double off = v.Known ? (at - wantedThen) / rate : 1e9;
      if (fabs(off) <= RestartSeconds)
      {
        // It goes on from where it stopped
        v.Source->SetFrequencyRatio(ratio);
        v.Source->SetVolume(volume);
        v.Source->Start(0);
        v.RatioSet = ratio;
        v.VolumeSet = volume;
        v.Mode = Running;
        v.Smooth = off;
        v.Correcting = false;
        v.LastControl = now;
        v.ChaseError = off;
        v.Stats.Resumes++;
        pos = at;
        sound = true;
      }
      else
      {
        long long begin = Clamp((long long)floor(wantedThen * (double)v.Rate + 0.5), 0ll, v.Frames - 2);
        if (Seek(v, begin, ratio, volume, state.SamplesPlayed))
        {
          v.Mode = Running;
          v.Known = true;
          v.Smooth = 0.;
          v.Correcting = false;
          v.LastControl = now;
          v.LastSeek = now;
          if (restart)
            v.Stats.Restarts++;
          else
            v.Stats.Starts++;
          pos = (double)begin / (double)v.Rate;
          sound = true;
          unsigned starts = g_PassStarts.load() + (g_TestRetry.exchange(false) ? 1u : 0u);
          if (stoppedNow && starts != pass.Starts)
          {
            // A pass began while this was being asked for (the thread was held
            // up): the voice may have gone on for that pass before it stopped,
            // so its count isn't sure. Out again for a pass and once more.
            Fade(v, starts, now, true);
            v.Unsure = true;
            v.Stats.Fades--;
            v.Stats.Retries++;
            sound = false;
          }
        }
        else
        {
          DropSource(v);
          v.NextMake = now + 1.;
        }
      }
    }
    Push(v, heardAt, pos, v.RatioSet, sound);
  }

  // What a sound is doing now, for Heard and GetStats
  void Tell(Voice& v, Slot& slot, double now)
  {
    Wanted const& w = v.Last;
    double duration = v.Rate > 0 ? (double)v.Frames / (double)v.Rate : 0.;
    Piece const* piece = Active(v, now);
    double heard = -1.;
    if (piece && piece->Sound)
    {
      heard = piece->Pos + piece->Slope * (now - piece->Time);
      if (heard >= duration)
        heard = -1.;
    }
    bool alive = w.Playing != 0 && now - w.Stamp <= StaleSeconds;
    double wanted = w.Seconds + (double)w.Rate * (now - w.Stamp);
    bool should = alive && w.Rate >= MinRate && w.Rate <= MaxRate && wanted >= 0. && wanted < duration;
    v.Stats.Time = now;
    v.Stats.Heard = heard;
    v.Stats.Wanted = alive ? wanted : -1.;
    v.Stats.Sounding = heard >= 0. ? 1 : 0;
    v.Stats.ShouldSound = should ? 1 : 0;
    v.Stats.Error = heard >= 0. && should ? heard - wanted : 0.;
    v.Stats.ChaseError = v.ChaseError;
    v.Stats.Ratio = v.RatioSet;
    v.Stats.Mode = (int)v.Mode;
    Told told{};
    told.Stats = v.Stats;
    told.Duration = duration;
    if (piece)
    {
      told.PieceTime = piece->Time;
      told.PiecePos = piece->Pos;
      told.PieceSlope = piece->Slope;
      told.PieceSound = piece->Sound ? 1 : 0;
    }
    slot.Said.Write(told);
  }

  float MasterWanted(Engine const& e)
  {
    return Clamp(g_MasterVolume.load(), 0.f, 1.f) * e.Focus;
  }

  void CloseEngine(Engine& e, Voice* voices)
  {
    for (int i = 0; i < MaxSounds; i++)
      DropSource(voices[i]);
    if (e.Master)
      e.Master->DestroyVoice();
    if (e.Xaudio)
      e.Xaudio->Release();
    e.Master = nullptr;
    e.Xaudio = nullptr;
    e.Running = false;
    e.VolumeSet = -1.f;
    e.Info.Open = 0;
    e.Info.Running = 0;
  }

  bool OpenEngine(Engine& e, double now)
  {
    HRESULT hr = HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
    if (!g_TestNoDevice.load())
    {
      if (!g_Create)
      {
        HMODULE dll = LoadLibraryW(L"xaudio2_9.dll");
        if (!dll)
          dll = LoadLibraryW(L"xaudio2_8.dll");
        g_Create = dll ? (tXAudio2Create)GetProcAddress(dll, "XAudio2Create") : nullptr;
      }
      hr = g_Create ? g_Create(&e.Xaudio, 0, XAUDIO2_DEFAULT_PROCESSOR) : HRESULT_FROM_WIN32(ERROR_MOD_NOT_FOUND);
      if (SUCCEEDED(hr))
      {
        g_Critical.store(false);
        hr = e.Xaudio->RegisterForCallbacks(&g_Events);
      }
      if (SUCCEEDED(hr))
        hr = e.Xaudio->CreateMasteringVoice(&e.Master);
      if (SUCCEEDED(hr))
      {
        // Its volume first, before any sound exists
        float volume = MasterWanted(e);
        hr = e.Master->SetVolume(volume);
        e.VolumeSet = volume;
      }
    }
    if (FAILED(hr))
    {
      if (e.Master)
        e.Master->DestroyVoice();
      if (e.Xaudio)
        e.Xaudio->Release();
      e.Master = nullptr;
      e.Xaudio = nullptr;
      e.Info.Failures++;
      e.Info.LastError = (long)hr;
      e.NextTry = now + RetrySeconds;
      if (e.LoggedError != (long)hr)
        Log("Voice: Windows' audio (XAudio2) couldn't be opened (HRESULT 0x%08X): no sound, tried again every %.0f s",
          (unsigned)hr, RetrySeconds);
      e.LoggedError = (long)hr;
      return false;
    }
    XAUDIO2_VOICE_DETAILS details{};
    e.Master->GetVoiceDetails(&details);
    e.Running = true;
    e.Started = now;
    e.ReadingCount = 0;
    e.PassesRead = 0;
    e.LoggedError = 0;
    e.Info.Open = 1;
    e.Info.Running = 1;
    e.Info.Channels = (int)details.InputChannels;
    e.Info.Rate = (int)details.InputSampleRate;
    e.Info.Opens++;
    Log("Voice: plays through Windows' audio (XAudio2, %u speakers, %u Hz)", details.InputChannels,
      details.InputSampleRate);
    return true;
  }

  unsigned __stdcall Run(void*)
  {
    HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    // (it has microseconds of work every 10 ms, and it should get to it within
    // the 7 ms after a pass even when the game keeps every core busy)
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    Engine e;
    Voice* voices = new Voice[MaxSounds];
    HANDLE wake = g_Wake.load();
    double last = Clock(), lastOpen = last, due = 0., lastSeen = 0.;
    unsigned acted = ~0u, seen = ~0u, wakes = 0;
    int open = 0, run = 0;
    while (!g_Stop.load())
    {
      WaitForSingleObject(wake, e.Running ? 30 : open ? 50 : 500);
      if (g_Stop.load())
        break;
      double now = Clock(), dt = now - last;
      last = now;
      wakes++;

      // Sounds opened since
      open = 0;
      for (int i = 0; i < MaxSounds; i++)
      {
        Slot& slot = g_Slots[i];
        Voice& v = voices[i];
        int state = slot.State.load();
        if (!v.Used && state == SlotOpen)
        {
          v = Voice();
          v.Used = true;
          v.Pcm = slot.Pcm;
          v.Rate = slot.Rate;
          v.Channels = slot.Channels;
          unsigned long long frames = v.Pcm->size() / (size_t)v.Channels;
          unsigned long long most = XAUDIO2_MAX_BUFFER_BYTES / ((unsigned long long)v.Channels * sizeof(short)) - 1;
          v.Frames = (long long)(std::min)(frames, most);
          // (XAudio2 takes 2 GB a buffer at most: a longer sound is played up to there, measured)
          if (frames > most)
            Log("Voice: a sound of %.0f s holds more than 2 GB of samples: only its first %.0f s are played",
              (double)frames / (double)v.Rate, (double)most / (double)v.Rate);
        }
        if (v.Used && state == SlotClosing)
          v.Closing = true;
        else if (state == SlotClosing)
        {
          // Closed before it was ever taken up here
          slot.Pcm.reset();
          slot.State.store(SlotFree);
        }
        if (v.Used)
          open++;
      }

      // The engine: made when a sound is open, stopped when none has been for
      // a while, let go when it's lost
      if (g_Critical.exchange(false) && e.Xaudio)
      {
        e.Info.LastError = g_CriticalCode.load();
        Log("Voice: Windows' audio stopped (critical error 0x%08X): it's opened again in %.0f s",
          (unsigned)e.Info.LastError, RetrySeconds);
        CloseEngine(e, voices);
        e.NextTry = now + RetrySeconds;
      }
      if (g_TestNoDevice.load() && e.Xaudio)
      {
        CloseEngine(e, voices);
        e.NextTry = now;
      }
      if (open)
      {
        lastOpen = now;
        if (!e.Xaudio && now >= e.NextTry)
          OpenEngine(e, now);
        else if (e.Xaudio && !e.Running)
        {
          HRESULT hr = e.Xaudio->StartEngine();
          if (SUCCEEDED(hr))
          {
            e.Running = true;
            e.Started = now;
            e.ReadingCount = 0;
            e.PassesRead = 0;
            e.Info.Running = 1;
          }
          else
          {
            e.Info.LastError = (long)hr;
            CloseEngine(e, voices);
            e.NextTry = now + RetrySeconds;
          }
        }
      }
      else if (e.Xaudio && e.Running && now - lastOpen > IdleEngineSeconds)
      {
        e.Xaudio->StopEngine();
        e.Running = false;
        e.Info.Running = 0;
      }
      Pass pass{};
      pass.Starts = g_PassStarts.load();
      pass.Ends = g_PassEnds.load();
      pass.Start = g_PassStart.load();
      bool between = pass.Starts == pass.Ends && g_PassStarts.load() == pass.Starts;
      bool newPass = between && pass.Starts != seen;
      if (newPass)
      {
        // When the passes are due. Each pass is 10 ms of sound whenever it
        // runs, but its start is seen a little early or late (measured: within
        // 1.1 ms most of the time, now and then 10 ms late). So the due time
        // goes on by 10 ms a pass and is pulled a tenth of the way to the
        // start seen, by 0.05 ms a pass at most: that also follows the sound
        // card's clock, which isn't the PC's (measured: 118 parts in a million
        // apart here)
        unsigned count = pass.Starts - seen;
        if (seen == ~0u || count > 8 || pass.Start - lastSeen > 0.1)
        {
          due = pass.Start;
          run = 0;
        }
        else
        {
          // (a run's first passes aren't evenly spaced: halfway to each start seen)
          due += (double)count * QuantumSeconds;
          due += ++run < 16 ? 0.5 * (pass.Start - due) : 0.1 * Clamp(pass.Start - due, -0.0005, 0.0005);
        }
        seen = pass.Starts;
        lastSeen = pass.Start;
      }
      pass.Next = due + QuantumSeconds;
      if (e.Xaudio && e.Running && now - (std::max)(pass.Start, e.Started) > NoPassSeconds)
      {
        Log("Voice: Windows' audio gave no processing pass for %.0f s: it's opened again", NoPassSeconds);
        CloseEngine(e, voices);
        e.NextTry = now;
      }
      if (e.Xaudio)
      {
        // Heard only while the focus window is in front; the master's volume
        HWND window = g_FocusWindow.load();
        float focus = !window || GetForegroundWindow() == window ? 1.f : 0.f;
        e.Focus += (focus - e.Focus) * (float)(std::min)(1., dt / FocusFadeSeconds);
        if (fabsf(e.Focus - focus) < 0.01f)
          e.Focus = focus;
        float volume = MasterWanted(e);
        if (volume != e.VolumeSet && SUCCEEDED(e.Master->SetVolume(volume)))
          e.VolumeSet = volume;
        e.Info.MasterVolume = e.VolumeSet;
        XAUDIO2_PERFORMANCE_DATA perf{};
        e.Xaudio->GetPerformanceData(&perf);
        if (newPass && e.Info.Rate > 0 && perf.CurrentLatencyInSamples && ++e.PassesRead > 2)
        {
          // The output delay: one reading a pass, the middle one of the last
          // 9 used. Measured: a new engine's first two passes say 20 and 30 ms
          // before it settles at 39.9 (so those are left out), and now and
          // then a single reading is a whole pass higher.
          double read = (double)perf.CurrentLatencyInSamples / (double)e.Info.Rate;
          e.Readings[e.ReadingCount % 9] = read;
          e.ReadingCount++;
          double sorted[9];
          int have = (std::min)(e.ReadingCount, 9);
          for (int i = 0; i < have; i++)
            sorted[i] = e.Readings[i];
          std::sort(sorted, sorted + have);
          e.Latency = sorted[have / 2];
          if (e.ReadingCount == 1)
            e.Info.LatencyLow = e.Info.LatencyHigh = read;
          e.Info.LatencyLow = (std::min)(e.Info.LatencyLow, read);
          e.Info.LatencyHigh = (std::max)(e.Info.LatencyHigh, read);
        }
        e.Info.Glitches = perf.GlitchesSinceEngineStarted;
        e.Info.EngineCycles += perf.AudioCyclesSinceLastQuery;
        e.Info.EngineTotalCycles += perf.TotalCyclesSinceLastQuery;
      }

      // The voices: once after each pass, and soon enough after it that what's
      // asked for is in before the next one
      bool canAct = e.Xaudio && e.Running && between && pass.Starts != acted && now >= pass.Start &&
        now - pass.Start < ActWithinSeconds;
      if (canAct)
      {
        if (acted != ~0u && pass.Starts - acted < 1000)
          e.Info.MissedPasses += pass.Starts - acted - 1;
        e.Info.Chases++;
        acted = pass.Starts;
      }
      for (int i = 0; i < MaxSounds; i++)
      {
        Voice& v = voices[i];
        if (!v.Used)
          continue;
        Slot& slot = g_Slots[i];
        if (v.Closing)
        {
          bool passes = e.Xaudio && e.Running;
          if (v.Mode == Running && passes)
          {
            // Out over one pass first
            if (canAct)
              Fade(v, pass.Starts, now, false);
            continue;
          }
          if (v.Mode == Fading && passes && pass.Ends < v.MarkStarts + 1 && now - v.MarkTime < 0.05)
            continue;
          DropSource(v);
          v = Voice();
          slot.Pcm.reset();
          slot.State.store(SlotFree);
          continue;
        }
        slot.Asked.Read(v.Last);
        if (canAct)
          Chase(v, e, pass, now);
        Tell(v, slot, now);
      }

      e.Info.Latency = e.Latency;
      e.Info.Passes = pass.Ends;
      e.Info.Wakes = wakes;
      ULONG64 cycles = 0;
      if (QueryThreadCycleTime(GetCurrentThread(), &cycles))
        e.Info.ThreadCycles = cycles;
      g_Info.Write(e.Info);
    }

    // Out without a click: the master down for a pass, then everything let go
    bool heard = false;
    for (int i = 0; i < MaxSounds; i++)
      heard = heard || voices[i].Mode == Running;
    if (heard && e.Master && e.Running)
    {
      e.Master->SetVolume(0.f);
      Sleep(25);
    }
    CloseEngine(e, voices);
    delete[] voices;
    e.Info.Open = 0;
    e.Info.Running = 0;
    g_Info.Write(e.Info);
    if (SUCCEEDED(com))
      CoUninitialize();
    return 0;
  }

  // The thread started unless it runs. All of it under the lock Shutdown holds too: a Shutdown on another thread
  // that fell between the steps here could leave the old thread running beside a new one (measured without the
  // lock: Shutdown in a loop on one thread against Open and Close on another crashed the process in 10 runs of 10)
  void StartThread()
  {
    // (the usual case, with nothing to wait for: it runs)
    if (g_ThreadState.load() == 1)
      return;
    ThreadLocked locked;
    int none = 0;
    if (!g_ThreadState.compare_exchange_strong(none, 1))
      return;
    if (!g_Wake.load())
      g_Wake.store(CreateEventW(nullptr, FALSE, FALSE, nullptr));
    if (!g_Wake.load())
    {
      // No event to wake the thread with (its waits would return at once, all the time): not started
      g_ThreadState.store(0);
      return;
    }
    g_Stop.store(false);
    uintptr_t thread = _beginthreadex(nullptr, 0, Run, nullptr, 0, nullptr);
    if (thread)
      g_Thread.store((HANDLE)thread);
    else
      g_ThreadState.store(0);
  }
}

int voice_player::Open(std::shared_ptr<const std::vector<short>> pcm, int rate, int channels)
{
  if (!pcm || rate < 1000 || rate > 200000 || channels < 1 || channels > 8 || pcm->size() / (size_t)channels < 4)
    return -1;
  for (int i = 0; i < MaxSounds; i++)
  {
    Slot& slot = g_Slots[i];
    int free = SlotFree;
    if (!slot.State.compare_exchange_strong(free, SlotClaimed))
      continue;
    slot.Pcm = std::move(pcm);
    slot.Rate = rate;
    slot.Channels = channels;
    slot.Asked.Write(Wanted{});
    slot.Said.Write(NothingTold());
    unsigned generation = slot.Generation.fetch_add(1) + 1;
    slot.State.store(SlotOpen);
    StartThread();
    Wake();
    return MakeHandle(i, generation);
  }
  return -1;
}

void voice_player::Want(int handle, bool playing, double seconds, float rate, float volume, double at)
{
  Slot* slot = SlotOf(handle);
  if (!slot)
    return;
  Wanted w{};
  // The moment the position is for: the caller's (it is on Clock's own clock, the performance counter in seconds),
  // or now when none is given (0 or less, or not a number) or when it's too far from now to be right
  double now = Clock();
  w.Stamp = at > 0. && at <= now + StampAheadSeconds && at >= now - StampBehindSeconds ? at : now;
  // (a position or rate that isn't a number: not playing)
  bool sane = seconds == seconds && rate == rate && fabs(seconds) < 1e9 && fabsf(rate) < 1e6f;
  w.Playing = playing && sane ? 1 : 0;
  w.Seconds = sane ? seconds : 0.;
  w.Rate = sane ? rate : 0.f;
  w.Volume = volume == volume ? volume : 0.f;
  slot->Asked.Write(w);
}

void voice_player::Close(int handle)
{
  if (handle < 0)
    return;
  int index = handle & 0xFF;
  if (index >= MaxSounds)
    return;
  Slot& slot = g_Slots[index];
  if (MakeHandle(index, slot.Generation.load()) != handle)
    return;
  int open = SlotOpen;
  if (slot.State.compare_exchange_strong(open, SlotClosing))
    Wake();
}

double voice_player::Heard(int handle)
{
  Slot* slot = SlotOf(handle);
  Told told{};
  if (!slot || !slot->Said.Read(told) || !told.PieceSound)
    return -1.;
  // (carried on from what the thread told at its last pass, 10 ms ago at most
  // while it runs)
  double since = Clamp(Clock() - told.PieceTime, 0., 0.5);
  double heard = told.PiecePos + told.PieceSlope * since;
  return heard < told.Duration ? heard : -1.;
}

void voice_player::Shutdown()
{
  // (all of it under the lock StartThread holds too: see there)
  ThreadLocked locked;
  int running = 1;
  if (!g_ThreadState.compare_exchange_strong(running, 2))
    return;
  g_Stop.store(true);
  Wake();
  HANDLE thread = g_Thread.exchange(nullptr);
  if (thread)
  {
    DWORD waited = WaitForSingleObject(thread, 5000);
    CloseHandle(thread);
    // The thread didn't end in time (it's held up somewhere): the slots and the samples are still its own, so
    // nothing is freed, and the state stays "being stopped", so that no second thread is ever started beside it
    if (waited != WAIT_OBJECT_0)
      return;
  }
  // The thread is gone: the slots are nobody else's now
  for (int i = 0; i < MaxSounds; i++)
  {
    Slot& slot = g_Slots[i];
    int state = slot.State.load();
    if (state == SlotOpen || state == SlotClosing)
    {
      slot.Pcm.reset();
      slot.State.store(SlotFree);
    }
  }
  g_ThreadState.store(0);
}

void voice_player::SetMasterVolume(float volume)
{
  g_MasterVolume.store(volume == volume ? volume : 0.f);
}

void voice_player::SetFocusWindow(void* window)
{
  g_FocusWindow.store((HWND)window);
}

void voice_player::SetLatencyTrim(double seconds)
{
  g_LatencyTrim.store(seconds == seconds ? Clamp(seconds, -0.2, 0.5) : 0.);
}

void voice_player::SetLog(void (*write)(const char* text))
{
  g_Log.store(write);
}

bool voice_player::GetStats(int handle, Stats& stats)
{
  Slot* slot = SlotOf(handle);
  Told told{};
  if (!slot || !slot->Said.Read(told))
    return false;
  stats = told.Stats;
  return true;
}

void voice_player::GetEngineInfo(EngineInfo& info)
{
  EngineInfo read{};
  g_Info.Read(read);
  info = read;
}

// For the tests, and not in VoicePlayer.h: they declare it themselves (namespace voice_player { void
// TestDevice(int what); }). 0 as normal, 1 behave as if the audio engine reported a critical error now (once), 2
// behave as if there is no audio device until told otherwise (an engine that's open is let go), 3 behave as if a
// processing pass ran in the middle of the next start (once)
namespace voice_player
{
  void TestDevice(int what)
  {
    if (what == 1)
    {
      g_CriticalCode.store((long)XAUDIO2_E_DEVICE_INVALIDATED);
      g_Critical.store(true);
    }
    if (what == 3)
    {
      g_TestRetry.store(true);
      return;
    }
    g_TestNoDevice.store(what == 2 ? 1 : 0);
    Wake();
  }
}
