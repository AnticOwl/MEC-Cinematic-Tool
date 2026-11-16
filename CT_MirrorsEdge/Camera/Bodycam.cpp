#include "Bodycam.h"
#include "BodycamMotion.h"
#include "../Main.h"
#include "../Util/Util.h"
#include "../Util/GameState.h"
#include "../Util/ImGuiEXT.h"
#include "../Visuals/LensDistortion.h"
#include "../imgui/imgui.h"
#include "../inih/cpp/INIReader.h"
#include <Windows.h>
#include <atomic>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>

// See Bodycam.h and BodycamMotion.h; the notes are in Research_Bodycam_View_2026-11-14.md.
namespace
{
  const int RenderViewTransform0 = 0x00;   // LinearTransform (the free camera writes both, WriteToGameCamera)
  const int RenderViewTransform1 = 0x40;
  const int RenderViewNearOffset = 0xEC;   // float meters, 0.06 in the game's first-person view
  const int ForceFovOffset = 0x54;         // GameRenderSettings, vertical degrees, -1 = the game's own
  // m: the bodycam's near plane (a camera on her chest sees her arms closer than the game's 0.06). An odd value, so a
  // near plane holding it is the bodycam's (a game camera's own 0.03 isn't mistaken for it)
  const float BodycamNearPlane = 0.0300013f;
  // ms: the overlay and the fisheye are dropped if the camera hook hasn't run for this long (nothing turns them off then)
  const ULONGLONG StaleMs = 250;
  // ms: a health of 0 or less from the start for this long isn't trusted (a living Faith would have been seen above
  // 0 by then): it's left out for this Faith, the first-person check alone decides
  const ULONGLONG HealthTrustMs = 8000;
  const int MaxStartLogLines = 40;         // "started / stopped for this Faith" lines, per session
  // m: the game's camera this near her camera joint (bone 179) is her first-person view (as Faith's size checks it:
  // 0.000 m measured in first person; cutscene and other cameras are further). It grows by the joint's own step since
  // the last frame (a camera a frame behind the bone at speed: zip lines, falls).
  const float FirstPersonReach = 0.5f;
  const float MaxReachGrowth = 2.f;
  const int MaxViewLogLines = 40;          // "the game's camera is ... from her camera joint" lines, per session
  const int MaxTransitionLogLines = 60;    // lines about writing the view and giving it back, per session
  const int MaxFaithLogLines = 20;         // lines about a Faith's bones and her front, per session

  // A mount's name in a sentence ("worn on her chest")
  const char* MountWord(int mount)
  {
    return mount == bodycam_motion::Mount_Helmet ? "head" : mount == bodycam_motion::Mount_Shoulder ?
      "right shoulder" : "chest";
  }

  // ---- Faith's bones: her world pose, read only, as the pose editor and Faith's size read it (PoseEditor.cpp,
  // FaithSize.cpp; checked live by both): her soldier's PamClientReplayRecordingComponent (+ 0x4B0, vtable checked)
  // holds her drawn ClientAntAnimatableEntity (+ 0x50, its type checked), its pose object (+ 0x158) its pose manager
  // (+ 0x1B8) and its slot's handle (+ 0x118: byte 3 a page, byte 0 a slot). The manager: + 0x08 the bone count,
  // + 0x8C the pose stride (bones x 0x20), + 0x210 the pages; a page + 0x08 the world pose (0x20 a bone: its world
  // place x y z first). The game puts its first-person camera on bone 179 from this pose before the camera hook.
  const DWORD KnownTimeStamp = 0x5762E3B7;
  const int SoldierRecordingComponentOffset = 0x4B0;
  const __int64 RecordingComponentVTableRva = 0x1C48CD0;
  const int ComponentAnimatableOffset = 0x50;
  const int ComponentManagerOffset = 0x98;           // the pose manager her pose must be in (as PoseEditor checks)
  const __int64 AnimatableTypeInfoRva = 0x27EEE10;   // ClientAntAnimatableEntity
  const int AnimatablePoseOffset = 0x158;
  const int PoseManagerOffset = 0x1B8, PoseHandleOffset = 0x118;
  const int ManagerBoneCountOffset = 0x08, ManagerPoseStrideOffset = 0x8C, ManagerPagesOffset = 0x210;
  const int PageLocalPoseOffset = 0x00, PageWorldPoseOffset = 0x08, PageOverridesOffset = 0x30;
  const int FaithBoneCount = 182;          // skeleton_faith
  const int BoneStride = 0x20;
  // skeleton_faith's bones the bodycam reads (PoseRest.h has her rest pose: +Y up, +Z her front, +X her left)
  enum
  {
    B_Hips, B_Spine2, B_LeftArm, B_RightArm, B_Neck, B_Head, B_HeadEnd, B_LeftEye, B_RightEye, B_LeftFoot, B_RightFoot,
    B_CameraJoint, B_Count
  };
  const int BoneIndex[B_Count] = { 4, 7, 9, 117, 50, 52, 53, 67, 69, 167, 175, 179 };
  // Her health: the component at soldier + 0x198, its value at + 0x20 (its getter, vtable slot 28 = 0x142E5C3C0,
  // is checked)
  const int SoldierHealthOffset = 0x198, HealthValueOffset = 0x20, HealthGetterSlot = 28;
  const __int64 HealthGetterRva = 0x2E5C3C0;
  const unsigned char HealthGetterBytes[6] = { 0xF3, 0x0F, 0x10, 0x41, 0x20, 0xC3 };   // movss xmm0, [rcx + 20h]; ret

  std::mutex g_Mutex;                      // the menu's thread and the camera hook's
  std::atomic<int> g_View(bodycam::View_Default);
  // The look (under g_Mutex): a preset, or Custom with the preset it was changed from
  int g_Preset = bodycam_motion::Preset_Chest;
  int g_BasePreset = bodycam_motion::Preset_Chest;
  bodycam_motion::Settings g_Settings = bodycam_motion::PresetSettings(bodycam_motion::Preset_Chest);

  bodycam_motion::Motion g_Motion;
  bool g_MotionFresh = true;               // start the movement afresh on the next frame it's used
  __int64 g_Soldier = 0;                   // the Faith it's running for (0: none)
  bool g_HealthSeen = false;               // her health was above 0 since it started for her
  bool g_Killed = false;
  bool g_HealthLow = false;                // her health read at 0 or less with none above 0 before, since g_HealthLowAt
  std::chrono::steady_clock::time_point g_HealthLowAt;
  bool g_HealthIgnored = false;            // ... for HealthTrustMs: left out for this Faith
  int g_StartLogLines = 0;
  bool g_FacingLogged = false, g_BonesLogged = false, g_BonesFailLogged = false;
  int g_FaithLogLines = 0;
  std::chrono::steady_clock::time_point g_LastTick;
  bool g_HasLastTick = false;

  // What the bodycam writes, and the game's values to give back:
  //  - ForceFov (a setting the game keeps): read before the first write, written back when the bodycam stops;
  //  - the render view's near plane (the game may write it each frame or not): the game's value is taken each frame
  //    it doesn't hold the bodycam's own, and written back once the bodycam stops if it still holds its value.
  bool g_FovHeld = false;
  float g_FovSaved = -1.f;
  bool g_NearHeld = false;
  float g_NearGame = 0.f;
  bool g_Writing = false;                  // the bodycam wrote the view on the last frame
  float g_Written[16];                     // the transform it wrote last
  bool g_ViewHoldsOurs = false;            // the render view may still hold it (not rewritten by the game since)
  float g_GameAim[16];                     // the game's own camera, as last written by the game
  bool g_HaveGameAim = false;
  float g_LastJoint[3];
  bool g_HaveLastJoint = false;
  bool g_FirstPerson = false;              // the game's own camera was on her camera joint when last seen
  bool g_FirstPersonKnown = false;
  int g_OutOfReachFrames = 0;              // fresh frames in a row with the game's camera out of reach
  bool g_CheckedLastFrame = false;         // the first-person view was decided on the last frame
  float g_FarthestInReach = 0.f;           // for the log: the farthest the game's camera was while in reach
  int g_ViewLogLines = 0;
  int g_TransitionLogLines = 0;
  // Whether the game rewrites its near plane each frame (counted over the first frames the bodycam writes)
  int g_NearRewritten = 0, g_NearKept = 0;
  bool g_NearCountLogged = false;
  bool g_BadOutputLogged = false;
  // What the menu shows (written on the hook's thread, under g_Mutex)
  char g_Status[192] = "Off";
  // The overlay (read on the UI thread)
  std::atomic<bool> g_OverlayShown(false), g_OverlayRec(false);
  std::atomic<float> g_OverlayVignette(0.f);
  std::atomic<ULONGLONG> g_LastUpdateTick(0);   // when the camera hook last ran Update
  // The 10-second log line (with diagnostics on)
  ULONGLONG g_NextReport = 0;
  unsigned g_FramesWritten = 0;

  int g_CodeOk = -1;                       // -1 not checked, 0 no, 1 yes
  char g_CodeWhy[128] = "";
  bool g_CodeLogged = false;

  float ReadFloat(__int64 address, bool& ok)
  {
    __try { return *(float*)address; }
    __except (EXCEPTION_EXECUTE_HANDLER) { ok = false; return 0.f; }
  }
  bool WriteFloat(__int64 address, float value)
  {
    __try { *(float*)address = value; return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
  }
  bool WriteTransform(__int64 address, float const t[16])
  {
    __try { memcpy((void*)address, t, 64); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
  }
  bool ReadTransform(__int64 address, float out[16])
  {
    __try { memcpy(out, (void*)address, 64); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
  }
  bool Finite(float const* v, int n)
  {
    for (int i = 0; i < n; ++i)
      if (!std::isfinite(v[i]))
        return false;
    return true;
  }
  float Distance(float const* a, float const* b)
  {
    float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
    return sqrtf(dx * dx + dy * dy + dz * dz);
  }

#ifdef BODYCAM_HARNESS
  // The offline harness's own game (not in the tools' build)
  bool HarnessCode(char* why, size_t whySize);
  bool HarnessBones(__int64 soldier, float places[B_Count][3], char* why, size_t whySize);
  bool HarnessHealth(__int64 soldier, float& health);
  std::chrono::steady_clock::time_point HarnessNow();
#endif

  // The build the tools know, and her health's getter as they know it (checked once)
  bool CheckCode()
  {
    if (g_CodeOk >= 0)
      return g_CodeOk == 1;
#ifdef BODYCAM_HARNESS
    g_CodeOk = HarnessCode(g_CodeWhy, sizeof(g_CodeWhy)) ? 1 : 0;
    return g_CodeOk == 1;
#else
    g_CodeOk = 0;
    __int64 base = (__int64)g_gameHandle;
    __try
    {
      IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)base;
      IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
      if (nt->FileHeader.TimeDateStamp != KnownTimeStamp)
      {
        sprintf_s(g_CodeWhy, "the game isn't the build the tools know (PE timestamp 0x%08X)", nt->FileHeader.TimeDateStamp);
        return false;
      }
      if (memcmp((void*)(base + HealthGetterRva), HealthGetterBytes, sizeof(HealthGetterBytes)))
      {
        sprintf_s(g_CodeWhy, "the game's code at 0x%llX isn't what the tools know", 0x140000000ull + HealthGetterRva);
        return false;
      }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      sprintf_s(g_CodeWhy, "the game's code couldn't be read");
      return false;
    }
    g_CodeOk = 1;
    return true;
#endif
  }

#ifndef BODYCAM_HARNESS
  // What an object's getType gives (vtable [0]: lea rax, [rip + x]; ret), as the pose editor reads it. Inside the
  // caller's __try.
  __int64 TypeOf(__int64 object)
  {
    __int64 vtable = *(__int64*)object;
    BYTE const* code = vtable ? *(BYTE const**)vtable : nullptr;
    if (!code || code[0] != 0x48 || code[1] != 0x8D || code[2] != 0x05)
      return 0;
    return (__int64)code + 7 + *(int const*)(code + 3);
  }
#endif

  // Her bones' world places; false with why if they can't be read
  bool ReadBones(__int64 soldier, float places[B_Count][3], char* why, size_t whySize)
  {
#ifdef BODYCAM_HARNESS
    return HarnessBones(soldier, places, why, whySize);
#else
    __int64 base = (__int64)g_gameHandle;
    const char* fail = nullptr;
    int bones = 0;
    __try
    {
      __int64 component = *(__int64*)(soldier + SoldierRecordingComponentOffset);
      __int64 animatable = 0, pose = 0, manager = 0;
      if (component < 0x10000 || *(__int64*)component != base + RecordingComponentVTableRva)
        fail = "her body isn't found";
      else if ((animatable = *(__int64*)(component + ComponentAnimatableOffset)) < 0x10000 ||
        TypeOf(animatable) != base + AnimatableTypeInfoRva)
        fail = "her body isn't found";
      else if ((pose = *(__int64*)(animatable + AnimatablePoseOffset)) < 0x10000 ||
        (manager = *(__int64*)(pose + PoseManagerOffset)) < 0x10000 ||
        manager != *(__int64*)(component + ComponentManagerOffset))
        fail = "her pose isn't there";
      else
      {
        unsigned handle = *(unsigned*)(pose + PoseHandleOffset);
        bones = *(int*)(manager + ManagerBoneCountOffset);
        int stride = *(int*)(manager + ManagerPoseStrideOffset);
        __int64* pagesBegin = *(__int64**)(manager + ManagerPagesOffset);
        __int64* pagesEnd = *(__int64**)(manager + ManagerPagesOffset + 8);
        unsigned page = handle >> 24, slot = handle & 0xFF;
        if (handle == 0xFFFFFFFF)
          fail = "her pose isn't there";
        else if (bones != FaithBoneCount || stride != bones * BoneStride)
          fail = "her skeleton isn't the one the tools know";
        else if (!pagesBegin || pagesEnd < pagesBegin || page >= (unsigned)(pagesEnd - pagesBegin) ||
          pagesBegin[page] < 0x10000)
          fail = "her pose isn't there";
        else
        {
          __int64 world = *(__int64*)(pagesBegin[page] + PageWorldPoseOffset);
          if (world < 0x10000 || *(__int64*)(pagesBegin[page] + PageLocalPoseOffset) < 0x10000 ||
            *(__int64*)(pagesBegin[page] + PageOverridesOffset) < 0x10000)
            fail = "her pose isn't there";
          else
          {
            float const* w = (float const*)(world + (__int64)slot * stride);
            for (int i = 0; i < B_Count; ++i)
              memcpy(places[i], w + BoneIndex[i] * (BoneStride / 4), 3 * sizeof(float));
          }
        }
      }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      fail = "her pose couldn't be read";
    }
    if (!fail)
      for (int i = 0; i < B_Count && !fail; ++i)
        if (!Finite(places[i], 3))
          fail = "her pose has unusable values";
    if (fail)
    {
      if (bones && bones != FaithBoneCount)
        sprintf_s(why, whySize, "%s (%d bones)", fail, bones);
      else
        sprintf_s(why, whySize, "%s", fail);
      return false;
    }
    return true;
#endif
  }

  // Her health (the component's value, read as its getter does); false if it can't be read
  bool ReadHealth(__int64 soldier, float& health)
  {
#ifdef BODYCAM_HARNESS
    return HarnessHealth(soldier, health);
#else
    if (g_CodeOk != 1)
      return false;
    __try
    {
      __int64 component = *(__int64*)(soldier + SoldierHealthOffset);
      if (component < 0x10000)
        return false;
      __int64 vtable = *(__int64*)component;
      if (*(__int64*)(vtable + HealthGetterSlot * 8) != (__int64)g_gameHandle + HealthGetterRva)
        return false;
      health = *(float*)(component + HealthValueOffset);
      return std::isfinite(health);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      return false;
    }
#endif
  }

  // The lines written when the bodycam starts and stops writing the view (capped: each start and stop adds them)
  bool TransitionLog()
  {
    if (g_TransitionLogLines >= MaxTransitionLogLines)
      return false;
    if (++g_TransitionLogLines == MaxTransitionLogLines)
      util::log::Write("Bodycam: no more lines about writing the view and giving it back this session (%d written)",
        MaxTransitionLogLines);
    return true;
  }
  bool StartLog()
  {
    if (g_StartLogLines >= MaxStartLogLines)
      return false;
    if (++g_StartLogLines == MaxStartLogLines)
      util::log::Write("Bodycam: no more lines about starting and stopping for a Faith this session (%d written)",
        MaxStartLogLines);
    return true;
  }
  bool FaithLog()
  {
    if (g_FaithLogLines >= MaxFaithLogLines)
      return false;
    ++g_FaithLogLines;
    return true;
  }

  // Gives the game back its camera, field of view and near plane (where the bodycam held them). The camera: if the
  // render view still holds what the bodycam wrote (the game didn't write since), the game's own last one is put
  // back. Each is kept held, and tried again on the next frame, if it can't be read or written. One line in CT.log
  // says what was given back.
  void Release(__int64 pRenderView, __int64 pRenderSettings, const char* why)
  {
    char camera[64] = "", fov[96] = "", nearPlane[96] = "";
    if (g_ViewHoldsOurs && pRenderView)
    {
      float now[16];
      if (ReadTransform(pRenderView + RenderViewTransform1, now))
      {
        if (memcmp(now, g_Written, sizeof(now)))
          g_ViewHoldsOurs = false;   // the game wrote its own since
        else if (!g_HaveGameAim)
          g_ViewHoldsOurs = false;   // (none known to put back: the game's next write takes it)
        else if (WriteTransform(pRenderView + RenderViewTransform0, g_GameAim) &&
          WriteTransform(pRenderView + RenderViewTransform1, g_GameAim))
        {
          g_ViewHoldsOurs = false;
          sprintf_s(camera, "the game's camera (its last)");
        }
      }
    }
    if (g_FovHeld && pRenderSettings && WriteFloat(pRenderSettings + ForceFovOffset, g_FovSaved))
    {
      g_FovHeld = false;
      sprintf_s(fov, "the game's field of view (ForceFov %.4g, as read before the bodycam wrote it)", g_FovSaved);
    }
    if (g_NearHeld && pRenderView)
    {
      bool ok = true;
      float now = ReadFloat(pRenderView + RenderViewNearOffset, ok);
      if (ok && now != BodycamNearPlane)
      {
        g_NearHeld = false;
        sprintf_s(nearPlane, "the near plane the game's own already (%.4g)", now);
      }
      else if (ok && WriteFloat(pRenderView + RenderViewNearOffset, g_NearGame))
      {
        g_NearHeld = false;
        sprintf_s(nearPlane, "the game's near plane (%.4g)", g_NearGame);
      }
    }
    if ((camera[0] || fov[0] || nearPlane[0]) && TransitionLog())
      util::log::Write("Bodycam: stopped writing the view (%s); given back: %s%s%s%s%s%s", why, camera, camera[0] &&
        (fov[0] || nearPlane[0]) ? "; " : "", fov, fov[0] && nearPlane[0] ? "; " : "", nearPlane, g_FovHeld ||
        g_NearHeld || g_ViewHoldsOurs ? " (the rest is tried again next frame)" : "");
  }

  void SetLens(bool on, float fisheye)
  {
    lensdistortion::SetBodycam(on && fisheye > 0.f, fisheye);
  }

  void StepAside()
  {
    g_Writing = false;
    g_MotionFresh = true;
    g_OverlayShown = false;
    SetLens(false, 0.f);
  }

  void Stop(__int64 pRenderView, __int64 pRenderSettings, const char* why)
  {
    Release(pRenderView, pRenderSettings, why);
    if (g_Soldier && StartLog())
      util::log::Write("Bodycam: stopped for this Faith (%s)", why);
    StepAside();
    g_Soldier = 0;
    g_HealthSeen = false;
    g_Killed = false;
    g_HasLastTick = false;
    g_HaveGameAim = false;
    g_HaveLastJoint = false;
    g_FirstPersonKnown = false;
    g_OutOfReachFrames = 0;
    g_CheckedLastFrame = false;
  }

  void Start(__int64 soldier)
  {
    g_Soldier = soldier;
    g_HealthSeen = false;
    g_Killed = false;
    g_HasLastTick = false;
    g_HaveGameAim = false;
    g_HaveLastJoint = false;
    g_FirstPersonKnown = false;
    g_OutOfReachFrames = 0;
    g_MotionFresh = true;
    g_FacingLogged = g_BonesLogged = g_BonesFailLogged = false;
    g_NextReport = GetTickCount64() + 10000;
    g_FramesWritten = 0;
    g_HealthLow = false;
    g_HealthIgnored = false;
    if (StartLog())
      util::log::Write("Bodycam: started for Faith (look %s, worn on her %s); it takes her first-person view",
        bodycam_motion::PresetName(g_Preset), MountWord(g_Settings.Mount));
  }

  // Whether the game is showing her first-person view: its own camera on her camera joint. Decided only on a frame
  // whose view is the game's (fresh: not what the bodycam wrote last). It takes two frames out of reach in a row to
  // step aside (no flicker on one late frame).
  bool FirstPersonView(float const* camera, bool fresh, float const* joint)
  {
    if (!fresh)
    {
      // (the game didn't write its camera: after a frame out of reach, it counts as out of reach too, so a shot the
      // game writes once isn't held by the bodycam)
      if (g_FirstPersonKnown && g_FirstPerson && g_OutOfReachFrames > 0)
      {
        ++g_OutOfReachFrames;
        g_FirstPerson = g_OutOfReachFrames < 2;
      }
      return g_FirstPersonKnown && g_FirstPerson;
    }
    float distance = Distance(camera, joint);
    float reach = FirstPersonReach;
    if (g_HaveLastJoint)
    {
      float step = Distance(joint, g_LastJoint);
      reach += step < MaxReachGrowth ? step : MaxReachGrowth;
    }
    bool inReach = distance < reach;
    if (g_FirstPersonKnown && inReach && distance > g_FarthestInReach)
      g_FarthestInReach = distance;
    g_OutOfReachFrames = inReach ? 0 : g_OutOfReachFrames + 1;
    bool firstPerson = inReach || (g_FirstPersonKnown && g_FirstPerson && g_OutOfReachFrames < 2);
    if ((!g_FirstPersonKnown || firstPerson != g_FirstPerson) && g_ViewLogLines < MaxViewLogLines &&
      util::DiagnosticsOn())
    {
      ++g_ViewLogLines;
      util::log::Write("Bodycam: the game's camera is %.3f m from her camera joint (reach %.2f m; the farthest in "
        "reach so far %.3f m): %s", distance, reach, g_FarthestInReach, firstPerson ? "her first-person view, the "
        "bodycam's" : "not her first-person view (a cutscene or another camera), left to the game");
    }
    g_FirstPerson = firstPerson;
    g_FirstPersonKnown = true;
    return firstPerson;
  }

  void LogBones(float const places[B_Count][3], float const* camera)
  {
    // (a diagnostic: with diagnostics off it takes none of the Faith lines' room)
    if (g_BonesLogged || !util::DiagnosticsOn() || !FaithLog())
      return;
    g_BonesLogged = true;
    float const* hips = places[B_Hips];
    util::log::Write("Bodycam: her bones read (her world pose, %d bones): hips at %.2f %.2f %.2f; above them: chest "
      "%.3f m, neck %.3f m, eyes %.3f m; shoulders %.3f m apart, eyes %.3f m; the game's camera %.3f m from her camera "
      "joint", FaithBoneCount, hips[0], hips[1], hips[2], places[B_Spine2][1] - hips[1], places[B_Neck][1] - hips[1],
      0.5f * (places[B_LeftEye][1] + places[B_RightEye][1]) - hips[1], Distance(places[B_LeftArm], places[B_RightArm]),
      Distance(places[B_LeftEye], places[B_RightEye]), Distance(camera, places[B_CameraJoint]));
  }

  void Status(const char* format, ...)
  {
    va_list args;
    va_start(args, format);
    vsprintf_s(g_Status, format, args);
    va_end(args);
  }

  // The config's keys for the settings
  struct Key { const char* Name; float bodycam_motion::Settings::* Value; };
  const Key FloatKeys[] = {
    { "BodycamFollow", &bodycam_motion::Settings::Follow }, { "BodycamBounce", &bodycam_motion::Settings::Bounce },
    { "BodycamInertia", &bodycam_motion::Settings::Inertia }, { "BodycamDamping", &bodycam_motion::Settings::Damping },
    { "BodycamSteps", &bodycam_motion::Settings::Steps }, { "BodycamLanding", &bodycam_motion::Settings::Landing },
    { "BodycamShake", &bodycam_motion::Settings::Shake }, { "BodycamLean", &bodycam_motion::Settings::Lean },
    { "BodycamLevel", &bodycam_motion::Settings::Level }, { "BodycamForward", &bodycam_motion::Settings::Forward },
    { "BodycamUp", &bodycam_motion::Settings::Up }, { "BodycamSide", &bodycam_motion::Settings::Side },
    { "BodycamFov", &bodycam_motion::Settings::Fov }, { "BodycamFisheye", &bodycam_motion::Settings::Fisheye },
    { "BodycamVignette", &bodycam_motion::Settings::Vignette } };

  int ClampPreset(long preset)
  {
    return (preset >= 0 && preset < bodycam_motion::Preset_Count) ? (int)preset : bodycam_motion::Preset_Chest;
  }
  int ClampBase(long preset)
  {
    return (preset >= 0 && preset < bodycam_motion::Preset_Count && preset != bodycam_motion::Preset_Custom) ?
      (int)preset : bodycam_motion::Preset_Chest;
  }
}

const char* bodycam::ViewName(int view)
{
  return view == View_Bodycam ? "Bodycam" : "Default";
}

void bodycam::SetView(int view)
{
  int v = (view == View_Bodycam) ? View_Bodycam : View_Default;
  int old = g_View.exchange(v);
  if (old != v)
    util::log::Write("Bodycam: First camera view %s", v == View_Bodycam ? "Bodycam (the tools' own bodycam)" :
      "Default (the game's own first-person view)");
}

int bodycam::GetView()
{
  return g_View.load();
}

void bodycam::ReadConfig(INIReader* pReader)
{
  SetView((int)pReader->GetInteger("Misc", "FirstCameraView", View_Default));
  std::lock_guard<std::mutex> lock(g_Mutex);
  g_Preset = ClampPreset(pReader->GetInteger("Misc", "BodycamPreset", bodycam_motion::Preset_Chest));
  g_BasePreset = ClampBase(pReader->GetInteger("Misc", "BodycamBase", g_Preset));
  if (g_Preset != bodycam_motion::Preset_Custom)
  {
    // A preset's values are its own (a later build's better values apply)
    g_BasePreset = g_Preset;
    g_Settings = bodycam_motion::PresetSettings(g_Preset);
    return;
  }
  bodycam_motion::Settings s = bodycam_motion::PresetSettings(g_BasePreset);
  s.Mount = (int)pReader->GetInteger("Misc", "BodycamMount", s.Mount);
  for (Key const& k : FloatKeys)
    s.*k.Value = (float)pReader->GetReal("Misc", k.Name, s.*k.Value);
  s.Overlay = pReader->GetBoolean("Misc", "BodycamOverlay", s.Overlay);
  bodycam_motion::Clamp(s);
  g_Settings = s;
}

void bodycam::WriteConfig(std::string& config)
{
  // 0 Default (the game's view), 1 Bodycam (Bodycam.cpp)
  config += "FirstCameraView = " + std::to_string(GetView()) + "\n";
  std::lock_guard<std::mutex> lock(g_Mutex);
  // The look: 0 Chest bodycam, 1 Helmet cam, 2 Shoulder cam, 3 Action cam, 4 Custom (the values below, changed from
  // BodycamBase), 5 GoPro (head mount)
  config += "BodycamPreset = " + std::to_string(g_Preset) + "\n";
  config += "BodycamBase = " + std::to_string(g_BasePreset) + "\n";
  config += "BodycamMount = " + std::to_string(g_Settings.Mount) + "\n";
  for (Key const& k : FloatKeys)
    config += std::string(k.Name) + " = " + std::to_string(g_Settings.*k.Value) + "\n";
  config += std::string("BodycamOverlay = ") + (g_Settings.Overlay ? "true" : "false") + "\n";
}

bool bodycam::DrawUI()
{
  bool changed = false;
  static const char* names[View_Count] = { "Default", "Bodycam" };
  int view = g_View.load();
  int picked = view;
  if (ImGui::Combo("First camera view", &picked, names, View_Count) && picked != view)
  {
    SetView(picked);
    view = picked;
    changed = true;
  }
  ImGui::HelpMarker("Default: the game's own first-person view.\n\nBodycam: a camera worn on Faith's body (the tools' "
    "own). It hangs on her chest, helmet or shoulder and moves with her: it gives a little on its strap, takes her "
    "steps and landings, shakes while she runs and leans into turns, through a wide lens. Your mouse moves it at once "
    "(Follow your aim sets how much) and it stops when the mouse stops; her body's own turns sway it.\n\nIt only "
    "takes her first-person view (the game's camera at her eyes, scripted first-person moments too): cutscenes and "
    "other cameras stay the game's. While the CAMERA tab's camera is on, that camera's view is shown; when she dies "
    "the game's view comes back until she's up again. Back on Default, the game's view, field of view and near plane "
    "are as before.");
  if (view != View_Bodycam)
    return changed;

  int preset;
  int basePreset;
  bodycam_motion::Settings s;
  char status[192];
  {
    std::lock_guard<std::mutex> lock(g_Mutex);
    preset = g_Preset;
    basePreset = g_BasePreset;
    s = g_Settings;
    strcpy_s(status, g_Status);
  }
  ImGui::TextDisabled("%s", status);

  bool edited = false;     // a value changed by hand: the look becomes Custom
  bool presetChanged = false;
  // The looks in the menu's order (Custom last), the config's numbers kept
  const char* presetNames[bodycam_motion::Preset_Count];
  int shown = 0;
  for (int i = 0; i < bodycam_motion::Preset_Count; ++i)
  {
    presetNames[i] = bodycam_motion::PresetName(bodycam_motion::PresetInMenu(i));
    if (bodycam_motion::PresetInMenu(i) == preset)
      shown = i;
  }
  int picked2 = shown;
  if (ImGui::Combo("Look", &picked2, presetNames, bodycam_motion::Preset_Count) && picked2 != shown)
  {
    int newPreset = bodycam_motion::PresetInMenu(picked2);
    presetChanged = true;
    if (newPreset != bodycam_motion::Preset_Custom)
    {
      s = bodycam_motion::PresetSettings(newPreset);
      basePreset = newPreset;
    }
    preset = newPreset;
  }
  ImGui::HelpMarker("Chest bodycam: on her chest, a wide lens, the most movement.\nHelmet cam: at the side of her head, "
    "close to where she looks.\nShoulder cam: on her right shoulder strap.\nAction cam (steady): a stabilized camera "
    "on her head that keeps the horizon level.\nGoPro (head mount): on her forehead, the widest view and the strongest "
    "fisheye, lightly stabilized (the horizon half levelled), no overlay.\nCustom: your own values (any change below "
    "makes it Custom).");

  if (ImGui::TreeNode("Movement"))
  {
    edited |= ImGui::SliderFloat("Follow your aim", &s.Follow, 0.f, 1.f, "%.2f");
    ImGui::HelpMarker("How much it looks where you aim. 1: exactly where you aim; less: partly where her body faces.");
    edited |= ImGui::SliderFloat("Strap bounce", &s.Bounce, 0.f, 1.f, "%.2f");
    ImGui::HelpMarker("0: held rigidly; 1: on a loose strap (it gives more under her steps and landings).");
    edited |= ImGui::SliderFloat("Body sway", &s.Inertia, 0.f, 1.f, "%.2f");
    ImGui::HelpMarker("How slowly her body's turns and tilts reach the camera (0: at once). Your aim isn't slowed.");
    edited |= ImGui::SliderFloat("Settle", &s.Damping, 0.2f, 1.2f, "%.2f");
    ImGui::HelpMarker("How quickly bounce and sway settle. Low: it swings back and forth; 1 and more: no overshoot.");
    edited |= ImGui::SliderFloat("Step jolts", &s.Steps, 0.f, 2.f, "%.2f");
    edited |= ImGui::SliderFloat("Landing impacts", &s.Landing, 0.f, 2.f, "%.2f");
    edited |= ImGui::SliderFloat("Shake", &s.Shake, 0.f, 2.f, "%.2f");
    ImGui::HelpMarker("A fine shake, more while she runs and after a hard landing.");
    edited |= ImGui::SliderFloat("Lean into turns", &s.Lean, 0.f, 2.f, "%.2f");
    edited |= ImGui::SliderFloat("Level horizon", &s.Level, 0.f, 1.f, "%.2f");
    ImGui::HelpMarker("Takes the roll out (a stabilized camera). 1: the horizon stays level, even in wall runs.");
    ImGui::TreePop();
  }
  if (ImGui::TreeNode("Where it's worn"))
  {
    const char* mountNames[bodycam_motion::Mount_Count];
    for (int i = 0; i < bodycam_motion::Mount_Count; ++i)
      mountNames[i] = bodycam_motion::MountName(i);
    int mount = s.Mount;
    if (ImGui::Combo("Worn on", &mount, mountNames, bodycam_motion::Mount_Count) && mount != s.Mount)
    {
      s.Mount = mount;
      edited = true;
    }
    edited |= ImGui::SliderFloat("Forward", &s.Forward, -0.1f, 0.4f, "%.3f m");
    edited |= ImGui::SliderFloat("Up", &s.Up, -0.2f, 0.3f, "%.3f m");
    edited |= ImGui::SliderFloat("Side", &s.Side, -0.25f, 0.25f, "%.3f m");
    ImGui::HelpMarker("From the place it's worn, in her own directions (Side: toward her right). Chest: her upper "
      "chest; helmet: between her eyes; shoulder: her right shoulder.");
    ImGui::TreePop();
  }
  if (ImGui::TreeNode("Lens and picture"))
  {
    edited |= ImGui::SliderFloat("Field of view", &s.Fov, 50.f, 120.f, "%.0f deg");
    ImGui::HelpMarker("Top to bottom, in degrees.");
    edited |= ImGui::SliderFloat("Fisheye", &s.Fisheye, 0.f, 0.6f, "%.2f");
    ImGui::HelpMarker("The wide lens's bend (the tools' lens distortion). While VISUALS > Lens distortion is on, "
      "that one is used instead.");
    edited |= ImGui::Checkbox("REC overlay", &s.Overlay);
    ImGui::HelpMarker("A blinking REC, the date and time and the camera's number over the picture (not over the "
      "menu's windows).");
    edited |= ImGui::SliderFloat("Vignette", &s.Vignette, 0.f, 1.f, "%.2f");
    ImGui::TreePop();
  }
  if (edited)
  {
    bodycam_motion::Clamp(s);
    preset = bodycam_motion::Preset_Custom;
  }
  if (preset == bodycam_motion::Preset_Custom)
  {
    char label[96];
    sprintf_s(label, "Back to %s", bodycam_motion::PresetName(basePreset));
    if (ImGui::Button(label))
    {
      preset = basePreset;
      s = bodycam_motion::PresetSettings(basePreset);
      presetChanged = true;
    }
  }
  if (edited || presetChanged)
  {
    std::lock_guard<std::mutex> lock(g_Mutex);
    bool lookChanged = g_Preset != preset;
    g_Preset = preset;
    g_BasePreset = basePreset;
    if (s.Mount != g_Settings.Mount)
      g_MotionFresh = true;   // worn elsewhere: its strap starts at the new place
    g_Settings = s;
    if (lookChanged)
      util::log::Write("Bodycam: look %s (worn on her %s, FOV %.0f)", bodycam_motion::PresetName(preset),
        MountWord(s.Mount), s.Fov);
    changed = true;
  }
  return changed;
}

void bodycam::Update(__int64 pRenderView, __int64 pRenderSettings, __int64 soldier, bool freeCamera, float worldScale)
{
  std::lock_guard<std::mutex> lock(g_Mutex);
  g_LastUpdateTick = GetTickCount64();
  if (g_View.load() != View_Bodycam)
  {
    if (g_Soldier || g_FovHeld || g_NearHeld || g_Writing || g_ViewHoldsOurs)
      Stop(pRenderView, pRenderSettings, "First camera view is Default");
    strcpy_s(g_Status, "Off");
    return;
  }
  // No Faith, or a loading screen: nothing of hers is read; it starts afresh when she's there
  bool loading = gamestate::Loading();
  if (!soldier || loading || !pRenderView)
  {
    const char* why = loading ? "a loading screen" : !soldier ? "Faith is gone" : "no render view";
    if (g_Soldier || g_FovHeld || g_NearHeld || g_Writing || g_ViewHoldsOurs)
      Stop(pRenderView, pRenderSettings, why);
    Status(loading ? "Waiting for the loading screen" : "Waiting for Faith");
    return;
  }
  if (!CheckCode())
  {
    if (!g_CodeLogged)
    {
      g_CodeLogged = true;
      util::log::Warning("Bodycam: not running: %s; the game's view stays", g_CodeWhy);
    }
    Status("Not running: %s", g_CodeWhy);
    return;
  }
  // A new Faith (a respawn, a new level): started afresh
  if (soldier != g_Soldier)
  {
    if (g_Soldier)
      Stop(pRenderView, pRenderSettings, "a new Faith");
    Start(soldier);
  }
  // Game time since the last frame: real time times the game's speed; nothing while it's paused or frozen
#ifdef BODYCAM_HARNESS
  auto now = HarnessNow();
#else
  auto now = std::chrono::steady_clock::now();
#endif
  double dt = g_HasLastTick ? std::chrono::duration<double>(now - g_LastTick).count() : 0.0;
  g_LastTick = now;
  g_HasLastTick = true;
  double gameDt = gamestate::Paused() ? 0.0 : dt * (worldScale > 0.f ? worldScale : 0.f);

  // Her health at 0 or less (killed, or not up yet): the game's view; above 0 again: afresh. At 0 or less from the
  // start for HealthTrustMs, it isn't trusted (the value read may not be her health): left out for this Faith
  float health;
  if (!g_HealthIgnored && ReadHealth(soldier, health))
  {
    bool alive = health > 0.f;
    if (!alive && !g_HealthSeen)
    {
      if (!g_HealthLow)
      {
        g_HealthLow = true;
        g_HealthLowAt = now;
      }
      else if (std::chrono::duration<double>(now - g_HealthLowAt).count() * 1000.0 >= (double)HealthTrustMs)
      {
        g_HealthIgnored = true;
        alive = true;
        if (FaithLog())
          util::log::Warning("Bodycam: her health has read %.4g since she was found (%llu s), never above 0: it isn't "
            "trusted for this Faith; only the first-person check decides", health, HealthTrustMs / 1000);
      }
    }
    if (alive && g_Killed)
    {
      g_MotionFresh = true;
      if (g_HealthSeen && FaithLog())
        util::log::Write("Bodycam: Faith is back (her health is above 0 again)");
    }
    else if (!alive && !g_Killed && g_HealthSeen && FaithLog())
      util::log::Write("Bodycam: Faith was killed (her health is %.4g): the game's view until she's back", health);
    g_Killed = !alive;
    if (alive && !g_HealthIgnored)
      g_HealthSeen = true;
  }
  if (g_HealthIgnored)
    g_Killed = false;

  float places[B_Count][3];
  char why[128] = "";
  bool haveBones = ReadBones(soldier, places, why, sizeof(why));
  if (!haveBones && !g_BonesFailLogged && FaithLog())
  {
    g_BonesFailLogged = true;
    util::log::Warning("Bodycam: Faith's bones can't be read (%s): the game's view stays", why);
  }
  float view[16];
  // (the rows' and the place's x y z: the w column isn't used)
  bool haveView = ReadTransform(pRenderView + RenderViewTransform1, view) && Finite(view, 3) && Finite(view + 4, 3) &&
    Finite(view + 8, 3) && Finite(view + 12, 3);
  // The game's own camera: what the view holds unless it's still what the bodycam wrote (the game didn't write
  // its camera since)
  bool fresh = haveView && !(g_ViewHoldsOurs && !memcmp(view, g_Written, sizeof(view)));
  if (fresh)
  {
    g_ViewHoldsOurs = false;
    memcpy(g_GameAim, view, sizeof(view));
    g_HaveGameAim = true;
  }
  if (haveBones && haveView)
    LogBones(places, g_GameAim + 12);

  // (after a frame it wasn't decided on, it's decided afresh: nothing carried over a gap)
  bool deciding = haveBones && haveView && g_HaveGameAim && !freeCamera && !g_Killed;
  bool checkedLastFrame = g_CheckedLastFrame;
  g_CheckedLastFrame = deciding;
  if (deciding && !checkedLastFrame)
  {
    g_FirstPersonKnown = false;
    g_OutOfReachFrames = 0;
  }
  bool firstPerson = deciding && FirstPersonView(view + 12, fresh, places[B_CameraJoint]);
  if (haveBones)
  {
    memcpy(g_LastJoint, places[B_CameraJoint], sizeof(g_LastJoint));
    g_HaveLastJoint = true;
  }
  else
    g_HaveLastJoint = false;

  float out[16];
  bool moved = false;
  bodycam_motion::Settings const s = g_Settings;
  if (firstPerson)
  {
    if (g_MotionFresh)
    {
      g_Motion.Reset();
      g_MotionFresh = false;
    }
    bodycam_motion::Input in;
    memcpy(in.Aim, g_GameAim, sizeof(in.Aim));
    memcpy(in.Hips, places[B_Hips], 12);
    memcpy(in.Neck, places[B_Neck], 12);
    memcpy(in.LeftArm, places[B_LeftArm], 12);
    memcpy(in.RightArm, places[B_RightArm], 12);
    memcpy(in.Spine2, places[B_Spine2], 12);
    memcpy(in.Head, places[B_Head], 12);
    memcpy(in.HeadEnd, places[B_HeadEnd], 12);
    memcpy(in.LeftEye, places[B_LeftEye], 12);
    memcpy(in.RightEye, places[B_RightEye], 12);
    memcpy(in.LeftFoot, places[B_LeftFoot], 12);
    memcpy(in.RightFoot, places[B_RightFoot], 12);
    moved = g_Motion.Update(in, s, gameDt, out) && Finite(out, 16);
    if (!moved && !g_BadOutputLogged)
    {
      g_BadOutputLogged = true;
      util::log::Warning("Bodycam: its movement had no usable camera for this frame (the game's view or her bones "
        "aren't usable): the game's view meanwhile");
    }
    bodycam_motion::Stats const& st = g_Motion.GetStats();
    if (st.Facing != 0 && !g_FacingLogged && util::DiagnosticsOn() && FaithLog())
    {
      g_FacingLogged = true;
      util::log::Write("Bodycam: her front found over %d frames (%d with her up x right toward the game's view): %s",
        st.FacingVotes, st.FacingAgree, st.Facing > 0 ? "up x right, as her rest pose says" :
        "right x up, the other way from her rest pose");
    }
  }
  if (!firstPerson || !moved)
  {
    const char* reason = freeCamera ? "the CAMERA tab's camera is on" : g_Killed ? (g_HealthSeen ? "Faith was killed" :
      "her health is 0 (not up yet)") : !haveBones ?
      "her bones can't be read" : !g_HaveGameAim ? "the game's camera can't be read" : !firstPerson ?
      "not her first-person view" : "no usable camera this frame";
    // The game's field of view written back. With the free camera on, it owns ForceFov from now on (it writes its own
    // later in this hook and -1 when it's turned off): if the write back failed, the hold is dropped all the same
    Release(pRenderView, pRenderSettings, reason);
    if (freeCamera && g_FovHeld)
    {
      g_FovHeld = false;
      if (TransitionLog())
        util::log::Write("Bodycam: ForceFov is the free camera's now");
    }
    StepAside();
    if (freeCamera)
      Status("Steps aside: the CAMERA tab's camera is on");
    else if (!haveBones)
      Status("Not running: %s (see CT.log)", why);
    else if (g_Killed)
      Status(g_HealthSeen ? "Faith was killed: the game's view until she's up" : "Waiting: her health is 0 (not up yet)");
    else if (!firstPerson)
      Status("Steps aside: the game isn't showing her first-person view");
    else
      Status("Steps aside: no usable camera this frame (see CT.log)");
    return;
  }

  // The bodycam writes the view. The game's values first (ForceFov once; the near plane whenever it doesn't hold
  // the bodycam's own)
  if (!g_FovHeld && pRenderSettings)
  {
    bool ok = true;
    float game = ReadFloat(pRenderSettings + ForceFovOffset, ok);
    if (ok)
    {
      g_FovSaved = game;
      g_FovHeld = true;
    }
  }
  {
    bool ok = true;
    float nowNear = ReadFloat(pRenderView + RenderViewNearOffset, ok);
    if (ok)
    {
      // Counted over the frames that follow one the bodycam wrote, then said once (a diagnostic)
      if (g_NearHeld && g_Writing && !g_NearCountLogged && util::DiagnosticsOn())
      {
        if (nowNear == BodycamNearPlane)
          ++g_NearKept;
        else
          ++g_NearRewritten;
        if (g_NearKept + g_NearRewritten >= 300)
        {
          g_NearCountLogged = true;
          util::log::Write("Bodycam: over %d frames after one it wrote, the render view's near plane was the game's "
            "again on %d and still the bodycam's on %d (the game's: %.4g)", g_NearKept + g_NearRewritten,
            g_NearRewritten, g_NearKept, g_NearGame);
        }
      }
      if (!(g_NearHeld && nowNear == BodycamNearPlane))
        g_NearGame = nowNear;
      g_NearHeld = true;
    }
  }
  bool wrote = WriteTransform(pRenderView + RenderViewTransform0, out) &&
    WriteTransform(pRenderView + RenderViewTransform1, out) &&
    (!g_NearHeld || WriteFloat(pRenderView + RenderViewNearOffset, BodycamNearPlane));
  if (g_FovHeld && pRenderSettings)
    wrote = WriteFloat(pRenderSettings + ForceFovOffset, s.Fov) && wrote;
  memcpy(g_Written, out, sizeof(g_Written));
  g_ViewHoldsOurs = true;
  if (!g_Writing && TransitionLog())
  {
    char kept[128];
    if (g_FovHeld && g_NearHeld)
      sprintf_s(kept, "the game's ForceFov was %.4g and its near plane %.4g, kept to give back", g_FovSaved, g_NearGame);
    else if (g_FovHeld)
      sprintf_s(kept, "the game's ForceFov was %.4g, kept to give back; its near plane couldn't be read", g_FovSaved);
    else if (g_NearHeld)
      sprintf_s(kept, "its near plane was %.4g, kept to give back; ForceFov couldn't be read", g_NearGame);
    else
      sprintf_s(kept, "ForceFov and the near plane couldn't be read");
    util::log::Write("Bodycam: applied: the view is the bodycam's (look %s, worn on her %s, FOV %.0f, near plane %.3g, "
      "fisheye %.2f; %s)%s", bodycam_motion::PresetName(g_Preset), MountWord(s.Mount), s.Fov, BodycamNearPlane,
      s.Fisheye, kept, wrote ? "" : "; a write failed");
  }
  g_Writing = true;
  SetLens(true, s.Fisheye);
  g_OverlayRec = s.Overlay;
  g_OverlayVignette = s.Vignette;
  g_OverlayShown = s.Overlay || s.Vignette > 0.001f;
  ++g_FramesWritten;
  bodycam_motion::Stats const& st = g_Motion.GetStats();
  Status("On: %s, worn on her %s (FOV %.0f); %u steps, %u landings so far", bodycam_motion::PresetName(g_Preset),
    MountWord(s.Mount), s.Fov, st.Steps, st.Landings);
  if (util::DiagnosticsOn() && GetTickCount64() >= g_NextReport)
  {
    g_NextReport = GetTickCount64() + 10000;
    util::log::Write("Bodycam (10 s, diagnostic): %u frames written; her speed %.2f m/s; %u steps, %u landings (the "
      "last at %.2f m/s), %u restarts; front %d (%d of %d); camera at %.2f %.2f %.2f, %.3f m from the game's",
      g_FramesWritten, st.Speed, st.Steps, st.Landings, st.LastImpact, st.Resets, st.Facing, st.FacingAgree,
      st.FacingVotes, out[12], out[13], out[14], Distance(out + 12, g_GameAim + 12));
    g_FramesWritten = 0;
  }
}

bool bodycam::WantsOverlay()
{
  // (not when the camera hook stopped calling Update: nothing would turn it off then)
  return g_OverlayShown.load() && GetTickCount64() - g_LastUpdateTick.load() < StaleMs;
}

void bodycam::DrawOverlay()
{
  if (!WantsOverlay())
    return;
  bool rec = g_OverlayRec.load();
  float vignette = g_OverlayVignette.load();
  ImVec2 size = ImGui::GetIO().DisplaySize;
  if (!(size.x > 16.f && size.y > 16.f))
    return;
  // A window over the whole screen that takes no input, behind the menu's (as CAMERA's guides)
  ImGui::SetNextWindowPos(ImVec2(0.f, 0.f));
  ImGui::SetNextWindowSize(size);
  ImGui::SetNextWindowBgAlpha(0.f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.f, 0.f));
  ImGui::Begin("##Bodycam", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
    ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoInputs |
    ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoBringToFrontOnFocus);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  if (vignette > 0.001f)
  {
    // Dark edges, darker in the corners (where two meet)
    int a = (int)(vignette * 200.f);
    a = a < 0 ? 0 : (a > 255 ? 255 : a);
    ImU32 dark = IM_COL32(0, 0, 0, a), clear = IM_COL32(0, 0, 0, 0);
    float bandY = size.y * 0.28f, bandX = size.x * 0.22f;
    dl->AddRectFilledMultiColor(ImVec2(0.f, 0.f), ImVec2(size.x, bandY), dark, dark, clear, clear);
    dl->AddRectFilledMultiColor(ImVec2(0.f, size.y - bandY), ImVec2(size.x, size.y), clear, clear, dark, dark);
    dl->AddRectFilledMultiColor(ImVec2(0.f, 0.f), ImVec2(bandX, size.y), dark, clear, clear, dark);
    dl->AddRectFilledMultiColor(ImVec2(size.x - bandX, 0.f), ImVec2(size.x, size.y), clear, dark, dark, clear);
  }
  if (rec)
  {
    ImFont* font = ImGui::GetFont();
    float fontSize = size.y / 36.f;
    fontSize = fontSize < 12.f ? 12.f : fontSize;
    float margin = size.y * 0.045f;
    ImU32 white = IM_COL32(235, 235, 235, 230), shadow = IM_COL32(0, 0, 0, 160), red = IM_COL32(230, 30, 30, 240);
    auto text = [&](ImVec2 p, const char* t)
    {
      dl->AddText(font, fontSize, ImVec2(p.x + 1.5f, p.y + 1.5f), shadow, t);
      dl->AddText(font, fontSize, p, white, t);
    };
    // REC, its dot blinking once a second
    bool lit = (GetTickCount64() % 1000) < 600;
    float r = fontSize * 0.32f;
    if (lit)
      dl->AddCircleFilled(ImVec2(margin + r, margin + fontSize * 0.5f), r, red, 20);
    text(ImVec2(margin + 2.6f * r, margin), "REC");
    // The date and time (this computer's clock)
    SYSTEMTIME t;
    GetLocalTime(&t);
    char stamp[64];
    sprintf_s(stamp, "%04u-%02u-%02u  %02u:%02u:%02u", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    ImVec2 stampSize = font->CalcTextSizeA(fontSize, FLT_MAX, 0.f, stamp);
    text(ImVec2(size.x - margin - stampSize.x, margin), stamp);
    text(ImVec2(margin, size.y - margin - fontSize), "CAM 01");
  }
  ImGui::End();
  ImGui::PopStyleVar(2);
}
