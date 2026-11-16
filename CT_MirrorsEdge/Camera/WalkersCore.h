#pragma once

// Walking crowds (2026-11-03; Research_Road_Traffic_2026-11-03.md section 8): the rules the tools add to the game's own
// walker step (0x143B8CF00, the Pedestrians behaviour), with nothing of the game in here, so the same code runs in the
// DLL and in the offline test that walks the city's 114 walking placements with a copy of the game's step. The game's
// walkers never slow down: its avoidance (0x143B87690) only turns them away from the walkers in front of them on their
// own two nodes, so a walker it can't turn away in time walks through the other, two placements on one path never see
// each other, and a respawn comes in on its spawn node whoever is there. Per walking placement, after the game's step:
// - a shown walker never ends a step closer than a body (0.5 - 0.6 m, centre to centre) to another shown walker, its own
//   placement's or another's: its move is cut short (it keeps the game's heading, so the game's own avoidance turns it
//   away and it goes round); a pair already that close may only move apart;
// - a walker the game respawns starts unseen and shows once its spot is clear; one found inside another after all the
//   moves (two placements' walkers meeting) goes back, and if that doesn't help the younger waits unseen;
// - the time of day's share of them shows (Density, from the caller): above it, shown walkers out of sight (far from
//   the view, or off to its side) go unseen; below it, unseen ones out of sight show again.
// An unseen walker is at y -10000 (RoadTrafficCore.h, HiddenY: never -10042, the game's despawn height) and stands
// still: the caller marks it despawned for the game's step only (the step skips despawned members, 0x143B8D096), so
// the game neither walks it nor turns a shown walker away from it (its avoidance goes by 3D distance).
#include "RoadTrafficCore.h"
#include "WalkersWalls.h"
#include "WalkersFloors.h"
#include <algorithm>

namespace walkers_core
{
  using road_traffic_core::Record;
  using road_traffic_core::HalfToFloat;
  using road_traffic_core::FloatToHalf;
  using road_traffic_core::SpeedTable;
  using road_traffic_core::Clamp;
  using road_traffic_core::Len2;
  using road_traffic_core::RecordSize;
  using road_traffic_core::NodeSize;
  using road_traffic_core::NodeRadius;
  using road_traffic_core::DespawnedFlag;
  using road_traffic_core::HiddenY;
  using road_traffic_core::OutOfSight;
  using road_traffic_core::Wanted;

  // ---- the rules' sizes ----
  const float BodyMin = 0.55f;       // centre to centre two shown walkers never come closer than (bodies 0.45 m across):
  const float BodyMax = 0.6f;        // their two MemberRadius, between these
  const float Follow = 1.6f;         // bodies of room a walker keeps in front of it (0.96 m behind another)
  const float SideLook = 2.5f;       // metres ahead a walker sidesteps one coming the other way or across from
  const float SideRoom = 0.15f;      // and to how far beside it, past a body
  const float SideSpeed = 0.9f;      // m/s it sidesteps at, at most
  const float PathEdge = 0.2f;       // how far past its path's width (the larger radius of its two nodes) it may go
  const float StuckSeconds = 4.f;    // a walker that hasn't got on for this long (see Ghost) ...
  const float GhostSeconds = 1.5f;   // walks the game's way for this long, through whoever is there, unless out of
                                     // sight: then it waits unseen until its spot is clear
  const float ClearExtra = 0.25f;    // an unseen walker shows where no shown one is within a body and this
  const float Slack = 0.1f;          // after all moves, closer than a body by more than this is inside (two placements
                                     // step apart, each against where the other was)
  const float Height = 1.5f;         // walkers further apart than this in height aren't in each other's way (stairs)
  const int MaxMembers = 640;        // a placement's members the rules look after (the city's largest has 300, 600 raised)
  const int MaxForeign = 1024;       // other placements' walkers looked at in one step
  const float OldSeconds = 0.5f;     // another placement's walkers older than this are left out
  const float Cell = 2.f;            // the neighbour grid's cells
  const int GridBuckets = 1024;
  const float FarAway = 150.f;       // out of sight: further from the view than this (a 1.8 m walker is ~11 px at 1080p)
  const float SideNear = 8.f;        // or nearer, but further than this and off to the view's side (see OutOfSight)
  const int DensityPerStep = 2;      // walkers the time of day's share hides or shows in one step, at most
  const float WallPullSeen = 0.6f;   // m/s a shown walker in sight is pulled back toward its path's line out of a wall
                                     // (WalkersWalls.h; slower than a sidestep, SideSpeed)
  const float WallPullUnseen = 3.f;  // out of sight

  // The walls table's segments by their ends (WalkersWalls.h: matched by the game's node positions within 2 cm, their
  // radii and the placement's MemberRadius, never by node index: only 41 of the 114 walking placements keep the
  // table's node order). Build() once before the first step.
  struct WallIndex
  {
    static const int Buckets = 4096;
    int Head[Buckets];
    int Next[walkers_walls::SegmentCount];
    bool Built = false;

    static unsigned Cell(int qx, int qz) { return ((unsigned)qx * 73856093u ^ (unsigned)qz * 19349663u) % Buckets; }

    void Build()
    {
      for (int i = 0; i < Buckets; ++i) Head[i] = -1;
      for (int s = 0; s < walkers_walls::SegmentCount; ++s)
      {
        walkers_walls::Segment const& g = walkers_walls::Segments[s];
        unsigned h = Cell((int)std::floor(g.Ax), (int)std::floor(g.Az));
        Next[s] = Head[h];
        Head[h] = s;
      }
      Built = true;
    }

    // The segment between node positions p and q (either way round) with radii rp, rq and member radius mr, -1 none
    int Find(float const* p, float rp, float const* q, float rq, float mr) const
    {
      if (!Built) return -1;
      for (int e = 0; e < 2; ++e)
      {
        float const* a = e ? q : p;
        float const* c = e ? p : q;
        float ra = e ? rq : rp, rc = e ? rp : rq;
        int qx = (int)std::floor(a[0]), qz = (int)std::floor(a[2]);
        for (int dx = -1; dx <= 1; ++dx)
          for (int dz = -1; dz <= 1; ++dz)
            for (int s = Head[Cell(qx + dx, qz + dz)]; s >= 0; s = Next[s])
            {
              walkers_walls::Segment const& g = walkers_walls::Segments[s];
              if (std::fabs(g.Ax - a[0]) < 0.02f && std::fabs(g.Ay - a[1]) < 0.02f && std::fabs(g.Az - a[2]) < 0.02f &&
                  std::fabs(g.Bx - c[0]) < 0.02f && std::fabs(g.By - c[1]) < 0.02f && std::fabs(g.Bz - c[2]) < 0.02f &&
                  std::fabs(g.Ra - ra) < 0.01f && std::fabs(g.Rb - rc) < 0.01f && std::fabs(g.Mr - mr) < 0.01f)
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
    float Y;                  // its height while unseen (where it shows again)
    float Speed;              // m/s it moved last step
    float Wait;               // seconds its move has been cut to under a quarter
    float Ghost;              // seconds left of walking the game's way through the others (it was stuck)
    uint32_t Age;             // steps since it appeared
    uint8_t Alive;            // seen not despawned last step
    uint8_t Hidden;           // 0 shown; 1 waiting unseen for its spot to clear; 2 unseen for the time of day's share
    uint8_t Despawned;        // despawned at its last step (when it's there again, the game has respawned it)
    uint32_t WallKey;         // the leg its walls segment is for: (From + 1) << 16 | (To + 1), 0 none looked up yet
    int WallSeg;              // that leg's segment in WalkersWalls.h, -1 none
  };

  struct Published            // a walker as the other placements see it
  {
    float X, Y, Z, DirX, DirZ;
    uint32_t Id;
  };

  struct Stats
  {
    long long Steps = 0, Walkers = 0, Held = 0, Waiting = 0, Revealed = 0, Appeared = 0, Respawned = 0, Reverted = 0;
    long long DensityHidden = 0, DensityShown = 0, Sidesteps = 0, Ghosts = 0, StuckUnseen = 0;
    long long FarCorners = 0;  // corner targets far off the path put back on it (After)
    long long WallPulled = 0, WallRefused = 0, WallTargets = 0, WallFound = 0, WallMissing = 0;   // the walls rule
    long long FloorTargets = 0, FloorSet = 0;   // corner targets set to the street's height; walkers put on it appearing
  };

  struct Slot
  {
    volatile long Seq;              // even while Pub is stable (a seqlock)
    uintptr_t Key;                  // the placement
    uintptr_t Records, Nodes;       // its records' and nodes' addresses and counts: a change starts it again
    int Count, NodeCount;
    double LastTime;                // seconds (the caller's clock) of its last step
    float MinX, MinY, MinZ, MaxX, MaxY, MaxZ;   // its published walkers' box
    int PubCount;
    Published Pub[MaxMembers];
    Member M[MaxMembers];
    Record Before[MaxMembers];      // the records as the game's step got them
    float NewX[MaxMembers], NewY[MaxMembers], NewZ[MaxMembers];   // where each shown one is now (before its move: was)
    float WayX[MaxMembers], WayZ[MaxMembers];   // the way each goes this step (the game's move, or its heading)
    uint8_t Visible[MaxMembers];
    uint8_t HeldNow[MaxMembers];    // its move this step was cut to under a quarter, or put back
    uint8_t Frozen[MaxMembers];     // marked despawned for the game's step by the caller (see Freeze / Thaw)
    Published Foreign[MaxForeign];
    int ForeignCount;
    int GridHead[GridBuckets];
    int GridNext[MaxMembers + MaxForeign];
    float GridX[MaxMembers + MaxForeign], GridY[MaxMembers + MaxForeign], GridZ[MaxMembers + MaxForeign];
    float GridR[MaxMembers + MaxForeign];   // its MemberRadius
    float GridUX[MaxMembers + MaxForeign], GridUZ[MaxMembers + MaxForeign];   // its way
    int DensityNext;                // where the time of day's share looks next
    float Radius;                   // this placement's MemberRadius
    Stats Counts;                   // only its own thread writes them (one step of a placement at a time)
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
    float Radius;             // the settings' MemberRadius
    float Dt;                 // the step's seconds
    double Now;               // a clock in seconds (for the other placements' walkers' age)
    float Density;            // the share of its members to show (0 - 1)
    bool ViewKnown;           // the view: where it is and its axis (OutOfSight)
    float ViewX, ViewY, ViewZ, ViewAX, ViewAY, ViewAZ;
    bool InSightToo = false;  // the crowd test (MISC > Performance) runs: the share hides and shows in sight too, as
                              // many a step as it takes
  };

  class Rules
  {
  public:
    WallIndex Walls;               // Build() it once before the first step

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
      Reset(*s, in);
      return s;
    }

    static Slot* FindExisting(uintptr_t key, Slot* storage, int storageCount)
    {
      for (int i = 0; i < storageCount; ++i)
        if (storage[i].Key == key) return &storage[i];
      return nullptr;
    }

    // The rules can't go on with this placement: its unseen walkers show where they are, at their own height. Returns
    // how many.
    int Release(Slot& s, StepInput const& in)
    {
      if ((uintptr_t)in.Records != s.Records || in.Count != s.Count) return 0;
      Record* rec = (Record*)in.Records;
      int shown = 0;
      for (int i = 0; i < in.Count; ++i)
      {
        Member& m = s.M[i];
        if (!m.Alive || !m.Hidden || (in.Flags[i] & DespawnedFlag) || !(rec[i].Pos[1] < -9000.f)) continue;
        rec[i].Pos[1] = m.Y;
        m.Hidden = 0;
        ++shown;
      }
      return shown;
    }

    // Before the game's step: what each walker was
    void Before(Slot& s, StepInput const& in)
    {
      if ((uintptr_t)in.Records != s.Records || in.Count != s.Count || (uintptr_t)in.Nodes != s.Nodes || in.NodeCount != s.NodeCount)
        Reset(s, in);
      s.Radius = in.Radius;
      memcpy(s.Before, in.Records, (size_t)in.Count * RecordSize);
    }

    // For the game's step only: the unseen walkers marked despawned, so the step leaves them where they are (the caller
    // calls Thaw right after the step, on the same thread)
    void Freeze(Slot& s, StepInput const& in)
    {
      if ((uintptr_t)in.Records != s.Records || in.Count != s.Count) return;
      Record* rec = (Record*)in.Records;
      for (int i = 0; i < in.Count; ++i)
      {
        s.Frozen[i] = 0;
        Member const& m = s.M[i];
        if (m.Alive && m.Hidden && !(in.Flags[i] & DespawnedFlag) && rec[i].Pos[1] < -9000.f)
        {
          in.Flags[i] |= DespawnedFlag;
          s.Frozen[i] = 1;
        }
      }
    }

    void Thaw(Slot& s, StepInput const& in)
    {
      if ((uintptr_t)in.Records != s.Records || in.Count != s.Count) return;
      for (int i = 0; i < in.Count; ++i)
        if (s.Frozen[i])
        {
          in.Flags[i] &= (uint16_t)~DespawnedFlag;
          s.Frozen[i] = 0;
        }
    }

    // After the game's step: the rules (others: every slot, this one too; read under their seqlock)
    void After(Slot& s, StepInput const& in, Slot* others, int otherCount)
    {
      ++s.Counts.Steps;
      GatherForeign(s, in, others, otherCount);
      Record* rec = (Record*)in.Records;

      // the game's corner (0x143B92550) is where the two legs' side-offset lines cross (0x143B966F0); its guard against
      // parallel legs, |cross| > 0.01, is on the legs' unscaled vectors, so at a turn of nearly 180 degrees the point
      // lies tens to hundreds of metres away and the walker walks straight off its path toward it, holding its height:
      // in the air beside a raised walkway (2026-11-09, the user: "some of their pathing is in the air"; live, 138 of
      // 4,926 shown walkers had such a target, 11 of them 7 - 18 m above the ground). A target further than two of its
      // node's radii + 0.5 m from it (the offset lines cross at most the radius / cos(half the turn) away, so only at
      // turns over 120 degrees) becomes the game's own point for parallel legs: the To node offset to the side of
      // From -> To (0x143B927CA); its height (the To node's) is kept
      for (int i = 0; i < in.Count; ++i)
      {
        if (in.Flags[i] & DespawnedFlag) continue;
        Record& r = rec[i];
        if (r.From >= in.NodeCount || r.To >= in.NodeCount || r.From == r.To) continue;
        float const* T = (float const*)(in.Nodes + (size_t)r.To * NodeSize);
        float rt = *(float const*)(in.Nodes + (size_t)r.To * NodeSize + NodeRadius);
        float fx = r.Target[0] - T[0], fz = r.Target[2] - T[2], lim = 2.f * rt + 0.5f;
        if (fx * fx + fz * fz <= lim * lim) continue;
        float const* F = (float const*)(in.Nodes + (size_t)r.From * NodeSize);
        float dx = T[0] - F[0], dz = T[2] - F[2], L = Len2(dx, dz);
        if (L < 1e-3f) continue;
        float off = HalfToFloat(r.Side) * rt;
        r.Target[0] = T[0] + off * dz / L;
        r.Target[2] = T[2] - off * dx / L;
        ++s.Counts.FarCorners;
      }

      // walls (2026-11-09, the user: "make sure that pedestrians don't walk inside walls"): the game puts each walker
      // somewhere across its path's width (Side x the node radius, never checked against anything) and its paths are
      // often wider than the walkway, so walkers walk in walls beside them (live, 6.6 % of shown walkers in a drawn
      // wall). A corner target in the first obstacle beside its leg (WalkersWalls.h) moves out to that obstacle's
      // face, toward the line; its height is kept
      for (int i = 0; i < in.Count; ++i)
      {
        if (in.Flags[i] & DespawnedFlag) continue;
        Record& r = rec[i];
        int g = WallSegment(s, in, i, r.From, r.To);
        if (g < 0) continue;
        float ex, ez;
        float d = WallDepth(g, r.Target[0], r.Target[2], ex, ez);
        if (d <= 0.f) continue;
        r.Target[0] += ex * (d + 0.01f);
        r.Target[2] += ez * (d + 0.01f);
        ++s.Counts.WallTargets;
      }

      // on the street (2026-11-15, the user: "make sure that every car and traffic in general are actually on the
      // ground"): the game's height runs straight from a walker to its corner target, at its To node's height, and on
      // the street a fifth of the walking nodes are off the drawn pavement Faith stands on (whole placements 16 cm in
      // it, one loop 1 m under it, some up to 1.1 m over it). A target there takes the street's height
      // (WalkersFloors.h), so the walker walks straight between street heights; elsewhere (roofs, walkways: no floor
      // data) it's the game's
      for (int i = 0; i < in.Count; ++i)
      {
        if (in.Flags[i] & DespawnedFlag) continue;
        Record& r = rec[i];
        int g = WallSegment(s, in, i, r.From, r.To);
        if (g < 0) continue;
        float h = NodeFloor(g, (float const*)(in.Nodes + (size_t)r.To * NodeSize));
        if (h > -1e29f && r.Target[1] != h)
        {
          r.Target[1] = h;
          ++s.Counts.FloorTargets;
        }
      }

      // who's there, and where each was before the step
      for (int i = 0; i < in.Count; ++i)
      {
        Member& m = s.M[i];
        s.Visible[i] = 0;
        if (in.Flags[i] & DespawnedFlag) { m.Alive = 0; m.Hidden = 0; m.Despawned = 1; continue; }
        Record const& b = s.Before[i];
        bool unseenStill = m.Alive && m.Hidden && b.Pos[1] < -9000.f;
        bool appeared = !m.Alive || std::fabs(b.Pos[0] - m.LastX) > 0.5f || std::fabs(b.Pos[2] - m.LastZ) > 0.5f;
        if (appeared && !unseenStill)
        {
          ++s.Counts.Appeared;
          // one the game respawned (it was despawned at its last step) starts unseen and shows once its spot is clear
          // (the game respawns before its step and draws after it); the others (a placement's walkers at its first
          // step) stay as they are, unless inside another (below)
          bool respawned = m.Despawned != 0;
          m.Alive = 1;
          m.Despawned = 0;
          m.Age = 0;
          m.Speed = 0.f;
          m.Wait = 0.f;
          m.Ghost = 0.f;
          m.Hidden = respawned ? 1 : 0;
          // on the street's height where its leg has one (see the targets above), from its first step
          float fy = rec[i].Pos[1] > -9000.f ? rec[i].Pos[1] : b.Pos[1];
          float fx = rec[i].Pos[1] > -9000.f ? rec[i].Pos[0] : b.Pos[0], fz = rec[i].Pos[1] > -9000.f ? rec[i].Pos[2] : b.Pos[2];
          if (LegFloor(s, in, i, rec[i], fx, fz, fy))
          {
            ++s.Counts.FloorSet;
            if (!respawned && rec[i].Pos[1] > -9000.f) rec[i].Pos[1] = fy;
          }
          if (respawned)
          {
            ++s.Counts.Respawned;
            Unsee(s, rec, i, fy, 1);
          }
        }
        s.Visible[i] = m.Hidden == 0 ? 1 : 0;
        s.NewX[i] = b.Pos[0]; s.NewY[i] = b.Pos[1]; s.NewZ[i] = b.Pos[2];
        float wx = rec[i].Pos[0] - b.Pos[0], wz = rec[i].Pos[2] - b.Pos[2], wl = Len2(wx, wz);
        if (wl > 1e-4f) { s.WayX[i] = wx / wl; s.WayZ[i] = wz / wl; }
        else
        {
          float h = HalfToFloat(rec[i].Heading) * 0.0174532925f;
          s.WayX[i] = std::sin(h); s.WayZ[i] = std::cos(h);
        }
      }
      BuildGrid(s, in);

      // one shown at its first step inside another waits unseen
      for (int i = 0; i < in.Count; ++i)
      {
        Member& m = s.M[i];
        if (!m.Alive || m.Age != 0 || m.Hidden) continue;
        if (Crowded(s, in, i, s.NewX[i], s.NewY[i], s.NewZ[i], 0.f))
        {
          Unsee(s, rec, i, rec[i].Pos[1], 1);
          ++s.Counts.Waiting;
        }
      }

      // each shown walker: the game's move, a sidestep from walkers coming the other way or across in front of it, then
      // its way on as far as it doesn't come too close to another (Closeness)
      for (int i = 0; i < in.Count; ++i)
      {
        Member& m = s.M[i];
        if (!m.Alive) continue;
        ++s.Counts.Walkers;
        ++m.Age;
        s.HeldNow[i] = 0;
        if (m.Hidden) continue;
        Record& r = rec[i];
        Record const& b = s.Before[i];
        float mx = r.Pos[0] - b.Pos[0], my = r.Pos[1] - b.Pos[1], mz = r.Pos[2] - b.Pos[2];
        float cruise = in.SpeedMin + (in.SpeedMax - in.SpeedMin) * SpeedTable[i % 12];
        float most = cruise * in.Dt * 1.05f + 0.05f;
        float len = Len2(mx, mz);
        if (!(len <= most) || !(std::fabs(my) < 2.f))
        {
          // not a walk (the game moved it further than it walks in a step): left as the game has it
          s.NewX[i] = r.Pos[0]; s.NewY[i] = r.Pos[1]; s.NewZ[i] = r.Pos[2];
          m.Speed = 0.f;
          continue;
        }
        float ux = s.WayX[i], uz = s.WayZ[i];
        if (m.Ghost > 0.f)
        {
          // stuck for long, in sight: the game's way through the others, for a moment
          m.Ghost -= in.Dt;
          s.NewX[i] = r.Pos[0]; s.NewY[i] = r.Pos[1]; s.NewZ[i] = r.Pos[2];
          m.Speed = in.Dt > 0.f ? len / in.Dt : 0.f;
          continue;
        }
        float px = b.Pos[0], pz = b.Pos[2];
        float lat = Sidestep(s, in, i, ux, uz);
        if (lat != 0.f)
        {
          float qx = px + uz * lat, qz = pz - ux * lat;
          if (!Blocked(s, in, i, qx, b.Pos[1], qz, ux, uz)) { px = qx; pz = qz; ++s.Counts.Sidesteps; }
        }
        float t = 1.f;
        if (len > 1e-5f && Blocked(s, in, i, px + mx, b.Pos[1] + my, pz + mz, ux, uz))
        {
          float lo = 0.f, hi = 1.f;
          for (int k = 0; k < 5; ++k)
          {
            float mid = 0.5f * (lo + hi);
            if (Blocked(s, in, i, px + mx * mid, b.Pos[1] + my * mid, pz + mz * mid, ux, uz)) hi = mid;
            else lo = mid;
          }
          t = lo;
          ++s.Counts.Held;
        }
        r.Pos[0] = px + mx * t; r.Pos[1] = b.Pos[1] + my * t; r.Pos[2] = pz + mz * t;
        WallPull(s, in, i, r);
        s.NewX[i] = r.Pos[0]; s.NewY[i] = r.Pos[1]; s.NewZ[i] = r.Pos[2];
        m.Speed = in.Dt > 0.f ? Len2(r.Pos[0] - b.Pos[0], r.Pos[2] - b.Pos[2]) / in.Dt : 0.f;
        s.HeldNow[i] = (t < 0.25f && len > 1e-5f) ? 1 : 0;
      }

      // nothing ends inside another walker (each move was checked against the others as they were then; this is
      // against all, another placement's that came this way too): one that moved goes back to where it was; one inside
      // another where it was, the younger of the two waits unseen
      for (int i = 0; i < in.Count; ++i)
      {
        if (!s.Visible[i] || s.M[i].Ghost > 0.f) continue;
        int other = -1;
        if (!Crowded(s, in, i, s.NewX[i], s.NewY[i], s.NewZ[i], -Slack, &other)) continue;
        Record const& b = s.Before[i];
        if (std::fabs(s.NewX[i] - b.Pos[0]) > 1e-4f || std::fabs(s.NewZ[i] - b.Pos[2]) > 1e-4f)
        {
          rec[i].Pos[0] = b.Pos[0]; rec[i].Pos[1] = b.Pos[1]; rec[i].Pos[2] = b.Pos[2];
          s.NewX[i] = b.Pos[0]; s.NewY[i] = b.Pos[1]; s.NewZ[i] = b.Pos[2];
          s.M[i].Speed = 0.f;
          s.HeldNow[i] = 1;
          ++s.Counts.Reverted;
          if (!Crowded(s, in, i, s.NewX[i], s.NewY[i], s.NewZ[i], -Slack, &other)) continue;
        }
        int hide = (other >= 0 && s.Visible[other] && s.M[other].Ghost <= 0.f && s.M[other].Age < s.M[i].Age) ? other : i;
        Unsee(s, rec, hide, rec[hide].Pos[1], 1);
        ++s.Counts.Waiting;
      }

      // held for long (StuckSeconds): out of sight it waits unseen until its spot is clear, in sight it walks the
      // game's way through the others for GhostSeconds
      for (int i = 0; i < in.Count; ++i)
      {
        Member& m = s.M[i];
        if (!s.Visible[i] || m.Ghost > 0.f) { m.Wait = 0.f; continue; }
        m.Wait = s.HeldNow[i] ? m.Wait + in.Dt : 0.f;
        if (m.Wait <= StuckSeconds) continue;
        m.Wait = 0.f;
        if (Unseen(in, s.NewX[i], s.NewY[i], s.NewZ[i]))
        {
          Unsee(s, rec, i, s.NewY[i], 1);
          ++s.Counts.StuckUnseen;
        }
        else
        {
          m.Ghost = GhostSeconds;
          ++s.Counts.Ghosts;
        }
      }

      // the unseen ones: a waiting one shows once its spot is clear (looked at every fourth step, in turn), unless the
      // time of day's share is full (it's then unseen for that); one unseen for the share shows when the share has
      // room, out of sight, its spot clear
      int shown = 0;
      for (int i = 0; i < in.Count; ++i) shown += s.Visible[i];
      int wanted = Wanted(in.Density, in.Count);
      for (int i = 0; i < in.Count; ++i)
      {
        Member& m = s.M[i];
        if (!m.Alive || !m.Hidden || ((unsigned)i + (unsigned)s.Counts.Steps) % 4u != 0u) continue;
        float x = rec[i].Pos[0], z = rec[i].Pos[2];
        if (m.Hidden == 1)
        {
          if (Crowded(s, in, i, x, m.Y, z, ClearExtra)) continue;
          if (shown >= wanted) { m.Hidden = 2; continue; }
        }
        else
        {
          // (with no view known, wherever its spot is clear: the caller has no share then)
          if (shown >= wanted || (!in.InSightToo && in.ViewKnown && !Unseen(in, x, m.Y, z)) || Crowded(s, in, i, x, m.Y, z, ClearExtra)) continue;
          ++s.Counts.DensityShown;
        }
        Show(s, rec, i);
        ++shown;
        ++s.Counts.Revealed;
      }
      // above the share: shown ones out of sight go unseen (a few a step, from where the last step stopped)
      if (shown > wanted && in.Count > 0)
      {
        int done = 0;
        int most = in.InSightToo ? in.Count : DensityPerStep;
        for (int k = 0; k < in.Count && shown > wanted && done < most; ++k)
        {
          int i = (s.DensityNext + k) % in.Count;
          if (!s.Visible[i] || s.M[i].Age < 2) continue;
          if (!in.InSightToo && !Unseen(in, s.NewX[i], s.NewY[i], s.NewZ[i])) continue;
          Unsee(s, rec, i, s.NewY[i], 2);
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
      for (int i = 0; i < MaxMembers; ++i) { memset(&s.M[i], 0, sizeof(Member)); s.Frozen[i] = 0; }
    }

    static bool Unseen(StepInput const& in, float x, float y, float z)
    {
      if (!in.ViewKnown) return false;
      return OutOfSight(x, y, z, in.ViewX, in.ViewY, in.ViewZ, in.ViewAX, in.ViewAY, in.ViewAZ, FarAway, SideNear);
    }

    void Unsee(Slot& s, Record* rec, int i, float y, uint8_t why)
    {
      Member& m = s.M[i];
      m.Hidden = why;
      m.Y = y;
      m.Speed = 0.f;
      m.Wait = 0.f;
      m.Ghost = 0.f;
      s.Visible[i] = 0;
      rec[i].Pos[1] = HiddenY;
    }

    void Show(Slot& s, Record* rec, int i)
    {
      Member& m = s.M[i];
      m.Hidden = 0;
      rec[i].Pos[1] = m.Y;
      s.Visible[i] = 1;
      s.NewX[i] = rec[i].Pos[0]; s.NewY[i] = m.Y; s.NewZ[i] = rec[i].Pos[2];
      s.Before[i].Pos[0] = rec[i].Pos[0]; s.Before[i].Pos[1] = m.Y; s.Before[i].Pos[2] = rec[i].Pos[2];
    }

    static float Body(float ra, float rb) { return Clamp(ra + rb, BodyMin, BodyMax); }

    static unsigned Bucket(float x, float z)
    {
      int gx = (int)std::floor(x / Cell), gz = (int)std::floor(z / Cell);
      return ((unsigned)gx * 73856093u ^ (unsigned)gz * 19349663u) % GridBuckets;
    }

    // The neighbour grid: own walkers (index < Count, looked up live in NewX..), the others' after them
    void BuildGrid(Slot& s, StepInput const& in)
    {
      for (int b = 0; b < GridBuckets; ++b) s.GridHead[b] = -1;
      for (int i = 0; i < in.Count; ++i)
      {
        if (!s.Visible[i]) continue;
        unsigned b = Bucket(s.NewX[i], s.NewZ[i]);
        s.GridNext[i] = s.GridHead[b];
        s.GridHead[b] = i;
      }
      for (int k = 0; k < s.ForeignCount; ++k)
      {
        int e = MaxMembers + k;
        s.GridX[e] = s.Foreign[k].X; s.GridY[e] = s.Foreign[k].Y; s.GridZ[e] = s.Foreign[k].Z;
        s.GridUX[e] = s.Foreign[k].DirX; s.GridUZ[e] = s.Foreign[k].DirZ;
        unsigned b = Bucket(s.GridX[e], s.GridZ[e]);
        s.GridNext[e] = s.GridHead[b];
        s.GridHead[b] = e;
      }
    }

    // Own walker j's position now (it may have moved this step: the grid holds it where it was, a cell is 2 m and a
    // step under 0.3 m, so the 3 x 3 cells round still have it)
    void At(Slot const& s, int e, float& x, float& y, float& z, float& r) const
    {
      if (e < MaxMembers) { x = s.NewX[e]; y = s.NewY[e]; z = s.NewZ[e]; r = s.Radius; }
      else { x = s.GridX[e]; y = s.GridY[e]; z = s.GridZ[e]; r = s.GridR[e]; }
    }

    void WayOf(Slot const& s, int e, float& ux, float& uz) const
    {
      if (e < MaxMembers) { ux = s.WayX[e]; uz = s.WayZ[e]; }
      else { ux = s.GridUX[e]; uz = s.GridUZ[e]; }
    }

    // How far (metres, + to its right) walker i steps aside this step for the walkers coming the other way or across
    // within SideLook in front of it and less than a body and SideRoom to its side: away from each, one dead ahead to its
    // own right (two coming at each other both keep right), the nearer the more; at most SideSpeed, and not further from
    // its path's node line than the larger radius of its two nodes and PathEdge (the game's own walkers keep within the
    // radius: their side offset is -1 .. 1 of it) unless it's that far already
    float Sidestep(Slot const& s, StepInput const& in, int i, float ux, float uz) const
    {
      Record const& b = s.Before[i];
      float rx = uz, rz = -ux;
      float lat = 0.f;
      int gx = (int)std::floor(b.Pos[0] / Cell), gz = (int)std::floor(b.Pos[2] / Cell);
      for (int dx = -2; dx <= 2; ++dx)
        for (int dz = -2; dz <= 2; ++dz)
        {
          unsigned bk = ((unsigned)(gx + dx) * 73856093u ^ (unsigned)(gz + dz) * 19349663u) % GridBuckets;
          for (int e = s.GridHead[bk]; e >= 0; e = s.GridNext[e])
          {
            if (e == i || (e < MaxMembers && !s.Visible[e])) continue;
            float ox, oy, oz, orad;
            At(s, e, ox, oy, oz, orad);
            if (std::fabs(oy - b.Pos[1]) > Height) continue;
            float ex = ox - b.Pos[0], ez = oz - b.Pos[2];
            float ahead = ex * ux + ez * uz, side = ex * rx + ez * rz;
            float body = Body(in.Radius, orad);
            if (ahead < -0.1f || ahead > SideLook || std::fabs(side) >= body + SideRoom) continue;
            float oux, ouz;
            WayOf(s, e, oux, ouz);
            if (oux * ux + ouz * uz >= 0.5f) continue;   // going its way: it follows that one
            float need = body + SideRoom - std::fabs(side);
            float dir = side > 0.05f ? -1.f : 1.f;
            lat += dir * need * Clamp(1.f - ahead / SideLook, 0.2f, 1.f);
          }
        }
      float most = SideSpeed * in.Dt;
      lat = Clamp(lat, -most, most);
      if (lat == 0.f || b.From >= in.NodeCount || b.To >= in.NodeCount || b.From == b.To) return lat;
      float const* f = (float const*)(in.Nodes + (size_t)b.From * NodeSize);
      float const* t = (float const*)(in.Nodes + (size_t)b.To * NodeSize);
      float lx = t[0] - f[0], lz = t[2] - f[2], ll = Len2(lx, lz);
      if (ll < 1e-3f) return lat;
      float nx = lz / ll, nz = -lx / ll;
      float now = (b.Pos[0] - f[0]) * nx + (b.Pos[2] - f[2]) * nz;
      float k = rx * nx + rz * nz;                    // a metre of sidestep is k metres off the line
      float after = now + lat * k;
      float rf = *(float const*)(in.Nodes + (size_t)b.From * NodeSize + NodeRadius);
      float rt = *(float const*)(in.Nodes + (size_t)b.To * NodeSize + NodeRadius);
      float band = (std::max)(rf, rt) + PathEdge;
      float limit = (std::max)(band, std::fabs(now));
      if (std::fabs(after) <= limit) return lat;
      if (std::fabs(k) < 0.1f) return 0.f;
      float fit = ((after > 0.f ? limit : -limit) - now) / k;
      return fit * lat > 0.f ? (std::fabs(fit) < std::fabs(lat) ? fit : lat) : 0.f;
    }

    // How close walker i at (x, z), going along (ux, uz), is to one at (ox, oz): under 1 is too close. A body all round,
    // and a walker's length of room in front (Follow bodies: walking behind one it keeps that, beside one a body)
    static float Closeness(float x, float z, float ux, float uz, float ox, float oz, float body)
    {
      float ex = ox - x, ez = oz - z;
      float ahead = ex * ux + ez * uz;
      float side = ex * uz - ez * ux;
      if (ahead <= 0.f) return (ex * ex + ez * ez) / (body * body);
      float f = body * Follow;
      return (ahead * ahead) / (f * f) + (side * side) / (body * body);
    }

    // Walker i at (x, y, z), going along (ux, uz), would be too close to another shown walker (Closeness): one it was
    // already that close to blocks only a move that brings them closer
    // The walls segment of walker i's leg a -> b (cached per walker while its leg stays the same), -1 none
    // The street's height at a node of walls segment g (WalkersFloors.h, its end A or B by position), or NoFloor
    static float NodeFloor(int g, float const* node)
    {
      walkers_walls::Segment const& S = walkers_walls::Segments[g];
      walkers_floors::Ends const& F = walkers_floors::Floor[g];
      if (std::fabs(S.Ax - node[0]) < 0.02f && std::fabs(S.Ay - node[1]) < 0.02f && std::fabs(S.Az - node[2]) < 0.02f)
        return F.A;
      if (std::fabs(S.Bx - node[0]) < 0.02f && std::fabs(S.By - node[1]) < 0.02f && std::fabs(S.Bz - node[2]) < 0.02f)
        return F.B;
      return walkers_floors::NoFloor;
    }

    // Its leg's height at (x, z) with the street's at either end where it has one (else the node's): false where
    // neither end has one (the game's own height is left)
    bool LegFloor(Slot& s, StepInput const& in, int i, Record const& r, float x, float z, float& y)
    {
      int g = WallSegment(s, in, i, r.From, r.To);
      if (g < 0) return false;
      float const* p = (float const*)(in.Nodes + (size_t)r.From * NodeSize);
      float const* q = (float const*)(in.Nodes + (size_t)r.To * NodeSize);
      float hp = NodeFloor(g, p), hq = NodeFloor(g, q);
      if (!(hp > -1e29f) && !(hq > -1e29f)) return false;
      if (!(hp > -1e29f)) hp = p[1];
      if (!(hq > -1e29f)) hq = q[1];
      float dx = q[0] - p[0], dz = q[2] - p[2], l2 = dx * dx + dz * dz;
      float t = l2 > 1e-6f ? Clamp(((x - p[0]) * dx + (z - p[2]) * dz) / l2, 0.f, 1.f) : 1.f;
      y = hp + (hq - hp) * t;
      return true;
    }

    int WallSegment(Slot& s, StepInput const& in, int i, unsigned a, unsigned b)
    {
      if (!Walls.Built || a >= (unsigned)in.NodeCount || b >= (unsigned)in.NodeCount || a == b) return -1;
      Member& m = s.M[i];
      uint32_t key = ((a + 1u) << 16) | (b + 1u);
      if (m.WallKey == key) return m.WallSeg;
      float const* p = (float const*)(in.Nodes + (size_t)a * NodeSize);
      float const* q = (float const*)(in.Nodes + (size_t)b * NodeSize);
      float rp = *(float const*)(in.Nodes + (size_t)a * NodeSize + NodeRadius);
      float rq = *(float const*)(in.Nodes + (size_t)b * NodeSize + NodeRadius);
      int g = Walls.Find(p, rp, q, rq, in.Radius);
      m.WallKey = key;
      m.WallSeg = g;
      if (g >= 0) ++s.Counts.WallFound; else ++s.Counts.WallMissing;
      return g;
    }

    // How far (x, z) is into the first obstacle beside segment g's line on its side (WalkersWalls.h), and (ex, ez) the
    // way back toward the line; 0 when it's clear, past that obstacle, beyond the samples, or the line point itself is
    // in a wall (the game's way there). Both samples either side of it count (a wall can start between two)
    static float WallDepth(int g, float x, float z, float& ex, float& ez)
    {
      walkers_walls::Segment const& S = walkers_walls::Segments[g];
      ex = ez = 0.f;
      if (S.Count <= 0) return 0.f;
      float dx = S.Bx - S.Ax, dz = S.Bz - S.Az, L = Len2(dx, dz);
      if (L < 1e-4f) return 0.f;
      float ux = dx / L, uz = dz / L, nx = uz, nz = -ux;   // n: the table's right
      float rx = x - S.Ax, rz = z - S.Az;
      float along = rx * ux + rz * uz, lat = rx * nx + rz * nz;
      float kf = (along - S.Start) / walkers_walls::Step;
      if (!(kf > -1.f && kf < (float)S.Count)) return 0.f;
      int k0 = (int)std::floor(kf);
      bool right = lat >= 0.f;
      float o = std::fabs(lat) * 100.f, depth = 0.f;
      for (int k = k0; k <= k0 + 1; ++k)
      {
        if (k < 0 || k >= S.Count) continue;
        short const* f = &walkers_walls::Free[4 * (S.First + k)];
        if (f[0] < 0) return 0.f;
        int freeCm = right ? f[2] : f[0], endCm = right ? f[3] : f[1];
        if (freeCm == 32767 || o <= (float)freeCm || (endCm != 32767 && o >= (float)endCm)) continue;
        float d = (o - (float)freeCm) * 0.01f;
        if (d > depth) depth = d;
      }
      if (depth <= 0.f) return 0.f;
      ex = right ? -nx : nx;
      ez = right ? -nz : nz;
      return depth;
    }

    // A shown walker whose body is in the first obstacle beside its leg: back toward the line, at most WallPullSeen
    // (WallPullUnseen out of sight) a second and only where it isn't too close to another walker (Blocked: then it's
    // left this step as it is). It's never pushed away from the line, never held, and never touched where it's clear.
    void WallPull(Slot& s, StepInput const& in, int i, Record& r)
    {
      int g = WallSegment(s, in, i, r.From, r.To);
      if (g < 0) return;
      float ex, ez;
      float d = WallDepth(g, r.Pos[0], r.Pos[2], ex, ez);
      if (d <= 0.f) return;
      float most = (Unseen(in, r.Pos[0], r.Pos[1], r.Pos[2]) ? WallPullUnseen : WallPullSeen) * in.Dt;
      float c = d < most ? d : most;
      float qx = r.Pos[0] + ex * c, qz = r.Pos[2] + ez * c;
      if (Blocked(s, in, i, qx, r.Pos[1], qz, s.WayX[i], s.WayZ[i])) { ++s.Counts.WallRefused; return; }
      r.Pos[0] = qx;
      r.Pos[2] = qz;
      ++s.Counts.WallPulled;
    }

    bool Blocked(Slot const& s, StepInput const& in, int i, float x, float y, float z, float ux, float uz) const
    {
      Record const& b = s.Before[i];
      int gx = (int)std::floor(x / Cell), gz = (int)std::floor(z / Cell);
      for (int dx = -1; dx <= 1; ++dx)
        for (int dz = -1; dz <= 1; ++dz)
        {
          unsigned bk = ((unsigned)(gx + dx) * 73856093u ^ (unsigned)(gz + dz) * 19349663u) % GridBuckets;
          for (int e = s.GridHead[bk]; e >= 0; e = s.GridNext[e])
          {
            if (e == i || (e < MaxMembers && !s.Visible[e])) continue;
            float ox, oy, oz, orad;
            At(s, e, ox, oy, oz, orad);
            if (std::fabs(oy - y) > Height) continue;
            float body = Body(in.Radius, orad);
            float now = Closeness(x, z, ux, uz, ox, oz, body);
            if (now >= 1.f) continue;
            float was = Closeness(b.Pos[0], b.Pos[2], ux, uz, ox, oz, body);
            if (was >= 1.f || now < was - 1e-5f) return true;
          }
        }
      return false;
    }

    // Walker i at (x, y, z) is closer than a body (+ extra) to another shown walker (each where it is now). other: an
    // own one found, -1 none or another placement's
    bool Crowded(Slot const& s, StepInput const& in, int i, float x, float y, float z, float extra, int* other = nullptr) const
    {
      int gx = (int)std::floor(x / Cell), gz = (int)std::floor(z / Cell);
      for (int dx = -1; dx <= 1; ++dx)
        for (int dz = -1; dz <= 1; ++dz)
        {
          unsigned bk = ((unsigned)(gx + dx) * 73856093u ^ (unsigned)(gz + dz) * 19349663u) % GridBuckets;
          for (int e = s.GridHead[bk]; e >= 0; e = s.GridNext[e])
          {
            if (e == i || (e < MaxMembers && (!s.Visible[e] || s.M[e].Ghost > 0.f))) continue;
            float ox, oy, oz, orad;
            At(s, e, ox, oy, oz, orad);
            if (std::fabs(oy - y) > Height) continue;
            float lim = Body(in.Radius, orad) + extra;
            float ex = x - ox, ez = z - oz;
            if (ex * ex + ez * ez < lim * lim)
            {
              if (other) *other = e < MaxMembers ? e : -1;
              return true;
            }
          }
        }
      return false;
    }

    // The other placements' shown walkers near this one's box (latest, not older than OldSeconds)
    void GatherForeign(Slot& s, StepInput const& in, Slot* others, int otherCount)
    {
      s.ForeignCount = 0;
      float minX = 1e30f, minY = 1e30f, minZ = 1e30f, maxX = -1e30f, maxY = -1e30f, maxZ = -1e30f;
      Record const* bs = s.Before;
      for (int i = 0; i < in.Count; ++i)
      {
        if (in.Flags[i] & DespawnedFlag) continue;
        Record const& b = bs[i];
        if (b.Pos[1] < -9000.f) continue;
        minX = (std::min)(minX, b.Pos[0]); maxX = (std::max)(maxX, b.Pos[0]);
        minY = (std::min)(minY, b.Pos[1]); maxY = (std::max)(maxY, b.Pos[1]);
        minZ = (std::min)(minZ, b.Pos[2]); maxZ = (std::max)(maxZ, b.Pos[2]);
      }
      if (minX > maxX) return;
      float pad = BodyMax + ClearExtra + 1.f;
      minX -= pad; minZ -= pad; maxX += pad; maxZ += pad; minY -= Height; maxY += Height;
      for (int k = 0; k < otherCount; ++k)
      {
        Slot const& o = others[k];
        if (&o == &s || !o.Key) continue;
        long seq = o.Seq;
        if (seq & 1) continue;
        if (in.Now - o.LastTime > OldSeconds) continue;
        if (o.MaxX < minX || o.MinX > maxX || o.MaxZ < minZ || o.MinZ > maxZ || o.MaxY < minY || o.MinY > maxY) continue;
        int n = o.PubCount;
        if (n < 0 || n > MaxMembers) continue;
        float r = o.Radius;
        int start = s.ForeignCount;
        for (int j = 0; j < n && s.ForeignCount < MaxForeign; ++j)
        {
          Published const& p = o.Pub[j];
          if (p.X < minX || p.X > maxX || p.Z < minZ || p.Z > maxZ || p.Y < minY || p.Y > maxY) continue;
          s.GridR[MaxMembers + s.ForeignCount] = r;
          s.Foreign[s.ForeignCount++] = p;
        }
        if (o.Seq != seq) s.ForeignCount = start;   // written meanwhile: left out this step
      }
    }

    void Publish(Slot& s, StepInput const& in, Record const* rec)
    {
      uint32_t idBase = (uint32_t)(s.Key >> 4) * 1024u;
      s.Seq += 1;
      int n = 0;
      float minX = 1e30f, minY = 1e30f, minZ = 1e30f, maxX = -1e30f, maxY = -1e30f, maxZ = -1e30f;
      for (int i = 0; i < in.Count; ++i)
      {
        if (!s.Visible[i]) continue;
        Published& p = s.Pub[n++];
        p.X = rec[i].Pos[0]; p.Y = rec[i].Pos[1]; p.Z = rec[i].Pos[2];
        p.DirX = s.WayX[i]; p.DirZ = s.WayZ[i];
        p.Id = idBase + (uint32_t)i;
        minX = (std::min)(minX, p.X); maxX = (std::max)(maxX, p.X);
        minY = (std::min)(minY, p.Y); maxY = (std::max)(maxY, p.Y);
        minZ = (std::min)(minZ, p.Z); maxZ = (std::max)(maxZ, p.Z);
      }
      s.PubCount = n;
      s.MinX = minX; s.MinY = minY; s.MinZ = minZ; s.MaxX = maxX; s.MaxY = maxY; s.MaxZ = maxZ;
      s.LastTime = in.Now;
      s.Seq += 1;
    }
  };

  // ---- the standing crowds (the Idle behaviour: no step of its own, its members never move) ----
  // Only the time of day's share: above it, members out of sight go unseen (y -10000, HiddenY: their height kept), a few
  // an update; below it, unseen ones out of sight come back (with no view known, wherever they are: the caller has no
  // share then). A member the rules didn't leave at -10000 is the game's again.
  struct IdleSlot
  {
    uintptr_t Key, Records;
    int Count;
    double LastTime;
    uint8_t Hidden[MaxMembers];
    float Y[MaxMembers];
    int Next;
    long long Hid, Shown;
  };

  class IdleRules
  {
  public:
    IdleSlot* FindSlot(StepInput const& in, IdleSlot* storage, int storageCount)
    {
      IdleSlot* empty = nullptr;
      IdleSlot* oldest = nullptr;
      for (int i = 0; i < storageCount; ++i)
      {
        IdleSlot& s = storage[i];
        if (s.Key == in.Key) return &s;
        if (s.Key == 0 && !empty) empty = &s;
        if (s.Key != 0 && in.Now - s.LastTime > 10.0 && (!oldest || s.LastTime < oldest->LastTime)) oldest = &s;
      }
      IdleSlot* s = empty ? empty : oldest;
      if (!s) return nullptr;
      Reset(*s, in);
      return s;
    }

    static IdleSlot* FindExisting(uintptr_t key, IdleSlot* storage, int storageCount)
    {
      for (int i = 0; i < storageCount; ++i)
        if (storage[i].Key == key) return &storage[i];
      return nullptr;
    }

    void Apply(IdleSlot& s, StepInput const& in)
    {
      if ((uintptr_t)in.Records != s.Records || in.Count != s.Count) Reset(s, in);
      s.LastTime = in.Now;
      Record* rec = (Record*)in.Records;
      int shown = 0;
      for (int i = 0; i < in.Count; ++i)
      {
        bool gone = (in.Flags[i] & DespawnedFlag) != 0;
        if (s.Hidden[i] && (gone || !(rec[i].Pos[1] < -9000.f) || rec[i].Pos[1] < -10040.f)) s.Hidden[i] = 0;
        if (!s.Hidden[i] && !gone && rec[i].Pos[1] > -9000.f) ++shown;
      }
      int wanted = Wanted(in.Density, in.Count);
      if (in.Count <= 0) return;
      int done = 0;
      for (int k = 0; k < in.Count && done < DensityPerStep && shown != wanted; ++k)
      {
        int i = (s.Next + k) % in.Count;
        if (shown < wanted && s.Hidden[i])
        {
          if (in.ViewKnown && !Unseen(in, rec[i].Pos[0], s.Y[i], rec[i].Pos[2])) continue;
          rec[i].Pos[1] = s.Y[i];
          s.Hidden[i] = 0;
          ++shown; ++done; ++s.Shown;
        }
        else if (shown > wanted && in.ViewKnown && !s.Hidden[i] && !(in.Flags[i] & DespawnedFlag) && rec[i].Pos[1] > -9000.f)
        {
          if (!Unseen(in, rec[i].Pos[0], rec[i].Pos[1], rec[i].Pos[2])) continue;
          s.Y[i] = rec[i].Pos[1];
          rec[i].Pos[1] = HiddenY;
          s.Hidden[i] = 1;
          --shown; ++done; ++s.Hid;
        }
      }
      s.Next = (s.Next + 5) % in.Count;
    }

    // Every member it keeps unseen back at its height (the rules stop for this placement). Returns how many.
    int Release(IdleSlot& s, StepInput const& in)
    {
      if ((uintptr_t)in.Records != s.Records || in.Count != s.Count) return 0;
      Record* rec = (Record*)in.Records;
      int shown = 0;
      for (int i = 0; i < in.Count; ++i)
        if (s.Hidden[i])
        {
          if (rec[i].Pos[1] < -9000.f && rec[i].Pos[1] > -10040.f) { rec[i].Pos[1] = s.Y[i]; ++shown; }
          s.Hidden[i] = 0;
        }
      return shown;
    }

  private:
    void Reset(IdleSlot& s, StepInput const& in)
    {
      s.Key = in.Key;
      s.Records = (uintptr_t)in.Records;
      s.Count = in.Count;
      s.LastTime = in.Now;
      s.Next = 0;
      for (int i = 0; i < MaxMembers; ++i) { s.Hidden[i] = 0; s.Y[i] = 0.f; }
    }

    static bool Unseen(StepInput const& in, float x, float y, float z)
    {
      if (!in.ViewKnown) return false;
      return OutOfSight(x, y, z, in.ViewX, in.ViewY, in.ViewZ, in.ViewAX, in.ViewAY, in.ViewAZ, FarAway, SideNear);
    }
  };
}
