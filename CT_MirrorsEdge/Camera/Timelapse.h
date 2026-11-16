#pragma once
#include <cstddef>
#include <string>

class INIReader;

// CAMERA > Time-lapse (the user, 2026-11-14: "do more research on how we can create timelapses and implement it";
// Research_Timelapse_2026-11-14.md in the project root).
//
// A time-lapse rendered a frame at a time, for exact light. For each frame:
//  - the time of day is held at that frame's time (From to To spread over the frames, or the camera track's own
//    time of day keyframes) and, along a camera track (a hyperlapse), the track is held at that frame's place on it;
//  - the game's own reset of its time of day lighting is done there (forced: every frame gets the same 5 passes and
//    two more at its own time, see util::hooks::RequestTodReset), with the game's own lighting steps held so the
//    light shown stays that reset's;
//  - once that's done, every reflection is captured again with it and the nearest round of them is waited for;
//  - then the finished frame (the game's picture, before the menu) is saved as a PNG into
//    Cinematic Tools\Captures\Time-lapse <date time>\, with Time-lapse.txt beside it (the settings, a line a frame
//    and the ffmpeg line that makes the video).
// Every frame gets the same light work at its own time, so the light doesn't step or flicker from frame to frame.
// The world (people, cars, clouds) runs at the game speed while each frame's light is worked out: the game's
// lighting update counts 30 Hz game ticks (read in its code, 0x14394DB80), so it most likely stands still while
// time is frozen (not seen yet: a frame then waits, and says so in the menu and CT.log).
namespace timelapse
{
  // What the time-lapse uses of the rest of the tools: CameraManager's link (CameraManager.cpp), or the offline
  // test's fake. Update and DrawUI are only called under CameraManager's lock, which covers all of this.
  struct Host
  {
    virtual ~Host() {}
    virtual double Now() = 0;                      // real seconds
    virtual bool CameraOn() = 0;                   // the CAMERA tab's free camera
    virtual bool CameraShakes() = 0;               // its camera shake on (each frame would get another shake)
    virtual bool TimeFrozen() = 0;                 // GAME > Freeze time on (no game time runs)
    virtual bool GamePaused() = 0;                 // the game's pause menu or loading screen (no game time either)
    virtual float WorldScale() = 0;                // game seconds a real second (the game speed, its slow motion; 0 frozen)
    virtual bool WindowHidden() = 0;               // the game's window minimized or hidden (no picture to take)
    virtual bool HudShown() = 0;                   // the game's HUD is drawn (it would be in the pictures)
    virtual long long LevelKey() = 0;              // which level's clock it is (another level: another key; 0 none)
    virtual float Clock() = 0;                     // the time of day (game seconds 0 - 86400), -1 without one
    virtual bool LightUsable(char* why, size_t size) = 0;   // false: why it can't be worked out now
    virtual bool StepsHoldable() = 0;              // the game's own lighting steps can be held (their code known)
    virtual long long LightSinceUpdateMs() = 0;    // real ms since the game's lighting last updated, -1: never
    // The camera's place (x, y, z), rotation (a quaternion) and field of view; the camera held there (Still) or
    // let go
    virtual void CameraPose(float pose[8]) = 0;
    virtual void HoldCamera(bool hold, float const* pose) = 0;
    // The selected camera track
    virtual int TrackKeyframes() = 0;
    virtual double TrackLength() = 0;              // seconds of track time
    virtual std::string TrackName() = 0;
    virtual bool TrackTimeOfDay(double time, float& clock) = 0;   // its keyframes' time of day there
    virtual void TrackHold(double time) = 0;       // played, paused at time (it sets the camera)
    virtual bool TrackHeld(double time) = 0;       // playing, paused, at time
    virtual bool TrackPlaying() = 0;               // playing (or paused)
    virtual bool TrackSetsTime() = 0;              // it sets the time of day as it plays (its keyframes' Time of day option)
    virtual void TrackRelease() = 0;               // stopped
    // The clock held at clock and the game's reset of its lighting done there: the request (0: couldn't); what
    // became of it (a util::hooks::TodOutcome: litAt the time lit, clockAt the lighting's own clock then, seconds
    // how long the reset took); its reflections (ReflectionStates, info: how their round went); all of it let go
    // (the clock goes on from where it is)
    struct ReflectionInfo
    {
      bool Marked;      // all seen marked to be captured again
      int Waiting;      // still waiting when taken as captured (the ones further away), of Count
      int Count;
      int Ended;        // 0 the count stopped going down, 1 none went down in 3 s, 2 15 s went by, 3 can't be told
    };
    virtual long LightAt(float clock, bool quiet) = 0;
    virtual int LightState(long id, float& litAt, float& clockAt, double& seconds) = 0;
    virtual int ReflectionsState(long id, ReflectionInfo& info) = 0;
    virtual void LightEnd() = 0;
    // The finished frame saved as a PNG at the next present; busy until it's written (or failed); a picture not
    // taken yet dropped
    virtual void Shot(std::wstring const& path) = 0;
    virtual bool ShotBusy() = 0;
    virtual void ShotCancel() = 0;
  };
  // LightState's (util::hooks::TodOutcome's values)
  enum LightStates { Light_Working, Light_Done, Light_NotReset, Light_Failed };
  enum ReflectionStates { Reflections_Working, Reflections_Done, Reflections_Unknown };

  // The camera hook, every rendered frame, camera on or off (the game thread)
  void Update(Host& host);
  // CAMERA tab, under its "Time-lapse" heading (the drawing thread); true when a setting changed (for the config)
  bool DrawUI(Host& host);
  // A time-lapse is being made
  bool Running();

  // [Camera] Timelapse*
  void ReadConfig(INIReader* pReader);
  void WriteConfig(std::string& config);
}
