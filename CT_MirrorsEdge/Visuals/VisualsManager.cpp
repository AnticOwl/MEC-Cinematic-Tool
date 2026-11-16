#include "VisualsManager.h"
#include "Weather.h"
#include "LensDistortion.h"
#include "FrameShot.h"
#include "../Main.h"
#include "../Util/Util.h"
#include "../imgui/imgui.h"
#include "../Util/ImGuiEXT.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace
{
  // The bounce light after a time of day jump. Enlighten (the light bouncing
  // off walls into the shade) solves its systems a few at a time, within a
  // budget per frame it reads from EnlightenRuntimeSettings every frame
  // (manager [0x14256AF40] + 0x105F8 is the registry's object): the solve
  // loop at 0x143791521 takes systems while MaxPerFrameSolveTime (+ 0x58, ms,
  // each system's cost taken off) lasts and fewer than JobCount (+ 0x68) jobs
  // are out; LightProbeMaxSourceSolveCount (+ 0x74, 0x14379193C) is shared out
  // over the queued probe work, each piece no more than it has left;
  // LightProbeMaxInstanceUpdateCount (+ 0x78, 0x14379B8F6) grows three lists
  // to that size before they're filled. So when the clock jumps from day to
  // night, the sky and the sun change at once but shaded walls stay lit and
  // fade over seconds (2026-10-23). Live, a normal level: 562 systems in 32
  // groups, a full pass 167 ms of their estimated cost, about 281 solves a
  // second at 5 ms (each system every ~2 s), 0 - 24 jobs out of 64. After a
  // jump only the solve time is raised, CatchUpSolveFactor times the game's
  // (no higher than MaxPerFrameSolveTimeMaximumBudget, + 0x60); the job count
  // and the probe counts stay the game's. With the game's maximums (128 ms,
  // 512 jobs, probes 8x) and the whole city loaded (3900 systems) the game
  // froze (2026-10-24 01:56): Enlighten's update (0x143793670) waiting at
  // 0x1437945AF for its jobs with 512 solves and 1688 probe tasks out, an
  // update the game never makes itself (the probes take whatever solve time
  // is left, with no count limit). A 4x test of all of them had run a whole
  // session on 2026-10-22; this is lighter than that.
  // MinSystemUpdateCount (+ 0x64) is left alone: it isn't a budget, systems
  // solved fewer times than it go first (0x143778450). The settings object is
  // found in the game's settings registry, the way CameraManager's
  // LookUpGameSettings does it (PE timestamp 0x5762E3B7 only).
  const DWORD KnownTimeStamp = 0x5762E3B7;
  const __int64 SettingsRegistryRva = 0x2374A88;
  const int SettingsBucketsOffset = 0xA8;
  const int SettingsBucketCountOffset = 0xB0;
  const __int64 EnlightenSettingsTypeRva = 0x28429D0;
  const int EnlSolveTimeOffset = 0x58;
  const int EnlSolveTimeLimitedOffset = 0x5C;
  const int EnlSolveTimeMaximumOffset = 0x60;
  const int EnlJobsOffset = 0x68;
  const int EnlJobsLimitedOffset = 0x6C;
  const int EnlJobsMaximumOffset = 0x70;
  const int EnlProbeSourcesOffset = 0x74;
  const int EnlProbeInstancesOffset = 0x78;
  const float CatchUpSolveFactor = 4.f;
  const ULONGLONG CatchUpMs = 3000;
  // A jump: within JumpWindowMs the clock moved more than ClockJumpSeconds
  // beyond what it may run on its own (NaturalClockRate game seconds a real
  // second; it ran 41, live 2026-10-24). So the menu, buttons and track cuts
  // count at once, a time-lapse faster than ~220 game seconds a second keeps
  // counting while it runs, and the game's own clock or a long frame never do.
  const float ClockJumpSeconds = 60.f;
  const float NaturalClockRate = 100.f;
  const ULONGLONG JumpWindowMs = 500;
  const float DaySeconds = 86400.f;
  // The game's own reset of the time of day's lighting (see
  // util::hooks::RequestTodReset) is asked for at once for a jump that comes
  // after ResetQuietMs without one. Jumps that keep coming (a slider dragged,
  // a time-lapse faster than ~220 game seconds a second: one at least every
  // JumpWindowMs) get one more ResetSettleMs after the last, not one each: a
  // reset starts the bounce light again from nothing, so it would never get
  // past its first passes.
  const ULONGLONG ResetQuietMs = 1000;
  const ULONGLONG ResetSettleMs = 800;
  // Reflections captured again once the reset's light is shown, a whole
  // cubemap a frame for this long (see RecaptureReflections)
  const ULONGLONG RecaptureMs = 4000;

  __int64 FindEnlightenSettings()
  {
    __int64 base = (__int64)g_gameHandle;
    __try
    {
      IMAGE_NT_HEADERS const* pNt = (IMAGE_NT_HEADERS const*)(base + ((IMAGE_DOS_HEADER const*)base)->e_lfanew);
      if (pNt->FileHeader.TimeDateStamp != KnownTimeStamp)
        return 0;
      __int64 type = base + EnlightenSettingsTypeRva;
      __int64 pRegistry = *(__int64*)(base + SettingsRegistryRva);
      if (!pRegistry) return 0;
      __int64* pBuckets = *(__int64**)(pRegistry + SettingsBucketsOffset);
      unsigned int count = *(unsigned int*)(pRegistry + SettingsBucketCountOffset);
      if (!pBuckets || !count) return 0;
      __int64 node = pBuckets[(unsigned int)type % count];
      for (int i = 0; node && i < 256; ++i)
      {
        if (*(__int64*)node == type)
          return *(__int64*)(node + 8);
        node = *(__int64*)(node + 0x10);
      }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
    return 0;
  }

  // counts: JobCount, its LimitedBudget, LightProbeMaxSourceSolveCount,
  // LightProbeMaxInstanceUpdateCount
  bool ReadEnlightenBudget(__int64 p, float& solveTime, float& solveTimeLimited, float& solveTimeMaximum, int* counts, int& jobsMaximum)
  {
    __try
    {
      solveTime = *(float*)(p + EnlSolveTimeOffset);
      solveTimeLimited = *(float*)(p + EnlSolveTimeLimitedOffset);
      solveTimeMaximum = *(float*)(p + EnlSolveTimeMaximumOffset);
      counts[0] = *(int*)(p + EnlJobsOffset);
      counts[1] = *(int*)(p + EnlJobsLimitedOffset);
      counts[2] = *(int*)(p + EnlProbeSourcesOffset);
      counts[3] = *(int*)(p + EnlProbeInstancesOffset);
      jobsMaximum = *(int*)(p + EnlJobsMaximumOffset);
      return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      return false;
    }
  }

  bool WriteEnlightenBudget(__int64 p, float solveTime, float solveTimeLimited, int const* counts)
  {
    __try
    {
      *(float*)(p + EnlSolveTimeOffset) = solveTime;
      *(float*)(p + EnlSolveTimeLimitedOffset) = solveTimeLimited;
      *(int*)(p + EnlJobsOffset) = counts[0];
      *(int*)(p + EnlJobsLimitedOffset) = counts[1];
      *(int*)(p + EnlProbeSourcesOffset) = counts[2];
      *(int*)(p + EnlProbeInstancesOffset) = counts[3];
      return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      return false;
    }
  }

  // The bigger solve time: CatchUpSolveFactor times the game's, no higher than
  // its maximum (and never below the game's value); a time the game leaves to
  // its default (0 or below) is left as it is
  float BoostTime(float value, float maximum)
  {
    if (!(value > 0.f))
      return value;
    float boosted = value * CatchUpSolveFactor;
    if (maximum > 0.f && boosted > maximum)
      boosted = maximum > value ? maximum : value;
    return boosted;
  }

  // More bounce light passes. In the manager's mode 1 (+ 0x10780, live)
  // Enlighten solves only what requests queue (0x143790F40 takes systems from
  // the queue at + 0x10788 until it's empty): a request (0x14378CF10(manager,
  // 3 flag bytes) -> its id; flags 0, 0, 0 = no hold for the sky update, no
  // reset of the light to black) is put on top of the request stack, and
  // 0x14379A7C0 takes the top one through states 0 - 1 - 2 (every system
  // queued once, 0x143795CF0) - 3 (the queues empty: off the stack). Its owner
  // reads the state (0x1437951F0(manager, &state, id), only for a live id) and
  // releases it once it's 3 (0x143790420(manager, id)). The game's own time of
  // day code files one such pass a step (0x143950920 -> 0x143950750, 1 pass),
  // so after a jump, or with the clock stopped, the bounce light got one pass
  // and kept part of the old light (2026-10-24, live: 0 solves for seconds
  // after a jump, then a single pass; 0 a second with the clock stopped).
  // The game's own requests come from its time of day lighting stepper
  // (0x143950920): each step is three one-pass jobs (+ 0x78 / + 0x94 / + 0xB0,
  // at its own target times, blended over three output buffers, 0x143797350),
  // and it starts the next step, with the new time and so the new sun, only
  // when all three are done (0x143950A0A - 0x143950ACC). The stack is last in,
  // first out, so a pass of ours filed while one of the game's waits keeps it
  // waiting; filing the next one at once kept it waiting through all of ours,
  // and the sun stayed at the old time (2026-10-24 02:22, live: 9 s after a
  // night -> day jump, until our passes ended; < 1 s when nothing of the game's
  // was waiting). So a pass is filed only while no request is pending at all
  // (the state map at + 0x90, begin / end).
  // These passes go into the output buffer the time of day's lighting isn't
  // showing yet (see util::hooks::RequestTodReset), so they're only the
  // fallback where its update isn't hooked; there its own reset is used.
  // After a jump CatchUpPasses more are filed, one at a time, each released
  // only once done; none in mode 0 (nothing there would finish them) or while
  // Enlighten is paused (+ 0x10848), and none filed later than PassesMaxMs
  // after the last jump: a safety limit, not the usual end (2026-10-24, live:
  // a pass ~0.3 s on a normal level, ~2.7 s with the whole city loaded, 5576
  // systems, so 12 take ~32 s there; 10 s had stopped them after 3 - 4).
  const __int64 EnlightenManagerRva = 0x256AF40;
  const __int64 FilePassRva = 0x378CF10;
  const __int64 PassStateRva = 0x37951F0;
  const __int64 ReleasePassRva = 0x3790420;
  const int EnlManagerModeOffset = 0x10780;
  const int EnlManagerPausedOffset = 0x10848;
  const int EnlRequestStatesOffset = 0x90;   // {id, state} pairs: begin, end
  const unsigned int PassDone = 3;
  const int CatchUpPasses = 12;
  const ULONGLONG PassesMaxMs = 45000;

  typedef unsigned int(__fastcall* FilePass_t)(__int64 manager, BYTE const* flags);
  typedef unsigned int*(__fastcall* PassState_t)(__int64 manager, unsigned int* state, unsigned int id);
  typedef void(__fastcall* ReleasePass_t)(__int64 manager, unsigned int id);

  // The three functions are the code the tools know (checked once)
  bool PassCodeKnown()
  {
    static int known = -1;
    if (known >= 0)
      return known == 1;
    known = 0;
    BYTE const* base = (BYTE const*)g_gameHandle;
    static const BYTE file[] = { 0x48, 0x89, 0xE0, 0x56, 0x57, 0x41, 0x56, 0x48, 0x83, 0xEC, 0x50, 0x48, 0xC7, 0x40, 0xC8, 0xFE,
      0xFF, 0xFF, 0xFF, 0x48, 0x89, 0x58, 0x18, 0x48, 0x89, 0x68, 0x20 };
    static const BYTE fileLock[] = { 0x48, 0x8D, 0x99, 0xC0, 0x10, 0x01, 0x00 };   // + 0x21: lea rbx, [rcx + 0x110C0]
    static const BYTE state[] = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x48, 0x89, 0x7C, 0x24, 0x18, 0x41,
      0x56, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8D, 0x99, 0xC0, 0x10, 0x01, 0x00 };
    static const BYTE release[] = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x18, 0x89, 0x54, 0x24, 0x10, 0x57, 0x48,
      0x83, 0xEC, 0x20, 0x48, 0x8D, 0x99, 0xC0, 0x10, 0x01, 0x00 };
    __try
    {
      IMAGE_NT_HEADERS const* pNt = (IMAGE_NT_HEADERS const*)(base + ((IMAGE_DOS_HEADER const*)base)->e_lfanew);
      if (pNt->FileHeader.TimeDateStamp == KnownTimeStamp && !memcmp(base + FilePassRva, file, sizeof(file)) &&
          !memcmp(base + FilePassRva + 0x21, fileLock, sizeof(fileLock)) && !memcmp(base + PassStateRva, state, sizeof(state)) &&
          !memcmp(base + ReleasePassRva, release, sizeof(release)))
        known = 1;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
    return known == 1;
  }

  // The live manager, or 0; whether it's in mode 1, whether it's paused
  __int64 PassManager(bool& modeOne, bool& paused)
  {
    modeOne = false;
    paused = false;
    __try
    {
      __int64 manager = *(__int64*)((__int64)g_gameHandle + EnlightenManagerRva);
      if (manager)
      {
        modeOne = *(int*)(manager + EnlManagerModeOffset) == 1;
        paused = *(BYTE*)(manager + EnlManagerPausedOffset) != 0;
      }
      return manager;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      return 0;
    }
  }

  // How many requests are still on the request stack (states 0 - 2; a done
  // one, 3, is off it and only waits for its owner to release it), -1 if it
  // can't be read. Read without the game's lock: at worst a frame stale, which
  // only moves a pass by a frame.
  int PendingRequests(__int64 manager)
  {
    __try
    {
      __int64 begin = *(__int64*)(manager + EnlRequestStatesOffset);
      __int64 end = *(__int64*)(manager + EnlRequestStatesOffset + 8);
      if (end < begin || end - begin > 0x80000)
        return -1;
      int pending = 0;
      for (__int64 p = begin; p < end && p < begin + 64 * 8; p += 8)
        if (*(unsigned int*)(p + 4) != PassDone)
          ++pending;
      return pending;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      return -1;
    }
  }

  bool FilePass(__int64 manager, unsigned int& id)
  {
    static const BYTE flags[4] = { 0, 0, 0, 0 };
    __try
    {
      id = ((FilePass_t)((__int64)g_gameHandle + FilePassRva))(manager, flags);
      return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      return false;
    }
  }

  bool PassState(__int64 manager, unsigned int id, unsigned int& state)
  {
    __try
    {
      ((PassState_t)((__int64)g_gameHandle + PassStateRva))(manager, &state, id);
      return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      return false;
    }
  }

  bool ReleasePass(__int64 manager, unsigned int id)
  {
    __try
    {
      ((ReleasePass_t)((__int64)g_gameHandle + ReleasePassRva))(manager, id);
      return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      return false;
    }
  }

  void FormatClock(float seconds, char* buffer, size_t size)
  {
    int s = (int)seconds;
    sprintf_s(buffer, size, "%02d:%02d", (s / 3600) % 24, (s / 60) % 60);
  }
  // MAIN + 0x50 -> +0x90 -> +0x1C0 -> +0x30 is the visual environment state
  // manager. Its current state starts at +0x85470, the DoF method lives in
  // the render settings at +0x138.
  const int StateOffset = 0x85470;
  // The render settings are the game's PostProcess settings (its reflected
  // fields, offline 2026-10-26): DofMethod + 0x154 (DofMethod_Gaussian 0,
  // DofMethod_Sprite 1, nothing else), BlurMethod + 0x158 (the same two),
  // SpriteDofEnable + 0x247. While SpriteDofEnable is off the game puts both
  // methods back to Gaussian (0x143759409); its PC settings turn it off on
  // High and Ultra (initfs, "Disable sprite dof on high and ultra"), so the
  // game itself never draws the sprite depth of field.
  const int RenderSettingsOffset = 0x138;
  const int DofMethodOffset = 0x154;   // int, in the render settings
  const int SpriteDofOffset = 0x247;   // bool, SpriteDofEnable: on with the Sprite method here

  // The sprite depth of field's crash guard (see InstallSpriteDofGuard). The
  // sprite pass (0x1435C6810, run when DofMethod is Sprite and the depth of
  // field is on, or BlurMethod is Sprite and a blur is wanted, 0x14375AF81)
  // looks at the first of its targets (+ 0x3B8, six made null by the
  // constructor 0x1435AF760) to see whether they have to be made again, but
  // only makes them after that look (0x1435C755F): so its first frame read a
  // null pointer and the game crashed at 0x1435C72EE (2026-10-26, CT.log:
  // reading 0x8, rax 0). Offsets from OFFSET_DOFFIX (0x1435C72EE).
  const int GuardSiteFromFix = -0xA;      // 0x1435C72E4: lea rbx, [rdi + 0x3B8]; the jump covers 14 bytes, to + 4
  const int GuardBackFromFix = 4;         // 0x1435C72F2: after mov rcx, [rax + 8]
  const int GuardRebuildFromFix = 0x7E;   // 0x1435C736C: mov eax, 6, where the targets are made again
  const int GuardFlag = 0x80, GuardCount = 0x84;   // in the guard's code page
  const BYTE GuardSiteBytes[] = { 0x48, 0x8D, 0x9F, 0xB8, 0x03, 0x00, 0x00, 0x48, 0x8B, 0x03, 0x48, 0x8B, 0x48, 0x08,
    0x48, 0x8B, 0x01, 0xFF, 0x50, 0x58 };
  const BYTE GuardRebuildBytes[] = { 0xB8, 0x06, 0x00, 0x00, 0x00 };

  // The exposure after a time of day jump. The tonemap is at state + 0x1D0
  // (the exposure code 0x1435BE3A0 gets view state + 0x84B40 from
  // 0x14374689B, the same place for the main view), DarkAdaptationTime + 0xA4
  // and LightAdaptationTime + 0xA8 in seconds. Each frame the exposure (kept
  // per view at + 0x468 / + 0x46C / + 0x470) moves toward its target by
  // min(frame time / time, 1), and goes straight there when both times are
  // 0.01 or less (0x1435BE585), with nothing divided.
  const int TonemapStateOffset = 0x1D0;
  const int AdaptationTimesOffset = 0xA4;   // dark, then light at + 0xA8
  const float InstantAdaptation = 0.01f;

  // times: dark, light
  bool ReadAdaptationTimes(__int64 pState, float* times)
  {
    __try
    {
      float const* p = (float const*)(pState + TonemapStateOffset + AdaptationTimesOffset);
      times[0] = p[0];
      times[1] = p[1];
      return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      return false;
    }
  }

  // Only where our InstantAdaptation still is, when restore is set
  bool WriteAdaptationTimes(__int64 pState, float const* times, bool restore)
  {
    __try
    {
      float* p = (float*)(pState + TonemapStateOffset + AdaptationTimesOffset);
      for (int i = 0; i < 2; ++i)
        if (!restore || p[i] == InstantAdaptation)
          p[i] = times[i];
      return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      return false;
    }
  }

  __int64 ReadPointer(__int64 address)
  {
    if (!address) return 0;
    return *(__int64*)address;
  }

  typedef VisualsManager::FieldType T;

  // The game's enums behind the Int fields (their reflected type info,
  // offline 2026-10-26: SimpleDofBlurFilter is a BlurFilter, TonemapMethod,
  // BloomMethod and BloomDirection their own), values 0 up in order
  const char* const BlurFilterItems = "None\0Gaussian 3 pixels\0Gaussian 5 pixels\0Gaussian 7 pixels\0Gaussian 9 pixels\0"
    "Gaussian 15 pixels\0Gaussian 31 pixels\0";
  const char* const TonemapMethodItems = "Linear\0Filmic\0Filmic neutral\0Linear, approximate gamma\0Parametric\0Filmic AD\0"
    "Filmic AD debug\0None\0";
  const char* const BloomMethodItems = "Gaussian simple\0Gaussian custom\0FFT\0";
  const char* const BloomDirectionItems = "None\0Horizontal\0Vertical\0";

  // Filmic effects' lens distortion (see VisualsManager::ApplyLensDistortion)
  const int FilmicEnableOffset = 0x80;
  const int FilmicLensOffset = 0x83;
  const int FilmicLensGainOffset = 0x24;
  const int FilmicLensCubicOffset = 0x28;

  // The depth of field's distances (see its component's fields): the focus
  // distance; Simple near start, near end, far start, far end; Sprite the same
  const int AutoFocusOffsets[9] = { 0x04, 0x18, 0x1C, 0x20, 0x24, 0x38, 0x3C, 0x40, 0x44 };
  const int AutoFocusCount = 9;

  // The distance a depth of field set-up is made for: its focus distance, or
  // with none, the middle of the sharp range of the method in use (its near
  // and far distances are the focus' offsets, see DrawDofUI: Simple sharp
  // from near end - 2 near start to far start, Sprite from - near start to far
  // start), or 10 m
  float AutoFocusReference(float const* base, bool sprite)
  {
    if (base[0] > 0.05f)
      return base[0];
    float middle = sprite ? (base[7] - base[5]) * 0.5f : (base[2] - 2.f * base[1] + base[3]) * 0.5f;
    return middle > 0.05f ? middle : 10.f;
  }

  // The depth of field's fields (DofComponentData - 0x80, its type info) and
  // the game's gap between each start and end (0x1435BFDA7 Simple, 0x1435C6A24
  // Sprite: an end is moved to its start + 0.01 m)
  const int DofEnableOffset = 0x58, DofFocusOffset = 0x04, DofBlurFactorOffset = 0x08, DofBlurAddOffset = 0x0C,
    DofSimpleFilterOffset = 0x10, DofSimpleMaxBlurOffset = 0x14, DofSimpleNearOffset = 0x18, DofSimpleFarOffset = 0x20,
    DofSpriteNearOffset = 0x38, DofSpriteFarOffset = 0x40, DofSpriteMaxBlurOffset = 0x48;
  const float DofEndGap = 0.01f;

  int FieldSize(T type)
  {
    switch (type)
    {
    case T::Bool:   return 1;
    case T::Float2: return 8;
    case T::Float3:
    case T::Float3Linked:
    case T::Color3: return 12;
    case T::Float4: return 16;
    default:        return 4;
    }
  }

  // Reflections with the new light. 0x1436F80C0 (the world renderer's update
  // calls it at 0x14373EB93 with its + 0x8C8 object and the state + 0x85470)
  // compares the state's sun (rotation X / Y + 0x84 / + 0x88, as a direction)
  // and + 0x20 with the ones it stored (+ 0x70 / + 0x60) whenever the state's
  // version (+ 0x1BC8) isn't the one it stored (+ 0x80), not while a capture
  // runs; past LocalIBLSunUpdateThreshold it has every reflection volume
  // captured again (0x1436FC9E0) and stores the new ones. A capture takes the
  // bounce light as it is then, so the ones the jump's own sun change starts
  // keep light from before the reset's passes. Once those are done the stored
  // direction is cleared and the stored version made another: the game's own
  // test has them all captured again at its next look. Plain stores from the
  // game thread while the render thread may read them; a store of its own at
  // the same moment comes with a capture of them all anyway.
  const __int64 ReflectionTestCallerRva = 0x373EB1D;
  const __int64 ReflectionTestRva = 0x36F80C0;
  const int WrReflectionsOffset = 0x8C8;
  const int ReflStoredSunOffset = 0x70;
  const int ReflStoredVersionOffset = 0x80;
  const int StateVersionOffset = 0x1BC8;

  bool ReflectionTestKnown()
  {
    static int known = -1;
    if (known >= 0)
      return known == 1;
    known = 0;
    BYTE const* base = (BYTE const*)g_gameHandle;
    static const BYTE object[] = { 0x48, 0x8B, 0x8B, 0xC8, 0x08, 0x00, 0x00 };          // 0x14373EB1D: mov rcx, [rbx + 0x8C8]
    static const BYTE state[] = { 0x4C, 0x8D, 0x8B, 0x70, 0x54, 0x08, 0x00 };           // 0x14373EB85: lea r9, [rbx + 0x85470]
    static const BYTE version[] = { 0x8B, 0x83, 0x80, 0x00, 0x00, 0x00, 0x39, 0x85, 0xC8, 0x1B, 0x00, 0x00 };   // 0x1436F81F3
    static const BYTE sun[] = { 0x0F, 0x28, 0x43, 0x70 };                                // 0x1436F82C2: movaps xmm0, [rbx + 0x70]
    static const BYTE store[] = { 0x66, 0x0F, 0x7F, 0x73, 0x70 };                        // 0x1436F8310: movdqa [rbx + 0x70], xmm6
    static const BYTE storeVersion[] = { 0x8B, 0x85, 0xC8, 0x1B, 0x00, 0x00, 0x89, 0x83, 0x80, 0x00, 0x00, 0x00 };   // 0x1436F8327
    __try
    {
      int rel;
      memcpy(&rel, base + 0x373EB94, 4);
      IMAGE_NT_HEADERS const* pNt = (IMAGE_NT_HEADERS const*)(base + ((IMAGE_DOS_HEADER const*)base)->e_lfanew);
      if (pNt->FileHeader.TimeDateStamp == KnownTimeStamp && !memcmp(base + ReflectionTestCallerRva, object, sizeof(object)) &&
          !memcmp(base + 0x373EB85, state, sizeof(state)) && base[0x373EB93] == 0xE8 && 0x373EB98 + rel == ReflectionTestRva &&
          !memcmp(base + 0x36F81F3, version, sizeof(version)) && !memcmp(base + 0x36F82C2, sun, sizeof(sun)) &&
          !memcmp(base + 0x36F8310, store, sizeof(store)) && !memcmp(base + 0x36F8327, storeVersion, sizeof(storeVersion)))
        known = 1;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
    return known == 1;
  }

  bool RecaptureReflections(__int64 worldRenderer)
  {
    if (!worldRenderer || !ReflectionTestKnown())
      return false;
    __try
    {
      __int64 p = *(__int64*)(worldRenderer + WrReflectionsOffset);
      if (!p || !*(__int64*)(p + 8))
        return false;
      volatile float* pSun = (volatile float*)(p + ReflStoredSunOffset);
      for (int i = 0; i < 4; ++i)
        pSun[i] = 0.f;
      *(volatile unsigned int*)(p + ReflStoredVersionOffset) = ~*(unsigned int const*)(worldRenderer + StateOffset + StateVersionOffset);
      return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      return false;
    }
  }

  // What marks every reflection to be captured again: 0x1436FC9E0 sets the
  // waiting bit (+ 0xC0 |= 4) on each volume of [0x142568CE0] + 0xD8 ..
  // + 0xE0 (0xD0 each). Its two callers (offline): the sun test above, which
  // stores the sun (+ 0x70) as it does, and the sky cubemap's capture
  // (0x1436FAC36 in 0x1436FAB20, with LocalIBLUpdateWithEnlightenSkyBoxChange
  // on). Read during the catch-up only, to log what starts the reflections
  // over (VisualsManager::NoteReflectionMarks).
  const __int64 ReflVolumesRva = 0x2568CE0;
  const int ReflVolumesBeginOffset = 0xD8;
  const int ReflVolumesEndOffset = 0xE0;
  const int ReflVolumeSize = 0xD0;
  const int ReflVolumeFlagsOffset = 0xC0;
  const unsigned int ReflVolumeWaiting = 4;
  const int MarkLogsMax = 8;

  bool ReflectionMarksKnown()
  {
    static int known = -1;
    if (known >= 0)
      return known == 1;
    known = 0;
    BYTE const* base = (BYTE const*)g_gameHandle;
    // mov rcx, [r8 + 0xd8]; lea rax, [rax + 0xd0]; or dword [rax + rcx - 0x10], 4
    static const BYTE mark[] = { 0x49, 0x8B, 0x88, 0xD8, 0x00, 0x00, 0x00, 0x48, 0x8D, 0x80, 0xD0, 0x00, 0x00, 0x00, 0x83, 0x4C, 0x08,
      0xF0, 0x04 };
    // the sky path: mov rax, [rbx + 0x18]; cmp byte [rax + 0x481], 0; je; call 0x1436FC9E0
    static const BYTE sky[] = { 0x48, 0x8B, 0x43, 0x18, 0x80, 0xB8, 0x81, 0x04, 0x00, 0x00, 0x00, 0x74, 0x05, 0xE8, 0xA5, 0x1D, 0x00,
      0x00 };
    __try
    {
      // mov r8, [rip + rel] at 0x1436FC9E0: the volume list
      int rel;
      memcpy(&rel, base + 0x36FC9E3, 4);
      if (ReflectionTestKnown() && base[0x36FC9E0] == 0x4C && base[0x36FC9E1] == 0x8B && base[0x36FC9E2] == 0x05 &&
          0x36FC9E7 + rel == ReflVolumesRva && !memcmp(base + 0x36FCA18, mark, sizeof(mark)) &&
          !memcmp(base + 0x36FAC29, sky, sizeof(sky)))
        known = 1;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
    return known == 1;
  }

  // How many reflection volumes are waiting to be captured, of how many
  bool CountWaitingReflections(int& waiting, int& count)
  {
    waiting = count = 0;
    if (!ReflectionMarksKnown())
      return false;
    __try
    {
      __int64 list = *(__int64 const*)((__int64)g_gameHandle + ReflVolumesRva);
      if (!list)
        return false;
      __int64 begin = *(__int64 const*)(list + ReflVolumesBeginOffset);
      __int64 end = *(__int64 const*)(list + ReflVolumesEndOffset);
      if (!begin || end < begin || (end - begin) % ReflVolumeSize != 0 || (end - begin) / ReflVolumeSize > 20000)
        return false;
      count = (int)((end - begin) / ReflVolumeSize);
      for (int i = 0; i < count; ++i)
        if (*(unsigned int const*)(begin + (__int64)i * ReflVolumeSize + ReflVolumeFlagsOffset) & ReflVolumeWaiting)
          ++waiting;
      return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      return false;
    }
  }

  // The sun direction the sun test stored last (see RecaptureReflections)
  bool ReadStoredSun(__int64 worldRenderer, float* sun)
  {
    if (!worldRenderer || !ReflectionTestKnown())
      return false;
    __try
    {
      __int64 p = *(__int64*)(worldRenderer + WrReflectionsOffset);
      if (!p)
        return false;
      for (int i = 0; i < 3; ++i)
        sun[i] = *(volatile float const*)(p + ReflStoredSunOffset + i * 4);
      return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      return false;
    }
  }
}

VisualsManager::VisualsManager() :
  m_DofMethodHasValue(false),
  m_DofMethod(0),
  m_SpriteDofFix(false),
  m_SpriteDofOriginalBytes{ 0, 0 }
{
  m_ppMain = util::offsets::GetOffset("OFFSET_MAIN");
  m_ppClientGameContext = util::offsets::GetOffset("OFFSET_CLIENTGAMECONTEXT");
  m_pSpriteDofFix = util::offsets::GetOffset("OFFSET_DOFFIX");
  m_ppTimeOfDay = util::offsets::GetOffset("OFFSET_TIMEOFDAY");

  // Offsets are relative to each component, taken from the original GUI's
  // Read/Write methods. Component offsets are the GUI's shared memory
  // offsets minus 0x10, which is where they sit in the game's state.
  m_Components.push_back({ "Color correction", 0x330, 0x50, {
    { "Enable", 0x44, T::Bool, 0 },
    { "Brightness", 0x00, T::Float3Linked, 0.005f },
    { "Contrast", 0x10, T::Float3Linked, 0.005f },
    { "Saturation", 0x20, T::Float3Linked, 0.005f },
    { "Hue", 0x34, T::Float, 0.5f },
    { "Color grading", 0x45, T::Bool, 0 },
    { "Color grading max HDR", 0x40, T::Float, 0.05f },
  } });

  m_Components.push_back({ "Depth of field", 0xA50, 0x60, {
    { "Enable", 0x58, T::Bool, 0 },
    { "Focus distance", 0x04, T::Float, 0.05f },
    { "Blur factor", 0x08, T::Float, 0.01f },
    { "Blur add", 0x0C, T::Float, 0.01f },
    { "Simple blur filter", 0x10, T::Int, 0, BlurFilterItems },
    { "Simple max blur", 0x14, T::Float, 0.01f },
    { "Simple near start", 0x18, T::Float, 0.05f },
    { "Simple near end", 0x1C, T::Float, 0.05f },
    { "Simple far start", 0x20, T::Float, 0.05f },
    { "Simple far end", 0x24, T::Float, 0.05f },
    { "Sprite near start", 0x38, T::Float, 0.05f },
    { "Sprite near end", 0x3C, T::Float, 0.05f },
    { "Sprite far start", 0x40, T::Float, 0.05f },
    { "Sprite far end", 0x44, T::Float, 0.05f },
    { "Sprite max blur", 0x48, T::Float, 0.01f },
    { "Hip to ironsights fade", 0x28, T::Float, 0.01f },
    { "Ironsights DoF start fade", 0x2C, T::Float, 0.01f },
    { "Ironsights focal distance", 0x30, T::Float, 0.05f },
    { "Ironsights circle distance", 0x34, T::Float, 0.05f },
    { "Ironsights DoF active", 0x5B, T::Bool, 0 },
    { "Ironsights circle blur", 0x5C, T::Bool, 0 },
    // UseCameraSettings: with it on (and the view's own camera settings, view
    // + 0xCDC) both methods take the camera's physical settings instead of
    // these (0x1437466AD, 0x143757F71), the focus distance with them
    { "Use the camera's own settings", 0x59, T::Bool, 0 },
  }, 15 }); // Drawn by DrawDofUI

  m_Components.push_back({ "Film grain", 0xB00, 0x30, {
    { "Enable", 0x28, T::Bool, 0 },
    { "Color scale", 0x00, T::Float3, 0.005f },
    { "Texture scale", 0x14, T::Float2, 0.01f },
    { "Linear filtering", 0x29, T::Bool, 0 },
    { "Random", 0x2A, T::Bool, 0 },
  } });

  m_OutdoorIndex = m_Components.size();
  m_Components.push_back({ "Outdoor light", 0x000, 0x120, {
    { "Enable", 0x118, T::Bool, 0 },
    { "Sun color", 0x00, T::Color3, 0 },
    { "Sun intensity", 0x94, T::Float, 0.05f },
    { "Sun rotation X", 0x84, T::Float, 0.5f },
    { "Sun rotation Y", 0x88, T::Float, 0.5f },
    // Frostbite's ShadowSunRotationEnable: on = shadows use their own direction
    // (the two "Shadow direction" values below), off = they follow the sun
    { "Separate shadow direction", 0x119, T::Bool, 0 },
    { "Sun angular radius", 0x98, T::Float, 0.005f },
    { "Sun disk intensity", 0x9C, T::Float, 0.05f },
    { "Sun specular scale", 0xA4, T::Float, 0.01f },
    { "Sun penumbra size", 0xC4, T::Float, 0.01f },
    { "Sky color", 0x60, T::Color3, 0 },
    { "Ground color", 0x70, T::Color3, 0 },
    { "Sky light angle factor", 0xA0, T::Float, 0.01f },
    // From the game's OutdoorLightComponentData (data offset - 0x80 = state offset)
    { "Shadow softness (PCSS)", 0xC8, T::Float, 0.01f },
    { "Shadow direction X", 0x8C, T::Float, 0.5f },
    { "Shadow direction Y", 0x90, T::Float, 0.5f },
    { "Sun shadow height scale", 0xAC, T::Float, 0.01f },
    { "Sky reflection shadow scale", 0xA8, T::Float, 0.01f },
    // Final sun luminance / illuminance (+ 0x10 / + 0x20) and the outer space
    // ones (+ 0x30 - + 0x50) are out: the game works them out from the sun's
    // color, intensity, angular radius and rotation (see RecomputeSun), so
    // they're never the menu's to set
    { "Translucency ambient", 0x100, T::Float, 0.01f },
    { "Translucency scale", 0x104, T::Float, 0.01f },
    { "Translucency power", 0x108, T::Float, 0.01f },
    { "Translucency distortion", 0x10C, T::Float, 0.01f },
    { "Particle sun shadow factor", 0x110, T::Float, 0.01f },
    { "Particle sun shadow smoothing", 0x114, T::Float, 0.01f },
    { "Sun PCSS filter adaptive", 0x11A, T::Bool, 0 },
    { "Cloud shadows", 0x11B, T::Bool, 0 },
    { "Cloud shadows top-down", 0x11C, T::Bool, 0 },
  }, 14 }); // Through "Shadow softness"

  m_Components.push_back({ "Tonemap", 0x1D0, 0x150, {
    { "Tonemap method", 0xC0, T::Int, 0, TonemapMethodItems },
    { "Exposure compensation", 0x98, T::Float, 0.01f },
    { "Automatic exposure", 0x148, T::Bool, 0 },
    { "EV", 0x94, T::Float, 0.01f },
    { "Clamp EV", 0x149, T::Bool, 0 },
    { "Min EV", 0xB4, T::Float, 0.01f },
    { "Max EV", 0xB8, T::Float, 0.01f },
    { "Bloom scale", 0x00, T::Float3, 0.005f },
    { "Auto exposure darkest exclude", 0x9C, T::Float, 0.005f },
    { "Auto exposure brightest exclude", 0xA0, T::Float, 0.005f },
    { "Auto exposure higher threshold", 0xAC, T::Float, 0.01f },
    { "Auto exposure lower threshold", 0xB0, T::Float, 0.01f },
    { "Dark adaptation time", 0xA4, T::Float, 0.01f },
    { "Light adaptation time", 0xA8, T::Float, 0.01f },
    { "Spot meter scale", 0xBC, T::Float, 0.01f },
    { "Black point", 0xC4, T::Float, 0.005f },
    { "White point", 0xC8, T::Float, 0.005f },
    { "Cross over point", 0xCC, T::Float, 0.005f },
    { "Toe strength", 0xD0, T::Float, 0.005f },
    { "Shoulder strength", 0xD4, T::Float, 0.005f },
    { "Split screen", 0xD8, T::Float, 0.005f },
    { "Black in", 0xDC, T::Float, 0.005f },
    { "White in", 0xE0, T::Float, 0.005f },
    { "Black out", 0xE4, T::Float, 0.005f },
    { "White out", 0xE8, T::Float, 0.005f },
    { "White level", 0xEC, T::Float, 0.005f },
    { "White clip", 0xF0, T::Float, 0.005f },
    { "Bloom method", 0xF4, T::Int, 0, BloomMethodItems },
    { "Bloom direction", 0xF8, T::Int, 0, BloomDirectionItems },
    { "Gaussian clamp", 0xFC, T::Float, 0.01f },
    { "Gaussian sharpness", 0x100, T::Float, 0.01f },
    { "FFT threshold", 0x118, T::Float, 0.01f },
    { "FFT cutoff", 0x11C, T::Float, 0.01f },
    { "FFT kernel scale", 0x120, T::Float, 0.01f },
    { "FFT kernel rotation", 0x124, T::Float, 0.5f },
    { "FFT spike scale limit", 0x128, T::Float, 0.01f },
    { "FFT spike scale limit enable", 0x14A, T::Bool, 0 },
    { "Chromostereopsis", 0x14B, T::Bool, 0 },
    { "Chromostereopsis scale", 0x138, T::Float, 0.01f },
    { "Chromostereopsis offset", 0x13C, T::Float, 0.01f },
  }, 8 });

  m_Components.push_back({ "Vignette", 0xAC0, 0x40, {
    { "Enable", 0x24, T::Bool, 0 },
    { "Color", 0x00, T::Color3, 0 },
    { "Scale", 0x14, T::Float2, 0.01f },
    { "Exponent", 0x1C, T::Float, 0.01f },
    { "Opacity", 0x20, T::Float, 0.005f },
  } });

  // FogComponentData's fields minus 0x80. The curves are cubics over the
  // distance between their start and end (t^3, t^2, t, 1): the game's own
  // transparency fade is -2 3 0 0, a smooth step. The game's fog set-up
  // (0x1437065E0, given the state's + 0x620) passes the shader the distance
  // curve only while "Fog gradient" is on and the fog color's curve only
  // while "Use the fog color" is on; the fog color, start and end always
  // (2026-10-25, offline).
  m_FogIndex = m_Components.size();
  m_Components.push_back({ "Fog", 0x620, 0xB0, {
    { "Enable", 0x9C, T::Bool, 0 },
    { "Start", 0x58, T::Float, 0.5f },
    { "End", 0x5C, T::Float, 0.5f },
    { "Curve", 0x00, T::Float4, 0.01f },
    { "Use the fog color", 0x9E, T::Bool, 0 },
    { "Fog color", 0x10, T::Color3, 0 },
    { "Height fog", 0xA0, T::Bool, 0 },
    { "Forward light scattering", 0x9F, T::Bool, 0 },
    { "Distance multiplier", 0x54, T::Float, 0.01f },
    { "Fog gradient (uses the curve)", 0x9D, T::Bool, 0 },
    { "Fog color start", 0x60, T::Float, 0.5f },
    { "Fog color end", 0x64, T::Float, 0.5f },
    { "Fog color curve", 0x20, T::Float4, 0.01f },
    { "Height fog altitude", 0x90, T::Float, 0.5f },
    { "Height fog depth", 0x94, T::Float, 0.5f },
    { "Height fog visibility range", 0x98, T::Float, 5.f },
    { "Height fog follow camera", 0x8C, T::Float, 0.01f },
    { "Transparency fade start", 0x68, T::Float, 0.5f },
    { "Transparency fade end", 0x6C, T::Float, 0.5f },
    { "Transparency fade clamp", 0x70, T::Float, 0.01f },
    { "Transparency fade curve", 0x30, T::Float4, 0.01f },
    { "Light scattering color", 0x40, T::Color3, 0 },
    { "Light scattering phase G", 0x74, T::Float, 0.01f },
    { "Light scattering strength", 0x78, T::Float, 0.01f },
    { "Light scattering presence", 0x7C, T::Float, 0.005f },
    { "Light scattering max blur length", 0x80, T::Float, 0.005f },
    { "Light scattering extinction", 0x84, T::Float, 0.05f },
    { "Light scattering smoothness", 0x88, T::Float, 0.05f },
  }, 8 }); // Through "Forward light scattering"

  // FilmicEffectsComponentData's fields minus 0x80 (its type info, offline
  // 2026-10-25), at state + 0x9B0: the game's renderer (0x14374669E) hands
  // context + 0x85320 on as the filmic effects, beside + 0x853C0 the depth of
  // field (state + 0xA50), + 0x85430 the vignette (+ 0xAC0) and + 0x85470
  // the film grain (+ 0xB00), and the post-process (0x1435BF526) reads its
  // + 0x80 Enable, + 0x81 / + 0x14 / + 0x18 the chromatic aberration and
  // + 0x83 / + 0x24 / + 0x28 the lens distortion from it (the lens
  // distortion is drawn by the tools instead, see ApplyLensDistortion). The
  // sprite depth of field reads + 0x86 / + 0x34 / + 0x38 the distance blur and
  // + 0x87 / + 0x3C / + 0x44 / + 0x48 the edge blur (0x1435C6D9E, the block
  // given by 0x143757E8E; whether to run it, 0x1435CE330). Nothing else of it
  // is read (2026-10-26, offline: those are the only three places that take
  // the block's address, no code reaches its other fields directly, and no
  // system shader has a parameter for them), so its vignetting, frame flash,
  // depth flash, heat ripple and the edge blur's depth target scale and matte
  // sizes did nothing and are out of the menu. + 0x78 is a texture (not
  // written).
  m_FilmicIndex = m_Components.size();
  m_Components.push_back({ "Filmic effects (lens distortion, chromatic aberration)", 0x9B0, 0x90, {
    { "Enable", FilmicEnableOffset, T::Bool, 0 },
    { "Lens distortion", FilmicLensOffset, T::Bool, 0 },
    { "Lens distortion gain", FilmicLensGainOffset, T::Float, 0.005f },
    { "Lens distortion cubic gain", FilmicLensCubicOffset, T::Float, 0.005f },
    { "Chromatic aberration", 0x81, T::Bool, 0 },
    // A factor, 1 = none (2026-11-05, offline: 0x1435C9E63 and the tonemap
    // shader's chromostereopsisParams): the red is sampled at t.x / (1 + a (s - 1)),
    // t.y / s from the frame's centre, the green at half that, the blue where
    // it is. The city's own: 1.005, aspect 0.5. Below 1 the red shrinks (0.1
    // pulled the edges into long streaks).
    { "Chromatic aberration scale (1 = none)", 0x14, T::Float, 0.0005f },
    { "Chromatic aberration aspect ratio", 0x18, T::Float, 0.01f },
    { "Distance blur", 0x86, T::Bool, 0 },
    { "Distance blur gain", 0x34, T::Float, 0.01f },
    { "Distance blur half distance", 0x38, T::Float, 0.5f },
    { "Edge blur", 0x87, T::Bool, 0 },
    { "Edge blur gain", 0x3C, T::Float, 0.01f },
    { "Edge blur fade near depth", 0x44, T::Float, 0.05f },
    { "Edge blur fade far depth", 0x48, T::Float, 0.05f },
  }, 7 }); // Through "Chromatic aberration aspect ratio"

  // The sky's two cloud layers (SkyComponentData; the Sky block is at state
  // + 0x390, its fields at data - 0x80, the sky's blend 0x14471D5D0 writes
  // each of these there; 2026-10-25, offline). The textures, the mask and the
  // sky type stay the game's.
  m_Components.push_back({ "Clouds", 0x390, 0x290, {
    { "Layer 1 opacity", 0x170, T::Float, 0.01f },
    { "Layer 2 opacity", 0x1AC, T::Float, 0.01f },
    { "Layer 1 speed", 0x160, T::Float, 0.001f },
    { "Layer 2 speed", 0x19C, T::Float, 0.001f },
    { "Cloud wind direction", 0x150, T::Float, 0.5f },
    { "Layer 1 altitude", 0x154, T::Float, 10.f },
    { "Layer 2 altitude", 0x190, T::Float, 10.f },
    { "Layer 1 thickness", 0x18C, T::Float, 1.f },
    { "Layer 2 thickness", 0x1C4, T::Float, 1.f },
    { "Cloud sun color", 0x00, T::Color3, 0 },
    { "Layer 1 color", 0x10, T::Color3, 0 },
    { "Layer 2 color", 0x20, T::Color3, 0 },
    { "Layer 1 tile factor", 0x158, T::Float, 0.01f },
    { "Layer 1 rotation", 0x15C, T::Float, 0.5f },
    { "Layer 1 sun light intensity", 0x164, T::Float, 0.01f },
    { "Layer 1 sun light power", 0x168, T::Float, 0.01f },
    { "Layer 1 ambient light intensity", 0x16C, T::Float, 0.01f },
    { "Layer 1 absorption", 0x180, T::Float, 0.01f },
    { "Layer 1 scattering", 0x184, T::Float, 0.01f },
    { "Layer 1 phase", 0x188, T::Float, 0.01f },
    { "Layer 2 tile factor", 0x194, T::Float, 0.01f },
    { "Layer 2 rotation", 0x198, T::Float, 0.5f },
    { "Layer 2 sun light intensity", 0x1A0, T::Float, 0.01f },
    { "Layer 2 sun light power", 0x1A4, T::Float, 0.01f },
    { "Layer 2 ambient light intensity", 0x1A8, T::Float, 0.01f },
    { "Layer 2 absorption", 0x1B8, T::Float, 0.01f },
    { "Layer 2 scattering", 0x1BC, T::Float, 0.01f },
    { "Layer 2 phase", 0x1C0, T::Float, 0.01f },
  }, 12 }); // Through "Layer 2 color"

  // The wind (WindComponentData; its block is at state + 0x6E0: the visual
  // environment mix 0x143703D60 gives it to 0x14471FB90, which copies the
  // Realm at + 0x00 and blends these seven behind override bits 1 - 7, the
  // same bits the game's environments set for these fields; 2026-10-25,
  // offline). Direction in degrees.
  m_Components.push_back({ "Wind", 0x6E0, 0x20, {
    { "Direction", 0x04, T::Float, 1.f },
    { "Strength", 0x08, T::Float, 0.05f },
    { "Variation", 0x0C, T::Float, 0.01f },
    { "Variation rate", 0x10, T::Float, 0.05f },
    { "Micro variation", 0x14, T::Float, 0.1f },
    { "Turbulence", 0x18, T::Float, 0.05f },
    { "Turbulence scale", 0x1C, T::Float, 0.01f },
  } });

  for (Component& component : m_Components)
    component.Values.resize(component.Size, 0);
  // The weather blocks a replay records and gives (see SetReplayWeatherLook)
  for (size_t i = 0; i < m_Components.size(); ++i)
  {
    if (!strcmp(m_Components[i].Name, "Clouds"))
      m_CloudsIndex = i;
    else if (!strcmp(m_Components[i].Name, "Wind"))
      m_WindIndex = i;
  }
  InstallSpriteDofGuard();
}

VisualsManager::~VisualsManager()
{
  PutBackBounceLight();
  PutBackExposure();
  util::hooks::SetReflectionCatchUp(false);
  // A bounce light pass still filed isn't released here (no calls into the
  // game while it may be closing): it finishes and leaves the request stack on
  // its own, only its two map entries stay
}

__int64 VisualsManager::GetStateManager()
{
  __int64 p = ReadPointer(m_ppMain);
  if (!p) return 0;
  p = *(__int64*)(p + 0x50);
  if (!p) return 0;
  p = *(__int64*)(p + 0x90);
  if (!p) return 0;
  p = *(__int64*)(p + 0x1C0);
  if (!p) return 0;
  return *(__int64*)(p + 0x30);
}

bool VisualsManager::IsLevelLoaded()
{
  // The original tools only touched the visual state once the player
  // existed (ClientGameContext + 0x68 -> +0x568 -> +0x14A0)
  __int64 p = ReadPointer(m_ppClientGameContext);
  if (!p) return false;
  p = *(__int64*)(p + 0x68);
  if (!p) return false;
  p = *(__int64*)(p + 0x568);
  if (!p) return false;
  return *(__int64*)(p + 0x14A0) != 0;
}

float* VisualsManager::GetTimeOfDay()
{
  // [[[OFFSET_TIMEOFDAY] + 0x8] + 0x28] + 0x30, the path MEC-TC uses
  __int64 p = ReadPointer(m_ppTimeOfDay);
  if (!p) return nullptr;
  p = *(__int64*)(p + 0x8);
  if (!p) return nullptr;
  p = *(__int64*)(p + 0x28);
  if (!p) return nullptr;
  return (float*)(p + 0x30);
}

void VisualsManager::UpdateTimeOfDay()
{
  if (!m_TodFreeze && !m_TodFix && !m_TrackTodActive && !m_ReplayTodActive && !m_LapseTodActive) return;
  // With MECMP in the game the clock is left as the game has it
  if (util::IsMecmpLoaded()) return;
  if (!IsLevelLoaded()) return;

  float* pTime = GetTimeOfDay();
  if (!pTime) return;

  // (a time-lapse's frame over everything: see LapseLightAt)
  if (m_LapseTodActive)
    *pTime = m_LapseTod;
  else if (m_TrackTodActive)
    *pTime = m_TrackTod;
  else if (m_ReplayTodActive)
    *pTime = m_ReplayTod;
  else if (m_TodFreeze)
    *pTime = m_TodFrozenValue;
  else if (m_TodFix && (int)*pTime == m_TodFixAt)
    *pTime = (float)(m_TodFixAt + m_TodFixSkip);
}

// After a jump of the clock (see ClockJumpSeconds; whoever moved it: the
// menu, a camera track, the game), the time of day's lighting gets the
// game's own reset at the new time (see util::hooks::RequestTodReset; more
// bounce light passes instead where that isn't hooked), the bounce light the
// bigger budget, the exposure adapts at once and reflections are captured a
// whole cubemap a frame until CatchUpMs after the last jump, and while the
// reset's passes run; once its first passes are done every reflection is
// captured again with the new light (see RecaptureReflections). Then the
// game's own values go back. Not with MECMP, the time of day menu (and its
// switch for this) is off then.
void VisualsManager::CatchUpBounceLight()
{
  ULONGLONG now = GetTickCount64();
  float* pTime = (IsLevelLoaded() && !util::IsMecmpLoaded()) ? GetTimeOfDay() : nullptr;
  bool resets = util::hooks::TodResetAvailable();
  // CAMERA > Time-lapse running (see LapseLightAt): only its resets, a frame
  // at a time, with the game's own steps held; its frames' jumps aren't
  // caught up after (the clock goes on from where it is when it ends)
  bool lapse = m_LapseOn && pTime;
  // The level's clock gone or another one (a level loaded): the hook forgets the lighting managers it has seen, so
  // the new level's starts clean (see util::hooks::ForgetTodManagers)
  __int64 clockKey = (__int64)GetTimeOfDay();
  if (!IsLevelLoaded())
    clockKey = 0;
  if (m_TodClockKey && clockKey != m_TodClockKey)
    util::hooks::ForgetTodManagers();
  m_TodClockKey = clockKey;
  // A time-lapse's light follows it as it runs (the hooks' "Following a clock that runs fast")
  util::hooks::SetTodFollow(m_BounceCatchUp && pTime && !lapse);
  util::hooks::SetTodHoldSteps(lapse);
  if (lapse)
  {
    m_LastClock = *pTime;
    m_LastClockTick = now;
    m_JumpWindowTick = now;
    m_JumpWindowClock = *pTime;
    m_JumpWindowSteps = 0.f;
    m_ResetAfterJumps = false;
    m_LastJumpTick = now;
    m_CatchUpUntil = now + CatchUpMs;
  }
  else if (pTime && m_BounceCatchUp && *pTime >= 0.f)
  {
    float clock = *pTime;
    if (m_LastClock >= 0.f)
    {
      float step = fabsf(clock - m_LastClock);
      if (step > DaySeconds / 2)
        step = DaySeconds - step;
      // A new window starts at the last update, so it covers this step's time
      if (now - m_JumpWindowTick >= JumpWindowMs)
      {
        m_JumpWindowTick = m_LastClockTick;
        m_JumpWindowClock = m_LastClock;
        m_JumpWindowSteps = 0.f;
      }
      m_JumpWindowSteps += step;
      if (m_JumpWindowSteps - NaturalClockRate * (now - m_JumpWindowTick) / 1000.f > ClockJumpSeconds)
      {
        if (now >= m_CatchUpUntil)
        {
          char from[16], to[16];
          FormatClock(m_JumpWindowClock, from, sizeof(from));
          FormatClock(clock, to, sizeof(to));
          util::log::Write("Time of day jumped from %s to %s: %s and %.0fx its solve time, the exposure adapts at once and "
            "reflections are captured a whole cubemap a frame until %.0f s after the last jump", from, to,
            resets ? "the lighting gets the game's own reset there" : "the bounce light gets more passes", CatchUpSolveFactor,
            CatchUpMs / 1000.0);
        }
        // How full the game's job queue gets after it, for the log (diagnostics only)
        if (util::DiagnosticsOn())
          util::hooks::NoteTimeJump(m_JumpWindowClock, clock);
        bool quietBefore = now - m_LastJumpTick >= ResetQuietMs;
        m_CatchUpUntil = now + CatchUpMs;
        m_JumpWindowTick = now;
        m_JumpWindowClock = clock;
        m_JumpWindowSteps = 0.f;
        if (resets)
        {
          // At once after a quiet moment; while jumps keep coming (a slider
          // dragged, a fast time-lapse) once more when they stop
          if (quietBefore)
          {
            util::hooks::RequestTodReset(clock);
            m_ResetAfterJumps = false;
          }
          else
            m_ResetAfterJumps = true;
        }
        else
        {
          // The passes count again from the last jump
          if (m_PassesLeft <= 0 && !m_PassFiled)
          {
            m_PassesStart = now;
            m_PassesDone = 0;
          }
          m_PassesLeft = CatchUpPasses;
        }
        m_LastJumpTick = now;
      }
    }
    else
    {
      m_JumpWindowTick = now;
      m_JumpWindowClock = clock;
      m_JumpWindowSteps = 0.f;
    }
    if (m_ResetAfterJumps && now - m_LastJumpTick >= ResetSettleMs)
    {
      util::hooks::RequestTodReset(clock);
      m_ResetAfterJumps = false;
    }
    m_LastClock = clock;
    m_LastClockTick = now;
  }
  else
  {
    m_LastClock = -1.f;
    m_ResetAfterJumps = false;
  }

  // The new light is shown (3 of the reset's first passes done, then all of
  // them): every reflection captured again with it
  // (a time-lapse's: once, when its reset is done, see LapseReflections)
  int passes = 0, of = 0;
  if (util::hooks::TakeTodResetSettled(passes, of) && m_BounceCatchUp && pTime && !lapse &&
      RecaptureReflections(GetStateManager()))
  {
    m_ReflectionsUntil = now + RecaptureMs;
    util::log::Write("Reflections: captured again with the new light (%d of the reset's %d passes done)", passes, of);
  }
  if (lapse)
    LapseReflections(now);

  // On while the passes or the reset go on too (they run on the bigger
  // budget), but not longer than PassesMaxMs after the last jump; all the
  // time while a time-lapse runs
  bool passing = (m_PassesLeft > 0 || m_PassFiled) && now - m_LastJumpTick < PassesMaxMs;
  bool resetting = util::hooks::TodResetBusy() && now - m_LastJumpTick < PassesMaxMs;
  bool want = (m_BounceCatchUp || lapse) && pTime && (now < m_CatchUpUntil || passing || resetting || now < m_ReflectionsUntil);
  RunBounceLightPasses(now, m_BounceCatchUp && pTime && passing);
  CatchUpExposure(want);
  // Reflections: a whole cubemap captured a frame (see util::hooks::SetReflectionCatchUp)
  if (want != m_ReflectionsCaughtUp)
  {
    util::hooks::SetReflectionCatchUp(want);
    m_ReflectionsCaughtUp = want;
    util::log::Write(want ? "Reflections: a whole cubemap captured a frame" : "Reflections: back to the game's faces a frame");
    m_MarksWatching = false;
    m_MarksLogged = 0;
  }
  if (want)
    NoteReflectionMarks(now);
  if (!want && !m_pEnlightenBoosted)
    return;
  __int64 pSettings = FindEnlightenSettings();
  // Another settings object than the one boosted (a level loaded): that one is
  // gone, nothing to put back
  if (m_pEnlightenBoosted && pSettings != m_pEnlightenBoosted)
    m_pEnlightenBoosted = 0;
  if (!want)
  {
    PutBackBounceLight();
    return;
  }
  if (m_pEnlightenBoosted || !pSettings)
    return;

  float solveTime, solveTimeLimited, solveTimeMaximum;
  int counts[4], jobsMaximum;
  if (!ReadEnlightenBudget(pSettings, solveTime, solveTimeLimited, solveTimeMaximum, counts, jobsMaximum))
    return;
  m_EnlightenOriginal = { solveTime, solveTimeLimited, counts[0], counts[1], counts[2], counts[3] };
  // Only the solve time: the job and probe counts stay the game's (see
  // CatchUpSolveFactor)
  float boostedTime = BoostTime(solveTime, solveTimeMaximum);
  float boostedTimeLimited = BoostTime(solveTimeLimited, solveTimeMaximum);
  if (!WriteEnlightenBudget(pSettings, boostedTime, boostedTimeLimited, counts))
    return;
  m_pEnlightenBoosted = pSettings;
  util::log::Write("Bounce light (Enlighten) budget: solve time %.1f -> %.1f ms (limited %.1f -> %.1f, at most %.1f); jobs %d, "
    "light probe sources %d, instances %d (the game's)", solveTime, boostedTime, solveTimeLimited, boostedTimeLimited,
    solveTimeMaximum, counts[0], counts[2], counts[3]);
  (void)jobsMaximum;
}

// During the catch-up (report section 10): when (nearly) every reflection is
// waiting to be captured again and the tools didn't ask, says which of the
// game's two did it. Its sun test stores the sun as it marks them (the sun
// moved); its sky cubemap's capture doesn't (its sky changed). The tools'
// recapture clears the stored sun, so a mark that stores one over a cleared
// one is theirs. A rise of at least 3 to within 2 of all of them counts (one
// may be captured already when it's read).
void VisualsManager::NoteReflectionMarks(ULONGLONG now)
{
  int waiting = 0, count = 0;
  float sun[3] = {};
  if (!CountWaitingReflections(waiting, count) || !ReadStoredSun(GetStateManager(), sun) || count <= 0)
  {
    m_MarksWatching = false;
    return;
  }
  if (m_MarksWatching && waiting >= count - 2 && waiting - m_MarksWaiting >= 3)
  {
    bool sunMoved = memcmp(sun, m_MarksSun, sizeof(sun)) != 0;
    bool ours = sunMoved && m_MarksSun[0] == 0.f && m_MarksSun[1] == 0.f && m_MarksSun[2] == 0.f;
    if (!ours && m_MarksLogged < MarkLogsMax && util::DiagnosticsOn())
    {
      ++m_MarksLogged;
      util::log::Write("Reflections: all %d marked to be captured again by the game's %s, %.1f s after the last jump (%d were "
        "waiting)", count, sunMoved ? "sun test (the sun moved)" : "sky cubemap (its sky changed)", (now - m_LastJumpTick) / 1000.0,
        m_MarksWaiting);
    }
  }
  m_MarksWatching = true;
  m_MarksWaiting = waiting;
  memcpy(m_MarksSun, sun, sizeof(sun));
}

// A time-lapse's reflections (see LapseLightAt): once its frame's reset is
// done, every reflection is captured again with that light (the game's sun
// test marks them all at its next look, see RecaptureReflections); then the
// game captures the nearest LapseReflNearest one at a time (1.5 - 2.2 frames
// each, a round of them 1 - 2 s, report section 10), and the ones further away
// stay waiting until the camera comes near them. So they're taken as captured
// once all were seen marked and the count waiting has stopped going down for
// LapseStableMs and LapseStableUpdates of its updates (or nothing went down in
// LapseNoDropMs: none near enough); at most LapseReflMaxMs.
namespace
{
  const ULONGLONG LapseMarkWaitMs = 1500;
  const ULONGLONG LapseStableMs = 400;
  const int LapseStableUpdates = 20;
  const ULONGLONG LapseNoDropMs = 3000;
  const ULONGLONG LapseReflMaxMs = 15000;
  const int LapseReflLogsMax = 2;
  const int LapseReflWarningsMax = 20;
  const int LapseQueueEvery = 30;   // a frame's job queue line every so many requests (see LapseLightAt)
}

void VisualsManager::LapseReflections(ULONGLONG now)
{
  if (!m_LapseId)
    return;
  float litAt = -1.f;
  double seconds = 0.0;
  // A frame's round that didn't go as it should: a warning each time (at most
  // LapseReflWarningsMax a time-lapse), and the frame's line says how it went
  auto warn = [&](char const* what, int waiting, int count)
  {
    if (m_LapseReflWarnings >= LapseReflWarningsMax)
      return;
    if (++m_LapseReflWarnings == LapseReflWarningsMax)
      util::log::Warning("Time-lapse: reflections %s (%d of %d waiting, %.1f s after the frame's light); no more of these "
        "lines this time (Time-lapse.txt has every frame's)", what, waiting, count, (now - m_LapseReflTick) / 1000.0);
    else
      util::log::Warning("Time-lapse: reflections %s (%d of %d waiting, %.1f s after the frame's light)", what, waiting, count,
        (now - m_LapseReflTick) / 1000.0);
  };
  if (m_LapseReflFor != m_LapseId)
  {
    if (util::hooks::TodResetOutcome(m_LapseId, litAt, seconds) != util::hooks::TodOutcome_Done)
      return;
    m_LapseReflFor = m_LapseId;
    m_LapseReflTick = now;
    m_LapseMarked = false;
    m_LapseWaiting = -1;
    m_LapseStableSince = now;
    m_LapseStableUpdates = 0;
    int waiting = 0, count = 0;
    bool known = ReflectionMarksKnown() && CountWaitingReflections(waiting, count);
    m_LapseReflInfo = { false, waiting, count, 0 };
    if (!known || !RecaptureReflections(GetStateManager()))
    {
      m_LapseRefl = 2;
      m_LapseReflInfo.Ended = 3;
      static bool logged = false;
      if (!logged)
        util::log::Warning("Time-lapse: the reflections' code isn't what the tools know (or none are loaded): each frame waits a "
          "fixed time for them instead of seeing them captured");
      logged = true;
      return;
    }
    m_LapseRefl = 0;
    return;
  }
  if (m_LapseRefl != 0)
    return;
  int waiting = 0, count = 0;
  if (!CountWaitingReflections(waiting, count))
  {
    m_LapseRefl = 2;
    m_LapseReflInfo.Ended = 3;
    return;
  }
  ULONGLONG since = now - m_LapseReflTick;
  m_LapseReflInfo.Waiting = waiting;
  m_LapseReflInfo.Count = count;
  if (count <= 0)
  {
    // (none loaded)
    m_LapseRefl = 1;
    m_LapseReflInfo.Ended = 1;
    return;
  }
  if (!m_LapseMarked && (waiting >= count - 2 || since >= LapseMarkWaitMs))
  {
    m_LapseMarked = true;
    m_LapseReflInfo.Marked = waiting >= count - 2;
    m_LapseWaiting = waiting;
    m_LapseStableSince = now;
    m_LapseStableUpdates = 0;
    // (the count when marked: going down from it is the round)
    m_LapseMarkedAt = waiting;
    if (!m_LapseReflInfo.Marked)
      warn("not all seen marked to be captured again: waiting for the count to settle anyway", waiting, count);
    return;
  }
  if (!m_LapseMarked)
    return;
  if (waiting != m_LapseWaiting)
  {
    m_LapseWaiting = waiting;
    m_LapseStableSince = now;
    m_LapseStableUpdates = 0;
  }
  else
    ++m_LapseStableUpdates;
  bool dropped = waiting < m_LapseMarkedAt;
  bool stable = now - m_LapseStableSince >= LapseStableMs && m_LapseStableUpdates >= LapseStableUpdates;
  if (stable && (dropped || since >= LapseNoDropMs))
  {
    m_LapseRefl = 1;
    m_LapseReflInfo.Ended = dropped ? 0 : 1;
    if (!dropped)
      warn("none captured in 3 s (none near enough, or they weren't marked)", waiting, count);
    else if (m_LapseReflLogs < LapseReflLogsMax)
    {
      ++m_LapseReflLogs;
      util::log::Write("Time-lapse: reflections captured again with the frame's light in %.1f s (%d of %d still waiting: the ones "
        "further away, captured when the camera comes near them)", since / 1000.0, waiting, count);
    }
  }
  else if (since >= LapseReflMaxMs)
  {
    m_LapseRefl = 1;
    m_LapseReflInfo.Ended = 2;
    warn("still being captured after 15 s: taken as they are", waiting, count);
  }
}

bool VisualsManager::LapseAvailable(char* why, size_t size)
{
  std::lock_guard<std::mutex> lock(m_Mutex);
  char const* reason = nullptr;
  if (util::IsMecmpLoaded())
    reason = "MECMP (multiplayer) is open: it keeps the time of day in sync";
  else if (!util::hooks::TodResetAvailable())
    reason = "the game's time of day lighting isn't the code the tools know (see CT.log)";
  else if (!IsLevelLoaded() || !GetTimeOfDay())
    reason = "load a level first";
  if (reason && why && size)
    strcpy_s(why, size, reason);
  return reason == nullptr;
}

__int64 VisualsManager::ClockKey()
{
  std::lock_guard<std::mutex> lock(m_Mutex);
  float* pTime = IsLevelLoaded() ? GetTimeOfDay() : nullptr;
  return (__int64)pTime;
}

long VisualsManager::LapseLightAt(float seconds, bool quiet)
{
  std::lock_guard<std::mutex> lock(m_Mutex);
  if (util::IsMecmpLoaded() || !util::hooks::TodResetAvailable() || !(seconds >= 0.f) || !(seconds < DaySeconds))
    return 0;
  float* pTime = IsLevelLoaded() ? GetTimeOfDay() : nullptr;
  if (!pTime)
    return 0;
  // How full the game's job queue gets after a frame's reset (diagnostics only, as after jumps): its window closes
  // 10 s after the last jump, so each frame noted is a line of its own (2026-11-16: one every frame, ~25 s apart with
  // the whole city). Noted for the run's first frames (not quiet) and every LapseQueueEvery after.
  float before = *pTime;
  if (!m_LapseOn)
  {
    m_LapseReflLogs = 0;
    m_LapseReflWarnings = 0;
    m_LapseQueueCount = 0;
  }
  ++m_LapseQueueCount;
  bool noteQueue = !quiet || m_LapseQueueCount % LapseQueueEvery == 0;
  m_LapseOn = true;
  m_LapseTodActive = true;
  m_LapseTod = seconds;
  *pTime = seconds;
  m_ResetAfterJumps = false;
  // (at once, not at the next visual update: the lighting may update before it)
  util::hooks::SetTodFollow(false);
  util::hooks::SetTodHoldSteps(true);
  m_LapseId = util::hooks::RequestTodReset(seconds, true, quiet);
  if (noteQueue)
    util::hooks::NoteTimeJump(before, seconds);
  m_LapseReflFor = 0;
  m_LapseRefl = 0;
  return m_LapseId;
}

int VisualsManager::LapseLightState(long id, float& litAt, float& clockAt, double& seconds)
{
  return util::hooks::TodResetOutcome(id, litAt, seconds, &clockAt);
}

int VisualsManager::LapseReflectionsState(long id, LapseReflInfo* info)
{
  std::lock_guard<std::mutex> lock(m_Mutex);
  if (!id || m_LapseReflFor != id)
    return 0;
  if (info)
    *info = m_LapseReflInfo;
  return m_LapseRefl;
}

void VisualsManager::EndLapse()
{
  std::lock_guard<std::mutex> lock(m_Mutex);
  if (!m_LapseOn && !m_LapseTodActive)
    return;
  if (m_LapseTodActive)
  {
    // The clock goes on from the last frame's time, or stays there if stopped;
    // a track let go after it keeps that too (see StopTrackTimeOfDay)
    m_TodFrozenValue = m_LapseTod;
    if (m_TrackTodActive)
      m_TrackTod = m_LapseTod;
  }
  m_LapseOn = false;
  m_LapseTodActive = false;
  m_LapseId = 0;
  m_LapseReflFor = 0;
  m_LapseRefl = 0;
  util::hooks::SetTodHoldSteps(false);
}

// The game's own budget back on the settings object that got the bigger one
// (if it's still the game's)
void VisualsManager::PutBackBounceLight()
{
  if (!m_pEnlightenBoosted)
    return;
  if (FindEnlightenSettings() == m_pEnlightenBoosted)
  {
    EnlightenBudget const& o = m_EnlightenOriginal;
    int counts[4] = { o.Jobs, o.JobsLimited, o.ProbeSources, o.ProbeInstances };
    if (WriteEnlightenBudget(m_pEnlightenBoosted, o.SolveTime, o.SolveTimeLimited, counts))
      util::log::Write("Bounce light (Enlighten) budget back to the game's");
  }
  m_pEnlightenBoosted = 0;
}

// The extra bounce light passes (see CatchUpPasses). A pass filed is waited
// for and released once done, also after the catch-up ends; a new one is filed
// only when mayFile, the manager is ready and the last one is released.
void VisualsManager::RunBounceLightPasses(ULONGLONG now, bool mayFile)
{
  if (!m_PassFiled && m_PassesLeft <= 0)
    return;
  if (!PassCodeKnown())
  {
    static bool logged = false;
    if (!logged)
      util::log::Warning("Enlighten's request code isn't what the tools know, the bounce light gets no extra passes");
    logged = true;
    m_PassesLeft = 0;
    m_PassFiled = false;
    return;
  }
  bool modeOne = false, paused = false;
  __int64 manager = PassManager(modeOne, paused);
  // Mode 0 solves all the time on its own (the bigger budget is enough there)
  if (manager && !modeOne && !m_PassFiled)
  {
    m_PassesLeft = 0;
    m_PassesDone = 0;
    return;
  }
  bool ready = manager && modeOne && !paused;
  if (m_PassFiled)
  {
    // Another manager (a level change): the request went with the old one
    if (manager != m_PassManager)
      m_PassFiled = false;
    else
    {
      unsigned int state = 0;
      if (!PassState(manager, m_PassId, state))
        m_PassFiled = false;
      else if (state != PassDone)
        return;
      else
      {
        ReleasePass(manager, m_PassId);
        m_PassFiled = false;
        ++m_PassesDone;
      }
    }
  }
  if (m_PassesLeft > 0 && mayFile && manager && ready)
  {
    // The game's own requests first (see PendingRequests): ours isn't filed
    // here, so anything pending is the game's
    if (PendingRequests(manager) != 0)
      return;
    unsigned int id = 0;
    if (FilePass(manager, id))
    {
      if (!m_PassesDone && m_PassesLeft == CatchUpPasses)
        util::log::Write("Bounce light (Enlighten): %d more passes", CatchUpPasses);
      m_PassId = id;
      m_PassManager = manager;
      m_PassFiled = true;
      --m_PassesLeft;
    }
    return;
  }
  if (m_PassesLeft > 0 && mayFile)
    return;   // the manager isn't ready (paused, or none): again next update
  // Done, or no more may be filed
  if (m_PassesLeft > 0)
    util::log::Write("Bounce light (Enlighten): %d passes done in %.1f s, %d not filed (the catch-up ended first)", m_PassesDone,
      (now - m_PassesStart) / 1000.0, m_PassesLeft);
  else if (m_PassesDone)
    util::log::Write("Bounce light (Enlighten): %d passes done in %.1f s", m_PassesDone, (now - m_PassesStart) / 1000.0);
  m_PassesLeft = 0;
  m_PassesDone = 0;
}

// While want, the tonemap's adaptation times are InstantAdaptation after every
// visual update, so the exposure takes its target at once (otherwise it
// adjusts over seconds, like eyes). The game recomputes the state
// every update, so whatever isn't ours is its own and is kept for putting
// back. A Tonemap override (Visual Environment) is written after this, its
// own times win.
void VisualsManager::CatchUpExposure(bool want)
{
  if (!want)
  {
    PutBackExposure();
    return;
  }
  __int64 pManager = IsLevelLoaded() ? GetStateManager() : 0;
  if (!pManager)
  {
    m_ExposureCaughtUp = false;
    return;
  }
  __int64 pState = pManager + StateOffset;
  float times[2];
  if (!ReadAdaptationTimes(pState, times))
    return;
  for (int i = 0; i < 2; ++i)
    if (!m_ExposureCaughtUp || times[i] != InstantAdaptation)
      m_GameAdaptation[i] = times[i];

  float instant[2] = { InstantAdaptation, InstantAdaptation };
  if (!WriteAdaptationTimes(pState, instant, false))
    return;
  if (!m_ExposureCaughtUp)
    util::log::Write("Exposure: adapts at once (the game's dark / light adaptation times %.2f / %.2f s)",
      m_GameAdaptation[0], m_GameAdaptation[1]);
  m_ExposureCaughtUp = true;
}

// The game's adaptation times back where ours still are
void VisualsManager::PutBackExposure()
{
  if (!m_ExposureCaughtUp)
    return;
  m_ExposureCaughtUp = false;
  __int64 pManager = IsLevelLoaded() ? GetStateManager() : 0;
  if (pManager && WriteAdaptationTimes(pManager + StateOffset, m_GameAdaptation, true))
    util::log::Write("Exposure: the game's adaptation times back");
}

float VisualsManager::GetCurrentTimeOfDay()
{
  std::lock_guard<std::mutex> lock(m_Mutex);
  float* pTime = IsLevelLoaded() ? GetTimeOfDay() : nullptr;
  return pTime ? *pTime : -1.f;
}

void VisualsManager::SetTimeOfDay(float seconds)
{
  std::lock_guard<std::mutex> lock(m_Mutex);
  if (util::IsMecmpLoaded()) return;
  float* pTime = IsLevelLoaded() ? GetTimeOfDay() : nullptr;
  // (not while a time-lapse holds it: see LapseLightAt)
  if (!pTime || seconds < 0 || m_LapseTodActive) return;

  *pTime = seconds;
  m_TodFrozenValue = seconds;
}

void VisualsManager::SetTrackTimeOfDay(float seconds)
{
  std::lock_guard<std::mutex> lock(m_Mutex);
  if (util::IsMecmpLoaded()) return;
  m_TrackTod = seconds;
  m_TrackTodActive = true;

  // Also right away, the visual environment update may come before or after
  // (not under a time-lapse's frame: its time wins, see LapseLightAt)
  float* pTime = IsLevelLoaded() ? GetTimeOfDay() : nullptr;
  if (pTime && !m_LapseTodActive)
    *pTime = seconds;
}

void VisualsManager::KeepClock()
{
  std::lock_guard<std::mutex> lock(m_Mutex);
  UpdateTimeOfDay();
}

void VisualsManager::StopTrackTimeOfDay()
{
  std::lock_guard<std::mutex> lock(m_Mutex);
  if (!m_TrackTodActive) return;

  // The clock goes on from where the track left it, or stays there when stopped
  m_TrackTodActive = false;
  m_TodFrozenValue = m_TrackTod;
}

// A replay's time of day (2026-10-25: a mission's time wasn't in its replay;
// missions set the clock through TimeOfDayClockControllerEntity, see
// DrawClockHold). The time the clock had when the replay first set it is
// given back when it stops, unless a track is setting it then (the track's
// time stays) or the clock is stopped (the stopped time is written anyway).
void VisualsManager::SetReplayTimeOfDay(float seconds)
{
  std::lock_guard<std::mutex> lock(m_Mutex);
  if (util::IsMecmpLoaded() || seconds < 0.f) return;
  float* pTime = IsLevelLoaded() ? GetTimeOfDay() : nullptr;
  if (!pTime) return;
  if (!m_ReplayTodActive)
    m_ReplayTodLive = *pTime;
  m_ReplayTod = seconds;
  m_ReplayTodActive = true;
  if (!m_TrackTodActive && !m_LapseTodActive)
    *pTime = seconds;
}

// How the level holds the clock (2026-10-25, offline: the game image and its
// data). The tools' clock is the ServerTimeOfDayClockEntity (vtable
// 0x141CD8438: its getType 0x143BC4E00 gives TypeInfo 0x142896A70); its
// constructor 0x143BC3AA0 sets + 0x30 the time (data DefaultTime), + 0xE8 the
// rate (86400 / FullDayDuration), + 0xEC a time scale (1) and + 0xF8 stopped
// (data PausedOnLoad), and its update (slot 23, 0x143BC78E0) tests + 0xF8.
// Missions and story beats set it through TimeOfDayClockControllerEntity
// (gameplay/logic/pf_settimeofday and others; ServerTimeOfDayClockControllerEntity,
// vtable 0x141CD8960): its events (0x143BC4AA0) write TimeToSet to + 0x30 once
// (event 0xA4259012) and stop (0x0DD8DDF7) or start (0xC44B5C3C) the clock
// through + 0xF8; its TimeScaleMultiplier goes to + 0xEC (0x143BC6520). So
// the tools' time still sets the clock in a mission (the same + 0x30); a
// mission can only stop it running or change its pace, shown under the slider.
static bool ReadClockHold(float const* pTime, bool& stopped, float& scale)
{
  const __int64 ServerClockVTableRva = 0x1CD8438;
  const int ClockTimeOffset = 0x30, ClockScaleOffset = 0xEC, ClockStoppedOffset = 0xF8;
  __try
  {
    __int64 clock = (__int64)pTime - ClockTimeOffset;
    if (*(__int64 const*)clock != (__int64)g_gameHandle + ServerClockVTableRva)
      return false;
    BYTE flag = *(BYTE const*)(clock + ClockStoppedOffset);
    scale = *(float const*)(clock + ClockScaleOffset);
    stopped = flag != 0;
    return flag <= 1 && scale >= 0.f && scale < 1000.f;
  }
  __except (EXCEPTION_EXECUTE_HANDLER)
  {
    return false;
  }
}

void VisualsManager::StopReplayTimeOfDay()
{
  std::lock_guard<std::mutex> lock(m_Mutex);
  if (!m_ReplayTodActive) return;
  m_ReplayTodActive = false;
  float* pTime = (IsLevelLoaded() && !util::IsMecmpLoaded()) ? GetTimeOfDay() : nullptr;
  if (pTime && !m_TrackTodActive && !m_LapseTodActive && m_ReplayTodLive >= 0.f)
    *pTime = m_ReplayTodLive;
  m_ReplayTodLive = -1.f;
}

void VisualsManager::DrawTimeOfDayUI()
{
  std::lock_guard<std::mutex> lock(m_Mutex);

  if (util::IsMecmpLoaded())
  {
    ImGui::PushTextWrapPos(0.f);
    ImGui::TextDisabled("Time of day: MECMP (multiplayer) is open, so these controls are disabled "
      "because MECMP syncs the time of day.");
    ImGui::PopTextWrapPos();
    return;
  }
  float* pTime = IsLevelLoaded() ? GetTimeOfDay() : nullptr;
  if (!pTime)
  {
    ImGui::TextDisabled("Time of day: load a level to change it");
    return;
  }

  float time = (m_TodFreeze && !m_TrackTodActive) ? m_TodFrozenValue : *pTime;
  int seconds = (int)(m_LapseTodActive ? m_LapseTod : time);
  char label[32];
  sprintf_s(label, "%02d:%02d", (seconds / 3600) % 24, (seconds / 60) % 60);

  // A time-lapse holds it a frame at a time (CAMERA > Time-lapse): shown, not changed here
  if (m_LapseTodActive)
  {
    ImGui::Text("Time of day  %s", label);
    ImGui::TextDisabled("CAMERA > Time-lapse is setting it, a frame at a time");
    return;
  }

  if (ImGui::SliderFloat("Time of day", &time, 0.f, 86399.f, label))
  {
    *pTime = time;
    m_TodFrozenValue = time;
  }

  // Steps of 10 minutes, round the clock past midnight
  float step = 0.f;
  if (ImGui::Button("- 10 min"))
    step = -600.f;
  ImGui::SameLine();
  if (ImGui::Button("+ 10 min"))
    step = 600.f;
  if (step != 0.f)
  {
    time = fmodf(time + step + 86400.f, 86400.f);
    *pTime = time;
    m_TodFrozenValue = time;
  }

  if (m_TrackTodActive)
    ImGui::TextDisabled("The camera track is setting the time of day");
  else if (m_ReplayTodActive)
    ImGui::TextDisabled("The replay is setting the time of day (as recorded)");
  // A mission's (or the level's) hold on the clock (see ReadClockHold)
  bool stopped = false;
  float scale = 1.f;
  if (ReadClockHold(pTime, stopped, scale) && (stopped || fabsf(scale - 1.f) > 0.001f))
  {
    if (stopped)
      ImGui::TextDisabled("The game has stopped its clock (a mission or the level): the time stays unless set here");
    else
      ImGui::TextDisabled("The game runs its clock at %.2fx (a mission's setting)", scale);
  }

  if (ImGui::Checkbox("Stop the clock", &m_TodFreeze) && m_TodFreeze)
    m_TodFrozenValue = *pTime;

  ImGui::Checkbox("Light catches up with time jumps", &m_BounceCatchUp);
  ImGui::HelpMarker("After the time jumps (this slider, the buttons, a track cut, a fast time-lapse), the light in the "
    "shade, the exposure and the reflections are redone at once instead of over several seconds. It takes a few "
    "seconds, and the game may run slower meanwhile.\n\nWhile the clock runs fast (a time-lapse, a track whose "
    "keyframes' times are far apart), the light in the shade follows it as it runs, a step at a time each about as "
    "far ahead as the clock goes in one, instead of keeping one time's light for 8 seconds; once it stops, it's "
    "redone once more at the time it stopped at.");
  ImGui::TextDisabled("Time-lapses with the light worked out for every frame: CAMERA > Time-lapse");

  // The stuck clock fix (m_TodFix: the clock stuck at m_TodFixAt is moved
  // ahead by m_TodFixSkip) is always on: not in the menu since 2026-11-01
}

void VisualsManager::OnVisualEnvironmentUpdate(bool beforeGameUpdate)
{
  std::lock_guard<std::mutex> lock(m_Mutex);

  // Before and after the game's update, so the clock the game moves on during
  // its update is put back straight away
  UpdateTimeOfDay();
  if (!beforeGameUpdate)
  {
    CatchUpBounceLight();
    // The depth of field's test pictures: asked for in its menu, or by the
    // file "Cinematic Tools\DoF test.txt" (looked for once a second, then
    // deleted; only with the diagnostics on), so they can be taken without the menu
    ULONGLONG now = GetTickCount64();
    if (util::DiagnosticsOn() && !m_DofTest.Step && now - m_DofTestFileTick >= 1000)
    {
      m_DofTestFileTick = now;
      if (GetFileAttributesA("Cinematic Tools\\DoF test.txt") != INVALID_FILE_ATTRIBUTES)
      {
        DeleteFileA("Cinematic Tools\\DoF test.txt");
        StartDofTest();
      }
    }
    RunDofTest();
  }
  // Auto-focus moves the depth of field's distances. Before the game's
  // update that's done after the self-check below, which compares what was
  // written last: it would take the moved distances for the game's change.
  bool autoFocusAfterCheck = beforeGameUpdate && m_Components[1].Override && m_Components[1].HasValues;
  if (!autoFocusAfterCheck)
    ApplyAutoFocus(m_Components[1]);

  Component const& dof = m_Components[1];
  bool anyOverride = m_DofMethodApplied || m_ReplayLookActive;
  for (Component const& component : m_Components)
    anyOverride |= component.Override && component.HasValues;
  if (!anyOverride || !IsLevelLoaded())
  {
    ApplyLensDistortion(0);
    ApplyWeatherLook(beforeGameUpdate);
    return;
  }

  __int64 pManager = GetStateManager();
  if (!pManager)
  {
    ApplyLensDistortion(0);
    return;
  }
  __int64 pState = pManager + StateOffset;

  // A playing replay's weather first: a change made in the menu goes on top
  // (see WriteReplayLook)
  if (m_ReplayLookActive)
    WriteReplayLook(pState);

  for (Component& component : m_Components)
  {
    if (!component.Override || !component.HasValues) continue;

    // Self-check: what we wrote after the last update should still be there.
    // If not, something other than the visual environment update changes it.
    if (beforeGameUpdate)
      CheckStillApplied(pState, component);
    if (autoFocusAfterCheck && &component == &m_Components[1])
      ApplyAutoFocus(component);

    WriteToGame(pState, component);
    if ((size_t)(&component - m_Components.data()) == m_OutdoorIndex)
      RecomputeSun(pState);
  }
  ApplyLensDistortion(pState);

  // DoF method, as the original tools did (with SpriteDofEnable, which the
  // game needs on to keep the Sprite method, see SpriteDofOffset). It isn't
  // part of the state the game recomputes, so the game's own values are put
  // back afterwards.
  __int64 pRenderSettings = *(__int64*)(pManager + RenderSettingsOffset);
  if (pRenderSettings)
  {
    if (dof.Override && m_DofMethodHasValue)
    {
      int method = m_DofMethod == 1 ? 1 : 0;
      *(BYTE*)(pRenderSettings + SpriteDofOffset) = (BYTE)method;
      *(int*)(pRenderSettings + DofMethodOffset) = method;
      m_DofMethodApplied = true;
    }
    else if (m_DofMethodApplied)
    {
      *(BYTE*)(pRenderSettings + SpriteDofOffset) = m_SpriteDofEnableOriginal;
      *(int*)(pRenderSettings + DofMethodOffset) = m_DofMethodOriginal;
      m_DofMethodApplied = false;
    }
  }
  // The guard caught the sprite pass's first frame (see InstallSpriteDofGuard)
  static unsigned int guardSeen = 0;
  if (m_pSpriteDofGuard && !beforeGameUpdate)
  {
    unsigned int count = *(volatile unsigned int*)(m_pSpriteDofGuard + GuardCount);
    if (count != guardSeen)
    {
      util::log::Write("Sprite depth of field: its buffers weren't made yet, the guard had the game make them (%u times so far)",
        count);
      guardSeen = count;
    }
  }
  ApplyWeatherLook(beforeGameUpdate);
}

// The sun after Outdoor light is changed (2026-11-04, offline). The tools write
// the renderer's view state, which the world renderer copies from the visual
// environment manager's own every frame (0x14372E27D: 0x1436ED460, the copy
// the update hook is on). The manager works the sun's derived values out in
// its own copy, from the game's values, before that (its mix 0x143703D60,
// after the blends): the direction from the rotation (0x1436E9990: + 0x1B90,
// + 0x1BA0) and the light the renderer lights with (0x1437050DC: + 0x10 /
// + 0x20 final luminance / illuminance, + 0x30 - + 0x50 the outer space ones;
// with the physical sky, Sky Enable + 0x600 and SkyType + 0x484 = 2, through
// 0x143729430, otherwise color x intensity over the sun's solid angle). The
// renderer lights with the final values (0x143735FD5), so the menu's sun
// color, intensity and rotation never reached them: it lit with the final
// values the menu had read. They're worked out again here, by the game's own
// code where it has it, on the view state after the change is written.
namespace
{
  const __int64 SunDirectionRva = 0x36E9990;
  const __int64 SunPhysicalRva = 0x3729430;
  const __int64 SunMixCheckRva = 0x37050DC;
  const BYTE SunDirectionBytes[] = { 0x53, 0x48, 0x81, 0xEC, 0x80, 0x00, 0x00, 0x00, 0x0F, 0x29, 0x74, 0x24, 0x70 };
  const BYTE SunPhysicalBytes[] = { 0x48, 0x89, 0xE0, 0x55, 0x53, 0x57, 0x48, 0x8D, 0x68, 0xA1, 0x48, 0x81, 0xEC, 0x90, 0x00, 0x00,
    0x00 };
  // cmp byte [r14 + 0x600], 0; je; cmp dword [r14 + 0x484], 2; jne: the mix's own choice
  const BYTE SunMixCheckBytes[] = { 0x41, 0x80, 0xBE, 0x00, 0x06, 0x00, 0x00, 0x00, 0x74, 0x20, 0x41, 0x83, 0xBE, 0x84, 0x04, 0x00,
    0x00, 0x02, 0x75, 0x16 };
  const int SkyEnableOffset = 0x600, SkyTypeOffset = 0x484, SkyLight1FromOutdoorOffset = 0x606;
  const int SunColorOffset = 0x00, SunIntensityOffset = 0x94, SunAngularRadiusOffset = 0x98;
  typedef void(__fastcall* SunDirection_t)(__int64 state);
  typedef void(__fastcall* SunPhysical_t)(float const* origin, __int64 state);

  bool SunCodeKnown()
  {
    static int known = -1;
    if (known >= 0)
      return known == 1;
    known = 0;
    BYTE const* base = (BYTE const*)g_gameHandle;
    __try
    {
      IMAGE_NT_HEADERS const* pNt = (IMAGE_NT_HEADERS const*)(base + ((IMAGE_DOS_HEADER const*)base)->e_lfanew);
      if (pNt->FileHeader.TimeDateStamp == KnownTimeStamp && !memcmp(base + SunDirectionRva, SunDirectionBytes, sizeof(SunDirectionBytes)) &&
          !memcmp(base + SunPhysicalRva, SunPhysicalBytes, sizeof(SunPhysicalBytes)) &&
          !memcmp(base + SunMixCheckRva, SunMixCheckBytes, sizeof(SunMixCheckBytes)))
        known = 1;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
    return known == 1;
  }

  bool CallSunDirection(__int64 state)
  {
    __try
    {
      ((SunDirection_t)((__int64)g_gameHandle + SunDirectionRva))(state);
      return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      return false;
    }
  }

  // As the mix calls it: the viewer at the origin (a zero vector, 16-aligned)
  bool CallSunPhysical(__int64 state)
  {
    __try
    {
      alignas(16) float origin[4] = { 0.f, 0.f, 0.f, 0.f };
      ((SunPhysical_t)((__int64)g_gameHandle + SunPhysicalRva))(origin, state);
      return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      return false;
    }
  }

  // The sun's source as the state has it: the physical sky, and whether its
  // sun (Light1) takes Outdoor light's color and intensity
  bool ReadSunSource(__int64 state, bool& physical, bool& fromOutdoor)
  {
    __try
    {
      BYTE const* s = (BYTE const*)state;
      physical = s[SkyEnableOffset] != 0 && *(int const*)(s + SkyTypeOffset) == 2;
      fromOutdoor = s[SkyLight1FromOutdoorOffset] != 0;
      return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      return false;
    }
  }

  // Outdoor light's sky and ground color as the renderer takes them
  // (2026-11-05, offline + live). 0x143736038 copies them (state + 0x60 /
  // + 0x70) into its constants (view + 0x845C0 / + 0x845D0), then zeroes both
  // when 0x143791B80 says so: on the bounce light's (Enlighten's) manager
  // (EnlightenManagerRva), [manager + 0x105F8] + 0xB0 set and manager + 0xBC
  // not 2. Their only other reader (0x143734D33, the sun, sky and ground for
  // 0x14358FAA0) runs only with the view's + 0x88276 set (r15 is 0 there). In
  // the city both hold: live 1 and 0, the constants 0 with the state's sky
  // color 0.32 0.34 0.40, + 0x88276 0; Sky color x4 in the test pictures
  // (16:50) brightened nothing.
  const __int64 SkyZeroCheckRva = 0x3791B80, SkyZeroCallRva = 0x3736063, SkySecondReaderRva = 0x3734D33;
  const BYTE SkyZeroCheckBytes[] = { 0x48, 0x8B, 0x81, 0xF8, 0x05, 0x01, 0x00, 0x80, 0xB8, 0xB0, 0x00, 0x00, 0x00, 0x00, 0x74, 0x0C,
    0x83, 0xB9, 0xBC, 0x00, 0x00, 0x00, 0x02, 0x74, 0x03, 0xB0, 0x01, 0xC3, 0x30, 0xC0, 0xC3 };
  // mov rcx, [EnlightenManagerRva]; call 0x143791B80; xorps xmm10; test al; je; both constants = xmm10
  const BYTE SkyZeroCallBytes[] = { 0x48, 0x8B, 0x0D, 0xD6, 0x4E, 0xE3, 0xFE, 0xE8, 0x11, 0xBB, 0x05, 0x00, 0x45, 0x0F, 0x57, 0xD2,
    0x84, 0xC0, 0x74, 0x10, 0x45, 0x0F, 0x29, 0x96, 0xC0, 0x45, 0x08, 0x00, 0x45, 0x0F, 0x29, 0x96, 0xD0, 0x45, 0x08, 0x00 };
  // cmp byte [rsi + 0x88276], r15b; je
  const BYTE SkySecondReaderBytes[] = { 0x44, 0x38, 0xBE, 0x76, 0x82, 0x08, 0x00, 0x0F, 0x84, 0x92, 0x01, 0x00, 0x00 };
  const int ViewStateOffset = 0x84970;   // the view's state: the view is at manager + StateOffset - this
  const int ViewSecondReaderOffset = 0x88276;

  // 1: the renderer uses neither sky nor ground color now, 0: it does, -1:
  // can't tell (other code, nothing loaded)
  int SkyColorUnused(__int64 pManager)
  {
    static int known = -1;
    BYTE const* base = (BYTE const*)g_gameHandle;
    __try
    {
      if (known < 0)
      {
        IMAGE_NT_HEADERS const* pNt = (IMAGE_NT_HEADERS const*)(base + ((IMAGE_DOS_HEADER const*)base)->e_lfanew);
        known = pNt->FileHeader.TimeDateStamp == KnownTimeStamp &&
          !memcmp(base + SkyZeroCheckRva, SkyZeroCheckBytes, sizeof(SkyZeroCheckBytes)) &&
          !memcmp(base + SkyZeroCallRva, SkyZeroCallBytes, sizeof(SkyZeroCallBytes)) &&
          !memcmp(base + SkySecondReaderRva, SkySecondReaderBytes, sizeof(SkySecondReaderBytes)) ? 1 : 0;
      }
      if (known != 1 || !pManager)
        return -1;
      __int64 manager = *(__int64 const*)(base + EnlightenManagerRva);
      if (!manager)
        return -1;
      __int64 settings = *(__int64 const*)(manager + 0x105F8);
      if (!settings)
        return -1;
      bool zeroed = *(BYTE const*)(settings + 0xB0) != 0 && *(int const*)(manager + 0xBC) != 2;
      bool secondReader = *(BYTE const*)(pManager + StateOffset - ViewStateOffset + ViewSecondReaderOffset) != 0;
      return zeroed && !secondReader ? 1 : 0;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      return -1;
    }
  }
}

void VisualsManager::RecomputeSun(__int64 pState)
{
  if (!m_SunRecompute || !pState)
    return;
  if (!SunCodeKnown())
  {
    static bool logged = false;
    if (!logged)
      util::log::Warning("Outdoor light: the game's sun code isn't what the tools know, a changed sun's light and direction "
        "aren't worked out again");
    logged = true;
    return;
  }
  BYTE* s = (BYTE*)pState;
  bool physical = false, fromOutdoor = false;
  bool ok = ReadSunSource(pState, physical, fromOutdoor) && CallSunDirection(pState);
  if (ok && physical)
  {
    // The sky's own sun lights the world (the city: Light1TakesColorFromOutdoorLight
    // 0, Light1Intensity 200, live 2026-11-05; the test pictures: Sun intensity
    // x4 and a red sun changed nothing): the menu's sun color and intensity
    // scale it instead (see ScaleSkySun)
    if (!fromOutdoor)
      ScaleSkySun(s);
    ok = CallSunPhysical(pState);
  }
  else if (ok)
  {
    // The mix's own (0x143705106): the sun's solid angle from its angular
    // radius in degrees, the color x intensity over it, nothing below 0
    float const* color = (float const*)(s + SunColorOffset);
    float intensity = *(float const*)(s + SunIntensityOffset);
    float solid = (1.f - cosf(*(float const*)(s + SunAngularRadiusOffset) * 3.14159274f * 0.00555555569f)) * 6.28318548f;
    float luminance[4], illuminance[4];
    for (int i = 0; i < 4; ++i)
    {
      illuminance[i] = (std::max)(0.f, color[i] * intensity);
      luminance[i] = (std::max)(0.f, color[i] * intensity / solid);
    }
    memcpy(s + 0x10, luminance, 16);
    memcpy(s + 0x20, illuminance, 16);
    memcpy(s + 0x30, luminance, 16);
    memcpy(s + 0x40, illuminance, 16);
    memset(s + 0x50, 0, 16);
  }
  if (!ok)
  {
    m_SunRecompute = false;
    util::log::Warning("Outdoor light: working the sun's light out again failed, turned off");
    return;
  }
  if (!m_SunRecomputeLogged)
  {
    m_SunRecomputeLogged = true;
    util::log::Write("Outdoor light: the sun's direction and light worked out again from the change (%s; final illuminance "
      "%.1f %.1f %.1f)", physical ? (fromOutdoor ? "the physical sky, its sun from Outdoor light" :
      "the physical sky, its sun from its own Light 1, not Outdoor light's color and intensity") : "no physical sky",
      ((float*)(s + 0x20))[0], ((float*)(s + 0x20))[1], ((float*)(s + 0x20))[2]);
  }
}

// With the physical sky's sun not taking Outdoor light's (see RecomputeSun):
// the sky's Light1Color (state + 0x3D0) and Light1Intensity (+ 0x5B0) times
// the menu's sun color and intensity over what they were when the effect was
// copied from the game (m_SunRef*), so unchanged they change nothing. The
// game's own value under it is kept: what's there is taken as new unless it's
// what was written last (the view state is copied from the game's every frame,
// but this runs at each of the update hook's calls).
void VisualsManager::ScaleSkySun(BYTE* s)
{
  const int SkyLight1ColorOffset = 0x3D0, SkyLight1IntensityOffset = 0x5B0;
  Component const& outdoor = m_Components[m_OutdoorIndex];
  float const* userColor = (float const*)(outdoor.Values.data() + SunColorOffset);
  float userIntensity = *(float const*)(outdoor.Values.data() + SunIntensityOffset);
  float ratio[4];
  for (int i = 0; i < 3; ++i)
    ratio[i] = m_SunRefColor[i] > 1e-6f ? userColor[i] / m_SunRefColor[i] : userColor[i];
  ratio[3] = m_SunRefIntensity > 1e-6f ? userIntensity / m_SunRefIntensity : userIntensity;
  float* color = (float*)(s + SkyLight1ColorOffset);
  float* intensity = (float*)(s + SkyLight1IntensityOffset);
  float now[4] = { color[0], color[1], color[2], *intensity };
  bool fresh = !m_SkySunApplied;
  for (int i = 0; i < 4; ++i)
    fresh |= now[i] != m_SkySunWritten[i];
  if (fresh)
    memcpy(m_SkySunBase, now, sizeof(now));
  float v[4];
  for (int i = 0; i < 4; ++i)
  {
    v[i] = m_SkySunBase[i] * ratio[i];
    v[i] = v[i] >= 0.f ? v[i] : 0.f;   // NaN too
  }
  color[0] = v[0];
  color[1] = v[1];
  color[2] = v[2];
  *intensity = v[3];
  memcpy(m_SkySunWritten, v, sizeof(v));
  m_SkySunApplied = true;
}

// VISUALS > Filmic effects' lens distortion (see lensdistortion): with the
// change on and both Enable and Lens distortion ticked, the bend is drawn by
// the tools at Present, from the gains set here, and the game's own is
// written off (after the component, so it wins at every update the
// component's values do); otherwise the tools draw none and the game keeps its
// own. pState 0: no level, none drawn.
void VisualsManager::ApplyLensDistortion(__int64 pState)
{
  bool want = false;
  float gain = 0.f, cubic = 0.f;
  Component const* pFilmic = m_FilmicIndex < m_Components.size() ? &m_Components[m_FilmicIndex] : nullptr;
  if (pState && pFilmic && pFilmic->Override && pFilmic->HasValues)
  {
    BYTE const* v = pFilmic->Values.data();
    want = v[FilmicEnableOffset] != 0 && v[FilmicLensOffset] != 0;
    memcpy(&gain, v + FilmicLensGainOffset, 4);
    memcpy(&cubic, v + FilmicLensCubicOffset, 4);
  }
  if (want)
    *(BYTE*)(pState + pFilmic->StateOffset + FilmicLensOffset) = 0;
  if (want != m_LensRedirected)
    util::log::Write(want ? "Lens distortion: drawn by the tools over the finished frame (gain %.3f, cubic gain %.3f), the game's own off" :
      "Lens distortion: the tools draw none, the game's own as it has it", gain, cubic);
  m_LensRedirected = want;
  lensdistortion::Set(want, gain, cubic);
}

// VISUALS > Weather's lightning, on top of what the game and the changes
// here set, before and after the game's update as the changes here are: the
// exposure compensation (Tonemap + 0x98) up to 1.5 EV, the sky's luminance
// (+ 0x390 + 0xF8) up to 5 times, the sky light (Outdoor light's sky color,
// + 0x60) brighter and bluer. A field the game (or a change here) set since it
// was written is taken as the new value under it; once the flash is over each
// is given back that value where ours still is. (The rainy city look was here
// too until 2026-10-25 20:xx: it's now given where the game mixes the state,
// see weather::AfterViewMix, so the sky and the sun are made from it.)
namespace
{
  enum LookFieldIndex { LookExposure, LookSkyLuminance, LookSkyLight, LookFieldCount };
  struct LookField { int Offset; int Floats; };   // state offset; floats
  const LookField LookFields[LookFieldCount] = {
    { 0x1D0 + 0x98, 1 },    // Tonemap: exposure compensation
    { 0x390 + 0xF8, 1 },    // Sky: luminance scale
    { 0x000 + 0x60, 3 },    // Outdoor light: sky color
  };
}

void VisualsManager::ApplyWeatherLook(bool beforeGameUpdate)
{
  float flash = weather::LightningFlash();
  bool active = flash > 0.f;
  if (!active && !m_WeatherLookApplied)
    return;
  if (!IsLevelLoaded())
    return;
  __int64 pManager = GetStateManager();
  if (!pManager)
    return;
  BYTE* pState = (BYTE*)(pManager + StateOffset);
  const float skyLight[3] = { 0.8f, 0.85f, 1.f };
  // Indoors the flash brightens neither the exposure nor the sky's light (both
  // light the whole frame, the room too: the user, 2026-10-25 22:1x, "it
  // should be visible only near the windows"): x how much the camera is
  // outside. The sky itself (seen through the windows) still flashes, and the
  // wet surfaces' pass lights what's under the open sky and the floor near
  // an opening.
  float rightAxis[3], backAxis[3], inside = 0.f;
  weather::GetListener(rightAxis, backAxis, inside);
  float outside = 1.f - (std::max)(0.f, (std::min)(1.f, inside));

  int k = 0;
  for (int f = 0; f < LookFieldCount; ++f)
  {
    LookField const& field = LookFields[f];
    float* base = m_WeatherLookBase + k;
    float* written = m_WeatherLookWritten + k;
    k += field.Floats;
    float* at = (float*)(pState + field.Offset);
    float now[3] = {};
    memcpy(now, at, field.Floats * sizeof(float));
    bool changed = !m_WeatherLookApplied;
    for (int i = 0; i < field.Floats; ++i)
      changed |= now[i] != written[i];
    if (changed)
      memcpy(base, now, sizeof(now[0]) * field.Floats);
    // Off: the value under it given back where ours still is
    if (!active && changed)
      continue;
    float v[3] = {};
    memcpy(v, base, sizeof(v[0]) * field.Floats);
    if (active)
    {
      if (f == LookExposure)
        v[0] += 1.5f * flash * outside;
      else if (f == LookSkyLuminance)
        v[0] *= 1.f + 4.f * flash;
      else if (f == LookSkyLight)
        for (int i = 0; i < 3; ++i)
          v[i] += flash * skyLight[i] * outside;
    }
    memcpy(at, v, field.Floats * sizeof(float));
    memcpy(written, v, sizeof(v[0]) * field.Floats);
  }

  if (active && !m_LightningApplied)
  {
    m_LightningWrites = 0;
    util::log::Write("Weather: lightning flash given to the visual environment (exposure compensation %.2f -> %.2f, the sky's "
      "luminance %.2f -> %.2f)", m_WeatherLookBase[0], m_WeatherLookWritten[0], m_WeatherLookBase[1], m_WeatherLookWritten[1]);
  }
  ++m_LightningWrites;
  if (!active && m_LightningApplied)
    util::log::Write("Weather: the lightning flash was given in %d visual environment updates", m_LightningWrites);
  m_LightningApplied = active;
  m_WeatherLookApplied = active;
}

void VisualsManager::SetAutoFocus(bool on, float distance)
{
  std::lock_guard<std::mutex> lock(m_Mutex);
  m_AutoFocusWanted = on && distance > 0.01f;
  m_AutoFocusDistance = distance;
}

bool VisualsManager::IsDofOverridden()
{
  std::lock_guard<std::mutex> lock(m_Mutex);
  return m_Components[1].Override && m_Components[1].HasValues;
}

void VisualsManager::ApplyAutoFocus(Component& dof)
{
  if ((int)dof.Values.size() < AutoFocusOffsets[AutoFocusCount - 1] + 4)
    return;
  auto field = [&](int i) { return (float*)(dof.Values.data() + AutoFocusOffsets[i]); };
  // Not while the test pictures set their own (see StartDofTest)
  bool want = m_AutoFocusWanted && dof.Override && dof.HasValues && !m_DofTest.Step;
  if (!want)
  {
    // Back as they were set (one changed in the menu meanwhile stays)
    if (m_AutoFocusApplied)
    {
      for (int i = 0; i < AutoFocusCount; ++i)
        if (*field(i) == m_AutoFocusWritten[i])
          *field(i) = m_AutoFocusBase[i];
      m_AutoFocusApplied = false;
    }
    return;
  }
  if (!m_AutoFocusApplied)
  {
    for (int i = 0; i < AutoFocusCount; ++i)
      m_AutoFocusBase[i] = *field(i);
    m_AutoFocusApplied = true;
  }
  else
  {
    // One changed in the menu since: that's the set-up at this scale now
    for (int i = 0; i < AutoFocusCount; ++i)
      if (*field(i) != m_AutoFocusWritten[i])
        m_AutoFocusBase[i] = *field(i) / m_AutoFocusScale;
  }
  float scale = m_AutoFocusDistance / AutoFocusReference(m_AutoFocusBase, m_DofMethodHasValue && m_DofMethod == 1);
  m_AutoFocusScale = scale < 0.01f ? 0.01f : (scale > 1000.f ? 1000.f : scale);
  for (int i = 0; i < AutoFocusCount; ++i)
  {
    m_AutoFocusWritten[i] = m_AutoFocusBase[i] * m_AutoFocusScale;
    *field(i) = m_AutoFocusWritten[i];
  }
}

void VisualsManager::CheckStillApplied(__int64 pState, Component& component)
{
  ULONGLONG now = GetTickCount64();
  if (now - component.EnabledTick < 1000)
    return;

  BYTE* pCurrent = (BYTE*)(pState + component.StateOffset);
  bool filmic = m_FilmicIndex < m_Components.size() && &component == &m_Components[m_FilmicIndex];
  for (Field const& field : component.Fields)
  {
    // The game's own lens distortion is written off while the tools draw it
    if (filmic && m_LensRedirected && field.Offset == FilmicLensOffset)
      continue;
    if (memcmp(pCurrent + field.Offset, component.Values.data() + field.Offset, FieldSize(field.Type)) != 0)
    {
      component.OverwrittenTick = now;
      component.OverwrittenField = field.Name;
      return;
    }
  }
}

void VisualsManager::ReadFromGame(Component& component)
{
  __int64 pManager = GetStateManager();
  if (!pManager || !IsLevelLoaded())
  {
    util::log::Warning("Visual environment not available, is a level loaded?");
    return;
  }

  memcpy(component.Values.data(), (void*)(pManager + StateOffset + component.StateOffset), component.Size);
  component.HasValues = true;
  // The sun as the game has it: what the menu's sun scales the sky's own by
  // (see ScaleSkySun)
  if ((size_t)(&component - m_Components.data()) == m_OutdoorIndex)
  {
    memcpy(m_SunRefColor, component.Values.data() + SunColorOffset, sizeof(m_SunRefColor));
    memcpy(&m_SunRefIntensity, component.Values.data() + SunIntensityOffset, sizeof(m_SunRefIntensity));
  }
  // Fresh values for auto-focus to start from
  if (&component == &m_Components[1])
    m_AutoFocusApplied = false;

  if (&component == &m_Components[1])
  {
    __int64 pRenderSettings = *(__int64*)(pManager + RenderSettingsOffset);
    if (pRenderSettings)
    {
      m_DofMethod = *(int*)(pRenderSettings + DofMethodOffset);
      if (!m_DofMethodApplied)
      {
        m_DofMethodOriginal = m_DofMethod;
        m_SpriteDofEnableOriginal = *(BYTE*)(pRenderSettings + SpriteDofOffset);
      }
      m_DofMethodHasValue = true;
    }
  }
}

// The game's weather for replays (2026-10-25, after the weather session's
// note: a mission's own weather comes through the game's visual environments,
// ToD and cloud VEs such as ve_tod_clouds_cirrus*, which carry the sky's
// cloud fields and a WindComponentData). The blended state's Clouds and Wind
// blocks (the components' StateOffset / Size) are read as the game shows them
// while a replay records, and while it plays their known fields are written
// at each visual environment update as the components' are: floats and
// colors in a straight line between the two recorded samples around the
// replay's time, anything else from the earlier one. When it stops nothing is
// written and the game's own blend puts its values back at its next update
// (as "Reset to game values" relies on). Each component's change made in the
// menu is written after it, so it wins.
size_t VisualsManager::WeatherLookSize()
{
  std::lock_guard<std::mutex> lock(m_Mutex);
  if (m_CloudsIndex >= m_Components.size() || m_WindIndex >= m_Components.size())
    return 0;
  return (size_t)m_Components[m_CloudsIndex].Size + m_Components[m_WindIndex].Size;
}

bool VisualsManager::ReadWeatherLook(BYTE* out)
{
  std::lock_guard<std::mutex> lock(m_Mutex);
  if (m_CloudsIndex >= m_Components.size() || m_WindIndex >= m_Components.size() || !IsLevelLoaded())
    return false;
  __int64 pManager = GetStateManager();
  if (!pManager)
    return false;
  Component const& clouds = m_Components[m_CloudsIndex];
  Component const& wind = m_Components[m_WindIndex];
  memcpy(out, (void const*)(pManager + StateOffset + clouds.StateOffset), clouds.Size);
  memcpy(out + clouds.Size, (void const*)(pManager + StateOffset + wind.StateOffset), wind.Size);
  return true;
}

void VisualsManager::SetReplayWeatherLook(BYTE const* a, BYTE const* b, float t)
{
  std::lock_guard<std::mutex> lock(m_Mutex);
  if (!a || m_CloudsIndex >= m_Components.size() || m_WindIndex >= m_Components.size())
  {
    m_ReplayLookActive = false;
    return;
  }
  size_t size = (size_t)m_Components[m_CloudsIndex].Size + m_Components[m_WindIndex].Size;
  m_ReplayLookA.assign(a, a + size);
  m_ReplayLookB.assign(b ? b : a, (b ? b : a) + size);
  m_ReplayLookT = t < 0.f ? 0.f : (t > 1.f ? 1.f : t);
  m_ReplayLookActive = true;
}

void VisualsManager::WriteReplayLook(__int64 pState)
{
  size_t base = 0;
  for (size_t index : { m_CloudsIndex, m_WindIndex })
  {
    Component const& component = m_Components[index];
    if (!(component.Override && component.HasValues) && m_ReplayLookA.size() >= base + component.Size &&
        m_ReplayLookB.size() >= base + component.Size)
    {
      BYTE* pTarget = (BYTE*)(pState + component.StateOffset);
      for (Field const& field : component.Fields)
      {
        BYTE const* pa = m_ReplayLookA.data() + base + field.Offset;
        BYTE const* pb = m_ReplayLookB.data() + base + field.Offset;
        int floats = field.Type == T::Float ? 1 : field.Type == T::Float2 ? 2 : field.Type == T::Float3 ||
          field.Type == T::Float3Linked || field.Type == T::Color3 ? 3 : field.Type == T::Float4 ? 4 : 0;
        if (!floats)
        {
          memcpy(pTarget + field.Offset, pa, FieldSize(field.Type));
          continue;
        }
        for (int k = 0; k < floats; ++k)
        {
          float va, vb;
          memcpy(&va, pa + k * 4, 4);
          memcpy(&vb, pb + k * 4, 4);
          float v = va + (vb - va) * m_ReplayLookT;
          memcpy(pTarget + field.Offset + k * 4, &v, 4);
        }
      }
    }
    base += component.Size;
  }
}

void VisualsManager::WriteToGame(__int64 pState, Component const& component)
{
  // Only the known fields are written, so texture pointers and anything
  // else in between always stay the game's own
  BYTE* pTarget = (BYTE*)(pState + component.StateOffset);
  for (Field const& field : component.Fields)
    memcpy(pTarget + field.Offset, component.Values.data() + field.Offset, FieldSize(field.Type));
}

void VisualsManager::DrawField(Component& component, Field const& field)
{
  void* pValue = component.Values.data() + field.Offset;
  switch (field.Type)
  {
  case T::Bool:
    ImGui::Checkbox(field.Name, (bool*)pValue);
    break;
  case T::Int:
    if (field.Items)
      ImGui::Combo(field.Name, (int*)pValue, field.Items);
    else
      ImGui::InputInt(field.Name, (int*)pValue);
    break;
  case T::Float:
    ImGui::DragFloat(field.Name, (float*)pValue, field.Speed, 0, 0, "%.3f");
    break;
  case T::Float2:
    ImGui::DragFloat2(field.Name, (float*)pValue, field.Speed, 0, 0, "%.3f");
    break;
  case T::Float3:
    ImGui::DragFloat3(field.Name, (float*)pValue, field.Speed, 0, 0, "%.3f");
    break;
  case T::Float3Linked:
  {
    // One value for red, green and blue: shown as their mean, and a change
    // moves all three by the same amount, so a tint the game set stays
    float* v = (float*)pValue;
    float mean = (v[0] + v[1] + v[2]) / 3.f;
    float value = mean;
    if (ImGui::DragFloat(field.Name, &value, field.Speed, 0, 0, "%.3f") && value != mean)
      for (int k = 0; k < 3; ++k)
        v[k] += value - mean;
    if (fabsf(v[0] - v[1]) > 0.0005f || fabsf(v[0] - v[2]) > 0.0005f)
    {
      ImGui::SameLine();
      ImGui::TextDisabled("r %.3f  g %.3f  b %.3f", v[0], v[1], v[2]);
    }
    break;
  }
  case T::Float4:
    ImGui::DragFloat4(field.Name, (float*)pValue, field.Speed, 0, 0, "%.3f");
    break;
  case T::Color3:
    // With the HDR flag ImGui's color drags have no limits (ColorEdit4 gives
    // its DragFloats 0, 0), so a drag to the left went below 0: no color of
    // the game's is, nothing here may be either
    if (ImGui::ColorEdit3(field.Name, (float*)pValue, ImGuiColorEditFlags_HDR | ImGuiColorEditFlags_Float))
    {
      float* c = (float*)pValue;
      for (int i = 0; i < 3; ++i)
        if (!(c[i] >= 0.f))
          c[i] = 0.f;
    }
    break;
  }
}

// VISUALS > Depth of field (under m_Mutex). How the game uses it (2026-11-04,
// offline: the Simple pass in the post-process 0x1435BFD03 and its composite
// 0x1435BC810 with its shader; the Sprite pass's constants 0x1435C6B4B and its
// CoC shader):
// - The near and far distances are offsets from the focus distance, in
//   metres, so they move with it. The Sprite method's are what they look like:
//   in front of the focus sharp up to Near start nearer, fully blurred at Near
//   end nearer; behind it sharp up to Far start farther, fully blurred at Far
//   end. The Simple method's far pair is the same, but its near pair isn't:
//   fully blurred at focus - Near start, sharp from focus - 2 Near start + Near
//   end (so a Near start of 0 blurred the focus itself fully). Both are shown
//   here the Sprite way and the Simple pair is written back through that
//   (Near start = fully blurred at, Near end = 2 x fully blurred at - sharp
//   from): what the game draws for the game's own values doesn't change.
// - Each end is at least its start + 0.01 m (the game moves it there).
// - Blur factor: Sprite's largest blur is the screen's height x clamp(0.01 x
//   Sprite max blur x Blur factor, 0, 1), so at 0 (the game's own value) it
//   blurred nothing whatever the focus; Simple's blur is made max(Blur factor x
//   Simple max blur, 1) wide (passed to 0x1435B42A0), so 0 is its narrowest.
// - Blur add: Sprite adds it to every pixel's blur; Simple with Enable on only
//   uses it at 1 or more (everything blurred), under 1 the depth's blur is
//   drawn without it.
void VisualsManager::DrawDofUI(Component& dof)
{
  BYTE* v = dof.Values.data();
  auto at = [&](int offset) { return (float*)(v + offset); };
  bool sprite = m_DofMethodHasValue && m_DofMethod == 1;

  ImGui::Checkbox("Enable", (bool*)(v + DofEnableOffset));
  if (m_DofMethodHasValue)
  {
    // The game's DofMethod has these two values only (see SpriteDofOffset)
    const char* methods[] = { "Simple (Gaussian)", "Sprite (bokeh)" };
    int method = m_DofMethod;
    if (method < 0 || method > 1) method = 0;
    if (ImGui::Combo("DoF method", &method, methods, 2))
      m_DofMethod = method;
    ImGui::HelpMarker("The game's two depth of field methods, each with its own distances and max blur (the other "
      "method's are under More settings). Simple blurs a half-size copy of the picture and lays it over by depth, so "
      "what's in focus bleeds into the blurred background just round it (a dark halo round dark hair). Sprite (bokeh) "
      "blurs each pixel by its own depth: no halo and a stronger, rounder blur, but its edge round what's in focus is "
      "hard (hair looks cut out). It also draws Filmic effects' edge blur and distance blur. The game itself never draws "
      "Sprite (its PC settings turn it off); choosing it here turns it on while this effect is changed.");
    sprite = method == 1;
  }

  ImGui::DragFloat("Focus distance", at(DofFocusOffset), 0.05f, 0.f, 0.f, "%.2f m");
  ImGui::HelpMarker("Where it's sharp, in metres from the camera. The distances below are in front of and behind it, so "
    "they move with it. CAMERA > Around the focus > Auto-focus keeps it on the focus.");
  if (ImGui::Button("Set up around this focus distance"))
    SetUpDofAround(dof, *at(DofFocusOffset));
  ImGui::ItemTooltip("Enable on, Blur factor 1, and for both methods: sharp from 10 % of the focus distance in front of "
    "it (at least 0.25 m) to 20 % behind it (at least 0.5 m), so a person at the focus stays sharp even close up; fully "
    "blurred at half of it in front and 1.5 times it behind (at least 0.5 m and 1 m past those). 5 m if the focus "
    "distance is under 0.1 m. Simple gets its widest filter (31) with max blur 5, Sprite max blur 3: measured at 4K, "
    "a background edge Simple blurs to about 25 pixels wide, Sprite to about 40.");

  ImGui::DragFloat("Blur factor", at(DofBlurFactorOffset), 0.01f, 0.f, 0.f, "%.2f");
  ImGui::HelpMarker("How strong the blur is, with the method's max blur. Sprite: its largest blur is max blur x this % "
    "of the screen's height (at most all of it), so at 0 nothing is blurred. Simple: its blur is a Gaussian whose "
    "spread is max blur x this (1 at least) pixels of a half-size copy of the picture, over the Simple blur filter's "
    "width (3 - 31 of those pixels): 0 is its narrowest, and a spread above a third of the filter only flattens it, "
    "it gets no wider.");
  if (!(*at(DofBlurFactorOffset) > 0.f))
    ImGui::TextColored(ImVec4(1.f, 0.8f, 0.2f, 1.f), sprite ? "Blur factor is 0: the Sprite method blurs nothing" :
      "Blur factor is 0: the Simple method's narrowest blur");

  // The distances, shown the Sprite way for both (see above); written back
  // only when changed, with the game's own limits
  auto drawRanges = [&](bool spriteFields)
  {
    ImGui::PushID(spriteFields ? "Sprite" : "Simple");
    int nearOffset = spriteFields ? DofSpriteNearOffset : DofSimpleNearOffset;
    int farOffset = spriteFields ? DofSpriteFarOffset : DofSimpleFarOffset;
    float sharpNear, fullNear;
    if (spriteFields)
    {
      sharpNear = *at(nearOffset);
      fullNear = *at(nearOffset + 4);
    }
    else
    {
      fullNear = *at(nearOffset);
      sharpNear = 2.f * fullNear - *at(nearOffset + 4);
    }
    float sharpFar = *at(farOffset), fullFar = *at(farOffset + 4);
    bool changed = false;
    changed |= ImGui::DragFloat("In front: sharp up to", &sharpNear, 0.05f, 0.f, 0.f, "%.2f m nearer");
    ImGui::HelpMarker("In front of the focus: sharp up to this much nearer than it, then more and more blurred.");
    changed |= ImGui::DragFloat("In front: fully blurred at", &fullNear, 0.05f, 0.f, 0.f, "%.2f m nearer");
    changed |= ImGui::DragFloat("Behind: sharp up to", &sharpFar, 0.05f, 0.f, 0.f, "%.2f m farther");
    ImGui::HelpMarker("Behind the focus: sharp up to this much farther than it, then more and more blurred.");
    changed |= ImGui::DragFloat("Behind: fully blurred at", &fullFar, 0.05f, 0.f, 0.f, "%.2f m farther");
    if (changed)
    {
      if (spriteFields)
        sharpNear = (std::max)(sharpNear, 0.f);
      fullNear = (std::max)((std::max)(fullNear, 0.f), sharpNear + DofEndGap);
      sharpFar = (std::max)(sharpFar, 0.f);
      fullFar = (std::max)(fullFar, sharpFar + DofEndGap);
      if (spriteFields)
      {
        *at(nearOffset) = sharpNear;
        *at(nearOffset + 4) = fullNear;
      }
      else
      {
        *at(nearOffset) = fullNear;
        *at(nearOffset + 4) = 2.f * fullNear - sharpNear;
      }
      *at(farOffset) = sharpFar;
      *at(farOffset + 4) = fullFar;
    }
    else
    {
      // The game's own values can have an end before its start: it moves it
      if (fullFar < sharpFar + DofEndGap)
        ImGui::TextDisabled("Behind: the game blurs fully at %.2f m (an end is never before its start)", sharpFar + DofEndGap);
      if (fullNear < sharpNear + DofEndGap)
        ImGui::TextDisabled("In front: the game blurs fully at %.2f m (an end is never before its start)",
          sharpNear + DofEndGap);
    }
    if (spriteFields)
    {
      ImGui::DragFloat("Max blur", at(DofSpriteMaxBlurOffset), 0.01f, 0.f, 0.f, "%.2f");
      float share = (std::min)(1.f, (std::max)(0.f, 0.01f * *at(DofSpriteMaxBlurOffset) * *at(DofBlurFactorOffset)));
      ImGui::TextDisabled("Its largest blur: %.1f %% of the screen's height", share * 100.f);
    }
    else
    {
      ImGui::DragFloat("Max blur", at(DofSimpleMaxBlurOffset), 0.01f, 0.f, 0.f, "%.2f");
      for (Field const& field : dof.Fields)
        if (field.Offset == DofSimpleFilterOffset)
          DrawField(dof, field);
      // The filter's taps (0x1435C02A5: BlurFilter 1 - 6 = 3, 5, 7, 9, 15,
      // 31; 0 none), one pixel of the blur's half-size copy apart (its
      // targets live: 1920 x 1027 for a 3840 x 2054 frame, 2026-11-04)
      static const int taps[7] = { 0, 3, 5, 7, 9, 15, 31 };
      int filter = *(int*)(v + DofSimpleFilterOffset);
      int n = filter >= 0 && filter <= 6 ? taps[filter] : 0;
      float spread = (std::max)(1.f, *at(DofBlurFactorOffset) * *at(DofSimpleMaxBlurOffset));
      if (n)
        ImGui::TextDisabled("Its blur: spread %.1f over %d pixels of a half-size copy (about %d of the screen's)", spread, n,
          2 * n);
      else
        ImGui::TextColored(ImVec4(1.f, 0.8f, 0.2f, 1.f), "Simple blur filter None: the Simple method blurs nothing");
    }
    ImGui::PopID();
  };
  drawRanges(sprite);

  ImGui::DragFloat("Blur add", at(DofBlurAddOffset), 0.01f, 0.f, 0.f, "%.2f");
  ImGui::HelpMarker("Blur added everywhere. Sprite adds it to every pixel's blur. Simple, with Enable on, only uses it "
    "at 1 or more (everything blurred); under 1 it makes no difference there.");

  if (ImGui::TreeNode("More settings"))
  {
    ImGui::TextDisabled(sprite ? "The Simple method's (used when it's the method):" : "The Sprite method's (used when it's the method):");
    drawRanges(!sprite);
    ImGui::Separator();
    for (Field const& field : dof.Fields)
      if (field.Offset == 0x59 || (field.Offset >= 0x28 && field.Offset <= 0x34) || field.Offset == 0x5B || field.Offset == 0x5C)
        DrawField(dof, field);
    ImGui::HelpMarker("Use the camera's own settings: on (and where the game's camera has its own), both methods take the "
      "camera's physical settings instead of these, the focus distance with them. The ironsights values are the game's "
      "aiming blur, drawn only while the Simple depth blur isn't.");
    if (m_DofTest.Step)
      ImGui::TextColored(ImVec4(0.45f, 0.9f, 0.45f, 1.f), "Test pictures: %d of 5", m_DofTest.Step);
    else if (ImGui::Button("Take test pictures"))
      StartDofTest();
    ImGui::HelpMarker("Five pictures of the frame as it is now, into Cinematic Tools\\Captures (and CT.log): the game's own "
      "depth of field; Simple and Sprite with the game's values and Enable on; Simple and Sprite set up around the focus "
      "(CAMERA > Around the focus's focus when there is one, otherwise this focus distance). Each is shown for 1.5 s, "
      "then everything goes back as it was. Keep the camera still meanwhile.");
    bool spriteFix = m_SpriteDofFix;
    if (ImGui::Checkbox("Sprite DoF fix", &spriteFix))
      SetSpriteDofFix(spriteFix);
    ImGui::HelpMarker("From the original tools: the Sprite depth of field makes its buffers again every frame instead of "
      "only when their size or settings change. It costs speed. The values here don't need it: the pass fills its "
      "constants from them every frame (0x1435C6B46), only its buffers wait for a change. Not needed against the crash "
      "the Sprite method's first frame caused either: that is fixed on its own.");
    ImGui::TreePop();
  }
}

// A clearly visible set-up for both methods, scaled to the focus (under
// m_Mutex): sharp from 10 % of it in front to 20 % behind, but at least 0.25
// m in front and 0.5 m behind (the test pictures, 2026-11-04: focused on Faith
// at 1.2 m, 12 cm in front and 24 cm behind left her face sharp but softened
// her shoulders by 30 % and with Sprite her legs by 37 %); fully blurred at
// half of it in front and 1.5 times it behind, at least 0.5 m and 1 m past the
// sharp ends. Simple's widest filter (31 taps of its half-size copy) with a
// spread of 5 of them, Sprite's largest blur 3 % of the screen's height. The
// change is turned on.
void VisualsManager::SetUpDofAround(Component& dof, float focus)
{
  BYTE* v = dof.Values.data();
  auto at = [&](int offset) { return (float*)(v + offset); };
  float f = focus >= 0.1f ? focus : 5.f;
  float sharpFront = (std::max)(0.1f * f, 0.25f), fullFront = (std::max)(0.5f * f, sharpFront + 0.5f);
  float sharpBehind = (std::max)(0.2f * f, 0.5f), fullBehind = (std::max)(1.5f * f, sharpBehind + 1.f);
  *at(DofFocusOffset) = f;
  v[DofEnableOffset] = 1;
  *at(DofBlurFactorOffset) = 1.f;
  *at(DofBlurAddOffset) = 0.f;
  *(int*)(v + DofSimpleFilterOffset) = 6;
  *at(DofSimpleMaxBlurOffset) = 5.f;
  // Simple's near pair the game's way (see DrawDofUI): fully blurred at, then
  // 2 x that - sharp up to
  *at(DofSimpleNearOffset) = fullFront;
  *at(DofSimpleNearOffset + 4) = 2.f * fullFront - sharpFront;
  *at(DofSimpleFarOffset) = sharpBehind;
  *at(DofSimpleFarOffset + 4) = fullBehind;
  *at(DofSpriteNearOffset) = sharpFront;
  *at(DofSpriteNearOffset + 4) = fullFront;
  *at(DofSpriteFarOffset) = sharpBehind;
  *at(DofSpriteFarOffset + 4) = fullBehind;
  *at(DofSpriteMaxBlurOffset) = 3.f;
  if (!dof.Override)
  {
    dof.Override = true;
    dof.EnabledTick = GetTickCount64();
  }
}

// VISUALS > Depth of field's test pictures (under m_Mutex; the steps run from
// the visual environment update, see RunDofTest): five set-ups, each shown
// for 1.5 s, then the frame saved (frameshot) into Cinematic Tools\Captures,
// then the set-up as it was put back (with auto-focus' unscaled one under it;
// auto-focus waits meanwhile). The focus: CAMERA > Around the focus's when
// it's found, otherwise the menu's.
void VisualsManager::StartDofTest()
{
  if (m_DofTest.Step)
    return;
  Component& dof = m_Components[1];
  if (!IsLevelLoaded() || !GetStateManager())
  {
    util::log::Warning("Depth of field test: no level loaded");
    return;
  }
  // Auto-focus' moved distances back to the set-up under them first
  if (m_AutoFocusApplied)
  {
    bool wanted = m_AutoFocusWanted;
    m_AutoFocusWanted = false;
    ApplyAutoFocus(dof);
    m_AutoFocusWanted = wanted;
  }
  if (!dof.HasValues)
    ReadFromGame(dof);
  if (!dof.HasValues)
    return;
  m_DofTest.Override = dof.Override;
  m_DofTest.HasValues = dof.HasValues;
  m_DofTest.Values = dof.Values;
  m_DofTest.Method = m_DofMethod;
  float menuFocus = *(float*)(dof.Values.data() + DofFocusOffset);
  bool cameraFocus = m_AutoFocusDistance > 0.3f;
  m_DofTest.Focus = cameraFocus ? m_AutoFocusDistance : (menuFocus >= 0.1f ? menuFocus : 5.f);
  SYSTEMTIME t;
  GetLocalTime(&t);
  swprintf_s(m_DofTest.Stamp, L"%02d%02d%02d", t.wHour, t.wMinute, t.wSecond);
  CreateDirectoryA("Cinematic Tools\\Captures", NULL);
  util::log::Write("Depth of field test: 5 pictures into Cinematic Tools\\Captures, focus %.2f m (%s)", m_DofTest.Focus,
    cameraFocus ? "the camera's focus" : "the menu's focus distance");
  m_DofTest.Step = 1;
  DofTestStep();
}

// The current step's set-up
void VisualsManager::DofTestStep()
{
  Component& dof = m_Components[1];
  BYTE* v = dof.Values.data();
  switch (m_DofTest.Step)
  {
  case 1:   // the game's own
    dof.Override = false;
    break;
  case 2:   // the game's values (read again: shown for 1.5 s with no change) with Enable on, Simple
    ReadFromGame(dof);
    v = dof.Values.data();
    v[DofEnableOffset] = 1;
    m_DofMethod = 0;
    dof.Override = true;
    dof.EnabledTick = GetTickCount64();
    break;
  case 3:   // the same, Sprite
    m_DofMethod = 1;
    break;
  case 4:   // set up around the focus, Simple
    SetUpDofAround(dof, m_DofTest.Focus);
    m_DofMethod = 0;
    break;
  case 5:   // the same, Sprite
    m_DofMethod = 1;
    break;
  }
  m_DofTest.StepTick = GetTickCount64();
  m_DofTest.ShotAsked = false;
}

void VisualsManager::RunDofTest()
{
  if (!m_DofTest.Step)
    return;
  static const wchar_t* names[6] = { L"", L"1 the game's own", L"2 Simple, game values, Enable on", L"3 Sprite, game values, Enable on",
    L"4 Simple, set up around the focus", L"5 Sprite, set up around the focus" };
  Component& dof = m_Components[1];
  ULONGLONG now = GetTickCount64();
  bool done = false;
  if (!m_DofTest.ShotAsked)
  {
    if (now - m_DofTest.StepTick < 1500)
      return;
    BYTE const* v = dof.Values.data();
    auto at = [&](int offset) { return *(float const*)(v + offset); };
    char name[64];
    WideCharToMultiByte(CP_UTF8, 0, names[m_DofTest.Step], -1, name, sizeof(name), nullptr, nullptr);
    if (dof.Override)
      util::log::Write("Depth of field test %d/5, %s: Enable %d, method %s, focus %.2f, blur factor %.2f, add %.2f; Simple filter "
        "%d, max %.2f, near %.2f / %.2f, far %.2f / %.2f; Sprite near %.2f / %.2f, far %.2f / %.2f, max %.2f", m_DofTest.Step, name,
        v[DofEnableOffset], m_DofMethod == 1 ? "Sprite" : "Simple", at(DofFocusOffset), at(DofBlurFactorOffset), at(DofBlurAddOffset),
        *(int const*)(v + DofSimpleFilterOffset), at(DofSimpleMaxBlurOffset), at(DofSimpleNearOffset), at(DofSimpleNearOffset + 4),
        at(DofSimpleFarOffset), at(DofSimpleFarOffset + 4), at(DofSpriteNearOffset), at(DofSpriteNearOffset + 4),
        at(DofSpriteFarOffset), at(DofSpriteFarOffset + 4), at(DofSpriteMaxBlurOffset));
    else
      util::log::Write("Depth of field test %d/5, %s", m_DofTest.Step, name);
    wchar_t path[MAX_PATH];
    swprintf_s(path, L"Cinematic Tools\\Captures\\DoF test %s %s.png", m_DofTest.Stamp, names[m_DofTest.Step]);
    frameshot::Request(path);
    m_DofTest.ShotAsked = true;
    return;
  }
  if (frameshot::Busy())
  {
    if (now - m_DofTest.StepTick < 30000)
      return;
    util::log::Warning("Depth of field test: picture %d wasn't saved in 30 s, stopped", m_DofTest.Step);
    done = true;
  }
  else if (++m_DofTest.Step <= 5)
  {
    DofTestStep();
    return;
  }
  else
    done = true;
  if (done)
  {
    dof.Override = m_DofTest.Override;
    dof.HasValues = m_DofTest.HasValues;
    dof.Values = m_DofTest.Values;
    dof.ResetTick = 0;
    dof.EnabledTick = now;
    m_DofMethod = m_DofTest.Method;
    m_DofTest.Step = 0;
    util::log::Write("Depth of field test: done, the depth of field is back as it was");
  }
}

void VisualsManager::DrawUI()
{
  std::lock_guard<std::mutex> lock(m_Mutex);

  float fontSize = ImGui::GetFontSize();

  ImGui::PushTextWrapPos(0.f);
  ImGui::TextDisabled("Open an effect and tick \"Change this effect\". Its current values are copied "
    "from the game, then what you set here is used every frame until you untick it. Drag a value "
    "left or right to change it, or Ctrl+click it to type a number.");
  ImGui::PopTextWrapPos();

  int overrides = 0;
  for (Component const& component : m_Components)
    overrides += component.Override ? 1 : 0;
  if (overrides > 0)
  {
    char label[64];
    sprintf_s(label, "Stop all changes (%d)", overrides);
    if (ImGui::Button(label))
    {
      // A reset still waiting would turn its change back on (see ResetTick)
      for (Component& component : m_Components)
      {
        component.Override = false;
        component.ResetTick = 0;
      }
    }
  }

  for (Component& component : m_Components)
  {
    // Fog and Wind: taken out of the menu (2026-10-25). Replays and the rainy
    // look still use their blocks.
    size_t index = &component - m_Components.data();
    if (index == m_FogIndex || index == m_WindIndex)
      continue;
    ImGui::PushID(component.Name);
    char header[96];
    sprintf_s(header, "%s%s###Header", component.Name, component.Override ? "    [changed]" : "");
    if (ImGui::CollapsingHeader(header))
    {
      bool wasOverride = component.Override;
      ImGui::Checkbox("Change this effect", &component.Override);
      if (component.Override && !wasOverride)
      {
        component.EnabledTick = GetTickCount64();
        if (!component.HasValues)
          ReadFromGame(component);
      }

      ImGui::SameLine();
      if (ImGui::Button("Reset to game values") && !component.ResetTick)
      {
        // While changed, the state holds our values. Pause the change so the
        // game puts its own back, then read them a moment later (below).
        if (component.Override)
        {
          component.Override = false;
          component.ResetTick = GetTickCount64();
        }
        else
          ReadFromGame(component);
      }
      ImGui::ItemTooltip("Copies the game's own values for this effect again.");

      if (component.ResetTick && GetTickCount64() - component.ResetTick > 300)
      {
        ReadFromGame(component);
        component.ResetTick = 0;
        component.Override = true;
        component.EnabledTick = GetTickCount64();
      }

      if (component.Override && component.HasValues)
      {
        if (GetTickCount64() - component.OverwrittenTick < 2000)
          ImGui::TextColored(ImVec4(1.f, 0.8f, 0.2f, 1.f), "The game keeps changing \"%s\", this may flicker or not show",
            component.OverwrittenField ? component.OverwrittenField : "?");
        else
          ImGui::TextColored(ImVec4(0.45f, 0.9f, 0.45f, 1.f), "Used every frame");
      }

      if (!component.HasValues)
        ImGui::TextDisabled("No values yet. Load a level, then tick \"Change this effect\".");
      else
      {
        ImGui::PushItemWidth(fontSize * 15.f);
        if (&component == &m_Components[1] && m_AutoFocusApplied)
          ImGui::TextColored(ImVec4(0.45f, 0.9f, 0.45f, 1.f), "Auto-focus (CAMERA > Around the focus): its distances x%.2f",
            m_AutoFocusScale);
        // Where this level's sun light comes from (see RecomputeSun)
        bool physical = false, fromOutdoor = false;
        __int64 pManager = index == m_OutdoorIndex && IsLevelLoaded() ? GetStateManager() : 0;
        if (pManager && ReadSunSource(pManager + StateOffset, physical, fromOutdoor) && physical && !fromOutdoor)
        {
          ImGui::PushTextWrapPos(0.f);
          ImGui::TextDisabled("This level's sky lights the world with its own sun (the sky's Light 1): Sun color and Sun "
            "intensity scale it, from what they were when this effect was copied from the game, and the sky's colour "
            "changes with it. Sun rotation moves the light and the shadows; the sun's disc in the sky stays where the "
            "game has it.");
          ImGui::PopTextWrapPos();
        }
        // Whether the renderer takes the sky and ground color (see SkyColorUnused)
        if (pManager && SkyColorUnused(pManager) == 1)
        {
          ImGui::PushTextWrapPos(0.f);
          ImGui::TextDisabled("Sky color and Ground color change nothing here: the game's renderer sets them to 0 "
            "(its bounce light, Enlighten, decides).");
          ImGui::PopTextWrapPos();
        }
        if (index == m_FilmicIndex)
        {
          ImGui::PushTextWrapPos(0.f);
          ImGui::TextDisabled("The lens distortion is drawn by the tools over the finished frame (the game's own formula), "
            "so the depth of field's blur and the tools' rain, snow, wet surfaces and green screen bend with the picture; "
            "the game's HUD, when it's shown, bends too. Edge blur and distance blur are drawn by the game's Sprite depth "
            "of field: they show only with Depth of field changed, its Enable ticked and its method Sprite. Chromatic "
            "aberration scale is how much bigger the red is drawn than the blue, from the middle of the frame out (the "
            "green half way): 1 is none, 1.005 (the city's own) 0.5 %, 1.02 about 20 pixels at the top and bottom of a "
            "4K frame. Its aspect "
            "ratio is how much of that goes sideways (0.5: half as much as up and down).");
          ImGui::PopTextWrapPos();
        }
        if (&component == &m_Components[1])
          DrawDofUI(component);
        else
        {
          size_t basicCount = component.BasicFieldCount > 0 ? (size_t)component.BasicFieldCount : component.Fields.size();
          for (size_t i = 0; i < basicCount && i < component.Fields.size(); ++i)
            DrawField(component, component.Fields[i]);

          if (basicCount < component.Fields.size() && ImGui::TreeNode("More settings"))
          {
            for (size_t i = basicCount; i < component.Fields.size(); ++i)
              DrawField(component, component.Fields[i]);
            ImGui::TreePop();
          }
        }
        ImGui::PopItemWidth();
      }
    }
    ImGui::PopID();
  }

  weather::DrawUI();
  weather::DrawSnowUI();
  DrawGreenPlane();
}

// VISUALS > Green screen (under m_Mutex): a flat rectangle of one color
// standing in the world (see greenplane); its settings go to it when changed
void VisualsManager::DrawGreenPlane()
{
  if (!ImGui::CollapsingHeader(m_Plane.On || m_Plane.GreenSky ? "Green screen    [on]###GreenPlane" : "Green screen###GreenPlane"))
    return;
  float fontSize = ImGui::GetFontSize();
  bool changed = false;
  bool wasOn = m_Plane.On;
  changed |= ImGui::Checkbox("Show it##GreenPlane", &m_Plane.On);
  ImGui::HelpMarker("A flat rectangle of one color standing in the world, to key out when editing. Whatever is in "
    "front of it (Faith, the KrugerSec, a railing) stays in front of it, and everything behind it is hidden. It's "
    "drawn over the game's finished picture in exactly its color: the exposure, the tonemap and the film effects "
    "don't change it.");
  if (m_Plane.On && !wasOn && !greenplane::IsPlaced())
    greenplane::RequestPlace();
  if (ImGui::Button("Put it where the camera looks##GreenPlane"))
    greenplane::RequestPlace();
  ImGui::HelpMarker("Stands it up straight, facing the camera, Distance away along where the camera looks. The "
    "settings below move and size it from there.");
  ImGui::PushItemWidth(fontSize * 12.f);
  changed |= ImGui::SliderFloat("Distance##GreenPlane", &m_Plane.Distance, 0.5f, 500.f, "%.2f m", 3.f);
  ImGui::HelpMarker("From where the camera was when it was put there, along where it looked.");
  changed |= ImGui::SliderFloat("Width##GreenPlane", &m_Plane.Width, 0.1f, 1000.f, "%.2f m", 3.f);
  changed |= ImGui::SliderFloat("Height##GreenPlane", &m_Plane.Height, 0.1f, 1000.f, "%.2f m", 3.f);
  changed |= ImGui::SliderFloat("Turn##GreenPlane", &m_Plane.TurnDegrees, -180.f, 180.f, "%.1f degrees");
  ImGui::HelpMarker("Round its upright middle, from facing the camera it was put down from.");
  changed |= ImGui::SliderFloat("Up / down##GreenPlane", &m_Plane.Raise, -100.f, 100.f, "%.2f m", 2.f);
  ImGui::PopItemWidth();
  changed |= ImGui::Checkbox("Keep buildings and objects##GreenPlane", &m_Plane.KeepWorld);
  ImGui::HelpMarker("Nothing in the world is hidden by it, wherever it is: it covers only the sky inside its outline. "
    "Off, everything behind it is hidden and only what's in front of it stays.");
  changed |= ImGui::Checkbox("Green sky##GreenPlane", &m_Plane.GreenSky);
  ImGui::HelpMarker("The whole sky turns the color below, everywhere on screen; buildings, objects and people stay "
    "as they are. With the rectangle shown or not.");
  ImGui::PushItemWidth(fontSize * 12.f);
  changed |= ImGui::ColorEdit3("Color##GreenPlane", m_Plane.Color);
  ImGui::PopItemWidth();
  if (m_Plane.Distance < 0.1f) m_Plane.Distance = 0.1f;
  if (m_Plane.Width < 0.01f) m_Plane.Width = 0.01f;
  if (m_Plane.Height < 0.01f) m_Plane.Height = 0.01f;
  float center[3];
  if (greenplane::GetPlace(center))
    ImGui::TextDisabled("Its middle: %.1f %.1f %.1f", center[0], center[1], center[2]);
  else
    ImGui::TextDisabled("Not put anywhere yet: it's put where the camera looks when it's first shown.");
  if (changed)
    greenplane::Set(m_Plane);
}

void VisualsManager::SetSpriteDofFix(bool enable)
{
  // The original patched OFFSET_DOFFIX with a jump to a stub that always
  // continued at +0x7E, skipping the game's "settings unchanged" checks.
  // A short jump straight to +0x7E does the same. With the guard in, those
  // bytes are under its jump: its flag does it instead.
  if (enable == m_SpriteDofFix) return;
  if (m_pSpriteDofGuard)
  {
    *(volatile BYTE*)(m_pSpriteDofGuard + GuardFlag) = enable ? 1 : 0;
    m_SpriteDofFix = enable;
    util::log::Write("Sprite DoF fix %s", enable ? "enabled" : "disabled");
    return;
  }
  if (!m_pSpriteDofFix) return;

  BYTE* pCode = (BYTE*)m_pSpriteDofFix;
  DWORD oldProtect;
  if (!VirtualProtect(pCode, 2, PAGE_EXECUTE_READWRITE, &oldProtect))
  {
    util::log::Error("Unable to patch Sprite DoF, VirtualProtect failed 0x%X", GetLastError());
    return;
  }

  if (enable)
  {
    memcpy(m_SpriteDofOriginalBytes, pCode, 2);
    pCode[0] = 0xEB; // jmp short
    pCode[1] = 0x7C; // +2 + 0x7C = +0x7E
  }
  else
    memcpy(pCode, m_SpriteDofOriginalBytes, 2);

  VirtualProtect(pCode, 2, oldProtect, &oldProtect);
  FlushInstructionCache(GetCurrentProcess(), pCode, 2);
  m_SpriteDofFix = enable;
  util::log::Write("Sprite DoF fix %s", enable ? "enabled" : "disabled");
}

// The sprite depth of field's crash guard (see GuardSiteFromFix): the sprite
// pass's look at its first target goes through a few instructions of the
// tools', which send it where the targets are made again when there's none
// yet (the same place the Sprite DoF fix sends it every frame, with the same
// registers: rbx the targets, rax the first), and otherwise do the game's own
// two instructions and go back. Its flag does the Sprite DoF fix. The game's
// 14 bytes are written once here, when the tools start: nothing runs them
// before (the game never runs the sprite pass, and where it did, its first
// frame crashed at them). Absolute jumps both ways, so the page can be
// anywhere.
static bool SpriteDofGuardCodeKnown(BYTE const* pSite, BYTE const* pRebuild)
{
  __try
  {
    return !memcmp(pSite, GuardSiteBytes, sizeof(GuardSiteBytes)) && !memcmp(pRebuild, GuardRebuildBytes, sizeof(GuardRebuildBytes));
  }
  __except (EXCEPTION_EXECUTE_HANDLER)
  {
    return false;
  }
}

void VisualsManager::InstallSpriteDofGuard()
{
  if (!m_pSpriteDofFix)
    return;
  BYTE* pSite = (BYTE*)m_pSpriteDofFix + GuardSiteFromFix;
  BYTE* pBack = (BYTE*)m_pSpriteDofFix + GuardBackFromFix;
  BYTE* pRebuild = (BYTE*)m_pSpriteDofFix + GuardRebuildFromFix;
  if (!SpriteDofGuardCodeKnown(pSite, pRebuild))
  {
    util::log::Warning("Sprite depth of field: the game's code isn't what the tools know, no crash guard (the Sprite method "
      "may crash the game on its first frame)");
    return;
  }
  BYTE* pCode = (BYTE*)VirtualAlloc(nullptr, 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
  if (!pCode)
  {
    util::log::Warning("Sprite depth of field: no memory for the crash guard (the Sprite method may crash the game on its "
      "first frame)");
    return;
  }
  auto put = [](BYTE*& p, std::initializer_list<BYTE> bytes) { for (BYTE b : bytes) *p++ = b; };
  auto put32 = [](BYTE*& p, int v) { memcpy(p, &v, 4); p += 4; };
  auto jumpTo = [&](BYTE*& p, BYTE const* pTo) { put(p, { 0xFF, 0x25, 0x00, 0x00, 0x00, 0x00 }); __int64 a = (__int64)pTo; memcpy(p, &a, 8); p += 8; };
  BYTE* p = pCode;
  put(p, { 0x48, 0x8D, 0x9F, 0xB8, 0x03, 0x00, 0x00 });                                  // lea rbx, [rdi + 0x3B8]
  put(p, { 0x48, 0x8B, 0x03 });                                                          // mov rax, [rbx]
  put(p, { 0x48, 0x85, 0xC0 });                                                          // test rax, rax
  put(p, { 0x74, 0x00 }); BYTE* pToNone = p - 1;                                         // je (none yet)
  put(p, { 0x80, 0x3D }); put32(p, (int)(pCode + GuardFlag - (p + 5))); put(p, { 0x00 }); // cmp byte [flag], 0
  put(p, { 0x75, 0x00 }); BYTE* pToRebuild = p - 1;                                      // jne (made again)
  put(p, { 0x48, 0x8B, 0x48, 0x08 });                                                    // mov rcx, [rax + 8]
  jumpTo(p, pBack);
  *pToNone = (BYTE)(p - (pToNone + 1));
  put(p, { 0xFF, 0x05 }); put32(p, (int)(pCode + GuardCount - (p + 4)));                 // inc dword [count]
  *pToRebuild = (BYTE)(p - (pToRebuild + 1));
  jumpTo(p, pRebuild);
  pCode[GuardFlag] = m_SpriteDofFix ? 1 : 0;
  memset(pCode + GuardCount, 0, 4);
  FlushInstructionCache(GetCurrentProcess(), pCode, 0x1000);

  DWORD oldProtect;
  if (!VirtualProtect(pSite, 14, PAGE_EXECUTE_READWRITE, &oldProtect))
  {
    VirtualFree(pCode, 0, MEM_RELEASE);
    util::log::Warning("Sprite depth of field: the crash guard couldn't be put in (VirtualProtect 0x%X)", GetLastError());
    return;
  }
  BYTE jump[14] = { 0xFF, 0x25, 0x00, 0x00, 0x00, 0x00 };
  __int64 a = (__int64)pCode;
  memcpy(jump + 6, &a, 8);
  memcpy(pSite, jump, sizeof(jump));
  VirtualProtect(pSite, 14, oldProtect, &oldProtect);
  FlushInstructionCache(GetCurrentProcess(), pSite, 14);
  m_pSpriteDofGuard = pCode;
  util::log::Write("Sprite depth of field: crash guard in at 0x%I64X (its first target is made before it's looked at)",
    (__int64)pSite);
}
