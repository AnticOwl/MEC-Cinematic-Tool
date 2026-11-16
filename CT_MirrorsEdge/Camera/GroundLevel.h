#pragma once
#include <cstddef>

// Walking the streets (GAME > Street level). Research_Ground_Level_2026-11-01.md has the decompile.
//
// The streets have no collision: their road meshes (objects/architecture/roads/streamingground) have no
// collision asset. Under them each district has a kill floor (PamKillVolumeEntityData linked to a
// VolumeVectorShapeData at y ~ 0, 4 - 61 m tall), and Ant's fall check kills her after FallThreshold m of
// falling (10 by default). So:
//
// - Solid street: invisible ground round Faith, at the height of the road meshes' surface (their triangles, with
//   the water and the street bridges' and elevated highways' decks, read from the game's data offline, are in the
//   DLL: Resources\GroundRoads.bin): box tiles on a 2 m grid within 8 m of her (1 m and 0.5 m where one plane
//   doesn't fit the road), each fixed where it's made; a new one near her only with its top at or under her
//   feet. And every model the city's free-roam levels draw with no collision, at any height (street furniture,
//   lamps, parked cars and boats, plazas, walkways, roofs, backdrop buildings, the train station under the lair:
//   Resources\GroundProps.bin, boxes per placement) within 40 m of her. They're
//   static Havok bodies the tools make in the client world (hknpWorld::createBody, boxes from
//   hknpConvexShape::createFromHalfExtents) and destroy when they're not wanted, all from the physics world's own
//   flush of its deferred work (0x1434F4C80), where the game makes and destroys its bodies too. A hit on a body
//   is turned into the game's ids by its user data (+0x88, the low 16 bits index the physics world's records,
//   0x1435177B0), and the record's material is looked up with no check. So the bodies take the user data,
//   collision filter and material of a real static body near Faith (layer 1, its record pointing back at it),
//   found by sweeps around her, and are given another's right after any flush that freed its record. While this
//   is on, the street kill floors don't kill (their inside test, 0x143970D30, says no for the 19 of them), and the
//   levels' other kill zones (29) don't kill Faith while the street is under her.
// - No fall death: the fall check's FallThreshold is raised while the run that reads it runs (0x142CE8200), so
//   HasFallenPastThreshold stays false. Every character that uses the same asset is affected.
namespace ground
{
  // Checks the game's code, loads the road triangles and hooks. Once, after util::hooks::Init.
  void Init();
  bool IsAvailable();

  void SetSolidStreet(bool on);
  void SetNoFallDeath(bool on);

  // Each frame on the game's thread (the camera hook): Faith's feet, or none
  void Update(bool hasFaith, float const* feet);
  // The Sky City station's collision the tools reshape while Solid street is on (its base block's west face moved to
  // open the station's lowest floor, the invisible wall at its walkway's doorway shrunk away, the hall's low ceiling
  // over its east end lifted), back once it's off and Faith is out of what it opened. Each frame on the game's thread
  // before Update: Faith's feet whether it's on or not, or none. It only decides: the physics flush writes.
  void UpdateSkyCity(bool hasFaith, float const* feet);
  // Something the reshape changed is changed (with Solid street off: Faith is in what it opened): Update has to be
  // given her feet, and keeps the ground and props on till it's back
  bool SkyCityHolds();
  // The gates Solid street takes away while it's on (a fence in downtown area 5's passage to the street), back once
  // it's off and Faith is clear of them. Each frame on the game's thread: Faith's feet whether it's on or not, or none
  void UpdateGates(bool hasFaith, float const* feet);

  // One line for the menu: what the floor does now
  const char* GetStatus(char* buffer, size_t size);

  // For other parts' checks (road traffic's ground check, diagnostics), any thread: the road data's surface at (x, z),
  // the highest no higher than top (false: none there); and Faith's feet as the camera last gave them (UpdateGates:
  // every frame, Solid street on or off; false when none came for over a second)
  bool RoadHeightUnder(float x, float z, float top, float& height);
  bool LastFeet(float* feet);
}
