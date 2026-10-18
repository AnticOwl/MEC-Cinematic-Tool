#pragma once
#include <atomic>
#include <mutex>
#include <vector>
#include <d3d11.h>
#include <wrl.h>

using namespace Microsoft::WRL;

enum SelectedMenu
{
  UIMenu_Camera,
  UIMenu_Follow,
  UIMenu_Visuals,
  UIMenu_Lights,
  UIMenu_Misc
};

class UI
{
public:
  UI();
  ~UI();

  // Called on the render thread once the game's Dx11 device is known
  bool Initialize();

  void Draw();
  void Update(double dt);

  bool IsEnabled() { return m_Enabled; }
  bool HasMouseFocus() { return m_Enabled && m_HasMouseFocus; }
  bool HasKeyboardFocus() { return m_Enabled && m_HasKeyboardFocus; }

  void Toggle();
  void SetEnabled(bool enabled);

  // 0 = small ... 3 = extra large, saved in the config
  int GetTextSize() { return m_TextSize; }
  void SetTextSize(int size);

  // The menu shows the Windows cursor, or (the old way) draws its own
  bool UsesWindowsCursor() { return !m_DrawOwnCursor; }
  void SetDrawOwnCursor(bool draw) { m_DrawOwnCursor = draw; }

  // Mouse wheel notches for scrolling the menu, added to its next frame.
  // From the window's wheel messages, and from the DirectInput wheel (camera
  // hook) for as long as the game sends no wheel messages.
  void AddWheelFromMessage(float notches);
  void AddWheelFromDirectInput(float notches);
  // The low-level mouse hook, the one that works in this game
  void AddWheelFromHook(float notches);
  void OnWheelHookReady();
  // Typed characters (WM_CHAR), handed to ImGui at the start of its next frame
  void AddCharFromMessage(unsigned int character);

private:
  void BuildFonts();
  void ApplyStyle(float scale);
  float GetResolutionScale();
  bool TabButton(const char* label, SelectedMenu menu, float width);
  void DrawMiscMenu();
  void AddWheel(float notches);

private:
  bool m_Initialized;
  bool m_Enabled;
  SelectedMenu m_SelectedMenu;

  bool m_HasMouseFocus;
  bool m_HasKeyboardFocus;

  int m_TextSize{ 1 };
  bool m_DrawOwnCursor{ false };
  bool m_FontsDirty{ false };
  float m_FontScale{ 1.f }; // Resolution scale the current fonts were made for

  std::atomic<float> m_PendingWheel{ 0.f };
  std::atomic<bool> m_SawWheelMessage{ false };
  std::atomic<bool> m_UsedDirectInputWheel{ false };
  std::atomic<bool> m_WheelHookReady{ false };
  std::atomic<bool> m_UsedWheelHook{ false };

  std::mutex m_CharMutex;
  std::vector<unsigned int> m_PendingChars;

public:
  UI(UI const&) = delete;
  void operator=(UI const&) = delete;
};
