#pragma once

// Road traffic (2026-11-03; Research_Road_Traffic_2026-11-03.md section 7): the rules the tools add to the game's own
// car step (0x143B8B0D0), with nothing of the game in here, so the same code runs in the DLL and in the offline test
// that drives the city's car network with a copy of the game's step. Per car placement, after the game's step:
// - each car follows its lane: the road segment's lane samples (RoadTrafficLanes.h: on the carriageway's tarmac),
//   joined at the nodes and rounded by a look-ahead along them, instead of chasing the next corner at a capped turn
//   rate; the game's own speed choice is kept as the most it may go;
// - a car never gets closer than a bumper gap to a car in its way (its own placement's, and the other car
//   placements' latest), slowing before it; of two crossing cars the one nearer the meeting point goes;
// - a car that would show inside another (two placements' cars meeting) waits unseen where it is until there's room,
//   a car the game respawns starts unseen and shows once its spawn point is clear, and one on a segment with no road
//   under it drives it unseen;
// - the time of day's share of the cars shows (StepInput::Density, DayShare): above it, shown cars out of sight go
//   unseen and drive on unseen; below it, unseen ones out of sight show again where they're in a gap (OutOfSight).
// The game's record of a car (0x24 bytes): position, heading (half, degrees), side offset s (half), the corner it
// drives to, from / to / next node, speed factor (half). Its node array: 0x30 bytes a node, position at + 0, radius
// at + 0x14.
#include "RoadTrafficLanes.h"
#include "RoadTrafficHeights.h"
#include <cmath>
#include <cstdint>
#include <cstring>
#ifdef ROAD_TRAFFIC_PROFILE
#include <intrin.h>
#define RT_TICK(var) var = __rdtsc()
#define RT_ADD(field, since) s.Counts.field += (long long)(__rdtsc() - since)
#else
#define RT_TICK(var) (void)0
#define RT_ADD(field, since) (void)0
#endif

namespace road_traffic_core
{
  // ---- the game's layouts ----
  const int RecordSize = 0x24;
  const int NodeSize = 0x30;
  const int NodeRadius = 0x14;
  const unsigned short DespawnedFlag = 8;         // a member's flag bit 3: despawned (the game puts it at y -10042)
  // Where the rules' unseen cars go: under the world but above -10042, the game's despawn height (0x143B97257). The
  // game's per-frame update (0x143B9B7E0, 0x143B9B9F1) keeps the node lists only for members above -10042: there every
  // member is in the group of the node the list map says, and the game's list move (0x143B9C070) relies on it. At
  // -10042 an unseen respawn was never listed (still in the 'none' group); despawned there, the move had 'none' at both
  // ends and wrote it over another member's entry, whose next move then shifted memory 4 bytes up without end (the crash
  // of 2026-11-03 13:47:41, build 7BFAC1779936; seen live: a member twice and one missing, an array shifted). Above
  // -10042 the placement's box reaches down to the unseen cars, but it still holds every shown one.
  const float HiddenY = -10000.f;

  // ---- the rules' sizes ----
  const float HalfLength = 2.3f;     // the sport car is 4.40 m (z -2.29 .. 2.11), the truck 3.72, the motorbike 2.0
  const float HalfWidth = 1.0f;      // the sport car 1.92 m
  const float LaneMargin = 0.3f;     // to the side of a car in the way
  const float OncomingMargin = 0.05f;   // to the side of one coming the other way (two one-way node lines of a street can
                                     // be 2.4 m apart: with LaneMargin those cars met head on and waited for each other)
  const float HardGap = 1.0f;        // least bumper gap, ever
  const float StopGap = 2.5f;        // bumper gap a car stops at behind another
  const float Braking = 5.f;         // m/s^2 the rules plan with
  const float PathAhead = 16.f;      // metres of lane looked at ahead of a car (look-ahead 8 m, a step 2.5 m at most)
  const int MaxMembers = 1024;       // a placement's members the rules look after (the city's largest has 400)
  const int MaxSlots = 48;           // car placements at once (the city has 21)
  const int MaxForeign = 2048;       // other placements' cars looked at in one step
  const float OldSeconds = 0.5f;     // another placement's cars older than this are left out
  const float Height = 3.f;          // cars further apart than this in height aren't in each other's way (bridges)
  const float WaitBonus = 3.f;       // metres nearer the meeting point a second of waiting counts as
  const float MinRadius = 4.f;       // the tightest a shown car turns (a small car's 10 m turning circle, at its middle)
  const float Cell = 16.f;           // the neighbour grid's cells
  const int GridBuckets = 2048;
  const float FarAway = 400.f;       // out of sight for the time of day's share: further from the view than this (a car
  const float SideNear = 25.f;       // is ~10 px at 1080p), or further than this and off to the view's side (OutOfSight)
  const int DensityPerStep = 2;      // cars the share hides in one step, at most
  const float PutBackSeconds = 6.f;  // a shown car whose move the final check puts back every step for this long waits
                                     // unseen (as one held beside a car that doesn't move either, SideTime)
  // never a jam (2026-11-13; Research_Road_Traffic_2026-11-03.md section 15)
  const float StandMove = 1.f;       // a shown car that hasn't got this far from where it stopped is standing
  const float CycleSeconds = 3.f;    // cars standing this long, each held back by the next and the last by the first (in
                                     // any placements, BreakCycles): one of them waits unseen
  const int CycleHops = 64;          // cars a wait cycle is looked for along, at most
  const float StandOutSeconds = 15.f;  // a shown car standing this long out of sight waits unseen, whatever holds it (a
                                     // red light holds one 12 s at most) ...
  const float StandInSeconds = 20.f;  // ... or this long in sight (StandingOut)
  const float StandEvery = 1.f;      // seconds between two of those in one placement, at least
  const float UnshowSeconds = 2.f;   // one made unseen by those drives on unseen this long at least

  // The time of day's share of a crowd shown (cars, walkers, idle people): all of it in the day, `night` of it at
  // night, eased in between (05:00 - 08:00 up, 18:00 - 23:00 down). clock: the game's time of day in seconds, 0 midnight
  // (the tools' menu shows it as hh:mm, seconds / 3600); anything else (unknown) is day.
  inline float DayShare(float clock, float night)
  {
    if (!(clock >= 0.f && clock <= 86400.f)) return 1.f;
    float h = clock / 3600.f;
    float t;
    if (h < 5.f || h >= 23.f) t = 0.f;
    else if (h < 8.f) t = (h - 5.f) / 3.f;
    else if (h < 18.f) t = 1.f;
    else t = 1.f - (h - 18.f) / 5.f;
    t = t * t * (3.f - 2.f * t);
    return night + (1.f - night) * t;
  }

  // Out of sight from a view at (vx, vy, vz) with axis (ax, ay, az) (the view's forward row, whichever way along it the
  // view looks: not relied on): further than `far`, or further than `near` and more than ~69 degrees off the axis both
  // ways (wider than any field of view the game or the tools give: 16:9 at 70 degrees high is 51 across, half)
  inline bool OutOfSight(float x, float y, float z, float vx, float vy, float vz, float ax, float ay, float az, float far, float near)
  {
    float dx = x - vx, dy = y - vy, dz = z - vz;
    float d2 = dx * dx + dy * dy + dz * dz;
    if (d2 > far * far) return true;
    if (d2 < near * near) return false;
    float c = (dx * ax + dy * ay + dz * az) / std::sqrt(d2);
    return std::fabs(c) < 0.35f;
  }

  // How many of `count` members a share wants shown (at least one: a placement with none shown would put its box under
  // the city, see HiddenY)
  inline int Wanted(float share, int count)
  {
    float d = share < 0.f ? 0.f : (share > 1.f ? 1.f : share);
    int w = (int)std::ceil(d * (float)count - 1e-4f);
    return w < 1 ? 1 : (w > count ? count : w);
  }

  // The most cars a placement shows, by its route: the city's car networks (data: network.txt, from the game's own) by
  // node count and node centroid (x, z), and how many cars their lanes hold flowing. A placement with more members
  // shows that many and the rest drive on unseen, as the time of day's share's. 0: no limit. (2026-11-12, the
  // roundabout by the station: placement 79's closed loop held its 82 cars nose to tail, 8.0 m a car, and all of them
  // crawled; Research section 15)
  struct Route { int Nodes; float X, Z; int Most; };
  const Route Routes[] = {
    { 38, -78.239f, -37.307f, 90 },   // placements 17 and 180 (one network)
    { 18, 327.738f, 587.136f, 0 },   // placement 21
    { 25, -99.328f, 312.746f, 65 },   // placement 79
    { 18, -113.491f, 537.519f, 0 },   // placement 83
    { 62, 352.409f, 262.890f, 45 },   // placement 93
    { 28, 455.445f, 567.763f, 51 },   // placement 98
    { 16, 83.190f, 112.797f, 0 },   // placement 102
    { 16, -175.549f, 850.728f, 0 },   // placement 104
    { 35, -332.336f, 114.217f, 0 },   // placement 118
    { 33, 542.618f, -133.880f, 0 },   // placement 126
    { 31, 306.155f, 480.462f, 0 },   // placement 136
    { 14, 77.367f, 431.705f, 0 },   // placement 151
    { 33, 47.313f, -69.818f, 0 },   // placement 163
    { 28, -416.278f, -165.575f, 0 },   // placement 186
    { 24, -33.090f, 377.366f, 55 },   // placement 188
    { 11, 175.629f, 519.792f, 31 },   // placement 190
    { 9, -32.636f, 552.907f, 0 },   // placement 222
    { 43, -296.431f, 265.059f, 69 },   // placement 235
    { 23, 351.222f, 526.679f, 0 },   // placement 244
    { 12, 130.957f, 981.624f, 0 },   // placement 254
  };
  const int RouteCount = (int)(sizeof(Routes) / sizeof(Routes[0]));
  const float RouteNear = 0.5f;      // metres a centroid may be off the table's

  // ---- half floats ----
  inline float HalfToFloat(uint16_t h)
  {
    uint32_t sign = (uint32_t)(h & 0x8000) << 16, exp = (h >> 10) & 0x1F, man = h & 0x3FF, bits;
    if (exp == 0)
    {
      if (man == 0) bits = sign;
      else
      {
        exp = 127 - 15 + 1;
        while (!(man & 0x400)) { man <<= 1; --exp; }
        bits = sign | (exp << 23) | ((man & 0x3FF) << 13);
      }
    }
    else if (exp == 31) bits = sign | 0x7F800000 | (man << 13);
    else bits = sign | ((exp + 127 - 15) << 23) | (man << 13);
    float f;
    memcpy(&f, &bits, 4);
    return f;
  }

  inline uint16_t FloatToHalf(float f)
  {
    uint32_t x;
    memcpy(&x, &f, 4);
    uint32_t sign = (x >> 16) & 0x8000;
    int exp = (int)((x >> 23) & 0xFF) - 127 + 15;
    uint32_t man = x & 0x7FFFFF;
    if (((x >> 23) & 0xFF) == 0xFF) return (uint16_t)(sign | 0x7C00 | (man ? 0x200 : 0));
    if (exp >= 31) return (uint16_t)(sign | 0x7C00);
    if (exp <= 0)
    {
      if (exp < -10) return (uint16_t)sign;
      man |= 0x800000;
      uint32_t shift = (uint32_t)(14 - exp);
      uint32_t half = man >> shift;
      if ((man >> (shift - 1)) & 1) ++half;
      return (uint16_t)(sign | half);
    }
    uint32_t half = sign | ((uint32_t)exp << 10) | (man >> 13);
    if (man & 0x1000) ++half;        // to nearest (a carry into the exponent is right)
    return (uint16_t)half;
  }

  // ---- the game's record ----
  struct Record
  {
    float Pos[3];
    uint16_t Heading;      // degrees, half (0 = +z, 90 = +x)
    uint16_t Side;         // s, half
    float Target[3];
    uint16_t From, To, Next;
    uint16_t Speed;        // 0 - 1, half
  };
  static_assert(sizeof(Record) == RecordSize, "the game's member record is 0x24 bytes");

  // The game's cruise speeds (0x143B95900): min + (max - min) x this, by the member's index modulo 12
  const float SpeedTable[12] = { 0.2f, 0.f, 0.4f, 0.1f, 0.6f, 0.3f, 0.8f, 0.5f, 0.95f, 0.7f, 1.f, 0.85f };

  inline float Len2(float x, float z) { return std::sqrt(x * x + z * z); }
  inline float Clamp(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

  // The two footprints (centre, unit forward) overlap, each half size grown by inflate (separating axes)
  inline bool Overlap(float ax, float az, float adx, float adz, float bx, float bz, float bdx, float bdz, float inflate)
  {
    float axes[4][2] = { { adx, adz }, { -adz, adx }, { bdx, bdz }, { -bdz, bdx } };
    float hl = HalfLength + inflate, hw = HalfWidth + inflate;
    float rx = bx - ax, rz = bz - az;
    for (int i = 0; i < 4; ++i)
    {
      float ux = axes[i][0], uz = axes[i][1];
      float d = std::fabs(rx * ux + rz * uz);
      float ra = hl * std::fabs(adx * ux + adz * uz) + hw * std::fabs(-adz * ux + adx * uz);
      float rb = hl * std::fabs(bdx * ux + bdz * uz) + hw * std::fabs(-bdz * ux + bdx * uz);
      if (d > ra + rb) return false;
    }
    return true;
  }

  // ---- the lane table's lookup (node positions to segment) ----
  struct LaneIndex
  {
    static const int Buckets = 4096;
    int Head[Buckets];
    int Next[road_traffic_lanes::SegmentCount];
    float Point[road_traffic_lanes::SampleCount][2][3];   // each sample's lane points (lane 0, lane 1) in the world
    bool Built = false;

    static unsigned Hash(float x, float z)
    {
      int qx = (int)std::floor(x), qz = (int)std::floor(z);
      return ((unsigned)qx * 73856093u ^ (unsigned)qz * 19349663u) % Buckets;
    }

    void Build()
    {
      for (int i = 0; i < Buckets; ++i) Head[i] = -1;
      for (int s = 0; s < road_traffic_lanes::SegmentCount; ++s)
      {
        unsigned h = Hash(road_traffic_lanes::Segments[s].From[0], road_traffic_lanes::Segments[s].From[2]);
        Next[s] = Head[h];
        Head[h] = s;
        road_traffic_lanes::Segment const& g = road_traffic_lanes::Segments[s];
        float dx = g.To[0] - g.From[0], dz = g.To[2] - g.From[2], len = std::sqrt(dx * dx + dz * dz);
        float px = len > 1e-4f ? dz / len : 0.f, pz = len > 1e-4f ? -dx / len : 0.f;
        for (int k = 0; k < g.Count; ++k)
        {
          float t = g.Count > 1 ? (float)k / (float)(g.Count - 1) : 0.f;
          for (int lane = 0; lane < 2; ++lane)
          {
            float o = road_traffic_lanes::Samples[g.First + k][lane];
            Point[g.First + k][lane][0] = g.From[0] + dx * t + px * o;
            Point[g.First + k][lane][1] = road_traffic_heights::SampleY[g.First + k][lane];   // the road's there
            Point[g.First + k][lane][2] = g.From[2] + dz * t + pz * o;
          }
        }
      }
      Built = true;
    }

    // The table's segment from a to b (positions as the game's nodes have them, within 2 cm), -1 for none
    int Find(float const* a, float const* b) const
    {
      for (int dx = -1; dx <= 1; ++dx)
        for (int dz = -1; dz <= 1; ++dz)
        {
          unsigned h = Hash(a[0] + dx * 0.05f, a[2] + dz * 0.05f);
          for (int s = Head[h]; s >= 0; s = Next[s])
          {
            road_traffic_lanes::Segment const& g = road_traffic_lanes::Segments[s];
            if (std::fabs(g.From[0] - a[0]) < 0.02f && std::fabs(g.From[1] - a[1]) < 0.02f && std::fabs(g.From[2] - a[2]) < 0.02f &&
                std::fabs(g.To[0] - b[0]) < 0.02f && std::fabs(g.To[1] - b[1]) < 0.02f && std::fabs(g.To[2] - b[2]) < 0.02f)
              return s;
          }
        }
      return -1;
    }
  };

  // ---- what the rules keep ----
  struct Member
  {
    float LastX, LastZ;       // where the rules left it (x, z)
    float Speed;              // m/s it moved last step
    float Wait;               // seconds it's been held back (it goes first sooner the longer it waits)
    float BlockedTime;        // seconds held still by a car that's waiting for it too
    float SideTime;           // seconds held still beside a car that doesn't move either
    float PutBackTime;        // seconds its move has been put back by the final check, step after step
    float LaneY;              // the height of its lane where it is (a car driving unseen gets it back for the game's step)
    uint32_t Age;             // steps since it appeared
    uint16_t PrevNode;        // the node before its from node, 0xFFFF unknown
    uint8_t Alive;            // seen not despawned last step
    uint8_t Hidden;           // 0 seen, 1 unseen (it drives its lane unseen until it's in a gap, then shows), 2 unseen
                              // for the time of day's share (shows when the share has room, out of sight)
    uint8_t Despawned;        // despawned at its last step (when it's there again, the game has respawned it)
    uint8_t HeldMutual;      // what held it back last step (for the logs): a car waiting for it too
    uint32_t HeldBy;          // that car's id (0 none)
    float HeldGap;            // and its bumper gap
    uint32_t SideBy;          // the car beside it it couldn't move past last step (for the logs; 0 none)
    float SideSpeed;          // that car's speed
    float HeadingDeg;         // its heading as the rules left it (the record's half float is 0.06 - 0.25 degrees coarse)
    uint8_t HeadingSet;       // HeadingDeg is the rules'
    float AnchorX, AnchorZ;   // where it stopped (StandMove; 1e30 none)
    float StandTime;          // seconds shown and standing there
    uint32_t Blocker;         // the car that held it back this step (put back > footprint > gap; 0 none: free, or the
                              // game's own step stopped it, as at a red light)
    float Unshow;             // seconds it drives on unseen before it may show (made unseen by BreakCycles, StandingOut)
  };

  struct Published            // a car as the other placements see it
  {
    float X, Y, Z, DirX, DirZ, Speed, Wait;
    uint32_t Id;
    uint32_t Blocker;         // its Blocker
    float Stand;              // its StandTime
  };

  struct Stats
  {
    long long Steps = 0, Cars = 0, Waiting = 0, Unseen = 0, Held = 0, Reverted = 0, Appeared = 0, NoLane = 0, Revealed = 0;
    long long Respawned = 0;  // respawns that started unseen
    long long TicksGather = 0, TicksPath = 0, TicksAllowed = 0, TicksMove = 0, TicksFinal = 0;   // ROAD_TRAFFIC_PROFILE only
    long long LaneFound = 0, LaneMissing = 0;   // segments looked up in the lane table: found, not (the game's own lane)
    long long DensityHidden = 0, DensityShown = 0;   // made unseen / shown again for the time of day's share
    long long PutBackHidden = 0;   // made unseen after their moves were put back for PutBackSeconds
    long long CycleHidden = 0, CycleCross = 0;   // made unseen to break a wait cycle; of them, cycles over 2+ placements
    long long StandHidden = 0, StandHiddenInSight = 0;   // made unseen after standing (StandingOut); of them in sight
    long long CapHidden = 0, CapHiddenInSight = 0;   // made unseen at a placement's first step, above its route's most
                                   // (Routes); of them in sight (its first step: none of its cars drawn yet, expected)
  };

  struct Slot
  {
    volatile long Seq;              // even while Pub is stable (a seqlock)
    uintptr_t Key;                  // the placement
    uintptr_t Records, Nodes;       // its records' and nodes' addresses and counts: a change starts it again
    int Count, NodeCount;
    double LastTime;                // seconds (the caller's clock) of its last step
    float MinX, MinY, MinZ, MaxX, MaxY, MaxZ;   // its published cars' box
    int PubCount;
    Published Pub[MaxMembers];
    Member M[MaxMembers];
    Record Before[MaxMembers];      // the records as the game's step got them
    // segment lookups by node pair: key from << 16 | to; the table's segment, -1 none
    static const int CacheSize = 2048;
    uint32_t CacheKey[CacheSize];
    int CacheValue[CacheSize];
    // this step: each own car's position and way (before it moves: where it was), seen or not; the others' cars
    float NewX[MaxMembers], NewY[MaxMembers], NewZ[MaxMembers], NewDX[MaxMembers], NewDZ[MaxMembers];
    float WayX[MaxMembers], WayZ[MaxMembers];   // its way before the step
    float StartDeg[MaxMembers];   // its heading (degrees) before the step
    uint8_t Visible[MaxMembers];
    Published Foreign[MaxForeign];
    int ForeignCount;
    // the neighbour grid (own cars where they were, then the others' cars after them)
    int GridHead[GridBuckets];
    int GridNext[MaxMembers + MaxForeign];
    float GridX[MaxMembers + MaxForeign], GridZ[MaxMembers + MaxForeign];
    int GridCX[MaxMembers + MaxForeign], GridCZ[MaxMembers + MaxForeign];   // its cell
    int DensityNext;                // where the time of day's share looks next
    uint32_t IdFirst;               // its cars' ids: IdFirst + member (by the slot: (slot + 1) << 10, set when it's taken)
    int Most;                       // the most cars it shows (Routes; 0 no limit)
    uint8_t MostLooked;             // Most looked up (Before)
    uint8_t Fresh;                  // its first step is to come (the share hides above Most in sight too, that once)
    float RouteX, RouteZ;           // its nodes' centroid (for the logs)
    int RouteRow;                   // its row in Routes (-1 none; for the logs)
    double StandNext;               // in.Now before which StandingOut leaves it
    uint8_t CycleState[MaxMembers]; // BreakCycles: 0 not looked at, 1 on the walk, 2 done this step
    static const int ForeignBuckets = 4096;
    int ForeignAt[ForeignBuckets];  // the others' cars by id this step (index + 1, 0 empty), built when a walk needs it
    uint8_t ForeignAtBuilt;
    Stats Counts;                   // only its own thread writes them (one step of a placement at a time)
  };

  // A slot's first car id: its cars are IdBase + member, unique over the slots (MaxMembers is 1 << 10). (Until
  // 2026-11-13 from the placement's address, (Key >> 4) * 1024 in 32 bits: two placements could share a range.)
  inline uint32_t IdBase(Slot const& s) { return s.IdFirst; }

  // ---- the lane path of one car ----
  struct Path
  {
    static const int MaxPoints = 48;
    float X[MaxPoints], Y[MaxPoints], Z[MaxPoints];
    int Seg[MaxPoints];            // the lane table's segment the point is on (-1 none) and how far along it (0 - 1)
    float T[MaxPoints];
    int Count = 0;
    int Lane = 0;                  // the car's lane (0, 1)
    bool OffRoad = false;          // the car's segment has no road under it
    float CarArc = 0.f;            // where along it the car was before the step
    float EndX = 0.f, EndY = 0.f, EndZ = 0.f;   // its lane's point at the to node

    void Add(float x, float y, float z, int seg = -1, float t = 0.f)
    {
      if (Count > 0 && std::fabs(X[Count - 1] - x) < 0.01f && std::fabs(Z[Count - 1] - z) < 0.01f) return;
      if (Count < MaxPoints) { X[Count] = x; Y[Count] = y; Z[Count] = z; Seg[Count] = seg; T[Count] = t; ++Count; }
    }
  };

  // Everything one step of one placement needs
  struct StepInput
  {
    uintptr_t Key;            // the placement
    uint8_t* Records;         // its first member's record
    uint16_t* Flags;          // its first member's flag
    int Count;
    uint8_t const* Nodes;     // its node array
    int NodeCount;
    float SpeedMin, SpeedMax; // the settings' SpeedRange
    float Dt;                 // the step's seconds
    double Now;               // a clock in seconds (for the other placements' cars' age)
    float Density = 1.f;      // the share of its cars to show (0 - 1; DayShare)
    bool ViewKnown = false;   // the view: where it is and its axis (OutOfSight); unknown, the share hides and shows none
    float ViewX = 0.f, ViewY = 0.f, ViewZ = 0.f, ViewAX = 0.f, ViewAY = 0.f, ViewAZ = 0.f;
    bool InSightToo = false;  // the crowd test (MISC > Performance) runs: the share hides and shows in sight too, as
                              // many a step as it takes
  };

  class Rules
  {
  public:
    LaneIndex Index;               // Build() it once before the first step

    // The slot of a placement (the caller keeps the slots and locks round this)
    Slot* FindSlot(StepInput const& in, Slot* storage, int storageCount)
    {
      Slot* empty = nullptr;
      Slot* oldest = nullptr;
      for (int i = 0; i < storageCount; ++i)
      {
        Slot& s = storage[i];
        if (s.Key == in.Key) return &s;
        if (s.Key == 0 && !empty) empty = &s;
        if (s.Key != 0 && in.Now - s.LastTime > 10.0 && (!oldest || s.LastTime < oldest->LastTime)) oldest = &s;
      }
      Slot* s = empty ? empty : oldest;
      if (!s) return nullptr;
      s->IdFirst = ((uint32_t)(s - storage) + 1u) << 10;
      Reset(*s, in);
      return s;
    }

    // A placement's slot if it has one (none made)
    static Slot* FindExisting(uintptr_t key, Slot* storage, int storageCount)
    {
      for (int i = 0; i < storageCount; ++i)
        if (storage[i].Key == key) return &storage[i];
      return nullptr;
    }

    // The rules can't go on with this placement and the game's step goes on alone: its cars the rules keep unseen go
    // back on their lane's height (or their corner's), shown where they are, or the game's step would lift them from
    // under the world along the line to their corner. Returns how many.
    int Release(Slot& s, StepInput const& in)
    {
      if ((uintptr_t)in.Records != s.Records || in.Count != s.Count) return 0;
      Record* rec = (Record*)in.Records;
      int shown = 0;
      for (int i = 0; i < in.Count; ++i)
      {
        Member& m = s.M[i];
        if (!m.Alive || !m.Hidden || (in.Flags[i] & DespawnedFlag) || !(rec[i].Pos[1] < -9000.f)) continue;
        rec[i].Pos[1] = m.LaneY > -9000.f ? m.LaneY : rec[i].Target[1];
        m.Hidden = 0;
        m.Unshow = 0.f;
        ++shown;
      }
      return shown;
    }

    // Before the game's step: what each car was
    void Before(Slot& s, StepInput const& in)
    {
      if ((uintptr_t)in.Records != s.Records || in.Count != s.Count || (uintptr_t)in.Nodes != s.Nodes || in.NodeCount != s.NodeCount)
        Reset(s, in);
      if (!s.MostLooked)
      {
        s.Most = RouteMost(in, s.RouteX, s.RouteZ, s.RouteRow);
        s.MostLooked = 1;
      }
      Record* rec = (Record*)in.Records;
      for (int i = 0; i < in.Count; ++i)
        if (s.M[i].Alive && s.M[i].Hidden && !(in.Flags[i] & DespawnedFlag) && rec[i].Pos[1] < -9000.f && s.M[i].LaneY > -9000.f)
          rec[i].Pos[1] = s.M[i].LaneY;
      memcpy(s.Before, in.Records, (size_t)in.Count * RecordSize);
    }

    // After the game's step: the rules (others: every slot, this one too; read under their seqlock)
    void After(Slot& s, StepInput const& in, Slot* others, int otherCount)
    {
      ++s.Counts.Steps;
      unsigned long long t0 = 0, t1 = 0;
      (void)t0; (void)t1;
      RT_TICK(t0);
      GatherForeign(s, in, others, otherCount);
      Record* rec = (Record*)in.Records;

      // who's there, seen or not, and where each was before the step
      for (int i = 0; i < in.Count; ++i)
      {
        Member& m = s.M[i];
        s.Visible[i] = 0;
        if (in.Flags[i] & DespawnedFlag) { m.Alive = 0; m.Hidden = 0; m.Despawned = 1; continue; }
        Record const& b = s.Before[i];
        bool appeared = !m.Alive || std::fabs(b.Pos[0] - m.LastX) > 0.5f || std::fabs(b.Pos[2] - m.LastZ) > 0.5f;
        if (appeared)
        {
          ++s.Counts.Appeared;
          // one the game respawned (it was despawned at its last step) starts unseen and shows where there's room:
          // the game respawns before its step (0x143B9CBE0 in 0x143B9B7E0) and draws after it, so it's never drawn
          // where it came in. The others (a placement's cars at its first step) stay as they are, unless inside
          // another (below): all of them unseen at once would put the placement's box under the city
          bool respawned = m.Despawned != 0;
          m.Alive = 1;
          m.Despawned = 0;
          m.Age = 0;
          m.PrevNode = 0xFFFF;
          m.Speed = 0.f;
          m.Wait = 0.f;
          m.BlockedTime = 0.f;
          m.SideTime = 0.f;
          m.PutBackTime = 0.f;
          Unstand(m);
          m.Blocker = 0;
          m.Unshow = 0.f;
          m.Hidden = (b.Pos[1] < -9000.f || respawned) ? 1 : 0;
          m.LaneY = HiddenY;      // not known until it has driven a step
          m.HeadingSet = 0;
          if (m.Hidden) rec[i].Pos[1] = HiddenY;
          if (respawned) ++s.Counts.Respawned;
        }
        s.Visible[i] = m.Hidden == 0 ? 1 : 0;
        // its heading goes on from the rules' own where the record still holds it (a step's turn at a high frame rate
        // is less than the half float's steps)
        s.StartDeg[i] = (m.HeadingSet && FloatToHalf(m.HeadingDeg) == b.Heading) ? m.HeadingDeg : HalfToFloat(b.Heading);
        float h = s.StartDeg[i] * 0.0174532925f;
        s.NewX[i] = b.Pos[0]; s.NewY[i] = b.Pos[1]; s.NewZ[i] = b.Pos[2];
        s.NewDX[i] = s.WayX[i] = std::sin(h); s.NewDZ[i] = s.WayZ[i] = std::cos(h);
      }
      BuildGrid(s, in);
      RT_ADD(TicksGather, t0);

      // one that appeared inside another waits unseen
      for (int i = 0; i < in.Count; ++i)
      {
        Member& m = s.M[i];
        if (!m.Alive || m.Age != 0 || m.Hidden) continue;
        int other;
        if (Inside(s, in, i, s.NewX[i], s.NewY[i], s.NewZ[i], s.NewDX[i], s.NewDZ[i], 0.15f, other))
        {
          m.Hidden = 1;
          s.Visible[i] = 0;
          rec[i].Pos[1] = HiddenY;
          ++s.Counts.Waiting;
        }
      }

      // the time of day's share: how many show and how many it wants
      int shown = 0;
      for (int i = 0; i < in.Count; ++i) shown += s.Visible[i];
      int wanted = Wanted(in.Density, in.Count);
      if (s.Most > 0 && s.Most < wanted) wanted = s.Most;   // the route's most (Routes)

      // each car: its lane, how far it may go, where it ends up
      for (int i = 0; i < in.Count; ++i)
      {
        Member& m = s.M[i];
        if (!m.Alive) continue;
        Record& r = rec[i];
        Record const& b = s.Before[i];
        ++s.Counts.Cars;
        ++m.Age;
        m.Blocker = 0;
        if (r.From >= in.NodeCount || r.To >= in.NodeCount) { Keep(s, in, i, r, b); ++s.Counts.NoLane; continue; }
        if (b.From != r.From || b.To != r.To)
          m.PrevNode = (r.From == b.To && b.From < in.NodeCount) ? b.From : (uint16_t)0xFFFF;
        float side = HalfToFloat(r.Side);
        int lane = side <= -0.5f ? 1 : 0;
        Path path;
        RT_TICK(t1);
        BuildPath(s, in, r, b, lane, side, m.PrevNode, path);
        RT_ADD(TicksPath, t1);
        if (path.Count < 2) { Keep(s, in, i, r, b); ++s.Counts.NoLane; continue; }
        path.CarArc = Project(path, b.Pos[0], b.Pos[2], nullptr, s.WayX[i], s.WayZ[i]);
        // its lane's end as the corner the game drives to, so the game's own arrival test goes by the lane
        r.Target[0] = path.EndX; r.Target[1] = path.EndY; r.Target[2] = path.EndZ;

        float cruise = in.SpeedMin + (in.SpeedMax - in.SpeedMin) * SpeedTable[i % 12];
        float advGame = Len2(r.Pos[0] - b.Pos[0], r.Pos[2] - b.Pos[2]);
        float most = cruise * in.Dt * 1.05f + 0.01f;
        if (!(advGame < most)) advGame = most;

        // turning round at a road's dead end (its next node is its from node: the game sends it back; the lanes are a
        // car's width apart there, so it would cross the cars coming the other way): it does the turn unseen, from 8 m
        // before the end node to 10 m after it
        bool turning = false;
        if (r.Next == r.From && r.Next < in.NodeCount)
        {
          float const* pt = NodePos(in, r.To);
          turning = Len2(b.Pos[0] - pt[0], b.Pos[2] - pt[2]) < 8.f;
        }
        else if (m.PrevNode == r.To)
        {
          float const* pf = NodePos(in, r.From);
          turning = Len2(b.Pos[0] - pf[0], b.Pos[2] - pf[2]) < 10.f;
        }
        if (path.OffRoad || turning)
        {
          // no road under this segment, or turning round: it drives unseen, in sight too. (Tried 2026-11-03 for the
          // pops: in sight a car turning round drove its lane round the road's end or made the game's own turn, and one
          // at the off-road segment's start waited there. Offline both jammed: placement 254's dead end at node 193
          // stood 74% of the time, placement 102 at its off-road segment 80%.)
          if (m.Hidden == 0) ++s.Counts.Unseen;
          m.Hidden = 1;
          Move(s, in, i, r, b, path, advGame, cruise, true);
          continue;
        }
        // made unseen by BreakCycles or StandingOut: drives on unseen a while before it may show (away from where it stood)
        if (m.Hidden && m.Unshow > 0.f)
        {
          m.Unshow -= in.Dt;
          Move(s, in, i, r, b, path, advGame, cruise, true);
          continue;
        }
        // unseen: shows where it is if that's in a gap (looked at every fourth step, in turn), else drives on unseen
        if (m.Hidden && ((unsigned)i + (unsigned)s.Counts.Steps) % 4u != 0u)
        {
          Move(s, in, i, r, b, path, advGame, cruise, true);
          continue;
        }
        if (m.Hidden)
        {
          int other;
          float py = b.Pos[1];
          if (path.Count >= 2) py = HeightAt(path, Project(path, b.Pos[0], b.Pos[2], nullptr, s.WayX[i], s.WayZ[i]));
          // the time of day's share full: it stays unseen for that. Any unseen car shows only out of sight (with no
          // view known, wherever it's in a gap: the caller has no share then), but where the placement has none shown,
          // anywhere: with none shown its box is under the city (see HiddenY)
          if (shown >= wanted) m.Hidden = 2;
          bool share = m.Hidden == 2;
          bool may = shown < wanted && (in.InSightToo || !in.ViewKnown || shown == 0 || Unseen(in, b.Pos[0], py, b.Pos[2]));
          if (may && !Inside(s, in, i, b.Pos[0], py, b.Pos[2], s.NewDX[i], s.NewDZ[i], 0.4f, other) && !Closing(s, in, i, b.Pos[0], py, b.Pos[2], cruise))
          {
            m.Hidden = 0;
            s.Visible[i] = 1;
            s.Before[i].Pos[1] = py;     // shown on its lane from here (the others' checks and a put-back use it)
            s.NewY[i] = py;
            ++s.Counts.Revealed;
            if (share) ++s.Counts.DensityShown;
            ++shown;
            advGame = cruise * in.Dt;
          }
          else
          {
            Move(s, in, i, r, b, path, advGame, cruise, true);
            continue;
          }
        }
        RT_TICK(t1);
        float allowed = Allowed(s, in, i, b, path, advGame, cruise);
        RT_ADD(TicksAllowed, t1);
        RT_TICK(t1);
        if (m.Hidden) { Move(s, in, i, r, b, path, advGame, cruise, true); continue; }   // stuck with another waiting for it: lets it by
        // it mustn't end inside a car (the gap rules look along its way; a car beside it where lanes come together
        // is found here): the longest move that doesn't
        Move(s, in, i, r, b, path, allowed, cruise, false);
        int other;
        bool sideHeld = false;
        float otherSpeed = 0.f;
        uint32_t otherId = 0;
        m.SideBy = 0; m.SideSpeed = 0.f;
        if (allowed > 1e-4f && Inside(s, in, i, s.NewX[i], s.NewY[i], s.NewZ[i], s.NewDX[i], s.NewDZ[i], 0.f, other, &otherSpeed, &otherId))
        {
          float lo = 0.f, hi = allowed;
          for (int k = 0; k < 4; ++k)
          {
            float mid = 0.5f * (lo + hi);
            Move(s, in, i, r, b, path, mid, cruise, false);
            int o2;
            float sp;
            uint32_t oid;
            if (Inside(s, in, i, s.NewX[i], s.NewY[i], s.NewZ[i], s.NewDX[i], s.NewDZ[i], 0.f, o2, &sp, &oid)) { hi = mid; other = o2; otherSpeed = sp; otherId = oid; }
            else lo = mid;
          }
          allowed = lo;
          Move(s, in, i, r, b, path, allowed, cruise, false);
          sideHeld = otherSpeed < 0.5f;
          m.SideBy = otherId; m.SideSpeed = otherSpeed;
        }
        if (m.SideBy) m.Blocker = m.SideBy;
        else if (allowed < advGame - 1e-4f) m.Blocker = m.HeldBy;
        if (allowed < advGame - 1e-4f) ++s.Counts.Held;
        if (allowed < 0.25f * advGame && advGame > 1e-4f) m.Wait += in.Dt;
        else m.Wait = 0.f;
        // beside a car that doesn't move either, for long: this one waits unseen until there's room
        if (sideHeld && allowed < 0.25f * advGame) m.SideTime += in.Dt;
        else m.SideTime = 0.f;
        if (m.SideTime > 6.f)
        {
          m.SideTime = 0.f;
          m.Hidden = 1;
          ++s.Counts.Waiting;
          Hold(s, in, i, r, b, path, true);
        }
        RT_ADD(TicksMove, t1);
      }

      // cars that have waited on each other in a loop for over 3 s (each held back by the next, the last by the first:
      // they can't sort it out): the youngest of them waits unseen until there's room. (Tried 2026-11-03: stuck cars
      // in sight waiting for the view to look away, 8 - 15 s, against the pops. Offline it jammed: placement 93 stood
      // 45 - 56% of the time with a view on it, 16% without.)
      {
        uint32_t idBase = IdBase(s);
        for (int i = 0; i < in.Count; ++i)
        {
          if (!s.Visible[i] || s.M[i].Wait < 3.f) continue;
          int cur = i, youngest = i;
          bool loop = false;
          for (int hop = 0; hop < 8; ++hop)
          {
            uint32_t by = s.M[cur].HeldBy ? s.M[cur].HeldBy : s.M[cur].SideBy;
            if (!by || by - idBase >= (uint32_t)in.Count) break;   // none, or another placement's
            int next = (int)(by - idBase);
            if (!s.Visible[next] || s.M[next].Wait < 3.f) break;
            if (s.M[next].Age < s.M[youngest].Age) youngest = next;
            if (next == i) { loop = true; break; }
            cur = next;
          }
          if (!loop) continue;
          Member& y = s.M[youngest];
          y.Hidden = 1;
          y.Wait = 0.f;
          s.Visible[youngest] = 0;
          rec[youngest].Pos[1] = HiddenY;
          rec[youngest].Speed = 0;
          y.Speed = 0.f;
          ++s.Counts.Waiting;
        }
      }

      RT_TICK(t1);
      // nothing ends inside another car (each car's own move was checked against those before it; this is against
      // all): one that moved into one goes back to where it was; one inside another where it was (another placement's
      // car came into it), the younger of the two waits unseen. One put back every step for PutBackSeconds waits
      // unseen too: two cars side by side where two lanes meet can hold each other for good with neither of the
      // timers above running (2026-11-04, placement 188's join at node 1853 by the train station, all its 82 cars
      // standing: the first car's move was allowed and then put back here, every step; the second, held, read the
      // first's speed before the put-back, 0.502 m/s, as moving, so its SideTime never ran)
      for (int i = 0; i < in.Count; ++i)
      {
        if (!s.Visible[i]) { s.M[i].PutBackTime = 0.f; continue; }
        int other;
        uint32_t backId = 0;
        if (!Inside(s, in, i, s.NewX[i], s.NewY[i], s.NewZ[i], s.NewDX[i], s.NewDZ[i], 0.f, other, nullptr, &backId)) { s.M[i].PutBackTime = 0.f; continue; }
        Record const& b = s.Before[i];
        if (std::fabs(s.NewX[i] - b.Pos[0]) > 1e-4f || std::fabs(s.NewZ[i] - b.Pos[2]) > 1e-4f)
        {
          Record& r = rec[i];
          r.Pos[0] = b.Pos[0]; r.Pos[1] = b.Pos[1]; r.Pos[2] = b.Pos[2];
          r.Heading = b.Heading;
          s.M[i].HeadingDeg = s.StartDeg[i];
          r.Speed = 0;
          s.NewX[i] = b.Pos[0]; s.NewY[i] = b.Pos[1]; s.NewZ[i] = b.Pos[2];
          s.NewDX[i] = s.WayX[i]; s.NewDZ[i] = s.WayZ[i];
          s.M[i].Speed = 0.f;
          ++s.Counts.Reverted;
          s.M[i].Blocker = backId;
          s.M[i].PutBackTime += in.Dt;
          if (!Inside(s, in, i, s.NewX[i], s.NewY[i], s.NewZ[i], s.NewDX[i], s.NewDZ[i], 0.f, other))
          {
            if (s.M[i].PutBackTime <= PutBackSeconds) continue;
            ++s.Counts.PutBackHidden;   // other is -1 here: it's this one that waits unseen
          }
        }
        else s.M[i].PutBackTime = 0.f;
        int hide = (other >= 0 && s.M[other].Age < s.M[i].Age) ? other : i;
        s.M[hide].Hidden = 1;
        s.Visible[hide] = 0;
        rec[hide].Pos[1] = HiddenY;
        rec[hide].Speed = 0;
        s.M[hide].Speed = 0.f;
        s.M[hide].PutBackTime = 0.f;
        ++s.Counts.Waiting;
      }
      // never a jam: how long each shown car has stood (within StandMove of where it stopped), then wait cycles over
      // any placements, then a car standing too long whatever holds it
      for (int i = 0; i < in.Count; ++i)
      {
        Member& m = s.M[i];
        if (!s.Visible[i]) { Unstand(m); continue; }
        float dx = rec[i].Pos[0] - m.AnchorX, dz = rec[i].Pos[2] - m.AnchorZ;
        if (!(dx * dx + dz * dz <= StandMove * StandMove)) { m.AnchorX = rec[i].Pos[0]; m.AnchorZ = rec[i].Pos[2]; m.StandTime = 0.f; }
        else m.StandTime += in.Dt;
      }
      BreakCycles(s, in, rec);
      StandingOut(s, in, rec);

      shown = 0;
      for (int i = 0; i < in.Count; ++i) shown += s.Visible[i];
      // its first step: above its route's most, cars go unseen in sight too, spread evenly over its members (the
      // game's first spread puts them along its lanes in member order), rather than one by one out of sight
      if (s.Fresh)
      {
        s.Fresh = 0;
        if (s.Most > 0 && shown > s.Most)
        {
          int eligible = 0;
          for (int i = 0; i < in.Count; ++i)
            if (s.Visible[i] && s.M[i].Age >= 1 && s.M[i].LaneY > -9000.f) ++eligible;
          int excess = shown - s.Most;
          if (excess > eligible) excess = eligible;
          if (excess > shown - 1) excess = shown - 1;
          int seen = 0, done = 0;
          for (int i = 0; i < in.Count && done < excess; ++i)
          {
            Member& m = s.M[i];
            if (!s.Visible[i] || m.Age < 1 || !(m.LaneY > -9000.f)) continue;
            ++seen;
            if ((long long)seen * excess / eligible <= (long long)done) continue;
            m.Hidden = 2;
            s.Visible[i] = 0;
            rec[i].Pos[1] = HiddenY;
            Unstand(m);
            --shown;
            ++done;
            ++s.Counts.CapHidden;
            if (!Unseen(in, s.NewX[i], s.NewY[i], s.NewZ[i])) ++s.Counts.CapHiddenInSight;
          }
        }
      }
      // above the time of day's share: shown cars out of sight go unseen (a few a step, from where the last step
      // stopped) and drive on unseen; one with no lane height known (no lane) stays
      if (shown > wanted && (in.ViewKnown || in.InSightToo) && in.Count > 0)
      {
        int done = 0;
        int most = in.InSightToo ? in.Count : DensityPerStep;
        for (int k = 0; k < in.Count && shown > wanted && done < most; ++k)
        {
          int i = (s.DensityNext + k) % in.Count;
          Member& m = s.M[i];
          if (!s.Visible[i] || m.Age < 2 || !(m.LaneY > -9000.f)) continue;
          if (!in.InSightToo && !Unseen(in, s.NewX[i], s.NewY[i], s.NewZ[i])) continue;
          m.Hidden = 2;
          s.Visible[i] = 0;
          rec[i].Pos[1] = HiddenY;
          Unstand(m);
          --shown;
          ++done;
          ++s.Counts.DensityHidden;
        }
        s.DensityNext = (s.DensityNext + 7) % in.Count;
      }
      for (int i = 0; i < in.Count; ++i)
        if (s.M[i].Alive)
        {
          s.M[i].LastX = rec[i].Pos[0];
          s.M[i].LastZ = rec[i].Pos[2];
        }
      Publish(s, in, rec);
      RT_ADD(TicksFinal, t1);
    }

  private:
    void Reset(Slot& s, StepInput const& in)
    {
      s.Seq += 2;
      s.Key = in.Key;
      s.Records = (uintptr_t)in.Records;
      s.Count = in.Count;
      s.Nodes = (uintptr_t)in.Nodes;
      s.NodeCount = in.NodeCount;
      s.PubCount = 0;
      s.LastTime = in.Now;
      s.DensityNext = 0;
      s.Most = 0;
      s.MostLooked = 0;
      s.Fresh = 1;
      s.RouteX = s.RouteZ = 0.f;
      s.RouteRow = -1;
      s.StandNext = 0.0;
      for (int i = 0; i < MaxMembers; ++i)
      {
        memset(&s.M[i], 0, sizeof(Member));
        s.M[i].PrevNode = 0xFFFF;
        Unstand(s.M[i]);
      }
      for (int i = 0; i < Slot::CacheSize; ++i) s.CacheKey[i] = 0xFFFFFFFFu;
    }

    float const* NodePos(StepInput const& in, int n) const { return (float const*)(in.Nodes + (size_t)n * NodeSize); }

    static void Unstand(Member& m) { m.StandTime = 0.f; m.AnchorX = m.AnchorZ = 1e30f; }

    // The route table's most for this placement's nodes (0 none); x, z its nodes' centroid, row its row (-1 none)
    static int RouteMost(StepInput const& in, float& x, float& z, int& row)
    {
      x = z = 0.f;
      row = -1;
      if (in.NodeCount <= 0) return 0;
      double sx = 0.0, sz = 0.0;
      for (int n = 0; n < in.NodeCount; ++n)
      {
        float const* p = (float const*)(in.Nodes + (size_t)n * NodeSize);
        sx += p[0];
        sz += p[2];
      }
      x = (float)(sx / in.NodeCount);
      z = (float)(sz / in.NodeCount);
      for (int k = 0; k < RouteCount; ++k)
        if (Routes[k].Nodes == in.NodeCount && std::fabs(Routes[k].X - x) < RouteNear && std::fabs(Routes[k].Z - z) < RouteNear)
        {
          row = k;
          return Routes[k].Most;
        }
      return 0;
    }

    // a car made unseen to end a jam: it drives on unseen UnshowSeconds at least
    void HideStuck(Slot& s, Record* rec, int i)
    {
      Member& m = s.M[i];
      m.Hidden = 1;
      m.Wait = 0.f;
      m.BlockedTime = 0.f;
      m.SideTime = 0.f;
      m.PutBackTime = 0.f;
      m.Unshow = UnshowSeconds;
      m.Speed = 0.f;
      s.Visible[i] = 0;
      rec[i].Pos[1] = HiddenY;
      rec[i].Speed = 0;
      Unstand(m);
    }

    bool CycleCar(Slot const& s, int i) const
    {
      return s.Visible[i] && s.M[i].Blocker != 0 && s.M[i].StandTime >= CycleSeconds;
    }

    // Another placement's car by id this step: its index in Foreign, -1 none
    int ForeignIndex(Slot& s, uint32_t id)
    {
      const unsigned mask = (unsigned)Slot::ForeignBuckets - 1u;
      if (!s.ForeignAtBuilt)
      {
        memset(s.ForeignAt, 0, sizeof(s.ForeignAt));
        for (int f = 0; f < s.ForeignCount; ++f)
        {
          unsigned h = (s.Foreign[f].Id * 2654435761u) >> 20;
          while (s.ForeignAt[h]) h = (h + 1u) & mask;
          s.ForeignAt[h] = f + 1;
        }
        s.ForeignAtBuilt = 1;
      }
      unsigned h = (id * 2654435761u) >> 20;
      for (int probe = 0; probe < Slot::ForeignBuckets && s.ForeignAt[h]; ++probe, h = (h + 1u) & mask)
        if (s.Foreign[s.ForeignAt[h] - 1].Id == id) return s.ForeignAt[h] - 1;
      return -1;
    }

    // Wait cycles: shown cars standing CycleSeconds, each held back by the next (Blocker) and the last by the first,
    // over any placements (the others' latest published cars near this one). Such cars can't sort it out (the loop
    // rule above sees only its own placement's, 8 at most, by HeldBy / SideBy). One of them waits unseen: the
    // placement with the lowest slot among the cycle's cars does it (each placement in it sees the same cycle), with
    // its own car out of sight first, else the youngest. A car the game's step stopped (a red light) has no Blocker:
    // a queue at a light ends there and is never a cycle.
    void BreakCycles(Slot& s, StepInput const& in, Record* rec)
    {
      bool any = false;
      for (int i = 0; i < in.Count && !any; ++i) any = CycleCar(s, i);
      if (!any) return;
      memset(s.CycleState, 0, (size_t)in.Count);
      s.ForeignAtBuilt = 0;
      const uint32_t own = IdBase(s) >> 10;
      int shown = 0;
      for (int i = 0; i < in.Count; ++i) shown += s.Visible[i];
      int walk[CycleHops];   // own member, or MaxMembers + the other's index in Foreign
      for (int start = 0; start < in.Count; ++start)
      {
        if (s.CycleState[start] != 0 || !CycleCar(s, start)) continue;
        int n = 0, at = -1, cur = start;
        bool cut = false;
        for (;;)
        {
          if (cur < MaxMembers)
          {
            if (s.CycleState[cur] == 2) break;   // looked at from another car this step
            if (s.CycleState[cur] == 1) { for (int q = 0; q < n; ++q) if (walk[q] == cur) at = q; break; }
            if (!CycleCar(s, cur)) break;
            s.CycleState[cur] = 1;
          }
          else
          {
            bool again = false;
            for (int q = 0; q < n; ++q) if (walk[q] == cur) { at = q; again = true; }
            if (again) break;
            Published const& p = s.Foreign[cur - MaxMembers];
            if (!(p.Stand >= CycleSeconds) || p.Blocker == 0) break;
          }
          if (n >= CycleHops) { cut = true; break; }   // (after the close was looked at: a cycle of CycleHops is found)
          walk[n++] = cur;
          uint32_t by = cur < MaxMembers ? s.M[cur].Blocker : s.Foreign[cur - MaxMembers].Blocker;
          if ((by >> 10) == own)
          {
            uint32_t i = by - IdBase(s);
            if (i >= (uint32_t)in.Count) break;
            cur = (int)i;
          }
          else
          {
            int f = ForeignIndex(s, by);
            if (f < 0) break;
            cur = MaxMembers + f;
          }
        }
        if (cut)
        {
          // too long to follow to its end from here: its cars are left to later walks (one of them may start nearer a
          // cycle it leads to), only this start is done. (Marked done, a cycle behind a tail of 54+ cars was never
          // found: the 2026-11-13 review's made-up test.)
          for (int q = 0; q < n; ++q) if (walk[q] < MaxMembers) s.CycleState[walk[q]] = 0;
          if (cur < MaxMembers) s.CycleState[cur] = 0;
          s.CycleState[start] = 2;
          continue;
        }
        for (int q = 0; q < n; ++q) if (walk[q] < MaxMembers) s.CycleState[walk[q]] = 2;
        if (at < 0) continue;
        // the cycle is walk[at .. n - 1]
        uint32_t lowest = own;
        bool cross = false;
        for (int q = at; q < n; ++q)
          if (walk[q] >= MaxMembers)
          {
            cross = true;
            uint32_t o = s.Foreign[walk[q] - MaxMembers].Id >> 10;
            if (o < lowest) lowest = o;
          }
        if (lowest != own) continue;   // another placement in it does it
        int victim = -1;
        bool victimFar = false;
        for (int q = at; q < n; ++q)
        {
          int i = walk[q];
          if (i >= MaxMembers || !(s.M[i].LaneY > -9000.f)) continue;
          bool far = Unseen(in, s.NewX[i], s.NewY[i], s.NewZ[i]);
          if (victim < 0 || (far && !victimFar) || (far == victimFar && s.M[i].Age < s.M[victim].Age)) { victim = i; victimFar = far; }
        }
        if (victim < 0 || shown <= 1) continue;   // never its last shown car (StandingOut's time comes for the others')
        HideStuck(s, rec, victim);
        --shown;
        ++s.Counts.CycleHidden;
        if (cross) ++s.Counts.CycleCross;
      }
    }

    // A shown car standing StandOutSeconds out of sight, or StandInSeconds in sight (or with no view known), waits
    // unseen whatever holds it (a jam the rules above don't see through): the longest standing one, one a StandEvery
    // in a placement at most, never its last shown car. A red light holds a car 12 s at most (the longest red: two
    // inputs at 10 s green + 1 s orange each), and a queue at a light moves up each green.
    void StandingOut(Slot& s, StepInput const& in, Record* rec)
    {
      if (in.Now < s.StandNext) return;
      int longest = -1, shown = 0;
      bool longestSeen = false;
      for (int i = 0; i < in.Count; ++i)
      {
        if (!s.Visible[i]) continue;
        ++shown;
        Member const& m = s.M[i];
        if (m.Age < 2 || !(m.LaneY > -9000.f) || !(m.StandTime >= StandOutSeconds)) continue;
        bool seen = !Unseen(in, s.NewX[i], s.NewY[i], s.NewZ[i]);
        if (seen && !(m.StandTime >= StandInSeconds)) continue;
        if (longest < 0 || m.StandTime > s.M[longest].StandTime) { longest = i; longestSeen = seen; }
      }
      if (longest < 0 || shown <= 1) return;
      HideStuck(s, rec, longest);
      s.StandNext = in.Now + StandEvery;
      ++s.Counts.StandHidden;
      if (longestSeen) ++s.Counts.StandHiddenInSight;
    }

    // Out of sight of the view (unknown: never)
    static bool Unseen(StepInput const& in, float x, float y, float z)
    {
      if (!in.ViewKnown) return false;
      return OutOfSight(x, y, z, in.ViewX, in.ViewY, in.ViewZ, in.ViewAX, in.ViewAY, in.ViewAZ, FarAway, SideNear);
    }

    // The table's segment for nodes a -> b of this placement (cached), -1 none
    int Segment(Slot& s, StepInput const& in, int a, int b)
    {
      uint32_t key = (uint32_t)a << 16 | (uint32_t)b;
      uint32_t h = (key * 2654435761u) % Slot::CacheSize;
      for (int probe = 0; probe < Slot::CacheSize; ++probe)
      {
        uint32_t k = (h + probe) % Slot::CacheSize;
        if (s.CacheKey[k] == key) return s.CacheValue[k];
        if (s.CacheKey[k] == 0xFFFFFFFFu)
        {
          int seg = Index.Find(NodePos(in, a), NodePos(in, b));
          if (seg >= 0) ++s.Counts.LaneFound;
          else ++s.Counts.LaneMissing;
          s.CacheKey[k] = key;
          s.CacheValue[k] = seg;
          return seg;
        }
      }
      return Index.Find(NodePos(in, a), NodePos(in, b));
    }

    // Segment a -> b's lane at fraction t of the way
    void LanePoint(Slot& s, StepInput const& in, int a, int b, int lane, float side, float t, float& x, float& y, float& z)
    {
      int seg = Segment(s, in, a, b);
      if (seg >= 0 && road_traffic_lanes::Segments[seg].Count >= 2)
      {
        road_traffic_lanes::Segment const& g = road_traffic_lanes::Segments[seg];
        float f = Clamp(t, 0.f, 1.f) * (float)(g.Count - 1);
        int k = (int)f;
        if (k >= g.Count - 1) k = g.Count - 2;
        float w = f - (float)k;
        float const* p0 = Index.Point[g.First + k][lane];
        float const* p1 = Index.Point[g.First + k + 1][lane];
        x = p0[0] + (p1[0] - p0[0]) * w; y = p0[1] + (p1[1] - p0[1]) * w; z = p0[2] + (p1[2] - p0[2]) * w;
        return;
      }
      float const* pa = NodePos(in, a);
      float const* pb = NodePos(in, b);
      float dx = pb[0] - pa[0], dz = pb[2] - pa[2];
      float len = Len2(dx, dz);
      float px = len > 1e-4f ? dz / len : 0.f, pz = len > 1e-4f ? -dx / len : 0.f;
      float o = side * *(float const*)(in.Nodes + (size_t)b * NodeSize + NodeRadius);   // the game's own lane
      x = pa[0] + dx * t + px * o;
      y = pa[1] + (pb[1] - pa[1]) * t;
      z = pa[2] + dz * t + pz * o;
    }

    // Segment a -> b's lane from metre m0 to m1 along it (its samples between, both ends)
    void AddLane(Slot& s, StepInput const& in, int a, int b, int lane, float side, float m0, float m1, Path& path)
    {
      float const* pa = NodePos(in, a);
      float const* pb = NodePos(in, b);
      float len = Len2(pb[0] - pa[0], pb[2] - pa[2]);
      if (len < 1e-3f) return;
      float t0 = Clamp(m0 / len, 0.f, 1.f), t1 = Clamp(m1 / len, 0.f, 1.f);
      int seg = Segment(s, in, a, b);
      int n = seg >= 0 && road_traffic_lanes::Segments[seg].Count >= 2 ? road_traffic_lanes::Segments[seg].Count : 2;
      int onTable = seg >= 0 && road_traffic_lanes::Segments[seg].Count >= 2 ? seg : -1;
      float x, y, z;
      LanePoint(s, in, a, b, lane, side, t0, x, y, z);
      path.Add(x, y, z, onTable, t0);
      // the samples strictly between t0 and t1
      int k0 = (int)std::floor(t0 * (float)(n - 1)) + 1, k1 = (int)std::ceil(t1 * (float)(n - 1)) - 1;
      if (k0 < 1) k0 = 1;
      if (k1 > n - 2) k1 = n - 2;
      for (int k = k0; k <= k1; ++k)
      {
        float t = (float)k / (float)(n - 1);
        if (t <= t0 || t >= t1) continue;
        LanePoint(s, in, a, b, lane, side, t, x, y, z);
        path.Add(x, y, z, onTable, t);
      }
      LanePoint(s, in, a, b, lane, side, t1, x, y, z);
      path.Add(x, y, z, onTable, t1);
    }

    // The car's lane round it: the end of the segment before (if known), its own, the start of the next one
    void BuildPath(Slot& s, StepInput const& in, Record const& r, Record const& b, int lane, float side, uint16_t prev, Path& path)
    {
      float const* pf = NodePos(in, r.From);
      float const* pt = NodePos(in, r.To);
      float dx = pt[0] - pf[0], dz = pt[2] - pf[2];
      float len = Len2(dx, dz);
      int seg = Segment(s, in, r.From, r.To);
      path.OffRoad = seg >= 0 && (road_traffic_lanes::Segments[seg].Flags & 1) != 0;
      path.Lane = lane;
      LanePoint(s, in, r.From, r.To, lane, side, 1.f, path.EndX, path.EndY, path.EndZ);
      // where along its segment the car is
      float at = len > 1e-3f ? ((b.Pos[0] - pf[0]) * dx + (b.Pos[2] - pf[2]) * dz) / len : 0.f;
      float m0 = at - 6.f, m1 = at + PathAhead;
      if (m0 < 0.f && prev != 0xFFFF && prev < in.NodeCount && prev != r.From)
      {
        float const* pp = NodePos(in, prev);
        float lp = Len2(pf[0] - pp[0], pf[2] - pp[2]);
        AddLane(s, in, prev, r.From, lane, side, lp + m0, lp, path);
      }
      AddLane(s, in, r.From, r.To, lane, side, m0 < 0.f ? 0.f : m0, m1 > len ? len : m1, path);
      if (m1 > len && r.Next < in.NodeCount && r.Next != r.To)
        AddLane(s, in, r.To, r.Next, lane, side, 0.f, m1 - len, path);   // (next = from: turning round at a road's end)
      // where two segments' lanes meet at a node with different offsets the joined lane can turn back on itself: such a
      // kink (a joint turning back by more than 120 degrees) is taken out
      for (int k = 1; k + 1 < path.Count;)
      {
        float ax = path.X[k] - path.X[k - 1], az = path.Z[k] - path.Z[k - 1];
        float bx = path.X[k + 1] - path.X[k], bz = path.Z[k + 1] - path.Z[k];
        if (ax * bx + az * bz < -0.5f * Len2(ax, az) * Len2(bx, bz))
        {
          for (int j = k; j + 1 < path.Count; ++j)
          {
            path.X[j] = path.X[j + 1]; path.Y[j] = path.Y[j + 1]; path.Z[j] = path.Z[j + 1];
            path.Seg[j] = path.Seg[j + 1]; path.T[j] = path.T[j + 1];
          }
          --path.Count;
          if (k > 1) --k;
        }
        else
          ++k;
      }
    }

    // The arc length along the path of a point's nearest place on it (and the path's height there)
    static float Project(Path const& path, float x, float z, float* outY, float = 0.f, float = 0.f)
    {
      float best = 1e30f, at = 0.f, s = 0.f, y = path.Y[0];
      for (int k = 0; k + 1 < path.Count; ++k)
      {
        float ax = path.X[k], az = path.Z[k], bx = path.X[k + 1], bz = path.Z[k + 1];
        float dx = bx - ax, dz = bz - az, l2 = dx * dx + dz * dz;
        float t = l2 < 1e-8f ? 0.f : Clamp(((x - ax) * dx + (z - az) * dz) / l2, 0.f, 1.f);
        float qx = ax + dx * t, qz = az + dz * t;
        float d = (x - qx) * (x - qx) + (z - qz) * (z - qz);
        float l = std::sqrt(l2);
        if (d < best) { best = d; at = s + t * l; y = path.Y[k] + (path.Y[k + 1] - path.Y[k]) * t; }
        s += l;
      }
      if (outY) *outY = y;
      return at;
    }

    static void PointAt(Path const& path, float arc, float& x, float& y, float& z)
    {
      for (int k = 0; k + 1 < path.Count; ++k)
      {
        float dx = path.X[k + 1] - path.X[k], dz = path.Z[k + 1] - path.Z[k];
        float l = Len2(dx, dz);
        if (arc <= l && l > 1e-6f)
        {
          float t = arc / l;
          x = path.X[k] + dx * t; y = path.Y[k] + (path.Y[k + 1] - path.Y[k]) * t; z = path.Z[k] + dz * t;
          return;
        }
        arc -= l;
      }
      x = path.X[path.Count - 1]; y = path.Y[path.Count - 1]; z = path.Z[path.Count - 1];
    }

    // The drawn road's height along the path at arc (as PointAt goes along it): on a piece of the path whose two points
    // are on one segment's lane, the road's height there from RoadTrafficHeights.h's fine heights (about every metre;
    // the path's own points are the lane samples, about 5.6 m apart, and the road bends between them); elsewhere (a
    // join of two segments' lanes, a segment the table hasn't) straight between the path's points
    static float HeightAt(Path const& path, float arc)
    {
      for (int k = 0; k + 1 < path.Count; ++k)
      {
        float dx = path.X[k + 1] - path.X[k], dz = path.Z[k + 1] - path.Z[k];
        float l = Len2(dx, dz);
        if (arc <= l && l > 1e-6f)
        {
          float w = arc / l;
          int seg = path.Seg[k];
          int n = seg >= 0 && seg == path.Seg[k + 1] ? (int)road_traffic_heights::FineCount[seg] : 0;
          if (n >= 2)
          {
            float t = Clamp(path.T[k] + (path.T[k + 1] - path.T[k]) * w, 0.f, 1.f);
            float f = t * (float)(n - 1);
            int j = (int)f;
            if (j >= n - 1) j = n - 2;
            float u = f - (float)j;
            float const* p0 = road_traffic_heights::FineY[road_traffic_heights::FineFirst[seg] + j];
            float const* p1 = road_traffic_heights::FineY[road_traffic_heights::FineFirst[seg] + j + 1];
            return p0[path.Lane] + (p1[path.Lane] - p0[path.Lane]) * u;
          }
          return path.Y[k] + (path.Y[k + 1] - path.Y[k]) * w;
        }
        arc -= l;
      }
      return path.Y[path.Count - 1];
    }

    // how far ahead on its lane a car steers for (2.5 + 0.08 x speed, 3 - 8 m, until 2026-11-03 14:3x: tighter, but the
    // turns started abruptly where two lanes meet)
    static float Lookahead(float speed) { return Clamp(3.f + 0.15f * speed, 4.f, 10.f); }

    // The car moves `advance` metres along its lane from where it was before the step (each call from its heading before
    // the step). A shown car steers like one: on an arc toward the point L ahead on its lane (pure pursuit's curvature,
    // 2 sin a / l), never tighter than MinRadius. Until 2026-11-03 14:3x it turned straight toward that point, at most
    // 0.6 rad a step: at the game's 140 steps a second no limit, so at a sharp joint of two lanes it pivoted (offline:
    // turns up to 1,300 deg/s, now under 350). An unseen one still turns straight to that point, as tight as it takes (a
    // turn round at a road's end).
    void Move(Slot& s, StepInput const& in, int i, Record& r, Record const& b, Path const& path, float advance, float cruise,
      bool unseen)
    {
      float x = b.Pos[0], z = b.Pos[2];
      float h = s.StartDeg[i] * 0.0174532925f;   // heading, radians from + z toward + x
      float dirx = std::sin(h), dirz = std::cos(h);
      float left = advance;
      float L = Lookahead(cruise);
      float arc = path.CarArc;            // its progress along the lane, step by step
      float turnLeft = Clamp(advance / 1.f, 0.6f, 1.5708f);   // unseen: radians it may turn this step
      const float kMost = 1.f / MinRadius;
      for (int steps = 0; left > 1e-4f && steps < 32; ++steps)
      {
        float cx, cy, cz;
        PointAt(path, arc + L, cx, cy, cz);
        float dx = cx - x, dz = cz - z, l = Len2(dx, dz);
        if (l < 1e-4f) break;
        float step = left < 0.5f * L ? left : 0.5f * L;
        float wx = dx / l, wz = dz / l;
        float ang = std::atan2(dirx * wz - dirz * wx, dirx * wx + dirz * wz);   // from its way to the wanted one (+: h falls)
        if (unseen)
        {
          float most = turnLeft * step / advance;
          if (std::fabs(ang) > most)
          {
            float a = ang > 0.f ? most : -most, c = std::cos(a), sn = std::sin(a);
            wx = dirx * c - dirz * sn; wz = dirx * sn + dirz * c;
          }
          dirx = wx; dirz = wz;
          h = std::atan2(dirx, dirz);
          x += dirx * step; z += dirz * step;
        }
        else
        {
          float k = Clamp(-2.f * std::sin(ang) / l, -kMost, kMost);   // dh / dm
          float hm = h + 0.5f * k * step;   // along the arc: its heading halfway
          x += std::sin(hm) * step; z += std::cos(hm) * step;
          h += k * step;
          dirx = std::sin(h); dirz = std::cos(h);
        }
        arc += step;
        left -= step;
      }
      // its height: the road's at its lane's point nearest where it is (until 2026-11-15 straight between the path's
      // points at arc; a car cutting inside a sharp turn is off its lane's point at arc)
      float y = HeightAt(path, Project(path, x, z, nullptr));
      r.Pos[0] = x; r.Pos[2] = z;
      r.Pos[1] = unseen ? HiddenY : y;
      s.M[i].LaneY = y;
      if (unseen && advance <= 1e-3f)
      {
        r.Heading = b.Heading;
        s.M[i].HeadingDeg = s.StartDeg[i];
      }
      else
      {
        float deg = h * 57.2957795f;
        while (deg < 0.f) deg += 360.f;
        while (deg >= 360.f) deg -= 360.f;
        r.Heading = FloatToHalf(deg);
        s.M[i].HeadingDeg = deg;
      }
      s.M[i].HeadingSet = 1;
      float factor = cruise > 0.01f && in.Dt > 1e-5f ? Clamp(advance / (cruise * in.Dt), 0.f, 1.f) : 0.f;
      r.Speed = FloatToHalf(factor);
      s.M[i].Speed = in.Dt > 1e-5f ? advance / in.Dt : 0.f;
      s.NewX[i] = x; s.NewY[i] = r.Pos[1]; s.NewZ[i] = z;
      if (advance > 1e-3f) { s.NewDX[i] = dirx; s.NewDZ[i] = dirz; }
      s.Visible[i] = unseen ? 0 : 1;
    }

    // Stays where it was before the step (unseen, or on its lane's height)
    void Hold(Slot& s, StepInput const&, int i, Record& r, Record const& b, Path const& path, bool unseen)
    {
      r.Pos[0] = b.Pos[0]; r.Pos[2] = b.Pos[2];
      float y = HiddenY;
      if (!unseen) Project(path, b.Pos[0], b.Pos[2], &y, s.WayX[i], s.WayZ[i]);
      r.Pos[1] = y;
      r.Heading = b.Heading;
      s.M[i].HeadingDeg = s.StartDeg[i];
      r.Speed = 0;
      s.M[i].Speed = 0.f;
      s.NewX[i] = b.Pos[0]; s.NewY[i] = y; s.NewZ[i] = b.Pos[2];
      s.Visible[i] = unseen ? 0 : 1;
    }

    // Left as the game's step made it (no lane for it); one the rules keep unseen stays unseen
    void Keep(Slot& s, StepInput const& in, int i, Record& r, Record const& b)
    {
      if (s.M[i].Hidden) r.Pos[1] = HiddenY;
      s.M[i].HeadingDeg = HalfToFloat(r.Heading);
      float h = HalfToFloat(r.Heading) * 0.0174532925f;
      s.NewX[i] = r.Pos[0]; s.NewY[i] = r.Pos[1]; s.NewZ[i] = r.Pos[2];
      s.NewDX[i] = std::sin(h); s.NewDZ[i] = std::cos(h);
      s.Visible[i] = (s.M[i].Hidden == 0 && r.Pos[1] > -9000.f) ? 1 : 0;
      s.M[i].Speed = in.Dt > 1e-5f ? Len2(r.Pos[0] - b.Pos[0], r.Pos[2] - b.Pos[2]) / in.Dt : 0.f;
    }

    // ---- the neighbour grid: own seen cars where they were before the step, then the others' cars ----
    static unsigned CellOf(int cx, int cz) { return ((unsigned)cx * 73856093u ^ (unsigned)cz * 19349663u) % GridBuckets; }

    void BuildGrid(Slot& s, StepInput const& in)
    {
      for (int k = 0; k < GridBuckets; ++k) s.GridHead[k] = -1;
      auto add = [&](int e, float x, float z) {
        int gx = (int)std::floor(x / Cell), gz = (int)std::floor(z / Cell);
        unsigned c = CellOf(gx, gz);
        s.GridX[e] = x; s.GridZ[e] = z;
        s.GridCX[e] = gx; s.GridCZ[e] = gz;
        s.GridNext[e] = s.GridHead[c];
        s.GridHead[c] = e;
      };
      for (int i = 0; i < in.Count; ++i)
        if (s.M[i].Alive) add(i, s.Before[i].Pos[0], s.Before[i].Pos[2]);
      for (int f = 0; f < s.ForeignCount; ++f)
        add(MaxMembers + f, s.Foreign[f].X, s.Foreign[f].Z);
    }

    // Calls fn(entry) for every grid entry in the box (own < MaxMembers, the others' after); fn returns true to stop
    template <class Fn> bool NearBox(Slot& s, float x0, float z0, float x1, float z1, Fn fn)
    {
      x0 -= 4.f; z0 -= 4.f; x1 += 4.f; z1 += 4.f;   // cars move a few metres in a step
      int cx0 = (int)std::floor(x0 / Cell), cx1 = (int)std::floor(x1 / Cell);
      int cz0 = (int)std::floor(z0 / Cell), cz1 = (int)std::floor(z1 / Cell);
      for (int cx = cx0; cx <= cx1; ++cx)
        for (int cz = cz0; cz <= cz1; ++cz)
          for (int e = s.GridHead[CellOf(cx, cz)]; e >= 0; e = s.GridNext[e])
          {
            float ex = s.GridX[e], ez = s.GridZ[e];
            if (ex < x0 || ex > x1 || ez < z0 || ez > z1) continue;
            if (s.GridCX[e] != cx || s.GridCZ[e] != cz) continue;   // another cell's (as BuildGrid worked it out)
            if (fn(e)) return true;
          }
      return false;
    }

    template <class Fn> bool Near(Slot& s, float x, float z, float radius, Fn fn)
    {
      return NearBox(s, x - radius, z - radius, x + radius, z + radius, fn);
    }

    // A seen car's footprint (own: where it is now this step; others': their latest) overlaps car i's at (x, y, z)
    bool Inside(Slot& s, StepInput const& in, int i, float x, float y, float z, float dx, float dz, float inflate, int& other,
      float* otherSpeed = nullptr, uint32_t* otherId = nullptr)
    {
      other = -1;
      int found = -2;
      float speed = 0.f;
      uint32_t id = 0;
      Near(s, x, z, 2.f * HalfLength + 2.f * inflate + 1.f, [&](int e) {
        if (e < MaxMembers)
        {
          if (e == i || e >= in.Count || !s.Visible[e]) return false;
          // where it is now, and where it was before the step (nothing moves into a place a car left this step, as
          // the gap rules go by those: so a car put back where it was is never inside another)
          bool hit = std::fabs(s.NewX[e] - x) <= 6.f && std::fabs(s.NewZ[e] - z) <= 6.f && std::fabs(s.NewY[e] - y) <= Height &&
            Overlap(x, z, dx, dz, s.NewX[e], s.NewZ[e], s.NewDX[e], s.NewDZ[e], inflate);
          Record const& b = s.Before[e];
          if (!hit && (b.Pos[0] != s.NewX[e] || b.Pos[2] != s.NewZ[e]))
            hit = std::fabs(b.Pos[0] - x) <= 6.f && std::fabs(b.Pos[2] - z) <= 6.f && std::fabs(b.Pos[1] - y) <= Height &&
              Overlap(x, z, dx, dz, b.Pos[0], b.Pos[2], s.WayX[e], s.WayZ[e], inflate);
          if (hit) { found = e; speed = s.M[e].Speed; id = IdBase(s) + (uint32_t)e; return true; }
          return false;
        }
        Published const& p = s.Foreign[e - MaxMembers];
        if (std::fabs(p.Y - y) > Height) return false;
        if (Overlap(x, z, dx, dz, p.X, p.Z, p.DirX, p.DirZ, inflate)) { found = -1; speed = p.Speed; id = p.Id; return true; }
        return false;
      });
      if (found == -2) return false;
      other = found;
      if (otherSpeed) *otherSpeed = speed;
      if (otherId) *otherId = id;
      return true;
    }

    // A seen car behind (x, y, z), going its way, that couldn't slow to `speed` (the showing car's) before reaching it
    bool Closing(Slot& s, StepInput const& in, int i, float x, float y, float z, float speed)
    {
      auto closing = [&](float bx, float by, float bz, float bdx, float bdz, float bspeed) {
        if (std::fabs(by - y) > Height) return false;
        if (bdx * s.NewDX[i] + bdz * s.NewDZ[i] < 0.5f) return false;   // only a car going its way would run into it
        float rx = x - bx, rz = z - bz;
        float along = rx * bdx + rz * bdz;
        if (along <= 0.f) return false;
        float lat = rx * -bdz + rz * bdx;
        if (std::fabs(lat) > 2.f * HalfWidth + LaneMargin + 0.5f) return false;
        float faster = bspeed - speed > 0.f ? bspeed - speed : 0.f;   // it shows going its own speed
        float need = faster * faster / (2.f * Braking) + 2.f * HalfLength + StopGap + 2.f;
        return along < need;
      };
      float bx = x - s.NewDX[i] * 70.f, bz = z - s.NewDZ[i] * 70.f;
      return NearBox(s, (x < bx ? x : bx) - 6.f, (z < bz ? z : bz) - 6.f, (x > bx ? x : bx) + 6.f, (z > bz ? z : bz) + 6.f, [&](int e) {
        if (e < MaxMembers)
          return e != i && e < in.Count && s.Visible[e] && closing(s.NewX[e], s.NewY[e], s.NewZ[e], s.NewDX[e], s.NewDZ[e], s.M[e].Speed);
        Published const& p = s.Foreign[e - MaxMembers];
        return closing(p.X, p.Y, p.Z, p.DirX, p.DirZ, p.Speed);
      });
    }

    // How far car i may go this step: its gap to the seen cars in its way where they were before the step (they only
    // go forward), slowing before them. Of two cars each in the other's way: going the same way the one ahead goes,
    // else the one nearer the meeting point (each second it's waited counting as WaitBonus metres nearer).
    float Allowed(Slot& s, StepInput const& in, int i, Record const& b, Path const& path, float advGame, float cruise)
    {
      float x = b.Pos[0], y = b.Pos[1], z = b.Pos[2];
      float cx, cy, cz;
      PointAt(path, path.CarArc, cx, y, cz);
      PointAt(path, path.CarArc + Lookahead(cruise), cx, cy, cz);
      float ux = cx - x, uz = cz - z, l = Len2(ux, uz);
      if (l < 1e-4f) { ux = s.NewDX[i]; uz = s.NewDZ[i]; }
      else { ux /= l; uz /= l; }
      Member& m = s.M[i];
      float reach = Clamp(m.Speed * m.Speed / (2.f * Braking) + StopGap + 2.f * HalfLength + 10.f, 20.f, 70.f);
      float best = advGame;
      m.HeldBy = 0; m.HeldGap = 0.f; m.HeldMutual = 0;
      bool mutualHold = false;
      uint32_t myId = IdBase(s) + (uint32_t)i;
      auto consider = [&](float bx, float by, float bz, float bdx, float bdz, float bspeed, float bwait, uint32_t id) {
        if (std::fabs(by - y) > Height) return;
        float rx = bx - x, rz = bz - z;
        if (rx * rx + rz * rz > reach * reach) return;
        float along = rx * ux + rz * uz;
        if (along <= 0.f) return;
        float lat = rx * -uz + rz * ux;
        float c = ux * bdx + uz * bdz, sn = std::fabs(ux * bdz - uz * bdx);
        float hbAlong = HalfLength * std::fabs(c) + HalfWidth * sn;
        float hbLat = HalfLength * sn + HalfWidth * std::fabs(c);
        float margin = c < -0.7f ? OncomingMargin : LaneMargin;
        if (std::fabs(lat) >= HalfWidth + hbLat + margin) return;
        float gap = along - HalfLength - hbAlong;
        // am I in its way too?
        float bAlong = -(rx * bdx + rz * bdz);
        float bLat = rx * bdz - rz * bdx;
        bool mutual = bAlong > 0.f && std::fabs(bLat) < HalfWidth + hbLat + margin;
        if (mutual)
        {
          bool iGo;
          if (c >= 0.5f)
          {
            // the same way, side by side or coming together: the one ahead goes
            float mx = ux + bdx, mz = uz + bdz;
            float pa = x * mx + z * mz, pb = bx * mx + bz * mz;
            iGo = pa > pb + 0.3f || (std::fabs(pa - pb) <= 0.3f && myId < id);
          }
          else
          {
            float cross = ux * bdz - uz * bdx;
            float tA = along, tB = bAlong;
            if (std::fabs(cross) > 0.2f)
            {
              // the meeting point of the two ways: x + u tA = bx + bd tB
              tA = (rx * bdz - rz * bdx) / cross;
              tB = (rx * uz - rz * ux) / cross;
            }
            float sa = tA - WaitBonus * m.Wait, sb = tB - WaitBonus * bwait;
            iGo = sa < sb - 0.5f || (std::fabs(sa - sb) <= 0.5f && myId < id);
          }
          if (iGo) return;
          mutualHold = true;
        }
        float vLead = bspeed * (c > 0.f ? c : 0.f);
        float vMax = vLead + std::sqrt(2.f * Braking * (gap > StopGap ? gap - StopGap : 0.f));
        float a = vMax * in.Dt;
        float hard = gap - HardGap;
        if (a > hard) a = hard;
        if (a < 0.f) a = 0.f;
        if (a < best) { best = a; m.HeldBy = id; m.HeldGap = gap; m.HeldMutual = mutual ? 1 : 0; }
      };
      // the cars in the box round its way ahead
      float ex = x + ux * reach, ez = z + uz * reach;
      const float pad = 6.f;
      NearBox(s, (x < ex ? x : ex) - pad, (z < ez ? z : ez) - pad, (x > ex ? x : ex) + pad, (z > ez ? z : ez) + pad, [&](int e) {
        if (e < MaxMembers)
        {
          if (e == i || e >= in.Count || !s.Visible[e]) return false;
          Record const& o = s.Before[e];
          consider(o.Pos[0], o.Pos[1], o.Pos[2], s.WayX[e], s.WayZ[e], s.M[e].Speed, s.M[e].Wait,
            IdBase(s) + (uint32_t)e);
          return false;
        }
        Published const& p = s.Foreign[e - MaxMembers];
        consider(p.X, p.Y, p.Z, p.DirX, p.DirZ, p.Speed, p.Wait, p.Id);
        return false;
      });
      // held still for long by a car that waits for it too (they can't both go): this one waits unseen until there's room
      if (mutualHold && best < 1e-3f) m.BlockedTime += in.Dt;
      else m.BlockedTime = 0.f;
      if (m.BlockedTime > 5.f)
      {
        m.BlockedTime = 0.f;
        m.Hidden = 1;
        s.Visible[i] = 0;
        ++s.Counts.Waiting;
        return 0.f;
      }
      return best;
    }

    // The other placements' latest cars near this one's (in x, z and height)
    void GatherForeign(Slot& s, StepInput const& in, Slot* others, int otherCount)
    {
      s.ForeignCount = 0;
      if (in.Count <= 0) return;
      Record const* rec = (Record const*)in.Records;
      float minX = 1e30f, minY = 1e30f, minZ = 1e30f, maxX = -1e30f, maxY = -1e30f, maxZ = -1e30f;
      for (int i = 0; i < in.Count; ++i)
      {
        // every car that's there, seen or waiting unseen (its corner's height: an unseen one's is -10000)
        if (in.Flags[i] & DespawnedFlag) continue;
        float y = rec[i].Target[1];
        minX = rec[i].Pos[0] < minX ? rec[i].Pos[0] : minX; maxX = rec[i].Pos[0] > maxX ? rec[i].Pos[0] : maxX;
        minY = y < minY ? y : minY; maxY = y > maxY ? y : maxY;
        minZ = rec[i].Pos[2] < minZ ? rec[i].Pos[2] : minZ; maxZ = rec[i].Pos[2] > maxZ ? rec[i].Pos[2] : maxZ;
      }
      if (minX > maxX) return;
      const float pad = 80.f, hpad = Height + 1.f;
      for (int k = 0; k < otherCount; ++k)
      {
        Slot& o = others[k];
        if (&o == &s || o.Key == 0) continue;
        long seq = o.Seq;
        if (seq & 1) continue;
        if (in.Now - o.LastTime > OldSeconds) continue;
        if (o.MaxX < minX - pad || o.MinX > maxX + pad || o.MaxZ < minZ - pad || o.MinZ > maxZ + pad ||
            o.MaxY < minY - hpad || o.MinY > maxY + hpad)
          continue;
        int n = o.PubCount;
        if (n > MaxMembers) n = MaxMembers;
        int start = s.ForeignCount;
        for (int j = 0; j < n && s.ForeignCount < MaxForeign; ++j)
        {
          Published p = o.Pub[j];
          if (p.X < minX - pad || p.X > maxX + pad || p.Z < minZ - pad || p.Z > maxZ + pad || p.Y < minY - hpad || p.Y > maxY + hpad)
            continue;
          s.Foreign[s.ForeignCount++] = p;
        }
        if (o.Seq != seq) s.ForeignCount = start;    // it changed while being read: left out this step
      }
    }

    void Publish(Slot& s, StepInput const& in, Record const* rec)
    {
      s.Seq += 1;   // odd: being written
      int n = 0;
      float minX = 1e30f, minY = 1e30f, minZ = 1e30f, maxX = -1e30f, maxY = -1e30f, maxZ = -1e30f;
      for (int i = 0; i < in.Count; ++i)
      {
        if (!s.Visible[i]) continue;
        Published& p = s.Pub[n++];
        p.X = rec[i].Pos[0]; p.Y = rec[i].Pos[1]; p.Z = rec[i].Pos[2];
        p.DirX = s.NewDX[i]; p.DirZ = s.NewDZ[i];
        p.Speed = s.M[i].Speed;
        p.Wait = s.M[i].Wait;
        p.Id = IdBase(s) + (uint32_t)i;
        p.Blocker = s.M[i].Blocker;
        p.Stand = s.M[i].StandTime;
        minX = p.X < minX ? p.X : minX; maxX = p.X > maxX ? p.X : maxX;
        minY = p.Y < minY ? p.Y : minY; maxY = p.Y > maxY ? p.Y : maxY;
        minZ = p.Z < minZ ? p.Z : minZ; maxZ = p.Z > maxZ ? p.Z : maxZ;
      }
      s.PubCount = n;
      s.MinX = minX; s.MinY = minY; s.MinZ = minZ; s.MaxX = maxX; s.MaxY = maxY; s.MaxZ = maxZ;
      s.LastTime = in.Now;
      s.Seq += 1;   // even again
    }
  };
}
