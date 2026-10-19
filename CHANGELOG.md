# Changelog

## Version 1.2

- **Fixed: Faith's hair missing with *Show Faith's body*** (the original tools lost it too). Her
  hair is cloth that the game only switches on near the camera, and with her body switched it
  stayed off. While her body is shown the tools now keep it on, also past the 100 m where its
  mesh would drop to a detail level without hair.
- **Fixed: *Follow Faith* drifting away from her.** With *Freeze Faith* off, the movement keys
  moved the camera as well as Faith. They now only move her, and the camera keeps its place next
  to her; the arrow keys, the FOV keys and the gamepad still adjust it.
- *Only where the camera looks* (stream distance): the extra zones only in front of the camera,
  about a third of the zones and meshes, so the stream distance can go up to 1000 m.
- **Fixed: loading around the camera stopping for the rest of a level.** The tools went by the
  mesh table's end, which only ever grows: the records of meshes that unloaded are used again.
  They now count the records in use (2341 in a test where the end was at 3657).
- Stream limits a step higher: zones are added below 100 slots (was 96) and meshes up to 3450
  (was 3300). From 2800 meshes on, a new zone is only added once the mesh count has stopped
  growing, so two bursts of meshes can't arrive at once.
- The free camera's default key is F10 (was Insert, which also opens MECMP's menu). Keys you
  bound yourself stay as they are.
- *Time of day* has *- 10 min* and *+ 10 min* buttons.
- When MECMP (the multiplayer mod) opens, the time of day controls get disabled, because MECMP
  syncs the time of day. The tools then leave the game's clock alone (a stopped clock, the stuck
  clock fix and a track's time of day) until MECMP is gone.
- *Stop the clock* holds the time on every rendered frame and before and after the game's own
  update, not only before it.

## Version 1.1

- **Fixed: the game crashing with a high stream distance.** The game only has room for 128 loaded
  zones per zone streamer and 4096 meshes, and loading the city around the camera ran out of both.
  The tools now add the camera's extra zones one at a time and only while there's room: no more
  from 96 zone slots or 3300 meshes, and they take zones away again from 104 slots or 3500 meshes
  (all of the camera's extra zones from 3700).
- The Camera tab shows *Meshes loaded: N of 4096* and *Zones loaded: N of 128*, in yellow while
  the camera's extra zones are held back.
- Crash notes in `CT.log` include the zone slots and meshes in use.
- The keyframe up / down arrows are smaller and centred, and every row has both (greyed out where
  the keyframe can't move any further), so they line up from row to row.

## Version 1

- First release.
