#pragma once
#include <memory>
#include <vector>

// Sounds that follow a clock set from outside: decoded 16-bit sound played through Windows' own audio (XAudio2,
// loaded when first needed), each at the position, speed and volume the caller says every frame. Made for a voice
// file whose mouth shapes are laid over a face at the game's time: the sound is heard where the game's clock is,
// through pauses, slow motion, jumps and loops.
//
// Open, Want, Close and Heard never wait. The work is done on one thread of the player's own, woken by XAudio2 at
// the end of each of its processing passes (every 10 ms). The audio engine is made when a sound is first opened and
// stopped 2 s after the last one is closed. Without an audio device, or when it is lost, the sounds are silent and
// the engine is tried again every 3 s while a sound is open.
//
// To know before using it (measured; more in VoicePlayer.cpp):
// - The pitch goes with the rate, as on a tape. Outside MinRate - MaxRate a sound is silent.
// - A sound wanted from its very start loses its first 60 - 94 ms (that long passes from the Want that says a new
//   place to that place being heard), unless its clock runs in from before 0. A sound shorter than about 0.11 s
//   does not play from its start at all.
// - Stops, starts and jumps are faded over 1 ms. A sound's own end is not: its last samples are played as they
//   are, so the caller fades them before Open.
// - While any sound is open, playing or not, the engine and the thread run: 100 wakes a second, about 0.1 % of a
//   core, and XAudio2's own processing 0.15 - 0.3 % more.
namespace voice_player
{
  // Any thread, never waits (but see Shutdown): a sound to play, interleaved 16-bit samples at rate Hz
  // (1000 - 200000), 1 - 8 channels. The samples are kept (shared) until it's closed. XAudio2 reads them in place,
  // no copy is made: they must never be changed after this call. Returns a handle, or -1 (nothing given, fewer than
  // 4 samples a channel, a rate or a channel count outside those, or all of MaxSounds places taken). Nothing is
  // heard until Want says so. Of a sound with more than 2 GB of samples only the first 2 GB are played (the log is
  // told).
  int Open(std::shared_ptr<const std::vector<short>> pcm, int rate, int channels);

  // One thread at a time for a handle, never waits; called every frame while the sound should follow: at the
  // moment "at" the wanted position is seconds into the sound, moving on at rate (1 real time, 0.5 half speed, 0
  // stopped), at volume 0 - 1 (up to 2).
  // "at" is the moment seconds is for, as the performance counter in seconds (QueryPerformanceCounter's count
  // divided by QueryPerformanceFrequency's): read it in the same breath as the clock seconds comes from, and it
  // doesn't matter how much later in the frame Want is called. 0 or less, or not a number: the moment of the call.
  // A moment more than 0.1 s after the call or more than 0.5 s before it is taken as the moment of the call too.
  // Between calls the position is carried on at that rate; StaleSeconds after the last moment said the sound stops
  // by itself (the game hung, or the caller went away). Silent while not playing, while the rate is outside
  // MinRate - MaxRate, and while the position is before the sound's start or past its end.
  void Want(int handle, bool playing, double seconds, float rate, float volume, double at = 0.0);

  // Any thread, never waits: the sound let go (faded out first when heard). Its place is free for another Open
  // only once the player's thread has let it go, some tens of ms later: an Open right after MaxSounds Closes can
  // return -1.
  void Close(int handle);

  // Any thread, never waits: the position (seconds) of the sound that reaches the speakers now, by XAudio2's own
  // count of what the voice has played and the output delay it reports (with SetLatencyTrim's added); -1 when
  // nothing of it is heard now
  double Heard(int handle);

  // The thread stopped and everything released (the audio engine, the voices, the samples); every handle is void
  // afterwards. Open starts it all again. It waits for the thread (measured: a few ms with nothing playing, up to
  // 0.1 s otherwise; 5 s at most, and should the thread not have ended by then nothing is released and no sound
  // plays from then on). So: not from DllMain, and not while other threads call Open (an Open that meets it waits
  // for it, and its handle may be void at once). A process may simply exit without it.
  void Shutdown();

  // Every sound times this (0 - 1; 1 at first), e.g. the game's own volume
  void SetMasterVolume(float volume);
  // Heard only while this window (an HWND) is in front, faded out and in (measured: half after 0.11 s, all of it
  // after 0.67 s); null (as at first): always
  void SetFocusWindow(void* window);
  // Added to the output delay XAudio2 reports (seconds, -0.2 to 0.5), for an output that is later than it says
  // (wireless headphones)
  void SetLatencyTrim(double seconds);
  // Told of what happens (the engine opened, lost, ...), from the player's thread; null: nothing told
  void SetLog(void (*write)(const char* text));

  const int MaxSounds = 16;
  const float MinRate = 0.25f;
  const float MaxRate = 2.f;
  const double StaleSeconds = 0.25;

  // How a sound is doing (for a menu's status line, and the tests)
  struct Stats
  {
    double Time;          // when this was worked out (the performance counter in seconds, as Want's "at")
    double Heard;         // the position reaching the speakers then, -1 when none
    double Wanted;        // the wanted position then (carried on from the last Want), -1 when it isn't to play
    double Error;         // Heard - Wanted in seconds of the sound (0 when either is missing)
    double ChaseError;    // what the chase works on: what is being made now against what's wanted when it's
                          // heard, seconds of real time
    float Ratio;          // the frequency ratio set on the voice
    int Mode;             // 0 no voice yet, 1 being readied, 2 stopped, 3 playing, 4 fading out
    int Sounding;         // 1 when something of it reaches the speakers at Time
    int ShouldSound;      // 1 when it's wanted to be heard at Time
    int Starts;           // started from silence (the first time, after a pause too long to go on from, after
                          // silence)
    int Restarts;         // started again from another place while it played (a jump, or too far off)
    int Resumes;          // went on from where it had stopped
    int Fades;            // faded out (to stop, or to start again)
    int Ends;             // played to its end
    int RatioChanges;     // SetFrequencyRatio calls while it played
    int Retries;          // a start that had to be done again (a processing pass ran in the middle of it)
  };
  bool GetStats(int handle, Stats& stats);

  struct EngineInfo
  {
    int Open;                 // 1 while the audio engine and its mastering voice exist
    int Running;              // 1 while it processes (it's stopped when no sound has been open for 2 s)
    int Channels, Rate;       // the output's
    double Latency;           // the output delay XAudio2 reports (seconds), without the trim: the value used
    double LatencyLow;        // and the lowest and highest single readings since the engine was made
    double LatencyHigh;
    unsigned Glitches;        // since the engine started
    unsigned Passes;          // processing passes seen so far (the count goes on over a Shutdown)
    int Opens;                // how many times the engine was made
    int Failures;             // how many times making it failed
    long LastError;           // the last failure's or loss's HRESULT
    unsigned long long EngineCycles;        // XAudio2's own count of what it spent processing
                                            // (XAUDIO2_PERFORMANCE_DATA)
    unsigned long long EngineTotalCycles;   // and of all the time gone by, in the same unit
    unsigned long long ThreadCycles;        // CPU cycles of the player's own thread (QueryThreadCycleTime)
    unsigned Wakes;           // how many times the thread woke
    unsigned Chases;          // passes after which the voices were looked at and steered
    unsigned MissedPasses;    // passes after which they weren't (the thread came too late, or two passes ran
                              // together)
    float MasterVolume;       // the volume set on the mastering voice (the master volume x the focus fade)
  };
  void GetEngineInfo(EngineInfo& info);
}
