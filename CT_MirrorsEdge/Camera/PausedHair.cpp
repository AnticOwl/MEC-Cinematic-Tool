#include "PausedHair.h"
#include "../Main.h"
#include "../Util/Util.h"
#include <Windows.h>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>

// See PausedHair.h and Research_Pose_Editor_2026-11-08.md section 16
namespace
{
  const DWORD KnownTimeStamp = 0x5762E3B7;

  // Faith's cloths, found as CameraManager.cpp's hair code finds them: her
  // soldier's components (+ 0x38, one every 0x20 bytes), her customization
  // component, each ClothEntity it holds, its EA cloth instance (+ 0x68)
  const __int64 CustomizationComponentVTableRva = 0x1C3C360;
  const __int64 ClothEntityVTableRva = 0x1AD2998;
  const __int64 ClothInstanceVTableRva = 0x1AB6380;
  const int ComponentCollectionOffset = 0x38;
  const int ClothEntityInstanceOffset = 0x68;
  const int MaxCloths = 4;

  // The game's cloth manager (the global EAClothSimUpdater's slot 3 hands
  // the physics step to) and the cloth settings (made by the game on first
  // use; + 0x2E: the cloth sim is on)
  const __int64 ClothManagerRva = 0x23DCA90;
  const __int64 ClothSettingsRva = 0x23DCCA8;
  const int SettingsSimulateOffset = 0x2E;
  const int ManagerListOffset = 0x00;         // begin, end: the cloth instances
  const int ManagerStepRecordOffset = 0xA0;   // the last physics step record it was handed (64 bytes)
  const int ManagerSimSyncOffset = 0xE0;      // the sim jobs' sync handle
  const int ManagerInterpSyncOffset = 0xF8;   // the interpolation jobs' sync handle
  const int MaxListed = 4096;

  // A cloth instance
  const int InstanceLodOffset = 0x180;        // u32: the sim job steps it at 0 only
  const int InstanceOffOffset = 0x18B;        // byte: set, the sim job doesn't step it
  const int InstanceSimulationOffset = 0x38;
  const int SimulationLastStepOffset = 0xE0;  // float: the EA step's time of its last step

  // The step record: + 0x10 a physics substep's time, + 0x2C this substep,
  // + 0x30 the last one. The cloth's time is (last + 1) x the substep's.
  const int RecordStepTimeOffset = 0x10;
  const int RecordSubstepOffset = 0x2C;
  const int RecordLastSubstepOffset = 0x30;
  const int RecordBytes = 0x40;

  const __int64 PreUpdateRva = 0x2FA2760;     // (instance, record)
  const __int64 SimJobRva = 0x2FA07A0;        // (job record)
  const __int64 JobDoneRva = 0x2FBAB50;       // (sync handle) -> done (or none)

  const float StepTime = 1.f / 60.f;          // s of real time a step, at least
  const float MaxStepTime = 1.f / 30.f;       // and at most (the game's own at 1x)
  const ULONGLONG LogPeriodMs = 10000;

  // What the sim job (0x142FA07A0) is handed: the manager's own is at
  // manager + 0x90 (the scheduler writes + 0x00, + 0x94, + 0x98, + 0xA0)
  struct alignas(16) SimJobRecord
  {
    volatile LONG Next;                       // + 0x00: the job adds 1 for each 4 instances it takes
    unsigned int Count;                       // + 0x04
    __int64 const* List;                      // + 0x08
    BYTE Record[RecordBytes];                 // + 0x10: the step record (the job reads + 0x20, + 0x40)
  };
  static_assert(offsetof(SimJobRecord, Count) == 0x04 && offsetof(SimJobRecord, List) == 0x08 &&
    offsetof(SimJobRecord, Record) == 0x10, "the sim job's record");

  struct CodeCheck
  {
    __int64 Rva;
    const char* What;
    BYTE Bytes[56];
    size_t Size;
  };
  const CodeCheck CodeChecks[] = {
    // EAClothSimUpdater slot 3: on the step's last substep, the manager
    // (mov rcx, [rip -> 0x1423DCA90]) and the step record to the scheduler
    { 0x2FA2720, "the cloth sim updater", { 0x53, 0x48, 0x83, 0xEC, 0x20, 0x8B, 0x42, 0x30, 0x48, 0x89, 0xD3, 0x39, 0x42,
      0x2C, 0x75, 0x19, 0xE8, 0x5B, 0x9E, 0xA7, 0xFF, 0x48, 0x8B, 0x0D, 0x54, 0xA3, 0x43, 0xFF, 0x48, 0x89, 0xDA, 0x48,
      0x83, 0xC4, 0x20, 0x5B, 0xE9 }, 37 },
    // The pre-update: (instance, record) ...
    { PreUpdateRva, "the cloth pre-update", { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48,
      0x83, 0xEC, 0x20, 0x48, 0x89, 0xD6, 0x48, 0x89, 0xCF, 0xE8, 0x26, 0x2C, 0x00, 0x00, 0x48, 0x89 }, 28 },
    // ... the record's + 0x30 and + 0x10 for the root's speed
    { 0x2FA27D7, "the cloth pre-update's step time", { 0x8B, 0x46, 0x30, 0x0F, 0x28, 0x8F, 0x00, 0x01, 0x00, 0x00, 0x0F,
      0x57, 0xC0, 0xFF, 0xC0, 0x0F, 0x5C, 0x8F, 0x40, 0x01, 0x00, 0x00, 0xF3, 0x48, 0x0F, 0x2A, 0xC0, 0xF3, 0x0F, 0x59,
      0x46, 0x10 }, 32 },
    // The sim job: its time ([rcx + 0x40] + 1) x [rcx + 0x20], its count
    // [rcx + 4], its batches of 4 (lock xadd [rcx])
    { SimJobRva, "the cloth sim job", { 0x55, 0x56, 0x41, 0x57, 0x48, 0x83, 0xEC, 0x40, 0x8B, 0x41, 0x40, 0x44, 0x8B,
      0x79, 0x04, 0x0F, 0x29, 0x74, 0x24, 0x30, 0x0F, 0x57, 0xF6, 0xFF, 0xC0, 0x48, 0x89, 0xCD, 0xF3, 0x48, 0x0F, 0x2A,
      0xF0, 0xB8, 0x0F, 0xA2, 0xFF, 0xE6, 0x8D, 0x80, 0xF2, 0x5D, 0x00, 0x19, 0xF3, 0x0F, 0x59, 0x71, 0x20, 0xF0, 0x0F,
      0xC1, 0x01 }, 53 },
    // ... its list [rbp + 8], the instance's LOD (+ 0x180) and + 0x18B
    { 0x2FA0804, "the cloth sim job's list", { 0x48, 0x8B, 0x45, 0x08, 0x48, 0x8B, 0x3C, 0xF0, 0x48, 0x89, 0xF9, 0xE8,
      0x0C, 0x48, 0x00, 0x00, 0x8B, 0x87, 0x80, 0x01, 0x00, 0x00, 0x85, 0xC0, 0x75, 0x30, 0x38, 0x87, 0x8B, 0x01, 0x00,
      0x00, 0x75, 0x28 }, 34 },
    // A sync handle's done check (reads only)
    { JobDoneRva, "the job done check", { 0x48, 0x89, 0xCA, 0x48, 0x8B, 0x49, 0x08, 0x48, 0x85, 0xC9, 0x75, 0x03, 0xB0,
      0x01, 0xC3, 0x48, 0x8B, 0x49, 0x08, 0x31, 0xC0, 0x48, 0x3B, 0x4A, 0x10, 0x0F, 0x95, 0xD0, 0xC3 }, 29 },
    // The scheduler: the settings (mov rax, [rip -> 0x1423DCCA8]), their
    // + 0x2E, the record into manager + 0xA0, the count and list into + 0x94
    // and + 0x98, the sim sync at + 0xE0
    { 0x2FA3D09, "the cloth settings", { 0x48, 0x8B, 0x05, 0x98, 0x8F, 0x43, 0xFF }, 7 },
    { 0x2FA3D2F, "the cloth sim switch", { 0x44, 0x38, 0x70, 0x2E }, 4 },
    { 0x2FA3D55, "the step record's copy", { 0x41, 0x0F, 0x10, 0x07, 0x0F, 0x11, 0x86, 0xA0, 0x00, 0x00, 0x00 }, 11 },
    { 0x2FA3DB1, "the sim job's count and list", { 0x89, 0x9E, 0x94, 0x00, 0x00, 0x00, 0x48, 0x89, 0x86, 0x98, 0x00,
      0x00, 0x00 }, 13 },
    { 0x2FA3E69, "the sim sync handle", { 0x4C, 0x8D, 0xAE, 0xE0, 0x00, 0x00, 0x00 }, 7 },
    // The interpolation's sync at + 0xF8
    { 0x2FA1C78, "the interpolation sync handle", { 0x4C, 0x8D, 0xAF, 0xF8, 0x00, 0x00, 0x00 }, 7 },
    // The EA step: its last step's time at + 0xE0 (handed in, then kept)
    { 0x3B7C5A0, "the EA cloth step", { 0x53, 0x48, 0x83, 0xEC, 0x30, 0xF3, 0x0F, 0x10, 0x91, 0xE0, 0x00, 0x00, 0x00,
      0x0F, 0x29, 0x74, 0x24, 0x20, 0x48, 0x89, 0xCB, 0x0F, 0x28, 0xF1, 0xE8, 0x33, 0x00, 0x00, 0x00, 0xF3, 0x0F, 0x11,
      0xB3, 0xE0, 0x00, 0x00, 0x00 }, 37 },
    // The cloth's reset (2026-11-11): its own method (instance vtable slot 12) only sets + 0x18A ...
    { 0x2FA39F0, "the cloth's reset method", { 0xC6, 0x81, 0x8A, 0x01, 0x00, 0x00, 0x01, 0xC3 }, 8 },
    // ... the sim job resets the simulation first when it's set (cmp [rdi + 0x18A], al; je; mov rcx, [rdi + 0x38];
    // call 0x143B787A0) ...
    { 0x2FA0826, "the sim job's reset", { 0x38, 0x87, 0x8A, 0x01, 0x00, 0x00, 0x74, 0x09, 0x48, 0x8B, 0x4F, 0x38, 0xE8,
      0x69, 0x7F, 0xBD, 0x00 }, 17 },
    // ... and the interpolation clears it every frame (0x142FA28F0's end)
    { 0x2FA29C5, "the interpolation's reset clear", { 0xC6, 0x83, 0x8A, 0x01, 0x00, 0x00, 0x00 }, 7 },
    // The EA reset: the particles ([[sim]]) from the skinned places ([[sim + 0xF0]]), their count [[[sim + 0x10] +
    // 0xB8] + 8] (the hair watch reads the same)
    { 0x3B787A0, "the EA cloth reset", { 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0x41, 0x10, 0x48, 0x89, 0xCB, 0x4C,
      0x8B, 0x88, 0xB8, 0x00, 0x00, 0x00, 0x4D, 0x85, 0xC9, 0x0F, 0x84, 0xA8, 0x00, 0x00, 0x00, 0x4C, 0x8B, 0x11, 0x4C,
      0x8B, 0x99, 0xF0, 0x00, 0x00, 0x00, 0x45, 0x31, 0xC0, 0x45, 0x39, 0x41, 0x08 }, 45 } };

  typedef void(__fastcall* tPreUpdate)(__int64 instance, BYTE const* record);
  typedef void(__fastcall* tSimJob)(SimJobRecord* job);
  typedef bool(__fastcall* tJobDone)(__int64 handle);

  std::once_flag g_Checked;
  bool g_Available = false;
  bool g_Broken = false;                      // a step faulted: off until the game restarts

  // The game's thread only
  bool g_Stepping = false;                    // frozen and wanted, last frame
  LARGE_INTEGER g_LastTime{};
  float g_Owed = 0.f;                         // s of real time not stepped yet
  long long g_Steps = 0, g_Busy = 0, g_NotFull = 0, g_NotListed = 0, g_LoggedSteps = 0;
  ULONGLONG g_LoggedAt = 0;
  int g_LastCount = -1;

  std::mutex g_StatusMutex;
  char g_Status[160] = "";

  void SetStatus(char const* text)
  {
    std::lock_guard<std::mutex> lock(g_StatusMutex);
    strcpy_s(g_Status, text);
  }

  bool CheckGame()
  {
    __int64 base = (__int64)g_gameHandle;
    __try
    {
      IMAGE_DOS_HEADER* pDos = (IMAGE_DOS_HEADER*)base;
      IMAGE_NT_HEADERS* pNt = (IMAGE_NT_HEADERS*)(base + pDos->e_lfanew);
      if (pNt->FileHeader.TimeDateStamp != KnownTimeStamp)
      {
        util::log::Warning("Paused hair: the game isn't the build the tools know (PE timestamp 0x%08X); off",
          pNt->FileHeader.TimeDateStamp);
        SetStatus("Not available: the game isn't the build the tools know");
        return false;
      }
      for (CodeCheck const& check : CodeChecks)
        if (memcmp((void const*)(base + check.Rva), check.Bytes, check.Size) != 0)
        {
          util::log::Warning("Paused hair: the game's code at 0x%I64X (%s) isn't what the tools know; off",
            0x140000000 + check.Rva, check.What);
          SetStatus("Not available: the game's cloth code isn't what the tools know (see CT.log)");
          return false;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      SetStatus("Not available: the game's code couldn't be read");
      return false;
    }
    return true;
  }

  bool Available()
  {
    std::call_once(g_Checked, [] { g_Available = CheckGame(); });
    return g_Available && !g_Broken;
  }

  __int64 ReadQword(__int64 address)
  {
    if (address < 0x10000 || address > 0x7FFFFFFF0000)
      return 0;
    __try
    {
      return *(__int64 const*)address;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      return 0;
    }
  }

  BYTE ReadByte(__int64 address)
  {
    __try
    {
      return *(BYTE const*)address;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      return 0;
    }
  }

  // Each EA cloth instance of hers (two ClothEntities can't share one)
  int FindCloths(__int64 soldier, __int64* out)
  {
    __int64 base = (__int64)g_gameHandle;
    int count = 0;
    __int64 collection = ReadQword(soldier + ComponentCollectionOffset);
    if (!collection)
      return 0;
    for (int entry = 0; entry < 32; ++entry)
    {
      __int64 component = ReadQword(collection + 0x20 + entry * 0x20);
      if (!component || ReadQword(component) != base + CustomizationComponentVTableRva)
        continue;
      for (int off = 0; off < 0x800 && count < MaxCloths; off += 8)
      {
        __int64 entity = ReadQword(component + off);
        if (!entity || entity == component || ReadQword(entity) != base + ClothEntityVTableRva)
          continue;
        __int64 instance = ReadQword(entity + ClothEntityInstanceOffset);
        if (!instance || ReadQword(instance) != base + ClothInstanceVTableRva)
          continue;
        bool known = false;
        for (int k = 0; k < count; ++k)
          known |= out[k] == instance;
        if (!known)
          out[count++] = instance;
      }
      break;
    }
    return count;
  }

  bool InList(__int64 const* begin, size_t listed, __int64 instance)
  {
    __try
    {
      for (size_t i = 0; i < listed; ++i)
        if (begin[i] == instance)
          return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
    return false;
  }

  bool ReadInstance(__int64 instance, unsigned int& lod, BYTE& off)
  {
    __try
    {
      lod = *(unsigned int const*)(instance + InstanceLodOffset);
      off = *(BYTE const*)(instance + InstanceOffOffset);
      return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      return false;
    }
  }

  bool JobsDone(__int64 manager)
  {
    tJobDone done = (tJobDone)((__int64)g_gameHandle + JobDoneRva);
    __try
    {
      return done(manager + ManagerSimSyncOffset) && done(manager + ManagerInterpSyncOffset);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      return false;
    }
  }

  bool CopyRecord(__int64 manager, BYTE* record)
  {
    __try
    {
      memcpy(record, (void const*)(manager + ManagerStepRecordOffset), RecordBytes);
      return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      return false;
    }
  }

  // The game's two per-cloth steps, as its scheduler and sim job make them:
  // every pre-update, then the sim job over the list
  bool StepCloths(__int64 const* cloths, int count, SimJobRecord* job)
  {
    __int64 base = (__int64)g_gameHandle;
    tPreUpdate preUpdate = (tPreUpdate)(base + PreUpdateRva);
    tSimJob simJob = (tSimJob)(base + SimJobRva);
    __try
    {
      for (int i = 0; i < count; ++i)
        preUpdate(cloths[i], job->Record);
      simJob(job);
      return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      return false;
    }
  }

  float LastStepOf(__int64 instance)
  {
    __try
    {
      __int64 simulation = *(__int64 const*)(instance + InstanceSimulationOffset);
      return simulation ? *(float const*)(simulation + SimulationLastStepOffset) : -1.f;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      return -1.f;
    }
  }

  void LogNow(char const* what)
  {
    util::log::Write("Paused hair: %s (%lld steps; skipped: %lld with the game's cloth jobs running, %lld not at full "
      "detail, %lld not in the game's cloth list)", what, g_Steps, g_Busy, g_NotFull, g_NotListed);
    g_LoggedSteps = g_Steps;
  }
}

void paused_hair::Status(char* out, size_t size)
{
  std::lock_guard<std::mutex> lock(g_StatusMutex);
  strncpy_s(out, size, g_Status, _TRUNCATE);
}

void paused_hair::Update(__int64 soldier, bool timeFrozen, bool wanted)
{
  LARGE_INTEGER now{}, frequency{};
  QueryPerformanceCounter(&now);
  QueryPerformanceFrequency(&frequency);
  bool step = wanted && timeFrozen && soldier != 0;
  if (!step)
  {
    if (g_Stepping)
    {
      LogNow(timeFrozen ? "stopped (unticked or no Faith)" : "stopped, time runs again");
      SetStatus(timeFrozen ? "" : "Not stepping: time isn't frozen (the game moves it)");
    }
    else if (!timeFrozen && wanted)
      SetStatus("Not stepping: time isn't frozen (the game moves it)");
    g_Stepping = false;
    return;
  }
  if (!Available())
  {
    if (g_Broken)
      SetStatus("Off: a step failed (see CT.log); back after the game restarts");
    return;
  }

  if (!g_Stepping)
  {
    // The first frozen frame: the time starts here
    g_Stepping = true;
    g_LastTime = now;
    g_Owed = 0.f;
    g_Steps = g_Busy = g_NotFull = g_NotListed = g_LoggedSteps = 0;
    g_LoggedAt = GetTickCount64();
    g_LastCount = -1;
    SetStatus("Waiting for her hair");
    return;
  }
  float elapsed = (float)((double)(now.QuadPart - g_LastTime.QuadPart) / (double)frequency.QuadPart);
  g_LastTime = now;
  if (elapsed > 0.f && elapsed < 1.f)
    g_Owed += elapsed;
  ULONGLONG tick = GetTickCount64();
  if (tick - g_LoggedAt >= LogPeriodMs)
  {
    g_LoggedAt = tick;
    if (g_Steps != g_LoggedSteps || g_Busy + g_NotFull + g_NotListed > 0)
      LogNow("while frozen");
  }
  if (g_Owed < StepTime)
    return;
  float dt = g_Owed < MaxStepTime ? g_Owed : MaxStepTime;
  g_Owed = 0.f;

  __int64 base = (__int64)g_gameHandle;
  __int64 cloths[MaxCloths];
  int found = FindCloths(soldier, cloths);
  __int64 manager = ReadQword(base + ClothManagerRva);
  __int64 settings = ReadQword(base + ClothSettingsRva);
  if (!found || !manager || !settings)
  {
    SetStatus(!found ? "Not stepping: her hair cloth isn't there" : "Not stepping: the game's cloths aren't set up");
    return;
  }
  if (!ReadByte(settings + SettingsSimulateOffset))
  {
    SetStatus("Not stepping: the game's cloth simulation is off");
    return;
  }

  // Hers that the game itself would step: in its list, at full detail
  __int64 begin = ReadQword(manager + ManagerListOffset);
  __int64 end = ReadQword(manager + ManagerListOffset + 8);
  size_t listed = begin && end > begin ? (size_t)(end - begin) / 8 : 0;
  if (listed > MaxListed)
    listed = 0;
  __int64 ready[MaxCloths];
  int count = 0;
  bool notListed = false, notFull = false;
  for (int i = 0; i < found; ++i)
  {
    unsigned int lod = 0;
    BYTE off = 0;
    if (!listed || !InList((__int64 const*)begin, listed, cloths[i]))
      notListed = true;
    else if (!ReadInstance(cloths[i], lod, off) || lod != 0 || off != 0)
      notFull = true;
    else
      ready[count++] = cloths[i];
  }
  g_NotListed += notListed;
  g_NotFull += notFull;
  if (!count)
  {
    SetStatus(notFull ? "Not stepping: her hair isn't at full detail (too far from the game's camera?)" :
      "Not stepping: her hair isn't in the game's cloth list");
    return;
  }

  // Never with the game's own cloth jobs running (they'd step or read the
  // same cloths): next frame
  if (!JobsDone(manager))
  {
    ++g_Busy;
    g_Owed = dt;
    return;
  }

  SimJobRecord job{};
  if (!CopyRecord(manager, job.Record))
    return;
  *(float*)(job.Record + RecordStepTimeOffset) = dt;
  *(unsigned int*)(job.Record + RecordSubstepOffset) = 0;
  *(unsigned int*)(job.Record + RecordLastSubstepOffset) = 0;
  job.Next = 0;
  job.Count = (unsigned int)count;
  job.List = ready;
  float lastBefore = LastStepOf(ready[0]);
  if (!StepCloths(ready, count, &job))
  {
    g_Broken = true;
    util::log::Error("Paused hair: a step of Faith's cloths faulted; off until the game restarts");
    LogNow("off");
    SetStatus("Off: a step failed (see CT.log); back after the game restarts");
    return;
  }
  ++g_Steps;
  if (count != g_LastCount)
  {
    // Once per change: what the EA step got (its time kept at + 0xE0)
    util::log::Write("Paused hair: stepping %d cloth(s) of Faith's while time is frozen (%d found); the EA step's "
      "time %.4f s before, %.4f s now", count, found, lastBefore, LastStepOf(ready[0]));
    g_LastCount = count;
  }
  char text[160];
  sprintf_s(text, "Stepping %d cloth%s of hers (%lld steps)", count, count == 1 ? "" : "s", g_Steps);
  SetStatus(text);
}

// Her hair reset at a clip's jumps (see PausedHair.h, KeepReset): the game's own cloth reset flag, the one its reset
// method (instance vtable slot 12, 0x142FA39F0) sets, written on her cloths every frame for a while. The game's sim
// job resets the simulation when it's set (0x142FA0826: the particles to their skinned places, still), and its
// interpolation clears it every frame (0x142FA29C5), so it's held across a few of the game's cloth steps.
namespace
{
  const int InstanceResetOffset = 0x18A;
  const ULONGLONG ResetHoldMs = 120;              // longer than 3 of the game's cloth steps (30 a second, at 1x)
  const ULONGLONG ResetLogPeriodMs = 10000;
  ULONGLONG g_ResetUntil = 0, g_ResetLoggedAt = 0;
  __int64 g_ResetCloths[MaxCloths] = {};
  int g_ResetFound = 0;
  long long g_ResetJumps = 0, g_ResetFrames = 0, g_ResetLoggedJumps = 0;
  bool g_ResetLogged = false;
  bool g_ResetThisFrame = false;                  // for the hair watch

  // Her cloth at full detail and simulated: its reset flag set (true), else false
  bool SetResetFlag(__int64 instance)
  {
    __try
    {
      if (*(unsigned int const*)(instance + InstanceLodOffset) != 0 || *(BYTE const*)(instance + InstanceOffOffset))
        return false;
      *(BYTE*)(instance + InstanceResetOffset) = 1;
      return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      return false;
    }
  }
}

void paused_hair::KeepReset(__int64 soldier, bool jumpNow, float gameScale)
{
  g_ResetThisFrame = false;
  if (!soldier || !Available())
  {
    g_ResetUntil = 0;
    g_ResetFound = 0;
    return;
  }
  ULONGLONG now = GetTickCount64();
  if (jumpNow)
  {
    // The game steps cloths in game time: slower than 1x, the hold is as long in game time (at 0.05x at least, 2.4 s);
    // frozen, the tools' own step runs at 60 a second of real time (Update)
    ULONGLONG hold = ResetHoldMs;
    if (gameScale > 0.f && gameScale < 1.f)
      hold = (ULONGLONG)(ResetHoldMs / (gameScale < 0.05f ? 0.05f : gameScale));
    g_ResetUntil = now + hold;
    g_ResetFound = FindCloths(soldier, g_ResetCloths);
  }
  if (now < g_ResetUntil)
  {
    __int64 instanceVTable = (__int64)g_gameHandle + ClothInstanceVTableRva;
    int set = 0;
    for (int i = 0; i < g_ResetFound; ++i)
      if (ReadQword(g_ResetCloths[i]) == instanceVTable && SetResetFlag(g_ResetCloths[i]))
        ++set;
    if (set)
    {
      ++g_ResetFrames;
      g_ResetThisFrame = true;
      if (jumpNow)
        ++g_ResetJumps;
    }
    if (!g_ResetLogged && jumpNow && set)
    {
      g_ResetLogged = true;
      util::log::Write("Hair: her hair reset at a jump of her pose (the game's own cloth reset, held %llu ms; %d cloth%s "
        "of hers)", g_ResetUntil - now, set, set == 1 ? "" : "s");
    }
  }
  if (now - g_ResetLoggedAt >= ResetLogPeriodMs)
  {
    g_ResetLoggedAt = now;
    if (g_ResetJumps != g_ResetLoggedJumps)
      util::log::Write("Hair (diagnostic): %lld jumps of her pose reset her hair in the last %.0f s (the flag set in %lld "
        "frames in all)", g_ResetJumps - g_ResetLoggedJumps, ResetLogPeriodMs / 1000.0, g_ResetFrames);
    g_ResetLoggedJumps = g_ResetJumps;
  }
}

// The hair watch (see PausedHair.h): reads only, on the game's thread, with [Debug] Diagnostics = true only
namespace
{
  const int InstanceRootPlaceOffset = 0x100;      // the root matrix's (+ 0xD0) place (the pre-update's movaps [+ 0x100])
  const int InstanceRootSpeedOffset = 0x150;      // 4 floats: the root's speed (the pre-update)
  const int InstanceFlagsOffset = 0x188;          // 8 bytes, + 0x18A the reset, + 0x18B off, + 0x18D the provider's
  const int WatchCloths = 2;
  const int Distinct = 4;                         // distinct LODs / flag bytes listed (more: "+more")

  struct ClothWindow
  {
    __int64 Instance = 0;
    int Frames = 0, Lod0 = 0, Off = 0, Reset = 0, Flag18D = 0, RootMoved = 0, StepMissed = 0;
    float RootMoveMost = 0.f, SpeedMost = 0.f, StepLow = 1e9f, StepHigh = -1e9f;
    unsigned int LodsSeen[Distinct] = {};
    int LodsCount = 0;
    bool LodsMore = false;
    BYTE FlagsSeen[Distinct][8] = {};
    int FlagsCount = 0;
    bool FlagsMore = false;
    float LastRoot[3] = {};
    bool LastRootOk = false;
    // Its particles against their skinned places (where her pose puts them): each frame's mean, summed; the most
    int ParticleFrames = 0, ParticleCount = 0;
    double ParticleMeanSum = 0.0;
    float ParticleMost = 0.f;
  };

  struct WatchState
  {
    ULONGLONG Since = 0, FoundAt = 0;
    __int64 Cloths[MaxCloths] = {};
    int Found = 0, Refound = 0;
    int Frames = 0, FrozenFrames = 0, HeadFrames = 0, ResetFrames = 0;
    float HeadPath = 0.f, HeadLast[3] = {};
    bool HeadLastOk = false;
    // The window's own pose: its clip (set on its first frame; a change of it ends the window)
    bool WinClipOn = false;
    char WinFile[96] = "";
    double WinFps = 0.0, WinTimeFrom = 0.0, WinTimeTo = 0.0;
    int WinPlaying = 0;
    ClothWindow Cloth[WatchCloths];
  };
  WatchState g_Watch;

  bool ReadClothNow(__int64 instance, unsigned int& lod, BYTE* flags, float* root, float* speed)
  {
    __try
    {
      lod = *(unsigned int const*)(instance + InstanceLodOffset);
      memcpy(flags, (void const*)(instance + InstanceFlagsOffset), 8);
      memcpy(root, (void const*)(instance + InstanceRootPlaceOffset), 3 * sizeof(float));
      memcpy(speed, (void const*)(instance + InstanceRootSpeedOffset), 4 * sizeof(float));
      return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      return false;
    }
  }

  // Her cloth's particles and their skinned places, as the EA reset reads them (0x143B787A0): [[sim]] and
  // [[sim + 0xF0]], 16 bytes each, [[[sim + 0x10] + 0xB8] + 8] of them. How many were read (at most MaxParticles; 0:
  // none)
  const int MaxParticles = 2048;
  float g_Particles[MaxParticles][4], g_Skinned[MaxParticles][4];
  int ReadParticles(__int64 instance)
  {
    __try
    {
      __int64 sim = *(__int64 const*)(instance + InstanceSimulationOffset);
      if (!sim)
        return 0;
      __int64 a = *(__int64 const*)sim, b = *(__int64 const*)(sim + 0x10), d = *(__int64 const*)(sim + 0xF0);
      if (!a || !b || !d)
        return 0;
      __int64 c = *(__int64 const*)(b + 0xB8);
      if (!c)
        return 0;
      unsigned int n = *(unsigned int const*)(c + 8);
      __int64 particles = *(__int64 const*)a, skinned = *(__int64 const*)d;
      if (!particles || !skinned || n == 0 || n > 65536)
        return 0;
      int count = n < (unsigned int)MaxParticles ? (int)n : MaxParticles;
      memcpy(g_Particles, (void const*)particles, (size_t)count * 16);
      memcpy(g_Skinned, (void const*)skinned, (size_t)count * 16);
      return count;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      return 0;
    }
  }

  // A new window (the cloths' last root places kept: a frame's move spans windows)
  void ResetWindow(ULONGLONG now)
  {
    WatchState& w = g_Watch;
    w.Since = now;
    w.Frames = w.FrozenFrames = w.HeadFrames = w.WinPlaying = w.Refound = w.ResetFrames = 0;
    w.HeadPath = 0.f;
    for (ClothWindow& c : w.Cloth)
    {
      ClothWindow fresh;
      fresh.Instance = c.Instance;
      memcpy(fresh.LastRoot, c.LastRoot, sizeof(fresh.LastRoot));
      fresh.LastRootOk = c.LastRootOk;
      c = fresh;
    }
  }

  // The window's line (truncating writes only: sprintf_s ends the game on an overflow)
  void WriteWindow(ULONGLONG now, char const* then)
  {
    WatchState const& w = g_Watch;
    char part[400];
    if (w.WinClipOn)
      _snprintf_s(part, sizeof(part), _TRUNCATE, "Hair watch (diagnostic): clip %s (%.0f fps; playing in %d of %d frames; "
        "%.2f - %.2f s into it)", w.WinFile, w.WinFps, w.WinPlaying, w.Frames, w.WinTimeFrom, w.WinTimeTo);
    else
      strcpy_s(part, "Hair watch (diagnostic): no clip");
    std::string text = part;
    _snprintf_s(part, sizeof(part), _TRUNCATE, "; last %.1f s, %d frames (%d frozen); ", (now - w.Since) / 1000.0, w.Frames,
      w.FrozenFrames);
    text += part;
    if (w.HeadFrames > 1)
      _snprintf_s(part, sizeof(part), _TRUNCATE, "her posed head moved %.2f m (%d frames known);", w.HeadPath, w.HeadFrames);
    else
      strcpy_s(part, "her posed head not known;");
    text += part;
    if (w.ResetFrames)
    {
      _snprintf_s(part, sizeof(part), _TRUNCATE, " the tools set her hair's reset flag in %d frames (a clip's jump);",
        w.ResetFrames);
      text += part;
    }
    if (w.Refound)
    {
      _snprintf_s(part, sizeof(part), _TRUNCATE, " her cloths looked for again %d times (one wasn't a cloth any more);",
        w.Refound);
      text += part;
    }
    if (!w.Found)
      text += " no cloth of hers found";
    for (int i = 0; i < WatchCloths; ++i)
    {
      ClothWindow const& c = w.Cloth[i];
      if (!c.Instance)
        continue;
      if (!c.Frames)
      {
        _snprintf_s(part, sizeof(part), _TRUNCATE, " cloth %d of %d: not read;", i + 1, w.Found);
        text += part;
        continue;
      }
      std::string lods, flags;
      for (int k = 0; k < c.LodsCount; ++k)
      {
        _snprintf_s(part, sizeof(part), _TRUNCATE, "%s%u", k ? "/" : "", c.LodsSeen[k]);
        lods += part;
      }
      if (c.LodsMore)
        lods += " +more";
      for (int k = 0; k < c.FlagsCount; ++k)
      {
        _snprintf_s(part, sizeof(part), _TRUNCATE, "%s%02X%02X%02X%02X%02X%02X%02X%02X", k ? " " : "", c.FlagsSeen[k][0],
          c.FlagsSeen[k][1], c.FlagsSeen[k][2], c.FlagsSeen[k][3], c.FlagsSeen[k][4], c.FlagsSeen[k][5], c.FlagsSeen[k][6],
          c.FlagsSeen[k][7]);
        flags += part;
      }
      if (c.FlagsMore)
        flags += " +more";
      char hair[160];
      if (c.ParticleFrames)
        _snprintf_s(hair, sizeof(hair), _TRUNCATE, "its particles from their skinned places: %.1f cm on average, at most "
          "%.1f cm (%d particles read, %d frames)", c.ParticleMeanSum / c.ParticleFrames * 100.0, c.ParticleMost * 100.f,
          c.ParticleCount, c.ParticleFrames);
      else
        strcpy_s(hair, "its particles not read");
      char step[120];
      if (c.StepLow <= c.StepHigh)
        _snprintf_s(step, sizeof(step), _TRUNCATE, "EA step %.4f - %.4f s%s", c.StepLow, c.StepHigh,
          c.StepMissed ? " (not read in some frames)" : "");
      else
        strcpy_s(step, "EA step not read (no simulation)");
      _snprintf_s(part, sizeof(part), _TRUNCATE, " cloth %d of %d: read in %d frames: LOD 0 in %d (LODs %s), off %d, "
        "reset %d, +18D %d, its root moved in %d (at most %.1f cm a frame), root speed at most %.2f m/s, %s, bytes +188: "
        "%s;", i + 1, w.Found, c.Frames, c.Lod0, lods.c_str(), c.Off, c.Reset, c.Flag18D, c.RootMoved,
        c.RootMoveMost * 100.f, c.SpeedMost, step, flags.c_str());
      text += part;
      text += " ";
      text += hair;
      text += ";";
    }
    if (then && then[0])
    {
      text += " Then ";
      text += then;
    }
    util::log::Write("%s", text.c_str());
  }
}

void paused_hair::Watch(__int64 soldier, bool timeFrozen, PoseState const& pose)
{
  WatchState& w = g_Watch;
  ULONGLONG now = GetTickCount64();
  if (!util::DiagnosticsOn() || !soldier || !pose.Held || !Available())
  {
    w.Found = 0;
    w.Since = 0;
    w.Frames = 0;
    w.HeadLastOk = false;
    w.WinClipOn = false;
    for (ClothWindow& c : w.Cloth)
      c = ClothWindow{};
    return;
  }
  if (!w.Since)
    ResetWindow(now);

  // A change of clip (on, off, another file) ends the window first, under its own clip
  bool changed = w.Frames > 0 && (pose.ClipOn != w.WinClipOn || (pose.ClipOn && strcmp(pose.File, w.WinFile) != 0));
  if (changed)
  {
    char then[160];
    if (pose.ClipOn)
      _snprintf_s(then, sizeof(then), _TRUNCATE, "clip %s (%.0f fps) on.", pose.File, pose.Fps);
    else
      strcpy_s(then, "the clip off.");
    WriteWindow(now, then);
    ResetWindow(now);
  }
  if (!w.Frames)
  {
    w.WinClipOn = pose.ClipOn;
    strcpy_s(w.WinFile, pose.ClipOn ? pose.File : "");
    w.WinFps = pose.Fps;
    w.WinTimeFrom = pose.Time;
  }
  w.WinTimeTo = pose.Time;
  w.WinPlaying += pose.Playing;

  // Her cloths, looked for again every 2 s, and at once when one isn't a cloth any more
  if (!w.Found || now - w.FoundAt >= 2000)
  {
    w.Found = FindCloths(soldier, w.Cloths);
    w.FoundAt = now;
    for (int i = 0; i < WatchCloths; ++i)
    {
      __int64 instance = i < w.Found ? w.Cloths[i] : 0;
      if (w.Cloth[i].Instance != instance)
      {
        w.Cloth[i] = ClothWindow{};
        w.Cloth[i].Instance = instance;
      }
    }
  }
  ++w.Frames;
  w.FrozenFrames += timeFrozen;
  w.ResetFrames += g_ResetThisFrame;
  if (pose.HeadOk)
  {
    ++w.HeadFrames;
    if (w.HeadLastOk)
    {
      float d[3] = { pose.Head[0] - w.HeadLast[0], pose.Head[1] - w.HeadLast[1], pose.Head[2] - w.HeadLast[2] };
      w.HeadPath += sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    }
    memcpy(w.HeadLast, pose.Head, sizeof(w.HeadLast));
  }
  w.HeadLastOk = pose.HeadOk;
  __int64 instanceVTable = (__int64)g_gameHandle + ClothInstanceVTableRva;
  for (ClothWindow& c : w.Cloth)
  {
    if (!c.Instance)
      continue;
    if (ReadQword(c.Instance) != instanceVTable)
    {
      // made again (an outfit change rebuilds her parts): looked for next frame
      c = ClothWindow{};
      w.Found = 0;
      ++w.Refound;
      continue;
    }
    unsigned int lod = 0;
    BYTE flags[8];
    float root[3], speed[4];
    if (!ReadClothNow(c.Instance, lod, flags, root, speed))
    {
      c.LastRootOk = false;
      continue;
    }
    ++c.Frames;
    c.Lod0 += lod == 0;
    c.Off += flags[3] != 0;     // + 0x18B
    c.Reset += flags[2] != 0;   // + 0x18A
    c.Flag18D += flags[5] != 0; // + 0x18D
    bool seen = false;
    for (int k = 0; k < c.LodsCount; ++k)
      seen |= c.LodsSeen[k] == lod;
    if (!seen)
    {
      if (c.LodsCount < Distinct)
        c.LodsSeen[c.LodsCount++] = lod;
      else
        c.LodsMore = true;
    }
    seen = false;
    for (int k = 0; k < c.FlagsCount; ++k)
      seen |= !memcmp(c.FlagsSeen[k], flags, 8);
    if (!seen)
    {
      if (c.FlagsCount < Distinct)
        memcpy(c.FlagsSeen[c.FlagsCount++], flags, 8);
      else
        c.FlagsMore = true;
    }
    if (c.LastRootOk)
    {
      float d[3] = { root[0] - c.LastRoot[0], root[1] - c.LastRoot[1], root[2] - c.LastRoot[2] };
      float moved = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
      if (moved > 1e-5f)
        ++c.RootMoved;
      if (moved > c.RootMoveMost && moved < 1e6f)
        c.RootMoveMost = moved;
    }
    memcpy(c.LastRoot, root, sizeof(c.LastRoot));
    c.LastRootOk = true;
    float s = sqrtf(speed[0] * speed[0] + speed[1] * speed[1] + speed[2] * speed[2]);
    if (s > c.SpeedMost && s < 1e6f)
      c.SpeedMost = s;
    int particles = ReadParticles(c.Instance);
    if (particles > 0)
    {
      double sum = 0.0;
      float most = 0.f;
      for (int k = 0; k < particles; ++k)
      {
        float d[3] = { g_Particles[k][0] - g_Skinned[k][0], g_Particles[k][1] - g_Skinned[k][1],
          g_Particles[k][2] - g_Skinned[k][2] };
        float distance = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        if (!(distance < 1e4f))
          continue;
        sum += distance;
        if (distance > most)
          most = distance;
      }
      ++c.ParticleFrames;
      c.ParticleCount = particles;
      c.ParticleMeanSum += sum / particles;
      if (most > c.ParticleMost)
        c.ParticleMost = most;
    }
    float step = LastStepOf(c.Instance);
    if (step < 0.f)
      ++c.StepMissed;
    else
    {
      if (step < c.StepLow)
        c.StepLow = step;
      if (step > c.StepHigh)
        c.StepHigh = step;
    }
  }

  // Once a second while a clip is on, every 5 s otherwise
  if (now - w.Since >= (w.WinClipOn ? 1000u : 5000u))
  {
    WriteWindow(now, "");
    ResetWindow(now);
  }
}
