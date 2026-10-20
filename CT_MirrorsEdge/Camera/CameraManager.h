#pragma once
#include "TrackPlayer.h"
#include "AttachTargets.h"
#include "../inih/cpp/INIReader.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

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
  // Render thread, from the Present hook
  void DrawUI();
  // The FOLLOW tab: following Faith or another object
  void DrawFollowUI();
  // One line under the menu tabs: camera, track and time state
  void DrawStatusBar();
  // A line about what the tools are doing, for the crash note
  void DescribeState(char* buffer, size_t size);

  bool IsCameraEnabled() { return m_CameraEnabled; }
  // While a track plays Faith can be given the controls back
  bool IsPlayerControllingFaith() { return m_PlayerControlDuringPlayback && m_TrackPlayer.IsPlaying(); }
  bool IsGamepadDisabled() { return m_CameraEnabled && m_GamepadDisabled && !IsPlayerControllingFaith(); };
  bool IsKbmDisabled() { return m_CameraEnabled && m_KbmDisabled && !IsPlayerControllingFaith(); };
  // Following Faith while she isn't frozen: the movement keys only move her,
  // so the camera keeps its place next to her instead of flying off as well
  bool IsMovementKeysForFaith() { return m_CameraEnabled && m_AttachToFaith && !m_KbmDisabled; }

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
  void UpdateInput(double dt, DirectX::XMFLOAT3 const& mouseState);

  void ToggleCamera();
  void MoveToPlayer();

  bool SeedFromGameCamera();
  void WriteToGameCamera(__int64 pRenderView);
  void ApplyFieldOfView(float fov);
  void ApplyMotionBlur();
  void ApplyCulling();
  void WatchInstanceLists();
  void ApplyHairFix();
  void UnlinkFaithCamera();
  void ApplyShadowDistance();
  void PublishStreamingFocus(double dt);
  void SetThirdPersonBody(bool enable);
  __int64* GetPlayerEntityPair();
  bool GetPlayerTransform(DirectX::XMFLOAT4X4A& transform);
  __int64 GetPlayerSoldier();
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
  bool NearbyItemShown(NearbyItem const& item);
  bool DrawNearbyList(); // true if a setting changed
  void DrawMarkers(int hovered);
  void DrawCrosshair();

  bool IsTimeFrozen();
  void ToggleTimeFreeze();
  void ToggleHudVisibility();
  bool IsHudVisible();
  void SetLodFix(bool enable);
  void SetLodScale(float scale);
  void ApplyLodSettings();

private:
  std::mutex m_Mutex;

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
  float m_StreamDistance{ 0.f }; // also load this far around the camera, 0 = its own and neighbouring zones
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
  std::atomic<int> m_BatchInstances{ 0 };
  std::atomic<int> m_BatchChunks{ 0 };
  int m_BatchPressureLogged{ 0 };
  unsigned long long m_BatchScanTick{ 0 };
  int m_InstanceCapacity[2]{};
  __int64 m_InstanceBegin[2]{};
  int m_InstanceReserveOriginal[2]{};
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

  bool m_TrackTimeOfDayApplied{ false }; // A playing track is setting the clock

  // Faith's body switch: the player entity it was done on and the camera
  // link that was there, so exactly that is put back
  __int64 m_ThirdPersonEntity{ 0 };
  __int64 m_ThirdPersonLink{ 0 };
  // Her own camera is unlinked this long after the switch, so her hair can come on first
  static const unsigned long long ThirdPersonUnlinkDelayMs = 500;
  unsigned long long m_ThirdPersonUnlinkAt{ 0 };  // 0: nothing waiting

  // Faith's hair cloth while her body is shown (see ApplyHairFix)
  static const int MaxHairCloths = 4;
  __int64 m_HairData[MaxHairCloths]{};  // ClothEntityData changed, and its 8 original floats
  float m_HairSaved[MaxHairCloths][8]{};
  __int64 m_HairMesh[MaxHairCloths]{};  // and its mesh asset with its original LodScale
  float m_HairLodScale[MaxHairCloths]{};
  int m_HairSavedCount{ 0 };

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

  // Following another object (character, traffic, drone, moving model) the
  // same way. Picks are asked for here and done on the game's thread: what's
  // in view is watched for a few frames (m_AttachScan), then the best is taken.
  attach::Target m_AttachTarget{};
  bool m_AttachObject{ false };   // following m_AttachTarget
  bool m_AttachTilt{ false };     // all of its rotation, not just its heading
  float m_AttachSmoothing{ 0.4f }; // 0 = none, 1 = the camera catches up over about half a second
  DirectX::XMFLOAT3 m_AttachRawPosition{};    // last frame's, for telling a jump
  DirectX::XMFLOAT3 m_AttachSmoothPosition{};
  DirectX::XMFLOAT4 m_AttachSmoothRotation{ 0, 0, 0, 1 };
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
  std::vector<NearbyItem> m_Nearby;            // nearest first, what the menu shows
  std::vector<NearbyItem> m_NearbyPending;
  int m_NearbyPendingTotal{ 0 };
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
  int m_NearbyTotal{ 0 };
  float m_NearbyRadius{ 150.f };
  bool m_NearbyPeople{ true };
  bool m_NearbyTraffic{ true };
  bool m_NearbyOther{ true };
  bool m_NearbyMovingOnly{ false };
  bool m_NearbyMarkers{ true };

  // The game's view, for drawing markers: vertical field of view (radians)
  // and aspect ratio, from the main render view (+0xE4, +0xF4)
  float m_ViewFovY{ 1.0472f };
  float m_ViewAspect{ 1.7778f };

  Camera m_Camera;
  TrackPlayer m_TrackPlayer;

public:
  CameraManager(CameraManager const&) = delete;
  void operator=(CameraManager const&) = delete;
};
