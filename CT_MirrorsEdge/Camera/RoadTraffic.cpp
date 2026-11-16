#include "RoadTraffic.h"
#include "RoadTrafficCore.h"
#include "WalkersCore.h"
#include "GroundLevel.h"
#include "../Main.h"
#include "../Util/Util.h"
#include "../Util/Perf.h"
#include "../Util/ImGuiEXT.h"
#include "../imgui/imgui.h"
#include <Windows.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <new>
#include <string>
#include <vector>

// Road traffic (2026-11-03; Research_Road_Traffic_2026-11-03.md). The city's road cars are members of one swarm
// (levels/sp/sp_maincity/crowds/cars/carcrowd_01), driven by 21 car placements (CrowdPlacementBoidsEntity), each
// with a Car behaviour object (vtable 0x141CCEF20, at placement + 0xA8) whose step 0x143B8B0D0(behaviour, params)
// moves all of the placement's cars once an update: params + 0 its node list, + 8 its CrowdBoidsSettings, + 0x10 the
// step's seconds. The game's step chases each car's next corner at a capped turn rate and only avoids the cars it
// finds on its own two nodes (none on the highway, whose settings have AvoidanceEnabled false), and its spawn
// (0x143B99C40) puts a respawned car on its spawn node whether or not one is there; the research has why cars leave
// the road and end up inside each other.
//
// What the tools do, in the game's own step (no pass of their own, no menu entry):
// - the game's step runs with a copy of the placement's settings with AvoidanceEnabled cleared (the rules keep the
//   gaps): params + 8 points at the copy for the call and back after; the settings asset itself isn't written;
// - before it, the rules note each car; after it, they move each car along its lane by at most what the game moved
//   it, keeping its gap to the cars in its way, and make the ones that would show inside another unseen
//   (RoadTrafficCore.h); a car the game has just respawned starts unseen and shows where there's room;
// - an unseen car is at y -10000: under the world, but above the game's despawn height -10042, so the game keeps it in
//   its node lists as it does a shown car (RoadTrafficCore.h, HiddenY: at -10042 the lists broke and the game crashed,
//   7BFAC1779936). The game updates a swarm only while it's needed, going by its placements' boxes [inferred]: the
//   11413E1A2147 build hid every car as the game first spread them (a spawn hook, gone now), the boxes ended 10 km under
//   the city, the car swarm was never updated and no car showed (2026-11-03). Nothing is hidden before the rules have
//   stepped a placement but a car inside another, and whenever the rules can't step one its unseen cars are put back.
// The step's caller (0x143B9B7E0) builds params on its stack: + 0 placement + 0x78 (the node vector), + 8 the settings
// ([[placement + 0x30] + 0x80]), + 0x10 the seconds (0 < dt <= 0.3: 0x143B92E00 clamps it, 0x143B9BC10 skips 0).
// The member records: the swarm's data store (placement + 0x98) column of key [0x141CCD778] (0x24 bytes a car, the
// placement's first at index [placement + 0xA0]), the flags column of key [0x141CCD780] (2 bytes a car, bit 3
// despawned); their count at placement + 0x48 (what the step gets from its + 0x38 part's slot 4); the nodes at
// [placement + 0x78] .. [placement + 0x80], 0x30 bytes each.
//
// Walking people and the time of day (2026-11-03, the research's section 8):
// - the walkers' step (the Pedestrians behaviour, vtable 0x141CCEED8: 0x143B8CF00(behaviour, params), params as the
//   cars') with the rules of WalkersCore.h round it: the unseen walkers marked despawned for the step only (the step
//   skips them), then after it each shown walker's move cut short or turned into a sidestep so it never ends inside
//   another; boats use the same behaviour and are left alone (their MemberRadius 12, a walker's 0.2 - 0.5);
// - the standing crowds (the Idle behaviour, vtable 0x141CCEEA0, no step) through the placement's per-frame update
//   0x143B9B7E0(placement, dt, time, ...), after the game's: only the time of day's share;
// - the time of day's share (DayShare): the game's clock ([[[OFFSET_TIMEOFDAY] + 8] + 0x28] + 0x30, seconds, as the
//   tools' time of day menu reads it); all of a crowd shows by day, CarsAtNight / PeopleAtNight of it at night. What
//   goes or comes for it does so out of sight of the view the game's walker step goes by (0x143B8CFE5: 0x143278E10 on
//   [0x142415E50], ids 0, 0; its translation and forward rows), read here without the game's lock (a frame off at worst,
//   and only for what's out of sight). The game's own respawn is held back to the share too: it brings a despawned
//   member back only while more than the despawner's ([behaviour + 0x18]) + 0x34 are despawned (0x143B9CC31), which
//   the update sets before the step from TargetSpawnedPercentage (0x143B9B8D1); the rules raise it, never lower it.
//   With no view known there's no share (all shows).
//
// More by day (2026-11-03, the research's sections 9 and 10): the road cars' crowd gets 2000 cars (the level's 1210;
// the highway and placement 93 keep theirs, see Cars) and the three big walking crowds (the pedestrians', the mid
// caste's and the high caste's) twice the members the level gives them. A crowd's room is its SwarmEntityData's MaxAgentCount, which sizes every per-member column when the
// crowd is made (its components' constructors, the renderer's buffers) and caps its placements when they register
// (0x143B97430: one that doesn't fit gets no members); a placement's members are its data's MaxCrowdMembers. Both are
// raised in the level data as the game makes each entity, in its factory (0x143B880B0 the crowd's, 0x143B8DE90 a
// placement's: the data at the creation info's + 0xD0), before anything reads them, so the game sizes everything for
// the raised counts itself. The crowds are matched by their room (1300, 901, 800, 721: no other crowd has these), the
// placements by their settings asset's name; every other crowd stays as the level has it. At night the raised crowds'
// share is smaller by as much, so nights stay as they were.
//
// The crowd test (MISC > Performance, 2026-11-03; the weather session for the user, who asked whether the many cars
// and people make the game lag: what twice the crowds cost the frame, measured): three phases in one place, as set,
// then the level's crowds, then as set again, each 20 s measured after 5 s to settle (the third 10 s). In the second, each raised
// placement's share is its level count (the extra cars and walkers go unseen as the time of day's share would make
// them), in sight too; in the third they show again where there's room, in sight too. The frame times come from the
// performance test's own (perf::FrameStart); the crowds' CPU from the hooks here: the game's per-frame update of every
// placement (0x143B9B7E0: its respawns, its step and the rules in it, its render copy), and inside it the game's own car
// and walker steps and the rules. Not taken out in the second phase: the room (the crowds' columns stay sized for the
// raised counts) and the game's car step still moving the unseen cars (its time is in the CPU numbers).
namespace
{
  using namespace road_traffic_core;
  namespace wk = walkers_core;

  const __int64 CarStepRva = 0x3B8B0D0;      // the Car behaviour's step
  const __int64 CarVtableRva = 0x1CCEF20;    // the Car behaviour's vtable
  const __int64 PlacementPartVtableRva = 0x1CCE6F0;   // a CrowdPlacementBoidsEntity's + 0x38 part (its constructor 0x143B8ED81)
  const __int64 RecordsKeyRva = 0x1CCD778;   // the data store keys (globals holding them)
  const __int64 FlagsKeyRva = 0x1CCD780;

  const BYTE CarStepBytes[] = { 0x48, 0x89, 0x54, 0x24, 0x10, 0x48, 0x89, 0x4C, 0x24, 0x08, 0x55, 0x53, 0x56, 0x57, 0x41,
    0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8D, 0xAC, 0x24, 0x58, 0xFF, 0xFF, 0xFF, 0x48, 0x81 };

  const int SettingsSize = 0x58;             // CrowdBoidsSettings
  const int SettingsAvoidance = 0x50;        // its AvoidanceEnabled
  const int SettingsBehaviour = 0x18;        // 1 Cars
  const int SettingsSpeed = 0x1C;            // SpeedRange (min, max)
  const int SettingsRadius = 0x24;           // MemberRadius

  const __int64 WalkerStepRva = 0x3B8CF00;   // the Pedestrians behaviour's step
  const __int64 WalkerVtableRva = 0x1CCEED8; // the Pedestrians behaviour's vtable
  const __int64 IdleVtableRva = 0x1CCEEA0;   // the Idle behaviour's
  const __int64 UpdateRva = 0x3B9B7E0;       // a placement's per-frame update
  const __int64 ViewOwnerRva = 0x2415E50;    // the global the walker step's view lookup is given
  const __int64 ViewCallRva = 0x3B8CFE5;     // that call in the walker step
  const __int64 ViewLookupRva = 0x3278E10;   // the lookup (an entry with ids 0, 0)
  const __int64 ViewCopyRva = 0x3278E5E;     // its copy of the entry's rows
  const __int64 RespawnMinRva = 0x3B9CC25;   // the respawn's test of the despawner's + 0x34
  const __int64 MinimumWriteRva = 0x3B9B8B8; // the update setting it from TargetSpawnedPercentage

  const BYTE WalkerStepBytes[] = { 0x48, 0x89, 0x54, 0x24, 0x10, 0x55, 0x53, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56,
    0x48, 0x8D, 0x6C, 0x24, 0x98, 0x48, 0x81, 0xEC, 0x68, 0x01, 0x00, 0x00, 0x49, 0x89, 0xCD, 0x48, 0x8B, 0x49 };
  const BYTE UpdateBytes[] = { 0x4C, 0x89, 0x4C, 0x24, 0x20, 0x53, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x57, 0x48,
    0x81, 0xEC, 0x98, 0x00, 0x00, 0x00, 0x48, 0x8B, 0x41, 0x30, 0x4C, 0x8D, 0x69, 0x78, 0x48, 0x89, 0xCB };
  // mov rcx, [0x142415E50]; mov [rbp + 0xA0], al; call 0x143278E10
  const BYTE ViewCallBytes[] = { 0x48, 0x8B, 0x0D, 0x64, 0x8E, 0x88, 0xFE, 0x88, 0x85, 0xA0, 0x00, 0x00, 0x00, 0xE8, 0x19,
    0xBE, 0x6E, 0xFF };
  // the entries at + 8 .. + 0x10, an entry's ids at + 0x14 and + 0x10
  const BYTE ViewLookupBytes[] = { 0x48, 0x8B, 0x41, 0x08, 0x4C, 0x8B, 0x51, 0x10, 0x4C, 0x39, 0xD0, 0x74, 0x18, 0x48,
    0x8B, 0x08, 0x44, 0x39, 0x41, 0x14, 0x75, 0x06, 0x44, 0x39, 0x49, 0x10, 0x74, 0x0B, 0x48, 0x83, 0xC0, 0x08, 0x4C, 0x39,
    0xD0, 0x75, 0xE8, 0x31, 0xC9 };
  // [entry + 0x208]: four rows from + 0x50, the last (translation) used if it isn't ~0 (else the entry's own at + 0xD0)
  const BYTE ViewCopyBytes[] = { 0x48, 0x8B, 0x83, 0x08, 0x02, 0x00, 0x00, 0x48, 0x85, 0xC0, 0x74, 0x46, 0x0F, 0x28, 0x40,
    0x50, 0x0F, 0x29, 0x07, 0x0F, 0x28, 0x48, 0x60, 0x0F, 0x29, 0x4F, 0x10, 0x0F, 0x28, 0x40, 0x70, 0x0F, 0x29, 0x47, 0x20,
    0x0F, 0x28, 0x88, 0x80, 0x00, 0x00, 0x00, 0x0F, 0x28, 0xC1, 0x0F, 0x29, 0x4F, 0x30 };
  // the despawned pool's size ([+ 0x48] - [+ 0x40]) / 4 against + 0x34, no respawn at or under it
  const BYTE RespawnMinBytes[] = { 0x48, 0x8B, 0x41, 0x48, 0x48, 0x2B, 0x41, 0x40, 0x48, 0xC1, 0xF8, 0x02, 0x3B, 0x41,
    0x34, 0x0F, 0x86 };
  // (100 - TargetSpawnedPercentage) / 100 x the placement's count, to + 0x34
  const BYTE MinimumWriteBytes[] = { 0x8B, 0x43, 0x48, 0x0F, 0x57, 0xC0, 0xF3, 0x48, 0x0F, 0x2A, 0xC0, 0xF3, 0x0F, 0x59,
    0xC1, 0xE8, 0x24, 0x73, 0x46, 0xFF, 0xF3, 0x48, 0x0F, 0x2C, 0xC0, 0x89, 0x47, 0x34 };

  const float CarsAtNight = 0.3f;            // the share of the road cars shown at night (DayShare), of the level's count
  const float PeopleAtNight = 0.25f;         // of the walking and standing people

  // More by day: the factories, the data they're given, and what's raised
  const __int64 SwarmCreateRva = 0x3B880B0;      // the SwarmEntity factory (self, creation info, arena)
  const __int64 PlacementCreateRva = 0x3B8DE90;  // the CrowdPlacementBoidsEntity factory (creation info)
  const __int64 SwarmDataLoadRva = 0x3B880EF;    // the first's  mov r8, [rbx + 0xD0]  (the data, for the constructor)
  const __int64 PlacementDataLoadRva = 0x3B8DEE4;   // the second's
  const __int64 SwarmCapGetterRva = 0x3B88D40;   // the game's MaxAgentCount getter (its six callers size the crowd)
  const __int64 PlacementCountGetterRva = 0x3B94B10;   // a placement's members (its vtable's + 0xD8)
  const __int64 SwarmDataVtableRva = 0x1CD2C30;  // SwarmEntityData (its destructor 0x143BB301A)
  const __int64 PlacementDataVtableRva = 0x1CD2DE0;   // CrowdPlacementBoidsEntityData (0x143BB17B9)
  const __int64 SettingsVtableRva = 0x1CD2CA8;   // CrowdBoidsSettings (0x143BB2BBA)
  const BYTE SwarmCreateBytes[] = { 0x4C, 0x89, 0x44, 0x24, 0x18, 0x57, 0x48, 0x83, 0xEC, 0x30, 0x48, 0xC7, 0x44, 0x24,
    0x20, 0xFE, 0xFF, 0xFF, 0xFF, 0x48, 0x89, 0x5C, 0x24, 0x40 };
  const BYTE PlacementCreateBytes[] = { 0x57, 0x48, 0x83, 0xEC, 0x30, 0x48, 0xC7, 0x44, 0x24, 0x20, 0xFE, 0xFF, 0xFF,
    0xFF, 0x48, 0x89, 0x5C, 0x24, 0x50, 0x48, 0x89, 0x74, 0x24, 0x58 };
  const BYTE DataLoadBytes[] = { 0x4C, 0x8B, 0x83, 0xD0, 0x00, 0x00, 0x00 };   // mov r8, [rbx + 0xD0]
  // mov rax, [rcx + 0x30]; mov eax, [rax + 0x84]; ret  /  mov rax, [rcx + 0x30]; mov eax, [rax + 0x70]; ret
  const BYTE SwarmCapGetterBytes[] = { 0x48, 0x8B, 0x41, 0x30, 0x8B, 0x80, 0x84, 0x00, 0x00, 0x00, 0xC3 };
  const BYTE PlacementCountGetterBytes[] = { 0x48, 0x8B, 0x41, 0x30, 0x8B, 0x40, 0x70, 0xC3 };
  const int InfoData = 0xD0;                 // the creation info's data
  const int SwarmDataStoreTypes = 0x80;      // SwarmEntityData's MaxDataStoreTypes (16 in every crowd)
  const int SwarmMaxAgents = 0x84;           // its MaxAgentCount
  const int PlacementData = 0x30;            // an entity's data
  const int PlacementMembers = 0x70;         // CrowdPlacementBoidsEntityData's MaxCrowdMembers
  const int PlacementSettings = 0x80;        // its BoidsSettings
  const int AssetName = 0x10;                // Asset's Name (the class is 0x18 bytes, its one field): a char pointer
  // The road cars: 2000 (the user, 2026-11-03 19:0x, after x 2 = 2420 jammed: "We can set the cars to 2000 however
  // lets fix the traffic jam and cars on the high way should go a little faster"). The highway (placement 83, 400 cars,
  // the only one with CarsTwoLaneSuperFast settings) keeps the level's 400: at 667 (x 2000 / 1210) its cars ran 51 of
  // their 60 - 70 m/s offline, at 400 59. Placement 93 (one-way streets with two traffic lights and a merge, the only
  // one-way placement of 65) keeps its 65: it queued 11 - 16% of the time at that already, 43% at x 1.65. The other 19
  // placements' 745 cars are raised to 1535 [data]. The walkers x 2.
  const unsigned LevelCars = 1210, Cars = 2000, LevelRaisedCars = 745, RaisedCars = 1535, WalkerRaise = 2;
  struct RaisedCrowd { unsigned Room, To; const char* Name; };
  const RaisedCrowd RaisedCrowds[] = {       // MaxAgentCount and what it's raised to; their placements' members
    { 1300, 2149, "the road cars (carcrowd_01)" },                       // 1210 -> 2000 (room 1300 x 2000 / 1210)
    { 901, 1802, "the mid caste walkers (midcastecrowd_01_walking)" },   // 887 -> 1774
    { 800, 1600, "the pedestrians (pedestriancrowd_01_walking)" },       // 721 -> 1442, + 33 of the rooftop bar's
    { 721, 1442, "the high caste walkers (highcastecrowd_01_walking)" }, // 688 -> 1376
  };
  // These crowds' placements, by their settings asset (and only theirs) and, where it's given, their level count (Only:
  // only that count; Not: any but that count): members x Times / Of (rounded down) + Plus; the first that fits. The 19
  // raised car placements come to 1528 that way; placement 186 (the only two-lane one of 100) takes the rest, so the
  // cars are 400 + 65 + 1535 = 2000 [data].
  struct RaisedKind { const char* Settings; unsigned Times, Of, Plus, Only, Not; };
  const RaisedKind RaisedSettings[] = {
    { "Levels/SP/SP_MainCity/Crowds/Cars/CarsOneWayBoidsSettings", RaisedCars, LevelRaisedCars, 0, 0, 65 },
    { "Levels/SP/SP_MainCity/Crowds/Cars/CarsTwoLaneFastBoidsSettings", RaisedCars, LevelRaisedCars, 7, 100, 0 },
    { "Levels/SP/SP_MainCity/Crowds/Cars/CarsTwoLaneFastBoidsSettings", RaisedCars, LevelRaisedCars, 0, 0, 0 },
    { "Levels/SP/SP_MainCity/Crowds/Pedestrian/PedestrianBoidsSettings", WalkerRaise, 1, 0, 0, 0 },
    { "Levels/SP/SP_MainCity/Crowds/MidCaste/MidCasteBoidsSettings_Walk", WalkerRaise, 1, 0, 0, 0 },
    { "Levels/SP/SP_MainCity/Crowds/HighCaste/HighCasteBoidsSettings_Walk", WalkerRaise, 1, 0, 0, 0 },
  };

  typedef void(__fastcall* tCarStep)(__int64 behaviour, __int64 params);
  tCarStep oCarStep = nullptr;
  tCarStep oWalkerStep = nullptr;
  typedef void(__fastcall* tUpdate)(__int64 placement, float dt, float time, __int64 a4, __int64 a5);
  tUpdate oUpdate = nullptr;
  typedef __int64(__fastcall* tCreate)(__int64 a1, __int64 a2, __int64 a3, __int64 a4);
  tCreate oSwarmCreate = nullptr;
  tCreate oPlacementCreate = nullptr;

  const int SlotCount = 32;
  Slot g_Slots[SlotCount];
  Rules g_Rules;
  SRWLOCK g_SlotLock = SRWLOCK_INIT;
  alignas(16) BYTE g_Settings[SlotCount][SettingsSize];   // each placement's settings copy for its step
  volatile long g_Busy[SlotCount];

  bool g_On = false;
  std::atomic<bool> g_Broken{ false };     // an exception in the rules: off for the rest of the session
  std::atomic<long long> g_Ticks{ 0 }, g_MaxTicks{ 0 }, g_Calls{ 0 };
  std::atomic<long long> g_Released{ 0 }, g_BusySteps{ 0 }, g_Refused{ 0 };
  std::atomic<long long> g_LastLog{ 0 };
  LARGE_INTEGER g_Freq;
  DWORD g_FirstThread = 0;
  std::atomic<int> g_OtherThreads{ 0 };

  // Why a step was left to the game (each kind logged once, with its values)
  enum Refusal { RefuseNone, RefusePlacement, RefuseStore, RefuseColumns, RefusePart, RefuseMembers, RefuseNodes,
    RefuseSettings, RefuseDt, RefuseSpeed, RefuseFault };
  const char* const RefusalText[] = { "", "no placement behind the behaviour", "no data store",
    "no records or flags column", "the placement's + 0x38 part isn't a CrowdPlacementBoidsEntity's",
    "a member count out of range", "no node array", "no car settings at params + 8", "the step's seconds out of range",
    "a speed range out of range", "a fault reading the game's data" };
  std::atomic<unsigned> g_RefusalsLogged{ 0 };

  // ReadOk: the rules can step it; ReadNoParams: its cars are read (in has them) but not the step's settings or
  // seconds; ReadNoPlacement: nothing of its cars; ReadNotCar: not a Car behaviour
  enum ReadResult { ReadOk, ReadNotCar, ReadNoPlacement, ReadNoParams };

  __int64 Base() { return (__int64)g_gameHandle; }

  bool CodeIs(__int64 rva, BYTE const* bytes, size_t size)
  {
    __try
    {
      return memcmp((void const*)(Base() + rva), bytes, size) == 0;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      return false;
    }
  }

  bool Plausible(__int64 p) { return p > 0x10000 && p < 0x7FFFFFFFFFFF && (p & 7) == 0; }

  // ---- the time of day and the view ----
  __int64 g_ClockPointer = 0;                // OFFSET_TIMEOFDAY (0: none)
  bool g_ViewCode = false;                   // the view's code is what the tools know (Init)
  std::atomic<float> g_LastClock{ -1.f };    // for the log
  std::atomic<int> g_LastView{ -1 };         // 1 read, 0 not, -1 never tried
  std::atomic<bool> g_ViewMissLogged{ false };

  // The game's time of day in seconds, -1 unknown
  float ReadClock()
  {
    if (!g_ClockPointer) return -1.f;
    __try
    {
      __int64 p = *(__int64*)g_ClockPointer;
      if (!Plausible(p)) return -1.f;
      p = *(__int64*)(p + 8);
      if (!Plausible(p)) return -1.f;
      p = *(__int64*)(p + 0x28);
      if (!Plausible(p)) return -1.f;
      float c = *(float*)(p + 0x30);
      return (c >= 0.f && c <= 86400.f) ? c : -1.f;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      return -1.f;
    }
  }

  struct View { float X, Y, Z, AX, AY, AZ; };

  // The view the game's walker step goes by: the first entry with ids 0, 0 of the views at [0x142415E50] + 8, its rows
  // at [entry + 0x208] + 0x50 (or the entry's own at + 0xD0 when that's none or its translation is ~0); row 3 is where
  // it is, row 2 its axis
  bool ReadView(View& v)
  {
    if (!g_ViewCode) return false;
    __try
    {
      __int64 owner = *(__int64*)(Base() + ViewOwnerRva);
      if (!Plausible(owner)) return false;
      __int64 b = *(__int64*)(owner + 8), e = *(__int64*)(owner + 0x10);
      if (!Plausible(b) || e < b || e - b > 8 * 256) return false;
      for (__int64 at = b; at < e; at += 8)
      {
        __int64 entry = *(__int64*)at;
        if (!Plausible(entry)) return false;
        if (*(int const*)(entry + 0x14) != 0 || *(int const*)(entry + 0x10) != 0) continue;
        float const* rows = nullptr;
        __int64 t = *(__int64*)(entry + 0x208);
        if (Plausible(t))
        {
          rows = (float const*)(t + 0x50);
          if (rows[12] * rows[12] + rows[13] * rows[13] + rows[14] * rows[14] < 1e-6f) rows = nullptr;
        }
        if (!rows) rows = (float const*)(entry + 0xD0);
        float ax = rows[8], ay = rows[9], az = rows[10];
        float x = rows[12], y = rows[13], z = rows[14];
        float al = std::sqrt(ax * ax + ay * ay + az * az);
        if (!(al > 0.5f && al < 2.f) || !(std::fabs(x) < 1e5f && std::fabs(y) < 1e5f && std::fabs(z) < 1e5f)) return false;
        v.X = x; v.Y = y; v.Z = z; v.AX = ax / al; v.AY = ay / al; v.AZ = az / al;
        return true;
      }
      return false;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      return false;
    }
  }

  // The night shares of a raised placement (see Raised): its members are more than the level's by RaisedCars /
  // LevelRaisedCars (cars) or WalkerRaise (walkers), its night share smaller by as much, so its nights are as the level's
  const float RaisedCarsAtNight = CarsAtNight * (float)LevelRaisedCars / (float)RaisedCars;
  const float RaisedPeopleAtNight = PeopleAtNight / (float)WalkerRaise;

  // The shares now (cars, people) and the view; with no view known, no share (1)
  void Shares(float& cars, float& people, View& v, bool& known, bool raised = false)
  {
    float clock = ReadClock();
    known = ReadView(v);
    g_LastClock.store(clock, std::memory_order_relaxed);
    g_LastView.store(known ? 1 : 0, std::memory_order_relaxed);
    if (!known && g_ViewCode && !g_ViewMissLogged.exchange(true))
      util::log::Warning("Road traffic: the view couldn't be read (the time of day's share is off until it can; logged "
        "once)");
    cars = known ? DayShare(clock, raised ? RaisedCarsAtNight : CarsAtNight) : 1.f;
    people = known ? DayShare(clock, raised ? RaisedPeopleAtNight : PeopleAtNight) : 1.f;
  }

  // The game respawns only up to the share: the despawner's + 0x34 (the members it keeps despawned at least) raised to
  // those the share leaves out, never lowered (the game's own, from TargetSpawnedPercentage, set this update)
  bool g_RespawnCode = false;
  std::atomic<long long> g_RespawnsHeld{ 0 };
  void HoldRespawns(__int64 behaviour, int count, int wanted)
  {
    if (!g_RespawnCode || wanted >= count) return;
    __try
    {
      __int64 d = *(__int64*)(behaviour + 0x18);
      if (!Plausible(d)) return;
      __int64 pb = *(__int64*)(d + 0x40), pe = *(__int64*)(d + 0x48);
      if (pe < pb || pe - pb > (__int64)count * 4 || ((pe - pb) & 3)) return;   // the despawned pool, a u32 each
      unsigned game = *(unsigned const*)(d + 0x34);
      unsigned want = (unsigned)(count - wanted);
      if (game <= (unsigned)count && want > game)
      {
        *(unsigned*)(d + 0x34) = want;
        ++g_RespawnsHeld;
      }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
  }

  // ---- more by day: the raised crowds ----
  SRWLOCK g_RaiseLock = SRWLOCK_INIT;        // one factory raising at a time
  const int RaisedKept = 1024;               // the placements' data the tools raised (a ring: the oldest go first)
  std::atomic<__int64> g_RaisedData[RaisedKept];
  std::atomic<unsigned> g_RaisedTo[RaisedKept];
  std::atomic<unsigned> g_RaisedFrom[RaisedKept];   // the level's members (the crowd test)
  std::atomic<long long> g_RaisedTotal{ 0 };
  std::atomic<long long> g_CrowdsRaised{ 0 }, g_PlacementsRaised{ 0 }, g_MembersAdded{ 0 };

  // `data` is a placement's data the tools raised, and still has the members they gave it; `level` the level's members
  // (when it's given)
  bool WasRaised(__int64 data, unsigned count, unsigned* level = nullptr)
  {
    long long total = g_RaisedTotal.load(std::memory_order_acquire);
    int n = total < RaisedKept ? (int)total : RaisedKept;
    for (int i = 0; i < n; ++i)
      if (g_RaisedData[i].load(std::memory_order_relaxed) == data && g_RaisedTo[i].load(std::memory_order_relaxed) == count)
      {
        if (level) *level = g_RaisedFrom[i].load(std::memory_order_relaxed);
        return true;
      }
    return false;
  }

  // Under g_RaiseLock
  void NoteRaised(__int64 data, unsigned count, unsigned level)
  {
    long long total = g_RaisedTotal.load(std::memory_order_relaxed);
    int i = (int)(total % RaisedKept);
    g_RaisedData[i].store(data, std::memory_order_relaxed);
    g_RaisedTo[i].store(count, std::memory_order_relaxed);
    g_RaisedFrom[i].store(level, std::memory_order_relaxed);
    g_RaisedTotal.store(total + 1, std::memory_order_release);
  }

  // The game's `name` is `want`, letters in either case (read up to the first difference)
  bool NameIs(char const* name, char const* want)
  {
    for (int i = 0;; ++i)
    {
      char a = name[i], b = want[i];
      if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
      if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
      if (a != b) return false;
      if (!a) return true;
    }
  }

  // A crowd about to be made (the SwarmEntity factory's creation info): its data's room raised when it's one of
  // RaisedCrowds. Its index there, or -1 (any other crowd; a data raised before has the raised room, which none has).
  int RaiseSwarm(__int64 info)
  {
    __try
    {
      __int64 data = *(__int64*)(info + InfoData);
      if (!Plausible(data) || *(__int64*)data != Base() + SwarmDataVtableRva) return -1;
      if (*(unsigned const*)(data + SwarmDataStoreTypes) != 16) return -1;
      unsigned room = *(unsigned const*)(data + SwarmMaxAgents);
      for (int k = 0; k < (int)(sizeof(RaisedCrowds) / sizeof(RaisedCrowds[0])); ++k)
        if (room == RaisedCrowds[k].Room)
        {
          *(unsigned*)(data + SwarmMaxAgents) = RaisedCrowds[k].To;
          return k;
        }
      return -1;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      return -1;
    }
  }

  // A placement about to be made (its factory's creation info): its data's members raised when its settings asset is
  // one of RaisedSettings and the tools haven't raised that data already. The members it had (0: not raised) and has.
  unsigned RaisePlacement(__int64 info, __int64& data, unsigned& to)
  {
    __try
    {
      data = *(__int64*)(info + InfoData);
      if (!Plausible(data) || *(__int64*)data != Base() + PlacementDataVtableRva) return 0;
      __int64 settings = *(__int64*)(data + PlacementSettings);
      if (!Plausible(settings) || *(__int64*)settings != Base() + SettingsVtableRva) return 0;
      char const* name = *(char const* const*)(settings + AssetName);
      if (!name) return 0;
      unsigned count = *(unsigned const*)(data + PlacementMembers);
      if (count == 0 || count > 1000 || WasRaised(data, count)) return 0;
      int kind = -1;
      for (int k = 0; k < (int)(sizeof(RaisedSettings) / sizeof(RaisedSettings[0])) && kind < 0; ++k)
      {
        RaisedKind const& c = RaisedSettings[k];
        if ((c.Only == 0 || count == c.Only) && (c.Not == 0 || count != c.Not) && NameIs(name, c.Settings)) kind = k;
      }
      if (kind < 0) return 0;
      RaisedKind const& r = RaisedSettings[kind];
      to = (unsigned)((unsigned long long)count * r.Times / r.Of) + r.Plus;
      if (to <= count) return 0;
      *(unsigned*)(data + PlacementMembers) = to;
      return count;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      return 0;
    }
  }

  // The SwarmEntity factory: the crowd's room raised (RaisedCrowds), then the game's
  __int64 __fastcall hSwarmCreate(__int64 self, __int64 info, __int64 arena, __int64 a4)
  {
    AcquireSRWLockExclusive(&g_RaiseLock);
    int k = RaiseSwarm(info);
    ReleaseSRWLockExclusive(&g_RaiseLock);
    if (k >= 0)
    {
      ++g_CrowdsRaised;
      util::log::Write("Road traffic: %s: the crowd's room raised from %u to %u members as the game makes it",
        RaisedCrowds[k].Name, RaisedCrowds[k].Room, RaisedCrowds[k].To);
    }
    return oSwarmCreate(self, info, arena, a4);
  }

  // The CrowdPlacementBoidsEntity factory: the placement's members raised (RaisedSettings), then the game's
  __int64 __fastcall hPlacementCreate(__int64 info, __int64 a2, __int64 a3, __int64 a4)
  {
    AcquireSRWLockExclusive(&g_RaiseLock);
    __int64 data = 0;
    unsigned to = 0;
    unsigned was = RaisePlacement(info, data, to);
    if (was) NoteRaised(data, to, was);
    ReleaseSRWLockExclusive(&g_RaiseLock);
    if (was)
    {
      ++g_PlacementsRaised;
      g_MembersAdded += (long long)to - (long long)was;
    }
    return oPlacementCreate(info, a2, a3, a4);
  }

  // The placement is one the tools raised (for its night share); `level` its level's members (the crowd test)
  bool Raised(__int64 placement, unsigned* level = nullptr)
  {
    if (!g_RaisedTotal.load(std::memory_order_relaxed)) return false;
    __try
    {
      __int64 data = *(__int64*)(placement + PlacementData);
      return Plausible(data) && WasRaised(data, *(unsigned const*)(data + PlacementMembers), level);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      return false;
    }
  }

  // ---- the crowd test (MISC > Performance): its mode for the hooks, and what they measure ----
  // 0 off; 1 the level's crowds (a raised placement shows its level count), in sight too; 2 as set, in sight too
  std::atomic<int> g_TestMode{ 0 };
  std::atomic<bool> g_TestOn{ false };       // a test runs (the hooks keep the shown counts)
  // The crowds' CPU so far (QueryPerformanceCounter ticks, any thread, never reset): the game's per-frame update of every
  // placement, the game's own car and walker steps (in it), the rules (in it)
  std::atomic<long long> g_UpdateAll{ 0 }, g_GameCarAll{ 0 }, g_GameWalkAll{ 0 }, g_RulesCarAll{ 0 }, g_RulesWalkAll{ 0 };
  std::atomic<long long> g_UpdateCalls{ 0 };
  // The longest of each the last minute (for the minute lines)
  std::atomic<long long> g_UpdateMost{ 0 }, g_GameCarMost{ 0 }, g_GameWalkMost{ 0 };

  void Most(std::atomic<long long>& most, long long ticks)
  {
    long long m = most.load(std::memory_order_relaxed);
    while (ticks > m && !most.compare_exchange_weak(m, ticks)) {}
  }

  // The shown members of each slot at its last step while a test runs, and when (QueryPerformanceCounter ticks)
  std::atomic<int> g_CarShown[SlotCount];
  std::atomic<long long> g_CarShownAt[SlotCount];

  // After a step (the slot's own thread, before it's let go): its shown members, `visible` one byte each
  void NoteShown(std::atomic<int>& shown, std::atomic<long long>& at, uint8_t const* visible, int count, long long now)
  {
    int n = 0;
    for (int i = 0; i < count; ++i) n += visible[i] ? 1 : 0;
    shown.store(n, std::memory_order_relaxed);
    at.store(now, std::memory_order_relaxed);
  }

  // A step's share and the crowd test: `share` as Shares gave it for an unraised crowd while the test shows the level's
  // crowds (mode 1), made a share of the placement's `count` members that shows its `level` count's worth
  void TestShare(float& share, bool& inSightToo, int mode, bool raised, unsigned level, int count)
  {
    if (!mode) return;
    inSightToo = true;
    if (mode == 1 && raised && level && count > 0) share *= (float)level / (float)count;
  }

  // The data store's column for a key (the game's own lookup, 0x143B886A0: a sorted array of { key, column } at
  // store + 0x10 .. + 0x18), then its array: [[column + 8] + 8]
  __int64 Column(__int64 store, unsigned __int64 key)
  {
    __int64 begin = *(__int64*)(store + 0x10), end = *(__int64*)(store + 0x18);
    if (!Plausible(begin) || end < begin || (end - begin) > 0x10000 || ((end - begin) & 15)) return 0;
    __int64 count = (end - begin) / 16, lo = 0;
    while (count > 0)
    {
      __int64 half = count / 2;
      if (*(unsigned __int64*)(begin + (lo + half) * 16) < key) { lo += half + 1; count -= half + 1; }
      else count = half;
    }
    if (begin + lo * 16 >= end || *(unsigned __int64*)(begin + lo * 16) != key) return 0;
    __int64 column = *(__int64*)(begin + lo * 16 + 8);
    if (!Plausible(column)) return 0;
    __int64 inner = *(__int64*)(column + 8);
    if (!Plausible(inner)) return 0;
    __int64 data = *(__int64*)(inner + 8);
    return Plausible(data) ? data : 0;
  }

  // Everything one step needs, read from the game (inside __try: no C++ objects): first the placement's cars, then
  // the step's settings and seconds. `refusal`, `placement` and `value` say why it isn't ReadOk.
  ReadResult Read(__int64 behaviour, __int64 params, StepInput& in, __int64& settings, int& refusal, __int64& placement,
    double& value)
  {
    volatile bool carsRead = false;
    __try
    {
      if (*(__int64*)behaviour != Base() + CarVtableRva) return ReadNotCar;
      placement = *(__int64*)(behaviour + 8);
      if (!Plausible(placement)) { refusal = RefusePlacement; return ReadNoPlacement; }
      __int64 store = *(__int64*)(placement + 0x98);
      if (!Plausible(store)) { refusal = RefuseStore; value = (double)store; return ReadNoPlacement; }
      unsigned int first = *(unsigned int*)(placement + 0xA0);
      __int64 records = Column(store, *(unsigned __int64*)(Base() + RecordsKeyRva));
      __int64 flags = Column(store, *(unsigned __int64*)(Base() + FlagsKeyRva));
      if (!records || !flags) { refusal = RefuseColumns; return ReadNoPlacement; }
      // the count the step goes by: the placement's + 0x38 part's slot 4 (0x143B95600: its + 0x10, the placement's
      // + 0x48), read here (that part's vtable checked: a CrowdPlacementBoidsEntity's)
      if (*(__int64*)(placement + 0x38) != Base() + PlacementPartVtableRva) { refusal = RefusePart; return ReadNoPlacement; }
      unsigned int count = *(unsigned int*)(placement + 0x48);
      if (count == 0 || count > (unsigned)MaxMembers) { refusal = RefuseMembers; value = (double)count; return ReadNoPlacement; }
      __int64 nodesBegin = *(__int64*)(placement + 0x78), nodesEnd = *(__int64*)(placement + 0x80);
      if (!Plausible(nodesBegin) || nodesEnd <= nodesBegin || (nodesEnd - nodesBegin) % NodeSize ||
          (nodesEnd - nodesBegin) / NodeSize > 65535)
      {
        refusal = RefuseNodes;
        value = (double)(nodesEnd - nodesBegin);
        return ReadNoPlacement;
      }
      uint8_t* rec = (uint8_t*)(records + (__int64)first * RecordSize);
      uint16_t* flag = (uint16_t*)(flags + (__int64)first * 2);
      // touch the last record and flag (a bad count faults here, not in the rules)
      volatile BYTE touch = rec[(size_t)count * RecordSize - 1];
      volatile uint16_t touch2 = flag[count - 1];
      (void)touch; (void)touch2;
      in.Key = (uintptr_t)placement;
      in.Records = rec;
      in.Flags = flag;
      in.Count = (int)count;
      in.Nodes = (uint8_t const*)nodesBegin;
      in.NodeCount = (int)((nodesEnd - nodesBegin) / NodeSize);
      carsRead = true;

      settings = *(__int64*)(params + 8);
      if (!Plausible(settings) || *(int*)(settings + SettingsBehaviour) != 1)
      {
        refusal = RefuseSettings;
        value = (double)settings;
        return ReadNoParams;
      }
      float dt = *(float*)(params + 0x10);
      if (!(dt > 0.f && dt <= 0.5f)) { refusal = RefuseDt; value = dt; return ReadNoParams; }
      in.SpeedMin = *(float*)(settings + SettingsSpeed);
      in.SpeedMax = *(float*)(settings + SettingsSpeed + 4);
      in.Dt = dt;
      if (!(in.SpeedMin >= 0.f && in.SpeedMax >= in.SpeedMin && in.SpeedMax < 500.f))
      {
        refusal = RefuseSpeed;
        value = in.SpeedMax;
        return ReadNoParams;
      }
      return ReadOk;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      refusal = RefuseFault;
      return carsRead ? ReadNoParams : ReadNoPlacement;
    }
  }

  bool RulesBefore(Slot& slot, StepInput const& in)
  {
    __try
    {
      g_Rules.Before(slot, in);
      return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      return false;
    }
  }

  bool RulesAfter(Slot& slot, StepInput const& in)
  {
    __try
    {
      g_Rules.After(slot, in, g_Slots, SlotCount);
      return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      return false;
    }
  }

  int ReleaseGuarded(Slot& slot, StepInput const& in)
  {
    __try
    {
      return g_Rules.Release(slot, in);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      return 0;
    }
  }

  // The rules can't step this placement: the cars they keep unseen go back on the road for the game's step (not while
  // its rules run on another thread; the lock keeps its slot from being given to another placement meanwhile)
  void LetGo(StepInput const& in)
  {
    AcquireSRWLockShared(&g_SlotLock);
    Slot* slot = Rules::FindExisting(in.Key, g_Slots, SlotCount);
    if (slot)
    {
      int index = (int)(slot - g_Slots);
      if (InterlockedCompareExchange(&g_Busy[index], 1, 0) == 0)
      {
        int shown = ReleaseGuarded(*slot, in);
        InterlockedExchange(&g_Busy[index], 0);
        if (shown) g_Released += shown;
      }
    }
    ReleaseSRWLockShared(&g_SlotLock);
  }

  void NoteRefusal(int refusal, __int64 placement, double value, bool walkers = false)
  {
    if (!walkers) ++g_Refused;
    unsigned bit = 1u << (refusal + (walkers ? 16 : 0));
    if (g_RefusalsLogged.fetch_or(bit) & bit) return;
    util::log::Warning("Road traffic: a %s step was left to the game: %s (placement %llX, %g); more of this kind are only "
      "counted", walkers ? "walker" : "car", RefusalText[refusal], (unsigned long long)placement, value);
  }

  // The step with the settings copy (AvoidanceEnabled cleared) at params + 8; the game's own pointer back after
  void StepWithCopy(__int64 behaviour, __int64 params, int slotIndex, __int64 settings)
  {
    BYTE* copy = g_Settings[slotIndex];
    memcpy(copy, (void const*)settings, SettingsSize);
    copy[SettingsAvoidance] = 0;
    *(__int64*)(params + 8) = (__int64)copy;
    oCarStep(behaviour, params);
    *(__int64*)(params + 8) = settings;
  }

  void MaybeLog();

  // ---- the walkers (WalkersCore.h) and the standing crowds ----
  const int WalkSlotCount = 128;             // walking placements at once (the city has 114)
  const int IdleSlotCount = 96;              // standing ones (64)
  wk::Slot* g_WalkSlots = nullptr;           // made at Init (~145 KB each)
  wk::IdleSlot* g_IdleSlots = nullptr;
  wk::Rules g_WalkRules;
  wk::IdleRules g_IdleRules;
  SRWLOCK g_WalkLock = SRWLOCK_INIT;
  SRWLOCK g_IdleLock = SRWLOCK_INIT;
  volatile long g_WalkBusy[WalkSlotCount];
  volatile long g_IdleBusy[IdleSlotCount];
  std::atomic<int> g_WalkShown[WalkSlotCount];         // as g_CarShown (the crowd test)
  std::atomic<long long> g_WalkShownAt[WalkSlotCount];
  bool g_WalkOn = false, g_IdleOn = false;
  std::atomic<bool> g_WalkBroken{ false }, g_IdleBroken{ false };
  std::atomic<long long> g_WalkTicks{ 0 }, g_WalkCalls{ 0 }, g_WalkBusySteps{ 0 }, g_WalkReleased{ 0 }, g_IdleCalls{ 0 };
  std::atomic<long long> g_WalkMaxTicks{ 0 };

  enum WalkRead { WalkOk, WalkNot, WalkNoPlacement, WalkNoParams };

  // A placement's members (as Read: its records, flags, count, nodes), its behaviour checked to be `vtable`; count at most
  // wk::MaxMembers. Inside __try (no C++ objects).
  bool ReadMembers(__int64 behaviour, __int64 vtable, wk::StepInput& in, int& refusal, __int64& placement, double& value)
  {
    if (*(__int64*)behaviour != Base() + vtable) return false;
    placement = *(__int64*)(behaviour + 8);
    if (!Plausible(placement)) { refusal = RefusePlacement; return false; }
    __int64 store = *(__int64*)(placement + 0x98);
    if (!Plausible(store)) { refusal = RefuseStore; value = (double)store; return false; }
    unsigned int first = *(unsigned int*)(placement + 0xA0);
    __int64 records = Column(store, *(unsigned __int64*)(Base() + RecordsKeyRva));
    __int64 flags = Column(store, *(unsigned __int64*)(Base() + FlagsKeyRva));
    if (!records || !flags) { refusal = RefuseColumns; return false; }
    if (*(__int64*)(placement + 0x38) != Base() + PlacementPartVtableRva) { refusal = RefusePart; return false; }
    unsigned int count = *(unsigned int*)(placement + 0x48);
    if (count == 0 || count > (unsigned)wk::MaxMembers) { refusal = RefuseMembers; value = (double)count; return false; }
    __int64 nodesBegin = *(__int64*)(placement + 0x78), nodesEnd = *(__int64*)(placement + 0x80);
    if (!Plausible(nodesBegin) || nodesEnd <= nodesBegin || (nodesEnd - nodesBegin) % NodeSize ||
        (nodesEnd - nodesBegin) / NodeSize > 65535)
    {
      refusal = RefuseNodes;
      value = (double)(nodesEnd - nodesBegin);
      return false;
    }
    uint8_t* rec = (uint8_t*)(records + (__int64)first * RecordSize);
    uint16_t* flag = (uint16_t*)(flags + (__int64)first * 2);
    volatile BYTE touch = rec[(size_t)count * RecordSize - 1];
    volatile uint16_t touch2 = flag[count - 1];
    (void)touch; (void)touch2;
    in.Key = (uintptr_t)placement;
    in.Records = rec;
    in.Flags = flag;
    in.Count = (int)count;
    in.Nodes = (uint8_t const*)nodesBegin;
    in.NodeCount = (int)((nodesEnd - nodesBegin) / NodeSize);
    return true;
  }

  // A walker step's placement, then its settings and seconds (boats, MemberRadius over 2, aren't walkers)
  WalkRead ReadWalkers(__int64 behaviour, __int64 params, wk::StepInput& in, int& refusal, __int64& placement, double& value)
  {
    volatile bool read = false;
    __try
    {
      if (*(__int64*)behaviour != Base() + WalkerVtableRva) return WalkNot;
      __int64 settings = *(__int64*)(params + 8);
      if (Plausible(settings) && *(int*)(settings + SettingsBehaviour) == 0 && !(*(float*)(settings + SettingsRadius) <= 2.f))
        return WalkNot;
      if (!ReadMembers(behaviour, WalkerVtableRva, in, refusal, placement, value)) return WalkNoPlacement;
      read = true;
      if (!Plausible(settings) || *(int*)(settings + SettingsBehaviour) != 0)
      {
        refusal = RefuseSettings;
        value = (double)settings;
        return WalkNoParams;
      }
      float dt = *(float*)(params + 0x10);
      if (!(dt > 0.f && dt <= 0.5f)) { refusal = RefuseDt; value = dt; return WalkNoParams; }
      in.SpeedMin = *(float*)(settings + SettingsSpeed);
      in.SpeedMax = *(float*)(settings + SettingsSpeed + 4);
      in.Radius = *(float*)(settings + SettingsRadius);
      in.Dt = dt;
      if (!(in.SpeedMin >= 0.f && in.SpeedMax >= in.SpeedMin && in.SpeedMax < 20.f) || !(in.Radius > 0.f))
      {
        refusal = RefuseSpeed;
        value = in.SpeedMax;
        return WalkNoParams;
      }
      return WalkOk;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      refusal = RefuseFault;
      return read ? WalkNoParams : WalkNoPlacement;
    }
  }

  bool WalkBefore(wk::Slot& slot, wk::StepInput const& in)
  {
    __try
    {
      g_WalkRules.Before(slot, in);
      g_WalkRules.Freeze(slot, in);
      return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      return false;
    }
  }

  void WalkThaw(wk::Slot& slot, wk::StepInput const& in)
  {
    __try
    {
      g_WalkRules.Thaw(slot, in);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
  }

  bool WalkAfter(wk::Slot& slot, wk::StepInput const& in)
  {
    __try
    {
      g_WalkRules.After(slot, in, g_WalkSlots, WalkSlotCount);
      return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      return false;
    }
  }

  int WalkRelease(wk::Slot& slot, wk::StepInput const& in)
  {
    __try
    {
      return g_WalkRules.Release(slot, in);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      return 0;
    }
  }

  // The rules can't step this walking placement: the walkers they keep unseen show where they are
  void LetGoWalkers(wk::StepInput const& in)
  {
    AcquireSRWLockShared(&g_WalkLock);
    wk::Slot* slot = wk::Rules::FindExisting(in.Key, g_WalkSlots, WalkSlotCount);
    if (slot)
    {
      int index = (int)(slot - g_WalkSlots);
      if (InterlockedCompareExchange(&g_WalkBusy[index], 1, 0) == 0)
      {
        int shown = WalkRelease(*slot, in);
        InterlockedExchange(&g_WalkBusy[index], 0);
        if (shown) g_WalkReleased += shown;
      }
    }
    ReleaseSRWLockShared(&g_WalkLock);
  }

  void __fastcall hWalkerStep(__int64 behaviour, __int64 params)
  {
    if (!g_WalkOn)
    {
      oWalkerStep(behaviour, params);
      return;
    }
    LARGE_INTEGER t0, t1;
    QueryPerformanceCounter(&t0);
    wk::StepInput in{};
    in.Now = (double)t0.QuadPart / (double)g_Freq.QuadPart;
    __int64 placement = 0;
    int refusal = RefuseNone;
    double value = 0.0;
    WalkRead got = ReadWalkers(behaviour, params, in, refusal, placement, value);
    if (got == WalkNot)
    {
      oWalkerStep(behaviour, params);
      return;
    }
    if (got != WalkOk || g_WalkBroken.load(std::memory_order_relaxed))
    {
      if (got == WalkOk || got == WalkNoParams)
        LetGoWalkers(in);
      if (refusal != RefuseNone)
        NoteRefusal(refusal, placement, value, true);
      oWalkerStep(behaviour, params);
      return;
    }
    {
      float cars;
      View view;
      bool known;
      unsigned level = 0;
      bool raised = Raised(placement, &level);
      int mode = g_TestMode.load(std::memory_order_relaxed);
      Shares(cars, in.Density, view, known, raised && mode != 1);
      TestShare(in.Density, in.InSightToo, mode, raised, level, in.Count);
      in.ViewKnown = known;
      if (known) { in.ViewX = view.X; in.ViewY = view.Y; in.ViewZ = view.Z; in.ViewAX = view.AX; in.ViewAY = view.AY; in.ViewAZ = view.AZ; }
      HoldRespawns(behaviour, in.Count, Wanted(in.Density, in.Count));
    }
    AcquireSRWLockExclusive(&g_WalkLock);
    wk::Slot* slot = g_WalkRules.FindSlot(in, g_WalkSlots, WalkSlotCount);
    ReleaseSRWLockExclusive(&g_WalkLock);
    int index = slot ? (int)(slot - g_WalkSlots) : -1;
    if (index < 0 || InterlockedCompareExchange(&g_WalkBusy[index], 1, 0) != 0)
    {
      ++g_WalkBusySteps;
      oWalkerStep(behaviour, params);
      return;
    }
    bool ok = WalkBefore(*slot, in);
    QueryPerformanceCounter(&t1);
    long long ticks = t1.QuadPart - t0.QuadPart;
    oWalkerStep(behaviour, params);
    QueryPerformanceCounter(&t0);
    long long game = t0.QuadPart - t1.QuadPart;
    WalkThaw(*slot, in);      // always: the walkers marked despawned for the step are the game's again
    if (ok) ok = WalkAfter(*slot, in);
    if (!ok)
    {
      int shown = WalkRelease(*slot, in);
      if (shown) g_WalkReleased += shown;
    }
    QueryPerformanceCounter(&t1);
    ticks += t1.QuadPart - t0.QuadPart;
    if (ok && g_TestOn.load(std::memory_order_relaxed))
      NoteShown(g_WalkShown[index], g_WalkShownAt[index], slot->Visible, in.Count, t1.QuadPart);
    InterlockedExchange(&g_WalkBusy[index], 0);
    g_GameWalkAll.fetch_add(game, std::memory_order_relaxed);
    g_RulesWalkAll.fetch_add(ticks, std::memory_order_relaxed);
    Most(g_GameWalkMost, game);
    if (!ok && !g_WalkBroken.exchange(true))
      util::log::Warning("Road traffic: the walker rules ran into an exception and are off for this session (the walkers "
        "they kept unseen are shown; the game's own walker step goes on)");
    g_WalkTicks += ticks;
    ++g_WalkCalls;
    long long most = g_WalkMaxTicks.load(std::memory_order_relaxed);
    while (ticks > most && !g_WalkMaxTicks.compare_exchange_weak(most, ticks)) {}
    MaybeLog();
  }

  // A standing crowd's members (the Idle behaviour)
  bool ReadIdle(__int64 placement, wk::StepInput& in)
  {
    __try
    {
      __int64 behaviour = *(__int64*)(placement + 0xA8);
      if (!Plausible(behaviour) || *(__int64*)behaviour != Base() + IdleVtableRva) return false;
      if (*(__int64*)(behaviour + 8) != placement) return false;
      int refusal = RefuseNone;
      __int64 p = 0;
      double value = 0.0;
      return ReadMembers(behaviour, IdleVtableRva, in, refusal, p, value);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      return false;
    }
  }

  bool IdleApply(wk::IdleSlot& slot, wk::StepInput const& in)
  {
    __try
    {
      g_IdleRules.Apply(slot, in);
      return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      return false;
    }
  }

  int IdleRelease(wk::IdleSlot& slot, wk::StepInput const& in)
  {
    __try
    {
      return g_IdleRules.Release(slot, in);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      return 0;
    }
  }

  // A placement's per-frame update: the game's, then (a standing crowd) the time of day's share
  void __fastcall hUpdate(__int64 placement, float dt, float time, __int64 a4, __int64 a5)
  {
    LARGE_INTEGER t0;
    QueryPerformanceCounter(&t0);
    oUpdate(placement, dt, time, a4, a5);
    {
      LARGE_INTEGER u;
      QueryPerformanceCounter(&u);
      long long ticks = u.QuadPart - t0.QuadPart;
      g_UpdateAll.fetch_add(ticks, std::memory_order_relaxed);
      g_UpdateCalls.fetch_add(1, std::memory_order_relaxed);
      Most(g_UpdateMost, ticks);
      t0 = u;
    }
    if (!g_IdleOn) return;
    wk::StepInput in{};
    in.Now = (double)t0.QuadPart / (double)g_Freq.QuadPart;
    if (!ReadIdle(placement, in)) return;
    bool broken = g_IdleBroken.load(std::memory_order_relaxed);
    if (!broken)
    {
      float cars;
      View view;
      bool known;
      Shares(cars, in.Density, view, known);
      in.ViewKnown = known;
      if (known) { in.ViewX = view.X; in.ViewY = view.Y; in.ViewZ = view.Z; in.ViewAX = view.AX; in.ViewAY = view.AY; in.ViewAZ = view.AZ; }
    }
    AcquireSRWLockExclusive(&g_IdleLock);
    wk::IdleSlot* slot = broken ? wk::IdleRules::FindExisting(in.Key, g_IdleSlots, IdleSlotCount)
                                : g_IdleRules.FindSlot(in, g_IdleSlots, IdleSlotCount);
    ReleaseSRWLockExclusive(&g_IdleLock);
    int index = slot ? (int)(slot - g_IdleSlots) : -1;
    if (index < 0 || InterlockedCompareExchange(&g_IdleBusy[index], 1, 0) != 0) return;
    bool ok = !broken && IdleApply(*slot, in);
    if (!ok) IdleRelease(*slot, in);
    InterlockedExchange(&g_IdleBusy[index], 0);
    if (!ok && !broken && !g_IdleBroken.exchange(true))
      util::log::Warning("Road traffic: the standing crowds' share ran into an exception and is off for this session (the "
        "people it kept unseen are shown)");
    ++g_IdleCalls;
  }

  void Log(bool broken)
  {
    if (broken)
    {
      util::log::Warning("Road traffic: the rules ran into an exception and are off for this session (the cars they kept "
        "unseen are shown; the game's own car step goes on)");
      return;
    }
    Stats t;
    int placements = 0;
    for (int k = 0; k < SlotCount; ++k)
    {
      Slot const& s = g_Slots[k];
      if (!s.Key) continue;
      ++placements;
      t.Steps += s.Counts.Steps; t.Cars += s.Counts.Cars; t.Waiting += s.Counts.Waiting; t.Unseen += s.Counts.Unseen;
      t.Held += s.Counts.Held; t.Reverted += s.Counts.Reverted; t.Appeared += s.Counts.Appeared; t.Revealed += s.Counts.Revealed;
      t.NoLane += s.Counts.NoLane; t.LaneFound += s.Counts.LaneFound; t.LaneMissing += s.Counts.LaneMissing;
      t.Respawned += s.Counts.Respawned;
      t.DensityHidden += s.Counts.DensityHidden; t.DensityShown += s.Counts.DensityShown;
      t.PutBackHidden += s.Counts.PutBackHidden;
      t.CycleHidden += s.Counts.CycleHidden; t.CycleCross += s.Counts.CycleCross;
      t.StandHidden += s.Counts.StandHidden; t.StandHiddenInSight += s.Counts.StandHiddenInSight;
      t.CapHidden += s.Counts.CapHidden; t.CapHiddenInSight += s.Counts.CapHiddenInSight;
    }
    long long calls = g_Calls.exchange(0), ticks = g_Ticks.exchange(0), most = g_MaxTicks.exchange(0);
    double ms = calls ? 1000.0 * (double)ticks / (double)g_Freq.QuadPart : 0.0;
    // the game's own steps and its per-frame crowd update, the last minute (Log runs one at a time)
    static long long lastGameCar = 0, lastGameWalk = 0, lastUpdate = 0, lastUpdateCalls = 0;
    long long gameCar = g_GameCarAll.load(), gameWalk = g_GameWalkAll.load(), update = g_UpdateAll.load();
    long long updateCalls = g_UpdateCalls.load();
    double toMs = 1000.0 / (double)g_Freq.QuadPart;
    double gameCarMs = (gameCar - lastGameCar) * toMs, gameWalkMs = (gameWalk - lastGameWalk) * toMs;
    double updateMs = (update - lastUpdate) * toMs;
    long long updates = updateCalls - lastUpdateCalls;
    lastGameCar = gameCar; lastGameWalk = gameWalk; lastUpdate = update; lastUpdateCalls = updateCalls;
    double gameCarMost = g_GameCarMost.exchange(0) * toMs, gameWalkMost = g_GameWalkMost.exchange(0) * toMs;
    double updateMost = g_UpdateMost.exchange(0) * toMs;
    util::log::Write("Road traffic: %d car placements so far; the last minute %lld steps took the rules %.2f ms in all "
      "(%.3f ms a step on average, %.3f ms the longest)%s and the game's own car step in them %.2f ms (%.3f ms the "
      "longest); so far %lld cars appeared (%lld of them respawns, started "
      "unseen), %lld shown, %lld made unseen inside another, %lld turned round or off the road unseen, %lld car-steps "
      "held back, %lld put back (%lld cars made unseen after %.0f s of it), %lld made unseen to end a wait cycle (%lld "
      "of them over two or more placements), %lld made unseen after standing %.0f s out of sight or %.0f s in sight (%lld "
      "in sight), %lld made unseen at a placement's first step above its route's most (%lld in sight), %lld without a "
      "lane; road segments found in "
      "the lane table %lld, not found %lld (those "
      "keep the game's own lane); steps left to the game: %lld refused, %lld with no slot free or the placement's step "
      "on another thread; "
      "%lld unseen cars shown again because the rules couldn't step them",
      placements, calls, ms, calls ? ms / (double)calls : 0.0, 1000.0 * (double)most / (double)g_Freq.QuadPart,
      g_OtherThreads.load() ? " (on more than one thread)" : "", gameCarMs, gameCarMost, t.Appeared, t.Respawned, t.Revealed, t.Waiting, t.Unseen,
      t.Held, t.Reverted, t.PutBackHidden, PutBackSeconds, t.CycleHidden, t.CycleCross,
      t.StandHidden, StandOutSeconds, StandInSeconds, t.StandHiddenInSight, t.CapHidden, t.CapHiddenInSight, t.NoLane, t.LaneFound, t.LaneMissing, g_Refused.load(),
      g_BusySteps.load(), g_Released.load());

    // the walkers, the standing crowds and the time of day's share
    wk::Stats w;
    int walkPlacements = 0, idlePlacements = 0;
    long long idleHid = 0, idleShown = 0;
    if (g_WalkSlots)
      for (int k = 0; k < WalkSlotCount; ++k)
      {
        wk::Slot const& s = g_WalkSlots[k];
        if (!s.Key) continue;
        ++walkPlacements;
        w.Appeared += s.Counts.Appeared; w.Respawned += s.Counts.Respawned; w.Held += s.Counts.Held;
        w.Sidesteps += s.Counts.Sidesteps; w.Reverted += s.Counts.Reverted; w.Waiting += s.Counts.Waiting;
        w.Revealed += s.Counts.Revealed; w.Ghosts += s.Counts.Ghosts; w.StuckUnseen += s.Counts.StuckUnseen;
        w.DensityHidden += s.Counts.DensityHidden; w.DensityShown += s.Counts.DensityShown;
        w.FarCorners += s.Counts.FarCorners;
        w.WallPulled += s.Counts.WallPulled; w.WallRefused += s.Counts.WallRefused; w.WallTargets += s.Counts.WallTargets;
        w.WallFound += s.Counts.WallFound; w.WallMissing += s.Counts.WallMissing;
        w.FloorTargets += s.Counts.FloorTargets; w.FloorSet += s.Counts.FloorSet;
      }
    if (g_IdleSlots)
      for (int k = 0; k < IdleSlotCount; ++k)
      {
        wk::IdleSlot const& s = g_IdleSlots[k];
        if (!s.Key) continue;
        ++idlePlacements;
        idleHid += s.Hid; idleShown += s.Shown;
      }
    long long wcalls = g_WalkCalls.exchange(0), wticks = g_WalkTicks.exchange(0), wmost = g_WalkMaxTicks.exchange(0);
    double wms = wcalls ? 1000.0 * (double)wticks / (double)g_Freq.QuadPart : 0.0;
    float clock = g_LastClock.load();
    int view = g_LastView.load();
    int seconds = clock >= 0.f ? (int)clock : 0;
    util::log::Write("Road traffic: %d walking placements so far; the last minute %lld steps took their rules %.2f ms in "
      "all (%.3f ms the longest) and the game's own walker step in them %.2f ms (%.3f ms the longest); the game's "
      "per-frame update of every crowd placement (its respawns, its step and the rules in it, its render copy) %lld "
      "calls, %.2f ms in all (%.3f ms the longest); so far %lld walkers appeared (%lld of them respawns), %lld walker-steps held back, %lld "
      "sidesteps, %lld put back, %lld corner targets far off their way put back on it, walls: %lld walker-steps pulled "
      "out, %lld not (another walker in the way), %lld corner targets moved out (path segments found %lld, not found "
      "%lld), on the street's height: %lld corner targets set, %lld walkers as they appeared, %lld made to wait unseen, "
      "%lld "
      "shown again, stuck: %lld walked through, %lld waited "
      "unseen; %lld walker steps with no slot free or on another thread, %lld unseen walkers shown again because the "
      "rules couldn't step them. Standing crowds: %d placements, %lld updates. The time of day %s%02d:%02d, the view %s: "
      "the cars' share %.0f%%, the people's %.0f%% (of a raised crowd's members: %.0f%% and %.0f%%); for it so far %lld "
      "cars made unseen and %lld shown again, %lld walkers and %lld, %lld standing people and %lld; the game's respawn "
      "held back %lld times. More by day: %lld crowds' room and %lld placements raised so far (%lld members more)",
      walkPlacements, wcalls, wms, 1000.0 * (double)wmost / (double)g_Freq.QuadPart, gameWalkMs, gameWalkMost, updates,
      updateMs, updateMost, w.Appeared, w.Respawned, w.Held,
      w.Sidesteps, w.Reverted, w.FarCorners, w.WallPulled, w.WallRefused, w.WallTargets, w.WallFound, w.WallMissing, w.FloorTargets, w.FloorSet,
      w.Waiting, w.Revealed, w.Ghosts, w.StuckUnseen, g_WalkBusySteps.load(),
      g_WalkReleased.load(), idlePlacements, g_IdleCalls.load(),
      clock >= 0.f ? "" : "unknown ", (seconds / 3600) % 24, (seconds / 60) % 60,
      view > 0 ? "read" : (view == 0 ? "not read" : "not tried"),
      100.f * (view > 0 ? DayShare(clock, CarsAtNight) : 1.f), 100.f * (view > 0 ? DayShare(clock, PeopleAtNight) : 1.f),
      100.f * (view > 0 ? DayShare(clock, RaisedCarsAtNight) : 1.f),
      100.f * (view > 0 ? DayShare(clock, RaisedPeopleAtNight) : 1.f),
      t.DensityHidden, t.DensityShown, w.DensityHidden, w.DensityShown, idleHid, idleShown, g_RespawnsHeld.load(),
      g_CrowdsRaised.load(), g_PlacementsRaised.load(), g_MembersAdded.load());
  }

  // Once a minute the log lines (whichever hook gets here first; the first one right away)
  void MaybeLog()
  {
    if (!util::DiagnosticsOn())   // the minute lines: diagnostics only
      return;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    long long last = g_LastLog.load(std::memory_order_relaxed);
    if (now.QuadPart - last > 60 * g_Freq.QuadPart && g_LastLog.compare_exchange_strong(last, now.QuadPart))
      Log(false);
  }

  // Once a placement (diagnostics): its route as the rules looked it up in the route table (Routes), so a log tells
  // whether the game's car networks are the ones the table was made from
  std::atomic<uintptr_t> g_RouteSaid[SlotCount];
  void SayRoute(int index, Slot const& s, StepInput const& in)
  {
    if (!s.MostLooked || !util::DiagnosticsOn() || g_RouteSaid[index].load(std::memory_order_relaxed) == s.Key) return;
    g_RouteSaid[index].store(s.Key, std::memory_order_relaxed);
    if (s.RouteRow < 0)
      util::log::Write("Road traffic: a car placement (%d nodes, centroid %.1f, %.1f; %d cars) isn't in the route table: "
        "no route limit", in.NodeCount, s.RouteX, s.RouteZ, in.Count);
    else if (s.Most > 0)
      util::log::Write("Road traffic: a car placement (%d nodes, centroid %.1f, %.1f; %d cars) is route %d of the table: "
        "at most %d of its cars show", in.NodeCount, s.RouteX, s.RouteZ, in.Count, s.RouteRow, s.Most);
    else
      util::log::Write("Road traffic: a car placement (%d nodes, centroid %.1f, %.1f; %d cars) is route %d of the table: "
        "no route limit", in.NodeCount, s.RouteX, s.RouteZ, in.Count, s.RouteRow);
  }

  // ---- The ground check (diagnostics; 2026-11-15, Research section 16): every 10 s the cars and walkers shown near
  // Faith against the road data under them (Resources\GroundRoads.bin, GroundLevel's: the drawn street, and with Solid
  // street what she stands on there), and her own feet against it. One CT.log line. ----
  std::atomic<long long> g_GroundCheckAt{ 0 };
  const float GroundCarsNear = 80.f;      // m round Faith
  const float GroundWalkersNear = 40.f;
  const float GroundSameLevel = 1.5f;     // a road this far under / over is the one it's on (else no road data at its height)

  struct GroundTally
  {
    int Shown = 0, OverRoad = 0, Over5 = 0;
    double SumAbs = 0.0;
    float Most = 0.f, MostX = 0.f, MostZ = 0.f;
    void Add(float x, float y, float z)
    {
      ++Shown;
      float h = 0.f;
      if (!ground::RoadHeightUnder(x, z, y + GroundSameLevel, h) || y - h > GroundSameLevel)
        return;
      float d = y - h;
      ++OverRoad;
      SumAbs += std::fabs(d);
      if (std::fabs(d) > 0.05f) ++Over5;
      if (std::fabs(d) > std::fabs(Most)) { Most = d; MostX = x; MostZ = z; }
    }
    void Say(char* out, size_t size) const
    {
      if (!OverRoad)
        snprintf(out, size, "-");
      else
        snprintf(out, size, "their height over the road %.3f m on average, %d more than 5 cm off, the most %+.2f m (at %.0f, %.0f)",
          SumAbs / OverRoad, Over5, Most, MostX, MostZ);
    }
  };

  void GroundCheck(double now)
  {
    float feet[3];
    if (!ground::LastFeet(feet))
      return;
    GroundTally cars, walkers;
    static Published carPub[MaxMembers];          // (one check at a time: g_GroundCheckAt)
    for (int k = 0; k < SlotCount; ++k)
    {
      Slot const& o = g_Slots[k];
      if (!o.Key || now - o.LastTime > 1.0)        // a placement not stepped (culled): left out
        continue;
      long seq = o.Seq;
      if (seq & 1)
        continue;
      int n = o.PubCount < MaxMembers ? o.PubCount : MaxMembers, got = 0;
      for (int j = 0; j < n; ++j)
      {
        Published p = o.Pub[j];
        float dx = p.X - feet[0], dz = p.Z - feet[2];
        if (dx * dx + dz * dz <= GroundCarsNear * GroundCarsNear)
          carPub[got++] = p;
      }
      if (o.Seq != seq)                           // it changed while read
        continue;
      for (int j = 0; j < got; ++j)
        cars.Add(carPub[j].X, carPub[j].Y, carPub[j].Z);
    }
    static wk::Published walkPub[wk::MaxMembers];
    for (int k = 0; g_WalkSlots && k < WalkSlotCount; ++k)
    {
      wk::Slot const& o = g_WalkSlots[k];
      if (!o.Key || now - o.LastTime > 1.0)
        continue;
      long seq = o.Seq;
      if (seq & 1)
        continue;
      int n = o.PubCount < wk::MaxMembers ? o.PubCount : wk::MaxMembers, got = 0;
      for (int j = 0; j < n; ++j)
      {
        wk::Published p = o.Pub[j];
        float dx = p.X - feet[0], dz = p.Z - feet[2];
        if (dx * dx + dz * dz <= GroundWalkersNear * GroundWalkersNear)
          walkPub[got++] = p;
      }
      if (o.Seq != seq)
        continue;
      for (int j = 0; j < got; ++j)
        walkers.Add(walkPub[j].X, walkPub[j].Y, walkPub[j].Z);
    }
    float under = 0.f;
    char faith[128], carText[192], walkText[192];
    if (ground::RoadHeightUnder(feet[0], feet[2], feet[1] + 0.5f, under) && feet[1] - under <= GroundSameLevel)
      snprintf(faith, sizeof faith, "the road under her at %.2f, her feet %+.2f m from it", under, feet[1] - under);
    else
      snprintf(faith, sizeof faith, "no road data at her height");
    cars.Say(carText, sizeof carText);
    walkers.Say(walkText, sizeof walkText);
    util::log::Write("Road traffic: the ground at Faith (%.1f, %.2f, %.1f; %s): %d cars shown within %.0f m, %d of them "
      "over road data: %s; %d walkers shown within %.0f m, %d of them over road data: %s", feet[0], feet[1], feet[2], faith,
      cars.Shown, GroundCarsNear, cars.OverRoad, carText, walkers.Shown, GroundWalkersNear, walkers.OverRoad, walkText);
  }

  void MaybeGroundCheck()
  {
    if (!util::DiagnosticsOn())
      return;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    long long last = g_GroundCheckAt.load(std::memory_order_relaxed);
    if (now.QuadPart - last > 10 * g_Freq.QuadPart && g_GroundCheckAt.compare_exchange_strong(last, now.QuadPart))
      GroundCheck((double)now.QuadPart / (double)g_Freq.QuadPart);
  }

  void __fastcall hCarStep(__int64 behaviour, __int64 params)
  {
    if (!g_On)
    {
      oCarStep(behaviour, params);
      return;
    }
    LARGE_INTEGER t0, t1;
    QueryPerformanceCounter(&t0);
    StepInput in;
    in.Count = 0;
    in.Now = (double)t0.QuadPart / (double)g_Freq.QuadPart;
    __int64 settings = 0, placement = 0;
    int refusal = RefuseNone;
    double value = 0.0;
    ReadResult got = Read(behaviour, params, in, settings, refusal, placement, value);
    if (got != ReadOk || g_Broken.load(std::memory_order_relaxed))
    {
      if (got == ReadOk || got == ReadNoParams)
        LetGo(in);   // the rules can't step it: the cars they keep unseen back on the road
      if (refusal != RefuseNone)
        NoteRefusal(refusal, placement, value);
      oCarStep(behaviour, params);
      return;
    }
    // the time of day's share (and the crowd test's), and the game's respawn held to it
    {
      float people;
      View view;
      bool known;
      unsigned level = 0;
      bool raised = Raised(placement, &level);
      int mode = g_TestMode.load(std::memory_order_relaxed);
      Shares(in.Density, people, view, known, raised && mode != 1);
      TestShare(in.Density, in.InSightToo, mode, raised, level, in.Count);
      in.ViewKnown = known;
      if (known) { in.ViewX = view.X; in.ViewY = view.Y; in.ViewZ = view.Z; in.ViewAX = view.AX; in.ViewAY = view.AY; in.ViewAZ = view.AZ; }
      HoldRespawns(behaviour, in.Count, Wanted(in.Density, in.Count));
    }
    AcquireSRWLockExclusive(&g_SlotLock);
    Slot* slot = g_Rules.FindSlot(in, g_Slots, SlotCount);
    ReleaseSRWLockExclusive(&g_SlotLock);
    int index = slot ? (int)(slot - g_Slots) : -1;
    if (index < 0 || InterlockedCompareExchange(&g_Busy[index], 1, 0) != 0)
    {
      ++g_BusySteps;
      oCarStep(behaviour, params);   // no room, or this placement's step is already running elsewhere
      return;
    }
    DWORD thread = GetCurrentThreadId();
    if (!g_FirstThread) g_FirstThread = thread;
    else if (thread != g_FirstThread) g_OtherThreads.store(1, std::memory_order_relaxed);
    bool ok = RulesBefore(*slot, in);
    if (ok && slot->Most > 0)
    {
      int wanted = Wanted(in.Density, in.Count);   // and to its route's most (Routes)
      if (slot->Most < wanted) wanted = slot->Most;
      HoldRespawns(behaviour, in.Count, wanted);
    }
    if (ok) SayRoute(index, *slot, in);
    QueryPerformanceCounter(&t1);
    long long ticks = t1.QuadPart - t0.QuadPart;
    if (ok)
      StepWithCopy(behaviour, params, index, settings);
    else
      oCarStep(behaviour, params);
    QueryPerformanceCounter(&t0);
    long long game = t0.QuadPart - t1.QuadPart;
    if (ok) ok = RulesAfter(*slot, in);
    if (!ok)
    {
      // off for the session: this placement's unseen cars shown now, the others' at their next step (LetGo)
      int shown = ReleaseGuarded(*slot, in);
      if (shown) g_Released += shown;
    }
    QueryPerformanceCounter(&t1);
    ticks += t1.QuadPart - t0.QuadPart;
    if (ok && g_TestOn.load(std::memory_order_relaxed))
      NoteShown(g_CarShown[index], g_CarShownAt[index], slot->Visible, in.Count, t1.QuadPart);
    InterlockedExchange(&g_Busy[index], 0);
    g_GameCarAll.fetch_add(game, std::memory_order_relaxed);
    g_RulesCarAll.fetch_add(ticks, std::memory_order_relaxed);
    Most(g_GameCarMost, game);
    if (!ok && !g_Broken.exchange(true))
      Log(true);
    g_Ticks += ticks;
    ++g_Calls;
    long long most = g_MaxTicks.load(std::memory_order_relaxed);
    while (ticks > most && !g_MaxTicks.compare_exchange_weak(most, ticks)) {}
    MaybeLog();
    MaybeGroundCheck();
  }
}

void road_traffic::Init()
{
  QueryPerformanceFrequency(&g_Freq);
  // the time of day's share: the clock, the view and the game's respawn minimum
  g_ClockPointer = util::offsets::GetOffset("OFFSET_TIMEOFDAY");
  g_ViewCode = CodeIs(ViewCallRva, ViewCallBytes, sizeof(ViewCallBytes)) &&
    CodeIs(ViewLookupRva, ViewLookupBytes, sizeof(ViewLookupBytes)) && CodeIs(ViewCopyRva, ViewCopyBytes, sizeof(ViewCopyBytes));
  g_RespawnCode = CodeIs(RespawnMinRva, RespawnMinBytes, sizeof(RespawnMinBytes)) &&
    CodeIs(MinimumWriteRva, MinimumWriteBytes, sizeof(MinimumWriteBytes));
  if (!g_ViewCode || !g_ClockPointer)
    util::log::Warning("Road traffic: %s isn't what the tools know: no time of day's share (all the cars and people show "
      "day and night)", !g_ViewCode ? "the game's view lookup" : "the time of day's offset");
  if (!g_RespawnCode)
    util::log::Warning("Road traffic: the game's respawn minimum isn't what the tools know: its respawns aren't held to the "
      "time of day's share (the rules keep the extra ones unseen)");

  // the road cars
  if (!CodeIs(CarStepRva, CarStepBytes, sizeof(CarStepBytes)))
    util::log::Warning("Road traffic: the game's car step isn't what the tools know: the road cars are left as they are");
  else
  {
    g_Rules.Index.Build();
    g_On = util::hooks::CreateGameHook("RoadTrafficCarStep", Base() + CarStepRva, (PVOID)hCarStep, (LPVOID*)&oCarStep);
    if (g_On)
      util::log::Write("Road traffic: the road cars keep to their lanes and their gaps, on the drawn road's height (%d "
        "road segments' lanes, %d heights along them)", road_traffic_lanes::SegmentCount, road_traffic_heights::FineTotal);
    else
      util::log::Warning("Road traffic: its hook isn't in: the road cars are left as they are");
  }

  // the walking people
  if (!CodeIs(WalkerStepRva, WalkerStepBytes, sizeof(WalkerStepBytes)))
    util::log::Warning("Road traffic: the game's walker step isn't what the tools know: the walking people are left as "
      "they are");
  else
  {
    g_WalkSlots = new (std::nothrow) wk::Slot[WalkSlotCount]();
    g_WalkRules.Walls.Build();   // WalkersWalls.h's index, before the first step
    if (g_WalkSlots)
      g_WalkOn = util::hooks::CreateGameHook("RoadTrafficWalkerStep", Base() + WalkerStepRva, (PVOID)hWalkerStep,
        (LPVOID*)&oWalkerStep);
    if (g_WalkOn)
      util::log::Write("Road traffic: the walking people keep a body apart, step aside for each other and show where "
        "there's room, are kept out of the walls beside their paths (%d path segments, %d samples) and walk on the "
        "street's height (%d walking nodes off it)", walkers_walls::SegmentCount, walkers_walls::SampleCount,
        walkers_floors::NodesCorrected);
    else
      util::log::Warning("Road traffic: the walkers' hook isn't in: the walking people are left as they are");
  }

  // the standing crowds (the time of day's share only)
  if (!CodeIs(UpdateRva, UpdateBytes, sizeof(UpdateBytes)))
    util::log::Warning("Road traffic: the game's crowd update isn't what the tools know: the standing crowds are left as "
      "they are");
  else
  {
    g_IdleSlots = new (std::nothrow) wk::IdleSlot[IdleSlotCount]();
    if (g_IdleSlots)
      g_IdleOn = util::hooks::CreateGameHook("RoadTrafficCrowdUpdate", Base() + UpdateRva, (PVOID)hUpdate, (LPVOID*)&oUpdate);
    if (!g_IdleOn)
      util::log::Warning("Road traffic: the crowd update's hook isn't in: the standing crowds are left as they are");
  }

  // more by day: only with the cars' and the walkers' rules in (they keep the extra ones apart), and before the level
  // (the factories raise the data as the game makes the crowds; a crowd made before isn't touched)
  if (!g_On || !g_WalkOn)
    util::log::Warning("Road traffic: the cars' or the walkers' rules aren't in: the crowds keep the level's members");
  else if (!CodeIs(SwarmCreateRva, SwarmCreateBytes, sizeof(SwarmCreateBytes)) ||
           !CodeIs(SwarmDataLoadRva, DataLoadBytes, sizeof(DataLoadBytes)) ||
           !CodeIs(PlacementCreateRva, PlacementCreateBytes, sizeof(PlacementCreateBytes)) ||
           !CodeIs(PlacementDataLoadRva, DataLoadBytes, sizeof(DataLoadBytes)) ||
           !CodeIs(SwarmCapGetterRva, SwarmCapGetterBytes, sizeof(SwarmCapGetterBytes)) ||
           !CodeIs(PlacementCountGetterRva, PlacementCountGetterBytes, sizeof(PlacementCountGetterBytes)))
    util::log::Warning("Road traffic: the game's crowd factories aren't what the tools know: the crowds keep the level's "
      "members");
  else
  {
    // the crowds' first: a placement raised in a crowd that isn't would only get no members (0x143B97430)
    bool swarms = util::hooks::CreateGameHook("RoadTrafficSwarmCreate", Base() + SwarmCreateRva, (PVOID)hSwarmCreate,
      (LPVOID*)&oSwarmCreate);
    bool placements = swarms && util::hooks::CreateGameHook("RoadTrafficPlacementCreate", Base() + PlacementCreateRva,
      (PVOID)hPlacementCreate, (LPVOID*)&oPlacementCreate);
    if (placements)
      util::log::Write("Road traffic: more by day: %u road cars (the level's %u: the highway's 400 and placement 93's 65 as "
        "they are, the other %u raised to %u), and %u times the level's walking people in the pedestrians', the mid caste's "
        "and the high caste's crowds, as the game makes them (nights as the level's)", Cars, LevelCars, LevelRaisedCars,
        RaisedCars, WalkerRaise);
    else
      util::log::Warning("Road traffic: the crowd factories' hooks aren't in%s: the crowds keep the level's members",
        swarms ? " (only the crowds' room is raised)" : "");
  }
  if (g_ViewCode && g_ClockPointer)
    util::log::Write("Road traffic: the time of day's share: all the cars and people by day, %.0f%% of the cars and %.0f%% "
      "of the people at night (05:00 - 08:00 up, 18:00 - 23:00 down); they go and come out of sight",
      100.f * CarsAtNight, 100.f * PeopleAtNight);
}

// ---- the crowd test (MISC > Performance; render thread: the menu and perf::FrameStart) ----
namespace
{
  const int TestPhases = 3;
  // seconds after each switch (and the start) before a phase is measured: the third longer, as cars coming back need
  // a gap (offline: the raised placements' cars back to 86% of the first phase's in 5 s, 93% in 15 s; the walkers in
  // 2 s; going unseen takes one step)
  const float TestSettle[TestPhases] = { 5.f, 5.f, 10.f };
  const float TestMeasure = 20.f;            // seconds measured a phase
  const char* const TestPhaseNames[TestPhases] = { "1, as set", "2, the level's crowds (the extra cars and walkers unseen, "
    "in sight too)", "3, as set again (the extra ones shown where there's room, in sight too)" };

  struct TestCpu { long long Update, GameCar, GameWalk, RulesCar, RulesWalk; };
  struct TestPhase
  {
    std::vector<float> FrameMs;
    TestCpu From{}, To{};
    double Cars = 0.0, Walkers = 0.0;        // shown, summed over the measured frames
  };
  int g_TestPhase = -1;                      // -1 none; 0 .. TestPhases - 1
  bool g_TestSettling = false;
  long long g_TestSince = 0;                 // when the settling or the measuring began (QueryPerformanceCounter)
  TestPhase g_Test[TestPhases];
  std::string g_TestResult;

  TestCpu CpuNow()
  {
    return TestCpu{ g_UpdateAll.load(), g_GameCarAll.load(), g_GameWalkAll.load(), g_RulesCarAll.load(),
      g_RulesWalkAll.load() };
  }

  // The members shown now: each slot's at its last step, if it stepped in the last 3 s
  void ShownNow(long long now, int& cars, int& walkers)
  {
    long long recent = 3 * g_Freq.QuadPart;
    cars = walkers = 0;
    for (int k = 0; k < SlotCount; ++k)
    {
      long long at = g_CarShownAt[k].load(std::memory_order_relaxed);
      if (at > 0 && now - at < recent) cars += g_CarShown[k].load(std::memory_order_relaxed);
    }
    for (int k = 0; k < WalkSlotCount; ++k)
    {
      long long at = g_WalkShownAt[k].load(std::memory_order_relaxed);
      if (at > 0 && now - at < recent) walkers += g_WalkShown[k].load(std::memory_order_relaxed);
    }
  }

  long long TestNow()
  {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
  }

  // The hooks' mode in each phase
  int TestModeFor(int phase) { return phase == 1 ? 1 : (phase == 2 ? 2 : 0); }

  void TestStop(char const* why)
  {
    g_TestMode.store(0);
    g_TestOn.store(false);
    g_TestPhase = -1;
    g_TestResult = std::string("stopped: ") + why;
    util::log::Write("Crowd test: stopped (%s); the crowds are as set again", why);
  }

  struct TestNumbers
  {
    int Frames = 0;
    double Avg = 0.0, P50 = 0.0, P95 = 0.0, P99 = 0.0, Max = 0.0, Cars = 0.0, Walkers = 0.0;
    double Update = 0.0, GameCar = 0.0, GameWalk = 0.0, RulesCar = 0.0, RulesWalk = 0.0;   // ms a frame
  };

  TestNumbers TestMeasureOf(TestPhase const& p)
  {
    TestNumbers n;
    n.Frames = (int)p.FrameMs.size();
    if (!n.Frames) return n;
    std::vector<float> sorted = p.FrameMs;
    std::sort(sorted.begin(), sorted.end());
    double sum = 0.0;
    for (float v : sorted) sum += v;
    n.Avg = sum / n.Frames;
    auto at = [&](double q) { return (double)sorted[(std::min)((size_t)(q * (n.Frames - 1) + 0.5), sorted.size() - 1)]; };
    n.P50 = at(0.5); n.P95 = at(0.95); n.P99 = at(0.99); n.Max = sorted.back();
    n.Cars = p.Cars / n.Frames;
    n.Walkers = p.Walkers / n.Frames;
    double perFrame = 1000.0 / (double)g_Freq.QuadPart / n.Frames;
    n.Update = (p.To.Update - p.From.Update) * perFrame;
    n.GameCar = (p.To.GameCar - p.From.GameCar) * perFrame;
    n.GameWalk = (p.To.GameWalk - p.From.GameWalk) * perFrame;
    n.RulesCar = (p.To.RulesCar - p.From.RulesCar) * perFrame;
    n.RulesWalk = (p.To.RulesWalk - p.From.RulesWalk) * perFrame;
    return n;
  }

  void TestFinish()
  {
    TestNumbers n[TestPhases];
    for (int p = 0; p < TestPhases; ++p) n[p] = TestMeasureOf(g_Test[p]);
    util::log::Write("Crowd test (MISC > Performance), %.0f s measured a phase after %.0f, %.0f and %.0f s to settle:",
      TestMeasure, TestSettle[0], TestSettle[1], TestSettle[2]);
    for (int p = 0; p < TestPhases; ++p)
      util::log::Write("  Phase %s: %d frames, %.1f fps; frame %.2f ms (median %.2f, 95 %% %.2f, 99 %% %.2f, longest %.2f); "
        "shown on average %.0f road cars and %.0f walking people (the rules' placements that stepped in the last 3 s); the "
        "crowds' CPU a frame (summed over the threads they ran on): the game's per-frame update of every crowd placement "
        "%.3f ms, in it the game's car step %.3f, its walker step %.3f, the rules %.3f (cars) and %.3f (walkers)",
        TestPhaseNames[p], n[p].Frames, n[p].Avg > 0.0 ? 1000.0 / n[p].Avg : 0.0, n[p].Avg, n[p].P50, n[p].P95, n[p].P99,
        n[p].Max, n[p].Cars, n[p].Walkers, n[p].Update, n[p].GameCar, n[p].GameWalk, n[p].RulesCar, n[p].RulesWalk);
    if (!n[0].Frames || !n[1].Frames || !n[2].Frames)
    {
      g_TestResult = "a phase had no frames (a loading screen?): run it again";
      util::log::Write("  %s", g_TestResult.c_str());
      return;
    }
    double asSet = 0.5 * (n[0].Avg + n[2].Avg), level = n[1].Avg, cost = asSet - level;
    char summary[768];
    sprintf_s(summary, "With the crowds as set the frame took %.2f ms (%.1f fps) on average (phases 1 and 3), with the "
      "level's %.2f ms (%.1f fps): the extra cars and walkers cost about %.2f ms a frame (%s). Phases 1 and 3 differ by "
      "%.2f ms, how much the frame time wanders by itself: a smaller difference than that isn't the crowds'. Shown: %.0f "
      "and %.0f road cars, %.0f and %.0f walking people (as set, the level's). In phase 2 the crowds' room stays raised "
      "and the game's car step still moves the unseen cars (that's in its CPU numbers).", asSet, 1000.0 / asSet, level,
      1000.0 / level, cost, cost > 0.0 ? "slower with them" : "no slower with them", std::fabs(n[0].Avg - n[2].Avg),
      0.5 * (n[0].Cars + n[2].Cars), n[1].Cars, 0.5 * (n[0].Walkers + n[2].Walkers), n[1].Walkers);
    g_TestResult = summary;
    util::log::Write("  %s", summary);
  }
}

void road_traffic::CrowdTestFrame(float ms)
{
  if (g_TestPhase < 0) return;
  if (perf::TestRunning())
  {
    TestStop("the other performance test started");
    return;
  }
  long long now = TestNow();
  double since = (double)(now - g_TestSince) / (double)g_Freq.QuadPart;
  TestPhase& p = g_Test[g_TestPhase];
  if (g_TestSettling)
  {
    if (since < TestSettle[g_TestPhase]) return;
    g_TestSettling = false;
    g_TestSince = now;
    p.From = CpuNow();
    return;
  }
  p.FrameMs.push_back(ms);
  int cars = 0, walkers = 0;
  ShownNow(now, cars, walkers);
  p.Cars += cars;
  p.Walkers += walkers;
  if (since < TestMeasure) return;
  p.To = CpuNow();
  if (++g_TestPhase >= TestPhases)
  {
    g_TestMode.store(0);
    g_TestOn.store(false);
    g_TestPhase = -1;
    TestFinish();
    return;
  }
  g_TestMode.store(TestModeFor(g_TestPhase));
  g_TestSettling = true;
  g_TestSince = now;
  util::log::Write("Crowd test: phase %s", TestPhaseNames[g_TestPhase]);
}

bool road_traffic::CrowdTestRunning()
{
  return g_TestPhase >= 0;
}

void road_traffic::DrawCrowdTest()
{
  if (g_TestPhase < 0)
  {
    if (g_PlacementsRaised.load() == 0)
      ImGui::TextDisabled("Crowd test: no crowd has been raised this session (the road cars and walkers are the level's)");
    else if (perf::TestRunning())
      ImGui::TextDisabled("Crowd test: waits for the test above to end");
    else if (ImGui::Button("Run a crowd test (80 s)"))
    {
      for (int p = 0; p < TestPhases; ++p) g_Test[p] = TestPhase();
      g_TestResult.clear();
      g_TestOn.store(true);
      g_TestMode.store(TestModeFor(0));
      g_TestPhase = 0;
      g_TestSettling = true;
      g_TestSince = TestNow();
      util::log::Write("Crowd test: phase %s (%.0f s to settle, then %.0f s measured)", TestPhaseNames[0], TestSettle[0],
        TestMeasure);
    }
    ImGui::HelpMarker("What the extra road cars and walking people cost the frame (the tools give the city 2000 road cars, "
      "the level 1210, and twice its walking people). Three phases: as set, then the level's crowds (the extra ones go "
      "unseen at once, in sight too), then as set again (they show again where there's room). Each is measured for 20 s "
      "after 5 s to settle (the last after 10 s). Stand still in a busy street by day and keep the camera still; the menu "
      "can be closed once it's started. The numbers go to CT.log and show here.");
  }
  else
  {
    double elapsed = (double)(TestNow() - g_TestSince) / (double)g_Freq.QuadPart;
    double left = (g_TestSettling ? TestSettle[g_TestPhase] - elapsed + TestMeasure : TestMeasure - elapsed);
    ImGui::TextColored(ImVec4(1.f, 0.8f, 0.2f, 1.f), "Crowd test: phase %d of %d (%s), %.0f s left in it", g_TestPhase + 1,
      TestPhases, g_TestPhase == 1 ? "the level's crowds" : "as set", left > 0.0 ? left : 0.0);
  }
  if (!g_TestResult.empty())
    ImGui::TextWrapped("Last crowd test: %s", g_TestResult.c_str());
}
