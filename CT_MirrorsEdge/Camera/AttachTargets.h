#pragma once
#include <DirectXMath.h>

// What the free camera can follow besides Faith: entities (characters,
// vehicles, drones, props, static models that move) and the members of swarms
// (the city's traffic and crowds). Found in the game's own per-class instance
// lists and read from memory without calling anything on them.
//
// Everything here runs on the game's thread (the camera hook).
namespace attach
{
  const int MaxCandidates = 48;
  const int MaxClasses = 64;

  // For the menu's filters
  enum class Group { People, Traffic, Other };

  struct Target
  {
    __int64 Entity;       // the entity, or a swarm member's SwarmEntity
    __int64 VTable;       // its vtable when it was picked: another one means it's gone
    __int64 Boids;        // swarm member: the swarm's CrowdBoidsComponent
    __int64 BoidsVTable;
    int Agent;            // swarm member: its record in the boids list, -1 for entities
    int Class;            // see ClassName / KindOf
    unsigned char SwarmType; // swarm member: 0 not known (nothing moved), 1 traffic, 2 crowd
    float Distance;       // from the camera when found
    float Angle;          // from the middle of the view when found, radians
    float Score;          // lower is a better pick
    DirectX::XMFLOAT3 Position; // where it was when found
    // Swarm member: the way it's going (smoothed), and the record's position
    // when it last changed, to move smoothly between the swarm's updates
    DirectX::XMFLOAT3 Heading;
    DirectX::XMFLOAT3 Seen;
    double SeenTime;
    float Interval;
  };

  struct View
  {
    DirectX::XMFLOAT3 Camera;
    DirectX::XMFLOAT3 Look;         // unit vector
    __int64 Skip;                   // Faith's soldier
    DirectX::XMFLOAT3 SkipPosition; // and where she is, in case her entity isn't that pointer
    bool HasSkipPosition;
  };

  struct Scan
  {
    // Best first. Static models are kept apart: they only count if they move.
    Target Best[MaxCandidates];
    int BestCount;
    Target Static[MaxCandidates];
    int StaticCount;
    // What's in the level, for the log
    int Found[MaxClasses];
    int Swarms;
    int NoTransform;
    int Errors;
    int Dropped;  // ScanNearby: within the radius but no room left in out
  };

  // Looks the classes up the first time (they're static data in the exe).
  // From Faith's class, or the build the tools were made for. False if not found.
  bool FindClasses(__int64 pFaithSoldier, bool& fromFaith);
  int ClassCount();
  const char* ClassName(int index);
  const char* Kind(int index);
  bool IsStatic(int index);
  bool IsSwarm(int index);

  // What it is ("character", "traffic", "crowd", ...), which filter it's
  // under, and how high above its origin its middle is
  const char* KindOf(Target const& target);
  Group GroupOf(Target const& target);
  float AimHeight(Target const& target);

  // Everything in front of the camera that could be followed
  void ScanView(View const& view, Scan& scan);

  // Everything within the radius of the camera, in no order. Gives how many
  // were written to out (at most max). Fills scan's counts, not its lists.
  int ScanNearby(View const& view, float radius, Target* out, int max, Scan& scan);

  // The target's transform now, if it still exists. A swarm member's is made
  // from where it is and the way it's moving.
  bool ReadTransform(Target& target, DirectX::XMFLOAT4X4& transform);

  // Moved further than this in one frame: not the same object any more
  float JumpLimit(Target const& target);

  // Same object (a swarm member is its swarm and record)
  inline bool SameTarget(Target const& a, Target const& b) { return a.Entity == b.Entity && a.Agent == b.Agent; }
}
