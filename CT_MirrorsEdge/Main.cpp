#include "Main.h"
#include "Util/Util.h"
#include <chrono>
#include <d3d11.h>
#include <fstream>

static const char* g_gameName = "Mirror's Edge Catalyst";
static const char* g_moduleNames[] = { "MirrorsEdgeCatalyst.exe", "MirrorsEdgeCatalystTrial.exe" };
static const char* g_configFile = "./Cinematic Tools/config.ini";

Main* g_mainHandle = nullptr;
HINSTANCE g_dllHandle = NULL;
HINSTANCE g_gameHandle = NULL;
HWND g_gameHwnd = NULL;
WNDPROC g_origWndProc = 0;
bool g_shutdown = false;
bool g_hasFocus = true;

ID3D11DeviceContext* g_d3d11Context = nullptr;
ID3D11Device* g_d3d11Device = nullptr;
IDXGISwapChain* g_dxgiSwapChain = nullptr;

namespace
{
  LPTOP_LEVEL_EXCEPTION_FILTER g_previousCrashFilter = nullptr;

  // ShowCursor(TRUE) calls the tools made for the menu, undone when it closes.
  // The display count belongs to the window's thread, so this runs from the
  // window procedure.
  int g_cursorShowCount = 0;

  void UpdateCursorVisibility()
  {
    UI* pUI = g_mainHandle->GetUI();
    if (pUI->IsEnabled() && pUI->UsesWindowsCursor())
    {
      // One step per message, in case the game keeps hiding it
      CURSORINFO info{};
      info.cbSize = sizeof(info);
      if (GetCursorInfo(&info))
      {
        if (!(info.flags & CURSOR_SHOWING) && g_cursorShowCount < 64)
        {
          ShowCursor(TRUE);
          g_cursorShowCount++;
        }
        if (!info.hCursor)
          SetCursor(LoadCursor(NULL, IDC_ARROW));
      }
    }
    else
    {
      // Back to however hidden the game had it
      for (; g_cursorShowCount > 0; --g_cursorShowCount)
        ShowCursor(FALSE);
    }
  }

  bool ReadQwordSafely(__int64 address, __int64& value)
  {
    __try
    {
      value = *(__int64*)address;
      return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      return false;
    }
  }

  // Inside one of the game exe's executable sections (from its PE header)
  bool IsGameCode(__int64 address)
  {
    __int64 base = (__int64)g_gameHandle;
    IMAGE_DOS_HEADER* pDos = (IMAGE_DOS_HEADER*)base;
    IMAGE_NT_HEADERS* pNt = (IMAGE_NT_HEADERS*)(base + pDos->e_lfanew);
    IMAGE_SECTION_HEADER* pSection = IMAGE_FIRST_SECTION(pNt);
    for (int i = 0; i < pNt->FileHeader.NumberOfSections; ++i, ++pSection)
    {
      if (!(pSection->Characteristics & IMAGE_SCN_MEM_EXECUTE))
        continue;
      __int64 start = base + pSection->VirtualAddress;
      if (address >= start && address < start + pSection->Misc.VirtualSize)
        return true;
    }
    return false;
  }

  // When the game is about to crash, note where and what the tools were
  // doing in CT.log, then let the game's or Windows' own handling go on
  LONG WINAPI CrashFilter(EXCEPTION_POINTERS* pInfo)
  {
    static volatile LONG s_noted = 0;
    if (pInfo && pInfo->ExceptionRecord && InterlockedExchange(&s_noted, 1) == 0)
    {
      EXCEPTION_RECORD* pRecord = pInfo->ExceptionRecord;
      __int64 address = (__int64)pRecord->ExceptionAddress;

      char module[MAX_PATH] = "?";
      HMODULE hModule = NULL;
      if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        (LPCSTR)address, &hModule) && hModule)
      {
        GetModuleFileNameA(hModule, module, MAX_PATH);
        char* pName = strrchr(module, '\\');
        if (pName) memmove(module, pName + 1, strlen(pName));
      }

      char text[1536];
      int length = sprintf_s(text, "[CRASH] The game crashed: exception 0x%08X at %s+0x%I64X", pRecord->ExceptionCode,
        module, hModule ? address - (__int64)hModule : address);
      if (pRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && pRecord->NumberParameters >= 2 && length > 0)
        sprintf_s(text + length, sizeof(text) - length, " (%s 0x%I64X)",
          pRecord->ExceptionInformation[0] == 1 ? "writing" : pRecord->ExceptionInformation[0] == 8 ? "executing" : "reading",
          (__int64)pRecord->ExceptionInformation[1]);
      util::log::CrashNote(text);

      // The registers, and the game code the stack goes back to, to see where
      // a bad value came from
      if (pInfo->ContextRecord)
      {
        CONTEXT const* c = pInfo->ContextRecord;
        sprintf_s(text, "[CRASH] rax %I64X rbx %I64X rcx %I64X rdx %I64X rsi %I64X rdi %I64X rbp %I64X rsp %I64X",
          c->Rax, c->Rbx, c->Rcx, c->Rdx, c->Rsi, c->Rdi, c->Rbp, c->Rsp);
        util::log::CrashNote(text);
        sprintf_s(text, "[CRASH] r8 %I64X r9 %I64X r10 %I64X r11 %I64X r12 %I64X r13 %I64X r14 %I64X r15 %I64X",
          c->R8, c->R9, c->R10, c->R11, c->R12, c->R13, c->R14, c->R15);
        util::log::CrashNote(text);

        int stackLength = sprintf_s(text, "[CRASH] game code on the stack:");
        int found = 0;
        for (int i = 0; i < 1024 && found < 16 && stackLength > 0 && stackLength < (int)sizeof(text) - 24; ++i)
        {
          __int64 value = 0;
          if (!ReadQwordSafely((__int64)c->Rsp + i * 8, value))
            break;
          if (IsGameCode(value))
          {
            stackLength += sprintf_s(text + stackLength, sizeof(text) - stackLength, " +0x%I64X",
              value - (__int64)g_gameHandle);
            ++found;
          }
        }
        util::log::CrashNote(text);
      }

      if (g_mainHandle && g_mainHandle->GetCameraManager())
      {
        char state[1024];
        g_mainHandle->GetCameraManager()->DescribeState(state, sizeof(state));
        sprintf_s(text, "[CRASH] Tools at the time: %s", state);
        util::log::CrashNote(text);
      }
    }

    return g_previousCrashFilter ? g_previousCrashFilter(pInfo) : EXCEPTION_CONTINUE_SEARCH;
  }
}

Main::Main() :
  m_ConfigChanged(false),
  m_dtConfigCheck(0),
  m_GraphicsInitialized(false),
  m_GraphicsFailed(false)
{

}

Main::~Main()
{
}

bool Main::Initialize()
{
  CreateDirectoryA("Cinematic Tools", NULL);

  // The console is off unless [General] ShowConsole = true (or the MISC tab)
  {
    INIReader earlyConfig(g_configFile);
    m_ShowConsole = earlyConfig.GetBoolean("General", "ShowConsole", false);
  }
  util::log::Init(m_ShowConsole);
  util::log::Write("Cinematic Tools for %s\n", g_gameName);

  // Used for relative offsets and signature scanning
  for (const char* moduleName : g_moduleNames)
  {
    g_gameHandle = GetModuleHandleA(moduleName);
    if (g_gameHandle) break;
  }

  if (g_gameHandle == NULL)
  {
    util::log::Error("Failed to retrieve module handle, GetLastError 0x%X", GetLastError());
    return false;
  }

  // Before the scan, which can wait several seconds for the game to start
  util::hooks::ReserveSpaceNearGame();

  // Offsets have to be known before anything reads them
  util::offsets::Scan();

  m_pCameraManager = std::make_unique<CameraManager>();
  m_pVisualsManager = std::make_unique<VisualsManager>();
  m_pLightManager = std::make_unique<LightManager>();
  m_pInputSystem = std::make_unique<InputSystem>();
  m_pUI = std::make_unique<UI>();

  m_pInputSystem->Initialize();

  // The window handle and Dx11 interfaces are taken from the game's
  // swapchain on the first Present call, see OnPresent.
  util::hooks::Init();

  LoadConfig();
  return true;
}

void Main::Run()
{
  // Main update loop. The camera itself is updated from the game's
  // camera update hook so that it moves once per rendered frame.
  auto lastUpdate = std::chrono::steady_clock::now();
  while (!g_shutdown)
  {
    auto now = std::chrono::steady_clock::now();
    std::chrono::duration<double> dt = now - lastUpdate;
    lastUpdate = now;

    // WM_ACTIVATE can be missed when the tools are injected after the
    // window got focus, so check directly
    if (g_gameHwnd)
      g_hasFocus = (GetForegroundWindow() == g_gameHwnd);

    m_pInputSystem->Update();
    m_pUI->Update(dt.count());

    // Check if config has been affected, if so, save it
    m_dtConfigCheck += dt.count();
    if (m_dtConfigCheck > 10.f)
    {
      m_dtConfigCheck = 0;
      if (m_ConfigChanged)
      {
        m_ConfigChanged = false;
        SaveConfig();
      }
    }

    Sleep(5);
  }

  // Save config and disable hooks before exit
  SaveConfig();
  util::hooks::SetHookState(false);
}

void Main::OnPresent(IDXGISwapChain* pSwapChain)
{
  if (!m_GraphicsInitialized)
  {
    if (m_GraphicsFailed || !InitializeGraphics(pSwapChain))
      return;
  }

  // Overlays (Origin etc.) can present their own swapchains, only draw on the game's
  if (pSwapChain == g_dxgiSwapChain)
    m_pUI->Draw();
}

bool Main::InitializeGraphics(IDXGISwapChain* pSwapChain)
{
  DXGI_SWAP_CHAIN_DESC desc;
  if (FAILED(pSwapChain->GetDesc(&desc)) || !desc.OutputWindow)
    return false;

  // Ignore swapchains that don't belong to a top level window of the game
  DWORD windowProcessId = 0;
  GetWindowThreadProcessId(desc.OutputWindow, &windowProcessId);
  if (windowProcessId != GetCurrentProcessId() || GetAncestor(desc.OutputWindow, GA_ROOT) != desc.OutputWindow)
    return false;

  HRESULT hr = pSwapChain->GetDevice(__uuidof(ID3D11Device), (void**)&g_d3d11Device);
  if (FAILED(hr) || !g_d3d11Device)
  {
    util::log::Error("Failed to retrieve ID3D11Device from swapchain, HRESULT 0x%X", hr);
    m_GraphicsFailed = true;
    return false;
  }

  g_d3d11Device->GetImmediateContext(&g_d3d11Context);
  g_dxgiSwapChain = pSwapChain;
  g_gameHwnd = desc.OutputWindow;
  g_hasFocus = (GetForegroundWindow() == g_gameHwnd);

  // Subclass the window with a new WndProc to catch messages
  g_origWndProc = (WNDPROC)SetWindowLongPtr(g_gameHwnd, GWLP_WNDPROC, (LONG_PTR)&WndProc);
  if (g_origWndProc == 0)
  {
    util::log::Error("Failed to set WndProc, GetLastError 0x%X", GetLastError());
    m_GraphicsFailed = true;
    return false;
  }

  if (!m_pUI->Initialize())
  {
    m_GraphicsFailed = true;
    return false;
  }

  // Set this late, the game has set up its own handling by now
  g_previousCrashFilter = SetUnhandledExceptionFilter(&CrashFilter);

  util::log::Ok("Dx11 interfaces retrieved, Device 0x%I64X Context 0x%I64X SwapChain 0x%I64X",
    g_d3d11Device, g_d3d11Context, g_dxgiSwapChain);

  m_GraphicsInitialized = true;
  return true;
}

void Main::LoadConfig()
{
  // Read config.ini using inih by Ben Hoyt
  // https://github.com/benhoyt/inih

  m_pConfig = std::make_unique<INIReader>(g_configFile);
  int parseResult = m_pConfig->ParseError();

  // If there's problems reading the file, notify the user.
  // Code-wise it should be safe to just continue,
  // since you can still request variables from INIReader.
  // They'll just return the specified default value.
  if (parseResult != 0)
    util::log::Warning("Config file could not be loaded, using default settings");

  m_pCameraManager->ReadConfig(m_pConfig.get());
  m_pInputSystem->ReadConfig(m_pConfig.get());
  m_pUI->SetTextSize((int)m_pConfig->GetInteger("General", "TextSize", 1));
  m_pUI->SetDrawOwnCursor(m_pConfig->GetBoolean("General", "DrawMenuCursor", false));
}

void Main::SaveConfig()
{
  std::fstream file;
  file.open(g_configFile, std::ios_base::out | std::ios_base::trunc);

  if (!file.is_open())
  {
    util::log::Error("Could not save config, failed to open file for writing. GetLastError 0x%X", GetLastError());
    return;
  }

  file << "[General]\n";
  file << "ShowConsole = " << (m_ShowConsole ? "true" : "false") << "\n";
  file << "TextSize = " << m_pUI->GetTextSize() << "\n";
  file << "DrawMenuCursor = " << (m_pUI->UsesWindowsCursor() ? "false" : "true") << "\n\n";
  file << m_pCameraManager->GetConfig();
  file << m_pInputSystem->GetConfig();

  file.close();
}

extern LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
LRESULT CALLBACK Main::WndProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
  if (g_mainHandle && g_mainHandle->m_pUI)
    UpdateCursorVisibility();

  // The camera's mouse look reads the game's own raw input, before the game
  // (or the menu, which keeps it from the game) gets the message
  if (uMsg == WM_INPUT && g_mainHandle && g_mainHandle->m_pInputSystem)
    g_mainHandle->m_pInputSystem->HandleMouseMsg(lParam);

  // The wheel doesn't go to ImGui directly: this runs on the window's thread,
  // and a wheel added to ImGui while the render thread is building the menu
  // is cleared at the end of that frame without being used
  if (uMsg == WM_MOUSEWHEEL && g_mainHandle->m_pUI->IsEnabled())
  {
    g_mainHandle->m_pUI->AddWheelFromMessage((float)GET_WHEEL_DELTA_WPARAM(wParam) / WHEEL_DELTA);
    return 0;
  }
  // Typed characters too, ImGui clears them at the end of every frame
  if (uMsg == WM_CHAR && g_mainHandle->m_pUI->IsEnabled())
  {
    g_mainHandle->m_pUI->AddCharFromMessage((unsigned int)wParam);
    return 0;
  }

  // The cursor is only ImGui's while the menu is open, the rest of the time
  // the game sets (hides) its own
  bool menuOpen = g_mainHandle->m_pUI->IsEnabled();
  if ((menuOpen || uMsg != WM_SETCURSOR) && ImGui_ImplWin32_WndProcHandler(hwnd, uMsg, wParam, lParam))
    return TRUE;

  // While the tools UI is open the game doesn't see the mouse (or raw input),
  // so clicks on the UI don't also go to the game.
  if (g_mainHandle->m_pUI->IsEnabled())
  {
    switch (uMsg)
    {
    case WM_MOUSEMOVE:
    case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK:
    case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_RBUTTONDBLCLK:
    case WM_MBUTTONDOWN: case WM_MBUTTONUP: case WM_MBUTTONDBLCLK:
    case WM_XBUTTONDOWN: case WM_XBUTTONUP: case WM_XBUTTONDBLCLK:
    case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL:
      return 0;
    case WM_INPUT:
      // DefWindowProc still has to run for raw input cleanup
      return DefWindowProc(hwnd, uMsg, wParam, lParam);
    }
  }

  switch (uMsg)
  {
  case WM_ACTIVATE:
    // Focus event
    g_hasFocus = (LOWORD(wParam) != WA_INACTIVE);
    break;
  case WM_KEYDOWN:
  case WM_SYSKEYDOWN: // Alt and F10
    if (g_mainHandle->m_pInputSystem->HandleKeyMsg(wParam, lParam))
      return TRUE;
    // F10 on its own would start Windows' keyboard menu mode (like Alt),
    // which stalls the game's window. It's the camera key, and the game reads
    // its keys another way, so it stops here.
    if (uMsg == WM_SYSKEYDOWN && wParam == VK_F10)
      return 0;
    break;
  case WM_SYSKEYUP:
    if (wParam == VK_F10)
      return 0;
    break;
  case WM_DESTROY:
    g_shutdown = true;
    break;
  }

  return CallWindowProc(g_origWndProc, hwnd, uMsg, wParam, lParam);
}

