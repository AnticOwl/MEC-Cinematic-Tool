#pragma once
#include <mutex>
#include <string>
#include <vector>
#include <Windows.h>

// Overrides for the game's visual environment (color correction, depth of
// field, film grain, outdoor light, tonemap, vignette), like the original
// tools' "Visual Environment" window. Field offsets come from the original
// tools' GUI.
class VisualsManager
{
public:
  enum class FieldType { Float, Int, Bool, Float2, Float3, Color3 };

  struct Field
  {
    const char* Name;
    int Offset;
    FieldType Type;
    float Speed; // Drag speed for float widgets
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

private:
  __int64 GetStateManager();
  bool IsLevelLoaded();

  void ReadFromGame(Component& component);
  void WriteToGame(__int64 pState, Component const& component);
  void CheckStillApplied(__int64 pState, Component& component);
  void DrawField(Component& component, Field const& field);

  void SetSpriteDofFix(bool enable);

  float* GetTimeOfDay();
  void UpdateTimeOfDay();

private:
  std::mutex m_Mutex;
  std::vector<Component> m_Components;

  // Not part of the DoF component itself, the original wrote it to the
  // state manager's render settings
  bool m_DofMethodHasValue;
  int m_DofMethod;
  int m_DofMethodOriginal{ 0 };
  bool m_DofMethodApplied{ false };

  bool m_SpriteDofFix;
  BYTE m_SpriteDofOriginalBytes[2];

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

  __int64 m_ppMain;
  __int64 m_ppClientGameContext;
  __int64 m_pSpriteDofFix;
  __int64 m_ppTimeOfDay;

public:
  VisualsManager(VisualsManager const&) = delete;
  void operator=(VisualsManager const&) = delete;
};
