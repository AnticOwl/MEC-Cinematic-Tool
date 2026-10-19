#include "CameraManager.h"
#include "../Main.h"
#include "../Util/Util.h"
#include "../imgui/imgui.h"
#include "../Util/ImGuiEXT.h"
#include <Windows.h>
#include <algorithm>

using namespace DirectX;

namespace
{
  // Mirror's Edge Catalyst structure offsets
  const int RenderViewOffset     = 0x58; // GameRenderer -> RenderView* (the main view)
  const int RenderViewTransform0 = 0x00; // LinearTransform
  const int RenderViewTransform1 = 0x40; // LinearTransform, +0x70 is the camera position
  const int ForceFovOffset       = 0x54; // GameRenderSettings, float degrees, -1 = no override
  const int TimeScaleOffset      = 0x48; // GameTimeSettings, float
  const int DrawHudOffset        = 0x48; // UISettings, bool
  const float FaithEyeHeight     = 1.6f;  // Meters above the soldier transform
  const int RenderViewTransform2 = 0x130; // Another copy of the camera transform
  const int FarPlaneOffset       = 0xF0;  // RenderView, float meters (near plane is +0xEC), game default 25000
  const int RenderViewFovOffset    = 0xE4; // float radians, the vertical field of view (ForceFov)
  const int RenderViewAspectOffset = 0xF4; // float, width / height
  const int LodScaleOffsetA      = 0x24;  // MeshSettings GlobalLodScale, float, game default 5
  const int LodScaleOffsetB      = 0x28;  // MeshSettings ShadowDistanceScale, float, game default 5
  // WorldRenderSettings fields, from the game's own type info
  const int MotionBlurScaleOffset  = 0x78;  // float, game default 0.5
  const int MotionBlurMaxOffset    = 0x80;  // float, game default 0.1
  const int MotionBlurEnableOffset = 0x145; // bool
  const int CullScreenAreaScaleOffset = 0x10; // float, game value 5 (Frostbite's default is 1)
  const int ShadowmapViewDistanceOffset = 0x64; // float, game value 200 m: sun shadows only reach this far from the camera
  const int ShadowmapTransitionBlendOffset = 0x6C;   // float, game 0.03: fade between the shadow cascades
  const int ShadowmapSliceSchemeWeightOffset = 0x5C; // float, game 0.88: log (1) / even (0) cascade splits

  // Reads a game object pointer from the address of a static pointer
  __int64 ReadPointer(__int64 address)
  {
    if (!address) return 0;
    return *(__int64*)address;
  }

  // Zone streaming. Each ZoneStreamerEntity
  // keeps a list of foci and loads the city zones (and their neighbours) that
  // they're in. The game's own focus is its camera, which stays with Faith.
  const int ZoneFocusListOffset     = 0x280; // ZoneStreamerEntity -> focus list
  const int ZoneFocusRecordsOffset  = 0x08;  // focus list -> records
  const int ZoneFocusCountOffset    = 0x48;  // focus list -> record count, uint32
  const unsigned int ZoneFocusRemoved = 1;   // record flag, the record goes after the next update
  const unsigned int MaxZoneFoci    = 256;   // the game has a handful, more means the list isn't valid
  const int MaxStreamingFoci = 72;        // the tools' own: camera, ahead, rings, track points
  const int FocusCamera = 0;
  const int FocusAhead = 1;
  const int FocusRingFirst = 2;           // rings around the camera, out to the stream distance
  const int MaxRingFoci = 40;             // enough for 600 m
  const int FocusTrackFirst = FocusRingFirst + MaxRingFoci;
  const float RingSpacing = 200.f;        // between the rings, and between the points on one
  const float MaxStreamDistance = 600.f;  // all round; the zone slot and mesh limits hold back what doesn't fit
  // Only where the camera looks: about a third of each ring, so it reaches
  // further for the same zones and meshes
  const float MaxStreamDistanceInView = 1000.f;
  const float ViewConeMargin = 20.f * 3.14159265f / 180.f; // past the edges of the view
  const float LookAheadSeconds = 2.5f;     // how far ahead the camera's movement is loaded
  const float LookAheadMaxDistance = 400.f;

  struct ZoneFocusRecord
  {
    __int64 Key;        // passed back to the focus, 0 for focus entities
    void* pFocus;
    int Zone;           // zone the focus is in, -1 for none
    unsigned int Flags;
  };

  // What the streamer gets from a focus each update
  struct ZoneFocusResult
  {
    float Position[4];
    float Direction[4]; // the camera's forward row, like the game's camera focus
  };

  // The streamer only calls a focus' first virtual function. These foci only
  // read the static data below, so they stay safe if the game asks for one
  // while the tools shut down. 0 is the camera, 1 a point ahead of it, the
  // rest are points along the selected camera track.
  struct StreamingFocus
  {
    void** pVTable;
    int Index;
  };

  SRWLOCK g_FocusLock = SRWLOCK_INIT;
  ZoneFocusResult g_FocusResults[MaxStreamingFoci]{};
  bool g_FocusWanted[MaxStreamingFoci]{};

  void GetStreamingFocus(StreamingFocus* pFocus, __int64 /*key*/, ZoneFocusResult* pResult)
  {
    AcquireSRWLockShared(&g_FocusLock);
    *pResult = g_FocusResults[pFocus->Index];
    ReleaseSRWLockShared(&g_FocusLock);
  }

  __int64 ZoneFocusNoop() { return 0; }

  // The game's focus interface has 4 functions (slot 1 and 2 return 0). The
  // rest are no-ops too, so a call on any other slot can't jump into garbage.
  void* g_FocusVTable[16] = { (void*)&GetStreamingFocus,
    (void*)&ZoneFocusNoop, (void*)&ZoneFocusNoop, (void*)&ZoneFocusNoop, (void*)&ZoneFocusNoop,
    (void*)&ZoneFocusNoop, (void*)&ZoneFocusNoop, (void*)&ZoneFocusNoop, (void*)&ZoneFocusNoop,
    (void*)&ZoneFocusNoop, (void*)&ZoneFocusNoop, (void*)&ZoneFocusNoop, (void*)&ZoneFocusNoop,
    (void*)&ZoneFocusNoop, (void*)&ZoneFocusNoop, (void*)&ZoneFocusNoop };
  StreamingFocus g_Foci[MaxStreamingFoci];

  // The game's ZoneStreamerFocusEntity makes the same calls when it's enabled and disabled
  typedef void(__fastcall* tZoneFocusChange)(__int64 pFocusList, __int64 key, void* pFocus);

  // Adds and removes the tools' foci in one streamer's list to match
  // wanted[]. Gives how many were in the list before and after. The game's
  // remove function writes out of bounds for a focus that isn't there, so
  // it's only called for ones found in the list.
  // How full a streamer's zone slots are. Each ZoneStreamerEntity has 128
  // slots for loaded zones (streamer + 0x2F8). The game's slot search
  // (0x143C80180) doesn't handle running out: it writes past the table into
  // other memory, which crashes the game sooner or later. The tools only add
  // their foci while there's plenty of room, and take some away when it fills.
  const int ZoneSlotsOffset = 0x2F8;
  const int ZoneSlotCount = 128;
  const int ZoneSlotsAddBelow = 100;    // add one more focus only below this many in use
  const int ZoneSlotsRemoveAbove = 108; // take the least important ones away above this
  // Zones only get their slots once they load, so the count runs behind what
  // was asked for. A new focus is added only once the count has stood still
  // this long, and never sooner than this after the last one.
  const ULONGLONG ZoneSlotsSettleMs = 750;

  // Per streamer: the slot count last seen, when it last changed, when a focus was last added
  struct StreamerGate
  {
    __int64 Streamer;
    int LastUsed;
    ULONGLONG LastChange;
    ULONGLONG LastAdd;
  };
  StreamerGate g_Gates[8]{};
  SRWLOCK g_GateLock = SRWLOCK_INIT;

  // Settled and room for one more?
  bool MayAddFocus(__int64 pZoneStreamer, int used)
  {
    ULONGLONG now = GetTickCount64();
    AcquireSRWLockExclusive(&g_GateLock);
    StreamerGate* pGate = nullptr;
    for (StreamerGate& gate : g_Gates)
      if (gate.Streamer == pZoneStreamer) { pGate = &gate; break; }
    if (!pGate)
    {
      // A new streamer (or one that came back after a level load) takes the oldest entry
      pGate = &g_Gates[0];
      for (StreamerGate& gate : g_Gates)
        if (gate.LastChange < pGate->LastChange) pGate = &gate;
      *pGate = { pZoneStreamer, used, now, 0 };
    }
    if (used != pGate->LastUsed)
    {
      pGate->LastUsed = used;
      pGate->LastChange = now;
    }
    bool settled = now - pGate->LastChange >= ZoneSlotsSettleMs && now - pGate->LastAdd >= ZoneSlotsSettleMs;
    bool allowed = settled && used < ZoneSlotsAddBelow;
    if (allowed)
      pGate->LastAdd = now;
    ReleaseSRWLockExclusive(&g_GateLock);
    return allowed;
  }

  // The mesh streaming manager keeps two lists of model instances (positioned
  // and distanced) that background jobs read through pointers copied at the
  // start of each job. The game sets aside room for them when a level loads
  // (MeshStreamingSettings ReservedPositionedInstanceCount / ...Distanced...);
  // when more instances register than that, a list is moved to new memory and
  // a job still reading the old one crashes. Extra zones around the camera
  // bring in a lot of instances, so the tools keep an eye on how full they are.
  const int InstanceListsOffset = 0x350070;   // two vectors, 0x20 apart: begin, end, capacity, allocator
  const int InstanceRecordSize = 0x20;
  const int StreamingSettingsOffset = 0x386480; // MeshStreamingSettings*
  const int ReservedCountOffset = 0x58;         // positioned at +0x58, distanced at +0x5C

  // The mesh table: a vector at +0x50 (begin, end, capacity) with room for
  // 4096 meshes of 0x350 bytes inside the manager (+0x70 up to the instance
  // lists at +0x350070). A new mesh takes a free record (first qword 0) if
  // there is one, otherwise the end moves on (0x143551D50), without looking
  // at the capacity: past 4096 it writes over the instance lists, and the
  // game crashes. So the end only shows the most there ever were in this
  // level; what counts is how many records are in use.
  const int MeshTableOffset = 0x50;
  const int MeshTableInline = 0x70;
  const int MeshRecordSize = 0x350;
  const int MeshTableCapacity = 4096;
  // One zone can bring in a few hundred meshes at once, so the limits leave
  // that much room below 4096 (4000 still went over it)
  const int MeshesStopAdding = 3450; // no more zones around the camera from here
  const int MeshesTakeAway = 3650;   // two of the camera's extra zones go
  const int MeshesTakeAll = 3800;    // all of them go, the camera's own too
  // The meshes of a focus's zones register in bursts as they load. Near the
  // limit another focus is only added once the count has stood still this
  // long, so two foci's bursts can't land on top of each other. (Further
  // down the game's own streaming around Faith keeps the count moving, and a
  // burst can't reach the limit anyway.)
  const int MeshesSettleFrom = 2800;
  const ULONGLONG MeshesSettleMs = 1000;
  std::atomic<ULONGLONG> g_MeshesChangedAt{ 0 };
  std::atomic<int> g_MeshesNow{ 0 };

  bool MeshesSettled()
  {
    return g_MeshesNow < MeshesSettleFrom || GetTickCount64() - g_MeshesChangedAt >= MeshesSettleMs;
  }

  struct InstanceLists
  {
    int Count[2];
    int Capacity[2];
    int Reserved[2];
    __int64 Begin[2];
    __int64 pSettings;
    int Meshes;
    bool MeshesInline; // still in the manager's own room for 4096
  };

  // Mesh records in use, with the game's own test (first qword not 0); -1 if unreadable
  int CountMeshesInUse(__int64 ppManager)
  {
    __try
    {
      __int64 pManager = ppManager ? *(__int64*)ppManager : 0;
      if (!pManager) return -1;
      __int64 begin = *(__int64*)(pManager + MeshTableOffset);
      __int64 end = *(__int64*)(pManager + MeshTableOffset + 8);
      if (!begin || end < begin) return 0;
      int count = (int)((end - begin) / MeshRecordSize);
      if (count > MeshTableCapacity + 64) count = MeshTableCapacity + 64;
      int used = 0;
      for (int i = 0; i < count; ++i)
        used += *(__int64*)(begin + (__int64)i * MeshRecordSize) != 0 ? 1 : 0;
      return used;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      return -1;
    }
  }

  bool ReadInstanceLists(__int64 ppManager, InstanceLists& lists)
  {
    __try
    {
      __int64 pManager = ppManager ? *(__int64*)ppManager : 0;
      if (!pManager) return false;
      for (int k = 0; k < 2; ++k)
      {
        __int64* pVector = (__int64*)(pManager + InstanceListsOffset + k * 0x20);
        __int64 begin = pVector[0], end = pVector[1], capacity = pVector[2];
        // An empty list can have no memory at all
        if (end < begin || capacity < end) return false;
        lists.Begin[k] = begin;
        lists.Count[k] = begin ? (int)((end - begin) / InstanceRecordSize) : 0;
        lists.Capacity[k] = begin ? (int)((capacity - begin) / InstanceRecordSize) : 0;
      }

      __int64* pMeshes = (__int64*)(pManager + MeshTableOffset);
      __int64 meshBegin = pMeshes[0], meshEnd = pMeshes[1];
      if (meshEnd < meshBegin) return false;
      lists.Meshes = meshBegin ? (int)((meshEnd - meshBegin) / MeshRecordSize) : 0;
      lists.MeshesInline = meshBegin == pManager + MeshTableInline || !meshBegin;
      lists.pSettings = *(__int64*)(pManager + StreamingSettingsOffset);
      for (int k = 0; k < 2; ++k)
        lists.Reserved[k] = lists.pSettings ? *(int*)(lists.pSettings + ReservedCountOffset + k * 4) : 0;
      return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      return false;
    }
  }

  // Asks for room for this many when the next level loads
  void RaiseInstanceReserve(__int64 pSettings, int k, int count)
  {
    __try
    {
      *(int*)(pSettings + ReservedCountOffset + k * 4) = count;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
  }

  int CountUsedZoneSlots(__int64 pZoneStreamer)
  {
    __int64* pSlots = (__int64*)(pZoneStreamer + ZoneSlotsOffset);
    int used = 0;
    for (int i = 0; i < ZoneSlotCount; ++i)
      used += pSlots[i] ? 1 : 0;
    return used;
  }

  // The order foci are added in, most important first: the camera, ahead of
  // it, the track's points, then the rings from the inside out. They're taken
  // away in the opposite order.
  int FocusPriority(int rank)
  {
    if (rank < FocusRingFirst)
      return rank;
    int trackCount = MaxStreamingFoci - FocusTrackFirst;
    if (rank < FocusRingFirst + trackCount)
      return FocusTrackFirst + (rank - FocusRingFirst);
    return FocusRingFirst + (rank - FocusRingFirst - trackCount);
  }

  // Adds and removes the tools' foci in one streamer's list to match
  // wanted[], within the zone slot budget above. Gives how many were in the
  // list before and after, and how many slots were in use. The game's remove
  // function writes out of bounds for a focus that isn't there, so it's only
  // called for ones found in the list.
  void SyncStreamingFoci(__int64 pZoneStreamer, bool const wanted[], __int64 pAdd, __int64 pRemove,
    int instancePressure, int& before, int& after, int& usedSlots, bool& limited)
  {
    before = after = usedSlots = 0;
    limited = false;
    __try
    {
      __int64 pFocusList = pZoneStreamer + ZoneFocusListOffset;
      ZoneFocusRecord* pRecords = *(ZoneFocusRecord**)(pFocusList + ZoneFocusRecordsOffset);
      unsigned int count = *(unsigned int*)(pFocusList + ZoneFocusCountOffset);
      if (count > MaxZoneFoci || (count && !pRecords))
        return;

      bool present[MaxStreamingFoci]{};
      for (unsigned int i = 0; i < count; ++i)
      {
        StreamingFocus* pFocus = (StreamingFocus*)pRecords[i].pFocus;
        if (pFocus >= &g_Foci[0] && pFocus < &g_Foci[MaxStreamingFoci] && !(pRecords[i].Flags & ZoneFocusRemoved))
          present[pFocus - &g_Foci[0]] = true;
      }
      for (int i = 0; i < MaxStreamingFoci; ++i)
        before += present[i] ? 1 : 0;

      usedSlots = CountUsedZoneSlots(pZoneStreamer);

      // Foci that aren't wanted any more go straight away
      for (int i = 0; i < MaxStreamingFoci; ++i)
      {
        if (present[i] && !wanted[i])
        {
          ((tZoneFocusChange)pRemove)(pFocusList, 0, &g_Foci[i]);
          present[i] = false;
        }
      }

      if (usedSlots > ZoneSlotsRemoveAbove || instancePressure >= 2)
      {
        // Too full: the two least important go (the camera's own only when the
        // game is already out of room for meshes)
        int removed = 0;
        int lastRank = instancePressure >= 3 ? 0 : 1;
        for (int rank = MaxStreamingFoci - 1; rank >= lastRank && removed < 2; --rank)
        {
          int i = FocusPriority(rank);
          if (present[i])
          {
            ((tZoneFocusChange)pRemove)(pFocusList, 0, &g_Foci[i]);
            present[i] = false;
            removed++;
          }
        }
        limited = true;
      }
      else
      {
        // One at a time, so the slots it fills show up before the next is added
        for (int rank = 0; rank < MaxStreamingFoci; ++rank)
        {
          int i = FocusPriority(rank);
          if (!wanted[i] || present[i])
            continue;
          // The camera's own focus goes in unless the game is out of room,
          // the rest only with room, once the slots have caught up with the last one
          if (i == FocusCamera && instancePressure >= 2)
          {
            limited = true;
            break;
          }
          if (i != FocusCamera && (instancePressure >= 1 || !MeshesSettled() || !MayAddFocus(pZoneStreamer, usedSlots)))
          {
            limited = usedSlots >= ZoneSlotsAddBelow || instancePressure >= 1;
            break;
          }
          ((tZoneFocusChange)pAdd)(pFocusList, 0, &g_Foci[i]);
          present[i] = true;
          break;
        }
      }

      for (int i = 0; i < MaxStreamingFoci; ++i)
        after += present[i] ? 1 : 0;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      after = before;
    }
  }

  // A pick watches what's in view for a moment: static models only count if
  // they moved this far by then, so buildings are never picked. In seconds,
  // so a slow lift moves far enough however fast the game draws frames.
  const double AttachWatchSeconds = 0.25;
  const float AttachMovedDistance = 0.02f;
}

CameraManager::CameraManager() :
  m_CameraEnabled(false),
  m_FirstEnable(true),
  m_AutoReset(true),
  m_GamepadDisabled(true),
  m_KbmDisabled(true),
  m_ThirdPersonToggle(true),
  m_ThirdPersonApplied(false),
  m_LodFix(false),
  m_PlaybackFps(0),
  m_HasLastFrame(false),
  m_Camera(),
  m_TrackPlayer()
{
  m_ppGameRenderer = util::offsets::GetOffset("OFFSET_GAMERENDERER");
  m_ppGameRenderSettings = util::offsets::GetOffset("OFFSET_GAMERENDERSETTINGS");
  m_ppGameTimeSettings = util::offsets::GetOffset("OFFSET_GAMETIMESETTINGS");
  m_ppUISettings = util::offsets::GetOffset("OFFSET_UISETTINGS");
  m_ppClientGameContext = util::offsets::GetOffset("OFFSET_CLIENTGAMECONTEXT");
  m_p3PToggle = util::offsets::GetOffset("OFFSET_3PTOGGLE");
  m_ppMeshSettings = util::offsets::GetOffset("OFFSET_MESHSETTINGS");
  m_ppWorldRenderSettings = util::offsets::GetOffset("OFFSET_WORLDRENDERSETTINGS");
  for (int i = 0; i < MaxStreamingFoci; ++i)
    g_Foci[i] = { g_FocusVTable, i };

  m_ppMeshStreaming = util::offsets::GetOffset("OFFSET_MESHSTREAMING");
  m_pAddZoneFocus = util::offsets::GetOffset("OFFSET_ZONEFOCUSADD");
  m_pRemoveZoneFocus = util::offsets::GetOffset("OFFSET_ZONEFOCUSREMOVE");
}

CameraManager::~CameraManager()
{

}

void CameraManager::HotkeyUpdate()
{
  InputSystem* pInput = g_mainHandle->GetInputSystem();
  std::lock_guard<std::mutex> lock(m_Mutex);

  if (pInput->WentDown(Action::ToggleCamera))
    ToggleCamera();

  if (pInput->WentDown(Action::ToggleFreezeTime))
    ToggleTimeFreeze();

  if (pInput->WentDown(Action::ToggleHUD))
    ToggleHudVisibility();

  if (pInput->WentDown(Action::ToggleGameInput))
  {
    m_KbmDisabled = !m_KbmDisabled;
    g_mainHandle->OnConfigChanged();
    util::log::Write("Game input is %s while the camera is enabled", m_KbmDisabled ? "blocked" : "allowed");
  }

  if (!m_CameraEnabled) return;

  if (pInput->WentDown(Action::Track_CreateNode))
    m_TrackPlayer.CreateNode(m_Camera);

  if (pInput->WentDown(Action::Track_DeleteNode))
    m_TrackPlayer.DeleteNode();

  if (pInput->WentDown(Action::Track_Play))
    m_TrackPlayer.Toggle();

  if (pInput->WentDown(Action::Camera_Attach))
    m_AttachRequest = AttachRequest_Toggle;
}

void CameraManager::OnCameraUpdate(__int64 pRenderView)
{
  // The hook runs for every render view, only the main one is overridden
  __int64 pGameRenderer = ReadPointer(m_ppGameRenderer);
  if (!pGameRenderer || pRenderView != *(__int64*)(pGameRenderer + RenderViewOffset))
    return;

  // Draw distance works with or without the free camera
  float farPlane = m_FarPlane;
  if (farPlane > 0)
    *(float*)(pRenderView + FarPlaneOffset) = farPlane;

  // The mouse is read once per frame, used or not, so that moving it in
  // another window or over the menu doesn't make the camera jump later.
  // Over the menu its wheel scrolls the menu, if the game sends no wheel messages.
  XMFLOAT3 mouseState = g_mainHandle->GetInputSystem()->GetMouseState();
  // Only used when the wheel hook isn't there; the menu ignores it unless
  // the mouse is over a menu window
  g_mainHandle->GetUI()->AddWheelFromDirectInput(mouseState.z / WHEEL_DELTA);

  std::lock_guard<std::mutex> lock(m_Mutex);

  // The view's field of view and aspect, for the FOLLOW tab's markers
  float fovY = *(float*)(pRenderView + RenderViewFovOffset);
  float aspect = *(float*)(pRenderView + RenderViewAspectOffset);
  if (fovY > 0.05f && fovY < 3.f)
    m_ViewFovY = fovY;
  if (aspect > 0.3f && aspect < 5.f)
    m_ViewAspect = aspect;

  // The game can reset its settings (options menu, level loads), so these
  // are applied every frame. LOD also has to wait for MeshSettings to exist,
  // which is after the tools have read their config.
  if (m_LodFix || m_LodScale > 0 || m_LodSaved)
    ApplyLodSettings();
  ApplyCulling();
  ApplyShadowDistance();
  WatchInstanceLists();
  ApplyHairFix();

  // A track stopped setting the time of day: the clock carries on from there
  if (m_TrackTimeOfDayApplied && !(m_CameraEnabled && m_TrackPlayer.IsPlaying() && m_TrackPlayer.IsTimeOfDayLocked()))
  {
    g_mainHandle->GetVisualsManager()->StopTrackTimeOfDay();
    m_TrackTimeOfDayApplied = false;
  }
  // A stopped clock is held every frame, not only at the game's visual updates
  g_mainHandle->GetVisualsManager()->KeepClock();

  // Motion blur too. With the camera on it's applied after the track has
  // updated the strength below.
  if (!m_CameraEnabled)
  {
    ApplyMotionBlur();
    return;
  }

  auto now = std::chrono::steady_clock::now();
  double dt = m_HasLastFrame ? std::chrono::duration<double>(now - m_LastFrame).count() : 0.0;
  m_LastFrame = now;
  m_HasLastFrame = true;

  // Don't let a loading screen or alt-tab turn into one huge step
  if (dt > 0.1) dt = 0.1;

  // With a fixed playback rate every rendered frame advances the track
  // by the same amount, which keeps frame-dumped recordings smooth.
  double trackDt = m_PlaybackFps > 0 ? 1.0 / m_PlaybackFps : dt;

  // Recorded into new keyframes
  m_Camera.TimeOfDay = g_mainHandle->GetVisualsManager()->GetCurrentTimeOfDay();

  // The body switch unlinks her own camera a moment after it starts
  if (m_ThirdPersonUnlinkAt && GetTickCount64() >= m_ThirdPersonUnlinkAt)
  {
    m_ThirdPersonUnlinkAt = 0;
    if (m_ThirdPersonApplied)
      UnlinkFaithCamera();
  }

  FollowAttachTarget(dt, trackDt);
  UpdateInput(dt, mouseState);
  UpdateCamera(dt, trackDt);
  WriteToGameCamera(pRenderView);
  PublishStreamingFocus(dt);
  UpdateNearby();
  ApplyFieldOfView(m_Camera.FieldOfView);
  ApplyMotionBlur();
}

void CameraManager::PublishStreamingFocus(double dt)
{
  // Read by GetStreamingFocus when the zone streamer updates. Same rows as the
  // game's camera focus: translation and forward.
  ZoneFocusResult results[MaxStreamingFoci]{};
  bool wanted[MaxStreamingFoci]{};
  bool on = m_StreamAroundCamera && m_CameraEnabled;

  XMVECTOR position = XMLoadFloat3((XMFLOAT3*)&m_Camera.Transform.m[3][0]);
  XMVECTOR forward = XMLoadFloat3((XMFLOAT3*)&m_Camera.Transform.m[2][0]);
  for (int i = 0; i < MaxStreamingFoci; ++i)
    XMStoreFloat3((XMFLOAT3*)results[i].Direction, forward);

  XMStoreFloat3((XMFLOAT3*)results[FocusCamera].Position, position);
  wanted[FocusCamera] = on;

  // Ahead: where the camera will be in a couple of seconds at its current
  // speed, so the zones are loading before it gets there. Jumps (Go to,
  // playback starting) reset the speed instead of throwing it far away.
  XMVECTOR last = XMLoadFloat3(&m_StreamLastPosition);
  float moved = XMVectorGetX(XMVector3Length(position - last));
  if (!m_StreamHasLast || dt <= 0 || moved > 50.f)
    m_StreamVelocity = XMFLOAT3(0, 0, 0);
  else
  {
    XMVECTOR velocity = (position - last) / (float)dt;
    float blend = (float)(dt / (dt + 0.25)); // about a quarter of a second of smoothing
    XMStoreFloat3(&m_StreamVelocity, XMVectorLerp(XMLoadFloat3(&m_StreamVelocity), velocity, blend));
  }
  XMStoreFloat3(&m_StreamLastPosition, position);
  m_StreamHasLast = true;

  XMVECTOR ahead = XMLoadFloat3(&m_StreamVelocity) * LookAheadSeconds;
  float aheadDistance = XMVectorGetX(XMVector3Length(ahead));
  if (aheadDistance > LookAheadMaxDistance)
    ahead = ahead * (LookAheadMaxDistance / aheadDistance);
  // Always in the list while it's on (it sits on the camera when it's still),
  // so zones aren't loaded and released over and over around a speed limit
  XMStoreFloat3((XMFLOAT3*)results[FocusAhead].Position, position + ahead);
  wanted[FocusAhead] = on && m_StreamAhead;

  // Around the camera, out to the stream distance. A focus loads its own zone
  // and the zones next to it, so rings of points about 200 m apart load
  // everything around the camera that far out.
  if (on && m_StreamDistance > 1.f)
  {
    float maxDistance = m_StreamInView ? MaxStreamDistanceInView : MaxStreamDistance;
    float distance = m_StreamDistance > maxDistance ? maxDistance : m_StreamDistance;
    int rings = (int)ceilf(distance / RingSpacing);
    // Only where the camera looks: points within the view's width (and a
    // margin) of the way it faces. The camera looks along -row 2.
    float lookX = -m_Camera.Transform.m[2][0], lookZ = -m_Camera.Transform.m[2][2];
    float lookLength = sqrtf(lookX * lookX + lookZ * lookZ);
    bool inViewOnly = m_StreamInView && lookLength > 0.1f; // straight up or down: all round
    float minDot = -2.f;
    if (inViewOnly)
    {
      lookX /= lookLength;
      lookZ /= lookLength;
      float halfWidth = atanf(tanf(m_ViewFovY * 0.5f) * m_ViewAspect) + ViewConeMargin;
      minDot = cosf(halfWidth < 1.5707963f ? halfWidth : 1.5707963f);
    }
    // Centred on a point that moves in 100 m steps: rings that followed the
    // camera exactly would load and unload zones at their edges all the time
    XMFLOAT3 cameraPosition;
    XMStoreFloat3(&cameraPosition, position);
    XMVECTOR centre = XMVectorSet(floorf(cameraPosition.x / 100.f + 0.5f) * 100.f, cameraPosition.y,
      floorf(cameraPosition.z / 100.f + 0.5f) * 100.f, 0);
    int slot = FocusRingFirst;
    for (int ring = 1; ring <= rings; ++ring)
    {
      float radius = distance * ring / rings;
      int count = (int)ceilf(XM_2PI * radius / RingSpacing);
      if (count < 6) count = 6;
      XMVECTOR points[64];
      int kept = 0;
      for (int k = 0; k < count && kept < 64; ++k)
      {
        float angle = XM_2PI * k / count + ring * 0.5f; // rings turned against each other
        XMVECTOR point = centre + XMVectorSet(cosf(angle) * radius, 0, sinf(angle) * radius, 0);
        if (inViewOnly)
        {
          XMFLOAT3 p;
          XMStoreFloat3(&p, point);
          float dx = p.x - cameraPosition.x, dz = p.z - cameraPosition.z;
          float length = sqrtf(dx * dx + dz * dz);
          if (length > 1.f && (dx * lookX + dz * lookZ) / length < minDot)
            continue;
        }
        points[kept++] = point;
      }
      if (slot + kept > FocusRingFirst + MaxRingFoci)
        break; // a whole ring (or its part in view) or none, never half of one
      for (int k = 0; k < kept; ++k)
      {
        XMStoreFloat3((XMFLOAT3*)results[slot].Position, points[k]);
        wanted[slot++] = true;
      }
    }
  }

  // Along the selected track, so playing it doesn't load anything new
  if (on && m_StreamTrack)
  {
    XMFLOAT3 points[MaxStreamingFoci - FocusTrackFirst];
    int count = m_TrackPlayer.GetStreamingPoints(points, MaxStreamingFoci - FocusTrackFirst, 150.f);
    for (int i = 0; i < count; ++i)
    {
      memcpy(results[FocusTrackFirst + i].Position, &points[i], sizeof(float) * 3);
      wanted[FocusTrackFirst + i] = true;
    }
  }

  AcquireSRWLockExclusive(&g_FocusLock);
  memcpy(g_FocusResults, results, sizeof(results));
  memcpy(g_FocusWanted, wanted, sizeof(wanted));
  ReleaseSRWLockExclusive(&g_FocusLock);

  m_StreamingFocusReady = true;
}

void CameraManager::OnZoneStreamerUpdate(__int64 pZoneStreamer)
{
  if (!m_pAddZoneFocus || !m_pRemoveZoneFocus)
    return;

  // No camera lock: this runs on the game's update. The wanted foci come
  // from the camera hook through the focus lock.
  bool wanted[MaxStreamingFoci]{};
  if (m_StreamingFocusReady && m_StreamAroundCamera && m_CameraEnabled)
  {
    AcquireSRWLockShared(&g_FocusLock);
    memcpy(wanted, g_FocusWanted, sizeof(wanted));
    ReleaseSRWLockShared(&g_FocusLock);
  }

  int before = 0, after = 0, usedSlots = 0;
  bool limited = false;
  SyncStreamingFoci(pZoneStreamer, wanted, m_pAddZoneFocus, m_pRemoveZoneFocus, m_InstancePressure,
    before, after, usedSlots, limited);
  if (before == 0 && after > 0)
    util::log::Write("Zone streamer 0x%I64X now also loads the city around the camera", pZoneStreamer);
  else if (before > 0 && after == 0)
    util::log::Write("Zone streamer 0x%I64X is back to loading around Faith only", pZoneStreamer);

  // For the menu: the fullest streamer over the last second, and when foci were held back
  ULONGLONG now = GetTickCount64();
  static ULONGLONG s_peakTick = 0;
  if (now - s_peakTick > 1000)
  {
    s_peakTick = now;
    m_StreamSlotsPeak = usedSlots;
  }
  else if (usedSlots > m_StreamSlotsPeak)
    m_StreamSlotsPeak = usedSlots;
  if (limited) m_StreamLimitedTick = now;
  static ULONGLONG s_lastLimitLog = 0;
  if (limited && GetTickCount64() - s_lastLimitLog > 30000)
  {
    s_lastLimitLog = GetTickCount64();
    util::log::Write("Zone streamer 0x%I64X has %d of its 128 zone slots in use, the camera's extra zones are held back so it can't overflow",
      pZoneStreamer, usedSlots);
  }
}


namespace
{
  // "PamClientCharacterEntity 0x..." or "SwarmEntity 0x... member 12", for the log
  void DescribeTarget(attach::Target const& target, char* buffer, size_t size)
  {
    if (target.Agent >= 0)
      sprintf_s(buffer, size, "%s 0x%I64X member %d", attach::ClassName(target.Class), target.Entity, target.Agent);
    else
      sprintf_s(buffer, size, "%s 0x%I64X", attach::ClassName(target.Class), target.Entity);
  }
}

bool CameraManager::GetAttachTransform(XMMATRIX& transform)
{
  if (m_AttachObject)
  {
    XMFLOAT4X4 object;
    if (!attach::ReadTransform(m_AttachTarget, object))
      return false;
    m_AttachDistance = XMVectorGetX(XMVector3Length(XMVectorSet(object._41, object._42, object._43, 0) -
      XMLoadFloat3(&m_Camera.Position)));

    // All of its rotation (a drone banking, a car on a slope), without any scale
    if (m_AttachRotation && m_AttachTilt)
    {
      XMVECTOR scale, rotation, translation;
      if (XMMatrixDecompose(&scale, &rotation, &translation, XMLoadFloat4x4(&object)))
      {
        transform = XMMatrixRotationQuaternion(XMQuaternionNormalize(rotation)) * XMMatrixTranslationFromVector(translation);
        return true;
      }
    }

    // Or just its heading, the same as for Faith
    XMMATRIX heading = XMMatrixIdentity();
    if (m_AttachRotation)
    {
      float forwardX = object._31, forwardZ = object._33;
      if (forwardX * forwardX + forwardZ * forwardZ > 1e-6f)
        heading = XMMatrixRotationY(atan2f(forwardX, forwardZ));
    }
    transform = heading * XMMatrixTranslation(object._41, object._42, object._43);
    return true;
  }

  XMFLOAT4X4A player;
  if (!GetPlayerTransform(player) || (player._41 == 0 && player._42 == 0 && player._43 == 0))
    return false;

  // Only her position, and her heading if wanted: the camera stays level
  // when her body leans in a wall run or a climb
  XMMATRIX heading = XMMatrixIdentity();
  if (m_AttachRotation)
  {
    float forwardX = player._31, forwardZ = player._33;
    if (forwardX * forwardX + forwardZ * forwardZ > 1e-6f)
      heading = XMMatrixRotationY(atan2f(forwardX, forwardZ));
  }
  transform = heading * XMMatrixTranslation(player._41, player._42, player._43);
  return true;
}

void CameraManager::FollowAttachTarget(double dt, double smoothDt)
{
  // Picks asked for by the menu or the hotkey, done here on the game's thread
  if (m_AttachRequest != AttachRequest_None)
  {
    AttachRequest request = m_AttachRequest;
    m_AttachRequest = AttachRequest_None;
    StartAttachPick(request);
  }
  if (m_AttachPicking)
  {
    m_AttachPickRemaining -= dt;
    if (m_AttachPickRemaining <= 0)
    {
      m_AttachPicking = false;
      FinishAttachPick();
    }
  }

  // A row clicked in the FOLLOW tab's list. It wins over a pick that's still
  // watching what's in view.
  if (m_AttachListRequested)
  {
    m_AttachListRequested = false;
    m_AttachPicking = false;
    XMFLOAT4X4 transform;
    if (attach::ReadTransform(m_AttachListRequest, transform))
    {
      AttachToObject(m_AttachListRequest);
      if (m_AttachMoveCamera)
        MoveCameraNextTo(transform, m_AttachTarget);
    }
    else
      sprintf_s(m_AttachMessage, "That %s is gone.", attach::KindOf(m_AttachListRequest));
  }
  if (m_AttachPlaceRequested)
  {
    m_AttachPlaceRequested = false;
    XMFLOAT4X4 transform;
    if (m_AttachObject && attach::ReadTransform(m_AttachTarget, transform))
      MoveCameraNextTo(transform, m_AttachTarget);
  }

  XMMATRIX now;
  m_AttachValid = (m_AttachToFaith || m_AttachObject) && GetAttachTransform(now);
  if (!m_AttachValid)
  {
    // An object that's gone doesn't come back: its memory can be reused
    if (m_AttachObject)
      DetachObject("is gone (destroyed, unloaded or despawned)");
    m_AttachHasLast = false;
    m_AttachTrackActive = false;
    return;
  }

  // Most likely destroyed and something else built in its memory, or a
  // swarm record given to a new member somewhere else
  if (m_AttachObject && m_AttachHasLast &&
    XMVectorGetX(XMVector3Length(now.r[3] - XMLoadFloat3(&m_AttachRawPosition))) > attach::JumpLimit(m_AttachTarget))
  {
    char reason[64];
    sprintf_s(reason, "jumped more than %.0f m in one frame", attach::JumpLimit(m_AttachTarget));
    DetachObject(reason);
    m_AttachValid = false;
    m_AttachHasLast = false;
    m_AttachTrackActive = false;
    return;
  }
  XMStoreFloat3(&m_AttachRawPosition, now.r[3]);

  // Smoothing, for objects: the camera follows a copy of it that catches up
  // over a moment, so small steps and wobbles don't shake the view. With a
  // fixed playback rate dt is the same every frame, for recordings.
  if (m_AttachObject && m_AttachSmoothing > 0.f)
  {
    XMVECTOR scale, rotation, translation;
    if (XMMatrixDecompose(&scale, &rotation, &translation, now))
    {
      rotation = XMQuaternionNormalize(rotation);
      if (m_AttachHasLast)
      {
        double timeConstant = m_AttachSmoothing * 0.5; // seconds
        float blend = smoothDt > 0 ? (float)(1.0 - exp(-smoothDt / timeConstant)) : 1.f;
        translation = XMVectorLerp(XMLoadFloat3(&m_AttachSmoothPosition), translation, blend);
        rotation = XMQuaternionNormalize(XMQuaternionSlerp(XMLoadFloat4(&m_AttachSmoothRotation), rotation, blend));
      }
      XMStoreFloat3(&m_AttachSmoothPosition, translation);
      XMStoreFloat4(&m_AttachSmoothRotation, rotation);
      now = XMMatrixRotationQuaternion(rotation) * XMMatrixTranslationFromVector(translation);
    }
  }
  XMStoreFloat4x4(&m_AttachNow, now);

  // The free camera is carried along by how she moved since the last frame,
  // so it can still be flown around her
  if (m_AttachHasLast)
  {
    XMMATRIX moved = XMMatrixInverse(nullptr, XMLoadFloat4x4(&m_AttachLast)) * now;
    XMMATRIX camera = XMMatrixRotationQuaternion(XMLoadFloat4(&m_Camera.Rotation)) *
      XMMatrixTranslationFromVector(XMLoadFloat3(&m_Camera.Position));
    XMVECTOR scale, rotation, translation;
    if (XMMatrixDecompose(&scale, &rotation, &translation, camera * moved))
    {
      XMStoreFloat4(&m_Camera.Rotation, XMQuaternionNormalize(rotation));
      XMStoreFloat3(&m_Camera.Position, translation);
    }
  }
  m_AttachLast = m_AttachNow;
  m_AttachHasLast = true;

  // A track follows her from where she is when it starts
  if (m_TrackPlayer.IsPlaying())
  {
    if (!m_AttachTrackActive)
    {
      m_AttachTrackStart = m_AttachNow;
      m_AttachTrackActive = true;
    }
  }
  else
    m_AttachTrackActive = false;
}

attach::View CameraManager::MakeAttachView(__int64 pSoldier)
{
  attach::View view{};
  XMMATRIX rotation = XMMatrixRotationQuaternion(XMLoadFloat4(&m_Camera.Rotation));
  view.Camera = m_Camera.Position;
  XMStoreFloat3(&view.Look, XMVector3Normalize(-rotation.r[2])); // W moves the camera along -row 2, where it looks
  view.Skip = pSoldier;
  XMFLOAT4X4A player;
  if (GetPlayerTransform(player))
  {
    view.HasSkipPosition = true;
    view.SkipPosition = XMFLOAT3(player._41, player._42, player._43);
  }
  return view;
}

void CameraManager::StartAttachPick(AttachRequest request)
{
  __int64 pSoldier = GetPlayerSoldier();
  bool firstTime = attach::ClassCount() == 0;
  bool fromFaith = false;
  if (!attach::FindClasses(pSoldier, fromFaith))
  {
    sprintf_s(m_AttachMessage, "The game's entity classes weren't found, see the log.");
    util::log::Warning("Attach: ClientPhysicsEntity's class info wasn't found (%s), nothing can be picked",
      pSoldier ? "not among Faith's classes or at its address on this build" : "no Faith, and not at its address on this build");
    return;
  }
  if (firstTime)
  {
    std::string names;
    for (int i = 0; i < attach::ClassCount(); ++i)
      names += std::string(i ? ", " : "") + attach::ClassName(i) + " (" + attach::Kind(i) + ")";
    util::log::Write("Attach: %d classes, found from %s: %s", attach::ClassCount(),
      fromFaith ? "Faith's class" : "its address on this build", names.c_str());
  }

  if (!m_AttachScan)
    m_AttachScan = std::make_unique<attach::Scan>();
  attach::Scan& scan = *m_AttachScan;
  scan = attach::Scan{};
  attach::ScanView(MakeAttachView(pSoldier), scan);

  // What's in the level, for the log: how many of each class
  std::string found;
  int total = 0;
  for (int i = 0; i < attach::ClassCount(); ++i)
  {
    if (!scan.Found[i]) continue;
    found += std::string(found.empty() ? "" : ", ") + attach::ClassName(i) + " " + std::to_string(scan.Found[i]);
    if (attach::IsSwarm(i))
      found += " members in " + std::to_string(scan.Swarms) + " swarms";
    total += scan.Found[i];
  }
  if (found.empty()) found = "none";
  if (scan.NoTransform || scan.Errors)
    found += ", " + std::to_string(scan.NoTransform) + " with another transform function, " +
      std::to_string(scan.Errors) + " unreadable";
  util::log::Write("Attach: camera at %.1f %.1f %.1f, %d in view, %d static models in view to watch. Objects: %s",
    m_Camera.Position.x, m_Camera.Position.y, m_Camera.Position.z, scan.BestCount, scan.StaticCount, found.c_str());

  if (scan.BestCount == 0 && scan.StaticCount == 0)
  {
    sprintf_s(m_AttachMessage, "Nothing that can move is in the middle of the view (%d objects checked).", total);
    return;
  }

  // Static models only count if they move (and swarm members that move go
  // first), so everything is watched for a moment
  m_AttachPickRequest = request;
  m_AttachPicking = true;
  m_AttachPickRemaining = AttachWatchSeconds;
  m_AttachMessage[0] = 0;
}

void CameraManager::FinishAttachPick()
{
  if (!m_AttachScan)
    return;
  attach::Scan& scan = *m_AttachScan;

  attach::Target picked[attach::MaxCandidates * 2];
  int count = 0, stayedStill = 0;
  XMFLOAT4X4 transform;
  for (int i = 0; i < scan.BestCount; ++i)
  {
    attach::Target& candidate = scan.Best[i];
    if (!attach::ReadTransform(candidate, transform))
      continue;
    // A swarm can have records left over from members that are gone: ones
    // that moved go first
    float moved = XMVectorGetX(XMVector3Length(XMVectorSet(transform._41, transform._42, transform._43, 0) -
      XMLoadFloat3(&candidate.Position)));
    if (attach::IsSwarm(candidate.Class) && moved <= AttachMovedDistance)
      candidate.Score += 0.1f;
    picked[count++] = candidate;
  }
  for (int i = 0; i < scan.StaticCount; ++i)
  {
    attach::Target& candidate = scan.Static[i];
    if (!attach::ReadTransform(candidate, transform))
      continue;
    float moved = XMVectorGetX(XMVector3Length(XMVectorSet(transform._41, transform._42, transform._43, 0) -
      XMLoadFloat3(&candidate.Position)));
    if (moved > AttachMovedDistance)
      picked[count++] = candidate;
    else
      ++stayedStill;
  }
  std::sort(picked, picked + count, [](attach::Target const& a, attach::Target const& b) { return a.Score < b.Score; });

  if (count == 0)
  {
    sprintf_s(m_AttachMessage, "Nothing that moves is in the middle of the view%s. The Nearby list below shows "
      "everything around the camera.", stayedStill ? " (only static models, and they stayed still)" : "");
    util::log::Write("Attach: nothing picked, %d static models in view stayed still", stayedStill);
    return;
  }

  // The hotkey on what's already followed lets go of it
  if (m_AttachPickRequest == AttachRequest_Toggle && m_AttachObject && attach::SameTarget(picked[0], m_AttachTarget))
  {
    DetachObject(nullptr);
    return;
  }
  AttachToObject(picked[0]);
  if (m_AttachMoveCamera && attach::ReadTransform(m_AttachTarget, transform))
    MoveCameraNextTo(transform, m_AttachTarget);
}

void CameraManager::AttachToObject(attach::Target const& target)
{
  if (m_AttachToFaith)
    util::log::Write("Camera no longer follows Faith");

  m_AttachTarget = target;
  m_AttachObject = true;
  m_AttachDistance = target.Distance;
  m_AttachToFaith = false;
  m_AttachHasLast = false;
  m_AttachTrackActive = false;
  m_AttachMessage[0] = 0;

  char description[128];
  DescribeTarget(target, description, sizeof(description));
  util::log::Write("Camera follows %s (%s) at %.1f %.1f %.1f, %.1f m away, %.1f degrees off centre",
    description, attach::KindOf(target), target.Position.x, target.Position.y, target.Position.z, target.Distance,
    XMConvertToDegrees(target.Angle));
}

void CameraManager::DetachObject(const char* reason)
{
  if (!m_AttachObject)
    return;

  char description[128];
  DescribeTarget(m_AttachTarget, description, sizeof(description));
  if (reason)
  {
    sprintf_s(m_AttachMessage, "The %s the camera followed (%s) %s.", attach::KindOf(m_AttachTarget),
      attach::ClassName(m_AttachTarget.Class), reason);
    util::log::Write("Camera no longer follows %s: it %s", description, reason);
  }
  else
  {
    m_AttachMessage[0] = 0;
    util::log::Write("Camera no longer follows %s", description);
  }

  m_AttachObject = false;
  m_AttachHasLast = false;
  m_AttachTrackActive = false;
}

void CameraManager::MoveCameraNextTo(XMFLOAT4X4 const& object, attach::Target const& target)
{
  // Behind it and a little above, looking at it. Turning with it from far
  // away swings the camera around a lot, from here it's a chase camera.
  bool person = attach::GroupOf(target) == attach::Group::People;
  float back = person ? 3.5f : 8.f;
  float above = person ? 0.6f : 2.5f;
  XMVECTOR aim = XMVectorSet(object._41, object._42 + attach::AimHeight(target) + 0.3f, object._43, 0);

  // Its forward row, flat; or the way the camera looks if it has none
  XMVECTOR forward = XMVectorSet(object._31, 0, object._33, 0);
  if (XMVectorGetX(XMVector3LengthSq(forward)) < 0.01f)
  {
    XMMATRIX rotation = XMMatrixRotationQuaternion(XMLoadFloat4(&m_Camera.Rotation));
    forward = XMVectorSet(-XMVectorGetX(rotation.r[2]), 0, -XMVectorGetZ(rotation.r[2]), 0);
    if (XMVectorGetX(XMVector3LengthSq(forward)) < 0.01f)
      forward = XMVectorSet(0, 0, 1, 0);
  }
  forward = XMVector3Normalize(forward);

  XMVECTOR position = aim - forward * back + XMVectorSet(0, above, 0, 0);
  // The camera's row 2 points away from where it looks
  XMVECTOR backward = XMVector3Normalize(position - aim);
  XMVECTOR right = XMVector3Normalize(XMVector3Cross(XMVectorSet(0, 1, 0, 0), backward));
  XMVECTOR up = XMVector3Cross(backward, right);
  XMMATRIX rotation;
  rotation.r[0] = right;
  rotation.r[1] = up;
  rotation.r[2] = backward;
  rotation.r[3] = XMVectorSet(0, 0, 0, 1);

  XMStoreFloat3(&m_Camera.Position, position);
  XMStoreFloat4(&m_Camera.Rotation, XMQuaternionNormalize(XMQuaternionRotationMatrix(rotation)));
  m_AttachHasLast = false;
}

void CameraManager::UpdateNearby()
{
  // Only while the FOLLOW tab is being drawn
  unsigned long long now = GetTickCount64();
  if (now > m_NearbyWantedUntil)
  {
    if (!m_Nearby.empty() || !m_NearbySeen.empty() || !m_NearbyPending.empty())
    {
      m_Nearby.clear();
      m_NearbyPending.clear();
      m_NearbySeen.clear();
    }
    m_NearbyHasPending = false;
    m_NearbyLastPublish = 0;
    return;
  }

  // Looked at once a second, to tell what moves (static models only show up
  // once they've moved). The rows only change every NearbyListSeconds, on
  // Refresh, after the range changes or while there are none, and never while
  // the mouse is over the list, so they hold still long enough to click.
  if (m_NearbyRescan || now - m_NearbyLastScan >= 1000)
  {
    m_NearbyRescan = false;
    m_NearbyLastScan = now;
    ScanNearby();
  }
  bool due = m_Nearby.empty() || m_NearbyPublishRequested || now - m_NearbyLastPublish >= NearbyListSeconds * 1000ull;
  if (due && m_NearbyHasPending && !m_NearbyListHovered)
  {
    // Swarm members keep their smoothing from the rows they had
    auto keyOf = [](attach::Target const& target) {
      return (unsigned long long)target.Entity | ((unsigned long long)(target.Agent + 1) << 48);
    };
    std::unordered_map<unsigned long long, size_t> previous;
    for (size_t i = 0; i < m_Nearby.size(); ++i)
      if (m_Nearby[i].Target.Agent >= 0)
        previous[keyOf(m_Nearby[i].Target)] = i;
    for (NearbyItem& item : m_NearbyPending)
    {
      auto kept = previous.find(keyOf(item.Target));
      if (kept == previous.end())
        continue;
      attach::Target const& before = m_Nearby[kept->second].Target;
      item.Target.Heading = before.Heading;
      item.Target.Seen = before.Seen;
      item.Target.SeenTime = before.SeenTime;
      item.Target.Interval = before.Interval;
    }

    m_Nearby.swap(m_NearbyPending);
    m_NearbyPending.clear();
    m_NearbyHasPending = false;
    m_NearbyTotal = m_NearbyPendingTotal;
    m_NearbyDropped = m_NearbyPendingDropped;
    m_NearbyListedRadius = m_NearbyRadius;
    m_NearbyLastPublish = now;
    m_NearbyPublishRequested = false;
  }

  // Every frame, so the markers stay on what moves
  for (NearbyItem& item : m_Nearby)
  {
    XMFLOAT4X4 transform;
    if (!item.Gone && attach::ReadTransform(item.Target, transform))
    {
      if (fabsf(transform._41 - item.Position.x) + fabsf(transform._42 - item.Position.y) +
        fabsf(transform._43 - item.Position.z) > 0.001f)
        item.LastMoved = now;
      item.Position = XMFLOAT3(transform._41, transform._42, transform._43);
    }
    else
      item.Gone = true;
  }
}

void CameraManager::ScanNearby()
{
  const int MaxFound = 32768; // static models count too, to tell which move
  const int MaxShown = 300;

  __int64 pSoldier = GetPlayerSoldier();
  bool fromFaith = false;
  if (!attach::FindClasses(pSoldier, fromFaith))
  {
    m_NearbyPending.clear();
    m_NearbyPendingTotal = 0;
    m_NearbyHasPending = true;
    return;
  }

  if (!m_NearbyStats)
    m_NearbyStats = std::make_unique<attach::Scan>();
  *m_NearbyStats = attach::Scan{};
  m_NearbyFound.resize(MaxFound);
  int count = attach::ScanNearby(MakeAttachView(pSoldier), m_NearbyRadius, m_NearbyFound.data(), MaxFound, *m_NearbyStats);
  std::sort(m_NearbyFound.begin(), m_NearbyFound.begin() + count,
    [](attach::Target const& a, attach::Target const& b) {
      // Ties in a fixed order, so the same one of a character's two entities is kept every time
      if (a.Distance != b.Distance) return a.Distance < b.Distance;
      if (a.Entity != b.Entity) return a.Entity < b.Entity;
      return a.Agent < b.Agent;
    });

  auto keyOf = [](attach::Target const& target) {
    return (unsigned long long)target.Entity | ((unsigned long long)(target.Agent + 1) << 48);
  };
  unsigned long long now = GetTickCount64();
  std::vector<NearbyItem> items;
  std::unordered_map<unsigned long long, NearbySeen> seen;
  seen.reserve(count);
  int total = 0;
  for (int i = 0; i < count; ++i)
  {
    attach::Target& target = m_NearbyFound[i];

    // A character is two entities at the same spot: only the first counts.
    // They're the same distance away, so next to each other once sorted.
    bool duplicate = false;
    for (int back = i - 1; target.Agent < 0 && back >= 0 && back >= i - 3; --back)
    {
      attach::Target const& other = m_NearbyFound[back];
      duplicate |= other.Agent < 0 && other.Class == target.Class &&
        fabsf(other.Position.x - target.Position.x) + fabsf(other.Position.y - target.Position.y) +
        fabsf(other.Position.z - target.Position.z) < 0.02f;
    }
    if (duplicate)
      continue;

    // Moving: since the last look, a second ago
    unsigned long long key = keyOf(target);
    NearbySeen entry = { target.Position, 0 };
    auto old = m_NearbySeen.find(key);
    if (old != m_NearbySeen.end())
    {
      entry.LastMoved = old->second.LastMoved;
      if (fabsf(old->second.Position.x - target.Position.x) + fabsf(old->second.Position.y - target.Position.y) +
        fabsf(old->second.Position.z - target.Position.z) > AttachMovedDistance)
        entry.LastMoved = now;
    }
    seen[key] = entry;

    // Static models only once they've moved (in the last 30 seconds), or the
    // list would be all buildings
    if (attach::IsStatic(target.Class) && !(entry.LastMoved && now - entry.LastMoved < 30000))
      continue;

    ++total;
    if ((int)items.size() < MaxShown)
      items.push_back({ target, target.Position, entry.LastMoved, false });
  }
  m_NearbyPending.swap(items);
  m_NearbyPendingTotal = total;
  m_NearbyPendingDropped = m_NearbyStats->Dropped;
  m_NearbyHasPending = true;
  m_NearbySeen.swap(seen);
}

bool CameraManager::ProjectToScreen(XMFLOAT3 const& world, float& x, float& y)
{
  // The camera's rows are right, up, and the opposite of where it looks
  XMMATRIX camera = XMLoadFloat4x4(&m_Camera.Transform);
  XMVECTOR offset = XMLoadFloat3(&world) - camera.r[3];
  float right = XMVectorGetX(XMVector3Dot(offset, camera.r[0]));
  float up = XMVectorGetX(XMVector3Dot(offset, camera.r[1]));
  float depth = -XMVectorGetX(XMVector3Dot(offset, camera.r[2]));
  if (depth < 0.1f)
    return false;

  float scale = tanf(m_ViewFovY * 0.5f);
  float nx = right / (depth * scale * m_ViewAspect);
  float ny = up / (depth * scale);
  ImVec2 size = ImGui::GetIO().DisplaySize;
  x = (nx * 0.5f + 0.5f) * size.x;
  y = (0.5f - ny * 0.5f) * size.y;
  return fabsf(nx) <= 1.f && fabsf(ny) <= 1.f;
}

bool CameraManager::NearbyItemShown(NearbyItem const& item)
{
  // Rows of things that are gone stay (greyed out) until the next list
  if (m_NearbyMovingOnly && (item.Gone || !IsNearbyMoving(item, GetTickCount64())))
    return false;
  switch (attach::GroupOf(item.Target))
  {
  case attach::Group::People: return m_NearbyPeople;
  case attach::Group::Traffic: return m_NearbyTraffic;
  default: return m_NearbyOther;
  }
}

void CameraManager::DrawCrosshair()
{
  // The middle of the screen, which is what "Attach" picks around
  ImDrawList* pDrawList = ImGui::GetOverlayDrawList();
  ImVec2 size = ImGui::GetIO().DisplaySize;
  ImVec2 centre(size.x * 0.5f, size.y * 0.5f);
  float arm = ImGui::GetFontSize() * 0.8f, gap = arm * 0.35f;
  for (int pass = 0; pass < 2; ++pass)
  {
    // A dark outline first, so it shows on bright and dark scenes
    ImU32 colour = pass ? IM_COL32(255, 255, 255, 230) : IM_COL32(0, 0, 0, 160);
    float thickness = pass ? 1.5f : 3.5f;
    pDrawList->AddLine(ImVec2(centre.x - arm, centre.y), ImVec2(centre.x - gap, centre.y), colour, thickness);
    pDrawList->AddLine(ImVec2(centre.x + gap, centre.y), ImVec2(centre.x + arm, centre.y), colour, thickness);
    pDrawList->AddLine(ImVec2(centre.x, centre.y - arm), ImVec2(centre.x, centre.y - gap), colour, thickness);
    pDrawList->AddLine(ImVec2(centre.x, centre.y + gap), ImVec2(centre.x, centre.y + arm), colour, thickness);
  }
}

void CameraManager::DrawMarkers(int hovered)
{
  if (!m_CameraEnabled)
    return;

  ImDrawList* pDrawList = ImGui::GetOverlayDrawList();
  float fontSize = ImGui::GetFontSize();
  const ImU32 red = IM_COL32(255, 107, 107, 255);
  const ImU32 shadow = IM_COL32(0, 0, 0, 170);
  char label[96];

  auto labelAt = [&](float x, float y, ImU32 colour, const char* text) {
    ImVec2 at(x + fontSize * 0.7f, y - fontSize * 0.5f);
    pDrawList->AddText(ImVec2(at.x + 1, at.y + 1), shadow, text);
    pDrawList->AddText(at, colour, text);
  };

  // A dot on everything in the list, a ring and its name on the one the
  // mouse is over
  unsigned long long now = GetTickCount64();
  for (int i = 0; m_NearbyMarkers && i < (int)m_Nearby.size(); ++i)
  {
    NearbyItem const& item = m_Nearby[i];
    if (item.Gone || !NearbyItemShown(item) || i == hovered)
      continue;
    XMFLOAT3 aim(item.Position.x, item.Position.y + attach::AimHeight(item.Target), item.Position.z);
    float x, y;
    if (ProjectToScreen(aim, x, y))
    {
      pDrawList->AddCircleFilled(ImVec2(x, y), fontSize * 0.22f, shadow, 8);
      pDrawList->AddCircleFilled(ImVec2(x, y), fontSize * 0.15f,
        IsNearbyMoving(item, now) ? IM_COL32(255, 255, 255, 220) : IM_COL32(255, 255, 255, 110), 8);
    }
  }
  if (hovered >= 0 && hovered < (int)m_Nearby.size() && !m_Nearby[hovered].Gone)
  {
    NearbyItem const& item = m_Nearby[hovered];
    XMFLOAT3 aim(item.Position.x, item.Position.y + attach::AimHeight(item.Target), item.Position.z);
    float x, y;
    if (ProjectToScreen(aim, x, y))
    {
      pDrawList->AddCircle(ImVec2(x, y), fontSize * 0.7f, shadow, 20, 4.f);
      pDrawList->AddCircle(ImVec2(x, y), fontSize * 0.7f, IM_COL32(255, 255, 255, 255), 20, 2.f);
      sprintf_s(label, "%s, %.0f m", attach::KindOf(item.Target),
        XMVectorGetX(XMVector3Length(XMLoadFloat3(&item.Position) - XMLoadFloat3(&m_Camera.Position))));
      labelAt(x, y, IM_COL32(255, 255, 255, 255), label);
    }
  }

  // The one being followed: where it really is, not the smoothed copy the
  // camera follows (that trails behind it)
  if (m_AttachObject && m_AttachValid)
  {
    XMFLOAT3 aim(m_AttachRawPosition.x, m_AttachRawPosition.y + attach::AimHeight(m_AttachTarget), m_AttachRawPosition.z);
    float x, y;
    if (ProjectToScreen(aim, x, y))
    {
      pDrawList->AddCircle(ImVec2(x, y), fontSize * 0.9f, shadow, 24, 4.5f);
      pDrawList->AddCircle(ImVec2(x, y), fontSize * 0.9f, red, 24, 2.5f);
      labelAt(x + fontSize * 0.2f, y, red, "following");
    }
  }
}

bool CameraManager::DrawNearbyList()
{
  bool configChanged = false;
  float fontSize = ImGui::GetFontSize();
  unsigned long long now = GetTickCount64();
  m_NearbyWantedUntil = now + 500;

  if (!m_CameraEnabled)
  {
    ImGui::TextDisabled("Turn the camera on to see what's around it.");
    m_NearbyListHovered = false;
    return false;
  }

  ImGui::PushItemWidth(fontSize * 10.f);
  if (ImGui::SliderFloat("Within##NearbyRadius", &m_NearbyRadius, 10.f, 1000.f, "%.0f m", 2.f))
  {
    // Ctrl+click lets any value be typed in
    m_NearbyRadius = m_NearbyRadius < 10.f ? 10.f : (m_NearbyRadius > 1000.f ? 1000.f : m_NearbyRadius);
    m_NearbyRadiusChanged = true;
    configChanged = true;
  }
  // A new list once the slider is let go
  if (m_NearbyRadiusChanged && !ImGui::IsItemActive())
  {
    m_NearbyRadiusChanged = false;
    m_NearbyRescan = true;
    m_NearbyPublishRequested = true;
  }
  ImGui::PopItemWidth();
  ImGui::SameLine(0.f, fontSize * 1.5f);
  if (ImGui::Button("Refresh now"))
  {
    m_NearbyRescan = true;
    m_NearbyPublishRequested = true;
  }
  ImGui::SameLine();
  int nextUpdate = m_NearbyLastPublish ? NearbyListSeconds - (int)((now - m_NearbyLastPublish) / 1000) : 0;
  ImGui::TextDisabled("new list in %d s", nextUpdate > 0 ? nextUpdate : 0);
  ImGui::ItemTooltip("The list is made again every 30 seconds (not while the mouse is over it), so it holds "
    "still while you pick. Distances and the markers on screen follow what moves all the time.");

  configChanged |= ImGui::Checkbox("People", &m_NearbyPeople);
  ImGui::ItemTooltip("Characters (KrugerSec and other NPCs) and people in crowds.");
  ImGui::SameLine();
  configChanged |= ImGui::Checkbox("Traffic", &m_NearbyTraffic);
  ImGui::ItemTooltip("Cars and flying traffic. A swarm whose members aren't moving can't be told apart "
    "from a crowd yet, so it's in here too.");
  ImGui::SameLine();
  configChanged |= ImGui::Checkbox("Other", &m_NearbyOther);
  ImGui::ItemTooltip("Drones, vehicles, props, and lifts, platforms and other models that moved in the "
    "last 30 seconds.");
  ImGui::SameLine(0.f, fontSize * 1.5f);
  configChanged |= ImGui::Checkbox("Only moving", &m_NearbyMovingOnly);
  ImGui::SameLine(0.f, fontSize * 1.5f);
  configChanged |= ImGui::Checkbox("On screen", &m_NearbyMarkers);
  ImGui::ItemTooltip("Puts a dot on everything in the list (brighter when it's moving) and a ring on the "
    "one the mouse is over. The one being followed has a red ring.");

  int hovered = -1, shown = 0;
  ImGui::BeginChild("##NearbyList", ImVec2(0, fontSize * 15.f), true);
  m_NearbyListHovered = ImGui::IsWindowHovered();
  ImGui::Columns(4, "##NearbyColumns", false);
  ImGui::SetColumnWidth(0, fontSize * 4.5f);
  ImGui::SetColumnWidth(1, fontSize * 7.f);
  ImGui::SetColumnWidth(2, fontSize * 15.f);
  for (int i = 0; i < (int)m_Nearby.size(); ++i)
  {
    NearbyItem const& item = m_Nearby[i];
    if (!NearbyItemShown(item))
      continue;
    ++shown;

    float distance = XMVectorGetX(XMVector3Length(XMLoadFloat3(&item.Position) - XMLoadFloat3(&m_Camera.Position)));
    bool following = m_AttachObject && attach::SameTarget(item.Target, m_AttachTarget);
    float x, y;
    XMFLOAT3 aim(item.Position.x, item.Position.y + attach::AimHeight(item.Target), item.Position.z);
    bool inView = !item.Gone && ProjectToScreen(aim, x, y);

    if (item.Gone)
      ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    char label[64];
    sprintf_s(label, "%.0f m##Nearby%d", distance, i);
    if (ImGui::Selectable(label, following, ImGuiSelectableFlags_SpanAllColumns))
    {
      m_AttachListRequest = item.Target;
      m_AttachListRequested = true;
    }
    if (ImGui::IsItemHovered())
      hovered = i;
    ImGui::NextColumn();
    ImGui::TextUnformatted(attach::KindOf(item.Target));
    ImGui::NextColumn();
    if (item.Target.Agent >= 0)
      ImGui::Text("%s #%d", attach::ClassName(item.Target.Class), item.Target.Agent);
    else
      ImGui::TextUnformatted(attach::ClassName(item.Target.Class));
    ImGui::NextColumn();
    if (item.Gone)
      ImGui::TextDisabled("gone");
    else
      ImGui::TextDisabled("%s%s%s", IsNearbyMoving(item, now) ? "moving" : "still", inView ? ", in view" : "",
        following ? ", followed" : "");
    ImGui::NextColumn();
    if (item.Gone)
      ImGui::PopStyleColor();
  }
  ImGui::Columns(1);
  if (shown == 0)
    ImGui::TextDisabled(m_Nearby.empty() ? "Nothing that can be followed is this close." : "Nothing matches the filters.");
  ImGui::EndChild();
  ImGui::TextDisabled("%d within %.0f m%s, %d listed. Click one to follow it.", m_NearbyTotal, m_NearbyListedRadius,
    m_NearbyTotal > (int)m_Nearby.size() ? " (the nearest are listed)" : "", shown);
  if (m_NearbyDropped)
    ImGui::TextDisabled("%d more were left out: too many objects, try a smaller range.", m_NearbyDropped);

  DrawMarkers(hovered);
  return configChanged;
}

void CameraManager::DrawFollowUI()
{
  std::lock_guard<std::mutex> lock(m_Mutex);
  bool configChanged = false;

  InputSystem* pInput = g_mainHandle->GetInputSystem();
  float fontSize = ImGui::GetFontSize();
  char label[128];

  ImGui::PushTextWrapPos(0.f);
  ImGui::TextDisabled("The camera travels with Faith or with something else that moves, and can still be "
    "flown around it. A track played while following moves with it from where it was when the track started.");
  ImGui::PopTextWrapPos();

  ImGui::SubHeading("Faith");
  if (ImGui::Checkbox("Follow Faith", &m_AttachToFaith))
  {
    if (m_AttachToFaith)
    {
      m_AttachPicking = false;
      DetachObject(nullptr);
    }
    m_AttachHasLast = false;
    m_AttachTrackActive = false;
    util::log::Write("Camera %s Faith", m_AttachToFaith ? "follows" : "no longer follows");
  }
  ImGui::HelpMarker("Attaches the camera to Faith: where she goes, the camera goes, keeping its place next "
    "to her. With Freeze Faith off, the movement keys (W A S D, Space, Ctrl) only move her; the arrow keys, "
    "the FOV keys and the gamepad still adjust the camera. With Freeze Faith on you can fly the camera "
    "around her instead. A track that plays while this is on moves with her from where she was when it "
    "started, so a camera move recorded around her can follow her while she runs (give her the "
    "controls with \"Faith keeps control while a track plays\" in the CAMERA tab).");
  if (m_AttachToFaith)
  {
    ImGui::SameLine(0.f, fontSize * 2.f);
    configChanged |= ImGui::Checkbox("Turn with her", &m_AttachRotation);
    ImGui::HelpMarker("Also turns the camera when Faith turns. Only her heading is used, so the camera "
      "stays level when she wall-runs or climbs.");
  }

  ImGui::SubHeading("Something else");
  if (m_AttachObject)
  {
    ImGui::TextColored(ImVec4(1.f, 0.42f, 0.42f, 1.f), "Following %s", attach::ClassName(m_AttachTarget.Class));
    ImGui::SameLine();
    if (m_AttachTarget.Agent >= 0)
      ImGui::TextDisabled("#%d (%s, %.1f m)", m_AttachTarget.Agent, attach::KindOf(m_AttachTarget), m_AttachDistance);
    else
      ImGui::TextDisabled("(%s, %.1f m)", attach::KindOf(m_AttachTarget), m_AttachDistance);
    if (ImGui::Button("Go next to it"))
      m_AttachPlaceRequested = true;
    ImGui::ItemTooltip("Puts the camera behind it and a little above, looking at it.");
    ImGui::SameLine();
    if (ImGui::Button("Let go"))
    {
      m_AttachPicking = false;
      DetachObject(nullptr);
    }
    ImGui::SameLine(0.f, fontSize * 2.f);
    configChanged |= ImGui::Checkbox("Turn with it", &m_AttachRotation);
    ImGui::HelpMarker("Also turns the camera when it turns (for traffic and crowds, the way it's going), "
      "like a chase camera. The camera turns around the object, so when it's far away every turn swings "
      "the camera a long way: use Go next to it, or untick this.");
    if (m_AttachRotation)
    {
      ImGui::SameLine(0.f, fontSize * 2.f);
      configChanged |= ImGui::Checkbox("Tilt with it too", &m_AttachTilt);
      ImGui::HelpMarker("Uses all of its rotation, so the camera also pitches and rolls with it: a drone "
        "banking, flying traffic climbing. Characters lean too, so it's best left off for them.");
    }
  }
  else
    ImGui::TextDisabled("Not following anything else. Pick something in the list below, or aim at it.");

  configChanged |= ImGui::Checkbox("Put the camera next to what I pick", &m_AttachMoveCamera);
  ImGui::HelpMarker("When you pick something, the camera goes behind it and a little above, looking at "
    "it. Untick to keep the camera where it is (it then moves along with the object from there).");
  ImGui::SameLine(0.f, fontSize * 2.f);
  ImGui::PushItemWidth(fontSize * 8.f);
  if (ImGui::SliderFloat("Smoothing", &m_AttachSmoothing, 0.f, 1.f, m_AttachSmoothing > 0.f ? "%.2f" : "off"))
  {
    m_AttachSmoothing = m_AttachSmoothing < 0.f ? 0.f : (m_AttachSmoothing > 1.f ? 1.f : m_AttachSmoothing);
    configChanged = true;
  }
  ImGui::PopItemWidth();
  ImGui::HelpMarker("How softly the camera follows: it moves and turns with a copy of the object that "
    "catches up over a moment (up to half a second at 1), so the swarm's small steps and a character's "
    "wobble don't shake the view. Higher is steadier and trails a little further behind on corners and "
    "when it speeds up. 0 follows every move exactly. Doesn't change Follow Faith.");

  sprintf_s(label, "Attach to what the camera looks at  (%s)###AttachPick", pInput->GetKeyName(Action::Camera_Attach).c_str());
  if (ImGui::Button(label))
  {
    if (!m_CameraEnabled)
      ToggleCamera();
    if (m_CameraEnabled)
      m_AttachRequest = AttachRequest_Pick;
  }
  bool showCrosshair = m_ShowCrosshair || ImGui::IsItemHovered();
  ImGui::HelpMarker("Follows what moves nearest the middle of the view (within about 12 degrees, up to "
    "1000 m). The hotkey does the same with the menu closed, and pressed again on the same object it "
    "lets go. If what's followed is destroyed or despawns, the camera stops following and stays where it is.");
  ImGui::SameLine(0.f, fontSize * 2.f);
  configChanged |= ImGui::Checkbox("Crosshair", &m_ShowCrosshair);
  ImGui::ItemTooltip("Marks the middle of the screen while this tab is open (it's also shown while the "
    "mouse is over the Attach button).");
  if (showCrosshair && m_CameraEnabled)
    DrawCrosshair();
  if (m_AttachPicking)
    ImGui::TextDisabled("Watching what's in view...");

  // Also while following something: why the last pick or follow ended
  if (m_AttachMessage[0])
  {
    ImGui::PushTextWrapPos(0.f);
    ImGui::TextDisabled("%s", m_AttachMessage);
    ImGui::PopTextWrapPos();
  }

  ImGui::SubHeading("Nearby");
  configChanged |= DrawNearbyList();

  if (configChanged)
    g_mainHandle->OnConfigChanged();
}

namespace
{
  // Faith's hair is a cloth mesh (characters/faith/hair/faith_hair_3pcloth_ingame)
  // that her PamClientCharacterCustomizationComponent holds. The game only
  // switches the cloth on within its activation radius (33 m) of the camera,
  // and with her body switched (her own camera unlinked) it stays off and her
  // hair isn't drawn. A radius of 100 km keeps it on. Its mesh also uses the
  // shared LodGroups/Character_Streamable2 (LODs at 5, 15, 30, 60, 100 and
  // 1025 m), and from 100 m there's no hair in it any more, so the mesh's own
  // LodScale (only this mesh has it) is raised too. The vtables are for PE
  // timestamp 0x5762E3B7 only, so on another build nothing is looked for.
  const DWORD HairFixTimeStamp = 0x5762E3B7;
  const __int64 CustomizationComponentVTableRva = 0x1C3C360;
  const __int64 ClothEntityVTableRva = 0x1AD2998;
  const __int64 ClothEntityDataVTableRva = 0x1AD2960;
  const __int64 RigidMeshAssetVTableRva = 0x1BBDAB0;
  const int ComponentCollectionOffset = 0x38;      // soldier -> its components, one every 0x20 bytes
  const int ClothEntityDataOffset = 0x30;          // ClothEntity -> ClothEntityData
  const int ClothActivationRadiusOffset = 0x84;    // 4 floats, then SmoothBlendOutDistance x4
  const int ClothMeshOffset = 0x70;                // ClothEntityData.Mesh
  const int MeshLodScaleOffset = 0x38;             // MeshAsset.LodScale
  const float HairLodScale = 1000.f;               // the 100 m LOD at 100 km

  bool IsHairFixBuild()
  {
    IMAGE_DOS_HEADER* pDos = (IMAGE_DOS_HEADER*)g_gameHandle;
    IMAGE_NT_HEADERS* pNt = (IMAGE_NT_HEADERS*)((BYTE*)g_gameHandle + pDos->e_lfanew);
    return pNt->FileHeader.TimeDateStamp == HairFixTimeStamp;
  }

  // One qword, or 0 if it isn't readable. The component holds plenty of values
  // that aren't pointers, so every read is checked on its own.
  __int64 ReadQwordOrZero(__int64 address)
  {
    if (address < 0x10000 || address > 0x7FFFFFFF0000)
      return 0;
    __try
    {
      return *(__int64*)address;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      return 0;
    }
  }

  // The ClothEntityData of each cloth Faith's customization component holds
  int FindHairClothData(__int64 pSoldier, __int64* pOut, int max)
  {
    __int64 base = (__int64)g_gameHandle;
    int count = 0;
    __int64 pCollection = ReadQwordOrZero(pSoldier + ComponentCollectionOffset);
    if (!pCollection) return 0;
    for (int entry = 0; entry < 32; ++entry)
    {
      __int64 pComponent = ReadQwordOrZero(pCollection + 0x20 + entry * 0x20);
      if (!pComponent || ReadQwordOrZero(pComponent) != base + CustomizationComponentVTableRva)
        continue;
      for (int off = 0; off < 0x800 && count < max; off += 8)
      {
        __int64 pCloth = ReadQwordOrZero(pComponent + off);
        if (!pCloth || pCloth == pComponent || ReadQwordOrZero(pCloth) != base + ClothEntityVTableRva)
          continue;
        __int64 pData = ReadQwordOrZero(pCloth + ClothEntityDataOffset);
        if (!pData || ReadQwordOrZero(pData) != base + ClothEntityDataVTableRva)
          continue;
        bool known = false;
        for (int k = 0; k < count; ++k)
          known |= pOut[k] == pData;
        if (!known)
          pOut[count++] = pData;
      }
      break;
    }
    return count;
  }

  void WriteFloats(__int64 address, float const* values, int count)
  {
    __try
    {
      memcpy((void*)address, values, sizeof(float) * count);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
  }

  bool ReadFloats(__int64 address, float* values, int count)
  {
    __try
    {
      memcpy(values, (void*)address, sizeof(float) * count);
      return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      return false;
    }
  }
}

void CameraManager::ApplyHairFix()
{
  // While Faith's body is shown her hair cloth is kept on, see above
  static bool s_supported = IsHairFixBuild();
  bool wanted = s_supported && m_CameraEnabled && m_ThirdPersonApplied;
  if (!wanted && m_HairSavedCount == 0)
    return;

  __int64 base = (__int64)g_gameHandle;
  if (!wanted)
  {
    // Back to the game's values. The cloth data and the mesh are assets that
    // stay as long as the level does, so they're put back while they still
    // are what they were, even if Faith can't be found (a loading screen)
    for (int i = 0; i < m_HairSavedCount; ++i)
    {
      if (ReadQwordOrZero(m_HairData[i]) == base + ClothEntityDataVTableRva)
        WriteFloats(m_HairData[i] + ClothActivationRadiusOffset, m_HairSaved[i], 8);
      if (m_HairMesh[i] && ReadQwordOrZero(m_HairMesh[i]) == base + RigidMeshAssetVTableRva)
        WriteFloats(m_HairMesh[i] + MeshLodScaleOffset, &m_HairLodScale[i], 1);
    }
    m_HairSavedCount = 0;
    return;
  }

  __int64 data[MaxHairCloths];
  __int64 pSoldier = GetPlayerSoldier();
  int count = pSoldier ? FindHairClothData(pSoldier, data, MaxHairCloths) : 0;

  // The cloth is switched on near the camera; out to 100 km it's always on
  const float wide[8] = { 100000.f, 100000.f, 100000.f, 100000.f, 99000.f, 99000.f, 99000.f, 99000.f };
  for (int k = 0; k < count; ++k)
  {
    int slot = -1;
    for (int i = 0; i < m_HairSavedCount; ++i)
      if (m_HairData[i] == data[k]) slot = i;
    if (slot < 0 && m_HairSavedCount < MaxHairCloths)
    {
      slot = m_HairSavedCount;
      if (!ReadFloats(data[k] + ClothActivationRadiusOffset, m_HairSaved[slot], 8))
        continue;
      // Its mesh, only if it really is a mesh asset
      __int64 pMesh = ReadQwordOrZero(data[k] + ClothMeshOffset);
      m_HairMesh[slot] = pMesh && ReadQwordOrZero(pMesh) == base + RigidMeshAssetVTableRva &&
        ReadFloats(pMesh + MeshLodScaleOffset, &m_HairLodScale[slot], 1) ? pMesh : 0;
      // Never take the tools' own values for the game's (the game's are 33 m,
      // 28 m and a detail scale of 1)
      if (m_HairSaved[slot][0] >= wide[0] * 0.5f)
        for (int f = 0; f < 8; ++f)
          m_HairSaved[slot][f] = f < 4 ? 33.f : 28.f;
      if (m_HairMesh[slot] && m_HairLodScale[slot] >= HairLodScale * 0.5f)
        m_HairLodScale[slot] = 1.f;
      m_HairData[m_HairSavedCount++] = data[k];
      util::log::Write("Faith's hair: cloth activation radius raised from %.0f m%s while her body is shown",
        m_HairSaved[slot][0], m_HairMesh[slot] ? " and its mesh's detail range" : "");
    }
    if (slot < 0)
      continue;
    WriteFloats(data[k] + ClothActivationRadiusOffset, wide, 8);
    if (m_HairMesh[slot])
      WriteFloats(m_HairMesh[slot] + MeshLodScaleOffset, &HairLodScale, 1);
  }
}

void CameraManager::WatchInstanceLists()
{
  InstanceLists lists{};
  if (!ReadInstanceLists(m_ppMeshStreaming, lists))
  {
    m_InstancePressure = 0;
    m_InstanceFill = -1.f;
    return;
  }

  float fill = 0.f;
  for (int k = 0; k < 2; ++k)
  {
    if (lists.Capacity[k] > 0)
    {
      float f = (float)lists.Count[k] / lists.Capacity[k];
      if (f > fill) fill = f;
    }

    // The game had to move a list while it was in use: logged, since that's
    // what crashes it. (It also moves them when a level loads, while empty.)
    if (m_InstanceBegin[k] && lists.Begin[k] != m_InstanceBegin[k] && m_InstanceCount[k] > 0)
      util::log::Warning("The game outgrew its %s model instance list (%d -> %d) while using it, it can crash after this",
        k == 0 ? "positioned" : "distanced", m_InstanceCapacity[k], lists.Capacity[k]);
    m_InstanceBegin[k] = lists.Begin[k];
    m_InstanceCapacity[k] = lists.Capacity[k];
    m_InstanceCount[k] = lists.Count[k];
  }

  // Mesh records in use, counted at most 20 times a second (it reads every
  // record); in between, records added at the end count as used
  ULONGLONG now = GetTickCount64();
  if (now - m_MeshScanTick >= 50 || lists.Meshes < m_MeshScanHighWater)
  {
    int used = CountMeshesInUse(m_ppMeshStreaming);
    if (used >= 0)
    {
      m_MeshInUse = used;
      m_MeshScanHighWater = lists.Meshes;
      m_MeshScanTick = now;
    }
  }
  int inUse = m_MeshInUse + (lists.Meshes > m_MeshScanHighWater ? lists.Meshes - m_MeshScanHighWater : 0);

  // The instance lists by how full they are, the mesh table (what fills up
  // first) by records in use, close to its 4096
  int listPressure = fill >= 0.95f ? 3 : (fill >= 0.9f ? 2 : (fill >= 0.8f ? 1 : 0));
  // Its end past 4096 means the game is already writing over its instance lists
  bool overflowing = !lists.MeshesInline || lists.Meshes > MeshTableCapacity;
  int meshPressure = (overflowing || inUse >= MeshesTakeAll) ? 3 :
    (inUse >= MeshesTakeAway ? 2 : (inUse >= MeshesStopAdding ? 1 : 0));
  float meshFill = (float)inUse / MeshTableCapacity;
  if (meshFill > fill) fill = meshFill;
  if (overflowing && !m_MeshesOverflowed)
    util::log::Warning("The game's mesh table went past its %d records (%d): it writes over its model instance lists, it can crash after this",
      MeshTableCapacity, lists.Meshes);
  m_MeshesOverflowed = overflowing;
  if (inUse != g_MeshesNow)
  {
    g_MeshesNow = inUse;
    g_MeshesChangedAt = now;
  }
  m_MeshCount = inUse;
  m_MeshHighWater = lists.Meshes;
  if (inUse > m_MeshPeak)
    m_MeshPeak = inUse;

  m_InstanceFill = fill;
  // 1 hold back, 2 take two away, 3 at the edge of the room the game has:
  // everything extra goes, the camera's own zones too
  m_InstancePressure = meshPressure > listPressure ? meshPressure : listPressure;

  // With the world loaded around the camera, ask for four times the game's
  // room from the next level load on (each instance is 32 bytes, so even
  // a lot of room is only a few MB)
  if (m_StreamAroundCamera && lists.pSettings)
  {
    for (int k = 0; k < 2; ++k)
    {
      if (!m_InstanceReserveOriginal[k] && lists.Reserved[k] > 0)
      {
        m_InstanceReserveOriginal[k] = lists.Reserved[k];
        util::log::Write("Model instance lists (%s): %d of %d in use, the game reserves %d per level",
          k == 0 ? "positioned" : "distanced", lists.Count[k], lists.Capacity[k], lists.Reserved[k]);
      }
      int wanted = m_InstanceReserveOriginal[k] * 4;
      if (m_InstanceReserveOriginal[k] > 0 && lists.Reserved[k] < wanted)
      {
        RaiseInstanceReserve(lists.pSettings, k, wanted);
        util::log::Write("Asked the game for room for %d %s model instances from the next level load",
          wanted, k == 0 ? "positioned" : "distanced");
      }
    }
  }
}

void CameraManager::ApplyCulling()
{
  __int64 pWorldRenderSettings = ReadPointer(m_ppWorldRenderSettings);
  if (!pWorldRenderSettings) return;

  float& cullScale = *(float*)(pWorldRenderSettings + CullScreenAreaScaleOffset);
  if (m_CullOverride)
  {
    if (!m_CullSaved)
    {
      m_CullOriginal = cullScale;
      m_CullSaved = true;
    }
    cullScale = m_CullScale;
  }
  else if (m_CullSaved)
  {
    cullScale = m_CullOriginal;
    m_CullSaved = false;
  }
}

void CameraManager::ApplyShadowDistance()
{
  __int64 pWorldRenderSettings = ReadPointer(m_ppWorldRenderSettings);
  if (!pWorldRenderSettings) return;

  // The sun's shadow cascades cover this distance from the camera. From high
  // up the ground is further away than the game's 200 m, so it loses its shadows.
  float& distance = *(float*)(pWorldRenderSettings + ShadowmapViewDistanceOffset);
  if (m_ShadowDistance > 0)
  {
    if (!m_ShadowSaved)
    {
      m_ShadowOriginal = distance;
      m_ShadowSaved = true;
    }
    distance = m_ShadowDistance;
  }
  else if (m_ShadowSaved)
  {
    distance = m_ShadowOriginal;
    m_ShadowSaved = false;
  }

  // The 4 layers: where one ends the next fades in over this fraction, and
  // the spread blends between log (1, small layers near the camera) and even (0) splits
  float& layerFade = *(float*)(pWorldRenderSettings + ShadowmapTransitionBlendOffset);
  float& layerSpread = *(float*)(pWorldRenderSettings + ShadowmapSliceSchemeWeightOffset);
  if (m_ShadowLayersOverride)
  {
    if (!m_ShadowLayersSaved)
    {
      m_ShadowLayerFadeOriginal = layerFade;
      m_ShadowLayerSpreadOriginal = layerSpread;
      m_ShadowLayersSaved = true;
    }
    layerFade = m_ShadowLayerFade;
    layerSpread = m_ShadowLayerSpread;
  }
  else if (m_ShadowLayersSaved)
  {
    layerFade = m_ShadowLayerFadeOriginal;
    layerSpread = m_ShadowLayerSpreadOriginal;
    m_ShadowLayersSaved = false;
  }
}

void CameraManager::ApplyMotionBlur()
{
  __int64 pWorldRenderSettings = ReadPointer(m_ppWorldRenderSettings);
  if (!pWorldRenderSettings) return;

  BYTE& enable = *(BYTE*)(pWorldRenderSettings + MotionBlurEnableOffset);
  float& scale = *(float*)(pWorldRenderSettings + MotionBlurScaleOffset);
  float& maximum = *(float*)(pWorldRenderSettings + MotionBlurMaxOffset);

  if (m_MotionBlurOverride)
  {
    if (!m_MotionBlurSaved)
    {
      m_MotionBlurOriginalEnable = enable;
      m_MotionBlurOriginalScale = scale;
      m_MotionBlurOriginalMax = maximum;
      m_MotionBlurSaved = true;
    }
    enable = m_MotionBlurEnable ? 1 : 0;
    scale = m_Camera.MotionBlur;
    maximum = m_MotionBlurMax;
  }
  else if (m_MotionBlurSaved)
  {
    enable = m_MotionBlurOriginalEnable;
    scale = m_MotionBlurOriginalScale;
    maximum = m_MotionBlurOriginalMax;
    m_MotionBlurSaved = false;
  }
}

void CameraManager::DescribeState(char* buffer, size_t size)
{
  // For the crash note: no lock, these are only read
  sprintf_s(buffer, size, "camera %s, track %s, follow Faith %s, follow object %s 0x%I64X, world around camera %s "
    "(distance %.0f, zone slots %d of 128, meshes in use %d of 4096, most %d, table end %d), Faith's body %s, Fix LOD %s, shadow distance %.0f, small object culling %s %.2f, "
    "camera at %.1f %.1f %.1f",
    m_CameraEnabled ? "on" : "off", m_TrackPlayer.IsPlaying() ? "playing" : "stopped",
    m_AttachToFaith ? "on" : "off", m_AttachObject ? attach::ClassName(m_AttachTarget.Class) : "none", m_AttachObject ? m_AttachTarget.Entity : 0,
    m_StreamAroundCamera ? "on" : "off", m_StreamDistance, (int)m_StreamSlotsPeak, (int)m_MeshCount, (int)m_MeshPeak, (int)m_MeshHighWater, m_ThirdPersonApplied ? "switched" : "normal",
    m_LodFix ? "on" : "off", m_ShadowDistance, m_CullOverride ? "on" : "off", m_CullScale,
    m_Camera.Position.x, m_Camera.Position.y, m_Camera.Position.z);
}

void CameraManager::DrawStatusBar()
{
  std::lock_guard<std::mutex> lock(m_Mutex);

  const ImVec4 active(1.f, 0.42f, 0.42f, 1.f);
  const ImVec4 on = ImGui::GetStyleColorVec4(ImGuiCol_Text);
  const ImVec4 off = ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
  auto separator = [&]() { ImGui::SameLine(); ImGui::TextDisabled("|"); ImGui::SameLine(); };

  ImGui::TextColored(m_CameraEnabled ? on : off, m_CameraEnabled ? "Camera on" : "Camera off");

  separator();
  if (m_TrackPlayer.IsPlaying())
    ImGui::TextColored(active, "Playing %s  %.1f / %.1f s", m_TrackPlayer.GetTrackName().c_str(),
      m_TrackPlayer.GetPlaybackTime(), m_TrackPlayer.GetTrackLength());
  else
    ImGui::TextColored(off, "%s: %d keyframe%s", m_TrackPlayer.GetTrackName().c_str(),
      m_TrackPlayer.GetNodeCount(), m_TrackPlayer.GetNodeCount() == 1 ? "" : "s");

  if (IsTimeFrozen())
  {
    separator();
    ImGui::TextColored(on, "Time frozen");
  }

  if (m_CameraEnabled && m_AttachToFaith)
  {
    separator();
    ImGui::TextColored(on, "Following Faith");
  }
  else if (m_CameraEnabled && m_AttachObject)
  {
    separator();
    ImGui::TextColored(on, "Following: %s", attach::KindOf(m_AttachTarget));
  }

  if (m_CameraEnabled)
  {
    separator();
    if (IsPlayerControllingFaith())
      ImGui::TextColored(on, "Faith has the controls");
    else
      ImGui::TextColored(off, m_KbmDisabled ? "Faith frozen" : (m_AttachToFaith ? "Faith moves, camera follows" : "Faith moves too"));
  }
}

void CameraManager::DrawUI()
{
  std::lock_guard<std::mutex> lock(m_Mutex);
  bool configChanged = false;

  InputSystem* pInput = g_mainHandle->GetInputSystem();
  float fontSize = ImGui::GetFontSize();
  char label[128];

  sprintf_s(label, "%s  (%s)###ToggleCamera", m_CameraEnabled ? "Turn camera off" : "Turn camera on",
    pInput->GetKeyName(Action::ToggleCamera).c_str());
  if (ImGui::Button(label, ImVec2(fontSize * 12.f, 0)))
    ToggleCamera();
  ImGui::SameLine();
  if (ImGui::Button("Move camera to Faith", ImVec2(fontSize * 12.f, 0)))
  {
    if (!m_CameraEnabled)
      ToggleCamera();
    MoveToPlayer();
  }
  ImGui::ItemTooltip("Puts the free camera at Faith's head, turning it on if it's off.");
  ImGui::TextDisabled("Follow Faith, cars, people and other moving things: FOLLOW tab");

  ImGui::PushItemWidth(fontSize * 13.f);

  if (ImGui::CollapsingHeader("Camera tracks", ImGuiTreeNodeFlags_DefaultOpen))
  {
    m_TrackPlayer.DrawUI(m_Camera, m_CameraEnabled);

    if (ImGui::TreeNode("Playback options"))
    {
      configChanged |= ImGui::Checkbox("Faith keeps control while a track plays", &m_PlayerControlDuringPlayback);
      ImGui::HelpMarker("During playback the game gets the keyboard, mouse and gamepad, so you can run "
        "around as Faith while the camera follows the track.");

      configChanged |= ImGui::InputFloat("Fixed playback FPS", &m_PlaybackFps, 1.f, 10.f, 1);
      if (m_PlaybackFps < 0) m_PlaybackFps = 0;
      ImGui::HelpMarker("0 plays the track in real time. When frame-dumping a recording, set this to the "
        "capture frame rate: every rendered frame then moves the track on by exactly 1/FPS.");
      ImGui::TreePop();
    }
  }

  if (ImGui::CollapsingHeader("Time and HUD", ImGuiTreeNodeFlags_DefaultOpen))
  {
    bool timeFrozen = IsTimeFrozen();
    sprintf_s(label, "Freeze time  (%s)", pInput->GetKeyName(Action::ToggleFreezeTime).c_str());
    if (ImGui::Checkbox(label, &timeFrozen))
      ToggleTimeFreeze();

    ImGui::SameLine(0.f, fontSize * 2.f);
    bool hudVisible = IsHudVisible();
    sprintf_s(label, "Show HUD  (%s)", pInput->GetKeyName(Action::ToggleHUD).c_str());
    if (ImGui::Checkbox(label, &hudVisible))
      ToggleHudVisibility();

    g_mainHandle->GetVisualsManager()->DrawTimeOfDayUI();
  }

  if (ImGui::CollapsingHeader("Detail and draw distance"))
  {
    bool lodFix = m_LodFix;
    if (ImGui::Checkbox("Fix LOD", &lodFix))
    {
      SetLodFix(lodFix);
      configChanged = true;
    }
    ImGui::HelpMarker("Keeps every model at its most detailed version however far away it is "
      "(shadows too), like the original tools.");

    if (!m_LodFix)
    {
      float lodScale = m_LodScale;
      if (ImGui::SliderFloat("Detail distance", &lodScale, 0.f, 200.f, lodScale > 0 ? "%.1f" : "game default (5)", 2.f))
      {
        SetLodScale(lodScale < 0.1f ? 0.f : lodScale);
        configChanged = true;
      }
      ImGui::HelpMarker("How far away models keep their detailed versions. Higher = more detail at a "
        "distance. Fix LOD is the same as a very high value.");
    }

    configChanged |= ImGui::Checkbox("Show small objects far away", &m_CullOverride);
    ImGui::HelpMarker("The game hides objects that look small on screen (its value is 5). Lower values "
      "keep more small props visible at a distance, at some cost in performance.");
    if (m_CullOverride)
      configChanged |= ImGui::SliderFloat("Small object culling", &m_CullScale, 0.f, 5.f, "%.2f  (game 5)");

    float farPlane = m_FarPlane;
    if (ImGui::SliderFloat("Draw distance", &farPlane, 0.f, 100000.f, farPlane > 0 ? "%.0f m" : "game default (25000 m)", 3.f))
    {
      m_FarPlane = farPlane < 1.f ? 0.f : farPlane;
      configChanged = true;
    }
    ImGui::HelpMarker("The camera's far plane. Slide to 0 for the game's own. Ctrl+click a slider to type a value.");

    if (m_pAddZoneFocus && m_pRemoveZoneFocus)
    {
      bool streamAroundCamera = m_StreamAroundCamera;
      if (ImGui::Checkbox("Load the world around the camera", &streamAroundCamera))
      {
        m_StreamAroundCamera = streamAroundCamera;
        configChanged = true;
        util::log::Write("Loading the world around the camera %s", streamAroundCamera ? "enabled" : "disabled");
      }
      ImGui::HelpMarker("The city is streamed in zone by zone around Faith, so props near a far-away camera "
        "can be missing. With this on, the zones around the free camera are loaded too, while the camera "
        "is on. They take a moment to appear after a long move, and use more memory.");
      if (m_StreamAroundCamera)
      {
        ImGui::Indent();
        float streamDistance = m_StreamDistance;
        float maxDistance = m_StreamInView ? MaxStreamDistanceInView : MaxStreamDistance;
        if (streamDistance > maxDistance) streamDistance = maxDistance;
        if (ImGui::SliderFloat("Stream distance", &streamDistance, 0.f, maxDistance,
          streamDistance > 1.f ? "%.0f m" : "nearby zones only"))
        {
          m_StreamDistance = streamDistance < 10.f ? 0.f : streamDistance;
          configChanged = true;
        }
        ImGui::HelpMarker("How far around the camera the city is loaded. The game loads the zone the "
          "camera is in and the zones next to it; with a distance, the zones out to that far are loaded "
          "too, so less pops in around the camera. Further means more memory and more loading, so keep "
          "it as low as works for the shot.\n\nThe game can only have 128 zones loaded per streamer and "
          "breaks when it runs out, so the tools add zones slowly and stop well before that. They also "
          "hold zones back when the game's room for meshes fills up (Fix LOD on brings in more), so in a "
          "busy area the full distance may not all load.");
        if (m_CameraEnabled && m_StreamAroundCamera)
        {
          float fill = m_InstanceFill;
          if (fill >= 0.f)
          {
            int k = m_InstanceCapacity[1] > 0 && (float)m_InstanceCount[1] / m_InstanceCapacity[1] >
              (m_InstanceCapacity[0] > 0 ? (float)m_InstanceCount[0] / m_InstanceCapacity[0] : 0.f) ? 1 : 0;
            (void)k;
            (void)fill;
            if (m_InstancePressure >= 1)
              ImGui::TextColored(ImVec4(1.f, 0.8f, 0.2f, 1.f), "Meshes in use: %d of 4096, at the limit: the camera's extra zones are held back",
                (int)m_MeshCount);
            else
              ImGui::TextDisabled("Meshes in use: %d of 4096 (most so far %d)", (int)m_MeshCount, (int)m_MeshPeak);
            ImGui::HelpMarker("The game has room for 4096 different meshes and crashes when the loaded "
              "zones need more (meshes of zones that unloaded leave free room it uses again). Extra zones "
              "around the camera bring in a lot, so the tools stop adding them "
              "at 3450, take two away at 3650 and all of them at 3800 (and near the limit wait for each "
              "zone's meshes to finish loading before adding the next). Whatever is loaded around Faith "
              "counts too, so how far the camera can load depends on where she is.");
          }
          int slots = m_StreamSlotsPeak;
          if (GetTickCount64() - m_StreamLimitedTick < 3000)
            ImGui::TextColored(ImVec4(1.f, 0.8f, 0.2f, 1.f), "Zones loaded: %d of 128, near the game's limit: the furthest are held back", slots);
          else
            ImGui::TextDisabled("Zones loaded: %d of 128", slots);
        }
        if (ImGui::Checkbox("Only where the camera looks", &m_StreamInView))
        {
          if (!m_StreamInView && m_StreamDistance > MaxStreamDistance)
            m_StreamDistance = MaxStreamDistance;
          configChanged = true;
        }
        ImGui::HelpMarker("Loads the extra zones only in front of the camera (the width of the view and "
          "a little more), not all the way round it. That's about a third of the zones and meshes, so the "
          "stream distance can go up to 1000 m. Behind the camera only the zones next to it stay loaded, "
          "so turning around loads the new view over a few seconds.");
        configChanged |= ImGui::Checkbox("Load ahead of the camera", &m_StreamAhead);
        ImGui::HelpMarker("Also loads where the camera is heading (about 2.5 seconds ahead at its current "
          "speed), so zones are loading before it reaches them and less pops in while flying.");
        configChanged |= ImGui::Checkbox("Keep the track's path loaded", &m_StreamTrack);
        ImGui::HelpMarker("Also loads the zones along the selected camera track (each keyframe and every "
          "150 m between them), so nothing new has to load while it plays. After adding keyframes far "
          "away, give it a few seconds before playing. Uses more memory for long tracks.");
        ImGui::Unindent();
      }
    }
    else
    {
      ImGui::PushTextWrapPos(0.f);
      ImGui::TextDisabled("Props far away from Faith can still be missing: the city is streamed in around "
        "Faith, and the zone streamer wasn't found in this game build.");
      ImGui::PopTextWrapPos();
    }
  }

  if (ImGui::CollapsingHeader("Sun shadows"))
  {
    float shadowDistance = m_ShadowDistance;
    if (ImGui::SliderFloat("Shadow distance", &shadowDistance, 0.f, 5000.f,
      shadowDistance > 0 ? "%.0f m" : "game default (200 m)", 3.f))
    {
      m_ShadowDistance = shadowDistance < 10.f ? 0.f : shadowDistance;
      configChanged = true;
    }
    ImGui::HelpMarker("How far from the camera the sun casts shadows. Raise it when the camera is high "
      "up and the ground loses its shadows (about the camera's height plus 200 m). The game's shadow map "
      "is spread over the whole distance, so shadows close by get softer and blockier the higher this is. "
      "Fix LOD also keeps far objects casting shadows.");

    configChanged |= ImGui::Checkbox("Adjust the shadow layers", &m_ShadowLayersOverride);
    ImGui::HelpMarker("The sun's shadow is drawn in 4 layers over the shadow distance: a sharp one close "
      "to the camera, then bigger and softer ones further out. Where one layer ends the next fades in, "
      "and that's the shadow you can see fading in and out as the camera moves.");
    if (m_ShadowLayersOverride)
    {
      configChanged |= ImGui::SliderFloat("Fade between layers", &m_ShadowLayerFade, 0.f, 0.5f, "%.3f  (game 0.03)", 2.f);
      ImGui::HelpMarker("How wide the fade from one layer to the next is. Higher makes the change smoother "
        "and harder to spot. 0 switches straight from one layer to the next.");
      configChanged |= ImGui::SliderFloat("Layer spread", &m_ShadowLayerSpread, 0.f, 1.f, "%.2f  (game 0.88)");
      ImGui::HelpMarker("How the layers are spread over the shadow distance. Higher keeps the sharp layers "
        "small and close to the camera (crisper close-ups, fades nearer the camera). Lower spreads them "
        "evenly (fades further away, softer shadows close by). Helps with a long shadow distance.");
      if (ImGui::Button("Game values##ShadowLayers"))
      {
        m_ShadowLayerFade = m_ShadowLayersSaved ? m_ShadowLayerFadeOriginal : 0.03f;
        m_ShadowLayerSpread = m_ShadowLayersSaved ? m_ShadowLayerSpreadOriginal : 0.88f;
        configChanged = true;
      }
    }

    ImGui::PushTextWrapPos(0.f);
    ImGui::TextDisabled("The sun's direction, softness and the other outdoor light shadow settings are in "
      "VISUALS > Outdoor light.");
    ImGui::PopTextWrapPos();
  }

  if (ImGui::CollapsingHeader("Motion blur"))
  {
    configChanged |= ImGui::Checkbox("Control motion blur", &m_MotionBlurOverride);
    ImGui::HelpMarker("Takes over the game's motion blur settings. The strength is also stored in "
      "each keyframe (see Keyframe options under Camera tracks).");
    if (m_MotionBlurOverride)
    {
      configChanged |= ImGui::Checkbox("Motion blur on", &m_MotionBlurEnable);
      configChanged |= ImGui::SliderFloat("Strength", &m_Camera.MotionBlur, 0.f, 5.f, "%.2f  (game 0.5)", 2.f);
      configChanged |= ImGui::SliderFloat("Max blur length", &m_MotionBlurMax, 0.f, 1.f, "%.3f  (game 0.1)", 2.f);
      ImGui::HelpMarker("The longest a blur streak can get, as a fraction of the screen.");
    }
  }

  if (ImGui::CollapsingHeader("Camera settings"))
  {
    configChanged |= ImGui::SliderFloat("Movement speed", &m_Camera.MovementSpeed, 0.1f, 200.f, "%.1f m/s", 3.f);

    // Stored in radians, shown in degrees
    float turnSpeed = XMConvertToDegrees(m_Camera.RotationSpeed);
    if (ImGui::SliderFloat("Turn speed (keys)", &turnSpeed, 5.f, 230.f, "%.0f" UI_DEG "/s"))
    {
      m_Camera.RotationSpeed = XMConvertToRadians(turnSpeed);
      configChanged = true;
    }
    float rollSpeed = XMConvertToDegrees(m_Camera.RollSpeed);
    if (ImGui::SliderFloat("Roll speed", &rollSpeed, 2.f, 115.f, "%.0f" UI_DEG "/s"))
    {
      m_Camera.RollSpeed = XMConvertToRadians(rollSpeed);
      configChanged = true;
    }
    configChanged |= ImGui::SliderFloat("Zoom speed", &m_Camera.FovSpeed, 1.f, 90.f, "%.0f" UI_DEG "/s");

    // 0.0015 radians per mouse count is 1x
    float mouseScale = m_Camera.MouseSensitivity / 0.0015f;
    if (ImGui::SliderFloat("Mouse sensitivity", &mouseScale, 0.1f, 6.f, "%.2fx", 1.5f))
    {
      m_Camera.MouseSensitivity = mouseScale * 0.0015f;
      configChanged = true;
    }
    ImGui::SliderFloat("Field of view", &m_Camera.FieldOfView, 5.f, 150.f, "%.1f" UI_DEG);

    if (ImGui::Button("Reset speeds"))
    {
      Camera defaults;
      m_Camera.MovementSpeed = defaults.MovementSpeed;
      m_Camera.RotationSpeed = defaults.RotationSpeed;
      m_Camera.RollSpeed = defaults.RollSpeed;
      m_Camera.FovSpeed = defaults.FovSpeed;
      m_Camera.MouseSensitivity = defaults.MouseSensitivity;
      configChanged = true;
    }

    ImGui::SubHeading("While the camera is on");
    configChanged |= ImGui::Checkbox("Start at the game camera", &m_AutoReset);
    ImGui::HelpMarker("Each time the camera is turned on it starts where the game's camera is. "
      "Untick to carry on from where you left it.");

    sprintf_s(label, "Freeze Faith  (%s)", pInput->GetKeyName(Action::ToggleGameInput).c_str());
    configChanged |= ImGui::Checkbox(label, &m_KbmDisabled);
    ImGui::HelpMarker("Keyboard and mouse only move the camera. Untick to move Faith and the camera "
      "at the same time.");

    configChanged |= ImGui::Checkbox("Gamepad moves the camera", &m_GamepadDisabled);
    ImGui::HelpMarker("Sticks, triggers and shoulder buttons move the camera.");

    configChanged |= ImGui::Checkbox("Show Faith's body", &m_ThirdPersonToggle);
    ImGui::HelpMarker("Switches the game to Faith's third-person body while the camera is on, so she "
      "can be seen, like the original tools, with her hair (which the original tools lost). Used the "
      "next time the camera is turned on.");
  }

  if (ImGui::CollapsingHeader("Keys"))
  {
    // Qualified, ToggleCamera would be the member function here
    static const Action shownActions[] = {
      Action::ToggleUI, Action::ToggleCamera, Action::Camera_Forward, Action::Camera_Backward,
      Action::Camera_Left, Action::Camera_Right, Action::Camera_Up, Action::Camera_Down,
      Action::Camera_RollLeft, Action::Camera_RollRight, Action::Camera_IncFov, Action::Camera_DecFov,
      Action::Track_CreateNode, Action::Track_DeleteNode, Action::Track_Play, Action::Camera_Attach,
      Action::ToggleFreezeTime, Action::ToggleHUD, Action::ToggleGameInput };

    ImGui::Columns(2, "##KeyList", false);
    ImGui::SetColumnWidth(0, fontSize * 12.f);
    for (Action action : shownActions)
    {
      ImGui::TextDisabled("%s", ActionUIStringMap.at(action).c_str());
      ImGui::NextColumn();
      ImGui::Text("%s", pInput->GetKeyName(action).c_str());
      ImGui::NextColumn();
    }
    ImGui::TextDisabled("Look around");
    ImGui::NextColumn();
    ImGui::Text("Mouse (with the menu open: hold right mouse button)");
    ImGui::NextColumn();
    ImGui::TextDisabled("Zoom");
    ImGui::NextColumn();
    ImGui::Text("Mouse wheel");
    ImGui::NextColumn();
    ImGui::Columns(1);

    if (ImGui::Button("Change hotkeys..."))
      pInput->ShowUI();
  }

  ImGui::PopItemWidth();

  if (configChanged)
    g_mainHandle->OnConfigChanged();
}

void CameraManager::ReadConfig(INIReader* pReader)
{
  std::lock_guard<std::mutex> lock(m_Mutex);

  m_Camera.MovementSpeed = (float)pReader->GetReal("Camera", "MovementSpeed", 3.0);
  m_Camera.RotationSpeed = (float)pReader->GetReal("Camera", "RotationSpeed", XM_PI / 4);
  m_Camera.RollSpeed = (float)pReader->GetReal("Camera", "RollSpeed", XM_PI / 8);
  m_Camera.FovSpeed = (float)pReader->GetReal("Camera", "FovSpeed", 20.0);
  m_Camera.MouseSensitivity = (float)pReader->GetReal("Camera", "MouseSensitivity", 0.0015);
  m_AutoReset = pReader->GetBoolean("Camera", "AutoReset", true);
  m_ThirdPersonToggle = pReader->GetBoolean("Camera", "ThirdPersonBody", true);
  m_PlaybackFps = (float)pReader->GetReal("Camera", "PlaybackFps", 0.0);
  m_PlayerControlDuringPlayback = pReader->GetBoolean("Camera", "FaithControlDuringPlayback", true);
  m_KbmDisabled = pReader->GetBoolean("Camera", "FreezeFaith", true);
  m_GamepadDisabled = pReader->GetBoolean("Camera", "GamepadMovesCamera", true);
  if (m_Camera.MovementSpeed <= 0) m_Camera.MovementSpeed = 3.f;
  if (m_Camera.MouseSensitivity <= 0) m_Camera.MouseSensitivity = 0.0015f;

  m_CullOverride = pReader->GetBoolean("Misc", "SmallObjectsOverride", false);
  m_CullScale = (float)pReader->GetReal("Misc", "SmallObjectCulling", 1.0);
  m_ShadowDistance = (float)pReader->GetReal("Misc", "ShadowDistance", 0.0);
  if (m_ShadowDistance < 0) m_ShadowDistance = 0;
  m_ShadowLayersOverride = pReader->GetBoolean("Misc", "ShadowLayersOverride", false);
  m_ShadowLayerFade = (float)pReader->GetReal("Misc", "ShadowLayerFade", 0.03);
  m_ShadowLayerSpread = (float)pReader->GetReal("Misc", "ShadowLayerSpread", 0.88);
  m_StreamAroundCamera = pReader->GetBoolean("Misc", "StreamAroundCamera", false);
  m_StreamAhead = pReader->GetBoolean("Misc", "StreamAhead", true);
  m_StreamTrack = pReader->GetBoolean("Misc", "StreamTrack", true);
  m_StreamInView = pReader->GetBoolean("Misc", "StreamInView", false);
  m_StreamDistance = (float)pReader->GetReal("Misc", "StreamDistance", 0.0);
  if (m_StreamDistance < 0) m_StreamDistance = 0;
  float maxStreamDistance = m_StreamInView ? MaxStreamDistanceInView : MaxStreamDistance;
  if (m_StreamDistance > maxStreamDistance) m_StreamDistance = maxStreamDistance;

  m_MotionBlurOverride = pReader->GetBoolean("Misc", "MotionBlurOverride", false);
  m_MotionBlurEnable = pReader->GetBoolean("Misc", "MotionBlurEnable", true);
  m_Camera.MotionBlur = (float)pReader->GetReal("Misc", "MotionBlurStrength", 0.5);
  m_MotionBlurMax = (float)pReader->GetReal("Misc", "MotionBlurMax", 0.1);
  m_FarPlane = (float)pReader->GetReal("Misc", "DrawDistance", 0.0);
  m_LodScale = (float)pReader->GetReal("Misc", "LodScale", 0.0);
  m_LodFix = pReader->GetBoolean("Misc", "FixLOD", false);
  if (m_LodFix || m_LodScale > 0)
    ApplyLodSettings();

  m_AttachRotation = pReader->GetBoolean("Follow", "TurnWithIt", true);
  m_AttachTilt = pReader->GetBoolean("Follow", "TiltWithIt", false);
  m_AttachMoveCamera = pReader->GetBoolean("Follow", "PutCameraNextToIt", true);
  m_AttachSmoothing = (float)pReader->GetReal("Follow", "Smoothing", 0.4);
  m_AttachSmoothing = m_AttachSmoothing < 0.f ? 0.f : (m_AttachSmoothing > 1.f ? 1.f : m_AttachSmoothing);
  m_ShowCrosshair = pReader->GetBoolean("Follow", "Crosshair", false);
  m_NearbyRadius = (float)pReader->GetReal("Follow", "NearbyRange", 150.0);
  m_NearbyRadius = m_NearbyRadius < 10.f ? 10.f : (m_NearbyRadius > 1000.f ? 1000.f : m_NearbyRadius);
  m_NearbyListedRadius = m_NearbyRadius;
  m_NearbyPeople = pReader->GetBoolean("Follow", "NearbyPeople", true);
  m_NearbyTraffic = pReader->GetBoolean("Follow", "NearbyTraffic", true);
  m_NearbyOther = pReader->GetBoolean("Follow", "NearbyOther", true);
  m_NearbyMovingOnly = pReader->GetBoolean("Follow", "NearbyOnlyMoving", false);
  m_NearbyMarkers = pReader->GetBoolean("Follow", "NearbyOnScreen", true);
}

const std::string CameraManager::GetConfig()
{
  std::lock_guard<std::mutex> lock(m_Mutex);

  std::string config = "[Camera]\n";
  config += "MovementSpeed = " + std::to_string(m_Camera.MovementSpeed) + "\n";
  config += "RotationSpeed = " + std::to_string(m_Camera.RotationSpeed) + "\n";
  config += "RollSpeed = " + std::to_string(m_Camera.RollSpeed) + "\n";
  config += "FovSpeed = " + std::to_string(m_Camera.FovSpeed) + "\n";
  config += "MouseSensitivity = " + std::to_string(m_Camera.MouseSensitivity) + "\n";
  config += std::string("AutoReset = ") + (m_AutoReset ? "true" : "false") + "\n";
  config += std::string("ThirdPersonBody = ") + (m_ThirdPersonToggle ? "true" : "false") + "\n";
  config += "PlaybackFps = " + std::to_string(m_PlaybackFps) + "\n";
  config += std::string("FaithControlDuringPlayback = ") + (m_PlayerControlDuringPlayback ? "true" : "false") + "\n";
  config += std::string("FreezeFaith = ") + (m_KbmDisabled ? "true" : "false") + "\n";
  config += std::string("GamepadMovesCamera = ") + (m_GamepadDisabled ? "true" : "false") + "\n\n";
  config += "[Misc]\n";
  config += std::string("FixLOD = ") + (m_LodFix ? "true" : "false") + "\n";
  config += "LodScale = " + std::to_string(m_LodScale) + "\n";
  config += "DrawDistance = " + std::to_string(m_FarPlane) + "\n";
  config += std::string("MotionBlurOverride = ") + (m_MotionBlurOverride ? "true" : "false") + "\n";
  config += std::string("MotionBlurEnable = ") + (m_MotionBlurEnable ? "true" : "false") + "\n";
  config += "MotionBlurStrength = " + std::to_string(m_Camera.MotionBlur) + "\n";
  config += "MotionBlurMax = " + std::to_string(m_MotionBlurMax) + "\n";
  config += std::string("SmallObjectsOverride = ") + (m_CullOverride ? "true" : "false") + "\n";
  config += "SmallObjectCulling = " + std::to_string(m_CullScale) + "\n";
  config += "ShadowDistance = " + std::to_string(m_ShadowDistance) + "\n";
  config += std::string("ShadowLayersOverride = ") + (m_ShadowLayersOverride ? "true" : "false") + "\n";
  config += "ShadowLayerFade = " + std::to_string(m_ShadowLayerFade) + "\n";
  config += "ShadowLayerSpread = " + std::to_string(m_ShadowLayerSpread) + "\n";
  config += std::string("StreamAroundCamera = ") + (m_StreamAroundCamera ? "true" : "false") + "\n";
  config += std::string("StreamAhead = ") + (m_StreamAhead ? "true" : "false") + "\n";
  config += std::string("StreamTrack = ") + (m_StreamTrack ? "true" : "false") + "\n";
  config += std::string("StreamInView = ") + (m_StreamInView ? "true" : "false") + "\n";
  config += "StreamDistance = " + std::to_string(m_StreamDistance) + "\n\n";
  config += "[Follow]\n";
  config += std::string("TurnWithIt = ") + (m_AttachRotation ? "true" : "false") + "\n";
  config += std::string("TiltWithIt = ") + (m_AttachTilt ? "true" : "false") + "\n";
  config += std::string("PutCameraNextToIt = ") + (m_AttachMoveCamera ? "true" : "false") + "\n";
  config += "Smoothing = " + std::to_string(m_AttachSmoothing) + "\n";
  config += std::string("Crosshair = ") + (m_ShowCrosshair ? "true" : "false") + "\n";
  config += "NearbyRange = " + std::to_string(m_NearbyRadius) + "\n";
  config += std::string("NearbyPeople = ") + (m_NearbyPeople ? "true" : "false") + "\n";
  config += std::string("NearbyTraffic = ") + (m_NearbyTraffic ? "true" : "false") + "\n";
  config += std::string("NearbyOther = ") + (m_NearbyOther ? "true" : "false") + "\n";
  config += std::string("NearbyOnlyMoving = ") + (m_NearbyMovingOnly ? "true" : "false") + "\n";
  config += std::string("NearbyOnScreen = ") + (m_NearbyMarkers ? "true" : "false") + "\n\n";

  return config;
}

void CameraManager::UpdateCamera(double dt, double trackDt)
{
  float pitch = (float)(m_Camera.dPitch * dt * m_Camera.RotationSpeed) + m_Camera.dMousePitch;
  float yaw = (float)(m_Camera.dYaw * dt * m_Camera.RotationSpeed) + m_Camera.dMouseYaw;
  float roll = (float)(m_Camera.dRoll * dt * m_Camera.RollSpeed);

  XMVECTOR qPitch = XMQuaternionRotationRollPitchYaw(pitch, 0, 0);
  XMVECTOR qYaw = XMQuaternionRotationRollPitchYaw(0, yaw, 0);
  XMVECTOR qRoll = XMQuaternionRotationRollPitchYaw(0, 0, roll);

  XMVECTOR qRotation = XMLoadFloat4(&m_Camera.Rotation);
  XMVECTOR vPosition = XMLoadFloat3(&m_Camera.Position);

  // Adds delta rotations to camera rotation
  qRotation = XMQuaternionMultiply(qPitch, qRotation);
  qRotation = XMQuaternionMultiply(qRotation, qYaw);
  qRotation = XMQuaternionMultiply(qRoll, qRotation);

  // Make sure it's normalized
  qRotation = XMQuaternionNormalize(qRotation);
  m_Camera.FieldOfView += (float)(m_Camera.dFov * dt * m_Camera.FovSpeed);

  // If a camera track is being played, get the current
  // state and overwrite position/rotation/FoV.
  bool trackPlaying = m_TrackPlayer.IsPlaying();
  if (trackPlaying)
  {
    CatmullRomNode resultNode = m_TrackPlayer.PlayForward(trackDt);

    // Following Faith: the whole track moves with her from where she was
    // when it started playing
    if (m_AttachTrackActive && m_AttachValid)
    {
      XMMATRIX moved = XMMatrixInverse(nullptr, XMLoadFloat4x4(&m_AttachTrackStart)) * XMLoadFloat4x4(&m_AttachNow);
      XMMATRIX trackCamera = XMMatrixRotationQuaternion(XMLoadFloat4(&resultNode.Rotation)) *
        XMMatrixTranslationFromVector(XMLoadFloat3(&resultNode.Position));
      XMVECTOR scale, rotation, translation;
      if (XMMatrixDecompose(&scale, &rotation, &translation, trackCamera * moved))
      {
        XMStoreFloat4(&resultNode.Rotation, XMQuaternionNormalize(rotation));
        XMStoreFloat3(&resultNode.Position, translation);
      }
    }

    if (m_TrackPlayer.IsRotationLocked())
      qRotation = XMLoadFloat4(&resultNode.Rotation);

    if (m_TrackPlayer.IsFovLocked())
      m_Camera.FieldOfView = resultNode.FieldOfView;

    if (m_TrackPlayer.IsMotionBlurLocked())
      m_Camera.MotionBlur = resultNode.MotionBlur < 0 ? 0 : resultNode.MotionBlur;

    if (m_TrackPlayer.IsTimeOfDayLocked() && resultNode.TimeOfDay >= 0)
    {
      g_mainHandle->GetVisualsManager()->SetTrackTimeOfDay(resultNode.TimeOfDay);
      m_TrackTimeOfDayApplied = true;
    }

    vPosition = XMLoadFloat3(&resultNode.Position);
  }

  if (m_Camera.FieldOfView < 5.f) m_Camera.FieldOfView = 5.f;
  if (m_Camera.FieldOfView > 150.f) m_Camera.FieldOfView = 150.f;

  // Create rotation matrix
  XMMATRIX rotMatrix = XMMatrixRotationQuaternion(qRotation);

  // Add delta positions
  if (!trackPlaying)
  {
    float moveScale = (float)(dt * m_Camera.MovementSpeed);
    vPosition += rotMatrix.r[0] * (m_Camera.dX * moveScale);
    vPosition += rotMatrix.r[1] * (m_Camera.dY * moveScale);
    vPosition += rotMatrix.r[2] * (m_Camera.dZ * moveScale);
  }

  // Store results
  rotMatrix.r[3] = XMVectorSetW(vPosition, 1.0f);

  XMStoreFloat3(&m_Camera.Position, vPosition);
  XMStoreFloat4(&m_Camera.Rotation, qRotation);
  XMStoreFloat4x4(&m_Camera.Transform, rotMatrix);

  m_Camera.dX = 0;
  m_Camera.dY = 0;
  m_Camera.dZ = 0;
  m_Camera.dPitch = 0;
  m_Camera.dYaw = 0;
  m_Camera.dRoll = 0;
  m_Camera.dFov = 0;
  m_Camera.dMousePitch = 0;
  m_Camera.dMouseYaw = 0;
}

void CameraManager::UpdateInput(double dt, XMFLOAT3 const& mouseState)
{
  InputSystem* pInput = g_mainHandle->GetInputSystem();
  UI* pUI = g_mainHandle->GetUI();

  if (!g_hasFocus || pUI->HasKeyboardFocus())
    return;

  // Faith has the controls during playback, the camera ignores them
  if (IsPlayerControllingFaith())
    return;

  // Same directions as the original tools: W moves along -forward (row 2),
  // D along +right (row 0), Space along +up (row 1).
  m_Camera.dX = pInput->GetActionState(Camera_Right) - pInput->GetActionState(Camera_Left);
  m_Camera.dY = pInput->GetActionState(Camera_Up) - pInput->GetActionState(Camera_Down);
  m_Camera.dZ = pInput->GetActionState(Camera_Backward) - pInput->GetActionState(Camera_Forward);

  m_Camera.dPitch = pInput->GetActionState(Camera_PitchUp) - pInput->GetActionState(Camera_PitchDown);
  m_Camera.dYaw = pInput->GetActionState(Camera_YawLeft) - pInput->GetActionState(Camera_YawRight);
  m_Camera.dRoll = pInput->GetActionState(Camera_RollLeft) - pInput->GetActionState(Camera_RollRight);
  m_Camera.dFov = pInput->GetActionState(Camera_IncFov) - pInput->GetActionState(Camera_DecFov);

  // With the menu open the mouse belongs to the menu, unless the right button
  // is held outside the menu window
  bool mouseLook = !pUI->IsEnabled() || (!pUI->HasMouseFocus() && (GetAsyncKeyState(VK_RBUTTON) & 0x8000));
  if (m_KbmDisabled && mouseLook)
  {
    m_Camera.dMousePitch = -mouseState.y * m_Camera.MouseSensitivity;
    m_Camera.dMouseYaw = -mouseState.x * m_Camera.MouseSensitivity;
    m_Camera.FieldOfView -= mouseState.z / WHEEL_DELTA * 2.f; // 2 degrees per wheel notch
  }
}

void CameraManager::ToggleCamera()
{
  // A short debounce so one press (or a click and a press) can't toggle twice
  ULONGLONG now = GetTickCount64();
  if (now - m_LastToggleTime < 300)
    return;
  m_LastToggleTime = now;

  bool enable = !m_CameraEnabled;

  if (enable)
  {
    // Copy the game camera before the third-person switch below detaches
    // it, after that the main view has no position any more
    bool gameCameraInWorld = SeedFromGameCamera();
    if (!gameCameraInWorld)
    {
      // Seeding moved nothing, so put the camera at Faith (if there is one)
      util::log::Warning("The game camera isn't in the world (loading screen, respawn or cutscene?)");
      MoveToPlayer();
    }
    else if (!m_FirstEnable && !m_AutoReset)
    {
      // Seeding is only wanted with Start at the game camera: go back to
      // where the free camera was
      m_Camera.Position = m_LastCameraPosition;
      m_Camera.Rotation = m_LastCameraRotation;
    }

    // Switching Faith's body during a loading screen or cutscene can leave
    // the game with a broken player camera, so only in normal play
    if (m_ThirdPersonToggle && gameCameraInWorld)
    {
      SetThirdPersonBody(true);
      m_ThirdPersonApplied = true;
    }

    m_FirstEnable = false;
    m_HasLastFrame = false;
    m_AttachHasLast = false;
    m_AttachTrackActive = false;
    m_StreamingFocusReady = false;
    m_CameraEnabled = true;
  }
  else
  {
    m_LastCameraPosition = m_Camera.Position;
    m_LastCameraRotation = m_Camera.Rotation;
    m_CameraEnabled = false;
    // Picks and clicks that weren't done yet would happen when it's on again
    m_AttachRequest = AttachRequest_None;
    m_AttachPicking = false;
    m_AttachListRequested = false;
    m_AttachPlaceRequested = false;
    m_StreamingFocusReady = false;
    m_TrackPlayer.Stop();
    ApplyFieldOfView(-1.f);

    if (m_ThirdPersonApplied)
    {
      SetThirdPersonBody(false);
      m_ThirdPersonApplied = false;
    }
  }

  util::log::Write("Camera %s", enable ? "enabled" : "disabled");
}

void CameraManager::MoveToPlayer()
{
  // The entity the original tools read (+0x40) holds zeros on this build,
  // the soldier transform is where Faith actually is (at her feet)
  XMFLOAT4X4A transform;
  if (!GetPlayerTransform(transform) ||
    (transform._41 == 0 && transform._42 == 0 && transform._43 == 0))
  {
    util::log::Warning("Couldn't find Faith, is a level loaded?");
    return;
  }

  m_Camera.Position = XMFLOAT3(transform._41, transform._42 + FaithEyeHeight, transform._43);
  util::log::Write("Camera moved to Faith at %.2f %.2f %.2f",
    m_Camera.Position.x, m_Camera.Position.y, m_Camera.Position.z);
}

bool CameraManager::SeedFromGameCamera()
{
  __int64 pGameRenderer = ReadPointer(m_ppGameRenderer);
  if (!pGameRenderer) return false;

  __int64 pRenderView = *(__int64*)(pGameRenderer + RenderViewOffset);
  if (!pRenderView) return false;

  // Near the origin (0, 1, 0 was seen) means there's no game camera in the
  // world right now: a loading screen, a respawn or a cutscene
  auto inWorld = [](XMFLOAT4X4 const* p) { return fabsf(p->_41) + fabsf(p->_42) + fabsf(p->_43) > 2.f; };
  XMFLOAT4X4* pTransform = (XMFLOAT4X4*)(pRenderView + RenderViewTransform1);
  if (!inWorld(pTransform))
    pTransform = (XMFLOAT4X4*)(pRenderView + RenderViewTransform2);
  if (!inWorld(pTransform))
    return false;

  XMMATRIX transform = XMLoadFloat4x4(pTransform);
  m_Camera.Position = XMFLOAT3(pTransform->_41, pTransform->_42, pTransform->_43);
  XMStoreFloat4(&m_Camera.Rotation, XMQuaternionNormalize(XMQuaternionRotationMatrix(transform)));

  util::log::Write("Camera starts at the game camera, %.2f %.2f %.2f",
    m_Camera.Position.x, m_Camera.Position.y, m_Camera.Position.z);
  return true;
}

__int64* CameraManager::GetPlayerEntityPair()
{
  // ClientGameContext + 0x78 -> +0xE0 -> +0xD8.
  // The names are guesses, the path is what the original tools used.
  __int64 pContext = ReadPointer(m_ppClientGameContext);
  if (!pContext) return nullptr;

  __int64 pPlayerManager = *(__int64*)(pContext + 0x78);
  if (!pPlayerManager) return nullptr;

  __int64 pLocalPlayer = *(__int64*)(pPlayerManager + 0xE0);
  if (!pLocalPlayer) return nullptr;

  __int64* pEntityPair = *(__int64**)(pLocalPlayer + 0xD8);
  if (!pEntityPair || !pEntityPair[0]) return nullptr;

  return pEntityPair;
}

void CameraManager::SetLodFix(bool enable)
{
  m_LodFix = enable;
  ApplyLodSettings();
  util::log::Write("LOD fix %s", enable ? "enabled" : "disabled");
}

void CameraManager::SetLodScale(float scale)
{
  m_LodScale = scale;
  ApplyLodSettings();
}

void CameraManager::ApplyLodSettings()
{
  __int64 pMeshSettings = ReadPointer(m_ppMeshSettings);
  if (!pMeshSettings) return;

  // Fix LOD uses the original tools' values (sub_180019510 in CT_MirrorsEdge.dll).
  // The original restored -1 / 5 / 5 afterwards, but the game's own +0x20 is 0,
  // so the values from before the first change are put back instead.
  int& forceLod = *(int*)(pMeshSettings + 0x20);
  float& lodScaleA = *(float*)(pMeshSettings + LodScaleOffsetA);
  float& lodScaleB = *(float*)(pMeshSettings + LodScaleOffsetB);

  if (!m_LodFix && m_LodScale <= 0)
  {
    if (m_LodSaved)
    {
      forceLod = (int)m_LodOriginal[0];
      lodScaleA = m_LodOriginal[1];
      lodScaleB = m_LodOriginal[2];
      m_LodSaved = false;
    }
    return;
  }

  if (!m_LodSaved)
  {
    m_LodOriginal[0] = (float)forceLod;
    m_LodOriginal[1] = lodScaleA;
    m_LodOriginal[2] = lodScaleB;
    m_LodSaved = true;
  }

  float scale = m_LodFix ? 100000.f : m_LodScale;
  forceLod = m_LodFix ? 0 : (int)m_LodOriginal[0];
  lodScaleA = scale;
  lodScaleB = scale;
}

void CameraManager::WriteToGameCamera(__int64 pRenderView)
{
  // The original tools wrote the same matrix to both transforms
  memcpy((void*)(pRenderView + RenderViewTransform0), &m_Camera.Transform, sizeof(XMFLOAT4X4));
  memcpy((void*)(pRenderView + RenderViewTransform1), &m_Camera.Transform, sizeof(XMFLOAT4X4));
}

void CameraManager::ApplyFieldOfView(float fov)
{
  __int64 pRenderSettings = ReadPointer(m_ppGameRenderSettings);
  if (pRenderSettings)
    *(float*)(pRenderSettings + ForceFovOffset) = fov;
}

void CameraManager::SetThirdPersonBody(bool enable)
{
  // Does what the original tools' camera toggle did (sub_1800034c0 in
  // CT_MirrorsEdge.dll). OFFSET_3PTOGGLE is FPSCamera's vtable, and what its
  // first function returns is FPSCamera's type info: +0x28 is its class id
  // range (0xED..0xED). 0xF2..0 makes nothing count as an FPSCamera any more,
  // so the game draws Faith's whole body. Her own camera is unlinked too,
  // otherwise it still hides her head, but only after half a second (see
  // UnlinkFaithCamera): her hair cloth is only switched on or off while it's
  // linked, so it has to come on first (ApplyHairFix keeps it on).
  typedef __int64(__fastcall* tGetViewSettings)(void*);

  // Same as GetPlayerTransform: no crash if the player is half loaded
  __try
  {
    __int64* pEntityPair = GetPlayerEntityPair();
    if (!pEntityPair || !m_p3PToggle) return;

    // FPSCamera's getType. The original passed the function's own address in rcx.
    tGetViewSettings getViewSettings = *(tGetViewSettings*)m_p3PToggle;
    __int64 pViewSettings = getViewSettings((void*)getViewSettings);
    if (!pViewSettings) return;

    __int64 pEntity = pEntityPair[0];
    if (enable)
    {
      // Remember exactly what was there, to put back the same value later
      m_ThirdPersonEntity = pEntity;
      m_ThirdPersonLink = *(__int64*)(pEntity + 0x150);
      *(DWORD*)(pViewSettings + 0x28) = 0xF2;
      m_ThirdPersonUnlinkAt = GetTickCount64() + ThirdPersonUnlinkDelayMs;
    }
    else
    {
      m_ThirdPersonUnlinkAt = 0;
      *(DWORD*)(pViewSettings + 0x28) = 0x00ED00ED;
      // Only on the same Faith, and only if the game hasn't linked its own
      // camera since. After a respawn or a level load the player is a new
      // object, and writing the old link into it would break it.
      if (pEntity == m_ThirdPersonEntity && *(__int64*)(pEntity + 0x150) == 0)
        *(__int64*)(pEntity + 0x150) = m_ThirdPersonLink ? m_ThirdPersonLink : pEntityPair[1];
      else if (pEntity != m_ThirdPersonEntity)
        util::log::Warning("Faith was replaced while the camera was on (respawn or level load), her camera was left as the game set it up");
      m_ThirdPersonEntity = 0;
    }
  }
  __except (EXCEPTION_EXECUTE_HANDLER)
  {
    util::log::Warning("Couldn't switch Faith's third-person body, is a level loaded?");
  }
}

void CameraManager::UnlinkFaithCamera()
{
  // The second half of the body switch: unlinked, her own camera no longer
  // hides her head. Only on the same Faith, with the link the switch saw.
  __try
  {
    __int64* pEntityPair = GetPlayerEntityPair();
    if (!pEntityPair) return;
    __int64 pEntity = pEntityPair[0];
    if (pEntity == m_ThirdPersonEntity && *(__int64*)(pEntity + 0x150) == m_ThirdPersonLink)
      *(__int64*)(pEntity + 0x150) = 0;
  }
  __except (EXCEPTION_EXECUTE_HANDLER)
  {
    util::log::Warning("Couldn't unlink Faith's camera, is a level loaded?");
  }
}

bool CameraManager::IsTimeFrozen()
{
  // Read from the game, so it stays right if something else changes the time scale
  __int64 pTimeSettings = ReadPointer(m_ppGameTimeSettings);
  return pTimeSettings && *(float*)(pTimeSettings + TimeScaleOffset) == 0.f;
}

void CameraManager::ToggleTimeFreeze()
{
  __int64 pTimeSettings = ReadPointer(m_ppGameTimeSettings);
  if (!pTimeSettings) return;

  bool freeze = !IsTimeFrozen();
  *(float*)(pTimeSettings + TimeScaleOffset) = freeze ? 0.f : 1.f;
  util::log::Write("Time %s", freeze ? "frozen" : "unfrozen");
}

bool CameraManager::GetPlayerTransform(XMFLOAT4X4A& transform)
{
  // ClientGameContext + 0x68 -> +0x568 -> +0x14A0 is the player's soldier.
  // Its vtable + 0xC0 copies its transform out (the original tools used it
  // for "lock to player"). The function uses movaps, so the output has to be
  // 16-byte aligned.
  // Called from the menu, so the player can be half gone during a level
  // load. A bad pointer gives "Faith not found" instead of a crash.
  __try
  {
    __int64 pContext = ReadPointer(m_ppClientGameContext);
    if (!pContext) return false;
    __int64 p = *(__int64*)(pContext + 0x68);
    if (!p) return false;
    p = *(__int64*)(p + 0x568);
    if (!p) return false;
    __int64 pSoldier = *(__int64*)(p + 0x14A0);
    if (!pSoldier) return false;

    typedef void(__fastcall* tGetTransform)(__int64, XMFLOAT4X4A*);
    tGetTransform getTransform = *(tGetTransform*)(*(__int64*)pSoldier + 0xC0);
    getTransform(pSoldier, &transform);
    return true;
  }
  __except (EXCEPTION_EXECUTE_HANDLER)
  {
    return false;
  }
}

__int64 CameraManager::GetPlayerSoldier()
{
  // The same path as GetPlayerTransform, without calling anything
  __try
  {
    __int64 pContext = ReadPointer(m_ppClientGameContext);
    if (!pContext) return 0;
    __int64 p = *(__int64*)(pContext + 0x68);
    if (!p) return 0;
    p = *(__int64*)(p + 0x568);
    if (!p) return 0;
    return *(__int64*)(p + 0x14A0);
  }
  __except (EXCEPTION_EXECUTE_HANDLER)
  {
    return 0;
  }
}

bool CameraManager::IsHudVisible()
{
  __int64 pUISettings = ReadPointer(m_ppUISettings);
  return !pUISettings || *(BYTE*)(pUISettings + DrawHudOffset) != 0;
}

void CameraManager::ToggleHudVisibility()
{
  __int64 pUISettings = ReadPointer(m_ppUISettings);
  if (!pUISettings) return;

  BYTE& drawHud = *(BYTE*)(pUISettings + DrawHudOffset);
  drawHud = drawHud ? 0 : 1;
}
