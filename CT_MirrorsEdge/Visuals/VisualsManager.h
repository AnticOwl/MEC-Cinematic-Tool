#pragma once
#include "GreenPlane.h"
#include <atomic>
#include <mutex>
#include <string>
#include <vector>
#include <Windows.h>

// Overrides for the game's visual environment (color correction, depth of
// field, film grain, outdoor light, tonemap, vignette, fog), like the original
// tools' "Visual Environment" window. Field offsets come from the original
// tools' GUI, the fog's from the game's FogComponentData class info.
class VisualsManager
{
public:
  // Float3Linked: three floats (red, green, blue) edited as one value
  enum class FieldType { Float, Int, Bool, Float2, Float3, Float4, Color3, Float3Linked };

  struct Field
  {
    const char* Name;
    int Offset;
    FieldType Type;
    float Speed; // Drag speed for float widgets
    // Int fields that are one of the game's enums: its names, in value order,
    // separated by zeros (an ImGui combo), so nothing else can be typed
    const char* Items{ nullptr };
  };

  struct Component
  {
    const char* Name;
    int StateOffset; // Offset inside the VisualEnvironment state
    int Size;
    std::vector<Field> Fields;
    int BasicFieldCount{ 0 }; // Fields shown first, the rest go under "More settings". 0 = all

    bool Override{ false };
    bool HasValues{ false };
    std::vector<BYTE> Values;

    // Self-check, see CheckStillApplied
    ULONGLONG EnabledTick{ 0 };
    ULONGLONG OverwrittenTick{ 0 };
    const char* OverwrittenField{ nullptr };

    // Reset to game values while changed: the change is paused this long
    // ago so the game puts its own values back first
    ULONGLONG ResetTick{ 0 };
  };

  VisualsManager();
  ~VisualsManager();

  // Game thread, from the visual environment update hook, before and after
  // the game's own update
  void OnVisualEnvironmentUpdate(bool beforeGameUpdate);
  // Render thread, from the Present hook
  void DrawUI();
  void DrawTimeOfDayUI();

  // For camera tracks (camera hook and menu). Seconds 0 - 86400, the getter
  // gives -1 when there's no time of day (no level loaded).
  float GetCurrentTimeOfDay();
  void SetTimeOfDay(float seconds);
  // While a track plays its time of day wins over the frozen clock and the fix
  void SetTrackTimeOfDay(float seconds);
  void StopTrackTimeOfDay();
  // While a replay plays (REPLAY > Time of day as recorded): its recorded
  // time wins over the stopped clock and the fix, a camera track's over it;
  // when it stops the clock gets back the time it had before
  void SetReplayTimeOfDay(float seconds);
  void StopReplayTimeOfDay();
  // The game's weather as it shows it (the blended visual environment's
  // Clouds and Wind blocks, see the components): read for a replay's
  // recording (false: no level), and while it plays given between two
  // recorded ones (t 0 - 1 from a to b; a null: stop). A Clouds or Wind change
  // made in the menu wins over it.
  size_t WeatherLookSize();
  bool ReadWeatherLook(BYTE* out);
  void SetReplayWeatherLook(BYTE const* a, BYTE const* b, float t);
  // Camera hook, every rendered frame: the stopped clock (or a track's time)
  // is written then too, so the clock can't creep on between visual updates
  void KeepClock();
  // CAMERA > Time-lapse (Camera/Timelapse.cpp), camera hook. LapseLightAt: the
  // time of day held at seconds (over a track's, a replay's and the stopped
  // clock) and the game's own reset of its lighting asked for there, forced
  // (see util::hooks::RequestTodReset; quiet: without its log lines); while
  // the time-lapse runs the catch-up stays on (the bigger budget, the
  // exposure at once, reflections a whole cubemap a frame), the game's own
  // steps are held, the following and the jump resets are off, and once the
  // reset is done every reflection is captured again. Returns the request (0:
  // can't, see LapseAvailable). LapseLightState: a util::hooks::TodOutcome
  // (litAt, seconds: the time lit, how long its reset took).
  // LapseReflectionsState: 0 being captured, 1 the nearest ones captured (the
  // count waiting has stopped going down), 2 can't be told (their code isn't
  // the tools'). EndLapse lets go: the clock goes on from the last time (or
  // stays there, stopped), the steps, following and resets come back.
  // (clockAt: the lighting's own clock when its reset was done. info: how the
  // reflections' round went, see LapseReflInfo)
  struct LapseReflInfo
  {
    bool Marked;     // all were seen marked to be captured again
    int Waiting;     // still waiting when taken as captured (the ones further away), of Count
    int Count;
    int Ended;       // 0 the count stopped going down, 1 none went down in 3 s, 2 15 s went by, 3 can't be told
  };
  bool LapseAvailable(char* why, size_t size);
  // Which clock the time of day is (its address: another level, another one;
  // 0 without one)
  __int64 ClockKey();
  long LapseLightAt(float seconds, bool quiet);
  int LapseLightState(long id, float& litAt, float& clockAt, double& seconds);
  int LapseReflectionsState(long id, LapseReflInfo* info);
  void EndLapse();
  // CAMERA > Around the focus > Auto-focus, from the camera hook every frame: the
  // depth of field override's distances (focus, Simple and Sprite near and
  // far) moved so the set-up made for its own focus distance is at this one
  // (metres); off, they go back as they were set
  void SetAutoFocus(bool on, float distance);
  // Whether the depth of field is changed here (auto-focus needs it)
  bool IsDofOverridden();

private:
  __int64 GetStateManager();
  bool IsLevelLoaded();

  void ReadFromGame(Component& component);
  void WriteToGame(__int64 pState, Component const& component);
  void CheckStillApplied(__int64 pState, Component& component);
  void DrawField(Component& component, Field const& field);
  // VISUALS > Depth of field's own menu (see its comment)
  void DrawDofUI(Component& dof);
  // Its "Set up around this focus distance" (both methods' fields)
  void SetUpDofAround(Component& dof, float focus);
  // Its test pictures (see StartDofTest): the step running (0: none), since
  // when, whether its picture is asked for, and the set-up to put back
  struct DofTest
  {
    int Step{ 0 };
    ULONGLONG StepTick{ 0 };
    bool ShotAsked{ false };
    bool Override{ false }, HasValues{ false };
    std::vector<BYTE> Values;
    int Method{ 0 };
    float Focus{ 5.f };
    wchar_t Stamp[16]{};
  };
  DofTest m_DofTest;
  ULONGLONG m_DofTestFileTick{ 0 };
  void StartDofTest();
  void RunDofTest();
  void DofTestStep();

  // Outdoor light's derived sun values worked out again on the view state
  // after the change is written (see RecomputeSun)
  size_t m_OutdoorIndex{ (size_t)-1 };
  bool m_SunRecompute{ true };
  bool m_SunRecomputeLogged{ false };
  void RecomputeSun(__int64 pState);
  // The sky's own sun scaled by the menu's sun (see ScaleSkySun): the sun's
  // color and intensity when the effect was copied from the game, and the
  // sky's sun under the change and as written last (color, intensity)
  float m_SunRefColor[3]{ 1.f, 1.f, 1.f };
  float m_SunRefIntensity{ 1.f };
  bool m_SkySunApplied{ false };
  float m_SkySunBase[4]{};
  float m_SkySunWritten[4]{};
  void ScaleSkySun(BYTE* s);

  void SetSpriteDofFix(bool enable);
  // Under m_Mutex, before and after the game's update: VISUALS > Weather's
  // lightning flash on top of what's there (see weather::LightningFlash; the
  // rainy city look is given where the game mixes, weather::AfterViewMix)
  void ApplyWeatherLook(bool beforeGameUpdate);
  // Under m_Mutex, before the components are written (see SetAutoFocus)
  void ApplyAutoFocus(Component& dof);

  // VISUALS > Green screen (see greenplane)
  void DrawGreenPlane();

  float* GetTimeOfDay();
  void UpdateTimeOfDay();
  // Game thread, after the game's visual environment update: when the time of
  // day jumps, the lighting gets the game's own reset at the new time and a
  // bigger budget for a moment
  void CatchUpBounceLight();
  void PutBackBounceLight();
  // The same, for the exposure: it takes its target at once while want
  void CatchUpExposure(bool want);
  void PutBackExposure();
  // More bounce light passes after a jump (see CatchUpPasses), one at a time;
  // a finished one is released whether or not more may be filed
  void RunBounceLightPasses(ULONGLONG now, bool mayFile);

private:
  std::mutex m_Mutex;
  std::vector<Component> m_Components;

  // Not part of the DoF component itself, the original wrote it to the
  // state manager's render settings
  bool m_DofMethodHasValue;
  int m_DofMethod;
  int m_DofMethodOriginal{ 0 };
  BYTE m_SpriteDofEnableOriginal{ 0 };   // the game's own SpriteDofEnable, put back with the method
  bool m_DofMethodApplied{ false };

  bool m_SpriteDofFix;
  BYTE m_SpriteDofOriginalBytes[2];
  // The sprite depth of field's crash guard (see InstallSpriteDofGuard): its
  // code, and the flag in it that does what the Sprite DoF fix's patch did
  BYTE* m_pSpriteDofGuard{ nullptr };
  void InstallSpriteDofGuard();

  // VISUALS > Filmic effects' lens distortion is drawn by the tools (see
  // lensdistortion): while it is, the game's own is written off
  size_t m_FilmicIndex{ (size_t)-1 };
  bool m_LensRedirected{ false };
  void ApplyLensDistortion(__int64 pState);

  // Auto-focus (see SetAutoFocus): the distances as set and as written last
  // (see AutoFocusOffsets), and the scale they were written at
  bool m_AutoFocusWanted{ false };
  float m_AutoFocusDistance{ 0.f };
  bool m_AutoFocusApplied{ false };
  float m_AutoFocusBase[9]{};
  float m_AutoFocusWritten[9]{};
  float m_AutoFocusScale{ 1.f };

  size_t m_FogIndex{ 0 };   // the Fog effect's place in m_Components

  // The lightning's fields (see ApplyWeatherLook): each one's value under it
  // (the game's, or a change made here), and what was written last
  static const int WeatherLookFloats = 8;
  bool m_WeatherLookApplied{ false };
  float m_WeatherLookBase[WeatherLookFloats]{};
  float m_WeatherLookWritten[WeatherLookFloats]{};
  bool m_LightningApplied{ false };   // for the log
  int m_LightningWrites{ 0 };         // this flash's
  // The green screen's settings as set here (see greenplane)
  greenplane::Settings m_Plane{ false, 5.f, 10.f, 6.f, 0.f, 0.f, { 0.f, 1.f, 0.f }, false, false };

  // Time of day, seconds 0 - 86400
  bool m_TodFreeze{ false };
  float m_TodFrozenValue{ 0 };
  // The game's time of day can get stuck at one value (65700 = 18:15 in MEC-TC's
  // default), skipping a few seconds ahead gets it going again
  bool m_TodFix{ true };
  int m_TodFixAt{ 65700 };
  int m_TodFixSkip{ 15 };
  // Set by a playing camera track
  bool m_TrackTodActive{ false };
  float m_TrackTod{ 0 };
  // Set by a playing replay, and the clock's time when it started
  bool m_ReplayTodActive{ false };
  float m_ReplayTod{ 0 };
  float m_ReplayTodLive{ -1.f };
  // A time-lapse running (see LapseLightAt): its clock (over everything else),
  // its request, and its reflections (see LapseReflections): the request
  // they were captured again for, their state, since when, whether all were
  // marked to be captured, the count waiting as last seen and how long it
  // hasn't changed
  __int64 m_TodClockKey{ 0 };   // the level's clock last seen (CatchUpBounceLight: another one, the managers forgotten)
  bool m_LapseOn{ false };
  bool m_LapseTodActive{ false };
  float m_LapseTod{ 0.f };
  long m_LapseId{ 0 };
  long m_LapseReflFor{ 0 };
  int m_LapseRefl{ 0 };
  ULONGLONG m_LapseReflTick{ 0 };
  bool m_LapseMarked{ false };
  int m_LapseMarkedAt{ 0 };
  int m_LapseWaiting{ -1 };
  ULONGLONG m_LapseStableSince{ 0 };
  int m_LapseStableUpdates{ 0 };
  int m_LapseReflLogs{ 0 };
  int m_LapseReflWarnings{ 0 };
  int m_LapseQueueCount{ 0 };   // the run's requests so far (its job queue lines, see LapseLightAt)
  LapseReflInfo m_LapseReflInfo{ false, 0, 0, 0 };
  void LapseReflections(ULONGLONG now);
  // A playing replay's weather (see SetReplayWeatherLook): Clouds then Wind
  size_t m_CloudsIndex{ (size_t)-1 }, m_WindIndex{ (size_t)-1 };
  std::vector<BYTE> m_ReplayLookA, m_ReplayLookB;
  float m_ReplayLookT{ 0.f };
  bool m_ReplayLookActive{ false };
  void WriteReplayLook(__int64 pState);
  // The bounce light catching up after the time of day jumps (see
  // CatchUpBounceLight): Enlighten's budget as the game had it, and which
  // settings object has the bigger one now (0 for none)
  struct EnlightenBudget
  {
    float SolveTime, SolveTimeLimited;
    int Jobs, JobsLimited, ProbeSources, ProbeInstances;
  };
  bool m_BounceCatchUp{ true };
  float m_LastClock{ -1.f };
  ULONGLONG m_LastClockTick{ 0 };
  // How far the clock moved in the current jump window (see ClockJumpSeconds)
  ULONGLONG m_JumpWindowTick{ 0 };
  float m_JumpWindowClock{ 0.f };
  float m_JumpWindowSteps{ 0.f };
  ULONGLONG m_CatchUpUntil{ 0 };
  __int64 m_pEnlightenBoosted{ 0 };
  EnlightenBudget m_EnlightenOriginal{};
  // Our adaptation times are in the tonemap; the game's dark / light ones
  bool m_ExposureCaughtUp{ false };
  float m_GameAdaptation[2]{};
  bool m_ReflectionsCaughtUp{ false };  // reflection captures a whole cubemap a frame
  // The lighting's reset after a jump (see ResetSettleMs): one more is asked
  // for when jumps stop; the reflections captured again until (see
  // RecaptureMs)
  bool m_ResetAfterJumps{ false };
  ULONGLONG m_ReflectionsUntil{ 0 };
  // What marks every reflection again during the catch-up (see
  // NoteReflectionMarks): the last look's waiting count and stored sun, and
  // how many were logged this time
  void NoteReflectionMarks(ULONGLONG now);
  bool m_MarksWatching{ false };
  int m_MarksWaiting{ 0 };
  float m_MarksSun[3]{};
  int m_MarksLogged{ 0 };
  // The extra bounce light passes: still to file, done, the one filed (its
  // request id and the manager it's in), when the last jump was
  ULONGLONG m_LastJumpTick{ 0 };
  int m_PassesLeft{ 0 };
  int m_PassesDone{ 0 };
  bool m_PassFiled{ false };
  unsigned int m_PassId{ 0 };
  __int64 m_PassManager{ 0 };
  ULONGLONG m_PassesStart{ 0 };

  __int64 m_ppMain;
  __int64 m_ppClientGameContext;
  __int64 m_pSpriteDofFix;
  __int64 m_ppTimeOfDay;

public:
  VisualsManager(VisualsManager const&) = delete;
  void operator=(VisualsManager const&) = delete;
};
