#include "Util.h"
#include <mutex>
#include <share.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <string>

using namespace util;

std::mutex g_logMutex;

namespace
{
  HANDLE hstdin, hstdout;
  FILE* pfstdin;
  FILE* pfstdout;
  FILE* pfileout;
  bool hasConsole = false;

  // The log's file can be held by another start of the game for its first second: through some launchers the game
  // starts twice in a row, both load the tools, and the first exits. 2026-11-12: the second start (the one that
  // stays) could not open CT.log, so the whole session had no log (the crash dump showed pfileout null; the 33 lines
  // in CT.log were the first start's). So a log that can't be opened is tried again at later lines, the lines kept
  // in memory until then; when another running game holds it for good, this process writes CT.<its id>.log.
  const char* const LogPath = ".\\Cinematic Tools\\CT.log";
  const char* const PreviousLogPath = ".\\Cinematic Tools\\CT.previous.log";
  const ULONGLONG HandOverMs = 5000;         // a CT.log written this recently and this small is the few lines of the
  const DWORD StubMaxBytes = 8 * 1024;       // start that led to this one (a session's log passes 8 KB about 9 s in,
                                             // just before the crash lines can first be written)
  const ULONGLONG LogRetryMs = 100;         // a log that isn't open is tried again this often (when a line comes)
  const ULONGLONG LogOwnFileAfterMs = 3000;  // after this long this process takes a file of its own
  const size_t MaxPendingLog = 1 << 20;      // bytes of lines kept while no file is open
  bool g_logInitDone = false;                // nothing is kept or tried before Init (as before: those lines are dropped)
  bool g_logGivenUp = false;                 // no file could be opened at all: nothing is tried or kept any more
  ULONGLONG g_logInitAt = 0, g_logTriedAt = 0;
  std::string g_pendingLog;                  // with g_logMutex held
  size_t g_pendingDropped = 0;

  static void TimeStamp(char* out, size_t size)
  {
    SYSTEMTIME now;
    GetLocalTime(&now);
    _snprintf_s(out, size, _TRUNCATE, "[%02d:%02d:%02d] ", now.wHour, now.wMinute, now.wSecond);
  }

  static void OwnLogPath(char* out, size_t size)
  {
    _snprintf_s(out, size, _TRUNCATE, ".\\Cinematic Tools\\CT.%lu.log", GetCurrentProcessId());
  }

  // Before every open of CT.log (opening it empties it): the last session's log is kept as CT.previous.log, so
  // it's still there after a crash and a restart. Not a first start's few lines (written within the last seconds and
  // small): moving those would put them over the real last session's log. While another process holds CT.log the
  // move fails and nothing changes; once it lets go, a real session's log (a game still closing when this one
  // started) is moved before it is emptied. Still lost: the log of a session that ended in its first seconds and was
  // started again within 5 s of its last line. No lock, system calls only (CrashNote and ExitNote get here too).
  static void KeepLastLog()
  {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExA(LogPath, GetFileExInfoStandard, &data))
      return;
    FILETIME nowTime;
    GetSystemTimeAsFileTime(&nowTime);
    ULONGLONG written = ((ULONGLONG)data.ftLastWriteTime.dwHighDateTime << 32) | data.ftLastWriteTime.dwLowDateTime;
    ULONGLONG nowValue = ((ULONGLONG)nowTime.dwHighDateTime << 32) | nowTime.dwLowDateTime;
    bool fresh = nowValue >= written && nowValue - written < HandOverMs * 10000ULL;
    bool little = !data.nFileSizeHigh && data.nFileSizeLow < StubMaxBytes;
    if (!(fresh && little))
      MoveFileExA(LogPath, PreviousLogPath, MOVEFILE_REPLACE_EXISTING);
  }

  // With g_logMutex held. The log's file opened if it can be: CT.log, or after LogOwnFileAfterMs this process's own.
  // first: Init's own try (no note; nothing is pending yet).
  static void OpenLogFile(bool first)
  {
    ULONGLONG now = GetTickCount64();
    g_logTriedAt = now;
    KeepLastLog();
    // Shared for reading, so the log can be opened while the game runs
    // (fopen_s would lock it)
    FILE* file = _fsopen(LogPath, "w", _SH_DENYWR);
    bool own = false;
    if (!file && !first && now - g_logInitAt >= LogOwnFileAfterMs)
    {
      char path[MAX_PATH];
      OwnLogPath(path, sizeof(path));
      file = _fsopen(path, "w", _SH_DENYWR);
      own = true;
      if (!file)
      {
        // Neither can be written (the folder is gone, or read-only): stop trying and keeping
        g_logGivenUp = true;
        g_pendingLog.clear();
        g_pendingLog.shrink_to_fit();
        return;
      }
    }
    if (!file)
      return;
    if (!first)
    {
      char stamp[32];
      TimeStamp(stamp, sizeof(stamp));
      if (own)
        fprintf(file, "%sCT.log is held by another running game, so this one's log is CT.%lu.log. The lines written since "
          "the tools started (%.1f s ago) follow.\n", stamp, GetCurrentProcessId(), (double)(now - g_logInitAt) / 1000.0);
      else
        fprintf(file, "%sThe log could be opened only now, %.1f s after the tools started: another start of the game still "
          "held it (the game starts twice in a row through some launchers). The lines written until now follow.\n", stamp,
          (double)(now - g_logInitAt) / 1000.0);
    }
    pfileout = file;
  }

  // With g_logMutex held: the lines kept while no file was open, written now that one is
  static void WritePendingLog()
  {
    if (!pfileout || (g_pendingLog.empty() && !g_pendingDropped))
      return;
    if (!g_pendingLog.empty())
      fwrite(g_pendingLog.data(), 1, g_pendingLog.size(), pfileout);
    if (g_pendingDropped)
      fprintf(pfileout, "(%zu more lines from before the log was open were not kept)\n", g_pendingDropped);
    fflush(pfileout);
    g_pendingLog.clear();
    g_pendingLog.shrink_to_fit();
    g_pendingDropped = 0;
  }

  // With g_logMutex held
  static void PrintHeld(WORD color, const char* type, const char* format, va_list args)
  {
    char stamp[32];
    TimeStamp(stamp, sizeof(stamp));
    std::string finalFormat = std::string(type) + format + "\n";

    va_list argsCopy;
    va_copy(argsCopy, args);
    if (hasConsole)
    {
      SetConsoleTextAttribute(hstdout, FOREGROUND_RED | FOREGROUND_INTENSITY);
      fputs(stamp, stdout);
      SetConsoleTextAttribute(hstdout, color);
      vfprintf(stdout, finalFormat.c_str(), args);
    }
    if (!pfileout && g_logInitDone && !g_logGivenUp && GetTickCount64() - g_logTriedAt >= LogRetryMs)
      OpenLogFile(false);
    if (pfileout)
    {
      WritePendingLog();
      fputs(stamp, pfileout);
      vfprintf(pfileout, finalFormat.c_str(), argsCopy);
      fflush(pfileout);
    }
    else if (g_logInitDone && !g_logGivenUp)
    {
      // No file yet: the line is kept (a line longer than this is cut)
      if (g_pendingLog.size() < MaxPendingLog)
      {
        // (vsnprintf, as vfprintf above reads a format: the _s kind refuses some that vfprintf prints)
        char line[4096];
        line[0] = 0;
        int length = vsnprintf(line, sizeof(line), finalFormat.c_str(), argsCopy);
        line[sizeof(line) - 1] = 0;
        g_pendingLog += stamp;
        g_pendingLog += line;
        if (length < 0 || length >= (int)sizeof(line))
          g_pendingLog += "\n";
      }
      else
        ++g_pendingDropped;
    }
    va_end(argsCopy);
  }

  // No lock (see CrashNote and ExitNote). A log that still isn't open is opened now, as this process's own file if
  // CT.log is still held: what a crash or the process's end says must not be lost. The lines kept in memory are left
  // where they are (another thread may be at them); the next ordinary line writes them.
  static void OpenLogForLastWords()
  {
    if (pfileout || !g_logInitDone)
      return;
    KeepLastLog();
    FILE* file = _fsopen(LogPath, "w", _SH_DENYWR);
    if (!file)
    {
      char path[MAX_PATH];
      OwnLogPath(path, sizeof(path));
      file = _fsopen(path, "w", _SH_DENYWR);
    }
    pfileout = file;
  }

  // While the diagnostics are off (util::DiagnosticsOn), their lines are dropped here too: every diagnostic line
  // says "diagnostic" in its format, or, built as text first, before its first ':' (e.g. "%s: ..." with the prefix
  // "Exit (diagnostic)")
  // (formatOnly: the format alone, no formatting: TryWarning's callers can't wait on anything)
  static bool IsDiagnosticLine(const char* format, va_list args, bool formatOnly = false)
  {
    if (util::DiagnosticsOn() || !format)
      return false;
    if (strstr(format, "diagnostic"))
      return true;
    if (formatOnly || format[0] != '%' || format[1] != 's')
      return false;
    char text[160];
    va_list argsCopy;
    va_copy(argsCopy, args);
    vsnprintf(text, sizeof(text), format, argsCopy);
    va_end(argsCopy);
    text[sizeof(text) - 1] = 0;
    if (char* colon = strchr(text, ':'))
      *colon = 0;
    return strstr(text, "diagnostic") != nullptr;
  }

  static void PrintMessage(WORD color, const char* type, const char* format, va_list args)
  {
    if (IsDiagnosticLine(format, args))
      return;
    // Block other threads from writing at the same time
    std::lock_guard<std::mutex> lock(g_logMutex);
    PrintHeld(color, type, format, args);
  }
}

void log::Init(bool showConsole)
{
  {
    std::lock_guard<std::mutex> lock(g_logMutex);
    g_logInitAt = GetTickCount64();
    // (the last session's log is kept first: KeepLastLog)
    OpenLogFile(true);
    g_logInitDone = true;
  }
  if (!showConsole)
    return;

  hasConsole = true;
  AllocConsole();
  freopen_s(&pfstdout, "CONOUT$", "w", stdout);
  freopen_s(&pfstdin, "CONIN$", "r", stdin);
  hstdin = GetStdHandle(STD_INPUT_HANDLE);
  hstdout = GetStdHandle(STD_OUTPUT_HANDLE);

  // Clicking a console with QuickEdit on puts it in "Select" mode, which
  // blocks every thread that writes to it, including the game's own threads
  // when they log from a hook. Turn it off so the game can't be frozen that way.
  DWORD consoleMode = 0;
  if (GetConsoleMode(hstdin, &consoleMode))
    SetConsoleMode(hstdin, (consoleMode & ~ENABLE_QUICK_EDIT_MODE) | ENABLE_EXTENDED_FLAGS);
}

void log::Write(const char* format, ...)
{
  va_list args;
  va_start(args, format);
  PrintMessage(FOREGROUND_BLUE | FOREGROUND_GREEN | FOREGROUND_RED | FOREGROUND_INTENSITY, "", format, args);
  va_end(args);
}

void log::Warning(const char* format, ...)
{
  va_list args;
  va_start(args, format);
  PrintMessage(FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY, "[WARNING] ", format, args);
  va_end(args);
}

void log::Error(const char* format, ...)
{
  va_list args;
  va_start(args, format);
  PrintMessage(FOREGROUND_RED | FOREGROUND_INTENSITY, "[ERROR] ", format, args);
  va_end(args);
}

void log::Ok(const char* format, ...)
{
  va_list args;
  va_start(args, format);
  PrintMessage(FOREGROUND_GREEN | FOREGROUND_INTENSITY, "[OK] ", format, args);
  va_end(args);
}
void log::CrashNote(const char* text)
{
  // No lock: the crashing thread might be the one holding it
  OpenLogForLastWords();
  if (pfileout)
  {
    fputs(text, pfileout);
    fputs("\n", pfileout);
    fflush(pfileout);
  }
}

void log::ExitNote(const char* text)
{
  // Not even the file's own lock: a thread Windows stopped mid-write still
  // holds it, and nothing else runs any more
  OpenLogForLastWords();
  if (!pfileout)
    return;
  SYSTEMTIME now;
  GetLocalTime(&now);
  char stamp[32];
  sprintf_s(stamp, "[%02d:%02d:%02d] ", now.wHour, now.wMinute, now.wSecond);
  _fwrite_nolock(stamp, 1, strlen(stamp), pfileout);
  _fwrite_nolock(text, 1, strlen(text), pfileout);
  _fwrite_nolock("\n", 1, 1, pfileout);
  _fflush_nolock(pfileout);
}

bool log::TryWarning(const char* format, ...)
{
  va_list args;
  va_start(args, format);
  if (IsDiagnosticLine(format, args, true))
  {
    va_end(args);
    return true;
  }
  std::unique_lock<std::mutex> lock(g_logMutex, std::try_to_lock);
  if (!lock.owns_lock())
  {
    va_end(args);
    return false;
  }
  PrintHeld(FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY, "[WARNING] ", format, args);
  va_end(args);
  return true;
}
