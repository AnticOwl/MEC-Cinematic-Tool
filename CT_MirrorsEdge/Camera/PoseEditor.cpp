#include "PoseEditor.h"
#include "PoseSkeletons.h"
#include "PoseSkeletonLive.h"
#include "AttachTargets.h"
#include "CameraCollision.h"
#include "PausedHair.h"
#include "PoseClips.h"
#include "PoseKeys.h"
#include "PoseRest.h"
#include "Voice.h"
#include "VoiceAlive.h"
#include "VoiceFeel.h"
#include "VoiceGaze.h"
#include "VoiceEmotion.h"
#include "VoicePlayer.h"
#include "../Main.h"
#include "../inih/cpp/INIReader.h"
#include "../Util/Util.h"
#include "../imgui/imgui.h"
#include "../imgui/imgui_internal.h"
#include "../Util/ImGuiEXT.h"
#include "../Visuals/Panini.h"
#include <Windows.h>
#include <ShObjIdl.h>
#include <DirectXMath.h>
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// The keyframes' file window (COM's file dialog, SHCreateItemFromParsingName; 2026-11-16)
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")

// See PoseEditor.h and Research_Pose_Editor_2026-11-08.md
namespace
{
  // The game build these offsets are from (the replay and Faith's size check
  // the same PE timestamp)
  const DWORD KnownTimeStamp = 0x5762E3B7;

  // Faith's body, found the way the replay finds it (FaithReplay.cpp): her
  // soldier's PamClientReplayRecordingComponent (+ 0x4B0) holds her drawn
  // ClientAntAnimatableEntity (+ 0x50) and her skeleton's pose manager
  // (+ 0x98); her box pose (her culling box's bones) is the animatable at
  // soldier + 0x340, another slot of the same manager.
  const int SoldierRecordingComponentOffset = 0x4B0;
  const __int64 RecordingComponentVTableRva = 0x1C48CD0;
  const int ComponentAnimatableOffset = 0x50;
  const int ComponentManagerOffset = 0x98;
  const int SoldierBoxAnimatableOffset = 0x340;
  // A character's body (the replay's people), a drone's or a gun's (actors)
  const int CharacterBodyOffset = 0x248;
  const int ActorBodyOffset = 0x100;
  const int WeaponOwnerOffset = 0x120;               // a gun's character (FaithReplay.cpp)
  const __int64 AnimatableTypeInfoRva = 0x27EEE10;   // ClientAntAnimatableEntity
  const __int64 WeaponTypeInfoRva = 0x284FB60;       // PamClientCharacterWeaponEntity
  const __int64 DroneTypeInfoRva = 0x2855C20;        // PamClientDroneActorEntity
  const __int64 ActorTypeInfoRva = 0x27BCC10;        // ClientActorEntity (its base class; decompiled 2026-11-12)
  const int TypeInstanceListsOffset = 0x60;          // a class's 4 instance lists, 0x10 apart
  const int EntityLinkOffset = 0x40;
  const int MaxPerList = 4096;
  // A body's pose: its pose object (animatable + 0x158) has its pose manager
  // (+ 0x1B8) and its slot's handle (+ 0x118: byte 3 a page, byte 0 a slot).
  // The manager: + 0x08 the bone count (0x142FD7814), + 0x8C the pose stride
  // per slot (bones x 0x20), + 0xF0 the parent of each bone (u32, -1 none: the
  // world pose is built from it, 0x142FD77E8 / 0x142FD021C), + 0x210 pages.
  // A page: + 0x00 the local pose, + 0x08 the world pose (0x20 a bone:
  // translation x y z, the bone's scale, then a rotation quaternion), + 0x30
  // one override per slot.
  const int AnimatablePoseOffset = 0x158;
  const int PoseManagerOffset = 0x1B8;
  const int PoseHandleOffset = 0x118;
  const int ManagerBoneCountOffset = 0x08;
  const int ManagerPoseStrideOffset = 0x8C;
  const int ManagerParentsOffset = 0xF0;
  const int ManagerPagesOffset = 0x210;
  const int PageLocalPoseOffset = 0x00;
  const int PageWorldPoseOffset = 0x08;
  const int PageOverridesOffset = 0x30;
  const __int64 GameAllocatorRva = 0x2364F80;        // what the game's own override vectors carry
  // Whether the game animates a body (FaithReplay.cpp's UpdatedByGame): body
  // = animatable + 0x70, "always update" + 0x5C3, + 0x5C0, mode [+ 0x118] +
  // 0x10 (4: never; 1 and 3 only when external)
  const int BodyUpdateOffset = 0x70;
  const int BodyExternalOffset = 0x5C0;
  const int BodyAlwaysUpdateOffset = 0x5C3;
  const int BodyUpdateModeOffset = 0x118;
  // The main render view (GameRenderer + 0x58: + 0x40 its transform, + 0x360
  // its projection), as the lights' dots read it
  const int RenderViewOffset = 0x58;
  const int RenderViewTransform = 0x40;
  const int RenderViewProjection = 0x360;

  const int FloatsPerBone = 8;
  const int MaxBones = 512;
  const int MaxHeld = 8;
  static_assert(MaxBones == pose_clips::MaxBones, "PoseClips.h sizes its arrays by the pose editor's MaxBones");
  const int MaxCandidates = 40;
  const float ScanRadius = 30.f;                     // m around the camera: people
  const float DroneScanRadius = 60.f;                // drones
  const ULONGLONG ScanPeriodMs = 1000;
  const ULONGLONG ListWantedMs = 2000;               // the list is kept up this long after the menu last showed it
  const ULONGLONG CheckLogMs = 10000;
  // A held bone counts as the game's taking the pose when its world pose is
  // its parent's times the local handed, this near
  const float TakenDistance = 0.01f;
  const float TakenDot = 0.9995f;
  const size_t MaxUndo = 50;
  const ULONGLONG AutosaveMs = 30000;
  const char* const PoseFolder = "Cinematic Tools\\Poses";

  // The game's code this relies on, byte for byte (Faith's size checks the
  // same)
  struct CodeCheck
  {
    __int64 Rva;
    const char* What;
    BYTE Bytes[28];
    size_t Size;
  };
  const CodeCheck CodeChecks[] = {
    // The override's blend: its 4th translation float broadcast into the
    // bone's scale (movss xmm1, [rbx + rbp + 0xC] .. movaps [r15 + rsi*8], xmm4)
    { 0x2FCFA5A, "the override's scale write", { 0xF3, 0x0F, 0x10, 0x4C, 0x2B, 0x0C, 0x0F, 0x28, 0xD9, 0x0F, 0x28, 0xE1,
      0x0F, 0x14, 0xE1, 0x0F, 0x14, 0xD9, 0x0F, 0x14, 0xE3, 0x41, 0x0F, 0x29, 0x24, 0xF7 }, 26 },
    // The pose job's copy of the slot's override (buffer, list, map, weight,
    // count, handle) into the job
    { 0x2FD7D7A, "the override's copy", { 0x49, 0x8B, 0x0C, 0x01, 0x48, 0x85, 0xC9, 0x74, 0x3A, 0x0F, 0x10, 0x01, 0x41,
      0x0F, 0x11, 0x80, 0x30, 0x01, 0x00, 0x00 }, 20 },
    // The blend skips an override whose handle is -1
    { 0x2FCF62D, "the override's handle check", { 0x83, 0xBA, 0x50, 0x01, 0x00, 0x00, 0xFF, 0x4D, 0x89, 0xC5, 0x48,
      0x89, 0xD7, 0x0F, 0x84, 0xB2, 0x04, 0x00, 0x00 }, 19 } };

  // The game's override, laid out like the ghost player component's (+0x1D8),
  // the replay's and Faith's size's
  struct GameVector
  {
    __int64 Allocator;
    void* Data;
    unsigned int Count;
    unsigned int Capacity;
  };
  struct PoseOverride
  {
    float* Bones;           // 0x20 per listed bone, in the list's order
    GameVector* List;       // the skeleton's bones it holds
    GameVector* Map;        // per skeleton bone, its place in Bones (-1 for none)
    float Weight;
    unsigned int Count;
    int Handle;             // the slot it's for (the game sets -1 when it releases the slot)
    int Unused;
  };
  static_assert(offsetof(PoseOverride, Weight) == 0x18 && offsetof(PoseOverride, Count) == 0x1C &&
    offsetof(PoseOverride, Handle) == 0x20, "the game's layout");

  // ---- Small maths: quaternions x y z w, a bone as translation x y z, its
  // 4th float, then a quaternion (the game's 0x20 layout) ----

  // a * b (as FaithReplay.cpp's QuatMul)
  void QMul(float const* a, float const* b, float* out)
  {
    float x = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1];
    float y = a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0];
    float z = a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3];
    float w = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
    out[0] = x; out[1] = y; out[2] = z; out[3] = w;
  }

  // v turned by q
  void QRot(float const* q, float const* v, float* out)
  {
    float c1[3] = { q[1] * v[2] - q[2] * v[1], q[2] * v[0] - q[0] * v[2], q[0] * v[1] - q[1] * v[0] };
    float c2[3] = { q[1] * c1[2] - q[2] * c1[1], q[2] * c1[0] - q[0] * c1[2], q[0] * c1[1] - q[1] * c1[0] };
    for (int k = 0; k < 3; ++k)
      out[k] = v[k] + 2.f * q[3] * c1[k] + 2.f * c2[k];
  }

  void QConj(float const* q, float* out)
  {
    out[0] = -q[0]; out[1] = -q[1]; out[2] = -q[2]; out[3] = q[3];
  }

  // Unit length; the identity for one that isn't a rotation
  void QNorm(float* q)
  {
    float length = sqrtf(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    if (!(length > 1e-6f) || !std::isfinite(length))
    {
      q[0] = q[1] = q[2] = 0.f;
      q[3] = 1.f;
      return;
    }
    for (int k = 0; k < 4; ++k)
      q[k] /= length;
  }

  void QIdentity(float* q)
  {
    q[0] = q[1] = q[2] = 0.f;
    q[3] = 1.f;
  }

  bool QIsIdentity(float const* q)
  {
    return fabsf(q[0]) < 1e-6f && fabsf(q[1]) < 1e-6f && fabsf(q[2]) < 1e-6f;
  }

  // A turn of angle (radians) about a unit axis
  void QAxis(float const* axis, float angle, float* out)
  {
    float s = sinf(angle * 0.5f);
    out[0] = axis[0] * s; out[1] = axis[1] * s; out[2] = axis[2] * s;
    out[3] = cosf(angle * 0.5f);
  }

  // Turns about the parent's X, then Y, then Z (degrees): qz * qy * qx
  void QFromAngles(float const* degrees, float* out)
  {
    const float toRad = 3.14159265f / 180.f;
    float x[4], y[4], z[4], zy[4];
    const float ax[3] = { 1.f, 0.f, 0.f }, ay[3] = { 0.f, 1.f, 0.f }, az[3] = { 0.f, 0.f, 1.f };
    QAxis(ax, degrees[0] * toRad, x);
    QAxis(ay, degrees[1] * toRad, y);
    QAxis(az, degrees[2] * toRad, z);
    QMul(z, y, zy);
    QMul(zy, x, out);
    QNorm(out);
  }

  // The angles QFromAngles makes q from
  void QToAngles(float const* q, float* degrees)
  {
    const float toDeg = 180.f / 3.14159265f;
    float x = q[0], y = q[1], z = q[2], w = q[3];
    float sinp = 2.f * (w * y - z * x);
    sinp = sinp > 1.f ? 1.f : (sinp < -1.f ? -1.f : sinp);
    degrees[0] = atan2f(2.f * (w * x + y * z), 1.f - 2.f * (x * x + y * y)) * toDeg;
    degrees[1] = asinf(sinp) * toDeg;
    degrees[2] = atan2f(2.f * (w * z + x * y), 1.f - 2.f * (y * y + z * z)) * toDeg;
  }

  // A bone's world pose from its parent's and its own local pose (the way
  // the game builds it, 0x142FCFC30: the 4th float is 1)
  void Compose(float const* parent, float const* local, float* out)
  {
    float t[3];
    QRot(parent + 4, local, t);
    float q[4];
    QMul(parent + 4, local + 4, q);
    out[0] = parent[0] + t[0];
    out[1] = parent[1] + t[1];
    out[2] = parent[2] + t[2];
    out[3] = 1.f;
    memcpy(out + 4, q, sizeof(q));
  }

  float Distance3(float const* a, float const* b)
  {
    float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
    return sqrtf(dx * dx + dy * dy + dz * dz);
  }

  void Cross(float const* a, float const* b, float* out)
  {
    out[0] = a[1] * b[2] - a[2] * b[1];
    out[1] = a[2] * b[0] - a[0] * b[2];
    out[2] = a[0] * b[1] - a[1] * b[0];
  }

  void Normalize3(float* v)
  {
    float length = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (length > 1e-6f)
      for (int k = 0; k < 3; ++k)
        v[k] /= length;
  }

  // ---- The game's memory (read only but the override table), all in __try ----

  // What an object's getType gives (vtable [0]: lea rax, [rip + x]; ret).
  // Inside the caller's __try.
  __int64 TypeOf(__int64 object)
  {
    __int64 vtable = *(__int64*)object;
    BYTE const* code = vtable ? *(BYTE const**)vtable : nullptr;
    if (!code || code[0] != 0x48 || code[1] != 0x8D || code[2] != 0x05)
      return 0;
    return (__int64)code + 7 + *(int const*)(code + 3);
  }

  bool TypeNameIs(__int64 typeInfo, char const* name)
  {
    __try
    {
      char const* s = *(char const**)*(__int64*)typeInfo;
      return s && strcmp(s, name) == 0;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      return false;
    }
  }

  // Where an entity is (its components' transform, as the replay reads it)
  bool EntityPlace(__int64 entity, float* place)
  {
    __try
    {
      BYTE const* components = *(BYTE const**)(entity + 0x38);
      if ((__int64)components < 0x10000)
        return false;
      memcpy(place, components + 0x40 + ((__int64)components[9] + 2 * components[0xA]) * 0x20, 3 * sizeof(float));
      return std::isfinite(place[0]) && std::isfinite(place[1]) && std::isfinite(place[2]);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      return false;
    }
  }

  struct LiveSlot
  {
    __int64 Manager;
    unsigned int Handle;
    int Bones;
    float* Local;
    float* World;
    __int64* Overrides;     // the page's override table
    unsigned int const* Parents;
  };

  // An animatable's pose slot, checked all the way; manager 0: whichever its
  // pose has. Inside the caller's __try.
  bool SlotOf(__int64 animatable, __int64 manager, LiveSlot& slot)
  {
    __int64 base = (__int64)g_gameHandle;
    if (animatable < 0x10000 || TypeOf(animatable) != base + AnimatableTypeInfoRva)
      return false;
    __int64 pose = *(__int64*)(animatable + AnimatablePoseOffset);
    if (pose < 0x10000)
      return false;
    __int64 m = *(__int64*)(pose + PoseManagerOffset);
    if (m < 0x10000 || (manager && m != manager))
      return false;
    unsigned int handle = *(unsigned int*)(pose + PoseHandleOffset);
    int bones = *(int*)(m + ManagerBoneCountOffset);
    int stride = *(int*)(m + ManagerPoseStrideOffset);
    if (handle == 0xFFFFFFFF || bones < 8 || bones > MaxBones || stride != bones * FloatsPerBone * (int)sizeof(float))
      return false;
    unsigned int const* parents = *(unsigned int const**)(m + ManagerParentsOffset);
    if ((__int64)parents < 0x10000)
      return false;
    __int64* pagesBegin = *(__int64**)(m + ManagerPagesOffset);
    __int64* pagesEnd = *(__int64**)(m + ManagerPagesOffset + 8);
    unsigned int page = handle >> 24, index = handle & 0xFF;
    if (!pagesBegin || pagesEnd < pagesBegin || page >= (unsigned int)(pagesEnd - pagesBegin))
      return false;
    __int64 pPage = pagesBegin[page];
    if (pPage < 0x10000)
      return false;
    __int64 local = *(__int64*)(pPage + PageLocalPoseOffset);
    __int64 world = *(__int64*)(pPage + PageWorldPoseOffset);
    __int64 overrides = *(__int64*)(pPage + PageOverridesOffset);
    if (local < 0x10000 || world < 0x10000 || overrides < 0x10000)
      return false;
    slot.Manager = m;
    slot.Handle = handle;
    slot.Bones = bones;
    slot.Local = (float*)(local + (__int64)index * stride);
    slot.World = (float*)(world + (__int64)index * stride);
    slot.Overrides = (__int64*)overrides;
    slot.Parents = parents;
    return true;
  }

  // Faith's body and box pose
  bool FaithSlots(__int64 soldier, __int64& animatable, LiveSlot& body, __int64& boxAnimatable, LiveSlot& box, bool& boxOk)
  {
    __int64 base = (__int64)g_gameHandle;
    boxOk = false;
    __try
    {
      if (soldier < 0x10000)
        return false;
      __int64 component = *(__int64*)(soldier + SoldierRecordingComponentOffset);
      if (component < 0x10000 || *(__int64*)component != base + RecordingComponentVTableRva)
        return false;
      __int64 manager = *(__int64*)(component + ComponentManagerOffset);
      animatable = *(__int64*)(component + ComponentAnimatableOffset);
      if (manager < 0x10000 || !SlotOf(animatable, manager, body))
        return false;
      boxAnimatable = *(__int64*)(soldier + SoldierBoxAnimatableOffset);
      boxOk = SlotOf(boxAnimatable, manager, box) && box.Handle != body.Handle && box.Bones == body.Bones;
      return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      return false;
    }
  }

  // A character's or actor's body at its offset, and its slot
  bool BodySlot(__int64 entity, int bodyOffset, __int64& animatable, LiveSlot& slot)
  {
    __try
    {
      if (entity < 0x10000)
        return false;
      animatable = *(__int64*)(entity + bodyOffset);
      return SlotOf(animatable, 0, slot);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      return false;
    }
  }

  bool StillThere(__int64 entity, __int64 vtable, int bodyOffset, __int64 animatable)
  {
    __try
    {
      return *(__int64*)entity == vtable && *(__int64*)(entity + bodyOffset) == animatable;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      return false;
    }
  }

  __int64 ReadVTable(__int64 entity)
  {
    __try
    {
      return *(__int64*)entity;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      return 0;
    }
  }

  // 1: the game animates the body, 0: it doesn't, -1: not readable
  int AnimatedByGame(__int64 animatable)
  {
    __try
    {
      BYTE const* body = (BYTE const*)(animatable + BodyUpdateOffset);
      int always = body[BodyAlwaysUpdateOffset], external = body[BodyExternalOffset];
      __int64 mode = *(__int64 const*)(body + BodyUpdateModeOffset);
      int m = mode >= 0x10000 ? *(int const*)(mode + 0x10) : -1;
      if (m == 4)
        return 0;
      return (external || (always && m != 1 && m != 3)) ? 1 : 0;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      return -1;
    }
  }

  // The slot's override now (-1: not readable)
  __int64 OverrideIn(LiveSlot const& slot)
  {
    __try
    {
      return *(__int64 volatile*)&slot.Overrides[slot.Handle & 0xFF];
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      return -1;
    }
  }

  bool CopyFrom(float const* source, float* target, size_t floats)
  {
    __try
    {
      memcpy(target, source, floats * sizeof(float));
      return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      return false;
    }
  }

  // ---- A body's skeleton (pose_skeleton_live: read from the game itself, see PoseSkeletonLive.h) ----

  // Why a body whose skeleton is known still can't be posed
  enum Cant { Cant_No, Cant_Unknown, Cant_NoRoot, Cant_RootUnanimated };

  // The body last read (ScanCandidates and Take: the game's thread, under g_Mutex)
  pose_skeleton_live::Body g_Body;
  // The pose managers whose skeleton was written to the log (once each; the last 64)
  __int64 g_LoggedManagers[64];
  int g_LoggedAs[64];                        // what was written of each: its skeleton and bone count
  int g_LoggedManagerCount = 0;

  // A skeleton whose movable bones sit side by side (an animated prop's, a rope's): there is no one bone to hold
  bool HasSeveralTops(int skeleton)
  {
    for (pose_skeletons::TopJoints const& tops : pose_skeletons::SeveralTopJoints)
      if (tops.Skeleton == skeleton)
        return true;
    return false;
  }

  // What was read of a pose manager's skeleton, into the log, once a manager: the asset's name as the game has it
  // (or which check kept it from being taken), the skeleton it was found as, and the bones its rig lacks. A body
  // that stays unknown gets its whole parent list written, so the next one explains itself.
  void LogSkeleton(__int64 manager, pose_skeleton_live::Body const& body, int skeleton)
  {
    // (a known skeleton's lines are research: only with [Debug] Diagnostics on; an unknown one's are always written,
    // the Pose menu's "skeleton not known" points to them)
    if (skeleton >= 0 && !util::DiagnosticsOn())
      return;
    // (written again when the same address reads as another skeleton: after a level load a new manager can be
    // made where an old one was)
    int as = skeleton + (body.Bones << 16);
    int kept = g_LoggedManagerCount < 64 ? g_LoggedManagerCount : 64, at = -1;
    for (int k = 0; k < kept && at < 0; ++k)
      if (g_LoggedManagers[k] == manager)
        at = k;
    if (at >= 0 && g_LoggedAs[at] == as)
      return;
    if (at < 0)
    {
      at = g_LoggedManagerCount++ % 64;
      if (g_LoggedManagerCount >= 128)
        g_LoggedManagerCount = 64;
      g_LoggedManagers[at] = manager;
    }
    g_LoggedAs[at] = as;
    char const* found = skeleton >= 0 ? pose_skeleton_live::At(skeleton).Label : "NOT KNOWN";
    char asset[pose_skeleton_live::AssetBytes + 96];
    if (body.AssetOk)
      _snprintf_s(asset, sizeof(asset), _TRUNCATE, "the game's asset %s, its %d bone names proven by the game's own "
        "hashes%s", body.Asset, body.Bones, body.DataAgrees == 1 ? ", the body's data names the same asset" :
        (body.DataAgrees == 0 ? ", the body's data names ANOTHER asset" : ""));
    else
      _snprintf_s(asset, sizeof(asset), _TRUNCATE, "its asset wasn't taken (%s)", body.Why[0] ? body.Why : "not read");
    std::string lacking;
    int listed = 0;
    for (int i = 1; body.ParentsOk && i < body.Bones; ++i)
      if (body.Parents[i] == -1 && listed < 12)
      {
        char one[pose_skeleton_live::NameBytes + 16];
        char const* name = skeleton >= 0 ? pose_skeleton_live::At(skeleton).Names[i] : (body.AssetOk ? body.Names[i] : "");
        _snprintf_s(one, sizeof(one), _TRUNCATE, "%s%d %s", listed ? ", " : ": ", i, name);
        lacking += one;
        ++listed;
      }
    util::log::Write("Pose: a body's skeleton, %d bones (pose manager 0x%I64X): %s; found as: %s; bones with no parent "
      "in its pose manager (its rig has no channel for them): %d%s%s; rig channels %s", body.Bones, manager, asset, found,
      body.Unparented, lacking.c_str(), body.Unparented > listed ? ", ..." : "", body.ChannelsOk ?
      std::to_string(body.Channels).c_str() : "not read");
    if (skeleton >= 0 || !body.ParentsOk)
      return;
    // Not known: its parents, 32 a line
    for (int from = 0; from < body.Bones; from += 32)
    {
      std::string line;
      for (int i = from; i < body.Bones && i < from + 32; ++i)
        line += (i > from ? " " : "") + std::to_string((int)body.Parents[i]);
      util::log::Write("Pose:   its bones' parents from bone %d: %s", from, line.c_str());
    }
  }

  // A body's skeleton: its index for pose_skeleton_live::At(), or -1 (not known). cant: why it can't be posed
  // although it's known (Cant_No: it can). body: what was read of it.
  int SkeletonOf(__int64 animatable, LiveSlot const& slot, pose_skeleton_live::Body& body, int& cant)
  {
    cant = Cant_Unknown;
    bool read = pose_skeleton_live::Read((__int64)g_gameHandle, animatable, slot.Manager, slot.Bones, body);
    int skeleton = read ? pose_skeleton_live::Find(body) : -1;
    LogSkeleton(slot.Manager, body, skeleton);
    if (skeleton < 0)
      return -1;
    pose_skeletons::Skeleton const& s = pose_skeleton_live::At(skeleton);
    if (s.HeldRoot <= 0 || s.HeldRoot >= s.Bones || HasSeveralTops(skeleton))
      cant = Cant_NoRoot;
    else if (!pose_skeleton_live::MayPose(body, s.HeldRoot))
      cant = Cant_RootUnanimated;
    else
      cant = Cant_No;
    return skeleton;
  }

  // ---- The main view, for the dots (read after the free camera is written,
  // as the lights' dots do) ----

  struct View
  {
    float T[16];
    float P[16];
    bool Ok;
  };
  std::mutex g_ViewMutex;
  View g_View{};
  std::atomic<bool> g_LookCameraWanted{ false };   // a held pose looks at the camera (Update; OnViewWritten reads it)
  __int64 g_ppGameRenderer = 0;

  bool ReadMainView(float* transform, float* projection)
  {
    __try
    {
      __int64 pGameRenderer = *(__int64*)g_ppGameRenderer;
      if (!pGameRenderer)
        return false;
      __int64 pRenderView = *(__int64*)(pGameRenderer + RenderViewOffset);
      if (!pRenderView)
        return false;
      memcpy(transform, (void const*)(pRenderView + RenderViewTransform), 16 * sizeof(float));
      memcpy(projection, (void const*)(pRenderView + RenderViewProjection), 16 * sizeof(float));
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      return false;
    }
    return projection[0] != 0.f && projection[5] != 0.f &&
      fabsf(transform[12]) + fabsf(transform[13]) + fabsf(transform[14]) > 2.f;
  }

  // Where a point is on the screen, in pixels (Panini-aware, as the lights'
  // dots); false behind the camera
  bool ToScreen(View const& v, float const* point, float& x, float& y)
  {
    float const* t = v.T;
    float offset[3] = { point[0] - t[12], point[1] - t[13], point[2] - t[14] };
    float right = offset[0] * t[0] + offset[1] * t[1] + offset[2] * t[2];
    float up = offset[0] * t[4] + offset[1] * t[5] + offset[2] * t[6];
    float depth = -(offset[0] * t[8] + offset[1] * t[9] + offset[2] * t[10]);
    if (depth < 0.1f)
      return false;
    float px = right / depth, py = up / depth;
    panini::ToPanini(px, py);
    float scale = panini::ScreenScale();
    float nx = px * v.P[0] / scale;
    float ny = py * v.P[5] / scale;
    ImVec2 size = ImGui::GetIO().DisplaySize;
    x = (nx * 0.5f + 0.5f) * size.x;
    y = (0.5f - ny * 0.5f) * size.y;
    return true;
  }

  // ---- Who can be posed ----

  enum Kind { Kind_Faith, Kind_Person, Kind_Drone };

  struct Candidate
  {
    __int64 Entity;
    __int64 VTable;
    __int64 Animatable;
    int Kind;
    float Distance;
    int Bones;
    int Skeleton;           // for pose_skeleton_live::At(), -1 not known
    int Cant;               // why it can't be posed though its skeleton is known (Cant_No: it can)
    int Animated;           // AnimatedByGame
    bool OtherOverride;     // its slot holds an override not the tools'
    bool Held;
    bool LettingGo;         // held, being let go (its bones put back at the game's next pass)
  };

  // ---- A held pose ----

  struct Snapshot
  {
    std::vector<float> Turns;   // Bones x 4
    std::vector<float> Moves;   // Bones x 3
    std::vector<float> Weapons; // per weapon WeaponSnapshotFloats: its frame (a bone), offsets and edit
    double KeysTime = 0.0;      // the keyframes playhead when it was taken
    std::vector<float> Pins;    // per limb PinSnapshotFloats (2026-11-16): put back for the pins that are on (Undo, a
                                // drag starting again from its press each frame); whether one is on stays as it is
  };
  const int WeaponSnapshotFloats = 1 + 8 + 8 + 3 + 4;

  // A step of the pose's Undo / Redo: the pose and its keyframes as they were (2026-11-15: one history for both),
  // and each bone's keyframe reference then (KeyRefTurn / KeyRefMove: which changes weren't keyframed yet)
  struct HistoryStep
  {
    Snapshot Pose;
    std::vector<pose_keys::Channel> Keys;
    double Length = 5.0;
    std::vector<float> RefTurns, RefMoves;
    // The keyframes' settings then; put back only by a step with Settings (the one a keyframes file's load pushes:
    // the load replaces them; the ticks and the clock push no step, so another step's Undo leaves them be)
    bool Settings = false;
    bool On = true, Loop = false;
    float Speed = 1.f;
    int Clock = 0;
    double Start = 0.0, Time = 0.0;
  };

  // The face's bones in groups (GAME > Pose's "Group the face"), by their
  // names under the Face bone: the same 57 face bones in Faith's, KrugerSec's,
  // the men's and the women's skeletons (2026-11-08, their SkeletonAssets).
  // Face, Jaw, Tongue, TongueTip and Chin stay single; a grouped bone's
  // children (the inner lips, the irises, the mouth's inner corners and
  // creases) go with it.
  const int FaceGroupCount = 9;
  const int EyesGroup = 0;                   // FaceGroupNames' "Eyes"
  // Dragging the eyes: they look at a point LookDistance in front of them,
  // moved LookGain times as far as the cursor went at their distance
  const float LookDistance = 0.5f;
  const float LookGain = 2.f;
  const int MaxGroupMembers = 16;
  const char* const FaceGroupNames[FaceGroupCount] = { "Eyes", "Upper eyelids", "Lower eyelids", "Eyebrows", "Upper lip",
    "Lower lip", "Mouth corners", "Cheeks", "Nose" };

  // A helper bone (roll, twist, bend, bulge, arm pit, collar, holder, ankle,
  // throat, the end bones, the gun grip): it rides on a real joint, so its
  // dot is hidden unless asked for (they sat on top of the arms' joints)
  bool IsHelperName(char const* n)
  {
    static char const* const parts[] = { "Roll", "Twist", "Bend", "Bulge", "ArmPit", "Collar", "Holder", "Ankle" };
    for (char const* part : parts)
      if (strstr(n, part))
        return true;
    size_t length = strlen(n);
    return !strcmp(n, "Wep_Root") || !strcmp(n, "Throat") || (length > 3 && !strcmp(n + length - 3, "End"));
  }

  int FaceGroupOf(char const* n)
  {
    auto is = [n](char const* s) { return strcmp(n, s) == 0; };
    if (is("LeftEye") || is("RightEye"))
      return 0;
    if (is("LeftUpEyelid") || is("RightUpEyelid"))
      return 1;
    if (is("LeftLowEyelid") || is("RightLowEyelid") || is("LeftLowEyelidCrease") || is("RightLowEyelidCrease"))
      return 2;
    if (strstr(n, "Eyebrow"))
      return 3;
    if (is("LeftUpperLip") || is("UpperLip") || is("RightUpperLip"))
      return 4;
    if (is("LeftLowerLip") || is("LowerLip") || is("RightLowerLip"))
      return 5;
    if (is("LeftMouth") || is("RightMouth"))
      return 6;
    if (strstr(n, "Cheek") || is("LeftCrease") || is("RightCrease"))
      return 7;
    if (is("LeftNose") || is("RightNose") || is("NoseTip") || is("LowNose"))
      return 8;
    return -1;
  }

  // A character's weapon (PamClientCharacterWeaponEntity: KrugerSec's guns
  // and batons, 2026-11-08 live: cpf_shotgun_Mesh, cpf_baton_Mesh). It's
  // drawn where its pose's bone 1 is, and that is its entity's transform (in
  // its local pose; its world pose's bone 1 has the same place, its rotation a
  // quarter turn off, as a character's). The game moves the entity with the
  // hand (the replay's finding), but not with the posed hand: which hand it
  // takes isn't known (live, frozen: a posed KrugerSec's shotgun 0.34 m and
  // another's baton 1.03 m from the posed grips).
  // So while posed, the tools move the entity with the bone it was on and
  // hold the game's own moves back, as the replay does for its guns
  // (util::hooks::MoveActorDrawn / HoldActor), and give its body's placement
  // the same place (util::hooks::PlaceBody: used by the game only for a body
  // with its placement on, + 0xB8: live, the shotgun's was, the baton's not).
  const int MaxWeapons = 4;
  const float GripReach = 0.25f;             // m: a weapon this near the grip (Wep_Root) when taken is in that hand
  const float CarryReach = 0.4f;             // m: else it goes with the nearest body bone this near, or stays the game's
  const int WeaponDataOffset = 0x30;         // the entity's data
  const int WeaponDataMeshOffset = 0x90;     // its Mesh (MeshAsset: + 0x10 its name)
  const int BodyPlacementOnOffset = 0xB8;    // a body (animatable + 0x70): its placement on (0x1432E06B0)
  const int WorldFrame = -2;                 // a weapon's Bone: put down, where it is in the world
  const int MaxWeaponBones = 64;             // a weapon's skeleton (26 bones, 2026-11-08 live)
  struct HeldWeapon
  {
    __int64 Entity, VTable, Animatable;
    // The frame it goes with: a bone, WorldFrame (put down), or -1 (left
    // where the game has it). Its offsets are in that frame; its edit (a
    // move in that frame, a turn about its own place) is on top of them.
    int Bone;
    float OffsetLocal[FloatsPerBone];        // its local pose's bone 1 (its entity's transform) in that frame
    float OffsetWorld[FloatsPerBone];        // its world pose's bone 1 (its body's placement) in that frame
    float EditMove[3];
    float EditTurn[4];
    int TakenBone;                           // as taken (Reset goes back to it)
    float TakenOffsetLocal[FloatsPerBone], TakenOffsetWorld[FloatsPerBone];
    char Name[48];
    bool Holding;                            // its entity moved by the tools, the game's own moves held back
    bool Placing;                            // its body's placement given
    // Its body drawn by the tools (see KeepWeapons): its slot buffers written
    // each frame, these as they were when the tools took it (put back when
    // they let go)
    bool Drawing;
    __int64 Manager;
    unsigned int Handle;
    int Bones;
    float Local0[MaxWeaponBones][FloatsPerBone];
    float World0[MaxWeaponBones][FloatsPerBone];
    float Matrices0[MaxWeaponBones][12];
    float Drawn0[12], Step0[12];             // bone 1's records in the two animation steps (page + 0x18, + 0x20)
    float WrittenMatrix[3];                  // the matrix of bone 1's place written last frame
    float MatrixGap;                         // m: how far the game moved that since (its jobs ran after the tools)
    // The world pose and matrices written last frame: when the slot's differ,
    // the game rebuilt the weapon's pose (time running: its own animation), and
    // the kept pose is taken afresh from it (Rebased)
    float LastWorld[MaxWeaponBones][FloatsPerBone];
    float LastMatrices[MaxWeaponBones][12];
    bool Rebased;
    float FirstLocal1[FloatsPerBone];        // its local bone 1 when the tools first took its body (to put it back there)
    bool FirstLocal1Ok;
    bool TimeRan;                            // time ran while the tools had it: what was kept isn't where the game has it now
    bool Gone;                               // it stopped being the character's (dropped or gone): the game's for good
    // Its body's pose override (the game's own, as the characters' poses
    // use): bone 1 where it's wanted, weight 1, so a pose job of the game's
    // that rebuilds the weapon after the tools' write (time running: Ant's
    // output, override applied, copied over the step and rebuilt into the
    // local and world pose and the matrices, 0x142FCC7C0) rebuilds it there.
    // Two in turn, one filled while the game can still read the other.
    alignas(16) float OverBuffer[2][FloatsPerBone];
    PoseOverride Over[2];
    GameVector OverListVector, OverMapVector;
    unsigned int OverList[1];
    unsigned int OverMap[MaxWeaponBones];
    bool OverReady;
    int OverNext;
    __int64* OverTable;                      // where it was handed (0: not now)
    unsigned int OverTableHandle;
    bool OverRefused;
    float Rows0[16];                         // its entity's rows before the tools first moved it
    bool Rows0Ok;
    float Written[FloatsPerBone];            // the local bone 1 written last frame
    bool WrittenOk;
    float Gap;                               // m: how far the game moved it from the tools' place since the last frame
    bool GapOk;
    int MoveFailures;
    int DrawFailures;
  };

  // An animation clip on a held pose (GAME > Pose > Animation clip;
  // PoseClips.h). While it's on, its pose takes the place of the pose taken
  // (Base): each frame the bones it turns get the clip's world rotations,
  // faced the way the pose taken's Hips faced when the clip was put on; the
  // elbow and knee rolls follow the forearm and shin, Faith's twist bones
  // take their fitted share, Neck1 and the finger bases take the game rest's
  // local, the other helpers (and the face) keep the pose taken's; and the
  // Hips move as the clip's do (scaled to the skeleton). The user's edits
  // (Turn, Move) stay on top of it.
  enum ClipClock { ClipGameTime, ClipRealTime, ClipTrackTime };
  enum ClipHips { ClipStay, ClipTravel };
  struct ClipPlay
  {
    std::shared_ptr<pose_clips::Clip const> Clip;   // null: none
    pose_clips::Fit Fit;                     // the clip on this skeleton
    bool On = false;                         // its pose is the base
    bool Playing = false;
    bool Loop = true;
    double Time = 0.0;                       // seconds into the clip
    float Speed = 1.f;
    int Clock = ClipGameTime;
    double TrackStart = 0.0;                 // the camera track's clock: the clip's time at the track's 0 s
    int Hips = ClipStay;
    float Facing[4] = { 0.f, 0.f, 0.f, 1.f };  // the clip's front turned onto the pose taken's Hips' (bone 1's frame)
    float Anchor[2] = { 0.f, 0.f };          // the Hips across the ground (x, z) when the clip was put on
    // The travel's way: the heading last made (radians about up) and the shift
    // that keeps the Hips where they are when it changes (the Hips' turn)
    float TravelYaw = 0.f;
    float TravelShift[2] = { 0.f, 0.f };
    bool TravelYawOk = false;
    float Local[MaxBones][FloatsPerBone];    // its locals now (see above for each kind of bone)
    bool LocalOk = false;
  };

  // A voice on a held pose (the Pose window's Voice tab; the user, 2026-11-12: "put a voice file (mp3/wav etc.) and
  // sync it to faiths mouth or ksec mouth"): a sound file's mouth shapes (voice::Sound, worked out from the sound when
  // it's read) laid over the mouth's bones as one more layer on the handed pose, after the edits, the keyframes and
  // the idle movement (voice_face: the jaw's turn, the lips' and the corners' moves), and the sound itself played in
  // step with the voice's time (voice_player). The edits and keyframes of those bones stay under it.
  enum VoiceClock { VoiceOwnTime, VoiceKeysTime, VoiceTrackTime };
  struct VoicePlay
  {
    std::shared_ptr<voice::Sound const> Sound;   // null: none
    voice_face::Rig Rig;                     // the mouth's bones in this pose (Ok false: it has none; the sound still plays)
    bool On = false;                         // the mouth follows the voice and the sound is played
    bool Playing = false;                    // its own clock runs
    bool Loop = false;
    double Time = 0.0;                       // seconds into the sound
    int Clock = VoiceOwnTime;
    double Start = 0.0;                      // the keyframes' / the camera track's clock: that clock's time at the voice's start
    bool Heard = true;                       // the sound is played (off: only the mouth moves)
    float Volume = 1.f;                      // 0 - 2
    // How much of the shapes (voice_face::Amounts), 0 - 2. The jaw is what the sound gives best; the lips' shapes are
    // soft hints from it (measured on test voices, Research_Pose_Editor section 28), so they start at half
    float Jaw = 1.f, Lips = 0.5f, Closure = 1.f;
    // How smoothly the mouth goes from shape to shape (voice::MouthAt): 1 eased (the default since 2026-11-14, after
    // the user: "i think mouth animations should be smoothed out they are too harsh right now"), 0 as before, 2 softer
    float Smoothing = 1.f;
    // The Voice tab's picture of the file at another Mouth smoothing than 1 (DrawVoicePicture, under g_Mutex): made
    // again when the smoothing or the sound changes (at 1 the sound's own Overview is drawn)
    std::vector<float> Overview;
    float OverviewSmoothing = 1.f;
    std::weak_ptr<voice::Sound const> OverviewSound;
    // The life around the mouth (the user, 2026-11-12, of seven suggestions: "Let's do number 4 for now": blinks,
    // brows that lift on stressed words, small head nods, all taken from loudness and pitch): voice::Sound's Life
    // curves laid on the eyelids, the eyebrows and the Head bone as more of the same layer (voice_face::LifeRig)
    // Which of its parts this body has, found when the voice is put on: a part is on only if ALL its bones are held
    // (half a part would be a wink, a lopsided brow). The moves themselves are made each frame from the pose as it
    // is then (KeepVoice).
    voice_face::LifeRig LifeRig{};
    bool LifeEyesNow = false, LifeBrowsNow = false, LifeHeadNow = false;   // the parts moved this frame (KeepVoice)
    bool LifeTried = false;                  // this frame's pose was looked at for them (the menu: a part that stops)
    float LifeBlinkTaken = 1.f;              // the share of the blink the lids take this frame (less: already turned shut)
    bool Life = true;
    float Blinks = 1.f, Brows = 1.f, Head = 1.f;   // amounts: 0 none, 1 as made, up to 2 (a blink never past shut)
    voice_face::LifeShape LifeShape{};       // this frame's (the menu shows it)
    // [ALIVE] A living face (the user, 2026-11-13, after trying the blinks, brows and head: "it works but still needs
    // more human expression now it looks more like a animatronic. We need more feeling"): VoiceAlive's eyes (looks
    // away, small eye movements), each eye's own varied blinks, each side's brows, breathing and a head that never
    // stops, worked out for this character from the sound's analysis (a take of its own); VoiceGaze's eyes held
    // against the head; the feeling the user picks, made of the game's own face poses (VoiceFeel, VoiceFeelFace).
    std::shared_ptr<voice_alive::Track const> Alive;   // this character's take (null: not made yet, or it couldn't be)
    int AliveTake = 0;                       // the take and the looks away it was made with
    float AliveLookAway = -1.f;
    std::string AliveFound;                  // the menu's line about it ("12 blinks, 8 looks away, ...")
    ULONGLONG AliveWantedAt = 0;             // when the menu last changed the take or the looks away (made once it rests)
    int Take = 1;                            // 1 - 99: another take of the same line (other blinks, glances, drift)
    float LookAway = 1.f;                    // 0 - 2: how much the eyes look away while speaking
    float Eyes = 1.f;                        // 0 - 1: how far the eyes move with the voice
    bool HoldGaze = true;                    // the eyes hold their target while the voice moves the head
    bool EyesNow = false;                    // the eyes were turned this frame (KeepVoice; the menu)
    float GazeNow[2] = { 0.f, 0.f };         // the gaze given this frame against the head as posed (degrees: right, down)
    int Feel = -1;                           // the feeling picked: -1 none, else a voice_feel::Feeling
    float FeelStrength = 1.f;                // 0 - 2
    float FeelFollow = 0.3f;                 // 0 - 1: how much it follows the voice (weaker in pauses, stronger on stress)
    float FeelUneven = 0.f;                  // 0 - 0.3: one side stronger
    bool FeelLeftStronger = false;
    // [SMILE] 0 - 1: how much the feeling's smile comes and goes with the speech (0: held all through, as before
    // 2026-11-16), and when it does (from the voice's phrases, made when the voice is put on)
    float FeelComes = 1.f;
    voice_feel::SmileTimes SmileTimes;
    float FeelSmileNow = 1.f;                // this frame: the smile's share shown (the menu)
    float FeelJawNow = 0.f;                  // [JAW] this frame: the feeling's jaw laid (the menu)
    bool FeelJawOn = false;                  // this frame: the feeling has a jaw at all (the menu's line, also at 0)
    // [EMOTION] The feeling read from the voice itself (voice_emotion; the user, 2026-11-13: "emotions should apply
    // automatically depending on the sound file expressions"): over the feeling picked, stretch by stretch, as far as
    // the model is sure of it; each stretch can be set by hand
    bool FeelAuto = false;                   // the menu's "From the voice"
    float FeelSure = voice_emotion::SureDefault;   // the model's chance at which the voice's feeling starts to show
    std::vector<voice_emotion::Stretch> EmotionStretches;   // this voice's stretches (its phrases, long ones in parts)
    std::vector<int> FeelSetting;            // per stretch: voice_emotion::FromVoice, AsPicked, NoFeeling or a preset
    std::shared_ptr<voice_emotion::Request> Emotion;        // its reading by the helper (null: not asked yet)
    std::shared_ptr<std::vector<voice_emotion::Reading> const> EmotionReadings;   // what it heard (null: not yet)
    float FeelAutoNow = 0.f;                 // the voice's share of the feeling as shown (eased in and out)
    int FeelStretchNow = -1;                 // this frame: the stretch shown (the menu)
    float FeelVoiceShareNow = 0.f;           // this frame: the share of the face the voice's feelings (or the stretches'
                                             // own settings) have; the feeling picked has the rest (the menu)
    float FeelStretchWeightNow = 0.f;        // this frame: FeelStretchNow's weight in the mix (under 1: changing; the menu)
    // [REVIEW] The stretches' looks as shown: eased towards what they are now over FeelChangeSeconds (a stretch's own
    // setting or the sureness changed never jumps the face in one frame), made anew for a new reading
    std::vector<voice_emotion::Look> FeelLooks;
    void const* FeelLooksFor = nullptr;      // the readings they were made for
    // The feeling as shown, moving to the one picked over FeelChangeSeconds ([0] none, [1 + f] feeling f), and the
    // strength as shown, eased to the one set: a change never jumps the face in one frame
    float FeelWeights[voice_feel::FeelingCount + 1] = { 1.f };
    float FeelStrengthNow = 1.f;
    voice_face::FeelRig FeelRig{};           // the feeling's bones in this pose, found when the voice is put on
    bool FeelNow = false;                    // the feeling's layer was laid this frame (KeepVoice; the menu)
    float FeelKeep = 1.f;                    // this frame: the share of the voice's brow raises the feeling keeps (the menu)
    float FeelNod = 0.f, FeelTilt = 0.f;     // this frame: the feeling's own nod and tilt of the head, radians (the menu)
    float Lead = 0.03f;                      // s: the mouth is this far ahead of the sound (a mouth that lags looks wrong)
    int Player = -1;                         // its sound in voice_player (-1: none)
    float Rate = 1.f;                        // how fast its time moved over the last frames (1 = real time), for the player
    float Pace = 1.f;                        // the measure Rate is taken from (KeepVoice)
    bool Moving = false;                     // its time moved by itself this frame (the sound is wanted)
    bool Paced = false;                      // a pace was taken since its time began to move on this clock (KeepVoice)
    bool WasOn = false;                      // the frame before it was on,
    int LastClock = -1;                      // and on this clock: its time then was this clock's
    ULONGLONG SetByHandAt = 0;               // when the menu last held its time (its own clock waits meanwhile)
    voice_face::Shape Shape;                 // the mouth's shape this frame (the menu shows it)
    voice_face::Offsets Layer;               // this frame's offsets (Count 0: none)
  };

  // The kinematics' rig (PoseKinematics.inl, PoseMotion.inl; 2026-11-16): each held pose's limbs, spine, neck, eyes
  // and helper bones by their names (FindKinRig, at Take)
  enum { Limb_LeftArm = 0, Limb_RightArm, Limb_LeftLeg, Limb_RightLeg, LimbCount };
  enum { Look_Off = 0, Look_Camera, Look_Point };
  const int MaxKinChain = 4, MaxKinRolls = 4, MaxKinTwists = 12, MaxKinSprung = 8;
  const unsigned char MaxLooseSteps = 7;
  struct KinLimb
  {
    int Upper = -1, Mid = -1, End = -1;      // arm: Arm, ForeArm, Hand; leg: UpLeg, Leg, Foot (-1: not found)
    int Collar = -1;                         // an arm's Shoulder (the collarbone; -1 none)
    bool Arm = false;
    float Hinge[3] = { 0.f, 0.f, 0.f };      // the elbow's or knee's hinge in the upper bone's own frame (ab x bc: unit)
    int HingeFrom = 0;                       // 0 none (straight), 1 the game's rest pose, 2 the pose taken
  };
  struct KinTwist
  {
    int Bone = -1, Driver = -1;
    unsigned char Kind = 0;                  // 0 the driver's twist about X, 1 its Euler X (z y x), 2 (x y z)
    float Share = 0.f;
  };
  struct KinRig
  {
    bool Found = false;
    KinLimb Limbs[LimbCount];
    int Spine[MaxKinChain] = {}, SpineCount = 0;   // from the Hips up
    int Neck[MaxKinChain] = {}, NeckCount = 0;
    int Head = -1;
    int Eye[2] = {}, Iris[2] = {}, EyeCount = 0;
    bool FaceOk = false;                     // the face's way (the eyes' gaze straight ahead) and the eyes' middle in the
    float FaceGaze[3] = {}, FaceMiddle[3] = {};   // head's own frame (the rest pose's, else the pose taken's): the look
    int Rolls[MaxKinRolls][2] = {}, RollCount = 0;  // (roll, its driver): an elbow's or knee's roll
    KinTwist Twists[MaxKinTwists];
    int TwistCount = 0;
    unsigned char Steps[MaxBones] = {};      // loose timing: joints above the spine's first bone (0: not delayed)
    bool UpperBody[MaxBones] = {};
    int Sprung[MaxKinSprung] = {}, SprungCount = 0;   // follow-through's bones, parents first
  };
  struct KinPin
  {
    bool On = false;
    float Place[3] = { 0.f, 0.f, 0.f };      // in the frame of the held root's parent (where the game has the character)
    float Turn[4] = { 0.f, 0.f, 0.f, 1.f };
    float Seen[3] = { 0.f, 0.f, 0.f };       // where its end was after its last hold (short of Place out of reach): moved
                                             // from there by anything else, the pin goes with it (StartHold)
  };
  const int PinSnapshotFloats = 1 + 3 + 4 + 3;   // a pin in a Snapshot: on, Place, Turn, Seen

  struct Held
  {
    bool Used;
    int Id;
    int Kind;
    __int64 Entity, VTable;
    int BodyOffset;              // people and drones: where the body is (Faith: her recording component's)
    __int64 Animatable, BoxAnimatable;
    __int64 Manager;
    unsigned int Handle, BoxHandle;
    bool HasBox;
    int Bones, Skeleton, Root;
    short Parents[MaxBones];
    unsigned char Depth[MaxBones];
    bool InHeld[MaxBones];
    bool Face[MaxBones];         // the Face bone and every bone below it (moved, not bent, when dragged)
    bool Finger[MaxBones];       // a hand's children (but Wep_Root) and below
    bool Helper[MaxBones];       // a roll, twist or other helper bone (its dot hidden unless asked for)
    // Whether the world's sweeps hit this character's own body (-1 not known
    // yet): probed at its first drag with "Bones stop at walls" (ProbeSelf)
    int SelfHit;
    bool Eye[MaxBones];          // LeftEye / RightEye: dragged, they look (LookEyes)
    short Part[MaxBones];        // the body part it's in (its first bone): a drag bends bones of that part only
    signed char Group[MaxBones]; // a face group it's in (FaceGroupNames), -1 none
    bool FollowsGroup[MaxBones]; // below a bone in a face group
    int GroupSize[FaceGroupCount];
    int GroupMembers[FaceGroupCount][MaxGroupMembers];
    int Count;
    unsigned int List[MaxBones];
    unsigned int Map[MaxBones];
    float Base[MaxBones][FloatsPerBone];   // the local pose when taken
    float Turn[MaxBones][4];               // each bone's turn in its parent's frame (local = turn * base)
    float Move[MaxBones][3];               // each bone's move, in its parent's frame (the held root's moves the whole body)
    // Two buffers in turn: one is filled while the game's pose job can still
    // be reading the other. The box pose's override shares them (same bones).
    alignas(128) float Buffer[2][MaxBones * FloatsPerBone];
    PoseOverride Body[2], Box[2];
    GameVector ListVector, MapVector;
    int Next;
    __int64* BodyTable;          // where ours was handed (0: not now)
    __int64* BoxTable;
    unsigned int BodyTableHandle, BoxTableHandle;
    float Handed[MaxBones][FloatsPerBone]; // the locals handed last frame, for the check
    bool HandedOk;
    float World[MaxBones][FloatsPerBone];  // this frame's world pose: the game's, the held bones from the pose
    bool WorldOk;
    char Label[64];
    char Asset[pose_skeleton_live::AssetBytes];   // its own SkeletonAsset's name (lower case), for the files
    char Status[200];
    bool Aside, Refused, BoxRefused, NoBoxLogged;
    // Whether the game took the pose: frames with time running / frozen
    int Frames, Taken, FrozenFrames, FrozenTaken;
    float Worst;
    ULONGLONG CheckSince;
    char CheckLine[200];
    // A character's weapons (guns, batons: actors of their own), each going
    // with the bone it was on when taken (see KeepWeapons)
    int GripBone;
    HeldWeapon Weapons[MaxWeapons];
    int WeaponCount;
    ULONGLONG WeaponsCheckedAt;
    // Idle movement (UpdateIdle): each held bone's animation as the game
    // made it under the pose, smoothed (where it has been: Ref, local
    // rotation then translation), and its movement about that now (Idle:
    // a turn in the bone's own frame, then a move in its parent's)
    float IdleRef[MaxBones][7];
    float Idle[MaxBones][7];
    bool IdleRefTurn[MaxBones];              // that bone's Ref rotation taken (from a good record the game wrote)
    bool IdleRefMove[MaxBones];              // and its translation
    bool IdleRefOk;
    bool IdleActive;                         // Idle is laid over the handed pose (by IdleWeight)
    float IdleWeight;                        // 0 - 1, fading in and out (IdleFadeSeconds)
    double IdleWeightAt;                     // the frame it was last faded at (performance counter, s; 0: none)
    bool IdleGateOpen;                       // the character standing (see UpdateIdle)
    float IdleRoot[FloatsPerBone];           // where it was at the last capture (its root's parent)
    bool IdleRootOk;
    // The gate's bones (FindIdleBones, at Take pose): 1 Hips or a leg (hips, thighs, shins, feet), 2 the spine
    // (Spine, Spine1, Spine2); IdlePeople: all ten found (a person: the people's gate, else the 10-degree one)
    unsigned char IdleGroup[MaxBones];
    bool IdlePeople;
    float IdlePrev[MaxBones][4];             // each held bone's rotation at the capture before (its turn speed)
    bool IdlePrevOk[MaxBones];
    double IdleCaptureAt;                    // the capture last used: when the game's animation made it (s)
    double IdleGameClock;                    // the game's time at that capture (s from the wanting; see UpdateIdle)
    double IdleCalmSince;                    // the gate shut: since when (IdleGameClock) all has been calm (-1: not calm)
    // The root's places and the hips', legs' and spine's turn speeds (degrees a second of the game's time,
    // IdleSpinBones in order) at the last IdleRingSize captures (8.5 s at 30 a second), with their IdleGameClock
    static const int IdleRingSize = 256;
    static const int IdleSpinBones = 10;
    double IdleRootTime[IdleRingSize];
    float IdleRootAt[IdleRingSize][3];
    int IdleRootHead, IdleRootCount;
    double IdleSpinTime[IdleRingSize];
    float IdleSpin[IdleRingSize][IdleSpinBones];
    int IdleSpinHead, IdleSpinCount;
    int IdleSpinBone[IdleSpinBones];         // Hips, LeftUpLeg, LeftLeg, LeftFoot, RightUpLeg, RightLeg, RightFoot, Spine, Spine1, Spine2
    // Since the last log line (the 10 s idle line): captures with the gate open, and those where each of its
    // limits was passed; those the 10-degree rule before 2026-11-14 would have shut; the most each came to
    int IdleOpenCaptures, IdleOverSpeed, IdleOverLegs, IdleOverSpin, IdleOldRule;
    float IdleMostSpeed, IdleMostLegs, IdleMostSpin, IdleMostStep;
    int IdleMostLegsBone, IdleMostSpinBone, IdleMostStepBone;
    // Letting go (KeepLettingGo): its overrides out, held on until the game's
    // own pass of its slots put back what the game doesn't animate
    bool Restoring;
    ULONGLONG RestoreSince;
    bool RestoreWaitLogged;
    char RestoreWhy[96];
    unsigned int IdleGen;                    // its wanting (IdleCapture::Gen)
    unsigned int IdleSeen;                   // the capture's count last read
    double IdleAt, IdleNewAt;                // the frame a capture was last used at (performance counter, s; 0: none)
    int IdleFrames;                          // captures used since the last log line
    // Undo and Redo (2026-11-15, the user: the keyframes "feel too bad"; there was one Undo for the pose and another
    // for its keyframes): the pose and the keyframes before each change (History, at most MaxUndo) and what each
    // Undo took back (Future). HistoryHasKeys: the last step was pushed before a change of the pose and holds the
    // keyframes as they still are, so keyframing that change (auto key after a drag) is part of the same step.
    std::vector<HistoryStep> History;
    std::vector<HistoryStep> Future;
    bool HistoryHasKeys;
    // What the last push cleared (the Redo steps) and trimmed (the oldest step, past MaxUndo), kept until that
    // push's step proves a change: put back if it's dropped (a drag put back, a click that changed nothing:
    // DropLastStep, SettleLastStep), so Redo and the oldest step aren't lost to nothing
    std::vector<HistoryStep> FutureStash;
    HistoryStep TrimmedStash;
    bool TrimmedOk;
    bool StashOk;
    bool KeysTimeMoved;                      // the keyframes' time moved this frame (KeepKeys, after Advance)
    pose_keys::Anim Keys;                    // this pose's keyframes
    double KeysAppliedTime;                  // the keys' time the pose was last made from
    unsigned KeysAppliedRevision;             // their Revision then
    bool KeysAppliedOk;                      // false: make the pose from them next frame
    unsigned KeysSavedRevision;               // the keys' Revision at the last autosave
    ULONGLONG KeysSavedAt;                    // when
    bool KeysAutosaveStarted;                 // whether this pose has written its autosave file yet
    std::wstring KeysFile;                    // its keyframes' file: the one they were last saved in by hand (the
                                              // keyframes bar's Save writes there; "": none, Save asks), a whole path
    std::wstring KeysSuggest;                 // the file last saved or loaded by hand: where Save as starts (2026-11-16)
    unsigned KeysLoads;                       // how many keyframes files were loaded onto it (TakeKeyFileAnswer)
    float KeyRefTurn[MaxBones][4];            // each bone's edit when the playhead last moved or it was keyed
    float KeyRefMove[MaxBones][3];            // (ChangedBones measures against these)
    double KeyTrackTimeBefore;                // the camera track's time last frame (is it moving?)
    bool KeyTrackMoving;                     // the track moved this frame (PlayingKeys' Clock_Track)
    ClipPlay Clip;                           // an animation clip on it (none: Clip.Clip null)
    VoicePlay Voice;                         // a voice on it (none: Voice.Sound null)
    // The kinematics (PoseKinematics.inl, PoseMotion.inl; 2026-11-16, ResetKinematics at Take)
    KinRig Kin;                              // its limbs, spine, neck, eyes and helper bones
    KinPin Pins[LimbCount];                  // a hand or foot pinned (the Bone tab's Pin)
    float MotionWeight;                      // the keyframes' playing layers (loose timing, follow-through, the feet): 0 - 1
    double KeysDt;                           // how far the keyframes' time moved this frame (KeepKeys; a loop's wrap taken out)
    float SpringTurn[MaxBones][4];           // follow-through: each sprung bone's turn in the character's frame,
    float SpringSpin[MaxBones][3];           // and its turning speed (radians a second of the keyframes' time)
    bool SpringOk[MaxBones];
    int LookMode;                            // Look at: Look_Off, Look_Camera, Look_Point
    float LookPoint[3];                      // Look_Point's place (world)
    bool LookPointOk;
    float LookAmount;                        // how much of the way the chest, neck and head turn (the eyes look all of it)
    float LookWeight;                        // 0 - 1, fading in and out
    float LookTarget[3];                     // where it looked last (it fades out from there)
    bool LookTargetOk;
    float LookYaw, LookPitch;                // the look's turn as laid (radians: about the character's up, then across),
    bool LookAngleOk;                        // eased towards the place's (LookAt)
  };

  std::mutex g_Mutex;                        // everything below (the menu and the game's thread)
  Held g_Held[MaxHeld];
  int g_NextId = 1;
  int g_Active = -1;                         // the held pose the menu and the dots edit (index in g_Held)
  int g_SelectedBone = -1;
  int g_SelectedGroup = -1;                  // a face group picked instead of a bone (-1 none)
  int g_SelectedWeapon = -1;                 // a weapon of the held pose picked instead (index in its Weapons, -1 none)
  std::vector<Candidate> g_Candidates;
  __int64 g_Picked = 0;                      // the candidate picked in the list (its entity)
  ULONGLONG g_ListWantedAt = 0, g_ScannedAt = 0;
  // From the menu, done on the game's thread
  bool g_TakeRequest = false;
  Candidate g_TakeWhat{};
  int g_ReleaseRequest = -2;                 // -2 none, -1 all, else a held index
  bool g_ShowBones = true;
  bool g_ShowFace = false;
  bool g_ShowFingers = false;
  bool g_GroupFace = true;                   // the face's bones in groups (FaceGroupNames)
  bool g_ShowHelpers = false;                // roll, twist and other helper bones' dots
  bool g_StopAtWalls = true;                 // a dragged bone (and its limb) stops at what's solid
  bool g_FreeMove = false;                   // dragging moves the bone itself (any bone), with X / Y / Z arrows
  const float MaxFreeMove = 1.f;             // m: a bone's move (not the Hips', not a face bone's)
  bool g_WallsReady = false;                 // the world's sweeps can be made (the game's thread says)
  const float BoneRadius = 0.05f;            // m: the dragged bone's ball in the world's sweeps
  const float JointRadius = 0.03f;           // m: the other moved joints' (smaller: toes sit 2 cm over the floor)
  const float SelfRadius = 0.5f;             // m: round where a character stands, its own body (see SelfHit)
  bool g_RingsOnBone = true;                 // the rings about the bone's own axes (else the world's)
  bool g_HairWhilePaused = true;             // Faith's hair simulated while time is frozen (PausedHair.cpp)
  bool g_WeaponsFollow = true;               // a posed character's guns and batons go with its posed bones (KeepWeapons)
  bool g_IdleOn = false;                     // the game's own animation's small movements over the pose (UpdateIdle)
  float g_IdleAmount = 1.f;                  // how much of them
  bool g_AutoKey = true;                     // Auto key: every change is keyframed at once, from the first one
                                             // (AutoKeyChanges; the KEYFRAMES row's red tick and Options)
  bool g_KeyAnchors = true;                  // a bone's first keyframe holds it as it was at the keyframe times
                                             // around it (KeyBone; Options: Hold the times around a new keyframe)
  bool g_ConfigWanted = false;               // a setting changed where DrawWindow's configChanged doesn't reach
  char g_KeyNote[512] = "";                  // what the last keyframing did (NoteKeys; under the Keyframe: row)
  ULONGLONG g_KeyNoteAt = 0;                 // when
  int g_KeyNoteHeld = 0;                     // the held pose's Id it was for
  const ULONGLONG KeyNoteMs = 8000;          // shown bright this long, then dim
  bool g_SnapKeys = true;                    // times snap to 0.05 s (step 3)
  bool g_WindowOpen = false;                 // the Pose window (step 3)
  const float IdleSmoothSeconds = 3.f;       // each bone's movement is taken about where it has been this long
  const float MaxIdleTurn = 15.f * 3.14159265f / 180.f;  // and kept small: a turn eased towards this (SoftTurn)
  const float MaxIdleMove = 0.08f;           // and this move (m): at most
  const float IdleFadeSeconds = 0.4f;        // it fades in and out this fast
  // When it isn't idle. A person (Hips, the legs and the spine found): its root moving, its hips or legs far off
  // where their animation has been, or those and the spine turning fast; open again once all of them have been
  // calm for a while. All of it in the game's time (UpdateIdle). Measured on the people in three of the tools'
  // replay recordings (2026-11-14, Research_Pose_Editor section 34): gesture idles move the arms, hands and head up
  // to 93 degrees off where they have been (calmer ones stay within about 8), and the 10-degree gate before shut the
  // standing men's idle in about 70 % of the captures, all standing people's in about half; standing, the hips and
  // legs stayed within 29 degrees and the root didn't move; run through UpdateIdle, the gate shut at walk starts at
  // once or within half a second.
  const float IdleGateSpeed = 0.3f;          // m/s, its root over IdleSpeedWindow: walking
  const float IdleOpenSpeed = 0.15f;
  const double IdleSpeedWindow = 0.5;
  const float IdleGateLegs = 40.f * 3.14159265f / 180.f;   // Hips or a leg this far off where its animation has been
  const float IdleOpenLegs = 25.f * 3.14159265f / 180.f;
  const float IdleGateSpin = 200.f;          // degrees a second: Hips, a leg or the spine turning this fast (its mean
  const float IdleOpenSpin = 120.f;          // over IdleSpinWindow, the fastest of them)
  const double IdleSpinWindow = 0.2;
  const double IdleCalmSeconds = 0.5;        // all calm this long before it opens again
  // Any other skeleton (no hips and legs: drones, animals, props): the gate before 2026-11-14, a body bone this far
  // off where its animation has been, or its root faster than IdleGateSpeed
  const float IdleGateTurn = 10.f * 3.14159265f / 180.f;
  const double IdleStaleSeconds = 0.5;       // no capture this long (longer at a slow game speed): it fades out
  // This frame's clocks (set in Update): the performance counter (s) when they were read, this frame's real time (s,
  // 0 - 0.25) and the game's time scale (the idle movement, the clips and the voices go by them)
  double g_ClipCounterSeconds = 0.0;
  double g_ClipRealDt = 0.0;
  float g_ClipGameScale = 1.f;
  char g_Filter[64] = "";
  char g_SaveName[64] = "";
  std::vector<std::string> g_PoseFiles;
  ULONGLONG g_FilesListedAt = 0;
  std::vector<std::wstring> g_KeyFiles;    // the .posekeys files' names (ListKeyFiles; wide since 2026-11-16: Save as
                                           // can name one past the ANSI code page)
  ULONGLONG g_KeyFilesListedAt = 0;
  char g_FileMessage[512] = "";              // (512 since 2026-11-16: a file anywhere is said with its path)
  char g_LetGoMessage[200] = "";
  bool g_AutosaveFailLogged = false;
  bool g_SlidersWereActive = false;
  // The pose when the Bone tab's slider in use was taken (DrawPosePanel; 2026-11-16): each frame's edit is made from it
  // (SliderHold), so a hold's feet and pins are taken once, not again from the frame before
  Snapshot g_SliderPress;
  int g_SliderPressHeld = 0;
  bool g_WindowFocus = false;                 // open the Pose window and bring it to the front next frame
  ImVec2 g_WindowMin, g_WindowMax;            // its rectangle last frame (for the overlay to keep out of)
  bool g_WindowRectOk = false;
  int g_PoseTab = 0;                          // Bone, Clip, Voice, Files, Options
  char g_KeySaveName[64] = "";
  bool g_LengthWasActive = false;             // the Length field pushes one key undo when it starts
  int g_BoneListBone = -2, g_BoneListGroup = -2;  // the selection last scrolled to in the bone list
  // The timeline's keyframe menu and mouse state (PLAN.md 3.3)
  int g_KeyMenuHeld = 0;
  int g_KeyMenuBone = -1;
  double g_KeyMenuTime = -1.0;
  bool g_KeyMenuOpen = false;
  // A Reset button's question when bones it resets have keyframes (RequestReset; the popup DrawResetAsk)
  struct ResetAsk
  {
    int HeldId;
    bool Bones[MaxBones];      // the bones it resets
    bool Weapons;              // Reset all: the weapons too
    int Keyed;                 // how many of them have keyframes
    int First;                 // the first of those
    char What[48];             // "this bone", "this group", ...
  };
  ResetAsk g_ResetAsk{};
  bool g_ResetAskOpen = false;
  struct KeyDragState
  {
    bool Pending, On;
    int Held, Bone;
    double StartTime, Length;
    double LastTime;
    ULONGLONG LastTick;
    float PressX;
  };
  KeyDragState g_KeyDrag{};
  bool g_KeyScrub = false;
  std::vector<double> g_KeyTimes;
  int g_KeyTimesHeld = 0;
  unsigned g_KeyTimesRevision = 0;
  size_t g_KeyTimesChannels = 0;
  int g_KeyTimelineBone = -2;                 // the selected bone last scrolled to in the timeline
  // The ring being dragged
  struct RingDrag
  {
    bool On;
    int Held, Bone, Ring;
    float Axis[3];
    float Tangent[2];        // pixels per radian along the ring where it was taken
    float Mouse[2];
    float Parent[4];         // the bone's parent's world rotation when taken
    float Turn[4];           // the bone's turn when taken
    int Group;               // a face group's ring: each member turned the same way about its own place (-1: a bone)
    float GroupParents[MaxGroupMembers][4];
    float GroupTurns[MaxGroupMembers][4];
    int Weapon = -1;         // a weapon's ring (Bone and Group -1): its edit turned (Parent: its frame's rotation)
    Snapshot Pose;           // the pose when taken (a bone's ring: each frame from it, the pins and planted feet kept)
  };
  RingDrag g_Drag{};
  // A bone's dot held with the mouse: pressed on it (Pending), then moving it
  // once the cursor went MoveStartPixels (On)
  struct MoveDrag
  {
    bool Pending, On;
    int Held, Bone;
    int Group;               // a face group's dot (Bone -1): every member moved the same way
    float Mouse[2];          // where it was pressed
    float Offset[2];         // the dot's place on the screen less the cursor's then
    float Base[3];           // the plane it moves on goes through here (the wheel: nearer or farther)
    float Start[3];          // the bone's (or group's middle's) place when pressed
    Snapshot Pose;           // the pose when it was pressed
    float Target[3];         // where it's wanted now
    bool TargetOk;
    // Shift held: the cursor going up and down moves it farther and nearer
    // along the view, from where it was when Shift went down
    bool Depth;
    float DepthBase[3];
    float DepthMouseY;
    // Bones stop at walls: the menu's thread only says where it's wanted
    // (Want); the game's thread (the world's sweeps are only made there)
    // moves it there as far as it can go from where it was last (Accepted),
    // and solves the pose
    float Want[3];
    bool WantOk;
    Snapshot Accepted;       // the pose the last frame that went through
    float AcceptedPlace[3];  // the dragged bone's place in it
    // Free move's arrows: taken by an arrow, it moves along that axis only,
    // as far as the cursor goes along the arrow on the screen
    bool OnAxis;
    float Axis[3];           // the world way it moves
    float AxisScreen[2];     // the arrow on the screen, pixels for AxisLength metres
    float AxisLength;
    int Weapon = -1;         // a weapon's dot (Bone and Group -1): its edit's move
    float WeaponFrame[FloatsPerBone];  // its frame (its bone's posed place, or the world's) when pressed
    bool Trail = false;      // a dot of the picked bone's path (2026-11-16): the playhead put at its time when pressed,
                             // the change keyframed there when let go after a drag (KeyTrailDrag)
    double TrailMirror = -1.0;   // its time a looping keyframes' end: the other end's time (LoopOtherEnd), else -1
    float PressedPlace[3];   // a path's dot: where the bone was when pressed (Start is where its dot was)
  };
  MoveDrag g_Move{};
  // Look at's point held with the mouse (2026-11-16): moved across the picture at its distance from the camera
  struct LookDrag
  {
    bool On;
    int Held;
    float Base[3];           // the plane it moves on goes through here
    float Offset[2];         // the dot's place on the screen less the cursor's when pressed
  };
  LookDrag g_LookDrag{};
  const float MoveStartPixels = 3.f;
  const float MaxFaceMove = 0.15f;           // m: a face bone's move
  const float MoveWheelStep = 0.9f;          // the distance from the camera a wheel notch
  const float DepthSpeed = 1.5f;             // Shift held: a pixel up moves it this many pixels' worth farther
  const float DotReach = 0.75f;              // font sizes: how near the cursor has to be to a dot to take it
  int g_MoveBones = 2;                       // a moved bone bends this many bones above it
  // The kinematics (2026-11-16; the Options tab, saved with the tools' config; PoseKinematics.inl, PoseMotion.inl)
  bool g_JointHinges = true;                 // elbows and knees bend about their hinge, to their natural side, within limits
  bool g_KeepEndTurn = true;                 // a dragged hand or foot keeps its angle in the world
  bool g_FeetPlanted = true;                 // moving or turning the Hips keeps the feet where they stand
  bool g_CollarFollows = true;               // the collarbone takes a share of an arm's raise
  bool g_SpineCurve = true;                  // a dragged chest or head bends the spine or the neck in a curve
  bool g_FarReach = false;                   // a hand dragged past the arm's reach bends the back towards it
  bool g_HelpersFollow = true;               // roll and twist bones follow their elbow, knee, hand or upper bone
  float g_LooseTiming = 0.5f;                // while the keyframes play: the head and arms follow the chest a moment later
  float g_FollowThrough = 0.5f;              // while the keyframes play: the head, arms and hands lag and settle
  bool g_FootLock = true;                    // while the keyframes play: a foot still at the keys either side stays put
  bool g_ShowTrail = true;                   // the picked bone's path over the keyframes, on the picture
  bool g_ShowGhosts = false;                 // the pose at the keyframes before and after the playhead, on the picture
  void FindKinRig(Held& h);
  void ResetKinematics(Held& h);
  void MakeHandedPose(Held& h, float* buffer, bool worldOk);
  char const* KeyEaseLabel(int ease);
  // The keyframes (PLAN.md 2.2 - 2.5); all called under g_Mutex.
  bool PlayingKeys(Held const& h);
  bool ClipUsable(Held const& h);
  bool TrackHoldsTime(Held const& h);
  bool KeysCurrent(Held const& h);
  void PushKeyUndo(Held& h, bool merge = false);
  int ChangedBones(Held const& h, int* out);
  void KeysMatchPose(Held& h);
  int KeyBone(Held& h, int bone);
  int KeyBones(Held& h, int const* bones, int count, bool automatic, bool merge);
  void SameLineIfFits(char const* nextLabel, bool checkbox = false);
  int KeyChanges(Held& h, bool automatic = false);
  int KeySelection(Held& h);
  int KeyWholePose(Held& h);
  int SelectedKeysAtPlayhead(Held const& h);
  int RemoveSelectedKeys(Held& h);
  bool UndoStep(Held& h, bool redo);
  void RemoveAllKeys(Held& h);
  void SetPlayhead(Held& h, double t);
  void AutoKeyChanges();
  void KeepKeys(Held& h);
  void ApplyKeysNow(Held& h);
  bool SaveKeys(Held const& h, char const* name, bool quiet = false);
  int KeysFileOf(Held const& h, pose_keys::File& file);
  bool LoadKeys(Held& h, char const* name);
  void ListKeyFiles();
  bool CleanName(char const* in, char* out, size_t size);
  std::wstring Widen(char const* text);
  // The game's thread
  std::once_flag g_Checked;
  bool g_Available = false;
  char g_Unavailable[200] = "";
  std::unique_ptr<attach::Scan> g_Scan;
  std::vector<attach::Target> g_Found;
  bool g_ClassesFound = false, g_ClassesLogged = false;
  bool g_DronesLogged = false;
  ULONGLONG g_ReplayAsideLoggedAt = 0;

  bool CheckGame()
  {
    __int64 base = (__int64)g_gameHandle;
    __try
    {
      IMAGE_DOS_HEADER* pDos = (IMAGE_DOS_HEADER*)base;
      IMAGE_NT_HEADERS* pNt = (IMAGE_NT_HEADERS*)(base + pDos->e_lfanew);
      if (pNt->FileHeader.TimeDateStamp != KnownTimeStamp)
      {
        sprintf_s(g_Unavailable, "Not available: the game isn't the build the tools know (PE timestamp 0x%08X)",
          pNt->FileHeader.TimeDateStamp);
        return false;
      }
      for (CodeCheck const& check : CodeChecks)
        if (memcmp((void const*)(base + check.Rva), check.Bytes, check.Size) != 0)
        {
          sprintf_s(g_Unavailable, "Not available: the game's pose code at 0x%I64X (%s) isn't what the tools know",
            0x140000000 + check.Rva, check.What);
          return false;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      strcpy_s(g_Unavailable, "Not available: the game's code couldn't be read");
      return false;
    }
    return true;
  }

  bool Available()
  {
    std::call_once(g_Checked, []
    {
      g_Available = CheckGame();
      if (!g_Available)
        util::log::Warning("Pose: %s; GAME > Pose is off", g_Unavailable);
      g_ppGameRenderer = util::offsets::GetOffset("OFFSET_GAMERENDERER");
    });
    return g_Available;
  }

  char const* BoneName(Held const& h, int bone)
  {
    static char buffer[16];
    if (h.Skeleton >= 0 && h.Skeleton < pose_skeleton_live::Count() && bone >= 0 && bone < h.Bones)
      return pose_skeleton_live::At(h.Skeleton).Names[bone];
    sprintf_s(buffer, "bone %d", bone);
    return buffer;
  }

  int BoneByName(int skeleton, char const* name)
  {
    if (skeleton < 0 || skeleton >= pose_skeleton_live::Count())
      return -1;
    pose_skeletons::Skeleton const& s = pose_skeleton_live::At(skeleton);
    for (int i = 0; i < s.Bones; ++i)
      if (!strcmp(s.Names[i], name))
        return i;
    return -1;
  }

  char const* KindName(int kind, int skeleton)
  {
    if (kind == Kind_Faith)
      return "Faith";
    if (skeleton >= 0 && skeleton < pose_skeleton_live::Count())
      return pose_skeleton_live::At(skeleton).Label;
    // (Kind_Drone: any actor with a body of its own; a drone when its skeleton says so)
    return kind == Kind_Drone ? "actor" : "character";
  }

  // The held bone's local pose as the editor has it (the 4th float always 1:
  // the game's blend takes it as the bone's scale and never puts it back)
  // The pose a bone's edits are on: an animation clip's while one is on
  // (ClipPlay), else the pose taken
  float const* BaseOf(Held const& h, int bone)
  {
    return h.Clip.On && h.Clip.LocalOk ? h.Clip.Local[bone] : h.Base[bone];
  }

  void EditedLocal(Held const& h, int bone, float* out)
  {
    float const* base = BaseOf(h, bone);
    memcpy(out, base, FloatsPerBone * sizeof(float));
    for (int k = 0; k < 3; ++k)
      out[k] += h.Move[bone][k];
    out[3] = 1.f;
    float q[4];
    QMul(h.Turn[bone], base + 4, q);
    QNorm(q);
    memcpy(out + 4, q, sizeof(q));
  }

  bool IsOurs(PoseOverride const* pair, __int64 entry)
  {
    return entry == (__int64)&pair[0] || entry == (__int64)&pair[1];
  }

  // Ours into the slot (false if it holds another override, or can't be written)
  bool HandTo(PoseOverride* pair, int k, LiveSlot const& slot, __int64*& table, unsigned int& tableHandle)
  {
    __try
    {
      LONG64 volatile* entry = (LONG64 volatile*)&slot.Overrides[slot.Handle & 0xFF];
      LONG64 now = *entry;
      if (now != 0 && !IsOurs(pair, now))
        return false;
      pair[k].Handle = (int)slot.Handle;
      InterlockedExchange64(entry, (LONG64)&pair[k]);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      pair[k].Handle = -1;
      return false;
    }
    table = slot.Overrides;
    tableHandle = slot.Handle;
    return true;
  }

  // Ours out of the slot it was handed to, if it's still there (the game
  // clears it itself when it releases a slot)
  void TakeBack(PoseOverride* pair, __int64*& table, unsigned int tableHandle)
  {
    if (table)
    {
      __try
      {
        LONG64 volatile* entry = (LONG64 volatile*)&table[tableHandle & 0xFF];
        LONG64 now = *entry;
        if (IsOurs(pair, now))
          InterlockedCompareExchange64(entry, 0, now);
      }
      __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    table = nullptr;
    pair[0].Handle = pair[1].Handle = -1;
  }

  // A character's weapons: the PamClientCharacterWeaponEntities whose
  // character is it (+ 0x120), each with a body of its own (+ 0x100)
  int FindWeapons(__int64 character, HeldWeapon* out, int max)
  {
    __int64 base = (__int64)g_gameHandle;
    __int64 typeInfo = base + WeaponTypeInfoRva;
    if (!TypeNameIs(typeInfo, "PamClientCharacterWeaponEntity"))
      return 0;
    int count = 0;
    __try
    {
      for (int list = 0; list < 4 && count < max; ++list)
      {
        __int64 link = *(__int64*)(typeInfo + TypeInstanceListsOffset + list * 0x10);
        for (int n = 0; link && n < MaxPerList && count < max; ++n, link = *(__int64*)link)
        {
          __int64 entity = link - EntityLinkOffset;
          if (TypeOf(entity) != typeInfo || *(__int64*)(entity + WeaponOwnerOffset) != character)
            continue;
          __int64 body = *(__int64*)(entity + ActorBodyOffset);
          if (body < 0x10000 || TypeOf(body) != base + AnimatableTypeInfoRva)
            continue;
          bool known = false;
          for (int k = 0; k < count; ++k)
            known |= out[k].Entity == entity;
          if (known)
            continue;
          HeldWeapon& w = out[count++];
          memset(&w, 0, sizeof(w));
          w.Entity = entity;
          w.VTable = *(__int64*)entity;
          w.Animatable = body;
          w.Bone = -1;
        }
      }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
    return count;
  }

  // Its mesh's name, its path's last part without "_Mesh" (cpf_baton)
  void WeaponName(HeldWeapon& w)
  {
    strcpy_s(w.Name, "weapon");
    __try
    {
      __int64 data = *(__int64*)(w.Entity + WeaponDataOffset);
      __int64 mesh = data >= 0x10000 ? *(__int64*)(data + WeaponDataMeshOffset) : 0;
      char const* name = mesh >= 0x10000 ? *(char const**)(mesh + 0x10) : nullptr;
      if ((__int64)name < 0x10000)
        return;
      char const* last = name;
      for (int n = 0; n < 200 && name[n]; ++n)
        if (name[n] == '/')
          last = name + n + 1;
      size_t length = 0;
      while (length + 1 < sizeof(w.Name) && last[length] >= 32 && last[length] < 127)
      {
        w.Name[length] = last[length];
        ++length;
      }
      w.Name[length] = 0;
      char* suffix = strstr(w.Name, "_Mesh");
      if (suffix)
        *suffix = 0;
      if (!w.Name[0])
        strcpy_s(w.Name, "weapon");
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      strcpy_s(w.Name, "weapon");
    }
  }

  // Where a weapon is: its pose's bone 1, local (its entity's transform) and
  // world (its body's placement); false if it isn't that character's any more
  bool WeaponPlace(HeldWeapon const& w, __int64 character, float* local, float* world)
  {
    LiveSlot slot{};
    __try
    {
      if (*(__int64*)w.Entity != w.VTable || *(__int64*)(w.Entity + ActorBodyOffset) != w.Animatable ||
          *(__int64*)(w.Entity + WeaponOwnerOffset) != character || !SlotOf(w.Animatable, 0, slot))
        return false;
      memcpy(local, slot.Local + 1 * FloatsPerBone, FloatsPerBone * sizeof(float));
      memcpy(world, slot.World + 1 * FloatsPerBone, FloatsPerBone * sizeof(float));
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      return false;
    }
    float const* both[2] = { local, world };
    for (float const* p : both)
    {
      float length = p[4] * p[4] + p[5] * p[5] + p[6] * p[6] + p[7] * p[7];
      if (!std::isfinite(p[0]) || !std::isfinite(p[1]) || !std::isfinite(p[2]) || !(length > 0.9f && length < 1.1f))
        return false;
    }
    return true;
  }

  // Its entity's rows (4x4, as FaithReplay.cpp's ReadEntityRows: its
  // components + 0x38, the transform at + 0x10 + (c[9] + 2 c[0xA]) x 0x20)
  bool WeaponRows(__int64 entity, float* rows)
  {
    __try
    {
      BYTE const* components = *(BYTE const**)(entity + 0x38);
      if ((__int64)components < 0x10000)
        return false;
      memcpy(rows, components + 0x10 + ((__int64)components[9] + 2 * components[0xA]) * 0x20, 16 * sizeof(float));
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      return false;
    }
    for (int i = 0; i < 16; ++i)
      if (!(fabsf(rows[i]) < 1e6f))
        return false;
    return true;
  }

  // A place and rotation into rows: the rotation as XMMatrixRotationQuaternion
  // makes it (an actor's rows are, the replay's finding), then the place; the
  // rows' fourth floats left as they are
  void RowsFrom(float const* qt, float* rows)
  {
    float x = qt[4], y = qt[5], z = qt[6], w = qt[7];
    rows[0] = 1.f - 2.f * (y * y + z * z); rows[1] = 2.f * (x * y + z * w); rows[2] = 2.f * (x * z - y * w);
    rows[4] = 2.f * (x * y - z * w); rows[5] = 1.f - 2.f * (x * x + z * z); rows[6] = 2.f * (y * z + x * w);
    rows[8] = 2.f * (x * z + y * w); rows[9] = 2.f * (y * z - x * w); rows[10] = 1.f - 2.f * (x * x + y * y);
    rows[12] = qt[0]; rows[13] = qt[1]; rows[14] = qt[2];
  }

  // a^-1 * b: b in a's frame
  void Relative(float const* a, float const* b, float* out)
  {
    float inverse[4];
    QConj(a + 4, inverse);
    float d[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] };
    QRot(inverse, d, out);
    out[3] = 1.f;
    QMul(inverse, b + 4, out + 4);
  }

  // A weapon body's slot buffers, as the replay finds a body's (FaithReplay.cpp
  // FindRawSlot): local and world pose (manager + 0x8C a slot), the 3x4
  // matrices (+ 0x10, manager + 0x90, there when manager + 0x2B9 is set) and
  // the two animation steps (+ 0x18, + 0x20, manager + 0x98; 0x30 a bone:
  // scale, rotation, translation). Live 2026-11-08 (cpf_baton, 26 bones):
  // 0x340, 0x4E0, 0x4E0, + 0x2B9 set. The two steps are Ant's last two pose
  // steps in the local pose's layout (bone 1 is local bone 1), which the
  // render-side entry at [page + 0x40] + slot x 0x20 points at as from and to
  // for its blend (read live on Faith, 2026-11-08); the world pose and the
  // matrices are built from the local pose only (0x142FCFC30), and a skinned
  // draw takes the matrices (Research_Pose_Editor section 2.2).
  const int ManagerMatrixStrideOffset = 0x90;
  const int ManagerStepStrideOffset = 0x98;
  const int ManagerHasMatricesOffset = 0x2B9;
  const int PageMatricesOffset = 0x10;
  const int PageDrawnOffset = 0x18;
  const int PageStepOffset = 0x20;
  const int StepBoneFloats = 12;
  struct WeaponSlot
  {
    __int64 Manager;
    unsigned int Handle;
    int Bones;
    float* Local;
    float* World;
    float* Matrices;
    float* Drawn;
    float* Step;
  };

  bool WeaponSlotOf(__int64 animatable, WeaponSlot& out)
  {
    LiveSlot slot{};
    __try
    {
      if (!SlotOf(animatable, 0, slot) || slot.Bones > MaxWeaponBones)
        return false;
      __int64 m = slot.Manager;
      int matrixStride = *(int*)(m + ManagerMatrixStrideOffset);
      int stepStride = *(int*)(m + ManagerStepStrideOffset);
      if (!*(BYTE*)(m + ManagerHasMatricesOffset) || matrixStride != slot.Bones * 12 * (int)sizeof(float) ||
          stepStride < slot.Bones * StepBoneFloats * (int)sizeof(float) || stepStride > 0x10000)
        return false;
      __int64* pages = *(__int64**)(m + ManagerPagesOffset);
      BYTE* page = (BYTE*)pages[slot.Handle >> 24];
      unsigned int index = slot.Handle & 0xFF;
      BYTE* matrices = *(BYTE**)(page + PageMatricesOffset);
      BYTE* drawn = *(BYTE**)(page + PageDrawnOffset);
      BYTE* step = *(BYTE**)(page + PageStepOffset);
      if ((__int64)matrices < 0x10000 || (__int64)drawn < 0x10000 || (__int64)step < 0x10000)
        return false;
      out.Manager = m;
      out.Handle = slot.Handle;
      out.Bones = slot.Bones;
      out.Local = slot.Local;
      out.World = slot.World;
      out.Matrices = (float*)(matrices + (size_t)index * matrixStride);
      out.Drawn = (float*)(drawn + (size_t)index * stepStride);
      out.Step = (float*)(step + (size_t)index * stepStride);
      return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      return false;
    }
  }

  // Its slot buffers as they are, kept (the tools take it over from there)
  bool KeepWeaponBuffers(HeldWeapon& w)
  {
    WeaponSlot slot{};
    if (!WeaponSlotOf(w.Animatable, slot) || slot.Bones < 3)
      return false;
    __try
    {
      memcpy(w.Local0, slot.Local, (size_t)slot.Bones * FloatsPerBone * sizeof(float));
      memcpy(w.World0, slot.World, (size_t)slot.Bones * FloatsPerBone * sizeof(float));
      memcpy(w.Matrices0, slot.Matrices, (size_t)slot.Bones * 12 * sizeof(float));
      memcpy(w.Drawn0, slot.Drawn + 1 * StepBoneFloats, sizeof(w.Drawn0));
      memcpy(w.Step0, slot.Step + 1 * StepBoneFloats, sizeof(w.Step0));
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      return false;
    }
    float const* l = w.Local0[1];
    float length = l[4] * l[4] + l[5] * l[5] + l[6] * l[6] + l[7] * l[7];
    if (!std::isfinite(l[0]) || !std::isfinite(l[1]) || !std::isfinite(l[2]) || !(length > 0.98f && length < 1.02f))
      return false;
    w.Manager = slot.Manager;
    w.Handle = slot.Handle;
    w.Bones = slot.Bones;
    return true;
  }

  // A rotation as a matrix for column vectors (rows), as FaithReplay.cpp's QuatMatrix
  void QMatrix(float const* q, float m[3][3])
  {
    float x = q[0], y = q[1], z = q[2], w = q[3];
    m[0][0] = 1.f - 2.f * (y * y + z * z); m[0][1] = 2.f * (x * y - z * w); m[0][2] = 2.f * (x * z + y * w);
    m[1][0] = 2.f * (x * y + z * w); m[1][1] = 1.f - 2.f * (x * x + z * z); m[1][2] = 2.f * (y * z - x * w);
    m[2][0] = 2.f * (x * z - y * w); m[2][1] = 2.f * (y * z + x * w); m[2][2] = 1.f - 2.f * (x * x + y * y);
  }

  // A step record's bone (scale, rotation, translation) with this local pose
  void StepWith(float const* kept, float const* local, float* out)
  {
    memcpy(out, kept, StepBoneFloats * sizeof(float));
    memcpy(out + 4, local + 4, 4 * sizeof(float));   // rotation
    memcpy(out + 8, local, 3 * sizeof(float));       // translation (scale and fourth floats kept)
  }

  // Its body where its local bone 1 (its entity's transform; bone 0 is the
  // identity, so that's in the world) is wanted: the whole body as it was
  // kept, moved rigidly by the move from its kept bone 1 to wanted. Bone 1's
  // local pose and both animation steps' bone 1, every world pose from bone 1
  // on, every matrix from bone 1 on (the world pose times the skinning's fixed
  // transform: the same move from the left). Bone 0 and the other bones'
  // local poses are left as they are. False if its slot isn't the one kept.
  // A 3x4 matrix's place (rows, the translation in the fourth column)
  void ReadMatrixPlace(float const* m, float* place)
  {
    __try
    {
      place[0] = m[3];
      place[1] = m[7];
      place[2] = m[11];
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
  }

  // The slot's buffers now, if the game rebuilt them since the tools' write
  // last frame (they differ from it): the kept pose taken afresh from them
  void RebaseWeaponBody(HeldWeapon& w, WeaponSlot const& slot)
  {
    if (!w.WrittenOk)
      return;
    __try
    {
      size_t worldBytes = (size_t)w.Bones * FloatsPerBone * sizeof(float), matrixBytes = (size_t)w.Bones * 12 * sizeof(float);
      if (!memcmp(slot.World, w.LastWorld, worldBytes) && !memcmp(slot.Matrices, w.LastMatrices, matrixBytes))
        return;
      float local1[FloatsPerBone];
      memcpy(local1, slot.Local + 1 * FloatsPerBone, sizeof(local1));
      float length = local1[4] * local1[4] + local1[5] * local1[5] + local1[6] * local1[6] + local1[7] * local1[7];
      if (!std::isfinite(local1[0]) || !std::isfinite(local1[1]) || !std::isfinite(local1[2]) ||
          !(length > 0.98f && length < 1.02f))
        return;
      memcpy(w.Local0[1], local1, sizeof(local1));
      memcpy(w.World0, slot.World, worldBytes);
      memcpy(w.Matrices0, slot.Matrices, matrixBytes);
      memcpy(w.Drawn0, slot.Drawn + 1 * StepBoneFloats, sizeof(w.Drawn0));
      memcpy(w.Step0, slot.Step + 1 * StepBoneFloats, sizeof(w.Step0));
      w.Rebased = true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
  }

  bool WriteWeaponBody(HeldWeapon& w, float const* wanted, float* matrixPlace)
  {
    WeaponSlot slot{};
    if (!WeaponSlotOf(w.Animatable, slot) || slot.Manager != w.Manager || slot.Handle != w.Handle || slot.Bones != w.Bones)
      return false;
    RebaseWeaponBody(w, slot);
    float inverse[4], dq[4], turned[3], dt[3];
    QConj(w.Local0[1] + 4, inverse);
    QMul(wanted + 4, inverse, dq);
    QNorm(dq);
    QRot(dq, w.Local0[1], turned);
    for (int k = 0; k < 3; ++k)
      dt[k] = wanted[k] - turned[k];
    float r[3][3];
    QMatrix(dq, r);
    static float world[MaxWeaponBones][FloatsPerBone], matrices[MaxWeaponBones][12];
    memcpy(world[0], w.World0[0], sizeof(world[0]));
    memcpy(matrices[0], w.Matrices0[0], sizeof(matrices[0]));
    for (int i = 1; i < w.Bones; ++i)
    {
      float const* w0 = w.World0[i];
      float p[3];
      QRot(dq, w0, p);
      for (int k = 0; k < 3; ++k)
        world[i][k] = dt[k] + p[k];
      world[i][3] = w0[3];
      QMul(dq, w0 + 4, world[i] + 4);
      QNorm(world[i] + 4);
      float const* m0 = w.Matrices0[i];
      for (int row = 0; row < 3; ++row)
      {
        for (int col = 0; col < 3; ++col)
          matrices[i][row * 4 + col] = r[row][0] * m0[0 * 4 + col] + r[row][1] * m0[1 * 4 + col] + r[row][2] * m0[2 * 4 + col];
        matrices[i][row * 4 + 3] = r[row][0] * m0[3] + r[row][1] * m0[7] + r[row][2] * m0[11] + dt[row];
      }
    }
    float local1[FloatsPerBone], drawn1[StepBoneFloats], step1[StepBoneFloats];
    memcpy(local1, wanted, sizeof(local1));
    local1[3] = w.Local0[1][3];
    StepWith(w.Drawn0, wanted, drawn1);
    StepWith(w.Step0, wanted, step1);
    __try
    {
      memcpy(slot.Local + 1 * FloatsPerBone, local1, sizeof(local1));
      memcpy(slot.Drawn + 1 * StepBoneFloats, drawn1, sizeof(drawn1));
      memcpy(slot.Step + 1 * StepBoneFloats, step1, sizeof(step1));
      memcpy(slot.World, world, (size_t)w.Bones * FloatsPerBone * sizeof(float));
      memcpy(slot.Matrices, matrices, (size_t)w.Bones * 12 * sizeof(float));
      memcpy(w.LastWorld, world, (size_t)w.Bones * FloatsPerBone * sizeof(float));
      memcpy(w.LastMatrices, matrices, (size_t)w.Bones * 12 * sizeof(float));
      matrixPlace[0] = matrices[1][3];
      matrixPlace[1] = matrices[1][7];
      matrixPlace[2] = matrices[1][11];
      return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      return false;
    }
  }

  // Its body's slot buffers as they were kept, if it's still that slot
  void RestoreWeaponBody(HeldWeapon const& w)
  {
    WeaponSlot slot{};
    if (!WeaponSlotOf(w.Animatable, slot) || slot.Manager != w.Manager || slot.Handle != w.Handle || slot.Bones != w.Bones)
      return;
    __try
    {
      memcpy(slot.Local, w.Local0, (size_t)w.Bones * FloatsPerBone * sizeof(float));
      memcpy(slot.World, w.World0, (size_t)w.Bones * FloatsPerBone * sizeof(float));
      memcpy(slot.Matrices, w.Matrices0, (size_t)w.Bones * 12 * sizeof(float));
      memcpy(slot.Drawn + 1 * StepBoneFloats, w.Drawn0, sizeof(w.Drawn0));
      memcpy(slot.Step + 1 * StepBoneFloats, w.Step0, sizeof(w.Step0));
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
  }

  // Where a weapon is wanted: its frame (its bone's posed place, or the
  // world's) times its offset with its edit (the move in that frame, the turn
  // about its own place)
  void WeaponFrame(Held const& h, HeldWeapon const& w, float (*world)[FloatsPerBone], float* frame)
  {
    if (w.Bone >= 0 && w.Bone < h.Bones)
      memcpy(frame, world[w.Bone], FloatsPerBone * sizeof(float));
    else
    {
      frame[0] = frame[1] = frame[2] = 0.f;
      frame[3] = 1.f;
      QIdentity(frame + 4);
    }
  }

  void WeaponWanted(Held const& h, HeldWeapon const& w, float (*world)[FloatsPerBone], float const* offset, float* out)
  {
    float frame[FloatsPerBone], edited[FloatsPerBone];
    WeaponFrame(h, w, world, frame);
    for (int k = 0; k < 3; ++k)
      edited[k] = offset[k] + w.EditMove[k];
    edited[3] = 1.f;
    QMul(w.EditTurn, offset + 4, edited + 4);
    QNorm(edited + 4);
    Compose(frame, edited, out);
  }

  // Its body's pose override handed for the next frame: bone 1 at wanted
  void HandWeaponPose(Held const& h, HeldWeapon& w, float const* wanted)
  {
    LiveSlot slot{};
    bool ok = false;
    __try
    {
      ok = SlotOf(w.Animatable, 0, slot) && slot.Bones == w.Bones && slot.Bones <= MaxWeaponBones && slot.Bones > 1;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      ok = false;
    }
    if (!ok)
      return;
    if (!w.OverReady)
    {
      __int64 allocator = (__int64)g_gameHandle + GameAllocatorRva;
      w.OverList[0] = 1;
      for (int i = 0; i < MaxWeaponBones; ++i)
        w.OverMap[i] = 0xFFFFFFFFu;
      w.OverMap[1] = 0;
      w.OverListVector = { allocator, w.OverList, 1u, 1u };
      w.OverMapVector = { allocator, w.OverMap, (unsigned int)w.Bones, (unsigned int)w.Bones };
      for (int k = 0; k < 2; ++k)
        w.Over[k] = { w.OverBuffer[k], &w.OverListVector, &w.OverMapVector, 1.f, 1u, -1, 0 };
      w.OverNext = 0;
      w.OverReady = true;
    }
    // Another slot than the one it was handed to: out of that one first
    if (w.OverTable && (w.OverTable != slot.Overrides || w.OverTableHandle != slot.Handle))
      TakeBack(w.Over, w.OverTable, w.OverTableHandle);
    int k = w.OverNext;
    memcpy(w.OverBuffer[k], wanted, sizeof(w.OverBuffer[k]));
    w.OverBuffer[k][3] = 1.f;                 // the override's 4th float is the bone's scale
    w.Over[k] = { w.OverBuffer[k], &w.OverListVector, &w.OverMapVector, 1.f, 1u, (int)slot.Handle, 0 };
    if (HandTo(w.Over, k, slot, w.OverTable, w.OverTableHandle))
    {
      w.OverNext ^= 1;
      w.OverRefused = false;
    }
    else if (!w.OverRefused)
    {
      w.OverRefused = true;
      util::log::Write("Pose: %s 0x%I64X: its %s's pose slot holds another override (a replay's?): only drawn by the "
        "tools", h.Label, h.Entity, w.Name);
    }
  }

  // Its weapons back to the game: their bodies' slot buffers as the tools
  // found them, their entities back where they were and the game's own moves
  // of them let through (its next one puts each where it has its hand), their
  // bodies' placement their own. Game thread.
  // characterThere: the character was found this frame (false: it's gone or
  // being torn down, and its weapons with it: nothing of them is touched).
  // What was kept is put back only into a weapon checked this frame (still
  // that weapon, the character's), and only if time didn't run while the
  // tools had it (else it isn't where the game has the weapon now, and the
  // game's next update and move put it in the hand once time runs). The
  // tools' own lists are always let go.
  void LetGoWeapon(Held const& h, HeldWeapon& w, bool characterThere)
  {
    float local[FloatsPerBone], world[FloatsPerBone];
    bool there = characterThere && (w.Drawing || w.Holding || w.Rows0Ok) && WeaponPlace(w, h.Entity, local, world);
    bool putBack = there && !w.TimeRan;
    if (w.Drawing && putBack)
    {
      // As kept, or (the game rebuilt it meanwhile) its pose now moved back to
      // its first bone 1
      float scratch[3];
      if (!w.Rebased)
        RestoreWeaponBody(w);
      else if (w.FirstLocal1Ok)
        WriteWeaponBody(w, w.FirstLocal1, scratch);
    }
    if (w.Placing)
      util::hooks::PlaceBody(w.Animatable + BodyUpdateOffset, nullptr);
    // Its override out (only if it's still ours there)
    TakeBack(w.Over, w.OverTable, w.OverTableHandle);
    if (w.Rows0Ok && putBack)
      util::hooks::MoveActorDrawn(w.Entity, w.Rows0);
    if (w.Holding)
      util::hooks::PoseHoldActor(w.Entity, false);
    w.Drawing = w.Placing = w.Holding = w.WrittenOk = w.GapOk = w.Rows0Ok = w.Rebased = w.TimeRan = w.FirstLocal1Ok = false;
  }

  void LetGoWeapons(Held& h, bool characterThere)
  {
    for (int i = 0; i < h.WeaponCount; ++i)
      LetGoWeapon(h, h.Weapons[i], characterThere);
  }

  // Each frame the pose is handed: each of its weapons where its frame is now
  // (its bone's posed place, or put down), as it was on it when taken, with
  // its edit. The game doesn't redraw a weapon from its moved entity while
  // time is frozen (2026-11-08 live: its entity at the posed grip, its local
  // bone 1, and so its world pose and matrices, 14.5 cm away where it was
  // before), so its body is drawn by the tools, the way the replay draws the
  // bodies the game doesn't update (FaithReplay.cpp DrawBody): its slot
  // buffers written each frame (WriteWeaponBody) at the camera hook, after
  // the frame's pose jobs (the game wrote Faith's world
  // pose and matrices before every one of 1336 camera hooks, and the tools'
  // writes there were drawn: Research_Pose_Editor section 2.2). Its update
  // isn't held: a held body goes on the game's list of bodies it doesn't
  // update, and nothing was found that puts one with a pose slot back
  // (MEC_Reverse_Notes, the replay's guns, 2026-10-24). The entity is moved
  // too, its own moves held back. Game thread.
  // drawn: the world pose of the character as the game drew it this frame
  // (KeepHeld's chain: the pose handed last frame), which the weapon goes with
  // (h.World is the one handed now, drawn next frame)
  void KeepWeapons(Held& h, ULONGLONG now, float (*drawn)[FloatsPerBone], bool timeFrozen)
  {
    bool check = now - h.WeaponsCheckedAt >= 1000;
    if (check)
      h.WeaponsCheckedAt = now;
    for (int i = 0; i < h.WeaponCount; ++i)
    {
      HeldWeapon& w = h.Weapons[i];
      if (w.Bone == -1 || w.Bone >= h.Bones)
      {
        // Left to the game (an Undo can give one back)
        if (w.Drawing || w.Holding || w.Placing || w.Rows0Ok || w.OverTable)
          LetGoWeapon(h, w, true);
        continue;
      }
      float local[FloatsPerBone], world[FloatsPerBone];
      if (!WeaponPlace(w, h.Entity, local, world))
      {
        // Dropped, gone or another's: the game's from now on (its memory may
        // be another's soon: nothing of it is put back)
        if (w.Placing)
          util::hooks::PlaceBody(w.Animatable + BodyUpdateOffset, nullptr);
        if (w.Holding)
          util::hooks::PoseHoldActor(w.Entity, false);
        TakeBack(w.Over, w.OverTable, w.OverTableHandle);
        w.Drawing = w.Placing = w.Holding = w.GapOk = w.WrittenOk = w.Rows0Ok = w.Rebased = w.TimeRan = w.FirstLocal1Ok = false;
        util::log::Write("Pose: %s 0x%I64X: its %s 0x%I64X isn't its own any more (dropped or gone): left to the game",
          h.Label, h.Entity, w.Name, w.Entity);
        w.Bone = -1;
        w.Gone = true;
        continue;
      }
      // Whether the game moved it from where the tools put it last frame: its
      // local bone 1, and its bone 1's matrix (the drawn one)
      if (w.WrittenOk && check)
      {
        w.Gap = Distance3(w.Written, local);
        WeaponSlot now{};
        float place[3] = { w.WrittenMatrix[0], w.WrittenMatrix[1], w.WrittenMatrix[2] };
        if (WeaponSlotOf(w.Animatable, now) && now.Bones == w.Bones)
          ReadMatrixPlace(now.Matrices + 1 * 12, place);
        w.MatrixGap = Distance3(w.WrittenMatrix, place);
        w.GapOk = true;
      }
      float wantLocal[FloatsPerBone], wantWorld[FloatsPerBone];
      WeaponWanted(h, w, drawn, w.OffsetLocal, wantLocal);
      WeaponWanted(h, w, drawn, w.OffsetWorld, wantWorld);
      bool wantOk = true;
      for (int k = 0; k < FloatsPerBone; ++k)
        wantOk = wantOk && std::isfinite(wantLocal[k]) && std::isfinite(wantWorld[k]) && fabsf(wantLocal[k]) < 5e5f &&
          fabsf(wantWorld[k]) < 5e5f;
      if (!wantOk)
        continue;

      if (!timeFrozen && (w.Drawing || w.Holding))
        w.TimeRan = true;

      // Its body, drawn by the tools
      if (!w.Drawing && w.DrawFailures < 3)
      {
        if (KeepWeaponBuffers(w))
        {
          w.Drawing = true;
          if (!w.FirstLocal1Ok)
          {
            memcpy(w.FirstLocal1, w.Local0[1], sizeof(w.FirstLocal1));
            w.FirstLocal1Ok = true;
          }
          util::log::Write("Pose: %s 0x%I64X: its %s's body is drawn by the tools now (%d bones)", h.Label, h.Entity, w.Name,
            w.Bones);
        }
        else if (++w.DrawFailures == 3)
          util::log::Warning("Pose: %s 0x%I64X: its %s's body couldn't be taken over (its slot buffers not found): only "
            "its entity is moved", h.Label, h.Entity, w.Name);
      }
      if (w.Drawing)
      {
        if (WriteWeaponBody(w, wantLocal, w.WrittenMatrix))
        {
          memcpy(w.Written, wantLocal, sizeof(w.Written));
          w.WrittenOk = true;
          w.DrawFailures = 0;
          // The override is used by the next frame's pose job, with the pose
          // handed now: from that pose
          float nextLocal[FloatsPerBone];
          WeaponWanted(h, w, h.World, w.OffsetLocal, nextLocal);
          HandWeaponPose(h, w, nextLocal);
        }
        else
        {
          // Its slot changed: taken over afresh next frame (that snapshot isn't
          // the first one: put back from the first bone 1, as after a rebase).
          // Its override out meanwhile (handed again with the next write).
          TakeBack(w.Over, w.OverTable, w.OverTableHandle);
          w.Drawing = w.WrittenOk = w.GapOk = false;
          w.Rebased = true;
          ++w.DrawFailures;
          util::log::Write("Pose: %s 0x%I64X: its %s's pose slot changed: taken over again", h.Label, h.Entity, w.Name);
        }
      }

      // Its entity (its rows as the game had them kept first, to put back;
      // after that its rows are made from those, not read back)
      alignas(16) float rows[16];
      if (w.Rows0Ok)
        memcpy(rows, w.Rows0, sizeof(rows));
      else if (!WeaponRows(w.Entity, rows))
      {
        if (w.MoveFailures++ == 0)
          util::log::Warning("Pose: %s 0x%I64X: its %s's transform couldn't be read: its entity isn't moved", h.Label,
            h.Entity, w.Name);
        continue;
      }
      if (!w.Rows0Ok)
      {
        memcpy(w.Rows0, rows, sizeof(w.Rows0));
        w.Rows0Ok = true;
      }
      RowsFrom(wantLocal, rows);
      if (!w.Holding)
      {
        w.Holding = util::hooks::PoseHoldActor(w.Entity, true);
        if (!w.Holding && w.MoveFailures++ == 0)
          util::log::Warning("Pose: %s 0x%I64X: the game's own moves of its %s can't be held back (too many held)",
            h.Label, h.Entity, w.Name);
      }
      if (!util::hooks::MoveActorDrawn(w.Entity, rows) && w.MoveFailures++ == 0)
        util::log::Warning("Pose: %s 0x%I64X: its %s's entity can't be moved (the models' setTransform isn't hooked)",
          h.Label, h.Entity, w.Name);
      // Its body's placement too, for when its body isn't drawn by the tools
      if (!w.Drawing)
      {
        float placement[16] = {};
        RowsFrom(wantWorld, placement);
        placement[15] = 1.f;
        if (util::hooks::PlaceBody(w.Animatable + BodyUpdateOffset, placement))
          w.Placing = true;
      }
      else if (w.Placing)
      {
        util::hooks::PlaceBody(w.Animatable + BodyUpdateOffset, nullptr);
        w.Placing = false;
      }
    }
  }

  // A click the overlay takes from ImGui ends a field being typed in, as a
  // click elsewhere would (else the typed text goes on into what's picked next)
  void EndTextEdit()
  {
    if (ImGui::IsAnyItemActive())
      ImGui::ClearActiveID();
  }

  // A weapon's frame changed with it staying where it is: its offsets made
  // anew from where it's wanted now, its edit none. To WorldFrame: put down.
  void MoveWeaponTo(Held& h, HeldWeapon& w, int bone)
  {
    if (!h.WorldOk || w.Bone == -1)
      return;
    float local[FloatsPerBone], world[FloatsPerBone], frame[FloatsPerBone];
    WeaponWanted(h, w, h.World, w.OffsetLocal, local);
    WeaponWanted(h, w, h.World, w.OffsetWorld, world);
    w.Bone = bone;
    WeaponFrame(h, w, h.World, frame);
    Relative(frame, local, w.OffsetLocal);
    Relative(frame, world, w.OffsetWorld);
    w.EditMove[0] = w.EditMove[1] = w.EditMove[2] = 0.f;
    QIdentity(w.EditTurn);
  }

  // As it was taken: its bone (or the game's), no edit
  void ResetWeapon(HeldWeapon& w)
  {
    if (w.Gone)
      return;
    w.Bone = w.TakenBone;
    memcpy(w.OffsetLocal, w.TakenOffsetLocal, sizeof(w.OffsetLocal));
    memcpy(w.OffsetWorld, w.TakenOffsetWorld, sizeof(w.OffsetWorld));
    w.EditMove[0] = w.EditMove[1] = w.EditMove[2] = 0.f;
    QIdentity(w.EditTurn);
  }

  void IdleUnwanted(int index);              // see Idle movement, below
  void ForgetIdleRefs(Held& h);
  void FindIdleBones(Held& h);
  void ResetIdleCounts(Held& h);
  bool BlendHook();

  // ---- What the game's blend writes ----
  //
  // The game's blend (0x142FCF620, see Idle movement below) writes the pose
  // into Ant's own records, in place, and Ant rewrites only the channels it
  // animates: one it doesn't (a free-moved bone's translation, mostly; the
  // size override's scale before it) kept the pose's value after the override
  // went, and the character stayed so (2026-11-09, the user: "free moved
  // bones don't return to their original position"). So for each held pose
  // and each of its slots (the body's; Faith's box pose's) the blend hook
  // keeps, per held bone, its record as the slot's last pass left it (Seen)
  // and as the game itself last wrote it (Game: the record just before the
  // pose's first blend, then each channel the game wrote since a pass). A
  // channel (scale, rotation or translation: 4 floats) that is, at a pass's
  // start, bit for bit as Seen is one the game didn't write since:
  //  - while our override is in, the idle movement leaves it out (it holds
  //    the tools' own pose, not the game's animation; CaptureIdle);
  //  - while it's out (let go, stepped aside for a replay, its slot taken by
  //    another override), the hook puts it back as in Game at the slot's next
  //    pass, before the blend (PutBackChannels): a channel the game animates
  //    gets the game's last value of it, one it doesn't the value it had
  //    before the pose.
  // The pose job fills the blend's item afresh for each slot (0x142FD7C60:
  // + 0 - 0xEF from the slot's own data, + 0xA0 its records; + 0x130 -
  // 0x157 from its override if it has one, else + 0x130 null and + 0x150 -1
  // as the caller set them, 0x142FCC935 / 0x142FCC981) and its map (rsp +
  // 0x40, 0x142FCC8EF: + 0 the manager, + 0x20 its bone -> record map, + 0x2C
  // its bones, + 0x30 [manager + 0x88], the record count step 2 copies the
  // records by at 0x142FCCB05), then calls the blend with or without an
  // override (offline, 2026-11-09). So a slot's pass without our override is
  // known by its manager and its records.
  struct BlendWatch
  {
    std::atomic<bool> On;
    std::atomic<int> Busy;                   // hooks working in it now (Unwatch waits for none)
    // Set by the game thread while it's off
    float* Buffers[2];                       // the pose's override bone buffers
    int Handles[2];                          // its slots: the body's, the box pose's (-1 none)
    int Count;
    unsigned int List[MaxBones];             // the held bones (skeleton bones)
    // The game thread's say, per slot
    std::atomic<bool> PutBack[2];            // our override isn't in it: its channels put back at its passes
    std::atomic<int> Passes[2];              // its passes that did (zeroed as PutBack goes on)
    std::atomic<int> Channels, Changed;      // channels put back; those of them more than PutBackNoticed off
    // Learnt by the hook at our blends (a slot's passes come one at a time)
    std::atomic<__int64> Records[2], Manager[2];  // the slot's records and their manager
    std::atomic<unsigned int> RecordCount[2];
    std::atomic<int> RecordsMoved;           // our blends that found a slot's records somewhere else
    std::atomic<bool> GameOk[2];             // the slot blended by us (set at its first pass, never cleared: the game thread reads it)
    std::atomic<bool> SeenOk[2];             // Seen holds (the hook's own gate)
    float Game[2][MaxBones][12];             // per skeleton bone: its record (scale, rotation, translation) as the game last wrote it
    float Seen[2][MaxBones][12];             // as the slot's last pass left it
    // The idle capture's copy, made before its seqlock opens (the body's passes only)
    float Scratch[MaxBones][12];
    unsigned char ScratchKept[MaxBones];
  };
  BlendWatch g_Watches[MaxHeld];
  std::atomic<int> g_Watched{ 0 };           // watches on
  std::atomic<int> g_BlendFaults{ 0 };       // passes whose reads or writes faulted (the rest of them skipped)
  const float PutBackNoticed = 1e-4f;        // a channel put back more than this off (m, or a rotation's part): changed
  const ULONGLONG PutBackMs = 3000;          // letting go waits this long for the game's pass at most
  const unsigned int MaxRecords = 65536;     // a slot's record count above this isn't taken as one

  // The hook out of a watch: none is in it once this returns. Game thread.
  void Unwatch(int index)
  {
    BlendWatch& w = g_Watches[index];
    if (w.On.exchange(false, std::memory_order_seq_cst))
      g_Watched.fetch_sub(1, std::memory_order_seq_cst);
    while (w.Busy.load(std::memory_order_seq_cst) != 0)
      YieldProcessor();
  }

  // Whether our override is out of that slot now (on: its channels are put
  // back at its passes). Game thread.
  void SetPutBack(int index, int k, bool on)
  {
    BlendWatch& w = g_Watches[index];
    if (w.PutBack[k].load(std::memory_order_relaxed) == on)
      return;
    if (on)
      w.Passes[k].store(0, std::memory_order_relaxed);
    w.PutBack[k].store(on, std::memory_order_seq_cst);
  }

  // A held pose's slots watched from its first blend on (Take). Game thread.
  bool Watch(Held& h)
  {
    int index = (int)(&h - g_Held);
    Unwatch(index);
    if (!BlendHook())
      return false;
    BlendWatch& w = g_Watches[index];
    w.Buffers[0] = h.Buffer[0];
    w.Buffers[1] = h.Buffer[1];
    w.Handles[0] = (int)h.Handle;
    w.Handles[1] = h.HasBox ? (int)h.BoxHandle : -1;
    w.Count = h.Count;
    memcpy(w.List, h.List, (size_t)h.Count * sizeof(w.List[0]));
    for (int k = 0; k < 2; ++k)
    {
      w.PutBack[k].store(false, std::memory_order_relaxed);
      w.Passes[k].store(0, std::memory_order_relaxed);
      w.Records[k].store(0, std::memory_order_relaxed);
      w.Manager[k].store(0, std::memory_order_relaxed);
      w.RecordCount[k].store(0, std::memory_order_relaxed);
      w.GameOk[k].store(false, std::memory_order_relaxed);
      w.SeenOk[k].store(false, std::memory_order_relaxed);
    }
    w.Channels.store(0, std::memory_order_relaxed);
    w.Changed.store(0, std::memory_order_relaxed);
    w.RecordsMoved.store(0, std::memory_order_relaxed);
    w.On.store(true, std::memory_order_seq_cst);
    g_Watched.fetch_add(1, std::memory_order_seq_cst);
    return true;
  }

  // Not editable from now on: no pick, no drag
  void Unpick(int index)
  {
    if (g_Active == index)
    {
      g_Active = -1;
      g_SelectedBone = -1;
      g_SelectedGroup = -1;
      g_SelectedWeapon = -1;
    }
    if (g_Drag.On && g_Drag.Held == index)
      g_Drag.On = false;
    if ((g_Move.Pending || g_Move.On) && g_Move.Held == index)
      g_Move.Pending = g_Move.On = false;
  }

  // Its overrides out, its watch off and its slot free (after letting go, or at once)
  void FinishRelease(Held& h, char const* why, char const* restored)
  {
    int index = (int)(&h - g_Held);
    TakeBack(h.Body, h.BodyTable, h.BodyTableHandle);
    TakeBack(h.Box, h.BoxTable, h.BoxTableHandle);
    Unwatch(index);
    if (h.Frames + h.FrozenFrames > 0)
      util::log::Write("Pose: %s 0x%I64X: the game took the pose in %d of %d frames with time running, %d of %d with time "
        "frozen, since the last line", h.Label, h.Entity, h.Taken, h.Frames, h.FrozenTaken, h.FrozenFrames);
    util::log::Write("Pose: %s 0x%I64X let go (%s)%s", h.Label, h.Entity, why, restored);
    int moved = g_Watches[index].RecordsMoved.load();
    if (moved)
      util::log::Warning("Pose: %s 0x%I64X: its slots' records were found somewhere else %d times while held (the tools "
        "take them to stay put)", h.Label, h.Entity, moved);
    int faults = g_BlendFaults.exchange(0);
    if (faults)
      util::log::Warning("Pose: the tools' part of the game's pose blend faulted in %d passes (the rest of each skipped)",
        faults);
    IdleUnwanted(index);
    h.Clip.Clip.reset();
    h.Clip.On = h.Clip.Playing = h.Clip.LocalOk = false;
    // Its voice goes with it (the sound stops, a reading of its feeling too)
    if (h.Voice.Player >= 0)
      voice_player::Close(h.Voice.Player);
    voice_emotion::Cancel(h.Voice.Emotion);   // [EMOTION] [REVIEW]
    h.Voice = VoicePlay();
    h.Restoring = false;
    h.Used = false;
    h.History.clear();
    h.History.shrink_to_fit();
    h.Future.clear();
    h.Future.shrink_to_fit();
    h.HistoryHasKeys = false;
    h.FutureStash.clear();
    h.FutureStash.shrink_to_fit();
    h.TrimmedStash = HistoryStep();
    h.TrimmedOk = h.StashOk = h.KeysTimeMoved = false;
    h.Keys = pose_keys::Anim();
    h.KeysFile.clear();
    h.KeysSuggest.clear();
    Unpick(index);
  }

  void AutosaveName(Held const& h, char* out, size_t size)
  {
    int index = (int)(&h - g_Held);
    bool sameLabel = false;
    for (int j = 0; j < MaxHeld; ++j)
      if (j != index && g_Held[j].Used && !strcmp(g_Held[j].Label, h.Label))
        sameLabel = true;
    char raw[128];
    if (sameLabel)
      _snprintf_s(raw, sizeof(raw), _TRUNCATE, "autosave %s %d", h.Label, index + 1);
    else
      _snprintf_s(raw, sizeof(raw), _TRUNCATE, "autosave %s", h.Label);
    char clean[64];
    if (!CleanName(raw, clean, sizeof(clean)))
      _snprintf_s(out, size, _TRUNCATE, "%s", raw);
    else
      _snprintf_s(out, size, _TRUNCATE, "%s", clean);
  }

  // The pose's keyframes written to its autosave file. The file of that name left by an earlier pose or session is
  // kept once as "<name> before" (the first time this pose writes its own).
  bool WriteAutosave(Held& h, bool quiet)
  {
    char name[128];
    AutosaveName(h, name, sizeof(name));
    if (!h.KeysAutosaveStarted)
    {
      char path[MAX_PATH];
      char before[MAX_PATH];
      _snprintf_s(path, sizeof(path), _TRUNCATE, "%s\\%s.posekeys", PoseFolder, name);
      _snprintf_s(before, sizeof(before), _TRUNCATE, "%s\\%s before.posekeys", PoseFolder, name);
      if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES)
        MoveFileExA(path, before, MOVEFILE_REPLACE_EXISTING);
      h.KeysAutosaveStarted = true;
    }
    bool saved = SaveKeys(h, name, quiet);
    h.KeysSavedRevision = h.Keys.Revision;
    h.KeysSavedAt = GetTickCount64();
    return saved;
  }

  // Keyframes at more than one time: an animation. Keyframes at one time only hold a still (Auto key keyframes a
  // pose's first change since 2026-11-15): no autosave and no Let go question for those, so a still doesn't push an
  // animation's autosave out (the review, 2026-11-15)
  bool KeysSpanTime(pose_keys::Anim const& a)
  {
    return pose_keys::KeyCount(a) > 0 && pose_keys::LastKeyTime(a) - pose_keys::FirstKeyTime(a) > pose_keys::TimeEpsilon;
  }

  void AutosaveKeysNow()
  {
    for (int i = 0; i < MaxHeld; ++i)
    {
      Held& h = g_Held[i];
      if (!h.Used || h.Restoring || !KeysSpanTime(h.Keys) ||
          h.Keys.Revision == h.KeysSavedRevision || GetTickCount64() - h.KeysSavedAt < AutosaveMs)
        continue;
      WriteAutosave(h, true);
    }
  }

  // Let go: its overrides out at once, and (see BlendWatch) the pose held on
  // (Restoring, not editable) until the game's own pass of each slot it was
  // blended into has put back what the game didn't write since (KeepLettingGo).
  // gone: the character or its slot was found gone or changed this frame: at
  // once (its weapons are then only let go of, nothing written into them).
  void Release(Held& h, char const* why, bool gone = false)
  {
    int index = (int)(&h - g_Held);
    if (!h.Restoring && KeysSpanTime(h.Keys))
    {
      char name[128];
      AutosaveName(h, name, sizeof(name));
      if (WriteAutosave(h, false))
      {
        _snprintf_s(g_FileMessage, sizeof(g_FileMessage), _TRUNCATE,
          "%s was let go: its keyframes are saved as \"%s\" (Files tab)", h.Label, name);
        _snprintf_s(g_LetGoMessage, sizeof(g_LetGoMessage), _TRUNCATE, "%s", g_FileMessage);
      }
      else
      {
        char reason[200];
        _snprintf_s(reason, sizeof(reason), _TRUNCATE, "%s", g_FileMessage);
        _snprintf_s(g_FileMessage, sizeof(g_FileMessage), _TRUNCATE,
          "%s was let go: its keyframes could NOT be saved (%s)", h.Label, reason);
        _snprintf_s(g_LetGoMessage, sizeof(g_LetGoMessage), _TRUNCATE, "%s", g_FileMessage);
        util::log::Warning("Pose: %s", g_FileMessage);
      }
    }
    if (h.Restoring)
    {
      // Gone, or let go again while it waits (the menu)
      FinishRelease(h, why, gone ? "; gone before the game's pass put its bones back" : "; let go again before the game's "
        "pass: what the game doesn't animate stays as posed");
      return;
    }
    bool there = !gone && (h.Kind != Kind_Person || StillThere(h.Entity, h.VTable, h.BodyOffset, h.Animatable));
    LetGoWeapons(h, there);
    IdleUnwanted(index);
    // Its voice ends here (the sound stops at once, not when the game's pass is through; a reading of its feeling too)
    if (h.Voice.Player >= 0)
      voice_player::Close(h.Voice.Player);
    voice_emotion::Cancel(h.Voice.Emotion);   // [EMOTION] [REVIEW]
    h.Voice = VoicePlay();
    TakeBack(h.Body, h.BodyTable, h.BodyTableHandle);
    TakeBack(h.Box, h.BoxTable, h.BoxTableHandle);
    BlendWatch& w = g_Watches[index];
    bool watched = w.On.load(std::memory_order_relaxed), blended = false;
    if (there)
      for (int k = 0; k < 2; ++k)
        if (w.GameOk[k].load(std::memory_order_acquire))
        {
          // Its passes counted from now (the override is out already)
          w.Passes[k].store(0, std::memory_order_seq_cst);
          w.PutBack[k].store(true, std::memory_order_seq_cst);
          blended = true;
        }
    if (blended)
    {
      h.Restoring = true;
      h.RestoreSince = GetTickCount64();
      h.RestoreWaitLogged = false;
      strncpy_s(h.RestoreWhy, why, _TRUNCATE);
      Unpick(index);
      strcpy_s(h.Status, "Letting go: its bones put back as the game has them");
      return;
    }
    FinishRelease(h, why, there && !watched ? "; its bones not put back: the game's pose blend isn't hooked (see above)" :
      "");
  }

  // Each frame while letting go: done once the game's own pass of each slot
  // the pose was blended into has put its channels back (the box pose's only
  // while it's still the one). While the game doesn't animate the body (far
  // away: its distance switch) no pass comes: it waits (KeepHeld lets it go
  // if the character or its slot goes). A body the game switched off for its
  // death isn't switched on again: let go then. With the game animating it
  // and still no pass in PutBackMs (the slot not recognised): let go, logged.
  void KeepLettingGo(Held& h, bool boxSame, ULONGLONG now)
  {
    BlendWatch& w = g_Watches[&h - g_Held];
    bool body = !w.GameOk[0].load(std::memory_order_acquire) || w.Passes[0].load(std::memory_order_acquire) > 0;
    bool box = !w.GameOk[1].load(std::memory_order_acquire) || !boxSame || w.Passes[1].load(std::memory_order_acquire) > 0;
    char done[260];
    if (body && box)
    {
      sprintf_s(done, ": %d of its bones' channels the game hadn't written since the pose put back as the game last had "
        "them, %d of them more than %g off (its body's records: %u)", w.Channels.load(), w.Changed.load(), PutBackNoticed,
        w.RecordCount[0].load());
      FinishRelease(h, h.RestoreWhy, done);
      return;
    }
    int animated = AnimatedByGame(h.Animatable);
    if (animated != 1)
    {
      if (animated == 0 && util::hooks::IsBodyDead(h.Animatable + BodyUpdateOffset))
      {
        sprintf_s(done, ": dead (the game switched its body off) before the game's pass, so what it doesn't animate stays "
          "as posed (%d channels put back)", w.Channels.load());
        FinishRelease(h, h.RestoreWhy, done);
        return;
      }
      if (!h.RestoreWaitLogged)
        util::log::Write("Pose: %s 0x%I64X: let go while the game doesn't animate its body: its bones are put back when it "
          "does", h.Label, h.Entity);
      h.RestoreWaitLogged = true;
      h.RestoreSince = now;
      strcpy_s(h.Status, "Letting go: waiting for the game to animate it again");
      return;
    }
    if (now >= h.RestoreSince + PutBackMs)
    {
      int k = body ? 1 : 0;
      sprintf_s(done, ": the game animates it, but no pass of its %s reached the tools within %.0f s (records 0x%I64X, "
        "manager 0x%I64X), so what it doesn't animate there stays as posed (%d channels put back)", k ? "box pose" : "body",
        PutBackMs / 1000.0, w.Records[k].load(), w.Manager[k].load(), w.Channels.load());
      FinishRelease(h, h.RestoreWhy, done);
    }
  }

  // The candidate's pose taken as it is now, and held. Game thread.
  void Take(Candidate const& c, __int64 soldier)
  {
    for (int i = 0; i < MaxHeld; ++i)
      if (g_Held[i].Used && g_Held[i].Entity == c.Entity)
      {
        if (g_Held[i].Restoring)
          util::log::Warning("Pose: %s 0x%I64X is being let go (its bones put back): take it again in a moment",
            g_Held[i].Label, c.Entity);
        else
          g_Active = i;
        return;
      }
    int index = -1;
    for (int i = 0; i < MaxHeld && index < 0; ++i)
      if (!g_Held[i].Used)
        index = i;
    if (index < 0)
    {
      util::log::Warning("Pose: %d poses are held already, let one go first", MaxHeld);
      return;
    }
    LiveSlot body{}, box{};
    __int64 animatable = 0, boxAnimatable = 0;
    bool boxOk = false, ok = false;
    int bodyOffset = 0;
    if (c.Kind == Kind_Faith)
      ok = c.Entity == soldier && FaithSlots(soldier, animatable, body, boxAnimatable, box, boxOk);
    else
    {
      bodyOffset = c.Kind == Kind_Drone ? ActorBodyOffset : CharacterBodyOffset;
      ok = BodySlot(c.Entity, bodyOffset, animatable, body) && animatable == c.Animatable;
    }
    char const* kind = KindName(c.Kind, c.Skeleton);
    if (!ok)
    {
      util::log::Warning("Pose: %s 0x%I64X: its pose slot isn't found any more, not taken", kind, c.Entity);
      return;
    }
    int cant = Cant_Unknown;
    int skeleton = SkeletonOf(animatable, body, g_Body, cant);
    if (skeleton < 0)
    {
      util::log::Warning("Pose: %s 0x%I64X: its skeleton (%d bones) isn't one the tools know the bones of, and its "
        "bones' names couldn't be read from the game (%s), not taken", kind, c.Entity, body.Bones,
        g_Body.Why[0] ? g_Body.Why : "its asset wasn't read");
      return;
    }
    kind = KindName(c.Kind, skeleton);
    if (cant != Cant_No)
    {
      util::log::Warning("Pose: %s 0x%I64X: %s, not taken", kind, c.Entity, cant == Cant_NoRoot ?
        "its bones don't hang on one bone a pose could hold" : "its animation rig has no channel for the bone a pose "
        "holds, so the game couldn't take a pose of it");
      return;
    }
    int animated = AnimatedByGame(animatable);
    if (animated == 0)
    {
      util::log::Warning("Pose: %s 0x%I64X: the game doesn't animate its body now (far away or taken down), so it can't "
        "take a pose; not taken", kind, c.Entity);
      return;
    }
    __int64 entry = OverrideIn(body);
    if (entry != 0)
    {
      util::log::Warning("Pose: %s 0x%I64X: its pose slot 0x%X holds another override (a replay's or a Dash ghost's), "
        "not taken", kind, c.Entity, body.Handle);
      return;
    }

    Held& h = g_Held[index];
    h.Used = true;
    h.Id = g_NextId++;
    h.Kind = c.Kind;
    h.Entity = c.Entity;
    h.VTable = ReadVTable(c.Entity);
    h.BodyOffset = bodyOffset;
    h.Animatable = animatable;
    h.BoxAnimatable = boxOk ? boxAnimatable : 0;
    h.HasBox = boxOk;
    h.Manager = body.Manager;
    h.Handle = body.Handle;
    h.BoxHandle = boxOk ? box.Handle : 0xFFFFFFFFu;
    h.Bones = body.Bones;
    h.Skeleton = skeleton;
    pose_skeletons::Skeleton const& s = pose_skeleton_live::At(skeleton);
    h.Root = s.HeldRoot;
    // Its own asset's name for the files (a table stands for several assets, whose bones are other lengths: a man's
    // file must not count as a woman in heels' own)
    if (g_Body.AssetOk && !pose_skeleton_live::IsMade(skeleton))
    {
      _snprintf_s(h.Asset, sizeof(h.Asset), _TRUNCATE, "%s", g_Body.Asset);
      _strlwr_s(h.Asset);
    }
    else
      _snprintf_s(h.Asset, sizeof(h.Asset), _TRUNCATE, "%s", s.Asset);
    // Held: the root and every bone below it (a bone's parent always comes
    // before it in the game's skeletons). The parents are the body's own pose
    // manager's, not the table's: they are what the game's pose pass goes by,
    // and a bone the body's animation rig has no channel for has none there
    // (the game draws it and what hangs on it apart from the body). Such a
    // bone is never listed: the game's blend would write its pose into
    // another bone's channel (pose_skeleton_live::MayPose).
    int face = BoneByName(skeleton, "Face");
    int leftHand = BoneByName(skeleton, "LeftHand"), rightHand = BoneByName(skeleton, "RightHand");
    // A hand's children are its fingers, and Wep_Root (the grip): that one isn't
    int grip = BoneByName(skeleton, "Wep_Root");
    for (int g = 0; g < FaceGroupCount; ++g)
      h.GroupSize[g] = 0;
    h.Count = 0;
    int leftOut = 0;
    char leftOutNames[240] = "";
    static bool byTable[MaxBones];
    for (int i = 0; i < h.Bones; ++i)
    {
      int p = g_Body.Parents[i];
      h.Parents[i] = (short)p;
      h.InHeld[i] = (i == h.Root || (p >= 0 && p < i && h.InHeld[p])) && pose_skeleton_live::MayPose(g_Body, i);
      // What the skeleton itself would have held and this body's rig can't: said in the log
      int tableParent = s.Parents[i];
      byTable[i] = i == h.Root || (tableParent >= 0 && tableParent < i && byTable[tableParent]);
      if (byTable[i] && !h.InHeld[i])
      {
        if (leftOut < 8)
        {
          size_t used = strlen(leftOutNames);
          _snprintf_s(leftOutNames + used, sizeof(leftOutNames) - used, _TRUNCATE, "%s%s", leftOut ? ", " : "", s.Names[i]);
        }
        ++leftOut;
      }
      h.Depth[i] = (unsigned char)(i == h.Root || p < 0 ? 0 : (h.Depth[p] < 250 ? h.Depth[p] + 1 : 250));
      h.Face[i] = i == face || (p >= 0 && h.Face[p]);
      h.Finger[i] = i != grip && p >= 0 && (p == leftHand || p == rightHand || h.Finger[p]);
      h.Helper[i] = IsHelperName(s.Names[i]);
      h.Eye[i] = !strcmp(s.Names[i], "LeftEye") || !strcmp(s.Names[i], "RightEye");
      // The body parts: one starts at each child of the held root (the spine,
      // each leg), at each shoulder, the neck, the face and each finger; any
      // other bone is in its parent's
      bool partStart = i == h.Root || p < 0 || p == h.Root || !strcmp(s.Names[i], "LeftShoulder") ||
        !strcmp(s.Names[i], "RightShoulder") || !strcmp(s.Names[i], "Neck") || i == face || (h.Finger[i] && !h.Finger[p]);
      h.Part[i] = (short)(partStart ? i : h.Part[p]);
      h.Group[i] = -1;
      h.FollowsGroup[i] = p >= 0 && (h.Group[p] >= 0 || h.FollowsGroup[p]);
      if (h.Face[i] && i != face && !h.FollowsGroup[i] && h.InHeld[i])
      {
        int g = FaceGroupOf(s.Names[i]);
        if (g >= 0 && h.GroupSize[g] < MaxGroupMembers)
        {
          h.Group[i] = (signed char)g;
          h.GroupMembers[g][h.GroupSize[g]++] = i;
        }
      }
      h.Map[i] = 0xFFFFFFFFu;
      if (h.InHeld[i])
      {
        h.Map[i] = (unsigned int)h.Count;
        h.List[h.Count++] = (unsigned int)i;
      }
    }
    if (!CopyFrom(body.Local, &h.Base[0][0], (size_t)h.Bones * FloatsPerBone))
    {
      h.Used = false;
      util::log::Warning("Pose: %s 0x%I64X: its local pose couldn't be read, not taken", kind, c.Entity);
      return;
    }
    for (int i = 0; i < h.Bones; ++i)
    {
      QNorm(h.Base[i] + 4);
      QIdentity(h.Turn[i]);
      h.Move[i][0] = h.Move[i][1] = h.Move[i][2] = 0.f;
    }
    __int64 allocator = (__int64)g_gameHandle + GameAllocatorRva;
    h.ListVector = { allocator, h.List, (unsigned int)h.Count, (unsigned int)h.Count };
    h.MapVector = { allocator, h.Map, (unsigned int)h.Bones, (unsigned int)h.Bones };
    for (int k = 0; k < 2; ++k)
    {
      h.Body[k] = { h.Buffer[k], &h.ListVector, &h.MapVector, 1.f, (unsigned int)h.Count, -1, 0 };
      h.Box[k] = { h.Buffer[k], &h.ListVector, &h.MapVector, 1.f, (unsigned int)h.Count, -1, 0 };
    }
    h.Next = 0;
    h.BodyTable = h.BoxTable = nullptr;
    h.HandedOk = false;
    h.WorldOk = false;
    h.Aside = h.Refused = h.BoxRefused = h.NoBoxLogged = false;
    h.Frames = h.Taken = h.FrozenFrames = h.FrozenTaken = 0;
    h.Worst = 0.f;
    h.CheckSince = GetTickCount64();
    h.CheckLine[0] = 0;
    h.Status[0] = 0;
    h.History.clear();
    h.Future.clear();
    h.HistoryHasKeys = false;
    h.FutureStash.clear();
    h.TrimmedStash = HistoryStep();
    h.TrimmedOk = h.StashOk = h.KeysTimeMoved = false;
    h.Keys = pose_keys::Anim();
    h.KeysAppliedTime = 0.0;
    h.KeysAppliedRevision = 0;
    h.KeysAppliedOk = true;
    h.KeysSavedRevision = 0;
    h.KeysSavedAt = 0;
    h.KeysAutosaveStarted = false;
    h.KeysFile.clear();
    h.KeysSuggest.clear();
    h.KeysLoads = 0;
    for (int i = 0; i < h.Bones; ++i)
    {
      QIdentity(h.KeyRefTurn[i]);
      h.KeyRefMove[i][0] = h.KeyRefMove[i][1] = h.KeyRefMove[i][2] = 0.f;
    }
    h.KeyTrackTimeBefore = -1.0;
    h.KeyTrackMoving = false;
    h.SelfHit = -1;
    h.IdleActive = false;
    ForgetIdleRefs(h);
    FindIdleBones(h);
    ResetIdleCounts(h);
    ResetKinematics(h);
    h.IdleWeight = 0.f;
    h.IdleWeightAt = h.IdleAt = h.IdleNewAt = 0.0;
    h.IdleGen = h.IdleSeen = 0;
    h.IdleFrames = 0;
    h.Restoring = false;
    h.RestoreSince = 0;
    h.RestoreWaitLogged = false;
    h.Clip = ClipPlay();
    voice_emotion::Cancel(h.Voice.Emotion);   // [EMOTION] [REVIEW] (none is expected here)
    h.Voice = VoicePlay();
    // Its slots' records watched from its first blend (see BlendWatch)
    if (!Watch(h))
      util::log::Warning("Pose: the game's pose blend isn't hooked, so letting go won't put back what the game doesn't "
        "animate (free-moved bones stay moved)");
    _snprintf_s(h.Label, sizeof(h.Label), _TRUNCATE, "%s", kind);
    // Its weapons: each on the bone it's on now (the pose is the game's yet).
    // In the grip's hand if it's near the grip, else with the nearest body
    // bone (a holster), else left where the game has it.
    h.GripBone = grip >= 0 && h.InHeld[grip] ? grip : -1;
    h.WeaponCount = 0;
    h.WeaponsCheckedAt = 0;
    char weaponText[200] = "";
    if (c.Kind == Kind_Person)
    {
      static float takeWorld[MaxBones][FloatsPerBone];
      bool worldOk = CopyFrom(body.World, &takeWorld[0][0], (size_t)h.Bones * FloatsPerBone);
      static HeldWeapon found[MaxWeapons];
      int count = worldOk ? FindWeapons(c.Entity, found, MaxWeapons) : 0;
      for (int k = 0; k < count; ++k)
      {
        HeldWeapon& w = found[k];
        WeaponName(w);
        float local[FloatsPerBone], world[FloatsPerBone];
        if (!WeaponPlace(w, c.Entity, local, world))
          continue;
        float nearest = FLT_MAX;
        if (grip >= 0 && h.InHeld[grip] && Distance3(takeWorld[grip], local) <= GripReach)
        {
          w.Bone = grip;
          nearest = Distance3(takeWorld[grip], local);
        }
        else
          for (int i = 0; i < h.Bones; ++i)
          {
            if (!h.InHeld[i] || h.Face[i] || h.Finger[i] || h.Helper[i])
              continue;
            float d = Distance3(takeWorld[i], local);
            if (d <= CarryReach && d < nearest)
            {
              nearest = d;
              w.Bone = i;
            }
          }
        char line[96];
        QIdentity(w.EditTurn);
        w.TakenBone = w.Bone;
        if (w.Bone >= 0)
        {
          Relative(takeWorld[w.Bone], local, w.OffsetLocal);
          Relative(takeWorld[w.Bone], world, w.OffsetWorld);
          memcpy(w.TakenOffsetLocal, w.OffsetLocal, sizeof(w.TakenOffsetLocal));
          memcpy(w.TakenOffsetWorld, w.OffsetWorld, sizeof(w.TakenOffsetWorld));
          _snprintf_s(line, sizeof(line), _TRUNCATE, "%s%s on %s (%.0f cm)", weaponText[0] ? ", " : "", w.Name,
            s.Names[w.Bone], nearest * 100.f);
        }
        else
          _snprintf_s(line, sizeof(line), _TRUNCATE, "%s%s left to the game (no bone within %.0f cm)",
            weaponText[0] ? ", " : "", w.Name, CarryReach * 100.f);
        strncat_s(weaponText, line, _TRUNCATE);
        h.Weapons[h.WeaponCount++] = w;
      }
    }
    g_Active = index;
    g_LetGoMessage[0] = '\0';
    g_SelectedBone = h.Root;
    g_SelectedGroup = -1;
    g_SelectedWeapon = -1;
    char boxText[64] = "";
    if (h.HasBox)
      sprintf_s(boxText, ", box pose slot 0x%X", h.BoxHandle);
    else if (c.Kind == Kind_Faith)
      strcpy_s(boxText, ", her box pose not found");
    util::log::Write("Pose: holding %s 0x%I64X (%s, %d bones, %d held from %s; pose slot 0x%X%s%s%s)", h.Label, h.Entity,
      h.Asset, h.Bones, h.Count, s.Names[h.Root], h.Handle, boxText, weaponText[0] ? "; its weapons: " : "", weaponText);
    if (leftOut)
      util::log::Write("Pose: %s 0x%I64X: %d bones below %s are left out of the pose, its animation rig has no channel "
        "for them or for a bone they hang on: %s%s", h.Label, h.Entity, leftOut, s.Names[h.Root], leftOutNames,
        leftOut > 8 ? ", ..." : "");
  }

  // ---- Idle movement ----
  //
  // A held pose replaces the character's animation (weight 1), so its idle
  // (breathing, small shifts, blinks) is gone. The game blends the override in
  // 0x142FCF620 (rdx: the pose job's item; r8: + 0x20 its skeleton-bone ->
  // Ant-record map): for each listed bone (item + 0x158, count + 0x14C) it
  // reads Ant's record (item + 0xA0 + map[bone] x 0x30: scale, rotation at +
  // 0x10, translation at + 0x20; the local layout antFbUpdateLocalPose turns
  // into the local pose) and writes the blend back over it (in place: the
  // same records are copied into the step and made into the local pose after
  // it; offline, 2026-11-09). So at its entry those records are the animation
  // as Ant made it this frame, under the pose (with time running Ant rewrites
  // them before each pose job). A hook there copies them, for the held poses
  // that want it (their override's bone buffer, item + 0x130, is one of the
  // held pose's two; the body's slot, item + 0x150, as Faith's box pose shares
  // the buffers), leaving out what the game didn't write (see BlendWatch: the
  // same hook puts that back when a pose is let go). It takes register
  // arguments only.
  const __int64 OverrideBlendRva = 0x2FCF620;
  struct IdleCodeCheck
  {
    __int64 Rva;
    BYTE Bytes[48];
    size_t Size;
  };
  const IdleCodeCheck IdleCodeChecks[] = {
    // Its start: the handle's -1 check, the records' null check
    { 0x2FCF620, { 0x48, 0x89, 0xE0, 0x57, 0x41, 0x55, 0x48, 0x81, 0xEC, 0xE8, 0x00, 0x00, 0x00, 0x83, 0xBA, 0x50, 0x01,
      0x00, 0x00, 0xFF, 0x4D, 0x89, 0xC5, 0x48, 0x89, 0xD7, 0x0F, 0x84, 0xB2, 0x04, 0x00, 0x00, 0x48, 0x83, 0xBA, 0xA0,
      0x00, 0x00, 0x00, 0x00, 0x0F, 0x84, 0xA4, 0x04, 0x00, 0x00 }, 46 },
    // The loop: bones + 0x130, list + 0x158, weight + 0x148, list[n], map + 0x20
    { 0x2FCF70B, { 0x48, 0x8B, 0xAF, 0x30, 0x01, 0x00, 0x00, 0x48, 0x8B, 0x87, 0x58, 0x01, 0x00, 0x00, 0xF3, 0x0F, 0x10,
      0xB7, 0x48, 0x01, 0x00, 0x00, 0x44, 0x89, 0xF3, 0x45, 0x0F, 0x28, 0xEE, 0x0F, 0xC6, 0xF6, 0x00, 0x8B, 0x0C, 0x98,
      0x49, 0x8B, 0x45, 0x20 }, 40 },
    // map[bone], then x 3 x 2 x 8 = 0x30 a record, the records + 0xA0
    { 0x2FCF743, { 0x8B, 0x04, 0x88 }, 3 },
    { 0x2FCF760, { 0x48, 0x8D, 0x34, 0x40, 0x48, 0x8B, 0x87, 0xA0, 0x00, 0x00, 0x00 }, 11 },
    { 0x2FCF77A, { 0x48, 0x01, 0xF6 }, 3 },
    // The record's rotation (+ 0x10) and translation (+ 0x20) read
    { 0x2FCF790, { 0x0F, 0x28, 0x5C, 0xF0, 0x10 }, 5 },
    { 0x2FCFA44, { 0x44, 0x0F, 0x5C, 0x64, 0xF0, 0x20 }, 6 },
    // Its only call (in the pose job): rdx = the item, r8 = the map
    { 0x2FCC990, { 0x4C, 0x8B, 0x8D, 0x78, 0x01, 0x00, 0x00, 0x48, 0x8B, 0x8D, 0x50, 0x01, 0x00, 0x00, 0x4C, 0x8D, 0x44,
      0x24, 0x40, 0x48, 0x8D, 0x55, 0x90, 0xE8, 0x74, 0x2C, 0x00, 0x00 }, 28 },
    // The map (rsp + 0x40) filled before it: the manager, [+ 0xB0] (the bone ->
    // record map), [+ 0x190], [+ 8] (its bones), [+ 0x88] (its records)
    { 0x2FCC8EF, { 0x48, 0x8B, 0x83, 0xB0, 0x00, 0x00, 0x00, 0x48, 0x8D, 0x4D, 0x90, 0x48, 0x89, 0x5C, 0x24, 0x40, 0x48,
      0x89, 0x44, 0x24, 0x60, 0x8B, 0x83, 0x90, 0x01, 0x00, 0x00, 0x89, 0x44, 0x24, 0x68, 0x8B, 0x43, 0x08, 0x89, 0x44,
      0x24, 0x6C, 0x8B, 0x83, 0x88, 0x00, 0x00, 0x00, 0x89, 0x44, 0x24, 0x70 }, 48 },
    // The item's override part: + 0x130 null and + 0x150 -1 first
    { 0x2FCC935, { 0x66, 0x0F, 0x7F, 0x85, 0xC0, 0x00, 0x00, 0x00 }, 8 },
    { 0x2FCC981, { 0xC7, 0x85, 0xE0, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF }, 10 },
    // The item filled: the slot's own data into + 0 (its records at + 0xA0),
    // the override (if any) into + 0x130
    { 0x2FD7C88, { 0x4A, 0x8B, 0x14, 0x10, 0x48, 0x8B, 0x42, 0x28, 0x49, 0x8B, 0x14, 0x01, 0x49, 0x8D, 0x80, 0x80, 0x00,
      0x00, 0x00, 0x48, 0x83, 0xEA, 0x80, 0x0F, 0x10, 0x42, 0x80 }, 27 },
    { 0x2FD7D7A, { 0x49, 0x8B, 0x0C, 0x01, 0x48, 0x85, 0xC9, 0x74, 0x3A, 0x0F, 0x10, 0x01, 0x41, 0x0F, 0x11, 0x80, 0x30,
      0x01, 0x00, 0x00 }, 20 } };
  const int ItemBonesOffset = 0x130, ItemListOffset = 0x158, ItemCountOffset = 0x14C, ItemHandleOffset = 0x150;
  const int ItemRecordsOffset = 0xA0, MapRecordsOffset = 0x20, RecordBytes = 0x30;
  const int MapManagerOffset = 0x00, MapBonesOffset = 0x2C, MapRecordCountOffset = 0x30;

  struct IdleCapture
  {
    std::atomic<unsigned int> Gen;           // which wanting it is for (0: not wanted)
    std::atomic<unsigned int> Seq;           // odd while it's written
    std::atomic<unsigned int> Count;         // captures made
    unsigned int CapturedGen;                // the Gen the records were taken for (written inside Seq)
    double At;                               // when (the performance counter, s; written inside Seq)
    float Records[MaxBones][12];             // per skeleton bone: scale, rotation, translation (Ant's)
    unsigned char Kept[MaxBones];            // per skeleton bone: 1 its rotation, 2 its translation not the game's (3: both, or not known)
  };
  IdleCapture g_IdleCaptures[MaxHeld];
  std::atomic<unsigned int> g_IdleGenNext{ 0 };
  typedef void(__fastcall* tOverrideBlend)(__int64, __int64, __int64, __int64);
  tOverrideBlend oOverrideBlend = nullptr;
  std::once_flag g_IdleHookOnce;
  bool g_IdleHooked = false;
  char g_IdleUnavailable[160] = "";

  // The performance counter in seconds, on any thread (the same clock as g_ClipCounterSeconds)
  double CounterSeconds()
  {
    static double const unit = []()
    {
      LARGE_INTEGER frequency;
      return QueryPerformanceFrequency(&frequency) && frequency.QuadPart ? 1.0 / (double)frequency.QuadPart : 0.0;
    }();
    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);
    return (double)counter.QuadPart * unit;
  }

  // A channel (4 floats) bit for bit the same
  bool SameBits(float const* a, float const* b)
  {
    return memcmp(a, b, 4 * sizeof(float)) == 0;
  }

  // Inside the pose job, before our blend of the body's slot: the held
  // pose's records copied for its idle movement, each bone's rotation and
  // translation the game didn't write since the slot's last pass marked (they
  // hold the tools' own pose). With none written, no capture. (Under Freeze
  // time the game still writes them every frame, CT.log 2026-11-14: a capture
  // a frame, probably with the same values, not measured; the movement then
  // eases out over IdleSmoothSeconds.) The game's memory is read before the
  // capture's seqlock opens (a fault can't leave it open).
  void CaptureIdle(IdleCapture& c, BlendWatch& w, BYTE const* records, unsigned int const* toRecord,
    unsigned int recordCount, int bones, bool seen)
  {
    unsigned int gen = c.Gen.load(std::memory_order_acquire);
    if (!gen)
      return;
    bool written = false;
    for (int n = 0; n < w.Count; ++n)
    {
      unsigned int bone = w.List[n];
      if (bone >= (unsigned int)bones || toRecord[bone] >= recordCount)
      {
        w.ScratchKept[bone] = 3;             // not known: no movement of its own
        continue;
      }
      float const* record = (float const*)(records + (size_t)toRecord[bone] * RecordBytes);
      memcpy(w.Scratch[bone], record, sizeof(w.Scratch[bone]));
      unsigned char kept = (unsigned char)(seen ? ((SameBits(record + 4, w.Seen[0][bone] + 4) ? 1 : 0) |
        (SameBits(record + 8, w.Seen[0][bone] + 8) ? 2 : 0)) : 0);
      w.ScratchKept[bone] = kept;
      written = written || kept != 3;
    }
    if (!written)
      return;
    c.Seq.fetch_add(1, std::memory_order_acq_rel);
    for (int n = 0; n < w.Count; ++n)
    {
      unsigned int bone = w.List[n];
      memcpy(c.Records[bone], w.Scratch[bone], sizeof(c.Records[bone]));
      c.Kept[bone] = w.ScratchKept[bone];
    }
    c.CapturedGen = gen;
    c.At = CounterSeconds();
    c.Seq.fetch_add(1, std::memory_order_release);
    c.Count.fetch_add(1, std::memory_order_release);
  }

  // A slot's pass with no override of ours: each held bone's channel that's
  // bit for bit as the slot's last pass left it (the game didn't write it
  // since) put back as the game last had it
  void PutBackChannels(BlendWatch& w, int k, BYTE* records, unsigned int const* toRecord, unsigned int recordCount,
    int bones)
  {
    int channels = 0, changed = 0;
    for (int n = 0; n < w.Count; ++n)
    {
      unsigned int bone = w.List[n];
      if (bone >= (unsigned int)bones || toRecord[bone] >= recordCount)
        continue;
      float* record = (float*)(records + (size_t)toRecord[bone] * RecordBytes);
      float const* seen = w.Seen[k][bone];
      float const* game = w.Game[k][bone];
      for (int part = 0; part < 12; part += 4)
      {
        if (!SameBits(record + part, seen + part) || SameBits(record + part, game + part))
          continue;
        float off = 0.f;
        for (int j = 0; j < 4; ++j)
        {
          float d = fabsf(record[part + j] - game[part + j]);
          off = (d > off || d != d) ? d : off;
        }
        memcpy(record + part, game + part, 4 * sizeof(float));
        ++channels;
        changed += !(off <= PutBackNoticed);
      }
    }
    w.Channels.fetch_add(channels, std::memory_order_relaxed);
    w.Changed.fetch_add(changed, std::memory_order_relaxed);
    w.Passes[k].fetch_add(1, std::memory_order_release);
  }

  // What a blend's pass is to a watch: 0 none of its slots', 2 one of its
  // slots' with our override, 3 one of its slots' without it (k: which slot;
  // AfterBlend follows both). Before the blend: what the game wrote since
  // the slot's last pass taken as its own; the idle capture (ours, the body);
  // the put back (not ours, if wanted)
  int SeeBlend(BlendWatch& w, int index, float const* overrideBones, int handle, BYTE* records, __int64 manager,
    unsigned int const* toRecord, unsigned int recordCount, int bones, int& k)
  {
    k = -1;
    bool ours = overrideBones && (overrideBones == w.Buffers[0] || overrideBones == w.Buffers[1]);
    if (ours)
    {
      k = handle == w.Handles[0] ? 0 : (w.Handles[1] != -1 && handle == w.Handles[1] ? 1 : -1);
      if (k < 0)
        return 0;
      __int64 known = w.Records[k].load(std::memory_order_relaxed);
      if (known && (known != (__int64)records || w.Manager[k].load(std::memory_order_relaxed) != manager))
      {
        // Its records somewhere else now: what was seen there doesn't hold here
        w.SeenOk[k].store(false, std::memory_order_release);
        w.RecordsMoved.fetch_add(1, std::memory_order_relaxed);
      }
      w.Records[k].store((__int64)records, std::memory_order_relaxed);
      w.Manager[k].store(manager, std::memory_order_relaxed);
      w.RecordCount[k].store(recordCount, std::memory_order_relaxed);
    }
    else
    {
      for (int s = 0; s < 2 && k < 0; ++s)
        if (w.SeenOk[s].load(std::memory_order_acquire) && w.Records[s].load(std::memory_order_relaxed) == (__int64)records &&
            w.Manager[s].load(std::memory_order_relaxed) == manager)
          k = s;
      if (k < 0)
        return 0;
    }
    bool seen = w.SeenOk[k].load(std::memory_order_acquire);
    if (!w.GameOk[k].load(std::memory_order_relaxed))
    {
      // The pose's first blend of the slot: the records are the game's own
      for (int n = 0; n < w.Count; ++n)
      {
        unsigned int bone = w.List[n];
        if (bone < (unsigned int)bones && toRecord[bone] < recordCount)
          memcpy(w.Game[k][bone], records + (size_t)toRecord[bone] * RecordBytes, sizeof(w.Game[k][bone]));
      }
      w.GameOk[k].store(true, std::memory_order_release);
    }
    else if (seen)
      for (int n = 0; n < w.Count; ++n)
      {
        unsigned int bone = w.List[n];
        if (bone >= (unsigned int)bones || toRecord[bone] >= recordCount)
          continue;
        float const* record = (float const*)(records + (size_t)toRecord[bone] * RecordBytes);
        for (int part = 0; part < 12; part += 4)
          if (!SameBits(record + part, w.Seen[k][bone] + part))
            memcpy(w.Game[k][bone] + part, record + part, 4 * sizeof(float));
      }
    if (ours && k == 0)
      CaptureIdle(g_IdleCaptures[index], w, records, toRecord, recordCount, bones, seen);
    if (!ours && seen && w.PutBack[k].load(std::memory_order_acquire))
      PutBackChannels(w, k, records, toRecord, recordCount, bones);
    return ours ? 2 : 3;
  }

  // Inside the pose job (a worker thread), before the game's blend: the watch
  // whose slot this pass is (still Busy: AfterBlend, then let go), or null
  BlendWatch* BeforeBlend(__int64 item, __int64 map, int& slot, BYTE*& records, unsigned int const*& toRecord,
    unsigned int& recordCount, int& bones)
  {
    float const* overrideBones = nullptr;
    int handle = -1;
    __int64 manager = 0;
    __try
    {
      overrideBones = *(float const* const*)(item + ItemBonesOffset);
      handle = *(int const*)(item + ItemHandleOffset);
      records = *(BYTE* const*)(item + ItemRecordsOffset);
      manager = *(__int64 const*)(map + MapManagerOffset);
      toRecord = *(unsigned int const* const*)(map + MapRecordsOffset);
      bones = *(int const*)(map + MapBonesOffset);
      recordCount = *(unsigned int const*)(map + MapRecordCountOffset);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      g_BlendFaults.fetch_add(1, std::memory_order_relaxed);
      return nullptr;
    }
    if (!records || !toRecord || manager < 0x10000 || bones <= 0 || bones > MaxBones || !recordCount ||
        recordCount > MaxRecords)
      return nullptr;
    for (int i = 0; i < MaxHeld; ++i)
    {
      BlendWatch& w = g_Watches[i];
      if (!w.On.load(std::memory_order_relaxed))
        continue;
      w.Busy.fetch_add(1, std::memory_order_seq_cst);
      int what = 0;
      if (w.On.load(std::memory_order_seq_cst))
      {
        __try
        {
          what = SeeBlend(w, i, overrideBones, handle, records, manager, toRecord, recordCount, bones, slot);
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
          g_BlendFaults.fetch_add(1, std::memory_order_relaxed);
          // Its slot's Seen isn't what the pass left now
          for (int s = 0; s < 2; ++s)
            if (w.Records[s].load(std::memory_order_relaxed) == (__int64)records)
              w.SeenOk[s].store(false, std::memory_order_release);
          what = 0;
        }
      }
      if (what >= 2)
        return &w;
      w.Busy.fetch_sub(1, std::memory_order_seq_cst);
    }
    return nullptr;
  }

  // After the game's blend of a watched slot: the records as the pass left
  // them, for the next one
  void AfterBlend(BlendWatch& w, int k, BYTE const* records, unsigned int const* toRecord, unsigned int recordCount,
    int bones)
  {
    for (int n = 0; n < w.Count; ++n)
    {
      unsigned int bone = w.List[n];
      if (bone < (unsigned int)bones && toRecord[bone] < recordCount)
        memcpy(w.Seen[k][bone], records + (size_t)toRecord[bone] * RecordBytes, sizeof(w.Seen[k][bone]));
    }
    w.SeenOk[k].store(true, std::memory_order_release);
  }

  void __fastcall hOverrideBlend(__int64 a, __int64 item, __int64 map, __int64 d)
  {
    BlendWatch* watch = nullptr;
    int slot = -1, bones = 0;
    BYTE* records = nullptr;
    unsigned int const* toRecord = nullptr;
    unsigned int recordCount = 0;
    if (g_Watched.load(std::memory_order_acquire) > 0)
      watch = BeforeBlend(item, map, slot, records, toRecord, recordCount, bones);
    oOverrideBlend(a, item, map, d);
    if (watch)
    {
      __try
      {
        AfterBlend(*watch, slot, records, toRecord, recordCount, bones);
      }
      __except (EXCEPTION_EXECUTE_HANDLER)
      {
        g_BlendFaults.fetch_add(1, std::memory_order_relaxed);
        watch->SeenOk[slot].store(false, std::memory_order_release);   // not what the pass left now
      }
      watch->Busy.fetch_sub(1, std::memory_order_seq_cst);
    }
  }

  bool IdleCodeOk()
  {
    __int64 base = (__int64)g_gameHandle;
    __try
    {
      for (IdleCodeCheck const& check : IdleCodeChecks)
        if (memcmp((void const*)(base + check.Rva), check.Bytes, check.Size) != 0)
        {
          sprintf_s(g_IdleUnavailable, "Not available: the game's pose blend at 0x%I64X isn't what the tools know",
            0x140000000 + check.Rva);
          return false;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      strcpy_s(g_IdleUnavailable, "Not available: the game's pose blend couldn't be read");
      return false;
    }
    return true;
  }

  // Made at the tools' start (pose_editor::Init, the init thread, at the end of Main::Init), or the first time it's
  // wanted if Init didn't run; made or not, it isn't tried again (std::call_once). Its code checked first.
  bool BlendHook()
  {
    std::call_once(g_IdleHookOnce, []
    {
      if (!IdleCodeOk())
      {
        util::log::Warning("Pose: %s; Idle movement is off, and Let go can't put bones back", g_IdleUnavailable);
        return;
      }
      g_IdleHooked = util::hooks::CreateGameHook("PoseOverrideBlend", (__int64)g_gameHandle + OverrideBlendRva,
        hOverrideBlend, (LPVOID*)&oOverrideBlend);
      if (!g_IdleHooked)
        strcpy_s(g_IdleUnavailable, "Not available: the game's pose blend couldn't be hooked (see CT.log)");
      util::log::Write("Pose: the game's pose blend %s (idle movement, putting bones back on Let go)", g_IdleHooked ?
        "hooked" : "not hooked");
    });
    return g_IdleHooked;
  }

  void IdleUnwanted(int index)
  {
    g_IdleCaptures[index].Gen.store(0, std::memory_order_release);
  }

  // Everything the idle movement has learnt of the animation. Wanted anew it starts from scratch, its gate shut: a
  // person's opens about 1 s later (IdleSpeedWindow of the game's time to know its root's speed, then IdleCalmSeconds
  // of calm), another skeleton's at the first capture.
  void ForgetIdleRefs(Held& h)
  {
    h.IdleRefOk = false;
    memset(h.IdleRefTurn, 0, sizeof(h.IdleRefTurn));
    memset(h.IdleRefMove, 0, sizeof(h.IdleRefMove));
    memset(h.IdlePrevOk, 0, sizeof(h.IdlePrevOk));
    h.IdleRootOk = false;
    h.IdleRootHead = h.IdleRootCount = 0;
    h.IdleSpinHead = h.IdleSpinCount = 0;
    h.IdleCaptureAt = 0.0;
    h.IdleGameClock = 0.0;
    h.IdleCalmSince = -1.0;
    h.IdleGateOpen = false;
  }

  // The 10 s log line's counts (UpdateIdle), from 0
  void ResetIdleCounts(Held& h)
  {
    h.IdleOpenCaptures = h.IdleOverSpeed = h.IdleOverLegs = h.IdleOverSpin = h.IdleOldRule = 0;
    h.IdleMostSpeed = h.IdleMostLegs = h.IdleMostSpin = h.IdleMostStep = 0.f;
    h.IdleMostLegsBone = h.IdleMostSpinBone = h.IdleMostStepBone = -1;
  }

  // The idle gate's bones by their names (at Take pose): Hips and the legs (group 1), the spine (group 2). A
  // skeleton without all ten keeps the 10-degree gate (IdlePeople false).
  void FindIdleBones(Held& h)
  {
    static char const* const names[Held::IdleSpinBones] = { "Hips", "LeftUpLeg", "LeftLeg", "LeftFoot", "RightUpLeg",
      "RightLeg", "RightFoot", "Spine", "Spine1", "Spine2" };
    memset(h.IdleGroup, 0, sizeof(h.IdleGroup));
    h.IdlePeople = true;
    for (int k = 0; k < Held::IdleSpinBones; ++k)
    {
      int bone = BoneByName(h.Skeleton, names[k]);
      h.IdleSpinBone[k] = bone >= 0 && bone < h.Bones && h.InHeld[bone] ? bone : -1;
      if (h.IdleSpinBone[k] < 0)
        h.IdlePeople = false;
      else
        h.IdleGroup[bone] = k < 7 ? 1 : 2;
    }
  }

  // Wanted for this pose; when it wasn't, a new wanting (its own Gen): only
  // captures made for it count, from scratch
  void IdleWant(Held& h, int index)
  {
    IdleCapture& c = g_IdleCaptures[index];
    if (c.Gen.load(std::memory_order_relaxed) != 0 && c.Gen.load(std::memory_order_relaxed) == h.IdleGen)
      return;
    c.Gen.store(0, std::memory_order_release);
    unsigned int gen = g_IdleGenNext.fetch_add(1, std::memory_order_relaxed) + 1;
    if (!gen)
      gen = g_IdleGenNext.fetch_add(1, std::memory_order_relaxed) + 1;
    h.IdleGen = gen;
    h.IdleSeen = c.Count.load(std::memory_order_acquire);
    ForgetIdleRefs(h);
    c.Gen.store(gen, std::memory_order_release);
  }

  // A turn's angle made amount times as big, then at most maxAngle (w kept >= 0)
  void ScaleTurn(float* q, float amount, float maxAngle)
  {
    if (q[3] < 0.f)
      for (int k = 0; k < 4; ++k)
        q[k] = -q[k];
    float s = sqrtf(q[0] * q[0] + q[1] * q[1] + q[2] * q[2]);
    if (!(s > 1e-7f))
    {
      QIdentity(q);
      return;
    }
    float angle = 2.f * atan2f(s, q[3]) * amount;
    if (angle > maxAngle)
      angle = maxAngle;
    float half = angle * 0.5f, k = sinf(half) / s;
    q[0] *= k; q[1] *= k; q[2] *= k;
    q[3] = cosf(half);
  }

  // A turn's angle (radians, 0 - pi)
  float TurnAngle(float const* q)
  {
    float s = sqrtf(q[0] * q[0] + q[1] * q[1] + q[2] * q[2]);
    return 2.f * atan2f(s, fabsf(q[3]));
  }

  // A turn's angle made amount times as big, then eased so that it never passes maxAngle: maxAngle x
  // tanh(angle / maxAngle). A bone whose animation goes far still moves, more slowly, instead of standing still at
  // a hard limit (w kept >= 0).
  void SoftTurn(float* q, float amount, float maxAngle)
  {
    if (q[3] < 0.f)
      for (int k = 0; k < 4; ++k)
        q[k] = -q[k];
    float s = sqrtf(q[0] * q[0] + q[1] * q[1] + q[2] * q[2]);
    if (!(s > 1e-7f) || !(maxAngle > 0.f))
    {
      QIdentity(q);
      return;
    }
    float angle = maxAngle * tanhf(2.f * atan2f(s, q[3]) * amount / maxAngle);
    float half = angle * 0.5f, k = sinf(half) / s;
    q[0] *= k; q[1] *= k; q[2] *= k;
    q[3] = cosf(half);
  }

  // Each frame for a held pose: the animation the game made under it (the
  // capture), smoothed, and each held bone's movement about that. It's laid
  // over the pose by a weight that goes to 1 in IdleFadeSeconds when wanted,
  // with captures coming, and the gate open, and to 0 the same way otherwise
  // (unticked, no capture for a while, walking, running, a big move of the
  // hips or legs). The gate (a person's: see IdleGateSpeed): shut when its root
  // moves, its hips or legs go far off where their animation has been or those
  // and its spine turn fast; open again after IdleCalmSeconds of all of them
  // calm. The smoothing, the speeds and the gate's windows go by the game's
  // time (IdleGameClock: the time between the captures' own stamps times the
  // game speed), so a slowed walk is still a walk; the fade goes by real time.
  void UpdateIdle(Held& h, int index, ULONGLONG now)
  {
    (void)now;
    double frame = g_ClipCounterSeconds;
    float dtWeight = h.IdleWeightAt > 0.0 ? (float)(frame - h.IdleWeightAt) : 0.f;
    h.IdleWeightAt = frame;
    dtWeight = dtWeight < 0.f ? 0.f : (dtWeight > 0.5f ? 0.5f : dtWeight);
    auto fade = [&](bool up)
    {
      float step = dtWeight / IdleFadeSeconds;
      h.IdleWeight = up ? (h.IdleWeight + step > 1.f ? 1.f : h.IdleWeight + step) :
        (h.IdleWeight - step < 0.f ? 0.f : h.IdleWeight - step);
      if (h.IdleWeight <= 0.f)
        h.IdleActive = false;
    };
    // (Also while the keyframes play, 2026-11-15, the user: "idle animation should be going on even when there are
    // key frames animating". The captures are the game's own animation under the pose, whatever the pose is, and
    // channels the game doesn't write hold the tools' pose and are left out (Kept) as at any time; a clip playing
    // still stops it: a whole animation of its own)
    bool want = g_IdleOn && g_IdleAmount > 0.f && !(h.Clip.On && h.Clip.Playing) && BlendHook();
    if (!want)
    {
      // No more captures; the last movement fades out
      IdleUnwanted(index);
      h.IdleGen = 0;
      ForgetIdleRefs(h);
      fade(false);
      return;
    }
    IdleWant(h, index);
    IdleCapture& c = g_IdleCaptures[index];
    unsigned int count = c.Count.load(std::memory_order_acquire);
    if (count == h.IdleSeen)
    {
      // Nothing new: kept, fading out after a while (the game doesn't blend this pose now). At a slow game speed
      // it waits at least 2.5 of the game's 30 a second ticks (under 1/6x; inferred: captures were measured only at
      // 1x, one a tick, and under Freeze time, one a frame).
      double stale = IdleStaleSeconds;
      if (g_ClipGameScale > 0.f && g_ClipGameScale < 1.f)
        stale = (std::max)(stale, 2.5 / (30.0 * (double)g_ClipGameScale));
      fade(h.IdleNewAt > 0.0 && frame - h.IdleNewAt < stale && h.IdleGateOpen);
      return;
    }
    unsigned int before = c.Seq.load(std::memory_order_acquire);
    if (before & 1u)
      return;
    static float records[MaxBones][12];
    static unsigned char kept[MaxBones];
    for (int n = 0; n < h.Count; ++n)
    {
      int i = (int)h.List[n];
      memcpy(records[i], c.Records[i], sizeof(records[i]));
      kept[i] = c.Kept[i];
    }
    unsigned int capturedGen = c.CapturedGen;
    double stamp = c.At;
    std::atomic_thread_fence(std::memory_order_acquire);
    if (c.Seq.load(std::memory_order_relaxed) != before)
      return;                                // written meanwhile: next frame
    h.IdleSeen = count;
    if (capturedGen != h.IdleGen)
      return;                                // made for another pose, or before this wanting
    // The game's time since the capture before (none: the first since it was wanted): the real time between their
    // stamps times the game speed (at least 0.0001: the game's own slow motion never goes under it; its real depth
    // isn't known), and at most one 30 a second tick a frame at any speed (the game doesn't run more: above 1x, or
    // under 30 frames a second); under Freeze time (speed 0) the real time: its captures then come every frame,
    // holding still
    float speedScale = g_ClipGameScale;
    if (!(speedScale > 0.f))
      speedScale = 1.f;
    else
    {
      if (speedScale < 0.0001f)
        speedScale = 0.0001f;
      if (g_ClipRealDt > 0.0)
      {
        float most = (float)(1.0 / (30.0 * g_ClipRealDt));
        speedScale = speedScale > most ? most : speedScale;
      }
    }
    float dt = h.IdleCaptureAt > 0.0 && stamp > h.IdleCaptureAt ? (float)(stamp - h.IdleCaptureAt) * speedScale : 0.f;
    h.IdleCaptureAt = stamp;
    h.IdleGameClock += dt;
    double at = h.IdleGameClock;
    h.IdleAt = frame;
    h.IdleNewAt = frame;
    float alpha = dt > 0.f ? 1.f - expf(-dt / IdleSmoothSeconds) : 0.f;
    float worst = 0.f;                       // the body's largest turn off where its animation has been
    float legs = 0.f;                        // Hips' and the legs' largest
    int legsBone = -1;
    float stepMost = 0.f;                    // the largest turn since the capture before (degrees)
    int stepBone = -1;
    float spin[Held::IdleSpinBones];         // the gate's bones' turn speeds since the capture before (degrees a second)
    for (int k = 0; k < Held::IdleSpinBones; ++k)
      spin[k] = 0.f;
    for (int n = 0; n < h.Count; ++n)
    {
      int i = (int)h.List[n];
      float const* r = records[i];
      float q[4] = { r[4], r[5], r[6], r[7] }, t[3] = { r[8], r[9], r[10] };
      float length = q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3];
      if (!(length > 0.9f && length < 1.1f) || !std::isfinite(t[0]) || !std::isfinite(t[1]) || !std::isfinite(t[2]))
      {
        QIdentity(h.Idle[i]);
        h.Idle[i][4] = h.Idle[i][5] = h.Idle[i][6] = 0.f;
        h.IdleRefTurn[i] = h.IdleRefMove[i] = false;
        h.IdlePrevOk[i] = false;
        continue;
      }
      QNorm(q);
      float* ref = h.IdleRef[i];
      // A rotation or translation the game didn't write since the pose's last
      // blend holds the tools' own pose (with the idle movement in it), not the
      // game's animation: no movement of its own (it would feed on itself), and
      // its Ref left as it is
      bool turnKept = (kept[i] & 1) != 0, moveKept = (kept[i] & 2) != 0;
      float turn[4];
      QIdentity(turn);
      if (!turnKept)
      {
        // How far it turned since the capture before
        if (h.IdlePrevOk[i])
        {
          float const* p = h.IdlePrev[i];
          float dot = fabsf(p[0] * q[0] + p[1] * q[1] + p[2] * q[2] + p[3] * q[3]);
          float step = 2.f * acosf(dot < 1.f ? dot : 1.f) * 57.2957795f;
          if (step > stepMost)
          {
            stepMost = step;
            stepBone = i;
          }
          if (h.IdleGroup[i] && dt > 0.f)
            for (int k = 0; k < Held::IdleSpinBones; ++k)
              if (h.IdleSpinBone[k] == i)
                spin[k] = step / dt;
        }
        memcpy(h.IdlePrev[i], q, sizeof(q));
        h.IdlePrevOk[i] = true;
        if (!h.IdleRefTurn[i])
        {
          memcpy(ref, q, sizeof(q));
          h.IdleRefTurn[i] = true;
        }
        else
        {
          // Towards now, by the smoothing (the shorter way round)
          float dot = ref[0] * q[0] + ref[1] * q[1] + ref[2] * q[2] + ref[3] * q[3];
          float sign = dot < 0.f ? -1.f : 1.f;
          for (int k = 0; k < 4; ++k)
            ref[k] += (q[k] * sign - ref[k]) * alpha;
          QNorm(ref);
        }
        // Its movement: ref^-1 * now in the bone's own frame
        float inverse[4];
        QConj(ref, inverse);
        QMul(inverse, q, turn);
        QNorm(turn);
        bool body = !h.Face[i] && !h.Finger[i] && !h.Helper[i] && !h.Eye[i];
        if (body || h.IdleGroup[i] == 1)
        {
          float angle = TurnAngle(turn);
          if (body)
            worst = angle > worst ? angle : worst;
          if (h.IdleGroup[i] == 1 && angle > legs)
          {
            legs = angle;
            legsBone = i;
          }
        }
        SoftTurn(turn, g_IdleAmount, MaxIdleTurn);
      }
      memcpy(h.Idle[i], turn, sizeof(turn));
      if (moveKept)
      {
        h.Idle[i][4] = h.Idle[i][5] = h.Idle[i][6] = 0.f;
        continue;
      }
      // The translation's, in its parent's frame
      if (!h.IdleRefMove[i])
      {
        memcpy(ref + 4, t, sizeof(t));
        h.IdleRefMove[i] = true;
      }
      else
        for (int k = 0; k < 3; ++k)
          ref[4 + k] += (t[k] - ref[4 + k]) * alpha;
      float move[3], length3 = 0.f;
      for (int k = 0; k < 3; ++k)
      {
        move[k] = (t[k] - ref[4 + k]) * g_IdleAmount;
        length3 += move[k] * move[k];
      }
      length3 = sqrtf(length3);
      float cut = length3 > MaxIdleMove ? MaxIdleMove / length3 : 1.f;
      for (int k = 0; k < 3; ++k)
        h.Idle[i][4 + k] = move[k] * cut;
    }

    // Its root (where it is: the bone the held root hangs from, as the game
    // drew it), kept with the game's time; its speed: from where it was
    // IdleSpeedWindow of the game's time ago (not known before it has been
    // watched that long). A skeleton whose held root has no parent, or a frame
    // with no world pose read: taken as still, as before 2026-11-14
    int base = h.Root >= 0 && h.Root < h.Bones ? h.Parents[h.Root] : -1;
    float speed = 0.f;
    bool speedKnown = base < 0 || !h.WorldOk;
    if (base >= 0 && h.WorldOk)
    {
      float const* p = h.World[base];
      int slot = (h.IdleRootHead + h.IdleRootCount) % Held::IdleRingSize;
      if (h.IdleRootCount == Held::IdleRingSize)
        h.IdleRootHead = (h.IdleRootHead + 1) % Held::IdleRingSize;
      else
        ++h.IdleRootCount;
      h.IdleRootTime[slot] = at;
      memcpy(h.IdleRootAt[slot], p, sizeof(h.IdleRootAt[slot]));
      memcpy(h.IdleRoot, p, sizeof(h.IdleRoot));
      h.IdleRootOk = true;
      // The newest place at least IdleSpeedWindow old
      for (int k = h.IdleRootCount - 2; k >= 0; --k)
      {
        int old = (h.IdleRootHead + k) % Held::IdleRingSize;
        double age = at - h.IdleRootTime[old];
        if (age >= IdleSpeedWindow)
        {
          speed = (float)(Distance3(p, h.IdleRootAt[old]) / age);
          speedKnown = true;
          break;
        }
      }
    }
    // Each of the gate's bones' mean turn speed over IdleSpinWindow; the fastest of them
    float spinMost = 0.f;
    int spinBone = -1;
    if (h.IdlePeople && dt > 0.f)
    {
      int slot = (h.IdleSpinHead + h.IdleSpinCount) % Held::IdleRingSize;
      if (h.IdleSpinCount == Held::IdleRingSize)
        h.IdleSpinHead = (h.IdleSpinHead + 1) % Held::IdleRingSize;
      else
        ++h.IdleSpinCount;
      h.IdleSpinTime[slot] = at;
      memcpy(h.IdleSpin[slot], spin, sizeof(spin));
    }
    if (h.IdlePeople && h.IdleSpinCount > 0)
    {
      float sum[Held::IdleSpinBones];
      for (int k = 0; k < Held::IdleSpinBones; ++k)
        sum[k] = 0.f;
      int samples = 0;
      for (int k = h.IdleSpinCount - 1; k >= 0; --k)
      {
        int s = (h.IdleSpinHead + k) % Held::IdleRingSize;
        if (samples > 0 && at - h.IdleSpinTime[s] > IdleSpinWindow)
          break;
        for (int b = 0; b < Held::IdleSpinBones; ++b)
          sum[b] += h.IdleSpin[s][b];
        ++samples;
      }
      for (int b = 0; b < Held::IdleSpinBones; ++b)
        if (sum[b] / (float)samples > spinMost)
        {
          spinMost = sum[b] / (float)samples;
          spinBone = h.IdleSpinBone[b];
        }
    }

    // The gate
    if (h.IdlePeople)
    {
      bool over = (speedKnown && speed > IdleGateSpeed) || legs > IdleGateLegs || spinMost > IdleGateSpin;
      bool calm = speedKnown && speed < IdleOpenSpeed && legs < IdleOpenLegs && spinMost < IdleOpenSpin;
      if (h.IdleGateOpen)
      {
        if (over)
        {
          h.IdleGateOpen = false;
          h.IdleCalmSince = -1.0;
        }
      }
      else if (!calm)
        h.IdleCalmSince = -1.0;
      else
      {
        if (h.IdleCalmSince < 0.0)
          h.IdleCalmSince = at;
        if (at - h.IdleCalmSince >= IdleCalmSeconds)
          h.IdleGateOpen = true;
      }
    }
    else
      h.IdleGateOpen = worst <= IdleGateTurn && (!speedKnown || speed <= IdleGateSpeed);

    // For the 10 s log line
    if (h.IdleGateOpen)
      ++h.IdleOpenCaptures;
    if (speedKnown && speed > IdleGateSpeed)
      ++h.IdleOverSpeed;
    if (legs > IdleGateLegs)
      ++h.IdleOverLegs;
    if (spinMost > IdleGateSpin)
      ++h.IdleOverSpin;
    if (worst > IdleGateTurn)
      ++h.IdleOldRule;
    if (speedKnown && speed > h.IdleMostSpeed)
      h.IdleMostSpeed = speed;
    if (legs > h.IdleMostLegs)
    {
      h.IdleMostLegs = legs;
      h.IdleMostLegsBone = legsBone;
    }
    if (spinMost > h.IdleMostSpin)
    {
      h.IdleMostSpin = spinMost;
      h.IdleMostSpinBone = spinBone;
    }
    if (stepMost > h.IdleMostStep)
    {
      h.IdleMostStep = stepMost;
      h.IdleMostStepBone = stepBone;
    }
    h.IdleRefOk = true;
    h.IdleActive = true;
    fade(h.IdleGateOpen);
    ++h.IdleFrames;
  }

  // The idle movement laid over a bone's handed local pose, by its weight
  void AddIdle(Held const& h, int bone, float* local)
  {
    if (!h.IdleActive || !(h.IdleWeight > 0.f))
      return;
    float turn[4];
    memcpy(turn, h.Idle[bone], sizeof(turn));
    ScaleTurn(turn, h.IdleWeight, MaxIdleTurn);
    float turned[4];
    QMul(local + 4, turn, turned);
    QNorm(turned);
    memcpy(local + 4, turned, sizeof(turned));
    for (int k = 0; k < 3; ++k)
      local[k] += h.Idle[bone][4 + k] * h.IdleWeight;
  }

  // ---- Animation clips (ClipPlay) ----

  // The clips' clocks this frame (set in Update)
  // (g_ClipRealDt: with the idle movement's settings, above)
  double g_ClipRealDtBefore = 0.0;           // the frame before's (KeepVoice: the camera track's time is a frame old)
  int g_ClipLongFrames = 0;                  // frames over 0.1 s in a row, this one counted
  // (g_ClipGameScale: with the idle movement's settings, above)
  double g_ClipTrackTime = -1.0;
  const wchar_t* const ClipFolderW = L"Cinematic Tools\\Animations";

  // A clip being read: one at a time, on a thread of its own
  struct ClipLoad
  {
    std::mutex Mutex;
    bool Done = false;
    std::shared_ptr<pose_clips::Clip const> Clip;
    std::string Error;
    int HeldId = 0;                          // the held pose it's for (Held::Id)
    std::string File;                        // its file's name (UTF-8)
  };
  std::shared_ptr<ClipLoad> g_ClipLoad;
  std::vector<std::wstring> g_ClipFiles;     // the folder's .fbx and .anim files
  ULONGLONG g_ClipFilesListedAt = 0;
  char g_ClipMessage[400] = "";

  std::string Utf8Of(std::wstring const& w)
  {
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n : 0, '\0');
    if (n > 0)
      WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
  }

  void ListClipFiles()
  {
    g_ClipFiles.clear();
    CreateDirectoryW(L"Cinematic Tools", nullptr);
    CreateDirectoryW(ClipFolderW, nullptr);
    WIN32_FIND_DATAW data;
    for (wchar_t const* ext : { L"\\*.fbx", L"\\*.anim" })
    {
      std::wstring pattern = std::wstring(ClipFolderW) + ext;
      HANDLE find = FindFirstFileW(pattern.c_str(), &data);
      if (find != INVALID_HANDLE_VALUE)
      {
        do
        {
          if (!(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
            g_ClipFiles.push_back(data.cFileName);
        } while (FindNextFileW(find, &data));
        FindClose(find);
      }
    }
    std::sort(g_ClipFiles.begin(), g_ClipFiles.end());
  }

  // A file read on a thread of its own (the menu and the game go on), handed
  // over by KeepClipLoad
  void StartClipLoad(Held const& h, std::wstring const& file)
  {
    if (g_ClipLoad)
      return;
    auto job = std::make_shared<ClipLoad>();
    job->HeldId = h.Id;
    job->File = Utf8Of(file);
    std::wstring path = std::wstring(ClipFolderW) + L"\\" + file;
    g_ClipLoad = job;
    _snprintf_s(g_ClipMessage, sizeof(g_ClipMessage), _TRUNCATE, "Reading %s...", job->File.c_str());
    std::thread([job, path]
    {
      SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
      std::string error;
      std::shared_ptr<pose_clips::Clip const> clip;
      try
      {
        clip = pose_clips::Load(path, error);
      }
      catch (...)
      {
        clip.reset();
        error = "couldn't be read (out of memory?)";
      }
      std::lock_guard<std::mutex> lock(job->Mutex);
      job->Clip = clip;
      job->Error = error;
      job->Done = true;
    }).detach();
  }

  // The clip faced the way the pose taken's Hips face (a turn about up): the
  // game rest Hips' front (+Z), or its left (+X) when that lies flatter,
  // carried by the pose taken's Hips. Taken lying or upside down (the rest's
  // up carried more than 60 deg off up): +X in bone 1's frame, the way people
  // face there (Faith's recorded median 90.3 deg, men 86 - 100; offline
  // 2026-11-11).
  void FaceClip(Held& h)
  {
    ClipPlay& c = h.Clip;
    QIdentity(c.Facing);
    if (h.Root < 0 || !c.Fit.Ok)
      return;
    float inverse[4], hips[4];
    QConj(c.Fit.RestHipsQ, inverse);
    memcpy(hips, h.Base[h.Root] + 4, sizeof(hips));
    QNorm(hips);
    float axes[2][3] = { { 0.f, 0.f, 1.f }, { 1.f, 0.f, 0.f } }, flattest = 0.f, yaw = 0.f;
    for (int a = 0; a < 2; ++a)
    {
      float local[3], now[3];
      QRot(inverse, axes[a], local);
      QRot(hips, local, now);
      float flat = sqrtf(now[0] * now[0] + now[2] * now[2]);
      if (flat > flattest)
      {
        flattest = flat;
        // The turn about up that takes the rest's +Z (or +X) onto now's way across the ground
        yaw = a == 0 ? atan2f(now[0], now[2]) : atan2f(-now[2], now[0]);
      }
    }
    // Lying or flipped when taken (the rest's up carried less than 60 deg from
    // up): +X in bone 1's frame
    float up[3] = { 0.f, 1.f, 0.f }, upLocal[3], upNow[3];
    QRot(inverse, up, upLocal);
    QRot(hips, upLocal, upNow);
    if (upNow[1] < 0.5f)
      yaw = 1.5707963f;
    c.Facing[1] = sinf(yaw * 0.5f);
    c.Facing[3] = cosf(yaw * 0.5f);
  }

  // A finished read handed to its held pose (under g_Mutex)
  void KeepClipLoad()
  {
    if (!g_ClipLoad)
      return;
    std::shared_ptr<pose_clips::Clip const> clip;
    std::string error, file;
    int heldId = 0;
    {
      std::lock_guard<std::mutex> lock(g_ClipLoad->Mutex);
      if (!g_ClipLoad->Done)
        return;
      clip = g_ClipLoad->Clip;
      error = g_ClipLoad->Error;
      file = g_ClipLoad->File;
      heldId = g_ClipLoad->HeldId;
    }
    g_ClipLoad.reset();
    if (!clip)
    {
      _snprintf_s(g_ClipMessage, sizeof(g_ClipMessage), _TRUNCATE, "%s %s", file.c_str(), error.c_str());
      util::log::Warning("Pose: the clip %s %s", file.c_str(), error.c_str());
      return;
    }
    util::log::Write("Pose: clip read: %s%s%s", clip->Report.c_str(), clip->Warning.empty() ? "" : "; check: ",
      clip->Warning.c_str());
    if (util::DiagnosticsOn())
    {
      std::string bones;
      for (size_t k = 0; k < clip->Bones.size(); ++k)
        bones += (k ? ", " : "") + clip->Bones[k] + " = " + clip->SourceNames[k];
      util::log::Write("Pose: clip %s's bones (found by %s): %s", file.c_str(), clip->BonesFrom.c_str(), bones.c_str());
    }
    Held* held = nullptr;
    for (int i = 0; i < MaxHeld && !held; ++i)
      if (g_Held[i].Used && !g_Held[i].Restoring && g_Held[i].Id == heldId)
        held = &g_Held[i];
    if (!held)
    {
      _snprintf_s(g_ClipMessage, sizeof(g_ClipMessage), _TRUNCATE, "%s was read, but its pose was let go meanwhile",
        file.c_str());
      return;
    }
    Held& h = *held;
    ClipPlay& c = h.Clip;
    pose_skeletons::Skeleton const& s = pose_skeleton_live::At(h.Skeleton);
    c.Clip = clip;
    pose_clips::FitTo(*clip, s.Asset, h.Bones, s.Names, s.Parents, &h.Base[0][0], c.Fit);
    c.Time = 0.0;
    c.TrackStart = 0.0;
    c.LocalOk = false;
    c.On = c.Playing = c.Fit.Ok;
    FaceClip(h);
    if (h.Root >= 0)
    {
      c.Anchor[0] = h.Base[h.Root][0];
      c.Anchor[1] = h.Base[h.Root][2];
    }
    c.TravelYawOk = false;
    c.TravelShift[0] = c.TravelShift[1] = 0.f;
    util::log::Write("Pose: clip %s on %s 0x%I64X: %s", file.c_str(), h.Label, h.Entity, c.Fit.Report.c_str());
    // (a clip that doesn't fit shows why in its own line in the Clip tab, from c.Fit.Report)
    g_ClipMessage[0] = 0;
  }

  // The clip's pose at its time, as locals (h.Clip.Local), each held bone as
  // the Fit has it (pose_clips::BoneUse): a bone the clip turns gets its world
  // rotation (faced) as a local under its parent as made, and the game rest's
  // translation (the Hips aside); a bone between two of those the game rest's
  // local; a roll its sibling's turn and their rest offset; the rest the pose
  // taken's local. The Hips across the ground where the pose taken has them,
  // at the game rest's height, moved as the clip's (only up and down unless
  // it follows the clip's travel), the travel along the way the body faces
  // (the clip's facing and the Hips' own turn's heading).
  void MakeClipPose(Held& h)
  {
    ClipPlay& c = h.Clip;
    static float world[MaxBones][4], rotation[MaxBones][4];
    float move[3];
    pose_clips::Sample(*c.Clip, c.Fit, c.Time, c.Loop, world, move);
    for (int n = 0; n < h.Count; ++n)
    {
      int i = (int)h.List[n], p = h.Parents[i];
      float parent[4] = { 0.f, 0.f, 0.f, 1.f };
      if (i != h.Root && p >= 0 && h.InHeld[p])
        memcpy(parent, rotation[p], sizeof(parent));
      float* local = c.Local[i];
      memcpy(local, h.Base[i], FloatsPerBone * sizeof(float));
      unsigned char use = c.Fit.Use[i];
      if (use == pose_clips::UseClip)
      {
        float w[4], inverse[4];
        QMul(c.Facing, world[i], w);
        QNorm(w);
        QConj(parent, inverse);
        QMul(inverse, w, local + 4);
        QNorm(local + 4);
        memcpy(rotation[i], w, sizeof(w));
        if (i != h.Root)
          memcpy(local, c.Fit.RestLocalT[i], 3 * sizeof(float));
        continue;
      }
      if (use == pose_clips::UseRest)
      {
        memcpy(local, c.Fit.RestLocalT[i], 3 * sizeof(float));
        memcpy(local + 4, c.Fit.RestLocalQ[i], 4 * sizeof(float));
      }
      QMul(parent, local + 4, rotation[i]);
      QNorm(rotation[i]);
    }
    // The rolls, after their siblings (they come first in the skeletons; nothing hangs from them)
    for (int n = 0; n < h.Count; ++n)
    {
      int i = (int)h.List[n], d = c.Fit.Follows[i];
      if (c.Fit.Use[i] != pose_clips::UseFollow || d < 0 || d >= h.Bones || !h.InHeld[d])
        continue;
      QMul(c.Local[d] + 4, c.Fit.FollowTurn[i], c.Local[i] + 4);
      QNorm(c.Local[i] + 4);
    }
    // Faith's twist bones: a share of their driver's turn from its rest (its
    // twist about X, or its Euler X), about their own X, on their rest local
    for (int n = 0; n < h.Count; ++n)
    {
      int i = (int)h.List[n], d = c.Fit.TwistOf[i];
      if (c.Fit.Use[i] != pose_clips::UseTwist || d < 0 || d >= h.Bones || !h.InHeld[d])
        continue;
      float inverse[4], q[4];
      QConj(c.Fit.RestLocalQ[d], inverse);
      QMul(inverse, c.Local[d] + 4, q);
      QNorm(q);
      float x = q[0], y = q[1], z = q[2], w = q[3], angle;
      if (c.Fit.TwistKind[i] == 0)
        angle = 2.f * atan2f(w < 0.f ? -x : x, fabsf(w));
      else if (c.Fit.TwistKind[i] == 1)
        angle = atan2f(2.f * (y * z + x * w), 1.f - 2.f * (x * x + y * y));
      else
        angle = atan2f(2.f * (x * w - y * z), 1.f - 2.f * (x * x + y * y));
      float half = 0.5f * c.Fit.TwistShare[i] * angle, turn[4] = { sinf(half), 0.f, 0.f, cosf(half) };
      memcpy(c.Local[i], c.Fit.RestLocalT[i], 3 * sizeof(float));
      QMul(c.Fit.RestLocalQ[i], turn, c.Local[i] + 4);
      QNorm(c.Local[i] + 4);
    }
    if (h.Root >= 0 && c.Fit.TargetHips == h.Root)
    {
      float faced[4], front[3] = { 0.f, 0.f, 1.f }, ahead[3], heading[4];
      memcpy(heading, c.Facing, sizeof(heading));
      QMul(h.Turn[h.Root], c.Facing, faced);
      QRot(faced, front, ahead);
      if (ahead[0] * ahead[0] + ahead[2] * ahead[2] > 1e-6f)
      {
        float yaw = atan2f(ahead[0], ahead[2]);
        heading[0] = heading[2] = 0.f;
        heading[1] = sinf(yaw * 0.5f);
        heading[3] = cosf(yaw * 0.5f);
      }
      float m[3] = { c.Hips == ClipTravel ? move[0] : 0.f, 0.f, c.Hips == ClipTravel ? move[2] : 0.f }, turned[3];
      QRot(heading, m, turned);
      // A new heading (the Hips turned) turns the travel from where the Hips
      // are, not about the clip's start: the shift keeps them there
      float yaw = 2.f * atan2f(heading[1], heading[3]);
      if (c.TravelYawOk && yaw != c.TravelYaw)
      {
        float was[4] = { 0.f, sinf(c.TravelYaw * 0.5f), 0.f, cosf(c.TravelYaw * 0.5f) }, before[3];
        QRot(was, m, before);
        c.TravelShift[0] += before[0] - turned[0];
        c.TravelShift[1] += before[2] - turned[2];
      }
      c.TravelYaw = yaw;
      c.TravelYawOk = true;
      float* t = c.Local[h.Root];
      t[0] = c.Anchor[0] + c.TravelShift[0] + turned[0];
      t[1] = c.Fit.RestHipsY + move[1];
      t[2] = c.Anchor[1] + c.TravelShift[1] + turned[2];
    }
    c.LocalOk = true;
  }

  // Each frame for a held pose (KeepHeld, before its locals are handed): its
  // clip's time moved on by its clock. False: no usable clip, so no pose to make.
  bool AdvanceClip(Held& h)
  {
    ClipPlay& c = h.Clip;
    if (!c.On || !c.Clip || !c.Fit.Ok)
    {
      c.LocalOk = false;
      return false;
    }
    double length = c.Clip->Length();
    if (c.Clock == ClipTrackTime)
    {
      // From Start as the track plays; at Start while no track plays
      if (c.Playing)
        c.Time = c.TrackStart + (g_ClipTrackTime >= 0.0 ? g_ClipTrackTime : 0.0) * c.Speed;
    }
    else if (c.Playing)
      c.Time += g_ClipRealDt * (c.Clock == ClipGameTime ? g_ClipGameScale : 1.0) * c.Speed;
    if (length > 0.0)
    {
      if (c.Loop)
      {
        c.Time = fmod(c.Time, length);
        if (c.Time < 0.0)
          c.Time += length;
      }
      else if (c.Time > length)
      {
        c.Time = length;
        if (c.Clock != ClipTrackTime)
          c.Playing = false;
      }
      else if (c.Time < 0.0)
        c.Time = 0.0;
    }
    else
      c.Time = 0.0;
    return true;
  }

  // The clip's frame now made the pose taken (Base): the clip off, the edits on
  // top of that frame (Reset goes back to it)
  void BakeClipFrame(Held& h)
  {
    ClipPlay& c = h.Clip;
    if (!c.On || !c.LocalOk)
      return;
    for (int n = 0; n < h.Count; ++n)
    {
      int i = (int)h.List[n];
      memcpy(h.Base[i], c.Local[i], sizeof(h.Base[i]));
    }
    c.On = c.Playing = c.LocalOk = false;
    _snprintf_s(g_ClipMessage, sizeof(g_ClipMessage), _TRUNCATE, "The frame at %.2f s is the pose now (the clip is off; "
      "Reset goes back to that frame)", c.Time);
    util::log::Write("Pose: %s 0x%I64X: the clip's frame at %.2f s made its pose", h.Label, h.Entity, c.Time);
  }

  // ---- Voices (VoicePlay) ----

  const wchar_t* const VoiceFolderW = L"Cinematic Tools\\Voices";
  // The kinds of sound file listed (what Windows' own decoders read; one it can't read says so when picked)
  // (.ogg, .oga and .opus are read only where Windows' Web Media Extensions are installed, .aiff not at all: they are
  // listed all the same, and say so when picked, rather than not showing)
  const wchar_t* const VoiceKinds[] = { L"\\*.wav", L"\\*.mp3", L"\\*.m4a", L"\\*.mp4", L"\\*.aac", L"\\*.flac", L"\\*.wma",
    L"\\*.ac3", L"\\*.mp2", L"\\*.webm", L"\\*.mka", L"\\*.ogg", L"\\*.oga", L"\\*.opus", L"\\*.aif", L"\\*.aiff" };
  const float MaxVoiceLead = 0.3f;           // s: the mouth ahead of (or, negative, behind) the sound at most this
  const double VoiceRateSeconds = 0.2;       // its time's pace is measured over about this long
  const ULONGLONG VoiceHandMs = 150;         // its own clock waits this long after the menu last held its time
  // A voice started from its beginning starts this far before it: the player needs 60 - 94 ms from the first call to
  // the first sample heard (measured offline), and the first syllable would be lost
  const double VoiceRunIn = 0.1;
  // (g_ClipCounterSeconds: with the idle movement's settings, above)
  // The Voice tab's (?) text
  char const* const VoiceHelp = "Plays a recorded line on the held character and moves its mouth with it. Put the sound "
    "file in the game folder's Cinematic Tools\\Voices and pick it here: WAV, MP3, M4A / AAC, FLAC, WMA or a video's "
    "sound track (what Windows itself can read; OGG and Opus only where Windows' Web Media Extensions are installed, "
    "AIFF not: convert those). Up to ten minutes are read. A damaged file can take 15 seconds to be given up "
    "on.\n\nThe mouth's movements are worked out from the sound alone when the file is read: the jaw from how loud "
    "the voice's vowels are, the lips from how it sounds. The tools don't know the words. Measured on computer-made "
    "test voices (recordings of people were measured only for how fast the mouth moves, and for whether it shuts on "
    "p, b and m: it does on about 83 % of them, but also on about 81 % of t, d, n, k, g and l): the jaw follows speech "
    "well (about one opening a syllable, fewer with Mouth smoothing raised; shut in pauses, on time), the wide and teeth shapes are fair, the rounded one is weak, and a p, b or m is only "
    "hinted at. The file should be the voice alone, with a short silence at its start and end: music, noise or a "
    "second voice under it move the mouth too. A loud clipped (overloaded) voice keeps its jaw movement but loses part "
    "of its lip closures and lip shapes, and a whisper opens the jaw about half as far, with weaker and partly wrong "
    "lip shapes.\n\nThe "
    "movements lie on top of the pose: the jaw, the lips and the mouth's corners keep your own edits and keyframes "
    "underneath. Works on Faith, KrugerSec and people with a full skeleton (on a character without those mouth bones "
    "only the sound plays). While a replay records or plays, every held pose steps aside and its mouth with it; the "
    "voice's sound still plays.\n\nThe "
    "sound is played by Windows beside the game's own, in step with the voice's time: it isn't placed in the world, "
    "the game's volume settings don't change it, and it's heard only while the game's window is in front.";

  // The (?) text of "A living face"
  // (its numbers: VoiceAlive.h / .cpp, VoiceLife.h, VoiceLifeFace.h, VoiceGaze.h; Research_Pose_Editor sections 30, 31)
  char const* const VoiceLifeHelp = "A living face, worked out from the sound's loudness and pitch when the file is "
    "read and made for this character (a take of its own), so it stays in step when you scrub, loop or slow the "
    "voice, and is the same every time for the same take.\n\n"
    "Eyes: they look at the listener (where the eyes are posed) with small movements about the listener's face, look "
    "away for a moment at the start of phrases and now and then inside long ones, and are back before the line ends. "
    "While the voice moves the head they keep looking at the same place, as real eyes do, and dip a little in a "
    "blink. The upper eyelids follow the eyes' movements up and down (eyes you turn yourself are left with the lids "
    "you give them).\n\n"
    "Blinks: about 20 to 30 a minute while speaking (studies of people talking: 26 on average), each eye its own and "
    "each blink a little different: some only part of the way, a few long or two in a row, drawn to the ends of "
    "phrases, to large eye movements and to strong nods. Closing and opening take as long as the game's own blinks.\n\n"
    "Brows: a raise on the syllables that stand out most by their pitch, each its own size and the two sides a little "
    "apart, small flickers on other stressed syllables, and a slow movement that never quite stops. They draw a "
    "little together where the voice pauses to think or stresses a word loudly and low.\n\n"
    "Breathing: a breath in before each group of words (the lips part a little), slow breathing in silence, and a "
    "small lift of the head with each breath.\n\n"
    "Head: nods on stressed syllables, a dip of the chin into the end of a phrase, a slow drift that never quite "
    "stops, a small share of each look away. A part of it goes to the upper neck.\n\n"
    "It hears stress, not words: what is louder, higher or longer than the syllables around it. Silence, noise, "
    "held notes or beats get blinks, glances, breathing and the head's slow drift only (no brow raises or nods). "
    "Music, noise or a hum under the voice can put brow "
    "raises and nods in wrong places; singing is taken for speech.\n\n"
    "The numbers come from studies of people talking and from the game's own animation, and were checked on "
    "computer-made voices and one recording of a person.\n\n"
    "Your own edits and keyframes of the eyes, the eyelids, the eyebrows, the head and the neck stay underneath. "
    "Works on Faith, KrugerSec and people with full face bones. Like the mouth, it steps aside while a replay records "
    "or plays.";

  // The (?) text of "Feeling"
  // (VoiceFeel.h / .cpp, VoiceFeelFace.h, VoiceFeelTables.h; Research_Pose_Editor section 31)
  char const* const VoiceFeelHelp = "A feeling on the face, made of two of the game's own face pose libraries (the "
    "poses the game animates faces with): Faith's for Faith and the woman's skeleton (women in flat shoes), Gabriel's "
    "for everyone else, women in heels included (the game gives most other characters a library of their own, not "
    "used here). They are put together the way psychologists describe expressions (FACS: a smile is the cheeks and "
    "the mouth's corners up; sadness the inner brows up and the corners down; anger the brows down and together, the "
    "lids tight, the lips pressed).\n\n"
    "The tools' own analysis of the voice (its loudness and pitch) can't tell which feeling it carries, so you pick "
    "it, and that analysis only changes how strongly it shows (Follows the voice). With From the voice ticked, a "
    "trained model (an optional download: see its (?)) picks the feeling stretch by stretch over the one picked, and "
    "makes its own lively feelings a little stronger or weaker by how stirred up the voice is.\n\n"
    "Happy, Sad, Afraid, Surprised, Angry, Disgusted and Contempt follow the research's lists; Calm, Tender, Worried "
    "and Tense are designed. Some hold the head too: Sad lowers the chin about 3 degrees, Tender tilts the head 2, "
    "Afraid and Surprised lift the chin about a degree, Contempt and Happy a little less (Strength changes it, the "
    "Head slider doesn't). "
    "Sad, Worried, Angry, Tense and Disgusted hold back the voice's brow raises (Angry keeps 30 % of them). Surprised "
    "drops the jaw a little (about 3 degrees in a pause) and Afraid parts the lips (about 2), under the speech's own jaw "
    "and scaled by Mouth opening as it is; where the lips close for p, b and m that jaw goes (with Lip closing at 0, p, "
    "b and m left out, it stays). A blink "
    "still shuts over any feeling, and the lips still close for p, b and m.\n\n"
    "One feeling for the whole voice (From the voice: one for each stretch of it): it isn't keyed on the timeline (another picked while it plays comes in over "
    "about half a second from then on, but nothing is recorded).\n\n"
    "Faith's own smile pose turns the mouth's "
    "corners much more than Gabriel's (41 degrees at its fullest, against 16), and women on the woman's skeleton take "
    "Faith's poses too: if it looks too much, lower the strength. Works on Faith, KrugerSec and people with full face "
    "bones.";

  // [SMILE] The (?) text of "Smile comes and goes" (VoiceFeel.h: SmileTimes, SmileFactor)
  char const* const VoiceSmileHelp = "How much a smile (Happy's, Tender's, a little of Calm's, and the voice's own Happy "
    "with From the voice) comes and goes with the speech, instead of being held at its fullest all through. At 1 it "
    "rises a little before the speech, is at its fullest for 4 s at most, then eases to 85 % over a second while the "
    "speech goes on; a phrase that begins after that brings it back up; after the speech (a pause of a second or "
    "more) it eases down to half over three quarters of a second. 0: held at its fullest all through.\n\n"
    "Why: smiles that are felt last about half a second to 4 s and come and go gradually, and in a study of smiles on "
    "made-up faces a smile held at its fullest about 5 s looked more fake than a short one (Krumhuber and Kappas "
    "2005). Those were smiles alone, with no speech; the times and levels here are designed within what it found. "
    "Contempt's one-sided smirk isn't counted as a smile and stays as it is.";

  // [EMOTION] The (?) texts of "From the voice" and of its sureness (VoiceEmotion.h / .cpp, EmotionHelper.exe;
  // Research_Pose_Editor section 32)
  char const* const VoiceEmotionHelp = "The feeling read from the voice itself, stretch by stretch (each phrase; one "
    "over 6 s in parts of 3 - 4.5 s), over the feeling picked. A trained model hears it: audeering's wav2vec2 "
    "emotion model, with a small part trained on the RAVDESS actors' recordings, gives the chances of 8 feelings: "
    "neutral, calm, happy, sad, angry, fearful, disgust and surprised, shown as Calm, Happy, Sad, Angry, Afraid, "
    "Disgusted and Surprised (neutral keeps the feeling picked; Tender, Worried, Tense and Contempt are only ever "
    "set by hand). A phrase under 3 s is heard with the voice around it, 3 s in all. A short phrase that begins inside "
    "what the phrases before it are heard as joins them: together they are heard as one (3 s around them, or their "
    "whole span once that is longer, under 6 s) and share its reading, each keeping its own row and setting in the "
    "list (heard apart, nearly the same 3 s could be heard as two different feelings). A short phrase that begins later "
    "starts a window of its own, which the next ones can join; a phrase of 3 s or more is heard by itself.\n\n"
    "It is an optional download (about 660 MB; CC BY-NC-SA 4.0, for non-commercial use only, and its makers ask that "
    "it be used for research only): EmotionHelper.exe and emotion_feel.onnx in Cinematic Tools\\EmotionModel. It "
    "runs outside the game at idle priority on 2 CPU threads, once for each voice, with about 1.7 GB of memory while "
    "it runs. It takes about 1.2 s to start, then about 1.5 - 1.9 s for each 10 s it hears with the game closed (up "
    "to about 2.9 s while the game runs; times vary with the PC): the time goes by the seconds it hears. It doesn't "
    "start while Windows has under about 5.7 GB of memory "
    "left to give, and stops if that falls under 2 GB (the game ends itself when Windows has none left): Try again "
    "then. What it heard is kept in EmotionModel\\cache, so the same voice is read once (and taken at once, even while "
    "another voice is being read).\n\n"
    "How right it is, measured on short acted studio recordings it never heard, one whole recording at a time (each "
    "actor saying the same few sentences): English actors 77 % (about 9 in 10 when it is 80 % sure or more), German "
    "actors 54 %. How right it is on game dialogue or your own recordings isn't known. Its most common slips "
    "over both: sad heard as calm, fearful as surprised, angry as happy. So a stretch's feeling shows only as far as "
    "the model is sure of it and the rest of the face keeps the feeling picked (Sureness needed); any stretch can be "
    "set by hand in the list below. The voice's own Happy, Angry, Afraid, Surprised and Disgusted show a little "
    "stronger (up to 1.15 times) when it is more stirred up, a little weaker (down to 0.85) when it is calmer (the "
    "feeling picked under them as it is).\n\n"
    "The face changes over about half a second around where a stretch begins (and across a loop's end, for a voice "
    "of 0.6 s or more), a stretch "
    "set by hand or the sureness changed comes in over the same time, and the same time in the voice always gives "
    "the same face (scrubbed, looped, played slow).";
  char const* const VoiceSureHelp = "The model's chance (0 - 1) at which a stretch's feeling starts to show; it is "
    "all there 0.4 above it, or at a chance of 1 where that comes first (so a stretch the model is fully sure of, a "
    "chance of 1, always shows fully; from 0.60 up only that one does). Lower: more of the voice's feelings show, more of them wrong. Measured on English actors it never heard, "
    "the mean over its 8 feelings (a neutral voice keeping the feeling picked counts as right): at 0.30, 72 % of the "
    "face shows the right feeling and 13 % a wrong one; at 0.45, 65 % and 8 %; at 0.55, 57 % and 5 %; at 0.70, 48 % "
    "and 3 %; at 0.95, 26 % and 1 %; the rest keeps the feeling picked. On German actors: at 0.55, 44 % and 18 %; at "
    "0.70, 38 % and 11 %.";

  // A sound file being read: one at a time, on a thread of its own
  struct VoiceLoad
  {
    std::mutex Mutex;
    bool Done = false;
    std::shared_ptr<voice::Sound const> Sound;
    std::string Error;
    int HeldId = 0;                          // the held pose it's for (Held::Id)
    std::string File;                        // its file's name (UTF-8)
  };
  std::shared_ptr<VoiceLoad> g_VoiceLoad;
  std::vector<std::wstring> g_VoiceFiles;    // the folder's sound files
  ULONGLONG g_VoiceFilesListedAt = 0;
  char g_VoiceMessage[400] = "";

  void ListVoiceFiles()
  {
    g_VoiceFiles.clear();
    CreateDirectoryW(L"Cinematic Tools", nullptr);
    CreateDirectoryW(VoiceFolderW, nullptr);
    WIN32_FIND_DATAW data;
    for (wchar_t const* kind : VoiceKinds)
    {
      std::wstring pattern = std::wstring(VoiceFolderW) + kind;
      HANDLE find = FindFirstFileW(pattern.c_str(), &data);
      if (find != INVALID_HANDLE_VALUE)
      {
        do
        {
          if (!(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
            g_VoiceFiles.push_back(data.cFileName);
        } while (FindNextFileW(find, &data));
        FindClose(find);
      }
    }
    std::sort(g_VoiceFiles.begin(), g_VoiceFiles.end());
    // (a pattern of three letters also finds longer endings that begin with them: each file once)
    g_VoiceFiles.erase(std::unique(g_VoiceFiles.begin(), g_VoiceFiles.end()), g_VoiceFiles.end());
  }

  // A file read on a thread of its own (the menu and the game go on), handed over by KeepVoiceLoad
  void StartVoiceLoad(Held const& h, std::wstring const& file)
  {
    if (g_VoiceLoad)
      return;
    auto job = std::make_shared<VoiceLoad>();
    job->HeldId = h.Id;
    job->File = Utf8Of(file);
    std::wstring path = std::wstring(VoiceFolderW) + L"\\" + file;
    g_VoiceLoad = job;
    _snprintf_s(g_VoiceMessage, sizeof(g_VoiceMessage), _TRUNCATE, "Reading %s...", job->File.c_str());
    std::thread([job, path]
    {
      SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
      std::string error;
      std::shared_ptr<voice::Sound const> sound;
      try
      {
        sound = voice::Load(path, error);
      }
      catch (...)
      {
        sound.reset();
        error = "couldn't be read (out of memory?)";
      }
      std::lock_guard<std::mutex> lock(job->Mutex);
      job->Sound = sound;
      job->Error = error;
      job->Done = true;
    }).detach();
  }

  // What the player has to say (from its own thread), into CT.log
  void VoicePlayerLog(const char* text)
  {
    util::log::Write("Pose: the voices' player: %s", text ? text : "");
  }

  // The voice taken off a held pose: its sound stops
  void RemoveVoice(Held& h)
  {
    if (h.Voice.Player >= 0)
      voice_player::Close(h.Voice.Player);
    voice_emotion::Cancel(h.Voice.Emotion);   // [EMOTION] (a reading still going is stopped)
    h.Voice = VoicePlay();
  }

  // A finished read handed to its held pose (under g_Mutex)
  void KeepVoiceLoad()
  {
    if (!g_VoiceLoad)
      return;
    std::shared_ptr<voice::Sound const> sound;
    std::string error, file;
    int heldId = 0;
    {
      std::lock_guard<std::mutex> lock(g_VoiceLoad->Mutex);
      if (!g_VoiceLoad->Done)
        return;
      sound = g_VoiceLoad->Sound;
      error = g_VoiceLoad->Error;
      file = g_VoiceLoad->File;
      heldId = g_VoiceLoad->HeldId;
    }
    g_VoiceLoad.reset();
    if (!sound)
    {
      _snprintf_s(g_VoiceMessage, sizeof(g_VoiceMessage), _TRUNCATE, "%s: %s", file.c_str(), error.c_str());
      util::log::Warning("Pose: the voice %s: %s", file.c_str(), error.c_str());
      return;
    }
    util::log::Write("Pose: voice read: %s%s%s", sound->Report.c_str(), sound->Warning.empty() ? "" : "; check: ",
      sound->Warning.c_str());
    if (util::DiagnosticsOn() && !sound->MouthNotes.empty())
      util::log::Write("Pose: voice %s: its mouth analysis: %.900s", file.c_str(), sound->MouthNotes.c_str());
    Held* held = nullptr;
    for (int i = 0; i < MaxHeld && !held; ++i)
      if (g_Held[i].Used && !g_Held[i].Restoring && g_Held[i].Id == heldId)
        held = &g_Held[i];
    if (!held)
    {
      _snprintf_s(g_VoiceMessage, sizeof(g_VoiceMessage), _TRUNCATE, "%s was read, but its pose was let go meanwhile",
        file.c_str());
      return;
    }
    Held& h = *held;
    // The new voice keeps the old one's settings (its clock, its amounts): another take of the same line
    VoicePlay keep = h.Voice;
    bool had = keep.Sound != nullptr;
    // [REVIEW] The same sound read again (its samples the same: compared on the game's thread, measured offline at
    // about 0.5 - 1.5 ms for a minute of 44.1 kHz stereo, 7 - 15 ms for ten, about 20 ms at the decoder's 128M-sample
    // cap). [CARRY] Its feeling's reading under way (its helper's turn come) or done, not taken yet, goes on: not
    // stopped by the Remove below and begun again from nothing. [REVIEW] Not one still waiting for its turn: it holds
    // the earlier read's own samples, a second copy of them (up to about 256 MB) for as long as another voice's
    // reading runs, and it has heard nothing yet (stopped, it is begun again on this read's sound, the same key)
    bool samePcm = had && keep.Sound->File == sound->File && keep.Sound->Pcm && sound->Pcm &&
      keep.Sound->Rate == sound->Rate && keep.Sound->Channels == sound->Channels && *keep.Sound->Pcm == *sound->Pcm;
    bool carryReading = false;
    if (samePcm && keep.Emotion && !keep.EmotionReadings)
    {
      voice_emotion::State state = voice_emotion::StatusOf(*keep.Emotion).Now;
      carryReading = state == voice_emotion::Running || state == voice_emotion::Done;
      if (carryReading)
        h.Voice.Emotion.reset();
    }
    RemoveVoice(h);
    VoicePlay& v = h.Voice;
    if (had)
    {
      v.Loop = keep.Loop;
      v.Clock = keep.Clock;
      v.Start = keep.Start;
      v.Heard = keep.Heard;
      v.Volume = keep.Volume;
      v.Jaw = keep.Jaw;
      v.Lips = keep.Lips;
      v.Closure = keep.Closure;
      v.Smoothing = keep.Smoothing;
      v.Life = keep.Life;
      v.Blinks = keep.Blinks;
      v.Brows = keep.Brows;
      v.Head = keep.Head;
      v.Lead = keep.Lead;
      // [ALIVE]
      v.Take = keep.Take;
      v.LookAway = keep.LookAway;
      v.Eyes = keep.Eyes;
      v.HoldGaze = keep.HoldGaze;
      v.Feel = keep.Feel;
      v.FeelStrength = keep.FeelStrength;
      v.FeelFollow = keep.FeelFollow;
      v.FeelUneven = keep.FeelUneven;
      v.FeelLeftStronger = keep.FeelLeftStronger;
      v.FeelComes = keep.FeelComes;   // [SMILE]
      v.FeelAuto = keep.FeelAuto;   // [EMOTION]
      v.FeelSure = keep.FeelSure;
    }
    else
      v.Take = 1 + (h.Id > 0 ? (h.Id - 1) % 99 : 0);   // [ALIVE] (two characters given the same line get takes of their own)
    // [ALIVE] The new voice's face starts with the feeling picked as it is, not blended in from none
    for (int f = 0; f <= voice_feel::FeelingCount; ++f)
      v.FeelWeights[f] = f == (v.Feel >= 0 && v.Feel < voice_feel::FeelingCount ? v.Feel + 1 : 0) ? 1.f : 0.f;
    v.FeelStrengthNow = v.FeelStrength;
    v.Sound = sound;
    voice_feel::SmileTimesOf(sound->LifeDebug, v.SmileTimes);   // [SMILE]
    // [EMOTION] Its stretches for the feeling read from the voice (the analysis' phrases, long ones in parts); the
    // stretches set by hand are kept when the same file is read again and gives the same stretches
    {
      double seconds = sound->Pcm && sound->Channels > 0 && sound->Rate > 0 ?
        (double)(sound->Pcm->size() / (size_t)sound->Channels) / sound->Rate : 0.0;
      v.EmotionStretches = voice_emotion::StretchesOf(sound->LifeDebug.PhraseFrom, sound->LifeDebug.PhraseTo, seconds);
      bool same = had && keep.Sound && keep.Sound->File == sound->File &&
        keep.EmotionStretches.size() == v.EmotionStretches.size() && keep.FeelSetting.size() == v.EmotionStretches.size();
      for (size_t i = 0; same && i < v.EmotionStretches.size(); ++i)
        same = keep.EmotionStretches[i].From == v.EmotionStretches[i].From &&
          keep.EmotionStretches[i].To == v.EmotionStretches[i].To;
      v.FeelSetting = same ? keep.FeelSetting : std::vector<int>(v.EmotionStretches.size(), voice_emotion::FromVoice);
      // [REVIEW] The same sound read again carries its reading, the voice's share and the looks as shown over: no
      // jump, no new reading
      bool sameSound = same && samePcm;
      if (sameSound && keep.EmotionReadings && keep.EmotionReadings->size() == v.EmotionStretches.size())
      {
        v.Emotion = keep.Emotion;
        v.EmotionReadings = keep.EmotionReadings;
        v.FeelAutoNow = keep.FeelAutoNow;
        v.FeelLooks = keep.FeelLooks;
        v.FeelLooksFor = keep.FeelLooksFor;
      }
      // [CARRY] ... and a reading still going, for the same stretches (else it is stopped now)
      if (carryReading)
      {
        bool sameHeard = sameSound;
        for (size_t i = 0; sameHeard && i < v.EmotionStretches.size(); ++i)
          sameHeard = keep.EmotionStretches[i].HeardFrom == v.EmotionStretches[i].HeardFrom &&
            keep.EmotionStretches[i].HeardTo == v.EmotionStretches[i].HeardTo;
        if (sameHeard)
          v.Emotion = keep.Emotion;
        else
          voice_emotion::Cancel(keep.Emotion);
      }
    }
    pose_skeletons::Skeleton const& s = pose_skeleton_live::At(h.Skeleton);
    // The mouth's bones, where the pose taken has them (all of them held, or none are moved)
    bool rig = voice_face::FindRig(h.Bones, s.Names, h.Parents, &h.Base[0][0], v.Rig);
    if (rig)
    {
      int const* bones[] = { &v.Rig.Jaw, &v.Rig.Lower[0], &v.Rig.Lower[1], &v.Rig.Lower[2], &v.Rig.Upper[0], &v.Rig.Upper[1],
        &v.Rig.Upper[2], &v.Rig.Corner[0], &v.Rig.Corner[1] };
      for (int const* bone : bones)
        if (*bone < 0 || *bone >= h.Bones || !h.InHeld[*bone])
          rig = false;
      if (!rig)
        v.Rig.Ok = false;
    }
    // The eyelids, the eyebrows and the head: each part by itself, and a part only if every one of its bones is held
    // (a bone this body's rig has no channel for stays the game's: with one lid bone of four the blink would be a
    // wink, with five brow bones of six a lopsided raise). The fold bones and Neck1 are extras: left out one by one.
    voice_face::LifeRig& r = v.LifeRig;
    bool life = voice_face::FindLifeRig(h.Bones, s.Names, h.Parents, &h.Base[0][0], r);
    if (life)
    {
      auto held = [&](int bone) { return bone >= 0 && bone < h.Bones && h.InHeld[bone]; };
      r.Eyes = r.Eyes && held(r.UpLid[0]) && held(r.UpLid[1]) && held(r.LowLid[0]) && held(r.LowLid[1]);
      for (int k = 0; k < 6; ++k)
        r.Brows = r.Brows && held(r.Brow[k]);
      for (int k = 0; k < 4; ++k)
        if (!held(r.Fold[k]))
          r.Fold[k] = -1;
      r.Head = r.Head && held(r.HeadBone);
      if (!held(r.Neck1))
        r.Neck1 = -1;
      // [ALIVE] The eyes: both eye bones held, or neither is turned (one eye looking away alone would be a squint)
      if (!held(r.EyeBone[0]) || !held(r.EyeBone[1]))
        r.EyeBone[0] = r.EyeBone[1] = -1;
      life = r.Ok = r.Eyes || r.Brows || r.Head;
    }
    // [ALIVE] The feeling's bones (the game's own face poses: Faith's library for Faith and the woman's skeleton, the
    // man's for everyone else; VoiceFeelFace)
    int feelLibrary = voice_feel_tables::LibraryMan;
    // [REVIEW 6] (not minding the case: a skeleton made from the game's own names keeps the game's spelling. A woman
    // in heels has the men's skeleton table and takes the man's library: which one looks right on her isn't known)
    if (s.Asset && (!_stricmp(s.Asset, "characters/skeletons/skeleton_faith") || !_stricmp(s.Asset, "characters/skeletons/skeleton_female")))
      feelLibrary = voice_feel_tables::LibraryFaith;
    bool feel = voice_face::FindFeelRig(h.Bones, s.Names, h.Parents, &h.Base[0][0], feelLibrary, v.FeelRig);
    v.On = true;
    v.Playing = false;
    v.Time = 0.0;
    // The player: told once where to write what happens to it (the audio opened, lost), and to be heard only while
    // the game's window is in front, like the tools' rain and thunder
    static bool playerSetUp = false;
    if (!playerSetUp)
    {
      playerSetUp = true;
      voice_player::SetLog(&VoicePlayerLog);
      voice_player::SetFocusWindow((void*)g_gameHwnd);
    }
    if (sound->Pcm && !sound->Pcm->empty())
      v.Player = voice_player::Open(sound->Pcm, sound->Rate, sound->Channels);
    // (the lids' arrangement: the game keeps the lid bones on the lid's edge or at the eye's middle, and the blink
    // is made for each; which one this body had when its pose was taken is worth knowing when a blink looks wrong)
    // (the face's own directions are taken from the Face bone, both eye bones and HeadEnd: without them, Face stays
    // -1 and none of the three parts can be made, the head included)
    // (the rig's details are research: only with [Debug] Diagnostics on; a body with no Face bone is always told)
    if (r.Face < 0)
      util::log::Write("Pose: voice %s on %s 0x%I64X: for blinks, brow raises and head movement: no Face bone with both "
        "eye bones and HeadEnd that the tools can use: none of them", file.c_str(), h.Label, h.Entity);
    else if (util::DiagnosticsOn())
      util::log::Write("Pose: voice %s on %s 0x%I64X: for blinks, brow raises and head movement: its eyelids %s%s, its "
        "eyebrows %s, its head %s%s", file.c_str(), h.Label, h.Entity, life && r.Eyes ? "found" : "not found or not held",
        !(life && r.Eyes) ? "" : (r.LidOnEdge[0] && r.LidOnEdge[1] ? " (lid bones on the lids' edges)" :
        (!r.LidOnEdge[0] && !r.LidOnEdge[1] ? " (lid bones at the eyes' middles)" : " (lid bones: one eye on the edge, one at the middle)")),
        life && r.Brows ? "found" : "not found or not held", life && r.Head ? "found" : "not found or not held",
        life && r.Head ? (r.Neck1 >= 0 ? " (with Neck1)" : " (without Neck1)") : "");
    // [ALIVE] (the eyes; the feeling's poses, whose library, the face's size against it, its bones found and held)
    if (util::DiagnosticsOn() && r.Face >= 0)
      util::log::Write("Pose: voice %s on %s 0x%I64X: for the living face's eyes: %s; for the feeling: %s", file.c_str(),
        h.Label, h.Entity, life && r.EyeBone[0] >= 0 ? "both eye bones found and held" : "the eye bones not found or not held",
        feel ? "found" : "no face it can be laid on");
    if (util::DiagnosticsOn() && feel)
    {
      // (how many of the feeling's bones this skeleton has, and how many of them this pose holds: the others stay
      // the game's)
      int feelBones = 0, feelHeld = 0;
      for (int b = 0; b < voice_feel_tables::BoneCount; ++b)
        if (v.FeelRig.Bone[b] >= 0)
        {
          ++feelBones;
          if (v.FeelRig.Bone[b] < h.Bones && h.InHeld[v.FeelRig.Bone[b]])
            ++feelHeld;
        }
      util::log::Write("Pose: voice %s on %s 0x%I64X: the feeling: the game's face poses of %s's library, the face %.2f times "
        "its size, %d of the poses' %d bones on this skeleton, %d of them held (lid bones %s)", file.c_str(), h.Label,
        h.Entity, voice_feel_tables::Libraries[v.FeelRig.Library].Name, v.FeelRig.Scale, feelBones, voice_feel_tables::BoneCount,
        feelHeld, v.FeelRig.Life.LidOnEdge[0] && v.FeelRig.Life.LidOnEdge[1] ? "on the lids' edges" :
        (!v.FeelRig.Life.LidOnEdge[0] && !v.FeelRig.Life.LidOnEdge[1] ? "at the eyes' middles" : "one eye on the edge, one at the middle"));
    }
    // (the analysis' figures are research, only with Diagnostics on; its one-line note is always written)
    if (util::DiagnosticsOn() && !sound->LifeNotes.empty())
      util::log::Write("Pose: voice %s: its analysis for blinks, brows and head: %.700s%s%.300s", file.c_str(),
        sound->LifeNotes.c_str(), sound->LifeNote.empty() ? "" : "; note: ", sound->LifeNote.c_str());
    else if (!sound->LifeNote.empty())
      util::log::Write("Pose: voice %s: %.300s", file.c_str(), sound->LifeNote.c_str());
    util::log::Write("Pose: voice %s on %s 0x%I64X: %s; the sound %s", file.c_str(), h.Label, h.Entity,
      rig ? "its mouth's bones found (the jaw, six lip bones, two corners)" :
      "this character has no mouth bones the tools know: no mouth moves",
      v.Player >= 0 ? "has a place in the player" :
      "has no place in the player (all its places are taken, or it is of a kind the player doesn't take): no sound is heard");
    g_VoiceMessage[0] = 0;
  }

  // Whether a voice's time is being moved on by its clock now (not standing, not set by hand)
  bool VoiceClockMoves(Held const& h)
  {
    VoicePlay const& v = h.Voice;
    if (!v.Sound || !v.On)
      return false;
    if (v.Clock == VoiceTrackTime)
      return h.KeyTrackMoving;
    if (v.Clock != VoiceKeysTime)
      return v.Playing;
    // The keyframes' playhead: moved on by its own clock only while there are keyframes (KeepKeys), whether or not
    // they drive their bones
    if (h.Keys.Channels.empty() || !h.Keys.Playing)
      return false;
    if (h.Keys.Clock == pose_keys::Clock_Track)
      return h.KeyTrackMoving;
    if (h.Keys.Clock == pose_keys::Clock_Clip)
      return ClipUsable(h) && h.Clip.Playing;
    return true;
  }

  // [ALIVE] The living face's settings that aren't in the analysis' own files
  const ULONGLONG AliveSettleMs = 300;       // a take is made again once the take or looks-away slider rests this long
  const float InhaleOpen = 0.15f;            // [d] (VoiceAlive's research) the jaw opens this share of its way for a breath in
  const float FrownAu4 = 0.65f;              // [d] VoiceAlive's frown (thinking, a loud low stress) at 1 as the game's
                                             // brow lowerer at 0.65: its inner brow 2.7 mm down, 3.3 mm in (the
                                             // research's design: 2.5 - 3 mm down at 1)
  const float FeelChangeSeconds = 0.6f;      // [d] a feeling picked comes in over this, the one before goes out
  const float FeelStrengthSeconds = 0.2f;    // [d] the strength as shown follows the one set with this time constant

  // [ALIVE] This character's take of the living face made from the sound's analysis (about 1.5 ms for 30 s of sound,
  // measured offline; the game's thread)
  void MakeAlive(Held& h)
  {
    VoicePlay& v = h.Voice;
    v.AliveTake = v.Take;
    v.AliveLookAway = v.LookAway;
    v.Alive.reset();
    v.AliveFound.clear();
    if (!v.Sound || v.Sound->LifeTrack.Frames.empty())
      return;
    voice_alive::Settings settings;
    settings.LookAway = !(v.LookAway > 0.f) ? 0.f : (v.LookAway > 2.f ? 2.f : v.LookAway);
    settings.Variation = v.Take;
    std::shared_ptr<voice_alive::Track> track;
    std::string error;
    // (its time is measured for the diagnostic line only: [Debug] Diagnostics on)
    bool timed = util::DiagnosticsOn();
    LARGE_INTEGER from{}, to{}, rate{};
    if (timed)
      QueryPerformanceCounter(&from);
    bool ok = false;
    try
    {
      track = std::make_shared<voice_alive::Track>();
      ok = voice_alive::Make(v.Sound->LifeTrack, v.Sound->LifeDebug, settings, *track, error);
    }
    catch (...)
    {
      ok = false;
      error = "not enough memory";
    }
    if (timed)
    {
      QueryPerformanceCounter(&to);
      QueryPerformanceFrequency(&rate);
    }
    if (!ok || !track || track->Frames.empty())
    {
      v.AliveFound = "the living face couldn't be worked out" + (error.empty() ? std::string() : " (" + error + ")");
      util::log::Warning("Pose: voice %s on %s 0x%I64X: take %d: %s", v.Sound->File.c_str(), h.Label, h.Entity, v.Take,
        v.AliveFound.c_str());
      return;
    }
    v.Alive = track;
    // The menu's line, from the take's own report ("name: value" lines)
    std::string report = track->Report, line;
    auto value = [&](char const* name) -> std::string
    {
      std::string key = std::string(name) + ": ";
      size_t at = report.find(key);
      if (at == std::string::npos || (at > 0 && report[at - 1] != '\n'))
        return std::string();
      size_t end = report.find('\n', at);
      return report.substr(at + key.size(), end == std::string::npos ? std::string::npos : end - at - key.size());
    };
    std::string blinks = value("blinks"), partial = value("partial blinks"), aways = value("looks away");
    std::string share = value("share of the speaking time looking away"), scans = value("small saccades (scan, away, back)");
    if (!blinks.empty())
      line = blinks + (blinks == "1" ? " blink" : " blinks") + (partial.empty() || partial == "0" ? "" : " (" + partial + " part way)");
    if (!aways.empty())
    {
      line += (line.empty() ? "" : ", ") + aways + (aways == "1" ? " look away" : " looks away");
      double shareValue = share.empty() ? -1.0 : atof(share.c_str());
      if (shareValue > 0.0)
      {
        char text[64];
        _snprintf_s(text, sizeof(text), _TRUNCATE, " (%.0f %% of the speaking time)", 100.0 * shareValue);
        line += text;
      }
    }
    if (!scans.empty())
      line += (line.empty() ? "" : ", ") + scans + (scans == "1" ? " small eye movement" : " small eye movements");
    v.AliveFound = line;
    if (!timed)
      return;
    double ms = rate.QuadPart > 0 ? 1000.0 * (double)(to.QuadPart - from.QuadPart) / (double)rate.QuadPart : 0.0;
    std::string oneLine;
    for (char c : report)
      oneLine += c == '\n' ? std::string("; ") : (c == '\r' ? std::string() : std::string(1, c));
    util::log::Write("Pose: voice %s on %s 0x%I64X: take %d of its living face (looks away x%.2f) made in %.1f ms: %.900s",
      v.Sound->File.c_str(), h.Label, h.Entity, v.Take, settings.LookAway, ms, oneLine.c_str());
  }

  // [ALIVE] The living face at a time into the sound, each value cut within its stated limits (VoiceAlive.h; the take
  // keeps them itself, measured: its guard changed nothing on 796 files): nothing else can reach a bone
  voice_alive::Frame AliveAt(voice_alive::Track const& track, double seconds)
  {
    voice_alive::Frame f = voice_alive::At(track, seconds);
    auto cut = [](float x, float lo, float hi) { return !(x == x) ? 0.f : (x < lo ? lo : (x > hi ? hi : x)); };
    f.GazeYaw = cut(f.GazeYaw, -voice_alive::MaxGazeYaw, voice_alive::MaxGazeYaw);
    f.GazePitch = cut(f.GazePitch, -voice_alive::MaxGazeUp, voice_alive::MaxGazeDown);
    f.LidFollow = cut(f.LidFollow, -voice_alive::MaxLidFollow, voice_alive::MaxLidFollow);
    f.LidLift = cut(f.LidLift, -voice_alive::MaxLidLift, voice_alive::MaxLidLift);
    f.BlinkLeft = cut(f.BlinkLeft, 0.f, 1.f);
    f.BlinkRight = cut(f.BlinkRight, 0.f, 1.f);
    f.BrowLeft = cut(f.BrowLeft, voice_alive::MinBrow, voice_alive::MaxBrow);
    f.BrowRight = cut(f.BrowRight, voice_alive::MinBrow, voice_alive::MaxBrow);
    f.Frown = cut(f.Frown, 0.f, 1.f);
    f.InnerUp = cut(f.InnerUp, 0.f, 1.f);
    f.Smile = cut(f.Smile, 0.f, 1.f);
    f.Breath = cut(f.Breath, 0.f, 1.f);
    f.Inhale = cut(f.Inhale, 0.f, 1.f);
    f.Nod = cut(f.Nod, -voice_alive::MaxNodUp, voice_alive::MaxNodDown);
    f.Turn = cut(f.Turn, -voice_alive::MaxTurn, voice_alive::MaxTurn);
    f.Tilt = cut(f.Tilt, -voice_alive::MaxTilt, voice_alive::MaxTilt);
    f.Energy = cut(f.Energy, 0.f, 1.f);
    return f;
  }

  // [ALIVE] The feeling as shown this frame: its weights moved towards the one picked, the strength eased to the one
  // set, and the shape of that mix of presets at the voice's energy (VoiceFeel::ShapeOf)
  voice_face::FeelShape FeelShapeNow(VoicePlay& v, double at, float dt)
  {
    int picked = v.Feel >= 0 && v.Feel < voice_feel::FeelingCount ? v.Feel + 1 : 0;
    float step = !(dt > 0.f) ? 0.f : (dt > 0.1f ? 0.1f : dt);
    float sum = 0.f;
    // [STEADY CHANGE] The weights move so they keep summing to 1: the one picked up by step / FeelChangeSeconds, the
    // others down by the same in all, each by its share. Before, each went down by the whole step, so a feeling
    // picked while another change was going left them summing to under 1, and shown as shares (divided by their sum)
    // the face moved up to 3.8 times faster than a change should (measured by the review: a pick 0.3 s into a change
    // moved 0.105 of the face in one frame at 60 fps, against 0.028). With one other weight (a plain change), the
    // same arithmetic as before.
    int others = 0;
    float rest = 0.f;
    for (int f = 0; f <= voice_feel::FeelingCount; ++f)
    {
      float w = v.FeelWeights[f];
      if (!(w > 0.f) || !(w <= 1.f))
        v.FeelWeights[f] = w = !(w > 0.f) ? 0.f : 1.f;
      if (f != picked && w > 0.f)
      {
        ++others;
        rest += w;
      }
    }
    if (others <= 1)
      for (int f = 0; f <= voice_feel::FeelingCount; ++f)
      {
        float w = v.FeelWeights[f] + (f == picked ? 1.f : -1.f) * step / FeelChangeSeconds;
        v.FeelWeights[f] = !(w > 0.f) ? 0.f : (w > 1.f ? 1.f : w);
      }
    else
    {
      // (as shares of their sum, which then stays 1)
      float total = rest + v.FeelWeights[picked];
      float up = step / FeelChangeSeconds;
      float p = v.FeelWeights[picked] / total, others0 = rest / total;
      float p1 = p + up < 1.f ? p + up : 1.f;
      float k = others0 > 0.f ? (1.f - p1) / others0 : 0.f;
      for (int f = 0; f <= voice_feel::FeelingCount; ++f)
        v.FeelWeights[f] = f == picked ? p1 : v.FeelWeights[f] / total * k;
    }
    for (int f = 0; f <= voice_feel::FeelingCount; ++f)
      sum += v.FeelWeights[f];
    if (!(sum > 0.f))
    {
      for (int f = 0; f <= voice_feel::FeelingCount; ++f)
        v.FeelWeights[f] = f == picked ? 1.f : 0.f;
      sum = 1.f;
    }
    float strength = !(v.FeelStrength > 0.f) ? 0.f : (v.FeelStrength > 2.f ? 2.f : v.FeelStrength);
    if (!(v.FeelStrengthNow == v.FeelStrengthNow))
      v.FeelStrengthNow = strength;
    float ease = step / FeelStrengthSeconds;
    v.FeelStrengthNow += (strength - v.FeelStrengthNow) * (ease > 1.f ? 1.f : ease);
    // [EMOTION] The feeling read from the voice, over the one picked: each stretch's look (the feeling picked as it
    // is shown now, with the voice's feeling over it as far as the model is sure of it, or the stretch's own setting),
    // mixed by the voice's time (the same time always gives the same face: scrubbed, played slow; looped on its own
    // clock, the last stretch changes into the first across the loop's end). [REVIEW] Each look is eased towards what
    // it is now (a stretch's setting or the sureness changed comes in over FeelChangeSeconds), and the voice's share
    // eases in and out when it's turned on or off or its reading arrives. shown: the presets' weights; units: the
    // weights their units are laid with (the voice's lively feeling's own part scaled by the voice's arousal)
    const int count = voice_feel::FeelingCount + 1;
    float shown[count], units[count];
    for (int f = 0; f < count; ++f)
      shown[f] = units[f] = v.FeelWeights[f] / sum;
    v.FeelVoiceShareNow = 0.f;
    v.FeelStretchWeightNow = 0.f;
    {
      float target = v.FeelAuto && v.EmotionReadings ? 1.f : 0.f;
      float move = step / FeelChangeSeconds;
      float before = v.FeelAutoNow;   // [FIX CHECK] (0: the looks weren't shown: made anew, not eased from what they were)
      if (!(v.FeelAutoNow == v.FeelAutoNow))
        v.FeelAutoNow = target;
      v.FeelAutoNow = v.FeelAutoNow < target ? (v.FeelAutoNow + move < target ? v.FeelAutoNow + move : target) :
        (v.FeelAutoNow - move > target ? v.FeelAutoNow - move : target);
      size_t n = v.EmotionStretches.size();
      if (v.FeelAutoNow > 0.f && v.EmotionReadings && v.EmotionReadings->size() == n)
      {
        bool fresh = v.FeelLooks.size() != n || v.FeelLooksFor != (void const*)v.EmotionReadings.get() || !(before > 0.f);
        v.FeelLooks.resize(n);
        v.FeelLooksFor = v.EmotionReadings.get();
        for (size_t i = 0; i < n; ++i)
        {
          voice_emotion::Look want = voice_emotion::LookOf(&(*v.EmotionReadings)[i], i < v.FeelSetting.size() ?
            v.FeelSetting[i] : voice_emotion::FromVoice, v.FeelWeights, v.FeelSure);
          voice_emotion::Look& look = v.FeelLooks[i];
          if (fresh)
          {
            look = want;
            continue;
          }
          // The same part of the way for every weight (they still sum to 1), none moving more than move a frame
          float gap = 0.f;
          for (int f = 0; f < count; ++f)
          {
            gap = fmaxf(gap, fabsf(want.Weights[f] - look.Weights[f]));
            gap = fmaxf(gap, fabsf(want.Units[f] - look.Units[f]));
          }
          if (!(gap > move))
          {
            look = want;   // (caught up: exactly what it is, so a resting face is the same face)
            continue;
          }
          float part = move / gap;
          for (int f = 0; f < count; ++f)
          {
            look.Weights[f] += part * (want.Weights[f] - look.Weights[f]);
            look.Units[f] += part * (want.Units[f] - look.Units[f]);
          }
          look.Share += part * (want.Share - look.Share);
          look.Scale = want.Scale;
          look.Shown = want.Shown;
          look.ByHand = want.ByHand;
        }
        double loop = v.Loop && v.Clock == VoiceOwnTime && v.Sound && v.Sound->Seconds > 0.0 ? v.Sound->Seconds : 0.0;
        float heard[count], heardUnits[count], share = 0.f, stretchWeight = 0.f;
        int stretch = -1;
        if (voice_emotion::MixAt(v.EmotionStretches, v.FeelLooks, at, FeelChangeSeconds, loop, heard, heardUnits, share,
          stretch, stretchWeight))
        {
          float a = v.FeelAutoNow;
          for (int f = 0; f < count; ++f)
          {
            shown[f] = (1.f - a) * shown[f] + a * heard[f];
            units[f] = (1.f - a) * units[f] + a * heardUnits[f];
          }
          v.FeelStretchNow = stretch;
          v.FeelStretchWeightNow = stretchWeight;
          v.FeelVoiceShareNow = a * share;
        }
      }
    }
    // The presets mixed by their weights (none: no unit, every brow raise kept)
    voice_feel::Preset mix{};
    mix.Name = "";
    mix.RaiseKeep = shown[0];
    for (int f = 0; f < voice_feel::FeelingCount; ++f)
    {
      float w = shown[f + 1], wu = units[f + 1];
      if (!(w > 0.f) && !(wu > 0.f))
        continue;
      voice_feel::Preset const& p = voice_feel::Presets[f];
      for (int u = 0; u < voice_face::AuCount; ++u)
      {
        mix.Au[u] += wu * p.Au[u];
        mix.RightOnly[u] += wu * p.RightOnly[u];
      }
      mix.Nod += wu * p.Nod;
      mix.Tilt += wu * p.Tilt;
      mix.RaiseKeep += w * p.RaiseKeep;
      mix.Jaw += wu * p.Jaw;   // [JAW]
    }
    voice_feel::Settings settings;
    settings.Strength = v.FeelStrengthNow;
    settings.FollowVoice = v.FeelFollow;
    settings.Asymmetry = v.FeelUneven;
    settings.LeftStronger = v.FeelLeftStronger;
    float energy = v.Sound ? voice_feel::EnergyAt(v.Sound->Energy, at) : 0.f;
    // [SMILE] The smile's share now, by the voice's own time (looped on its own clock: across the loop's end)
    double smileLoop = v.Loop && v.Clock == VoiceOwnTime && v.Sound && v.Sound->Seconds > 0.0 ? v.Sound->Seconds : 0.0;
    float smile = voice_feel::SmileFactor(v.SmileTimes, at, smileLoop, v.FeelComes);
    v.FeelSmileNow = smile;
    // [JAW] [FIX CHECK] The closure a feeling's jaw goes by, taken over time: the most of JawClosure within FeelJawSpread
    // frames either way, nearer ones counting more (read from the voice's own mouth curve at those times: no state, the
    // same time always gives the same face)
    float jawShut = -1.f;
    v.FeelJawOn = mix.Jaw > 0.f && v.FeelStrengthNow > 0.f && v.Rig.Ok;
    if (mix.Jaw > 0.f && v.Sound)
    {
      jawShut = 0.f;
      for (int k = -voice_feel::FeelJawSpread; k <= voice_feel::FeelJawSpread; ++k)
      {
        float closed = k == 0 ? v.Shape.Closed : voice::MouthAt(*v.Sound, at + 0.01 * k, v.Smoothing).Closed;
        float w = 1.f - (float)(k < 0 ? -k : k) / (float)(voice_feel::FeelJawSpread + 1);
        float c = w * voice_feel::JawClosure(closed, v.Closure);
        if (c > jawShut)
          jawShut = c;
      }
    }
    return voice_feel::ShapeOf(mix, settings, energy, v.Shape, v.Closure, smile, jawShut);
  }

  // Each frame for a held pose (KeepHeld, after its keyframes and its clip, before its locals are handed): its
  // voice's time moved on by its clock, the mouth's shape at that time made the layer's offsets, and its sound asked
  // for at that time.
  void KeepVoice(Held& h)
  {
    VoicePlay& v = h.Voice;
    v.Layer.Count = 0;
    v.Shape = voice_face::Shape();
    v.LifeShape = voice_face::LifeShape();
    v.LifeEyesNow = v.LifeBrowsNow = v.LifeHeadNow = v.LifeTried = false;   // (set again below when the life moves the face)
    v.LifeBlinkTaken = 1.f;
    v.EyesNow = v.FeelNow = false;   // [ALIVE]
    v.GazeNow[0] = v.GazeNow[1] = 0.f;
    v.FeelKeep = 1.f;
    v.FeelNod = v.FeelTilt = 0.f;
    v.FeelStretchNow = -1;   // [EMOTION]
    if (!v.Sound)
      return;
    // [EMOTION] The voice's feeling: read once it's wanted (EmotionHelper.exe, from a thread of the tools' own), and
    // what it heard taken once it's there
    if (v.FeelAuto && !v.Emotion && v.FeelRig.Ok && !v.EmotionStretches.empty() && voice_emotion::Available())
      v.Emotion = voice_emotion::Start(v.Sound, v.EmotionStretches);
    if (v.Emotion && !v.EmotionReadings)
    {
      voice_emotion::Status s = voice_emotion::StatusOf(*v.Emotion);
      if (s.Now == voice_emotion::Done && s.Readings && s.Readings->size() == v.EmotionStretches.size())
        v.EmotionReadings = s.Readings;
    }
    double length = v.Sound->Seconds;
    double before = v.Time;
    bool moves = VoiceClockMoves(h);
    if (v.On)
    {
      if (v.Clock == VoiceKeysTime)
        v.Time = h.Keys.Time - v.Start;
      else if (v.Clock == VoiceTrackTime)
        v.Time = (g_ClipTrackTime >= 0.0 ? g_ClipTrackTime : 0.0) - v.Start;
      else if (GetTickCount64() - v.SetByHandAt < VoiceHandMs)
        moves = false;   // its time is held in the menu (the slider, the picture): it waits there, silent
      else if (v.Playing)
      {
        v.Time += g_ClipRealDt;
        if (v.Time >= length)
        {
          if (v.Loop && length > 0.0)
            v.Time = fmod(v.Time, length);
          else
          {
            v.Time = length;
            v.Playing = false;
            moves = false;
          }
        }
      }
    }
    if (!std::isfinite(v.Time))
      v.Time = 0.0;
    // Another clock that stands this frame (the game's time frozen under the keyframes): the voice stands
    if (v.Clock != VoiceOwnTime && v.Time == before)
      moves = false;

    // Its pace against real time (1: as recorded), for the sound: its own clock is real time itself; another clock's
    // is measured, the first frame's as it is and then over a fifth of a second. A jump (a loop, a cut) isn't a pace.
    // The rate told to the player is held still while the measure stays within 1 % of it, and is exactly 1 within
    // 2 % of real time: the player takes a new rate as a change of speed and chases harder (its pitch wobbles), and
    // takes a clock a little off the told rate in its stride (measured offline: 3 % off stands 15 ms behind).
    // The camera track is moved on AFTER pose_editor::Update (CameraManager::OnCameraUpdate), by that frame's own dt,
    // 0.1 s at most: the step seen here was made over the frame before, so it is held against that frame's length.
    // A frame over 0.1 s gives no pace (the track was held back in it: the player takes the jump), unless the frames
    // stay that long (three in a row: a slow frame dump), where the track's pace really is that slow.
    // The first pace taken is taken as it is (Paced), and none is taken from a time that wasn't this clock's the
    // frame before (the voice was off, just read, or on another clock).
    bool byTrack = v.Clock == VoiceTrackTime || (v.Clock == VoiceKeysTime && (h.Keys.Clock == pose_keys::Clock_Track ||
      (h.Keys.Clock == pose_keys::Clock_Clip && h.Clip.Clock == ClipTrackTime)));
    double real = byTrack ? g_ClipRealDtBefore : g_ClipRealDt;
    if (!moves || v.Clock == VoiceOwnTime)
    {
      v.Pace = v.Rate = 1.f;
      v.Paced = false;
    }
    else if (!v.WasOn || v.LastClock != v.Clock)
      v.Paced = false;
    else if (real > 0.0 && (!byTrack || real <= 0.1 || g_ClipLongFrames >= 3))
    {
      double pace = (v.Time - before) / real;
      if (pace > 0.0 && pace < 8.0)
      {
        double share = v.Paced ? real / VoiceRateSeconds : 1.0;
        if (share > 1.0)
          share = 1.0;
        v.Pace += (float)((pace - (double)v.Pace) * share);
        float told = fabsf(v.Pace - 1.f) < 0.02f ? 1.f : v.Pace;
        if (!v.Paced || told == 1.f || fabsf(told - v.Rate) > 0.01f * v.Rate)
          v.Rate = told;
        v.Paced = true;
      }
    }
    v.WasOn = v.On;
    v.LastClock = v.Clock;
    v.Moving = moves;

    // The sound, where the voice's time is at this frame's reading of the clock (before its start and after its end
    // the player is silent by itself: a voice whose time runs in from before 0 is heard from its first sample)
    if (v.Player >= 0)
      voice_player::Want(v.Player, v.On && v.Heard && moves, v.Time, v.Rate, v.Volume, g_ClipCounterSeconds);
    if (!v.On)
      return;
    // (the face ahead of the sound only while it plays: paused, it shows the time it stands at)
    double at = v.Time + (moves ? (double)v.Lead * (double)v.Rate : 0.0);
    // [ALIVE] The settings within their ranges (one that isn't a number would never equal the take's and make it
    // again every AliveSettleMs)
    // (and the others too, so that no value typed into a slider that isn't a number stays in one: 0 for those)
    auto within = [](float x, float most) { return !(x > 0.f) ? 0.f : (x > most ? most : x); };
    v.Take = v.Take < 1 ? 1 : (v.Take > 99 ? 99 : v.Take);
    v.LookAway = within(v.LookAway, 2.f);
    v.Eyes = within(v.Eyes, 1.f);
    if (!(v.Feel >= -1 && v.Feel < voice_feel::FeelingCount))
      v.Feel = -1;
    v.FeelStrength = within(v.FeelStrength, 2.f);
    v.FeelFollow = within(v.FeelFollow, 1.f);
    v.FeelUneven = within(v.FeelUneven, 0.3f);
    v.FeelComes = within(v.FeelComes, 1.f);   // [SMILE]
    v.FeelSure = !(v.FeelSure >= voice_emotion::SureLeast) ? voice_emotion::SureLeast :
      (v.FeelSure > voice_emotion::SureMost ? voice_emotion::SureMost : v.FeelSure);   // [EMOTION]
    // [ALIVE] This character's take of the living face, made (again) when there is none for the take and the looks
    // away set, once those sliders have rested AliveSettleMs
    if (v.Life &&!v.Sound->LifeTrack.Frames.empty() && (v.AliveTake != v.Take || v.AliveLookAway != v.LookAway) &&
        GetTickCount64() - v.AliveWantedAt >= AliveSettleMs)
      MakeAlive(h);
    bool haveAlive = v.Life && v.Alive && !v.Alive->Frames.empty();
    voice_alive::Frame alive{};
    if (haveAlive)
      alive = AliveAt(*v.Alive, at);
    voice_face::Amounts amounts;
    float jaw[FloatsPerBone];
    v.FeelJawNow = 0.f;
    v.FeelJawOn = false;
    if (v.Rig.Ok)
    {
      v.Shape = voice::MouthAt(*v.Sound, at, v.Smoothing);
      // [ALIVE] The lips part for a quick breath in before speech
      if (haveAlive && alive.Inhale > 0.f)
      {
        float open = v.Shape.Open + InhaleOpen * alive.Inhale;
        v.Shape.Open = open > 1.f ? 1.f : open;
      }
      amounts.Jaw = v.Jaw;
      amounts.Lips = v.Lips;
      amounts.Closure = v.Closure;
      // The jaw as it is posed under the voice: the edit and the keyframes on the pose taken (or the clip's), and
      // the idle movement
      EditedLocal(h, v.Rig.Jaw, jaw);
      AddIdle(h, v.Rig.Jaw, jaw);
      voice_face::MakeOffsets(v.Rig, v.Shape, amounts, jaw + 4, v.Layer);
    }
    int mouthEntries = v.Layer.Count;   // [JAW] the mouth's own entries, first in the layer (MakeOffsets: 9)
    // The life around the mouth: the eyelids, the eyebrows and the head, after the mouth's bones in the same layer.
    // Its moves are made from the pose the layer is laid on AS IT IS THIS FRAME (the edits, the keyframes, the clip
    // and the idle movement; a bone that isn't held: as taken): the lids' way down is measured from where the lid
    // bones stand now, so lids the user has MOVED part closed are shut from there and not driven past shut (measured
    // offline with the way taken once from the pose taken: half-closed lids ended 7 - 8 mm under the lower lid), and
    // the head's own axes are the Head's and Neck1's rotations of now. Finding the bones again takes about 4 - 11
    // microseconds (measured). A part the voice's own rig has off (not there, or not all held) stays off.
    // [ALIVE] With the living face (VoiceAlive's take) the shape is its own: each eye's blink, each side's brows, its
    // head; the feeling (VoiceFeelFace) is laid on the same pose; then the eyes (VoiceGaze), held against the head as
    // the layer has turned it; then the lids, which follow the eyes. In that order: the brows and the head, the
    // feeling's face and head, the eyes, the life's lids, the feeling's lids.
    bool feelPicked = v.Feel >= 0 && v.Feel < voice_feel::FeelingCount;
    for (int f = 1; f <= voice_feel::FeelingCount; ++f)
      feelPicked = feelPicked || v.FeelWeights[f] > 0.f;   // (one still going out)
    feelPicked = feelPicked || v.FeelAutoNow > 0.f || (v.FeelAuto && v.EmotionReadings);   // [EMOTION]
    if ((v.Life || feelPicked) && (v.LifeRig.Ok || v.FeelRig.Ok) && h.Skeleton >= 0 && h.Skeleton < pose_skeleton_live::Count())
    {
      if (haveAlive)
      {
        voice_face::LifeShape& s = v.LifeShape;
        s = voice_face::LifeShape();
        s.Sided = true;
        s.BlinkSide[0] = alive.BlinkLeft;
        s.BlinkSide[1] = alive.BlinkRight;
        s.BrowSide[0] = alive.BrowLeft;
        s.BrowSide[1] = alive.BrowRight;
        s.Nod = alive.Nod;
        s.Turn = alive.Turn;
        s.Tilt = alive.Tilt;
        // (both sides' middle, for the menu's bars)
        s.Blink = 0.5f * (alive.BlinkLeft + alive.BlinkRight);
        s.Brow = 0.5f * (alive.BrowLeft + alive.BrowRight);
      }
      else if (v.Life)
        v.LifeShape = voice::LifeAt(*v.Sound, at);
      voice_face::LifeAmounts life;
      life.Blinks = v.Blinks;
      life.Brows = v.Brows;
      life.Head = v.Head;
      static float pose[MaxBones][FloatsPerBone];   // (the game's thread only, like the clip's)
      for (int i = 0; i < h.Bones; ++i)
        if (h.InHeld[i])
        {
          EditedLocal(h, i, pose[i]);
          AddIdle(h, i, pose[i]);
        }
        else
          memcpy(pose[i], h.Base[i], sizeof(pose[i]));
      // The Face bone's own rotation as the pose under the edits has it (as taken, or the clip's), not as the user
      // turned it: the eyes, lids and brows hang on the Face bone and turn with it, so the face's own axes in its
      // frame are the same however it is turned. With the edited rotation (found by the review, measured through
      // this code) a Face turned 30 degrees sent the brows 30 degrees off the face's up and the nod 26 - 28 degrees
      // off the head's own axis, and from 40 - 60 degrees the blink and the head were dropped. (The game never turns
      // the Face bone: identity on every body in every recorded sample.)
      int faceBone = v.LifeRig.Ok ? v.LifeRig.Face : v.FeelRig.Life.Face;
      if (faceBone >= 0 && faceBone < h.Bones)
        memcpy(pose[faceBone] + 4, (h.InHeld[faceBone] ? BaseOf(h, faceBone) : h.Base[faceBone]) + 4, 4 * sizeof(float));
      char const* const* names = pose_skeleton_live::At(h.Skeleton).Names;
      voice_face::LifeRig now;
      bool nowOk = false;
      if (v.Life && v.LifeRig.Ok)
      {
        v.LifeTried = true;
        nowOk = voice_face::FindLifeRig(h.Bones, names, h.Parents, &pose[0][0], now);
      }
      if (nowOk)
      {
        now.Eyes = now.Eyes && v.LifeRig.Eyes;
        now.Brows = now.Brows && v.LifeRig.Brows;
        now.Head = now.Head && v.LifeRig.Head;
        for (int k = 0; k < 4; ++k)
          if (v.LifeRig.Fold[k] < 0)
            now.Fold[k] = -1;
        if (v.LifeRig.Neck1 < 0)
          now.Neck1 = -1;
        if (v.LifeRig.EyeBone[0] < 0 || v.LifeRig.EyeBone[1] < 0)   // [ALIVE] (both eyes held, or neither turned)
          now.EyeBone[0] = now.EyeBone[1] = -1;
        now.Ok = now.Eyes || now.Brows || now.Head;
        // Lid bones at the eye's middle (the game's other arrangement, on people) are shut by TURNING them, which
        // their places don't show: the part of the upper lid's turn about the head's right, in the blink's sense,
        // that the pose under the layer has is taken off the blink (by how far the lid is from the edge). Measured by
        // the review: without it a lid already turned shut by 35 degrees was turned 35 more.
        // [ALIVE] The turn is measured against the identity, the game's own bind rotation of every face bone (every
        // library's bind, the feeling's research), not against the pose as taken: a pose TAKEN with the eyes shut
        // (the game's people mid-blink: 32 - 35 degrees there; open: 3.3 at most) is covered too, as the feeling's
        // own reading of it (VoiceFeelFace's PoseClosed) does.
        float taken[2] = { 1.f, 1.f };   // how much of its blink each eye is left (the menu's Blink bar)
        for (int s = 0; now.Eyes && s < 2; ++s)
        {
          int lid = now.UpLid[s];
          float share = 1.f - now.LidEdge[s];
          if (!(share > 0.f) || lid < 0 || lid >= h.Bones)
            continue;
          float q[4];
          memcpy(q, pose[lid] + 4, sizeof(q));
          if (q[3] < 0.f)
            for (int k = 0; k < 4; ++k)
              q[k] = -q[k];
          float about = q[0] * now.Right[0] + q[1] * now.Right[1] + q[2] * now.Right[2];
          float closed = now.NodSign * 2.f * atan2f(about, q[3]);
          if (!std::isfinite(closed))
            continue;
          float uncapped = now.LidAngle[s];
          float angle = now.LidAngle[s] - closed * share;
          now.LidAngle[s] = !(angle > 0.f) ? 0.f : (angle > voice_face::LidAngleMost ? voice_face::LidAngleMost : angle);
          // The lower lid's share of the blink (lid bones between the two arrangements) goes down with the upper's:
          // without it, its move dropped in one frame where the upper lid's reached 0 (the fix check's find)
          if (uncapped > 0.f && now.LidAngle[s] < uncapped)
          {
            float left = now.LidAngle[s] / uncapped;
            for (int k = 0; k < 3; ++k)
              now.LowLidMove[s][k] *= left;
            taken[s] = left;
          }
        }
        v.LifeBlinkTaken = (taken[0] + taken[1]) * 0.5f;
        v.LifeEyesNow = now.Eyes;
        v.LifeBrowsNow = now.Brows;
        v.LifeHeadNow = now.Head;
      }
      float const* headQ = nowOk ? pose[now.HeadBone] + 4 : nullptr;
      float const* faceQ = nowOk ? pose[now.Face] + 4 : nullptr;
      float const* neck1Q = nowOk && now.Neck1 >= 0 ? pose[now.Neck1] + 4 : nullptr;
      // 1. The brows and the head
      if (nowOk)
        voice_face::AddLifeOffsets(now, v.LifeShape, life, headQ, faceQ, v.Layer, neck1Q,
          voice_face::LifePartBrows | voice_face::LifePartHead);
      // 2. The feeling's face (every bone but the lids) and its nod and tilt, on the same pose; the living face's
      // frown (thinking, a loud low stress) as the game's brow lowerer
      voice_face::FeelRig feel;
      bool feelOk = v.FeelRig.Ok && voice_face::FindFeelRig(h.Bones, names, h.Parents, &pose[0][0], v.FeelRig.Library, feel);
      voice_face::FeelShape feelShape;
      float const* feelHead = nullptr;
      float const* feelFace = nullptr;
      float const* feelNeck = nullptr;
      if (feelOk)
      {
        feelShape = FeelShapeNow(v, at, (float)g_ClipRealDt);
        if (haveAlive && alive.Frown > 0.f)
        {
          float brows = !(v.Brows > 0.f) ? 0.f : (v.Brows > 2.f ? 2.f : v.Brows);
          feelShape.Left[voice_face::Au4] += FrownAu4 * alive.Frown * brows;
          feelShape.Right[voice_face::Au4] += FrownAu4 * alive.Frown * brows;
        }
        // [JAW] The feeling's jaw (Surprised, Afraid) under the speech's own: the mouth's entries made again with the
        // jaw that much further open where the speech has it less (Open + Jaw x (1 - Open): the speech still opens it
        // all the way; the feeling's jaw is already gone where the lips close for p, b and m: ShapeOf), laid over the
        // mouth's own entries before the feeling's corner and lip moves are added to them. v.Shape stays the speech's
        // (the feeling's lip gates, the menu's bars).
        if (feelShape.Jaw > 0.f && v.Rig.Ok && mouthEntries > 0 && mouthEntries <= v.Layer.Count &&
          v.Layer.Bone[0] == v.Rig.Jaw)
        {
          voice_face::Shape opened = v.Shape;
          float j = feelShape.Jaw > 1.f ? 1.f : feelShape.Jaw;
          float open = opened.Open < 0.f ? 0.f : (opened.Open > 1.f ? 1.f : opened.Open);
          opened.Open = open + j * (1.f - open);
          voice_face::Offsets mouth;
          voice_face::MakeOffsets(v.Rig, opened, amounts, jaw + 4, mouth);
          bool alike = mouth.Count == mouthEntries;
          for (int n = 0; alike && n < mouthEntries; ++n)
            alike = mouth.Bone[n] == v.Layer.Bone[n];
          if (alike)
          {
            for (int n = 0; n < mouthEntries; ++n)
            {
              memcpy(v.Layer.Turn[n], mouth.Turn[n], sizeof(v.Layer.Turn[n]));
              memcpy(v.Layer.Move[n], mouth.Move[n], sizeof(v.Layer.Move[n]));
            }
            v.FeelJawNow = j;
          }
        }
        feelHead = pose[feel.Life.HeadBone] + 4;
        feelFace = pose[feel.Life.Face] + 4;
        feelNeck = feel.Life.Neck1 >= 0 ? pose[feel.Life.Neck1] + 4 : nullptr;
        voice_face::AddFeelOffsets(feel, feelShape, nowOk ? &now : nullptr, feelHead, feelFace, feelNeck, v.Layer,
          voice_face::FeelPartFace);
        v.FeelNow = true;
        // (for the menu: the share of the voice's brow raises the feeling keeps, and its own nod and tilt, as
        // AddFeelOffsets takes them)
        float keep = feelShape.RaiseKeep;
        v.FeelKeep = !(keep >= 0.f) ? 0.f : (keep > 1.f ? 1.f : keep);
        auto posture = [](float x) { return !(x == x) ? 0.f : (x > voice_face::FeelHeadMost ? voice_face::FeelHeadMost :
          (x < -voice_face::FeelHeadMost ? -voice_face::FeelHeadMost : x)); };
        v.FeelNod = feel.Life.Head ? posture(feelShape.Nod) : 0.f;
        v.FeelTilt = feel.Life.Head ? posture(feelShape.Tilt) : 0.f;
      }
      // 3. The eyes: the take's gaze, held in the world against the head as the layer has now turned it (the life's
      // and the feeling's turns of Head and Neck1), brought back onto EyeMost where it would pass it; and how far each
      // eye looks down more than as posed, for the lids.
      // [REVIEW 4] Against the eyes as posed NOW, not as taken: the lids follow only what the voice does to the eyes.
      // The user's own eye edits and lid edits stay the user's pair (before: an eye edited down and its lid lowered
      // by hand to match were lowered again, to 86 % shut; and a sound with no take laid lids it never had: the
      // review).
      float lookDown[2] = { 0.f, 0.f }, lookRight[2] = { 0.f, 0.f };
      if (nowOk && now.EyeBone[0] >= 0 && now.EyeBone[1] >= 0 && now.EyeBone[0] < h.Bones && now.EyeBone[1] < h.Bones)
      {
        float eyes = !(v.Eyes > 0.f) ? 0.f : (v.Eyes > 1.f ? 1.f : v.Eyes);
        bool lay = haveAlive && (eyes > 0.f || v.HoldGaze);
        // (the take's gaze is the eyes in the head against ITS planned head, its own counter-turn taken off already;
        // the gaze it plans, against the head as posed, is that plus the take's head: held, VoiceGaze takes the
        // layer's real head back out of it; not held, the eyes look that way in the head and ride with every turn)
        float yaw = haveAlive ? eyes * (alive.GazeYaw + alive.Turn) : 0.f;
        float pitch = haveAlive ? eyes * (alive.GazePitch + alive.Nod) : 0.f;
        float const* eyeQ[2] = { pose[now.EyeBone[0]] + 4, pose[now.EyeBone[1]] + 4 };
        // [REVIEW 3] The Face bone's rotation as it is handed (the user's edit and the idle movement in it): the eyes
        // hang on it so, and the head's counter-turn is taken in its frame (pose[] has the Face as taken, for the
        // face's own axes; with that one a Face turned 10 - 30 degrees by the user put the held eyes 1 - 9 degrees
        // off: the review)
        float faceHanded[FloatsPerBone];
        if (h.InHeld[now.Face])
        {
          EditedLocal(h, now.Face, faceHanded);
          AddIdle(h, now.Face, faceHanded);
        }
        else
          memcpy(faceHanded, h.Base[now.Face], sizeof(faceHanded));
        if (voice_face::AddGazeOffsets(now, yaw, pitch, v.HoldGaze, lay, neck1Q, headQ, faceHanded + 4, eyeQ, eyeQ, v.Layer,
            lookDown, lookRight) && lay)
        {
          // (the menu's line: what was laid, the two eyes' mean, in degrees against the eyes as posed)
          v.EyesNow = true;
          v.GazeNow[0] = 57.29578f * 0.5f * (lookRight[0] + lookRight[1]);
          v.GazeNow[1] = 57.29578f * 0.5f * (lookDown[0] + lookDown[1]);
        }
      }
      // 4. The life's lids: each eye's blink, and its upper lid following its eye (the energy's small widening off)
      float follow[2] = { 0.f, 0.f }, blinkUsed[2] = { 0.f, 0.f };
      for (int s = 0; s < 2; ++s)
      {
        follow[s] = voice_face::LidFollowOf(lookDown[s]) - (haveAlive ? alive.LidLift : 0.f);
        v.LifeShape.LidFollow[s] = follow[s];
        blinkUsed[s] = voice_face::LifeBlinkOf(v.LifeShape, life, s);
      }
      if (nowOk)
        voice_face::AddLifeOffsets(now, v.LifeShape, life, headQ, faceQ, v.Layer, neck1Q, voice_face::LifePartLids);
      // 5. The feeling's lids, the life's blink and follow laid again from where the feeling puts them
      if (feelOk)
        voice_face::AddFeelOffsets(feel, feelShape, nowOk ? &now : nullptr, feelHead, feelFace, feelNeck, v.Layer,
          voice_face::FeelPartLids, blinkUsed, follow);
    }
    // Held bones only, each once: a bone this body's pose doesn't hold (its rig has no channel for it) stays the
    // game's, and nothing that isn't a number goes into the pose
    int kept = 0;
    for (int n = 0; n < v.Layer.Count && n < voice_face::MaxDriven; ++n)
    {
      int bone = v.Layer.Bone[n];
      bool ok = bone >= 0 && bone < h.Bones && h.InHeld[bone];
      for (int k = 0; ok && k < 4; ++k)
        ok = std::isfinite(v.Layer.Turn[n][k]);
      for (int k = 0; ok && k < 3; ++k)
        ok = std::isfinite(v.Layer.Move[n][k]);
      for (int k = 0; ok && k < kept; ++k)
        ok = v.Layer.Bone[k] != bone;
      if (!ok)
        continue;
      if (kept != n)
      {
        v.Layer.Bone[kept] = bone;
        memcpy(v.Layer.Turn[kept], v.Layer.Turn[n], sizeof(v.Layer.Turn[kept]));
        memcpy(v.Layer.Move[kept], v.Layer.Move[n], sizeof(v.Layer.Move[kept]));
      }
      ++kept;
    }
    v.Layer.Count = kept;
  }

  // The voice's layer laid over a bone's handed local pose (after the idle movement)
  void AddVoice(Held const& h, int bone, float* local)
  {
    voice_face::Offsets const& layer = h.Voice.Layer;
    for (int n = 0; n < layer.Count; ++n)
      if (layer.Bone[n] == bone)
      {
        float turned[4];
        QMul(layer.Turn[n], local + 4, turned);
        QNorm(turned);
        memcpy(local + 4, turned, sizeof(turned));
        for (int k = 0; k < 3; ++k)
          local[k] += layer.Move[n][k];
        return;
      }
  }

  // Every frame for a held pose: its slots found again, the check of what the
  // game made of last frame's pose, the world pose for the dots, and this
  // frame's pose handed. Game thread.
  void KeepHeld(Held& h, __int64 soldier, bool replayBusy, bool timeFrozen, ULONGLONG now)
  {
    LiveSlot body{}, box{};
    __int64 animatable = 0, boxAnimatable = 0;
    bool boxOk = false, ok;
    if (h.Kind == Kind_Faith)
      ok = h.Entity == soldier && FaithSlots(soldier, animatable, body, boxAnimatable, box, boxOk) &&
        animatable == h.Animatable;
    else
      ok = StillThere(h.Entity, h.VTable, h.BodyOffset, h.Animatable) && BodySlot(h.Entity, h.BodyOffset, animatable, body) &&
        animatable == h.Animatable;
    if (!ok || body.Manager != h.Manager || body.Handle != h.Handle || body.Bones != h.Bones)
    {
      Release(h, h.Kind == Kind_Faith && h.Entity != soldier ? "Faith is gone or new (a respawn or a level load)" :
        "its body or pose slot is gone or changed", true);
      return;
    }
    if (h.BodyTable && h.Body[h.Next ^ 1].Handle == -1)
    {
      // The game released the slot (it sets the override's handle to -1)
      Release(h, "the game released its pose slot", true);
      return;
    }
    bool boxSame = boxOk && h.HasBox && boxAnimatable == h.BoxAnimatable && box.Handle == h.BoxHandle;
    int index = (int)(&h - g_Held);
    if (h.Restoring)
    {
      KeepLettingGo(h, boxSame, now);
      return;
    }
    if (h.HasBox && !boxSame && h.BoxTable)
      TakeBack(h.Box, h.BoxTable, h.BoxTableHandle);

    // The game's world pose and local pose now, and each bone's world pose as
    // the game builds it: bone 0's, then every bone its parent's times its
    // local pose. The world pose's own entry for the root (bone 1,
    // AITrajectory) has another rotation, a quarter turn about up from the one
    // its children are built on (2026-11-08, live, Faith: Hips, CameraBase,
    // Connect and Trajectory 0.71 dot off it, Hips 10.6 cm; the chain of local
    // poses from bone 0 gave every other bone's world pose exactly). So the
    // chain is the frame the held bones hang from, not that entry.
    static float live[MaxBones][FloatsPerBone], liveLocal[MaxBones][FloatsPerBone], chain[MaxBones][FloatsPerBone];
    bool liveOk = CopyFrom(body.World, &live[0][0], (size_t)h.Bones * FloatsPerBone) &&
      CopyFrom(body.Local, &liveLocal[0][0], (size_t)h.Bones * FloatsPerBone);
    if (liveOk)
    {
      memcpy(chain[0], live[0], sizeof(chain[0]));
      for (int i = 1; i < h.Bones; ++i)
      {
        int p = h.Parents[i];
        if (p >= 0 && p < i)
          Compose(chain[p], liveLocal[i], chain[i]);
        else
          memcpy(chain[i], live[i], sizeof(chain[i]));
      }
    }

    // What the game made of the locals handed last frame: each held bone's
    // world pose its parent's (as the game builds it) times them
    // (not while a clip, the keyframes or a voice change the pose every frame: the game can be a frame behind a
    // changing pose, and a jaw that opens fast would count as "not taken"; Voice.Moving is still the frame
    // before's here, the frame whose hand-over is checked)
    if (liveOk && h.HandedOk && h.BodyTable && !(h.Clip.On && h.Clip.Playing) && !PlayingKeys(h) &&
        !(h.Voice.Sound && h.Voice.On && (h.Voice.Rig.Ok || (h.Voice.Life && h.Voice.LifeRig.Ok) ||
          (h.Voice.FeelRig.Ok && (h.Voice.Life || h.Voice.Feel >= 0))) && h.Voice.Moving))
    {
      float worst = 0.f;
      bool taken = true;
      for (int n = 0; n < h.Count; ++n)
      {
        int i = (int)h.List[n], p = h.Parents[i];
        if (p < 0)
          continue;
        float expected[FloatsPerBone];
        Compose(chain[p], h.Handed[i], expected);
        float distance = Distance3(expected, live[i]);
        float dot = fabsf(expected[4] * live[i][4] + expected[5] * live[i][5] + expected[6] * live[i][6] +
          expected[7] * live[i][7]);
        if (!(distance <= TakenDistance) || !(dot >= TakenDot))
          taken = false;
        if (distance > worst)
          worst = distance;
      }
      if (timeFrozen)
      {
        ++h.FrozenFrames;
        h.FrozenTaken += taken;
      }
      else
      {
        ++h.Frames;
        h.Taken += taken;
      }
      if (worst > h.Worst)
        h.Worst = worst;
    }
    if (now - h.CheckSince >= CheckLogMs)
    {
      if (h.Frames + h.FrozenFrames > 0)
      {
        sprintf_s(h.CheckLine, "The game took the pose in %d of %d frames with time running, %d of %d with time frozen "
          "(last %.0f s)", h.Taken, h.Frames, h.FrozenTaken, h.FrozenFrames, (now - h.CheckSince) / 1000.0);
        util::log::Write("Pose: %s 0x%I64X: the game took the pose in %d of %d frames with time running, %d of %d with "
          "time frozen; the most a held bone was off its parent times the pose: %.1f cm", h.Label, h.Entity, h.Taken,
          h.Frames, h.FrozenTaken, h.FrozenFrames, h.Worst * 100.f);
      }
      if (g_IdleOn)
      {
        // What its gate did with them (a person's limits, or the 10-degree one where no hips and legs were found): research,
        // only with [Debug] Diagnostics on (without it the line is the one before these figures)
        char gate[1024] = "";
        if (h.IdleFrames > 0 && util::DiagnosticsOn())
        {
          char legsName[64], spinName[64], stepName[64];
          strcpy_s(legsName, h.IdleMostLegsBone >= 0 ? BoneName(h, h.IdleMostLegsBone) : "none");
          strcpy_s(spinName, h.IdleMostSpinBone >= 0 ? BoneName(h, h.IdleMostSpinBone) : "none");
          strcpy_s(stepName, h.IdleMostStepBone >= 0 ? BoneName(h, h.IdleMostStepBone) : "none");
          if (h.IdlePeople)
            _snprintf_s(gate, sizeof(gate), _TRUNCATE, "; its gate open in %d of them (speeds in the game's "
              "time); its root faster than %.2f m/s in %d, its hips or a leg more "
              "than %.0f degrees off where their animation has been in %d, those or its spine turning faster than %.0f "
              "degrees a second in %d; the 10-degree rule before 2026-11-14 would have shut %d; at most: its root %.2f "
              "m/s, %s %.1f degrees off, %s %.0f degrees a second; the largest turn between two of them %.1f degrees (%s)",
              h.IdleOpenCaptures, IdleGateSpeed, h.IdleOverSpeed, IdleGateLegs * 57.2957795f, h.IdleOverLegs,
              IdleGateSpin, h.IdleOverSpin, h.IdleOldRule, h.IdleMostSpeed, legsName, h.IdleMostLegs * 57.2957795f,
              spinName, h.IdleMostSpin, h.IdleMostStep, stepName);
          else
            _snprintf_s(gate, sizeof(gate), _TRUNCATE, "; its gate (no hips and legs found: any body bone "
              "more than 10 degrees off, or its root "
              "faster than %.2f m/s) open in %d of them; its root faster in %d; at most its root %.2f m/s; the largest "
              "turn between two of them %.1f degrees (%s)", IdleGateSpeed, h.IdleOpenCaptures, h.IdleOverSpeed,
              h.IdleMostSpeed, h.IdleMostStep, stepName);
        }
        util::log::Write("Pose: %s 0x%I64X: idle movement from %d of the game's animation frames under the pose (last %.0f s)%s",
          h.Label, h.Entity, h.IdleFrames, (now - h.CheckSince) / 1000.0, gate);
      }
      h.IdleFrames = 0;
      ResetIdleCounts(h);
      for (int i = 0; i < h.WeaponCount; ++i)
      {
        HeldWeapon const& w = h.Weapons[i];
        if (w.Bone != -1 && w.GapOk && (w.Drawing || w.Holding))
          util::log::Write("Pose: %s 0x%I64X: its %s (%s%s): %s; since the tools' write the frame before, the game moved its "
            "local bone 1 %.1f cm and its bone 1's matrix %.1f cm", h.Label, h.Entity, w.Name,
            w.Bone == WorldFrame ? "put down" : "with ", w.Bone == WorldFrame ? "" : BoneName(h, w.Bone),
            w.Drawing ? "its body drawn by the tools" : (w.Holding ? "only its entity moved" : "not moved by the tools now"),
            w.Gap * 100.f, w.MatrixGap * 100.f);
      }
      h.Frames = h.Taken = h.FrozenFrames = h.FrozenTaken = 0;
      h.Worst = 0.f;
      h.CheckSince = now;
    }

    // This frame's pose: the world pose for the dots and the rings (the
    // game's chain, the held bones from their parents and the pose) and the
    // locals to hand
    float* buffer = h.Buffer[h.Next];
    if (liveOk)
      memcpy(h.World, chain, (size_t)h.Bones * FloatsPerBone * sizeof(float));
    UpdateIdle(h, (int)(&h - g_Held), now);
    bool clipPose = AdvanceClip(h);
    KeepKeys(h);
    if (clipPose)
      MakeClipPose(h);
    KeepVoice(h);
    // (EditedLocal, AddIdle, AddVoice per bone as before 2026-11-16, then the kinematics' layers: PoseMotion.inl)
    MakeHandedPose(h, buffer, liveOk);
    h.WorldOk = liveOk;

    // Steps aside while a replay records or plays (it hands its own override
    // to people and Faith)
    if (replayBusy)
    {
      if (h.BodyTable || h.BoxTable)
        util::log::Write("Pose: %s 0x%I64X steps aside while a replay records or plays (the pose is kept)", h.Label,
          h.Entity);
      TakeBack(h.Body, h.BodyTable, h.BodyTableHandle);
      TakeBack(h.Box, h.BoxTable, h.BoxTableHandle);
      // Meanwhile the game's own animation, all of it (see BlendWatch)
      SetPutBack(index, 0, true);
      SetPutBack(index, 1, true);
      LetGoWeapons(h, true);
      h.HandedOk = false;
      h.Aside = true;
      strcpy_s(h.Status, "Steps aside while a replay records or plays (the pose is kept)");
      return;
    }
    if (h.Aside)
      util::log::Write("Pose: %s 0x%I64X held again", h.Label, h.Entity);
    h.Aside = false;

    int k = h.Next;
    h.Body[k] = { buffer, &h.ListVector, &h.MapVector, 1.f, (unsigned int)h.Count, (int)body.Handle, 0 };
    bool bodyIn = HandTo(h.Body, k, body, h.BodyTable, h.BodyTableHandle);
    if (!bodyIn)
    {
      if (!h.Refused)
        util::log::Warning("Pose: %s 0x%I64X: its pose slot 0x%X holds another override now (a replay's or a Dash "
          "ghost's): the pose waits", h.Label, h.Entity, body.Handle);
      h.Refused = true;
      h.HandedOk = false;
      SetPutBack(index, 0, true);
      LetGoWeapons(h, true);
      strcpy_s(h.Status, "Its pose slot holds another override (a replay's or a Dash ghost's): the pose waits");
      return;
    }
    h.Refused = false;
    bool boxIn = false;
    if (boxSame)
    {
      h.Box[k] = { buffer, &h.ListVector, &h.MapVector, 1.f, (unsigned int)h.Count, (int)box.Handle, 0 };
      boxIn = HandTo(h.Box, k, box, h.BoxTable, h.BoxTableHandle);
      if (!boxIn && !h.BoxRefused)
        util::log::Warning("Pose: %s 0x%I64X: its box pose's slot 0x%X holds another override", h.Label, h.Entity,
          box.Handle);
      h.BoxRefused = !boxIn;
    }
    else if (h.HasBox && !h.NoBoxLogged)
    {
      util::log::Warning("Pose: %s 0x%I64X: its box pose isn't found any more: only the body is posed (it may be culled "
        "by its unposed box)", h.Label, h.Entity);
      h.NoBoxLogged = true;
    }
    h.Next ^= 1;
    h.HandedOk = true;
    SetPutBack(index, 0, false);
    SetPutBack(index, 1, !boxIn);
    // Its weapons with the posed bones
    if (!g_WeaponsFollow)
      LetGoWeapons(h, true);
    else if (h.WorldOk)
      KeepWeapons(h, now, chain, timeFrozen);
    int animated = AnimatedByGame(h.Animatable);
    sprintf_s(h.Status, "Held%s%s", h.HasBox ? (boxIn ? " (and its box pose)" : " (not its box pose)") : "",
      animated == 0 ? ". The game doesn't animate its body now, so it doesn't take the pose" : "");
  }

  // The drones and the other plain actors within DroneScanRadius of the camera
  // with a body of their own (+ 0x100), into out (their entity, animatable and
  // distance); how many drones there are at all, how many with a body, and the
  // first body's bones.
  // The plain actors (class ClientActorEntity itself) are, by the game's data
  // (2026-11-12), the bear mascots, the pigeons and the drones that have no
  // AI: the tools had the bear's skeleton from the first build but never
  // listed one. Their bodies sit at the same place as a drone's (the class
  // the drone's is derived from). Weapons are actors of another class of
  // their own and aren't walked here: they go with their character.
  int WalkDrones(float const* camera, Candidate* out, int max, int& drones, int& withBody, int& firstBones)
  {
    struct ActorClass
    {
      __int64 Rva;
      char const* Name;
    };
    static const ActorClass classes[] = { { DroneTypeInfoRva, "PamClientDroneActorEntity" },
      { ActorTypeInfoRva, "ClientActorEntity" } };
    __int64 base = (__int64)g_gameHandle;
    int count = 0;
    drones = withBody = firstBones = 0;
    for (int k = 0; k < 2; ++k)
    {
      __int64 typeInfo = base + classes[k].Rva;
      if (!TypeNameIs(typeInfo, classes[k].Name))
        continue;
      __try
      {
        for (int list = 0; list < 4; ++list)
        {
          __int64 link = *(__int64*)(typeInfo + TypeInstanceListsOffset + list * 0x10);
          for (int n = 0; link && n < MaxPerList; ++n, link = *(__int64*)link)
          {
            __int64 entity = link - EntityLinkOffset;
            if (TypeOf(entity) != typeInfo)
              continue;
            float place[3];
            if (!EntityPlace(entity, place) || !(Distance3(place, camera) <= DroneScanRadius))
              continue;
            if (k == 0)
              ++drones;
            __int64 animatable = *(__int64*)(entity + ActorBodyOffset);
            LiveSlot slot{};
            if (!SlotOf(animatable, 0, slot))
              continue;
            if (k == 0 && !withBody++)
              firstBones = slot.Bones;
            if (count < max)
            {
              out[count] = Candidate{};
              out[count].Entity = entity;
              out[count].Animatable = animatable;
              out[count].Distance = Distance3(place, camera);
              ++count;
            }
          }
        }
      }
      __except (EXCEPTION_EXECUTE_HANDLER)
      {
      }
    }
    return count;
  }

  // People, drones and Faith that can be posed, nearest first. Game thread.
  void ScanCandidates(__int64 soldier, ULONGLONG now)
  {
    std::vector<Candidate> found;
    float camera[3] = { 0.f, 0.f, 0.f }, look[3] = { 0.f, 0.f, 1.f };
    bool haveCamera = false;
    {
      std::lock_guard<std::mutex> lock(g_ViewMutex);
      if (g_View.Ok)
      {
        camera[0] = g_View.T[12]; camera[1] = g_View.T[13]; camera[2] = g_View.T[14];
        look[0] = -g_View.T[8]; look[1] = -g_View.T[9]; look[2] = -g_View.T[10];
        haveCamera = true;
      }
    }
    float faithPlace[3];
    bool faithThere = soldier >= 0x10000 && EntityPlace(soldier, faithPlace);
    if (!haveCamera && faithThere)
    {
      memcpy(camera, faithPlace, sizeof(camera));
      haveCamera = true;
    }
    auto addOne = [&](int kind, __int64 entity, __int64 animatable, LiveSlot const& slot, float distance)
    {
      Candidate c{};
      c.Entity = entity;
      c.VTable = ReadVTable(entity);
      c.Animatable = animatable;
      c.Kind = kind;
      c.Distance = distance;
      c.Bones = slot.Bones;
      c.Skeleton = SkeletonOf(animatable, slot, g_Body, c.Cant);
      c.Animated = AnimatedByGame(animatable);
      __int64 entry = OverrideIn(slot);
      c.OtherOverride = entry != 0;
      for (int i = 0; i < MaxHeld; ++i)
        if (g_Held[i].Used && g_Held[i].Entity == entity)
        {
          c.Held = true;
          c.LettingGo = g_Held[i].Restoring;
          if (IsOurs(g_Held[i].Body, entry))
            c.OtherOverride = false;
        }
      found.push_back(c);
    };

    // Faith
    if (soldier >= 0x10000)
    {
      __int64 animatable = 0, boxAnimatable = 0;
      LiveSlot body{}, box{};
      bool boxOk = false;
      if (FaithSlots(soldier, animatable, body, boxAnimatable, box, boxOk))
        addOne(Kind_Faith, soldier, animatable, body, faithThere && haveCamera ? Distance3(faithPlace, camera) : 0.f);
    }

    // People: the attach scan's characters (not swarm members)
    if (haveCamera)
    {
      if (!g_ClassesFound)
      {
        bool fromFaith = false;
        g_ClassesFound = attach::FindClasses(soldier, fromFaith);
        if (!g_ClassesFound && !g_ClassesLogged)
        {
          util::log::Warning("Pose: the game's entity classes weren't found: only Faith and drones are listed");
          g_ClassesLogged = true;
        }
      }
      if (g_ClassesFound)
      {
        if (!g_Scan)
          g_Scan = std::make_unique<attach::Scan>();
        g_Found.resize(256);
        attach::View view{};
        view.Camera = DirectX::XMFLOAT3(camera[0], camera[1], camera[2]);
        Normalize3(look);
        view.Look = DirectX::XMFLOAT3(look[0], look[1], look[2]);
        view.Skip = soldier;
        int count = attach::ScanNearby(view, ScanRadius, g_Found.data(), (int)g_Found.size(), *g_Scan);
        for (int n = 0; n < count && (int)found.size() < MaxCandidates; ++n)
        {
          attach::Target const& target = g_Found[n];
          if (target.Agent >= 0 || target.Entity == soldier || strcmp(attach::KindOf(target), "character") != 0)
            continue;
          __int64 animatable = 0;
          LiveSlot slot{};
          if (!BodySlot(target.Entity, CharacterBodyOffset, animatable, slot))
            continue;
          float place[3];
          float distance = EntityPlace(target.Entity, place) ? Distance3(place, camera) : target.Distance;
          addOne(Kind_Person, target.Entity, animatable, slot, distance);
        }
      }

      // Drones: whether they have a body of their own wasn't known (the log
      // says, once)
      int drones = 0, withBody = 0, firstBones = 0;
      static Candidate droneFound[MaxCandidates];
      int droneCount = WalkDrones(camera, droneFound, MaxCandidates, drones, withBody, firstBones);
      for (int n = 0; n < droneCount && (int)found.size() < MaxCandidates; ++n)
      {
        Candidate& c = droneFound[n];
        LiveSlot slot{};
        __int64 animatable = 0;
        if (!BodySlot(c.Entity, ActorBodyOffset, animatable, slot) || animatable != c.Animatable)
          continue;
        addOne(Kind_Drone, c.Entity, animatable, slot, c.Distance);
      }
      if (util::DiagnosticsOn() && drones > 0 && !g_DronesLogged)
      {
        util::log::Write("Pose: %d drones within %.0f m of the camera, %d of them with an Ant body at + 0x100%s", drones,
          DroneScanRadius, withBody, withBody ? "" : " (so drones can't be posed this way)");
        if (withBody)
          util::log::Write("Pose:   the first drone's body has %d bones", firstBones);
        g_DronesLogged = true;
      }
    }
    std::stable_sort(found.begin(), found.end(), [](Candidate const& a, Candidate const& b)
    {
      if ((a.Kind == Kind_Faith) != (b.Kind == Kind_Faith))
        return a.Kind == Kind_Faith;
      return a.Distance < b.Distance;
    });
    g_Candidates.swap(found);
    g_ScannedAt = now;
  }

  // ---- Undo ----

  Snapshot SnapshotOf(Held const& h)
  {
    Snapshot s;
    s.Turns.assign(&h.Turn[0][0], &h.Turn[0][0] + (size_t)h.Bones * 4);
    s.Moves.assign(&h.Move[0][0], &h.Move[0][0] + (size_t)h.Bones * 3);
    for (int i = 0; i < h.WeaponCount; ++i)
    {
      HeldWeapon const& w = h.Weapons[i];
      s.Weapons.push_back((float)w.Bone);
      s.Weapons.insert(s.Weapons.end(), w.OffsetLocal, w.OffsetLocal + FloatsPerBone);
      s.Weapons.insert(s.Weapons.end(), w.OffsetWorld, w.OffsetWorld + FloatsPerBone);
      s.Weapons.insert(s.Weapons.end(), w.EditMove, w.EditMove + 3);
      s.Weapons.insert(s.Weapons.end(), w.EditTurn, w.EditTurn + 4);
    }
    s.KeysTime = h.Keys.Time;
    for (int l = 0; l < LimbCount; ++l)
    {
      KinPin const& pin = h.Pins[l];
      s.Pins.push_back(pin.On ? 1.f : 0.f);
      s.Pins.insert(s.Pins.end(), pin.Place, pin.Place + 3);
      s.Pins.insert(s.Pins.end(), pin.Turn, pin.Turn + 4);
      s.Pins.insert(s.Pins.end(), pin.Seen, pin.Seen + 3);
    }
    return s;
  }

  // A step of the history: this pose (or the one given, taken just before) with the keyframes and references now
  HistoryStep StepOf(Held const& h, Snapshot const& pose)
  {
    HistoryStep step;
    step.Pose = pose;
    step.Keys = h.Keys.Channels;
    step.Length = h.Keys.Length;
    step.RefTurns.assign(&h.KeyRefTurn[0][0], &h.KeyRefTurn[0][0] + (size_t)h.Bones * 4);
    step.RefMoves.assign(&h.KeyRefMove[0][0], &h.KeyRefMove[0][0] + (size_t)h.Bones * 3);
    step.On = h.Keys.On;
    step.Loop = h.Keys.Loop;
    step.Speed = h.Keys.Speed;
    step.Clock = h.Keys.Clock;
    step.Start = h.Keys.Start;
    step.Time = h.Keys.Time;
    return step;
  }

  // Whether a scalar field is being typed into right now (ctrl+click or double-click turned it into a text box): read
  // before the field is drawn, so a number typed in the frame the mouse is pressed still counts
  bool TypingInField()
  {
    ImGuiContext& g = *GImGui;
    return g.ActiveId != 0 && g.ActiveId == g.ScalarAsInputTextId;
  }

  // A drag field's new value counts on a drag or a typed number, not on a click alone (a click rounds the value to
  // what the field shows, ImGui 1.61, and returns it as changed)
  bool DragTakes(bool typing)
  {
    return typing || !ImGui::IsMouseDown(0) || ImGui::IsMouseDragging(0, 1.f);
  }

  void ForgetStash(Held& h)
  {
    h.FutureStash.clear();
    h.TrimmedStash = HistoryStep();
    h.TrimmedOk = false;
    h.StashOk = false;
  }

  // After a step was pushed onto History: the Redo steps it ends and the oldest step it trims (past MaxUndo) kept
  // aside until the step proves a change
  void NoteNewStep(Held& h)
  {
    h.FutureStash = std::move(h.Future);
    h.Future.clear();
    h.TrimmedOk = false;
    h.TrimmedStash = HistoryStep();
    if (h.History.size() > MaxUndo)
    {
      h.TrimmedStash = std::move(h.History.front());
      h.History.erase(h.History.begin());
      h.TrimmedOk = true;
    }
    h.StashOk = true;
  }

  // The last push's step taken off again, as if it had never been pushed (Redo and the oldest step put back)
  void DropLastStep(Held& h)
  {
    if (h.History.empty())
      return;
    h.History.pop_back();
    if (h.StashOk)
    {
      h.Future = std::move(h.FutureStash);
      if (h.TrimmedOk)
        h.History.insert(h.History.begin(), std::move(h.TrimmedStash));
    }
    ForgetStash(h);
  }

  bool SameKeys(std::vector<pose_keys::Channel> const& a, std::vector<pose_keys::Channel> const& b)
  {
    if (a.size() != b.size())
      return false;
    for (size_t c = 0; c < a.size(); ++c)
    {
      if (a[c].Bone != b[c].Bone || a[c].Keys.size() != b[c].Keys.size())
        return false;
      for (size_t k = 0; k < a[c].Keys.size(); ++k)
      {
        pose_keys::Key const& x = a[c].Keys[k];
        pose_keys::Key const& y = b[c].Keys[k];
        if (x.Time != y.Time || x.Ease != y.Ease || x.Auto != y.Auto || memcmp(x.Turn, y.Turn, sizeof(x.Turn)) != 0 ||
            memcmp(x.Move, y.Move, sizeof(x.Move)) != 0)
          return false;
      }
    }
    return true;
  }

  // Whether a step holds the pose and keyframes as they are now (its push changed nothing)
  bool StepIsNow(Held const& h, HistoryStep const& step)
  {
    if (step.Length != h.Keys.Length || !SameKeys(step.Keys, h.Keys.Channels))
      return false;
    // a keyframes file's load changes their settings and the time too (a reload of the same file with the playhead
    // at 0 leaves the keys and the pose as they were)
    if (step.Settings && (step.On != h.Keys.On || step.Loop != h.Keys.Loop || step.Speed != h.Keys.Speed ||
        step.Clock != h.Keys.Clock || step.Start != h.Keys.Start || step.Time != h.Keys.Time))
      return false;
    Snapshot now = SnapshotOf(h);
    if (now.Weapons != step.Pose.Weapons || now.Turns.size() != step.Pose.Turns.size() ||
        now.Moves.size() != step.Pose.Moves.size() || now.Turns.size() != (size_t)h.Bones * 4)
      return false;
    // With the keyframes on and their time moved since the step (playing), the bones with keyframes are theirs
    // (KeepKeys samples them again): only the others tell whether the step changed something
    bool timeMoved = h.Keys.On && step.Pose.KeysTime != h.Keys.Time;
    for (int b = 0; b < h.Bones; ++b)
    {
      if (timeMoved && pose_keys::Find(h.Keys, b) != nullptr)
        continue;
      for (int k = 0; k < 4; ++k)
        if (now.Turns[(size_t)b * 4 + k] != step.Pose.Turns[(size_t)b * 4 + k])
          return false;
      for (int k = 0; k < 3; ++k)
        if (now.Moves[(size_t)b * 3 + k] != step.Pose.Moves[(size_t)b * 3 + k])
          return false;
    }
    return true;
  }

  // Each frame (the overlay's, with AutoKeyChanges): once what pushed the last step is let go (no drag, no item
  // held), a step that changed nothing is dropped (a ring clicked and not turned, a slider or the Length clicked and
  // not moved, a Reset of what was already reset), so Redo stays; one that changed something is kept for good
  void SettleLastStep(Held& h)
  {
    if (!h.StashOk || !h.Used)
      return;
    if (h.History.empty())
    {
      ForgetStash(h);
      return;
    }
    int index = (int)(&h - g_Held);
    if ((g_Drag.On && g_Drag.Held == index) || ((g_Move.On || g_Move.Pending) && g_Move.Held == index))
      return;
    if ((g_KeyDrag.On || g_KeyDrag.Pending || g_KeyScrub) && g_KeyDrag.Held == h.Id)
      return;
    if (ImGui::IsAnyItemActive())
      return;
    if (StepIsNow(h, h.History.back()))
      DropLastStep(h);
    else
      ForgetStash(h);
  }

  // Whether a pose (a snapshot of it) has changes its keyframe references don't hold yet: changes not keyframed
  bool SnapshotHasUnkeyed(Held const& h, Snapshot const& s)
  {
    if (s.Turns.size() != (size_t)h.Bones * 4 || s.Moves.size() != (size_t)h.Bones * 3)
      return true;
    for (int n = 0; n < h.Count; ++n)
    {
      int bone = (int)h.List[n];
      for (int k = 0; k < 4; ++k)
        if (fabsf(s.Turns[(size_t)bone * 4 + k] - h.KeyRefTurn[bone][k]) > 1e-6f)
          return true;
      for (int k = 0; k < 3; ++k)
        if (fabsf(s.Moves[(size_t)bone * 3 + k] - h.KeyRefMove[bone][k]) > 1e-6f)
          return true;
    }
    return false;
  }

  // A step before a change of the pose: the pose as it was (s) and the keyframes as they are now. Auto key
  // keyframing the change before the time moves belongs to this step: PushKeyUndo(merge) then pushes nothing.
  // (The merge only when the pose pushed had no change not keyframed yet: else auto key would keyframe those older
  // changes into this step too, and one Undo would take back two changes; the review, 2026-11-15.)
  void PushUndo(Held& h, Snapshot const& s)
  {
    h.History.push_back(StepOf(h, s));
    NoteNewStep(h);
    h.HistoryHasKeys = !SnapshotHasUnkeyed(h, s);
  }

  void ApplySnapshot(Held& h, Snapshot const& s)
  {
    if (s.Turns.size() == (size_t)h.Bones * 4)
      memcpy(&h.Turn[0][0], s.Turns.data(), s.Turns.size() * sizeof(float));
    if (s.Moves.size() == (size_t)h.Bones * 3)
      memcpy(&h.Move[0][0], s.Moves.data(), s.Moves.size() * sizeof(float));
    if (s.Weapons.size() == (size_t)h.WeaponCount * WeaponSnapshotFloats)
      for (int i = 0; i < h.WeaponCount; ++i)
      {
        HeldWeapon& w = h.Weapons[i];
        float const* f = s.Weapons.data() + (size_t)i * WeaponSnapshotFloats;
        int bone = (int)f[0];
        // One the game took back (dropped or gone) stays the game's
        if (w.Gone && bone != -1)
          continue;
        w.Bone = bone;
        memcpy(w.OffsetLocal, f + 1, FloatsPerBone * sizeof(float));
        memcpy(w.OffsetWorld, f + 9, FloatsPerBone * sizeof(float));
        memcpy(w.EditMove, f + 17, 3 * sizeof(float));
        memcpy(w.EditTurn, f + 20, 4 * sizeof(float));
      }
    if (s.Pins.size() == (size_t)LimbCount * PinSnapshotFloats)
      for (int l = 0; l < LimbCount; ++l)
      {
        float const* f = s.Pins.data() + (size_t)l * PinSnapshotFloats;
        KinPin& pin = h.Pins[l];
        if (!pin.On || !(f[0] > 0.5f))
          continue;   // (on in both: its place then; else as it is, StartHold's check takes it from the pose)
        memcpy(pin.Place, f + 1, sizeof(pin.Place));
        memcpy(pin.Turn, f + 4, sizeof(pin.Turn));
        memcpy(pin.Seen, f + 8, sizeof(pin.Seen));
      }
  }

  // How far a bone can be moved: the held root (the whole body) as far as
  // anyone would go (the user, 2026-11-08: "remove the limitation ... make it
  // infinite"): 100 km (the city is a few km across), so no sum overflows;
  // a face bone MaxFaceMove, any other (Free move) MaxFreeMove
  const float MaxRootMove = 1e5f;
  float MoveLimit(Held const& h, int bone)
  {
    return bone == h.Root ? MaxRootMove : (h.Face[bone] ? MaxFaceMove : MaxFreeMove);
  }

  // Within its limit; a move that isn't a number (or is endless) is none
  void ClampMove(Held& h, int bone)
  {
    float limit = MoveLimit(h, bone);
    for (int k = 0; k < 3; ++k)
    {
      float& m = h.Move[bone][k];
      if (!std::isfinite(m))
        m = 0.f;
      m = m < -limit ? -limit : (m > limit ? limit : m);
    }
  }

  bool IsEdited(Held const& h, int bone)
  {
    return !QIsIdentity(h.Turn[bone]) || h.Move[bone][0] != 0.f || h.Move[bone][1] != 0.f || h.Move[bone][2] != 0.f;
  }

  // ---- The keyframes (PLAN.md 2.2 - 2.5) ----

  bool KeyRefDiffers(Held const& h, int bone)
  {
    for (int k = 0; k < 4; ++k)
      if (fabsf(h.Turn[bone][k] - h.KeyRefTurn[bone][k]) > 1e-6f)
        return true;
    for (int k = 0; k < 3; ++k)
      if (fabsf(h.Move[bone][k] - h.KeyRefMove[bone][k]) > 1e-6f)
        return true;
    return false;
  }

  bool KeysCurrent(Held const& h)
  {
    return h.Keys.Channels.empty() || (h.KeysAppliedOk && h.Keys.Time == h.KeysAppliedTime &&
      h.Keys.Revision == h.KeysAppliedRevision);
  }

  bool ClipUsable(Held const& h)
  {
    return h.Clip.On && h.Clip.Clip && h.Clip.Fit.Ok;
  }

  bool TrackHoldsTime(Held const& h)
  {
    if (h.Keys.Clock == pose_keys::Clock_Track)
      return g_ClipTrackTime >= 0.0;
    if (h.Keys.Clock == pose_keys::Clock_Clip)
      return !h.Keys.Channels.empty() && ClipUsable(h) && h.Clip.Clock == ClipTrackTime && h.Clip.Playing &&
        h.Keys.Playing;
    return false;
  }

  bool PlayingKeys(Held const& h)
  {
    if (!h.Keys.On || h.Keys.Channels.empty())
      return false;
    if (h.Keys.Clock == pose_keys::Clock_Game || h.Keys.Clock == pose_keys::Clock_Real)
      return h.Keys.Playing;
    if (h.Keys.Clock == pose_keys::Clock_Track)
      return h.Keys.Playing && h.KeyTrackMoving;
    if (h.Keys.Clock == pose_keys::Clock_Clip)
      return h.Keys.Playing && ClipUsable(h) && h.Clip.Playing;
    return false;
  }

  // A step before a change of the keyframes: the pose and the keyframes as they are now. merge (auto key keyframing
  // a change): none when the last step was pushed before that change of the pose and still holds the keyframes as
  // they are, so the change and its keyframe go in one Undo. Every other change of the keyframes is a step of its own
  // (a drag in the timeline starts from it every frame, and drops it if the key ends where it was).
  void PushKeyUndo(Held& h, bool merge)
  {
    if (merge && h.HistoryHasKeys && !h.History.empty())
    {
      h.HistoryHasKeys = false;
      return;
    }
    h.History.push_back(StepOf(h, SnapshotOf(h)));
    NoteNewStep(h);
    h.HistoryHasKeys = false;
  }

  void KeysMatchPose(Held& h)
  {
    h.KeysAppliedTime = h.Keys.Time;
    h.KeysAppliedRevision = h.Keys.Revision;
    h.KeysAppliedOk = true;
  }

  int ChangedBones(Held const& h, int* out)
  {
    int count = 0;
    for (int n = 0; n < h.Count; ++n)
    {
      int bone = (int)h.List[n];
      if (KeyRefDiffers(h, bone))
        out[count++] = bone;
    }
    return count;
  }

  // A bone keyframed at the playhead. Its first keyframe while the pose has keyframes at other times (with
  // g_KeyAnchors): also keyframes holding it as it was (its reference) at the nearest keyframe time of any bone
  // before and after, so the change shows only between them; those are Auto (drawn hollow). Returns how many holds.
  // KeyBoneIn: those keys made in into (all: the keyframes they're made among, h.Keys; the motion trail lays them in a
  // copy of a few channels, 2026-11-16: into has the bone's channel if all has it)
  int KeyBoneIn(Held const& h, pose_keys::Anim const& all, pose_keys::Anim& into, int bone)
  {
    if (bone < 0 || bone >= h.Bones || !h.InHeld[bone])
      return 0;
    int holds = 0;
    if (g_KeyAnchors && !all.Channels.empty() && pose_keys::Find(all, bone) == nullptr)
    {
      double before = 0.0;
      if (pose_keys::KeyTimeBefore(all, all.Time, before) &&
          pose_keys::SetKey(into, bone, before, h.KeyRefTurn[bone], h.KeyRefMove[bone], -1, true) >= 0)
        ++holds;
      double after = 0.0;
      if (pose_keys::KeyTimeAfter(all, all.Time, after) &&
          pose_keys::SetKey(into, bone, after, h.KeyRefTurn[bone], h.KeyRefMove[bone], -1, true) >= 0)
        ++holds;
    }
    pose_keys::SetKey(into, bone, all.Time, h.Turn[bone], h.Move[bone], -1);
    return holds;
  }

  int KeyBone(Held& h, int bone)
  {
    if (bone < 0 || bone >= h.Bones || !h.InHeld[bone])
      return 0;
    int holds = KeyBoneIn(h, h.Keys, h.Keys, bone);
    memcpy(h.KeyRefTurn[bone], h.Turn[bone], sizeof(h.KeyRefTurn[bone]));
    memcpy(h.KeyRefMove[bone], h.Move[bone], sizeof(h.KeyRefMove[bone]));
    return holds;
  }

  // What the last keyframing did, shown under the Keyframe: row (the user, 2026-11-15: auto key keyframed without
  // saying so). count bones keyframed at the playhead, holds: the hollow keyframes made with them (KeyBone).
  void NoteKeys(Held& h, int const* bones, int count, int holds, bool automatic)
  {
    if (count <= 0)
      return;
    char what[96];
    if (count == 1)
      _snprintf_s(what, sizeof(what), _TRUNCATE, "%s", BoneName(h, bones[0]));
    else
      _snprintf_s(what, sizeof(what), _TRUNCATE, "%d bones", count);
    _snprintf_s(g_KeyNote, sizeof(g_KeyNote), _TRUNCATE, "%s %s at %.2f s", automatic ? "Auto key: keyframed" :
      "Keyframed", what, h.Keys.Time);
    size_t used = strlen(g_KeyNote);
    if (holds > 0 && count == 1)
      _snprintf_s(g_KeyNote + used, sizeof(g_KeyNote) - used, _TRUNCATE, ", held as it was at the %s around it (%d "
        "hollow)", holds == 1 ? "keyframe time" : "keyframe times", holds);
    else if (holds > 0)
      _snprintf_s(g_KeyNote + used, sizeof(g_KeyNote) - used, _TRUNCATE, ", the new ones held as they were at the "
        "keyframe times around (%d hollow)", holds);
    g_KeyNoteAt = GetTickCount64();
    g_KeyNoteHeld = h.Id;
  }

  // These bones keyframed at the playhead as one Undo step (merge: into the step pushed before the change they
  // keyframe, see PushKeyUndo), said under the Keyframe: row. Only with KeysCurrent (the references are the pose
  // made from the keyframes now). Returns count.
  int KeyBones(Held& h, int const* bones, int count, bool automatic, bool merge)
  {
    if (count <= 0)
      return 0;
    PushKeyUndo(h, merge);
    int holds = 0;
    for (int k = 0; k < count; ++k)
      holds += KeyBone(h, bones[k]);
    KeysMatchPose(h);
    NoteKeys(h, bones, count, holds, automatic);
    return count;
  }

  int KeyChanges(Held& h, bool automatic)
  {
    if (!KeysCurrent(h))
      return 0;
    int bones[MaxBones];
    int count = ChangedBones(h, bones);
    if (count == 0)
      return 0;
    return KeyBones(h, bones, count, automatic, automatic);
  }

  // Auto key: what is still un-keyed is keyframed now, before the time moves or playing starts (from the first
  // change: since 2026-11-15 a pose with no keyframes yet is keyframed too, as the red Auto key says)
  void CommitAutoKey(Held& h)
  {
    if (g_AutoKey && h.Keys.On && !PlayingKeys(h))
      KeyChanges(h, true);
  }

  int SelectedBones(Held const& h, int* out)
  {
    if (g_SelectedWeapon >= 0)
      return 0;
    int count = 0;
    if (g_SelectedGroup >= 0)
    {
      if (g_SelectedGroup >= FaceGroupCount)
        return 0;
      for (int k = 0; k < h.GroupSize[g_SelectedGroup]; ++k)
      {
        int bone = h.GroupMembers[g_SelectedGroup][k];
        if (bone >= 0 && bone < h.Bones && h.InHeld[bone])
          out[count++] = bone;
      }
      return count;
    }
    if (g_SelectedBone >= 0 && g_SelectedBone < h.Bones && h.InHeld[g_SelectedBone])
    {
      out[0] = g_SelectedBone;
      return 1;
    }
    return 0;
  }

  int KeySelection(Held& h)
  {
    if (!KeysCurrent(h))
      return 0;
    int bones[MaxBones];
    int count = SelectedBones(h, bones);
    if (count == 0)
      return 0;
    bool anyChange = false;
    for (int k = 0; k < count; ++k)
    {
      pose_keys::Channel* channel = pose_keys::Find(h.Keys, bones[k]);
      int key = channel ? pose_keys::KeyAt(*channel, h.Keys.Time) : -1;
      if (channel == nullptr || key < 0 || channel->Keys[(size_t)key].Auto || KeyRefDiffers(h, bones[k]))
      {
        anyChange = true;
        break;
      }
    }
    if (!anyChange)
      return 0;
    return KeyBones(h, bones, count, false, false);
  }

  int KeyWholePose(Held& h)
  {
    if (!KeysCurrent(h))
      return 0;
    int bones[MaxBones];
    int count = 0;
    for (int n = 0; n < h.Count; ++n)
    {
      int bone = (int)h.List[n];
      if (pose_keys::Find(h.Keys, bone) != nullptr || IsEdited(h, bone) || KeyRefDiffers(h, bone))
        bones[count++] = bone;
    }
    if (count == 0)
      return 0;
    bool anyChange = false;
    for (int k = 0; k < count; ++k)
    {
      pose_keys::Channel* channel = pose_keys::Find(h.Keys, bones[k]);
      int key = channel ? pose_keys::KeyAt(*channel, h.Keys.Time) : -1;
      if (channel == nullptr || key < 0 || channel->Keys[(size_t)key].Auto || KeyRefDiffers(h, bones[k]))
      {
        anyChange = true;
        break;
      }
    }
    if (!anyChange)
      return 0;
    return KeyBones(h, bones, count, false, false);
  }

  int SelectedKeysAtPlayhead(Held const& h)
  {
    int bones[MaxBones];
    int count = SelectedBones(h, bones);
    int keys = 0;
    for (int k = 0; k < count; ++k)
    {
      pose_keys::Channel const* channel = pose_keys::Find(h.Keys, bones[k]);
      if (channel != nullptr && pose_keys::KeyAt(*channel, h.Keys.Time) >= 0)
        ++keys;
    }
    return keys;
  }

  int RemoveSelectedKeys(Held& h)
  {
    if (!KeysCurrent(h))
      return 0;
    int bones[MaxBones];
    int count = SelectedBones(h, bones);
    if (count == 0)
      return 0;
    int found = 0;
    for (int k = 0; k < count; ++k)
    {
      pose_keys::Channel* channel = pose_keys::Find(h.Keys, bones[k]);
      if (channel != nullptr && pose_keys::KeyAt(*channel, h.Keys.Time) >= 0)
        ++found;
    }
    if (found == 0)
      return 0;
    PushKeyUndo(h);
    int removed = 0;
    for (int k = 0; k < count; ++k)
    {
      pose_keys::Channel* channel = pose_keys::Find(h.Keys, bones[k]);
      if (channel == nullptr)
        continue;
      int index = pose_keys::KeyAt(*channel, h.Keys.Time);
      if (index >= 0 && pose_keys::RemoveKey(h.Keys, bones[k], index))
        ++removed;
    }
    if (h.Keys.Channels.empty())
      h.Keys.Playing = false;
    return removed;
  }

  // Undo (redo: Redo): the pose and its keyframes as they were before the last change (as the last Undo took them
  // back), together, so a drag and the keyframe auto key made of it go in one step. With Auto key off and the
  // playhead at the time the step was taken at, all is as it was then (the changes not keyframed then are again).
  // Otherwise the bones with keyframes are read again from them where the playhead is (they own those bones there),
  // the others are as they were, and no change counts as not keyframed.
  bool UndoStep(Held& h, bool redo)
  {
    std::vector<HistoryStep>& from = redo ? h.Future : h.History;
    std::vector<HistoryStep>& to = redo ? h.History : h.Future;
    if (from.empty())
      return false;
    ForgetStash(h);
    HistoryStep step = std::move(from.back());
    from.pop_back();
    HistoryStep now = StepOf(h, SnapshotOf(h));
    now.Settings = step.Settings;   // (Redo of a load puts the file's settings back)
    to.push_back(std::move(now));
    if (to.size() > MaxUndo)
      to.erase(to.begin());
    if (step.Settings)
    {
      h.Keys.On = step.On;
      h.Keys.Loop = step.Loop;
      h.Keys.Speed = step.Speed;
      h.Keys.Clock = step.Clock;
      h.Keys.Start = step.Start;
      h.Keys.Playing = false;
      h.Keys.Time = step.Time < 0.0 ? 0.0 : (step.Time > step.Length ? step.Length : step.Time);
    }
    bool samePlace = step.Pose.KeysTime == h.Keys.Time && step.RefTurns.size() == (size_t)h.Bones * 4 &&
      step.RefMoves.size() == (size_t)h.Bones * 3;
    ApplySnapshot(h, step.Pose);
    h.Keys.Channels = std::move(step.Keys);
    h.Keys.Length = step.Length;
    ++h.Keys.Revision;
    if (h.Keys.Channels.empty())
      h.Keys.Playing = false;
    // Not with Auto key on: it would keyframe again at once what this took back when the step was taken after the
    // change (a step auto key pushed itself, with no step before the change, holds the changed pose)
    if (samePlace && !g_AutoKey)
    {
      memcpy(&h.KeyRefTurn[0][0], step.RefTurns.data(), step.RefTurns.size() * sizeof(float));
      memcpy(&h.KeyRefMove[0][0], step.RefMoves.data(), step.RefMoves.size() * sizeof(float));
      KeysMatchPose(h);
    }
    else
    {
      for (int bone = 0; bone < h.Bones; ++bone)
      {
        memcpy(h.KeyRefTurn[bone], h.Turn[bone], sizeof(h.KeyRefTurn[bone]));
        memcpy(h.KeyRefMove[bone], h.Move[bone], sizeof(h.KeyRefMove[bone]));
      }
      h.KeysAppliedOk = false;
    }
    h.HistoryHasKeys = false;
    return true;
  }

  void RemoveAllKeys(Held& h)
  {
    if (h.Keys.Channels.empty())
      return;
    PushKeyUndo(h);
    h.Keys.Channels.clear();
    ++h.Keys.Revision;
    h.Keys.Playing = false;
  }

  void SetPlayhead(Held& h, double t)
  {
    if (TrackHoldsTime(h) && h.Keys.Playing)
      return;
    CommitAutoKey(h);
    h.HistoryHasKeys = false;
    if (!std::isfinite(t) || h.Keys.Length <= 0.0)
    {
      h.Keys.Time = 0.0;
      return;
    }
    if (t < 0.0)
      t = 0.0;
    if (t > h.Keys.Length)
      t = h.Keys.Length;
    h.Keys.Time = t;
    ClipPlay& c = h.Clip;
    if (h.Keys.Clock == pose_keys::Clock_Clip && ClipUsable(h) && h.Keys.Speed > 0.f)
    {
      double length = c.Clip->Length();
      double clipTime = (t - h.Keys.Start) / (double)h.Keys.Speed;
      if (clipTime < 0.0)
        clipTime = 0.0;
      if (clipTime > length)
        clipTime = length;
      if (c.Loop && length > 0.0 && clipTime >= length)
        clipTime = std::nextafter(length, 0.0);
      c.Time = clipTime;
      h.Keys.Time = h.Keys.Start + c.Time * (double)h.Keys.Speed;
    }
  }

  // Play / Pause, as the Pose window's button and its hotkey do it (on the clip's clock the clip with them)
  void TogglePlay(Held& h)
  {
    if (pose_keys::KeyCount(h.Keys) == 0 || h.Keys.Clock == pose_keys::Clock_Track)
      return;
    bool clipClock = h.Keys.Clock == pose_keys::Clock_Clip && ClipUsable(h);
    bool shownPlaying = clipClock ? (h.Keys.Playing && h.Clip.Playing) : h.Keys.Playing;
    bool wasOn = h.Keys.On;
    CommitAutoKey(h);
    h.Keys.Playing = !shownPlaying;
    if (h.Keys.Playing)
      h.KeysTimeMoved = true;   // (until the next KeepKeys measures it: the Bone tab's line doesn't flash "held")
    h.Keys.On = true;
    if (!wasOn)
      h.KeysAppliedOk = false;
    if (h.Keys.Playing && (h.Keys.Clock == pose_keys::Clock_Game || h.Keys.Clock == pose_keys::Clock_Real) &&
        !h.Keys.Loop && h.Keys.Time >= h.Keys.Length)
      SetPlayhead(h, 0.0);
    if (clipClock)
    {
      ClipPlay& c = h.Clip;
      c.Playing = h.Keys.Playing;
      if (c.Playing && !c.Loop && c.Clock != ClipTrackTime && c.Time >= c.Clip->Length())
        c.Time = 0.0;   // as the Clip tab's own Play does
    }
  }

  // To the keyframe before (direction < 0) or the next one; with none that way, the start or the end
  void JumpToKey(Held& h, int direction)
  {
    if (pose_keys::KeyCount(h.Keys) == 0 || TrackHoldsTime(h))
      return;
    double found = 0.0;
    if (direction < 0)
      SetPlayhead(h, pose_keys::KeyTimeBefore(h.Keys, h.Keys.Time, found) ? found : 0.0);
    else
      SetPlayhead(h, pose_keys::KeyTimeAfter(h.Keys, h.Keys.Time, found) ? found : h.Keys.Length);
  }

  // One frame back or on: a 30th of a second (the game's tick), while not playing
  void StepFrame(Held& h, int direction)
  {
    if (TrackHoldsTime(h) || PlayingKeys(h))
      return;
    SetPlayhead(h, h.Keys.Time + (direction < 0 ? -1.0 : 1.0) / 30.0);
  }

  // A hotkey's key name ("" with none bound, or no input yet)
  std::string HotkeyName(Action action)
  {
    if (!g_mainHandle || !g_mainHandle->GetInputSystem())
      return std::string();
    return g_mainHandle->GetInputSystem()->GetKeyName(action);
  }

  // The last item's tooltip, with its hotkey when one is bound
  void ItemTooltipKey(char const* text, Action action)
  {
    std::string key = HotkeyName(action);
    if (key.empty())
      ImGui::ItemTooltip(text);
    else
    {
      char line[200];
      _snprintf_s(line, sizeof(line), _TRUNCATE, "%s (%s)", text, key.c_str());
      ImGui::ItemTooltip(line);
    }
  }

  // Undo and Redo: one history for the pose and its keyframes (id: the place's own, two places draw them)
  void DrawUndoButtons(Held& h, char const* id)
  {
    char label[64];
    _snprintf_s(label, sizeof(label), _TRUNCATE, "Undo (%d)###%sUndo", (int)h.History.size(), id);
    SameLineIfFits(label);
    bool gate = !h.History.empty();
    if (!gate)
      ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.5f);
    bool press = ImGui::Button(label);
    if (!gate)
      ImGui::PopStyleVar();
    if (press && gate)
      UndoStep(h, false);
    ItemTooltipKey("Takes back the last change of the pose or its keyframes, both together", Action::Pose_Undo);
    _snprintf_s(label, sizeof(label), _TRUNCATE, "Redo (%d)###%sRedo", (int)h.Future.size(), id);
    SameLineIfFits(label);
    gate = !h.Future.empty();
    if (!gate)
      ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.5f);
    press = ImGui::Button(label);
    if (!gate)
      ImGui::PopStyleVar();
    if (press && gate)
      UndoStep(h, true);
    ItemTooltipKey("Puts back what Undo took back", Action::Pose_Redo);
  }

  // The Pose window's hotkeys (rebound in the tools' Hotkeys; by default numpad keys the camera's and the game's saved
  // binds don't use, its default keys unknown): while the window is open, for the pose it edits, not during a drag
  void PoseHotkeys()
  {
    if (!g_WindowOpen || g_Active < 0 || g_Active >= MaxHeld || !g_mainHandle || !g_mainHandle->GetInputSystem())
      return;
    InputSystem* input = g_mainHandle->GetInputSystem();
    Held& h = g_Held[g_Active];
    if (!h.Used || h.Restoring)
      return;
    if (g_Drag.On || g_Move.On || g_Move.Pending || g_KeyDrag.On || g_KeyDrag.Pending || g_KeyScrub)
      return;
    if (input->WentDown(Action::Pose_Redo))
      UndoStep(h, true);
    else if (input->WentDown(Action::Pose_Undo))
      UndoStep(h, false);
    if (input->WentDown(Action::Pose_Play))
      TogglePlay(h);
    // As the buttons: not while playing or with the keyframes off; the whole pose once something is posed or keyed,
    // Remove with a picked bone's keyframe at the playhead
    bool canKey = h.Keys.On && !PlayingKeys(h);
    if (canKey && input->WentDown(Action::Pose_KeyChanges))
      KeyChanges(h);
    if (canKey && input->WentDown(Action::Pose_KeyWholePose))
    {
      bool wholeAny = false;
      for (int n = 0; n < h.Count && !wholeAny; ++n)
      {
        int b = (int)h.List[n];
        wholeAny = pose_keys::Find(h.Keys, b) != nullptr || IsEdited(h, b) || KeyRefDiffers(h, b);
      }
      if (wholeAny)
        KeyWholePose(h);
    }
    if (canKey && input->WentDown(Action::Pose_RemoveKey) && SelectedKeysAtPlayhead(h) > 0)
      RemoveSelectedKeys(h);
    if (input->WentDown(Action::Pose_PreviousKey))
      JumpToKey(h, -1);
    if (input->WentDown(Action::Pose_NextKey))
      JumpToKey(h, 1);
    if (input->WentDown(Action::Pose_FrameBack))
      StepFrame(h, -1);
    if (input->WentDown(Action::Pose_FrameOn))
      StepFrame(h, 1);
  }

  void AutoKeyChanges()
  {
    if (!g_AutoKey || !g_WindowOpen || g_Active < 0 || g_Active >= MaxHeld)
      return;
    Held& h = g_Held[g_Active];
    if (!h.Used || h.Restoring || !h.Keys.On || PlayingKeys(h) || !KeysCurrent(h))
      return;
    if (g_Drag.On || g_Move.On || g_Move.Pending)
      return;
    if (ImGui::IsAnyItemActive())
      return;
    int bones[MaxBones];
    int count = ChangedBones(h, bones);
    if (count == 0)
      return;
    KeyBones(h, bones, count, true, true);
  }

  void KeepKeys(Held& h)
  {
    pose_keys::Anim& a = h.Keys;
    h.KeyTrackMoving = g_ClipTrackTime >= 0.0 && h.KeyTrackTimeBefore >= 0.0 &&
      g_ClipTrackTime != h.KeyTrackTimeBefore;
    h.KeyTrackTimeBefore = g_ClipTrackTime;
    h.KeysTimeMoved = false;
    h.KeysDt = 0.0;
    if (a.Channels.empty())
      return;
    if (a.Clock == pose_keys::Clock_Track)
      a.Playing = true;   // the track plays it: this clock has no pause of its own
    ClipPlay const& c = h.Clip;
    double timeBefore = a.Time;
    pose_keys::Advance(a, g_ClipRealDt, g_ClipGameScale, g_ClipTrackTime, c.Time, ClipUsable(h));
    h.KeysTimeMoved = a.Time != timeBefore;
    // How far it moved (the follow-through's springs step by it): a loop's wrap is time going on, not back
    double dt = a.Time - timeBefore;
    if (dt < 0.0 && a.Loop && a.Length > 0.0 && timeBefore - a.Time > 0.5 * a.Length)
      dt += a.Length;
    h.KeysDt = std::isfinite(dt) && dt > 0.0 ? (dt > 0.25 ? 0.25 : dt) : 0.0;
    if (!a.On)
      return;
    int index = (int)(&h - g_Held);
    if ((g_Drag.On && g_Drag.Held == index) || ((g_Move.On || g_Move.Pending) && g_Move.Held == index))
      return;
    ApplyKeysNow(h);
  }

  // The keyframes' pose at the playhead into the edit, as KeepKeys does it each frame (2026-11-16: also at once when
  // a dot of the picked bone's path puts the playhead at its time, before its drag starts from that pose): with the
  // time moved, every keyed bone from its keys and every reference the pose; with only the keys changed, the keyed
  // bones that have no change of their own not keyframed yet
  void ApplyKeysNow(Held& h)
  {
    pose_keys::Anim& a = h.Keys;
    if (!a.On || a.Channels.empty())
      return;
    bool moved = !h.KeysAppliedOk || a.Time != h.KeysAppliedTime;
    if (!moved && a.Revision == h.KeysAppliedRevision)
      return;
    for (size_t channel = 0; channel < a.Channels.size(); ++channel)
    {
      int bone = a.Channels[channel].Bone;
      if (bone < 0 || bone >= h.Bones || !h.InHeld[bone])
        continue;
      if (!moved && KeyRefDiffers(h, bone))
        continue;   // an un-keyed change stays while only keys changed
      float turn[4], move[3];
      if (!pose_keys::Sample(a.Channels[channel], a.Time, turn, move, a.Loop ? a.Length : 0.0))
        continue;
      memcpy(h.Turn[bone], turn, sizeof(turn));
      memcpy(h.Move[bone], move, sizeof(move));
      ClampMove(h, bone);
      QNorm(h.Turn[bone]);
      memcpy(h.KeyRefTurn[bone], h.Turn[bone], sizeof(turn));
      memcpy(h.KeyRefMove[bone], h.Move[bone], sizeof(move));
    }
    if (moved)
    {
      for (int bone = 0; bone < h.Bones; ++bone)
      {
        memcpy(h.KeyRefTurn[bone], h.Turn[bone], 4 * sizeof(float));
        memcpy(h.KeyRefMove[bone], h.Move[bone], 3 * sizeof(float));
      }
    }
    h.KeysAppliedTime = a.Time;
    h.KeysAppliedRevision = a.Revision;
    h.KeysAppliedOk = true;
  }

  void ResetBone(Held& h, int bone)
  {
    QIdentity(h.Turn[bone]);
    h.Move[bone][0] = h.Move[bone][1] = h.Move[bone][2] = 0.f;
  }

  // A Reset button (bones: which it resets, MaxBones of them; weapons: Reset all's weapons too). A bone with
  // keyframes goes back to them when the time moves (they own it), so a reset of it alone doesn't last: with any
  // such among them it asks first (DrawResetAsk: keyframe the reset at the playhead, or take their keyframes away);
  // with none, it resets at once.
  void RequestReset(Held& h, bool const* bones, bool weapons, Snapshot const& before, char const* what)
  {
    int keyed = 0;
    int first = -1;
    for (int b = 0; b < h.Bones; ++b)
      if (bones[b] && pose_keys::Find(h.Keys, b) != nullptr)
      {
        if (first < 0)
          first = b;
        ++keyed;
      }
    if (keyed == 0 || !h.Keys.On)
    {
      PushUndo(h, before);
      for (int b = 0; b < h.Bones; ++b)
        if (bones[b])
          ResetBone(h, b);
      if (weapons)
        for (int i = 0; i < h.WeaponCount; ++i)
          ResetWeapon(h.Weapons[i]);
      if (StepIsNow(h, h.History.back()))
        DropLastStep(h);   // nothing was posed: no step (and Redo kept)
      if (keyed > 0)
      {
        // (the keyframes are off: they put nothing back until Use the keyframes is ticked again; only the keyed
        // bones go back then)
        if (keyed == 1)
          _snprintf_s(g_KeyNote, sizeof(g_KeyNote), _TRUNCATE, "Reset %s; the keyframes are off: %s has keyframes, "
            "and they put it back once Use the keyframes is ticked", what, BoneName(h, first));
        else
          _snprintf_s(g_KeyNote, sizeof(g_KeyNote), _TRUNCATE, "Reset %s; the keyframes are off: %d of these bones "
            "have keyframes, and they put those back once Use the keyframes is ticked", what, keyed);
        g_KeyNoteAt = GetTickCount64();
        g_KeyNoteHeld = h.Id;
      }
      return;
    }
    ResetAsk& a = g_ResetAsk;
    a.HeldId = h.Id;
    memcpy(a.Bones, bones, sizeof(a.Bones));
    a.Weapons = weapons;
    a.Keyed = keyed;
    a.First = first;
    strncpy_s(a.What, what, _TRUNCATE);
    g_ResetAskOpen = true;
  }

  // The Reset question answered: the bones reset, then (keyframe) their reset keyframed at the playhead (the
  // keyframed ones; with Auto key on the others it changed too, as auto key would), or their keyframes taken away;
  // all one Undo step.
  void DoReset(Held& h, bool keyframe)
  {
    ResetAsk const& a = g_ResetAsk;
    bool current = KeysCurrent(h);
    PushUndo(h, SnapshotOf(h));
    for (int b = 0; b < h.Bones; ++b)
      if (a.Bones[b])
        ResetBone(h, b);
    if (a.Weapons)
      for (int i = 0; i < h.WeaponCount; ++i)
        ResetWeapon(h.Weapons[i]);
    h.HistoryHasKeys = false;   // the step just pushed holds the keyframes as they are: their change is in it
    if (keyframe)
    {
      int list[MaxBones];
      int count = 0;
      int holds = 0;
      for (int b = 0; b < h.Bones; ++b)
      {
        if (!a.Bones[b] || !h.InHeld[b])
          continue;
        bool keyed = pose_keys::Find(h.Keys, b) != nullptr;
        // a bone with no keyframes needs its reference for its holds: only when it is the pose's now (current)
        if (keyed || (g_AutoKey && current && KeyRefDiffers(h, b)))
        {
          holds += KeyBone(h, b);
          list[count++] = b;
        }
      }
      if (current)
        KeysMatchPose(h);
      NoteKeys(h, list, count, holds, false);
    }
    else
    {
      int removed = 0;
      for (int b = 0; b < h.Bones; ++b)
      {
        if (!a.Bones[b])
          continue;
        if (pose_keys::RemoveChannel(h.Keys, b))
          ++removed;
        // not a change to keyframe (auto key): it stays so at every time now
        memcpy(h.KeyRefTurn[b], h.Turn[b], sizeof(h.KeyRefTurn[b]));
        memcpy(h.KeyRefMove[b], h.Move[b], sizeof(h.KeyRefMove[b]));
      }
      if (h.Keys.Channels.empty())
        h.Keys.Playing = false;
      _snprintf_s(g_KeyNote, sizeof(g_KeyNote), _TRUNCATE, "Reset %s and took the keyframes of %d %s away", a.What,
        removed, removed == 1 ? "bone" : "bones");
      g_KeyNoteAt = GetTickCount64();
      g_KeyNoteHeld = h.Id;
    }
  }

  // Shown on the picture and in the list (no search): its kind is shown, and
  // with the face grouped, a face bone that's in a group or below one isn't
  // (its group is)
  bool BoneShown(Held const& h, int bone)
  {
    if (h.Face[bone] && !g_ShowFace)
      return false;
    if (h.Finger[bone] && !g_ShowFingers)
      return false;
    if (h.Helper[bone] && !g_ShowHelpers)
      return false;
    if (g_GroupFace && (h.Group[bone] >= 0 || h.FollowsGroup[bone]))
      return false;
    return true;
  }

  bool GroupShown(Held const& h, int group)
  {
    return g_ShowFace && g_GroupFace && group >= 0 && group < FaceGroupCount && h.GroupSize[group] > 0;
  }

  bool GroupEdited(Held const& h, int group)
  {
    for (int k = 0; k < h.GroupSize[group]; ++k)
      if (IsEdited(h, h.GroupMembers[group][k]))
        return true;
    return false;
  }

  // The middle of a group's members (world places in w)
  void GroupMiddle(Held const& h, int group, float (*w)[FloatsPerBone], float* out)
  {
    out[0] = out[1] = out[2] = 0.f;
    int n = h.GroupSize[group];
    for (int k = 0; k < n; ++k)
      for (int a = 0; a < 3; ++a)
        out[a] += w[h.GroupMembers[group][k]][a];
    if (n > 0)
      for (int a = 0; a < 3; ++a)
        out[a] /= (float)n;
  }

  // ---- Saved poses: Cinematic Tools\Poses\<name>.txt, a held bone a line:
  // its name, its local rotation (x y z w) and translation (x y z), as the
  // pose has them ----

  void ListPoseFiles()
  {
    g_PoseFiles.clear();
    WIN32_FIND_DATAA data;
    char pattern[MAX_PATH];
    sprintf_s(pattern, "%s\\*.txt", PoseFolder);
    HANDLE find = FindFirstFileA(pattern, &data);
    if (find != INVALID_HANDLE_VALUE)
    {
      do
      {
        if (!(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
        {
          std::string name = data.cFileName;
          if (name.size() > 4)
            g_PoseFiles.push_back(name.substr(0, name.size() - 4));
        }
      } while (FindNextFileA(find, &data));
      FindClose(find);
    }
    std::sort(g_PoseFiles.begin(), g_PoseFiles.end());
  }

  bool CleanName(char const* in, char* out, size_t size)
  {
    size_t n = 0;
    for (char const* p = in; *p && n + 1 < size; ++p)
    {
      char c = *p;
      if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == ' ' || c == '-' || c == '_')
        out[n++] = c;
    }
    while (n > 0 && out[n - 1] == ' ')
      --n;
    out[n] = 0;
    return n > 0;
  }

  void SavePose(Held const& h, char const* name)
  {
    char clean[64];
    if (!CleanName(name, clean, sizeof(clean)))
    {
      strcpy_s(g_FileMessage, "Give the pose a name first (letters, digits, spaces, - and _)");
      return;
    }
    CreateDirectoryA("Cinematic Tools", nullptr);
    CreateDirectoryA(PoseFolder, nullptr);
    char path[MAX_PATH];
    sprintf_s(path, "%s\\%s.txt", PoseFolder, clean);
    FILE* f = nullptr;
    if (fopen_s(&f, path, "w") || !f)
    {
      sprintf_s(g_FileMessage, "%s couldn't be written", path);
      return;
    }
    fprintf(f, "# MEC Cinematic Tools pose, %s, %s; each held bone: name, local rotation x y z w, translation x y z\n",
      h.Label, h.Asset);
    for (int n = 0; n < h.Count; ++n)
    {
      int i = (int)h.List[n];
      float local[FloatsPerBone];
      EditedLocal(h, i, local);
      fprintf(f, "%s %.7g %.7g %.7g %.7g %.7g %.7g %.7g\n", BoneName(h, i), local[4], local[5], local[6], local[7], local[0],
        local[1], local[2]);
    }
    fclose(f);
    sprintf_s(g_FileMessage, "Saved %s (%d bones)", path, h.Count);
    util::log::Write("Pose: %s saved from %s 0x%I64X (%d bones)", path, h.Label, h.Entity, h.Count);
    g_FilesListedAt = 0;
  }

  // Each saved bone with the same name in the held skeleton gets the saved
  // rotation; the held root its translation, and every bone its translation
  // when the pose was saved from the same skeleton (another skeleton's bones
  // are other lengths)
  void LoadPose(Held& h, char const* name)
  {
    char path[MAX_PATH];
    _snprintf_s(path, sizeof(path), _TRUNCATE, "%s\\%s.txt", PoseFolder, name);
    FILE* f = nullptr;
    if (fopen_s(&f, path, "r") || !f)
    {
      _snprintf_s(g_FileMessage, sizeof(g_FileMessage), _TRUNCATE, "%s couldn't be read", path);
      return;
    }
    Snapshot before = SnapshotOf(h);
    int set = 0, notHere = 0;
    bool sameSkeleton = false;
    char line[512];
    while (fgets(line, sizeof(line), f))
    {
      if (line[0] == '#')
      {
        // "# MEC Cinematic Tools pose, <who>, <its SkeletonAsset>; ..."
        char const* asset = h.Asset;
        char const* at = strstr(line, asset);
        size_t length = strlen(asset);
        sameSkeleton |= at && (at[length] == ';' || at[length] == ',' || at[length] == '\n');
        continue;
      }
      char bone[128];
      float q[4], t[3];
      if (sscanf_s(line, "%127s %f %f %f %f %f %f %f", bone, (unsigned)sizeof(bone), &q[0], &q[1], &q[2], &q[3], &t[0], &t[1],
          &t[2]) != 8)
        continue;
      int i = BoneByName(h.Skeleton, bone);
      if (i < 0 || !h.InHeld[i])
      {
        ++notHere;
        continue;
      }
      QNorm(q);
      // local = turn * base, so turn = saved * base^-1 (the base shown: a
      // clip's frame while one is on)
      float inverse[4];
      QConj(BaseOf(h, i) + 4, inverse);
      QMul(q, inverse, h.Turn[i]);
      QNorm(h.Turn[i]);
      if (i == h.Root || sameSkeleton)
      {
        for (int k = 0; k < 3; ++k)
          h.Move[i][k] = t[k] - BaseOf(h, i)[k];
        ClampMove(h, i);
      }
      else
        h.Move[i][0] = h.Move[i][1] = h.Move[i][2] = 0.f;
      ++set;
    }
    fclose(f);
    if (set > 0)
      PushUndo(h, before);
    _snprintf_s(g_FileMessage, sizeof(g_FileMessage), _TRUNCATE, "Loaded %s: %d bones set", name, set);
    if (notHere)
    {
      size_t used = strlen(g_FileMessage);
      _snprintf_s(g_FileMessage + used, sizeof(g_FileMessage) - used, _TRUNCATE,
        ", %d not in this skeleton's held bones", notHere);
    }
    util::log::Write("Pose: %s loaded onto %s 0x%I64X: %d bones set, %d not in its held bones", path, h.Label, h.Entity, set,
      notHere);
  }

  // ---- Saved keyframes: Cinematic Tools\Poses\<name>.posekeys, the file of PLAN.md 1.2 ----

  void ListKeyFiles()
  {
    g_KeyFiles.clear();
    WIN32_FIND_DATAW data;
    std::wstring pattern = Widen(PoseFolder) + L"\\*.posekeys";
    HANDLE find = FindFirstFileW(pattern.c_str(), &data);
    if (find != INVALID_HANDLE_VALUE)
    {
      do
      {
        if (!(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
        {
          std::wstring name = data.cFileName;
          if (name.size() > 9 && !_wcsicmp(name.c_str() + name.size() - 9, L".posekeys"))
            g_KeyFiles.push_back(name.substr(0, name.size() - 9));
        }
      } while (FindNextFileW(find, &data));
      FindClose(find);
    }
    std::sort(g_KeyFiles.begin(), g_KeyFiles.end());
  }

  bool SaveKeys(Held const& h, char const* name, bool quiet)
  {
    char clean[64];
    if (!CleanName(name, clean, sizeof(clean)))
    {
      if (quiet)
      {
        if (!g_AutosaveFailLogged)
        {
          g_AutosaveFailLogged = true;
          util::log::Warning("Pose: the keyframes' autosave couldn't be written: give it a name first (letters, digits, "
            "spaces, - and _)");
        }
      }
      else
        _snprintf_s(g_FileMessage, sizeof(g_FileMessage), _TRUNCATE,
          "The keyframes couldn't be saved: give it a name first (letters, digits, spaces, - and _)");
      return false;
    }
    CreateDirectoryA("Cinematic Tools", nullptr);
    CreateDirectoryA(PoseFolder, nullptr);
    char path[MAX_PATH];
    _snprintf_s(path, sizeof(path), _TRUNCATE, "%s\\%s.posekeys", PoseFolder, clean);
    pose_keys::File file;
    int keys = KeysFileOf(h, file);
    std::string error;
    if (!pose_keys::SaveFile(path, file, error))
    {
      if (quiet)
      {
        if (!g_AutosaveFailLogged)
        {
          g_AutosaveFailLogged = true;
          util::log::Warning("Pose: the keyframes' autosave couldn't be written: %s", error.c_str());
        }
      }
      else
        _snprintf_s(g_FileMessage, sizeof(g_FileMessage), _TRUNCATE, "The keyframes couldn't be saved: %s",
          error.c_str());
      return false;
    }
    if (!quiet)
    {
      _snprintf_s(g_FileMessage, sizeof(g_FileMessage), _TRUNCATE, "Saved %s (%d bones, %d keyframes)", path,
        (int)file.Bones.size(), keys);
      util::log::Write("Pose: %s saved from %s 0x%I64X (%d bones, %d keyframes)", path, h.Label, h.Entity,
        (int)file.Bones.size(), keys);
    }
    g_KeyFilesListedAt = 0;
    return true;
  }

  // The keyframes as a file holds them (and the bones posed without keyframes); returns how many keyframes
  int KeysFileOf(Held const& h, pose_keys::File& file)
  {
    file = pose_keys::File();
    file.Label = h.Label;
    file.Skeleton = h.Asset;
    file.Clip = h.Clip.Clip ? h.Clip.Clip->File : std::string();
    file.OverClip = h.Clip.On && h.Clip.LocalOk;
    file.Length = h.Keys.Length;
    file.Loop = h.Keys.Loop;
    file.Speed = h.Keys.Speed;
    file.Clock = h.Keys.Clock;
    file.Start = h.Keys.Start;
    int keys = 0;
    for (int n = 0; n < h.Count; ++n)
    {
      int bone = (int)h.List[n];
      pose_keys::Channel const* channel = pose_keys::Find(h.Keys, bone);
      bool edited = IsEdited(h, bone);
      if (channel == nullptr && !edited)
        continue;
      pose_keys::FileBone boneFile;
      boneFile.Name = BoneName(h, bone);
      if (channel != nullptr)
      {
        boneFile.Keys = channel->Keys;
        keys += (int)channel->Keys.size();
      }
      if (channel == nullptr && edited)
      {
        boneFile.HasStatic = true;
        memcpy(boneFile.StaticTurn, h.Turn[bone], sizeof(boneFile.StaticTurn));
        memcpy(boneFile.StaticMove, h.Move[bone], sizeof(boneFile.StaticMove));
      }
      if (!file.OverClip)
      {
        boneFile.HasBase = true;
        memcpy(boneFile.BaseQ, h.Base[bone] + 4, sizeof(boneFile.BaseQ));
        memcpy(boneFile.BaseT, h.Base[bone], sizeof(boneFile.BaseT));
      }
      file.Bones.push_back(boneFile);
    }
    return keys;
  }

  // ---- The keyframes in a file anywhere (2026-11-16, the user: "also make it possible to save the keyframes to a
  // file"): the keyframes bar's Save / Save as / Load, the Windows file window on a thread of its own ----

  std::wstring Widen(char const* text)
  {
    int n = MultiByteToWideChar(CP_ACP, 0, text, -1, nullptr, 0);
    std::wstring w(n > 1 ? (size_t)(n - 1) : 0, L'\0');
    if (n > 1)
      MultiByteToWideChar(CP_ACP, 0, text, -1, &w[0], n);
    return w;
  }

  // A keyframes file of Cinematic Tools\Poses as a whole path (name: its name without .posekeys)
  std::wstring PoseKeysPath(char const* name)
  {
    std::wstring relative = Widen(PoseFolder) + L"\\" + Widen(name) + L".posekeys";
    wchar_t full[1024];
    DWORD length = GetFullPathNameW(relative.c_str(), (DWORD)(sizeof(full) / sizeof(full[0])), full, nullptr);
    return length > 0 && length < sizeof(full) / sizeof(full[0]) ? std::wstring(full) : relative;
  }

  std::wstring PoseKeysPath(std::wstring const& name)
  {
    std::wstring relative = Widen(PoseFolder) + L"\\" + name + L".posekeys";
    wchar_t full[1024];
    DWORD length = GetFullPathNameW(relative.c_str(), (DWORD)(sizeof(full) / sizeof(full[0])), full, nullptr);
    return length > 0 && length < sizeof(full) / sizeof(full[0]) ? std::wstring(full) : relative;
  }

  // The file's name alone, for what's said (UTF-8)
  std::string FileNameOf(std::wstring const& path)
  {
    size_t slash = path.find_last_of(L"\\/");
    return Utf8Of(slash == std::wstring::npos ? path : path.substr(slash + 1));
  }

  // The keyframes saved in a file anywhere (a whole path); said in g_FileMessage
  bool SaveKeysToPath(Held const& h, std::wstring const& path)
  {
    pose_keys::File file;
    int keys = KeysFileOf(h, file);
    std::string error;
    std::string shown = Utf8Of(path);
    std::string logged = FileNameOf(path);   // (CT.log gets the file's name alone, not the user's folders)
    if (!pose_keys::SaveFile(path.c_str(), file, error))
    {
      _snprintf_s(g_FileMessage, sizeof(g_FileMessage), _TRUNCATE, "The keyframes couldn't be saved as %s: %s",
        shown.c_str(), error.c_str());
      util::log::Warning("Pose: keyframes couldn't be saved as %s: %s", logged.c_str(), error.c_str());
      return false;
    }
    _snprintf_s(g_FileMessage, sizeof(g_FileMessage), _TRUNCATE, "Saved %s (%d bones, %d keyframes)", shown.c_str(),
      (int)file.Bones.size(), keys);
    util::log::Write("Pose: %s saved from %s 0x%I64X (%d bones, %d keyframes)", logged.c_str(), h.Label, h.Entity,
      (int)file.Bones.size(), keys);
    g_KeyFilesListedAt = 0;
    return true;
  }

  bool LoadKeysFile(Held& h, pose_keys::File const& file, char const* name, char const* path);

  bool LoadKeys(Held& h, char const* name)
  {
    if (g_Drag.On || g_Move.On || g_Move.Pending)
    {
      _snprintf_s(g_FileMessage, sizeof(g_FileMessage), _TRUNCATE, "Let go of the bone before loading keyframes");
      return false;
    }
    if (name == nullptr || name[0] == '\0' || strchr(name, '\\') || strchr(name, '/') || strchr(name, ':') ||
      strstr(name, ".."))
    {
      _snprintf_s(g_FileMessage, sizeof(g_FileMessage), _TRUNCATE, "That keyframes file's name can't be used");
      return false;
    }
    char path[MAX_PATH];
    int length = _snprintf_s(path, sizeof(path), _TRUNCATE, "%s\\%s.posekeys", PoseFolder, name);
    if (length < 0 || length >= MAX_PATH)
    {
      _snprintf_s(g_FileMessage, sizeof(g_FileMessage), _TRUNCATE, "That keyframes file's name can't be used");
      return false;
    }
    pose_keys::File file;
    std::string error;
    if (!pose_keys::LoadFile(path, file, error))
    {
      _snprintf_s(g_FileMessage, sizeof(g_FileMessage), _TRUNCATE, "%s couldn't be loaded: %s", name,
        error.c_str());
      return false;
    }
    return LoadKeysFile(h, file, name, path);
  }

  // Keyframes loaded from a file anywhere (a whole path); said in g_FileMessage
  bool LoadKeysFromPath(Held& h, std::wstring const& path)
  {
    std::string name = FileNameOf(path), shown = Utf8Of(path);
    if (g_Drag.On || g_Move.On || g_Move.Pending)
    {
      _snprintf_s(g_FileMessage, sizeof(g_FileMessage), _TRUNCATE, "Let go of the bone before loading keyframes");
      return false;
    }
    pose_keys::File file;
    std::string error;
    if (!pose_keys::LoadFile(path.c_str(), file, error))
    {
      _snprintf_s(g_FileMessage, sizeof(g_FileMessage), _TRUNCATE, "%s couldn't be loaded: %s", shown.c_str(),
        error.c_str());
      return false;
    }
    return LoadKeysFile(h, file, name.c_str(), name.c_str());   // (CT.log gets the file's name alone)
  }

  // A file's keyframes put on the pose (one Undo step; name: said, path: logged)
  bool LoadKeysFile(Held& h, pose_keys::File const& file, char const* name, char const* path)
  {
    ++h.KeysLoads;
    Snapshot before = SnapshotOf(h);
    PushUndo(h, before);
    h.History.back().Settings = true;   // the load replaces the keyframes' settings too: Undo puts them back
    pose_keys::Anim keys;
    keys.Revision = h.Keys.Revision + 1;
    int loadedBones = 0, loadedKeys = 0, notHere = 0, refused = 0;
    for (size_t b = 0; b < file.Bones.size(); ++b)
    {
      pose_keys::FileBone const& boneFile = file.Bones[b];
      int bone = BoneByName(h.Skeleton, boneFile.Name.c_str());
      if (bone < 0 || !h.InHeld[bone])
      {
        ++notHere;
        continue;
      }
      ++loadedBones;
      bool rebase = boneFile.HasBase && !file.OverClip && !(h.Clip.On && h.Clip.LocalOk);
      bool keepMove = bone == h.Root || file.Skeleton == h.Asset;
      auto place = [&](float const* sourceTurn, float const* sourceMove, float* turn, float* move)
      {
        memcpy(turn, sourceTurn, 4 * sizeof(float));
        memcpy(move, sourceMove, 3 * sizeof(float));
        if (rebase)
          pose_keys::Rebase(boneFile.BaseQ, boneFile.BaseT, h.Base[bone] + 4, h.Base[bone], keepMove, turn, move);
        else if (!keepMove)
          move[0] = move[1] = move[2] = 0.f;
        float limit = MoveLimit(h, bone);
        for (int k = 0; k < 3; ++k)
        {
          if (!std::isfinite(move[k]))
            move[k] = 0.f;
          move[k] = move[k] < -limit ? -limit : (move[k] > limit ? limit : move[k]);
        }
      };
      for (size_t k = 0; k < boneFile.Keys.size(); ++k)
      {
        pose_keys::Key const& key = boneFile.Keys[k];
        float turn[4], move[3];
        place(key.Turn, key.Move, turn, move);
        if (pose_keys::SetKey(keys, bone, key.Time, turn, move, key.Ease) >= 0)
          ++loadedKeys;
        else
          ++refused;
      }
      if (boneFile.HasStatic)
      {
        float turn[4], move[3];
        place(boneFile.StaticTurn, boneFile.StaticMove, turn, move);
        memcpy(h.Turn[bone], turn, sizeof(turn));
        memcpy(h.Move[bone], move, sizeof(move));
      }
    }
    keys.Loop = file.Loop;
    float speed = file.Speed;
    if (speed < 0.05f)
      speed = 0.05f;
    if (speed > 4.f)
      speed = 4.f;
    keys.Speed = speed;
    keys.Clock = file.Clock < 0 ? 0 : (file.Clock >= pose_keys::Clock_Count ? pose_keys::Clock_Count - 1 : file.Clock);
    double start = file.Start;
    if (start < -3600.0)
      start = -3600.0;
    if (start > 3600.0)
      start = 3600.0;
    keys.Start = start;
    pose_keys::SetLength(keys, file.Length);
    keys.Time = 0.0;
    keys.Playing = false;
    keys.On = true;
    h.Keys = keys;
    h.KeysAppliedOk = false;
    for (int bone = 0; bone < h.Bones; ++bone)
    {
      memcpy(h.KeyRefTurn[bone], h.Turn[bone], 4 * sizeof(float));
      memcpy(h.KeyRefMove[bone], h.Move[bone], 3 * sizeof(float));
    }
    ForgetStash(h);   // a load is a step of its own, kept (SettleLastStep doesn't judge it)
    _snprintf_s(g_FileMessage, sizeof(g_FileMessage), _TRUNCATE, "Loaded %s: %d keyframes on %d bones", name,
      loadedKeys, loadedBones);
    if (notHere)
    {
      size_t used = strlen(g_FileMessage);
      _snprintf_s(g_FileMessage + used, sizeof(g_FileMessage) - used, _TRUNCATE,
        ", %d bones not in this skeleton's held bones", notHere);
    }
    if (refused)
    {
      size_t used = strlen(g_FileMessage);
      _snprintf_s(g_FileMessage + used, sizeof(g_FileMessage) - used, _TRUNCATE, ", %d keyframes refused", refused);
    }
    std::string currentClip = h.Clip.Clip ? h.Clip.Clip->File : std::string();
    bool overOtherClip = file.OverClip && (!(h.Clip.On && h.Clip.LocalOk) || file.Clip != currentClip);
    if (overOtherClip)
    {
      size_t used = strlen(g_FileMessage);
      _snprintf_s(g_FileMessage + used, sizeof(g_FileMessage) - used, _TRUNCATE,
        ", its edits were made over \"%s\"", file.Clip.c_str());
    }
    util::log::Write("Pose: %s loaded onto %s 0x%I64X: %d keyframes on %d bones, %d not in its held bones, "
      "%d keyframes refused%s", path, h.Label, h.Entity, loadedKeys, loadedBones, notHere, refused,
      overOtherClip ? " (made over another clip)" : "");
    return true;
  }

  // The keyframes in a file anywhere, off the menu's thread: the Windows file window (Save as, Load) and the file's
  // writing or reading run on a thread of their own (KeyFileWork), so neither the window nor a slow folder (a network
  // drive asleep, a cloud file being fetched) holds up the game (the review, 2026-11-16: the I/O ran under g_Mutex on
  // the menu's thread); the answer is taken on the menu's thread (TakeKeyFileAnswer). One at a time.
  enum KeyFileKind { KeyFile_SaveAs, KeyFile_Save, KeyFile_Load };
  struct KeyFileAsk
  {
    std::mutex Mutex;
    bool Open = false;          // asked, its answer not taken yet
    bool Done = false;          // answered: Path ("": the window closed with no file), Error ("": none)
    int Kind = KeyFile_SaveAs;
    int HeldId = 0;             // for this pose (h.Id)
    std::wstring Path;
    std::string Error;
    pose_keys::File Loaded;     // KeyFile_Load: the file as read
    int Bones = 0, Keys = 0;    // a save: what it holds
    unsigned LoadsAtAsk = 0;    // the pose's KeysLoads when asked
    ULONGLONG QuietUntil = 0;   // the file window closed with a mouse button down: its press isn't taken until then
  };
  KeyFileAsk g_KeyFileAsk;
  // The offline harness only: in place of the file window, the path it would give ("": closed with no file)
  std::wstring (*g_KeyFileWindowStub)(bool save, std::wstring const& folder, std::wstring const& name) = nullptr;
  // The offline harness only: told where the keyframes bar's file buttons and the Load menu's items are
  void (*g_KeyFileItemStub)(char const* what, ImVec2 min, ImVec2 max) = nullptr;
  // ... and where the Voice tab's emotion items are (the last item drawn: its rect; nothing in the game)
  void ReportItemForTests(char const* what)
  {
    if (g_KeyFileItemStub)
      g_KeyFileItemStub(what, ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
  }
  bool g_MouseQuiet = false;    // a press that came as the file window closed: not taken until the buttons are up

  // What's under way (-1: nothing), for the buttons' gates and tips
  int KeyFileBusy()
  {
    std::lock_guard<std::mutex> lock(g_KeyFileAsk.Mutex);
    return g_KeyFileAsk.Open ? g_KeyFileAsk.Kind : -1;
  }

  bool KeyFileWindowOpen()
  {
    return KeyFileBusy() >= 0;
  }

  bool MouseButtonDown()
  {
    return (GetAsyncKeyState(VK_LBUTTON) & 0x8000) || (GetAsyncKeyState(VK_RBUTTON) & 0x8000) ||
      (GetAsyncKeyState(VK_MBUTTON) & 0x8000);
  }

  // The file window's events: OnFileOk comes as a file is picked, before the window closes (the final review,
  // 2026-11-16: checked after Show returned, the press the game sees as it comes back to the front was already taken).
  // A mouse button down then (a double-click on a file: the window closes on its second press) quiets that press. The
  // rest left to the window (E_NOTIMPL: its own way).
  class KeyFileEvents : public IFileDialogEvents
  {
    LONG m_Refs = 1;
  public:
    IFACEMETHODIMP QueryInterface(REFIID riid, void** out) override
    {
      if (!out)
        return E_POINTER;
      if (riid == __uuidof(IUnknown) || riid == __uuidof(IFileDialogEvents))
      {
        *out = static_cast<IFileDialogEvents*>(this);
        AddRef();
        return S_OK;
      }
      *out = nullptr;
      return E_NOINTERFACE;
    }
    IFACEMETHODIMP_(ULONG) AddRef() override
    {
      return (ULONG)InterlockedIncrement(&m_Refs);
    }
    IFACEMETHODIMP_(ULONG) Release() override
    {
      LONG n = InterlockedDecrement(&m_Refs);
      if (n == 0)
        delete this;
      return (ULONG)n;
    }
    IFACEMETHODIMP OnFileOk(IFileDialog*) override
    {
      if (MouseButtonDown())
      {
        std::lock_guard<std::mutex> lock(g_KeyFileAsk.Mutex);
        g_KeyFileAsk.QuietUntil = GetTickCount64() + 1000;
      }
      return S_OK;
    }
    IFACEMETHODIMP OnFolderChanging(IFileDialog*, IShellItem*) override { return E_NOTIMPL; }
    IFACEMETHODIMP OnFolderChange(IFileDialog*) override { return E_NOTIMPL; }
    IFACEMETHODIMP OnSelectionChange(IFileDialog*) override { return E_NOTIMPL; }
    IFACEMETHODIMP OnShareViolation(IFileDialog*, IShellItem*, FDE_SHAREVIOLATION_RESPONSE*) override { return E_NOTIMPL; }
    IFACEMETHODIMP OnTypeChange(IFileDialog*) override { return E_NOTIMPL; }
    IFACEMETHODIMP OnOverwrite(IFileDialog*, IShellItem*, FDE_OVERWRITE_RESPONSE*) override { return E_NOTIMPL; }
  };

  // The window itself (KeyFileWork's thread, COM's single-threaded apartment there): a .posekeys file to save as (save)
  // or to load, starting in folder with name; "" when closed with no file (error: why, when it failed). No owner
  // window: the game's window belongs to another thread, and an owner there would tie the two threads' input together;
  // so it can open behind the game (Alt+Tab). FOS_NOCHANGEDIR: the tools' own files are found from the game's folder
  // as the current one.
  std::wstring ShowKeyFileWindow(bool save, std::wstring const& folder, std::wstring const& name, std::string& error)
  {
    std::wstring chosen;
    HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    IFileDialog* dialog = nullptr;
    HRESULT made = save ? CoCreateInstance(__uuidof(FileSaveDialog), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)) :
      CoCreateInstance(__uuidof(FileOpenDialog), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog));
    if (SUCCEEDED(made) && dialog)
    {
      COMDLG_FILTERSPEC types[] = { { L"Pose keyframes (*.posekeys)", L"*.posekeys" }, { L"All files (*.*)", L"*.*" } };
      dialog->SetFileTypes(2, types);
      dialog->SetFileTypeIndex(1);
      dialog->SetDefaultExtension(L"posekeys");
      FILEOPENDIALOGOPTIONS options = 0;
      if (SUCCEEDED(dialog->GetOptions(&options)))
        dialog->SetOptions(options | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | FOS_NOCHANGEDIR |
          (save ? FOS_OVERWRITEPROMPT : FOS_FILEMUSTEXIST));
      dialog->SetTitle(save ? L"Save the keyframes (Cinematic Tools)" : L"Load keyframes (Cinematic Tools)");
      IShellItem* start = nullptr;
      if (!folder.empty() && SUCCEEDED(SHCreateItemFromParsingName(folder.c_str(), nullptr, IID_PPV_ARGS(&start))) &&
          start)
      {
        dialog->SetFolder(start);
        start->Release();
      }
      if (!name.empty())
        dialog->SetFileName(name.c_str());
      KeyFileEvents* events = new KeyFileEvents();
      DWORD cookie = 0;
      bool advised = SUCCEEDED(dialog->Advise(events, &cookie));
      HRESULT shown = dialog->Show(nullptr);
      if (advised)
        dialog->Unadvise(cookie);
      events->Release();
      if (SUCCEEDED(shown))
      {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dialog->GetResult(&item)) && item)
        {
          PWSTR path = nullptr;
          if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path)
          {
            chosen = path;
            CoTaskMemFree(path);
          }
          item->Release();
        }
        if (chosen.empty())
          error = "the file window gave no file on the disk";
      }
      else if (shown != HRESULT_FROM_WIN32(ERROR_CANCELLED))
      {
        char text[96];
        _snprintf_s(text, sizeof(text), _TRUNCATE, "the file window failed (0x%08X)", (unsigned)shown);
        error = text;
      }
      dialog->Release();
    }
    else
    {
      char text[96];
      _snprintf_s(text, sizeof(text), _TRUNCATE, "the file window couldn't be made (0x%08X)", (unsigned)made);
      error = text;
    }
    if (SUCCEEDED(init))
      CoUninitialize();
    return chosen;
  }

  struct KeyFileJob
  {
    int Kind;
    std::wstring Folder, Name;  // the window's start (Save as, Load)
    std::wstring Path;          // KeyFile_Save: the file
    std::shared_ptr<pose_keys::File> Data;   // a save: the keyframes as they were when asked
  };

  // The thread: the window (Save as, Load), then the file written or read; the answer left in g_KeyFileAsk
  void KeyFileWork(KeyFileJob job)
  {
    std::wstring path = job.Path;
    std::string error;
    if (job.Kind != KeyFile_Save)
    {
      bool save = job.Kind == KeyFile_SaveAs;
      if (g_KeyFileWindowStub)
        path = g_KeyFileWindowStub(save, job.Folder, job.Name);
      else
      {
        path = ShowKeyFileWindow(save, job.Folder, job.Name, error);
        // (the buttons up by now: the game saw no press, or its press was quieted already; none to quiet later)
        if (!MouseButtonDown())
        {
          std::lock_guard<std::mutex> lock(g_KeyFileAsk.Mutex);
          g_KeyFileAsk.QuietUntil = 0;
        }
      }
    }
    pose_keys::File loaded;
    if (!path.empty() && error.empty())
    {
      if (job.Kind == KeyFile_Load)
        pose_keys::LoadFile(path.c_str(), loaded, error);
      else if (job.Data)
        pose_keys::SaveFile(path.c_str(), *job.Data, error);
      else
        error = "nothing to save";
    }
    std::lock_guard<std::mutex> lock(g_KeyFileAsk.Mutex);
    g_KeyFileAsk.Done = true;
    g_KeyFileAsk.Path = path;
    g_KeyFileAsk.Error = error;
    if (job.Kind == KeyFile_Load && error.empty())
      g_KeyFileAsk.Loaded = std::move(loaded);
  }

  void SayKeyNote(Held const& h, char const* text)
  {
    _snprintf_s(g_KeyNote, sizeof(g_KeyNote), _TRUNCATE, "%s", text);
    g_KeyNoteAt = GetTickCount64();
    g_KeyNoteHeld = h.Id;
  }

  // Save as (the window), Save (into h.KeysFile) or Load (the window) for this pose, on KeyFileWork's thread. The window
  // starts at the pose's keyframes' file, else the file it last loaded, else Cinematic Tools\Poses with the Files tab's
  // keyframes name (else the pose's). A save writes the keyframes as they are now.
  void AskKeyFile(Held const& h, int kind)
  {
    KeyFileJob job;
    job.Kind = kind;
    std::wstring const& known = !h.KeysFile.empty() ? h.KeysFile : h.KeysSuggest;
    size_t slash = known.find_last_of(L"\\/");
    if (slash != std::wstring::npos)
    {
      job.Folder = known.substr(0, slash);
      job.Name = known.substr(slash + 1);
    }
    if (job.Folder.empty())
    {
      CreateDirectoryA("Cinematic Tools", nullptr);
      CreateDirectoryA(PoseFolder, nullptr);
      std::wstring path = PoseKeysPath("x");
      size_t end = path.find_last_of(L"\\/");
      if (end != std::wstring::npos)
        job.Folder = path.substr(0, end);
    }
    if (job.Name.empty() && kind == KeyFile_SaveAs)
    {
      char clean[64];
      if (CleanName(g_KeySaveName, clean, sizeof(clean)) || CleanName(h.Label, clean, sizeof(clean)))
        job.Name = Widen(clean) + L".posekeys";
    }
    if (kind == KeyFile_Load)
      job.Name.clear();   // (the window's name field empty: a file to pick)
    int bones = 0, keys = 0;
    if (kind != KeyFile_Load)
    {
      job.Data = std::make_shared<pose_keys::File>();
      keys = KeysFileOf(h, *job.Data);
      bones = (int)job.Data->Bones.size();
    }
    if (kind == KeyFile_Save)
    {
      job.Path = h.KeysFile;
      if (job.Path.empty())
        return;
    }
    {
      std::lock_guard<std::mutex> lock(g_KeyFileAsk.Mutex);
      if (g_KeyFileAsk.Open)
        return;
      g_KeyFileAsk.Open = true;
      g_KeyFileAsk.Done = false;
      g_KeyFileAsk.Kind = kind;
      g_KeyFileAsk.HeldId = h.Id;
      g_KeyFileAsk.Path.clear();
      g_KeyFileAsk.Error.clear();
      g_KeyFileAsk.Loaded = pose_keys::File();
      g_KeyFileAsk.Bones = bones;
      g_KeyFileAsk.Keys = keys;
      g_KeyFileAsk.LoadsAtAsk = h.KeysLoads;
    }
    try
    {
      std::thread(KeyFileWork, std::move(job)).detach();
    }
    catch (...)
    {
      std::lock_guard<std::mutex> lock(g_KeyFileAsk.Mutex);
      g_KeyFileAsk.Open = false;
      SayKeyNote(h, "The keyframes' file couldn't be done (no thread for it)");
      return;
    }
    if (kind == KeyFile_Save)
    {
      std::string shown = "Saving the keyframes into " + Utf8Of(h.KeysFile) + "...";
      SayKeyNote(h, shown.c_str());
    }
    else
      SayKeyNote(h, kind == KeyFile_SaveAs ? "The file window is open: pick where to save the keyframes (if it isn't in "
        "front, it's behind the game: Alt+Tab to it)" : "The file window is open: pick the keyframes to load (if it isn't "
        "in front, it's behind the game: Alt+Tab to it)");
  }

  // The answer, once it's in (the menu's thread, each frame; a load waits while a bone is held): a save's file becomes
  // the pose's keyframes file (the bar's Save writes there next); a load's keyframes put on the pose (one Undo step),
  // and its file only offered to Save as next (the review, 2026-11-16: a loaded file as Save's file was written over
  // after an Undo of the load, a lossy load onto another skeleton, or an autosave loaded). Nothing for a pose let go.
  void TakeKeyFileAnswer()
  {
    int kind = 0, heldId = 0, bones = 0, keys = 0;
    unsigned loadsAtAsk = 0;
    std::wstring path;
    std::string error;
    pose_keys::File loaded;
    {
      std::lock_guard<std::mutex> lock(g_KeyFileAsk.Mutex);
      if (!g_KeyFileAsk.Open || !g_KeyFileAsk.Done)
        return;
      // (a load waits while anything is held: a bone, a ring, a keyframe on the timeline, a field; the final review)
      if (g_KeyFileAsk.Kind == KeyFile_Load && (g_Drag.On || g_Move.On || g_Move.Pending || g_KeyDrag.On ||
          g_KeyDrag.Pending || g_KeyScrub || ImGui::IsAnyItemActive()))
        return;
      kind = g_KeyFileAsk.Kind;
      heldId = g_KeyFileAsk.HeldId;
      path = g_KeyFileAsk.Path;
      error = g_KeyFileAsk.Error;
      bones = g_KeyFileAsk.Bones;
      keys = g_KeyFileAsk.Keys;
      loadsAtAsk = g_KeyFileAsk.LoadsAtAsk;
      loaded = std::move(g_KeyFileAsk.Loaded);
      g_KeyFileAsk.Loaded = pose_keys::File();
      g_KeyFileAsk.Open = g_KeyFileAsk.Done = false;
    }
    Held* found = nullptr;
    for (int i = 0; i < MaxHeld && !found; ++i)
      if (g_Held[i].Used && !g_Held[i].Restoring && g_Held[i].Id == heldId)
        found = &g_Held[i];
    bool save = kind != KeyFile_Load;
    std::string shown = Utf8Of(path);
    char note[sizeof(g_KeyNote)];
    if (!error.empty() && path.empty())
      _snprintf_s(note, sizeof(note), _TRUNCATE, "The keyframes weren't %s: %s", save ? "saved" : "loaded", error.c_str());
    else if (!error.empty())
    {
      _snprintf_s(note, sizeof(note), _TRUNCATE, save ? "The keyframes couldn't be saved as %s: %s" :
        "%s couldn't be loaded: %s", shown.c_str(), error.c_str());
      // (CT.log gets the file's name alone, not the user's folders; the menu's note keeps the whole path)
      util::log::Warning(save ? "Pose: The keyframes couldn't be saved as %s: %s" : "Pose: %s couldn't be loaded: %s",
        FileNameOf(path).c_str(), error.c_str());
    }
    else if (path.empty())
      _snprintf_s(note, sizeof(note), _TRUNCATE, "The file window was closed: nothing %s", save ? "saved" : "loaded");
    else if (save)
    {
      _snprintf_s(note, sizeof(note), _TRUNCATE, "Saved %s (%d bones, %d keyframes)%s", shown.c_str(), bones, keys,
        found ? "" : "; the pose was let go meanwhile");
      util::log::Write("Pose: %s saved (%d bones, %d keyframes)", FileNameOf(path).c_str(), bones, keys);
      g_KeyFilesListedAt = 0;
      // (Save's file only with no load since it was asked: else Save would write the loaded keyframes into it
      // unasked; the final review)
      if (found && found->KeysLoads == loadsAtAsk)
        found->KeysFile = path;
      if (found)
        found->KeysSuggest = path;
    }
    else if (!found)
      _snprintf_s(note, sizeof(note), _TRUNCATE, "The pose was let go while the file window was open: nothing loaded");
    else
    {
      std::string name = FileNameOf(path);
      LoadKeysFile(*found, loaded, name.c_str(), name.c_str());   // (CT.log gets the file's name alone)
      found->KeysFile.clear();
      found->KeysSuggest = path;
      _snprintf_s(note, sizeof(note), _TRUNCATE, "%s", g_FileMessage);
    }
    _snprintf_s(g_FileMessage, sizeof(g_FileMessage), _TRUNCATE, "%s", note);
    _snprintf_s(g_KeyNote, sizeof(g_KeyNote), _TRUNCATE, "%s", note);
    g_KeyNoteAt = GetTickCount64();
    g_KeyNoteHeld = heldId;
  }

  // A press as the file window closed (a double-click on a file in it closes it on the second press: the game comes back
  // to the front with the button down; the reviews, 2026-11-16): the first press seen within 1 s of the file picked with
  // a button down (KeyFileEvents::OnFileOk) isn't taken until the buttons are up: by nothing on the menu's frame
  // (pose_editor::QuietMouse, from UI.cpp before the overlays; DrawOverlay calls it too, for the harness). A file picked
  // with no button down (the window's Save or Open button: on the release; Enter) quiets nothing.
  void QuietMouseAfterWindow()
  {
    ImGuiIO& io = ImGui::GetIO();
    bool pressed = io.MouseClicked[0] || io.MouseClicked[1] || io.MouseClicked[2];
    if (pressed)
    {
      std::lock_guard<std::mutex> lock(g_KeyFileAsk.Mutex);
      if (g_KeyFileAsk.QuietUntil != 0 && GetTickCount64() < g_KeyFileAsk.QuietUntil)
        g_MouseQuiet = true;
      g_KeyFileAsk.QuietUntil = 0;
    }
    if (!g_MouseQuiet)
      return;
    if (!io.MouseDown[0] && !io.MouseDown[1] && !io.MouseDown[2])
    {
      g_MouseQuiet = false;
      return;
    }
    for (int k = 0; k < 3; ++k)
    {
      io.MouseClicked[k] = false;
      io.MouseDoubleClicked[k] = false;
    }
  }

  // ---- The turning rings ----

  const int RingSegments = 48;
  const float RingPixels = 4.5f;       // font sizes: a ring's radius on the screen
  const float RingReach = 0.4f;        // font sizes: how near the cursor has to be

  struct Ring
  {
    float Axis[3], U[3], V[3];
    ImU32 Colour;
    ImVec2 Points[RingSegments + 1];
    bool Shown[RingSegments + 1];
  };

  // Rings about X, Y and Z and the view's axis, round a bone: the bone's own
  // axes (its world rotation, boneRotation), or the world's (null)
  int MakeRings(View const& view, float const* centre, float const* boneRotation, Ring* rings, float& radius)
  {
    float const* t = view.T;
    float offset[3] = { centre[0] - t[12], centre[1] - t[13], centre[2] - t[14] };
    float depth = -(offset[0] * t[8] + offset[1] * t[9] + offset[2] * t[10]);
    ImVec2 size = ImGui::GetIO().DisplaySize;
    if (depth < 0.2f || view.P[0] == 0.f || size.x <= 0.f)
      return 0;
    radius = depth * (RingPixels * ImGui::GetFontSize()) / (fabsf(view.P[0]) * size.x * 0.5f);
    float axes[3][3] = { { 1.f, 0.f, 0.f }, { 0.f, 1.f, 0.f }, { 0.f, 0.f, 1.f } };
    if (boneRotation)
    {
      float q[4];
      memcpy(q, boneRotation, sizeof(q));
      QNorm(q);
      for (int a = 0; a < 3; ++a)
      {
        float unit[3] = { a == 0 ? 1.f : 0.f, a == 1 ? 1.f : 0.f, a == 2 ? 1.f : 0.f };
        QRot(q, unit, axes[a]);
        Normalize3(axes[a]);
      }
    }
    const ImU32 colours[4] = { IM_COL32(235, 80, 80, 230), IM_COL32(90, 210, 90, 230), IM_COL32(90, 140, 255, 230),
      IM_COL32(255, 255, 255, 200) };
    for (int r = 0; r < 4; ++r)
    {
      Ring& ring = rings[r];
      if (r < 3)
      {
        // u, v with u x v = the axis
        int a = (r + 1) % 3, b = (r + 2) % 3;
        for (int k = 0; k < 3; ++k)
        {
          ring.U[k] = axes[a][k];
          ring.V[k] = axes[b][k];
        }
      }
      else
      {
        // The view's own: the camera's right and up (the ring a bit bigger)
        for (int k = 0; k < 3; ++k)
        {
          ring.U[k] = t[k] * 1.15f;
          ring.V[k] = t[4 + k] * 1.15f;
        }
      }
      Cross(ring.U, ring.V, ring.Axis);
      Normalize3(ring.Axis);
      ring.Colour = colours[r];
      for (int s = 0; s <= RingSegments; ++s)
      {
        float a = 6.2831853f * s / RingSegments;
        float p[3];
        for (int k = 0; k < 3; ++k)
          p[k] = centre[k] + radius * (cosf(a) * ring.U[k] + sinf(a) * ring.V[k]);
        float x = 0.f, y = 0.f;
        ring.Shown[s] = ToScreen(view, p, x, y);
        ring.Points[s] = ImVec2(x, y);
      }
    }
    return 4;
  }

  // How near a point is to a segment, in pixels
  float SegmentDistance(ImVec2 p, ImVec2 a, ImVec2 b, float& along)
  {
    float dx = b.x - a.x, dy = b.y - a.y;
    float lengthSq = dx * dx + dy * dy;
    along = lengthSq > 1e-6f ? ((p.x - a.x) * dx + (p.y - a.y) * dy) / lengthSq : 0.f;
    along = along < 0.f ? 0.f : (along > 1.f ? 1.f : along);
    float cx = a.x + dx * along - p.x, cy = a.y + dy * along - p.y;
    return sqrtf(cx * cx + cy * cy);
  }

  // ---- Moving a bone with the mouse ----

  // The world pose with the held bones from the pose as it is now (the rest
  // from this frame's chain, h.World)
  void ComposeHeld(Held const& h, float (*out)[FloatsPerBone])
  {
    memcpy(out, h.World, (size_t)h.Bones * FloatsPerBone * sizeof(float));
    for (int n = 0; n < h.Count; ++n)
    {
      int i = (int)h.List[n], p = h.Parents[i];
      if (p < 0)
        continue;
      float local[FloatsPerBone];
      EditedLocal(h, i, local);
      Compose(out[p], local, out[i]);
    }
  }

  // The bone turned by q in the world (about its own place): its turn becomes
  // parent^-1 * q * parent * its turn (world is the pose's world now)
  void ApplyWorldTurn(Held& h, int bone, float const* q, float (*world)[FloatsPerBone])
  {
    int p = h.Parents[bone];
    float parent[4] = { 0.f, 0.f, 0.f, 1.f };
    if (p >= 0)
      memcpy(parent, world[p] + 4, sizeof(parent));
    QNorm(parent);
    float inverse[4], a[4], b[4], turn[4];
    QConj(parent, inverse);
    QMul(inverse, q, a);
    QMul(a, parent, b);
    QMul(b, h.Turn[bone], turn);
    QNorm(turn);
    memcpy(h.Turn[bone], turn, sizeof(turn));
  }

  // The shortest turn from one unit vector to another
  void FromTo(float const* u, float const* v, float* q)
  {
    float d = u[0] * v[0] + u[1] * v[1] + u[2] * v[2];
    if (d < -0.99999f)
    {
      float other[3] = { fabsf(u[0]) > 0.9f ? 0.f : 1.f, fabsf(u[0]) > 0.9f ? 1.f : 0.f, 0.f };
      float axis[3];
      Cross(u, other, axis);
      Normalize3(axis);
      QAxis(axis, 3.14159265f, q);
      return;
    }
    float c[3];
    Cross(u, v, c);
    q[0] = c[0]; q[1] = c[1]; q[2] = c[2]; q[3] = 1.f + d;
    QNorm(q);
  }

  float Length3(float const* v)
  {
    return sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
  }

  // A unit vector at right angles to v
  void AnyPerpendicular(float const* v, float* out)
  {
    float other[3] = { fabsf(v[0]) > 0.9f ? 0.f : 1.f, fabsf(v[0]) > 0.9f ? 1.f : 0.f, 0.f };
    Cross(v, other, out);
    Normalize3(out);
  }

  // Where the point under the screen spot x, y is, on the plane across the
  // picture through base (the camera's right and up): Newton steps on
  // ToScreen, as the lights' dots are dragged (LightManager.cpp DragPlace),
  // so it's where the dot is drawn (the Panini view too). False behind the
  // camera.
  bool PlanePoint(View const& view, float const* base, float x, float y, float* out)
  {
    float const* t = view.T;
    float depth = -((base[0] - t[12]) * t[8] + (base[1] - t[13]) * t[9] + (base[2] - t[14]) * t[10]);
    if (depth < 0.1f)
      return false;
    float axisA[3] = { t[0], t[1], t[2] }, axisB[3] = { t[4], t[5], t[6] };
    Normalize3(axisA);
    Normalize3(axisB);
    float step = depth * 0.001f;
    float a = 0.f, b = 0.f, sx, sy;
    if (!ToScreen(view, base, sx, sy))
      return false;
    float error = (x - sx) * (x - sx) + (y - sy) * (y - sy);
    for (int i = 0; i < 16 && error > 0.0025f; ++i)
    {
      float v[3], pa[3], pb[3];
      for (int k = 0; k < 3; ++k)
      {
        v[k] = base[k] + axisA[k] * a + axisB[k] * b;
        pa[k] = v[k] + axisA[k] * step;
        pb[k] = v[k] + axisB[k] * step;
      }
      float ax, ay, bx, by;
      if (!ToScreen(view, pa, ax, ay) || !ToScreen(view, pb, bx, by))
        break;
      float j11 = (ax - sx) / step, j21 = (ay - sy) / step, j12 = (bx - sx) / step, j22 = (by - sy) / step;
      float det = j11 * j22 - j12 * j21;
      if (fabsf(det) < 1e-8f)
        break;
      float ex = x - sx, ey = y - sy;
      float da = (j22 * ex - j12 * ey) / det, db = (j11 * ey - j21 * ex) / det;
      float length = sqrtf(da * da + db * db);
      if (length > depth)
      {
        da *= depth / length;
        db *= depth / length;
      }
      bool nearer = false;
      for (int halve = 0; halve < 6 && !nearer; ++halve, da *= 0.5f, db *= 0.5f)
      {
        float n[3];
        for (int k = 0; k < 3; ++k)
          n[k] = base[k] + axisA[k] * (a + da) + axisB[k] * (b + db);
        float nx, ny;
        if (!ToScreen(view, n, nx, ny))
          continue;
        float e = (x - nx) * (x - nx) + (y - ny) * (y - ny);
        if (e < error)
        {
          a += da;
          b += db;
          sx = nx;
          sy = ny;
          error = e;
          nearer = true;
        }
      }
      if (!nearer)
        break;
    }
    for (int k = 0; k < 3; ++k)
      out[k] = base[k] + axisA[k] * a + axisB[k] * b;
    return true;
  }

  // The kinematics (2026-11-16): the hinged limbs, holds, curves (PoseKinematics.inl) and the layers over the pose
  // handed each frame (PoseMotion.inl)
#include "PoseKinematics.inl"
#include "PoseMotion.inl"

  // Two bones bent so the third's place is the target (as near as they
  // reach): an upper bone (a), its child (b) and its child (c), e.g. a
  // collarbone and an upper arm for an elbow, a shin and a foot for a toe.
  // The joint keeps bending in the plane it's bent in (its normal carried:
  // until 2026-11-16 the side was, and it flipped when the drag direction
  // passed it); no hinge, no limits (a limb's own: SolveLimb).
  void TwoBone(Held& h, int a, int b, int c, float const* target, float (*w)[FloatsPerBone])
  {
    ComposeHeld(h, w);
    LimbSolve s;
    SolveTwo(w[a], w[b], w[c], target, nullptr, false, false, s);
    ApplyWorldTurn(h, a, s.TurnA, w);
    ComposeHeld(h, w);
    ApplyWorldTurn(h, b, s.TurnB, w);
  }

  // The bone moved by a world offset (its move is in its parent's frame; w is
  // the pose's world now)
  void TranslateBone(Held& h, int bone, float const* delta, float (*w)[FloatsPerBone])
  {
    int p = h.Parents[bone];
    float inverse[4] = { 0.f, 0.f, 0.f, 1.f }, local[3];
    if (p >= 0)
    {
      float parent[4];
      memcpy(parent, w[p] + 4, sizeof(parent));
      QNorm(parent);
      QConj(parent, inverse);
    }
    QRot(inverse, delta, local);
    for (int k = 0; k < 3; ++k)
      h.Move[bone][k] += local[k];
    ClampMove(h, bone);
  }

  // A face group moved by a world offset: each member the same way
  void MoveGroup(Held& h, int group, float const* delta)
  {
    static float w[MaxBones][FloatsPerBone];
    ComposeHeld(h, w);
    for (int k = 0; k < h.GroupSize[group]; ++k)
      TranslateBone(h, h.GroupMembers[group][k], delta, w);
  }

  // A bone that can't be moved without bending a bone outside its body part
  // (the first bone of a part: the spine's first, a thigh, a shoulder, the
  // neck, a finger's first) swings about its own place instead: its farthest
  // child goes as far and the same way as the cursor did
  void SwingBone(Held& h, int bone, float const* target, float (*w)[FloatsPerBone])
  {
    ComposeHeld(h, w);
    int child = -1;
    float best = 1e-3f;
    for (int c = bone + 1; c < h.Bones; ++c)
      if (h.Parents[c] == bone && h.InHeld[c])
      {
        float d = Distance3(w[c], w[bone]);
        if (d > best)
        {
          best = d;
          child = c;
        }
      }
    if (child < 0)
      return;
    float u[3] = { w[child][0] - w[bone][0], w[child][1] - w[bone][1], w[child][2] - w[bone][2] };
    float v[3] = { u[0] + target[0] - w[bone][0], u[1] + target[1] - w[bone][1], u[2] + target[2] - w[bone][2] };
    if (Length3(v) < 1e-4f)
      return;
    Normalize3(u);
    Normalize3(v);
    float q[4];
    FromTo(u, v, q);
    ApplyWorldTurn(h, bone, q, w);
  }

  // The bone moved to the target: the held root (Hips) moves the whole body,
  // a face bone moves itself; any other bone bends the bones above it (up to
  // chainBones of them) so it gets there, as near as they reach. They're
  // only ever bones of its own body part (an arm's, a leg's, the spine's, the
  // neck's, a finger's), never the held root: a hand or a knee doesn't turn
  // the whole body (2026-11-08, the user: "using a bone moves the whole body
  // instead of that bone"). A part's first bone swings instead (SwingBone).
  // From the pose as it is.
  // Since 2026-11-16 (PoseKinematics.inl): a hand or foot bends its arm or
  // leg about the elbow's or knee's hinge, within its limits, and keeps its
  // angle (Hands and feet keep their angle); an arm's collarbone joins in;
  // an elbow or knee is aimed by its upper bone; a chest or head bends the
  // spine or neck in a curve; a toe with 3 bones taken in moves its foot;
  // the Hips keep the feet where they stand; pinned hands and feet stay.
  void SolveMove(Held& h, int bone, float const* target, int chainBones)
  {
    static float w[MaxBones][FloatsPerBone];
    if (bone < 0 || bone >= h.Bones || !h.InHeld[bone])
      return;
    ComposeHeld(h, w);
    KinHold hold;
    bool kin = h.Kin.Found;
    if (kin)
      StartHold(h, bone, w, hold);
    float endTurn[4];
    memcpy(endTurn, w[bone] + 4, sizeof(endTurn));
    QNorm(endTurn);
    if (bone == h.Root || h.Face[bone] || g_FreeMove)
    {
      // Moved itself (Free move: any bone; its children go with it)
      float delta[3] = { target[0] - w[bone][0], target[1] - w[bone][1], target[2] - w[bone][2] };
      TranslateBone(h, bone, delta, w);
      if (kin)
        FinishHold(h, hold, w);
      return;
    }
    int chain[4];
    int count = 0;
    for (int a = h.Parents[bone]; a >= 0 && count < chainBones && count < 4 && h.InHeld[a] && a != h.Root &&
        h.Part[a] == h.Part[bone]; a = h.Parents[a])
      chain[count++] = a;
    int role = -1, limbIndex = kin ? LimbOf(h, bone, &role) : -1;
    KinLimb const* limb = limbIndex >= 0 ? &h.Kin.Limbs[limbIndex] : nullptr;
    int toeRole = -1, toeLimb = kin && h.Parents[bone] >= 0 ? LimbOf(h, h.Parents[bone], &toeRole) : -1;
    bool done = true;
    if (limb && role == 2 && count >= 2 && chain[0] == limb->Mid && chain[1] == limb->Upper)
    {
      // A hand or a foot
      ReachLimb(h, limbIndex, target, count, w);
      if (g_KeepEndTurn)
        SetWorldTurn(h, bone, endTurn, w);
    }
    else if (limb && role == 1 && count >= 1 && chain[0] == limb->Upper)
      AimLimbJoint(h, limbIndex, target, count, w);   // an elbow or a knee
    else if (toeLimb >= 0 && toeRole == 2 && !h.Kin.Limbs[toeLimb].Arm && count >= 3 &&
        chain[0] == h.Kin.Limbs[toeLimb].End && chain[1] == h.Kin.Limbs[toeLimb].Mid)
      ReachToe(h, toeLimb, bone, target, w);          // a toe, its foot moving
    else if (kin && g_SpineCurve && count >= 2 && InSpineOrNeck(h, chain[0]) && InSpineOrNeck(h, chain[count - 1]))
    {
      bool all = true;
      for (int k = 0; k < count; ++k)
        all = all && InSpineOrNeck(h, chain[k]);
      if (all)
        CurveChain(h, bone, chain, count, target, w);   // a chest or a head
      else
        done = false;
    }
    else
      done = false;
    if (done)
    {
      FinishHold(h, hold, w);
      return;
    }
    if (count == 0)
    {
      SwingBone(h, bone, target, w);
      if (kin)
        FinishHold(h, hold, w);
      return;
    }
    if (count == 2)
    {
      TwoBone(h, chain[1], chain[0], bone, target, w);
      if (kin)
        FinishHold(h, hold, w);
      return;
    }
    // One bone: it turns to point at it; more: each in turn, again and again
    // (cyclic coordinate descent)
    int passes = count == 1 ? 1 : 16;
    for (int pass = 0; pass < passes; ++pass)
    {
      for (int k = 0; k < count; ++k)
      {
        ComposeHeld(h, w);
        float const* a = w[chain[k]];
        float u[3] = { w[bone][0] - a[0], w[bone][1] - a[1], w[bone][2] - a[2] };
        float v[3] = { target[0] - a[0], target[1] - a[1], target[2] - a[2] };
        if (Length3(u) < 1e-4f || Length3(v) < 1e-4f)
          continue;
        Normalize3(u);
        Normalize3(v);
        float q[4];
        FromTo(u, v, q);
        ApplyWorldTurn(h, chain[k], q, w);
      }
      ComposeHeld(h, w);
      if (Distance3(w[bone], target) < 0.001f)
        break;
    }
    // (a hand or foot with only its forearm or shin bending keeps its angle too)
    if (limb && role == 2 && g_KeepEndTurn)
      SetWorldTurn(h, bone, endTurn, w);
    if (kin)
      FinishHold(h, hold, w);
  }

  // The eye a dragged bone stands for: an eye, or an iris (its child); -1
  int EyeOf(Held const& h, int bone)
  {
    if (bone < 0 || bone >= h.Bones)
      return -1;
    if (h.Eye[bone])
      return bone;
    int p = h.Parents[bone];
    return p >= 0 && h.Eye[p] ? p : -1;
  }

  // The eyes turned to look somewhere: from the pose they were pressed in,
  // each eye's gaze is towards its iris (the iris sits 0.5 - 1.4 cm in front
  // of the eye's middle in Faith's, KrugerSec's and the women's skeletons'
  // bind poses), they looked at a point LookDistance in front of them, and
  // that point moves LookGain times as far as the cursor went from start to
  // target. Both eyes look at the same point (they turn in for a near one).
  // eye: one eye (an eye bone), or -1 for the Eyes group.
  void LookEyes(Held& h, int eye, float const* start, float const* target)
  {
    static float w[MaxBones][FloatsPerBone];
    ComposeHeld(h, w);
    int eyes[2], irises[2], count = 0;
    auto add = [&](int e)
    {
      if (count >= 2 || e < 0 || e >= h.Bones || !h.Eye[e])
        return;
      int iris = -1;
      float best = 1e-4f;
      for (int c = e + 1; c < h.Bones; ++c)
        if (h.Parents[c] == e && Distance3(w[c], w[e]) > best)
        {
          best = Distance3(w[c], w[e]);
          iris = c;
        }
      if (iris < 0)
        return;
      eyes[count] = e;
      irises[count] = iris;
      ++count;
    };
    if (eye >= 0)
      add(eye);
    else
      for (int k = 0; k < h.GroupSize[EyesGroup]; ++k)
        add(h.GroupMembers[EyesGroup][k]);
    if (count == 0)
      return;
    float middle[3] = {}, gaze[3] = {};
    float gazes[2][3];
    for (int n = 0; n < count; ++n)
      for (int k = 0; k < 3; ++k)
      {
        gazes[n][k] = w[irises[n]][k] - w[eyes[n]][k];
        middle[k] += w[eyes[n]][k] / count;
      }
    for (int n = 0; n < count; ++n)
    {
      Normalize3(gazes[n]);
      for (int k = 0; k < 3; ++k)
        gaze[k] += gazes[n][k];
    }
    Normalize3(gaze);
    float look[3];
    for (int k = 0; k < 3; ++k)
      look[k] = middle[k] + gaze[k] * LookDistance + (target[k] - start[k]) * LookGain;
    for (int n = 0; n < count; ++n)
    {
      float want[3] = { look[0] - w[eyes[n]][0], look[1] - w[eyes[n]][1], look[2] - w[eyes[n]][2] };
      if (Length3(want) < 1e-4f)
        continue;
      Normalize3(want);
      float q[4];
      FromTo(gazes[n], want, q);
      ApplyWorldTurn(h, eyes[n], q, w);
    }
  }

  // ---- Bones stop at walls (the game's thread: collision::Sweep, the free
  // camera's sweep through the game's physics, is only made there) ----

  // Within the space round where the character stands (its own body), when
  // the world's sweeps hit its own body (SelfHit 1)
  bool InSelf(Held const& h, float const* p)
  {
    if (h.SelfHit != 1)
      return false;
    float const* root = h.World[1];
    float dx = p[0] - root[0], dz = p[2] - root[2];
    return dx * dx + dz * dz < SelfRadius * SelfRadius && p[1] > root[1] - 0.3f && p[1] < root[1] + 2.2f;
  }

  // Where the line from p to q leaves that space (false: it doesn't)
  bool LeaveSelf(Held const& h, float const* p, float const* q, float* out)
  {
    for (int s = 1; s <= 50; ++s)
    {
      float t = s / 50.f;
      for (int k = 0; k < 3; ++k)
        out[k] = p[k] + (q[k] - p[k]) * t;
      if (!InSelf(h, out))
        return true;
    }
    return false;
  }

  // Whether the world's sweeps hit the character's own body: 4 rays at its
  // chest from 0.8 m away (in front, behind, left, right). Its own collision
  // capsule would be hit 0.1 - 0.55 m from its middle by 3 or 4 of them; a
  // wall can't be that near on 3 sides. Whether the sweep (collision filter
  // 9) takes characters at all wasn't known (2026-11-08); the log says.
  void ProbeSelf(Held& h)
  {
    float const* root = h.World[1];
    float centre[3] = { root[0], root[1] + 1.2f, root[2] };
    const float ways[4][2] = { { 1.f, 0.f }, { -1.f, 0.f }, { 0.f, 1.f }, { 0.f, -1.f } };
    int hits = 0;
    float nearest = 99.f, farthest = 0.f;
    for (int k = 0; k < 4; ++k)
    {
      float from[3] = { centre[0] + ways[k][0] * 0.8f, centre[1], centre[2] + ways[k][1] * 0.8f };
      float fraction = 1.f, normal[3];
      if (!collision::Sweep(from, centre, 0.f, &fraction, normal))
        continue;
      float distance = 0.8f * (1.f - fraction);   // from the middle
      if (distance >= 0.1f && distance <= 0.55f)
      {
        ++hits;
        nearest = (std::min)(nearest, distance);
        farthest = (std::max)(farthest, distance);
      }
    }
    h.SelfHit = hits >= 3 ? 1 : 0;
    if (h.SelfHit)
      util::log::Write("Pose: %s 0x%I64X: the world's sweeps hit its own body (%d of 4 rays at its chest, %.2f - %.2f m from "
        "its middle): what they touch within %.1f m of where it stands is taken as its own and left out", h.Label, h.Entity,
        hits, nearest, farthest, SelfRadius);
    else
      util::log::Write("Pose: %s 0x%I64X: the world's sweeps don't hit its own body (%d of 4 rays at its chest touched "
        "something 0.1 - 0.55 m from its middle): everything they touch counts", h.Label, h.Entity, hits);
  }

  // A ball of radius moved from from towards to as far as it gets, sliding
  // along what it touches (the free camera's way, collision::Move), its own
  // body left out (InSelf); where it ends up is in out
  void ClampPath(Held const& h, float const* from, float const* to, float radius, float* out)
  {
    float place[3] = { from[0], from[1], from[2] }, target[3] = { to[0], to[1], to[2] };
    for (int i = 0; i < 6; ++i)
    {
      float delta[3] = { target[0] - place[0], target[1] - place[1], target[2] - place[2] };
      float length = Length3(delta);
      if (length < 1e-4f)
        break;
      float fraction = 1.f, normal[3];
      if (!collision::Sweep(place, target, radius, &fraction, normal))
      {
        memcpy(place, target, sizeof(place));
        break;
      }
      float contact[3] = { place[0] + delta[0] * fraction, place[1] + delta[1] * fraction, place[2] + delta[2] * fraction };
      if (InSelf(h, contact))
      {
        // Its own body: on from where the line leaves it
        float past[3];
        if (!LeaveSelf(h, contact, target, past))
        {
          memcpy(place, target, sizeof(place));
          break;
        }
        memcpy(place, past, sizeof(place));
        continue;
      }
      float direction[3] = { delta[0] / length, delta[1] / length, delta[2] / length };
      float wall[3] = { normal[0], normal[1], normal[2] };
      if (Length3(wall) > 1e-4f)
        Normalize3(wall);
      else
        for (int k = 0; k < 3; ++k)
          wall[k] = -direction[k];
      // Already touching and going away from it (or along it): free
      if (delta[0] * wall[0] + delta[1] * wall[1] + delta[2] * wall[2] >= 0.f)
      {
        memcpy(place, target, sizeof(place));
        break;
      }
      float travel = fraction * length - 0.01f;
      if (travel > 0.f)
        for (int k = 0; k < 3; ++k)
          place[k] += direction[k] * travel;
      // The rest of the move along the surface
      float rest[3] = { target[0] - place[0], target[1] - place[1], target[2] - place[2] };
      float into = rest[0] * wall[0] + rest[1] * wall[1] + rest[2] * wall[2];
      for (int k = 0; k < 3; ++k)
        target[k] = place[k] + rest[k] - wall[k] * into;
    }
    memcpy(out, place, sizeof(place));
  }

  // Whether a ball going from from to to goes into something (its own body
  // left out)
  bool PathBlocked(Held const& h, float const* from, float const* to, float radius)
  {
    float place[3] = { from[0], from[1], from[2] };
    for (int i = 0; i < 4; ++i)
    {
      float delta[3] = { to[0] - place[0], to[1] - place[1], to[2] - place[2] };
      if (Length3(delta) < 1e-4f)
        return false;
      float fraction = 1.f, normal[3];
      if (!collision::Sweep(place, to, radius, &fraction, normal))
        return false;
      float contact[3] = { place[0] + delta[0] * fraction, place[1] + delta[1] * fraction, place[2] + delta[2] * fraction };
      if (!InSelf(h, contact))
        return delta[0] * normal[0] + delta[1] * normal[1] + delta[2] * normal[2] < 0.f;   // into it, not away
      if (!LeaveSelf(h, contact, to, place))
        return false;
    }
    return false;
  }

  // A joint whose path the walls check: the body's main joints (not the face,
  // fingers or helpers, which sit close to a main one)
  bool CheckedJoint(Held const& h, int bone)
  {
    return h.InHeld[bone] && !h.Face[bone] && !h.Finger[bone] && !h.Helper[bone];
  }

  // One frame of a bone dragged with "Bones stop at walls": the bone goes
  // from where it was last towards where it's wanted, as far as it gets
  // (sliding along walls), and the pose is solved for that from the pose it
  // was pressed in; if any of the main joints that move would go into
  // something on the way, the frame is left out (the pose stays as it was
  // last). Faces and groups aren't checked. Game thread, under g_Mutex.
  void DragWithWalls(Held& h, int heldIndex)
  {
    static float before[MaxBones][FloatsPerBone], after[MaxBones][FloatsPerBone];
    if (!g_Move.On || g_Move.Held != heldIndex || !g_Move.WantOk || g_Move.Group >= 0 || g_Move.Bone < 0 ||
        g_Move.Bone >= h.Bones || !h.WorldOk)
      return;
    if (g_Move.Pose.Turns.size() != (size_t)h.Bones * 4 || g_Move.Pose.Moves.size() != (size_t)h.Bones * 3 ||
        g_Move.Accepted.Turns.size() != (size_t)h.Bones * 4 || g_Move.Accepted.Moves.size() != (size_t)h.Bones * 3)
      return;
    int bone = g_Move.Bone;
    if (h.Face[bone])
    {
      // A face bone moves itself a little, inside the head: not checked
      ApplySnapshot(h, g_Move.Pose);
      SolveMove(h, bone, g_Move.Want, g_MoveBones);
      return;
    }
    if (h.SelfHit < 0)
      ProbeSelf(h);
    float place[3];
    ClampPath(h, g_Move.AcceptedPlace, g_Move.Want, BoneRadius, place);
    memcpy(g_Move.Target, g_Move.Want, sizeof(g_Move.Target));   // the line to where it's wanted shows when it stops short
    ApplySnapshot(h, g_Move.Accepted);
    ComposeHeld(h, before);
    ApplySnapshot(h, g_Move.Pose);
    SolveMove(h, bone, place, g_MoveBones);
    ComposeHeld(h, after);
    bool blocked = false;
    for (int n = 0; n < h.Count && !blocked; ++n)
    {
      int i = (int)h.List[n];
      if (i == bone || !CheckedJoint(h, i) || Distance3(before[i], after[i]) < 0.002f)
        continue;
      blocked = PathBlocked(h, before[i], after[i], JointRadius);
    }
    if (blocked)
    {
      ApplySnapshot(h, g_Move.Accepted);
      return;
    }
    g_Move.Accepted = SnapshotOf(h);
    memcpy(g_Move.AcceptedPlace, after[bone], sizeof(g_Move.AcceptedPlace));
  }

  // ---- The Pose window: its parts and shared bits ----

  char const* PoseIntroText()
  {
    return "Pose Faith, KrugerSec and other people bone by bone: pick one, Take pose, then drag its bones' dots on "
      "the picture where you like, turn a bone with its rings, or use the sliders here. Freeze time holds everyone "
      "while you pose (with time running, posed KrugerSec keep their AI: they walk, aim and fight in the pose).";
  }

  void SameLineIfFits(char const* nextLabel, bool checkbox)
  {
    char const* end = strstr(nextLabel, "##");
    ImGuiStyle const& style = ImGui::GetStyle();
    float width = ImGui::CalcTextSize(nextLabel, end).x +
      (checkbox ? ImGui::GetFrameHeight() + style.ItemInnerSpacing.x : style.FramePadding.x * 2.f);
    // The cursor is already back at the line's start here: measure from where the previous item ended
    float right = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;
    if (ImGui::GetItemRectMax().x + style.ItemSpacing.x + width <= right)
      ImGui::SameLine();
  }

  bool TabButton(char const* label, bool selected, float width)
  {
    ImVec4 white = ImGui::GetStyleColorVec4(ImGuiCol_Text);
    ImVec4 red = ImGui::GetStyleColorVec4(ImGuiCol_Button);
    if (selected)
    {
      ImGui::PushStyleColor(ImGuiCol_Button, white);
      ImGui::PushStyleColor(ImGuiCol_ButtonHovered, white);
      ImGui::PushStyleColor(ImGuiCol_ButtonActive, white);
      ImGui::PushStyleColor(ImGuiCol_Text, red);
    }
    bool clicked = ImGui::Button(label, ImVec2(width, ImGui::GetFrameHeight() * 1.35f));
    if (selected)
      ImGui::PopStyleColor(4);
    return clicked;
  }

  bool AnyEdited(Held const& h)
  {
    for (int n = 0; n < h.Count; ++n)
      if (IsEdited(h, (int)h.List[n]))
        return true;
    return false;
  }

  Held* DrawWho()
  {
    float fontSize = ImGui::GetFontSize();
    Held* active = g_Active >= 0 && g_Held[g_Active].Used && !g_Held[g_Active].Restoring ? &g_Held[g_Active] : nullptr;
    int editable = 0;
    for (int i = 0; i < MaxHeld; ++i)
      editable += g_Held[i].Used && !g_Held[i].Restoring;
    char header[128];
    _snprintf_s(header, sizeof(header), _TRUNCATE, "Who: %s###PoseWho",
      active ? active->Label : (editable ? "pick a held one" : "nobody held"));
    if (ImGui::CollapsingHeader(header, ImGuiTreeNodeFlags_DefaultOpen))
    {
      ImGui::BeginChild("##PoseWho", ImVec2(fontSize * 18.f, fontSize * 6.5f), true);
      ImGui::Text("Near the camera (%d):", (int)g_Candidates.size());
      for (Candidate const& c : g_Candidates)
      {
        char row[200];
        char const* note = c.LettingGo ? "  (being let go)" : c.Held ? "  (held)" : c.Skeleton < 0 ?
          "  (skeleton not known: can't be posed; CT.log says what was read of it)" : c.Cant == Cant_NoRoot ?
          "  (its bones don't hang on one bone: can't be posed)" : c.Cant == Cant_RootUnanimated ?
          "  (the game animates no main bone of it: can't be posed)" : c.Animated == 0 ?
          "  (the game doesn't animate it now: can't be posed)" : c.OtherOverride ? "  (a replay or Dash ghost poses it)" : "";
        if (c.Kind == Kind_Faith)
          _snprintf_s(row, sizeof(row), _TRUNCATE, "Faith%s##%I64X", note, c.Entity);
        else
          _snprintf_s(row, sizeof(row), _TRUNCATE, "%s, %.1f m%s##%I64X", KindName(c.Kind, c.Skeleton), c.Distance, note,
            c.Entity);
        if (ImGui::Selectable(row, g_Picked == c.Entity))
        {
          g_Picked = c.Entity;
          for (int i = 0; i < MaxHeld; ++i)
            if (g_Held[i].Used && !g_Held[i].Restoring && g_Held[i].Entity == c.Entity && g_Active != i)
            {
              g_Active = i;
              g_SelectedBone = g_Held[i].Root;
              g_SelectedGroup = -1;
              g_SelectedWeapon = -1;
            }
        }
        if (note[0] && !c.Held)
          ImGui::ItemTooltip(note + 2);
      }
      if (g_Candidates.empty())
      {
        ImGui::PushTextWrapPos(0.f);
        ImGui::TextDisabled(g_ScannedAt ? "Nobody that can be posed within %.0f m of the camera" : "Looking...", ScanRadius);
        ImGui::PopTextWrapPos();
      }
      ImGui::EndChild();

      ImGui::SameLine();
      active = g_Active >= 0 && g_Held[g_Active].Used && !g_Held[g_Active].Restoring ? &g_Held[g_Active] : nullptr;
      int heldCount = 0;
      for (int i = 0; i < MaxHeld; ++i)
        heldCount += g_Held[i].Used;
      ImGui::BeginChild("##PoseWhoInfo", ImVec2(0.f, fontSize * 6.5f), false);
      Candidate const* picked = nullptr;
      for (Candidate const& c : g_Candidates)
        if (c.Entity == g_Picked)
          picked = &c;
      bool canTake = picked && !picked->Held && picked->Skeleton >= 0 && picked->Cant == Cant_No && picked->Animated != 0 &&
        !picked->OtherOverride;
      if (!canTake)
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.5f);
      if (ImGui::Button("Take pose", ImVec2(fontSize * 7.f, 0.f)) && canTake)
      {
        g_TakeWhat = *picked;
        g_TakeRequest = true;
      }
      if (!canTake)
        ImGui::PopStyleVar();
      ImGui::HelpMarker("Holds the picked one's pose as it is now: from then on it's drawn in the pose you make, from its "
        "hips down to its fingers, while it stays where the game has it. Up to 8 at a time.");
      bool activeHasKeys = active && KeysSpanTime(active->Keys);
      bool anyHeldHasKeys = false;
      for (int i = 0; i < MaxHeld; ++i)
        if (g_Held[i].Used && KeysSpanTime(g_Held[i].Keys))
          anyHeldHasKeys = true;
      if (activeHasKeys)
      {
        if (ImGui::ConfirmButton("Let go") && active)
          g_ReleaseRequest = g_Active;
      }
      else if (ImGui::Button("Let go") && active)
        g_ReleaseRequest = g_Active;
      char allLabel[48];
      _snprintf_s(allLabel, sizeof(allLabel), _TRUNCATE, "Let go of all (%d)", heldCount);
      SameLineIfFits(allLabel);
      if (anyHeldHasKeys)
      {
        if (ImGui::ConfirmButton(allLabel) && heldCount)
          g_ReleaseRequest = -1;
      }
      else if (ImGui::Button(allLabel) && heldCount)
        g_ReleaseRequest = -1;

      if (active)
      {
        ImGui::Text("Editing: %s", active->Label);
        ImGui::HelpMarker(PoseIntroText());
        ImGui::PushTextWrapPos(0.f);
        ImGui::TextDisabled("%s", active->Status[0] ? active->Status : "Being taken...");
        if (active->CheckLine[0])
          ImGui::TextDisabled("%s", active->CheckLine);
        ImGui::PopTextWrapPos();
      }
      if (g_HairWhilePaused)
      {
        char hairStatus[160];
        paused_hair::Status(hairStatus, sizeof(hairStatus));
        if (hairStatus[0])
        {
          ImGui::PushTextWrapPos(0.f);
          ImGui::TextDisabled("%s", hairStatus);
          ImGui::PopTextWrapPos();
        }
      }
      for (int i = 0; i < MaxHeld; ++i)
        if (g_Held[i].Used && g_Held[i].Restoring && g_Held[i].RestoreWaitLogged)
        {
          ImGui::PushTextWrapPos(0.f);
          ImGui::TextDisabled("%s: let go, its bones put back when the game animates it again (it doesn't now). Let go of "
            "all lets it go now, as posed.", g_Held[i].Label);
          ImGui::PopTextWrapPos();
        }
      ImGui::EndChild();
    }

    active = g_Active >= 0 && g_Held[g_Active].Used && !g_Held[g_Active].Restoring ? &g_Held[g_Active] : nullptr;
    if (!active)
    {
      int heldCount = 0;
      for (int i = 0; i < MaxHeld; ++i)
        heldCount += g_Held[i].Used;
      ImGui::PushTextWrapPos(0.f);
      ImGui::TextDisabled("%s", PoseIntroText());
      ImGui::PopTextWrapPos();
      ImGui::TextDisabled(heldCount ? "Pick a held one in the list to edit it" : "No pose held");
      if (g_LetGoMessage[0])
      {
        ImGui::PushTextWrapPos(0.f);
        ImGui::TextUnformatted(g_LetGoMessage);
        ImGui::PopTextWrapPos();
      }
    }
    return active;
  }

  void DrawOptionsTab(Held& h, bool& configChanged)
  {
    float fontSize = ImGui::GetFontSize();
    configChanged |= ImGui::Checkbox("Show the bones on the picture", &g_ShowBones);
    configChanged |= ImGui::Checkbox("Face", &g_ShowFace);
    SameLineIfFits("Group the face", true);
    configChanged |= ImGui::Checkbox("Group the face", &g_GroupFace);
    ImGui::HelpMarker("The face's bones as groups (eyes, eyelids, brows, lips, mouth corners, cheeks, nose): a group's dot "
      "turns or moves all its bones together. Dragging the Eyes makes them look at a point.");
    SameLineIfFits("Fingers", true);
    configChanged |= ImGui::Checkbox("Fingers", &g_ShowFingers);
    SameLineIfFits("Helper bones", true);
    configChanged |= ImGui::Checkbox("Helper bones", &g_ShowHelpers);
    const char* bends[] = { "only the bone above it", "the 2 bones above it (arm, leg)", "the 3 bones above it",
      "the 4 bones above it" };
    int bendIndex = g_MoveBones - 1;
    ImGui::PushItemWidth(fontSize * 9.f);
    if (ImGui::Combo("A dragged bone bends", &bendIndex, bends, 4))
    {
      g_MoveBones = bendIndex + 1;
      configChanged = true;
    }
    ImGui::PopItemWidth();
    ImGui::Checkbox("Free move", &g_FreeMove);
    ImGui::HelpMarker("Dragging a bone moves the bone itself instead of bending the bones above it; the bones below it "
      "go with it. Drag one of the picked bone's red, green or blue arrows to move it along that axis only.");
    configChanged |= ImGui::Checkbox("Bones stop at walls", &g_StopAtWalls);
    if (g_StopAtWalls && !g_WallsReady)
    {
      ImGui::SameLine();
      ImGui::TextDisabled("(not available now: see CT.log)");
    }
    ImGui::Checkbox("Idle movement", &g_IdleOn);
    ImGui::HelpMarker("Lays the character's own idle animation (breathing, weight shifts) over the pose in small moves; "
      "Amount scales them. It pauses while the character walks or runs, and under Freeze time.");
    if (g_IdleOn)
    {
      ImGui::SliderFloat("Amount##PoseIdle", &g_IdleAmount, 0.f, 2.f, "%.2f");
      if (g_IdleUnavailable[0])
        ImGui::TextDisabled("%s", g_IdleUnavailable);
    }
    configChanged |= ImGui::Checkbox("Rings on the bone's own axes", &g_RingsOnBone);
    ImGui::HelpMarker("Ticked, the picked bone's red, green and blue rings are its own X, Y and Z; unticked, the world's. "
      "The white ring turns it about the view.");

    // The kinematics (2026-11-16, PoseKinematics.inl, PoseMotion.inl)
    ImGui::SubHeading("Joints");
    configChanged |= ImGui::Checkbox("Elbows and knees bend like real joints", &g_JointHinges);
    ImGui::HelpMarker("A dragged hand or foot bends its elbow or knee only the way it bends by nature, at most about 150 "
      "degrees (with \"A dragged bone bends\" at 2 bones or more). Off: no hinge and no limit.");
    configChanged |= ImGui::Checkbox("Dragged hands and feet keep their angle", &g_KeepEndTurn);
    ImGui::HelpMarker("On: a hand or foot you drag stays turned the way it is in the world (a foot flat on the floor "
      "stays flat). Off: it turns with its forearm or shin.");
    configChanged |= ImGui::Checkbox("Feet stay where they stand when the Hips move", &g_FeetPlanted);
    ImGui::HelpMarker("Dragging or turning the Hips bends the legs so the feet stay put, as far as the legs reach (the "
      "Hips' Move field still moves the whole body). Off: the whole body moves with the Hips.");
    configChanged |= ImGui::Checkbox("Collarbone joins an arm raise", &g_CollarFollows);
    ImGui::HelpMarker("A dragged hand or elbow takes its collarbone along a little, so the shoulder rises with a raised arm "
      "and comes forward with a reach.");
    configChanged |= ImGui::Checkbox("Back and neck bend in a curve", &g_SpineCurve);
    ImGui::HelpMarker("Dragging the chest or the head bends the spine or neck below it in a curve, not at one joint (with "
      "\"A dragged bone bends\" at 2 bones or more).");
    configChanged |= ImGui::Checkbox("A far reach bends the back", &g_FarReach);
    ImGui::HelpMarker("A hand dragged past its arm's reach bends the back towards it (at most 35 degrees).");
    configChanged |= ImGui::Checkbox("Roll and twist bones follow", &g_HelpersFollow);
    ImGui::HelpMarker("The roll and twist bones follow their limb's turn, so a turned wrist doesn't twist like a sweet "
      "wrapper.");

    ImGui::SubHeading("While the keyframes play");
    configChanged |= ImGui::SliderFloat("Loose timing", &g_LooseTiming, 0.f, 1.f, "%.2f");
    ImGui::HelpMarker("While the keyframes play, the head and arms follow the chest a moment later, the hands later still. "
      "0: off.");
    configChanged |= ImGui::SliderFloat("Follow-through", &g_FollowThrough, 0.f, 1.f, "%.2f");
    ImGui::HelpMarker("While the keyframes play, the arms, hands and head lag a little and settle, as if they had weight. "
      "0: off.");
    configChanged |= ImGui::Checkbox("Feet stay put between keyframes", &g_FootLock);
    ImGui::HelpMarker("While the keyframes play, a foot that's in the same place at the keyframes either side stays there "
      "instead of sliding. Not with an animation clip on.");

    ImGui::SubHeading("On the picture");
    configChanged |= ImGui::Checkbox("The picked bone's path over the keyframes", &g_ShowTrail);
    ImGui::HelpMarker("An orange line of where the picked bone goes over the keyframes, with a dot at each keyframe time. "
      "Drag a dot to move the bone at that time (keyframed when you let go); a click only moves the playhead there. "
      "With Auto key off, keyframe or undo a change first.");
    configChanged |= ImGui::Checkbox("Ghost poses at the keyframes either side", &g_ShowGhosts);
    ImGui::HelpMarker("The pose at the keyframe time before the playhead (blue) and after it (green), as lines between "
      "the bones.");

    ImGui::SubHeading("Look at");
    {
      const char* looks[] = { "Off", "The camera", "A point (drag its dot)" };
      int look = h.LookMode < Look_Off || h.LookMode > Look_Point ? Look_Off : h.LookMode;
      if (ImGui::Combo("##PoseLook", &look, looks, 3) && look != h.LookMode)
      {
        if (look == Look_Point && !h.LookPointOk)
          PlaceLookPoint(h);
        h.LookMode = look;
      }
      ImGui::HelpMarker("The eyes look at the camera or at a point you drag, and the chest, neck and head turn part of the "
        "way (Amount). It keeps looking while the body moves or the keyframes play.");
      if (h.LookMode != Look_Off)
      {
        ImGui::SliderFloat("Amount##PoseLookAmount", &h.LookAmount, 0.f, 1.f, "%.2f");
        if (!h.Kin.FaceOk)
          ImGui::TextDisabled("This character's eyes or head weren't found: it can't look.");
        else if (h.LookMode == Look_Point && !h.LookPointOk)
          ImGui::TextDisabled("Its point is placed in front of the face once the pose is in the world.");
      }
    }

    ImGui::SubHeading("Keyframes");
    bool wasOn = h.Keys.On;
    ImGui::Checkbox("Use the keyframes", &h.Keys.On);
    if (h.Keys.On && !wasOn)
      h.KeysAppliedOk = false;
    configChanged |= ImGui::Checkbox("Auto key (every change is keyframed at once)", &g_AutoKey);
    ImGui::HelpMarker("While the keyframes are on and not playing, every change to a bone is keyframed at the playhead (the "
      "KEYFRAMES row's red Auto key is the same switch). Off: press Keyframe to keep a change.");
    configChanged |= ImGui::Checkbox("Hold the times around a new keyframe", &g_KeyAnchors);
    ImGui::HelpMarker("A bone's first keyframe also gets hollow keyframes at the nearest times before and after, so the "
      "change shows only between them.");
    configChanged |= ImGui::Checkbox("Times snap to 0.05 s", &g_SnapKeys);
    ImGui::HelpMarker("Times you set by hand on the timeline snap to 0.05 s.");
    if (h.Keys.Clock == pose_keys::Clock_Game || h.Keys.Clock == pose_keys::Clock_Real)
      ImGui::SliderFloat("Speed", &h.Keys.Speed, 0.05f, 4.f, "%.2fx");
    else if (h.Keys.Clock == pose_keys::Clock_Track)
    {
      float begins = h.Keys.Start == 0.0 ? 0.f : (float)-h.Keys.Start;
      bool typing = TypingInField();
      if (ImGui::DragFloat("Begins at", &begins, 0.01f, 0.f, 3600.f, "%.2f s") && DragTakes(typing))
        h.Keys.Start = -(double)begins;   // (a click alone only rounds it: a file's -1.234 stays)
    }
    if (ImGui::ConfirmButton("Remove all keyframes"))
      RemoveAllKeys(h);
  }

  void DrawFilesTab(Held& h)
  {
    float fontSize = ImGui::GetFontSize();
    ImGui::InputText("Pose name", g_SaveName, sizeof(g_SaveName));
    SameLineIfFits("Save");
    if (ImGui::Button("Save"))
      SavePose(h, g_SaveName);
    ULONGLONG now = GetTickCount64();
    if (now - g_FilesListedAt > 2000)
    {
      ListPoseFiles();
      g_FilesListedAt = now;
    }
    if (!g_PoseFiles.empty())
    {
      ImGui::Text("Load onto %s:", h.Label);
      ImGui::SameLine();
      ImGui::HelpMarker("Each saved bone with the same name gets the saved turn, so a pose loads onto any skeleton "
        "with those bones (Faith's onto a KrugerSec's and so on). Where two skeletons' bones are set up differently, "
        "a bone can come out turned another way. Undo takes a load back.");
      ImGui::BeginChild("##PoseFiles", ImVec2(0.f, fontSize * 5.f), true);
      for (std::string const& file : g_PoseFiles)
      {
        std::string label = file + "##posefile";
        if (ImGui::Selectable(label.c_str(), false))
          LoadPose(h, file.c_str());
      }
      ImGui::EndChild();
    }
    if (g_FileMessage[0])
    {
      ImGui::PushTextWrapPos(0.f);
      ImGui::TextDisabled("%s", g_FileMessage);
      ImGui::PopTextWrapPos();
    }

    ImGui::SubHeading("Keyframes");
    ImGui::InputText("Keyframes name", g_KeySaveName, sizeof(g_KeySaveName));
    bool canSaveKeys = pose_keys::KeyCount(h.Keys) > 0 || AnyEdited(h);
    SameLineIfFits("Save##PoseKeysSave");
    if (!canSaveKeys)
      ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.5f);
    if (ImGui::Button("Save##PoseKeysSave") && canSaveKeys && SaveKeys(h, g_KeySaveName))
    {
      char clean[64];
      if (CleanName(g_KeySaveName, clean, sizeof(clean)))
        h.KeysFile = h.KeysSuggest = PoseKeysPath(clean);   // (the keyframes bar's Save writes there from now on)
    }
    if (!canSaveKeys)
      ImGui::PopStyleVar();
    if (now - g_KeyFilesListedAt > 2000)
    {
      ListKeyFiles();
      g_KeyFilesListedAt = now;
    }
    ImGui::Text("Load keyframes onto %s:", h.Label);
    ImGui::BeginChild("##PoseKeyFiles", ImVec2(0.f, fontSize * 5.f), true);
    for (std::wstring const& file : g_KeyFiles)
    {
      std::string label = Utf8Of(file) + "##posekeysfile";
      if (ImGui::Selectable(label.c_str(), false))
      {
        std::wstring path = PoseKeysPath(file);
        if (LoadKeysFromPath(h, path))
        {
          h.KeysFile.clear();   // (Save asks after a load: a loaded file isn't written over by mistake)
          h.KeysSuggest = path;
        }
      }
    }
    ImGui::EndChild();
    if (g_FileMessage[0])
    {
      ImGui::PushTextWrapPos(0.f);
      ImGui::TextDisabled("%s", g_FileMessage);
      ImGui::PopTextWrapPos();
    }
    char autosave[128];
    AutosaveName(h, autosave, sizeof(autosave));
    char line[640];
    _snprintf_s(line, sizeof(line), _TRUNCATE, "The keyframes go when the pose is let go (a respawn or a level load lets "
      "go by itself): those at two times or more are then saved as \"%s\", and while you work too, at most every 30 s, "
      "so a crash or closing the game loses little; keyframes at one time only (a still) aren't saved by themselves: "
      "press Save above. A file of that name from before is kept once as \"%s before\".", autosave, autosave);
    ImGui::PushTextWrapPos(0.f);
    ImGui::TextDisabled("%s", line);
    ImGui::PopTextWrapPos();
  }

  void DrawBonesPane(Held& h, float height)
  {
    float fontSize = ImGui::GetFontSize();
    ImGui::BeginChild("##PoseBonesPane", ImVec2(fontSize * 15.f, height), true);
    ImGui::PushItemWidth(-1.f);
    ImGui::InputText("##PoseFind", g_Filter, sizeof(g_Filter));
    if (!g_Filter[0] && !ImGui::IsItemActive())
    {
      ImVec2 at = ImGui::GetItemRectMin();
      at.x += ImGui::GetStyle().FramePadding.x;
      at.y += ImGui::GetStyle().FramePadding.y;
      ImGui::GetWindowDrawList()->AddText(at, ImGui::GetColorU32(ImGuiCol_TextDisabled), "Find a bone");
    }
    ImGui::PopItemWidth();
    ImGui::BeginChild("##PoseBones", ImVec2(0.f, 0.f), true);
    bool scrollGroup = g_SelectedGroup != g_BoneListGroup;
    bool scrollBone = g_SelectedBone != g_BoneListBone;
    bool groupDrawn = false, boneDrawn = false;
    for (int n = 0; n < h.Count; ++n)
    {
      int i = (int)h.List[n];
      char const* name = BoneName(h, i);
      if (g_Filter[0])
      {
        bool match = false;
        size_t fl = strlen(g_Filter);
        for (char const* p = name; *p && !match; ++p)
          match = _strnicmp(p, g_Filter, fl) == 0;
        if (!match)
          continue;
      }
      else
      {
        if (GroupShown(h, h.Group[i]))
        {
          int g = h.Group[i];
          if (h.GroupMembers[g][0] != i)
            continue;
          char row[200];
          int indent = h.Depth[i] > 30 ? 30 : h.Depth[i];
          _snprintf_s(row, sizeof(row), _TRUNCATE, "%*s%s (%d bones)##group%d", indent, "", FaceGroupNames[g], h.GroupSize[g],
            g);
          bool wasSelected = g_SelectedGroup == g;
          bool clicked = ImGui::Selectable(row, wasSelected);
          if (wasSelected && scrollGroup && !clicked)
          {
            ImGui::SetScrollHere();
            g_BoneListGroup = g_SelectedGroup;
          }
          if (clicked)
          {
            g_SelectedGroup = g;
            g_SelectedBone = -1;
            g_SelectedWeapon = -1;
            g_BoneListGroup = g_SelectedGroup;
            g_BoneListBone = g_SelectedBone;
          }
          if (g_SelectedGroup == g)
            groupDrawn = true;
          bool keyed = false;
          for (int k = 0; k < h.GroupSize[g]; ++k)
            if (pose_keys::Find(h.Keys, h.GroupMembers[g][k]) != nullptr)
            {
              keyed = true;
              break;
            }
          char marks[3];
          int mark = 0;
          if (GroupEdited(h, g))
            marks[mark++] = '*';
          if (keyed)
            marks[mark++] = 'K';
          marks[mark] = '\0';
          if (marks[0])
          {
            ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - fontSize * 1.8f);
            ImGui::TextDisabled("%s", marks);
          }
          continue;
        }
        if (!BoneShown(h, i) && i != g_SelectedBone)
          continue;
      }
      char row[200];
      int indent = g_Filter[0] ? 0 : (h.Depth[i] > 30 ? 30 : h.Depth[i]);
      _snprintf_s(row, sizeof(row), _TRUNCATE, "%*s%s##bone%d", indent, "", name, i);
      bool wasSelected = g_SelectedBone == i;
      bool clicked = ImGui::Selectable(row, wasSelected);
      if (wasSelected && scrollBone && !clicked)
      {
        ImGui::SetScrollHere();
        g_BoneListBone = g_SelectedBone;
      }
      if (clicked)
      {
        g_SelectedBone = i;
        g_SelectedGroup = -1;
        g_SelectedWeapon = -1;
        g_BoneListBone = g_SelectedBone;
        g_BoneListGroup = g_SelectedGroup;
      }
      if (g_SelectedBone == i)
        boneDrawn = true;
      char marks[3];
      int mark = 0;
      if (IsEdited(h, i))
        marks[mark++] = '*';
      if (pose_keys::Find(h.Keys, i) != nullptr)
        marks[mark++] = 'K';
      marks[mark] = '\0';
      if (marks[0])
      {
        ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - fontSize * 1.8f);
        ImGui::TextDisabled("%s", marks);
      }
    }
    if (!groupDrawn)
      g_BoneListGroup = g_SelectedGroup;
    if (!boneDrawn)
      g_BoneListBone = g_SelectedBone;
    ImGui::EndChild();
    ImGui::EndChild();
  }

  // A Bone tab slider's edit (absolute: it sets the bone's turn or move from the slider) with the pins and planted feet
  // kept: made each frame from the pose the slider was taken in (g_SliderPress), as a ring's drag is from its press
  void SliderHold(Held& h, int bone, std::function<void()> const& edit, bool plantFeet = true)
  {
    if (!PlayingKeys(h) && h.Kin.Found && g_SliderPressHeld == h.Id &&
        g_SliderPress.Turns.size() == (size_t)h.Bones * 4 && g_SliderPress.Moves.size() == (size_t)h.Bones * 3)
      ApplySnapshot(h, g_SliderPress);
    HoldAround(h, bone, edit, plantFeet);
  }

  // A bone of an arm or a leg (2026-11-16): its elbow's or knee's direction (swung about the line from the shoulder or
  // hip to the hand or foot, which stay where they are), and a hand's or foot's pin
  void DrawLimbControls(Held& h, int bone, bool& slidersActive)
  {
    int role = -1, l = h.Kin.Found ? LimbOf(h, bone, &role) : -1;
    if (l < 0)
      return;
    KinLimb const& limb = h.Kin.Limbs[l];
    static float swivel = 0.f;
    static int swivelBone = -1;
    if (swivelBone != bone)
    {
      swivel = 0.f;
      swivelBone = bone;
    }
    // (not while the keyframes play with keys on the limb: they'd put it back each frame)
    bool keyedLimb = pose_keys::Find(h.Keys, limb.Upper) || pose_keys::Find(h.Keys, limb.Mid) ||
      pose_keys::Find(h.Keys, limb.End);
    if (PlayingKeys(h) && keyedLimb)
      ImGui::TextDisabled(limb.Arm ? "Elbow direction: pause the keyframes (the arm has keyframes)." :
        "Knee direction: pause the keyframes (the leg has keyframes).");
    else
    {
      float was = swivel;
      ImGui::DragFloat(limb.Arm ? "Elbow direction##PoseSwivel" : "Knee direction##PoseSwivel", &swivel, 0.5f, -180.f,
        180.f, "%+.0f deg");
      bool active = ImGui::IsItemActive();
      slidersActive |= active;
      if (swivel != was && std::isfinite(swivel) && std::isfinite(was))
        SwivelLimb(h, l, (swivel - was) * KinPi / 180.f);
      if (!active)
        swivel = 0.f;
      ImGui::SameLine();
      ImGui::HelpMarker(limb.Arm ? "Drag sideways: the elbow swings round the line from the shoulder to the hand, the "
        "hand staying where it is and as it's turned (the upper arm and forearm turn). It shows how far this drag has "
        "gone." : "Drag sideways: the knee swings round the line from the hip to the foot, the foot staying where it is "
        "and as it's turned (the thigh and shin turn). It shows how far this drag has gone.");
    }
    if (role == 2)
    {
      bool pinned = h.Pins[l].On;
      if (ImGui::Checkbox(limb.Arm ? "Pin the hand where it is##PosePin" : "Pin the foot where it is##PosePin", &pinned))
      {
        if (pinned)
        {
          static float w[MaxBones][FloatsPerBone];
          ComposeHeld(h, w);
          TakePin(h, l, w);
        }
        h.Pins[l].On = pinned;
      }
      ImGui::SameLine();
      ImGui::HelpMarker("Pinned, it stays where it is (and as it's turned) while you pose the rest: drag the hips, bend the "
        "back, turn the shoulders, and the arm or leg bends to keep it there (as far as it reaches). Drag it, its elbow or "
        "knee, or a toe yourself to move it: the pin goes with it, and so it does when something else moves it (Undo, a "
        "reset, the keyframes' time). Pins hold while you pose, not while keyframes or a clip play (keyframe the pose as "
        "you want it). A magenta square shows it, with the bones.");
    }
  }

  // A bone with keyframes before and after the playhead (2026-11-16): set it between them (a breakdown: 0 % its
  // keyframe before, 100 % the one after, past them to overshoot); auto key keyframes it at the playhead
  void DrawBetweenKeys(Held& h, int bone, bool& slidersActive)
  {
    pose_keys::Channel const* channel = h.Keys.On ? pose_keys::Find(h.Keys, bone) : nullptr;
    if (!channel)
      return;
    int before = -1, after = -1;
    for (size_t k = 0; k < channel->Keys.size(); ++k)
    {
      double t = channel->Keys[k].Time;
      if (t < h.Keys.Time - pose_keys::TimeEpsilon)
        before = (int)k;
      else if (t > h.Keys.Time + pose_keys::TimeEpsilon && after < 0)
        after = (int)k;
    }
    if (before < 0 || after < 0)
      return;
    static float between = 50.f;
    static int betweenBone = -1;
    static double betweenTime = -1.0;
    if (betweenBone != bone || betweenTime != h.Keys.Time)
    {
      between = 50.f;
      betweenBone = bone;
      betweenTime = h.Keys.Time;
    }
    float was = between;
    ImGui::SliderFloat("Between its keyframes##PoseBetween", &between, -25.f, 125.f, "%.0f %%");
    slidersActive |= ImGui::IsItemActive();
    if (between != was && std::isfinite(between))
    {
      pose_keys::Key const& a = channel->Keys[(size_t)before];
      pose_keys::Key const& b = channel->Keys[(size_t)after];
      float s = between / 100.f;
      SliderHold(h, bone, [&]()
      {
        QSlerp(a.Turn, b.Turn, s, h.Turn[bone]);
        for (int k = 0; k < 3; ++k)
          h.Move[bone][k] = a.Move[k] + (b.Move[k] - a.Move[k]) * s;
        ClampMove(h, bone);
      });
    }
    ImGui::SameLine();
    char help[400];
    _snprintf_s(help, sizeof(help), _TRUNCATE, "Sets this bone between its keyframe before the playhead (0 %%, at %.2f s) "
      "and its one after (100 %%, at %.2f s): 50 %% is halfway, past either end overshoots. With Auto key on it's "
      "keyframed at the playhead.", channel->Keys[(size_t)before].Time, channel->Keys[(size_t)after].Time);
    ImGui::HelpMarker(help);
  }

  void DrawBoneTab(Held& h, Snapshot const& before, bool& slidersActive)
  {
    for (int i = 0; i < h.WeaponCount; ++i)
    {
      HeldWeapon const& w = h.Weapons[i];
      char row[200];
      char where[64];
      if (w.Bone == WorldFrame)
        _snprintf_s(where, sizeof(where), _TRUNCATE, "put down");
      else if (w.Bone >= 0)
        _snprintf_s(where, sizeof(where), _TRUNCATE, "with %s", BoneName(h, w.Bone));
      else
        _snprintf_s(where, sizeof(where), _TRUNCATE, "%s", w.Gone ? "the game's (dropped or gone)" :
          "the game's (not on a bone when taken)");
      char const* state = w.Bone == -1 ? "" : (!g_WeaponsFollow ? ", where the game has it (Weapons go with the hands is off)" :
        (w.Drawing ? ", drawn by the tools" : (w.Holding ? ", its entity moved (its body not drawn by the tools)" : "")));
      _snprintf_s(row, sizeof(row), _TRUNCATE, "Its %s: %s%s%s##weapon%d", w.Name, where, state,
        i == g_SelectedWeapon ? "  (picked)" : "", i);
      if (ImGui::Selectable(row, g_SelectedWeapon == i))
      {
        g_SelectedWeapon = i;
        g_SelectedBone = -1;
        g_SelectedGroup = -1;
      }
    }

    ImGui::PushID(g_SelectedGroup >= 0 ? 10000 + g_SelectedGroup : (g_SelectedWeapon >= 0 ? 20000 + g_SelectedWeapon :
      g_SelectedBone));
    bool playing = PlayingKeys(h);
    if (g_SelectedGroup >= 0 && g_SelectedGroup < FaceGroupCount && h.GroupSize[g_SelectedGroup] > 0)
    {
      int g = g_SelectedGroup, first = h.GroupMembers[g][0];
      bool groupHasKeys = false;
      for (int k = 0; k < h.GroupSize[g]; ++k)
        if (pose_keys::Find(h.Keys, h.GroupMembers[g][k]) != nullptr)
          groupHasKeys = true;
      ImGui::Text("%s (%d bones)", FaceGroupNames[g], h.GroupSize[g]);
      if (playing && groupHasKeys)
        ImGui::TextDisabled("The keyframes are playing: pause them to pose.");
      else
      {
        float angles[3];
        QToAngles(h.Turn[first], angles);
        bool changed = false;
        changed |= ImGui::SliderFloat("Turn X##PoseGroupX", &angles[0], -180.f, 180.f, "%.1f deg");
        slidersActive |= ImGui::IsItemActive();
        changed |= ImGui::SliderFloat("Turn Y##PoseGroupY", &angles[1], -180.f, 180.f, "%.1f deg");
        slidersActive |= ImGui::IsItemActive();
        changed |= ImGui::SliderFloat("Turn Z##PoseGroupZ", &angles[2], -180.f, 180.f, "%.1f deg");
        slidersActive |= ImGui::IsItemActive();
        ImGui::SameLine();
        ImGui::HelpMarker("Each bone of the group gets this turn about its parent bone's X, Y and Z, about its own place "
          "(the values shown are its first bone's).");
        if (changed)
        {
          float q[4];
          QFromAngles(angles, q);
          for (int k = 0; k < h.GroupSize[g]; ++k)
            memcpy(h.Turn[h.GroupMembers[g][k]], q, sizeof(q));
        }
        float move[3] = { h.Move[first][0], h.Move[first][1], h.Move[first][2] };
        if (ImGui::SliderFloat3("Move##PoseGroupMove", move, -MaxFaceMove, MaxFaceMove, "%.3f m"))
          for (int k = 0; k < h.GroupSize[g]; ++k)
          {
            int m = h.GroupMembers[g][k];
            memcpy(h.Move[m], move, sizeof(move));
            ClampMove(h, m);
          }
        slidersActive |= ImGui::IsItemActive();
        ImGui::SameLine();
        ImGui::HelpMarker("Moves each bone of the group the same way, in its parent bone's frame (the face's; the jaw's "
          "for the lower lip and the low cheeks).");
      }
      if (ImGui::Button("Reset this group"))
      {
        bool which[MaxBones] = {};
        for (int k = 0; k < h.GroupSize[g]; ++k)
        {
          int m = h.GroupMembers[g][k];
          bool below[MaxBones] = {};
          for (int b = m; b < h.Bones; ++b)
          {
            below[b] = b == m || (h.Parents[b] >= m && below[h.Parents[b]]);
            if (below[b])
              which[b] = true;
          }
        }
        RequestReset(h, which, false, before, "this group");
      }
    }
    else if (g_SelectedBone >= 0 && g_SelectedBone < h.Bones && h.InHeld[g_SelectedBone])
    {
      int i = g_SelectedBone;
      bool boneHasKeys = pose_keys::Find(h.Keys, i) != nullptr;
      ImGui::Text("%s", BoneName(h, i));
      if (playing && boneHasKeys)
        ImGui::TextDisabled("The keyframes are playing: pause them to pose.");
      else
      {
        if (playing && g_AutoKey)
        {
          ImGui::PushTextWrapPos(0.f);
          if (h.KeysTimeMoved)
            ImGui::TextDisabled("Playing: a change here isn't keyframed, and it stays at every time.");
          else if (h.Keys.Clock == pose_keys::Clock_Track)
            ImGui::TextDisabled("The track plays, but the keyframes' time is held (at their start or end, Loop off): "
              "Auto key keyframes a change here once the track stops, unless the time moves first.");
          else
            ImGui::TextDisabled("Playing, but the keyframes' time is held (time frozen, the clip holding it, or the "
              "keyframes at their start or end with Loop off): Auto key keyframes a change here when you pause, unless "
              "the time moves first.");
          ImGui::PopTextWrapPos();
        }
        float angles[3];
        QToAngles(h.Turn[i], angles);
        float shown[3] = { angles[0], angles[1], angles[2] };
        bool changed = false;
        changed |= ImGui::SliderFloat("Turn X##PoseX", &shown[0], -180.f, 180.f, "%.1f deg");
        slidersActive |= ImGui::IsItemActive();
        changed |= ImGui::SliderFloat("Turn Y##PoseY", &shown[1], -180.f, 180.f, "%.1f deg");
        slidersActive |= ImGui::IsItemActive();
        changed |= ImGui::SliderFloat("Turn Z##PoseZ", &shown[2], -180.f, 180.f, "%.1f deg");
        slidersActive |= ImGui::IsItemActive();
        ImGui::SameLine();
        ImGui::HelpMarker("The bone's turn from the pose it was taken in, about its parent bone's X, Y and Z (in that "
          "order). The rings on the picture turn it too (see Rings on the bone's own axes).");
        if (changed)
          SliderHold(h, i, [&]() { QFromAngles(shown, h.Turn[i]); });   // (pinned hands and feet, planted feet kept)
        bool moved = h.Move[i][0] != 0.f || h.Move[i][1] != 0.f || h.Move[i][2] != 0.f;
        if (i == h.Root || h.Face[i] || g_FreeMove || moved)
        {
          // (the root's is a drag field: a click alone rounds it to what it shows, ImGui 1.61, so only a drag or a
          // typed number is applied, as the Time field does)
          float moveShown[3] = { h.Move[i][0], h.Move[i][1], h.Move[i][2] };
          bool typing = TypingInField();
          bool changedMove = i == h.Root ? ImGui::DragFloat3("Move##PoseMove", moveShown, 0.01f, 0.f, 0.f, "%.3f m") :
            ImGui::SliderFloat3("Move##PoseMove", moveShown, -MoveLimit(h, i), MoveLimit(h, i), "%.3f m");
          if (changedMove && (i != h.Root || DragTakes(typing)))
            SliderHold(h, i, [&]()
            {
              memcpy(h.Move[i], moveShown, sizeof(moveShown));
              ClampMove(h, i);
            }, false);   // (the Hips' Move moves the whole body: the feet go with it; pins hold)
          slidersActive |= ImGui::IsItemActive();
          ImGui::SameLine();
          ImGui::HelpMarker(i == h.Root ? "Moves the whole body from where the character is, in the frame of the bone "
            "under it (AITrajectory: where the game has the character, turned its way), as far as you like (up to 100 km): "
            "drag a field sideways, or double-click it and type a number." : (h.Face[i] ? "Moves the face bone, in "
            "its parent bone's frame." : "Moves the bone (and every bone below it) from where it is, in its parent bone's "
            "frame. The mesh stretches between it and its parent."));
        }
        DrawLimbControls(h, i, slidersActive);
        DrawBetweenKeys(h, i, slidersActive);
      }
      if (ImGui::Button("Reset this bone"))
      {
        bool which[MaxBones] = {};
        which[i] = true;
        RequestReset(h, which, false, before, "this bone");
      }
      SameLineIfFits("Reset it and the bones below");
      if (ImGui::Button("Reset it and the bones below"))
      {
        bool below[MaxBones] = {};
        for (int b = i; b < h.Bones; ++b)
          below[b] = b == i || (h.Parents[b] >= 0 && h.Parents[b] >= i && below[h.Parents[b]]);
        RequestReset(h, below, false, before, "it and the bones below");
      }
    }
    else if (g_SelectedWeapon >= 0 && g_SelectedWeapon < h.WeaponCount)
    {
      HeldWeapon& w = h.Weapons[g_SelectedWeapon];
      ImGui::Text("Its %s", w.Name);
      if (w.Bone == -1)
      {
        ImGui::PushTextWrapPos(0.f);
        ImGui::TextDisabled(!w.Gone ? "It wasn't on one of its bones when the pose was taken, so the game has it." :
          "It isn't its own any more (dropped or gone): the game has it.");
        ImGui::PopTextWrapPos();
        float local[FloatsPerBone], world[FloatsPerBone];
        if (!w.Gone && ImGui::Button("Pose it where it is") && WeaponPlace(w, h.Entity, local, world))
        {
          PushUndo(h, before);
          w.Bone = WorldFrame;
          memcpy(w.OffsetLocal, local, sizeof(w.OffsetLocal));
          memcpy(w.OffsetWorld, world, sizeof(w.OffsetWorld));
          w.OffsetLocal[3] = w.OffsetWorld[3] = 1.f;
          w.EditMove[0] = w.EditMove[1] = w.EditMove[2] = 0.f;
          QIdentity(w.EditTurn);
        }
      }
      else
      {
        int attachTo = w.TakenBone >= 0 ? w.TakenBone : (h.GripBone >= 0 ? h.GripBone : h.Root);
        bool attached = w.Bone >= 0;
        char label[96];
        _snprintf_s(label, sizeof(label), _TRUNCATE, "Goes with %s##PoseWeaponWith", BoneName(h, attached ? w.Bone : attachTo));
        if (ImGui::Checkbox(label, &attached) && h.WorldOk)
        {
          PushUndo(h, before);
          MoveWeaponTo(h, w, attached ? attachTo : WorldFrame);
        }
        ImGui::SameLine();
        ImGui::HelpMarker("Ticked, it goes with that bone when you pose it (where it is on the bone stays). Unticked, "
          "it's put down: it stays where it is in the world however the body moves. Either way, drag its square on the "
          "picture to move it, turn it with its rings, or use the sliders.");
        float angles[3];
        QToAngles(w.EditTurn, angles);
        bool changed = false;
        changed |= ImGui::SliderFloat("Turn X##PoseWeaponX", &angles[0], -180.f, 180.f, "%.1f deg");
        slidersActive |= ImGui::IsItemActive();
        changed |= ImGui::SliderFloat("Turn Y##PoseWeaponY", &angles[1], -180.f, 180.f, "%.1f deg");
        slidersActive |= ImGui::IsItemActive();
        changed |= ImGui::SliderFloat("Turn Z##PoseWeaponZ", &angles[2], -180.f, 180.f, "%.1f deg");
        slidersActive |= ImGui::IsItemActive();
        ImGui::SameLine();
        ImGui::HelpMarker(w.Bone >= 0 ? "Its turn about its own place, about its bone's X, Y and Z (in that order)." :
          "Its turn about its own place, about the world's X, Y (up) and Z (in that order).");
        if (changed)
          QFromAngles(angles, w.EditTurn);
        float weaponMove[3] = { w.EditMove[0], w.EditMove[1], w.EditMove[2] };
        bool typing = TypingInField();
        if (ImGui::DragFloat3("Move##PoseWeaponMove", weaponMove, 0.005f, 0.f, 0.f, "%.3f m") &&
            DragTakes(typing))   // (a click alone only rounds it)
          for (int k = 0; k < 3; ++k)
            w.EditMove[k] = std::isfinite(weaponMove[k]) ? weaponMove[k] : 0.f;
        slidersActive |= ImGui::IsItemActive();
        ImGui::SameLine();
        ImGui::HelpMarker(w.Bone >= 0 ? "Its move from where it was on its bone, in the bone's frame. Drag a field "
          "sideways, or double-click it and type a number." : "Its move from where it was put down, in the world's "
          "frame (Y is up). Drag a field sideways, or double-click it and type a number.");
      }
      if (!w.Gone)
      {
        if (ImGui::Button("Reset this weapon"))
        {
          PushUndo(h, before);
          ResetWeapon(w);
        }
        ImGui::SameLine();
        ImGui::HelpMarker("Back as it was when the pose was taken: on the bone it was on (or the game's), not moved or "
          "turned.");
      }
    }
    else
      ImGui::TextDisabled("Pick a bone in the list or click its dot on the picture");
    ImGui::PopID();

    if (SelectedKeysAtPlayhead(h) > 0)
    {
      int bones[MaxBones];
      int count = SelectedBones(h, bones);
      pose_keys::Ease ease = pose_keys::Ease_Smooth;
      for (int k = 0; k < count; ++k)
      {
        pose_keys::Channel* channel = pose_keys::Find(h.Keys, bones[k]);
        int key = channel ? pose_keys::KeyAt(*channel, h.Keys.Time) : -1;
        if (key >= 0)
        {
          ease = (pose_keys::Ease)channel->Keys[key].Ease;
          break;
        }
      }
      const char* eases[pose_keys::Ease_Count];
      for (int e = 0; e < pose_keys::Ease_Count; ++e)
        eases[e] = KeyEaseLabel(e);
      int easeIndex = (int)ease;
      if (easeIndex < 0 || easeIndex >= pose_keys::Ease_Count)
        easeIndex = 0;
      ImGui::Text("Its keyframe at %.2f s goes to the next:", h.Keys.Time);
      if (ImGui::Combo("##PoseKeyEase", &easeIndex, eases, pose_keys::Ease_Count))
      {
        bool anyChange = false;
        for (int k = 0; k < count; ++k)
        {
          pose_keys::Channel* channel = pose_keys::Find(h.Keys, bones[k]);
          int key = channel ? pose_keys::KeyAt(*channel, h.Keys.Time) : -1;
          if (key >= 0 && channel->Keys[key].Ease != easeIndex)
          {
            anyChange = true;
            break;
          }
        }
        if (anyChange)
        {
          PushKeyUndo(h);
          for (int k = 0; k < count; ++k)
          {
            pose_keys::Channel* channel = pose_keys::Find(h.Keys, bones[k]);
            int key = channel ? pose_keys::KeyAt(*channel, h.Keys.Time) : -1;
            if (key >= 0)
              pose_keys::SetEase(h.Keys, bones[k], key, easeIndex);
          }
        }
      }
    }

    if (ImGui::Button("Reset all"))
    {
      bool which[MaxBones] = {};
      for (int b = 0; b < h.Bones; ++b)
        which[b] = true;
      RequestReset(h, which, true, before, "all");
    }
    DrawUndoButtons(h, "PoseBone");
  }

  void DrawClipTab(Held& h)
  {
    float fontSize = ImGui::GetFontSize();
    ULONGLONG now = GetTickCount64();
    ImGui::PushID("PoseClip");
    ImGui::Separator();
    ImGui::Text("Animation clip");
    ImGui::SameLine();
    ImGui::HelpMarker("Plays an animation of a human rig on the held character: Mixamo's or any other's (its bones are "
      "found by their names and where they hang). Put its .fbx file in the game folder's Cinematic Tools\\Animations "
      "(from Mixamo: FBX Binary, 30 frames per second, keyframe reduction none, With Skin; from Blender or another tool: "
      "with its mesh, so its rest pose is in the file. Without one (an armature-only export) the rest is worked out "
      "from the motion and the head, thumbs, feet and twists can be off, the more so the further the clip stays from "
      "standing; the clip's check line says so). Its pose takes the place of the pose taken, and your edits stay on top "
      "of it. Works on Faith, KrugerSec and people with a full skeleton. The elbow and knee rolls follow the forearm "
      "and shin (and Faith's twist bones her own way of twisting); the face and other helper bones keep the pose taken's "
      "turn, and the feet don't stick to the ground. The clip faces the way the character faced when it was put on; turn "
      "the Hips to turn it. Bones can't be dragged while it plays: pause it first. When a clip jumps (its loop, a new "
      "time, another clip), Faith's hair is reset to her head (the game's own cloth reset) rather than trailing after "
      "it.\n\nUnity humanoid clips (.anim, Unity's binary asset; text .anim files aren't read yet) play too: they hold "
      "Unity's muscles, not bones, and Unity's own numbers for turning those into bones aren't in the file, so the tools "
      "use an approximate model of them (fitted to clips' own hand and foot goals): the hands and feet are put on those "
      "goals on the tools' reference body, the spine, head and fingers are the model's (no goal to check them against). "
      "The character's arms and legs are proportioned differently, so on it hand-to-hand or hand-to-face contacts can be "
      "several cm off (and the feet don't stick to the ground, as above). The eyes, jaw, props and other objects in the "
      "clip aren't played.");
    if (now - g_ClipFilesListedAt > 2000)
    {
      ListClipFiles();
      g_ClipFilesListedAt = now;
    }
    ClipPlay& clip = h.Clip;
    if (g_ClipFiles.empty())
      ImGui::TextDisabled("No .fbx or .anim files in Cinematic Tools\\Animations (in the game folder)");
    else
    {
      ImGui::BeginChild("##ClipFiles", ImVec2(0.f, fontSize * 4.f), true);
      for (size_t f = 0; f < g_ClipFiles.size(); ++f)
      {
        std::string name = Utf8Of(g_ClipFiles[f]);
        bool current = clip.Clip && clip.Clip->File == name;
        ImGui::PushID((int)f);
        if (ImGui::Selectable(name.c_str(), current) && !g_ClipLoad)
          StartClipLoad(h, g_ClipFiles[f]);
        ImGui::PopID();
      }
      ImGui::EndChild();
    }
    ImGui::PushTextWrapPos(0.f);
    if (g_ClipMessage[0])
      ImGui::TextDisabled("%s", g_ClipMessage);
    if (clip.Clip)
    {
      // A short line about the loaded clip (2026-11-16, the user: "a small paragraph about the loaded anim"); the
      // reading's and the fit's full reports are in CT.log ("Pose: clip read", "Pose: clip ... on") and on hover
      pose_clips::Clip const& c = *clip.Clip;
      if (clip.Fit.Ok)
        ImGui::TextDisabled("\"%s\" (%s): %.2f s, %d frames at %.0f fps%s. %d of its bones turn %s's.", c.Take.c_str(),
          c.File.c_str(), c.Length(), c.Frames, c.Fps, c.FromUnity ? ", a Unity clip" : "", clip.Fit.Driven, h.Label);
      else
        ImGui::TextDisabled("\"%s\" (%s): %.2f s, %d frames at %.0f fps%s. It can't play on %s: %s.", c.Take.c_str(),
          c.File.c_str(), c.Length(), c.Frames, c.Fps, c.FromUnity ? ", a Unity clip" : "", h.Label, clip.Fit.Report.c_str());
      std::string details = c.Report + "\n\n" + clip.Fit.Report;
      ImGui::ItemTooltip(details.c_str());
      if (!c.Warning.empty())
        ImGui::TextColored(ImVec4(1.f, 0.75f, 0.3f, 1.f), "Check: %s", c.Warning.c_str());
    }
    ImGui::PopTextWrapPos();
    if (clip.Clip && !clip.Clip->FromUnity)
    {
      if (ImGui::Button("Save its bone map to edit"))
      {
        std::string message;
        pose_clips::SaveBoneMap(*clip.Clip, message);
        _snprintf_s(g_ClipMessage, sizeof(g_ClipMessage), _TRUNCATE, "%s", message.c_str());
      }
      ImGui::SameLine();
      ImGui::HelpMarker("The tools find which of a clip's bones plays each part (hips, spine, arms, hands, fingers, "
        "legs, feet) by their names and where they hang, in any human rig. If one is wrong, this writes <the clip's "
        "name>.bones.txt beside it: change the bone named for that part, then pick the clip again. While that file is "
        "there it's used instead; delete it to go back. It also holds the file's up axis and unit as read (commented "
        "out, unless they came from it already): if the clip plays lying, upside down or the wrong size, remove the # "
        "before \"Up =\" or \"Unit =\" and set them.");
      if (ImGui::TreeNode("Its bones"))
      {
        for (size_t k = 0; k < clip.Clip->Bones.size(); ++k)
          ImGui::TextDisabled("%s = %s", clip.Clip->Bones[k].c_str(), clip.Clip->SourceNames[k].c_str());
        ImGui::TreePop();
      }
    }
    if (clip.Clip && clip.Fit.Ok)
    {
      ImGui::Checkbox("Use the clip", &clip.On);
      SameLineIfFits(clip.Playing ? "Pause" : "Play");
      if (ImGui::Button(clip.Playing ? "Pause" : "Play"))
      {
        clip.Playing = !clip.Playing;
        if (clip.Playing)
          clip.On = true;
        if (clip.Playing && clip.Clock != ClipTrackTime && !clip.Loop && clip.Time >= clip.Clip->Length())
          clip.Time = 0.0;
      }
      double trackAt = g_ClipTrackTime >= 0.0 ? g_ClipTrackTime * clip.Speed : 0.0;
      if (clip.Clock != ClipTrackTime)
      {
        SameLineIfFits("From the start");
        if (ImGui::Button("From the start"))
        {
          clip.Time = 0.0;
          clip.TravelShift[0] = clip.TravelShift[1] = 0.f;
        }
      }
      SameLineIfFits("Loop", true);
      ImGui::Checkbox("Loop", &clip.Loop);
      float length = (float)clip.Clip->Length();
      bool track = clip.Clock == ClipTrackTime;
      float shown = (float)(track ? clip.TrackStart : clip.Time);
      if (ImGui::SliderFloat(track ? "Start (s)" : "Time (s)", &shown, 0.f, length > 0.f ? length : 0.01f, "%.2f"))
      {
        if (track)
        {
          clip.TrackStart = shown;
          clip.Time = shown + trackAt;
        }
        else
          clip.Time = shown;
        clip.On = true;
      }
      ImGui::SliderFloat("Speed", &clip.Speed, 0.05f, 4.f, "%.2fx");
      const char* clocks[] = { "Game time (Game speed; stops with Freeze time)", "Real time (plays with time frozen too)",
        "The camera track's time" };
      ImGui::Combo("Clock", &clip.Clock, clocks, 3);
      const char* hips[] = { "Stay in place (only up and down)", "Follow the clip's travel" };
      ImGui::Combo("Hips", &clip.Hips, hips, 2);
      if (track)
      {
        ImGui::PushTextWrapPos(0.f);
        ImGui::TextDisabled("The clip plays from Start as the camera track plays (a shot sequence's time goes on "
          "through its cuts), and waits at Start while no track plays.");
        ImGui::PopTextWrapPos();
      }
      if (ImGui::Button("Use this frame as the pose"))
        BakeClipFrame(h);
      ImGui::HelpMarker("The clip's pose now becomes the held pose (as if taken so): the clip goes off, your edits "
        "stay on it, and Reset goes back to it.");
      SameLineIfFits("Remove the clip");
      if (ImGui::Button("Remove the clip"))
      {
        clip.Clip.reset();
        clip.On = clip.Playing = clip.LocalOk = false;
      }
    }
    ImGui::PopID();
  }

  void VoiceStretchTooltip(VoicePlay const& v, size_t i, bool own);

  // The Voice tab's picture of the file: where the mouth opens over its length, the time now as a line; held with
  // the mouse (its own clock only) it sets the time. Drawn by hand on an invisible button.
  void DrawVoicePicture(VoicePlay& v, bool own, ULONGLONG now)
  {
    float fontSize = ImGui::GetFontSize();
    float length = (float)v.Sound->Seconds;
    ImVec2 size(ImGui::GetContentRegionAvailWidth(), fontSize * 2.5f);
    if (size.x < fontSize * 4.f)
      size.x = fontSize * 4.f;
    ImVec2 from = ImGui::GetCursorScreenPos();
    ImVec2 to(from.x + size.x, from.y + size.y);
    ImGui::InvisibleButton("##VoicePicture", size);
    ReportItemForTests("voice picture");
    if (own && ImGui::IsItemActive() && length > 0.f)
    {
      float share = (ImGui::GetIO().MousePos.x - from.x) / size.x;
      if (!(share > 0.f))
        share = 0.f;
      if (share > 1.f)
        share = 1.f;
      v.Time = (double)share * (double)length;
      v.SetByHandAt = now;
    }
    // [STRIP] The voice's feeling stretch by stretch, a strip along the picture's top (when its reading is there)
    size_t stretches = v.EmotionStretches.size();
    bool strip = v.FeelAuto && v.FeelRig.Ok && v.EmotionReadings && v.EmotionReadings->size() == stretches &&
      stretches > 0 && length > 0.f;
    float stripHeight = fontSize * 0.3f;
    int under = -1;
    if (strip && ImGui::IsItemHovered() && ImGui::GetIO().MousePos.y < from.y + stripHeight + 1.f)
    {
      double t = (double)((ImGui::GetIO().MousePos.x - from.x) / size.x) * (double)length;
      for (size_t i = 0; i < stretches && under < 0; ++i)
        if (t >= v.EmotionStretches[i].From && t <= v.EmotionStretches[i].To)
          under = (int)i;
    }
    if (under >= 0)
      VoiceStretchTooltip(v, (size_t)under, own);
    else if (ImGui::IsItemHovered())
      ImGui::SetTooltip("%s%s", own ? "Where the mouth opens over the file. Hold and drag: its time." :
        "Where the mouth opens over the file, and where it is now.", strip ? "\nThe strip along its top: the voice's "
        "feeling, stretch by stretch (the mouse on one tells what it shows and why)." : "");
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(from, to, ImGui::GetColorU32(ImGuiCol_FrameBg));
    // (at the Mouth smoothing the face follows: voice::MouthAt and voice::OverviewOf choose alike)
    std::vector<float> const* shown = &v.Sound->Overview;
    if (v.Smoothing != 1.f && v.Smoothing == v.Smoothing)
    {
      if (v.OverviewSound.lock() != v.Sound || v.OverviewSmoothing != v.Smoothing || v.Overview.empty())
      {
        voice::OverviewOf(*v.Sound, v.Smoothing, v.Overview);
        v.OverviewSmoothing = v.Smoothing;
        v.OverviewSound = v.Sound;
      }
      shown = &v.Overview;
    }
    std::vector<float> const& overview = *shown;
    int points = (int)overview.size();
    ImU32 bar = ImGui::GetColorU32(ImGuiCol_PlotHistogram);
    for (int p = 0; p < points; ++p)
    {
      float value = overview[p];
      if (!(value > 0.f))
        continue;
      if (value > 1.f)
        value = 1.f;
      float x0 = from.x + size.x * (float)p / (float)points;
      float x1 = from.x + size.x * (float)(p + 1) / (float)points;
      draw->AddRectFilled(ImVec2(x0, to.y - value * size.y), ImVec2(x1, to.y), bar);
    }
    // [STRIP] Over the bars: each stretch's feeling, stronger the more of the face it has; the feeling picked faint;
    // a stretch set by hand outlined, the one shown now outlined brighter
    if (strip)
      for (size_t i = 0; i < stretches; ++i)
      {
        voice_emotion::Stretch const& st = v.EmotionStretches[i];
        voice_emotion::Look look = voice_emotion::LookOf(&(*v.EmotionReadings)[i], i < v.FeelSetting.size() ?
          v.FeelSetting[i] : voice_emotion::FromVoice, v.FeelWeights, v.FeelSure);
        float x0 = from.x + size.x * (float)(st.From / (double)length);
        float x1 = from.x + size.x * (float)(st.To / (double)length);
        if (x1 < x0 + 1.f)
          x1 = x0 + 1.f;
        ImVec2 a(x0, from.y), b(x1, from.y + stripHeight);
        bool feeling = look.Shown >= 0 && look.Share > 0.f;
        draw->AddRectFilled(a, b, feeling ? ImGui::GetColorU32(ImGuiCol_CheckMark, 0.25f + 0.75f * look.Share) :
          ImGui::GetColorU32(ImGuiCol_TextDisabled, 0.35f));
        if ((int)i == v.FeelStretchNow)
          draw->AddRect(a, b, ImGui::GetColorU32(ImGuiCol_Text));
        else if (look.ByHand)
          draw->AddRect(a, b, ImGui::GetColorU32(ImGuiCol_Text, 0.6f));
      }
    if (length > 0.f && v.Time >= 0.0 && v.Time <= (double)length)
    {
      float x = from.x + size.x * (float)(v.Time / (double)length);
      draw->AddLine(ImVec2(x, from.y), ImVec2(x, to.y), ImGui::GetColorU32(ImGuiCol_Text), 1.f);
    }
  }

  // [EMOTION] "From the voice": the tick, what its reading is doing, how sure the voice must be
  void DrawVoiceEmotion(VoicePlay& v)
  {
    ImGui::Checkbox("From the voice", &v.FeelAuto);
    ImGui::SameLine();
    ImGui::HelpMarker(VoiceEmotionHelp);
    ImVec4 const warn(1.f, 0.75f, 0.3f, 1.f);
    voice_emotion::Status s;
    if (v.Emotion)
      s = voice_emotion::StatusOf(*v.Emotion);
    bool going = v.Emotion && (s.Now == voice_emotion::Waiting || s.Now == voice_emotion::Running);
    // [STOP] A reading stopped (its helper ended at once): From the voice goes off, and ticked again the voice is read
    // anew. On its own line (a wrapped text has no end to sit beside).
    auto stop = [&]()
    {
      bool pressed = ImGui::SmallButton("Stop##EmotionStop");
      ReportItemForTests("emotion stop");
      if (pressed)
      {
        voice_emotion::Cancel(v.Emotion);
        v.Emotion.reset();
        v.EmotionReadings.reset();
        v.FeelAuto = false;
      }
      // ([REVIEW] by what it is doing: a reading still waiting has no helper of its own, another voice's goes on)
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s%s Ticked again, the voice is read anew from the start.", s.Now == voice_emotion::Running ?
          "Stops reading this voice's feeling: its EmotionHelper.exe is ended now." : "Takes this voice out of the wait: "
          "nothing of it has been heard yet, and another voice's reading goes on.", v.FeelAuto ? " From the voice goes off."
          : "");
    };
    if (!v.FeelAuto)
    {
      // [STOP] A reading still going while unticked goes on (kept for when it's ticked again): said, and stoppable
      if (going)
      {
        ImGui::PushTextWrapPos(0.f);
        ImGui::TextDisabled("Still reading this voice's feeling (%d of %d stretches): kept for when From the voice is "
          "ticked again.", s.Done, s.Total);
        ImGui::PopTextWrapPos();
        stop();
      }
      return;
    }
    ImGui::PushTextWrapPos(0.f);
    std::string why;
    if (v.EmotionStretches.empty())
    {
      // [NO PHRASES] Its phrases were never looked for (the analysis runs at 8,000 - 192,000 Hz, Voice.cpp) or it failed:
      // not "no speech"
      if (v.Sound && v.Sound->LifeTrack.Frames.empty())
      {
        if (v.Sound->Rate < 8000 || v.Sound->Rate > 192000)
          ImGui::TextColored(warn, "Its phrases couldn't be looked for, so its feeling can't be read: the sound is %d Hz, "
            "and phrases are found at 8,000 - 192,000 Hz (save it again at such a rate). The feeling picked shows.",
            v.Sound->Rate);
        else
          ImGui::TextColored(warn, "Its phrases couldn't be found, so its feeling can't be read: the feeling picked "
            "shows. (%s.)", v.Sound->LifeNote.empty() ? "The analysis of its loudness and pitch failed" :
            v.Sound->LifeNote.c_str());
      }
      else
        ImGui::TextColored(warn, "No speech was found in this sound: there is no feeling to read in it (the feeling "
          "picked shows).");
    }
    else if (!v.Emotion && !voice_emotion::Available(&why))
      ImGui::TextColored(warn, "%s. The feeling picked shows meanwhile.", why.c_str());
    else if (!v.Emotion || s.Now == voice_emotion::Waiting || s.Now == voice_emotion::Cancelled)
    {
      ImGui::TextDisabled("Waiting to read the voice's feeling (one voice at a time)...");
      if (going)
        stop();
    }
    else if (s.Now == voice_emotion::Running)
    {
      // [PROGRESS] The model starting (before its first stretch: it reads the job, loads, checks itself), then stretch
      // by stretch, and the seconds so far
      if (!s.ModelReady)
        ImGui::TextDisabled("Reading the voice's feeling with EmotionHelper.exe, outside the game at idle priority (about "
          "1.7 GB of memory meanwhile): starting the model, %.0f s so far. The feeling picked shows until it's done.",
          s.Elapsed);
      else
        ImGui::TextDisabled("Reading the voice's feeling with EmotionHelper.exe, outside the game at idle priority (about "
          "1.7 GB of memory meanwhile): %d of %d stretches, %.0f s so far. The feeling picked shows until it's done.",
          s.Done, s.Total, s.Elapsed);
      stop();
    }
    else if (s.Now == voice_emotion::Failed)
    {
      ImGui::TextColored(warn, "The voice's feeling couldn't be read: %s.", s.Message.c_str());
      if (s.CanRetry)
      {
        bool again = ImGui::Button("Try again");
        ReportItemForTests("emotion try again");
        if (again)
        {
          v.Emotion.reset();
          v.EmotionReadings.reset();
        }
      }
    }
    else if (v.EmotionReadings && v.EmotionReadings->size() == v.EmotionStretches.size())
    {
      // [WHY] What the stretches show and why: the voice's feeling on half the face or more, on less, or the feeling
      // picked (not sure enough, heard as neutral, too short to hear); the stretches set by hand apart
      int shown[voice_feel::FeelingCount] = {}, count[6] = {};
      for (size_t i = 0; i < v.EmotionStretches.size(); ++i)
      {
        voice_emotion::Reading const& r = (*v.EmotionReadings)[i];
        voice_emotion::Look look = voice_emotion::LookOf(&r, i < v.FeelSetting.size() ? v.FeelSetting[i] :
          voice_emotion::FromVoice, v.FeelWeights, v.FeelSure);
        voice_emotion::Why w = voice_emotion::WhyOf(&r, look);
        ++count[w];
        if (w == voice_emotion::WhyShown && look.Shown >= 0 && look.Shown < voice_feel::FeelingCount)
          ++shown[look.Shown];
      }
      std::string line;
      char part[96];
      for (int f = 0; f < voice_feel::FeelingCount; ++f)
        if (shown[f] > 0)
        {
          _snprintf_s(part, sizeof(part), _TRUNCATE, "%s%s %d", line.empty() ? "" : ", ", voice_feel::Presets[f].Name,
            shown[f]);
          line += part;
        }
      if (!line.empty())
        line += " (on half the face or more)";
      if (count[voice_emotion::WhyPartly] > 0)
      {
        _snprintf_s(part, sizeof(part), _TRUNCATE, "%spartly %d", line.empty() ? "" : ", ",
          count[voice_emotion::WhyPartly]);
        line += part;
      }
      int kept = count[voice_emotion::WhyNotSure] + count[voice_emotion::WhyNeutral] + count[voice_emotion::WhyTooShort];
      if (kept > 0)
      {
        _snprintf_s(part, sizeof(part), _TRUNCATE, "%s%s %d (", line.empty() ? "" : ", ", v.Feel >= 0 ?
          "the feeling picked" : "no feeling (none picked)", kept);
        line += part;
        bool first = true;
        int const reasons[3] = { voice_emotion::WhyNotSure, voice_emotion::WhyNeutral, voice_emotion::WhyTooShort };
        char const* const names[3] = { "not sure enough", "heard as neutral", "too short to hear" };
        for (int k = 0; k < 3; ++k)
          if (count[reasons[k]] > 0)
          {
            _snprintf_s(part, sizeof(part), _TRUNCATE, "%s%s %d", first ? "" : ", ", names[k], count[reasons[k]]);
            line += part;
            first = false;
          }
        line += ")";
      }
      if (count[voice_emotion::WhyByHand] > 0)
      {
        _snprintf_s(part, sizeof(part), _TRUNCATE, "%sset by hand %d", line.empty() ? "" : ", ",
          count[voice_emotion::WhyByHand]);
        line += part;
      }
      ImGui::TextDisabled("The voice's feeling, %d stretches%s: %s", (int)v.EmotionStretches.size(),
        s.FromCache ? " (kept from an earlier reading)" : "", line.c_str());
    }
    ImGui::PopTextWrapPos();
    if (v.EmotionReadings)
    {
      ImGui::SliderFloat("Sureness needed", &v.FeelSure, voice_emotion::SureLeast, voice_emotion::SureMost, "%.2f");
      ImGui::SameLine();
      ImGui::HelpMarker(VoiceSureHelp);
    }
  }

  // [WHY] A stretch's whole story, for its row's tooltip and the voice picture's: when, what it is heard as (and with
  // which stretches together), all 8 chances, the model's own three numbers, and why the face shows what it shows
  void VoiceStretchTooltip(VoicePlay const& v, size_t i, bool own)
  {
    size_t n = v.EmotionStretches.size();
    if (i >= n || !v.EmotionReadings || v.EmotionReadings->size() != n)
      return;
    voice_emotion::Stretch const& st = v.EmotionStretches[i];
    voice_emotion::Reading const& r = (*v.EmotionReadings)[i];
    int setting = i < v.FeelSetting.size() ? v.FeelSetting[i] : voice_emotion::FromVoice;
    voice_emotion::Look look = voice_emotion::LookOf(&r, setting, v.FeelWeights, v.FeelSure);
    voice_emotion::Look heard = voice_emotion::LookOf(&r, voice_emotion::FromVoice, v.FeelWeights, v.FeelSure);
    voice_emotion::Why w = voice_emotion::WhyOf(&r, look);
    ImGui::BeginTooltip();
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 34.f);
    // When, and what was heard for it
    size_t first = i, last = i;
    while (first > 0 && v.EmotionStretches[first - 1].HeardFrom == st.HeardFrom &&
      v.EmotionStretches[first - 1].HeardTo == st.HeardTo)
      --first;
    while (last + 1 < n && v.EmotionStretches[last + 1].HeardFrom == st.HeardFrom &&
      v.EmotionStretches[last + 1].HeardTo == st.HeardTo)
      ++last;
    std::string together;
    if (last > first)
    {
      // (two: the other one; more: all of them, this one among them)
      char t[96];
      if (last - first == 1)
        _snprintf_s(t, sizeof(t), _TRUNCATE, "; heard together with stretch %d (one reading)", (int)(first == i ? last :
          first) + 1);
      else
        _snprintf_s(t, sizeof(t), _TRUNCATE, "; heard together, stretches %d - %d (one reading)", (int)first + 1,
          (int)last + 1);
      together = t;
    }
    // ([REVIEW] a phrase of 3 s or more is heard as itself, its window's edges an ulp or so off its own)
    bool aroundIt = fabs(st.HeardFrom - st.From) > 1e-6 || fabs(st.HeardTo - st.To) > 1e-6;
    if (own)
      ImGui::Text("Stretch %d of %d: %.2f - %.2f s", (int)i + 1, (int)n, st.From, st.To);
    else
      ImGui::Text("Stretch %d of %d: %.2f - %.2f s into the voice (from %.2f s on the %s time)", (int)i + 1, (int)n,
        st.From, st.To, st.From + v.Start, v.Clock == VoiceKeysTime ? "keyframes'" : "camera track's");
    if (aroundIt || !together.empty())
      ImGui::Text("Heard: %.2f - %.2f s%s%s", st.HeardFrom, st.HeardTo, aroundIt ? " (with the voice around it)" : "",
        together.c_str());
    if (!r.Ok)
      ImGui::Text("Too short to hear (under 0.1 s): no reading.");
    else
    {
      int order[voice_emotion::ClassCount];
      voice_emotion::ChancesInOrder(r, order);
      std::string chances;
      char t[48];
      for (int c = 0; c < voice_emotion::ClassCount; ++c)
      {
        _snprintf_s(t, sizeof(t), _TRUNCATE, "%s%s %.1f %%", c ? ", " : "", voice_emotion::ClassNames[order[c]],
          100.f * r.Chance[order[c]]);
        chances += t;
      }
      ImGui::TextWrapped("The model's chances: %s", chances.c_str());
      ImGui::Text("Arousal %.2f, dominance %.2f, valence %.2f (the model's own, about 0 - 1)", r.Arousal, r.Dominance,
        r.Valence);
    }
    float sure = v.FeelSure, top1 = sure + voice_emotion::SureSpan < 1.f ? sure + voice_emotion::SureSpan : 1.f;
    float topChance = 0.f;
    int top = 0;
    for (int c = 1; r.Ok && c < voice_emotion::ClassCount; ++c)
      if (r.Chance[c] > r.Chance[top])
        top = c;
    if (r.Ok)
      topChance = r.Chance[top];
    char const* picked = v.Feel >= 0 ? "the feeling picked" : "no feeling (none picked)";
    switch (w)
    {
    case voice_emotion::WhyShown:
    case voice_emotion::WhyPartly:
      ImGui::TextWrapped("Shows %s on %.0f %% of the face (the rest: %s): its chance, %.3f, against Sureness needed "
        "%.2f; all of it from a chance of %.2f.", voice_feel::Presets[look.Shown].Name, 100.f * look.Share, picked,
        topChance, sure, top1);
      if (look.Scale != 1.f)
        ImGui::TextWrapped("Its own part %.2f times as strong, by the voice's arousal.", look.Scale);
      break;
    case voice_emotion::WhyNotSure:
      if (topChance > voice_emotion::SureLeast)
      {
        voice_emotion::Look least = voice_emotion::LookOf(&r, voice_emotion::FromVoice, v.FeelWeights,
          voice_emotion::SureLeast);
        ImGui::TextWrapped("Not sure enough: its chance, %.3f, isn't above Sureness needed (%.2f), so %s shows. It "
          "would show with Sureness needed under %.3f (at the least, %.2f: %s on %.0f %% of the face).", topChance,
          sure, picked, topChance, voice_emotion::SureLeast, least.Shown >= 0 ? voice_feel::Presets[least.Shown].Name :
          "it", 100.f * least.Share);
      }
      else
        ImGui::TextWrapped("Not sure enough: its chance, %.3f, is under the least Sureness needed (%.2f), so it never "
          "shows from the voice: %s shows (or set it by hand).", topChance, voice_emotion::SureLeast, picked);
      break;
    case voice_emotion::WhyNeutral:
      ImGui::TextWrapped("Heard as neutral: %s shows.", picked);
      break;
    case voice_emotion::WhyTooShort:
      ImGui::TextWrapped("%s shows.", v.Feel >= 0 ? "The feeling picked" : "No feeling (none picked)");
      break;
    case voice_emotion::WhyByHand:
      if (heard.Shown >= 0 && heard.Share > 0.f)
        ImGui::TextWrapped("Set by hand. From the voice it would show %s on %.0f %% of the face.",
          voice_feel::Presets[heard.Shown].Name, 100.f * heard.Share);
      else
        ImGui::TextWrapped("Set by hand. From the voice it would show %s.", picked);
      break;
    }
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
  }

  // [EMOTION] The stretches one by one: when, what the voice was heard as, what the face shows, and each one's own
  // setting (only the rows in view are made: a long voice has hundreds). [JUMP] On the voice's own clock, a row's time
  // takes the voice there.
  void DrawVoiceEmotionStretches(VoicePlay& v, bool own, ULONGLONG now)
  {
    size_t n = v.EmotionStretches.size();
    if (!v.EmotionReadings || v.EmotionReadings->size() != n || n == 0)
      return;
    if (v.FeelSetting.size() != n)
      v.FeelSetting.assign(n, voice_emotion::FromVoice);
    bool open = ImGui::TreeNode("The voice's feeling, stretch by stretch");
    ReportItemForTests("emotion stretches");
    if (!open)
      return;
    ImGui::PushTextWrapPos(0.f);
    ImGui::TextDisabled("Each stretch: from the voice (as far as the model is sure), the feeling picked, none, or a "
      "feeling of its own. Then when it is%s, what the model heard most (its chance) and what the face shows; the mouse "
      "on a row tells why. > : the stretch shown now.", own ? " (press it: the voice goes there)" : "");
    ImGui::PopTextWrapPos();
    const char* items[3 + voice_feel::FeelingCount];
    items[0] = "From the voice";
    items[1] = "The feeling picked";
    items[2] = "None";
    for (int f = 0; f < voice_feel::FeelingCount; ++f)
      items[3 + f] = voice_feel::Presets[f].Name;
    ImGuiListClipper clipper((int)n, ImGui::GetFrameHeightWithSpacing());
    while (clipper.Step())
      for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i)
      {
        ImGui::PushID(i);
        int& setting = v.FeelSetting[i];
        int item = setting == voice_emotion::AsPicked ? 1 : (setting == voice_emotion::NoFeeling ? 2 :
          (setting >= 0 && setting < voice_feel::FeelingCount ? 3 + setting : 0));
        ImGui::PushItemWidth(ImGui::GetFontSize() * 8.f);
        if (ImGui::Combo("##setting", &item, items, 3 + voice_feel::FeelingCount))
          setting = item == 1 ? voice_emotion::AsPicked : (item == 2 ? voice_emotion::NoFeeling :
            (item >= 3 && item < 3 + voice_feel::FeelingCount ? item - 3 : voice_emotion::FromVoice));
        ImGui::PopItemWidth();
        ImGui::SameLine();
        voice_emotion::Stretch const& st = v.EmotionStretches[i];
        voice_emotion::Reading const& r = (*v.EmotionReadings)[i];
        voice_emotion::Look look = voice_emotion::LookOf(&r, setting, v.FeelWeights, v.FeelSure);
        char heard[48] = "too short to hear";
        if (r.Ok)
        {
          int top = 0;
          for (int c = 1; c < voice_emotion::ClassCount; ++c)
            if (r.Chance[c] > r.Chance[top])
              top = c;
          _snprintf_s(heard, sizeof(heard), _TRUNCATE, "%s %.0f %%", voice_emotion::ClassNames[top], r.Chance[top] * 100.f);
        }
        char face[64];
        if (look.Shown >= 0 && look.Share > 0.f)
          _snprintf_s(face, sizeof(face), _TRUNCATE, "%s %.0f %%%s", voice_feel::Presets[look.Shown].Name, look.Share * 100.f,
            look.ByHand ? " (set by hand)" : "");
        else
          _snprintf_s(face, sizeof(face), _TRUNCATE, "%s%s", look.Share > 0.f ? "none" : "the feeling picked",
            look.ByHand ? " (set by hand)" : "");
        bool hovered = false;
        if (own)
        {
          // [JUMP] The voice to the stretch's start (held there a moment, as the Time slider holds it)
          char when[48];
          // ("###go": its ID the same with or without the "> " shown now; [REVIEW] with "##go" a press held while the
          // voice played into or out of the stretch was lost)
          _snprintf_s(when, sizeof(when), _TRUNCATE, "%s%.1f - %.1f s###go", i == v.FeelStretchNow ? "> " : "", st.From,
            st.To);
          bool go = ImGui::SmallButton(when);
          char what[48];
          _snprintf_s(what, sizeof(what), _TRUNCATE, "emotion row %d time", i);
          ReportItemForTests(what);
          if (go)
          {
            v.Time = st.From;
            v.SetByHandAt = now;
          }
          hovered = ImGui::IsItemHovered();
          ImGui::SameLine();
          ImGui::Text("heard: %s  shows: %s", heard, face);
        }
        else
          ImGui::Text("%s%.1f - %.1f s  heard: %s  shows: %s", i == v.FeelStretchNow ? "> " : "", st.From, st.To, heard, face);
        {
          char what[48];
          _snprintf_s(what, sizeof(what), _TRUNCATE, "emotion row %d text", i);
          ReportItemForTests(what);
        }
        hovered = hovered || ImGui::IsItemHovered();
        if (hovered)
          VoiceStretchTooltip(v, (size_t)i, own);
        ImGui::PopID();
      }
    ImGui::TreePop();
  }

  void DrawVoiceTab(Held& h)
  {
    float fontSize = ImGui::GetFontSize();
    ULONGLONG now = GetTickCount64();
    ImGui::PushID("PoseVoice");
    ImGui::Separator();
    ImGui::Text("Voice");
    ImGui::SameLine();
    ImGui::HelpMarker(VoiceHelp);
    if (now - g_VoiceFilesListedAt > 2000)
    {
      ListVoiceFiles();
      g_VoiceFilesListedAt = now;
    }
    VoicePlay& v = h.Voice;
    if (g_VoiceFiles.empty())
      ImGui::TextDisabled("No sound files in Cinematic Tools\\Voices (in the game folder)");
    else
    {
      ImGui::BeginChild("##VoiceFiles", ImVec2(0.f, fontSize * 4.f), true);
      for (size_t f = 0; f < g_VoiceFiles.size(); ++f)
      {
        std::string name = Utf8Of(g_VoiceFiles[f]);
        bool current = v.Sound && v.Sound->File == name;
        ImGui::PushID((int)f);
        if (ImGui::Selectable(name.c_str(), current) && !g_VoiceLoad)
          StartVoiceLoad(h, g_VoiceFiles[f]);
        ImGui::PopID();
      }
      ImGui::EndChild();
    }
    ImGui::PushTextWrapPos(0.f);
    if (g_VoiceMessage[0])
      ImGui::TextDisabled("%s", g_VoiceMessage);
    if (v.Sound)
    {
      ImGui::TextDisabled("%s", v.Sound->Report.c_str());
      if (!v.Sound->Warning.empty())
        ImGui::TextColored(ImVec4(1.f, 0.75f, 0.3f, 1.f), "Check: %s", v.Sound->Warning.c_str());
      if (!v.Rig.Ok)
        ImGui::TextColored(ImVec4(1.f, 0.75f, 0.3f, 1.f), "This character has no mouth bones the tools know (Faith, "
          "KrugerSec and people with a full skeleton have them): no mouth moves.");
      if (v.Player < 0)
        ImGui::TextColored(ImVec4(1.f, 0.75f, 0.3f, 1.f), "The player has no place for this sound (it holds 16 at "
          "most, of up to 200,000 samples a second): no sound is heard.");
      else
      {
        voice_player::EngineInfo info{};
        voice_player::GetEngineInfo(info);
        if (!info.Open && info.Failures > 0)
          ImGui::TextColored(ImVec4(1.f, 0.75f, 0.3f, 1.f), "Windows' audio couldn't be opened (no output device?): no "
            "sound is heard. The player tries again every few seconds (see CT.log).");
      }
      // (the player is silent outside its rates: VoicePlayer.h, MinRate - MaxRate)
      if (v.On && v.Heard && v.Moving && v.Player >= 0 &&
          (v.Rate < voice_player::MinRate || v.Rate > voice_player::MaxRate))
        ImGui::TextColored(ImVec4(1.f, 0.75f, 0.3f, 1.f), "The voice's time runs at %.2f times the recording's pace: the "
          "sound is only played from about 0.25 to 2 times it (the mouth still moves).", v.Rate);
    }
    ImGui::PopTextWrapPos();
    if (!v.Sound)
    {
      ImGui::PopID();
      return;
    }

    float length = (float)v.Sound->Seconds;
    bool own = v.Clock == VoiceOwnTime;
    ImGui::Checkbox("Use the voice", &v.On);
    if (own)
    {
      SameLineIfFits(v.Playing ? "Pause" : "Play");
      if (ImGui::Button(v.Playing ? "Pause" : "Play"))
      {
        v.Playing = !v.Playing;
        if (v.Playing)
        {
          v.On = true;
          if (!v.Loop && v.Time >= (double)length - 0.01)
            v.Time = 0.0;
          if (v.Time <= 0.0)
            v.Time = -VoiceRunIn;
        }
      }
      SameLineIfFits("From the start");
      if (ImGui::Button("From the start"))
        v.Time = v.Playing ? -VoiceRunIn : 0.0;
      SameLineIfFits("Loop", true);
      ImGui::Checkbox("Loop", &v.Loop);
    }
    DrawVoicePicture(v, own, now);
    if (own)
    {
      float shown = (float)v.Time;
      if (ImGui::SliderFloat("Time (s)", &shown, 0.f, length > 0.f ? length : 0.01f, "%.2f"))
        v.Time = shown;
      if (ImGui::IsItemActive())
        v.SetByHandAt = now;
    }
    else
    {
      bool keys = v.Clock == VoiceKeysTime;
      float start = (float)v.Start;
      if (ImGui::DragFloat("Begins at (s)", &start, 0.01f, -length, (float)pose_keys::MaxTime, "%.2f"))
        v.Start = start;
      ImGui::SameLine();
      ImGui::HelpMarker(keys ? "The keyframes' time at which the voice begins. Below 0: the voice is already that far "
        "in when the keyframes begin. Let it begin 0.1 s or more after the time playing starts from: the sound needs "
        "that long to come in, and a voice already running when playing starts loses its first tenth of a second." :
        "The camera track's time at which the voice begins (a shot sequence's time goes on through its cuts). Below 0: "
        "the voice is already that far in when the track begins. Let it begin 0.1 s or more into the track: the sound "
        "needs that long to come in, and a voice already running when the track starts loses its first tenth of a "
        "second.");
      ImGui::PushTextWrapPos(0.f);
      if (v.Time < 0.0)
        ImGui::TextDisabled("Begins in %.2f s", -v.Time);
      else if (v.Time >= (double)length)
        ImGui::TextDisabled("Over (%.2f s long)", length);
      else
        ImGui::TextDisabled("At %.2f of %.2f s", v.Time, length);
      if (keys && h.Keys.Channels.empty())
        ImGui::TextDisabled("No keyframes yet: the voice follows the keyframes' time when you move it, and plays with "
          "them once there are keyframes to play.");
      else if (!keys)
        ImGui::TextDisabled("The voice plays as the camera track plays, and waits at the track's start while no track "
          "plays.");
      ImGui::PopTextWrapPos();
    }
    // [REVIEW 7] The face (the living face, its settings folded, then the feeling) before the sound's and the mouth's
    // sliders: the Feeling list is found without scrolling far (the review: it was 2.5 - 3.4 panel heights down)
    // The life around the mouth: a living face (the eyes, blinks, brows, breathing and the head)
    ImGui::Checkbox("A living face", &v.Life);
    ImGui::SameLine();
    ImGui::HelpMarker(VoiceLifeHelp);
    // [FIX CHECK] The Feeling list on the tick's own row where it fits, and the face part before the Clock (the fix
    // check: 1.05 - 1.39 tab heights down, under what the tab shows at all three sizes, when it came after the clock
    // and under the tick)
    // The feeling: picked by the user, made of the game's own face poses
    {
      float comboWidth = fontSize * 8.f;
      float rowWidth = comboWidth + ImGui::CalcTextSize("Feeling").x + ImGui::GetStyle().ItemInnerSpacing.x +
        ImGui::GetStyle().ItemSpacing.x + fontSize;
      if (ImGui::GetContentRegionAvailWidth() >= rowWidth)
        ImGui::SameLine(0.f, fontSize);
      const char* feelings[voice_feel::FeelingCount + 1];
      feelings[0] = "None";
      for (int f = 0; f < voice_feel::FeelingCount; ++f)
        feelings[f + 1] = voice_feel::Presets[f].Name;
      int feel = v.Feel >= 0 && v.Feel < voice_feel::FeelingCount ? v.Feel + 1 : 0;
      ImGui::PushItemWidth(comboWidth);
      bool picked = ImGui::Combo("Feeling", &feel, feelings, voice_feel::FeelingCount + 1);
      ImGui::PopItemWidth();
      if (picked && feel >= 0 && feel <= voice_feel::FeelingCount)
        v.Feel = feel - 1;
      // (Feeling's (?) taken out 2026-11-16: VoiceFeelHelp is no longer shown)
      if (!v.FeelRig.Ok)
      {
        ImGui::PushTextWrapPos(0.f);
        ImGui::TextColored(ImVec4(1.f, 0.75f, 0.3f, 1.f), "This character has no face a feeling can be laid on (a Face bone "
          "with both eye bones and HeadEnd, its left and right known).");
        ImGui::PopTextWrapPos();
      }
      if (v.FeelRig.Ok)
        DrawVoiceEmotion(v);   // [EMOTION] "From the voice"
      if (v.FeelRig.Ok && (v.Feel >= 0 || v.FeelAuto))
      {
        ImGui::SliderFloat("Strength", &v.FeelStrength, 0.f, 2.f, "%.2f");
        ImGui::SameLine();
        ImGui::HelpMarker("How strongly the feeling shows. 1: as designed, a clear face but not an extreme one. 2: twice "
          "that, up to the game's own poses at their fullest (never past them). 0: none (and the voice's brow raises "
          "aren't held back).");
        ImGui::SliderFloat("Follows the voice", &v.FeelFollow, 0.f, 1.f, "%.2f");
        ImGui::SameLine();
        ImGui::HelpMarker("How much the feeling's strength follows the voice: stronger on stressed words, weaker in "
          "pauses. 0: the same all through. 1: the brows ease to about two thirds in the pauses between words and towards "
          "half in long silences (the eyes and the mouth ease less: to 0.7 and 0.85 at most), about nine tenths on the "
          "most stressed words. It never goes while the voice is on.");
        // [SMILE]
        ImGui::SliderFloat("Smile comes and goes", &v.FeelComes, 0.f, 1.f, "%.2f");
        ImGui::SameLine();
        ImGui::HelpMarker(VoiceSmileHelp);
        ImGui::SliderFloat("Uneven", &v.FeelUneven, 0.f, 0.3f, "%.2f");
        ImGui::SameLine();
        ImGui::HelpMarker("One side of the face this much stronger, the other this much weaker: real faces are seldom "
          "even (the game's own recorded brows: in the most uneven people one side moves nearly twice as far as the "
          "other). 0.2 here makes one side 1.5 times the other, up to Strength 1; stronger, the strongest parts reach the "
          "game's own poses on both sides and even out. 0: both sides the same.");
        if (v.FeelUneven > 0.f)
        {
          ImGui::SameLine();
          ImGui::Checkbox("Its left stronger", &v.FeelLeftStronger);
        }
      }
      if (v.FeelRig.Ok && v.FeelAuto)
        DrawVoiceEmotionStretches(v, own, now);   // [EMOTION]
    }
    if (v.Life)
    {
      ImGui::PushTextWrapPos(0.f);
      if (v.Sound->Life.empty())
        ImGui::TextColored(ImVec4(1.f, 0.75f, 0.3f, 1.f), "For this sound %s: no blinks, brow raises, eye or head movement "
          "(a feeling picked above still shows).", v.Sound->LifeNote.empty() ? "they couldn't be worked out" :
          v.Sound->LifeNote.c_str());
      else
      {
        // [REVIEW 8] With a take in use its blinks are the face's: the sound's own count (the analysis' blinks, which
        // the take replaces) is left out of the found line, so the tab gives one blink count
        std::string found = v.Sound->LifeFound;
        if (v.Alive && !v.AliveFound.empty())
        {
          size_t comma = found.find(", ");
          found = comma == std::string::npos ? std::string() : found.substr(comma + 2);
        }
        if (!found.empty())
          ImGui::TextDisabled("Found in this sound: %s.", found.c_str());
        if (!v.AliveFound.empty())
          ImGui::TextDisabled("This take: %s.", v.AliveFound.c_str());
        if (!v.Sound->LifeNote.empty())
          ImGui::TextColored(ImVec4(1.f, 0.75f, 0.3f, 1.f), "Check: %s.", v.Sound->LifeNote.c_str());
      }
      // (the face's own directions come from the Face bone, both eye bones and HeadEnd: without them no part can be
      // made, the head included, though the Head bone may be there and held)
      if (v.LifeRig.Face < 0)
        ImGui::TextColored(ImVec4(1.f, 0.75f, 0.3f, 1.f), "This character has no face bones the tools can use (a Face bone "
          "with both eye bones and HeadEnd; the low-detail people and the animals have none): no blinks, no brow raises, "
          "no eye movement, and no head movement either (the head's directions are taken from the eyes).");
      else if (!v.LifeRig.Ok || !v.LifeRig.Eyes || !v.LifeRig.Brows || !v.LifeRig.Head || v.LifeRig.EyeBone[0] < 0)
      {
        // (the four parts in one text: a comma between two of them)
        bool lids = !(v.LifeRig.Ok && v.LifeRig.Eyes), brows = !(v.LifeRig.Ok && v.LifeRig.Brows),
          head = !(v.LifeRig.Ok && v.LifeRig.Head), eyes = !(v.LifeRig.Ok && v.LifeRig.EyeBone[0] >= 0);
        std::string missing;
        for (auto part : { std::make_pair(lids, " its eyelids"), std::make_pair(brows, " its eyebrows"),
          std::make_pair(head, " its head"), std::make_pair(eyes, " its eyes") })
          if (part.first)
            missing += (missing.empty() ? "" : ",") + std::string(part.second);
        ImGui::TextColored(ImVec4(1.f, 0.75f, 0.3f, 1.f), "Not found on this character, or not all held in this pose:%s.",
          missing.c_str());
      }
      // A part there and held that the pose as it is now gives no directions for (its eye bones, HeadEnd or nose tip
      // edited out of place): it stops while that lasts
      if (v.LifeTried && v.LifeRig.Ok)
      {
        bool eyes = v.LifeRig.Eyes && !v.LifeEyesNow, brows = v.LifeRig.Brows && !v.LifeBrowsNow,
          head = v.LifeRig.Head && !v.LifeHeadNow;
        if (eyes || brows || head)
          ImGui::TextColored(ImVec4(1.f, 0.75f, 0.3f, 1.f), "Not made as the face is posed now:%s%s%s%s%s (its eye bones, "
            "HeadEnd or nose tip as edited give no directions).", eyes ? " its eyelids" : "", eyes && (brows || head) ? "," : "",
            brows ? " its eyebrows" : "", brows && head ? "," : "", head ? " its head" : "");
      }
      ImGui::PopTextWrapPos();
      if (ImGui::TreeNode("Settings of the living face"))
      {
        ImGui::SliderFloat("Blinks", &v.Blinks, 0.f, 1.f, "%.2f");
        ImGui::SliderFloat("Brows", &v.Brows, 0.f, 2.f, "%.2f");
        ImGui::SliderFloat("Head", &v.Head, 0.f, 2.f, "%.2f");
        ImGui::SliderFloat("Eyes", &v.Eyes, 0.f, 1.f, "%.2f");
        float lookAway = v.LookAway;
        if (ImGui::SliderFloat("Looks away", &lookAway, 0.f, 2.f, "%.2f"))
        {
          v.LookAway = !(lookAway > 0.f) ? 0.f : (lookAway > 2.f ? 2.f : lookAway);
          v.AliveWantedAt = now;
        }
        ImGui::Checkbox("Eyes hold their target", &v.HoldGaze);
        int take = v.Take;
        if (ImGui::SliderInt("Take", &take, 1, 99))
        {
          v.Take = take < 1 ? 1 : (take > 99 ? 99 : take);
          v.AliveWantedAt = now;
        }
        ImGui::TreePop();
      }
    }

    const char* clocks[] = { "Its own (real time: plays with time frozen too)", "The keyframes' time",
      "The camera track's time" };
    int clock = v.Clock;
    if (ImGui::Combo("Clock", &clock, clocks, 3) && clock != v.Clock && clock >= 0 && clock < 3)
    {
      v.Clock = clock;
      v.Playing = false;
    }
    ImGui::Checkbox("The sound is heard", &v.Heard);
    ImGui::SliderFloat("Volume", &v.Volume, 0.f, 2.f, "%.2f");
    ImGui::SliderFloat("Mouth opening", &v.Jaw, 0.f, 2.f, "%.2f");
    ImGui::SameLine();
    ImGui::HelpMarker("How far the jaw opens with the voice. 1: about 9 degrees on the loudest open vowels; raise it for "
      "a shouted line. This is what the sound gives best: about one opening a syllable, shut in every pause (a feeling "
      "of Surprised or Afraid holds it a little open there; this scales that too).");
    ImGui::SliderFloat("Lip shapes", &v.Lips, 0.f, 2.f, "%.2f");
    ImGui::SameLine();
    ImGui::HelpMarker("How much the lips and the mouth's corners take the shapes heard in the sound: wide for \"ee\", "
      "rounded for \"oo\", the teeth for s and sh. These are worked out from how the voice sounds, not from its words, "
      "and can be wrong (an \"oo\" said far forward reads as \"ee\", an l as rounded), so they start at half. 0: only "
      "the jaw moves.");
    ImGui::SliderFloat("Lip closing", &v.Closure, 0.f, 2.f, "%.2f");
    ImGui::SameLine();
    ImGui::HelpMarker("How much the mouth shuts where a p, b or m is heard. The sound alone only hints at these (an m "
      "and an n are the same hum: on test voices the hint was sure of a third to a half of them, and now and then took "
      "a t, k or n for one), so the mouth shuts by how sure the hint is. 0: left out (for a voice over music or "
      "noise).");
    ImGui::SliderFloat("Mouth smoothing", &v.Smoothing, 0.f, 2.f, "%.2f");
    ImGui::SameLine();
    ImGui::HelpMarker("How smoothly the mouth goes from one shape to the next. 1: the jaw and lips ease into and out of "
      "each shape and still shut on p, b and m. 0: quicker and harder. 2: softer still, the "
      "closures eased too. In between, the two are mixed. The smoother, the more quick syllables run together and open "
      "the mouth less; on computer-made test voices the jaw opened to a fifth of its full opening on 91 % of the vowels at 0, 87 % at 1 "
      "and 81 % at 2, and in fast speech on 84, 75 and 65 %. If the mouth still looks too busy, lowering Mouth "
      "opening also slows it: it then has less far to go.");
    ImGui::SliderFloat("Mouth ahead by (s)", &v.Lead, -MaxVoiceLead, MaxVoiceLead, "%.2f");
    ImGui::SameLine();
    ImGui::HelpMarker("The mouth moves this much before its sound is heard (a mouth that's late looks wrong sooner "
      "than one that's early). Raise it if the mouth looks late, lower it if early.");
    if ((v.Rig.Ok || (v.Life && v.LifeRig.Ok) || v.FeelRig.Ok) && ImGui::TreeNode("The face now"))
    {
      auto bar = [&](float amount, char const* name)
      {
        ImGui::ProgressBar(amount > 0.f ? (amount < 1.f ? amount : 1.f) : 0.f, ImVec2(fontSize * 8.f, 0.f), "");
        ImGui::SameLine();
        ImGui::TextUnformatted(name);
      };
      if (v.Rig.Ok)
      {
        float const amounts[5] = { v.Shape.Open, v.Shape.Wide, v.Shape.Round, v.Shape.Closed, v.Shape.Teeth };
        char const* const names[5] = { "Open (ah)", "Wide (ee)", "Rounded (oo)", "Pressed (p, b, m)", "Teeth (s, sh)" };
        for (int k = 0; k < 5; ++k)
          bar(amounts[k], names[k]);
      }
      // (as numbers that are numbers: these go into a text; and what rounds to 0.0 as 0: no "-0.0")
      auto shown = [](float value) { return !std::isfinite(value) || fabsf(value) < 0.05f ? 0.f : value; };
      float const degrees = 57.29578f;
      if (v.Life && v.LifeRig.Ok)
      {
        // What the face is given this frame: each part that moves now, times its amount as the face takes it
        auto given = [](float shape, float amount)
        {
          float a = !(amount > 0.f) ? 0.f : (amount > 2.f ? 2.f : amount);
          return std::isfinite(shape) && a > 0.f ? shape * a : 0.f;
        };
        if (v.LifeEyesNow)
          bar(given(v.LifeShape.Blink, v.Blinks) * v.LifeBlinkTaken, "Blink");   // (lids already turned shut take less)
        if (v.LifeBrowsNow)
        {
          // (times the share a feeling keeps of the voice's raises)
          float brow = given(v.LifeShape.Brow, v.Brows) * (v.FeelNow ? v.FeelKeep : 1.f);
          char name[48];
          _snprintf_s(name, sizeof(name), _TRUNCATE, "Brows up (%.1f mm)", shown(1000.f * voice_face::BrowLift * brow));
          bar(brow, name);
        }
        if (v.LifeHeadNow)
        {
          float nod = given(v.LifeShape.Nod, v.Head) * degrees, turn = given(v.LifeShape.Turn, v.Head) * degrees,
            tilt = given(v.LifeShape.Tilt, v.Head) * degrees;
          ImGui::TextWrapped("Head: chin down %+.1f, nose to its right %+.1f, top to its right %+.1f degrees", shown(nod),
            shown(turn), shown(tilt));
        }
        if (v.EyesNow)
          ImGui::TextWrapped("Eyes, as laid: %+.1f to its right, %+.1f down against the pose (degrees; %s)",
            shown(v.GazeNow[0]), shown(v.GazeNow[1]), v.HoldGaze ? "held against the voice's head" : "riding with the head");
      }
      if (v.FeelNow)
      {
        // The feeling as shown: the one picked, or while another still goes out, both; and its own head posture
        int picked = v.Feel >= 0 && v.Feel < voice_feel::FeelingCount ? v.Feel + 1 : 0, going = -1;
        for (int f = 0; f <= voice_feel::FeelingCount; ++f)
          if (f != picked && v.FeelWeights[f] > 0.f && (going < 0 || v.FeelWeights[f] > v.FeelWeights[going]))
            going = f;
        auto nameOf = [](int f) { return f <= 0 ? "none" : voice_feel::Presets[f - 1].Name; };
        // [EMOTION] [REVIEW] With the voice's feeling on, the feeling picked has only the share the voice leaves it
        float pickedShare = 1.f - (v.FeelVoiceShareNow > 1.f ? 1.f : v.FeelVoiceShareNow);
        char onFace[48] = "";
        if (v.FeelVoiceShareNow > 0.f)
          _snprintf_s(onFace, sizeof(onFace), _TRUNCATE, ", on %.0f %% of the face", pickedShare * 100.f);
        if (going >= 0)
          ImGui::TextWrapped("Feeling picked: going from %s to %s%s", nameOf(going), nameOf(picked), onFace);
        else if (picked > 0)
          ImGui::TextWrapped("Feeling picked: %s at %.2f%s", nameOf(picked), v.FeelStrengthNow, onFace);
        // and the voice's own over it
        if (v.FeelAutoNow > 0.f && v.FeelStretchNow >= 0 && v.FeelStretchNow < (int)v.FeelLooks.size() &&
          v.FeelStretchNow < (int)v.EmotionStretches.size())
        {
          voice_emotion::Look const& look = v.FeelLooks[v.FeelStretchNow];
          char const* how = look.ByHand ? "set by hand" : "from the voice";
          // [FIX CHECK] While two stretches change into each other the voice's share is theirs together: named, not
          // given to one of them
          bool changing = v.FeelStretchWeightNow < 0.999f;
          char scale[48] = "";
          if (!look.ByHand && look.Shown >= 0 && look.Share > 0.f && fabsf(look.Scale - 1.f) > 0.005f)
            _snprintf_s(scale, sizeof(scale), _TRUNCATE, ", its own part x%.2f by the voice", look.Scale);
          if (changing)
            ImGui::TextWrapped("Stretch %d of %d, %s: %s%s; changing with the stretch next to it: the voice's feelings on "
              "%.0f %% of the face", v.FeelStretchNow + 1, (int)v.EmotionStretches.size(), how, look.Shown >= 0 &&
              look.Share > 0.f ? voice_feel::Presets[look.Shown].Name : (look.Share > 0.f ? "no feeling" :
              "the feeling picked"), scale, v.FeelVoiceShareNow * 100.f);
          else if (look.Shown >= 0 && look.Share > 0.f)
            ImGui::TextWrapped("Stretch %d of %d, %s: %s on %.0f %% of the face%s%s", v.FeelStretchNow + 1,
              (int)v.EmotionStretches.size(), how, voice_feel::Presets[look.Shown].Name, v.FeelVoiceShareNow * 100.f,
              scale, v.FeelAutoNow < 1.f ? " (coming in or going out)" : "");
          else
            ImGui::TextWrapped("Stretch %d of %d, %s: %s", v.FeelStretchNow + 1, (int)v.EmotionStretches.size(), how,
              look.Share > 0.f ? "no feeling" : "the feeling picked");
        }
        float nod = shown(v.FeelNod * degrees), tilt = shown(v.FeelTilt * degrees);
        if (nod != 0.f || tilt != 0.f)
          ImGui::TextWrapped("The feeling's own head: chin down %+.1f, top to its right %+.1f degrees", nod, tilt);
        // [SMILE] [JAW]
        if (v.FeelComes > 0.f && !v.SmileTimes.From.empty())
          ImGui::TextWrapped("Its smile (where it has one): %.0f %% of it, coming and going with the speech",
            100.f * v.FeelSmileNow);
        // ([FIX CHECK] the line stays while the feeling has a jaw, also where it is gone for a p, b or m: it no longer
        // comes and goes, moving the button under it)
        if (v.FeelJawOn && v.FeelJawNow > 0.f)
          ImGui::TextWrapped("Its jaw: %.0f %% of the way open beneath the speech's own (%.1f degrees in a pause, with "
            "Mouth opening at %.2f)", 100.f * v.FeelJawNow, v.FeelJawNow * voice_face::JawOpenAngle * degrees *
            (!(v.Jaw > 0.f) ? 0.f : (v.Jaw > 2.f ? 2.f : v.Jaw)), !(v.Jaw > 0.f) ? 0.f : (v.Jaw > 2.f ? 2.f : v.Jaw));
        else if (v.FeelJawOn)
          ImGui::TextWrapped("Its jaw: shut here (the lips close for a p, b or m)");
      }
      ImGui::TreePop();
    }
    if (ImGui::Button("Remove the voice"))
      RemoveVoice(h);
    ImGui::PopID();
  }

  void DrawPosePanel(Held& h, float height, bool& configChanged)
  {
    float fontSize = ImGui::GetFontSize();
    if (g_PoseTab < 0)
      g_PoseTab = 0;
    if (g_PoseTab > 4)
      g_PoseTab = 4;
    static char const* tabIds[] = { "##PoseTabBone", "##PoseTabClip", "##PoseTabVoice", "##PoseTabFiles",
      "##PoseTabOptions" };
    ImGui::BeginChild("##PosePanel", ImVec2(0.f, height), false);
    float width = (ImGui::GetContentRegionAvailWidth() - ImGui::GetStyle().ItemSpacing.x * 4.f) / 5.f;
    if (width < 1.f)
      width = 1.f;
    if (TabButton("Bone", g_PoseTab == 0, width))
      g_PoseTab = 0;
    ImGui::SameLine();
    if (TabButton("Clip", g_PoseTab == 1, width))
      g_PoseTab = 1;
    ImGui::SameLine();
    if (TabButton("Voice", g_PoseTab == 2, width))
      g_PoseTab = 2;
    ImGui::SameLine();
    if (TabButton("Files", g_PoseTab == 3, width))
      g_PoseTab = 3;
    ImGui::SameLine();
    if (TabButton("Options", g_PoseTab == 4, width))
      g_PoseTab = 4;

    Snapshot before = SnapshotOf(h);
    if (!g_SlidersWereActive || g_SliderPressHeld != h.Id)
    {
      g_SliderPress = before;   // (the frame a slider is taken, this is the pose it's taken in: SliderHold)
      g_SliderPressHeld = h.Id;
    }
    bool slidersActive = false;
    ImGui::BeginChild(tabIds[g_PoseTab], ImVec2(0.f, 0.f), false);
    ImGui::PushItemWidth(fontSize * 11.f);
    if (g_PoseTab == 0)
      DrawBoneTab(h, before, slidersActive);
    else if (g_PoseTab == 1)
      DrawClipTab(h);
    else if (g_PoseTab == 2)
      DrawVoiceTab(h);
    else if (g_PoseTab == 3)
      DrawFilesTab(h);
    else
      DrawOptionsTab(h, configChanged);
    ImGui::PopItemWidth();
    ImGui::EndChild();
    if (slidersActive && !g_SlidersWereActive)
      PushUndo(h, before);
    g_SlidersWereActive = slidersActive;
    ImGui::EndChild();
  }

  void DrawPoseMiddle(Held& h, bool& configChanged)
  {
    float fontSize = ImGui::GetFontSize();
    float middleHeight = (std::min)((std::max)(ImGui::GetContentRegionAvail().y - fontSize * 16.5f, fontSize * 10.f),
      fontSize * 20.f);
    DrawBonesPane(h, middleHeight);
    ImGui::SameLine();
    DrawPosePanel(h, middleHeight, configChanged);
  }

  char const* KeyEaseLabel(int ease)
  {
    switch (ease)
    {
    case pose_keys::Ease_Linear:
      return "Linear";
    case pose_keys::Ease_EaseInOut:
      return "Ease in and out";
    case pose_keys::Ease_Hold:
      return "Hold";
    case pose_keys::Ease_In:
      return "Ease in (starts slow)";
    case pose_keys::Ease_Out:
      return "Ease out (ends slow)";
    case pose_keys::Ease_Overshoot:
      return "Overshoot and settle";
    default:
      return "Smooth";
    }
  }

  void CancelKeyMouse()
  {
    g_KeyDrag.Pending = false;
    g_KeyDrag.On = false;
    g_KeyScrub = false;
  }

  void UpdateKeyTimeCache(Held const& h)
  {
    if (g_KeyTimesHeld == h.Id && g_KeyTimesRevision == h.Keys.Revision &&
        g_KeyTimesChannels == h.Keys.Channels.size())
      return;
    std::vector<double> times;
    for (size_t c = 0; c < h.Keys.Channels.size(); ++c)
    {
      std::vector<pose_keys::Key> const& keys = h.Keys.Channels[c].Keys;
      for (size_t k = 0; k < keys.size(); ++k)
        times.push_back(keys[k].Time);
    }
    std::sort(times.begin(), times.end());
    g_KeyTimes.clear();
    for (size_t i = 0; i < times.size(); ++i)
      if (g_KeyTimes.empty() || times[i] - g_KeyTimes.back() > pose_keys::TimeEpsilon)
        g_KeyTimes.push_back(times[i]);
    g_KeyTimesHeld = h.Id;
    g_KeyTimesRevision = h.Keys.Revision;
    g_KeyTimesChannels = h.Keys.Channels.size();
  }

  void DrawKeyTimeline(Held& h)
  {
    if ((g_KeyDrag.Pending || g_KeyDrag.On || g_KeyScrub) && g_KeyDrag.Held != h.Id)
      CancelKeyMouse();
    ULONGLONG now = GetTickCount64();
    if ((g_KeyDrag.Pending || g_KeyDrag.On || g_KeyScrub) && now - g_KeyDrag.LastTick > 500)
      CancelKeyMouse();
    if (!(h.Keys.Length > 0.0))
    {
      CancelKeyMouse();
      return;
    }
    UpdateKeyTimeCache(h);

    float fontSize = ImGui::GetFontSize();
    ImVec2 mouse = ImGui::GetMousePos();
    bool mouseDown = ImGui::IsMouseDown(0);
    bool released = !mouseDown;

    float fullWidth = ImGui::GetContentRegionAvail().x;
    float totalHeight = (std::max)(fontSize * 5.f, ImGui::GetContentRegionAvail().y);
    ImVec2 headerMin = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##PoseKeyHeader", ImVec2(fullWidth, fontSize * 2.7f));
    bool headerHovered = ImGui::IsItemHovered();
    bool headerActive = ImGui::IsItemActive();
    ImVec2 headerMax = ImGui::GetItemRectMax();

    float namesWidth = fontSize * 9.f;
    float halfHover = fontSize * 0.42f;
    float scrollbar = ImGui::GetStyle().ScrollbarSize;
    float areaLeft = headerMin.x + namesWidth;
    float areaRight = headerMax.x - scrollbar - halfHover;
    float areaWidth = areaRight - areaLeft;
    if (areaWidth < fontSize * 4.f)
    {
      areaWidth = fontSize * 4.f;
      areaRight = areaLeft + areaWidth;
    }
    float headerHeight = fontSize * 2.7f;
    float rulerHeight = fontSize * 1.4f;
    float allHeight = fontSize * 1.3f;
    float allTop = headerMin.y + rulerHeight;
    float rowHeight = fontSize * 1.3f;
    float half = fontSize * 0.32f;
    float restHeight = (std::max)(fontSize * 2.3f, totalHeight - headerHeight);

    auto keyX = [&](double time)
    {
      return areaLeft + (float)((time / h.Keys.Length) * (double)areaWidth);
    };
    auto snapTime = [&](double time)
    {
      if (g_SnapKeys)
        time = std::floor(time / 0.05 + 0.5) * 0.05;
      return time;
    };
    auto scrubTime = [&]()
    {
      double time = ((double)mouse.x - (double)areaLeft) / (double)areaWidth * h.Keys.Length;
      double best = fontSize * 0.45f;
      bool found = false;
      for (size_t i = 0; i < g_KeyTimes.size(); ++i)
      {
        float x = keyX(g_KeyTimes[i]);
        float distance = std::fabs(x - mouse.x);
        if (distance <= best)
        {
          best = distance;
          time = g_KeyTimes[i];
          found = true;
        }
      }
      if (!found)
        time = snapTime(time);
      if (time < 0.0)
        time = 0.0;
      if (time > h.Keys.Length)
        time = h.Keys.Length;
      return time;
    };

    int selectedChannel = -1;
    for (size_t c = 0; c < h.Keys.Channels.size(); ++c)
      if (h.Keys.Channels[c].Bone == g_SelectedBone)
        selectedChannel = (int)c;
    size_t channelCount = h.Keys.Channels.size();
    float contentHeight = (float)channelCount * rowHeight;
    ImVec2 rowsMin(0.f, 0.f);
    float childTop = 0.f;
    float childBottom = 0.f;

    struct TimelineHit
    {
      bool Any;
      bool Name;
      int Bone;
      double Time;
      int KeyIndex;
    };
    auto hitTest = [&](bool overHeader, bool overRows, ImVec2 at)
    {
      TimelineHit hit{};
      hit.Bone = -3;
      if (overHeader && at.x >= areaLeft - fontSize * 0.45f && at.x <= areaRight + fontSize * 0.45f &&
          at.y >= allTop && at.y <= headerMax.y)
      {
        float best = fontSize * 0.45f;
        for (size_t i = 0; i < g_KeyTimes.size(); ++i)
        {
          float x = keyX(g_KeyTimes[i]);
          float distance = std::fabs(x - at.x);
          if (distance <= best)
          {
            best = distance;
            hit.Any = true;
            hit.Bone = -2;
            hit.Time = g_KeyTimes[i];
          }
        }
      }
      if (overRows && at.y >= childTop && at.y <= childBottom)
      {
        float localY = at.y - rowsMin.y;
        if (localY >= 0.f && localY < contentHeight)
        {
          int row = (int)(localY / rowHeight);
          if (row >= 0 && row < (int)channelCount)
          {
            pose_keys::Channel const& channel = h.Keys.Channels[(size_t)row];
            if (at.x >= areaLeft - fontSize * 0.45f && at.x <= areaRight + fontSize * 0.45f)
            {
              float best = fontSize * 0.45f;
              for (size_t k = 0; k < channel.Keys.size(); ++k)
              {
                float x = keyX(channel.Keys[k].Time);
                float distance = std::fabs(x - at.x);
                if (distance <= best)
                {
                  best = distance;
                  hit.Any = true;
                  hit.Bone = channel.Bone;
                  hit.Time = channel.Keys[k].Time;
                  hit.KeyIndex = (int)k;
                }
              }
            }
            if (!hit.Any && at.x >= headerMin.x && at.x < areaLeft)
            {
              hit.Name = true;
              hit.Bone = channel.Bone;
              hit.Time = -1.0;
            }
          }
        }
      }
      return hit;
    };

    auto dragTime = [&]()
    {
      double time = ((double)mouse.x - (double)areaLeft) / (double)areaWidth * g_KeyDrag.Length;
      time = snapTime(time);
      if (time < 0.0)
        time = 0.0;
      if (time > g_KeyDrag.Length)
        time = g_KeyDrag.Length;
      return time;
    };
    ImGui::SetCursorScreenPos(ImVec2(headerMin.x, headerMin.y + headerHeight));
    ImDrawList* rootDraw = ImGui::GetWindowDrawList();
    // (A child without a border has no window padding in this ImGui. No style is pushed before BeginChild and popped
    // after it: ImGui 1.61 ends the game when a window's style stack differs between its Begin and its End.)
    ImGui::BeginChild("##PoseKeyRows", ImVec2(0.f, restHeight), false, ImGuiWindowFlags_AlwaysVerticalScrollbar);

    if (selectedChannel >= 0 && g_SelectedBone != g_KeyTimelineBone)
    {
      float rowTop = (float)selectedChannel * rowHeight;
      float visibleTop = ImGui::GetScrollY();
      float visibleHeight = ImGui::GetWindowHeight();
      if (rowTop < visibleTop || rowTop + rowHeight > visibleTop + visibleHeight)
      {
        float target = rowTop - (restHeight - rowHeight) * 0.5f;
        float maxScroll = (std::max)(0.f, contentHeight - restHeight);
        if (target < 0.f)
          target = 0.f;
        if (target > maxScroll)
          target = maxScroll;
        ImGui::SetScrollY(target);
      }
    }
    g_KeyTimelineBone = g_SelectedBone;

    ImGui::InvisibleButton("##PoseKeyRowsArea", ImVec2(namesWidth + areaWidth + halfHover, contentHeight));
    bool rowsHovered = ImGui::IsItemHovered();
    bool rowsActive = ImGui::IsItemActive();
    rowsMin = ImGui::GetItemRectMin();
    childTop = ImGui::GetWindowPos().y;
    childBottom = childTop + ImGui::GetWindowHeight();
    ImDrawList* childDraw = ImGui::GetWindowDrawList();

    TimelineHit press = hitTest(headerHovered, rowsHovered, mouse);
    bool leftClick = ImGui::IsMouseClicked(0);
    bool rightClick = ImGui::IsMouseClicked(1);
    bool pointerActive = headerActive || rowsActive;
    if (leftClick && pointerActive && (headerHovered || rowsHovered) &&
        !g_KeyDrag.Pending && !g_KeyDrag.On && !g_KeyScrub)
    {
      if (press.Any)
      {
        if (press.Bone >= 0)
        {
          g_SelectedBone = press.Bone;
          g_KeyTimelineBone = g_SelectedBone;
          g_SelectedGroup = -1;
          g_SelectedWeapon = -1;
          g_BoneListBone = g_SelectedBone;
          g_BoneListGroup = g_SelectedGroup;
        }
        if (!TrackHoldsTime(h) && !PlayingKeys(h))
          SetPlayhead(h, press.Time);
        g_KeyDrag.Pending = true;
        g_KeyDrag.On = false;
        g_KeyDrag.Held = h.Id;
        g_KeyDrag.Bone = press.Bone;
        g_KeyDrag.StartTime = press.Time;
        g_KeyDrag.PressX = mouse.x;
        g_KeyDrag.LastTick = now;
        g_KeyScrub = false;
      }
      else if (mouse.x >= areaLeft && mouse.x <= areaRight && !TrackHoldsTime(h))
      {
        g_KeyDrag.Pending = false;
        g_KeyDrag.On = false;
        g_KeyDrag.Held = h.Id;
        g_KeyScrub = true;
        g_KeyDrag.LastTick = now;
        SetPlayhead(h, scrubTime());
      }
    }
    if (rightClick && (headerHovered || rowsHovered) && !g_KeyDrag.Pending && !g_KeyDrag.On && !g_KeyScrub)
    {
      if (press.Any)
      {
        g_KeyMenuHeld = h.Id;
        g_KeyMenuBone = press.Bone;
        g_KeyMenuTime = press.Time;
        g_KeyMenuOpen = true;
      }
      else if (press.Name)
      {
        g_KeyMenuHeld = h.Id;
        g_KeyMenuBone = press.Bone;
        g_KeyMenuTime = -1.0;
        g_KeyMenuOpen = true;
      }
    }

    auto applyDrag = [&](double time)
    {
      if (h.History.empty())
      {
        CancelKeyMouse();
      }
      else
      {
        h.Keys.Channels = h.History.back().Keys;
        h.Keys.Length = h.History.back().Length;
        if (g_KeyDrag.Bone == -2)
        {
          pose_keys::MoveKeysAt(h.Keys, g_KeyDrag.StartTime, time);
          ++h.Keys.Revision;
          g_KeyDrag.LastTime = time;
          if (!TrackHoldsTime(h))
            SetPlayhead(h, time);
        }
        else
        {
          pose_keys::Channel* channel = pose_keys::Find(h.Keys, g_KeyDrag.Bone);
          int key = channel != nullptr ? pose_keys::KeyAt(*channel, g_KeyDrag.StartTime) : -1;
          if (channel == nullptr || key < 0)
          {
            ++h.Keys.Revision;
            CancelKeyMouse();
          }
          else
          {
            pose_keys::MoveKey(h.Keys, g_KeyDrag.Bone, key, time);
            ++h.Keys.Revision;
            g_KeyDrag.LastTime = time;
            if (!TrackHoldsTime(h))
              SetPlayhead(h, time);
          }
        }
      }
    };

    if (g_KeyDrag.On)
    {
      if (g_KeyDrag.Held != h.Id || h.History.empty())
      {
        CancelKeyMouse();
      }
      else if (released)
      {
        if (std::fabs(g_KeyDrag.LastTime - g_KeyDrag.StartTime) <= pose_keys::TimeEpsilon)
        {
          h.Keys.Channels = h.History.back().Keys;
          h.Keys.Length = h.History.back().Length;
          DropLastStep(h);   // its own step, and the Redo it cleared put back
          ++h.Keys.Revision;
        }
        CancelKeyMouse();
      }
      else if (mouseDown)
      {
        applyDrag(dragTime());
      }
    }
    else if (g_KeyDrag.Pending)
    {
      if (g_KeyDrag.Held != h.Id || released)
      {
        CancelKeyMouse();
      }
      else if (mouseDown && std::fabs(mouse.x - g_KeyDrag.PressX) > MoveStartPixels && !PlayingKeys(h) &&
               h.Keys.On)
      {
        PushKeyUndo(h);
        g_KeyDrag.Length = h.Keys.Length;
        g_KeyDrag.On = true;
        g_KeyDrag.Pending = false;
        g_KeyDrag.LastTick = now;
        applyDrag(dragTime());
      }
    }
    else if (g_KeyScrub)
    {
      if (g_KeyDrag.Held != h.Id || released)
      {
        CancelKeyMouse();
      }
      else if (mouseDown)
      {
        SetPlayhead(h, scrubTime());
      }
    }
    if (g_KeyDrag.Pending || g_KeyDrag.On || g_KeyScrub)
      g_KeyDrag.LastTick = now;

    UpdateKeyTimeCache(h);
    TimelineHit hitDraw = hitTest(headerHovered, rowsHovered, mouse);

    float scrollY = ImGui::GetScrollY();
    int firstRow = (int)(scrollY / rowHeight);
    if (firstRow < 0)
      firstRow = 0;
    int lastRow = (int)((scrollY + childBottom - childTop) / rowHeight);
    if (lastRow >= (int)channelCount)
      lastRow = (int)channelCount - 1;

    auto drawDiamond = [&](ImDrawList* draw, float x, float y, float size, ImU32 colour, bool hollow)
    {
      if (draw->VtxBuffer.Size > 60000)
        return;
      ImVec2 a(x, y - size), b(x + size, y), c(x, y + size), d(x - size, y);
      if (hollow)
      {
        // A hold keyframe the editor made with a bone's first one (Key::Auto): its outline only
        draw->AddQuad(a, b, c, d, IM_COL32(0, 0, 0, 153), 3.f);
        draw->AddQuad(a, b, c, d, colour, 1.5f);
        return;
      }
      draw->AddQuadFilled(a, b, c, d, colour);
      draw->AddQuad(a, b, c, d, IM_COL32(0, 0, 0, 153), 1.f);
    };

    ImU32 textColour = ImGui::GetColorU32(ImGuiCol_Text);
    ImU32 dimColour = ImGui::GetColorU32(ImGuiCol_TextDisabled);
    ImU32 grabColour = ImGui::GetColorU32(ImGuiCol_SliderGrab);
    ImU32 grabActiveColour = ImGui::GetColorU32(ImGuiCol_SliderGrabActive);

    for (int row = firstRow; row <= lastRow; ++row)
      if (row % 2 == 1)
        childDraw->AddRectFilled(ImVec2(headerMin.x, rowsMin.y + row * rowHeight),
          ImVec2(areaRight + halfHover, rowsMin.y + (row + 1) * rowHeight), ImGui::GetColorU32(ImGuiCol_Text, 0.04f));

    childDraw->PushClipRect(ImVec2(headerMin.x, childTop), ImVec2(areaLeft, childBottom), true);
    for (int row = firstRow; row <= lastRow; ++row)
    {
      ImVec2 at(headerMin.x + fontSize * 0.3f, rowsMin.y + row * rowHeight + fontSize * 0.15f);
      childDraw->AddText(at, h.Keys.Channels[(size_t)row].Bone == g_SelectedBone ? textColour : dimColour,
        BoneName(h, h.Keys.Channels[(size_t)row].Bone));
    }
    childDraw->PopClipRect();

    childDraw->PushClipRect(ImVec2(areaLeft - halfHover, childTop), ImVec2(areaRight + halfHover, childBottom), true);
    for (int row = firstRow; row <= lastRow; ++row)
    {
      pose_keys::Channel const& channel = h.Keys.Channels[(size_t)row];
      int playKey = channel.Bone == g_SelectedBone ? pose_keys::KeyAt(channel, h.Keys.Time) : -1;
      float y = rowsMin.y + row * rowHeight + rowHeight * 0.5f;
      float lastX = -1e30f;
      for (size_t k = 0; k < channel.Keys.size(); ++k)
      {
        float x = keyX(channel.Keys[k].Time);
        if (x < areaLeft - halfHover || x > areaRight + halfHover)
          continue;
        bool hovered = hitDraw.Any && hitDraw.Bone == channel.Bone && hitDraw.KeyIndex == (int)k;
        if (!hovered && (int)k != playKey && x - lastX < half)
          continue;
        lastX = x;
        float size = hovered ? halfHover : half;
        ImU32 colour = hovered ? grabActiveColour : ((int)k == playKey ? textColour : grabColour);
        drawDiamond(childDraw, x, y, size, colour, channel.Keys[k].Auto);
      }
    }
    childDraw->PopClipRect();

    float playheadX = keyX(h.Keys.Time);
    childDraw->AddLine(ImVec2(playheadX, childTop), ImVec2(playheadX, childBottom), textColour, 2.f);
    ImGui::EndChild();

    rootDraw->PushClipRect(ImVec2(headerMin.x, allTop), ImVec2(areaLeft, headerMax.y), true);
    rootDraw->AddText(ImVec2(headerMin.x + fontSize * 0.3f, allTop + fontSize * 0.15f), dimColour, "All bones");
    rootDraw->PopClipRect();

    double steps[] = { 0.05, 0.1, 0.25, 0.5, 1.0, 2.0, 5.0, 10.0, 30.0, 60.0, 300.0, 600.0 };
    double step = 600.0;
    for (int i = 0; i < 12; ++i)
      if (steps[i] / h.Keys.Length * (double)areaWidth >= (double)fontSize * 4.0)
      {
        step = steps[i];
        break;
      }
    for (double time = 0.0; time <= h.Keys.Length + 1e-9; time += step)
    {
      if (rootDraw->VtxBuffer.Size > 60000)
        continue;
      float x = keyX(time);
      if (x < areaLeft - 1.f || x > areaRight + 1.f)
        continue;
      rootDraw->AddLine(ImVec2(x, headerMin.y), ImVec2(x, headerMin.y + fontSize * 0.5f), dimColour, 1.f);
      char label[32];
      if (time < 60.0)
        _snprintf_s(label, sizeof(label), _TRUNCATE, "%gs", time);
      else
      {
        double minutes = time / 60.0;
        if (std::fabs(minutes - std::floor(minutes + 0.5)) < 1e-9)
          _snprintf_s(label, sizeof(label), _TRUNCATE, "%gm", minutes);
        else
          _snprintf_s(label, sizeof(label), _TRUNCATE, "%gs", time);
      }
      float labelWidth = ImGui::CalcTextSize(label).x;
      float labelX = x + 2.f + labelWidth > headerMax.x ? x - 2.f - labelWidth : x + 2.f;
      rootDraw->AddText(ImVec2(labelX, headerMin.y + fontSize * 0.35f), dimColour, label);
    }

    rootDraw->PushClipRect(ImVec2(areaLeft - halfHover, allTop), ImVec2(areaRight + halfHover, headerMax.y), true);
    float lastX = -1e30f;
    for (size_t i = 0; i < g_KeyTimes.size(); ++i)
    {
      float x = keyX(g_KeyTimes[i]);
      if (x < areaLeft - halfHover || x > areaRight + halfHover)
        continue;
      bool hovered = hitDraw.Any && hitDraw.Bone == -2 && hitDraw.Time == g_KeyTimes[i];
      bool atPlayhead = std::fabs(g_KeyTimes[i] - h.Keys.Time) <= pose_keys::TimeEpsilon;
      if (!hovered && !atPlayhead && x - lastX < half)
        continue;
      lastX = x;
      float size = hovered ? halfHover : half;
      ImU32 colour = hovered ? grabActiveColour : (atPlayhead ? textColour : grabColour);
      drawDiamond(rootDraw, x, allTop + allHeight * 0.5f, size, colour, false);
    }
    rootDraw->PopClipRect();

    rootDraw->AddLine(ImVec2(playheadX, headerMin.y), ImVec2(playheadX, headerMax.y), textColour, 2.f);
    rootDraw->AddTriangleFilled(ImVec2(playheadX - fontSize * 0.3f, headerMin.y),
      ImVec2(playheadX + fontSize * 0.3f, headerMin.y), ImVec2(playheadX, headerMin.y + fontSize * 0.5f), textColour);

    if (hitDraw.Any)
    {
      char tooltip[256];
      tooltip[0] = '\0';
      if (hitDraw.Bone == -2)
      {
        int count = 0;
        for (size_t c = 0; c < h.Keys.Channels.size(); ++c)
          if (pose_keys::KeyAt(h.Keys.Channels[c], hitDraw.Time) >= 0)
            ++count;
        _snprintf_s(tooltip, sizeof(tooltip), _TRUNCATE, "%d keyframes at %.2f s", count, hitDraw.Time);
      }
      else
      {
        pose_keys::Channel* channel = pose_keys::Find(h.Keys, hitDraw.Bone);
        int key = channel != nullptr ? pose_keys::KeyAt(*channel, hitDraw.Time) : -1;
        if (key >= 0)
          _snprintf_s(tooltip, sizeof(tooltip), _TRUNCATE, "%s at %.2f s, %s to the next%s", BoneName(h, hitDraw.Bone),
            channel->Keys[(size_t)key].Time, KeyEaseLabel(channel->Keys[(size_t)key].Ease),
            !channel->Keys[(size_t)key].Auto ? "" : (g_AutoKey && h.Keys.On && !PlayingKeys(h) ? "\nHollow: made "
            "with the bone's first keyframe, holding it as it was here (press Keyframe: this bone here, or change it "
            "here, to make it your own)" : "\nHollow: made with the bone's first keyframe, holding it as it was here "
            "(press Keyframe: this bone here to make it your own)"));
      }
      if (tooltip[0] != '\0')
        ImGui::SetTooltip("%s", tooltip);
    }
  }

  // The Reset question (RequestReset), in a popup opened at the Pose window's top level
  void DrawResetAsk()
  {
    ResetAsk const& a = g_ResetAsk;
    Held* found = nullptr;
    for (int i = 0; i < MaxHeld; ++i)
      if (g_Held[i].Used && !g_Held[i].Restoring && g_Held[i].Id == a.HeldId)
        found = &g_Held[i];
    if (!found)
    {
      ImGui::CloseCurrentPopup();
      return;
    }
    Held& h = *found;
    ImGui::PushTextWrapPos(ImGui::GetFontSize() * 26.f);
    if (a.Keyed == 1)
      ImGui::Text("Reset %s: %s has keyframes, and they put it back when the time moves.", a.What,
        BoneName(h, a.First));
    else
      ImGui::Text("Reset %s: %d of these bones have keyframes, and they put them back when the time moves.", a.What,
        a.Keyed);
    ImGui::PopTextWrapPos();
    bool canKey = h.Keys.On && !PlayingKeys(h);
    char label[96];
    _snprintf_s(label, sizeof(label), _TRUNCATE, "Keyframe the reset at %.2f s###PoseResetKey", h.Keys.Time);
    if (!canKey)
      ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.5f);
    bool key = ImGui::Button(label);
    if (!canKey)
      ImGui::PopStyleVar();
    if (!h.Keys.On)
      ImGui::ItemTooltip("The keyframes are off: tick Use the keyframes (Options)");
    else if (PlayingKeys(h))
      ImGui::ItemTooltip("Pause the keyframes first");
    else
      ImGui::ItemTooltip("Resets them and keyframes that at the playhead; their keyframes at other times stay");
    if (key && canKey)
    {
      DoReset(h, true);
      ImGui::CloseCurrentPopup();
    }
    bool remove = ImGui::Button("Reset and remove their keyframes###PoseResetRemove");
    ImGui::ItemTooltip("Resets them and takes all their keyframes away: they stay so at every time");
    if (remove)
    {
      DoReset(h, false);
      ImGui::CloseCurrentPopup();
    }
    if (ImGui::Button("Cancel###PoseResetCancel"))
      ImGui::CloseCurrentPopup();
  }

  void DrawKeyMenu()
  {
    Held* active = g_Active >= 0 && g_Held[g_Active].Used && !g_Held[g_Active].Restoring ? &g_Held[g_Active] : nullptr;
    bool valid = active != nullptr && active->Id == g_KeyMenuHeld;
    if (valid && g_KeyMenuTime >= 0.0)
    {
      if (g_KeyMenuBone == -2)
      {
        bool found = false;
        for (size_t c = 0; c < active->Keys.Channels.size(); ++c)
          if (pose_keys::KeyAt(active->Keys.Channels[c], g_KeyMenuTime) >= 0)
            found = true;
        valid = found;
      }
      else if (g_KeyMenuBone >= 0 && g_KeyMenuBone < active->Bones)
      {
        pose_keys::Channel* channel = pose_keys::Find(active->Keys, g_KeyMenuBone);
        int key = channel != nullptr ? pose_keys::KeyAt(*channel, g_KeyMenuTime) : -1;
        valid = key >= 0;
      }
      else
        valid = false;
    }
    else if (valid)
    {
      if (g_KeyMenuBone < 0 || g_KeyMenuBone >= active->Bones)
        valid = false;
    }

    if (!valid)
    {
      ImGui::CloseCurrentPopup();
    }
    else
    {
      Held& h = *active;
      if (g_KeyMenuTime < 0.0)
      {
        ImGui::TextDisabled("%s", BoneName(h, g_KeyMenuBone));
        if (ImGui::MenuItem("Remove this bone's keyframes"))
        {
          pose_keys::Channel* channel = pose_keys::Find(h.Keys, g_KeyMenuBone);
          if (channel != nullptr)
          {
            PushKeyUndo(h);
            pose_keys::RemoveChannel(h.Keys, g_KeyMenuBone);
            if (h.Keys.Channels.empty())
              h.Keys.Playing = false;
          }
        }
      }
      else if (g_KeyMenuBone == -2)
      {
        ImGui::TextDisabled("All bones, %.2f s", g_KeyMenuTime);
        if (ImGui::MenuItem("Delete the keyframes at this time"))
        {
          PushKeyUndo(h);
          pose_keys::RemoveKeysAt(h.Keys, g_KeyMenuTime);
          if (h.Keys.Channels.empty())
            h.Keys.Playing = false;
        }
        ImGui::Separator();
        bool allEase[pose_keys::Ease_Count];
        for (int e = 0; e < pose_keys::Ease_Count; ++e)
          allEase[e] = true;
        for (size_t c = 0; c < h.Keys.Channels.size(); ++c)
        {
          pose_keys::Channel* channel = pose_keys::Find(h.Keys, h.Keys.Channels[c].Bone);
          int key = channel != nullptr ? pose_keys::KeyAt(*channel, g_KeyMenuTime) : -1;
          if (key >= 0)
            for (int e = 0; e < pose_keys::Ease_Count; ++e)
              if (channel->Keys[(size_t)key].Ease != e)
                allEase[e] = false;
        }
        for (int e = 0; e < pose_keys::Ease_Count; ++e)
          if (ImGui::MenuItem(KeyEaseLabel(e), nullptr, allEase[e]) && !allEase[e])
          {
            PushKeyUndo(h);
            for (size_t c = 0; c < h.Keys.Channels.size(); ++c)
            {
              int bone = h.Keys.Channels[c].Bone;
              pose_keys::Channel* channel = pose_keys::Find(h.Keys, bone);
              int key = channel != nullptr ? pose_keys::KeyAt(*channel, g_KeyMenuTime) : -1;
              if (key >= 0)
                pose_keys::SetEase(h.Keys, bone, key, e);
            }
          }
        ImGui::Separator();
        char copy[96];
        _snprintf_s(copy, sizeof(copy), _TRUNCATE, "Copy to the timeline's time (%.2f s)", h.Keys.Time);
        bool canCopy = std::fabs(h.Keys.Time - g_KeyMenuTime) > pose_keys::TimeEpsilon;
        if (ImGui::MenuItem(copy, nullptr, false, canCopy))
        {
          struct CopyKey
          {
            int Bone;
            float Turn[4];
            float Move[3];
            int Ease;
          };
          std::vector<CopyKey> copies;
          for (size_t c = 0; c < h.Keys.Channels.size(); ++c)
          {
            int bone = h.Keys.Channels[c].Bone;
            pose_keys::Channel* channel = pose_keys::Find(h.Keys, bone);
            int key = channel != nullptr ? pose_keys::KeyAt(*channel, g_KeyMenuTime) : -1;
            if (key >= 0)
            {
              CopyKey copyKey;
              copyKey.Bone = bone;
              memcpy(copyKey.Turn, channel->Keys[(size_t)key].Turn, sizeof(copyKey.Turn));
              memcpy(copyKey.Move, channel->Keys[(size_t)key].Move, sizeof(copyKey.Move));
              copyKey.Ease = channel->Keys[(size_t)key].Ease;
              copies.push_back(copyKey);
            }
          }
          if (!copies.empty())
          {
            PushKeyUndo(h);
            for (size_t i = 0; i < copies.size(); ++i)
              pose_keys::SetKey(h.Keys, copies[i].Bone, h.Keys.Time, copies[i].Turn, copies[i].Move, copies[i].Ease);
          }
        }
      }
      else
      {
        ImGui::TextDisabled("%s, %.2f s", BoneName(h, g_KeyMenuBone), g_KeyMenuTime);
        if (ImGui::MenuItem("Delete keyframe"))
        {
          pose_keys::Channel* channel = pose_keys::Find(h.Keys, g_KeyMenuBone);
          int key = channel != nullptr ? pose_keys::KeyAt(*channel, g_KeyMenuTime) : -1;
          if (key >= 0)
          {
            PushKeyUndo(h);
            pose_keys::RemoveKey(h.Keys, g_KeyMenuBone, key);
            if (h.Keys.Channels.empty())
              h.Keys.Playing = false;
          }
        }
        ImGui::Separator();
        pose_keys::Channel* channel = pose_keys::Find(h.Keys, g_KeyMenuBone);
        int currentKey = channel != nullptr ? pose_keys::KeyAt(*channel, g_KeyMenuTime) : -1;
        int currentEase = currentKey >= 0 ? channel->Keys[(size_t)currentKey].Ease : -1;
        for (int e = 0; e < pose_keys::Ease_Count; ++e)
          if (ImGui::MenuItem(KeyEaseLabel(e), nullptr, currentEase == e) && currentEase != e)
          {
            pose_keys::Channel* found = pose_keys::Find(h.Keys, g_KeyMenuBone);
            int key = found != nullptr ? pose_keys::KeyAt(*found, g_KeyMenuTime) : -1;
            if (key >= 0)
            {
              PushKeyUndo(h);
              pose_keys::SetEase(h.Keys, g_KeyMenuBone, key, e);
            }
          }
        ImGui::Separator();
        char copy[96];
        _snprintf_s(copy, sizeof(copy), _TRUNCATE, "Copy to the timeline's time (%.2f s)", h.Keys.Time);
        bool canCopy = std::fabs(h.Keys.Time - g_KeyMenuTime) > pose_keys::TimeEpsilon;
        if (ImGui::MenuItem(copy, nullptr, false, canCopy))
        {
          pose_keys::Channel* found = pose_keys::Find(h.Keys, g_KeyMenuBone);
          int key = found != nullptr ? pose_keys::KeyAt(*found, g_KeyMenuTime) : -1;
          if (key >= 0)
          {
            float turn[4];
            float move[3];
            int ease = found->Keys[(size_t)key].Ease;
            memcpy(turn, found->Keys[(size_t)key].Turn, sizeof(turn));
            memcpy(move, found->Keys[(size_t)key].Move, sizeof(move));
            PushKeyUndo(h);
            pose_keys::SetKey(h.Keys, g_KeyMenuBone, h.Keys.Time, turn, move, ease);
          }
        }
      }
    }
  }

  void DrawKeyframes(Held& h)
  {
    float fontSize = ImGui::GetFontSize();
    int keyCount = pose_keys::KeyCount(h.Keys);
    bool noKeys = keyCount == 0;
    bool timeLocked = TrackHoldsTime(h);
    bool jumpsLocked = noKeys || timeLocked;
    bool keysOn = h.Keys.On;
    bool playing = PlayingKeys(h);
    bool clipClock = h.Keys.Clock == pose_keys::Clock_Clip && ClipUsable(h);
    bool shownPlaying = clipClock ? (h.Keys.Playing && h.Clip.Playing) : h.Keys.Playing;
    ImGui::SubHeading("KEYFRAMES");
    ImGui::HelpMarker("A keyframe holds a bone's turn and move at a time, over the pose taken or over the animation clip "
      "when one is on. Auto key (red when on): while the keyframes are on and not playing, every change you make is "
      "keyframed at the playhead, from the first one; "
      "off, press Keyframe: the changes. Move the time, pose again: Play goes from one keyframe to the other. A bone with "
      "keyframes follows them when the time moves (a change of it not keyframed goes then); a bone without keyframes "
      "keeps its pose. A bone's first keyframe, made while the pose has keyframes at other times, comes with hollow ones "
      "holding it as it was at the keyframe times around it (Options). Click a keyframe to go to it, drag it to move it "
      "in time, right click for its menu. Undo and Redo take back and put back changes of the pose and of its keyframes "
      "alike. Save keeps the keyframes in a .posekeys file: the one they were last saved in, else (none yet, or after a "
      "Load) it asks where, as Save as does (any folder, the Windows file window). Load puts a file's back (the Files "
      "tab does the same by name, in Cinematic Tools\\Poses). With the Pose window closed, Auto key waits: a change made "
      "on the picture is keyframed once the window is open again (a path dot's drag at once). Hotkeys (by default on the numpad with Num Lock on; change them in the tools' Hotkeys window): in the "
      "buttons' and the Time field's tips. Weapons aren't keyframed; the keyframes go when the pose is let go (save them in Files); the "
      "camera track's clock gives no time after a track ends, the keyframes hold their place then.");
    char status[256];
    status[0] = '\0';
    if (h.Keys.Clock == pose_keys::Clock_Game && h.Keys.Playing && g_ClipGameScale == 0.f)
      _snprintf_s(status, sizeof(status), _TRUNCATE, "Time is frozen: the keyframes wait (Clock: Real time plays with "
        "time frozen)");
    else if (h.Keys.Clock == pose_keys::Clock_Track && g_ClipTrackTime < 0.0)
      _snprintf_s(status, sizeof(status), _TRUNCATE, "Waits for the camera track: play it (CAMERA tab)");
    else if (h.Keys.Clock == pose_keys::Clock_Track && g_ClipTrackTime >= 0.0 && !h.KeyTrackMoving)
      _snprintf_s(status, sizeof(status), _TRUNCATE, "The camera track is paused at %.2f s and holds the time",
        g_ClipTrackTime);
    else if (h.Keys.Clock == pose_keys::Clock_Clip && !ClipUsable(h))
      _snprintf_s(status, sizeof(status), _TRUNCATE, "No clip is on");
    else if (clipClock && TrackHoldsTime(h))
      _snprintf_s(status, sizeof(status), _TRUNCATE, "The clip follows the camera track and holds the time: pause to move it");
    else if (h.Keys.Clock == pose_keys::Clock_Clip && ClipUsable(h) && !h.Clip.Playing)
      _snprintf_s(status, sizeof(status), _TRUNCATE, "The clip is paused");
    if (status[0])
    {
      ImGui::SameLine();
      ImGui::TextColored(ImVec4(1.f, 0.8f, 0.2f, 1.f), "%s", status);
    }

    if (jumpsLocked)
      ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.5f);
    bool press = ImGui::Button("|<");
    if (jumpsLocked)
      ImGui::PopStyleVar();
    if (press && !jumpsLocked)
      SetPlayhead(h, 0.0);
    ImGui::ItemTooltip("To the start");
    SameLineIfFits("<");
    if (jumpsLocked)
      ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.5f);
    press = ImGui::Button("<");
    if (jumpsLocked)
      ImGui::PopStyleVar();
    if (press && !jumpsLocked)
      JumpToKey(h, -1);
    ItemTooltipKey("To the keyframe before", Action::Pose_PreviousKey);
    const char* playLabel = shownPlaying ? "Pause###PoseKeysPlay" : "Play###PoseKeysPlay";
    SameLineIfFits(playLabel);
    bool playLocked = noKeys || h.Keys.Clock == pose_keys::Clock_Track;
    if (playLocked)
      ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.5f);
    press = ImGui::Button(playLabel, ImVec2(fontSize * 4.f, 0.f));
    if (playLocked)
      ImGui::PopStyleVar();
    if (press && !playLocked)
      TogglePlay(h);
    if (playLocked && h.Keys.Clock == pose_keys::Clock_Track && !noKeys)
      ImGui::ItemTooltip("On the camera track's clock the track plays them: play it (CAMERA tab)");
    else
      ItemTooltipKey(shownPlaying ? "Pause" : "Play", Action::Pose_Play);
    SameLineIfFits(">");
    if (jumpsLocked)
      ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.5f);
    press = ImGui::Button(">");
    if (jumpsLocked)
      ImGui::PopStyleVar();
    if (press && !jumpsLocked)
      JumpToKey(h, 1);
    ItemTooltipKey("To the next keyframe", Action::Pose_NextKey);
    SameLineIfFits(">|");
    if (jumpsLocked)
      ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.5f);
    press = ImGui::Button(">|");
    if (jumpsLocked)
      ImGui::PopStyleVar();
    if (press && !jumpsLocked)
      SetPlayhead(h, h.Keys.Length);
    ImGui::ItemTooltip("To the end");

    ImGui::SameLine();
    if (timeLocked)
      ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.5f);
    ImGui::PushItemWidth(fontSize * 5.f);
    float timeValue = (float)h.Keys.Time;
    bool timeChanged = ImGui::DragFloat("Time", &timeValue, 0.01f, 0.f, (float)h.Keys.Length, "%.2f s");
    ImGui::PopItemWidth();
    if (timeLocked)
      ImGui::PopStyleVar();
    {
      std::string back = HotkeyName(Action::Pose_FrameBack), on = HotkeyName(Action::Pose_FrameOn);
      if (!back.empty() || !on.empty())
      {
        char tip[200];
        _snprintf_s(tip, sizeof(tip), _TRUNCATE, "The playhead. One frame back / on (a 30th of a second, while "
          "paused): %s / %s", back.empty() ? "none" : back.c_str(), on.empty() ? "none" : on.c_str());
        ImGui::ItemTooltip(tip);
      }
      else
        ImGui::ItemTooltip("The playhead");
    }
    if (timeChanged && !timeLocked && (!ImGui::IsMouseDown(0) || ImGui::IsMouseDragging(0, 1.f)))
    {
      double t = (double)timeValue, key = 0.0, best = 0.0;
      bool have = false;
      // A keyframe the field would show as this same value: go exactly to the nearest one
      for (double from = t - 0.005 - pose_keys::TimeEpsilon;
           pose_keys::KeyTimeAfter(h.Keys, from, key) && key <= t + 0.005 + pose_keys::TimeEpsilon; from = key)
        if (!have || std::fabs(key - t) < std::fabs(best - t))
        {
          best = key;
          have = true;
        }
      if (have)
        t = best;
      SetPlayhead(h, t);
    }

    // (counted before Length and Loop: their change makes the keyframes not current for a frame, which would read 0)
    int changedCount = 0;
    if (KeysCurrent(h))
    {
      int changedBones[MaxBones];
      changedCount = ChangedBones(h, changedBones);
    }
    ImGui::SameLine();
    ImGui::PushItemWidth(fontSize * 5.f);
    float lengthValue = (float)h.Keys.Length;
    bool lengthTyping = TypingInField();
    bool lengthChanged = ImGui::DragFloat("Length", &lengthValue, 0.05f, 0.1f, 3600.f, "%.2f s");
    bool lengthActive = ImGui::IsItemActive();
    if (lengthActive && !g_LengthWasActive)
      PushKeyUndo(h);
    if (lengthChanged && DragTakes(lengthTyping))   // (a click alone only rounds it)
    {
      double lengthBefore = h.Keys.Length;
      pose_keys::SetLength(h.Keys, (double)lengthValue);
      if (h.Keys.Length != lengthBefore)
        ++h.Keys.Revision;   // the loop's curve depends on it: the pose is made again from the keyframes
    }
    g_LengthWasActive = lengthActive;
    ImGui::PopItemWidth();
    SameLineIfFits("Loop", true);
    if (ImGui::Checkbox("Loop", &h.Keys.Loop))
      ++h.Keys.Revision;   // the curve depends on it (a bone keyed at 0 s and at the end carries on through the wrap)
    ImGui::ItemTooltip("Plays them over and over; a bone keyed at 0 s and at the end carries on through the wrap");
    char changesLabel[64];
    _snprintf_s(changesLabel, sizeof(changesLabel), _TRUNCATE, "the changes (%d)###PoseKeyChanges", changedCount);
    bool autoShown = g_AutoKey;
    bool autoLive = g_AutoKey && keysOn && !playing;   // it keyframes now: not with the keyframes off or playing
    char const* autoSays = !g_AutoKey ? (!keysOn ? "Auto key is off, and the keyframes are off (Options: Use the "
      "keyframes)" : (playing ? "Auto key is off: pause the keyframes, then pose and press Keyframe: the changes (a "
      "change made while they play isn't counted there once the time moves; a bone without keyframes keeps it: pick it "
      "and press Keyframe: this bone)" :
      "Auto key is off: press Keyframe: the changes to keep a change")) :
      (!keysOn ? "Auto key is on, but the keyframes are off (Options: Use the keyframes): changes aren't keyframed now" :
      (playing ? "Auto key waits while the keyframes play: a change now isn't keyframed" :
      "Auto key is on: each change you make is keyframed at the playhead"));
    if (autoShown)
    {
      ImVec4 red = autoLive ? ImVec4(1.f, 0.35f, 0.3f, 1.f) : ImVec4(0.65f, 0.42f, 0.4f, 1.f);
      ImGui::PushStyleColor(ImGuiCol_Text, red);
      ImGui::PushStyleColor(ImGuiCol_CheckMark, red);
    }
    if (ImGui::Checkbox("Auto key###PoseAutoKey", &g_AutoKey))
      g_ConfigWanted = true;
    if (autoShown)
      ImGui::PopStyleColor(2);
    ImGui::ItemTooltip(autoSays);
    SameLineIfFits("Keyframe:");
    ImGui::TextUnformatted("Keyframe:");
    SameLineIfFits(changesLabel);
    bool changesGate = keysOn && !playing && changedCount > 0;
    if (changedCount > 0)
      ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
    if (!changesGate)
      ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.5f);
    bool changes = ImGui::Button(changesLabel);
    if (!changesGate)
      ImGui::PopStyleVar();
    if (changedCount > 0)
      ImGui::PopStyleColor();
    if (changes && changesGate)
      KeyChanges(h);
    if (!keysOn)
      ImGui::ItemTooltip("The keyframes are off: tick Use the keyframes (Options)");
    else if (playing)
      ImGui::ItemTooltip("Pause the keyframes first");
    else if (changedCount > 0)
      ItemTooltipKey("Keyframes them at the playhead. Not keyframed, a change of a bone that has keyframes goes when "
        "the time moves (they put it back); a bone without keyframes keeps it", Action::Pose_KeyChanges);
    else
      ItemTooltipKey(g_AutoKey ? "Nothing to keyframe now: Auto key keyframes each change at once" : "Nothing to "
        "keyframe now: change a bone first", Action::Pose_KeyChanges);

    bool hasSelection = (g_SelectedGroup >= 0 && g_SelectedGroup < FaceGroupCount &&
      h.GroupSize[g_SelectedGroup] > 0) || (g_SelectedBone >= 0 && g_SelectedBone < h.Bones && h.InHeld[g_SelectedBone]);
    bool boneGate = keysOn && !playing && hasSelection;
    SameLineIfFits("this bone");
    if (!boneGate)
      ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.5f);
    bool bone = ImGui::Button("this bone");
    if (!boneGate)
      ImGui::PopStyleVar();
    if (bone && boneGate)
      KeySelection(h);
    if (!keysOn)
      ImGui::ItemTooltip("The keyframes are off: tick Use the keyframes (Options)");
    else if (playing)
      ImGui::ItemTooltip("Pause the keyframes first");
    else if (!hasSelection)
      ImGui::ItemTooltip("Pick a bone or face group first");
    else
      ImGui::ItemTooltip("Keyframes the picked bone (or face group) at the playhead, as it is");

    bool wholeAny = false;
    for (int n = 0; n < h.Count && !wholeAny; ++n)
    {
      int b = (int)h.List[n];
      wholeAny = pose_keys::Find(h.Keys, b) != nullptr || IsEdited(h, b) || KeyRefDiffers(h, b);
    }
    bool wholeGate = keysOn && !playing && wholeAny;
    SameLineIfFits("the whole pose");
    if (!wholeGate)
      ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.5f);
    bool whole = ImGui::Button("the whole pose");
    if (!wholeGate)
      ImGui::PopStyleVar();
    if (whole && wholeGate)
      KeyWholePose(h);
    if (!keysOn)
      ImGui::ItemTooltip("The keyframes are off: tick Use the keyframes (Options)");
    else if (playing)
      ImGui::ItemTooltip("Pause the keyframes first");
    else if (!wholeAny)
      ImGui::ItemTooltip("Nothing is posed yet: change a bone first, or pick a bone and press this bone");
    else
      ItemTooltipKey("Keyframes every bone that is posed or has keyframes, at the playhead", Action::Pose_KeyWholePose);

    int selected = SelectedKeysAtPlayhead(h);
    bool removeGate = keysOn && !playing && selected > 0;
    if (!removeGate)
      ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.5f);
    bool remove = ImGui::Button("Remove keyframe");
    if (!removeGate)
      ImGui::PopStyleVar();
    if (remove && removeGate)
      RemoveSelectedKeys(h);
    if (!keysOn)
      ImGui::ItemTooltip("The keyframes are off: tick Use the keyframes (Options)");
    else if (playing)
      ImGui::ItemTooltip("Pause the keyframes first");
    else if (selected == 0)
      ImGui::ItemTooltip("Pick a bone that has a keyframe at the playhead");
    else
      ItemTooltipKey("Removes the picked bone's keyframe at the playhead", Action::Pose_RemoveKey);

    DrawUndoButtons(h, "PoseKeys");
    // The keyframes in a file (2026-11-16, the user: "also make it possible to save the keyframes to a file"): Save
    // into the file they were last saved in by hand (none, or after a load: as Save as), Save as (the Windows file
    // window: any folder), Load (a file of Cinematic Tools\Poses, or another through the file window). The window and
    // the file's writing or reading go on a thread of their own (AskKeyFile).
    {
      int busy = KeyFileBusy();
      bool asking = busy >= 0;
      bool canSave = pose_keys::KeyCount(h.Keys) > 0 || AnyEdited(h);
      char const* askingSays = busy == KeyFile_Save ? "The keyframes are being saved" :
        "A file window is open: if it isn't in front, it's behind the game (Alt+Tab to it)";
      std::string fileName = h.KeysFile.empty() ? std::string() : FileNameOf(h.KeysFile);
      auto seen = [](char const* what)
      {
        if (g_KeyFileItemStub)
          g_KeyFileItemStub(what, ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
      };
      SameLineIfFits("Save##PoseKeysBarSave");
      bool gate = canSave && !asking;
      if (!gate)
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.5f);
      bool press = ImGui::Button("Save##PoseKeysBarSave");
      if (!gate)
        ImGui::PopStyleVar();
      seen("Save");
      if (press && gate)
        AskKeyFile(h, h.KeysFile.empty() ? KeyFile_SaveAs : KeyFile_Save);
      if (asking)
        ImGui::ItemTooltip(askingSays);
      else if (!canSave)
        ImGui::ItemTooltip("Nothing to save yet: pose it or keyframe it first");
      else if (h.KeysFile.empty())
        ImGui::ItemTooltip("Saves the keyframes in a file: where, the file window asks first (as Save as). After a Load "
          "it asks too, so a loaded file isn't written over by mistake.");
      else
      {
        std::string tip = "Saves the keyframes into " + Utf8Of(h.KeysFile) + ", the file they were last saved in "
          "(Save as: another file)";
        ImGui::ItemTooltip(tip.c_str());
      }
      SameLineIfFits("Save as...##PoseKeysBarSaveAs");
      if (!gate)
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.5f);
      press = ImGui::Button("Save as...##PoseKeysBarSaveAs");
      if (!gate)
        ImGui::PopStyleVar();
      seen("Save as");
      if (press && gate)
        AskKeyFile(h, KeyFile_SaveAs);
      ImGui::ItemTooltip(asking ? askingSays : (!canSave ? "Nothing to save yet: pose it or keyframe it first" :
        "Saves the keyframes in a .posekeys file you pick, in any folder (the Windows file window); Save writes into it "
        "next. Load puts them back on a pose with those bones' names (on another skeleton, the bones' turns only)."));
      SameLineIfFits("Load##PoseKeysBarLoad");
      if (asking)
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.5f);
      press = ImGui::Button("Load##PoseKeysBarLoad");
      if (asking)
        ImGui::PopStyleVar();
      seen("Load");
      if (press && !asking)
      {
        ListKeyFiles();
        g_KeyFilesListedAt = GetTickCount64();
        ImGui::OpenPopup("##PoseKeysBarLoadMenu");
      }
      {
        char tip[240];
        _snprintf_s(tip, sizeof(tip), _TRUNCATE, "Loads keyframes onto %s: a file of Cinematic Tools\\Poses, or another "
          "one anywhere (the file window). They replace its keyframes; Undo takes a load back.", h.Label);
        ImGui::ItemTooltip(asking ? askingSays : tip);
      }
      if (ImGui::BeginPopup("##PoseKeysBarLoadMenu"))
      {
        if (g_KeyFiles.empty())
          ImGui::TextDisabled("No keyframes files in Cinematic Tools\\Poses yet");
        for (std::wstring const& file : g_KeyFiles)
        {
          std::string name = Utf8Of(file), label = name + "##posekeysbarfile";
          bool picked = ImGui::Selectable(label.c_str(), false);
          seen(name.c_str());
          if (picked)
          {
            std::wstring path = PoseKeysPath(file);
            if (LoadKeysFromPath(h, path))
            {
              h.KeysFile.clear();   // (Save asks after a load: a loaded file isn't written over by mistake)
              h.KeysSuggest = path;
            }
            SayKeyNote(h, g_FileMessage);
          }
        }
        ImGui::Separator();
        bool other = ImGui::Selectable("Another file...##posekeysbarother", false);
        seen("Another file");
        if (other && !KeyFileWindowOpen())
          AskKeyFile(h, KeyFile_Load);
        ImGui::EndPopup();
      }
      if (!fileName.empty() && !asking)
      {
        // (which file Save writes: shown in its tip; here only when the line has room)
        std::string shown = "(" + fileName + ")";
        SameLineIfFits(shown.c_str());
        ImGui::TextDisabled("%s", shown.c_str());
      }
    }
    {
      ImGuiStyle const& style = ImGui::GetStyle();
      float comboWidth = fontSize * 11.f + style.ItemInnerSpacing.x + ImGui::CalcTextSize("Clock").x;
      if (ImGui::GetItemRectMax().x + style.ItemSpacing.x + comboWidth <=
          ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x)
        ImGui::SameLine();
    }
    ImGui::PushItemWidth(fontSize * 11.f);
    const char* clocks[] = { "Game time (Game speed; stops with Freeze time)", "Real time (plays with time frozen too)",
      "The camera track's time", "The animation clip's time" };
    int clock = h.Keys.Clock;
    if (ImGui::Combo("Clock", &clock, clocks, 4))
    {
      h.Keys.Clock = clock;
      if (clock == pose_keys::Clock_Track)
      {
        h.Keys.Speed = 1.f;
        h.Keys.Playing = true;
      }
      else if (clock == pose_keys::Clock_Clip)
      {
        h.Keys.Speed = 1.f;
        h.Keys.Start = 0.0;
        h.Keys.Playing = true;
        if (h.Clip.Clip)
        {
          double length = h.Clip.Clip->Length();
          if (pose_keys::LastKeyTime(h.Keys) <= length)
          {
            double lengthBefore = h.Keys.Length;
            pose_keys::SetLength(h.Keys, length);
            if (h.Keys.Length != lengthBefore)
              ++h.Keys.Revision;   // (the loop's curve depends on it)
          }
        }
      }
      else
        h.Keys.Playing = false;
    }
    ImGui::PopItemWidth();
    // What the last keyframing did (bright a while, then dim; the line stays so the timeline doesn't jump)
    ImGui::PushTextWrapPos(0.f);
    if (g_KeyNote[0] && g_KeyNoteHeld == h.Id)
    {
      if (GetTickCount64() - g_KeyNoteAt < KeyNoteMs)
        ImGui::TextColored(ImVec4(1.f, 0.6f, 0.55f, 1.f), "%s", g_KeyNote);
      else
        ImGui::TextDisabled("%s", g_KeyNote);
    }
    else
      ImGui::TextDisabled("%s", autoSays);
    ImGui::PopTextWrapPos();


    if (noKeys)
    {
      CancelKeyMouse();
      ImGui::PushTextWrapPos(0.f);
      if (g_AutoKey && h.Keys.On)
        ImGui::TextDisabled("No keyframes yet. Auto key is on: pose it and each change is keyframed at this time. Move "
          "the time, pose again: Play goes from one to the other.");
      else if (g_AutoKey)
        ImGui::TextDisabled("No keyframes yet. Auto key is on, but the keyframes are off: tick Use the keyframes "
          "(Options) to keyframe the changes.");
      else if (!h.Keys.On)
        ImGui::TextDisabled("No keyframes yet. The keyframes are off: tick Use the keyframes (Options), then pose it "
          "and press Keyframe: the changes (or tick Auto key).");
      else
        ImGui::TextDisabled("No keyframes yet. Pose it, then press Keyframe: the changes (or tick Auto key). Move the "
          "time, pose again, keyframe again: Play goes from one to the other.");
      ImGui::PopTextWrapPos();
    }
    else
      DrawKeyTimeline(h);
  }
}

// [CPU] The game's pose blend hooked at the tools' start, on the init thread with the other hooks (Main::Init), and
// not at the first Take on the game's thread: that frame took 45 - 53 ms (measured in CT.log; that making the hook,
// MinHook's thread snapshot and freeze, is why is inferred: Research_CPU_Memory_2026-11-13 section 2.3). Until a pose
// is taken the hook only passes the call on: about 9 - 12 ns more a call, measured offline with the tools' own MinHook
// on a copy of the blend's entry (scratchpad voice\hitch; how many calls the game makes a frame wasn't measured).
void pose_editor::Init()
{
  BlendHook();
}

void pose_editor::OnViewWritten()
{
  // (with the menu open: the dots; closed too while a pose looks at the camera: Look at, 2026-11-16)
  if (g_shutdown || !g_mainHandle || !g_mainHandle->GetUI() || (!g_mainHandle->GetUI()->IsEnabled() &&
      !g_LookCameraWanted.load(std::memory_order_relaxed)) || !Available() || !g_ppGameRenderer)
    return;
  View view{};
  view.Ok = ReadMainView(view.T, view.P);
  std::lock_guard<std::mutex> lock(g_ViewMutex);
  g_View = view;
}

void pose_editor::Update(__int64 soldier, bool replayBusy, bool timeFrozen, float gameTimeScale, double trackTime)
{
  // Her hair while frozen (its own checks of the game's code)
  paused_hair::Update(soldier, timeFrozen, g_HairWhilePaused);
  if (!Available())
    return;
  std::lock_guard<std::mutex> lock(g_Mutex);
  ULONGLONG now = GetTickCount64();
  // The clips' clocks: this frame's real time (the performance counter), the
  // game's time scale, the camera track's time
  {
    static LARGE_INTEGER frequency{}, last{};
    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);
    if (!frequency.QuadPart)
      QueryPerformanceFrequency(&frequency);
    double dt = last.QuadPart && frequency.QuadPart ? (double)(counter.QuadPart - last.QuadPart) / frequency.QuadPart : 0.0;
    last = counter;
    g_ClipCounterSeconds = frequency.QuadPart ? (double)counter.QuadPart / (double)frequency.QuadPart : 0.0;
    g_ClipRealDtBefore = g_ClipRealDt;
    g_ClipRealDt = dt < 0.0 ? 0.0 : (dt > 0.25 ? 0.25 : dt);
    g_ClipLongFrames = g_ClipRealDt > 0.1 ? (g_ClipLongFrames < 1000 ? g_ClipLongFrames + 1 : 1000) : 0;
    g_ClipGameScale = gameTimeScale >= 0.f && gameTimeScale < 100.f ? gameTimeScale : 1.f;
    g_ClipTrackTime = trackTime;
  }
  KeepClipLoad();
  KeepVoiceLoad();

  // What the menu asked for
  if (g_ReleaseRequest != -2)
  {
    for (int i = 0; i < MaxHeld; ++i)
      if (g_Held[i].Used && (g_ReleaseRequest == -1 || g_ReleaseRequest == i))
        Release(g_Held[i], "the menu's Let go");
    g_ReleaseRequest = -2;
  }
  if (g_TakeRequest)
  {
    g_TakeRequest = false;
    Take(g_TakeWhat, soldier);
  }

  // A bone dragged with "Bones stop at walls": solved here, where the world's
  // sweeps can be made, before its pose is handed
  g_WallsReady = collision::Available();
  if (g_StopAtWalls && g_WallsReady && g_Move.On && g_Move.Held >= 0 && g_Move.Held < MaxHeld && g_Held[g_Move.Held].Used)
    DragWithWalls(g_Held[g_Move.Held], g_Move.Held);

  for (int i = 0; i < MaxHeld; ++i)
    if (g_Held[i].Used)
      KeepHeld(g_Held[i], soldier, replayBusy, timeFrozen, now);
  // (Look at the camera: the camera's place is read with the menu closed too, while filming: OnViewWritten)
  bool lookCamera = false;
  for (int i = 0; i < MaxHeld; ++i)
    lookCamera |= g_Held[i].Used && g_Held[i].LookMode == Look_Camera;
  g_LookCameraWanted.store(lookCamera, std::memory_order_relaxed);

  // Her hair while she's posed: reset when her pose jumps (PausedHair.h, KeepReset), and a read-only log with
  // [Debug] Diagnostics (Watch), with her pose's clip and head
  {
    paused_hair::PoseState pose;
    void const* clipNow = nullptr;
    bool keysNow = false;
    double keysTime = 0.0;
    for (int i = 0; i < MaxHeld; ++i)
    {
      Held const& h = g_Held[i];
      // (not while her pose isn't handed: a replay's or another override's is drawn)
      if (!h.Used || h.Kind != Kind_Faith || h.Entity != soldier || h.Restoring || h.Aside || h.Refused)
        continue;
      pose.Held = true;
      ClipPlay const& c = h.Clip;
      pose.ClipOn = c.On && c.Clip && c.Fit.Ok && c.LocalOk;
      pose.Playing = pose.ClipOn && c.Playing;
      if (c.Clip)
      {
        strncpy_s(pose.File, c.Clip->File.c_str(), _TRUNCATE);
        pose.Fps = c.Clip->Fps;
      }
      clipNow = pose.ClipOn ? c.Clip.get() : nullptr;
      pose.Time = c.Time;
      keysNow = h.Keys.On && !h.Keys.Channels.empty();
      keysTime = h.Keys.Time;
      int head = BoneByName(h.Skeleton, "Head");
      if (h.WorldOk && head >= 0 && head < h.Bones)
      {
        memcpy(pose.Head, h.World[head], sizeof(pose.Head));
        pose.HeadOk = true;
      }
      break;
    }
    // A jump: the clip's loop or a new time (back, or on by more than 0.25 s), a clip put on, taken off or another,
    // the keyframes' playhead doing the same, with her head moved 10 cm or more in the frame; or her head moved
    // 50 cm or more in one frame, whatever did it
    static paused_hair::PoseState last;
    static void const* lastClip = nullptr;
    static bool lastKeysNow = false;
    static double lastKeysTime = 0.0;
    bool jump = false;
    if (pose.Held && last.Held && pose.HeadOk && last.HeadOk)
    {
      float d[3] = { pose.Head[0] - last.Head[0], pose.Head[1] - last.Head[1], pose.Head[2] - last.Head[2] };
      float moved = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
      bool keysJump = keysNow && lastKeysNow &&
        (keysTime < lastKeysTime - 1e-6 || keysTime > lastKeysTime + 0.25);
      bool clipJump = pose.ClipOn != last.ClipOn || clipNow != lastClip ||
        (pose.ClipOn && (pose.Time < last.Time - 1e-6 || pose.Time > last.Time + 0.25)) || keysJump;
      jump = (clipJump && moved >= 0.10f) || moved >= 0.5f;
    }
    // Let go (or stepped aside) from a clip's pose: the game's own pose takes over at once, maybe metres away
    if (last.Held && !pose.Held && last.ClipOn)
      jump = true;
    last = pose;
    lastClip = clipNow;
    lastKeysNow = keysNow;
    lastKeysTime = keysTime;
    paused_hair::KeepReset(soldier, jump, gameTimeScale);
    paused_hair::Watch(soldier, timeFrozen, pose);
  }

  // How often the game tried to update or move the weapons the tools hold
  static ULONGLONG weaponsLoggedAt = 0;
  if (now - weaponsLoggedAt >= CheckLogMs)
  {
    weaponsLoggedAt = now;
    int moves = util::hooks::PoseActorMovesHeld();
    if (moves)
      util::log::Write("Pose: the game's own moves of posed people's weapons were held back %d times in the last %.0f s "
        "(time %s)", moves, CheckLogMs / 1000.0, timeFrozen ? "frozen" : "running");
  }

  // The list, while the menu shows it
  if (now - g_ListWantedAt <= ListWantedMs && now - g_ScannedAt >= ScanPeriodMs)
    ScanCandidates(soldier, now);
}

void pose_editor::DrawUI()
{
  if (!Available())
  {
    ImGui::TextDisabled("%s", g_Unavailable[0] ? g_Unavailable : "Not available (see CT.log)");
    return;
  }
  std::lock_guard<std::mutex> lock(g_Mutex);
  ImGui::PushTextWrapPos(0.f);
  ImGui::TextDisabled("Faith, KrugerSec and other people posed bone by bone, with keyframes and animation clips.");
  ImGui::PopTextWrapPos();
  if (ImGui::Button(g_WindowOpen ? "Show the Pose window" : "Open the Pose window..."))
  {
    g_WindowOpen = true;
    g_WindowFocus = true;
  }
  std::string status;
  std::string labels;
  int held = 0;
  bool keysPlaying = false, clipPlaying = false, voicePlaying = false;
  for (int i = 0; i < MaxHeld; ++i)
    if (g_Held[i].Used)
    {
      ++held;
      if (!labels.empty())
        labels += ", ";
      labels += g_Held[i].Label;
      if (PlayingKeys(g_Held[i]))
        keysPlaying = true;
      if (g_Held[i].Clip.On && g_Held[i].Clip.Playing)
        clipPlaying = true;
      if (g_Held[i].Voice.Sound && g_Held[i].Voice.On && g_Held[i].Voice.Moving)
        voicePlaying = true;
    }
  if (held == 0)
    status = "No pose held";
  else
  {
    status = std::to_string(held) + " held: " + labels;
    if (keysPlaying)
      status += " (keyframes playing)";
    if (clipPlaying)
      status += " (clip playing)";
    if (voicePlaying)
      status += " (voice playing)";
  }
  ImGui::PushTextWrapPos(0.f);
  ImGui::TextDisabled("%s", status.c_str());
  ImGui::PopTextWrapPos();
}

void pose_editor::HotkeyUpdate()
{
  if (g_shutdown || !g_mainHandle || !g_mainHandle->GetUI() || !g_mainHandle->GetUI()->IsEnabled() || !Available())
    return;
  std::lock_guard<std::mutex> lock(g_Mutex);
  PoseHotkeys();
}

void pose_editor::DrawWindow()
{
  std::lock_guard<std::mutex> lock(g_Mutex);
  g_WindowRectOk = false;
  if (!ImGui::IsMouseDown(0) &&
      (!g_WindowOpen || !Available() || g_Active < 0 || g_Active >= MaxHeld || !g_Held[g_Active].Used ||
       g_Held[g_Active].Restoring || pose_keys::KeyCount(g_Held[g_Active].Keys) == 0))
    CancelKeyMouse();
  if (!g_WindowOpen)
    return;

  float fontSize = ImGui::GetFontSize();
  ImGuiIO& io = ImGui::GetIO();
  float width = io.DisplaySize.x >= fontSize * 110.f ? fontSize * 46.f : fontSize * 40.f;
  float height = (std::min)(fontSize * 44.f, io.DisplaySize.y - fontSize * 4.f);
  ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - width - fontSize * 2.f, fontSize * 2.f), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowSize(ImVec2(width, height), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowSizeConstraints(ImVec2((std::min)(fontSize * 40.f, io.DisplaySize.x - fontSize * 2.f),
    (std::min)(fontSize * 36.f, io.DisplaySize.y - fontSize * 2.f)), io.DisplaySize);
  if (g_WindowFocus)
  {
    ImGui::SetNextWindowFocus();
    ImGui::SetNextWindowCollapsed(false);
    g_WindowFocus = false;
  }

  bool open = true;
  bool configChanged = false;
  if (ImGui::Begin("Pose###PoseWindow", &open))
  {
    ImVec2 pos = ImGui::GetWindowPos();
    ImVec2 size = ImGui::GetWindowSize();
    g_WindowMin = pos;
    g_WindowMax = ImVec2(pos.x + size.x, pos.y + size.y);
    g_WindowRectOk = true;
    g_ListWantedAt = GetTickCount64();
    if (g_KeyMenuOpen)
    {
      ImGui::OpenPopup("##PoseKeyMenu");
      g_KeyMenuOpen = false;
    }
    if (ImGui::BeginPopup("##PoseKeyMenu"))
    {
      DrawKeyMenu();
      ImGui::EndPopup();
    }
    if (g_ResetAskOpen)
    {
      ImGui::OpenPopup("##PoseResetAsk");
      g_ResetAskOpen = false;
    }
    if (ImGui::BeginPopup("##PoseResetAsk"))
    {
      DrawResetAsk();
      ImGui::EndPopup();
    }
    if (!Available())
    {
      ImGui::PushTextWrapPos(0.f);
      ImGui::TextDisabled("%s", g_Unavailable[0] ? g_Unavailable : "Not available (see CT.log)");
      ImGui::PopTextWrapPos();
    }
    else
    {
      Held* active = DrawWho();
      if (active)
      {
        DrawPoseMiddle(*active, configChanged);
        DrawKeyframes(*active);
      }
    }
  }
  else
  {
    if (!ImGui::IsMouseDown(0))
      CancelKeyMouse();
    ImVec2 pos = ImGui::GetWindowPos();
    ImVec2 size = ImGui::GetWindowSize();
    g_WindowMin = pos;
    g_WindowMax = ImVec2(pos.x + size.x, pos.y + size.y);
    g_WindowRectOk = true;
  }
  ImGui::End();
  if (open && !ImGui::IsAnyItemActive() && !g_KeyDrag.On && !g_KeyDrag.Pending && !g_KeyScrub &&
      !g_Drag.On && !g_Move.On && !g_Move.Pending)
    AutosaveKeysNow();
  g_WindowOpen = open;
  configChanged |= g_ConfigWanted;
  g_ConfigWanted = false;
  if (configChanged && g_mainHandle)
    g_mainHandle->OnConfigChanged();
}

void pose_editor::QuietMouse()
{
  QuietMouseAfterWindow();
}

void pose_editor::DrawOverlay()
{
  if (!Available())
    return;
  std::lock_guard<std::mutex> lock(g_Mutex);
  ImGuiIO& io = ImGui::GetIO();
  // The keyframes' file (2026-11-16): a press as its window closed not taken; its answer taken (saved or loaded)
  QuietMouseAfterWindow();
  TakeKeyFileAnswer();
  Held* active = g_Active >= 0 && g_Held[g_Active].Used ? &g_Held[g_Active] : nullptr;
  if (g_Drag.On && (!io.MouseDown[0] || !active || g_Drag.Held != g_Active))
    g_Drag.On = false;
  // A held dot let go; a path's dot after a drag: its change keyframed at its time (2026-11-16)
  auto endMove = []()
  {
    if (g_Move.On && g_Move.Trail && g_Move.Held >= 0 && g_Move.Held < MaxHeld && g_Held[g_Move.Held].Used &&
        !g_Held[g_Move.Held].Restoring)
      KeyTrailDrag(g_Held[g_Move.Held], g_Move.Pose, g_Move.TrailMirror);
    g_Move.Pending = g_Move.On = false;
  };
  if ((g_Move.Pending || g_Move.On) && (!io.MouseDown[0] || !active || g_Move.Held != g_Active))
    endMove();
  AutoKeyChanges();
  for (int i = 0; i < MaxHeld; ++i)
    SettleLastStep(g_Held[i]);
  // (the Pose window closed: its own autosave doesn't run, and a path dot's drag keyframes all the same)
  if (!g_WindowOpen && !g_Drag.On && !g_Move.On && !g_Move.Pending)
    AutosaveKeysNow();
  if (!active || !g_ShowBones || !active->WorldOk)
  {
    if (g_Move.Pending || g_Move.On)
      endMove();
    return;
  }
  View view;
  {
    std::lock_guard<std::mutex> viewLock(g_ViewMutex);
    view = g_View;
  }
  if (!view.Ok)
    return;
  Held& h = *active;
  float fontSize = ImGui::GetFontSize();
  bool clipOverWindow = g_WindowOpen && g_WindowRectOk;
  ImVec2 clipMin, clipMax;
  if (clipOverWindow)
  {
    float grow = fontSize * 0.5f;
    clipMin = ImVec2(g_WindowMin.x - grow, g_WindowMin.y - grow);
    clipMax = ImVec2(g_WindowMax.x + grow, g_WindowMax.y + grow);
  }
  auto overWindow = [&](ImVec2 p)
  {
    return clipOverWindow && p.x >= clipMin.x && p.x <= clipMax.x && p.y >= clipMin.y && p.y <= clipMax.y;
  };

  // The ring held with the mouse turns its bone, or each of its face group's
  // bones about its own place: by the angle the cursor went along the ring
  // where it was taken
  if (g_Drag.On)
  {
    float tx = g_Drag.Tangent[0], ty = g_Drag.Tangent[1];
    float tt = tx * tx + ty * ty;
    float angle = tt > 1e-6f ? ((io.MousePos.x - g_Drag.Mouse[0]) * tx + (io.MousePos.y - g_Drag.Mouse[1]) * ty) / tt : 0.f;
    angle = angle > 6.2831853f ? 6.2831853f : (angle < -6.2831853f ? -6.2831853f : angle);
    float q[4];
    QAxis(g_Drag.Axis, angle, q);
    // turn = parent^-1 * q * parent * turn when taken
    auto turnOne = [&](int bone, float const* parent, float const* before)
    {
      if (angle == 0.f)
      {
        memcpy(h.Turn[bone], before, sizeof(h.Turn[bone]));   // not turned: as taken, to the bit
        return;
      }
      float inverse[4], a[4], b[4];
      QConj(parent, inverse);
      QMul(inverse, q, a);
      QMul(a, parent, b);
      QMul(b, before, h.Turn[bone]);
      QNorm(h.Turn[bone]);
    };
    if (g_Drag.Weapon >= 0 && g_Drag.Weapon < h.WeaponCount && h.Weapons[g_Drag.Weapon].Bone != -1)
    {
      // Its edit's turn, in its frame: frame^-1 * q * frame * the turn when taken
      float inverse[4], a[4], b[4];
      float* turn = h.Weapons[g_Drag.Weapon].EditTurn;
      if (angle == 0.f)
        memcpy(turn, g_Drag.Turn, 4 * sizeof(float));   // not turned: as taken, to the bit
      else
      {
        QConj(g_Drag.Parent, inverse);
        QMul(inverse, q, a);
        QMul(a, g_Drag.Parent, b);
        QMul(b, g_Drag.Turn, turn);
        QNorm(turn);
      }
    }
    else if (g_Drag.Group >= 0 && g_Drag.Group < FaceGroupCount)
    {
      for (int k = 0; k < h.GroupSize[g_Drag.Group]; ++k)
        turnOne(h.GroupMembers[g_Drag.Group][k], g_Drag.GroupParents[k], g_Drag.GroupTurns[k]);
    }
    else if (g_Drag.Bone >= 0 && g_Drag.Bone < h.Bones)
    {
      // (2026-11-16: from the pose when taken each frame, the pinned hands and feet, and the feet when it's the Hips,
      // kept where they are: HoldAround)
      if (g_Drag.Pose.Turns.size() == (size_t)h.Bones * 4 && g_Drag.Pose.Moves.size() == (size_t)h.Bones * 3)
        ApplySnapshot(h, g_Drag.Pose);
      HoldAround(h, g_Drag.Bone, [&]() { turnOne(g_Drag.Bone, g_Drag.Parent, g_Drag.Turn); });
    }
  }

  // A bone's or a face group's dot held with the mouse: it's moved once the
  // cursor has gone a few pixels (a click alone only picks it), to the point
  // under the cursor at its distance from the camera (the wheel: nearer or
  // farther), each frame from the pose it was pressed in
  bool moveWeapon = g_Move.Weapon >= 0 && g_Move.Weapon < h.WeaponCount && h.Weapons[g_Move.Weapon].Bone != -1;
  bool moveThing = (g_Move.Bone >= 0 && g_Move.Bone < h.Bones) || (g_Move.Group >= 0 && g_Move.Group < FaceGroupCount) ||
    moveWeapon;
  if (g_Move.Pending && moveThing)
  {
    float dx = io.MousePos.x - g_Move.Mouse[0], dy = io.MousePos.y - g_Move.Mouse[1];
    if (dx * dx + dy * dy >= MoveStartPixels * MoveStartPixels)
    {
      g_Move.Pending = false;
      g_Move.On = true;
      PushUndo(h, g_Move.Pose);
      g_Move.Accepted = g_Move.Pose;
      // (a path's dot: the bone's real place when pressed, which its dot needn't be; the second review, 2026-11-16)
      memcpy(g_Move.AcceptedPlace, g_Move.Trail ? g_Move.PressedPlace : g_Move.Start, sizeof(g_Move.AcceptedPlace));
      g_Move.WantOk = false;
    }
  }
  bool poseOk = g_Move.Pose.Turns.size() == (size_t)h.Bones * 4 && g_Move.Pose.Moves.size() == (size_t)h.Bones * 3;
  // A right click while one's held puts it back as it was pressed
  if ((g_Move.On || g_Move.Pending) && io.MouseClicked[1])
  {
    if (g_Move.On && poseOk)
    {
      ApplySnapshot(h, g_Move.Pose);
      DropLastStep(h);   // the drag's own step (pushed when it started), and the Redo it cleared put back
      h.HistoryHasKeys = false;
    }
    g_Move.Pending = g_Move.On = false;
    io.MouseClicked[1] = false;
  }
  if (g_Move.On && moveThing && poseOk)
  {
    // Shift: up and down move it farther and nearer along the view, from
    // where it is when Shift goes down; let go, across the picture again
    // from there (no jump either way)
    bool depth = !g_Move.OnAxis && (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    if (depth != g_Move.Depth)
    {
      float now[3];
      memcpy(now, g_Move.TargetOk ? g_Move.Target : g_Move.Start, sizeof(now));
      if (depth)
      {
        memcpy(g_Move.DepthBase, now, sizeof(now));
        g_Move.DepthMouseY = io.MousePos.y;
      }
      else
      {
        float x, y;
        memcpy(g_Move.Base, now, sizeof(now));
        if (ToScreen(view, now, x, y))
        {
          g_Move.Offset[0] = x - io.MousePos.x;
          g_Move.Offset[1] = y - io.MousePos.y;
        }
      }
      g_Move.Depth = depth;
    }
    // The wheel: nearer or farther too
    if (io.MouseWheel != 0.f && !g_Move.OnAxis)
    {
      float* base = g_Move.Depth ? g_Move.DepthBase : g_Move.Base;
      float away[3] = { base[0] - view.T[12], base[1] - view.T[13], base[2] - view.T[14] };
      float distance = Length3(away);
      if (distance > 1e-3f)
      {
        float wanted = distance * powf(MoveWheelStep, io.MouseWheel);
        wanted = wanted < 0.3f ? 0.3f : (wanted > 500.f ? 500.f : wanted);
        for (int k = 0; k < 3; ++k)
          base[k] = view.T[12 + k] + away[k] * (wanted / distance);
      }
      io.MouseWheel = 0.f;
    }
    float target[3];
    if (g_Move.OnAxis)
    {
      // Along its arrow: as far as the cursor went along the arrow's way on the
      // screen, in the arrow's metres
      float ax = g_Move.AxisScreen[0], ay = g_Move.AxisScreen[1];
      float aa = ax * ax + ay * ay;
      float along = aa > 1e-4f ? ((io.MousePos.x - g_Move.Mouse[0]) * ax + (io.MousePos.y - g_Move.Mouse[1]) * ay) / aa *
        g_Move.AxisLength : 0.f;
      along = along > 5.f ? 5.f : (along < -5.f ? -5.f : along);
      for (int k = 0; k < 3; ++k)
        target[k] = g_Move.Start[k] + g_Move.Axis[k] * along;
      g_Move.TargetOk = true;
    }
    else if (g_Move.Depth)
    {
      float forward[3] = { -view.T[8], -view.T[9], -view.T[10] };
      Normalize3(forward);
      float off[3] = { g_Move.DepthBase[0] - view.T[12], g_Move.DepthBase[1] - view.T[13], g_Move.DepthBase[2] - view.T[14] };
      float depthNow = off[0] * forward[0] + off[1] * forward[1] + off[2] * forward[2];
      depthNow = depthNow < 0.3f ? 0.3f : depthNow;
      float height = ImGui::GetIO().DisplaySize.y;
      // A pixel up is as far as a pixel across the picture there, times DepthSpeed
      float perPixel = fabsf(view.P[5]) > 1e-4f && height > 0.f ? depthNow / (fabsf(view.P[5]) * height * 0.5f) : 0.f;
      float along = (g_Move.DepthMouseY - io.MousePos.y) * perPixel * DepthSpeed;
      along = along < 0.3f - depthNow ? 0.3f - depthNow : along;   // not behind or into the camera
      for (int k = 0; k < 3; ++k)
        target[k] = g_Move.DepthBase[k] + forward[k] * along;
      g_Move.TargetOk = true;
    }
    else
      g_Move.TargetOk = PlanePoint(view, g_Move.Base, io.MousePos.x + g_Move.Offset[0], io.MousePos.y + g_Move.Offset[1],
        target);
    // The eyes look (not with Free move: then they're moved like any bone)
    bool eyes = !g_FreeMove && ((g_Move.Group >= 0 && g_Move.Group == EyesGroup) || EyeOf(h, g_Move.Bone) >= 0);
    if (g_Move.TargetOk && moveWeapon)
    {
      // A weapon: its edit's move, in its frame (it isn't stopped by walls)
      memcpy(g_Move.Target, target, sizeof(target));
      ApplySnapshot(h, g_Move.Pose);
      HeldWeapon& w = h.Weapons[g_Move.Weapon];
      float const* frame = g_Move.WeaponFrame;
      float inverse[4], away[3] = { target[0] - frame[0], target[1] - frame[1], target[2] - frame[2] }, inFrame[3];
      QConj(frame + 4, inverse);
      QRot(inverse, away, inFrame);
      for (int k = 0; k < 3; ++k)
      {
        float m = inFrame[k] - w.OffsetLocal[k];
        w.EditMove[k] = std::isfinite(m) ? m : 0.f;
      }
    }
    else if (g_Move.TargetOk && g_StopAtWalls && g_WallsReady && g_Move.Group < 0 && !eyes)
    {
      // Bones stop at walls: the game's thread moves it (DragWithWalls)
      memcpy(g_Move.Want, target, sizeof(target));
      g_Move.WantOk = true;
    }
    else if (g_Move.TargetOk)
    {
      memcpy(g_Move.Target, target, sizeof(target));
      ApplySnapshot(h, g_Move.Pose);
      if (eyes)
        LookEyes(h, g_Move.Group >= 0 ? -1 : EyeOf(h, g_Move.Bone), g_Move.Start, target);
      else if (g_Move.Group >= 0)
      {
        float delta[3] = { target[0] - g_Move.Start[0], target[1] - g_Move.Start[1], target[2] - g_Move.Start[2] };
        MoveGroup(h, g_Move.Group, delta);
      }
      else
        SolveMove(h, g_Move.Bone, target, g_MoveBones);
    }
  }

  // The pose as it is now (the dots follow the mouse at once, not at the
  // next camera hook)
  static float shown[MaxBones][FloatsPerBone];
  ComposeHeld(h, shown);

  // The bones on the screen: At where the bone is, Draw where its dot is
  // drawn (face dots spread apart where they'd be on top of each other)
  struct Dot
  {
    ImVec2 At, Draw;
    bool Shown;
  };
  static Dot dots[MaxBones];
  int face = BoneByName(h.Skeleton, "Face");
  for (int n = 0; n < h.Count; ++n)
  {
    int i = (int)h.List[n];
    float x = 0.f, y = 0.f;
    // The Face bone sits on the head: with the face grouped, only in the list
    bool kind = BoneShown(h, i) && !(g_GroupFace && i == face);
    dots[i].Shown = (kind || i == g_SelectedBone) && ToScreen(view, shown[i], x, y);
    dots[i].At = dots[i].Draw = ImVec2(x, y);
  }
  // The face groups' dots: the middle of their bones
  float groupMiddle[FaceGroupCount][3];
  ImVec2 groupAt[FaceGroupCount], groupDraw[FaceGroupCount];
  bool groupShown[FaceGroupCount];
  for (int g = 0; g < FaceGroupCount; ++g)
  {
    groupShown[g] = false;
    if (!GroupShown(h, g))
      continue;
    GroupMiddle(h, g, shown, groupMiddle[g]);
    float x = 0.f, y = 0.f;
    groupShown[g] = ToScreen(view, groupMiddle[g], x, y);
    groupAt[g] = groupDraw[g] = ImVec2(x, y);
  }
  // The weapons' handles: where each one's pivot (its entity's place) is in
  // the pose as it is now, its rotation with it. One left to the game has none.
  float weaponPlace[MaxWeapons][FloatsPerBone];
  ImVec2 weaponAt[MaxWeapons], weaponDraw[MaxWeapons];
  bool weaponShown[MaxWeapons];
  for (int i = 0; i < MaxWeapons; ++i)
  {
    weaponShown[i] = false;
    if (i >= h.WeaponCount || h.Weapons[i].Bone == -1 || !g_WeaponsFollow)
      continue;
    WeaponWanted(h, h.Weapons[i], shown, h.Weapons[i].OffsetLocal, weaponPlace[i]);
    float x = 0.f, y = 0.f;
    weaponShown[i] = ToScreen(view, weaponPlace[i], x, y);
    weaponAt[i] = weaponDraw[i] = ImVec2(x, y);
  }
  bool weaponOk = g_SelectedWeapon >= 0 && g_SelectedWeapon < h.WeaponCount && weaponShown[g_SelectedWeapon];

  // Spread apart: the face's dots (the eyes' and the eyelids' middles are
  // both between the eyes, the lips' and the mouth corners' in the middle of
  // the mouth)
  {
    ImVec2* spread[96];
    int count = 0;
    for (int n = 0; n < h.Count && count < 96; ++n)
    {
      int i = (int)h.List[n];
      if (dots[i].Shown && h.Face[i])
        spread[count++] = &dots[i].Draw;
    }
    for (int g = 0; g < FaceGroupCount && count < 96; ++g)
      if (groupShown[g])
        spread[count++] = &groupDraw[g];
    float apart = fontSize * 0.85f;
    for (int pass = 0; pass < 12; ++pass)
      for (int a = 0; a < count; ++a)
        for (int b = a + 1; b < count; ++b)
        {
          float dx = spread[b]->x - spread[a]->x, dy = spread[b]->y - spread[a]->y;
          float distance = sqrtf(dx * dx + dy * dy);
          if (distance >= apart)
            continue;
          if (distance < 0.01f)
          {
            // Right on top of each other: apart in a fixed way
            float angle = (float)(a * 7 + b) * 2.39996f;
            dx = cosf(angle);
            dy = sinf(angle);
            distance = 1.f;
          }
          float push = (apart - distance) * 0.5f / distance;
          spread[a]->x -= dx * push;
          spread[a]->y -= dy * push;
          spread[b]->x += dx * push;
          spread[b]->y += dy * push;
        }
  }

  // A weapon's handle sits on its grip, where the hand's dot is: drawn apart
  // from the dots, the groups' and the other handles (they stay where they are)
  {
    float apart = fontSize * 0.85f;
    for (int pass = 0; pass < 6; ++pass)
      for (int i = 0; i < h.WeaponCount && i < MaxWeapons; ++i)
      {
        if (!weaponShown[i])
          continue;
        auto pushFrom = [&](ImVec2 other, int salt)
        {
          float dx = weaponDraw[i].x - other.x, dy = weaponDraw[i].y - other.y;
          float distance = sqrtf(dx * dx + dy * dy);
          if (distance >= apart)
            return;
          if (distance < 0.01f)
          {
            float angle = (float)(i * 7 + salt) * 2.39996f;
            dx = cosf(angle);
            dy = sinf(angle);
            distance = 1.f;
          }
          float push = (apart - distance) / distance;
          weaponDraw[i].x += dx * push;
          weaponDraw[i].y += dy * push;
        };
        for (int n = 0; n < h.Count; ++n)
        {
          int b = (int)h.List[n];
          if (dots[b].Shown)
            pushFrom(dots[b].Draw, b);
        }
        for (int g = 0; g < FaceGroupCount; ++g)
          if (groupShown[g])
            pushFrom(groupDraw[g], 200 + g);
        for (int k = 0; k < i; ++k)
          if (weaponShown[k])
            pushFrom(weaponDraw[k], 300 + k);
      }
  }

  if (clipOverWindow)
  {
    for (int n = 0; n < h.Count; ++n)
    {
      int i = (int)h.List[n];
      if (dots[i].Shown && overWindow(dots[i].Draw))
        dots[i].Shown = false;
    }
    for (int g = 0; g < FaceGroupCount; ++g)
      if (groupShown[g] && overWindow(groupDraw[g]))
        groupShown[g] = false;
    for (int i = 0; i < h.WeaponCount && i < MaxWeapons; ++i)
      if (weaponShown[i] && overWindow(weaponDraw[i]))
        weaponShown[i] = false;
  }

  // The picked bone's or face group's rings (not while something is moved)
  Ring rings[4];
  float radius = 0.f;
  int ringCount = 0;
  bool groupOk = g_SelectedGroup >= 0 && g_SelectedGroup < FaceGroupCount && groupShown[g_SelectedGroup];
  bool selectedOk = !groupOk && g_SelectedBone >= 0 && g_SelectedBone < h.Bones && h.InHeld[g_SelectedBone];
  if (!g_Move.On)
  {
    if (groupOk)
    {
      int first = h.GroupMembers[g_SelectedGroup][0];
      ringCount = MakeRings(view, groupMiddle[g_SelectedGroup], g_RingsOnBone ? shown[first] + 4 : nullptr, rings, radius);
    }
    else if (selectedOk)
      ringCount = MakeRings(view, shown[g_SelectedBone], g_RingsOnBone ? shown[g_SelectedBone] + 4 : nullptr, rings,
        radius);
    else if (weaponOk)
      ringCount = MakeRings(view, weaponPlace[g_SelectedWeapon], g_RingsOnBone ? weaponPlace[g_SelectedWeapon] + 4 : nullptr,
        rings, radius);
  }
  // Free move: the picked bone's X / Y / Z arrows, in place of its rings (the
  // bone's own axes or the world's, as the rings' tick says)
  struct Arrow
  {
    float Axis[3];
    ImVec2 From, To;
    bool Shown;
    ImU32 Colour;
  };
  Arrow arrows[3];
  int arrowCount = 0;
  if (g_FreeMove && selectedOk && ringCount > 0)
  {
    ringCount = 0;
    float const* p = shown[g_SelectedBone];
    float q[4] = { 0.f, 0.f, 0.f, 1.f };
    if (g_RingsOnBone)
    {
      memcpy(q, p + 4, sizeof(q));
      QNorm(q);
    }
    const ImU32 colours[3] = { IM_COL32(235, 80, 80, 235), IM_COL32(90, 210, 90, 235), IM_COL32(90, 140, 255, 235) };
    for (int a = 0; a < 3; ++a)
    {
      float unit[3] = { a == 0 ? 1.f : 0.f, a == 1 ? 1.f : 0.f, a == 2 ? 1.f : 0.f };
      Arrow& arrow = arrows[a];
      QRot(q, unit, arrow.Axis);
      Normalize3(arrow.Axis);
      float tip[3] = { p[0] + arrow.Axis[0] * radius, p[1] + arrow.Axis[1] * radius, p[2] + arrow.Axis[2] * radius };
      float x0, y0, x1, y1;
      arrow.Shown = ToScreen(view, p, x0, y0) && ToScreen(view, tip, x1, y1);
      arrow.From = ImVec2(x0, y0);
      arrow.To = ImVec2(x1, y1);
      arrow.Colour = colours[a];
    }
    arrowCount = 3;
  }

  if (clipOverWindow)
  {
    for (int r = 0; r < ringCount; ++r)
      for (int s = 0; s <= RingSegments; ++s)
        if (rings[r].Shown[s] && overWindow(rings[r].Points[s]))
          rings[r].Shown[s] = false;
    for (int a = 0; a < arrowCount; ++a)
      if (arrows[a].Shown && (overWindow(arrows[a].From) || overWindow(arrows[a].To)))
        arrows[a].Shown = false;
  }

  // The picked bone's path over the keyframes and its keyframe dots (2026-11-16), with what will be keyframed laid in
  // (a path's dot dragged, or auto key's change), so the path and the dragged dot follow the cursor; its dots over the
  // menu's windows not taken
  static KinTrail trail;
  bool trailDrag = (g_Move.On || g_Move.Pending) && g_Move.Trail && g_Move.Held == g_Active;
  bool willKey = h.Keys.On && !PlayingKeys(h) && KeysCurrent(h) && (trailDrag || (g_AutoKey && g_WindowOpen));
  MakeTrail(h, view, groupOk ? -1 : g_SelectedBone, willKey, trailDrag && !g_AutoKey ? &g_Move.Pose : nullptr,
    trailDrag ? g_Move.TrailMirror : -1.0, trailDrag, trail);
  if (clipOverWindow)
    for (TrailDot& d : trail.Dots)
      if (d.Shown && overWindow(d.At))
        d.Shown = false;

  // Look at's point (2026-11-16): its dot, taken before the bones' (it isn't the pose's: also while the keyframes play)
  bool lookShown = false;
  ImVec2 lookAt(0.f, 0.f);
  if (h.LookMode == Look_Point && h.LookPointOk)
  {
    float x = 0.f, y = 0.f;
    lookShown = ToScreen(view, h.LookPoint, x, y);
    lookAt = ImVec2(x, y);
  }
  if (g_LookDrag.On && (!io.MouseDown[0] || g_LookDrag.Held != g_Active || h.LookMode != Look_Point))
    g_LookDrag.On = false;
  if (g_LookDrag.On)
  {
    float target[3];
    if (PlanePoint(view, g_LookDrag.Base, io.MousePos.x + g_LookDrag.Offset[0], io.MousePos.y + g_LookDrag.Offset[1],
        target) && std::isfinite(target[0] + target[1] + target[2]))
      memcpy(h.LookPoint, target, sizeof(target));
  }
  bool hoverLook = false;
  if (lookShown && !g_LookDrag.On && !g_Drag.On && !g_Move.Pending && !g_Move.On && !io.WantCaptureMouse &&
      !overWindow(lookAt))
  {
    float dx = lookAt.x - io.MousePos.x, dy = lookAt.y - io.MousePos.y, reach = fontSize * DotReach;
    hoverLook = dx * dx + dy * dy <= reach * reach;
    if (hoverLook && io.MouseClicked[0] && !io.MouseDownOwned[0])
    {
      g_LookDrag.On = true;
      g_LookDrag.Held = g_Active;
      memcpy(g_LookDrag.Base, h.LookPoint, sizeof(g_LookDrag.Base));
      g_LookDrag.Offset[0] = lookAt.x - io.MousePos.x;
      g_LookDrag.Offset[1] = lookAt.y - io.MousePos.y;
      EndTextEdit();
      io.MouseClicked[0] = false;
    }
  }

  // What the cursor is on (off the menu's windows): the nearest dot (a
  // bone's or a face group's) first, then a ring. The dots come first: the
  // rings ran across the next joints' dots and took their clicks.
  int hoverRing = -1, hoverSegment = -1, hoverDot = -1, hoverGroup = -1, hoverArrow = -1, hoverWeapon = -1;
  int hoverTrail = -1;
  char const* trailRefusal = nullptr;   // why the path dot under the cursor can't be taken now (null: it can)
  if (!g_Drag.On && !g_Move.Pending && !g_Move.On && !io.WantCaptureMouse && !g_LookDrag.On && !hoverLook)
  {
    float reach = fontSize * DotReach;
    float bestDot = reach * reach;
    for (int n = 0; n < h.Count; ++n)
    {
      int i = (int)h.List[n];
      if (!dots[i].Shown)
        continue;
      float dx = dots[i].Draw.x - io.MousePos.x, dy = dots[i].Draw.y - io.MousePos.y;
      if (dx * dx + dy * dy <= bestDot)
      {
        bestDot = dx * dx + dy * dy;
        hoverDot = i;
      }
    }
    for (int g = 0; g < FaceGroupCount; ++g)
    {
      if (!groupShown[g])
        continue;
      float dx = groupDraw[g].x - io.MousePos.x, dy = groupDraw[g].y - io.MousePos.y;
      if (dx * dx + dy * dy <= bestDot)
      {
        bestDot = dx * dx + dy * dy;
        hoverGroup = g;
        hoverDot = -1;
      }
    }
    for (int i = 0; i < h.WeaponCount && i < MaxWeapons; ++i)
    {
      if (!weaponShown[i])
        continue;
      float dx = weaponDraw[i].x - io.MousePos.x, dy = weaponDraw[i].y - io.MousePos.y;
      if (dx * dx + dy * dy <= bestDot)
      {
        bestDot = dx * dx + dy * dy;
        hoverWeapon = i;
        hoverDot = hoverGroup = -1;
      }
    }
    // A dot of the picked bone's path (2026-11-16): after the dots (the one at the playhead's time is under the bone's
    // own: that one takes it), before the arrows and the rings; one that can't be taken now (TrailRefusal) leaves the
    // click to them
    if (hoverDot < 0 && hoverGroup < 0 && hoverWeapon < 0)
    {
      float bestTrail = reach * reach;
      for (size_t n = 0; n < trail.Dots.size(); ++n)
      {
        TrailDot const& d = trail.Dots[n];
        if (!d.Shown)
          continue;
        float dx = d.At.x - io.MousePos.x, dy = d.At.y - io.MousePos.y;
        if (dx * dx + dy * dy <= bestTrail)
        {
          bestTrail = dx * dx + dy * dy;
          hoverTrail = (int)n;
        }
      }
      // Dots on one place (a loop's two ends; a hollow hold's time): the one at the playhead's own time first, then
      // one that can be taken now (the second review, 2026-11-16: the later one took it, and was refused)
      if (hoverTrail >= 0)
      {
        float limit = sqrtf(bestTrail) + 1.f, reach2 = reach * reach;
        auto rankOf = [&](size_t n)
        {
          double t = trail.Dots[n].Time;
          return fabs(t - h.Keys.Time) <= pose_keys::TimeEpsilon ? 0 : (TrailRefusal(h, trail.Bone, t) ? 2 : 1);
        };
        int rank = rankOf((size_t)hoverTrail);
        for (size_t n = 0; n < trail.Dots.size() && rank > 0; ++n)
        {
          TrailDot const& d = trail.Dots[n];
          float dx = d.At.x - io.MousePos.x, dy = d.At.y - io.MousePos.y;
          if (!d.Shown || (int)n == hoverTrail || dx * dx + dy * dy > limit * limit || dx * dx + dy * dy > reach2)
            continue;
          int r = rankOf(n);
          if (r < rank)
          {
            rank = r;
            hoverTrail = (int)n;
          }
        }
      }
    }
    if (hoverTrail >= 0)
      trailRefusal = TrailRefusal(h, trail.Bone, trail.Dots[(size_t)hoverTrail].Time);
    if (hoverDot < 0 && hoverGroup < 0 && hoverWeapon < 0 && (hoverTrail < 0 || trailRefusal))
    {
      float best = RingReach * fontSize;
      for (int a = 0; a < arrowCount; ++a)
      {
        if (!arrows[a].Shown)
          continue;
        float along;
        float d = SegmentDistance(io.MousePos, arrows[a].From, arrows[a].To, along);
        if (d < best)
        {
          best = d;
          hoverArrow = a;
        }
      }
      for (int r = 0; r < ringCount; ++r)
        for (int s = 0; s < RingSegments; ++s)
        {
          if (!rings[r].Shown[s] || !rings[r].Shown[s + 1])
            continue;
          float along;
          float d = SegmentDistance(io.MousePos, rings[r].Points[s], rings[r].Points[s + 1], along);
          if (d < best)
          {
            best = d;
            hoverRing = r;
            hoverSegment = s;
          }
        }
    }
  }

  // While a clip or the keyframes play nothing is taken (an edit would fight it): pause it
  bool canGrab = !(h.Clip.On && h.Clip.Playing) && !PlayingKeys(h);
  // A click: on an arrow it moves the bone along it, on a ring it turns the
  // bone (or the group), on a dot it picks it and holds it. The click is the
  // pose's: taken off ImGui's input, so the lights' dots and the camera's
  // track path (drawn after) don't take it too.
  if (canGrab && io.MouseClicked[0] && !io.MouseDownOwned[0] && !g_Drag.On && hoverArrow >= 0 && selectedOk)
  {
    Arrow const& arrow = arrows[hoverArrow];
    g_Move.Pending = true;
    g_Move.On = false;
    g_Move.Trail = false;
    g_Move.Held = g_Active;
    g_Move.Bone = g_SelectedBone;
    g_Move.Group = -1;
    g_Move.Weapon = -1;
    g_Move.Mouse[0] = io.MousePos.x;
    g_Move.Mouse[1] = io.MousePos.y;
    g_Move.Offset[0] = g_Move.Offset[1] = 0.f;
    memcpy(g_Move.Start, shown[g_SelectedBone], sizeof(g_Move.Start));
    memcpy(g_Move.Base, g_Move.Start, sizeof(g_Move.Base));
    g_Move.Pose = SnapshotOf(h);
    g_Move.TargetOk = false;
    g_Move.Depth = false;
    g_Move.OnAxis = true;
    memcpy(g_Move.Axis, arrow.Axis, sizeof(g_Move.Axis));
    g_Move.AxisScreen[0] = arrow.To.x - arrow.From.x;
    g_Move.AxisScreen[1] = arrow.To.y - arrow.From.y;
    g_Move.AxisLength = radius;
    EndTextEdit();
    io.MouseClicked[0] = false;
  }
  else if (canGrab && io.MouseClicked[0] && !io.MouseDownOwned[0] && !g_Drag.On && hoverRing >= 0)
  {
    Ring const& ring = rings[hoverRing];
    PushUndo(h, SnapshotOf(h));
    g_Drag.On = true;
    g_Drag.Held = g_Active;
    g_Drag.Ring = hoverRing;
    memcpy(g_Drag.Axis, ring.Axis, sizeof(g_Drag.Axis));
    // The ring's way on the screen where it was taken: pixels per radian
    float step = 6.2831853f / RingSegments;
    ImVec2 a = ring.Points[hoverSegment], b = ring.Points[hoverSegment + 1];
    g_Drag.Tangent[0] = (b.x - a.x) / step;
    g_Drag.Tangent[1] = (b.y - a.y) / step;
    g_Drag.Mouse[0] = io.MousePos.x;
    g_Drag.Mouse[1] = io.MousePos.y;
    auto parentOf = [&](int bone, float* out)
    {
      int p = h.Parents[bone];
      if (p >= 0)
        memcpy(out, shown[p] + 4, 4 * sizeof(float));
      else
        QIdentity(out);
      QNorm(out);
    };
    g_Drag.Weapon = -1;
    if (weaponOk && !groupOk && !selectedOk)
    {
      // A weapon's ring: its edit's turn, in its frame
      HeldWeapon const& w = h.Weapons[g_SelectedWeapon];
      float frame[FloatsPerBone];
      WeaponFrame(h, w, shown, frame);
      g_Drag.Weapon = g_SelectedWeapon;
      g_Drag.Group = -1;
      g_Drag.Bone = -1;
      memcpy(g_Drag.Parent, frame + 4, sizeof(g_Drag.Parent));
      QNorm(g_Drag.Parent);
      memcpy(g_Drag.Turn, w.EditTurn, sizeof(g_Drag.Turn));
    }
    else if (groupOk)
    {
      g_Drag.Group = g_SelectedGroup;
      g_Drag.Bone = -1;
      for (int k = 0; k < h.GroupSize[g_SelectedGroup]; ++k)
      {
        int m = h.GroupMembers[g_SelectedGroup][k];
        parentOf(m, g_Drag.GroupParents[k]);
        memcpy(g_Drag.GroupTurns[k], h.Turn[m], sizeof(g_Drag.GroupTurns[k]));
      }
    }
    else
    {
      g_Drag.Group = -1;
      g_Drag.Bone = g_SelectedBone;
      parentOf(g_SelectedBone, g_Drag.Parent);
      memcpy(g_Drag.Turn, h.Turn[g_SelectedBone], sizeof(g_Drag.Turn));
    }
    g_Drag.Pose = SnapshotOf(h);
    EndTextEdit();
    io.MouseClicked[0] = false;
  }
  else if (canGrab && io.MouseClicked[0] && !io.MouseDownOwned[0] && !g_Drag.On && hoverWeapon >= 0)
  {
    // A weapon's handle: picked, and held (moved if the cursor goes on)
    HeldWeapon const& w = h.Weapons[hoverWeapon];
    g_SelectedWeapon = hoverWeapon;
    g_SelectedBone = -1;
    g_SelectedGroup = -1;
    g_Move.Pending = true;
    g_Move.On = false;
    g_Move.Trail = false;
    g_Move.Held = g_Active;
    g_Move.Mouse[0] = io.MousePos.x;
    g_Move.Mouse[1] = io.MousePos.y;
    g_Move.Bone = -1;
    g_Move.Group = -1;
    g_Move.Weapon = hoverWeapon;
    WeaponFrame(h, w, shown, g_Move.WeaponFrame);
    g_Move.Offset[0] = weaponAt[hoverWeapon].x - io.MousePos.x;
    g_Move.Offset[1] = weaponAt[hoverWeapon].y - io.MousePos.y;
    memcpy(g_Move.Start, weaponPlace[hoverWeapon], sizeof(g_Move.Start));
    memcpy(g_Move.Base, g_Move.Start, sizeof(g_Move.Base));
    g_Move.Pose = SnapshotOf(h);
    g_Move.TargetOk = false;
    g_Move.Depth = false;
    g_Move.OnAxis = false;
    EndTextEdit();
    io.MouseClicked[0] = false;
  }
  else if (canGrab && io.MouseClicked[0] && !io.MouseDownOwned[0] && !g_Drag.On && (hoverDot >= 0 || hoverGroup >= 0))
  {
    // Picked, and held: moved if the cursor goes on. The drag goes by where
    // the bone (or the group's middle) really is, not where its dot is drawn.
    g_Move.Pending = true;
    g_Move.On = false;
    g_Move.Trail = false;
    g_Move.Held = g_Active;
    g_Move.Mouse[0] = io.MousePos.x;
    g_Move.Mouse[1] = io.MousePos.y;
    g_Move.Weapon = -1;
    if (hoverGroup >= 0)
    {
      g_SelectedGroup = hoverGroup;
      g_SelectedBone = -1;
      g_SelectedWeapon = -1;
      g_Move.Group = hoverGroup;
      g_Move.Bone = -1;
      g_Move.Offset[0] = groupAt[hoverGroup].x - io.MousePos.x;
      g_Move.Offset[1] = groupAt[hoverGroup].y - io.MousePos.y;
      memcpy(g_Move.Start, groupMiddle[hoverGroup], sizeof(g_Move.Start));
    }
    else
    {
      g_SelectedBone = hoverDot;
      g_SelectedGroup = -1;
      g_SelectedWeapon = -1;
      g_Move.Group = -1;
      g_Move.Bone = hoverDot;
      g_Move.Offset[0] = dots[hoverDot].At.x - io.MousePos.x;
      g_Move.Offset[1] = dots[hoverDot].At.y - io.MousePos.y;
      memcpy(g_Move.Start, shown[hoverDot], sizeof(g_Move.Start));
    }
    memcpy(g_Move.Base, g_Move.Start, sizeof(g_Move.Base));
    g_Move.Pose = SnapshotOf(h);
    g_Move.TargetOk = false;
    g_Move.Depth = false;
    g_Move.OnAxis = false;
    EndTextEdit();
    io.MouseClicked[0] = false;
  }
  else if (canGrab && io.MouseClicked[0] && !io.MouseDownOwned[0] && !g_Drag.On && hoverTrail >= 0 && trail.Ok &&
    trail.Bone >= 0 && trail.Bone < h.Bones && h.InHeld[trail.Bone])
  {
    // A dot of the picked bone's path (2026-11-16): the playhead to its time and the pose there at once (a click alone
    // does only that), then held like the bone's own dot; let go after a drag, the change is keyframed at that time
    // (endMove). Not when it can't be taken now (TrailRefusal: the click is only taken off ImGui's input).
    double time = trail.Dots[(size_t)hoverTrail].Time;
    if (!trailRefusal)
    {
      if (fabs(time - h.Keys.Time) > pose_keys::TimeEpsilon)
        SetPlayhead(h, time);
      ApplyKeysNow(h);
      ComposeHeld(h, shown);
      int bone = trail.Bone;
      // Held where the dot is, the bone's place at that time as keyed (the review, 2026-11-16: from the bone, a
      // change not keyframed yet put the drag off the dot): the bone is there, unless a bone of the path has such a
      // change at the playhead's own time (Auto key on with the Pose window closed: Auto key keyframes them all when
      // let go; with it off TrailRefusal refuses); then the drag takes it from its pose to where the dot is dragged,
      // and Bones stop at walls sweeps it from where it really is (PressedPlace, the second review)
      float place[FloatsPerBone], frame[FloatsPerBone];
      if (HeldFrame(h, frame))
        PlaceAt(h, bone, h.Keys.Time, frame, place);
      else
        memcpy(place, shown[bone], sizeof(place));
      g_Move.Pending = true;
      g_Move.On = false;
      g_Move.Trail = true;
      g_Move.TrailMirror = LoopOtherEnd(h, h.Keys.Time);
      g_Move.Held = g_Active;
      g_Move.Mouse[0] = io.MousePos.x;
      g_Move.Mouse[1] = io.MousePos.y;
      g_Move.Weapon = -1;
      g_Move.Group = -1;
      g_Move.Bone = bone;
      float x = io.MousePos.x, y = io.MousePos.y;
      if (!ToScreen(view, place, x, y))
      {
        x = io.MousePos.x;
        y = io.MousePos.y;
      }
      g_Move.Offset[0] = x - io.MousePos.x;
      g_Move.Offset[1] = y - io.MousePos.y;
      memcpy(g_Move.Start, place, sizeof(g_Move.Start));
      memcpy(g_Move.Base, g_Move.Start, sizeof(g_Move.Base));
      memcpy(g_Move.PressedPlace, shown[bone], sizeof(g_Move.PressedPlace));
      g_Move.Pose = SnapshotOf(h);
      g_Move.TargetOk = false;
      g_Move.Depth = false;
      g_Move.OnAxis = false;
    }
    EndTextEdit();   // (refused too: the click is taken off ImGui, so a field typed in wouldn't let go; the second review)
    io.MouseClicked[0] = false;
  }

  ImDrawList* pDrawList = ImGui::GetOverlayDrawList();
  const ImU32 shadow = IM_COL32(0, 0, 0, 160);
  const ImU32 line = IM_COL32(255, 255, 255, 90);
  const ImU32 dot = IM_COL32(255, 220, 90, 230);
  const ImU32 groupDot = IM_COL32(120, 210, 255, 235);
  const ImU32 turned = IM_COL32(255, 140, 40, 255);
  const ImU32 picked = IM_COL32(255, 255, 255, 255);
  auto shadowedText = [&](ImVec2 at, ImU32 colour, char const* text)
  {
    pDrawList->AddText(ImVec2(at.x + 1.f, at.y + 1.f), shadow, text);
    pDrawList->AddText(at, colour, text);
  };
  auto labelText = [&](ImVec2 at, ImU32 colour, char const* text)
  {
    if (clipOverWindow)
    {
      ImVec2 size = ImGui::CalcTextSize(text);
      if (at.x + size.x >= clipMin.x && at.x <= clipMax.x && at.y + size.y >= clipMin.y && at.y <= clipMax.y)
        return;
    }
    shadowedText(at, colour, text);
  };
  // The ghost poses and the picked bone's path under the skeleton (2026-11-16); its dot under the cursor or dragged
  // bigger, with its time
  int hotTrail = hoverTrail;
  if (hotTrail < 0 && (g_Move.On || g_Move.Pending) && g_Move.Trail && g_Move.Held == g_Active)
    for (size_t n = 0; n < trail.Dots.size() && hotTrail < 0; ++n)
      if (fabs(trail.Dots[n].Time - h.Keys.Time) <= pose_keys::TimeEpsilon)
        hotTrail = (int)n;
  DrawKinTrail(h, view, pDrawList, fontSize, trail, hotTrail);
  // The skeleton's lines, then its dots
  for (int n = 0; n < h.Count; ++n)
  {
    int i = (int)h.List[n], p = h.Parents[i];
    if (p < 0 || !h.InHeld[p] || !dots[i].Shown || !dots[p].Shown)
      continue;
    pDrawList->AddLine(dots[p].Draw, dots[i].Draw, shadow, 3.f);
    pDrawList->AddLine(dots[p].Draw, dots[i].Draw, line, 1.5f);
  }
  // A face dot spread from where its bone (or group) really is: a faint line
  // to there
  auto leader = [&](ImVec2 from, ImVec2 to)
  {
    float dx = to.x - from.x, dy = to.y - from.y;
    if (dx * dx + dy * dy > 9.f)
    {
      pDrawList->AddLine(from, to, IM_COL32(255, 255, 255, 70), 1.f);
      pDrawList->AddCircleFilled(to, 2.f, IM_COL32(255, 255, 255, 120), 6);
    }
  };
  for (int n = 0; n < h.Count; ++n)
  {
    int i = (int)h.List[n];
    if (dots[i].Shown && h.Face[i])
      leader(dots[i].Draw, dots[i].At);
  }
  for (int g = 0; g < FaceGroupCount; ++g)
    if (groupShown[g])
      leader(groupDraw[g], groupAt[g]);
  // The picked or hovered group's bones: small dots where they are
  for (int g = 0; g < FaceGroupCount; ++g)
  {
    if (!groupShown[g] || (g != g_SelectedGroup && g != hoverGroup))
      continue;
    for (int k = 0; k < h.GroupSize[g]; ++k)
    {
      float x, y;
      if (ToScreen(view, shown[h.GroupMembers[g][k]], x, y))
      {
        ImVec2 p(x, y);
        if (!clipOverWindow || !overWindow(p))
          pDrawList->AddCircleFilled(p, 2.5f, groupDot, 8);
      }
    }
  }
  // The dots: the picked one, the one under the cursor and the one being
  // moved on top, with a ring round them
  int moving = g_Move.On && g_Move.Bone >= 0 && g_Move.Bone < h.Bones ? g_Move.Bone : -1;
  auto drawDot = [&](int i)
  {
    bool hot = i == hoverDot || i == moving;
    float size = fontSize * (i == g_SelectedBone ? 0.34f : (hot ? 0.32f : 0.24f));
    pDrawList->AddCircleFilled(dots[i].Draw, size + 1.5f, shadow, 16);
    pDrawList->AddCircleFilled(dots[i].Draw, size, i == g_SelectedBone ? picked : (IsEdited(h, i) ? turned : dot), 16);
    if (hot)
      pDrawList->AddCircle(dots[i].Draw, size + fontSize * 0.18f, IM_COL32(255, 255, 255, 220), 20, 2.f);
  };
  for (int n = 0; n < h.Count; ++n)
  {
    int i = (int)h.List[n];
    if (dots[i].Shown && i != g_SelectedBone && i != hoverDot && i != moving)
      drawDot(i);
  }
  for (int i : { g_SelectedBone, hoverDot, moving })
    if (i >= 0 && i < h.Bones && h.InHeld[i] && dots[i].Shown)
      drawDot(i);
  // The pins' squares and the look's point over the dots (2026-11-16)
  DrawKinPins(h, view, pDrawList, fontSize);
  // The path's dot under the cursor or dragged: its time (and why it can't be taken)
  if (hotTrail >= 0 && trail.Dots[(size_t)hotTrail].Shown)
  {
    TrailDot const& d = trail.Dots[(size_t)hotTrail];
    char label[160];
    // (a loop's two ends: both times when their dots are one place, as the drag then keyframes both)
    double other = LoopOtherEnd(h, d.Time);
    bool together = false;
    for (TrailDot const& e : trail.Dots)
      if (other >= 0.0 && fabs(e.Time - other) <= pose_keys::TimeEpsilon && Distance3(e.Place, d.Place) < 1e-4f)
        together = true;
    int used = together ? _snprintf_s(label, sizeof(label), _TRUNCATE, "%.2f s and %.2f s (the loop's ends)",
      (std::min)(d.Time, other), (std::max)(d.Time, other)) : _snprintf_s(label, sizeof(label), _TRUNCATE, "%.2f s", d.Time);
    if (hoverTrail >= 0 && trailRefusal && used > 0)
      _snprintf_s(label + used, sizeof(label) - (size_t)used, _TRUNCATE, ": %s", trailRefusal);
    labelText(ImVec2(d.At.x + fontSize * 0.5f, d.At.y - fontSize * 1.2f), IM_COL32(255, 210, 150, 230), label);
  }
  if (lookShown && !overWindow(lookAt))
  {
    bool hot = hoverLook || g_LookDrag.On;
    float size = fontSize * (hot ? 0.36f : 0.3f);
    pDrawList->AddCircle(lookAt, size + 1.5f, shadow, 20, 4.f);
    pDrawList->AddCircle(lookAt, size, IM_COL32(90, 230, 255, 240), 20, 2.f);
    pDrawList->AddCircleFilled(lookAt, fontSize * 0.08f, IM_COL32(90, 230, 255, 240), 8);
    if (hot)
      labelText(ImVec2(lookAt.x + fontSize * 0.5f, lookAt.y - fontSize * 1.2f), IM_COL32(255, 255, 255, 200),
        "Look at");
  }
  for (int g = 0; g < FaceGroupCount; ++g)
  {
    if (!groupShown[g])
      continue;
    float size = fontSize * (g == g_SelectedGroup ? 0.34f : (g == hoverGroup ? 0.32f : 0.24f));
    ImU32 colour = g == g_SelectedGroup ? picked : (GroupEdited(h, g) ? turned : groupDot);
    // A diamond, so groups stand apart from bones
    ImVec2 c = groupDraw[g];
    pDrawList->AddQuadFilled(ImVec2(c.x, c.y - size - 1.5f), ImVec2(c.x + size + 1.5f, c.y), ImVec2(c.x, c.y + size + 1.5f),
      ImVec2(c.x - size - 1.5f, c.y), shadow);
    pDrawList->AddQuadFilled(ImVec2(c.x, c.y - size), ImVec2(c.x + size, c.y), ImVec2(c.x, c.y + size), ImVec2(c.x - size, c.y),
      colour);
  }
  // The weapons' handles: squares, so they stand apart from bones and groups
  const ImU32 weaponDot = IM_COL32(150, 240, 150, 235);
  for (int i = 0; i < h.WeaponCount && i < MaxWeapons; ++i)
  {
    if (!weaponShown[i])
      continue;
    HeldWeapon const& w = h.Weapons[i];
    bool edited = w.EditMove[0] != 0.f || w.EditMove[1] != 0.f || w.EditMove[2] != 0.f || !QIsIdentity(w.EditTurn) ||
      w.Bone != w.TakenBone || memcmp(w.OffsetLocal, w.TakenOffsetLocal, sizeof(w.OffsetLocal)) != 0;
    bool hot = i == hoverWeapon || (g_Move.On && g_Move.Weapon == i);
    float size = fontSize * (i == g_SelectedWeapon ? 0.30f : (hot ? 0.28f : 0.22f));
    ImVec2 c = weaponDraw[i];
    leader(c, weaponAt[i]);
    pDrawList->AddRectFilled(ImVec2(c.x - size - 1.5f, c.y - size - 1.5f), ImVec2(c.x + size + 1.5f, c.y + size + 1.5f), shadow);
    pDrawList->AddRectFilled(ImVec2(c.x - size, c.y - size), ImVec2(c.x + size, c.y + size),
      i == g_SelectedWeapon ? picked : (edited ? turned : weaponDot));
    if (hot)
      pDrawList->AddRect(ImVec2(c.x - size - fontSize * 0.18f, c.y - size - fontSize * 0.18f),
        ImVec2(c.x + size + fontSize * 0.18f, c.y + size + fontSize * 0.18f), IM_COL32(255, 255, 255, 220), 0.f, 0, 2.f);
  }
  // While one's moved: what the keys do, by the cursor
  if (g_Move.On)
  {
    ImVec2 at(io.MousePos.x + fontSize * 1.2f, io.MousePos.y + fontSize * 0.9f);
    if (!clipOverWindow || !overWindow(at))
      shadowedText(at, IM_COL32(255, 255, 255, 210), g_Move.OnAxis ? "Along the arrow   Right click: cancel" :
        (g_Move.Depth ? "Nearer / farther (let go of Shift: across)" : "Shift: nearer / farther   Right click: cancel"));
  }
  // A moved bone that can't reach where it's wanted: a line to there
  if (g_Move.On && g_Move.TargetOk && g_Move.Bone >= 0 && g_Move.Bone < h.Bones && dots[g_Move.Bone].Shown)
  {
    float tx, ty;
    if (ToScreen(view, g_Move.Target, tx, ty))
    {
      ImVec2 at(tx, ty), from = dots[g_Move.Bone].At;
      float dx = at.x - from.x, dy = at.y - from.y;
      if (dx * dx + dy * dy > 36.f && (!clipOverWindow || (!overWindow(at) && !overWindow(from))))
      {
        pDrawList->AddLine(from, at, IM_COL32(255, 255, 255, 110), 1.5f);
        pDrawList->AddCircle(at, fontSize * 0.25f, shadow, 12, 3.f);
        pDrawList->AddCircle(at, fontSize * 0.25f, IM_COL32(255, 255, 255, 200), 12, 1.5f);
      }
    }
  }
  // Free move's arrows, with a head at the tip
  for (int a = 0; a < arrowCount; ++a)
  {
    Arrow const& arrow = arrows[a];
    if (!arrow.Shown)
      continue;
    bool hot = a == hoverArrow;
    pDrawList->AddLine(arrow.From, arrow.To, shadow, hot ? 5.f : 3.5f);
    pDrawList->AddLine(arrow.From, arrow.To, arrow.Colour, hot ? 3.f : 2.f);
    float dx = arrow.To.x - arrow.From.x, dy = arrow.To.y - arrow.From.y;
    float length = sqrtf(dx * dx + dy * dy);
    if (length > 1.f)
    {
      float ux = dx / length, uy = dy / length, size = fontSize * (hot ? 0.55f : 0.45f);
      ImVec2 base(arrow.To.x - ux * size, arrow.To.y - uy * size);
      ImVec2 left(base.x - uy * size * 0.5f, base.y + ux * size * 0.5f), right(base.x + uy * size * 0.5f, base.y - ux * size * 0.5f);
      pDrawList->AddTriangleFilled(ImVec2(arrow.To.x + ux * 1.5f, arrow.To.y + uy * 1.5f), ImVec2(left.x - uy * 1.5f, left.y + ux * 1.5f),
        ImVec2(right.x + uy * 1.5f, right.y - ux * 1.5f), shadow);
      pDrawList->AddTriangleFilled(arrow.To, left, right, arrow.Colour);
    }
  }
  // The rings, and the picked and hovered names
  for (int r = 0; r < ringCount; ++r)
  {
    Ring const& ring = rings[r];
    bool hot = r == hoverRing || (g_Drag.On && g_Drag.Ring == r);
    for (int s = 0; s < RingSegments; ++s)
    {
      if (!ring.Shown[s] || !ring.Shown[s + 1])
        continue;
      pDrawList->AddLine(ring.Points[s], ring.Points[s + 1], shadow, hot ? 5.f : 3.5f);
      pDrawList->AddLine(ring.Points[s], ring.Points[s + 1], ring.Colour, hot ? 3.f : 1.75f);
    }
  }
  // The pick checked again here: a click above may have picked a bone in
  // place of a group, or a group in place of a bone, after groupOk and
  // selectedOk were worked out (2026-11-08 13:58: pressing a bone's dot with
  // a face group picked gave FaceGroupNames[-1] here and crashed the game)
  bool groupLabel = g_SelectedGroup >= 0 && g_SelectedGroup < FaceGroupCount && groupShown[g_SelectedGroup];
  bool boneLabel = !groupLabel && g_SelectedBone >= 0 && g_SelectedBone < h.Bones && h.InHeld[g_SelectedBone];
  bool weaponLabel = !groupLabel && !boneLabel && g_SelectedWeapon >= 0 && g_SelectedWeapon < h.WeaponCount &&
    g_SelectedWeapon < MaxWeapons && weaponShown[g_SelectedWeapon];
  if (weaponLabel)
  {
    ImVec2 at(weaponDraw[g_SelectedWeapon].x + fontSize * 0.6f, weaponDraw[g_SelectedWeapon].y - fontSize * 1.4f);
    labelText(at, picked, h.Weapons[g_SelectedWeapon].Name);
  }
  if (groupLabel)
  {
    ImVec2 at(groupDraw[g_SelectedGroup].x + fontSize * 0.6f, groupDraw[g_SelectedGroup].y - fontSize * 1.4f);
    labelText(at, picked, FaceGroupNames[g_SelectedGroup]);
  }
  else if (boneLabel && dots[g_SelectedBone].Shown)
  {
    ImVec2 at(dots[g_SelectedBone].Draw.x + fontSize * 0.6f, dots[g_SelectedBone].Draw.y - fontSize * 1.4f);
    labelText(at, picked, BoneName(h, g_SelectedBone));
  }
  if (hoverGroup >= 0 && hoverGroup != g_SelectedGroup)
  {
    ImVec2 at(groupDraw[hoverGroup].x + fontSize * 0.5f, groupDraw[hoverGroup].y - fontSize * 1.2f);
    labelText(at, IM_COL32(255, 255, 255, 200), FaceGroupNames[hoverGroup]);
  }
  else if (hoverDot >= 0 && hoverDot != g_SelectedBone && dots[hoverDot].Shown)
  {
    ImVec2 at(dots[hoverDot].Draw.x + fontSize * 0.5f, dots[hoverDot].Draw.y - fontSize * 1.2f);
    labelText(at, IM_COL32(255, 255, 255, 200), BoneName(h, hoverDot));
  }
  else if (hoverWeapon >= 0 && hoverWeapon != g_SelectedWeapon && hoverWeapon < h.WeaponCount && weaponShown[hoverWeapon])
  {
    ImVec2 at(weaponDraw[hoverWeapon].x + fontSize * 0.5f, weaponDraw[hoverWeapon].y - fontSize * 1.2f);
    labelText(at, IM_COL32(255, 255, 255, 200), h.Weapons[hoverWeapon].Name);
  }
}

void pose_editor::ReadConfig(INIReader* pReader)
{
  std::lock_guard<std::mutex> lock(g_Mutex);
  g_ShowBones = pReader->GetBoolean("Pose", "ShowBones", true);
  g_ShowFace = pReader->GetBoolean("Pose", "ShowFace", false);
  g_GroupFace = pReader->GetBoolean("Pose", "GroupFace", true);
  g_ShowFingers = pReader->GetBoolean("Pose", "ShowFingers", false);
  g_ShowHelpers = pReader->GetBoolean("Pose", "ShowHelpers", false);
  long dragBends = pReader->GetInteger("Pose", "DragBends", 2);
  g_MoveBones = dragBends < 1 ? 1 : (dragBends > 4 ? 4 : (int)dragBends);
  g_StopAtWalls = pReader->GetBoolean("Pose", "StopAtWalls", true);
  g_RingsOnBone = pReader->GetBoolean("Pose", "RingsOnBone", true);
  g_AutoKey = pReader->GetBoolean("Pose", "AutoKey", true);
  g_KeyAnchors = pReader->GetBoolean("Pose", "KeyAnchors", true);
  g_SnapKeys = pReader->GetBoolean("Pose", "SnapKeys", true);
  // The kinematics (2026-11-16)
  g_JointHinges = pReader->GetBoolean("Pose", "JointHinges", true);
  g_KeepEndTurn = pReader->GetBoolean("Pose", "KeepEndTurn", true);
  g_FeetPlanted = pReader->GetBoolean("Pose", "FeetPlanted", true);
  g_CollarFollows = pReader->GetBoolean("Pose", "CollarFollows", true);
  g_SpineCurve = pReader->GetBoolean("Pose", "SpineCurve", true);
  g_FarReach = pReader->GetBoolean("Pose", "FarReach", false);
  g_HelpersFollow = pReader->GetBoolean("Pose", "HelpersFollow", true);
  auto unit = [](double value, float fallback)
  {
    return std::isfinite(value) ? (value < 0.0 ? 0.f : (value > 1.0 ? 1.f : (float)value)) : fallback;
  };
  g_LooseTiming = unit(pReader->GetReal("Pose", "LooseTiming", 0.5), 0.5f);
  g_FollowThrough = unit(pReader->GetReal("Pose", "FollowThrough", 0.5), 0.5f);
  g_FootLock = pReader->GetBoolean("Pose", "FootLock", true);
  g_ShowTrail = pReader->GetBoolean("Pose", "ShowTrail", true);
  g_ShowGhosts = pReader->GetBoolean("Pose", "ShowGhosts", false);
}

std::string pose_editor::GetConfig()
{
  std::lock_guard<std::mutex> lock(g_Mutex);
  std::string config = "[Pose]\n";
  config += std::string("ShowBones = ") + (g_ShowBones ? "true" : "false") + "\n";
  config += std::string("ShowFace = ") + (g_ShowFace ? "true" : "false") + "\n";
  config += std::string("GroupFace = ") + (g_GroupFace ? "true" : "false") + "\n";
  config += std::string("ShowFingers = ") + (g_ShowFingers ? "true" : "false") + "\n";
  config += std::string("ShowHelpers = ") + (g_ShowHelpers ? "true" : "false") + "\n";
  config += "DragBends = " + std::to_string(g_MoveBones) + "\n";
  config += std::string("StopAtWalls = ") + (g_StopAtWalls ? "true" : "false") + "\n";
  config += std::string("RingsOnBone = ") + (g_RingsOnBone ? "true" : "false") + "\n";
  config += std::string("AutoKey = ") + (g_AutoKey ? "true" : "false") + "\n";
  config += std::string("KeyAnchors = ") + (g_KeyAnchors ? "true" : "false") + "\n";
  config += std::string("SnapKeys = ") + (g_SnapKeys ? "true" : "false") + "\n";
  config += std::string("JointHinges = ") + (g_JointHinges ? "true" : "false") + "\n";
  config += std::string("KeepEndTurn = ") + (g_KeepEndTurn ? "true" : "false") + "\n";
  config += std::string("FeetPlanted = ") + (g_FeetPlanted ? "true" : "false") + "\n";
  config += std::string("CollarFollows = ") + (g_CollarFollows ? "true" : "false") + "\n";
  config += std::string("SpineCurve = ") + (g_SpineCurve ? "true" : "false") + "\n";
  config += std::string("FarReach = ") + (g_FarReach ? "true" : "false") + "\n";
  config += std::string("HelpersFollow = ") + (g_HelpersFollow ? "true" : "false") + "\n";
  char number[32];
  _snprintf_s(number, sizeof(number), _TRUNCATE, "%.3f", g_LooseTiming);
  config += std::string("LooseTiming = ") + number + "\n";
  _snprintf_s(number, sizeof(number), _TRUNCATE, "%.3f", g_FollowThrough);
  config += std::string("FollowThrough = ") + number + "\n";
  config += std::string("FootLock = ") + (g_FootLock ? "true" : "false") + "\n";
  config += std::string("ShowTrail = ") + (g_ShowTrail ? "true" : "false") + "\n";
  config += std::string("ShowGhosts = ") + (g_ShowGhosts ? "true" : "false") + "\n";
  config += "\n";
  return config;
}
