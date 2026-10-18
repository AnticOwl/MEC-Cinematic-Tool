# Mirror's Edge Catalyst Cinematic Tools, with keyframed camera tracks

A rebuild of the Mirror's Edge Catalyst cinematic tools on top of Hattiwatti's
[CinematicTools template](https://github.com/Hattiwatti/CinematicTools) (template commit in
`CT_MirrorsEdge/TEMPLATE_COMMIT.txt`), using the hooks and offsets reversed from the original 2018
`CT_MirrorsEdge.dll`. It has the original tools' features in an in-game menu, and adds a lot on top:

- free camera with mouse look, gamepad and adjustable speeds (up to 200 m/s)
- **keyframed camera tracks**: position, rotation, field of view, motion blur and **time of day**,
  with Smooth / Linear / Ease in-out / Hold timing, editable and reorderable keyframes, a live
  preview while editing and a fixed playback rate for frame dumping
- play as Faith while a track plays, and **follow Faith** or other moving things (NPCs, traffic,
  drones, props) with the camera
- **load the city around the camera** (zone streaming), ahead of it, along a track and out to a
  set distance, so a far-away camera isn't surrounded by missing props
- time freeze, HUD toggle, time of day with a stuck-clock fix, Fix LOD, small-object culling,
  draw distance, sun shadow distance and layers, motion blur
- visual environment overrides (color correction, depth of field, film grain, outdoor light,
  tonemap, vignette) and a light manager
- a `usp10.dll` loader, so no injector is needed

> **Status:** for the Steam / EA app build (PE timestamp `0x5762E3B7`), built with VS 2022.
> Tested in-game: the loader, free camera, keyframes and playback, time of day, streaming around
> the camera, the menu (scrolling, cursor) and lights. See *Known risks* at the end for what's
> only been worked out from the game's code. `Cinematic Tools/CT.log` in the game folder shows
> what was found and hooked.
>
> Not affiliated with or endorsed by EA or DICE. Mirror's Edge is a trademark of Electronic Arts.

## Layout

| Path | What |
|---|---|
| `dist/` | What goes into the game folder: `CT_MirrorsEdge.dll` and the `usp10.dll` loader |
| `CT_MirrorsEdge/` | The tools (Visual Studio project `Cinematic Tools.vcxproj`) |
| `Loader/` | `usp10.dll` proxy that loads the tools when the game starts |
| `minhook/` | [MinHook](https://github.com/TsudaKageyu/minhook) v1.3.3, the only external dependency |
| `build.ps1` | Builds MinHook (first run only), the tools and the loader, and fills `dist/` |
| `tools/MinHookNoLTCG.props` | Build workaround, see the comment inside |
| `ctdis.py` | The script used to disassemble the original DLL |
| `CHANGELOG.md` | What changed in each version |
| `LICENSE` | MIT, for the tools themselves |
| `THIRD_PARTY_NOTICES.md` | Licenses of MinHook, Dear ImGui and inih |

## Building

Requirements: Visual Studio 2022 or the Build Tools with the C++ workload and a Windows 10/11 SDK.

```powershell
powershell -ExecutionPolicy Bypass -File build.ps1
```

Output: `dist/CT_MirrorsEdge.dll` and `dist/usp10.dll`. The CRT is linked statically, so no
VC++ redistributable is needed. The C4244 warnings from `Input/InputSystem.cpp` come from
the template and are harmless.

## Installing and running

1. Copy `dist/CT_MirrorsEdge.dll` and `dist/usp10.dll` (or the two files from a release) next to
   `MirrorsEdgeCatalyst.exe`, e.g. `...\steamapps\common\Mirrors Edge Catalyst`.
2. Start the game normally. `usp10.dll` is loaded by the game itself and loads the tools, so no
   injector or `MEC Cinematic Tools.exe` window is needed any more. Press F5 in-game.
   It works next to Catalyst Clarity (which uses `dinput8.dll`). If another mod also ships a
   `usp10.dll`, only one of them can be used this way. Windows 10 or 11 is needed (the proxy
   forwards everything to GDI32, like the system `usp10.dll` does there).

   Why `usp10.dll`: the game's imports are only resolved after EA's DRM decrypts the exe, and by
   then Steam's and the EA overlay's DLLs have already loaded the system `version.dll` and
   `winmm.dll`, so proxies with those names are never used. `usp10.dll` is first loaded by the
   game itself.
3. To uninstall, delete the two files, plus `version.dll` if an older build put one there (and the
   `Cinematic Tools` folder for its log and config).

Any injector still works too. The tools refuse to start twice in one game, so an injector plus the
loader doesn't double-hook.

The log goes to `Cinematic Tools\CT.log` in the game folder and can be opened while the game
runs. There's no console window unless you tick *Show the console window* in the MISC tab (or set
`ShowConsole = true` under `[General]` in `Cinematic Tools\config.ini`). The Steam / EA app exe
is wrapped by EA's DRM, which only decrypts the game code during start-up, so the log first says
`Waiting for the game code to be decrypted...` and continues once it's readable.

The previous session's log is kept as `CT.previous.log`, so it's still there after a crash and a
restart. If the game crashes, the log ends with two `[CRASH]` lines: where it crashed and what the
tools were doing at that moment (camera, track, follow, streaming, Faith's body switch, LOD,
shadows). Windows also saves a crash dump in `%LOCALAPPDATA%\CrashDumps`.

## Controls (defaults, rebindable in MISC → Change hotkeys...)

| Key | Action |
|---|---|
| F5 | Toggle the in-game UI (or the ✕ on its window) |
| Insert | Toggle the free camera |
| W A S D / Space / Left Ctrl | Move |
| Mouse, arrow keys | Pitch / yaw (with the menu open: hold the right mouse button outside its window) |
| Numpad 1 / 3 | Roll |
| Page Up / Page Down, mouse wheel | Field of view (the wheel scrolls the menu when it's over it) |
| **Numpad +** | **Add keyframe at the current camera** |
| **Numpad −** | **Delete the last keyframe** |
| **Numpad \*** | **Play / stop the track** |
| Delete | Freeze time |
| Home | Hide HUD |
| End | Block / unblock game input while the camera is on |
| Numpad 5 | Attach the camera to what it looks at (again on the same object: let go). Needs Num Lock on, like Numpad 1 / 3 |

Gamepad movement works as in the template (left stick, right stick, triggers, shoulders).

## UI tabs

The menu uses the game logo's red and white, with Segoe UI text that scales with the game's
resolution (*Text size* in MISC makes it bigger or smaller). The window can be moved and resized,
and remembers where it was (`Cinematic Tools\imgui.ini`). The line under the tabs always shows
whether the camera is on, the track's state, whether time is frozen and who has the controls.
Grey `(?)` marks explain an option when hovered. Clear all, Delete track, Forget all lights and
Reset all hotkeys need a second click.

- **CAMERA**, in collapsible sections:
  - *Turn camera on / off* and *Move camera to Faith* (turns the camera on at Faith's head).
  - *Camera tracks*: the tracks (new, rename, delete), play / stop with a progress bar, playback
    speed, add / delete keyframes, and the keyframe list (the one playing is highlighted).
    *Keyframe options*: what the keyframes control (rotation, field of view, motion blur), play
    manually, time between new keyframes and how new keyframes move. *Playback options*: *Faith
    keeps control while a track plays* (on by default: during playback the game gets your keyboard,
    mouse and gamepad, so you can play as Faith while the camera follows the track) and fixed
    playback FPS.
  - *Time and HUD*: freeze time, show HUD, and the time of day slider (midnight - noon -
    midnight) with *Stop the clock* and the *Stuck clock fix* (when the clock gets stuck at 65700
    it's moved 15 s ahead, both editable).
  - *Detail and draw distance*: *Fix LOD*, *Detail distance* (the game's LOD scale, default 5),
    *Show small objects far away* and *Load the world around the camera* (see below) and *Draw
    distance* (the camera's far plane, game default 25000 m).
  - *Sun shadows*: *Shadow distance* (how far from the camera the sun casts shadows, game default
    200 m: raise it when the camera is high up and the ground loses its shadows; the further it
    goes, the softer and blockier close shadows get) and *Adjust the shadow layers*. The sun's
    shadow is drawn in 4 layers, sharp near the camera and softer further out, and where one layer
    ends the next fades in: that's the shadow seen fading in and out as the camera moves. *Fade
    between layers* sets how wide that fade is (game 0.03, higher = smoother), *Layer spread* how the
    layers are spread over the distance (game 0.88, lower = spread more evenly, so the fades move
    further away). The sun's own shadow settings (softness, a separate shadow direction, height
    scale) are in VISUALS > Outdoor light.
  - *Motion blur*: *Control motion blur* (on/off, strength, max blur length). The strength is also
    stored in each keyframe.
  - *Camera settings*: movement, turn, roll and zoom speeds (in m/s and degrees per second), mouse
    sensitivity, field of view, *Start at the game camera*, *Freeze Faith*, *Gamepad moves the
    camera*, *Show Faith's body*.
  - *Keys*: every hotkey as it's currently bound.
- **FOLLOW**: *Follow Faith* with *Turn with her*; following traffic, crowds, characters, drones
  and other moving things, picked from the *Nearby* list (with markers in the game view) or with
  *Attach to what the camera looks at*; *Go next to it*, *Let go*, *Turn with it*, *Tilt with it
  too* (see below).
- **VISUALS**: color correction, depth of field (with the DoF method and the original tools'
  Sprite DoF fix), film grain, outdoor light, tonemap and vignette. Open an effect and tick *Change
  this effect*: its current values are copied from the game, and what you set is used every frame
  until you untick it. *Reset to game values* copies the game's values again. The less used fields
  are under *More settings*. A changed effect shows *Used every frame*, or a warning naming the
  field if something else in the game keeps changing it.
- **LIGHTS**: new sphere and spot lights at the camera; color, intensity, radius, brightness,
  angles and what they light up, with the rest under *More settings*. *Forget all lights* after
  loading another level or checkpoint. A light the game can no longer update is taken off the list
  instead of crashing the game.
- **MISC**: text size, *Mouse cursor*, *Change hotkeys...* (click a key, press the new one; *Reset
  all to defaults*), console window on/off. While the menu is open the Windows cursor is shown:
  the game hides it (`ShowCursor` / `SetCursor(NULL)`), so the tools show it as many times as
  needed from the game window's thread, give it the arrow through a `SetCursor` hook, and hide it
  the same number of times when the menu closes. *Mouse cursor: Drawn by the menu* goes back to
  ImGui's own software cursor.

### Far-away props

*Fix LOD* keeps the most detailed models at any distance, and *Show small objects far away* lowers
the game's screen-area culling (`WorldRenderSettings.CullScreenAreaScale`, 5 in Catalyst, where
small objects on screen get hidden; try 0.5 - 1). Neither can load things the game hasn't streamed
in. Catalyst streams the city in zones: each `ZoneStreamerEntity` loads the zone its foci are in,
plus the neighbouring zones. Its own focus is the game camera, which stays with Faith even while
the free camera is somewhere else, so props near a far-away free camera are missing.

*Load the world around the camera* gives the zone streamer a second focus at the free camera while
the camera is on. The zones around Faith stay loaded, so she doesn't lose the ground under her
feet. After a long move the new zones take a moment to stream in, and having two areas loaded uses
more memory. Unticking it, or turning the camera off, removes the focus, and the game unloads the
camera's zones the way it does when its own foci move on. The log (`Cinematic Tools/CT.log`) says
when each streamer gets or loses the camera focus. If it doesn't help somewhere, moving Faith there
still works (*Faith keeps control* lets you run with a track playing).

Props popping in while flying are zones that only start loading once the camera gets near them.
*Stream distance* (0 - 400 m, off by default) loads the city that far around the camera too: the
tools add rings of foci about 200 m apart around it (up to 20), and each loads its zone and the
zones next to it. More distance means less pop-in around the camera, but more memory and loading.

Each zone streamer only has room for 128 loaded zones, and the game breaks when it runs out (it
writes past the end of its table, which crashed the game with a high stream distance). The tools
count the streamer's used slots before every update. Zones only get a slot once they load, so the
count runs behind what was asked for: the tools add their extra foci one at a time, only once the
count has stood still for 0.75 s and only while fewer than 96 are in use, and above 104 they take
the two least important away (the camera's own focus always stays). With *Fix LOD* on, every far
model is loaded in full detail as well, which brings in more meshes (see below).

The tighter limit is meshes: the game has room for 4096 different meshes in its mesh streaming
and crashes when the loaded zones need more (it moves its mesh table while background jobs are
still reading it). Extra zones bring in a lot of meshes, and whatever is loaded around Faith
counts too. The tools watch the count (*Meshes loaded: N of 4096* in the menu): they stop adding
zones at 3300, take two away at 3500, and at 3700 take the camera's own zones away as well.
How far around the camera can be loaded therefore depends on the area. (They also ask for more
room for model instances from the next level load, which the game has plenty of already.) The
menu shows *Zones loaded: N of 128*, in yellow when zones are being held back. The rings move in
100 m steps rather than with every bit of camera movement.

Two sub-options (both on by default) load more in advance:

- *Load ahead of the camera*: a third focus about 2.5 seconds ahead of the camera at its current
  speed (at most 400 m), so the zones it's flying into are loading before it gets there.
- *Keep the track's path loaded*: foci at every keyframe of the selected track and every 150 m
  between them (up to 30), so the whole route is loaded while the camera is on and playback
  doesn't have to load anything. After adding keyframes far away, wait a few seconds before
  playing (or play it once).

Some pop-in can still be left: models switching detail (use *Fix LOD*), small objects (*Show small
objects far away*), and meshes and textures that the game streams separately inside a zone.

### Following Faith and other objects

Everything about following is in the **FOLLOW** tab (the free camera has to be on).

*Follow Faith* attaches the camera to her: her movement since the last frame is added to the
camera, so it travels with her and can still be flown around her. *Turn with her* adds her
heading too (only the heading, so the camera stays level when she wall-runs or climbs). A track
played while it's on moves with her from where she was when it started: freeze time, record a
move around her, give her the controls (*Faith keeps control while a track plays*, CAMERA tab)
and play it while she runs.

**Nearby** lists everything around the camera that can be followed, nearest first: distance, what
it is, its class, and whether it's moving and in view. So that rows don't move while you pick,
the list is only made again every 30 seconds (never while the mouse is over it), on *Refresh now*,
or when you let go of *Within*; distances, moving / still and the markers keep up all the time,
and something that disappears stays as a grey "gone" row until the next list. *Within* sets how
far it looks (10 - 1000 m); *People* (KrugerSec and other characters, crowds), *Traffic* (cars,
flying traffic) and *Other* (drones, vehicles, props, lifts and platforms that moved in the last
30 seconds) filter it, and *Only moving* hides what stands still. With *On
screen* every listed object gets a dot in the game view (brighter when it moves), the one under
the mouse gets a white ring with its name, and the one being followed a red ring. **Click a row to
follow it.**

*Put the camera next to what I pick* (on by default) puts the camera behind the object and a
little above, looking at it, so *Turn with it* works like a chase camera. Without it the camera
stays where it is and moves along with the object from there; with *Turn with it* on that swings
the camera around the object at that distance, a long way for something far off. *Go next to it*
does the same placement later. *Let go* stops following; *Tilt with it too* also uses its pitch
and roll (a drone banking, flying traffic climbing).

*Smoothing* (0.4 by default) makes the camera follow a copy of the object that catches up over a
moment (up to half a second at 1) instead of every small step: the swarm moves its members in
steps, and characters wobble as they walk. Higher is steadier but trails a little further on
corners and when the object speeds up; 0 follows exactly. It doesn't change *Follow Faith*. With
a fixed playback FPS it uses the fixed frame time, so recordings come out the same.

*Attach to what the camera looks at* (or Numpad 5 with the menu closed) picks whatever moves
nearest the middle of the view instead, within about 12 degrees and 1000 m; *Crosshair* marks the
middle of the screen. Pressing Numpad 5 again on the same object lets go. A pick watches what's
in view for a quarter of a second before it decides (that's how it tells what moves), and
clicking a row, *Let go* or *Follow Faith* in that time cancels it.

The FOLLOW tab's settings (*Turn with it*, *Tilt with it too*, *Put the camera next to what I
pick*, *Smoothing*, *Crosshair*, the list's range, filters and *On screen*) are saved in
`Cinematic Tools\config.ini` under `[Follow]`.

What can be followed:

- **traffic and crowds**: Catalyst's cars, flying traffic and crowds aren't entities but members
  of swarms (`SwarmEntity`, a few hundred members each), listed as `SwarmEntity #123`. A swarm is
  traffic when its moving members cover more than 12 cm per update, a crowd otherwise. The swarm
  updates its members less often than the game draws frames, so the camera glides between their
  positions instead of moving in steps.
- **characters**: KrugerSec and other NPCs (`PamClientCharacterEntity`).
- **drones, vehicles, moving models, physics props**, where a level has them.
- **lifts, platforms and other static models that move**: a static model only shows up (and can
  only be picked) once it has moved, so buildings never do. With time frozen nothing moves.

The objects come from the game's own instance lists and swarm data, read from memory without
calling anything on them. Every frame the tools check that the object is still there (same
vtable, still in its list, still the same swarm member, no jump of more than 100 m, or 15 m for
a swarm member); when it's destroyed, unloaded or despawns, the camera stops following it, stays
where it is, and the menu and the log say why. Terrain, water, bullets and held weapons aren't
listed. Each *Attach* pick writes to the log how many objects of each class there are.

### Making a camera move

1. Press Insert, fly to the first shot and press Numpad +. Move the camera to the next shot and
   repeat. A keyframe that's the same view as the one before it is marked *(same view as the one
   before)*, and the camera holds still between the two.
2. Open the UI (F5). Each keyframe in *Camera tracks* can be expanded to edit its position,
   pitch / yaw / roll, FOV, the time to the next keyframe and how it gets there:
   - **Smooth**: a spline through the keyframes (Catmull-Rom with time-aware, clamped tangents,
     so two keyframes at the same spot hold still instead of swinging out and back)
   - **Linear**: straight line at constant speed
   - **Ease in/out (bezier)**: the smooth path, timed with a cubic bezier ease (0.42, 0, 0.58, 1)
     so the camera slows into and out of the keyframe
   - **Hold**: stays on the keyframe, then cuts to the next one at its time

   *Go to* moves the camera to a keyframe, *Set from camera* overwrites it with the current camera.
   While you drag or type a keyframe's position, angles or field of view, the camera shows that
   keyframe live (and its time of day, when the track sets the time of day). The up / down arrows
   at the end of a keyframe's row move it earlier or later (greyed out where it can't go any
   further): the two keyframes swap everything except their times, so the shots change order and
   the track's timing stays the same.
   Each keyframe also stores a motion blur strength. With *Control motion blur* on and *Motion
   blur* ticked in *Keyframe options*, it's interpolated along the track like the FOV. Frostbite
   works the blur out from how the camera moves between frames, so fast moves blur more by
   themselves.

   **Day and night:** each keyframe also stores the time of day when it's added. Tick *Time of
   day* in *Keyframe options* and the track sets the game's clock between the keyframes, with the
   same Smooth / Linear / Ease / Hold timing as the camera. The clock runs forward to the next
   keyframe, through midnight if it has to, so 06:00 → 18:00 → 06:00 is a full day and night;
   *Clock runs backwards to the next* on a keyframe turns it back instead. Freeze the clock (*Stop
   the clock* under *Time and HUD*) while adding keyframes and set each time with the slider, or
   edit a keyframe's *Time of day* afterwards. Changing a keyframe's time turns the time of day
   channel on by itself (*Track sets the time of day*, also under each keyframe), and the game
   shows that time live while you drag. When the track ends the clock carries on from there (or
   stays there if it's stopped).
3. Press Numpad \* to play. Playback stops on the last keyframe.
4. For frame-dumped recordings, set *Fixed playback FPS* to the capture frame rate. Every rendered
   frame then advances the track by exactly 1/FPS, however fast the game actually renders.

Tracks and lights live in memory only. They aren't saved when the game closes.

## What changed compared to the template

- MEC offsets and signatures (`Util/Offsets.cpp`). The scan waits for EA's DRM to decrypt the
  code first. The original tools' hardcoded addresses are used as a fallback only when the game's
  PE timestamp is the build they came from.
- Hooks (`Util/Hooks.cpp`): `IDXGISwapChain::Present` (address found with a throwaway D3D11
  device), MEC's camera update (free camera, draw distance, light updates), entry input (blocks
  player input), visual environment update (overrides), the zone streamer's update (adds the
  camera's streaming focus), and `SetCursorPos`/`ClipCursor` (free cursor while the UI is open).
- Hook memory: the game's heap takes all the memory within 1 GB of its exe, which is all
  MinHook v1.3.3 searches for trampolines (`MH_ERROR_MEMORY_ALLOC`). The tools reserve a slot
  next to the exe right after injection, and `build.ps1` patches MinHook's search range to
  1.75 GB (`minhook/src/buffer.c`).
- `CameraManager`: the camera and track update run in the game's camera hook (once per rendered
  frame). It seeds from the game camera before the original tools' third-person switch, writes
  FOV to `GameRenderSettings + 0x54`, and has time freeze, HUD, LOD and draw distance controls.
- `TrackPlayer`: rewritten around the same idea, with the template's bugs fixed (uninitialised
  track index, unsigned underflow, forward-only manual scrubbing, null `PrimitiveBatch`,
  quaternion sign flips), editable keyframes and the interpolation modes above.
- `VisualsManager` and `LightManager`: new, from the original tools' visual environment and light
  manager code.
- Template bugs that would have stopped it working at all: globals that were declared but never
  defined, a missing `UI::Toggle`, `InputSystem::Update` never being called, the UI indexing images
  and fonts that were never loaded, `util::CharToByte` reading past its argument (corrupting
  signatures), the scanner dereferencing address 0 on a failed match, a raw-input handler
  being fed `WM_MOUSEMOVE`, and keyboard polling with `GetKeyState` from a thread without a
  message queue.
- boost and DirectXTK removed (std/Win32 instead). Project retargeted to VS 2022 (v143) with
  static CRT and no LTCG.

## Known risks / not verified

- The light manager builds the game's light entities the way the original tools did, with
  hardcoded vtable addresses. It's disabled automatically on any other game build, but on this one
  it's untested.
- The draw distance far plane (`RenderView + 0xF0`) was found by looking at live memory. Things
  the game streams or culls separately may not appear further out.
- *Load the world around the camera* registers a focus with the game's zone streamer through the
  same functions `ZoneStreamerFocusEntity` uses. How much memory two loaded areas take, and
  whether the streamer keeps running while time is frozen, hasn't been measured.
- Following other objects: picking traffic worked in-game, but the camera swung around wildly
  (it was 70 - 400 m away with *Turn with it* on), which is why the camera now goes next to what's
  picked. The *Nearby* list, the markers (they assume the game view fills the window) and the
  traffic / crowd split haven't been tried in-game yet. Swarm data was read live, not confirmed
  against the game's own code.
- The camera update function's extra arguments, the meaning of the third-person switch values and
  the DoF method numbers are inferred.
- The template's controller-input hook (blocking only the gamepad from the game) isn't ported.
  "Block game input" blocks all player input.

## Credits

- [Hattiwatti](https://github.com/Hattiwatti) for the original Mirror's Edge Catalyst Cinematic
  Tools and the CinematicTools template this is built on
- MEC-TC (Mirror's Edge Catalyst Time Changer) for the time of day address and the stuck clock fix
- [MinHook](https://github.com/TsudaKageyu/minhook), [Dear ImGui](https://github.com/ocornut/imgui)
  and [inih](https://github.com/benhoyt/inih), see `THIRD_PARTY_NOTICES.md`

## License

MIT, see `LICENSE`. The port is built on Hattiwatti's CinematicTools template and the original
tools' findings, used with Hattiwatti's permission. The bundled libraries keep their own licenses
(`THIRD_PARTY_NOTICES.md`); include that file with any release of the DLLs.
