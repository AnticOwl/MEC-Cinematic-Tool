# Changelog

## Unreleased

- The status bar under the tabs shows the camera's position (x, y, z) while it's on.

## Version 1.2.1

- **Fixed: crashes with the stream distance up** (also after alt-tabbing). The renderer files the
  instances it draws into a few draw batches, and one batch has room for 111872 at once; past that
  it writes over its own memory and crashes. Loading far around the camera (106000 model
  instances) got there. The tools now watch the fullest batch (*Renderer draw batch* in the menu),
  stop adding zones around the camera at 70 %, take some away from 80 % and all of them from 88 %.
- *Unrestricted* (stream distance, off by default) ignores those two renderer limits, as before
  they were watched: more loads far away, but the game can crash. A red warning in the menu and
  the status bar shows while it's on, and the log notes it.
- The renderer's memory for those instances, which it sets aside once when the game starts (16 MB)
  and doesn't give back during play, is made 64 MB as the game starts. The level alone takes
  5.5 MB, and far streaming took the rest in seconds. The tools also watch it (*Renderer instance
  memory*).
- **Fixed: the free camera's mouse look stopping.** It read the mouse through DirectInput in
  background mode, which runs on a low-level hook that Windows silently removes when the game
  is busy. It now reads the game's own raw input, so the mouse wheel's field of view zoom works
  too. Mouse movement from while the game stalls (loading screens) is dropped rather than turning
  the camera all at once afterwards.
- **Fixed: loading around the camera doing nothing**, worst after a teleport (fast travel to a
  safe house). The tools went by the end of the game's model instance list, but the instances of
  zones that unloaded leave free slots the game uses again (33180 of 60123 after a teleport in a
  test). They now count the instances in use, as they do with meshes.
- *Only where the camera looks*: each point in view keeps its own focus while the camera turns,
  so turning only loads and unloads zones at the edges of the view.
- Near the mesh limit the tools only wait while meshes are still arriving, not for every small
  change in the count.
- The tools always ask for more room for model instances from the next level load, also with
  *Load the world around the camera* off.
- The menu also shows the model instances in use, and the menu and log say which limit holds the
  camera's zones back.
- The menu and README note that some safe houses may or may not load with *Load the world around
  the camera*.
- **Fixed: clicking a row in the FOLLOW tab's *Nearby* list seeming to do nothing.** It did start
  following, but with *Put the camera next to what I pick* off the camera stayed where it was, often
  far away or facing elsewhere. A click now always puts the camera next to what was clicked; the
  checkbox, now *Put the camera next to what I aim at*, is only for *Attach to what the camera
  looks at*.
- *Pause* / *Resume* next to *Play* / *Stop*: a paused track holds the camera on that frame, and
  carries on from there. The status bar says when it's paused.

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
