#include "CameraManager.h"
#include "../Main.h"
#include "../Util/Util.h"
#include "../Util/GameState.h"
#include "../Util/Perf.h"
#include "../imgui/imgui.h"
#include "../Util/ImGuiEXT.h"
#include "../Visuals/Panini.h"
#include "../Visuals/GreenPlane.h"
#include "../Visuals/Weather.h"
#include "../Visuals/WetLook.h"
#include "../Visuals/DryShapes.h"
#include "CameraCollision.h"
#include "DogenDoor.h"
#include "FaithOutfit.h"
#include "FaithSize.h"
#include "PoseEditor.h"
#include "GroundLevel.h"
#include "CullingOff.h"
#include "Bodycam.h"
#include "Timelapse.h"
#include "../Visuals/FrameShot.h"
#include <Windows.h>
#include <algorithm>
#include <cmath>
#include <cctype>
#include <cfloat>
#include <cstring>
#include <unordered_set>

using namespace DirectX;

namespace
{
  // Mirror's Edge Catalyst structure offsets
  const int RenderViewOffset     = 0x58; // GameRenderer -> RenderView* (the main view)
  const int RenderViewTransform0 = 0x00; // LinearTransform
  const int RenderViewTransform1 = 0x40; // LinearTransform, +0x70 is the camera position
  const int ForceFovOffset       = 0x54; // GameRenderSettings, float degrees, -1 = no override
  const int TimeScaleOffset      = 0x48; // GameTimeSettings, float
  // Game speed (GAME > Game speed). Above 1x the game simulates at most
  // one 30 Hz tick a frame in single player (ForceSinglePlayerFixedTick), so
  // it goes at most frame rate / 30 times as fast
  // (Research_GameSpeed_2026-10-25.md)
  // (GameSpeedMin / GameSpeedMax: CameraStructs.h, keyframes have them too)
  const float GameSimRate        = 30.f;
  const int DrawHudOffset        = 0x48; // UISettings, bool
  const float FaithEyeHeight     = 1.6f;  // Meters above the soldier transform
  // Camera moves (CAMERA > Around the focus, How the camera moves, Camera
  // shake, ...; see UpdateCamera)
  const float FaithFocusHeight = 1.3f;     // what orbit, aim, auto-focus and dolly zoom go by on her: her chest
  const float HeavyMaxSeconds = 1.f;       // the heaviest camera takes about this long to get going or to stop
  const float OrbitMinDistance = 0.3f;
  const float OrbitMaxDistance = 5000.f;
  const float OrbitMaxPitch = 1.4835f;     // 85 degrees up or down
  const float OrbitWheelStep = 0.85f;      // each wheel notch towards it: this much of the distance
  // Finding the way round between orbit keyframes (CameraManager::FindOrbitWay):
  // a track that goes less than this round the focus doesn't say which way
  // (the shorter way then), and the most points an arc is checked at
  const float OrbitWayMinSweep = 0.1745f;  // 10 degrees
  const int OrbitArcMaxPoints = 65;
  const float SpeedHeightReference = 20.f; // speed by height: the movement speed as set this high above Faith
  const float SpeedHeightMin = 0.25f;      // and at most this slow near her, this fast high up
  const float SpeedHeightMax = 25.f;
  const int MaxPathPoints = 4096;          // the track's path on screen
  // A playing track kept out of walls (see GuardTrackPlace): a jump of more
  // than GuardJump in a frame is a cut or a seek (put there at once); held off
  // the track by a wall for GuardHoldSeconds while the track's own way is
  // clear (it came out the other side), the camera cuts to it
  const float GuardJump = 10.f;
  const double GuardHoldSeconds = 0.75;
  // The track's path checked for walls (see StepTrackWallCheck): sampled
  // every WallCheckStep seconds, swept from one sample to the next once
  // WallCheckSpacing metres apart, WallCheckSweepsPerFrame a frame at most; two
  // samples more than WallCheckCut apart are a cut (a Hold keyframe)
  const double WallCheckStep = 0.02;
  const float WallCheckSpacing = 0.75f;
  const int WallCheckSweepsPerFrame = 64;
  const int WallCheckSamplesPerFrame = 6000;
  const float WallCheckCut = 25.f;
  const double WallSpanJoin = 0.3;         // seconds between two places that are one
  const int MaxWallFixPasses = 6;
  const int PathPointsPerSegment = 24;

  // The shake's kinds: at full strength, how much pitch and yaw, roll
  // (radians), sideways and up and down (metres), and three wave frequencies
  // (Hz) for each of those five channels, the first the strongest. Hand-held
  // is the first shake's (the same as before there were kinds); walking and
  // running bob at a step's pace, a vehicle buzzes, an earthquake shakes hard.
  struct ShakeProfile
  {
    const char* Name;
    float Angle, Roll, Sideways, UpDown;
    float Frequencies[5][3];
  };
  const ShakeProfile ShakeProfiles[] = {
    { "Hand-held", 0.0140f, 0.0070f, 0.015f, 0.015f,
      { { 0.37f, 0.83f, 1.91f }, { 0.29f, 0.71f, 1.63f }, { 0.23f, 0.59f, 1.37f }, { 0.43f, 0.97f, 2.11f }, { 0.31f, 0.89f, 1.79f } } },
    { "Walking", 0.0090f, 0.0080f, 0.012f, 0.025f,
      { { 1.90f, 3.80f, 0.47f }, { 0.95f, 1.90f, 0.31f }, { 0.95f, 1.90f, 0.23f }, { 0.95f, 1.90f, 0.41f }, { 1.90f, 3.80f, 0.53f } } },
    { "Running", 0.0180f, 0.0140f, 0.020f, 0.050f,
      { { 2.80f, 5.60f, 0.61f }, { 1.40f, 2.80f, 0.43f }, { 1.40f, 2.80f, 0.37f }, { 1.40f, 2.80f, 0.53f }, { 2.80f, 5.60f, 0.71f } } },
    { "Vehicle", 0.0050f, 0.0040f, 0.006f, 0.010f,
      { { 9.30f, 14.7f, 0.61f }, { 8.10f, 13.3f, 0.47f }, { 7.70f, 12.9f, 0.53f }, { 10.1f, 15.3f, 0.41f }, { 11.3f, 17.1f, 0.67f } } },
    { "Earthquake", 0.0300f, 0.0250f, 0.060f, 0.080f,
      { { 3.10f, 4.70f, 1.30f }, { 2.90f, 5.30f, 1.10f }, { 3.70f, 6.10f, 0.90f }, { 4.30f, 6.70f, 1.70f }, { 3.30f, 5.90f, 1.90f } } },
  };
  const int ShakeProfileCount = sizeof(ShakeProfiles) / sizeof(ShakeProfiles[0]);
  // The Impact button: a jolt at these frequencies, fading away over ImpactSeconds
  const ShakeProfile ImpactProfile = { "Impact", 0.0350f, 0.0300f, 0.040f, 0.060f,
    { { 13.1f, 19.7f, 7.30f }, { 11.9f, 17.3f, 6.10f }, { 12.7f, 21.1f, 5.30f }, { 14.3f, 18.9f, 6.70f }, { 15.1f, 22.3f, 7.90f } } };
  const double ImpactFade = 0.25;      // seconds to fall to about a third
  const double ImpactSeconds = 1.5;

  // Smooth noise for the shake, -1 to 1: three waves for each channel, at
  // frequencies that don't line up, so it never visibly repeats
  float ShakeNoise(double t, int channel, ShakeProfile const& profile)
  {
    static const float phases[5][3] = {
      { 0.0f, 1.3f, 2.9f }, { 0.7f, 2.1f, 4.4f }, { 1.9f, 3.3f, 5.1f }, { 2.5f, 0.4f, 3.8f }, { 4.1f, 5.6f, 1.2f } };
    static const float weights[3] = { 1.f, 0.5f, 0.25f };
    double sum = 0.0;
    for (int k = 0; k < 3; ++k)
      sum += weights[k] * sin(2.0 * XM_PI * profile.Frequencies[channel][k] * t + phases[channel][k]);
    return (float)(sum / 1.75);
  }

  // The letterbox's shapes (CAMERA > Letterbox and guide lines)
  struct LetterboxRatio
  {
    const char* Name;
    float Ratio;   // width / height, 0: off
  };
  const LetterboxRatio LetterboxRatios[] = {
    { "Off", 0.f }, { "1.85 : 1", 1.85f }, { "2 : 1", 2.f }, { "2.20 : 1", 2.2f }, { "2.39 : 1", 2.39f },
    { "2.76 : 1", 2.76f }, { "4 : 3", 4.f / 3.f }, { "1 : 1", 1.f }, { "9 : 16 (phone)", 9.f / 16.f } };
  const int LetterboxCount = sizeof(LetterboxRatios) / sizeof(LetterboxRatios[0]);

  // The rotation that looks along look, level (no roll); with right as it
  // was when it looks straight up or down
  XMVECTOR LookRotation(XMVECTOR look, XMVECTOR right)
  {
    XMVECTOR back = XMVector3Normalize(XMVectorSetW(-look, 0.f));
    XMVECTOR level = XMVector3Cross(XMVectorSet(0.f, 1.f, 0.f, 0.f), back);
    if (XMVectorGetX(XMVector3LengthSq(level)) < 1e-8f)
      level = XMVectorSetW(right, 0.f) - back * XMVectorGetX(XMVector3Dot(right, back));
    level = XMVector3Normalize(level);
    XMVECTOR up = XMVector3Cross(back, level);
    XMMATRIX rows(level, up, back, XMVectorSet(0.f, 0.f, 0.f, 1.f));
    return XMQuaternionNormalize(XMQuaternionRotationMatrix(rows));
  }

  float Clamp(float value, float low, float high)
  {
    return !(value >= low) ? low : (value > high ? high : value);
  }
  const ULONGLONG FaithBackDelayMs = 5000; // after a respawn, before the tools' zones load again
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
  const int FocusCamera = 0;
  const int FocusAhead = 1;
  const int FocusRingFirst = 2;           // corners of a 200 m grid around the camera, out to the stream distance
  const int MaxRingFoci = 56;             // 600 m all round (about 38 with the ones going out), 1000 m in view (about 45)
  const int FocusTrackFirst = FocusRingFirst + MaxRingFoci;
  const int MaxTrackFoci = 30;            // points along the selected track
  const int FocusDistrictFirst = FocusTrackFirst + MaxTrackFoci;
  const int MaxDistrictFoci = 64;         // the districts set to Always in the menu (all of them take about 60)
  const int FocusWeatherFirst = FocusDistrictFirst + MaxDistrictFoci;
  const int MaxWeatherFoci = 2;           // the game's rain zones (VISUALS > Weather, see weather::ZonePoints)
  const int FocusShard = FocusWeatherFirst + MaxWeatherFoci; // the real Shard's zone (GAME > Districts), its streamer only
  const int MaxStreamingFoci = FocusShard + 1; // the tools' own
  const int MaxFocusStreamers = 4;        // a district's focus is only for these streamers
  // How important a focus is: which go in first, and which go first when
  // there's no room. What the camera looks at comes before what's behind it.
  enum FocusTier { TierCamera = 0, TierDistrict = 1, TierInView = 2, TierTrack = 3, TierBehind = 4 };
  const float InViewNear = 150.f;         // this close to the camera counts as in view (zones are big)
  const float GridSpacing = 200.f;        // between the corners of the camera's streaming grid
  const float GridKeepMargin = 100.f;     // a corner goes this much further out than it comes in
  const float GridKeepAngle = 15.f * 3.14159265f / 180.f; // and only where the camera looks, this far outside the view
  const float MaxStreamDistance = 600.f;  // all round; the zone slot and mesh limits hold back what doesn't fit
  // Only where the camera looks: about a third of the corners around it, so
  // it reaches further for the same zones and meshes
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
  int g_FocusOrder[MaxStreamingFoci]{};        // the foci by importance, most important first
  int g_FocusTier[MaxStreamingFoci]{};         // FocusTier of each
  __int64 g_FocusStreamers[MaxStreamingFoci][MaxFocusStreamers]{}; // a district's focus is only for these
  int g_FocusDistrict[MaxStreamingFoci]{};     // the district of a district's focus, -1 for the rest
  std::atomic<bool> g_DistrictFocusPresent[MaxDistrictFoci]{}; // in its streamer's list (for the menu)

  // The free camera, for the order zones load in (SortZoneLoads)
  struct SortView
  {
    bool Active;
    float X, Z;
    bool HasLook;
    float LookX, LookZ, MinDot;
  };
  SortView g_SortView{};

  // Zones are tested with their heights (ZoneStreamerSettings.TestZoneHeights
  // is on in the game): a focus above or below the band of the zone under it
  // is in no zone of that streamer and loads nothing there. Most buildings'
  // zones end at 400 m (some go higher, some are stacked under others), so a
  // camera higher up had none around it. A focus that's in no zone's band
  // where it is moves into the nearest band of a zone under or over it (the
  // same x and z: the zone the game would have found without the heights).
  // The focus is told which streamer asks by its key (SyncStreamingFoci adds
  // it with the streamer).
  const int ZoneGridOffset = 0x2D8;        // ZoneStreamerEntity -> its grid (the data's GridResource)
  const int GridZoneCountOffset = 0x18;    // int
  const int GridBoundsOffset = 0x20;       // min x, min z, max x, max z per zone
  const int GridCentresOffset = 0x28;      // x, z per zone (the game sorts loads by them)
  const int GridZonesOffset = 0x30;        // 0x20 per zone: lowest y, height, edge block count at +8, edge blocks at +0x10
  const int GridZoneStride = 0x20;

  bool SignBit(float value)
  {
    unsigned int bits;
    memcpy(&bits, &value, sizeof(bits));
    return (bits >> 31) != 0;
  }

  // Whether (x, z) is inside a zone's outline: the game's own test
  // (0x143C7FA60), 4 edges in each block of 5 rows of 4 floats. Reads game
  // memory, so only inside __try.
  bool InZoneOutline(__int64 pZone, float x, float z)
  {
    int blocks = *(int*)(pZone + 8);
    float const* pEdges = *(float const**)(pZone + 0x10);
    if (blocks <= 0 || blocks > 4096 || (__int64)pEdges < 0x10000)
      return false;
    unsigned int crossings = 0;
    for (int b = 0; b < blocks; ++b)
    {
      float const* rows = pEdges + b * 20;
      for (int lane = 0; lane < 4; ++lane)
      {
        float across = x - rows[lane];
        float along = z - rows[4 + lane];
        float side = across - rows[8 + lane] * along * rows[16 + lane];
        float past = along - rows[12 + lane];
        crossings ^= (!SignBit(along) && SignBit(side) && SignBit(past)) ? 1u : 0u;
      }
    }
    return (crossings & 1) != 0;
  }

  // The zone a focus at (x, y, z) is in, the way the game finds it with the
  // heights: one at (x, z) whose band holds y. When none does, y moves into
  // the nearest band of a zone at (x, z), and that's the zone. -1 for none
  // (y unchanged).
  int FindFocusZone(__int64 pZoneStreamer, float x, float z, float& y)
  {
    __try
    {
      __int64 pGrid = *(__int64*)(pZoneStreamer + ZoneGridOffset);
      if (pGrid < 0x10000)
        return -1;
      int count = *(int*)(pGrid + GridZoneCountOffset);
      float const* pBounds = *(float const**)(pGrid + GridBoundsOffset);
      __int64 pZones = *(__int64*)(pGrid + GridZonesOffset);
      if (count <= 0 || count > 4096 || (__int64)pBounds < 0x10000 || pZones < 0x10000)
        return -1;
      int bestZone = -1;
      float bestY = y, bestGap = FLT_MAX;
      for (int k = 0; k < count; ++k)
      {
        float const* bounds = pBounds + k * 4;
        // The game's box test first (cmpltps, mask 3)
        if (!(bounds[0] < x && bounds[1] < z && !(bounds[2] < x) && !(bounds[3] < z)))
          continue;
        __int64 pZone = pZones + (__int64)k * GridZoneStride;
        if (!InZoneOutline(pZone, x, z))
          continue;
        float low = *(float*)pZone, height = *(float*)(pZone + 4);
        if (!(height > 0.f))
          continue;
        float high = low + height;
        if (low < y && y < high)
          return k; // a zone here holds it, as the game will find it
        float inside = height > 2.f ? (y <= low ? low + 1.f : high - 1.f) : low + height * 0.5f;
        float gap = y <= low ? low - y : y - high;
        if (gap < bestGap)
        {
          bestGap = gap;
          bestY = inside;
          bestZone = k;
        }
      }
      if (bestZone >= 0)
        y = bestY;
      return bestZone;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      return -1;
    }
  }

  // y moved into the band of the zone at (x, z) whose band is nearest, when
  // no zone there holds it. Unchanged when one does, or none is there.
  void FitHeight(__int64 pZoneStreamer, float x, float z, float& y)
  {
    FindFocusZone(pZoneStreamer, x, z, y);
  }

  void GetStreamingFocus(StreamingFocus* pFocus, __int64 key, ZoneFocusResult* pResult)
  {
    AcquireSRWLockShared(&g_FocusLock);
    *pResult = g_FocusResults[pFocus->Index];
    ReleaseSRWLockShared(&g_FocusLock);
    if (key)
      FitHeight(key, pResult->Position[0], pResult->Position[2], pResult->Position[1]);
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
  // Zones only get their slots once they load, so the tools count the ones
  // asked for too (CountZonesInUse): a focus that asks for too many is taken
  // away again before they load.
  // The tools give each streamer room for 512 when the game starts (Hooks.cpp,
  // hZoneStreamerCtor); ZoneSlotCount() is what it has, and the levels keep
  // the same margins below it.
  const int ZoneSlotsOffset = 0x2F8;
  int ZoneSlotCount() { return util::hooks::ZoneSlotRoom(); }
  int ZoneSlotsAddBelow() { return ZoneSlotCount() - 28; }    // add one more focus only below this many in use or asked for
  int ZoneSlotsRemoveAbove() { return ZoneSlotCount() - 20; } // take the least important ones away above this
  // The zones a focus brings in also bring meshes and instances, which only
  // show in the counts as they load. So a new focus is added only once the
  // slot count has stood still this long, and never sooner than this after
  // the last one.
  const ULONGLONG ZoneSlotsSettleMs = 750;
  // Far from every limit that wait is skipped: one focus every 300 ms, as long
  // as few zones are still waiting to load (their meshes and instances can't
  // reach a limit even if they all come at once)
  const ULONGLONG FastFillPacingMs = 300;
  const int FastFillZonesBelow = 80;    // zones in use or asked for
  const int FastFillMostWaiting = 4;    // zones asked for that haven't loaded yet
  const float FastFillMostFill = 0.5f;  // meshes and model instances (of their room)
  const float FastFillBatchMargin = 0.2f; // the draw batch this far below the hold-back level

  // Per streamer: the slot count last seen, when it last changed, when a focus
  // was last added, and when foci were last taken away (for room, or to make
  // room for one in view); how many of the zones it was asked for hadn't
  // loaded yet at its last update; and when it last asked for a turn at the
  // gate all streamers share near the limits and last had one (see
  // MayAddFocus)
  struct StreamerGate
  {
    __int64 Streamer;
    int LastUsed;
    ULONGLONG LastChange;
    ULONGLONG LastAdd;
    ULONGLONG LastRemove;
    ULONGLONG LastSwap;
    int Waiting;
    ULONGLONG WaitingAt;
    ULONGLONG NearAsk;
    ULONGLONG NearServed;
  };
  enum Turn { Turn_Remove, Turn_Swap };
  StreamerGate g_Gates[8]{};
  SRWLOCK g_GateLock = SRWLOCK_INIT;
  // A streamer's waiting zones count for the shared gate this long after its
  // last update (one that has gone, after a level load, doesn't hold it)
  const ULONGLONG GateWaitingFreshMs = 2000;

  // Near the limits (from their settle levels, see AboveSettleLevel) all
  // streamers share one gate for the foci that aren't the camera's own: one
  // focus in any of them at a time (each streamer added its own in the same
  // update, and their zones landed on top of each other), and only once
  // every zone asked for in all of them has loaded: zones only get their
  // slots and their instances once they load, so before that the counts
  // didn't show what the last focus brings yet. A zone can wait for good (a
  // bundle that never starts loading), so after AddWaitMostMs of zones
  // waiting all along the next one goes in anyway, and the wait starts again
  // (at most one such add every AddWaitMostMs while zones go on waiting).
  // The streamers take turns: one doesn't get the turn while another that
  // has asked for one in the last GateWaitingFreshMs has waited longer since
  // its last (they update in the same order every frame, so the first could
  // otherwise take every turn while it had a focus missing).
  ULONGLONG g_LastNearAdd = 0;      // with g_GateLock
  ULONGLONG g_WaitingSince = 0;     // with g_GateLock: since when some zone has been waiting (0: none)
  const ULONGLONG AddWaitMostMs = 10000;

  // The tools' foci taken away for room are held out: any of them, by its
  // index, for every streamer (taken away from one streamer's list, no
  // streamer adds it back). At the limit the same foci used to go back in as
  // soon as the fill was under the hold-back level again, their zones loaded,
  // it went over, and they were taken away again: every 1 - 4 s, the same
  // zones loading and unloading (a tester's log, 2.0.0: 38 times in 10
  // minutes). Held out, a focus only comes back once the fill is well under
  // the hold-back level (HoldBackUseRelease of it, with nothing at any level),
  // and not before its time is up: HoldFirstMs, twice as long each time it's
  // taken away again within HoldAgainWithinMs of coming back, up to
  // HoldMostMs. While the most important focus that's missing is held out,
  // nothing after it goes in either (otherwise less important ones took its
  // room, and were taken away in turn). A focus that now stands for another
  // place (a new grid corner, the Always districts worked out again, a track
  // point that moved) forgets its hold: it was for the old place (see
  // ForgetMovedHolds).
  struct FocusHold
  {
    bool Out;
    ULONGLONG Until;     // not back before this
    ULONGLONG HoldMs;    // this time's
    ULONGLONG BackAt;    // when it last came back (0: not yet)
    float X, Z;          // where it was when it was taken away
    int Tier;            // and its tier then (FocusTier)
    ULONGLONG TierForgotAt; // when it last came back by coming into view (ForgetMovedHolds)
  };
  FocusHold g_FocusHolds[MaxStreamingFoci]{};  // with g_GateLock
  const ULONGLONG HoldFirstMs = 10000;
  const ULONGLONG HoldMostMs = 120000;
  const ULONGLONG HoldAgainWithinMs = 60000;
  const float HoldForgetDistance = 100.f;      // moved this far (flat), it's another place
  // How close the fullest of the limits is to where the tools hold back (1:
  // at its hold-back level, see WatchInstanceLists), and how far under it the
  // foci held out come back: 0.875 of the draw batch's 80 % is 70 %
  std::atomic<float> g_LimitUse{ 0.f };
  const float HoldBackUseRelease = 0.875f;
  std::atomic<int> g_TakeAways{ 0 };   // foci taken away for room, all streamers (for the log)

  // Taken away for room: held out (see FocusHold); x and z where it was. One
  // already held out (another streamer's list had it too) keeps its time.
  // Call with g_GateLock held.
  void HoldOut(int i, ULONGLONG now, float x, float z, int tier)
  {
    FocusHold& hold = g_FocusHolds[i];
    if (hold.Out)
      return;
    ULONGLONG ms = hold.BackAt && now - hold.BackAt < HoldAgainWithinMs ? hold.HoldMs * 2 : HoldFirstMs;
    if (ms < HoldFirstMs) ms = HoldFirstMs;
    if (ms > HoldMostMs) ms = HoldMostMs;
    hold.Out = true;
    hold.Until = now + ms;
    hold.HoldMs = ms;
    hold.X = x;
    hold.Z = z;
    hold.Tier = tier;
  }

  // A focus that now stands for another place forgets its hold (see
  // FocusHold): a grid corner's focus given to a new corner, the Always
  // districts worked out again, a track point that moved. The camera's own
  // and the one ahead of it go with the camera, so theirs only once it is
  // further than the stream distance in view from where they were taken
  // away (Go to, a track starting somewhere else, the camera off and on
  // elsewhere): the zones that filled the room went with the old place, and
  // held out the camera's own focus would hold back everything after it.
  // One taken away behind the camera (or on the track) that's now in view
  // forgets it too: turned round at a limit, the view would otherwise stay
  // unloaded (held out, it holds back everything after it, so nothing behind
  // makes room for it, see SyncStreamingFoci); in view it goes the way 2.0.0
  // had it, one behind taken away for it a second. At most once every
  // HoldAgainWithinMs a focus, keeping the rest of its record (taken away
  // again soon, its hold doubles): one at the edge of the view that the
  // camera's shake swings in and out would otherwise come back after every
  // take-away. Camera hook.
  void ForgetMovedHolds(ZoneFocusResult const results[], bool const wanted[], int const tier[])
  {
    ULONGLONG now = GetTickCount64();
    AcquireSRWLockExclusive(&g_GateLock);
    for (int i = 0; i < MaxStreamingFoci; ++i)
    {
      FocusHold& hold = g_FocusHolds[i];
      if (!hold.Out || !wanted[i])
        continue;
      if (hold.Tier > TierInView && tier[i] == TierInView &&
        !(hold.TierForgotAt && now - hold.TierForgotAt < HoldAgainWithinMs))
      {
        hold.Out = false;
        hold.BackAt = now;
        hold.TierForgotAt = now;
        continue;
      }
      float forget = i < FocusRingFirst ? MaxStreamDistanceInView : HoldForgetDistance;
      float dx = results[i].Position[0] - hold.X, dz = results[i].Position[2] - hold.Z;
      if (dx * dx + dz * dz > forget * forget)
        hold = FocusHold{};
    }
    ReleaseSRWLockExclusive(&g_GateLock);
  }

  // A level load: every hold is forgotten (their zones went with the level)
  void ForgetAllHolds()
  {
    AcquireSRWLockExclusive(&g_GateLock);
    for (FocusHold& hold : g_FocusHolds)
      hold = FocusHold{};
    ReleaseSRWLockExclusive(&g_GateLock);
  }

  // How many of the tools' foci that are wanted are held out now (for the log)
  int CountHeldOut()
  {
    bool wanted[MaxStreamingFoci];
    AcquireSRWLockShared(&g_FocusLock);
    memcpy(wanted, g_FocusWanted, sizeof(wanted));
    ReleaseSRWLockShared(&g_FocusLock);
    int held = 0;
    AcquireSRWLockShared(&g_GateLock);
    for (int i = 0; i < MaxStreamingFoci; ++i)
      held += g_FocusHolds[i].Out && wanted[i] ? 1 : 0;
    ReleaseSRWLockShared(&g_GateLock);
    return held;
  }

  // Taking foci away only shows in the counts once their zones have unloaded,
  // so it's paced: two every half second when it's too full (not on every
  // update, which emptied everything within a second and then loaded it all
  // again), and one a second to make room for a focus in view.
  const ULONGLONG RemovePacingMs = 500;
  const ULONGLONG SwapPacingMs = 1000;

  // The streamer's entry (a new streamer, or one that came back after a level
  // load, takes the oldest). Call with g_GateLock held.
  StreamerGate* FindGate(__int64 pZoneStreamer, int used, ULONGLONG now)
  {
    for (StreamerGate& gate : g_Gates)
      if (gate.Streamer == pZoneStreamer) return &gate;
    StreamerGate* pGate = &g_Gates[0];
    for (StreamerGate& gate : g_Gates)
      if (gate.LastChange < pGate->LastChange) pGate = &gate;
    *pGate = { pZoneStreamer, used, now, 0, 0, 0, 0, 0, 0, 0 };
    return pGate;
  }

  // A streamer's zones asked for that haven't loaded yet, at its update: for
  // the shared gate (MayAddFocus), with since when any streamer has had some
  void NoteZonesWaiting(__int64 pZoneStreamer, int used, int waiting)
  {
    ULONGLONG now = GetTickCount64();
    AcquireSRWLockExclusive(&g_GateLock);
    StreamerGate* pGate = FindGate(pZoneStreamer, used, now);
    pGate->Waiting = waiting > 0 ? waiting : 0;
    pGate->WaitingAt = now;
    int all = 0;
    for (StreamerGate const& gate : g_Gates)
      if (gate.Streamer && now - gate.WaitingAt <= GateWaitingFreshMs) all += gate.Waiting;
    if (all == 0)
      g_WaitingSince = 0;
    else if (!g_WaitingSince)
      g_WaitingSince = now;
    ReleaseSRWLockExclusive(&g_GateLock);
  }

  // Whether it's been long enough since the last removal (swap, zone read);
  // if so, this one counts as the last
  bool TakeTurn(__int64 pZoneStreamer, Turn turn, ULONGLONG ms)
  {
    ULONGLONG now = GetTickCount64();
    AcquireSRWLockExclusive(&g_GateLock);
    StreamerGate* pGate = FindGate(pZoneStreamer, 0, now);
    ULONGLONG& last = turn == Turn_Swap ? pGate->LastSwap : pGate->LastRemove;
    bool allowed = now - last >= ms;
    if (allowed)
      last = now;
    ReleaseSRWLockExclusive(&g_GateLock);
    return allowed;
  }


  // A streamer's table of its bundles: its zones, then the regions above
  // them (as many as its data's BundleNames). 8 bytes each: flags, slot + 1
  // (0: no slot), refs (+4: the foci whose zone it is or is next to, and the
  // zones under it), parent. A bundle with refs is wanted and gets a slot
  // when it loads; flag 4 is set while it's loading (0x143C816D7 clears it
  // once it's loaded), and the slot goes when it's released (0x143C81E60).
  const int ZoneTableOffset = 0x2F0;
  const int ZoneDataOffset = 0x28;             // ZoneStreamerEntity -> its ZoneStreamerEntityData
  const int ZoneBundleNamesOffset = 0x80;      // data -> BundleNames (count at - 4)
  const unsigned char ZoneLoadingFlag = 4;
  // The zone table's other bits: 0 load and 1 unload asked (requests for one
  // step of the streamer's update, which acts on them, 0x143C81E60 then
  // 0x143C81D20, and clears them), 3 available (the game only loads an
  // available zone, 0x143C80180, and only releases one, 0x143C81E60)
  const unsigned char ZoneLoadAsked = 1;
  const unsigned char ZoneUnloadAsked = 2;
  const unsigned char ZoneAvailableFlag = 8;

  // How many of a streamer's bundles are loaded or wanted (refs, or a slot),
  // and how many of the wanted ones aren't loaded yet. A focus's zones count
  // from the moment the streamer has found its zone (on its next update),
  // long before they get their slots. One that isn't available and has no
  // slot doesn't count: the game won't load it (a district set to Never).
  // -1 if it couldn't be read.
  int CountZonesInUse(__int64 pZoneStreamer, int& waiting)
  {
    waiting = 0;
    __try
    {
      __int64 data = *(__int64*)(pZoneStreamer + ZoneDataOffset);
      __int64 pTable = *(__int64*)(pZoneStreamer + ZoneTableOffset);
      if (data < 0x10000 || pTable < 0x10000)
        return -1;
      __int64 pNames = *(__int64*)(data + ZoneBundleNamesOffset);
      if (pNames < 0x10000)
        return -1;
      int count = *(int*)(pNames - 4);
      if (count <= 0 || count > 4096)
        return -1;
      int inUse = 0;
      for (int k = 0; k < count; ++k)
      {
        unsigned char const* pEntry = (unsigned char const*)(pTable + (__int64)k * 8);
        unsigned short slot = *(unsigned short const*)(pEntry + 2);
        unsigned short refs = *(unsigned short const*)(pEntry + 4);
        if (!slot && !(pEntry[0] & ZoneAvailableFlag))
          continue;
        if (refs || slot)
          ++inUse;
        if (refs && (!slot || (pEntry[0] & ZoneLoadingFlag)))
          ++waiting;
      }
      return inUse;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      return -1;
    }
  }

  // Vistas: the city's buildings seen from far away. VISTA_CityBuildings is a
  // ZoneStreamerVistaEntity with one zone for each zone of the buildings'
  // streamer: its objects are a low-detail copy of that zone's buildings.
  // Each zone lists vista zones a bit further away (VistaZoneInfo.Neighbours),
  // and a vista zone shows while any focus is in a zone that lists it: on
  // each focus's zone change (0x143C87630) a count per vista zone (+4 of its
  // state) goes up or down, and 0x143C881C0 shows its objects (0x143C87F60)
  // when it goes above 0 and hides them when it's back to 0. The real
  // buildings only load next to each focus, so with Faith's focus alone the
  // two don't meet; with the free camera's foci far from her, each one's
  // vista zones are where the other has the real buildings loaded, and both
  // show at once.
  //
  // The vista is a client entity: it listens to the client's copy of the
  // buildings' streamer (a RealmProxy, which passes the server streamer's
  // zone changes on), not to the server streamers the zone streamer hook
  // sees. Their zone names, centres and loaded zones come from the server
  // streamer with the same sublevel.
  //
  // The tools hold vista zones back: 0x143C87F60 (hooked) leaves them hidden
  // when the game shows them, and the game's own counts go on as before, so
  // letting one go shows it again if the game still does.
  const int StreamerListenersOffset = 0x38;    // ZoneStreamerEntity / RealmProxy -> vector of listeners (begin, end)
  const int VistaListenerOffset = 0x30;        // the vista's listener (what's in that vector)
  // PE timestamp 0x5762E3B7 only
  const __int64 VistaVTableRva = 0x1CFFF88;    // ZoneStreamerVistaEntity
  const __int64 VistaListenerVTableRva = 0x1D00040;
  const __int64 RealmProxyClassRva = 0x289EEA0; // RealmProxy's ClassInfo
  const __int64 RealmProxyVTableRva = 0x1CFE7C0;
  const int ClassInstanceListsOffset = 0x60;   // ClassInfo -> four instance lists, 0x10 apart (first link)
  const int StreamerClassLinkOffset = 0x268;   // a streamer's link in its class's instance list (next link first)
  const int VistaStreamerOffset = 0x50;        // the streamer it listens to
  const int VistaStatesOffset = 0x58;          // 8 bytes per zone: first object, object count, focus count (byte), flags
  const int VistaZoneCountOffset = 0x60;
  const unsigned short VistaShown = 1;         // flags: the game shows it
  const unsigned short VistaHiddenForGood = 2; // the game hides it for good (next to where the level started)
  const int FocusCacheOffset = 0x2A8;          // ZoneStreamerEntity -> where each focus is (0x20 each), the game's own first
  const int MaxVistas = 4;
  const int MaxVistaHosts = 16;                // streamers a vista can listen to
  const int MaxVistaZones = 2048;
  const ULONGLONG VistaReadMs = 250;
  const ULONGLONG VistaGoneMs = 5000;          // a server streamer not updated this long is gone
  bool IsRenderArenaBuild();                   // the same build check (below)

  struct VistaZone
  {
    char Name[24];          // its zone's bundle name in the streamer
    float X, Z;             // its zone's centre
    unsigned short Objects;
    unsigned short Flags;   // VistaShown, VistaHiddenForGood
    bool Loaded;            // its zone's buildings are loaded
    bool UserOff;           // switched off in the menu
    bool Off;               // held back now (switched off, all off, or over its loaded buildings)
  };
  struct Vista
  {
    __int64 Entity;
    __int64 States;         // a new level has new ones
    __int64 Streamer;       // the one it listens to
    bool Matched;           // a server streamer with as many zones: the names, centres and Loaded are its zones'
    std::string Layer;
    float FocusX, FocusZ;   // the game's own focus (next to Faith), for the distances with the camera off
    ULONGLONG Seen;
    std::vector<VistaZone> Zones;
  };
  std::vector<Vista> g_Vistas;
  SRWLOCK g_VistaLock = SRWLOCK_INIT;

  // The server's zone streamers, as the zone streamer hook sees them
  struct ServerStreamer
  {
    __int64 Streamer;
    __int64 Data;
    char Path[128];         // its sublevel
    ULONGLONG Seen;
  };
  ServerStreamer g_ServerStreamers[8]{};
  SRWLOCK g_ServerStreamerLock = SRWLOCK_INIT;

  __int64 ReadStreamerData(__int64 pStreamer)
  {
    __try
    {
      return *(__int64*)(pStreamer + ZoneDataOffset);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      return 0;
    }
  }

  // A streamer's sublevel path (SubLevelPath of its data)
  void ReadSubLevelPath(__int64 pStreamer, char* path, size_t size)
  {
    path[0] = 0;
    __try
    {
      __int64 data = *(__int64*)(pStreamer + ZoneDataOffset);
      const char* pPath = data >= 0x10000 ? *(const char**)(data + 0x68) : nullptr;
      size_t length = 0;
      while (pPath && length + 1 < size && pPath[length] >= 32 && pPath[length] < 127)
      {
        path[length] = pPath[length];
        ++length;
      }
      path[length] = 0;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      path[0] = 0;
    }
  }

  // From the zone streamer hook: this streamer is there
  void NoteServerStreamer(__int64 pZoneStreamer)
  {
    ULONGLONG now = GetTickCount64();
    __int64 data = ReadStreamerData(pZoneStreamer);
    AcquireSRWLockExclusive(&g_ServerStreamerLock);
    ServerStreamer* pEntry = nullptr;
    for (ServerStreamer& entry : g_ServerStreamers)
      if (entry.Streamer == pZoneStreamer) { pEntry = &entry; break; }
    if (!pEntry)
    {
      pEntry = &g_ServerStreamers[0];
      for (ServerStreamer& entry : g_ServerStreamers)
        if (entry.Seen < pEntry->Seen) pEntry = &entry;
      pEntry->Streamer = pZoneStreamer;
      pEntry->Data = 0;
      pEntry->Path[0] = 0;
    }
    if (pEntry->Data != data || !pEntry->Path[0])
    {
      pEntry->Data = data;
      ReadSubLevelPath(pZoneStreamer, pEntry->Path, sizeof(pEntry->Path));
    }
    pEntry->Seen = now;
    ReleaseSRWLockExclusive(&g_ServerStreamerLock);
  }

  // The client's copies of the streamers, from RealmProxy's instance lists
  int FindRealmProxies(__int64* proxies, int max)
  {
    __int64 base = (__int64)g_gameHandle;
    int found = 0;
    __try
    {
      __int64 pClass = base + RealmProxyClassRva;
      for (int list = 0; list < 4 && found < max; ++list)
      {
        __int64 link = *(__int64*)(pClass + ClassInstanceListsOffset + list * 0x10);
        for (int n = 0; link >= 0x10000 && n < 64 && found < max; ++n)
        {
          __int64 pProxy = link - StreamerClassLinkOffset;
          if (*(__int64*)pProxy == base + RealmProxyVTableRva)
            proxies[found++] = pProxy;
          link = *(__int64*)link;
        }
      }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
    return found;
  }

  struct VistaFound
  {
    __int64 Entity;
    __int64 States;
    int Count;
    __int64 Host;           // the streamer it listens to
  };

  // The vistas listening to a streamer, from its listeners
  int FindVistas(__int64 pStreamer, VistaFound* vistas, int max)
  {
    __int64 base = (__int64)g_gameHandle;
    int found = 0;
    __try
    {
      __int64* pBegin = *(__int64**)(pStreamer + StreamerListenersOffset);
      __int64* pEnd = *(__int64**)(pStreamer + StreamerListenersOffset + 8);
      if (!pBegin || pEnd < pBegin || pEnd - pBegin > 64)
        return 0;
      for (__int64* p = pBegin; p < pEnd && found < max; ++p)
      {
        __int64 pListener = *p;
        if (pListener < 0x10000)
          continue;
        __int64 pVista = pListener - VistaListenerOffset;
        if (*(__int64*)pVista != base + VistaVTableRva || *(__int64*)pListener != base + VistaListenerVTableRva ||
          *(__int64*)(pVista + VistaStreamerOffset) != pStreamer)
          continue;
        __int64 states = *(__int64*)(pVista + VistaStatesOffset);
        int count = *(int*)(pVista + VistaZoneCountOffset);
        if (states < 0x10000 || count <= 0 || count > MaxVistaZones)
          continue;
        vistas[found++] = { pVista, states, count, pStreamer };
      }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
    return found;
  }

  // One vista's zones (count of them): the game's states, and with a server
  // streamer that has as many zones (matched) their names, centres and
  // whether their buildings are loaded. No C++ objects in here (for __try).
  // False if it couldn't be read.
  bool ReadVistaZones(VistaFound const& vista, __int64 pServerStreamer, VistaZone* zones, bool& matched,
    float& focusX, float& focusZ)
  {
    matched = false;
    focusX = focusZ = 0.f;
    __try
    {
      unsigned char const* pStates = (unsigned char const*)vista.States;
      for (int k = 0; k < vista.Count; ++k)
      {
        VistaZone& zone = zones[k];
        zone.Objects = *(unsigned short const*)(pStates + k * 8 + 2);
        zone.Flags = *(unsigned short const*)(pStates + k * 8 + 6);
        zone.Name[0] = 0;
        zone.X = zone.Z = 0.f;
        zone.Loaded = false;
      }
      if (!pServerStreamer)
        return true;
      __int64 pCache = *(__int64*)(pServerStreamer + FocusCacheOffset);
      if (pCache >= 0x10000 && *(unsigned int*)(pServerStreamer + ZoneFocusListOffset + ZoneFocusCountOffset))
      {
        focusX = *(float*)pCache;
        focusZ = *(float*)(pCache + 8);
      }
      __int64 data = *(__int64*)(pServerStreamer + ZoneDataOffset);
      __int64 pGrid = *(__int64*)(pServerStreamer + ZoneGridOffset);
      __int64 pTable = *(__int64*)(pServerStreamer + ZoneTableOffset);
      if (data < 0x10000 || pGrid < 0x10000 || pTable < 0x10000)
        return true;
      int count = *(int*)(pGrid + GridZoneCountOffset);
      float const* pCentres = *(float const**)(pGrid + GridCentresOffset);
      __int64 pNames = *(__int64*)(data + ZoneBundleNamesOffset);
      unsigned int nameCount = pNames >= 0x10000 ? *(unsigned int*)(pNames - 4) : 0;
      if (count != vista.Count || (__int64)pCentres < 0x10000 || nameCount < (unsigned int)count)
        return true;
      matched = true;
      for (int k = 0; k < count; ++k)
      {
        VistaZone& zone = zones[k];
        zone.X = pCentres[k * 2];
        zone.Z = pCentres[k * 2 + 1];
        unsigned char const* pEntry = (unsigned char const*)(pTable + (__int64)k * 8);
        zone.Loaded = *(unsigned short const*)(pEntry + 2) != 0 && !(pEntry[0] & ZoneLoadingFlag);
        const char* pName = *(const char**)(pNames + (__int64)k * 8);
        size_t n = 0;
        while (pName && n + 1 < sizeof(zone.Name) && pName[n] >= 32 && pName[n] < 127)
        {
          zone.Name[n] = pName[n];
          ++n;
        }
        zone.Name[n] = 0;
      }
      return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      return false;
    }
  }

  // "Levels/SP/SP_MainCity/CentralCityBuildings_ZS_ZS_" -> "Buildings"
  std::string LayerName(const char* path)
  {
    std::string name(path);
    size_t slash = name.find_last_of('/');
    if (slash != std::string::npos)
      name = name.substr(slash + 1);
    size_t zs = name.find("_ZS");
    if (zs != std::string::npos)
      name = name.substr(0, zs);
    if (name.size() > 11 && name.compare(0, 11, "CentralCity") == 0)
      name = name.substr(11);
    return name.empty() ? std::string("City") : name;
  }

  // The vistas: read four times a second, and each zone held back or let go
  // as the menu and its buildings say. From the camera hook, on the game's
  // own thread (the client's).
  void UpdateVistas(bool allShown, bool hideLoaded)
  {
    static bool s_vistaBuild = IsRenderArenaBuild();
    static ULONGLONG s_lastRead = 0;
    ULONGLONG now = GetTickCount64();
    if (!s_vistaBuild || now - s_lastRead < VistaReadMs)
      return;
    s_lastRead = now;

    // Where vistas can listen: the client's copies of the streamers, and
    // (in case) the server's own
    ServerStreamer servers[8];
    AcquireSRWLockShared(&g_ServerStreamerLock);
    memcpy(servers, g_ServerStreamers, sizeof(servers));
    ReleaseSRWLockShared(&g_ServerStreamerLock);
    __int64 hosts[MaxVistaHosts + 8];
    int hostCount = FindRealmProxies(hosts, MaxVistaHosts);
    for (ServerStreamer const& server : servers)
      if (server.Streamer && now - server.Seen <= VistaGoneMs)
        hosts[hostCount++] = server.Streamer;
    VistaFound found[MaxVistas];
    int foundCount = 0;
    for (int h = 0; h < hostCount && foundCount < MaxVistas; ++h)
      foundCount += FindVistas(hosts[h], found + foundCount, MaxVistas - foundCount);

    std::vector<VistaZone> zones;
    struct Change
    {
      __int64 Entity;
      unsigned int Zone;
      bool Show;
    };
    std::vector<Change> changes;
    for (int v = 0; v < foundCount; ++v)
    {
      // The server streamer of the same sublevel has its zones
      char path[128];
      ReadSubLevelPath(found[v].Host, path, sizeof(path));
      __int64 data = ReadStreamerData(found[v].Host);
      __int64 server = 0;
      for (ServerStreamer const& entry : servers)
      {
        if (!entry.Streamer || now - entry.Seen > VistaGoneMs)
          continue;
        if (entry.Streamer == found[v].Host || (data && entry.Data == data) || (path[0] && strcmp(entry.Path, path) == 0))
        {
          server = entry.Streamer;
          break;
        }
      }
      zones.assign(found[v].Count, VistaZone{});
      bool matched = false;
      float focusX = 0.f, focusZ = 0.f;
      if (!ReadVistaZones(found[v], server, zones.data(), matched, focusX, focusZ))
        continue;
      std::string layer = LayerName(path);

      AcquireSRWLockExclusive(&g_VistaLock);
      Vista* pVista = nullptr;
      for (Vista& vista : g_Vistas)
        if (vista.Entity == found[v].Entity) { pVista = &vista; break; }
      if (!pVista)
      {
        g_Vistas.push_back(Vista());
        pVista = &g_Vistas.back();
        util::log::Write("Vista 0x%I64X: %d zones of far-away %s, %s (thread %u)", found[v].Entity, found[v].Count,
          layer.c_str(), matched ? "one for each zone of its streamer" : "its streamer's zones not found",
          GetCurrentThreadId());
      }
      // The same level (the same states): the menu's choices and what's held
      // back stay. A new one starts with nothing held back.
      bool sameLevel = pVista->States == found[v].States && pVista->Zones.size() == zones.size();
      for (size_t k = 0; k < zones.size(); ++k)
      {
        VistaZone& zone = zones[k];
        zone.UserOff = sameLevel && pVista->Zones[k].UserOff;
        bool wasOff = sameLevel && pVista->Zones[k].Off;
        zone.Off = !allShown || zone.UserOff || (hideLoaded && matched && zone.Loaded);
        // Shown or hidden now only if the game shows it (otherwise the hook
        // sees to it when the game does)
        if (zone.Off != wasOff && (zone.Flags & VistaShown) && !(zone.Flags & VistaHiddenForGood))
          changes.push_back({ found[v].Entity, (unsigned int)k, !zone.Off });
      }
      pVista->Entity = found[v].Entity;
      pVista->States = found[v].States;
      pVista->Streamer = found[v].Host;
      pVista->Matched = matched;
      pVista->Layer = layer;
      pVista->FocusX = focusX;
      pVista->FocusZ = focusZ;
      pVista->Seen = now;
      pVista->Zones.swap(zones);
      ReleaseSRWLockExclusive(&g_VistaLock);
    }

    // Forgotten: the ones that aren't there any more
    AcquireSRWLockExclusive(&g_VistaLock);
    g_Vistas.erase(std::remove_if(g_Vistas.begin(), g_Vistas.end(), [&](Vista const& vista) {
      for (int v = 0; v < foundCount; ++v)
        if (found[v].Entity == vista.Entity) return false;
      return true;
    }), g_Vistas.end());
    ReleaseSRWLockExclusive(&g_VistaLock);

    // Outside the lock: the hook takes it too
    for (Change const& change : changes)
      util::hooks::ShowVistaZone(change.Entity, change.Zone, change.Show);
  }

  __int64 ReadVistaStates(__int64 pVista)
  {
    __try
    {
      return *(__int64*)(pVista + VistaStatesOffset);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      return 0;
    }
  }

  // Whether the tools hold this zone of the vista back (for the hook)
  bool VistaZoneOff(__int64 pVista, unsigned int zone)
  {
    __int64 states = ReadVistaStates(pVista);
    if (!states)
      return false;
    bool off = false;
    AcquireSRWLockShared(&g_VistaLock);
    for (Vista const& vista : g_Vistas)
    {
      if (vista.Entity == pVista && vista.States == states)
      {
        off = zone < vista.Zones.size() && vista.Zones[zone].Off;
        break;
      }
    }
    ReleaseSRWLockShared(&g_VistaLock);
    return off;
  }

  // The Shard (Kruger's tower, "SkyCity" in the game's data). The real tower
  // is one zone of the city's buildings, "55" (CentralCityBuildings_ZS_ZS_55,
  // in the Default district). From the city the game shows a far-away model
  // of it instead: the sub-level SkyCity_Backdrop (skycity_backdrop_far_whole
  // and _lit) or, once the story has wrecked the tower, SkyCity_RuinBackdrop
  // (skycity_backdrop_far_destroyed). The main level's district streaming
  // (layer48_districtstreaming) streams that sub-level in when Faith enters
  // the city's area (layer1_streaming's volume 22) and out when she leaves
  // it, and out there the real tower loads around her (2026-10-25: the game's
  // data, and a live read with both loaded). Around the free camera, or with
  // the Default district on Always, the real tower loaded while Faith was in
  // the city, and both were drawn, one over the other. The tools make the
  // game's swap for the camera:
  // - unless the camera (with it off, the game's own focus, next to Faith) is
  //   out of the city's area and in the area around the Shard (ShardArea),
  //   the real tower's zone is kept out like a district set to Never
  //   (EnforceNeverDistricts: the zones the game's own foci are in and next
  //   to stay, so a mission going there still has it);
  // - while the real tower is loaded, the far-away model's members are hidden
  //   (their Visible, through the model group's own handler) and shown again
  //   once it isn't;
  // - out of the city the free camera also counts for the game's camera
  //   trigger on the area around the Shard, which streams the tower's top in
  //   (Kruger's office, sub-level SkyCityTopOpenBlinds / ClosedBlinds; see
  //   util::hooks::SetAreaTriggerCamera).
  // Always on, with the free camera on or off (with it off the view is the
  // game's own camera and Faith, as the game has it).
  const char ShardZoneName[] = "55";
  const char ShardStreamerPath[] = "CentralCityBuildings_ZS";  // in the buildings' sublevel path
  const char ShardLodMesh[] = "skycity_backdrop_far_";         // in the far-away model's mesh names (any case)
  // The city's area the game streams the Shard by: x, z of volume 22 (it's
  // 2000 m tall from y = 0, which this leaves out: over the city is in it)
  const float CityArea[4][2] = { { 1691.87f, -1155.07f }, { 1708.01f, 1891.06f }, { -1278.21f, 1590.95f },
    { -1235.05f, -1259.32f } };
  // The area around the Shard the game streams the tower's top by: x, z of
  // layer1_streaming's volume 15 (as util::hooks' ShardArea, which the game's
  // own volume test agreed with on every probe point, 2026-10-25). It starts
  // at the city's south edge. Out of the city the real tower is the view's
  // only in there: past the city's other edges (north, east, west) nothing of
  // the Shard is near, and with the Default district on Always the real tower
  // loaded 4.6 km away and the far-away model went (user, 2026-11-03: the
  // camera high over the city's north-west edge, CT.log 11:11 at -677, 1704)
  const float ShardArea[8][2] = { { 2406.72f, -1968.19f }, { 2723.83f, -3548.88f }, { 1756.24f, -4646.43f },
    { 326.239f, -4850.4f }, { -872.062f, -3680.05f }, { -781.582f, -2490.59f }, { 210.563f, -1424.52f },
    { 1429.53f, -1162.08f } };
  const float CityEdgeMargin = 50.f;      // the camera changes sides only this far past the edge (either area)
  const ULONGLONG ShardReadMs = 100;      // the real tower's zone read this often (camera hook)
  const ULONGLONG ShardSearchMs = 2000;   // and the far-away model looked for, while it's hidden
  // The model groups (ClientStaticModelGroupEntity): their ClassInfo's
  // instance lists (+ 0x60, 4 lists 0x10 apart, the link at entity + 0x40),
  // the group's data at + 0x30 (StaticModelGroupEntityData: its MemberDatas
  // at + 0x90, 0x68 bytes each; a member's MeshAsset at + 0x38 and its
  // instance count at + 0x40; an asset's name at + 0x10), the member count at
  // + 0xE8 (0x1431840F0, 0x14317B310, 0x14318DC70; read live 2026-10-25).
  // PE timestamp 0x5762E3B7 only (IsRenderArenaBuild).
  const __int64 ModelGroupClassRva = 0x27ED720;
  const __int64 ModelGroupVTableRva = 0x1AFBE90;
  const int EntityLinkOffset = 0x40;      // an entity's link in its class's instance list
  const int AssetNameOffset = 0x10;
  const int ModelGroupDataOffset = 0x30;
  const int ModelGroupMemberCountOffset = 0xE8;
  const int GroupDataMembersOffset = 0x90;
  const int GroupMemberDataSize = 0x68;
  const int MemberMeshOffset = 0x38;
  const int MemberInstanceCountOffset = 0x40;
  const int MaxShardLodMembers = 4;       // members matched per group: the far-away models' 1 or 2, the windows' 2 or 3
  const int MaxShardLodGroups = 4;
  const int MaxShardLodInstances = 64;    // instances per member (the lair's 512 blinds: 14)
  const int MaxGroupMembers = 4096;       // a buildings zone's group has hundreds (zone 55's: 591)

  // -1 unknown, 0 out of the city's area, 1 in it (zone streamer hook)
  std::atomic<int> g_ShardViewerInCity{ -1 };
  // The same for the area around the Shard (ShardArea), and whether the real
  // tower may load for the view: out of the city and in that area (-1 unknown)
  std::atomic<int> g_ShardViewerNearShard{ -1 };
  std::atomic<int> g_ShardTowerAllowed{ -1 };
  // When the real tower was last kept out again, and whether a far-away
  // model of the Shard is loaded (camera hook): back in the city the real
  // tower is kept out only once the far-away one is back (the game streams it
  // in again, see util::hooks::UpdateLandmarkTriggers), or after this long
  std::atomic<ULONGLONG> g_ShardInCitySince{ 0 };
  std::atomic<bool> g_ShardLodPresent{ false };
  // For the menu (camera hook): the real tower's zone (1 loaded, 0 not, -1
  // not known) and whether its top is loaded (UpdateShardTip, while it is)
  std::atomic<int> g_ShardTowerLoaded{ -1 };
  std::atomic<bool> g_ShardTopIn{ false };
  const ULONGLONG ShardBackdropWaitMs = 10000;
  const ULONGLONG ShardPresenceMs = 1000;  // the far-away model looked for this often while it's shown

  // How far (x, z) is inside an area: the distance to its nearest edge,
  // negative outside
  float AreaDepth(float const (*corners)[2], int count, float x, float z)
  {
    bool inside = false;
    float nearest = FLT_MAX;
    for (int i = 0, j = count - 1; i < count; j = i++)
    {
      float ax = corners[j][0], az = corners[j][1], bx = corners[i][0], bz = corners[i][1];
      if ((bz > z) != (az > z) && x < (ax - bx) * (z - bz) / (az - bz) + bx)
        inside = !inside;
      float ex = ax - bx, ez = az - bz;
      float t = ((x - bx) * ex + (z - bz) * ez) / (ex * ex + ez * ez);
      t = t < 0.f ? 0.f : (t > 1.f ? 1.f : t);
      float dx = x - (bx + t * ex), dz = z - (bz + t * ez);
      nearest = (std::min)(nearest, sqrtf(dx * dx + dz * dz));
    }
    return inside ? nearest : -nearest;
  }
  float CityAreaDepth(float x, float z) { return AreaDepth(CityArea, 4, x, z); }
  float ShardAreaDepth(float x, float z) { return AreaDepth(ShardArea, (int)_countof(ShardArea), x, z); }

  // The real tower's zone in a streamer's data: its index by name if it's
  // the buildings' streamer, -1 if not (or it can't be read)
  int ReadShardZoneIndex(__int64 data)
  {
    __try
    {
      const char* pPath = data >= 0x10000 ? *(const char**)(data + 0x68) : nullptr;
      if (!pPath || !strstr(pPath, ShardStreamerPath))
        return -1;
      __int64 pNames = *(__int64*)(data + ZoneBundleNamesOffset);
      if (pNames < 0x10000)
        return -1;
      int count = *(int*)(pNames - 4);
      for (int k = 0; k < count && k < 4096; ++k)
      {
        const char* pName = *(const char**)(pNames + (__int64)k * 8);
        if (pName && strcmp(pName, ShardZoneName) == 0)
          return k;
      }
      return -1;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      return -1;
    }
  }

  // The same, remembered per data (each caller keeps its own, on its thread)
  struct ShardZoneCache
  {
    __int64 Data[4];
    int Zone[4];
    int Next;
  };
  int ShardZoneOf(__int64 data, ShardZoneCache& cache)
  {
    if (!data)
      return -1;
    for (int i = 0; i < 4; ++i)
      if (cache.Data[i] == data) return cache.Zone[i];
    int zone = ReadShardZoneIndex(data);
    cache.Data[cache.Next] = data;
    cache.Zone[cache.Next] = zone;
    cache.Next = (cache.Next + 1) % 4;
    return zone;
  }

  // A zone's state in its streamer's table: 1 loaded, 0 not (or still
  // loading), -1 unreadable
  int ReadZoneLoaded(__int64 pZoneStreamer, int zone)
  {
    __try
    {
      unsigned char const* pTable = *(unsigned char const**)(pZoneStreamer + ZoneTableOffset);
      if ((__int64)pTable < 0x10000)
        return -1;
      unsigned char const* pEntry = pTable + (__int64)zone * 8;
      return *(unsigned short const*)(pEntry + 2) != 0 && !(pEntry[0] & ZoneLoadingFlag) ? 1 : 0;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      return -1;
    }
  }

  // The game's own first focus (the game's camera, next to Faith), x and z
  bool ReadGameFocusPoint(__int64 pZoneStreamer, float& x, float& z)
  {
    __try
    {
      __int64 pCache = *(__int64*)(pZoneStreamer + FocusCacheOffset);
      if (pCache < 0x10000 || !*(unsigned int*)(pZoneStreamer + ZoneFocusListOffset + ZoneFocusCountOffset))
        return false;
      x = *(float*)pCache;
      z = *(float*)(pCache + 8);
      // Not placed yet at a level load: FLT_MAX there (seen 2026-10-25 19:40)
      return fabsf(x) < 100000.f && fabsf(z) < 100000.f;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      return false;
    }
  }

  // Whether the real tower is kept out for the view (the free camera, with
  // it off the game's own camera): it is unless the view is out of the city's
  // area and in the area around the Shard, each changing sides only
  // CityEdgeMargin past its edge. From the zone streamer hook (each streamer
  // asks; the answer is the same).
  bool UpdateShardViewer(float x, float z, bool freeCamera)
  {
    auto side = [](std::atomic<int>& state, float depth) {
      int was = state;
      int in = was < 0 ? (depth >= 0.f ? 1 : 0) : was == 1 ? (depth > -CityEdgeMargin ? 1 : 0) : (depth > CityEdgeMargin ? 1 : 0);
      state = in;
      return in != was;
    };
    bool cityTurned = side(g_ShardViewerInCity, CityAreaDepth(x, z));
    side(g_ShardViewerNearShard, ShardAreaDepth(x, z));
    int allowed = g_ShardViewerInCity == 0 && g_ShardViewerNearShard == 1 ? 1 : 0;
    const char* who = freeCamera ? "free camera" : "game's camera";
    if (g_ShardTowerAllowed.exchange(allowed) != allowed)
    {
      if (allowed)
        util::log::Write("The Shard: the %s is out of the city's area, in the area around the Shard (%.0f, %.0f): its real "
          "tower can load, and its far-away model goes once it has", who, x, z);
      else
      {
        g_ShardInCitySince = GetTickCount64();
        util::log::Write("The Shard: the %s is %s (%.0f, %.0f): its real tower is kept out once its far-away model is back, "
          "which shows", who, g_ShardViewerInCity == 1 ? "in the city's area" : "out of the city's area but not in the area "
          "around the Shard", x, z);
      }
    }
    else if (cityTurned && !allowed && g_ShardViewerInCity == 0)
      util::log::Write("The Shard: the %s is out of the city's area (%.0f, %.0f), but not in the area around the Shard: its "
        "real tower stays kept out", who, x, z);
    return allowed == 0;
  }

  // One model group with members whose mesh is one of a set (the far-away
  // model's, the lair's closed blinds, ...): which members, how many
  // instances each
  struct ModelGroupMatch
  {
    __int64 Group;
    __int64 Data;
    int Members[MaxShardLodMembers];
    int Instances[MaxShardLodMembers];
    int Count;
  };
  typedef bool (*MeshMatch)(const char* pName);   // game memory: only inside __try

  bool HasShardLodName(const char* pName)
  {
    if ((__int64)pName < 0x10000)
      return false;
    size_t length = sizeof(ShardLodMesh) - 1;
    for (size_t i = 0; pName[i] && i < 256; ++i)
    {
      size_t n = 0;
      while (n < length && pName[i + n] && tolower((unsigned char)pName[i + n]) == ShardLodMesh[n])
        ++n;
      if (n == length)
        return true;
    }
    return false;
  }

  // Whether a mesh's name (its last part, any case) is one of these
  bool MeshNameIs(const char* pName, const char* const* names, int count)
  {
    if ((__int64)pName < 0x10000)
      return false;
    const char* pLast = pName;
    for (size_t i = 0; pName[i] && i < 512; ++i)
      if (pName[i] == '/') pLast = pName + i + 1;
    for (int k = 0; k < count; ++k)
    {
      size_t n = 0;
      while (names[k][n] && pLast[n] && tolower((unsigned char)pLast[n]) == names[k][n])
        ++n;
      if (!names[k][n] && !pLast[n])
        return true;
    }
    return false;
  }

  // The lair's closed blinds (the train station, buildings zone 8: 14 of the
  // 512 and 6 of the 256, one in front of each of its animated windows) and
  // the Shard's train station gates and windows (zone 55: 8 gates, 2 side
  // windows, the exterior window); 2026-10-25, game data
  const char* const LairBlindsMeshes[] = { "lairblinds_256_mesh", "lairblinds_512_mesh" };
  const char* const ShardStationMeshes[] = { "skycity_buildings_trainstation_gate_mesh",
    "skycity_buildings_trainstation_sidewindow_mesh", "skycity_buildings_trainstation_exteriorwindow_mesh" };
  bool IsLairBlindsMesh(const char* pName) { return MeshNameIs(pName, LairBlindsMeshes, (int)_countof(LairBlindsMeshes)); }
  bool IsShardStationMesh(const char* pName) { return MeshNameIs(pName, ShardStationMeshes, (int)_countof(ShardStationMeshes)); }

  // Reads one group: false if it isn't one or has none of the members
  // wanted. Game memory, so only inside __try.
  bool ReadModelGroup(__int64 group, __int64 vtable, MeshMatch match, ModelGroupMatch& out)
  {
    if (*(__int64*)group != vtable)
      return false;
    __int64 data = *(__int64*)(group + ModelGroupDataOffset);
    int count = *(int*)(group + ModelGroupMemberCountOffset);
    if (data < 0x10000 || count <= 0 || count > MaxGroupMembers)
      return false;
    __int64 members = *(__int64*)(data + GroupDataMembersOffset);
    if (members < 0x10000)
      return false;
    out.Group = group;
    out.Data = data;
    out.Count = 0;
    for (int k = 0; k < count && out.Count < MaxShardLodMembers; ++k)
    {
      __int64 member = members + (__int64)k * GroupMemberDataSize;
      __int64 mesh = *(__int64*)(member + MemberMeshOffset);
      if (mesh < 0x10000 || !match(*(const char**)(mesh + AssetNameOffset)))
        continue;
      int instances = *(int*)(member + MemberInstanceCountOffset);
      if (instances <= 0 || instances > MaxShardLodInstances)
        continue;
      out.Members[out.Count] = k;
      out.Instances[out.Count] = instances;
      ++out.Count;
    }
    return out.Count > 0;
  }

  // The groups with such members, from the model groups' instance lists
  int FindModelGroups(MeshMatch match, ModelGroupMatch* groups, int max)
  {
    __int64 base = (__int64)g_gameHandle;
    int found = 0;
    __try
    {
      for (int list = 0; list < 4 && found < max; ++list)
      {
        __int64 link = *(__int64*)(base + ModelGroupClassRva + ClassInstanceListsOffset + list * 0x10);
        for (int n = 0; link >= 0x10000 && n < 20000 && found < max; ++n)
        {
          if (ReadModelGroup(link - EntityLinkOffset, base + ModelGroupVTableRva, match, groups[found]))
            ++found;
          link = *(__int64*)link;
        }
      }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
    return found;
  }

  // Whether a group the tools found is still that group (its zone or
  // sub-level can stream out, and another group be made where it was)
  bool StillModelGroup(ModelGroupMatch const& known, MeshMatch match)
  {
    ModelGroupMatch now;
    __try
    {
      if (!ReadModelGroup(known.Group, (__int64)g_gameHandle + ModelGroupVTableRva, match, now))
        return false;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      return false;
    }
    if (now.Data != known.Data || now.Count != known.Count)
      return false;
    for (int m = 0; m < now.Count; ++m)
      if (now.Members[m] != known.Members[m] || now.Instances[m] != known.Instances[m]) return false;
    return true;
  }

  bool SetModelGroupVisible(ModelGroupMatch const& group, bool visible)
  {
    bool ok = true;
    for (int m = 0; m < group.Count; ++m)
      for (int i = 0; i < group.Instances[m]; ++i)
        ok &= util::hooks::SetModelGroupMemberVisible(group.Group, group.Members[m], i, visible);
    return ok;
  }

  // The far-away model's
  typedef ModelGroupMatch ShardLodGroup;
  int FindShardLodGroups(ShardLodGroup* groups, int max) { return FindModelGroups(HasShardLodName, groups, max); }
  bool StillShardLodGroup(ShardLodGroup const& known) { return StillModelGroup(known, HasShardLodName); }
  bool SetShardLodVisible(ShardLodGroup const& group, bool visible) { return SetModelGroupVisible(group, visible); }

  // Models hidden while a menu choice says so (the lair's blinds open, the
  // Shard's station open): looked for every ModelHideSearchMs (their zones
  // come and go), each hidden group shown again when the choice is off.
  // Camera hook only.
  struct ModelHide
  {
    const char* What;       // for the log
    MeshMatch Match;
    std::vector<ModelGroupMatch> Hidden;
    ULONGLONG LastSearch;
    bool On;
  };
  const ULONGLONG ModelHideSearchMs = 2000;

  void UpdateModelHide(ModelHide& hide, bool wanted)
  {
    static bool s_build = IsRenderArenaBuild();
    if (!s_build)
      return;
    ULONGLONG now = GetTickCount64();
    if (!wanted)
    {
      if (hide.On)
      {
        int shown = 0;
        for (ModelGroupMatch const& group : hide.Hidden)
          shown += StillModelGroup(group, hide.Match) && SetModelGroupVisible(group, true) ? 1 : 0;
        util::log::Write("%s shown again (%d model groups)", hide.What, shown);
        hide.Hidden.clear();
        hide.On = false;
      }
      return;
    }
    if (hide.On && now - hide.LastSearch < ModelHideSearchMs)
      return;
    hide.LastSearch = now;
    hide.Hidden.erase(std::remove_if(hide.Hidden.begin(), hide.Hidden.end(),
      [&](ModelGroupMatch const& group) { return !StillModelGroup(group, hide.Match); }), hide.Hidden.end());
    ModelGroupMatch found[MaxShardLodGroups];
    int count = FindModelGroups(hide.Match, found, MaxShardLodGroups);
    int added = 0, models = 0;
    for (int f = 0; f < count; ++f)
    {
      bool known = false;
      for (ModelGroupMatch const& group : hide.Hidden)
        known |= group.Group == found[f].Group && group.Data == found[f].Data;
      if (known)
        continue;
      if (!SetModelGroupVisible(found[f], false))
      {
        static bool s_failLogged = false;
        if (!s_failLogged)
        {
          s_failLogged = true;
          util::log::Warning("%s can't be hidden (model group 0x%I64X): the model groups' code isn't what the tools know",
            hide.What, found[f].Group);
        }
        SetModelGroupVisible(found[f], true);
        continue;
      }
      hide.Hidden.push_back(found[f]);
      ++added;
      for (int m = 0; m < found[f].Count; ++m)
        models += found[f].Instances[m];
    }
    if (added)
      util::log::Write("%s hidden: %d models in %d model groups (%d hidden in all)", hide.What, models, added,
        (int)hide.Hidden.size());
    else if (!hide.On)
      util::log::Write("%s: none loaded now (they're hidden when their zone loads)", hide.What);
    hide.On = true;
  }

  ModelHide g_LairBlindsHide = { "The lair's closed blinds (train station)", IsLairBlindsMesh, {}, 0, false };
  ModelHide g_ShardStationHide = { "The Shard station's gates and windows", IsShardStationMesh, {}, 0, false };

  // The lair's two windows that open (2026-10-27, game data). Each window of
  // the Runners' lair (the old train station, buildings zone 8) is a frame in
  // the wall with a sash in it: the sash's own frame, its blinds and two
  // glass panes (its mesh's sections Frame, Aluminium, windowInner,
  // windowOuter). 20 windows are one model each, shut (lairblinds_256 /
  // _512, their glass apart: lairblindswindow). The other two have only the
  // frame in the zone (lairblinds_256_frame, its 2 instances), and their sash
  // is a model of its own (lairblinds_256_open), put in by the train
  // station's prefab (pf_dt_trainstation) by the story:
  // - by the curtain (hinge at -18.28, 69.25, 273.73): swung 45 degrees out
  //   (#5) or shut (#7), one streamed in and the other out by CompareBool #71
  //   (a progression flag, a mission's status and the install progress,
  //   Math #76);
  // - the other (frame at -15.24, 69.25, 317.61): pf_dt_lairblindscombatarea01
  //   (#3) has it shut, or "broken" (lairblinds_256_broken: the same sash,
  //   vertices and materials, with its origin on the room's side: hinged
  //   0.32 m further in) and turned into the room by 0, 17 or 129 degrees
  //   (CompareBool #8 and PropertySelect #13 on its BlueprintTransform, from
  //   progression flags).
  // Ticked, a sash found at either hinge is turned shut about its hinge: the
  // rows of the game's own shut sash (#7), or of the combat area itself
  // (where the broken sash fills the frame), its position kept. Only what's
  // drawn moves, as the replay moves models (ReplayModels.cpp), not its
  // collision. Looked for every LairWindowsSearchMs while the free camera or
  // Faith is within LairWindowsNear; off, the ones still as the tools left
  // them get the game's transform back. Camera hook only.
  struct LairSash
  {
    const char* Label;    // its mesh's file name without "_Mesh" (attach's label)
    float Hinge[3];       // where the data puts it
    float Rows[3][3];     // shut: right, up, forward
    const char* What;
  };
  // The last one is always shut (the combat area's whole sash): only counted
  const LairSash LairSashes[3] = {
    { "lairblinds_256_open", { -18.28095f, 69.25327f, 273.72645f },
      { { 0.8325976f, 0.f, -0.5538781f }, { 0.f, 1.f, 0.f }, { 0.5538781f, 0.f, 0.8325976f } },
      "the window by the curtain" },
    { "lairblinds_256_broken", { -14.99058f, 69.25326f, 317.40748f },
      { { -0.6455658f, 0.f, -0.7637047f }, { 0.f, 1.f, 0.f }, { 0.7637047f, 0.f, -0.6455658f } },
      "the window by the combat area (its broken sash)" },
    { "lairblinds_256_open", { -15.23497f, 69.25326f, 317.61406f },
      { { -0.6455658f, 0.f, -0.7637047f }, { 0.f, 1.f, 0.f }, { 0.7637047f, 0.f, -0.6455658f } },
      "the window by the combat area" } };
  const int LairSashCount = (int)_countof(LairSashes);
  const float LairSashFindDistance = 0.3f;
  const float LairWindowsMiddle[3] = { -16.64f, 71.49f, 295.57f };  // between the hinges, at the sashes' middle height
  const float LairWindowsScanRadius = 30.f;   // both hinges are 22 m from the middle
  const float LairWindowsNear = 250.f;
  const ULONGLONG LairWindowsSearchMs = 2000;
  const int MaxLairWindowCandidates = 4096;
  const float ShutEpsilon = 1e-3f;

  // The static models' getter and setter (vtable slots 24 and 25) and the
  // flags that make the setter move only what's drawn, as in ReplayModels.cpp
  const __int64 ModelGetTransformRva = 0x32E1440;
  const __int64 ModelSetTransformRva = 0x32E8300;
  const int ModelFlagsOffset = 0x18;
  const unsigned int ModelDeferredFlag = 1u << 21;
  const unsigned int ModelPhysicsFlag = 1u << 27;
  typedef void(__fastcall* tModelSetTransform)(__int64, float const*);

  bool IsModelSetterCode()
  {
    BYTE const* p = (BYTE const*)g_gameHandle + ModelSetTransformRva;
    // mov r8b, 1; jmp rel32 -> 0x1432EB1F0, or the tools' own hook there
    return util::hooks::IsModelSetTransformHooked() || (p[0] == 0x41 && p[1] == 0xB0 && p[2] == 0x01 && p[3] == 0xE9 &&
      ModelSetTransformRva + 3 + 5 + *(int const*)(p + 4) == 0x32EB1F0);
  }

  // Still the same model, with the static models' getter and setter; then
  // moved at once: flag bit 21 (queued) cleared for the call, so the setter
  // writes the rows and runs the change handler (slot 28, 0x143193310) now.
  // Drawn only: bit 27 cleared too. With its collision: bit 27 as the game
  // has it, and the handler's 0x1431CBCD0 passes the change on to the
  // model's physics component at + 0x60 while it's set (its slot 30,
  // 0x1434E2470: each of its bodies). Not with bit 21 left set: the queue
  // runs the handler later in the frame, which then skips the collision of a
  // queued model (0x143193607: bit 27 cleared round 0x1431CBCD0). The setter
  // (0x1432EB1F0) reads the rows with movaps: given an aligned copy.
  bool SetModel(__int64 entity, __int64 vtable, float const* transform, bool withCollision)
  {
    __int64 base = (__int64)g_gameHandle;
    alignas(16) float rows[16];
    memcpy(rows, transform, sizeof(rows));
    unsigned int* flags = (unsigned int*)(entity + ModelFlagsOffset);
    unsigned int mask = withCollision ? ModelDeferredFlag : (ModelDeferredFlag | ModelPhysicsFlag);
    unsigned int before = 0;
    bool cleared = false;
    __try
    {
      if (*(__int64 const*)entity != vtable || *(__int64 const*)(vtable + 24 * 8) != base + ModelGetTransformRva ||
        *(__int64 const*)(vtable + 25 * 8) != base + ModelSetTransformRva)
        return false;
      before = *flags;
      *flags = before & ~mask;
      cleared = true;
      ((tModelSetTransform)(base + ModelSetTransformRva))(entity, rows);
      *flags = (*flags & ~mask) | (before & mask);
      return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      if (cleared)
        *flags = (*flags & ~mask) | (before & mask);
      return false;
    }
  }

  // The model's flags (for the log: bits 21 queued, 27 collision follows)
  unsigned int ModelFlags(__int64 entity)
  {
    __try
    {
      return *(unsigned int const*)(entity + ModelFlagsOffset);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      return 0;
    }
  }

  // A static model's collision bodies in the game's physics (Havok, hknp).
  // Moving the model doesn't move them: its physics component's slot 30
  // (0x1434E2470) only passes the change on to the spatial queries (each
  // record's + 0x40, -1 for a body not in them), and nothing in the game
  // moves a Fixed body after it's made. Read live 2026-10-27 03:5x with the
  // sashes shut: their records followed (record + 0xC8 is the model's
  // transform), their Havok bodies stayed where the game made them (body 374
  // still swung 45 degrees, 2054 still turned 129; their effect-world copies
  // 254 / 1715 too), and Faith walked through. So the tools move the bodies
  // themselves, the way a body record's own slot 48 (0x1434F1DF0, setting a
  // body's position and rotation) does for a body it doesn't keyframe:
  // hknpWorld::setBodyTransform (0x143F3A6B0: Havok world, body id, rows,
  // activation 1). The component at model + 0x60: its body count word +
  // 0x60, ids [+ 0x30], manager + 0x58; a body's record
  // [[[manager + 0xF0] + ((id >> 16) & 0xFF) * 8] + 0x130][id & 0xFFFF]
  // (0x1434EE790 / 0x1434EE750), vtable 0x141BAC160: its Havok world
  // [[record + 0x30] + 0xE0], its Havok id + 0x58, below the world's
  // capacity (+ 0x2C, low 30 bits); the bodies at [world + 0x20], 0x90 bytes
  // each, their rows first (the same layout as the game's: right, up,
  // forward, position; Havok keeps its own w lanes, 0x143F3A8D8 ..).
  const __int64 BodyRecordVTableRva = 0x1BAC160;
  const __int64 BodySetPoseRva = 0x34F1DF0;
  const __int64 SetBodyTransformRva = 0x3F3A6B0;
  const int ModelPhysicsOffset = 0x60;
  const int HavokBodySize = 0x90;
  typedef void(__fastcall* tSetBodyTransform)(__int64, unsigned int, float const*, int);

  bool HavokBodyCodeKnown()
  {
    BYTE const* base = (BYTE const*)g_gameHandle;
    static const BYTE head[] = { 0x89, 0x54, 0x24, 0x10, 0x55, 0x53, 0x56, 0x57, 0x41, 0x57, 0x48, 0x89, 0xE5, 0x48, 0x83, 0xEC };
    static const BYTE world[] = { 0x48, 0x8B, 0xB8, 0xE0, 0x00, 0x00, 0x00, 0x48, 0x63, 0x41, 0x58 };  // its Havok world, its id
    static const BYTE owner[] = { 0x48, 0x8B, 0x41, 0x30 };
    static const BYTE activation[] = { 0x41, 0xB9, 0x0F, 0xA2, 0x4F, 0xC3 };                     // + 0x3CB05DF2: 1
    static const BYTE worldOf[] = { 0x8B, 0x02, 0xC1, 0xE8, 0x10, 0x0F, 0xB6, 0xD0, 0x48, 0x8B, 0x81, 0xF0, 0x00, 0x00, 0x00 };
    static const BYTE recordOf[] = { 0x0F, 0xB7, 0x12, 0x48, 0x8B, 0x81, 0x30, 0x01, 0x00, 0x00 };
    BYTE const* call = base + 0x34F1ED6;
    return IsRenderArenaBuild() && !memcmp(base + SetBodyTransformRva, head, sizeof(head)) &&
      !memcmp(base + 0x34F1E37, world, sizeof(world)) && !memcmp(base + 0x34F1E17, owner, sizeof(owner)) &&
      !memcmp(base + 0x34F1EC6, activation, sizeof(activation)) && call[0] == 0xE8 &&
      0x34F1ED6 + 5 + *(int const*)(call + 1) == SetBodyTransformRva &&
      !memcmp(base + 0x34EE790, worldOf, sizeof(worldOf)) && !memcmp(base + 0x34EE750, recordOf, sizeof(recordOf)) &&
      *(__int64 const*)(base + BodyRecordVTableRva + 48 * 8) == (__int64)base + BodySetPoseRva;
  }

  // The model's Havok bodies put at the rows: each one only if it's where
  // the model is (within 0.5 m). How many moved, -1 on a fault.
  int MoveModelBodies(__int64 entity, float const* transform)
  {
    __int64 base = (__int64)g_gameHandle;
    alignas(16) float rows[16];
    memcpy(rows, transform, sizeof(rows));
    int moved = 0;
    __try
    {
      __int64 component = *(__int64*)(entity + ModelPhysicsOffset);
      if (component < 0x10000)
        return 0;
      int count = *(unsigned short const*)(component + 0x60);
      __int64 ids = *(__int64*)(component + 0x30);
      __int64 manager = *(__int64*)(component + 0x58);
      if (count <= 0 || count > 8 || ids < 0x10000 || manager < 0x10000)
        return 0;
      __int64 worlds = *(__int64*)(manager + 0xF0);
      for (int i = 0; i < count; ++i)
      {
        unsigned int id = *(unsigned int const*)(ids + i * 4);
        __int64 world = *(__int64*)(worlds + (__int64)((id >> 16) & 0xFF) * 8);
        __int64 records = world >= 0x10000 ? *(__int64*)(world + 0x130) : 0;
        __int64 record = records >= 0x10000 ? *(__int64*)(records + (__int64)(id & 0xFFFF) * 8) : 0;
        if (record < 0x10000 || *(__int64 const*)record != base + BodyRecordVTableRva)
          continue;
        __int64 owner = *(__int64*)(record + 0x30);
        __int64 havok = owner >= 0x10000 ? *(__int64*)(owner + 0xE0) : 0;
        int body = *(int const*)(record + 0x58);
        if (havok < 0x10000 || body < 0 || body >= (int)(*(unsigned int const*)(havok + 0x2C) & 0x3FFFFFFF))
          continue;
        float const* now = (float const*)(*(__int64*)(havok + 0x20) + (__int64)body * HavokBodySize);
        float dx = now[12] - rows[12], dy = now[13] - rows[13], dz = now[14] - rows[14];
        if (dx * dx + dy * dy + dz * dz > 0.25f)
          continue;
        ((tSetBodyTransform)(base + SetBodyTransformRva))(havok, (unsigned int)body, rows, 1);
        ++moved;
      }
      return moved;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      return -1;
    }
  }

  bool SameTransform(float const* a, float const* b)
  {
    for (int i = 0; i < 16; ++i)
      if (fabsf(a[i] - b[i]) > ShutEpsilon)
        return false;
    return true;
  }

  // The sash's transform shut: its rows, its own position (and w's) kept
  void ShutTransform(float const* now, int sash, float* out)
  {
    memcpy(out, now, 16 * sizeof(float));
    for (int r = 0; r < 3; ++r)
      for (int c = 0; c < 3; ++c)
        out[r * 4 + c] = LairSashes[sash].Rows[r][c];
  }

  // How far it's turned from shut, degrees (about the vertical)
  float TurnedFromShut(float const* now, int sash)
  {
    float const* shut = LairSashes[sash].Rows[0];
    float dot = now[0] * shut[0] + now[1] * shut[1] + now[2] * shut[2];
    return acosf(dot < -1.f ? -1.f : (dot > 1.f ? 1.f : dot)) * 57.29578f;
  }

  bool NearLairWindows(float x, float y, float z)
  {
    float dx = x - LairWindowsMiddle[0], dy = y - LairWindowsMiddle[1], dz = z - LairWindowsMiddle[2];
    return dx * dx + dy * dy + dz * dz < LairWindowsNear * LairWindowsNear;
  }

  // A shut sash is solid: its collision moves with it, its records (SetModel
  // with its collision) and its Havok bodies (MoveModelBodies), in the
  // client's physics, the one Faith walks in (the sofa window's server copy
  // is shut in the game's data, PropertySelect #13 being Realm_Client, and she
  // walked through it there: user, 2026-10-27; solid this way, checked by the
  // user the same day, so it's no longer a menu choice of its own). The
  // server's copy stays as the game has it. Only while Faith is
  // LairSashClearance or more across from the hinge, so no sash's collision
  // swings into her; a nearer one is shut drawn only and made solid once
  // she's clear.
  const float LairSashClearance = 4.f;

  struct ShutSash
  {
    attach::Target Target;
    int Sash;
    bool Solid;               // its collision moved with it (its Havok bodies at Written)
    bool SolidFailed;         // tried, no body moved
    float Original[16];       // the game's transform
    float Written[16];
    float BodyOriginal[16];   // where the game made its bodies (it never moves them)
  };

  // Its collision shut too (the rows written)
  int g_SashBodiesMoved = 0;   // by the last SetSashSolid, for the log
  bool SetSashSolid(ShutSash& shut)
  {
    bool ok = SetModel(shut.Target.Entity, shut.Target.VTable, shut.Written, true);
    int moved = ok ? MoveModelBodies(shut.Target.Entity, shut.Written) : 0;
    g_SashBodiesMoved = moved;
    shut.Solid = moved > 0;
    return shut.Solid;
  }
  std::vector<ShutSash> g_ShutSashes;
  ULONGLONG g_LairWindowsLastSearch = 0;
  bool g_LairWindowsOn = false;
  int g_LairWindowsLogged = -1;   // at the last log line: shut by the tools * 100 + solid * 10 + already shut
  int g_LairWindowsBack = 0;      // turned back since the tick went off
  bool g_LairWindowsWaitLogged = false;

  // Faith clear of the sash's swing (not known: not clear)
  bool ClearOfSash(int sash, bool hasFaith, float const* faith)
  {
    if (!hasFaith)
      return false;
    float dx = faith[0] - LairSashes[sash].Hinge[0], dy = faith[1] - LairSashes[sash].Hinge[1],
      dz = faith[2] - LairSashes[sash].Hinge[2];
    return dx * dx + dz * dz > LairSashClearance * LairSashClearance || dy < -3.f || dy > 6.f;
  }

  void UpdateLairWindows(bool wanted, bool nearLair, __int64 soldier, bool hasFaith, float const* faith)
  {
    static bool s_code = IsRenderArenaBuild() && IsModelSetterCode();
    static bool s_havok = HavokBodyCodeKnown();
    bool solid = s_havok;
    if (wanted && !s_havok)
    {
      static bool s_havokLogged = false;
      if (!s_havokLogged)
      {
        s_havokLogged = true;
        util::log::Warning("The lair's shut windows can't be made solid: the game's physics code isn't what the tools know "
          "(they're shut, drawn only)");
      }
    }
    if (!wanted)
    {
      if (!g_LairWindowsOn)
        return;
      // Back as the game has them: the ones still as the tools left them, a
      // solid one once Faith is clear of it (each frame till then)
      int waiting = 0;
      for (size_t i = 0; i < g_ShutSashes.size();)
      {
        ShutSash& shut = g_ShutSashes[i];
        XMFLOAT4X4 now;
        bool asLeft = attach::ReadTransform(shut.Target, now) && SameTransform(&now._11, shut.Written);
        if (asLeft && shut.Solid && !ClearOfSash(shut.Sash, hasFaith, faith))
        {
          ++waiting;
          ++i;
          continue;
        }
        if (asLeft && SetModel(shut.Target.Entity, shut.Target.VTable, shut.Original, shut.Solid))
        {
          if (shut.Solid)
            MoveModelBodies(shut.Target.Entity, shut.BodyOriginal);
          ++g_LairWindowsBack;
        }
        g_ShutSashes.erase(g_ShutSashes.begin() + i);
      }
      if (waiting)
      {
        if (!g_LairWindowsWaitLogged)
        {
          g_LairWindowsWaitLogged = true;
          util::log::Write("The lair's open windows: %d solid sashes turn back once Faith is %.0f m from them", waiting,
            LairSashClearance);
        }
        return;
      }
      util::log::Write("The lair's open windows: as the game has them (%d sashes the tools shut turned back)", g_LairWindowsBack);
      g_LairWindowsOn = false;
      g_LairWindowsLogged = -1;
      g_LairWindowsBack = 0;
      g_LairWindowsWaitLogged = false;
      return;
    }
    if (!s_code)
    {
      static bool s_codeLogged = false;
      if (!s_codeLogged)
      {
        s_codeLogged = true;
        util::log::Warning("The lair's open windows can't be shut: the static models' setTransform isn't the code the tools know");
      }
      return;
    }
    ULONGLONG now = GetTickCount64();
    if (g_LairWindowsOn && now - g_LairWindowsLastSearch < LairWindowsSearchMs)
      return;
    g_LairWindowsLastSearch = now;
    g_LairWindowsOn = true;
    g_LairWindowsBack = 0;
    g_LairWindowsWaitLogged = false;

    // The ones shut: gone with their zone (or streamed out by the story), or
    // turned by the game again (the broken sash's turn is the game's
    // property): that's the game's transform now, shut again. Solid as the
    // menu says, once Faith is clear.
    for (size_t i = 0; i < g_ShutSashes.size();)
    {
      ShutSash& shut = g_ShutSashes[i];
      XMFLOAT4X4 transform;
      if (!attach::ReadTransform(shut.Target, transform))
      {
        g_ShutSashes.erase(g_ShutSashes.begin() + i);
        continue;
      }
      float const* m = &transform._11;
      const char* what = LairSashes[shut.Sash].What;
      bool clear = ClearOfSash(shut.Sash, hasFaith, faith);
      if (!SameTransform(m, shut.Written))
      {
        // The game's turn moves what's drawn, not its Havok bodies: those
        // stay as they were (shut if solid)
        float turned = TurnedFromShut(m, shut.Sash);
        memcpy(shut.Original, m, sizeof(shut.Original));
        ShutTransform(m, shut.Sash, shut.Written);
        bool ok = SetModel(shut.Target.Entity, shut.Target.VTable, shut.Written, false);
        util::log::Write("The lair's open windows: %s was turned again by the game (%.0f degrees from shut): %s", what, turned,
          ok ? "shut again" : "couldn't be shut");
      }
      if (solid && !shut.Solid && !shut.SolidFailed && clear)
      {
        // Made solid now that Faith is clear of it
        bool ok = SetSashSolid(shut);
        util::log::Write("The lair's open windows: %s %s (%d Havok bodies moved)", what, ok ? "made solid" :
          "couldn't be made solid (no Havok body of its own where it is)", g_SashBodiesMoved);
        shut.SolidFailed = !ok;   // not tried again each look; the log says why
      }
      ++i;
    }
    if (!nearLair)
      return;

    bool fromFaith = false;
    if (!attach::FindClasses(soldier, fromFaith))
    {
      static bool s_classesLogged = false;
      if (!s_classesLogged)
      {
        s_classesLogged = true;
        util::log::Warning("The lair's open windows: the game's entity classes weren't found, its sashes can't be looked for");
      }
      return;
    }
    attach::View view{};
    view.Camera = XMFLOAT3(LairWindowsMiddle[0], LairWindowsMiddle[1], LairWindowsMiddle[2]);
    view.Look = XMFLOAT3(0.f, 0.f, 1.f);
    view.Skip = soldier;
    static std::vector<attach::Target> found(MaxLairWindowCandidates);
    std::unique_ptr<attach::Scan> scan = std::make_unique<attach::Scan>();
    int count = attach::ScanNearby(view, LairWindowsScanRadius, found.data(), (int)found.size(), *scan);
    int alreadyShut = 0;
    for (int i = 0; i < count; ++i)
    {
      attach::Target& target = found[i];
      const char* className = attach::ClassName(target.Class);
      if (strcmp(className, "ClientStaticModelEntity") && strcmp(className, "PamClientStaticModelEntity"))
        continue;
      int sash = -1;
      for (int k = 0; k < LairSashCount && sash < 0; ++k)
      {
        float dx = target.Position.x - LairSashes[k].Hinge[0], dy = target.Position.y - LairSashes[k].Hinge[1],
          dz = target.Position.z - LairSashes[k].Hinge[2];
        if (!_stricmp(target.Label, LairSashes[k].Label) && dx * dx + dy * dy + dz * dz < LairSashFindDistance * LairSashFindDistance)
          sash = k;
      }
      if (sash < 0)
        continue;
      bool known = false;
      for (ShutSash const& shut : g_ShutSashes)
        known |= shut.Target.Entity == target.Entity;
      if (known)
        continue;
      XMFLOAT4X4 transform;
      if (!attach::ReadTransform(target, transform))
        continue;
      float const* m = &transform._11;
      float written[16];
      ShutTransform(m, sash, written);
      if (SameTransform(m, written))
      {
        ++alreadyShut;
        continue;
      }
      float turned = TurnedFromShut(m, sash);
      if (!SetModel(target.Entity, target.VTable, written, false))
      {
        util::log::Warning("The lair's open windows: %s (model 0x%I64X) couldn't be shut: not a static model the tools know",
          LairSashes[sash].What, target.Entity);
        continue;
      }
      ShutSash shut{};
      shut.Target = target;
      shut.Sash = sash;
      memcpy(shut.Original, m, sizeof(shut.Original));
      memcpy(shut.Written, written, sizeof(shut.Written));
      memcpy(shut.BodyOriginal, m, sizeof(shut.BodyOriginal));
      bool withCollision = solid && ClearOfSash(sash, hasFaith, faith) && SetSashSolid(shut);
      shut.SolidFailed = solid && ClearOfSash(sash, hasFaith, faith) && !withCollision;
      g_ShutSashes.push_back(shut);
      util::log::Write("The lair's open windows: %s shut, %s (its sash %s turned %.0f degrees about its hinge at %.2f, %.2f, "
        "%.2f; model 0x%I64X, flags 0x%08X; %d Havok bodies moved)", LairSashes[sash].What, withCollision ? "solid" :
        shut.SolidFailed ? "drawn only (no Havok body of its own where it is)" : solid ?
        "drawn only till Faith is clear of it" : "drawn only", target.Label, turned, m[12], m[13], m[14], target.Entity,
        ModelFlags(target.Entity), withCollision || shut.SolidFailed ? g_SashBodiesMoved : 0);
    }
    int solidCount = 0;
    for (ShutSash const& shut : g_ShutSashes)
      solidCount += shut.Solid ? 1 : 0;
    int state = (int)g_ShutSashes.size() * 100 + solidCount * 10 + alreadyShut;
    if (state != g_LairWindowsLogged)
    {
      g_LairWindowsLogged = state;
      if (g_ShutSashes.empty() && !alreadyShut)
        util::log::Write("The lair's open windows: no sash of theirs loaded now (%d entities within %.0f m of them; shut "
          "when the lair loads)", count, LairWindowsScanRadius);
      else
        util::log::Write("The lair's open windows: %d sashes shut by the tools (%d solid), %d already shut as the game has them",
          (int)g_ShutSashes.size(), solidCount, alreadyShut);
    }
  }

  // The Shard's fans. The far-away sub-level has two spinning fans
  // (skycity_building_fan01) placed in the sky at 1.7 and 2.9 times their
  // size; the real tower's zone has none: the Shard mission places its own
  // (theshard/layer1_theshard_logic: an intact one, pf_windfarm_propeller_
  // skycity, and a broken one, pf_backdrop_windfarm_propellerbroken_ingame,
  // a different model). Up close the tools move the far-away pair onto the
  // mission's two places, at the mission's size there, so the far-away
  // sub-level can stay (its far-away tower is hidden as before). The one on
  // the broken fan's place is an intact fan. The sizes (2026-10-26, game
  // data): the intact one is skycity_building_fan01 at 0.98
  // (pf_windfarm_propeller_skycity_blades); the broken one is
  // skycity_building_fanbroken01 at 0.588 (pf_backdrop_windfarm_
  // propellerbroken_ingame > _skycity > _skycity_realdeal), the same hub and
  // blades as fan01 (their boxes match but for the broken blade), so the fan
  // put there is 0.588 too (it was 0.98, 1.67 times the mission's: user,
  // 2026-10-26). Neither prefab moves its fan off the place: their timelines
  // only turn it about its z, as the rotation entity does. How the game spins
  // them (offline; read live 2026-10-25 22:4x): a ClientSimpleRotationEntity
  // (vtable 0x141A6E940) keeps, for each entity it spins, a record at
  // [e + 0x38, e + 0x40), 0x50 bytes: the entity, and at + 0x10 its base
  // transform (4 rows, world space); each frame its job (0x142D44080) gives
  // the entity base x spin through the entity's vtable + 0xC8. The client's
  // updater ([0x1423D55C8], vtable 0x141A6EA38, set up at 0x142D462D0) lists
  // the rotation entities at + 0x28 / + 0x30. So the base transform in the
  // record is where the fan is drawn: the tools write the mission's there,
  // and put the game's back.
  // There's one updater per realm (2026-11-03, offline): 0x142D462D0 takes
  // the slot [0x1423D55C8 + realm * 8], the realm bit 0 of the entity's
  // + 0x18 (its creation info's flags, 0x143160147), so both are searched
  // (only realm 0's was before: twice, with the camera at the city's
  // north-west edge, the far-away fans weren't in it). An entity is in its
  // updater from its set-up (0x142D48450, AutoStart, as the far-away pair
  // has) until a Pause or Stop (its event handler 0x142D45C50) or its end
  // (0x142D41B90); the updater applies adds and removes at the start of each
  // update (0x143173840), nothing else takes one out.
  const __int64 RotationUpdaterSlotRva = 0x23D55C8;
  const int RotationRealms = 2;
  const __int64 RotationUpdaterVTableRva = 0x1A6EA38;
  const __int64 RotationEntityVTableRva = 0x1A6E940;
  const int UpdaterEntitiesOffset = 0x28;     // begin, end
  const int RotationRecordsOffset = 0x38;     // begin, end
  const int RotationRecordSize = 0x50;
  const int RecordBaseOffset = 0x10;          // 4 rows of 4 floats
  const int StaticModelDataOffset = 0x30;     // a static model's data, its Mesh at + 0x98 (as AttachTargets)
  const int StaticModelMeshOffset = 0x98;
  const char FanMesh[] = "skycity_building_fan01_mesh";
  const float FanFindDistance = 25.f;         // the far-away fan this close to where the data puts it
  const ULONGLONG ShardFanCheckMs = 500;
  // Not both found this long while the far-away sub-level is loaded: it
  // streams out instead, fans and all (see CameraManager::OnCameraUpdate)
  const ULONGLONG ShardFanWaitMs = 5000;

  struct FanMove
  {
    float From[3];        // the far-away fan: where the far-away sub-level has it
    float Rows[3][3];     // the mission's fan: right, up, forward (without its scale)
    float Place[3];
    float Scale;          // the mission's fan's, inside its prefab
    const char* What;
  };
  const FanMove FanMoves[2] = {
    // 1.7 times, high up -> the intact fan (layer1_theshard_logic #133)
    { { 1065.72f, 3888.83f, -2911.28f },
      { { -0.947751f, 0.f, -0.319009f }, { 0.f, 1.f, 0.f }, { 0.319009f, 0.f, -0.947751f } },
      { 942.885f, 553.f, -2927.71f }, 0.98f, "the mission's intact fan" },
    // 2.9 times, just under the top -> the broken fan's place (#102)
    { { 1228.97f, 1356.55f, -2812.15f },
      { { -0.632765f, -0.0187366f, -0.774118f }, { -0.0118576f, 0.999824f, -0.0145069f }, { 0.774253f, 0.f, -0.632876f } },
      { 882.303f, 1432.02f, -2955.78f }, 0.588f, "the mission's broken fan (an intact one there)" } };

  // A rotation entity's clock: its job (0x142D44080) adds the time step to
  // it every update (+ 0x78, addss), then turns its records' entities by it;
  // Stop sets it to 0 (0x142D45C9E)
  const int RotationClockOffset = 0x78;

  struct MovedFan
  {
    __int64 Rotation;     // the rotation entity
    __int64 Record;
    __int64 Entity;       // the fan
    float Original[16];   // the game's base transform
    float Written[16];
    float Clock;          // its rotation entity's clock when last read
    bool Turning;         // the clock has moved since the tools moved it: the game draws it where they put it
  };
  std::vector<MovedFan> g_MovedFans;   // camera hook only

  bool ShardFanCodeKnown()
  {
    BYTE const* pBase = (BYTE const*)g_gameHandle;
    static const BYTE records[] = { 0x48, 0x8B, 0x77, 0x40, 0x48, 0x8B, 0x5F, 0x38 };   // mov rsi, [rdi + 0x40]; mov rbx, [rdi + 0x38]
    static const BYTE rows[] = { 0x48, 0x8D, 0x6B, 0x30 };                             // lea rbp, [rbx + 0x30]
    static const BYTE step[] = { 0x48, 0x83, 0xC3, 0x50, 0x48, 0x83, 0xC5, 0x50 };     // add rbx, 0x50; add rbp, 0x50
    static const BYTE setTransform[] = { 0x48, 0x8B, 0x01, 0xFF, 0x90, 0xC8, 0x00, 0x00, 0x00 }; // call [rax + 0xC8]
    static const BYTE slot[] = { 0x48, 0x8D, 0x0D };                                   // lea rcx, [rip + the updaters' slots]
    static const BYTE realm[] = { 0x8B, 0x71, 0xE8, 0x83, 0xE6, 0x01, 0x48, 0x63, 0xC6 }; // mov esi, [rcx - 0x18]; and esi, 1; movsxd rax, esi
    static const BYTE realmSlot[] = { 0x48, 0x8D, 0x3C, 0xC1 };                       // lea rdi, [rcx + rax * 8]
    BYTE const* pSlot = pBase + 0x2D462F1;
    return IsRenderArenaBuild() && !memcmp(pBase + 0x2D440F4, records, sizeof(records)) &&
      !memcmp(pBase + 0x2D4416D, rows, sizeof(rows)) && !memcmp(pBase + 0x2D44308, step, sizeof(step)) &&
      !memcmp(pBase + 0x2D442FA, setTransform, sizeof(setTransform)) && !memcmp(pSlot, slot, sizeof(slot)) &&
      !memcmp(pBase + 0x2D462E8, realm, sizeof(realm)) && !memcmp(pBase + 0x2D462F8, realmSlot, sizeof(realmSlot)) &&
      pSlot + 7 + *(int const*)(pSlot + 3) == pBase + RotationUpdaterSlotRva &&
      *(__int64 const*)(pBase + RotationEntityVTableRva) == (__int64)pBase + 0x2D46160;   // its getType
  }

  // The far-away fans' records: found[k] for FanMoves[k] (0 if not found),
  // from both realms' updaters; counts[realm] their rotation entities (-1 no
  // updater)
  struct FanFound
  {
    __int64 Rotation, Record, Entity;
  };
  void FindShardFansIn(int realm, FanFound* found, int& count)
  {
    __int64 base = (__int64)g_gameHandle;
    count = -1;
    __try
    {
      __int64 updater = *(__int64*)(base + RotationUpdaterSlotRva + realm * 8);
      if (updater < 0x10000 || *(__int64*)updater != base + RotationUpdaterVTableRva)
        return;
      __int64 begin = *(__int64*)(updater + UpdaterEntitiesOffset), end = *(__int64*)(updater + UpdaterEntitiesOffset + 8);
      if (begin < 0x10000 || end < begin || (end - begin) / 8 > 8192)
        return;
      count = (int)((end - begin) / 8);
      for (__int64 p = begin; p < end; p += 8)
      {
        __int64 rotation = *(__int64*)p;
        if (rotation < 0x10000 || *(__int64*)rotation != base + RotationEntityVTableRva)
          continue;
        __int64 rb = *(__int64*)(rotation + RotationRecordsOffset), re = *(__int64*)(rotation + RotationRecordsOffset + 8);
        if (rb < 0x10000 || re < rb || (re - rb) / RotationRecordSize > 64)
          continue;
        for (__int64 record = rb; record < re; record += RotationRecordSize)
        {
          __int64 entity = *(__int64*)record;
          __int64 data = entity >= 0x10000 ? *(__int64*)(entity + StaticModelDataOffset) : 0;
          __int64 mesh = data >= 0x10000 ? *(__int64*)(data + StaticModelMeshOffset) : 0;
          if (mesh < 0x10000)
            continue;
          const char* pName = *(const char**)(mesh + AssetNameOffset);
          const char* const names[] = { FanMesh };
          if (!MeshNameIs(pName, names, 1))
            continue;
          float const* m = (float const*)(record + RecordBaseOffset);
          for (int k = 0; k < 2; ++k)
          {
            float dx = m[12] - FanMoves[k].From[0], dy = m[13] - FanMoves[k].From[1], dz = m[14] - FanMoves[k].From[2];
            if (dx * dx + dy * dy + dz * dz < FanFindDistance * FanFindDistance)
              found[k] = { rotation, record, entity };
          }
        }
      }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
  }

  void FindShardFans(FanFound* found, int* counts)
  {
    for (int k = 0; k < 2; ++k)
      found[k] = { 0, 0, 0 };
    for (int realm = 0; realm < RotationRealms; ++realm)
    {
      FanFound inRealm[2] = {};
      FindShardFansIn(realm, inRealm, counts[realm]);
      for (int k = 0; k < 2; ++k)
        if (!found[k].Record && inRealm[k].Record)
          found[k] = inRealm[k];
    }
  }

  bool ReadBase(__int64 record, float* m)
  {
    __try
    {
      memcpy(m, (void const*)(record + RecordBaseOffset), 16 * sizeof(float));
      return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      return false;
    }
  }

  bool WriteBase(__int64 record, float const* m)
  {
    __try
    {
      memcpy((void*)(record + RecordBaseOffset), m, 16 * sizeof(float));
      return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      return false;
    }
  }

  // Whether a fan the tools moved is still that record, as they left it
  bool StillMovedFan(MovedFan const& fan)
  {
    __int64 base = (__int64)g_gameHandle;
    __try
    {
      if (*(__int64*)fan.Rotation != base + RotationEntityVTableRva || *(__int64*)fan.Record != fan.Entity)
        return false;
      __int64 rb = *(__int64*)(fan.Rotation + RotationRecordsOffset), re = *(__int64*)(fan.Rotation + RotationRecordsOffset + 8);
      if (fan.Record < rb || fan.Record >= re)
        return false;
      return !memcmp((void const*)(fan.Record + RecordBaseOffset), fan.Written, sizeof(fan.Written));
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      return false;
    }
  }

  bool ReadRotationClock(__int64 rotation, float& clock)
  {
    __try
    {
      clock = *(float const*)(rotation + RotationClockOffset);
      return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      return false;
    }
  }

  // Both moved and seen turning since
  bool ShardFansTurning()
  {
    if (g_MovedFans.size() != 2)
      return false;
    for (MovedFan const& fan : g_MovedFans)
      if (!fan.Turning)
        return false;
    return true;
  }

  // Up close (wanted): the far-away fans on the mission's places; else back
  // where the game had them. Gives whether both are on the mission's places
  // and seen turning there (their rotation entities' clocks moving: a
  // rotation entity that isn't updated leaves its fan where it was).
  bool UpdateShardFans(bool wanted, bool codeKnown)
  {
    static ULONGLONG s_last = 0;
    static bool s_missingLogged = false;
    ULONGLONG now = GetTickCount64();
    if (!codeKnown)
      return false;
    if (!wanted)
    {
      if (!g_MovedFans.empty())
      {
        int back = 0;
        for (MovedFan const& fan : g_MovedFans)
          back += StillMovedFan(fan) && WriteBase(fan.Record, fan.Original) ? 1 : 0;
        util::log::Write("The Shard: its far-away fans back where the game has them (%d of %d)", back, (int)g_MovedFans.size());
        g_MovedFans.clear();
      }
      s_missingLogged = false;
      return false;
    }
    if (now - s_last < ShardFanCheckMs)
      return ShardFansTurning();
    s_last = now;
    // The ones that went (the far-away sub-level streamed out, or another
    // version in) are forgotten
    g_MovedFans.erase(std::remove_if(g_MovedFans.begin(), g_MovedFans.end(),
      [](MovedFan const& fan) { return !StillMovedFan(fan); }), g_MovedFans.end());
    for (MovedFan& fan : g_MovedFans)
    {
      float clock = 0.f;
      if (!fan.Turning && ReadRotationClock(fan.Rotation, clock) && clock != fan.Clock)
        fan.Turning = true;
      fan.Clock = clock;
    }
    if (g_MovedFans.size() == 2)
      return ShardFansTurning();
    FanFound found[2];
    int counts[RotationRealms];
    FindShardFans(found, counts);
    for (int k = 0; k < 2; ++k)
    {
      if (!found[k].Record)
        continue;
      bool known = false;
      for (MovedFan const& fan : g_MovedFans)
        known |= fan.Record == found[k].Record;
      if (known)
        continue;
      MovedFan fan = { found[k].Rotation, found[k].Record, found[k].Entity };
      if (!ReadBase(fan.Record, fan.Original) || !ReadRotationClock(fan.Rotation, fan.Clock))
        continue;
      FanMove const& move = FanMoves[k];
      memcpy(fan.Written, fan.Original, sizeof(fan.Written));
      for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c)
          fan.Written[r * 4 + c] = move.Rows[r][c] * move.Scale;
      for (int c = 0; c < 3; ++c)
        fan.Written[12 + c] = move.Place[c];
      if (!WriteBase(fan.Record, fan.Written))
        continue;
      g_MovedFans.push_back(fan);
      util::log::Write("The Shard: its far-away fan at %.0f %.0f %.0f moved onto %s, %.0f %.0f %.0f, at %.3f its size (fan 0x%I64X)",
        move.From[0], move.From[1], move.From[2], move.What, move.Place[0], move.Place[1], move.Place[2], move.Scale, fan.Entity);
    }
    if (g_MovedFans.size() < 2 && g_ShardLodPresent && !s_missingLogged)
    {
      s_missingLogged = true;
      util::log::Write("The Shard: %d of its 2 far-away fans found to move (the far-away sub-level is loaded; the rotation "
        "updaters have %d and %d rotation entities, -1 for none)", (int)g_MovedFans.size(), counts[0], counts[1]);
    }
    return ShardFansTurning();
  }

  // The ruined far-away model's smoke (2026-11-03, the user, with the real
  // Shard streamed from anywhere: "find out why the smoke stays as well"):
  // it's an effect of its own, not a model group member, so hiding the
  // far-away model left it. fx/areas/shard/fx_shard_backdrop_smoke, the
  // only thing in pf_skycity_backdrop_far_destroyed_nongroupable_autogen_gen4,
  // which SkyCity_RuinBackdrop's art layer places at -2520.04 26.72 -1590.79
  // (no rotation; the effect at local 3413.03 2915.43 -1589.46): at 892.99
  // 2942.15 -3180.25. One emitter, EM_Shard_Backdrop_Smoke_Smoke: 650 m
  // puffs, 0.75 a second, living 60 s (+- 20 %), in world space; the
  // effect's CullDistance 15000 m (game data, read offline). The whole
  // far-away model has no smoke. While the far-away model is hidden the
  // tools move the smoke's instance 100 km down the game's own way, as the
  // weather moves the rain (Weather.cpp: the place at + 0x100, + 0x1B8 bit
  // 2, 0x1430606F0; the same code bytes checked), so the effect manager's
  // own culling stops it (0x14305F350 -> 0x143060450, its emitters stop
  // spawning at 0x1430F9370). The puffs already up stay until their time is
  // up: no code was found that clears them. It goes back to its place when
  // the far-away model is shown again (the culling starts it again, and it
  // resets when started, ResetInstanceWhenStarted).
  const __int64 EffectManagerSlotRva = 0x2416160;   // [ ]: the client effect manager
  const int EffectInstancesOffset = 0x48C0;         // its placed effects' instances: begin, + 8 end
  const size_t MostEffectInstances = 1u << 20;
  const int EffectPlaceOffset = 0x100;              // an instance's 4x4
  const int EffectFlagsOffset = 0x1B8;
  const BYTE EffectMovedFlag = 0x02;
  const __int64 EffectMoveRva = 0x30606F0;
  const int EffectDataComponentsOffset = 0x60;      // EffectEntityData: its components (count at - 4)
  const int EmitterDataDocumentOffset = 0xA0;       // EmitterEntityData: its emitter document
  const int EmitterDocumentHighOffset = 0x28;       // its high quality EmitterTemplateData
  const int EmitterTemplateNameOffset = 0x158;      // its DebugName
  const char ShardSmokeEmitter[] = "EM_Shard_Backdrop_Smoke_Smoke";
  const float ShardSmokePlace[3] = { 892.99f, 2942.15f, -3180.25f };
  const float ShardSmokeFindDistance = 200.f;
  const float ShardSmokeDrop = 100000.f;            // under the city, far beyond the CullDistance
  const ULONGLONG ShardSmokeCheckMs = 1000;
  typedef void(__fastcall* tEffectMove)(__int64);

  struct MovedSmoke
  {
    __int64 Instance;
    __int64 Data;
    float Original[16];   // the game's place
    float Written[16];
  };
  std::vector<MovedSmoke> g_MovedSmoke;   // camera hook only

  // The code that keeps and moves the effect instances, as the weather checks it
  bool IsEffectMoveCode()
  {
    BYTE const* pBase = (BYTE const*)g_gameHandle;
    // 0x1432A1FF0: mov rdx, rcx; mov rcx, [0x142416160]; jmp 0x1432905B0 (the manager's culling)
    static const BYTE culling[] = { 0x48, 0x89, 0xCA, 0x48, 0x8B, 0x0D, 0x66, 0x41, 0x17, 0xFF, 0xE9, 0xB1, 0xE5, 0xFE, 0xFF };
    // 0x143290605: mov r15, [rsi + 0x48C0]; mov rax, [rsi + 0x48C8] (its list)
    static const BYTE list[] = { 0x4C, 0x8B, 0xBE, 0xC0, 0x48, 0x00, 0x00, 0x48, 0x8B, 0x86, 0xC8, 0x48, 0x00, 0x00 };
    // 0x14305F051 (the EffectEntity's Transform): mov rcx, [rbx + 0x50]; the 4 rows to + 0x100 .. + 0x130;
    // or byte [rcx + 0x1B8], 2; call 0x1430606F0
    static const BYTE move[] = { 0x48, 0x8B, 0x4B, 0x50, 0x0F, 0x28, 0x00, 0x0F, 0x29, 0x81, 0x00, 0x01, 0x00, 0x00, 0x0F, 0x28,
      0x48, 0x10, 0x0F, 0x29, 0x89, 0x10, 0x01, 0x00, 0x00, 0x0F, 0x28, 0x40, 0x20, 0x0F, 0x29, 0x81, 0x20, 0x01, 0x00, 0x00,
      0x0F, 0x28, 0x48, 0x30, 0x80, 0x89, 0xB8, 0x01, 0x00, 0x00, 0x02, 0x0F, 0x29, 0x89, 0x30, 0x01, 0x00, 0x00, 0xE8, 0x64,
      0x16, 0x00, 0x00 };
    // 0x1430606F0: mov [rsp + 0x18], rbx; push rsi; push rdi; push r14; sub rsp, 0x20
    static const BYTE moveStart[] = { 0x48, 0x89, 0x5C, 0x24, 0x18, 0x56, 0x57, 0x41, 0x56, 0x48, 0x83, 0xEC, 0x20 };
    return IsRenderArenaBuild() && !memcmp(pBase + 0x32A1FF0, culling, sizeof(culling)) &&
      !memcmp(pBase + 0x3290605, list, sizeof(list)) && !memcmp(pBase + 0x305F051, move, sizeof(move)) &&
      !memcmp(pBase + EffectMoveRva, moveStart, sizeof(moveStart));
  }

  // The client effect manager's list of placed effects' instances
  bool EffectInstanceList(__int64 const*& begin, size_t& count)
  {
    __try
    {
      __int64 manager = *(__int64 const*)((__int64)g_gameHandle + EffectManagerSlotRva);
      if (manager < 0x10000)
        return false;
      __int64 const* first = *(__int64 const* const*)(manager + EffectInstancesOffset);
      __int64 const* last = *(__int64 const* const*)(manager + EffectInstancesOffset + 8);
      if (!first || last < first || (size_t)(last - first) > MostEffectInstances)
        return false;
      begin = first;
      count = (size_t)(last - first);
      return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      return false;
    }
  }

  __int64 EffectInstanceAt(__int64 const* begin, size_t i)
  {
    __try
    {
      return begin[i];
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      return 0;
    }
  }

  // Whether an instance is the Shard's smoke: its first emitter's DebugName;
  // its data and place given
  bool IsShardSmoke(__int64 instance, __int64& data, float* place)
  {
    __try
    {
      data = *(__int64 const*)instance;
      if (data < 0x10000)
        return false;
      __int64 components = *(__int64 const*)(data + EffectDataComponentsOffset);
      if (components < 0x10000)
        return false;
      unsigned int count = *(unsigned int const*)(components - 4);
      if (!count || count > 64)
        return false;
      __int64 emitter = *(__int64 const*)components;
      __int64 document = emitter >= 0x10000 ? *(__int64 const*)(emitter + EmitterDataDocumentOffset) : 0;
      __int64 high = document >= 0x10000 ? *(__int64 const*)(document + EmitterDocumentHighOffset) : 0;
      char const* name = high >= 0x10000 ? *(char const* const*)(high + EmitterTemplateNameOffset) : nullptr;
      if ((__int64)name < 0x10000)
        return false;
      for (int i = 0; ShardSmokeEmitter[i] || name[i]; ++i)
        if (name[i] != ShardSmokeEmitter[i])
          return false;
      memcpy(place, (void const*)(instance + EffectPlaceOffset), 16 * sizeof(float));
      return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      return false;
    }
  }

  // As the game's Transform property does (0x14305F010): the place, its flag, the move
  void MoveEffectInstance(__int64 instance, float const* rows)
  {
    __try
    {
      memcpy((void*)(instance + EffectPlaceOffset), rows, 16 * sizeof(float));
      *(BYTE*)(instance + EffectFlagsOffset) |= EffectMovedFlag;
      ((tEffectMove)((__int64)g_gameHandle + EffectMoveRva))(instance);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
  }

  // Whether a smoke the tools moved is still that instance, where they put it
  bool StillMovedSmoke(MovedSmoke const& smoke, __int64 const* begin, size_t count)
  {
    bool listed = false;
    for (size_t i = 0; i < count && !listed; ++i)
      listed = EffectInstanceAt(begin, i) == smoke.Instance;
    if (!listed)
      return false;
    __try
    {
      return *(__int64 const*)smoke.Instance == smoke.Data &&
        !memcmp((void const*)(smoke.Instance + EffectPlaceOffset), smoke.Written, sizeof(smoke.Written));
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      return false;
    }
  }

  // Away (the far-away model hidden): the smoke moved out of sight, looked
  // for once a second while none is; else back at its place
  void UpdateShardSmoke(bool away)
  {
    static int s_code = 0;   // 0 not checked, 1 the code the tools know, -1 not
    static ULONGLONG s_last = 0;
    static bool s_elsewhereLogged = false;
    if (!s_code)
    {
      s_code = IsEffectMoveCode() ? 1 : -1;
      if (s_code < 0)
        util::log::Warning("The Shard: the effect instances' code isn't what the tools know: the ruined far-away model's "
          "smoke stays while the real tower is loaded");
    }
    if (s_code != 1)
      return;
    __int64 const* begin = nullptr;
    size_t count = 0;
    if (!away)
    {
      if (g_MovedSmoke.empty())
        return;
      int back = 0;
      if (EffectInstanceList(begin, count))
        for (MovedSmoke const& smoke : g_MovedSmoke)
          if (StillMovedSmoke(smoke, begin, count))
          {
            MoveEffectInstance(smoke.Instance, smoke.Original);
            ++back;
          }
      util::log::Write("The Shard: its far-away smoke back at its place (%d of the %d the tools moved still there)", back,
        (int)g_MovedSmoke.size());
      g_MovedSmoke.clear();
      return;
    }
    ULONGLONG now = GetTickCount64();
    if (now - s_last < ShardSmokeCheckMs)
      return;
    s_last = now;
    if (!EffectInstanceList(begin, count))
      return;
    // The ones that went (their sub-level streamed out) or the game placed
    // again are forgotten, unwritten
    g_MovedSmoke.erase(std::remove_if(g_MovedSmoke.begin(), g_MovedSmoke.end(),
      [&](MovedSmoke const& smoke) { return !StillMovedSmoke(smoke, begin, count); }), g_MovedSmoke.end());
    if (!g_MovedSmoke.empty())
      return;
    for (size_t i = 0; i < count; ++i)
    {
      __int64 instance = EffectInstanceAt(begin, i);
      MovedSmoke smoke = { instance };
      if (instance < 0x10000 || !IsShardSmoke(instance, smoke.Data, smoke.Original))
        continue;
      float dx = smoke.Original[12] - ShardSmokePlace[0], dy = smoke.Original[13] - ShardSmokePlace[1],
        dz = smoke.Original[14] - ShardSmokePlace[2];
      if (dx * dx + dy * dy + dz * dz > ShardSmokeFindDistance * ShardSmokeFindDistance)
      {
        if (!s_elsewhereLogged)
        {
          s_elsewhereLogged = true;
          util::log::Write("The Shard: an effect with the far-away smoke's emitter at %.1f %.1f %.1f, not the far-away "
            "model's (left as it is)", smoke.Original[12], smoke.Original[13], smoke.Original[14]);
        }
        continue;
      }
      memcpy(smoke.Written, smoke.Original, sizeof(smoke.Written));
      smoke.Written[13] -= ShardSmokeDrop;
      MoveEffectInstance(instance, smoke.Written);
      g_MovedSmoke.push_back(smoke);
      util::log::Write("The Shard: the ruined far-away model's smoke (effect instance 0x%I64X, at %.1f %.1f %.1f) moved "
        "%.0f km down while the real tower is loaded: the game stops it there (the puffs already up stay until their "
        "time is up, about a minute)", instance, smoke.Original[12], smoke.Original[13], smoke.Original[14], ShardSmokeDrop / 1000.f);
    }
  }

  // The far-away model hidden while the real tower is loaded, shown again
  // once it isn't. From the camera hook, on the game's own thread (the
  // client's), where the replay gives model groups their Visible too. Gives
  // the real tower's zone: 1 loaded, 0 not, -1 not known.
  int UpdateShardLod()
  {
    static bool s_build = IsRenderArenaBuild();
    static ULONGLONG s_lastRead = 0, s_lastSearch = 0;
    static ShardZoneCache s_zones{};
    static std::vector<ShardLodGroup> s_hidden;   // camera hook only: the groups the tools hid
    static int s_lastLoaded = -1;
    static bool s_failLogged = false;
    ULONGLONG now = GetTickCount64();
    if (!s_build)
      return -1;
    if (now - s_lastRead < ShardReadMs)
      return s_lastLoaded;
    s_lastRead = now;

    // The real tower: its zone in the buildings' server streamer
    ServerStreamer servers[8];
    AcquireSRWLockShared(&g_ServerStreamerLock);
    memcpy(servers, g_ServerStreamers, sizeof(servers));
    ReleaseSRWLockShared(&g_ServerStreamerLock);
    int loaded = -1;
    for (ServerStreamer const& server : servers)
    {
      if (!server.Streamer || now - server.Seen > VistaGoneMs || !strstr(server.Path, ShardStreamerPath))
        continue;
      int zone = ShardZoneOf(server.Data, s_zones);
      if (zone >= 0)
        loaded = ReadZoneLoaded(server.Streamer, zone);
      break;
    }
    bool hide = loaded == 1;
    bool turned = (loaded == 1) != (s_lastLoaded == 1);
    s_lastLoaded = loaded;

    if (!hide)
    {
      if (!s_hidden.empty())
      {
        int shown = 0;
        for (ShardLodGroup const& group : s_hidden)
          shown += StillShardLodGroup(group) && SetShardLodVisible(group, true) ? 1 : 0;
        util::log::Write("The Shard: the real tower isn't loaded any more; its far-away model is shown again (%d of the %d "
          "model groups the tools hid are still there)", shown, (int)s_hidden.size());
        s_hidden.clear();
        g_ShardLodPresent = shown > 0;
      }
      // Whether the far-away model is loaded (the zone streamer hook keeps
      // the real tower in until it is, back in the city)
      if (now - s_lastSearch >= ShardPresenceMs)
      {
        s_lastSearch = now;
        ShardLodGroup probe[1];
        g_ShardLodPresent = FindShardLodGroups(probe, 1) > 0;
      }
      return loaded;
    }

    // Hidden: the ones that went are forgotten (their sub-level streamed
    // out), and new ones (streamed in again) are looked for now and then
    s_hidden.erase(std::remove_if(s_hidden.begin(), s_hidden.end(),
      [](ShardLodGroup const& group) { return !StillShardLodGroup(group); }), s_hidden.end());
    if (!turned && now - s_lastSearch < ShardSearchMs)
    {
      g_ShardLodPresent = !s_hidden.empty();
      return loaded;
    }
    s_lastSearch = now;
    ShardLodGroup found[MaxShardLodGroups];
    int count = FindShardLodGroups(found, MaxShardLodGroups);
    g_ShardLodPresent = count > 0;
    int hidden = 0, members = 0;
    for (int f = 0; f < count; ++f)
    {
      bool known = false;
      for (ShardLodGroup const& group : s_hidden)
        known |= group.Group == found[f].Group && group.Data == found[f].Data;
      if (known)
        continue;
      if (!SetShardLodVisible(found[f], false))
      {
        if (!s_failLogged)
        {
          s_failLogged = true;
          util::log::Warning("The Shard: its far-away model can't be hidden (model group 0x%I64X): the model groups' code "
            "isn't what the tools know", found[f].Group);
        }
        SetShardLodVisible(found[f], true);
        continue;
      }
      s_hidden.push_back(found[f]);
      ++hidden;
      members += found[f].Count;
    }
    if (hidden)
      util::log::Write("The Shard: the real tower (zone %s of the buildings) is loaded; its far-away model is hidden "
        "(%d models in %d model groups)", ShardZoneName, members, hidden);
    else if (turned)
      util::log::Write("The Shard: the real tower (zone %s of the buildings) is loaded; no far-away model of it is loaded",
        ShardZoneName);
    return loaded;
  }

  // The Shard's tip, for lightning to strike (weather::SetShardTop): the top
  // of the far-away model the game draws from the city, from its mesh's
  // vertices (2026-10-25, game data: the MeshSet's LOD0 positions, half
  // floats, so within 2 - 4 m), moved by its model group instance's transform
  // as the game has it (the member's + 0x08; read live 23:5x: the ruin's
  // rows identity and translation -2520.04 26.72 -1590.79, as in the data):
  // - whole (skycity_backdrop_far_whole_mesh): the centre of its top, a disk
  //   at y 4444 inside a ring 4 higher (local 346.88 4444 22.88; placed as in
  //   the data, 1111.99 4470.72 -2939.67);
  // - ruined (skycity_backdrop_far_destroyed_mesh): its highest vertex (local
  //   3654 4340 -1386; 1133.96 4366.72 -2976.79).
  // The real tower (zone 55, drawn instead out of the city): its highest
  // point is the 42 m antenna (antenna05) on the roof of its top, the
  // sub-level SkyCityTopOpenBlinds / ClosedBlinds (the only users of
  // skycity_building_top_mesh). The antenna, like most of that sub-level's
  // pieces, keeps no transform in the level data: its group physics res
  // (staticmodelgroup_physics_win32) places it, 128-byte records (rotation
  // rows + 0, position + 0x30, scale + 0x40, 0xFFFF0000 | the physics part at
  // + 0x54): part 30, its member's PhysicsPartRange (member + 0x48) in both
  // versions, rows (-0.29028 0 0.95694) (0 1 0) (-0.95694 0 -0.29028) and
  // position 900.5045 1635.79 -2948.6228 in both (2026-10-26, game data; the
  // part numbers and the record read live 00:3x, the record found in the
  // loaded physics through the group). Its mesh's top, a 16 cm cap at local
  // 0 41.938 0, is then at 900.504 1677.728 -2948.621; the physics res's own
  // box tops out at 1677.734. The tower top's own mesh (part 283, placed where
  // the data places Kruger's office) only reaches about 1636. Given only while
  // that sub-level is loaded with the antenna as part 30.
  // None while both or neither far-away model is loaded, or while the real
  // tower is drawn without its top.
  const char* const ShardFarWholeMeshes[] = { "skycity_backdrop_far_whole_mesh" };
  const char* const ShardFarRuinMeshes[] = { "skycity_backdrop_far_destroyed_mesh" };
  const char* const ShardTopMeshes[] = { "skycity_building_top_mesh" };
  const char* const ShardAntennaMeshes[] = { "antenna05_mesh" };
  bool IsShardFarWholeMesh(const char* pName) { return MeshNameIs(pName, ShardFarWholeMeshes, 1); }
  bool IsShardFarRuinMesh(const char* pName) { return MeshNameIs(pName, ShardFarRuinMeshes, 1); }
  bool IsShardTopMesh(const char* pName) { return MeshNameIs(pName, ShardTopMeshes, 1); }
  const float ShardFarWholeTip[3] = { 346.88f, 4444.f, 22.88f };
  const float ShardFarRuinTip[3] = { 3654.f, 4340.f, -1386.f };
  const float ShardRealTip[3] = { 900.504f, 1677.728f, -2948.621f };
  const unsigned int ShardAntennaPart = 30;
  const int MemberTransformsOffset = 0x08;     // a member's instance transforms: 4 float4 rows each (right, up, forward, position)
  const int MemberPhysicsPartsOffset = 0x48;   // a member's PhysicsPartRange: first, last
  const ULONGLONG ShardTipCheckMs = 2000;

  // Whether the real tower's top is loaded with its antenna as the part measured
  bool ShardTopLoaded()
  {
    ModelGroupMatch top[1];
    if (FindModelGroups(IsShardTopMesh, top, 1) < 1)
      return false;
    __try
    {
      int count = *(int*)(top[0].Group + ModelGroupMemberCountOffset);
      __int64 members = *(__int64*)(top[0].Data + GroupDataMembersOffset);
      if (count <= 0 || count > MaxGroupMembers || members < 0x10000)
        return false;
      for (int k = 0; k < count; ++k)
      {
        __int64 member = members + (__int64)k * GroupMemberDataSize;
        __int64 mesh = *(__int64*)(member + MemberMeshOffset);
        if (mesh < 0x10000 || !MeshNameIs(*(const char**)(mesh + AssetNameOffset), ShardAntennaMeshes, 1))
          continue;
        return *(unsigned int*)(member + MemberPhysicsPartsOffset) == ShardAntennaPart &&
          *(unsigned int*)(member + MemberPhysicsPartsOffset + 4) == ShardAntennaPart;
      }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
    return false;
  }

  // A found group's first member's first instance transform, as the game has it
  bool ReadMemberTransform(ModelGroupMatch const& group, float (&m)[16])
  {
    __try
    {
      __int64 members = *(__int64*)(group.Data + GroupDataMembersOffset);
      if (members < 0x10000)
        return false;
      __int64 transforms = *(__int64*)(members + (__int64)group.Members[0] * GroupMemberDataSize + MemberTransformsOffset);
      if (transforms < 0x10000)
        return false;
      memcpy(m, (void const*)transforms, sizeof(m));
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      return false;
    }
    for (int i = 0; i < 16; ++i)
      if (i % 4 != 3 && (!std::isfinite(m[i]) || fabsf(m[i]) > 100000.f))
        return false;
    return true;
  }

  // Every frame from the camera hook, with UpdateShardLod's result (it looks
  // every 2 s). Only a burst of lightning reads the tip (weather::SetShardTop),
  // so the far-away models (two walks through every model group and its
  // members' names: a 1.2 - 2.5 ms frame every 2 s in the city, CT.log
  // 2026-11-13) are only looked for while lightning can come
  // (weather::ShardTopWanted, asked every frame before weather::Update); a
  // check that left them out is made up for, the far-away models only, as
  // soon as it can come (the weather makes no burst before that). The real
  // tower's top is looked at every 2 s as before (the Districts menu shows it).
  void UpdateShardTip(int shardLoaded)
  {
    static ULONGLONG s_last = 0;
    static float s_tip[3] = {};
    static int s_state = -1;   // 0 none loaded, 1 whole, 2 ruined, 3 both, 4 the real tower without its top, 5 not known, 6 the real tower, 7 the far-away models not looked for (no lightning)
    static unsigned int s_leftOut = 0;   // checks that left the far-away models out since they were last looked for
    ULONGLONG now = GetTickCount64();
    bool lightning = weather::ShardTopWanted();
    bool due = now - s_last >= ShardTipCheckMs;
    bool makeUp = !due && s_state == 7 && lightning;
    if (due || makeUp)
    {
      if (due)
        s_last = now;
      bool lookFar = makeUp || shardLoaded == 0;
      int state = shardLoaded == 1 ? 4 : 5;
      float tip[3] = {};
      if (!lookFar && shardLoaded == 1 && ShardTopLoaded())
      {
        memcpy(tip, ShardRealTip, sizeof(tip));
        state = 6;
      }
      else if (lookFar && !lightning)
      {
        state = 7;
        ++s_leftOut;
      }
      else if (lookFar)
      {
        if (s_state == 7 && util::DiagnosticsOn())
          util::log::Write("The Shard's tip for lightning (diagnostic): lightning can come, its far-away models looked for "
            "again (%u checks left them out before)", s_leftOut);
        s_leftOut = 0;
        ModelGroupMatch whole[2], ruin[2];
        int w = FindModelGroups(IsShardFarWholeMesh, whole, 2);
        int r = FindModelGroups(IsShardFarRuinMesh, ruin, 2);
        float m[16];
        state = w && r ? 3 : 0;
        if (w + r == 1 && ReadMemberTransform(w ? whole[0] : ruin[0], m))
        {
          float const* local = w ? ShardFarWholeTip : ShardFarRuinTip;
          for (int i = 0; i < 3; ++i)
            tip[i] = m[12 + i] + m[i] * local[0] + m[4 + i] * local[1] + m[8 + i] * local[2];
          state = w ? 1 : 2;
        }
      }
      bool known = state == 1 || state == 2 || state == 6;
      bool moved = known && (fabsf(tip[0] - s_tip[0]) > 0.5f || fabsf(tip[1] - s_tip[1]) > 0.5f ||
        fabsf(tip[2] - s_tip[2]) > 0.5f);
      if (state != s_state || moved)
      {
        if (known)
          util::log::Write("The Shard's tip for lightning: %s, at %.2f %.2f %.2f", state == 6 ? "the real tower's antenna" :
            state == 1 ? "its far-away model, whole" : "its far-away model, ruined", tip[0], tip[1], tip[2]);
        else
          util::log::Write("The Shard's tip for lightning: none (%s)", state == 7 ? "not looked for while no lightning can "
            "come" : state == 4 ? "the real tower is drawn without its top, "
            "or its antenna isn't the part measured" : state == 3 ? "both far-away models are loaded" : state == 0 ?
            "no far-away model of it is loaded" : "the real tower's zone isn't known");
      }
      s_state = state;
      g_ShardTopIn = state == 6;
      if (known)
        memcpy(s_tip, tip, sizeof(s_tip));
    }
    weather::SetShardTop(s_state == 1 || s_state == 2 || s_state == 6, s_tip);
  }

  // Districts. Each zone of a streamer belongs to a region bundle of the city
  // (its parent in the zone table: Region_Props_Anchor, Region_Building_
  // Rezoning, ...), the districts the game streams by. Props and buildings
  // have the same ones; the ground's zones aren't split by district. In the
  // menu each district is Normal, Always (loaded while the camera is on,
  // held back by the same limits as the rest) or Never (the camera's foci
  // leave it out; what's around Faith, and the zones next to loaded ones,
  // still load: a focus loads its zone and every zone next to it).
  //
  // Always: foci for that streamer only, on as few of the district's zones as
  // cover it with the zones next to them (the ground's streamer gets the
  // props' too, so there's ground under it).
  enum DistrictState { District_Normal = 0, District_Always = 1, District_Never = 2 };
  const int ZoneInfosOffset = 0x70;            // data -> ZoneInfos (count at - 4): 0x10 each, Neighbours (u16 array) first
  const int ZoneInfoStride = 0x10;
  const int MaxZoneNeighbours = 64;
  const int MaxDistrictLayerZones = 512;
  const int MaxRegionBundles = 64;
  const ULONGLONG DistrictReadMs = 500;

  struct District
  {
    std::string Key;        // the region without its layer: "Rezoning", "DowntownNorth"
    std::string Name;       // for the menu
    int State;
  };
  struct DistrictZone
  {
    int District;           // in g_Districts, -1 for none
    float X, Y, Z;          // a point inside it, in the middle of its height band
    bool Loaded;
    std::vector<unsigned short> Neighbours;
  };
  struct DistrictLayer
  {
    __int64 Streamer;
    __int64 Data;
    std::string Name;       // Props, Buildings, Ground
    bool HasDistricts;
    std::vector<DistrictZone> Zones;
    std::vector<int> Total, Loaded; // per district
    ULONGLONG Seen;
  };
  std::vector<District> g_Districts;           // only grows (a district keeps its place)
  std::vector<DistrictLayer> g_DistrictLayers;
  std::vector<std::pair<std::string, int>> g_DistrictPending; // from the config, not seen yet
  SRWLOCK g_DistrictLock = SRWLOCK_INIT;
  std::atomic<unsigned int> g_DistrictGeneration{ 1 }; // changes when the Always foci need working out again

  struct RawZone
  {
    int Parent;
    float X, Y, Z;
    bool Loaded;
    int NeighbourCount;
    unsigned short Neighbours[MaxZoneNeighbours];
  };
  struct RawRegion
  {
    char Name[64];
  };

  // A point inside a zone, near its centre (the centre of a bent zone can be
  // outside it). Reads game memory, so only inside __try.
  void InsideZonePoint(float const* pBounds, __int64 pZone, float cx, float cz, float& x, float& z)
  {
    x = cx;
    z = cz;
    if (InZoneOutline(pZone, cx, cz))
      return;
    float best = FLT_MAX;
    for (int i = 1; i < 8; ++i)
    {
      for (int j = 1; j < 8; ++j)
      {
        float px = pBounds[0] + (pBounds[2] - pBounds[0]) * i / 8.f;
        float pz = pBounds[1] + (pBounds[3] - pBounds[1]) * j / 8.f;
        float d = (px - cx) * (px - cx) + (pz - cz) * (pz - cz);
        if (d < best && InZoneOutline(pZone, px, pz))
        {
          best = d;
          x = px;
          z = pz;
        }
      }
    }
  }

  // A streamer's zones: parent bundle, a point inside, the middle of its
  // band, loaded, and (full) its neighbours; and the names of its regions.
  // Gives the zone count, -1 if it couldn't be read. No C++ objects (__try).
  int ReadDistrictZones(__int64 pStreamer, bool full, RawZone* zones, int maxZones, RawRegion* regions,
    int maxRegions, int& regionCount, __int64& data)
  {
    regionCount = 0;
    __try
    {
      data = *(__int64*)(pStreamer + ZoneDataOffset);
      __int64 pGrid = *(__int64*)(pStreamer + ZoneGridOffset);
      __int64 pTable = *(__int64*)(pStreamer + ZoneTableOffset);
      if (data < 0x10000 || pGrid < 0x10000 || pTable < 0x10000)
        return -1;
      int count = *(int*)(pGrid + GridZoneCountOffset);
      float const* pBounds = *(float const**)(pGrid + GridBoundsOffset);
      float const* pCentres = *(float const**)(pGrid + GridCentresOffset);
      __int64 pZones = *(__int64*)(pGrid + GridZonesOffset);
      __int64 pNames = *(__int64*)(data + ZoneBundleNamesOffset);
      __int64 pInfos = *(__int64*)(data + ZoneInfosOffset);
      if (count <= 0 || count > maxZones || (__int64)pBounds < 0x10000 || (__int64)pCentres < 0x10000 ||
        pZones < 0x10000 || pNames < 0x10000 || pInfos < 0x10000)
        return -1;
      int nameCount = *(int*)(pNames - 4);
      int infoCount = *(int*)(pInfos - 4);
      if (nameCount < count || nameCount - count > maxRegions || infoCount < count)
        return -1;
      for (int k = 0; k < count; ++k)
      {
        RawZone& zone = zones[k];
        unsigned char const* pEntry = (unsigned char const*)(pTable + (__int64)k * 8);
        zone.Parent = *(short const*)(pEntry + 6);
        zone.Loaded = *(unsigned short const*)(pEntry + 2) != 0 && !(pEntry[0] & ZoneLoadingFlag);
        if (!full)
          continue;
        __int64 pZone = pZones + (__int64)k * GridZoneStride;
        InsideZonePoint(pBounds + k * 4, pZone, pCentres[k * 2], pCentres[k * 2 + 1], zone.X, zone.Z);
        float low = *(float*)pZone, height = *(float*)(pZone + 4);
        zone.Y = height > 0.f ? low + height * 0.5f : 50.f;
        unsigned short const* pNeighbours = *(unsigned short const**)(pInfos + (__int64)k * ZoneInfoStride);
        int n = pNeighbours ? *(int const*)((char const*)pNeighbours - 4) : 0;
        if (n < 0) n = 0;
        if (n > MaxZoneNeighbours) n = MaxZoneNeighbours;
        zone.NeighbourCount = n;
        for (int i = 0; i < n; ++i)
          zone.Neighbours[i] = pNeighbours[i];
      }
      regionCount = nameCount - count;
      for (int r = 0; r < regionCount; ++r)
      {
        const char* pName = *(const char**)(pNames + (__int64)(count + r) * 8);
        size_t n = 0;
        while (pName && n + 1 < sizeof(regions[r].Name) && pName[n] >= 32 && pName[n] < 127)
        {
          regions[r].Name[n] = pName[n];
          ++n;
        }
        regions[r].Name[n] = 0;
      }
      return count;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      return -1;
    }
  }

  // "Region_Props_DowntownNorth", "Region_Building_PrisionYard" -> "DowntownNorth", "PrisonYard"
  std::string DistrictKey(std::string const& region)
  {
    std::string key = region;
    for (const char* prefix : { "Region_Props_", "Region_Building_", "BuildingRegion_", "Region_Ground_", "Region_" })
    {
      size_t length = strlen(prefix);
      if (key.compare(0, length, prefix) == 0)
      {
        key = key.substr(length);
        break;
      }
    }
    size_t typo = key.find("Prision");
    if (typo != std::string::npos)
      key.replace(typo, 7, "Prison");
    return key;
  }

  // "DowntownNorth" -> "Downtown North", and the few the game names differently on its map
  std::string DistrictName(std::string const& key)
  {
    if (key == "Rezoning") return "Rezoning (Development Zone)";
    if (key == "PrisonDefault") return "Prison";
    if (key == "Default") return "Default (the rest)";
    std::string name;
    for (size_t i = 0; i < key.size(); ++i)
    {
      if (i && isupper((unsigned char)key[i]) && !isupper((unsigned char)key[i - 1]))
        name += ' ';
      name += key[i];
    }
    return name;
  }

  // Index of a district in g_Districts, added if it's new. Call with g_DistrictLock held.
  int FindDistrict(std::string const& key)
  {
    for (size_t d = 0; d < g_Districts.size(); ++d)
      if (g_Districts[d].Key == key) return (int)d;
    int state = District_Normal;
    for (auto& pending : g_DistrictPending)
      if (pending.first == key) state = pending.second;
    g_Districts.push_back({ key, DistrictName(key), state });
    return (int)g_Districts.size() - 1;
  }

  // Reads one streamer's zones and districts, twice a second (zone streamer
  // hook). The neighbours and inside points only when its data is new.
  void UpdateDistrictLayer(__int64 pZoneStreamer)
  {
    ULONGLONG now = GetTickCount64();
    AcquireSRWLockShared(&g_DistrictLock);
    DistrictLayer const* pKnown = nullptr;
    for (DistrictLayer const& layer : g_DistrictLayers)
      if (layer.Streamer == pZoneStreamer) pKnown = &layer;
    bool due = !pKnown || now - pKnown->Seen >= DistrictReadMs;
    __int64 knownData = pKnown ? pKnown->Data : 0;
    ReleaseSRWLockShared(&g_DistrictLock);
    if (!due)
      return;

    static std::vector<RawZone> s_zones(MaxDistrictLayerZones); // zone streamer hook only
    RawRegion regions[MaxRegionBundles];
    int regionCount = 0;
    __int64 data = 0;
    bool full = !knownData || ReadStreamerData(pZoneStreamer) != knownData;
    int count = ReadDistrictZones(pZoneStreamer, full, s_zones.data(), MaxDistrictLayerZones, regions, MaxRegionBundles,
      regionCount, data);
    char path[128];
    if (full)
      ReadSubLevelPath(pZoneStreamer, path, sizeof(path));

    AcquireSRWLockExclusive(&g_DistrictLock);
    DistrictLayer* pLayer = nullptr;
    for (DistrictLayer& layer : g_DistrictLayers)
      if (layer.Streamer == pZoneStreamer) pLayer = &layer;
    if (count > 0)
    {
      if (!pLayer)
      {
        g_DistrictLayers.push_back(DistrictLayer());
        pLayer = &g_DistrictLayers.back();
        pLayer->Streamer = pZoneStreamer;
        full = true;
      }
      if (full || pLayer->Data != data || (int)pLayer->Zones.size() != count)
      {
        if (!full)
        {
          // Its data changed between the two reads: again next time, in full
          pLayer->Data = 0;
          ReleaseSRWLockExclusive(&g_DistrictLock);
          return;
        }
        pLayer->Data = data;
        pLayer->Name = LayerName(path);
        // A region's district (by name), for the regions that are a zone's
        // own parent (not the ones above them, like Downtown over Downtown
        // North). The ground's regions (Default, Release) aren't districts.
        std::vector<int> regionDistrict(regionCount, -1);
        bool hasDistricts = false;
        std::vector<char> parentOfZone(regionCount, 0);
        for (int k = 0; k < count; ++k)
        {
          int r = s_zones[k].Parent - count;
          if (r >= 0 && r < regionCount)
            parentOfZone[r] = 1;
        }
        for (int r = 0; r < regionCount; ++r)
        {
          std::string key = DistrictKey(regions[r].Name);
          if (pLayer->Name == "Ground" || key.empty() || !parentOfZone[r])
            continue;
          regionDistrict[r] = FindDistrict(key);
        }
        pLayer->Zones.assign(count, DistrictZone());
        for (int k = 0; k < count; ++k)
        {
          RawZone const& raw = s_zones[k];
          DistrictZone& zone = pLayer->Zones[k];
          int r = raw.Parent - count;
          zone.District = r >= 0 && r < regionCount ? regionDistrict[r] : -1;
          hasDistricts |= zone.District >= 0;
          zone.X = raw.X;
          zone.Y = raw.Y;
          zone.Z = raw.Z;
          zone.Neighbours.assign(raw.Neighbours, raw.Neighbours + raw.NeighbourCount);
        }
        pLayer->HasDistricts = hasDistricts;
        ++g_DistrictGeneration;
      }
      int districts = (int)g_Districts.size();
      pLayer->Total.assign(districts, 0);
      pLayer->Loaded.assign(districts, 0);
      for (int k = 0; k < count; ++k)
      {
        DistrictZone& zone = pLayer->Zones[k];
        zone.Loaded = s_zones[k].Loaded;
        if (zone.District >= 0 && zone.District < districts)
        {
          pLayer->Total[zone.District]++;
          pLayer->Loaded[zone.District] += zone.Loaded ? 1 : 0;
        }
      }
      pLayer->Seen = now;
    }
    // Layers whose streamer has gone (a level unloaded)
    size_t before = g_DistrictLayers.size();
    g_DistrictLayers.erase(std::remove_if(g_DistrictLayers.begin(), g_DistrictLayers.end(),
      [&](DistrictLayer const& layer) { return now - layer.Seen > VistaGoneMs; }), g_DistrictLayers.end());
    if (g_DistrictLayers.size() != before)
      ++g_DistrictGeneration;
    ReleaseSRWLockExclusive(&g_DistrictLock);
  }

  // Whether a focus at this point would be in a Never district of this
  // streamer (zone streamer hook)
  bool InNeverDistrict(__int64 pZoneStreamer, float x, float y, float z)
  {
    int zone = FindFocusZone(pZoneStreamer, x, z, y);
    if (zone < 0)
      return false;
    bool never = false;
    AcquireSRWLockShared(&g_DistrictLock);
    for (DistrictLayer const& layer : g_DistrictLayers)
    {
      if (layer.Streamer != pZoneStreamer || zone >= (int)layer.Zones.size())
        continue;
      int d = layer.Zones[zone].District;
      never = d >= 0 && d < (int)g_Districts.size() && g_Districts[d].State == District_Never;
      break;
    }
    ReleaseSRWLockShared(&g_DistrictLock);
    return never;
  }

  // Any district set to Never (so the zone streamer hook knows whether to look)
  std::atomic<bool> g_AnyNeverDistrict{ false };

  // Any district set to Always or Never (for the menu: they apply whether
  // the world is loaded around the camera or not)
  bool AnyDistrictSet()
  {
    bool any = false;
    AcquireSRWLockShared(&g_DistrictLock);
    for (District const& district : g_Districts)
      any |= district.State != District_Normal;
    ReleaseSRWLockShared(&g_DistrictLock);
    return any;
  }

  // GAME > Districts > the real Shard: a point inside its tower's zone (55) in
  // the buildings' streamer, and that streamer, for its focus (camera hook)
  bool ShardZonePoint(float* point, __int64& streamer)
  {
    static ShardZoneCache s_zones{};   // camera hook only
    bool found = false;
    AcquireSRWLockShared(&g_DistrictLock);
    for (DistrictLayer const& layer : g_DistrictLayers)
    {
      int zone = ShardZoneOf(layer.Data, s_zones);
      if (zone < 0 || zone >= (int)layer.Zones.size())
        continue;
      DistrictZone const& inside = layer.Zones[zone];
      point[0] = inside.X;
      point[1] = inside.Y;
      point[2] = inside.Z;
      streamer = layer.Streamer;
      found = true;
      break;
    }
    ReleaseSRWLockShared(&g_DistrictLock);
    return found;
  }

  // The Always districts' foci: for each district and layer, the fewest of
  // its zones that with the zones next to them cover all of it (the one that
  // covers the most of what's left, then again). Camera hook.
  struct DistrictFocus
  {
    float X, Y, Z;
    __int64 Streamers[MaxFocusStreamers];
    int District;
  };
  std::atomic<bool> g_DistrictFociShort{ false }; // more Always districts than foci for them

  void BuildDistrictFoci(std::vector<DistrictFocus>& foci, int max)
  {
    foci.clear();
    bool shortOfFoci = false;
    AcquireSRWLockShared(&g_DistrictLock);
    // The layer with the most zones also takes the layers without districts (the ground)
    DistrictLayer const* pMain = nullptr;
    for (DistrictLayer const& layer : g_DistrictLayers)
      if (layer.HasDistricts && (!pMain || layer.Zones.size() > pMain->Zones.size())) pMain = &layer;
    for (size_t d = 0; d < g_Districts.size(); ++d)
    {
      if (g_Districts[d].State != District_Always)
        continue;
      for (DistrictLayer const& layer : g_DistrictLayers)
      {
        if (!layer.HasDistricts)
          continue;
        int count = (int)layer.Zones.size();
        std::vector<char> left(count, 0);
        int leftCount = 0;
        for (int k = 0; k < count; ++k)
          if (layer.Zones[k].District == (int)d) { left[k] = 1; ++leftCount; }
        while (leftCount > 0 && (int)foci.size() < max)
        {
          int best = -1, bestCovers = 0;
          for (int k = 0; k < count; ++k)
          {
            if (layer.Zones[k].District != (int)d)
              continue;
            int covers = left[k] ? 1 : 0;
            for (unsigned short n : layer.Zones[k].Neighbours)
              if (n < count && n != k && left[n]) ++covers;
            if (covers > bestCovers) { best = k; bestCovers = covers; }
          }
          if (best < 0)
            break;
          DistrictFocus focus{ layer.Zones[best].X, layer.Zones[best].Y, layer.Zones[best].Z, {}, (int)d };
          int s = 0;
          focus.Streamers[s++] = layer.Streamer;
          if (&layer == pMain)
            for (DistrictLayer const& other : g_DistrictLayers)
              if (!other.HasDistricts && s < MaxFocusStreamers) focus.Streamers[s++] = other.Streamer;
          foci.push_back(focus);
          auto cover = [&](int k) { if (k >= 0 && k < count && left[k]) { left[k] = 0; --leftCount; } };
          cover(best);
          for (unsigned short n : layer.Zones[best].Neighbours)
            cover(n);
        }
        shortOfFoci |= leftCount > 0;
      }
    }
    ReleaseSRWLockShared(&g_DistrictLock);
    g_DistrictFociShort = shortOfFoci;
  }
  std::vector<DistrictFocus> g_DistrictFoci;   // camera hook only
  unsigned int g_DistrictFociGeneration = 0;

  // The game loads the zones waiting to load in order of distance from its
  // first focus, its own camera next to Faith (the sorter at
  // ZoneStreamerEntity + 0x2E8, 0x143C87880: only x and z of the first focus,
  // against the zones' centres in the grid). So with the free camera far away
  // what's in front of it, facing away from Faith, loaded last. While the
  // world is loaded around the free camera the tools order them instead:
  // what's around Faith first (she mustn't lose the ground under her), then
  // what the free camera looks at or is close to, nearest first, then the
  // rest, nearest the camera first.
  const float FaithZoneRadius = 150.f;
  const int MaxSortedZones = 256;          // the game's list holds 128

  bool SortZoneLoads(SortView const& view, __int64 pSorter, float const* pCache, unsigned short* zones, int count)
  {
    if (count < 0 || count > MaxSortedZones)
      return false;
    if (count <= 1)
      return true; // nothing to order
    unsigned short zone[MaxSortedZones];
    int tierOf[MaxSortedZones];
    float key[MaxSortedZones];
    __try
    {
      __int64 pGrid = *(__int64*)(pSorter + 8);
      if (pGrid < 0x10000)
        return false;
      int zoneCount = *(int*)(pGrid + GridZoneCountOffset);
      float const* pCentres = *(float const**)(pGrid + GridCentresOffset);
      if (zoneCount <= 0 || (__int64)pCentres < 0x10000)
        return false;
      float faithX = pCache[0], faithZ = pCache[2]; // the first focus: the game's camera
      for (int i = 0; i < count; ++i)
      {
        unsigned short z = zones[i];
        zone[i] = z;
        if (z >= 0x8000 || (int)z >= zoneCount)
        {
          tierOf[i] = 3;
          key[i] = FLT_MAX;
          continue;
        }
        float x = pCentres[z * 2], y = pCentres[z * 2 + 1];
        float fx = x - faithX, fz = y - faithZ;
        float faith = sqrtf(fx * fx + fz * fz);
        float cx = x - view.X, cz = y - view.Z;
        float camera = sqrtf(cx * cx + cz * cz);
        if (faith < FaithZoneRadius)
        {
          tierOf[i] = 0;
          key[i] = faith;
        }
        else if (!view.HasLook || camera < InViewNear || (cx * view.LookX + cz * view.LookZ) / camera >= view.MinDot)
        {
          tierOf[i] = 1;
          key[i] = camera;
        }
        else
        {
          tierOf[i] = 2;
          key[i] = camera;
        }
      }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      return false;
    }

    // Insertion sort, it's a few dozen at most (and keeps ties in the game's order)
    for (int i = 1; i < count; ++i)
    {
      unsigned short z = zone[i];
      int t = tierOf[i];
      float k = key[i];
      int j = i - 1;
      while (j >= 0 && (tierOf[j] > t || (tierOf[j] == t && key[j] > k)))
      {
        zone[j + 1] = zone[j];
        tierOf[j + 1] = tierOf[j];
        key[j + 1] = key[j];
        --j;
      }
      zone[j + 1] = z;
      tierOf[j + 1] = t;
      key[j + 1] = k;
    }
    for (int i = 0; i < count; ++i)
      zones[i] = zone[i];
    return true;
  }

  // Settled and room for one more? slots: slots in use, inUse: with the
  // zones asked for; fast: far from every limit (FastFillPacingMs); nearLimit:
  // from the limits' settle levels on (AboveSettleLevel), where all streamers
  // share one gate (see g_LastNearAdd)
  bool MayAddFocus(__int64 pZoneStreamer, int slots, int inUse, bool fast, bool nearLimit)
  {
    ULONGLONG now = GetTickCount64();
    nearLimit = nearLimit && !fast;
    AcquireSRWLockExclusive(&g_GateLock);
    StreamerGate* pGate = FindGate(pZoneStreamer, slots, now);
    if (slots != pGate->LastUsed)
    {
      pGate->LastUsed = slots;
      pGate->LastChange = now;
    }
    bool settled = fast ? now - pGate->LastAdd >= FastFillPacingMs :
      now - pGate->LastChange >= ZoneSlotsSettleMs && now - pGate->LastAdd >= ZoneSlotsSettleMs;
    if (settled && nearLimit && inUse < ZoneSlotsAddBelow())
    {
      // (asking only when it could be given one: one at its zone-slot add
      // level asking every update would hold every other streamer back)
      pGate->NearAsk = now;
      settled = now - g_LastNearAdd >= ZoneSlotsSettleMs &&
        (!g_WaitingSince || now - g_WaitingSince >= AddWaitMostMs);
      // (in turns: not while another streamer asking has waited longer)
      for (StreamerGate const& gate : g_Gates)
        if (settled && &gate != pGate && gate.Streamer && gate.NearAsk && now - gate.NearAsk <= GateWaitingFreshMs &&
          gate.NearServed < pGate->NearServed)
          settled = false;
    }
    bool allowed = settled && inUse < ZoneSlotsAddBelow();
    if (allowed)
    {
      pGate->LastAdd = now;
      if (nearLimit)
      {
        g_LastNearAdd = now;
        pGate->NearServed = now;
        // Gone in while zones were still waiting (after AddWaitMostMs of
        // it): the wait starts again, so the next one waits for the loads
        // (or AddWaitMostMs) as well. Otherwise, with some zone nearly always
        // waiting while the camera or Faith moves (the count is of every zone
        // the streamer was asked for, the game's own too) or one that never
        // loads, every add after the first 10 s went in at the settle pace.
        if (g_WaitingSince)
          g_WaitingSince = now;
      }
    }
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
  const int InstanceReserveFactor = 8;          // times the game's own (4 until the whole city)
  // A new instance takes a free slot if there is one, and only with none left
  // does the list grow (0x1435447E0). The free slots are a linked list per
  // instance list, 0x6060 apart, with its length at +0x10. So a list's end
  // only shows the most there ever were; what counts is the end minus the
  // free slots (a teleport leaves a lot of them behind).
  const int InstanceFreeListOffset = 0x3500B0;
  const int InstanceFreeListStride = 0x6060;
  const int InstanceFreeCountOffset = 0x10;

  // The renderer keeps the bounds of each mesh's instances in chunks of 64
  // (0x2D80 bytes) that it takes from one arena (0x1436C3960): a global at
  // 0x142568C00 with the next free byte (+0), the start (+8) and the size
  // (+0x10: 16 MB, or 128 MB when the tools got there first, see
  // EnlargeRenderArena in Offsets.cpp), set aside once when the game starts
  // (0x1436D4CE0). Chunks
  // are only given back when the renderer is torn down, and nothing checks the
  // end: past it they're written over other memory and the game crashes.
  // Every kind of mesh the camera's zones bring in can take more, so the tools
  // stop well before. PE timestamp 0x5762E3B7 only.
  const __int64 RenderArenaRva = 0x2568C00;
  const DWORD RenderArenaTimeStamp = 0x5762E3B7;
  const float ArenaStopAdding = 0.70f; // no more zones around the camera
  const float ArenaTakeAway = 0.80f;   // the extra zones go (its use doesn't go down, but moving foci would load more)
  const float ArenaTakeAll = 0.88f;    // the camera's own too

  bool IsRenderArenaBuild()
  {
    IMAGE_DOS_HEADER* pDos = (IMAGE_DOS_HEADER*)g_gameHandle;
    IMAGE_NT_HEADERS* pNt = (IMAGE_NT_HEADERS*)((BYTE*)g_gameHandle + pDos->e_lfanew);
    return pNt->FileHeader.TimeDateStamp == RenderArenaTimeStamp;
  }

  // The renderer files every instance into a draw batch: per renderer object
  // (two, found through the array at 0x1424A9B98, owner + 0x48, vtable
  // 0x141BF41D0) four categories (+0x90, 0x28980 bytes each) of up to 9
  // batches (count at +0x28978), 0x4808 bytes each. A batch has room for 2048
  // chunk pointers (+0x130), its chunk count (+0x4130) and one fill byte per
  // chunk (+0x4134), which only has room for 1748 chunks. A new instance goes
  // into the first chunk that isn't full, and only when all are full does the
  // batch take another chunk, with no check: past 1748 the fill bytes go into
  // the next batch, and at 2048 the new chunk's address goes over the batch's
  // own count. Then the bounds job (0x1436D5A00) writes to a garbage address.
  // The game only ever has one batch per category (its constructor,
  // 0x1436C1B03, and the add always uses the first), so the next batch's room
  // is all zeros and the fill bytes past 1748 are safe there (seen 2026-10-22
  // in all 8 categories). Unloading zones gives room back (0x1436D27A0 takes
  // the instance out), but the game finds an instance by a handle that has
  // its chunk in 10 bits: past 1024 chunks it lost them (seen: 36638
  // instances in chunks 1024 - 1596 whose memory was freed). The tools widen
  // it (Offsets.cpp, WidenBatchChunkIndex); without that only 1024 are safe.
  const __int64 RendererOwnersRva = 0x24A9B98;
  const int RendererOwnerCount = 2;
  const int RendererOffset = 0x48;
  const __int64 RendererVTableRva = 0x1BF41D0;
  const int RendererCategoryOffset = 0x90;
  const int RendererCategories = 4;
  const int BatchCountOffset = 0x28978;
  const int MaxBatches = 9;
  const int BatchStride = 0x4808;
  const int BatchChunkCountOffset = 0x4130;
  const int BatchFillOffset = 0x4134;
  const int BatchChunkPointers = 2048;
  // Where the game starts to go wrong: the end of the chunk pointers, or
  // without the wider handle the first 1024
  int BatchSafeChunks() { return util::offsets::BatchChunkIndexWidened() ? BatchChunkPointers : 1024; }
  // A batch's fill bytes only have room for this many chunks: past it they
  // go into the next batch's start (all zeros while it's unused)
  const int BatchFillRoomChunks = 1748;
  // The hold-back levels below are shares of a category's room: 1748 chunks,
  // as they were tested, 300 short of the end; without the wider handle the
  // same share of the first 1024. Only category 0 is spread (hBatchAdd in
  // Hooks.cpp): with the spread on, its 9 batches up to the spread's chunks
  // each (the last can go on to 2048); the other three keep only their first
  // batch whatever the spread, so their room stays 1748 chunks. (Until
  // 2.0.1 the spread's room was taken for whichever category was fullest.)
  int CategoryRoom(int category)
  {
    int spread = util::hooks::BatchSpread();
    if (category == 0 && spread > 0)
      return MaxBatches * spread * 64;
    return (util::offsets::BatchChunkIndexWidened() ? BatchFillRoomChunks : 874) * 64;
  }
  // Zones are held back from BatchLimitFixed (80 %; until 2026-11-01 the
  // menu's Hold back from, 60 - 78 %), taken away 10 % higher and all of
  // them 18 % higher, never past 86 % and 92 %: zones that are still loading
  // go on adding to it after that (at 82 %, taking them all away at 96 % was
  // too late, and it ran past its room). After the batch ran past its room
  // once, BatchLimitDefault (70 %) is used instead.
  const float BatchLimitDefault = 0.70f;
  const float BatchLimitFixed = 0.80f;
  float BatchTakeAwayFrom(float limit) { return limit + 0.10f < 0.86f ? limit + 0.10f : 0.86f; }
  float BatchTakeAllFrom(float limit) { return limit + 0.18f < 0.92f ? limit + 0.18f : 0.92f; }

  // Each renderer object keeps its instances' handles in a list (renderer +
  // 0x142768: begin, end of a u32 array) with room for 200000, or 1000000 once
  // the tools made it bigger (EnlargeRendererInstances in Offsets.cpp), and
  // takes a new one without a check: past it, it writes 16 GB away and
  // crashes. Their records come from a pool with room for 262144, or 1048576
  // once the tools moved its list of blocks (Hooks.cpp, EnlargeRecordPool),
  // with no check either. Every instance it has is in one of its batches.
  const int RendererInstanceListOffset = 0x142768;
  const int RecordRoomMargin = 32768;

  // What a read of the renderer's draw batches found (ReadFullestBatch)
  struct BatchReading
  {
    // The fullest category of both renderer objects for its own room (see
    // CategoryRoom): its instances (all its batches), its room, its chunks
    int Instances, Room, Chunks;
    float Fill;
    int MostInstances;  // the most instances in any one category (what arriving zones show in, see BatchBurst)
    int MostChunks;     // the most chunks any one batch has
    // The fullest single batch against the end of its own chunk pointers
    // (BatchSafeChunks, where the game crashes), whatever its category's room
    float CrashFill;
    // A batch past its room: past its chunk pointers, or with the spread past
    // its fill bytes' room while the next batch is in use (its fill bytes
    // are then written over that batch's start)
    bool Overflowed;
    // The renderer whose instance list is fullest: its instances (all its
    // categories) and the list's room
    int RendererInstances, RendererRoom;
    // For the scan's guess at a level load (see WatchInstanceLists) and
    // whether the spread can be switched (BatchSpreadSwitchSafe): the
    // renderer objects, and in category 0 of any of them the most batches in
    // use and the most chunks in its first batch; Partial: a renderer object
    // without all its categories (those weren't read)
    __int64 Renderers[RendererOwnerCount];
    int FirstBatches, FirstChunks;
    bool Partial;
    // Category 0 with one batch, and its second batch's first bytes not all
    // 0: its first batch's fill bytes past 1748 chunks (written there, they
    // aren't cleared again); the research saw everything after the first
    // batch 0 in a renderer that never had that many
    bool FirstSpilled;
  };

  // 1 when read, 0 when no renderer object is there (no level), -1 when the
  // read failed halfway
  int ReadFullestBatch(BatchReading& r)
  {
    __int64 base = (__int64)g_gameHandle;
    r = BatchReading{};
    float rendererFill = -1.f;
    bool any = false;
    int safeChunks = BatchSafeChunks();
    __try
    {
      for (int o = 0; o < RendererOwnerCount; ++o)
      {
        __int64 pOwner = *(__int64*)(base + RendererOwnersRva + o * 8);
        if (pOwner < 0x10000) continue;
        __int64 pRenderer = *(__int64*)(pOwner + RendererOffset);
        if (pRenderer < 0x10000 || *(__int64*)pRenderer != base + RendererVTableRva) continue;
        any = true;
        r.Renderers[o] = pRenderer;
        __int64* pList = (__int64*)(pRenderer + RendererInstanceListOffset);
        int room = pList[0] && pList[1] > pList[0] ? (int)((pList[1] - pList[0]) / 4) : 0;
        // and the instances' records (a pool of blocks of 1024), less a
        // margin: zones still loading go on adding instances after the tools
        // hold back
        int recordRoom = (int)util::hooks::RecordPoolRoom() - RecordRoomMargin;
        if (room > recordRoom)
          room = recordRoom;
        int total = 0;
        for (int c = 0; c < RendererCategories; ++c)
        {
          __int64 pCategory = *(__int64*)(pRenderer + RendererCategoryOffset + c * 8);
          if (pCategory < 0x10000)
          {
            r.Partial = true;
            continue;
          }
          int batches = *(int*)(pCategory + BatchCountOffset);
          if (batches > MaxBatches) batches = MaxBatches;
          int categoryInstances = 0, categoryChunks = 0;
          for (int b = 0; b < batches; ++b)
          {
            BYTE* pBatch = (BYTE*)(pCategory + (__int64)b * BatchStride);
            int count = *(int*)(pBatch + BatchChunkCountOffset);
            if (c == 0 && b == 0 && count > r.FirstChunks)
              r.FirstChunks = count > 4096 ? 4096 : count;
            if (count <= 0) continue;
            if (count > 4096) count = 4096; // not a count any more: written over
            // The fill bytes past 1748 are in the next batch's room (unused,
            // in the same category, unless it's spread). Past the chunk
            // pointers the count itself has been written over: counted as
            // full, the worst case.
            int readable = count > BatchChunkPointers ? BatchChunkPointers : count;
            int sum = 0;
            for (int i = 0; i < readable; ++i)
              sum += pBatch[BatchFillOffset + i];
            if (count > BatchChunkPointers)
              sum += (count - BatchChunkPointers) * 64;
            if (count > r.MostChunks)
              r.MostChunks = count;
            // A new instance goes into the batch's first chunk that isn't
            // full, and only with none left does it take another chunk, so
            // its instances (not its chunk count: the research only saw that
            // grow while the renderer lives) say how close it is to taking
            // its last one
            float crashFill = (float)sum / (float)(safeChunks * 64);
            if (crashFill > r.CrashFill)
              r.CrashFill = crashFill;
            if (count > safeChunks || (count > BatchFillRoomChunks && b + 1 < batches))
              r.Overflowed = true;
            categoryInstances += sum;
            categoryChunks += count;
          }
          if (c == 0 && batches > r.FirstBatches)
            r.FirstBatches = batches;
          if (c == 0 && batches == 1)
          {
            BYTE const* pNext = (BYTE const*)(pCategory + BatchStride);
            for (int i = 0; i < BatchChunkPointers - BatchFillRoomChunks && !r.FirstSpilled; ++i)
              r.FirstSpilled = pNext[i] != 0;
          }
          int categoryRoom = CategoryRoom(c);
          float categoryFill = (float)categoryInstances / (float)categoryRoom;
          if (categoryFill > r.Fill)
          {
            r.Fill = categoryFill;
            r.Instances = categoryInstances;
            r.Room = categoryRoom;
            r.Chunks = categoryChunks;
          }
          if (categoryInstances > r.MostInstances)
            r.MostInstances = categoryInstances;
          total += categoryInstances;
        }
        float fill = room > 0 ? (float)total / (float)room : 0.f;
        if (room > 0 && fill > rendererFill)
        {
          rendererFill = fill;
          r.RendererInstances = total;
          r.RendererRoom = room;
        }
      }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      return -1;
    }
    // Nothing in any category yet: the room of the first
    if (r.Room <= 0)
      r.Room = CategoryRoom(0);
    return any ? 1 : 0;
  }

  // Whether the spread can be switched now with renderer objects there (with
  // none it always can): in every one category 0 (the only one spread) has
  // only ever used its first batch, and that batch has never run past 1748
  // chunks (FirstSpilled), so batches 1 - 8 are still as the renderer's
  // constructor made them (zeros); and that batch is BatchSwitchMargin
  // chunks or more short of where the add's picking changes (PickBatch): the
  // spread's chunks, now and after the switch, and 1748, past which it keeps
  // the first batch (that one then runs on to 2048 as the game's own does).
  // Switched then, it's as if the renderer had had the new setting from its
  // start, unless more than BatchSwitchMargin chunks arrive between the read
  // and the switch: switched on, a first batch past 1748 is kept (as the
  // game's own) and the per-batch guard still takes zones away; switched off,
  // a category already in its other batches stays spread in the add's hook
  // (Hooks.cpp, g_BatchSpreadLast). fromChunks / toChunks: the spread now and
  // after (0 off).
  const int BatchSwitchMargin = 128;
  bool BatchSpreadSwitchSafe(BatchReading const& r, int fromChunks, int toChunks)
  {
    if (r.Partial || r.FirstBatches > 1 || r.FirstSpilled)
      return false;
    int edge = BatchFillRoomChunks;
    if (fromChunks > 0 && fromChunks < edge) edge = fromChunks;
    if (toChunks > 0 && toChunks < edge) edge = toChunks;
    return r.FirstChunks + BatchSwitchMargin <= edge;
  }

  bool ReadRenderArena(__int64& used, __int64& size)
  {
    __try
    {
      __int64* pArena = (__int64*)((__int64)g_gameHandle + RenderArenaRva);
      __int64 next = pArena[0], start = pArena[1];
      unsigned int bytes = *(unsigned int*)(pArena + 2);
      if (!start || next < start || !bytes)
        return false;
      used = next - start;
      size = bytes;
      return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      return false;
    }
  }

  // The mesh table: a vector at +0x50 (begin, end, capacity) with room for
  // 4096 meshes of 0x350 bytes inside the manager (+0x70 up to the instance
  // lists at +0x350070). A new mesh takes a free record (first qword 0) if
  // there is one, otherwise the end moves on (0x143551D50), without looking
  // at the capacity: past 4096 it writes over the instance lists, and the
  // game crashes. So the end only shows the most there ever were in this
  // level; what counts is how many records are in use. The tools move it to
  // a block with room for 16384 when the game starts (Hooks.cpp,
  // hMeshManagerCtor); MeshCapacity() is what it has.
  const int MeshTableOffset = 0x50;
  const int MeshTableInline = 0x70;
  const int MeshRecordSize = 0x350;
  int MeshCapacity() { return util::hooks::MeshTableRoom(); }
  // One zone can bring in a few hundred meshes at once, so the limits leave
  // that much room below the end (with 4096: 3450, 3650, 3800; 4000 still
  // went over it)
  int MeshesStopAdding() { return MeshCapacity() - 646; } // no more zones around the camera from here
  int MeshesTakeAway() { return MeshCapacity() - 446; }   // two of the camera's extra zones go
  int MeshesTakeAll() { return MeshCapacity() - 296; }    // all of them go, the camera's own too
  // The meshes of a focus's zones register in bursts as they load. Near the
  // limit another focus is only added once the count has stopped rising (by
  // more than MeshesBurst) for this long, so two foci's bursts can't land on
  // top of each other. (Further
  // down the game's own streaming around Faith keeps the count moving, and a
  // burst can't reach the limit anyway.)
  int MeshesSettleFrom() { return MeshCapacity() - 1296; }
  const ULONGLONG MeshesSettleMs = 1000;
  const int MeshesBurst = 16;            // smaller rises (and drops) don't count as arriving
  std::atomic<ULONGLONG> g_MeshesChangedAt{ 0 };
  std::atomic<int> g_MeshesNow{ 0 };
  int g_MeshesMark = 0;                  // camera hook only (game thread)

  bool MeshesSettled()
  {
    return g_MeshesNow < MeshesSettleFrom() || GetTickCount64() - g_MeshesChangedAt >= MeshesSettleMs;
  }

  // The same for the renderer's draw batch: a focus's zones keep adding to it
  // for a while after it goes in (they load one after another), and adding
  // the next one before that let it run past its room. From 20 % below the
  // hold-back level on (half full by default), another focus waits until it
  // hasn't risen by more than BatchBurst for BatchSettleMs.
  const int BatchBurst = 1000;
  const ULONGLONG BatchSettleMs = 1500;
  const float BatchSettleBelow = 0.2f;
  // The log at the limit: the first warning, then at most a summary this often
  const ULONGLONG BatchLogSummaryMs = 30000;
  // GAME > Level streaming > Room for the whole city (the batch spread,
  // [Misc] BatchSpreadChunks): category 0 of the renderer's draw batches goes
  // on into its unused batches past this many chunks each. The research's
  // whole city (every district Always, every zone of the three streamers
  // loaded, 361029 renderer instances) was with 1700; without it one batch
  // has room for 111872, and the whole city needs far more than that.
  const int BatchSpreadDefault = 1700;
  std::atomic<ULONGLONG> g_BatchChangedAt{ 0 };
  std::atomic<float> g_BatchFillNow{ 0.f };
  std::atomic<float> g_BatchSettleFrom{ 0.5f };
  int g_BatchMark = 0;                   // camera hook only (game thread)

  bool BatchSettled()
  {
    return g_BatchFillNow < g_BatchSettleFrom || GetTickCount64() - g_BatchChangedAt >= BatchSettleMs;
  }

  // From the settle levels on (the draw batch's and the meshes', where each
  // streamer already waited for the counts to settle in 2.0.0): near enough a
  // limit for the gate all streamers share (MayAddFocus). Not the model
  // instance lists or the renderer's memory: with the whole city loaded (the
  // research's run with every district Always) those stood at 63 % and 51 %,
  // which from their settle levels would have put the end of it through the
  // shared gate, one focus at a time waiting for every zone to load.
  bool AboveSettleLevel()
  {
    return g_BatchFillNow >= g_BatchSettleFrom || g_MeshesNow >= MeshesSettleFrom();
  }

  struct InstanceLists
  {
    int Count[2];   // up to the end
    int Free[2];    // free slots before the end
    int Capacity[2];
    int Reserved[2];
    __int64 Begin[2];
    __int64 pSettings;
    int Meshes;
    bool MeshesInRoom; // in the manager's own room for 4096, or the tools' bigger one
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
      if (count > MeshCapacity() + 64) count = MeshCapacity() + 64;
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
        int free = *(int*)(pManager + InstanceFreeListOffset + k * InstanceFreeListStride + InstanceFreeCountOffset);
        lists.Free[k] = free >= 0 && free <= lists.Count[k] ? free : 0;
      }

      __int64* pMeshes = (__int64*)(pManager + MeshTableOffset);
      __int64 meshBegin = pMeshes[0], meshEnd = pMeshes[1];
      if (meshEnd < meshBegin) return false;
      lists.Meshes = meshBegin ? (int)((meshEnd - meshBegin) / MeshRecordSize) : 0;
      lists.MeshesInRoom = !meshBegin || meshBegin == pManager + MeshTableInline || meshBegin == util::hooks::MeshTableBegin();
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
    int count = 0;
    __int64* pSlots = util::hooks::ZoneSlots(pZoneStreamer, count);
    int used = 0;
    for (int i = 0; pSlots && i < count; ++i)
      used += pSlots[i] ? 1 : 0;
    return used;
  }

  // Adds and removes the tools' foci in one streamer's list to match
  // wanted[], within the zone slot budget above, in order[] (most important
  // first, see PublishStreamingFocus): what's most important goes in first
  // and what's least important goes first. fastFill: far from the renderer's
  // and the mesh streaming's limits (see FastFillPacingMs). Gives how many
  // were in the list before and after, how many zones were in use or asked
  // for, and which foci are in it. results: where each focus is (for the
  // holds, see FocusHold).
  // The foci are added with the streamer as their key, which the focus gets
  // back when the streamer asks where it is (GetStreamingFocus). The game's
  // remove function writes out of bounds for a focus that isn't there, so
  // it's only called for ones found in the list.
  void SyncStreamingFoci(__int64 pZoneStreamer, bool const wanted[], int const order[], int const tier[],
    ZoneFocusResult const results[], __int64 pAdd, __int64 pRemove, int instancePressure, bool swapUseful, bool fastFill,
    int& before, int& after, int& usedZones, bool& limited, bool present[])
  {
    before = after = usedZones = 0;
    limited = false;
    for (int i = 0; i < MaxStreamingFoci; ++i)
      present[i] = false;
    __try
    {
      __int64 pFocusList = pZoneStreamer + ZoneFocusListOffset;
      ZoneFocusRecord* pRecords = *(ZoneFocusRecord**)(pFocusList + ZoneFocusRecordsOffset);
      unsigned int count = *(unsigned int*)(pFocusList + ZoneFocusCountOffset);
      if (count > MaxZoneFoci || (count && !pRecords))
        return;

      for (unsigned int i = 0; i < count; ++i)
      {
        StreamingFocus* pFocus = (StreamingFocus*)pRecords[i].pFocus;
        if (pFocus >= &g_Foci[0] && pFocus < &g_Foci[MaxStreamingFoci] && !(pRecords[i].Flags & ZoneFocusRemoved))
          present[pFocus - &g_Foci[0]] = true;
      }
      for (int i = 0; i < MaxStreamingFoci; ++i)
        before += present[i] ? 1 : 0;

      int usedSlots = CountUsedZoneSlots(pZoneStreamer);
      int zonesWaiting = 0;
      usedZones = CountZonesInUse(pZoneStreamer, zonesWaiting);
      if (usedZones < usedSlots)
        usedZones = usedSlots; // couldn't be read: the slots at least
      // For the gate all streamers share near the limits
      NoteZonesWaiting(pZoneStreamer, usedSlots, zonesWaiting);
      auto remove = [&](int i) {
        ((tZoneFocusChange)pRemove)(pFocusList, pZoneStreamer, &g_Foci[i]);
        present[i] = false;
      };

      // Foci that aren't wanted any more go straight away
      for (int i = 0; i < MaxStreamingFoci; ++i)
      {
        if (present[i] && !wanted[i])
          remove(i);
      }

      // The foci held out since they were taken away for room (see
      // FocusHold), and the ones taken away now (at most two, and one to
      // make room for one in view), held out at the end
      ULONGLONG now = GetTickCount64();
      bool heldOut[MaxStreamingFoci];
      ULONGLONG heldUntil[MaxStreamingFoci];
      AcquireSRWLockShared(&g_GateLock);
      for (int i = 0; i < MaxStreamingFoci; ++i)
      {
        heldOut[i] = g_FocusHolds[i].Out;
        heldUntil[i] = g_FocusHolds[i].Until;
      }
      ReleaseSRWLockShared(&g_GateLock);
      const int MostTakenAway = 4;
      int takenAway[MostTakenAway];
      int takenCount = 0;
      auto takeAway = [&](int i) {
        remove(i);
        heldOut[i] = true;
        if (takenCount < MostTakenAway)
          takenAway[takenCount++] = i;
      };

      // The camera's own only when the game is already out of room; the one
      // ahead of it whenever the others go, after all of them. Kept in until
      // the top level as well, the moving point ahead went on loading new
      // zones and took the draw batch past the top level (95 %), then in and
      // out every second. (By which focus it is: with Load ahead off, second
      // in the order is one of the others.)
      bool mayGo[2] = { instancePressure >= 3, true };
      if (usedZones > ZoneSlotsRemoveAbove() || instancePressure >= 2)
      {
        // Too full: the two least important go. Near the end of the zone slots,
        // or out of room, on every update; otherwise paced (RemovePacingMs)
        bool urgent = usedZones > ZoneSlotsRemoveAbove() || instancePressure >= 3;
        int removed = 0;
        if (urgent || TakeTurn(pZoneStreamer, Turn_Remove, RemovePacingMs))
        {
          // In three rounds, the least important first in each: the camera's
          // extra zones and the Always districts; then the zones the game's
          // rain is in and the real Shard's, only right at the end (urgent)
          // and once all of those have gone (in a tester's log the game's
          // rain went when its zone was taken away with the rest); then the
          // camera's own and the one ahead of it
          for (int round = 0; round < 3 && removed < 2; ++round)
          {
            for (int rank = MaxStreamingFoci - 1; rank >= 0 && removed < 2; --rank)
            {
              int i = order[rank];
              if (!present[i])
                continue;
              bool ownFocus = i == FocusCamera || i == FocusAhead;
              bool kept = i >= FocusWeatherFirst; // the weather's foci and FocusShard
              if ((ownFocus ? 2 : (kept ? 1 : 0)) != round || (ownFocus && !mayGo[i]) || (kept && !urgent))
                continue;
              takeAway(i);
              removed++;
            }
          }
        }
        // Held back: some of the camera's were taken away or are waiting
        // (with the camera off there are none, and nothing to say)
        bool waiting = false;
        for (int i = 0; i < MaxStreamingFoci; ++i)
          waiting |= wanted[i] && !present[i];
        limited = removed > 0 || waiting;
      }
      else
      {
        // One at a time, so the slots it fills show up before the next is
        // added. The camera's own focus and the one ahead of it go in whenever
        // there's room for anything, while the rest are held back or not
        // (unless they're held out themselves, below); the rest once the
        // slots have caught up with the last one (or far from every limit, at
        // a steady pace; near the limits one at a time in all streamers, once
        // the zones asked for have loaded: see MayAddFocus).
        bool fast = fastFill && usedZones < FastFillZonesBelow && zonesWaiting <= FastFillMostWaiting;
        bool nearLimit = AboveSettleLevel();
        // A focus held out comes back once there's room again: nothing at
        // any level, the fullest limit well under its hold-back level, the
        // zone slots under theirs, and its time up. The camera's own and the
        // one ahead of it as in 2.0.0 (nothing at any level, the zone slots
        // under theirs) once their time is up and the draw batch and the
        // meshes have settled (they skip the gates below): in busy parts
        // what's loaded around Faith alone can keep the draw batch over 70 %
        // without the room for the whole city, and held out they'd hold back
        // everything after them for as long as the camera stays there.
        bool roomAgain = instancePressure == 0 && g_LimitUse < HoldBackUseRelease && usedZones <= ZoneSlotsAddBelow();
        bool ownRoomAgain = instancePressure == 0 && usedZones <= ZoneSlotsAddBelow() && MeshesSettled() && BatchSettled();
        for (int rank = 0; rank < MaxStreamingFoci; ++rank)
        {
          int i = order[rank];
          if (!wanted[i] || present[i])
            continue;
          bool ownFocus = i == FocusCamera || i == FocusAhead;
          if (heldOut[i] && !((ownFocus ? ownRoomAgain : roomAgain) && now >= heldUntil[i]))
          {
            limited = true;
            break;
          }
          if (!ownFocus && (instancePressure >= 1 || !MeshesSettled() || !BatchSettled() ||
            !MayAddFocus(pZoneStreamer, usedSlots, usedZones, fast, nearLimit)))
          {
            limited = usedZones >= ZoneSlotsAddBelow() || instancePressure >= 1;
            // Held back while something less important behind the camera is
            // loaded: that one makes room for this one (in view), one a second
            // at most. Not when it's the renderer's instance memory that's
            // full: that doesn't empty again.
            if (limited && swapUseful && tier[i] <= TierInView && MeshesSettled() && BatchSettled())
            {
              for (int back = MaxStreamingFoci - 1; back > rank && back >= 2; --back)
              {
                int j = order[back];
                if (present[j] && tier[j] >= TierTrack)
                {
                  if (TakeTurn(pZoneStreamer, Turn_Swap, SwapPacingMs))
                    takeAway(j);
                  break;
                }
              }
            }
            break;
          }
          ((tZoneFocusChange)pAdd)(pFocusList, pZoneStreamer, &g_Foci[i]);
          present[i] = true;
          if (heldOut[i])
          {
            AcquireSRWLockExclusive(&g_GateLock);
            g_FocusHolds[i].Out = false;
            g_FocusHolds[i].BackAt = now;
            ReleaseSRWLockExclusive(&g_GateLock);
          }
          break;
        }
      }

      // The ones taken away for room: held out (another streamer's list may
      // have had them too, they keep their time then)
      if (takenCount > 0)
      {
        AcquireSRWLockExclusive(&g_GateLock);
        for (int k = 0; k < takenCount; ++k)
        {
          int i = takenAway[k];
          HoldOut(i, now, results[i].Position[0], results[i].Position[2], tier[i]);
        }
        ReleaseSRWLockExclusive(&g_GateLock);
        g_TakeAways += takenCount;
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

  // Never: a district's zones are unloaded the way the game unloads a zone
  // nothing wants any more, and kept from loading again whatever wants them
  // (zone streamer hook, before the streamer's update; see ZoneLoadAsked):
  // - A loaded (or loading) zone of it is asked to unload, and the game's own
  //   step (0x143C81CD0: release, load, clear the requests; the game also
  //   runs it at a level load) releases it, as on any unload.
  // - Once it has no slot it's made unavailable, so the game doesn't load it.
  //   The foci that want it keep their references and let go as usual.
  // Back to Normal (or kept, below) it's made available again and asked to
  // load if anything wants it, as the game does when a zone becomes
  // available (0x143C81BAC); only with zone slots to spare (the game's slot
  // search doesn't check for a free one).
  // The zones the game's own foci are in and the zones next to them are
  // kept: around Faith, the ground under her.
  const int NeverSlotsSpare = 16;
  const int MaxGameFoci = 16;
  typedef void(__fastcall* tZoneStep)(__int64 pZoneStreamer);

  struct NeverStreamer
  {
    __int64 Streamer;
    __int64 Data;              // its data when its zones were made unavailable
    std::vector<char> Blocked; // made unavailable by the tools
    int BlockedCount;
  };
  std::vector<NeverStreamer> g_NeverStreamers; // zone streamer hook only
  bool g_NeverStepFailed = false;

  // The zones the game's own foci are in (not the tools'). Gives how many
  // foci it has, -1 if the list couldn't be read.
  int ReadGameFocusZones(__int64 pZoneStreamer, int* zones, int max, int& zoneCount)
  {
    zoneCount = 0;
    __try
    {
      __int64 pFocusList = pZoneStreamer + ZoneFocusListOffset;
      ZoneFocusRecord* pRecords = *(ZoneFocusRecord**)(pFocusList + ZoneFocusRecordsOffset);
      unsigned int count = *(unsigned int*)(pFocusList + ZoneFocusCountOffset);
      if (count > MaxZoneFoci || (count && !pRecords))
        return -1;
      int foci = 0;
      for (unsigned int i = 0; i < count; ++i)
      {
        StreamingFocus* pFocus = (StreamingFocus*)pRecords[i].pFocus;
        if (pFocus >= &g_Foci[0] && pFocus < &g_Foci[MaxStreamingFoci])
          continue;
        ++foci;
        if (pRecords[i].Zone >= 0 && zoneCount < max)
          zones[zoneCount++] = pRecords[i].Zone;
      }
      return foci;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      return -1;
    }
  }

  // One pass over a streamer's zones. never[k]: in a Never district; kept[k]:
  // held by the game's own foci. Marks released[k] for the zones asked to
  // unload. Gives how many zones were asked to unload and to load, false if
  // the table couldn't be read.
  bool MarkNeverZones(__int64 pZoneStreamer, int count, char const* never, char const* kept, char* blocked,
    char* released, int& blockedCount, int& unloadAsked, int& loadAsked)
  {
    unloadAsked = loadAsked = 0;
    __try
    {
      unsigned char* pTable = *(unsigned char**)(pZoneStreamer + ZoneTableOffset);
      if ((__int64)pTable < 0x10000)
        return false;
      int spare = ZoneSlotCount() - CountUsedZoneSlots(pZoneStreamer) - NeverSlotsSpare;
      for (int k = 0; k < count; ++k)
      {
        unsigned char* pEntry = pTable + (__int64)k * 8;
        unsigned short slot = *(unsigned short*)(pEntry + 2);
        unsigned short refs = *(unsigned short*)(pEntry + 4);
        released[k] = 0;
        if (never[k] && !kept[k])
        {
          // Not available: already kept out, or the game's own (not installed)
          if (!(pEntry[0] & ZoneAvailableFlag))
            continue;
          // Still loading: left for a later update, once it's loaded.
          // Released now, it would keep its loading bit without a slot, and
          // the game's next check (0x143C81C40) would read slot -1
          if (pEntry[0] & ZoneLoadingFlag)
            continue;
          if (slot)
          {
            pEntry[0] = (unsigned char)((pEntry[0] | ZoneUnloadAsked) & ~ZoneLoadAsked);
            released[k] = 1;
            ++unloadAsked;
          }
          else
          {
            pEntry[0] = (unsigned char)(pEntry[0] & ~(ZoneAvailableFlag | ZoneLoadAsked));
            if (!blocked[k])
            {
              blocked[k] = 1;
              ++blockedCount;
            }
          }
        }
        else if (blocked[k])
        {
          // Only what the tools made unavailable, and only asked to load when
          // wanted and not loaded: a zone with a slot asked to load would be
          // loaded again into another (the same streamer back with the same
          // data, say after a reload, has its zones loaded and available)
          bool available = (pEntry[0] & ZoneAvailableFlag) != 0;
          bool load = !available && refs && !slot;
          if (load && spare <= 0)
            continue; // the next update
          blocked[k] = 0;
          --blockedCount;
          if (available)
            continue;
          pEntry[0] |= ZoneAvailableFlag;
          if (load)
          {
            pEntry[0] = (unsigned char)((pEntry[0] | ZoneLoadAsked) & ~ZoneUnloadAsked);
            ++loadAsked;
            --spare;
          }
        }
      }
      return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      return false;
    }
  }

  // After the game's step: the zones it released are made unavailable
  void BlockReleasedZones(__int64 pZoneStreamer, int count, char const* released, char* blocked, int& blockedCount)
  {
    __try
    {
      unsigned char* pTable = *(unsigned char**)(pZoneStreamer + ZoneTableOffset);
      if ((__int64)pTable < 0x10000)
        return;
      for (int k = 0; k < count; ++k)
      {
        unsigned char* pEntry = pTable + (__int64)k * 8;
        if (!released[k] || *(unsigned short*)(pEntry + 2) || !(pEntry[0] & ZoneAvailableFlag))
          continue;
        pEntry[0] = (unsigned char)(pEntry[0] & ~(ZoneAvailableFlag | ZoneLoadAsked));
        if (!blocked[k])
        {
          blocked[k] = 1;
          ++blockedCount;
        }
      }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
  }

  // The tools' step comes before the game's update, so before its check of
  // the loading bundles (0x143C81C40), which drops the loading bit of those
  // asked to unload ahead of its own step. Done here the same way for any
  // the game itself has asked to unload: released with the bit still on,
  // a bundle would be left loading without a slot.
  void DropLoadingOfUnloads(__int64 pZoneStreamer)
  {
    __try
    {
      unsigned char* pTable = *(unsigned char**)(pZoneStreamer + ZoneTableOffset);
      __int64 data = *(__int64*)(pZoneStreamer + ZoneDataOffset);
      __int64 pNames = data >= 0x10000 ? *(__int64*)(data + ZoneBundleNamesOffset) : 0;
      if ((__int64)pTable < 0x10000 || pNames < 0x10000)
        return;
      unsigned int bundles = *(unsigned int*)(pNames - 4);
      for (unsigned int k = 0; k < bundles; ++k)
      {
        unsigned char* pEntry = pTable + (__int64)k * 8;
        if ((pEntry[0] & ZoneUnloadAsked) && (pEntry[0] & ZoneLoadingFlag))
          pEntry[0] = (unsigned char)(pEntry[0] & ~ZoneLoadingFlag);
      }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
  }

  bool RunZoneStep(__int64 pZoneStep, __int64 pZoneStreamer)
  {
    DropLoadingOfUnloads(pZoneStreamer);
    __try
    {
      ((tZoneStep)pZoneStep)(pZoneStreamer);
      return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      return false;
    }
  }

  // Whether a zone is asked to load (its table entry's request bit)
  bool IsZoneLoadAsked(__int64 pZoneStreamer, int zone)
  {
    __try
    {
      unsigned char const* pTable = *(unsigned char const**)(pZoneStreamer + ZoneTableOffset);
      return (__int64)pTable >= 0x10000 && (pTable[(__int64)zone * 8] & ZoneLoadAsked) != 0;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      return false;
    }
  }

  // paused: as if no district were Never (what was kept out is let back).
  // shardOut: the real Shard's zone kept out too, the same way (see
  // ShardZoneName); only the buildings' streamer has it
  void EnforceNeverDistricts(__int64 pZoneStreamer, __int64 pZoneStep, bool paused, bool shardOut)
  {
    if (!pZoneStep || g_NeverStepFailed)
      return;
    NeverStreamer* pState = nullptr;
    for (NeverStreamer& state : g_NeverStreamers)
      if (state.Streamer == pZoneStreamer) pState = &state;
    bool anyNever = g_AnyNeverDistrict && !paused;
    shardOut = shardOut && !paused;
    if (!anyNever && !shardOut && (!pState || pState->BlockedCount == 0))
      return;

    // The zones the game's own foci hold. None found (a loading screen):
    // nothing changes while a district is set to Never, as what's under Faith
    // can't be told
    int gameZones[MaxGameFoci];
    int gameZoneCount = 0;
    int gameFoci = ReadGameFocusZones(pZoneStreamer, gameZones, MaxGameFoci, gameZoneCount);
    if ((anyNever || shardOut) && gameFoci <= 0)
      return;

    static std::vector<char> s_never, s_kept, s_released; // zone streamer hook only
    int count = 0;
    __int64 data = 0;
    AcquireSRWLockShared(&g_DistrictLock);
    for (DistrictLayer const& layer : g_DistrictLayers)
    {
      if (layer.Streamer != pZoneStreamer || !layer.HasDistricts)
        continue;
      count = (int)layer.Zones.size();
      data = layer.Data;
      s_never.assign(count, 0);
      s_kept.assign(count, 0);
      for (int k = 0; k < count; ++k)
      {
        int d = layer.Zones[k].District;
        s_never[k] = anyNever && d >= 0 && d < (int)g_Districts.size() && g_Districts[d].State == District_Never;
      }
      for (int g = 0; g < gameZoneCount; ++g)
      {
        int z = gameZones[g];
        if (z < 0 || z >= count)
          continue;
        s_kept[z] = 1;
        for (unsigned short n : layer.Zones[z].Neighbours)
          if (n < count) s_kept[n] = 1;
      }
      break;
    }
    ReleaseSRWLockShared(&g_DistrictLock);
    if (count <= 0 || !data)
      return;
    static ShardZoneCache s_shardZones{}; // zone streamer hook only
    int shard = ShardZoneOf(data, s_shardZones);
    if (shard >= count)
      shard = -1;
    if (shardOut && shard >= 0)
      s_never[shard] = 1;

    if (!pState)
    {
      g_NeverStreamers.push_back({ pZoneStreamer, data, std::vector<char>(count, 0), 0 });
      pState = &g_NeverStreamers.back();
    }
    else if (pState->Data != data || (int)pState->Blocked.size() != count)
    {
      // Another level's zones: what was made unavailable went with the old table
      pState->Data = data;
      pState->Blocked.assign(count, 0);
      pState->BlockedCount = 0;
    }

    s_released.assign(count, 0);
    int unloadAsked = 0, loadAsked = 0;
    bool shardWasOut = shard >= 0 && pState->Blocked[shard];
    if (!MarkNeverZones(pZoneStreamer, count, s_never.data(), s_kept.data(), pState->Blocked.data(), s_released.data(),
      pState->BlockedCount, unloadAsked, loadAsked))
      return;
    if (unloadAsked + loadAsked == 0)
      return;
    // The Shard's own share, logged apart from the districts'
    int shardUnload = shard >= 0 && s_released[shard] ? 1 : 0;
    int shardLoad = shard >= 0 && shardWasOut && !pState->Blocked[shard] && IsZoneLoadAsked(pZoneStreamer, shard) ? 1 : 0;
    if (!RunZoneStep(pZoneStep, pZoneStreamer))
    {
      g_NeverStepFailed = true;
      util::log::Error("Districts set to Never: the game's zone step failed on zone streamer 0x%I64X, they're left loaded from now on",
        pZoneStreamer);
      return;
    }
    BlockReleasedZones(pZoneStreamer, count, s_released.data(), pState->Blocked.data(), pState->BlockedCount);
    if (unloadAsked > shardUnload)
      util::log::Write("Districts set to Never: %d zones of zone streamer 0x%I64X unloaded (%d kept out in all)",
        unloadAsked - shardUnload, pZoneStreamer, pState->BlockedCount);
    if (loadAsked > shardLoad)
      util::log::Write("Districts back from Never: %d zones of zone streamer 0x%I64X loading again (%d still kept out)",
        loadAsked - shardLoad, pZoneStreamer, pState->BlockedCount);
    if (shardUnload)
      util::log::Write("The Shard: its real tower (zone %s of zone streamer 0x%I64X) unloaded, the view is in the city's area",
        ShardZoneName, pZoneStreamer);
    if (shardLoad)
      util::log::Write("The Shard: its real tower (zone %s of zone streamer 0x%I64X) loading again", ShardZoneName, pZoneStreamer);
  }
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
  m_ppListenerSpace = util::offsets::GetOffset("OFFSET_LISTENERSPACE");
  if (!m_ppListenerSpace)
    util::log::Warning("The sound listener's space wasn't found, sounds stay heard from the game's camera");
  m_ppClientGameContext = util::offsets::GetOffset("OFFSET_CLIENTGAMECONTEXT");
  m_p3PToggle = util::offsets::GetOffset("OFFSET_3PTOGGLE");
  m_ppMeshSettings = util::offsets::GetOffset("OFFSET_MESHSETTINGS");
  m_ppWorldRenderSettings = util::offsets::GetOffset("OFFSET_WORLDRENDERSETTINGS");
  for (int i = 0; i < MaxStreamingFoci; ++i)
    g_Foci[i] = { g_FocusVTable, i };
  // Until the first publish: the camera, ahead of it, then the rest
  for (int i = 0; i < MaxStreamingFoci; ++i)
  {
    g_FocusOrder[i] = i;
    g_FocusTier[i] = i <= FocusAhead ? TierCamera : TierBehind;
  }

  m_ppMeshStreaming = util::offsets::GetOffset("OFFSET_MESHSTREAMING");
  m_pAddZoneFocus = util::offsets::GetOffset("OFFSET_ZONEFOCUSADD");
  m_pRemoveZoneFocus = util::offsets::GetOffset("OFFSET_ZONEFOCUSREMOVE");
  m_pZoneStep = util::offsets::GetOffset("OFFSET_ZONESTEP");
  if (!m_pZoneStep)
    util::log::Warning("The zone streamer's step wasn't found: districts set to Never won't be unloaded");
}

CameraManager::~CameraManager()
{

}

igcs::capture::Pose CameraManager::GetIgcsPose() const
{
  igcs::capture::Pose pose;
  pose.position = { m_Camera.Position.x, m_Camera.Position.y, m_Camera.Position.z };
  pose.rotation = { m_Camera.Rotation.x, m_Camera.Rotation.y, m_Camera.Rotation.z, m_Camera.Rotation.w };
  pose.fovDegrees = m_Camera.FieldOfView;
  return pose;
}

void CameraManager::ApplyIgcsPose(igcs::capture::Pose const& pose)
{
  igcs::capture::Basis basis;
  if (!igcs::capture::BuildBasis(pose.rotation, basis))
    return;

  m_Camera.Position = XMFLOAT3(pose.position.x, pose.position.y, pose.position.z);
  m_Camera.Rotation = XMFLOAT4(pose.rotation.x, pose.rotation.y, pose.rotation.z, pose.rotation.w);
  m_Camera.FieldOfView = pose.fovDegrees;
  m_Camera.Transform = XMFLOAT4X4(
    basis.right.x, basis.right.y, basis.right.z, 0.f,
    basis.up.x, basis.up.y, basis.up.z, 0.f,
    basis.forward.x, basis.forward.y, basis.forward.z, 0.f,
    pose.position.x, pose.position.y, pose.position.z, 1.f);
}

void CameraManager::ClearCameraDeltas()
{
  m_Camera.dX = m_Camera.dY = m_Camera.dZ = 0.f;
  m_Camera.dPitch = m_Camera.dYaw = m_Camera.dRoll = m_Camera.dFov = 0.f;
  m_Camera.dMousePitch = m_Camera.dMouseYaw = 0.f;
}

void CameraManager::ResetIgcsTransientState()
{
  ClearCameraDeltas();
  m_HasLastFrame = false;
  m_AttachHasLast = false;
  m_AttachValid = false;
  m_AttachTrackActive = false;
  m_AttachRequest = AttachRequest_None;
  m_AttachPickRequest = AttachRequest_None;
  m_AttachPicking = false;
  m_AttachPickRemaining = 0;
  m_AttachScan.reset();
  m_AttachListRequested = false;
  m_AttachPlaceRequested = false;
  m_StreamHasLast = false;
  m_StreamVelocity = XMFLOAT3(0, 0, 0);
}

int CameraManager::StartIgcsSession(std::uint8_t type)
{
  std::lock_guard<std::mutex> lock(m_Mutex);
  if (type != 1) return 4;
  if (m_IgcsSession.IsActive()) return 3;
  if (!m_CameraEnabled) return 1;
  if (m_TrackPlayer.IsPlaying()) return 2;
  if (!m_IgcsViewReady || !GetPlayerSoldier()) return 5;

  int result = m_IgcsSession.Start(type, m_CameraEnabled, false, GetIgcsPose());
  if (result != 0) return result;

  ApplyIgcsPose(m_IgcsSession.GetCurrent());
  ResetIgcsTransientState();
  m_IgcsRestoreFrame = false;
  m_IgcsSessionActive = true;
  try { util::log::Write("IGCS capture started; End or Cancel in ReShade releases the camera"); }
  catch (...) {}
  return 0;
}

void CameraManager::EndIgcsSession()
{
  std::lock_guard<std::mutex> lock(m_Mutex);
  igcs::capture::Pose restored;
  if (!m_IgcsSession.End(restored)) return;

  ApplyIgcsPose(restored);
  ResetIgcsTransientState();
  m_IgcsRestoreFrame = true;
  m_IgcsSessionActive = false;
  try { util::log::Write("IGCS capture ended; camera restored"); }
  catch (...) {}
}

void CameraManager::MoveIgcsCamera(float horizontal, float vertical, float fovDegrees, bool fromStartPosition)
{
  std::lock_guard<std::mutex> lock(m_Mutex);
  if (m_IgcsSession.Move(horizontal, vertical, fovDegrees, fromStartPosition))
    ApplyIgcsPose(m_IgcsSession.GetCurrent());
}

igcs::CameraData CameraManager::GetIgcsCameraData()
{
  std::lock_guard<std::mutex> lock(m_Mutex);
  igcs::CameraData data{};
  igcs::capture::Pose pose = GetIgcsPose();
  igcs::capture::Basis basis;
  if (!igcs::capture::IsValidPose(pose) || !igcs::capture::BuildBasis(pose.rotation, basis))
    return data;

  data.cameraEnabled = m_CameraEnabled && m_IgcsViewReady ? 1 : 0;
  data.cameraMovementLocked = m_IgcsSession.IsActive() ? 1 : 0;
  data.fov = pose.fovDegrees;
  data.coordinates[0] = pose.position.x;
  data.coordinates[1] = pose.position.y;
  data.coordinates[2] = pose.position.z;
  double length = std::sqrt((double)pose.rotation.x * pose.rotation.x + (double)pose.rotation.y * pose.rotation.y +
    (double)pose.rotation.z * pose.rotation.z + (double)pose.rotation.w * pose.rotation.w);
  data.lookQuaternion[0] = (float)(pose.rotation.x / length);
  data.lookQuaternion[1] = (float)(pose.rotation.y / length);
  data.lookQuaternion[2] = (float)(pose.rotation.z / length);
  data.lookQuaternion[3] = (float)(pose.rotation.w / length);
  data.rotationMatrixRightVector[0] = basis.right.x;
  data.rotationMatrixRightVector[1] = basis.right.y;
  data.rotationMatrixRightVector[2] = basis.right.z;
  data.rotationMatrixUpVector[0] = basis.up.x;
  data.rotationMatrixUpVector[1] = basis.up.y;
  data.rotationMatrixUpVector[2] = basis.up.z;
  data.rotationMatrixForwardVector[0] = basis.forward.x;
  data.rotationMatrixForwardVector[1] = basis.forward.y;
  data.rotationMatrixForwardVector[2] = basis.forward.z;

  float sinPitch = -basis.forward.y;
  if (sinPitch < -1.f) sinPitch = -1.f;
  if (sinPitch > 1.f) sinPitch = 1.f;
  data.pitch = asinf(sinPitch);
  if (cosf(data.pitch) > 0.0001f)
  {
    data.yaw = atan2f(basis.forward.x, basis.forward.z);
    data.roll = atan2f(basis.right.y, basis.up.y);
  }
  else
  {
    data.yaw = atan2f(-basis.right.z, basis.right.x);
    data.roll = 0.f;
  }
  return data;
}

void CameraManager::HotkeyUpdate()
{
  InputSystem* pInput = g_mainHandle->GetInputSystem();
  std::lock_guard<std::mutex> lock(m_Mutex);

  if (m_IgcsSession.IsActive()) return;

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

  // The undo key is Backspace alone, so it goes down with Shift and
  // Backspace (redo) too, and redoing wins then
  if (pInput->WentDown(Action::Track_Redo))
    m_TrackPlayer.Redo();
  else if (pInput->WentDown(Action::Track_Undo))
    m_TrackPlayer.Undo();

  // Bookmarks: the go key is the digit alone, so it goes down with Shift and
  // the digit too, and saving wins then
  for (int i = 0; i < BookmarkCount; ++i)
  {
    if (pInput->WentDown((Action)(Action::Camera_SaveBookmark1 + i)))
      UseBookmark(i, true);
    else if (pInput->WentDown((Action)(Action::Camera_GoToBookmark1 + i)))
      UseBookmark(i, false);
  }
}

// The camera hook's parts in its performance line (perf::LapStart, Lap; a
// part by its name's address)
namespace
{
  const char* const LapVistas = "the far-away buildings";
  const char* const LapShard = "the Shard (its far-away model, fans, smoke, triggers)";
  const char* const LapLair = "the lair's blinds and windows";
  const char* const LapDogen = "Dogen's door";
  const char* const LapStreet = "street level";
  const char* const LapInput = "the mouse";
  const char* const LapLock = "waiting for the camera's lock";
  const char* const LapSettings = "the game's settings applied (LOD, culling, shadows, hair, animation, billboards, reflections, speed)";
  const char* const LapInstances = "the instance lists watched";
  const char* const LapClock = "the clock and the track's seek";
  const char* const LapLoad = "the loading screen's watch, her soldier";
  const char* const LapReplay = "the replay (recording, playing)";
  const char* const LapFeet = "her steps in the puddles";
  const char* const LapCamera = "the free camera (following, input, moving, written)";
  const char* const LapGreen = "the green screen's view";
  const char* const LapWeather = "the weather";
  const char* const LapDry = "the kept-dry boxes (Faith, people, traffic, vehicles)";
  const char* const LapSounds = "the listener, the sounds and the markers";
  const char* const LapLogs = "the sound and world logs";
  const char* const LapStreaming = "the streaming foci";
  const char* const LapNearby = "the nearby list";
  const char* const LapBlur = "the motion blur and the field of view";
  const char* const LapTimelapse = "the time-lapse";
}

// CAMERA > Time-lapse's link to the rest of the tools (see timelapse::Host), made where it's used (the camera hook,
// the menu) under the camera manager's lock, from what the manager has then
namespace
{
  // Still: where the camera is held; the camera hook puts it back there every frame (OnCameraUpdate)
  struct LapseHold
  {
    bool On;
    XMFLOAT3 Position;
    XMFLOAT4 Rotation;
    float FieldOfView;
  };
  LapseHold g_LapseHold{ false, { 0.f, 0.f, 0.f }, { 0.f, 0.f, 0.f, 1.f }, 60.f };

  class TimelapseLink : public timelapse::Host
  {
  public:
    TimelapseLink(TrackPlayer& track, Camera const& camera, bool cameraOn, bool frozen, bool shakes, float worldScale, bool hud)
      : m_Track(track), m_Camera(camera), m_CameraOn(cameraOn), m_Frozen(frozen), m_Shakes(shakes), m_WorldScale(worldScale),
        m_Hud(hud) {}
    double Now() override
    {
      static LARGE_INTEGER frequency = {};
      if (!frequency.QuadPart)
        QueryPerformanceFrequency(&frequency);
      LARGE_INTEGER now;
      QueryPerformanceCounter(&now);
      return (double)now.QuadPart / (double)frequency.QuadPart;
    }
    bool CameraOn() override { return m_CameraOn; }
    bool CameraShakes() override { return m_Shakes; }
    bool TimeFrozen() override { return m_Frozen; }
    bool GamePaused() override { return gamestate::Paused() || gamestate::Loading(); }
    float WorldScale() override { return m_WorldScale; }
    bool WindowHidden() override { return g_gameHwnd && (IsIconic(g_gameHwnd) || !IsWindowVisible(g_gameHwnd)); }
    bool HudShown() override { return m_Hud; }
    long long LevelKey() override { return g_mainHandle->GetVisualsManager()->ClockKey(); }
    float Clock() override
    {
      return util::IsMecmpLoaded() ? -1.f : g_mainHandle->GetVisualsManager()->GetCurrentTimeOfDay();
    }
    bool LightUsable(char* why, size_t size) override { return g_mainHandle->GetVisualsManager()->LapseAvailable(why, size); }
    bool StepsHoldable() override { return util::hooks::TodHoldAvailable(); }
    long long LightSinceUpdateMs() override { return util::hooks::TodSinceUpdateMs(); }
    void CameraPose(float pose[8]) override
    {
      pose[0] = m_Camera.Position.x;
      pose[1] = m_Camera.Position.y;
      pose[2] = m_Camera.Position.z;
      pose[3] = m_Camera.Rotation.x;
      pose[4] = m_Camera.Rotation.y;
      pose[5] = m_Camera.Rotation.z;
      pose[6] = m_Camera.Rotation.w;
      pose[7] = m_Camera.FieldOfView;
    }
    void HoldCamera(bool hold, float const* pose) override
    {
      g_LapseHold.On = hold && pose;
      if (g_LapseHold.On)
      {
        g_LapseHold.Position = XMFLOAT3(pose[0], pose[1], pose[2]);
        g_LapseHold.Rotation = XMFLOAT4(pose[3], pose[4], pose[5], pose[6]);
        g_LapseHold.FieldOfView = pose[7];
      }
    }
    int TrackKeyframes() override { return m_Track.GetNodeCount(); }
    double TrackLength() override { return m_Track.GetTrackLength(); }
    std::string TrackName() override { return m_Track.GetTrackName(); }
    bool TrackTimeOfDay(double time, float& clock) override { return m_Track.TimeOfDayAt(time, clock); }
    void TrackHold(double time) override
    {
      if (m_Track.IsPlaying() && !m_Track.IsPaused())
        m_Track.TogglePause();
      m_Track.Seek(time);
    }
    bool TrackHeld(double time) override { return m_Track.IsPaused() && fabs(m_Track.GetPlaybackTime() - time) < 1e-6; }
    bool TrackPlaying() override { return m_Track.IsPlaying(); }
    bool TrackSetsTime() override { return m_Track.IsTimeOfDayLocked(); }
    void TrackRelease() override { m_Track.Stop(); }
    long LightAt(float clock, bool quiet) override { return g_mainHandle->GetVisualsManager()->LapseLightAt(clock, quiet); }
    int LightState(long id, float& litAt, float& clockAt, double& seconds) override
    {
      return g_mainHandle->GetVisualsManager()->LapseLightState(id, litAt, clockAt, seconds);
    }
    int ReflectionsState(long id, ReflectionInfo& info) override
    {
      VisualsManager::LapseReflInfo got{ false, 0, 0, 3 };
      int state = g_mainHandle->GetVisualsManager()->LapseReflectionsState(id, &got);
      info = { got.Marked, got.Waiting, got.Count, got.Ended };
      return state;
    }
    void LightEnd() override { g_mainHandle->GetVisualsManager()->EndLapse(); }
    void Shot(std::wstring const& path) override { frameshot::Request(path); }
    bool ShotBusy() override { return frameshot::Busy(); }
    void ShotCancel() override { frameshot::Cancel(); }

  private:
    TrackPlayer& m_Track;
    Camera const& m_Camera;
    bool m_CameraOn, m_Frozen, m_Shakes;
    float m_WorldScale;
    bool m_Hud;
  };
}

void CameraManager::OnCameraUpdate(__int64 pRenderView)
{
  // The hook runs for every render view, only the main one is overridden
  __int64 pGameRenderer = ReadPointer(m_ppGameRenderer);
  if (!pGameRenderer || pRenderView != *(__int64*)(pGameRenderer + RenderViewOffset))
    return;
  perf::LapStart();

  // The far-away buildings, as the menu says (on the client's thread, where
  // the game shows and hides them)
  UpdateVistas(m_VistasShown, m_VistaHideLoaded);
  perf::Lap(LapVistas);
  // The Shard's far-away model, hidden while its real tower is loaded; the
  // free camera for the game's triggers that stream the tower's top in (out
  // of the city) and the Bauble; and the two as before the story, if ticked
  int shardLoaded = UpdateShardLod();
  g_ShardTowerLoaded = shardLoaded;
  // Its tip, for the weather's lightning
  UpdateShardTip(shardLoaded);
  // GAME > Districts > the real Shard streamed from anywhere: its top's
  // trigger counts as entered too
  util::hooks::SetAreaTriggerCamera(m_CameraEnabled, g_ShardViewerInCity == 0, m_Camera.Transform.m[3][0],
    m_Camera.Transform.m[3][1], m_Camera.Transform.m[3][2], m_StreamRealShard);
  // With the real tower loaded (out of the city, or from anywhere with the
  // real Shard streamed), the Shard's far-away fans go onto the mission's
  // fans' places, and the ruined far-away model's smoke out of sight. Only
  // if the tools can't move the fans (their code isn't what the tools know,
  // or they aren't found within ShardFanWaitMs while the far-away sub-level
  // is loaded) does the whole far-away sub-level go (with its fans and
  // smoke), as when Faith leaves the city, until the real tower isn't shown
  // in its place any more.
  static bool s_fanCodeKnown = ShardFanCodeKnown();
  static bool s_fanCodeLogged = false;
  if (!s_fanCodeKnown && !s_fanCodeLogged)
  {
    s_fanCodeLogged = true;
    util::log::Warning("The Shard: the rotating fans' code isn't what the tools know: up close its far-away sub-level "
      "streams out instead (no fans there)");
  }
  bool shardUpClose = (g_ShardViewerInCity == 0 || m_StreamRealShard) && shardLoaded == 1;
  static ULONGLONG s_fansMissingSince = 0;
  static bool s_fansNotFound = false;
  bool fansMoved = UpdateShardFans(shardUpClose && !s_fansNotFound, s_fanCodeKnown);
  if (!shardUpClose || !s_fanCodeKnown)
  {
    s_fansMissingSince = 0;
    s_fansNotFound = false;
  }
  else if (!s_fansNotFound)
  {
    // Not counted while the game is paused or loading (its updates, the
    // fans' clocks with them, may be held then)
    ULONGLONG now = GetTickCount64();
    if (fansMoved || !g_ShardLodPresent || gamestate::Paused() || gamestate::Loading())
      s_fansMissingSince = 0;
    else if (!s_fansMissingSince)
      s_fansMissingSince = now;
    else if (now - s_fansMissingSince >= ShardFanWaitMs)
    {
      s_fansNotFound = true;
      util::log::Write("The Shard: its far-away fans not found to move, or not turning where the tools put them, in "
        "%.0f s (%d moved): its far-away sub-level streams out instead", ShardFanWaitMs / 1000.0,
        (int)g_MovedFans.size());
    }
  }
  UpdateShardSmoke(shardLoaded == 1);
  util::hooks::UpdateLandmarkTriggers(m_UnbrokenLandmarks, shardUpClose && (!s_fanCodeKnown || s_fansNotFound),
    m_KrugerBlinds);
  perf::Lap(LapShard);
  // The lair's blinds open if ticked. The Shard station's gates and windows
  // stay as the game has them: their tick is gone (2026-10-27), so this only
  // returns.
  UpdateModelHide(g_LairBlindsHide, m_OpenLairBlinds);
  UpdateModelHide(g_ShardStationHide, m_OpenShardStation);
  // The lair's two windows that open, shut and solid if ticked (looked for
  // with the free camera or Faith near them)
  bool closeLairWindows = m_CloseLairWindows;
  if (closeLairWindows || g_LairWindowsOn)
  {
    XMFLOAT4X4A player;
    bool hasFaith = GetPlayerTransform(player);
    float faith[3] = { hasFaith ? player._41 : 0.f, hasFaith ? player._42 : 0.f, hasFaith ? player._43 : 0.f };
    bool nearLair = (m_CameraEnabled && NearLairWindows(m_Camera.Transform.m[3][0], m_Camera.Transform.m[3][1],
      m_Camera.Transform.m[3][2])) || (hasFaith && NearLairWindows(faith[0], faith[1], faith[2]));
    UpdateLairWindows(closeLairWindows, nearLair, GetPlayerSoldier(), hasFaith, faith);
  }
  perf::Lap(LapLair);
  // Dogen's door (Anchor) open while Faith is at it; no menu entry (DogenDoor.cpp)
  {
    XMFLOAT4X4A player;
    bool hasFaith = GetPlayerTransform(player);
    dogen::Update(hasFaith, hasFaith ? &player._41 : nullptr);
  }
  perf::Lap(LapDogen);
  // GAME > Street level: the ground tiles and props round Faith (the physics world's flush makes and
  // destroys them)
  {
    XMFLOAT4X4A player;
    bool readable = GetPlayerTransform(player);
    // The Sky City station's collision reshaped while Solid street is on, back once it's off and she's out of what
    // it opened (till then Street level stays on: SkyCityHolds)
    ground::UpdateSkyCity(readable, readable ? &player._41 : nullptr);
    bool hasFaith = (m_SolidStreet || m_NoFallDeath || ground::SkyCityHolds()) && readable;
    ground::Update(hasFaith, hasFaith ? &player._41 : nullptr);
    // The gates Solid street takes away: where Faith is, on or off (one comes back only once she's clear of it)
    ground::UpdateGates(readable, readable ? &player._41 : nullptr);
  }
  perf::Lap(LapStreet);

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
  g_mainHandle->GetUI()->AddWheelFromRawInput(mouseState.z / WHEEL_DELTA);
  perf::Lap(LapInput);

  std::lock_guard<std::mutex> lock(m_Mutex);
  perf::Lap(LapLock);

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
  // GAME > Level streaming > No culling (CullingOff.cpp): its CT.log lines, with the render view's position (its
  // LinearTransform's translation) for the 10-s line's check that the main view is the camera
  {
    XMFLOAT4X4 const* pViewTransform = (XMFLOAT4X4 const*)(pRenderView + RenderViewTransform1);
    culling_off::FrameUpdate(pViewTransform->_41, pViewTransform->_42, pViewTransform->_43);
  }
  ApplyShadowDistance();
  perf::Lap(LapSettings);
  WatchInstanceLists();
  perf::Lap(LapInstances);
  ApplyHairFix();
  ApplyAnimationSmoothing();
  ApplyBillboardDetail();
  ApplyNearReflections();
  ApplyGameSpeed();
  perf::Lap(LapSettings);

  // CAMERA > Time-lapse: a frame at a time, camera on or off (Timelapse.cpp); before the clock is kept below, and
  // before the track plays (it holds the track at its frame's place)
  {
    TimelapseLink link(m_TrackPlayer, m_Camera, m_CameraEnabled, IsTimeFrozen(), m_ShakeStrength > 0.f ||
      m_TrackPlayer.IsShakeLocked(), IsTimeFrozen() ? 0.f : GameSpeedNow() * m_GameTimeScale, IsHudVisible());
    timelapse::Update(link);
  }
  perf::Lap(LapTimelapse);

  // A track stopped setting the time of day: the clock carries on from there
  if (m_TrackTimeOfDayApplied && !(m_CameraEnabled && m_TrackPlayer.IsPlaying() && m_TrackPlayer.IsTimeOfDayLocked()))
  {
    g_mainHandle->GetVisualsManager()->StopTrackTimeOfDay();
    m_TrackTimeOfDayApplied = false;
  }
  // A stopped clock is held every frame, not only at the game's visual updates
  g_mainHandle->GetVisualsManager()->KeepClock();

  // The camera track sent to another moment (its slider): a replay playing
  // along with it goes to the same moment (both move on by the same step,
  // the track's times its playback speed). At the game speed, the replay is
  // where the game time the track makes up to there takes it.
  double sought = 0, soughtGame = 0;
  if (m_TrackPlayer.TakeSeek(sought, soughtGame, m_GameSpeed) && m_TrackPlayer.GetPlaybackSpeed() > 0.f)
    m_Replay.SeekAlongWithTrack((m_ReplayAtGameSpeed ? soughtGame : sought) / m_TrackPlayer.GetPlaybackSpeed());
  perf::Lap(LapClock);

  // A loading screen up a while: what the game's level streaming waits on, in the log
  util::hooks::WatchLoadScreen();
  // Faith gone or new: the replay's recorded sounds are checked again
  util::hooks::NoteFaithSoldier(GetPlayerSoldier());
  perf::Lap(LapLoad);

  // Recording Faith, or playing a recording on her body (camera on or off)
  {
    XMFLOAT4X4A player;
    bool hasPlayer = GetPlayerTransform(player) && !(player._41 == 0 && player._42 == 0 && player._43 == 0);
    m_Replay.Update(GetPlayerSoldier(), m_CameraEnabled && m_TrackPlayer.IsPlaying(), m_TrackPlayer.IsPaused(),
      m_PlaybackFps > 0 ? 1.0 / m_PlaybackFps : 0.0, m_CameraEnabled && m_ThirdPersonApplied, hasPlayer ? &player._11 : nullptr);
    perf::Lap(LapReplay);
    // Her steps in the wet surfaces' puddles
    util::hooks::SetFaithFeet(GetPlayerSoldier(), hasPlayer ? &player._41 : nullptr);
    perf::Lap(LapFeet);
  }
  // GAME > Faith > Size: bone 1's scale through the game's pose override, after the replay's update (it steps
  // aside while a replay records or plays: FaithSize.cpp). Switched off with its menu (the GAME tab).
  // faith_size::Update(GetPlayerSoldier(), m_Replay.IsBusy(), m_CameraEnabled ? 0 : pRenderView);
  // GAME > Pose: the held poses handed to the game's pose override, after the replay's update (they step aside
  // while a replay records or plays: PoseEditor.cpp)
  pose_editor::Update(GetPlayerSoldier(), m_Replay.IsBusy(), IsTimeFrozen(), IsTimeFrozen() ? 0.f : GameSpeedNow() *
    m_GameTimeScale, m_CameraEnabled && m_TrackPlayer.IsPlaying() ? m_TrackPlayer.GetSequenceTime() : -1.0);
  // GAME > First person > First camera view > Bodycam: the tools' own bodycam on Faith's body (after the pose
  // override, so it sees a posed body); in her first-person view with the free camera off, the main view and field
  // of view are its (Bodycam.cpp). Before the green screen, weather and wet surfaces take the view.
  bodycam::Update(pRenderView, ReadPointer(m_ppGameRenderSettings), GetPlayerSoldier(), m_CameraEnabled,
    IsTimeFrozen() ? 0.f : GameSpeedNow() * m_GameTimeScale);

  // Motion blur too. With the camera on it's applied after the track has
  // updated the strength below.
  if (!m_CameraEnabled)
  {
    m_IgcsViewReady = false;
    PublishListener(false, 0.0);
    panini::Set(false, 0.f, 0.f, 0.f, 0.f);
    g_mainHandle->GetVisualsManager()->SetAutoFocus(false, 0.f);
    util::hooks::SetBillboardCamera(false, nullptr, 0.f);
    util::hooks::UpdateSpotSounds(false, nullptr);
    util::hooks::UpdateCrowdTrackers(false, nullptr, nullptr);
    perf::Lap(LapSounds);
    UpdateFaithSoundLog();
    UpdateWorldLog();
    perf::Lap(LapLogs);
    // The Always districts' foci (only theirs are wanted with the camera off)
    PublishStreamingFocus(0.0);
    perf::Lap(LapStreaming);
    ApplyMotionBlur();
    // The game's own camera, final for the frame, for the green screen
    greenplane::SetView(pRenderView);
    perf::Lap(LapBlur);
    // and for the weather (the rain kept at it)
    weather::Update(pRenderView);
    perf::Lap(LapWeather);
    // and for the wet surfaces: what they leave alone (Faith, people,
    // vehicles, trains; Faith only looked up while they're on)
    {
      XMFLOAT4X4A player;
      // (and while it rains: the rain is kept out of people and drones,
      // see dry::GetBodies)
      bool wetOn = wetlook::Reach() > 0.f || weather::IsRaining(nullptr);
      bool hasFaith = wetOn && GetPlayerTransform(player);
      dry::Update(wetOn ? pRenderView : 0, wetOn ? GetPlayerSoldier() : 0, hasFaith ? &player._11 : nullptr);
    }
    perf::Lap(LapDry);
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
  // Her body the game's way on a new Faith too (see KeepViewModeBody)
  if (m_ThirdPersonApplied)
    KeepViewModeBody();

  if (m_IgcsSession.IsActive())
  {
    ApplyIgcsPose(m_IgcsSession.GetCurrent());
  }
  else if (m_IgcsRestoreFrame)
  {
    ApplyIgcsPose(m_IgcsSession.GetOrigin());
    m_IgcsRestoreFrame = false;
  }
  else
  {
    FollowAttachTarget(dt, trackDt);
    UpdateInput(dt, mouseState);
    // CAMERA > Time-lapse, still: the camera held where it was when it started (no input, following, orbit or drift),
    // put back before the move and again after it if anything moved it (TimelapseLink::HoldCamera)
    if (g_LapseHold.On)
    {
      m_Camera.dX = m_Camera.dY = m_Camera.dZ = 0.f;
      m_Camera.dPitch = m_Camera.dYaw = m_Camera.dRoll = m_Camera.dFov = 0.f;
      m_Camera.dMousePitch = m_Camera.dMouseYaw = 0.f;
      m_Velocity = XMFLOAT3(0.f, 0.f, 0.f);
      m_TurnRates[0] = m_TurnRates[1] = m_TurnRates[2] = 0.f;
      m_Camera.Position = g_LapseHold.Position;
      m_Camera.Rotation = g_LapseHold.Rotation;
      m_Camera.FieldOfView = g_LapseHold.FieldOfView;
    }
    UpdateCamera(dt, trackDt);
    // (and the view built from it again, every frame: a held camera has no shake, so every frame is the same view)
    if (g_LapseHold.On)
    {
      m_Camera.Position = g_LapseHold.Position;
      m_Camera.Rotation = g_LapseHold.Rotation;
      m_Camera.FieldOfView = g_LapseHold.FieldOfView;
      XMMATRIX held = XMMatrixRotationQuaternion(XMLoadFloat4(&m_Camera.Rotation));
      held.r[3] = XMVectorSetW(XMLoadFloat3(&m_Camera.Position), 1.f);
      XMStoreFloat4x4(&m_Camera.Transform, held);
    }
    StepTrackWallCheck();
    ProcessOrbitRequests();
    // A playing track's game speed, from this frame's place on it
    ApplyGameSpeed();
  }
  WriteToGameCamera(pRenderView);
  m_IgcsViewReady = GetPlayerSoldier() != 0;
  // The free camera, written for the frame, for the green screen
  greenplane::SetView(pRenderView);
  perf::Lap(LapCamera);
  // and for the weather (the rain kept at it)
  weather::Update(pRenderView);
  perf::Lap(LapWeather);
  // and for the wet surfaces: what they leave alone (as above)
  {
    XMFLOAT4X4A player;
    // (and while it rains: the rain is kept out of people and drones, see
    // dry::GetBodies)
    bool wetOn = wetlook::Reach() > 0.f || weather::IsRaining(nullptr);
    bool hasFaith = wetOn && GetPlayerTransform(player);
    dry::Update(wetOn ? pRenderView : 0, wetOn ? GetPlayerSoldier() : 0, hasFaith ? &player._11 : nullptr);
  }
  perf::Lap(LapDry);
  // The Panini projection, drawn at Present with this view's field of view
  panini::Set(m_Panini, m_PaniniDistance, m_PaniniCrop, m_ViewFovY, m_ViewAspect);
  // Auto-focus: the depth of field's set-up moved to the focus' distance
  g_mainHandle->GetVisualsManager()->SetAutoFocus(m_AutoFocus && m_FocusOk, m_FocusDistance);
  PublishListener(m_SoundAtCamera, dt);
  // The video billboards sized from this camera (its field of view as the
  // render view had it last frame, in degrees like the game's camera entry)
  util::hooks::SetBillboardCamera(true, &m_Camera.Transform.m[0][0], m_ViewFovY * 57.2957795f);
  float spotCamera[3] = { m_Camera.Transform._41, m_Camera.Transform._42, m_Camera.Transform._43 };
  util::hooks::UpdateSpotSounds(m_SpotSounds && m_SoundAtCamera, spotCamera);
  {
    // The camera's own transform (a LinearTransform like the level's; the
    // trackers go by its position)
    alignas(16) float tracked[16];
    memcpy(tracked, &m_Camera.Transform, sizeof(tracked));
    XMFLOAT4X4A player;
    bool hasFaith = GetPlayerTransform(player);
    float faith[3] = { player._41, player._42, player._43 };
    util::hooks::UpdateCrowdTrackers(m_SpotSounds && m_SoundAtCamera, tracked, hasFaith ? faith : nullptr);
  }
  perf::Lap(LapSounds);
  UpdateFaithSoundLog();
  UpdateWorldLog();
  perf::Lap(LapLogs);
  PublishStreamingFocus(dt);
  perf::Lap(LapStreaming);
  UpdateNearby();
  perf::Lap(LapNearby);
  ApplyFieldOfView(m_Camera.FieldOfView);
  ApplyMotionBlur();
  perf::Lap(LapBlur);
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
  float streamingFovY = m_ViewFovY;
  if (m_IgcsSession.IsActive())
  {
    // Multishot aperture samples are not camera travel. Keep streaming centred
    // on the capture origin for the duration of the render.
    auto const& origin = m_IgcsSession.GetOrigin();
    auto const& basis = m_IgcsSession.GetBasis();
    position = XMVectorSet(origin.position.x, origin.position.y, origin.position.z, 1.f);
    forward = XMVectorSet(basis.forward.x, basis.forward.y, basis.forward.z, 0.f);
    streamingFovY = XMConvertToRadians(origin.fovDegrees);
  }
  for (int i = 0; i < MaxStreamingFoci; ++i)
    XMStoreFloat3((XMFLOAT3*)results[i].Direction, forward);

  XMStoreFloat3((XMFLOAT3*)results[FocusCamera].Position, position);
  wanted[FocusCamera] = on;

  // Ahead: where the camera will be in a couple of seconds at its current
  // speed, so the zones are loading before it gets there. Jumps (Go to,
  // playback starting) reset the speed instead of throwing it far away.
  XMVECTOR last = XMLoadFloat3(&m_StreamLastPosition);
  float moved = XMVectorGetX(XMVector3Length(position - last));
  if (m_IgcsSession.IsActive() || !m_StreamHasLast || dt <= 0 || moved > 50.f)
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

  // Where the camera looks, flat: the view's width and a margin either side of
  // the way it faces (it looks along -row 2). Used for Only where the camera
  // looks, and for what's in view going first. Straight up or down,
  // everything around counts as in view.
  XMFLOAT3 cameraPosition;
  XMStoreFloat3(&cameraPosition, position);
  float lookX = -XMVectorGetX(forward), lookZ = -XMVectorGetZ(forward);
  float lookLength = sqrtf(lookX * lookX + lookZ * lookZ);
  bool hasLook = lookLength > 0.1f;
  float minDot = -2.f;
  float keepMinDot = -2.f; // a corner of the grid stays this far round
  if (hasLook)
  {
    lookX /= lookLength;
    lookZ /= lookLength;
    float halfWidth = atanf(tanf(streamingFovY * 0.5f) * m_ViewAspect) + ViewConeMargin;
    if (halfWidth > 1.5707963f) halfWidth = 1.5707963f;
    minDot = cosf(halfWidth);
    keepMinDot = cosf(halfWidth + GridKeepAngle);
  }
  auto inCone = [&](float x, float z) {
    float dx = x - cameraPosition.x, dz = z - cameraPosition.z;
    float length = sqrtf(dx * dx + dz * dz);
    return !hasLook || length <= 1.f || (dx * lookX + dz * lookZ) / length >= minDot;
  };
  auto flatDistance = [&](float x, float z) {
    float dx = x - cameraPosition.x, dz = z - cameraPosition.z;
    return sqrtf(dx * dx + dz * dz);
  };

  // Around the camera, out to the stream distance: a focus on each corner of
  // a fixed 200 m grid that's that close. A focus loads its own zone and the
  // zones next to it, so that loads everything around the camera that far
  // out. The grid stays where it is when the camera moves: moving on brings
  // in the corners ahead and lets go of the ones behind, and the rest keep
  // their foci and their zones (rings around the camera moved every point
  // each time it had gone 100 m, which changed the zones of many of them). A
  // corner comes in within the stream distance and goes once it's 100 m
  // further out, or (only where the camera looks) 15 degrees outside the view,
  // so it doesn't come and go at the edge.
  static_assert(MaxRingFoci == MaxGridPoints, "one grid corner per ring focus");
  if (on && m_StreamDistance > 1.f)
  {
    float maxDistance = m_StreamInView ? MaxStreamDistanceInView : MaxStreamDistance;
    float distance = m_StreamDistance > maxDistance ? maxDistance : m_StreamDistance;
    bool inViewOnly = m_StreamInView && hasLook;
    auto staysInView = [&](float x, float z) {
      float dx = x - cameraPosition.x, dz = z - cameraPosition.z;
      float length = sqrtf(dx * dx + dz * dz);
      return length <= InViewNear || (dx * lookX + dz * lookZ) / length >= keepMinDot;
    };
    for (GridPoint& point : m_GridPoints)
    {
      if (!point.Used)
        continue;
      float x = point.X * GridSpacing, z = point.Z * GridSpacing;
      if (flatDistance(x, z) > distance + GridKeepMargin || (inViewOnly && !staysInView(x, z)))
        point.Used = false;
    }

    // The corners that come in, nearest first, into the free foci
    struct Corner
    {
      int X, Z;
      float Distance;
    };
    const int MaxCorners = 256; // 1000 m: 13 x 13 corners in the square around it
    Corner corners[MaxCorners];
    int cornerCount = 0;
    int minX = (int)floorf((cameraPosition.x - distance) / GridSpacing);
    int maxX = (int)ceilf((cameraPosition.x + distance) / GridSpacing);
    int minZ = (int)floorf((cameraPosition.z - distance) / GridSpacing);
    int maxZ = (int)ceilf((cameraPosition.z + distance) / GridSpacing);
    for (int gx = minX; gx <= maxX; ++gx)
    {
      for (int gz = minZ; gz <= maxZ && cornerCount < MaxCorners; ++gz)
      {
        float x = gx * GridSpacing, z = gz * GridSpacing;
        float d = flatDistance(x, z);
        if (d > distance || (inViewOnly && !inCone(x, z)))
          continue;
        bool used = false;
        for (GridPoint const& point : m_GridPoints)
          used |= point.Used && point.X == gx && point.Z == gz;
        if (!used)
          corners[cornerCount++] = { gx, gz, d };
      }
    }
    std::sort(corners, corners + cornerCount, [](Corner const& a, Corner const& b) { return a.Distance < b.Distance; });
    int next = 0;
    for (int c = 0; c < cornerCount; ++c)
    {
      while (next < MaxGridPoints && m_GridPoints[next].Used)
        ++next;
      if (next == MaxGridPoints)
        break; // all taken: the furthest wait until nearer ones go
      m_GridPoints[next] = { true, corners[c].X, corners[c].Z };
    }

    for (int i = 0; i < MaxGridPoints; ++i)
    {
      if (!m_GridPoints[i].Used)
        continue;
      XMStoreFloat3((XMFLOAT3*)results[FocusRingFirst + i].Position,
        XMVectorSet(m_GridPoints[i].X * GridSpacing, cameraPosition.y, m_GridPoints[i].Z * GridSpacing, 0));
      wanted[FocusRingFirst + i] = true;
    }
  }
  else
  {
    for (GridPoint& point : m_GridPoints)
      point.Used = false;
  }

  // Along the selected track, so playing it doesn't load anything new
  if (on && m_StreamTrack)
  {
    XMFLOAT3 points[MaxTrackFoci];
    int count = m_TrackPlayer.GetStreamingPoints(points, MaxTrackFoci, 150.f);
    // A track playing while the camera follows Faith goes where she took it
    // (see UpdateCamera): its points go with it
    bool moved = m_TrackPlayer.IsPlaying() && m_AttachTrackActive && m_AttachValid;
    XMMATRIX move = moved ? XMMatrixInverse(nullptr, XMLoadFloat4x4(&m_AttachTrackStart)) * XMLoadFloat4x4(&m_AttachNow) :
      XMMatrixIdentity();
    for (int i = 0; i < count; ++i)
    {
      if (moved)
        XMStoreFloat3(&points[i], XMVector3TransformCoord(XMLoadFloat3(&points[i]), move));
      memcpy(results[FocusTrackFirst + i].Position, &points[i], sizeof(float) * 3);
      wanted[FocusTrackFirst + i] = true;
    }
  }

  // The districts set to Always in the menu, each focus for its own streamers
  // (worked out again only when the districts or their streamers change).
  // With the camera on or off, and whether the world around the camera is
  // loaded or not.
  __int64 focusStreamers[MaxStreamingFoci][MaxFocusStreamers]{};
  int focusDistrict[MaxStreamingFoci];
  for (int i = 0; i < MaxStreamingFoci; ++i)
    focusDistrict[i] = -1;
  {
    unsigned int generation = g_DistrictGeneration;
    if (generation != g_DistrictFociGeneration)
    {
      BuildDistrictFoci(g_DistrictFoci, MaxDistrictFoci);
      g_DistrictFociGeneration = generation;
    }
    for (size_t k = 0; k < g_DistrictFoci.size() && k < (size_t)MaxDistrictFoci; ++k)
    {
      DistrictFocus const& focus = g_DistrictFoci[k];
      int i = FocusDistrictFirst + (int)k;
      results[i].Position[0] = focus.X;
      results[i].Position[1] = focus.Y;
      results[i].Position[2] = focus.Z;
      memcpy(focusStreamers[i], focus.Streamers, sizeof(focus.Streamers));
      focusDistrict[i] = focus.District;
      wanted[i] = true;
    }
  }

  // VISUALS > Weather: the zones the game's rain is placed in, kept loaded
  // while it rains (camera on or off), like the Always districts
  {
    float points[MaxWeatherFoci][3];
    int count = weather::ZonePoints(points, MaxWeatherFoci);
    for (int k = 0; k < count && k < MaxWeatherFoci; ++k)
    {
      memcpy(results[FocusWeatherFirst + k].Position, points[k], sizeof(points[k]));
      wanted[FocusWeatherFirst + k] = true;
    }
  }

  // GAME > Districts > the real Shard: its tower's zone (and the zones next
  // to it) kept loaded in the buildings' streamer, camera on or off, like an
  // Always district (see EnforceNeverDistricts for the rest of it)
  if (m_StreamRealShard)
  {
    float point[3];
    __int64 streamer = 0;
    if (ShardZonePoint(point, streamer))
    {
      memcpy(results[FocusShard].Position, point, sizeof(point));
      focusStreamers[FocusShard][0] = streamer;
      wanted[FocusShard] = true;
    }
  }

  // The order foci go in (and, backwards, go out when there's no room): the
  // camera, ahead of it, the Always districts, what's in view (or within
  // InViewNear) nearest first, then the track's points and what's behind the
  // camera, nearest first. Foci that aren't wanted go last.
  int order[MaxStreamingFoci];
  int tier[MaxStreamingFoci];
  float rankDistance[MaxStreamingFoci];
  for (int i = 0; i < MaxStreamingFoci; ++i)
  {
    order[i] = i;
    float x = results[i].Position[0], z = results[i].Position[2];
    rankDistance[i] = flatDistance(x, z);
    if (i == FocusCamera || i == FocusAhead)
    {
      tier[i] = TierCamera;
      rankDistance[i] = (float)i;
    }
    else if (i >= FocusDistrictFirst)
      tier[i] = TierDistrict;
    else if (inCone(x, z) || rankDistance[i] < InViewNear)
      tier[i] = TierInView;
    else
      tier[i] = i >= FocusTrackFirst ? TierTrack : TierBehind;
  }
  std::sort(order, order + MaxStreamingFoci, [&](int a, int b) {
    int tierA = wanted[a] ? tier[a] : TierBehind + 1, tierB = wanted[b] ? tier[b] : TierBehind + 1;
    if (tierA != tierB) return tierA < tierB;
    if (rankDistance[a] != rankDistance[b]) return rankDistance[a] < rankDistance[b];
    return a < b;
  });

  // Held out for room at another place: that hold is forgotten
  ForgetMovedHolds(results, wanted, tier);

  AcquireSRWLockExclusive(&g_FocusLock);
  memcpy(g_FocusResults, results, sizeof(results));
  memcpy(g_FocusWanted, wanted, sizeof(wanted));
  memcpy(g_FocusOrder, order, sizeof(order));
  memcpy(g_FocusTier, tier, sizeof(tier));
  memcpy(g_FocusStreamers, focusStreamers, sizeof(focusStreamers));
  memcpy(g_FocusDistrict, focusDistrict, sizeof(focusDistrict));
  g_SortView = { on, cameraPosition.x, cameraPosition.z, hasLook, lookX, lookZ, minDot };
  ReleaseSRWLockExclusive(&g_FocusLock);

  m_StreamingFocusReady = true;
}

bool CameraManager::IsVistaZoneHeldBack(__int64 pVista, unsigned int zone)
{
  static bool s_threadLogged = false;
  if (!s_threadLogged)
  {
    s_threadLogged = true;
    util::log::Write("The game shows vistas on thread %u", GetCurrentThreadId());
  }
  return VistaZoneOff(pVista, zone);
}

bool CameraManager::OnZoneLoadOrder(__int64 pSorter, float const* pCache, unsigned short* pZones, int count)
{
  // Only while the free camera has the world loaded around it
  if (!pCache || !m_CameraEnabled || !m_StreamAroundCamera || !m_StreamingFocusReady)
    return false;
  SortView view;
  AcquireSRWLockShared(&g_FocusLock);
  view = g_SortView;
  ReleaseSRWLockShared(&g_FocusLock);
  if (!view.Active)
    return false;
  bool sorted = SortZoneLoads(view, pSorter, pCache, pZones, count);
  static bool s_logged = false;
  if (sorted && count > 1 && !s_logged)
  {
    s_logged = true;
    util::log::Write("Zones now load in the free camera's view first (around Faith before that)");
  }
  return sorted;
}

void CameraManager::OnZoneStreamerUpdate(__int64 pZoneStreamer)
{
  // For the vistas: which streamers there are
  NoteServerStreamer(pZoneStreamer);
  static bool s_threadLogged = false;
  if (!s_threadLogged)
  {
    s_threadLogged = true;
    util::log::Write("Zone streamers update on thread %u", GetCurrentThreadId());
  }

  // Its zones' districts, for the menu and the districts' foci
  UpdateDistrictLayer(pZoneStreamer);
  // Faith dead or not spawned yet: nothing of the tools' own (see
  // StreamingPausedForFaith)
  bool paused = StreamingPausedForFaith();
  // The Shard's real tower kept out unless the view is out of the city's area
  // and in the area around the Shard (see ShardZoneName, ShardArea): the free
  // camera, or with it off the game's own focus (next to Faith). Not known
  // yet: as it was.
  bool shardOut = false;
  {
    float x = 0.f, z = 0.f;
    bool known = false;
    bool freeCamera = m_CameraEnabled;
    if (freeCamera)
    {
      AcquireSRWLockShared(&g_FocusLock);
      x = g_SortView.X;
      z = g_SortView.Z;
      ReleaseSRWLockShared(&g_FocusLock);
      known = m_StreamingFocusReady;
    }
    else
      known = ReadGameFocusPoint(pZoneStreamer, x, z);
    shardOut = known ? UpdateShardViewer(x, z, freeCamera) : g_ShardTowerAllowed == 0;
    // Back in the city: not before the far-away model is there again (or
    // after ShardBackdropWaitMs), so the Shard doesn't go missing meanwhile
    shardOut = shardOut && (g_ShardLodPresent || GetTickCount64() - g_ShardInCitySince >= ShardBackdropWaitMs);
    // GAME > Districts > the real Shard: never kept out
    shardOut = shardOut && !m_StreamRealShard;
  }
  // The districts set to Never unloaded and kept out (camera on or off)
  EnforceNeverDistricts(pZoneStreamer, m_pZoneStep, paused, shardOut);

  if (!m_pAddZoneFocus || !m_pRemoveZoneFocus)
    return;

  // No camera lock: this runs on the game's update. The wanted foci come
  // from the camera hook through the focus lock (published every frame: with
  // the camera off only the Always districts' are wanted).
  bool wanted[MaxStreamingFoci]{};
  int order[MaxStreamingFoci];
  int tier[MaxStreamingFoci];
  for (int i = 0; i < MaxStreamingFoci; ++i)
  {
    order[i] = i;
    tier[i] = i <= FocusAhead ? TierCamera : TierBehind;
  }
  // Not published since the camera was switched on or off: the foci stay as
  // they are for this update (taken out and back, an Always district's zones
  // would unload and load again)
  if (!m_StreamingFocusReady)
    return;
  static ZoneFocusResult s_results[MaxStreamingFoci]; // zone streamer hook only
  {
    static __int64 s_streamers[MaxStreamingFoci][MaxFocusStreamers];
    AcquireSRWLockShared(&g_FocusLock);
    memcpy(wanted, g_FocusWanted, sizeof(wanted));
    memcpy(order, g_FocusOrder, sizeof(order));
    memcpy(tier, g_FocusTier, sizeof(tier));
    memcpy(s_results, g_FocusResults, sizeof(s_results));
    memcpy(s_streamers, g_FocusStreamers, sizeof(s_streamers));
    ReleaseSRWLockShared(&g_FocusLock);
    // A district's focus only for its own streamers (the weather's are for all)
    for (int i = FocusDistrictFirst; i < FocusWeatherFirst; ++i)
    {
      bool mine = false;
      for (int s = 0; s < MaxFocusStreamers; ++s)
        mine |= s_streamers[i][s] == pZoneStreamer;
      wanted[i] &= mine;
    }
    // The real Shard's too (the buildings' streamer)
    {
      bool mine = false;
      for (int s = 0; s < MaxFocusStreamers; ++s)
        mine |= s_streamers[FocusShard][s] == pZoneStreamer;
      wanted[FocusShard] &= mine;
    }
    if (paused)
    {
      for (int i = 0; i < MaxStreamingFoci; ++i)
        wanted[i] = false;
    }
    // The camera's own foci leave the districts set to Never out (where the
    // game would put them in this streamer)
    if (g_AnyNeverDistrict)
    {
      for (int i = 0; i < FocusDistrictFirst; ++i)
        if (wanted[i] && InNeverDistrict(pZoneStreamer, s_results[i].Position[0], s_results[i].Position[1], s_results[i].Position[2]))
          wanted[i] = false;
      for (int i = FocusWeatherFirst; i < MaxStreamingFoci; ++i)
        if (wanted[i] && InNeverDistrict(pZoneStreamer, s_results[i].Position[0], s_results[i].Position[1], s_results[i].Position[2]))
          wanted[i] = false;
    }
  }

  int before = 0, after = 0, usedZones = 0;
  bool limited = false;
  bool present[MaxStreamingFoci];
  // Making room for what's in view only helps when unloading gives room back:
  // not for the renderer's instance memory (which doesn't empty again)
  bool swapUseful = m_StreamUnrestricted || m_ArenaPressure < 1;
  // Far from every limit: foci go in at a steady pace (FastFillPacingMs)
  float instanceFill = m_InstanceFill;
  long long arenaSize = m_ArenaSize;
  bool fastFill = m_InstancePressure == 0 && instanceFill >= 0.f && instanceFill < FastFillMostFill &&
    m_BatchFill < m_BatchLimit - FastFillBatchMargin &&
    (arenaSize <= 0 || (float)m_ArenaUsed / (float)arenaSize < ArenaStopAdding - FastFillBatchMargin);
  SyncStreamingFoci(pZoneStreamer, wanted, order, tier, s_results, m_pAddZoneFocus, m_pRemoveZoneFocus, m_InstancePressure,
    swapUseful, fastFill, before, after, usedZones, limited, present);
  if (before == 0 && after > 0)
    util::log::Write("Zone streamer 0x%I64X now also loads the city around the camera or the Always districts", pZoneStreamer);
  else if (before > 0 && after == 0)
    util::log::Write("Zone streamer 0x%I64X is back to loading around Faith only", pZoneStreamer);

  // For the menu: whether each district's focus is in (the list of the
  // first streamer it's for)
  AcquireSRWLockShared(&g_FocusLock);
  for (int k = 0; k < MaxDistrictFoci; ++k)
  {
    int i = FocusDistrictFirst + k;
    if (g_FocusStreamers[i][0] == pZoneStreamer)
      g_DistrictFocusPresent[k] = present[i];
  }
  ReleaseSRWLockShared(&g_FocusLock);

  // For the menu: the fullest streamer over the last second, and when foci were held back
  ULONGLONG now = GetTickCount64();
  static ULONGLONG s_peakTick = 0;
  if (now - s_peakTick > 1000)
  {
    s_peakTick = now;
    m_StreamSlotsPeak = usedZones;
  }
  else if (usedZones > m_StreamSlotsPeak)
    m_StreamSlotsPeak = usedZones;
  if (limited) m_StreamLimitedTick = now;
  static ULONGLONG s_lastLimitLog = 0;
  if (limited && GetTickCount64() - s_lastLimitLog > 30000)
  {
    s_lastLimitLog = GetTickCount64();
    if (usedZones >= ZoneSlotsAddBelow())
      util::log::Write("Zone streamer 0x%I64X has %d of its %d zone slots in use or asked for: the camera's extra zones are held back so it can't run out",
        pZoneStreamer, usedZones, ZoneSlotCount());
    else if (!m_StreamUnrestricted && m_BatchPressure >= 1 && m_BatchPressure >= m_MeshPressure && m_BatchPressure >= m_ListPressure)
    {
      if (m_BatchListFuller)
        util::log::Write("The camera's extra zones are held back: the renderer's instance list has %d of its %d (all its draw batches; zone streamer 0x%I64X, %d zone slots)",
          (int)m_RendererInstances, (int)m_RendererRoom, pZoneStreamer, usedZones);
      else
        util::log::Write("The camera's extra zones are held back: the renderer's fullest draw batch has %d of its %d instances (zone streamer 0x%I64X, %d zone slots)",
          (int)m_BatchInstances, (int)m_BatchRoom, pZoneStreamer, usedZones);
    }
    else if (!m_StreamUnrestricted && m_ArenaPressure >= 1 && m_ArenaPressure >= m_MeshPressure && m_ArenaPressure >= m_ListPressure)
      util::log::Write("The camera's extra zones are held back: the renderer's memory for mesh instances is %.1f of %d MB used (zone streamer 0x%I64X, %d zone slots)",
        m_ArenaUsed / 1048576.0, (int)(m_ArenaSize >> 20), pZoneStreamer, usedZones);
    else if (m_MeshPressure >= m_ListPressure && m_MeshPressure >= 1)
      util::log::Write("The camera's extra zones are held back: %d of the game's %d meshes are in use (zone streamer 0x%I64X, %d zone slots)",
        (int)m_MeshCount, MeshCapacity(), pZoneStreamer, usedZones);
    else if (m_ListPressure >= 1)
      util::log::Write("The camera's extra zones are held back: %d of the game's %d model instances are in use (zone streamer 0x%I64X, %d zone slots)",
        m_InstanceInUse[0], m_InstanceCapacity[0], pZoneStreamer, usedZones);
  }
}


namespace
{
  // The follow smoothing's times at Smoothing 1 (see FollowAttachTarget):
  // how quickly the copy's gap to the object closes (its time constant), and
  // how long the object's speed and turning are smoothed over
  const double FollowRiseSeconds = 0.25, FollowSpeedSeconds = 0.5;

  // A turn as a vector (its axis times its angle, radians), the shorter way
  XMVECTOR TurnVector(XMVECTOR q)
  {
    q = XMQuaternionNormalize(q);
    if (XMVectorGetW(q) < 0.f)
      q = XMVectorNegate(q);
    float s = XMVectorGetX(XMVector3Length(q));
    if (s < 1e-7f)
      return XMVectorScale(XMVectorSetW(q, 0.f), 2.f);
    float angle = 2.f * atan2f(s, XMVectorGetW(q));
    return XMVectorScale(XMVectorSetW(q, 0.f), angle / s);
  }

  // The turn a vector stands for
  XMVECTOR TurnOf(XMVECTOR v)
  {
    float angle = XMVectorGetX(XMVector3Length(v));
    if (angle < 1e-7f)
      return XMQuaternionNormalize(XMVectorSetW(XMVectorScale(v, 0.5f), 1.f));
    return XMQuaternionRotationNormal(XMVectorScale(v, 1.f / angle), angle);
  }

  // A gap closing as a critically damped spring does (no swinging past), at
  // the rate w, over dt: from gap e and its rate of change d to theirs dt
  // later, exactly (e(t) = (e + (d + w e) t) exp(-w t))
  void Settle(XMVECTOR& e, XMVECTOR& d, float w, float dt)
  {
    XMVECTOR b = XMVectorAdd(d, XMVectorScale(e, w));
    float fade = expf(-w * dt);
    XMVECTOR e1 = XMVectorScale(XMVectorAdd(e, XMVectorScale(b, dt)), fade);
    XMVECTOR d1 = XMVectorScale(XMVectorSubtract(d, XMVectorScale(b, w * dt)), fade);
    e = e1;
    d = d1;
  }

  // "PamClientCharacterEntity 0x...", "ClientStaticModelEntity 0x... (monorail_train_01)"
  // or "SwarmEntity 0x... member 12 (Cars)", for the log
  void DescribeTarget(attach::Target const& target, char* buffer, size_t size)
  {
    char label[sizeof(target.Label) + 3] = "";
    if (target.Label[0])
      sprintf_s(label, " (%s)", target.Label);
    if (target.Agent >= 0)
      sprintf_s(buffer, size, "%s 0x%I64X member %d%s", attach::ClassName(target.Class), target.Entity, target.Agent, label);
    else
      sprintf_s(buffer, size, "%s 0x%I64X%s", attach::ClassName(target.Class), target.Entity, label);
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

  // Only her position, and her heading if wanted: the camera stays level
  // when her body leans in a wall run or a climb. The replayed Faith while a
  // replay plays (see GetFaithPlace).
  XMFLOAT3 place;
  float yaw = 0.f;
  bool hasHeading = false;
  if (!GetFaithPlace(place, yaw, hasHeading))
    return false;
  XMMATRIX heading = m_AttachRotation && hasHeading ? XMMatrixRotationY(yaw) : XMMatrixIdentity();
  transform = heading * XMMatrixTranslation(place.x, place.y, place.z);
  return true;
}

bool CameraManager::GetFaithPlace(XMFLOAT3& place, float& heading, bool& hasHeading)
{
  hasHeading = false;
  float played[3];
  if (m_FocusReplayedFaith && m_Replay.GetPlayedFaith(played, heading, hasHeading))
  {
    place = XMFLOAT3(played[0], played[1], played[2]);
    return true;
  }
  XMFLOAT4X4A player;
  if (!GetPlayerTransform(player) || (player._41 == 0 && player._42 == 0 && player._43 == 0))
    return false;
  place = XMFLOAT3(player._41, player._42, player._43);
  if (player._31 * player._31 + player._33 * player._33 > 1e-6f)
  {
    heading = atan2f(player._31, player._33);
    hasHeading = true;
  }
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
  // watching what's in view. The camera always goes next to it: what's in the
  // list can be far away or behind the camera, and following it from there
  // looks like nothing happened.
  if (m_AttachListRequested)
  {
    m_AttachListRequested = false;
    m_AttachPicking = false;
    XMFLOAT4X4 transform;
    if (attach::ReadTransform(m_AttachListRequest, transform))
    {
      AttachToObject(m_AttachListRequest);
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
  // swarm record given to a new member somewhere else. A train or an airplane
  // whose route starts again (still the same model, with the same data) is
  // followed on from there: the camera goes next to it again.
  if (m_AttachObject && m_AttachHasLast &&
    XMVectorGetX(XMVector3Length(now.r[3] - XMLoadFloat3(&m_AttachRawPosition))) > attach::JumpLimit(m_AttachTarget))
  {
    XMFLOAT4X4 object;
    if (attach::KeepsThroughJumps(m_AttachTarget) && attach::ReadTransform(m_AttachTarget, object))
    {
      util::log::Write("The %s the camera follows jumped %.0f m (its route started again): the camera goes next to it",
        attach::KindOf(m_AttachTarget), XMVectorGetX(XMVector3Length(now.r[3] - XMLoadFloat3(&m_AttachRawPosition))));
      MoveCameraNextTo(object, m_AttachTarget); // also starts the smoothing and the carrying along afresh
    }
    else
    {
      char reason[64];
      sprintf_s(reason, "jumped more than %.0f m in one frame", attach::JumpLimit(m_AttachTarget));
      DetachObject(reason);
      m_AttachValid = false;
      m_AttachHasLast = false;
      m_AttachTrackActive = false;
      return;
    }
  }
  XMStoreFloat3(&m_AttachRawPosition, now.r[3]);

  // Smoothing, for objects: the camera follows a copy of it, so small steps
  // and wobbles don't shake the view. With a fixed playback rate dt is the
  // same every frame, for recordings.
  // 2026-11-08 (the user: "make the follow smooth slider even smoother you
  // can see the camera falling behind and it's not a smooth motion"): until
  // now the copy caught up with the object over a time constant (Smoothing x
  // 0.5 s), a first-order lag: it trails anything moving by its speed times
  // that (4 m behind a car at 20 m/s at 0.4, 12 degrees behind a 60 degree a
  // second turn), and its own speed changed in a step with every step of the
  // object's. Now it's a critically damped follower told the object's speed
  // and turning (each smoothed over Smoothing x FollowSpeedSeconds): its gap
  // to the object closes without swinging past, at the rate 1 / (Smoothing x
  // FollowRiseSeconds), and at a steady speed or turn there's no gap; its own
  // speed and turning change smoothly. Each frame is worked out exactly for
  // its time (any frame rate moves it the same). Offline (the follow sims,
  // 2026-11-08, 30 - 240 fps), at 0.4 against the old: 0.07 m behind a 20
  // m/s object instead of 4 m, 0 degrees behind a steady turn instead of 12,
  // its acceleration from a 2 cm wobble and the swarm's held steps 6 m/s2
  // instead of 103; braking from 10 m/s over a second it runs 0.5 m past
  // where it stops before settling (the old stopped 2 m short, then crept up).
  if (m_AttachObject && m_AttachSmoothing > 0.f)
  {
    XMVECTOR scale, rotation, translation;
    if (XMMatrixDecompose(&scale, &rotation, &translation, now))
    {
      rotation = XMQuaternionNormalize(rotation);
      // The object's own place and turn this frame (for the next frame's speed and turning)
      XMVECTOR objectPosition = translation, objectRotation = rotation;
      float dt = (float)smoothDt;
      if (!m_AttachHasLast || m_AttachSmoothFrames == 0)
      {
        // Afresh: the copy where the object is, still
        XMStoreFloat3(&m_AttachSmoothPosition, translation);
        XMStoreFloat4(&m_AttachSmoothRotation, rotation);
        m_AttachSmoothVelocity = m_AttachSmoothSpin = m_AttachObjectVelocity = m_AttachObjectSpin = XMFLOAT3(0.f, 0.f, 0.f);
        m_AttachSmoothFrames = 1;
        XMStoreFloat3(&m_AttachObjectLastPosition, objectPosition);
        XMStoreFloat4(&m_AttachObjectLastRotation, objectRotation);
      }
      else if (dt > 1e-6f)
      {
        // The object's speed and turning since its last frame (the turn about
        // a world axis: "last, then this"), smoothed; its first ones taken as
        // they are, the copy moving with it from there
        XMVECTOR lastRotation = XMLoadFloat4(&m_AttachObjectLastRotation);
        XMVECTOR velocity = XMVectorScale(XMVectorSubtract(translation, XMLoadFloat3(&m_AttachObjectLastPosition)), 1.f / dt);
        XMVECTOR spin = XMVectorScale(TurnVector(XMQuaternionMultiply(XMQuaternionInverse(lastRotation), rotation)), 1.f / dt);
        if (m_AttachSmoothFrames == 1)
        {
          XMStoreFloat3(&m_AttachObjectVelocity, velocity);
          XMStoreFloat3(&m_AttachObjectSpin, spin);
          XMStoreFloat3(&m_AttachSmoothVelocity, velocity);
          XMStoreFloat3(&m_AttachSmoothSpin, spin);
          m_AttachSmoothFrames = 2;
        }
        else
        {
          float blend = 1.f - expf(-dt / (float)(m_AttachSmoothing * FollowSpeedSeconds));
          XMStoreFloat3(&m_AttachObjectVelocity, XMVectorLerp(XMLoadFloat3(&m_AttachObjectVelocity), velocity, blend));
          XMStoreFloat3(&m_AttachObjectSpin, XMVectorLerp(XMLoadFloat3(&m_AttachObjectSpin), spin, blend));
        }
        XMVECTOR objectVelocity = XMLoadFloat3(&m_AttachObjectVelocity), objectSpin = XMLoadFloat3(&m_AttachObjectSpin);
        float rate = (float)(1.0 / (m_AttachSmoothing * FollowRiseSeconds));
        // The gap to where the object would be at its smoothed speed (it
        // gets to where it is now at the frame's end), closed over the frame
        XMVECTOR gap = XMVectorSubtract(XMLoadFloat3(&m_AttachSmoothPosition),
          XMVectorSubtract(translation, XMVectorScale(objectVelocity, dt)));
        XMVECTOR closing = XMVectorSubtract(XMLoadFloat3(&m_AttachSmoothVelocity), objectVelocity);
        Settle(gap, closing, rate, dt);
        translation = XMVectorAdd(translation, gap);
        XMStoreFloat3(&m_AttachSmoothVelocity, XMVectorAdd(objectVelocity, closing));
        // The same for the turn: the gap a turn after where the object
        // would be at its smoothed turning
        XMVECTOR start = XMQuaternionMultiply(rotation, TurnOf(XMVectorScale(objectSpin, -dt)));
        XMVECTOR turnGap = TurnVector(XMQuaternionMultiply(XMQuaternionInverse(start), XMLoadFloat4(&m_AttachSmoothRotation)));
        XMVECTOR turning = XMVectorSubtract(XMLoadFloat3(&m_AttachSmoothSpin), objectSpin);
        Settle(turnGap, turning, rate, dt);
        rotation = XMQuaternionNormalize(XMQuaternionMultiply(objectRotation, TurnOf(turnGap)));
        XMStoreFloat3(&m_AttachSmoothSpin, XMVectorAdd(objectSpin, turning));
        XMStoreFloat3(&m_AttachSmoothPosition, translation);
        XMStoreFloat4(&m_AttachSmoothRotation, rotation);
        XMStoreFloat3(&m_AttachObjectLastPosition, objectPosition);
        XMStoreFloat4(&m_AttachObjectLastRotation, objectRotation);
      }
      else
      {
        // No time went by: the copy stays where it is
        translation = XMLoadFloat3(&m_AttachSmoothPosition);
        rotation = XMLoadFloat4(&m_AttachSmoothRotation);
      }
      now = XMMatrixRotationQuaternion(rotation) * XMMatrixTranslationFromVector(translation);
    }
  }
  else
    m_AttachSmoothFrames = 0;
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
      attach::LabelOf(m_AttachTarget), reason);
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

namespace
{
  // An object's rows as 3D vectors: right, up, forward, position. Their w
  // isn't 0 0 0 1 in the game's transforms (the position's is about -3e-10),
  // so nothing here may use it: XMVector3TransformCoord divides by it and
  // sent the camera hundreds of thousands of kilometres away.
  XMVECTOR ObjectRow(XMMATRIX const& model, int row)
  {
    return XMVectorSetW(model.r[row], 0.f);
  }

  // A point given in an object's own space, in the world
  XMVECTOR ObjectPoint(XMMATRIX const& model, XMFLOAT3 const& local)
  {
    return ObjectRow(model, 3) + ObjectRow(model, 0) * local.x + ObjectRow(model, 1) * local.y +
      ObjectRow(model, 2) * local.z;
  }

  // Somewhere the camera can go: a number, and within 10 km of the object
  bool SanePlace(XMVECTOR place, XMMATRIX const& model)
  {
    return !XMVector3IsNaN(place) && !XMVector3IsInfinite(place) &&
      XMVectorGetX(XMVector3Length(place - ObjectRow(model, 3))) < 10000.f;
  }
}

void CameraManager::MoveCameraNextTo(XMFLOAT4X4 const& object, attach::Target const& target)
{
  XMMATRIX model = XMLoadFloat4x4(&object);

  // A security camera: at its lens, looking where it looks. Its mesh is 0.9 m
  // long along its forward row, from 0.2 m behind where it's mounted to the
  // lens 0.7 m in front (most point 72 degrees down, some 15).
  if (attach::IsSecurityCamera(target))
  {
    XMFLOAT3 lensLocal = target.HasBox ?
      XMFLOAT3(0.f, (target.BoxMin.y + target.BoxMax.y) * 0.5f, target.BoxMax.z + 0.05f) : XMFLOAT3(0.f, 0.1f, 0.75f);
    XMVECTOR lens = ObjectPoint(model, lensLocal);
    XMVECTOR backward = -XMVector3Normalize(ObjectRow(model, 2)); // the camera's row 2 points away from where it looks
    XMVECTOR right = XMVector3Normalize(XMVector3Cross(XMVector3Normalize(ObjectRow(model, 1)), backward));
    XMVECTOR up = XMVector3Cross(backward, right);
    XMMATRIX rotation;
    rotation.r[0] = right;
    rotation.r[1] = up;
    rotation.r[2] = backward;
    rotation.r[3] = XMVectorSet(0, 0, 0, 1);
    XMVECTOR orientation = XMQuaternionNormalize(XMQuaternionRotationMatrix(rotation));
    if (!SanePlace(lens, model) || XMVector4IsNaN(orientation))
    {
      util::log::Warning("Not moving the camera to the security camera: its transform doesn't make sense");
      return;
    }
    XMStoreFloat3(&m_Camera.Position, lens);
    XMStoreFloat4(&m_Camera.Rotation, orientation);
    m_AttachHasLast = false;
    return;
  }

  // Behind it and a little above, looking at it. Turning with it from far
  // away swings the camera around a lot, from here it's a chase camera.
  bool person = attach::GroupOf(target) == attach::Group::People;
  float back = person ? 3.5f : 8.f;
  float above = person ? 0.6f : 2.5f;
  XMVECTOR aim = XMVectorSet(object._41, object._42 + attach::AimHeight(target) + 0.3f, object._43, 0);

  // A static model with a box (a train carriage is 11 - 15 m long, a blimp
  // 109 m): far enough back to see all of it, looking at its middle
  if (target.HasBox)
  {
    XMFLOAT3 centreLocal((target.BoxMin.x + target.BoxMax.x) * 0.5f, (target.BoxMin.y + target.BoxMax.y) * 0.5f,
      (target.BoxMin.z + target.BoxMax.z) * 0.5f);
    aim = ObjectPoint(model, centreLocal);
    float sizeX = (target.BoxMax.x - target.BoxMin.x) * XMVectorGetX(XMVector3Length(ObjectRow(model, 0)));
    float sizeY = (target.BoxMax.y - target.BoxMin.y) * XMVectorGetX(XMVector3Length(ObjectRow(model, 1)));
    float sizeZ = (target.BoxMax.z - target.BoxMin.z) * XMVectorGetX(XMVector3Length(ObjectRow(model, 2)));
    back = fmaxf(back, fmaxf(sizeX, sizeZ) * 0.75f + 4.f);
    above = fmaxf(above, sizeY * 0.5f + 1.5f);
  }

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

  XMVECTOR orientation = XMQuaternionNormalize(XMQuaternionRotationMatrix(rotation));
  if (!SanePlace(position, model) || XMVector4IsNaN(orientation))
  {
    util::log::Warning("Not moving the camera next to the %s: its transform doesn't make sense", attach::KindOf(target));
    return;
  }
  XMStoreFloat3(&m_Camera.Position, position);
  XMStoreFloat4(&m_Camera.Rotation, orientation);
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
    m_NearbyFar = m_NearbyPendingFar;
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
  const int MaxShown = 300;   // within range
  const int MaxFar = 100;     // trains and aircraft further away (the city had 45)

  __int64 pSoldier = GetPlayerSoldier();
  bool fromFaith = false;
  if (!attach::FindClasses(pSoldier, fromFaith))
  {
    m_NearbyPending.clear();
    m_NearbyPendingTotal = 0;
    m_NearbyPendingFar = 0;
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
  int total = 0, nearCount = 0, farCount = 0;
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
    // list would be all buildings. Trains, aircraft and security cameras are
    // listed all the same.
    if (attach::OnlyWhenMoving(target) && !(entry.LastMoved && now - entry.LastMoved < 30000))
      continue;

    // Only what's ticked, so the nearest of it are listed (unticked rows
    // would take up the list's places)
    NearbyItem item = { target, target.Position, entry.LastMoved, false };
    if (!NearbyGroupShown(attach::GroupOf(target)) || (m_NearbyMovingOnly && !IsNearbyMoving(item, now)))
      continue;

    // Nearest first: what's further away than the range (trains and
    // aircraft) comes after everything within it
    if (target.Distance > m_NearbyRadius)
    {
      if (farCount < MaxFar)
      {
        items.push_back(item);
        ++farCount;
      }
      continue;
    }
    ++total;
    if (nearCount < MaxShown)
    {
      items.push_back(item);
      ++nearCount;
    }
  }
  m_NearbyPending.swap(items);
  m_NearbyPendingTotal = total;
  m_NearbyPendingFar = farCount;
  m_NearbyPendingDropped = m_NearbyStats->Dropped;
  m_NearbyHasPending = true;
  m_NearbySeen.swap(seen);
}

bool CameraManager::ProjectToScreen(XMFLOAT3 const& world, float& x, float& y)
{
  bool onScreen = false;
  return ProjectPoint(world, x, y, onScreen) && onScreen;
}

bool CameraManager::ProjectPoint(XMFLOAT3 const& world, float& x, float& y, bool& onScreen)
{
  // The camera's rows are right, up, and the opposite of where it looks
  XMMATRIX camera = XMLoadFloat4x4(&m_Camera.Transform);
  XMVECTOR offset = XMLoadFloat3(&world) - camera.r[3];
  float right = XMVectorGetX(XMVector3Dot(offset, camera.r[0]));
  float up = XMVectorGetX(XMVector3Dot(offset, camera.r[1]));
  float depth = -XMVectorGetX(XMVector3Dot(offset, camera.r[2]));
  if (depth < 0.1f)
    return false;

  // Where the Panini projection puts it, when it's on (see panini::ToPanini)
  float px = right / depth, py = up / depth;
  panini::ToPanini(px, py);
  float scale = tanf(m_ViewFovY * 0.5f) * panini::ScreenScale();
  float nx = px / (scale * m_ViewAspect);
  float ny = py / scale;
  ImVec2 size = ImGui::GetIO().DisplaySize;
  x = (nx * 0.5f + 0.5f) * size.x;
  y = (0.5f - ny * 0.5f) * size.y;
  onScreen = fabsf(nx) <= 1.f && fabsf(ny) <= 1.f;
  return true;
}

void CameraManager::DrawOverlay(bool menuOpen)
{
  std::lock_guard<std::mutex> lock(m_Mutex);
  bool playing = m_TrackPlayer.IsPlaying();
  // An editing aid: not while the track plays (the menu can be in a recording).
  // With the preview on (Preview the track on its path) the menu closed too,
  // to watch it from anywhere; nothing on it can be clicked then.
  bool pathShown = m_CameraEnabled && !playing && ((menuOpen && m_ShowTrackPath) || m_PathPreview);
  // A drag on the path ends with it, and its right-click menu closes (with
  // the menu too: no clicks reach it then)
  if (!pathShown || !menuOpen)
    EndPathDrag();
  DrawTrackPathMenu(pathShown && menuOpen);
  // GAME > First person > Bodycam's REC overlay and vignette (only while it writes the view: never with this camera)
  bodycam::DrawOverlay();
  if (!m_CameraEnabled)
    return;
  DrawComposition(playing);
  if (pathShown)
    DrawTrackPath();
}

bool CameraManager::WantsOverlay()
{
  std::lock_guard<std::mutex> lock(m_Mutex);
  bool guides = m_GuideThirds || m_GuideGolden || m_GuideCenter || m_GuideSafe;
  return bodycam::WantsOverlay() || (m_CameraEnabled && (m_Letterbox > 0 ||
    (guides && (m_GuidesWhilePlaying || !m_TrackPlayer.IsPlaying())) || (m_PathPreview && !m_TrackPlayer.IsPlaying())));
}

void CameraManager::DrawComposition(bool playing)
{
  bool guides = (m_GuideThirds || m_GuideGolden || m_GuideCenter || m_GuideSafe) && (m_GuidesWhilePlaying || !playing);
  float ratio = m_Letterbox > 0 && m_Letterbox < LetterboxCount ? LetterboxRatios[m_Letterbox].Ratio : 0.f;
  if (!guides && ratio <= 0.f)
    return;
  // A window over the whole screen that takes no input, behind the menu's
  ImVec2 size = ImGui::GetIO().DisplaySize;
  ImGui::SetNextWindowPos(ImVec2(0.f, 0.f));
  ImGui::SetNextWindowSize(size);
  ImGui::SetNextWindowBgAlpha(0.f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.f, 0.f));
  ImGui::Begin("##Composition", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
    ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoInputs |
    ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoBringToFrontOnFocus);
  ImDrawList* pDrawList = ImGui::GetWindowDrawList();

  // The picture's frame: the letterbox's (bars above and below, or at the sides)
  ImVec2 from(0.f, 0.f), to = size;
  if (ratio > 0.f && size.x > 0.f && size.y > 0.f)
  {
    ImU32 bar = IM_COL32(0, 0, 0, (int)(Clamp(m_LetterboxOpacity, 0.f, 1.f) * 255.f + 0.5f));
    if (size.x / size.y > ratio)
    {
      float width = size.y * ratio;
      from.x = (size.x - width) * 0.5f;
      to.x = from.x + width;
      pDrawList->AddRectFilled(ImVec2(0.f, 0.f), ImVec2(from.x, size.y), bar);
      pDrawList->AddRectFilled(ImVec2(to.x, 0.f), ImVec2(size.x, size.y), bar);
    }
    else
    {
      float height = size.x / ratio;
      from.y = (size.y - height) * 0.5f;
      to.y = from.y + height;
      pDrawList->AddRectFilled(ImVec2(0.f, 0.f), ImVec2(size.x, from.y), bar);
      pDrawList->AddRectFilled(ImVec2(0.f, to.y), ImVec2(size.x, size.y), bar);
    }
  }

  if (guides)
  {
    const ImU32 line = IM_COL32(255, 255, 255, 150);
    const ImU32 shadow = IM_COL32(0, 0, 0, 110);
    float w = to.x - from.x, h = to.y - from.y;
    auto vertical = [&](float f) {
      float x = from.x + w * f;
      pDrawList->AddLine(ImVec2(x + 1.f, from.y), ImVec2(x + 1.f, to.y), shadow, 1.f);
      pDrawList->AddLine(ImVec2(x, from.y), ImVec2(x, to.y), line, 1.f);
    };
    auto horizontal = [&](float f) {
      float y = from.y + h * f;
      pDrawList->AddLine(ImVec2(from.x, y + 1.f), ImVec2(to.x, y + 1.f), shadow, 1.f);
      pDrawList->AddLine(ImVec2(from.x, y), ImVec2(to.x, y), line, 1.f);
    };
    if (m_GuideThirds)
      for (float f : { 1.f / 3.f, 2.f / 3.f })
      {
        vertical(f);
        horizontal(f);
      }
    if (m_GuideGolden)
      for (float f : { 0.381966f, 0.618034f })
      {
        vertical(f);
        horizontal(f);
      }
    if (m_GuideCenter)
    {
      ImVec2 c((from.x + to.x) * 0.5f, (from.y + to.y) * 0.5f);
      float arm = (std::min)(w, h) * 0.03f;
      pDrawList->AddLine(ImVec2(c.x - arm, c.y), ImVec2(c.x + arm, c.y), line, 1.5f);
      pDrawList->AddLine(ImVec2(c.x, c.y - arm), ImVec2(c.x, c.y + arm), line, 1.5f);
    }
    if (m_GuideSafe)
    {
      // Action safe (93 %) and title safe (90 %), as EBU R 95 has them
      for (float f : { 0.93f, 0.90f })
      {
        float mx = w * (1.f - f) * 0.5f, my = h * (1.f - f) * 0.5f;
        pDrawList->AddRect(ImVec2(from.x + mx, from.y + my), ImVec2(to.x - mx, to.y - my), line, 0.f, ImDrawCornerFlags_All, 1.f);
      }
    }
  }
  ImGui::End();
  ImGui::PopStyleVar(2);
}

void CameraManager::DrawTrackPath()
{
  static XMFLOAT3 points[MaxPathPoints];   // render thread, under m_Mutex
  static bool breaks[MaxPathPoints];
  static ImVec2 keyScreen[MaxPathPoints];
  static bool keyShown[MaxPathPoints], roundFocus[MaxPathPoints];
  static XMFLOAT3 keyPlace[MaxPathPoints];
  static XMFLOAT4 keyRotation[MaxPathPoints];
  static float keyFov[MaxPathPoints];
  // The arches of the moves round the focus, by the keyframe each starts
  // at: their handle (the arc's middle) and the plain arc's middle under it
  static XMFLOAT3 archPlace[MaxPathPoints], archPlain[MaxPathPoints];
  static ImVec2 archScreen[MaxPathPoints];
  static bool archShown[MaxPathPoints];
  XMFLOAT3 const* focus = m_FocusOk ? &m_FocusPoint : nullptr;
  float baseYaw = OrbitBaseYaw();
  ImGuiIO& io = ImGui::GetIO();

  // The point held with the mouse follows the cursor (before the path is
  // drawn, so it's drawn where it went); let go, the drag ends, one undo step.
  // An orbit keyframe's place and an arch go round the focus: without one
  // the drag ends too.
  if (m_PathDragActive)
  {
    // The held point's place on screen moved by itself since the last frame
    // (the camera moved or turned: keys, a gamepad, orbit, aim, following, a
    // heavy camera coasting; it zoomed; the point an orbit keyframe goes round
    // moved): it's taken again from where it is now, against where the cursor
    // was then, so none of that carries it and the mouse still moves it
    // (2026-11-09, the user: "Camera nodes should stay in place and not
    // follow the camera"; it was kept under the cursor, so it went wherever
    // the camera went and was left there). While it can't be seen (behind
    // the camera) nothing moves; it's taken again once it can be.
    float lastMouseX = m_PathDragMouseX, lastMouseY = m_PathDragMouseY;
    m_PathDragMouseX = io.MousePos.x;
    m_PathDragMouseY = io.MousePos.y;
    XMFLOAT3 held;
    float heldX = 0.f, heldY = 0.f;
    bool heldOnScreen = false;
    bool heldThere = io.MouseDown[0] && PathDragPart(held);
    bool heldSeen = heldThere && ProjectPoint(held, heldX, heldY, heldOnScreen);
    if (heldSeen && (!m_PathDragHeldSeen || heldX != m_PathDragHeldX || heldY != m_PathDragHeldY))
    {
      m_PathDragBase = held;
      m_PathDragOffsetX = heldX - lastMouseX;
      m_PathDragOffsetY = heldY - lastMouseY;
    }
    if (!io.MouseDown[0] || !heldThere)
      EndPathDrag();
    else if (!heldSeen)
    {
      // behind the camera: nothing this frame
    }
    else if (m_PathDragPart == 3)
    {
      // An arch: its handle moves in the upright plane through the focus and
      // the arc's middle (where the focus is now); up and down it rises or
      // dips, out and in it goes further from or nearer the focus there
      XMFLOAT3 now, place, pivot;
      float yaw, pitch, distance;
      if (!m_TrackPlayer.GetOrbitArch(m_PathDragKeyframe, focus, baseYaw, now, yaw, pitch, distance, pivot))
        EndPathDrag();
      else
      {
        XMVECTOR out = XMVectorSet(sinf(yaw), 0.f, cosf(yaw), 0.f);
        if (PathDragAlong(now, out, XMVectorSet(0.f, 1.f, 0.f, 0.f), true, io.MousePos.x + m_PathDragOffsetX,
            io.MousePos.y + m_PathDragOffsetY, place) &&
            XMVectorGetX(XMVector3Length(XMLoadFloat3(&place) - XMLoadFloat3(&now))) > 1e-4f)
        {
          XMVECTOR offset = XMLoadFloat3(&place) - XMLoadFloat3(&pivot);
          // (not past its point's upright line: the arc's middle stays on its side)
          float across = (std::max)(XMVectorGetX(XMVector3Dot(offset, out)), 0.05f);
          float up = XMVectorGetY(offset);
          m_TrackPlayer.SetOrbitArch(m_PathDragKeyframe, atan2f(up, across) - pitch,
            sqrtf(across * across + up * up) - distance, focus, baseYaw, OrbitMinDistance, OrbitMaxPitch);
        }
      }
    }
    else
    {
      // Shift held or let go: from where it is now, the other way
      bool height = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
      XMFLOAT3 now;
      float x, y;
      bool onScreen = false;
      if (height != m_PathDragHeight && PathDragPart(now) && ProjectPoint(now, x, y, onScreen))
      {
        m_PathDragHeight = height;
        m_PathDragBase = now;
        m_PathDragOffsetX = x - lastMouseX;
        m_PathDragOffsetY = y - lastMouseY;
      }
      // Only once it moves: a click that doesn't move it changes nothing
      // (no undo step, a keyframe's handles stay automatic)
      XMFLOAT3 place, held;
      if (PathDragPlace(m_PathDragBase, m_PathDragHeight, io.MousePos.x + m_PathDragOffsetX,
          io.MousePos.y + m_PathDragOffsetY, place) && PathDragPart(held) &&
          XMVectorGetX(XMVector3Length(XMLoadFloat3(&place) - XMLoadFloat3(&held))) > 1e-4f)
      {
        if (m_PathDragPart == 0 && m_PathDragOrbit)
        {
          // An orbit keyframe: where round its point that is (as circling has
          // it: no nearer or further than OrbitMinDistance - OrbitMaxDistance,
          // no more than OrbitMaxPitch up or down; yaw from the world's
          // heading), its own view kept
          XMFLOAT3 pivot;
          if (!m_TrackPlayer.GetOrbitPivot(m_PathDragKeyframe, focus, baseYaw, pivot))
            EndPathDrag();
          else
          {
            XMFLOAT3 offset;
            XMStoreFloat3(&offset, XMLoadFloat3(&place) - XMLoadFloat3(&pivot));
            float across = sqrtf(offset.x * offset.x + offset.z * offset.z);
            float distance = Clamp(sqrtf(across * across + offset.y * offset.y), OrbitMinDistance, OrbitMaxDistance);
            float pitch = Clamp(atan2f(offset.y, across), -OrbitMaxPitch, OrbitMaxPitch);
            std::vector<CatmullRomNode> const& nodes = m_TrackPlayer.GetNodes();
            float yaw = across > 1e-4f ? atan2f(offset.x, offset.z) :
              (m_PathDragKeyframe < (int)nodes.size() ? nodes[m_PathDragKeyframe].OrbitYaw : 0.f);
            m_TrackPlayer.SetOrbitKeyframePlace(m_PathDragKeyframe, yaw, pitch, distance, focus, baseYaw);
          }
        }
        else if (m_PathDragPart == 0)
          m_TrackPlayer.SetKeyframePosition(m_PathDragKeyframe, place);
        else
          m_TrackPlayer.SetKeyframeHandle(m_PathDragKeyframe, m_PathDragPart == 2, place);
      }
    }
    // Where the held point is on screen now, after this frame's move (see above)
    XMFLOAT3 after;
    float afterX = 0.f, afterY = 0.f;
    bool afterOnScreen = false;
    m_PathDragHeldSeen = m_PathDragActive && PathDragPart(after) && ProjectPoint(after, afterX, afterY, afterOnScreen);
    m_PathDragHeldX = afterX;
    m_PathDragHeldY = afterY;
  }

  int count = m_TrackPlayer.GetPathPoints(points, breaks, MaxPathPoints, PathPointsPerSegment, focus, baseYaw);
  if (count < 1)
    return;
  ImDrawList* pDrawList = ImGui::GetOverlayDrawList();
  float fontSize = ImGui::GetFontSize();
  const ImU32 colour = IM_COL32(255, 205, 60, 235);
  const ImU32 orbitColour = IM_COL32(120, 230, 215, 245);   // an orbit keyframe's dot: it goes round the focus
  const ImU32 shadow = IM_COL32(0, 0, 0, 160);
  const ImU32 white = IM_COL32(255, 255, 255, 255);
  const ImU32 handleColour = IM_COL32(110, 200, 255, 240);
  const ImU32 archColour = IM_COL32(150, 240, 110, 245);

  // The path: a line between each two points in front of the camera
  bool hasLast = false;
  ImVec2 last;
  for (int i = 0; i < count; ++i)
  {
    float x, y;
    bool onScreen = false;
    bool front = ProjectPoint(points[i], x, y, onScreen);
    ImVec2 at(x, y);
    if (front && hasLast && !breaks[i])
    {
      pDrawList->AddLine(last, at, shadow, 4.f);
      pDrawList->AddLine(last, at, colour, 2.f);
    }
    hasLast = front;
    last = at;
  }

  // Where it goes through something solid (see StepTrackWallCheck)
  const ImU32 wallColour = IM_COL32(255, 60, 50, 255);
  for (WallStretch const& stretch : m_WallStretches)
  {
    float ax, ay, bx, by;
    bool aOn = false, bOn = false;
    if (ProjectPoint(stretch.A, ax, ay, aOn) && ProjectPoint(stretch.B, bx, by, bOn))
    {
      pDrawList->AddLine(ImVec2(ax, ay), ImVec2(bx, by), shadow, 7.f);
      pDrawList->AddLine(ImVec2(ax, ay), ImVec2(bx, by), wallColour, 4.f);
    }
  }

  // Its keyframes (where each is and looks), and the curve handles of the
  // one picked (clicked on)
  int keyframes = m_TrackPlayer.GetKeyframePoints(keyPlace, MaxPathPoints, focus, baseYaw, roundFocus, keyRotation,
    keyFov);
  for (int i = 0; i < keyframes; ++i)
  {
    float x = 0.f, y = 0.f;
    keyShown[i] = ProjectToScreen(keyPlace[i], x, y);
    keyScreen[i] = ImVec2(x, y);
  }
  int picked = m_TrackPlayer.GetPickedKeyframe();
  XMFLOAT3 handles[2];
  bool hasHandle[2] = { false, false };
  bool showHandles = picked >= 0 && picked < keyframes && !roundFocus[picked] &&
    m_TrackPlayer.GetKeyframeHandles(picked, focus != nullptr, handles[0], handles[1], hasHandle[0], hasHandle[1]);
  ImVec2 handleScreen[2];
  bool handleShown[2] = { false, false };
  for (int side = 0; side < 2 && showHandles; ++side)
  {
    float x = 0.f, y = 0.f;
    handleShown[side] = hasHandle[side] && ProjectToScreen(handles[side], x, y);
    handleScreen[side] = ImVec2(x, y);
  }

  // The arches: a handle in the middle of each move round the focus
  int arches = 0;
  for (int i = 0; i + 1 < keyframes; ++i)
  {
    archShown[i] = false;
    float yaw, pitch, distance;
    XMFLOAT3 archPivot;
    if (!m_TrackPlayer.GetOrbitArch(i, focus, baseYaw, archPlace[i], yaw, pitch, distance, archPivot))
      continue;
    XMStoreFloat3(&archPlain[i], XMLoadFloat3(&archPivot) + OrbitDirection(yaw, pitch) * distance);
    float x = 0.f, y = 0.f;
    archShown[i] = ProjectToScreen(archPlace[i], x, y);
    archScreen[i] = ImVec2(x, y);
    arches = i + 1;
  }

  // What the cursor is on, off the menu's windows: the nearest keyframe (an
  // orbit keyframe's dot moves round the focus), handle or arch
  int hoverKeyframe = -1, hoverPart = 0;
  if (!m_PathDragActive && !io.WantCaptureMouse)
  {
    float reach = fontSize * 0.7f;
    float best = reach * reach;
    auto consider = [&](ImVec2 at, int keyframe, int part)
    {
      float dx = at.x - io.MousePos.x, dy = at.y - io.MousePos.y;
      if (dx * dx + dy * dy <= best)
      {
        best = dx * dx + dy * dy;
        hoverKeyframe = keyframe;
        hoverPart = part;
      }
    };
    for (int i = 0; i < keyframes; ++i)
      if (keyShown[i])
        consider(keyScreen[i], i, 0);
    for (int side = 0; side < 2; ++side)
      if (handleShown[side])
        consider(handleScreen[side], picked, side + 1);
    for (int i = 0; i < arches; ++i)
      if (archShown[i])
        consider(archScreen[i], i, 3);
  }

  // A click on the picture (not on the menu): grab what's under the cursor,
  // or put the picked keyframe down
  if (io.MouseClicked[0] && !io.MouseDownOwned[0] && !m_PathDragActive)
  {
    if (hoverKeyframe >= 0)
    {
      if (hoverPart == 0)
        m_TrackPlayer.PickKeyframe(hoverKeyframe);
      ImVec2 at = hoverPart == 0 ? keyScreen[hoverKeyframe] :
        (hoverPart == 3 ? archScreen[hoverKeyframe] : handleScreen[hoverPart - 1]);
      m_PathDragActive = true;
      m_PathDragKeyframe = hoverKeyframe;
      m_PathDragPart = hoverPart;
      m_PathDragOrbit = hoverPart == 0 && roundFocus[hoverKeyframe];
      m_PathDragHeight = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
      m_PathDragBase = hoverPart == 0 ? keyPlace[hoverKeyframe] :
        (hoverPart == 3 ? archPlace[hoverKeyframe] : handles[hoverPart - 1]);
      m_PathDragOffsetX = at.x - io.MousePos.x;
      m_PathDragOffsetY = at.y - io.MousePos.y;
      // (taken again from where it is on the first frame of the drag)
      m_PathDragHeldSeen = false;
      m_PathDragMouseX = io.MousePos.x;
      m_PathDragMouseY = io.MousePos.y;
      m_TrackPlayer.BeginPathDrag();
    }
    else
      m_TrackPlayer.PickKeyframe(-1);
  }
  // A right click on one: its menu (DrawTrackPathMenu)
  if (io.MouseClicked[1] && !io.MouseDownOwned[1] && !m_PathDragActive && hoverKeyframe >= 0)
  {
    m_PathMenuKeyframe = hoverKeyframe;
    m_PathMenuPart = hoverPart == 3 ? 3 : 0;
    if (hoverPart != 3)
      m_TrackPlayer.PickKeyframe(hoverKeyframe);
    ImGui::OpenPopup("##TrackPathMenu");
  }

  // Where each keyframe looks, when the track sets the view (Keyframe
  // options > Rotation): its camera's outline, under the dots, the picked or
  // hovered one's brighter with a line on along where it looks
  if (m_ShowKeyframeViews && m_TrackPlayer.IsRotationLocked())
    for (int i = 0; i < keyframes; ++i)
    {
      bool lit = i == picked || (hoverKeyframe == i && hoverPart == 0) ||
        (m_PathDragActive && m_PathDragKeyframe == i && m_PathDragPart == 0);
      DrawKeyframeView(pDrawList, keyPlace[i], keyRotation[i], keyFov[i], m_KeyframeViewSize,
        lit ? IM_COL32(255, 255, 255, 235) : IM_COL32(255, 255, 255, 125), lit ? 1.6f : 1.f, lit ? 6.f : 0.f);
    }

  // The picked keyframe's handles: a line out to each, under the dots
  for (int side = 0; side < 2; ++side)
    if (handleShown[side] && keyShown[picked])
    {
      pDrawList->AddLine(keyScreen[picked], handleScreen[side], shadow, 3.5f);
      pDrawList->AddLine(keyScreen[picked], handleScreen[side], handleColour, 1.5f);
    }

  // The arches' handles: diamonds, with a line down to the plain arc's
  // middle while it's raised, dipped or bulged
  for (int i = 0; i < arches; ++i)
  {
    if (!archShown[i])
      continue;
    float px, py;
    bool onScreen = false;
    if (XMVectorGetX(XMVector3Length(XMLoadFloat3(&archPlace[i]) - XMLoadFloat3(&archPlain[i]))) > 0.01f &&
        ProjectPoint(archPlain[i], px, py, onScreen))
    {
      pDrawList->AddLine(ImVec2(px, py), archScreen[i], shadow, 3.f);
      pDrawList->AddLine(ImVec2(px, py), archScreen[i], archColour, 1.f);
      pDrawList->AddCircleFilled(ImVec2(px, py), fontSize * 0.09f, archColour, 8);
    }
    bool held = m_PathDragActive && m_PathDragKeyframe == i && m_PathDragPart == 3;
    float r = fontSize * ((held || (hoverKeyframe == i && hoverPart == 3)) ? 0.32f : 0.24f);
    ImVec2 c = archScreen[i];
    pDrawList->AddQuadFilled(ImVec2(c.x, c.y - r - 2.f), ImVec2(c.x + r + 2.f, c.y), ImVec2(c.x, c.y + r + 2.f),
      ImVec2(c.x - r - 2.f, c.y), shadow);
    pDrawList->AddQuadFilled(ImVec2(c.x, c.y - r), ImVec2(c.x + r, c.y), ImVec2(c.x, c.y + r), ImVec2(c.x - r, c.y),
      archColour);
  }

  char label[16];
  for (int i = 0; i < keyframes; ++i)
  {
    if (!keyShown[i])
      continue;
    float x = keyScreen[i].x, y = keyScreen[i].y;
    bool held = m_PathDragActive && m_PathDragKeyframe == i && m_PathDragPart == 0;
    float size = (held || (hoverKeyframe == i && hoverPart == 0)) ? 1.3f : 1.f;
    pDrawList->AddCircleFilled(ImVec2(x, y), fontSize * 0.32f * size, shadow, 12);
    pDrawList->AddCircleFilled(ImVec2(x, y), fontSize * 0.22f * size, roundFocus[i] ? orbitColour : colour, 12);
    if (i == picked)
      pDrawList->AddCircle(ImVec2(x, y), fontSize * 0.45f, white, 16, 2.f);
    sprintf_s(label, "%d", i + 1);
    ImVec2 at(x + fontSize * 0.5f, y - fontSize * 1.1f);
    pDrawList->AddText(ImVec2(at.x + 1, at.y + 1), shadow, label);
    pDrawList->AddText(at, white, label);
  }

  // The handles themselves: small squares, on top
  for (int side = 0; side < 2; ++side)
  {
    if (!handleShown[side])
      continue;
    bool held = m_PathDragActive && m_PathDragKeyframe == picked && m_PathDragPart == side + 1;
    float r = fontSize * ((held || (hoverKeyframe == picked && hoverPart == side + 1)) ? 0.26f : 0.19f);
    ImVec2 c = handleScreen[side];
    pDrawList->AddRectFilled(ImVec2(c.x - r - 1.5f, c.y - r - 1.5f), ImVec2(c.x + r + 1.5f, c.y + r + 1.5f), shadow);
    pDrawList->AddRectFilled(ImVec2(c.x - r, c.y - r), ImVec2(c.x + r, c.y + r), handleColour);
  }

  // The preview (Preview the track on its path): the camera as the track
  // would have it now, on top, the real one left where it is. Its clock runs
  // in real time at the playback speed, round and round with a second's wait
  // at the end; a keyframe being changed in the list (TrackPlayer::
  // TakePreviewHold) or its dot dragged here holds it at that keyframe.
  if (m_PathPreview)
  {
    auto now = std::chrono::steady_clock::now();
    double dt = m_PreviewClockOn ? std::chrono::duration<double>(now - m_PreviewLast).count() : 0.0;
    m_PreviewLast = now;
    m_PreviewClockOn = true;
    // (a frame that took long, or a gap while it wasn't drawn: a step, not a jump)
    dt = dt < 0.0 ? 0.0 : (dt > 0.1 ? 0.1 : dt);
    double end = m_TrackPlayer.GetTrackLength();
    double held = 0.0;
    std::vector<CatmullRomNode> const& keyNodes = m_TrackPlayer.GetNodes();
    if (m_TrackPlayer.TakePreviewHold(held))
    {
      m_PreviewTime = held;
      m_PreviewWaited = 0.0;
    }
    else if (m_PathDragActive && m_PathDragPart == 0 && m_PathDragKeyframe >= 0 &&
             m_PathDragKeyframe < (int)keyNodes.size())
    {
      m_PreviewTime = keyNodes[m_PathDragKeyframe].TimeStamp;
      m_PreviewWaited = 0.0;
    }
    else if (m_PreviewTime >= end)
    {
      m_PreviewTime = end;
      m_PreviewWaited += dt;
      if (m_PreviewWaited >= 1.0)
      {
        m_PreviewTime = 0.0;
        m_PreviewWaited = 0.0;
      }
    }
    else
      m_PreviewTime = (std::min)(end, (m_PreviewTime < 0.0 ? 0.0 : m_PreviewTime) + dt * m_TrackPlayer.GetPlaybackSpeed());

    XMFLOAT3 previewPlace;
    XMFLOAT4 previewRotation;
    float previewFov;
    float px, py;
    if (m_TrackPlayer.PreviewAt(m_PreviewTime, focus, baseYaw, previewPlace, previewRotation, previewFov) &&
        ProjectToScreen(previewPlace, px, py))
    {
      const ImU32 previewColour = IM_COL32(255, 110, 205, 255);
      // Where it looks, when the track sets the view, with its field of view
      // when the track sets that too (else the camera's own)
      if (m_TrackPlayer.IsRotationLocked())
        DrawKeyframeView(pDrawList, previewPlace, previewRotation,
          m_TrackPlayer.IsFovLocked() ? previewFov : m_Camera.FieldOfView, m_KeyframeViewSize, previewColour, 2.f, 8.f);
      pDrawList->AddCircleFilled(ImVec2(px, py), fontSize * 0.36f, shadow, 16);
      pDrawList->AddCircleFilled(ImVec2(px, py), fontSize * 0.26f, previewColour, 16);
      char time[48];
      sprintf_s(time, "%.2f / %.2f s", m_PreviewTime, end);
      ImVec2 at(px + fontSize * 0.6f, py + fontSize * 0.3f);
      pDrawList->AddText(ImVec2(at.x + 1, at.y + 1), shadow, time);
      pDrawList->AddText(at, previewColour, time);
    }
  }
  else
    m_PreviewClockOn = false;

  // What a drag there does, next to the cursor; while an orbit keyframe or
  // an arch is dragged, where it is round the focus
  char hint[192] = "";
  std::vector<CatmullRomNode> const& nodes = m_TrackPlayer.GetNodes();
  if (m_PathDragActive && m_PathDragKeyframe >= 0 && m_PathDragKeyframe < (int)nodes.size() &&
      (m_PathDragOrbit || m_PathDragPart == 3))
  {
    CatmullRomNode const& node = nodes[m_PathDragKeyframe];
    if (m_PathDragPart == 3)
      sprintf_s(hint, "%s %.0f" UI_DEG ", %.1f m %s the focus in the middle", node.OrbitLift >= 0.f ? "Rising" : "Dipping",
        fabsf(XMConvertToDegrees(node.OrbitLift)), fabsf(node.OrbitBulge), node.OrbitBulge >= 0.f ? "further from" : "nearer");
    else
      sprintf_s(hint, "%.0f" UI_DEG " round, %.0f" UI_DEG " up, %.1f m from its point", XMConvertToDegrees(node.OrbitYaw),
        XMConvertToDegrees(node.OrbitPitch), node.OrbitDistance);
  }
  else if (hoverKeyframe >= 0 && hoverKeyframe < (int)nodes.size())
  {
    float time = nodes[hoverKeyframe].TimeStamp;
    if (hoverPart == 3)
      sprintf_s(hint, "Arch to keyframe %d: drag up or down to raise or dip it, out or in to widen or narrow it; "
        "right-click to flatten it", hoverKeyframe + 2);
    else if (hoverPart == 0 && roundFocus[hoverKeyframe])
      sprintf_s(hint, "Orbit keyframe %d (%.2f s): drag it round its point (Shift: up and down); right-click for more",
        hoverKeyframe + 1, time);
    else if (hoverPart == 0)
      sprintf_s(hint, "Keyframe %d (%.2f s): drag to move it (Shift: up and down); right-click for more",
        hoverKeyframe + 1, time);
    else
      sprintf_s(hint, "Curve handle: drag to bend the path (Shift: up and down); right-click for more");
  }
  if (hint[0])
  {
    ImVec2 at(io.MousePos.x + fontSize * 1.2f, io.MousePos.y + fontSize * 0.6f);
    pDrawList->AddText(ImVec2(at.x + 1, at.y + 1), shadow, hint);
    pDrawList->AddText(at, white, hint);
  }
}

void CameraManager::DrawTrackPathMenu(bool pathShown)
{
  if (!ImGui::BeginPopup("##TrackPathMenu"))
    return;
  std::vector<CatmullRomNode> const& nodes = m_TrackPlayer.GetNodes();
  int k = m_PathMenuKeyframe;
  // (closed with the path, or when what it was for is gone: undone, another track)
  if (!pathShown || k < 0 || k >= (int)nodes.size() || (m_PathMenuPart == 3 && !m_TrackPlayer.IsOrbitSegment(k)))
    ImGui::CloseCurrentPopup();
  else if (m_PathMenuPart == 3)
  {
    ImGui::TextDisabled("The arch from keyframe %d to %d", k + 1, k + 2);
    if (ImGui::MenuItem("Flatten the arch", nullptr, false, nodes[k].OrbitLift != 0.f || nodes[k].OrbitBulge != 0.f))
      m_TrackPlayer.FlattenOrbitArch(k);
  }
  else
  {
    ImGui::TextDisabled("Keyframe %d, at %.2f s", k + 1, nodes[k].TimeStamp);
    if (ImGui::MenuItem("Go to it"))
      m_TrackPlayer.GoToKeyframe(k, m_Camera, m_FocusOk ? &m_FocusPoint : nullptr, OrbitBaseYaw());
    if (ImGui::MenuItem("Automatic curve", nullptr, false, nodes[k].Handles))
      m_TrackPlayer.ResetKeyframeHandles(k);
    if (ImGui::MenuItem("Delete it"))
      m_TrackPlayer.DeleteKeyframe(k);
  }
  ImGui::EndPopup();
}

void CameraManager::DrawKeyframeView(ImDrawList* pDrawList, XMFLOAT3 const& place, XMFLOAT4 const& rotation,
  float fieldOfView, float length, unsigned int colour, float thickness, float aimLength)
{
  // A thing in the world, length metres from its place to its outline: where
  // the camera is changes nothing but how it's seen (before 2026-11-09 it was
  // sized from the camera's distance to stay about 2.4 lines of text long on
  // screen, so it grew and shrank in the world as the camera moved and
  // turned; the user: "Camera nodes should stay in place and not follow the
  // camera")
  XMVECTOR apex = XMLoadFloat3(&place);
  if (!(length > 0.f))
    return;
  // Its rows right, up and back: it looks down -back (as the camera's)
  XMMATRIX view = XMMatrixRotationQuaternion(XMQuaternionNormalize(XMLoadFloat4(&rotation)));
  XMVECTOR right = view.r[0], up = view.r[1], ahead = -view.r[2];
  // Its picture's edges at that distance: the field of view's (vertical) and
  // the screen's shape
  float halfHeight = tanf(XMConvertToRadians(Clamp(fieldOfView, 5.f, 170.f)) * 0.5f) * length;
  float halfWidth = halfHeight * m_ViewAspect;
  XMVECTOR middle = apex + ahead * length;
  XMVECTOR corners[4] = { middle - right * halfWidth + up * halfHeight, middle + right * halfWidth + up * halfHeight,
    middle + right * halfWidth - up * halfHeight, middle - right * halfWidth - up * halfHeight };
  auto project = [&](XMVECTOR point, ImVec2& at)
  {
    XMFLOAT3 p;
    XMStoreFloat3(&p, point);
    bool onScreen = false;
    return ProjectPoint(p, at.x, at.y, onScreen);
  };
  const ImU32 shadow = IM_COL32(0, 0, 0, 110);
  // Not with the camera right at it (nearer than twice its size): it would
  // only fill the picture, its near side sweeping across it with every move
  // (2026-11-09, the user: "they move again with the camera ... the camera
  // things are still huge", with 2 m outlines a few metres away)
  XMMATRIX camera = XMLoadFloat4x4(&m_Camera.Transform);
  if (XMVectorGetX(XMVector3Length(apex - camera.r[3])) < 2.f * length)
    return;
  // A line in the world, the part of it at least half a metre in front of the
  // camera (nearer, its ends would land far off the picture), so an outline
  // whose place is behind the camera still shows what's ahead of it
  auto depthOf = [&](XMVECTOR point) { return -XMVectorGetX(XMVector3Dot(point - camera.r[3], camera.r[2])); };
  const float cutDepth = 0.5f;
  auto line = [&](XMVECTOR p, XMVECTOR q)
  {
    float dp = depthOf(p), dq = depthOf(q);
    if (dp < cutDepth && dq < cutDepth)
      return;
    if (dp < cutDepth)
      p = p + (q - p) * Clamp((cutDepth - dp) / (dq - dp), 0.f, 1.f);
    else if (dq < cutDepth)
      q = q + (p - q) * Clamp((cutDepth - dq) / (dp - dq), 0.f, 1.f);
    ImVec2 a, b;
    if (!project(p, a) || !project(q, b))
      return;
    pDrawList->AddLine(a, b, shadow, thickness + 2.f);
    pDrawList->AddLine(a, b, colour, thickness);
  };
  for (int k = 0; k < 4; ++k)
  {
    line(apex, corners[k]);
    line(corners[k], corners[(k + 1) & 3]);
  }
  // Which way is up in its picture: a small triangle over its top edge (when
  // all of it is in front of the camera)
  ImVec2 t0, t1, t2;
  if (project(middle + up * (halfHeight * 1.45f), t0) &&
      project(middle - right * (halfWidth * 0.3f) + up * (halfHeight * 1.05f), t1) &&
      project(middle + right * (halfWidth * 0.3f) + up * (halfHeight * 1.05f), t2))
    pDrawList->AddTriangleFilled(t0, t1, t2, colour);
  // Where it looks, on from the outline's middle (aimLength times as far)
  if (aimLength > 1.f)
    line(middle, apex + ahead * (length * aimLength));
}

bool CameraManager::PathDragPart(XMFLOAT3& place)
{
  std::vector<CatmullRomNode> const& nodes = m_TrackPlayer.GetNodes();
  if (m_PathDragKeyframe < 0 || m_PathDragKeyframe >= (int)nodes.size())
    return false;
  XMFLOAT3 const* focus = m_FocusOk ? &m_FocusPoint : nullptr;
  if (m_PathDragPart == 3)
  {
    float yaw, pitch, distance;
    XMFLOAT3 pivot;
    return m_TrackPlayer.GetOrbitArch(m_PathDragKeyframe, focus, OrbitBaseYaw(), place, yaw, pitch, distance, pivot);
  }
  // (where it's drawn: an orbit keyframe round its point)
  if (m_PathDragPart == 0)
    return m_TrackPlayer.GetKeyframePlace(m_PathDragKeyframe, focus, OrbitBaseYaw(), place);
  XMFLOAT3 in, out;
  bool hasIn, hasOut;
  m_TrackPlayer.GetKeyframeHandles(m_PathDragKeyframe, m_FocusOk, in, out, hasIn, hasOut);
  place = m_PathDragPart == 2 ? out : in;
  return true;
}

bool CameraManager::PathDragPlace(XMFLOAT3 const& base, bool height, float x, float y, XMFLOAT3& place)
{
  // The ways it can go: the camera's right and up (at the same distance in
  // front of it), or only straight up
  XMMATRIX camera = XMLoadFloat4x4(&m_Camera.Transform);
  XMVECTOR axisA = height ? XMVectorSet(0.f, 1.f, 0.f, 0.f) : XMVector3Normalize(camera.r[0]);
  XMVECTOR axisB = XMVector3Normalize(camera.r[1]);
  return PathDragAlong(base, axisA, axisB, !height, x, y, place);
}

bool CameraManager::PathDragAlong(XMFLOAT3 const& base, FXMVECTOR axisA, FXMVECTOR axisB, bool twoAxes, float x, float y,
  XMFLOAT3& place)
{
  XMMATRIX camera = XMLoadFloat4x4(&m_Camera.Transform);
  XMVECTOR origin = XMLoadFloat3(&base);
  float depth = -XMVectorGetX(XMVector3Dot(origin - camera.r[3], camera.r[2]));
  if (depth < 0.1f)
    return false;
  // Newton steps on ProjectPoint itself, so it ends up under the cursor
  // where it's drawn (the Panini projection too): how far on screen a small
  // move goes, then the move that takes it to the cursor. A move is only
  // taken when it brings it nearer (else half of it is tried), so where the
  // cursor is somewhere it can't go (up and down, or the Panini view's
  // edges) it stops as near as it gets instead of flying off.
  float step = depth * 0.001f;
  float a = 0.f, b = 0.f;
  float sx, sy;
  bool onScreen = false;
  if (!ProjectPoint(base, sx, sy, onScreen))
    return false;
  float error = (x - sx) * (x - sx) + (y - sy) * (y - sy);
  for (int i = 0; i < 16 && error > 0.0025f; ++i)
  {
    XMVECTOR v = origin + axisA * a + axisB * b;
    XMFLOAT3 atA, atB;
    XMStoreFloat3(&atA, v + axisA * step);
    XMStoreFloat3(&atB, v + axisB * step);
    float ax, ay, bx = 0.f, by = 0.f;
    if (!ProjectPoint(atA, ax, ay, onScreen) || (twoAxes && !ProjectPoint(atB, bx, by, onScreen)))
      break;
    float ex = x - sx, ey = y - sy;
    float j11 = (ax - sx) / step, j21 = (ay - sy) / step;  // pixels a metre
    float da, db = 0.f;
    if (!twoAxes)
    {
      // One way only: as near the cursor as that line comes
      float jj = j11 * j11 + j21 * j21;
      if (jj < 1e-8f)
        break;
      da = (j11 * ex + j21 * ey) / jj;
    }
    else
    {
      float j12 = (bx - sx) / step, j22 = (by - sy) / step;
      float det = j11 * j22 - j12 * j21;
      if (fabsf(det) < 1e-8f)
        break;
      da = (j22 * ex - j12 * ey) / det;
      db = (j11 * ey - j21 * ex) / det;
    }
    // No more than its distance from the camera at a time
    float length = sqrtf(da * da + db * db);
    if (length > depth)
    {
      da *= depth / length;
      db *= depth / length;
    }
    bool nearer = false;
    for (int halve = 0; halve < 6 && !nearer; ++halve, da *= 0.5f, db *= 0.5f)
    {
      XMFLOAT3 at;
      XMStoreFloat3(&at, v + axisA * da + axisB * db);
      float nx, ny;
      if (!ProjectPoint(at, nx, ny, onScreen))
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
  XMStoreFloat3(&place, origin + axisA * a + axisB * b);
  return true;
}

void CameraManager::EndPathDrag()
{
  if (!m_PathDragActive)
    return;
  m_PathDragActive = false;
  if (m_TrackPlayer.EndPathDrag())
  {
    if (m_PathDragPart == 3)
    {
      std::vector<CatmullRomNode> const& nodes = m_TrackPlayer.GetNodes();
      if (m_PathDragKeyframe >= 0 && m_PathDragKeyframe < (int)nodes.size())
        util::log::Write("Track: the arch from keyframe %d to %d dragged on the drawn path: rising %.1f degrees, %.2f m "
          "further out in the middle", m_PathDragKeyframe + 1, m_PathDragKeyframe + 2,
          XMConvertToDegrees(nodes[m_PathDragKeyframe].OrbitLift), nodes[m_PathDragKeyframe].OrbitBulge);
    }
    else
      util::log::Write("Track: keyframe %d's %s moved on the drawn path", m_PathDragKeyframe + 1,
        m_PathDragPart == 0 ? (m_PathDragOrbit ? "place round its point" : "place") :
        (m_PathDragPart == 1 ? "handle in" : "handle out"));
  }
  m_PathDragOrbit = false;
}

bool CameraManager::NearbyGroupShown(attach::Group group)
{
  switch (group)
  {
  case attach::Group::People: return m_NearbyPeople;
  case attach::Group::Cars: return m_NearbyCars;
  case attach::Group::Drones: return m_NearbyDrones;
  case attach::Group::Boats: return m_NearbyBoats;
  case attach::Group::Trains: return m_NearbyTrains;
  case attach::Group::Aircraft: return m_NearbyAircraft;
  case attach::Group::Cameras: return m_NearbyCameras;
  default: return m_NearbyOther;
  }
}

bool CameraManager::NearbyItemShown(NearbyItem const& item)
{
  // Rows of things that are gone stay (greyed out) until the next list. The
  // filters also apply here, so a change shows before the next list does.
  if (m_NearbyMovingOnly && (item.Gone || !IsNearbyMoving(item, GetTickCount64())))
    return false;
  return NearbyGroupShown(attach::GroupOf(item.Target));
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

  // What's listed, named from the game's own models. A change makes a new
  // list straight away.
  bool filterChanged = false;
  filterChanged |= ImGui::Checkbox("People", &m_NearbyPeople);
  ImGui::ItemTooltip("Characters (KrugerSec and other NPCs) and the crowds, also the people standing still on "
    "balconies and roofs.");
  ImGui::SameLine();
  filterChanged |= ImGui::Checkbox("Cars", &m_NearbyCars);
  ImGui::ItemTooltip("The road traffic: cars, trucks and motorcycles. They're all one swarm and which member is "
    "which model isn't known, so each is listed as road traffic.");
  ImGui::SameLine();
  filterChanged |= ImGui::Checkbox("Drones", &m_NearbyDrones);
  ImGui::ItemTooltip("The flying traffic (news and snooper drones, VTOL helicopters), the drones high above the "
    "city, and mission drones.");
  ImGui::SameLine();
  filterChanged |= ImGui::Checkbox("Boats", &m_NearbyBoats);
  ImGui::ItemTooltip("The boats on the water around the city.");
  ImGui::SameLine();
  filterChanged |= ImGui::Checkbox("Trains", &m_NearbyTrains);
  ImGui::ItemTooltip("The monorail trains, a row for each carriage. Listed wherever they are, also beyond the "
    "range: they go up to about 135 m/s and cross it in seconds.");

  filterChanged |= ImGui::Checkbox("Aircraft", &m_NearbyAircraft);
  ImGui::ItemTooltip("The airplane (about 1.5 km up), the blimps and the parked VTOLs. Listed wherever they "
    "are, also beyond the range.");
  ImGui::SameLine();
  filterChanged |= ImGui::Checkbox("Security cameras", &m_NearbyCameras);
  ImGui::ItemTooltip("The security cameras within the range. They don't move: following one puts the camera "
    "at its lens, looking where it looks.");
  ImGui::SameLine();
  filterChanged |= ImGui::Checkbox("Other", &m_NearbyOther);
  ImGui::ItemTooltip("Lifts, doors and other models that moved in the last 30 seconds, props, and anything "
    "else. The window shutters on building fronts aren't listed.");
  ImGui::SameLine(0.f, fontSize * 1.5f);
  filterChanged |= ImGui::Checkbox("Only moving", &m_NearbyMovingOnly);
  if (filterChanged)
  {
    m_NearbyRescan = true;
    m_NearbyPublishRequested = true;
    configChanged = true;
  }
  ImGui::SameLine(0.f, fontSize * 1.5f);
  configChanged |= ImGui::Checkbox("On screen", &m_NearbyMarkers);
  ImGui::ItemTooltip("Puts a dot on everything in the list (brighter when it's moving) and a ring on the "
    "one the mouse is over. The one being followed has a red ring.");

  // Nearest the camera first, as things move. Not while the mouse is over the
  // list (as of the last frame), so the rows hold still to be clicked; a new
  // list is never made then either.
  if (!m_NearbyListHovered || m_NearbyOrder.size() != m_Nearby.size())
  {
    std::vector<float> distances(m_Nearby.size());
    for (size_t i = 0; i < m_Nearby.size(); ++i)
      distances[i] = m_Nearby[i].Gone ? FLT_MAX :
        XMVectorGetX(XMVector3Length(XMLoadFloat3(&m_Nearby[i].Position) - XMLoadFloat3(&m_Camera.Position)));
    m_NearbyOrder.resize(m_Nearby.size());
    for (size_t i = 0; i < m_NearbyOrder.size(); ++i)
      m_NearbyOrder[i] = (int)i;
    // Ties (and the gone ones, last) in the order they were listed
    std::stable_sort(m_NearbyOrder.begin(), m_NearbyOrder.end(),
      [&](int a, int b) { return distances[a] < distances[b]; });
  }

  int hovered = -1, shown = 0;
  ImGui::BeginChild("##NearbyList", ImVec2(0, fontSize * 15.f), true);
  m_NearbyListHovered = ImGui::IsWindowHovered();
  ImGui::Columns(4, "##NearbyColumns", false);
  ImGui::SetColumnWidth(0, fontSize * 4.5f);
  ImGui::SetColumnWidth(1, fontSize * 7.f);
  ImGui::SetColumnWidth(2, fontSize * 15.f);
  for (int i : m_NearbyOrder)
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
      ImGui::Text("%s #%d", attach::LabelOf(item.Target), item.Target.Agent);
    else
      ImGui::TextUnformatted(attach::LabelOf(item.Target));
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
  char farText[64] = "";
  if (m_NearbyFar)
    sprintf_s(farText, " and %d trains and aircraft further away", m_NearbyFar);
  ImGui::TextDisabled("%d within %.0f m%s%s, %d listed. Click one to follow it, the camera goes next to it.", m_NearbyTotal,
    m_NearbyListedRadius, m_NearbyTotal > (int)m_Nearby.size() - m_NearbyFar ? " (the nearest are listed)" : "", farText, shown);
  if (m_NearbyDropped)
    ImGui::TextDisabled("%d more were left out: too many objects, try a smaller range.", m_NearbyDropped);

  DrawMarkers(hovered);
  return configChanged;
}

bool CameraManager::DrawDistrictList()
{
  float fontSize = ImGui::GetFontSize();
  bool configChanged = false;

  // A copy of what's needed, so the locks aren't held while drawing
  struct Row
  {
    int Index;
    std::string Name, Key;
    int State, Total, Loaded;
    bool HeldBack;
  };
  std::vector<Row> rows;
  AcquireSRWLockShared(&g_DistrictLock);
  for (size_t d = 0; d < g_Districts.size(); ++d)
  {
    Row row{ (int)d, g_Districts[d].Name, g_Districts[d].Key, g_Districts[d].State, 0, 0, false };
    for (DistrictLayer const& layer : g_DistrictLayers)
    {
      if (d < layer.Total.size())
      {
        row.Total += layer.Total[d];
        row.Loaded += layer.Loaded[d];
      }
    }
    if (row.Total > 0)
      rows.push_back(row);
  }
  ReleaseSRWLockShared(&g_DistrictLock);
  // Held back: an Always district's focus that's wanted but not in
  AcquireSRWLockShared(&g_FocusLock);
  for (int k = 0; k < MaxDistrictFoci; ++k)
  {
    int i = FocusDistrictFirst + k;
    if (!g_FocusWanted[i] || g_DistrictFocusPresent[k])
      continue;
    for (Row& row : rows)
      if (row.Index == g_FocusDistrict[i]) row.HeldBack = true;
  }
  ReleaseSRWLockShared(&g_FocusLock);
  std::sort(rows.begin(), rows.end(), [](Row const& a, Row const& b) {
    return a.Total != b.Total ? a.Total > b.Total : a.Name < b.Name;
  });

  ImGui::PushTextWrapPos(0.f);
  ImGui::TextDisabled("The districts the game streams the city by, with the camera on or off. Always keeps one "
    "loaded, within the same limits as the rest. Never unloads it and keeps it from loading, except the zone "
    "Faith is in and the zones next to it (the ground under her).");
  ImGui::PopTextWrapPos();
  // The real Shard, from anywhere
  bool realShard = m_StreamRealShard;
  if (ImGui::Checkbox("The real Shard, from anywhere", &realShard))
  {
    m_StreamRealShard = realShard;
    // Off in the city: the real tower kept out only once the far-away model
    // is back (it may have streamed out, see OnCameraUpdate), or after
    // ShardBackdropWaitMs
    if (!realShard)
      g_ShardInCitySince = GetTickCount64();
    configChanged = true;
    util::log::Write("The real Shard %s", realShard ? "streamed from anywhere: its tower's zone and its top load, and its "
      "far-away model is hidden once they have" : "as the game streams it: from the city its far-away model again");
  }
  if (m_StreamRealShard)
  {
    int tower = g_ShardTowerLoaded;
    ImGui::SameLine();
    if (tower == 1)
      ImGui::TextDisabled(g_ShardTopIn ? "(tower and top loaded)" : "(tower loaded, its top not yet)");
    else
      ImGui::TextDisabled(tower == 0 ? "(loading)" : "(not known yet)");
  }

  if (rows.empty())
  {
    ImGui::TextDisabled("No districts yet: load a level.");
    return configChanged;
  }

  ImGui::Columns(3, "##DistrictColumns", false);
  ImGui::SetColumnWidth(0, fontSize * 15.f);
  ImGui::SetColumnWidth(1, fontSize * 16.f);
  for (Row const& row : rows)
  {
    ImGui::TextUnformatted(row.Name.c_str());
    ImGui::NextColumn();
    static const char* const states[] = { "Normal", "Always", "Never" };
    for (int s = 0; s < 3; ++s)
    {
      if (s) ImGui::SameLine();
      char label[96];
      sprintf_s(label, "%s##District%d", states[s], row.Index);
      if (ImGui::RadioButton(label, row.State == s) && row.State != s)
      {
        AcquireSRWLockExclusive(&g_DistrictLock);
        if (row.Index < (int)g_Districts.size())
          g_Districts[row.Index].State = s;
        bool anyNever = false;
        for (District const& district : g_Districts)
          anyNever |= district.State == District_Never;
        g_AnyNeverDistrict = anyNever;
        ++g_DistrictGeneration;
        ReleaseSRWLockExclusive(&g_DistrictLock);
        configChanged = true;
        util::log::Write("District %s (%s): %s", row.Name.c_str(), row.Key.c_str(), states[s]);
      }
    }
    ImGui::NextColumn();
    if (row.State == District_Always && row.HeldBack)
      ImGui::TextColored(ImVec4(1.f, 0.8f, 0.2f, 1.f), "%d of %d zones loaded, held back", row.Loaded, row.Total);
    else
      ImGui::TextDisabled("%d of %d zones loaded", row.Loaded, row.Total);
    ImGui::NextColumn();
  }
  ImGui::Columns(1);
  if (g_DistrictFociShort)
    ImGui::TextColored(ImVec4(1.f, 0.8f, 0.2f, 1.f), "Too many districts on Always: the last ones don't all fit (%d foci).",
      MaxDistrictFoci);

  if (ImGui::Button("All back to Normal##Districts"))
  {
    AcquireSRWLockExclusive(&g_DistrictLock);
    for (District& district : g_Districts)
      district.State = District_Normal;
    g_DistrictPending.clear();
    g_AnyNeverDistrict = false;
    ++g_DistrictGeneration;
    ReleaseSRWLockExclusive(&g_DistrictLock);
    configChanged = true;
    util::log::Write("Every district back to Normal");
  }

  // Every district in the list to Always or Never. The ones from the config
  // that aren't in this level stay as they were.
  auto setAll = [&](int state) {
    AcquireSRWLockExclusive(&g_DistrictLock);
    for (Row const& row : rows)
      if (row.Index < (int)g_Districts.size())
        g_Districts[row.Index].State = state;
    bool anyNever = false;
    for (District const& district : g_Districts)
      anyNever |= district.State == District_Never;
    g_AnyNeverDistrict = anyNever;
    ++g_DistrictGeneration;
    ReleaseSRWLockExclusive(&g_DistrictLock);
    configChanged = true;
    util::log::Write("Every district in the list to %s", state == District_Always ? "Always" : "Never");
  };
  ImGui::SameLine();
  if (ImGui::Button("All to Always##Districts"))
    setAll(District_Always);
  // As the renderer's draw batches are now (Room for the whole city)
  ImGui::ItemTooltip(util::hooks::BatchSpread() > 0 ?
    "Keeps every district in the list loaded. The renderer's draw batch has room for all of it (Room for the whole "
    "city); at the game's other limits some of it can still be held back." :
    "Keeps every district in the list loaded as far as there's room. Until Room for the whole city is on, the "
    "renderer's draw batch holds only part of the city, and the rest is held back.");
  ImGui::SameLine();
  if (ImGui::Button("All to Never##Districts"))
    setAll(District_Never);
  ImGui::ItemTooltip("Unloads every district in the list and keeps them from loading, except the zone Faith is in "
    "and the zones next to it.");
  return configChanged;
}

// The vistas' list: taken out of the menu (2026-10-25), GAME > Detail has
// only Show the vistas now
#if 0
bool CameraManager::DrawVistaList()
{
  float fontSize = ImGui::GetFontSize();
  ULONGLONG now = GetTickCount64();
  bool configChanged = false;

  // A copy, so the lock isn't held while drawing
  std::vector<Vista> vistas;
  AcquireSRWLockShared(&g_VistaLock);
  for (Vista const& vista : g_Vistas)
    if (now - vista.Seen <= VistaGoneMs)
      vistas.push_back(vista);
  ReleaseSRWLockShared(&g_VistaLock);

  bool allShown = m_VistasShown;
  if (ImGui::Checkbox("Show the vistas", &allShown))
  {
    m_VistasShown = allShown;
    configChanged = true;
    util::log::Write("Vistas %s", allShown ? "shown" : "all hidden");
  }
  ImGui::HelpMarker("Vistas are the low-detail copies of the city's buildings that the game shows further "
    "away, where the real ones aren't loaded. Off hides all of them: past what's loaded the city is empty.");
  bool hideLoaded = m_VistaHideLoaded;
  if (ImGui::Checkbox("Hide a vista where its buildings are loaded", &hideLoaded))
  {
    m_VistaHideLoaded = hideLoaded;
    configChanged = true;
    util::log::Write("Vistas %s where their buildings are loaded", hideLoaded ? "hidden" : "shown as the game wants");
  }
  ImGui::HelpMarker("Each zone of the city's buildings has a vista, shown when a streaming focus is a bit "
    "further away. With the camera far from Faith, the vistas around each of them are where the other has "
    "the real buildings loaded, and both show at once (blocky copies over the buildings). This hides a "
    "zone's vista while its own buildings are loaded.");

  if (vistas.empty())
  {
    ImGui::TextDisabled(IsRenderArenaBuild() ? "No vistas yet: load a level." : "Vistas aren't known in this game build.");
    return configChanged;
  }

  for (size_t v = 0; v < vistas.size(); ++v)
  {
    Vista const& vista = vistas[v];
    int count = (int)vista.Zones.size();
    int shownCount = 0, offCount = 0;
    for (VistaZone const& zone : vista.Zones)
    {
      shownCount += (zone.Flags & VistaShown) && !(zone.Flags & VistaHiddenForGood) && !zone.Off && zone.Objects ? 1 : 0;
      offCount += zone.UserOff ? 1 : 0;
    }
    ImGui::TextDisabled("%s: %d zones, %d shown now", vista.Layer.c_str(), count, shownCount);

    // Nearest the camera first (with it off, nearest the game's own camera,
    // next to Faith); in their own order when the zones aren't known
    float fromX = m_CameraEnabled ? m_Camera.Position.x : vista.FocusX;
    float fromZ = m_CameraEnabled ? m_Camera.Position.z : vista.FocusZ;
    std::vector<int> rows(count);
    std::vector<float> distances(count, 0.f);
    for (int k = 0; k < count; ++k)
    {
      rows[k] = k;
      float dx = vista.Zones[k].X - fromX, dz = vista.Zones[k].Z - fromZ;
      distances[k] = sqrtf(dx * dx + dz * dz);
    }
    if (vista.Matched)
      std::sort(rows.begin(), rows.end(), [&](int a, int b) {
        return distances[a] != distances[b] ? distances[a] < distances[b] : a < b;
      });

    char id[64];
    sprintf_s(id, "##VistaList%d", (int)v);
    ImGui::BeginChild(id, ImVec2(0, fontSize * 14.f), true);
    ImGui::Columns(3, "##VistaColumns", false);
    ImGui::SetColumnWidth(0, fontSize * 10.f);
    ImGui::SetColumnWidth(1, fontSize * 5.f);
    ImGui::TextDisabled("Vista");
    ImGui::NextColumn();
    ImGui::TextDisabled("Distance");
    ImGui::NextColumn();
    ImGui::TextDisabled("State");
    ImGui::NextColumn();
    ImGui::Separator();
    for (int row : rows)
    {
      VistaZone const& zone = vista.Zones[row];
      char label[64];
      if (zone.Name[0])
        sprintf_s(label, "Zone %s##Vista%d_%d", zone.Name, (int)v, row);
      else
        sprintf_s(label, "Zone %d##Vista%d_%d", row, (int)v, row);
      bool value = !zone.UserOff;
      if (!allShown)
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * 0.4f);
      if (ImGui::Checkbox(label, &value))
      {
        AcquireSRWLockExclusive(&g_VistaLock);
        for (Vista& live : g_Vistas)
          if (live.Entity == vista.Entity && live.States == vista.States && row < (int)live.Zones.size())
            live.Zones[row].UserOff = !value;
        ReleaseSRWLockExclusive(&g_VistaLock);
        util::log::Write("%s vista, zone %s (%d): %s", vista.Layer.c_str(), zone.Name, row, value ? "on" : "off");
      }
      if (!allShown)
        ImGui::PopStyleVar();
      ImGui::NextColumn();
      if (vista.Matched)
        ImGui::TextDisabled("%.0f m", distances[row]);
      ImGui::NextColumn();
      bool gameShows = (zone.Flags & VistaShown) && !(zone.Flags & VistaHiddenForGood);
      if (!zone.Objects)
        ImGui::TextDisabled("no buildings");
      else if (zone.Flags & VistaHiddenForGood)
        ImGui::TextDisabled("hidden by the game");
      else if (!gameShows)
        ImGui::TextDisabled(zone.Loaded ? "not shown, buildings loaded" : "not shown");
      else if (!zone.Off)
        ImGui::TextDisabled(zone.Loaded ? "shown, buildings loaded" : "shown");
      else if (zone.UserOff || !allShown)
        ImGui::TextColored(ImVec4(1.f, 0.8f, 0.2f, 1.f), "off");
      else
        ImGui::TextDisabled("hidden: buildings loaded");
      ImGui::NextColumn();
    }
    ImGui::Columns(1);
    ImGui::EndChild();

    ImGui::TextDisabled("%d switched off", offCount);
    ImGui::SameLine();
    sprintf_s(id, "All on##VistaAllOn%d", (int)v);
    bool allOn = ImGui::Button(id);
    ImGui::SameLine();
    sprintf_s(id, "All off##VistaAllOff%d", (int)v);
    bool allOff = ImGui::Button(id);
    if (allOn || allOff)
    {
      AcquireSRWLockExclusive(&g_VistaLock);
      for (Vista& live : g_Vistas)
        if (live.Entity == vista.Entity && live.States == vista.States)
          for (VistaZone& zone : live.Zones)
            zone.UserOff = allOff;
      ReleaseSRWLockExclusive(&g_VistaLock);
      util::log::Write("%s vista: every zone %s", vista.Layer.c_str(), allOff ? "off" : "on");
    }
  }
  return configChanged;
}
#endif

void CameraManager::DrawReplayUI()
{
  std::lock_guard<std::mutex> lock(m_Mutex);
  DrawMoveToReplayedFaith(ImGui::GetFontSize() * 18.f);
  if (m_Replay.DrawUI())
    g_mainHandle->OnConfigChanged();
}

void CameraManager::DrawFollowUI()
{
  std::lock_guard<std::mutex> lock(m_Mutex);
  if (m_IgcsSession.IsActive())
  {
    ImGui::TextWrapped("IGCS DoF is using the camera. End or cancel the session in ReShade to change follow settings.");
    return;
  }
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
    ImGui::TextColored(ImVec4(1.f, 0.42f, 0.42f, 1.f), "Following %s", attach::LabelOf(m_AttachTarget));
    ImGui::SameLine();
    if (m_AttachTarget.Agent >= 0)
      ImGui::TextDisabled("#%d (%s, %.1f m)", m_AttachTarget.Agent, attach::KindOf(m_AttachTarget), m_AttachDistance);
    else
      ImGui::TextDisabled("(%s, %.1f m)", attach::KindOf(m_AttachTarget), m_AttachDistance);
    if (ImGui::Button("Go next to it"))
      m_AttachPlaceRequested = true;
    ImGui::ItemTooltip("Puts the camera behind it and a little above, looking at it (a security camera: at its "
      "lens, looking where it looks).");
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

  configChanged |= ImGui::Checkbox("Put the camera next to what I aim at", &m_AttachMoveCamera);
  ImGui::HelpMarker("When you attach to what the camera looks at, the camera goes behind it and a little "
    "above, looking at it. Untick to keep the camera where it is (it then moves along with the object "
    "from there). Clicking a row in the list below always puts the camera next to it.");
  ImGui::SameLine(0.f, fontSize * 2.f);
  ImGui::PushItemWidth(fontSize * 8.f);
  if (ImGui::SliderFloat("Smoothing", &m_AttachSmoothing, 0.f, 1.f, m_AttachSmoothing > 0.f ? "%.2f" : "off"))
  {
    m_AttachSmoothing = m_AttachSmoothing < 0.f ? 0.f : (m_AttachSmoothing > 1.f ? 1.f : m_AttachSmoothing);
    configChanged = true;
  }
  ImGui::PopItemWidth();
  ImGui::HelpMarker("How softly the camera follows: it moves and turns with a copy of the object that keeps "
    "to the object's speed and turning, so it doesn't fall behind on a straight run or a steady turn, and the "
    "swarm's small steps and a character's wobble don't shake the view. Higher is steadier; when the object "
    "brakes or stops turning sharply, the copy runs on a little and settles back (about half a metre past a "
    "stop from 36 km/h at 0.4). 0 follows every move exactly. Doesn't change Follow Faith.");

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
    "1000 m, trains and aircraft up to 5 km), or a security camera there. The hotkey does the same with the "
    "menu closed, and pressed again on the same object it lets go. If what's followed is destroyed or "
    "despawns, the camera stops following and stays where it is.");
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
  //
  // Each cloth also has its own copy of the radius and the fade distance,
  // taken from the data when the cloth is made (0x142F9DCD0, for the current
  // quality level): a cloth made before the data was raised (at a level
  // load, with the camera off) kept 33 m and 28 m. The game switches it on
  // within that radius of its activation points (0x142FA0D20; the one point
  // is the game's own camera, at Faith's real head), measured from the
  // cloth's box, so with a replay far from her the hair went off past 33 m.
  // That copy is raised as well.
  const DWORD KnownBuildTimeStamp = 0x5762E3B7;
  const __int64 CustomizationComponentVTableRva = 0x1C3C360;
  const __int64 ClothEntityVTableRva = 0x1AD2998;
  const __int64 ClothEntityDataVTableRva = 0x1AD2960;
  const __int64 RigidMeshAssetVTableRva = 0x1BBDAB0;
  const __int64 ClothInstanceVTableRva = 0x1AB6380;  // the EA cloth instance
  const int ComponentCollectionOffset = 0x38;      // soldier -> its components, one every 0x20 bytes
  const int ClothEntityDataOffset = 0x30;          // ClothEntity -> ClothEntityData
  const int ClothEntityInstanceOffset = 0x68;      // ClothEntity -> its EA cloth instance
  const int ClothInstanceRadiusOffset = 0x9C;      // the instance's radius, then its fade distance
  const int ClothActivationRadiusOffset = 0x84;    // 4 floats, then SmoothBlendOutDistance x4
  const int ClothMeshOffset = 0x70;                // ClothEntityData.Mesh
  const int MeshLodScaleOffset = 0x38;             // MeshAsset.LodScale
  const float HairLodScale = 1000.f;               // the 100 m LOD at 100 km

  bool IsKnownBuild()
  {
    IMAGE_DOS_HEADER* pDos = (IMAGE_DOS_HEADER*)g_gameHandle;
    IMAGE_NT_HEADERS* pNt = (IMAGE_NT_HEADERS*)((BYTE*)g_gameHandle + pDos->e_lfanew);
    return pNt->FileHeader.TimeDateStamp == KnownBuildTimeStamp;
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

  // The same, and whether the read was an access violation (caught)
  __int64 ReadQwordOrZero(__int64 address, bool& violation)
  {
    violation = false;
    if (address < 0x10000 || address > 0x7FFFFFFF0000)
      return 0;
    __try
    {
      return *(__int64*)address;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      violation = GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION;
      return 0;
    }
  }

  struct HairCloth
  {
    __int64 Entity;  // the ClothEntity
    __int64 Data;    // its ClothEntityData
  };

  // Many of the component's values aren't pointers, and reading one that points
  // nowhere is an access violation caught (1.5-1.8 microseconds each, measured
  // outside the game; with her body shown, "the game's settings applied", where
  // the hair fix is timed, took 0.05-0.23 ms a frame, 2026-11-13). A
  // value whose read was one is remembered at its offset. While the offset keeps
  // that value, Windows is asked (VirtualQuery) whether its page is still free,
  // reserved or no-access, and only then the value isn't read: the read would be
  // that access violation again. Otherwise it's read as before, so a cloth is
  // found the frame it's readable, as before. The camera hook's thread only.
  struct HairLook
  {
    __int64 Unreadable[0x800 / 8];    // per offset, the value there whose read was an access violation (0: none)
    // CT.log's 10-s line (diagnostics only)
    ULONGLONG LoggedAt;
    unsigned Frames, Violations, Skipped, Queries;
  };
  HairLook g_HairLook{};
  const int NowhereRuns = 16;         // runs of such pages kept during one look

  // Whether reading this address would be an access violation now: its page is
  // free, reserved or no-access. The runs of such pages found in this look are
  // kept in from/to (up to NowhereRuns), so one question covers every value in
  // them.
  bool PointsNowhere(__int64 address, __int64* from, __int64* to, int& runs)
  {
    for (int r = 0; r < runs; ++r)
      if (address >= from[r] && address < to[r])
        return true;
    ++g_HairLook.Queries;
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery((void*)address, &mbi, sizeof(mbi)) != sizeof(mbi))
      return false;
    bool nowhere = mbi.State == MEM_FREE || mbi.State == MEM_RESERVE ||
      (mbi.State == MEM_COMMIT && (mbi.Protect & 0xFF) == PAGE_NOACCESS);
    if (nowhere && runs < NowhereRuns)
    {
      from[runs] = (__int64)mbi.BaseAddress;
      to[runs] = (__int64)mbi.BaseAddress + (__int64)mbi.RegionSize;
      ++runs;
    }
    return nowhere;
  }

  // Each cloth Faith's customization component holds, with its ClothEntityData
  // (two cloths can share one)
  int FindHairCloths(__int64 pSoldier, HairCloth* pOut, int max)
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
      // The values whose read was an access violation (see HairLook)
      HairLook& look = g_HairLook;
      __int64 nowhereFrom[NowhereRuns], nowhereTo[NowhereRuns];
      int nowhereRuns = 0;
      ++look.Frames;
      for (int off = 0; off < 0x800 && count < max; off += 8)
      {
        __int64 pCloth = ReadQwordOrZero(pComponent + off);
        // Still that value, and its page still isn't there: still no cloth
        __int64& unreadable = look.Unreadable[off / 8];
        if (pCloth && pCloth == unreadable && PointsNowhere(pCloth, nowhereFrom, nowhereTo, nowhereRuns))
        {
          ++look.Skipped;
          continue;
        }
        unreadable = 0;
        if (!pCloth || pCloth == pComponent)
          continue;
        bool violation;
        __int64 vtable = ReadQwordOrZero(pCloth, violation);
        if (violation)
        {
          unreadable = pCloth;
          ++look.Violations;
        }
        if (vtable != base + ClothEntityVTableRva)
          continue;
        __int64 pData = ReadQwordOrZero(pCloth + ClothEntityDataOffset);
        if (!pData || ReadQwordOrZero(pData) != base + ClothEntityDataVTableRva)
          continue;
        bool known = false;
        for (int k = 0; k < count; ++k)
          known |= pOut[k].Entity == pCloth;
        if (!known)
          pOut[count++] = { pCloth, pData };
      }
      break;
    }
    return count;
  }

  // Her body no longer shown: the 10-s line below starts again once it is
  void RestartHairLookLine()
  {
    g_HairLook.LoggedAt = 0;
  }

  // Every 10 s while her body is shown, in CT.log (diagnostics only): the
  // component's values that point nowhere, read (each an access violation) and
  // not read (Windows asked instead)
  void LogHairLook()
  {
    HairLook& look = g_HairLook;
    if (!util::DiagnosticsOn())
      return;
    ULONGLONG now = GetTickCount64();
    if (!look.LoggedAt || now - look.LoggedAt >= 10000)
    {
      if (look.LoggedAt && look.Frames)
        util::log::Write("Faith's hair (diagnostic): her customization component looked through in %u frames in %.0f s; "
          "values that point nowhere: %.1f a frame read (an access violation caught each), %.1f a frame not read "
          "(Windows said their memory still isn't there, %.1f questions a frame)", look.Frames,
          (now - look.LoggedAt) / 1000.0, (double)look.Violations / look.Frames, (double)look.Skipped / look.Frames,
          (double)look.Queries / look.Frames);
      look.LoggedAt = now;
      look.Frames = look.Violations = look.Skipped = look.Queries = 0;
    }
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
  static bool s_supported = IsKnownBuild();
  bool wanted = s_supported && m_CameraEnabled && m_ThirdPersonApplied;
  if (!wanted)
    RestartHairLookLine();
  if (!wanted && m_HairSavedCount == 0 && m_HairInstanceCount == 0)
    return;

  __int64 base = (__int64)g_gameHandle;
  if (!wanted)
  {
    // Back to the game's values. The cloth data and the mesh are assets that
    // stay as long as the level does, so they're put back while they still
    // are what they were, even if Faith can't be found (a loading screen).
    // A cloth's own copy only while it's still a cloth instance.
    for (int i = 0; i < m_HairSavedCount; ++i)
    {
      if (ReadQwordOrZero(m_HairData[i]) == base + ClothEntityDataVTableRva)
        WriteFloats(m_HairData[i] + ClothActivationRadiusOffset, m_HairSaved[i], 8);
      if (m_HairMesh[i] && ReadQwordOrZero(m_HairMesh[i]) == base + RigidMeshAssetVTableRva)
        WriteFloats(m_HairMesh[i] + MeshLodScaleOffset, &m_HairLodScale[i], 1);
    }
    for (int i = 0; i < m_HairInstanceCount; ++i)
    {
      if (ReadQwordOrZero(m_HairInstance[i]) == base + ClothInstanceVTableRva)
        WriteFloats(m_HairInstance[i] + ClothInstanceRadiusOffset, m_HairInstanceSaved[i], 2);
    }
    m_HairSavedCount = 0;
    m_HairInstanceCount = 0;
    return;
  }

  HairCloth cloths[MaxHairCloths];
  __int64 pSoldier = GetPlayerSoldier();
  int count = pSoldier ? FindHairCloths(pSoldier, cloths, MaxHairCloths) : 0;
  LogHairLook();

  // The cloth is switched on near the camera; out to 100 km it's always on
  const float wide[8] = { 100000.f, 100000.f, 100000.f, 100000.f, 99000.f, 99000.f, 99000.f, 99000.f };
  const float instanceWide[2] = { wide[0], wide[4] };
  for (int k = 0; k < count; ++k)
  {
    // The cloth's own copy (see above), for each cloth
    __int64 pInstance = ReadQwordOrZero(cloths[k].Entity + ClothEntityInstanceOffset);
    if (pInstance && ReadQwordOrZero(pInstance) == base + ClothInstanceVTableRva)
    {
      int is = -1;
      for (int i = 0; i < m_HairInstanceCount; ++i)
        if (m_HairInstance[i] == pInstance) is = i;
      if (is < 0 && m_HairInstanceCount < MaxHairCloths &&
        ReadFloats(pInstance + ClothInstanceRadiusOffset, m_HairInstanceSaved[m_HairInstanceCount], 2))
      {
        is = m_HairInstanceCount;
        // Never the tools' own values for the game's (33 m and 28 m)
        if (m_HairInstanceSaved[is][0] >= wide[0] * 0.5f)
        {
          m_HairInstanceSaved[is][0] = 33.f;
          m_HairInstanceSaved[is][1] = 28.f;
        }
        m_HairInstance[m_HairInstanceCount++] = pInstance;
        util::log::Write("Faith's hair: the live cloth's own activation radius raised from %.0f m too",
          m_HairInstanceSaved[is][0]);
      }
      if (is >= 0)
        WriteFloats(pInstance + ClothInstanceRadiusOffset, instanceWide, 2);
    }

    __int64 pData = cloths[k].Data;
    int slot = -1;
    for (int i = 0; i < m_HairSavedCount; ++i)
      if (m_HairData[i] == pData) slot = i;
    if (slot < 0 && m_HairSavedCount < MaxHairCloths)
    {
      slot = m_HairSavedCount;
      if (!ReadFloats(pData + ClothActivationRadiusOffset, m_HairSaved[slot], 8))
        continue;
      // Its mesh, only if it really is a mesh asset
      __int64 pMesh = ReadQwordOrZero(pData + ClothMeshOffset);
      m_HairMesh[slot] = pMesh && ReadQwordOrZero(pMesh) == base + RigidMeshAssetVTableRva &&
        ReadFloats(pMesh + MeshLodScaleOffset, &m_HairLodScale[slot], 1) ? pMesh : 0;
      // Never take the tools' own values for the game's (the game's are 33 m,
      // 28 m and a detail scale of 1)
      if (m_HairSaved[slot][0] >= wide[0] * 0.5f)
        for (int f = 0; f < 8; ++f)
          m_HairSaved[slot][f] = f < 4 ? 33.f : 28.f;
      if (m_HairMesh[slot] && m_HairLodScale[slot] >= HairLodScale * 0.5f)
        m_HairLodScale[slot] = 1.f;
      m_HairData[m_HairSavedCount++] = pData;
      util::log::Write("Faith's hair: cloth activation radius raised from %.0f m%s while her body is shown",
        m_HairSaved[slot][0], m_HairMesh[slot] ? " and its mesh's detail range" : "");
    }
    if (slot < 0)
      continue;
    WriteFloats(pData + ClothActivationRadiusOffset, wide, 8);
    if (m_HairMesh[slot])
      WriteFloats(m_HairMesh[slot] + MeshLodScaleOffset, &HairLodScale, 1);
  }
}

void CameraManager::SwitchBatchSpread(int chunks, const char* when)
{
  util::hooks::SetBatchSpread(chunks);
  int spread = util::hooks::BatchSpread();
  if (chunks > 0 && spread <= 0)
  {
    m_BatchSpreadMissing = true;
    util::log::Warning("Room for the whole city: the renderer's add isn't hooked, its draw batches can't be spread "
      "(one a category, as the game has it)");
  }
  else if (spread > 0)
    util::log::Write("Room for the whole city on (%s): the renderer's first category of draw batches goes on into its %d "
      "unused batches past %d chunks each, room for %d instances", when, MaxBatches - 1, spread, CategoryRoom(0));
  else
    util::log::Write("Room for the whole city off (%s): one draw batch a category as the game has it, room for %d instances",
      when, CategoryRoom(0));
}

void CameraManager::WatchInstanceLists()
{
  InstanceLists lists{};
  if (!ReadInstanceLists(m_ppMeshStreaming, lists))
  {
    m_InstancePressure = 0;
    m_InstanceFill = -1.f;
    g_LimitUse = 0.f;
    return;
  }

  float fill = 0.f;
  for (int k = 0; k < 2; ++k)
  {
    int inUse = lists.Count[k] - lists.Free[k];
    if (lists.Capacity[k] > 0)
    {
      float f = (float)inUse / lists.Capacity[k];
      if (f > fill) fill = f;
    }
    m_InstanceInUse[k] = inUse;

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
  // first) by records in use, close to its room
  int listPressure = fill >= 0.95f ? 3 : (fill >= 0.9f ? 2 : (fill >= 0.8f ? 1 : 0));
  // Its end past its room means the game is already writing over what's next
  bool overflowing = !lists.MeshesInRoom || lists.Meshes > MeshCapacity();
  int meshPressure = (overflowing || inUse >= MeshesTakeAll()) ? 3 :
    (inUse >= MeshesTakeAway() ? 2 : (inUse >= MeshesStopAdding() ? 1 : 0));
  float meshFill = (float)inUse / MeshCapacity();
  float listFill = fill;
  if (meshFill > fill) fill = meshFill;
  if (overflowing && !m_MeshesOverflowed)
    util::log::Warning("The game's mesh table went past its %d records (%d): it writes over its model instance lists, it can crash after this",
      MeshCapacity(), lists.Meshes);
  m_MeshesOverflowed = overflowing;
  // Meshes are arriving while the count keeps going up by more than a few;
  // drops and small changes (the game's own streaming) don't hold anything back
  if (inUse > g_MeshesMark + MeshesBurst)
  {
    g_MeshesMark = inUse;
    g_MeshesChangedAt = now;
  }
  else if (inUse < g_MeshesMark)
    g_MeshesMark = inUse;
  g_MeshesNow = inUse;
  m_MeshCount = inUse;
  m_MeshHighWater = lists.Meshes;
  if (inUse > m_MeshPeak)
    m_MeshPeak = inUse;

  m_InstanceFill = fill;
  // 1 hold back, 2 take two away, 3 at the edge of the room the game has:
  // everything extra goes, the camera's own zones too
  m_MeshPressure = meshPressure;
  m_ListPressure = listPressure;

  // The renderer's instance arena (see RenderArenaRva)
  static bool s_arenaBuild = IsRenderArenaBuild();
  int arenaPressure = 0;
  __int64 arenaUsed = 0, arenaSize = 0;
  if (s_arenaBuild && ReadRenderArena(arenaUsed, arenaSize))
  {
    float arenaFill = (float)arenaUsed / (float)arenaSize;
    arenaPressure = arenaFill >= ArenaTakeAll ? 3 : (arenaFill >= ArenaTakeAway ? 2 : (arenaFill >= ArenaStopAdding ? 1 : 0));
    m_ArenaUsed = arenaUsed;
    m_ArenaSize = arenaSize;
    // Not while unrestricted (nothing is held back), so turning it off at a
    // high fill still logs why zones go
    if (!m_StreamUnrestricted)
    {
      if (arenaPressure > m_ArenaPressureLogged)
        util::log::Warning("The renderer's memory for mesh instances is %.1f of %d MB used: the camera's extra zones are %s",
          arenaUsed / 1048576.0, (int)(arenaSize >> 20),
          arenaPressure >= 3 ? "all taken away" : (arenaPressure == 2 ? "taken away" : "held back"));
      m_ArenaPressureLogged = arenaPressure;
    }
  }
  m_ArenaPressure = arenaPressure;

  // The renderer's fullest draw batch (see RendererOwnersRva), ten times a
  // second (it adds up up to a few thousand bytes per batch)
  if (s_arenaBuild && now - m_BatchScanTick >= 100)
  {
    BatchReading reading;
    int read = ReadFullestBatch(reading);
    if (read > 0)
    {
      m_BatchInstances = reading.Instances;
      m_BatchRoom = reading.Room;
      m_BatchFill = reading.Fill;
      m_BatchChunks = reading.Chunks;
      m_BatchMostChunks = reading.MostChunks;
      m_BatchArriving = reading.MostInstances;
      m_BatchCrashFill = reading.CrashFill;
      m_RendererInstances = reading.RendererInstances;
      m_RendererRoom = reading.RendererRoom;
      // Past its room the game has written over the memory after the batch,
      // and it can crash from here on (until a level load builds a new renderer)
      bool overflowed = reading.Overflowed;
      if (overflowed && !m_BatchOverflowed)
        util::log::Warning("The renderer's draw batch went past its room (one batch at %d chunks; its room is %d, or %d while "
          "the next batch is in use): the game can crash from here on, restart it",
          reading.MostChunks, BatchSafeChunks(), BatchFillRoomChunks);
      m_BatchOverflowed = overflowed;
    }
    else if (read == 0)
    {
      // No renderer: no level
      m_BatchInstances = m_BatchChunks = m_BatchMostChunks = 0;
      m_BatchRoom = reading.Room;
      m_BatchFill = 0.f;
      m_BatchArriving = 0;
      m_BatchCrashFill = 0.f;
      m_RendererInstances = m_RendererRoom = 0;
      m_BatchOverflowed = false;
    }
    // A read that failed halfway keeps the last numbers (as 0 it let everything back in)

    // The foci held out for room (see FocusHold) were for the last level:
    // forgotten when a scan finds no renderer object, or other ones, or
    // category 0's first batch with fewer chunks or batches than at the last
    // scan (a renderer made again: the research only saw them grow while one
    // lives). Only a guess at a level load (no hook on the renderer's
    // constructor or teardown shows one), which is all the holds need.
    if (read >= 0 && !reading.Partial)
    {
      static_assert(sizeof(m_BatchRenderers) / sizeof(m_BatchRenderers[0]) == RendererOwnerCount, "one for each renderer owner");
      bool levelLoad = read == 0;
      for (int o = 0; o < RendererOwnerCount; ++o)
        levelLoad |= reading.Renderers[o] != m_BatchRenderers[o];
      levelLoad |= reading.FirstChunks < m_BatchFirstChunks || reading.FirstBatches < m_BatchFirstBatches;
      if (levelLoad)
        ForgetAllHolds();
      for (int o = 0; o < RendererOwnerCount; ++o)
        m_BatchRenderers[o] = reading.Renderers[o];
      m_BatchFirstChunks = reading.FirstChunks;
      m_BatchFirstBatches = reading.FirstBatches;
    }

    // GAME > Level streaming > Room for the whole city (the batch spread),
    // changed in the menu (or by the config while it couldn't be switched):
    // switched when a scan finds no renderer object, or every one's
    // category 0 still in its first batch and well short of where the
    // picking changes (BatchSpreadSwitchSafe). Not otherwise: the add's hook
    // (hBatchAdd, PickBatch) keeps a first batch that's already past its fill
    // bytes' room (1748 chunks), so switched on with more than that in it, it
    // would run on to the 2048-chunk crash while the tools count the spread's
    // room; and switched off while category 0's other batches are in use, its
    // first batch would grow past 1748 again, its fill bytes over the second
    // batch's start. Whether a level load starts the renderer's batches over
    // isn't known (no hook on its constructor or teardown); a renderer made
    // between the scan and the switch starts with its batches empty (its
    // constructor zeroes them). Not before the config has been read (the
    // camera update runs before the tools' set-up has read it and hooked
    // the add).
    if (read >= 0 && m_BatchSpreadConfigRead && !m_BatchSpreadMissing && m_BatchSpreadWanted != util::hooks::BatchSpread())
    {
      int from = util::hooks::BatchSpread(), to = m_BatchSpreadWanted;
      if (read == 0)
        SwitchBatchSpread(to, "with no level loaded");
      else if (BatchSpreadSwitchSafe(reading, from, to))
        SwitchBatchSpread(to, "with the renderer's first draw batch still well short of it");
    }
    m_BatchScanTick = now;
  }
  // The fullest category for its own room (see CategoryRoom), or the
  // renderer's instance list if that's fuller (with the batches spread
  // category 0's room is far more than the list's)
  float batchFill = m_BatchFill;
  m_BatchListFuller = m_RendererRoom > 0 && (float)m_RendererInstances / (float)m_RendererRoom > batchFill;
  if (m_RendererRoom > 0 && (float)m_RendererInstances / (float)m_RendererRoom > batchFill)
    batchFill = (float)m_RendererInstances / (float)m_RendererRoom;
  // Arriving zones: the most instances in any one category
  int batchNow = m_BatchArriving;
  if (batchNow > g_BatchMark + BatchBurst)
  {
    g_BatchMark = batchNow;
    g_BatchChangedAt = now;
  }
  else if (batchNow < g_BatchMark)
    g_BatchMark = batchNow;
  g_BatchFillNow = batchFill;
  // After it has run past its room, no higher level than the default
  float batchLimit = m_BatchLimit;
  if (m_BatchOverflowed && batchLimit > BatchLimitDefault)
    batchLimit = BatchLimitDefault;
  g_BatchSettleFrom = batchLimit - BatchSettleBelow;
  int batchPressure = batchFill >= BatchTakeAllFrom(batchLimit) ? 3 :
    (batchFill >= BatchTakeAwayFrom(batchLimit) ? 2 : (batchFill >= batchLimit ? 1 : 0));
  // Any one batch near the end of its own chunk pointers (BatchSafeChunks,
  // where the game crashes), whatever its category's room says: taken away
  // from the same top levels, so that acts before it gets there and not only
  // afterwards (the overflow above). A spread batch is full at the spread's
  // chunks, at most 1748 of its 2048 (85 %), so only from the take-away level.
  float crashFill = m_BatchCrashFill;
  int crashPressure = crashFill >= BatchTakeAllFrom(BatchLimitFixed) ? 3 :
    (crashFill >= BatchTakeAwayFrom(BatchLimitFixed) ? 2 : 0);
  if (crashPressure > batchPressure)
    batchPressure = crashPressure;
  // Logged the first time it goes over a level (since it was last well back
  // under the first: it goes up and down as zones load and unload); while it
  // goes on going over them again, a summary at most every
  // BatchLogSummaryMs instead of a warning each time (a tester's log had 80
  // of them in 10 minutes, in pairs)
  bool rise = batchPressure > m_BatchPressureLogged && !m_StreamUnrestricted;
  if (rise)
    m_BatchPressureLogged = batchPressure;
  else if (batchFill < batchLimit - 0.1f)
    m_BatchPressureLogged = 0;
  if (rise && !m_BatchLogTick)
  {
    if (m_BatchListFuller)
      util::log::Warning("The renderer's instance list has %d of its %d (all its draw batches; the fullest category %d of its "
        "%d instances, %d chunks, at most %d of %d in one batch): the camera's extra zones are %s",
        (int)m_RendererInstances, (int)m_RendererRoom, (int)m_BatchInstances, (int)m_BatchRoom, (int)m_BatchChunks,
        (int)m_BatchMostChunks, BatchSafeChunks(),
        batchPressure >= 3 ? "all taken away" : (batchPressure == 2 ? "taken away" : "held back"));
    else
      util::log::Warning("The renderer's fullest draw batch has %d of its %d instances (%d chunks, at most %d of %d in one batch), "
        "its instance list %d of %d: the camera's extra zones are %s",
        (int)m_BatchInstances, (int)m_BatchRoom, (int)m_BatchChunks, (int)m_BatchMostChunks, BatchSafeChunks(),
        (int)m_RendererInstances, (int)m_RendererRoom,
        batchPressure >= 3 ? "all taken away" : (batchPressure == 2 ? "taken away" : "held back"));
    m_BatchLogTick = now;
    m_BatchLogRises = 0;
    m_BatchLogHighest = 0;
    m_BatchLogLowest = m_BatchLogFullest = batchFill;
    m_BatchLogTakeAways = g_TakeAways;
  }
  else if (m_BatchLogTick)
  {
    if (rise)
    {
      ++m_BatchLogRises;
      if (batchPressure > m_BatchLogHighest)
        m_BatchLogHighest = batchPressure;
    }
    if (batchFill < m_BatchLogLowest) m_BatchLogLowest = batchFill;
    if (batchFill > m_BatchLogFullest) m_BatchLogFullest = batchFill;
    if (now - m_BatchLogTick >= BatchLogSummaryMs)
    {
      int takeAways = g_TakeAways - m_BatchLogTakeAways;
      if (m_BatchLogRises > 0 || takeAways > 0)
      {
        util::log::Warning("The renderer's draw batches in the last %d s: the camera's extra zones were held back or taken away "
          "%d more times (at most: %s), %d of the tools' zone points taken away for room (at any of the limits; %d held out "
          "now), between %.0f %% and %.0f %% full (the fuller of the fullest category and the instance list), now %d of %d "
          "instances (%s)",
          (int)((now - m_BatchLogTick) / 1000), m_BatchLogRises,
          m_BatchLogHighest >= 3 ? "all taken away" : (m_BatchLogHighest == 2 ? "taken away" : (m_BatchLogHighest == 1 ? "held back" : "none")),
          takeAways, CountHeldOut(), m_BatchLogLowest * 100.f, m_BatchLogFullest * 100.f,
          m_BatchListFuller ? (int)m_RendererInstances : (int)m_BatchInstances, m_BatchListFuller ? (int)m_RendererRoom : (int)m_BatchRoom,
          m_BatchListFuller ? "the renderer's instance list, all its batches" : "the fullest category's draw batches");
        m_BatchLogTick = now;
        m_BatchLogRises = 0;
        m_BatchLogHighest = 0;
        m_BatchLogLowest = m_BatchLogFullest = batchFill;
        m_BatchLogTakeAways = g_TakeAways;
      }
      else
        m_BatchLogTick = 0; // quiet that long: the next time is a warning again
    }
  }
  m_BatchPressure = batchPressure;
  if (batchPressure > arenaPressure)
    arenaPressure = batchPressure;

  // How close the fullest limit is to where the tools hold back (1: at its
  // first level), for the foci held out (see FocusHold): the draw batch (or
  // the renderer's instance list), the meshes and the model instance lists.
  // Not the renderer's instance memory: its use doesn't go down within a
  // level, so past the release level every focus held out would stay out for
  // the rest of it (its own pressure holds every add back from its 70 % on
  // anyway, and a focus held out comes back only with no pressure at all)
  float limitUse = batchLimit > 0.f ? batchFill / batchLimit : 0.f;
  float meshUse = (float)inUse / (float)MeshesStopAdding();
  if (meshUse > limitUse)
    limitUse = meshUse;
  float listUse = listFill / 0.8f;   // its first level (listPressure, above)
  if (listUse > limitUse)
    limitUse = listUse;
  g_LimitUse = limitUse;

  // Unrestricted: the renderer's two limits are only shown, the camera's
  // zones load past them (the user's call, it can crash the game)
  if (m_StreamUnrestricted)
    arenaPressure = 0;
  int pressure = meshPressure > listPressure ? meshPressure : listPressure;
  m_InstancePressure = arenaPressure > pressure ? arenaPressure : pressure;

  // Ask for eight times the game's room for model instances from the next
  // level load on (each is 32 bytes, so even a lot of room is only a few MB).
  // Always, so a level that loads with Load the world around the camera off
  // still has the room when it's turned on.
  if (lists.pSettings)
  {
    for (int k = 0; k < 2; ++k)
    {
      if (!m_InstanceReserveOriginal[k] && lists.Reserved[k] > 0)
      {
        m_InstanceReserveOriginal[k] = lists.Reserved[k];
        util::log::Write("Model instance lists (%s): %d of %d in use, the game reserves %d per level",
          k == 0 ? "positioned" : "distanced", lists.Count[k], lists.Capacity[k], lists.Reserved[k]);
      }
      int wanted = m_InstanceReserveOriginal[k] * InstanceReserveFactor;
      if (m_InstanceReserveOriginal[k] > 0 && lists.Reserved[k] < wanted)
      {
        // The game puts its own number back now and then, it's asked again
        // each time but only logged the first time and when it first happens
        RaiseInstanceReserve(lists.pSettings, k, wanted);
        if (m_InstanceReserveAsks[k] == 0)
          util::log::Write("Asked the game for room for %d %s model instances from the next level load",
            wanted, k == 0 ? "positioned" : "distanced");
        else if (m_InstanceReserveAsks[k] == 1)
          util::log::Write("The game put its room for %s model instances back to %d, it's asked for %d again "
            "whenever it does (not logged again)", k == 0 ? "positioned" : "distanced", lists.Reserved[k], wanted);
        if (m_InstanceReserveAsks[k] < 2)
          ++m_InstanceReserveAsks[k];
      }
    }
  }
}

namespace
{
  // The game's settings objects by their TypeInfo, the way the game looks
  // them up (0x142AB21C0): [SettingsRegistryRva] + 0xA8 buckets, + 0xB0 their
  // count, the bucket is the TypeInfo's low 32 bits mod the count, each node
  // { TypeInfo, settings, next }. For PE timestamp 0x5762E3B7 only.
  const __int64 SettingsRegistryRva = 0x2374A88;
  const int SettingsBucketsOffset = 0xA8;
  const int SettingsBucketCountOffset = 0xB0;

  // AntSettings. Characters are animated 30 times a second and each frame is
  // blended between the last two poses. Past ReducedInterpolationDistance
  // (10 m) from the camera only some of it is (the game checks it at every
  // pose, 0x142FD7A10); 0 turns that off, so all of them get the full blend
  // (the game has room for 100 at once, MaxInterpolationSlots, left as it is).
  const __int64 AntSettingsTypeRva = 0x27C2DD0;
  const int ReducedInterpolationDistanceOffset = 0x18;

  // LinearMediaBillboardSettings, read every frame (0x1437E2760). Every
  // billboard video has five versions, LOD 0 (full size) to 4: VP6 at 30 fps,
  // most 1440 x 720, then 720 x 368, 368 x 176, 176 x 96 and 96 x 48 (read
  // from the game's data, Research_Billboards_2026-10-24.md). A billboard
  // gets LOD 0 only with SkipLOD below 0 (the game's data has 0) and when its
  // screen size times LODScreenSurfaceScale (and an overload factor that drops
  // when too many videos play) is at least 0.5; LOD 1 from 0.25, LOD 2 from
  // 0.1, LOD 3 from 0.025, LOD 4 from 0.005, each only above SkipLOD.
  // DownscaleOnOverloadEnable stays on. With the tools' camera on, the sizes
  // are measured from it (see util::hooks::SetBillboardCamera).
  const __int64 BillboardSettingsTypeRva = 0x2847730;
  const int BillboardSkipLodOffset = 0x10;      // int
  const int BillboardSurfaceScaleOffset = 0x18; // float

  // What each Billboard quality sets (0 is the game's own, left alone). A
  // higher one lagged the game twice: Highest (every billboard at full size,
  // removed 2026-10-22) and Maximum (the 4 biggest in view at full size,
  // removed 2026-10-24); a saved one of those is Much sharper now.
  struct BillboardLevel
  {
    const char* Name;
    int SkipLod;
    float SurfaceScale;
  };
  const BillboardLevel BillboardLevels[] = {
    { "Default quality (game quality)", 0, 1.f },
    { "Sharper", 0, 4.f },       // bigger versions 4x sooner, still not the full size
    { "Much sharper", -1, 4.f }, // and the full size allowed
  };
  const int BillboardLevelCount = sizeof(BillboardLevels) / sizeof(BillboardLevels[0]);

  __int64 LookUpGameSettings(__int64 typeRva)
  {
    __int64 base = (__int64)g_gameHandle;
    __int64 type = base + typeRva;
    __try
    {
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

  __int64 FindGameSettings(__int64 typeRva)
  {
    static bool s_known = IsKnownBuild();
    return s_known ? LookUpGameSettings(typeRva) : 0;
  }

  bool ReadInt(__int64 address, int& value)
  {
    __try
    {
      value = *(int*)address;
      return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      return false;
    }
  }

  void WriteInt(__int64 address, int value)
  {
    __try
    {
      *(int*)address = value;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
  }
}

void CameraManager::ApplyAnimationSmoothing()
{
  if (!m_SmoothAnimations && !m_AnimationSaved)
    return;
  __int64 pSettings = FindGameSettings(AntSettingsTypeRva);
  if (!pSettings)
    return;
  __int64 pDistance = pSettings + ReducedInterpolationDistanceOffset;

  if (!m_SmoothAnimations)
  {
    if (pSettings == m_pAnimationSettings)
      WriteFloats(pDistance, &m_AnimationOriginal, 1);
    m_AnimationSaved = false;
    return;
  }

  if (!m_AnimationSaved || pSettings != m_pAnimationSettings)
  {
    float distance;
    if (!ReadFloats(pDistance, &distance, 1))
      return;
    // Never the tools' own 0 for the game's (the game's is 10 m)
    m_AnimationOriginal = distance > 0.f ? distance : 10.f;
    m_pAnimationSettings = pSettings;
    m_AnimationSaved = true;
    util::log::Write("Smooth animations: characters past %.0f m are blended as smoothly as near ones",
      m_AnimationOriginal);
  }
  const float none = 0.f;
  WriteFloats(pDistance, &none, 1);
}

void CameraManager::ApplyBillboardDetail()
{
  bool active = m_BillboardQuality > 0 && m_BillboardQuality < BillboardLevelCount;
  if (!active && !m_BillboardSaved)
    return;
  __int64 pSettings = FindGameSettings(BillboardSettingsTypeRva);
  if (!pSettings)
    return;
  __int64 pSkip = pSettings + BillboardSkipLodOffset;
  __int64 pScale = pSettings + BillboardSurfaceScaleOffset;

  // Back to the game's own
  if (!active)
  {
    if (pSettings == m_pBillboardSettings)
    {
      WriteInt(pSkip, m_BillboardSkipOriginal);
      WriteFloats(pScale, &m_BillboardScaleOriginal, 1);
      util::log::Write("Billboard quality: the game's own again (SkipLOD %d, screen size scale %.1f)",
        m_BillboardSkipOriginal, m_BillboardScaleOriginal);
    }
    m_BillboardSaved = false;
    m_BillboardLoggedQuality = -1;
    return;
  }

  BillboardLevel const& level = BillboardLevels[m_BillboardQuality];
  if (!m_BillboardSaved || pSettings != m_pBillboardSettings)
  {
    int skip;
    float scale;
    if (!ReadInt(pSkip, skip) || !ReadFloats(pScale, &scale, 1))
      return;
    // Never one of the tools' own levels for the game's (the game's are 0 and 1)
    bool ours = false;
    for (int i = 1; i < BillboardLevelCount; ++i)
      ours |= skip == BillboardLevels[i].SkipLod && scale == BillboardLevels[i].SurfaceScale;
    m_BillboardSkipOriginal = ours ? 0 : skip;
    m_BillboardScaleOriginal = ours ? 1.f : scale;
    m_pBillboardSettings = pSettings;
    m_BillboardSaved = true;
    m_BillboardLoggedQuality = -1;
  }
  if (m_BillboardLoggedQuality != m_BillboardQuality)
  {
    m_BillboardLoggedQuality = m_BillboardQuality;
    util::log::Write("Billboard quality: %s (SkipLOD %d, screen size scale %.0f; the game's are %d and %.1f)",
      level.Name, level.SkipLod, level.SurfaceScale, m_BillboardSkipOriginal, m_BillboardScaleOriginal);
  }
  WriteInt(pSkip, level.SkipLod);
  WriteFloats(pScale, &level.SurfaceScale, 1);
}

// The reflection volumes around the camera get the cubemaps first (see
// util::hooks::SetNearReflections) while a district is Always, loaded with
// the camera on or off, always (no setting since 2026-10-25). With only the
// game's own zones its order stays.
void CameraManager::ApplyNearReflections()
{
  bool anyAlways = false;
  AcquireSRWLockShared(&g_DistrictLock);
  for (District const& district : g_Districts)
    anyAlways |= district.State == District_Always;
  ReleaseSRWLockShared(&g_DistrictLock);
  util::hooks::SetNearReflections(anyAlways);
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
  // A level can set a reach of its own, which the game takes instead of
  // WorldRenderSettings' (see util::hooks::SetSunShadowReach): ours goes over that too
  util::hooks::SetSunShadowReach(m_ShadowDistance);
  // Lens flares far from the free camera fade out, always (no setting since
  // 2026-10-25), only with it on: what Faith's own camera sees stays the game's
  util::hooks::SetFarFlareFade(m_CameraEnabled);

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
  // For the crash note: no lock, these are only read. Cut short rather than
  // fail if it's ever longer than the buffer (in a crash handler).
  _snprintf_s(buffer, size, _TRUNCATE, "camera %s, track %s, follow Faith %s, follow object %s 0x%I64X, world around camera %s "
    "(distance %.0f, zone slots %d of %d, meshes in use %d of %d, most %d, table end %d, model instances %d of %d, renderer instance memory %.1f of %d MB, fullest draw batch %d of %d in %d chunks, renderer instances %d of %d, held back from %.0f %%%s, batches spread from %d chunks), Faith's body %s, Fix LOD %s, shadow distance %.0f, small object culling %s %.2f, "
    "camera at %.1f %.1f %.1f",
    m_CameraEnabled ? "on" : "off", m_TrackPlayer.IsPaused() ? "paused" : (m_TrackPlayer.IsPlaying() ? "playing" : "stopped"),
    m_AttachToFaith ? "on" : "off", m_AttachObject ? attach::ClassName(m_AttachTarget.Class) : "none", m_AttachObject ? m_AttachTarget.Entity : 0,
    m_StreamAroundCamera ? "on" : "off", m_StreamDistance, (int)m_StreamSlotsPeak, ZoneSlotCount(), (int)m_MeshCount, MeshCapacity(), (int)m_MeshPeak, (int)m_MeshHighWater, m_InstanceInUse[0], m_InstanceCapacity[0],
    m_ArenaUsed / 1048576.0, (int)(m_ArenaSize >> 20), (int)m_BatchInstances, (int)m_BatchRoom, (int)m_BatchChunks, (int)m_RendererInstances, (int)m_RendererRoom,
    m_BatchLimit * 100.f, m_StreamUnrestricted ? ", UNRESTRICTED" : "", util::hooks::BatchSpread(), m_ThirdPersonApplied ? "switched" : "normal",
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

  if (m_IgcsSession.IsActive())
  {
    separator();
    ImGui::TextColored(active, "IGCS capture active - End / Cancel in ReShade");
  }

  separator();
  if (m_TrackPlayer.IsPlaying())
    ImGui::TextColored(active, "%s %s  %.1f / %.1f s", m_TrackPlayer.IsPaused() ? "Paused" : "Playing",
      m_TrackPlayer.GetTrackName().c_str(), m_TrackPlayer.GetPlaybackTime(), m_TrackPlayer.GetTrackLength());
  else
    ImGui::TextColored(off, "%s: %d keyframe%s", m_TrackPlayer.GetTrackName().c_str(),
      m_TrackPlayer.GetNodeCount(), m_TrackPlayer.GetNodeCount() == 1 ? "" : "s");

  if (IsTimeFrozen())
  {
    separator();
    ImGui::TextColored(on, "Time frozen");
  }
  else if (GameSpeedNow() != 1.f)
  {
    separator();
    ImGui::TextColored(on, "Game speed %.2fx", GameSpeedNow());
  }

  if (timelapse::Running())
  {
    separator();
    ImGui::TextColored(active, "Making a time-lapse");
  }

  char replay[96];
  if (m_Replay.GetStatus(replay, sizeof(replay))[0])
  {
    separator();
    ImGui::TextColored(active, "%s", replay);
  }

  if (m_StreamAroundCamera && m_StreamUnrestricted)
  {
    separator();
    ImGui::TextColored(ImVec4(1.f, 0.35f, 0.35f, 1.f), "UNRESTRICTED streaming (can crash)");
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

    // Where the camera is, every frame (game coordinates, y is up)
    separator();
    ImGui::TextColored(off, "x %.1f  y %.1f  z %.1f", m_Camera.Position.x, m_Camera.Position.y, m_Camera.Position.z);
  }
}

// The GAME tab: the game's clock, speed and HUD, detail and draw distance,
// sun shadows
void CameraManager::DrawGameUI()
{
  std::lock_guard<std::mutex> lock(m_Mutex);
  bool configChanged = false;

  InputSystem* pInput = g_mainHandle->GetInputSystem();
  float fontSize = ImGui::GetFontSize();
  char label[128];

  ImGui::PushItemWidth(fontSize * 13.f);

  // The tab has no headers to open: each part is always shown under its name
  ImGui::SectionHeading("Game speed");
  bool timeFrozen = IsTimeFrozen();
  sprintf_s(label, "Freeze time  (%s)", pInput->GetKeyName(Action::ToggleFreezeTime).c_str());
  if (ImGui::Checkbox(label, &timeFrozen))
    ToggleTimeFreeze();

  if (ImGui::SliderFloat("Game speed", &m_GameSpeed, GameSpeedMin, GameSpeedMax, "%.2fx", 2.f))
  {
    if (m_GameSpeed < GameSpeedMin) m_GameSpeed = GameSpeedMin;
    if (m_GameSpeed > GameSpeedMax) m_GameSpeed = GameSpeedMax;
  }
  ImGui::SameLine();
  if (ImGui::Button("1x##GameSpeed"))
    m_GameSpeed = 1.f;
  float speedNow = GameSpeedNow();
  if (speedNow != m_GameSpeed)
    ImGui::TextDisabled("The camera track sets it now: %.2fx", speedNow);
  if (speedNow > 1.f)
  {
    float fps = ImGui::GetIO().Framerate;
    if (fps > 0.f && fps / GameSimRate < speedNow)
      ImGui::TextDisabled("At %.0f fps the game goes %.2fx at most", fps, fps / GameSimRate);
  }
  configChanged |= ImGui::Checkbox("Replays at this speed", &m_ReplayAtGameSpeed);
  ImGui::HelpMarker("A replay playing goes at the game speed too, so Faith and the people recorded slow down "
    "with the world, and Freeze time holds it. Recording always keeps real time. Untick it to play replays at "
    "the speed they were recorded whatever the game speed.");

  ImGui::SectionHeading("Time of day");
  g_mainHandle->GetVisualsManager()->DrawTimeOfDayUI();

  ImGui::SectionHeading("HUD");
  bool hudVisible = IsHudVisible();
  sprintf_s(label, "Show HUD  (%s)", pInput->GetKeyName(Action::ToggleHUD).c_str());
  if (ImGui::Checkbox(label, &hudVisible))
    ToggleHudVisibility();

  // Default or the tools' own bodycam (Bodycam.cpp)
  ImGui::SectionHeading("First person");
  ImGui::PushID("FirstCameraView");
  configChanged |= bodycam::DrawUI();
  ImGui::PopID();

  ImGui::SectionHeading("Faith");
  // In the menu's order: Runner (the game's own), Prologue, Prologue with gridlink
  static const int outfitOrder[faith_outfit::Choice_Count] = { faith_outfit::Choice_Game,
    faith_outfit::Choice_ReleaseNoGridlink, faith_outfit::Choice_Release };
  const char* outfits[faith_outfit::Choice_Count];
  int shown = 0;
  for (int i = 0; i < faith_outfit::Choice_Count; ++i)
  {
    outfits[i] = faith_outfit::ChoiceName(outfitOrder[i]);
    if (outfitOrder[i] == faith_outfit::GetChoice())
      shown = i;
  }
  if (!faith_outfit::IsAvailable())
    ImGui::TextDisabled("Outfit: not available (see CT.log)");
  else if (m_Replay.IsBusy())
    ImGui::TextDisabled("Outfit: %s (not while a replay records or plays)", outfits[shown]);
  else if (ImGui::Combo("Outfit", &shown, outfits, faith_outfit::Choice_Count))
    faith_outfit::SetChoice(outfitOrder[shown]);
  // Her arm tattoo off (2026-11-08): wherever she'd wear the tattoo outfit,
  // its twin without the tattoo (FaithOutfit.h, SetNoTattoo); a replay's too
  if (!faith_outfit::IsAvailable())
    ImGui::TextDisabled("No arm tattoo: not available (see CT.log)");
  else
  {
    bool noTattoo = faith_outfit::GetNoTattoo();
    if (ImGui::Checkbox("No arm tattoo", &noTattoo))
    {
      faith_outfit::SetNoTattoo(noTattoo);
      configChanged = true;
    }
  }
  // Her accessories (2026-11-08): the MagRope and the bag left out of her
  // outfit, the gridlink added (FaithOutfit.h, SetPart); saved
  if (!faith_outfit::ArePartsAvailable())
    ImGui::TextDisabled("MagRope, bag, gridlink: not available (see CT.log)");
  else
  {
    for (int part = 0; part < faith_outfit::Part_Count; ++part)
    {
      bool on = faith_outfit::GetPart(part);
      if (ImGui::Checkbox(faith_outfit::PartLabel(part), &on))
      {
        faith_outfit::SetPart(part, on);
        configChanged = true;
      }
    }
  }
  // GAME > Faith > Size switched off (the user, 2026-11-03: "ok the fun is over comment out the slider we can keep the files"): its menu, with
  // faith_size::Init (Main.cpp) and faith_size::Update (OnCameraUpdate), is
  // commented out; Camera/FaithSize.cpp and .h stay in the project.
//   if (!faith_size::IsAvailable())
//     ImGui::TextDisabled("Size: not available (see CT.log)");
//   else if (m_Replay.IsBusy())
//     ImGui::TextDisabled("Size: %.2f (steps aside while a replay records or plays)", faith_size::GetSize());
//   else
//   {
//     float faithSize = faith_size::GetSize();
//     if (ImGui::SliderFloat("Size##FaithSize", &faithSize, faith_size::MinSize, faith_size::MaxSize, "%.2f",
//       faith_size::SliderPower))
//       faith_size::SetSize(faithSize);
//     ImGui::SameLine();
//     if (ImGui::Button("1.00##FaithSize"))
//       faith_size::SetSize(1.f);
//     ImGui::HelpMarker("Her model only, bigger or smaller about her feet: her drawn body, and in first person (free "
//       "camera off) the game's camera with her eyes, and her hair (Hair follows size). Her collision and her moves stay "
//       "her normal size: she "
//       "runs, jumps and climbs as far as before, a big Faith goes into walls and ceilings and a small one stops short "
//       "of them, and her hands miss ledges by the difference. 1.00 gives her back exactly as the game has her. While a "
//       "replay records or plays she's her normal size. "
//       "Not saved: she's 1.00 at each start.");
//     if (faith_size::IsHairAvailable())
//     {
//       bool hair = faith_size::GetHair();
//       if (ImGui::Checkbox("Hair follows size", &hair))
//         faith_size::SetHair(hair);
//       ImGui::HelpMarker("While she's sized, her hair is her size too: it hangs from her sized head, and its strands "
//         "are as long, and swing as far, as her size makes them. Moving the slider fast, it swings into place. Unticked, "
//         "her hair keeps its normal size (it stands off a small head). On at each start, not saved.");
//     }
//     if (faith_size::IsCollisionAvailable())
//     {
//       bool collision = faith_size::GetCollision();
//       if (ImGui::Checkbox("Collision follows size", &collision))
//         faith_size::SetCollision(collision);
//       ImGui::HelpMarker("While she's sized, the capsules her movement collides with are her size too: walls and "
//         "ceilings stop her body where it is, a big Faith doesn't fit through normal doors and a small one fits through "
//         "small gaps. Her moves still find ledges and obstacles, and travel, at her normal size. She can get stuck in "
//         "tight places when big: untick it or go back to 1.00. Not saved.");
//     }
//   }

  // Faith, KrugerSec and other people posed bone by bone (PoseEditor.cpp)
  ImGui::SectionHeading("Pose");
  ImGui::PushID("Pose");
  pose_editor::DrawUI();
  ImGui::PopID();

  ImGui::SectionHeading("Faith in the streets");
  if (!ground::IsAvailable())
    ImGui::TextDisabled("Not available (see CT.log)");
  else
  {
    bool solidStreet = m_SolidStreet;
    if (ImGui::Checkbox("Solid street", &solidStreet))
    {
      m_SolidStreet = solidStreet;
      ground::SetSolidStreet(solidStreet);
      configChanged = true;
    }
    ImGui::HelpMarker("Ground level and most props are now solid, and the street's kill "
      "zones spare Faith. To get down there from a roof, use your own tool or a glitch (or a fence ;) ). A long fall still kills faith.");
    // No death from long falls (m_NoFallDeath): taken out of the menu and the
    // config (2026-11-04, the user's list), always off: the game's fall check
    char groundStatus[192];
    ImGui::TextDisabled("%s", ground::GetStatus(groundStatus, sizeof(groundStatus)));
  }

  ImGui::SectionHeading("Detail");
  // Room between the groups of settings
  auto gap = [&]() { ImGui::Dummy(ImVec2(0.f, fontSize * 0.5f)); };
  bool lodFix = m_LodFix;
  if (ImGui::Checkbox("Fix LOD", &lodFix))
  {
    SetLodFix(lodFix);
    configChanged = true;
  }
  // Detail distance (m_LodScale): taken out of the menu and the config
  // (2026-11-01), Fix LOD does the same; 0 is the game's own

  configChanged |= ImGui::Checkbox("Show small objects far away", &m_CullOverride);
  if (m_CullOverride)
    configChanged |= ImGui::SliderFloat("Small object culling", &m_CullScale, 0.f, 5.f, "%.2f  (game 5)");

  configChanged |= ImGui::Checkbox("Smooth animations far away", &m_SmoothAnimations);
  ImGui::HelpMarker("The game animates characters 30 times a second and blends that up to the frame "
    "rate, but fully only within 10 m of the camera. With this on, characters farther away move as "
    "smoothly as near ones (the game does this for up to 100 characters at once).");

  gap();
  const char* billboardNames[BillboardLevelCount];
  for (int i = 0; i < BillboardLevelCount; ++i)
    billboardNames[i] = BillboardLevels[i].Name;
  ImGui::PushItemWidth(fontSize * 14.f);
  configChanged |= ImGui::Combo("Billboard quality", &m_BillboardQuality, billboardNames, BillboardLevelCount);
  ImGui::PopItemWidth();
  ImGui::HelpMarker("Default quality: the game's own, at most 720 x 368, and that only up close.\n"
    "Sharper: at most 720 x 368, from 4 times as far.\n"
    "Much sharper: up to the full size, 1440 x 720.");

  gap();
  float farPlane = m_FarPlane;
  if (ImGui::SliderFloat("Draw distance", &farPlane, 0.f, 100000.f, farPlane > 0 ? "%.0f m" : "game default (25000 m)", 3.f))
  {
    m_FarPlane = farPlane < 1.f ? 0.f : farPlane;
    configChanged = true;
  }
  ImGui::HelpMarker("The camera's far plane. Slide to 0 for the game's own. Ctrl+click a slider to type a value.");

  // The vistas: only on or off here (the list of their zones is #if 0, see
  // DrawVistaList). Hidden where their buildings are loaded stays the
  // config's (HideVistasOverLoadedBuildings).
  bool vistasShown = m_VistasShown;
  if (ImGui::Checkbox("Show the vistas", &vistasShown))
  {
    m_VistasShown = vistasShown;
    configChanged = true;
    util::log::Write("Vistas %s", vistasShown ? "shown" : "all hidden");
  }

  gap();
  bool unbroken = m_UnbrokenLandmarks;
  if (ImGui::Checkbox("Unbroken Shard and Bauble", &unbroken))
  {
    m_UnbrokenLandmarks = unbroken;
    configChanged = true;
    util::log::Write("Unbroken Shard and Bauble %s", unbroken ? "on" : "off");
  }
  bool openLair = m_OpenLairBlinds;
  if (ImGui::Checkbox("Open the lair's blinds (train station)", &openLair))
  {
    m_OpenLairBlinds = openLair;
    configChanged = true;
    util::log::Write("The lair's blinds %s", openLair ? "open: its closed blinds hidden" : "as the game has them");
  }
  bool closeLairWindows = m_CloseLairWindows;
  if (ImGui::Checkbox("Close the lair's open windows", &closeLairWindows))
  {
    m_CloseLairWindows = closeLairWindows;
    configChanged = true;
    util::log::Write("The lair's open windows %s", closeLairWindows ? "shut" : "as the game has them");
  }

  // Kruger's office, the top of the Shard: Open (1) or Closed (2); "as the
  // game has them" (0) isn't a choice since 2026-11-04
  static const char* const blindsNames[] = { "Open", "Closed" };
  int krugerBlinds = m_KrugerBlinds == 2 ? 1 : 0;
  ImGui::PushItemWidth(fontSize * 12.f);
  if (ImGui::Combo("Shard office blinds", &krugerBlinds, blindsNames, 2))
  {
    m_KrugerBlinds = krugerBlinds == 1 ? 2 : 1;
    configChanged = true;
  }
  ImGui::PopItemWidth();

  ImGui::SectionHeading("Level streaming");
  // No culling: the main view draws every loaded mesh (CullingOff.cpp; not tied to the zone streamer)
  configChanged |= culling_off::DrawUI();
  if (m_pAddZoneFocus && m_pRemoveZoneFocus)
  {
    // Room for the whole city: the renderer's draw batches spread (see
    // BatchSpreadDefault), on by default, switched only with no renderer
    // there or with every renderer's category 0 still in its first batch and
    // well short of where the picking changes (BatchSpreadSwitchSafe, see
    // WatchInstanceLists)
    // Always on since 2026-11-14, no tick (a config's 0 counts as on too, see
    // ReadConfig); only what it's doing is shown
    if (m_BatchSpreadMissing)
      ImGui::TextDisabled("Room for the whole city: not with this game build");
    else if (m_BatchSpreadWanted != util::hooks::BatchSpread())
      ImGui::TextDisabled("Room for the whole city: on once the renderer's draw batches allow it");

    bool streamAroundCamera = m_StreamAroundCamera;
    if (ImGui::Checkbox("Load the world around the camera", &streamAroundCamera))
    {
      m_StreamAroundCamera = streamAroundCamera;
      configChanged = true;
      util::log::Write("Loading the world around the camera %s", streamAroundCamera ? "enabled" : "disabled");
    }
    // The renderer's draw batch: how full it is, and whether it ran past
    // its room
    auto drawBatchLines = [&]() {
      // (with the batches spread the renderer's instance list fills first)
      bool listFuller = m_BatchListFuller;
      const char* fullName = listFuller ? "Renderer instances (all its batches)" : "Renderer draw batch";
      int fullNow = listFuller ? (int)m_RendererInstances : (int)m_BatchInstances;
      int fullRoom = listFuller ? (int)m_RendererRoom : (int)m_BatchRoom;
      if (m_BatchPressure >= 1 && m_StreamUnrestricted)
        ImGui::TextColored(ImVec4(1.f, 0.35f, 0.35f, 1.f), "%s: %d of %d instances, nearly full: NOT held back (unrestricted)",
          fullName, fullNow, fullRoom);
      else if (m_BatchPressure >= 1)
        ImGui::TextColored(ImVec4(1.f, 0.8f, 0.2f, 1.f), "%s: %d of %d instances, nearly full: the camera's extra zones are held back",
          fullName, fullNow, fullRoom);
      else
        ImGui::TextDisabled("Renderer draw batch: %d of %d instances (the fullest, %d chunks, at most %d of %d in one batch)",
          (int)m_BatchInstances, (int)m_BatchRoom, (int)m_BatchChunks, (int)m_BatchMostChunks, BatchSafeChunks());
      if (m_RendererRoom > 0)
        ImGui::TextDisabled("Renderer instances: %d of %d (the fullest renderer, all its batches)",
          (int)m_RendererInstances, (int)m_RendererRoom);
      if (m_BatchOverflowed)
      {
        ImGui::PushTextWrapPos(0.f);
        ImGui::TextColored(ImVec4(1.f, 0.35f, 0.35f, 1.f), "The draw batch ran past its room (one batch at %d chunks): "
          "the game can crash from here on. Restart it before going on.", (int)m_BatchMostChunks);
        ImGui::PopTextWrapPos();
      }
    };
    // The districts are shown with it off too while any of them is Always or
    // Never, or the real Shard is streamed from anywhere: those apply with it
    // on or off (they were out of sight then, and couldn't be put back)
    bool districtsSet = AnyDistrictSet() || m_StreamRealShard;
    if (m_StreamAroundCamera || districtsSet)
    {
      ImGui::Indent();
      if (m_StreamAroundCamera)
      {
        float streamDistance = m_StreamDistance;
        float maxDistance = m_StreamInView ? MaxStreamDistanceInView : MaxStreamDistance;
        if (streamDistance > maxDistance) streamDistance = maxDistance;
        if (ImGui::SliderFloat("Stream distance", &streamDistance, 0.f, maxDistance,
          streamDistance > 1.f ? "%.0f m" : "nearby zones only"))
        {
          m_StreamDistance = streamDistance < 10.f ? 0.f : streamDistance;
          configChanged = true;
        }
      }
      else
      {
        ImGui::PushTextWrapPos(0.f);
        ImGui::TextDisabled("Nothing extra loads around the camera, but what's set below (districts on Always or "
          "Never, the real Shard from anywhere) still applies, with the camera on or off.");
        ImGui::PopTextWrapPos();
        if (m_InstanceFill >= 0.f && m_ArenaSize > 0)
          drawBatchLines();
      }
      if (m_StreamAroundCamera && !m_CameraEnabled && districtsSet && m_InstanceFill >= 0.f && m_ArenaSize > 0)
        drawBatchLines();
      if (m_CameraEnabled && m_StreamAroundCamera)
      {
        float fill = m_InstanceFill;
        if (fill >= 0.f)
        {
          if (m_MeshPressure >= 1)
            ImGui::TextColored(ImVec4(1.f, 0.8f, 0.2f, 1.f), "Meshes in use: %d of %d, at the limit: the camera's extra zones are held back",
              (int)m_MeshCount, MeshCapacity());
          else
            ImGui::TextDisabled("Meshes in use: %d of %d (most so far %d)", (int)m_MeshCount, MeshCapacity(), (int)m_MeshPeak);
          if (m_ListPressure >= 1)
            ImGui::TextColored(ImVec4(1.f, 0.8f, 0.2f, 1.f), "Model instances in use: %d of %d, nearly full: the camera's extra zones are held back",
              m_InstanceInUse[0], m_InstanceCapacity[0]);
          else
            ImGui::TextDisabled("Model instances in use: %d of %d", m_InstanceInUse[0], m_InstanceCapacity[0]);
          if (m_ArenaSize > 0)
          {
            float usedMb = (float)(m_ArenaUsed / 1048576.0);
            int sizeMb = (int)(m_ArenaSize >> 20);
            if (m_ArenaPressure >= 1 && m_StreamUnrestricted)
              ImGui::TextColored(ImVec4(1.f, 0.35f, 0.35f, 1.f), "Renderer instance memory: %.1f of %d MB, nearly full: NOT held back (unrestricted)",
                usedMb, sizeMb);
            else if (m_ArenaPressure >= 1)
              ImGui::TextColored(ImVec4(1.f, 0.8f, 0.2f, 1.f), "Renderer instance memory: %.1f of %d MB, nearly full: the camera's extra zones are held back",
                usedMb, sizeMb);
            else
              ImGui::TextDisabled("Renderer instance memory: %.1f of %d MB", usedMb, sizeMb);
            drawBatchLines();
            // Hold back from (m_BatchLimit): taken out of the menu and the
            // config (2026-11-01), always BatchLimitFixed (80 %)
          }
        }
        int slots = m_StreamSlotsPeak;
        if (slots >= ZoneSlotsAddBelow() && GetTickCount64() - m_StreamLimitedTick < 3000)
          ImGui::TextColored(ImVec4(1.f, 0.8f, 0.2f, 1.f), "Zones loaded or loading: %d of %d, near the game's limit: the furthest are held back", slots, ZoneSlotCount());
        else
          ImGui::TextDisabled("Zones loaded or loading: %d of %d", slots, ZoneSlotCount());
      }
      // Unrestricted (m_StreamUnrestricted, past the renderer's limits): taken
      // out of the menu and the config (2026-11-01), always off
      if (m_StreamAroundCamera)
      {
        if (ImGui::Checkbox("Only where the camera looks", &m_StreamInView))
        {
          if (!m_StreamInView && m_StreamDistance > MaxStreamDistance)
            m_StreamDistance = MaxStreamDistance;
          configChanged = true;
        }
        configChanged |= ImGui::Checkbox("Load ahead of the camera", &m_StreamAhead);
        configChanged |= ImGui::Checkbox("Keep the track's path loaded", &m_StreamTrack);
      }
      // The districts, always shown (no fold-out)
      ImGui::SubHeading("Districts");
      configChanged |= DrawDistrictList();
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

  ImGui::SectionHeading("Sun shadows");
  float shadowDistance = m_ShadowDistance;
  if (ImGui::SliderFloat("Shadow distance", &shadowDistance, 0.f, 5000.f,
    shadowDistance > 0 ? "%.0f m" : "game default", 3.f))
  {
    m_ShadowDistance = shadowDistance < 10.f ? 0.f : shadowDistance;
    configChanged = true;
  }

  configChanged |= ImGui::Checkbox("Adjust the shadow layers", &m_ShadowLayersOverride);
  if (m_ShadowLayersOverride)
  {
    configChanged |= ImGui::SliderFloat("Fade between layers", &m_ShadowLayerFade, 0.f, 0.5f, "%.3f  (game 0.03)", 2.f);
    configChanged |= ImGui::SliderFloat("Layer spread", &m_ShadowLayerSpread, 0.f, 1.f, "%.2f  (game 0.88)");
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

  ImGui::PopItemWidth();

  if (configChanged)
    g_mainHandle->OnConfigChanged();
}

void CameraManager::DrawUI()
{
  std::lock_guard<std::mutex> lock(m_Mutex);
  ImGui::Text("IGCS Connector: %s", m_IgcsConnected ? "connected" : "not connected");
  if (m_IgcsSession.IsActive())
  {
    ImGui::TextWrapped("IGCS DoF is using the camera. Camera controls and follow are held until you end or cancel the session in ReShade, including after rendering has finished.");
    return;
  }
  if (m_IgcsConnected)
    ImGui::TextWrapped("For IGCS DoF: enable the camera, stop any camera track, and freeze time before starting in ReShade. Use Stop the clock if the time of day still moves.");
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
  DrawMoveToReplayedFaith(fontSize * 24.f + ImGui::GetStyle().ItemSpacing.x);
  ImGui::TextDisabled("Follow Faith, cars, people and other moving things: FOLLOW tab");

  ImGui::PushItemWidth(fontSize * 13.f);

  // The tab has no headers to open: each part is always shown under its name
  ImGui::SectionHeading("Camera tracks");
  // Keyframes can be ticked Orbit keyframe while there's a focus to go round
  m_TrackPlayer.SetOrbitFocusKnown(m_CameraEnabled && m_FocusOk);
  // (with the preview on the path, a keyframe changed in the list is shown by it, the camera stays)
  m_TrackPlayer.SetPreviewOnPath(m_CameraEnabled && m_PathPreview);
  m_TrackPlayer.DrawUI(m_Camera, m_CameraEnabled);

  configChanged |= ImGui::Checkbox("Show the track's path", &m_ShowTrackPath);
  if (m_ShowTrackPath)
  {
    // Where each keyframe looks is always drawn with the path (no tick since
    // 2026-11-09, m_ShowKeyframeViews always on, see ReadConfig)
    ImGui::Indent();
    if (ImGui::SliderFloat("Outline size", &m_KeyframeViewSize, 0.1f, 5.f, "%.2f m", 2.f))
    {
      m_KeyframeViewSize = Clamp(m_KeyframeViewSize, 0.1f, 5.f);
      configChanged = true;
    }
    ImGui::Unindent();
  }
  configChanged |= ImGui::Checkbox("Preview the track on its path", &m_PathPreview);

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

  // A time-lapse made a frame at a time, the light worked out for each (Timelapse.cpp)
  ImGui::SectionHeading("Time-lapse");
  {
    TimelapseLink link(m_TrackPlayer, m_Camera, m_CameraEnabled, IsTimeFrozen(), m_ShakeStrength > 0.f ||
      m_TrackPlayer.IsShakeLocked(), IsTimeFrozen() ? 0.f : GameSpeedNow() * m_GameTimeScale, IsHudVisible());
    configChanged |= timelapse::DrawUI(link);
  }

  ImGui::SectionHeading("Motion blur");
  configChanged |= ImGui::Checkbox("Control motion blur", &m_MotionBlurOverride);
  if (m_MotionBlurOverride)
  {
    configChanged |= ImGui::Checkbox("Motion blur on", &m_MotionBlurEnable);
    configChanged |= ImGui::SliderFloat("Strength", &m_Camera.MotionBlur, 0.f, 5.f, "%.2f  (game 0.5)", 2.f);
    configChanged |= ImGui::SliderFloat("Max blur length", &m_MotionBlurMax, 0.f, 1.f, "%.3f  (game 0.1)", 2.f);
    ImGui::HelpMarker("The longest a blur streak can get, as a fraction of the screen.");
  }

  ImGui::SectionHeading("Speed and field of view");
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

  ImGui::SectionHeading("While the camera is on");
  configChanged |= ImGui::Checkbox("Start at Faith's camera", &m_AutoReset);
  ImGui::HelpMarker("Each time the camera is turned on it starts where Faith's camera is. "
    "Untick to carry on from where you left it.");

  sprintf_s(label, "Freeze Faith  (%s)", pInput->GetKeyName(Action::ToggleGameInput).c_str());
  configChanged |= ImGui::Checkbox(label, &m_KbmDisabled);
  ImGui::HelpMarker("Keyboard and mouse only move the camera. Untick to move Faith and the camera "
    "at the same time.");

  configChanged |= ImGui::Checkbox("Gamepad moves the camera", &m_GamepadDisabled);
  ImGui::HelpMarker("Sticks, triggers and shoulder buttons move the camera.");

  configChanged |= ImGui::Checkbox("Show Faith's body", &m_ThirdPersonToggle);
  ImGui::HelpMarker("Shows Faith's whole body, with her head, while the camera is on, so she can be "
    "seen. Used the next time the camera is turned on.");
  if (m_ThirdPersonToggle)
  {
    int way = m_BodyWay;
    if (ImGui::Combo("Her body", &way, "The game's third-person body\0Her whole first-person body\0"))
    {
      m_BodyWay = way;
      configChanged = true;
      // At once while it's shown
      if (m_CameraEnabled && m_ThirdPersonApplied)
      {
        SetThirdPersonBody(false);
        SetThirdPersonBody(true);
      }
    }
    ImGui::HelpMarker("The game's third-person body: the game is told she's seen in third person, so it "
      "draws her as it draws a third-person character, and nothing else is changed. Her whole first-person "
      "body: the original tools' way (the game stops taking any camera for a first-person one, and her own "
      "camera is unlinked half a second later so her head shows), with her hair kept. Changes at once while "
      "the camera is on. If her head, hair or shadow looks wrong with one, try the other.");
  }

  // Hear from the camera (m_SoundAtCamera) and World sounds near the camera
  // (m_SpotSounds): always on, no ticks since 2026-11-09 (see ReadConfig)

  configChanged |= DrawCameraMovesUI();

  // Diagnostics: commented out of the menu (2026-11-04, the user's list)
#if 0
  ImGui::SectionHeading("Diagnostics (written to CT.log)");
  // A diagnostic for now (voice lines and music near the camera)
  double soundLogLeft = util::hooks::FaithSoundLogRemaining();
  if (soundLogLeft > 0.0)
    ImGui::TextDisabled("Logging sounds: %.0f s left", soundLogLeft);
  else if (ImGui::Button("Log sounds for 30 s") && !util::hooks::StartFaithSoundLog(30.0))
    util::log::Warning("Sound log: the sound hooks aren't there, nothing can be logged");
  ImGui::HelpMarker("A diagnostic: for 30 seconds the log (CT.log) gets every sound the game makes, every event its "
    "voices and sound entities are given, what started it, and how far it was from Faith and from the free "
    "camera, then every music sound entity there is. It only reads, nothing in the game changes.");

  // A diagnostic for now (the replay and the world)
  double worldLogLeft = util::hooks::WorldLogRemaining();
  if (worldLogLeft > 0.0)
    ImGui::TextDisabled("Logging the world: %.0f s left", worldLogLeft);
  else
  {
    ImGui::TextUnformatted("Log the world for 30 s:");
    static const char* const labels[] = { "Door", "Elevator", "MagRope", "Takedown", "Other" };
    for (int i = 0; i < 5; ++i)
    {
      ImGui::SameLine();
      char id[48];
      sprintf_s(id, "%s##WorldLog", labels[i]);
      if (ImGui::Button(id) && !util::hooks::StartWorldLog(30.0, labels[i]))
        util::log::Warning("World log: the timelines' hooks aren't there, nothing can be logged");
    }
  }
  ImGui::HelpMarker("A diagnostic for the replay: for 30 seconds the log (CT.log) records which of the level's "
    "timelines move and when, the models within 40 m of Faith that move or turn, which of the MagRope line's "
    "fields change, and at the start and the end the characters near Faith and where their poses are. Press "
    "the button for what you're about to do (a door, an elevator, a MagRope swing, a takedown), then do it "
    "within the 30 seconds. It only reads.");
#endif

  // Keys: taken out of the menu (2026-10-25). The Hotkeys window (MISC >
  // Change hotkeys...) lists them all.
#if 0
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
    ImGui::TextDisabled("Camera bookmarks");
    ImGui::NextColumn();
    ImGui::Text("%s - %s go, %s - %s save", pInput->GetKeyName(Action::Camera_GoToBookmark1).c_str(),
      pInput->GetKeyName(Action::Camera_GoToBookmark9).c_str(), pInput->GetKeyName(Action::Camera_SaveBookmark1).c_str(),
      pInput->GetKeyName(Action::Camera_SaveBookmark9).c_str());
    ImGui::NextColumn();
    ImGui::Columns(1);

    if (ImGui::Button("Change hotkeys..."))
      pInput->ShowUI();
  }
#endif

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
  m_BodyWay = pReader->GetInteger("Camera", "ThirdPersonBodyWay", 0) == 1 ? 1 : 0;
  // Hear from the camera, World sounds near the camera: always on, no setting since 2026-11-09
  m_SoundAtCamera = true;
  m_SpotSounds = true;
  m_PlaybackFps = (float)pReader->GetReal("Camera", "PlaybackFps", 0.0);
  // CAMERA > Time-lapse's settings ([Camera] Timelapse*)
  timelapse::ReadConfig(pReader);
  m_PlayerControlDuringPlayback = pReader->GetBoolean("Camera", "FaithControlDuringPlayback", true);
  m_KbmDisabled = pReader->GetBoolean("Camera", "FreezeFaith", true);
  m_GamepadDisabled = pReader->GetBoolean("Camera", "GamepadMovesCamera", true);
  m_Heaviness = Clamp((float)pReader->GetReal("Camera", "Heaviness", 0.0), 0.f, 1.f);
  m_FlyLevel = pReader->GetBoolean("Camera", "FlyLevel", false);
  m_Collision = pReader->GetBoolean("Camera", "StopAtWalls", false);
  m_CollisionRadius = Clamp((float)pReader->GetReal("Camera", "WallDistance", 0.25), 0.05f, 1.f);
  m_TrackCollision = pReader->GetBoolean("Camera", "TracksStopAtWalls", true);
  m_SpeedByHeight = Clamp((float)pReader->GetReal("Camera", "SpeedByHeight", 0.0), 0.f, 1.f);
  m_ShakeStrength = Clamp((float)pReader->GetReal("Camera", "ShakeStrength", 0.0), 0.f, 1.f);
  m_ShakeSpeed = Clamp((float)pReader->GetReal("Camera", "ShakeSpeed", 1.0), 0.25f, 3.f);
  m_AutoFocus = pReader->GetBoolean("Camera", "AutoFocus", false);
  m_ShowTrackPath = pReader->GetBoolean("Camera", "ShowTrackPath", false);
  m_ShowKeyframeViews = true;   // where each keyframe looks: always drawn with the path since 2026-11-09
  // (a new name: the 13:28 build's KeyframeViewSize, 2 m, was far too big)
  m_KeyframeViewSize = Clamp((float)pReader->GetReal("Camera", "KeyframeOutlineSize", 0.3), 0.1f, 5.f);
  m_PathPreview = pReader->GetBoolean("Camera", "PathPreview", false);
  m_TrackPlayer.SetGameSpeedLocked(pReader->GetBoolean("Camera", "TrackSetsGameSpeed", false));
  m_Panini = pReader->GetBoolean("Camera", "Panini", false);
  m_PaniniDistance = Clamp((float)pReader->GetReal("Camera", "PaniniDistance", 1.0), 0.f, 1.f);
  m_PaniniCrop = Clamp((float)pReader->GetReal("Camera", "PaniniCrop", 1.0), 0.f, 1.f);
  m_FocusReplayedFaith = pReader->GetBoolean("Camera", "FocusReplayedFaith", true);
  m_ShakePreset = (int)pReader->GetInteger("Camera", "ShakeKind", 0);
  m_ShakePreset = m_ShakePreset < 0 || m_ShakePreset >= ShakeProfileCount ? 0 : m_ShakePreset;
  m_ImpactStrength = Clamp((float)pReader->GetReal("Camera", "ImpactStrength", 0.5), 0.05f, 1.f);
  m_Letterbox = (int)pReader->GetInteger("Camera", "Letterbox", 0);
  m_Letterbox = m_Letterbox < 0 || m_Letterbox >= LetterboxCount ? 0 : m_Letterbox;
  m_LetterboxOpacity = Clamp((float)pReader->GetReal("Camera", "LetterboxOpacity", 1.0), 0.f, 1.f);
  m_GuideThirds = pReader->GetBoolean("Camera", "GuideThirds", false);
  m_GuideGolden = pReader->GetBoolean("Camera", "GuideGolden", false);
  m_GuideCenter = pReader->GetBoolean("Camera", "GuideCentre", false);
  m_GuideSafe = pReader->GetBoolean("Camera", "GuideSafeAreas", false);
  m_GuidesWhilePlaying = pReader->GetBoolean("Camera", "GuidesWhilePlaying", false);
  for (int i = 0; i < BookmarkCount; ++i)
  {
    // x y z, the rotation's x y z w, the field of view; empty: not set
    CameraBookmark& mark = m_Bookmarks[i];
    mark.Set = false;
    std::string text = pReader->Get("Bookmarks", "Bookmark" + std::to_string(i + 1), "");
    float v[8];
    if (sscanf_s(text.c_str(), "%f %f %f %f %f %f %f %f", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7]) != 8)
      continue;
    bool finite = true;
    for (float f : v)
      finite = finite && std::isfinite(f);
    XMVECTOR rotation = XMVectorSet(v[3], v[4], v[5], v[6]);
    float length = XMVectorGetX(XMVector4Length(rotation));
    if (!finite || length < 0.5f || length > 2.f)
      continue;
    mark.Set = true;
    mark.Position = XMFLOAT3(v[0], v[1], v[2]);
    XMStoreFloat4(&mark.Rotation, XMQuaternionNormalize(rotation));
    mark.FieldOfView = Clamp(v[7], 5.f, 150.f);
  }
  if (m_Camera.MovementSpeed <= 0) m_Camera.MovementSpeed = 3.f;
  if (m_Camera.MouseSensitivity <= 0) m_Camera.MouseSensitivity = 0.0015f;

  m_CullOverride = pReader->GetBoolean("Misc", "SmallObjectsOverride", false);
  m_CullScale = (float)pReader->GetReal("Misc", "SmallObjectCulling", 1.0);
  m_SmoothAnimations = pReader->GetBoolean("Misc", "SmoothAnimations", true);
  // Before the dropdown: Sharper billboards on or off (on, the full size was
  // allowed: Much sharper)
  int oldQuality = pReader->GetBoolean("Misc", "SharperBillboards", true) ? 2 : 0;
  m_BillboardQuality = (int)pReader->GetInteger("Misc", "BillboardQuality", oldQuality);
  // Out of range: the nearest one there
  if (m_BillboardQuality < 0)
    m_BillboardQuality = 0;
  if (m_BillboardQuality >= BillboardLevelCount)
    m_BillboardQuality = BillboardLevelCount - 1;
  m_ShadowDistance = (float)pReader->GetReal("Misc", "ShadowDistance", 0.0);
  if (m_ShadowDistance < 0) m_ShadowDistance = 0;
  m_ShadowLayersOverride = pReader->GetBoolean("Misc", "ShadowLayersOverride", false);
  m_ShadowLayerFade = (float)pReader->GetReal("Misc", "ShadowLayerFade", 0.03);
  m_ShadowLayerSpread = (float)pReader->GetReal("Misc", "ShadowLayerSpread", 0.88);
  m_StreamAroundCamera = pReader->GetBoolean("Misc", "StreamAroundCamera", false);
  m_StreamAhead = pReader->GetBoolean("Misc", "StreamAhead", true);
  m_StreamTrack = pReader->GetBoolean("Misc", "StreamTrack", true);
  m_StreamInView = pReader->GetBoolean("Misc", "StreamInView", false);
  // Unrestricted and Hold back from: no setting since 2026-11-01
  m_StreamUnrestricted = false;
  m_BatchLimit = BatchLimitFixed;
  m_VistasShown = pReader->GetBoolean("Misc", "ShowVistas", true);
  m_Replay.SetLooping(pReader->GetBoolean("Replay", "Loop", false));
  m_Replay.SetWithTrack(pReader->GetBoolean("Replay", "PlayAlongWithTrack", true));
  m_Replay.SetSounds(pReader->GetBoolean("Replay", "Sounds", true));
  m_Replay.SetTimeOfDay(pReader->GetBoolean("Replay", "TimeOfDay", true));
  m_Replay.SetRecordedOutfit(pReader->GetBoolean("Replay", "RecordedOutfit", true));
  m_Replay.SetSaveRecordings(pReader->GetBoolean("Replay", "SaveRecordings", true));
  m_Replay.SetRecordDashes(pReader->GetBoolean("Replay", "RecordDashes", true));
  m_Replay.SetStartAfterLoad(pReader->GetBoolean("Replay", "StartAfterLoadingScreen", true));
  m_Replay.SetSoundTrack(pReader->GetBoolean("Replay", "SoundTrack", true));
  // Always on, no ticks or config keys since 2026-11-04 (the user's list)
  m_Replay.SetModels(true);
  m_Replay.SetModelShapes(true);
  m_Replay.SetGameDraws(true);
  m_Replay.SetWorldDraws(true);
  m_Replay.SetSkinnedTracks(true);
  m_Replay.SetPeople(true);
  m_Replay.SetKeepingPeople(true);
  m_Replay.SetStandIns(true);
  m_Replay.SetKeepWorld(true);
  m_Replay.SetReloadBundles(true);
  m_Replay.SetMissionWorld(true);
  m_ReplayAtGameSpeed = pReader->GetBoolean("Replay", "AtGameSpeed", true);
  {
    AcquireSRWLockExclusive(&g_DistrictLock);
    g_DistrictPending.clear();
    for (int state : { (int)District_Always, (int)District_Never })
    {
      std::string list = pReader->Get("Misc", state == District_Always ? "DistrictsAlways" : "DistrictsNever", "");
      size_t start = 0;
      while (start <= list.size())
      {
        size_t comma = list.find(',', start);
        std::string key = list.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
        key.erase(0, key.find_first_not_of(' '));
        key.erase(key.find_last_not_of(' ') + 1);
        if (!key.empty())
          g_DistrictPending.push_back({ key, state });
        if (comma == std::string::npos)
          break;
        start = comma + 1;
      }
    }
    bool anyNever = false;
    for (District& district : g_Districts)
    {
      district.State = District_Normal;
      for (auto& pending : g_DistrictPending)
        if (pending.first == district.Key) district.State = pending.second;
      anyNever |= district.State == District_Never;
    }
    for (auto& pending : g_DistrictPending)
      anyNever |= pending.second == District_Never;
    g_AnyNeverDistrict = anyNever;
    ++g_DistrictGeneration;
    ReleaseSRWLockExclusive(&g_DistrictLock);
  }
  m_VistaHideLoaded = pReader->GetBoolean("Misc", "HideVistasOverLoadedBuildings", true);
  m_UnbrokenLandmarks = pReader->GetBoolean("Misc", "UnbrokenShardAndBauble", false);
  m_StreamRealShard = pReader->GetBoolean("Misc", "StreamRealShard", false);
  int krugerBlinds = (int)pReader->GetInteger("Misc", "KrugersOfficeBlinds", 0);
  m_KrugerBlinds = krugerBlinds == 2 ? 2 : 1;   // Open or Closed (0, as the game has them, is Open now)
  m_OpenLairBlinds = pReader->GetBoolean("Misc", "OpenLairBlinds", false);
  m_CloseLairWindows = pReader->GetBoolean("Misc", "CloseLairWindows", false);
  faith_outfit::SetNoTattoo(pReader->GetBoolean("Misc", "NoArmTattoo", false));
  for (int part = 0; part < faith_outfit::Part_Count; ++part)
    faith_outfit::SetPart(part, pReader->GetBoolean("Misc", faith_outfit::PartSetting(part), false));
  m_SolidStreet = pReader->GetBoolean("Misc", "SolidStreet", false);
  m_NoFallDeath = false;   // no setting since 2026-11-04
  ground::SetSolidStreet(m_SolidStreet);
  ground::SetNoFallDeath(m_NoFallDeath);
  culling_off::SetEnabled(pReader->GetBoolean("Misc", "NoCulling", false));
  culling_off::SetViewMargin((float)pReader->GetReal("Misc", "NoCullingViewMargin", 20.0));
  culling_off::SetSmallRange((float)pReader->GetReal("Misc", "NoCullingSmallRange", 2000.0));
  bodycam::ReadConfig(pReader);
  m_StreamDistance = (float)pReader->GetReal("Misc", "StreamDistance", 0.0);
  if (m_StreamDistance < 0) m_StreamDistance = 0;
  float maxStreamDistance = m_StreamInView ? MaxStreamDistanceInView : MaxStreamDistance;
  if (m_StreamDistance > maxStreamDistance) m_StreamDistance = maxStreamDistance;
  // GAME > Level streaming > Room for the whole city: category 0 of the
  // renderer's draw batches goes on into its unused batches past this many
  // chunks each (0 off, at most 1748; see hBatchAdd and BatchSpreadDefault).
  // A test setting until 2.0.1 (off by default, not in the menu). Only with
  // the wider handle and the add hooked (Hooks.cpp), otherwise it stays off.
  // Read once, when the tools load (Main::LoadConfig, after the hooks).
  {
    int spread = (int)pReader->GetInteger("Misc", "BatchSpreadChunks", BatchSpreadDefault);
    // Always on since 2026-11-14 (no tick): 0 (off) or less is the default
    if (spread <= 0) spread = BatchSpreadDefault;
    if (spread > BatchFillRoomChunks) spread = BatchFillRoomChunks;
    m_BatchSpreadOn = spread > 0 ? spread : BatchSpreadDefault;
    m_BatchSpreadWanted = spread;
    m_BatchSpreadMissing = !util::offsets::BatchChunkIndexWidened() || !util::hooks::BatchSpreadHooked();
    if (m_BatchSpreadMissing)
    {
      if (spread > 0)
        util::log::Write("Room for the whole city: not with this game build (the renderer's draw batches can't be spread "
          "without the wider handle and the add's hook), one batch as the game has it");
    }
    else
    {
      // Switched now if it can be (see WatchInstanceLists): with no renderer
      // object there, or every one's category 0 still in its first batch
      // and well short of where the picking changes (the game's first level
      // may already be there when the config is read: a tester's 2.0.0 log
      // had its three physics worlds made before it). Otherwise the scan
      // switches it once it can.
      BatchReading reading;
      int read = IsRenderArenaBuild() ? ReadFullestBatch(reading) : 0;
      if (read == 0)
        SwitchBatchSpread(spread, "the config, before any level");
      else if (read > 0 && BatchSpreadSwitchSafe(reading, util::hooks::BatchSpread(), spread))
        SwitchBatchSpread(spread, "the config, the renderer's first draw batch still well short of it");
      else if (spread != util::hooks::BatchSpread() && read > 0)
        util::log::Write("Room for the whole city %s from the config: not yet (the renderer's first draw batch has %d chunks, "
          "%d batches in use), once the draw batches allow it", spread > 0 ? "on" : "off", reading.FirstChunks,
          reading.FirstBatches);
      else if (spread != util::hooks::BatchSpread())
        util::log::Write("Room for the whole city %s from the config: not yet (the renderer's draw batches couldn't be "
          "read), once the draw batches allow it", spread > 0 ? "on" : "off");
      // These renderer objects aren't a level load for the scan
      if (read > 0 && !reading.Partial)
      {
        for (int o = 0; o < RendererOwnerCount; ++o)
          m_BatchRenderers[o] = reading.Renderers[o];
        m_BatchFirstChunks = reading.FirstChunks;
        m_BatchFirstBatches = reading.FirstBatches;
      }
    }
    m_BatchSpreadConfigRead = true;
  }

  m_MotionBlurOverride = pReader->GetBoolean("Misc", "MotionBlurOverride", false);
  m_MotionBlurEnable = pReader->GetBoolean("Misc", "MotionBlurEnable", true);
  m_Camera.MotionBlur = (float)pReader->GetReal("Misc", "MotionBlurStrength", 0.5);
  m_MotionBlurMax = (float)pReader->GetReal("Misc", "MotionBlurMax", 0.1);
  m_FarPlane = (float)pReader->GetReal("Misc", "DrawDistance", 0.0);
  // Detail distance (m_LodScale): no setting since 2026-11-01, the game's own
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
  // Cars were in Traffic before
  m_NearbyCars = pReader->GetBoolean("Follow", "NearbyCars", pReader->GetBoolean("Follow", "NearbyTraffic", true));
  m_NearbyDrones = pReader->GetBoolean("Follow", "NearbyDrones", true);
  m_NearbyBoats = pReader->GetBoolean("Follow", "NearbyBoats", true);
  m_NearbyTrains = pReader->GetBoolean("Follow", "NearbyTrains", true);
  m_NearbyAircraft = pReader->GetBoolean("Follow", "NearbyAircraft", true);
  m_NearbyCameras = pReader->GetBoolean("Follow", "NearbySecurityCameras", true);
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
  config += "ThirdPersonBodyWay = " + std::to_string(m_BodyWay) + "\n";
  config += "PlaybackFps = " + std::to_string(m_PlaybackFps) + "\n";
  timelapse::WriteConfig(config);
  config += std::string("FaithControlDuringPlayback = ") + (m_PlayerControlDuringPlayback ? "true" : "false") + "\n";
  config += std::string("FreezeFaith = ") + (m_KbmDisabled ? "true" : "false") + "\n";
  config += std::string("GamepadMovesCamera = ") + (m_GamepadDisabled ? "true" : "false") + "\n";
  config += "Heaviness = " + std::to_string(m_Heaviness) + "\n";
  config += std::string("FlyLevel = ") + (m_FlyLevel ? "true" : "false") + "\n";
  config += std::string("StopAtWalls = ") + (m_Collision ? "true" : "false") + "\n";
  config += "WallDistance = " + std::to_string(m_CollisionRadius) + "\n";
  config += std::string("TracksStopAtWalls = ") + (m_TrackCollision ? "true" : "false") + "\n";
  config += "SpeedByHeight = " + std::to_string(m_SpeedByHeight) + "\n";
  config += "ShakeStrength = " + std::to_string(m_ShakeStrength) + "\n";
  config += "ShakeSpeed = " + std::to_string(m_ShakeSpeed) + "\n";
  config += std::string("AutoFocus = ") + (m_AutoFocus ? "true" : "false") + "\n";
  config += std::string("ShowTrackPath = ") + (m_ShowTrackPath ? "true" : "false") + "\n";
  config += "KeyframeOutlineSize = " + std::to_string(m_KeyframeViewSize) + "\n";
  config += std::string("PathPreview = ") + (m_PathPreview ? "true" : "false") + "\n";
  config += std::string("TrackSetsGameSpeed = ") + (m_TrackPlayer.IsGameSpeedLocked() ? "true" : "false") + "\n";
  config += std::string("Panini = ") + (m_Panini ? "true" : "false") + "\n";
  config += "PaniniDistance = " + std::to_string(m_PaniniDistance) + "\n";
  config += "PaniniCrop = " + std::to_string(m_PaniniCrop) + "\n";
  config += std::string("FocusReplayedFaith = ") + (m_FocusReplayedFaith ? "true" : "false") + "\n";
  config += "ShakeKind = " + std::to_string(m_ShakePreset) + "\n";
  config += "ImpactStrength = " + std::to_string(m_ImpactStrength) + "\n";
  config += "Letterbox = " + std::to_string(m_Letterbox) + "\n";
  config += "LetterboxOpacity = " + std::to_string(m_LetterboxOpacity) + "\n";
  config += std::string("GuideThirds = ") + (m_GuideThirds ? "true" : "false") + "\n";
  config += std::string("GuideGolden = ") + (m_GuideGolden ? "true" : "false") + "\n";
  config += std::string("GuideCentre = ") + (m_GuideCenter ? "true" : "false") + "\n";
  config += std::string("GuideSafeAreas = ") + (m_GuideSafe ? "true" : "false") + "\n";
  config += std::string("GuidesWhilePlaying = ") + (m_GuidesWhilePlaying ? "true" : "false") + "\n\n";
  config += "[Bookmarks]\n";
  for (int i = 0; i < BookmarkCount; ++i)
  {
    CameraBookmark const& mark = m_Bookmarks[i];
    char line[256] = "";
    if (mark.Set)
      sprintf_s(line, "%.4f %.4f %.4f %.6f %.6f %.6f %.6f %.3f", mark.Position.x, mark.Position.y, mark.Position.z,
        mark.Rotation.x, mark.Rotation.y, mark.Rotation.z, mark.Rotation.w, mark.FieldOfView);
    config += "Bookmark" + std::to_string(i + 1) + " = " + line + "\n";
  }
  config += "\n";
  config += "[Misc]\n";
  config += std::string("FixLOD = ") + (m_LodFix ? "true" : "false") + "\n";
  config += "DrawDistance = " + std::to_string(m_FarPlane) + "\n";
  config += std::string("MotionBlurOverride = ") + (m_MotionBlurOverride ? "true" : "false") + "\n";
  config += std::string("MotionBlurEnable = ") + (m_MotionBlurEnable ? "true" : "false") + "\n";
  config += "MotionBlurStrength = " + std::to_string(m_Camera.MotionBlur) + "\n";
  config += "MotionBlurMax = " + std::to_string(m_MotionBlurMax) + "\n";
  config += std::string("SmallObjectsOverride = ") + (m_CullOverride ? "true" : "false") + "\n";
  config += "SmallObjectCulling = " + std::to_string(m_CullScale) + "\n";
  config += std::string("SmoothAnimations = ") + (m_SmoothAnimations ? "true" : "false") + "\n";
  config += "BillboardQuality = " + std::to_string(m_BillboardQuality) + "\n";
  config += "ShadowDistance = " + std::to_string(m_ShadowDistance) + "\n";
  config += std::string("ShadowLayersOverride = ") + (m_ShadowLayersOverride ? "true" : "false") + "\n";
  config += "ShadowLayerFade = " + std::to_string(m_ShadowLayerFade) + "\n";
  config += "ShadowLayerSpread = " + std::to_string(m_ShadowLayerSpread) + "\n";
  config += std::string("StreamAroundCamera = ") + (m_StreamAroundCamera ? "true" : "false") + "\n";
  config += std::string("StreamAhead = ") + (m_StreamAhead ? "true" : "false") + "\n";
  config += std::string("StreamTrack = ") + (m_StreamTrack ? "true" : "false") + "\n";
  config += std::string("StreamInView = ") + (m_StreamInView ? "true" : "false") + "\n";
  config += std::string("ShowVistas = ") + (m_VistasShown ? "true" : "false") + "\n";
  {
    // The districts seen this time, and the ones from the config that weren't
    std::string lists[3];
    AcquireSRWLockShared(&g_DistrictLock);
    auto add = [&](std::string const& key, int state) {
      if (state == District_Normal) return;
      if (!lists[state].empty()) lists[state] += ",";
      lists[state] += key;
    };
    for (District const& district : g_Districts)
      add(district.Key, district.State);
    for (auto& pending : g_DistrictPending)
    {
      bool seen = false;
      for (District const& district : g_Districts)
        seen |= district.Key == pending.first;
      if (!seen)
        add(pending.first, pending.second);
    }
    ReleaseSRWLockShared(&g_DistrictLock);
    config += "DistrictsAlways = " + lists[District_Always] + "\n";
    config += "DistrictsNever = " + lists[District_Never] + "\n";
  }
  config += std::string("HideVistasOverLoadedBuildings = ") + (m_VistaHideLoaded ? "true" : "false") + "\n";
  config += std::string("UnbrokenShardAndBauble = ") + (m_UnbrokenLandmarks ? "true" : "false") + "\n";
  config += std::string("StreamRealShard = ") + (m_StreamRealShard ? "true" : "false") + "\n";
  config += "KrugersOfficeBlinds = " + std::to_string((int)m_KrugerBlinds) + "\n";
  config += std::string("OpenLairBlinds = ") + (m_OpenLairBlinds ? "true" : "false") + "\n";
  config += std::string("CloseLairWindows = ") + (m_CloseLairWindows ? "true" : "false") + "\n";
  config += std::string("NoArmTattoo = ") + (faith_outfit::GetNoTattoo() ? "true" : "false") + "\n";
  for (int part = 0; part < faith_outfit::Part_Count; ++part)
    config += std::string(faith_outfit::PartSetting(part)) + " = " + (faith_outfit::GetPart(part) ? "true" : "false") + "\n";
  config += std::string("SolidStreet = ") + (m_SolidStreet ? "true" : "false") + "\n";
  config += std::string("NoCulling = ") + (culling_off::GetEnabled() ? "true" : "false") + "\n";
  config += "NoCullingViewMargin = " + std::to_string(culling_off::GetViewMargin()) + "\n";
  config += "NoCullingSmallRange = " + std::to_string(culling_off::GetSmallRange()) + "\n";
  // FirstCameraView and the bodycam's look and settings (Bodycam.cpp)
  bodycam::WriteConfig(config);
  config += "StreamDistance = " + std::to_string(m_StreamDistance) + "\n";
  // Room for the whole city: only when it isn't the default (so the default
  // can change), what was chosen even before it's switched (with no level
  // loaded, at the latest the next game start)
  if (m_BatchSpreadWanted != BatchSpreadDefault)
    config += "BatchSpreadChunks = " + std::to_string((int)m_BatchSpreadWanted) + "\n";
  config += "\n";
  config += "[Replay]\n";
  config += std::string("Loop = ") + (m_Replay.IsLooping() ? "true" : "false") + "\n";
  config += std::string("PlayAlongWithTrack = ") + (m_Replay.IsWithTrack() ? "true" : "false") + "\n";
  config += std::string("Sounds = ") + (m_Replay.IsSounds() ? "true" : "false") + "\n";
  config += std::string("TimeOfDay = ") + (m_Replay.IsTimeOfDay() ? "true" : "false") + "\n";
  config += std::string("RecordedOutfit = ") + (m_Replay.IsRecordedOutfit() ? "true" : "false") + "\n";
  config += std::string("SaveRecordings = ") + (m_Replay.IsSaveRecordings() ? "true" : "false") + "\n";
  config += std::string("RecordDashes = ") + (m_Replay.IsRecordDashes() ? "true" : "false") + "\n";
  config += std::string("StartAfterLoadingScreen = ") + (m_Replay.IsStartAfterLoad() ? "true" : "false") + "\n";
  config += std::string("SoundTrack = ") + (m_Replay.IsSoundTrack() ? "true" : "false") + "\n";
  config += std::string("AtGameSpeed = ") + (m_ReplayAtGameSpeed ? "true" : "false") + "\n\n";
  config += "[Follow]\n";
  config += std::string("TurnWithIt = ") + (m_AttachRotation ? "true" : "false") + "\n";
  config += std::string("TiltWithIt = ") + (m_AttachTilt ? "true" : "false") + "\n";
  config += std::string("PutCameraNextToIt = ") + (m_AttachMoveCamera ? "true" : "false") + "\n";
  config += "Smoothing = " + std::to_string(m_AttachSmoothing) + "\n";
  config += std::string("Crosshair = ") + (m_ShowCrosshair ? "true" : "false") + "\n";
  config += "NearbyRange = " + std::to_string(m_NearbyRadius) + "\n";
  config += std::string("NearbyPeople = ") + (m_NearbyPeople ? "true" : "false") + "\n";
  config += std::string("NearbyCars = ") + (m_NearbyCars ? "true" : "false") + "\n";
  config += std::string("NearbyDrones = ") + (m_NearbyDrones ? "true" : "false") + "\n";
  config += std::string("NearbyBoats = ") + (m_NearbyBoats ? "true" : "false") + "\n";
  config += std::string("NearbyTrains = ") + (m_NearbyTrains ? "true" : "false") + "\n";
  config += std::string("NearbyAircraft = ") + (m_NearbyAircraft ? "true" : "false") + "\n";
  config += std::string("NearbySecurityCameras = ") + (m_NearbyCameras ? "true" : "false") + "\n";
  config += std::string("NearbyOther = ") + (m_NearbyOther ? "true" : "false") + "\n";
  config += std::string("NearbyOnlyMoving = ") + (m_NearbyMovingOnly ? "true" : "false") + "\n";
  config += std::string("NearbyOnScreen = ") + (m_NearbyMarkers ? "true" : "false") + "\n\n";

  return config;
}

XMVECTOR CameraManager::GuardTrackPlace(XMVECTOR wanted, double dt)
{
  if (!m_Collision || !m_TrackCollision)
  {
    m_GuardValid = false;
    return wanted;
  }
  XMFLOAT3 want;
  XMStoreFloat3(&want, wanted);
  // The track's start, a cut or a seek: there at once
  if (!m_GuardValid || XMVectorGetX(XMVector3Length(wanted - XMLoadFloat3(&m_GuardLastTrack))) > GuardJump)
  {
    m_GuardValid = true;
    m_GuardPlace = want;
    m_GuardLastTrack = want;
    m_GuardHeld = 0.0;
    return wanted;
  }
  XMFLOAT3 from = m_GuardPlace, to = want, wall;
  collision::Move(&from.x, &to.x, m_CollisionRadius, &wall.x);
  if (XMVectorGetX(XMVector3Length(XMLoadFloat3(&to) - wanted)) > 0.05f)
  {
    m_GuardHeld += dt;
    // Held at a wall the track went through: once its own way is clear again
    // (it came out the other side), cut to it
    float fraction;
    XMFLOAT3 normal;
    if (m_GuardHeld >= GuardHoldSeconds &&
        !collision::Sweep(&m_GuardLastTrack.x, &want.x, m_CollisionRadius, &fraction, &normal.x))
    {
      to = want;
      m_GuardHeld = 0.0;
    }
  }
  else
    m_GuardHeld = 0.0;
  m_GuardPlace = to;
  m_GuardLastTrack = want;
  return XMLoadFloat3(&to);
}

void CameraManager::StepTrackWallCheck()
{
  std::vector<CatmullRomNode> const& nodes = m_TrackPlayer.GetNodes();
  if (!m_Collision || nodes.size() < 2 || !collision::Available())
  {
    m_WallCheckNodes.clear();
    m_WallCheckRunning = false;
    m_WallSpans.clear();
    m_WallStretches.clear();
    m_WallFixWanted = false;
    m_WallCheckLogOwed = false;
    return;
  }
  if (m_TrackPlayer.IsPlaying())
    return;
  // Changed since it was checked (keyframes, the track, the radius): again
  bool same = nodes.size() == m_WallCheckNodes.size() && m_WallCheckRadius == m_CollisionRadius;
  for (size_t i = 0; same && i < nodes.size(); ++i)
    same = !memcmp(&nodes[i].Position, &m_WallCheckNodes[i].Position, sizeof(XMFLOAT3)) &&
      nodes[i].TimeStamp == m_WallCheckNodes[i].TimeStamp && nodes[i].Interpolation == m_WallCheckNodes[i].Interpolation &&
      nodes[i].Orbit == m_WallCheckNodes[i].Orbit && nodes[i].Handles == m_WallCheckNodes[i].Handles &&
      !memcmp(&nodes[i].HandleIn, &m_WallCheckNodes[i].HandleIn, sizeof(XMFLOAT3)) &&
      !memcmp(&nodes[i].HandleOut, &m_WallCheckNodes[i].HandleOut, sizeof(XMFLOAT3)) &&
      nodes[i].OrbitYaw == m_WallCheckNodes[i].OrbitYaw && nodes[i].OrbitPitch == m_WallCheckNodes[i].OrbitPitch &&
      nodes[i].OrbitDistance == m_WallCheckNodes[i].OrbitDistance && nodes[i].OrbitWay == m_WallCheckNodes[i].OrbitWay &&
      nodes[i].OrbitTurns == m_WallCheckNodes[i].OrbitTurns && nodes[i].OrbitLift == m_WallCheckNodes[i].OrbitLift &&
      nodes[i].OrbitBulge == m_WallCheckNodes[i].OrbitBulge &&
      nodes[i].OrbitFocusSet == m_WallCheckNodes[i].OrbitFocusSet &&
      !memcmp(&nodes[i].OrbitFocus, &m_WallCheckNodes[i].OrbitFocus, sizeof(XMFLOAT3));
  if (!same)
  {
    m_WallCheckNodes = nodes;
    m_WallCheckRadius = m_CollisionRadius;
    m_WallCheckRunning = true;
    m_WallCheckTime = 0.0;
    m_WallCheckHasLast = false;
    m_WallSpans.clear();
    m_WallStretches.clear();
  }
  if (!m_WallCheckRunning)
  {
    // The finished check's result, once the keyframes aren't being dragged:
    // a drag changes them every frame, and each frame's check would be logged
    if (m_WallCheckLogOwed && !m_TrackPlayer.IsBeingEdited())
    {
      m_WallCheckLogOwed = false;
      util::log::Write("Camera collision: the selected track's path (%.1f s) %s", m_WallCheckNodes.back().TimeStamp,
        m_WallSpans.empty() ? "goes through nothing solid" :
        ("goes through something solid in " + std::to_string(m_WallSpans.size()) + " places").c_str());
    }
    if (m_WallFixWanted)
      FixTrackWalls();
    return;
  }
  double end = nodes.back().TimeStamp;
  int sweeps = 0, samples = 0;
  while (m_WallCheckRunning && sweeps < WallCheckSweepsPerFrame && samples < WallCheckSamplesPerFrame)
  {
    double time = m_WallCheckTime;
    m_WallCheckTime = time + WallCheckStep;
    if (time >= end)
    {
      time = end;
      m_WallCheckRunning = false;
    }
    ++samples;
    CatmullRomNode node;
    if (!m_TrackPlayer.SampleAt(time, node))
    {
      // Round a focus: where it goes isn't known before it plays
      m_WallCheckHasLast = false;
      continue;
    }
    if (!m_WallCheckHasLast)
    {
      m_WallCheckLast = node.Position;
      m_WallCheckLastTime = time;
      m_WallCheckHasLast = true;
      continue;
    }
    XMVECTOR a = XMLoadFloat3(&m_WallCheckLast), b = XMLoadFloat3(&node.Position);
    float distance = XMVectorGetX(XMVector3Length(b - a));
    if (distance < WallCheckSpacing && m_WallCheckRunning)
      continue;
    if (distance <= WallCheckCut)
    {
      ++sweeps;
      float fraction;
      XMFLOAT3 normal;
      if (collision::Sweep(&m_WallCheckLast.x, &node.Position.x, m_CollisionRadius, &fraction, &normal.x) &&
          XMVectorGetX(XMVector3Dot(b - a, XMLoadFloat3(&normal))) < 0.f)
      {
        if (!m_WallSpans.empty() && m_WallCheckLastTime - m_WallSpans.back().To <= WallSpanJoin)
          m_WallSpans.back().To = time;
        else
          m_WallSpans.push_back({ m_WallCheckLastTime, time });
        m_WallStretches.push_back({ m_WallCheckLast, node.Position });
      }
    }
    m_WallCheckLast = node.Position;
    m_WallCheckLastTime = time;
  }
  // Finished: logged from the next frame on (above)
  if (!m_WallCheckRunning)
    m_WallCheckLogOwed = true;
}

void CameraManager::FixTrackWalls()
{
  if (m_WallSpans.empty() || m_WallFixPasses >= MaxWallFixPasses)
  {
    util::log::Write("Camera collision: %d keyframes put in to take the track round what's in the way; %d places left",
      m_WallFixAdded, (int)m_WallSpans.size());
    m_WallFixWanted = false;
    return;
  }
  ++m_WallFixPasses;
  static const float offsets[] = { 1.5f, 3.f, 6.f, 12.f, 24.f, 48.f, 96.f, 192.f };
  std::vector<std::pair<double, XMFLOAT3>> inserts;
  double end = m_TrackPlayer.GetTrackLength();
  for (WallSpan const& span : m_WallSpans)
  {
    double middle = (span.From + span.To) * 0.5;
    CatmullRomNode before, at, after;
    if (!m_TrackPlayer.SampleAt((std::max)(0.0, span.From - WallSpanJoin), before) || !m_TrackPlayer.SampleAt(middle, at) ||
        !m_TrackPlayer.SampleAt((std::min)(end, span.To + WallSpanJoin), after))
      continue;
    XMVECTOR a = XMLoadFloat3(&before.Position), base = XMLoadFloat3(&at.Position), b = XMLoadFloat3(&after.Position);
    XMVECTOR up = XMVectorSet(0.f, 1.f, 0.f, 0.f);
    XMVECTOR along = XMVectorSetY(b - a, 0.f);
    if (XMVectorGetX(XMVector3LengthSq(along)) < 1e-6f)
      along = XMVectorSet(1.f, 0.f, 0.f, 0.f);
    XMVECTOR side = XMVector3Normalize(XMVector3Cross(up, along));
    // The smallest way round: over it, or to either side
    bool found = false;
    XMFLOAT3 place;
    for (float offset : offsets)
    {
      XMVECTOR tries[3] = { base + up * offset, base + side * offset, base - side * offset };
      for (XMVECTOR const& candidate : tries)
      {
        XMFLOAT3 c, from, to, normal;
        float fraction;
        XMStoreFloat3(&c, candidate);
        XMStoreFloat3(&from, a);
        XMStoreFloat3(&to, b);
        if (!collision::Sweep(&from.x, &c.x, m_CollisionRadius, &fraction, &normal.x) &&
            !collision::Sweep(&c.x, &to.x, m_CollisionRadius, &fraction, &normal.x))
        {
          place = c;
          found = true;
          break;
        }
      }
      if (found)
        break;
    }
    if (found)
      inserts.push_back({ middle, place });
  }
  int added = m_TrackPlayer.InsertNodes(inserts);
  m_WallFixAdded += added;
  if (!added)
  {
    util::log::Write("Camera collision: no way round found for the %d places left (%d keyframes put in)", (int)m_WallSpans.size(),
      m_WallFixAdded);
    m_WallFixWanted = false;
  }
  // The changed keyframes are checked again from the start (StepTrackWallCheck)
}

void CameraManager::ProcessOrbitRequests()
{
  // Not while the track plays: they wait for it to stop
  if (m_TrackPlayer.IsPlaying())
    return;
  TrackPlayer::OrbitRequest request;
  while (m_TrackPlayer.TakeOrbitRequest(request))
  {
    std::vector<CatmullRomNode> const& nodes = m_TrackPlayer.GetNodes();
    int i = request.Keyframe;
    // Only on the track shown, the keyframe still there (and still ticked)
    if (request.Track != m_TrackPlayer.GetSelectedTrackIndex() || i < 0 || i >= (int)nodes.size() ||
        (request.Ticked && !nodes[i].Orbit))
      continue;
    if (!request.Ticked)
    {
      if (m_TrackPlayer.IsOrbitSegment(i))
        m_TrackPlayer.SetOrbitNote(i, FindOrbitWay(i, true));
      continue;
    }
    // Placed already (the same tick asked twice)
    if (nodes[i].OrbitDistance > 0.f)
      continue;
    // Where round the focus it is, from where it is now, the way circling
    // starts (see OrbitFocus), and its view kept (TrackPlayer::SetOrbitPlace)
    if (!m_FocusOk)
    {
      m_TrackPlayer.UntickOrbit(i);
      m_TrackPlayer.SetOrbitNote(i, "There was no focus to go round (Faith, or what the camera follows).");
      continue;
    }
    // (round the focus where it is now, kept as its point: see
    // CatmullRomNode::OrbitFocus; its yaw from the world's heading)
    XMFLOAT3 offset;
    XMStoreFloat3(&offset, XMLoadFloat3(&nodes[i].Position) - XMLoadFloat3(&m_FocusPoint));
    float distance = sqrtf(offset.x * offset.x + offset.y * offset.y + offset.z * offset.z);
    float yaw = offset.x * offset.x + offset.z * offset.z > 1e-8f ? XMScalarModAngle(atan2f(offset.x, offset.z)) : 0.f;
    float pitch = distance > 1e-4f ? Clamp(asinf(Clamp(offset.y / distance, -1.f, 1.f)), -OrbitMaxPitch, OrbitMaxPitch) : 0.f;
    distance = Clamp(distance, OrbitMinDistance, OrbitMaxDistance);
    m_TrackPlayer.SetOrbitPlace(i, yaw, pitch, distance, m_FocusPoint);
    util::log::Write("Track: keyframe %d placed round the focus from where it is (%.0f degrees round, %.0f up, %.1f m), "
      "its view kept", i + 1, XMConvertToDegrees(yaw), XMConvertToDegrees(pitch), distance);
    // The ways round on both sides of it, each noted at the keyframe it starts from
    if (m_TrackPlayer.IsOrbitSegment(i - 1))
      m_TrackPlayer.SetOrbitNote(i - 1, FindOrbitWay(i - 1, false));
    if (m_TrackPlayer.IsOrbitSegment(i))
      m_TrackPlayer.SetOrbitNote(i, FindOrbitWay(i, false));
  }
}

std::string CameraManager::FindOrbitWay(int index, bool ownStep)
{
  char text[320];
  // (round the point the move goes round: its keyframes', see
  // CatmullRomNode::OrbitFocus; the focus where it is only for one without)
  XMFLOAT3 const* live = m_FocusOk ? &m_FocusPoint : nullptr;
  float baseYaw = OrbitBaseYaw();
  XMFLOAT3 focus;
  if (!m_TrackPlayer.SegmentPivot(index, live, baseYaw, focus))
    return "No point to go round now (no focus): the way round wasn't looked for.";
  std::vector<CatmullRomNode> const& nodes = m_TrackPlayer.GetNodes();
  CatmullRomNode const& n1 = nodes[index];
  CatmullRomNode const& n2 = nodes[index + 1];

  // The way the track goes round the focus between their places, and the
  // whole turns it makes more than that way's own step
  float sweep = m_TrackPlayer.PathSweep(index, focus);
  int way = 0, turns = 0;
  if (fabsf(sweep) > OrbitWayMinSweep)
  {
    way = sweep > 0.f ? 1 : -1;
    float step = fabsf(m_TrackPlayer.OrbitStepWith(index, way, 0, live, baseYaw));
    turns = (std::max)(0, (int)floorf((fabsf(sweep) - step) / XM_2PI + 0.5f));
  }
  // The other way round: against the track's way, or the shorter way's
  int other = way != 0 ? -way : (m_TrackPlayer.OrbitStepWith(index, 0, 0, live, baseYaw) >= 0.f ? -1 : 1);

  // What's tried, in order: the track's way, risen a little; the other way
  // round; then risen more. The first that goes through nothing is taken,
  // else the one that goes through least.
  struct Candidate { int Way, Turns; float Lift; };
  const float lift10 = XMConvertToRadians(10.f), lift20 = XMConvertToRadians(20.f);
  const float lift35 = XMConvertToRadians(35.f), lift50 = XMConvertToRadians(50.f);
  const Candidate order[] = {
    { way, turns, 0.f }, { way, turns, lift10 }, { way, turns, lift20 },
    { other, 0, 0.f }, { other, 0, lift10 }, { other, 0, lift20 },
    { way, turns, lift35 }, { other, 0, lift35 }, { way, turns, lift50 }, { other, 0, lift50 } };
  const int candidates = (int)(sizeof(order) / sizeof(order[0]));
  // No higher over the focus than circling it may go (an end that isn't an
  // orbit keyframe: how high it is over the focus now)
  auto pitchOf = [&](CatmullRomNode const& node)
  {
    if (node.Orbit && node.OrbitDistance > 0.f)
      return node.OrbitPitch;
    XMVECTOR offset = XMLoadFloat3(&node.Position) - XMLoadFloat3(&focus);
    float distance = XMVectorGetX(XMVector3Length(offset));
    return distance > 1e-4f ? asinf(Clamp(XMVectorGetY(offset) / distance, -1.f, 1.f)) : 0.f;
  };
  float top = (pitchOf(n1) + pitchOf(n2)) * 0.5f;

  int best = 0;
  float bestBlocked = -1.f;
  int sweeps = 0;
  bool checked = collision::Available();
  if (checked)
  {
    static XMFLOAT3 arc[OrbitArcMaxPoints];   // game thread
    for (int c = 0; c < candidates; ++c)
    {
      if (order[c].Lift > 0.f && top + order[c].Lift > OrbitMaxPitch)
        continue;
      int count = m_TrackPlayer.OrbitArcPoints(index, order[c].Way, order[c].Turns, order[c].Lift, live, baseYaw, arc,
        OrbitArcMaxPoints);
      if (count < 2)
        continue;
      // Along it as the wall check does (see StepTrackWallCheck): what it
      // goes into, not what it comes out of
      float blocked = 0.f;
      for (int k = 1; k < count; ++k)
      {
        float fraction;
        XMFLOAT3 normal;
        ++sweeps;
        if (collision::Sweep(&arc[k - 1].x, &arc[k].x, m_CollisionRadius, &fraction, &normal.x))
        {
          XMVECTOR a = XMLoadFloat3(&arc[k - 1]), b = XMLoadFloat3(&arc[k]);
          if (XMVectorGetX(XMVector3Dot(b - a, XMLoadFloat3(&normal))) < 0.f)
            blocked += XMVectorGetX(XMVector3Length(b - a));
        }
      }
      if (bestBlocked < 0.f || blocked < bestBlocked)
      {
        best = c;
        bestBlocked = blocked;
      }
      if (blocked <= 0.f)
        break;
    }
    checked = bestBlocked >= 0.f;
  }
  Candidate const& chosen = order[best];
  m_TrackPlayer.SetOrbitWay(index, chosen.Way, chosen.Turns, chosen.Lift, ownStep);

  // What was found, in words
  char turnsText[48] = "", liftText[48] = "";
  if (chosen.Way != 0 && chosen.Turns > 0)
    sprintf_s(turnsText, ", %d whole turn%s more", chosen.Turns, chosen.Turns > 1 ? "s" : "");
  if (chosen.Lift > 0.f)
    sprintf_s(liftText, ", rising %.0f degrees in the middle", XMConvertToDegrees(chosen.Lift));
  const char* why = way == 0 ? (chosen.Way == 0 ? " (the track itself doesn't go round it)" : " (the shorter way is blocked)")
    : (chosen.Way == way ? " (the way the track goes)" : " (the track's way is blocked)");
  const char* walls = !checked ? " Walls weren't checked: the game's collision isn't there." :
    (bestBlocked <= 0.f ? " Clear of walls round its point." : "");
  char blockedText[96] = "";
  if (checked && bestBlocked > 0.f)
    sprintf_s(blockedText, " It goes through something solid whichever way (about %.1f m of it, the least found).", bestBlocked);
  sprintf_s(text, "Keyframe %d to %d: %s%s%s%s.%s%s", index + 1, index + 2,
    chosen.Way == 0 ? "the shorter way" : (chosen.Way > 0 ? "round to the right" : "round to the left"), why, turnsText,
    liftText, walls, blockedText);
  util::log::Write("Track: orbit %s (%.0f degrees round on the track's path, %d sweeps)", text,
    XMConvertToDegrees(sweep), sweeps);
  return text;
}

void CameraManager::UpdateCamera(double dt, double trackDt)
{
  float pitch = (float)(m_Camera.dPitch * dt * m_Camera.RotationSpeed) + m_Camera.dMousePitch;
  float yaw = (float)(m_Camera.dYaw * dt * m_Camera.RotationSpeed) + m_Camera.dMouseYaw;
  float roll = (float)(m_Camera.dRoll * dt * m_Camera.RollSpeed);

  // The heavy camera: turning (and moving, below) gets going and stops over a
  // moment instead of at once
  float heavyBlend = 1.f;
  if (m_Heaviness > 0.f && dt > 0.0)
  {
    heavyBlend = (float)(1.0 - exp(-dt / (m_Heaviness * HeavyMaxSeconds)));
    float wanted[3] = { pitch / (float)dt, yaw / (float)dt, roll / (float)dt };
    for (int i = 0; i < 3; ++i)
      m_TurnRates[i] += (wanted[i] - m_TurnRates[i]) * heavyBlend;
    pitch = m_TurnRates[0] * (float)dt;
    yaw = m_TurnRates[1] * (float)dt;
    roll = m_TurnRates[2] * (float)dt;
  }
  else if (m_Heaviness <= 0.f)
    m_TurnRates[0] = m_TurnRates[1] = m_TurnRates[2] = 0.f;

  XMVECTOR qRotation = XMLoadFloat4(&m_Camera.Rotation);
  XMVECTOR vPosition = XMLoadFloat3(&m_Camera.Position);
  bool trackPlaying = m_TrackPlayer.IsPlaying();

  // What orbit, aim, auto-focus and dolly zoom go by (see FindFocus).
  // Circling it, turning circles it instead (see OrbitFocus).
  XMVECTOR focus = XMVectorZero();
  m_FocusOk = FindFocus(focus);
  if (m_FocusOk)
    XMStoreFloat3(&m_FocusPoint, focus);
  // Orbit keyframes are played round it where it is now (see
  // TrackPlayer::SetLiveFocus). A track moving with what's followed is
  // played where it was when it started, then moved with it (below), so it
  // gets the focus where that move takes it back to, and the heading then.
  if (m_FocusOk && trackPlaying && m_AttachTrackActive && m_AttachValid)
  {
    XMMATRIX start = XMLoadFloat4x4(&m_AttachTrackStart);
    XMMATRIX moved = XMMatrixInverse(nullptr, start) * XMLoadFloat4x4(&m_AttachNow);
    XMFLOAT3 startFocus;
    XMStoreFloat3(&startFocus, XMVector3TransformCoord(focus, XMMatrixInverse(nullptr, moved)));
    float x = m_AttachTrackStart._31, z = m_AttachTrackStart._33;
    float startYaw = m_AttachRotation && x * x + z * z > 1e-6f ? atan2f(x, z) : 0.f;
    m_TrackPlayer.SetLiveFocus(&startFocus, startYaw);
  }
  else
    m_TrackPlayer.SetLiveFocus(m_FocusOk ? &m_FocusPoint : nullptr, OrbitBaseYaw());
  // Kept with new keyframes (see CatmullRomNode::Orbit); set below while circling
  m_Camera.Orbiting = false;
  bool orbit = m_Orbit && m_FocusOk && !trackPlaying;
  if (!orbit)
    m_OrbitActive = false;
  else
  {
    m_Velocity = XMFLOAT3(0, 0, 0);
    m_TurnRates[2] = 0.f;
  }

  if (!orbit)
  {
    XMVECTOR qPitch = XMQuaternionRotationRollPitchYaw(pitch, 0, 0);
    XMVECTOR qYaw = XMQuaternionRotationRollPitchYaw(0, yaw, 0);
    XMVECTOR qRoll = XMQuaternionRotationRollPitchYaw(0, 0, roll);

    // Adds delta rotations to camera rotation
    qRotation = XMQuaternionMultiply(qPitch, qRotation);
    qRotation = XMQuaternionMultiply(qRotation, qYaw);
    qRotation = XMQuaternionMultiply(qRoll, qRotation);

    // Make sure it's normalized
    qRotation = XMQuaternionNormalize(qRotation);
  }
  m_Camera.FieldOfView += (float)(m_Camera.dFov * dt * m_Camera.FovSpeed);

  // If a camera track is being played, get the current
  // state and overwrite position/rotation/FoV.
  float trackShake = -1.f;
  if (trackPlaying)
  {
    CatmullRomNode resultNode = m_TrackPlayer.PlayForward(trackDt);
    // A shot sequence's next shot follows her from where she is when it starts
    if (m_TrackPlayer.TakeCut() && m_AttachTrackActive && m_AttachValid)
      m_AttachTrackStart = m_AttachNow;
    if (m_TrackPlayer.IsShakeLocked())
      trackShake = resultNode.Shake;
    m_TrackGameSpeed = resultNode.GameSpeed;

    // Between orbit keyframes: round the point the move goes round (theirs,
    // CatmullRomNode::OrbitFocus: where the focus was when they were ticked or
    // made; or, for one without, the focus where it is now, as TrackPlayer
    // gives it), looking at it, with their own view on top (OrbitView); one
    // that can't be placed, as kept. Before following moves the track, so the
    // moves round a point go with it as the rest of the track does.
    if (resultNode.Orbit && resultNode.OrbitFocusSet)
    {
      XMVECTOR direction = OrbitDirection(resultNode.OrbitYaw, resultNode.OrbitPitch);
      XMStoreFloat3(&resultNode.Position, XMLoadFloat3(&resultNode.OrbitFocus) + direction * resultNode.OrbitDistance);
      XMVECTOR look = LookRotation(-direction, XMMatrixRotationQuaternion(qRotation).r[0]);
      if (!IsPlainOrbitView(resultNode.OrbitView))
        look = XMQuaternionNormalize(XMQuaternionMultiply(XMLoadFloat4(&resultNode.OrbitView), look));
      XMStoreFloat4(&resultNode.Rotation, look);
    }

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
    vPosition = GuardTrackPlace(vPosition, dt);
  }
  else
    m_GuardValid = false;

  if (m_Camera.FieldOfView < 5.f) m_Camera.FieldOfView = 5.f;
  if (m_Camera.FieldOfView > 150.f) m_Camera.FieldOfView = 150.f;

  // Create rotation matrix
  XMMATRIX rotMatrix = XMMatrixRotationQuaternion(qRotation);

  // The movement speed, set by the height above Faith when wanted
  m_SpeedFactor = SpeedByHeight(vPosition);
  float speed = m_Camera.MovementSpeed * m_SpeedFactor;

  if (orbit)
  {
    OrbitFocus(focus, pitch, yaw, speed, dt, heavyBlend, vPosition, qRotation, rotMatrix);
    m_Camera.Orbiting = true;
    // (what new keyframes go round, kept: see CatmullRomNode::OrbitFocus)
    XMStoreFloat3(&m_Camera.OrbitFocus, focus);
    m_Camera.OrbitYaw = XMScalarModAngle(m_OrbitYaw + OrbitBaseYaw());
    m_Camera.OrbitPitch = m_OrbitPitch;
    m_Camera.OrbitDistance = m_OrbitDistance;
  }
  else if (!trackPlaying)
  {
    // Add delta positions. Flying level: forward and back, and sideways,
    // along the ground whichever way the camera looks; up and down straight
    // up. Where it faces along the ground is its back row's level part with
    // its up row's (the one that's left when it looks straight up or down).
    XMVECTOR right = rotMatrix.r[0], up = rotMatrix.r[1], back = rotMatrix.r[2];
    if (m_FlyLevel)
    {
      float backY = XMVectorGetY(back);
      XMVECTOR facing = XMVectorSetY(back, 0.f) - XMVectorSetY(up, 0.f) * (backY < 0.f ? -1.f : 1.f);
      if (XMVectorGetX(XMVector3LengthSq(facing)) > 1e-8f)
      {
        back = XMVector3Normalize(facing);
        up = XMVectorSet(0.f, 1.f, 0.f, 0.f);
        right = XMVector3Cross(up, back);
      }
    }
    XMVECTOR move = right * m_Camera.dX + up * m_Camera.dY + back * m_Camera.dZ;
    XMVECTOR before = vPosition;
    if (m_Heaviness > 0.f && dt > 0.0)
    {
      XMVECTOR velocity = XMLoadFloat3(&m_Velocity);
      velocity += (move * speed - velocity) * heavyBlend;
      XMStoreFloat3(&m_Velocity, velocity);
      vPosition += velocity * (float)dt;
    }
    else
    {
      if (m_Heaviness <= 0.f)
        m_Velocity = XMFLOAT3(0, 0, 0);
      vPosition += move * (float)(dt * speed);
    }
    // Stopping at walls: the move swept as a ball through the game's physics,
    // up to what's in the way and along it (see collision::Move)
    if (m_Collision)
    {
      XMFLOAT3 from, to, wall;
      XMStoreFloat3(&from, before);
      XMStoreFloat3(&to, vPosition);
      if (collision::Move(&from.x, &to.x, m_CollisionRadius, &wall.x))
      {
        // The heavy camera's speed into what it touched goes
        XMVECTOR velocity = XMLoadFloat3(&m_Velocity);
        XMVECTOR normal = XMLoadFloat3(&wall);
        float into = XMVectorGetX(XMVector3Dot(velocity, normal));
        if (into < 0.f)
          XMStoreFloat3(&m_Velocity, velocity - normal * into);
      }
      vPosition = XMLoadFloat3(&to);
    }
  }
  else
    m_Velocity = XMFLOAT3(0, 0, 0);

  // Aimed at the focus while it moves freely or plays a track (circling it
  // aims already)
  if (m_LookAt && m_FocusOk && !orbit)
  {
    XMVECTOR look = focus - vPosition;
    if (XMVectorGetX(XMVector3LengthSq(look)) > 1e-4f)
    {
      qRotation = LookRotation(look, rotMatrix.r[0]);
      rotMatrix = XMMatrixRotationQuaternion(qRotation);
    }
  }

  // Dolly zoom: the field of view set so the focus keeps its size on screen
  // as the camera moves to or from it. A change of the field of view made
  // since (the keys, the wheel, the slider, a track) sets the size kept.
  m_FocusDistance = m_FocusOk ? XMVectorGetX(XMVector3Length(focus - vPosition)) : 0.f;
  if (m_DollyZoom && m_FocusOk)
  {
    float distance = m_FocusDistance > 0.1f ? m_FocusDistance : 0.1f;
    float fov = m_Camera.FieldOfView;
    if (!m_DollyActive || fabsf(fov - m_DollyFov) > 1e-3f)
      m_DollyWidth = distance * tanf(XMConvertToRadians(fov) * 0.5f);
    m_Camera.FieldOfView = Clamp(XMConvertToDegrees(2.f * atanf(m_DollyWidth / distance)), 5.f, 150.f);
    m_DollyFov = m_Camera.FieldOfView;
    m_DollyActive = true;
  }
  else
    m_DollyActive = false;

  // Store results
  XMStoreFloat3(&m_Camera.Position, vPosition);
  XMStoreFloat4(&m_Camera.Rotation, qRotation);

  // What the game gets: that, with the hand-held shake on top (not kept in
  // the camera's own place, so keyframes and bookmarks don't get it). On the
  // track's clock while one plays, so a fixed playback rate repeats it.
  // The shake's strength: the track's while it sets it (see the keyframe
  // options), else the menu's; an impact on top while it lasts
  m_Camera.ShakeStrength = m_ShakeStrength;
  m_Camera.GameSpeed = m_GameSpeed;
  float shake = trackShake >= 0.f ? trackShake : m_ShakeStrength;
  double clockStep = trackPlaying ? trackDt : dt;
  float impact = 0.f;
  if (m_ImpactAge >= 0.0)
  {
    impact = m_ImpactStrength * (float)exp(-m_ImpactAge / ImpactFade);
    m_ImpactAge += clockStep;
    if (m_ImpactAge > ImpactSeconds)
      m_ImpactAge = -1.0;
  }
  XMVECTOR viewPosition = vPosition;
  if (shake > 0.f || impact > 0.f)
  {
    m_ShakeTime += clockStep * m_ShakeSpeed;
    ShakeProfile const& p = ShakeProfiles[m_ShakePreset >= 0 && m_ShakePreset < ShakeProfileCount ? m_ShakePreset : 0];
    ShakeProfile const& j = ImpactProfile;
    // Each channel: the kind's at the shake's strength, the impact's at its own
    auto channel = [&](int c, float kindSize, float impactSize) {
      return ShakeNoise(m_ShakeTime, c, p) * kindSize * shake + ShakeNoise(m_ShakeTime, c, j) * impactSize * impact;
    };
    XMVECTOR qShake = XMQuaternionRotationRollPitchYaw(channel(0, p.Angle, j.Angle), channel(1, p.Angle, j.Angle),
      channel(2, p.Roll, j.Roll));
    viewPosition += rotMatrix.r[0] * channel(3, p.Sideways, j.Sideways) + rotMatrix.r[1] * channel(4, p.UpDown, j.UpDown);
    rotMatrix = XMMatrixRotationQuaternion(XMQuaternionNormalize(XMQuaternionMultiply(qShake, qRotation)));
  }
  rotMatrix.r[3] = XMVectorSetW(viewPosition, 1.0f);
  XMStoreFloat4x4(&m_Camera.Transform, rotMatrix);

  ClearCameraDeltas();
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
  // With the cursor on a light's dot and Ctrl held, Ctrl + the wheel is the
  // light's radius (LightManager::HoldsCtrl): "move down" waits then when
  // it's on Ctrl (the default, Left Ctrl)
  float down = pInput->GetActionState(Camera_Down);
  int downKey = pInput->GetKeyboardBinding(Camera_Down);
  auto isCtrl = [](int key) { return key == VK_CONTROL || key == VK_LCONTROL || key == VK_RCONTROL; };
  if (down > 0.f && (isCtrl(downKey & 0xFF) || isCtrl(downKey >> 8)) && g_mainHandle->GetLightManager()->HoldsCtrl())
    down = 0.f;
  m_Camera.dY = pInput->GetActionState(Camera_Up) - down;
  m_Camera.dZ = pInput->GetActionState(Camera_Backward) - pInput->GetActionState(Camera_Forward);

  m_Camera.dPitch = pInput->GetActionState(Camera_PitchUp) - pInput->GetActionState(Camera_PitchDown);
  m_Camera.dYaw = pInput->GetActionState(Camera_YawLeft) - pInput->GetActionState(Camera_YawRight);
  m_Camera.dRoll = pInput->GetActionState(Camera_RollLeft) - pInput->GetActionState(Camera_RollRight);
  m_Camera.dFov = pInput->GetActionState(Camera_IncFov) - pInput->GetActionState(Camera_DecFov);

  // With the menu open the mouse belongs to the menu, unless the right button
  // is held outside the menu window
  bool mouseLook = !pUI->IsEnabled() || (!pUI->HasMouseFocus() && (GetAsyncKeyState(VK_RBUTTON) & 0x8000));
  // Not while a point of the drawn track path is dragged (see DrawTrackPath)
  if (m_PathDragActive && pUI->IsEnabled())
    mouseLook = false;
  if (m_KbmDisabled && mouseLook)
  {
    m_Camera.dMousePitch = -mouseState.y * m_Camera.MouseSensitivity;
    m_Camera.dMouseYaw = -mouseState.x * m_Camera.MouseSensitivity;
    // The wheel zooms, 2 degrees a notch; circling the focus it goes to and from it
    float notches = mouseState.z / WHEEL_DELTA;
    if (m_OrbitActive)
      m_OrbitWheel += notches;
    else
      m_Camera.FieldOfView -= notches * 2.f;
  }
}

bool CameraManager::FindFocus(XMVECTOR& point)
{
  // What's followed, where the camera follows it (its smoothed copy)
  if (m_AttachValid)
  {
    float height = m_AttachObject ? attach::AimHeight(m_AttachTarget) : FaithFocusHeight;
    point = XMVectorSet(m_AttachNow._41, m_AttachNow._42 + height, m_AttachNow._43, 0.f);
    return true;
  }
  XMFLOAT3 place;
  float heading;
  bool hasHeading;
  if (!GetFaithPlace(place, heading, hasHeading))
    return false;
  point = XMVectorSet(place.x, place.y + FaithFocusHeight, place.z, 0.f);
  return true;
}

float CameraManager::OrbitBaseYaw()
{
  // Following something with Turn with it on: the circle turns with it
  if (!m_AttachValid || !m_AttachRotation)
    return 0.f;
  float x = m_AttachNow._31, z = m_AttachNow._33;
  return x * x + z * z > 1e-6f ? atan2f(x, z) : 0.f;
}

void CameraManager::OrbitFocus(XMVECTOR focus, float pitch, float yaw, float speed, double dt, float heavyBlend,
  XMVECTOR& position, XMVECTOR& rotation, XMMATRIX& rows)
{
  float baseYaw = OrbitBaseYaw();
  if (!m_OrbitActive)
  {
    // From where the camera is, so turning it on doesn't move it
    XMFLOAT3 offset;
    XMStoreFloat3(&offset, position - focus);
    float distance = sqrtf(offset.x * offset.x + offset.y * offset.y + offset.z * offset.z);
    m_OrbitDistance = Clamp(distance, OrbitMinDistance, OrbitMaxDistance);
    if (offset.x * offset.x + offset.z * offset.z > 1e-8f)
      m_OrbitYaw = atan2f(offset.x, offset.z) - baseYaw;
    m_OrbitPitch = distance > 1e-4f ? asinf(Clamp(offset.y / distance, -1.f, 1.f)) : 0.f;
    m_OrbitMove[0] = m_OrbitMove[1] = m_OrbitMove[2] = 0.f;
    m_OrbitWheel = 0.f;
    m_OrbitActive = true;
  }

  // Turning goes round it the same way as turning in place: yawing the view
  // goes round, looking up goes down under it
  m_OrbitYaw += yaw;
  m_OrbitPitch -= pitch;

  // The movement keys: sideways round it, up and down over it, forward and
  // back to and from it (the heavy camera smooths them); the wheel to and from it
  float wanted[3] = { m_Camera.dX * speed, m_Camera.dY * speed, m_Camera.dZ * speed };
  for (int i = 0; i < 3; ++i)
    m_OrbitMove[i] += (wanted[i] - m_OrbitMove[i]) * heavyBlend;
  float step = (float)dt;
  float ring = m_OrbitDistance * cosf(m_OrbitPitch);
  m_OrbitYaw += m_OrbitMove[0] * step / (ring > 0.1f ? ring : 0.1f);
  m_OrbitPitch += m_OrbitMove[1] * step / m_OrbitDistance;
  m_OrbitDistance += m_OrbitMove[2] * step;
  m_OrbitDistance *= powf(OrbitWheelStep, m_OrbitWheel);
  m_OrbitWheel = 0.f;
  m_OrbitDistance = Clamp(m_OrbitDistance, OrbitMinDistance, OrbitMaxDistance);
  m_OrbitPitch = Clamp(m_OrbitPitch, -OrbitMaxPitch, OrbitMaxPitch);
  m_OrbitYaw = XMScalarModAngle(m_OrbitYaw);

  XMVECTOR direction = OrbitDirection(baseYaw + m_OrbitYaw, m_OrbitPitch);
  position = focus + direction * m_OrbitDistance;
  rotation = LookRotation(-direction, rows.r[0]);
  rows = XMMatrixRotationQuaternion(rotation);
}

float CameraManager::SpeedByHeight(XMVECTOR position)
{
  if (m_SpeedByHeight <= 0.f)
    return 1.f;
  XMFLOAT3 place;
  float heading;
  bool hasHeading;
  if (!GetFaithPlace(place, heading, hasHeading))
    return 1.f;
  float ratio = Clamp((XMVectorGetY(position) - place.y) / SpeedHeightReference, SpeedHeightMin, SpeedHeightMax);
  return powf(ratio, m_SpeedByHeight);
}

void CameraManager::UseBookmark(int index, bool save)
{
  if (index < 0 || index >= BookmarkCount || !m_CameraEnabled)
    return;
  CameraBookmark& mark = m_Bookmarks[index];
  if (save)
  {
    mark.Set = true;
    mark.Position = m_Camera.Position;
    mark.Rotation = m_Camera.Rotation;
    mark.FieldOfView = m_Camera.FieldOfView;
    util::log::Write("Camera bookmark %d saved at %.1f %.1f %.1f", index + 1, mark.Position.x, mark.Position.y, mark.Position.z);
    g_mainHandle->OnConfigChanged();
    return;
  }
  if (!mark.Set)
    return;
  m_Camera.Position = mark.Position;
  m_Camera.Rotation = mark.Rotation;
  m_Camera.FieldOfView = mark.FieldOfView;
  // It starts from there: no speed carried over, the circle and the dolly
  // zoom set up again from there
  m_Velocity = XMFLOAT3(0, 0, 0);
  m_TurnRates[0] = m_TurnRates[1] = m_TurnRates[2] = 0.f;
  m_OrbitActive = false;
  m_DollyActive = false;
  util::log::Write("Camera at bookmark %d, %.1f %.1f %.1f", index + 1, mark.Position.x, mark.Position.y, mark.Position.z);
}

bool CameraManager::DrawCameraMovesUI()
{
  bool configChanged = false;
  float fontSize = ImGui::GetFontSize();

  ImGui::SectionHeading("Around the focus");
  if (!m_CameraEnabled)
    ImGui::TextDisabled("The camera is off: these work while it's on.");
  else if (m_FocusOk)
    ImGui::Text("Focus: %s, %.1f m away", m_AttachObject && m_AttachValid ? attach::KindOf(m_AttachTarget) : "Faith",
      m_FocusDistance);
  else
    ImGui::TextDisabled("Focus: none (Faith isn't there and nothing is followed)");
  ImGui::HelpMarker("Orbit, aim, auto-focus and dolly zoom go by the focus: what the camera follows (FOLLOW tab), "
    "else Faith, at her chest.");
  configChanged |= ImGui::Checkbox("While a replay plays, Faith is the replayed one", &m_FocusReplayedFaith);
  ImGui::HelpMarker("While a recording plays (REPLAY tab), following Faith (FOLLOW), orbit, aim, auto-focus, dolly "
    "zoom and speed by height go by the replayed Faith, where she was then and which way she faced, instead of the "
    "real one standing still. Recordings made before this build have no heading: following turns with her only in "
    "new ones.");

  ImGui::Checkbox("Orbit", &m_Orbit);
  ImGui::SameLine(0.f, fontSize * 2.f);
  ImGui::Checkbox("Aim at it", &m_LookAt);
  ImGui::HelpMarker("Fly freely (or play a track) while the camera keeps looking at the focus, level. Off again each "
    "time the game starts.");
  ImGui::Checkbox("Dolly zoom", &m_DollyZoom);
  ImGui::HelpMarker("The \"vertigo\" effect: the field of view changes as the camera moves to or from the focus, "
    "so the focus keeps its size on screen and the background grows or shrinks behind it. Zooming meanwhile sets "
    "the size kept. While a track plays with its field of view locked, the track's field of view wins. Off again "
    "each time the game starts.");
  ImGui::SameLine(0.f, fontSize * 2.f);
  configChanged |= ImGui::Checkbox("Auto-focus", &m_AutoFocus);
  ImGui::HelpMarker("The depth of field focuses on the focus: its distances (the focus distance and the Simple and "
    "Sprite near and far ones) are moved with the focus' distance, so the set-up you made at its own focus distance "
    "goes with it. Needs VISUALS > Depth of field > Change this effect.");
  if (m_AutoFocus && !g_mainHandle->GetVisualsManager()->IsDofOverridden())
    ImGui::TextColored(ImVec4(1.f, 0.8f, 0.2f, 1.f), "Tick VISUALS > Depth of field > Change this effect for it");

  ImGui::SectionHeading("How the camera moves");
  if (ImGui::SliderFloat("Heavy camera", &m_Heaviness, 0.f, 1.f, m_Heaviness > 0.f ? "%.2f" : "off"))
  {
    m_Heaviness = Clamp(m_Heaviness, 0.f, 1.f);
    configChanged = true;
  }
  ImGui::HelpMarker("Moving and turning get going and stop smoothly instead of at once, like a heavy camera. 1 "
    "takes about a second.");
  if (ImGui::SliderFloat("Speed by height", &m_SpeedByHeight, 0.f, 1.f, m_SpeedByHeight > 0.f ? "%.2f" : "off"))
  {
    m_SpeedByHeight = Clamp(m_SpeedByHeight, 0.f, 1.f);
    configChanged = true;
  }
  if (m_SpeedByHeight > 0.f && m_CameraEnabled)
  {
    ImGui::SameLine();
    ImGui::TextDisabled("x%.2f now", m_SpeedFactor);
  }
  ImGui::HelpMarker("Faster high up, slower low down: the movement speed is as set 20 m above Faith, up to 25 times "
    "it far above her and down to a quarter of it near her height (at 1; less in between).");
  configChanged |= ImGui::Checkbox("Fly level", &m_FlyLevel);
  ImGui::HelpMarker("Forward, back and sideways stay at the same height whichever way the camera looks, like a "
    "drone; up and down still go straight up and down.");
  configChanged |= ImGui::Checkbox("Camera Collision", &m_Collision);
  if (m_Collision)
  {
    ImGui::Indent();
    if (ImGui::SliderFloat("Distance from walls", &m_CollisionRadius, 0.05f, 1.f, "%.2f m"))
    {
      m_CollisionRadius = Clamp(m_CollisionRadius, 0.05f, 1.f);
      configChanged = true;
    }
    ImGui::HelpMarker("How close the camera gets to what it stops at: it's moved as a ball of this radius.");
    configChanged |= ImGui::Checkbox("Camera tracks too", &m_TrackCollision);
    ImGui::HelpMarker("While a camera track plays it's kept out of walls the same way: where its path only grazes "
      "something the camera slides along it, and where the path goes right through something the camera waits at "
      "it and cuts to where the path comes out. To keep the shot whole, take the track round them (below).");
    if (!collision::Available())
      ImGui::TextDisabled("Not working now: the game's physics isn't there (is a level loaded?)");
    else if (m_TrackPlayer.GetNodeCount() >= 2)
    {
      // The selected track's check (see StepTrackWallCheck)
      ImGui::PushTextWrapPos(0.f);
      float length = m_TrackPlayer.GetTrackLength();
      if (m_WallFixWanted)
        ImGui::TextDisabled("Taking the selected track round what's in the way (pass %d)...", m_WallFixPasses + 1);
      else if (m_WallCheckRunning)
        ImGui::TextDisabled("Checking the selected track for walls... %.0f%%", length > 0.f ? 100.0 * m_WallCheckTime / length : 0.0);
      else if (m_WallSpans.empty())
        ImGui::TextDisabled("The selected track's path doesn't go through anything solid.");
      else
      {
        std::string times;
        for (size_t i = 0; i < m_WallSpans.size() && i < 6; ++i)
        {
          char at[32];
          sprintf_s(at, "%s%.1f s", i ? ", " : "", (m_WallSpans[i].From + m_WallSpans[i].To) * 0.5);
          times += at;
        }
        if (m_WallSpans.size() > 6)
          times += ", ...";
        ImGui::TextColored(ImVec4(1.f, 0.4f, 0.3f, 1.f), "The selected track's path goes through something solid in %d "
          "places (red on its path): at %s.", (int)m_WallSpans.size(), times.c_str());
        if (ImGui::Button("Take the track round them"))
        {
          m_WallFixWanted = true;
          m_WallFixPasses = 0;
          m_WallFixAdded = 0;
        }
        ImGui::HelpMarker("Puts a keyframe in each of those places, over or beside what's in the way (the smallest "
          "way round it finds, up to 192 m off), then checks the path again, a few times over. The camera still "
          "looks where it did there. Undo takes the keyframes out again.");
      }
      ImGui::PopTextWrapPos();
    }
    ImGui::Unindent();
  }

  ImGui::SectionHeading("Camera shake");
  if (ImGui::SliderFloat("Hand-held shake", &m_ShakeStrength, 0.f, 1.f, m_ShakeStrength > 0.f ? "%.2f" : "off"))
  {
    m_ShakeStrength = Clamp(m_ShakeStrength, 0.f, 1.f);
    configChanged = true;
  }
  {
    const char* kinds[ShakeProfileCount];
    for (int i = 0; i < ShakeProfileCount; ++i)
      kinds[i] = ShakeProfiles[i].Name;
    if (ImGui::Combo("Shake kind", &m_ShakePreset, kinds, ShakeProfileCount))
    {
      m_ShakePreset = m_ShakePreset < 0 ? 0 : (m_ShakePreset >= ShakeProfileCount ? ShakeProfileCount - 1 : m_ShakePreset);
      configChanged = true;
    }
  }
  if (ImGui::SliderFloat("Shake speed", &m_ShakeSpeed, 0.25f, 3.f, "%.2fx"))
  {
    m_ShakeSpeed = Clamp(m_ShakeSpeed, 0.25f, 3.f);
    configChanged = true;
  }
  // The Impact button and Impact strength: taken out of the menu (2026-11-09,
  // the user's list). m_ImpactStrength stays in the config.

  ImGui::SectionHeading("Letterbox and guide lines");
  {
    const char* ratios[LetterboxCount];
    for (int i = 0; i < LetterboxCount; ++i)
      ratios[i] = LetterboxRatios[i].Name;
    if (ImGui::Combo("Letterbox", &m_Letterbox, ratios, LetterboxCount))
    {
      m_Letterbox = m_Letterbox < 0 ? 0 : (m_Letterbox >= LetterboxCount ? LetterboxCount - 1 : m_Letterbox);
      configChanged = true;
    }
    if (m_Letterbox > 0 && ImGui::SliderFloat("Bar opacity", &m_LetterboxOpacity, 0.f, 1.f, "%.2f"))
    {
      m_LetterboxOpacity = Clamp(m_LetterboxOpacity, 0.f, 1.f);
      configChanged = true;
    }
  }
  configChanged |= ImGui::Checkbox("Thirds", &m_GuideThirds);
  ImGui::SameLine();
  configChanged |= ImGui::Checkbox("Golden ratio", &m_GuideGolden);
  ImGui::SameLine();
  configChanged |= ImGui::Checkbox("Centre", &m_GuideCenter);
  ImGui::SameLine();
  configChanged |= ImGui::Checkbox("Safe areas", &m_GuideSafe);
  configChanged |= ImGui::Checkbox("Guides while a track plays", &m_GuidesWhilePlaying);

  ImGui::SectionHeading("Wide fields of view");
  configChanged |= ImGui::Checkbox("Panini projection", &m_Panini);
  ImGui::HelpMarker("For wide fields of view: the middle of the picture stays as it is and the sides are squeezed "
    "instead of stretched, so things near the edges keep their shape. It's drawn over the game's finished picture, "
    "the game's HUD too (hide the HUD for shots), while the camera is on.");
  if (m_Panini)
  {
    if (ImGui::SliderFloat("Panini amount", &m_PaniniDistance, 0.f, 1.f, "%.2f"))
    {
      m_PaniniDistance = Clamp(m_PaniniDistance, 0.f, 1.f);
      configChanged = true;
    }
    ImGui::HelpMarker("0 is the normal picture, 1 the classic Panini projection.");
    if (ImGui::SliderFloat("Crop to fit", &m_PaniniCrop, 0.f, 1.f, "%.2f"))
    {
      m_PaniniCrop = Clamp(m_PaniniCrop, 0.f, 1.f);
      configChanged = true;
    }
    ImGui::HelpMarker("1 fills the screen (the picture is cropped a little); lower shows all of it, with black "
      "corners.");
  }

  // Track and bookmarks: taken out of the menu (2026-10-25). Show the track's
  // path is with the camera tracks now (DrawUI); the bookmarks still work with
  // their hotkeys and stay in the config.
#if 0
  ImGui::SubHeading("Track and bookmarks");
  InputSystem* pInput = g_mainHandle->GetInputSystem();
  ImGui::TextDisabled("Bookmarks: %s - %s go to one, %s - %s save it", pInput->GetKeyName(Action::Camera_GoToBookmark1).c_str(),
    pInput->GetKeyName(Action::Camera_GoToBookmark9).c_str(), pInput->GetKeyName(Action::Camera_SaveBookmark1).c_str(),
    pInput->GetKeyName(Action::Camera_SaveBookmark9).c_str());
  for (int i = 0; i < BookmarkCount; ++i)
  {
    ImGui::PushID(i);
    CameraBookmark const& mark = m_Bookmarks[i];
    ImGui::AlignTextToFramePadding();
    if (mark.Set)
      ImGui::Text("%d   %.0f, %.0f, %.0f", i + 1, mark.Position.x, mark.Position.y, mark.Position.z);
    else
      ImGui::TextDisabled("%d   -", i + 1);
    // Its buttons in a column; none (the camera off, not saved): the next
    // row on its own line
    if (m_CameraEnabled || mark.Set)
      ImGui::SameLine(fontSize * 12.f);
    if (m_CameraEnabled)
    {
      if (ImGui::Button("Save"))
        UseBookmark(i, true);
      if (mark.Set)
      {
        ImGui::SameLine();
        if (ImGui::Button("Go"))
          UseBookmark(i, false);
      }
    }
    if (mark.Set)
    {
      if (m_CameraEnabled)
        ImGui::SameLine();
      if (ImGui::Button("Clear"))
      {
        m_Bookmarks[i].Set = false;
        configChanged = true;
      }
    }
    ImGui::PopID();
  }
#endif
  return configChanged;
}

void CameraManager::ToggleCamera()
{
  if (m_IgcsSession.IsActive()) return;

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
    m_IgcsViewReady = false;
    m_IgcsRestoreFrame = false;
    m_HasLastFrame = false;
    m_AttachHasLast = false;
    m_AttachTrackActive = false;
    m_StreamingFocusReady = false;
    m_OrbitActive = false;
    m_DollyActive = false;
    m_Velocity = XMFLOAT3(0, 0, 0);
    m_TurnRates[0] = m_TurnRates[1] = m_TurnRates[2] = 0.f;
    m_CameraEnabled = true;
  }
  else
  {
    m_LastCameraPosition = m_Camera.Position;
    m_LastCameraRotation = m_Camera.Rotation;
    m_CameraEnabled = false;
    m_IgcsViewReady = false;
    m_IgcsRestoreFrame = false;
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

bool CameraManager::MoveToReplayedFaith()
{
  float place[3];
  float heading = 0.f;
  bool hasHeading = false;
  if (!m_Replay.GetPlayedFaith(place, heading, hasHeading))
  {
    util::log::Warning("No replay is playing: there's no replayed Faith to move the camera to");
    return false;
  }
  m_Camera.Position = XMFLOAT3(place[0], place[1] + FaithEyeHeight, place[2]);
  m_Velocity = XMFLOAT3(0, 0, 0);
  util::log::Write("Camera moved to the replayed Faith at %.2f %.2f %.2f", m_Camera.Position.x, m_Camera.Position.y,
    m_Camera.Position.z);
  return true;
}

void CameraManager::DrawMoveToReplayedFaith(float width)
{
  float place[3];
  float heading = 0.f;
  bool hasHeading = false;
  bool playing = m_Replay.GetPlayedFaith(place, heading, hasHeading);
  if (!playing)
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * 0.4f);
  if (ImGui::Button("Move camera to the replayed Faith", ImVec2(width, 0)) && playing)
  {
    if (!m_CameraEnabled)
      ToggleCamera();
    MoveToReplayedFaith();
  }
  if (!playing)
    ImGui::PopStyleVar();
  ImGui::ItemTooltip(playing ? "Puts the free camera at the replayed Faith's head, where she is in the replay now, "
    "turning it on if it's off." : "While a replay plays: puts the free camera at the replayed Faith's head.");
}

void CameraManager::MoveToPlayer()
{
  if (m_IgcsSession.IsActive()) return;

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

void CameraManager::PublishListener(bool active, double dt)
{
  // A jump this far in a frame (a pick, a track's cut, a teleport) has no
  // speed: it would pitch every sound for a frame
  const float ListenerJump = 20.f;

  int otherSpace = m_ListenerOtherSpace.load();
  if (otherSpace > 0 && m_ListenerOtherSpaceLogged == 0)
    util::log::Warning("The game puts its sound listener in another space here, so it's left where the game puts it");
  m_ListenerOtherSpaceLogged = otherSpace;

  ListenerPose pose{};
  pose.Active = active;
  if (active)
  {
    memcpy(pose.Transform, &m_Camera.Transform, sizeof(pose.Transform));
    XMFLOAT3 position(m_Camera.Transform._41, m_Camera.Transform._42, m_Camera.Transform._43);
    XMVECTOR moved = XMLoadFloat3(&position) - XMLoadFloat3(&m_ListenerLastPosition);
    float distance = XMVectorGetX(XMVector3Length(moved));
    if (m_ListenerWasActive && dt > 0 && distance < ListenerJump)
      XMStoreFloat4((XMFLOAT4*)pose.Velocity, XMVectorSetW(moved / (float)dt, 0.f));
    m_ListenerLastPosition = position;
  }
  if (active != m_ListenerWasActive)
    util::log::Write(active ? "Sounds are heard from the free camera" : "Sounds are heard from the game's camera again");
  bool ended = m_ListenerWasActive && !active;
  m_ListenerWasActive = active;

  while (m_ListenerLock.test_and_set(std::memory_order_acquire)) {}
  m_ListenerPose = pose;
  if (ended)
    m_ListenerStillCalls.store(2);
  m_ListenerLock.clear(std::memory_order_release);
}

// The world log (a diagnostic in util::hooks): where Faith is, every frame
// while it records, and the characters near her at its start and end
namespace
{
  // For the world log: the models within 40 m of Faith (looked for at the
  // start and every 10 s), and how far each moved and turned from where it
  // was first seen
  struct WorldMover
  {
    attach::Target Target;
    XMFLOAT3 Start;
    XMFLOAT3 StartForward;
    XMFLOAT3 Last;
    XMFLOAT3 FaithAtMove;
    float MaxMove;
    float MaxTurn;          // degrees
    double FirstMove, LastMove;
    bool Moved;
  };
  std::vector<WorldMover> g_WorldMovers;
  std::unordered_set<__int64> g_WorldMoverEntities;
  int g_WorldMoverScans = 0;
  const float WorldMoverRadius = 40.f;
  const size_t MaxWorldMovers = 8000;

  void AddWorldMovers(float const* faith, __int64 pSoldier)
  {
    attach::View view{};
    view.Camera = XMFLOAT3(faith[0], faith[1], faith[2]);
    view.Look = XMFLOAT3(0.f, 0.f, 1.f);
    view.Skip = pSoldier;
    std::vector<attach::Target> found(32768);
    std::unique_ptr<attach::Scan> scan = std::make_unique<attach::Scan>();
    int count = attach::ScanNearby(view, WorldMoverRadius, found.data(), (int)found.size(), *scan);
    for (int i = 0; i < count && g_WorldMovers.size() < MaxWorldMovers; ++i)
    {
      attach::Target& target = found[i];
      if (target.Agent >= 0 || target.Distance > WorldMoverRadius || attach::GroupOf(target) == attach::Group::People ||
          g_WorldMoverEntities.count(target.Entity))
        continue;
      XMFLOAT4X4 transform;
      if (!attach::ReadTransform(target, transform))
        continue;
      WorldMover mover{};
      mover.Target = target;
      mover.Start = mover.Last = XMFLOAT3(transform._41, transform._42, transform._43);
      mover.StartForward = XMFLOAT3(transform._31, transform._32, transform._33);
      g_WorldMovers.push_back(mover);
      g_WorldMoverEntities.insert(target.Entity);
    }
    ++g_WorldMoverScans;
  }

  void UpdateWorldMovers(double t, float const* faith)
  {
    for (WorldMover& mover : g_WorldMovers)
    {
      XMFLOAT4X4 transform;
      if (!attach::ReadTransform(mover.Target, transform))
        continue;
      XMFLOAT3 now(transform._41, transform._42, transform._43);
      float dx = now.x - mover.Start.x, dy = now.y - mover.Start.y, dz = now.z - mover.Start.z;
      float move = sqrtf(dx * dx + dy * dy + dz * dz);
      XMVECTOR f0 = XMVector3Normalize(XMLoadFloat3(&mover.StartForward));
      XMVECTOR f1 = XMVector3Normalize(XMVectorSet(transform._31, transform._32, transform._33, 0));
      float cosine = XMVectorGetX(XMVector3Dot(f0, f1));
      cosine = cosine > 1.f ? 1.f : (cosine < -1.f ? -1.f : cosine);
      float turn = acosf(cosine) * 57.29578f;
      bool stepped = fabsf(now.x - mover.Last.x) + fabsf(now.y - mover.Last.y) + fabsf(now.z - mover.Last.z) > 1e-4f;
      mover.Last = now;
      if (move > mover.MaxMove) mover.MaxMove = move;
      if (turn > mover.MaxTurn) mover.MaxTurn = turn;
      if ((move > 0.02f || turn > 1.f) && !mover.Moved)
      {
        mover.Moved = true;
        mover.FirstMove = t;
        if (faith)
          mover.FaithAtMove = XMFLOAT3(faith[0], faith[1], faith[2]);
      }
      if (mover.Moved && (stepped || turn > 1.f))
        mover.LastMove = t;
    }
  }

  void LogWorldMovers(double length)
  {
    std::vector<WorldMover*> moved;
    for (WorldMover& mover : g_WorldMovers)
      if (mover.Moved)
        moved.push_back(&mover);
    std::sort(moved.begin(), moved.end(), [](WorldMover const* a, WorldMover const* b) { return a->FirstMove < b->FirstMove; });
    util::log::Write("World log: %d models within %.0f m of Faith watched (%d looks), %d moved or turned:", (int)g_WorldMovers.size(),
      WorldMoverRadius, g_WorldMoverScans, (int)moved.size());
    int shown = 0;
    for (WorldMover const* mover : moved)
    {
      if (shown++ >= 150)
        break;
      attach::Target const& target = mover->Target;
      float fx = mover->FaithAtMove.x - mover->Start.x, fy = mover->FaithAtMove.y - mover->Start.y, fz = mover->FaithAtMove.z - mover->Start.z;
      bool always = mover->FirstMove < 0.5 && mover->LastMove > length - 0.5;
      util::log::Write("World log:   %s%s '%s' 0x%I64X at %.1f %.1f %.1f: moved %.2f m, turned %.0f deg, %.2f - %.2f s, Faith then "
        "%.1f %.1f %.1f (%.1f m away)", always ? "the whole time: " : "", attach::ClassName(target.Class), attach::LabelOf(target),
        target.Entity, mover->Start.x, mover->Start.y, mover->Start.z, mover->MaxMove, mover->MaxTurn, mover->FirstMove, mover->LastMove,
        mover->FaithAtMove.x, mover->FaithAtMove.y, mover->FaithAtMove.z, sqrtf(fx * fx + fy * fy + fz * fz));
    }
  }
}

void CameraManager::UpdateWorldLog()
{
  // Diagnostic (temporary): the effects the game plays near Faith (see the
  // hooks' hEffectPlay)
  {
    XMFLOAT4X4A player;
    bool there = GetPlayerTransform(player);
    float at[3] = { player._41, player._42, player._43 };
    util::hooks::UpdateEffectLog(there ? at : nullptr);
  }
  if (util::hooks::WorldLogRemaining() <= 0.0 && !m_WorldLogWasOn)
    return;
  double left = util::hooks::WorldLogRemaining();
  bool on = left > 0.0;
  const double length = 30.0;
  double t = length - left;
  XMFLOAT4X4A player;
  bool hasFaith = GetPlayerTransform(player);
  float faith[3] = { player._41, player._42, player._43 };
  bool fromFaith = false;
  __int64 pSoldier = GetPlayerSoldier();
  bool classes = pSoldier && attach::FindClasses(pSoldier, fromFaith);
  if (on && !m_WorldLogWasOn)
  {
    g_WorldMovers.clear();
    g_WorldMoverEntities.clear();
    g_WorldMoverScans = 0;
    if (hasFaith && classes)
      AddWorldMovers(faith, pSoldier);
  }
  else if (on && hasFaith && classes && t >= 10.0 * g_WorldMoverScans)
    AddWorldMovers(faith, pSoldier);
  if (on != m_WorldLogWasOn)
    LogCharacterPoses(hasFaith ? faith : nullptr, on ? "start" : "end");
  if (on || m_WorldLogWasOn)
    UpdateWorldMovers(on ? t : length, hasFaith ? faith : nullptr);
  bool ended = !on && m_WorldLogWasOn;
  m_WorldLogWasOn = on;
  util::hooks::UpdateWorldLog(hasFaith ? faith : nullptr);
  if (ended)
  {
    LogWorldMovers(length);
    g_WorldMovers.clear();
    g_WorldMoverEntities.clear();
  }
}

namespace
{
  bool ReadQwordSafe(__int64 address, __int64& value)
  {
    __try { value = *(__int64 const*)address; return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { value = 0; return false; }
  }

  // An object's getType result, 0 if it can't be read
  __int64 TypeInfoOf(__int64 object)
  {
    __try
    {
      __int64 vtable = *(__int64 const*)object;
      BYTE const* code = vtable > 0x10000 ? *(BYTE const**)vtable : nullptr;
      if (!code || code[0] != 0x48 || code[1] != 0x8D || code[2] != 0x05)
        return 0;
      return (__int64)code + 7 + *(int const*)(code + 3);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      return 0;
    }
  }
  // For the world log: where a character's ClientAntAnimatableEntity objects
  // are (pointers in its first 0x800 bytes whose getType is that class), each
  // with its pose handle ([[animatable + 0x158] + 0x118]) and pose manager
  // ([[animatable + 0x158] + 0x1B8]), and whether it has Faith's replay
  // recording component at + 0x4B0 (the replay's way to her body's pose)
  // Each read on its own, so one bad pointer doesn't end the look (the
  // first version read them all in one __try and found nothing)
  void ProbeCharacter(__int64 pEntity, const char* name, float distance)
  {
    __int64 base = (__int64)g_gameHandle;
    __int64 animatableType = base + 0x27EEE10;
    char found[600];
    size_t len = 0;
    found[0] = 0;
    for (int off = 0x08; off < 0x800 && len + 90 < sizeof(found); off += 8)
    {
      __int64 p = 0;
      if (!ReadQwordSafe(pEntity + off, p) || p < 0x10000 || (p & 7) || TypeInfoOf(p) != animatableType)
        continue;
      __int64 pose = 0, manager = 0, handle = -1;
      if (ReadQwordSafe(p + 0x158, pose) && pose > 0x10000)
      {
        ReadQwordSafe(pose + 0x1B8, manager);
        ReadQwordSafe(pose + 0x118, handle);
      }
      len += sprintf_s(found + len, sizeof(found) - len, " +0x%X (0x%I64X, handle 0x%X, manager 0x%I64X)", off, p,
        (unsigned int)handle, manager);
    }
    __int64 component = 0, vtable = 0;
    bool recording = ReadQwordSafe(pEntity + 0x4B0, component) && component > 0x10000 && ReadQwordSafe(component, vtable) &&
      vtable == base + 0x1C48CD0;
    util::log::Write("World log:   %s 0x%I64X, %.0f m: replay recording component %s; animatables at%s", name, pEntity, distance,
      recording ? "yes" : "no", len ? found : " none found");
  }
}

void CameraManager::LogCharacterPoses(float const* faith, const char* when)
{
  __int64 pSoldier = GetPlayerSoldier();
  bool fromFaith = false;
  if (!faith || !pSoldier || !attach::FindClasses(pSoldier, fromFaith))
  {
    util::log::Write("World log (%s): Faith or the character classes aren't there, no characters read", when);
    return;
  }
  util::log::Write("World log (%s): Faith and the characters within 60 m of her, and where their poses are:", when);
  ProbeCharacter(pSoldier, "Faith", 0.f);
  attach::View view{};
  view.Camera = XMFLOAT3(faith[0], faith[1], faith[2]);
  view.Look = XMFLOAT3(0.f, 0.f, 1.f);
  view.Skip = pSoldier;
  std::vector<attach::Target> found(8192);
  std::unique_ptr<attach::Scan> scan = std::make_unique<attach::Scan>();
  int count = attach::ScanNearby(view, 60.f, found.data(), (int)found.size(), *scan);
  int shown = 0;
  for (int i = 0; i < count && shown < 24; ++i)
  {
    attach::Target const& target = found[i];
    if (target.Agent >= 0 || attach::GroupOf(target) != attach::Group::People)
      continue;
    ++shown;
    ProbeCharacter(target.Entity, attach::ClassName(target.Class), target.Distance);
  }
  if (!shown)
    util::log::Write("World log:   no other characters found");
}

// The Faith sound log (a diagnostic in util::hooks): where she is, every frame
// while it records
void CameraManager::UpdateFaithSoundLog()
{
  if (util::hooks::FaithSoundLogRemaining() <= 0.0 && !m_SoundLogWasOn)
    return;
  m_SoundLogWasOn = util::hooks::FaithSoundLogRemaining() > 0.0;
  XMFLOAT4X4A player;
  bool hasFaith = GetPlayerTransform(player);
  float faith[3] = { player._41, player._42, player._43 };
  float camera[3] = { m_Camera.Transform._41, m_Camera.Transform._42, m_Camera.Transform._43 };
  util::hooks::UpdateFaithSoundLog(hasFaith ? faith : nullptr, m_CameraEnabled ? camera : nullptr);
}

bool CameraManager::GetListenerTransform(float const* pGameTransform, float* pTransform)
{
  float camera[16];
  while (m_ListenerLock.test_and_set(std::memory_order_acquire)) {}
  bool active = m_ListenerPose.Active;
  if (active)
    memcpy(camera, m_ListenerPose.Transform, sizeof(camera));
  m_ListenerLock.clear(std::memory_order_release);
  if (!active)
  {
    m_ListenerOverridden = false;
    return false;
  }

  // The game multiplies the camera's transform by another one when this is
  // set (0x1432C1350): the listener is in another space than the world's,
  // so it's left as the game puts it. Not found: likewise.
  if (!m_ppListenerSpace || *(__int64*)m_ppListenerSpace != 0)
  {
    ++m_ListenerOtherSpace;
    // From the free camera back to the game's: its next velocity would be the jump
    if (m_ListenerOverridden.exchange(false))
      m_ListenerStillCalls.store(2);
    return false;
  }
  m_ListenerOverridden = true;

  // What the game does with its camera's (0x1432C1350): the right and
  // backward rows turned around, the position as it is (checked live: the
  // listener is exactly that of the game camera). Their w's stay the game's.
  static const float Sign[4] = { -1.f, 1.f, -1.f, 1.f };
  for (int row = 0; row < 4; ++row)
  {
    for (int k = 0; k < 3; ++k)
      pTransform[row * 4 + k] = camera[row * 4 + k] * Sign[row];
    pTransform[row * 4 + 3] = pGameTransform[row * 4 + 3];
  }
  return true;
}

bool CameraManager::GetListenerVelocity(float* pVelocity)
{
  float velocity[4];
  while (m_ListenerLock.test_and_set(std::memory_order_acquire)) {}
  bool active = m_ListenerPose.Active;
  memcpy(velocity, m_ListenerPose.Velocity, sizeof(velocity));
  m_ListenerLock.clear(std::memory_order_release);
  // Only while the listener is at the free camera (the velocity is set just
  // before the transform, so that's the last frame's): from the first frame
  // the game's own is still right
  if (active && m_ListenerOverridden)
  {
    memcpy(pVelocity, velocity, sizeof(velocity));
    return true;
  }

  // Just after it ends the game's own is from the free camera back to its
  // camera in one frame: none instead
  int still = m_ListenerStillCalls.load();
  while (still > 0 && !m_ListenerStillCalls.compare_exchange_weak(still, still - 1)) {}
  if (still <= 0)
    return false;
  memset(pVelocity, 0, sizeof(float) * 4);
  return true;
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

// Faith's view mode (2026-11-08; MECMP found it, GridNode uses it, read in
// the game's code here). Her PamClientCharacterEntity has an interface at
// + 0x1D8 (vtable 0x141C3CBF0, 27 slots: the next vtable starts at
// 0x141C3CCC8) whose slot 15, 0x14390ED90, says how she's seen: 0 first
// person (her player, + 0x238 through the interface's + 0x60, is a real local
// one, + 0x1398 clear, with flag 4 at + 0x1520), else 1 third person. Nine
// places in the game ask it: which of her + 0x240 / + 0x248 it hands out
// (her box pose or her body, 0x14321F730), her mesh parts' first-person
// flags (0x14322B0D0, 0x14322B5D0), her culling box (4 m or 2 m round,
// 0x14390FE10; 1.5 m more in first person, 0x14390FF60), a first-person
// camera step (0x1432228A0), and three that pick first- or third-person data
// (0x142C6A3E6, 0x142C6F100, 0x143C4A656). Saying 1, the game draws her as
// it draws a third-person character, with nothing else changed. Done with a
// copy of that vtable for her interface alone, slot 15 answering 1; the
// game's own goes back when the camera's off. Both her pose objects stay the
// replay's (it overrides her box pose and her body), so a recording plays on
// her as before.
namespace
{
  const int ViewModeInterfaceOffset = 0x1D8;
  const __int64 ViewModeVTableRva = 0x1C3CBF0;
  const int ViewModeVTableSlots = 27;
  const int ViewModeSlot = 15;
  const __int64 GetViewModeRva = 0x390ED90;
  const int SoldierPlayerOffset = 0x238;       // PamClientCharacterEntity -> its ClientPlayer
  const int PlayerSoldierOffset = 0x14A0;      // ClientPlayer -> its character
  // The copy: the qword before the game's vtable first (nothing reads before
  // a vtable here, there's no RTTI, but it's the game's), then its 27 slots
  void* g_ViewModeVTable[1 + ViewModeVTableSlots];
  int __fastcall ViewModeThirdPerson(void*) { return 1; }

  bool IsViewModeCode(BYTE const* pBase)
  {
    // mov rax, [rcx + 0x60]; test rax, rax; je; cmp byte [rax + 0x1398], 0; jne; test byte [rax + 0x1520], 4; je;
    // xor eax, eax; ret; mov eax, 0xC390A20F; lea eax, [rax + 0x3C6F5DF2] (1); ret
    static const BYTE code[] = { 0x48, 0x8B, 0x41, 0x60, 0x48, 0x85, 0xC0, 0x74, 0x15, 0x80, 0xB8, 0x98, 0x13, 0x00, 0x00,
      0x00, 0x75, 0x0C, 0xF6, 0x80, 0x20, 0x15, 0x00, 0x00, 0x04, 0x74, 0x03, 0x31, 0xC0, 0xC3, 0xB8, 0x0F, 0xA2, 0x90, 0xC3,
      0x8D, 0x80, 0xF2, 0x5D, 0x6F, 0x3C, 0xC3 };
    return !memcmp(pBase + GetViewModeRva, code, sizeof(code)) &&
      *(__int64 const*)(pBase + ViewModeVTableRva + ViewModeSlot * 8) == (__int64)pBase + GetViewModeRva;
  }

  // The reads and writes, with nothing to unwind (SEH). What her interface
  // holds now (0: a fault)
  __int64 ViewModeVTableOf(__int64 soldier)
  {
    __try
    {
      return soldier < 0x10000 ? 0 : *(__int64 const*)(soldier + ViewModeInterfaceOffset);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      return 0;
    }
  }

  // Hers made to say third person: 1 done, 0 not the layout the tools know
  // (seen: what was there), -1 a fault
  int ViewModeSet(__int64 soldier, __int64& seen)
  {
    BYTE* pBase = (BYTE*)g_gameHandle;
    __int64 game = (__int64)pBase + ViewModeVTableRva;
    __int64 fake = (__int64)&g_ViewModeVTable[1];
    seen = 0;
    __try
    {
      if (soldier < 0x10000)
        return 0;
      __int64* pInterface = (__int64*)(soldier + ViewModeInterfaceOffset);
      seen = *pInterface;
      if (seen == fake)
        return 1;
      __int64 player = *(__int64 const*)(soldier + SoldierPlayerOffset);
      if (seen != game || player < 0x10000 || *(__int64 const*)(player + PlayerSoldierOffset) != soldier)
        return 0;
      if (!g_ViewModeVTable[1])
      {
        memcpy(g_ViewModeVTable, (void const*)(game - 8), sizeof(g_ViewModeVTable));
        g_ViewModeVTable[1 + ViewModeSlot] = (void*)&ViewModeThirdPerson;
      }
      *pInterface = fake;
      return 1;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      return -1;
    }
  }

  // The game's own back, while it's still the tools' copy there: true when it was
  bool ViewModeGiveBack(__int64 soldier)
  {
    __int64 game = (__int64)g_gameHandle + ViewModeVTableRva;
    __int64 fake = (__int64)&g_ViewModeVTable[1];
    __try
    {
      __int64* pInterface = (__int64*)(soldier + ViewModeInterfaceOffset);
      if (soldier < 0x10000 || *pInterface != fake)
        return false;
      *pInterface = game;
      return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      return false;
    }
  }
}

// Faith's body while the camera is on: the game's third-person body (her view
// mode, above), or her whole first-person body (the original tools' way,
// SetWholeFirstPersonBody) when that's chosen or her view mode isn't the code
// the tools know
void CameraManager::SetThirdPersonBody(bool enable)
{
  std::lock_guard<std::recursive_mutex> lock(m_BodyLock);
  if (enable)
  {
    if (m_BodyWayApplied >= 0)
      return;
    if (m_BodyWay == 0 && SetViewModeBody(true))
    {
      m_BodyWayApplied = 0;
      return;
    }
    SetWholeFirstPersonBody(true);
    m_BodyWayApplied = 1;
    util::log::Write("Faith's body: her whole first-person body (the original tools' way)");
    return;
  }
  if (m_BodyWayApplied == 0)
    SetViewModeBody(false);
  else if (m_BodyWayApplied == 1)
    SetWholeFirstPersonBody(false);
  m_BodyWayApplied = -1;
}

// Her view mode says third person (enable), or the game's again. False when
// it can't: no Faith, or not the code the tools know (said once, not when
// quiet: a Faith being made isn't linked to her player yet).
bool CameraManager::SetViewModeBody(bool enable, bool quiet)
{
  std::lock_guard<std::recursive_mutex> lock(m_BodyLock);
  __int64 soldier = GetPlayerSoldier();
  if (!enable)
  {
    // Only on the same Faith: one that's gone isn't written to
    __int64 was = m_BodySoldier;
    m_BodySoldier = 0;
    if (was && soldier == was && ViewModeGiveBack(was))
      util::log::Write("Faith's body: back to the game's first-person body");
    return true;
  }
  static bool s_known = IsViewModeCode((BYTE*)g_gameHandle);
  if (!s_known)
  {
    if (!m_BodyWayWarned)
      util::log::Warning("Faith's view mode isn't the code the tools know: her whole first-person body is shown instead");
    m_BodyWayWarned = true;
    return false;
  }
  __int64 seen = 0;
  int done = ViewModeSet(soldier, seen);
  if (done == 1)
  {
    if (m_BodySoldier != soldier)
      util::log::Write("Faith's body: the game's third-person body (her view mode says third person)");
    m_BodySoldier = soldier;
    return true;
  }
  if (quiet)
    return false;
  if (!m_BodyWayWarned && soldier)
    util::log::Warning("Faith's view mode isn't where the tools know it (%s 0x%I64X): her whole first-person body is "
      "shown instead", done < 0 ? "a fault reading it, at" : "vtable", done < 0 ? soldier : seen);
  m_BodyWayWarned = m_BodyWayWarned || soldier != 0;
  return false;
}

// Every frame while her body is shown the game's way: a new Faith (a respawn,
// a level load) shows it too, and so does hers if the game set it back
void CameraManager::KeepViewModeBody()
{
  std::unique_lock<std::recursive_mutex> lock(m_BodyLock, std::try_to_lock);
  if (!lock.owns_lock() || m_BodyWayApplied != 0)
    return;
  __int64 soldier = GetPlayerSoldier();
  if (soldier < 0x10000 || (soldier == m_BodySoldier && ViewModeVTableOf(soldier) == (__int64)&g_ViewModeVTable[1]))
    return;
  if (soldier != m_BodySoldier)
    m_BodySoldier = 0;   // the one before isn't written to again
  SetViewModeBody(true, true);
}

void CameraManager::SetWholeFirstPersonBody(bool enable)
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
  return m_TimeFrozen;
}

void CameraManager::ToggleTimeFreeze()
{
  if (!ReadPointer(m_ppGameTimeSettings)) return;

  // A freeze the tools find set (read on the first apply) counts too
  ApplyGameSpeed();
  m_TimeFrozen = !m_TimeFrozen;
  ApplyGameSpeed();
  util::log::Write("Time %s", m_TimeFrozen ? "frozen" : "unfrozen");
}

// The time scale (GameTimeSettings + 0x48: the game's 30 Hz sim ticks and
// its frame time go by it, 0 stops them) is the game's own value times Game
// speed, or 0 while frozen. The game writes it too: its slow motion
// (SlowMotionEntity, 0x1433362A0) eases it every frame, and its end puts 1.0
// back (0x143332D10). So a value the tools didn't write is the game's, kept
// as the base: its slow motion plays on, times the speed, and the speed
// outlasts it. At 1x and not frozen the game's value is left alone.
float CameraManager::GameSpeedNow()
{
  if (m_CameraEnabled && m_TrackPlayer.IsPlaying() && m_TrackPlayer.IsGameSpeedLocked() && m_TrackGameSpeed > 0.f)
    return m_TrackGameSpeed;
  return m_GameSpeed;
}

void CameraManager::ApplyGameSpeed()
{
  __int64 pTimeSettings = ReadPointer(m_ppGameTimeSettings);
  if (!pTimeSettings) return;
  float* pScale = (float*)(pTimeSettings + TimeScaleOffset);
  float now = *pScale;
  float speed = GameSpeedNow();

  float base = m_GameTimeScale;
  if (!m_TimeScaleRead)
  {
    // A 0 at the start is a freeze left set: the game's own slow motion never
    // goes under 0.0001
    m_TimeScaleRead = true;
    m_TimeFrozen = now == 0.f;
    base = now > 0.f ? now : 1.f;
  }
  else if (!m_TimeScaleWritten || now != m_WrittenTimeScale)
    base = now > 0.f ? now : 1.f;
  if ((base != 1.f) != (m_GameTimeScale != 1.f) && (m_TimeFrozen || speed != 1.f))
    util::log::Write("Game speed: the game's own slow motion %s (%.2fx)", base != 1.f ? "started" : "ended", base);
  m_GameTimeScale = base;

  // The billboards' videos go by the game's time while the speed is the tools'
  util::hooks::SetFeedsAtGameSpeed(!m_TimeFrozen && speed != 1.f);
  // A replay playing goes at it too (not the game's own slow motion: a
  // recording made in it has it already)
  m_Replay.SetPlaySpeed(!m_ReplayAtGameSpeed ? 1.0 : (m_TimeFrozen ? 0.0 : speed));

  if (!m_TimeFrozen && speed == 1.f)
  {
    // The game's own value back, once
    if (m_TimeScaleWritten)
    {
      if (now != m_GameTimeScale)
        *pScale = m_GameTimeScale;
      util::log::Write("Game speed: 1x, the game's time scale is its own (%.2f)", m_GameTimeScale);
    }
    m_TimeScaleWritten = false;
    return;
  }

  float want = m_TimeFrozen ? 0.f : m_GameTimeScale * speed;
  if (!m_TimeScaleWritten && !m_TimeFrozen)
    util::log::Write("Game speed: %.2fx (the game's own time scale %.2f)%s", speed, m_GameTimeScale,
      speed != m_GameSpeed ? ", from the camera track" : "");
  if (now != want)
    *pScale = want;
  m_WrittenTimeScale = want;
  m_TimeScaleWritten = true;
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

// Faith dead or not spawned yet (a level load, a respawn): the tools let go
// of every zone they ask for (the camera's, the Always districts', and the
// Never districts are let back) until she has been back for a few seconds. A
// respawn with the whole city held waited for ever on its loading screen
// (seen 2026-10-22: Faith's soldier 0, every zone loaded, the game busy). Zone
// streamer thread only.
bool CameraManager::StreamingPausedForFaith()
{
  ULONGLONG now = GetTickCount64();
  if (!GetPlayerSoldier())
  {
    if (!m_StreamPausedForFaith)
    {
      m_StreamPausedForFaith = true;
      util::log::Write("Faith isn't spawned (dead or loading): the tools let go of the zones they ask for until she's back");
    }
    m_FaithBackAt = 0;
    return true;
  }
  if (!m_StreamPausedForFaith)
    return false;
  if (m_FaithBackAt == 0)
    m_FaithBackAt = now;
  if (now - m_FaithBackAt < FaithBackDelayMs)
    return true;
  m_StreamPausedForFaith = false;
  util::log::Write("Faith is back: the tools ask for their zones again");
  return false;
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
