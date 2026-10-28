# Mirror's Edge Catalyst Cinematic Tools, with keyframed camera tracks

A rebuild of Hattiwatti's Mirror's Edge Catalyst cinematic tools on his
[CinematicTools template](https://github.com/Hattiwatti/CinematicTools), with an in-game menu:

- free camera (mouse, keyboard, gamepad) and **keyframed camera tracks**: position, rotation,
  field of view, motion blur and time of day, with pause, live preview and a fixed playback rate
  for frame dumping
- play as Faith while a track plays, **follow Faith** or NPCs, traffic, drones and moving props
- **load the city around the camera**, so a far-away camera isn't surrounded by missing props
- time freeze, HUD toggle, time of day, Fix LOD, draw and shadow distance, motion blur, visual
  effects (color, DoF, grain, light, tonemap, vignette) and lights
- **native IGCS DoF integration** for the standard IGCS Connector addon; this fork's first
  implementation is a test build awaiting validation in Mirror's Edge Catalyst

## Installing

1. Copy `CT_MirrorsEdge.dll` and `usp10.dll` (from a release, or `dist/` after building) next to
   `MirrorsEdgeCatalyst.exe`.
2. Start the game and press **F5** for the menu, **F10** for the free camera.

No injector is needed: the game loads `usp10.dll`, which loads the tools. It works next to
Catalyst Clarity; another mod with its own `usp10.dll` can't be used at the same time. Windows 10
or 11. To uninstall, delete the two files and the `Cinematic Tools` folder.

Settings are saved in `Cinematic Tools\config.ini`, the log in `Cinematic Tools\CT.log` (the
previous one is kept as `CT.previous.log`, with `[CRASH]` lines at the end if the game crashed).

## IGCS DoF

The native camera interface lets [IGCS Connector](https://github.com/FransBouma/IgcsConnector)
move this camera during a DoF capture, then restore the starting pose and FOV when the
session ends or is cancelled. After a render reaches **Done**, save the result before
clicking **End session**; that action removes the rendered result. It supports
multishot sessions used by IGCS DoF; panorama sessions are not implemented in this first version.
Install the standard addon and its DoF shader/texture separately, following the
[installation and test guide](docs/IGCS_DOF.md). Use one camera provider at a time: disable
the IGCS Relay or another IGCS camera provider for these native tests.

The camera DLL does not automatically pause the game or change visual effects. Freeze the
scene and time of day, and disable native DoF and motion blur before a capture. End or cancel
the capture in ReShade before changing the camera or loading another level.

This integration does not change Faith's texture streaming or tattoo resolution.

## Building

Visual Studio 2022 (or the Build Tools) with the C++ workload and a Windows 10/11 SDK:

```powershell
powershell -ExecutionPolicy Bypass -File build.ps1
```

This fills `dist/` with `CT_MirrorsEdge.dll` and `usp10.dll` (static CRT, no redistributable
needed). The C4244 warnings from `Input/InputSystem.cpp` come from the template.

Run the production capture-state tests and check the built DLL's x64 exports:

```powershell
powershell -ExecutionPolicy Bypass -File tools/test-igcs.ps1 -DllPath dist/CT_MirrorsEdge.dll
```

The GitHub Actions workflow builds and tests both Release and Debug x64. Its Release artifact
contains the two DLLs, licenses, the IGCS test guide, checksums and `BUILD_INFO.txt` identifying
the exact commit. A successful build is not an in-game compatibility result.

The camera-session math and ABI assertions also run without Windows:

```sh
g++ -std=c++14 -Wall -Wextra -Werror -pedantic -ICT_MirrorsEdge tests/igcs_session_tests.cpp -o /tmp/mec_igcs_tests
/tmp/mec_igcs_tests
```

## Controls

Defaults, rebindable in MISC → *Change hotkeys...*

| Key | Action |
|---|---|
| F5 | Menu |
| F10 | Free camera on / off |
| W A S D / Space / Left Ctrl | Move |
| Mouse, arrow keys | Look (with the menu open: hold the right mouse button outside it) |
| Numpad 1 / 3 | Roll |
| Page Up / Page Down, mouse wheel | Field of view |
| Numpad + / − | Add keyframe / delete the last one |
| Numpad \* | Play / stop the track |
| Numpad 5 | Follow what the camera looks at (again: let go) |
| Delete | Freeze time |
| Home | Hide HUD |
| End | Block / unblock game input |

Numpad 1, 3 and 5 need Num Lock on. The gamepad works too. Grey `(?)` marks in the menu explain
each option.

## Making a camera move

1. Press F10, fly to the first shot and press Numpad +. Repeat for each shot.
2. In the menu (CAMERA → *Camera tracks*), open a keyframe to change its position, angles, FOV,
   time to the next one and how it gets there (Smooth, Linear, Ease in/out or Hold). The arrows on
   a row move a keyframe earlier or later.
3. Press Numpad \* (or *Play*) to play; *Pause* holds the camera on that frame.
4. For recordings, set *Fixed playback FPS* to the capture frame rate.

Keyframes also store motion blur and the time of day: tick them in *Keyframe options* and the
track sets them too (06:00 → 18:00 → 06:00 is a full day and night). *Faith keeps control while a
track plays* (on by default) lets you play as Faith during playback.

Tracks and lights aren't saved when the game closes.

## Menu tabs

- **CAMERA**: camera on / off, tracks and keyframes, time and HUD (time of day with ±10 min,
  *Stop the clock*), detail and draw distance, sun shadows, motion blur and camera speeds. *Show
  Faith's body* shows her third-person body, hair included. The time of day controls are
  disabled while MECMP is open, because MECMP syncs the time of day.
- **FOLLOW**: *Follow Faith*, or click something in the *Nearby* list to follow it (the camera goes
  next to it), or aim at it and use *Attach to what the camera looks at*. *Turn with it*, *Tilt with
  it too* and *Smoothing* change how the camera follows.
- **VISUALS**: open an effect and tick *Change this effect* to override it.
- **LIGHTS**: add sphere and spot lights at the camera. Use *Forget all lights* after loading
  another level or checkpoint.
- **MISC**: text size, mouse cursor, hotkeys, console window.

## Loading the world around the camera

The game only streams the city in around Faith, so a far-away camera sees missing props. *Load
the world around the camera* (CAMERA → *Detail and draw distance*) also loads it around the free
camera, ahead of it and along the selected track:

- *Stream distance* loads up to 600 m all round, or 1000 m with *Only where the camera looks*.
- The game crashes when some of its limits run out (zones, meshes, model instances, the renderer's
  draw batches and memory). The tools watch them and hold the camera's zones back in time; the
  menu shows how full each one is.
- *Unrestricted* ignores the renderer's limits for more distance, but **the game can crash**.
- After starting the game, quit to the main menu and continue once: the next level load gets
  more room for model instances.
- Some safe houses may or may not load with this setting.

*Fix LOD* and *Show small objects far away* help with the detail of what is loaded.

## Credits and license

- [Hattiwatti](https://github.com/Hattiwatti) for the original tools and the CinematicTools
  template
- MEC-TC (Mirror's Edge Catalyst Time Changer) for the time of day address and the stuck clock fix
- [MinHook](https://github.com/TsudaKageyu/minhook), [Dear ImGui](https://github.com/ocornut/imgui)
  and [inih](https://github.com/benhoyt/inih)

MIT, see `LICENSE`. The bundled libraries keep their own licenses (`THIRD_PARTY_NOTICES.md`);
include that file with any release of the DLLs. See `CHANGELOG.md` for what changed in each version.
