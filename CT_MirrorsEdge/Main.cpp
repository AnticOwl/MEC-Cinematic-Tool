#include "Main.h"
#include "Util/Util.h"
#include "Visuals/Panini.h"
#include "Visuals/LensDistortion.h"
#include "Visuals/FrameShot.h"
#include "Visuals/GreenPlane.h"
#include "Visuals/GameFrame.h"
#include "Visuals/WetLook.h"
#include "Visuals/WorldImages.h"
#include "Camera/FaithOutfit.h"
#include "Camera/FaithSize.h"
#include "Camera/GroundLevel.h"
#include "Camera/RoadTraffic.h"
#include "Camera/CullingOff.h"
#include "Camera/ReplayAudio.h"
#include "Camera/ReplayShapes.h"
#include "Camera/ReplayAssetLog.h"
#include "Camera/ReplayDrawLog.h"
#include "Camera/ReplayGameDraws.h"
#include "Camera/ReplayStore.h"
#include "Camera/PoseEditor.h"
#include "Util/Perf.h"
#include "Util/GameState.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <d3d11.h>
#include <fstream>
#include <psapi.h>
#include <stdarg.h>
#include <tlhelp32.h>
#include <csignal>
#include <intrin.h>

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

  // The Windows cursor over the menu. The display count belongs to the
  // window's thread, so this runs from the window procedure. With ShowCursor
  // hooked the game's own count is kept apart while the menu is open (see
  // HoldCursorShown); the old way, ShowCursor(TRUE) steps undone when the menu
  // closes, is only for a game where the hook couldn't be made. Counting
  // steps against the game's own (it sets the count to 2 or -2 in up to 64
  // steps) left the count far below the game's after the menu, and the
  // cursor invisible; steps taken while another window was in front (the
  // cursor there hidden) made it worse.
  int g_cursorShowCount = 0;

  void UpdateCursorVisibility()
  {
    UI* pUI = g_mainHandle->GetUI();
    bool show = pUI->IsEnabled() && pUI->UsesWindowsCursor();
    bool foreground = GetForegroundWindow() == g_gameHwnd;
    bool hooked = util::hooks::HoldCursorShown(show);
    if (show && foreground)
    {
      CURSORINFO info{};
      info.cbSize = sizeof(info);
      if (GetCursorInfo(&info))
      {
        if (!hooked && !(info.flags & CURSOR_SHOWING) && g_cursorShowCount < 64)
        {
          ShowCursor(TRUE);
          g_cursorShowCount++;
        }
        if (!info.hCursor)
          SetCursor(LoadCursor(NULL, IDC_ARROW));
      }
    }
    else if (!show)
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

  // A code address as module+offset, in any module; executable memory in no
  // module (hook relays and trampolines) as ?address. False if it isn't code.
  bool DescribeCode(__int64 address, char* out, size_t size)
  {
    MEMORY_BASIC_INFORMATION mbi{};
    if (address < 0x10000 || !VirtualQuery((void const*)address, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT)
      return false;
    if (!(mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)))
      return false;
    HMODULE hModule = NULL;
    char module[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
      (LPCSTR)address, &hModule) && hModule)
    {
      GetModuleFileNameA(hModule, module, MAX_PATH);
      char* pName = strrchr(module, '\\');
      sprintf_s(out, size, "%s+0x%I64X", pName ? pName + 1 : module, address - (__int64)hModule);
    }
    else
      sprintf_s(out, size, "?%I64X", address);
    return true;
  }

  // One step up the call chain with the modules' own unwind data (.pdata, the
  // game ships its own): the caller's registers in place, so each frame is a
  // real caller. The stack scans below list any code address on the stack,
  // values left behind by earlier calls too (2026-11-06 22:28: the tools'
  // drawing code under GridNode's crash). A function with no unwind data is
  // a leaf only for the crash's own frame (its caller at rsp); past that there's
  // nothing to go by (a hook's relay, a trampoline, a cave): false, as for a
  // stack that can't be read. RtlLookupFunctionEntry is safe here, on the
  // crashing thread with the others running.
  bool UnwindStep(CONTEXT& context, bool first)
  {
    __try
    {
      DWORD64 imageBase = 0;
      PRUNTIME_FUNCTION pFunction = RtlLookupFunctionEntry(context.Rip, &imageBase, nullptr);
      if (!pFunction)
      {
        if (!first)
          return false;
        context.Rip = *(DWORD64 const*)context.Rsp;
        context.Rsp += 8;
        return true;
      }
      PVOID handlerData = nullptr;
      DWORD64 establisher = 0;
      RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, context.Rip, pFunction, &context, &handlerData, &establisher, nullptr);
      return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      return false;
    }
  }

  // The call chain from the crash, as far as the unwind data goes
  void NoteCallChain(CONTEXT const* pContext)
  {
    const int MaxFrames = 40;
    CONTEXT context = *pContext;
    char text[4096];
    int length = sprintf_s(text, "[CRASH] call chain (unwound, each a real caller):");
    int frames = 0;
    char const* stop = "";
    char where[MAX_PATH + 32];
    while (frames < MaxFrames && length > 0 && length < (int)sizeof(text) - 2 * MAX_PATH)
    {
      if (!DescribeCode((__int64)context.Rip, where, sizeof(where)))
      {
        stop = frames ? " (ends: not code)" : "";
        break;
      }
      length += sprintf_s(text + length, sizeof(text) - length, " %d: %s", frames, where);
      ++frames;
      DWORD64 rsp = context.Rsp;
      if (!UnwindStep(context, frames == 1))
      {
        stop = " (ends: no unwind data past here, or the stack can't be read)";
        break;
      }
      if (!context.Rip)
      {
        stop = " (the thread's start)";
        break;
      }
      if (context.Rsp <= rsp)
      {
        stop = " (ends: the stack didn't move on)";
        break;
      }
    }
    if (length > 0 && length < (int)sizeof(text) - 96)
      sprintf_s(text + length, sizeof(text) - length, "%s", frames >= MaxFrames ? " ..." : stop);
    util::log::CrashNote(text);
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

        // Who called whom (the scans below also show what's left on the stack)
        NoteCallChain(c);

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

        // Every module's code the stack goes back to, with where on the stack
        // (slot 0 is what called the crash address when that was a call): which
        // of the mods hooking the same function a bad call came from
        // (2026-10-23: a call to 0 in Present with MECMP, Catalyst Clarity and
        // the tools all on it)
        char other[3072];
        int otherLength = sprintf_s(other, "[CRASH] code on the stack (slot: where):");
        found = 0;
        for (int i = 0; i < 512 && found < 24 && otherLength > 0 && otherLength < (int)sizeof(other) - 96; ++i)
        {
          __int64 value = 0;
          if (!ReadQwordSafely((__int64)c->Rsp + i * 8, value))
            break;
          char where[MAX_PATH + 32];
          if (DescribeCode(value, where, sizeof(where)))
          {
            otherLength += sprintf_s(other + otherLength, sizeof(other) - otherLength, " %d: %s", i, where);
            ++found;
          }
        }
        util::log::CrashNote(other);
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

  // What the tools' own C runtime ends the game for goes past the crash filter: it is a "fail fast", no exception a
  // filter is shown. A failed ImGui check (IM_ASSERT is compiled in: imconfig.h), abort() from anything else, and a
  // secure CRT call that refuses its arguments (a text longer than its buffer in sprintf_s or strcpy_s) all end the
  // game with Windows' code 0xC0000409 and, until now, nothing in CT.log (2026-11-12: the game ended at ImGui's
  // check of a window's stacks; only Windows' crash dump said where). So each writes its own [CRASH] lines first:
  // what, the call chain, the tools' state. The game then ends exactly as it did before.
  volatile LONG g_runtimeEndNoted = 0;

  void NoteRuntimeEnd(char const* what)
  {
    if (InterlockedExchange(&g_runtimeEndNoted, 1) != 0)
      return;
    util::log::CrashNote(what);
    CONTEXT context;
    RtlCaptureContext(&context);
    NoteCallChain(&context);
    if (g_mainHandle && g_mainHandle->GetCameraManager())
    {
      char state[1024], text[1100];
      g_mainHandle->GetCameraManager()->DescribeState(state, sizeof(state));
      _snprintf_s(text, sizeof(text), _TRUNCATE, "[CRASH] Tools at the time: %s", state);
      util::log::CrashNote(text);
    }
  }

  // abort() of the tools' C runtime (theirs alone: it is linked into the DLL), before it ends the process
  void __cdecl OnRuntimeAbort(int)
  {
    NoteRuntimeEnd("[CRASH] The tools' C runtime ends the game with abort(): a failed check, or a C++ exception nothing "
      "caught");
  }

  // A call the tools' C runtime refuses. Its own handler ends the process the same way (no details in a release build)
  void __cdecl OnRuntimeRefusal(wchar_t const*, wchar_t const*, wchar_t const*, unsigned int, uintptr_t)
  {
    NoteRuntimeEnd("[CRASH] The tools' C runtime refused a call's arguments and ends the game: most often a text longer "
      "than its buffer (sprintf_s, strcpy_s and the like)");
    __fastfail(FAST_FAIL_INVALID_ARG);
  }
}

// ImGui's failed check (imconfig.h's IM_ASSERT): written to CT.log, then assert()'s own failure as before. In the
// game that is the C runtime's Abort / Retry / Ignore message box (the tools' runtime doesn't know of a console);
// Abort ends the game through abort(), Ignore comes back here and the game goes on.
void CtImGuiCheckFailed(const wchar_t* expression, unsigned line)
{
  char text[768];
  _snprintf_s(text, sizeof(text), _TRUNCATE, "[CRASH] A check inside the menu's library (ImGui) failed: %ls (imgui line %u). "
    "The C runtime's Abort / Retry / Ignore box follows; Abort ends the game", expression ? expression : L"?", line);
  NoteRuntimeEnd(text);
  _wassert(expression ? expression : L"?", L"imgui", line);
  // Back here only after Ignore (or Retry under a debugger): the next end is to be written again
  util::log::CrashNote("[CRASH] That box was answered with Ignore (or Retry under a debugger): the game goes on");
  InterlockedExchange(&g_runtimeEndNoted, 0);
}

namespace
{
  // The hang watch (2026-10-27, the user: the game doesn't close after
  // quitting; Windows closed its frozen window at 01:07 and 02:59). CT.log
  // said nothing about a session's end, so where the game waits was unknown.
  // This writes where each of the game's threads is, to a file in the tools'
  // folder, when the game's window hasn't answered Windows for about
  // HangSeconds (Windows calls it "not responding" after 5), again once if it
  // still hasn't HangAgainSeconds later (to compare), and when the game still
  // runs ExitSeconds after its window closed. Read only: each thread's
  // registers (no thread is suspended) and the code addresses on its stack.
  // No lock is waited for (the stuck thread may hold one), nothing allocated.
  // 15:45 the same day, the new lines showed what a quit that doesn't end the
  // game is: the game took its level out and went on running frames (its
  // window answering) until the user closed the window 3 minutes later, which
  // froze it. A quit that works ends the process ~5 s after the level goes,
  // without closing the window (14:41). So the watch also writes it all, with
  // the level streaming's state, LevelGoneSeconds after Faith went with no
  // loading screen up, and CloseSeconds after the window was asked to close
  // if it still hasn't.
  const int HangSeconds = 15;
  const int HangAgainSeconds = 30;
  const int ExitSeconds = 20;
  const int LevelGoneSeconds = 20;
  const int CloseSeconds = 2;
  const int QuitWaitInPlaySeconds = 120;
  const int MaxReports = 6;          // a session
  // A stuck quit is ended by the tools (see HangWatch): its server waiting on
  // the same unload ticket this long, or this long once the window was asked
  // to close (it freezes then, and Windows ends it with exit code 1 a few
  // seconds later: 2026-10-29 02:12, before the first version's 15 s), or a
  // quit not done this long for any reason. A quit that works ends the game
  // in about 5 s. What the quit is at goes in the log every few seconds.
  const int StuckQuitSeconds = 8;
  const int StuckAfterCloseSeconds = 1;
  const int QuitMaxSeconds = 30;
  const int QuitLogEverySeconds = 5;

  std::atomic<ULONGLONG> g_closeAskedAt{ 0 };   // WM_CLOSE's GetTickCount64

  struct WatchModule
  {
    __int64 Base, End;
    __int64 CodeStart[8], CodeEnd[8];
    int Codes;
    char Name[64];
  };
  WatchModule g_watchModules[512];
  int g_watchModuleCount = 0;

  typedef LONG(NTAPI* tNtQueryInformationThread)(HANDLE, int, PVOID, ULONG, PULONG);
  tNtQueryInformationThread g_ntQueryInformationThread = nullptr;

  // The modules and their code sections, read from the loader's list the way
  // another process would (no loader lock)
  void ReadWatchModules()
  {
    HMODULE modules[512];
    DWORD needed = 0;
    g_watchModuleCount = 0;
    if (!K32EnumProcessModules(GetCurrentProcess(), modules, sizeof(modules), &needed))
      return;
    int count = (int)(needed / sizeof(HMODULE));
    for (int i = 0; i < count && i < 512; ++i)
    {
      MODULEINFO info{};
      if (!K32GetModuleInformation(GetCurrentProcess(), modules[i], &info, sizeof(info)))
        continue;
      WatchModule& m = g_watchModules[g_watchModuleCount];
      m.Base = (__int64)info.lpBaseOfDll;
      m.End = m.Base + info.SizeOfImage;
      m.Codes = 0;
      if (!K32GetModuleBaseNameA(GetCurrentProcess(), modules[i], m.Name, sizeof(m.Name)))
        strcpy_s(m.Name, "?");
      __try
      {
        IMAGE_DOS_HEADER* pDos = (IMAGE_DOS_HEADER*)m.Base;
        IMAGE_NT_HEADERS* pNt = (IMAGE_NT_HEADERS*)(m.Base + pDos->e_lfanew);
        IMAGE_SECTION_HEADER* pSection = IMAGE_FIRST_SECTION(pNt);
        for (int s = 0; s < pNt->FileHeader.NumberOfSections && m.Codes < 8; ++s, ++pSection)
          if (pSection->Characteristics & IMAGE_SCN_MEM_EXECUTE)
          {
            m.CodeStart[m.Codes] = m.Base + pSection->VirtualAddress;
            m.CodeEnd[m.Codes] = m.CodeStart[m.Codes] + pSection->Misc.VirtualSize;
            ++m.Codes;
          }
      }
      __except (EXCEPTION_EXECUTE_HANDLER)
      {
        // No sections known: any address inside it counts
        m.Codes = 1;
        m.CodeStart[0] = m.Base;
        m.CodeEnd[0] = m.End;
      }
      ++g_watchModuleCount;
    }
  }

  // name+offset when the address is in a module's code, else false
  bool WatchCode(__int64 address, char* out, size_t size)
  {
    for (int i = 0; i < g_watchModuleCount; ++i)
    {
      WatchModule const& m = g_watchModules[i];
      if (address < m.Base || address >= m.End)
        continue;
      for (int s = 0; s < m.Codes; ++s)
        if (address >= m.CodeStart[s] && address < m.CodeEnd[s])
        {
          sprintf_s(out, size, "%s+0x%I64X", m.Name, address - m.Base);
          return true;
        }
      return false;
    }
    return false;
  }

  void Put(HANDLE file, const char* format, ...)
  {
    char text[1024];
    va_list args;
    va_start(args, format);
    int length = vsprintf_s(text, format, args);
    va_end(args);
    DWORD written = 0;
    if (length > 0)
      WriteFile(file, text, (DWORD)length, &written, NULL);
  }

  // Where the window's thread is, for CT.log: the first code addresses on its stack
  char g_windowThreadWhere[600];

  // One file with every thread: the address it's at, how much processor time
  // it has used, where it started, and the code addresses on its stack
  // (slot: where; slot 0 is at its stack pointer). Returns false if it
  // couldn't be written.
  bool WriteThreadReport(const char* why, const char* kind, DWORD windowThread, char* fileName, size_t fileNameSize)
  {
    SYSTEMTIME now;
    GetLocalTime(&now);
    sprintf_s(fileName, fileNameSize, ".\\Cinematic Tools\\%s_%04d-%02d-%02d_%02d%02d%02d.txt", kind,
      now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond);
    HANDLE file = CreateFileA(fileName, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE)
      return false;

    ReadWatchModules();
    Put(file, "Cinematic Tools hang watch, %02d:%02d:%02d: %s\r\n", now.wHour, now.wMinute, now.wSecond, why);
    Put(file, "Game process %lu, the window's thread %lu, this (the watch's) thread %lu\r\n\r\n",
      GetCurrentProcessId(), windowThread, GetCurrentThreadId());

    g_windowThreadWhere[0] = 0;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot != INVALID_HANDLE_VALUE)
    {
      THREADENTRY32 entry{};
      entry.dwSize = sizeof(entry);
      for (BOOL more = Thread32First(snapshot, &entry); more; more = Thread32Next(snapshot, &entry))
      {
        if (entry.th32OwnerProcessID != GetCurrentProcessId() || entry.th32ThreadID == GetCurrentThreadId())
          continue;
        HANDLE thread = OpenThread(THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, entry.th32ThreadID);
        if (!thread)
        {
          Put(file, "thread %lu: couldn't be opened (%lu)\r\n", entry.th32ThreadID, GetLastError());
          continue;
        }
        FILETIME created, exited, kernel, user;
        unsigned long long cpuMs = 0;
        if (GetThreadTimes(thread, &created, &exited, &kernel, &user))
          cpuMs = (((unsigned long long)kernel.dwHighDateTime << 32 | kernel.dwLowDateTime) +
            ((unsigned long long)user.dwHighDateTime << 32 | user.dwLowDateTime)) / 10000;
        __int64 start = 0;
        if (g_ntQueryInformationThread)
          g_ntQueryInformationThread(thread, 9, &start, sizeof(start), NULL);   // ThreadQuerySetWin32StartAddress
        char startText[96] = "?";
        if (!WatchCode(start, startText, sizeof(startText)))
          sprintf_s(startText, "0x%I64X", start);

        CONTEXT context{};
        context.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
        bool haveContext = GetThreadContext(thread, &context) != 0;
        DWORD contextError = haveContext ? 0 : GetLastError();
        CloseHandle(thread);
        bool isWindow = entry.th32ThreadID == windowThread;
        if (!haveContext)
        {
          Put(file, "thread %lu%s: cpu %llu ms, started at %s, its registers couldn't be read (%lu)\r\n",
            entry.th32ThreadID, isWindow ? " (the game's window)" : "", cpuMs, startText, contextError);
          continue;
        }
        char ripText[96];
        if (!WatchCode((__int64)context.Rip, ripText, sizeof(ripText)))
          sprintf_s(ripText, "0x%I64X", (__int64)context.Rip);
        Put(file, "thread %lu%s: cpu %llu ms, started at %s, at %s, rsp 0x%I64X\r\n ",
          entry.th32ThreadID, isWindow ? " (the game's window)" : "", cpuMs, startText, ripText, (__int64)context.Rsp);

        int whereLength = 0;
        if (isWindow)
          whereLength = sprintf_s(g_windowThreadWhere, "at %s; on its stack:", ripText);
        int found = 0;
        for (int i = 0; i < 4096 && found < 48; ++i)
        {
          __int64 value = 0;
          if (!ReadQwordSafely((__int64)context.Rsp + i * 8, value))
            break;
          char where[96];
          if (!WatchCode(value, where, sizeof(where)))
            continue;
          Put(file, " %d: %s", i, where);
          if (isWindow && found < 12 && whereLength > 0 && whereLength < (int)sizeof(g_windowThreadWhere) - 100)
            whereLength += sprintf_s(g_windowThreadWhere + whereLength, sizeof(g_windowThreadWhere) - whereLength, " %s", where);
          ++found;
        }
        Put(file, "\r\n");
      }
      CloseHandle(snapshot);
    }

    Put(file, "\r\nModules:\r\n");
    for (int i = 0; i < g_watchModuleCount; ++i)
      Put(file, "  %016I64X %08I64X %s\r\n", g_watchModules[i].Base, g_watchModules[i].End - g_watchModules[i].Base,
        g_watchModules[i].Name);
    CloseHandle(file);
    return true;
  }

  // What the game's quit waits for, read offline from its frame function
  // 0x142A2D470 (slot 0xB8 of Main's vtable; its main loop 0x142A29220 runs
  // it until it returns false, then posts WM_QUIT): once the quit is asked
  // ([[Main + 0x288] + 8] 1 or 2, see 0x142A31670), it goes on returning true
  // until [Main + 0x48] is 0, the client's state [[Main + 0x50] + 0x318] is 16
  // (0x14319C9A0) and each object listed from [Main + 0x160] to
  // [Main + 0x168] answers true to its slot 0x60. Read only; logs the values,
  // and each listed object's vtable and slot 0x60, to find the one that stops
  // a quit (2026-10-27 17:55: the level streaming was empty then)
  // 20:07 the same day, read live in a stuck quit: [Main + 0x48] is the
  // game's server (vtable 0x141B42EE0), still there. Its shutdown step
  // (0x143344D90) only lets it go idle, which ends the quit, once
  // 0x1432EFD20 is done: that waits on a resource manager ticket at
  // [server + 0x10] (made by 0x142ABA6B0 with kind 3; state [server + 0x18])
  // through 0x142ABB590 -> 0x142AC2C50 -> 0x142AC4FE0: the manager
  // [0x1423A4228], the ticket's queue at manager + 0x4400 + ((t >> 16) & 0xF)
  // * 0xA80, done when the queue's counter (word + 0x170 + ((t >> 20) & 0xF)
  // * 2) has reached its low 16 bits. Its counter stood one short (0x31B for
  // 0x31C), and the operation (the queue's map + 0x38 / + 0x40 by key, the
  // node's + 8) was named "...carusRunnersLair" (16 characters at + 0x30)
  struct QuitWait
  {
    bool Read, FrameKnown;
    __int64 Main, Server, Client;
    int QuitState, ClientState, Listed;
    __int64 VTables[24], Slots60[24];
    unsigned int Ticket;
    int TicketState, QueueDone;
    char Operation[17];
  };

  void ReadQuitWait(QuitWait& w)
  {
    memset(&w, 0, sizeof(w));
    __int64 base = (__int64)g_gameHandle;
    __try
    {
      w.Main = *(__int64 const*)(base + 0x2142A48);
      if (w.Main < 0x10000)
        return;
      // Slot 0xB8 is a thunk, jmp 0x142A2D470 (E9 rel32), at 0x142A31650
      __int64 slot = *(__int64 const*)(*(__int64 const*)w.Main + 0xB8);
      w.FrameKnown = slot == base + 0x2A2D470 || (slot == base + 0x2A31650 && *(BYTE const*)slot == 0xE9 &&
        slot + 5 + *(int const*)(slot + 1) == base + 0x2A2D470);
      __int64 quit = *(__int64 const*)(w.Main + 0x288);
      w.QuitState = quit >= 0x10000 ? *(int const*)(quit + 8) : -1;
      w.Server = *(__int64 const*)(w.Main + 0x48);
      w.Client = *(__int64 const*)(w.Main + 0x50);
      w.ClientState = w.Client >= 0x10000 ? *(int const*)(w.Client + 0x318) : -1;
      __int64 begin = *(__int64 const*)(w.Main + 0x160), end = *(__int64 const*)(w.Main + 0x168);
      for (__int64 p = begin; p && p < end && w.Listed < 24; p += 8)
      {
        __int64 object = *(__int64 const*)p;
        __int64 vtable = object >= 0x10000 ? *(__int64 const*)object : 0;
        w.VTables[w.Listed] = vtable;
        w.Slots60[w.Listed] = vtable >= 0x10000 ? *(__int64 const*)(vtable + 0x60) : 0;
        ++w.Listed;
      }
      w.QueueDone = -1;
      if (w.Server >= 0x10000)
      {
        w.Ticket = *(unsigned int const*)(w.Server + 0x10);
        w.TicketState = *(int const*)(w.Server + 0x18);
        __int64 manager = *(__int64 const*)(base + 0x23A4228);
        unsigned int t = w.Ticket & 0xFFFFFF;
        if (manager >= 0x10000 && t)
        {
          __int64 queue = manager + 0x4400 + ((t >> 16) & 0xF) * 0xA80;
          w.QueueDone = *(unsigned short const*)(queue + 0x170 + ((t >> 20) & 0xF) * 2);
          unsigned int key = (((t >> 20) & 0xF) << 20) | (t & 0xFFFF);
          unsigned int buckets = *(unsigned int const*)(queue + 0x40);
          __int64 table = *(__int64 const*)(queue + 0x38);
          __int64 node = buckets && table >= 0x10000 ? *(__int64 const*)(table + (key % buckets) * 8) : 0;
          for (int i = 0; node >= 0x10000 && i < 256 && *(unsigned int const*)node != key; ++i)
            node = *(__int64 const*)(node + 0x10);
          __int64 operation = node >= 0x10000 ? *(__int64 const*)(node + 8) : 0;
          if (operation >= 0x10000)
          {
            memcpy(w.Operation, (void const*)(operation + 0x30), 16);
            for (int i = 0; i < 16; ++i)
              if (w.Operation[i] && (w.Operation[i] < 0x20 || w.Operation[i] > 0x7E))
                w.Operation[i] = '?';
          }
        }
      }
      w.Read = true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
  }

  void LogQuitWait(const char* when)
  {
    QuitWait w;
    ReadQuitWait(w);
    if (!w.Read)
    {
      util::log::Write("Exit (diagnostic): what the game's quit waits for couldn't be read (%s)", when);
      return;
    }
    __int64 base = (__int64)g_gameHandle;
    char list[24 * 40] = "";
    int length = 0;
    for (int i = 0; i < w.Listed && length < (int)sizeof(list) - 40; ++i)
      length += sprintf_s(list + length, sizeof(list) - length, " %I64X/%I64X", w.VTables[i] - base, w.Slots60[i] - base);
    util::log::Write("Exit (diagnostic): what the game's quit waits for (%s; frame function %s): the quit [[Main + 0x288] + 8] = %d "
      "(0 none, 1 asked, 2 quitting), [Main + 0x48] = 0x%I64X (the server, needs 0), the client's state [[Main + 0x50] + 0x318] = %d "
      "(needs 16), %d objects that must answer their slot 0x60 (vtable/slot RVAs):%s", when,
      w.FrameKnown ? "as known" : "NOT as known", w.QuitState, w.Server, w.ClientState, w.Listed, list);
    if (w.Server)
      util::log::Write("Exit (diagnostic): the server's shutdown waits for the resource manager's ticket 0x%08X (its state %d); "
        "that queue has done up to 0x%X; the operation: '%s'", w.Ticket, w.TicketState, w.QueueDone, w.Operation);
  }

  // The hang reports (Hang_*.txt) but the newest HangReportsKept deleted, once
  // at the start (nothing else deletes them). The other kinds are left alone
  const int HangReportsKept = 10;

  void DeleteOldHangReports()
  {
    struct Report
    {
      FILETIME Written;
      char Name[MAX_PATH];
    };
    std::vector<Report> found;
    WIN32_FIND_DATAA data;
    HANDLE find = FindFirstFileA("Cinematic Tools\\Hang_*.txt", &data);
    if (find == INVALID_HANDLE_VALUE)
      return;
    do
    {
      // The pattern can match a short (8.3) name too: the long name checked again
      size_t length = strlen(data.cFileName);
      if ((data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || length < 9 || strncmp(data.cFileName, "Hang_", 5) != 0 ||
        strcmp(data.cFileName + length - 4, ".txt") != 0)
        continue;
      Report report;
      report.Written = data.ftLastWriteTime;
      strcpy_s(report.Name, data.cFileName);
      found.push_back(report);
    } while (FindNextFileA(find, &data));
    FindClose(find);
    if ((int)found.size() <= HangReportsKept)
      return;
    // The newest first
    std::sort(found.begin(), found.end(), [](Report const& a, Report const& b)
      { return CompareFileTime(&a.Written, &b.Written) > 0; });
    for (size_t i = HangReportsKept; i < found.size(); ++i)
    {
      char path[MAX_PATH + 32];
      sprintf_s(path, "Cinematic Tools\\%s", found[i].Name);
      DeleteFileA(path);
    }
  }

  DWORD WINAPI HangWatch(LPVOID)
  {
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (ntdll)
      g_ntQueryInformationThread = (tNtQueryInformationThread)GetProcAddress(ntdll, "NtQueryInformationThread");
    DeleteOldHangReports();

    int reports = 0;
    int hungFor = 0;        // seconds Windows has called the window hung (not answering for 5 s before that)
    int closedFor = 0;      // seconds since the window closed
    ULONGLONG closeReported = 0, goneReported = 0;   // the WM_CLOSE and the level going last reported
    DWORD windowThread = 0;
    int windowSeconds = 0;  // since the window was known
    unsigned int stuckTicket = 0;   // the server's ticket a quit is seen stuck on
    int stuckFor = 0;               // seconds since
    int quitFor = 0;                // seconds the quit has been asked
    for (;;)
    {
      Sleep(1000);
      HWND hwnd = g_gameHwnd;
      if (!hwnd)
        continue;
      if (!windowThread)
        windowThread = GetWindowThreadProcessId(hwnd, NULL);
      // Once in play too, to compare with a stuck quit's (diagnostics only)
      if (util::DiagnosticsOn() && ++windowSeconds == QuitWaitInPlaySeconds)
        LogQuitWait("in play, for comparison");

      // A stuck quit ended (2026-10-28, the user: "make the tools end the game
      // when its quit is stuck (quitting, level gone, server waiting on the
      // level unload for 15 s), the same way the game's own watchdog does").
      // Seen in 7 quits on 10-27/28: the quit asked ([[Main + 0x288] + 8] 2),
      // Faith's level gone, no loading screen, and the server's shutdown
      // waiting on the same resource manager ticket (the Game compartment's
      // unload) with its queue one short of it, for minutes, until the window
      // was closed. A quit that works is past that in a second or two (the
      // ticket goes back to 0). The game's own watchdog (0x14344B6E6) ends a
      // wait that takes too long with exit code 42: so does this.
      // 2026-11-01: Faith's level and the loading screen are no longer asked
      // about (the camera update that notes Faith may stop with the quit);
      // [[Main + 0x288] + 8] not 0 is the engine ending its main loop (the
      // frame function returns false then, once the server is gone)
      util::hooks::KeepBodyHookOffForMecmp();
      {
        QuitWait w;
        ReadQuitWait(w);
        unsigned int index = w.Ticket & 0xFFFF;
        bool pending = w.QueueDone >= 0 && ((w.QueueDone - (int)index) & 0xFFFF) >= 0x8000;
        // (1 asked, 2 quitting; -1: its quit flag couldn't be read, not a quit)
        bool quitting = w.Read && w.FrameKnown && w.QuitState > 0;
        bool waiting = quitting && w.QuitState == 2 && w.Server && w.Ticket && pending;
        quitFor = quitting ? quitFor + 1 : 0;
        if (!waiting || w.Ticket != stuckTicket)
        {
          stuckTicket = waiting ? w.Ticket : 0;
          stuckFor = 0;
        }
        else
          ++stuckFor;
        if (util::DiagnosticsOn() && quitting && (quitFor == 1 || quitFor % QuitLogEverySeconds == 0))
          LogQuitWait(quitFor == 1 ? "the quit started" : "the quit goes on");
        const char* why = nullptr;
        if (stuckFor >= StuckQuitSeconds)
          why = "its server's shutdown has waited on the same unload all that time";
        else if (g_closeAskedAt.load() && stuckFor >= StuckAfterCloseSeconds)
          why = "its window was asked to close while its server's shutdown waits on an unload";
        else if (quitFor >= QuitMaxSeconds)
          why = "it hasn't ended";
        if (why)
        {
          util::log::Warning("Exit: the game's quit is stuck (%d s since it was asked, %d s on the server's unload ticket "
            "0x%08X, that queue done up to 0x%X, the operation '%s'; %s): the tools end the game, as its own watchdog does "
            "(exit code 42)", quitFor, stuckFor, w.Ticket, w.QueueDone, w.Operation, why);
          if (util::DiagnosticsOn())
            util::hooks::LogLevelStreaming("Exit (diagnostic)");
          ExitProcess(42);
        }
      }

      char fileName[MAX_PATH];
      if (g_shutdown)
      {
        // Its window gone and the tools stopped, the game should end within
        // seconds (03:37 the same day: 6 s after it took its level out)
        if (++closedFor == ExitSeconds && reports < MaxReports)
        {
          // The report and its line: diagnostics only (counted in MaxReports only when written, so the
          // Hang_ report keeps its room)
          if (util::DiagnosticsOn())
          {
            ++reports;
            char why[128];
            sprintf_s(why, "the game still runs %d s after its window closed", ExitSeconds);
            if (WriteThreadReport(why, "ExitHang", windowThread, fileName, sizeof(fileName)))
              util::log::TryWarning("Exit: the game still runs %d s after its window closed: where each of its threads is was "
                "written to %s", ExitSeconds, fileName + 2);
          }
        }
        continue;
      }

      // Asked to close (the user's X or Alt+F4) and still not closing
      ULONGLONG now = GetTickCount64();
      ULONGLONG closeAsked = g_closeAskedAt.load();
      if (closeAsked && closeAsked != closeReported && now - closeAsked >= CloseSeconds * 1000ULL && reports < MaxReports)
      {
        closeReported = closeAsked;
        // The report and the lines: diagnostics only (counted only when written, as above)
        if (util::DiagnosticsOn())
        {
          ++reports;
          char why[128];
          sprintf_s(why, "the game's window was asked to close %d s ago and isn't closing", CloseSeconds);
          if (WriteThreadReport(why, "Close", windowThread, fileName, sizeof(fileName)))
            util::log::TryWarning("Exit: the game's window was asked to close %d s ago and isn't closing: where each of its "
              "threads is was written to %s. The window's thread is %s", CloseSeconds, fileName + 2, g_windowThreadWhere);
          util::hooks::LogLevelStreaming("Exit (diagnostic)");
          LogQuitWait("the window asked to close");
        }
      }

      // The level gone (Faith with it) a while, no loading screen: a quit
      // that doesn't end the game, or the game's own menu without a level
      ULONGLONG goneSince = util::hooks::FaithGoneSince();
      if (goneSince && goneSince != goneReported && now - goneSince >= LevelGoneSeconds * 1000ULL &&
        !util::hooks::LoadScreenShown() && !gamestate::Loading() && reports < MaxReports)
      {
        goneReported = goneSince;
        // The report and the lines: diagnostics only (counted only when written, as above)
        if (util::DiagnosticsOn())
        {
          ++reports;
          char why[128];
          sprintf_s(why, "the game took its level out %d s ago and is still running, no loading screen up", LevelGoneSeconds);
          if (WriteThreadReport(why, "LevelGone", windowThread, fileName, sizeof(fileName)))
            util::log::TryWarning("Exit: the game took its level out %d s ago and is still running (no loading screen; a quit "
              "ends it in about 5 s): where each of its threads is was written to %s. Its level streaming:", LevelGoneSeconds,
              fileName + 2);
          util::hooks::LogLevelStreaming("Exit (diagnostic)");
          LogQuitWait("the level gone");
        }
      }

      if (!IsHungAppWindow(hwnd))
      {
        if (hungFor > 0)
          util::log::TryWarning("The game's window answers Windows again, after about %d s", hungFor + 5);
        hungFor = 0;
        continue;
      }
      ++hungFor;
      int stuck = hungFor + 5;
      if ((stuck == HangSeconds || stuck == HangSeconds + HangAgainSeconds) && reports < MaxReports)
      {
        ++reports;
        char why[128];
        sprintf_s(why, "the game's window hasn't answered Windows for about %d s", stuck);
        if (WriteThreadReport(why, "Hang", windowThread, fileName, sizeof(fileName)))
          util::log::TryWarning("The game's window hasn't answered Windows for about %d s (the game is stuck): where each of "
            "its threads is was written to %s. The window's thread is %s", stuck, fileName + 2, g_windowThreadWhere);
      }
    }
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

  // The console is off unless [General] ShowConsole = true (or the MISC tab). The diagnostics are off unless
  // [Debug] Diagnostics = true: read here, before the log, the hooks and the tools' threads (util::DiagnosticsOn)
  {
    INIReader earlyConfig(g_configFile);
    m_ShowConsole = earlyConfig.GetBoolean("General", "ShowConsole", false);
    util::g_DiagnosticsOn = earlyConfig.GetBoolean("Debug", "Diagnostics", false);
  }
  util::log::Init(m_ShowConsole);
  // What the tools' own C runtime ends the game for goes past the crash filter (set later, in InitializeGraphics):
  // its lines are written by these (see NoteRuntimeEnd). Set from the start: they are the tools' runtime's own, the
  // game has another, and the start-up is where a too-long text or an abort would otherwise leave nothing.
  signal(SIGABRT, &OnRuntimeAbort);
  _set_invalid_parameter_handler(&OnRuntimeRefusal);
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

  // Writes where the game's threads are when its window stops answering or
  // the game doesn't end after its window closed (see HangWatch). It waits
  // for the window, which the first Present gives.
  HANDLE hWatch = CreateThread(NULL, 0, HangWatch, NULL, 0, NULL);
  if (hWatch) CloseHandle(hWatch);

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
  // Before the level: it notes the outfit entities as the game makes them
  faith_outfit::Init();
  // GAME > Faith > Size: its hooks (collision and hair). Switched off with its menu (CameraManager.cpp's GAME
  // tab): no hooks made
  // faith_size::Init();
  // GAME > Street level: the kill floors' test, the fall check, the physics' flush
  ground::Init();
  // The road cars keep to their lanes and their gaps (the game's car step; no menu entry)
  road_traffic::Init();
  // GAME > Level streaming > No culling: the hooks on the main view's two mesh cull tests (pass-through until on);
  // before LoadConfig, which may switch it on
  culling_off::Init();
  // The game's sound back if a replay's sound track was cut short last time
  replay_audio::Init();

  LoadConfig();
  // GAME > Pose: the game's pose blend hooked here at the start, not at the first Take on the game's thread (that
  // frame took 45 - 53 ms, CT.log 2026-11-13; that making the hook, which freezes every thread, is why is inferred).
  // Last, so everything before the first level is made as early as before
  pose_editor::Init();
  return true;
}

void Main::Run()
{
  // Main update loop. The camera itself is updated from the game's
  // camera update hook so that it moves once per rendered frame.
  auto lastUpdate = std::chrono::steady_clock::now();
  double dtPresentCheck = 1.0;
  while (!g_shutdown)
  {
    auto now = std::chrono::steady_clock::now();
    std::chrono::duration<double> dt = now - lastUpdate;
    lastUpdate = now;

    // How full the game's job queue gets (logged below; diagnostics only)
    if (util::DiagnosticsOn())
      util::hooks::SampleJobQueue();

    // Whether presents still pass the tools' jump in dxgi
    dtPresentCheck += dt.count();
    if (dtPresentCheck >= 1.0)
    {
      dtPresentCheck = 0;
      util::hooks::CheckPresentHooks();
      util::hooks::LogLightMapIds();
      util::hooks::LogPhysicsBodies();
      util::hooks::LogJobQueue();
      util::hooks::LogMissionCleanupGuard();
      if (util::DiagnosticsOn())
        util::hooks::LogSunShadowReach();
    }

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

  // Save config and disable hooks before exit. Each step goes in CT.log, so
  // a game that doesn't close shows how far the tools got (the hang watch
  // writes where its threads are if it's still running ExitSeconds later)
  auto exitStart = std::chrono::steady_clock::now();
  util::log::Write("Exit: the tools stop (their config saved, their hooks off; nothing else is freed or put back while "
    "the game closes)");
  SaveConfig();
  util::hooks::LogMissionCleanupGuard();
  util::hooks::SetHookState(false);
  std::chrono::duration<double, std::milli> exitTook = std::chrono::steady_clock::now() - exitStart;
  util::log::Write("Exit: the tools stopped in %.0f ms; the rest is the game's own closing", exitTook.count());
}

void Main::OnPresent(IDXGISwapChain* pSwapChain)
{
  if (!m_GraphicsInitialized)
  {
    if (m_GraphicsFailed || !InitializeGraphics(pSwapChain))
      return;
  }

  // Overlays (Origin etc.) can present their own swapchains, only draw on the game's.
  // The green screen goes over the game's picture first, then the Panini
  // projection (which bends it with the rest), the menu over that.
  if (pSwapChain == g_dxgiSwapChain)
  {
    // The frame's time and what the tools cost (MISC > Performance)
    perf::FrameStart(g_d3d11Device, g_d3d11Context);
    // Diagnostic (temporary): the game's draws described while the replay records (see ReplayDrawLog.h)
    replay_drawlog::Present(g_d3d11Device, g_d3d11Context);
    // The recorded models drawn with the game's own draws: their captures read back (see ReplayGameDraws.h)
    replay_gamedraws::Present(pSwapChain, g_d3d11Context);
    // Diagnostic: the replay's meshes and textures, loaded, drawn and missed (see ReplayAssetLog.h)
    replay_assetlog::Present(g_d3d11Device);
    // Saved recordings: a save's graphics card copies read back, a load's objects made (see ReplayStore.h)
    replay_store::Present(g_d3d11Device, g_d3d11Context);
    {
      perf::Scope timing(perf::SectionPasses);
      perf::GpuBegin(g_d3d11Context, 0);
      // The wet surfaces (VISUALS > Weather) first, under the green screen.
      // The performance test's second phase leaves the wet surfaces and the
      // Panini projection out (the green screen keeps its frame's bookkeeping)
      bool paused = perf::DrawingPaused();
      if (!paused)
      {
        wetlook::Apply(pSwapChain, g_d3d11Device, g_d3d11Context);
        // The replay's own drawing of recorded models the game doesn't have
        // (a mission's doors and gates once it's over), with the game's depth
        replay_shapes::Apply(pSwapChain, g_d3d11Device, g_d3d11Context);
        // The recorded Dash markers (the game's runner's vision layer) mixed
        // in as the game's tonemap does, under the pictures in the world
        replay_gamedraws::ApplyRunnersVision(pSwapChain, g_d3d11Device, g_d3d11Context);
        // The pictures in the world (VISUALS > Images) over the wet surfaces
        // (so their puddles and snow don't go over a picture), under the
        // green screen; before the green screen's Apply, which ends the
        // frame's depth
        worldimages::Apply(pSwapChain, g_d3d11Device, g_d3d11Context);
      }
      greenplane::Apply(pSwapChain, g_d3d11Device, g_d3d11Context);
      // The snow's copies of the game's G-buffer are this frame's only (GameFrame.h)
      gameframe::FrameEnd();
      // The lens distortion (VISUALS > Filmic effects) bends all of it, the
      // game's depth of field and the tools' passes with the picture
      if (!paused)
      {
        lensdistortion::Apply(pSwapChain, g_d3d11Device, g_d3d11Context);
        panini::Apply(pSwapChain, g_d3d11Device, g_d3d11Context);
      }
      perf::GpuEnd(g_d3d11Context, 0);
    }
    // A picture of the finished frame when one is asked for (VISUALS > Depth
    // of field's test pictures), before the menu goes over it
    frameshot::OnPresent(pSwapChain, g_d3d11Device, g_d3d11Context);
    {
      perf::Scope timing(perf::SectionMenu);
      perf::GpuBegin(g_d3d11Context, 1);
      m_pUI->Draw();
      perf::GpuEnd(g_d3d11Context, 1);
    }
  }
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
  worldimages::ReadConfig(m_pConfig.get());
  pose_editor::ReadConfig(m_pConfig.get());
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
  // Not in the menu: written back only while on, so it survives the save
  if (util::DiagnosticsOn())
    file << "[Debug]\nDiagnostics = true\n\n";
  file << m_pCameraManager->GetConfig();
  file << m_pInputSystem->GetConfig();
  file << worldimages::GetConfig();
  file << pose_editor::GetConfig();

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
  case WM_CLOSE:
    util::log::Write("Exit: the game's window was asked to close");
    g_closeAskedAt = GetTickCount64();
    break;
  case WM_DESTROY:
    util::log::Write("Exit: the game's window is closing");
    g_shutdown = true;
    break;
  }

  return CallWindowProc(g_origWndProc, hwnd, uMsg, wParam, lParam);
}

