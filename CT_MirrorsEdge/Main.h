#pragma once
#include "Camera/CameraManager.h"
#include "Input/InputSystem.h"
#include "Visuals/VisualsManager.h"
#include "Lights/LightManager.h"
#include "UI.h"

#include "inih/cpp/INIReader.h"
#include <memory>
#include <Windows.h>

class Main
{
public:
  Main();
  ~Main();

  bool Initialize();
  void Run();

  // Called from the IDXGISwapChain::Present hook (render thread)
  void OnPresent(IDXGISwapChain* pSwapChain);

  CameraManager* GetCameraManager() { return m_pCameraManager.get(); }
  VisualsManager* GetVisualsManager() { return m_pVisualsManager.get(); }
  LightManager* GetLightManager() { return m_pLightManager.get(); }
  InputSystem* GetInputSystem() { return m_pInputSystem.get(); }
  UI* GetUI() { return m_pUI.get(); }

  void OnConfigChanged() { m_ConfigChanged = true; }
  bool* GetShowConsoleSetting() { return &m_ShowConsole; }

private:
  bool InitializeGraphics(IDXGISwapChain* pSwapChain);

  void LoadConfig();
  void SaveConfig();

  static LRESULT CALLBACK WndProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam);

private:
  std::unique_ptr<INIReader> m_pConfig;

  std::unique_ptr<CameraManager> m_pCameraManager;
  std::unique_ptr<VisualsManager> m_pVisualsManager;
  std::unique_ptr<LightManager> m_pLightManager;
  std::unique_ptr<InputSystem> m_pInputSystem;
  std::unique_ptr<UI> m_pUI;

  bool m_ConfigChanged;
  double m_dtConfigCheck;

  bool m_GraphicsInitialized;
  bool m_GraphicsFailed;
  bool m_ShowConsole{ false };

public:
  Main(Main const&) = delete;
  void operator=(Main const&) = delete;
};

extern bool g_shutdown;

extern bool g_hasFocus;

extern Main* g_mainHandle;
extern HINSTANCE g_dllHandle;

extern HINSTANCE g_gameHandle;
extern HWND g_gameHwnd;
extern WNDPROC g_origWndProc;

extern ID3D11Device* g_d3d11Device;
extern ID3D11DeviceContext* g_d3d11Context;
extern IDXGISwapChain* g_dxgiSwapChain;
