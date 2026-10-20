#pragma once
#include "ActionDefs.h"
#include "../inih/cpp/INIReader.h"

#include <array>
#include <atomic>
#include <chrono>
#include <functional>
#include <string>
#ifndef DIRECTINPUT_VERSION
#define DIRECTINPUT_VERSION 0x0800
#endif
#include <dinput.h>
#include <DirectXMath.h>
#include <thread>
#include <Xinput.h>

enum GamepadType
{
  XInput,
  DirectInput
};

struct GamepadInfo
{
  bool IsPresent{ false };
  GamepadType Type;

  int XInputId{ 0 };
  LPDIRECTINPUTDEVICE8 DInputGamepad{ NULL };

  XINPUT_STATE XInputState{ 0 };
  DIJOYSTATE2 DInputState{ 0 };
};

struct CaptureInfo
{
  bool CaptureKb{ false };
  bool CaptureGamepad{ false };
  int ActionIndex{ 0 };

  int CapturedKbKey{ 0 };
  std::string CapturedKbName{ "" };
};

class InputSystem
{
public:
  InputSystem();
  ~InputSystem();

  void Initialize();
  // WM_INPUT on the game's window (the game reads the mouse through raw
  // input): adds up the mouse's movement and wheel for GetMouseState
  void HandleMouseMsg(LPARAM lParam);
  bool HandleKeyMsg(WPARAM wParam, LPARAM lParam);

  void Update();

  void ShowUI();
  void DrawUI();

  // "Insert", "Ctrl + F2" and so on, for showing the hotkeys in the UI
  std::string GetKeyName(Action action) const { return m_KeyboardKeyNames[action]; }
  bool IsCapturing() const { return m_CaptureState.CaptureKb || m_CaptureState.CaptureGamepad; }

  bool IsActionDown(Action action);
  bool WentDown(Action action);
  float GetActionState(Action action);
  // Movement and wheel (in WHEEL_DELTA units) since the last call
  DirectX::XMFLOAT3 GetMouseState();

  void ReadConfig(INIReader* pReader);
  const std::string GetConfig();

  void RegisterHotkeyCallback(std::function<void(InputSystem const&)> func);

private:
  void ActionUpdate();
  void ControllerUpdate();
  void HotkeyUpdate();

  void UpdateXInput();
  void UpdateDInput();

  void StartKeyboardCapture(int index);
  void StartGamepadCapture(int index);
  void UpdateKeyName(int action);
  void ResetBindings();

  static BOOL DIEnumDevicesCallback(LPCDIDEVICEINSTANCE lpddi, LPVOID pvRef);

private:
  bool m_ShowUI;

  RAWINPUTDEVICE m_RawInput;
  LPDIRECTINPUT8 m_DInputInterface;
  GamepadInfo m_Gamepad;

  std::array<int, Action::ActionCount>              m_KeyboardBindings;
  std::array<GamepadKey, Action::ActionCount>       m_GamepadBindings;
  std::array<std::string, Action::ActionCount>      m_KeyboardKeyNames;

  std::array<float, Action::ActionCount>            m_WantedActionStates;
  std::array<float, Action::ActionCount>            m_PreviousActionStates;
  std::array<float, Action::ActionCount>            m_SmoothActionStates;
  std::array<float, GamepadKey::GamepadKey_Count>   m_GamepadKeyStates;

  DirectX::XMFLOAT2 m_PrevMousePos;
  // From the window's thread (HandleMouseMsg), read in the camera hook (GetMouseState)
  std::atomic<long> m_MouseX{ 0 };
  std::atomic<long> m_MouseY{ 0 };
  std::atomic<long> m_MouseWheel{ 0 };
  unsigned long long m_MouseReadTick{ 0 };  // when GetMouseState last ran (only the camera hook calls it)

  CaptureInfo m_CaptureState;

  std::thread m_ActionThread;
  std::thread m_ControllerThread;
  std::thread m_HotkeyThread;

  std::chrono::steady_clock::time_point m_LastUpdate;

public:
  InputSystem(InputSystem const&) = delete;
  void operator=(InputSystem const&) = delete;
};