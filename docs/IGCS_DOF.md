# Native IGCS DoF: installation and first in-game test

This fork adds a native camera interface for IGCS DoF. The initial implementation is a test
build: **validation in Mirror's Edge Catalyst is pending**. Automated checks cover camera
sampling, state restoration, data layout, Windows builds and exported function names; they
cannot establish render timing or image quality in the game.

## Install

1. Keep a copy of the existing `CT_MirrorsEdge.dll` and `usp10.dll` so you can restore your
   previous camera build. Copy this build's two DLLs next to `MirrorsEdgeCatalyst.exe`.
2. Install ReShade with addon support. The official IGCS Connector currently requires
   ReShade 6.4 or newer. Install the standard `IgcsConnector.addon64` beside the game executable.
3. Install the IGCS DoF shader and its texture using the
   [official IGCS DoF guide](https://opm.fransbouma.com/igcsdof.htm). These files and ReShade are
   separate downloads and are not included in this camera package.
4. Use **one camera provider at a time**. For the native test, temporarily move the IGCS Relay
   addon or other IGCS camera-provider DLLs outside the game folder, then restart the game.
   The standard IGCS Connector addon stays installed. The tools do not install or remove any
   addons automatically.
5. Start the game, load a normal gameplay scene, and enable the free camera with F10.
   In ReShade's Add-ons tab, check that IGCS Connector detects the camera as enabled and shows
   plausible coordinates and FOV. The camera menu is opened with F5.

Connection discovery starts automatically and tries at one-second intervals for up to 120
attempts. It stops after connecting. If the addon was unavailable during that startup period,
restart the game after installing it. Status and connection errors are recorded in
`Cinematic Tools/CT.log`. Hot unloading/reloading of the camera or connector DLL is not part of
this first version's supported workflow.

## Prepare a stable scene

- Stop camera-track playback, including paused playback. The capture start is rejected while
  a track is playing or paused.
- Use **Delete** to freeze game time and check the camera's time-freeze status. In CAMERA,
  enable **Stop the clock** to hold time of day. The native bridge does not toggle either
  setting automatically.
- In VISUALS → Depth of field, enable the override and turn off its **Enable** field. In the
  camera's motion-blur controls, enable the override and turn motion blur off. This avoids
  combining the engine's blur with the IGCS sampling test.
- Hide the game HUD, close the camera tools menu with F5, and keep the scene static while
  configuring and rendering IGCS DoF. Native follow/manual camera changes are held during a
  capture; this does not freeze animated objects in the game.
- For the first test, use the game's usual rendering configuration. Test Catalyst Clarity
  or another temporal/upscaling modification separately after the baseline works.

## First capture and restoration check

1. Place the camera on a stationary subject with clear foreground and background detail.
   Record the starting coordinates, orientation and FOV, or save a reference screenshot of
   the camera info. A moderate FOV makes the first test easier to inspect.
2. Configure IGCS DoF in ReShade using its official guide. First start a short capture and
   **cancel it from the IGCS controls**. Verify that the original framing and FOV return and
   normal camera movement works again.
3. Start a small complete render and verify that the sampling does not drift across the
   scene. When the render is **Done**, save the result first: IGCS still owns the camera at
   this point. Then click **End session**, which removes the rendered result and restores
   the starting camera pose. Verify that the original framing and FOV return.
4. Repeat with a visibly rolled camera and a camera looking in another direction. Sampling
   should follow the camera's screen-right/screen-up axes, including the roll.
5. Repeat a second capture immediately. Check that it begins from the original pose, rather
   than the final sample of the previous capture.
6. Try starting while the camera is disabled and while a track is active. Neither case should
   move the camera; IGCS should reject the capture until the camera is enabled and the track
   is stopped.

Allow enough frames between samples for the game to render the new view and for temporal
effects to settle. There is **no validated universal frame-delay preset** for Catalyst in
this first build. Compare a short render at increasing wait values if ghosting or misaligned
samples appear. A successful connection alone does not establish correct sampling timing.

End or cancel the IGCS session before pressing F10, editing a track, loading a checkpoint,
returning to the main menu or closing the game. During a session, camera controls that could
replace the capture pose are held until IGCS ends it.

## What this version covers

- Native connection and camera telemetry for the standard IGCS Connector.
- Multishot session start, movement and end used by IGCS DoF.
- Offsets measured from a fixed capture origin for absolute samples, and from the current
  sample for relative moves. Camera right/up axes come from the saved orientation.
- Restoration of the capture's starting camera pose and FOV when the IGCS session ends or
  is cancelled. Reaching the render's **Done** state alone does not end the session.
- Invalid input and conflicting session rejection.

Panorama capture and ReShade-effect recording along camera paths are not implemented in this
first integration. The camera's existing paths, lights and native postprocessing remain
separate controls. This change does not address Faith's tattoo/skin texture resolution.

## Report the test result

Include `BUILD_INFO.txt`, your game executable version/source (Steam or EA), ReShade and
IGCS Connector versions, and whether Catalyst Clarity or another camera/graphics mod is
loaded. Attach the relevant `Cinematic Tools/CT.log` lines and a before/after image if a
capture drifts or does not restore correctly. For image artifacts, include FOV, camera roll,
sample count and frame-delay settings.

If IGCS cannot find the camera, check the addon-enabled ReShade build, file location, startup
timeout and whether another provider is loaded. If the camera is found but a start is
rejected, verify F10 is enabled, track playback is stopped, and a previous session has ended.
