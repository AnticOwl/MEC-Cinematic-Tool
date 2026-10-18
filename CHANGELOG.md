# Changelog

## Version 1.1

- **Fixed: the game crashing with a high stream distance.** The game only has room for 128 loaded
  zones per zone streamer and 4096 meshes, and loading the city around the camera ran out of both.
  The tools now add the camera's extra zones one at a time and only while there's room: no more
  from 96 zone slots or 3300 meshes, and they take zones away again from 104 slots or 3500 meshes
  (all of the camera's extra zones from 3700).
- Stream distance goes up to 400 m (was 600 m), with up to 20 rings of foci (was 40).
- The Camera tab shows *Meshes loaded: N of 4096* and *Zones loaded: N of 128*, in yellow while
  the camera's extra zones are held back.
- Crash notes in `CT.log` include the zone slots and meshes in use.
- The keyframe up / down arrows are smaller and centred, and every row has both (greyed out where
  the keyframe can't move any further), so they line up from row to row.

## Version 1

- First release.
