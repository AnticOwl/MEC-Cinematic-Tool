#pragma once
#include "TrackPlayer.h"
#include "FaithReplay.h"
#include "AttachTargets.h"
#include "IgcsCameraData.h"
#include "IgcsSession.h"
#include "../inih/cpp/INIReader.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

struct ImDrawList;   // (imgui.h's)

class CameraManager
{
public:
  CameraManager();
  ~CameraManager();

  // Tools thread, from InputSystem::HotkeyUpdate
  void HotkeyUpdate();
  // Game thread, from the camera update hook. Moves the camera and
  // writes it over the game's main render view.
  void OnCameraUpdate(__int64 pRenderView);
  // Game thread, from the zone streamer update hook. Adds a streaming focus
  // at the free camera, or takes it away again.
  void OnZoneStreamerUpdate(__int64 pZoneStreamer);
  // Game thread, from the zone streamer's sorter hook: orders the zones about
  // to load by the free camera's view. False to leave it to the game.
  bool OnZoneLoadOrder(__int64 pSorter, float const* pCache, unsigned short* pZones, int count);
  // Game thread, from the vista hook: whether the tools hold this zone of
  // the vista back (it stays hidden when the game shows it)
  bool IsVistaZoneHeldBack(__int64 pVista, unsigned int zone);
  // From the sound listener's hooks (whichever thread the sound update is
  // on; no mutex, the camera hook's pose is kept apart for them): the
  // listener's transform made from the game's (its w's kept) and its
  // velocity while the free camera is heard from. False to leave the game's.
  bool GetListenerTransform(float const* pGameTransform, float* pTransform);
  bool GetListenerVelocity(float* pVelocity);
  // Any thread: whether the lair's two windows that open are to be shut
  // (GAME > Close the lair's open windows, UpdateLairWindows): the weather
  // keeps the wind out of the lair then (Weather.cpp's InShutLair)
  bool LairWindowsShut() const { return m_CloseLairWindows; }
  // Render thread, from the Present hook
  void DrawUI();
  // The FOLLOW tab: following Faith or another object
  void DrawFollowUI();
  // The GAME tab: game speed, time of day, HUD, detail and draw distance, sun shadows
  void DrawGameUI();
  // Over the picture, every menu frame (menuOpen) and, while WantsOverlay,
  // with the menu closed too: the letterbox and the composition guides, and
  // with the menu open the track's path. Before the menu's windows (the
  // letterbox goes under them).
  void DrawOverlay(bool menuOpen);
  bool WantsOverlay();
  // The REPLAY tab: recording Faith's movement and playing it on her body
  void DrawReplayUI();
  // One line under the menu tabs: camera, track and time state
  void DrawStatusBar();
  // A line about what the tools are doing, for the crash note
  void DescribeState(char* buffer, size_t size);

  // Native IGCS entry points. They only change logical camera state; the
  // normal game-thread hook remains the writer of the game's render view.
  int StartIgcsSession(std::uint8_t type);
  void EndIgcsSession();
  void MoveIgcsCamera(float horizontal, float vertical, float fovDegrees, bool fromStartPosition);
  igcs::CameraData GetIgcsCameraData();
  void SetIgcsConnected(bool connected) { m_IgcsConnected = connected; }

  bool IsCameraEnabled() { return m_CameraEnabled; }
  // While a track plays Faith can be given the controls back
  bool IsPlayerControllingFaith() { return !m_IgcsSessionActive && m_PlayerControlDuringPlayback && m_TrackPlayer.IsPlaying(); }
  bool IsGamepadDisabled() { return m_IgcsSessionActive || (m_CameraEnabled && m_GamepadDisabled && !IsPlayerControllingFaith()); };
  bool IsKbmDisabled() { return m_IgcsSessionActive || (m_CameraEnabled && m_KbmDisabled && !IsPlayerControllingFaith()); };
  // Following Faith while she isn't frozen: the movement keys only move her,
  // so the camera keeps its place next to her instead of flying off as well
  bool IsMovementKeysForFaith() { return !m_IgcsSessionActive && m_CameraEnabled && m_AttachToFaith && !m_KbmDisabled; }

  void ReadConfig(INIReader* pReader);
  const std::string GetConfig();

private:
  enum AttachRequest
  {
    AttachRequest_None,
    AttachRequest_Pick,   // what the camera looks at
    AttachRequest_Toggle  // the same, or let go if that's what it already follows
  };

  // One row of the FOLLOW tab's list of what's around the camera
  struct NearbyItem
  {
    attach::Target Target;
    DirectX::XMFLOAT3 Position; // this frame's, for the markers on screen
    unsigned long long LastMoved; // GetTickCount64 when it last moved, 0 = not seen moving
    bool Gone;                  // couldn't be read any more
  };
  static bool IsNearbyMoving(NearbyItem const& item, unsigned long long now)
  {
    // Not now - LastMoved: LastMoved can be a tick ahead of another thread's now
    return item.LastMoved && now < item.LastMoved + 3000;
  }
  static const int NearbyListSeconds = 30; // how long the list's rows stay as they are
  struct NearbySeen
  {
    DirectX::XMFLOAT3 Position;
    unsigned long long LastMoved; // GetTickCount64, 0 = not seen moving
  };

  void UpdateCamera(double dt, double trackDt);
  // A playing track kept out of walls (see m_TrackCollision); game thread
  DirectX::XMVECTOR GuardTrackPlace(DirectX::XMVECTOR wanted, double dt);
  // The selected track's path checked for walls a little each frame, and
  // taken round them when asked (see m_WallSpans); game thread
  void StepTrackWallCheck();
  void FixTrackWalls();
  // Keyframes ticked Orbit keyframe in the menu (TrackPlayer::OrbitRequest):
  // placed round the focus, and the best way round found; game thread
  void ProcessOrbitRequests();
  // The way round from orbit keyframe index to the next: the way the track
  // goes round the focus, checked for walls (risen or the other way round
  // when blocked). Gives what it found, for the menu.
  std::string FindOrbitWay(int index, bool ownStep);
  void UpdateInput(double dt, DirectX::XMFLOAT3 const& mouseState);
  igcs::capture::Pose GetIgcsPose() const;
  void ApplyIgcsPose(igcs::capture::Pose const& pose);
  void ClearCameraDeltas();
  void ResetIgcsTransientState();

  void ToggleCamera();
  void MoveToPlayer();
  // The free camera at the replayed Faith's head while a replay plays (false: none plays)
  bool MoveToReplayedFaith();
  // Its button (on while a replay plays; turns the camera on if it's off)
  void DrawMoveToReplayedFaith(float width);

  bool SeedFromGameCamera();
  void WriteToGameCamera(__int64 pRenderView);
  void ApplyFieldOfView(float fov);
  void ApplyMotionBlur();
  void ApplyCulling();
  void WatchInstanceLists();
  // Room for the whole city: the renderer's draw batches spread from this
  // many chunks (0 off), logged with when; only where it's safe (see
  // WatchInstanceLists and ReadConfig)
  void SwitchBatchSpread(int chunks, const char* when);
  void ApplyHairFix();
  void ApplyAnimationSmoothing();
  void ApplyBillboardDetail();
  void ApplyNearReflections();
  void UnlinkFaithCamera();
  void ApplyShadowDistance();
  void PublishStreamingFocus(double dt);
  void SetThirdPersonBody(bool enable);
  // The two ways of showing Faith's body (see SetThirdPersonBody)
  void SetWholeFirstPersonBody(bool enable);
  bool SetViewModeBody(bool enable, bool quiet = false);
  void KeepViewModeBody();
  __int64* GetPlayerEntityPair();
  bool GetPlayerTransform(DirectX::XMFLOAT4X4A& transform);
  __int64 GetPlayerSoldier();
  bool StreamingPausedForFaith(); // zone streamer thread
  bool GetAttachTransform(DirectX::XMMATRIX& transform);
  // dt is real time (for the pick's watch), smoothDt the fixed playback step if there's one
  void FollowAttachTarget(double dt, double smoothDt);
  void StartAttachPick(AttachRequest request);
  void FinishAttachPick();
  void AttachToObject(attach::Target const& target);
  void DetachObject(const char* reason);
  attach::View MakeAttachView(__int64 pSoldier);
  void MoveCameraNextTo(DirectX::XMFLOAT4X4 const& object, attach::Target const& target);
  void UpdateNearby();
  void ScanNearby();
  bool ProjectToScreen(DirectX::XMFLOAT3 const& world, float& x, float& y);
  // The same, also off screen (false only behind the camera); onScreen
  // whether it's in the picture
  bool ProjectPoint(DirectX::XMFLOAT3 const& world, float& x, float& y, bool& onScreen);
  bool NearbyGroupShown(attach::Group group);
  bool NearbyItemShown(NearbyItem const& item);
  bool DrawNearbyList(); // true if a setting changed
  bool DrawVistaList();  // the vistas' zones, each can be switched off (true if a setting changed); #if 0, not in the menu
  bool DrawDistrictList(); // the city's districts: Normal, Always or Never (true if a setting changed)
  void DrawMarkers(int hovered);
  void DrawCrosshair();

  // Camera moves (CAMERA > Around the focus, How the camera moves, Camera
  // shake, ...; see UpdateCamera)
  bool FindFocus(DirectX::XMVECTOR& point);   // what's followed, else Faith (her chest); w 0
  float OrbitBaseYaw();                       // the heading the circle turns with
  void OrbitFocus(DirectX::XMVECTOR focus, float pitch, float yaw, float speed, double dt, float heavyBlend,
    DirectX::XMVECTOR& position, DirectX::XMVECTOR& rotation, DirectX::XMMATRIX& rows);
  float SpeedByHeight(DirectX::XMVECTOR position);
  void UseBookmark(int index, bool save);
  void DrawTrackPath();
  // Its right-click menu (opened in DrawTrackPath), every frame the overlay
  // is drawn: an open popup ImGui isn't given takes every click, so it's
  // closed here when the path isn't shown (pathShown)
  void DrawTrackPathMenu(bool pathShown);
  // The drawn path's points dragged with the mouse (see DrawTrackPath): where
  // the one held goes for the cursor at x, y, from base in the plane facing
  // the camera, or straight up and down (height)
  bool PathDragPlace(DirectX::XMFLOAT3 const& base, bool height, float x, float y, DirectX::XMFLOAT3& place);
  // The same along any two ways (axisA, axisB; only axisA when twoAxes is
  // false): an orbit's arch handle in the upright plane through the focus
  bool PathDragAlong(DirectX::XMFLOAT3 const& base, DirectX::FXMVECTOR axisA, DirectX::FXMVECTOR axisB, bool twoAxes,
    float x, float y, DirectX::XMFLOAT3& place);
  bool PathDragPart(DirectX::XMFLOAT3& place);  // where the part held is now
  void EndPathDrag();
  // A keyframe's view drawn on the path: the camera's outline (its field of
  // view and the screen's shape) at where it is and looks, length metres
  // long in the world (colour: an ImU32)
  void DrawKeyframeView(ImDrawList* pDrawList, DirectX::XMFLOAT3 const& place, DirectX::XMFLOAT4 const& rotation,
    float fieldOfView, float length, unsigned int colour, float thickness, float aimLength);
  void DrawComposition(bool playing);
  // Faith for following and the camera moves: the replayed one while a
  // replay plays (m_FocusReplayedFaith), else where she is: at her feet, and
  // her heading (radians, as the soldier's atan2(row 2 x, z)) when known
  bool GetFaithPlace(DirectX::XMFLOAT3& place, float& heading, bool& hasHeading);
  bool DrawCameraMovesUI();   // true if a setting changed

  bool IsTimeFrozen();
  void ToggleTimeFreeze();
  void ApplyGameSpeed();
  float GameSpeedNow();   // the slider's, or a playing track's while it sets the game speed
  void ToggleHudVisibility();
  bool IsHudVisible();
  void SetLodFix(bool enable);
  void SetLodScale(float scale);
  void ApplyLodSettings();

private:
  std::mutex m_Mutex;

  igcs::capture::Session m_IgcsSession;
  // Input hooks query this without the camera mutex. All session/pose changes
  // themselves are serialized by m_Mutex.
  std::atomic<bool> m_IgcsSessionActive{ false };
  std::atomic<bool> m_IgcsConnected{ false };
  bool m_IgcsViewReady{ false };
  bool m_IgcsRestoreFrame{ false };

  std::atomic<bool> m_CameraEnabled;
  bool m_FirstEnable;
  bool m_AutoReset;

  bool m_GamepadDisabled;
  bool m_KbmDisabled;
  bool m_ThirdPersonToggle;
  bool m_ThirdPersonApplied;
  bool m_PlayerControlDuringPlayback{ true };
  bool m_LodFix;
  unsigned long long m_LastToggleTime{ 0 };
  bool m_LodSaved{ false };
  float m_LodOriginal[3]{ 0, 5.f, 5.f };
  float m_LodScale{ 0 };              // 0 = game default
  std::atomic<float> m_FarPlane{ 0 }; // 0 = game default, read from the camera hook

  float m_PlaybackFps; // 0 = play tracks in real time

  // Game speed and Freeze time (see ApplyGameSpeed): the game's time scale
  // is its own value times m_GameSpeed, or 0 while frozen. Not kept in the
  // config: every start is at 1x.
  float m_GameSpeed{ 1.f };
  bool m_TimeFrozen{ false };
  bool m_TimeScaleRead{ false };    // the game's value has been read once
  bool m_TimeScaleWritten{ false }; // the time scale is the tools' now
  float m_GameTimeScale{ 1.f };     // the game's own (1, or its slow motion)
  float m_WrittenTimeScale{ 1.f };
  float m_TrackGameSpeed{ -1.f };   // the playing track's keyframes' (-1: none), see GameSpeedNow
  bool m_ReplayAtGameSpeed{ true }; // a replay plays at the game speed (and holds while frozen)

  std::chrono::steady_clock::time_point m_LastFrame;
  bool m_HasLastFrame;

  // Addresses of the game's static pointers, resolved once
  __int64 m_ppGameRenderer;
  __int64 m_ppGameRenderSettings;
  __int64 m_ppGameTimeSettings;
  __int64 m_ppUISettings;
  __int64 m_ppClientGameContext;
  __int64 m_p3PToggle;
  __int64 m_ppMeshSettings;
  __int64 m_ppWorldRenderSettings;

  // Motion blur override (WorldRenderSettings), the strength is m_Camera.MotionBlur
  bool m_MotionBlurOverride{ false };
  bool m_MotionBlurEnable{ true };
  float m_MotionBlurMax{ 0.1f };
  bool m_MotionBlurSaved{ false };
  unsigned char m_MotionBlurOriginalEnable{ 1 };
  float m_MotionBlurOriginalScale{ 0.5f };
  float m_MotionBlurOriginalMax{ 0.1f };

  // Small object (screen area) culling override, WorldRenderSettings
  bool m_CullOverride{ false };
  float m_CullScale{ 1.f };
  bool m_CullSaved{ false };
  float m_CullOriginal{ 5.f };

  // Characters far away animated as smoothly as near ones, AntSettings
  bool m_SmoothAnimations{ true };
  bool m_AnimationSaved{ false };
  float m_AnimationOriginal{ 10.f };
  __int64 m_pAnimationSettings{ 0 };   // where the game's value was saved from

  // Sounds heard from the free camera (the sound listener's hooks). The camera
  // hook publishes where the listener goes; the hooks read it under a spin
  // lock held only for a copy.
  void PublishListener(bool active, double dt);
  // The Faith sound log (a diagnostic in util::hooks): fed from here
  void UpdateFaithSoundLog();
  bool m_SoundLogWasOn{ false };
  // The world log (a diagnostic in util::hooks), and its look at the
  // characters near Faith when it starts and ends
  void UpdateWorldLog();
  void LogCharacterPoses(float const* faith, const char* when);
  bool m_WorldLogWasOn{ false };
  struct ListenerPose
  {
    float Transform[16];  // the camera's: rows right, up, backward, position
    float Velocity[4];
    bool Active;
  };
  bool m_SoundAtCamera{ true };
  bool m_SpotSounds{ false };  // test: the level's spot sounds start near the camera too
  __int64 m_ppListenerSpace{ 0 };  // set: the game puts its listener in another space, left alone then
  ListenerPose m_ListenerPose{};
  std::atomic_flag m_ListenerLock = ATOMIC_FLAG_INIT;
  std::atomic<int> m_ListenerStillCalls{ 0 }; // velocity 0 for these after it ends: the game's own is the jump back
  std::atomic<int> m_ListenerOtherSpace{ 0 }; // listener calls left alone: it was in another space
  std::atomic<bool> m_ListenerOverridden{ false }; // the last transform given was the free camera's
  int m_ListenerOtherSpaceLogged{ 0 };
  bool m_ListenerWasActive{ false };
  DirectX::XMFLOAT3 m_ListenerLastPosition{};

  // Billboard quality, LinearMediaBillboardSettings: 0 the game's own, then
  // Sharper, Much sharper (see BillboardLevels in the .cpp)
  int m_BillboardQuality{ 2 };
  int m_BillboardLoggedQuality{ -1 };
  bool m_BillboardSaved{ false };
  int m_BillboardSkipOriginal{ 0 };
  float m_BillboardScaleOriginal{ 1.f };
  __int64 m_pBillboardSettings{ 0 };

  // Sun shadow distance override, WorldRenderSettings ShadowmapViewDistance
  float m_ShadowDistance{ 0.f }; // 0 = the game's own (200 m)
  bool m_ShadowSaved{ false };
  float m_ShadowOriginal{ 200.f };

  // The sun shadow's 4 layers (cascades): the fade between them and how
  // they're spread over the shadow distance
  bool m_ShadowLayersOverride{ false };
  float m_ShadowLayerFade{ 0.03f };   // ShadowmapTransitionBlendAmount, game 0.03
  float m_ShadowLayerSpread{ 0.88f }; // ShadowmapSliceSchemeWeight, game 0.88
  bool m_ShadowLayersSaved{ false };
  float m_ShadowLayerFadeOriginal{ 0.03f };
  float m_ShadowLayerSpreadOriginal{ 0.88f };

  // Zone streaming around the free camera, read from the zone streamer's thread
  std::atomic<bool> m_StreamAroundCamera{ false };
  std::atomic<bool> m_StreamingFocusReady{ false }; // the camera position was published since enabling
  bool m_StreamAhead{ true };  // also load where the camera is heading
  bool m_StreamTrack{ true };  // also load along the selected track
  bool m_StreamInView{ false }; // the extra zones only where the camera looks
  bool m_StreamUnrestricted{ false }; // past the renderer's limits (draw batch, instance memory): can crash
  std::atomic<float> m_BatchLimit{ 0.80f }; // draw batch fill the camera's extra zones are held back from (fixed, BatchLimitFixed)
  bool m_BatchLimitEditing{ false };
  std::atomic<bool> m_BatchOverflowed{ false }; // a draw batch ran past its room this level
  std::atomic<int> m_BatchMostChunks{ 0 };
  std::atomic<int> m_RendererInstances{ 0 }; // the fullest renderer's instances, all its batches
  std::atomic<int> m_RendererRoom{ 0 };      // its instance list's room
  float m_StreamDistance{ 0.f }; // also load this far around the camera, 0 = its own and neighbouring zones
  // The camera's streaming points around it, on the corners of a fixed
  // 200 m grid (see PublishStreamingFocus): the corner each ring focus is on
  struct GridPoint
  {
    bool Used;
    int X, Z;   // the corner, in 200 m steps
  };
  static const int MaxGridPoints = 56;
  GridPoint m_GridPoints[MaxGridPoints]{};
  // Vistas, the far-away stand-ins of the city's buildings (see UpdateVistas),
  // read from the zone streamer's thread: all shown or hidden, and each
  // zone's hidden while its own buildings are loaded
  std::atomic<bool> m_VistasShown{ true };
  std::atomic<bool> m_VistaHideLoaded{ true };
  // The Shard and the Bauble as before the story (util::hooks::UpdateLandmarkTriggers)
  std::atomic<bool> m_UnbrokenLandmarks{ false };
  // GAME > Districts: the real Shard (its tower's zone and its top) streamed in
  // from anywhere, instead of the far-away model the city shows
  std::atomic<bool> m_StreamRealShard{ false };
  // Kruger's office (the Shard's top): 1 blinds open, 2 closed (0, as the game has it, no longer chosen)
  std::atomic<int> m_KrugerBlinds{ 1 };
  // The lair's closed blinds hidden, and the Shard station's gates and windows (UpdateModelHide).
  // The station's has no tick or config key since 2026-10-27: always off.
  std::atomic<bool> m_OpenLairBlinds{ false };
  std::atomic<bool> m_OpenShardStation{ false };
  // The lair's two windows that open, turned shut and solid (UpdateLairWindows)
  std::atomic<bool> m_CloseLairWindows{ false };
  // GAME > Street level (ground::, GroundLevel.cpp): an invisible floor under Faith at the street's height, the
  // street kill floors sparing her while it holds her; and no death from long falls
  std::atomic<bool> m_SolidStreet{ false };
  std::atomic<bool> m_NoFallDeath{ false };
  // For the menu, from the zone streamer hook: its fullest zone slot count since the camera was
  // turned on, and when it last held the camera's extra zones back
  std::atomic<int> m_StreamSlotsPeak{ 0 };
  std::atomic<unsigned long long> m_StreamLimitedTick{ 0 };

  // The mesh streaming manager's model instance lists (see WatchInstanceLists)
  __int64 m_ppMeshStreaming{ 0 };
  std::atomic<int> m_InstancePressure{ 0 };  // 0 fine, 1 hold back, 2 take zones away, 3 take all away
  std::atomic<float> m_InstanceFill{ -1.f }; // the fuller list, 0 - 1, -1 = unknown
  int m_InstanceCount[2]{};               // up to each list's end
  int m_InstanceInUse[2]{};               // without the free slots (what counts)
  std::atomic<int> m_MeshPressure{ 0 };   // the parts of m_InstancePressure (with the two below), for the menu and log
  std::atomic<int> m_ListPressure{ 0 };
  std::atomic<int> m_ArenaPressure{ 0 };          // the renderer's instance arena (see RenderArenaRva)
  std::atomic<long long> m_ArenaUsed{ 0 };
  std::atomic<long long> m_ArenaSize{ 0 };
  int m_ArenaPressureLogged{ 0 };
  std::atomic<int> m_BatchPressure{ 0 };          // the renderer's fullest draw batch (see RendererOwnersRva)
  // The fullest category for its own room (CategoryRoom: category 0's
  // spread, the others one batch): its instances, its room, how full
  std::atomic<int> m_BatchInstances{ 0 };
  std::atomic<int> m_BatchRoom{ 111872 };
  std::atomic<float> m_BatchFill{ 0.f };
  std::atomic<int> m_BatchChunks{ 0 };
  int m_BatchArriving{ 0 };                       // the most instances in any one category (see BatchBurst)
  float m_BatchCrashFill{ 0.f };                  // the fullest single batch against its chunk pointers' end
  int m_BatchPressureLogged{ 0 };
  // The log at the limit: when the last warning or summary was written (0:
  // none since it was last quiet), and what happened since (see
  // WatchInstanceLists)
  unsigned long long m_BatchLogTick{ 0 };
  int m_BatchLogRises{ 0 };
  int m_BatchLogHighest{ 0 };
  float m_BatchLogLowest{ 0.f };
  float m_BatchLogFullest{ 0.f };
  int m_BatchLogTakeAways{ 0 };
  unsigned long long m_BatchScanTick{ 0 };
  // GAME > Level streaming > Room for the whole city (the renderer's draw
  // batches spread, [Misc] BatchSpreadChunks): what was chosen (0 off, else
  // chunks; switched only with no renderer there or its first draw batch
  // well short of it, see WatchInstanceLists), its chunks when ticked on,
  // whether it can't be spread (no wider handle, or the add isn't hooked: it
  // stays off), and whether the config has been read (the scan doesn't
  // switch before; with m_Mutex)
  std::atomic<int> m_BatchSpreadWanted{ 1700 };
  int m_BatchSpreadOn{ 1700 };
  std::atomic<bool> m_BatchSpreadMissing{ false };
  bool m_BatchSpreadConfigRead{ false };
  // The renderer's instance list is fuller (for its room) than the fullest
  // category of draw batches: with the batches spread it's the limit that's
  // reached first, and what the menu and the log name
  std::atomic<bool> m_BatchListFuller{ false };
  // At the last scan, for its guess at a level load (the renderer objects,
  // and category 0's first batch's chunks and batches in use: the research
  // only saw them grow while the renderer lives)
  __int64 m_BatchRenderers[2]{};
  int m_BatchFirstChunks{ 0 };
  int m_BatchFirstBatches{ 0 };
  int m_InstanceCapacity[2]{};
  __int64 m_InstanceBegin[2]{};
  int m_InstanceReserveOriginal[2]{};
  int m_InstanceReserveAsks[2]{};      // how often it was asked for, only the first two are logged
  std::atomic<int> m_MeshCount{ 0 };   // mesh records in use, the game has room for 4096
  std::atomic<int> m_MeshPeak{ 0 };
  std::atomic<int> m_MeshHighWater{ 0 }; // where the table's end is (the most there were)
  int m_MeshInUse{ 0 };                    // records in use at the last count
  int m_MeshScanHighWater{ 0 };            // the end at the last count
  unsigned long long m_MeshScanTick{ 0 };
  bool m_MeshesOverflowed{ false };  // the mesh table's end went past its 4096 records
  DirectX::XMFLOAT3 m_StreamLastPosition{ 0, 0, 0 };
  DirectX::XMFLOAT3 m_StreamVelocity{ 0, 0, 0 };
  bool m_StreamHasLast{ false };
  __int64 m_pAddZoneFocus;
  __int64 m_pRemoveZoneFocus;
  __int64 m_pZoneStep{ 0 };   // a streamer's release / load step, for the districts set to Never
  bool m_StreamPausedForFaith{ true }; // zone streamer thread: Faith not spawned (or back for less than a few seconds)
  unsigned long long m_FaithBackAt{ 0 };

  bool m_TrackTimeOfDayApplied{ false }; // A playing track is setting the clock

  // Faith's body switch: the player entity it was done on and the camera
  // link that was there, so exactly that is put back
  __int64 m_ThirdPersonEntity{ 0 };
  __int64 m_ThirdPersonLink{ 0 };
  // Her own camera is unlinked this long after the switch, so her hair can come on first
  static const unsigned long long ThirdPersonUnlinkDelayMs = 500;
  unsigned long long m_ThirdPersonUnlinkAt{ 0 };  // 0: nothing waiting
  // How her body is shown (CAMERA > Show Faith's body > Her body; config
  // ThirdPersonBodyWay): 0 the game's third-person body (her view mode says
  // third person), 1 her whole first-person body (the original tools' way).
  // m_BodyWayApplied: the way that's on now (-1: none). m_BodySoldier: whose
  // view mode says third person (0: nobody's). Under m_BodyLock (the camera
  // is turned on and off from the menu and the hotkeys, kept on a new Faith
  // from the camera hook)
  int m_BodyWay{ 0 };
  int m_BodyWayApplied{ -1 };
  __int64 m_BodySoldier{ 0 };
  bool m_BodyWayWarned{ false };
  std::recursive_mutex m_BodyLock;

  // Faith's hair cloth while her body is shown (see ApplyHairFix)
  static const int MaxHairCloths = 4;
  __int64 m_HairData[MaxHairCloths]{};  // ClothEntityData changed, and its 8 original floats
  float m_HairSaved[MaxHairCloths][8]{};
  __int64 m_HairMesh[MaxHairCloths]{};  // and its mesh asset with its original LodScale
  float m_HairLodScale[MaxHairCloths]{};
  int m_HairSavedCount{ 0 };
  __int64 m_HairInstance[MaxHairCloths]{};   // each cloth's own instance, with its own radius and fade distance
  float m_HairInstanceSaved[MaxHairCloths][2]{};
  int m_HairInstanceCount{ 0 };

  // Where the free camera was when it was turned off (Start at the game camera off)
  DirectX::XMFLOAT3 m_LastCameraPosition{ 0, 0, 0 };
  DirectX::XMFLOAT4 m_LastCameraRotation{ 0, 0, 0, 1 };

  // Following Faith: her movement since the last frame moves the free camera,
  // and her movement since a track started moves the track
  bool m_AttachToFaith{ false };
  bool m_AttachRotation{ true };   // turn with her heading too
  bool m_AttachHasLast{ false };
  bool m_AttachValid{ false };     // m_AttachNow is this frame's
  bool m_AttachTrackActive{ false };
  DirectX::XMFLOAT4X4 m_AttachLast{};
  DirectX::XMFLOAT4X4 m_AttachNow{};
  DirectX::XMFLOAT4X4 m_AttachTrackStart{};

  // Following another object (character, traffic, drone, train, aircraft,
  // security camera, moving model) the same way. Picks are asked for here and done on the game's thread: what's
  // in view is watched for a few frames (m_AttachScan), then the best is taken.
  attach::Target m_AttachTarget{};
  bool m_AttachObject{ false };   // following m_AttachTarget
  bool m_AttachTilt{ false };     // all of its rotation, not just its heading
  float m_AttachSmoothing{ 0.4f }; // 0 = none, 1 = the softest (see FollowAttachTarget)
  DirectX::XMFLOAT3 m_AttachRawPosition{};    // last frame's, for telling a jump
  // The copy the camera follows (see FollowAttachTarget): its place and
  // turn, how fast it moves and turns (m/s, rad/s about a world axis), the
  // object's own speed and turning smoothed, the object's place and turn
  // last frame, and how many of the object's frames it has (0: start
  // afresh, 1: its speed not known yet)
  DirectX::XMFLOAT3 m_AttachSmoothPosition{};
  DirectX::XMFLOAT4 m_AttachSmoothRotation{ 0, 0, 0, 1 };
  DirectX::XMFLOAT3 m_AttachSmoothVelocity{};
  DirectX::XMFLOAT3 m_AttachSmoothSpin{};
  DirectX::XMFLOAT3 m_AttachObjectVelocity{};
  DirectX::XMFLOAT3 m_AttachObjectSpin{};
  DirectX::XMFLOAT3 m_AttachObjectLastPosition{};
  DirectX::XMFLOAT4 m_AttachObjectLastRotation{ 0, 0, 0, 1 };
  int m_AttachSmoothFrames{ 0 };
  std::unique_ptr<attach::Scan> m_AttachScan;
  AttachRequest m_AttachPickRequest{ AttachRequest_None };
  bool m_AttachPicking{ false };  // a pick is watching what's in view
  double m_AttachPickRemaining{ 0 }; // seconds before it's decided
  bool m_ShowCrosshair{ false };
  AttachRequest m_AttachRequest{ AttachRequest_None };
  bool m_AttachMoveCamera{ true };      // put the camera next to what's picked
  bool m_AttachPlaceRequested{ false }; // "Go next to it"
  attach::Target m_AttachListRequest{}; // clicked in the list, attached on the game's thread
  bool m_AttachListRequested{ false };
  float m_AttachDistance{ 0 };
  char m_AttachMessage[192]{};    // why the last pick or follow ended, for the menu

  // What's around the camera, for the FOLLOW tab: looked at once a second
  // while the tab is open, the rows changed every 30 seconds (m_NearbyPending
  // is the newest), positions read every frame for the distances and markers
  std::vector<NearbyItem> m_Nearby;            // nearest first when made, what the menu shows
  std::vector<int> m_NearbyOrder;              // m_Nearby's rows as shown: nearest the camera now first
  std::vector<NearbyItem> m_NearbyPending;
  int m_NearbyPendingTotal{ 0 };
  int m_NearbyPendingFar{ 0 };
  int m_NearbyPendingDropped{ 0 };
  int m_NearbyDropped{ 0 };                    // within range but beyond the scan buffer
  bool m_NearbyHasPending{ false };
  unsigned long long m_NearbyLastPublish{ 0 };
  bool m_NearbyPublishRequested{ false };      // Refresh, or the range changed
  bool m_NearbyRescan{ false };
  bool m_NearbyListHovered{ false };           // the mouse is over the list: keep the rows
  bool m_NearbyRadiusChanged{ false };
  float m_NearbyListedRadius{ 150.f };
  std::vector<attach::Target> m_NearbyFound;   // scan buffer
  std::unique_ptr<attach::Scan> m_NearbyStats;
  std::unordered_map<unsigned long long, NearbySeen> m_NearbySeen; // to tell what moves
  unsigned long long m_NearbyWantedUntil{ 0 }; // the tab was drawn recently
  unsigned long long m_NearbyLastScan{ 0 };
  int m_NearbyTotal{ 0 };                      // within range, of what's ticked
  int m_NearbyFar{ 0 };                        // trains and aircraft further away
  float m_NearbyRadius{ 150.f };
  // Which groups are listed (attach::Group)
  bool m_NearbyPeople{ true };
  bool m_NearbyCars{ true };
  bool m_NearbyDrones{ true };
  bool m_NearbyBoats{ true };
  bool m_NearbyTrains{ true };
  bool m_NearbyAircraft{ true };
  bool m_NearbyCameras{ true };
  bool m_NearbyOther{ true };
  bool m_NearbyMovingOnly{ false };
  bool m_NearbyMarkers{ true };

  // The game's view, for drawing markers: vertical field of view (radians)
  // and aspect ratio, from the main render view (+0xE4, +0xF4)
  float m_ViewFovY{ 1.0472f };
  float m_ViewAspect{ 1.7778f };

  // Camera moves (CAMERA > Around the focus and below), all off by default: circling or
  // aiming at the focus (what's followed, else Faith), a heavy camera, flying
  // level, speed by height, a hand-held shake, auto-focus and dolly zoom on
  // the focus, bookmarks, the track's path on screen and the Panini
  // projection. Orbit, aim and dolly zoom aren't kept in the config.
  bool m_Orbit{ false };
  bool m_LookAt{ false };
  float m_Heaviness{ 0.f };        // 0 - 1: how long moving and turning take to get going and to stop (see HeavyMaxSeconds)
  bool m_FlyLevel{ false };        // forward, back and sideways along the ground; up and down straight up
  bool m_Collision{ false };       // flying freely, it stops at walls (see collision::Move)
  float m_CollisionRadius{ 0.25f };// metres: the ball it's swept as
  // A playing track kept out of walls too: slid along them, and held at one
  // it goes right through until it comes out, then cut to it
  bool m_TrackCollision{ true };
  bool m_GuardValid{ false };
  DirectX::XMFLOAT3 m_GuardPlace{};      // where the camera was put last frame
  DirectX::XMFLOAT3 m_GuardLastTrack{};  // where the track was last frame
  double m_GuardHeld{ 0.0 };             // seconds held off the track by a wall
  // The selected track's path checked for walls (see StepTrackWallCheck):
  // the keyframes and radius it was checked with, how far it got, and where
  // it goes through something solid (times, and the stretches to draw red)
  std::vector<CatmullRomNode> m_WallCheckNodes;
  float m_WallCheckRadius{ 0.f };
  bool m_WallCheckRunning{ false };
  double m_WallCheckTime{ 0.0 };
  bool m_WallCheckHasLast{ false };
  DirectX::XMFLOAT3 m_WallCheckLast{};
  double m_WallCheckLastTime{ 0.0 };
  struct WallSpan { double From, To; };
  std::vector<WallSpan> m_WallSpans;
  struct WallStretch { DirectX::XMFLOAT3 A, B; };
  std::vector<WallStretch> m_WallStretches;
  bool m_WallFixWanted{ false };
  bool m_WallCheckLogOwed{ false };  // a check finished: its result goes in the log once the keyframes aren't being dragged
  int m_WallFixPasses{ 0 };
  int m_WallFixAdded{ 0 };
  float m_SpeedByHeight{ 0.f };    // 0 - 1: how much the height above Faith sets the movement speed
  float m_ShakeStrength{ 0.f };    // 0 - 1
  float m_ShakeSpeed{ 1.f };       // 0.25 - 3
  bool m_AutoFocus{ false };       // the depth of field override's distances follow the focus (VisualsManager::SetAutoFocus)
  bool m_DollyZoom{ false };
  bool m_ShowTrackPath{ false };
  bool m_ShowKeyframeViews{ true };   // with the path: where each keyframe looks (DrawKeyframeView)
  // Their length in the world, metres, the same for every keyframe whatever
  // moves (2026-11-09: sized from the keyframes' spacing, the user saw one
  // "get bigger with every move of the node"; at 2 m they were "still huge")
  float m_KeyframeViewSize{ 0.3f };
  // The track previewed on its drawn path without moving the camera (CAMERA >
  // Preview the track on its path): its clock (seconds of track time; real
  // time at the playback speed, round and round with PreviewEndWait at the
  // end), when it last moved on, how long it's waited at the end
  bool m_PathPreview{ false };
  double m_PreviewTime{ 0.0 };
  std::chrono::steady_clock::time_point m_PreviewLast{};
  bool m_PreviewClockOn{ false };
  double m_PreviewWaited{ 0.0 };
  // A drag on the drawn path (render thread, under m_Mutex): the keyframe and
  // its part held (0 its place, 1 its handle in, 2 its handle out, 3 the
  // arch of the orbit from it to the next), where that was when the drag (or
  // Shift's height drag) began, and the cursor's offset from it on screen
  // then; an orbit keyframe's place is moved round the focus (m_PathDragOrbit)
  bool m_PathDragActive{ false };
  int m_PathDragKeyframe{ -1 };
  int m_PathDragPart{ 0 };
  bool m_PathDragOrbit{ false };
  // The drawn path's right-click menu: for that keyframe (its part as above)
  int m_PathMenuKeyframe{ -1 };
  int m_PathMenuPart{ 0 };
  bool m_PathDragHeight{ false };
  DirectX::XMFLOAT3 m_PathDragBase{};
  float m_PathDragOffsetX{ 0.f }, m_PathDragOffsetY{ 0.f };
  // The cursor and the held point's place on screen as the drag last saw
  // them: when that place moved by itself since (the camera moved, turned or
  // zoomed, the focus moved), the held point is taken again from where it is
  // now, so it stays put in the world and only the mouse moves it
  float m_PathDragMouseX{ 0.f }, m_PathDragMouseY{ 0.f };
  bool m_PathDragHeldSeen{ false };
  float m_PathDragHeldX{ 0.f }, m_PathDragHeldY{ 0.f };
  bool m_Panini{ false };
  float m_PaniniDistance{ 1.f };   // 0 - 1
  float m_PaniniCrop{ 1.f };       // 0 - 1
  bool m_FocusReplayedFaith{ true }; // while a replay plays, Faith (to follow, circle, aim at) is the replayed one
  int m_ShakePreset{ 0 };          // ShakeProfiles
  float m_ImpactStrength{ 0.5f };  // a one-off jolt (the Impact button)
  double m_ImpactAge{ -1.0 };      // seconds since it started (-1: none)
  // Composition guides (CAMERA > Letterbox and guide lines)
  int m_Letterbox{ 0 };            // LetterboxRatios (0: off)
  float m_LetterboxOpacity{ 1.f };
  bool m_GuideThirds{ false };
  bool m_GuideGolden{ false };
  bool m_GuideCenter{ false };
  bool m_GuideSafe{ false };
  bool m_GuidesWhilePlaying{ false };
  struct CameraBookmark
  {
    bool Set;
    DirectX::XMFLOAT3 Position;
    DirectX::XMFLOAT4 Rotation;
    float FieldOfView;
  };
  static const int BookmarkCount = 9;
  CameraBookmark m_Bookmarks[BookmarkCount]{};
  // Their state (the camera hook's)
  bool m_FocusOk{ false };         // the focus was found this frame
  DirectX::XMFLOAT3 m_FocusPoint{};  // where, this frame (for the track's path)
  float m_FocusDistance{ 0.f };    // from the camera, this frame
  bool m_OrbitActive{ false };     // circling it this frame (its angles are set)
  float m_OrbitYaw{ 0.f };         // around it, from the heading it turns with (see OrbitBaseYaw)
  float m_OrbitPitch{ 0.f };       // up from level
  float m_OrbitDistance{ 5.f };
  float m_OrbitMove[3]{};          // the movement keys while circling: m/s round, up, away
  float m_OrbitWheel{ 0.f };       // wheel notches since the last frame, while circling
  DirectX::XMFLOAT3 m_Velocity{ 0, 0, 0 };   // the heavy camera's, world space
  float m_TurnRates[3]{};          // the heavy camera's pitch, yaw and roll, radians a second
  bool m_DollyActive{ false };
  float m_DollyWidth{ 0.f };       // distance x tan(field of view / 2), kept while it's on
  float m_DollyFov{ 0.f };         // the field of view it set last
  double m_ShakeTime{ 0.0 };
  float m_SpeedFactor{ 1.f };      // this frame's (for the menu)

  Camera m_Camera;
  TrackPlayer m_TrackPlayer;
  FaithReplay m_Replay;

public:
  CameraManager(CameraManager const&) = delete;
  void operator=(CameraManager const&) = delete;
};
