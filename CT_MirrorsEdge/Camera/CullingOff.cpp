#include "CullingOff.h"
#include "../Main.h"
#include "../Util/Util.h"
#include "../Util/GameState.h"
#include "../Util/ImGuiEXT.h"
#include "../imgui/imgui.h"
#include <Windows.h>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>

// The renderer's mesh cull, read from the game's code 2026-11-14 / 15 (Research_Culling_Off_2026-11-14.md; two
// adversarial reviews found the tree path, the world views and the visible list's room below):
// - The cull runs once per world view (0x143711960 loops them, calling 0x1436C5FB0 with src = slot + 0x20; the world
//   view is at slot + 0x10, its descriptor at view + 8 with a type at + 0x10: 1 RootStaticLocalIBL, 5
//   RootDynamicLocalIBL, 4 WorldPlanarReflection; the camera's own type isn't known yet).
// - 0x1436C5FB0 picks one of two paths every call by MeshCullSettings.HiEnable (+0x44 of the object the renderer
//   keeps at + 0x142918, 0x1436C6141), for every world view: not 0 (the game's default, 1 read live), its tree path
//   (meshCullTree jobs 0x1436D1600) the tools don't change; 0, its flat path: a parallel job 0x1436D1230 per renderer
//   batch, which builds its context on its stack (0x1436C0450) and tests chunk after chunk (0x1436D3E20). While the
//   tick is on, HiEnable is set to 0 (the game's value saved first and written back when it's off), so the game culls
//   every world view on its flat path.
// - A chunk holds up to 64 instances (spheres + 0, size thresholds + 0x400, flag bytes + 0x500, records + 0x540) and
//   is kept packed (the take-out 0x1436D27A0 moves the last instance into the gap), so the first `count` are live.
// - Every instance is tested against every view of the world view at once, into a mask of view bits. View 0 (bit 1)
//   is the world view's own camera: 0x1436C7AD0 when ctx + 0x674 is 1, else 0x1436C77E0, both (ctx, chunk, chunk +
//   0x400, count, mask, mask2, ctx + 0x6A0 the planes, 1). Outside one of 6 planes, or smaller on screen than the
//   threshold, no bit 1; not too small, bit 31 (0x80000000), which the shadow and other views' tests require.
// - After the view tests, an instance in view 0 whose flags have 8 goes to a deferred list (the occlusion buffer's
//   test, a box size test and a clip test) while ctx + 0x670 is set, i.e. while the world view has an occlusion
//   buffer (src + 0x110: pointer, width, height); without one the game sends it on directly, as for any other.
// - The visible instances that aren't the direct mesh renderer's go into a per world view array of
//   WorldRenderSettings.GenericEntityMaxVisibleEntityCount entries (+0x1C0, set up at 0x143716262 - 0x143716277: room
//   at slot + 0, count at slot + 4); the flush adds the whole count (0x14356A839) and copies only up to the room, but
//   genProcessCull (0x143714D50) goes over the whole count (0x143714D5C), past the array once the count is over it.
// With it on, after the game's own view-0 test, for world views other than reflections: an instance without bit 1 gets
// it if it's within the view margin (the side and near planes moved out by an angle, the far plane kept) and, when the
// game found it too small, within the small objects' range (2026-11-15: all around had cost 100 -> 7 fps); never with
// a threshold of +infinity. ctx + 0x670 is cleared, so the game goes on as for a world view without an occlusion
// buffer. Bit 31 stays the game's, so the other views keep the flat cull's own frustum and small-object
// tests. And genProcessCull's count is held to the room while on (and 3 s after), so the extra meshes past it are
// dropped as the flush means them to be, not read past the array. Written: the job's own context and mask (its
// stack), MeshCullSettings.HiEnable while on, and that count where it's over the room.
namespace
{
  const __int64 OrthoTestRva = 0x36C7AD0;   // view 0 when ctx + 0x674 == 1
  const __int64 PerspTestRva = 0x36C77E0;   // view 0 otherwise
  const __int64 OrthoCallRva = 0x36D3FF8;   // their one caller, the chunk test 0x1436D3E20
  const __int64 PerspCallRva = 0x36D4001;
  const __int64 GenProcessCullRva = 0x3714D50; // the generic entities' visible list job (its thunk 0x143715270)
  const int CtxOcclusionOn = 0x670;         // byte, the world view has an occlusion buffer (read at 0x1436D427A)
  const int CtxListA = 0x3278;              // the deferred list's object (0x1436C05EB), on the job's stack
  const int ListAFromCtx = 0x32A0;          // job + 0xA7F0 - job + 0x7550 (0x1436D1363, 0x1436D1372)
  const int ListASrcBuffer = 0x3820;        // &src + 0x110 (0x1436BFE25, 0x1436BFE93)
  const int SrcBuffer = 0x110;
  const int SrcViewPosition = 0x30;         // copied to ctx + 0x640 (x lanes, 0x1436C0560)
  const int CtxViewPosition = 0x640;
  const int SrcWorldView = -0x10;           // src = slot + 0x20, the world view at slot + 0x10 (0x1437123C0, 0x143711EAB)
  const int WorldViewDesc = 8;              // (0x143712186)
  const int DescName = 8;
  const int DescType = 0x10;                // (0x14371218A)
  const int DescPosition = 0x680;           // (copied to src + 0x30)
  const int ChunkFlags = 0x500;             // byte per instance, 8: goes to the deferred list
  const BYTE OccludeeFlag = 8;
  const unsigned int MainViewBit = 1;       // view 0 (the caller passes 1)
  const unsigned int MaxChunkInstances = 64;
  const unsigned int NotSmallBit = 0x80000000u; // the game's "not too small" mark (constant at 0x142568AF0)
  // The view margin's slider (degrees around the view, as seen from the camera): this much or more is all around
  const float AllAroundDeg = 89.5f, MaxMarginDeg = 90.f, DefaultMarginDeg = 20.f;
  // Small objects' range (metres): this much or more is any distance; 0 leaves them to the game
  const float SmallRangeAny = 2000.f;
  // A plane the camera is within this of (metres) is a side or the near plane; one it's further inside of, the far
  // plane (the game's far plane is 25 km out; the tools' Draw distance slider goes down to 1 m)
  const float FarPlaneTol = 0.5f;
  const ULONGLONG LineEveryMs = 10000;
  const ULONGLONG GuardAfterOffMs = 3000;   // genProcessCull's count still held to the room this long after off
  // False keeps it inert in a build (no hooks, no menu entry, nothing written): it was, while under review (2026-11-15)
  const bool Released = true;
  // World view types not opened up (reflections)
  const int TypeStaticLocalIBL = 1, TypePlanarReflection = 4, TypeDynamicLocalIBL = 5;

  // The renderer objects (CameraManager.cpp's RendererOwnersRva: two owners, the renderer at owner + 0x48, vtable
  // 0x141BF41D0 written by its constructor at 0x1436C16BA) and their MeshCull settings (a new object per renderer,
  // made by its constructor; both renderers had the same one, read live 2026-11-15)
  const __int64 RendererOwnersRva = 0x24A9B98;
  const int RendererOwnerCount = 2;
  const int RendererOffset = 0x48;
  const __int64 RendererVTableRva = 0x1BF41D0;
  const int RendererMeshCull = 0x142918;    // written at 0x1436C184B (the settings container "MeshCull")
  const __int64 MeshCullVTableRva = 0x1BF4240; // MeshCullSettings (its TypeInfo 0x142840150 and default instance)
  const int MeshCullHiEnable = 0x44;        // Boolean (type info), read at 0x1436C6148

  typedef void(__fastcall* tMainViewTest)(__int64 ctx, __int64 chunk, float const* thresholds, unsigned int count,
    unsigned int* mask, unsigned int const* mask2, __int64 view, unsigned int viewBit);
  tMainViewTest oOrthoTest = nullptr;
  tMainViewTest oPerspTest = nullptr;
  // The job body: (job, slot); it reads nothing else at its start (0x143714D50 - 0x143714DAD)
  typedef __int64(__fastcall* tGenProcessCull)(__int64, __int64, __int64, __int64);
  tGenProcessCull oGenProcessCull = nullptr;

  bool g_Available = false;
  std::atomic<bool> g_On{ false };       // acting: wanted and the hooks are in
  std::atomic<bool> g_Wanted{ false };   // the user's choice, kept in the config even where it can't act
  std::atomic<float> g_MarginDeg{ DefaultMarginDeg };
  std::atomic<float> g_SmallRange{ SmallRangeAny };
  std::atomic<ULONGLONG> g_GuardUntil{ 0 };

  // The world views seen (the 10-s line lists them): their type and name, filled by the cull jobs
  const int ViewSlots = 8;
  struct ViewSeen
  {
    std::atomic<int> State{ 0 };         // 0 free, 1 being filled, 2 Type and Name set
    int Type = 0;
    __int64 Name = 0;
    std::atomic<unsigned int> X{ 0 }, Y{ 0 }, Z{ 0 };
  };
  ViewSeen g_Views[ViewSlots];

  // Counts from the cull jobs (many threads): a slot by thread, plain 64-bit counters
  struct alignas(64) CountSlot
  {
    std::atomic<unsigned long long> Changed{ 0 };     // chunk tests the tools changed
    std::atomic<unsigned long long> Tested{ 0 };      // their instances
    std::atomic<unsigned long long> Added{ 0 };       // instances given bit 1
    std::atomic<unsigned long long> Occludees{ 0 };   // instances that would have gone to the deferred list
    std::atomic<unsigned long long> KeptOut{ 0 };     // a threshold of +infinity: left to the game's test
    std::atomic<unsigned long long> AddedSmall{ 0 };  // of Added: the ones the game had found too small
    std::atomic<unsigned long long> LeftMargin{ 0 };  // left out: past the view margin
    std::atomic<unsigned long long> LeftSmall{ 0 };   // left out: too small, past the small objects' range
    std::atomic<unsigned long long> BadPlanes{ 0 };   // chunk tests with a plane that couldn't be normalised (skipped)
    std::atomic<unsigned long long> OddLayout{ 0 };   // perspective chunk tests whose planes weren't 5 at the camera + 1
    std::atomic<unsigned long long> NotOpened{ 0 };   // chunk tests of reflection world views
    std::atomic<unsigned long long> Unknown{ 0 };     // chunk tests whose world view couldn't be read: left alone
    std::atomic<unsigned long long> ViewChunks[ViewSlots];
    std::atomic<unsigned long long> Seen{ 0 };        // this slot's chunk tests, for the position samples
  };
  const int CountSlots = 16;
  CountSlot g_Counts[CountSlots];

  // genProcessCull's counts (once per world view a frame)
  std::atomic<unsigned int> g_ListRoom{ 0 }, g_ListMostAsked{ 0 };
  std::atomic<unsigned long long> g_ListOverHeld{ 0 }, g_ListOverByItself{ 0 };

  // The camera thread's (FrameUpdate) own
  bool g_LoggedOn = false;
  ULONGLONG g_OnSince = 0;
  ULONGLONG g_LineAt = 0;
  ULONGLONG g_OffLineAt = 0;
  unsigned long long g_Frames = 0;
  unsigned long long g_TotalAdded = 0, g_TotalTested = 0, g_TotalFrames = 0;
  // The settings as last logged, and a change waiting to settle (a slider being dragged) before it's logged
  float g_LoggedMargin = -1.f, g_LoggedRange = -1.f, g_PendingMargin = -1.f, g_PendingRange = -1.f;
  ULONGLONG g_PendingAt = 0;
  const ULONGLONG SettleMs = 1000;
  // HiEnable as the game had it, per settings object written, with the renderer owner it was found through
  struct SavedHiEnable
  {
    __int64 Object;
    BYTE Value;
    int Owner;
  };
  const int SavedSlots = 4;
  SavedHiEnable g_Saved[SavedSlots] = {};
  bool g_MeshCullWarned = false, g_NoRoomWarned = false;
  // For the menu (the last 10-s line's numbers a frame; -1: none yet) and the cull path
  std::atomic<int> g_ShownTested{ -1 }, g_ShownAdded{ 0 }, g_ShownOccludees{ 0 };
  std::atomic<int> g_ShownAddedSmall{ 0 }, g_ShownLeftMargin{ 0 }, g_ShownLeftSmall{ 0 };
  // 0 no renderer read, 1 flat as the game had it, 2 switched to flat, 3 an unknown settings object, 4 no room to keep
  // the game's value (not switched)
  std::atomic<int> g_PathState{ 0 };
  const char* const PathNames[] = { "no renderer read", "flat, as the game had it", "flat, switched from its tree path",
    "unknown settings object, not switched", "not switched (no room to keep the game's value)" };

  CountSlot& MySlot()
  {
    return g_Counts[(GetCurrentThreadId() >> 2) & (CountSlots - 1)];
  }

  unsigned int FloatBits(float f)
  {
    unsigned int u;
    memcpy(&u, &f, sizeof(u));
    return u;
  }

  float BitsFloat(unsigned int u)
  {
    float f;
    memcpy(&f, &u, sizeof(f));
    return f;
  }

  struct JobView
  {
    int Type;
    __int64 Name;
    float Position[3];
    bool Occlusion;    // the world view has an occlusion buffer
  };

  // The chunk test's world view, from its context back through the job's deferred list to src. False when any link
  // isn't what the code reads (then the tools leave the chunk alone).
  bool ReadJobView(__int64 ctx, JobView& out)
  {
    __try
    {
      __int64 listA = *(__int64 const*)(ctx + CtxListA);
      if (listA != ctx + ListAFromCtx)
        return false;
      __int64 bufferAt = *(__int64 const*)(listA + ListASrcBuffer);
      if (bufferAt < 0x10000)
        return false;
      __int64 src = bufferAt - SrcBuffer;
      if (*(unsigned int const*)(src + SrcViewPosition) != *(unsigned int const*)(ctx + CtxViewPosition))
        return false;
      __int64 view = *(__int64 const*)(src + SrcWorldView);
      if (view < 0x10000)
        return false;
      __int64 desc = *(__int64 const*)(view + WorldViewDesc);
      if (desc < 0x10000)
        return false;
      out.Type = *(int const*)(desc + DescType);
      out.Name = *(__int64 const*)(desc + DescName);
      memcpy(out.Position, (void const*)(desc + DescPosition), sizeof(out.Position));
      out.Occlusion = *(__int64 const*)bufferAt != 0;
      return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      return false;
    }
  }

  int ViewIndex(int type, __int64 name)
  {
    for (int i = 0; i < ViewSlots; ++i)
    {
      ViewSeen& v = g_Views[i];
      int state = v.State.load(std::memory_order_acquire);
      if (state == 2 && v.Type == type && v.Name == name)
        return i;
      if (state == 0)
      {
        // a free slot: claimed, filled, then shown as set (another thread filling it meanwhile: the next slot)
        int expected = 0;
        if (v.State.compare_exchange_strong(expected, 1, std::memory_order_acq_rel))
        {
          v.Type = type;
          v.Name = name;
          v.State.store(2, std::memory_order_release);
          return i;
        }
      }
    }
    return -1;
  }

  // After the game's own test of a chunk for view 0
  void AfterMainViewTest(__int64 ctx, __int64 chunk, float const* thresholds, unsigned int count, unsigned int* mask,
    __int64 view, unsigned int viewBit, bool persp)
  {
    if (!g_On.load(std::memory_order_relaxed) || g_shutdown || viewBit != MainViewBit || !mask || !thresholds)
      return;
    CountSlot& slot = MySlot();
    JobView job;
    if (!ReadJobView(ctx, job))
    {
      slot.Unknown.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    int index = ViewIndex(job.Type, job.Name);
    if (index >= 0)
    {
      slot.ViewChunks[index].fetch_add(1, std::memory_order_relaxed);
      if ((slot.Seen.fetch_add(1, std::memory_order_relaxed) & 255) == 0)
      {
        g_Views[index].X.store(FloatBits(job.Position[0]), std::memory_order_relaxed);
        g_Views[index].Y.store(FloatBits(job.Position[1]), std::memory_order_relaxed);
        g_Views[index].Z.store(FloatBits(job.Position[2]), std::memory_order_relaxed);
      }
    }
    if (job.Type == TypeStaticLocalIBL || job.Type == TypeDynamicLocalIBL || job.Type == TypePlanarReflection)
    {
      slot.NotOpened.fetch_add(1, std::memory_order_relaxed);
      return;
    }

    if (count > MaxChunkInstances)
      count = MaxChunkInstances;

    // view 0's position, the one the game's own size test measures from (ctx + 0x640 / 0x650 / 0x660)
    float cx = *(float const*)(ctx + CtxViewPosition), cy = *(float const*)(ctx + CtxViewPosition + 0x10),
      cz = *(float const*)(ctx + CtxViewPosition + 0x20);

    // The view margin: the game's own 6 planes of view 0 (SoA, x / y / z / w at view + 0x00 / 0x20 / 0x40 / 0x60, 4
    // bytes a plane; outside when centre . n + w > radius, as its test reads them), normalised here. The planes the
    // camera is on or inside of (the 4 sides and the near plane: within FarPlaneTol of it) are moved out by distance x
    // sin(margin), an angle around the view as seen from the camera; the far plane (the camera well inside it) keeps
    // the game's own cut, so the draw distance stays. That split is for the perspective test; its expected layout (5
    // planes at the camera, 1 behind it) is checked every call, and anything else, and the ortho test, move all 6
    // (counted). A plane that can't be normalised is skipped (counted). 89.5 degrees or more: all around.
    float marginDeg = g_MarginDeg.load(std::memory_order_relaxed);
    bool allAround = !(marginDeg < AllAroundDeg);
    float sinMargin = sinf((marginDeg > 0.f ? marginDeg : 0.f) * 0.01745329252f);
    float planes[6][4];
    bool usePlane[6] = {}, marginOn[6] = {};
    unsigned int badPlanes = 0, oddLayout = 0;
    if (!allAround)
    {
      float const* p = (float const*)view;
      int atCamera = 0, behind = 0;
      float side[6];
      for (int k = 0; k < 6; ++k)
      {
        float nx = p[k], ny = p[8 + k], nz = p[16 + k], w = p[24 + k];
        float length = sqrtf(nx * nx + ny * ny + nz * nz);
        if (!(length > 1e-6f) || !(length < 1e6f) || !(w == w))
        {
          ++badPlanes;
          continue;
        }
        planes[k][0] = nx / length;
        planes[k][1] = ny / length;
        planes[k][2] = nz / length;
        planes[k][3] = w / length;
        usePlane[k] = true;
        side[k] = cx * planes[k][0] + cy * planes[k][1] + cz * planes[k][2] + planes[k][3];
        if (side[k] >= -FarPlaneTol && side[k] <= FarPlaneTol)
          ++atCamera;
        else if (side[k] < -FarPlaneTol)
          ++behind;
      }
      bool split = persp && !badPlanes && atCamera == 5 && behind == 1;
      oddLayout = persp && !badPlanes && !split ? 1 : 0;
      for (int k = 0; k < 6; ++k)
        marginOn[k] = usePlane[k] && (!split || side[k] >= -FarPlaneTol);
    }
    // Small objects: let in only out to this far (<= 0: left to the game; SmallRangeAny or more: any distance)
    float smallRange = g_SmallRange.load(std::memory_order_relaxed);
    bool smallAny = !(smallRange < SmallRangeAny), smallOn = smallRange > 0.f;

    unsigned int added = 0, addedSmall = 0, leftMargin = 0, leftSmall = 0, keptOut = 0, occludees = 0;
    BYTE const* flags = (BYTE const*)(chunk + ChunkFlags);
    for (unsigned int i = 0; i < count; ++i)
    {
      unsigned int m = mask[i];
      if (!(m & MainViewBit))
      {
        // A threshold of +infinity: the game's size test (size < threshold) doesn't let it through unless one of its
        // force bits is set: left to the game's test (NaN and -infinity never count as small there)
        if (thresholds[i] > 3.4e38f)
        {
          ++keptOut;
          continue;
        }
        // The game's own answer: bit 31 = not too small (so without bit 1 it was outside the view); without bit 31 it
        // was too small (and perhaps outside the view too)
        bool tooSmall = !(m & NotSmallBit);
        float const* sphere = (float const*)(chunk + (__int64)i * 16);
        float dx = sphere[0] - cx, dy = sphere[1] - cy, dz = sphere[2] - cz;
        float distance = sqrtf(dx * dx + dy * dy + dz * dz);
        bool inMargin = true;
        if (!allAround)
        {
          float grow = distance * sinMargin;
          for (int k = 0; k < 6 && inMargin; ++k)
            if (usePlane[k])
              inMargin = !(sphere[0] * planes[k][0] + sphere[1] * planes[k][1] + sphere[2] * planes[k][2] + planes[k][3] >
                sphere[3] + (marginOn[k] ? grow : 0.f));
        }
        if (!inMargin)
        {
          ++leftMargin;
          continue;
        }
        if (tooSmall && !(smallAny || (smallOn && distance - sphere[3] <= smallRange)))
        {
          ++leftSmall;
          continue;
        }
        mask[i] = m | MainViewBit;
        ++added;
        addedSmall += tooSmall ? 1 : 0;
      }
      if (job.Occlusion && (flags[i] & OccludeeFlag))
        ++occludees;
    }
    *(BYTE*)(ctx + CtxOcclusionOn) = 0;

    slot.Changed.fetch_add(1, std::memory_order_relaxed);
    slot.Tested.fetch_add(count, std::memory_order_relaxed);
    if (addedSmall)
      slot.AddedSmall.fetch_add(addedSmall, std::memory_order_relaxed);
    if (leftMargin)
      slot.LeftMargin.fetch_add(leftMargin, std::memory_order_relaxed);
    if (leftSmall)
      slot.LeftSmall.fetch_add(leftSmall, std::memory_order_relaxed);
    if (added)
      slot.Added.fetch_add(added, std::memory_order_relaxed);
    if (occludees)
      slot.Occludees.fetch_add(occludees, std::memory_order_relaxed);
    if (keptOut)
      slot.KeptOut.fetch_add(keptOut, std::memory_order_relaxed);
    if (badPlanes)
      slot.BadPlanes.fetch_add(1, std::memory_order_relaxed);
    if (oddLayout)
      slot.OddLayout.fetch_add(1, std::memory_order_relaxed);
  }

  void __fastcall hOrthoTest(__int64 ctx, __int64 chunk, float const* thresholds, unsigned int count, unsigned int* mask,
    unsigned int const* mask2, __int64 view, unsigned int viewBit)
  {
    oOrthoTest(ctx, chunk, thresholds, count, mask, mask2, view, viewBit);
    AfterMainViewTest(ctx, chunk, thresholds, count, mask, view, viewBit, false);
  }

  void __fastcall hPerspTest(__int64 ctx, __int64 chunk, float const* thresholds, unsigned int count, unsigned int* mask,
    unsigned int const* mask2, __int64 view, unsigned int viewBit)
  {
    oPerspTest(ctx, chunk, thresholds, count, mask, mask2, view, viewBit);
    AfterMainViewTest(ctx, chunk, thresholds, count, mask, view, viewBit, true);
  }

  void StoreMax(std::atomic<unsigned int>& at, unsigned int value)
  {
    unsigned int now = at.load(std::memory_order_relaxed);
    while (value > now && !at.compare_exchange_weak(now, value, std::memory_order_relaxed))
    {
    }
  }

  // The world view's generic visible list: its room and the count asked for; held to the room when asked
  bool HoldToRoom(__int64 slot, bool hold, unsigned int& room, unsigned int& asked)
  {
    __try
    {
      room = *(unsigned int const*)slot;
      asked = *(unsigned int const*)(slot + 4);
      if (hold && asked > room)
        *(unsigned int volatile*)(slot + 4) = room;
      return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      return false;
    }
  }

  __int64 __fastcall hGenProcessCull(__int64 job, __int64 slot, __int64 a3, __int64 a4)
  {
    if (slot && !g_shutdown)
    {
      bool hold = g_On.load(std::memory_order_relaxed) || GetTickCount64() < g_GuardUntil.load(std::memory_order_relaxed);
      unsigned int room = 0, asked = 0;
      if (HoldToRoom(slot, hold, room, asked))
      {
        g_ListRoom.store(room, std::memory_order_relaxed);
        StoreMax(g_ListMostAsked, asked);
        if (asked > room)
          (hold ? g_ListOverHeld : g_ListOverByItself).fetch_add(1, std::memory_order_relaxed);
      }
    }
    return oGenProcessCull(job, slot, a3, a4);
  }

  bool Matches(BYTE const* pBase, __int64 rva, BYTE const* bytes, size_t size)
  {
    return !memcmp(pBase + rva, bytes, size);
  }

  bool CallsTo(BYTE const* pBase, __int64 rva, __int64 target)
  {
    return pBase[rva] == 0xE8 && rva + 5 + *(int const*)(pBase + rva + 1) == target;
  }

  // lea reg, [rip + x] (7 bytes) pointing at target
  bool LeaTo(BYTE const* pBase, __int64 rva, BYTE b0, BYTE b1, BYTE b2, __int64 target)
  {
    return pBase[rva] == b0 && pBase[rva + 1] == b1 && pBase[rva + 2] == b2 &&
      rva + 7 + *(int const*)(pBase + rva + 3) == target;
  }

  // The tests, their caller, the job, the world view loop and the generic visible list the change relies on, byte for
  // byte (read 2026-11-14 / 15 from the running game: the Steam build the tools' other hooks are for)
  bool CodeKnown(BYTE const* pBase)
  {
    // 0x1436C7AD0: mov rax, rsp; mov [rax + 0x10], rbx; push rbp, rsi, rdi; sub rsp, 0x210
    static const BYTE orthoStart[] = { 0x48, 0x89, 0xE0, 0x48, 0x89, 0x58, 0x10, 0x55, 0x56, 0x57, 0x48, 0x81, 0xEC, 0x10,
      0x02, 0x00, 0x00 };
    // 0x1436C7AFD: mov r11, [rsp + 0x260] (the view); movss xmm8, [rsp + 0x268] (the bit); mov r10, [rsp + 0x250] (mask)
    static const BYTE orthoArgs[] = { 0x4C, 0x8B, 0x9C, 0x24, 0x60, 0x02, 0x00, 0x00, 0xF3, 0x44, 0x0F, 0x10, 0x84, 0x24,
      0x68, 0x02, 0x00, 0x00, 0x4C, 0x8B, 0x94, 0x24, 0x50, 0x02, 0x00, 0x00 };
    // 0x1436C7D6A: orps xmm0, [r10 - 0x10]; movaps [r10 - 0x10], xmm0 (the mask written, 4 at a time)
    static const BYTE orthoWrite[] = { 0x41, 0x0F, 0x56, 0x42, 0xF0, 0x41, 0x0F, 0x29, 0x42, 0xF0 };
    // 0x1436C77E0: mov [rsp + 0x10], rbx; mov [rsp + 0x20], rsi; push rdi; sub rsp, 0x210
    static const BYTE perspStart[] = { 0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x20, 0x57, 0x48, 0x81, 0xEC,
      0x10, 0x02, 0x00, 0x00 };
    // 0x1436C780D: mov rax, [rsp + 0x250] (the view); movss xmm9, [rsp + 0x258] (the bit); mov rbx, [rsp + 0x240] (mask)
    static const BYTE perspArgs[] = { 0x48, 0x8B, 0x84, 0x24, 0x50, 0x02, 0x00, 0x00, 0xF3, 0x44, 0x0F, 0x10, 0x8C, 0x24,
      0x58, 0x02, 0x00, 0x00, 0x48, 0x8B, 0x9C, 0x24, 0x40, 0x02, 0x00, 0x00 };
    // 0x1436C7A4B: orps xmm0, [rbx - 0x10]; movaps [rbx - 0x10], xmm0
    static const BYTE perspWrite[] = { 0x0F, 0x56, 0x43, 0xF0, 0x0F, 0x29, 0x43, 0xF0 };
    // The caller (0x1436D3E20). 0x1436D3FBA: cmp dword [rbx + 0x674], 1; mov dword [rsp + 0x38], 1 (view 0's bit)
    static const BYTE callerPick[] = { 0x83, 0xBB, 0x74, 0x06, 0x00, 0x00, 0x01, 0xC7, 0x44, 0x24, 0x38, 0x01, 0x00, 0x00,
      0x00 };
    // 0x1436D3FB3: lea rdx, [r15 + 0x400] (the thresholds; again at 0x1436D3F73), 0x1436D3FDC: mov r8, rdx
    static const BYTE callerThresholds[] = { 0x49, 0x8D, 0x97, 0x00, 0x04, 0x00, 0x00 };
    static const BYTE callerThresholdsArg[] = { 0x49, 0x89, 0xD0 };
    // 0x1436D3FE4: lea rax, [rbp - 0x80] (the mask); mov r9d, r14d (the count); mov rcx, rbx (ctx); mov rdx, r15
    // (the chunk); mov [rsp + 0x20], rax
    static const BYTE callerArgs[] = { 0x48, 0x8D, 0x45, 0x80, 0x45, 0x89, 0xF1, 0x48, 0x89, 0xD9, 0x4C, 0x89, 0xFA, 0x48,
      0x89, 0x44, 0x24, 0x20 };
    // 0x1436D4166: mov r8d, [rbp + r11 * 4 - 0x80] (each instance's mask read back); 0x1436D4186: and r8, [rbx + 0x5F0]
    static const BYTE maskRead[] = { 0x46, 0x8B, 0x44, 0x9D, 0x80 };
    static const BYTE maskViews[] = { 0x4C, 0x23, 0x83, 0xF0, 0x05, 0x00, 0x00 };
    // 0x1436D425E: test [rbx + 0x5E0], r10d; 0x1436D426B: test byte [r15 + r11 + 0x500], 8; 0x1436D427A: cmp byte
    // [rbx + 0x670], 0 (the deferred list's three conditions)
    static const BYTE occlusionViews[] = { 0x44, 0x85, 0x93, 0xE0, 0x05, 0x00, 0x00 };
    static const BYTE occlusionFlag[] = { 0x43, 0xF6, 0x84, 0x1F, 0x00, 0x05, 0x00, 0x00, 0x08 };
    static const BYTE occlusionOn[] = { 0x80, 0xBB, 0x70, 0x06, 0x00, 0x00, 0x00 };
    // The setup (0x1436C0450) at 0x1436C058A: cmp qword [r14 + 0x110], 0; setne cl; mov [rbp + 0x670], cl
    static const BYTE setupOcclusion[] = { 0x49, 0x83, 0xBE, 0x10, 0x01, 0x00, 0x00, 0x00, 0x0F, 0x95, 0xD1, 0x88, 0x8D,
      0x70, 0x06, 0x00, 0x00 };
    // 0x1436C05EB: mov [rbp + 0x3278], rsi (the deferred list's object); 0x1436C0560: pshufd xmm0, [r14 + 0x30], 0;
    // movaps [rbp + 0x640], xmm0 (view 0's position from src)
    static const BYTE setupListA[] = { 0x48, 0x89, 0xB5, 0x78, 0x32, 0x00, 0x00 };
    static const BYTE setupPosition[] = { 0x66, 0x41, 0x0F, 0x70, 0x46, 0x30, 0x00, 0x0F, 0x29, 0x85, 0x40, 0x06, 0x00, 0x00 };
    // The list's object (0x1436BFE00) at 0x1436BFE25: lea rcx, [rbx + 0x110]; at 0x1436BFE93: mov [rsi + 0x3820], rcx
    static const BYTE listASrc[] = { 0x48, 0x8D, 0x8B, 0x10, 0x01, 0x00, 0x00 };
    static const BYTE listAKeep[] = { 0x48, 0x89, 0x8E, 0x20, 0x38, 0x00, 0x00 };
    // The job (0x1436D1230): 0x1436D133C mov rdx, rsi (src); lea rcx, [rbp + 0xA7F0]; call the list's object;
    // 0x1436D1363 lea r9, [rbp + 0xA7F0]; 0x1436D136F mov rdx, rsi; lea rcx, [rbp + 0x7550]; call the setup;
    // 0x1436D143F lea rcx, [rbp + 0x7550]; call the chunk test
    static const BYTE jobListA[] = { 0x48, 0x89, 0xF2, 0x48, 0x8D, 0x8D, 0xF0, 0xA7, 0x00, 0x00 };
    static const BYTE jobListAArg[] = { 0x4C, 0x8D, 0x8D, 0xF0, 0xA7, 0x00, 0x00 };
    static const BYTE jobSetup[] = { 0x48, 0x89, 0xF2, 0x48, 0x8D, 0x8D, 0x50, 0x75, 0x00, 0x00 };
    static const BYTE jobChunk[] = { 0x48, 0x8D, 0x8D, 0x50, 0x75, 0x00, 0x00 };
    // The world view loop (0x143711960): 0x143711EAB mov [rsi + 0x10], rdx (the world view); 0x1437123C0 lea r8,
    // [rsi + 0x20] (src); 0x143712186 mov rax, [r15 + 8]; cmp dword [rax + 0x10], 1 (the descriptor's type);
    // 0x143701F6D mov qword [r12 + 0x10], 4 (WorldPlanarReflection's type)
    static const BYTE loopView[] = { 0x48, 0x89, 0x56, 0x10 };
    static const BYTE loopSrc[] = { 0x4C, 0x8D, 0x46, 0x20 };
    static const BYTE loopType[] = { 0x49, 0x8B, 0x47, 0x08, 0x83, 0x78, 0x10, 0x01 };
    static const BYTE planarType[] = { 0x49, 0xC7, 0x44, 0x24, 0x10, 0x04, 0x00, 0x00, 0x00 };
    // The path pick (0x1436C5FB0) at 0x1436C6141: mov rax, [rbx + 0x142918]; cmp byte [rax + 0x44], 0; je (flat)
    static const BYTE pathPick[] = { 0x48, 0x8B, 0x83, 0x18, 0x29, 0x14, 0x00, 0x80, 0x78, 0x44, 0x00, 0x0F, 0x84, 0xDC,
      0x01, 0x00, 0x00 };
    // The renderer's constructor: 0x1436C184B mov [rdi + 0x142918], rax (MeshCull, its type 0x142840150 at 0x1436C1825)
    static const BYTE ctorMeshCull[] = { 0x48, 0x89, 0x87, 0x18, 0x29, 0x14, 0x00 };
    // The take-out (0x1436D27A0) keeps chunks packed, at 0x1436D295A: dec byte [rsi + rdi + 0x4134]
    static const BYTE takeOutPack[] = { 0xFE, 0x8C, 0x3E, 0x34, 0x41, 0x00, 0x00 };
    // genProcessCull (0x143714D50): push rbx, rbp, r12, r13, r15; sub rsp, 0x60; mov ebx, [rdx + 4] (the count); its
    // thunk 0x143715270 (jmp), started at 0x14371252A (lea r8, the thunk)
    static const BYTE genStart[] = { 0x53, 0x55, 0x41, 0x54, 0x41, 0x55, 0x41, 0x57, 0x48, 0x83, 0xEC, 0x60, 0x8B, 0x5A,
      0x04 };
    // The list's set-up (0x1437161F0) at 0x143716262: mov dword [rdx + 4], 0; mov rax, [r13 + 0x538]; mov r8d, [rax +
    // 0x1C0]; mov [rdx], r8d (the count 0, the room = GenericEntityMaxVisibleEntityCount)
    static const BYTE listSetup[] = { 0xC7, 0x42, 0x04, 0x00, 0x00, 0x00, 0x00, 0x49, 0x8B, 0x85, 0x38, 0x05, 0x00, 0x00,
      0x44, 0x8B, 0x80, 0xC0, 0x01, 0x00, 0x00, 0x44, 0x89, 0x02 };
    // The flush (0x14356A710) at 0x14356A82F: mov rax, [rsi + 0x10]; mov r8d, [rax] (the room); mov edx, r12d; lock
    // xadd [rax + 4], edx (the whole count added)
    static const BYTE flushAdd[] = { 0x48, 0x8B, 0x46, 0x10, 0x44, 0x8B, 0x00, 0x44, 0x89, 0xE2, 0xF0, 0x0F, 0xC1, 0x50, 0x04 };
    return Matches(pBase, OrthoTestRva, orthoStart, sizeof(orthoStart)) &&
      Matches(pBase, 0x36C7AFD, orthoArgs, sizeof(orthoArgs)) &&
      Matches(pBase, 0x36C7D6A, orthoWrite, sizeof(orthoWrite)) &&
      Matches(pBase, PerspTestRva, perspStart, sizeof(perspStart)) &&
      Matches(pBase, 0x36C780D, perspArgs, sizeof(perspArgs)) &&
      Matches(pBase, 0x36C7A4B, perspWrite, sizeof(perspWrite)) &&
      Matches(pBase, 0x36D3FBA, callerPick, sizeof(callerPick)) &&
      Matches(pBase, 0x36D3F73, callerThresholds, sizeof(callerThresholds)) &&
      Matches(pBase, 0x36D3FB3, callerThresholds, sizeof(callerThresholds)) &&
      Matches(pBase, 0x36D3FDC, callerThresholdsArg, sizeof(callerThresholdsArg)) &&
      Matches(pBase, 0x36D3FE4, callerArgs, sizeof(callerArgs)) &&
      CallsTo(pBase, OrthoCallRva, OrthoTestRva) && CallsTo(pBase, PerspCallRva, PerspTestRva) &&
      Matches(pBase, 0x36D4166, maskRead, sizeof(maskRead)) &&
      Matches(pBase, 0x36D4186, maskViews, sizeof(maskViews)) &&
      Matches(pBase, 0x36D425E, occlusionViews, sizeof(occlusionViews)) &&
      Matches(pBase, 0x36D426B, occlusionFlag, sizeof(occlusionFlag)) &&
      Matches(pBase, 0x36D427A, occlusionOn, sizeof(occlusionOn)) &&
      Matches(pBase, 0x36C058A, setupOcclusion, sizeof(setupOcclusion)) &&
      Matches(pBase, 0x36C05EB, setupListA, sizeof(setupListA)) &&
      Matches(pBase, 0x36C0560, setupPosition, sizeof(setupPosition)) &&
      Matches(pBase, 0x36BFE25, listASrc, sizeof(listASrc)) &&
      Matches(pBase, 0x36BFE93, listAKeep, sizeof(listAKeep)) &&
      Matches(pBase, 0x36D133C, jobListA, sizeof(jobListA)) && CallsTo(pBase, 0x36D1346, 0x36BFE00) &&
      Matches(pBase, 0x36D1363, jobListAArg, sizeof(jobListAArg)) &&
      Matches(pBase, 0x36D136F, jobSetup, sizeof(jobSetup)) && CallsTo(pBase, 0x36D1379, 0x36C0450) &&
      Matches(pBase, 0x36D143F, jobChunk, sizeof(jobChunk)) && CallsTo(pBase, 0x36D1446, 0x36D3E20) &&
      Matches(pBase, 0x3711EAB, loopView, sizeof(loopView)) &&
      Matches(pBase, 0x37123C0, loopSrc, sizeof(loopSrc)) && CallsTo(pBase, 0x37123F9, 0x36C5FB0) &&
      Matches(pBase, 0x3712186, loopType, sizeof(loopType)) &&
      Matches(pBase, 0x3701F6D, planarType, sizeof(planarType)) &&
      Matches(pBase, 0x36C6141, pathPick, sizeof(pathPick)) &&
      Matches(pBase, 0x36C184B, ctorMeshCull, sizeof(ctorMeshCull)) &&
      LeaTo(pBase, 0x36C1825, 0x48, 0x8D, 0x0D, 0x2840150) &&
      LeaTo(pBase, 0x36C16BA, 0x48, 0x8D, 0x05, RendererVTableRva) &&
      Matches(pBase, 0x36D295A, takeOutPack, sizeof(takeOutPack)) &&
      Matches(pBase, GenProcessCullRva, genStart, sizeof(genStart)) &&
      pBase[0x3715270] == 0xE9 && 0x3715270 + 5 + *(int const*)(pBase + 0x3715271) == GenProcessCullRva &&
      LeaTo(pBase, 0x371252A, 0x4C, 0x8D, 0x05, 0x3715270) &&
      Matches(pBase, 0x3716262, listSetup, sizeof(listSetup)) &&
      Matches(pBase, 0x356A82F, flushAdd, sizeof(flushAdd));
  }

  struct Totals
  {
    unsigned long long Changed, Tested, Added, Occludees, KeptOut, NotOpened, Unknown, ViewChunks[ViewSlots];
    unsigned long long AddedSmall, LeftMargin, LeftSmall, BadPlanes, OddLayout;
  };

  void TakeCounts(Totals& t)
  {
    memset(&t, 0, sizeof(t));
    for (CountSlot& slot : g_Counts)
    {
      t.Changed += slot.Changed.exchange(0, std::memory_order_relaxed);
      t.Tested += slot.Tested.exchange(0, std::memory_order_relaxed);
      t.Added += slot.Added.exchange(0, std::memory_order_relaxed);
      t.Occludees += slot.Occludees.exchange(0, std::memory_order_relaxed);
      t.KeptOut += slot.KeptOut.exchange(0, std::memory_order_relaxed);
      t.AddedSmall += slot.AddedSmall.exchange(0, std::memory_order_relaxed);
      t.LeftMargin += slot.LeftMargin.exchange(0, std::memory_order_relaxed);
      t.LeftSmall += slot.LeftSmall.exchange(0, std::memory_order_relaxed);
      t.BadPlanes += slot.BadPlanes.exchange(0, std::memory_order_relaxed);
      t.OddLayout += slot.OddLayout.exchange(0, std::memory_order_relaxed);
      t.NotOpened += slot.NotOpened.exchange(0, std::memory_order_relaxed);
      t.Unknown += slot.Unknown.exchange(0, std::memory_order_relaxed);
      for (int i = 0; i < ViewSlots; ++i)
        t.ViewChunks[i] += slot.ViewChunks[i].exchange(0, std::memory_order_relaxed);
    }
  }

  // The renderers' MeshCull settings objects (up to two; 0 for none, -1 for an object with another vtable)
  void FindMeshCull(__int64 objects[RendererOwnerCount])
  {
    __int64 base = (__int64)g_gameHandle;
    for (int o = 0; o < RendererOwnerCount; ++o)
    {
      objects[o] = 0;
      __try
      {
        __int64 pOwner = *(__int64 const*)(base + RendererOwnersRva + o * 8);
        if (pOwner < 0x10000)
          continue;
        __int64 pRenderer = *(__int64 const*)(pOwner + RendererOffset);
        if (pRenderer < 0x10000 || *(__int64 const*)pRenderer != base + RendererVTableRva)
          continue;
        __int64 pSettings = *(__int64 const*)(pRenderer + RendererMeshCull);
        if (pSettings < 0x10000)
          continue;
        if (*(__int64 const*)pSettings != base + MeshCullVTableRva)
        {
          objects[o] = -1;   // something else there: not written
          continue;
        }
        objects[o] = pSettings;
      }
      __except (EXCEPTION_EXECUTE_HANDLER)
      {
        objects[o] = 0;
      }
    }
  }

  bool ReadByte(__int64 at, BYTE& value)
  {
    __try
    {
      value = *(BYTE const*)at;
      return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      return false;
    }
  }

  bool WriteByte(__int64 at, BYTE value)
  {
    __try
    {
      *(BYTE volatile*)at = value;
      return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      return false;
    }
  }

  bool AnySaved()
  {
    for (SavedHiEnable const& s : g_Saved)
      if (s.Object)
        return true;
    return false;
  }

  // While on: the game's mesh cull on its flat path (MeshCullSettings.HiEnable 0, its value saved first); off: the
  // saved values written back, each only into an object its renderer still uses (checked by its vtable). Nothing
  // during the loading screen, when renderers and their settings come and go.
  void ApplyCullPath(bool on)
  {
    if (gamestate::Loading())
      return;
    __int64 objects[RendererOwnerCount];
    FindMeshCull(objects);
    // A saved object its renderer no longer uses, while that renderer uses another one: the game replaced it
    // (each renderer makes its own): forgotten, not written (it may be gone)
    for (SavedHiEnable& s : g_Saved)
    {
      if (!s.Object)
        continue;
      int live = -1;
      for (int o = 0; o < RendererOwnerCount; ++o)
        if (objects[o] == s.Object)
          live = o;
      if (live >= 0)
        s.Owner = live;
      else if (objects[s.Owner] > 0)
      {
        util::log::Write("No culling: the renderer's MeshCull settings were replaced (0x%I64X -> 0x%I64X): the old one "
          "is forgotten, not written", s.Object, objects[s.Owner]);
        s.Object = 0;
      }
    }
    bool any = false, foreign = false, noRoom = false, switched = false;
    for (int o = 0; o < RendererOwnerCount; ++o)
    {
      __int64 object = objects[o];
      if (object == -1)
        foreign = true;
      if (object <= 0)
        continue;
      any = true;
      // the same object for both renderers is handled once
      if (o == 1 && object == objects[0])
        continue;
      SavedHiEnable* saved = nullptr;
      for (SavedHiEnable& s : g_Saved)
        if (s.Object == object)
          saved = &s;
      BYTE value;
      if (!ReadByte(object + MeshCullHiEnable, value))
        continue;
      if (on)
      {
        if (!saved)
        {
          for (SavedHiEnable& s : g_Saved)
            if (!s.Object)
            {
              saved = &s;
              break;
            }
          if (!saved)
          {
            noRoom = true;
            if (!g_NoRoomWarned)
            {
              g_NoRoomWarned = true;
              util::log::Warning("No culling: no room left to keep the game's MeshCull HiEnable (settings at 0x%I64X): "
                "its path isn't switched", object);
            }
            continue;
          }
          saved->Object = object;
          saved->Value = value;
          saved->Owner = o;
          if (value)
            util::log::Write("No culling: the game's mesh cull was on its tree path (MeshCull HiEnable %u, settings at "
              "0x%I64X): every world view switched to its flat path (HiEnable 0) while on", (unsigned)value, object);
          else
            util::log::Write("No culling: the game's mesh cull is on its flat path already (MeshCull HiEnable 0, settings "
              "at 0x%I64X): nothing to switch", object);
        }
        if (value != 0)
          WriteByte(object + MeshCullHiEnable, 0);
        switched |= saved->Value != 0;
      }
      else if (saved)
      {
        if (value != saved->Value)
        {
          WriteByte(object + MeshCullHiEnable, saved->Value);
          util::log::Write("No culling: the game's MeshCull HiEnable is back to %u (settings at 0x%I64X)",
            (unsigned)saved->Value, object);
        }
        saved->Object = 0;
      }
    }
    if (on)
    {
      g_PathState.store(!any ? (foreign ? 3 : 0) : (noRoom ? 4 : (switched ? 2 : 1)));
      if (foreign && !g_MeshCullWarned)
      {
        g_MeshCullWarned = true;
        util::log::Warning("No culling: a renderer's MeshCull settings aren't the object the tools know, its mesh cull path "
          "isn't changed");
      }
    }
  }

  // A world view's name, if it's a readable string
  void ReadName(__int64 name, char* out, size_t size)
  {
    out[0] = 0;
    if (name < 0x10000)
      return;
    __try
    {
      size_t i = 0;
      for (; i + 1 < size; ++i)
      {
        char c = ((char const*)name)[i];
        if (!c)
          break;
        if (c < 32 || c > 126)
        {
          i = 0;
          break;
        }
        out[i] = c;
      }
      out[i] = 0;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      out[0] = 0;
    }
  }

  // "20 degrees" / "all around", and "out to 300 m" / "at any distance" / "left to the game"
  void DescribeSettings(char* margin, size_t marginSize, char* range, size_t rangeSize)
  {
    float m = g_MarginDeg.load(), r = g_SmallRange.load();
    if (!(m < AllAroundDeg))
      _snprintf_s(margin, marginSize, _TRUNCATE, "all around");
    else
      _snprintf_s(margin, marginSize, _TRUNCATE, "%.0f degrees", m);
    if (!(r < SmallRangeAny))
      _snprintf_s(range, rangeSize, _TRUNCATE, "at any distance");
    else if (r > 0.f)
      _snprintf_s(range, rangeSize, _TRUNCATE, "out to %.0f m", r);
    else
      _snprintf_s(range, rangeSize, _TRUNCATE, "left to the game");
  }

  // "type 1 'Name' at x y z: N chunk tests a frame (a reflection: not opened up)", one per world view seen in 10 s
  void DescribeViews(Totals const& t, double frames, char* out, size_t size)
  {
    out[0] = 0;
    size_t used = 0;
    for (int i = 0; i < ViewSlots; ++i)
    {
      if (g_Views[i].State.load(std::memory_order_acquire) != 2 || !t.ViewChunks[i])
        continue;
      int type = g_Views[i].Type;
      char name[48];
      ReadName(g_Views[i].Name, name, sizeof(name));
      bool reflection = type == TypeStaticLocalIBL || type == TypeDynamicLocalIBL || type == TypePlanarReflection;
      int n = _snprintf_s(out + used, size - used, _TRUNCATE, "%stype %d '%s' at %.1f %.1f %.1f: %.0f chunk tests a frame%s",
        used ? "; " : "", type, name[0] ? name : "?", BitsFloat(g_Views[i].X.load()), BitsFloat(g_Views[i].Y.load()),
        BitsFloat(g_Views[i].Z.load()), t.ViewChunks[i] / frames, reflection ? " (a reflection: not opened up)" : "");
      if (n < 0)
        break;
      used += (size_t)n;
    }
    if (!used)
      _snprintf_s(out, size, _TRUNCATE, "none read");
  }
}

void culling_off::Init()
{
  if (!Released)
    return;
  if (!CodeKnown((BYTE const*)g_gameHandle))
  {
    util::log::Warning("No culling: the game's mesh cull isn't the code the tools know, GAME > Level streaming > No "
      "culling is off");
    return;
  }
  __int64 base = (__int64)g_gameHandle;
  // The guard first: without it the extra meshes could make genProcessCull read past its list
  bool hooked = util::hooks::CreateGameHook("GenericVisibleListGuard", base + GenProcessCullRva, hGenProcessCull,
    (LPVOID*)&oGenProcessCull);
  hooked = hooked && util::hooks::CreateGameHook("MainViewCullOrtho", base + OrthoTestRva, hOrthoTest, (LPVOID*)&oOrthoTest);
  hooked = hooked && util::hooks::CreateGameHook("MainViewCullPerspective", base + PerspTestRva, hPerspTest,
    (LPVOID*)&oPerspTest);
  // All or nothing: a hook made stays, but passes everything through while it isn't on (the tick isn't shown)
  g_Available = hooked && oGenProcessCull && oOrthoTest && oPerspTest;
  if (!g_Available)
  {
    g_On.store(false);
    util::log::Warning("No culling: not all its hooks are in, GAME > Level streaming > No culling is off");
  }
}

bool culling_off::IsAvailable()
{
  return g_Available;
}

void culling_off::SetEnabled(bool on)
{
  g_Wanted.store(on);
  g_On.store(on && g_Available);
}

bool culling_off::GetEnabled()
{
  return g_Wanted.load();
}

void culling_off::SetViewMargin(float degrees)
{
  if (!(degrees >= 0.f))
    degrees = 0.f;
  g_MarginDeg.store(degrees > MaxMarginDeg ? MaxMarginDeg : degrees);
}

float culling_off::GetViewMargin()
{
  return g_MarginDeg.load();
}

void culling_off::SetSmallRange(float metres)
{
  // under 1 m: left to the game; within half a metre of the end: any distance (as the slider shows them)
  if (!(metres >= 1.f))
    metres = 0.f;
  g_SmallRange.store(metres > SmallRangeAny - 0.5f ? SmallRangeAny : metres);
}

float culling_off::GetSmallRange()
{
  return g_SmallRange.load();
}

void culling_off::FrameUpdate(float x, float y, float z)
{
  if (!g_Available)
    return;
  ULONGLONG now = GetTickCount64();
  bool on = g_On.load();
  bool edge = on != g_LoggedOn;
  ++g_Frames;
  if (edge && on)
  {
    Totals t;
    TakeCounts(t);
    char margin[48], range[48];
    DescribeSettings(margin, sizeof(margin), range, sizeof(range));
    util::log::Write("No culling: on (view margin %s, small objects %s). While on, the game culls every world view with its "
      "flat mesh cull (MeshCull HiEnable 0; by default it uses its tree cull), and the camera's view lets loaded meshes "
      "through the frustum test within the view margin and through the small-object test within the small objects' "
      "range, and skips the occlusion test. The other views (shadows, reflection captures, planar reflections) aren't "
      "opened up but are culled by the flat cull's tests; particles, decals and lights aren't part of the mesh cull. It "
      "costs fps.", margin, range);
    g_OnSince = now;
    g_LoggedMargin = g_PendingMargin = g_MarginDeg.load();
    g_LoggedRange = g_PendingRange = g_SmallRange.load();
    g_TotalAdded = g_TotalTested = g_TotalFrames = 0;
    g_ShownTested.store(-1);
    g_ListMostAsked.store(0);
    g_ListOverHeld.store(0);
    unsigned long long byItself = g_ListOverByItself.exchange(0);
    if (byItself && util::DiagnosticsOn())
      util::log::Write("No culling (guard): before it went on, the game's generic visible list was asked for more than "
        "its room (%u) %llu times by the game itself (left as the game has it)", g_ListRoom.load(), byItself);
  }
  // The cull path: switched every frame while on (the game may reset its settings), given back once off (each saved
  // value as soon as its renderer is read again)
  if (on || AnySaved())
    ApplyCullPath(on);
  if (edge && !on)
  {
    // genProcessCull's count still held for frames culled while it was on
    g_GuardUntil.store(now + GuardAfterOffMs);
    Totals t;
    TakeCounts(t);
    g_TotalAdded += t.Added;
    g_TotalTested += t.Tested;
    g_TotalFrames += g_Frames;
    double frames = g_TotalFrames ? (double)g_TotalFrames : 1.0;
    util::log::Write("No culling: off after %.0f s. The game culls the camera's view with its own tests again (they "
      "weren't changed, only their results); its MeshCull path %s. While on: %.0f meshes a frame let into the camera's "
      "view that the game's tests left out, of %.0f tested a frame",
      (now - g_OnSince) / 1000.0, AnySaved() ? "goes back as the game had it once its renderer is read again" :
      "is as the game had it (line above when it was switched)", g_TotalAdded / frames, g_TotalTested / frames);
    g_PathState.store(0);
  }
  if (edge)
  {
    g_LoggedOn = on;
    g_LineAt = now;
    g_OffLineAt = now;
    g_Frames = 0;
    return;
  }
  if (!on)
  {
    // Off: only if the game itself went past its generic visible list's room (left as the game has it)
    if (now - g_OffLineAt >= LineEveryMs)
    {
      g_OffLineAt = now;
      unsigned long long byItself = g_ListOverByItself.exchange(0);
      if (byItself && util::DiagnosticsOn())
        util::log::Write("No culling (guard, 10 s): the game's generic visible list was asked for more than its room (%u) "
          "%llu times by the game itself, with No culling off (left as the game has it)", g_ListRoom.load(), byItself);
    }
    return;
  }
  // A setting changed while on: logged once it has held for a second (so a 10-s line's settings can be matched)
  {
    float margin = g_MarginDeg.load(), range = g_SmallRange.load();
    if (margin != g_LoggedMargin || range != g_LoggedRange)
    {
      if (margin != g_PendingMargin || range != g_PendingRange)
      {
        g_PendingMargin = margin;
        g_PendingRange = range;
        g_PendingAt = now;
      }
      else if (now - g_PendingAt >= SettleMs)
      {
        char marginText[48], rangeText[48];
        DescribeSettings(marginText, sizeof(marginText), rangeText, sizeof(rangeText));
        util::log::Write("No culling: now view margin %s, small objects %s (the next 10-s line mixes both settings)",
          marginText, rangeText);
        g_LoggedMargin = margin;
        g_LoggedRange = range;
      }
    }
  }
  if (now - g_LineAt < LineEveryMs)
    return;

  Totals t;
  TakeCounts(t);
  double frames = g_Frames ? (double)g_Frames : 1.0;
  g_TotalAdded += t.Added;
  g_TotalTested += t.Tested;
  g_TotalFrames += g_Frames;
  g_Frames = 0;
  g_LineAt = now;
  int path = g_PathState.load();
  char const* pathName = PathNames[path >= 0 && path <= 4 ? path : 0];
  unsigned int room = g_ListRoom.load(), mostAsked = g_ListMostAsked.exchange(0);
  unsigned long long overHeld = g_ListOverHeld.exchange(0), byItself = g_ListOverByItself.exchange(0);
  if (!t.Changed && !t.NotOpened && !t.Unknown)
  {
    if (util::DiagnosticsOn())
      util::log::Write("No culling (10 s): the game's flat mesh cull wasn't called (cull path: %s; no level drawn?)", pathName);
    g_ShownTested.store(-1);
    return;
  }
  g_ShownTested.store((int)(t.Tested / frames + 0.5));
  g_ShownAdded.store((int)(t.Added / frames + 0.5));
  g_ShownAddedSmall.store((int)(t.AddedSmall / frames + 0.5));
  g_ShownLeftMargin.store((int)(t.LeftMargin / frames + 0.5));
  g_ShownLeftSmall.store((int)(t.LeftSmall / frames + 0.5));
  g_ShownOccludees.store((int)(t.Occludees / frames + 0.5));
  // The 10-s line itself: a diagnostic (the menu reads the numbers above)
  if (!util::DiagnosticsOn())
    return;
  char views[640];
  DescribeViews(t, frames, views, sizeof(views));
  char margin[48], range[48];
  DescribeSettings(margin, sizeof(margin), range, sizeof(range));
  // (the counters are taken one after another while the jobs run, so "too small" can be a little ahead of the total)
  double outsideView = t.Added > t.AddedSmall ? (double)(t.Added - t.AddedSmall) : 0.0;
  char line[2000];
  _snprintf_s(line, sizeof(line), _TRUNCATE, "No culling (10 s): view margin %s, small objects %s; cull path %s; %.0f chunk "
    "tests a frame changed (%.0f meshes); %.0f a frame let into the camera's view that the game's frustum and size tests "
    "left out (%.0f %%; mask bits, not what's drawn): %.0f outside the view (within the margin), %.0f too small; left to "
    "the game: %.0f a frame past the view margin, %.0f small ones past their range; %.0f a frame not sent to the "
    "occlusion test, %llu left to the game's test in 10 s (a size threshold of infinity); %.0f chunk tests a frame not "
    "opened up (reflections), %llu in 10 s not read (left alone); the planes: %llu chunk tests in 10 s with a plane that "
    "couldn't be read (skipped), %llu not the expected 5 at the camera + 1 (the margin on all 6); the generic visible "
    "list: at most %u asked of a room of %u, over it %llu times (held to the room) and %llu times by the game itself; "
    "world views: %s; the camera at %.1f %.1f %.1f",
    margin, range, pathName, t.Changed / frames, t.Tested / frames, t.Added / frames,
    t.Tested ? 100.0 * t.Added / t.Tested : 0.0, outsideView / frames, t.AddedSmall / frames,
    t.LeftMargin / frames, t.LeftSmall / frames, t.Occludees / frames, t.KeptOut, t.NotOpened / frames, t.Unknown,
    t.BadPlanes, t.OddLayout, mostAsked, room, overHeld, byItself, views, x, y, z);
  util::log::Write("%s", line);
}

bool culling_off::DrawUI()
{
  if (!g_Available)
  {
    if (Released)
      ImGui::TextDisabled("No culling: not with this game build");
    return false;
  }
  bool changed = false;
  bool on = g_On.load();
  if (ImGui::Checkbox("No culling (camera view)", &on))
  {
    SetEnabled(on);
    changed = true;
  }
  ImGui::HelpMarker("Turns off the game's mesh culling for the camera's view: meshes behind other things are no longer "
    "left out (the occlusion test is skipped), nor are small ones (within the small objects' range), nor ones just "
    "outside the view (within the view margin). For that the game culls with its own flat mesh cull instead of its tree "
    "cull (its MeshCull HiEnable setting, given back when it's off). What's loaded doesn't change: that's the settings "
    "below.\n"
    "Not opened up, but culled by that flat cull's tests while it's on: the shadows and the other views, reflection "
    "captures and planar reflections (so they can differ a little from the game's own). Particles, decals and lights "
    "aren't part of the mesh cull.\n"
    "It costs fps: the game draws much more every frame. Meshes outside the view can't be seen in the picture, so a "
    "smaller margin lets far fewer in than all around (the margin at its top).\n"
    "Off gives the game's own culling back.");
  if (on)
  {
    ImGui::PushID("NoCulling");
    ImGui::Indent();
    float margin = g_MarginDeg.load();
    if (ImGui::SliderFloat("View margin", &margin, 0.f, MaxMarginDeg,
      margin < AllAroundDeg ? "%.0f" UI_DEG " around the view" : "all around"))
    {
      SetViewMargin(margin);
      changed = true;
    }
    ImGui::ItemTooltip("Meshes outside the view are let in out to this angle past its edges (as seen from the camera), "
      "for a camera turning faster than the game's culling follows. They can't be seen in the "
      "picture, so a wider margin draws more for nothing. The draw distance stays the game's. At the top: all around.");
    float range = g_SmallRange.load();
    if (ImGui::SliderFloat("Small objects", &range, 0.f, SmallRangeAny,
      range <= 0.f ? "the game's (left out when small)" : (range < SmallRangeAny ? "drawn out to %.0f m" : "drawn at any distance"),
      2.f))
    {
      SetSmallRange(range < 1.f ? 0.f : range);
      changed = true;
    }
    ImGui::ItemTooltip("Meshes the game leaves out for being too small on screen are drawn out to this distance from the "
      "camera. At the left the game's own size test stays; at the right they're drawn at any distance.");
    int tested = g_ShownTested.load();
    ImGui::PushTextWrapPos(0.f);
    int path = g_PathState.load();
    if (path == 3 || path == 4)
      ImGui::TextDisabled("The game's mesh cull path isn't switched: nothing changes (CT.log)");
    else if (tested >= 0)
    {
      int added = g_ShownAdded.load(), addedSmall = g_ShownAddedSmall.load();
      if (addedSmall > added)
        addedSmall = added;
      ImGui::TextDisabled("Last 10 s, a frame: %d meshes let into the camera's view that the game left out (%d outside the "
        "view, %d too small) of %d tested; %d not sent to the occlusion test. Left to the game: %d past the view margin, %d "
        "small ones past their range.", added, added - addedSmall, addedSmall, tested, g_ShownOccludees.load(), g_ShownLeftMargin.load(),
        g_ShownLeftSmall.load());
    }
    else
      ImGui::TextDisabled("Its numbers come every 10 s with a level drawn");
    ImGui::PopTextWrapPos();
    ImGui::Unindent();
    ImGui::PopID();
  }
  return changed;
}
