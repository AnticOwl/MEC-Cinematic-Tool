#include "UI.h"
#include "Main.h"
#include "Util/Util.h"
#include "Util/ImGuiEXT.h"
#include "imgui/imgui.h"
#include "imgui/imgui_impl_dx11.h"
#include <cmath>

const char* g_creditsText = "The following libraries are used in making the Cinematic Tools:\n"
"MinHook by Tsuda Kageyu, inih by Ben Hoyt, ImGui by Omar Cornut.\n\n"
"Based on Cinematic Tools by Hattiwatti.\n\n"
"Thank you for using the tools and making awesome stuff with it!";

namespace
{
  // The game logo's colours: the white runner tag on crimson (#960000 in the
  // exe icon), on near-black so it reads over the game
  const ImVec4 g_logoRed(0.588f, 0.000f, 0.000f, 1.f);      // #960000
  const ImVec4 g_logoRedLight(0.722f, 0.110f, 0.110f, 1.f); // #B81C1C
  const ImVec4 g_logoRedBright(0.831f, 0.165f, 0.165f, 1.f); // #D42A2A
  const ImVec4 g_logoRedDark(0.361f, 0.000f, 0.000f, 1.f);  // #5C0000
  const ImVec4 g_logoWhite(0.961f, 0.961f, 0.961f, 1.f);    // #F5F5F5

  // Text sizes at 1080p, scaled with the game's resolution
  const char* g_textSizeNames[] = { "Small", "Normal", "Large", "Extra large" };
  const float g_textSizes[] = { 15.f, 17.f, 20.f, 24.f };
  const int g_textSizeCount = 4;

  // Latin-1 plus dashes, bullets, the ellipsis and arrows
  const ImWchar g_glyphRanges[] = { 0x0020, 0x00FF, 0x2013, 0x2014, 0x2022, 0x2022, 0x2026, 0x2026, 0x2190, 0x2193, 0 };

  ImVec4 WithAlpha(ImVec4 color, float alpha)
  {
    color.w = alpha;
    return color;
  }

  // The game reads the mouse through raw input, so its window gets no wheel
  // messages. The tools read that raw input too (InputSystem::HandleMouseMsg),
  // but a low-level mouse hook sees the wheel before any of that, so the menu
  // uses it while it's open. It only reads it.
  UI* g_pWheelUI = nullptr;
  DWORD g_wheelThreadId = 0;

  LRESULT CALLBACK WheelHookProc(int code, WPARAM wParam, LPARAM lParam)
  {
    if (code == HC_ACTION && wParam == WM_MOUSEWHEEL && g_pWheelUI && g_hasFocus)
    {
      const MSLLHOOKSTRUCT* pInfo = (const MSLLHOOKSTRUCT*)lParam;
      g_pWheelUI->AddWheelFromHook((float)(short)HIWORD(pInfo->mouseData) / WHEEL_DELTA);
    }
    return CallNextHookEx(NULL, code, wParam, lParam);
  }

  // Every mouse event on the PC goes through a low-level hook first, so it's
  // only set while the menu is open: during play the mouse doesn't wait on it
  const UINT WM_WHEELHOOK_ON = WM_APP + 1;
  const UINT WM_WHEELHOOK_OFF = WM_APP + 2;

  DWORD WINAPI WheelHookThread(LPVOID)
  {
    // Low-level hooks run on the thread that set them, which needs a message
    // loop. Peeking makes the queue right away, and a high priority keeps the
    // hook quick while the game uses every core.
    MSG msg;
    PeekMessageW(&msg, NULL, WM_USER, WM_USER, PM_NOREMOVE);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);

    HMODULE hSelf = NULL;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
      (LPCWSTR)&WheelHookProc, &hSelf);

    HHOOK hHook = NULL;
    bool warned = false;
    auto setHook = [&](bool on)
    {
      if (on && !hHook)
      {
        hHook = SetWindowsHookExW(WH_MOUSE_LL, WheelHookProc, hSelf, 0);
        if (hHook)
          g_pWheelUI->OnWheelHookReady();
        else if (!warned)
        {
          warned = true;
          util::log::Warning("Couldn't set the mouse wheel hook (0x%X), the menu may not scroll with the wheel", GetLastError());
        }
      }
      else if (!on && hHook)
      {
        UnhookWindowsHookEx(hHook);
        hHook = NULL;
      }
    };

    // The menu may have been opened before this thread had its queue
    setHook(g_pWheelUI->IsEnabled());

    while (GetMessageW(&msg, NULL, 0, 0) > 0)
    {
      if (msg.message == WM_WHEELHOOK_ON || msg.message == WM_WHEELHOOK_OFF)
        setHook(msg.message == WM_WHEELHOOK_ON);
    }

    setHook(false);
    return 0;
  }
}

UI::UI() :
  m_Initialized(false),
  m_Enabled(false),
  m_SelectedMenu(UIMenu_Camera),
  m_HasKeyboardFocus(false),
  m_HasMouseFocus(false)
{

}

UI::~UI()
{
  if (g_wheelThreadId)
    PostThreadMessageW(g_wheelThreadId, WM_QUIT, 0, 0);

  if (m_Initialized)
  {
    ImGui_ImplDX11_Shutdown();
    ImGui::DestroyContext();
  }
}

bool UI::Initialize()
{
  /////////////////////////
  // ImGui Configuration //
  /////////////////////////

  // ImGui is an immensely useful and simple-to-use UI system made by Omar Cornut.
  // Definitely check it out:
  // https://github.com/ocornut/imgui

  ImGui::CreateContext();
  if (!ImGui_ImplDX11_Init(g_gameHwnd, g_d3d11Device, g_d3d11Context))
  {
    util::log::Error("ImGui Dx11 implementation failed to initialize");
    return false;
  }

  // The window position and size are kept with the tools' config, not
  // in the game folder
  ImGui::GetIO().IniFilename = ".\\Cinematic Tools\\imgui.ini";

  BuildFonts();

  g_pWheelUI = this;
  HANDLE hThread = CreateThread(NULL, 0, WheelHookThread, NULL, 0, &g_wheelThreadId);
  if (hThread) CloseHandle(hThread);

  m_Initialized = true;
  util::log::Ok("UI Initialized, press F5 to open it");
  return true;
}

void UI::SetTextSize(int size)
{
  if (size < 0) size = 0;
  if (size >= g_textSizeCount) size = g_textSizeCount - 1;
  if (size == m_TextSize) return;

  m_TextSize = size;
  m_FontsDirty = true;
}

float UI::GetResolutionScale()
{
  RECT rect;
  if (!g_gameHwnd || !GetClientRect(g_gameHwnd, &rect) || rect.bottom <= 0)
    return 1.f;

  float scale = rect.bottom / 1080.f;
  if (scale < 0.75f) scale = 0.75f;
  if (scale > 2.25f) scale = 2.25f;
  return scale;
}

void UI::BuildFonts()
{
  ImGuiIO& io = ImGui::GetIO();
  m_FontScale = GetResolutionScale();
  float size = roundf(g_textSizes[m_TextSize] * m_FontScale);

  io.Fonts->Clear();

  // Segoe UI is on every Windows since Vista and reads much better than
  // ImGui's built-in pixel font
  char path[MAX_PATH];
  UINT length = GetWindowsDirectoryA(path, MAX_PATH);
  ImFont* pFont = nullptr;
  if (length > 0 && length < MAX_PATH - 24)
  {
    strcat_s(path, "\\Fonts\\segoeui.ttf");
    if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES)
      pFont = io.Fonts->AddFontFromFileTTF(path, size, nullptr, g_glyphRanges);
  }

  if (!pFont)
  {
    ImFontConfig config;
    config.SizePixels = roundf(13.f * size / 17.f);
    io.Fonts->AddFontDefault(&config);
  }

  ApplyStyle(size / 17.f);
}

void UI::ApplyStyle(float scale)
{
  ImGuiStyle& style = ImGui::GetStyle();
  style = ImGuiStyle();

  // Square corners, like the game's own menus, and a bit more room than
  // ImGui's defaults so the controls are easier to hit
  style.WindowRounding = 0.f;
  style.ChildRounding = 0.f;
  style.FrameRounding = 0.f;
  style.PopupRounding = 0.f;
  style.ScrollbarRounding = 0.f;
  style.GrabRounding = 0.f;
  style.WindowBorderSize = 1.f;
  style.FrameBorderSize = 0.f;
  style.WindowTitleAlign = ImVec2(0.f, 0.5f);
  style.WindowPadding = ImVec2(12, 10);
  style.FramePadding = ImVec2(8, 4);
  style.ItemSpacing = ImVec2(8, 6);
  style.ItemInnerSpacing = ImVec2(6, 4);
  style.IndentSpacing = 18;
  style.ScrollbarSize = 16;
  style.GrabMinSize = 12;

  ImVec4* colors = style.Colors;
  colors[ImGuiCol_Text] = g_logoWhite;
  colors[ImGuiCol_TextDisabled] = ImVec4(0.62f, 0.62f, 0.62f, 1.f);
  colors[ImGuiCol_WindowBg] = ImVec4(0.055f, 0.055f, 0.055f, 0.95f);
  colors[ImGuiCol_ChildBg] = ImVec4(0.f, 0.f, 0.f, 0.f);
  colors[ImGuiCol_PopupBg] = ImVec4(0.08f, 0.08f, 0.08f, 0.98f);
  colors[ImGuiCol_Border] = WithAlpha(g_logoRed, 0.70f);
  colors[ImGuiCol_BorderShadow] = ImVec4(0.f, 0.f, 0.f, 0.f);

  colors[ImGuiCol_FrameBg] = ImVec4(0.14f, 0.14f, 0.14f, 1.f);
  colors[ImGuiCol_FrameBgHovered] = ImVec4(0.20f, 0.11f, 0.11f, 1.f);
  colors[ImGuiCol_FrameBgActive] = ImVec4(0.26f, 0.09f, 0.09f, 1.f);

  colors[ImGuiCol_TitleBg] = g_logoRedDark;
  colors[ImGuiCol_TitleBgActive] = g_logoRed;
  colors[ImGuiCol_TitleBgCollapsed] = WithAlpha(g_logoRedDark, 0.75f);
  colors[ImGuiCol_MenuBarBg] = ImVec4(0.10f, 0.10f, 0.10f, 1.f);

  colors[ImGuiCol_ScrollbarBg] = ImVec4(0.04f, 0.04f, 0.04f, 0.60f);
  colors[ImGuiCol_ScrollbarGrab] = g_logoRedDark;
  colors[ImGuiCol_ScrollbarGrabHovered] = g_logoRed;
  colors[ImGuiCol_ScrollbarGrabActive] = g_logoRedLight;

  colors[ImGuiCol_CheckMark] = g_logoWhite;
  colors[ImGuiCol_SliderGrab] = g_logoRedLight;
  colors[ImGuiCol_SliderGrabActive] = g_logoRedBright;

  colors[ImGuiCol_Button] = g_logoRed;
  colors[ImGuiCol_ButtonHovered] = g_logoRedLight;
  colors[ImGuiCol_ButtonActive] = g_logoRedBright;

  // Section headers and keyframe rows
  colors[ImGuiCol_Header] = WithAlpha(g_logoRed, 0.55f);
  colors[ImGuiCol_HeaderHovered] = WithAlpha(g_logoRedLight, 0.80f);
  colors[ImGuiCol_HeaderActive] = g_logoRedBright;

  colors[ImGuiCol_Separator] = WithAlpha(g_logoRed, 0.60f);
  colors[ImGuiCol_SeparatorHovered] = g_logoRedLight;
  colors[ImGuiCol_SeparatorActive] = g_logoRedBright;

  colors[ImGuiCol_ResizeGrip] = WithAlpha(g_logoRed, 0.40f);
  colors[ImGuiCol_ResizeGripHovered] = g_logoRedLight;
  colors[ImGuiCol_ResizeGripActive] = g_logoRedBright;

  colors[ImGuiCol_PlotLines] = g_logoWhite;
  colors[ImGuiCol_PlotLinesHovered] = g_logoRedBright;
  colors[ImGuiCol_PlotHistogram] = g_logoRedLight;
  colors[ImGuiCol_PlotHistogramHovered] = g_logoRedBright;

  colors[ImGuiCol_TextSelectedBg] = WithAlpha(g_logoRedLight, 0.50f);
  colors[ImGuiCol_ModalWindowDarkening] = ImVec4(0.f, 0.f, 0.f, 0.60f);
  colors[ImGuiCol_DragDropTarget] = g_logoRedBright;
  colors[ImGuiCol_NavHighlight] = g_logoWhite;
  colors[ImGuiCol_NavWindowingHighlight] = g_logoWhite;

  style.ScaleAllSizes(scale);
}

// The menu tabs. The open one is drawn like the logo itself: white with red
bool UI::TabButton(const char* label, SelectedMenu menu, float width)
{
  bool selected = m_SelectedMenu == menu;
  if (selected)
  {
    ImGui::PushStyleColor(ImGuiCol_Button, g_logoWhite);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, g_logoWhite);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, g_logoWhite);
    ImGui::PushStyleColor(ImGuiCol_Text, g_logoRed);
  }

  bool pressed = ImGui::Button(label, ImVec2(width, ImGui::GetFrameHeight() * 1.35f));
  if (selected)
    ImGui::PopStyleColor(4);

  if (pressed)
    m_SelectedMenu = menu;
  return pressed;
}

void UI::AddWheel(float notches)
{
  if (!m_Enabled || notches == 0.f) return;

  float current = m_PendingWheel.load();
  while (!m_PendingWheel.compare_exchange_weak(current, current + notches)) {}
}

void UI::OnWheelHookReady()
{
  if (!m_WheelHookReady.exchange(true))
    util::log::Ok("Mouse wheel hook set, the menu scrolls with the wheel (only while it's open)");
}

void UI::AddWheelFromHook(float notches)
{
  if (!m_UsedWheelHook.exchange(true) && m_Enabled)
    util::log::Write("Menu scrolling: first wheel turn seen by the hook");
  AddWheel(notches);
}

void UI::AddWheelFromMessage(float notches)
{
  // Logged once, so CT.log shows whether the game sends these at all
  if (!m_SawWheelMessage.exchange(true))
    util::log::Write("The game window gets mouse wheel messages");
  // The hook already counts every wheel turn
  if (!m_WheelHookReady)
    AddWheel(notches);
}

void UI::AddCharFromMessage(unsigned int character)
{
  if (!m_Enabled || character == 0 || character >= 0x10000) return;

  std::lock_guard<std::mutex> lock(m_CharMutex);
  if (m_PendingChars.size() < 64)
    m_PendingChars.push_back(character);
}

void UI::AddWheelFromRawInput(float notches)
{
  if (notches == 0.f) return;
  if (!m_UsedRawInputWheel.exchange(true))
    util::log::Write("Raw input sees the mouse wheel");
  // Only without the hook and wheel messages, so one notch never scrolls twice
  if (!m_WheelHookReady && !m_SawWheelMessage)
    AddWheel(notches);
}

void UI::DrawMiscMenu()
{
  ImGui::SubHeading("Menu");
  ImGui::PushItemWidth(ImGui::GetFontSize() * 10);
  int textSize = m_TextSize;
  if (ImGui::Combo("Text size", &textSize, g_textSizeNames, g_textSizeCount))
  {
    SetTextSize(textSize);
    g_mainHandle->OnConfigChanged();
  }
  ImGui::PopItemWidth();
  ImGui::HelpMarker("Scales with the game's resolution too. Drag the window's bottom right corner to resize it.");

  int cursor = m_DrawOwnCursor ? 1 : 0;
  const char* cursorNames[] = { "Windows cursor", "Drawn by the menu" };
  ImGui::PushItemWidth(ImGui::GetFontSize() * 10);
  if (ImGui::Combo("Mouse cursor", &cursor, cursorNames, 2))
  {
    m_DrawOwnCursor = cursor == 1;
    g_mainHandle->OnConfigChanged();
  }
  ImGui::PopItemWidth();
  ImGui::HelpMarker("The cursor shown while this menu is open. The Windows cursor is sharper and has no "
    "lag. If it doesn't show up on your setup, pick \"Drawn by the menu\".");

  if (ImGui::Button("Change hotkeys..."))
    g_mainHandle->GetInputSystem()->ShowUI();

  ImGui::SubHeading("Log");
  if (ImGui::Checkbox("Show the console window (from the next start)", g_mainHandle->GetShowConsoleSetting()))
    g_mainHandle->OnConfigChanged();
  ImGui::TextDisabled("The log is always written to Cinematic Tools\\CT.log in the game folder.");

  ImGui::SubHeading("About");
  ImGui::TextWrapped("%s", g_creditsText);
}

void UI::Draw()
{
  if (!m_Initialized) return;

  ImGuiIO& io = ImGui::GetIO();
  // Normally the Windows cursor is shown (see UpdateCursorVisibility in Main.cpp),
  // the menu only draws its own when asked to
  io.MouseDrawCursor = m_Enabled && m_DrawOwnCursor;
  if (!m_Enabled)
  {
    m_HasKeyboardFocus = false;
    m_HasMouseFocus = false;
    m_PendingWheel = 0.f;
    return;
  }

  // New fonts after a text size or resolution change. The font texture is
  // made again by the next NewFrame.
  if (fabsf(GetResolutionScale() - m_FontScale) > 0.05f)
    m_FontsDirty = true;
  if (m_FontsDirty)
  {
    m_FontsDirty = false;
    BuildFonts();
    ImGui_ImplDX11_InvalidateDeviceObjects();
  }

  // The render target is made every frame so that no reference to the
  // backbuffer is kept around when the game resizes its swapchain.
  ComPtr<ID3D11Texture2D> pBackBuffer;
  ComPtr<ID3D11RenderTargetView> pRTV;
  HRESULT hr = g_dxgiSwapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)pBackBuffer.GetAddressOf());
  if (FAILED(hr) || !pBackBuffer)
    return;

  hr = g_d3d11Device->CreateRenderTargetView(pBackBuffer.Get(), NULL, pRTV.GetAddressOf());
  if (FAILED(hr))
    return;

  // Put the game's render targets back when done
  ComPtr<ID3D11RenderTargetView> pOldRTV;
  ComPtr<ID3D11DepthStencilView> pOldDSV;
  g_d3d11Context->OMGetRenderTargets(1, pOldRTV.GetAddressOf(), pOldDSV.GetAddressOf());
  g_d3d11Context->OMSetRenderTargets(1, pRTV.GetAddressOf(), nullptr);

  // During gameplay the game reads the mouse through raw input and the
  // window may not get normal mouse messages, so read the cursor directly.
  // ClipCursor goes through the cursor hook, which drops the game's clip.
  ClipCursor(nullptr);
  POINT cursor;
  if (GetCursorPos(&cursor) && ScreenToClient(g_gameHwnd, &cursor))
    io.MousePos = ImVec2((float)cursor.x, (float)cursor.y);

  io.MouseDown[0] = g_hasFocus && (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
  io.MouseDown[1] = g_hasFocus && (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;
  io.MouseDown[2] = g_hasFocus && (GetAsyncKeyState(VK_MBUTTON) & 0x8000) != 0;

  // The wheel is collected outside ImGui (see AddWheel) and handed over
  // here, on this thread, right before the frame that uses it
  io.MouseWheel += m_PendingWheel.exchange(0.f);
  {
    std::lock_guard<std::mutex> lock(m_CharMutex);
    for (unsigned int character : m_PendingChars)
      io.AddInputCharacter((ImWchar)character);
    m_PendingChars.clear();
  }

  ImGui_ImplDX11_NewFrame();

  float fontSize = ImGui::GetFontSize();
  bool windowOpen = true;
  ImGui::SetNextWindowPos(ImVec2(fontSize * 2.f, fontSize * 2.f), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowSize(ImVec2(fontSize * 36.f, io.DisplaySize.y * 0.8f), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowSizeConstraints(ImVec2(fontSize * 26.f, fontSize * 14.f), ImVec2(FLT_MAX, FLT_MAX));
  ImGui::Begin("Cinematic Tools   (F5 to show / hide)###CinematicTools", &windowOpen);
  {
    float spacing = ImGui::GetStyle().ItemSpacing.x;
    float tabWidth = (ImGui::GetContentRegionAvailWidth() - spacing * 4.f) / 5.f;
    TabButton("CAMERA", UIMenu_Camera, tabWidth);
    ImGui::SameLine();
    TabButton("FOLLOW", UIMenu_Follow, tabWidth);
    ImGui::SameLine();
    TabButton("VISUALS", UIMenu_Visuals, tabWidth);
    ImGui::SameLine();
    TabButton("LIGHTS", UIMenu_Lights, tabWidth);
    ImGui::SameLine();
    TabButton("MISC", UIMenu_Misc, tabWidth);

    g_mainHandle->GetCameraManager()->DrawStatusBar();
    ImGui::Separator();

    // Only the tab's content scrolls, the tabs and status stay in place
    ImGui::BeginChild("##TabContent", ImVec2(0, 0), false);
    if (m_SelectedMenu == UIMenu_Camera)
      g_mainHandle->GetCameraManager()->DrawUI();
    else if (m_SelectedMenu == UIMenu_Follow)
      g_mainHandle->GetCameraManager()->DrawFollowUI();
    else if (m_SelectedMenu == UIMenu_Visuals)
      g_mainHandle->GetVisualsManager()->DrawUI();
    else if (m_SelectedMenu == UIMenu_Lights)
      g_mainHandle->GetLightManager()->DrawUI();
    else if (m_SelectedMenu == UIMenu_Misc)
      DrawMiscMenu();
    ImGui::EndChild();
  } ImGui::End();

  g_mainHandle->GetInputSystem()->DrawUI();

  ImGui::Render();
  ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

  g_d3d11Context->OMSetRenderTargets(1, pOldRTV.GetAddressOf(), pOldDSV.Get());

  m_HasKeyboardFocus = io.WantCaptureKeyboard;
  m_HasMouseFocus = io.WantCaptureMouse;

  // The window's close button
  if (!windowOpen)
    SetEnabled(false);
}

void UI::Update(double dt)
{
}

void UI::Toggle()
{
  SetEnabled(!m_Enabled);
}

void UI::SetEnabled(bool enabled)
{
  m_Enabled = enabled;
  // The wheel hook is only needed while the menu is open
  if (g_wheelThreadId)
    PostThreadMessageW(g_wheelThreadId, enabled ? WM_WHEELHOOK_ON : WM_WHEELHOOK_OFF, 0, 0);
}
