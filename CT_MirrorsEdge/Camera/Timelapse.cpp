#include "Timelapse.h"
#include "../Util/Util.h"
#include "../Util/ImGuiEXT.h"
#include "../imgui/imgui.h"
#include "../inih/cpp/INIReader.h"
#include <Windows.h>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>

// See Timelapse.h. Update and DrawUI run under CameraManager's lock, so the state here has no lock of its own; the
// menu only asks (g_Command), Update (the game thread) does.
namespace
{
  const double DaySeconds = 86400.0;
  const int FrameRates[] = { 24, 25, 30, 50, 60 };
  const int FrameRateCount = (int)(sizeof(FrameRates) / sizeof(FrameRates[0]));
  const int DefaultRate = 2;   // 30 fps
  const int FramesMax = 20000;
  const float SecondsMin = 0.1f;
  const float SecondsMax = 600.f;
  const float MinFrameMax = 60.f;
  // A frame's light given up on after this long with game time running. Measured 2026-11-13 on a normal level: a
  // reset's three steps 3.0 - 4.2 s; with the whole city loaded one pass took about 2.7 s (2026-10-24), so its
  // seven passes about 19 s.
  const double LightMaxSeconds = 120.0;
  // and after this long of real time not paused (at a very low game speed 120 game s would be hours)
  const double LightMaxRealSeconds = 600.0;
  // The game's lighting not updating this long with game time running: said in the menu, and once in CT.log
  const long long LightStallMs = 3000;
  // Reflections the tools can't watch: taken as captured this long after the light (a round of the nearest 32
  // took 1 - 2 s, report section 10)
  const double ReflectionsGuessSeconds = 2.5;
  // Once all of it is done, the frame is taken after this many more of the camera hook's frames and this long: the
  // picture shown can be a frame or two behind the game thread
  const int SettleFrames = 4;
  const double SettleSeconds = 0.15;
  const double ShotMaxSeconds = 30.0;
  // After the game's pause menu or loading screen, the picture is taken this long after it went (its fade)
  const double UnpauseSeconds = 0.5;
  // Along a track: after the camera moves to the frame's place, this long (and this many frames) before its light
  // is asked for, so what streams in there for the new view is there for the light's passes (how long streaming
  // takes there isn't known; a frame's step along a track is usually short)
  const double TrackMoveSeconds = 1.0;
  const int TrackMoveFrames = 10;
  // The track played on or moved this many times in one frame: the time-lapse stops (someone's using it)
  const int ReholdsMax = 5;
  // The light is the frame's own time either way (the hook sets the reset's jobs to it); a lighting clock this far
  // from it when its reset was done means the game's clock didn't follow the tools' at all: the frame is done again
  // once, then the time-lapse stops
  const float ClockToleranceSeconds = 60.f;
  // Served without a reset (its own reset counting down, say): asked again once after this long, then it stops
  const double NoResetWaitSeconds = 2.0;
  // The first frames of a run keep the reset's own lines in CT.log (they show it working); later ones only write
  // the frame's own line
  const int LoudFrames = 2;
  // For the menu's estimate: a frame's time on a normal level (the reset's three steps 3 - 4 s, 2026-11-13, then
  // 1 - 2 s of reflections), a guess until frames have been made
  const double GuessFrameSeconds = 5.5;
  // Game time a camera hook frame counts for at most: the game simulates at most one 30 Hz tick a rendered frame
  // (its single player fixed tick, Research_GameSpeed section 2), so a slow frame is one tick of the world
  const double TickSeconds = 1.0 / 30.0;

  enum LapseCamera { Mode_Still, Mode_Track, Mode_Count };
  enum LapseCount { Count_Video, Count_Every };
  // One picture every: these many game seconds at least and at most (a day at 1 s is 86,400 pictures: FramesMax cuts
  // it)
  const float EveryMin = 1.f;
  const float EveryMax = 4.f * 3600.f;

  struct Settings
  {
    int Mode{ Mode_Still };
    float From{ 6.f * 3600.f };     // game seconds 0 - 86400, whole minutes
    float To{ 20.f * 3600.f };
    bool Backwards{ false };        // the clock goes back from From to To
    bool TrackClock{ false };       // along a track: its keyframes' time of day, not From - To
    float Seconds{ 10.f };          // the video's length
    int Rate{ DefaultRate };        // FrameRates
    float MinFrame{ 0.f };          // game seconds between frames at least (even world motion)
    // How many pictures: from the video's length (Seconds x the frame rate), or one every Every game seconds of the
    // clock from From on (a time-lapse camera's interval; shown in EveryUnit: 0 seconds, 1 minutes)
    int Count{ Count_Video };
    float Every{ 60.f };
    int EveryUnit{ 1 };
  };

  enum Phase { Phase_Idle, Phase_Set, Phase_Move, Phase_Light, Phase_Reflections, Phase_Settle, Phase_Shot };

  // What a run makes, fixed when it starts (a carry-on goes on with it)
  struct Plan
  {
    Settings S;
    int Frames{ 0 };
    bool CameraOn{ false };
    std::string Track;
    int TrackKeys{ 0 };
    double TrackLength{ 0.0 };
    float Pose[8]{};                // Still: where the camera is held
    long long Level{ 0 };           // the level's clock (another level: stop)
    std::wstring Folder;            // ends with a backslash
    std::string FolderText;         // the same, for the log and the menu
  };

  struct Run
  {
    Phase At{ Phase_Idle };
    Plan P;
    int First{ 0 };                 // this run's first frame (a carry-on starts later)
    int Frame{ 0 };                 // the frame being made
    bool Retried{ false };          // this frame was lit again once (its clock far off)
    bool NoResetRetried{ false };   // asked again once after being served without a reset
    int Reholds{ 0 };               // this frame's track held again (played on or moved)
    double LastPausedReal{ -1e30 }; // Now() when the game was last seen paused, loading or its window hidden
    double LightReal{ 0.0 };        // real seconds not paused since the frame's light was asked for
    bool ShotSpoiled{ false };      // paused or hidden while the picture was being taken: taken again
    double PrevShot{ -1.0 };        // LastShot before this frame's picture was asked for
    long Request{ 0 };
    float Target{ 0.f };            // its time of day
    float LitAt{ -1.f };
    float ClockAt{ -1.f };          // the lighting's own clock at its reset
    double ResetSeconds{ 0.0 };
    timelapse::Host::ReflectionInfo Refl{ false, 0, 0, 0 };
    double TrackTime{ 0.0 };
    double Last{ -1.0 };            // Now() at the last update
    double Running{ 0.0 };          // game seconds since the start (the world's time: see TickSeconds)
    double PhaseRunning{ 0.0 };     // Running when the phase began
    double PhaseReal{ 0.0 };        // Now() when the phase began
    double FrameReal{ 0.0 };        // Now() when the frame began
    double LastShot{ -1.0 };        // Running when the last frame was taken (-1: none this run)
    double Gap{ -1.0 };             // Running between the last two frames taken
    double LightSeconds{ 0.0 };
    double ReflSeconds{ 0.0 };
    double ShotAsked{ 0.0 };
    int SettleCount{ 0 };
    double Started{ 0.0 };
    int Saved{ 0 };
    double FrameSum{ 0.0 };         // real seconds the frames saved took, for the estimate
    double GapMin{ 1e30 }, GapMax{ 0.0 };
    bool StallLogged{ false };
    bool ReflUnknownLogged{ false };
    char Status[200]{};
  };

  enum Command { Command_None, Command_Start, Command_Continue, Command_Stop };

  Settings g_S;
  Run g_Run;
  Command g_Command = Command_None;
  // After a stop: the plan and the frame it can carry on from
  bool g_CanContinue = false;
  Plan g_ContinuePlan;
  int g_ContinueFrame = 0;
  // The last outcome, and the line that makes the video, shown under the buttons
  std::string g_Message;
  char g_Ffmpeg[512] = "";

  float WrapClock(double seconds)
  {
    double s = fmod(seconds, DaySeconds);
    if (s < 0.0)
      s += DaySeconds;
    float f = (float)s;
    return f >= 86400.f ? 0.f : f;
  }

  float WholeMinute(float seconds)
  {
    return WrapClock(floor((double)seconds / 60.0 + 0.5) * 60.0);
  }

  // Game seconds between two times of day, round the clock
  float ClockDistance(float a, float b)
  {
    double d = fmod(fabs((double)a - b), DaySeconds);
    return (float)(d > DaySeconds / 2 ? DaySeconds - d : d);
  }

  void Hhmm(double seconds, char* out, size_t size)
  {
    int s = (int)WrapClock(seconds);
    sprintf_s(out, size, "%02d:%02d", s / 3600 % 24, s / 60 % 60);
  }

  void Hhmmss(double seconds, char* out, size_t size)
  {
    int s = (int)WrapClock(seconds);
    sprintf_s(out, size, "%02d:%02d:%02d", s / 3600 % 24, s / 60 % 60, s % 60);
  }

  void Duration(double seconds, char* out, size_t size)
  {
    if (seconds < 60.0)
      sprintf_s(out, size, "%.0f s", seconds);
    else if (seconds < 3600.0)
      sprintf_s(out, size, "%d min %02d s", (int)(seconds / 60.0), (int)fmod(seconds, 60.0));
    else
      sprintf_s(out, size, "%d h %02d min", (int)(seconds / 3600.0), (int)fmod(seconds / 60.0, 60.0));
  }

  // Game seconds the clock goes from From to To (back with Backwards); a whole day when they're the same
  double Span(Settings const& s)
  {
    double d = s.Backwards ? (double)s.From - s.To : (double)s.To - s.From;
    d = fmod(d, DaySeconds);
    if (d < 0.0)
      d += DaySeconds;
    return d < 30.0 ? DaySeconds : d;
  }

  bool WholeDay(Settings const& s)
  {
    return Span(s) >= DaySeconds;
  }

  // Game seconds between two frames: From and To are both frames, except over a whole day, where the frame after
  // the last would be the first again (the video loops)
  // Whether the pictures go by the interval (One picture every): only for a clock From - To (not a track's own time
  // of day)
  bool ByInterval(Settings const& s)
  {
    return s.Count == Count_Every && !(s.Mode == Mode_Track && s.TrackClock) && s.Every >= EveryMin;
  }

  // How many pictures: the video's length x its frame rate; or by the interval, every Every game s from From while it's
  // still within To (From and To both pictures when it fits: a whole day, as many as fit in it, the next would be From
  // again), at most FramesMax
  // The pictures the interval gives over From - To before FramesMax (over a whole day as many as fit, the next would be
  // From again). With a little tolerance, so an interval that divides the span exactly isn't a picture short from
  // float rounding (2026-11-16 review: 2.1 min from the slider was 126.0000076 s, which left out the 20:00 picture of
  // 06:00 - 20:00)
  double IntervalPictures(Settings const& s)
  {
    double q = Span(s) / s.Every;
    double k = floor(q + 1e-6 * (q > 1.0 ? q : 1.0));
    return WholeDay(s) ? k : k + 1.0;
  }

  int FramesOf(Settings const& s)
  {
    double n = ByInterval(s) ? IntervalPictures(s) : floor((double)s.Seconds * FrameRates[s.Rate] + 0.5);
    return n < 2.0 ? 2 : (n > FramesMax ? FramesMax : (int)n);
  }

  // Whether FramesOf had to cut the pictures by the interval short (more than FramesMax)
  bool IntervalCut(Settings const& s)
  {
    return ByInterval(s) && IntervalPictures(s) > FramesMax;
  }

  // An interval longer than From - To: the two pictures are From and To (see Step)
  bool IntervalLonger(Settings const& s)
  {
    return ByInterval(s) && !WholeDay(s) && Span(s) < s.Every;
  }

  // Over a whole day by the interval: game seconds from the last picture until From comes round again (the interval
  // itself when it divides 24 hours, so the video loops evenly)
  double DayGapAtEnd(Settings const& s)
  {
    return DaySeconds - s.Every * (FramesOf(s) - 1);
  }

  double Step(Settings const& s, int frames)
  {
    if (ByInterval(s))
    {
      // (an interval longer than From - To: the second picture is To, not after it)
      if (!WholeDay(s) && frames >= 2 && s.Every * (frames - 1) > Span(s) + 0.5)
        return Span(s) / (frames - 1);
      return s.Every;
    }
    return WholeDay(s) ? Span(s) / frames : Span(s) / (frames - 1);
  }

  // The interval as kept: whole game seconds (the slider's 0.1 game minute is 6 s), within EveryMin - EveryMax; 60 for
  // nothing usable (a hand-edited config)
  float RoundEvery(float every)
  {
    if (!(every > 0.f))
      return 60.f;
    every = floorf(every + 0.5f);
    return every < EveryMin ? EveryMin : (every > EveryMax ? EveryMax : every);
  }

  float ClockAt(Settings const& s, int frames, int frame)
  {
    double d = Step(s, frames) * frame;
    return WrapClock(s.Backwards ? s.From - d : s.From + d);
  }

  // Along a track: its start to its end
  double TrackTimeAt(Plan const& p, int frame)
  {
    return p.Frames > 1 ? p.TrackLength * frame / (p.Frames - 1) : 0.0;
  }

  // Whether a track has a time of day at every frame of a plan (from first on)
  bool TrackClockAtEveryFrame(timelapse::Host& host, Plan const& trial, int first)
  {
    for (int i = first; i < trial.Frames; ++i)
    {
      float clock = -1.f;
      if (!host.TrackTimeOfDay(TrackTimeAt(trial, i), clock))
        return false;
    }
    return true;
  }

  std::wstring FramePath(Plan const& p, int frame)
  {
    wchar_t name[32];
    swprintf_s(name, L"%05d.png", frame);
    return p.Folder + name;
  }

  // A line added to the folder's Time-lapse.txt
  void Note(Plan const& p, char const* format, ...)
  {
    FILE* f = nullptr;
    if (_wfopen_s(&f, (p.Folder + L"Time-lapse.txt").c_str(), L"a") != 0 || !f)
      return;
    va_list args;
    va_start(args, format);
    vfprintf(f, format, args);
    va_end(args);
    fputc('\n', f);
    fclose(f);
  }

  void SetStatus(char const* format, ...)
  {
    va_list args;
    va_start(args, format);
    vsnprintf(g_Run.Status, sizeof(g_Run.Status), format, args);
    va_end(args);
  }

  void Enter(Phase phase, double now)
  {
    g_Run.At = phase;
    g_Run.PhaseRunning = g_Run.Running;
    g_Run.PhaseReal = now;
    g_Run.SettleCount = 0;
  }

  void Release(timelapse::Host& host)
  {
    if (g_Run.P.S.Mode == Mode_Track)
      host.TrackRelease();
    else
      host.HoldCamera(false, nullptr);
    host.LightEnd();
  }

  void Stop(timelapse::Host& host, char const* why, bool error)
  {
    Run& r = g_Run;
    // (a picture asked for and not taken yet isn't taken: Continue makes that frame again)
    if (r.At == Phase_Shot)
      host.ShotCancel();
    Release(host);
    g_CanContinue = r.Frame < r.P.Frames;
    if (g_CanContinue)
    {
      g_ContinuePlan = r.P;
      g_ContinueFrame = r.Frame;
    }
    char text[400];
    sprintf_s(text, "Stopped at frame %d of %d: %s. %d saved this time, in %s", r.Frame + 1, r.P.Frames, why, r.Saved,
      r.P.FolderText.c_str());
    if (error)
      util::log::Warning("Time-lapse: %s", text);
    else
      util::log::Write("Time-lapse: %s", text);
    Note(r.P, "%s", text);
    g_Message = text;
    r.At = Phase_Idle;
  }

  void Finish(timelapse::Host& host, double now)
  {
    Run& r = g_Run;
    Release(host);
    g_CanContinue = false;
    char took[32];
    Duration(now - r.Started, took, sizeof(took));
    // (the pad makes an odd width or height even, which yuv420p needs; nothing changes for even ones)
    sprintf_s(g_Ffmpeg, "ffmpeg -framerate %d -i %%05d.png -vf \"pad=ceil(iw/2)*2:ceil(ih/2)*2\" -c:v libx264 -crf 16 -pix_fmt "
      "yuv420p Time-lapse.mp4", FrameRates[r.P.S.Rate]);
    char gaps[96] = "";
    if (r.GapMax > 0.0)
      sprintf_s(gaps, ", %.1f - %.1f game s between frames", r.GapMin, r.GapMax);
    char text[600];
    sprintf_s(text, "Done: %d frames (%d this time) in %s%s, in %s. The video, from that folder: %s", r.P.Frames, r.Saved, took,
      gaps, r.P.FolderText.c_str(), g_Ffmpeg);
    util::log::Write("Time-lapse: %s", text);
    Note(r.P, "%s", text);
    g_Message = text;
    r.At = Phase_Idle;
  }

  bool Start(timelapse::Host& host, bool carryOn, double now)
  {
    char why[256] = "";
    auto fail = [&](char const* text)
    {
      g_Message = std::string("Not started: ") + text;
      util::log::Warning("Time-lapse: not started: %s", text);
      return false;
    };
    if (carryOn && !g_CanContinue)
      carryOn = false;
    Plan p;
    int first = 0;
    if (carryOn)
    {
      p = g_ContinuePlan;
      first = g_ContinueFrame;
    }
    else
    {
      p.S = g_S;
      p.Frames = FramesOf(g_S);
      p.CameraOn = host.CameraOn();
    }
    if (host.ShotBusy())
      return fail("a frame picture is still being written, a moment");
    if (!host.LightUsable(why, sizeof(why)))
      return fail(why);
    if (host.Clock() < 0.f)
      return fail("load a level first");
    if (host.CameraOn() != p.CameraOn)
      return fail(p.CameraOn ? "it was made with the camera on: turn it on again" : "it was made with the camera off: turn it off "
        "again");
    if (carryOn && host.LevelKey() != p.Level)
      return fail("another level is loaded than the one it was made in (Start makes a new one)");
    // (a still one carried on: it would stop at once, a track playing changes the game speed and the camera)
    if (carryOn && p.S.Mode == Mode_Still && host.TrackPlaying())
      return fail("a camera track is playing and this time-lapse was made still: stop the track first");
    p.Level = host.LevelKey();
    // A camera track playing when Start is pressed with Still picked: the time-lapse goes along it (the user,
    // 2026-11-16: "timelapse should be able to record it even if you have a camera track"), its start to its end, with
    // the track's own time of day when the track sets it as it plays and has one at every frame, else From - To
    bool alongPlaying = !carryOn && p.S.Mode == Mode_Still && host.TrackPlaying();
    if (alongPlaying)
    {
      p.S.Mode = Mode_Track;
      p.S.TrackClock = false;
    }
    if (p.S.Mode == Mode_Track)
    {
      if (!host.CameraOn())
        return fail("turn the camera on: the track plays with it");
      int keys = host.TrackKeyframes();
      double length = host.TrackLength();
      if (keys < 2)
        return fail("the selected camera track needs 2 keyframes or more");
      if (!(length > 0.0))
        return fail("the selected camera track's keyframes are all at 0 s");
      if (carryOn && (host.TrackName() != p.Track || keys != p.TrackKeys || length != p.TrackLength))
        return fail("the selected track isn't the one it was made along, or its keyframes changed (Start makes a new one)");
      p.Track = host.TrackName();
      p.TrackKeys = keys;
      p.TrackLength = length;
      // (with its own clock the pictures go by the video's length, see ByInterval)
      if (alongPlaying && host.TrackSetsTime())
      {
        Plan trial = p;
        trial.S.TrackClock = true;
        trial.Frames = FramesOf(trial.S);
        if (TrackClockAtEveryFrame(host, trial, first))
        {
          p.S.TrackClock = true;
          p.Frames = trial.Frames;
        }
      }
      if (p.S.TrackClock)
        for (int i = first; i < p.Frames; ++i)
        {
          float clock = -1.f;
          if (!host.TrackTimeOfDay(TrackTimeAt(p, i), clock))
          {
            sprintf_s(why, "the track has no time of day at %.2f s (frame %d): give its keyframes one, or untick Time of day "
              "from the track's keyframes", TrackTimeAt(p, i), i + 1);
            return fail(why);
          }
        }
    }
    else if (!carryOn)
      host.CameraPose(p.Pose);

    if (!carryOn)
    {
      SYSTEMTIME t;
      GetLocalTime(&t);
      wchar_t stamp[MAX_PATH], folder[MAX_PATH];
      swprintf_s(stamp, L"Cinematic Tools\\Captures\\Time-lapse %04d-%02d-%02d %02d-%02d-%02d", t.wYear, t.wMonth, t.wDay, t.wHour,
        t.wMinute, t.wSecond);
      CreateDirectoryW(L"Cinematic Tools", nullptr);
      CreateDirectoryW(L"Cinematic Tools\\Captures", nullptr);
      // A folder of its own, never one already there (two started in the same second)
      bool made = false;
      for (int n = 1; n <= 99 && !made; ++n)
      {
        if (n == 1)
          wcscpy_s(folder, stamp);
        else
          swprintf_s(folder, L"%s (%d)", stamp, n);
        made = CreateDirectoryW(folder, nullptr) != 0;
        if (!made && GetLastError() != ERROR_ALREADY_EXISTS)
          break;
      }
      if (!made)
        return fail("its folder couldn't be made in Cinematic Tools\\Captures");
      p.Folder = std::wstring(folder) + L"\\";
      char text[MAX_PATH * 3];
      WideCharToMultiByte(CP_UTF8, 0, folder, -1, text, sizeof(text), nullptr, nullptr);
      p.FolderText = text;

      char from[16], to[16];
      Hhmm(p.S.From, from, sizeof(from));
      Hhmm(p.S.To, to, sizeof(to));
      Note(p, "Mirror's Edge Catalyst time-lapse (Cinematic Tools), started %04d-%02d-%02d %02d:%02d:%02d", t.wYear, t.wMonth, t.wDay,
        t.wHour, t.wMinute, t.wSecond);
      Note(p, "Frames: %d at %d fps (a %.2f s video)%s", p.Frames, FrameRates[p.S.Rate], (double)p.Frames / FrameRates[p.S.Rate],
        ByInterval(p.S) ? (IntervalCut(p.S) ? ", one every so many game s of the clock, cut at the most there can be" :
        ", one every so many game s of the clock") : "");
      if (p.S.Mode == Mode_Track && p.S.TrackClock)
        Note(p, "Clock: the track's time of day keyframes");
      else
        Note(p, "Clock: %s to %s%s%s, a frame every %.1f game s", from, to, p.S.Backwards ? ", backwards" : "",
          WholeDay(p.S) ? " (a whole day: the frame after the last would be the first)" : "", Step(p.S, p.Frames));
      if (p.S.Mode == Mode_Track)
        Note(p, "Camera: along the track '%s' (%d keyframes, %.2f s), its start to its end%s", p.Track.c_str(), p.TrackKeys,
          p.TrackLength, alongPlaying ? " (it was playing when the time-lapse started)" : "");
      else if (p.CameraOn)
        Note(p, "Camera: still, at x %.2f y %.2f z %.2f, field of view %.1f", p.Pose[0], p.Pose[1], p.Pose[2], p.Pose[7]);
      else
        Note(p, "Camera: the game's own (the free camera off)");
      Note(p, "Game time between frames at least: %.1f s", p.S.MinFrame);
      Note(p, "Columns: the picture; its time of day; the time lit; the lighting's own clock then; seconds for the light and "
        "for the reflections; the reflections' round (marked: all seen marked to be captured again; ended: settled, none "
        "near, 15 s, or not watchable; still waiting / loaded); game seconds since the frame before");
      Note(p, "picture    time      lit at    its clock  light s  reflections s  marked  ended       waiting  game s");
    }
    else
      Note(p, "Carried on at frame %d (%05d.png)", first + 1, first);

    g_Run = Run{};
    Run& r = g_Run;
    r.P = p;
    r.First = first;
    r.Frame = first;
    r.Started = now;
    r.Last = now;
    Enter(Phase_Set, now);
    g_CanContinue = false;
    g_Ffmpeg[0] = 0;
    g_Message.clear();
    if (p.S.Mode == Mode_Still)
      host.HoldCamera(true, p.Pose);

    char from[16], to[16];
    Hhmm(p.S.From, from, sizeof(from));
    Hhmm(p.S.To, to, sizeof(to));
    char clock[160];
    if (p.S.Mode == Mode_Track && p.S.TrackClock)
      sprintf_s(clock, "the track's own time of day");
    else
      sprintf_s(clock, "%s to %s%s, a frame every %.1f game s", from, to, p.S.Backwards ? " backwards" : "", Step(p.S, p.Frames));
    util::log::Write("Time-lapse: %s, frames %d - %d of %d (%d fps), %s; %s; into %s", carryOn ? "carried on" : "started", first + 1,
      p.Frames, p.Frames, FrameRates[p.S.Rate], clock, p.S.Mode == Mode_Track ? ("along the track '" + p.Track + "'").c_str() :
      (p.CameraOn ? "the camera held still" : "the game's own camera"), p.FolderText.c_str());
    if (alongPlaying)
      util::log::Write("Time-lapse: the camera track '%s' was playing, so it goes along it (Still was picked), its start to its "
        "end, %s", p.Track.c_str(), p.S.TrackClock ? "with the track's own time of day (it sets it as it plays)" :
        (host.TrackSetsTime() ? "with From - To (the track sets the time of day, but not at every frame)" : "with From - To (the "
        "track doesn't set the time of day)"));
    if (!host.StepsHoldable())
      util::log::Warning("Time-lapse: the game's own lighting steps can't be held (their code isn't what the tools know): a frame "
        "that waits more than about 8 s after its reset could show the next step's light");
    // (Still: the held camera has no shake, see CameraManager's hold)
    if (p.S.Mode == Mode_Track && host.CameraShakes())
      util::log::Write("Time-lapse: camera shake is on, so every frame gets another shake");
    if (host.HudShown())
      util::log::Write("Time-lapse: the game's HUD is switched on (GAME > HUD): whatever of it is on screen is in the pictures");
    return true;
  }

  // The light asked for at the frame's time (a quiet request after the run's first LoudFrames)
  bool AskLight(timelapse::Host& host, double now)
  {
    Run& r = g_Run;
    r.Request = host.LightAt(r.Target, r.Frame - r.First >= LoudFrames);
    if (!r.Request)
      return false;
    r.LightReal = 0.0;
    Enter(Phase_Light, now);
    char at[16];
    Hhmm(r.Target, at, sizeof(at));
    SetStatus("Frame %d of %d at %s: the light", r.Frame + 1, r.P.Frames, at);
    return true;
  }

  void Advance(timelapse::Host& host, double now)
  {
    Run& r = g_Run;
    double dt = now - r.Last;
    if (!(dt >= 0.0))
      dt = 0.0;
    r.Last = now;
    bool frozen = host.TimeFrozen(), paused = host.GamePaused(), hidden = host.WindowHidden();
    // The world's time: the game speed's, at most a tick a rendered frame (see TickSeconds); none while paused
    if (!paused)
    {
      double scale = host.WorldScale();
      double game = dt * (scale > 0.0 ? scale : 0.0);
      r.Running += game > TickSeconds ? TickSeconds : game;
      // (a long hitch counts for a second at most)
      r.LightReal += dt > 1.0 ? 1.0 : dt;
    }
    // (no picture within UnpauseSeconds of the pause menu, a loading screen or a hidden window: their fade)
    if (paused || hidden)
      r.LastPausedReal = now;

    // What it can't go on without
    if (host.Clock() < 0.f)
      return Stop(host, "the level went (no time of day)", true);
    if (host.LevelKey() != r.P.Level)
      return Stop(host, "another level was loaded", true);
    if (host.CameraOn() != r.P.CameraOn)
      return Stop(host, r.P.CameraOn ? "the camera was turned off" : "the camera was turned on", true);
    if (r.P.S.Mode == Mode_Track && (host.TrackKeyframes() != r.P.TrackKeys || host.TrackName() != r.P.Track ||
        host.TrackLength() != r.P.TrackLength))
      return Stop(host, "another camera track was picked, or its keyframes changed", true);
    if (r.P.S.Mode == Mode_Still && host.TrackPlaying())
      return Stop(host, "a camera track was played (it would change the game speed and the camera)", true);
    // The track let go of meanwhile (played on or stopped from its keys): held again and the frame done again
    if (r.P.S.Mode == Mode_Track && r.At != Phase_Set && r.At != Phase_Shot && !host.TrackHeld(r.TrackTime))
    {
      if (++r.Reholds > ReholdsMax)
        return Stop(host, "the track kept being played or moved while the frame was made", true);
      host.TrackHold(r.TrackTime);
      Enter(Phase_Set, now);
      return;
    }

    double phaseRunning = r.Running - r.PhaseRunning;
    double phaseReal = now - r.PhaseReal;
    int shown = r.Frame + 1;
    char at[16];
    Hhmm(r.Target, at, sizeof(at));
    switch (r.At)
    {
    case Phase_Set:
    {
      r.TrackTime = r.P.S.Mode == Mode_Track ? TrackTimeAt(r.P, r.Frame) : 0.0;
      if (r.P.S.Mode == Mode_Track)
        host.TrackHold(r.TrackTime);
      float clock = ClockAt(r.P.S, r.P.Frames, r.Frame);
      if (r.P.S.Mode == Mode_Track && r.P.S.TrackClock && !host.TrackTimeOfDay(r.TrackTime, clock))
        return Stop(host, "the track has no time of day there any more", true);
      // (a track's own can be 86400.0 just before midnight going backwards)
      r.Target = WrapClock(clock);
      r.FrameReal = now;
      if (r.P.S.Mode == Mode_Track)
      {
        Enter(Phase_Move, now);
        Hhmm(r.Target, at, sizeof(at));
        SetStatus("Frame %d of %d at %s: the camera moved to its place", shown, r.P.Frames, at);
        break;
      }
      if (!AskLight(host, now))
        return Stop(host, "its light couldn't be asked for (see CT.log)", true);
      break;
    }
    case Phase_Move:
    {
      ++r.SettleCount;
      if (r.SettleCount >= TrackMoveFrames && phaseReal >= TrackMoveSeconds && !AskLight(host, now))
        return Stop(host, "its light couldn't be asked for (see CT.log)", true);
      break;
    }
    case Phase_Light:
    {
      float litAt = -1.f, clockAt = -1.f;
      double seconds = 0.0;
      int state = host.LightState(r.Request, litAt, clockAt, seconds);
      if (state == timelapse::Light_Done)
      {
        if (clockAt >= 0.f && ClockDistance(clockAt, r.Target) > ClockToleranceSeconds)
        {
          char clockText[16], target[16];
          Hhmmss(clockAt, clockText, sizeof(clockText));
          Hhmmss(r.Target, target, sizeof(target));
          if (r.Retried)
          {
            char why[200];
            sprintf_s(why, "frame %d's lighting clock was at %s, not %s, twice (the game's clock didn't follow the tools')", shown,
              clockText, target);
            return Stop(host, why, true);
          }
          util::log::Warning("Time-lapse: frame %d's lighting clock was at %s, not %s (the game's clock didn't follow the "
            "tools'): its light done again", shown, clockText, target);
          r.Retried = true;
          if (!AskLight(host, now))
            return Stop(host, "its light couldn't be asked for (see CT.log)", true);
          break;
        }
        r.LitAt = litAt;
        r.ClockAt = clockAt;
        r.ResetSeconds = seconds;
        r.LightSeconds = phaseReal;
        Enter(Phase_Reflections, now);
        SetStatus("Frame %d of %d at %s: the reflections", shown, r.P.Frames, at);
        break;
      }
      if (state == timelapse::Light_NotReset)
      {
        // (its own reset counting down, say: once more after a moment)
        if (r.NoResetRetried)
          return Stop(host, "the game's lighting couldn't be reset there, twice (no clock, no jobs, or a reset of its own "
            "counting down)", true);
        if (phaseReal < NoResetWaitSeconds)
          break;
        util::log::Warning("Time-lapse: frame %d: the game's lighting couldn't be reset there: asked again", shown);
        r.NoResetRetried = true;
        if (!AskLight(host, now))
          return Stop(host, "its light couldn't be asked for (see CT.log)", true);
        break;
      }
      if (state == timelapse::Light_Failed)
        return Stop(host, "the game's reset of its lighting wasn't done (Enlighten paused?)", true);
      long long since = host.LightSinceUpdateMs();
      bool stalled = since < 0 || since > LightStallMs;
      if (paused)
        SetStatus("Frame %d of %d at %s: waiting, the game is paused (or loading)", shown, r.P.Frames, at);
      else if (frozen && stalled)
        SetStatus("Frame %d of %d at %s: waiting, time is frozen and the game's lighting doesn't update meanwhile: unfreeze "
          "it (a low game speed holds the world almost still)", shown, r.P.Frames, at);
      else if (stalled)
        SetStatus("Frame %d of %d at %s: the game's lighting hasn't updated for %.1f s", shown, r.P.Frames, at,
          since < 0 ? 0.0 : since / 1000.0);
      else
        SetStatus("Frame %d of %d at %s: the light (%.1f s)", shown, r.P.Frames, at, phaseReal);
      if (stalled && !r.StallLogged && phaseReal * 1000.0 > LightStallMs)
      {
        r.StallLogged = true;
        util::log::Warning("Time-lapse: the game's lighting hasn't updated for %.1f s (frame %d; time %s, the game %s): the frame "
          "waits for it", since < 0 ? -1.0 : since / 1000.0, shown, frozen ? "frozen" : "running", paused ? "paused or loading" :
          "not paused");
      }
      if (phaseRunning > LightMaxSeconds || r.LightReal > LightMaxRealSeconds)
      {
        char why[200];
        if (phaseRunning > LightMaxSeconds)
          sprintf_s(why, "frame %d's light wasn't done in %.0f s of game time", shown, LightMaxSeconds);
        else
          sprintf_s(why, "frame %d's light wasn't done in %.0f min (not counting pauses)", shown, LightMaxRealSeconds / 60.0);
        return Stop(host, why, true);
      }
      break;
    }
    case Phase_Reflections:
    {
      timelapse::Host::ReflectionInfo info{ false, 0, 0, 0 };
      int state = host.ReflectionsState(r.Request, info);
      if (state == timelapse::Reflections_Unknown && !r.ReflUnknownLogged)
      {
        r.ReflUnknownLogged = true;
        util::log::Write("Time-lapse: the reflections can't be watched: each frame waits %.1f s for them", ReflectionsGuessSeconds);
      }
      if (state == timelapse::Reflections_Done || (state == timelapse::Reflections_Unknown && phaseReal >= ReflectionsGuessSeconds))
      {
        r.Refl = info;
        if (state == timelapse::Reflections_Unknown)
          r.Refl.Ended = 3;
        r.ReflSeconds = phaseReal;
        Enter(Phase_Settle, now);
      }
      else
        SetStatus("Frame %d of %d at %s: the reflections (%.1f s)", shown, r.P.Frames, at, phaseReal);
      break;
    }
    case Phase_Settle:
    {
      // (the pause menu or a loading screen would be in the picture; a hidden window has none to take); counted
      // again from when it's back, and no picture within UnpauseSeconds of one (see LastPausedReal)
      if (paused || hidden)
      {
        r.SettleCount = 0;
        SetStatus(hidden ? "Frame %d of %d at %s: waiting, the game's window is minimized" : "Frame %d of %d at %s: waiting, the "
          "game is paused (or loading)", shown, r.P.Frames, at);
        break;
      }
      ++r.SettleCount;
      // Even motion: this many game seconds since the frame before. Not while the world stands still with time
      // frozen and nothing has run since that frame (a time-lapse made with time frozen throughout: it would wait
      // for ever); time that ran before a freeze is waited out once it runs again.
      double since = r.LastShot < 0.0 ? 1e30 : r.Running - r.LastShot;
      bool stillWorld = frozen && since < 0.05;
      if (!stillWorld && since < r.P.S.MinFrame)
      {
        if (frozen)
          SetStatus("Frame %d of %d at %s: %.1f game s more once time runs again (Game time between frames)", shown, r.P.Frames,
            at, r.P.S.MinFrame - since);
        else
          SetStatus("Frame %d of %d at %s: %.1f game s more (Game time between frames)", shown, r.P.Frames, at,
            r.P.S.MinFrame - since);
        break;
      }
      if (r.SettleCount < SettleFrames || phaseReal < SettleSeconds || now - r.LastPausedReal < UnpauseSeconds)
        break;
      std::wstring path = FramePath(r.P, r.Frame);
      // (a carry-on may find this frame from before, half written)
      DeleteFileW(path.c_str());
      host.Shot(path);
      r.ShotAsked = now;
      r.ShotSpoiled = false;
      r.PrevShot = r.LastShot;
      r.Gap = r.LastShot < 0.0 ? -1.0 : r.Running - r.LastShot;
      r.LastShot = r.Running;
      Enter(Phase_Shot, now);
      SetStatus("Frame %d of %d at %s: saving", shown, r.P.Frames, at);
      break;
    }
    case Phase_Shot:
    {
      // (paused or hidden before the picture was taken: it may show that; taken again once it's written)
      if (paused || hidden)
        r.ShotSpoiled = true;
      if (host.ShotBusy())
      {
        // (a minimized window draws no frames: not counted)
        if (hidden)
          r.ShotAsked = now;
        if (now - r.ShotAsked > ShotMaxSeconds)
        {
          char why[96];
          sprintf_s(why, "frame %d's picture wasn't written in %.0f s", shown, ShotMaxSeconds);
          return Stop(host, why, true);
        }
        break;
      }
      std::wstring path = FramePath(r.P, r.Frame);
      if (r.ShotSpoiled)
      {
        DeleteFileW(path.c_str());
        r.LastShot = r.PrevShot;
        r.ShotSpoiled = false;
        Enter(Phase_Settle, now);
        SetStatus("Frame %d of %d at %s: taken again (the game was paused or its window hidden as it was taken)", shown,
          r.P.Frames, at);
        break;
      }
      WIN32_FILE_ATTRIBUTE_DATA file{};
      if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &file) || (file.nFileSizeHigh == 0 && file.nFileSizeLow == 0))
      {
        char why[128];
        sprintf_s(why, "frame %d's picture wasn't saved (see the Frame picture line in CT.log)", shown);
        return Stop(host, why, true);
      }
      char target[16], lit[16], clockText[16], gap[32] = "";
      Hhmmss(r.Target, target, sizeof(target));
      Hhmmss(r.LitAt, lit, sizeof(lit));
      if (r.ClockAt >= 0.f)
        Hhmmss(r.ClockAt, clockText, sizeof(clockText));
      else
        strcpy_s(clockText, "-");
      if (r.Gap >= 0.0)
      {
        sprintf_s(gap, "%.2f", r.Gap);
        if (r.Gap < r.GapMin)
          r.GapMin = r.Gap;
        if (r.Gap > r.GapMax)
          r.GapMax = r.Gap;
      }
      static const char* ended[4] = { "settled", "none near", "15 s", "unknown" };
      char waiting[32];
      sprintf_s(waiting, "%d/%d", r.Refl.Waiting, r.Refl.Count);
      Note(r.P, "%05d.png  %s  %s  %-9s  %7.2f  %13.2f  %-6s  %-10s  %-7s  %s", r.Frame, target, lit, clockText, r.LightSeconds,
        r.ReflSeconds, r.Refl.Ended == 3 ? "-" : (r.Refl.Marked ? "yes" : "no"), ended[r.Refl.Ended >= 0 && r.Refl.Ended < 4 ?
        r.Refl.Ended : 3], r.Refl.Ended == 3 ? "-" : waiting, r.Gap >= 0.0 ? gap : "-");
      util::log::Write("Time-lapse: frame %d of %d at %s (lit at %s): the light %.1f s (its reset %.1f s), the reflections %.1f s%s%s; "
        "saved as %05d.png", shown, r.P.Frames, target, lit, r.LightSeconds, r.ResetSeconds, r.ReflSeconds, r.Gap >= 0.0 ? ", game "
        "time since the frame before " : "", r.Gap >= 0.0 ? (std::string(gap) + " s").c_str() : "", r.Frame);
      ++r.Saved;
      r.FrameSum += now - r.FrameReal;
      r.Retried = false;
      r.NoResetRetried = false;
      r.Reholds = 0;
      if (++r.Frame >= r.P.Frames)
        return Finish(host, now);
      Enter(Phase_Set, now);
      break;
    }
    default:
      break;
    }
  }

  // The time of day as a slider (whole minutes), with steps and the clock's time now
  bool TimeSlider(char const* name, float& value, timelapse::Host& host)
  {
    bool changed = false;
    char label[16];
    Hhmm(value, label, sizeof(label));
    float v = value;
    ImGui::PushID(name);
    if (ImGui::SliderFloat(name, &v, 0.f, 86399.f, label))
    {
      value = WholeMinute(v);
      changed = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("-10 min"))
    {
      value = WholeMinute(value - 600.f);
      changed = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("+10 min"))
    {
      value = WholeMinute(value + 600.f);
      changed = true;
    }
    float clock = host.Clock();
    if (clock >= 0.f)
    {
      ImGui::SameLine();
      if (ImGui::Button("Now"))
      {
        value = WholeMinute(clock);
        changed = true;
      }
    }
    ImGui::PopID();
    return changed;
  }

  void DrawRunning(timelapse::Host& host)
  {
    Run& r = g_Run;
    float fontSize = ImGui::GetFontSize();
    char overlay[48];
    sprintf_s(overlay, "%d / %d", r.Frame, r.P.Frames);
    ImGui::ProgressBar(r.P.Frames > 0 ? (float)r.Frame / r.P.Frames : 0.f, ImVec2(fontSize * 20.f, 0.f), overlay);
    ImGui::PushTextWrapPos(0.f);
    ImGui::TextUnformatted(r.Status);
    double perFrame = r.Saved > 0 ? r.FrameSum / r.Saved : GuessFrameSeconds;
    float scaleNow = host.WorldScale();
    double minReal = scaleNow > 0.f ? r.P.S.MinFrame / scaleNow : r.P.S.MinFrame;
    if (minReal > perFrame)
      perFrame = minReal;
    char left[32];
    Duration(perFrame * (r.P.Frames - r.Frame), left, sizeof(left));
    ImGui::TextDisabled("About %s left (%.1f s a frame%s)", left, perFrame, r.Saved > 0 ? " so far" : ", a guess");
    if (r.GapMax > 0.0)
      ImGui::TextDisabled("Game time between frames so far: %.1f - %.1f s", r.GapMin, r.GapMax);
    ImGui::TextDisabled("Into %s", r.P.FolderText.c_str());
    ImGui::TextDisabled("Leave the camera, the track and the time of day alone until it's done. Stop keeps what's saved; "
      "Continue then carries on from the next frame.");
    ImGui::PopTextWrapPos();
    if (ImGui::Button("Stop time-lapse"))
      g_Command = Command_Stop;
  }
}

void timelapse::Update(Host& host)
{
  double now = host.Now();
  Command command = g_Command;
  g_Command = Command_None;
  if (command == Command_Stop && g_Run.At != Phase_Idle)
    Stop(host, "stopped from the menu", false);
  else if ((command == Command_Start || command == Command_Continue) && g_Run.At == Phase_Idle)
    Start(host, command == Command_Continue, now);
  if (g_Run.At != Phase_Idle)
    Advance(host, now);
}

bool timelapse::Running()
{
  return g_Run.At != Phase_Idle;
}

bool timelapse::DrawUI(Host& host)
{
  bool changed = false;
  ImGui::PushID("Timelapse");
  if (g_Run.At != Phase_Idle)
  {
    DrawRunning(host);
    ImGui::PopID();
    return false;
  }

  int mode = g_S.Mode;
  if (ImGui::Combo("Camera##TimelapseCamera", &mode, "Still (where it is now)\0Along the selected camera track\0") &&
      mode != g_S.Mode && mode >= 0 && mode < Mode_Count)
  {
    g_S.Mode = mode;
    changed = true;
  }
  if (g_S.Mode == Mode_Track)
  {
    int keys = host.TrackKeyframes();
    std::string name = host.TrackName();
    if (keys < 2)
      ImGui::TextDisabled("'%s' needs 2 keyframes or more", name.c_str());
    else
      ImGui::TextDisabled("'%s': %d keyframes, %.2f s, from its start to its end", name.c_str(), keys, host.TrackLength());
    if (!host.CameraOn())
      ImGui::TextDisabled("Turn the camera on: the track plays with it");
    changed |= ImGui::Checkbox("Time of day from the track's keyframes", &g_S.TrackClock);
    ImGui::HelpMarker("Each frame's time of day is the track's at that frame's place on it (the keyframes' Time of day, "
      "eased between them as the track plays them), instead of From - To spread evenly. The keyframe options' Time of "
      "day tick doesn't matter here.");
    float c0 = -1.f, c1 = -1.f;
    if (g_S.TrackClock && keys >= 2 && !(host.TrackTimeOfDay(0.0, c0) && host.TrackTimeOfDay(host.TrackLength(), c1)))
      ImGui::TextDisabled("Its keyframes have no time of day: give them one, or untick this");
  }
  bool range = !(g_S.Mode == Mode_Track && g_S.TrackClock);
  if (range)
  {
    changed |= TimeSlider("From", g_S.From, host);
    changed |= TimeSlider("To", g_S.To, host);
    changed |= ImGui::Checkbox("Clock runs backwards", &g_S.Backwards);
  }

  // What Start would do now: a track playing with Still picked goes along it, with its own time of day when it sets
  // it and has one at every frame (then the pictures go by the video's length), see Start
  bool playing = g_S.Mode == Mode_Still && host.TrackPlaying() && host.TrackKeyframes() >= 2 && host.TrackLength() > 0.0;
  Settings eff = g_S;
  bool ownClock = false;
  if (playing)
  {
    eff.Mode = Mode_Track;
    eff.TrackClock = false;
    if (host.TrackSetsTime())
    {
      Plan trial;
      trial.S = eff;
      trial.S.TrackClock = true;
      trial.Frames = FramesOf(trial.S);
      trial.TrackLength = host.TrackLength();
      ownClock = TrackClockAtEveryFrame(host, trial, 0);
      eff.TrackClock = ownClock;
    }
  }

  // How many pictures: by the video's length, or one every so long of the clock (only with a clock From - To)
  bool intervalOk = range && !ownClock;
  if (intervalOk)
  {
    int count = g_S.Count;
    if (ImGui::Combo("Pictures", &count, "By the video's length\0One every so long of the clock\0") && count != g_S.Count &&
        (count == Count_Video || count == Count_Every))
    {
      g_S.Count = count;
      changed = true;
    }
    ImGui::HelpMarker("By the video's length: the pictures are spread evenly from From to To, as many as the video needs.\n\n"
      "One every so long of the clock: like a time-lapse camera's interval, a picture every so many game seconds or minutes "
      "of the time of day from From until To; the video is as long as those pictures make it.");
  }
  if (intervalOk && g_S.Count == Count_Every)
  {
    // The interval in the unit picked (game seconds or minutes), kept in game seconds
    int unit = g_S.EveryUnit == 0 ? 0 : 1;
    float scale = unit == 0 ? 1.f : 60.f;
    float shown = g_S.Every / scale;
    float low = (unit == 0 ? EveryMin : 0.5f * 60.f) / scale, high = (unit == 0 ? 600.f : EveryMax) / scale;
    if (ImGui::SliderFloat("One picture every", &shown, low, high, unit == 0 ? "%.0f game s" : "%.1f game min", 2.f))
    {
      float every = RoundEvery(shown * scale);
      if (every != g_S.Every)
      {
        g_S.Every = every;
        changed = true;
      }
    }
    ImGui::SameLine();
    ImGui::PushID("EveryUnit");
    if (ImGui::Button(unit == 0 ? "in seconds" : "in minutes"))
    {
      // (only how it's shown: the value stays, even outside the other slider's range)
      g_S.EveryUnit = unit == 0 ? 1 : 0;
      changed = true;
    }
    ImGui::PopID();
    ImGui::ItemTooltip("Switches the slider between game seconds and game minutes");
  }
  else
  {
    float seconds = g_S.Seconds;
    if (ImGui::InputFloat("Video length (s)", &seconds, 1.f, 10.f, 1))
    {
      if (!(seconds >= SecondsMin))
        seconds = SecondsMin;
      if (seconds > SecondsMax)
        seconds = SecondsMax;
      if (seconds != g_S.Seconds)
      {
        g_S.Seconds = seconds;
        changed = true;
      }
    }
  }
  int rate = g_S.Rate;
  if (ImGui::Combo("Frame rate", &rate, "24 fps\0" "25 fps\0" "30 fps\0" "50 fps\0" "60 fps\0") && rate != g_S.Rate && rate >= 0 &&
      rate < FrameRateCount)
  {
    g_S.Rate = rate;
    changed = true;
  }
  // (as Start will make them: eff)
  int frames = FramesOf(eff);
  bool effRange = !(eff.Mode == Mode_Track && eff.TrackClock);
  ImGui::PushTextWrapPos(0.f);
  if (ByInterval(eff))
  {
    char note[160] = "";
    if (IntervalCut(eff))
      strcpy_s(note, "; cut at 20,000 pictures: it ends early");
    else if (IntervalLonger(eff))
      strcpy_s(note, "; the interval is longer than From - To: From and To only");
    else if (WholeDay(eff) && DayGapAtEnd(eff) > eff.Every + 0.5)
      sprintf_s(note, " (a whole day: %.0f game s from the last back to From, not %.0f: it won't loop evenly)", DayGapAtEnd(eff),
        (double)eff.Every);
    else if (WholeDay(eff))
      strcpy_s(note, " (a whole day)");
    ImGui::TextDisabled("%d pictures (%.1f a game hour), a %.1f s video at %d fps%s", frames, 3600.0 / eff.Every,
      (double)frames / FrameRates[eff.Rate], FrameRates[eff.Rate], note);
  }
  else if (effRange)
  {
    double step = Step(eff, frames);
    if (step >= 120.0)
      ImGui::TextDisabled("%d frames, one every %.1f game minutes%s", frames, step / 60.0, WholeDay(eff) ? " (a whole day)" : "");
    else
      ImGui::TextDisabled("%d frames, one every %.1f game seconds%s", frames, step, WholeDay(eff) ? " (a whole day)" : "");
  }
  else
    ImGui::TextDisabled("%d frames, the time of day as the track's keyframes have it", frames);
  ImGui::PopTextWrapPos();

  float minFrame = g_S.MinFrame;
  if (ImGui::SliderFloat("Game time between frames", &minFrame, 0.f, MinFrameMax, "at least %.1f s"))
  {
    minFrame = minFrame < 0.f ? 0.f : (minFrame > MinFrameMax ? MinFrameMax : minFrame);
    if (minFrame != g_S.MinFrame)
    {
      g_S.MinFrame = minFrame;
      changed = true;
    }
  }
  ImGui::HelpMarker("People, cars and anything else that moves (clouds too, if they go by game time) carry on at the game "
    "speed while each frame's light is worked out, so from one frame to the next they move by however much game time "
    "that took. Set this above the most a frame takes (the menu and CT.log say) and every frame is that much game time "
    "after the one before: even motion. Game time is real time x the game speed (GAME > Game speed), and at most one "
    "30th of a second a drawn frame, as the game itself runs. 0: each frame as soon as its light is done.");

  char estimate[32];
  double guess = GuessFrameSeconds + (eff.Mode == Mode_Track ? TrackMoveSeconds : 0.0);
  // (the game time between frames in real seconds at the game speed now)
  float scaleNow = host.WorldScale();
  double minReal = scaleNow > 0.f ? g_S.MinFrame / scaleNow : g_S.MinFrame;
  double perFrame = guess > minReal ? guess : minReal;
  Duration(perFrame * frames, estimate, sizeof(estimate));
  ImGui::PushTextWrapPos(0.f);
  ImGui::TextDisabled("About %s to make (about %.0f s a frame on a normal level, a guess; much longer with the whole city "
    "loaded)", estimate, perFrame);

  char why[256] = "";
  bool usable = host.LightUsable(why, sizeof(why));
  if (!usable)
    ImGui::TextDisabled("Can't start: %s", why);
  if (host.TimeFrozen())
    ImGui::TextDisabled("Time is frozen: the game's lighting most likely doesn't update until it runs again (a low game "
      "speed holds the world almost still)");
  if (g_S.Mode == Mode_Track && host.CameraShakes())
    ImGui::TextDisabled("Camera shake may be on (the menu's, or the track's keyframes'): each frame would get another shake "
      "(Still holds the camera without it)");
  if (host.HudShown())
    ImGui::TextDisabled("The game's HUD is switched on (GAME > HUD): whatever of it is on screen will be in the pictures");
  if (playing)
    ImGui::TextDisabled("A camera track is playing: Start makes the time-lapse along it ('%s', its start to its end) with %s. "
      "Stop the track first for a still one.", host.TrackName().c_str(), ownClock ? "its own time of day (it sets it and has "
      "one at every frame; the pictures go by the video's length)" : (host.TrackSetsTime() ? "the time of day From - To (it "
      "sets the time of day, but not at every frame)" : "the time of day From - To (it doesn't set the time of day)"));
  ImGui::TextDisabled("Keep the game's window up (not minimized) while it runs: a minimized window draws no pictures");
  ImGui::PopTextWrapPos();

  if (ImGui::Button("Start time-lapse"))
    g_Command = Command_Start;
  if (g_CanContinue)
  {
    ImGui::SameLine();
    if (ImGui::Button("Continue"))
      g_Command = Command_Continue;
    ImGui::SameLine();
    ImGui::TextDisabled("from frame %d of %d (its own settings)", g_ContinueFrame + 1, g_ContinuePlan.Frames);
  }
  if (!g_Message.empty())
  {
    ImGui::PushTextWrapPos(0.f);
    ImGui::TextUnformatted(g_Message.c_str());
    ImGui::PopTextWrapPos();
  }
  if (g_Ffmpeg[0])
  {
    ImGui::InputText("##Ffmpeg", g_Ffmpeg, sizeof(g_Ffmpeg), ImGuiInputTextFlags_ReadOnly);
    ImGui::SameLine();
    if (ImGui::Button("Copy"))
      ImGui::SetClipboardText(g_Ffmpeg);
  }
  ImGui::PopID();
  return changed;
}

void timelapse::ReadConfig(INIReader* pReader)
{
  Settings s;
  long mode = pReader->GetInteger("Camera", "TimelapseCamera", s.Mode);
  s.Mode = mode == Mode_Track ? Mode_Track : Mode_Still;
  s.From = WholeMinute((float)pReader->GetReal("Camera", "TimelapseFrom", s.From));
  s.To = WholeMinute((float)pReader->GetReal("Camera", "TimelapseTo", s.To));
  s.Backwards = pReader->GetBoolean("Camera", "TimelapseBackwards", s.Backwards);
  s.TrackClock = pReader->GetBoolean("Camera", "TimelapseTrackClock", s.TrackClock);
  float seconds = (float)pReader->GetReal("Camera", "TimelapseSeconds", s.Seconds);
  s.Seconds = !(seconds >= SecondsMin) ? SecondsMin : (seconds > SecondsMax ? SecondsMax : seconds);
  long fps = pReader->GetInteger("Camera", "TimelapseFps", FrameRates[DefaultRate]);
  s.Rate = DefaultRate;
  for (int i = 0; i < FrameRateCount; ++i)
    if (FrameRates[i] == fps)
      s.Rate = i;
  s.Count = pReader->GetInteger("Camera", "TimelapsePictures", s.Count) == Count_Every ? Count_Every : Count_Video;
  s.Every = RoundEvery((float)pReader->GetReal("Camera", "TimelapseEvery", s.Every));
  s.EveryUnit = pReader->GetInteger("Camera", "TimelapseEveryUnit", s.EveryUnit) == 0 ? 0 : 1;
  float minFrame = (float)pReader->GetReal("Camera", "TimelapseMinFrame", s.MinFrame);
  s.MinFrame = !(minFrame >= 0.f) ? 0.f : (minFrame > MinFrameMax ? MinFrameMax : minFrame);
  g_S = s;
}

void timelapse::WriteConfig(std::string& config)
{
  // CAMERA > Time-lapse: 0 still, 1 along the selected track; times of day in game seconds
  config += "TimelapseCamera = " + std::to_string(g_S.Mode) + "\n";
  config += "TimelapseFrom = " + std::to_string(g_S.From) + "\n";
  config += "TimelapseTo = " + std::to_string(g_S.To) + "\n";
  config += std::string("TimelapseBackwards = ") + (g_S.Backwards ? "true" : "false") + "\n";
  config += std::string("TimelapseTrackClock = ") + (g_S.TrackClock ? "true" : "false") + "\n";
  config += "TimelapseSeconds = " + std::to_string(g_S.Seconds) + "\n";
  config += "TimelapseFps = " + std::to_string(FrameRates[g_S.Rate]) + "\n";
  config += "TimelapseMinFrame = " + std::to_string(g_S.MinFrame) + "\n";
  // 0 by the video's length, 1 one every TimelapseEvery game seconds of the clock (shown in 0 seconds, 1 minutes)
  config += "TimelapsePictures = " + std::to_string(g_S.Count) + "\n";
  config += "TimelapseEvery = " + std::to_string(g_S.Every) + "\n";
  config += "TimelapseEveryUnit = " + std::to_string(g_S.EveryUnit) + "\n";
}
