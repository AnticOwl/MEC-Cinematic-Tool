#include "GroundLevel.h"
#include "CameraCollision.h"
#include "../Main.h"
#include "../Util/Util.h"
#include "../Util/PackedResource.h"
#include "../resource.h"
#include <Windows.h>
#include <intrin.h>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <memory>
#include <unordered_map>
#include <vector>

#pragma intrinsic(_ReturnAddress)

namespace
{
  // The game's code (offline, 2026-11-01; Research_Ground_Level_2026-11-01.md sections 2 and 4)

  // The kill volumes' inside test, 0x143970D30(entity, character) -> bool. Only their per-frame check
  // (0x143972370) calls it: at 0x143972400 for the player, at 0x14397247F for the other characters.
  // PamServerKillVolumeEntity: vtable 0x141C50698, + 0x28 its data (PamKillVolumeEntityData: + 0x10 Flags,
  // + 0x18 DamageType).
  const __int64 KillInsideRva = 0x3970D30;
  const __int64 KillInsidePlayerCallRva = 0x3972400;
  const __int64 KillInsideOthersCallRva = 0x397247F;
  const __int64 KillVolumeVTableRva = 0x1C50698;
  const __int64 KillVolumePropertyRva = 0x39726C0;   // its slot 2, the property handler (Enabled)
  // Ant's RayCastCollisionProcessorAsset (vtable 0x141A62EE0): its run, slot 4, 0x142CE8200(asset, info), reads
  // FallThreshold (+ 0x40) at 0x142CE8632 for HasFallenPastThreshold.
  const __int64 FallRunRva = 0x2CE8200;
  const __int64 FallAssetVTableRva = 0x1A62EE0;
  const __int64 FallThresholdReadRva = 0x2CE8632;
  // A Havok hit turned into the game's ids, 0x1435177B0(physics world, flags, hit, out): hit + 0x40 its body,
  // the body's user data (+ 0x88) the game's id, the low 16 bits its record in [world + 0x130].
  const __int64 HitBodyRva = 0x35177B0;
  // The physics world's flush of its deferred work, 0x1434F4C80(physics world): its records' changes, then
  // hknpWorld::destroyBodies at 0x1434F4E91 for the bodies it put off destroying.
  const __int64 PhysicsFlushRva = 0x34F4C80;
  const __int64 PhysicsFlushDestroyCallRva = 0x34F4E91;
  // Havok (hknp, 2013.3)
  const __int64 BuildConfigRva = 0x3F2E490;       // hknpConvexShape::BuildConfig's constructor (0x2C bytes)
  const __int64 BoxShapeRva = 0x3F2F140;          // a box from half extents (half, convex radius, config)
  const __int64 BodyCinfoRva = 0x3F38A70;         // hknpBodyCinfo's constructor (0x60 bytes)
  const __int64 StringPtrResetRva = 0x3CD1F70;    // hkStringPtr's destructor (the cinfo's name at + 0x20)
  const __int64 CreateBodyRva = 0x3F41880;        // hknpWorld::createBody(world, &id, cinfo, flags): bit 0 don't add
  const __int64 CreateBodyAddCallRva = 0x3F41A38; // its call of addBodies when bit 0 is clear
  const __int64 AddBodiesRva = 0x3F41A80;         // hknpWorld::addBodies(world, ids, count, flags)
  const __int64 DestroyBodiesRva = 0x3F423E0;     // hknpWorld::destroyBodies(world, ids, count, 1)
  const __int64 SetBodyTransformRva = 0x3F3A6B0;  // hknpWorld::setBodyTransform(world, id, rows, 1)
  // A body's physics record (vtable 0x141BAC160); a hit's material comes from its slot 89, 0x1434F0D00:
  // [[[record + 0xD0] + 0x18] + 0x2C][index], or 0 with no + 0xD0.
  const __int64 BodyRecordVTableRva = 0x1BAC160;
  const __int64 RecordMaterialRva = 0x34F0D00;

  // The game's physics world (the flush's and the hit's first argument)
  const int WorldHavokOffset = 0xE0;              // its hknpWorld
  const int WorldRecordsOffset = 0x130;           // its records, by the low 16 bits of a body's user data
  // hknpWorld
  const int HavokBodiesOffset = 0x20;             // the bodies, 0x90 bytes each
  const int HavokCapacityOffset = 0x2C;           // how many there's room for (low 30 bits)
  const int HavokBodyCountOffset = 0x68;          // how many there are (its body manager, + 0x10: + 0x58; Hooks.cpp)
  const int BodySize = 0x90;
  const int BodyShapeOffset = 0x50;
  const int BodyFilterOffset = 0x5C;
  const int BodyIdOffset = 0x60;
  const int BodyMotionOffset = 0x68;              // 0: static
  const int BodyMaterialOffset = 0x6C;
  const int BodyQualityOffset = 0x6E;
  const int BodyUserDataOffset = 0x88;
  // A record
  const int RecordOwnerOffset = 0x30;             // its physics world ([+ 0xE0] the hknpWorld)
  const int RecordBodyOffset = 0x58;              // its Havok body
  const int RecordMaterialsOffset = 0xD0;
  // hknpBodyCinfo
  const int CinfoShapeOffset = 0x00;
  const int CinfoMotionOffset = 0x0C;
  const int CinfoQualityOffset = 0x10;
  const int CinfoMaterialOffset = 0x12;
  const int CinfoFilterOffset = 0x14;
  const int CinfoNameOffset = 0x20;
  const int CinfoPositionOffset = 0x30;
  // An hknpShape: + 0x10 its flags (bit 2: a hit gives the shape key, not + 0x18), + 0x18 its user data
  const int ShapeFlagsOffset = 0x10;
  const int ShapeUserDataOffset = 0x18;

  // The 19 street kill floors, by their data's Flags (each unique among the 100 kill volumes in the game's data; every
  // ebx parsed, 2026-11-02: the first list of 90 came from the files whose header named the type, and missed
  // downtownarea4 and 5's): a VolumeVectorShapeData at y -30.7 - 10.8 over 5,000 m2 or more. Release's third
  // warehouse (0x71CC3713, a floor at y 7.7 inside a building) is left out.
  const unsigned StreetFloors[] = {
    0x616AC382, // anchorarea1, y 0, 20 m
    0x61A7F3EB, // anchorarea2, y 0, 27 m
    0x6001740D, // anchorarea3, y -10.6, 20 m
    0x60F99671, // anchorarea3, y 0.7, 61 m
    0x6155A694, // construction, y 9.3, 4 m
    0x61D82966, // downtownarea1, y 0, 5 m
    0x61883FEF, // downtownarea2
    0x601656C3, // downtownarea3
    0x6143F6A6, // downtownarea4, y 0, 5 m, x -291 - -54, z 107 - 258 (Faith died in it 2026-11-02 05:40)
    0x61B863F1, // downtownarea5, y 0, 5 m, x -371 - -135, z -52 - 142 (Faith died in it 2026-11-02 05:42)
    0x6066B05C, // downtownarea6
    0x60C3806C, // downtownarea7
    0x606F0E71, // downtownarea8
    0x60477767, // release (layer3_release_flow), y -0.9, 4 m
    0x61E139E0, // theviewarea1, y 10.8, 5 m
    0x61D10D34, // theviewarea2, y -3.8
    0x6124258D, // theviewarea3, y 2.4
    0x60699B8F, // theviewarea4, y -30.7
    0x606865F5, // trainstation, y 0, 19 m
  };
  const int StreetFloorCount = sizeof(StreetFloors) / sizeof(StreetFloors[0]);
  const unsigned KillDamageType = 104;
  // The levels' other kill volumes of that DamageType: OBBData boxes and floors placed by the level files, none in a
  // prefab (out of bounds and mission zones). 2026-11-01 23:50:30 Faith died in 0x607E7FBE, The View's two boxes from
  // y 0 to 61 over the marina's quay. They spare her while Solid street is on and the street is under her (within
  // OverStreetMax): high ones (the Shard's) and those under the streets (the Kingdom's) kill as before. The prefabs'
  // kill volumes (fans, the monorail train, lifts, vents, grid nodes; the same DamageType) are never spared. With the
  // street floors that's all 48 level files' volumes of the DamageType (every ebx parsed, 2026-11-02); the 49th,
  // Vive la Resistance's round building (0x7048928D, DamageType 103, a box at y 121), is left as the game has it.
  const unsigned LevelKillZones[] = {
    0x60078C16, // anchorarea3, y 543
    0x60235660, // benefactor, a box centred at y 128 (half height 12.5)
    0x60321A16, // anchorarea3, y 46 - 55
    0x6037AE7D, // familymatters (after the museum), a box centred at y 16.1 (half height 1)
    0x6045F0E8, // familymatters, y 22 - 29
    0x605E2BE3, // kingdom, y -154
    0x6071CA5E, // downtown, y 5 - 7, 300 x 82 m
    0x60751A54, // theshard, y 172 - 407
    0x607E7FBE, // theviewarea3, y 0 - 61, the marina's quay
    0x6094ADE1, // downtownarea1, y 25 - 27
    0x6097BC6C, // theviewarea2, y 23 - 28
    0x60D8766C, // construction, y -77 - -34
    0x60EA10D1, // anchorarea1, a box centred at y 60.3 (half height 10)
    0x611D11A9, // anchorarea1, a box centred at y 39.2 (half height 9)
    0x6128B8D8, // anchorarea1, two floors at y 60.9, 4 and 5 m
    0x614D0216, // theshard, y 1490
    0x615041E6, // downtownarea6, y 42 - 46
    0x6175F402, // rezoningcityalert, y 57 - 60
    0x618F492E, // downtownarea5, a box centred at (-229.5, 2.5, 102.9) (half sizes 1.4 x 2.5 x 1.3 m)
    0x61A9C07B, // anchorarea3, y 38 - 48
    0x61BE78DB, // familymatters (escape), y 210 - 213
    0x61C2F60D, // kingdom, y -192
    0x61CDAF54, // anchorarea1, a box centred at y 39.2 (half height 9), 111 m long
    0x61DCFA0C, // anchorarea2, y 45 - 49
    0x70FE57A6, // droneworks, y 0 - 52
    0x71283159, // kingdom, y -201 - -134
    0x71428715, // savantextraordinaireallcom, y 141 - 158
    0x71CC3713, // release, the third warehouse's floor, y 7.7
    0x71F739E6, // savantextraordinaireallcom, y 111 - 127
  };
  const int LevelKillZoneCount = sizeof(LevelKillZones) / sizeof(LevelKillZones[0]);
  static_assert(sizeof(StreetFloors) / sizeof(StreetFloors[0]) <= 64 && LevelKillZoneCount <= 64,
    "a bit each in g_FloorsSpared / g_ZonesSpared");
  const float OverStreetMax = 80.f;               // the street this far under her feet at most: she's over it
  // The Sky City train station at the Shard's foot (zone 55; the user, 2026-11-09: "make the shard mall floor solid and
  // remove the death zones from there"; Faith died at its west end at 20:46:26 in 0x60B8849B). Its own prefab
  // (pf_skycity_trainstation_nongroupable_autogen_gen4, placed once, by zone 55's layer) has three kill boxes, all this
  // one Flags: over its platform landing and upper stair (y 148.8 - 152.1), over its lower landing and the forecourt
  // west of it (138.6 - 142.6) and under the track north-west of it (123.9 - 130.7); spared for Faith while Solid street
  // is on. The Shard's level (theshard) has a box over the station's east roof (y 172 - 175) in 0x60751A54, whose
  // other box is up the tower (y 400 - 407): spared while she's in the station's area (SkyCityArea: x0, z0, x1, z1, at
  // y SkyCityY0 - SkyCityY1: the station's height, not the tower's box over it)
  const unsigned SkyCityStationKills = 0x60B8849B;
  const unsigned SkyCityShardKills = 0x60751A54;
  const float SkyCityArea[4] = { 560.f, -3100.f, 840.f, -2780.f };
  const float SkyCityY0 = 100.f, SkyCityY1 = 200.f;
  // The station's sunken part (2026-11-10, block/'s deaths research): its lowest floor and pools, opened by the
  // tools' reshape of the base block (skycity namespace below), its stairs, platform, upper floor and roof, in the
  // station's frame (u along its right, v along its forward, from StationOrigin: u0, v0, u1, v1) and world y
  // (StationPitY0 - Y1). The drop from its upper floor to the lowest is 10.5 - 12.9 m, from the platform 14.7 - 15.1
  // (the fall check's FallThreshold is 10 m by default; the game's data's value isn't known): while Solid street is on
  // and her feet are in it, no fall death, as the tick does. Its bottom 2 m under the lowest floor: a fall that goes
  // under it or out through its sides gets the threshold back and ends as the game has it.
  const float StationOrigin[2] = { 584.991f, -3012.527f };
  const float StationRight[2] = { 0.95654f, 0.29159f };       // x, z
  const float StationPit[4] = { 102.f, -10.f, 247.f, 95.f };
  const float StationPitY0 = 133.4f, StationPitY1 = 176.f;

  // The surfaces of the tools' bodies, for footsteps, landings, slides and hit effects: a hit on one gets its
  // surface's MaterialDecl (the hit conversion 0x1435177B0 writes the decl at out + 8 when the query's flags have bit
  // 0; hHitBody puts this one there after it). Byte 2 is the city material grid's map index, which picks the sound
  // (MaterialSoundId) and the effects; byte 3 the physics properties; 0x8000 "set". Each is a decl the city's own
  // collision uses (staticmodelgroup_physics_win32, 2026-11-02). The data files give each road triangle and prop box
  // one of these (1 - SurfaceCount; the road sections' and prop meshes' materials sorted offline, groundgen2.py).
  const unsigned SurfaceDecls[] = {
    0,
    0x12608000, // 1 tarmac: map 96, sound id 6 (me_footstep_tarmac_*): the roads and their markings
    0x12078000, // 2 concrete: map 7, sound id 1; the street set's kerbs (curbstreet_*): pavements, plazas, decks
    0x12108000, // 3 water: map 16, sound id 60; The View's water colliders
    0x12028000, // 4 metal: map 2, sound id 20; signs, lights, construction fences: signs, lamps, cars, bollards
    0x123F8000, // 5 tarp: map 63, sound id 32; the tarps: parasols' tops, site fences
    0x5E118001, // 6 vegetation: map 17; shrubs and trees: kerb plants
    0x12278000, // 7 bin: map 39, sound id 27; trashcan_01
    0x12328000, // 8 concrete pillar: map 50, sound id 3; concpillar01: the highway pillars
    0x120B8000, // 9 wood: map 11, sound id 50 (5,207 of the city's parts): wood sections, crates, boardwalks
  };
  const unsigned SurfaceCount = sizeof(SurfaceDecls) / sizeof(SurfaceDecls[0]) - 1;

  // A prop box's group (its surface byte's high 4 bits): 0 always made; GroupBaubleRuins only while the Bauble's ruins
  // are the version the game streams (their BaubleRuins trigger on), GroupBaubleMall only while the whole mall is
  // (BaubleMall): each one's boxes would stand inside the other. Update's thread only
  const unsigned char GroupBaubleRuins = 1;
  const unsigned char GroupBaubleMall = 2;
  const unsigned GroupCount = 2;
  bool g_GroupOn[GroupCount + 1] = { true, false, false };

  // The ground: boxes whose tops lie on the road surface, on a 2 m grid (split into 1 m and 0.5 m squares where one
  // plane doesn't fit the road: kerbs, holes), each fixed where it's made. The one floor box of the builds before
  // moved with her and was raised into her going down a slope and up another (2026-11-01, she fell through); a box
  // made round her feet doesn't hold her either (21:20, coming up stairs). So a tile never moves, and one near her
  // is made only with its top at or under her feet.
  const float TileCell = 2.f;                     // the grid's squares (level 0); level 1 is 1 m, level 2 0.5 m
  const int TileMaxLevel = 2;
  const float GridStep = 0.5f;                    // the smallest square's side: keys and the cover map count in it
  const float TileHalfY = 0.5f;                   // 1 m thick
  const float TileRadius = 8.f;                   // tiles are planned this near her (to their square)
  const float TileKeepRadius = 10.f;              // and kept this near
  const float FitTolerance = 0.06f;               // the road at a square's corners this close to one plane: one tile
  const float CornerInset = 0.45f;                // its corners tested this part of its side from its middle
  // Each point of a square takes the highest road surface no higher than her feet + ReachBase + ReachPerMetre a
  // metre from her (up to ReachMax): the street under a bridge she's under, the bridge when she's on it. Per metre
  // as steep as the streets (0.8: 39 degrees; 0.35 left the squares just uphill of her on a street steeper than
  // 19 degrees without tiles until she was at them: the offline test of 2026-11-01, falls between two slopes)
  const float ReachBase = 0.5f;
  const float ReachPerMetre = 0.8f;
  const float ReachMax = 2.5f;
  const float NearDistance = 0.75f;               // a tile or prop this close to her: her capsule and a frame or two
  const float MakeTolerance = 0.03f;              // a new tile near her: its top no more than this over her feet
  const float LeadDistance = 0.3f;                //   anywhere within this of her (a frame or two at a sprint)
  // Over a square whose road is up to LowerMax over her feet, with no tile under her: a level tile at her feet
  // (LoweredBelow under them) instead (the top of stairs a little short of the street, real ground a little under
  // the road drawn; up to a step she can take up onto the road's own tiles)
  const float LowerMax = 0.4f;
  const float LoweredBelow = 0.01f;
  const float BelowLimit = 0.25f;                 // her feet this far under the top of the tile she's over: let go
  // A tile within this of her middle holds her when its top is (her capsule, 0.35 m round, rests on its edge within
  // ~10 cm); also how far beside her the level tile at her feet can go (a ragged road edge)
  const float HeldMargin = 0.25f;
  const float HeldBelow = 0.3f;                   //   no more than this under her feet
  const float MaxTiltCos = 0.82f;                 // steeper than ~35 degrees: the tile stays level
  // The props: made this near her, kept this near (across); over or under her feet no more than this
  const float PropRadius = 40.f;
  const float PropKeepRadius = 50.f;
  const float PropReachY = 40.f;
  const float PropKeepY = 50.f;
  const size_t MaxPropItems = 3000;               // the nearest; a dense plaza or roof has ~1 box a m2
  const float PropHeight = 1.9f;                  // her height, for not making a prop round her
  const float ConvexRadius = 0.01f;               // the game's for its boxes (0x143516556)
  const int MaxMakePerFlush = 32;
  // Room for bodies always left to the game: with its buffer full, Havok's createBody writes the body past it and
  // the game crashes (Research_Havok_2026-10-25 section 4; the tools give each world room for 65536)
  const unsigned BodyReserve = 4096;
  const int MaxTrackedId = 1 << 20;               // body ids the hit hook can tell are the tools'
  const float NoFallThreshold = 1e9f;
  const ULONGLONG ProbeEveryMs = 2000;
  const ULONGLONG GoneWorldMs = 3000;             // a world not flushed for this long is taken as gone
  const ULONGLONG RepublishMs = 100;              // a plan not all made yet is handed over again this often
  const ULONGLONG PlanMemoMs = 250;               // a plan taken again (PlanMemo) is made anew at least this often
  const ULONGLONG FaithGoneMs = 1500;             // Faith not there for this long (a load, a respawn): all let go
  const float NearMadeRadius = 8.f;               // g_PlanAllMade: the plan's items this near her made

  typedef bool(__fastcall* tKillInside)(__int64, __int64);
  typedef __int64(__fastcall* tFallRun)(__int64, __int64);
  typedef __int64(__fastcall* tHitBody)(__int64, __int64, __int64, __int64);
  typedef __int64(__fastcall* tPhysicsFlush)(__int64);
  typedef __int64(__fastcall* tBuildConfig)(void*);
  typedef __int64(__fastcall* tBoxShape)(float const*, float, void*);
  typedef __int64(__fastcall* tBodyCinfo)(void*);
  typedef void(__fastcall* tStringPtrReset)(void*);
  typedef int*(__fastcall* tCreateBody)(__int64, int*, void*, unsigned char);
  typedef void(__fastcall* tAddBodies)(__int64, int const*, int, unsigned char);
  typedef void(__fastcall* tDestroyBodies)(__int64, int const*, int, int);
  typedef void(__fastcall* tSetBodyTransform)(__int64, unsigned int, float const*, int);

  tKillInside oKillInside = nullptr;
  tFallRun oFallRun = nullptr;
  tHitBody oHitBody = nullptr;
  tPhysicsFlush oPhysicsFlush = nullptr;

  bool g_Available = false;
  SRWLOCK g_FeetLock = SRWLOCK_INIT;      // Faith's feet as UpdateGates last had them (LastFeet)
  float g_LastFeet[3] = {};
  ULONGLONG g_LastFeetAt = 0;
  std::atomic<bool> g_Broken{ false };          // a Havok call faulted: the tools' bodies stay off until the game restarts
  std::atomic<bool> g_SolidStreet{ false };
  std::atomic<bool> g_NoFallDeath{ false };
  // Faith falling far below every street (under VoidY): the fall check works as the game has it, so a fall through a
  // gap ends instead of going on for ever
  std::atomic<bool> g_InVoid{ false };
  const float VoidY = -100.f;
  std::atomic<bool> g_OverStreet{ false };      // the road data has a surface under her feet (OverStreetMax)
  std::atomic<bool> g_AtSkyCity{ false };       // her feet in SkyCityArea
  std::atomic<bool> g_InStationPit{ false };    // her feet in StationPit
  // The Sky City reshape (skycity namespace): something the tools changed in zone 55's collision is changed (set by the
  // flush that makes it, before the camera hook hears): with Solid street off (Faith is in the space it opened) Street
  // level keeps its ground and props till it's back
  std::atomic<bool> g_SkyHolding{ false };
  // Street level on: Solid street, or held on by the Sky City reshape (the bodies, the kill zones spared)
  bool StreetOn()
  {
    return g_SolidStreet.load(std::memory_order_relaxed) || g_SkyHolding.load(std::memory_order_relaxed);
  }
  // Street level's plan round her handed over with all of it made (each frame's), since when (0: not)
  std::atomic<bool> g_PlanAllMade{ false };
  ULONGLONG g_PlanAllMadeSince = 0;
  // The props data has the Sky City reshape's stand-ins (skycity::DataProbes): 1 the base block's, 2 the hull's
  int g_SkyDataHas = 0;
  // The tools' bodies in the game's physics, for the hit hook: how many, their hknpWorld, each id's surface
  // (SurfaceDecls; 0: not one of the tools')
  std::atomic<int> g_BodyCount{ 0 };
  std::atomic<__int64> g_BodiesHavok{ 0 };
  std::atomic<unsigned char> g_OurSurface[MaxTrackedId];

  __int64 Base()
  {
    return (__int64)g_gameHandle;
  }

  bool CallsAt(BYTE const* base, __int64 callRva, __int64 targetRva)
  {
    return base[callRva] == 0xE8 && callRva + 5 + *(int const*)(base + callRva + 1) == targetRva;
  }

  // The bytes each function starts with, as read from the game's image
  bool CodeKnown()
  {
    BYTE const* base = (BYTE const*)g_gameHandle;
    // push rbx; sub rsp, 0xB0; mov rax, [rdx]
    static const BYTE killInside[] = { 0x53, 0x48, 0x81, 0xEC, 0xB0, 0x00, 0x00, 0x00, 0x48, 0x8B, 0x02 };
    // after the player's call: test al, al; je; mov rax, [rsi + 0x28]; ...; mov edx, [rax + 0x18] (DamageType)
    static const BYTE killPlayerAfter[] = { 0x84, 0xC0, 0x74, 0x14, 0x48, 0x8B, 0x46, 0x28, 0x4C, 0x8B, 0x03, 0x48, 0x89,
      0xD9, 0x8B, 0x50, 0x18 };
    // push rbp; push rsi; push r15; lea rbp, [rsp - 0x47]; sub rsp, 0x100
    static const BYTE fallRun[] = { 0x55, 0x56, 0x41, 0x57, 0x48, 0x8D, 0x6C, 0x24, 0xB9, 0x48, 0x81, 0xEC, 0x00, 0x01,
      0x00, 0x00 };
    // movss xmm0, [rsi + 0x40]; mov rcx, [rsi + 0x38]
    static const BYTE fallRead[] = { 0xF3, 0x0F, 0x10, 0x46, 0x40, 0x48, 0x8B, 0x4E, 0x38 };
    // mov [rsp + 0x18], rbx; push rbp; push rsi; push rdi; push r14; push r15; sub rsp, 0x20; movsxd rax, [r8 + 0x40];
    // mov r14, rcx; mov rsi, rdx; lea r11, [rax + rax * 8]; mov rax, [rcx + 0xE0]; mov rbx, r9; shl r11, 4;
    // mov r15, r8; add r11, [rax + 0x20]; mov r10, [r11 + 0x88]
    static const BYTE hitBody[] = { 0x48, 0x89, 0x5C, 0x24, 0x18, 0x55, 0x56, 0x57, 0x41, 0x56, 0x41, 0x57, 0x48, 0x83,
      0xEC, 0x20, 0x49, 0x63, 0x40, 0x40, 0x49, 0x89, 0xCE, 0x48, 0x89, 0xD6, 0x4C, 0x8D, 0x1C, 0xC0, 0x48, 0x8B, 0x81,
      0xE0, 0x00, 0x00, 0x00, 0x4C, 0x89, 0xCB, 0x49, 0xC1, 0xE3, 0x04, 0x4D, 0x89, 0xC7, 0x4C, 0x03, 0x58, 0x20, 0x4D,
      0x8B, 0x93, 0x88, 0x00 };
    // mov rax, rsp; push rbp; sub rsp, 0x70; mov rbp, [rcx + 0x1D0]
    static const BYTE flush[] = { 0x48, 0x89, 0xE0, 0x55, 0x48, 0x83, 0xEC, 0x70, 0x48, 0x8B, 0xA9, 0xD0, 0x01, 0x00, 0x00 };
    static const BYTE buildConfig[] = { 0x48, 0x83, 0xEC, 0x18, 0xF3, 0x0F, 0x10, 0x05, 0xE8, 0x39, 0xAD, 0xFD };
    static const BYTE boxShape[] = { 0x48, 0x83, 0xEC, 0x48, 0x0F, 0x28, 0x19, 0x0F, 0x57, 0xC0 };
    static const BYTE bodyCinfo[] = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x89, 0xCB };
    static const BYTE stringReset[] = { 0x41, 0x83, 0xC8, 0xFF, 0x31, 0xD2, 0xE9 };
    static const BYTE createBody[] = { 0x44, 0x88, 0x4C, 0x24, 0x20, 0x4C, 0x89, 0x44, 0x24, 0x18, 0x55, 0x41, 0x54 };
    // in createBody: jne (bit 0 set: not added); mov r8d, 1; mov rdx, r12; mov rcx, rbp; call addBodies
    static const BYTE createBodyAdd[] = { 0x75, 0x11, 0x41, 0xB8, 0x01, 0x00, 0x00, 0x00, 0x4C, 0x89, 0xE2, 0x48, 0x89, 0xE9 };
    // test r8d, r8d; jle; mov rax, rsp; mov [rax + 0x20], r9b
    static const BYTE addBodies[] = { 0x45, 0x85, 0xC0, 0x0F, 0x8E, 0xB1, 0x05, 0x00, 0x00, 0x48, 0x89, 0xE0, 0x44, 0x88, 0x48,
      0x20 };
    static const BYTE destroyBodies[] = { 0x48, 0x89, 0x54, 0x24, 0x10, 0x53, 0x55, 0x57, 0x41, 0x55, 0x41, 0x56 };
    static const BYTE setBodyTransform[] = { 0x89, 0x54, 0x24, 0x10, 0x55, 0x53, 0x56, 0x57, 0x41, 0x57, 0x48, 0x89, 0xE5 };
    // mov rax, [rcx + 0xD0]; test rax, rax; je; mov rax, [rax + 0x18]; mov ecx, r9d; mov rax, [rax + 0x2C]
    static const BYTE recordMaterial[] = { 0x48, 0x8B, 0x81, 0xD0, 0x00, 0x00, 0x00, 0x48, 0x85, 0xC0, 0x74, 0x14, 0x48,
      0x8B, 0x40, 0x18, 0x44, 0x89, 0xC9, 0x48, 0x8B, 0x40, 0x2C };
    return !memcmp(base + KillInsideRva, killInside, sizeof(killInside)) &&
      CallsAt(base, KillInsidePlayerCallRva, KillInsideRva) && CallsAt(base, KillInsideOthersCallRva, KillInsideRva) &&
      !memcmp(base + KillInsidePlayerCallRva + 5, killPlayerAfter, sizeof(killPlayerAfter)) &&
      !memcmp(base + FallRunRva, fallRun, sizeof(fallRun)) &&
      !memcmp(base + FallThresholdReadRva, fallRead, sizeof(fallRead)) &&
      *(__int64 const*)(base + FallAssetVTableRva + 4 * 8) == (__int64)base + FallRunRva &&
      !memcmp(base + HitBodyRva, hitBody, sizeof(hitBody)) &&
      !memcmp(base + PhysicsFlushRva, flush, sizeof(flush)) &&
      CallsAt(base, PhysicsFlushDestroyCallRva, DestroyBodiesRva) &&
      !memcmp(base + BuildConfigRva, buildConfig, sizeof(buildConfig)) &&
      !memcmp(base + BoxShapeRva, boxShape, sizeof(boxShape)) &&
      !memcmp(base + BodyCinfoRva, bodyCinfo, sizeof(bodyCinfo)) &&
      !memcmp(base + StringPtrResetRva, stringReset, sizeof(stringReset)) &&
      !memcmp(base + CreateBodyRva, createBody, sizeof(createBody)) &&
      !memcmp(base + CreateBodyAddCallRva - 14, createBodyAdd, sizeof(createBodyAdd)) &&
      CallsAt(base, CreateBodyAddCallRva, AddBodiesRva) &&
      !memcmp(base + AddBodiesRva, addBodies, sizeof(addBodies)) &&
      !memcmp(base + DestroyBodiesRva, destroyBodies, sizeof(destroyBodies)) &&
      !memcmp(base + SetBodyTransformRva, setBodyTransform, sizeof(setBodyTransform)) &&
      !memcmp(base + RecordMaterialRva, recordMaterial, sizeof(recordMaterial)) &&
      *(__int64 const*)(base + BodyRecordVTableRva + 89 * 8) == (__int64)base + RecordMaterialRva &&
      *(__int64 const*)(base + KillVolumeVTableRva + 2 * 8) == (__int64)base + KillVolumePropertyRva;
  }

  // ---- The road meshes' surface (Resources\GroundRoads.bin: "CTGR", version 2, vertex count, triangle count,
  // the vertices (x y z floats, world), the triangles (3 u32 each), each triangle's surface (a byte, SurfaceDecls);
  // the floor-like triangles of the city's 24 road placements with their water (the marina's, the station's: the
  // ground holds her on it), railings and glass left out, and the decks of the street bridges, platforms and elevated
  // highway pieces)
  struct Roads
  {
    std::vector<float> Vertices;
    std::vector<unsigned> Triangles;
    std::vector<unsigned char> Surfaces;
    float MinX = 0.f, MinZ = 0.f;
    int CellsX = 0, CellsZ = 0;
    std::vector<unsigned> CellStart;
    std::vector<unsigned> CellTriangles;
    static constexpr float Cell = 16.f;
    // A cell with many triangles (the densest has 1,934, round x 47 z 438) gets a grid of its own, of 2 m
    static constexpr int SubCells = 8;
    static constexpr float SubCell = Cell / SubCells;
    static constexpr unsigned SubThreshold = 48;
    std::vector<int> SubOf;             // a cell's own grid, -1 for none
    std::vector<unsigned> SubStart;     // SubCells * SubCells + 1 a grid, into SubTriangles
    std::vector<unsigned> SubTriangles;
    int SubGrids = 0;

    bool Load()
    {
      void* data = nullptr;
      DWORD size = 0;
      if (!util::GetResource(IDR_GROUND_ROADS, data, size) || size < 16)
        return false;
      BYTE const* p = (BYTE const*)data;
      unsigned version = 0, vertexCount = 0, triangleCount = 0;
      memcpy(&version, p + 4, 4);
      memcpy(&vertexCount, p + 8, 4);
      memcpy(&triangleCount, p + 12, 4);
      if (memcmp(p, "CTGR", 4) || version != 2 || vertexCount == 0 || triangleCount == 0 || triangleCount >= (1u << 28) ||
          16 + (unsigned __int64)vertexCount * 12 + (unsigned __int64)triangleCount * 13 != size)
        return false;
      Vertices.resize((size_t)vertexCount * 3);
      Triangles.resize((size_t)triangleCount * 3);
      Surfaces.resize(triangleCount);
      memcpy(Vertices.data(), p + 16, (size_t)vertexCount * 12);
      memcpy(Triangles.data(), p + 16 + (size_t)vertexCount * 12, (size_t)triangleCount * 12);
      memcpy(Surfaces.data(), p + 16 + (size_t)vertexCount * 12 + (size_t)triangleCount * 12, triangleCount);
      for (unsigned i : Triangles)
        if (i >= vertexCount)
          return false;
      for (unsigned char s : Surfaces)
        if (s == 0 || s > SurfaceCount)
          return false;
      float maxX = -1e30f, maxZ = -1e30f;
      MinX = MinZ = 1e30f;
      for (size_t v = 0; v < vertexCount; ++v)
      {
        MinX = (std::min)(MinX, Vertices[v * 3]); maxX = (std::max)(maxX, Vertices[v * 3]);
        MinZ = (std::min)(MinZ, Vertices[v * 3 + 2]); maxZ = (std::max)(maxZ, Vertices[v * 3 + 2]);
      }
      CellsX = (int)((maxX - MinX) / Cell) + 1;
      CellsZ = (int)((maxZ - MinZ) / Cell) + 1;
      // Each triangle in every cell its box covers
      std::vector<unsigned> counts((size_t)CellsX * CellsZ + 1, 0);
      for (int pass = 0; pass < 2; ++pass)
      {
        for (unsigned t = 0; t < triangleCount; ++t)
        {
          int x0, x1, z0, z1;
          TriangleCells(t, x0, x1, z0, z1);
          for (int z = z0; z <= z1; ++z)
            for (int x = x0; x <= x1; ++x)
            {
              size_t c = (size_t)z * CellsX + x;
              if (pass == 0)
                ++counts[c];
              else
                CellTriangles[CellStart[c] + counts[c]++] = t;
            }
        }
        if (pass == 0)
        {
          CellStart.assign(counts.size(), 0);
          unsigned total = 0;
          for (size_t c = 0; c + 1 < counts.size(); ++c)
          {
            CellStart[c] = total;
            total += counts[c];
          }
          CellStart.back() = total;
          CellTriangles.resize(total);
          std::fill(counts.begin(), counts.end(), 0);
        }
      }
      BuildSubGrids();
      return true;
    }

    void TriangleBox(unsigned t, float& lx, float& hx, float& lz, float& hz) const
    {
      lx = 1e30f; hx = -1e30f; lz = 1e30f; hz = -1e30f;
      for (int k = 0; k < 3; ++k)
      {
        float const* v = &Vertices[(size_t)Triangles[(size_t)t * 3 + k] * 3];
        lx = (std::min)(lx, v[0]); hx = (std::max)(hx, v[0]);
        lz = (std::min)(lz, v[2]); hz = (std::max)(hz, v[2]);
      }
    }

    void TriangleCells(unsigned t, int& x0, int& x1, int& z0, int& z1) const
    {
      float lx, hx, lz, hz;
      TriangleBox(t, lx, hx, lz, hz);
      x0 = (std::max)(0, (int)((lx - MinX) / Cell)); x1 = (std::min)(CellsX - 1, (int)((hx - MinX) / Cell));
      z0 = (std::max)(0, (int)((lz - MinZ) / Cell)); z1 = (std::min)(CellsZ - 1, (int)((hz - MinZ) / Cell));
    }

    static int ClampSub(int v)
    {
      return v < 0 ? 0 : (v >= SubCells ? SubCells - 1 : v);
    }

    float CellX(int cx) const
    {
      return MinX + (float)cx * Cell;
    }

    float CellZ(int cz) const
    {
      return MinZ + (float)cz * Cell;
    }

    void BuildSubGrids()
    {
      SubOf.assign((size_t)CellsX * CellsZ, -1);
      SubGrids = 0;
      for (size_t c = 0; c < SubOf.size(); ++c)
        if (CellStart[c + 1] - CellStart[c] > SubThreshold)
          SubOf[c] = SubGrids++;
      const size_t perGrid = (size_t)SubCells * SubCells;
      std::vector<unsigned> counts((size_t)SubGrids * perGrid, 0);
      SubStart.assign((size_t)SubGrids * (perGrid + 1), 0);
      for (int pass = 0; pass < 2; ++pass)
      {
        for (size_t c = 0; c < SubOf.size(); ++c)
        {
          int g = SubOf[c];
          if (g < 0)
            continue;
          float cellX = CellX((int)(c % (size_t)CellsX)), cellZ = CellZ((int)(c / (size_t)CellsX));
          for (unsigned i = CellStart[c]; i < CellStart[c + 1]; ++i)
          {
            unsigned t = CellTriangles[i];
            float lx, hx, lz, hz;
            TriangleBox(t, lx, hx, lz, hz);
            int x0 = ClampSub((int)floorf((lx - cellX) / SubCell)), x1 = ClampSub((int)floorf((hx - cellX) / SubCell));
            int z0 = ClampSub((int)floorf((lz - cellZ) / SubCell)), z1 = ClampSub((int)floorf((hz - cellZ) / SubCell));
            for (int z = z0; z <= z1; ++z)
              for (int x = x0; x <= x1; ++x)
              {
                size_t s = (size_t)z * SubCells + x;
                if (pass == 0)
                  ++counts[(size_t)g * perGrid + s];
                else
                  SubTriangles[SubStart[(size_t)g * (perGrid + 1) + s] + counts[(size_t)g * perGrid + s]++] = t;
              }
          }
        }
        if (pass == 0)
        {
          unsigned total = 0;
          for (int g = 0; g < SubGrids; ++g)
          {
            for (size_t s = 0; s < perGrid; ++s)
            {
              SubStart[(size_t)g * (perGrid + 1) + s] = total;
              total += counts[(size_t)g * perGrid + s];
            }
            SubStart[(size_t)g * (perGrid + 1) + perGrid] = total;
          }
          SubTriangles.resize(total);
          std::fill(counts.begin(), counts.end(), 0);
        }
      }
    }

    // The triangles that can hold (x, z): its cell's, or its 2 m square's in a dense cell
    bool Candidates(float x, float z, unsigned const*& first, unsigned const*& last) const
    {
      if (CellsX == 0)
        return false;
      int cx = (int)floorf((x - MinX) / Cell), cz = (int)floorf((z - MinZ) / Cell);
      if (cx < 0 || cz < 0 || cx >= CellsX || cz >= CellsZ)
        return false;
      size_t c = (size_t)cz * CellsX + cx;
      int g = SubOf.empty() ? -1 : SubOf[c];
      if (g < 0)
      {
        first = CellTriangles.data() + CellStart[c];
        last = CellTriangles.data() + CellStart[c + 1];
        return true;
      }
      int sx = ClampSub((int)floorf((x - CellX(cx)) / SubCell)), sz = ClampSub((int)floorf((z - CellZ(cz)) / SubCell));
      size_t s = (size_t)g * ((size_t)SubCells * SubCells + 1) + (size_t)sz * SubCells + sx;
      first = SubTriangles.data() + SubStart[s];
      last = SubTriangles.data() + SubStart[s + 1];
      return true;
    }

    // A triangle's upward unit normal (the data's are floor-like: |ny| > 0.3)
    void Normal(unsigned t, float* normal) const
    {
      float const* a = &Vertices[(size_t)Triangles[(size_t)t * 3] * 3];
      float const* b = &Vertices[(size_t)Triangles[(size_t)t * 3 + 1] * 3];
      float const* d = &Vertices[(size_t)Triangles[(size_t)t * 3 + 2] * 3];
      float e1x = b[0] - a[0], e1y = b[1] - a[1], e1z = b[2] - a[2];
      float e2x = d[0] - a[0], e2y = d[1] - a[1], e2z = d[2] - a[2];
      float nx = e1y * e2z - e1z * e2y, ny = e1z * e2x - e1x * e2z, nz = e1x * e2y - e1y * e2x;
      float len = sqrtf(nx * nx + ny * ny + nz * nz);
      if (!(len > 1e-12f))
      {
        normal[0] = 0.f; normal[1] = 1.f; normal[2] = 0.f;
        return;
      }
      if (ny < 0.f)
        len = -len;
      normal[0] = nx / len; normal[1] = ny / len; normal[2] = nz / len;
    }

    // A triangle's plane at (x, z): its height there and its normal
    bool Plane(unsigned t, float x, float z, float& y, float* normal) const
    {
      if ((size_t)t * 3 + 2 >= Triangles.size())
        return false;
      Normal(t, normal);
      if (!(normal[1] >= 0.05f))
        return false;
      float const* a = &Vertices[(size_t)Triangles[(size_t)t * 3] * 3];
      y = a[1] - (normal[0] * (x - a[0]) + normal[2] * (z - a[2])) / normal[1];
      return true;
    }

    // The highest road surface at (x, z) no higher than top: its height, its upward normal and its triangle
    bool Height(float x, float z, float top, float& height, float* normal, unsigned* tri = nullptr) const
    {
      unsigned const* first = nullptr;
      unsigned const* last = nullptr;
      if (!Candidates(x, z, first, last))
        return false;
      bool found = false;
      for (; first != last; ++first)
      {
        unsigned t = *first;
        float const* a = &Vertices[(size_t)Triangles[(size_t)t * 3] * 3];
        float const* b = &Vertices[(size_t)Triangles[(size_t)t * 3 + 1] * 3];
        float const* d = &Vertices[(size_t)Triangles[(size_t)t * 3 + 2] * 3];
        // Barycentric in x / z
        float e1x = b[0] - a[0], e1z = b[2] - a[2], e2x = d[0] - a[0], e2z = d[2] - a[2];
        float det = e1x * e2z - e2x * e1z;
        if (fabsf(det) < 1e-8f)
          continue;
        float px = x - a[0], pz = z - a[2];
        float u = (px * e2z - e2x * pz) / det;
        float v = (e1x * pz - px * e1z) / det;
        const float eps = 1e-4f;
        if (u < -eps || v < -eps || u + v > 1.f + eps)
          continue;
        float y = a[1] + u * (b[1] - a[1]) + v * (d[1] - a[1]);
        if (y > top || (found && y <= height))
          continue;
        height = y;
        Normal(t, normal);
        if (tri)
          *tri = t;
        found = true;
      }
      return found;
    }
  };
  Roads g_Roads;

  // ---- The props (Resources\GroundProps.bin, stored packed as GroundProps.ctpk: "CTGP", version 2, shape count, box count, the shapes (half sizes, 3
  // floats each), the boxes (u32 shape, 12 floats: right, up, forward (unit), centre), each box's surface (a byte: its
  // low 4 bits SurfaceDecls', its high 4 its group)): every model the city's free-roam levels draw with no collision, at any height (street furniture,
  // lamps, signs, parked cars and boats, statues, storefronts, plazas, walkways, roofs, backdrop buildings, the train
  // station under the lair), as boxes of their LOD0 triangles (small ones up to 2.5 m tall a box per 2 m band; the
  // rest columns that keep their tops to 0.1 m, a roof over a floor a slab, with thin columns for what the columns
  // don't hold: poles, walls, railings), each at its placement; and the road
  // meshes' walls, barriers and railings 0.3 m tall or more as thin columns (the road data has only their floors,
  // and the tiles are never walls); and the models whose zone has no collision for them though their group says it
  // does (the Bauble's base, the plaza with pools by it, the Bauble's ruins) and the whole Bauble's roofs, their tops
  // as 0.3 m slabs and their walls thin columns, nothing filled under them. Left out: what
  // only blocks wider than 2 m could hold within its count (the mountains round the city, a far dome, the harbour
  // piers' underside, the Shard's unreachable surroundings) and such blocks turned over, on their side or scaled down
  struct Props
  {
    struct Box
    {
      unsigned Shape;
      unsigned char Surface;
      unsigned char Group;        // 0 always; else made only while that group is on (GroupOn)
      float Rows[12];             // right, up, forward (a rotation), centre
      float Min[3], Max[3];       // its box in the world's axes
    };
    std::vector<float> Halves;    // 3 a shape
    std::vector<Box> Boxes;
    int Mirrored = 0, Skipped = 0;
    // A grid over x / z of the boxes' indexes, each box in every cell its box reaches; the boxes that reach more than
    // GridMaxSpan cells a side are in Wide instead (looked at each time)
    static constexpr float GridCell = 32.f;
    static constexpr int GridMaxSpan = 8;
    static constexpr int GridMaxCells = 4096;     // a side
    float GridX0 = 0.f, GridZ0 = 0.f, CellStep = GridCell;
    int GridNX = 0, GridNZ = 0;
    std::vector<unsigned> CellStart;              // GridNX * GridNZ + 1
    std::vector<unsigned> CellBoxes;
    std::vector<unsigned> Wide;
    std::vector<unsigned> Seen;                   // a box's last Stamp (PlanProps: each box once)
    unsigned Stamp = 0;

    void CellRange(float lo, float hi, float origin, int n, int& c0, int& c1) const
    {
      c0 = (int)floorf((lo - origin) / CellStep);
      c1 = (int)floorf((hi - origin) / CellStep);
      c0 = (std::max)(c0, 0);
      c1 = (std::min)(c1, n - 1);
    }

    void BuildGrid()
    {
      float x0 = 1e30f, z0 = 1e30f, x1 = -1e30f, z1 = -1e30f;
      for (Box const& b : Boxes)
        if (b.Min[0] < 1e29f)
        {
          x0 = (std::min)(x0, b.Min[0]); z0 = (std::min)(z0, b.Min[2]);
          x1 = (std::max)(x1, b.Max[0]); z1 = (std::max)(z1, b.Max[2]);
        }
      Seen.assign(Boxes.size(), 0);
      if (x0 > x1)
        return;
      CellStep = (std::max)(GridCell, (std::max)(x1 - x0, z1 - z0) / (float)(GridMaxCells - 1));
      GridX0 = x0; GridZ0 = z0;
      GridNX = (int)((x1 - x0) / CellStep) + 1;
      GridNZ = (int)((z1 - z0) / CellStep) + 1;
      std::vector<unsigned> count((size_t)GridNX * GridNZ + 1, 0);
      for (int pass = 0; pass < 2; ++pass)
      {
        for (unsigned i = 0; i < Boxes.size(); ++i)
        {
          Box const& b = Boxes[i];
          if (b.Min[0] > 1e29f)
            continue;
          int i0, i1, k0, k1;
          CellRange(b.Min[0], b.Max[0], GridX0, GridNX, i0, i1);
          CellRange(b.Min[2], b.Max[2], GridZ0, GridNZ, k0, k1);
          if (i1 - i0 >= GridMaxSpan || k1 - k0 >= GridMaxSpan)
          {
            if (pass == 0)
              Wide.push_back(i);
            continue;
          }
          for (int ci = i0; ci <= i1; ++ci)
            for (int ck = k0; ck <= k1; ++ck)
            {
              size_t c = (size_t)ci * GridNZ + ck;
              if (pass == 0)
                ++count[c + 1];
              else
                CellBoxes[CellStart[c] + count[c]++] = i;
            }
        }
        if (pass == 0)
        {
          for (size_t c = 1; c < count.size(); ++c)
            count[c] += count[c - 1];
          CellStart = count;
          CellBoxes.resize(CellStart.back());
          std::fill(count.begin(), count.end(), 0);
        }
      }
    }

    bool Load()
    {
      // Stored packed (Resources\GroundProps.ctpk, made by tools/pack_resource.py from GroundProps.bin) and unpacked
      // here into memory of its own, freed when Load returns: everything below copies out of it
      util::UnpackedResource raw;
      if (!util::GetPackedResource(IDR_GROUND_PROPS, raw) || raw.Size < 16 || raw.Size > MAXDWORD)
        return false;
      BYTE const* p = raw.Data;
      DWORD size = (DWORD)raw.Size;
      unsigned version = 0, shapeCount = 0, boxCount = 0;
      memcpy(&version, p + 4, 4);
      memcpy(&shapeCount, p + 8, 4);
      memcpy(&boxCount, p + 12, 4);
      if (memcmp(p, "CTGP", 4) || version != 2 || shapeCount == 0 || shapeCount > (1u << 22) || boxCount >= (1u << 26) ||
          16 + (unsigned __int64)shapeCount * 12 + (unsigned __int64)boxCount * 53 != size)
        return false;
      Halves.resize((size_t)shapeCount * 3);
      memcpy(Halves.data(), p + 16, (size_t)shapeCount * 12);
      for (float h : Halves)
        if (!(h >= 0.01f && h < 2000.f))         // far skyscrapers are placed scaled up: columns up to ~1.1 km
          return false;
      BYTE const* b = p + 16 + (size_t)shapeCount * 12;
      BYTE const* surfaces = b + (size_t)boxCount * 52;
      Boxes.reserve(boxCount);
      for (unsigned i = 0; i < boxCount; ++i, b += 52)
      {
        Box box;
        memcpy(&box.Shape, b, 4);
        memcpy(box.Rows, b + 4, 48);
        box.Surface = surfaces[i] & 0x0F;
        box.Group = surfaces[i] >> 4;
        if (box.Shape >= shapeCount || box.Surface == 0 || box.Surface > SurfaceCount || box.Group > GroupCount)
          return false;
        float* r = box.Rows;
        // A rotation: unit rows at right angles (a mirrored placement's forward is turned round: the box is the same)
        bool ok = true;
        for (int m = 0; m < 3 && ok; ++m)
          for (int n = m; n < 3 && ok; ++n)
          {
            float dot = r[m * 3] * r[n * 3] + r[m * 3 + 1] * r[n * 3 + 1] + r[m * 3 + 2] * r[n * 3 + 2];
            ok = fabsf(dot - (m == n ? 1.f : 0.f)) < 1e-3f;
          }
        for (int k = 9; k < 12 && ok; ++k)
          ok = fabsf(r[k]) < 1e5f;
        if (!ok)
        {
          ++Skipped;
          // Kept so that the box indexes stay the file's; it's never planned
          box.Min[0] = box.Min[1] = box.Min[2] = 1e30f;
          box.Max[0] = box.Max[1] = box.Max[2] = 1e30f;
          Boxes.push_back(box);
          continue;
        }
        float det = r[0] * (r[4] * r[8] - r[5] * r[7]) - r[1] * (r[3] * r[8] - r[5] * r[6]) +
          r[2] * (r[3] * r[7] - r[4] * r[6]);
        if (det < 0.f)
        {
          r[6] = -r[6]; r[7] = -r[7]; r[8] = -r[8];
          ++Mirrored;
        }
        float const* h = &Halves[(size_t)box.Shape * 3];
        for (int k = 0; k < 3; ++k)
        {
          float e = fabsf(r[k]) * h[0] + fabsf(r[3 + k]) * h[1] + fabsf(r[6 + k]) * h[2];
          box.Min[k] = r[9 + k] - e;
          box.Max[k] = r[9 + k] + e;
        }
        Boxes.push_back(box);
      }
      BuildGrid();
      return true;
    }
  };
  Props g_Props;
  bool g_PropsLoaded = false;

  // ---- The shapes: the tiles' (a size for each level and tilt), then the props'
  // A tilted tile's top is narrower seen from above: its sides are lengthened by 1 / the tilt's cosine, in steps
  const float TiltStepCos[4] = { 0.995f, 0.95f, 0.9f, MaxTiltCos };
  const unsigned TileShapeCount = (TileMaxLevel + 1) * 4;

  float TileSize(int level)
  {
    return TileCell / (float)(1 << level);
  }

  int TileSpan(int level)     // its side in GridStep squares
  {
    return 4 >> level;
  }

  int TiltStep(float upY)
  {
    for (int s = 0; s < 3; ++s)
      if (upY >= TiltStepCos[s])
        return s;
    return 3;
  }

  void ShapeHalf(unsigned shape, float* half)
  {
    if (shape < TileShapeCount)
    {
      float side = TileSize((int)(shape / 4)) * 0.5f / TiltStepCos[shape % 4];
      half[0] = side; half[1] = TileHalfY; half[2] = side;
    }
    else
      memcpy(half, &g_Props.Halves[(size_t)(shape - TileShapeCount) * 3], 3 * sizeof(float));
  }

  // ---- A tile, from its key. Bit 63: a tile (a prop's key is its box's index); bit 62: lowered (a level tile at her
  // feet); bits 60-61 its level; 44-59 and 28-43 its square's corner on the 0.5 m grid (+ GridOffset); 0-27 the road
  // triangle its top lies on, or a lowered one's surface (bits 24-27) and top in cm (0-23, + TopOffset)
  const unsigned __int64 TileBit = 1ull << 63;
  const unsigned __int64 LoweredBit = 1ull << 62;
  const int GridOffset = 32768;
  const int TopOffset = 1 << 23;

  unsigned LoweredValue(unsigned surface, int topCm)
  {
    return (surface << 24) | (unsigned)(topCm + TopOffset);
  }

  struct Tile
  {
    int Level = 0, Gx = 0, Gz = 0;
    float X0 = 0.f, Z0 = 0.f, Size = 0.f;   // its square
    float Top[3] = {};                      // the middle of its top
    float Up[3] = { 0.f, 1.f, 0.f };
    unsigned Shape = 0;
    unsigned char Surface = 0;              // SurfaceDecls
  };

  bool GridFits(int gx, int gz, int level)
  {
    return gx + GridOffset >= 0 && gz + GridOffset >= 0 && gx + TileSpan(level) + GridOffset <= 0xFFFF &&
      gz + TileSpan(level) + GridOffset <= 0xFFFF;
  }

  unsigned __int64 TileKey(int level, int gx, int gz, bool lowered, unsigned value)
  {
    return TileBit | (lowered ? LoweredBit : 0) | ((unsigned __int64)level << 60) |
      ((unsigned __int64)(unsigned)(gx + GridOffset) << 44) | ((unsigned __int64)(unsigned)(gz + GridOffset) << 28) |
      (unsigned __int64)(value & 0xFFFFFFF);
  }

  void TileUp(float const* normal, float* up)
  {
    if (normal[1] >= MaxTiltCos)
      memcpy(up, normal, 3 * sizeof(float));
    else
    {
      up[0] = 0.f; up[1] = 1.f; up[2] = 0.f;
    }
  }

  bool DecodeTile(unsigned __int64 key, Tile& t)
  {
    if (!(key & TileBit))
      return false;
    t.Level = (int)((key >> 60) & 3);
    if (t.Level > TileMaxLevel)
      return false;
    t.Gx = (int)((key >> 44) & 0xFFFF) - GridOffset;
    t.Gz = (int)((key >> 28) & 0xFFFF) - GridOffset;
    t.Size = TileSize(t.Level);
    t.X0 = (float)t.Gx * GridStep;
    t.Z0 = (float)t.Gz * GridStep;
    t.Top[0] = t.X0 + t.Size * 0.5f;
    t.Top[2] = t.Z0 + t.Size * 0.5f;
    unsigned value = (unsigned)(key & 0xFFFFFFF);
    if (key & LoweredBit)
    {
      t.Top[1] = (float)((int)(value & 0xFFFFFF) - TopOffset) / 100.f;
      t.Up[0] = 0.f; t.Up[1] = 1.f; t.Up[2] = 0.f;
      t.Surface = (unsigned char)(value >> 24);
    }
    else
    {
      float normal[3];
      if (!g_Roads.Plane(value, t.Top[0], t.Top[2], t.Top[1], normal))
        return false;
      TileUp(normal, t.Up);
      t.Surface = g_Roads.Surfaces[value];
    }
    if (t.Surface == 0 || t.Surface > SurfaceCount)
      return false;
    t.Shape = (unsigned)(t.Level * 4 + TiltStep(t.Up[1]));
    return true;
  }

  // The box's rows (right, up, forward, its middle): its top's middle on the road, tilted with it
  void TileRows(Tile const& t, float* rows)
  {
    float const* up = t.Up;
    // right: x without its part along up; forward = right x up
    float right[3] = { 1.f - up[0] * up[0], -up[0] * up[1], -up[0] * up[2] };
    float len = sqrtf(right[0] * right[0] + right[1] * right[1] + right[2] * right[2]);
    for (float& v : right)
      v /= len;
    float forward[3] = { right[1] * up[2] - right[2] * up[1], right[2] * up[0] - right[0] * up[2],
      right[0] * up[1] - right[1] * up[0] };
    float const* r[3] = { right, up, forward };
    for (int i = 0; i < 3; ++i)
    {
      rows[i * 4] = r[i][0]; rows[i * 4 + 1] = r[i][1]; rows[i * 4 + 2] = r[i][2]; rows[i * 4 + 3] = 0.f;
    }
    rows[12] = t.Top[0] - up[0] * TileHalfY;
    rows[13] = t.Top[1] - up[1] * TileHalfY;
    rows[14] = t.Top[2] - up[2] * TileHalfY;
    rows[15] = 1.f;
  }

  // From (x, z) to its square (0 inside it)
  float TileDistance(Tile const& t, float x, float z)
  {
    float dx = (std::max)((std::max)(t.X0 - x, x - (t.X0 + t.Size)), 0.f);
    float dz = (std::max)((std::max)(t.Z0 - z, z - (t.Z0 + t.Size)), 0.f);
    return sqrtf(dx * dx + dz * dz);
  }

  // Its top over (x, z), taken into its square
  float TileTopAt(Tile const& t, float x, float z)
  {
    float px = (std::min)((std::max)(x, t.X0), t.X0 + t.Size);
    float pz = (std::min)((std::max)(z, t.Z0), t.Z0 + t.Size);
    return t.Top[1] - (t.Up[0] * (px - t.Top[0]) + t.Up[2] * (pz - t.Top[2])) / t.Up[1];
  }

  // As high as its top gets within LeadDistance of (x, z): where she can be by the time the flush makes it (the
  // offline test: up a slope at a sprint, tiles made 5 - 9 cm over her feet)
  float TileTopNear(Tile const& t, float x, float z)
  {
    return TileTopAt(t, x, z) + sqrtf(t.Up[0] * t.Up[0] + t.Up[2] * t.Up[2]) / t.Up[1] * LeadDistance;
  }

  // ---- A real static body whose ids the tools' bodies take (the camera hook's)
  struct Donor
  {
    __int64 World = 0;          // the game's physics world
    __int64 Havok = 0;          // its hknpWorld
    int Body = -1;
    __int64 Record = 0;
    unsigned __int64 UserData = 0;
    unsigned Filter = 0;
    unsigned short Material = 0;
    unsigned char Quality = 0;
  };

  // A body that's a static, layer 1 body whose record points back at it, with the hit material lookup's table
  // readable (index 0, what the tools' shapes give)
  // why: what turned it down (a short text, for the log)
  bool ReadDonor(__int64 world, int body, Donor& out, char const** why = nullptr)
  {
    __int64 base = Base();
    char const* volatile no = "a read faulted";
    __try
    {
      __int64 havok = *(__int64 const*)(world + WorldHavokOffset);
      no = "not a body of this world";
      if (havok < 0x10000 || body < 0 || (unsigned)body >= (*(unsigned const*)(havok + HavokCapacityOffset) & 0x3FFFFFFF))
        goto refused;
      __int64 b = *(__int64 const*)(havok + HavokBodiesOffset) + (__int64)body * BodySize;
      no = "no shape, or not this body";
      if (*(__int64 const*)(b + BodyShapeOffset) < 0x10000 || *(int const*)(b + BodyIdOffset) != body)
        goto refused;
      no = "not static (it moves)";
      if (*(int const*)(b + BodyMotionOffset) != 0)
        goto refused;
      unsigned filter = *(unsigned const*)(b + BodyFilterOffset);
      no = "not collision layer 1 (Static)";
      if ((filter & 0x1F) != 1)
        goto refused;
      unsigned __int64 userData = *(unsigned __int64 const*)(b + BodyUserDataOffset);
      __int64 records = *(__int64 const*)(world + WorldRecordsOffset);
      no = "no records";
      if (records < 0x10000)
        goto refused;
      __int64 record = *(__int64 const*)(records + (__int64)(userData & 0xFFFF) * 8);
      no = "its record isn't a body record";
      if (record < 0x10000 || *(__int64 const*)record != base + BodyRecordVTableRva)
        goto refused;
      no = "its record is another body's";
      if (*(int const*)(record + RecordBodyOffset) != body)
        goto refused;
      __int64 owner = *(__int64 const*)(record + RecordOwnerOffset);
      no = "its record is another world's";
      if (owner < 0x10000 || *(__int64 const*)(owner + WorldHavokOffset) != havok)
        goto refused;
      __int64 materials = *(__int64 const*)(record + RecordMaterialsOffset);
      no = "its record's material table isn't there";
      if (materials)
      {
        __int64 t = *(__int64 const*)(materials + 0x18);
        if (t < 0x10000)
          goto refused;
        __int64 table = *(__int64 const*)(t + 0x2C);
        if (table < 0x10000)
          goto refused;
        volatile unsigned first = *(unsigned const*)table;
        (void)first;
      }
      out.World = world;
      out.Havok = havok;
      out.Body = body;
      out.Record = record;
      out.UserData = userData;
      out.Filter = filter;
      out.Material = *(unsigned short const*)(b + BodyMaterialOffset);
      out.Quality = *(unsigned char const*)(b + BodyQualityOffset);
      return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      no = "a read faulted";
    }
  refused:
    if (why)
      *why = no;
    return false;
  }

  bool DonorStillThere(Donor const& d)
  {
    Donor now;
    return d.Body >= 0 && ReadDonor(d.World, d.Body, now) && now.Record == d.Record && now.UserData == d.UserData &&
      now.Havok == d.Havok;
  }

  // Every field the same (a plan carries the donor it was made with: PlanMemo)
  bool SameDonor(Donor const& a, Donor const& b)
  {
    return a.World == b.World && a.Havok == b.Havok && a.Body == b.Body && a.Record == b.Record &&
      a.UserData == b.UserData && a.Filter == b.Filter && a.Material == b.Material && a.Quality == b.Quality;
  }

  // ---- What the camera hook wants in the game's physics, taken by the flush
  struct Item
  {
    unsigned __int64 Key;       // a tile's (TileBit) or a prop box's index
    unsigned Shape;             // in the shape table
    float Distance;             // from her when planned
    bool Make;                  // the flush may make it (false: kept if it's there, as near her it'd be made into her)
    unsigned char Surface;      // SurfaceDecls
    float Rows[16];             // copied to an aligned array for Havok
  };
  struct Plan
  {
    Donor From;
    std::vector<Item> Items;                    // nearest first
    std::vector<unsigned __int64> SortedKeys;   // their keys, sorted
    std::vector<unsigned> SortedItems;          // the item of each of SortedKeys (its index in Items)
  };

  // The sorts' work lists (RadixOrder, KeyOrder, NearestFirst), kept by their caller from call to call: cleared, not
  // freed and made again each frame (2026-11-13, "tools fixes only"). Update's thread has its own; PublishMade (the
  // flush, on the game's threads) makes one each time, as before
  struct SortWork
  {
    std::vector<unsigned> Count, Other;                               // RadixOrder's
    std::vector<unsigned> Low, LowAt, HighAt, LowOrder, HighOrder;    // KeyOrder's
    std::vector<unsigned __int64> High;
    std::vector<unsigned> Bits, Order;                                // NearestFirst's
    std::vector<Item> Sorted;
  };

  // The order a stable sort of n keys (unsigned integers) puts them in, as their indexes: a radix sort, a byte a pass
  // (least significant first), the bytes every key has the same left out (2026-11-03: std::sort and std::stable_sort
  // of the plan's ~3,000 items and keys took ~0.3 ms of the planning, this ~0.03)
  template <class K>
  void RadixOrder(K const* key, size_t n, std::vector<unsigned>& order, SortWork& work)
  {
    order.resize(n);
    for (size_t i = 0; i < n; ++i)
      order[i] = (unsigned)i;
    if (n < 2)
      return;
    std::vector<unsigned>& count = work.Count;          // (not on the stack: the flush runs on the game's threads)
    count.assign(sizeof(K) * 256, 0);
    for (size_t i = 0; i < n; ++i)
      for (int b = 0; b < (int)sizeof(K); ++b)
        ++count[b * 256 + ((key[i] >> (8 * b)) & 0xFF)];
    std::vector<unsigned>& other = work.Other;          // (each pass writes all n of it before they're read)
    other.resize(n);
    for (int b = 0; b < (int)sizeof(K); ++b)
    {
      unsigned* c = &count[b * 256];
      if (c[(key[0] >> (8 * b)) & 0xFF] == n)
        continue;
      unsigned sum = 0;
      for (int d = 0; d < 256; ++d)
      {
        unsigned here = c[d];
        c[d] = sum;
        sum += here;
      }
      for (size_t i = 0; i < n; ++i)
        other[c[(key[order[i]] >> (8 * b)) & 0xFF]++] = order[i];
      order.swap(other);
    }
  }

  // The order n keys are sorted in (RadixOrder): those under 2^32 (the props' box indexes, three bytes) as 32-bit
  // numbers, then the others (the tiles') as 64-bit
  void KeyOrder(unsigned __int64 const* keys, size_t n, std::vector<unsigned>& order, SortWork& work)
  {
    std::vector<unsigned>& low = work.Low;
    std::vector<unsigned>& lowAt = work.LowAt;
    std::vector<unsigned>& highAt = work.HighAt;
    std::vector<unsigned>& lowOrder = work.LowOrder;
    std::vector<unsigned>& highOrder = work.HighOrder;
    std::vector<unsigned __int64>& high = work.High;
    low.clear();
    lowAt.clear();
    highAt.clear();
    high.clear();
    low.reserve(n);
    lowAt.reserve(n);
    for (size_t i = 0; i < n; ++i)
      if (keys[i] >> 32)
      {
        high.push_back(keys[i]);
        highAt.push_back((unsigned)i);
      }
      else
      {
        low.push_back((unsigned)keys[i]);
        lowAt.push_back((unsigned)i);
      }
    RadixOrder(low.data(), low.size(), lowOrder, work);
    RadixOrder(high.data(), high.size(), highOrder, work);
    order.clear();
    order.reserve(n);
    for (unsigned k : lowOrder)
      order.push_back(lowAt[k]);
    for (unsigned k : highOrder)
      order.push_back(highAt[k]);
  }

  // The keys of a (sorted) that aren't in b (sorted), each once
  void NotIn(std::vector<unsigned __int64> const& a, std::vector<unsigned __int64> const& b,
    std::vector<unsigned __int64>& out)
  {
    out.clear();
    size_t j = 0;
    for (size_t i = 0; i < a.size(); ++i)
    {
      if (i && a[i - 1] == a[i])
        continue;
      while (j < b.size() && b[j] < a[i])
        ++j;
      if (j == b.size() || b[j] != a[i])
        out.push_back(a[i]);
    }
  }

  // The items nearest first, ties in the order planned: what std::stable_sort by distance gives, by a radix sort of the
  // distances' bits (as unsigned integers they're in the floats' order, all being +0 or more), or std::stable_sort
  // itself if one is a NaN or has its sign bit
  // (sorted, swapped with items, keeps their buffer from before: a plan's list from before it's handed over, never
  // one that's out)
  void NearestFirst(std::vector<Item>& items, SortWork& work)
  {
    size_t n = items.size();
    std::vector<unsigned>& bits = work.Bits;
    bits.resize(n);
    for (size_t i = 0; i < n; ++i)
    {
      memcpy(&bits[i], &items[i].Distance, sizeof(unsigned));
      if (bits[i] > 0x7F800000u)
      {
        std::stable_sort(items.begin(), items.end(), [](Item const& a, Item const& b) { return a.Distance < b.Distance; });
        return;
      }
    }
    std::vector<unsigned>& order = work.Order;
    RadixOrder(bits.data(), n, order, work);
    std::vector<Item>& sorted = work.Sorted;
    sorted.clear();
    sorted.reserve(n);
    for (unsigned i : order)
      sorted.push_back(items[i]);
    items.swap(sorted);
  }

  SRWLOCK g_PlanLock = SRWLOCK_INIT;
  std::shared_ptr<Plan const> g_Plan;

  void PublishPlan(std::shared_ptr<Plan const> plan)
  {
    AcquireSRWLockExclusive(&g_PlanLock);
    g_Plan.swap(plan);
    ReleaseSRWLockExclusive(&g_PlanLock);
    // The one let go here (outside the lock) can be the last reference
  }

  std::shared_ptr<Plan const> TakePlan()
  {
    AcquireSRWLockShared(&g_PlanLock);
    std::shared_ptr<Plan const> plan = g_Plan;
    ReleaseSRWLockShared(&g_PlanLock);
    return plan;
  }

  // What the flush has made (their keys, sorted), for the camera hook
  SRWLOCK g_MadeLock = SRWLOCK_INIT;
  std::shared_ptr<std::vector<unsigned __int64> const> g_MadeKeys;

  std::shared_ptr<std::vector<unsigned __int64> const> TakeMade()
  {
    AcquireSRWLockShared(&g_MadeLock);
    std::shared_ptr<std::vector<unsigned __int64> const> made = g_MadeKeys;
    ReleaseSRWLockShared(&g_MadeLock);
    return made;
  }

  bool Contains(std::vector<unsigned __int64> const& sorted, unsigned __int64 key)
  {
    return std::binary_search(sorted.begin(), sorted.end(), key);
  }

  // ---- The bodies as made (the flush's only, under g_BodyLock)
  struct Made
  {
    int Id;
    __int64 Shape;
  };
  struct Bodies
  {
    __int64 World = 0;
    __int64 Havok = 0;
    Donor From;                 // whose ids they all carry
    std::unordered_map<unsigned __int64, Made> ByKey;
    std::shared_ptr<std::vector<unsigned __int64> const> Sorted;   // ByKey's keys, sorted, as last handed over
    ULONGLONG LastFlush = 0;
    bool Logged = false;
    bool FailLogged = false;
  };
  SRWLOCK g_BodyLock = SRWLOCK_INIT;
  Bodies g_Bodies;

  // The tools' own time (2026-11-02, the user: "find if anything costs frames"): the physics flush's work here
  // (any thread the game flushes on) and the planning (Update's), logged every TimingMs while Solid street is on
  const ULONGLONG TimingMs = 10000;
  long long Ticks()
  {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
  }
  std::atomic<long long> g_FlushTicks{ 0 }, g_FlushMaxTicks{ 0 };
  std::atomic<unsigned> g_FlushCount{ 0 }, g_BodiesMade{ 0 }, g_BodiesDestroyed{ 0 };
  // Its parts (2026-11-03, "where the time goes"), in the flushes of the bodies' world: checking they're still the
  // tools', destroying those the plan doesn't want, making new ones, the check after the game's own work
  enum FlushPart { PartCheck, PartDrop, PartMake, PartAfter, FlushParts };
  std::atomic<long long> g_FlushPartTicks[FlushParts];
  std::atomic<unsigned> g_FlushesOfBodies{ 0 };
  std::atomic<bool> g_NoRoomLogged{ false };
  std::vector<__int64> g_Shapes;        // made once each and kept (the tools' reference is never given back); -1: failed

  // An id as one of the tools' bodies with that surface, or not (0)
  void MarkOurs(int id, unsigned char surface)
  {
    if (id >= 0 && id < MaxTrackedId)
      g_OurSurface[id].store(surface);
  }

  unsigned char OurSurface(int id)
  {
    return id >= 0 && id < MaxTrackedId ? g_OurSurface[id].load(std::memory_order_relaxed) : 0;
  }

  void PublishMade()
  {
    std::vector<unsigned __int64> unsorted;
    unsorted.reserve(g_Bodies.ByKey.size());
    for (auto const& kv : g_Bodies.ByKey)
      unsorted.push_back(kv.first);
    std::vector<unsigned> order;
    SortWork work;              // (its own: the flush runs on the game's threads)
    KeyOrder(unsorted.data(), unsorted.size(), order, work);
    auto keys = std::make_shared<std::vector<unsigned __int64>>();
    keys->reserve(order.size());
    for (unsigned i : order)
      keys->push_back(unsorted[i]);
    std::shared_ptr<std::vector<unsigned __int64> const> made = std::move(keys);
    g_Bodies.Sorted = made;
    AcquireSRWLockExclusive(&g_MadeLock);
    g_MadeKeys.swap(made);
    ReleaseSRWLockExclusive(&g_MadeLock);
  }

  void NoteBodies()
  {
    g_BodiesHavok = g_Bodies.ByKey.empty() ? 0 : g_Bodies.Havok;
    g_BodyCount = (int)g_Bodies.ByKey.size();
  }

  // The bodies the tools' sweeps hit (the hit hook fills it on the camera hook's thread only)
  struct Capture
  {
    __int64 World = 0;
    int Bodies[8] = {};
    int Count = 0;
  };
  std::atomic<DWORD> g_CaptureThread{ 0 };
  Capture g_Capture;

  // ---- The status line (the camera hook writes it, the menu reads it)
  SRWLOCK g_StatusLock = SRWLOCK_INIT;
  char g_Status[192] = "Off";
  char g_LastLogged[192] = "";

  void SetStatus(char const* text, bool log)
  {
    AcquireSRWLockExclusive(&g_StatusLock);
    strcpy_s(g_Status, text);
    bool changed = strcmp(g_LastLogged, text) != 0;
    if (changed && log)
      strcpy_s(g_LastLogged, text);
    ReleaseSRWLockExclusive(&g_StatusLock);
    if (changed && log)
      util::log::Write("Street level: %s", text);
  }

  // ---- The hooks

  // A kill volume's data Flags, when it's a PamServerKillVolumeEntity of DamageType KillDamageType
  bool KillVolumeFlags(__int64 entity, unsigned& flags)
  {
    __try
    {
      if (*(__int64 const*)entity != Base() + KillVolumeVTableRva)
        return false;
      __int64 data = *(__int64 const*)(entity + 0x28);
      if (data < 0x10000)
        return false;
      flags = *(unsigned const*)(data + 0x10);
      return *(unsigned const*)(data + 0x18) == KillDamageType;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      return false;
    }
  }

  int IndexOf(unsigned const* list, int count, unsigned flags)
  {
    for (int i = 0; i < count; ++i)
      if (list[i] == flags)
        return i;
    return -1;
  }

  std::atomic<unsigned __int64> g_FloorsSpared{ 0 };    // one bit a street floor, logged once
  std::atomic<unsigned __int64> g_ZonesSpared{ 0 };     // one bit a level kill zone
  SRWLOCK g_KillLogLock = SRWLOCK_INIT;
  unsigned g_OtherKills[32] = {};
  int g_OtherKillCount = 0;

  bool __fastcall hKillInside(__int64 entity, __int64 character)
  {
    bool solid = StreetOn();
    unsigned flags = 0;
    bool known = solid && KillVolumeFlags(entity, flags);
    int street = known ? IndexOf(StreetFloors, StreetFloorCount, flags) : -1;
    if (street >= 0)
    {
      unsigned __int64 bit = 1ull << street;
      if (!(g_FloorsSpared.fetch_or(bit) & bit))
        util::log::Write("Street level: the street kill floor 0x%08X spares those inside it while Solid street is on", flags);
      return false;
    }
    // The level's other zones: for Faith only (the player's call), while she's over the street
    bool player = (__int64)_ReturnAddress() == Base() + KillInsidePlayerCallRva + 5;
    // The Sky City station's (SkyCityStationKills): for Faith; the Shard level's box over it while she's there
    if (known && player && (flags == SkyCityStationKills ||
        (flags == SkyCityShardKills && g_AtSkyCity.load(std::memory_order_relaxed))))
    {
      static std::atomic<int> s_logged{ 0 };
      int bit = flags == SkyCityStationKills ? 1 : 2;
      if (!(s_logged.fetch_or(bit) & bit))
        util::log::Write("Street level: the kill volume 0x%08X (%s) spares Faith while Solid street is on", flags,
          bit == 1 ? "the Sky City station's" : "the Shard's level, over the Sky City station");
      return false;
    }
    int zone = known && player && g_OverStreet.load(std::memory_order_relaxed) ?
      IndexOf(LevelKillZones, LevelKillZoneCount, flags) : -1;
    if (zone >= 0)
    {
      unsigned __int64 bit = 1ull << zone;
      if (!(g_ZonesSpared.fetch_or(bit) & bit))
        util::log::Write("Street level: the level's kill zone 0x%08X spares Faith while she's over the street", flags);
      return false;
    }
    bool inside = oKillInside(entity, character);
    // Diagnostic: Faith inside a kill volume that isn't spared, with Solid street on (diagnostics only)
    if (util::DiagnosticsOn() && inside && solid && player)
    {
      unsigned other = 0;
      __try
      {
        __int64 data = *(__int64 const*)(entity + 0x28);
        other = data > 0x10000 ? *(unsigned const*)(data + 0x10) : 0;
      }
      __except (EXCEPTION_EXECUTE_HANDLER)
      {
      }
      bool first = false;
      AcquireSRWLockExclusive(&g_KillLogLock);
      int i = 0;
      while (i < g_OtherKillCount && g_OtherKills[i] != other)
        ++i;
      if (i == g_OtherKillCount && g_OtherKillCount < 32)
      {
        g_OtherKills[g_OtherKillCount++] = other;
        first = true;
      }
      ReleaseSRWLockExclusive(&g_KillLogLock);
      if (first)
        util::log::Write("Street level: Faith is inside a kill volume the tools don't spare here (data Flags 0x%08X; over "
          "the street: %s): it kills as the game has it", other, g_OverStreet.load() ? "yes" : "no");
    }
    return inside;
  }

  // The assets whose FallThreshold the tools raised, with what it was
  struct Raised
  {
    __int64 Asset;
    float Original;
  };
  SRWLOCK g_RaisedLock = SRWLOCK_INIT;
  Raised g_Raised[8] = {};
  std::atomic<int> g_RaisedCount{ 0 };

  void AdjustFallThreshold(__int64 asset)
  {
    // Ticked, or Faith in the Sky City station's sunken part with Solid street on (or held on: g_SkyHolding)
    bool ticked = g_NoFallDeath.load(std::memory_order_relaxed);
    bool pit = StreetOn() && g_InStationPit.load(std::memory_order_relaxed);
    bool wanted = (ticked || pit) && !g_InVoid.load(std::memory_order_relaxed);
    if (!wanted && g_RaisedCount.load(std::memory_order_relaxed) == 0)
      return;
    __try
    {
      if (*(__int64 const*)asset != Base() + FallAssetVTableRva)
        return;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      return;
    }
    float* threshold = (float*)(asset + 0x40);
    // Already raised: nothing to do (no lock on the animation threads' every call)
    if (wanted && *threshold == NoFallThreshold)
      return;
    float logOriginal = -1.f;
    AcquireSRWLockExclusive(&g_RaisedLock);
    int count = g_RaisedCount.load(std::memory_order_relaxed);
    int i = 0;
    while (i < count && g_Raised[i].Asset != asset)
      ++i;
    if (wanted)
    {
      if (*threshold != NoFallThreshold)
      {
        if (i == count && count < 8)
        {
          g_Raised[count] = { asset, *threshold };
          g_RaisedCount.store(count + 1);
          logOriginal = *threshold;
          *threshold = NoFallThreshold;
        }
        else if (i < count)
        {
          g_Raised[i].Original = *threshold;
          logOriginal = *threshold;
          *threshold = NoFallThreshold;
        }
      }
    }
    else if (i < count)
    {
      if (*threshold == NoFallThreshold)
        *threshold = g_Raised[i].Original;
      g_Raised[i] = g_Raised[count - 1];
      g_RaisedCount.store(count - 1);
    }
    ReleaseSRWLockExclusive(&g_RaisedLock);
    // (the station's: once an asset, as she walks in and out of it)
    static std::atomic<__int64> s_pitLogged{ 0 };
    if (logOriginal >= 0.f && (ticked || s_pitLogged.exchange(asset) != asset))
      util::log::Write("No fall death: the fall check (Ant asset 0x%I64X) had FallThreshold %.2f m; raised while %s", asset,
        logOriginal, ticked ? "ticked" : "Faith is in the Sky City station's sunken part (Solid street)");
  }

  __int64 __fastcall hFallRun(__int64 asset, __int64 info)
  {
    AdjustFallThreshold(asset);
    return oFallRun(asset, info);
  }

  // A hit on one of the tools' bodies whose user data names no live body record (it would crash the game's lookup,
  // as at 21:21:08 on 2026-11-01). The bodies' borrowed ids are put right after each flush, where records die, so
  // this shouldn't happen; if it does, the hit is left as the collectors set it (no object, -1).
  std::atomic<bool> g_UnsafeHitLogged{ false };
  bool BodyHitUnsafe(__int64 world, int body)
  {
    __try
    {
      __int64 havok = *(__int64 const*)(world + WorldHavokOffset);
      if (havok != g_BodiesHavok.load(std::memory_order_relaxed))
        return false;
      __int64 b = *(__int64 const*)(havok + HavokBodiesOffset) + (__int64)body * BodySize;
      unsigned __int64 userData = *(unsigned __int64 const*)(b + BodyUserDataOffset);
      __int64 records = *(__int64 const*)(world + WorldRecordsOffset);
      __int64 record = *(__int64 const*)(records + (__int64)(userData & 0xFFFF) * 8);
      return record < 0x10000 || *(__int64 const*)record != Base() + BodyRecordVTableRva;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      return true;
    }
  }

  // flags: the query's (a pointer; its byte's bit 0 asks for the material, bit 1 the shape key's part, bit 2 hit + 0x48)
  __int64 __fastcall hHitBody(__int64 world, __int64 flags, __int64 hit, __int64 out)
  {
    unsigned char surface = 0;
    if (g_BodyCount.load(std::memory_order_relaxed) > 0)
    {
      int body = -1;
      __try
      {
        body = *(int const*)(hit + 0x40);
      }
      __except (EXCEPTION_EXECUTE_HANDLER)
      {
      }
      surface = OurSurface(body);
      if (surface && BodyHitUnsafe(world, body))
      {
        if (!g_UnsafeHitLogged.exchange(true))
          util::log::Warning("Street level: a hit on the tools' body %d found its borrowed ids gone: left without the "
            "game's ids", body);
        return 0;
      }
    }
    if (g_CaptureThread.load(std::memory_order_relaxed) == GetCurrentThreadId())
    {
      __try
      {
        if (g_Capture.Count < 8)
        {
          g_Capture.World = world;
          g_Capture.Bodies[g_Capture.Count++] = *(int const*)(hit + 0x40);
        }
      }
      __except (EXCEPTION_EXECUTE_HANDLER)
      {
      }
    }
    __int64 result = oHitBody(world, flags, hit, out);
    // One of the tools' bodies: its own surface's MaterialDecl where the query asked for the material, where the game
    // put the borrowed record's (0x14351785D - 0x1435178A2: out + 8, or the world's default when it isn't set)
    if (surface)
    {
      __try
      {
        if (*(unsigned char const*)flags & 1)
          *(unsigned*)(out + 8) = SurfaceDecls[surface];
      }
      __except (EXCEPTION_EXECUTE_HANDLER)
      {
      }
    }
    return result;
  }

  // ---- Havok, from the flush (a fault turns the tools' bodies off for good: g_Broken)

  bool MakeShape(float const* halfSize, __int64& shape)
  {
    __int64 base = Base();
    __try
    {
      alignas(16) unsigned char config[0x40] = {};
      ((tBuildConfig)(base + BuildConfigRva))(config);
      alignas(16) float half[4] = { halfSize[0], halfSize[1], halfSize[2], 0.f };
      shape = ((tBoxShape)(base + BoxShapeRva))(half, ConvexRadius, config);
      if (shape < 0x10000)
        return false;
      // A hit's material index comes from the shape's user data (0: the record's first) unless its flags' bit 2
      // says the hit's shape key: a box has none
      if (*(unsigned short const*)(shape + ShapeFlagsOffset) & 4)
        return false;
      *(__int64*)(shape + ShapeUserDataOffset) = 0;
      return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      g_Broken = true;
      return false;
    }
  }

  bool BodyStillOurs(__int64 world, __int64 havok, Made const& m)
  {
    __try
    {
      if (*(__int64 const*)(world + WorldHavokOffset) != havok || m.Id < 0 ||
          (unsigned)m.Id >= (*(unsigned const*)(havok + HavokCapacityOffset) & 0x3FFFFFFF))
        return false;
      __int64 b = *(__int64 const*)(havok + HavokBodiesOffset) + (__int64)m.Id * BodySize;
      return *(__int64 const*)(b + BodyShapeOffset) == m.Shape && *(int const*)(b + BodyIdOffset) == m.Id;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      return false;
    }
  }

  // A static body with the set's settings and ids at the item's rows: made but not added (so nothing can hit it
  // before it has its user data), then added and put in place. noRoom: not made, the game's body buffer has less
  // than BodyReserve free (or its count can't be read as a count)
  bool CreateBody(Bodies const& set, Item const& item, __int64 shape, int& id, bool& noRoom)
  {
    __int64 base = Base();
    noRoom = false;
    __try
    {
      unsigned capacity = *(unsigned const*)(set.Havok + HavokCapacityOffset) & 0x3FFFFFFF;
      unsigned count = *(unsigned const*)(set.Havok + HavokBodyCountOffset);
      if (count > capacity || capacity - count <= BodyReserve)
      {
        noRoom = true;
        return false;
      }
      alignas(16) unsigned char cinfo[0x80] = {};
      ((tBodyCinfo)(base + BodyCinfoRva))(cinfo);
      *(__int64*)(cinfo + CinfoShapeOffset) = shape;
      *(int*)(cinfo + CinfoMotionOffset) = 0;
      *(unsigned char*)(cinfo + CinfoQualityOffset) = set.From.Quality;
      *(unsigned short*)(cinfo + CinfoMaterialOffset) = set.From.Material;
      *(unsigned*)(cinfo + CinfoFilterOffset) = set.From.Filter;
      memcpy(cinfo + CinfoPositionOffset, &item.Rows[12], 3 * sizeof(float));
      id = 0x7FFFFFFF;
      ((tCreateBody)(base + CreateBodyRva))(set.Havok, &id, cinfo, 1);
      ((tStringPtrReset)(base + StringPtrResetRva))(cinfo + CinfoNameOffset);
      if (id < 0 || (unsigned)id >= (*(unsigned const*)(set.Havok + HavokCapacityOffset) & 0x3FFFFFFF))
        return false;
      __int64 b = *(__int64 const*)(set.Havok + HavokBodiesOffset) + (__int64)id * BodySize;
      if (*(__int64 const*)(b + BodyShapeOffset) != shape)
        return false;
      *(unsigned __int64*)(b + BodyUserDataOffset) = set.From.UserData;
      MarkOurs(id, item.Surface);
      alignas(16) float rows[16];
      memcpy(rows, item.Rows, sizeof(rows));
      ((tAddBodies)(base + AddBodiesRva))(set.Havok, &id, 1, 0);
      ((tSetBodyTransform)(base + SetBodyTransformRva))(set.Havok, (unsigned)id, rows, 1);
      return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      g_Broken = true;
      return false;
    }
  }

  bool DestroyBodies(__int64 havok, int const* ids, int count)
  {
    if (count <= 0)
      return true;
    __try
    {
      ((tDestroyBodies)(Base() + DestroyBodiesRva))(havok, ids, count, 1);
      return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      g_Broken = true;
      return false;
    }
  }

  bool SetUserData(__int64 havok, int id, unsigned __int64 userData)
  {
    __try
    {
      __int64 b = *(__int64 const*)(havok + HavokBodiesOffset) + (__int64)id * BodySize;
      *(unsigned __int64*)(b + BodyUserDataOffset) = userData;
      return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      return false;
    }
  }

  // Any static body of this world (not avoid) whose record can lend its ids, by going through the records
  bool ScanForDonor(__int64 world, int avoid, Donor& out)
  {
    __int64 records = 0;
    __try
    {
      records = *(__int64 const*)(world + WorldRecordsOffset);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      return false;
    }
    if (records < 0x10000)
      return false;
    __int64 vtable = Base() + BodyRecordVTableRva;
    for (int i = 0; i < 0x10000; ++i)
    {
      __int64 record = 0;
      int body = -1;
      __try
      {
        record = *(__int64 const*)(records + (__int64)i * 8);
        if (record >= 0x10000 && *(__int64 const*)record == vtable)
          body = *(int const*)(record + RecordBodyOffset);
      }
      __except (EXCEPTION_EXECUTE_HANDLER)
      {
        break;
      }
      if (body >= 0 && body != avoid && ReadDonor(world, body, out) && out.Record == record)
        return true;
    }
    return false;
  }

  // All of the set destroyed (its ids no longer marked) and the set emptied
  void DestroyAll(Bodies& set)
  {
    std::vector<int> ids;
    ids.reserve(set.ByKey.size());
    for (auto const& kv : set.ByKey)
      ids.push_back(kv.second.Id);
    DestroyBodies(set.Havok, ids.data(), (int)ids.size());
    for (int id : ids)
      MarkOurs(id, 0);
    set = Bodies();
  }

  // The bodies made and destroyed as the plan has them, in this world's flush, before the game's own work there (the
  // world isn't stepping; the game destroys its bodies here too)
  void ApplyBodies(__int64 world)
  {
    std::shared_ptr<Plan const> plan = TakePlan();
    bool solid = StreetOn();
    AcquireSRWLockExclusive(&g_BodyLock);
    Bodies& set = g_Bodies;
    ULONGLONG now = GetTickCount64();
    char note[256] = "";
    bool changed = false;
    long long lap = Ticks(), parts[FlushParts] = {};
    // The set's keys sorted (handed over each time the set changes, here and in CheckAfterFlush), gone through
    // alongside the plan's instead of looking each one up (2026-11-03: the same bodies destroyed and made, in the same
    // order). has: every key of the set is in it; all: and nothing else, but keys the plan doesn't have
    std::shared_ptr<std::vector<unsigned __int64> const> sorted = set.Sorted;
    bool sortedHas = sorted && sorted->size() == set.ByKey.size(), sortedAll = sortedHas;
    if (!set.ByKey.empty() && set.World == world)
    {
      set.LastFlush = now;
      // Those that aren't the tools' any more (the world changed under them) are let go
      int gone = 0;
      for (auto it = set.ByKey.begin(); it != set.ByKey.end();)
        if (!BodyStillOurs(world, set.Havok, it->second))
        {
          MarkOurs(it->second.Id, 0);
          it = set.ByKey.erase(it);
          ++gone;
        }
        else
          ++it;
      if (gone)
      {
        sprintf_s(note, "%d of the tools' bodies aren't theirs any more (the world changed); let go", gone);
        changed = true;
        sortedAll = false;
      }
      long long t = Ticks();
      parts[PartCheck] = t - lap;
      lap = t;
      // The ones the plan doesn't want, or all of them (off, a fault, or the plan is for another world). Wanted or
      // not, from the set's sorted keys against the plan's: the set is gone through for their ids only if some aren't
      bool keep = !g_Broken.load() && solid && plan && plan->From.World == world && plan->From.Havok == set.Havok;
      std::vector<int> ids;
      std::vector<unsigned __int64> keys, unwanted;
      bool look = true;
      if (keep && sortedHas)
      {
        NotIn(*sorted, plan->SortedKeys, unwanted);
        look = !unwanted.empty();
      }
      if (look)
        for (auto const& kv : set.ByKey)
          if (!keep || (sortedHas ? std::binary_search(unwanted.begin(), unwanted.end(), kv.first) :
              !Contains(plan->SortedKeys, kv.first)))
          {
            ids.push_back(kv.second.Id);
            keys.push_back(kv.first);
          }
      if (!ids.empty())
      {
        g_BodiesDestroyed.fetch_add((unsigned)ids.size(), std::memory_order_relaxed);
        bool done = DestroyBodies(set.Havok, ids.data(), (int)ids.size());
        // Faulted or not, they aren't the tools' any more (after a fault nothing more is made)
        for (size_t i = 0; i < ids.size(); ++i)
        {
          MarkOurs(ids[i], 0);
          set.ByKey.erase(keys[i]);
        }
        changed = true;
        if (!done)
          sprintf_s(note, "destroying %d of the tools' bodies faulted: Solid street stays off", (int)ids.size());
        if (!keep)
          sortedAll = false;
      }
      t = Ticks();
      parts[PartDrop] = t - lap;
      lap = t;
    }
    else if (!set.ByKey.empty() && set.World != world && plan && plan->From.World == world &&
      now - set.LastFlush > GoneWorldMs)
    {
      sprintf_s(note, "the world of the tools' %d bodies isn't flushed any more (gone with its level); let go",
        (int)set.ByKey.size());
      for (auto const& kv : set.ByKey)
        MarkOurs(kv.second.Id, 0);
      set = Bodies();
      changed = true;
      sortedAll = false;
    }
    if (set.ByKey.empty() && set.World)
    {
      set = Bodies();
      sortedAll = false;
    }
    // Make what's wanted and not made yet, nearest first. A new set takes the plan's ids while their record lives
    // (it can have gone in the last flush); a set's ids are checked right after every flush (CheckAfterFlush)
    if (!g_Broken.load() && solid && plan && plan->From.World == world && (set.ByKey.empty() || set.World == world))
    {
      if (set.ByKey.empty() && DonorStillThere(plan->From))
      {
        set.World = world;
        set.Havok = plan->From.Havok;
        set.From = plan->From;
        set.LastFlush = now;
      }
      if (set.World == world && set.Havok == plan->From.Havok && DonorStillThere(set.From))
      {
        int made = 0, failed = 0;
        // The items whose keys aren't the set's (the plan's sorted keys against the set's), else all; the others
        // would be passed over as made
        size_t count = plan->Items.size();
        std::vector<unsigned char> notMade;
        bool any = true;
        if (sortedAll && plan->SortedKeys.size() == count && plan->SortedItems.size() == count)
        {
          notMade.assign(count, 0);
          any = false;
          size_t j = 0;
          for (size_t i = 0; i < count; ++i)
          {
            unsigned __int64 key = plan->SortedKeys[i];
            while (j < sorted->size() && (*sorted)[j] < key)
              ++j;
            if (j == sorted->size() || (*sorted)[j] != key)
            {
              notMade[plan->SortedItems[i]] = 1;
              any = true;
            }
          }
        }
        for (size_t index = 0; any && index < count; ++index)
        {
          if (!notMade.empty() && !notMade[index])
            continue;
          Item const& item = plan->Items[index];
          if (made >= MaxMakePerFlush || g_Broken.load())
            break;
          if (!item.Make || set.ByKey.count(item.Key) || item.Shape >= g_Shapes.size() || g_Shapes[item.Shape] == -1)
            continue;
          __int64& shape = g_Shapes[item.Shape];
          if (!shape)
          {
            float half[3];
            ShapeHalf(item.Shape, half);
            if (!MakeShape(half, shape))
            {
              shape = -1;
              util::log::Warning("Street level: the box shape %u (%.2f x %.2f x %.2f m) couldn't be made%s", item.Shape,
                half[0] * 2.f, half[1] * 2.f, half[2] * 2.f, g_Broken.load() ? " (it faulted: Solid street stays off)" :
                "");
              continue;
            }
          }
          int id = -1;
          bool noRoom = false;
          if (CreateBody(set, item, shape, id, noRoom))
          {
            set.ByKey[item.Key] = { id, shape };
            ++made;
            g_BodiesMade.fetch_add(1, std::memory_order_relaxed);
            changed = true;
          }
          else if (noRoom)
          {
            if (!g_NoRoomLogged.exchange(true))
              util::log::Warning("Street level: the game's physics has less than %u bodies of room left: the tools make "
                "no more ground or props while it's so (logged once)", BodyReserve);
            break;
          }
          else
            ++failed;
        }
        if (made && !set.Logged)
        {
          set.Logged = true;
          util::log::Write("Street level: the tools' ground and props are made in the game's physics with the ids of "
            "body %d (filter 0x%X, material %u, quality %u): %d bodies in the first flush", set.From.Body,
            set.From.Filter, set.From.Material, set.From.Quality, made);
        }
        if (failed && !set.FailLogged)
        {
          set.FailLogged = true;
          util::log::Warning("Street level: %d of the tools' bodies couldn't be made%s (logged once)", failed,
            g_Broken.load() ? " (a call faulted: Solid street stays off)" : " (no id, or not the shape given)");
        }
        parts[PartMake] = Ticks() - lap;
      }
    }
    if (g_Broken.load() && !set.ByKey.empty())
    {
      // After a fault: all of them go (if destroying faults too, they're let go)
      DestroyAll(set);
      changed = true;
    }
    NoteBodies();
    if (changed)
      PublishMade();
    if (set.World == world)
    {
      g_FlushesOfBodies.fetch_add(1, std::memory_order_relaxed);
      for (int p = 0; p < FlushParts; ++p)
        g_FlushPartTicks[p].fetch_add(parts[p], std::memory_order_relaxed);
    }
    ReleaseSRWLockExclusive(&g_BodyLock);
    if (note[0])
      util::log::Write("Street level: %s", note);
  }

  // After the game's own deferred work in this flush: it frees records there (0x1434F4C80 puts the slot back on
  // the free list), so the bodies' borrowed ids are checked now, before anything can hit them, and given another
  // live record's or the bodies destroyed
  void CheckAfterFlush(__int64 world)
  {
    char note[256] = "";
    AcquireSRWLockExclusive(&g_BodyLock);
    Bodies& set = g_Bodies;
    if (!set.ByKey.empty() && set.World == world && !DonorStillThere(set.From))
    {
      std::shared_ptr<Plan const> plan = TakePlan();
      Donor d;
      bool found = false;
      if (plan && plan->From.World == world && plan->From.Havok == set.Havok && plan->From.Body != set.From.Body &&
          DonorStillThere(plan->From))
      {
        d = plan->From;
        found = true;
      }
      else
        found = ScanForDonor(world, set.From.Body, d) && d.Havok == set.Havok;
      bool rebound = found;
      if (found)
        for (auto const& kv : set.ByKey)
          if (!SetUserData(set.Havok, kv.second.Id, d.UserData))
          {
            rebound = false;
            break;
          }
      if (rebound)
      {
        sprintf_s(note, "body %d's record went (its ids were on the tools' %d bodies); they take body %d's",
          set.From.Body, (int)set.ByKey.size(), d.Body);
        set.From = d;
      }
      else
      {
        int count = (int)set.ByKey.size(), from = set.From.Body;
        DestroyAll(set);
        sprintf_s(note, "body %d's record went (its ids were on the tools' %d bodies) and no other could lend its ids: "
          "they're destroyed%s", from, count, g_Broken.load() ? " (it faulted: Solid street stays off)" : "");
        PublishMade();
      }
    }
    NoteBodies();
    ReleaseSRWLockExclusive(&g_BodyLock);
    if (note[0])
      util::log::Write("Street level: %s", note);
  }

  namespace skycity
  {
    void Flush(__int64 world);
  }

  __int64 __fastcall hPhysicsFlush(__int64 world)
  {
    bool active = StreetOn() || g_BodyCount.load(std::memory_order_relaxed) > 0;
    long long spent = 0;
    if (active)
    {
      long long t0 = Ticks();
      ApplyBodies(world);
      spent += Ticks() - t0;
    }
    // The Sky City reshape's changes to zone 55's collision, after the boxes standing in for it are made
    skycity::Flush(world);
    __int64 result = oPhysicsFlush(world);
    // After it: records it freed can't stay on the tools' bodies
    if (g_BodyCount.load(std::memory_order_relaxed) > 0)
    {
      long long t0 = Ticks();
      CheckAfterFlush(world);
      long long after = Ticks() - t0;
      spent += after;
      g_FlushPartTicks[PartAfter].fetch_add(after, std::memory_order_relaxed);
    }
    if (active)
    {
      g_FlushTicks.fetch_add(spent, std::memory_order_relaxed);
      g_FlushCount.fetch_add(1, std::memory_order_relaxed);
      long long most = g_FlushMaxTicks.load(std::memory_order_relaxed);
      while (spent > most && !g_FlushMaxTicks.compare_exchange_weak(most, spent, std::memory_order_relaxed))
        ;
    }
    return result;
  }

  // ---- The camera hook's part

  Donor g_Donor;                // the camera hook's only
  __int64 g_WorldSeen = 0;      // the physics world the sweeps last hit in
  ULONGLONG g_LastProbe = 0;
  bool g_PlanOut = false;       // a plan is handed over
  std::vector<unsigned __int64> g_PlannedKeys;
  Donor g_PlannedFrom;
  ULONGLONG g_LastPublish = 0;
  std::vector<unsigned __int64> g_PlannedLowered;   // the level tiles at her feet in the plan handed over
  int g_LoweredLogged = 0, g_LetGoLogged = 0;

  void NotePlannedLowered(std::vector<Item> const& items)
  {
    g_PlannedLowered.clear();
    for (Item const& it : items)
      if ((it.Key & TileBit) && (it.Key & LoweredBit))
        g_PlannedLowered.push_back(it.Key);
  }

  // Sweeps round Faith; the first body hit that can lend its ids
  bool Probe(float const* feet, Donor& out)
  {
    struct Line { float from[3], to[3]; };
    Line lines[20];
    int n = 0;
    lines[n++] = { { feet[0], feet[1] + 1.5f, feet[2] }, { feet[0], feet[1] - 3.f, feet[2] } };
    for (int ring = 0; ring < 2; ++ring)
    {
      float y = feet[1] + (ring ? 6.f : 1.2f);
      for (int k = 0; k < 8; ++k)
      {
        float a = k * 0.7853982f + ring * 0.3926991f;
        lines[n++] = { { feet[0], y, feet[2] }, { feet[0] + 80.f * cosf(a), y, feet[2] + 80.f * sinf(a) } };
      }
    }
    lines[n++] = { { feet[0], feet[1] + 2.f, feet[2] }, { feet[0], feet[1] + 80.f, feet[2] } };
    for (int i = 0; i < n; ++i)
    {
      g_Capture = Capture();
      g_CaptureThread.store(GetCurrentThreadId());
      float fraction = 1.f, normal[3];
      bool hit = collision::Sweep(lines[i].from, lines[i].to, 0.2f, &fraction, normal);
      g_CaptureThread.store(0);
      if (g_Capture.Count > 0)
        g_WorldSeen = g_Capture.World;
      if (!hit)
        continue;
      for (int k = 0; k < g_Capture.Count; ++k)
      {
        char const* why = "";
        if (ReadDonor(g_Capture.World, g_Capture.Bodies[k], out, &why))
          return true;
        // The first few turned down, for CT.log
        static int s_logged = 0;
        if (s_logged < 12)
        {
          ++s_logged;
          util::log::Write("Street level: body %d (hit by a sweep round Faith) can't lend the ground its ids: %s",
            g_Capture.Bodies[k], why);
        }
      }
    }
    return false;
  }

  // The ground round her on the 0.5 m grid: what covers each square
  const unsigned char Taken = 1;       // a tile of the plan
  struct Cover
  {
    static constexpr int Side = 64;    // 32 m: the keep radius and a 2 m tile's side each way
    int Gx0 = 0, Gz0 = 0;
    unsigned char Cells[Side * Side] = {};

    void Reset(float x, float z)
    {
      Gx0 = (int)floorf(x / GridStep) - Side / 2;
      Gz0 = (int)floorf(z / GridStep) - Side / 2;
      memset(Cells, 0, sizeof(Cells));
    }

    unsigned char Get(Tile const& t) const
    {
      unsigned char v = 0;
      int span = TileSpan(t.Level);
      for (int z = t.Gz - Gz0; z < t.Gz - Gz0 + span; ++z)
        for (int x = t.Gx - Gx0; x < t.Gx - Gx0 + span; ++x)
          if (x >= 0 && z >= 0 && x < Side && z < Side)
            v |= Cells[z * Side + x];
      return v;
    }

    void Mark(Tile const& t, unsigned char v)
    {
      int span = TileSpan(t.Level);
      for (int z = t.Gz - Gz0; z < t.Gz - Gz0 + span; ++z)
        for (int x = t.Gx - Gx0; x < t.Gx - Gx0 + span; ++x)
          if (x >= 0 && z >= 0 && x < Side && z < Side)
            Cells[z * Side + x] |= v;
    }
  };

  // The highest road a point can take: her feet + the reach at its distance from her
  float Limit(float const* feet, float x, float z)
  {
    float dx = x - feet[0], dz = z - feet[2];
    return feet[1] + (std::min)(ReachBase + ReachPerMetre * sqrtf(dx * dx + dz * dz), ReachMax);
  }

  // The tiles of one square (its corner gx, gz on the 0.5 m grid): one when the road at its middle and its corners
  // lies on one plane (within FitTolerance), else its quarters, down to 0.5 m
  void PlanSquare(int level, int gx, int gz, float const* feet, std::vector<unsigned __int64>& out)
  {
    float size = TileSize(level);
    float cx = (float)gx * GridStep + size * 0.5f, cz = (float)gz * GridStep + size * 0.5f;
    float height = 0.f, normal[3];
    unsigned tri = 0;
    bool centre = g_Roads.Height(cx, cz, Limit(feet, cx, cz), height, normal, &tri);
    bool fits = centre;
    if (centre)
    {
      float up[3];
      TileUp(normal, up);
      for (int k = 0; k < 4 && fits; ++k)
      {
        float px = cx + ((k & 1) ? 1.f : -1.f) * size * CornerInset;
        float pz = cz + ((k & 2) ? 1.f : -1.f) * size * CornerInset;
        float h = 0.f, n[3];
        float want = height - (up[0] * (px - cx) + up[2] * (pz - cz)) / up[1];
        fits = g_Roads.Height(px, pz, Limit(feet, px, pz), h, n) && fabsf(h - want) <= FitTolerance;
      }
    }
    if (centre && (fits || level == TileMaxLevel))
    {
      out.push_back(TileKey(level, gx, gz, false, tri));
      return;
    }
    if (level == TileMaxLevel)
      return;
    int half = TileSpan(level) / 2;
    for (int q = 0; q < 4; ++q)
      PlanSquare(level + 1, gx + (q & 1) * half, gz + (q >> 1) * half, feet, out);
  }

  // What the planning found, for the status line
  struct PlanNotes
  {
    int Tiles = 0, Props = 0;
    bool PropsCapped = false;   // more props round her than MaxPropItems: the nearest taken
    bool Under = false;         // a tile of the plan under her, its top at or under her feet
    float UnderTop = 0.f;       // the highest
    bool OverHer = false;       // the road over her feet where she is, its tile waiting
  };

  Item MakeItem(unsigned __int64 key, unsigned shape, float distance, bool make, unsigned char surface)
  {
    Item it;
    it.Key = key;
    it.Shape = shape;
    it.Distance = distance;
    it.Make = make;
    it.Surface = surface;
    return it;
  }

  // fall: how far her feet went down since the frame before (0 going up or level)
  void PlanTiles(float const* feet, float fall, std::vector<unsigned __int64> const& made, std::vector<Item>& items,
    PlanNotes& notes)
  {
    float fx = feet[0], fy = feet[1], fz = feet[2];
    static Cover s_cover;       // the camera hook's only
    Cover& cover = s_cover;
    cover.Reset(fx, fz);
    // (its work lists are kept from call to call as cover is: cleared, not freed and made again)
    static std::vector<Tile> s_planned;
    std::vector<Tile>& planned = s_planned;   // the tiles added, as items[first + i]
    planned.clear();
    size_t first = items.size();
    // Each one the flush may make only if it's away from her or its top is at or under her feet near her: one kept
    // because it's made isn't made again near her (if it went in a flush between)
    auto mayMake = [&](Tile const& t, float distance)
    {
      return distance > NearDistance || TileTopNear(t, fx, fz) <= fy + MakeTolerance;
    };
    auto add = [&](unsigned __int64 key, Tile const& t, float distance)
    {
      Item it = MakeItem(key, t.Shape, distance, mayMake(t, distance), t.Surface);
      TileRows(t, it.Rows);
      items.push_back(it);
      planned.push_back(t);
      cover.Mark(t, Taken);
    };

    // 1. The tiles made. One she's in or under (her feet BelowLimit under its top where she is) is let go; the others
    //    stay while she's near them (step 4) or while nothing the plan has covers their ground (step 7)
    struct Kept
    {
      Tile T;
      unsigned __int64 Key;
      float Distance;
    };
    static std::vector<Kept> s_nearMade, s_farMade;
    static std::vector<unsigned __int64> s_letGo;
    std::vector<Kept>& nearMade = s_nearMade;
    std::vector<Kept>& farMade = s_farMade;
    std::vector<unsigned __int64>& letGo = s_letGo;
    nearMade.clear();
    farMade.clear();
    letGo.clear();
    for (unsigned __int64 key : made)
    {
      Kept k;
      k.Key = key;
      if (!(key & TileBit) || !DecodeTile(key, k.T))
        continue;
      k.Distance = TileDistance(k.T, fx, fz);
      if (k.Distance > TileKeepRadius)
        continue;
      float top = TileTopAt(k.T, fx, fz);
      if (k.Distance == 0.f && top > fy + BelowLimit)
      {
        letGo.push_back(key);
        if (g_LetGoLogged < 20)
        {
          ++g_LetGoLogged;
          util::log::Write("Street level: Faith is %.2f m under the top of the tile she's over (in it or below it; x %.1f "
            "z %.1f): it's let go", top - fy, fx, fz);
        }
        continue;
      }
      if (k.Distance <= NearDistance)
        nearMade.push_back(k);
      else
        farMade.push_back(k);
    }
    std::sort(letGo.begin(), letGo.end());

    // 2. The tiles the road wants within TileRadius: each point of a square takes the highest road no higher than her
    //    feet + the reach at its distance
    static std::vector<unsigned __int64> s_wanted;
    std::vector<unsigned __int64>& wanted = s_wanted;
    wanted.clear();
    int cx0 = (int)floorf((fx - TileRadius) / TileCell), cx1 = (int)floorf((fx + TileRadius) / TileCell);
    int cz0 = (int)floorf((fz - TileRadius) / TileCell), cz1 = (int)floorf((fz + TileRadius) / TileCell);
    for (int cz = cz0; cz <= cz1; ++cz)
      for (int cx = cx0; cx <= cx1; ++cx)
      {
        int gx = cx * TileSpan(0), gz = cz * TileSpan(0);
        if (!GridFits(gx, gz, 0))
          continue;
        float x0 = (float)cx * TileCell, z0 = (float)cz * TileCell;
        float dx = (std::max)((std::max)(x0 - fx, fx - (x0 + TileCell)), 0.f);
        float dz = (std::max)((std::max)(z0 - fz, fz - (z0 + TileCell)), 0.f);
        float d = sqrtf(dx * dx + dz * dz);
        if (d > TileRadius)
          continue;
        PlanSquare(0, gx, gz, feet, wanted);
      }

    // 3. Each (but one let go): in the plan. One near her whose top is over her feet is kept if it's there but not
    //    made (Make false): the plan never takes away a tile only for being near her (one made from a plan before,
    //    that she may stand on by now: the offline test, a dip of her feet at the start)
    // The one to lower if nothing holds her: the nearest new one within HeldMargin (her square's middle can miss the
    // road at a ragged edge: then the one beside her)
    bool lowerable = false;
    Tile lower;
    float lowerRoad = 0.f, lowerDistance = 0.f;
    for (unsigned __int64 key : wanted)
    {
      Tile t;
      if (Contains(letGo, key) || !DecodeTile(key, t))
        continue;
      float distance = TileDistance(t, fx, fz);
      if (distance <= HeldMargin && !Contains(made, key) && !mayMake(t, distance))
      {
        float top = TileTopAt(t, fx, fz);
        if (distance == 0.f && top > fy + MakeTolerance)
          notes.OverHer = true;
        if (top <= fy + LowerMax && (!lowerable || distance < lowerDistance))
        {
          lowerable = true;
          lower = t;
          lowerRoad = top;
          lowerDistance = distance;
        }
      }
      add(key, t, distance);
    }
    static std::vector<unsigned __int64> s_wantedIn;
    std::vector<unsigned __int64>& wantedIn = s_wantedIn;
    wantedIn.clear();
    for (size_t i = first; i < items.size(); ++i)
      wantedIn.push_back(items[i].Key);
    std::sort(wantedIn.begin(), wantedIn.end());

    // 4. The made ones near her that the road doesn't want now stay while she's near them
    for (Kept const& k : nearMade)
      if (!Contains(wantedIn, k.Key))
        add(k.Key, k.T, k.Distance);
    // A level tile at her feet asked for and not made yet stays asked for while she's near it and not under it (a
    // new one at her feet each frame till then would take away the one made in between: the offline test)
    for (unsigned __int64 key : g_PlannedLowered)
    {
      Tile t;
      if (Contains(made, key) || !DecodeTile(key, t))
        continue;
      float distance = TileDistance(t, fx, fz);
      if (distance <= NearDistance && !(distance == 0.f && TileTopAt(t, fx, fz) > fy + BelowLimit))
        add(key, t, distance);
    }

    // 5. Held: a tile of the plan under her with its top at her feet or a little under them, one that's there or is
    //    to be made (not one kept only if it's there)
    bool held = false;
    for (size_t i = 0; i < planned.size(); ++i)
    {
      Tile const& t = planned[i];
      Item const& it = items[first + i];
      if (TileDistance(t, fx, fz) > HeldMargin || !(it.Make || Contains(made, it.Key)))
        continue;
      float top = TileTopAt(t, fx, fz);
      if (top > fy + MakeTolerance)
        continue;
      if (top >= fy - HeldBelow)
        held = true;
      if (!notes.Under || top > notes.UnderTop)
      {
        notes.Under = true;
        notes.UnderTop = top;
      }
    }

    // 6. Nothing holds her and the road is a little over her feet (the top of stairs short of the street): a level
    //    tile at her feet in its square, under them by as far as she can drop before the flush makes it (two frames
    //    at the drop of the last: the offline test, running fast downhill)
    if (!held && lowerable)
    {
      int topCm = (int)floorf((fy - LoweredBelow - 2.f * fall) * 100.f);
      Tile t;
      unsigned __int64 key = TileKey(lower.Level, lower.Gx, lower.Gz, true, LoweredValue(lower.Surface, topCm));
      bool already = false;
      for (size_t i = first; i < items.size(); ++i)
        already = already || items[i].Key == key;
      if (topCm > -TopOffset && topCm < TopOffset && !already && DecodeTile(key, t))
      {
        add(key, t, lowerDistance);
        notes.Under = true;
        notes.UnderTop = t.Top[1];
        if (g_LoweredLogged < 30)
        {
          ++g_LoweredLogged;
          util::log::Write("Street level: a level tile at Faith's feet (y %.2f; the road is %.2f m over them there, x %.1f "
            "z %.1f, and no tile held her)", t.Top[1], lowerRoad - fy, fx, fz);
        }
      }
    }

    // 7. The made ones further out stay where nothing the plan has covers their ground (where the road wants none now)
    for (Kept const& k : farMade)
      if (!Contains(wantedIn, k.Key) && !(cover.Get(k.T) & Taken))
        add(k.Key, k.T, k.Distance);
    notes.Tiles = (int)planned.size();
  }

  // ---- The props round her: a box's distance from her feet (to its box in the world's axes), across and up or down
  struct Near { float Distance; unsigned Index; bool Make; };
  bool NearerFirst(Near const& a, Near const& b) { return a.Distance < b.Distance; }

  // (B: a Props::Box, or its copy in the cache, CachedBox)
  template <class B>
  void BoxDistance(B const& b, float fx, float fy, float fz, float& d, float& dy)
  {
    float dx = (std::max)((std::max)(b.Min[0] - fx, fx - b.Max[0]), 0.f);
    float dz = (std::max)((std::max)(b.Min[2] - fz, fz - b.Max[2]), 0.f);
    dy = (std::max)((std::max)(b.Min[1] - fy, fy - b.Max[1]), 0.f);
    d = sqrtf(dx * dx + dz * dz);
  }

  // Whether box i (b: it, or its copy in the cache) is wanted with her feet at fx, fy, fz, and its distance
  template <class B>
  bool Wanted(B const& b, unsigned i, float fx, float fy, float fz, std::vector<unsigned __int64> const& made, Near& out)
  {
    if (b.Min[0] > 1e29f || !g_GroupOn[b.Group])
      return false;
    float d, dy;
    BoxDistance(b, fx, fy, fz, d, dy);
    if (d > PropKeepRadius || dy > PropKeepY)
      return false;
    // Never made round her (her feet to her head against its box), unless its top is at or under her feet (a
    // floor she's on: it can't hold her inside it)
    bool make = !(d <= NearDistance && fy + PropHeight > b.Min[1] && fy - 0.1f < b.Max[1]) ||
      b.Max[1] <= fy + MakeTolerance;
    // (made looked at only for those it decides: most are within reach)
    if ((d > PropRadius || dy > PropReachY || !make) && !Contains(made, (unsigned __int64)i))
      return false;
    out = { sqrtf(d * d + dy * dy), i, make };
    return true;
  }

  // Each box whose cells come within radius (across) of x / z given to look once (the wide ones too), ring by ring
  // out from her cell. found: what look keeps; once it has more than want and want of them are no further than the
  // next ring can be (a box only there is outside the square of the rings done, at least the gap to its side away),
  // the rest can't be nearer: the search stops and gives that gap; else -1. Update's thread only (Seen, Stamp)
  template <class Look>
  float RingSearch(float fx, float fz, float radius, size_t want, std::vector<Near> const& found, Look look)
  {
    Props& P = g_Props;
    if (++P.Stamp == 0)
    {
      std::fill(P.Seen.begin(), P.Seen.end(), 0u);
      P.Stamp = 1;
    }
    auto once = [&](unsigned i)
    {
      if (P.Seen[i] != P.Stamp)
      {
        P.Seen[i] = P.Stamp;
        look(i);
      }
    };
    for (unsigned i : P.Wide)
      once(i);
    if (P.GridNX <= 0)
      return -1.f;
    int cx = (int)floorf((fx - P.GridX0) / P.CellStep), cz = (int)floorf((fz - P.GridZ0) / P.CellStep);
    int rings = (int)ceilf(radius / P.CellStep) + 1;
    for (int r = 0; r <= rings; ++r)
    {
      for (int ci = cx - r; ci <= cx + r; ++ci)
      {
        if (ci < 0 || ci >= P.GridNX)
          continue;
        bool side = ci == cx - r || ci == cx + r;
        for (int ck = cz - r; ck <= cz + r; ck += side ? 1 : 2 * r)
        {
          // (a cell all further than radius away has nothing for her)
          float gx = (std::max)((std::max)(P.GridX0 + ci * P.CellStep - fx, fx - (P.GridX0 + (ci + 1) * P.CellStep)), 0.f);
          float gz = (std::max)((std::max)(P.GridZ0 + ck * P.CellStep - fz, fz - (P.GridZ0 + (ck + 1) * P.CellStep)), 0.f);
          if (ck >= 0 && ck < P.GridNZ && gx * gx + gz * gz <= radius * radius)
          {
            size_t c = (size_t)ci * P.GridNZ + ck;
            for (unsigned n = P.CellStart[c]; n < P.CellStart[c + 1]; ++n)
              once(P.CellBoxes[n]);
          }
          if (r == 0)
            break;
        }
      }
      // (more than want already: the whole search would have more too)
      if (found.size() > want && r < rings)
      {
        float gap = (std::min)((std::min)(P.GridX0 + (cx + r + 1) * P.CellStep - fx, fx - (P.GridX0 + (cx - r) * P.CellStep)),
          (std::min)(P.GridZ0 + (cz + r + 1) * P.CellStep - fz, fz - (P.GridZ0 + (cz - r) * P.CellStep)));
        size_t nearer = 0;
        for (Near const& f : found)
          nearer += f.Distance <= gap;
        if (nearer >= want)
          return gap;
      }
    }
    return -1.f;
  }

  // The boxes round where she was, gathered now and then so each frame looks at those only (2026-11-02, the user:
  // "find if anything costs frames"): every box within PropKeepRadius / PropKeepY + CacheMargin of At, or the nearest
  // CacheCount of them. A frame's are taken from it only when that's what the whole search would give: she's within
  // CacheMargin of At, and either it has every box within reach or the nearest MaxPropItems wanted are nearer than
  // Reach less how far she's moved (a box's distance changes no more than she moves, and one not kept was at least
  // Reach from At); else it's gathered again where she is, and failing that the whole search is done
  const float CacheMargin = 2.f;
  const size_t CacheCount = 8000;
  // What a frame reads of a box kept, copied side by side (2026-11-03: read from the 2.8 million boxes they were
  // 8,000 places apart in 224 MB)
  struct CachedBox
  {
    float Min[3], Max[3];
    unsigned Index;
    unsigned char Group;
  };
  struct PropCache
  {
    bool Valid = false;
    float At[3] = {};
    bool Capped = false;          // false: every box within reach of At is kept
    float Reach = 0.f;            // Capped: a box not kept was at least this far from At
    std::vector<CachedBox> Boxes;
  };
  PropCache g_PropCache;
  unsigned g_PropGathers = 0, g_PropSearches = 0;   // for the timing (Update's thread)

  void GatherProps(float const* at)
  {
    static std::vector<Near> s_all;
    std::vector<Near>& all = s_all;
    all.clear();
    float fx = at[0], fy = at[1], fz = at[2];
    float gap = RingSearch(fx, fz, PropKeepRadius + CacheMargin, CacheCount, all, [&](unsigned i)
    {
      Props::Box const& b = g_Props.Boxes[i];
      if (b.Min[0] > 1e29f || !g_GroupOn[b.Group])   // (the cache is dropped when a group changes)
        return;
      float d, dy;
      BoxDistance(b, fx, fy, fz, d, dy);
      if (d <= PropKeepRadius + CacheMargin && dy <= PropKeepY + CacheMargin)
        all.push_back({ sqrtf(d * d + dy * dy), i, false });
    });
    PropCache& C = g_PropCache;
    C.Capped = gap >= 0.f || all.size() > CacheCount;
    C.Reach = gap >= 0.f ? gap : FLT_MAX;
    if (all.size() > CacheCount)
    {
      std::nth_element(all.begin(), all.begin() + CacheCount, all.end(), NearerFirst);
      C.Reach = (std::min)(C.Reach, all[CacheCount].Distance);
      all.resize(CacheCount);
    }
    C.Boxes.clear();
    for (Near const& n : all)
    {
      Props::Box const& b = g_Props.Boxes[n.Index];
      CachedBox c;
      memcpy(c.Min, b.Min, sizeof(c.Min));
      memcpy(c.Max, b.Max, sizeof(c.Max));
      c.Index = n.Index;
      c.Group = b.Group;
      C.Boxes.push_back(c);
    }
    C.At[0] = fx; C.At[1] = fy; C.At[2] = fz;
    C.Valid = true;
    ++g_PropGathers;
  }

  void PlanProps(float const* feet, std::vector<unsigned __int64> const& made, std::vector<Item>& items,
    PlanNotes& notes)
  {
    float fx = feet[0], fy = feet[1], fz = feet[2];
    Props& P = g_Props;
    PropCache& C = g_PropCache;
    // The boxes wanted round her (the items are made for the nearest only); kept from call to call (Update's thread)
    static std::vector<Near> s_found;
    std::vector<Near>& found = s_found;
    auto fromCache = [&]() -> bool
    {
      float ex = fx - C.At[0], ey = fy - C.At[1], ez = fz - C.At[2];
      float moved = sqrtf(ex * ex + ey * ey + ez * ez);
      if (!C.Valid || !(moved <= CacheMargin))
        return false;
      found.clear();
      Near n;
      for (CachedBox const& c : C.Boxes)
        if (Wanted(c, c.Index, fx, fy, fz, made, n))
          found.push_back(n);
      if (!C.Capped)
        return true;
      if (found.size() <= MaxPropItems)
        return false;
      std::nth_element(found.begin(), found.begin() + (MaxPropItems - 1), found.end(), NearerFirst);
      return found[MaxPropItems - 1].Distance < C.Reach - moved;
    };
    bool more = false;
    if (!fromCache())
    {
      GatherProps(feet);
      if (!fromCache())
      {
        // The whole search, to PropKeepRadius: once MaxPropItems are no further than the next ring, more were there
        ++g_PropSearches;
        found.clear();
        more = RingSearch(fx, fz, PropKeepRadius, MaxPropItems, found, [&](unsigned i)
        {
          Near n;
          if (Wanted(g_Props.Boxes[i], i, fx, fy, fz, made, n))
            found.push_back(n);
        }) >= 0.f;
      }
    }
    // The nearest only, where the boxes are dense
    if (found.size() > MaxPropItems)
    {
      std::nth_element(found.begin(), found.begin() + MaxPropItems, found.end(), NearerFirst);
      found.resize(MaxPropItems);
      more = true;
    }
    notes.PropsCapped = notes.PropsCapped || more;
    notes.Props += (int)found.size();
    items.reserve(items.size() + found.size());
    for (size_t n = 0; n < found.size(); ++n)
    {
      // (the boxes a few ahead asked for now: each is somewhere in 224 MB)
      if (n + 8 < found.size())
      {
        char const* ahead = (char const*)&P.Boxes[found[n + 8].Index];
        _mm_prefetch(ahead, _MM_HINT_T0);
        _mm_prefetch(ahead + offsetof(Props::Box, Rows) + sizeof(float) * 11, _MM_HINT_T0);
      }
      Near const& f = found[n];
      Props::Box const& b = P.Boxes[f.Index];
      Item it = MakeItem((unsigned __int64)f.Index, TileShapeCount + b.Shape, f.Distance, f.Make, b.Surface);
      float const* r = b.Rows;
      for (int k = 0; k < 3; ++k)
      {
        it.Rows[k * 4] = r[k * 3]; it.Rows[k * 4 + 1] = r[k * 3 + 1]; it.Rows[k * 4 + 2] = r[k * 3 + 2];
        it.Rows[k * 4 + 3] = 0.f;
      }
      it.Rows[12] = r[9]; it.Rows[13] = r[10]; it.Rows[14] = r[11]; it.Rows[15] = 1.f;
      items.push_back(it);
    }
  }

  // The last plan and what it was made from (2026-11-13, "tools fixes only": the planning took 0.15 - 0.21 ms of every
  // frame, the same plan again with Faith still or the game paused). A frame whose every input is the last plan's, bit
  // for bit, takes that plan instead of planning anew, which would give the same: her feet and her fall, what the flush
  // has made (the same list: one handed over is never changed, and this one is held so its memory can't become
  // another's), the level tiles at her feet asked for (g_PlannedLowered), the prop groups on, the body whose ids the
  // plan carries, and the props' cache as the planning left it (only a group change drops it between plans). The road
  // and prop data don't change once loaded, and the planning reads no time. A plan that wrote to CT.log isn't taken
  // again (planning anew would write the line again), and one is made anew at least every PlanMemoMs (a safety net).
  // Update's thread only
  struct PlanMemo
  {
    bool Valid = false;
    ULONGLONG At = 0;                     // when it was planned
    float Feet[3] = {};
    float Fall = 0.f;
    std::shared_ptr<std::vector<unsigned __int64> const> Made;
    std::vector<unsigned __int64> Lowered;
    bool GroupOn[GroupCount + 1] = {};
    Donor From;
    std::shared_ptr<Plan const> Last;     // never changed once planned (it can be the one handed over)
    PlanNotes Notes;
  };
  PlanMemo g_PlanMemo;

  void WantNone(char const* status)
  {
    g_PlanAllMade = false;
    g_PlanAllMadeSince = 0;
    // (the next plan is made anew)
    if (g_PlanMemo.Valid || g_PlanMemo.Last)
      g_PlanMemo = PlanMemo();
    if (g_PlanOut)
    {
      PublishPlan(nullptr);
      g_PlanOut = false;
      g_PlannedKeys.clear();
      g_PlannedLowered.clear();
    }
    SetStatus(status, true);
  }
}

// ---- The gates Solid street takes away (2026-11-09; Research_Ground_Level_2026-11-01.md section 22)
//
// The user, Faith standing at a fence gate in a passage of downtown area 5 (-268.8, 0, 39.4): "While solid street is
// active the fence that faith is next to right now should get removed". It's five pieces of the city's static model
// groups (fence_01_128_electric and fence_01_256_electric, 10 cm thick, 3.2 m tall), in two zones' groups, each with
// its own collision; behind it a 3.9 m wide passage to the street (whose kill floor Solid street spares her in).
// Taken away the way Dogen's door is opened (DogenDoor.cpp, its research file): each piece's member found by its mesh,
// its instance count and its first physics part (each set unique in sp_maincity's 2,392 files, offline), its draw
// object checked to be where the data puts it, its collision parts switched off on its group's physics component's
// bodies (slot 60, as the member's Enabled event does; slot 59 back on) and its instance hidden (the group's Visible,
// util::hooks::SetModelGroupMemberVisible; the renderer's handle at + 0x8C, -1 while not drawn). Collision first, then
// hidden; one that can't be hidden gets its collision back (it's gone or there, never one without the other). Back once
// Solid street is off and Faith is clear of it. A zone streamed in again comes as its file has it: looked at each frame
// while one is near or taken away. The same electric fence pieces are used in 546 places, so only these instances.
namespace
{
  namespace gates
  {
    // The game's code and structures, as DogenDoor.cpp reads them (its comments have the decompile)
    const DWORD GameTimeStamp = 0x5762E3B7;
    const __int64 ModelGroupClassRva = 0x27ED720;
    const __int64 ModelGroupVTableRva = 0x1AFBE90;
    const int ClassInstanceListsOffset = 0x60;
    const int EntityLinkOffset = 0x40;
    const int GroupDataOffset = 0x30;
    const int GroupRecordsOffset = 0xE0;
    const int GroupMemberCountOffset = 0xE8;
    const int DataMembersOffset = 0x90;
    const int MemberDataSize = 0x68;
    const int MemberTransformsOffset = 0x08;
    const int MemberMeshOffset = 0x38;
    const int MemberInstancesOffset = 0x40;
    const int MemberFirstPartOffset = 0x48;
    const int MemberPartsOffset = 0x50;
    const int AssetNameOffset = 0x10;
    const int RecordSize = 0x130;
    const int RecordDataOffset = 0x18;
    const int RecordPhysicsOffset = 0x20;
    const int RecordMadeOffset = 0x120;
    const int MaxGroupMembers = 4096;
    const int MaxGroupsWalked = 20000;
    const __int64 PhysicsComponentVTableRva = 0x1B1CE08;
    const __int64 PartOnRva = 0x34DB9D0;
    const __int64 PartOffRva = 0x34DB840;
    const int PartOnSlot = 59, PartOffSlot = 60;
    const __int64 BodyWorldRva = 0x34EE790;
    const __int64 BodyPartOffRva = 0x34EC740;
    const __int64 BodyPartOnRva = 0x34EC8C0;
    const __int64 RecordPartOnRva = 0x34F0100;
    const __int64 RecordPartOffRva = 0x34EFFA0;
    const int RecordPartOnSlot = 81, RecordPartOffSlot = 82;
    const int ComponentBodiesOffset = 0x30;
    const int ComponentManagerOffset = 0x58;
    const int ComponentBodyCountOffset = 0x60;
    const int ManagerWorldsOffset = 0xF0;
    const int RecordPartBitsOffset = 0xB8;
    const int RecordPartCountOffset = 0xC0;
    const int MaxBodies = 16;
    const __int64 StaticRecordPartOnRva = 0x34F00E0;
    const __int64 StaticRecordPartOffRva = 0x34EFF90;
    const __int64 StaticPartOnRva = 0x34EC800;
    const __int64 StaticPartOffRva = 0x34EC670;
    const int StaticRecordPartBitsOffset = 0x68;
    const int StaticRecordPartCountOffset = 0x70;
    const __int64 BodyChangedRva = 0x3F3CD00;
    const __int64 PartObjectRva = 0x34DC6D0;
    const int PartObjectSlot = 66;
    const int ComponentPartObjectsOffset = 0x68;
    const __int64 VisibleRva = 0x318F730;
    const __int64 DescribeRva = 0x3769330;
    const int ObjectRowsOffset = 0x10;
    const int ObjectHandleOffset = 0x8C;
    typedef void(__fastcall* tPartSwitch)(__int64, unsigned int);

    // One piece: its member (its mesh's file name, lower case; its instance count; its first physics part and parts a
    // instance, placed by its physics), which instance, the instance's placement (right, up, forward, position: the
    // rows its draw object has, within RowsTurnEpsilon / RowsPlaceEpsilon) and its collision's box in the world (x0, y0,
    // z0, x1, y1, z1)
    struct Piece
    {
      const char* Mesh;
      int Instances;
      unsigned int First;
      unsigned int Parts;
      int Instance;
      float Rows[4][3];
      float Box[6];
    };
    const int MaxPieces = 5;
    struct Gate
    {
      const char* Name;     // in the log
      float Middle[3];
      int Count;
      Piece Pieces[MaxPieces];
    };
    // Downtown area 5's passage (offline, 2026-11-09: the zones' groups and staticmodelgroup_physics_win32). All five
    // turned the same way, their compound instances' rows; zone 210's member 6 (2 instances, parts 9 - 12) instances 0
    // and 1, zone 211's member 295 (1, 1943 - 1944) and member 6 (4, 30 - 37) instances 1 and 2 (its 0 and 3 are 44 and
    // 52 m away, and stay)
    const float FenceRight[3] = { 0.273959f, 0.f, -0.9617414f }, FenceForward[3] = { 0.9617414f, 0.f, 0.273959f };
    const int GateCount = 1;
    const Gate Gates[GateCount] = {
      { "The fence in downtown area 5's passage (-269, 0, 39)", { -269.4f, 1.5f, 38.75f }, 5, {
        { "fence_01_128_electric_mesh", 2, 9, 2, 0,
          { { FenceRight[0], 0.f, FenceRight[2] }, { 0.f, 1.f, 0.f }, { FenceForward[0], 0.f, FenceForward[2] },
            { -269.0501f, -0.1199809f, 37.50271f } }, { -269.04f, -0.11f, 36.28f, -268.64f, 3.07f, 37.51f } },
        { "fence_01_128_electric_mesh", 2, 9, 2, 1,
          { { FenceRight[0], 0.f, FenceRight[2] }, { 0.f, 1.f, 0.f }, { FenceForward[0], 0.f, FenceForward[2] },
            { -268.8912f, -0.1199809f, 36.94489f } }, { -268.88f, -0.11f, 35.73f, -268.49f, 3.07f, 36.95f } },
        { "fence_01_256_electric_mesh", 1, 1943, 2, 0,
          { { FenceRight[0], 0.f, FenceRight[2] }, { 0.f, 1.f, 0.f }, { FenceForward[0], 0.f, FenceForward[2] },
            { -269.7542f, -0.1199809f, 39.97438f } }, { -269.75f, -0.11f, 37.52f, -269.00f, 3.07f, 39.98f } },
        { "fence_01_128_electric_mesh", 4, 30, 2, 1,
          { { FenceRight[0], 0.f, FenceRight[2] }, { 0.f, 1.f, 0.f }, { FenceForward[0], 0.f, FenceForward[2] },
            { -270.1076f, -0.1199809f, 41.21503f } }, { -270.10f, -0.11f, 40.00f, -269.70f, 3.07f, 41.22f } },
        { "fence_01_128_electric_mesh", 4, 30, 2, 2,
          { { FenceRight[0], 0.f, FenceRight[2] }, { 0.f, 1.f, 0.f }, { FenceForward[0], 0.f, FenceForward[2] },
            { -270.2665f, -0.1199809f, 41.77284f } }, { -270.25f, -0.11f, 40.55f, -269.86f, 3.07f, 41.78f } } } } };
    const float RowsTurnEpsilon = 2e-3f, RowsPlaceEpsilon = 0.02f;
    const float SearchDistance = 60.f;          // its groups looked for while she's this near (and Solid street on)
    const ULONGLONG SearchMs = 2000;
    const float NotFoundLogDistance = 20.f;
    // Back only once her feet are this far (across) from a piece's box, or over or under it: not into her
    const float ClearAcross = 0.6f, ClearOver = 0.3f, FaithHeight = 1.9f;
    const ULONGLONG RetryMs = 500;
    const int MostFailures = 3;

    struct PieceState
    {
      __int64 Group;
      __int64 Data;
      int Members;          // the group's member count
      int Member;
      __int64 Object;       // its draw object, once its rows were found as the data has them
      bool PartsOff;        // the tools took its collision off (since its group was found), not put back yet
      bool Hidden;          // the tools hid it, not shown again yet
      bool Bad;             // its draw object's rows weren't the data's: left as the game has it
      bool GaveUp;          // it couldn't be taken away (or put back): left alone
      int AwayFailures, BackFailures;
      ULONGLONG LastTry;
      bool PartsUnknownLogged;

      bool Touched() const { return PartsOff || Hidden; }
    };
    struct GateState
    {
      PieceState Pieces[MaxPieces];
      ULONGLONG LastSearch;
      bool NotFoundLogged;
      int AwayLogged;       // -1 back as the game has it last logged, 1 taken away, 0 neither
      bool WaitLogged;      // its coming back waits for Faith to be clear of it (logged)
    };
    GateState g_Gates[GateCount] = {};
    int g_Code = 0;         // 0 not checked, 1 the code the tools know, -1 not

    // DogenDoor.cpp's CodeKnown (the same bytes): the part switches, the records' part bits, the draw object, the
    // Visible handler
    bool CodeKnown()
    {
      BYTE const* base = (BYTE const*)g_gameHandle;
      IMAGE_DOS_HEADER const* pDos = (IMAGE_DOS_HEADER const*)base;
      IMAGE_NT_HEADERS const* pNt = (IMAGE_NT_HEADERS const*)(base + pDos->e_lfanew);
      if (pNt->FileHeader.TimeDateStamp != GameTimeStamp)
        return false;
      static const BYTE switchHead[] = { 0x48, 0x89, 0x6C, 0x24, 0x20, 0x56, 0x57, 0x41, 0x56, 0x48, 0x83, 0xEC, 0x20, 0x0F,
        0xB7, 0x69, 0x60, 0x31, 0xF6, 0x41, 0x89, 0xD6, 0x48, 0x89, 0xCF, 0x85, 0xED, 0x74, 0x4C, 0x48, 0x89, 0x5C, 0x24, 0x48,
        0x48, 0x8B, 0x47, 0x30, 0x48, 0x8D, 0x1C, 0xB5, 0x00, 0x00, 0x00, 0x00, 0x48, 0x8D, 0x54, 0x24, 0x40, 0x8B, 0x0C, 0x03,
        0x89, 0x4C, 0x24, 0x40, 0x48, 0x8B, 0x4F, 0x58 };
      static const BYTE switchMiddle[] = { 0x48, 0x8B, 0x4F, 0x30, 0x45, 0x89, 0xF0, 0x8B, 0x14, 0x0B, 0x48, 0x89, 0xC1, 0x89,
        0x54, 0x24, 0x50, 0x48, 0x8D, 0x54, 0x24, 0x50 };
      static const BYTE bodyPart[] = { 0x0F, 0xB7, 0x12, 0x48, 0x8B, 0x81, 0x30, 0x01, 0x00, 0x00, 0x48, 0x8B, 0x0C, 0xD0, 0x44,
        0x89, 0xC2, 0x48, 0x8B, 0x01, 0xFF, 0xA0 };
      static const BYTE bodyWorld[] = { 0x8B, 0x02, 0xC1, 0xE8, 0x10, 0x0F, 0xB6, 0xD0, 0x48, 0x8B, 0x81, 0xF0, 0x00, 0x00, 0x00,
        0x48, 0x8B, 0x04, 0xD0, 0xC3 };
      static const BYTE recordBit[] = { 0x53, 0x48, 0x83, 0xEC, 0x20, 0x4C, 0x8D, 0x81, 0xB8, 0x00, 0x00, 0x00, 0x48, 0x89, 0xCB,
        0x41, 0x89, 0xD2, 0x41, 0x8B, 0x40, 0x08, 0xC1, 0xEA, 0x03, 0x41, 0x83, 0xE2, 0x07, 0x48, 0x83, 0xE8, 0x40, 0x41, 0x0F,
        0xB6, 0xCA, 0x49, 0x89, 0xC1, 0x49, 0xF7, 0xD9, 0x49, 0x09, 0xC1, 0x48, 0x83, 0xC8, 0x01, 0x48, 0xF7, 0xD8, 0x49, 0xC1,
        0xF9, 0x3F, 0x48, 0xC1, 0xF8, 0x3F, 0x49, 0x21, 0xC1, 0x49, 0x8B, 0x00, 0x4C, 0x29, 0xC0, 0x49, 0x21, 0xC1 };
      auto callsTo = [&](__int64 at, __int64 target) {
        return base[at] == 0xE8 && at + 5 + *(int const*)(base + at + 1) == target;
      };
      for (__int64 rva : { PartOffRva, PartOnRva })
      {
        if (memcmp(base + rva, switchHead, sizeof(switchHead)) || !callsTo(rva + 62, BodyWorldRva) ||
          memcmp(base + rva + 67, switchMiddle, sizeof(switchMiddle)))
          return false;
      }
      if (!callsTo(PartOffRva + 89, BodyPartOffRva) || !callsTo(PartOnRva + 89, BodyPartOnRva))
        return false;
      if (memcmp(base + BodyPartOffRva, bodyPart, sizeof(bodyPart)) || *(int const*)(base + BodyPartOffRva + 22) != RecordPartOffSlot * 8 ||
        memcmp(base + BodyPartOnRva, bodyPart, sizeof(bodyPart)) || *(int const*)(base + BodyPartOnRva + 22) != RecordPartOnSlot * 8 ||
        memcmp(base + BodyWorldRva, bodyWorld, sizeof(bodyWorld)) ||
        memcmp(base + RecordPartOffRva, recordBit, sizeof(recordBit)) || memcmp(base + RecordPartOnRva, recordBit, sizeof(recordBit)))
        return false;
      static const BYTE partObject[] = { 0x48, 0x8B, 0x41, 0x68, 0x89, 0xD2, 0x48, 0x8B, 0x04, 0xD0, 0xC3 };
      static const BYTE describe[] = { 0x0F, 0x28, 0x41, 0x10, 0x49, 0x89, 0xD0, 0x0F, 0x29, 0x02, 0x0F, 0x28, 0x49, 0x20, 0x0F,
        0x29, 0x4A, 0x10, 0x0F, 0x28, 0x41, 0x30, 0x0F, 0x29, 0x42, 0x20, 0x0F, 0x28, 0x49, 0x40, 0x0F, 0x29, 0x4A, 0x30 };
      static const BYTE visiblePart[] = { 0x0F, 0xAF, 0x50, 0x50, 0x03, 0x50, 0x48, 0x41, 0xFF, 0x90, 0x10, 0x02, 0x00, 0x00 };
      static const BYTE visibleHandle[] = { 0x44, 0x8B, 0xBB, 0x8C, 0x00, 0x00, 0x00 };
      static const BYTE visibleKeep[] = { 0x89, 0xB3, 0x8C, 0x00, 0x00, 0x00 };
      if (memcmp(base + PartObjectRva, partObject, sizeof(partObject)) || memcmp(base + DescribeRva, describe, sizeof(describe)) ||
        memcmp(base + VisibleRva + 0x45, visiblePart, sizeof(visiblePart)) ||
        memcmp(base + VisibleRva + 0xE1, visibleHandle, sizeof(visibleHandle)) || !callsTo(VisibleRva + 0x109, DescribeRva) ||
        memcmp(base + VisibleRva + 0x23C, visibleKeep, sizeof(visibleKeep)))
        return false;
      static const BYTE staticBits[] = { 0x8B, 0x41, 0x70, 0x4C, 0x8D, 0x49, 0x68, 0x41, 0x89, 0xD2 };
      static const BYTE staticTail[] = { 0x49, 0x8B, 0x4B, 0x30, 0x41, 0x8B, 0x53, 0x50, 0x48, 0x8B, 0x89, 0xE0, 0x00, 0x00, 0x00, 0xE9 };
      static const BYTE bodyChanged[] = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x6C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18,
        0x57, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x63, 0xDA, 0x49, 0x89, 0xC8 };
      auto jumpsTo = [&](__int64 at, __int64 target) {
        return base[at] == 0xE9 && at + 5 + *(int const*)(base + at + 1) == target;
      };
      if (!jumpsTo(StaticRecordPartOffRva, StaticPartOffRva) || !jumpsTo(StaticRecordPartOnRva, StaticPartOnRva) ||
        memcmp(base + StaticPartOffRva, staticBits, sizeof(staticBits)) || memcmp(base + StaticPartOnRva, staticBits, sizeof(staticBits)) ||
        memcmp(base + StaticPartOffRva + 0xA2, staticTail, sizeof(staticTail)) || memcmp(base + StaticPartOnRva + 0xA0, staticTail, sizeof(staticTail)) ||
        base + StaticPartOffRva + 0xA2 + 0x14 + *(int const*)(base + StaticPartOffRva + 0xA2 + 0x10) != base + BodyChangedRva ||
        base + StaticPartOnRva + 0xA0 + 0x14 + *(int const*)(base + StaticPartOnRva + 0xA0 + 0x10) != base + BodyChangedRva ||
        memcmp(base + BodyChangedRva, bodyChanged, sizeof(bodyChanged)))
        return false;
      __int64 const* componentVTable = (__int64 const*)(base + PhysicsComponentVTableRva);
      return componentVTable[PartOnSlot] == (__int64)base + PartOnRva && componentVTable[PartOffSlot] == (__int64)base + PartOffRva &&
        componentVTable[PartObjectSlot] == (__int64)base + PartObjectRva;
    }

    // Whether a mesh's name (its last part, any case) is this one. Game memory: only inside __try.
    bool NameIs(const char* pName, const char* wanted)
    {
      if ((__int64)pName < 0x10000)
        return false;
      const char* pLast = pName;
      for (size_t i = 0; pName[i] && i < 512; ++i)
        if (pName[i] == '/') pLast = pName + i + 1;
      size_t i = 0;
      for (; wanted[i]; ++i)
        if (tolower((unsigned char)pLast[i]) != wanted[i])
          return false;
      return pLast[i] == 0;
    }

    // A member's data, if the group is a model group with that many members. Game memory: only inside __try.
    __int64 MemberData(__int64 group, int k, __int64& data, int& count)
    {
      __int64 base = (__int64)g_gameHandle;
      if (*(__int64 const*)group != base + ModelGroupVTableRva)
        return 0;
      data = *(__int64 const*)(group + GroupDataOffset);
      count = *(int const*)(group + GroupMemberCountOffset);
      if (data < 0x10000 || count <= 0 || count > MaxGroupMembers || k < 0 || k >= count)
        return 0;
      __int64 members = *(__int64 const*)(data + DataMembersOffset);
      return members < 0x10000 ? 0 : members + (__int64)k * MemberDataSize;
    }

    // A piece's member as the data has it: its instance count, its parts, placed by its physics, the mesh (the numbers
    // first: a search reads every member of every group). Game memory: only inside __try.
    bool IsMember(Piece const& piece, __int64 member)
    {
      if (*(int const*)(member + MemberInstancesOffset) != piece.Instances ||
        *(unsigned int const*)(member + MemberFirstPartOffset) != piece.First ||
        *(unsigned int const*)(member + MemberPartsOffset) != piece.Parts)
        return false;
      __int64 transforms = *(__int64 const*)(member + MemberTransformsOffset);
      if (transforms >= 0x10000 && *(int const*)(transforms - 4) != 0)
        return false;
      __int64 mesh = *(__int64 const*)(member + MemberMeshOffset);
      return mesh >= 0x10000 && NameIs(*(const char* const*)(mesh + AssetNameOffset), piece.Mesh);
    }

    // The groups of a gate's pieces not found yet, in one walk of the model groups' instance lists; which were found
    void FindPieces(Gate const& gate, GateState& st, bool (&found)[MaxPieces])
    {
      __int64 base = (__int64)g_gameHandle;
      for (bool& f : found)
        f = false;
      __try
      {
        for (int list = 0; list < 4; ++list)
        {
          __int64 link = *(__int64 const*)(base + ModelGroupClassRva + ClassInstanceListsOffset + list * 0x10);
          for (int n = 0; link >= 0x10000 && n < MaxGroupsWalked; ++n)
          {
            __int64 group = link - EntityLinkOffset;
            link = *(__int64 const*)link;
            __int64 data = 0;
            int count = 0;
            if (!MemberData(group, 0, data, count))
              continue;
            for (int k = 0; k < count; ++k)
            {
              __int64 member = MemberData(group, k, data, count);
              if (!member)
                break;
              for (int p = 0; p < gate.Count; ++p)
              {
                PieceState& ps = st.Pieces[p];
                if (ps.Group || !IsMember(gate.Pieces[p], member))
                  continue;
                ps = {};
                ps.Group = group;
                ps.Data = data;
                ps.Members = count;
                ps.Member = k;
                found[p] = true;
              }
            }
          }
        }
      }
      __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    // Still that group: the same data and members, the piece's member where it was
    bool StillPiece(Piece const& piece, PieceState const& ps)
    {
      __try
      {
        __int64 data = 0;
        int count = 0;
        __int64 member = MemberData(ps.Group, ps.Member, data, count);
        return member && data == ps.Data && count == ps.Members && IsMember(piece, member);
      }
      __except (EXCEPTION_EXECUTE_HANDLER)
      {
        return false;
      }
    }

    unsigned int FirstPart(Piece const& piece)
    {
      return piece.First + (unsigned int)piece.Instance * piece.Parts;
    }

    // The member's physics component, if its record is made and is the member's; 0 if not (the Visible handler makes
    // a record not made yet: it's not called then)
    __int64 Component(PieceState const& ps)
    {
      __int64 base = (__int64)g_gameHandle;
      __try
      {
        __int64 records = *(__int64 const*)(ps.Group + GroupRecordsOffset);
        if (records < 0x10000)
          return 0;
        __int64 record = records + (__int64)ps.Member * RecordSize;
        __int64 member = *(__int64 const*)(ps.Data + DataMembersOffset) + (__int64)ps.Member * MemberDataSize;
        if (!*(BYTE const*)(record + RecordMadeOffset) || *(__int64 const*)(record + RecordDataOffset) != member)
          return 0;
        __int64 component = *(__int64 const*)(record + RecordPhysicsOffset);
        if (component < 0x10000 || *(__int64 const*)component != base + PhysicsComponentVTableRva)
          return 0;
        return component;
      }
      __except (EXCEPTION_EXECUTE_HANDLER)
      {
        return 0;
      }
    }

    // The piece's parts on the component's bodies: how many bodies, how many with all its parts off, all on. False if
    // a body's record isn't one the tools know.
    bool ReadParts(Piece const& piece, __int64 component, int& bodies, int& allOff, int& allOn)
    {
      unsigned int first = FirstPart(piece);
      __int64 base = (__int64)g_gameHandle;
      bodies = allOff = allOn = 0;
      __try
      {
        int count = *(unsigned short const*)(component + ComponentBodyCountOffset);
        __int64 ids = *(__int64 const*)(component + ComponentBodiesOffset);
        __int64 manager = *(__int64 const*)(component + ComponentManagerOffset);
        if (count <= 0 || count > MaxBodies || ids < 0x10000 || manager < 0x10000)
          return false;
        for (int b = 0; b < count; ++b)
        {
          unsigned int id = *(unsigned int const*)(ids + b * 4);
          __int64 worlds = *(__int64 const*)(manager + ManagerWorldsOffset);
          __int64 world = worlds >= 0x10000 ? *(__int64 const*)(worlds + ((id >> 16) & 0xFF) * 8) : 0;
          __int64 records = world >= 0x10000 ? *(__int64 const*)(world + WorldRecordsOffset) : 0;
          __int64 record = records >= 0x10000 ? *(__int64 const*)(records + (id & 0xFFFF) * 8) : 0;
          if (record < 0x10000)
            return false;
          __int64 const* vtable = *(__int64 const* const*)record;
          int bitsAt, countAt;
          if (vtable[RecordPartOnSlot] == base + StaticRecordPartOnRva && vtable[RecordPartOffSlot] == base + StaticRecordPartOffRva)
            bitsAt = StaticRecordPartBitsOffset, countAt = StaticRecordPartCountOffset;
          else if (vtable[RecordPartOnSlot] == base + RecordPartOnRva && vtable[RecordPartOffSlot] == base + RecordPartOffRva)
            bitsAt = RecordPartBitsOffset, countAt = RecordPartCountOffset;
          else
            return false;
          unsigned int parts = *(unsigned int const*)(record + countAt);
          if (parts < first + piece.Parts)
            return false;
          BYTE const* bits = parts > 64 ? *(BYTE const* const*)(record + bitsAt) : (BYTE const*)(record + bitsAt);
          if ((__int64)bits < 0x10000)
            return false;
          int on = 0;
          for (unsigned int p = first; p < first + piece.Parts; ++p)
            on += (bits[p >> 3] >> (p & 7)) & 1;
          ++bodies;
          allOff += on == 0;
          allOn += on == (int)piece.Parts;
        }
        return true;
      }
      __except (EXCEPTION_EXECUTE_HANDLER)
      {
        return false;
      }
    }

    // Its parts off or on, on all its bodies, as the component's own slots do it
    bool SwitchParts(Piece const& piece, __int64 component, bool on)
    {
      __int64 base = (__int64)g_gameHandle;
      __try
      {
        tPartSwitch partSwitch = (tPartSwitch)(base + (on ? PartOnRva : PartOffRva));
        for (unsigned int p = FirstPart(piece); p < FirstPart(piece) + piece.Parts; ++p)
          partSwitch(component, p);
        return true;
      }
      __except (EXCEPTION_EXECUTE_HANDLER)
      {
        return false;
      }
    }

    // Its draw object (the component's for its instance's first part), its rows, whether it's drawn
    __int64 PieceObject(Piece const& piece, __int64 component)
    {
      __try
      {
        __int64 objects = *(__int64 const*)(component + ComponentPartObjectsOffset);
        __int64 object = objects >= 0x10000 ? *(__int64 const*)(objects + (__int64)FirstPart(piece) * 8) : 0;
        return object >= 0x10000 ? object : 0;
      }
      __except (EXCEPTION_EXECUTE_HANDLER)
      {
        return 0;
      }
    }

    bool ReadObject(__int64 object, float (&rows)[16], bool& shown)
    {
      __try
      {
        memcpy(rows, (void const*)(object + ObjectRowsOffset), sizeof(rows));
        shown = *(int const*)(object + ObjectHandleOffset) != -1;
        return true;
      }
      __except (EXCEPTION_EXECUTE_HANDLER)
      {
        return false;
      }
    }

    bool RowsAreTheData(Piece const& piece, float const (&rows)[16])
    {
      for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 3; ++c)
          if (fabsf(rows[r * 4 + c] - piece.Rows[r][c]) > (r < 3 ? RowsTurnEpsilon : RowsPlaceEpsilon))
            return false;
      return true;
    }

    // Faith clear of the piece: her feet ClearAcross or more from its box across, or her ClearOver over its top, or her
    // head under its bottom
    bool Clear(Piece const& piece, float const* feet)
    {
      float const* b = piece.Box;
      float dx = (std::max)((std::max)(b[0] - feet[0], feet[0] - b[3]), 0.f);
      float dz = (std::max)((std::max)(b[2] - feet[2], feet[2] - b[5]), 0.f);
      return dx * dx + dz * dz >= ClearAcross * ClearAcross || feet[1] >= b[4] + ClearOver || feet[1] + FaithHeight <= b[1];
    }

    // What the tools did undone: its collision on again if they took it off, drawn again if they hid it (what the game
    // did itself is left). True once nothing the tools did is left.
    bool PutBack(Piece const& piece, PieceState& ps, __int64 component, __int64 object, int bodies, int allOn, bool shown)
    {
      if (ps.PartsOff)
      {
        int allOff = 0;
        if (allOn == bodies || (SwitchParts(piece, component, true) && ReadParts(piece, component, bodies, allOff, allOn) &&
            allOn == bodies))
          ps.PartsOff = false;
      }
      if (ps.Hidden)
      {
        float rows[16];
        if (shown || (object && util::hooks::SetModelGroupMemberVisible(ps.Group, ps.Member, piece.Instance, true) &&
            ReadObject(object, rows, shown) && shown))
          ps.Hidden = false;
      }
      return !ps.Touched();
    }

    // One gate: its pieces taken away while wanted (Solid street on), back once not and Faith is clear of them
    void UpdateGate(Gate const& gate, GateState& st, bool wanted, float const* feet, ULONGLONG now)
    {
      float dx = feet[0] - gate.Middle[0], dz = feet[2] - gate.Middle[2];
      float distance = sqrtf(dx * dx + dz * dz);
      bool touched = false, unfound = false;
      for (int p = 0; p < gate.Count; ++p)
      {
        touched |= st.Pieces[p].Group && st.Pieces[p].Touched();
        unfound |= !st.Pieces[p].Group;
      }
      // Nothing taken and not wanted, or wanted with her far from it (its zones may not be there): nothing to do
      if (!touched && (!wanted || distance > SearchDistance))
        return;
      for (int p = 0; p < gate.Count; ++p)
      {
        PieceState& ps = st.Pieces[p];
        if (ps.Group && !StillPiece(gate.Pieces[p], ps))
        {
          util::log::Write("Street level: %s, piece %d (%s instance %d): its zone's model group is gone (streamed out)%s",
            gate.Name, p, gate.Pieces[p].Mesh, gate.Pieces[p].Instance, ps.Touched() ? "; it comes back as its file has it" :
            "");
          // (taken away again once it's found again: logged again)
          if (ps.Touched())
          {
            st.AwayLogged = 0;
            st.WaitLogged = false;
          }
          ps = {};
          unfound = true;
        }
      }
      if (wanted && unfound && distance <= SearchDistance && now - st.LastSearch >= SearchMs)
      {
        st.LastSearch = now;
        bool found[MaxPieces];
        FindPieces(gate, st, found);
        unfound = false;
        for (int p = 0; p < gate.Count; ++p)
        {
          unfound |= !st.Pieces[p].Group;
          if (found[p])
            util::log::Write("Street level: %s, piece %d (%s instance %d, collision parts %u - %u): found (group 0x%llX, "
              "member %d)", gate.Name, p, gate.Pieces[p].Mesh, gate.Pieces[p].Instance, FirstPart(gate.Pieces[p]),
              FirstPart(gate.Pieces[p]) + gate.Pieces[p].Parts - 1, st.Pieces[p].Group, st.Pieces[p].Member);
        }
        if (!unfound)
          st.NotFoundLogged = false;
        else if (!st.NotFoundLogged && distance < NotFoundLogDistance)
        {
          st.NotFoundLogged = true;
          util::log::Write("Street level: %s: Faith is %.0f m from it and not all its pieces' model groups are found (not "
            "loaded, or not as the data has them)", gate.Name, distance);
        }
      }

      // away: taken away; left: found and left as the game has it (rows not the data's, given up, a body unknown);
      // back: nothing the tools did left on it; waiting: to come back once Faith is clear of it
      int away = 0, left = 0, back = 0, waiting = 0;
      for (int p = 0; p < gate.Count; ++p)
      {
        Piece const& piece = gate.Pieces[p];
        PieceState& ps = st.Pieces[p];
        // (one not found, or whose zone went, is as its file has it)
        if (!ps.Group || ps.Bad || ps.GaveUp)
        {
          back += !ps.Touched();
          left += ps.Group != 0;
          continue;
        }
        __int64 component = Component(ps);
        int bodies = 0, allOff = 0, allOn = 0;
        if (!component || !ReadParts(piece, component, bodies, allOff, allOn))
        {
          if (component && !ps.PartsUnknownLogged)
          {
            ps.PartsUnknownLogged = true;
            util::log::Warning("Street level: %s, piece %d: a body of its collision isn't as the tools know it, it stays as "
              "the game has it", gate.Name, p);
          }
          back += !ps.Touched();
          left += component != 0;
          continue;
        }
        __int64 object = PieceObject(piece, component);
        float rows[16];
        bool shown = false;
        if (!object || !ReadObject(object, rows, shown))
        {
          back += !ps.Touched();
          continue;
        }
        if (object != ps.Object)
        {
          // Its draw object (new, or a new one for its group): where the data puts it, or the piece is left alone
          if (!RowsAreTheData(piece, rows))
          {
            ps.Bad = true;
            bool put = PutBack(piece, ps, component, object, bodies, allOn, shown);
            util::log::Warning("Street level: %s, piece %d: its draw object 0x%llX isn't where the data puts it (rows %.3f "
              "%.3f %.3f / %.3f %.3f %.3f / %.2f %.2f %.2f), it stays as the game has it%s", gate.Name, p, object, rows[0],
              rows[1], rows[2], rows[8], rows[9], rows[10], rows[12], rows[13], rows[14], put ? "" : " (putting back what "
              "the tools did failed)");
            back += put;
            left += 1;
            continue;
          }
          ps.Object = object;
        }
        if (wanted)
        {
          // Its collision off, then it hidden. A try that doesn't take is undone in the same frame (never drawn with no
          // collision, nor hidden and solid), tried again after RetryMs, at most MostFailures times. Drawn again by the
          // game once taken away: hidden again at once.
          if ((allOff < bodies || shown) && (ps.AwayFailures == 0 || now - ps.LastTry >= RetryMs))
          {
            ps.LastTry = now;
            bool ok = true;
            if (allOff < bodies)
            {
              ps.PartsOff = true;
              ok = SwitchParts(piece, component, false) && ReadParts(piece, component, bodies, allOff, allOn) &&
                allOff == bodies;
            }
            if (ok && shown)
            {
              ps.Hidden = true;
              ok = util::hooks::SetModelGroupMemberVisible(ps.Group, ps.Member, piece.Instance, false) &&
                ReadObject(object, rows, shown) && !shown;
            }
            if (ok)
              ps.AwayFailures = 0;
            else
            {
              ReadParts(piece, component, bodies, allOff, allOn);
              ReadObject(object, rows, shown);
              bool put = PutBack(piece, ps, component, object, bodies, allOn, shown);
              if (++ps.AwayFailures >= MostFailures || !put)
              {
                ps.GaveUp = true;
                util::log::Warning("Street level: %s, piece %d: it couldn't be taken away %d times (collision off on %d of %d "
                  "bodies, %s): it stays as the game has it%s", gate.Name, p, ps.AwayFailures, allOff, bodies, shown ?
                  "drawn" : "not drawn", put ? "" : " (putting back what the tools did failed)");
                back += put;
                left += 1;
                continue;
              }
            }
          }
          away += allOff == bodies && !shown;
        }
        else if (ps.Touched())
        {
          if (!Clear(piece, feet))
          {
            ++waiting;
            continue;
          }
          if (ps.BackFailures && now - ps.LastTry < RetryMs)
            continue;
          ps.LastTry = now;
          if (PutBack(piece, ps, component, object, bodies, allOn, shown))
          {
            ps.BackFailures = 0;
            ++back;
          }
          else if (++ps.BackFailures >= MostFailures)
          {
            ps.GaveUp = true;
            util::log::Warning("Street level: %s, piece %d: what the tools did didn't come undone %d times (%s%s)", gate.Name,
              p, MostFailures, ps.PartsOff ? "its collision is off" : "", ps.Hidden ? (ps.PartsOff ? ", not drawn" :
              "not drawn") : "");
          }
        }
        else
          ++back;
      }
      if (wanted)
      {
        st.WaitLogged = false;
        // (every piece taken away or left as the game has it, some taken away)
        if (away && away + left == gate.Count && st.AwayLogged != 1)
        {
          st.AwayLogged = 1;
          if (away == gate.Count)
            util::log::Write("Street level: %s taken away while Solid street is on (%d pieces: collision off, not drawn)",
              gate.Name, gate.Count);
          else
            util::log::Write("Street level: %s taken away while Solid street is on, %d of its %d pieces (the others as the "
              "game has them, see above)", gate.Name, away, gate.Count);
        }
      }
      else
      {
        if (waiting && !st.WaitLogged)
        {
          st.WaitLogged = true;
          util::log::Write("Street level: %s comes back once Faith is clear of it (she's at %.1f %.1f %.1f)", gate.Name,
            feet[0], feet[1], feet[2]);
        }
        else if (!waiting)
          st.WaitLogged = false;
        if (back == gate.Count && st.AwayLogged == 1)
        {
          st.AwayLogged = -1;
          util::log::Write("Street level: %s back as the game has it (Solid street off)", gate.Name);
        }
      }
    }
  }
}

// ---- The Sky City station's collision reshaped (2026-11-10; Research_Ground_Level_2026-11-01.md section 24)
//
// The user, after the station's floors were made solid (section 23): "reshape it so the base block doesn't have any
// death zones etc", then of the walkway outside its north end: "make sure that the outside balcony area is solid as
// well" and "there is also an invisible wall preventing from going there". Two convex shapes of zone 55's static group
// collision (the zone body's root shape, as its file stores it: two Havok packfiles, these objects the same bytes in
// both; offline, reshape/hullfind.py):
// - Sky City's base block (skycity_building_01_mesh's part 7225, hkworld leaf 72654): a box under the whole of Sky
//   City (its frame's u -195.8 .. 159.6, h 72.9 .. 107.3) that fills the station's lowest floor, its pools and its hall
//   6.7 m deep (Faith walked on its top over the hall, CT.log 2026-11-09 23:03 - 23:08). Its west face is moved east to
//   u -73.54, 5 cm past what's drawn at the hall's east end: its 4 west vertices' x and that plane's d (block/
//   b16_final.py). A shrink needs nothing else: the compounds' trees only have to hold it. What that takes away outside
//   the hall, Street level's props put back exactly (the fill: reshape/skycity2gen.py).
// - The station exterior's north wedge (its part 6753's hull 3 of 3, leaf 71840): its south face is the invisible wall
//   across the walkway's doorway (v 62.2 of the station's frame; Faith stopped at it, 23:05), its inside fills the
//   walkway's end and the upper floor's north band. Shrunk to a 4 cm copy of itself about a point 5.4 m inside
//   skycity_building_01's leaf 72657, which stays: its vertices pulled in, its planes' d with them, its normals, faces
//   and indices as they are (reshape/hullcollapse.py). What it held (the north roof, the walls round the walkway),
//   Street level's props stand in for (the exterior's and interior's drawn triangles there, as boxes).
// - The low ceiling over the hall's east end (2026-11-10, the user: "Yes, fix it"; section 25): skycity_building_01's
//   leaves 72657, 72678, 72679 and 72656 come down to 1.6 - 2.5 m over the hall's floor. Each is cut at the base
//   block's top (h 107.28835, the leaves' local y 102.57965088): what they lose outside the hall lies inside the
//   block's old box (the block's kept part, or Street level's fill for it), or is a sliver under the 1 cm convex
//   radius. 72678 and 72679 keep their faces (vertices moved); 72657 and 72656 get new arrays and smaller counts
//   (72657 faces 20 -> 18; 72656 planes 24 -> 20, faces 21 -> 19), the offsets as they are. Made twice offline,
//   independently, byte for byte the same (ceiling/C and ceiling/verify/G), checked against the file's 2,213
//   polytopes' rules and the game's readers of them (verify/V2, V3).
// Each is found as DogenDoor.cpp's fill blocks are: the member (mesh, instance count, first part, parts a instance:
// each set unique in sp_maincity's files) -> its group's physics component -> each body's record -> its Havok body's
// shape (the zone's root) -> the convex object at its distance from the root in the first packfile or the second,
// checked byte for byte (headers, vertices, planes, faces, indices); written in the physics flush of its bodies' world
// (no step runs then; the camera hook only asks), in the order that keeps a read in between near the old shape or the
// new (Write), then the body's caches renewed (0x143F3CD00) as the game does after its collision changes, and again at
// that world's next flush. Changed while Solid street is on and the props data has
// the stand-ins (DataProbes), once nothing is lost under Faith: she isn't in or on the space it opens, or Street
// level's boxes within NearMadeRadius of her have been made for ChangeAfterMs. Back as its file has it once Solid
// street is off and she isn't in that space (on or over a top of it is fine: it comes back under her feet); till then
// Street level stays on (g_SkyHolding). A zone streamed in again comes as its file has it.
namespace
{
  namespace skycity
  {
    // Each shape's object from + 0x10 to its end (+ 0x00 .. + 0x0F is its vtable and reference count in memory): as
    // the file has it and as the tools make it (reshape/images.py; the file's two packfiles' copies the same)
    // Block: + 0x10 .. + 0x190 as the file has it, then as the tools make it
    const BYTE BlockFile[384] = {
      0x43, 0x01, 0x00, 0x01, 0x0A, 0xD7, 0x23, 0x3C, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x08, 0x00, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x08, 0x00, 0x90, 0x00, 0x06, 0x00, 0x0C, 0x01, 0x18, 0x00, 0x28, 0x01, 0x00, 0x00, 0x00, 0x00,
      0x84, 0xBD, 0x0E, 0x44, 0xC8, 0x28, 0xCD, 0x42, 0x74, 0x2A, 0xCB, 0x42, 0x00, 0x00, 0x00, 0x3F,
      0x84, 0xBD, 0x0E, 0x44, 0x04, 0x6C, 0x88, 0x42, 0x74, 0x2A, 0xCB, 0x42, 0x01, 0x00, 0x00, 0x3F,
      0x84, 0xBD, 0x0E, 0x44, 0xC8, 0x28, 0xCD, 0x42, 0x9E, 0x77, 0x4A, 0xC3, 0x02, 0x00, 0x00, 0x3F,
      0x88, 0x85, 0x57, 0x43, 0xC8, 0x28, 0xCD, 0x42, 0x74, 0x2A, 0xCB, 0x42, 0x03, 0x00, 0x00, 0x3F,
      0x84, 0xBD, 0x0E, 0x44, 0x04, 0x6C, 0x88, 0x42, 0x9E, 0x77, 0x4A, 0xC3, 0x04, 0x00, 0x00, 0x3F,
      0x88, 0x85, 0x57, 0x43, 0xC8, 0x28, 0xCD, 0x42, 0x9E, 0x77, 0x4A, 0xC3, 0x05, 0x00, 0x00, 0x3F,
      0x88, 0x85, 0x57, 0x43, 0x04, 0x6C, 0x88, 0x42, 0x74, 0x2A, 0xCB, 0x42, 0x06, 0x00, 0x00, 0x3F,
      0x88, 0x85, 0x57, 0x43, 0x04, 0x6C, 0x88, 0x42, 0x9E, 0x77, 0x4A, 0xC3, 0x07, 0x00, 0x00, 0x3F,
      0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0x7F, 0x3F, 0x00, 0x00, 0x00, 0x00, 0xC7, 0x28, 0xCD, 0xC2,
      0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0x7F, 0xBF, 0x00, 0x00, 0x00, 0x80, 0x03, 0x6C, 0x88, 0x42,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x00, 0x00, 0x80, 0xBF, 0x9E, 0x77, 0x4A, 0xC3,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x3F, 0x74, 0x2A, 0xCB, 0xC2,
      0x00, 0x00, 0x80, 0xBF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x88, 0x85, 0x57, 0x43,
      0x00, 0x00, 0x80, 0x3F, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x84, 0xBD, 0x0E, 0xC4,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xEE, 0xFF, 0x7F, 0xFF,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xEE, 0xFF, 0x7F, 0xFF,
      0x80, 0x04, 0x00, 0x00, 0x80, 0x04, 0x04, 0x00, 0x80, 0x04, 0x08, 0x00, 0x80, 0x04, 0x0C, 0x00,
      0x80, 0x04, 0x10, 0x00, 0x80, 0x04, 0x14, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x02, 0x05, 0x03, 0x07, 0x04, 0x01, 0x06, 0x02, 0x04, 0x07, 0x05, 0x06, 0x01, 0x00, 0x03,
      0x03, 0x05, 0x07, 0x06, 0x04, 0x02, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    };
    const BYTE BlockTools[384] = {
      0x43, 0x01, 0x00, 0x01, 0x0A, 0xD7, 0x23, 0x3C, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x08, 0x00, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x08, 0x00, 0x90, 0x00, 0x06, 0x00, 0x0C, 0x01, 0x18, 0x00, 0x28, 0x01, 0x00, 0x00, 0x00, 0x00,
      0x84, 0xBD, 0x0E, 0x44, 0xC8, 0x28, 0xCD, 0x42, 0x74, 0x2A, 0xCB, 0x42, 0x00, 0x00, 0x00, 0x3F,
      0x84, 0xBD, 0x0E, 0x44, 0x04, 0x6C, 0x88, 0x42, 0x74, 0x2A, 0xCB, 0x42, 0x01, 0x00, 0x00, 0x3F,
      0x84, 0xBD, 0x0E, 0x44, 0xC8, 0x28, 0xCD, 0x42, 0x9E, 0x77, 0x4A, 0xC3, 0x02, 0x00, 0x00, 0x3F,
      0xB8, 0xE6, 0xA8, 0x43, 0xC8, 0x28, 0xCD, 0x42, 0x74, 0x2A, 0xCB, 0x42, 0x03, 0x00, 0x00, 0x3F,
      0x84, 0xBD, 0x0E, 0x44, 0x04, 0x6C, 0x88, 0x42, 0x9E, 0x77, 0x4A, 0xC3, 0x04, 0x00, 0x00, 0x3F,
      0xB8, 0xE6, 0xA8, 0x43, 0xC8, 0x28, 0xCD, 0x42, 0x9E, 0x77, 0x4A, 0xC3, 0x05, 0x00, 0x00, 0x3F,
      0xB8, 0xE6, 0xA8, 0x43, 0x04, 0x6C, 0x88, 0x42, 0x74, 0x2A, 0xCB, 0x42, 0x06, 0x00, 0x00, 0x3F,
      0xB8, 0xE6, 0xA8, 0x43, 0x04, 0x6C, 0x88, 0x42, 0x9E, 0x77, 0x4A, 0xC3, 0x07, 0x00, 0x00, 0x3F,
      0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0x7F, 0x3F, 0x00, 0x00, 0x00, 0x00, 0xC7, 0x28, 0xCD, 0xC2,
      0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0x7F, 0xBF, 0x00, 0x00, 0x00, 0x80, 0x03, 0x6C, 0x88, 0x42,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x00, 0x00, 0x80, 0xBF, 0x9E, 0x77, 0x4A, 0xC3,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x3F, 0x74, 0x2A, 0xCB, 0xC2,
      0x00, 0x00, 0x80, 0xBF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0xB8, 0xE6, 0xA8, 0x43,
      0x00, 0x00, 0x80, 0x3F, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x84, 0xBD, 0x0E, 0xC4,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xEE, 0xFF, 0x7F, 0xFF,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xEE, 0xFF, 0x7F, 0xFF,
      0x80, 0x04, 0x00, 0x00, 0x80, 0x04, 0x04, 0x00, 0x80, 0x04, 0x08, 0x00, 0x80, 0x04, 0x0C, 0x00,
      0x80, 0x04, 0x10, 0x00, 0x80, 0x04, 0x14, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x02, 0x05, 0x03, 0x07, 0x04, 0x01, 0x06, 0x02, 0x04, 0x07, 0x05, 0x06, 0x01, 0x00, 0x03,
      0x03, 0x05, 0x07, 0x06, 0x04, 0x02, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    };
    // Wedge: + 0x10 .. + 0x1F0 as the file has it, then as the tools make it
    const BYTE WedgeFile[480] = {
      0x43, 0x01, 0x00, 0x01, 0x0A, 0xD7, 0x23, 0x3C, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x08, 0x00, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x0C, 0x00, 0x90, 0x00, 0x0B, 0x00, 0x4C, 0x01, 0x22, 0x00, 0x78, 0x01, 0x00, 0x00, 0x00, 0x00,
      0xBC, 0x7D, 0x71, 0x43, 0xAC, 0x83, 0x87, 0x42, 0x49, 0xCD, 0x95, 0x42, 0x00, 0x00, 0x00, 0x3F,
      0x18, 0x4A, 0x75, 0x43, 0x74, 0xEC, 0xB3, 0x41, 0xDC, 0xD8, 0x77, 0x42, 0x01, 0x00, 0x00, 0x3F,
      0x18, 0x4A, 0x75, 0x43, 0x74, 0xEC, 0xB3, 0x41, 0xBE, 0x21, 0xB3, 0x42, 0x02, 0x00, 0x00, 0x3F,
      0x18, 0x4A, 0x75, 0x43, 0x31, 0xAE, 0x87, 0x42, 0xDC, 0xD8, 0x77, 0x42, 0x03, 0x00, 0x00, 0x3F,
      0xCE, 0xCC, 0xE7, 0x42, 0x74, 0xEC, 0xB3, 0x41, 0x0E, 0xE8, 0x84, 0x42, 0x04, 0x00, 0x00, 0x3F,
      0x6C, 0xC8, 0xE7, 0x42, 0x74, 0xEC, 0xB3, 0x41, 0xDC, 0xD8, 0x77, 0x42, 0x05, 0x00, 0x00, 0x3F,
      0xB1, 0xB6, 0xD3, 0x42, 0x96, 0x18, 0x24, 0x42, 0x0E, 0xE8, 0x84, 0x42, 0x06, 0x00, 0x00, 0x3F,
      0x27, 0x4B, 0xD2, 0x42, 0xC0, 0x15, 0x24, 0x42, 0xEB, 0x48, 0x77, 0x42, 0x07, 0x00, 0x00, 0x3F,
      0x2A, 0xCD, 0xF0, 0xBD, 0xC8, 0x1F, 0x98, 0x3E, 0x35, 0x94, 0x72, 0x3F, 0xDE, 0xD8, 0x7A, 0xC2,
      0x35, 0x78, 0x83, 0x3A, 0x00, 0x00, 0x00, 0x00, 0xF8, 0xFF, 0x7F, 0xBF, 0xE4, 0xDC, 0x76, 0x42,
      0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0x7F, 0xBF, 0x00, 0x00, 0x00, 0x00, 0x73, 0xEC, 0xB3, 0x41,
      0xEE, 0x40, 0x33, 0xBE, 0xBA, 0x45, 0xC2, 0xBD, 0xE3, 0xDF, 0x7A, 0x3F, 0x92, 0xCD, 0x2A, 0xC2,
      0x00, 0x00, 0x00, 0x80, 0x16, 0x8F, 0xF8, 0xBB, 0x1C, 0xFE, 0x7F, 0xBF, 0xB9, 0x85, 0x78, 0x42,
      0x13, 0x31, 0x40, 0xBE, 0xDA, 0x23, 0x7B, 0x3F, 0x7D, 0xE1, 0x47, 0xBD, 0x7E, 0xED, 0x8B, 0xC1,
      0xFF, 0xFF, 0x7F, 0x3F, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x17, 0x4A, 0x75, 0xC3,
      0x74, 0xBB, 0x47, 0xBE, 0x89, 0xF8, 0x7A, 0x3F, 0x56, 0x20, 0xF0, 0x3C, 0xE2, 0x25, 0xAC, 0xC1,
      0xA4, 0x00, 0x72, 0x3F, 0xCF, 0xA4, 0x2D, 0x3E, 0xE9, 0xA2, 0x8E, 0x3E, 0x55, 0x52, 0x82, 0xC3,
      0x69, 0x0D, 0x5F, 0xBF, 0xA1, 0xBD, 0xF1, 0xBE, 0x08, 0x0E, 0x09, 0x3E, 0x8A, 0x6A, 0xCD, 0x42,
      0x31, 0x77, 0x5D, 0xBF, 0xA3, 0x68, 0x00, 0xBF, 0xB5, 0x17, 0xD8, 0x3A, 0x94, 0xDF, 0xDE, 0x42,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xEE, 0xFF, 0x7F, 0xFF,
      0x20, 0x03, 0x00, 0x00, 0x01, 0x03, 0x03, 0x00, 0x55, 0x04, 0x06, 0x00, 0x20, 0x03, 0x0A, 0x00,
      0x01, 0x03, 0x0D, 0x00, 0x06, 0x03, 0x10, 0x00, 0x1B, 0x03, 0x13, 0x00, 0x06, 0x03, 0x16, 0x00,
      0x1B, 0x03, 0x19, 0x00, 0x0B, 0x03, 0x1C, 0x00, 0x0B, 0x03, 0x1F, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x06, 0x02, 0x01, 0x07, 0x03, 0x05, 0x01, 0x02, 0x04, 0x02, 0x06, 0x04, 0x01, 0x05, 0x07,
      0x00, 0x03, 0x07, 0x01, 0x03, 0x02, 0x00, 0x07, 0x06, 0x00, 0x02, 0x03, 0x07, 0x04, 0x06, 0x07,
      0x05, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    };
    const BYTE WedgeTools[480] = {
      0x43, 0x01, 0x00, 0x01, 0x0A, 0xD7, 0x23, 0x3C, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x08, 0x00, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x0C, 0x00, 0x90, 0x00, 0x0B, 0x00, 0x4C, 0x01, 0x22, 0x00, 0x78, 0x01, 0x00, 0x00, 0x00, 0x00,
      0x7D, 0xEA, 0x6F, 0x43, 0x9F, 0xEF, 0xE1, 0x41, 0xBC, 0xD1, 0x86, 0x42, 0x00, 0x00, 0x00, 0x3F,
      0xC8, 0xEA, 0x6F, 0x43, 0xCF, 0xD3, 0xE1, 0x41, 0xC0, 0xCF, 0x86, 0x42, 0x01, 0x00, 0x00, 0x3F,
      0xC8, 0xEA, 0x6F, 0x43, 0xCF, 0xD3, 0xE1, 0x41, 0xFD, 0xD3, 0x86, 0x42, 0x02, 0x00, 0x00, 0x3F,
      0xC8, 0xEA, 0x6F, 0x43, 0xAC, 0xEF, 0xE1, 0x41, 0xC0, 0xCF, 0x86, 0x42, 0x03, 0x00, 0x00, 0x3F,
      0xD8, 0xE0, 0x6F, 0x43, 0xCF, 0xD3, 0xE1, 0x41, 0x70, 0xD0, 0x86, 0x42, 0x04, 0x00, 0x00, 0x3F,
      0xD8, 0xE0, 0x6F, 0x43, 0xCF, 0xD3, 0xE1, 0x41, 0xC0, 0xCF, 0x86, 0x42, 0x05, 0x00, 0x00, 0x3F,
      0x13, 0xE0, 0x6F, 0x43, 0x32, 0xDF, 0xE1, 0x41, 0x70, 0xD0, 0x86, 0x42, 0x06, 0x00, 0x00, 0x3F,
      0x05, 0xE0, 0x6F, 0x43, 0x32, 0xDF, 0xE1, 0x41, 0xBA, 0xCF, 0x86, 0x42, 0x07, 0x00, 0x00, 0x3F,
      0x2A, 0xCD, 0xF0, 0xBD, 0xC8, 0x1F, 0x98, 0x3E, 0x35, 0x94, 0x72, 0x3F, 0x29, 0x3B, 0x30, 0xC2,
      0x35, 0x78, 0x83, 0x3A, 0x00, 0x00, 0x00, 0x00, 0xF8, 0xFF, 0x7F, 0xBF, 0x86, 0x54, 0x86, 0x42,
      0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0x7F, 0xBF, 0x00, 0x00, 0x00, 0x00, 0xCE, 0xD3, 0xE1, 0x41,
      0xEE, 0x40, 0x33, 0xBE, 0xBA, 0x45, 0xC2, 0xBD, 0xE3, 0xDF, 0x7A, 0x3F, 0xB5, 0x1B, 0xAB, 0xC1,
      0x00, 0x00, 0x00, 0x80, 0x16, 0x8F, 0xF8, 0xBB, 0x1C, 0xFE, 0x7F, 0xBF, 0x62, 0x3C, 0x87, 0x42,
      0x13, 0x31, 0x40, 0xBE, 0xDA, 0x23, 0x7B, 0x3F, 0x7D, 0xE1, 0x47, 0xBD, 0x15, 0xE7, 0xA4, 0x41,
      0xFF, 0xFF, 0x7F, 0x3F, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xC7, 0xEA, 0x6F, 0xC3,
      0x74, 0xBB, 0x47, 0xBE, 0x89, 0xF8, 0x7A, 0x3F, 0x56, 0x20, 0xF0, 0x3C, 0xD9, 0x0F, 0x89, 0x41,
      0xA4, 0x00, 0x72, 0x3F, 0xCF, 0xA4, 0x2D, 0x3E, 0xE9, 0xA2, 0x8E, 0x3E, 0xD0, 0x5D, 0x7A, 0xC3,
      0x69, 0x0D, 0x5F, 0xBF, 0xA1, 0xBD, 0xF1, 0xBE, 0x08, 0x0E, 0x09, 0x3E, 0xCA, 0x4F, 0x55, 0x43,
      0x31, 0x77, 0x5D, 0xBF, 0xA3, 0x68, 0x00, 0xBF, 0xB5, 0x17, 0xD8, 0x3A, 0x1E, 0x91, 0x5D, 0x43,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xEE, 0xFF, 0x7F, 0xFF,
      0x20, 0x03, 0x00, 0x00, 0x01, 0x03, 0x03, 0x00, 0x55, 0x04, 0x06, 0x00, 0x20, 0x03, 0x0A, 0x00,
      0x01, 0x03, 0x0D, 0x00, 0x06, 0x03, 0x10, 0x00, 0x1B, 0x03, 0x13, 0x00, 0x06, 0x03, 0x16, 0x00,
      0x1B, 0x03, 0x19, 0x00, 0x0B, 0x03, 0x1C, 0x00, 0x0B, 0x03, 0x1F, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x06, 0x02, 0x01, 0x07, 0x03, 0x05, 0x01, 0x02, 0x04, 0x02, 0x06, 0x04, 0x01, 0x05, 0x07,
      0x00, 0x03, 0x07, 0x01, 0x03, 0x02, 0x00, 0x07, 0x06, 0x00, 0x02, 0x03, 0x07, 0x04, 0x06, 0x07,
      0x05, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    };
    // Ceiling72678: + 0x10 .. + 0x190 as the file has it, then as the tools make it
    const BYTE Ceiling72678File[384] = {
      0x43, 0x01, 0x00, 0x01, 0x0A, 0xD7, 0x23, 0x3C, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x08, 0x00, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x08, 0x00, 0x90, 0x00, 0x06, 0x00, 0x0C, 0x01, 0x18, 0x00, 0x28, 0x01, 0x00, 0x00, 0x00, 0x00,
      0x57, 0x03, 0xA7, 0x43, 0xD6, 0xA3, 0x06, 0x43, 0xDF, 0x5C, 0x47, 0x42, 0x00, 0x00, 0x00, 0x3F,
      0x57, 0x03, 0xA7, 0x43, 0x4C, 0xBA, 0xC4, 0x42, 0xDF, 0x5C, 0x47, 0x42, 0x01, 0x00, 0x00, 0x3F,
      0x57, 0x03, 0xA7, 0x43, 0xD6, 0xA3, 0x06, 0x43, 0x7A, 0x55, 0x8D, 0xC2, 0x02, 0x00, 0x00, 0x3F,
      0xFF, 0xE7, 0xA3, 0x43, 0xD6, 0xA3, 0x06, 0x43, 0xDF, 0x5C, 0x47, 0x42, 0x03, 0x00, 0x00, 0x3F,
      0x57, 0x03, 0xA7, 0x43, 0x4C, 0xBA, 0xC4, 0x42, 0xCC, 0x7E, 0x8C, 0xC2, 0x04, 0x00, 0x00, 0x3F,
      0xFF, 0xE7, 0xA3, 0x43, 0xD6, 0xA3, 0x06, 0x43, 0x7A, 0x55, 0x8D, 0xC2, 0x05, 0x00, 0x00, 0x3F,
      0xFF, 0xE7, 0xA3, 0x43, 0x4C, 0xBA, 0xC4, 0x42, 0xDF, 0x5C, 0x47, 0x42, 0x06, 0x00, 0x00, 0x3F,
      0xFF, 0xE7, 0xA3, 0x43, 0x4C, 0xBA, 0xC4, 0x42, 0xCC, 0x7E, 0x8C, 0xC2, 0x07, 0x00, 0x00, 0x3F,
      0x00, 0x00, 0x80, 0x3F, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x57, 0x03, 0xA7, 0xC3,
      0x00, 0x00, 0x80, 0xBF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0xFF, 0xE7, 0xA3, 0x43,
      0x00, 0x00, 0x00, 0x00, 0xFE, 0xFF, 0x7F, 0x3F, 0x00, 0x00, 0x00, 0x00, 0xD5, 0xA3, 0x06, 0xC3,
      0x00, 0x00, 0x00, 0x80, 0x00, 0x00, 0x80, 0xBF, 0x00, 0x00, 0x00, 0x00, 0x4C, 0xBA, 0xC4, 0x42,
      0x00, 0x00, 0x00, 0x00, 0x78, 0x5C, 0x3D, 0xBC, 0x9F, 0xFB, 0x7F, 0xBF, 0x52, 0x36, 0x8A, 0xC2,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x3F, 0xDF, 0x5C, 0x47, 0xC2,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xEE, 0xFF, 0x7F, 0xFF,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xEE, 0xFF, 0x7F, 0xFF,
      0x80, 0x04, 0x00, 0x00, 0x80, 0x04, 0x04, 0x00, 0x80, 0x04, 0x08, 0x00, 0x7F, 0x04, 0x0C, 0x00,
      0x7F, 0x04, 0x10, 0x00, 0x80, 0x04, 0x14, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x04, 0x02, 0x00, 0x01, 0x03, 0x05, 0x07, 0x06, 0x05, 0x03, 0x00, 0x02, 0x01, 0x06, 0x07, 0x04,
      0x02, 0x04, 0x07, 0x05, 0x06, 0x01, 0x00, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    };
    const BYTE Ceiling72678Tools[384] = {
      0x43, 0x01, 0x00, 0x01, 0x0A, 0xD7, 0x23, 0x3C, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x08, 0x00, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x08, 0x00, 0x90, 0x00, 0x06, 0x00, 0x0C, 0x01, 0x18, 0x00, 0x28, 0x01, 0x00, 0x00, 0x00, 0x00,
      0x57, 0x03, 0xA7, 0x43, 0xD6, 0xA3, 0x06, 0x43, 0xDF, 0x5C, 0x47, 0x42, 0x00, 0x00, 0x00, 0x3F,
      0x57, 0x03, 0xA7, 0x43, 0xC8, 0x28, 0xCD, 0x42, 0xDF, 0x5C, 0x47, 0x42, 0x01, 0x00, 0x00, 0x3F,
      0x57, 0x03, 0xA7, 0x43, 0xD6, 0xA3, 0x06, 0x43, 0x7A, 0x55, 0x8D, 0xC2, 0x02, 0x00, 0x00, 0x3F,
      0xFF, 0xE7, 0xA3, 0x43, 0xD6, 0xA3, 0x06, 0x43, 0xDF, 0x5C, 0x47, 0x42, 0x03, 0x00, 0x00, 0x3F,
      0x57, 0x03, 0xA7, 0x43, 0xC8, 0x28, 0xCD, 0x42, 0xBE, 0x97, 0x8C, 0xC2, 0x04, 0x00, 0x00, 0x3F,
      0xFF, 0xE7, 0xA3, 0x43, 0xD6, 0xA3, 0x06, 0x43, 0x7A, 0x55, 0x8D, 0xC2, 0x05, 0x00, 0x00, 0x3F,
      0xFF, 0xE7, 0xA3, 0x43, 0xC8, 0x28, 0xCD, 0x42, 0xDF, 0x5C, 0x47, 0x42, 0x06, 0x00, 0x00, 0x3F,
      0xFF, 0xE7, 0xA3, 0x43, 0xC8, 0x28, 0xCD, 0x42, 0xBE, 0x97, 0x8C, 0xC2, 0x07, 0x00, 0x00, 0x3F,
      0x00, 0x00, 0x80, 0x3F, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x57, 0x03, 0xA7, 0xC3,
      0x00, 0x00, 0x80, 0xBF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0xFF, 0xE7, 0xA3, 0x43,
      0x00, 0x00, 0x00, 0x00, 0xFE, 0xFF, 0x7F, 0x3F, 0x00, 0x00, 0x00, 0x00, 0xD5, 0xA3, 0x06, 0xC3,
      0x00, 0x00, 0x00, 0x80, 0x00, 0x00, 0x80, 0xBF, 0x00, 0x00, 0x00, 0x00, 0xC8, 0x28, 0xCD, 0x42,
      0x00, 0x00, 0x00, 0x00, 0x78, 0x5C, 0x3D, 0xBC, 0x9F, 0xFB, 0x7F, 0xBF, 0x52, 0x36, 0x8A, 0xC2,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x3F, 0xDF, 0x5C, 0x47, 0xC2,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xEE, 0xFF, 0x7F, 0xFF,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xEE, 0xFF, 0x7F, 0xFF,
      0x80, 0x04, 0x00, 0x00, 0x80, 0x04, 0x04, 0x00, 0x80, 0x04, 0x08, 0x00, 0x7F, 0x04, 0x0C, 0x00,
      0x7F, 0x04, 0x10, 0x00, 0x80, 0x04, 0x14, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x04, 0x02, 0x00, 0x01, 0x03, 0x05, 0x07, 0x06, 0x05, 0x03, 0x00, 0x02, 0x01, 0x06, 0x07, 0x04,
      0x02, 0x04, 0x07, 0x05, 0x06, 0x01, 0x00, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    };
    // Ceiling72679: + 0x10 .. + 0x190 as the file has it, then as the tools make it
    const BYTE Ceiling72679File[384] = {
      0x43, 0x01, 0x00, 0x01, 0x0A, 0xD7, 0x23, 0x3C, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x08, 0x00, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x08, 0x00, 0x90, 0x00, 0x07, 0x00, 0x0C, 0x01, 0x1A, 0x00, 0x28, 0x01, 0x00, 0x00, 0x00, 0x00,
      0x56, 0xE3, 0xA3, 0x43, 0x4C, 0xBA, 0xC4, 0x42, 0xC8, 0x7E, 0x8C, 0xC2, 0x00, 0x00, 0x00, 0x3F,
      0x56, 0xE3, 0xA3, 0x43, 0x4C, 0xBA, 0xC4, 0x42, 0xE4, 0x5C, 0x47, 0x42, 0x01, 0x00, 0x00, 0x3F,
      0x56, 0xE3, 0xA3, 0x43, 0x9A, 0xA0, 0x06, 0x43, 0x76, 0x55, 0x8D, 0xC2, 0x02, 0x00, 0x00, 0x3F,
      0x56, 0xE3, 0xA3, 0x43, 0x9A, 0xA0, 0x06, 0x43, 0xE4, 0x5C, 0x47, 0x42, 0x03, 0x00, 0x00, 0x3F,
      0xC4, 0x54, 0xA3, 0x43, 0x31, 0x9E, 0x05, 0x43, 0x76, 0x55, 0x8D, 0xC2, 0x04, 0x00, 0x00, 0x3F,
      0xC4, 0x54, 0xA3, 0x43, 0x4C, 0xBA, 0xC4, 0x42, 0xC8, 0x7E, 0x8C, 0xC2, 0x05, 0x00, 0x00, 0x3F,
      0xC2, 0x54, 0xA3, 0x43, 0x31, 0x9E, 0x05, 0x43, 0xE4, 0x5C, 0x47, 0x42, 0x06, 0x00, 0x00, 0x3F,
      0xC2, 0x54, 0xA3, 0x43, 0x4C, 0xBA, 0xC4, 0x42, 0xE4, 0x5C, 0x47, 0x42, 0x07, 0x00, 0x00, 0x3F,
      0x00, 0x00, 0x80, 0x3F, 0x00, 0x00, 0x00, 0x80, 0x00, 0x00, 0x00, 0x00, 0x56, 0xE3, 0xA3, 0xC3,
      0x00, 0x00, 0x80, 0xBF, 0x00, 0x00, 0x00, 0x80, 0x4D, 0xF5, 0x07, 0xB5, 0xC3, 0x54, 0xA3, 0x43,
      0x82, 0xE7, 0x2B, 0xBF, 0x65, 0xB2, 0x3D, 0x3F, 0x00, 0x00, 0x00, 0x00, 0x7B, 0xAF, 0xF0, 0x42,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0xBF, 0x00, 0x00, 0x00, 0x00, 0x4C, 0xBA, 0xC4, 0x42,
      0x04, 0xA9, 0x2B, 0x3C, 0xB2, 0x6A, 0x3D, 0xBC, 0x06, 0xF8, 0x7F, 0xBF, 0x7A, 0x12, 0x91, 0xC2,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x00, 0x00, 0x80, 0x3F, 0xE4, 0x5C, 0x47, 0xC2,
      0x00, 0x00, 0x00, 0x80, 0xA6, 0xD9, 0x42, 0xBC, 0x5D, 0xFB, 0x7F, 0xBF, 0x4B, 0x25, 0x8A, 0xC2,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xEE, 0xFF, 0x7F, 0xFF,
      0x7F, 0x04, 0x00, 0x00, 0x44, 0x04, 0x04, 0x00, 0x44, 0x04, 0x08, 0x00, 0x7F, 0x04, 0x0C, 0x00,
      0x01, 0x03, 0x10, 0x00, 0x80, 0x04, 0x13, 0x00, 0x01, 0x03, 0x17, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x02, 0x03, 0x01, 0x06, 0x04, 0x05, 0x07, 0x04, 0x06, 0x03, 0x02, 0x00, 0x01, 0x07, 0x05,
      0x00, 0x04, 0x02, 0x01, 0x03, 0x06, 0x07, 0x00, 0x05, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    };
    const BYTE Ceiling72679Tools[384] = {
      0x43, 0x01, 0x00, 0x01, 0x0A, 0xD7, 0x23, 0x3C, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x08, 0x00, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x08, 0x00, 0x90, 0x00, 0x07, 0x00, 0x0C, 0x01, 0x1A, 0x00, 0x28, 0x01, 0x00, 0x00, 0x00, 0x00,
      0x56, 0xE3, 0xA3, 0x43, 0xC8, 0x28, 0xCD, 0x42, 0xBD, 0x97, 0x8C, 0xC2, 0x00, 0x00, 0x00, 0x3F,
      0x56, 0xE3, 0xA3, 0x43, 0xC8, 0x28, 0xCD, 0x42, 0xE4, 0x5C, 0x47, 0x42, 0x01, 0x00, 0x00, 0x3F,
      0x56, 0xE3, 0xA3, 0x43, 0x9A, 0xA0, 0x06, 0x43, 0x76, 0x55, 0x8D, 0xC2, 0x02, 0x00, 0x00, 0x3F,
      0x56, 0xE3, 0xA3, 0x43, 0x9A, 0xA0, 0x06, 0x43, 0xE4, 0x5C, 0x47, 0x42, 0x03, 0x00, 0x00, 0x3F,
      0xC4, 0x54, 0xA3, 0x43, 0x31, 0x9E, 0x05, 0x43, 0x76, 0x55, 0x8D, 0xC2, 0x04, 0x00, 0x00, 0x3F,
      0xC4, 0x54, 0xA3, 0x43, 0xC8, 0x28, 0xCD, 0x42, 0x74, 0x98, 0x8C, 0xC2, 0x05, 0x00, 0x00, 0x3F,
      0xC2, 0x54, 0xA3, 0x43, 0x31, 0x9E, 0x05, 0x43, 0xE4, 0x5C, 0x47, 0x42, 0x06, 0x00, 0x00, 0x3F,
      0xC2, 0x54, 0xA3, 0x43, 0xC8, 0x28, 0xCD, 0x42, 0xE4, 0x5C, 0x47, 0x42, 0x07, 0x00, 0x00, 0x3F,
      0x00, 0x00, 0x80, 0x3F, 0x00, 0x00, 0x00, 0x80, 0x00, 0x00, 0x00, 0x00, 0x56, 0xE3, 0xA3, 0xC3,
      0x00, 0x00, 0x80, 0xBF, 0x00, 0x00, 0x00, 0x80, 0x4D, 0xF5, 0x07, 0xB5, 0xC3, 0x54, 0xA3, 0x43,
      0x82, 0xE7, 0x2B, 0xBF, 0x65, 0xB2, 0x3D, 0x3F, 0x00, 0x00, 0x00, 0x00, 0x7B, 0xAF, 0xF0, 0x42,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0xBF, 0x00, 0x00, 0x00, 0x00, 0xC8, 0x28, 0xCD, 0x42,
      0x04, 0xA9, 0x2B, 0x3C, 0xB2, 0x6A, 0x3D, 0xBC, 0x06, 0xF8, 0x7F, 0xBF, 0x7A, 0x12, 0x91, 0xC2,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x00, 0x00, 0x80, 0x3F, 0xE4, 0x5C, 0x47, 0xC2,
      0x3E, 0x49, 0xA4, 0x3A, 0xAC, 0xD9, 0x42, 0xBC, 0x4F, 0xFB, 0x7F, 0xBF, 0xE5, 0xF6, 0x8A, 0xC2,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xEE, 0xFF, 0x7F, 0xFF,
      0x7F, 0x04, 0x00, 0x00, 0x44, 0x04, 0x04, 0x00, 0x44, 0x04, 0x08, 0x00, 0x7F, 0x04, 0x0C, 0x00,
      0x01, 0x03, 0x10, 0x00, 0x80, 0x04, 0x13, 0x00, 0x01, 0x03, 0x17, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x02, 0x03, 0x01, 0x06, 0x04, 0x05, 0x07, 0x04, 0x06, 0x03, 0x02, 0x00, 0x01, 0x07, 0x05,
      0x00, 0x04, 0x02, 0x01, 0x03, 0x06, 0x07, 0x00, 0x05, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    };
    // Ceiling72657: + 0x10 .. + 0x370 as the file has it, then as the tools make it
    const BYTE Ceiling72657File[864] = {
      0x03, 0x01, 0x00, 0x01, 0x0A, 0xD7, 0x23, 0x3C, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x14, 0x00, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x14, 0x00, 0x50, 0x01, 0x14, 0x00, 0x8C, 0x02, 0x46, 0x00, 0xD8, 0x02, 0x00, 0x00, 0x00, 0x00,
      0x3F, 0x80, 0xB4, 0x43, 0xE6, 0x95, 0xEE, 0x42, 0x4F, 0xDA, 0x20, 0x42, 0x00, 0x00, 0x00, 0x3F,
      0x84, 0x94, 0xB4, 0x43, 0x50, 0xB2, 0xEE, 0x42, 0xA4, 0x8F, 0x72, 0x42, 0x01, 0x00, 0x00, 0x3F,
      0xA4, 0x0F, 0xB7, 0x43, 0xB9, 0xC5, 0xC4, 0x42, 0x7F, 0xB7, 0xAD, 0x42, 0x02, 0x00, 0x00, 0x3F,
      0xFE, 0x3A, 0xA3, 0x43, 0xAB, 0xA7, 0xEB, 0x42, 0xE3, 0x97, 0x8C, 0xC2, 0x03, 0x00, 0x00, 0x3F,
      0xFE, 0x3A, 0xA3, 0x43, 0x35, 0x9B, 0xEC, 0x42, 0x1B, 0xB1, 0x26, 0x42, 0x04, 0x00, 0x00, 0x3F,
      0xD2, 0x2E, 0xB7, 0x43, 0xD5, 0x66, 0xED, 0x42, 0xE4, 0x48, 0xA5, 0x42, 0x05, 0x00, 0x00, 0x3F,
      0x85, 0x0B, 0xA8, 0x43, 0xD4, 0x4F, 0xED, 0x42, 0xA6, 0xF6, 0x63, 0x42, 0x06, 0x00, 0x00, 0x3F,
      0x1D, 0x7F, 0xAD, 0x43, 0x0C, 0xC5, 0xC4, 0x42, 0xD4, 0xCF, 0x9E, 0x42, 0x07, 0x00, 0x00, 0x3F,
      0x4B, 0x9E, 0xAD, 0x43, 0x6B, 0x61, 0xED, 0x42, 0x0D, 0x30, 0x96, 0x42, 0x08, 0x00, 0x00, 0x3F,
      0xD1, 0x4C, 0xB3, 0x43, 0xEC, 0x04, 0xC4, 0x42, 0x65, 0xF0, 0x8C, 0xC2, 0x09, 0x00, 0x00, 0x3F,
      0x6C, 0xA9, 0xB3, 0x43, 0xFA, 0x87, 0xED, 0x42, 0xB9, 0xA1, 0x8C, 0xC2, 0x0A, 0x00, 0x00, 0x3F,
      0xBF, 0xF6, 0xB3, 0x43, 0x54, 0x44, 0xED, 0x42, 0xA4, 0xA6, 0x8C, 0xC2, 0x0B, 0x00, 0x00, 0x3F,
      0x0F, 0x49, 0xB4, 0x43, 0xC7, 0x1A, 0xEE, 0x42, 0x5C, 0x06, 0x48, 0xC1, 0x0C, 0x00, 0x00, 0x3F,
      0x47, 0x0F, 0xA7, 0x43, 0x94, 0xBB, 0xC4, 0x42, 0x98, 0x7C, 0x70, 0x42, 0x0D, 0x00, 0x00, 0x3F,
      0xFE, 0x3A, 0xA3, 0x43, 0x65, 0x0E, 0xC4, 0x42, 0x3E, 0xAF, 0x2C, 0x42, 0x0E, 0x00, 0x00, 0x3F,
      0xFE, 0x3A, 0xA3, 0x43, 0x20, 0xCE, 0xC3, 0x42, 0x00, 0x32, 0x04, 0x42, 0x0F, 0x00, 0x00, 0x3F,
      0xFE, 0x3A, 0xA3, 0x43, 0xFA, 0x67, 0xC3, 0x42, 0xA5, 0xE1, 0x8C, 0xC2, 0x10, 0x00, 0x00, 0x3F,
      0xFE, 0x3A, 0xA3, 0x43, 0xFA, 0x67, 0xC3, 0x42, 0xA5, 0xE1, 0x8C, 0xC2, 0x10, 0x00, 0x00, 0x3F,
      0xFE, 0x3A, 0xA3, 0x43, 0xFA, 0x67, 0xC3, 0x42, 0xA5, 0xE1, 0x8C, 0xC2, 0x10, 0x00, 0x00, 0x3F,
      0xFE, 0x3A, 0xA3, 0x43, 0xFA, 0x67, 0xC3, 0x42, 0xA5, 0xE1, 0x8C, 0xC2, 0x10, 0x00, 0x00, 0x3F,
      0x88, 0xB9, 0x1C, 0x3C, 0xDF, 0xFC, 0x7F, 0xBF, 0x82, 0xC2, 0xFD, 0x3A, 0x15, 0x6C, 0xBD, 0x42,
      0xE2, 0xC5, 0xE9, 0xBC, 0xB0, 0xE4, 0x7F, 0x3F, 0x9E, 0x70, 0x8E, 0xBB, 0x1F, 0x89, 0xD9, 0xC2,
      0x32, 0x30, 0x7F, 0x3F, 0xF0, 0xBB, 0x82, 0xBD, 0xB1, 0x96, 0x42, 0xBD, 0xF7, 0x46, 0xB1, 0xC3,
      0x06, 0xB8, 0x7F, 0x3F, 0x20, 0x95, 0xA9, 0xBC, 0x39, 0x2A, 0x2C, 0xBD, 0x2A, 0x04, 0xB4, 0xC3,
      0x00, 0x00, 0x80, 0xBF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFE, 0x3A, 0xA3, 0x43,
      0xDF, 0xDE, 0x56, 0xBF, 0x2E, 0x82, 0x22, 0x3E, 0x08, 0x1A, 0x05, 0x3F, 0xCA, 0x91, 0x69, 0x43,
      0x58, 0x73, 0x87, 0xBA, 0x55, 0x8F, 0xEA, 0x3B, 0x4A, 0xFE, 0x7F, 0xBF, 0x0D, 0x9A, 0x8D, 0xC2,
      0x45, 0x60, 0x9E, 0xBB, 0x0C, 0xFA, 0x7F, 0xBF, 0x91, 0x29, 0x4E, 0x3C, 0x93, 0x1B, 0xC6, 0x42,
      0xD0, 0x35, 0x53, 0xBF, 0x0B, 0xE1, 0x01, 0x3E, 0xCC, 0xF7, 0x0C, 0x3F, 0x42, 0x14, 0x66, 0x43,
      0x38, 0x44, 0xB8, 0xBE, 0x3E, 0xB0, 0x4A, 0x3E, 0xBA, 0x68, 0x69, 0x3F, 0xA8, 0x08, 0x04, 0x42,
      0x84, 0x56, 0xB6, 0xBE, 0x76, 0xA1, 0x46, 0x3E, 0x46, 0x01, 0x6A, 0x3F, 0x7C, 0x40, 0xFF, 0x41,
      0x9F, 0xE0, 0xF5, 0xBC, 0x74, 0xD9, 0x7F, 0x3F, 0xC1, 0xDB, 0x87, 0x3C, 0x34, 0xE3, 0xDA, 0xC2,
      0xE9, 0xAF, 0x5B, 0xBC, 0x4E, 0xD7, 0x7F, 0x3F, 0x32, 0x72, 0x05, 0x3D, 0x73, 0xD0, 0xE8, 0xC2,
      0x6F, 0x3E, 0x69, 0xBF, 0x72, 0xD1, 0xF8, 0x3C, 0x7D, 0x74, 0xD2, 0x3E, 0xFE, 0x5B, 0x8A, 0x43,
      0x51, 0xB5, 0x5B, 0x3E, 0xBA, 0x06, 0x7A, 0x3F, 0x8F, 0x00, 0x1B, 0xBC, 0x93, 0xC1, 0x41, 0xC3,
      0xC4, 0x64, 0x1E, 0x3E, 0x20, 0xEA, 0x7C, 0x3F, 0x22, 0x22, 0xBD, 0xBB, 0x8F, 0x76, 0x2D, 0xC3,
      0x12, 0x6E, 0x0E, 0x3E, 0x7E, 0x82, 0x7D, 0x3F, 0x4F, 0xFB, 0x76, 0xBB, 0x74, 0x31, 0x28, 0xC3,
      0xCA, 0x4B, 0xB3, 0xBD, 0x36, 0x90, 0x7E, 0xBF, 0x3E, 0x4B, 0x73, 0x3D, 0x1B, 0xFD, 0xF6, 0x42,
      0xAD, 0x8E, 0x41, 0xBF, 0x5A, 0xF9, 0xEB, 0x3E, 0xAE, 0xE9, 0xED, 0x3E, 0x65, 0xF1, 0x2C, 0x43,
      0x59, 0xE5, 0x19, 0xBB, 0x34, 0x3A, 0x7D, 0x3D, 0x74, 0x82, 0x7F, 0xBF, 0x12, 0x5C, 0x99, 0xC2,
      0x01, 0x04, 0x00, 0x00, 0x02, 0x07, 0x04, 0x00, 0x04, 0x03, 0x0B, 0x00, 0x04, 0x03, 0x0E, 0x00,
      0x22, 0x05, 0x11, 0x00, 0x04, 0x03, 0x16, 0x00, 0x04, 0x04, 0x19, 0x00, 0x01, 0x04, 0x1D, 0x00,
      0x04, 0x03, 0x21, 0x00, 0x00, 0x03, 0x24, 0x00, 0x00, 0x03, 0x27, 0x00, 0x02, 0x03, 0x2A, 0x00,
      0x02, 0x03, 0x2D, 0x00, 0x0F, 0x03, 0x30, 0x00, 0x05, 0x04, 0x33, 0x00, 0x01, 0x03, 0x37, 0x00,
      0x01, 0x03, 0x3A, 0x00, 0x08, 0x03, 0x3D, 0x00, 0x1A, 0x03, 0x40, 0x00, 0x04, 0x03, 0x43, 0x00,
      0x09, 0x02, 0x0F, 0x10, 0x0A, 0x03, 0x04, 0x06, 0x01, 0x00, 0x0C, 0x02, 0x09, 0x0B, 0x02, 0x0B,
      0x05, 0x0E, 0x04, 0x03, 0x10, 0x0F, 0x04, 0x0D, 0x08, 0x03, 0x0B, 0x09, 0x10, 0x0F, 0x02, 0x07,
      0x0E, 0x07, 0x08, 0x0D, 0x05, 0x08, 0x07, 0x02, 0x05, 0x07, 0x01, 0x06, 0x08, 0x01, 0x08, 0x05,
      0x04, 0x0E, 0x0D, 0x0C, 0x05, 0x0B, 0x0A, 0x00, 0x05, 0x0C, 0x00, 0x01, 0x05, 0x07, 0x0D, 0x0E,
      0x04, 0x08, 0x06, 0x03, 0x0A, 0x0B, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    };
    const BYTE Ceiling72657Tools[864] = {
      0x03, 0x01, 0x00, 0x01, 0x0A, 0xD7, 0x23, 0x3C, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x14, 0x00, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x14, 0x00, 0x50, 0x01, 0x12, 0x00, 0x8C, 0x02, 0x46, 0x00, 0xD8, 0x02, 0x00, 0x00, 0x00, 0x00,
      0x3F, 0x80, 0xB4, 0x43, 0xE6, 0x95, 0xEE, 0x42, 0x4F, 0xDA, 0x20, 0x42, 0x00, 0x00, 0x00, 0x3F,
      0x84, 0x94, 0xB4, 0x43, 0x50, 0xB2, 0xEE, 0x42, 0xA4, 0x8F, 0x72, 0x42, 0x01, 0x00, 0x00, 0x3F,
      0xFE, 0x3A, 0xA3, 0x43, 0xC8, 0x28, 0xCD, 0x42, 0xC5, 0xCF, 0x8C, 0xC2, 0x02, 0x00, 0x00, 0x3F,
      0xFE, 0x3A, 0xA3, 0x43, 0xAB, 0xA7, 0xEB, 0x42, 0xE3, 0x97, 0x8C, 0xC2, 0x03, 0x00, 0x00, 0x3F,
      0xFE, 0x3A, 0xA3, 0x43, 0x35, 0x9B, 0xEC, 0x42, 0x1B, 0xB1, 0x26, 0x42, 0x04, 0x00, 0x00, 0x3F,
      0xD2, 0x2E, 0xB7, 0x43, 0xD5, 0x66, 0xED, 0x42, 0xE4, 0x48, 0xA5, 0x42, 0x05, 0x00, 0x00, 0x3F,
      0x85, 0x0B, 0xA8, 0x43, 0xD4, 0x4F, 0xED, 0x42, 0xA6, 0xF6, 0x63, 0x42, 0x06, 0x00, 0x00, 0x3F,
      0x1D, 0x40, 0xA6, 0x43, 0xC8, 0x28, 0xCD, 0x42, 0x3A, 0xE4, 0x60, 0x42, 0x07, 0x00, 0x00, 0x3F,
      0x4B, 0x9E, 0xAD, 0x43, 0x6B, 0x61, 0xED, 0x42, 0x0D, 0x30, 0x96, 0x42, 0x08, 0x00, 0x00, 0x3F,
      0xFE, 0x3A, 0xA3, 0x43, 0xC8, 0x28, 0xCD, 0x42, 0xD8, 0x56, 0x2B, 0x42, 0x09, 0x00, 0x00, 0x3F,
      0x6C, 0xA9, 0xB3, 0x43, 0xFA, 0x87, 0xED, 0x42, 0xB9, 0xA1, 0x8C, 0xC2, 0x0A, 0x00, 0x00, 0x3F,
      0xBF, 0xF6, 0xB3, 0x43, 0x54, 0x44, 0xED, 0x42, 0xA4, 0xA6, 0x8C, 0xC2, 0x0B, 0x00, 0x00, 0x3F,
      0x0F, 0x49, 0xB4, 0x43, 0xC7, 0x1A, 0xEE, 0x42, 0x5C, 0x06, 0x48, 0xC1, 0x0C, 0x00, 0x00, 0x3F,
      0x14, 0x16, 0xB7, 0x43, 0xC8, 0x28, 0xCD, 0x42, 0xE9, 0xF9, 0xAB, 0x42, 0x0D, 0x00, 0x00, 0x3F,
      0x1F, 0x7F, 0xAF, 0x43, 0xC8, 0x28, 0xCD, 0x42, 0xFB, 0x25, 0xA0, 0x42, 0x0E, 0x00, 0x00, 0x3F,
      0x8E, 0x85, 0xAD, 0x43, 0xC8, 0x28, 0xCD, 0x42, 0xBC, 0x07, 0x9D, 0x42, 0x0F, 0x00, 0x00, 0x3F,
      0x5A, 0x6B, 0xA8, 0x43, 0xC8, 0x28, 0xCD, 0x42, 0xF0, 0xE6, 0x7C, 0x42, 0x10, 0x00, 0x00, 0x3F,
      0x6C, 0x6B, 0xB6, 0x43, 0xC8, 0x28, 0xCD, 0x42, 0xDC, 0x36, 0x59, 0x42, 0x11, 0x00, 0x00, 0x3F,
      0x79, 0x72, 0xB3, 0x43, 0xC8, 0x28, 0xCD, 0x42, 0x0C, 0xE0, 0x8C, 0xC2, 0x12, 0x00, 0x00, 0x3F,
      0x79, 0x72, 0xB3, 0x43, 0xC8, 0x28, 0xCD, 0x42, 0x0C, 0xE0, 0x8C, 0xC2, 0x12, 0x00, 0x00, 0x3F,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0xBF, 0x00, 0x00, 0x00, 0x00, 0xC8, 0x28, 0xCD, 0x42,
      0xE2, 0xC5, 0xE9, 0xBC, 0xB0, 0xE4, 0x7F, 0x3F, 0x9E, 0x70, 0x8E, 0xBB, 0x1F, 0x89, 0xD9, 0xC2,
      0x32, 0x30, 0x7F, 0x3F, 0xF0, 0xBB, 0x82, 0xBD, 0xB1, 0x96, 0x42, 0xBD, 0xF7, 0x46, 0xB1, 0xC3,
      0x06, 0xB8, 0x7F, 0x3F, 0x20, 0x95, 0xA9, 0xBC, 0x39, 0x2A, 0x2C, 0xBD, 0x2A, 0x04, 0xB4, 0xC3,
      0x00, 0x00, 0x80, 0xBF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFE, 0x3A, 0xA3, 0x43,
      0xDF, 0xDE, 0x56, 0xBF, 0x2E, 0x82, 0x22, 0x3E, 0x08, 0x1A, 0x05, 0x3F, 0xCA, 0x91, 0x69, 0x43,
      0x58, 0x73, 0x87, 0xBA, 0x55, 0x8F, 0xEA, 0x3B, 0x4A, 0xFE, 0x7F, 0xBF, 0x0D, 0x9A, 0x8D, 0xC2,
      0xD0, 0x35, 0x53, 0xBF, 0x0B, 0xE1, 0x01, 0x3E, 0xCC, 0xF7, 0x0C, 0x3F, 0x42, 0x14, 0x66, 0x43,
      0x38, 0x44, 0xB8, 0xBE, 0x3E, 0xB0, 0x4A, 0x3E, 0xBA, 0x68, 0x69, 0x3F, 0xA8, 0x08, 0x04, 0x42,
      0x84, 0x56, 0xB6, 0xBE, 0x76, 0xA1, 0x46, 0x3E, 0x46, 0x01, 0x6A, 0x3F, 0x7C, 0x40, 0xFF, 0x41,
      0x9F, 0xE0, 0xF5, 0xBC, 0x74, 0xD9, 0x7F, 0x3F, 0xC1, 0xDB, 0x87, 0x3C, 0x34, 0xE3, 0xDA, 0xC2,
      0xE9, 0xAF, 0x5B, 0xBC, 0x4E, 0xD7, 0x7F, 0x3F, 0x32, 0x72, 0x05, 0x3D, 0x73, 0xD0, 0xE8, 0xC2,
      0x6F, 0x3E, 0x69, 0xBF, 0x72, 0xD1, 0xF8, 0x3C, 0x7D, 0x74, 0xD2, 0x3E, 0xFE, 0x5B, 0x8A, 0x43,
      0x51, 0xB5, 0x5B, 0x3E, 0xBA, 0x06, 0x7A, 0x3F, 0x8F, 0x00, 0x1B, 0xBC, 0x93, 0xC1, 0x41, 0xC3,
      0xC4, 0x64, 0x1E, 0x3E, 0x20, 0xEA, 0x7C, 0x3F, 0x22, 0x22, 0xBD, 0xBB, 0x8F, 0x76, 0x2D, 0xC3,
      0x12, 0x6E, 0x0E, 0x3E, 0x7E, 0x82, 0x7D, 0x3F, 0x4F, 0xFB, 0x76, 0xBB, 0x74, 0x31, 0x28, 0xC3,
      0xAD, 0x8E, 0x41, 0xBF, 0x5A, 0xF9, 0xEB, 0x3E, 0xAE, 0xE9, 0xED, 0x3E, 0x65, 0xF1, 0x2C, 0x43,
      0x59, 0xE5, 0x19, 0xBB, 0x34, 0x3A, 0x7D, 0x3D, 0x74, 0x82, 0x7F, 0xBF, 0x12, 0x5C, 0x99, 0xC2,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xEE, 0xFF, 0x7F, 0xFF,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xEE, 0xFF, 0x7F, 0xFF,
      0x7A, 0x09, 0x00, 0x00, 0x02, 0x07, 0x09, 0x00, 0x04, 0x03, 0x10, 0x00, 0x04, 0x04, 0x13, 0x00,
      0x22, 0x04, 0x17, 0x00, 0x04, 0x04, 0x1B, 0x00, 0x04, 0x04, 0x1F, 0x00, 0x04, 0x03, 0x23, 0x00,
      0x00, 0x04, 0x26, 0x00, 0x00, 0x03, 0x2A, 0x00, 0x02, 0x03, 0x2D, 0x00, 0x02, 0x03, 0x30, 0x00,
      0x0F, 0x03, 0x33, 0x00, 0x05, 0x04, 0x36, 0x00, 0x01, 0x03, 0x3A, 0x00, 0x01, 0x03, 0x3D, 0x00,
      0x1A, 0x03, 0x40, 0x00, 0x04, 0x03, 0x43, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x12, 0x11, 0x0D, 0x0E, 0x0F, 0x10, 0x07, 0x09, 0x02, 0x0A, 0x03, 0x04, 0x06, 0x01, 0x00, 0x0C,
      0x12, 0x0B, 0x11, 0x11, 0x0B, 0x05, 0x0D, 0x09, 0x04, 0x03, 0x02, 0x04, 0x07, 0x10, 0x08, 0x03,
      0x0B, 0x12, 0x02, 0x0F, 0x08, 0x10, 0x05, 0x08, 0x0F, 0x0E, 0x0D, 0x05, 0x0E, 0x01, 0x06, 0x08,
      0x01, 0x08, 0x05, 0x04, 0x09, 0x07, 0x0C, 0x05, 0x0B, 0x0A, 0x00, 0x05, 0x0C, 0x00, 0x01, 0x05,
      0x04, 0x08, 0x06, 0x03, 0x0A, 0x0B, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    };
    // Ceiling72656: + 0x10 .. + 0x3C0 as the file has it, then as the tools make it
    const BYTE Ceiling72656File[944] = {
      0x03, 0x01, 0x00, 0x01, 0x0A, 0xD7, 0x23, 0x3C, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x14, 0x00, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x18, 0x00, 0x50, 0x01, 0x15, 0x00, 0xCC, 0x02, 0x4A, 0x00, 0x28, 0x03, 0x00, 0x00, 0x00, 0x00,
      0x61, 0x85, 0xB7, 0x43, 0xF6, 0xB5, 0xC3, 0x42, 0x19, 0xF2, 0xA1, 0x42, 0x00, 0x00, 0x00, 0x3F,
      0x76, 0x87, 0xB7, 0x43, 0xCC, 0xB4, 0xC3, 0x42, 0x5E, 0xED, 0xA1, 0x42, 0x01, 0x00, 0x00, 0x3F,
      0xD0, 0xA3, 0xB7, 0x43, 0xBE, 0xD7, 0x06, 0x43, 0x2B, 0xAD, 0x91, 0x42, 0x02, 0x00, 0x00, 0x3F,
      0xB2, 0x01, 0xA7, 0x43, 0x73, 0x9A, 0x06, 0x43, 0x25, 0xF9, 0x7C, 0x40, 0x03, 0x00, 0x00, 0x3F,
      0xB2, 0x01, 0xA7, 0x43, 0xB8, 0xB4, 0x06, 0x43, 0xAC, 0x23, 0x02, 0x42, 0x04, 0x00, 0x00, 0x3F,
      0x32, 0xA7, 0xB7, 0x43, 0x52, 0xD1, 0x06, 0x43, 0xA3, 0xB6, 0x91, 0x42, 0x05, 0x00, 0x00, 0x3F,
      0x1A, 0xE2, 0xAA, 0x43, 0x0E, 0xB7, 0x06, 0x43, 0xC6, 0x94, 0x46, 0x42, 0x06, 0x00, 0x00, 0x3F,
      0xA6, 0x45, 0xAE, 0x43, 0x48, 0xA3, 0xC3, 0x42, 0xC6, 0xC4, 0x86, 0x42, 0x07, 0x00, 0x00, 0x3F,
      0x32, 0x55, 0xB0, 0x43, 0x74, 0xAB, 0xC3, 0x42, 0x23, 0x10, 0x93, 0x42, 0x08, 0x00, 0x00, 0x3F,
      0x1D, 0x73, 0xB0, 0x43, 0x52, 0xD1, 0x06, 0x43, 0xAE, 0x8D, 0x82, 0x42, 0x09, 0x00, 0x00, 0x3F,
      0xBC, 0x20, 0xB6, 0x43, 0x00, 0xFA, 0xC2, 0x42, 0xDB, 0xEB, 0x8C, 0xC2, 0x0A, 0x00, 0x00, 0x3F,
      0xE7, 0xCB, 0xB6, 0x43, 0xEB, 0xF2, 0x05, 0x43, 0xCD, 0x70, 0x8C, 0xC2, 0x0B, 0x00, 0x00, 0x3F,
      0x7A, 0x34, 0xB7, 0x43, 0xF3, 0x96, 0x06, 0x43, 0x20, 0x1B, 0x2D, 0x40, 0x0C, 0x00, 0x00, 0x3F,
      0x2F, 0x40, 0xB7, 0x43, 0xC9, 0x9C, 0x06, 0x43, 0x2B, 0x42, 0x0F, 0x41, 0x0D, 0x00, 0x00, 0x3F,
      0x09, 0xE6, 0xA9, 0x43, 0xC9, 0xA6, 0xC3, 0x42, 0x71, 0x4A, 0x59, 0x42, 0x0E, 0x00, 0x00, 0x3F,
      0xB2, 0x01, 0xA7, 0x43, 0x80, 0xF3, 0x05, 0x43, 0x56, 0x67, 0x8C, 0xC2, 0x0F, 0x00, 0x00, 0x3F,
      0xB2, 0x01, 0xA7, 0x43, 0xF1, 0x99, 0xC3, 0x42, 0x0A, 0x28, 0x0E, 0x42, 0x10, 0x00, 0x00, 0x3F,
      0xB2, 0x01, 0xA7, 0x43, 0x2B, 0xFB, 0xC2, 0x42, 0x64, 0xE2, 0x8C, 0xC2, 0x11, 0x00, 0x00, 0x3F,
      0xB2, 0x01, 0xA7, 0x43, 0x2B, 0xFB, 0xC2, 0x42, 0x64, 0xE2, 0x8C, 0xC2, 0x11, 0x00, 0x00, 0x3F,
      0xB2, 0x01, 0xA7, 0x43, 0x2B, 0xFB, 0xC2, 0x42, 0x64, 0xE2, 0x8C, 0xC2, 0x11, 0x00, 0x00, 0x3F,
      0x2B, 0xC9, 0x7F, 0x3F, 0xC6, 0x9A, 0x15, 0xBD, 0xF1, 0xC4, 0x96, 0xBC, 0xF2, 0xD7, 0xB4, 0xC3,
      0x7F, 0xF8, 0x7F, 0x3F, 0xCE, 0xB1, 0x18, 0xBC, 0x23, 0x3C, 0x43, 0xBC, 0xDC, 0x91, 0xB6, 0xC3,
      0x1D, 0x20, 0x9B, 0xB8, 0xCF, 0xFF, 0x7F, 0xBF, 0x59, 0xA3, 0x1C, 0x3B, 0xE0, 0x5D, 0xC3, 0x42,
      0x00, 0x00, 0x80, 0xBF, 0x00, 0x00, 0x00, 0x80, 0x00, 0x00, 0x00, 0x00, 0xB2, 0x01, 0xA7, 0x43,
      0xDA, 0x28, 0xAF, 0x38, 0x77, 0xFD, 0x7F, 0x3F, 0x38, 0xF7, 0x0F, 0xBC, 0x5E, 0x97, 0x06, 0xC3,
      0x73, 0xF3, 0x8F, 0x39, 0x91, 0xFF, 0x7F, 0x3F, 0xD1, 0x78, 0x6C, 0xBB, 0x0D, 0xAE, 0x06, 0xC3,
      0x11, 0x53, 0x19, 0xBA, 0xC7, 0xB2, 0xD8, 0x3B, 0x8E, 0xFE, 0x7F, 0xBF, 0x11, 0xC8, 0x8D, 0xC2,
      0xCB, 0xC6, 0xA8, 0xBB, 0xD9, 0xFE, 0x7F, 0xBF, 0x69, 0xB6, 0x3F, 0x3B, 0xA2, 0xD4, 0xC6, 0x42,
      0x7E, 0x98, 0x74, 0xBF, 0xD2, 0x2C, 0xC4, 0x3C, 0xF4, 0x9D, 0x96, 0x3E, 0x1E, 0x2B, 0x99, 0x43,
      0xC7, 0xC3, 0x4E, 0xBF, 0x4E, 0x81, 0x08, 0x3E, 0x57, 0x09, 0x13, 0x3F, 0x43, 0x90, 0x65, 0x43,
      0x1E, 0x01, 0x68, 0xBF, 0x5A, 0xB4, 0xCD, 0x3D, 0x00, 0x3A, 0xD2, 0x3E, 0x7E, 0xE8, 0x89, 0x43,
      0x3B, 0xC6, 0xE9, 0xBE, 0x0F, 0x47, 0x49, 0x3E, 0x0E, 0x21, 0x5E, 0x3F, 0x32, 0xFB, 0x9B, 0x42,
      0x78, 0xF9, 0xE6, 0xBE, 0x4F, 0x21, 0x47, 0x3E, 0x0F, 0xFB, 0x5E, 0x3F, 0x9A, 0x0B, 0x98, 0x42,
      0xD5, 0x4F, 0x53, 0xBF, 0x3E, 0xCD, 0xE9, 0x3D, 0x46, 0x86, 0x0D, 0x3F, 0x30, 0x48, 0x6F, 0x43,
      0xCE, 0x9C, 0xBE, 0xBC, 0xFA, 0xEA, 0x7F, 0x3F, 0x10, 0x02, 0x24, 0x3C, 0xDE, 0x6E, 0xFE, 0xC2,
      0x67, 0x2D, 0x7A, 0x3F, 0xB5, 0xBF, 0x58, 0x3E, 0xA3, 0xC7, 0x54, 0xBC, 0x6E, 0x45, 0xC1, 0xC3,
      0x52, 0xD5, 0x54, 0x3F, 0xC0, 0x36, 0x0E, 0x3F, 0x50, 0x92, 0x68, 0xBC, 0x76, 0xAD, 0xBD, 0xC3,
      0x7E, 0x44, 0x31, 0x3F, 0x5C, 0xAC, 0x38, 0x3F, 0x3C, 0xD5, 0x34, 0xBC, 0x6B, 0x65, 0xAF, 0xC3,
      0x85, 0x2E, 0xDC, 0xBE, 0xFE, 0x45, 0x49, 0x3E, 0x30, 0x93, 0x61, 0x3F, 0x36, 0x85, 0x86, 0x42,
      0xA5, 0x37, 0x01, 0x3F, 0x96, 0x0F, 0x3A, 0x3E, 0x61, 0x0B, 0x58, 0x3F, 0xDA, 0xB0, 0x87, 0xC3,
      0xCF, 0x8A, 0x85, 0xBD, 0x48, 0x52, 0x7D, 0xBF, 0x63, 0xCB, 0x03, 0x3E, 0xBF, 0xAF, 0xDC, 0x42,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xEE, 0xFF, 0x7F, 0xFF,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xEE, 0xFF, 0x7F, 0xFF,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xEE, 0xFF, 0x7F, 0xFF,
      0x02, 0x03, 0x00, 0x00, 0x02, 0x03, 0x03, 0x00, 0x00, 0x05, 0x06, 0x00, 0x18, 0x05, 0x0B, 0x00,
      0x00, 0x04, 0x10, 0x00, 0x00, 0x06, 0x14, 0x00, 0x7E, 0x04, 0x1A, 0x00, 0x00, 0x04, 0x1E, 0x00,
      0x0C, 0x03, 0x22, 0x00, 0x03, 0x03, 0x25, 0x00, 0x0C, 0x03, 0x28, 0x00, 0x01, 0x03, 0x2B, 0x00,
      0x01, 0x03, 0x2E, 0x00, 0x03, 0x04, 0x31, 0x00, 0x02, 0x03, 0x35, 0x00, 0x12, 0x03, 0x38, 0x00,
      0x1E, 0x03, 0x3B, 0x00, 0x30, 0x03, 0x3E, 0x00, 0x02, 0x03, 0x41, 0x00, 0x4F, 0x03, 0x44, 0x00,
      0x0C, 0x03, 0x47, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x01, 0x0A, 0x0B, 0x01, 0x0B, 0x05, 0x11, 0x0A, 0x01, 0x08, 0x07, 0x11, 0x10, 0x04, 0x03, 0x0F,
      0x0F, 0x03, 0x0C, 0x0B, 0x0C, 0x03, 0x04, 0x09, 0x02, 0x0D, 0x0F, 0x0B, 0x0A, 0x11, 0x10, 0x11,
      0x07, 0x0E, 0x04, 0x10, 0x0E, 0x06, 0x08, 0x09, 0x04, 0x0E, 0x06, 0x02, 0x09, 0x08, 0x00, 0x02,
      0x08, 0x08, 0x06, 0x0E, 0x07, 0x04, 0x06, 0x09, 0x05, 0x0B, 0x0D, 0x0B, 0x0C, 0x0D, 0x02, 0x05,
      0x0D, 0x00, 0x05, 0x02, 0x00, 0x01, 0x05, 0x00, 0x08, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    };
    const BYTE Ceiling72656Tools[944] = {
      0x03, 0x01, 0x00, 0x01, 0x0A, 0xD7, 0x23, 0x3C, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x14, 0x00, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x14, 0x00, 0x50, 0x01, 0x13, 0x00, 0xCC, 0x02, 0x4A, 0x00, 0x28, 0x03, 0x00, 0x00, 0x00, 0x00,
      0x9B, 0x86, 0xA9, 0x43, 0xC8, 0x28, 0xCD, 0x42, 0x93, 0x0E, 0x4E, 0x42, 0x00, 0x00, 0x00, 0x3F,
      0xB2, 0x01, 0xA7, 0x43, 0xC8, 0x28, 0xCD, 0x42, 0xAC, 0x99, 0x0C, 0x42, 0x01, 0x00, 0x00, 0x3F,
      0xD0, 0xA3, 0xB7, 0x43, 0xBE, 0xD7, 0x06, 0x43, 0x2B, 0xAD, 0x91, 0x42, 0x02, 0x00, 0x00, 0x3F,
      0xB2, 0x01, 0xA7, 0x43, 0x73, 0x9A, 0x06, 0x43, 0x25, 0xF9, 0x7C, 0x40, 0x03, 0x00, 0x00, 0x3F,
      0xB2, 0x01, 0xA7, 0x43, 0xB8, 0xB4, 0x06, 0x43, 0xAC, 0x23, 0x02, 0x42, 0x04, 0x00, 0x00, 0x3F,
      0x32, 0xA7, 0xB7, 0x43, 0x52, 0xD1, 0x06, 0x43, 0xA3, 0xB6, 0x91, 0x42, 0x05, 0x00, 0x00, 0x3F,
      0x1A, 0xE2, 0xAA, 0x43, 0x0E, 0xB7, 0x06, 0x43, 0xC6, 0x94, 0x46, 0x42, 0x06, 0x00, 0x00, 0x3F,
      0xB3, 0x89, 0xB7, 0x43, 0xC8, 0x28, 0xCD, 0x42, 0xFA, 0xDE, 0x9F, 0x42, 0x07, 0x00, 0x00, 0x3F,
      0x85, 0x8B, 0xB7, 0x43, 0xC8, 0x28, 0xCD, 0x42, 0xA1, 0xDA, 0x9F, 0x42, 0x08, 0x00, 0x00, 0x3F,
      0x1D, 0x73, 0xB0, 0x43, 0x52, 0xD1, 0x06, 0x43, 0xAE, 0x8D, 0x82, 0x42, 0x09, 0x00, 0x00, 0x3F,
      0xB6, 0xA1, 0xAF, 0x43, 0xC8, 0x28, 0xCD, 0x42, 0xBB, 0xEA, 0x8C, 0x42, 0x0A, 0x00, 0x00, 0x3F,
      0xE7, 0xCB, 0xB6, 0x43, 0xEB, 0xF2, 0x05, 0x43, 0xCD, 0x70, 0x8C, 0xC2, 0x0B, 0x00, 0x00, 0x3F,
      0x7A, 0x34, 0xB7, 0x43, 0xF3, 0x96, 0x06, 0x43, 0x20, 0x1B, 0x2D, 0x40, 0x0C, 0x00, 0x00, 0x3F,
      0x2F, 0x40, 0xB7, 0x43, 0xC9, 0x9C, 0x06, 0x43, 0x2B, 0x42, 0x0F, 0x41, 0x0D, 0x00, 0x00, 0x3F,
      0x85, 0x06, 0xAA, 0x43, 0xC8, 0x28, 0xCD, 0x42, 0x36, 0xE1, 0x56, 0x42, 0x0E, 0x00, 0x00, 0x3F,
      0xB2, 0x01, 0xA7, 0x43, 0x80, 0xF3, 0x05, 0x43, 0x56, 0x67, 0x8C, 0xC2, 0x0F, 0x00, 0x00, 0x3F,
      0x09, 0x59, 0xB0, 0x43, 0xC8, 0x28, 0xCD, 0x42, 0xE6, 0xF1, 0x90, 0x42, 0x10, 0x00, 0x00, 0x3F,
      0xE7, 0x6E, 0xB7, 0x43, 0xC8, 0x28, 0xCD, 0x42, 0xFA, 0xAA, 0x74, 0x42, 0x11, 0x00, 0x00, 0x3F,
      0xA3, 0x38, 0xB6, 0x43, 0xC8, 0x28, 0xCD, 0x42, 0xAC, 0xDA, 0x8C, 0xC2, 0x12, 0x00, 0x00, 0x3F,
      0xB2, 0x01, 0xA7, 0x43, 0xC8, 0x28, 0xCD, 0x42, 0x37, 0xD1, 0x8C, 0xC2, 0x13, 0x00, 0x00, 0x3F,
      0x2B, 0xC9, 0x7F, 0x3F, 0xC6, 0x9A, 0x15, 0xBD, 0xF1, 0xC4, 0x96, 0xBC, 0xF2, 0xD7, 0xB4, 0xC3,
      0x7F, 0xF8, 0x7F, 0x3F, 0xCE, 0xB1, 0x18, 0xBC, 0x23, 0x3C, 0x43, 0xBC, 0xDC, 0x91, 0xB6, 0xC3,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0xBF, 0x00, 0x00, 0x00, 0x00, 0xC8, 0x28, 0xCD, 0x42,
      0x00, 0x00, 0x80, 0xBF, 0x00, 0x00, 0x00, 0x80, 0x00, 0x00, 0x00, 0x00, 0xB2, 0x01, 0xA7, 0x43,
      0xDA, 0x28, 0xAF, 0x38, 0x77, 0xFD, 0x7F, 0x3F, 0x38, 0xF7, 0x0F, 0xBC, 0x5E, 0x97, 0x06, 0xC3,
      0x73, 0xF3, 0x8F, 0x39, 0x91, 0xFF, 0x7F, 0x3F, 0xD1, 0x78, 0x6C, 0xBB, 0x0D, 0xAE, 0x06, 0xC3,
      0x11, 0x53, 0x19, 0xBA, 0xC7, 0xB2, 0xD8, 0x3B, 0x8E, 0xFE, 0x7F, 0xBF, 0x11, 0xC8, 0x8D, 0xC2,
      0x7E, 0x98, 0x74, 0xBF, 0xD2, 0x2C, 0xC4, 0x3C, 0xF4, 0x9D, 0x96, 0x3E, 0x1E, 0x2B, 0x99, 0x43,
      0xC7, 0xC3, 0x4E, 0xBF, 0x4E, 0x81, 0x08, 0x3E, 0x57, 0x09, 0x13, 0x3F, 0x43, 0x90, 0x65, 0x43,
      0x1E, 0x01, 0x68, 0xBF, 0x5A, 0xB4, 0xCD, 0x3D, 0x00, 0x3A, 0xD2, 0x3E, 0x7E, 0xE8, 0x89, 0x43,
      0x3B, 0xC6, 0xE9, 0xBE, 0x0F, 0x47, 0x49, 0x3E, 0x0E, 0x21, 0x5E, 0x3F, 0x32, 0xFB, 0x9B, 0x42,
      0x67, 0x59, 0xE7, 0xBE, 0xFD, 0x0C, 0x47, 0x3E, 0x53, 0xE3, 0x5E, 0x3F, 0x18, 0xA8, 0x98, 0x42,
      0xD5, 0x4F, 0x53, 0xBF, 0x3E, 0xCD, 0xE9, 0x3D, 0x46, 0x86, 0x0D, 0x3F, 0x30, 0x48, 0x6F, 0x43,
      0xCE, 0x9C, 0xBE, 0xBC, 0xFA, 0xEA, 0x7F, 0x3F, 0x10, 0x02, 0x24, 0x3C, 0xDE, 0x6E, 0xFE, 0xC2,
      0x67, 0x2D, 0x7A, 0x3F, 0xB5, 0xBF, 0x58, 0x3E, 0xA3, 0xC7, 0x54, 0xBC, 0x6E, 0x45, 0xC1, 0xC3,
      0x52, 0xD5, 0x54, 0x3F, 0xC0, 0x36, 0x0E, 0x3F, 0x50, 0x92, 0x68, 0xBC, 0x76, 0xAD, 0xBD, 0xC3,
      0x7E, 0x44, 0x31, 0x3F, 0x5C, 0xAC, 0x38, 0x3F, 0x3C, 0xD5, 0x34, 0xBC, 0x6B, 0x65, 0xAF, 0xC3,
      0x85, 0x2E, 0xDC, 0xBE, 0xFE, 0x45, 0x49, 0x3E, 0x30, 0x93, 0x61, 0x3F, 0x36, 0x85, 0x86, 0x42,
      0xA5, 0x37, 0x01, 0x3F, 0x96, 0x0F, 0x3A, 0x3E, 0x61, 0x0B, 0x58, 0x3F, 0xDA, 0xB0, 0x87, 0xC3,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xEE, 0xFF, 0x7F, 0xFF,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xEE, 0xFF, 0x7F, 0xFF,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xEE, 0xFF, 0x7F, 0xFF,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xEE, 0xFF, 0x7F, 0xFF,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xEE, 0xFF, 0x7F, 0xFF,
      0x02, 0x03, 0x00, 0x00, 0x02, 0x04, 0x03, 0x00, 0x7D, 0x0A, 0x07, 0x00, 0x18, 0x05, 0x11, 0x00,
      0x00, 0x04, 0x16, 0x00, 0x00, 0x06, 0x1A, 0x00, 0x7E, 0x04, 0x20, 0x00, 0x0C, 0x03, 0x24, 0x00,
      0x03, 0x04, 0x27, 0x00, 0x0C, 0x04, 0x2B, 0x00, 0x00, 0x03, 0x2F, 0x00, 0x00, 0x03, 0x32, 0x00,
      0x03, 0x03, 0x35, 0x00, 0x02, 0x03, 0x38, 0x00, 0x12, 0x03, 0x3B, 0x00, 0x1E, 0x03, 0x3E, 0x00,
      0x30, 0x03, 0x41, 0x00, 0x02, 0x03, 0x44, 0x00, 0x4F, 0x03, 0x47, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x12, 0x0B, 0x11, 0x11, 0x0B, 0x05, 0x08, 0x12, 0x11, 0x08, 0x07, 0x10, 0x0A, 0x0E, 0x00, 0x01,
      0x13, 0x01, 0x04, 0x03, 0x0F, 0x13, 0x0F, 0x03, 0x0C, 0x0B, 0x0C, 0x03, 0x04, 0x09, 0x02, 0x0D,
      0x0F, 0x0B, 0x12, 0x13, 0x04, 0x01, 0x00, 0x06, 0x0A, 0x10, 0x09, 0x04, 0x00, 0x0E, 0x06, 0x02,
      0x09, 0x10, 0x07, 0x02, 0x10, 0x0A, 0x06, 0x0E, 0x04, 0x06, 0x09, 0x05, 0x0B, 0x0D, 0x0B, 0x0C,
      0x0D, 0x02, 0x05, 0x0D, 0x07, 0x05, 0x02, 0x08, 0x05, 0x07, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    };

    // Its members (their Rows and Box unused): the shapes are in their physics
    const int MemberCount = 2;
    const gates::Piece Members[MemberCount] = {
      { "skycity_building_01_mesh", 1, 7225, 2, 0, {}, {} },
      { "skycity_buildings_trainstation_exterior_mesh", 1, 6753, 3, 0, {}, {} } };
    struct Shape
    {
      const char* Name;             // in the log
      int Member;                   // its member (Members)
      __int64 FromRoot[2];          // the object's distance from the root shape, first packfile / second
      int Size;                     // the object's bytes
      BYTE const* File;             // + 0x10 .. Size as the file has it
      BYTE const* Tools;            // and as the tools make it (+ 0x10 .. + 0x2F the same)
      float const (*Opened)[4];     // the space the change opens, world planes (inside n . p + d <= 0)
      int OpenedCount;
      bool Tops;                    // Faith on a top of that space is out of it (false: the shape's own rest is over it)
      int DataBit;                  // g_SkyDataHas: the props data has its stand-ins
    };
    // An object's header (+ 0x30 .. + 0x4F: its arrays' counts and offsets, the offsets the same in both images) and its
    // arrays after it
    const int HeaderStart = 0x30, DataStart = 0x50;
    // The space each change opens, hkworld's planes: the block's old box west of its new face (top, bottom, south, north,
    // old west, the new west turned round); the wedge's old planes (reshape/cconsts.py); each ceiling leaf's old planes
    // and the cut (the last: h <= 107.28835) (ceiling/verify/judge/judge_extra.json)
    const float BlockOpened[6][4] = {
      { -0.0034228f, 0.9999312f, 0.0112210f, -107.2883363f },
      { 0.0034228f, -0.9999312f, -0.0112210f, 72.9196626f },
      { 0.2915665f, 0.0117314f, -0.9564786f, -3230.7712575f },
      { -0.2915665f, -0.0117314f, 0.9564786f, 2926.7211158f },
      { -0.9565445f, -0.0000022f, -0.2915865f, -195.8209931f },
      { 0.9565445f, 0.0000022f, 0.2915865f, 73.5400000f },
    };
    const float WedgeOpened[11][4] = {
      { -0.3897671f, 0.2859804f, 0.8753838f, 2768.8063330f },
      { 0.2925258f, 0.0117314f, -0.9561857f, -2991.3226112f },
      { 0.0034228f, -0.9999312f, -0.0112210f, 104.1985579f },
      { -0.4528496f, -0.1063500f, 0.8852214f, 2901.4634746f },
      { 0.2915840f, 0.0041462f, -0.9565362f, -2990.5214410f },
      { -0.1686608f, 0.9815205f, -0.0903943f, -306.4915659f },
      { 0.9565445f, 0.0000022f, 0.2915865f, 73.5531161f },
      { -0.1984770f, 0.9799432f, -0.0178371f, -74.3110162f },
      { 0.8224358f, 0.1662962f, 0.5440082f, 877.5354741f },
      { -0.8708435f, -0.4736894f, -0.1313393f, 272.1491491f },
      { -0.8262704f, -0.5015834f, -0.2563030f, -118.3755384f },
    };
    const float Ceiling72656Opened[22][4] = {
      { 0.9612352f, -0.0363040f, 0.2733293f, -6.2509177f },
      { 0.9599413f, -0.0091771f, 0.2800510f, 10.1140203f },
      { 0.0026551f, -0.9999564f, -0.0089565f, 109.5995744f },
      { -0.9565445f, -0.0000022f, -0.2915865f, -77.3293680f },
      { -0.0007808f, 0.9999957f, 0.0028404f, -165.8750354f },
      { -0.0021081f, 0.9999670f, 0.0078498f, -150.2025539f },
      { 0.2909779f, 0.0183438f, -0.9565539f, -3099.3997619f },
      { -0.0023570f, -0.9999480f, -0.0099247f, 110.8639258f },
      { -0.9997854f, 0.0204924f, 0.0030428f, 804.0538590f },
      { -0.9404977f, 0.1265568f, 0.3153530f, 1636.0453308f },
      { -0.9869454f, 0.0956159f, 0.1296007f, 1145.9750858f },
      { -0.6904118f, 0.1863664f, 0.6989987f, 2516.8847557f },
      { -0.6861437f, 0.1842304f, 0.7037514f, 2527.2497107f },
      { -0.9511446f, 0.1076659f, 0.2893649f, 1573.3447286f },
      { -0.0285973f, 0.9994929f, 0.0140073f, -111.1807733f },
      { 0.9378501f, 0.2118087f, 0.2749078f, -24.8810928f },
      { 0.7974893f, 0.5556531f, 0.2350755f, -82.9758226f },
      { 0.6631094f, 0.7214615f, 0.1994473f, -102.7782374f },
      { -0.6689419f, 0.1862045f, 0.7196142f, 2557.8366155f },
      { 0.2361392f, 0.1717883f, 0.9564137f, 2491.0475181f },
      { -0.0965118f, -0.9909787f, 0.0929872f, 477.9400495f },
      { -0.0034228f, 0.9999312f, 0.0112210f, -107.2883378f },
    };
    const float Ceiling72657Opened[21][4] = {
      { 0.0120082f, -0.9999063f, -0.0065795f, 109.2172586f },
      { -0.0294506f, 0.9995654f, -0.0012623f, -138.3767134f },
      { 0.9675812f, -0.0632712f, 0.2445062f, -88.0811253f },
      { 0.9678201f, -0.0202043f, 0.2508309f, -76.3312598f },
      { -0.9565445f, -0.0000022f, -0.2915865f, -84.8817354f },
      { -0.9550013f, 0.1525875f, 0.2543415f, 1462.0681250f },
      { 0.2905458f, 0.0188888f, -0.9566746f, -3099.4844794f },
      { -0.0048696f, -0.9999880f, -0.0005938f, 139.8795939f },
      { -0.9501743f, 0.1203646f, 0.2875435f, 1557.6618360f },
      { -0.6107705f, 0.1872274f, 0.7693538f, 2645.1051354f },
      { -0.6078326f, 0.1832377f, 0.7726341f, 2652.6202228f },
      { -0.0369662f, 0.9991485f, 0.0183251f, -76.2736191f },
      { -0.0257457f, 0.9989282f, 0.0384660f, -27.9674973f },
      { -0.9914671f, 0.0255471f, 0.1278295f, 1146.5669381f },
      { 0.2046505f, 0.9767095f, 0.0644728f, -138.7472847f },
      { 0.1462608f, 0.9879476f, 0.0506680f, -131.9671061f },
      { 0.1307567f, 0.9902487f, 0.0480645f, -127.0542666f },
      { -0.0976573f, -0.9950166f, 0.0201273f, 272.0397817f },
      { -0.8602880f, 0.4554022f, 0.2291581f, 1266.9359064f },
      { 0.2885501f, 0.0735278f, -0.9546374f, -3100.4397529f },
      { -0.0034228f, 0.9999312f, 0.0112210f, -107.2883378f },
    };
    const float Ceiling72678Opened[7][4] = {
      { 0.9565445f, 0.0000022f, 0.2915865f, 77.3165201f },
      { -0.9565445f, -0.0000022f, -0.2915865f, -83.5301425f },
      { -0.0034228f, 0.9999312f, 0.0112210f, -139.3486614f },
      { 0.0034228f, -0.9999312f, -0.0112210f, 103.0725494f },
      { 0.2915865f, 0.0001737f, -0.9565444f, -3097.1534221f },
      { -0.2915665f, -0.0117314f, 0.9564786f, 2978.4633327f },
      { -0.0034228f, 0.9999312f, 0.0112210f, -107.2883378f },
    };
    const float Ceiling72679Opened[8][4] = {
      { 0.9565445f, 0.0000022f, 0.2915865f, 83.5665500f },
      { -0.9565443f, -0.0000021f, -0.2915870f, -84.6819447f },
      { -0.6448571f, 0.7409510f, -0.1874859f, -159.3635104f },
      { 0.0034228f, -0.9999312f, -0.0112210f, 103.0725494f },
      { 0.3015925f, 0.0001697f, -0.9534369f, -3096.1072841f },
      { -0.2915665f, -0.0117314f, 0.9564786f, 2978.4633136f },
      { 0.2915865f, -0.0001613f, -0.9565444f, -3097.1066919f },
      { -0.0034228f, 0.9999312f, 0.0112210f, -107.2883378f },
    };
    const int ShapeCount = 6;
    const Shape Shapes[ShapeCount] = {
      { "Sky City's base block (its west face to the station hall's east end)", 0, { 2406608, 2408624 }, 400, BlockFile,
        BlockTools, BlockOpened, 6, true, 1 },
      { "the Sky City station's north wedge (the invisible wall at the walkway's doorway)", 1, { 2396160, 2398080 }, 496,
        WedgeFile, WedgeTools, WedgeOpened, 11, true, 2 },
      { "Sky City's leaf 72657 (the hall's low ceiling over its east end, its middle)", 0, { 2408608, 2410624 }, 880,
        Ceiling72657File, Ceiling72657Tools, Ceiling72657Opened, 21, false, 1 },
      { "Sky City's leaf 72678 (the hall's low ceiling over its east end, south)", 0, { 2420288, 2422304 }, 400,
        Ceiling72678File, Ceiling72678Tools, Ceiling72678Opened, 7, false, 1 },
      { "Sky City's leaf 72679 (the hall's low ceiling over its east end, its south edge)", 0, { 2420768, 2422784 }, 400,
        Ceiling72679File, Ceiling72679Tools, Ceiling72679Opened, 8, false, 1 },
      { "Sky City's leaf 72656 (the hall's low ceiling over its east end, its east edge)", 0, { 2407568, 2409584 }, 960,
        Ceiling72656File, Ceiling72656Tools, Ceiling72656Opened, 22, false, 1 } };
    static_assert(sizeof(BlockFile) == 400 - 0x10 && sizeof(BlockTools) == 400 - 0x10 && sizeof(WedgeFile) == 496 - 0x10 &&
      sizeof(WedgeTools) == 496 - 0x10 && sizeof(Ceiling72657File) == 880 - 0x10 && sizeof(Ceiling72657Tools) == 880 - 0x10 &&
      sizeof(Ceiling72678File) == 400 - 0x10 && sizeof(Ceiling72678Tools) == 400 - 0x10 &&
      sizeof(Ceiling72679File) == 400 - 0x10 && sizeof(Ceiling72679Tools) == 400 - 0x10 &&
      sizeof(Ceiling72656File) == 960 - 0x10 && sizeof(Ceiling72656Tools) == 960 - 0x10, "an image is its object from + 0x10");
    // Points only the stand-ins hold (inside a box of the new props data; in none of GroundProps v12's): the fill's
    // three parts at h 90, three of the hull's stand-in boxes' middles (reshape/cconsts.py, hullprobes.py)
    const float DataProbes[6][3] = {
      { 729.596f, 124.545f, -2855.316f }, { 787.863f, 126.890f, -3046.458f }, { 748.936f, 125.323f, -2918.759f },
      { 685.742f, 161.299f, -2912.637f }, { 743.831f, 166.318f, -2896.568f }, { 760.598f, 168.919f, -2891.555f } };
    const float Middle[2] = { 740.f, -2950.f };       // the station's (x, z)
    const float SearchDistance = 600.f;              // its zone's group looked for while she's this near
    const ULONGLONG SearchMs = 3000;
    const float NotFoundLogDistance = 150.f;
    const ULONGLONG CheckMs = 250;
    const ULONGLONG ChangeAfterMs = 300;              // the boxes near her made this long before the change
    const ULONGLONG GiveUpMs = 10000;                // changed, Solid street off, its member's physics not readable
    const ULONGLONG FlushLateMs = 2000;              // a change asked for and no flush of its bodies' world has done it
    // Faith in a space: a point of her body (her feet + BodyAt up) within BodyRadius of it; on a top of it (a plane
    // facing up, TopNy or more): her feet no more than OnTopBelow under it
    const float BodyAt[5] = { 0.15f, 0.6f, 1.05f, 1.5f, 1.85f };
    const float BodyRadius = 0.35f;
    const float TopNy = 0.7f, OnTopBelow = 0.1f;
    // The records' (DogenDoor.cpp's)
    const int RecordWorldOffset = 0x30;
    const int StaticRecordHavokIdOffset = 0x50;
    const int RecordHavokIdOffset = 0x58;
    typedef void(__fastcall* tBodyChanged)(__int64, unsigned int);

    // The camera hook's (Update's) own
    struct ShapeState
    {
      bool Changed;                 // the tools' bytes on its object, as the last flush that looked at it found
      bool WaitLogged, UnknownLogged, OtherLogged, OtherBytesLogged;
      bool DiffLogged;              // its "aren't as the tools know them" line has given a first byte not as the file's
    };
    struct MemberState
    {
      gates::PieceState Ps;         // its group (Group 0: not found)
      ULONGLONG Unreadable;         // since when its physics isn't readable while a shape is changed (0: it is)
      bool SeenNone;                // the last flush that looked found none of its bodies' shapes (or not its physics)
      bool FaultLogged;
    };
    ShapeState g_State[ShapeCount] = {};
    MemberState g_Members[MemberCount] = {};
    bool g_ImageOk[ShapeCount] = {};                 // its two images as CheckImages wants them
    ULONGLONG g_LastCheck = 0, g_LastSearch = 0;
    bool g_NotFoundLogged = false, g_WasSolid = false;

    // Handed between Update (the camera hook) and Flush (the physics flush of the bodies' world, before the game's part:
    // no step runs then, and the game makes its own body changes there), under g_Lock. Update asks each check; the flush
    // looks, changes what's asked, renews the bodies' caches and answers.
    struct Request
    {
      bool Posted;                  // not taken by a flush yet
      unsigned int Seq;
      ULONGLONG At;                 // when first asked (a run of asks, not answered)
      gates::PieceState Ps;
      bool Asked[ShapeCount], Wanted[ShapeCount], ToNew[ShapeCount], ToOld[ShapeCount];
      float Feet[3];
      bool HasFeet;
    };
    struct Answer
    {
      bool Done;                    // not taken by Update yet
      unsigned int Seq;
      bool Fault, NoComponent;
      int AsFile[ShapeCount], AsTools[ShapeCount], Unknown[ShapeCount], Now[ShapeCount];
      int Other[ShapeCount];        // bodies in another world on a root of their own (looked at only, for the log)
      int NotReadable[ShapeCount];  // of Unknown: bodies (or their records) that couldn't be read
      bool NoWriter[ShapeCount];    // the client world's body couldn't be read: none looked at
      bool HasDiff[ShapeCount];     // a root's object as neither image: Diff, the first byte (Differs) not as the file
      int Diff[ShapeCount][2];      // has it at each packfile's distance (0 none, -1 not 16-byte aligned)
      unsigned __int64 OtherBytes[ShapeCount];   // bytes the tools don't go by that aren't the file's (OtherBytes)
    };
    struct Renewal                  // a body whose caches its own world's next flushes renew (each world's in its own)
    {
      __int64 Havok, Root;          // its hknpWorld, and the root its shape was when queued
      unsigned int Id;
      int Left;                     // how many more of that world's flushes
      int Age;                      // other worlds' flushes since (a world that doesn't flush again: dropped)
    };
    const int MaxRenewals = 64, MaxRenewalAge = 2000;
    SRWLOCK g_Lock = SRWLOCK_INIT;
    Request g_Request[MemberCount] = {};
    Answer g_Answer[MemberCount] = {};
    Renewal g_Renewals[MaxRenewals] = {};
    int g_RenewalCount = 0;
    unsigned int g_Seq = 0;
    bool g_LateLogged[MemberCount] = {};
    std::atomic<bool> g_FlushHas{ false };           // something posted or to renew (so the flush needn't lock)

    // Which of the stand-ins the props data has (DataProbes in a box of it): g_SkyDataHas's bits
    int DataHas(Props const& props)
    {
      bool in[6] = {};
      for (Props::Box const& b : props.Boxes)
      {
        if (b.Min[0] > 1e29f)
          continue;
        for (int k = 0; k < 6; ++k)
        {
          float const* p = DataProbes[k];
          if (in[k] || p[0] < b.Min[0] || p[0] > b.Max[0] || p[1] < b.Min[1] || p[1] > b.Max[1] || p[2] < b.Min[2] ||
              p[2] > b.Max[2])
            continue;
          float const* half = &props.Halves[(size_t)b.Shape * 3];
          float dx = p[0] - b.Rows[9], dy = p[1] - b.Rows[10], dz = p[2] - b.Rows[11];
          bool inside = true;
          for (int a = 0; a < 3 && inside; ++a)
            inside = fabsf(dx * b.Rows[a * 3] + dy * b.Rows[a * 3 + 1] + dz * b.Rows[a * 3 + 2]) <= half[a] + 1e-3f;
          in[k] = inside;
        }
      }
      return (in[0] && in[1] && in[2] ? 1 : 0) | (in[3] && in[4] && in[5] ? 2 : 0);
    }

    // An image's bytes at an object's offset (image byte 0 is the object's + 0x10)
    unsigned int ImageU16(BYTE const* image, int at)
    {
      return image[at - 0x10] | (unsigned int)image[at - 0x10 + 1] << 8;
    }

    // Its arrays' places, from an image's header: vertices, planes, faces, indices, the end
    struct Layout
    {
      int Vertices, Planes, Faces, Indices, End;
    };
    Layout LayoutOf(Shape const& s, BYTE const* image)
    {
      return { 0x30 + (int)ImageU16(image, 0x32), 0x40 + (int)ImageU16(image, 0x42), 0x44 + (int)ImageU16(image, 0x46),
        0x48 + (int)ImageU16(image, 0x4A), s.Size };
    }

    // Its images as the write wants them (Init): the same offsets, the arrays in order where 16-byte (vertices, planes)
    // and 4-byte (faces, indices) stores fit them, each count within its array's room, nothing but the four counts
    // differing in + 0x10 .. + 0x4F
    bool ImagesOk(Shape const& s)
    {
      Layout a = LayoutOf(s, s.File), b = LayoutOf(s, s.Tools);
      if (memcmp(&a, &b, sizeof(a)) || a.Vertices != DataStart || a.Planes <= a.Vertices || a.Faces <= a.Planes ||
          a.Indices <= a.Faces || a.End <= a.Indices || (a.Planes - a.Vertices) % 16 || (a.Faces - a.Planes) % 16 ||
          (a.Indices - a.Faces) % 4 || (a.End - a.Indices) % 4 || a.End % 16)
        return false;
      for (BYTE const* image : { s.File, s.Tools })
      {
        unsigned int vertices = ImageU16(image, 0x30), planes = ImageU16(image, 0x40), faces = ImageU16(image, 0x44),
          indices = ImageU16(image, 0x48);
        if (vertices < 4 || vertices % 4 || (int)vertices * 16 > a.Planes - a.Vertices || planes < 4 || planes < faces ||
            (int)planes * 16 > a.Faces - a.Planes || faces < 4 || (int)faces * 4 > a.Indices - a.Faces ||
            (int)indices > a.End - a.Indices)
          return false;
      }
      for (int at = 0x10; at < DataStart; ++at)
      {
        bool count = at == 0x30 || at == 0x31 || at == 0x40 || at == 0x41 || at == 0x44 || at == 0x45 || at == 0x48 ||
          at == 0x49;
        if (!count && s.File[at - 0x10] != s.Tools[at - 0x10])
          return false;
      }
      return true;
    }

    // Init's: each shape's images looked at (a shape whose images the write can't take isn't changed)
    void CheckImages()
    {
      for (int k = 0; k < ShapeCount; ++k)
      {
        g_ImageOk[k] = ImagesOk(Shapes[k]);
        if (!g_ImageOk[k])
          util::log::Warning("Street level: %s: its bytes in the tools aren't as their write wants them; it isn't changed",
            Shapes[k].Name);
      }
    }

    // Faith in the space a change opens (a point of her body within BodyRadius of it), and on a top of it
    void Where(Shape const& s, float const* feet, bool& in, bool& onTop)
    {
      in = onTop = false;
      for (float at : BodyAt)
      {
        float p[3] = { feet[0], feet[1] + at, feet[2] };
        float most = -1e30f;
        for (int i = 0; i < s.OpenedCount; ++i)
          most = (std::max)(most, s.Opened[i][0] * p[0] + s.Opened[i][1] * p[1] + s.Opened[i][2] * p[2] + s.Opened[i][3]);
        in |= most < BodyRadius;
      }
      for (int i = 0; i < s.OpenedCount && s.Tops; ++i)
        onTop |= s.Opened[i][1] >= TopNy &&
          s.Opened[i][0] * feet[0] + s.Opened[i][1] * feet[1] + s.Opened[i][2] * feet[2] + s.Opened[i][3] >= -OnTopBelow;
    }

    // A body of the component: its Havok world, id and shape (the record's, DogenDoor.cpp's), and its world's index in
    // the manager (a group's bodies: 0 the client world, where Faith moves, 1 the effects world, each its own hknpWorld
    // with the same root: block_reports.txt section 3). Game memory: only inside __try.
    bool BodyOf(__int64 component, int b, __int64& havok, unsigned int& id, __int64& shape, int& worldIndex)
    {
      __int64 base = Base();
      unsigned int bodyId = *(unsigned int const*)(*(__int64 const*)(component + gates::ComponentBodiesOffset) + b * 4);
      worldIndex = (int)((bodyId >> 16) & 0xFF);
      __int64 manager = *(__int64 const*)(component + gates::ComponentManagerOffset);
      __int64 worlds = manager >= 0x10000 ? *(__int64 const*)(manager + gates::ManagerWorldsOffset) : 0;
      __int64 world = worlds >= 0x10000 ? *(__int64 const*)(worlds + worldIndex * 8) : 0;
      __int64 records = world >= 0x10000 ? *(__int64 const*)(world + WorldRecordsOffset) : 0;
      __int64 record = records >= 0x10000 ? *(__int64 const*)(records + (bodyId & 0xFFFF) * 8) : 0;
      if (record < 0x10000)
        return false;
      __int64 const* vtable = *(__int64 const* const*)record;
      int idAt;
      if (vtable[gates::RecordPartOffSlot] == base + gates::StaticRecordPartOffRva)
        idAt = StaticRecordHavokIdOffset;
      else if (vtable[gates::RecordPartOffSlot] == base + gates::RecordPartOffRva)
        idAt = RecordHavokIdOffset;
      else
        return false;
      __int64 physics = *(__int64 const*)(record + RecordWorldOffset);
      havok = physics >= 0x10000 ? *(__int64 const*)(physics + WorldHavokOffset) : 0;
      id = *(unsigned int const*)(record + idAt);
      if (havok < 0x10000 || id >= (*(unsigned int const*)(havok + HavokCapacityOffset) & 0x3FFFFFFF))
        return false;
      __int64 body = *(__int64 const*)(havok + HavokBodiesOffset) + (__int64)id * BodySize;
      shape = *(__int64 const*)(body + BodyShapeOffset);
      return shape >= 0x10000 && *(unsigned int const*)(body + BodyIdOffset) == id;
    }

    // The bytes of an object the tools go by: its header's four arrays' counts and offsets (+ 0x30, + 0x40, + 0x44,
    // + 0x48, 4 bytes each) and all of it from DataStart to its end (the arrays and the room between them). Not + 0x10 ..
    // + 0x2F (flags, convex radius, user data, the properties pointer) nor the header's other bytes (+ 0x34 .. + 0x3F,
    // + 0x4C .. + 0x4F): the game may set those after loading the file, and the write keeps them as they are. (The
    // first in-game run of a compare of the whole object, 2026-11-10 14:05, found every body as neither image; the
    // 01:16 build, which went by the counts and the arrays only, changed them.)
    bool Counted(int at)
    {
      return (at >= 0x30 && at < 0x34) || (at >= 0x40 && at < 0x4C);
    }
    // The first byte (its offset in the object) of the ones the tools go by where the object isn't as an image has it;
    // 0: none. Game memory: only inside __try.
    int Differs(Shape const& s, __int64 obj, BYTE const* image)
    {
      for (int at = HeaderStart; at < DataStart; ++at)
        if (Counted(at) && *(BYTE const*)(obj + at) != image[at - 0x10])
          return at;
      if (!memcmp((void const*)(obj + DataStart), image + DataStart - 0x10, s.Size - DataStart))
        return 0;
      for (int at = DataStart; at < s.Size; ++at)
        if (*(BYTE const*)(obj + at) != image[at - 0x10])
          return at;
      return 0;
    }
    // The bytes before DataStart the tools don't go by that aren't as the file has them (bit at - 0x10). Game memory:
    // only inside __try.
    unsigned __int64 OtherBytes(Shape const& s, __int64 obj)
    {
      unsigned __int64 bits = 0;
      for (int at = 0x10; at < DataStart; ++at)
        if (!Counted(at) && *(BYTE const*)(obj + at) != s.File[at - 0x10])
          bits |= 1ull << (at - 0x10);
      return bits;
    }

    // The shape's object at a root: 1 as the file has it, 2 as the tools make it, 0 neither (not the zone's root, or
    // something else changed it), in the bytes the tools go by (Counted, Differs); its place in obj. Game memory: only
    // inside __try.
    int ObjectAt(Shape const& s, __int64 root, __int64& obj)
    {
      for (int pf = 0; pf < 2; ++pf)
      {
        __int64 o = root + s.FromRoot[pf];
        if (o & 15)
          continue;
        bool old = !Differs(s, o, s.File);
        bool made = !old && !Differs(s, o, s.Tools);
        if (old || made)
        {
          obj = o;
          return old ? 1 : 2;
        }
      }
      return 0;
    }

    // One store of an image's bytes into the object, in the order written (the compiler keeps it; x64 keeps stores in
    // order). 16 and 8 bytes aligned: one store each.
    void Put16(__int64 obj, BYTE const* image, int at)
    {
      _mm_store_si128((__m128i*)(obj + at), _mm_loadu_si128((__m128i const*)(image + at - 0x10)));
      std::atomic_signal_fence(std::memory_order_seq_cst);
    }
    // (the header's counts and offsets (Counted) from the image, one store each: + 0x40 8 bytes (the plane and face
    // counts together), + 0x30 and + 0x48 4 bytes; the header's other bytes (+ 0x34 .. + 0x3F, + 0x4C .. + 0x4F) are never
    // written, so no byte the tools don't go by is)
    void PutHeader(__int64 obj, BYTE const* image, int at)
    {
      if (at == 0x40)
      {
        unsigned __int64 v;
        memcpy(&v, image + at - 0x10, 8);
        *(unsigned __int64 volatile*)(obj + at) = v;
      }
      else if (at == 0x30 || at == 0x48)
      {
        unsigned int v;
        memcpy(&v, image + at - 0x10, 4);
        *(unsigned int volatile*)(obj + at) = v;
      }
      else
        return;
      std::atomic_signal_fence(std::memory_order_seq_cst);
    }
    void Put4(__int64 obj, BYTE const* image, int at)
    {
      unsigned int v;
      memcpy(&v, image + at - 0x10, 4);
      *(unsigned int volatile*)(obj + at) = v;
      std::atomic_signal_fence(std::memory_order_seq_cst);
    }

    // The object (as the other image has it) written as the tools make it (make) or as its file has it, in the order
    // that keeps a read in between near the old shape or the new (Research section 25: the game's readers take the
    // counts from the header each time; a count read is the old or the new with either's arrays in its room).
    // Made: the vertices, the planes up, the faces and indices, then the counts. Back: the counts, the planes down, the
    // faces and indices, the vertices. The counts: + 0x48 and + 0x30 (4 bytes each) and + 0x40 (8), nothing else of the
    // header (PutHeader). Game memory: only inside __try.
    void Write(Shape const& s, __int64 obj, bool make)
    {
      BYTE const* to = make ? s.Tools : s.File;
      Layout l = LayoutOf(s, to);
      if (make)
      {
        for (int at = l.Vertices; at < l.Planes; at += 16)
          Put16(obj, to, at);
        for (int at = l.Planes; at < l.Faces; at += 16)
          Put16(obj, to, at);
        for (int at = l.Faces; at < l.End; at += 4)
          Put4(obj, to, at);
        for (int at = DataStart - 8; at >= HeaderStart; at -= 8)
          PutHeader(obj, to, at);
      }
      else
      {
        for (int at = HeaderStart; at < DataStart; at += 8)
          PutHeader(obj, to, at);
        for (int at = l.Faces - 16; at >= l.Planes; at -= 16)
          Put16(obj, to, at);
        for (int at = l.End - 4; at >= l.Faces; at -= 4)
          Put4(obj, to, at);
        for (int at = l.Planes - 16; at >= l.Vertices; at -= 16)
          Put16(obj, to, at);
      }
    }

    // A body still the one with that id with that root as its shape (its slot's id and shape: a slot reused by another
    // body has another shape): its caches renewed. Game memory: only inside __try.
    bool Renew(__int64 havok, unsigned int id, __int64 root)
    {
      if (id >= (*(unsigned int const*)(havok + HavokCapacityOffset) & 0x3FFFFFFF))
        return false;
      __int64 body = *(__int64 const*)(havok + HavokBodiesOffset) + (__int64)id * BodySize;
      if (*(unsigned int const*)(body + BodyIdOffset) != id || *(__int64 const*)(body + BodyShapeOffset) != root)
        return false;
      ((tBodyChanged)(Base() + gates::BodyChangedRva))(havok, id);
      return true;
    }

    // A body to be renewed at its world's next flushes (under g_Lock; one there already: the most of the two counts)
    void QueueRenewal(__int64 havok, unsigned int id, __int64 root, int times)
    {
      for (int i = 0; i < g_RenewalCount; ++i)
        if (g_Renewals[i].Havok == havok && g_Renewals[i].Id == id)
        {
          g_Renewals[i].Root = root;
          g_Renewals[i].Left = (std::max)(g_Renewals[i].Left, times);
          return;
        }
      if (g_RenewalCount < MaxRenewals)
        g_Renewals[g_RenewalCount++] = { havok, root, id, times, 0 };
    }

    // The renewals due in this world's flush (under g_Lock)
    void RenewDue(__int64 havokWorld)
    {
      int kept = 0;
      for (int i = 0; i < g_RenewalCount; ++i)
      {
        Renewal e = g_Renewals[i];
        if (e.Havok == havokWorld)
        {
          __try
          {
            Renew(e.Havok, e.Id, e.Root);
          }
          __except (EXCEPTION_EXECUTE_HANDLER)
          {
            e.Left = 0;
          }
          --e.Left;
        }
        else if (++e.Age > MaxRenewalAge)
          e.Left = 0;
        if (e.Left > 0)
          g_Renewals[kept++] = e;
      }
      g_RenewalCount = kept;
    }

    // Another world's root looked at only (its own read: a fault there costs nothing done here). Game memory.
    int LookOnly(Shape const& s, __int64 root)
    {
      __int64 obj = 0;
      __try
      {
        return ObjectAt(s, root, obj);
      }
      __except (EXCEPTION_EXECUTE_HANDLER)
      {
        return 0;
      }
    }

    // A member's request in a world's flush (its Havok world): -1 not this world's (left for its own flush), else its
    // answer. Written in the flush of the world of its first body in the client world (Faith's; only for a component
    // with no body there, its first readable body's; that body not readable now: nothing written, nothing seen): each
    // of that world's bodies' roots looked at once (bodies sharing a root: the first's states); a shape written where
    // it's the other and asked so (made only while Solid street is on now); every body there on a root written to
    // renewed now (0x143F3CD00) and again at that world's next flush. Its bodies in other worlds: on a root looked at
    // here, that root's states, and renewed at their own world's next two flushes (that world may be stepping now:
    // never called for it from here); on a root of their own, looked at only, for the log (Other: not what's held).
    // Game memory: only inside __try.
    int FlushMember(int m, Request const& r, __int64 havokWorld, Answer& a)
    {
      __int64 component = gates::StillPiece(Members[m], r.Ps) ? gates::Component(r.Ps) : 0;
      if (!component)
      {
        a.NoComponent = true;
        return 1;
      }
      int bodies = *(unsigned short const*)(component + gates::ComponentBodyCountOffset);
      if (bodies <= 0 || bodies > gates::MaxBodies)
      {
        for (int k = 0; k < ShapeCount; ++k)
        {
          a.Unknown[k] += r.Asked[k];
          a.NotReadable[k] += r.Asked[k];
        }
        return 1;
      }
      __int64 roots[gates::MaxBodies] = {}, havoks[gates::MaxBodies] = {};
      unsigned int ids[gates::MaxBodies] = {};
      int worldIndex[gates::MaxBodies] = {};
      bool ok[gates::MaxBodies] = {};
      for (int b = 0; b < bodies; ++b)
        ok[b] = BodyOf(component, b, havoks[b], ids[b], roots[b], worldIndex[b]);
      int writer = -1;
      bool clientListed = false;
      for (int b = 0; b < bodies; ++b)
        if (worldIndex[b] == 0)
        {
          clientListed = true;
          if (ok[b] && writer < 0)
            writer = b;
        }
      if (clientListed && writer < 0)
      {
        int unreadable = 0;
        for (int b = 0; b < bodies; ++b)
          unreadable += !ok[b];
        for (int k = 0; k < ShapeCount; ++k)
          if (r.Asked[k])
          {
            a.Unknown[k] += bodies;
            a.NotReadable[k] += unreadable;
            a.NoWriter[k] = true;
          }
        return 1;
      }
      for (int b = 0; b < bodies && writer < 0; ++b)
        if (ok[b])
          writer = b;
      if (writer >= 0 && havoks[writer] != havokWorld)
        return -1;
      bool solidNow = g_SolidStreet.load() && !g_Broken.load();
      signed char states[gates::MaxBodies][ShapeCount] = {};
      bool written[gates::MaxBodies] = {}, looked[gates::MaxBodies] = {}, own[gates::MaxBodies] = {};
      // This world's bodies, then the others
      for (int pass = 0; pass < 2; ++pass)
        for (int b = 0; b < bodies; ++b)
        {
          if (!ok[b] || (havoks[b] == havokWorld) != (pass == 0))
            continue;
          int earlier = -1;
          for (int e = 0; e < bodies && earlier < 0; ++e)
            if (looked[e] && roots[e] == roots[b] && (pass == 1 || havoks[e] == havokWorld))
              earlier = e;
          for (int k = 0; k < ShapeCount; ++k)
          {
            if (!r.Asked[k] || Shapes[k].Member != m)
              continue;
            int state;
            if (earlier >= 0 && !own[earlier])
              state = states[earlier][k];
            else if (pass == 1)
            {
              // (a root of its own in another world: looked at only, for the log)
              own[b] = true;
              states[b][k] = (signed char)(earlier >= 0 ? states[earlier][k] : LookOnly(Shapes[k], roots[b]));
              ++a.Other[k];
              continue;
            }
            else
            {
              Shape const& s = Shapes[k];
              __int64 obj = 0;
              state = ObjectAt(s, roots[b], obj);
              if (state == 0 && !a.HasDiff[k])
              {
                // (for the log: the first byte not as the file has it, at each packfile's distance)
                a.HasDiff[k] = true;
                for (int pf = 0; pf < 2; ++pf)
                {
                  __int64 o = roots[b] + s.FromRoot[pf];
                  a.Diff[k][pf] = (o & 15) ? -1 : Differs(s, o, s.File);
                }
              }
              if (state != 0)
                a.OtherBytes[k] |= OtherBytes(s, obj);
              if ((state == 1 && r.ToNew[k] && solidNow) || (state == 2 && r.ToOld[k]))
              {
                bool make = state == 1;
                Write(s, obj, make);
                state = !Differs(s, obj, make ? s.Tools : s.File) ? (make ? 2 : 1) : 0;
                written[b] = true;
                ++a.Now[k];
              }
            }
            states[b][k] = (signed char)state;
            a.AsFile[k] += state == 1;
            a.AsTools[k] += state == 2;
            a.Unknown[k] += state == 0;
          }
          looked[b] = true;
          if (earlier >= 0)
            written[b] = written[earlier];
        }
      for (int b = 0; b < bodies; ++b)
      {
        if (!ok[b])
        {
          for (int k = 0; k < ShapeCount; ++k)
          {
            a.Unknown[k] += r.Asked[k] && Shapes[k].Member == m;
            a.NotReadable[k] += r.Asked[k] && Shapes[k].Member == m;
          }
          continue;
        }
        if (!written[b])
          continue;
        if (havoks[b] == havokWorld)
        {
          if (Renew(havokWorld, ids[b], roots[b]))
            QueueRenewal(havokWorld, ids[b], roots[b], 1);
        }
        else
          QueueRenewal(havoks[b], ids[b], roots[b], 2);
      }
      return 1;
    }

    // The flush's part: in each flush of the game's physics worlds, before the game's own (hPhysicsFlush). Never waits
    // for Update: if it has the lock, the next flush does it.
    void Flush(__int64 world)
    {
      if (!g_FlushHas.load(std::memory_order_acquire) || !TryAcquireSRWLockExclusive(&g_Lock))
        return;
      __int64 havokWorld = 0;
      __try
      {
        havokWorld = *(__int64 const*)(world + WorldHavokOffset);
      }
      __except (EXCEPTION_EXECUTE_HANDLER) {}
      if (havokWorld >= 0x10000)
      {
        RenewDue(havokWorld);
        for (int m = 0; m < MemberCount; ++m)
        {
          Request& r = g_Request[m];
          if (!r.Posted)
            continue;
          int result = 0;
          Answer a = {};
          __try
          {
            result = FlushMember(m, r, havokWorld, a);
          }
          __except (EXCEPTION_EXECUTE_HANDLER)
          {
            a = {};
            a.Fault = true;
            result = 1;
          }
          if (result != 1)
            continue;
          r.Posted = false;
          a.Done = true;
          a.Seq = r.Seq;
          g_Answer[m] = a;
          // (made now: Street level stays on from here, before Update has the answer)
          for (int k = 0; k < ShapeCount; ++k)
            if (a.AsTools[k] > 0)
              g_SkyHolding = true;
        }
      }
      bool more = g_RenewalCount > 0;
      for (Request const& r : g_Request)
        more |= r.Posted;
      g_FlushHas.store(more, std::memory_order_release);
      ReleaseSRWLockExclusive(&g_Lock);
    }

    bool AnyChanged()
    {
      for (ShapeState const& ss : g_State)
        if (ss.Changed)
          return true;
      return false;
    }

    // A member forgotten (its group gone, or not to be trusted): its shapes' states, its request
    void ForgetMember(int m)
    {
      g_Members[m] = {};
      for (int k = 0; k < ShapeCount; ++k)
        if (Shapes[k].Member == m)
          g_State[k] = {};
      g_Request[m].Posted = false;
      g_Request[m].Seq = ++g_Seq;
      g_LateLogged[m] = false;
    }

    // The flushes' answers taken (under g_Lock)
    void TakeAnswers()
    {
      for (int m = 0; m < MemberCount; ++m)
      {
        Answer& a = g_Answer[m];
        Request const& r = g_Request[m];
        if (!a.Done)
          continue;
        a.Done = false;
        if (a.Seq != r.Seq)
          continue;
        g_LateLogged[m] = false;
        if (a.Fault)
        {
          // (what's changed may still be: held on, looked at again each check, given up as unreadable)
          if (!g_Members[m].FaultLogged)
            util::log::Warning("Street level: a fault looking at %s's collision for the Sky City reshape; looked at again "
              "each check (what it changed stays held till then, or 10 s after Solid street is off)", Members[m].Mesh);
          g_Members[m].FaultLogged = true;
          g_Members[m].SeenNone = true;
          continue;
        }
        g_Members[m].FaultLogged = false;
        if (a.NoComponent)
        {
          g_Members[m].SeenNone = true;
          continue;
        }
        float const* feet = r.HasFeet ? r.Feet : nullptr;
        bool seen = false;
        for (int k = 0; k < ShapeCount; ++k)
        {
          if (!r.Asked[k])
            continue;
          Shape const& s = Shapes[k];
          ShapeState& ss = g_State[k];
          int asFile = a.AsFile[k], asTools = a.AsTools[k], unknown = a.Unknown[k];
          // (none of its bodies' shapes seen: still what it was last seen as, till its member is given up)
          if (asFile || asTools)
          {
            ss.Changed = asTools > 0;
            seen = true;
          }
          if (a.Now[k])
          {
            if (asTools && !asFile && !unknown)
              util::log::Write("Street level: %s changed in the game's collision (%d of its zone's bodies; Faith at %.1f %.1f "
                "%.1f)", s.Name, asTools, feet ? feet[0] : 0.f, feet ? feet[1] : 0.f, feet ? feet[2] : 0.f);
            else if (!asTools && asFile && !unknown)
              util::log::Write("Street level: %s back as the game has it (%d of its zone's bodies)", s.Name, asFile);
            else
              util::log::Write("Street level: %s: %d of its zone's bodies changed now; %d as the tools make it, %d as its "
                "file has it, %d neither", s.Name, a.Now[k], asTools, asFile, unknown);
          }
          // (once, and once more if a later answer first gives the byte: an earlier one may only have had bodies that
          // couldn't be read)
          if (unknown && (!ss.UnknownLogged || (a.HasDiff[k] && !ss.DiffLogged)))
          {
            ss.UnknownLogged = true;
            ss.DiffLogged |= a.HasDiff[k];
            char place[2][32];
            for (int pf = 0; pf < 2; ++pf)
              if (a.Diff[k][pf] < 0)
                sprintf_s(place[pf], "not 16-byte aligned");
              else
                sprintf_s(place[pf], "+ 0x%X", a.Diff[k][pf]);
            char read[96] = "", bytes[128] = "";
            if (a.NotReadable[k] || a.NoWriter[k])
              sprintf_s(read, "; %d couldn't be read%s", a.NotReadable[k], a.NoWriter[k] ? " (the client world's among "
                "them: none looked at)" : "");
            if (a.HasDiff[k])
              sprintf_s(bytes, "; the first byte not as its file has it: %s / %s (the first packfile's place / the "
                "second's)", place[0], place[1]);
            util::log::Write("Street level: %s: %d of its zone's bodies aren't as the tools know them (left as they are)%s%s",
              s.Name, unknown, read, bytes);
          }
          if (a.OtherBytes[k] && !ss.OtherBytesLogged && util::DiagnosticsOn())
          {
            ss.OtherBytesLogged = true;
            char list[300] = "";   // (all 48 of them fit: 6 characters each)
            size_t used = 0;
            for (int at = 0x10; at < DataStart && used + 8 < sizeof(list); ++at)
              if (a.OtherBytes[k] >> (at - 0x10) & 1)
                used += (size_t)sprintf_s(list + used, sizeof(list) - used, " +0x%X", at);
            util::log::Write("Street level (diagnostic): %s: the game has bytes of it the tools don't go by that aren't as "
              "its file has them (left as they are):%s", s.Name, list);
          }
          if (a.Other[k] && !ss.OtherLogged)
          {
            ss.OtherLogged = true;
            util::log::Write("Street level: %s: %d of its zone's bodies are in another world with their own copy of its "
              "collision (left as they are)", s.Name, a.Other[k]);
          }
          bool waiting = (r.Wanted[k] && asFile && !r.ToNew[k]) || (!r.Wanted[k] && asTools && !r.ToOld[k]);
          if (waiting && !ss.WaitLogged)
          {
            ss.WaitLogged = true;
            if (r.Wanted[k])
              util::log::Write("Street level: %s waits for Street level's boxes round Faith (she's in or on the space it "
                "opens, at %.1f %.1f %.1f)", s.Name, feet ? feet[0] : 0.f, feet ? feet[1] : 0.f, feet ? feet[2] : 0.f);
            else
              util::log::Write("Street level: %s comes back once Faith is out of the space it opened (she's at %.1f %.1f "
                "%.1f); Street level stays on till then", s.Name, feet ? feet[0] : 0.f, feet ? feet[1] : 0.f,
                feet ? feet[2] : 0.f);
          }
          else if (!waiting)
            ss.WaitLogged = false;
        }
        g_Members[m].SeenNone = !seen;
      }
    }

    void Update(bool readable, float const* feet)
    {
      if (!readable)
        feet = nullptr;
      bool solid = g_Available && !g_Broken.load() && g_SolidStreet.load() && g_PropsLoaded;
      // (each frame: the flushes' answers, and Street level held on while something's changed)
      AcquireSRWLockExclusive(&g_Lock);
      TakeAnswers();
      bool changed = AnyChanged();
      g_SkyHolding = changed;
      // (Solid street off: a change asked for while it was on and not made yet isn't made)
      if (!solid)
        for (Request& r : g_Request)
          for (int k = 0; k < ShapeCount; ++k)
          {
            r.ToNew[k] = false;
            if (!g_State[k].Changed)
              r.Wanted[k] = false;
          }
      ReleaseSRWLockExclusive(&g_Lock);
      if (!solid && !changed)
      {
        g_WasSolid = false;
        return;
      }
      if (gates::g_Code == 0)
      {
        // (looked at once Solid street is first on, as the gates' code is)
        bool known = false;
        __try
        {
          known = gates::CodeKnown();
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
        gates::g_Code = known ? 1 : -1;
        if (!known)
          util::log::Warning("Street level: the game's code isn't what the tools know: the gates Solid street takes away and "
            "the Sky City station's collision stay as the game has them");
      }
      if (gates::g_Code < 0)
        return;
      ULONGLONG now = GetTickCount64();
      bool toggled = solid != g_WasSolid;
      g_WasSolid = solid;
      if (!toggled && now - g_LastCheck < CheckMs)
        return;
      g_LastCheck = now;
      float distance = 1e30f;
      if (feet)
        distance = sqrtf((feet[0] - Middle[0]) * (feet[0] - Middle[0]) + (feet[2] - Middle[1]) * (feet[2] - Middle[1]));
      bool nearMade = g_PlanAllMade.load() && g_PlanAllMadeSince && now - g_PlanAllMadeSince >= ChangeAfterMs;

      // (the check under g_Lock: an answer can't come between taking the answers and asking again)
      AcquireSRWLockExclusive(&g_Lock);
      TakeAnswers();
      // Its zone's group gone (streamed out): what the tools changed went with it
      for (int m = 0; m < MemberCount; ++m)
      {
        MemberState& ms = g_Members[m];
        if (ms.Ps.Group && !gates::StillPiece(Members[m], ms.Ps))
        {
          for (int k = 0; k < ShapeCount; ++k)
            if (Shapes[k].Member == m)
              util::log::Write("Street level: %s: its zone's model group is gone (streamed out)%s", Shapes[k].Name,
                g_State[k].Changed ? "; it comes back as its file has it" : "");
          ForgetMember(m);
        }
      }
      // Looked for while wanted and she's near
      bool unfound = false;
      for (int k = 0; k < ShapeCount; ++k)
        unfound |= !g_Members[Shapes[k].Member].Ps.Group && solid && g_ImageOk[k] && (g_SkyDataHas & Shapes[k].DataBit);
      if (unfound && distance <= SearchDistance && now - g_LastSearch >= SearchMs)
      {
        g_LastSearch = now;
        gates::Gate gate = {};
        gate.Name = "Sky City";
        gate.Count = MemberCount;
        for (int m = 0; m < MemberCount; ++m)
          gate.Pieces[m] = Members[m];
        gates::GateState found = {};
        for (int m = 0; m < MemberCount; ++m)
          found.Pieces[m].Group = g_Members[m].Ps.Group;      // (those found already aren't looked for)
        bool got[gates::MaxPieces];
        gates::FindPieces(gate, found, got);
        bool all = true;
        for (int m = 0; m < MemberCount; ++m)
        {
          if (got[m])
          {
            ForgetMember(m);
            g_Members[m].Ps = found.Pieces[m];
            util::log::Write("Street level: the Sky City reshape's %s found (group 0x%llX, member %d of %d)", Members[m].Mesh,
              found.Pieces[m].Group, found.Pieces[m].Member, found.Pieces[m].Members);
          }
          all &= g_Members[m].Ps.Group != 0;
        }
        if (all)
          g_NotFoundLogged = false;
        else if (!g_NotFoundLogged && distance <= NotFoundLogDistance)
        {
          g_NotFoundLogged = true;
          util::log::Write("Street level: Faith is %.0f m from the Sky City station and its zone's model group isn't found (not "
            "loaded, or not as the tools know it)", distance);
        }
      }

      for (int m = 0; m < MemberCount; ++m)
      {
        MemberState& ms = g_Members[m];
        if (!ms.Ps.Group)
          continue;
        Request r = {};
        bool any = false, held = false;
        for (int k = 0; k < ShapeCount; ++k)
        {
          if (Shapes[k].Member != m)
            continue;
          r.Wanted[k] = solid && g_ImageOk[k] && (g_SkyDataHas & Shapes[k].DataBit);
          r.Asked[k] = r.Wanted[k] || g_State[k].Changed;
          any |= r.Asked[k];
          held |= g_State[k].Changed;
        }
        if (!any)
          continue;
        bool readableNow = gates::Component(ms.Ps) != 0;
        if (!readableNow || ms.SeenNone)
        {
          // (its physics or its bodies' shapes not readable while something's changed and Solid street is off: not held
          // on for ever)
          if (held && !solid)
          {
            if (!ms.Unreadable)
              ms.Unreadable = now;
            else if (now - ms.Unreadable >= GiveUpMs)
            {
              util::log::Warning("Street level: %s's physics can't be read to put the Sky City reshape back (%.0f s); left, "
                "and Street level goes off", Members[m].Mesh, GiveUpMs / 1000.0);
              ForgetMember(m);
              continue;
            }
          }
        }
        else
          ms.Unreadable = 0;
        if (!readableNow)
          continue;
        // Changed with her there, out of the space it opens or the boxes round her made; back with her out of it, on or
        // over a top of it, or not there; or at once if Street level's bodies are off for good (a fault: g_Broken), as
        // nothing can stand in for it then
        for (int k = 0; k < ShapeCount; ++k)
        {
          if (!r.Asked[k])
            continue;
          bool in = false, onTop = false;
          if (feet)
            Where(Shapes[k], feet, in, onTop);
          r.ToNew[k] = r.Wanted[k] && feet && (!in || nearMade);
          r.ToOld[k] = !r.Wanted[k] && (g_Broken.load() || !feet || !in || onTop);
        }
        r.Ps = ms.Ps;
        r.HasFeet = feet != nullptr;
        if (feet)
          memcpy(r.Feet, feet, sizeof(r.Feet));
        Request& posted = g_Request[m];
        r.At = posted.Posted ? posted.At : now;
        if (posted.Posted && now - posted.At >= FlushLateMs && !g_LateLogged[m])
        {
          g_LateLogged[m] = true;
          util::log::Write("Street level: the Sky City reshape asked of %s's collision isn't done: no flush of its bodies' "
            "world in %.0f s", Members[m].Mesh, FlushLateMs / 1000.0);
        }
        r.Posted = true;
        r.Seq = ++g_Seq;
        posted = r;
        g_FlushHas.store(true, std::memory_order_release);
      }
      g_SkyHolding = AnyChanged();
      ReleaseSRWLockExclusive(&g_Lock);
    }
  }
}

void ground::Init()
{
  if (!CodeKnown())
  {
    util::log::Warning("Street level: the game's code isn't what the tools know, GAME > Faith in the streets is off");
    return;
  }
  if (!g_Roads.Load())
  {
    util::log::Warning("Street level: the road surface data (Resources\\GroundRoads.bin) couldn't be read, GAME > Street "
      "level is off");
    return;
  }
  g_PropsLoaded = g_Props.Load();
  if (!g_PropsLoaded)
  {
    g_Props = Props();
    util::log::Warning("Street level: the props data (Resources\\GroundProps.ctpk) couldn't be read: no solid props");
  }
  else
  {
    g_SkyDataHas = skycity::DataHas(g_Props);
    util::log::Write("Street level: the props data %s the stand-ins for Sky City's base block (and the station hall's low "
      "ceiling over its east end) and %s the Sky City station's north wedge's: %s", (g_SkyDataHas & 1) ? "has" : "hasn't",
      (g_SkyDataHas & 2) ? "has" : "hasn't", g_SkyDataHas == 3 ? "both are reshaped while Solid street is on" :
      g_SkyDataHas ? "only that one is reshaped" : "neither is reshaped");
  }
  skycity::CheckImages();
  g_Shapes.assign(TileShapeCount + g_Props.Halves.size() / 3, 0);
  __int64 base = Base();
  bool hooked = util::hooks::CreateGameHook("KillVolumeInside", base + KillInsideRva, hKillInside, (LPVOID*)&oKillInside);
  hooked = util::hooks::CreateGameHook("FallCheckRun", base + FallRunRva, hFallRun, (LPVOID*)&oFallRun) && hooked;
  hooked = util::hooks::CreateGameHook("HitBodyIds", base + HitBodyRva, hHitBody, (LPVOID*)&oHitBody) && hooked;
  hooked = util::hooks::CreateGameHook("PhysicsFlush", base + PhysicsFlushRva, hPhysicsFlush, (LPVOID*)&oPhysicsFlush) &&
    hooked;
  g_Available = hooked;
  if (hooked)
    util::log::Write("Street level: ready (%u road triangles, %d x %d cells of %.0f m, %d dense ones with a grid of %.0f m; "
      "%u prop boxes of %u sizes, %d mirrored, %d left out; a grid of %d x %d cells of %.0f m, %u wide boxes)",
      (unsigned)(g_Roads.Triangles.size() / 3), g_Roads.CellsX, g_Roads.CellsZ, Roads::Cell, g_Roads.SubGrids,
      Roads::SubCell, (unsigned)g_Props.Boxes.size(), (unsigned)(g_Props.Halves.size() / 3), g_Props.Mirrored,
      g_Props.Skipped, g_Props.GridNX, g_Props.GridNZ, g_Props.CellStep, (unsigned)g_Props.Wide.size());
  else
    util::log::Warning("Street level: not all its hooks are in, GAME > Faith in the streets is off");
}

bool ground::IsAvailable()
{
  return g_Available;
}

void ground::SetSolidStreet(bool on)
{
  if (g_SolidStreet.exchange(on) != on)
    util::log::Write("Street level: solid street %s", on ? "on" : "off");
}

void ground::SetNoFallDeath(bool on)
{
  if (g_NoFallDeath.exchange(on) != on)
    util::log::Write("Street level: no fall death %s", on ? "on" : "off");
}

void ground::Update(bool hasFaith, float const* feet)
{
  // Falling in the void: under VoidY and going down
  static float s_prevY = 0.f;
  static bool s_hadFeet = false;
  bool inVoid = false;
  float fall = 0.f;             // how far her feet went down since the frame before
  if (hasFaith && feet)
  {
    inVoid = feet[1] < VoidY && feet[1] < s_prevY - 0.005f;
    if (s_hadFeet)
      fall = (std::min)((std::max)(s_prevY - feet[1], 0.f), 1.f);
    s_prevY = feet[1];
  }
  s_hadFeet = hasFaith && feet;
  // In the Sky City station's sunken part (StationPit; no fall death there with Solid street on)
  {
    bool inPit = false;
    if (hasFaith && feet)
    {
      float dx = feet[0] - StationOrigin[0], dz = feet[2] - StationOrigin[1];
      float u = dx * StationRight[0] + dz * StationRight[1], v = dz * StationRight[0] - dx * StationRight[1];
      inPit = u >= StationPit[0] && u <= StationPit[2] && v >= StationPit[1] && v <= StationPit[3] &&
        feet[1] >= StationPitY0 && feet[1] <= StationPitY1;
    }
    g_InStationPit = inPit;
  }
  if (g_InVoid.exchange(inVoid) != inVoid)
    util::log::Write(inVoid ? "Street level: Faith is falling below y %.0f: the game's fall check works as it has it" :
      "Street level: Faith is out of the void (y %.0f): no fall death again if ticked", VoidY);
  if (!g_Available)
  {
    SetStatus("Not available: the game's code isn't what the tools know (see CT.log)", false);
    return;
  }
  if (g_Broken.load())
  {
    WantNone("Off: a call into the game's physics faulted (see CT.log); restart the game to try again");
    return;
  }
  // (off, unless what the Sky City reshape changed is still changed: Faith is in the space it opened)
  // (Solid street read once: the menu may switch it between two reads)
  bool solidStreet = g_SolidStreet.load();
  bool holding = !solidStreet && g_SkyHolding.load();
  if (!solidStreet && !holding)
  {
    WantNone("Off");
    return;
  }
  ULONGLONG now = GetTickCount64();
  static ULONGLONG s_lastFaith = 0;
  if (!hasFaith || !feet)
  {
    // A frame or two without her (her transform not readable) leaves the ground as it is: taking it away would
    // drop her if she's on it
    if (g_PlanOut && now - s_lastFaith < FaithGoneMs)
      SetStatus("Faith isn't there for a moment: the ground stays as it is", false);
    else
      WantNone("Faith isn't there (a load or a respawn)");
    return;
  }
  s_lastFaith = now;
  // Over the street: the level's kill zones spare her (hKillInside)
  {
    float h = 0.f, n[3];
    g_OverStreet = g_Roads.Height(feet[0], feet[2], feet[1] + 1.f, h, n) && feet[1] - h <= OverStreetMax;
    g_AtSkyCity = feet[0] >= SkyCityArea[0] && feet[2] >= SkyCityArea[1] && feet[0] <= SkyCityArea[2] &&
      feet[2] <= SkyCityArea[3] && feet[1] >= SkyCityY0 && feet[1] <= SkyCityY1;
  }

  // The ids the bodies take: a real static body near her (its area stays loaded while she's there)
  if (g_Donor.Body >= 0 && !DonorStillThere(g_Donor))
  {
    util::log::Write("Street level: body %d (its ids lent to the ground) changed or went; looking for another",
      g_Donor.Body);
    g_Donor = Donor();
  }
  if (g_Donor.Body < 0 || now - g_LastProbe > ProbeEveryMs)
  {
    g_LastProbe = now;
    Donor found;
    bool lent = Probe(feet, found);
    // Nothing near can lend its ids (all the bodies hit have other records, as in The View): any static body of the
    // world can, while its record lives (the flush checks it each time)
    if (!lent && g_Donor.Body < 0 && g_WorldSeen)
      lent = ScanForDonor(g_WorldSeen, -1, found);
    if (lent)
    {
      if (util::DiagnosticsOn() && (found.Body != g_Donor.Body || found.World != g_Donor.World))
        util::log::Write("Street level: the ground takes the ids of body %d (record 0x%I64X, user data 0x%I64X, filter "
          "0x%X, material %u)", found.Body, found.Record, found.UserData, found.Filter, found.Material);
      g_Donor = found;
    }
  }
  if (g_Donor.Body < 0)
  {
    WantNone("Waiting for a solid surface near Faith to lend the ground its ids (none hit yet)");
    return;
  }

  // What's wanted round her: the ground tiles, then the props, nearest first
  // The groups (see GroupBaubleRuins), looked at once a second
  {
    static ULONGLONG s_groupsAt = 0;
    if (now - s_groupsAt >= 1000)
    {
      s_groupsAt = now;
      bool ruins = util::hooks::BaubleRuinsTrigger() == 1;
      if (ruins != g_GroupOn[GroupBaubleRuins])
      {
        g_GroupOn[GroupBaubleRuins] = ruins;
        g_PropCache.Valid = false;
        util::log::Write("Street level: the Bauble's ruins %s", ruins ? "are the version streamed (their trigger on): "
          "they and their rubble are made solid" : "aren't streamed (their trigger off): no boxes for them");
      }
      bool mall = util::hooks::BaubleMallTrigger() == 1;
      if (mall != g_GroupOn[GroupBaubleMall])
      {
        g_GroupOn[GroupBaubleMall] = mall;
        g_PropCache.Valid = false;
        util::log::Write("Street level: the whole Bauble mall %s", mall ? "is the version streamed (its trigger on): "
          "its roofs are made solid" : "isn't streamed (its trigger off): no boxes for its roofs");
      }
    }
  }
  // The planning's parts (2026-11-03, "where the time goes"): the tiles, the props, putting the items nearest first,
  // their keys sorted, compared with the last plan and handed over
  enum PlanPart { PartTiles, PartProps, PartOrder, PartKeys, PartCompare, PlanParts };
  static long long s_partTicks[PlanParts] = {};
  long long planStart = Ticks(), lap = planStart;
  auto planLap = [&](int part)
  {
    long long t = Ticks();
    s_partTicks[part] += t - lap;
    lap = t;
  };
  std::shared_ptr<std::vector<unsigned __int64> const> madeKeys = TakeMade();
  static const std::vector<unsigned __int64> s_none;
  std::vector<unsigned __int64> const& made = madeKeys ? *madeKeys : s_none;
  // The last plan again when nothing it's made from has changed (PlanMemo), else a new one
  PlanMemo& memo = g_PlanMemo;
  bool reused = memo.Valid && now - memo.At < PlanMemoMs && !memcmp(memo.Feet, feet, sizeof(memo.Feet)) &&
    !memcmp(&memo.Fall, &fall, sizeof(fall)) && memo.Made == madeKeys && memo.Lowered == g_PlannedLowered &&
    !memcmp(memo.GroupOn, g_GroupOn, sizeof(g_GroupOn)) && SameDonor(memo.From, g_Donor) &&
    (!g_PropsLoaded || g_PropCache.Valid);
  std::shared_ptr<Plan const> plan;
  PlanNotes notes;
  if (reused)
  {
    plan = memo.Last;
    notes = memo.Notes;
  }
  else
  {
    static SortWork s_work;     // (Update's thread only)
    int letGoLogged = g_LetGoLogged, loweredLogged = g_LoweredLogged;
    auto fresh = std::make_shared<Plan>();
    fresh->From = g_Donor;
    fresh->Items.reserve(memo.Last ? memo.Last->Items.size() : 0);   // (room for as many as the last: grown once)
    PlanTiles(feet, fall, made, fresh->Items, notes);
    planLap(PartTiles);
    if (g_PropsLoaded)
      PlanProps(feet, made, fresh->Items, notes);
    planLap(PartProps);
    NearestFirst(fresh->Items, s_work);
    planLap(PartOrder);
    {
      size_t n = fresh->Items.size();
      static std::vector<unsigned __int64> s_keys;
      std::vector<unsigned __int64>& keys = s_keys;
      keys.resize(n);
      for (size_t i = 0; i < n; ++i)
        keys[i] = fresh->Items[i].Key;
      KeyOrder(keys.data(), n, fresh->SortedItems, s_work);
      fresh->SortedKeys.resize(n);
      for (size_t i = 0; i < n; ++i)
        fresh->SortedKeys[i] = keys[fresh->SortedItems[i]];
    }
    planLap(PartKeys);
    // (what it was made from: nothing above changes any of it)
    memo.Valid = g_LetGoLogged == letGoLogged && g_LoweredLogged == loweredLogged;
    memo.At = now;
    memcpy(memo.Feet, feet, sizeof(memo.Feet));
    memo.Fall = fall;
    memo.Made = madeKeys;
    memo.Lowered = g_PlannedLowered;
    memcpy(memo.GroupOn, g_GroupOn, sizeof(g_GroupOn));
    memo.From = g_Donor;
    memo.Last = fresh;
    memo.Notes = notes;
    plan = std::move(fresh);
  }
  // One to make that isn't made: the plan's sorted keys gone through alongside the made ones
  bool toMake = false;
  for (size_t i = 0, j = 0; i < plan->SortedKeys.size() && !toMake; ++i)
  {
    unsigned __int64 key = plan->SortedKeys[i];
    while (j < made.size() && made[j] < key)
      ++j;
    toMake = (j == made.size() || made[j] != key) && plan->Items[plan->SortedItems[i]].Make;
  }
  // (what's near her made: every item of the plan within NearMadeRadius of her that may be made is; for the Sky City
  // reshape, which takes the game's collision from under her only once its stand-ins round her are there)
  bool nearMade = g_PlanOut;
  for (size_t i = 0; i < plan->Items.size() && nearMade; ++i)
  {
    Item const& it = plan->Items[i];
    if (it.Distance > NearMadeRadius)
      break;
    nearMade = !it.Make || std::binary_search(made.begin(), made.end(), it.Key);
  }
  bool differs = !g_PlanOut || plan->SortedKeys != g_PlannedKeys || g_PlannedFrom.Body != g_Donor.Body ||
    g_PlannedFrom.World != g_Donor.World || g_PlannedFrom.UserData != g_Donor.UserData;
  if (differs || (toMake && now - g_LastPublish >= RepublishMs))
  {
    g_PlannedKeys = plan->SortedKeys;
    g_PlannedFrom = g_Donor;
    NotePlannedLowered(plan->Items);
    g_PlanOut = true;
    g_LastPublish = now;
    PublishPlan(std::move(plan));
  }
  if (nearMade && !g_PlanAllMade.load())
    g_PlanAllMadeSince = now;
  else if (!nearMade)
    g_PlanAllMadeSince = 0;
  g_PlanAllMade = nearMade;
  planLap(PartCompare);
  if (util::DiagnosticsOn())
  {
    // The timing (see TimingMs): this frame's planning, and every TimingMs the line (diagnostics only)
    static long long s_planTicks = 0, s_planMost = 0;
    static unsigned s_frames = 0, s_reused = 0, s_gathers = 0, s_searches = 0;
    static ULONGLONG s_since = 0;
    long long spent = Ticks() - planStart;
    s_planTicks += spent;
    s_planMost = (std::max)(s_planMost, spent);
    ++s_frames;
    if (reused)
      ++s_reused;
    if (!s_since)
    {
      s_since = now;
      s_gathers = g_PropGathers;
      s_searches = g_PropSearches;
    }
    else if (now - s_since >= TimingMs)
    {
      LARGE_INTEGER f;
      QueryPerformanceFrequency(&f);
      double perMs = (double)f.QuadPart / 1000.0;
      long long flush = g_FlushTicks.exchange(0), flushMost = g_FlushMaxTicks.exchange(0);
      unsigned flushes = g_FlushCount.exchange(0), madeN = g_BodiesMade.exchange(0), gone = g_BodiesDestroyed.exchange(0);
      unsigned ofBodies = g_FlushesOfBodies.exchange(0);
      double perFrame = perMs * s_frames, perFlush = perMs * (std::max)(ofBodies, 1u);
      long long fp[FlushParts];
      for (int p = 0; p < FlushParts; ++p)
        fp[p] = g_FlushPartTicks[p].exchange(0);
      util::log::Write("Street level (performance, diagnostic): %u frames in %.0f s, %u of them taking the last plan "
        "again (nothing it's made from had changed). Planning %.3f ms a frame (most %.2f; "
        "the tiles %.3f, the props %.3f, nearest first %.3f, the keys sorted %.3f, compared and handed over %.3f; the "
        "props round her gathered %u times, searched whole %u times). The physics flush's part %.3f ms a flush (most "
        "%.2f, %u flushes, %u of them the bodies' world's: in each of those checking the bodies %.3f, destroying %.3f, "
        "making %.3f, after the game's work %.3f); bodies made %u, destroyed %u; %d bodies now", s_frames,
        (now - s_since) / 1000.0, s_reused, s_planTicks / perMs / s_frames, s_planMost / perMs,
        s_partTicks[PartTiles] / perFrame,
        s_partTicks[PartProps] / perFrame, s_partTicks[PartOrder] / perFrame, s_partTicks[PartKeys] / perFrame,
        s_partTicks[PartCompare] / perFrame, g_PropGathers - s_gathers, g_PropSearches - s_searches,
        flushes ? flush / perMs / flushes : 0.0, flushMost / perMs, flushes, ofBodies, fp[PartCheck] / perFlush,
        fp[PartDrop] / perFlush, fp[PartMake] / perFlush, fp[PartAfter] / perFlush, madeN, gone, g_BodyCount.load());
      s_planTicks = s_planMost = 0;
      for (long long& p : s_partTicks)
        p = 0;
      s_frames = s_reused = 0;
      s_since = now;
      s_gathers = g_PropGathers;
      s_searches = g_PropSearches;
    }
  }

  char text[192];
  char under[64];
  if (notes.Under)
    sprintf_s(under, "a tile at y %.2f", notes.UnderTop);
  else if (notes.OverHer)
    sprintf_s(under, "the road is over her feet (its tile waits for her)");
  else
    sprintf_s(under, "no road known");
  if (holding)
    sprintf_s(text, "Going off once Faith is out of the Sky City station's space the tools opened (its collision comes "
      "back then); %d tiles and %d props round her", notes.Tiles, notes.Props);
  else
    sprintf_s(text, "%d ground tiles and %d props round Faith%s, %d bodies in the game's physics. Under her: %s",
      notes.Tiles, notes.Props, notes.PropsCapped ? " (the nearest)" : "", g_BodyCount.load(), under);
  SetStatus(text, false);
}

void ground::UpdateSkyCity(bool hasFaith, float const* feet)
{
  if (!g_Available)
    return;
  skycity::Update(hasFaith, feet);
}

bool ground::SkyCityHolds()
{
  return g_SkyHolding.load();
}

void ground::UpdateGates(bool hasFaith, float const* feet)
{
  if (hasFaith && feet)
  {
    AcquireSRWLockExclusive(&g_FeetLock);
    memcpy(g_LastFeet, feet, sizeof g_LastFeet);
    g_LastFeetAt = GetTickCount64();
    ReleaseSRWLockExclusive(&g_FeetLock);
  }
  if (!hasFaith || !feet)
    return;
  bool wanted = g_Available && !g_Broken.load() && g_SolidStreet.load();
  if (gates::g_Code == 0)
  {
    // (looked at once Solid street is first on: until then nothing's taken away)
    if (!wanted)
      return;
    bool known = false;
    __try
    {
      known = gates::CodeKnown();
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
    gates::g_Code = known ? 1 : -1;
    if (!known)
      util::log::Warning("Street level: the game's code isn't what the tools know: the gates Solid street takes away stay "
        "as the game has them");
  }
  if (gates::g_Code < 0)
    return;
  ULONGLONG now = GetTickCount64();
  for (int g = 0; g < gates::GateCount; ++g)
    gates::UpdateGate(gates::Gates[g], gates::g_Gates[g], wanted, feet, now);
}

const char* ground::GetStatus(char* buffer, size_t size)
{
  AcquireSRWLockShared(&g_StatusLock);
  strncpy_s(buffer, size, g_Status, _TRUNCATE);
  ReleaseSRWLockShared(&g_StatusLock);
  return buffer;
}

bool ground::RoadHeightUnder(float x, float z, float top, float& height)
{
  float normal[3];
  return g_Roads.Height(x, z, top, height, normal);   // (only read after Init)
}

bool ground::LastFeet(float* feet)
{
  AcquireSRWLockShared(&g_FeetLock);
  bool fresh = g_LastFeetAt != 0 && GetTickCount64() - g_LastFeetAt < 1000;
  if (fresh)
    memcpy(feet, g_LastFeet, sizeof g_LastFeet);
  ReleaseSRWLockShared(&g_FeetLock);
  return fresh;
}
