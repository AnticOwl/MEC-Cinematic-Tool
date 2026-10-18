// usp10.dll proxy that loads CT_MirrorsEdge.dll into Mirror's Edge Catalyst.
//
// The game imports USP10.dll, and nothing loads it before the game's own
// imports are resolved, so the copy next to MirrorsEdgeCatalyst.exe wins and
// the tools get loaded at start-up without an injector. (version.dll and
// winmm.dll don't work: Steam's and EA's DLLs load the system copies first.)
//
// Every export is forwarded to GDI32 through usp10.def, the same way the
// system usp10.dll forwards them on Windows 10 and 11.

#include <Windows.h>
#include <cwchar>

namespace
{
  HMODULE g_self = NULL;

  DWORD WINAPI LoadTools(LPVOID)
  {
    wchar_t path[MAX_PATH];
    DWORD length = GetModuleFileNameW(g_self, path, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) return 0;

    wchar_t* slash = wcsrchr(path, L'\\');
    if (!slash) return 0;
    *(slash + 1) = 0;
    wcscat_s(path, L"CT_MirrorsEdge.dll");

    if (!LoadLibraryW(path))
    {
      wchar_t message[MAX_PATH + 128];
      swprintf_s(message, L"Couldn't load the cinematic tools:\n%s\n\nError %lu", path, GetLastError());
      MessageBoxW(NULL, message, L"Cinematic Tools loader", MB_OK | MB_ICONWARNING);
    }
    return 0;
  }

  bool IsGameProcess()
  {
    wchar_t exe[MAX_PATH];
    DWORD length = GetModuleFileNameW(NULL, exe, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) return false;
    _wcslwr_s(exe);
    return wcsstr(exe, L"mirrorsedgecatalyst") != nullptr;
  }
}

BOOL WINAPI DllMain(HINSTANCE hInstance, DWORD reason, LPVOID)
{
  if (reason == DLL_PROCESS_ATTACH)
  {
    g_self = hInstance;
    DisableThreadLibraryCalls(hInstance);

    // Loading another DLL isn't allowed from DllMain, so it happens on a
    // thread that starts once the loader is done with us
    if (IsGameProcess())
    {
      HANDLE thread = CreateThread(NULL, 0, LoadTools, NULL, 0, NULL);
      if (thread) CloseHandle(thread);
    }
  }
  return TRUE;
}
