#include "Util.h"
#include "../Main.h"

#include <MinHook.h>
#include <d3d11.h>
#include <DirectXMath.h>
#include <unordered_map>
#include <Windows.h>
#include <Psapi.h>

#pragma comment(lib, "d3d11.lib")

// Function definitions
typedef HRESULT(WINAPI* tIDXGISwapChain_Present)(IDXGISwapChain*, UINT, UINT);

// Neither of these game functions is fully known, the original tools' detours
// only touched rcx before calling the original. Forward all four integer
// argument registers so nothing gets lost.
typedef __int64(__fastcall* tCameraUpdate)(__int64, __int64, __int64, __int64);
typedef __int64(__fastcall* tEntryInput)(__int64, __int64, __int64, __int64);
typedef __int64(__fastcall* tVisualEnvironmentUpdate)(__int64, __int64, __int64, __int64);
typedef __int64(__fastcall* tZoneStreamerUpdate)(__int64, __int64, __int64, __int64);

typedef BOOL(WINAPI* tSetCursorPos)(int, int);
typedef BOOL(WINAPI* tClipCursor)(const RECT*);
typedef HCURSOR(WINAPI* tSetCursor)(HCURSOR);

//////////////////////////
////   RENDER HOOKS   ////
//////////////////////////

// Most games today run on DirectX 11. This hook is called just before
// the image is actually drawn on screen so we can draw more stuff
// on top, like the tools UI.

tIDXGISwapChain_Present oIDXGISwapChain_Present = nullptr;
HRESULT WINAPI hIDXGISwapChain_Present(IDXGISwapChain* pSwapchain, UINT SyncInterval, UINT Flags)
{
  if (!g_shutdown)
    g_mainHandle->OnPresent(pSwapchain);

  return oIDXGISwapChain_Present(pSwapchain, SyncInterval, Flags);
}

//////////////////////////
////   CAMERA HOOKS   ////
//////////////////////////

// Mirror's Edge Catalyst: OFFSET_CAMERAUPDATE is called once per frame per
// render view. After the game has written its own camera, the free camera
// transform is written over the main view (GameRenderer + 0x58).

tCameraUpdate oCameraUpdate = nullptr;
__int64 __fastcall hCameraUpdate(__int64 pRenderView, __int64 a2, __int64 a3, __int64 a4)
{
  __int64 result = oCameraUpdate(pRenderView, a2, a3, a4);

  if (!g_shutdown)
  {
    g_mainHandle->GetCameraManager()->OnCameraUpdate(pRenderView);
    g_mainHandle->GetLightManager()->OnGameUpdate();
  }

  return result;
}

// OFFSET_ZONESTREAMERUPDATE is ZoneStreamerEntity's update: it asks each of
// its streaming foci where it is and loads the city zones around them. The
// free camera's focus is added or removed right before, on the same thread
// as the game's own focus changes.

tZoneStreamerUpdate oZoneStreamerUpdate = nullptr;
__int64 __fastcall hZoneStreamerUpdate(__int64 pZoneStreamer, __int64 a2, __int64 a3, __int64 a4)
{
  if (!g_shutdown)
    g_mainHandle->GetCameraManager()->OnZoneStreamerUpdate(pZoneStreamer);

  return oZoneStreamerUpdate(pZoneStreamer, a2, a3, a4);
}


//////////////////////////
////  VISUALS HOOKS   ////
//////////////////////////

// Like the original tools, overrides are written both before and after the
// game updates its visual environment, so the game's own blending neither
// starts from nor ends up with its own values.

tVisualEnvironmentUpdate oVisualEnvironmentUpdate = nullptr;
__int64 __fastcall hVisualEnvironmentUpdate(__int64 a1, __int64 a2, __int64 a3, __int64 a4)
{
  if (!g_shutdown)
    g_mainHandle->GetVisualsManager()->OnVisualEnvironmentUpdate(true);

  __int64 result = oVisualEnvironmentUpdate(a1, a2, a3, a4);

  if (!g_shutdown)
    g_mainHandle->GetVisualsManager()->OnVisualEnvironmentUpdate(false);

  return result;
}

//////////////////////////
////   INPUT HOOKS    ////
//////////////////////////

// Returning 0 from OFFSET_ENTRYINPUT stops the player from receiving any
// input, which is what "disableAllInput" did in the original tools.

tEntryInput oEntryInput = nullptr;
__int64 __fastcall hEntryInput(__int64 a1, __int64 a2, __int64 a3, __int64 a4)
{
  if (!g_shutdown && (g_mainHandle->GetCameraManager()->IsKbmDisabled() || g_mainHandle->GetUI()->IsEnabled()))
    return 0;

  return oEntryInput(a1, a2, a3, a4);
}

//////////////////////////
////   CURSOR HOOKS   ////
//////////////////////////

// During gameplay the game keeps the cursor clipped and re-centered for
// mouse look. While the tools UI is open those calls are ignored so the
// cursor can move freely over the UI.

tSetCursorPos oSetCursorPos = nullptr;
BOOL WINAPI hSetCursorPos(int x, int y)
{
  if (!g_shutdown && g_mainHandle->GetUI()->IsEnabled())
    return TRUE;

  return oSetCursorPos(x, y);
}

tClipCursor oClipCursor = nullptr;
BOOL WINAPI hClipCursor(const RECT* pRect)
{
  if (!g_shutdown && g_mainHandle->GetUI()->IsEnabled())
    return oClipCursor(nullptr);

  return oClipCursor(pRect);
}

// The game hides the Windows cursor by setting none. While the menu is open
// (and uses the Windows cursor) it gets the arrow instead.
tSetCursor oSetCursor = nullptr;
HCURSOR WINAPI hSetCursor(HCURSOR hCursor)
{
  if (!hCursor && !g_shutdown && g_mainHandle->GetUI()->IsEnabled() && g_mainHandle->GetUI()->UsesWindowsCursor())
  {
    static HCURSOR s_arrow = LoadCursor(NULL, IDC_ARROW);
    hCursor = s_arrow;
  }
  return oSetCursor(hCursor);
}

//////////////////////////
////   OTHER HOOKS    ////
//////////////////////////

// These could be hooks on AI to disable them, object iterators
// to find certain components, something to disable the usual
// outlines on friendly players/characters etc...


/*----------------------------------------------------------------*/

namespace
{
  std::unordered_map<std::string, util::hooks::Hook> m_CreatedHooks;

  // Placeholder reservation next to the game exe, released right before the
  // game hooks are created so MinHook's VirtualAlloc can land in it
  LPVOID m_pReservedHookSpace = nullptr;

  // IDXGISwapChain::Present lives in dxgi.dll and is shared by every
  // swapchain, so creating a throwaway one is enough to find its address.
  __int64 GetPresentAddress()
  {
    WNDCLASSEXA wc{ 0 };
    wc.cbSize = sizeof(WNDCLASSEXA);
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "CT_DummyDx11Window";
    RegisterClassExA(&wc);

    HWND hwnd = CreateWindowA(wc.lpszClassName, "", WS_OVERLAPPEDWINDOW, 0, 0, 100, 100, NULL, NULL, wc.hInstance, NULL);
    if (!hwnd)
    {
      util::log::Error("Failed to create dummy window, GetLastError 0x%X", GetLastError());
      UnregisterClassA(wc.lpszClassName, wc.hInstance);
      return 0;
    }

    DXGI_SWAP_CHAIN_DESC desc{ 0 };
    desc.BufferCount = 1;
    desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.OutputWindow = hwnd;
    desc.SampleDesc.Count = 1;
    desc.Windowed = TRUE;
    desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    IDXGISwapChain* pSwapChain = nullptr;
    ID3D11Device* pDevice = nullptr;
    ID3D11DeviceContext* pContext = nullptr;

    __int64 address = 0;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL, 0, D3D11_SDK_VERSION,
      &desc, &pSwapChain, &pDevice, NULL, &pContext);

    if (SUCCEEDED(hr) && pSwapChain)
      address = (*(__int64**)pSwapChain)[8]; // Present
    else
      util::log::Error("D3D11CreateDeviceAndSwapChain failed, HRESULT 0x%X", hr);

    if (pSwapChain) pSwapChain->Release();
    if (pContext) pContext->Release();
    if (pDevice) pDevice->Release();

    DestroyWindow(hwnd);
    UnregisterClassA(wc.lpszClassName, wc.hInstance);
    return address;
  }
}

// Creates a normal function hook with MinHook,
// which places a jmp instruction at the start of the function.
template <typename T>
static void CreateHook(std::string const& name, __int64 target, PVOID hook, T original)
{
  if (!target)
  {
    util::log::Error("Could not create %s hook, target address is unknown", name.c_str());
    return;
  }

  LPVOID* pOriginal = reinterpret_cast<LPVOID*>(original);
  MH_STATUS result = MH_CreateHook((LPVOID)target, hook, pOriginal);
  if (result != MH_OK)
  {
    util::log::Error("Could not create %s hook. MH_STATUS 0x%X error code 0x%X", name.c_str(), result, GetLastError());
    return;
  }

  result = MH_EnableHook((LPVOID)target);
  if (result != MH_OK)
  {
    util::log::Error("Could not enable %s hook. MH_STATUS 0x%X error code 0x%X", name.c_str(), result, GetLastError());
    return;
  }

  util::hooks::Hook hookInfo{ 0 };
  hookInfo.Address = target;
  hookInfo.Type = util::hooks::HookType::MinHook;
  hookInfo.Enabled = true;

  m_CreatedHooks.emplace(name, hookInfo);
  util::log::Ok("Hooked %s at 0x%I64X", name.c_str(), target);
}

// Write to VTable
static PBYTE WINAPI WriteToVTable(PDWORD64* ppVTable, PVOID hook, SIZE_T iIndex)
{
  DWORD dwOld = 0;
  VirtualProtect((void*)((*ppVTable) + iIndex), sizeof(PDWORD64), PAGE_EXECUTE_READWRITE, &dwOld);

  PBYTE pOrig = ((PBYTE)(*ppVTable)[iIndex]);
  (*ppVTable)[iIndex] = (DWORD64)hook;

  VirtualProtect((void*)((*ppVTable) + iIndex), sizeof(PDWORD64), dwOld, &dwOld);
  return pOrig;
}

// Hooks a function by changing the address at given index
// in the virtual function table.
template <typename T>
static void CreateVTableHook(std::string const& name, PDWORD64* ppVTable, PVOID hook, SIZE_T iIndex, T original)
{
  LPVOID* pOriginal = reinterpret_cast<LPVOID*>(original);
  *pOriginal = reinterpret_cast<LPVOID>(WriteToVTable(ppVTable, hook, iIndex));

  util::hooks::Hook hookInfo{ 0 };
  hookInfo.Address = (__int64)ppVTable;
  hookInfo.Index = iIndex;
  hookInfo.Type = util::hooks::HookType::VTable;
  hookInfo.Original = pOriginal;
  hookInfo.Enabled = true;

  m_CreatedHooks.emplace(name, hookInfo);
}

void util::hooks::ReserveSpaceNearGame()
{
  // MinHook needs its trampolines within reach of a rel32 jump from the
  // hooked function. Mirror's Edge fills the space around its exe with heap
  // allocations soon after start-up, so grab one allocation unit early.
  MODULEINFO info;
  if (!GetModuleInformation(GetCurrentProcess(), g_gameHandle, &info, sizeof(MODULEINFO)))
    return;

  SYSTEM_INFO si;
  GetSystemInfo(&si);
  const ULONG_PTR granularity = si.dwAllocationGranularity;
  const ULONG_PTR imageStart = (ULONG_PTR)info.lpBaseOfDll;
  const ULONG_PTR imageEnd = imageStart + info.SizeOfImage;
  const ULONG_PTR firstAbove = (imageEnd + granularity - 1) & ~(granularity - 1);
  const ULONG_PTR firstBelow = (imageStart & ~(granularity - 1)) - granularity;

  for (ULONG_PTR distance = 0; distance < 0x20000000 && !m_pReservedHookSpace; distance += granularity)
  {
    m_pReservedHookSpace = VirtualAlloc((LPVOID)(firstAbove + distance), granularity, MEM_RESERVE, PAGE_NOACCESS);
    if (!m_pReservedHookSpace && firstBelow > distance)
      m_pReservedHookSpace = VirtualAlloc((LPVOID)(firstBelow - distance), granularity, MEM_RESERVE, PAGE_NOACCESS);
  }

  if (m_pReservedHookSpace)
    util::log::Write("Reserved hook memory at 0x%I64X", m_pReservedHookSpace);
  else
    util::log::Warning("No free memory next to the game exe, hooks will look further away");
}

void util::hooks::Init()
{
  MH_STATUS status = MH_Initialize();
  if (status != MH_OK)
  {
    util::log::Error("Failed to initialize MinHook, MH_STATUS 0x%X", status);
    return;
  }

  CreateHook("SwapChainPresent", GetPresentAddress(), hIDXGISwapChain_Present, &oIDXGISwapChain_Present);

  HMODULE hUser32 = GetModuleHandleA("user32.dll");
  CreateHook("SetCursorPos", (__int64)GetProcAddress(hUser32, "SetCursorPos"), hSetCursorPos, &oSetCursorPos);
  CreateHook("ClipCursor", (__int64)GetProcAddress(hUser32, "ClipCursor"), hClipCursor, &oClipCursor);
  CreateHook("SetCursor", (__int64)GetProcAddress(hUser32, "SetCursor"), hSetCursor, &oSetCursor);

  // Free the placeholder so MinHook can put the game hooks' trampolines there
  if (m_pReservedHookSpace)
  {
    VirtualFree(m_pReservedHookSpace, 0, MEM_RELEASE);
    m_pReservedHookSpace = nullptr;
  }

  CreateHook("CameraUpdate", util::offsets::GetOffset("OFFSET_CAMERAUPDATE"), hCameraUpdate, &oCameraUpdate);
  CreateHook("EntryInput", util::offsets::GetOffset("OFFSET_ENTRYINPUT"), hEntryInput, &oEntryInput);
  CreateHook("VisualEnvironmentUpdate", util::offsets::GetOffset("OFFSET_VISUALENVUPDATE"), hVisualEnvironmentUpdate, &oVisualEnvironmentUpdate);
  CreateHook("ZoneStreamerUpdate", util::offsets::GetOffset("OFFSET_ZONESTREAMERUPDATE"), hZoneStreamerUpdate, &oZoneStreamerUpdate);
}

// In some cases it's useful or even required to disable all hooks or just certain ones
void util::hooks::SetHookState(bool enable, std::string const& name)
{
  if (name.empty())
  {
    MH_STATUS status = enable ? MH_EnableHook(MH_ALL_HOOKS) : MH_DisableHook(MH_ALL_HOOKS);
    if (status != MH_OK)
      util::log::Error("MinHook failed to %s all hooks, MH_STATUS 0x%X", (enable ? "enable" : "disable"), status);

    for (auto& entry : m_CreatedHooks)
    {
      Hook& hook = entry.second;
      if (hook.Type == HookType::MinHook) continue;
      if (hook.Enabled != enable)
      {
        *hook.Original = WriteToVTable((PDWORD64*)hook.Address, *hook.Original, hook.Index);
        hook.Enabled = enable;
      }
      else
        util::log::Warning("VTable hook %s is already %s", entry.first.c_str(), enable ? "enabled" : "disabled");
    }
  }
  else
  {
    auto result = m_CreatedHooks.find(name);
    if (result == m_CreatedHooks.end())
    {
      util::log::Error("Hook %s does not exit", name.c_str());
      return;
    }

    Hook& hook = result->second;
    if (hook.Type == HookType::MinHook)
    {
      MH_STATUS status = enable ? MH_EnableHook((LPVOID)hook.Address) : MH_DisableHook((LPVOID)hook.Address);
      if (status != MH_OK)
        util::log::Error("MinHook failed to %s hook %s, MH_STATUS 0x%X", (enable ? "enable" : "disable"), name.c_str(), status);
    }
    else
    {
      if (hook.Enabled != enable)
      {
        *hook.Original = WriteToVTable((PDWORD64*)hook.Address, *hook.Original, hook.Index);
        hook.Enabled = enable;
      }
      else
        util::log::Warning("VTable hook %s is already %s", name.c_str(), enable ? "enabled" : "disabled");
    }
  }
}
