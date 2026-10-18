#include "Main.h"
#include <cstdio>

DWORD WINAPI RunCT(LPVOID lpArg)
{
  g_mainHandle = new Main();
  if (g_mainHandle->Initialize())
    g_mainHandle->Run();

  delete g_mainHandle;

  return 0;
}

DWORD WINAPI DllMain(_In_ HINSTANCE hInstance, _In_ DWORD fdwReason, _In_ LPVOID lpvReserved)
{
  if (fdwReason == DLL_PROCESS_ATTACH)
  {
    g_dllHandle = hInstance;

    // Only one copy of the tools per game process, e.g. when both the
    // version.dll loader and an injector load it (from different folders)
    char mutexName[64];
    sprintf_s(mutexName, "CT_MirrorsEdge_%lu", GetCurrentProcessId());
    HANDLE instanceMutex = CreateMutexA(NULL, FALSE, mutexName);
    if (!instanceMutex || GetLastError() == ERROR_ALREADY_EXISTS)
      return 1;

    CreateThread(NULL, NULL, RunCT, NULL, NULL, NULL);
  }

  return 1;
}
