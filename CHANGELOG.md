# Changelog

## Version 2.1.0

- **Release 2.1.0: diagnostics off for users, no local paths in the DLL, the emotion model a separate download**
  (2026-11-16).
  - The research and measurement CT.log lines added since 2.0.2 (Pose and Voice, No culling, Bodycam, reflections) are
    now behind the hidden diagnostics switch (`[Debug] Diagnostics`, util::DiagnosticsOn()), like the others since 2.0.0.
  - ufbx's asserts are off (`UFBX_NO_ASSERT`): the DLL holds no build paths, and a bad FBX file no longer stops the
    game on an assert.
  - The time-lapse's unused same-time test mode was removed.
  - Test-status wording was taken out of the tooltips.
  - The emotion model (EmotionHelper.exe + emotion_feel.onnx) ships as a separate, optional zip.
  - The KSEC work (KSEC chases, KSEC in the streets) is not in this release.

- **Time-lapse and Pose window: fewer (?), shorter help** (the user's list, 2026-11-16).
  - **CAMERA > Time-lapse**:
    - No (?) on *Camera*, *Clock runs backwards* or *Start time-lapse*.
    - The *Test: every frame at From* tick is gone; it was never saved, so the test stays off.
  - **Pose window > Clip**:
    - A loaded clip now gets one short line: its name and file, its length, frames and frame rate, and how many of
      its bones turn the character's. When it can't play on the character, the line says why.
    - The full reading and fit reports are on hover (and in CT.log as before). The yellow *Check:* line stays.
  - **Pose window > Voice**:
    - No (?) on *Feeling* or *The sound is heard*.
    - No (?) in *Settings of the living face* (Blinks, Brows, Head, Eyes, Looks away, Eyes hold their target, Take).
  - **Pose window > Options**:
    - *Idle movement*'s (?) is now two sentences.
    - Most other help texts are one or two sentences: Group the face, Rings on the bone's own axes, the Joints
      ones, the While the keyframes play ones, the path over the keyframes, Look at, Auto key and Hold the times
      around a new keyframe.
  - Checked by read-only agents against copies of the files from before. The critic's five notes were fixed: each
    short text still states only what the code does, and a clip that can't play shows why.
  - Build 00f1466ff665 (whole folder, Idle), installed with the game closed (game folder and dist, hash checked).
    Built without errors; not tested in game.

- **CAMERA > Time-lapse goes along a camera track that's playing, and can take one picture every so long** (the user, 2026-11-16: "timelapse should be able to
  record it even if you have a camera track"; CT.log 11-16 00:15:39: Still picked, Track #1 playing, the time-lapse
  refused with "a camera track is playing: stop it first").
  - With Still picked, a track playing (or paused) when Start is pressed now makes the time-lapse along that track:
    its frames spread from the track's start to its end, each at its place on it.
  - Its time of day: the track's own when the track sets the time of day as it plays (its keyframe options' Time of day
    tick) and has one at every frame, as when it's played; otherwise From - To.
  - The menu says so while a track plays; CT.log: `Time-lapse: the camera track 'X' was playing, so it goes along it
    (Still was picked), its start to its end, with ...`. The menu's own Camera setting stays Still.
  - A track started during a Still time-lapse still stops it (it would move the held camera's game speed).
  - **New: Pictures > One every so long of the clock** (the user: "also add a slider that takes images per x
    minutes/seconds since it's a timelapse"): *One picture every* N game seconds or game minutes (a button switches the
    unit; 1 s - 4 h) from From while it's within To, like a time-lapse camera's interval. The menu shows how many
    pictures that makes, how many a game hour, and the video's length at the frame rate; over a whole day as many as fit
    (the next would be From again); more than 20,000 is cut there (the menu says it ends early). Only with the clock
    From - To (not the same-time test or a track's own time of day, which go by the video's length). Saved as
    `TimelapsePictures`, `TimelapseEvery` (game seconds), `TimelapseEveryUnit`.
  - Checked: compiled clean; the offline harness passes with the new cases (a track that sets the time, one that
    doesn't, one that does but has no time of day on its keyframes; the interval's counts, steps, cut, whole day,
    backwards over midnight, a run of 6 pictures 2 game minutes apart, the config, the menu soak).
  - Build **64CB2E245EF2** (11-16 00:30, Idle priority, the whole shared tree: the staged BDA75BD9B1C2's + these).
    STAGED in `Timelapse_2026-11-14\builds\64CB2E245EF2\`; a job installs it once the game
    closes, only over E3F1650380FC or BDA75BD9B1C2 (result in install_result.txt there). Superseded by the fixes below
    (they go in with the pose session's next whole-tree build, staged to land over it). Not committed.
  - **Fixed after the user's "check about any bugs or issues and fix them no mistakes"** (the first real run's log and
    pictures read, then a fourth skeptic review of today's changes):
    - One picture every dropped the last picture for some minute values: the slider leaves 2.1 min as 126.0000076 s, so
      06:00 - 20:00 gave 400 pictures and no 20:00 one. The interval is now kept in whole game seconds, and the count
      has a small tolerance.
    - An interval longer than From - To put the second picture after To: it's now From and To (the menu says so).
    - The seconds / minutes button changed the value (60 min became 600 s); it only changes how it's shown now.
    - Over a whole day with an interval that doesn't divide 24 h (7 min), the menu said it loops: it now says the gap at
      the end isn't even.
    - With a track playing and Still picked, the menu's count, summary and time estimate are worked out as Start will
      make them (its own time of day only when it has one at every frame, then by the video's length); the menu line
      says which. The same-time test along a playing track keeps every frame at From.
    - Continue of a still time-lapse while a track plays is refused (it would have stopped at once).
    - A hand-edited `TimelapseEvery` that isn't a number or is 0 or less falls back to 60 s.
    - The job queue's line came every frame (its window closes 10 s after a jump; frames are ~25 s apart with the whole
      city): now the run's first two frames and every 30th.
    - The HUD line said the HUD *is* in the pictures; from high up none of it showed. It now says whatever of it is on
      screen is.
    - The install-when-closed script (`Timelapse_2026-11-14\tools\install_when_closed.ps1`): every failure caught and
      written down, the DLL swapped in (a copy beside it, then a move over it), the old one put back if anything fails
      after the game folder's changed. Tested on temporary files: installs, refuses another build, rolls back.
    - Checked: compiled clean; the offline harness passes (36 runs, new cases for each fix).
    - In **B1F4273D6225** (the pose session's whole-tree build, 11-16 02:01 at Idle, from these files as of 01:05),
      INSTALLED 02:03 with the game closed (game folder and dist); every one of these fixes' strings checked in the DLL.
      64CB2E245EF2 was never installed. Not tested in game. Not committed.
  - **Measured on the first real run** (Research_Timelapse section 1): no exposure flicker (the sky smooth); at night
    the lit city centre changes between frames (the world moves ~25 game s a frame), by day it's smooth; the job queue
    62 - 66 % full after each frame, never full.

- **GAME > Level streaming > No culling: a View margin and a Small objects range** (the user, 2026-11-15, after trying
  it: "can we find any way to improve fps with this? right now when you enable it you get 7 fps with my pc").
  - **Measured first** (CT.log 13:49 - 13:54): about 100 fps off, about 7 fps on. The game's Present call stayed at
    0.2 ms, so the time goes on the CPU. The camera's view drew about 325,000 meshes instead of about 40,000; most of
    the extra are outside the view, where they can't be seen at all.
  - **View margin** (0 - 90 degrees, default 20; at the top, all around = the earlier behaviour): a mesh outside the view
    is let in only within this angle past the view's edges, as seen from the camera (the game's own side and near
    planes, moved out; the far plane keeps the game's own cut, so the draw distance stays).
  - **Small objects** (default: at any distance; at the left, the game's own size test): a mesh the game found too
    small is let in only out to this distance from the camera.
  - The occlusion test stays skipped, as before.
  - CT.log's 10-s line now splits what's let in (outside the view, within the margin / too small) and what's left to
    the game (past the margin / small ones past their range), and counts planes that couldn't be read or weren't laid
    out as expected (then the margin goes on all 6, the earlier rule). A changed setting is logged once it has held
    for a second: `No culling: now view margin ..., small objects ...`.
  - `[Misc] NoCullingViewMargin` = 20, `NoCullingSmallRange` = 2000 (any distance).
  - **Checked:** a skeptic review (its finding that the margin also pushed out the far draw-distance cut, by 1.5x at
    20 degrees and with no limit in the band's corners, is fixed, plus several smaller ones); the offline harness:
    1,230,059 checks, including a geometric stand-in for the game's test with a double-precision reference over 6,000
    random views (407,996 meshes judged), 20 fixed cases at known angles and distances (10 / 19 / 21 / 30 degrees past
    an edge, behind the camera, past the far plane at 26 km and 37 km, 72 degrees off at 1,000 km), a broken plane, an
    odd plane layout, 177 M calls from 8 threads with the sliders clicked under ImGui 1.61. **The fps it gives back is
    not measured yet.**
  - Build **A2CBA749CC53** (14:31, Idle, the whole folder: C1FAE37FBDAB's tree plus this and the road traffic
    session's cars and walkers on the ground, cleared by that session). **INSTALLED 14:32:31** with the game closed
    (game folder and dist, hashes checked twice; C1FAE37FBDAB kept as
    `Culling_Off_2026-11-14\builds\A2CBA749CC53\installed_before_A2CBA749CC53.dll`).

- **Road traffic: cars and walking people on the ground** (the user, 2026-11-15: "make sure that every car and traffic
  in general are actually on the ground check for faith's pos to find what ground is"). In the shared folder 14:10.
  Built in E883E6191F7A (14:12, Idle priority, the whole folder), which is NOT installed: its staged install was
  stopped at 14:16 because that build also holds the culling session's CullingOff edits, held until that session's
  review. **INSTALLED in the culling session's A2CBA749CC53** (14:31, Idle, the whole tree with these files as tested;
  installed 14:32:31 with the game closed). **Not tested in game.**
  - **What the ground is:** the drawn street (`Resources\GroundRoads.bin`, the road meshes' tops). It is what Faith
    stands on in the street with Solid street: 37 of 39 of her foot positions in the last two CT.logs are within
    1.3 cm of it.
  - **Cars:** the game's road nodes sit on the drawn road, but a car's height ran in a straight line from node to
    node, while the road rises and dips between them: 22% of the lanes were 0.2 - 0.89 m in the air and 21% were
    0.1 - 0.36 m into the road. Now each lane follows the road's own height, about every metre
    (`Camera\RoadTrafficHeights.h`, generated from the road data), and a car takes it where it is. Where road pieces
    lie stacked, it takes the top one that goes on for a metre, so it isn't lifted by a kerb lip under its middle.
    - Offline (the rules on every lane of the city, 300 s): cars more than 10 cm off the road 47% -> 0.011% of
      car-frames, the worst +0.89 m -> 0.12 m. What's left is at a few steps in the road mesh itself (up to 24 cm),
      which a car now ramps over in a metre.
  - **Walking people:** the game's walker height also runs straight to the next node's height, and on the street a
    fifth of the walking nodes are off the pavement. Whole placements walked 16 cm inside it, one loop 1 m under it,
    and some nodes floated up to 1.1 m. The 218 street nodes that are off get the pavement's height
    (`Camera\WalkersFloors.h`), so walkers walk straight between pavement heights. Walkers on roofs and walkways, P25
    (a raised floor the data hasn't got) and a few floats with no evidence either way are left as the game has them.
    - Offline (the walker rules with a walker on every path, 240 s): walkers more than 10 cm off the street
      35% -> 7.2%, the lowest -1.02 m -> -0.63 m.
  - **Faith's position as the check:** with diagnostics on, every 10 s CT.log has "Road traffic: the ground at
    Faith (...)": her feet against the road under her, and the cars within 80 m and walkers within 40 m against the
    road under them (the average, how many are more than 5 cm off, the worst one and where). The street-level code
    gives that (two read-only additions to GroundLevel).
  - **Left / not known:** the elevated highways and the underground loop have no road data (their cars as before);
    cars don't tilt on slopes (the game keeps no pitch); where each model's soles are against its height is
    unknown; the old car and walker tests and the game-data tools were lost in the 11-14 scratch cleanup, so this
    was checked with new tests in `Road_Traffic_Ground_2026-11-15\` (kept in the project folder).
  - Research_Road_Traffic_2026-11-03.md section 16.

- **New: GAME > Level streaming > No culling (camera view)** (the user, 2026-11-14: "lets add a tick that disables
  frostum culling and culling in general in the level streaming category"; the research is in
  `Research_Culling_Off_2026-11-14.md`, its tools in `Culling_Off_2026-11-14\tools\`).
  - **What it does:** the camera's view gets every mesh the renderer has loaded: the game's view frustum and
    small-object (screen size) tests are passed for every instance, and its occlusion test (meshes behind other
    things) is skipped. What's loaded doesn't change (that's the other Level streaming settings).
  - **How:** the game culls with one of two mesh culls, picked every frame by its MeshCull HiEnable setting: its tree
    cull (1, the game's own; read live) or its flat cull (0). While the tick is on, the setting is 0 (the game's value
    saved first, written back when it's off), and hooks on the flat cull's camera-view tests (0x1436C7AD0 /
    0x1436C77E0) add every instance after the game's own test, for every world view except reflection captures and
    planar reflections (types 1, 4, 5). Bit 31 (the "not too small" mark the other views need) stays the game's.
  - **A guard:** past WorldRenderSettings.GenericEntityMaxVisibleEntityCount (32768) visible generic entities in a
    world view, the game's genProcessCull job (0x143714D50) would read past its list; a hook holds its count to the
    room while on (and 3 s after), so the extra ones are dropped as the game's flush means them to be.
  - **Not opened up** (culled by the flat cull's own tests while on, so they can differ a little from the game's
    tree cull): shadows and the other views of the camera's world view, reflection captures, planar reflections.
    Not part of the mesh cull, so unchanged: particles (their frustum test comes before their size and distance
    tests), decals, lights, terrain, characters' animation culling.
  - **Costs fps:** the flat cull tests every loaded instance every frame, and every loaded mesh is drawn. Not measured
    yet.
  - `[Misc] NoCulling` = false (default) / true.
  - CT.log: `No culling: on. ...`; `No culling: the game's mesh cull was on its tree path (MeshCull HiEnable 1, ...):
    every world view switched to its flat path (HiEnable 0) while on`; every 10 s `No culling (10 s): cull path ...;
    ... a frame let into the camera's view that the game's frustum and size tests left out ...; the generic visible
    list: at most N asked of a room of 32768 ...; world views: type T 'Name' at x y z ...`; `No culling: the game's
    MeshCull HiEnable is back to 1 ...`; `No culling: off after N s. ...`.
  - **Checked:** two adversarial reviews against the game's code (the first found the tree path: the first version
    would have done nothing; the second the visible list's room); every finding fixed. The offline harness under the
    tools' ImGui 1.61 with its checks live: the 45 code checks against the game's own bytes, 411,038 checks, 350 M cull
    calls from 8 threads, the setting's switch and give-back (no renderer, a replaced renderer, the loading screen, a
    foreign object), the guard. **Not tested in game.**
  - Build **B325903C4079** (11-15 02:25, Idle; 644E0C3D5318's tree plus this). **INSTALLED 02:27:50** with the game
    closed (game folder and dist, hashes checked twice; 644E0C3D5318 kept as
    `Culling_Off_2026-11-14\builds\B325903C4079\installed_before_B325903C4079.dll`). Untested in game.

- **New: CAMERA > Time-lapse, a time-lapse made a frame at a time with the light worked out for each** (the user,
  2026-11-14: "do more research on how we can create timelapses and implement it"; the research, the three ways
  compared and what isn't known are in `Research_Timelapse_2026-11-14.md`).
  - **Why not just speed the clock up:** the game lights its time of day in steps (one every 8 s of game time, each lit
    ahead), reflections take 1 - 2 s a round, and the exact reset only comes when the clock stops, so a live
    time-lapse steps and flickers.
  - **What it does, for each frame:** holds the time of day at the frame's time; forces the game's own reset of its
    lighting there, at exactly that time (the same 5 passes and 2 more for every frame) with the game's own light steps
    held; captures every reflection again with that light and waits for the nearest round; then saves the finished
    picture (before the menu) as a PNG into `Cinematic Tools\Captures\Time-lapse <date time>\`, with `Time-lapse.txt`
    (the settings, a line a frame, the ffmpeg line that makes the video).
  - **Camera:** Still (held where it is, no shake) or Along the selected camera track (a hyperlapse: the frames spread
    from its start to its end, the clock from From - To or the track's own time-of-day keyframes).
  - **Settings:** From / To (or the same time twice for a whole day that loops), Clock runs backwards, Video length and
    Frame rate (the number of frames), Game time between frames (even motion of people, cars and clouds), and a test
    that makes every frame at one time to see what would flicker. Saved as `[Camera] Timelapse*`.
  - **Start / Stop / Continue**, a progress bar, what it's doing, the time left; it waits through pauses, loading and a
    minimized window, and stops with the reason (level changed, camera turned on or off, track changed, the light not
    done) so Continue can carry on.
  - **About 4 - 6 s a frame** on a normal level (much more with the whole city): a 10 s video at 30 fps takes 20 - 30 min.
  - Supporting changes: Util/Hooks.cpp (forced and quiet resets with a per-request outcome, the game's steps held,
    `TodManager` kept apart per level), Visuals/VisualsManager.cpp (the time-lapse's clock over all others, the
    reflections watched), Visuals/FrameShot.cpp (no half-written file left after a failed write; a pending picture can
    be dropped), TrackPlayer::TimeOfDayAt, CameraManager (the menu, the camera hook call, the still hold).
  - **Checked:** compiled clean; an offline harness (the menu under the tools' ImGui 1.61 with its checks live, and the
    whole flow against a fake game: 29 runs, freezes, pauses, failures, Continue, game speed 0.25, the config) passes;
    three adversarial reviews (game-side code, the state machine, the design) and a second round on the fixes, every
    finding fixed or written up. **Not tested in game.**
  - CT.log: `Time-lapse: started, ...`, `Time of day lighting: its own steps held for the time-lapse ...`, a line every
    frame `Time-lapse: frame N of M at HH:MM:SS (lit at HH:MM:SS): the light X s ..., the reflections Y s ...; saved as
    0000N.png`, and `Time-lapse: Done: ...` with the ffmpeg line.
  - Build **644E0C3D5318** (11-15 02:13, Idle priority, the shared tree built by the bodycam session with these files as
    reviewed; the earlier 6E8DD0FA6C65 of 01:06 had a first, unreviewed version of them). INSTALLED 02:14 with the game
    closed (game folder and dist, hashes checked; the time-lapse's strings checked in the DLL). **Not tested in game.**
    Not committed.

- **New: GAME > First person > First camera view: Default / Bodycam, the tools' own bodycam** (the user, 2026-11-14:
  "lets add a 'First camera view' (default/bodycam) ... to the game tab", then "discard everything you did and lets
  create our own then with better and improved features and better bodycam movement"; notes in
  `Research_Bodycam_View_2026-11-14.md` section 14).
  - **Replaces the port of the BODYCAM mod** (build CA197334C001, 11-14 13:29): `Camera/BodycamGraph.h` and
    `Camera/BodycamLogic.*` are deleted, and nothing of the mod's data or logic is used. Builds since 13:29 held the
    port or this work in progress.
  - **A camera worn on Faith's body:** on her chest, her head (helmet) or her right shoulder (`Camera/BodycamMotion.*`,
    pure math). Its place and facing come from her bones' world places (hips, neck, upper arms, chest, head, eyes),
    read only from her world pose. No game code is called.
  - **Aim:** your mouse moves it at once (*Follow your aim* sets how much), and nothing moves on after the mouse
    stops. Her body's own turns sway it: her facing is smoothed (*Body sway*, *Settle*).
  - **Movement:**
    - it hangs on a strap (*Strap bounce*): it stays on its mount at a steady run whatever the frame times are, and
      gives when she speeds up, stops, steps and lands;
    - step jolts when her feet come down while she runs (stairs too);
    - landing impacts after a real fall: a dip at its deepest about 70 ms after, stronger the faster she fell; not
      on stairs or a jolt of her hips;
    - a fine shake, more while she runs and after a hard landing;
    - leaning into turns;
    - *Level horizon* (a stabilized camera; it stops levelling near upside down).
  - **Looks:** Chest bodycam (the default), Helmet cam, Shoulder cam, Action cam (steady), GoPro (head mount). Any
    change makes it Custom; *Back to <look>* undoes the changes.
    - **GoPro (head mount)** (the user, 2026-11-15: "can we also add a go-pro preset?"): on her forehead, the widest
      view (100 degrees top to bottom) with the strongest fisheye, a stiff mount, the shake and jolts damped and the
      horizon half levelled, as an action camera's stabilization does; no REC overlay, a light vignette.
  - **Lens and picture:**
    - field of view (80 degrees top to bottom by default);
    - a fisheye bend through the tools' lens distortion (VISUALS' own lens distortion wins while it's on);
    - a REC overlay (blinking REC, the date and time, CAM 01) and a vignette, drawn behind the menu's windows and not
      bent.
  - **Only her first-person view:** cutscenes and other cameras stay the game's, the CAMERA tab's camera wins, and at
    health 0 it's the game's view until she's up. When it stops (Default, a cutscene, death, a loading screen, no
    Faith), the game's camera, ForceFov and near plane are given back: ForceFov and the near plane as read before the
    first write; the camera is the game's last if the game hasn't rewritten the view.
  - Config `[Misc]`: `FirstCameraView` (0 Default, 1 Bodycam), `BodycamPreset` (0 Chest, 1 Helmet, 2 Shoulder, 3 Action
    cam, 4 Custom, 5 GoPro), `BodycamBase`, `BodycamMount`
    and one key per value (`BodycamFollow` ... `BodycamVignette`). A look's values are its own; only Custom's are read
    from the file.
  - CT.log, all lines start with `Bodycam:`:
    - applied: `applied: the view is the bodycam's (look ..., FOV 80, near plane 0.03, ...; the game's ForceFov was -1
      and its near plane 0.06, kept to give back)`;
    - given back: `stopped writing the view (...); given back: ...the game's field of view (ForceFov -1, ...); the
      game's near plane (0.06)`;
    - `her bones read (...)` and `her front found over N frames (...)`, expected to say "up x right, as her rest pose
      says".
  - **Checked offline:**
    - the movement: 84 checks, including steady runs with frame times 20 % off (1.5 mm), 60 vs 144 fps, landings,
      stairs, hip jolts, the mouse alone, upside down, paused and moved, and tiny frame times;
    - the menu and the view writes under the tools' ImGui 1.61 with its checks live: 8 runs of 30,000 - 60,000 frames
      against a simulated game (cutscenes, shots written once, deaths, respawns, loading, pause, the free camera,
      stale frames, both handednesses);
    - two adversarial reviews (game side, movement math). No crash found; all their verified findings are fixed.
  - **Not tested in game.**
  - Build **6E8DD0FA6C65** (11-15 01:06, Idle priority): the shared tree of C1FC968E1B4A (time-lapses, culling, the UI
    session's level streaming change) plus this session's last `Bodycam.cpp` and `LensDistortion.cpp`.
    INSTALLED 01:06 with the game closed (game folder and dist, hashes checked); the replaced C1FC968E1B4A is kept in
    `Bodycam_2026-11-14\builds\6E8DD0FA6C65\`. Not committed.
  - Build **644E0C3D5318** (11-15 02:13, Idle priority) adds the GoPro (head mount) look: the whole shared tree (the
    time-lapse session's reviewed files, the culling session's CullingOff switched off until its review). Movement
    checks 92 (the GoPro look in every per-look test, the config numbers), the ImGui harness runs and a drawn menu with
    the GoPro look pass. INSTALLED 02:14 with the game closed (game folder and dist, hashes checked); the replaced
    6E8DD0FA6C65 is kept in `Bodycam_2026-11-14\builds\644E0C3D5318\`. **Not tested in game.** Not committed.

- **GAME > Level streaming: *Room for the whole city* is always on** (the user, 2026-11-14). The tick is gone, and
  a config's `[Misc] BatchSpreadChunks = 0` now counts as on too (the default spread, 1700 chunks). It still
  switches only when the renderer's draw batches allow it. The menu shows a line while it's waiting for that, or
  when this game build can't do it. The All to Always tooltip no longer talks about it being off. It went in with
  the folder's later builds (in 00f1466ff665, installed 11-16, checked); not tested in game.

- **Fixed: a start-up data race on the tools' hook list** (the user, 2026-11-13: "Yes, fix it").
  - **What was wrong:** the list of hooks made (Util/Hooks.cpp) had no lock. At start-up, three threads add to it in
    the same seconds: the tools' start-up thread, the drawing thread at the first frame, and the game's thread at a
    first use. In CT.log 2026-11-13 21:30:52-53, the drawing thread's ReplayStore/SsrOrder hooks and the start-up
    thread's road traffic hooks were made together. Two threads adding to the map at once can break it.
  - **Also wrong:** CreateGameHook reported a hook as made when the list grew during its call. That counted another
    thread's hook as its own, so a failed hook could read as made. For example, a failed RoadTrafficCarStep would turn
    on "more by day" without the lane rules.
  - **Now:**
    - The list is only read or changed under its own lock, including SetHookState at exit.
    - CreateHook returns whether its own hook was added, and CreateGameHook gives that back.
    - With diagnostics on, a hook that takes 1 s or more to make writes `Hook timing (diagnostic): <name> took N s to
      make, on thread <id>`. That measures the start-up wait (4 - 6 s before the first frame) in game.
  - Two reviewers checked every thread and lock order (no cycle), and every caller. No hook is made at a different
    time or on a different thread.
  - Build BEB0F0C7518D (11-13 22:59, Idle priority; the folder is BB054C26265F plus this, plus the pose session's
    PoseEditor.cpp from 22:46).
    - INSTALLED 23:08 with the game closed (game folder and dist, hash checked). **Not tested in game.**

- **Less CPU on the game's main thread, same look** (the user, 2026-11-13: "Find any way we can improve cpu usage of
  the game", then "Tools fixes only"; the research is in `Research_CPU_Memory_2026-11-13.md`).
  - **Shard tip:** the far-away Shard models' tip for lightning is only looked for while lightning can come (a storm,
    or Strike now). The two walks through every model group were a 1.2 - 2.5 ms frame every 2 s in the city.
    - The real tower's top (the Districts menu) is still looked at every 2 s.
    - A burst of lightning can come one frame later than before. In a rare case it waits for a loading screen: Strike
      now pressed in the very frame before one starts.
    - CT.log: without lightning, the tip line now reads `The Shard's tip for lightning: none (not looked for while no
      lightning can come)`. The tip line comes back on each storm start and each Strike now.
  - **Street level (Solid street on):** a frame whose every input is the last plan's, bit for bit, takes that plan
    again instead of planning anew. Examples: Faith still, the game paused, time frozen.
    - A plan is still made anew at least every 250 ms.
    - The planner's work lists are kept from frame to frame instead of being made again: about 85 heap allocations a
      frame down to about 4.
    - It was 0.15 - 0.21 ms of every frame. How often frames repeat while she moves is unknown.
    - The 10-s `Street level (performance, diagnostic)` line now says how many frames took the last plan again.
  - **Hair fix (free camera, her body shown):** her customization component's values that point at no memory are no
    longer read again while Windows says their memory still isn't there (VirtualQuery). Each such read was an access
    violation caught, measured at about 1.6 microseconds outside the game. Every other value is read every frame as
    before, so a cloth is found the frame it's readable. Its timing slot, "the game's settings applied", took 0.05 -
    0.23 ms a frame. New diagnostic line: `Faith's hair (diagnostic): ...`.
  - **First pose Take:** the game's pose blend is now hooked at the tools' start (pose_editor::Init, last in
    Main::Init) instead of at the first Take on the game's thread. That frame took 45 - 53 ms; that making the hook
    caused it is inferred.
    - The hook now exists in every session, as a pass-through (about 10 ns a call, measured outside the game) until
      something is posed.
    - CT.log shows `Hooked PoseOverrideBlend` and `Pose: the game's pose blend hooked ...` at every start.
  - **Not changed:**
    - The 4 - 6 s wait at start-up: it comes before the game's first frame, and moving the hook that waits would only
      move the wait.
    - A data race on the tools' hook list at start-up was found (two threads add hooks at once with no lock). Fixing
      it is a separate change, waiting for the user.
  - Build BB054C26265F (11-13 22:45, Idle priority; the folder is 935727C51F4B plus these, plus the pose session's
    PoseEditor.cpp from 22:39 and VoiceEmotion from 22:16 - 22:19).
    - INSTALLED 22:46 with the game closed (game folder and dist, hash checked). The old DLL is kept
      (CT_MirrorsEdge_935727c51f4b.dll). **Not tested in game.**

- **Reflections settle sooner after a time of day jump** (GAME > Time of day, *Light catches up with time jumps*).
  - **What lagged:** measured on two jumps, reflections were final 4 - 5 s after the jump, against about 3 s for
    the shade.
    - They were captured again only once the light's reset had done its 5 passes.
    - Then the game captures its 32 nearest reflections one at a time (1 - 2 s).
    - Then it started them all over once more by itself, when the reset's last step moved the sky the light is worked
      out for 8 minutes ahead.
  - **Now:**
    - The reset's later steps stay at the jump's time, so that sky doesn't move during it.
    - Reflections are captured again after 3 of the 5 passes, and once more when all 5 are done, so they end with the
      whole new light.
  - CT.log now names what makes the game capture every reflection again during the catch-up: `Reflections: all N
    marked to be captured again by the game's sky cubemap (its sky changed)` or `... sun test (the sun moved)`.
  - Build D9CA6F54AFEF (11-13 16:01, Idle priority; the folder is B3846F99D215 plus this).
    - INSTALLED 16:03 when the game closed (game folder and dist, hash checked). **Not tested in game.**

- **Fixed: after dragging the time of day, the light could stay at a time you passed through** (GAME > Time of
  day, *Light catches up with time jumps*).
  - **What went wrong:** in the 2026-11-13 13:43 run, the light's reset ran at 11:21 while the slider was still
    moving, the drag ended at 08:54, and the last reset was skipped ("already at 08:54"). The shade kept the 11:21
    light until the game's own steps caught up, several seconds later.
  - **Why:** whether a reset is needed was judged by the time the game sets at each of its own steps, which is set
    to the clock before anything is lit for it.
  - **Now** the tools note the time each lighting pass is worked out for, and judge by the two the picture shows. No
    reset when both are within 10 minutes of the clock: the game's own steps light up to about 8 minutes ahead.
  - Build B3846F99D215 (11-13 15:29, Idle priority; the folder is 08A51E9A3C59 plus this).
    - INSTALLED 15:30 with the game closed (game folder and dist, hash checked). **Not tested in game.**
  - When it skips one, CT.log says so with the times, for example `the light shown is for 09:02 - 09:02 (the clock
    08:54), no reset`. When it can't tell yet (just after a level loads), it says `the light shown isn't known yet`
    and goes by the old rule.

- **GAME > Pose: "Animation clip": Mixamo animations played on a posed character** (the user, 2026-11-11: "I want a system
  that takes a custom animation (maximo etc) and plays it via the pose system").
  - Build 907551328CFE (11-11 02:59, Idle priority; the folder is 2.0.1 (4cdb6de) plus this).
    - INSTALLED 03:00 with the game closed (game folder and dist, hash checked twice). **Not tested in game.**
    - The game folder's `Cinematic Tools\Animations` has the two test clips in it ("Samba Dancing.fbx" and
      "mixamo.fbx"). Delete them whenever you like.
  - **How to use it:**
    - Download the animation from Mixamo as FBX Binary, 30 frames per second, keyframe reduction none. Without Skin
      or With Skin both work.
    - Put the .fbx file in the game folder's `Cinematic Tools\Animations` (the tools make the folder).
    - Take a pose, then pick the file under GAME > Pose > Animation clip. It's read in about 0.1 s and starts playing.
    - Controls: Use the clip, Play / Pause, From the start, Loop, Time, Speed (0.05 - 4x).
    - **Clock:** Game time follows Game speed and stops with Freeze time. Real time plays with time frozen too. The
      camera track's time plays from Start as the track plays, through a shot sequence's cuts.
    - **Hips:** Stay in place (only up and down) or Follow the clip's travel. Turning the Hips with the ring turns the
      clip, and its travel goes the new way from where the character stands.
    - **Use this frame as the pose:** makes the clip's frame now the held pose (then edit it, save it, Reset goes
      back to it). Playing on afterwards continues the clip where it was.
    - Your bone edits stay on top of the clip. Bones can't be grabbed while it plays; pause it first.
  - **Works on** Faith, KrugerSec, men, women and women in heels (the full skeletons). The tools pick the men's or the
    heeled women's rest pose by the character's own bone lengths: the two share one bone list, but the heels pose has
    the feet 29.6 deg steeper.
  - **How it works:**
    - The file is read with ufbx 0.23.1 (third-party, MIT or public domain; credited in THIRD_PARTY_NOTICES.md). That
      covers Mixamo's PreRotation (FBX 7.7), its units and axes, and its takes ("mixamo.com" is the animation).
    - Each of Mixamo's 65 bones gives its game bone's world turn, from the clip's T-pose to the game's own rest pose (an
      A-pose with bent elbows, from the game's skeleton files).
    - The spine, legs, arms, hands and fingers follow the clip's directions exactly.
    - Each bone's roll is matched through the joints:
      - the elbows bend about the game's own elbow hinge (measured in the clip from its bent frames);
      - the knees bend about the knee's averaged hinge;
      - the palms face the clip's way (the knuckles).
    - Feet and toes take the clip's turn from rest to rest, so flat feet stay flat and heels stay heels.
    - Bones Mixamo doesn't have:
      - ElbowRoll and KneeRoll follow the forearm and shin, as the game keeps them;
      - Faith's forearm, upper-arm and thigh twist bones take the share of the twist her own animation gives them
        (fitted on 4,992 of her recorded frames);
      - Neck1 and the finger bases take the game's rest pose;
      - the face and the other helper bones keep the pose taken's turn.
    - The clip faces the way the character faced when it was put on. The Hips move as the clip's, scaled to the
      character's height.
  - **Checked offline** on two real Mixamo exports (three.js's public examples "Samba Dancing.fbx", FBX 7.4, and
    "mixamo.fbx", FBX 7.7):
    - Mixamo's rest pose is a T-pose, the same as the skin's bind pose. Only Hips has translation keys.
    - The tools' C++ (a test program running PoseClips.cpp) matches a separate Python model of the retarget.
    - On all four skeletons, the limbs point exactly along the clip (0.0000 deg), and the elbows bend about the game's
      hinge (0.000 deg).
    - The forearms keep the game's rest twist exactly. The knees are within 7 - 8.5 deg of their averaged hinge, because
      the clips' own knees aren't perfect hinges.
    - Stick figures of the clip and of Faith match frame by frame.
  - **Reviewed:** three adversarial rounds:
    - wf_efde6085-c1f: 3 lenses with a skeptic for each finding;
    - wf_d75c3da4-f90: a regression check of those fixes;
    - and the fixes' own offline checks.
  - **Fixed in review:**
    - a rig without finger bones left the arms 48 - 58 deg off;
    - a mesh named like a bone could be taken as the bone;
    - a long file name could close the game;
    - the camera track clock jumped back at a shot sequence's cuts;
    - turning the Hips made the character walk sideways, or swing round the clip's start;
    - playing on after "Use this frame" turned the character twice;
    - the elbow and knee rolls and Faith's twist bones stayed as taken;
    - the upper arm and forearm were rolled 10 - 11 deg off the elbow's hinge;
    - Idle movement snapped on and off at Play and Pause;
    - smaller menu and clock issues.
  - **Log:**
    - "Pose: clip read: ..." (frames, fps, bones, units, the bind pose check, the hinges, any "check:" warning);
    - "Pose: clip <file> on <who>: N of its bones turn <skeleton>'s ...".
  - **Not known until tested in game:**
    - How it looks.
    - Whether the game shows its 30 Hz pose steps smoothly. It applies the pose at its own step, so playing back
      faster than real time skips frames.
    - Mixamo "Without Skin" files weren't seen. The tools take their bones' default pose as the rest pose, and warn if
      it isn't a T-pose.
  - **What it can't do:**
    - no foot or hand contact (different body sizes mean some sliding);
    - no faces;
    - no crowds or knocked-down characters;
    - it pauses while a replay records or plays, as posing does.
  - Files:
    - new: `Camera/PoseClips.*`, `Camera/PoseRest.h` (generated), `ufbx/` (third-party);
    - changed: `Camera/PoseEditor.*`, `Camera/CameraManager.cpp` and `Camera/TrackPlayer.h` (the clocks), the project
      files, `THIRD_PARTY_NOTICES.md`.
    - Research: section 22 of `Research_Pose_Editor_2026-11-08.md` (outside the repo).
- **GAME > Pose > Animation clip: clips from any human rig, not only Mixamo's** (the user, 2026-11-11: "can you make a
  converter program? for the bones etc." and "not for unreal i mean in general").
  - Build 43DB727B84D4 (12:02, Idle priority; the folder is 2.0.1 (4cdb6de) plus the clips and this).
    - INSTALLED 12:03 with the game closed (game folder, hash checked twice). **Not tested in game.**
  - **Which bone is which:** found by their names' words and where they hang (no list of rigs):
    - the head, hands and feet by name; the Hips where the head and both feet hang from; the spine, legs and arms on
      the paths between them (by words, else by order); the fingers by name. Twist, roll, IK, corrective and other
      helper bones are passed over.
    - Checked on Mixamo's names (`mixamorig:Hips`, and `mixamorigHips` with no colon), Unreal's mannequin (UE4 and
      UE5: pelvis, spine_01 - 05, clavicle_l, thigh_l, calf_l, ball_l, thumb_01_l...), Unity Humanoid, 3ds Max Biped
      (`Bip01 L Thigh`), Blender Rigify (`DEF-thigh.L`), Daz, VRoid and Character Creator.
    - A rig whose left and right are named the other way round is found by its toes (they must point ahead) and
      swapped back.
    - **The bone map:** "Save its bone map to edit" writes `<the clip's name>.bones.txt` beside the clip ("Hips =
      pelvis" and so on). Change a line and pick the clip again; while the file is there it's used instead. "Its
      bones" lists what was found.
  - **Its rest pose** (what each bone's turn is measured from):
    - the skin's bind pose when the file has its mesh (FBX "With Skin");
      - followed when the rig was moved or turned after binding;
      - a skin that covers only part of the body (a hat on the head) isn't taken;
      - an empty bind matrix is passed over, and of several skins the one with the most bones counts first.
    - else the bones' default pose if it's a T- or A-pose (Mixamo "Without Skin": as before);
    - else (an armature-only export, Blender's for one, whose default pose is just a frame of the animation) it's
      worked out from the body and the motion. The check line says so:
      - each bone's roll from the body over the whole clip (across the hips, across the chest, the chest's up, the
        knuckles), in the bone's own frame;
      - a bone aiming past one in between (UE's spine_03 at spine_05) by where that bone is over the clip;
      - the elbows' and knees' hinges in the forearm's and shin's own frames;
      - the feet and toes where each foot stands flattest (of the frames where that leg stands);
      - the Hips' height standing on its legs.
  - **Files whose settings don't match their data:** the up and the unit are checked against the body.
    - The user's `exported-model.fbx` (ComfyUI's three.js FBX exporter) says Z up and centimetres. Its bones are in
      metres and it stands along +Y: the installed build said "no Hips bone" for it, and with only the bone finder it
      read as a 1 cm character upside down.
    - The unit goes by tens when the legs come out under 10 cm or over 10 m long. (The clip plays the same at any
      unit; it counts for the tools' cm-sized checks.)
    - The up is only taken from the body when the rest pose doesn't stand by the settings, and then only:
      - when the skin's bind pose stands as a T- or A-pose along another axis (a mesh isn't bound lying down);
      - or when the settings are shown wrong already (the unit off by tens, or no axes given):
        - a default pose that's a T- or A-pose along another axis, if it's a rest pose kept apart from the animation
          (no frame of the clip within 10 deg of it) and the body has a floor along that axis;
        - else the axis the body stands along in the most frames, of those along which its lowest point (the floor)
          moves 1.2 legs at most over the clip, if that beats the settings' up.
    - A clip with right settings that lies, falls, swims, hangs or stands on its hands keeps them.
    - **Not always right:** from the motion alone a clip that lies can't always be told from one that stands with
      wrong axes. Three review rounds found such cases for every rule tried. With its settings shown wrong, a clip
      that lies more than it stands on a still floor is stood up.
    - **The way out:** the bone map now holds the file's up and unit as read (`# Up = +Y`, `# Unit = 0.01`). Remove
      the # and set them, and they replace the settings and all these checks. A map with only those two lines
      leaves the bones to the finder.
      - `Up` takes +X -X +Y -Y +Z -Z (any case). An `Up` the same as the file's own leaves the clip as it was.
      - `Unit` is metres per file unit: a number alone (or with "m"). "1 cm" is refused and listed in the check line.
      - A `Unit` line alone still lets the up be fixed when the file's own unit is off.
      - Saving the map again keeps the lines that were in use, and a UTF-8 BOM is read.
    - Checked with copies of the user's clip and of Flair and two Blender dance copies with their settings patched
      (put right; right axes with the wrong unit; wrong axes and unit): all six read exactly like their correct
      versions (0.00 deg, 0.0 mm). A map's Up and Unit lines read the same as the automatic result.
  - **Checked offline** (a test program running the tools' PoseClips.cpp, on Faith):
    - **9 rig styles x 2 Mixamo clips** (mixamo.fbx and "Samba Dancing.fbx", renamed and saved by Blender 5.2 with no
      mesh, so no rest pose): all 65 bones found in every copy, the mirror-named copies swapped back. Against the
      originals, relative to the Hips:
      - upper arms, forearms, hands, thighs, Spine2 and the Hips 0.00 deg; Spine and Spine1 0.01 - 0.84;
      - neck 0.10 / 1.97 deg, head 0.71 / 9.93, shins 0.60 - 9.36, collarbones 3.8 - 8.7;
      - fingers 0.05 - 4.5 deg, thumbs up to 15.4, feet and toes 6.6 - 8.8;
      - the Hips' heading 0.00 deg, their place within 1.4 / 2.8 cm.
    - **Clips with a rest pose don't change:** mixamo.fbx and "Samba Dancing.fbx" 0.00 deg against the build before.
      Four of the user's UE5 clips (52 bones each, found by name) read with their skin's bind pose.
    - **UE5 fighting and death clips** saved the same way with no mesh are much worse: thumbs up to 77 deg, the head
      36 - 76, one forearm 30 - 58. They stand far from any rest pose the whole time.
    - **The user's three clips** from the game folder:
      - `exported-model.fbx` ("Hit_Knockback_RM", 16 frames at 24 fps): read with +Y up and 1 m units, standing 0.92
        m high. Faith's trunk, thigh, upper arm, shin and chest front match the clip's own to 0.01 at frames 0 and 10,
        and her Hips' path is the clip's scaled to her height.
      - `Flair.fbx` and `Dancing Twerk.fbx` (Mixamo Without Skin): their default pose is a T-pose, read as before.
  - **Reviewed:** seven rounds, each finding checked by a skeptic before it was fixed:
    - wf_9f1c4551-e65 (the rest pose: 3 reviewers): the left/right swap ran on a no-rest clip's default pose; binds
      weren't followed when the rig moved after binding; the Hips' height when no leg stands; plus 8 smaller ones;
    - wf_a19c70f3-8e5 (a regression check of those): a body skin without finger clusters was dropped; binds followed
      a mesh the animation moves; a leg held up pulled the Hips' height; the floor warning one way only;
    - wf_9047616e-e51, wf_a49aabc4-dd4, wf_29d39494-746 (the up and unit from the body): clips with right settings
      that lie on a side, fall, swim or hang were stood up, then a fall's lying axis was taken, then a spread-out
      lying frame passed as a T-pose (hence the rule above and the bone map's Up line);
    - wf_9bd07548-557 (the bone map override): a Unit line alone hid the wrong unit; saving dropped lines in use; an Up
      like the file's own re-picked the front;
    - wf_8b1adef8-b32 (a check of those fixes): two wrong texts (a file with no axes said "its settings' up"; the menu's help after a re-save), fixed in 43DB727B84D4.
  - **Not known until tested in game:** how the no-rest-pose clips look (their thumbs, feet and head most of all).
  - **Known limits:** with its settings shown wrong, a clip that lies more than it stands on a still floor is stood up
    (set `Up =`); a default pose of another take than the one played could pass as a rest pose (not seen).
  - Files: `Camera/PoseClips.*`, `Camera/PoseEditor.cpp` (the menu's help). Research: section 23 of
    `Research_Pose_Editor_2026-11-08.md` (outside the repo).
- **GAME > Pose > Animation clip: Unity humanoid clips (.anim)** (the user, 2026-11-11: "Can we also make it play .anim
  files? ... high priority", then "Approximate solver", "Like we did with Unreal pretty much").
  - Build 7F4DEE0A428E (15:09, Idle priority; the folder is 2.0.1 (4cdb6de) plus the clips, the converter and this).
    - INSTALLED 15:10 with the game closed (game folder, hash checked twice; usp10.dll left as it was). The user's 36
      .anim clips were copied into `Cinematic Tools\Animations` (copies; the originals stay where they were). **Not tested in game.**
  - **How to use it:** put the .anim file (Unity's binary asset) in the game folder's `Cinematic Tools\Animations` beside
    the .fbx files and pick it under GAME > Pose > Animation clip, as an FBX clip.
  - **What's read:** a humanoid clip holds no bones: Unity's 89 body muscles, the body's place and turn, and each hand's
    and foot's IK goal. Unity's own numbers for turning muscles into bones aren't in the file, so:
    - an approximate model of them (each bone's zero-muscle pose, fitted to the user's 36 clips' own hand and foot goals
      on Mixamo's T-pose; Unity's default muscle limits; Unity's body frame): over the 36, the hands 8 cm, the feet 10 cm
      off their goals and 8 deg turned (rms) before the next step; each clip's report gives its own (hands 4 - 15 cm, feet 4 - 17 cm over the 36);
    - then each arm and leg bent to its goal (two-bone IK about the knee's and elbow's hinge), the hands and feet turned
      as their goals, the body lowered where a foot can't reach, and the feet stood on the floor;
    - the result plays through the FBX clips' fit (Mixamo's 65 bones, that T-pose as the rest).
  - **Checks (offline):** the C++ against the Python prototype on all 36 clips (within 0.000006 deg); all 36 fit onto
    Faith and sample finite values; read in 2 - 114 ms; stick figures of the reference and of Faith look right
    (ThumbsUp, Orange Justice, Eastern Bloc).
  - **Not known until tested in game / limits:** the spine, neck, head, fingers and thumbs have no goal to check the
    model against; on the character (its own proportions) hand contacts can be several cm off and the feet don't stick
    to the ground; text (YAML) .anim files aren't read (only Unity 5.6's binary format was tested on real files); props,
    jaw and eyes aren't played; Unity's root-motion options and Mirror aren't applied; weighted tangents (2018+) play
    unweighted (a warning says so); some hands can't reach their goals in a few frames (up to 8 cm short, the check line
    says so).
  - Reviews (each finding checked by a skeptic; details in research section 24):
    - wf_816bfa10-3ae: 10 fixed (a crafted file could hang the loader for hours; a NaN key reached the game as the
      Hips' place; the floor per foot sank a standing foot under a raised one; 1-frame poses got no floor lift; texts);
    - wf_c1e5a3c9-646 (a check of those): 9 fixed (memory through copied names and leaf lists; an all-broken curve
      played as 0; one bad key time dropped the keys after it; the floor rule's cliff; 1e30 values; texts);
    - wf_227e1a4d-972 (a final check): 4 fixed (long type tree names, a broken body curve's message, scripts' curve
      count, the broken-keys text) plus the editor copy standing in for a partly broken curve; 3 rejected.
  - Files: new `Camera/PoseClipsUnity.cpp`, `Camera/PoseClipsUnityData.h` (generated); changed `Camera/PoseClips.*` (the
    .anim dispatch, Clip::FromUnity, no bone map for those), `Camera/PoseEditor.cpp` (the list, help), the project
    files. Research: section 24 of `Research_Pose_Editor_2026-11-08.md` (outside the repo).
- **GAME > Pose > Animation clip: Faith's hair is put back on her head when a clip makes her pose jump** (the user,
  2026-11-11, on 24 fps clips: "the hair doesn't really follow faith (simulation stops?)").
  - Build 0AE3B15EB722 (11-11 16:40, Idle priority; the folder is 2.0.1 (4cdb6de) plus the clips, the converter, the
    .anim clips and this).
    - INSTALLED 16:41 with the game closed (game folder, hash checked twice). **Not tested in game.**
  - **What the test showed** (a diagnostic "Hair watch" in CT.log, written only with `[Debug] Diagnostics = true`; the
    user ran `exported-model.fbx` and others): the simulation never stops. On every clip her cloths were at full detail,
    not switched off, not being reset, and the game's cloth step ran every 1/30 s. The cloth's root never moved.
    The user's answer to "did calm 24 fps clips look wrong too?": "no it didn't".
  - **The cause, from the watch's numbers:** the pose jumps. `exported-model.fbx` is a 0.62 s knockback that loops; her
    head moved 3.5 - 7.6 m per second and jumped at each loop.
  - **The fix:** at a jump of her pose the tools set the game's own "reset this cloth" flag on her cloths for 120 ms
    (longer in slow motion: divided by the game speed), so the game puts the hair's points back on their skinned
    places. A jump is: the clip's time going back or on by more than 0.25 s, a clip put on, taken off or changed, each
    with her head moved 10 cm or more in that frame; or her head moved 50 cm or more in one frame, whatever did it; or
    letting go of a pose that had a clip on.
    - CT.log says "Hair: her hair reset at a jump of her pose" once, and with Diagnostics on a count every 10 s.
    - The clip's help text says so ("When a clip jumps ... Faith's hair is reset to her head").
  - **The game's code this uses** (read offline; four new byte checks, the fix stays off if one fails): the cloth
    instance's reset byte at +0x18A (set by its own method, vtable 0x141AB6380 slot 12 = 0x142FA39F0); the simulation
    job 0x142FA07A0 calls the reset 0x143B787A0 at 0x142FA0826 when the byte is set; the interpolation 0x142FA28F0
    clears the byte every frame at 0x142FA29C5 (hence the tools set it again each frame for the 120 ms).
  - **Not known until tested in game:** whether the game takes the flag in the frame the tools set it (the watch's
    "distance of the hair's points from their skinned places" line will show it). If it doesn't, the fallback is to
    call the game's reset itself (0x142FA5020 + 0x143B787A0), not built.
  - `[Debug] Diagnostics = true` is still set in the game folder's `Cinematic Tools\config.ini` from that test (a
    backup of the file as it was is kept); it is switched off again after the user's test of this fix.
  - Reviewed: wf_3eff7c87-e85 (the watch: a stale clip flag, window labels, a stale cloth pointer check, placeholders,
    the diagnostics gate, poses stepped aside or refused) and wf_e7026c36-55f (the reset: slow motion's hold, the count's
    wording, the jump at Let go), each finding checked by a skeptic before it was fixed.
  - Files: `Camera/PausedHair.cpp/.h` (KeepReset, Watch, the four checks), `Camera/PoseEditor.cpp` (the jump rule in
    Update, the help text). Research: section 25 of `Research_Pose_Editor_2026-11-08.md` (outside the repo).
- **GAME > Pose is now its own window, with bone keyframes and a timeline** (the user, 2026-11-11: "Make the pose
  stuff a new window ( window can be enabled by clicking a button in game session)", "In the Pose window UX needs to be
  proper", "bones should be keyframed and let the player keyframe each bone etc and the keyframes can be played with
  free camera playback (or not) and/or the already loaded animation file (fbx/.anim)"; then "After planning send this
  to deepseek for code writting").
  - Build 36923B81816C (11-12 13:13, Idle priority, no warnings; the folder is 2.0.1 (4cdb6de) plus the clips, the
    converter, the .anim clips, the hair reset and this).
    - INSTALLED 13:13 with the game closed (game folder, hash checked twice; usp10.dll left as it was).
      **Not tested in game: nothing of the window or the keyframes has been seen on screen yet.**
    - **First run in the game (the user, 11-12 13:24 - 13:28: "It crashed find out why"): the game ended the first time
      the timeline was drawn,** that is right after the first keyframe was made. Fixed in **92A6DABA27C6** (built 13:32 at
      Idle priority, no warnings; INSTALLED 13:32 with the game closed, hash checked twice). Not tested in game since.
      - What Windows recorded: MirrorsEdgeCatalyst.exe ended in CT_MirrorsEdge.dll with code 0xc0000409 (a "fail fast":
        the C runtime's abort), and a crash dump. From the dump, with the build's PDB: abort, called by ImGui's check
        of a window's stacks (`CheckStacksSize`, imgui.cpp 5232: "PushStyleVar/PopStyleVar Mismatch!") at the EndChild of
        the timeline's rows (`DrawKeyTimeline`, PoseEditor.cpp 6852), from `DrawKeyframes`.
      - The cause: the rows' child was opened as `PushStyleVar(WindowPadding, 0); BeginChild(...); PopStyleVar();`. That
        is the usual way in today's ImGui, but the tools' ImGui 1.61 wants the style stack the same at a window's Begin and
        End, and its checks are compiled into the release build: a failed one ends the game. The push was not even needed
        (a child without a border has no padding there). The two lines are gone.
      - Why four review rounds missed it: they checked that every push has its pop, not that a window's Begin and End see
        the same stack. Nothing had been run.
      - **So the window's code is now run offline** (a test program, the pose window harness):
        PoseEditor.cpp itself, the keyframe module and the tools' own ImGui 1.61 with its checks, a made-up held pose and
        a simulated mouse; what the pose editor calls of the game is stubbed. It draws every tab with and without
        keyframes and with a clip, makes keyframes, clicks, drags and scrubs on the timeline, opens the right-click menus
        and picks their rows, plays on each clock, loads and saves, draws 24,000 keyframes, resizes the window, lets go of
        a pose, then 60,000 frames of random mouse input. On the build that crashed it stops at the same check with the
        same message. On the fixed code: 10 runs (text 11 - 34 px, screens 1280x720 to 3840x2160, the user's own 29 px on
        3840x2160 among them), 77 checks each, about 1.1 million frames, no failed check and none of ImGui's.
      - What it does not run: Take pose and Let go against the game, the dots and rings on the picture (they need the
        game's poses and camera), a real clip's pose, and how any of it looks.
      - With very large text on a small screen (24 px on 1280x720) the window is shorter than its content: the timeline
        is then below what shows until the window is scrolled.
      - CT.log has nothing of that session (it ends one second after the start, in the offset scan). Most likely the
        game started twice in a row, as it does through Steam (another tool's log names process 29168 at 13:24:57.1;
        the process that crashed was 30520, started 13:24:58.0), and the second one could not open the log the first
        still held. Confirmed from the dump and fixed: see the CT.log entry below (D5A5E10303C2).
  - **Who wrote what:** the plan, the checks and the review rounds are Claude's; the code was written by DeepSeek
    (through the DeepSeek harness's delegate tool, one conversation) from that plan, as the user asked. Steps 1 and 2
    (the module, the keys in the editor) and every correction round were read line by line; of steps 3 and 4 the
    keyframe rows, the window's frame, the timeline and the menu were read, and the reviewers read all of them.
  - **Where it is:** GAME tab > Pose > "Open the Pose window...". The GAME tab keeps only that button and a status
    line. The window is not saved as open: it starts closed each time.
  - **The window, top to bottom:**
    - **Who:** the "Near the camera" list, Take pose, Let go, Let go of all, and the status lines of the pose being
      edited. With no pose being edited the rest of the window isn't drawn.
    - **The bones** (left: the find field and the bone list, a "*" on an edited bone and a "K" on one with keyframes) and
      four tabs: **Bone** (the picked bone's, face group's or weapon's sliders, Reset, Undo), **Clip** (the animation
      clip block as it was), **Files** (saved poses; saved keyframes), **Options** (the display and drag options;
      the keyframe options).
    - **Keyframes:** the time buttons, Play / Pause, Time, Length, Loop; "Keyframe: the changes (N) / this bone / the
      whole pose"; Remove keyframe, Undo keyframes (N), Clock; and the timeline.
    - Every control and help text of the old Pose section is in the window. The bones' dots, rings and names on the
      picture are not drawn over the window.
  - **Keyframes, how to use them:**
    - Pose a bone, press "Keyframe: the changes". Move the time, pose again: with "Changes are keyframed at once"
      (Options, on by default) every change after the first keyframe is kept as a keyframe at the timeline's time.
      Press Play.
    - A keyframe holds one bone's turn and move at a time. Each bone has its own keyframes; a bone without keyframes
      keeps its edit all the time.
    - The first keyframe of a bone made in a pose that already has keyframes also keeps that bone as it was at the
      keyframe times before and after, so the change belongs to its own time only.
    - **To the next keyframe** a bone goes Smooth (a curve through the neighbouring keyframes; the default), Linear,
      Ease in and out, or Hold (it jumps at the next one). Set it in the Bone tab or the keyframe's right-click menu.
    - **Clock:** Game time (follows Game speed, stops with Freeze time), Real time (plays with time frozen too), the
      camera track's time (the keyframes follow the free camera's track as it plays; "Begins at" in Options says where
      on the track they start), or the animation clip's time (the keyframes follow the loaded .fbx / .anim clip, on top
      of it).
    - **The timeline:** one row per bone with keyframes and an "All bones" row. Click a keyframe to go to it and pick
      its bone; drag it to move it in time (the keyframes it passes are untouched, one it is dropped on is replaced);
      click or drag on the ruler to set the time (it snaps to a nearby keyframe, else to 0.05 s with "Times snap to
      0.05 s"); right click a keyframe for Delete, the four eases and "Copy to the timeline's time"; right click a
      bone's name for "Remove this bone's keyframes".
    - **Undo keyframes** takes back the last change of the keyframes (50 steps). The pose's own Undo stays in the Bone
      tab.
    - **Files:** keyframes are saved by name as `Cinematic Tools\Poses\<name>.posekeys` (text) and loaded onto the pose
      being edited, bones by name. Saved over a pose taken, they carry that pose with them, so they load onto a pose
      taken later; saved over a clip, they are edits over that clip.
    - **The work is kept:** letting go of a pose with keyframes (also a respawn or a level load) saves them as
      "autosave <who>" first, and Let go asks for a second click. While you work, the same file is written again at
      most every 30 s after a change (with the window open and nothing being dragged or typed), so a crash or closing
      the game with a pose still held loses little. A file of that name from an earlier pose or session is kept once
      as "autosave <who> before".
  - **Saved in config.ini,** new section `[Pose]`: ShowBones, ShowFace, GroupFace, ShowFingers, ShowHelpers, DragBends,
    StopAtWalls, RingsOnBone, AutoKey, SnapKeys (these options were not saved before).
  - **Known limits:**
    - Weapons aren't keyframed (they go with the posed bones as before).
    - The keyframes go when the pose is let go (they are autosaved, see above; load the autosave file onto a pose
      taken again to go on).
    - The camera track's clock gives no time after a track ends: the keyframes hold their place then.
    - With a clip's Hips on "Follow the clip's travel", keyframed Hips turns shift the travel at every change of
      heading, so a loop or a scrub doesn't come back to the same place.
    - The timeline has no zoom: a wider window or a shorter Length gives detail, the Time field exact times.
    - Thousands of keyframes in view are thinned out on the timeline (drawn only where they don't overlap) so the menu's
      draw limit can't be passed.
  - **Checks (all offline; none in the game):**
    - The keyframe module (`Camera/PoseKeys.*`: the data, the curve, the clocks, the file) builds on its own. Its
      author's test: 11,523 checks pass. A second test written by an independent agent from the plan alone, without
      seeing the code's test: 4,287,017 checks; its first run found 1,824 failures in 19 groups (numbers written with
      the process's locale; times losing precision past 1000 s; files the saver wrote and the loader refused; an
      endless Length; indented lines skipped silently), all fixed, then 0 failures.
    - Reviews, each finding checked by a second reviewer before it was sent back:
      - wf_e6d0544c-586: the plan itself, by three readers (code, UX, spec), before any code;
      - wf_c12c694e-c05: steps 1 - 3 by four reviewers: 25 findings, 20 confirmed and 5 partly, 18 different fixes
        (the track clock could stop following for good; a row-breaking helper that never broke rows; the Time field
        moving off a keyframe on a click; the clip clock's Play button; messages nobody could see; and smaller ones);
      - wf_c2a95f54-422: the timeline by two reviewers: 11 findings, 8 confirmed, 2 partly, 1 rejected (a keyframe
        could not be dragged at all and scrubbing stopped after one frame; a keyframe at the end drawn as a half; the
        rows jumping under the mouse; a draw limit that thousands of keyframes would have passed);
      - wf_81130609-e41: a last check of the 26 corrections by two reviewers: nothing wrong in the timeline's; 2 small
        points in the others (the Time field took the first, not the nearest, of two keyframes within 0.01 s; the
        Time field locked before the first keyframe when the clip follows the camera track), both fixed.
    - The autosave every 30 s and those last two fixes came after the reviews: read line by line, not seen by a
      second reviewer.
    - A full build at Idle priority after the last fix: no warnings.
  - Files: new `Camera/PoseKeys.h`, `Camera/PoseKeys.cpp`; changed `Camera/PoseEditor.cpp` (6,230 to 8,600 lines),
    `Camera/PoseEditor.h`, `UI.cpp` (the window drawn after the Hotkeys window), `Main.cpp` (the `[Pose]` section), the
    project files. Research: section 26 of `Research_Pose_Editor_2026-11-08.md` (outside the repo).
- **CT.log: a session can no longer run without a log, and the ends the crash lines missed are written** (the user,
  2026-11-12, after the Pose window's crash left nothing in CT.log: "Yes, fix it").
  - Build D5A5E10303C2 (11-12 14:40, Idle priority, no warnings; the folder is 2.0.1 (4cdb6de) plus the clips, the
    converter, the .anim clips, the hair reset, the Pose window and this).
    - INSTALLED 14:40 with the game closed (game folder, hash checked twice; usp10.dll left as it was).
      **Not tested in game.**
  - **What had happened** (from Windows' crash dump of that session): in the game process that crashed, the log's file
    was never open (its file pointer was null). The game had been started twice within a second: another tool's log
    names process 29168 at 13:24:57.1, the one that ran and crashed was 30520, started 13:24:58.0. Both starts load
    the tools. The first made CT.log and still held it when the second tried to open it. So the session that mattered
    wrote no log at all, and CT.log held the first start's 33 lines.
  - **The log's file now:**
    - A log that can't be opened is tried again when later lines come (at most every 0.1 s). The lines until then are
      kept in memory (up to 1 MB) and written first, after a line that says the log was opened late and why. Nothing
      waits: the tools' start is not held up.
    - If CT.log is still held after 3 s (another game really running beside this one), this process writes
      `CT.<its process id>.log` instead, and says so in its first line.
    - A crash line or the exit lines open a file at once if none is open yet.
    - Before every open of CT.log (opening empties it) the log that is there is kept as CT.previous.log, unless it is a
      first start's few lines: written within the last 5 seconds and under 8 KB (a session's log passes 8 KB about 9 s
      in, just before crash lines can first be written). The old code moved whatever was there, so in a double start
      whose first process had already gone the first start's lines went over the real last session's log (read from
      the code, not seen happen).
    - Still lost: the log of a session that ended in its first seconds and was started again within 5 s.
    - A single start writes the same log as before.
  - **Ends that left no crash lines.** The crash lines are written by an exception filter. Three ways the tools' own
    C runtime ends the game are no exception a filter sees (Windows' "fail fast", code 0xC0000409): a failed check
    inside ImGui (its checks are compiled into the release build), abort() (also an uncaught C++ exception), and a call
    the runtime refuses (a text longer than its buffer in sprintf_s, strcpy_s and the like). Each now writes first:
    `[CRASH] ...` with what it was (for ImGui: the check's own text and line), the call chain as module+offset, and the
    tools' state. Then the game ends exactly as before (same code, Windows' dump still made).
    - A failed ImGui check shows the C runtime's Abort / Retry / Ignore box, as it always did here. Abort ends the
      game. After Ignore the game goes on: the log then says so, and a later failure is written again.
    - The abort and refused-call hooks are set at the very start of the tools (they belong to the tools' own runtime;
      the game has another), so the start-up is covered too.
  - **Checks (offline):**
    - the log module itself in a test program, 10 situations, 32 checks: an old log kept as CT.previous.log; a first
      start's lines not moved over it; lines before the start-up dropped as before; the file held for 0.6 s (the note,
      then every line in order); held for good (the process's own file after 3 s); a crash line while held; no folder;
      more lines than are kept (counted, a long line cut); a game still closing that held its whole log (kept, not
      emptied); a quick restart (kept);
    - the C runtime's behaviour with the same hooks, built like the tools, 8 cases: a failed check, abort(), sprintf_s
      and strcpy_s overflowing, an uncaught C++ exception, and each of three ends after an ignored check: every one
      writes its note and the process still ends with 0xC0000409;
    - the Pose window's offline run with the new ImGui hook: stops at the same check on the crashing source, passes on
      the fixed one (5 runs, 77 checks each).
    - Review wf_b400f6e8-75e (two reviewers, each finding checked by a second): 5 findings, 4 confirmed and fixed
      (a late open could empty the log of a game that was still closing; the 5 s rule alone took a quickly restarted
      session's log for a first start's lines; after Ignore nothing more was written; the two hooks came too late to
      cover the start-up), 1 partly and left: two threads failing within about a millisecond of each other can end the
      game before the first has written its call chain (its first line is there; Windows' dump has the rest).
  - **Not known until it happens in the game:** whether the game starts twice at every launch here or only sometimes
    (seen once, in one dump); how the call chain reads in a real failure; the Abort / Retry / Ignore box itself (in
    full screen it may sit behind the game).
  - Files: `Util/Log.cpp`, `Main.cpp` (NoteRuntimeEnd, the two hooks, CtImGuiCheckFailed), `imgui/imconfig.h`
    (IM_ASSERT). The full build recompiled everything and showed old warnings at 22 places in `Input/InputSystem.cpp`
    (unchanged since 10-20; float conversions), none in these files.

- **GAME > Pose window > Voice: a sound file played on a posed character, its mouth moving with it** (the user,
  2026-11-12: "put a voice file (mp3/wav etc.) and sync it to faiths mouth or ksec mouth if it's possible implement
  it"). In 36F6C4538534 (installed 2026-11-12 19:16, **not tested in game; nothing was heard on the PC during the work**).
  - **What it does.** A fifth tab, Voice, between Clip and Files. It lists the sound files of the game folder's
    `Cinematic Tools\Voices`; a picked file is read on a thread of its own and put on the held character.
    - The mouth: the jaw, six lip bones and the two mouth corners move with the sound, as one more layer on the pose
      after the edits, the keyframes and the idle movement. Those bones keep the user's own edits and keyframes
      underneath, and nothing of the voice is saved into a pose or keyframes file.
    - The sound: played by Windows (XAudio2, an engine of the tools' own beside the game's), in step with the voice's
      time. Not placed in the world, not scaled by the game's volume, heard only while the game's window is in front.
    - Three clocks: its own (real time: Play / Pause / From the start / Loop, a picture of where the mouth opens that
      can be dragged, a Time slider), the keyframes' time or the camera track's time (with "Begins at").
    - Sliders: Volume, Mouth opening, Lip shapes (starts at half), Lip closing, Mouth ahead by (0.03 s; the pose made
      now is seen a frame or two later, the sound is aimed at when it is heard). "The mouth now" shows the five
      shapes. "The sound is heard" off: only the mouth moves.
  - **How.**
    - Decoding (`VoiceDecode`): Windows' own Media Foundation, loaded when first needed. Read on the test PC: WAV in
      its layouts, MP3, MP2, AAC in M4A / MP4 / ADTS, ALAC, FLAC, WMA, AC-3, Opus in WebM, Vorbis in MKA, a video's
      sound track; Ogg Vorbis / Opus only with Windows' Web Media Extensions; AIFF not. Encoder lead-in silence is
      dropped where the file says how much (tagged MP3 and M4A then start 0 samples off the source). Up to 10 minutes
      are kept. Each block is waited for 15 s at most: Windows' AAC decoder was seen spinning for ever on some damaged
      M4A files (7 of 600 damaged copies); such a decoder keeps a processor core busy until the game closes.
    - The mouth's shapes (`VoiceMouth`): worked out from the sound alone (no word recognition, no libraries): five
      curves a hundredth of a second, Open, Wide, Round, Closed, Teeth, from band levels, voicing and gaps.
    - The face (`VoiceFace`): the face's own axes from where the bones are (corner to corner, chin to nose), the jaw
      turned about the real hinge (9 degrees at full opening: the lower lip 15 - 16 mm down), the lips and corners
      moved by shares of the mouth's width. The same 57 face bones are on Faith, KrugerSec, men and women.
    - The player (`VoicePlayer`): each frame it is told where the sound should be and at what pace; small errors are
      taken out by the playing speed, errors over 80 ms by starting again from the right place; volume goes to 0 for
      a pass before any stop. The pitch goes with the pace; from about a quarter of the recording's pace to twice it,
      silent outside (the tab says so when that happens).
  - **Measured (offline, on computer-made voices only: David, Zira, Mark, a Greek voice; no recording of a person):**
    - against Windows' own viseme timelines, on files never tuned on: the jaw 0.78 - 0.80 correlation, one opening a
      syllable (0.98 jaw peaks per vowel), shut in every pause, 10 - 15 ms early; wide 0.69, teeth (s, sh) 0.78;
      rounded 0.35 - 0.46 (a forward "oo" can read as "ee", an l as rounded); p / b / m only hinted (the hint is
      sure of a third to a half; with the jaw, the mouth is seen shut on 81 - 91 % of them);
    - music at 10 dB under the voice moves the mouth in 22 % of the pause frames; a clipped voice kept its jaw but lost
      part of its closures and lip shapes; a made-up whisper opened the jaw about half as far;
    - the player, by XAudio2's own count of the samples that left the voice: within 3 ms of the clock in steady play,
      at the right place 60 - 94 ms after a start or a jump, silent 57 - 63 ms after a pause. Its output delay is
      what XAudio2 reports (40 ms here): not checked by ear or microphone.
  - **First run in the game** (the user's, 11-12 19:28, CT.log): a real recording (a 30.6 s MP3, 22050 Hz mono) was
    read, its mouth analysed (the mouth moves in 78 % of it), put on Faith (her mouth's bones found) and the player
    opened Windows' audio (XAudio2, 2 speakers, 48000 Hz). How it looked and sounded: not reported; the user then
    asked for improvements and chose blinks, brows and head movement (the next entry when built).
  - **Limits.** A character a replay records or plays can't be posed, so its mouth can't move this way (the sound
    still plays). The file must be the voice alone. A loop's wrap loses the first 60 - 70 ms. Letters other than
    Western European and Greek show as "?" in the list (the file still loads); a name with "##" in it is shown cut.
    Not tried on Windows 10 or on a PC without the Web Media Extensions.
  - **Checks (offline):** the face maths on five rest poses (140 checks); the real path file -> decode -> shapes on 17
    files of 12 kinds (230 checks); each of the three researched modules merged with proof it is the same code
    (decode: identical compiled code and 200 files' results; mouth: 223 output files byte-identical; player: token
    diff tied to the four asked changes, its own tests rerun); the Pose window's harness with the tab, the clocks,
    the layer on a made-up body through the real hand-over (170 checks at three sizes).
  - **Review** wf_1bab6756-e2f (four reviewers, each batch re-tested by a second): fixed: on the camera track's clock
    the pace told to the player was measured across two different frames (the track moves after the pose editor's
    update), so it jumped at uneven frames and after a hitch; a voice put on while its clock ran at another pace
    crept to it over 0.75 s; no word in the menu when the pace is outside what the player plays; two warning lines
    that each said the other half worked; "Teeth (f, s)" (f is mostly missed), "1 min 60.0 s"; the "took the pose"
    count ran while a voice moved the jaw; the help left out that the sound goes on while a pose steps aside.
  - Files: `Camera/Voice.h/.cpp`, `VoiceDecode.h/.cpp`, `VoiceMouth.h/.cpp`, `VoicePlayer.h/.cpp`, `VoiceFace.h/.cpp`
    (new), `Camera/PoseEditor.cpp` (VoicePlay, KeepVoice, AddVoice, the load job, the tab), `UI.cpp` (Greek letters
    in the menu's font), the project files. Research: `Research_Pose_Editor_2026-11-08.md` section 28.

- **GAME > Pose: every skeleton: "skeleton not known: can't be posed" fixed at its cause** (the user, 2026-11-12:
  "find a way to find every skeleton and you can decompile all the necessery systems to find them so we can fix the
  'Skeleton not know: can't be posed' issue"). In 36F6C4538534 (installed 2026-11-12 19:16, **not tested in game: the cause
  is from the game's code and data, the game was closed during the work**).
  - **The cause (decompiled, two independent readings).** When the game makes a body's pose manager it copies the
    skeleton's parent list and writes "no parent" for every bone the body's animation rig has no channel for
    (0x142FC9197). The pose editor compared that list exactly with its tables, so such a body never matched a
    skeleton that was in the tables. By the game's data the only character bones in no rig are bone 161 of the men's
    skeleton (BlendshapeDriver) and of the women in heels' (FacePosesAnimation): every man and every woman in heels.
    It fits the record: only Faith, women on the other skeleton and KrugerSec were ever held.
  - **What changed.**
    - A body's skeleton is read from the game itself (`Camera/PoseSkeletonLive`): its own parent list (now also
      used for the pose, as the game's pose pass does), which bones have a rig channel, and the SkeletonAsset its pose
      manager was made from (manager + 0x00) with its name and its bones' names. The names are taken only when
      proven: the asset's vtable, as many names as bones, every name hashing (FNV-1) to the hash the game keeps
      beside it. If a check fails the tables are used alone and CT.log says which check failed.
    - A bone without a rig channel is never handed to the game: the game's blend would write its pose into the first
      channel, another bone's (decompiled: 0x142FCF743, 0x142FC921E). Such a bone and what hangs on it are left out
      of the pose, and the log names them.
    - The tables hold every skeleton of the game's data now: 22 assets found by type over all 43,704 data files
      (patched ones included), 18 different bone layouts (the first 9 unchanged; new: weapons, pigeon, rat, animated
      props, the mechanical arm, ropes, three trees). An asset the tables don't have gets a skeleton made from the
      game's own names.
    - Listed too: the plain actors with a body (by the data: bear mascots, pigeons, drones without AI).
    - Still refused, with a reason in the list: a skeleton whose movable bones don't hang on one bone (animated
      props, ropes), and a body whose main bone the game doesn't animate.
    - Pose and keyframes files carry the body's own asset name (a man's file is no longer a woman in heels' "own
      skeleton": their bones are other lengths).
    - CT.log, once a pose manager: "Pose: a body's skeleton, N bones ...: the game's asset ..., found as ..., bones
      with no parent ...". A body that stays unknown gets its whole parent list written.
  - **Checks (offline):** the reader against made-up memory laid out as the decompile says, with every way the asset
    can be wrong or gone (258 checks); the pose editor's real Take, per-frame hand-over and Let go on made-up bodies
    (a man, a woman in heels, a KrugerSec without ankle channels, a body with no readable asset, a prop, a skeleton
    the tables don't have): the made-up game took every frame, and no bone ever went through another bone's channel
    (about 30,000 pose passes a run).
  - **Review** (the same workflow): fixed: Take pose ended the game for a made skeleton whose label was 64
    characters or more (none of the game's own); a man's file loaded on a woman in heels gave her his bone lengths;
    a weapon could be offered a grip bone the pose doesn't hold; the skeleton log was keyed by address only.
  - **First run in the game** (the user's, 11-12 19:25, CT.log): Faith's skeleton was read from the game as
    decompiled: "the game's asset Characters/Skeletons/Skeleton_Faith, its 182 bone names proven by the game's own
    hashes, the body's data names the same asset; found as: Faith; bones with no parent: 0; rig channels 184", and
    she was held (174 bones from the Hips, her box pose too). So the asset, its names, the hashes and the channel
    lists are where the code reads them.
  - **Still not known:** that men and women in heels are the bodies that showed the message (no man was near in that
    run: the first one seen writes the line that says so); the plain actors' list; the new tables' labels (working
    names from the data's file names).
  - Files: `Camera/PoseSkeletonLive.h/.cpp` (new), `Camera/PoseSkeletons.h` (regenerated), `Camera/PoseEditor.cpp`
    (SkeletonOf, LogSkeleton, Take, WalkDrones, the list's notes). Research: section 29.

- **GAME > Pose window > Voice: blinks, brow raises and head movement from the voice** (the user, 2026-11-12, of seven
  suggestions for better lip sync: "Let's do number 4 for now": blinks, brows that lift on stressed words, small head
  nods, all taken from loudness and pitch). In 1647CAFCA8A9 (installed 2026-11-13 01:21 with the game closed, hash
  checked twice; **not tested in game; nothing was heard on the PC during the work; nobody has seen it on a face**;
  not committed).
  - **What it does.** A tick in the Voice tab, "Blinks, brows and head" (on), with three sliders:
    - Blinks (0 - 1): how far the eyes close. The upper lid goes down in 0.08 s and up in 0.17 s; blinks come every
      2 to 6 s, now and then two in a row, drawn to the ends of phrases, to the pause before a phrase and to strong
      nods.
    - Brows (0 - 2): a raise on the syllables that stand out most by their pitch (3 mm at the brow's middle at 1),
      2 s apart at least.
    - Head (0 - 2): a small nod on the syllables that stand out by loudness and length (up to 3.5 degrees), a chin dip
      into the end of a phrase, a slow drift with the voice's pitch and loudness, a small new turn and tilt for a
      phrase (about 2 degrees at most); 30 % of it on the upper neck (Neck1).
    - The tab says what was found in the sound ("8 blinks, 4 brow raises, ...") and when to doubt it (no speech found:
      blinks only; noise, music or a hum under the voice); "The face now" shows what the face does this moment.
    - Worked out once when the file is read, so it stays in step when the voice is scrubbed, looped or slowed, and is
      the same every time for the same file. The user's own edits and keyframes of the lids, brows, head and neck stay
      underneath.
  - **How.**
    - The analysis (`VoiceLife`, no libraries): a loudness track and a pitch tracker of its own (60 - 400 Hz),
      syllables and phrases, a stress score per syllable against its neighbours (level, pitch over the running pitch,
      rise, length). Brow raises lean on pitch, nods on loudness and length. Silence, noise, tones, held notes,
      beats, a murmur of many voices and the made-up whispers tried get blinks only.
    - The face (`VoiceLifeFace`), sized from the game's own animation in the tools' saved replays (Faith's face never
      moves there; 20 men and KrugerSec have face bones, five blinks were found): the game's blink is the upper lid
      alone, one turn of its skin about a line 4 - 7 mm behind the eye bone; the lower lid does not rise. The head's own
      axes are taken from the eyes and HeadEnd (the mouth's lean 23 - 38 degrees forward of them).
    - Each frame the bones are found again on the pose as it is then, so a lid the user has moved part closed is shut
      from where it stands, a lid turned part shut (people's lid bones at the eye's middle) only the rest of the way,
      and the nod goes about the head's own right as the edits, keyframes or a clip turned it. A Face bone turned by
      the user changes nothing of it. A part (eyelids, eyebrows, head) is on only if every one of its bones is held
      (no wink).
  - **Measured (offline):** the user's recording (30.6 s): 8 blinks, 4 clear brow raises (the strongest at 17.3 s),
    nods up to 2.7 degrees, turns up to 2.2. The blink put through three recorded people's own skins lands 0.5 - 0.7 mm
    (mean) from the game's own blink. Pitch: 97.5 % and 94.3 % of voiced frames within 5 % of two other estimators on
    the user's recording; 99.5 - 100 % on made-up voices with a known pitch. 41,860 hostile edge runs of the analysis:
    no fault, every value in range.
  - **Limits.** It hears stress, not words. Music, noise or a hum 10 dB under the voice puts about a third of the
    brow raises and nods elsewhere; singing is taken for speech; a voice above 400 Hz is tracked an octave down.
    A pose taken with the eyes shut blinks from there as if open (set Blinks to 0); lid bones on the lid's edge
    (Faith's) that are turned, not moved, get the whole blink on top (whether such a turn closes the lid is not known). Faith's own skin and animated face
    are in no recording: her blink was checked on her bones only (her right lid goes a little further than her left:
    39.5 / 34.1 degrees, as her bones stand).
  - **Checks (offline):** the face module's own test on the five rest poses, recorded poses and every skeleton (507
    checks; the merged module gives the same output line for line outside the new lid band); the real path file ->
    decode -> mouth and life on 17 files of 12 kinds and the user's recording (445 checks); the Pose window's harness
    with a made-up voice through the real KeepVoice and hand-over (228 checks at three sizes: blink, brows, head,
    the amounts, lids moved or turned part shut by edits, lids drawn through the 6.5 - 9.5 mm band, the head and the
    Face bone turned by edits, a man, a lid bone not held, a faceless skeleton, a face edited out of shape).
  - **Review** wf_4b66c1c7-723 (three reviewers, each re-checked by a skeptic; about 690,000 hostile frames, 24
    hostile sound files, all 18 skeletons): no crash, nothing not a number reached a bone, no lid past 60 degrees.
    Fixed: a Face bone turned by the user sent the brows and the nod off the face's axes and from 40 - 60 degrees
    dropped the blink and the head; lids at the eye's middle already turned shut were turned 35 degrees more; the lid
    jumped 6 - 12 degrees in one frame where its bones cross 8 mm from the eye (now mixed over 6.5 - 9.5 mm); "The
    face now" ignored the sliders; "its head bone" was said of characters without face bones; the noise note came up
    on clean lines with no pause in them (now says so); the help's blink rhythm and what it promised; "1 blinks";
    head poses named that don't show; an empty field in a log line; "The sound is heard" help still said only the
    mouth moves.
  - **Fix check** wf_8611e268-ef9 (two checkers): every finding's own evidence re-run against the fixed code (all
    nine changes hold) and the new code attacked. Found and fixed: the lower lid could still move 1.4 mm in one frame
    in two cases between the arrangements; the band's 9.5 mm end was one float short of exact; "The face now" kept
    its lines with the voice off, showed a whole blink where lids already turned shut take none, and dropped parts
    the edited pose gives no directions for without a word (now a line says so); "-0.0" in the head line. After
    them the checker's scan (11 bodies, about 52,000 lid poses) gives every offset outside the band bit for bit the
    checked module's.
  - Files: `Camera/VoiceLife.h/.cpp`, `Camera/VoiceLifeFace.h/.cpp` (new), `Camera/Voice.h/.cpp` (the analysis in
    Load, LifeAt, the tab's texts), `Camera/VoiceFace.h` (MaxDriven 32), `Camera/PoseEditor.cpp` (VoicePlay,
    KeepVoiceLoad, KeepVoice, the tab), the project files. Research: section 30.

- **GAME > Pose window > Voice: a living face (eyes, varied blinks, breathing, a head that never stops) and a feeling
  made of the game's own face poses** (the user, 2026-11-13, after trying the blinks, brows and head: "ok it works but
  still needs more human expression now it looks more like a animatronic. We need more feeling no mistakes"). In
  935727C51F4B (installed 2026-11-13 18:56 with the game closed, hash checked twice, over D9CA6F54AFEF, the Time of day session's build installed at 16:03: built from the same folder, it also holds that session's reflection fixes A and B and B3846F99D215, untested in game and uncommitted; **not tested in game; nothing was heard on the PC during the work; nobody has seen it on a
  face**; not committed).
  - **What it does.** "Blinks, brows and head" is now "A living face" (on):
    - Eyes: they look at the listener (where the eyes are posed) with small movements about the listener's face, look
      away at the start of phrases and now and then inside long ones (about two fifths of the speaking time on the
      recording), are back before a line ends, and dip a little in a blink. While the voice moves the head they keep
      looking at the same place ("Eyes hold their target", on). The upper lids follow the eyes' movements up and down
      (eyes the user turns are left with the user's own lids).
    - Blinks: each eye its own, each blink different (part way, long, two in a row), about 20 - 30 a minute while
      speaking. Brows: each side its own, varied raises, small flickers, a slow movement that never stops, drawn
      together where the voice thinks or stresses a word loudly and low. Breathing: a breath in before each group of
      words (the lips part a little), slow breathing in silence, a small lift of the head. Head: a slow drift that
      never stops on top of the nods.
    - New sliders: Eyes (0 - 1), Looks away (0 - 2), Take (1 - 99: another take of the same line; two characters given
      one file start on different takes).
    - "Feeling" (None by default): Calm, Happy, Tender, Sad, Worried, Afraid, Surprised, Angry, Tense, Disgusted,
      Contempt, with Strength (0 - 2), Follows the voice (0 - 1, 0.3) and Uneven (0 - 0.3). Made of the game's own face
      poses (decoded from its animation bank: Faith's library for Faith and the woman's skeleton, Gabriel's for
      everyone else) as FACS action units; the voice only sets how strongly it shows. A new feeling comes in over
      0.6 s.
  - **How.** `VoiceAlive` (the research's checked prototype, renamed), `VoiceFeel` / `VoiceFeelFace` /
    `VoiceFeelTables` (the checked feeling prototype, its parts split and the life's blink handed in), `VoiceGaze`
    (new: the eyes' counter-turn worked out from the turns the layer lays on Neck1 and Head), `VoiceLifeFace` (each
    side's blink and brows, the lid's follow), `VoiceFace` (MaxDriven 64), `Voice` (the analysis' data kept for each
    character's take and the feeling's energy). The middle-lid cap of the blink reads the lid's turn against the
    game's bind, so a pose taken mid-blink there isn't shut 35 degrees further.
  - **Measured (offline):** the user's recording, take 0: 12 blinks (23.1 a minute talking), 8 looks away (37 % of
    the speaking time), 52 small eye movements; on the voices tried the head nods up to about 4 degrees down and 2 up,
    turns up to about 5. The eyes hold their place within 0.0001 degrees against the layer's own head at the Head
    amounts 0 - 2 (with the feeling's nod and tilt, and with the Face bone turned by the user); at most 54 bones in the
    layer on every body.
  - **Limits.** Not seen on a face. The feeling is one per voice (not keyed on the timeline). Faith's own smile pose
    turns her mouth corners 41 degrees (others' about 12). Women's faces were not tried (Faith's library is used for
    them). The target is taken as far away (a near one is missed by 1 - 2 degrees while the head turns). The feeling's
    lid poses on lid bones at the eye's middle (people) are converted, not the game's own middle-lid poses.
  - **Checks (offline):** the alive module's own test (364,222 checks, byte-identical curves); the feeling's (8,500);
    the life face module's (507, line for line); the eyes and the whole order on 11 bodies (1,776); the real path on
    17 files and the user's recording (700); the Pose window's harness through the real KeepVoice (270 at three
    sizes); the menu checker's harness with ImGui's IDs hooked (329 at three sizes).
  - **Review** wf_f756d95b-0a5 (wiring, modules, menu and texts; each re-checked by a skeptic who reproduced every
    finding). Confirmed and fixed: the eyes didn't hold their target with the Face bone turned by the user (1 - 9
    degrees off: the head's counter-turn was taken in the Face's taken frame); the lids followed the user's own eye
    edits on top of the user's own lid edits (a hand-posed look down went 86 % shut) and so changed the old path for a
    sound without a take (now the lids follow only what the voice does to the eyes); the lid's upward follow opened
    nearly shut eyes (a pose taken mid-blink from 2 % to 41 - 47 % open) and snapped shut 14.8 degrees as a lid closed
    (now cut at 0.8 of the way left too); eyes near the 35-degree limit lost the head's counter-turn (up to 7.6 degrees
    off) and two eyes posed at different side angles ended 4 - 7 degrees apart in height (now the look is brought back
    onto the limit by its side angle only, its height kept: 0.98 degrees off where the limit needs 0.96, the eyes
    within 0.5 degrees); a feeling held back the voice's brow raises even at Strength 0 (now by its strength); the
    tab gave two blink counts; the Feeling list sat 2.5 - 3.4 panel heights down (the face part now comes before the
    sound's and the mouth's sliders, the living face's sliders folded); texts: women in heels take Gabriel's poses,
    a feeling's own head posture and its hold on the brow raises, one feeling per voice, 41 against 16 degrees, two
    fifths of the speaking time looking away, the no-speech note, the "Eyes" line now shows what was laid.
    Fix check wf_81033aff-09e (three checkers re-running each finding's own programs on the fixed code): all held but
    the eye limit's first cure, which had its own fault (near the limit in height eyes posed up flicked 5 - 6 degrees
    in one frame and converged eyes went parallel); now the height is kept only well inside the limit and mixed
    smoothly into the nearest point on it (every case of the checker's program at gain 1); and a wide-eyed feeling's
    lid popped 2 - 9 degrees as a lid was closed by hand (now its lid moves fade in over 5.7 degrees above shut: 0.02
    at most). The Feeling list is on the tick's row now (in sight without scrolling at 17 and 29 px). Left as known:
    two eyes both posed past 35 degrees at different angles can end a few degrees apart in height; a cut at the limit
    can roll the eye about its look by a degree or two.
  - Files: `Camera/VoiceAlive.h/.cpp`, `Camera/VoiceFeel.h/.cpp`, `Camera/VoiceFeelFace.h/.cpp`,
    `Camera/VoiceFeelTables.h`, `Camera/VoiceGaze.h/.cpp` (new), `Camera/VoiceLifeFace.h/.cpp`, `Camera/VoiceFace.h`,
    `Camera/Voice.h/.cpp`, `Camera/PoseEditor.cpp`, the project files. Research: section 31.

- **GAME > Pose window > Voice: "From the voice": the feeling read from the voice itself, stretch by stretch** (the
  user, 2026-11-13: "emotions should apply automatically depending on the sound file expressions"; they chose a trained
  emotion model, EmoDB + RAVDESS for testing only, and the model as an optional download). In 49AEFCF2E9C9 (installed
  2026-11-13 23:41 with the game closed, over BEB0F0C7518D, the camera session's build with its CPU fixes and the
  hook-list lock, which this build also holds; every file hash checked twice), with the optional
  `Cinematic Tools\EmotionModel` folder installed beside it (EmotionHelper.exe, emotion_feel.onnx, its README and
  licence). **Not tested in game; nobody has seen it on a face; nothing was heard on the PC during the work**; not
  committed.
  - **What it does.** A tick, "From the voice", under the Feeling list. Each stretch of the voice (each phrase; one over
    6 s in parts of 3 - 4.5 s) gets the feeling a trained model hears in it, over the feeling picked, as far as the
    model is sure of it (Sureness needed, 0.55: smoothstep from it to 0.4 above it, or to a chance of 1). The model's 8
    feelings show as Calm, Happy, Sad, Angry, Afraid, Disgusted and Surprised; neutral keeps the feeling picked. A
    lively feeling heard in the voice shows a little stronger or weaker by how stirred up the voice is (0.85 - 1.15
    times; only its own part). The face changes over about 0.6 s around where a stretch begins, across a loop's end too
    (a voice of 0.6 s or more), and the same time in the voice always gives the same face. Each stretch can be set by
    hand (from the voice, the feeling picked, none, any preset) in "The voice's feeling, stretch by stretch"; a change
    there or of the sureness eases in over the same 0.6 s. "The face now" says what shows.
  - **The model** (the optional download, about 660 MB, CC BY-NC-SA 4.0: non-commercial; audeering's card says "for
    research purpose only"): audeering's wav2vec2-large-robust-12 emotion model, written out in plain torch (checked
    against its card's own numbers: within 2.3e-7, and 6.9e-9 for its pooled states), with a small head trained on the
    RAVDESS recordings that gives the chances of 8 feelings; exported to ONNX (opset 17) and run by Windows' own Windows
    ML. `tools\emotion_model` holds the scripts and steps that make it from the public downloads; `build.ps1` now also
    builds EmotionHelper.exe into `dist\EmotionModel` with the README and licence (the model is added by hand).
  - **How right it is**, measured on short acted studio recordings it never heard, one whole recording at a time:
    English actors (RAVDESS, each actor left out of the training in turn) 77 % of 8 feelings, 92 % when it is 80 % sure
    or more; German actors (EmoDB, never used to fit) 54 %, 74 % at 90 % sure or more. On game dialogue: not measured.
    What the face shows at 0.55, the mean over the feelings: English 57 % right, 5 % wrong, 38 % the feeling picked;
    German 44 / 18 / 38. Through the shipped helper the same as through PyTorch.
  - **EmotionHelper.exe** reads it outside the game: idle priority, 2 CPU threads, about 1.7 GB while it runs, about
    1.2 s to start and 1.5 - 1.9 s for each 10 s it hears with the game closed (up to 2.9 s measured while the game
    ran); one voice at a time; ended with the game; its answers kept in `EmotionModel\cache`, so the same voice is read
    once (another helper or model reads it anew).
  - **Reviewed** (4 reviewers and their skeptics: 27 findings confirmed and fixed; a fix check with 3 more and their
    skeptics: 17 confirmed and fixed). Offline: 79,430,938 checks of `VoiceEmotion.cpp` with the real helper; the Pose
    window's harness 330 checks at three window sizes with the real helper and 20,000 - 30,000 random-input frames each.
  - **Also in this work:** the living face's changelog entry was put under Version 2.0.2 by mistake (that section is road
    traffic only); it is moved here.
  - **Not known until it runs in the game:** how it looks on a face; how right it is on game voices; other Windows
    (Windows ML comes with Windows 10 1809 and later; whether its version reads this model was tested only on Windows 11
    build 26300).
  - Files: `Camera/VoiceEmotion.h/.cpp` (new), `Camera/PoseEditor.cpp` (VoicePlay, Release / FinishRelease,
    KeepVoiceLoad, KeepVoice, FeelShapeNow, the Voice tab, the help texts), `EmotionHelper/` (new project),
    `tools/emotion_model/` (new), `build.ps1`, the project files. Research: section 32.

- **GAME > Pose window > Voice: a smoother mouth, and "Mouth smoothing"** (the user, 2026-11-14: "i think mouth
  animations should be smoothed out they are too harsh right now"). In 604FAA183C96 (installed 2026-11-14 02:13 with the game closed, over 49AEFCF2E9C9 (which also holds the camera session's CPU fixes and hook-list lock, as this build does); every file hash checked twice, 30 s apart). **Not tested in game;
  nobody has seen it on a face; nothing was heard on the PC during the work**; not committed.
  - **What changed.** The mouth's curves are now smoothed three ways when the file is read, and a new slider, **Mouth
    smoothing** (0 - 2, 1 by default, kept when another file replaces the voice), picks one or mixes two neighbours:
    0 is the mouth as before, exactly; 1 eases the jaw and lips into and out of each shape; 2 is softer still, the
    closures eased too. The Voice tab's picture of the file follows the setting.
  - **How (VoiceMouth.cpp).** At 1 the closures go into the jaw before it is smoothed (before, they were multiplied in
    afterwards, so the jaw shut at the lips' speed), and the whole jaw is smoothed by penalised least squares
    (Whittaker - Henderson, second differences, zero phase) that holds it to its raw curve in closures and where it was
    shut before; the lips' shapes by Gaussians of 20 - 35 ms. At 2 the jaw is stiffer and held less hard in closures.
  - **Measured** offline on 31 recorded voices (the user's 4 files, EmoDB, RAVDESS), the jaw as the face turns it at
    Mouth opening 1: its speed's 95th percentile and maximum 146 and 368 degrees a second before, 106 and 225 at 1, 85
    and 149 at 2; its moves of 0.3 or more from 10 to 90 % in a median 43 - 49 ms before, 59 at 1, 70 at 2. On
    computer-made test voices p / b / m are seen as shut about as often as before at 1 (92 against 91 % on clean
    voices, 82 against 84 % in fast speech); the cost is quick syllables that run together: the jaw opens to a fifth
    on 91 % of vowels before, 87 % at 1 and 81 % at 2 (fast speech 84, 75, 65 %).
  - **Checked.** At 0 the curves are the old module's, number for number, on 437 of 437 files; at 1 and 2 within 5e-7
    of the study's Python reference. The mouth test suite passes (unit, 9,120 odd-size runs, threads, out of memory);
    the real `voice::Load` / `MouthAt` / `OverviewOf` 840,236 checks; the Pose window's harness 341 checks at three
    window sizes. Reviewed: 3 reviewers and 2 skeptics (6 findings confirmed, all fixed: the picture showed setting 1
    only, the rest words), a fix check with 2 checkers and a skeptic (7 confirmed, all words, fixed).
  - **If it still looks harsh:** raise Mouth smoothing towards 2, or lower Mouth opening (the jaw opens wider than
    people's in typical speech on these lines; a smaller opening also moves slower).
  - Files: `Camera/VoiceMouth.h/.cpp`, `Camera/Voice.h/.cpp`, `Camera/PoseEditor.cpp` (VoicePlay, the Voice tab's
    slider, picture and help texts). Research: section 33.

- **GAME > Pose > Idle movement keeps going on other characters** (the user, 2026-11-14: "Idle animations on other
  units don't loop properly"; asked: "Stops, then restarts"). In 8685390F4504 (installed 2026-11-14 04:47 with the game closed, over A44C4B10CD4E (a 03:38 build of the shared tree, which already held this change's first, unreviewed version); it holds the camera session's fixes too; every file hash checked twice, 30 s apart). **Not tested in game**; not
  committed.
  - **Why it stopped.** The idle faded out whenever any body bone was more than 10 degrees off where its animation had
    been over the last 3 s. Measured on the people in three of the tools' replay recordings: standing NPCs' gesture
    idles move their arms, hands and heads up to 93 degrees from that, so the gate was shut much of the time (on one
    man it opened about 1.1 s in each 11.6 s loop of his idle; which man the user held isn't known). Why Faith's own
    idle wasn't stopped as often isn't known (the recordings never have her still for more than 2 s).
  - **Now, on people** (every human skeleton of the game): it stops while they walk or run (the root faster than 0.3
    m/s over half a second) or their hips or legs move far or fast (Hips or a leg 40 degrees off, or the hips, legs or
    spine turning faster than 200 degrees a second), and comes back about a second after they stand still again. Run
    in the change's own code over the 79 recorded tracks: standing men and low-detail people kept their idle all the
    time from about 1.5 s after Take; standing guards (during chases) about 60 %; walking and running people 0.2 - 0.4
    % of the time. Drones, animals and props keep the 10-degree rule.
  - The 15-degree limit on each bone is now soft (an arm whose animation goes far still moves, more slowly); the gate
    goes by the game's time, so slow motion and fast game speeds are judged alike (tested from 0.01x to 4x, and at 12
    and 20 frames a second).
  - The 10 s CT.log line says what the gate did (open in how many captures, which limits were passed, what the
    10-degree turn rule would have shut). The tick has a help text now.
  - **Under Freeze time** the game's animation doesn't move: the idle movement eases out over a few seconds, then holds
    still (as before).
  - Checked: the real code driven by the 79 recorded tracks at 0.01x - 4x, at 12, 20 and 115 frames a second and under Freeze time; the Pose window's harness 348 checks at three sizes (a new scenario runs the gate in the window); reviewed (3 reviewers and their skeptics: 10 findings confirmed and fixed; a fix check: 14 confirmed and fixed).
  - File: `Camera/PoseEditor.cpp`. Research: section 34.

- **GAME > Pose window: the keyframes made easier** (the user, 2026-11-15: the Pose tab's keyframes "feel too bad"; of
  ten suggestions they chose "Start with 1-3"). In C88E5C29DB9E (installed 2026-11-15 16:53 with the game closed, over
  A2CBA749CC53, the culling session's 14:31 build of the shared tree, so it holds every session's work of then; game
  folder and dist, hashes checked twice, 30 s apart). **Not tested in game**; not committed.
  - **Smooth eases, as Blender's, Maya's and Unity's default keyframes do (auto-clamped).** A bone's first and last
    keyframe ease in and settle; a hold between two equal keyframes stays still (it drifted 7 - 11 degrees); a fast
    move into a rest no longer goes past it (60 degrees in 0.2 s went 7.5 degrees past); where the motion turns back it
    stops at the keyframe; between Smooth keyframes the speed carries on through them; next to a Linear segment its
    speed carries on. A turn is worked out as one rotation: a steady spin stays steady (at 120 degrees a keyframe it
    pulsed 100 - 132 degrees a second, now 120.0), and a turn about several axes never passes its keyframes (up to 8
    degrees before). With Loop on, a bone keyed at 0 s and at the end carries on through the wrap instead of stopping
    there. Linear, Ease in and out and Hold are exactly as before.
  - **One Undo and Redo for the pose and its keyframes**, in two places (under the bone's controls and in KEYFRAMES).
    A change and the keyframe Auto key makes of it are one step. A dot drag put back with a right click, a keyframe let
    go where it was, or a click that changed nothing (a ring held but not turned, a field clicked) leaves no step and
    keeps Redo. Undo of a keyframes file's load puts the clock, speed, loop, Use the keyframes and the playhead back
    too. Up to 50 steps.
  - **Hotkeys**, while the tools' menu and the Pose window are open (rebind them in the tools' Hotkeys window; by
    default on the numpad with Num Lock on: keys the camera's and the game's saved binds don't use; the game's own
    default keys aren't known): Num 0 play / pause,
    Num 8 keyframe the changes, Num 2 keyframe the whole pose, Num . remove the picked bone's keyframe at the playhead,
    Num 4 / Num 6 to the keyframe before / after, Num 7 / Num 9 one frame (a 30th of a second) back / on, Num / undo,
    Shift + Num / redo. The buttons' and the Time field's tips name them.
  - **Auto key shows what it does.** A red Auto key tick in the KEYFRAMES row (the same switch as Options), dimmed
    while it can't keyframe (the keyframes off, or playing). It now keyframes from the first change (before, only once
    the pose had a keyframe). A line under the row says what was keyframed, for example "Auto key: keyframed
    RightForeArm at 1.00 s, held as it was at the keyframe time around it (1 hollow)". The keyframes that hold a bone
    as it was at the keyframe times around its first keyframe are drawn hollow; Options > Hold the times around a new
    keyframe turns them off; Keyframe: this bone on a hollow one makes it the user's own.
  - **Reset asks** when the bones it resets have keyframes (the keyframes put a reset back when the time moves):
    keyframe the reset at the playhead, or reset and remove their keyframes. With Use the keyframes off it resets at
    once and says which bones the keyframes will put back.
  - **Keyframes at one time only (a still) aren't autosaved**, and Let go doesn't ask about them: a still no longer
    pushes an animation's autosave out ("autosave <name> before" was the only copy kept). The Files tab says so.
  - Ticking Loop or changing Length makes the pose again from the keyframes at once (the curve depends on them).
    Clicking a drag field (the root's or a weapon's Move, Length, Begins at) no longer changes its value by rounding it.
  - The texts say what really happens: a bone without keyframes keeps its pose when the time moves; only a bone with
    keyframes goes back to them; what Auto key does while the keyframes are off, playing, or their time is held.
  - Checked offline: the curve test (22 checks: the new curve against what it must do and the other eases against the
    file as it was); the Pose window's harness, rebuilt (159 checks at three window sizes, two new scenarios for all of
    the above, random input; no ImGui check fired). Reviewed by 4 reviewers and their skeptics (13 findings confirmed
    and fixed), a fix check (3 checkers, 3 skeptics: 12 confirmed and fixed), a last check (2 checkers, 2 skeptics: 8
    confirmed and fixed). Found on the way and fixed: the first one-rotation curve made the turning speed jump at
    keyframes for turns about several axes (median 28 % of the speed); now as smooth there as before (0.4 % at most).
  - Not checked offline: the hotkeys themselves (they run on the input's thread, which the harness doesn't have), the
    dots and rings (no camera view offline), and anything in the game.
  - Files: `Camera/PoseKeys.h/.cpp`, `Camera/PoseEditor.cpp/.h`, `Input/ActionDefs.h`, `Input/InputSystem.cpp` (the
    pose hotkeys called with the camera's). Research: section 35.
- **GAME > Pose: Idle movement goes on while the keyframes play** (the user, 2026-11-15: "idle animation should be
  going on even when there are key frames animating"). Before, it faded out at Play and started again from scratch at
  Pause (`UpdateIdle`'s want had `!PlayingKeys(h)`, copied from the clips' rule when the keyframes were made; no
  reason of its own found). Now it's laid over the keyframes as over a still pose: what it adds is the game's own
  animation under the pose, whatever the pose is, and channels the game doesn't write (they hold the tools' pose) are
  left out as before. A clip playing still fades it out (a whole animation of its own), so on the clip's clock, where
  the keyframes play only with the clip, it stays off. The tick's help says so. In E757C34B6728 (built from the whole
  tree at 22:22, staged; it installs over C0B0D70F6B69 when the game closes). Not tested in game; not committed.
  - Checked offline: the Pose window's harness, new scenario 3d (the real `UpdateIdle` on made-up captures as the
    game's blend hook writes them: Spine1 swaying 3.4 degrees, the keys playing a looped arm on the Real clock, 9
    checks): the idle movement fades in and stays while they play (weight 1.000; Spine1 up to 3.55 degrees off its
    keyed pose, the keyed arm, which the made-up animation holds still, 0.0000 off its keys), Pause and Play don't
    start it again, unticked it fades out while they play on. The whole harness: 168 checks at three window sizes, 0
    failed. Not checked offline: the real captures from the game's blend.
  - File: `Camera/PoseEditor.cpp` (UpdateIdle, the Idle movement help).
- **GAME > Pose: the kinematics expanded** (the user, 2026-11-15: "We also need to expand the kinematic system feels to
  basic and movements are robotic"; 2026-11-16: "do all" of the twelve suggestions in
  PoseKeys_Work_2026-11-15\kinematics_suggestions.md). New files `Camera/PoseKinematics.inl` (the solver) and
  `Camera/PoseMotion.inl` (the layers over the handed pose), included in `PoseEditor.cpp`. In B1F4273D6225 (built 02:01
  from the whole tree; INSTALLED 11-16 02:03 over BDA75BD9B1C2, game folder and dist, hashes checked twice). Not tested in
  game; not committed.
  - **Elbows and knees bend like real joints** (Options > Joints, on; with "A dragged bone bends" at 2 or more): a
    dragged hand or foot bends its elbow or knee about the joint's own hinge (from the game's rest pose of the
    skeleton, all five bent the natural way; else from the pose taken), at most 150 degrees (eased over the last part;
    a joint already bent further can stay so), the upper arm or thigh rolled about its own length so the joint stays a
    hinge. Fixes a bug: the old two-bone bend kept its *side*, which flipped when the drag direction passed it (a
    standing leg's foot lifted forward over the hip bent the knee backwards). Every two-bone bend now carries its
    plane's normal by the limb's swing, so it doesn't flip (the review found that the first version, a projection,
    still flipped when a hand was raised out to the side past shoulder height).
  - **Dragged hands and feet keep their angle** (on): a dragged hand or foot keeps its turn in the world (with 1 bone
    bending too).
  - **Feet stay where they stand when the Hips move** (on): the Hips' dot, rings and Turn sliders bend the legs so the
    feet stay put (past the legs' reach they straighten; more than 25 cm past it the feet go with the Hips); the Hips'
    Move field still moves the whole body. **Pin the hand / foot where it is** (Bone tab, a hand or foot picked): it
    stays while the rest is posed (dots, rings, sliders); editing its own limb or a toe takes the pin with it, and so
    does anything else that moves it (Undo, a reset, the keyframes' time: the pin is in Undo's steps and follows a
    hand moved by something else); a magenta square on the picture. The Bone tab's sliders now make each frame's
    change from the pose they were taken in, like the rings, so held feet and pins don't drift.
  - **Elbow / Knee direction** (Bone tab, an arm's or leg's bone picked; not while that limb's keyframes play): drag it
    sideways to swing the elbow or knee round the line from the shoulder or hip to the hand or foot, which stay where
    they are.
  - **Back and neck bend in a curve** (on): a chest (Spine2) or head drag with 2 bones or more turns each spine or neck
    bone below it the same share.
  - **Collarbone joins an arm raise** (on): a dragged hand or elbow turns its collarbone 30 % of its upper arm's swing
    (half with "A dragged bone bends" taking the collarbone in), at most 25 degrees from the pose taken. An elbow
    dragged with 2 bones or more is now aimed by its upper arm with that share, not bent as a collarbone - upper arm
    pair (so it no longer reaches the cursor exactly, as a knee never did).
  - **A far reach bends the back** (off): a hand dragged past its arm's reach bends the spine towards it, at most 35
    degrees.
  - A toe dragged with 3 bones or more moves its foot (the leg about its knee) and points the foot, instead of the
    free three-bone bend.
  - **Roll and twist bones follow** (on): the elbow and knee rolls take their forearm's or shin's turn; Faith's forearm,
    upper arm and thigh twist bones their share of their driver's (her rules, as the clips use them), laid last so
    they follow the layers below too. The dots don't show it.
  - **While the keyframes play** (Options): **Loose timing** (0.5): the head and arms follow the chest a moment later, the
    hands later still (0.02 s a joint at 1; the Hips and legs never). **Follow-through** (0.5): the arms, hands and head
    lag and settle on springs in the character's frame, by the keyframes' own time, each joint at most 8 - 20 degrees
    off (the head left to a voice that moves it). **Feet stay put between keyframes** (on): a foot in the same place
    (3 cm, 10 degrees) at the keyframes either side of the playhead is kept there between them (eased over 0.15 s at a
    stand's ends, not at a key between two stands). All three only while the keyframes' time moves (faded over
    0.25 s): paused, the pose is exactly as keyed. No holds or pins while the keyframes play.
  - **Look at** (Options, per character): the camera (also with the menu closed: its place is read then too while a
    pose looks at it) or a point (a ringed dot to drag, placed 1.5 m in front of the face): the eyes look at it, the
    chest, neck and head turn part of the way (Amount, 0.7), measured from the face's own way (its rest pose's eyes, in
    the pose before the voice: a voice's nods and glances go on over it), at most 70 degrees round and 40 up or down,
    eased (0.12 s); a place 110 - 150 degrees round behind the head fades the look out (no swing from side to side);
    the eyes at most 35 degrees from the face's way.
  - **On the picture** (Options): the picked bone's path over the keyframes (an orange line, a dot per keyframe time, at
    most 120: on) and ghost poses at the keyframe times either side (off). Shown with the bones.
  - **More eases**: Ease in (starts slow), Ease out (ends slow), Overshoot and settle (goes 10 % past the next key and
    comes back). A Smooth key next to them carries their speed across the key. In the keyframes file as easein,
    easeout, overshoot: such a file doesn't load in an older build.
  - **Between its keyframes** (Bone tab, a keyed bone between two of its keys): set it between them (0 % the one
    before, 100 % the one after, past either end overshoots); Auto key keyframes it.
  - Reviewed (workflow wf_4166313b-153: three reviewers, a skeptic each trying to refute every finding): 18 findings,
    15 confirmed (12 issues: Look at's camera place frozen with the menu closed, the Hips' Move field dragging the
    legs, sliders taking the holds again each frame, pins left out of Undo and the cancel, a pinned foot's toe undone,
    the hinge flip past shoulder height, Look at flipping with a place behind the head and fighting a voice's glances,
    the helpers laid before the layers, the foot lock's edges, the swivel while keys play, a cut-off help text, help
    texts promising more than the code did), all fixed; 3 not confirmed (handled anyway: the look point placed when the
    world comes, the trail's dots capped, the overlay's items shown with the bones).
  - Checked offline: the Pose window's harness, new scenarios 3e (Faith's limbs on her rest pose: the rig found, the
    knees' hinges along +X, the old side of the "foot over the hip" drag down and the new knee 0.337 m up on its hinge,
    the hand dragged five ways on its hinge with its turn kept and at most 150 degrees, the hand raised out to the side
    from -60 to +60 degrees with the elbow's biggest step 9.2 mm per 2 degrees, a hold at its own place turning nothing,
    the Hips moved and turned with the feet 0.00 mm off, a 5 m move taking the feet along, a slider there and back
    leaving the legs as they were, a pin holding a hand to 0.00 mm while Spine1 turns 25 degrees, a pin put back by a
    cancel, a pinned hand moved by something else not pulled back, a pinned foot's toe dragged, the head's neck curve
    32.9 / 32.9 degrees, the collarbone, a far reach, the swivel, a toe), 3f (the helpers to 0.01 degrees and the
    elbow's roll on its sprung forearm, loose timing 12 degrees behind on a 120 degrees a second key and exact when
    paused, follow-through up to 44 degrees on a 280 degrees a second swing at 1 and settled after, a foot 0.00 mm off
    where it stands between keys whose own curves slide it 20.4 mm, the eyes 0.02 degrees off the camera, the camera
    orbiting behind at 240 degrees a second with the head's biggest step 2.0 degrees a frame and the look gone straight
    behind), 3g (the eases' shapes, Overshoot's peak 1.100, Ease in arriving at 90 degrees a second and the Smooth key
    after it leaving at 90, the file's words). Not checked offline: the game's own poses and skeletons other than
    Faith's rest pose, a voice with Look at, the game taking the layered pose, how any of it looks.
  - Files: `Camera/PoseKinematics.inl`, `Camera/PoseMotion.inl` (new), `Camera/PoseEditor.cpp`, `Camera/PoseKeys.cpp`,
    `Camera/PoseKeys.h`.
- **GAME > Pose: drag the dots of the picked bone's path** (the user, 2026-11-16: "make it so that you are able to move
  the dotted orange nodes of the movement"). In 08EDCB33575C (INSTALLED 11-16 05:23 over DDB153D9AFEE, game folder and dist). Not tested in
  game; not committed.
  - A press on a dot of the orange path puts the playhead at its time and the pose there at once (a click alone does
    only that); the bone is then held like its own dot (the bones above it bend as "A dragged bone bends" says; Shift,
    the wheel, the arrows, Bones stop at walls and a right click work as for its dot). Let go after a drag, the change
    is keyframed at that time, with Auto key off too (only the bones the drag changed then), in one Undo step with the
    drag. While held, the path, the dragged dot and the ghost poses show the keyframes as they'll be made (hollow holds
    included), so the dot stays under the cursor. The dot under the cursor or dragged is drawn bigger with its time.
  - The dot at the playhead's time lies under the bone's own dot, which takes the click (and drags as usual). With Loop
    on, the dots at 0 s and the Length are one place when the bones are the same at both ends: a drag of it keyframes
    each such bone at both ends (the loop stays seamless); the playhead's own end is the one taken. With the playhead
    at that end the dot is under the bone's own dot, which drags as usual and keyframes that end only.
  - Not taken (the label says why): while the camera track holds the keyframes' time (their Track clock), past their
    Length, and with Auto key off while a bone with keyframes has a change not keyframed yet (at another time: any
    such bone, moving the time would drop the change; at the playhead's time: a bone of the path, the dot isn't where
    the bone is). A ring under the cursor takes such a click instead. Not while the keyframes play.
  - Reviewed three times (workflows wf_2c6de33f-dd6, wf_653979bf-bdc, wf_6b9fdbbe-aba): fixed the drag starting from
    the bone instead of the dot, two Undo steps, a thinned path's dragged dot vanishing, a copy of the keyframes every
    frame, ghost poses not following, a click near the rings dropping changes not keyframed, the path's drags with the
    Pose window closed not autosaved, the loop's two ends, the stacked dots, a refused click leaving a text field
    active, the loop's other end added as a dot for nothing, the dot at the playhead with a change of a bone above it,
    and Bones stop at walls sweeping from the dot instead of the bone.
  - Files: `Camera/PoseMotion.inl` (MakeTrail, DrawKinTrail, DrawKinPins, TrailRefusal, KeyTrailDrag, LoopOtherEnd),
    `Camera/PoseEditor.cpp` (DrawOverlay, ApplyKeysNow split out of KeepKeys, KeyBoneIn split out of KeyBone).
- **GAME > Pose window: Save, Save as and Load in the keyframes bar** (the user, 2026-11-16: "also make it possible to
  save the keyframes to a file"; asked which, they chose the buttons by the timeline and the Windows file window for
  any folder, on its own thread). In 08EDCB33575C (installed 05:23). Not tested in game (the file window has never been opened in the
  game); not committed.
  - **Save** writes the keyframes into the file they were last saved in by hand; with none yet, or after a Load, it
    asks where first (so a loaded file is never written over without asking). **Save as** opens the Windows file
    window (a .posekeys file anywhere, overwrite asked by the window). **Load** lists Cinematic Tools\Poses' files
    (names past the ANSI code page too) and "Another file..." (the file window). A load is one Undo step. The window and
    the file's writing or reading run on a thread of their own, so neither holds the game up; the buttons wait while
    one is under way. The window has no owner window (the game's belongs to another thread): it can open behind the
    game (Alt+Tab; the note under the bar says so). A press that comes as the window closes with a button down (a
    double-click on a file; seen by the window's OnFileOk, before it closes) isn't taken by the menu or anything on the
    picture (`pose_editor::QuietMouse`, called in `UI.cpp` right after ImGui's NewFrame). A load waits while a bone, a
    ring, a keyframe on the timeline or a field is held. A Save as answered after a load (from the Files tab while the
    window was open) doesn't make Save write there unasked. The KEYFRAMES help says that with the Pose window closed
    Auto key waits (it always did) until the window is open again.
  - A saved file now takes the old one's place in one step (MoveFileEx), for the Files tab and the autosave too: a
    failed replace loses neither, the new keyframes stay in the .tmp beside it (a read-only file there: no tries again,
    no wait).
  - Reviewed twice (wf_c562cb29-cc2, wf_6b9fdbbe-aba): fixed Save writing over a loaded file after an Undo of the load
    (and over an autosave or a lossy load's file), the old file deleted before the new one was in place, the file I/O
    on the game's frame, mangled non-ANSI names in the list, the double-click click-through (twice: its guard was set
    too late), a load landing during a timeline drag, a late Save as answer after a load, retries on a read-only file.
  - Checked offline: the Pose window's harness, scenario 3i (a whole path past ASCII saved, saved over and loaded back;
    the bar's Save clicked with no file (asks in Cinematic Tools\Poses with the pose's name, written), again (written
    there, nothing asked); the Load menu's file named past ASCII clicked (loaded, Save's file let go of); Undo of the
    load, then Save (asks, the earlier file untouched); Load > Another file; a load answered while a bone is held
    waiting; the window failing; the pose let go; a press quieted as after a double-click and the next one taken; a
    Save as answered after a load; let go and taken again), with the window's stand-in on the real worker thread. Not checked offline: the real Windows file window in the game (focus, a fullscreen game, behind the
    game, shell extensions in the game's process).
  - Files: `Camera/PoseEditor.cpp`, `Camera/PoseEditor.h`, `Camera/PoseKeys.cpp`, `Camera/PoseKeys.h`, `UI.cpp` (one
    line).
- **GAME > Pose window > Voice: the emotions engine improved** (the user, 2026-11-16: "find any improvements for the
  emotions engine and implement them no mistakes"). Found by four finders (the reading, the face, bugs, use), each
  answered by a skeptic (workflow wf_2c4a1261-10a: 22 proposals, 18 upheld with corrections, 4 refuted); reviewed by four
  reviewers, each answered by a verifier (wf_b8913a25-0bf: 15 findings, 14 confirmed, 1 real but practically unreachable;
  all fixed), and the fixes checked again (wf_b65e9509-a64: all 15 fixed, 4 small new ones found and fixed). In
  A05EDC98DFD2 (installed 2026-11-16 15:19 by the PC's clock, over A6D8984E12FF). Tried in the game 16:31 - 16:41
  (CT.log: Faith's lines read anew with the shared windows, CS05 kept from its earlier reading, a reading stopped by
  another voice, cached readings taken, a 4 min 15 s voice of 48 stretches read in 11 s, no warning); the user:
  "Everything seems fine". Not committed.
  - **From the voice: phrases the model hears together share one reading.** A short phrase is heard with the voice
    around it (3 s); a short phrase that begins inside that window now joins it (the group heard as one: 3 s around it
    or its whole span, under 6 s), each phrase keeping its own row and setting. Before, on Faith's lines, two windows
    sharing 98 % of their samples were heard as different feelings (fearful 38 % against calm 29 %), and one 3 s window
    was read twice. The helper hears each window once (on the five voices tried: 79.8 s heard become 63.0 s). A voice
    with no shared windows keeps its cached reading (A_VO_CS05_Faith: the same key); the others are read once more.
  - **A voice already read is taken at once**, also while another voice's reading runs (it waited for that reading
    before). A Stop during its hashing is taken (checked between 8 MB chunks).
  - **Memory**: a reading doesn't start while Windows has under about 5.7 GB of memory left to give (1.7 GB for the
    reading, 4 GB kept for the game, which ends itself when Windows has none left), and a running one is stopped if that
    falls under 2 GB; both, and the helper ending out of memory, keep Try again (before, an out-of-memory end said "no
    Try again" and blamed Windows ML). The messages round down and state the rule.
  - **Stop**: a Stop button while the voice's feeling is read (also when From the voice is unticked while it runs); the
    progress says "starting the model" until its first stretch, and the seconds so far.
  - **Why a stretch shows what it shows**: the mouse on a row of "The voice's feeling, stretch by stretch" (or on the
    new strip along the voice picture's top) shows all 8 chances, the model's arousal / dominance / valence, what it was
    heard with (the phrases heard together), and why: shown, partly, not sure enough (with the Sureness at which it would
    show), heard as neutral, too short, set by hand. The summary line counts them by reason. On the voice's own clock a
    row's time is a button that takes the voice there.
  - **The log** says when a reading starts (its windows, the seconds heard, the memory left), ends (with its cache
    file, its own time and the wait for another voice's reading apart), stops or fails (one warning), and, as diagnostic
    lines, what each stretch was heard as (its top three chances and A/D/V).
  - **A sound whose phrases were never looked for** (outside 8,000 - 192,000 Hz, or its analysis failed) says so, not
    "No speech was found".
  - **The same file loaded again while its reading runs** keeps that reading (not stopped and begun again); one still
    waiting for its turn is begun again on the new read (not to hold a second copy of the sound).
  - **The face**: a feeling picked while another change is still going no longer moves the face faster than a change
    (it could move it up to 3.8 times as fast: 0.105 of the face in one frame); **Surprised drops the jaw a little and
    Afraid parts the lips** (about 3 and 2 degrees in a pause, under the speech's own jaw, scaled by Mouth opening, gone
    where the lips close for p, b and m: the closure taken over 80 ms either way, so it goes no faster than the speech's
    own jaw moves; measured on the five voices tried, no closure left apart at Lip closing 0.5 - 2 and every Mouth
    smoothing; with Lip closing 0, p, b and m left out, it stays); **"Smile comes and goes"** (1 by default; 0: held as before): a smile rises a
    little before speech, is at its fullest 4 s at most, eases to 85 % while the speech goes on, and to half between
    stretches of speech (felt smiles last about 0.5 - 4 s; one held at its fullest about 5 s looked more fake:
    Krumhuber and Kappas 2005, smiles alone on made-up faces; the times and levels are designed within that).
  - Refused by the skeptics, not built: eased (slow in, slow out) changes, the left side stronger by default, keeping
    stretch settings across files. Not done, for later: measuring how right the reading is as the tools hear it
    (phrases in their windows, about 3 - 4 h of computing with the game closed), which decides two changes to the helper
    (hearing a stretch with silence around it as the model was trained, averaging four starts).
  - Checked offline: Emotion_Tests_2026-11-16 (emotion_test: the stretch rule on the five real voices and 100,000
    random ones, the looks, the parser and why on the 24 real readings, the reading thread against a stand-in helper:
    2,365,900 checks; face_test: the smile, ShapeOf bit for bit the old one with the smile all there, the jaw: 6,186,586
    checks; jaw_gate: the jaw at every closure of the five voices), and the Pose window's harness (new scenario 3j with
    the stand-in: the reading, Stop, Try again, the carry and the waiting one not carried, the rows' and the strip's
    tooltips, the time buttons, the smile, the jaw and its closure over time, a change picked mid-change, no phrases:
    59 checks; the whole harness at three sizes).
  - Files: `Camera/VoiceEmotion.h/.cpp`, `Camera/VoiceFeel.h/.cpp`, `Camera/VoiceFeelFace.h/.cpp`, `Camera/PoseEditor.cpp`.

## Version 2.0.2

Road traffic only: 2.0.1 with the change below and nothing else.

- **Road traffic: no more traffic jams, and the roundabout by the station flows** (the user, 2026-11-12, with a
  screenshot of the roundabout packed with cars: "the traffic still looks pretty bad find improvements and fix it no
  mistakes high priority", then "make sure that they are never traffic jams"). Release build FE9B5E7EB26D (2.0.1's
  sources plus this alone). The same traffic code is in the developer's build 08A51E9A3C59 (with other work not in
  this release), installed 13:42.
  - **What it was** (the offline car test, with the game's own car step):
    - The roundabout's ring belongs to one car placement (79): a closed 658 m loop with no spawn or despawn, holding
      its 82 cars, 8 m of road a car. It never locked, but it always crawled nose to tail at 72 - 78% of its speed.
      Four more routes were as full (190, 98, and 17 with 180, which share their roads; 244 less so).
    - Queues that didn't clear in one green: 188's split and merge just north of the ring, 235's light, 93's ten
      lights and its tight corner, 98's merge.
    - Cars waiting on each other in a circle across two placements, or more than 8 cars, were never sorted out: a
      made-up ring packed with 36 cars and two lights stood for good, 126's hairpin held cars up to 149 s, and at
      3,000 cars the whole city gridlocked.
  - **The fix** (Camera/RoadTrafficCore.h, Camera/RoadTraffic.cpp):
    - **Each route's most cars:** a table of the city's 20 car networks (matched by their nodes, from the game's
      data) with how many cars each holds flowing. 79 (the ring) shows 65 of its 82, 17 90 of 103, 98 51 of 82, 190 31
      of 41, 188 55 of 82, 235 69 of 92, 93 45 of 65; the rest as before. The others drive on unseen as at night, and
      the game respawns only up to it. A placement's extra cars go at its very first step (before the game has drawn
      it, by the game's step and cull order [inferred]); afterwards only out of sight, as the time of day's share.
    - **Cars waiting on each other in a circle** for 3 s, in any placements, up to 64 cars: one of them (out of sight
      first) drives on unseen. A queue at a red light is never one.
    - **A last stop:** a car that has stood still 15 s out of sight, or 20 s in sight, drives on unseen, whatever
      holds it (the longest red light is about 12 s). One a second per placement, never a placement's last car.
    - Car ids are now unique per placement (by its slot, not its address).
  - **Offline, 11 runs of 30 minutes** (3 seeds, Faith's view, a view of the roundabout, low step rates, 8 of 21
    placements stepped as the game culls, 3,000 cars, night), installed -> new:
    - no car stood 20 s or more in any run (before: up to 149 s at 2,000 cars, 1,754 s at 3,000);
    - the longest queue (p99) 33 - 78 -> 20 - 25 cars; cars stopped 2.2 - 2.9% -> 1.2 - 1.7% of the time;
    - the ring: 72 - 78% -> 88 - 92% of its speed, stopped 2 - 5% -> 0 - 1%; 98 61 - 70% -> 89 - 92%, 93 68 - 79% ->
      83 - 93%, 188 69 - 84% -> 84 - 86%, 190 74 - 89% -> 87 - 93%;
    - cars disappearing in view: 4 - 12% fewer (on screen at the roundabout 37 -> 11 in 15 minutes); never two cars
      inside each other; turning unchanged; the rules cost the same (2.25 against 2.31 ms a frame offline, three runs each, one at a time);
    - the price: 139 fewer cars shown by day (1,907 -> 1,768 of 2,000), 11 at night.
    - With the new rules switched off, every car's record came out the same as the installed rules' (3 runs); with
      them on, nothing changed before they first acted.
  - Checked by four independent reviewers (the code, remaining jams, side effects, the limits). Their findings are
    in: a circle behind a queue of 54+ cars or of exactly 64 was missed (fixed), the first limits left the ring
    empty once a lap and took cars where nothing jammed (retuned).
  - **Left:** 93's tight corner, where its two one-way roads meet 2.3 m apart (pairs hold each other a few seconds,
    sorted out by the circle rule), and placement 102 by the ring at about 70% of its speed (not standing).
  - **CT.log** (diagnostics on): one line per car placement, e.g. "Road traffic: a car placement (25 nodes, centroid
    -99.3, 312.7; 82 cars) is route 2 of the table: at most 65 of its cars show" (or "no route limit" / "isn't in
    the route table", which would mean the game's networks differ from the table's). The minute line adds the cars
    made unseen to end a circle, after standing (and how many in sight), and at a first step (and how many in the
    rules' wide sight test; a placement turned to face is first stepped near the view, still before its first
    drawing [inferred]).
  - **Works in game** (2026-11-13 13:42 - 13:50, with the developer's build 08A51E9A3C59, which has the same traffic
    code; the user: "I tested it and it works fine"). From CT.log:
    - All 21 car placements matched a row of the route table ("is route N of the table", none "isn't in the route
      table"), the eight limited ones at their limits (79: 65, 17 and 180: 90, 98: 51, 190: 31, 188: 55, 235: 69,
      93: 45).
    - In 8 minutes: 12 cars made unseen to end a wait cycle (none over two placements), none after standing, 117 at
      a placement's first step (56 of them inside the rules' wide sight test), none after 6 s of put-backs.
  - Research_Road_Traffic_2026-11-03.md section 15.

## Version 2.0.1

Fixes for the first outside tester's report on 2.0.0 (2026-11-10, their CT.log: "zone loading doesn't load the whole
map" and "snow is applied weirdly"; the user: "Yes, fix all four"). Made by a workflow (an implementer, a skeptical
reviewer and a fixer per area), then three more read-only reviews (the whole diff, then the changes after each); every
finding checked against the code before it was fixed.

- **GAME > Level streaming > Room for the whole city, on by default** (the tester's "doesn't load the whole map").
  - Cause: the renderer's draw batch spread (`[Misc] BatchSpreadChunks`) was a hidden test setting, 0 by default; the
    developer's own config.ini had 1700, so the whole city only ever loaded there. Without it one batch has room for
    111872 instances, and the tester's log was at its limit all along.
  - Now 1700 by default (written to config.ini only when it differs) and a menu tick, first under Level streaming. It's
    switched only with no renderer there, or while in every renderer category 0 has only ever used its first batch,
    that batch has never run past 1748 chunks (its fill bytes past 1748 land on the second batch's first 300 bytes:
    checked for 0) and it is 128 chunks or more short of where the add's picking changes (the spread's chunks, and
    1748): then it's as if the renderer had had the setting from its start. Switched off while an add raced the switch
    into the next batch, the hook keeps that category spread (g_BatchSpreadLast). 2.0.0 switched it at config time with
    no check. The tools check when the config is read and again from the batch scan; not before the config has been
    read (the camera update runs earlier). The log says when it's switched ("Room for the whole city on/off (...)").
  - The add counts as hooked only when its hook was made and enabled (MinHook gives the original's pointer before it
    enables): a failed enable leaves the spread off instead of counting room the game doesn't use.
  - The draw batch's room is per category (category 0 9 x 1700 x 64 with the spread, the others 1748 x 64); with the
    spread off the numbers are 2.0.0's. A new per-batch guard takes zones away from 86 % / 92 % of a batch's 2048
    chunk pointers (where the game crashes), so it acts before the overflow guard. With the spread on the renderer's
    instance list (1000000) is the limit reached first: the menu and the log then name it.
  - Without the wider batch handle or the add's hook: "Room for the whole city: not with this game build" and the real
    room (55936 without the wider handle).
- **The streaming no longer cycles at the limit** (the tester's log: 80 draw batch warnings from 19:01:09 to 19:10:46,
  38 of them "taken away", half of them 2 s or less apart: the same zones loading and unloading).
  - A zone point taken away for room is held out, by its index, for every streamer: 10 s, twice as long when taken
    away again within 60 s of coming back, at most 120 s, and (not the camera's own two) until the fullest limit is
    under 0.875 of its hold-back level (the draw batch under 70 %) with nothing at any level. The camera's own point
    and the one ahead come back as in 2.0.0 (nothing at any level) once their time is up and the counts have settled.
    While the most important missing point is held out nothing after it goes in. A hold is forgotten when its point
    stands for another place (100 m; the camera's own 1000 m), at a level load (a guess: renderer objects changed),
    and, at most once a minute a point, when a point taken away behind the camera comes into view (turned round at a
    limit, the view would otherwise stay unloaded; in view one behind is taken away for it a second, as in 2.0.0).
  - From the draw batch's and the meshes' settle levels on (where each streamer already waited in 2.0.0), all
    streamers share one gate: one zone point at a time, once every zone asked for has loaded (or 10 s of waiting, then
    the wait starts again), the streamer that has waited longest first. Not from the model instance lists' or the
    renderer memory's: with the whole city loaded those stood at 63 % and 51 %.
  - Removal order: the camera's extra zones and the Always districts first, the rain's zones and the real Shard's only
    on the urgent path, the camera's own two last. The renderer's instance memory (never goes down within a level) is
    left out of the release level.
  - Batch warnings: one at the first rise, then a summary at most every 30 s.
  - The Districts list shows whenever a district is Always or Never (or the real Shard is on), with Load the world
    around the camera off too.
- **Snow, the wet look and the replay's passes at any resolution scale** (the tester's "snow is applied weirdly": they
  render at 5120 x 2880 into a 2560 x 1440 frame).
  - Cause: the game's G-buffer pass was only recognised by a depth the swap chain's size, so at another resolution
    scale it was never seen: the snow fell back to depth facing and the picture's light ("no copy of the game's
    G-buffer for 10 s"), the ice on water to the old camera.
  - Now the render size is learned at Present: the frame's size at once (100 %, as before), another window-shaped
    G-buffer pass's size after 30 frames in a row, and not while the game's resolution scale (GameRenderSettings +
    0x14, the original tools' resolutionScale; whether it follows the game's menu setting isn't checked yet) reads
    100 %. Logged: "Replay: the game draws its view at W x H for the w x h frame (N % ...; the game's own resolution
    scale reads X)", or "... isn't taken for the game's view: its own resolution scale reads 1.000" when refused. Only
    the targets' format verdict is cached, the depth's size is read each time.
  - The lit, decal, half-size and runner's vision passes are told apart at that size; GameFrame's scene depths are
    learned again when it changes; WetLook reads the G-buffer copies at depth texels (neighbours whole texels away);
    the replay's runner's vision mask is render-sized and the mix (RunnersVision.hlsl) reads it scaled, so the Dash
    markers show at other scales too.
  - Headers recompiled with fxc 10.0.22000.0 at Idle (WetLookPs, WetLookRainPs, WetLookSnowGroundPs, WetLookQueryPs,
    GbufferWetPs, RunnersVisionPs); the untouched sources recompile byte for byte to the committed headers. At 100 %
    the shader math reduces to 2.0.0's.
- **Start-up warning gone**: "The Shard and the Bauble: the camera triggers' code isn't what the tools know" was a
  start-up race (the camera update ran before the check). The check's state is now three-way (not checked yet /
  failed / hooked) and set after the hook is made; a real failure gives one warning naming the menu's Unbroken Shard
  and Bauble and Shard office blinds.
- Log text: "snowy city look taken back" (was always "rainy"), spot sounds "(the camera off)" (was "-1 m away"), the
  Bauble's trigger line; README's level streaming section brought up to date (the old 70 % / Hold back / Unrestricted
  text).
- Build 16B723FA6B2C (usp10.dll 18F8619DD56D; 11-10 23:36, build.ps1 at Idle; no warnings in the changed files;
  check_dll: no user paths, PDB name only, props and roads embedded): **INSTALLED** 23:36 (game folder, dist; the game
  closed).
- **Works in game** (11-10 23:44 - 11-11 00:03, 16B723FA6B2C, config.ini without BatchSpreadChunks; the user: "zones now
  load", "Same with rain", "replay with a dash works now"). From CT.log:
  - Both sessions: "Room for the whole city on (the config, before any level)" (no renderer when the config was read);
    23:52:51 "The renderer's first draw batch is full to 1700 chunks, instances go on into the next ones"; no draw
    batch warnings.
  - The game's resolution scale (GameRenderSettings + 0x14) follows its menu: "reads 1.000" at 100 % (3840 x 2054),
    "reads 2.000" at 200 % (7680 x 4108 for 3840 x 2054; also 2560 x 1440 for 1280 x 720 and 3828 x 2344 for
    1913 x 1172 after window size changes).
  - Snow at 200 % (23:57:59 - 23:58:25): the G-buffer pass's depth 7680 x 4108, no "no copy of the game's G-buffer"
    warning (the tester's fallback). Rain at 200 %: "the wet ground written into the game's G-buffer (3828 x 2344 ...)".
  - Dash replay at 200 %: the markers drawn into the replay's mask (3828 x 2344) and mixed into the 1913 x 1172 picture.
  - No Shard / Bauble start-up warning. Not seen: the cycling case with the tick off, 150 %.
  - Old, not from 2.0.1: at the 23:47 quit "The game outgrew its positioned model instance list (524288 -> 0)" (the
    lists freed at the quit; the exit was normal), and in the replay "0 of the 15 voices of the recorded sounds are
    there" (the replay's sound code is 2.0.0's).
- To test: a default config (no BatchSpreadChunks) and "Room for the whole city on" in CT.log at the start; every
  district Always loads the whole city; with the tick off, at the limit no cycling (summaries every 30 s); snow and
  rain at 100 % exactly as before, and at 200 % (and 150 %) with the game's G-buffer ("the game draws its view at 5120
  x 2880 ... reads 2.000"; if it says "isn't taken ... reads 1.000" the scale field doesn't follow the setting); the Dash
  markers in a replay at 200 %; no Shard start-up warning.

## Version 2.0.0

- **GAME > Faith in the streets > Solid street: the Sky City reshape applies again** (the release check of the user's
  11-10 14:04 run with C31482BE67B7 found it hadn't; the user chose to have 2.0.0 wait for the fix). Details in
  Research_Ground_Level_2026-11-01.md section 27.
  - In that run all six shapes (the base block, the north wedge, the four ceiling leaves) were logged "aren't as the
    tools know them (left as they are)" and nothing was changed. Since the low ceiling entry below (9F8FF7F19543), each
    shape's object was compared whole, from + 0x10 to its end, with the file's image, and the game fills a pointer into
    each one when it loads the zone, very likely: + 0x20, the shape's properties (zone 55's file has exactly one pointer
    fixup inside each of the six objects, there, to the mass properties stored right after it; not read in the
    game). The 01:16 build (BDED5CA8E39A),
    which compared only the counts and the arrays, had changed the block and the wedge in the game.
  - Now a shape's object is matched by its header's four count / offset pairs (+ 0x30 .. + 0x33, + 0x40 .. + 0x4B) and
    everything from + 0x50 to its end. Nothing before + 0x30 and none of the header's other bytes is matched on or
    written (each flush still compares those 48 bytes with the file, for the diagnostics' line only): the counts are one store each (+ 0x40 8 bytes, + 0x30 and + 0x48 4 bytes; the + 0x38 word isn't written).
  - The "aren't as the tools know them" line now says how many bodies couldn't be read and which byte first isn't as
    the file has it, at each packfile's place (logged again if a later look first finds that byte). With diagnostics
    on, a line lists the bytes the tools don't go by that the game has changed.
  - The run's "no flush of its bodies' world in 2 s" lines each came 2 s into a game pause of over 2 s (the physics
    doesn't flush while paused): harmless, left as they are.
  - Offline checks: skysim, 1,311 checks on zone 55's real bytes, with new rounds where the game's own bytes are set (a
    properties pointer, user data, flags, all 48 bytes the tools don't go by), a count, an offset or a shape's last
    byte off, a root not 16-byte aligned, and unreadable bodies; the release build's source fails 335 of them, and four
    loosened versions of the compare 101 - 278. holdsim all pass; perfsim's 120 s walk trace is the same as the
    installed v14 build's (0E24BF212533F4EA). The diagnosis and the fix were each checked by independent agents, every
    finding put to a skeptic; the review's findings (the log text, the header stores, the tests' gaps) are all fixed.
  - Build 7BC3594EA5F0 (11-10 15:23, build.ps1 at Idle, GroundLevel.cpp only; GroundProps.ctpk unchanged): **INSTALLED**
    15:24 (game folder, dist; the game closed).
  - **Works in game** (11-10 16:1x, 7BC3594EA5F0): at 16:11:43 CT.log has all six shapes (the base block, the four
    ceiling leaves, the north wedge) "changed in the game's collision (2 of its zone's bodies)", and the station's kill
    box spares Faith. The user, at the station: "ok everything works at it should".
- **GAME > Faith in the streets > Solid street: stand-ins for the Sky City hall's walk-through surfaces** (after the
  low ceiling entry's install below, the user, 2026-11-10: "Yes, all of them", to "Add stand-in boxes for the hall's walk-through
  surfaces?"). Data only: no source change. Details in Research_Ground_Level_2026-11-01.md section 26.
  - GroundProps.bin v14 (48BEEE9BB51A, 151,689,514 bytes; GroundProps.ctpk 20,045,552): v13's 2,839,646 boxes kept, plus
    3,904 Street level boxes (concrete) about 1 mm behind the drawn surfaces that had had no collision since the base
    block's cut. They are the 58-degree leaning wall (153), the 45-degree ceiling (208), the shop-front and side cards
    (1,544), the pool rims (1,586), the exterioraddons pillars' feet (103), the two niche walls (70) and the hall's west
    end (240: its west wall, and the landing's south-west wall, west curb, south slope and north-west pillar).
  - The rules each box was held to: its faces behind the drawn surface (bridges across slits narrower than Faith up to
    14.9 mm), no box face standing in air she can see or reach, no walkable floor inside a box, pieces 1 m or less (8
    at 1.04 m). Where a box's back shows into a space she can't reach, it's 0.10 m deep, the thinnest the props data
    takes. Accepted: some faces 5 - 7.7 mm behind at the landing's curb and north-west pillar, boxes inside that pillar's
    floor trim seen only through a 0.06 - 5.09 mm slit, and box bottoms seen up through the pillar's floorless footprint.
  - Four rounds of independent offline checks, every problem raised put to a second agent trying to refute it; the
    last round found nothing to fix. Loader rules, holdsim: all pass. perfsim's 120 s walk: planning 0.198 ms a frame
    (0.196 on v13), the flush's part 0.047 ms. The walk test: the props cache against the whole search on 515,355
    frames, 0 differ.
  - Build C31482BE67B7 (11-10 13:56, build.ps1 at Idle, the release cleanup at 1e12d0d plus v14; the embedded
    GroundProps.ctpk checked against the .bin): **INSTALLED** 13:57 (game folder, dist). **Not tested in game.**
  - Left for later: walk-throughs at the west end outside the hall (the north-west corner's glass and facade, the
    south-west wall's outer face and end cap, the stairs' risers and treads, the slope over the stairs, the north-east
    pillar's south face, the lintel over the north-west pillar).

- **Release cleanup for 2.0.0** (the user, 2026-11-10: "Wait for the latest solid ground session to end then clean up";
  their choices: GroundProps.bin compressed, diagnostics off behind a switch, source comments and this changelog left
  as they are).
  - **Diagnostics off by default, behind one switch:** `[Debug] Diagnostics = true` in Cinematic Tools\config.ini turns
    them back on (read in Main::Initialize before the log, the hooks and the tools' threads: util::DiagnosticsOn();
    written back by SaveConfig only while on). Off: no "(diagnostic)" lines and none of the work behind them: the
    Effects, Sun shadows, Green screen (and its depth read-back), Wet surfaces, Snow, Weather, Free camera, Loading
    screen and Exit lines; the crowd shader .dxbc dumps; Save the G-buffer (hidden); the SSR watcher's hooks; the asset
    log's research hooks (CreateTexture2D, bundles, entity creation; the mission hooks stay); the draw log stage 0 on
    every recording (replay_shapes::WantDrawHooks keeps the replay's draw hooks); the replay's people lines, recipe
    lines and other research lines; the recording-cost and "(performance, diagnostic)" timers; the job-queue sampler
    and peaks (the "was full" warning stays); the crowd tracker line; the container, drone and character destroy
    diagnostics with their stack captures; the world log's timeline hooks and the sound log's entity table; the DoF
    test's file trigger (the button stays); the hang watch's Close / LevelGone / ExitHang reports. Kept: crash notes,
    the Hang_ report (the 10 newest are kept, older ones deleted at start), the stuck-quit end, every feature line.
    util::log also drops any line whose format says "diagnostic" (or, for "%s" lines, whose text does before its
    first ':') while off.
  - **GroundProps.bin stored packed:** the DLL embeds Resources\GroundProps.ctpk (LZMS, 19,997,200 bytes for v13's
    151,476,818), unpacked at ground::Init into memory freed when Props::Load returns (Util/PackedResource.cpp: header,
    stream size and SHA-256 checked; cabinet.dll taken from the system folder: the DLL imports it anyway, for
    ReplayStore's saved recordings). tools/pack_resource.py
    packs it; build.ps1 packs again whenever GroundProps.bin is newer; GroundProps.bin is in .gitignore. Offline test of
    the unpack code (scratchpad test_unpack.exe): byte for byte in 1.04 s at Idle; 9 kinds of damaged input refused.
  - **Panini** (Visuals/Panini.hlsl) rewritten from the 2010 paper's equation (cos(phi) solved from x = k sin(phi) /
    (d + cos(phi))), recompiled with the header's fxc lines (the old source rebuilt byte for byte with them first):
    2,000,000 points in float32 within 0.02 px of the old shader at 4K.
  - **No build paths in the DLLs:** /PDBALTPATH for both DLLs; imgui's checks (IM_ASSERT, imgui/imconfig.h) fail as
    assert() does (the CRT's message, then abort) but name the file "imgui" instead of its full path.
  - Also off with the switch (found by a review of the cleanup): road traffic's minute lines, the free camera's lens
    count, the mouse-wheel probe lines, the pose editor's drone line, three playback recipe lines, the mission reads
    while not recording, and Street level's planning-time line, unspared kill volume line and ground-ids line. A report kind that isn't written no longer takes one of the hang watch's 6 a session.
  - THIRD_PARTY_NOTICES.md: MinHook's licence text, ProggyClean.ttf, the Panini paper. .gitignore: logs, dumps,
    Cinematic Tools\ output, .claude\, *.npz. The project filters list every file (78 were missing).
- **GAME > Faith in the streets > Solid street: the Sky City hall's low ceiling over its east end lifted** (after the
  previous entry's install, the user: "Yes, fix it"). Details in Research_Ground_Level_2026-11-01.md section 25.
  - With Solid street on, skycity_building_01's four convex leaves that made the low ceiling (72657, 72678, 72679,
    72656, 1.6 - 2.5 m over the hall's floor) are cut at the base block's top, about 6.7 m over the floor (the drawn
    45-degree ceiling, from 4.15 m, had no collision behind it until v14's stand-ins, the entry above). What they lose
    outside the hall lies inside the base block's old box (its kept part or Street level's fill), apart from two
    slivers of up to 1.2 and 11.8 mm on the building's outer faces. Two of them get shorter face lists (the game reads
    the counts fresh each call).
  - The reshape's writes moved into the physics flush: the camera hook only decides, the client world's (Faith's)
    flush writes each shape whole in a safe order, and each body on a written root (the client and the effects world
    very likely share zone 55's collision; not checked in game) is renewed in its own world's flush. Unticked while Faith is where the ceiling was,
    Solid street stays on (the menu line says so) until she's out of it.
  - Still with no collision since the base block's cut (until v14's stand-ins, the entry above): the 58-degree
    leaning wall (into a dark closed space), the 45-degree ceiling, the shop-front cards (closed rooms behind them),
    the pool rims and the pillars' feet.
  - Build 9F8FF7F19543 (11-10 05:10, the ground-level session): INSTALLED (game folder, dist). 551 offline checks on
    zone 55's real collision bytes pass; two code reviews, every finding fixed. **Not tested in game.** In its first
    run in the game (C31482BE67B7, 11-10 14:04) the reshape didn't apply: all six shapes "aren't as the tools know
    them (left as they are)"; the stricter whole-object compare was the cause, very likely through the + 0x20 pointer
    the game fills in at load (not read in the game): fixed in 7BC3594EA5F0, the entry "the Sky City reshape applies
    again" above.

- **GAME > Faith in the streets > Solid street: the Sky City train station at the Shard's foot** (the user, 2026-11-09:
  "make the shard mall floor solid and remove the death zones from there", then "ok reshape it so the base block doesn't
  have any death zones etc", "make sure that the outside balcony area is solid as well" and "there is also an invisible
  wall preventing from going there"). Details in Research_Ground_Level_2026-11-01.md sections 23 and 24.
  - With Solid street on: the station's floors, landings and the plaza round it are held by Street level boxes; the
    station's three kill boxes spare Faith, and the Shard level's box over its east roof does while she's in the station.
  - Sky City's base block (a box under the whole of Sky City that filled the station's lowest floor, pools and hall) now
    ends at the hall's east end, so the lowest floor, the pools (walkable water) and the shops can be reached; Street
    level boxes copy the old block everywhere else. The convex shape that was the invisible wall at the north walkway's
    doorway is shrunk to a 4 cm copy inside Sky City's building, and Street level boxes hold the roof and walls it held.
    No fall death in the station's sunken part. Both shapes go back as the game has them once Solid street is off and
    Faith is out of the space they opened (Street level stays on till then, and the menu line says so).
  - Still as the game has it: an invisible ceiling 1.6 - 2.5 m over the last 7 - 11 m of the hall's east end (Sky City's
    building), the lower terrace's east end, and the shop fronts (flat pictures).
  - Builds: A97DA31332D9 (11-09 23:22, section 23's GroundLevel.cpp and GroundProps.bin v12), then BDED5CA8E39A (11-10
    01:15, build.ps1 at Idle, GroundLevel.cpp, GroundLevel.h, CameraManager.cpp and GroundProps.bin v13, 151,476,818
    bytes). BDED5CA8E39A INSTALLED 01:16 (game folder, dist, x64\Release). Offline tests pass. In game (11-10 03:13):
    the station's kill box spared Faith, and the base block and the wedge "changed in the game's collision"; the
    opened hall wasn't walked through.

- **Fixed: GAME > Pose, free-moved bones staying moved after Let go** (the user, 2026-11-09 evening: "free moved bones
  don't return to their original position fix it").
  - Build A97DA31332D9 (11-09 23:22, Idle priority). It's the whole folder: FCEB05D7D7CC, plus Camera/PoseEditor.cpp,
    plus the ground-level session's Camera/GroundLevel.cpp b1d0ac305c1c (22:22) and Resources/GroundProps.bin
    169398043477 (22:41). That session's notes had those finished and waiting for a build.
  - INSTALLED 23:22 with the game closed (game folder and dist, hash checked twice). **Not tested in game.**
  - **Why [offline]:** the game blends a pose override into Ant's own records, in place (0x142FCF620, section 19 of the
    research). Ant rewrites only the channels it animates, and a bone's translation usually isn't one of them (nor was
    the size override's scale, which "the game never puts back").
    - So after Let go a free-moved bone kept the pose's place.
    - The next Take pose started from there, so Reset couldn't bring it back either.
    - A turned bone whose rotation the game doesn't animate (some face and helper bones) stayed turned the same way.
  - **Idle movement made it worse [offline]:** its capture read those channels back as if they were the game's
    animation. That was the tools' own previous value, so the movement fed on itself there.
  - **The fix:** the blend hook is now made at Take pose, after 13 pieces of the game's code are checked byte for byte.
    For each held pose and slot (the body; Faith's box pose), it keeps two things per held bone:
    - **Seen:** the record as the slot's last pass left it.
    - **Game:** the game's own latest value. At first that's the record just before the pose's first blend. After that,
      it's any channel (scale, rotation or translation) the game wrote since a pass.
    - A channel that is still bit for bit as Seen at a pass's start is one the game didn't write since.
  - **Let go:** the override goes out at once. At the slot's next pass (the game's own, with no override), each such
    channel is put back as in Game, before the game's blend.
    - Bones the game animates keep the game's latest values. The rest get what they had before the pose.
    - The pose shows as "being let go" until each slot has had that pass, which with time running is within about
      1/30 s. It can't be edited meanwhile.
    - While the game doesn't animate the body (far away), it waits, and the menu says so. It ends when the game animates
      the body again, when the character or its slot goes, or when the body is dead (then the channels stay as posed,
      logged). Let go of all ends a waiting one at once, as posed.
    - If the game animates the body but no pass is recognised within 3 s, it's let go as posed, and the log says so.
  - **The same while the override is out for other reasons:** a pose stepping aside for a replay (recording or
    playing), or a slot that another override holds. The game's whole animation shows meanwhile. The pose comes back as
    it was.
  - **Idle movement** leaves those channels out: no movement of their own, and their reference stays as it was. A pass
    where the game wrote nothing (time frozen) is no capture. The capture copies the game's memory before its seqlock
    opens.
  - **Reviewed:** two adversarial rounds (workflows wf_2fc551a2-3f2: 3 lenses with a skeptic for each finding;
    wf_950bc545-778: 2 lenses). Fixed:
    - the Let go timer could wrap round in its first frame and give up at once;
    - a body the game wasn't updating was given up on after 3 s (now it waits; a dead one is let go);
    - a fault in the idle capture could leave its seqlock open;
    - the slot's "blended" flag was cleared for a moment on every pass, so a Let go at that moment skipped the put back;
    - a pose waiting to be put back was hidden and couldn't be ended.
    Changed after round 1: what's put back is the game's own latest value of each channel, not its value at Take pose.
    A skeptic found that this matters little with time running (the blend runs about 30 times a second, at the game's
    tick). It does matter when time ran and was then frozen.
  - **Checked live, read-only (23:0x):** Faith's slot records pointer stayed the same in 1,283 samples over 3 s. The
    hook matches a pass without our override by that pointer and its manager.
  - **Log:** "Pose: ... let go (the menu's Let go): N of its bones' channels the game hadn't written since the pose put
    back as the game last had them, M of them more than 0.0001 off (its body's records: R)". Also: "let go while the
    game doesn't animate its body: its bones are put back when it does", and a warning if a slot's records were ever
    found somewhere else.
  - **Not known until tested:**
    - Whether it looks right in game.
    - Whether the game updates a body with no override while time is frozen. If not, Let go while frozen gives up after
      3 s and leaves those channels as posed (logged).
  - Bones moved by an earlier build stay as they are until the level reloads (not checked). The records the hook copies
    before the first blend already hold the moved values.
  - **Not covered:** the weapons' own override (bone 1 of a gun's body). Whether the game animates that bone, and so
    whether a gun moved by hand stays there after Let go, isn't known.
  - An earlier draft of this fix (the override handed at weight 0 for a few frames) was compiled at 21:37 and never
    reviewed. The other sessions' folder builds 58d5e3b11014 (21:40) and FCEB05D7D7CC (22:04) contain it; this build
    replaces it.
  - `Camera/PoseEditor.cpp`: BlendWatch, Watch / Unwatch / SetPutBack, Release / KeepLettingGo, BeforeBlend / SeeBlend /
    PutBackChannels / AfterBlend in the blend hook, CaptureIdle's kept channels, UpdateIdle, the menu. Research: section
    20 of `Research_Pose_Editor_2026-11-08.md` (outside the repo).

- **The free camera also leaves out the heat haze, glass shards, runner's-flow streaks, Faith's movement looks, lens
  flares, lens reflections and lens dirt** (the user, 2026-11-09: "Hide them too"). Build FCEB05D7D7CC (22:04, Idle
  priority; the menu tidy and the walker fix are in it too), STAGED: it goes into the game folder and dist when the game
  closes (over older files only, hash checked again 30 s later). **Not tested in game.** With the free camera off the
  game is as before.
  - **5 more particle emitters (now 21 of the game's 863):** the heat haze when running through heat
    (Em_Ambient_Heat_L/M_RunThrogh), the glass shards (Em_Glass_GPMWindow_SecondaryHits_Shards_1_Release_Green) and
    the runner's-flow speed lines (Em_RunnersFlow_Camprox_Flow, _Streak). Same way as the lens drops: the game is told
    they have nothing to draw.
  - **12 more of Faith's movement looks (visual environments, from her FaithCharacter_Movement_VEs):** the speed's
    motion blur, the wallrun's (depth of field; motion blur and vignette), close focus, falling, dash, slide, skill
    roll, MagRope (two), and the runner's-flow colour and tonemap (VE_RunnersFlow_Add and _Sub, together). Left out of
    the game's mix, as her hit effects and hard landing already were.
  - **Kept:** VE_RunnersFlow itself. Its only part feeds SurgeIntensity, which only the world's and people's material
    shaders read (a glow on them while she's in flow), so it isn't on the screen. Found by the review; the first
    build of this had it left out.
  - **Lens dirt and lens reflections:** after the game's mix, the bloom's lens-dirt texture is cleared (the game's own
    no-dirt tonemap) and the 4 lens reflections are switched off. With the camera off the game mixes again and they're
    back the same frame.
  - **Lens flares, the sun's included:** while the camera is on, the game's flare draw skips its draws by its own way
    out. The flares' visibility checks still run, so they're right the frame the camera goes off.
  - **Unknown:** whether the tools can name Faith's movement looks (they're made inside her prefabs). If they can't,
    those still show, and CT.log says so once each: "Free camera (diagnostic): a visual environment in the picture the
    tools can't name (its data ..., visibility ..., blend mode N, parts 0x... / 0x...): not left out".
  - Reviewed by two read-only agents against the game's code: the flare skip's bytes, MinHook's copy of the game's
    instruction, the way out, the look offsets and the 21 names. No crash found.

- **Menu tidy: fewer (?), more always-on settings** (the user's list, 2026-11-09).
  - **CAMERA**:
    - *Show where each keyframe looks* is always on with the track's path. It has no tick and is no longer saved
      (`ShowKeyframeViews`).
    - No (?) on *Show the track's path*, *Outline size*, *Preview the track on its path* or *Control motion blur*.
    - *Start at the game camera* is now *Start at Faith's camera*.
    - *Hear from the camera* and *World sounds near the camera* are always on. They have no ticks and are no longer
      saved (`HearFromCamera`, `WorldSoundsAtCamera`).
    - No (?) on *Orbit*, *Hand-held shake*, *Shake kind*, *Letterbox* or the guide lines.
    - *Speed by height* is right under *Heavy camera*.
    - *Stop at walls* is now *Camera Collision*, with no (?). Its saved key is unchanged.
    - The *Impact* button and *Impact strength* are gone from the menu.
  - **GAME > Pose**:
    - *Her hair moves while paused* and *Weapons go with the hands* are always on, with no ticks.
    - No (?) on *A dragged bone bends*, *Helper bones*, *Bones stop at walls* or *Idle movement*.
    - *Free move*'s (?) is two sentences.
  - **GAME > Detail**: no (?) on *Fix LOD*, *Show small objects far away*, *Show the vistas* or *Shard office blinds*.
  - **GAME > Level streaming** and **GAME > Sun shadows**: no (?) at all.
  - **FOLLOW**: no (?) on *Follow Faith*.
  - **REPLAY**:
    - The tab's intro text ("Records Faith's movement ...") is gone.
    - No (?) on *Faith's sounds*, *Time of day and weather as recorded*, *Faith's outfit as recorded* or *Play
      along with the camera track*. When outfits aren't available, a plain line says so.
  - **MISC**: no (?) on *Text size* or *Mouse cursor*.
  - Every item was checked in the code by read-only agents against copies of the files from before; none was
    missing or wrong.
  - Build 58d5e3b11014, built at Idle priority, without errors; never installed itself. The pose session's later
    A97DA31332D9 (23:22, from the same folder) is installed in the game folder and dist and holds all of it
    (checked). Not checked in game yet.

- **Walking people kept out of the walls beside their paths** (the user, 2026-11-09: "make sure that pedestrians
  don't walk inside walls"). In the shared folder 18:42 (Camera/WalkersWalls.h new, WalkersCore.h, RoadTraffic.cpp);
  In the snow session's build FCEB05D7D7CC (22:04, Idle, the whole folder), STAGED to install when the game closes
  (game folder + dist, over older files only, hash checked again 30 s later). **Not tested in game.** CT.log at start:
  "... kept out of the walls beside their paths (1101 path segments, 130755 samples)".
  - **Why they were in walls:** no walker code checks collision, the game's or ours. The game puts each walker
    somewhere across its path's whole width, and many paths are wider than the walkway. Live, 6.6% of shown walkers
    were in a drawn wall. 58% of those come from that width, 20% from paths whose line runs through a building, 17%
    from the game's corner cut, and 3% from our sidestep.
  - **The fix:** a table made offline from the game's collision. For every walking path segment, every 0.25 m and
    on both sides, it gives how far is clear from the line and where the first obstacle ends; it was checked against
    an independent brute force and against the live walkers. A walker whose body is in that first obstacle is pulled
    back toward its line:
    - at most 0.6 m/s in sight (slower than a sidestep), 3 m/s out of sight;
    - only where it won't come too close to another walker;
    - corner targets in a wall move to the wall's face.
    - It never pushes a walker away from its line or holds it. Walkers that are clear, past an obstacle (the far
      side of a pillar), or on a path through a building are left exactly as now.
  - **Offline** (twice the walkers, 240 s, 6 runs):
    - bodies more than 5 cm into a wall went from 5.4 - 5.7% of walker-frames to 0.35 - 0.43%;
    - standing still and the longest stands are unchanged;
    - pairs closer than 0.5 m fell;
    - walkers on paths with no wall came out the same every frame.
  - **Left as the game has it:** paths whose line runs through a building (about 3% of walker-frames), and walls the
    collision decode doesn't have (compressed meshes, prefab physics, doors).

- **The free camera no longer shows the game's rain drops and water on the lens** (the user, 2026-11-09, through the
  weather session: "Freecam shouldn't get affected by on screen effects like rain droplets"). Build 066B83939038 (17:33),
  INSTALLED with the game closed (game folder + dist, checked 30 s later). **Not tested in game.**
  - **What draws them** (read from the game's data, all 863 particle emitter templates): 16 emitters, the ones named
    "OnScreen" or "CamDroplets": the rain's drops on the lens, the waterfalls' and wall streams' water, foam, splash
    and drops on the screen, the drips' and leaks' drops, and the drops from Faith's steps, landings and slides in
    water. There's no separate "wet lens" screen pass in the game.
  - **Now:** while the free camera is on, the game is told those 16 have nothing to draw (its own per-emitter check
    of what each view draws), so drops already on the lens go at once. Nothing of the game's is changed; with the
    free camera off the game draws them as before. Faith's hard-landing screen effect (a red-damage flash with colour
    fringes) is now left out too, as her hit effects already were.
  - **Left as they are, to decide:** the heat haze when running through heat, the glass shards from one tutorial wall,
    the runner's-flow speed lines, Faith's other movement looks (speed, wallrun, falling, dash, slide, roll, MagRope,
    close-focus blur), and lens flares with the bloom's lens dirt.
  - New CT.log line every 10 s while it hides some: "Free camera: the game's drops and water on the lens left out of
    the picture (N emitter draws in the last 10 s)".

- **The track's path: keyframe nodes stay in place as the camera moves** (the user, 2026-11-09: "Camera nodes should
  stay in place and not follow the camera"). Two causes, both on CAMERA > *Show the track's path*:
  - Each keyframe's camera outline was sized from the free camera's distance, to stay about 2.4 lines of text long on
    screen. So its body, its "up" mark and the picked one's aim line grew, shrank and slid in the world whenever the
    camera moved or only turned. Now every outline (the pink preview camera's too) is one fixed size in the world,
    set by the new *Outline size* under *Show where each keyframe looks* (0.3 m by default, 0.1 - 5 m; config
    `[Camera] KeyframeOutlineSize`). It looks bigger as you come closer and nothing else changes it. It's hidden while
    the camera is nearer to it than twice its size, and its edges are cut half a metre in front of the camera.
    - The 13:03 build sized each outline from the distance to its nearest keyframe, so it grew as you dragged its
      keyframe away (the user: "the thing that shows where the camera is facing gets bigger with every move of the
      node").
    - The 13:28 build made them 2 m (config `KeyframeViewSize`, now ignored). A 2 m outline a few metres away, pointing
      towards the camera, reached almost to it: its near side swept across the picture with every camera move and its
      edges, cut 11 cm in front of the camera, shot across the screen (the user: "they move again with the camera ...
      the camera things are still huge"). The dots and the path were drawn exactly as in the 13:03 build.
  - While a dot, curve handle or arch was held with the mouse, it was kept under the cursor every frame. So moving
    the camera meanwhile (keys, a gamepad, orbit, aim, following, a heavy camera coasting) carried the keyframe
    with the camera, and it was left there when you let go. Now, whenever the held point's place on screen moves by
    itself (the camera moves, turns or zooms, or the point an orbit keyframe goes round moves), it's taken again from
    where it is. So it stays put in the world and only the mouse moves it. While it's behind the camera nothing moves.
  - Ruled out (checked in the code and the 12:13 - 12:26 log): the focus and orbit pivots (no orbit keyframes that
    session), FOLLOW, the projection and its timing (the game's camera hadn't moved on by Present in any of about
    3800 frames measured), Panini (off) and lens distortion (not drawn).
  - Checked by three independent code reviews before the fix and three after it. The user confirmed in game (13:2x)
    that the nodes stay where they should with 6f080ac50012. The small outlines are build 09b21433dac8 (14:53:25, at
    Idle), INSTALLED at the 14:55 game close (game folder + dist, hash checked 30 s later). Not checked in game yet.
- **Walking people no longer walk off their way into the air at sharp turns** (the user, 2026-11-09: "check if you can
  make pedestrians properly stick to the ground some of their pathing is on the air for some of them"). Build
  47766314315C, built 11:46:14 at Idle from the shared folder; only WalkersCore.h and RoadTraffic.cpp changed since
  C2622CA0C52D. INSTALLED with the game closed (game folder + dist, hash checked 30 s later). **Not tested in game.**
  - **What the game does** (decompiled and read live, read-only):
    - A walker's height is a straight line toward its next corner's height. Nothing puts walkers on the ground.
    - The crowds' "terrain snap" job is never run, it's switched off on every walking placement, and with no terrain
      in the city its height would be 0. It can't be used.
  - **The cause fixed here:** the game puts a walker's corner where its two legs' side-offset lines cross. At a turn of
    nearly 180 degrees that point is tens to hundreds of metres away, and the walker walks straight off its path
    toward it, holding its height: in the air beside raised walkways. Live, 138 of 4,926 shown walkers had such a
    target, and 11 of them were 7 - 18 m above the ground.
  - **The fix:** a corner target further than two of its node's radii + 0.5 m from the node becomes the game's own
    point for parallel legs: the node, offset to the walker's side. Only turns over 120 degrees can have one.
  - **Offline** (the walker test, twice the walkers):
    - The 85 placements with no turn over 115 degrees come out the same, every record in every frame.
    - Far targets went to 0.
    - Over 20 minutes, walkers more than 10 m off their path in the second 10 minutes went from 219,228 walker-frames
      to 104. More than 3 m off went from 298,236 to 35,769: the rest is the game's own corner cutting on wide paths.
    - Walkers inside each other are unchanged.
  - CT.log's walker line now says how many corner targets were put back on the path.
  - **Not fixed yet:** path points that sit 0.3 - 1.2 m above the floor (about 0.8 km of paths, placement 181 the
    largest), and heights between corners that cut across steps or curved ramps.

- **Changed: the rain's puddles a little more visible** (the user, 2026-11-08 evening: "can we make the puddles from
  rain a little more visible? tiny bit like 10%").
  - What a puddle adds over the wet ground round it is its darkening of the ground's colour and its smoothness and flat
    mirror. The darkening is now x 0.67 instead of x 0.7, so a puddle's colour differs from the wet ground's 10 % more
    (1 - 0.33 x its share instead of 1 - 0.30 x). Its smoothness and mirror are unchanged.
    - In the game's own lighting (VISUALS > Weather > Wet surfaces > "In the game's own lighting", on by default):
      `Visuals/GbufferWet.hlsl`, GbufferWetPs.
    - With that off (the tools' own at Present): `Visuals/WetLook.hlsl` PsMain, in both the full and the rain-only
      shader. Its puddle sheen also goes from 0.4 to 0.44.
  - Checked before the change: all 12 compiled wet-look headers were byte-identical to their sources, compiled again
    with fxc 10.0.22000 at Idle. After it, only GbufferWetPs.h, WetLookPs.h and WetLookRainPs.h differ, with the same
    temps and instruction counts. Their disassembly differs only in those constants (-0.30 → -0.33, 0.40 → 0.44).
    Puddle shapes, coverage, the Puddles slider and Faith's steps in them are untouched.
  - Build 209F32FAE109, made by the tattoo session at 23:42:47 (Idle, whole folder, after these files), is INSTALLED in the
    game folder and dist (23:43:58). I checked its DLL: each of the three new shaders' bytecode is in it once, and none
    of the old ones. **Not tested in game.**
- **New: GAME > Faith > "No MagRope", "No bag", "Add the gridlink"** (the user, 2026-11-08 ~19:20: "add options to
  remove the mag as well and every accessory on her and a gridlick add tick").
  - Build 209F32FAE109 (11-08 23:42, Idle, whole folder, with the puddle shaders the parent session changed at 23:25 -
    23:30): INSTALLED 23:43 with the game closed (game folder + dist, hash checked after the copy and 30 s later). It
    replaces 7C420305FC40 (23:22, installed 23:24), which didn't yet have the last review fix below or the shaders.
    **Not tested in game.**
  - **Reviewed twice by agents** (6, then 2) before the install: the train ride and no-op rebuild issues they found
    are fixed. Last fix: if C_Faith_Prologue's gridlink groups can't be found, a tick change no longer rebuilds her
    parts for nothing.
  - **Her accessories [the game's data, read offline, checked by a second reader]:** the separate worn parts are the
    MagRope (the gauntlet on her right forearm, and the module on it: Unlock_Faith_MagR_Pickup_1 / 2 / 3, one per
    story stage, 3 in free roam), her cross-body sling bag (Unlock_Faith_Toolbag) and the gridlink (a band on her LEFT
    wrist, only in C_Faith_Prologue). Each has a third- and a first-person unlock.
    - The module rests on the gauntlet, so No MagRope takes both off. The arm skin under the gauntlet and the top under
      the bag's strap are complete (no holes).
    - Can't be taken off: her right half-finger glove (a section of the arms mesh, with no palm skin under it), the
      sleeve at her right elbow (a section of the top, filling a hole in the arm skin) and her shoes (part of the
      trousers section). "Headparts" is her lashes, teeth and eyes, not an accessory.
    - The gridlink unlocks, mesh and textures are in the main city's own bundle and its network registry. In bind pose
      the band sits 2.2 mm or more clear of her free-roam left forearm (clipping in motion not checked).
  - **How [the game's code, decompiled and checked twice]:** the game gives a character an outfit's parts in
    applyCustomization (0x14336CF00, the character's vtable + 0x340; through 0x14336C860 it takes one unlock from each
    of the outfit's VisualGroups). With a tick on, the tools hook it and hand the game, for that call, a copy of the
    outfit (on the hook's stack) whose group list leaves out the MagRope or bag groups, or adds C_Faith_Prologue's two
    gridlink groups. Only the game's own groups and unlocks are handed over.
    - Nothing keeps the copy, its list or the args after the call, and nothing is ref-counted on the way. The server
      sends her part set to the client, which rebuilds all her parts from it.
    - Only for a character with a player (Faith), and only for the spawner's seven outfits, plus The Shard train ride's
      own C_Faith_NoTat_MagRope3_NPC_InGHair (parts taken off only, no gridlink added).
    - Her very first outfit (the game keeps it as her "original", for a later restore) is given as it is and changed at
      the next server update.
    - A tick change dresses her again in the spawner's outfit she wears, the way the tattoo tick does. It doesn't if
      her parts wouldn't change (each rebuild restarts her hair), or if her last outfit is a mission's own (it would be
      replaced): the ticks then wait for the game's next outfit.
    - Code checks at start: the hook's 17 bytes, both vtable slots, 6 places in 0x14336C860 and the outfit data's
      vtable. If any differs, the ticks show "not available (see CT.log)".
  - **Saved** (`[Misc] NoMagRope`, `NoBag`, `AddGridlink`, off by default). The replay doesn't record parts: in
    playback they're as the ticks are then.
  - **Log:** "Faith's parts: ... ticked", "Faith's parts: C_Faith_WithTattoo given with N of its 15 parts left out and
    N added (...)", "Faith's outfit: dressed again (her parts) in ...", and why she isn't dressed again when she isn't.
  - **Not known:**
    - Nothing was seen in game: the looks, clipping in motion and the first-person view.
    - Whether MagRope gameplay or effects (the cable) look odd without the gauntlet. Only the drawn parts change; the
      MagRope's abilities are in her character, not her outfit.
    - Whether a save stores her edited part set (the server component can write "ActiveVisualAssets"). The spawner
      dresses her again at each spawn, so the ticks apply again then.
    - The game's random pick for characters' outfits (one draw per group) is drawn fewer or more times, so random NPC
      looks can come out differently.
  - `Camera/FaithOutfit.*` (the hook, its checks, SetPart / GetPart, Dress's redress), `Camera/CameraManager.cpp` (the
    ticks under No arm tattoo, the config). Research: `Research_Faith_Parts_2026-11-08.md` (outside the repo).

- **New: GAME > Faith > "No arm tattoo"** (the user, 2026-11-08: "add an option to remove faiths arm tatto in the faith
  option (pretty sure it's a faith texture that switches)", "make no mistakes").
  - Build DA66C4674E51 (11-08 15:12, Idle, whole folder, after the pose session's 15:06 build): STAGED. It installs
    once the game closes, over older builds only, hash checked after the copy and 30 s later (stager pid 30564).
    It went in at 15:34 (then the pose session's newer whole-folder builds, which hold it). **Used in game 16:10:**
    CT.log shows the tick on and off and the tools' Apply sent each time; how it looked wasn't reported.
  - **How the tattoo is drawn [the game's data, read offline]:** a texture switch, as the user thought. It isn't baked
    into one skin texture with no switch, a separate mesh part, a decal or a shader parameter.
    - Her arms are one mesh, `characters/faith/arms/faith_arms_mesh`. Its skin section (SkinUpper) takes its colour,
      normal and SSS maps from `t_arms_dao`, `t_arms_nrs` and `t_arms_ex`, which hold the ink. The mesh variation
      `Faith_Arms_NoTattoo` (NameHash 4240144052, in the main city's MeshVariationDb) swaps them for
      `t_arms_notattoo_dao / _nrs / _ex`.
    - Decoded and compared: the two colour maps (BC7 and BC3, 2048 x 2048) differ only in the tattoo, in one arm's
      part of the texture (pixels x 356 – 625, y 174 – 1097; the AO in alpha is the same). The SSS maps differ only in
      its shape. SkinUpper's UVs cover 60,840 of the tattoo's 60,847 pixels, in every LOD.
    - The other meshes using those maps show none of it. Her top's arm guard (faith_upperbody, gloveArmguard) uses
      t_arms_dao, but its UVs touch no tattoo pixel: they sit on the dark band, which is the same in both maps. The
      Prologue top's skin uses t_arms_ex for the hands' part only, and touches none of it either.
  - **What picks it [data; the code path is the outfit research's of 2026-10-26]:** her outfit. The spawner
    pf_spspawn_01 dresses her in one of seven outfits by a story flag. Only C_Faith_WithTattoo (flag 7, set at The
    Shard: free roam after the story) has the tattoo arms (Unlock_Faith_Arms / Arms1p: the mesh's own materials).
    - C_Faith_NoTat_MagRope3 (flag 6) has the same 13 other parts, health values, impact materials and game state write.
      Its arms are Unlock_Faith_Arms_NoTattoo / Arms1p_NoTattoo: the same mesh with the no-tattoo variation.
    - Both outfit entities link to the same spawn reference (Faith). Everything they use is in the main city's level
      bundle, loaded with the city.
    - The user's CT.log (14:47:22) has the game dressing her in C_Faith_WithTattoo.
  - **The tick:** wherever she'd wear C_Faith_WithTattoo, the tools dress her in C_Faith_NoTat_MagRope3 instead. That
    covers the game's own outfit and a replay's recorded one.
    - It sends that outfit's own Apply at the next server update, the way the Outfit menu dresses her. Unticked,
      C_Faith_WithTattoo's own Apply puts the tattoo back.
    - When the game dresses her itself (a level load, a respawn), she has the tattoo until that next server update.
    - The other outfits have no tattoo and are left alone. The story flag and the save aren't touched.
  - **Saved** (`[Misc] NoArmTattoo`, off by default). It can be changed while a replay records or plays. A recording
    made with the tick notes the tattoo outfit, so in playback the tattoo shows or not as the tick is then.
  - **Log:** "Faith's outfit: arm tattoo off ...", then "Faith's outfit: dressed in C_Faith_NoTat_MagRope3, ..., in
    place of C_Faith_WithTattoo: the arm tattoo off (Apply sent ...)".
  - **Not known:**
    - The tools' own Apply hasn't been seen working in game: no CT.log so far has a "dressed in ... (Apply sent" line.
      The game's own Apply is seen. If it fails, she keeps the tattoo.
    - How the engine turns the arms' MeshAndVariationPair into the variation's materials wasn't traced. The tick only
      sends the game's own Apply on an outfit the story itself uses before The Shard.
    - Whether the no-tattoo textures show sharp at once or stream in.
  - `Camera/FaithOutfit.*`: SetNoTattoo / GetNoTattoo, the twin in WantedOutfit, WornOutfit telling the twin as the
    tattoo outfit. `Camera/CameraManager.cpp`: the tick under Outfit, the config. Research:
    `Research_Faith_Tattoo_2026-11-08.md` (outside the repo).

- **GAME > Pose: "Idle movement"** (the user, 2026-11-09 ~04:00: "can we add a tick that brings back her micro movements
  (idle animation) ?").
  - Build C2622CA0C52D (11-09 04:35, Idle, whole folder = 209F32FAE109 + Camera/PoseEditor.cpp, and the ground-level
    session's unbuilt fence-gate sources: Camera/CameraManager.cpp 03:59, GroundLevel.cpp 04:20, GroundLevel.h 03:58;
    that session didn't answer, and its notes had the work finished and reviewed). INSTALLED 04:35 with the game closed
    (game folder and dist, hash checked twice). **Not tested in game.**
  - **Reviewed:** an adversarial round (3 lenses, a skeptic for each finding). Fixed:
    - a stale capture after a re-take in the same held slot (now a generation per wanting, and a reference per bone);
    - a shiver at step rate on walking characters (now a gate);
    - a pop on untick (now a 0.4 s fade).
    The blend turned out to write in place (no failure found); a guard skips a capture that is the tools' own pose read
    back.
  - **Fades out** while the character walks, runs or fights: any body bone more than 10 degrees off its smoothed
    animation, or its root faster than 0.3 m/s. It fades back in when the character stands again.
  - **Why a pose has none [offline, 2026-11-09]:** a held pose is the game's own pose override at weight 1, so it
    replaces the character's animation (breathing, small shifts of weight, blinks) on every bone it holds.
  - **Where the animation is still there:** the game blends an override into Ant's output in 0x142FCF620, which runs in
    the pose job (its only call, 0x142FCC9A7).
    - For each listed bone it reads Ant's record at [item + 0xA0] + map[bone] x 0x30 (scale, rotation at + 0x10,
      translation at + 0x20). The map is the third argument's + 0x20; the list is item + 0x158, the count + 0x14C.
    - It writes the blend to another buffer. So at the function's entry, those records are the animation as the game
      made it that frame, under the pose.
  - **The tick** (off by default, not saved; with an Amount slider, 0 – 2):
    - A hook at the blend's entry copies the held bones' records, only for held poses with the tick on. It matches by
      the override's bone buffer (item + 0x130) and the body's slot (item + 0x150; Faith's box pose shares the
      buffers). The other characters' blends just pass through.
    - It's made the first time the tick is used, after 8 pieces of the function and its call are checked byte for
      byte.
    - Each frame (game thread), each held bone's animation is smoothed over 3 s. Its movement about that (a turn in
      the bone's own frame, a move in its parent's) is laid over your pose, times Amount, at most 15 degrees and 8 cm.
    - So the idle's small movements come back on top of the pose, but not where the animation has the bones.
    - The bones' dots still show the pose without it.
  - **Only with time running:** Freeze time stops the game's animation, so the movement fades out then.
  - **Log:** "Pose: the game's pose blend hooked (Idle movement)" once, then every 10 s per pose: "idle movement from N
    of the game's animation frames under the pose".
  - **Not known until tested:**
    - How it looks.
    - Whether a running KrugerSec's walk, which the 15 degree / 8 cm cap keeps small, looks like a shiver.

- **GAME > Pose: weapons really follow the posed hand, can be posed on their own, and the Hips move without a limit**
  (the user, 2026-11-08 ~18:00: "Looks like i still can't move the guns with the hand or at their own fix it", then
  "also remove the limitation space moving the hips bone -2 and 2 is way to little it make it infinite").
  - Build 37C6FD89E9EF (11-08 19:01, Idle, whole folder = 9FA79FFA9920 + Camera/PoseEditor.cpp): INSTALLED with the
    game closed (game folder and dist, hash checked after the copy and 30 s later). Since replaced by the tattoo
    session's 209F32FAE109 (23:42, whole folder, which holds it).
  - **Tested by the user (2026-11-09 ~04:00): "no problems yet with the guns everything works fine".**
  - **Why 9FA79FFA9920 didn't move them [live, targeted read-only reads, 17:45, time frozen]:**
    - The baton's entity was at the posed grip (0.000 m). That part worked.
    - Its body's local bone 1, world pose and 3x4 matrices stayed 14.5 cm away, where the weapon was before. Nothing
      the game runs while frozen copies a moved entity into a weapon's pose: for the baton (body + 0xB8 = 0) the
      placement only clears the pose's placement flags (0x1432EA220 -> 0x142FD0830) [offline].
    - CT.log 17:49, time running: the baton was still 20 – 79 cm and the shotgun 89 cm from the posed grips.
  - **What the tools do now:** they draw each weapon's body themselves, the way the replay draws bodies the game doesn't
    update (FaithReplay.cpp WriteBody).
    - When the pose takes the weapon, its slot buffers are kept.
    - Then, every frame at the camera hook (after the frame's pose jobs, where the tools' writes are drawn: Faith's size
      test), the tools write a rigid move of the kept pose to where the weapon is wanted. That covers local bone 1, bone
      1 of both animation steps (page + 0x18 / + 0x20, local layout) and the world pose and matrices of bones 1 – n.
    - The weapon's body also gets the game's own pose override (bone 1 where it's wanted, weight 1, two buffers in
      turn, the way posed characters get theirs). With time running, a game pose job that rebuilds the weapon after the
      tools' write then rebuilds it there too. If the game rebuilds the weapon's buffers, the kept snapshot is taken
      afresh from them, so the weapon's own animation goes on.
    - Its entity is still moved too, with the game's own moves held back.
    - The body's update isn't held. A held body goes on the game's list of bodies it doesn't update, and nothing was
      found that puts one with a pose slot back (the replay's guns, 2026-10-24).
    - The weapon follows the hand as the game drew it that frame (the world pose the pose was handed in), so it doesn't
      run a frame ahead of the hand.
    - When the weapon is let go (Let go, a replay, unticking), its kept buffers and its entity's rows are put back,
      but only into a weapon checked that frame, and only if time didn't run while the tools had it. If time did run,
      the game's next update puts the weapon in the hand. With time still frozen, it stays where the pose had it
      until time runs.
    - A weapon that's dropped or gone, or whose character is found gone, is left alone: nothing is written into it.
      Undo and Reset don't bring a dropped weapon back.
  - **Checked by an offline investigation with adversarial verifiers:**
    - The render-side entry at page + 0x40 points at the slot's two step buffers. It isn't a separate drawn copy, so
      it isn't written; an earlier note had read it as world-space.
    - A skinned draw takes the slot's matrices; a weapon is a skinned mesh (cpf_baton is a SkinnedMeshAsset).
  - **Posing a weapon on its own:** each weapon has a square on the picture (green, set apart from the hand's dot with a
    leader line).
    - Drag the square to move the weapon (Shift: nearer / farther, the wheel, right click cancels).
    - Click it to pick it and turn it with its rings (on its own axes with "Rings on the bone's own axes").
    - Its row under Editing picks it too. The weapon section there has:
      - **Goes with <bone>:** unticked, the weapon is put down and stays where it is in the world.
      - Turn X / Y / Z sliders, and a Move field with no limit.
      - Reset this weapon, and "Pose it where it is" for a weapon that wasn't on a bone when taken.
    - Undo and Reset all cover the weapons. Saved poses don't hold weapons.
    - The picked bone's, group's or weapon's sliders have IDs of their own. A number being typed into a field ends
      when the picture takes a click, so it no longer carries over to the next thing picked.
  - **Reviewed:** two adversarial review rounds (four lenses, a skeptic for each finding). The first round's
    confirmed defects are fixed: let-go writing into a possibly freed weapon, its entity's rows kept after the tools had
    moved it, a stale put-back after time ran, the weapon's own animation frozen, Undo bringing back a dropped weapon,
    stale log lines, and typed text carried to the next pick. The second round's are fixed too: the weapon's override
    left in its slot after a failed write, and an extreme typed Hips value stranding the weapon's entity.
  - **The Hips' move is as good as unlimited:** up to 100 km in each axis (was ± 2 m). The city is a few km across;
    a finite cap keeps the sums from overflowing (the second review round). Its Move is a drag field (drag sideways,
    or double-click and type a number), and dragging the Hips' dot isn't held to 2 m any more. Other bones keep 1 m,
    face bones 0.15 m.
  - **Shared code:** none changed. Util/Hooks.cpp / Util.h are as in 9FA79FFA9920: a pose-only body update hold was
    added during this work and taken out again.
  - **Log:**
    - "Pose: ...: its cpf_baton's body is drawn by the tools now (26 bones)".
    - Every 10 s: how far the game moved the weapon's local bone 1 and its bone 1's matrix since the tools' write the
      frame before. Both should be 0.0 cm. A matrix gap means a game job writes the matrices after the camera hook, and
      the weapon would flicker.
    - Every 10 s: how often the game's own moves of the weapons were held back.
  - **Not known until tested:**
    - Whether the drawn weapon follows. A gun's draw might not take the slot's matrices directly: in a 2026-10-27
      replay test, guns' slot matrices were 1.2 km from where they were drawn, cause unknown. The world pose is written
      too, which covers the other case.
    - Whether the weapon's culling box stays at its old place, which could cull a moved weapon at the screen's edge.
    - With time running, whether a game job writes the weapon's buffers after the camera hook (the matrix gap line).
  - Research: `Research_Pose_Editor_2026-11-08.md` section 18 (outside the repo).

- **GAME > Pose: "Weapons go with the hands"** (the user, 2026-11-08 16:0x: "if ksec holds batoon/gun etc it should
  move with them when moving the bone").
  - Build 9FA79FFA9920 (11-08 16:12, Idle, whole folder = 470DB0C0E4F4 + Camera/PoseEditor.cpp, Util/Hooks.cpp,
    Util/Util.h): STAGED. It installs about 15 s after the game closes, over older builds only, hash checked twice
    (stager pid 36128). Dist has it. **Not tested in game.**
  - **Read live (targeted, read-only), 2026-11-08 ~16:00, two posed KrugerSec with time frozen:**
    - Batons are weapons like guns: `PamClientCharacterWeaponEntity` (cpf_baton_Mesh; the other's cpf_shotgun_Mesh).
    - A weapon's pose has bone 1 at its entity's transform. Its local bone 1 is the entity's rows (as
      XMMatrixRotationQuaternion makes them). Its world bone 1 is at the same place, its rotation turned. Bone 2's
      world pose is bone 1's local one, as on a character.
    - The posed hands had left them behind: the shotgun was 0.34 m and the baton 1.03 m from the posed grips
      (Wep_Root). Which hand the game moves them with isn't known.
    - The shotgun's body had its placement on (+ 0xB8 = 1, no physics part). The baton's was off (+ 0xB8 = 0), so
      the body placement hook (PlaceBody) alone wouldn't move it.
  - **The tick** (ticked by default, not saved):
    - At Take pose, each of the character's weapons is put on a bone. If it's within 25 cm of the grip (Wep_Root), it
      goes with the grip. Otherwise it goes with the nearest body bone within 40 cm (no fingers, face or helper
      bones), e.g. a holster. Otherwise the game keeps it. The log's "holding" line lists each weapon, its bone and how
      far it was from that bone.
    - Each frame the pose is handed, the tools move the weapon's entity to that bone's posed place, as it was on the
      bone. This is the replay's move for guns (MoveActorDrawn: what's drawn moves, not its collision), and the game's
      own moves of it are held back. Its body's placement gets the same place (PlaceBody), which only counts for a
      body with its placement on.
    - It's let go (the game moves it again) when the pose is let go, while a replay records or plays, while the pose
      slot holds another override, when unticked, or when it's no longer that character's (dropped).
  - **Shared code: Util/Hooks.cpp and Util/Util.h.** The models' setTransform hooks have a second hold list, the
    pose's (`util::hooks::PoseHoldActor`, `PoseActorMovesHeld`). The replay's `ReleaseActors` leaves it alone, and the
    pose never takes the replay's holds. The setTransform body hook is on while either list holds something, and still
    never with MECMP in the game.
  - **Menu and log:**
    - A line per weapon under Editing: "Its cpf_baton (on Wep_Root): goes with the pose, N cm off it".
    - Every 10 s the log says how far each weapon is from where the pose has it. That shows whether the game draws it
      where it's moved, frozen and running.
  - **Not known until tested:**
    - Whether the game updates a weapon's pose from its moved entity while time is frozen.
    - Whether the turn of the weapon's world bone 1 holds when the tools set the rows.
  - Research: `Research_Pose_Editor_2026-11-08.md` section 17 (outside the repo).

- **GAME > Pose: "Her hair moves while paused"** (the user, 2026-11-08 15:0x: "faith's hair should be able to move
  when game is paused").
  - Build 470DB0C0E4F4 (11-08 15:25, Idle, whole folder: it holds Free move below and the tattoo session's
    DA66C4674E51 sources, CameraManager.cpp / FaithOutfit.* of 15:08 – 15:10): STAGED. It installs about 15 s after the
    game closes, over older builds only, hash checked twice (stager pid 29828). Dist has it. **Not tested in game.**
  - **Why her hair stopped [decompiled offline]:** the game steps every cloth from its physics step (EAClothSimUpdater
    slot 3, 0x142FA2720 -> the cloth manager's scheduler 0x142FA3A10). Freeze time sets the time scale to 0, so there
    are no physics steps. Each frame the game only blends the last two steps (EAClothUpdater, 0x142FA1960), which
    moves no particle, so a head posed while frozen left the hair where it was.
  - **The tick** (new file Camera/PausedHair.cpp/.h; ticked by default, not saved; above "No pose held" in GAME >
    Pose):
    - While Freeze time is on, the tools run the game's own two per-cloth steps for Faith's cloths only, on the game's
      thread. These are the pre-update 0x142FA2760 (her root's place) and the sim job's body 0x142FA07A0 (her
      anchors from her pose, the world's collision, the EA step, the drawn mesh).
    - The sim job is handed the tools' own job record, which lists only her cloths. Its step record is a copy of the
      game's last one, with the step's time set.
    - Everyone else's cloth stays frozen.
    - It steps once at least 1/60 s of real time has passed, at most 1/30 s a step. The EA step takes the time since its
      last step itself (+ 0xE0).
    - Only cloths the game itself would step: in its cloth list, at full detail, + 0x18B clear.
    - Never while the game's cloth jobs run (sim sync + 0xE0, interpolation sync + 0xF8): it waits for the next frame
      and never waits on a job itself.
    - 13 pieces of the game's code are checked byte for byte first. A fault in a step switches it off until the game
      restarts.
    - A line under the tick says what it does ("Stepping N cloths of hers (N steps)" or why not).
  - **Log:** "Paused hair: stepping N cloth(s) of Faith's while time is frozen ...", then every 10 s the steps and
    skips, and the totals on unfreezing.
  - **Not known until tested:**
    - Whether her hair visibly follows a head posed while frozen. Her anchors are made from her local pose, which takes
      the pose override while frozen, so it should.
    - How often the game's cloth jobs are still running when the camera hook comes (the log's skipped count).
    - How it looks at up to 60 steps a second (the game's own is 30 at 1x).
  - Research: `Research_Pose_Editor_2026-11-08.md` section 16 (outside the repo).

- **GAME > Pose: "Free move"** (the user, 2026-11-08 15:0x: "add a tick to free move every bone to any axis").
  - Build 102AEDF5C406 (11-08 15:06, Idle, whole folder; only PoseEditor.cpp changed since 014B40A404E4): it was
    STAGED at 15:07 and replaced, before the game closed, by the build above (which holds it). **Not tested in game.**
  - **Free move** (unticked by default): dragging any bone moves that bone itself, any way, instead of bending the
    bones above it. Every bone below it goes with it, and the mesh stretches between it and its parent.
    - The picked bone has red, green and blue arrows (X, Y, Z) in place of its rings. They're the bone's own axes or the
      world's, as "Rings on the bone's own axes" says. Drag one to move the bone along that axis only.
    - Every bone has Move sliders: up to 1 m, the Hips 2 m, face bones 0.15 m.
    - The eyes are moved too, not turned to look. "Bones stop at walls" still holds.

- **GAME > Pose: "Bones stop at walls", and the eyes look where they're dragged** (the user, 2026-11-08 14:3x: "add
  wall collision checks to the pose editor and a tick for it", then "make it be able to move her eyes as well").
  - Build 014B40A404E4 (11-08 14:58, Idle, whole folder; only PoseEditor.cpp changed since 90077703507A): STAGED. It
    installs about 15 s after the game closes, over older builds only, hash checked twice (stager pid 19384). Dist has
    it already. **Not tested in game.**
  - **Bones stop at walls** (ticked by default):
    - A dragged bone is a 5 cm ball moved from where it was last towards the cursor through the game's physics, with
      the free camera's sweep (collision::Sweep, filter 9). It stops at walls, floors and anything solid and slides
      along them.
    - The pose is solved for where it got to. If another main joint would go into something on its way (a 3 cm ball
      from where it was to where it'd be: the elbow, the knee, every joint when the Hips are dragged), the frame is
      left out and the pose stays as it was.
    - Not checked: the face, the fingers, helper bones, and turns with the rings or sliders.
    - With the tick, a drag is solved on the game's thread (where the sweeps can be made) at the camera hook, so the
      pose is handed the same frame.
  - **Its own body:** whether the game's sweep hits characters wasn't known. At a character's first drag with the
    tick, 4 rays go at its chest from 0.8 m away. If 3 or 4 touch something 0.1 – 0.55 m from its middle (its own
    capsule), what the sweeps touch within 0.5 m of where it stands is left out. The log says which ("Pose: … the
    world's sweeps hit / don't hit its own body").
  - **The eyes:**
    - Dragging the Eyes group (or one eye, or an iris, with the face ungrouped) turns them to look at a point. That
      point starts 0.5 m in front of them and moves twice as far as the cursor does at their distance. Both eyes look
      at it, so they turn in for a near one.
    - Each eye's gaze is taken as towards its iris, which sits 0.5 – 1.4 cm in front of the eye's middle in the bind
      poses of Faith's, KrugerSec's and the women's skeletons [data].

- **GAME > Pose: dragging bones reworked** (the user, 2026-11-08 14:1x: "Improve the UX for the dots ... not smooth at
  all also make it possible to move the bones back and forth also for some reason using a bone moves the whole body
  instead of that bone ... not rotate the whole body because you moved one bone fix it").
  - Build 90077703507A (11-08 14:23, Idle, whole folder): installed 14:24 with the game closed, game folder and dist,
    hash checked 30 s later. **Not tested in game.**
  - **The whole body doesn't turn any more.**
    - A drag never bends the Hips. It only bends bones of the dragged bone's own body part: an arm (from the shoulder),
      a leg (from the thigh), the spine, the neck (with the head), or a finger.
    - Before, dragging the spine or a thigh turned the Hips, which turned everything, and so did a chain that reached
      them.
    - A bone at the top of its part (the spine's first, a thigh, a shoulder, the neck, a finger's first) swings about
      its own joint instead: its farthest child goes the way the cursor goes.
    - Checked offline on Faith's and KrugerSec's skeletons, e.g. hand → forearm + arm, foot → knee + thigh, Spine2 →
      Spine1 + Spine, thigh / spine / neck / shoulder → swing.
  - **Nearer and farther:** while holding a dot, hold Shift and move the mouse up (farther) or down (nearer), along
    the view, from where it is; let go of Shift to move across the picture again from there, with no jump either way.
    The wheel still works. A hint by the cursor says so.
  - **Right click while holding a dot** puts it back as it was, and drops its Undo step.
  - **Cleaner dots:**
    - Helper bones are hidden unless "Helper bones" is ticked: rolls, twists, bends, bulges, arm pits, collar, holders,
      ankles, throat, the end bones and the gun grip. They sat on top of the elbows', wrists' and knees' dots and took
      their clicks.
    - The default picture now has 25 dots: hips, spine, neck, head, shoulders, arms, hands, legs, feet, toes.
    - Dots take the click before the rings do (the rings crossed the next joints' dots), and reach farther (0.75 font
      sizes).
    - The dot under the cursor or being dragged is bigger, with a white ring, and is drawn on top.

- **Fixed: GAME > Pose crashed the game when a bone was dragged** (the user, 2026-11-08 14:0x: "Dragging crashed the
  game"; CT.log 13:58: exception 0xC0000005 at CT_MirrorsEdge.dll+0x1BCB83 reading 0x6).
  - Build 40C26B9CE369 (11-08 14:03, Idle, whole folder): installed 14:03 with the game closed, game folder and dist,
    hash checked 30 s later. **Not tested in game.**
  - **What was wrong** (the crash's call chain resolved with the 1DC32C70D640 build's PDB, and that build's code read
    at the call):
    - ImDrawList::AddText was called from pose_editor::DrawOverlay with the text FaceGroupNames[g_SelectedGroup], read
      afresh at the call.
    - Pressing a bone's dot with a face group picked sets the group to -1 in the same frame. That happened after the
      overlay had checked that a group was picked, and before the label was drawn. FaceGroupNames[-1] read the 8 bytes
      before the table (0x6), and ImGui read the text there.
  - **Now:**
    - The label's checks are made again where it's drawn.
    - Taking a pose, letting one go, or switching to another held character clears a picked face group too.
    - Every other use of a picked or hovered bone or group was checked: each is bounds-checked where it's used.

- **GAME > Pose: "Group the face"** (the user, 2026-11-08 13:5x: "add a tick that groups most facial bones
  (chicks/mouth/eyes) etc.").
  - Build 1DC32C70D640 (11-08 13:55, Idle, whole folder): installed 13:55 with the game closed, game folder and dist,
    hash checked 30 s later. **Not tested in game.** (AF167597A31A, the bone dragging, had installed itself at 13:50
    when the game closed.)
  - **The groups:** Eyes, Upper eyelids, Lower eyelids, Eyebrows, Upper lip, Lower lip, Mouth corners, Cheeks, Nose.
    - Found by bone name under the Face bone. Faith's, KrugerSec's, the men's and the women's skeletons have the same
      57 face bones, and the groups take all but five of them.
    - Face, Jaw, Tongue, TongueTip and Chin stay single.
    - A grouped bone's children go with it: the inner lips, the irises, the mouth's inner corners and creases.
  - **With the tick on (the default):**
    - Each group has one diamond dot at the middle of its bones, and one row in the bone list.
    - Its sliders and rings turn each bone of the group the same way about its own place (the eyes look, the lids
      close).
    - Dragging its dot, or its Move sliders, moves them all the same way.
    - Face dots that would sit on top of each other are spread apart on the picture, with a faint line to where they
      really are. A drag still goes by the real place.
  - **Also:**
    - Face bones are moved when dragged, not bent like an arm, and have Move sliders (0.15 m at most). Every bone now
      has its own move; the Hips' still moves the whole body.
    - "Face and fingers" is now two ticks, "Face" and "Fingers".
    - Undo keeps the moves too.
    - A pose loaded onto the skeleton it was saved from sets every bone's translation; onto another skeleton, only the
      Hips'.

- **GAME > Pose: drag a bone anywhere with the mouse** (the user, 2026-11-08 13:4x: "make it to be able to grab the
  bone and move it with your mouse whereever you like").
  - Build AF167597A31A (11-08 13:46, Idle, whole folder): STAGED. It installs about 15 s after the game closes, over
    older builds only, hash checked twice (stager pid 35048). Dist has it already. **Not tested in game.**
  - **How it works:**
    - Press on a bone's dot and drag. The bone follows the cursor at its distance from the camera, and the mouse wheel
      moves it nearer or farther while held. A click without moving only picks the bone.
    - The bones above it bend so it gets there, as near as they reach. "A dragged bone bends" sets how many: 1 aims
      the bone above; 2 (the default) is an arm or a leg, bending at the elbow or knee the way it already bends; 3 or
      4 turn each in turn (cyclic coordinate descent).
    - Dragging the Hips moves the whole body. When a bone can't reach, a line shows where it was wanted.
    - Each frame it's solved again from the pose it was pressed in. One Undo step a drag.
    - The dots and rings are drawn from the pose as it is that frame, so they follow the mouse at once.
  - The two-bone solve was checked offline (scratchpad twobone_test.py): 2000 random reachable targets were hit
    exactly, the bend side never flipped, and an out-of-reach target gets the limb stretched straight toward it.
  - **The 13:39 build's test (1CCB0E0956EE, the log 13:43 – 13:47):** the game takes the pose with Freeze time on
    (1626 – 1759 of every 10 s's frames, 0.0 cm). With time running it took it in 1690 of 1690 and 1530 of 1530 when
    the pose was left alone; 1401 of 1502 and 1701 of 1923 when it changed in between (the most off 0.8 cm).

- **Fixed: GAME > Pose's bones not on the model** (the user, 2026-11-08 13:3x: "bones are not in the correct pivot as
  the model and it's really hard to animate").
  - Build 1CCB0E0956EE (11-08 13:35, Idle, whole folder): STAGED. It installs about 15 s after the game closes, over
    older builds only, hash checked twice (stager pid 30928, log in the pose session's scratchpad
    stage_install.log). Dist has it already. **Not tested in game.**
  - **What was wrong:**
    - The bones hanging off AITrajectory (bone 1) are built by the game on its local rotation. The world pose's own
      entry for bone 1 holds another rotation, a quarter turn about up (live, read only).
    - The editor built Hips and everything below from that entry. So its dots were a quarter turn off about the feet
      (Faith seen side on), the rings turned about wrong frames, and its check failed on every frame ("0 of N frames,
      10.0 cm").
  - **Now:**
    - Each bone's place is the chain of the game's local poses from bone 0, the same way the game builds its world
      pose. Read live, that chain matched every bone's world pose within 2 mm.
    - The rings turn about the picked bone's own axes, ticked by default ("Rings on the bone's own axes"); unticked,
      about the world's.

- **New: GAME > Pose, a pose editor** (the user, 2026-11-08: "find out if it's possible to create a pose editor in game
  so you can pose faith and other units (ksec etc.)", then "Yes, build it" after
  Research_Pose_Editor_2026-11-08.md).
  - Build D75B84E5F8D1 (11-08 13:25, Idle, whole folder after the Dogen session's 9C910A40338C): installed 13:26 with
    the game closed, game folder and dist, hash checked 30 s later. **Not tested in game.**
  - **What it does:**
    - Lists Faith and the characters within 30 m of the camera (KrugerSec, people; drones too if they turn out to have
      a body). Take pose holds one's pose; up to 8 at a time.
    - Each held bone can be turned with X / Y / Z sliders (about its parent's axes, from the pose it was taken in), or
      with rings on the picture round the picked bone (world X red, Y green, Z blue, the view white). The Hips can be
      moved.
    - Bone dots on the picture: click one to pick it. Face and finger bones are hidden unless ticked.
    - Reset bone / bone and below / all, and Undo (a button).
    - Poses saved by name in `Cinematic Tools\Poses\<name>.txt`, one bone a line. They load by bone name onto any held
      skeleton with those bones.
  - **How:** the game's own pose override (the Dash ghosts' and the replay's), weight 1, on the Hips and every bone
    below them. Bones 0 - 3, the camera bones and Connect stay the game's, so the character stays where the game has
    it. Faith's box pose gets the same bones.
    - It steps aside while a replay records or plays, and never writes over another override.
    - Bone names come from the game's skeletons (Camera/PoseSkeletons.h, made from the game's data). A skeleton is
      matched by its bone count and the live parent list (pose manager + 0xF0).
  - **What isn't known yet (the log says):**
    - Whether the game takes a pose with Freeze time on. The log line every 10 s is "Pose: ... the game took the pose
      in N of M frames with time running, N of M with time frozen".
    - Whether a KrugerSec's gun follows its posed hand. The menu says how far it is from the posed hand's grip.
    - Whether drones have a body that can be posed ("Pose: N drones ... with an Ant body", once).
  - **Can't be posed:** crowd people (they share poses drawn from a texture), bodies the game doesn't animate (taken
    down, far away), Dash ghosts.
  - Files: Camera/PoseEditor.cpp/.h and Camera/PoseSkeletons.h (new). Shared edits: CameraManager.cpp (include, the
    update after the replay's, the GAME tab section before "Faith in the streets"), Util/Hooks.cpp (the view after
    the lights' update), UI.cpp (the overlay after the world images'), the vcxproj and filters.

- **Fixed: glass and other see-through parts flickering in replays** (the user, 2026-11-08 12:36: "Arrows/starting
  line works now however there is still a minor texture/glass texture flickering this must be from the alphas").
  - Build 24F1284D7D31 (11-08 12:45, whole folder): installed with the game closed, game folder and dist, hash
    checked 30 s later. **Not checked in game yet.**
    - It replaced the Dogen's door session's 409be4863d7b (installed 12:44) and holds that session's sources as they
      were at 12:44:35, including its lamp dimming in DogenDoor.cpp ("ceiling lamps' lights couldn't be dimmed" is
      in the DLL).
    - The first link at 12:44 failed (LNK1104 on the DLL) while that session's build wrote it.
  - The 12:32 test of 6E1A17004951 confirmed the draw hook fix: 669 - 1348 draws a frame seen, and the other d3d11
    functions were hooked as they turned up. The Dash's start, arrows and finish work (the user).
  - **What was wrong:**
    - **A second layer of glass.** The world's kept draws of the decal, lit and half-size passes went by whether the
      game drew their mesh in its G-buffer pass. Glass is drawn only in the lit picture's passes (the asset log:
      railing, catwalk, skybridge and building glass all "#3" only). So the game was never seen drawing it, and the
      replay drew its kept glass over the game's own, every frame.
    - **Passes by number.** A kept draw of those passes is drawn at the end of the pass of its kind with its number
      in the frame. That count isn't fixed: the game binds the lit picture's target from at least 9 places in its
      view's drawing (0x143742BD0), each only when it has draws for it. A frame had 17 at most while it recorded,
      and the glass was kept at the 13th. A frame with fewer didn't draw it at all.
  - **Now:**
    - The world's kept draws of those passes are left out where the game draws the mesh in those passes too, using
      its own instances' places read back from them. They're left out until those are known, as in the G-buffer
      pass.
    - The fewest passes of each kind in the last 32 frames are kept. At the end of the last of those, the kept draws
      of every later number are drawn too, once a frame, always at the same place.
    - A diagnostic line every 10 s while it plays gives each kind's fewest and most passes, and how many frames
      needed that.

- **Lights: SetFixedLightIntensity** (for the Dogen's door session: the kitchen lamps dimmer at night without
  removing and re-adding them). A light made from code gets a new intensity (lumens, kept within 0 - 1e8); it's sent
  with the next update, and nothing is sent when it's unchanged. The game reads it through the entity's Intensity
  property (data + 0xA8) each time it builds the light, as the menu's Brightness. Built in 409be4863d7b (whole
  folder, installed 12:44 with the game closed); no caller yet.

- **Fixed: replays missing the Dash's arrows, and flickering textures** (the user, 2026-11-08 04:33: "replays are
  weird now arrows disappear also textures are flickering").
  - Build 6E1A17004951 (11-08 04:48): installed with the game closed, game folder and dist, hash checked 30 s later.
    **Not checked in game yet.**
  - **What went wrong** (CT.log 04:31 - 04:33 against 03:19 - 03:22):
    - The replay's draw hooks saw 1 of the game's draws a frame while it recorded, against 844 - 1461 before. So the
      recording kept 113 of the game's draws instead of 1831. Of the Dash, it kept only the first arrow's first half
      second.
    - While it played it couldn't see the game drawing its own models. It took them for not drawn and drew its own
      copies over them, turning them on and off (the flicker; 109 models, and 256 parts of the world).
  - **Why:** d3d11 changes an immediate context's draw functions while it runs. It keeps a table of them in the context
    and writes other functions into it when it starts or stops checking draws (d3d11.dll 0x46F40):
    - DrawIndexed 0x153B70 or 0x154D30 in place of 0x1535A0;
    - DrawIndexedInstanced 0x1A4580 or 0x155640 in place of 0x1531B0;
    - and the other draws and dispatches.

    The hooks went on whichever were in the table when they were made:
    - the quick ones in the 03:19 session (0x...6535A0 / 0x...6531B0);
    - the checking ones this time (0x...653B70 / 0x...6A4580), which the game hardly drew through after.

    This isn't new: any session could start on either.
  - **Now:**
    - Every draw function found in the context's table is hooked: the ones there when the hooks are made, then any
      other seen at the game's render target changes, hooked at the next Present (up to 4 more of each).
    - CT.log says "the game's context draws through another of d3d11's ... functions: hooked too" when it happens.
  - **Also fixed:**
    - **A Dash's markers are kept one by one.** The arrows share one mesh and look, so a later arrow's draw was merged
      into the first one's. It was then drawn with that one's material values, and so with its brightness as the Dash
      had it then: the arrow pixel shader multiplies its mask by b1 x. Now each marker is a variation of its own,
      with its own values over time (up to 200 a mesh part, saved too).
    - **Unordered access views.** The runner's vision layer isn't drawn in a frame whose G-buffer pass ends with
      unordered access views bound, because the replay's own target change would let them go.

- **Fixed: a Dash's start, arrows and finish line in replays** (the user, 2026-11-08: "Replay arrow/starting are
  wrongly textured / missing model(?) finish line is missing").
  - Build 5E0FB4037FCD (11-08 04:06): installed with the game closed, game folder and dist, hash checked 30 s later.
    **Not checked in game yet.** It also has the follow smoothing below.
  - **What was wrong:**
    - The game draws a Dash's markers (dash_start, the dash_arrow meshes, dash_end, checkpointcircle) only into its
      runner's vision mask: one R8 target with the scene's depth. Their PamStaticModelEntityData has PrimaryInRVLayer
      on; the 103 other props read have it off. Its tonemap then mixes that mask into the picture in red.
    - The replay kept only the G-buffer, decal, lit and half-size passes, so it had none of their draws.
    - It judged when each recorded model shows from those passes' draws. So the finish counted as never shown and
      wasn't drawn. The start and most arrows counted as always shown, and were drawn the whole replay as solid lit
      shapes with a texture taken from the red-layer draw (t2, not a colour texture).
  - **Read in the game first:**
    - its shaders, all in systems/shaderprogramdb and the city's shader database, read with fxc /dumpbin:
      - the markers' own shaders, which write only an intensity;
      - the tonemap pixel shaders, 1792 of whose 3584 read runnersVisionAlphaMaskTexture; all 15 compared end with
        the same red mix;
    - its code:
      - the pass is drawn, cleared first, only when [draw lists + 0x100] has something (0x143743A10);
      - the tonemap mix is switched on by the same three checks (0x14374697D) and binds the mask and the colours
        (0x1435CA23E);
    - its data: the city's colours, from VE_ToD_PostEffects' RunnersVisionComponentData (OverlayColor 0.7 0.05 0.05,
      AlphaBlendAmount 0.463, AdditiveBlendAmount 0.493, colour burn on). These are the renderer's + 0x86210 /
      + 0x86224 / + 0x86228, matching that component's layout.
    - The Dash switches its markers through their prefabs' Enabled field (hash 2662400 = "Enabled"), which the
      replay doesn't record.
  - **Now:**
    - That pass is a kept kind of its own (Pass_Rv): one R8_UNORM target with the frame's depth. Every such pass in
      all the saved logs was a Dash marker's.
    - While it plays, the kept draws are drawn into a mask of the replay's own at the end of the game's G-buffer pass,
      with the game's depth, as the game's layer is. At Present, after the replay's shapes, the mask is mixed in with
      the game's own formula (Camera/RunnersVision.hlsl). Its compiled code matches the game's instruction for
      instruction. The game's own mask is left alone.
    - A Dash's marker is never drawn as a solid shape.
    - It shows from the game's first draw of it to its last (0.5 s each side), and never if the game didn't draw it
      while it recorded.
    - Its draws are matched within 3 m and drawn as kept, because the game draws dash_arrowmedium 1.14 m from the
      model's origin (its part's offset), with its prefab's scale.
    - The game's own live markers count as drawn by the game, so the copy is let go where the game draws its own.
    - Recordings save which models are Dash markers (tag "RLVE"). In a recording saved before this build, the markers
      have none of these draws: they aren't drawn (no wrong look), and recording the Dash again fixes it.
  - **Known:**
    - A marker shows only from when Faith first came within the replay's 60 m model scan of it, and the game drew it.
    - The mix is applied to the finished frame, so it also goes over the game's HUD where a marker is behind it.
    - The second vertex stream bound at the arrows' draw is left over from earlier draws: the same buffer is bound at
      doors', adverts' and kiosks' draws. The arrow meshes have one stream.

- **Changed: the follow smoothing doesn't fall behind and moves smoothly** (the user, 2026-11-08: "make the follow
  smooth slider even smoother you can see the camera falling behind and it's not a smooth motion"). FOLLOW >
  Smoothing, objects only (not Follow Faith). In build 5E0FB4037FCD. **Not checked in game yet.**
  - **What was wrong:** the copy the camera follows caught up with the object over Smoothing x 0.5 s, a first-order
    lag.
    - It trails by speed times that: 4 m behind a car at 20 m/s at 0.4, and 12 degrees behind a 60 degree a second
      turn.
    - Its own speed jumped with every step of the object's.
  - **Now:** a critically damped follower that's given the object's speed and turning. Those are smoothed over
    Smoothing x 0.5 s, and the gap closes at the rate 1 / (Smoothing x 0.25 s) without swinging past.
    - At a steady speed or turn there's no gap, and its own speed changes smoothly.
    - Each frame is worked out exactly for its time, so any frame rate gives the same motion.
    - The turn is worked out about a world axis with DirectXMath's product order.
  - **Offline test** (scratchpad follow\sim.py, sim2.py, simrot.py; 30 - 240 fps), at 0.4 against the old:

    | | Old | New |
    |---|---|---|
    | Behind a 20 m/s object | 4 m | 0.07 m (the 2 cm wobble it was given) |
    | Behind a steady turn | 12 degrees | 0 degrees |
    | Acceleration from the wobble and the swarm's held steps | 103 m/s² | 6 m/s² |
    | Braking from 10 m/s over 1 s | stops 2 m short, then creeps up | runs 0.5 m past, then settles back |

    At 1, it runs 2.1 m past instead. The menu's help text says so.

- **New: Dashes record themselves** (the user, 2026-11-08, from GridNode's message listener). REPLAY > "Record Dashes by
  themselves", on by default, kept in config.ini (RecordDashes).
  - Build 17090B388B23 (11-08 01:47): installed with the game closed, game folder and dist, hash checked 30 s later.
  - **Checked in game** (the user's test, 11-08 03:19 - 03:22, CT.log):
    - Two Dashes started recordings by themselves.
    - The finished one stopped 2 s after its finish (54.0 s). Save every recording was off in config.ini, so it was
      kept to play and not saved, as designed.
    - Both the started and the finished message came on the client's message manager. The finished one came once,
      not twice. Its fields from + 0x48 were 398BF72E 0 B32 1 920 0 0 0.
  - **How it hears them:** the tools register their own listener with the game's message managers, the client's and
    the server's, the way the game's own code does. Nothing is hooked, patched or sent. Read in the game's code before
    building:
    - the register function (0x142A3AD40, byte-checked);
    - the 298 registrations of the game's own, which take the managers from the client context 0x142401CB0, the
      server context 0x14253D670 and the core 0x142142CE0, realm 0xFF;
    - the dispatch (0x142A394A0), which calls a listener's slot 0 with the message.
  - **The messages:** the "PamSocialPlay" category's NamedChallengeStarted, NamedChallengeFinished and
    NamedChallengeCancelled (Dashes are named challenges). The server sends them to the player. The finished one is
    sent twice, so repeats within 1.5 s are taken as one.
  - **What happens:**
    - A Dash starting starts a recording, if nothing is recording or playing.
    - Its finish stops it 2 s later, so the end is in the recording. It's saved like any recording, with its folder
      named Dash.
    - A cancelled or restarted Dash's recording stops and isn't saved. It stays loaded to play until the next one.
    - A recording started from the menu is left alone.
  - **Logged for the first test:**
    - each Dash message, with the manager it came on and its raw fields;
    - every other PamSocialPlay message by name, once (time trials send ChallengeFinished, for example).
    - What the fields hold isn't known: GridNode's "time in ms at + 0x58" doesn't match the sender's code, so no time
      is shown.

- **Crash notes now give the real call chain** (the user, 2026-11-08, from GridNode's crash notes).
  - Build 199FA4DA3C67 (11-08 01:32): installed with the game closed, game folder and dist, hash checked 30 s later.
  - A new CT.log line, "[CRASH] call chain (unwound, each a real caller)", comes right after the registers. The stack is
    walked with each module's own unwind data, so every frame is a real caller. A crash's own frame may be a leaf
    function (its caller's address at the top of the stack). The chain ends where there is no unwind data: a hook's
    relay, a trampoline, a cave.
  - The two stack scans stay below it. They also list values left behind by earlier calls, which is why the tools'
    drawing code showed up under GridNode's crash on 11-06.
  - Tested outside the game with a crash three calls deep: the chain ran from the crashing function to the thread's
    start, with every frame where its call was.
- **Not built: timeline sync for replayed doors.** The replay already records and plays doors, elevators, the MagRope's
  visuals and collectables as moving models (ReplayModels, since 10-23). Forcing a running timeline runs its event
  tracks' update over the jump (0x1436903E0), and door timelines have event tracks, so it could set off their logic.
  The door gaps still open (the prison-door arrow signs, the gate arrows) have other causes.

- **New: Faith's body the game's own way, through her view mode** (the user, 2026-11-08, from GridNode's and MECMP's
  findings).
  - Build 172054BAC91C (11-08 01:22): installed with the game closed, game folder and dist, hash checked 30 s later.
    **Not checked in game yet.**
  - **How it works** (read in the game's code before building):
    - Faith's client character has an interface at +0x1D8 whose slot 15 (0x14390ED90) tells the game how she's seen:
      first person for the local player, else third person.
    - Nine places in the game ask it: which of her two pose objects it hands out (+0x240 her box pose, +0x248 her
      body), her mesh parts' first-person flags (most likely what hides her head), her culling box, a first-person
      camera step, and three that pick first- or third-person data.
    - While the camera is on, her interface gets its own copy of that vtable, with slot 15 saying third person. Nothing
      else in the game is changed. The game's own vtable goes back when the camera goes off. A new Faith (a respawn, a
      level load) gets it too.
    - Each step is checked first: the function's bytes, the slot, and her player linking back to her. If any check
      fails, the tools fall back to the old way and say so once in CT.log.
  - **The old way stays as a choice:** CAMERA > Show Faith's body > Her body: "The game's third-person body" (new,
    default) or "Her whole first-person body" (the original tools' way: the camera type id changed for the whole game,
    her own camera unlinked half a second later). It changes at once while the camera is on and is kept in config.ini
    (ThirdPersonBodyWay).
  - The replay overrides both of her pose objects, so recordings still play on her body either way.
  - **Not known yet:** whether her hair cloth looks and moves the same with the game's third-person body. The hair fix
    (cloth kept on far from the camera) runs with both ways.

- **New: a guard on the game's own crash when quitting during a Dash** (the user, 2026-11-06, from GridNode's findings,
  github.com/CodeNameMeteor/Gridnode).
  - Build 243289D718D8 (11-06 18:51): installed with the game closed, game folder and dist, hash checked 30 s later.
    **Not checked in game yet.**
  - **The crash** (GridNode saw it twice; not seen in our logs). A clean-up in the game's mission code (0x1439E08F0)
    takes the server world from its global (0x1425469E0, null between levels) and asks 0x143164D50 for the first entity
    of a class in it. That function reads the world's index at +0x28 straight away, so with the world already gone it
    reads address 0x28 and the game crashes. Read in the game's code here before building.
  - **The guard:** MinHook's jump replaces that one call (0x1439E0938; the other 239 callers are left alone). A small
    cave makes the game's call when there is a world. With none it answers "no such entity", so the clean-up skips its
    loop as it does when there is none. The cave calls nothing of the tools', and it stays on while the game closes.
  - The code is checked byte for byte first; if it differs, the log says the quit isn't guarded. When the guard
    answers, CT.log says "The game's mission clean-up ran with no server world (quitting during a Dash)".

- **Fixed: orbit keyframes followed Faith** (the user, 2026-11-06: "There is a bug where orbit ticked camera follows
  faith find what went wrong and fix it").
  - Build 08375C7A0291 (11-06 16:17): installed at game close (16:19), game folder and dist, hash checked. **Not
    checked in game yet.**
  - **What went wrong:** an orbit keyframe kept only where it was round the focus (yaw, pitch, distance), not the
    focus itself. When the track played, and on the drawn path, it was put round the focus where it was right then.
    The focus is Faith (or what the camera follows), so a ticked keyframe and the moves to and from it went
    wherever she went. Ticking was meant to leave it where it was.
  - **Now each orbit keyframe keeps its own point** (CatmullRomNode::OrbitFocus): where the focus was when it was
    ticked, or when it was made while circling. Its yaw is measured from the world's heading, so the circle's turn
    doesn't change it later.
    - Between two orbit keyframes the move goes round their points. When the points differ it goes smoothly from
      one to the other.
    - Next to a plain keyframe, the move goes round the orbit keyframe's point.
    - The drawn path, the keyframe dots, the arch diamonds, dragging them, the pink preview camera, "Go to", the way
      round and its wall check all use the same points. None of them need a focus any more for a keyframe that has
      its point.
    - Keyframes without a point (none are made now) still go round the focus where it is, as before.
  - **Following Faith is FOLLOW's job.** A track set to move with what's followed now carries its moves round a
    point along with the rest of the track. Before, those moves were placed after the track was moved, so they
    ignored it.
  - The wall check now runs again when a keyframe's place round its point, its way round or its arch changes.
  - Texts now say "its point" instead of "the focus": the row, its button "Look at its point", the hints on the
    drawn path, and the help.

- **Game speed keyframes, and the light during a time-lapse** (the user, 2026-11-06: "Check if game speed keyframes
  actually work", "Improve more the 'Light catches up with time jumps' since we did a lot of decompiling since last
  time fix any bugs no mistakes"). Notes: section 8 of `Research_TimeOfDay_Jump_2026-10-25.md` (outside the repo).
  - Build 3E1E66D6B22F (11-06 15:40): installed with the game closed, game folder and dist. **Not checked in game
    yet.**
  - **Game speed keyframes work, but only with Keyframe options > Game speed ticked.**
    - Traced in the code:
      - each keyframe keeps GAME > Game speed's value as it's added (1x unless the slider was moved);
      - the track interpolates it;
      - while the track plays with the tick on, the camera hook writes it into the game's time scale, after the
        camera update each frame;
      - the track itself keeps real time.
    - That tick was off at every start and silent: the 15:06 and 15:16 runs played Track #1 many times without one
      "Game speed:" line.
    - Now changing a keyframe's Game speed in its row ticks it (as a keyframe's time of day already does, with a
      log line "The track now sets the game speed").
    - The row says "Not used now: Keyframe options > Game speed is off" while it's off.
    - The tick is kept in config.ini (TrackSetsGameSpeed).
  - **Fixed: a track's (or any moving clock's) time of day reset at its start never ran.**
    - The reset waited for the game's lighting clock to reach the time asked for. A clock that keeps moving never
      does, so a newer request replaced it (15:24:44: the track's jump to 00:02 got none).
    - It now also goes ahead once that clock has moved more than 120 game s from where it was when the request was
      first looked at. It's not measured against the light's own time, which trails the clock between steps and
      would let a plain jump through before the clock had the new time (the 2026-10-25 04:35:28 case).
  - **New: the light follows a time-lapse while it runs.**
    - From the disassembly of the game's step (0x143950920): its steps don't go by the clock. Each update adds its
      game frames (dt × 30) to + 0x58, and a step comes every UpdateInterval (240) of them, about 8 s, lighting the
      clock + 0, + 240 and + 480 s.
    - So a 5-second time-lapse got one step (15:24: 00:02 → 20:19, the light changed once, at 20:19).
    - While the clock runs forward faster than 600 game s a real second:
      - the step count grows by how far the game's own clock went instead;
      - UpdateInterval is set to how far the clock goes in a step's time, measured as the steps come (at least
        0.3 s, at least the game's 240, at most 6 h).
    - So each step lights where the clock will be when it's shown.
    - It stops below 300 game s a second, on a cut (over 2 h in one update) or a pause. The game's own interval goes
      back on its data at once, and the reset after the time-lapse (as before) redoes the light at the final time.
    - Checked by bytes before it's used: the step's interval read, its × 30 count (and that the constant is 30) and
      its progress test. A warning if they differ, then only the start and end resets, as before.
    - Log lines:
      - "Time of day lighting: the clock runs N game minutes a second: the light follows it ..."
      - "... followed the clock for X s (N steps, one every X s); its own steps again ..."
  - Only while Light catches up with time jumps is on, a level is loaded and MECMP isn't there.
  - Checks: TrackPlayer, CameraManager, Hooks and VisualsManager compile with no warnings with the project's flags.
    The build has no errors, and the new strings are in the DLL.

- **CAMERA > Show the track's path: orbit keyframes and their arches can be dragged, each keyframe shows where it
  looks, and dots have a right-click menu** (the user, 2026-11-06: "Improve the camera dots UX", "Orbit keyframe
  improvements (make it be able to affect the arch of the dots)", "add something to show where the camera looks").
  - Builds:
    - 61A8A3B3B34C (11-06 15:03), installed when the game closed at 15:05. In the 15:06–15:12 run, the log shows a
      keyframe's dot and its curve handle dragged on the path. Orbit dots, arches, the view outlines and the menu
      weren't checked.
    - 96C2959FDA7D (11-06 15:14), installed at 15:15 with the game closed, game folder and dist: adds the preview
      below. **Not checked in game yet.**
  - **Preview the track on its path** (new tick under Show the track's path; the user: "add a preview camera path
    button that previews the camera path without playing the track", then "make it a tick that disables the live
    preview when moving the camera pos as well"):
    - A pink camera plays the selected track along its drawn path, as the camera would, at the playback speed, again
      and again, with a second's wait at the end.
    - It shows its view outline (with Rotation set by the keyframes) and its time, e.g. "2.35 / 8.00 s". Your camera
      stays where it is.
    - The path and the pink camera stay over the picture with the menu closed too, so you can fly round and watch;
      nothing can be clicked then.
    - While it's on, changing a keyframe's position, angles or field of view in the list no longer moves your camera
      there (the live preview); the pink camera goes to that keyframe instead. Dragging a keyframe's dot holds it
      there too.
    - It's sampled the way playback samples the track: the same segments, holds, eases, and orbit moves round the
      focus where it is now.
    - It plays only the selected track (not the shot sequence), where its keyframes are: not moved with what the
      camera follows, as a playing track can be.
    - Kept in the config as PathPreview (off by default).
  - **Where each keyframe looks:** at each dot, the outline of its camera, as wide as its field of view and shaped
    like the screen.
    - A small triangle marks its picture's top.
    - The clicked or hovered keyframe's outline is brighter, with a line on along where it looks.
    - Shown while the keyframes set the rotation (Keyframe options > Rotation). New tick under Show the track's
      path: "Show where each keyframe looks" (on by default, kept in the config as ShowKeyframeViews).
  - **Orbit keyframes on the path:**
    - Their dots are turquoise and can be dragged round the focus (before, they couldn't be moved), with Shift for up
      and down.
    - A dragged keyframe keeps its view relative to the focus. Distance stays within 0.3–5000 m, height within 85°
      up or down.
  - **The arch of each move round the focus:** a green diamond in its middle.
    - Drag it up or down to raise or dip the arc. That's the keyframe's existing Lift, which Find the best way round
      also sets.
    - Drag it out or in to take the arc further from or nearer the focus in the middle: a new Bulge, sine-shaped
      like Lift.
    - A thin line shows how far it is from the plain arc.
    - Each is eased back as far as needed to keep the whole arc at least 0.3 m from the focus and within 85° of
      level.
    - The keyframe's row says "rising/dipping … in the middle" and "… m further from/nearer the focus", with a new
      "Flatten the arch" button.
    - Playing the track goes through the same arch.
  - **Hints and menu:**
    - The hint by the cursor now shows the keyframe's time. While an orbit keyframe or an arch is dragged, it shows
      where it is round the focus.
    - Right-click a dot: Go to it (the camera to its view, round the focus for an orbit keyframe), Automatic curve,
      Delete it.
    - Right-click a diamond: Flatten the arch. Each is one undo step.
    - The menu is drawn every frame and closes with the path, so a stale popup can't take the mouse.
  - Checks:
    - Built with no errors.
    - TrackPlayer.cpp and CameraManager.cpp compile with no warnings with the project's own flags (compiled
      separately into the scratchpad).
    - The new strings are in the DLL.

- **VISUALS > Lights: Ctrl + wheel over a dot changes its light radius** (the user, 2026-11-05: "what if cntrl and
  scrolling increased the light radius"; build 287215858f0b, installed 23:26 after the game closed).
  - With the cursor on a light's dot, the wheel still makes it brighter or dimmer. With Ctrl held, it makes its
    light radius bigger or smaller instead: x 1.15 a notch, within the Light radius box's 0.05 - 1000 m.
  - While Ctrl is held, that light's radius is drawn (a ring round a sphere, a line to a spot's end). The hint by
    the cursor shows the radius.
  - Ctrl is also the free camera's "move down" (the user's config: Camera_Down = Left Ctrl). While the cursor is on
    a dot with Ctrl held, that move waits, so the camera doesn't sink while you scroll. Only when "move down" is
    bound to a Ctrl key.
  - Shared files changed: Camera/CameraManager.cpp (UpdateInput: the wait, CRLF kept), Input/InputSystem.h
    (GetKeyboardBinding).
  - The user's test of 6a54bd1662fb (CT.log 22:54 - 23:19): new lights placed 3.2 - 5.1 m away on what the camera
    looked at; drags and aims; looks; Remove and Remove all, every light checked "off in the game" about a second
    later; no warnings. Undo wasn't tried. **The Ctrl + wheel isn't checked in game.**

- **VISUALS > Lights: new lights on what the camera looks at, and Undo** (the user, 2026-11-05: "New lights could
  appear on the surface you're looking at ... An undo for drags and removals. Do those"; build 6a54bd1662fb,
  installed 22:31 with the game closed).
  - **New lights go on what the camera is looking at** (the middle of the picture). The game's own ray, the one the
    free camera stops at walls with (collision::Sweep), is cast up to 60 m.
    - A sphere goes half its light radius off the surface, along the surface's normal.
    - A spot goes 0.6 of its radius back toward the camera, still pointing the way the camera looks, so its light
      lands where you look.
    - With nothing within 60 m, the light stays 2 m in front. With a surface nearer than 1 m, it goes half way.
    - The ray is cast on the game thread, before the light is first sent. CT.log says where it went.
  - **"Where the camera looks"** (under Place) does the same for a light that's already there.
  - **Undo** (next to the New buttons, once there's something to undo; its tooltip says what) takes back the last:
    - move: a dot's drag, a spot's aim, the Position and Move boxes (one step per drag), and Where the camera
      looks / In front of / At the camera / Aim like the camera;
    - removal: Remove, or Remove all as one step. The lights come back with their own names and settings.
    - Undoing a move puts back only where the light was and how it pointed; its other settings stay as they are.
    - Up to 50 steps. No key for it: Ctrl is the camera's "move down", Backspace the track's undo.
  - **Not checked in game.**

- **VISUALS > Lights: removing really removes, checked; aim spots with the mouse** (the user, 2026-11-05: "Check if
  everything works. Fix any bugs. Confirm that forgetting lights actually removes the light", and "find any
  improvements we can do to the user experience for the lights"; build 975b6c0f0c5b, staged 22:23, installs when the
  game closes).
  - **"Forget all" is gone.** It only ever took lights off the tools' list, so they stayed lit. "Remove all" is the
    one button now: it turns each light off in the game, as each light's Remove does.
  - **Each removal is now checked in the game's own list of lights.** About a second after the off is sent, the
    game must have taken the light's handle back (it writes 0xFFFF as it takes the render light away, 0x143762D0A)
    and let its render light go. The tab then says "Removed: all are off in the game", or which light may still be
    lit after 6 s, and CT.log has a line for each light.
  - **Fixed: a remove could leave a light lit.** Remove cleared four of the light's bytes, including the game's
    "queued" byte (+ 0xA2). The check made before the off is sent could then read a render light the game hadn't
    updated yet and skip the off. Now only the light's on byte is cleared, as the On box does.
  - **Fixed: a light the game's full update queue dropped could never change or be removed again.** The game sets
    the queued byte before adding to the queue and leaves it set when the queue is full (0x1437623E0). A light
    still marked queued 5 s after it was sent is now sent again (a line in CT.log).
  - **Aim a spot with the mouse:** the picked spot's line ends in a small square. Drag it to point the spot; the
    mouse wheel while dragging takes the square nearer or farther, to aim toward or away from the camera.
  - **The list and the picture together:** with the cursor on a light in the list, its dot is picked out on the
    picture (bigger, ringed, named).
  - Checked live, read-only (the game's render lights, 22:1x): 480 lights in use, every one with its own index at
    + 0xB4, its record linked both ways, and the same position and radius in both. **The changes are not checked
    in game.**

- **VISUALS: *Test every effect (pictures)* removed** (asked: "check the last test to see if every visual is ok (it is)
  and remove the option after that"). The button, the `Cinematic Tools\Visuals test.txt` trigger and the test's code
  are gone. VISUALS > Depth of field > More settings > *Take test pictures* (and `DoF test.txt`) stays.
  - **The last test (11-05 21:43:42, 97680356920D, the camera still) was right on every step:**
    - saturation 0.338 → 0.009; grain; exposure +2 (mean 98 → 217); vignette (corners 58 → 32);
    - sun ×4 (mean 98 → 148, final illuminance 114.7 → 458.9); red sun (red / blue 0.74 → 1.04);
    - sun rotation +25: a building face goes from sunlit to shade, the sun's direction y 0.760 → 0.964;
    - lens distortion, chromatic aberration 1.03 and the clouds' swapped layers all visible;
    - Sky color ×4 nothing, as expected (the game sets it to 0 here).
  - **The run just before it (21:43:09) had one miss:** its red-sun step wasn't red in the picture, though the red sun
    was worked out at the step's start (114.7 / 16.6 / 16.2). The camera was still moving during that run's first
    steps. The cause wasn't found: nothing in the log, and nothing in the menu turns a change off by itself. The same
    step was right in the two runs before (16:50, 17:35) and the one after.
  - Build E9ABBB7AB3F7 (11-05 21:52, Idle, the whole folder; only these two files changed since 97680356920D):
    installed at game close (22:05, game folder and dist, over older files only, hash checked 30 s later). Checked in it: no "Test every effect" or "Visuals test" text left; the other session's
    "g_ShelterGrid at byte %d" and GbufferWetPs blob, the Outdoor light / chromatic aberration lines and the DoF
    test still there.

- **Rain: wet gravel keeps its stones** (asked: "Read the latest g buffer saved diagnostic and fix the issue for rain
  no mistakes"). Notes: section 17 of `Research_ScreenSpace_2026-11-02.md` (outside the repo).
  - Build 48062D8087F6 (11-05 16:32): installed with the game closed, in the game folder and dist, hash checked.
    **Not checked in game yet.**
  - **Fixed: a wet gravel roof turned into one flat mirror.**
    - The G-buffer dumps 20261105_161518 and _161520 show every gravel pixel's facing written straight up, with its
      smoothness raised to 0.70.
    - Cause: the water's film laid the facing flat on every level surface. The stones (34° off level at the median)
      were gone, and the game's reflections showed a smooth sheet.
    - Now the film lies flat only where the game's facing is within 12° of the surface. That covers decals, stains
      and the turn across a model's triangles. It fades out to nothing past 25°, which is the surface's own relief:
      gravel, a vent's rim, a box's bevel. Puddles still lie flat.
  - Checked by modelling all three dumps:
    - The gravel keeps the game's facing.
    - These move by more than 1° on under 1.5 % of their pixels, only at edges, a lip and grilles: the concrete path,
      the box tops and vents of 20261105_161550, and the 11-03 roof box whose triangle lines the flattening was added
      for.
  - Only GbufferWetPs changed (35 temps; 11,154 -> 11,162 slots). GbufferWetInfoPs compiles byte-identical. The new
    shader is in the DLL once, and the old one is gone.
  - **Found, not fixed (dump 20261105_161550): an open box top at the foot of a tall wall is drawn dry,** a patch with
    soft edges.
    - It comes from the weather's 2 m roof squares, blended between their middles.
    - A possible cause: a square whose probe hit the top of the building beside it, so the low box top counts as
      more than 2 m under it.
    - The dump doesn't hold the squares, so this isn't confirmed.
  - **Added: the G-buffer dump saves the roof squares too.** Build 7676F33BD4D4 (11-05 16:50, built at Idle, staged
    to install when the game closes, over older files only). **Not checked in game yet.**
    - Two more files per dump: `<stamp>_shelter.raw`, the squares the pass read (t3), and `<stamp>_wetparams.raw`,
      the pass's constants (b0).
    - `_info.txt` says what each holds and where the squares' corner, size and count are (g_ShelterGrid, byte 304).
    - Weather > Wet surfaces > Save the G-buffer (diagnostic) at the dry spot now tells which square keeps it dry,
      and whether a roof or "under the first surface found looking down" does it.
    - GbufferWet.cpp (the two parts; TexelBytes knows R32G32B32A32_FLOAT), WetLook.cpp/.h
      (`wetlook::ShelterGridOffset`). No shader changed.

- **VISUALS: Outdoor light's sun lights the world again; colour fields and Lights' typed values stay in range; a test
  of every effect** (asked: "Fix any bugs in the visual tab you find no mistakes high priority", "and ensure that
  everything actually works"). Notes: section 6 of `Research_Visuals_Audit_2026-10-26.md` (outside the repo).
  Builds:
  - F958B7826BB0 (11-04 23:53, installed with the game closed): everything below except the sky's own sun.
  - 7D3F5332C04B (11-05 16:16, installed at game close in the game folder and dist, hash checked 30 s later): adds
    the sky's own sun and the hint. Since 16:32 the installed build is 48062D8087F6 (another session's, from the whole
    folder): it holds these changes (their strings checked in it). Checked in game at 16:50 (below).
  - 97680356920D (11-05 17:07, Idle, the whole folder, installed with the game closed in the game folder and dist,
    hash checked 30 s later): the Sky color / Ground color line, the test's sun reading and the chromatic aberration
    fixes (below). It also holds the other session's 7676F33BD4D4 work (its "g_ShelterGrid at byte %d" string and
    the 16:32 GbufferWetPs blob, both checked in it). Checked in game at 17:35 (below).
  - **Fixed: Outdoor light's sun color, intensity, angular radius and rotation did nothing to the light.** The game
    works the sun's light and direction out in its mix, before the tools write the menu's values, so the menu only
    changed the raw fields. The tools now call the game's own code again on the view state right after the change:
    - the direction at 0x1436E9990;
    - the physical sky's sun at 0x143729430 when the level has one (Sky enabled, sky type 2), else the mix's own
      formula.

    Both calls are guarded (byte checks, `__try`). If either fails, it turns itself off with a line in CT.log.
  - **Removed from Outdoor light:** "Final sun luminance / illuminance" and "Outer space sun luminance / illuminance
    1 / 2". They're the results of that work, so the game overwrote whatever was typed there.
  - **Fixed in 7D3F5332C04B: the city's sky has its own sun.** The test pictures (below) showed Sun intensity ×4 and a
    red sun changing nothing: the final illuminance stayed 114.29. Read live, the sky's Light 1 doesn't take Outdoor
    light's color (Light1TakesColorFromOutdoorLight 0) and has its own intensity, 200. The menu's Sun color and Sun
    intensity now scale the sky's Light 1 color and intensity, from what they were when the effect was copied from the
    game. So they change nothing until moved, and the game's own changes underneath (time of day) are kept. Outdoor
    light shows a line saying so when the level is like this. The sun's disc in the sky stays where the game draws it.
  - **Fixed: colour fields could go negative.** ImGui 1.61's HDR colour fields have no limits, so a drag or a typed
    value could take a channel below 0. Every effect's colour fields are now held at 0 or more, and so are Lights' colours.
  - **Fixed: Lights' Ctrl+click typed values weren't clamped** (ImGui 1.61 doesn't clamp a typed value). They're now
    held in the menu's ranges:
    - intensity, radius, size and dimmer;
    - the shadow's cull, fade (up to the cull), radius and dimmer;
    - the spot's outer angle 1 – 179°, the inner up to the outer;
    - an all-black colour is kept just above 0.
  - **New: VISUALS > *Test every effect (pictures)*** (or the file `Cinematic Tools\Visuals test.txt`). It takes 13
    pictures into `Cinematic Tools\Captures`, 1.5 s apart, each with one change on the game's own values, then puts
    every effect back. CT.log has the sun's final illuminance and direction for each.
  - **What the pictures showed** (11-05 16:10, outside in the city, F958B7826BB0):
    - **Works:** saturation 0, film grain, exposure +2, vignette, lens distortion, the clouds' opacity. (Chromatic
      aberration was counted here too, wrongly: see 16:50.)
    - **Sun rotation +25 works:** the lit floor 124 → 165, final illuminance 114 → 119, the direction's y (up)
      0.745 → 0.957. The sun's disc did not move.
    - **Didn't work** (fixed in 7D3F5332C04B, above): Sun intensity ×4 and a red sun.
    - **Unclear:** Sky color ×4 changed the shaded wall by +3, no more than the frames drift by themselves.
  - **Again at 16:50 with 48062D8087F6** (same area, the camera a little different, so compared only within this run):
    - **Sun intensity ×4 works:** final illuminance 114.6 → 458.3, the whole picture brighter, the sky too.
    - **Red sun works:** final illuminance 114.6 / 110.4 / 107.4 → 114.6 / 16.6 / 16.1; the light, the sky and the
      sun's disc red.
    - **Sun rotation +25 works:** the floor between the railing and the wall goes from shadow to sunlit, 77 → 179
      (step 03, which changes nothing, gives 77.2).
    - **Sky color ×4 does nothing, and that's the game:** live, the renderer copies Sky color and Ground color into
      its constants and then sets both to 0 (0x143736063: 0x143791B80, on the bounce light's (Enlighten's) manager).
      The state had sky (0.32, 0.34, 0.40) and ground 0.05, the constants 0. Their only other reader (0x143734D33)
      runs only with a view flag (+ 0x88276) that's 0 here. **New (in 97680356920D):** Outdoor light shows "Sky color and
      Ground color change nothing here" when both hold, with byte checks on the three code places.
    - **Fixed (in 97680356920D): the test's CT.log sun was the game's about once in three.** It was read at the hook's
      start, and one of the game's three state copies a frame puts the game's own values there; step 06's line showed
      the game's sun though its picture had the change. It's now read after the tools' write.
    - **Wrong test value, and the field fixed (in 97680356920D): chromatic aberration.** Its picture (at 16:10 too)
      was the whole frame smeared into streaks, not fringes. The scale is a factor, 1 = none. From the game's code
      (0x1435C9E63) and its shader: the red is sampled at t.x / (1 + a (s − 1)), t.y / s from the frame's middle, the
      green at half that, the blue unmoved. The city's own is 1.005 (aspect 0.5), so the test's 0.1 drew the red at a
      tenth of its size with the edges pulled in. Now:
      - the test uses 1.03 (the red about 30 px off at the top and bottom of a 4K frame);
      - the field is "Chromatic aberration scale (1 = none)";
      - its drag step is 0.0005 (it was 0.005, the city's whole effect per pixel dragged);
      - the Filmic effects help explains the scale and the aspect ratio.
    - Everything else as at 16:10: saturation 0.282 → 0.020, vignette corners 74 → 38, the clouds' sky 103 → 85,
      exposure +2, grain and lens distortion plainly visible.
  - **Again at 17:35 with 97680356920D, everything works:**
    - every CT.log line now matches its step: rotation +25 shows the moved sun (direction y 0.760 → 0.963, light
      114.7 → 119.4), sun ×4 458.9, red sun 114.7 / 16.6 / 16.2;
    - chromatic aberration 1.03: a clean picture. The rail shows separate red, green and blue copies, and the red is
      moved 27 px at the bottom edge and 26 px at the right (the formula gives about 26 and 25 there);
    - the Sky color line's condition read live again: the renderer's sky and ground constants 0. The line itself
      wasn't seen (no view of the menu from here).
  - **For the weather session:** VISUALS > Weather's lightning makes Outdoor light's Sky color brighter and bluer
    (`ApplyWeatherLook`). Where the renderer sets the sky color to 0, as in the city, that part of the flash does
    nothing; its exposure and sky brightness parts are not affected.
  - **Not fixed, for the weather session (its files):** VISUALS > Weather's typed values aren't clamped either. A
    thunder interval or a fog distance of 0 or less can be typed in.

- **The game's depth read right at the screen's edges; the camera checked against the game's own** (the user,
  2026-11-04: "find anything wrong with depth maps and fix it", for the game's depth the tools read and the snow's depth
  on the ground). Build 3A8F4D383BB4 (16:40), STAGED: installs when the game closes. **Not tested in game.**
  - **Fixed:** a point the snow and the wet look look at just past the screen's right or bottom edge read the game's
    depth outside its texture, which came back as sky; past the left or top edge it read the edge itself. So along the
    right and bottom edges the snow thinned (its edges, its width test), and in the bottom right corner it was gone.
    Now every edge reads its own pixel. Nothing inside the screen changes; the shaders' registers are the same.
  - **Measured from now on:** whether the camera the tools place everything with is the one the game drew the frame
    with, or the next frame's. Twice a second the game's own camera for the frame is read back and compared; every
    10 s CT.log has "Wet surfaces (diagnostic): the tools' camera against the game's own of the frame ..." with how
    many pixels off a point 20 m ahead lands with the frame's camera, the frame before's and the frame after's. If
    it's off, the snow's and the puddles' patterns, prints and kept-dry boxes lag a frame while turning; the fix will
    depend on what it shows.
  - **The snow's depth on the ground:** gone through, nothing wrong found (it's the same on the CPU, for prints, as on
    the GPU).
  - Also holds the VISUALS session's Depth of field changes ("Take test pictures").

- **Depth of field checked in game: test pictures, the set-up button fixed, the Simple blur's width** (asked: "Verify
  those as well and implement any fix you find"). Notes: section 5 of `Research_Visuals_Audit_2026-10-26.md`.
  - **Test pictures** (in 8DCEB32BD4BC, installed with the game closed, hash checked). VISUALS > Depth of field >
    More settings > *Take test pictures*, or the file `Cinematic Tools\DoF test.txt`. It saves five pictures of the
    finished frame (before the menu) into `Cinematic Tools\Captures`, then puts the depth of field back:
    1. the game's own DoF;
    2. Simple with the game's values and Enable on;
    3. Sprite with the game's values and Enable on;
    4. Simple with the set-up button;
    5. Sprite with the set-up button.

    New `Visuals/FrameShot.*` writes the PNGs with Windows' encoder on its own thread.
  - **What the pictures showed** (16:20, the camera on Faith at 1.21 m):
    - With the set-up, her face is exactly as sharp as with no depth of field, and the background is strongly blurred
      in both methods.
    - With the game's values, Sprite blurs nothing (Blur factor 0) and Simple only the far city.
    - The Sprite method does draw on PC. Its crash guard was needed both on first use and again after switching back
      from Simple, so the game frees its buffers in between.
    - Simple leaves a dark halo round her hair. Sprite has no halo but a hard, slightly jagged outline. Both are now in
      the method's help.
  - **Fixed: the button's sharp zone was thinner than a person when close.** At 1.2 m it was 12 cm in front and 24 cm
    behind, which softened her shoulders by 30 % and, with Sprite, her legs by 37 %. It's now at least 0.25 m in front
    and 0.5 m behind, and fully blurred at least 0.5 m / 1 m past those.
  - **The Simple blur's width (code, the running game and the pictures):**
    - It's a Gaussian on a half-size copy (1920 × 1027 at 3840 × 2054).
    - It spans the filter's 3, 5, 7, 9, 15 or 31 pixels of that copy (6 – 62 screen pixels). Its spread is
      max(Blur factor × Simple max blur, 1) of them.
    - Nothing caps the filter on PC.
    - A background edge measured 24 – 26 px wide with the button's set-up (Sprite: 36 – 45 px).
    - The menu line and the Blur factor help said "made N wide", which was wrong. They now give spread / taps /
      screen pixels.
    - The button uses the 31-pixel filter with max blur 5 (it was 9 pixels and 2, only about 18 px across at 4K), and
      Sprite max blur 3.
  - The zone fix is in 3A8F4D383BB4: the snow session built it at 16:40:07 from this folder, and I checked its
    strings. It's staged to install at game close. Not checked in game yet.

- **Depth of field: the focus distance does something now** (build 88601E3581C0, installed in the game folder and dist
  with the game closed, hash checked 30 s later; it also has the snow session's 15:43 work). Asked: "Check if Depth of
  field actually works and fix all the bugs", then "Focus distance doesn't". Notes: section 4 of
  `Research_Visuals_Audit_2026-10-26.md` (outside the repo).
  - **Why it didn't seem to work.** The game's code does use the focus distance in both methods, but "Change this
    effect" copies the game's own values, and those are a switched-off set-up:
    - **Blur factor 0.** The Sprite method's largest blur is the screen's height x clamp(0.01 x Sprite max blur x Blur
      factor, 0, 1) (0x1435C6B85), so at 0 it blurred nothing at any focus. Simple's blur is max(Blur factor x Simple
      max blur, 1) wide, so 0 is only its narrowest.
    - **The near and far distances are offsets from the focus distance, not distances from the camera.** The game's
      own ones (near 5 / 8, far 30 / 7) keep everything sharp from 2 m in front of the focus to 30 m behind it. So a
      focus between 0 and 5 m changed nothing near the camera.
    - **The Simple method read its near pair the other way round.** It's fully blurred at focus − near start and only
      sharp from focus − 2 near start + near end, so a near start of 0 blurred the focused thing itself. The Sprite
      method and both far pairs work the expected way: sharp up to start, fully blurred at end (its CoC shader).
  - **The menu now has its own Depth of field layout.**
    - Enable, DoF method, Focus distance, and a *Set up around this focus distance* button. The button gives a clearly
      visible set-up for both methods: sharp from 10 % of the focus distance in front of it to 20 % behind, fully
      blurred at half of it in front and 1.5 times it behind, Blur factor 1, max blur 2. It also ticks "Change this
      effect".
    - Blur factor, with a warning at 0.
    - The method's four distances as *In front: sharp up to / fully blurred at* and *Behind: sharp up to / fully
      blurred at*, in metres from the focus. The Simple near pair is converted to and from the game's way, so the
      game's own values still draw exactly as before. Each end is kept at least 0.01 m past its start, as the game does
      anyway.
    - The method's max blur, with the blur size it gives. Blur add, which does nothing in Simple under 1.
    - *More settings*: the other method's distances, the ironsights values, *Use the camera's own settings* (new: with
      it on and a camera that has its own settings, the game ignores the focus distance) and *Sprite DoF fix*.
  - Auto-focus with a focus distance of 0 used a wrong middle for the sharp range; it now uses the method's own.
  - CAMERA > Auto-focus's help no longer says Sprite needs the Sprite DoF fix. The Sprite pass fills its constants
    every frame.
  - **Not checked in game:** whether the button's set-up looks good, and the Sprite method's look on PC (DICE turned
    it off there).

- **No thin rectangle round the traffic's cars in the snow; no snow on railings and other thin things** (the user,
  2026-11-04, with screenshots: "Traffic still has the weird square on snow fix it", a thin line round each driving car
  with snow inside it, and "Snow also looks weird on metal railings and thin metal objects fix it no mistakes"). Build
  914B81820C59 (15:43), INSTALLED with the game closed (game folder + dist; its shaders checked in the DLL). **Not
  tested in game.**
  - **The rectangle:** every road traffic car's model has a shadow plane under it, a flat rectangle 3.46 x 5.64 m about
    4 cm over the road (1.8 times the car's width and 1.3 times its length, as in the screenshot). The snow fades
    where the surface drops away by more than half the snow's depth (at least 3 cm), so it took the plane's edge for a
    drop and left a 1 - 2 pixel line round every car. Last night's "parking bays" were these planes too. Now a drop
    has to be 6 cm or more: the snow lies over the plane's edge as over the road. (That the plane is in the game's
    depth is inferred from the line, not read from the game.)
  - **Railings:** the handrail's top, the steps' lips and the stringers' edges are a few centimetres across. Where the
    depth can't tell how such thin parts face, the snow took the game's own facing, up on a rounded or bevelled top,
    and its patches and edge cut it into jagged bits, some of them on the sides. Now no snow lies on anything under
    about 7 cm across (rails, post tops, pipes, a step's lip); the corner of a wider top fades over its last 5 cm.
  - Only the snow's two pixel shaders changed (registers as before, about 300 instruction slots more each); the other
    14 compile byte for byte the same.

- **Replays: KrugerSec who come during a fight are recorded; their lines and the people's are heard when the replay can't
  say them live** (the user, 2026-11-04 02:0x: "Something broke with replays spawned in ksec are invisible read the log
  and fix it once and for all with the ksec encounters no mistakes high priority", then "Dialog recording is also broken
  fix it"). In the shared folder from 02:16 - 02:18; built into the road traffic session's 5EA19EC6C800 (02:37:35, from
  the whole folder), INSTALLED with the game closed (game folder + dist; checked: its new log text and the /Enemy/ test
  are in it, no source newer). Compile-checked before in a private folder. **Not tested in game.**
  - **What the log showed** (CT.log, the 01:57 recording of 93.7 s and its 01:59 playbacks):
    - The recording took 48 people, the limit (MaxPeople). 41 of them were the road traffic's walkers
      (SpawnTemplate_AIMale_ErrorTemplate 37, AIFemale_Heels_ErrorTemplate 4). A walker that went out of sight and came
      back was recorded again as another person (its body address may be another's then), so one walker took up to 4
      places. The last place was taken 27.5 s in.
    - KrugerSec who came later weren't recorded at all: the asset log has female_enforcer drawn by the game 36 - 68 s, and
      none of the 48 is one. At the playback only the 4 KrugerSec recorded from the start (2 Enforcer_Lowsense, 2
      SubProtector) were there, drawn with the game's kept draws (all of their draws, every frame); the others weren't.
    - 143 voice lines were recorded, all KrugerSec's and the people's. The sound track kept only a mission's narrative,
      its loudspeakers and cutscene voice-over (8.9 s of 93.7 s), and left the others "to be said live only". At the
      playback 142 of them couldn't be said live (their people gone from the level, their dialog unloaded): silent.
  - **KrugerSec's own places** (FaithReplay.cpp ScanPeople): a character whose spawn template (looked up as it's found,
    the same way RecordPeople did) is under /Enemy/ takes one of MaxEnemies (64) places, apart from the others' 48. A
    person seen without moving who went again doesn't take one of the 48. All of them together at most 512.
    TemplateReach moved up with the people's constants. One whose template isn't known yet takes one of the 48, or is
    looked at again at the next scan.
  - **The dialog** (Hooks.cpp ReplayVoiceLineSpans, ReplayDialogFromTrack; ReplayAudio.cpp's log text): the sound track
    now keeps every voice line's span, and the track is heard during any line the replay isn't saying live, KrugerSec's
    and the people's too (before: a mission's narrative and loudspeakers only). A line it can say live is still said
    live, the track silent then.
    - What it costs: while the track is heard, everything the recording heard then plays with it (music, shots, steps),
      so those can be doubled for the line's length (from 0.2 s before it to 0.6 s after).
    - Recordings made before this have the other lines cut out of their track already: record again to hear them.
  - **Not changed:** the people's radius (40 m from Faith; KrugerSec shooting from farther aren't recorded), and the kept
    draws themselves (the log has the 4 recorded ones drawn every frame; whether you saw them isn't known).

- **Road traffic: the gridlock by the train station (the transit hub) fixed** (the user, 2026-11-04 ~01:5x: "There is
  still some traffic jam near the transit hub fix it no mistakes high priority"). Build 5EA19EC6C800, built 02:37:35
  at Idle from the shared folder, INSTALLED with the game closed (game folder + dist, hash checked 37 s later). **Not
  tested in game.**
  - **What it was** (the live game read at 01:3x, read-only):
    - Every one of placement 188's 82 cars stood at speed 0. That's the loop round the station, x -116 .. 80,
      z 233 .. 499, with no spawn or despawn.
    - They were queued on the three branches into its join (node 1853) and back up the loop; the road past the join
      was empty.
    - CT.log's "put back" count went up by 500 - 1,500 a minute from 01:32 on (920 before).
  - **Why, offline** (the car test locked the same way at 540 s):
    - Two cars stood side by side where two lanes meet, each in the other's way.
    - The first was allowed a small move each step, then put back by the final check, so its stuck timers never ran.
    - The second read the first's speed before the put-back (0.502 m/s) as moving, so its 6-s side timer never ran
      either.
    - Nothing ever made one of them wait unseen, so the whole loop stood for good.
  - **Fix:** a car whose move is put back every step for 6 s waits unseen until there's room, as a car held beside a
    stopped one already did. It shows again only out of sight.
  - **Offline, 16 runs of 30 minutes** (seeds 1 - 4, 30 and 10 steps a second, 2,000 cars):
    - Before: two runs locked for good (188 at 540 s; another placement in a 10-step run).
    - After: none locked. 188 now stands 9 - 11% of the time at its join, against 73% when it locked and 2% with the
      level's 40 cars.
    - No cars inside each other. The runs that never locked came out the same before and after: every car's record,
      every frame, in a 120-s run.
  - CT.log's minute line now says how many cars were made unseen after 6 s of being put back.
  - The build also holds the UI session's "Faith in the streets" rename (01:51 - 01:52, after its own f5594e362f34)
    and the replay FPS session's E9B8C0ABAA2F sources.

- **Menu tidy: REPLAY ticks always on, fewer (?), renames** (the user's list, 2026-11-04).
  - **REPLAY**: these are always on, with no tick and no config key:
    - Doors, elevators, moving objects and mission items
    - Draw the ones a mission took
    - With the game's own drawing of them
    - The mission's world too
    - Other animated things
    - KrugerSec and other people
    - Keep them in the level until it plays
    - Stand-ins for people a mission took
    - Keep the mission's world after it's over
    - Load the mission's bundles again while it plays
    - The mission's world from the game
    Their status lines stay (shapes, parts and world draws kept, stand-ins, kept mission worlds with *Let the
    mission's world go*, bundles), each now saying what it's about.
  - **MISC**: the *Run a performance test* and *Run a crowd test* buttons are commented out (`#if 0`). The live
    *Now:* line stays.
  - **VISUALS**:
    - *Weather* is now *Rain*; while it rains the header says *[on]*.
    - Every (?) in Rain and in Snow is gone (27). The two that credited TheNollz' Rain mod and Toffiklubitoffi's
      Snowfall mod are kept as comments in the code. Hover tooltips on sliders stay.
  - **GAME**:
    - *Street level* is now *Faith in the street*. Its *Solid street* help is short: the streets and props are
      solid, and getting down from a roof takes your own tool or a glitch.
    - *No death from long falls* is gone and always off (`[Misc] NoFallDeath` no longer read), so a long fall kills
      as in the game.
    - *Shard office blinds* is *Open* or *Closed* only. *As the game has them* is gone; a config with it (0)
      starts as *Open*.
  - **CAMERA**: *Diagnostics (written to CT.log)* is commented out (`#if 0`).
  - Build f5594e362f34, built at Idle priority, installed with the game closed (game folder and dist, hash
    checked). Built without errors; not checked in game yet.

- **Replay recording: the asset log's once-a-second hitch and the read back's copies gone, the mesh table read only
  when it changed** (the user, 2026-11-04 01:2x, after recording with A1000B3CF55E's cost lines: "Done recorded one").
  Build E9B8C0ABAA2F, built 01:34:54 at Idle from the shared folder (A1000B3CF55E's sources plus the four files below;
  the top session told first). STAGED 01:35 to install at game close (game folder + dist, only over an older file,
  checked again 30 s later). **Not tested in game.**
  - **What the cost lines said** (CT.log 01:19 - 01:21, two recordings, 13.8 s and 55.6 s):
    - Game thread: the asset log's scan 0.15 - 0.31 ms a frame, but 23 - 32 ms in one frame every second (about 19
      frames over 5 ms in every 10 s). The mesh table read (UpdateWorld) 6 - 8 ms once a second. Everything else under
      0.05 ms a frame, at most 1 - 3 ms in a frame.
    - Render thread: 1.3 - 1.7 ms a frame, 55 - 63 frames over 5 ms in 10 s. Most of it the copies read back and
      decoded: 0.52 - 0.68 ms a frame (up to 17.7 in one), their maps and copies out 0.31 - 0.46 (up to 11.8). The draw
      hooks 0.6 - 0.9 ms a frame (0.31 - 0.35 us a draw; keeping the game's draws 0.07 - 0.18, the asset log's draw
      sample 0.08 - 0.11). The draw log's one snapshot 5 s in: 15 + 24 ms in two frames, once a recording.
    - The staging fix held: 0 - 33 buffers made in 10 s (400 before), 1100 - 2500 reused.
  - **The asset log's scan** (ReplayAssetLog.cpp). The ReplayAssets.log it writes is the same.
    - Every texture slot of every draw noted (up to 8 a draw, about 15 000 draws a second) had its description made
      with sprintf and looked up in a map of strings. Now each texture keeps the id it got (DescribedId); it's used
      again only while what the description is made from is the same.
    - Each draw's mesh was looked up by name (a string copy and a lookup in a map of strings); now once a scan per
      vertex buffer (the same names: the table doesn't change within a scan). The same for the other passes' draws.
    - What a scan takes from the render thread now goes through over the next frames, 0.5 ms a frame, all of it before
      the next look at the mesh table and when it stops recording or playing.
    - The mesh table isn't gone through again when it's the same as at the last look (the same MeshSets, data and
      vertex buffers, the same mode): nothing came in or went out then. In the 55.6 s recording it changed in 9 seconds.
  - **The read back** (ReplayGameDraws.cpp ReadBack): each page (about 1 MB, 130 - 165 MB a second) was copied out
    whole before the decode read a few KB of it. Now all of a batch's pages are mapped (or none, while one is still
    being copied), read where they're mapped, unmapped and given back after. A page whose map fails is given back
    unread, as before.
  - **The mesh table read** (UpdateWorld, ReplayShapes MeshTableKey): its pointers (MeshSets, data, loaded levels'
    vertex buffers) read every second, the names and reach (and the maps made from them) only when those changed.
  - **Not changed yet:** the draw hooks' look at every draw (about 0.4 - 0.6 ms a frame), the draw log's snapshot 5 s
    into a recording, the GPU's copies (still whole pages).

- **No snow on walls; the ice on pools no longer cut along a line in first person** (the user, 2026-11-04, with
  screenshots: "Find out why snow still does this on the walls, first person has still the water issue with snow high
  priority no mistakes", "Generally there shouldn't be any snow on any wall"). In the weather session's build A1000B3CF55E
  (00:57), INSTALLED 00:58 at the game's close (game folder + dist; its shaders checked in the DLL). **Not tested in game.**
  - **Walls:** the bands along the foot of walls and frames were the piled snow's side, drawn up each wall's foot
    with the light of the gravel in front of it. It's switched off: snow lies only on what faces up, up to the foot of
    a wall. The snow's two pixel shaders are also lighter for it (the full one 28,612 instruction slots instead of
    36,148, the snow's own 10,713 instead of 18,188).
  - **The ice:** it's the only thing the tools draw from the world's own shapes and test against the game's depth,
    and it was drawn with the camera the tools read each frame. If that camera is even slightly turned from the one
    the game drew the frame with, the water's surface comes out a little off the ice. The error grows across the
    screen, so past a straight line that moves with the camera the ice was taken off. Now, whenever the game's own
    camera for the frame is there (copied with its G-buffer), the ice is drawn and tested with it, as the game draws
    its own models. **How far off the tools' camera was hasn't been measured:** if the line is still there in this
    build, the cause is something else.

- **Replay recording: a staging-buffer bug fixed, the mesh table's maps made outside the render thread's lock, and
  the recording's cost logged by part** (the user, 2026-11-04 00:2x: "find out why fps drop when recording a replay and
  fix the fps issue no mistakes"). In the shared folder from 00:50; compiled into C3E82647FB33 (00:52:16, Idle) and,
  with the snow session's WetLook.cpp edit of 00:52:45, into E4E21FDA9323 (00:52:54, their build; not installed).
  The top session's A1000B3CF55E (00:57:10, Idle, every current source: these changes, the snow session's walls / ice
  fix, its own CameraManager label fix of 00:13) is the one INSTALLED: 00:58:09 at game close, game folder + dist
  (checked: both new log lines in it; E4E21's job kept it). **Not tested in game.**
  - **What the logs show (CT.log / CT.previous.log, nothing new run):**
    - The drop is mostly slow frames, not a slower average. With Faith standing still (three mission-test pairs) the
      median frame went 7.84 -> 8.06, 8.70 -> 8.35 and 9.28 -> 11.15 ms; the 95th percentile 10.75 -> 13.46,
      11.36 -> 15.26 and 12.16 -> 19.85 ms.
    - The user's 00:22:28 recording (15 s): 143 fps before, 105.6 fps in the 10 s fully inside it (median 8.81 ms,
      95 % 14.52, 99 % 21.42), about 120 fps after (with the menu open and Faith elsewhere, so not a clean pair).
    - The replay's game-thread work (camera hook line): 0.28 ms a frame, but one frame of 23.7, 28.3 and 27.2 ms in
      the three 10-s lines around it (one holds the start, one the stop; the middle one is unexplained).
    - Its render-thread work: draw hooks 0.54 ms a frame (1853 draws, 0.29 us each), its work at Present 0.36 ms,
      117 MB a second read back from the graphics card, 401 staging buffers made and 1171 reused in 10 s.
    - Every recording-cost line since 11-03 23:42 makes 30 - 56 staging buffers a second, about a third of those it
      reuses.
  - **Fixed: the staging buffers** (ReplayGameDraws.cpp TakeStaging).
    - The camera's copy (its 1136-byte constant buffer, one in every batch) asked for 1136 bytes. A kept buffer was
      reused only up to twice the size asked, and the smallest made is 4096 bytes. So none ever fitted: a new one was
      made for every batch, then kept, piling up in the pool.
    - A kept buffer is now reused up to the size a new one would have.
    - The camera's page now counts its own buffer's size (1136) where the staging buffer's (4096) was taken: the
      bytes read and the in-flight count were off. Its staging buffer's size goes back to the pool with it.
    - A batch none of whose draws got all their copies gave its staging buffers away without returning them or
      taking them off the in-flight count; now they go back to the pool.
  - **Fixed: the once-a-second mesh table** (ReplayGameDraws.cpp UpdateWorld). The maps of every mesh's reach and
    short name (one entry per loaded level's vertex buffer) were built while holding g_Mutex. The render thread's
    captures and read back wait on that lock. Now they're built first and only swapped in under it.
  - **New log lines while it records** (every 10 s; the game thread's also when it stops):
    - "Replay (recording cost, render thread, diagnostic)": its work a frame, the most in one frame, how many frames
      over 2 and 5 ms. By part, each with its most in a frame: the draw hooks (in them the game's draws kept, the
      asset log's sampled draws, the draw log's), the copies at the passes' ends (staging buffers made in them), the
      read back (its maps), the crowds' copies, the asset log's and draw log's frame.
    - "Replay (recording cost, game thread, diagnostic)": the same for FaithReplay::Update's parts: the mesh table,
      the models' other levels, the asset log, the bundles, her pose, the moving models, the people, the game's draws
      matched to people, the vehicles, her sounds, gun effects, mission items, MagRope, loads noted, the rest. Also
      "the frame it started / stopped recording in took N ms".
  - **Still to find** (the next recording's lines say which): what makes the 8 - 34 ms game-thread frames, and how
    much the asset log's draw sample (every 15th frame, every draw) and the draw log's (every 60th) cost.
- **REPLAY > Mission test removed; the replay fixes it led to kept** (the user, 2026-11-04 00:0x: "Keep anything intersting
  you found and fix stuff that might need with the actual replays otherwise just remove it"). Build 664BD4741D8D, built
  00:11:04 at Idle from the shared folder (526CE18DC458's sources with the changes below). INSTALLED 00:11:27 with the
  game closed (game folder + dist, hash checked 30 s later). **Not tested in game.**
  - **Removed:**
    - Camera/MissionTest.cpp, MissionTest.h and MissionTestPoints.h. Kept outside the project in
      Mission_Audit_2026-11-03\removed_mission_test\ for their decompiled addresses.
    - Their calls in Main.cpp (save restore, Present, WM_CLOSE), CameraManager.cpp (Update, DrawUI) and
      FaithOutfit.cpp (ServerUpdate); the project's three entries.
    - The helpers only the test used: FaithReplay's test API (RequestRecordOrStop, RequestPlay, RequestStop,
      ForgetIfIdle, SetFaithProtected, IsRecording, IsPlaying, RecordClock, PlayTime, RecordedLength, Message,
      Summary), replay_assetlog's RunningMissions, MissionEntity and MissionState, and util::log's Warnings and Errors
      counters.
    - The DLL has no "Mission test:" text left.
  - **Done by hand first (game closed, 00:06:42):**
    - The user's save was put back from the run's copy (Run 2026-11-03 23.25.50\Save before the test, 4 files,
      same bytes after). The save at the test's end is kept beside it ("Save at the test's end (not used)").
    - restore.txt and autorun.txt renamed, so nothing waits for code that's gone.
  - **Kept (the replay itself):**
    - ReplayGameDraws' probes: every draw of a mesh in its probe frame (03B8932CE546).
    - ReplayShapes' copies drawn only 10 cm (+ 0.4 % a metre) in front of the game's surface (526CE18DC458).
  - **New for the replay:** a still model's copy that the game still has but doesn't draw is left out while the camera
    is within 3 m of it (NearCameraSkipMetres; ReplayShapes::Draw::LiveCopy from ReplayModels' CopyLive). The game
    doesn't draw a prop that near the camera, not even while it records. The test's start points 9 and 20 had an air
    conditioner's one-colour copy over half the picture. Unverified in game.
- **Replay copies drawn only clearly in front of the game's surface; Faith protected while the test records; a pause
  before each start point's load** (the user, 2026-11-03 23:3x: "Yes build them last try if this doesn't work again and
  crashes let's remove the whole testing thing"). Build 526CE18DC458, built 23:37:46 at Idle from the shared folder:
  6D539F65DA49's sources plus Camera/ReplayShapes.cpp, ReplayShapes.hlsl (a comment), FaithReplay.h/.cpp and
  MissionTest.cpp. INSTALLED 23:38:13 with the game closed (game folder + dist, hash checked 30 s later). The run by
  itself (every start point) goes on at start point 6 at the game's next start. **Not tested in game.**
  - **Grey copies, second fix:** the 23:25 run on 6D539F65DA49 still had one-colour copies over the game's own
    surfaces (Icarus Tunnel Meeting: the tunnel's ceiling over most of the picture; Flow Tutorial: a wall).
    - Those copies lay right on the game's own surfaces. ReplayShapes' shader draws a pixel unless it's farther than
      the game's depth plus a slack, and the slack let a pixel 3 cm (+ 0.05 % a metre) behind it be drawn.
    - Now a copy's pixel must be at least 10 cm (+ 0.4 % a metre) in front of the game's surface (DepthNearer; the
      constant's sign flipped, the shader unchanged).
    - A model the game really doesn't draw (a door, a gate, glass) has what's behind it farther away and still shows.
      One flush with a wall shows only where it stands out from it.
  - **Faith's protection:** FaithReplay's KeepPeople sets util::hooks::ProtectFaith each frame. It now holds while
    the test asks (FaithReplay::SetFaithProtected) as well as while it plays. The test sets it before recording and
    clears it at the run's end.
  - **The test:**
    - It waits 5 s after a start point's playback before asking the next load (both loads that didn't end were asked
      in the second the last playback stopped, as all were; whether that matters isn't known).
    - The stuck-load note now says what was seen: Faith didn't come back in 60 s and no loading screen showed.
  - **The run on it (from 23:42:21, at start point 6):**
    - 6 - 8 OK (1 - 3.7 mean). The big one-colour copies are gone.
    - 9 LOOK (about 11): one small grey copy where the recording shows nothing, a model the game didn't draw while it
      recorded either.
    - 10 - 15: LOOK or OK, no hangs.
  - **Why loads hang and pictures go missing:** the game stops drawing while its window isn't in front (windowed,
    FullscreenEnabled 0), and the load waits for that.
    - At 23:53:27 the tools' Present stopped (no Frame line until 23:55:02, "1276 frames in 96 s"), while the camera
      hook went on at 200 a second. So 16 had no pictures.
    - 17's load (asked 23:54:17) waited: Faith back 23:55:15, "loaded in 52.5 s", OK. That was right after the user
      was told at 23:54 to bring the game to the front.
    - The same matches 23:25:53 - 23:27:19 (no pictures at 1), and 23:30:17 (5's load never ended: drawing never came
      back within 60 s). Each was while the user was likely reading the session's messages.
  - **00:01:24, start point 38 (Grid_Node_Anchor:_Mission_Start):**
    - The level went and none came (0 sub-levels on both sides), with the game drawing at 200 a second: the same as
      Benefactor:_Mission_Start at 22:01. It was not a background window.
    - All 37 of its AutoloadSublevels are shipped. So "a missing sub-level stops the load" (the 22:2x entry) doesn't
      explain it.
    - Both loads that hung this way are a mission's plain "<mission>:_Mission_Start" with ApplyDebugFlags True and
      debug flag values. The 22 Release section start points (ApplyDebugFlags False) and Release:_Mission_Start (no
      debug flag values) all loaded. Two of two, not proven.
    - The run so far: 38 of 504 in 26 min (7 OK, 14 LOOK, 2 FAILED, 15 skipped). Start point 21 (KrugerSec) recorded
      and played 20 s: Faith's protection held.
    - The grey copies of near-camera props remain (9, 20, 22): models the game doesn't draw that close to the camera
      while it records either.
  - **If this run fails again and crashes, the user wants the whole mission test removed.**
- **REPLAY > Mission test: the replay teleport's done sent by the test when the game's doesn't come; the manager's
  events logged** (after the run by itself of 22:48 on 1DBA564AA511). Build 6D539F65DA49, built 22:56:39 at Idle from
  the shared folder: 03B8932CE546's sources plus Camera/MissionTest.cpp (22:56:16). INSTALLED 23:07:19 at game close
  (game folder + dist, hash checked 30 s later), in place of 1DBA564AA511. **Not tested in game.**
  - **The 22:48 run** (63 missions, the game's replay; the user stopped it at 22:51:16):
    - Benefactor: the manager was found (0x63ACDC80), the map's replay asked (event 0x7DD57BA0, the hooked lookup gave
      the mission once), and Faith moved 6.8 s later, to Benefactor's area (24.6 m from layer52's spawn location 7).
    - But the mission's state stayed "available 1, running 0, replay 0" for the 2 minutes the test waited. So the
      teleport's done (0x68FB16FC, which sets replay state 2) never reached the manager, or found no mission.
    - It was recorded anyway; its playback stopped by itself at 18 of 20 s, no reason given => FAILED. The user then
      stopped the run, during The Meta Grid's ask.
    - The screen wasn't white in its pictures, so the teleport's fade had gone.
  - **Where the done comes from [data]:**
    - gameplay/logic/pf_teleportplayer sends it (0xE9785E6B) when its gameplay/logic/zonestreamertransition does.
    - That sends 0xDEC1A06A on zonestreamertransitionchannel and waits for that channel's 0xE9785E6B.
    - layer42_zonestreamingcontrol answers it: a 0.3 s delay, then three ZoneStreamerQuery entities (Mode 1, one per
      zone-streamed sub-level), all three done through an EventAndGate and a PamDeferredOperationsControl.
    - Why that didn't answer isn't known. The tools add a streaming focus round the camera and keep the Shard's zone 55
      in; whether those hold the query isn't known either.
  - **The 23:11 run on it** (re-armed; the run stopped itself after 6, the game closed itself at 23:17:23, 7 s):
    - Benefactor, The Meta Grid, Grid Node Downtown, Back in the Game and Grid Node Anchor: the manager got each ask
      (the new log), but Faith was never moved => FAILED.
    - Old Friends (23:12:27): moved 2.1 s after the ask. The game's own done never came (only the test's is in the
      manager's log). The test sent it at 23:13:00 and the manager took it: the replay state went to 3, the other
      missions' availability was looked at, and the mission's world was streamed (the asset log: StreamOut, its data
      StreamOutOnCompleted 1). It stayed at 3; recorded where Faith was => OK (pictures 1.1 - 1.3 mean).
    - The missions asked after it had replay state 1 already at their ask, and none was moved.
  - **What the teleport waits for [data]:**
    - layer42's queries are ZoneStreamerQueryEntityMode_AllStreaming (Mode 1 of FocusPoint, AllStreaming, AllFoci,
      AllFociAndStreaming; the names from the game's reflected enum): they wait for all the zone streamer's
      streaming.
    - The tools' settings then: StreamAroundCamera false, no Always or Never districts, StreamRealShard true (GAME >
      Districts > the real Shard). That adds a focus round the real Shard's zone 55 to the buildings' streamer, far
      from Faith ("now also loads the city around the camera or the Always districts", 23:10:52).
    - Whether that focus keeps "all streaming" from ending isn't known (the query's code wasn't read). It's the one
      extra thing the tools ask of the streamers then.
    - The fade the teleport waits on before moving Faith can't hang: gameplay/fadetowhite gives its faded-out event at
      once when there's nothing to fade.
  - **23:22 - 23:29** (the real Shard turned off by the user in the menu, 23:22:44; the streamers "back to loading
    around Faith only" 23:22:58):
    - A replay run (23:23:55) was stopped by the user 21 s in. Benefactor's ask reached the manager, but Faith hadn't
      moved yet, so whether the Shard holds the teleport's done is still not known.
    - Two start-point runs were stopped at once. Then "every start point" from 23:25:50:
      - Release:_Mission_Start: "no pictures to compare" => LOOK. The tools' Present ran about 11 times a second
        then (Frame line 23:27:19: 944 frames in 86 s, while the game's camera hook counted 200 a second), likely
        the game's window in the background.
      - Release:_First_Warehouse_Wallrun: OK, pictures 1.3 - 1.7 mean (the 21:40 run's was 40 - 63, LOOK).
  - **23:30:17, the "every start point" run's 5th, Release:_Third_Warehouse_End_Combat** (which loaded at 21:40):
    - The new level did load (physics worlds made 23:30:18 - 20; 59 sub-levels in on both sides), but Faith never
      spawned, and no loading screen was seen.
    - The tools' Present-side frame lines stop at 23:30:19, while the camera hook goes on at 200 a second. So the
      game stopped presenting at that load.
    - In LevelGone_2026-11-03_233038.txt, the busiest thread, 35024 (the one that shows the vistas), is in the
      game's message dispatch (0x142A386C1 on its stack), no tools frame on it.
    - The test's 60 s guard took it: FAILED, run kept, the game closed (its quit stuck, then ended by the tools at
      23:31:57). The note's wording ("its level went, and no ... other level came") is wrong for this case: the
      level came, Faith didn't.
    - So the stuck loads aren't only the missing-bundle start points. The 23:25:50 load also had the tools'
      Present at about 11 a second for 86 s (it came back). Cause unknown.
  - **Found: the test's protection of Faith while it records doesn't hold.** FaithReplay sets
    util::hooks::ProtectFaith(m_State == State_Playing) on each update. The log has "takes no damage" and "takes
    damage again" in the same second, 23:25:34 and 23:26:07. Not fixed yet.
  - **Fix and diagnostic:**
    - With Faith moved by the replay and the replay state not 2 after 30 s, the test sends 0x68FB16FC to the manager
      itself, with its lookup naming the mission, as the teleport's prefab would. It notes that in the line.
    - While a replay run goes, a hook on the manager's events (0x143A74BF0, slot 4; byte-checked before it's made)
      logs each event it gets, up to 200 a run, and whether the lookup gave the test's mission. The next run shows
      whether the game's done ever comes. Once the hook is made, the code checks of that function skip its first
      bytes.
- **Replay: no more one-colour copies over models the game draws** (the user, 2026-11-03 22:3x: "Yes, fix them", on the
  grey shapes in the mission test's pictures). Build 03B8932CE546, built 22:49:52 at Idle from the shared folder:
  1DBA564AA511's sources plus Camera/ReplayGameDraws.cpp (22:48:51) and Camera/MissionTest.cpp (22:49:15). Staged
  22:50, then replaced by 6D539F65DA49 (next entry), which holds it. **Not tested in game.**
  - **What the pictures showed (the 21:40 run):** in items 17, 18, 20 and 22, a big flat grey shape over much of the
    playback. The replay drew its own copies of billboard_01_1024x1536 (all 20 s, and the asset log has the game
    drawing that mesh at all 20 of them), aircon08_s_160x40_b (19 s) and an industrial wall module (11 s).
  - **Why [code]:**
    - ReplayModels::Play draws a recorded model's copy when replay_gamedraws::GameDrawsModel finds none of the game's
      draws of its mesh within 1 m of its place. The same places decide which kept world draws are drawn.
    - Those places came from ProbeDraw: one draw of the mesh read back every 30 frames, its instances' places
      replacing the mesh's.
    - Where the game draws a mesh in more than one draw a frame (its instances in more than one batch), only the first
      draw's instances were known. The recorded models in the other batches counted as not drawn all through.
    - The copies are drawn both sides alike. So behind a billboard, which the game draws one-sided and you see through
      from the back, the copy showed as a huge grey plane.
    - That several batches is what happened to these four is the reading of the code and the asset log's line. It
      wasn't measured: the asset lists of those runs were overwritten.
  - **Fix:**
    - In the frame a mesh is probed, every draw of it is read back, and their places go together by the frame they
      were drawn in. Later draws are read up to 64 a pass in all; a new mesh starts only while there's room for 16, as
      before.
    - A frame missing one of the mesh's draws (no room, or a draw that couldn't be read) has that mesh's places
      dropped. The last complete ones stay until they're stale; then it's "not known" and nothing changes.
    - A batch read back after a later one doesn't replace newer places.
  - **Also (MissionTest.cpp):** a run by itself's own first start no longer logs "the run by itself that was waiting
    (not begun) is replaced by this one" (22:48:21). Its file was ended and written again with the same choices, so
    nothing was lost.
- **Faster rain and snow, third step: the snow's pressed lists and the rain's kept-out boxes skip what can't reach;
  and the lag at 21:17 - 21:27 found** (the user, 2026-11-03: "Keep going", "make sure nothing breaks"; then "read the
  latest performance log and find out why the game lags / check if its the solid street or the many pedestrians cars").
  Build 1DBA564AA511, built 22:23:32 on 2026-11-03 from the shared folder (by the mission test session's compile check;
  the weather session's MSBuild after its "done" had nothing left to do, every changed object compiled after its source's
  last save); INSTALLED 22:45:09 when the game closed (game folder + dist, hash checked 30 s later; the weather session's
  scratchpad stage_1DBA564AA511\install.log). The mission test session's 03B8932CE546 (these sources and its replay draws
  fix) is staged over it. It holds the snow sessions' pile light and road paint (WetLook.hlsl
  f51f0e94ce1a), street level's faster planning (GroundLevel.cpp 00795D012888), the road traffic session's crowd test,
  the mission test's skips and the Dogen door. **Not tested in game.**
  - **The bounds (the same result):** SnowPrintsAt's three lists (her prints, the KrugerSec's boots, her hand's stamps),
    SnowTrailAt, SnowRollsAt, SnowStampsAt and RainKeptOut went through all of their list at every pixel they're asked
    about. WetLook.cpp's MakeListBounds puts a box round each list, from the list as written in the constants just
    before they go out: each item's box is the shader's own test that lets it through, 5 cm wider (prints and boots 0.3
    m across and 0.35 up and down; hand stamps 0.3; a slide stretch's rectangle inside the box round its ends that far x
    1.4143 out; rolls 1 m and 0.5; a body stamp's -4 - 132 x -4 - 260 texel parallelogram with its rise; a rain box's
    height and 10 cm round its sides). Outside a box the loop isn't gone through: none of its items would have let the
    point through, so each gives what it gave. Checked offline: each of 520,817 points an item's own test lets through
    (300 random sets of each list) is inside its list's box. New WetParams pairs g_SnowPrintBounds ... g_RainBoxBounds
    (60304 - 60528).
  - **Shaders (all of them compiled from f51f0e94ce1a at Idle priority):** the code changed only in WetLookPs (36,148
    slots, 43 temps), RainPs (22,441, 41), SnowGroundPs (18,188, 29: the road paint's) and GbufferWetPs (11,059, 35);
    the other seven differ only in their listing, WetLookVs byte-identical.
  - **Why the game lagged at 21:17 - 21:27 (CT.log):** not street level or the crowds. Every part of the camera hook
    slowed 10 - 25 times at once (the replay's check from 0.006 to 0.16 ms), with 5 - 9 shader compiles running at normal
    priority (fxc, each a whole core of the 16) from 21:16:42 and 21:18:51. Set to Idle at 21:27:2x, frames per 10 s
    went from 606 - 816 to 1,136 - 1,161 with nothing else changed. Every session now builds at Idle priority.
    - Street level was 1.0 - 1.25 ms of the game thread a frame then (about 3,080 bodies) and its flushes about 50 ms
      a second: the street level session's 00795D012888 makes the same bodies with less (offline 0.69 -> 0.38 ms a
      frame, 0.29 -> 0.09 ms a flush).
    - The traffic's own rules were 0.35 - 0.75 ms of CPU a frame over the job threads; the game's own cost of twice the
      crowd isn't in any line: the crowd test (MISC > Performance, "Run a crowd test (80 s)") measures it.

- **REPLAY > Mission test: start points the game can't load are skipped; Faith protected while it records** (the user,
  2026-11-03 22:08: "did some runs read the log and try to find anything valuable also the game is in an infinite loading
  loop find out why"). In the shared folder 22:2x (MissionTest.cpp 932AA6431C29, MissionTest.h 916B070C48F4,
  MissionTestPoints.h 1908130DED93). Build 1DBA564AA511 (22:23:32, at Idle; the top session checked its other sources
  final and made it the combined build); STAGED by the top session 22:24, INSTALLED 22:45:09 at game close (game folder + dist, hash checked
  30 s later; its scratchpad stage_1DBA564AA511\install.log; the test's strings checked in the installed DLL). The armed run by itself (autorun.txt) starts at the first load after it. **Not
  tested in game.**
  - **The 21:40 run** (installed AC4CD319E993, "every start point", 20 s): Release's 22 start points loaded (7 - 13 s
    each), 3 OK, 18 LOOK, 1 FAILED. Then the load at Benefactor:_Mission_Start (22:01:55) took the level out and never
    began the new one:
    - no "Physics: a world made" (every load that worked made its worlds within a second), no loading screen;
    - the server's and client's level streaming 0 sub-levels, the client's state 6;
    - the game left so at 200 fps (LevelGone_2026-11-03_220216.txt). The test gave up 5 min later (22:06:56).
  - **Why [data; the game's code not read]:**
    - The city's LevelDescriptionAsset (levels/sp/sp_maincity/sp_maincity/description, patched) lists 582 start points,
      each with AutoloadSublevels.
    - Benefactor:_Mission_Start's 41 include Levels/SP/SP_MainCity/CentralCityBuildings_ZS_ZS_116. No bundle of that
      path is in any of the game's 622 (Data or Patch); 113, 115, 117 - 119 are.
    - None of Release's 22 asks for a missing one, and all 22 loaded. Benefactor:_Mission_Start was the first in the
      list that does.
    - 192 of the test's 504 start points ask for one of 13 missing sub-levels (CentralCityBuildings_ZS_ZS_44 in 44,
      _116 in 40, _138 in 39, Ground_ZS_ZS_17 in 37, ...). That the missing bundle is what stops the load is the
      likeliest reading of this; the load's code wasn't followed.
  - **Fix:**
    - MissionTestPoints.h: each start point's Missing (Mission_Audit_2026-11-03\startpoint_bundles.py). The test writes
      "not asked ... => SKIPPED" for those and doesn't ask.
    - Drone Works', Prisoner X's and Sanctuary's first start points moved to ones that load. 27 missions have none that
      load (Benefactor, Top of the World, Family Matters, Exit Strategy, ...): their missions can only be tested by the
      game's replay mode.
    - A start point's load with Faith gone 60 s and no loading screen seen fails that one. The run ends; a run by itself
      keeps its file and closes the game, and goes on at the next start.
    - util::hooks::ProtectFaith(true) before each recording (the replay already protects her while it plays). Item 21
      (Release:_Second_Rooftop_Encounter_) FAILED because KrugerSec shot Faith dead as the recording ended (35 gun
      effects recorded, "Faith isn't spawned" at 22:00:42), so the playback had no Faith.
    - The end line counts the skipped ones too.
  - **Found in the 22 runs' pictures (not fixed here):**
    - **The replay draws one-colour copies of models the game also draws, across the view.** 17 (an industrial wall
      module, drawn by the replay 11 s), 18 (billboard_01_1024x1536, 20 s, "the game drew it too" at all 20), 20
      (aircon08_s_160x40_b, 19 s), 22: a big flat grey shape over much of the playback's picture. That's a replay fault
      for 1:1 playback.
    - **The camera isn't replayed:** in 2 and 10 the playback looks another way than the recording, so their numbers
      compare different views. The pictures compare the live game camera.
    - 1: the intro cutscene in the recording against the live one. 6: the game's map screen was up in the recording.
  - **Correction:** ApplyDebugFlags is True on the 159 plain start points (PamStartPointEntityData) and False on the 345
    mission ones. DCFAD69692D0's entry and section 9 of the research note said False on all 504. All carry debug flag
    data. Whether the retail game applies them isn't known.
- **MISC > Performance: a crowd test, what the extra road cars and walkers cost the frame** (the weather session for
  the user, 2026-11-03, who asked whether the many cars and people make the game lag). In 1DBA564AA511 (the weather
  session's build, 22:23) and the builds after it.
  - **Tested 23:13 (6D539F65DA49), in one place at 158 fps:** the frame wasn't slower with twice the crowds (as set
    6.22 - 6.38 ms per 10 s, the level's 6.56 - 6.62). The crowds' CPU was 0.38 ms a frame over the job threads,
    against 0.29 with the level's; most of it is the tools' rules, the game's own steps 0.05 ms. The test's third phase
    was spoiled by the replay starting to record at 23:14:37, so its own summary says nothing; the research's section
    11.5 has the clean numbers.
  - **Tested again 2026-11-04 00:28, in a busier place at 113 - 116 fps:** the same, 8.4 - 8.9 ms per 10 s with the
    crowds as set and with the level's (medians 8.45 and 8.47); the crowds' CPU 0.51 against 0.40 ms a frame. Its first
    phase was spoiled by a time-of-day jump (the game's lighting reset), so again its own summary says nothing
    (section 11.6).
  - **The test** ("Run a crowd test (80 s)"): in one place, the crowds as set (2,000 road cars, twice the walkers),
    then the level's (1,210 cars, its walkers: the extra ones go unseen at once, in sight too), then as set again
    (they show again where there's room). Each phase is measured for 20 s after 5 s to settle (the last 10 s: cars
    coming back need a gap). CT.log gets each phase's frame times, the cars and walkers shown, and the crowds' CPU a
    frame; the summary is the frame time with and without the extra ones, and how much it wanders by itself.
  - **New in the minute lines:** the game's own car and walker steps, and its per-frame update of every crowd placement
    (its respawns, its step and the rules in it, its render copy), so the game's crowd work is measured, not only the
    tools'.
  - **The car rules ~10% quicker, with the same result:** the neighbour grid keeps each car's cell instead of working
    it out again for every neighbour looked at. Offline, every car's record in every frame came out the same as
    820661E1B5DF's (and with the test off the walkers' too).
  - Not taken out in the test's second phase: the crowds' room (sized for the raised counts when the level loads) and
    the game's car step still moving the unseen cars (that's in the CPU numbers).

- **No more grey boxes round parked cars in the snow, or sharp ones in the haze** (the user, 2026-11-03, with a
  screenshot from high over a street under snow: "cars on rain/snow have a box around them make it invisible fix it no
  mistakes"). In the shared folder 21:51 (WetLook.hlsl f51f0e94ce1a); in the weather session's build 1DBA564AA511
  (22:23), STAGED: installs when the game closes (its three shaders checked in the DLL). **Not tested in game.**
  - **Correction (2026-11-04):** the boxes were the cars' own shadow planes, not the paint: see "No thin rectangle round
    the traffic's cars in the snow" above. The change below stays, for paint that's brighter than the road round it.
  - **What they were thought to be:** the game's painted parking bays round the cars (and the zebra crossings' stripes), not the
    tools' boxes. The snow isn't laid on what's much brighter than the ground round it, to keep the game's HUD, its
    markers, glows and effects visible; a thin white line on dark asphalt counted, so it showed as a grey line in the
    snow. The rain's and the snow's haze is kept off the same things, so the lines stayed sharp in the haze (the
    rain's "box").
  - **Now:** where the game's own surface colour (its G-buffer, which has the road's paint in it) is as bright against
    its surroundings as the picture is, the brightness is the paint's own: the snow is laid on it and the haze covers
    it. The HUD, markers and effects, drawn over the ground after the game's lighting, still show through.
  - Only the wet look's three pixel shaders changed (the snow's copy uses one register more, 29); the other 13 compile
    byte for byte the same.

- **The snow piled against walls without the streaks on gravel** (the user, 2026-11-03, with a screenshot of a gravel
  roof: "Snow is still on walls streched fix it no mistakes"). In the shared folder 21:33 (WetLook.hlsl 89b070d9365c);
  in the weather session's build 1DBA564AA511 (22:23, with the road paint), STAGED: it installs into the game folder
  and dist when the game closes. Its two snow shaders are checked byte for byte in the DLL. **Not tested in game.**
  - **Why:** every pixel of the pile up the wall takes its light from the ground just in front of its own foot: 3 small
    taps 8 px out. On gravel every stone is lit its own way, so each column of the pile took other stones' light and
    drew it up its whole height, in streaks a pixel or three wide. (The 19:11 fix made the foot itself exact and its
    look follow the drift; the light was still each column's own.)
  - **Now:** the pile's light comes from points on the ground fixed on the screen, 16 px apart at 4K, each a small
    patch of the game's light. The 4 round the spot in front of the foot are blended by how near it they are, so the
    next column over gets nearly the same light, and it changes smoothly along the foot. Only points on the ground
    there count, not the wall, a box's top or what's seen past an edge. Without the game's G-buffer, the picture round
    the foot is taken smoothly too.
  - **Checked offline** on a stand-in for gravel (random light per pixel): the light's change from one column to the
    next went from 0.053 on average (0.23 at most) to about 0.008 (0.05 at most), at any slant of the foot.
  - **What it gives up:** light and shade on the ground sharper than about 16 px (a shadow's edge across the foot) is
    soft on the pile.
  - Only the two snow pass shaders changed: the same registers (43 and 28), slightly fewer instructions (35,779 ->
    35,490 and 17,803 -> 17,531). The other 14 shaders compile byte for byte the same.

- **REPLAY > Mission test: the game's replay mode fixed (its mission manager is found), and runs by itself** (the user,
  2026-11-03 21:0x: "find a way to automate this even further"). Build B93E0931A447, built 21:18 on 2026-11-03 from the
  shared folder. **Not staged:** it also holds WetLook.cpp (21:16) and WetLook.hlsl (21:15), edited after the weather
  session's AC4CD319E993 with no shader header made since 21:00. The top session (whose edits they are) said 21:2x: hold
  it, they're mid-work; its next build, after the two snow sessions' WetLook.hlsl edits and a shader recompile, will hold
  these 21:18 mission-test sources too, and it sends the hash. **Not tested in game.**
  - **Armed 21:3x** (the user: "Yes, arm it"): autorun.txt written from outside the game (each mission by the game's
    replay, 20 s each, save put back, game closed at the end). The installed AC4CD319E993 ignores it; the first build
    with these sources starts the run 20 s after the city is up.
  - **What the 20:44 - 20:51 tries showed (CT.log, MissionTest\Run 2026-11-03 20.46.11):**
    - "Every start point", 60 s, stopped after 2 of 504:
      - Release:_Mission_Start: the playback stopped by itself at 52.6 s, no reason given ("Replay: stopped" with
        nothing before it) => FAILED. Its pictures differ a lot (mean 41 - 87): the recording is the game's intro
        cutscene, and the playback shows the live cutscene, which went on meanwhile.
      - Release:_First_Warehouse_Wallrun: 8 people and a vehicle recorded, played 60 s => LOOK (worst cell 40.5). The
        red guide runner of the recording isn't in the playback.
    - "One mission (the game's replay)", 20:51:41 - 20:51:50, 4 tries: "not started: the game's mission manager
      wasn't found" each time. So the replay mode never ran.
  - **Why the manager wasn't found [decompile]:**
    - The test took the manager 0x58 before its list node, where mission entities have theirs.
    - The manager registers its node at + 0x30: 0x143A709A7 is lea rcx, [rdi + 0x30] with type info 0x142864380.
      The game's own getter 0x1439DA130 takes 0x30 off, and the constructor writes the vtable 0x141C7ADD0 at + 0
      (0x143A72687).
    - The vtable check refused the wrong address, so nothing was called with it.
    - The list's realm global [0x1425469E0] is set once per level, to the level's own object (0x14333F6D3), not per
      thread. So the lookup works from the menu's thread too.
  - **The replay starts the mission by itself [decompile]:** this was the open question of DCFAD69692D0.
    - The mission's update 0x143A7A380 takes the replay state (+ 0x124):
      - from 2 to 3, once no other mission's world (+ 0xD0, + 0xD8) is in, asking for its own world (message
        0xD27BC17E);
      - from 3 to 4, once its world is in (0x143A7A1A0).
    - At 4 it fires 0xAD0E16FE on the manager (made as 0xC347050F - 0x1638EE11), then sets every mission's state back
      to 0.
    - In the city's LevelData, the manager's 0xAD0E16FE goes to layer5_setup's replay control channel
      (gameplay/schematicchannels/replaycontrolchannel) as 0xA439B114.
    - In gameplay/logic/pf_missioninteraction, that channel event goes to the prefab's output 0x3F2ED268: the same
      output the marker's interaction (through its EventSync) and the progression flag's start give. So the mission
      starts as if Faith had pressed its marker. No button press is needed.
    - The open-world starts (pf_deliverymissionstartlogic, pf_diversionmissionstartlogic) use the same channel.
  - **Run by itself (new; the menu's tick, on by default):** the choices and where the run got to are kept in
    Cinematic Tools\MissionTest\autorun.txt (key=value lines).
    - While the file is there, a run starts by itself 20 s after the city is up with Faith in it (no loading screen).
      If it can't start, it's tried again every 30 s, logging each new reason.
    - Each item writes its list position as "going" when it begins, and the next position when its line is written.
    - At the next start of the game the run goes on in the same folder, at the next one:
      - the one going when the game ended gets a FAILED line ("the game ended while this one went": a crash, or a hang
        it was ended in);
      - if the game's window was asked to close while it went (Main's WM_CLOSE calls missiontest::WindowClosing), it's
        done again.
    - The counts and minutes carry over. The save copy made at the run's start is kept, and the marker is written
      again, so the save still goes back after the run.
    - "Close the game when it's over" (on by default): 5 s after the run's last line, the tools post WM_CLOSE to the
      game's window, as its X does. Not when it's stopped from the menu.
    - Stop the test, the menu's "Forget it" (shown while one waits) or deleting the file ends it. The file is kept as
      "autorun <stopped|ended|forgotten|replaced> <time>.txt".
    - A menu start ends a run by itself that was waiting.
    - A run also starts by itself when the file is put there from outside the game.
  - **Also:**
    - The lines are numbered in the whole list ([number/total]), also when it starts further on.
    - A mission the game refuses at once (no such mission entity) no longer counts towards the 3-in-a-row stop. Only
      timeouts do.
- **Faster rain and snow, second step: the interiors and the falling water by where they reach; the test's CPU
  numbers right** (the user, 2026-11-03 20:3x: "Tests done", after the rain and snow tests on 99559FA92411). Build
  AC4CD319E993, built 21:01 on 2026-11-03 from the shared folder; INSTALLED 21:01 with the game closed (game folder + dist, hash checked, still there 30 s later); it also holds the snow session's IceLook change (WetLook.hlsl 060b1be6ba22, WetLookIcePs.h 39a9d1770b45: the ice over pools lit from the sky's colour, not the game's water with its reflection). **Not tested in game.**
  - **What the tests showed (CT.log 20:27 - 20:29, 4K):** rain 88 fps with the tools' drawing, 116 with it paused;
    snow 108 and 128. The kept-dry grid of 99559FA92411 worked: the boxes' part of the info pass 0.00 ms (the info pass
    0.32 - 0.42 ms a frame, 1.57 at 18:57 - 19:00 before the grid). Left: in the rain the wet ground's puddle tests
    0.97, the rain in the distance 0.80, the drips, drainpipes and splashes 0.59, the water down slopes 0.41, the shelter
    and interiors 0.25; in the snow the shelter and interiors 0.53, the piled side 0.53.
  - **The game's interiors by where they reach (the same result):** InteriorAt went through all of their boxes, up to
    32, at every point it's asked about: each pixel's shelter (the info pass in the rain, the pass at Present in the
    snow), each drop of the rain in the distance (RainOpenAt), the snow's flakes and the ice. WetLook.cpp's BoxGrid
    (the kept-dry grid's, made general; MakeInteriorGrid) lists them in a 32 x 32 grid over x and z, each in every cell
    its sphere (InteriorAt's reach, 25 cm wider) comes over; InteriorAt goes through its cell's, in the same order (the
    prisms as before). Checked offline in 32-bit floats against the old loop: bit-identical at 675,504 points x
    "known" values of 250 random interior sets (named and generic, turned and tipped, points in and round them, just
    inside their spheres, on the cells' borders; 81,239 inside one).
  - **The falling water by the frame's tiles (the same result):** every pixel in the box round all the drips went
    through all of them (48 while it rains on the roofs), and likewise the drainpipes' water and the splashes.
    WetLook.cpp's FallingTiles lists each stream in the 8 x 8 tiles of the frame its own box comes over (the box its
    list's bounds were already made of, a pixel wider); DripsAt, PipesAt, SplashesAt and HerSplashAt go through their
    tile's (each takes its streams' most, so the order doesn't matter). A list not made in a frame: all of it
    everywhere. Checked offline: each of 2,033,994 pixels inside a stream's box (400 frames from 720p to 8K, boxes on,
    off and across the edges) finds its stream in its tile.
  - **MISC > Performance: each phase's CPU time up to its own end.** It was measured up to the test's end and divided by
    the phase's frames, so phases 1 and 2 had 3 and 2 times their own (the 20:28 test's "camera hook 2.67 ms" in phase
    1 was about 0.85; the 11-02 snow test's 5.8 ms the same mistake). The 10-s lines of 99559FA92411 had it right.
  - New WetParams fields g_InteriorGrid (55152), g_InteriorCells[256] (55168), g_FallingGrid (59264),
    g_FallingTiles[64] (59280): 60,304 bytes in all (65,536 the most a constant buffer takes).
  - Shaders recompiled, temps all as before: WetLookPs 34,111 -> 35,654 slots, RainPs 20,773 -> 22,099, SnowGroundPs
    16,505 -> 17,689, QueryPs 7,320 -> 7,429, SnowVs 753 -> 863, IcePs 1,733 -> 1,833 (1,865 with the
    snow session's IceLook), GbufferWetInfoPs 599 -> 711.
    GbufferWetPs, NodesPs, SnowPs, IceVs: the same instructions (only their listing has the new fields); WetLookVs
    byte-identical. (More instructions in the shader, fewer gone through at a pixel.)

- **The ice over pools no longer turns grey towards the screen's edge** (the user, 2026-11-03: "When i have snow
  enabled water shader breaks at the edge of the screen", with a screenshot of a pool's ice white on one side of a
  sharp straight line and flat grey on the other; the grey part moves with the screen, on all the water). In the
  shared folder 20:59; in the weather session's build AC4CD319E993 (21:01), INSTALLED 21:01 with the game closed
  (game folder + dist; its ice shader checked byte for byte in the DLL, the old one not in it). **Not tested in game.**
  - **Why:** the ice took its light from the game's water under it, the pixel itself and the frame a few pixels round.
    The game's water mirrors what's on the screen, and towards the screen's edge, where what it would mirror is off
    the screen, it shows a grey instead. So the ice came out white where the mirror reached and grey past it, with the
    mirror's edge as a line across the pool that moved as the camera turned; something dark mirrored in the water
    showed as a dark patch on the ice.
  - **Now:** a pool's ice, and the snow dusting it, are lit by the sky's light (the same light the snow on the ground
    falls back to where the game's own light can't be told), so they look the same anywhere on the screen. Streams,
    walls and falls of ice are lit as before.
  - **What it gives up:** a pool's ice no longer darkens in shade: under a bridge or a sign it's as bright as in the
    open, and at night it's as dark as the sky, not lit by the street lamps.
  - Only the ice's shader changed (17 registers as before, 1,833 -> 1,865 instruction slots); the other 15 shaders
    compile byte for byte the same.

- **Faster rain and snow, same look; and where each frame's time goes, measured** (the user, 2026-11-03 ~19:0x: "After
  all the sessions are done find a way to improve performance in fps even with rain/snow enabled high priority no
  mistakes"). Build 99559FA92411, built 20:19 on 2026-11-03 from the shared folder (holds DCFAD69692D0's sources);
  INSTALLED 20:19 with the game closed (game folder + dist, hash checked, still there 30 s later). **Not tested in game.**
  - **What the 18:37 run's CT.log showed (at 4K, rain):** about 85 - 114 fps before the rain, 65 - 86 with it, 44 - 58
    from 18:59:30. The tools' dearest part was the wet ground drawn into the game's lighting (GbufferWet), 3.7 - 6.6 ms
    of the GPU's a frame; the wet look at Present 0.7 - 2.2 ms; the weather's game-thread work 0.4 - 1.1 ms; the scan for
    what's kept dry 2.5 - 2.9 ms once a second (one frame in each second). The 11-02 snow test had the camera hook at
    5.8 ms of CPU a frame, not broken down.
  - **The kept-dry boxes by where they reach (the same result, fewer tests):** every pixel of the wet ground's info pass
    (and of the wet look and the snow at Present where they work it out) went through every kept-dry box, up to 64 (the
    people, the traffic and the vehicles round the camera), each with a sphere test. WetLook.cpp's MakeDryGrid now lists
    them in a 32 x 32 grid over x and z round the camera (each box in every cell its sphere comes over, 25 cm wider), and
    WetLook.hlsl's KeptDryKind goes through its cell's boxes only, in the same order. A box whose sphere doesn't reach a
    point was skipped there at once, so the result is the same. Checked offline in 32-bit floats against the old loop:
    bit-identical at 650,340 points of 500 random box sets (people, cars, buses, plain and tilted boxes; points in and
    round the boxes, just inside their spheres, on the cells' borders). The gain isn't measured yet. New WetParams fields
    g_DryGrid (46944) and g_DryCells[512] (46960), 55,152 bytes in all.
  - **Measured from now on:**
    - CT.log's frame line every 10 s, "Frame (performance, diagnostic)": fps, the frame's times, how long the game's own
      Present call takes (long while the graphics card is behind and the game waits on it), the tools' CPU by section,
      their GPU at Present.
    - The camera hook's parts every 10 s, "Camera hook (performance, diagnostic)": each part's time a frame, the dearest
      first (the far-away buildings, the Shard, street level, the replay, the weather, the kept-dry boxes, waiting for
      the camera's lock, the free camera, the lights, ...).
    - "Wet surfaces in the game's lighting (performance, diagnostic)" has the ground's nodes apart from where it's open
      and kept dry.
    - After MISC > Performance's test, a new breakdown of that pass, like the wet look's: both its draws drawn again 13
      times in 3 frames, each leaving out one more part (WetLook.hlsl's g_Skip, b2, all 0 in every frame's own draws).
      Line "Performance test, the wet ground's parts in the game's lighting": the kept-dry boxes, the shelter and the
      interiors; the rain's rings, the other rings, the puddles' tests, the water down slopes, prints and tracks, the
      facing, the rest.
  - Shaders recompiled: WetLookPs (33,668 -> 34,111 slots), WetLookRainPs (20,547 -> 20,773), WetLookSnowGroundPs
    (16,285 -> 16,505), GbufferWetInfoPs (476 -> 599), GbufferWetPs (11,003 -> 11,048, temps 34 -> 35); temps otherwise
    as before. WetLookQueryPs, NodesPs, SnowVs, SnowPs, IceVs, IcePs: the same instructions, only their listing has the
    two new fields; WetLookVs byte-identical.
  - **Next:** a MISC > Performance test in the rain and one in the snow give both breakdowns; the dearest parts get cut.

- **REPLAY > Mission test: missions started the way the map's mission replay does** (the user, 2026-11-03 19:2x: "Looks
  like it can't go past the starting cutscene find out why and fix it"). Build DCFAD69692D0, built 19:41 on 2026-11-03
  from the shared folder (holds the road traffic session's 820661E1B5DF sources, and the ground-level session's new
  Resources\GroundProps.bin of 19:39, sha256 3A0AD0E829AE, checked whole in the DLL); INSTALLED 19:44:12 when the game closed (game folder + dist, hash checked, still there 30 s later). **Not
  tested in game.**
  - **Why (CT.log of the 19:23 - 19:27 runs):**
    - the load at `Release:_Mission_Start` worked: Faith was gone at 19:23:30 and back at 19:23:40, at (34, 6, 1193)
      in the Release area, and the test recorded 60 s and played it;
    - but the Release mission never ran: no stream-in of its world was asked, it never showed as running, and Faith
      still wore her late-game outfit (the mission dresses her in its own);
    - the save has Release done, and the game never makes a done story mission available again by itself
      (ReenableOnCompletedDelay -1);
    - the start points' debug flags are off in the retail data (ApplyDebugFlags False on all 504), so the start point
      only places Faith;
    - its opening ran into a mission that couldn't start.
  - **The fix: the game's own mission replay [decompile]:**
    - The server's PamServerMissionManagerEntity (type info 0x142864380, vtable 0x141C7ADD0, events in slot 4,
      0x143A74BF0) takes the map UI's event 0x7DD57BA0.
    - It asks 0x143A75AC0 for the mission to replay (MissionToReplay, from the UI's shared data). It gets that
      mission's replay start location (link 0xC85FDBEB to a spawn location of layer52_replaystartlocations), and its
      teleport prefab (link + 0x58) moves Faith there.
    - The prefab's done event 0x68FB16FC takes the other missions out (0x143A79520), sets the mission's replay state
      (+ 0x124) to 2 and runs its availability (0x143A77640).
    - 0x143A75AC0 has no other caller. The test hooks it (byte-checked) to name the mission it wants while it asks, and
      sends 0x7DD57BA0 itself on the server's update thread.
    - The manager is read from its type's list ([0x142864380 + (realm + 6) * 16] - 0x58, the realm at
      [[0x1425469E0] + 0x28]; vtable checked). A running mission is taken out first with the manager's
      AbortCurrentMission (0x975A3FCA).
    - The 63 missions with a replay start location (31 story and side-story, 32 open-world) are in MissionTestPoints.h
      (ReplayMissions).
  - **Each mission:** the test waits for the teleport (Faith moved over 20 m, or a loading screen) within 40 s. Then it
    waits for replay state 2 and the mission running, logging each change of its state (+ 0x110 - + 0x127). Once it
    runs, the test waits 10 s and then records and plays as before. If the mission hasn't run 2 minutes after the ask
    (it may wait for Faith to interact at its start: every mission but Release starts through
    gameplay/logic/pf_missioninteraction), it records anyway and says so.
  - The menu's Which: Each mission (the game's replay, 63), One mission, and the start point choices (for missions the
    save hasn't done). The result line gives the teleport, replay-state and running times and the state's changes.
- **Road traffic: 2,000 cars, fewer jams, a faster highway, and no cars popping in** (the user, 2026-11-03,
  after testing 90EFC3980468: "You can see cars disappearing and reappearing also lets decrease the cars down to
  1500, there are a lot of traffic jams right now", then "We can set the cars to 2000 however lets fix the traffic jam
  and cars on the high way should go a little faster"). Build 820661E1B5DF, built 19:21 on 2026-11-03 from the shared
  folder. It also holds the snow session's pile fix (WetLook, 19:12) and the replay session's mission test. STAGED
  19:23 (game running), INSTALLED 19:28:54 at game close over E6FD6AB497A9 (game folder + dist, hash checked, still
  there 30 s later). **Not tested in game.**
  - **What the test's log showed (90EFC3980468, x 2):**
    - about 1,100 car respawns a minute, each shown again where there was room, in view too;
    - about 120 a minute turned round or crossed the off-road line unseen (30 at x 1);
    - about 25 a minute made unseen when stuck (12 at x 1);
    - 1.7 million car-steps held back a minute (0.6 million at x 1).
  - **The cars: 2,000 [data].**
    - The highway (placement 83, the only one with CarsTwoLaneSuperFast settings) keeps the level's 400.
    - Placement 93 (one-way streets with a merge and two traffic lights, the only one-way placement of 65) keeps its 65.
    - The other 19 placements' 745 cars go to 1,535 (x 1535 / 745, rounded down; placement 186, the only two-lane one
      of 100, takes the 7 the rounding leaves). The car crowd's room is 2,149.
    - At night their share is smaller by as much, so nights stay as before.
  - **The highway goes faster:** at 667 cars (2,000 spread evenly) it's a car every ~15 m of lane at 230 km/h. A car
    just shown again, still picking up speed, slows the whole column behind it. Offline: 51 of its 60 - 70 m/s at
    667 cars, 58.5 at 400 (the x 2 build had 800).
  - **No pops in:** an unseen car (a respawn, one that waited, one done turning round) shows again only out of sight.
    The day/night share already worked this way.
    - A placement with none shown may show one anywhere: with none shown, its box sits under the city and the game
      stops updating it (section 7.6).
    - Shown cars never wait for unseen ones, so this can't cause a jam.
  - **Fewer head-on holds:** some streets are two one-way node lines 2.4 m apart. Two oncoming cars there counted as
    in each other's way (a car's width + 0.3 m), met head on and waited 5 s, then one went unseen. Oncoming cars now
    count only when they'd touch (0.05 m); a real overlap is still caught by the footprint test. Offline at x 1,
    placement 93 stopped 11% of the time instead of 36%.
  - **Still unseen in view, as at x 1:** a car turning round at a dead end, one on placement 102's off-road line, and a
    stuck one (5 s with a car it waits for, 6 s beside one, 3 s in a loop).
  - **Tried and dropped (pops against jams):**
    - Stuck cars in sight waiting 8 - 15 s for the view to look away: placement 93 stood 45 - 56% of the time with a view
      on it.
    - In sight, a car turning round driving round the road's end, or making the game's own turn: the cars at placement
      254's dead end turned into each other and stood 74% of the time.
    - In sight, a car waiting at the off-road line's start: placement 102 stood 80% of the time.
    - A follower passing on the full speed of the car ahead (not its speed x the cosine between them): the highway
      didn't get faster (51.4 m/s both ways).
  - **Offline:** 2,000 cars, 120 s at 30 steps a second, from five camera spots (`rt\sim`: SIM_VIEW, SIM_CARS,
    SIM_HIGHWAY, SIM_KEEP), against the x 1 build you called perfect.
    - Cars shown: 1,832 - 1,887 (1,169 at x 1).
    - Shown again in sight per 2 minutes: 0 - 2 (95 - 326 at x 1). Went unseen in sight: 8 - 183 (3 - 112).
    - Stopped 2.0 - 3.3% of the time (2.3%), longest stand 13 s (11 s). None inside another, 99.2% on tarmac, the
      game's node lists clean.
    - Highway 58.5 m/s (58.3). Placement 93 stopped 5 - 22% (36%).
    - Queues remain at placement 235's traffic light (5 - 17% stopped) and placement 188's three-way merge (6 - 15%);
      both 2% at x 1. 188 can't be told apart from 222 and 254 (same settings and count).
    - The rules: 2.5 ms a frame for all car placements on one thread (1.25 at x 1). In the game a placement steps about
      13 times a second, not 30. The x 2 test's log: the longest step 2.75 ms.
  - **Note:** E6FD6AB497A9 (the replay session's mission test, installed 19:15) holds this session's half-done
    18:50 - 19:03 road traffic files: 1,500 cars and the in-sight waits that jam. Played before this one installs, it
    can jam.
  - **Files:** Camera/RoadTraffic.cpp (the counts), RoadTrafficCore.h (show only out of sight, the oncoming margin),
    RoadTraffic.h.
  - **Write-up:** Research_Road_Traffic_2026-11-03.md section 10.

- **The snow piled against walls, boxes and curbs without stripes** (the user, 2026-11-03: "Snow depth map still looks
  weird find a way and fix it", then, asked which, "Stripes or bands on walls"). WetLook.hlsl's SnowPileSide and PsMain's
  pile, in the shared folder since 19:11. In the road traffic session's build 820661E1B5DF (19:21, in dist; staged to
  install in the game folder when the game closes; checked: all 9 WetLook shaders in it are the folder's). The snow
  session's own 19:12 build a7b34724245b isn't used: it held the road traffic session's unfinished files. **Not tested in
  the game.**
  - **What made the stripes** (the user's 11-01 screenshot of a parapet's foot: a band of dark columns crossed by fine
    lines): the pile was drawn with the look of the ground's snow at its foot, one point for every pixel up a column, so
    the grain, the glints and the ripples there were drawn up the wall as streaks. Its foot was found only to within
    1.6 pixels, so the next pixel up often got another row, and the look and the light jumped from row to row. And within
    about 3.6 m of a wall the search ended at 64 pixels, so the pile's top was cut in a hard straight line.
  - **Now:** the foot's row is found exactly (in whole rows), the same for every pixel over it. The pile looks like
    where each pixel sees it: a drift leaning on the wall at about 45 degrees, so its grain and glints lie on it as on
    the ground. It's lit by the ground just in front, from however many of the 3 points the game's G-buffer has there
    (all 3 or none before). The search goes twice as far (128 pixels at 4K), and past that the pile fades out instead
    of being cut.
  - Where it lies and how high is unchanged. The rain's shaders are byte-identical; the snow's pass is about 2 % longer
    (fxc: 16,285 instruction slots against 15,900, the same registers).

- **REPLAY > Mission test: the replay tested at every mission's start points by itself** (the user, 2026-11-03: "Can
  you run simulations or something to ensure that every mission/side mission will work with replays because i'm not
  testing 12 missions and 12 side missions. find a way no mistakes", then "Yes, build it"). Build E6FD6AB497A9, built
  19:07 on 2026-11-03 from the shared folder (it also holds the road traffic session's RoadTraffic.cpp/.h and
  RoadTrafficCore.h changes of 18:50 - 19:03); INSTALLED 19:15:14 when the game closed (game folder + dist, hash
  checked, still there 30 s later). **Not tested in game.** Research_Mission_Replay_Audit_2026-11-03.md sections 6 and 8.
  - **What it does:** for each start point chosen, the city is loaded again at it; once the loading screen is gone
    and 10 s have passed, the replay records (20 s by default) while Faith stands where the start point put her, then
    plays it back. Three pictures (a quarter, half and three quarters in) of the recording and of the playback at the
    same replay times go side by side with their difference into one picture. One line per start point goes to
    `Cinematic Tools\MissionTest\Run <date time>\results.txt` and CT.log: how long the load took, the level's start
    point after it, where Faith is, the missions running, what was recorded, how far it played, the warnings and
    errors meanwhile, and how far the pictures differ (mean and worst of 16 x 9 cells). It ends with OK, LOOK (a cell
    over 30 of 255, or no pictures) or FAILED (not loaded, recording or playback stopped early, or an error logged).
  - **Which start points (Camera\MissionTestPoints.h, generated from the game's data):**
    - all 504 of the city's flow layers: 159 plain ones (PamStartPointEntityData: a mission's beginning, briefings, a
      side mission's place) and 345 mission sections (PamMissionStartPointEntityData);
    - "Each mission's start" is 88 of them: a story mission's `<name>:_Mission_Start`, else its own start point;
    - every name checked against its NameHash, none applies debug flags.
  - **The load is the game's own [decompile]:** its LoadLevel event (LevelControlEntity, 0x143383090 -> 0x14338A790)
    copies the level's LevelSetup (level + 0x118) and sets StartPoint (+ 0x18), with IsSaveGame, HasPersistentSave and
    ForceReloadResources 0. It hands it to 0x143341D40, which posts a ServerLoadLevelMessage, then sends a
    ServerRoundResetMessage. The test does exactly that on the server's update thread (FaithOutfit's Server::update
    hook), where the event runs. Eight game functions are byte-checked first, all against the decrypted image. The
    game's data does the same with prefabs/pf_loadlevellogic. The game also reads `-startpoint <name>` from its
    command line into GameSettings.StartPoint (0x1432F5420); the test doesn't use that.
  - **Your save:** before the first load, `Documents\Mirrors Edge Catalyst\settings` (PROF_SAVE and its backups)
    is copied into the run's folder ("Save before the test"). It goes back at the next game start, before the game
    reads it: the tools start with the game (the usp10 loader) and do this first. The menu's tick can keep the save
    as the test leaves it instead. A second run in the same session keeps the first run's copy. The game saves when
    it quits (AutoSaveOnQuit), so putting it back while the game runs wouldn't hold.
  - **While it runs:** the replay doesn't save its recordings (it would be 88 - 504 saves) and doesn't loop. Both go
    back to how they were at the end. Each recording is forgotten before the next load.
  - **It stops by itself** after 3 start points in a row the game didn't load, a load still on after 5 minutes, or a
    recording that didn't stop. The menu's Stop also ends it.
  - **Also:** util::log counts its warnings and errors (Warnings(), Errors()); replay_assetlog::RunningMissions();
    FaithReplay gets the calls the test drives it with (RequestRecordOrStop, RequestPlay, RequestStop, ForgetIfIdle,
    the times, Summary).
  - **Not known until it runs:**
    - whether the retail game honours a start point on a load (the line says "NOT the one asked" when the level's
      start point after the load isn't the one asked);
    - whether the mission then starts;
    - whether Faith survives standing still where enemies come;
    - how the pictures compare (live effects differ between the two).
- **Road traffic: twice the road cars and walking people by day** (the user, 2026-11-03, asked whether to test
  EBAC009D921B first or raise the day's count above the game's own maximum: "Raise it now"). Hidden, like the rest of
  road traffic: no menu entry. Build 90EFC3980468, built 18:20 on 2026-11-03 from the shared folder (only this
  session's files changed since EBAC009D921B); INSTALLED 18:20 (game closed; game folder + dist, hash checked, still
  there 40 s later). **Not tested in game.**
  - **What's raised:** the road cars' crowd (carcrowd_01, 1,210 cars) and the three big walking crowds: the
    pedestrians' (721 of its 754; the rooftop bar's 33 not), the mid caste's (887) and the high caste's (688). Each gets
    twice the level's members, 3,506 more in all [data]. Every other crowd keeps the level's count: standing people,
    workers, enforcers, the low caste, joggers, drones, boats.
  - **How [decompile]:**
    - A crowd's room is its SwarmEntityData's MaxAgentCount. When the crowd is made, its parts size every per-member
      column from it: the records, the render copies, the terrain snap, the variation, and the renderer's buffers
      (0x143B8E6B0, 0x143B8EEA0, 0x143B9FE40, 0x143BACF10 -> 0x143B9F010). A placement registering with a full crowd
      gets no members (0x143B97430).
    - A placement's members are its data's MaxCrowdMembers (read through its vtable's + 0xD8, 0x143B94B10).
    - Both are raised in the level data as the game makes each entity, in its factory, before anything reads them:
      0x143B880B0 for a crowd, 0x143B8DE90 for a placement; the data is at the creation info's + 0xD0, which becomes
      the entity's + 0x30. So the game sizes everything for the raised counts itself.
    - The crowds are matched by their room: 1300, 901, 800 and 721, which no other crowd has. The placements are
      matched by their settings asset's name (Asset's Name at + 0x10). Both checks also need the data's vtable.
  - **Nights stay as they were:** the raised crowds' night share is halved (15% of the cars, 12.5% of the walkers),
    so the same number shows at night as before.
  - **The renderer is data-driven [decompile]:**
    - its per-member buffers are sized from the room;
    - members are numbered in 16 bits (up to 65,535);
    - its fixed blocks (12 views x 8 x 38 KB) hold counters by mesh and LOD (the cull job 0x143BA6D80), not one per
      member.
    - It and the crowd update walk every slot up to the room, so only the crowds that get more members get more room.
  - **Offline (x2, 120 s, 30 steps a second):**
    - Cars: 2,289 shown a frame (1,169 before). None inside another, 99.2% on tarmac, the game's node lists clean.
      The rules take 3.2 ms a frame across all car placements on one thread (1.3 ms before); the longest single
      placement step was 5.7 ms, near the start.
    - Walkers: 5,009 shown (2,719 before). Pairs closer than 0.3 m 0.001 a frame; standing 0.62% of walker-frames
      (0.34% before), the longest stand 8 s. The rules take 2.2 ms (0.9 ms before), and the copy of the game's own
      walker step 0.86 ms (0.43 ms before).
    - At night, x2 at half the share: 380 cars (368 before) and 708 walkers (724 before).
  - **The cost:** about twice the game's own crowd work by day, and about twice the drawing of cars and people. The
    drawing isn't measured.
  - **The walker rules' limits** (WalkersCore.h): 640 members a placement (the largest has 600 now) and 1,024 other
    placements' walkers looked at a step (was 512; better offline even at x1). The car rules' limits already fit.
  - **The log:**
    - each raised crowd: "Road traffic: <crowd>: the crowd's room raised from N to 2N members as the game makes it";
    - the second minute line: placements and members raised, both shares, and the walkers' longest rules step.
  - **Files:** Camera/RoadTraffic.cpp (two factory hooks, the raise, the night share), RoadTraffic.h, WalkersCore.h.
  - **Write-up:** Research_Road_Traffic_2026-11-03.md section 9.

- **Road traffic: busy days and quiet nights for the cars and the people, and walking people that keep their
  distance** (the user, 2026-11-03: "during the day there should be a lot more cars/people and during the night not so
  many no mistakes", then "also fix the pedestrians pathing as well while at it"). A hidden feature like the cars'
  lanes: no menu entry. Build EBAC009D921B, built 16:47 on 2026-11-03 from the shared folder (holds the Faith size
  session's 5CEC9EAB4CF9: Faith size stays off, its strings are absent); INSTALLED 16:47 (game closed; game folder +
  dist, hash checked, still there 30 s later). **Not tested in game.**
  - **Day and night:** the game's clock (seconds, 0 at midnight, the one the tools' time of day menu shows) sets a share:
    all the cars and people from 08:00 to 18:00; 30% of the cars and 25% of the people from 23:00 to 05:00; eased in
    between (up 05:00 - 08:00, down 18:00 - 23:00). The clock runs ~41 game seconds a real second, so a day is ~35
    minutes.
  - **"A lot more in the day" is the game's own full count:** every placement's MaxCrowdMembers, about 96% of them
    shown at once now. More than that would need the swarms' MaxAgentCount raised before the level makes them: a
    placement that doesn't fit gets no members at all (0x143B97430), and the swarm's buffers are sized from that count.
    Not done.
  - **Where they go and come:** only out of sight. That means further than 400 m (cars) or 150 m (people) from the
    view, or more than ~69 degrees off to its side; never in view.
    - The view is the one the game's walker step reads (0x143278E10's entry), read without the game's lock.
    - Which way along its axis it looks isn't relied on; whether it follows the tools' free camera is [inferred].
    - With no view or clock read, everything shows as before (logged once).
  - **The game's respawn is held to the share [decompile]:** it brings a despawned member back only while more than
    its despawner's + 0x34 are despawned (0x143B9CC31). Each update sets that from TargetSpawnedPercentage
    (0x143B9B8D1). The tools raise it to what the share leaves out and never lower it.
  - **Walking people (Pedestrians behaviour, step 0x143B8CF00) [decompile]:** the game's walkers never slow down. Its
    avoidance only turns them away from people ahead on their own two nodes, so a walker it can't turn away walks
    through the other. Two placements on one path never see each other (31 and 109, 100 walkers each on 61 shared
    nodes). A respawn comes in on top of whoever is there. Now:
    - a shown walker never ends a step closer than a body (0.55 - 0.6 m) to another, its own placement's or another's,
      and keeps ~1 m behind one going its way;
    - it steps aside for one coming the other way or across (at most 0.9 m/s, within its path's width: the larger
      radius of its two nodes);
    - a respawn waits unseen until its spot is clear;
    - one stuck for 4 s walks the game's way through the others for 1.5 s in sight, or waits unseen out of sight;
    - boats use the same behaviour and are left alone (MemberRadius 12).
  - **Standing crowds** (the Idle behaviour, no step of their own): only the share. That comes from a hook on the
    placement's per-frame update (0x143B9B7E0), after the game's update.
  - **Offline:** 114 walking placements, 2,720 walkers, the game's step copied, 30 steps a second, 120 s.
    - Pairs closer than 0.5 m: 15 - 17 a frame -> 0.16 - 0.2. Closer than 0.3 m: ~5 -> 0.001.
    - Walker-frames standing: 0.2% -> 0.34%, the longest 5.3 s. Sudden turns: 0.52% -> 0.37%.
    - The rules take 0.8 ms a frame, all placements on one thread (0.35 ms at the night share).
    - Night share 0.25: 724 of 2,720 shown.
    - Cars by day: the same as 3218B47629F3's test (1,163 shown, none inside another). At a night share of 0.3: 368
      shown, none inside another, never a placement with none shown, the game's node lists clean.
  - **Tried and dropped:** only cutting a walker's move short, without the sidestep. It gridlocked: walkers met head
    on and stopped; 48% of walker-frames were standing, some for the whole run.
  - **The log:** a second "Road traffic:" line once a minute. It gives the walkers, the standing crowds, the time of
    day and both shares, and how many went and came for them.
  - **Files:** Camera/WalkersCore.h (new), RoadTrafficCore.h (DayShare, OutOfSight, the cars' share), RoadTraffic.cpp
    (the walker and update hooks, the clock and the view), RoadTraffic.h, the project files.
  - **Write-up:** Research_Road_Traffic_2026-11-03.md section 8.

- **GAME > Faith > Size switched off** (the user, 2026-11-03: "ok the fun is over comment out the slider we can keep
  the files"). Build 5CEC9EAB4CF9, built 16:38 on 2026-11-03 from the shared folder; staged 16:38 with the game
  running, INSTALLED 16:43 at game close (game folder + dist, hashes checked, still there 30 s later).
  - The Size slider, its 1.00 button, "Hair follows size" and "Collision follows size" are commented out of the GAME
    tab (CameraManager.cpp), with faith_size::Update (OnCameraUpdate) and faith_size::Init (Main.cpp).
  - So she's always her normal size, and none of its hooks are made: collision casts, hair bone matrices.
  - Camera/FaithSize.cpp and .h stay in the project and still compile. Uncommenting those three places brings it
    back as it was in 49AB7C71B765.
  - The string diff against the installed 49AB7C71B765 shows only Faith's size's own text gone (the linker drops the
    unused code).

- **Fixed: GAME > Faith > Size > "Hair follows size", gravity and wind at her size, and no slow catch-up** (the user,
  2026-11-03, after testing 6E423806B377: "when she is big they go all over the place an small doesn't work great
  either", then "also it takes a while for the hair to catch up when small or big"). Build 49AB7C71B765, built 16:28
  on 2026-11-03 from the shared folder (holds another session's FDCFC96954D9 of 16:13: the string diff shows only
  this change); INSTALLED 16:29 at game close (game folder + dist, hashes checked). **Not tested in game.**
  - **What the log showed [CT.log 16:18 - 16:20]:**
    - The bone matrices now went in: 53,400 every 10 s, 178 bones × 300 cloth updates.
    - Her hair's anchors sat on her head: at 0.40 they were at y 66.222 - 66.307 with her head end at 66.290; at 10
      they were at 78.35 - 80.41 with the head end at 79.97.
    - But a second after 0.01 -> 10, the hair's box spanned 26.6 - 90.0 m. Ten seconds later it was 3 m.
  - **Why [decompiled, and read live]:**
    - The hair's gravity and wind are in metres and weren't sized. Each step the solver recomputes gravity × dt²
      from the vector at its simulation + 0x68 (0x143B76390: −8.8375 × (1/30)² = −0.0098194), and its wind from
      + 0x74, which the game rewrites as the wind changes.
    - So a big Faith's hair moved as in a tenth of the gravity, and a small one's was pulled a hundred times too
      hard.
    - On a size change, its particles stayed where they were and were dragged into place over seconds.
  - **Now, in the hair's own update, right before its step** (the sim job makes the palette and then steps; it
    runs from the bone matrix hooks, bone 0):
    - Its gravity and wind are the game's times the size. The game's come back at 1.00, unticked, or during a
      replay.
    - When the size has changed, its 105 particles, and their places a step before, are moved about her feet by the
      new size over the old, as her body is. So the hair is its new size at once. The arrays' room is checked first
      (118 each).
  - **Log:** a new line every 10 s, "Faith's size (hair): its gravity's y ... particles moved to a new size N times".

- **Fixed: GAME > Faith > Size > "Hair follows size", her hair now built on her sized body** (the user, 2026-11-03,
  after testing A3148087A053: "yes the hair sized but not in the way we want find out what went wrong and fix it").
  Build 6E423806B377, built 16:06 on 2026-11-03 from the shared folder (string diff against the installed
  A3148087A053 shows only this change). Staged 16:07 with the game running; INSTALLED 16:11 at game close (game
  folder + dist, hashes checked, still there 30 s later). **Not tested in game.**
  - **What went wrong [CT.log 15:51 - 15:54, then read live, read only]:**
    - The lengths part worked: "8 of 8 lists the tools' copies".
    - The anchor part never acted: "0 scaled, 0 left, 0 with another world pose" in every line.
    - Her cloth's pose has its + 0x12D flag set. So the game (0x142FA5020) doesn't ask the hooked slot 1 for her bone
      matrices. It chains them itself from her local pose (0x142FA45D0): the cloth's root, then each bone's local
      record (0x142FD46A0) times its parent's, each handed to 0x142FA4950 for its palette entry.
    - Replayed from her live records at size 10, this gave the game's palette to within 0.0001.
    - Her local pose has a scale of 1 / size on her spine (bone 7) and ten more bones (0.1 at size 10). That undoes
      the size from the spine up: in the hair's skeleton her hips were sized, but her spine, neck, head and the hair's
      anchors were normal size on top. So the hair sat off a small head and inside a big one, at its new length.
  - **Now:** a second hook, on 0x142FA4950, the chain's palette entry. For her body's pose, each bone from the hips
    on gets her drawn body's world matrix instead of the chain's:
    - the game's own matrix from her world pose (slot 1);
    - its rows times the size;
    - its place sized about bone 1 when it's still the game's (the cloth may run before the tools size her pose that
      frame).
    - Live at size 10, a bone's inverse bind times that matrix was her drawn body's skinning matrix to within 0.00004.
    - Slot 1's hook (the other way, flag not set) sizes the same way now: before, it left a place that wasn't the
      tools'.
    - Checked at start: 3 more byte checks (the choice between the two ways, the chain's palette loop and entry), 20
      in all, verified against the game's image.
  - **Log:** "Faith's size (hair): ... bone matrices for her cloths sized: N (M of them into the chain's palette; A
    with places the tools wrote, B with the game's places sized here), L left ...".

- **GAME > Faith > Size: "Hair follows size", her hair sized with her** (the user, 2026-11-03: "It works now find out
  why the hair doesn't snap with the size", then "Yes, research it", then "Yes, build it"). Build A3148087A053, built
  15:32 on 2026-11-03 from the shared folder (holds the shard session's 04C836A2A75E, installed 14:53: the string diff
  shows only this change); INSTALLED 15:33 (game closed; game folder + dist, hashes checked). **Not tested in game.**
  - **Why it stayed big [decompiled, and read live at size 0.30; Research_Faith_Size_2026-11-03.md section 6.5]:**
    her hair is EA SimpleCloth.
    - Its anchors are placed from the bone matrices it asks its pose provider for (0x14304E7A0 -> 0x142FD2B00): her
      sized bone places, but rows of length 1. So they sat at her sized head plus their normal-size offset (0.80 m
      over her feet, her head end at 0.50).
    - Its lengths are in metres, and its solver reads them every pass through pointers to its definition's arrays.
    - The earlier guess (colliders pushing it out) was wrong: its collider lists are empty.
  - **Now, ticked (on at start, not saved), while she's sized:**
    - **Anchors.** A hook on the cloth provider's slot 1 (only ClothEntity uses that vtable) scales the three rows of
      a bone's matrix by the size. That's only for her body's pose, and only for a bone whose world place is what the
      tools wrote this frame. Every other cloth gets the game's matrix.
    - **Lengths.** Each cloth of hers posed by her body gets its solver's eight length pointers pointed at the tools'
      copies, every length times the size: structure, shear, fold and long-range rest lengths; anchor and anchor
      plane distances; radii; fixed distances.
      - The copies have the game's counts, 16-byte aligned, two per definition (the one not pointed at is filled).
      - At 1.00, unticked, or while a replay records or plays, the pointers go back to the definition's.
      - The definition is never written.
      - A pointer that's neither the game's nor the tools' is left alone.
    - **Checked at start:** the code it relies on (17 byte checks, the provider's slot and jump). Checked every frame:
      each solver's counts and sizes against its definition's.
    - **Left alone:** gravity, wind, and the cloth's root (it already had the size).
  - **Log:**
    - every 10 s while sized, "Faith's size (hair): ... bone matrices ... scaled / left ...; N cloth(s) of hers
      handled, K of their lists the tools' copies";
    - the diagnostic line now gives her hair's first particles (its anchor count) next to her sized head;
    - "her cloth 0x... 's lengths are the tools' at X" and "... its definition's again".
  - **Not known until tested:**
    - whether the drawn hair (wrapped onto the cloth on the GPU) has offsets in metres, so a small Faith's hair would
      look thicker;
    - one parameter of unknown meaning (0.007547) that isn't sized;
    - moving the slider fast, the hair swings into place over some frames.

- **Fixed (build 04C836A2A75E): the big far-away fans and the smoke left behind with the real Shard** (the user,
  2026-11-03, streaming the whole city: "ticking the actual shard leaves behind the big shard helix it should get
  removed even when streaming the whole city", then "find out why the smoke stays as well"). Built 14:53 from the
  shared folder (no newer source); INSTALLED when the game closed (game folder + dist, over 3218B47629F3, hash
  checked and again 30 s later). **Not tested in game.**
  - **Why the big fans stayed [log + code]:** the tools move the far-away pair onto the real tower through the game's
    rotation updater, and at 14:30 they weren't in it ("0 of its 2 far-away fans found to move"), so they stayed
    in the sky at 1.7 and 2.9 times their size. Every time they were missing (11:12, 14:30) the far-away sub-level
    had streamed in during play through the switched trigger (Unbroken Shard on, or swapped back). In the 14:40 run
    it came with the level and both were found every time. Offline: a rotation entity is in its updater from its
    set-up (AutoStart) until a Pause or Stop event or its end; nothing in the data or the tools sends those to these
    fans, and the updaters are only destroyed with the level. So why they were missing is still **not known**.
  - **What changed:** both realms' updaters are searched (the game has one per realm; only realm 0's was read; live
    at 14:5x realm 1 had none). A moved fan counts only once its rotation entity's clock (+ 0x78) is seen moving,
    i.e. the game turns it where the tools put it (read live: +1.0 a second on both). If the pair isn't moved and
    turning within 5 s while the far-away sub-level is loaded (not counted while paused or loading), the tools
    stream the whole far-away sub-level out, the game's way, as when Faith leaves the city: no big fans and no
    smoke, but then no fans on the real tower either, until the real tower goes. Unticking the real Shard now waits
    for the far-away model to come back (or 10 s) before the real tower is kept out.
  - **Why the smoke stayed [game data]:** it's an effect of its own in the ruined far-away sub-level
    (fx_shard_backdrop_smoke, the only thing in its prefab), not a model group member, and the tools hide only the
    far-away model's model group members. The whole far-away model has no smoke.
  - **What changed:** while the far-away model is hidden, the smoke's instance (found by its emitter's name,
    EM_Shard_Backdrop_Smoke_Smoke, at 893 2942 -3180) is moved 100 km down the game's own way (as the weather moves
    the rain); the effect manager's culling (15 km) then stops it. **The puffs already in the sky stay until their
    time is up (60 s, randomised by 20%)**: no game code was found that clears them. Back at its place when the
    far-away model is shown again.
  - Log lines: "The Shard: the ruined far-away model's smoke ... moved 100 km down", "... smoke back at its place",
    "0 of its 2 far-away fans found to move (... the rotation updaters have N and M rotation entities ...)", "its
    far-away fans not found to move, or not turning where the tools put them, in 5 s".
- **Road traffic: cars turn on arcs instead of snapping round corners** (the user, 2026-11-03, testing CA34FF0A07C1:
  "Everything looks and works perfectly however i think we can smooth out some animation you can see them snapping
  when taking some corners"). Build 3218B47629F3, built 14:36 on 2026-11-03 from the shared folder (only
  RoadTraffic.cpp recompiled; holds the Faith size session's B593B5787F95, checked: "Collision follows size");
  INSTALLED 14:37 (game closed; game folder + dist, hash checked and again 30 s later). **Not tested in game.**
  - **Why they snapped [code + offline test]:** a shown car turned straight toward a point 3 - 8 m ahead on its lane,
    at most 0.6 rad (34 degrees) a step. At the game's step rate that's barely a limit, so where two lanes meet at a
    sharp joint the car pivoted. Offline at 13 steps a second, sampled about every 0.1 s: turns up to 1,969 deg/s, 0.2%
    of moving samples tighter than a 3.5 m radius (the game alone, at 140 steps a second: 356 deg/s at most).
  - **Now:** a shown car steers on an arc toward that point (pure pursuit's curvature, 2 sin a / l), never tighter
    than a 4 m radius (a small car's 10 m turning circle), and looks 4 - 10 m ahead (3 + 0.15 x speed). Between
    steps the rules keep its heading in full precision: the record's half float is 0.06 - 0.25 degrees coarse, which
    at the game's step rate is more than a step's turn. Unseen cars (a turn round at a road's end) still turn as
    before. A slip fixed with it: when the side check shortened a move, each try started from the last try's heading,
    not the car's.
  - **Tried and dropped:** a steering-speed limit as well (from straight to the tightest turn in 0.5 s, or 0.25 s). It
    lagged at corners: cars swung into other lanes and jammed (0.5 s: 89% of footprints on tarmac, 19% of car-frames
    standing over 30 s). B593B5787F95 (the Faith size session's 14:30 build) had compiled that 0.5 s version mid-edit;
    nobody played on it (the last game, 14:15 - 14:34, ran CA34FF0A07C1), and 3218B47629F3 replaced it.
  - **Offline test (sampled every 0.1 s; at 13 steps a second, the game's measured rate, 300 s; old -> new):** fastest
    turn 1,969 -> 385 deg/s; tighter than 3.5 m 9,583 -> 0; sudden yaw-rate changes (over 600 deg/s^2) 2.0% -> 0.6%
    (the game alone 2.7%); footprints on tarmac 98.75% -> 99.26%; cars inside another 0.00%; no car standing over
    12 s; the game's node lists clean. Also at 30 steps a second (3 seeds x 300 s) and 140 (120 s): the same picture,
    the rules 1.2 - 1.3 ms a frame for all placements on one thread.
  - **In the game (CT.log of the 14:15 - 14:34 test, CA34FF0A07C1):** about 16,000 car steps a minute (about 13 a
    second per placement), the rules 1.2 - 1.9 s of job-thread time a minute, on more than one thread.

- **GAME > Faith > Size: "Collision follows size" (option A), her capsules handed to the game at her size** (the
  user, 2026-11-03: "Lets try option A", then "Done read the logs also her hair is still on the air when too small").
  Build B593B5787F95, built 14:30 on 2026-11-03 from the shared folder (holds the road traffic session's RoadTrafficCore.h
  as saved at 14:23, newer than their 14:02 build); installed 14:34 at game close (game folder + dist, hashes checked), then replaced 14:37 by the road traffic
  session's 3218B47629F3 (built 14:36 from the shared folder; holds this work, its strings checked; its finished
  steering replaces the 14:23 RoadTrafficCore.h trial B593B5787F95 had compiled, which nobody played). **Not tested
  in game.**
  - **What the diagnostic's logs showed [log, 13:47 and 14:18 - 14:26]:** her movement sweeps two ready-made capsules
    through FrostbiteCollisionWorld slot 7, asked by 0x142F3C117: radius 0.30 / half height 0.60 centred 0.90 m over her
    feet (1.8 m tall), and 0.30 / 0.18 centred 0.48 m up. Her ledge probes (0x142F4B1D8: capsules at 1.44 - 2.16 m,
    2.5 m ahead) and box probes (0x142F4B21A) are other shapes. Every cast had scale 1.000 at sizes 0.09 - 2.00 (the
    size didn't reach her collision). Her character's id in the casts was 7, 10, then 22 after a respawn.
  - **The capsule [live, read only]:** hknpCapsuleShape (vtable 0x141D4F448, 0x1B0 bytes): + 0x14 convex radius (0.99 x
    the radius), + 0x50 / + 0x60 its ends, + 0x70 eight hull vertices, + 0xF0 six planes; Frostbite's wrapper (vtable
    0x141BB19C0) holds it at + 0x08.
  - **Now, ticked (off at start, not saved):** while she's sized, a slot 7 cast with her id and one of her capsules
    gets the tools' copy of it with every length times the size (its own wrapper; its size field 0, so the game
    never counts or frees it), and its place raised along the capsule's axis so its bottom stays where the game put
    it. The sweep is the game's: her moves keep their distances. Her id is learnt once a second from the movement
    casts that start over her feet (20 or more); her capsules are taken from her movement casts after checking their
    vtables and numbers. The game's own shapes are never written.
  - **Log:** "Faith's size: collision follows size ticked / unticked", and every 10 s while sized: "Faith's size
    (collision): at X, her id N, K capsules of hers known: r .. h ..; M casts of hers handed sized".
  - **Not known:** whether other kinds of queries (overlaps, the sphere and ray casts) also keep her out of walls at
    her normal size; how her moves behave with a bigger or smaller body (she may get stuck when big).
  - **Her hair [log, 14:21 - 14:24 at 0.50]:** its cloth's box spanned 0.75 - 1.06 m over her feet while her sized head
    end was at 0.82 m: anchored at the sized head, but the cloth's own lengths stay in metres, so it sticks up. Not
    changed in this build; the cloth's rest lengths and radii aren't found yet.

- **Road traffic: the crash of 13:47:41 fixed (unseen cars back at y -10000)** (reported by the Faith size session on
  2026-11-03, after the user's "game hanged": an access violation at MirrorsEdgeCatalyst.exe+0x3B9C00C, game code
  only on the stack). Build CA34FF0A07C1, built 14:02 on 2026-11-03 from the shared folder (only RoadTraffic.cpp was
  recompiled; it holds all of 7BFAC1779936). Staged 14:03 (the game was still up, held after the crash); INSTALLED 14:06
  at game close in the game folder (dist already had it), hash checked and again 30 s later.
  **Not tested in game.**
  - **Cause: my 7BFAC1779936 [decompile + live, read only, game held after the crash]:** it kept unseen cars at
    y -10042, the game's despawn height. The game's per-frame update (0x143B9B7E0) keeps each car placement's node
    lists (behaviour + 0x10: a member-to-node map, one member array grouped by node) only for members above -10042. A
    respawned car the rules kept unseen was never listed, so it stayed in the 'none' group. When the game then despawned
    it, its list move (0x143B9C070) had 'none' at both ends and wrote the car over another car's entry. That other car's
    next move couldn't find itself in its group, and the move then shifted memory 4 bytes up without end. Seen live:
    placement 63D21950's member array has member 23 twice and member 36 missing; 63D25370's array has a 0 inserted at
    its start; everything from 0x55353744 to the end of mapped memory (about 0x55400000) is 4 bytes up. That stretch
    held every car, idle and pedestrian placement's behaviour object, and the next read of one (0x143B9B953) took a
    garbage pointer. The free camera and Faith's size were turned on in the same 2 s; they didn't cause it.
  - **Now:** unseen cars are at y -10000, under the world but above -10042, so the game lists them each frame like
    shown cars, and a despawn always finds a car in its group. The placement's box then reaches down to its unseen
    cars, but it still holds every shown one (the 11413E1A2147 stall needed every car hidden at setup, which nothing
    does now). The rest of 7BFAC1779936 stays: no spawn hook, respawns started unseen by the rules, unseen cars put back
    when the rules can't step.
  - **Offline test:** the sim now carries the game's node lists (0x143B9C070, 0x143B9BFA0 and the despawn's move,
    ported as decompiled). At -10042: 201 moves with the same node at both ends and 11,691 with a member not in its
    group in 300 s, which is this crash's cause. At -10000: none after the first 0.05 s (3 seeds x 300 s), the same as
    the game alone. The 4 - 5 same-node moves in the first frames, with or without the rules, come from the test's
    guess at the lists' first state. Everything else is as before: cars inside another 0.00%, tarmac 98.6%, no
    placement all unseen, no car standing over 12 s.

- **Road traffic: the cars are back (the 11413E1A2147 build showed none)** (the user, 2026-11-03, testing it: "There
  are no cars in the street now"). The fix is in 7BFAC1779936: the Faith size session built it at 13:40 from the shared
  folder with these sources as saved (RoadTraffic.cpp 13:39:46, RoadTrafficCore.h 13:37:21) and INSTALLED it at 13:41
  (game closed). Checked: the game folder's DLL is the same file as dist, it has this build's log texts and no spawn
  hook, and RoadTraffic.obj (13:40:03) holds the unseen height -10042 and none at -10000. **Not tested in game.**
  - **Why no car showed [live, read only, 13:2x; decompile]:** every car was at y -10000 and none was despawned. The
    car swarm's clock (CrowdBoidsComponent + 0xA8) stood at 0.000 while the other swarms' went on, 1 s a second. Every
    car placement's box (+ 0xC0 / + 0xD0) was at y -10002 .. -9996. The cause: the spawn hook hid each car as the game
    first spread them (0x143B99860 calls the same spawn, 0x143B99C40, as the respawns). The game builds each placement's
    box from those cars at setup (0x143B97F9A). After that the swarm's update (0x143B92E00, the only code that moves
    its clock on and starts the placements' jobs) was never called. The game seems to update a swarm only while it's
    needed [inferred: the far people swarms stand at 0.000 too]. So the car step never ran, and neither did the rules
    that show the cars. This was my mistake: the offline test has no such gate, and it ran respawns after the step.
    The game runs them before it (0x143B9CBE0 at 0x143B9B841).
  - **Now:** there's no spawn hook. The game respawns before its step and draws after it, so the rules start a
    respawned car unseen themselves (one that was despawned at its last step); it's never drawn where it came in. At a
    placement's first step the cars stay as the game spread them; only one inside another waits unseen. Unseen cars
    sit at y -10042, the game's own despawn height (0x143B97257); -10000 was my misreading. The game's per-frame update
    leaves cars at that height out of the placement's box and node lists (0x143B9B9F1), as it does its despawned cars.
    When the rules can't step a placement (a refused read, an exception), the cars they keep unseen go back to their
    lane's height before the game's step. Each kind of refusal is logged once with its values, and the once-a-minute
    "Road traffic:" line counts them.
  - **Offline test, now in the game's order (5 x 300 s):** cars inside another 0.00%; footprints on tarmac 98.6%; no
    placement ever had all its cars unseen; on the first frame 1,154 - 1,160 of about 1,205 cars shown; no car stood
    longer than 12 s; 1.36 ms a frame for all placements on one thread (run alone; 1.51 ms with five tests at once).
  - Readers of car positions: unseen cars were at y -10042 in this build; back at -10000 since CA34FF0A07C1 (this
    height broke the game's node lists and crashed the game). y < -9000 covers both.

- **GAME > Faith > Size: collision diagnostic, the first step of "collision follows size"** (the user, 2026-11-03,
  after the collision research: "Lets try option A"; option A's first step is finding her capsule shapes). Build
  7BFAC1779936, built 13:40 on 2026-11-03 from the shared folder (holds the road traffic session's sources as they were at
  13:39); INSTALLED 13:41 (the game had closed; game folder + dist, hash checked and again 30 s later). **Not tested in game. Changes nothing in the game: it only logs.**
  - **Hooks** (code and vtable checked first; Main.cpp calls faith_size::Init right after faith_outfit::Init):
    FrostbiteCollisionWorld's slot 6 (0x142FCB200, a shape cast with an Ant collision volume, 10 arguments), slot 7
    (0x142FCB5B0, a shape cast with a Havok shape, 9) and slot 8 (0x142FCC1B0, a collision object registered, 6).
    Each passes every argument to the game's own as it came (all 64 bits).
  - **While she's sized**, the casts that start within 3 m (x the size above 1) of her feet are tallied by slot,
    shape and the game's code that asked: the shape's vtable and type, its + 0x60 .. + 0x6C (a capsule's Radius,
    HalfHeight), its own scale and place, the cast's scale, its place from her feet, its sweep, the ids and filter.
    Every 10 s with the timing line: "Faith's size (collision diagnostic): at X, N shape casts in all; K kinds
    started within R m of her feet", then the 24 most frequent.
  - **Every registration** (the first 60 of a session, from the level load on): "Faith's size (collision diagnostic):
    registration by 0x...: asset ... type ..., ids ..., N volumes: [...]" with each volume's numbers.
  - **What it's for:** which shapes are her movement capsule, which path they take (made each query, or registered
    once), and whether her capsule's casts already carry bone 1's scale. Research_Faith_Size_2026-11-03.md section 6.

- **GAME > Faith > Size, third build: 0.01 - 10, her first-person view moved with her, a hair line in the log** (the
  user, 2026-11-03, testing 31F4EF5B37C7: "hair doesn't scale same with first person camera everything else works",
  then "lets make the slider go from 0.01 to 10"). Build CD67DA38095D, built 13:17 on 2026-11-03 from the shared
  folder (no source newer but this change); STAGED 13:18 (game running; a job installs it at game close, only over
  older files, game folder + dist, hash checked and again 30 s later). **Not tested in game.**
  - **Timing [log, 31F4EF5B37C7]:** "the game had written her body's world pose anew before 1336 of them [1336 camera
    hooks]": the game rewrites her pose before every camera hook, and her body showed sized, so the frame is drawn
    after the hook.
  - **Range:** 0.01 - 10 (ImGui 1.61's power 3.32 puts 1.00 in the middle of the slider; ctrl+click types a size).
  - **First person:** the game places its camera on her camera joint (bone 179) from her unsized pose before the hook.
    With the free camera off, a game camera within 0.5 m of her camera joint (as the game wrote it) is moved to bone
    1's place + size x (its place - bone 1's place), in both render view transforms (+ 0x00, + 0x40, as the free
    camera writes them); its rotation stays; a place the tools wrote isn't moved again. Log, once a sizing: "the game's
    camera is X m from her camera joint (her first-person view: moved with her / not her first-person view: left
    alone)".
  - **Her hair [live, read only, 13:1x at size 1.00]:** its EA cloth instance holds her root place (bone 1's world
    place, + 0x100, its fourth float 1.0) and a box at her head bones 50 - 53 as the game wrote them (+ 0x160 /
    + 0x170). What it skins with and when isn't known; every 10 s while sized the log now gives the cloth's root and
    box next to her head end's place as the game wrote it and as sized ("Faith's size (diagnostic): at X her hair
    cloth's root ..."). The menu says her hair doesn't follow yet.
  - **Not known:** whether the game's lighting copes with very large or small sizes (the skinning matrices carry the
    size in their rotation rows).

- **Road traffic keeps to the road, keeps its distance and never shows inside another car** (the user, 2026-11-03:
  "Decompile the entity swarm and their pathing logic for road cars and improve the road traffic pathing they should
  always follow the road respect each other with distance and not be on top of each other high priority no mistakes";
  a hidden QoL feature like Dogen's door: always on, no menu entry, no config key). Build 11413E1A2147, built 12:53
  on 2026-11-03 from the shared folder (no source newer than it; its strings hold 31F4EF5B37C7's); INSTALLED 12:54
  (game closed; game folder + dist, hashes checked, again after 30 s; over 2D05F48452D5 of 12:50, the same rules
  without the lane-table counts in the log). **Not tested in game.** Research and offline tests:
  Research_Road_Traffic_2026-11-03.md in the top folder.
  - **Why cars left the road and stacked up [decompile + data]:** the game's car step (0x143B8B0D0) chases each car's
    next corner at a capped turn rate; its lanes are a share of each node's radius, which puts the two-lane roads'
    cars on the pavement or the median for long stretches (only 80% of the game's lane line on tarmac); its avoidance
    only sees the cars on a car's own two nodes, is switched off in the highway's settings, and two placements share
    one road without seeing each other; respawns go onto spawn points as often as every 0.01 s whether or not a car
    is there.
  - **Now:** Camera/RoadTraffic.cpp hooks the game's car step and its spawn (0x143B99C40), both checked by their
    first 32 bytes. The step runs with its own avoidance off (a settings copy for the call); after it, the rules
    (Camera/RoadTrafficCore.h) move each car along its lane (Camera/RoadTrafficLanes.h: every segment's lanes sampled
    on the tarmac of Resources\GroundRoads.bin) by at most what the game moved it, keep a gap to the cars in its way
    (other car placements' too), and make a car that would show inside another, or turns round at a dead end, drive
    on unseen until it's in a gap. A spawned car starts unseen. CT.log has a "Road traffic:" line once a minute with
    the rules' time per step.
  - **Offline test (a C++ copy of the game's step on all 21 car placements, 4 x 300 s):** cars inside another car
    11.86% of car-frames without, 0.00% with; footprints on tarmac 88% without, 98.6% with; 1,167 cars seen a frame
    (1,208 without); no car stood longer than 12 s. The rules' cost there: 1.2 - 1.5 ms a frame for all placements on
    one thread.

- **GAME > Faith > Size, second build: her body and box pose sized directly, her hair set back at 1.00** (the user,
  2026-11-03, testing 9B8C4CFC7806: "No her body doesn't scale only her hair back to normal doesn't work either first
  person camera doesn's strink either"). Build 31F4EF5B37C7, built 12:44 on 2026-11-03 from the shared folder (no
  source newer but this change; the road traffic session's RoadTraffic*.h aren't in the project; its strings hold
  all of 9B8C4CFC7806's but the size's own); INSTALLED 12:48 (game closed; game folder + dist, hashes checked).
  **Not tested in game.**
  - **Why the first build drew no bigger body [live, read only, 12:3x]:** with bone 1's scale at 0.50 her hips stayed
    0.870 m from bone 1 (their local offset's length) and every skinning matrix row had length 1.0000: the game builds
    her world pose and skinning matrices from translations and rotations only. Only her hair cloth follows the bone's
    scale.
  - **Why 1.00 didn't bring her hair back [live]:** 0.50 stayed in bone 1's scale in her local, world and Ant poses
    with the override gone: the game never writes that scale back itself.
  - **Now:** every frame from the camera hook, both of her pose slots (body + 0x248, box pose + 0x340) get their world
    pose and skinning matrices sized about bone 1, bones from the hips on (each world place p1 + s (p - p1); each
    matrix's rotation rows times s and its translation p1 + s (t - p1)). A bone still holding what the tools wrote
    isn't sized again; a value the game wrote that's already nearer the sized length is left alone. At 1.00 (or when a
    replay records or plays) the bones still holding the tools' values get the game's back. Bone 1's scale is still
    set through the override for her hair, and at 1.00 is handed 1.00 until her pose reads 1.00 again (120 frames at
    most), then taken back.
  - **Not known:** whether the game writes her pose after the camera hook and before drawing (then nothing shows);
    the log's "Faith's size (diagnostic)" line every 10 s says how often the game had written it anew between two
    camera hooks. Whether her first-person view follows.

- **GAME > Faith > Size: Faith's model bigger or smaller, 0.50 - 1.50, about her feet** (the user, 2026-11-03: "add a
  slider that resizes faith model/collision box if it's possible no mistakes"; after the research, "Yes, build it" for
  a model-only slider). Build 9B8C4CFC7806, built 12:27 on 2026-11-03 from the shared folder (no source newer than the
  installed 27F6933FCD63 but this change: the road traffic session's RoadTraffic*.h aren't in the project); INSTALLED 12:28 (game closed; game folder + dist, hashes checked).
  **Not tested in game.** Research: Research_Faith_Size_2026-11-03.md.
  - **How [decompile]:** the game's animation system keeps a uniform scale per bone (the 4th float of each bone's
    "translation x y z 1") and carries it down the skeleton (0x142C1D5B0). The game's own pose override writes its 4th
    translation float as the bone's scale whatever its weight (0x142FCFA5A); rotation and translation are blended by
    the weight. Camera/FaithSize.cpp hands an override of bone 1 only (AITrajectory, at her feet: hips, legs, spine,
    arms, head and CameraJoint hang off it), weight 0, the size in that float, to both of her pose slots (the drawn
    body, soldier + 0x248, and her box pose, + 0x340, whose bones make her bounding box), every frame from the camera
    hook right after the replay's update, two overrides in turn per slot like the replay's. The rotation given is bone
    1's own from her last pose (at weight 0 the game keeps her animation's rotation either way).
  - **At 1.00 nothing is handed:** what was handed is taken back (only if the slot still holds ours), and her pose is
    the game's own again. The "1.00" button and anything within 0.005 of 1 give exactly 1. Not saved in the config.
  - **Steps aside while a replay records or plays** (the replay hands its own override to the same two slots and
    writes 1.0 into every bone's scale); the menu then shows the size greyed. A slot holding another override is never
    written.
  - **Checks:** the PE timestamp (0x5762E3B7, the replay's) and the bytes of the scale write, the override's copy into
    the pose job (0x142FD7D7A) and its handle check (0x142FCF62D); otherwise "Size: not available".
  - **Not changed:** her collision (the Ant movement capsule and her AI capsule body), her moves (their distances stay
    the game's), the free camera's FaithEyeHeight 1.6 / FaithFocusHeight 1.3, the snow's FaithHipsLow, the wet look's
    splash sizes, the replay. All of them are right at 1.00.
  - **Not verified:** the code that builds her skinning matrices wasn't found, so that her drawn mesh scales (and not
    only her bones' places) is inferred; whether the first-person view, her hair cloth and her culling follow.
  - **Log:** "Faith's size: X, bone 1's scale through the game's pose override (weight 0) on her body's pose slot ...,
    box pose slot ..." when handed; "now X" (the slider rested 1 s); "back to the game's own (1.00)"; "steps aside
    while a replay records or plays"; warnings when her slot or box pose isn't found or holds another override.

- **Replays: people's and drones' parts drawn with the bones the game's meshes list (the ghost runner's head, the
  hunter drone's body), Faith's hair no longer on a KrugerSec, the prison door's arrows hidden when the game hid them**
  (the user, 2026-11-03, after the 10:56 test of A080D9C7984E's work in the 10:48 build: "Hunter drone is missing once
  again", "this door texture should move with the door" (9.webp: a gate door's glass with the arrows), "ghost runner
  head still stays at place instead of following the body", "ksec has a weird faith hair next to him"). Build
  27F6933FCD63, built 11:25 on 2026-11-03 from the shared folder (no source newer than it, so it holds the Dogen's door
  session's FB7EA2579761 and the 11:22 F44D1CC7AD41 too); INSTALLED 11:54:57 when the game closed (game
  folder + dist, hash checked after 30 s and again later). **Not tested in game.**
  - **The ghost runner's head [game files + log]:** a skinned mesh level's section has, at + 0x41, how many bone matrices
    its draw has and, at + 0x44, a list of that many 16-bit skeleton bones in their order (+ 0x40 bones a vertex). Read
    from the game's files for the ghost runner (characters/projection/female_runner_projection_01/fullbody: 80 bones,
    the first 54 52 7), both enforcers, the subprotector and the hunter drone; the enforcers' counts (62, 50, 18, 16,
    15, 1) are the ones the replay had matched. The replay took 12 header rows before a draw's bones when they left only
    transforms; the ghost runner's part (243 rows a draw) has 3 and 80 bones, so its first 3 bones (the skeleton's 54
    and 52, the parents of its 34 face bones, and 7) were drawn as header rows kept from its first frame: the head stayed
    where it was. Now each person's part looks its section up by the game's vertex buffer (the mesh table, looked
    through again at most once a second for one not known; replay_shapes::SectionBones), takes the rows before the
    listed bones as its header, and is drawn with the listed bones when each fits the recorded pose (within 4 cm), with
    no ties to settle. Log: "a part's bones (N indices) taken as the game's mesh lists them: B bones after H header
    rows, K of them another skeleton bone than the nearest-fitting one" (once a part), "not taken as (one didn't fit
    the recorded pose)", or "not found in the game's mesh: matched as before".
  - **The hunter drone [game files + log]:** its mesh has a gun (6675 indices, bone 4), lights (600, 5 bones), the body
    (22569, 17 bones) and a copy with no material. The replay kept 2 parts, 7275 indices: the gun and the lights only,
    so it drew no body. A drone's part is taken from two draws of it only when every one of its bones moved between them;
    the body lists the skeleton's bone 2, which the gun and lights don't (that it stood still is inferred, not logged).
    Now a drone's or gun's part uses its section's list too: its header from it, any of its bones having moved is
    enough, and each bone is fitted to its listed recorded bone alone (as before where that doesn't fit and every bone
    moved; log "as the game's mesh lists them don't move as its recorded bones did"). The "kept N parts" line gives how
    many had their bones listed.
  - **Faith's hair [log]:** a cloth of 10497 indices, read only 110.6 - 114.1 s, was a recorded person's at 19 of its
    33 samples and Faith's at 14 (Faith was fighting the SubProtector found at 103.92 s), so it was kept as his and
    carried by his bone 140 for his whole track. Her place is where she stands, 1.6 m under her hair. Now she counts as
    the line from there up 1.9 m (within 0.8 m), and a cloth with a quarter of its samples hers isn't kept as anyone
    else's (the game draws it). That it's her hair: its samples and her hair cloth; its mesh wasn't known.
  - **The prison door's arrows [log]:** the camera kept per G-buffer pass (A080D9C7984E) worked: "taken with the place
    as a column vector" (34912 of 35243 places). Both signs got "start ... 9.9/10.1 - 31.1 s" (in the camera's view and
    not drawn 4 times after; drawn at 159 and 179 of their times in view before); the gate's two doors moved
    31.17 - 34.51 s. In the playback the game drew its own signs ("the game draws it again ... its recorded copy let go"
    at 19.32 s), which nothing in the live game had switched off, so they stayed after 31.1 s. Now a model whose
    window's hidden side the camera saw (3 times or more) has the game's own taken out of sight then: its transform
    shrunk to 1/10000 where it stands and moved 200 m down, given back once shown again and when the replay stops
    (ReplayModels HidesLive; saved as a "HIDE" trailer). Log: "is hidden by the game then, but the game has it: its own
    taken out of sight". Not verified: that the game's static model setter takes the scale.

- **Faith's wet footprints and handprints only on what's drier** (the user, 2026-11-03, after 0BA17DD04C1F "Looks
  right now": "remove the footsteps/handprints from wet surfaces it should only be on dry"; build C437A00F27E1, staged 10:53; superseded in dist by the Dogen's door session's FB7EA2579761 of 11:04, which holds it
  (WetLook.obj / GbufferWet.obj compiled after these sources, its new log text in the DLL, checked) and installs when the game
  closes).
  - No prints pressed into the wet ground any more: the lighter ones where the water was pressed out of concrete or
    stone and the darker ones in wet gravel (WetLook.cpp's StepWater no longer makes PrintPressed / PrintGravel).
  - Her wet shoes' prints on a wall she runs along, and her handprints on walls and poles, are left only where her
    shoes or hand are wetter than the wall (none on a wall out in the rain; before, a wall print was left however
    wet the wall was). On the ground this already held.
  - Drawn per pixel only as much as they're wetter than the surface there (PsMain's surfaceWet: the wet look's own
    wetness before the prints), so a print fades where rain later reaches it or where it crosses a roof's edge. A
    handprint now keeps how wet her hand was (the shader takes the surface off).
  - Shaders WetLookPs, WetLookRainPs, GbufferWetPs recompiled. **Not tested in the game.**

- **Puddles no longer step at a box's edges or fold along its top** (the user, 2026-11-03: the rain "still looks bad
  on top of vents/boxes/woodplanks", "It's the same" after B67B44C4FAE7; build 0BA17DD04C1F, installed 03:31, game
  folder and dist, hash checked). The G-buffer dump saved at 02:27 at a roof box showed the cause: the puddle tests
  (WetLook.hlsl's PuddleSurface and FlatAround) read points at fixed offsets round each pixel, and each point counts as
  dropping away or not. As a pixel moves, a point goes over the box's edge at once, so the puddle steps on a copy of
  the box's outline moved by each offset: those were the rectangles. Where the top's four faces (1.6 and 4 degrees)
  meet, the 3- and 8-pixel points lay a millimetre under the plane: those were the lines from corner to corner. A model
  of those tests on the dump's depth matched 99.9 % of the hard steps in the saved smoothness, pixel for pixel.
  - The 1 m test ("is there flat ground a metre round, or is it a small top") is now worked out once a frame at fixed
    points of the world: 256 x 256 nodes 0.5 m apart round the camera (WetLook.hlsl's NodesPs, drawn by
    GbufferWet.cpp before its info, or at Present when that pass didn't draw). Each pixel takes it from the 4 x 4
    nodes round it, smoothly (GroundGoesOn: all of a node that's higher, its broadness where it's level with the
    pixel, none where it's lower). From 49 m round the camera it eases out to as if none were known.
  - The hole test counts a ring's points only in opposite pairs that both lie under the plane (metal with holes), so an
    edge or a gap on one side isn't a hole. The stairs test takes only step-sized heights (6 - 35 cm), so a box's
    corner with a vent rising behind it isn't a tread.
  - FlatAround needs a drop of 1 cm (or 0.1 % of the distance) as well as its slope, so a fold isn't an edge.
  - These replace PuddleSurface for the puddles in the game's G-buffer, at Present and in the query for her steps
    (g_Options.w). The water down slopes keeps PuddleSurface.
  - Checked on the dump with the same tests in the model: hard steps of the puddle factor on the two box tops 42,242 ->
    274 pixels (lower box) and none left on the upper one except at real geometry (the rail's foot, grates). The
    broadness's largest jump beyond its own ramp is 0.023. **Not tested in the game.** Not covered: puddles past
    about 60 m (no broadness there), and a fold seen at a glancing angle far off may still cut a thin line.
  - Shaders: WetLookPs, WetLookRainPs, WetLookQueryPs, GbufferWetPs recompiled, WetLookNodesPs new (the snow's ground
    copy compiles to the same bytes); 43 / 41 / 24 / 34 temps. The G-buffer dump also saves the nodes.

- **Lights: lights from code, and lights made again when the game drops them** (for the Dogen's door session's
  kitchen lamps; build d064b4281d56, staged 02:53, installs when the game closes).
  - `LightManager::AddFixedLight` / `RemoveFixedLight`: lights made by the tools' own code. They aren't in the
    Lights list, its dots or Remove all.
  - Each light's render handle (entity + 0x80) is now checked about once a second, for the menu's lights too. It's
    the index of the light's 0xC00-byte entry in the game's render lights ([0x142568CE0] + 0x48, 0x1436E4F80). That
    entry keeps its own index (+ 0xB4, put back by each update at 0x143707581), its position, radius and shape. A
    full rebuild of the game's lights (0x1436EA240, after some graphics settings changes) empties the list and
    makes only the game's own again.
  - A light unchanged for 1 s whose entry is gone, freed or someone else's twice in a row gets its handle reset and
    is made again. Before, it stayed dark and its old handle could later turn another light off.
  - A light being turned off or removed is checked the same way first. If its handle isn't its own any more,
    nothing is sent through it.
  - The "made before a loading screen" line is gone (the check covers it). "Forget all" now says Remove all is the
    one that turns lights off, and it no longer drops the fixed lights' queued changes.
  - **Not checked in game.**

- **A small easter egg in Anchor** (the user, 2026-11-03; on their word it has no menu entry and no config key, and
  isn't described here: see Research_Dogen_Apartment_2026-11-03.md in the top folder, which isn't part of a release).
  Its lamps dimmer at night (the user, 11-08: "Lights should be getting dimmer during night time in dogans
  apparment"; that file's section 8): build 9C910A40338C (11-08 12:47, game folder + dist over 24F1284D7D31, the game
  closed, hashes checked), with LightManager::SetFixedLightIntensity added for it by the Lights session. Camera/
  DogenDoor.cpp and Camera/DogenGlow.cpp / .h changed. Not tested in game.
  A second one in The View (the user, 11-03 21:1x: "Can you make the door faith is next to now same as the dogens
  door?"; that research file's section 7): build 1D64414A9889 (21:46; its install at the game's close left the folder
  alone, as it then held 1DBA564AA511, a later build of the shared folder: the door is in the builds since, checked in
  6E1A17004951). Camera/DogenDoor.cpp now takes a list of doors; Dogen's is as it was, its log's "found it" line now
  has the instance. Not tested in game.
  Build FDCFC96954D9 (16:13, game folder + dist over another session's 6E423806B377 of 16:07, hashes checked; built
  from the shared folder after it, no source newer): the lamps' light at 1,350 lm each (the user, 11-03: "increase the
  lights strength to 1,350lm"; was 1,000 lm), nothing else of it changed. Before it FB7EA2579761 (11:04; its
  game-folder install at the game's close left the folder alone, as it then held a newer build), whose changes are in
  the installed 27F6933FCD63 (another session's whole-folder build of 11:25, in the game
  folder and dist; checked: block 36's new tables and the glow are in it). Before it: 656B6CB3154B (10:49, game folder + dist
  over 0BA17DD04C1F, hashes checked; two calls added to Visuals/GameFrame.cpp with the snow session's go-ahead; new
  Camera/DogenGlow.cpp, .h, .hlsl and its shaders' headers), 045F2AFC7972 (03:16), 9DB55AC6495D (02:57, over the Lights session's
  d064b4281d56, which
  holds its LightManager::AddFixedLight), 2D1D257923BB (01:49),
  610A02C44F1F (02:02, tested 02:1x: "doesn't seem i can enter", "the weird collision inside"), 1FD47E66D94E and
  7AE20C252A96 (staged, never installed). Built from the shared folder. Changed: Camera/DogenDoor.cpp / .h (new),
  an include and one call in Camera/CameraManager.cpp, util::hooks::SetModelGroupMemberVariation (new, Util/Hooks.cpp
  and Util/Util.h), the project files. **The 03:04 game's log shows it working** (both of its checks passed, the
  lights made, Faith's steps inside and then out through the door); 045F2AFC7972's changes aren't checked in game;
  656B6CB3154B's were seen working (the user, 11-03 11:0x), FB7EA2579761's and FDCFC96954D9's aren't.

- **Replays: the prison door's arrow signs hidden once the game switched them off, the late KrugerSec's baton recorded,
  checks for the stretched heads** (the user, 2026-11-03, after the 00:21 test of B67B44C4FAE7, which held
  BEEEBE0362C7's replay work: "ghost runner head is still streching", "the ksec was missing the batoon", "Door arrow
  textures don't move with the doors either"). Build A080D9C7984E, installed 00:58 on 2026-11-03 (game closed; game
  folder + dist over B67B44C4FAE7 / 15513B272920, hash checked after 30 s; built from the shared folder, every object
  newer than its source, so it holds 15513B272920's sources too). **Not tested in game.**
  - **Arrow signs [game data + log]:** the Release prison door prefab
    (pf_area00_release_animatedprisondoor_nongroupable_autogen_gen4) has the two arrow signs (plane_1-1_01_gatesign_01)
    as planes that never move and the two doors on a timeline. Its open input event starts the doors' timeline and sends
    the event to the bool that drives both signs' Enabled (DefaultValue True). That event is read as "set false" from
    the wiring (the other input sets it with the signs' flicker timeline); not read in the game's code. So the game stops
    drawing the signs as the doors open. The replay drew its copies (kept at 7.9 s) for the whole playback: the show
    windows judged "hidden" only from neighbours drawn within 4 m without them, and the doors were the only ones, further
    off once open. Now the camera of each G-buffer pass read back while it records is kept (viewConstants + 160
    viewProjMatrix, + 720 cameraPos); a model the game never drew in its G-buffer pass also counts the times it was in
    that camera's picture, no further than its own draws were (+ 5 m), and not drawn. Only for one drawn at most of its
    in-view times between its first and last draw (3 at least), since draws are read back up to 48 a pass. Which way
    the matrix is used is judged from the G-buffer pass's own draws (log: "the recording's camera ... taken with the
    place as a row/column vector"). The log's diagnostic line for such models now has "in the camera's view and not
    drawn N times before and M after".
  - **Baton [log + inference]:** the SubProtector found at 135.65 s had no weapon recorded; every other melee KrugerSec's
    was (41046-index draws, the baton mesh's). A body found was taken for known when any recorded one, gone or not, had
    its address; three weapons had gone at 70.96 s. Now a gone one's address doesn't count, and a body found at a gone
    one's address is logged ("found with the body address ... of a recorded one that went at N s"). That the game reused
    that memory is inferred, not seen.
  - **Stretched heads: not fixed, diagnostics only.** The ghost runners (SpawnTemplate_Projection_Replay, 182 bones, one
    part of 20964 indices, 70 of 77 bones fitting more than one skeleton bone) were there 1.9 - 2.6 s, so their pose
    was never checked (first check at 3 s); its 41 hierarchy ties were never settled. New log lines: the first pose
    check 1 s in; a later frame of a part whose bones don't fit the skeleton bone they're drawn from ("drawn from a
    skeleton bone that didn't fit it then"); a part's header rows (kept from one frame) that change ("header rows ... not
    the same in this frame of it"); a part drawn with a bone more than 3 m from the person, or its matrix's place more
    than 8 m ("drawn far from where it is").
  - **Log:** "drawn with its material values" at most 2 a mesh, 150 in all (the 00:21 test's 40 went to air conditioners
    and billboards, so the signs' lines weren't there).

- **Diagnostic: save the game's G-buffer where the wet ground looks wrong** (the user, 2026-11-03, after B67B44C4FAE7:
  "It's the same": the patches and lines on a roof box's top weren't what that fix took them for). VISUALS > Weather >
  Wet surfaces > *Save the G-buffer (diagnostic)*, under *In the game's own lighting*: the next frame the wet ground
  goes into the G-buffer, its targets 0 - 2, depth and view constants as they were and targets 0 - 1 and the info as
  the pass left them are saved raw into Cinematic Tools\GbufferDump\ (about 250 MB at 4K), with a text file saying
  what each is. Build 15513B272920, staged 00:26 on 2026-11-03: it installs itself when the game closes (game
  folder, hash checked; dist has it). B67B44C4FAE7's 00:19 - 00:22 test: the pass at Present 0.84 - 0.96 ms (about
  3 ms before), the G-buffer pass 4.2 - 5.4 ms (where it's open and kept dry about 1.5, the wet ground about 3.8).
  **Not run yet.**

- **Wet surfaces in the game's own lighting: faster, and no patches or lines on vents, boxes and planks** (the user,
  2026-11-02, after testing D1308F6536CE: "Yes, speed it up"; then "the rain still looks bad on top of vents/boxes/
  woodplanks find a way to fix it no mistakes", with a screenshot of a roof box's top: hard-edged darker rectangles
  and straight lines across it). Build B67B44C4FAE7, installed 23:51 on 2026-11-02 (game closed; game folder + dist
  over D1308F6536CE, hash checked). **Not tested in game.**
  - **The patches and lines** (GbufferWet.hlsl): the wet ground's smoothness went only part of the way to the wet
    value (as wet as Wetness made it), so a decal's or a stain's own smoothness, different from the surface's, still
    showed; around 0.6 - 0.7, where the game's screen-space reflections start, each became a hard-edged patch that
    reflected more or less. And the facing written was the game's own, which turns a little across a model's
    triangles and inside its decals: once glossy, lines and patches. Now once the surface has enough water on it (from
    30 % wet) the water's film sets its smoothness, whatever was under it; where the depth's surface is level and clean
    the water lies on that plane (the decals' and triangles' own facing replaced); elsewhere it's the game's exact
    facing. The thin and small things (vents' slats, planks and their gaps, the edges of a box's top) were left dry in
    stripes where the depth jumped; their wetness now follows their own facing.
  - **Faster:** a first small pass in the G-buffer pass works out once where it's open to the sky and what's kept dry
    (the weather's squares, the game's interiors, Faith's, people's and vehicles' boxes) into a texture of the tools'
    own; the wet ground's pass and the wet look at Present read it instead of working it out again (at Present that
    was about 1.9 ms of its 3.1 in the 23:26 breakdown). The wet ground's pass also skips everything else where the
    ground is dry and no print or tyre track can reach. Not measured yet.
  - **Log:** the G-buffer pass's 10 s line now splits its GPU time: "where it's open and kept dry" and "the wet ground".

- **Wet surfaces in the game's own lighting** (VISUALS > Weather > Wet surfaces > *In the game's own lighting*, on by
  default; the user, 2026-11-02: "Lets go for A option", fix 7.A of Research_ScreenSpace_2026-11-02.md, the weather
  session's go-ahead on its files). Build D1308F6536CE, installed 22:47 on 2026-11-02 (game closed; game folder + dist,
  hash checked; over 72B22F8CEEE7, installed 22:43 over BEEEBE0362C7: the same change, without the breakdown's note
  below and with Weather.cpp's line ends turned to LF by mistake, put back to CRLF). **Tested 23:19 - 23:25: the user,
  "it looks much better than before".** CT.log: drawn in nearly every frame (two single frames skipped, no G-buffer
  copy); its GPU time 1.8 - 7.2 ms a frame (mostly 3 - 5) at 3840 x 2054, and the pass at Present still 1.8 - 3.6 ms
  (its kept-dry boxes 1.15 ms and the shelter and interiors 0.76 in the 23:26 breakdown): together more than the
  5.9 ms the pass alone cost before.
  - **What it does:** as the frame's first lit pass begins (after the G-buffer, its decals and the two G-buffer passes
    after them; before the game's screen-space reflections: the 21:36 order log), one full-screen pass
    (Visuals/GbufferWet.hlsl, the wet look's own functions) writes the wet ground into the game's G-buffer targets 0
    and 1: smoother (0.5 - 0.75 by Reflections, puddles 0.85 - 0.98), darker base colour (as the wet look darkened the
    frame), puddles' facing flat with the rain's ripples and the step, drip, pipe, slide and body rings, the slopes'
    rivulets. The game's own SSR, cube maps and lights then show it: no halo round what stands in front, the cube maps
    where a reflection misses. The wet look at Present then leaves its wet ground's darkening, sheen and reflection out
    (g_Options.y); body wetness, handprints, lightning, distant rain, haze, drips and splashes stay as they were.
    Off: as before.
  - **The camera:** the wet look's constants are a frame old by then; the pass takes them with this frame's camera's
    turn and projection (greenplane::FrameCamera) and moves its points for the lists (prints, rings, the boxes kept dry,
    interiors) by the camera's motion since.
  - **Fixed on the way (the snow and the wet look):** the game's G-buffer copy (GameFrame.cpp) was taken as the frame's
    first G-buffer pass ended, before the decals and the two G-buffer passes after them, so it had no decals and
    showed what was under whatever those passes draw. It's taken as the first lit pass begins now.
  - **Log:** "Wet surfaces in the game's lighting: ..." (its first draw, why not drawn) and every 10 s its GPU ms
    ("(performance, diagnostic)"). The performance test's breakdown of the wet look's pass says when the wet ground was
    in the G-buffer (its parts for it are then about 0 there).
  - **Not known until tried:** the look (the game's SSR is half-res and temporal, fades out 100 - 150 m away and when
    looking straight down; puddles from above reflect by the game's Fresnel now, less than the tools' 30 % floor),
    its GPU cost, a possible hitch the first time it draws (its shader isn't warmed up), and what the game's two
    G-buffer passes after the decals draw.

- **Crash fixed: the rain's sound made from a world that had gone** (the user, 2026-11-02: "Find out why the game
  crashed", then "Yes, fix it"). Build BEEEBE0362C7, installed (game folder and dist, hash checked). **Not checked in
  game.**
  - **What it was** (CT.log 21:40 - 21:47): a replay of Release streamed Release's world in, and the game made a voice
    for the rain sound that came with it, which the weather took for its own rain voice. When the replay ended the game
    streamed that world out (21:47:09). 5 s later, out from under a roof, the weather let its voice go and made a new
    one from that asset; the game crashed in its sound code (0x143022492, reading address 0), with a register pointing
    into the asset. The asset still read as it had (its first bytes and its name), so the tools' check passed.
  - **Now:** before a mission's world streams out (the game's own and the replay's), at a loading screen's start, and
    when a rain of the game's is gone, the weather lets its rain voice go and forgets the asset at its next update, and
    takes none for 15 s while that world goes; then it takes the one the game makes next, as after a death. The rain
    can be silent for 15 - 25 s after such a change.

- **Replays: the gate signs' arrows move as the game moved them, Icarus's hoodie stays on when the game stopped drawing it,
  no more "shown part of the time" for air conditioners** (the user, 2026-11-02, after the 21:36 test of BA75E1C5BC9D /
  616AFE8848BB with D090DD0807D4's replay work: "everything looks fine (icarus hoodie, foodome texture gone)", "gates
  textures don't move with the doors", "when faith isn't around icarus anymore the hoodie is gone"). Build 30800192696E,
  installed 21:56 on 2026-11-02 (game closed; game folder + dist over 616AFE8848BB, hash checked after 30 s; built from
  the shared folder, every other session's object newer than its source, SsrOrder.cpp among them, except
  Camera/ReplayAssetLog.cpp: the weather session's 21:55:28 crash-fix edit came after its object, so it isn't in this
  build). At 21:58 the weather session's BEEEBE0362C7 replaced it (game folder + dist): it holds these changes and that
  crash fix (every object newer than its source, checked). **Not tested in game.**
  - **Gate arrows:** the 21:36 log has the two gate sign planes (60.3 / 61.8, 8.5, 1176 - 1180) drawn by the game only
    in its lit picture's pass and only where they stand (no draw of them away from there: D090DD0807D4's follow never
    applied). The gate's events start two timelines that drive two ShaderParameterEntities on them (game data, section
    1), so the arrows' motion is their material values; the replay drew them with the values of the moment it kept
    (8.4 s). Now a recorded model's later draws of the same objects keep its b1s (externalConstants) each time they
    change, with the recording's time (and the last time before a change that they held), at most 4096 a draw and 64 MB
    in all; while it plays its b1s are the tools' own, their plain numbers in a straight line between the samples around
    the time played. Taken every 30 frames as before (about 0.5 s). Not saved with recordings. Log: "drawn with its
    material values as the game had them at each time (N samples ...)". That the values are in b1 is from the shaders'
    names (externalConstants), not read in the game's code.
  - **Hoodie when Faith isn't near Icarus:** the game draws a cloth only while it's in sight; Icarus's samples ended at
    90.4 s (he was recorded to 103.9 s) with gaps. Each kept cloth now has an owner: the recorded person with the bone
    nearest its vertices' middle (within 1 m) at its first sample that has one, that bone's pose kept for each sample.
    Where no sample is near the time played, its sample nearest in time is moved as that bone moved since (its vertices'
    places, or its instance's transform when they're in its own space; their normals as they were), only while that
    person is drawn. No cloth sway then. Log: "N of the M cloths kept carried by the person they're on".
  - **Less cloth memory:** a cloth draw keeps only its buffer's bytes from its own offset (two of Icarus's cloth draws
    share one 229296-byte buffer; the second's start at byte 211296, both kept it whole: 283 MB in the 21:36 test).
  - **"Shown part of the time":** back to models the game never drew in its G-buffer pass (D090DD0807D4's widening gave
    air conditioners, lights and a billboard windows from far-off draws in the 21:36 test).
  - **Bone order (stretched heads):** the 21:36 log disproved D090DD0807D4's assumption (0 parts in the skeleton's
    order, 3 - 5 of each person's not), so it never applied; now it can't apply while any part judged wasn't. The
    stretched heads aren't fixed by it.

- **A read-only check of the game's screen-space reflections** (the user, 2026-11-02: "Yes, check first", to the
  check that Research_ScreenSpace_2026-11-02.md's fix 7.A needs). Build 616AFE8848BB, installed (game folder and
  dist, hash checked). **Not checked in game.** Nothing of the game's is changed: Visuals/SsrOrder.cpp only watches.
  - **Its settings, once:** GlobalPostProcessSettings' fields for them (ScreenSpaceRaytraceEnable, Quality, Fullres,
    DeferredResolve, ReuseAlloc, ThicknessFadeScale, AsyncComputeMethod, BlurPyramidEnable), read through the game's
    settings registry, in CT.log as "Screen-space reflections (diagnostic): GlobalPostProcessSettings ...".
  - **Where they run, 3 frames:** once the game draws the world (5 s after the tools hook it, a second apart), each
    frame's order in one line: the pass changes (G-buffer, decals, lit, ...), the reflections' driver (0x1435BBB00,
    hooked only if its first 16 bytes are the image's) starting and ending, the compute dispatches and command lists
    between them, and whether the driver ran after the decal pass ended (what fix 7.A needs).
  - **How often:** the driver's calls counted for 10 s after that, in one line. Then the hooks only pass the calls on.

- **The snow's pass faster, with the same look; the ice no longer comes and goes on pools** (the user, 2026-11-02:
  "Check the snow performance test and find a way to improve performance like the rain session also", then, with a
  screenshot of a pool partly ice, partly water: "You can still see the snow interacting with the water z fighting on
  water fix it"). Build C90DE4523A0A, installed 20:48 on 2026-11-02 (the game closed; game folder and dist, hash checked; over the weather session's BA75E1C5BC9D, built from the shared folder at 20:45 with its sources, nothing else changed since). **Not checked in game; how much faster isn't known until the next snow
  performance test.**
  - **What the test showed** (CT.log 20:09 - 20:10, snow lying): 97 fps with everything on, 144.5 with the tools'
    drawing paused; the wet look's pass 3.6 - 6.2 ms of the GPU's at 4K, the ice and the flakes 0.13 - 0.19 ms. The
    breakdown had the snow inside one part ("the rest", 4.5 ms of a 3.6 ms pass: the rain's parts, which don't run in
    the snow, came out at plus and minus 2 ms of noise). Its third phase isn't usable: the game drew only 248 frames in
    it.
  - **A copy of the pass for the snow**, the rain's copy the other way round: the same shader without the rain's code
    (the wet ground, its prints and tracks, her splash, the drips, the drainpipes, the splashes, the distant rain).
    It's drawn while snow lies and none of those is on this frame, read from the pass's own switches, so it draws what
    the whole one would. fxc's count: 28 temps and 15,838 instruction slots, against 43 and 31,079 for the whole
    one: with less room kept for each pixel, more are worked on at once. How many more is up to the graphics
    driver.
  - **The pile against walls and curbs** found its foot reading the depth one row after another (up to 9 rounds, each
    waiting for the one before); now 4, with the same rows and the same result (the rain's reflections got the same
    change).
  - **The performance test's breakdown** splits the snow into 6 parts now: its piled side, what's pressed into it, its
    light, its edges and what's kept out of it, its facing, and the rest. It times the whole pass and the snow's copy in
    the same frames. 22 draws of the pass in each of the 3 frames right after a test (a short hitch).
  - **The ice** was taken off wherever the weather's 2 m squares find something over it (a bridge, the monorail's
    track, a sign), squares looked at again every 2 - 5 s and none past 50 m. So as the camera moved, a pool's ice came
    and went in straight-edged patches, while the waterfalls over it stood frozen (those run on only inside the game's
    interiors). Now the ice goes by the same interiors: water under a bridge freezes, water indoors stays water.
    **Not confirmed** that this is what the screenshot shows: where the camera was isn't logged. The ice's pieces for
    that plaza's pools are at the water's height (within 4 cm where Faith stepped in it at 20:14 - 20:16).
  - **New log line** while the ice is drawn, at Faith's step in the game's water (once in 30 s): "Snow (diagnostic):
    Faith's step in the game's water at ...: the ice there is ... at h m, n cm over/under her", or that there's none
    there. If a pool still isn't iced, stepping into it tells whether the ice has a piece there.

- **The rain's pass faster, with the same look** (the user, 2026-11-02: "New rain performance test can you read it
  and find any improvements? no mistakes high priority"). Build BA75E1C5BC9D, installed when the game closed at 20:29 (game folder and dist, hash checked twice 30 s apart; over the replay session's D090DD0807D4, whose changes it holds). **Not checked in game; how much faster isn't
  known until the next performance test.**
  - **What the test showed** (CT.log 19:52): the wet look's pass took 8.5 ms of the GPU's at 4K (6.6 - 9.7 ms a frame
    over the minutes round it). The breakdown: reflections 1.14 ms, the puddles' surface tests 1.14, the rest of the
    wet ground 2.28, and 3.90 for everything else (the frame's facing, the shelter, the interiors, prints, haze,
    lightning, the drips, the rain in the distance) as one part. **Two of its parts were wrong:** the kept-dry boxes
    (-0.18) and the edge tests (0.06) were never really left out (HLSL works out both sides of ?:), so their cost is in
    the 2.28.
  - **A copy of the pass for the rain**, drawn while no snow lies: the same shader without the snow on the ground's
    code, which drew nothing then anyway. fxc's count: 31,000 instructions and 43 temps for the whole one, 18,406 and 40 for
    the copy. The GPU keeps room for the most a pixel can need, so with less of it more pixels are worked on at once.
    How many more is up to the graphics driver: the next breakdown times both copies in the same frames.
  - **Lists looked at only where they can reach:** every pixel of the frame went through all the drips (48 while it
    rains on the roofs), the drainpipes, the splashes and her own splash, and every wet pixel through where the drips
    and the drainpipes land and her footprints, to find that nearly all of them were too far. Now one box round each
    list (worked out on the CPU, a pixel or 5 cm wider than the shader's own tests) is tested first.
  - **Smaller ones:** the interiors' test read an array by a number worked out as it runs (kept in memory, not in the
    GPU's registers, in 13 places); the game's G-buffer is read only where the surface leans; the slopes' test and the
    squares' read for puddles only where they're used.
  - **The performance test's breakdown:** 15 parts now, each really left out (an if), and the whole copy timed too
    when the rain's is drawn. It holds the frame up longer right after the test: 18 draws of the pass in each of 3
    frames.

- **Replays: no other sign's look on the gate, gate signs shown only while the game showed them and moving with the
  doors, the loudspeakers' KrugerSec announcements, cloth (Icarus's jacket), people's tied bones chosen in the skeleton's
  order** (the user, 2026-11-02, after the 19:40 test of D229D8EE7198 with 448A17A12E0A's replay work: "the food-dome
  texture is still there in the replay when it shouldn't", "Just like the ksec the ghost runner head is stretched",
  "Icarus doesn't has a hoodie still", "the arrow texture from the doors doesn't move with them", "kruger sec
  annoucnment doesn't show up"). Build D090DD0807D4, installed 20:29 on 2026-11-02 when the game closed (game folder +
  dist, hash checked after 30 s). Built from the shared folder at 20:16 (holds the wet look's
  performance test). **Not tested in game.**
  - **Gate "FOOD DOME" look:** the gate's sign planes (plane_1-1_01) had their own draw kept in one pass, so in another pass
    the one look kept of that shared plane mesh, another sign's, was taken as "one look" and drawn on both panes. A model
    that didn't move is now drawn in each pass only with a kept draw that has its own instance (within 0.3 m); none:
    that part isn't drawn. Log: "not drawn for a model that didn't move" and, within 30 m of the replayed Faith,
    "a model that didn't move at x y z drawn with the game's draw kept at N s".
  - **When the signs were shown:** 448A17A12E0A's check left out every model the game drew in its G-buffer pass; the
    asset log of the 19:40 test has plane_1-1_01 there (37 - 46 s). Now any still model counts. It's shown only from its
    first to its last draw when its neighbours (within 4 m) were drawn without it at least 3 times before / after, Faith
    no further from it than when the game drew it (+ 3 m), the game drew it steadily in between, and at least 3 times or
    over a second (448A's rule gave an air conditioner 0.0 - 0.0 s). A start or end without such evidence stays open.
  - **Arrows not moving with the doors:** the sign planes' own transforms never changed, but the game drew them on the
    gate's doors as they opened. A still model the game drew away from where it stood (2 draws at least) now follows the
    recorded model that moved whose moves put it within 15 cm of every one of its draws (within 12 m); its own copy in
    the game isn't moved, and its kept draws are matched where it was when each was kept. Saved with recordings ("FOLW"
    after "SHOW"). Log: "N models that didn't move themselves follow a recorded one that moved", "drawn by the game
    elsewhere too, along no recorded model", and up to 40 diagnostic lines of what was seen of each.
  - **KrugerSec announcement:** the loudspeaker lines (ME_Sound/VO/VO_PA_LoFi at 31.9 s and 41.2 s of the 19:40
    recording) were left out of the sound track to be said live, but the playback had 11 of the 39 recorded voices
    (the mission's sounds gone). They're kept in the track now like the narrative lines, heard from it only when the
    replay can't say them live.
  - **Icarus's jacket (cloth):** the 19:40 log's new line found a G-buffer draw whose vertex shader doesn't read bones
    and whose vertex buffer the graphics card writes (2493 vertices of 48 bytes, bind 0x89, misc 0x20): EACloth's output.
    Such a draw is now a track of its own, keyed by its index buffer, its vertices copied with each sample (every 0.1 s,
    16 a frame, 384 MB in all) and blended between samples in the 4-byte columns that are plain numbers. While it plays
    they're put in a buffer of the tools' each frame. When it stops, a cloth is kept only when its vertices' middle was
    nearest a recorded person at most of its samples (not Faith's, not nobody's: the game draws those). Not saved with
    recordings. Log: "a cloth's draw kept as a track", "cloths: N kept ...".
  - **Stretched heads (ghost runner, KrugerSec):** a draw's bone matrix is the bone's pose against the bind pose, so
    bones that hadn't moved against each other give the same matrix; the frame a part was kept from couldn't tell them
    apart (the ghost runner's 70 of 77 bones), and a wrong one is pulled away once the person moves. Whether a part's bone
    list is in the skeleton's order is judged from its bones that fitted one skeleton bone only: a part with 5 of them in
    rising order counts as in order. When a part's are in order (8 of them, or 2 while every part judged was), its tied
    bones are chosen in rising order with the least summed distance. **Assumption, not read in the game's code**; the
    log's "parts' bones in the skeleton's order, ties chosen in it, N not" says whether it held.

- **The performance test times the wet look part by part** (the user, 2026-11-02: "Yes, add it", to a per-part GPU
  breakdown of the wet look in the performance test). Build 3F2FF6FD1F4A, staged: it installs itself once the game closes (60 s
  after, only over an older build). **Not checked in game.**
  - **When:** right after a performance test (MISC > Performance), while the wet surfaces are drawn. In 3 frames 0.3 s
    apart, their pass is drawn 7 more times into a frame of its own that's never shown, timed by the GPU. That's a short
    hitch of about 7 times the pass's cost in each of those frames. Every frame's own drawing is as before.
  - **How:** each extra draw leaves out one more part than the one before (WetLook.hlsl's g_Skip). The reflections go
    first, then the puddles' surface tests, the edge and slope tests, the kept-dry boxes, the rest of the wet ground,
    then everything but reading the frame, then that too. Each part's cost is the difference, the mean over the frames.
  - **Where:** one line in CT.log, "Performance test, the wet look's parts (GPU ms ...)". If it can't be made, a line
    says why: no rain or wet look drawn in the 15 s after, or the GPU's timings didn't hold.

- **No hitch once a second in the rain from the scan for what's kept dry** (the user, 2026-11-02: "Yes, fix the spikes").
  Build 9784B5460FB8, installed (game folder and dist, hash checked after 30 s). **Not checked in game.**
  - **What it was:** the weather's performance line showed "people kept dry" at 0.06 - 0.15 ms a frame on average but
    4.5 - 8 ms in single frames: about one frame a second. The wet look looks for people, traffic, drones, vehicles
    and trains within 190 m once a second (DryShapes), through the scan the free camera and the replay use too
    (AttachTargets). That scan read and searched the mesh name of every static model in the level, near or not. With
    the whole city loaded that's thousands a scan, all in one frame of the game thread.
  - **Now:** a static model past a scan's radius is measured first, from its place and its box, exactly where the scan
    measures it. Only a train, an airplane, a blimp or a VTOL is listed that far, and which a model is comes from its
    mesh's name, so that's read once per model and remembered while it stays the same model. Every scan with a radius
    lists exactly what it did before: the wet look's, the camera's nearby list, the replay's, the lair windows'.
  - **To see it:** the weather's performance line now gives the scan's own time ("their scan ... ms each"). Not
    known: how much of the spike was the name reading. Reading each model's place is still done every scan, so the
    scan can still show up there.

- **The rain's wet look faster at 4K; no light halo round people in the rain** (the user, 2026-11-02: "Rain still
  drops your fps by a lot read the latest performance test and find ways to improve it"; a screenshot of two KrugerSec
  on a wet floor, "there is a halo around units faith as well", "with rain enabled", "check if its the screen space if
  it is fix it no mistakes"). Build 88FFAC80A64B, installed (game folder and dist, hash checked after 30 s). **Not checked in
  game.**
  - **What the test showed** (MISC > Performance, 15:59, 3840 x 2054, RTX 5080): 116 fps with the tools' drawing paused,
    76 with it on. The wet look's pass took 5.6 - 7.4 ms of the GPU a frame; everything else of the rain under 1 ms.
  - **Why so slow (most likely):** far longer than its arithmetic needs on that GPU. Two parts read the depth one point after another, each
    read waiting for the one before to tell whether to go on: the reflections' march (up to 48 steps a ray) and the
    puddles' check for holes, stairs and edges (24 points, then 8 and 8). The ripples' edge test did the same.
  - **Now, with the same picture:**
    - the reflections' march reads 4 steps at a time and goes through them in order as before;
    - the puddles' check reads 4 points at a time (stairs and edges all at once);
    - the ripples' edge test reads its points together;
    - a reflection that the game's own fade for rays coming back at the camera takes away entirely isn't traced at
      all. Looking down at the ground steeper than 60 degrees, that's every one.
    - Not known: how much faster it is on the user's GPU. The shader holds more values at once now (fxc's count 38 ->
      47), which can let fewer pixels run at a time. The next performance test will show it (the log's "the wet look's
      pass ... ms").
  - **The halo** was the screen-space guard added on 2026-11-01 for the wet look over outlines. Within 3 - 8 pixels of
    anything nearer than the ground, the ground lost up to 60 % of its darkening and all its mirror and sheen. On a
    dark wet floor that left a light band round everyone. Now it only applies where the pixel's own colour is what's in
    front's (its antialiased or motion-blurred edge), told from the colours of the points round it. The ground next to
    someone is wet like the rest. The reflections weren't the cause: they're cut in that band, and outside it a body's
    reflection could only fall on one side of it, not all round.

- **Replays: signs the game switches on and off are drawn only while it showed them; bone maps steadier; a log to
  find the cloth draw** (the user, 2026-11-02, after the 15:53 test of 5EE8947A05CD: "Doors still have the solid
  texture for some reason from one side", "Icarus is still missing the hoodie", "ghost runner model is bugged").
  Build 448A17A12E0A, installed 16:12 on 2026-11-02 (game closed; game folder + dist, hash checked; built from the
  shared folder, so it holds the other sessions' work up to 16:03, including the GAME tab tidy-up and the street
  level work after F4317ECCFBDB). **Not tested in game.**
  - **The gate's "solid texture from one side":**
    - The asset log of the 15:53 test: the replay drew plane_1-1_01 (the gate's sign planes) at 6 - 92 s of the
      playback, but the game drew that mesh only at 29 - 38 s of the recording. The gate's doors were drawn from 6 s.
    - The signs are switched on by the gate's events (their Enabled). The game's creation log shows no plane made
      then, so switching them on only makes the game draw them; the replay doesn't record that.
    - A sign plane is one-sided, which is why it shows from one side only.
    - **Now** the replay notes where the game drew each recorded model's mesh while it records: each part's draw it
      already reads back, every half second.
      - A model that didn't move, and that the game drew only in its decal, lit or half-size passes (signs, screens),
        is drawn only between the first and last times the game drew it.
      - That applies when the game drew the models next to it (within 2.5 m) and not it, at least 3 times, before or
        after those times.
      - One the game never drew, while it drew its neighbours, isn't drawn.
    - Log when recording stops: "N models the game showed only part of the time (drawn only then) and N it never
      showed".
    - Saved with the recording (a trailer after the models; older files have none, older builds skip it).
  - **People's bones:** the 15:53 test changed up to 48 of a part's 59 bones in a second, back and forth, on
    KrugerSec standing still. Their pose check was exact, so these were bones in the same place (a limb's and its
    twist bone's). A bone now changes only to one that fits better by 2 mm a frame on the whole.
  - **Hoodie (cloth):** still no cloth line in the 15:53 test: no shader of the game's has a resource named for
    cloth. The draw log now also describes draws whose vertex data comes from a buffer the graphics card writes (as
    the cloth's compute shaders do): which buffers (size, stride, bind flags) and their state, 8 lines a recording.
    Each vertex shader is checked once.
  - **Ghost runner:** the Release mission's red projection runner (SpawnTemplate_Projection_Replay) was recorded
    only at 89.7 - 92.5 s. Its mesh, female_runner_projection_01_fullbody, is drawn by the game only in its lit pass,
    so it's drawn now because people's lit pass draws are kept (since CF50CD909EA6): 1 part, its bones matched
    within 1.5 cm, 70 of 77 fitting more than one skeleton bone. What's wrong with it on screen isn't known from the
    log; not changed apart from the bones' margin above.

- **GAME tab tidied** (the user's list, 2026-11-01).
  - *Game speed*: no (?).
  - *Time of day*:
    - No (?) by the slider or by *Stop the clock*.
    - *Light catches up with time jumps* has a short help.
    - *Stuck clock fix* is gone from the menu. It's always on: the clock stuck at 65700 s (18:15) is moved 15 s
      ahead, as before by default.
  - *Faith*:
    - *Outfit* has no (?) and no line of text under it.
    - The list is *Runner* (what the game has her in; was "Free roam"), *Prologue* (the civilian clothes, no
      gridlink; was "Release, no gridlink") and *Prologue with gridlink* (was "Release"). The outfits are the same.
  - *Detail*:
    - *Detail distance* is gone (Fix LOD does the same). It's no longer read or saved (`[Misc] LodScale`), so it's
      the game's own.
    - More room between its groups of settings.
    - *Billboard quality*'s help just says what each choice gives.
    - *Unbroken Shard and Bauble*, *Open the lair's blinds* and *Close the lair's open windows* have no (?).
    - *Kruger's office blinds* is now *Shard office blinds*, at the end of Detail.
  - *Loading the world* is now *Level streaming*.
    - *Unrestricted* is gone: always off (`[Misc] StreamUnrestricted` no longer read).
    - *Hold back from* is gone: always 80 % (`[Misc] StreamBatchLimit` no longer read). That's 2 points above the
      slider's old top of 78 %. Taking zones away still starts at 86 % and taking them all at 92 %, the same
      caps as at 78 %. After the draw batch has run past its room once, 70 % is used, as before.
    - *Only where the camera looks*, *Load ahead of the camera* and *Keep the track's path loaded* have
      one-line helps.
    - *The real Shard, from anywhere* (in Districts) has no (?).
  - Build 842971cf27d1, installed (game folder and dist, hash checked). Built without errors; not checked in game
    yet.

- **Wet tyre tracks after a puddle; the crowd's and the traffic's marks only at their own height** (the user,
  2026-11-01: "Yes, add them", to wet tyre tracks after a car drives through a puddle). Build C7D079303622, installed
  2026-11-01 20:06 by the install-at-close job (game folder + dist, hash checked). **Not tested in game.**
  - **Wet tyre tracks:** a car's back wheel that drives through a puddle carries its water on. Every 0.5 m it's looked
    for a puddle under it, by the same test the wet look draws puddles with. Out of one, it leaves a darker, wetter
    track, wearing off over 12 m. The track dries over 30 s.
    - As wet as the puddles: the puddles dry over 20 minutes and the ground over 10 (Weather.cpp), so after the rain
      the tracks show across a drier road. In a downpour they're no wetter than the road. They fade into the fog and
      at the wet look's fade distance as the wet ground does.
    - All the traffic as cars still (motorcycles can't be told apart yet).
  - **Marks only at their own height (fix):** since 66FD0C267403 a mark showed at any height over or under where it
    was made (a step under an awning on the awning's top, a car's track on a bridge over it). The map now keeps the
    ground's height with each mark (1.8 cm steps over 1200 m round the camera's height). A mark shows only within
    0.4 - 0.8 m of it. Only where a stamp leaves a mark is the height set (the weather session's review).
  - The map is 82 m across at 4 cm now (92 m at 3 cm before): the height takes room.
  - Known: a person stepping on a bridge right over a fresh track still moves that sole-sized bit of the track up to
    the bridge.

- **Replays: people's bones told apart over every frame, still props need their own draw, a cloth draw log that
  can't run out, and the videos' streaming in the log** (after the 19:04 test of CF50CD909EA6). Build 5EE8947A05CD,
  staged to install when the game closes (game folder + dist). **Not tested in game.**
  - **What the 19:04 test showed:**
    - The video channels were found: all 64 of the billboard manager's frame textures are views, and kept screen
      draws matched channels 1, 2, 3, 6, 8 and 15 (commercial_advert_13, billboard_01, world screens).
    - Icarus was drawn with 16 of his 16 kept draws (12 before: the lit pass's parts are kept now).
    - The pose check: for every character checked, Icarus too (at 27 s and 44 s), the replay's world pose matched the
      game's own on every bone (none off by 5 mm), and the game's matrices were its world pose times the fixed
      transforms. So the replay's skeleton isn't what's wrong with the mouth.
    - Many skeleton ties: 38 - 43 bones per skeleton fit two parents in the first samples (most between bones 44 and
      46, which never came apart: either gives the same pose).
    - No cloth draw was logged: the draw log's 120 lines (24 a pass) were used up in the recording's first seconds,
      and Icarus came at 24 s.
    - The gate's sign planes counted as drawn by the game: the bus sign plane of the same prefab was drawn within 2 m.
  - **People's bones (the mouth):** a kept draw's bones were mapped to the skeleton bone nearest them in the one
    frame its set was kept from. Icarus's set was replaced at 79 s by a "more detailed" one from a frame matched only
    within 3.1 cm (the one before: 0.1 cm), and face bones are a centimetre or two apart. Now each part keeps, per
    bone, the skeleton bones that fitted it, sums how far each is in every later frame of that part, and is drawn
    from the nearest over all of them. A more detailed set takes over those sums. The log says how many bones fit more
    than one, and each time a part's bones change ("checked against N frames of it").
  - **Still props:** a model that didn't move needs its own kept draw within 30 cm (2 m before) to be drawn with it,
    or to count as drawn by the game. Moving ones (doors) keep 2 m.
  - **Cloth:** cloth draws have their own 8 lines in the draw log, on any context. A shader made with a resource
    named for cloth is in the log when it's made, so if no cloth draw shows, the log tells whether such a shader
    exists at all.
  - **Videos:** every 20 s while the replay streams channels for its screens: "Billboards: N video channels sized for
    the replay's screens (channel: the game's size -> the replay's)".

- **The crowd's and the traffic's marks: water trails through puddles, footprints and tyre tracks in the snow; the
  crowd no longer dropped from the wet look's list** (the user, 2026-11-01: "cars/motorbikes should leave behind water
  trails if on puddles and pedestrians should have the pudde ripple effect when they walk if it's possible make no
  mistakes", "Do the same about snow too"). Build 66FD0C267403, installed 2026-11-01 19:31 with the game closed (game folder + dist, hash checked; it holds 5EE8947A05CD). **Not tested in game.**
  - **What there was:** the crowd's and the citizens' strides already rang the puddles they stepped in (a ring each, at
    most 24 at once, 1.6 s), and the road traffic's back wheels did every 1.2 m: no trails. In the snow only the
    KrugerSec left prints (48 at once).
  - **New: a map of the ground's marks round the camera** (Visuals/GroundMarks.cpp, GroundMarks.hlsl): 92 m across at
    3 cm, wrapping round as the camera moves. Steps (a 27 x 10 cm sole, the way they walk) and each back wheel's path
    from frame to frame (a 22 cm tyre) are drawn into it on the graphics card.
    - **Rain:** where they go through puddles and wet ground, the water is pushed aside the way Faith's slide pushes it
      (less mirror, a lighter film), coming back over 4 s. The rings are as before.
    - **Snow:** footprints and tyre tracks pressed in as her prints are (floor and wall), filled by the falling snow
      at her prints' rate (5 minutes at intensity 1, faster in heavier snow; they stay while it doesn't snow).
    - One lookup a pixel. Faded out on a circle near the map's edge (about 36 - 45 m), and where a pixel covers more
      than 8 - 20 cm of the ground. The map is cleared when the weather comes back on.
  - **The crowd dropped:** where the wet look's scan found over 2048 things nearby, its list was full and the people and
    the traffic were left out ("0 people, 0 road traffic ... the scan's list was full"): no rings, no dry boxes. Its
    list now doubles, up to 16384, after a scan that was full. Its own list only: the camera's, the replay's and the
    others' aren't changed.
  - Each person's stride and each car's wheels now carry over the scans: their steps stay in step, their tracks
    unbroken.
  - **Motorcycles:** the road traffic is one swarm (cars, trucks, motorcycles), and which member is which isn't known on
    the processor, so all of it leaves two car tracks (the user's choice). A diagnostic logs the first 8 traffic
    members' records and their swarm's component once, to find it ("Ground marks (diagnostic): road traffic member").
  - **Snow on walls striped and flickering, fixed** ("snow looks weird on walls/edges and it's flickering make no
    mistakes and fix", with a screenshot of dark and light stripes along a parapet's foot). The snow piled against a
    wall's foot took its light, since D9483CC3053B, from the game's light at the foot's own pixel: the line where the
    wall meets the ground, both in one pixel, moved by the game's jitter each frame. So column by column and frame by
    frame it went from dark to bright. Now it's lit as the ground's snow just in front of it: the game's light 8 px
    further out on the ground, 3 taps 6 px apart, averaged. Where any of them can't be read, it uses the smooth light
    it had before.

- **Replays: wall videos follow their channel, the gate's sign planes no longer drawn with another sign's picture,
  glasses lenses kept; the cloth jacket and the face bones looked into** (the user, 2026-11-01: "doors still look the
  same with the solid texture", "Icarus still has no hoodie / glasses are transparent when they shouldn't", "Videos that
  play on the walls (not billboards) are frozen", "Do a decompile on cloth-simulated mesh and facial bones"). Build
  CF50CD909EA6, installed 18:51 on 2026-11-01 (game closed; game folder + dist, hash checked; built from the shared
  folder, so it holds the snow session's FCCC1B7C574B). **Not tested in game.**
  - **The gate panes ("the facility 7 texture with the foodome texture together"): the cause is in the game's data.**
    - The panes are two sign planes of the gate prefab (pf_releasegateglass: plane_1-1_01 with the variation
      plane_1-1_01_gatesign_01). Their own texture, t_gatesign_02_d (decoded), is colour masks: arrows, an outline
      glow, a logo. It has no "7" and no text.
    - They're switched off by default: the prefab's BoolEntity drives their Enabled, DefaultValue False, and only the
      gate's own events switch them on. So the game never drew them while you recorded.
    - The replay still drew a copy of each (the game doesn't draw them now either). plane_1-1_01 is one plane mesh
      shared by many signs, and the only look kept for it was the Facility 07 sign from another part of the city
      (plane_1-1_01_facility07, central city props zone 293). The replay used that look for both panes.
    - **Now** a model that none of its mesh's kept draws (in any pass) has within 2 m isn't drawn with another
      object's look; its plain shape isn't drawn either. The log says which: "a model of it the game never drew while
      it recorded ...".
    - Not done: the signs' own on/off while you recorded isn't recorded. If the gate switches them on during a
      recording, they'll be drawn for the whole playback.
  - **Videos on the walls frozen: the replay drew its kept copy of the screen with one frame.**
    - The replay keeps the screens it draws itself (e.g. cornerbuilding_01_lms: drawn by the replay for 112 s of the
      last playback, the game drew it at 10 of those seconds) with the texture their video showed then (the shader's
      g_lmsAlbedoTexture).
    - A video's frames are a ring of textures (decompiled: feed + 0x270, the frame now at + 0x248). Once the screen
      isn't in the game's world, its video isn't streamed at all and shows its still picture.
    - **Now** each frame the billboard manager's table of its 32 channels' textures (0x1437DF8D0) is read in the hook
      the tools already had (hBillboardSizes). A texture's view is at + 0xB0 (the game's Dx11Texture, m_shaderView);
      it's only taken when it is a D3D view like the game's others.
    - A kept draw whose video slot held one of a channel's recent frames is drawn with that channel's frame of the
      moment.
    - Where the replay draws such a screen is handed back, and that channel is sized as if a 100 m² screen were there
      (at least the game's lowest streamed level, at most level 1), so it streams.
    - The log names the draws matched to a channel ("shows video channel N's frames"), and the ones whose texture
      wasn't any channel's.
    - **Saved recordings:** format version 4 keeps each draw's channel. Recordings saved before have no channel, so
      their screens still show the kept frame. An older build can't load a version 4 file.
  - **Glasses lenses see-through:** Icarus's lenses are drawn in the game's lit (forward) pass, and the replay kept
    people's draws from the G-buffer pass only. Now people's draws in the lit pass are kept and matched to them too,
    and drawn in that pass (as the game drew them, its blending). Guns', drones' and doors' are still the G-buffer
    pass's only.
  - **Mouth (face bones): a likely cause found and fixed; a check added.**
    - The replay rebuilds a person's pose from its recorded local pose, each bone under its parent. The parents are
      worked out from the first 30 samples of that skeleton (every body of it counts, so it can be a few frames).
    - A tie went to the lowest bone number. A lip bone whose jaw sits at its head's pivot, mouth shut for those
      frames, fits the head as well as the jaw, so it could hang from the head and not follow the jaw.
    - **Now** tied bones keep all their candidates. Each later frame drops one that fails 3 times while another holds,
      and the last one left is the parent. The log lists the ties at the start and each one settled ("bone N hangs
      from bone M").
    - **Check (log):** 3 s and 20 s into each person's recording, the pose the replay makes against the game's own world
      pose, every bone ("... N of its M bones off by more than 5 mm ..."), and the game's matrices against its world
      pose times the fixed transforms. If the face bones are off there, the cause is something else and the line names
      them.
  - **Hoodie (cloth jacket): not fixed yet; what's known.**
    - Icarus's jacket is icarus_jacket_cloth (a mesh plus a cloth asset). It isn't in the game's mesh table at all:
      the asset log never saw it.
    - The game's code names it a "SkinnedClothPrimitive" ("clothPrimDraw"). Compute shaders make the cloth's points'
      transforms and wrap the render mesh to them (ComputeClothPointsTransformsCS, ClothWrapCS).
    - No skinned track of an unnamed mesh was kept, so its draw doesn't read bones the way people's do.
    - **Now** the draw log also describes cloth draws (a vertex shader whose resources are named for cloth): their
      buffers, views and constants, and a snapshot of their data while recording. That tells how to keep and play it.

- **Snow on the ground stronger: whole, whiter, and fogged instead of faded; "How much of the day it snows" (and
  rains) now true** (the user, 2026-11-01 18:0x: "make the snow on the ground effect stronger no mistakes", "Also check
  if how much of the day it snows actually works"). Build FCCC1B7C574B, staged to install 150 s after the game closes
  (only over CCC31B7A6DE1, which it holds). **Not tested in game.**
  - **"Snow on the ground" now 1.0 by default (was 0.85).** At 0.85 the snow stayed patchy even once settled: its
    patches were down to half see-through, until about 8 cm had piled (over 10 minutes). Now the settled ground is
    covered all over, and it piles to 12 cm (10 before).
  - **"Snow brightness" now 2.5 by default (was 1.75).** Still rolled off below white, so its shading shows. For a
    mid-grey ground's light that's about 0.86 of white (0.73 at 1.75).
  - **In the fog:** the snow was faded out by the fog's transmittance, so the bare ground showed through it. In a
    blizzard's 120 m, snow 30 m off was half the ground's. Now it stays whole and is fogged towards the sky's own colour
    as the frame shows it (the haze's sky colour, standing in for the fog's). The snowy look's thick fog is that
    colour; in clear weather the fog is thin and far snow is only tinted a little. Until that colour is known (the
    first moments), it's faded as before. The pile's side the same way.
  - The sliders are as before: lower them for the old look.
  - **"How much of the day it snows" didn't work as set** (Random snow; Random rain's "How much of the day it rains"
    is the same code). The gaps between spells were drawn on their own, so the setting held only on average over many
    days, while one day's forecast repeats every day:
    - 0.1, 0.2 and 0.3 all gave the same 33 % of the day;
    - 0.05 gave 25 % and 0.9 gave 74 %;
    - the hours before the first spell (up to 6) were always dry.
    Now the spells add up to exactly the set share (each 40 - 150 game minutes), with the dry time between them
    shared out at random from a random time of day. Checked offline with the same code over 5 seeds and 7 settings:
    within 0.03 %. The spells still come from a seed ("New forecast" makes another), and their own weather (rain,
    look, fog, storms) is drawn as before.
    - It follows the game's clock (about 20 game seconds a real second: a game day is about 72 real minutes, a
      spell 2 - 7.5). With the clock stopped (VISUALS' time of day stopped, or a mission stopping it), the
      forecast stays at that hour.

- **Replays: the gate's glass no longer drawn as a solid print; a log of how people, guns and drones are posed** (the
  user, 2026-11-01: "Gates still have the weird textures", "it's the facility 7 texture with the foodome texture
  together"; "Character(s) bones don't seem correct now", "ksec ... almost indivisible ... could still see
  hands/baton", "hunter drone was invisible"). Build CCC31B7A6DE1, installed when the game closed after 17:00 on 2026-11-01
  (an install-at-close job; game folder + dist, hash checked). **Not tested in game.**
  - **The gate, checked first:** the gate's own textures (decoded from the game's files: t_releasegatedoor_01_d,
    t_releasegate_01_e, the glass's roughness) have no "7" or text.
  - **The cause:** the door's glass panes (releasegatedoor_01 section 1) are drawn by the game only in a lit picture's
    pass, transparent, with a printed texture in t3 (BC7 sRGB) over clear glass. The replay's copy of the mesh took
    that as the panes' colour texture. Whenever their kept lit-pass draw wasn't drawn (matched by its number among the
    frame's lit passes), the copy drew the panes opaque, with the print solid over each pane.
  - **Now:** the replay notes which passes the game draws each section of a recorded mesh in (immediate context
    only). A section the game draws only in its decal or transparent passes is never drawn as the opaque copy. The
    note is saved with a recording (older files read as before).
  - **People, guns, drones (diagnostic):** while it plays, a few lines each in CT.log ("drawn with the game's kept
    draws") give how many of its draws are drawn, the bone palettes against its skeleton, the first bone's matrix and
    where the recording has it. This is to find why some looked wrong or invisible: the recording kept draws for all
    15 people and 14 guns and drones in the 16:28 run, so the fault is in how they're drawn back, and the log alone
    doesn't say which.

- **Replays: no doubled glass on the game's own mission models** (the user, 2026-11-01: "Yes, fix it", after the 16:01
  test). Build 57A4132EC9AE, installed 2026-11-01 16:20 with the game closed (game folder + dist, hash checked). **Tested in the
  16:28 run: the gate glass's copy was let go once the game drew its own (22.1 s played).**
  - **What the 16:01 test showed:** with the mission's world streamed in by the game, its gate glass
    (csg_releasegateglass_01) was "in the level, but the game doesn't draw it where it was" all through the playback. So
    the replay drew its recorded copy over the game's own glass.
  - **Why:** the game draws that glass only in a lit picture's pass. The check whether the game draws a recorded model
    where it was (GameDrawsModel) only noted the game's draws, and read back their places, in its G-buffer pass.
  - **Now:** the game's draws of the recorded models' meshes are noted in its decal, lit and half-size passes too, and
    their places read back there (on their own, throttled apart). A model drawn where it was in any of those passes
    is the game's, and its copy is let go.
  - The world's own kept draws still go by the G-buffer pass's, as before.

- **Replays: the mission hooks go in again** (the user's 15:52 test of 12B5395D573E, "Done, read logs"). Build 8FA3AA8FB98E, installed
  when the game closed after 15:57 on 2026-11-01 (an install-at-close job; game folder + dist, hash checked). **Not tested in
  game.**
  - In 12B5395D573E the log said "the game's mission entity code isn't what the tools know": the code check
    (ReplayAssetLog's CodeIs) compared at most 16 bytes. The new sub-level checks are 17 and 20, so it refused them, and
    with them all 7 mission hooks. The mission log, the mission's world tick and its guards did nothing.
  - It now compares up to 32 bytes. The patterns themselves are unchanged (read from the decrypted game image).
  - What that test did show: the dialog track keeps 76.6 s of 123.2 s (25 narrative lines, 2 cutscene voice-overs; 42
    other lines left to be said live), 284 crowd members drawn, recording at 137 - 178 frames a second.

- **Replays: the mission's world from the game, its two unknowns decompiled and guarded** (the user, 2026-11-01:
  "find these as well high priority no mistakes": whether the game streams the world straight back out, and what the
  mission's logic does while the mission isn't running). Build 12B5395D573E, installed when the game closed after 15:1x on 2026-11-01 (an install-at-close job; game folder + dist, hash checked); built from the shared folder, holds 69EF97F31328. **Not tested in game.**
  Research: Research_Mission_Lifecycle_2026-11-01.md section 8.
  - **Release's world isn't streamed back out by the game** while no mission runs (all its stream-out paths decompiled).
    Its only checks of where Faith is are two links (a StreamingSphere, other areas) that Release doesn't have; they
    answer "no area" then. With another mission running, the state update streams it out.
  - **What its logic does when it's in:**
    - its area's ambience starts by itself: rain sounds and sound areas, a rain mixer, mist, rain and pigeon effects,
      one debris effect, 4 video billboards, its lighting environments near their areas;
    - no AI spawns and no timelines play by themselves; nothing writes a progression flag without an event;
    - Release's "Start" comes only from the "Game_Start" and "Release:_Mission_Start" start points;
    - its 80 enabled area triggers fire for the player. They start KrugerSec, voice-over, input restrictions, a VTOL
      and progression flags (the save). The replay leaves the real Faith where she stands, so they fire only if she's
      in their area (x -68 - 140, z 462 - 1213; the lair is about 170 m outside).
  - **Guards:**
    - the world is asked for only while no mission runs, the real Faith's place is known, she's 60 m beyond Release's
      trigger area and 150 m from where the recording went;
    - it's streamed out again during the playback if she comes within 30 m / 100 m, or a mission starts;
    - each refusal is in the log.

- **Replays: the mission's world from the game (a test tick), the mission log fixed, the dialog track only the mission's
  dialog** (the user, 2026-11-01: "Done, read logs", then "Try again"). Build 69EF97F31328, installed 2026-11-01 14:5x with the game closed (game folder + dist, hash checked; over 420D15286E67, built from the shared folder). **Not tested in game.**
  Research: Research_Mission_Lifecycle_2026-11-01.md section 7.
  - **What the 04:33 test showed:**
    - the game calls its mission code on its server update thread, the one FaithOutfit's Server::update hook already runs
      on, not on the replay's thread;
    - the mission log ran out at 400 lines while the level loaded (121 missions), so the abort wasn't in it;
    - 0x143A74150, logged as "streamed out", is not a stream out: it decides whether the mission keeps being updated;
    - the dialog track kept 148.2 s of 149.7 s: KrugerSec's distant chatter, their barks and the prison's loudspeakers
      count as voice lines too.
  - **Decompiled (offline):** the mission entity links two ServerSubLevelEntity objects, "SubLevel" (+ 0xD0, the
    mission's world) and "PreMissionSubLevel" (+ 0xD8), named from their hashes. 0x143A7A0A0(entity, in) sends the
    SubLevel the event "StreamIn" or "StreamOut"; the sub-level entity queues it and streams the sub-level in or out.
  - **REPLAY > "The mission's world from the game (test)"** (off by default, config MissionWorld):
    - the missions running while it records (+ 0x11F set) are noted by name and saved with a recording (section MISN);
    - when such a recording plays, the server update asks that mission's entity once to stream its SubLevel in
      (0x143A7A0A0(entity, true)), as the mission's own update does when Faith comes near an available mission;
    - when the playback ends, it's streamed out again (0x143A7A0A0(entity, false), as an abort does), unless the mission
      is running then.
  - **The mission log:**
    - 60 lines per mission, 6000 in all;
    - 0x143A74150 not hooked any more;
    - the SubLevel and PreMissionSubLevel stream calls are logged (with their entities and class names);
    - each mission's two sub-level entities are in its state lines.
  - **The dialog track:** only a mission's narrative lines (ME_Sound/VO/VO_Narrative, VO_Narrative_Faith) and a
    cutscene's voice-over (sounds under /VO/ given to a voice no voice line used, not the loudspeakers') are kept. The
    others are still said live by the replay. The counts are in the log.

- **No rain dripping indoors from edges** (the user, 2026-11-01: "I can still see some rain dripping indoor from edges
  check out what it is and fix it"). Build 420D15286E67, installed (game folder and dist). **Not checked in game.**
  - **The roofs' drips** (the tools' drops off roof edges, where a roofed 2 m square meets an open one) fell from the
    two squares' shared side, which can be up to a square in from the roof's real edge: under the roof, inside the
    building, by its openings and edges. They now fall just past where the roof really ends. That's measured by the
    roof edges' own looks out from each square's middle, the same that shape the boxes keeping the game's rain out.
    Where that isn't known (beyond about 24 m, or no edge within the square), no drip is made rather than a wrong one.
  - **A ceiling with something hanging under it** (a duct, a pipe, a beam, a lamp less than 2 m over the floor)
    counted as open sky since today's "something standing there isn't a roof" rule. The room's squares were open,
    their edges dripped and the game's rain wasn't kept out. Now when such a low cover has something over it (a
    ceiling, a roof, a bridge) it's covered by that. A car's or a bench's top under the open sky still isn't a roof.

- **No decal patches in the wet look; less rain inside buildings** (the user, 2026-11-01: a screenshot of overlapping
  rectangles on a wet floor, "some meshes have this weird effect with wetness smooth it out and fix it"; "You can
  still see some rain inside buildings"). Build 971EF0400FD6, installed (game folder and dist, 04:21). **Not checked in game.**
  - **The rectangles:** the wet look's shading took the game's own surface facing (for the triangles on rounded
    models), and that includes its decals' and textures' bumps, so a grime decal on a floor showed as a rectangle
    lighter or darker than the floor round it. The game's facing is now taken only where the surface leans (where the
    triangles showed). On level ground it's the depth's again, as before today.
  - **Rain inside buildings, two causes from today's changes:**
    - The roof search's new "something standing there isn't a roof" rule (a roof less than 2 m over the floor under
      it) also caught a room's ceiling over a table, a counter or a bed, whose top the downward look found first. The
      room counted as open and the rain wasn't kept out. Now when that happens the next surface under it is looked
      for, and a ceiling 2 m or more over that floor is a roof again. A car's or a bench's top over the road still
      isn't.
    - The rain the tools draw in the distance went on past the weather's squares (to remove their square edge), where
      it can't be told whether a point is in a building, so drops showed in rooms seen 50 - 60 m off. It now fades out
      on a circle 4 - 14 m short of the squares' edge, so there's no square edge either.
  - Rain can still show under roofs far out: the game keeps rain out of at most 64 of the tools' boxes, 32 per rain
    emitter (CT.log's "more roofs than boxes"). If it's near the camera, a screenshot will tell.

- **The rain's square round the camera, softened** (the user, 2026-11-01: "I can still see the square from the rain
  flickering", after the screenshot from up high). The game's own rain streaks live in boxes round the camera, each a
  few metres across (the densest 7 m, 10 m ahead along the view), aligned with the world, their faces hard (the game's
  data fades them out over 0 m across). From up high a box's outline showed as a square, lighter inside where its
  streaks are, flickering as they wrap at its faces. While the tools' rain is on, those boxes now fade over a quarter
  of their size at their faces; the game's values come back when the rain stops. CT.log says how many were softened
  ("streaks' camera boxes softened"). Not known yet: whether the game reads the fade while it plays or only when the
  rain's emitters are made. If the square still shows, say so. (The previous change, past the weather's squares counted
  as open, stays: it took away another, larger edge.) Build 33768C4B71FB, installed (game folder and dist). **Not
  checked in game.**

- **The lair's curtains hang still with its windows shut** (the user, 2026-11-01: "When 'Close the lair's open windows'
  is enabled the curtains on the lair shouldn't wave like there is air"). The curtain by the open window is cloth that
  the game's own wind moves (its ClothEntityData's WindScale 0.01; nothing else in the lair, pf_dt_trainstation, makes
  wind). With the camera in the lair and the tick on, the tools already took their rain wind and its sound away there,
  but the game's own wind went on. Now the game's wind strength and turbulence are 0 there, rain or not, after each mix
  of the view. The game's own comes back once the camera is out, or the rain's wind if it rains. From outside the lair
  (the camera not in it) the wind is as outside. Build C48481BCDE6C, staged: it installs itself once the game closes.
  **Not checked in game.**

- **No square round the camera in the rain** (the user, 2026-11-01, a screenshot from up high: "this is the square
  ... is it caused by the wetness but can we make it seamless so there is not a square there?"). The weather looks
  for roofs in 2 m squares, 51 x 51 of them round the camera (about 100 m across). Past them it can't tell, and the
  rain the tools draw in the distance and the snow's flakes counted that as not open, so they stopped at the squares'
  edge. The puddles also blended to "not open" over the last square. From up high that showed as a square round the
  camera, lighter inside where the drops were. Past the squares now counts as open, as the puddles and the snow on
  the ground already had it there, so there's no edge. A square not looked at yet still counts as not open. Build
  6908CCC93809, installed (game folder and dist). **Not checked in game.**

- **Rain: cheaper at 4K, smooth on models, no wet look over outlines or blue rim round Faith, no dry box round parked
  cars, a slight turbulence** (the user, 2026-11-01: "Edge detection doesn't work" (the wet look over outlines), "When
  faith runs there is a weird blue hue around her body", "Rain shader doesn't apply smoothly (you can see the triangles
  of the mesh) on some meshes", "Improve the performance of the Rain it tanks the fps from 130 to 70 at 4k", "Cars have
  a weird square around them with rain", "add a slight rain turbulance"). Build 178E7B8AABD0, installed (game folder and dist), with the snow session's
  37F2E68C49AC work. **Not checked in game.**
  - **Performance:** the user's 03:03 performance test measured the wet look's pass at 3.3 - 6.0 ms of GPU time a
    frame at 3840 x 2054.
    - The kept-dry outline softening and the slope test no longer look round pixels where their answer is already
      none (the same result).
    - Rough wet ground's reflection, a blur, is marched in at most 20 steps (it was 48).
    - Reflections under 2.5 - 5 % of the colour are faded out and not traced.
    - In the rain the game's G-buffer is copied (2 of its targets) for the next item, which costs a little.
    - What it saves isn't measured: run MISC > Performance again to compare.
  - **Triangles:** how the wet look shades a surface (its darkening, its mirror's way and share, the lightning) now
    takes the game's own per-pixel facing (its G-buffer), smooth over a model, instead of the depth's, which is flat
    over each triangle. Where puddles, slopes and prints go is still told by the depth. Its normal maps' bumps show a
    little in the mirror.
  - **Outlines:** the ground's or a wall's pixels within 3 - 8 pixels of something in front (its outline, softened
    by the game's antialiasing and smeared by its motion blur) no longer get the puddle mirror and sheen, and get
    less of the darkening.
  - **The blue round Faith:** her wet look kept her pixels' colours deeper and cubed them for gloss. On her outline,
    which shows some of the sky behind her (more as she runs), that made a blue rim. Now her colours are kept as they
    are and her gloss goes by brightness (the same on grey).
  - **Parked cars:** a vehicle model's box went 25 cm under its wheels, so the road under it and round it was kept
    dry, a rectangle round the car. Vehicles and trains now stand on the ground as Faith and people do: only what's
    4 - 8 cm or more over the ground is kept. Moving traffic wasn't kept at all in the user's last session (the scan's
    list was full).
  - **The square at ground level with the free camera, and round cars** (the user, 2026-11-01: "in the ground level
    you can see a square while using the freecam"): the weather's squares look for a roof straight up from the
    camera's height, so with the camera low anything taller than it (a car, a van, a bench, a planter) was a roof.
    Its squares were covered: dry, no puddles, the game's rain kept out of them. A roof less than 2 m over the floor
    found under the same place now counts as none (a real roof, an awning, a balcony or a low ceiling is higher).
  - **Turbulence:** VISUALS > Weather > Wind with the rain > Turbulence (0 - 1, 0.35 by default): the game's wind with
    the rain gusts round its strength (three waves 4.7, 1.9 and 0.83 s long) and its turbulence is a little higher.
    The game's rain streaks blow with its wind (their emitters' WorldWindData), as cloth, foliage and smoke do.

- **Snow brightness slider (VISUALS > Snow, under "Snow on the ground"), brighter by default; Faith's shoes not blue on snow** (the user, 2026-11-01:
  "Snow looks good now but it's not as apparent of an effect now can we increase it?"). Build 37F2E68C49AC, installed
  2026-11-01 03:33 at game close + 150 s (game folder + dist, hash checked; it holds all of 251D559BE528). **Not tested in game.**
  - **The first-time lag, corrected:** CT.log (02:42 - 02:45) showed the shaders made in 10 ms on the worker thread,
    then one frame of 154 ms of the wet look's work in the first 10 s of snow: the driver turns a shader into its own
    code at its first draw, not when it's made. So D9483CC3053B's worker thread alone saved little. Now each shader is
    also drawn once, a pixel, in the game's first frames (its logos or menu), with the constants all 0. CT.log says
    "its N shader pairs drawn once ... in N ms".
  - The new light gave back what fresh snow does, 0.85 of the light reaching the ground. That's physically right but
    dimmer than before: a mid-grey ground's snow went from about 0.86 of white to about 0.57. The old light was the
    ground's picture x 1.9, about 4 times in linear light.
  - "Snow brightness" multiplies the snow's light (and the sky light used where the ground's can't be told). 1 is
    fresh snow's own, 1.75 the default, up to 4. It's still rolled off below white, so its shading shows.
  - The glints were dimmer with the new light (they took part of their light from the old ground light, no longer
    worked out): they use the game's light now.
  - Without the G-buffer copy, the old light is scaled by the slider against its default, so the default leaves it
    as it was.
  - **Faith's shoes turned blue on snow** ("Faiths shoes turn blue on snow fix it", with a screenshot: cyan snow on
    her red shoe tops). Two causes:
    - The snow's light took its colour channel by channel, as the picture over the surface's own colour. On a
      strongly coloured surface (red shoes, red brick, green paint), a channel it gives back little of was divided
      by little, so the sky mirrored in it came out the opposite colour. Now the light's brightness is taken over
      all channels, and its colour only where the surface's own is near grey.
    - Her shoes were kept clear of snow only within 0.43 m of her transform's place, so with her legs out ahead
      (sliding, sitting, looking down at them) the snow lay on them. Now each shoe is also kept by her own ankle and
      toe-base bones (read with her body each frame). It's sized from her shoe's mesh: within 10 cm of the line from
      3 cm behind the ankle to 6.5 cm past the toe base, over its sole. So the ground's snow round her shoes stays.

- **The whole tool read for bugs: four ways it could end the game, a thread race and a wrong menu line fixed, two
  things made faster** (the user, 2026-11-01: "After all the sessions end read all the code and of the tool find any
  bugs and improve performance implement them and make sure nothing breaks and make no mistakes"). Build
  251D559BE528, installed 2026-11-01 02:32 with the game closed (game folder + dist, hash checked; it holds everything
  in D9483CC3053B, which it replaces). **Not tested in game.** Every source file was read (Camera, Visuals, Lights,
  Util, Input, UI, Main; the generated tables checked by script).
  - **Fixed: the game could end** (an over-long `sprintf_s` ends the process: the tools set no handler for it):
    - **The hang watch (Main.cpp)** counted -1 (the quit flag's pointer not readable) as a quit, and a quit 30 s old
      ends the game with exit code 42: if that pointer were ever unset for 30 s, the game would have been ended. Only
      1 (asked) and 2 (quitting) count now. It was in every build installed since c2c1fb6460bf (01:59).
    - **A replayed person's log line (FaithReplay.cpp, StartDrawing):** while a recording plays, the replay posing a
      second body of a skeleton with bones without a parent wrote 113 characters into a 96-character buffer. Now 160.
    - **The draw-inputs diagnostic (ReplayGameDraws.cpp, LogDrawInputs):** a vertex shader with 7 or more buffer
      views went past its 700-character line, on one of the first "not instanced" world draws while recording. It's
      now cut short where it's full.
    - **Deleting a saved recording (ReplayStore.cpp):** a folder renamed by hand to more than about 140 characters
      could go past the 300-character status line. It's now cut short.
  - **Fixed, other:**
    - **A thread race (ReplayShapes.cpp, NotePastPass):** its list of shapes logged was changed outside its lock. The
      draw hooks run on the game's worker threads too (deferred contexts), so two at once could corrupt it. It's
      under the lock now.
    - **REPLAY > the dialog's status line** said the game's own sound was muted while the track played, which it isn't
      any more. It now says the track plays for the dialog the game can't say live.
  - **Faster:**
    - **Playing a recording (ReplayGameDraws.cpp, DrawRecipes):** the frame's list of models was copied at the end
      of every kept pass, up to 1 + 3 x 24 a frame. It's now copied only for a pass with recipes to draw. Same result.
    - **The dialog's sound track (Hooks.cpp, ReplayDialogFromTrack):** each frame it searched every recorded sound's
      name twice (up to 16,384) before looking at its time, under the replay sounds' lock that the sound thread's
      hooks wait on. The time is checked first now. Same result.
  - **Checked, nothing wrong found:**
    - the lock order between the menu, the weather, the wet look, the snow bodies, the dry shapes, the images and
      the green screen;
    - the wet look's constants against all 6 shader headers (97 fields, 46,688 bytes, all match);
    - the snow meshes' indices and bones (all in range);
    - every other fixed-size text buffer's longest line;
    - the other sessions' 2026-11-01 changes (dialog-only track, packed parts, staging pool, reflection passes, the
      G-buffer copies and depth targets, the body hook, the hang watch).
  - **Found, left alone:**
    - **The green screen's old way of finding the scene's depth** (used while the wet look and snow are off: the
      G-buffer pass isn't watched then) never takes a smaller depth target than the one it holds. After a drop in
      resolution, the green screen, images and replay shapes would keep the old one. It can't tell that apart from a
      pause or loading screen where the scene isn't drawn, and it can't be tried here.
    - The hotkey window's key name is written by the window thread while the menu reads it (only while a key is
      being caught).
    - The replay's sound hooks go through all 1,024 voice slots for each sound event while a replay records or plays
      (bounding it needs a new count).
    - Two `QueryPerformanceCounter` calls per game draw while the replay's draw hooks are in: the replay's own cost
      measurement.
    - If MECMP loaded at the very moment the replay turns its body hook on, the hook could stay on (the hang watch's
      thread and the replay's both switch it).
    - Labels with a hotkey's name in 128 characters: Windows' key names are far shorter.

- **Snow: lit by the game's own surfaces, its own facing on models, no lag the first time, the scene's depth from the
  G-buffer pass** (the user, 2026-11-01: "Drone takedowns remove the snow shader", "snow looks blurry if not looking it
  directly or on bright places", "check if snow is reflection based", "game lags the first time when enabling
  rain/snow", "make the snow layer look better on model/meshes"). Build D9483CC3053B, installed 2026-11-01 02:27 with
  the game closed (game folder + dist, hash checked; all 7 shaders in it; 3FA657AEFF9F at 02:24 before it, without the
  stale-target guard); it holds all of 715CDCED454C.
  **Not tested in game.** Research: Research_Snow_2026-10-26.md section 23.
  - **Its light was the ground's picture, reflections and all.** The snow took the light of the ground's own picture
    round it (blurred, x 1.9), so it copied the ground's reflections, which grow at a glancing view and in bright
    places, and went flat white there.
    - Now it takes the light that reaches the ground: the picture over what the surface gives back of the light, by
      the game's own G-buffer (its base colour, metal mask, reflectance and smoothness, read from the game's
      shaders), per 2 x 2 pixels. The texture and most of the mirror image are taken out, and it's sharp.
    - Where that can't be told (a mirror-like or metal surface, too dark, far in the fog), the sky's light is used,
      never the ground's picture.
    - It's kept below white (rolled off from 0.8), so its shading shows in bright places.
  - **Models, rails, pipes, edges:** where the depth can't tell which way a surface faces, the snow takes the game's
    own per-pixel normal from its G-buffer. So snow lies on rails, pipes, small and far models, and stops at a model's
    own edge. Clean surfaces keep the smooth facing from the depth.
  - **Drone takedowns:** the scene's depth is now the depth the game's G-buffer pass draws with (the last 4 it used).
    Another target the frame's size cleared for something else (likely the disrupted drone's screen distortion; not
    seen in a log yet) can't take its place any more. That's logged as "another depth target shaped like the frame
    was cleared".
    - If no G-buffer pass is seen for 30 frames, the old rule comes back until one is. If none of its targets is
      cleared for 30 frames while another one is, they're given up on, so a stale target never stops the wet look,
      haze, puddles or snow.
    - When the weather isn't drawn while it's on, CT.log now says why, and when it's drawn again.
  - **First-time lag:** the 7 shaders (the main one is about 740 KB, 24,000 instructions) are made on a worker
    thread from the game's first frame, not in the frame rain or snow first comes on. CT.log gives the ms.
  - New: Visuals/GameFrame.cpp / .h. Small changes in GreenPlane.cpp (OnDepthClear), ReplayGameDraws.cpp (its
    render target hook calls GameFrame; g_PassKinds cleared on a size change) and Main.cpp.
  - The changes are worked out from the game's shader code and the tools' own; how they look and what they cost is
    still to be seen.

- **Replays: the missions logged, read only (the step before having the game stream a mission's world in while its
  recording plays)** (the user, 2026-11-01: "Yes, build it", the read-only mission entity log). Build 715CDCED454C, installed 2026-11-01 02:20 with the game closed (game folder + dist, hash checked; holds everything in c2c1fb6460bf).
  **Not tested in game.**
  Research: Research_Mission_Lifecycle_2026-11-01.md.
  - **The server's mission entities** (PamServerMissionEntity: constructor 0x143A72330, destructor 0x143A72F70, vtable
    0x141C7ACD8) are noted as the game makes and destroys them. Their code was byte-checked before hooking.
  - **Each call is logged with the thread that made it** (which thread may call these is the open question):
    - the mission's availability (0x143A77640);
    - its state update (0x143A76D60, with its three conditions);
    - its abort handler (0x143A792D0);
    - its stream out (0x143A74150).
  - **Each mission's state is logged whenever it changes**, looked at once a second from the replay's own thread (its id
    logged once):
    - its name;
    - its data (Enabled, StreamInOnAvailable, the stream-out and re-enable settings);
    - its state (re-enable timer, stream-out time, the flags at + 0x11C - + 0x124).
  - Nothing of the game's is written or called. Lines start with "Replay missions:".

- **Replays: dialog-only sound track with live dialog, smaller saves, cheaper recording, models and people that don't
  vanish off-camera, the saved list always open** (the user, 2026-11-01, a list while testing). Build 2C946F5ED115,
  installed 2026-11-01 01:57 with the game closed (game folder + dist, hash checked); it holds all of 17761413793B.
  **Not tested in game.**
  Research: Research_Mission_Lifecycle_2026-11-01.md, Research_Crowds_Textures_2026-11-01.md.
  - **"The mission's dialog as recorded"** (was "The game's sound as recorded"):
    - while it plays, the game's own sound isn't muted any more;
    - the replay says the recorded voice lines live through the game's voice-over system, where their waves are
      still loaded;
    - the recorded track is heard only while a line it can't say live covers the time (faded over 30 ms);
    - once it stops recording, the track keeps only the dialog's spans (the voice lines and a cutscene's voice-over,
      0.25 s before and 0.6 s after each); the rest is silence, which packs to almost nothing in a saved recording;
    - a loaded recording (which has no voice lines) plays its dialog-only track as it is;
    - what else was heard at a line's moment is still in its span: Windows gives the game's whole mix, not its voices
      one by one.
  - **Saved recordings:**
    - a block-compressed texture part is packed with its blocks' bytes regrouped (8-byte blocks for BC1 / BC4, 16 for
      the others) when that packs smaller. New pack methods 3 / 4; older parts read as before;
    - "Saved recordings" is a section of its own, always open, not a header.
  - **Recording's cost:**
    - the copies read back from the graphics card now reuse a pool of staging buffers (each copy made a new one: a
      driver allocation at every capture);
    - the crowds' b1 windows come from one copy of their buffer a frame;
    - "Replay (recording cost): ..." every 10 s while it records: frames a second, the draw hooks' ms a frame and per
      draw, its work at Present, MB a second copied back, staging buffers made and reused.
  - **Models Faith never looked at:**
    - the game's reflection cubemap faces (the G-buffer's targets at 512 x 512, around the camera) are kept from too,
      for a model or world part none of the G-buffer pass's draws gave yet;
    - drawn in the G-buffer pass when it plays; the G-buffer pass's own draw of it takes its place later;
    - nothing is drawn into those passes.
  - **People:** the "seen" rule (hidden while the game hadn't drawn them for 5 s) now applies only to the mission's
    projection runners (the ghost runners it was for); everyone else is shown for all the time they were recorded.
  - **Missions (research only):** what an abort does (0x143A792D0: Release streams out at once and comes back 1 s
    later), Release's mission settings, and a plan to have the game stream a mission's world in while its recording
    plays.

- **VISUALS > Lights: "Light radius", 1.0 m to start, "Show its shape" kept** (the user, 2026-11-01: "rename About
  like a light bulb to Light radius, remove the reach text and (?). It should also start a 1.0 m. 'Show it's shape'
  check if it does anything if it doesn't remove it").
  - The "About like a light bulb" line is gone. Below Brightness is now the light's radius, "Light radius" (it was
    "Reach", the game's AttenuationRadius), without its (?).
  - New lights start with a 1.0 m light radius (10 m before). The looks still set their own; a copy keeps the
    other's.
  - "Show its shape" does something in the game's code, so it stays (in Advanced, now under Size). The game's
    light update (0x1437064A0) puts the flag in the GPU light's data (0x143706862). When the flag is off it swaps
    the light's shape values for an "off" value (-1e9, -1e9, 100, 100; 0x1437069D7). Checked offline by this
    session and the UI session; what it looks like on screen isn't known. Its tooltip says so and suggests trying it
    with a Size above 0.
  - Only Lights/LightManager.cpp changed. It's in c2c1fb6460bf (the whole folder, built 01:59 once the replay
    session's FaithReplay.cpp built again), installed in the game folder and dist; the new text was checked in both.
    **Not checked in game.**

- **VISUALS > Lights: "Remove all"** ("Forgetting lights doesn't actually remove any light"). "Forget all" only
  ever took the lights off the tools' list without telling the game (the original tools' button, meant for after a
  level change), so they stayed lit. The new "Remove all" (two clicks) turns each light off in the game and takes
  it off the list, the same as each light's own Remove. "Forget all" stays next to it with a tooltip saying what it
  does. When lights were made before a loading screen, a line under the list says so: the game may have dropped
  them, and Forget all is then the safe way to clear the list. In b9140e69406e (another session's whole-folder build,
  01:48, installed in the game folder and dist; checked). **Not checked in game.**

- **VISUALS > Images and Lights: two ticks removed, one button renamed** (the user, 2026-11-01: "Images > Look again
  should be renamed to 'Refresh folder'. Dots on the picture should always be enabled remove the tick. Show the light
  on the picture should always be enabled remove the tick").
  - Images: "Look again" is now "Refresh folder" (it does the same: lists the folder again and reads the placed
    pictures again from their files). The "Dots on the picture" tick is gone: the dots are always there while the
    menu is open. "Drag along the ground" stays, and its help now explains the dots too. config.ini's `[Images] Dots`
    is no longer read or written.
  - Lights: the "Show the lights on the picture" tick is gone: the lights' dots are always there while the menu is
    open. "Keep height" stays, and its help now has the dots' text. The dots' flag (m_ShowDots) was never saved.
  - Files: Visuals/WorldImages.cpp, Lights/LightManager.cpp / .h (the lights session was told). In build b9140e69406e
    (built 01:48 from the whole folder, no source newer; installed in the game folder + dist with the game closed,
    hash checked; the DLL has "Refresh folder" and neither tick's label). **Not checked in game.**

- **Replays: the crowds recorded and played back (people, cars, drones, boats of the city's swarms)** (the user,
  2026-11-01: "Decompile the whole texture/crowd system and how they are placed in the prefabs and fix them").
  Build 17761413793B, installed 2026-11-01 01:38 with the game closed (game folder + dist, hash checked). **Not tested in game.**
  Research: Research_Crowds_Textures_2026-11-01.md.
  - **Why they were missing:**
    - a crowd draw reads its members through the crowd system's own buffers: an indirection buffer of 16-bit member
      numbers, a float4 a member, and an animation value a member for people;
    - each draw's first entry is in b1. None of it is the instance buffer b0 points into;
    - so the world's capture refused them ("no instance offsets in its b0") and the tracks never saw them.
  - **Now, while it records:** every 0.1 s, each crowd draw in the G-buffer pass has its b1 window and those buffers
    copied on the graphics card and read back. Its members are kept with its kind (mesh part and look, its state kept
    once like any kept draw).
  - **While it plays:**
    - each kind's members are drawn at the time played, in a straight line between samples;
    - they're drawn with the tools' own buffers;
    - the game's own crowd draws (its swarms now aren't the recorded ones) aren't drawn while recorded crowds play.
  - **Saved recordings:** a crowds section (`CRWD`). An older file loads as before, without crowds.
  - **Not 1:1 yet:**
    - the people's poses come from the swarm's pose texture as it is when it plays (walkers walk, not in their
      recorded phase), and in a later session as saved;
    - no crowd shadows.
  - **To finish the poses:** the crowd shaders' code is saved once each to `Cinematic Tools\Shaders` (they aren't
    in the shader databases read offline). The pose texture's size and the first samples' values are logged.
  - **Also in this build:**
    - **The gate (diagnostics):**
      - the log line for a live model the game doesn't draw where it was is now written once per label, not for the
        first 40 lines only (air conditioners took all 40 in one second in the last test);
      - a part left out for having no look of its own is now named in the log.
    - **Texture research only (nothing changed):** how a shared mesh's look is chosen (the mesh variation database by
      object variation name hash), and how a mission changes one at run time (shader parameter entities: the gate
      glass's darkening). Two fixes are proposed in the research doc.

- **The game no longer freezes when its job queue fills up (the tools keep the extra jobs instead of running one in
  another's place)** (the user, 2026-10-28: "The loading screen takes a lot of time and now it froze"). Build
  D61D546DD0D3, staged 04:48 to install when the game closes (game folder + dist, hash checked by the job). **Not tested
  in game.**
  - **What froze (read live, read-only: the game's threads' stacks with GetThreadContext + StackWalk64, nothing
    written):**
    - Release's loading screen while the replay recorded (from 04:30:24). The game's job queue (1024 jobs for any
      thread) was full again and again from 04:30:36 to 04:30:56, and the tools' help ran a queued job 541 times.
    - The game stopped drawing about 04:30:56, with the queue empty and two threads at full CPU.
    - Thread 33132 was running a job that handed a job on into the full queue. The help ran another queued job on that
      thread in its place, and that job waits for a job that never finishes. The thread under it still owes its
      hand-on.
    - Thread 31080 waits in the same loop. The quit that hung at 04:28 had a thread in that wait too.
    - So the help itself (added 2026-10-25 for the first full-queue hang, 5e8f1141b8ab) hung the game.
  - **Now:**
    - the game's push has one caller, the hand-on, which doesn't use what the push returns (it then wakes a worker).
    - A job that finds the queue full is kept by the tools, and the push leaves the game's own way.
    - A thread of the tools hands each kept job on with the game's hand-on once the queue has 16 free, the oldest
      first.
    - No thread runs another job in its place, and none waits for room.
    - If a job can't be kept (no memory), the game's own code goes on as before the tools.
    - Log: "Hooked JobQueueFull ..."; "[WARNING] The game's job queue was full (1024 jobs): N jobs kept by the tools to
      be handed on once there's room (... handed on, at most M kept at once, K kept now ...)".
  - **Why not a bigger queue:** every take looks through all of the queue's slots for the best job (0x142FBA210), so a
    bigger queue would slow every take.
  - **Not changed:** what fills the queue during the loading screen (the city's props and the mission streaming in
    while the time of day jumps and its catch-up runs).

- **Replays: the recorded doors and glass drawn when the game's own aren't; no other object's look for a shared mesh**
  (the user, 2026-10-28, after a screenshot of a replay with the Release gate's doors missing and its glass looking
  wrong: "Fix the gate first: draw the recorded doors and glass when the game's live ones aren't actually drawn, and
  stop shared meshes falling back to another object's look"). Build 8D8B816A067C, installed 04:22 with the game closed
  (game folder + dist, hash checked). **Not tested in game.**
  - **Why (the 04:01 test's logs):**
    - the screenshot's playback (04:09:51, paused at about 9.6 s) ran while the mission's objects were in the level
      again: "354 moving models (295 of them there now ...)";
    - the replay then moves the game's own objects and doesn't draw its copies of them;
    - the game didn't draw its `releasegatedoor_01` at any of those seconds, so the doors weren't drawn by anyone;
    - the glass was the game's own, as the mission has it now.
    - In the playback before it, with those objects gone, the replay drew its door copies throughout (1,794 draws).
  - **Now, while it plays:**
    - the game's drawing of the recorded models' meshes is read back the way the world's is (where the game draws
      their instances);
    - a model the game has but doesn't draw within 1 m of where it was, for half a second, gets its recorded copy drawn
      too, and let go again once the game draws it there for half a second;
    - logged each time ("... is in the level, but the game doesn't draw it where it was ...").
    - This needs "The mission's world too" on (it's what reads back the game's drawing).
  - **Shared meshes:** a model none of whose part's kept looks had an instance within 2 m of it took the first look kept
    of that mesh, another object's. For example, the gate's sign planes (`plane_1-1_01`, a mesh many signs use with
    their own pictures).
    - Now that happens only when all of the part's kept looks are alike: the same shaders, and in each slot the same
      texture, or one of its kind at another size (streamed in or out), or a placeholder.
    - Else the part isn't drawn, nor its plain shape: nothing rather than another object's picture.

- **Replays: the crowds' instance buffers read back for the log** (the user, 2026-10-28, after the 04:01 test of
  138097065E6F: "read the log"). Build 6EBD06F75FF6, installed at game close 04:11:41 (game folder + dist, hash checked). **Not
  tested in game.**
  - **What the 04:01 test showed:** the crowd system's draws read their instances from **t24 `transformBuffer`**
    through **t23 `indirectionBuffer`**, with **b1 `externalConstants`** (16 bytes; its window moves 80 constants a
    draw) and b2 `viewConstants`.
    - The six draws logged were all 6-index ones (two triangles an instance: taken to be the crowds' far quads): the
      backdrop cars and the crowd motorbikes.
    - The log asked the vertex shader for its views t0 - t7 only, so it said "none". The crowds' are t23 and t24.
  - **Now:**
    - the views logged are t0 - t31;
    - up to 6 full-body crowd draws (more than 6 indices) are logged besides 4 of the far quads;
    - for up to 4 crowd draws (one far quad, three full bodies), the b1 window and the t23 and t24 buffers are copied on
      the graphics card and read back a few frames later;
    - their layout is logged: sizes, structures, views, the b1 window as numbers, t23's first 32 values and t24's
      entries (its first three and the ones t23 names).
  - **That's what keeping the crowds needs:** where each instance's transform is. Nothing is drawn from it yet.

- **Replays: the crowds' draw inputs really logged now** (the user, 2026-10-28, after the 03:52 test of 15CA7A7F4355:
  "Done read the log"). Build 138097065E6F, installed at game close 04:00:29 (game folder + dist, hash checked). **Not tested in
  game.**
  - **What the 03:52 test showed:**
    - **Release's ground** (`ground_release_01`) is kept now. The replay drew it at 18 of the 58 seconds it was drawn
      while it recorded; the game drew it itself at 7 of them. Before, it was never kept.
    - **The monorail ground** was drawn at all 122 of its seconds again.
    - **The save worked** (2026-10-28 03-55-00 Trainstation+Release).
  - **The crowds' log didn't run: my mistake in 15CA7A7F4355.** It waited for the reason "no instance offsets in its
    b0", but the copy step's text is "not instanced: no instance offsets in its b0" (TakeState). Now it matches on the
    shorter text.
  - **It also logs the names in the crowd's vertex shader** (its constants and resources, as the draw log noted them
    when the shader was made), to see how the crowd system feeds it its instances. The crowd's shader has no
    `dxVectorBufferOffsets`, where the game's other instanced draws find theirs.
  - **Still missing in the 03:52 playback:**
    - the crowds (`lowcaste_crowd_fullbody_*` 74 - 75 seconds, `midcaste_crowd_*`, crowd drones and cars);
    - a building (`csg_rz_area01_building09`, 21 seconds);
    - the red tutorial pipe (10 seconds);
    - the vista meshes (left out on purpose).

- **Replays: Release's ground kept; the crowds' draw inputs in the log** (the user, 2026-10-28, after the 03:33 test of
  00C3D74666BE: "Read the logs"; Research_Mission_Prefabs_2026-10-28.md section 7). Build 15CA7A7F4355, installed at game close
  03:50:34 (game folder + dist, hash checked). **Not tested in game.**
  - **The 03:33 test answered the research build:**
    - the Release mission's partitions are found by guid in the game's registries, also while a recording plays (the
      replay's bundle reload makes them again);
    - a partition's objects are in its file's order;
    - the creation structure holds the entity's transform at + 0x40, its data at + 0xD0 and its entity bus at + 0x10.
  - **Long world meshes:** a world draw's instance was only taken when its origin was within 600 m of the camera, so
    `ground_release_01` (Release's ground) and other big meshes weren't kept. Now that search adds the mesh's
    bounding-box reach, as the keeping does.
  - **Crowds:** every crowd draw (pedestrians, crowd drones, cars, motorbikes, trucks) failed with "no instance
    offsets in its b0": their instances aren't where the game's other draws keep them. The first 6 such draws now
    have their vertex buffers, vertex shader views and constant buffers logged, to find where the crowd system keeps
    them.

- **Replays (research, read only): the game's creations of static models and prefabs, and its partitions by guid, in the
  log** (the user, 2026-10-28: "Yes, build the read-only step: log the game's creation info for static models and
  prefabs, and find where loaded partitions are kept by guid"; Research_Mission_Prefabs_2026-10-28.md sections 2, 6). Build
  00C3D74666BE, installed 03:26:12 with the game closed (game folder + dist, hash checked). **Not tested in game.**
  - **Two byte-checked hooks.** Each only reads, then passes the call on:
    - the shared create of the static models' factory (and 38 other kinds') 0x143163210;
    - the create of the reference kinds' factories 0x1431C3F40 (ReferenceObjectData, SpatialReferenceObjectData,
      ObjectReferenceObjectData, SpatialPrefabReferenceObjectData).
  - **While the replay records, each creation is a line** (up to 160 static models and 80 references a recording):
    - the data's type and realm;
    - the static model's mesh, or the reference's blueprint and its place;
    - the data, the context (`info` + 0x10, with its vtable), the entity, the info, the thread;
    - the game's code that called.
  - **The first 3 static models and 4 references** also have their 0x100 bytes of `info`, the data's first 0x40 and
    the context's first 0x40 dumped (pointers to the game's objects named by their vtable).
  - **Partitions by guid** (found offline: a compartment's entry + 0x68 is its registry; its by-guid map's buckets at
    + 0x130, their count at + 0x138; the game's lookup 0x142AEB5E0 is the registry vtable's slot 16):
    - the probe reads each loaded compartment's map (read only, never calling the game);
    - it checks each registry's vtable holds the lookup, and looks for 5 of the Release mission's partitions by their
      guids (`release`, `layer1_release_logic`, `pf_releasegateglass`, `csg_releasegateglass_01`,
      `releasegatedoor_01_businpc`);
    - it runs 8 s into a recording, as it stops, and 10 s into a playback (when the replay has loaded the mission's
      bundles again);
    - each partition found has its head dumped, and any array it points at that holds data a creation was logged from
      is named, to learn where a partition keeps its objects.

- **Replays: 24 passes of a kind kept; why a world draw wasn't kept, in the log** (the user, 2026-10-28, after the 02:57
  test of 2628A83E96DF: "Read the logs"). Build 248CF728F525, installed at game close 03:09:30 (game folder + dist, hash checked).
  **Not tested in game.**
  - **What the 02:57 test showed:**
    - **The gate glass was kept:** "'csg_releasegateglass_01_Mesh' section 0 kept as the game draws it". The 8-pass
      limit was why before: a frame had up to 17 lit picture's passes after its G-buffer pass.
    - **The monorail ground** (`monorailground_release_01`) was drawn by the replay at all 137 seconds it was drawn while
      it recorded. Before, it was missing at all of them.
    - **The save worked:** 2026-10-28 03-00-08 AnchorArea1+Release, 34.2 MB and 514.8 MB of new parts.
    - **Still missing:**
      - the crowds (`lowcaste_crowd_fullbody_*`, 84 - 90 seconds each), the crowds' news drones and backdrop cars. They
        aren't among the tracks, so the replay doesn't take their vertex shader for one that reads bones; as world draws
        they were let go without a line;
      - a catwalk's glass sheet, a floodlight, a ladder, a toolbox, some pipes (5 - 58 seconds each).
    - **Vista meshes** (zones' far stand-ins) are left out on purpose.
  - **Now:**
    - **24 passes of each kind are kept.** A world key holds its pass in 7 bits from bit 25. A draw whose first index
      doesn't fit below that isn't kept (never seen).
    - **Each world draw not kept is in the log**, once a mesh, with why and its instance layout (up to 120 meshes).
    - **"kept draws drawn in the game's ..." is logged once for each pass**, not each kind, so the glass's line shows.
  - **The asset log's texture list stays a rough guide.** One mesh part is drawn in the G-buffer pass with up to 25
    textures in a slot over a recording (shader permutations, variations, streamed sizes), so "other textures" there
    isn't a mistake by itself.

- **Replays: the gate glass's pass kept, long meshes kept, the asset log's texture check fixed** (the user, 2026-10-28:
  "Search all the game patches we might need them in the feature make no mistakes", then "After this read the logs and
  based on the game patch research you did fix all the model/mesh missing issues and texture ones";
  Research_Game_Patch_2026-10-28.md, Research_Mission_Placement_2026-10-27.md section 7). Build 2628A83E96DF, installed at game close
  02:55:45 (game folder + dist, hash checked). **Not tested in game.**
  - **The patch:** every file it changes, adds or drops was catalogued (Game_Patch_Catalog_2026-10-28), and every count
    checked against the others.
    - It changes nothing of the Release gate: no mesh, texture, placement, or Release's variation data. The meshes it
      touches are all new ones.
    - The replay reads the game's memory, so it has the patched data anyway: the patch isn't why models or textures
      differ.
    - The fb tools' importers.py and show.py miss the 502 delta-stored ebx; new scripts read them.
  - **The gate glass** (02:29 test, build DCE0C6DE0AF4):
    - It was recorded (in the list of models that didn't move).
    - The game draws it in the lit picture's pass (pass #3 in the asset log's new list), which the replay keeps. But
      it was never tried.
    - The replay kept a kind's first 8 passes after the G-buffer pass in a frame; the glass is taken to be past those.
      Now 15.
    - CT.log says how many there are in a frame, and names a recorded model drawn past the 15th.
  - **Long meshes** (`monorailground_release_01` missing at all 173 seconds played; elevated roads, backdrops):
    - A world draw was kept by its instances' places only (within 100 m of the camera).
    - Now each mesh's bounding box is read from the game's mesh table (the MeshSet's data starts with it, checked to be
      plausible). An instance counts by the nearest its mesh can be, scaled.
    - Such a draw is drawn while its mesh reaches within 150 m of the replayed Faith.
    - Vista meshes keep the old rule (a zone's stand-in).
    - Tries aren't counted before the boxes are known.
  - **The asset log's texture check:** the replay's draws in the decal, lit and half-size passes were compared with the
    game's G-buffer ones (a sign plane "played with" a 3840 x 2054 target). Now only the G-buffer pass's are compared.
  - **Crowds** (missing in the playback; 86 tracks kept): CT.log lists the tracks kept and the ones not usable, by mesh,
    for the next fix.

- **The KrugerSec body ripples stop while the game is paused** (the user, 2026-10-28: "These ripples don't freeze while
  the game is paused, unlike Faith's step rings. Can we make them freeze?"). Their waves' ages ran on the wall clock;
  now each body's down and knock times are put onto the rings' own clock (which stops while paused) when first seen,
  so his waves hold still in a pause as her rings do. The water his body pressed already did. Build 22F29E061CBC,
  staged: it installs itself once the game closes. **Not checked in game.**

- **A KrugerSec going down in the rain's water leaves ripples in his body's shape** (the user, 2026-10-28: "ksec
  ragdoll/dying should leave behind their model ripples"). Before, a ragdoll's knock made one round ring at the knock.
  Now waves go out from the outline of his body (his bones as capsules, their sizes from his mesh; the snow session's
  SnowBodies): one train when he lies down and a smaller one at each later knock (a bounce, a kick). Like her slide's
  wake, they're quick at first and slow, their crests spread and they die away over 6 s, stopping at edges. Under him
  the water is pushed away. Where his body pressed the water (his mesh's stamps), its shape stays a few seconds after he
  moves. A knock's round ring isn't made where one of them lies. Build DCE0C6DE0AF4, installed (game folder and dist),
  with the snow session's real-hand prints and the landing pads. **Not checked in game.**

- **A ripple when Faith lands on a landing pad** (the user, 2026-10-28: "landing on a jumppad should leave a ripple",
  "i mean landingpads", with a screenshot of a soft-landing tarp). The Runners' soft-landing tarps sag, too far from
  level for a puddle, and rings were only made in puddles, so a landing there left nothing. The 18 tarps placed in the
  city were read from the game's data. Now when her feet come down fast (4 m/s or more) and stop inside one of them in
  the rain, a landing's ring (as big as the fall makes it) spreads over the wet tarp. It covers whatever of the tarp
  faces up, allowing for its folds and sag, and stops at its edge. A landing near a tarp but outside its box is noted
  in CT.log. In de80f12cba0e (the exit-hang session's build, installed 02:14). **Not checked in game.**

- **Snow and rain: her handprints are her own hand, posed as she touched** (the user, through the weather session,
  2026-10-28: "handprints for snow/rain should be faithful to faiths hand"). Build e61c3f129bf9 (02:20, the whole folder: the weather session's landing pads and the exit-hang session's 15 s stuck-quit end are in it; 7 blobs checked), installed with the game closed (game folder + dist, hash checked). **Not tested in game.**
  - **The print:** at a touch, her hand from her own arms mesh (skin and glove, full detail), posed as the game has
    it, is pressed for 0.4 s into a stamp in the touched surface's plane, ground or wall. It covers where the hand came
    within 1.5 cm of its nearest point to the surface, at 4 mm a texel, so the fingers show as she held them.
  - **Where it's drawn:** both the snow's prints (floor, inner wall, rim) and the rain's wet prints are drawn from it.
    A touch where her hand never came within 6 cm of the surface, or her body wasn't read, keeps the drawn hand
    (HandShape).
  - **KrugerSec in the rain (for the weather session's ripples):** a KrugerSec lying in the water now presses his
    outline into it as in the snow. SnowBodies gives his body as 15 capsules sized from an Enforcer's mesh
    (snowbody::LyingBodies). Research section 22.

- **Replays: saves no longer hang, the half-size transparent passes kept, where the mission's gate glass is drawn
  logged** (the user, 2026-10-28: "I think the gates models etc. you are trying to find are mission streamed based ones
  find out how they are placed and implement them in the replays also the replay is ready";
  Research_Mission_Placement_2026-10-27.md section 6). Build 2DA21C67747C, installed 02:09 with the game closed (game folder + dist, hash checked; has the snow session's SnowHandMesh work as of 02:08). **Not tested in game.**
  - **The saved recording that never finished:** the 01:48 save of the 01:44 recording (167 s) wrote 1532 parts and then
    waited forever; no Recording.ctr was written.
    - The save writes its parts in order, but the readback copied any later part that fit its budgets. A part left out
      stayed out once the read-ahead parts (512 MB waiting to be written) filled the queue.
    - Now the part the writer waits for is always copied.
    - That recording can't be got back: record it again.
  - **Where the gate's parts come from:**
    - The glass (`pf_releasegateglass`), the two animated prison doors and the bus doors are placed by Release's own
      `layer1_release_logic`; the frame by its `layer5_release_sound`.
    - These are patched ebx files, which the first search skipped.
    - The barriers and `customreleasegate_01` are the city's zone props 291 / 293.
  - **The glass:** a plain static model with a glass material, never drawn in the G-buffer pass, and no kept draw of it
    was drawn. Two things now:
    - **The half-size transparent passes are kept:** one RGBA16F target with a depth target half the picture's size,
      kept like the decal and lit passes and drawn at the end of the same pass while it plays.
    - **The asset log** lists every named mesh drawn only outside the G-buffer pass while it records, with each pass's
      targets, depth size and whether the replay keeps it.
  - **CT.log** lists the labels of the recorded models that didn't move.
  - **Not built:** the game making the mission's prefabs again while it plays (the glass in its own pass, shadows, signs,
    the glass darkening). The game's code that makes entities from a blueprint isn't decompiled yet.

- **Snow: gravel white under it once it covers the ground** (the user, 2026-10-28, a screenshot of a roof's gravel
  bed: "This is how gravel looks"). Build 49dc04be2aee (01:50, the whole folder: the replay session's 374D811C3D6E sources in it; 7 blobs checked), staged: it installs at game close + 150 s, only over an older build. **Not tested in game.**
  - Until 3 cm had piled, the snow's facing and bends were taken pixel by pixel, so on gravel each stone's facets cut
    its cover: grey and speckled while the concrete round it was white.
  - Once the snow covers the ground, relief of a few cm counts as buried. Its facing is taken over 12 cm on the ground
    (4 - 48 px), wider than a stone, and only where the surface is in line that far, so never across an edge.
  - The soft edge ignores drops under 4 cm at full cover. Research section 21.

- **Replays: the mission's runners and ghost runners go when the game takes them out, fewer false variations** (the
  user, 2026-10-28, after the 01:21 Release test: "You can still see the runners/ghost runner not despawning in the
  actual mission"). Build 374D811C3D6E, installed at game close 01:40 (game folder + dist, hash checked). **Not tested in
  game.**
  - **Why they stayed:** the hold that keeps the people near the recorded ones in the level was on while it recorded.
    - It refused the game's requests to take out 9 characters: the 4 ghost runners (SpawnTemplate_Projection_Replay,
      each about 2 s after it was made), both runner_02s and the opening's cutscene people.
    - It kept the sub-level the Icarus runner was in loaded from 81 s.
    - The log called every one of them "KrugerSec". The 5168E0086553 entry's "stayed in the level, hidden" was wrong:
      it was the hold.
  - **Now:** while it records, and between recording and playing, nothing is held when the replay draws the people with
    the game's own draws of them (the default). One the game takes out is drawn from those up to when it went, as in
    the mission. The hold stays while a recording plays, and for recording too when that drawing is off.
  - **The log:** the kept ones are logged as characters, drones or vehicles.
  - **Variations:** the 01:21 test kept 31 "variations" of a backdrop car driving by, and many of billboards and air
    conditioners drawn in batches elsewhere. Now a later draw with the same shaders and the same textures in every slot
    is the same look wherever it is: its instances are added to the kept one.

- **Replays: mesh variations kept apart** (the user, 2026-10-27: "Do more research and decompiling on how the game
  places textures/meshes/models on missions especially and find a way to improve replays";
  Research_Mission_Placement_2026-10-27.md). Build A49A81555A52, installed 23:43 (game folder + dist, hash checked).
  **Not tested in game.**
  - **The variations:** each level's MeshVariationDatabase gives a mesh variations, each with its own textures
    (Release: door_breachsingle_01 in 3, door_glitchkey_oneway_right's red one). The game draws each as a draw of its
    own.
  - **What the replay did:** it kept one draw per part, and its texture swap took textures across variations.
  - **Now, while it records:** a kept model's or world part is kept again from the game's later draws of it (a
    candidate: every 30 frames for a model, every 20 for the world).
    - A candidate with an instance within 0.3 m of a kept one's is the same objects. It adds the instances that one
      hadn't, and its bigger textures with the same shaders.
    - Else it's another variation, kept as its own (up to 32 a part; logged).
  - **Playback:** a model is drawn with the variation whose instance is nearest where it was.
  - **People:** the by-mesh texture swap for people is gone.
  - **Saved:** variations load back under their own numbers (bit 40 of the key).
  - **Not built, to try first:** the write-up's section 4, letting the game build the mission's world itself through
    the districts (GAME > Loading the world > Districts: Prison Yard to Always, Prison to Never, in the prison area).

- **Snow: no grey strips along shadows and edges, shadows on the snow as the game has them, no concrete or gravel
  showing through** (the user, 2026-10-27: "Snow becomes grey on edge and if i don't look at it directly find a fix",
  "Shadows bug out the snow effect fix it no mistakes", "snow still looks off on concrete surfeces and gravel fix it
  high priority no istakes"). Build fe96e22c41e7 (23:23, the whole folder; 7 blobs checked), installed with the game closed (game folder + dist, hash checked). **Not tested in game.**
  - **Grey strips:** the test that keeps snow off the game's effects and HUD took the sunlit side of every shadow's
    edge, and every lit edge with something darker beyond, for an overlay. In strong sun it took up to four fifths of
    the snow off there. A pixel now also has to be more than about twice as bright as its surroundings. A shadow's
    edge is 1.3 - 1.6x; glows, sparks and HUD text are over 2x.
  - **Shadows:** the snow was darker and bluer wherever its spot was darker than the frame 128 px round it. A big
    shadow came out lit inside with a dark band along its edge, and by an edge or towards the horizon the bright sky in
    that blur greyed the snow. Now it's the sky's light, or the ground's own where the sun's brighter, so the game's
    shadows lie on the snow as they lie on the ground.
  - **Concrete and gravel:** the snow's sunlight came from the ground 6 px round, so stains, seams and stones showed
    through. It now comes from its own surface 6 - 20 px round, gravel's stones included; deep snow's no longer from
    the frame 32 px round (walls, sky). Shadows on the snow are about 20 px soft at 4K.
  - Not changed: gravel's own shape (its stones' facets) and the fog's grey at middle distances (the fog's colour
    isn't known to the pass). Research section 20.

- **Ripples stop at edges** (the user, 2026-10-27: "Ripple effect should respect edges and not go over them"). The
  rings from Faith's steps and landings, other people's steps, the cars' wheels and her slide's waves spread wherever
  the ground was within 30 cm of their height: down curbs and steps, over ledges' and roofs' edges and gaps. The rings
  on the game's own puddles also went onto walls and whatever stood there. Now a ripple only reaches what it gets to
  over the same surface: between where it started and each point, the surface is looked at every 30 cm, and where it
  drops away (more than about 5 cm) the ripple stops there, softly. Faith's legs or anything else in front don't stop
  it. The rings on the game's own puddles are only on level ground, and the slide's waves only on what faces up. Build
  1017B084D768, staged: it installs itself once the game closes. **Not checked in game.**

- **Replays: doors' decals and screens, the right textures, half-missing models, runners that vanish** (the user,
  2026-10-27, after the 22:50 test: "ksec doors still have the wrong textures and alphas on doors are still wrong and
  half of the mesh is missing also ghost runners/runner don't despawn"). Build 5168E0086553, installed
  23:20 when the game closed, then replaced by FE96E22C41E7 (the snow session's whole-folder build, 23:23:47, which
  holds these changes: checked by its strings and by the sources being older; game folder + dist). **Not tested in game.**
  - **What the 22:50 test showed:**
    - 65 tracks kept (3.4 MB), 12173 texture slots swapped.
    - The replay's own drawing took 1.1 - 1.9 ms a frame on the processor (260 - 570 draws).
  - **Textures:** the swap took textures from the game's draws of the same mesh part with other shaders. The gate
    frame's slot 7 got a 2048 x 2048 texture array it was never drawn with. Now a kept draw takes a texture only from a
    draw with its own vertex and pixel shaders, and only a bigger one of the same kind (format, layers, shape). A
    placeholder (8 x 8 or less) may take any.
  - **Other passes:** the draw log saw door_glitchkey drawn in two passes after the G-buffer pass, which nothing kept:
    the decal pass (the G-buffer's first three targets, 6 indices) and the lit picture's pass (one RGBA16F target with
    the G-buffer's depth).
    - Now a recorded model's or the world's draw in those passes is kept too: its pass, and its place among that kind's
      passes after the G-buffer pass in the frame (parts that aren't a section of the shape are numbered from 0x4000).
    - While it plays, each is drawn at the end of the same pass, at the level the G-buffer pass drew the model at.
    - People and tracks stay in the G-buffer pass only. Shadows still aren't kept.
  - **Half-missing models:** another level is drawn whole only when every part of it was kept. Its drawn parts are
    counted from the mesh data, in the section's bits 6 - 11. Otherwise the model is drawn at its shape's level, as
    before.
  - **The world where the game draws it:** all of a mesh's kept copies were left out whenever the game drew any copy of
    that mesh (in the same session the mission's world is held).
    - Now the game's own copies of a kept world mesh are read back every 30 frames while it plays (at most 16 a pass).
    - A kept copy is left out only within 0.5 m of one of the game's, and only while that's known (150 frames).
  - **Runners that didn't despawn:** the Release mission's projection runners (SpawnTemplate_Projection_Replay, 7 of
    them) and its runner (runner_02, drawn by the game only at 11 - 44 s) stayed in the level, hidden, so the replay
    drew them to the end.
    - Now each recorded person's times the game drew it come from the tracks' samples.
    - A person drawn with its kept draws is drawn only within 0.5 s of one, or between two up to 5 s apart.
    - Saved with the recording.

- **Replays: no repeating sound while the start waits** (the user, 2026-10-27: "there is a weird sound stack until the replay
  loads fix that"). Build 9B9BFF6E81D9, installed 22:38 (game folder + dist, hash checked). **Not tested in game.**
  - While the start is held for the mission's bundles (up to 20 s), the replay's time stands still, but the sound track
    wasn't told it was held (only a pause or the stand-ins' hold was).
  - The track ran on past the time standing still and jumped back to it each time it was 60 ms off (JumpSeconds). The
    same short piece played over and over until the replay moved.
  - Now the bundles' hold silences the track as a pause does, and holds the gun effects the same way.

- **No more bare "shadow" squares in the snow (or dry ones in the rain); fewer squares flickering** (the user,
  2026-10-27: "snow has a weird 'shadow' square that goes away when i move the camera too close from a distance",
  "snow sometimes flicker"; found by the snow session). The weather's 2 m squares, which say what's under a roof or
  under a ledge, were each looked at from their middle alone, up and down from the camera's height. A ledge, a roof's
  edge, a wall top or a sign right there counted for the whole square: its street half was "under" it and got no snow
  (or stayed dry in the rain) until a look from another height found the street. Squares on such edges could flip
  between looks. Now each square is looked at from five places (its middle and its quarters' middles) and takes what
  most of them found. To keep the cost as before, a new square is first looked at from its middle (as quickly as
  before), then from all five; while the five agree, only the middle is looked at again, all five when that changes.
  The look in the shader is the same. Build A1A704818BDB, installed (game folder and dist), with the fans and the snow session's latest. **Not checked in
  game.**

- **Replays: the game's better textures, models at any detail level, and every other animated thing** (the user,
  2026-10-27, after the 22:02 test: "hunter drone didn't had any model textures are still the wrong ones/alphas the same
  ... Decompile the above and any systems you need to learn that stuff so we are done with the whole promise of the
  replay system"). Build BAF79F097CFF, installed 22:20 (game folder + dist, hash checked). **Not tested in game.**
  - **What the 22:02 test showed:**
    - The loaded 21:26 recording's people and models now drew with the game's own draws (773 model parts, 39 people
      parts).
    - The new recording kept 583 of the world's draws and drew 771 parts of them.
    - It saved 357.7 MB of new parts in 42 s.
  - **Textures and alpha:** the asset log showed the kept draws drew with the textures they had when kept. The glitch-key
    door was kept with 128 x 128 versions while the game drew it at 1024 x 1024. Texture streaming makes new texture
    objects, and a placeholder has no cut-out alpha. Now, while it records, a kept draw (a model's, the world's, a
    person's, gun's or drone's) takes each texture slot anew from the game's draw of the same mesh part whenever that
    texture has more texels (every 30 frames for models and people, every 20 for the world). The count is in CT.log.
  - **Models the game drew only at another detail level:** while it records, each recorded model's mesh is looked up
    once a second at its other levels loaded (replay_shapes::MapLevels). The game's draws at those levels are kept too
    (their parts numbered from 0x8000, the level from bit 12). A model with no kept draw at its shape's level is drawn
    whole from its most detailed other level's, and its plain shape isn't drawn then.
  - **Other animated things:** "Other animated things (drones, crowds)", on; config SkinnedTracks.
    - The drone missing in the 22:02 test was the security drone (dronesecurityskinned, 27 - 67 s), not the hunter drone
      (drone_hunter, recorded and drawn). The offline game data shows its mesh is the no-AI drone's
      (characters/enemy/dronesecurity/aidrone_noai) and is in the Release level's mesh variations. The replay records
      drones only as PamClientDroneActorEntity within 100 m. This one wasn't recorded (which class, or how far, isn't
      known).
    - **While it records:** every skinned draw in the G-buffer pass is now kept as a track: its state once, its
      instances (bones) every 0.1 s of the recording. At most 512 tracks and 512 MB.
    - **When it stops:** Faith's (by mesh name) are let go, and so are the instances within 1.6 m (2 m for a gun or
      drone) of a recorded person, gun or drone drawn with its own kept draws.
    - **While it plays:** each instance is drawn between the samples around the time, matched to the next sample's
      nearest within 1.5 m. Not in a gap over 0.6 s, not over 250 m from the replayed Faith, and not within 0.5 m of a
      posed draw of the same mesh.
    - Saved in a new section (TRAK).
  - **Speed:** CT.log now has the replay's own drawing's time on the processor every 600 frames it draws (ms a frame,
    draws a frame).

- **No rain or snow look on the city's fans** (the user, 2026-10-27: "make snow/water not apply on any fan surfece").
  The big air fans (their housings, covers and blades) stood in the wet look's puddles, rings and slope water and had
  snow lying on them. The 329 fans placed in the city's zones are now among the boxes the rain and snow looks leave
  alone, each 30 cm bigger all round, with the same 25 cm fade, so they look as the game draws them. The fans were
  read from the game's data, offline. The ramps by the fans and the fan tubes' bodies are left out. SkyCity's,
  KrugerSec's and Black November's fans aren't among them: those aren't placed in the city's zones. Only the 64
  nearest kept boxes count at once, as before. In A1A704818BDB (installed 22:21, with the squares' fix below). **Not
  checked in game.**

- **Snow: body outlines smoothed, stamps only where a body presses, her rain slide's wake in her body's shape** (the
  user, through the weather session, 2026-10-27: "snowbody outlines should be smoothed out", "waterslideing should have
  faiths model outline just like snow and smoothed out", "snow has a weird 'shadow' square ...", "snow sometimes
  flicker find out why and fix it"). Build 80ebd7988909 (22:14, the whole folder, so the weather session's feet band
  and fans are in it too; 7 blobs checked). Installed 22:18 when the game closed (game folder + dist, hash checked).
  **Not tested in game.**
  - **Smoothed:** what the pass draws is each body's mask through a 3 cm Gaussian. The triangles' 2 cm steps and the
    mottled floor are gone.
  - **The 21:41 test's bug:** walking down a slope widened the band looked in, so empty stamps were taken and stamps
    the snow was filling were thrown away for them. A stamp is now taken only once a texel is pressed, and given back
    otherwise. Her knees count only while her hips are low. A KrugerSec lying there again reuses their own stamp.
  - **Her slide through rain water:** while the wet look is on, her body (hips low) presses stamps too, kept 7 s.
    SlideWake pushes the water aside in that shape where a stamp covers it, else the 30 cm band as before. The waves
    are unchanged.
  - **The shadow square and the flicker:** most likely the weather's 2 m squares. One ray per square, at its middle,
    from the camera's height, decides "under" for the whole square: a ledge or roof edge there leaves the street half
    without snow until a closer sweep. Not confirmed (a screenshot would). Passed to the weather session, whose code it
    is.

- **No rain or snow look on Faith's and people's shoes; ripples on the game's own puddles; no water down railings or
  holed metal** (the user, 2026-10-27: "Smooth out the shader when faith ksec interact with snow/rain and the shader is
  on their model", "dropping from a big jump should create a ripple if there is a puddle on the landing pad", "Waterslides
  effect shouldn't be possible on metal railings/metal holes"). Build a62409831150 (with the feet change already in
  another session's 8e1eb73985e3), staged (the game was running): it installs itself once the game closes.
  - **Their feet:** the boxes the rain and snow looks leave alone round Faith and people started 12 cm over the ground,
    so their shoes and ankles had the puddle's mirror and rings or snow on them, with a line where the box began. Now
    what's 4 - 8 cm or more over the ground they stand on is theirs down to their feet; the ground stays wet or snowy
    up to them. Every box's edge fades smoothly (25 cm).
  - **Big drops on the game's own puddles:** the tools ring only their own puddles; the game's puddle decals (its
    "wet concrete", where it plays her water splash) had none, so a landing there made no ripple (CT.log 21:41,
    21:47). Now her steps and landings on the game's own water ring there too, a big drop's ring as big as in the
    tools' puddles (fall-scaled), within about 1.4 m of where she landed.
  - **The water down slopes** now needs the slope to be solid and broad, as puddles do: none on railings, metal with
    holes, steps or narrow tops; nor the drips off their ends.
  - **Not checked in game.**

- **Saved recordings can be renamed** (the user, 2026-10-27: "add the option to rename the replays"). Build E6DDBC5EB5DB,
  installed 21:56 (game folder + dist, hash checked). **Not tested in game.**
  - REPLAY > Saved recordings: "Rename" next to Load / Delete opens the name in place (Enter or "Save name" renames,
    "Cancel" closes).
  - The recording's folder is renamed on the store's thread, after any save, load or delete before it.
  - The name loses what a folder's name can't have (\ / : * ? " < > |), leading and trailing spaces and dots, and
    anything past 60 characters. It isn't renamed over another recording's (the status line says so).
  - The list is now newest first by when each was saved (from its file), so a renamed one keeps its place. "Loaded
    from" follows a rename.

- **Saved recordings: people, guns and models drawn after a restart; the mission's world kept too** (the user,
  2026-10-27, after loading the 21:26 Release recording in a new session: "After restarting ksec wasn't there and
  mission characters in general ... textures are mismatched/wrong ones texture alphas don't exist and the ksec gates are
  still only the doors part find a way to fix everything"). Build 8E1EB73985E3, installed 21:52 when the game closed (game folder + dist, hash checked). **Not tested in game.**
  - **People, guns, drones and models:** in the 21:33 session the load made all 1349 parts. No kept draw was drawn,
    though: the replay's draw hooks only went in when a recording captured shapes, and that session only loaded. They
    go in now when a recording is loaded (replay_shapes::WantDrawHooks). The 21:26 recording needs no new save for this.
  - **Textures and alpha:** what showed was the plain-shape fallback. It's the tools' own shader with one colour
    texture, picked from the game's draws, and no alpha cut-outs. With the kept draws drawn, those parts use the game's
    own shaders, textures and materials. A model with no kept draw (not drawn by the game while it recorded) is still a
    plain shape.
  - **The mission's world too** (REPLAY, under the game's own drawing; on; config WorldDraws). The KrugerSec gates'
    barriers and pillars (releasebarrier_01 / _02, releasepillar_02, the mounting plates) aren't entities the model scan
    sees (probably the mission's model groups). In the same session the game still had them (the held mission world).
    After a restart only the recorded doors and frames were there.
    - **While it records:** every 20 frames, the G-buffer pass's draws that aren't a recorded model's (by its shape's
      buffer and mesh name), nor a person's, gun's, drone's or door's (bone shaders, the wanted buffers), are kept once
      each.
    - **What's kept of each:** its copies within 100 m of the camera, and its mesh's name and level from the game's mesh
      table. Only a mesh's most detailed level seen is kept.
    - **Dropped:** a single one kept within 2.5 m of Faith or a recorded person, gun or drone at the time (a part of
      theirs drawn without bones).
    - **While it plays:** each is drawn as kept within 150 m of the replayed Faith, unless the game drew its mesh in
      its main pass within the last 60 frames (found by the game's buffers of that mesh, read once a second). At most
      4000 are kept, and at most 131072 float4s of their copies are drawn a frame, the nearest first.
    - **Saved with the recording** (the recipes section's version 2; version 1 files still load).
  - **Faster saves:** parts are packed with XPRESS-Huffman (about 100 MB/s in the offline test). LZMS took 106 s for the
    21:26 recording's 124 MB of parts, mostly block-compressed textures it hardly shrinks. Sections (frame data) stay
    LZMS.
  - **Folder names:** the city's own bundles (VisualEnvironments, VISTA, LMS, logic, backdrops, sky, ground, ...) are
    left out of a recording's name; the 21:26 one was "VisualEnvironments+VISTA_CityBuildings+R...".

- **Replays: every recording saved to Cinematic Tools\Recordings with all its parts, and loaded in any session** (the
  user, 2026-10-27: "save every recording to Cinematic Tools\Recordings with all its parts, and load it in any session.
  also find a way to make them smaller"; Design_Mission_Capture_2026-10-27.md stage 1). Build 5D78B2B62BA3, installed 21:16 (game folder + dist, hash checked). **Not tested
  in game.** The store's packing, file blocks, hashing and texture sizes were checked offline (a test program including
  ReplayStore.cpp: all passed).
  - **What's saved:** a recording is saved 1.5 s after it stops (REPLAY > Saved recordings > "Save every recording", on;
    config SaveRecordings). It's saved before a new recording or a load replaces it.
    - Faith: frames, places, time of day, weather, outfit, loading screens.
    - The people: frames as packed, world poses, fixed transforms, their skeletons' hierarchy, their kept draws' bone
      maps.
    - The guns', drones' and doors' kept draws; the moving models; the shapes.
    - The kept draws with every graphics card object they use: shader code, input layouts, states, samplers, vertex and
      index buffers, textures (every level and layer, read back from the graphics card).
    - The mission's bundles and the sound track.
  - **How:** new Camera/ReplayStore.h/.cpp.
    - Shader code is kept as the game makes each shader (the draw log's hooks), input layouts through a new
      CreateInputLayout hook (both from the first Present). A kept draw using one made before that can't be saved; the
      log counts them by kind.
    - Each part's bytes go to Recordings\Parts\<SHA-256>.part, once for all recordings. The recording's own file is
      `<folder>\Recording.ctr`.
    - The game's addresses (entities, bodies, pose managers, buffers) are written as keys. Loaded, they point into zeroed
      memory of the tools': the replay finds none of them in the level and plays it as one whose mission is over (models
      found again by name and place, people drawn with their kept draws or by stand-ins, the bundles loaded again).
  - **Smaller:**
    - Every section and part is packed with Windows' own LZMS compression (kept as it is when that isn't smaller).
    - Frame data (Faith, people, guns, sound) is written column by column, each value as its change from the frame
      before (lossless). In the offline test, smooth float frames packed 5 times smaller than plain LZMS (768 KB: 293 KB
      against 55 KB).
    - Meshes and textures shared by recordings are stored once.
  - **REPLAY > Saved recordings:** the list (newest first; length, size, what's in it) with Load and Delete (asks
    first). Deleting removes the parts no other recording uses. Also "Save this recording now" and the parts folder's
    size.
  - **Not saved:** her own sounds and the gun effects as the game played them (the sound track has what was heard), the
    mission items, the MagRope's line, the vehicles.

- **Replays: Faith in her recorded outfit, the asset log's textures slot by slot, what the replay drew, every recipe in
  the log** (the user, 2026-10-27, after the 20:23 test: "Build both: the log fixes (texture check slot by slot, which
  meshes the replay drew, every recipe listed) and Faith wearing her recorded outfit in playback"). Build 06E35055CF0E, staged with an install-at-close job (the game was running; game folder + dist, hash checked).
  **Not tested in game.**
  - **Her outfit:** while it records, what she wears (the tools' knowledge of it, GAME > Faith's) is kept each time it
    changes. While it plays she's dressed in it, the way GAME > Faith dresses her (the spawner's outfit entity's own
    Apply, at the server's next update), over the menu's choice. When it stops she's dressed back as the menu or the
    game has her. REPLAY tab: "Faith's outfit as recorded" (on; config RecordedOutfit). In the 20:23 test her Release
    outfit's meshes (faith_prologue upper and lower body, the gridlink) were missing from the whole playback.
  - **Textures slot by slot:** each draw's textures are kept by the pixel shader's slot, as they were described when
    drawn (an address can hold another texture later). A slot with a texture of a kind (format, shape, layers) the
    other time didn't have there is "other textures"; one whose kinds only changed their biggest size is "another
    resolution". Each line names the slots that differ.
  - **What the replay drew:** a list "Drawn by the replay itself" in ReplayAssets.log (when, how many draws, at how
    many of those seconds the game drew it too, when it was drawn while recording); the count and the first 40 in
    CT.log.
  - **Every recipe in CT.log:** each model part kept as the game draws it is logged (the log stopped after 30; the
    20:23 test kept 82, so whether door_breachsingle_01 had one wasn't known).

- **VISUALS > Color correction: Brightness, Contrast and Saturation are one slider each.** Before, each had three
  boxes (red, green, blue).
  - The slider shows the mean of the three. Dragging it, or Ctrl+click to type a value, moves all three by the
    same amount, so a tint the game set stays.
  - When the game's three aren't equal, their values are shown next to the slider (r g b).
  - Replays and the game's values are unchanged: still three numbers each.
  - Build 84d1f96a9d2a. Built without errors; not checked in game yet. Installed now as part of 49baf3b329a7
    (another session's build from the same folder; the change checked in it).

- **Replays: skinned doors drawn as models, guns found by their weapon meshes, and the asset log's textures compared draw
  by draw** (the user, 2026-10-27, after the 19:40 test: "build the fixes, doors captured as models, guns found by their
  weapon mesh names, and the asset log's texture check per draw"). Build F9F26E996F81, installed when the game closed at 20:20 (game folder + dist, hash checked). **Not tested in game.**
  - **Doors:** a recorded model whose mesh asset isn't a RigidMeshAsset (a skinned door) now has its shape kept like any
    other model: its asset's MeshSet, with the MeshSet's vtable checked. The 19:40 asset log showed
    door_glitchkey_oneway_right drawn 442 times as a model, by a shader that doesn't read bones. The game's own draws of
    it are now kept and drawn at its recorded places, as other models' are. A model's draw whose shader does read bones
    isn't kept as a model's (the skinned ones stay with the people's capture).
  - **Guns and batons:** they're known by the game's weapon meshes now, every mesh in the game's mesh table whose name
    starts with "weapons/" (cpf_shotgun, cpf_baton, ...), looked for again every 2 s while it records. In the 19:40 test
    the MeshSet at a gun entity + 0x258 wasn't one for any of 13 guns.
    - A draw of a weapon mesh goes to the nearest gun within 3 m (guns of a kind share a mesh) and is checked with a
      later draw of it, as before.
    - With no weapon mesh in the table, a gun is still found by how it moves (as the drone is).
  - **The asset log's textures, draw by draw:** a mesh's textures are compared for each of its draws (its first index
    and index count) drawn both while it recorded and while it played.
    - In the 19:40 test one vertex buffer was shared by many meshes, each with its own textures, so the whole buffer's
      textures never matched.
    - The report now has two lists: "other textures" (a texture of another format or shape) and "another resolution"
      (the same textures, only their size changed: a level streamed in or out). Each line shows the first such draw,
      recorded against played.

- **Replays: the recorded mission's bundles loaded again while it plays** (the user, 2026-10-27: "Load the bundle
  again (the 1:1 way, recommended) ... lets do this for the replay implement it"). Build 0E7C2BC4D4D3, installed with the
  game closed (game folder + dist, hash checked). **Not tested in game.** REPLAY > *Load the mission's bundles again while
  it plays*, on by default (`[Replay] ReloadMissionBundles`).
  - **While it records,** once a second it notes every sub-level bundle the game's bundle loader has in: a mission's, its
    approach and briefing, the areas around. With each it keeps how the game asked for it (name, kind, heap, parent,
    priority) and when it was in.
  - **While it plays,** the bundles that were in at the time played are asked for again through the game's own loader,
    8 s ahead, with the same request. They're let go 5 s after the playback is past them, and all of them when it stops,
    at a loading screen, when the option is turned off, or when the recording is forgotten.
  - The game's loader loads them as its own, so their meshes, textures and sound data are the game's own again. The
    mission's people and logic aren't made, and what the replay plays is still what it recorded.
  - **The start waits** for the bundles needed at the time played, at most 20 s.
  - Every game function used is checked byte by byte first: the loader's request, release and "is it in", the request
    maker, its lock, its update and the call to it. If anything differs, the option says it's not available. The
    replay's requests and releases are made in a hook on the loader's own update, on its thread.
  - CT.log says which thread the game's loader runs on and which one the replay runs on. ReplayAssets.log says the
    thread of each bundle call.
  - **Decompiled for it** (Research_Mission_Loading_2026-10-27.md sections 12 - 13):
    - which thread loads and unloads (the loader's update, in the game's frame update);
    - a mission's heap setting (every one of the city's 266 sub-levels: Parent, 0 KB, grow on);
    - the texture streaming manager ([0x142563A18], its pools);
    - which sounds load with their bundle (3,177 of the game's 4,990 sound waves: Persistent / DivisiblePersistent;
      1,659 Default ones load when played; 154 Divisible ones load when played and go when idle).

- **Replays: a log of what a recording loads and draws, and what its playbacks miss (diagnostic)** (the user,
  2026-10-27: "log every mesh / model that loads during the mission loading and check what misses on the replay", "and
  texture"; then the decompile of how a mission loads, Research_Mission_Loading_2026-10-27.md). First in build
  405108B2043C (18:43, installed with the game closed), with the bundles in build 2C630A679A55. **Not tested in game.** All of
  it goes to `Cinematic Tools\ReplayAssets.log` (written again at each recording), the totals and the first 40 missing
  names to CT.log.
  - **Bundles:** the game's bundles in its compartments as the recording starts, then each bundle the game registers,
    loads or unloads while it records or plays: name, compartment, parent and kind, the time, and whether a loading
    screen was up. These are log-only, byte-checked hooks on the game's bundle manager (0x142ABA450, 0x142ABA6E0,
    0x142ABA6B0); nothing is changed.
  - **Meshes:** once a second the game's mesh table is read. Each mesh that comes in or goes out while it records is
    listed by name, with the time and whether a loading screen was up.
  - **Textures:** each texture the game makes while it records that a shader reads, with its size, format and detail
    levels.
  - **Draws:** every 15th frame, which meshes the game's G-buffer pass draws (and the replay's own drawing there), and
    with which textures. A mesh is named by its vertex buffer.
  - **When playback stops,** the report lists:
    - each mesh drawn while it recorded at the seconds played that wasn't drawn then (missing), and whether it's still
      loaded;
    - each mesh missing at only some of those seconds;
    - each mesh drawn with textures of another size or format than when it recorded.

    Only the seconds the playback covered count. A camera looking elsewhere than the recording's misses what's out of
    its view.
  - **Also fixed:** a gun's or door's mesh buffers were read only up to its first level not loaded. A far mesh can have
    only its last level loaded, so they're now read for every level its data has.

- **Snow: bodies pressed in by their own meshes, KrugerSec boot prints, no snow crunch on glass or metal** (the user,
  2026-10-27: "Here is a list where the snow footsteps sound shouldn't be played Glass / Glass panes Metal objects", "The
  snow slide should be exactly faiths mesh instead of what it is now", "Ksec should leave behind snow footsteps and
  when they die and ragdoll they should leave behind their outline in the snow", "faiths snow handprints should be
  visible when she jump rolls / lands"). Build d3ce5c0cfefe, installed with the game closed (game folder + dist, hash
  checked; built from the whole folder, so it holds the other sessions' work up to then). **Not tested in game.**
  - **Glass and metal:** her steps in the snow on them keep the game's own sound. Those are sound ids 10 - 19 and 20 -
    29, every glass and metal wave of her footstep switchers; CT.log had the crunch on 11, 13, 24, 26, 27 and 29. The
    snow's print stays there.
  - **New Visuals/SnowBodies.cpp:**
    - Each frame Faith's body and the KrugerSec's within 40 m are read as the game poses them: their skinning matrices,
      read only. This is checked once per kind against their world pose; if it doesn't check, that kind presses nothing
      and the log says so.
    - Their own meshes, taken from the game's data with their skin weights (new Visuals/SnowBodyMesh.h), are put there:
      - Faith: her lower body, upper body and arms with her gloves, her default clothes.
      - KrugerSec: an Enforcer's full body, for every kind.
    - Each triangle down in the snow is pressed into a stamp, a top-down mask at 2 cm a texel (16 of them, at t4). The
      pass draws a stamp like a print (floor, inner wall, rim), and the snow fills it as it fills her prints.
  - **What presses:**
    - Faith's slides, rolls and hands where they come down are her real shape, in her pose.
    - Her legs and shoes press only while her hips are low; her steps have their own prints.
    - Her old slide trough and roll shape give way wherever her body pressed. They stay where it didn't (its pose not
      read, or not low), so nothing is lost if the read fails.
    - KrugerSec press while they lie (hips down by their feet: down, dead, a ragdoll), on their last step's ground, else
      on the ground their body rests on.
  - **KrugerSec steps:** a boot print each, the last 48 kept. The boot is an Enforcer's sole, measured from his mesh:
    28.4 cm long, with a heel block, a shank and a 9.4 cm ball; it has no tread, since that's in the textures. The toe's
    way comes from their foot's bones, else from their step before.
  - **Her landing handprints:** the game does put both hands down on a hard landing (FX_Hand_* beside every
    FX_FootLandHard_* in the logs). NoteHand's log line now has its own 30 s limit, so whether those touches arrive
    shows in CT.log.
  - WetParams grows to 43472, with static_asserts in step with fxc's listing. Faith's hips must be within 2 m of her
    transform, a KrugerSec's within 8 m of their place, or that frame isn't taken. Research section 18.

- **Replays, stage 2b fixed: guns, batons, the hunter drone and skinned doors found by their own mesh, never by a
  person's body** (the user, 2026-10-27, after the 17:51 test: "doors are models and icarus still doesn't have what he
  should and ksec are missing their guns now"). Build D09C70DEBC9D, installed with the game closed (game folder +
  dist, hash checked; it replaced 9ED5511B1106 and holds the other sessions' work up to 18:19). **Not tested in game.**
  - **Why the 17:51 test went wrong (its CT.log):** the door door_glitchkey_oneway_right took Icarus's body (its
    107556-index set) and five other models took KrugerSec bodies, because a draw nobody fitted went to the nearest
    shapeless model. None of the 12 guns fitted: their pose slot's matrices were 1.2 km from where the game drew them,
    or couldn't be read. The drone's draws were never kept: its shader skins with one bone a vertex, which the bone
    test didn't see.
  - **Bone-reading shaders** are found by their instructions now (a vertex input's index times 3, one bone or four),
    checked offline on all 2637 of the city's instanced vertex shaders: 912 found, all the right ones (the old test
    found 649).
  - **A gun's or door's draws are known by its mesh's vertex buffers** (a gun's mesh at its entity + 0x258, a door's
    through its data, each pointer's vtable checked). Their draws are kept every 10 frames, on top of the people's.
    One goes to the nearest gun or door of that mesh within 3 m (guns of a kind share one), never to a person, and the
    people's matching leaves them alone.
  - **Guns and drones are recorded as their whole world pose** each frame (26 bones: 832 bytes; the drone's 49: 1.6 KB)
    in place of the slot's matrices.
  - **Posed by how they move:** each bone of a kept draw is a recorded bone (a door: its transform) times a fixed
    transform. From the first draw, all of a gun's bones follow its bone 1 (where it is). When a later draw of the same
    part comes, the gun having moved, each bone is fitted to the recorded bone that moved the same way (within 4 cm).
    A set a later draw doesn't fit is let go (the wrong one of two guns of a kind), the gun given up on after 3.
  - **The drone** (where its mesh is isn't known): a skinned draw near it, no person's part, waits for a later draw
    of the same part, and is taken only if every bone of it moved and fits one of the drone's recorded bones.
  - **The log:** each gun's and door's buffers read (or not), each set kept, checked or let go, and at the end of a
    recording who has parts and who hasn't and why.
  - **Still not covered:** Icarus's hood / jacket (cloth simulated on the graphics card, its own step).

- **Replays: KrugerSec weapons, the hunter drone and skinned doors drawn with the game's own draws too (stage 2b); the
  log says who has none and why** (the user, 2026-10-27, after the 17:28 test: "Icarus missing his hood/jacket also
  doors and ksec batons are missing", "ksec hunter drone is also missing"; then "build stage 2b first ... and log why
  anyone isn't"). Build 4117666709B9, installed when the game closed (game folder + dist, hash checked). **Not tested
  in game.**
  - **Guns, batons and drones** (the recording's guns and drones): their skinning matrices are recorded as the game
    has them (their pose slot's matrices), one set with each stored frame. A gun's 26 bones are about 1.2 KB a frame;
    the drone's 49 about 2.4 KB. Their draws are matched to those, and posed from them when it plays, so their
    skeleton's hierarchy isn't needed: the hunter drone's was worked out with a loop. Anyone else is still posed from
    their skeleton (stage 2).
  - **Doors and other recorded models with no shape kept** (not rigid meshes: door_glitchkey_oneway_right,
    door_breachsingle_01):
    - A kept skinned draw that fits no person is matched to the nearest moving recorded model with no shape (within
      3 m).
    - Its bones are kept relative to the model's transform then.
    - When it plays, they're posed with the model's recorded transform while the game doesn't have the model.
    - Exact for a door the level moves as a whole. A door's own bone animation, if it has one, isn't recorded.
  - **The log at the end of a recording**, for each recorded person, gun or drone with no parts, which of these
    applies: its matrices weren't recorded; its skeleton's fixed transforms weren't read (how many tries); its
    hierarchy isn't known or has a loop; no kept draw was near it; or how many were near and how close the best came.
    The same for each moving shapeless model, and a total.
  - **Not covered yet:** Icarus's hood / jacket. It's cloth simulated on the graphics card
    (icarus_jacket_cloth_clothasset), not drawn through bones; it needs its simulated cloth recorded, a step of its
    own.

- **Replays: the recorded people drawn with the game's own draws of their own bodies (stage 2)** (the user, 2026-10-27:
  "build Stage 2, the recorded people (runners, KSec) drawn with the game's own draws of their own bodies, posed from
  the recorded animation"). Build DCADA6D3F1B0, installed with the game closed (game folder + dist, hash checked; it
  replaced A262CFE344AA and holds the other sessions' work up to 17:02). **Not tested in game.** Under the same option
  as stage 1 (REPLAY > ... > *With the game's own drawing of them*).
  - **While it records:**
    - Every 30 frames, the game's G-buffer draws whose vertex shader reads bone matrices (up to 64) are kept the way
      the models' are: the game's shaders, textures, buffers and states held, the constants and instances read back.
    - Each instance is handed over with the recording's time.
    - Each recorded person's skeleton is read while they're live: their pose manager, and their matrices' fixed
      transforms (the inverse bind pose), kept once two frames agree.
    - Each kept draw is matched to the recorded person whose matrices fit every bone of it, within 4 cm and 0.05.
      The matrices are made from the recording: bones 0 - 3 as recorded, the others from their parent, times the
      fixed transforms. The match looks at times from 0.2 s before to 0.05 s after (the frame drawn is a little
      older), in 1/120 s steps.
    - That also gives the skeleton bone of each of the draw's bones.
    - A person keeps the parts of one frame: the most detailed set seen (the most indices).
    - Faith's own draws, and draws that fit no one, are let go.
  - **While it plays:**
    - A recorded person who isn't in the level any more and has parts kept is drawn with them at the end of the G-buffer
      pass, with stage 1's machinery.
    - Their matrices are made from the recording at the replay's time; the header rows are as kept.
    - They're not borrowed nor given a stand-in. A live one is still played on their own body.
  - Not covered:
    - shadows;
    - their motion vectors;
    - see-through or forward-drawn parts (hair or eyes, if the game draws them there);
    - cloth simulated on the graphics card;
    - people whose skeleton's hierarchy or fixed transforms weren't read while they were recorded.
  - **Log:** a line for each person's kept parts (how well the bones matched and how far the frame was), and one
    when the first people are drawn.

- **VISUALS > Lights: position sliders** ("add a position sliders that you can move the lights"). Under "Place" for
  the picked light:
  - "Position": its x, y (up) and z in the world, in metres; drag a number or Ctrl+click it to type one.
  - "Move": three boxes to drag left or right. They slide the light from where it is to the camera's right, up, or
    away from the camera (level with the ground, so it doesn't go into the floor when the camera looks down). Each
    goes back to 0 when let go.

  In the replay session's DCADA6D3F1B0 (17:02), then in build c7b70077d0b8 from the same folder, installed 17:05
  with the game closed. **Not checked in game.**

- **Replays: gone people are played only by the same model (1:1)** (the user, 2026-10-27, after the 16:35 test: "ksec
  models are almost correct runner models are still incorrect/missing check if the 'borrow spawner' has to do anything
  with it if so remove it since we are doing a 1:1"). Build A262CFE344AA, installed when the game closed (game folder
  + dist, hash checked); in DCADA6D3F1B0 too. **Not tested in game.**
  - **What the test showed:** the borrowing was it. The two Runner_02 runners were played by city men (HighCaste02 591 m
    away, Black_November_02 905 m away) taken as "a spawn template of the same kind". Two Protectors got Sentinel
    stand-ins and an Enforcer01 an Enforcer02, from a spawner "of the same kind".
  - **Now:**
    - A gone person is played only by a live one with the same entity data, or else the same spawn template.
    - Its stand-in is asked only of a spawner of its own template. Spawners "of its folder and kind" aren't asked
      any more, and a person with none of its own left isn't shown. The runners, with no runner left in free roam,
      aren't shown until the people are drawn with the game's own draws (stage 2).
  - Guns are still played by live ones with the same data (the same gun). Icarus is still played by the lair's Icarus
    (the same template).
  - The same test showed stage 1 working: 71 recipes kept while it recorded, 709 parts drawn with the game's own draws
    when it played; the G-buffer was bound at the pass's end every time (no warning).

- **No bright fringe round Faith as she moves in the rain, and she doesn't get brighter** (the user, 2026-10-27, with a
  screenshot: "there is still a weird halo when faith moves in the rain", "she also gets brighter"). Build
  a262cfe344aa (another session's 16:45 build of the shared code, with the snow session's shoes/hands/gravel round;
  checked: its wet-look code compiled after these shaders, all 7 blobs, no source newer); installed (game folder + dist,
  hash checked).
  - Letting her own splash show through, the floor in it was compared with the frame round it blurred 64 - 128 px.
    That included her dark legs and hands, so the floor beside her and she herself counted as brighter and were put
    back as the game drew them, dry and light: a jagged bright fringe round her outline while she stepped in water,
    and she got lighter. Now the floor is compared only with the floor round it on its own plane, and she and people
    are left out.
  - The soft outline (the halo fix before) also made the floor's pixels next to her partly dry, a thin light line round
    her on the dark wet floor. Now only her own edge is softened.
  - **Not checked in game.**

- **Replays: the mission's models drawn with the game's own drawing (stage 1)** (the user, 2026-10-27: "build Stage
  1, the recorded mission models redrawn with the game's own shaders, textures and data at the end of its G-buffer
  pass while the replay plays"). Build 602DC7DE8A22, installed when the game closed (game folder + dist, hash checked).
  **Not tested in game.**
  - New Camera/ReplayGameDraws.*. Option REPLAY > Doors, elevators... > Draw the ones a mission took > *With the
    game's own drawing of them*, on by default; config `[Replay] GameDraws`.
  - **While it records:**
    - The first draw of each part (shape section) of a recorded model in the game's main G-buffer pass is kept as a
      recipe. The pass is known by its 4 targets in their formats and a depth target the frame's size.
    - Its shaders, input layout, vertex and index buffers, views, samplers and depth, blend and raster states are
      held by reference.
    - At the end of that pass, the game's buffers its constants and instances are in are copied on the graphics card.
      That's the next render-target change on its immediate context, now hooked: OMSetRenderTargets and its UAV form.
    - The copies are read back 3 frames later: its b0 / b1 windows (D3D 11.1 offsets into the game's 1 MB pages), its
      instances, and which rows are the world matrix. Those are 3 rows whose axes are at right angles, within 600 m
      of the camera.
    - A part whose data can't be read is tried again (3 times). Draws whose world matrix is in b0 (not instanced) are
      left as shapes.
  - **While it plays, at the end of the game's main G-buffer pass (its targets still bound):**
    - Each part of a model the game doesn't have is drawn with its recipe, one instanced draw per recipe: the game's
      shaders, buffers, textures and states.
    - The tools supply their own b0 (the recipe's, its offsets pointing into the tools' instance buffer), b1 (a copy),
      and b2 (this frame's camera, copied at the pass's first draw).
    - Each model's own instance as recorded (picked by its place, so its lightmap values are its own) has its
      recorded transform written into the world rows.
    - Every state changed is put back as it was, the constant buffers with their 11.1 offsets.
    - The game then lights, fogs and reflects them as its own. Those parts aren't drawn as plain shapes then.
  - Not yet drawn this way: shadows, the motion of moving ones, see-through parts, and parts the game only drew in
    other passes (decals). They stay shapes or missing.
  - Nothing of the game's is written: D3D calls on its context, the tools' own buffers.
  - Unknown until tested:
    - whether the end-of-pass point is where the draws can go (the log says if the G-buffer wasn't bound);
    - how much video memory the held textures take;
    - whether any game state is missed when it's put back.

- **Replays' draw log: the snapshot copies the constant ring; state logs spread over the passes** (after the user's
  15:59 test of the draw log). Build 3F7E87C71817 (16:04, from the folder, so with the other sessions' work in it),
  installed when the game closed (game folder + dist, hash checked). **Not tested.**
  - The test showed b0 and b1 are windows (D3D 11.1 offsets) into one 1 MB dynamic buffer. The snapshot copied
    constant buffers only up to 64 KB, so no draw's instances were decoded. Now buffers up to 2 MB are copied (about
    48 MB of staging for the 24 draws, freed after the read).
  - All 48 state logs went to the G-buffer pass in the first second. Now there's one per vertex shader, pixel shader
    and pass, at most 24 per pass and 120 in all, so the shadow, motion and forward passes are logged too.
  - What the test settled is in Research_Replay_Mission_World_2026-10-26.md section 19.

- **Replays: a draw log, stage 0 of drawing a recording with the game's own draws (diagnostic, log only)** (the user,
  2026-10-27: "build the log-only Stage 0 (constant buffer binding, the instance buffer, the G-buffer pass's targets
  and depth test, shader names via the device's create hooks, characters' draws), drawing nothing"). Build
  74C52B5AAB24, installed when the game closed (game folder + dist, hash checked). It holds EFAC0F682C4C's
  mission-world hold. **Not tested in game.**
  - New Camera/ReplayDrawLog.*, fed by the replay's existing draw hooks (ReplayShapes.cpp); now also hooked while it
    records with no shape captured. Its lines start "Draw log (diagnostic):".
  - **The game's shaders by name:**
    - At the first Present, the device's CreateVertexShader and CreatePixelShader are hooked (vtable slots 12 and 15).
    - Every shader the game makes after that is noted by its object: a hash of its code, its bindings (slot and
      name), its constant buffers' sizes, where dxVectorBufferOffsets sits in its functionConstants, a pixel shader's
      number of outputs, and whether a vertex shader reads bone matrices.
    - The bone test (the code holds the immediate l(3, 3, 3, 3), bone index x 3 rows) was checked offline on the
      city's 2637 instanced vertex shaders: it matches all 649 that fxc shows doing that multiply, and none of the
      others.
  - **While the replay records, once a second (one frame in 60):**
    - every indexed draw is counted by its pass (its render targets and depth target);
    - a draw of a recorded model, or one whose vertex shader reads bones (outside depth-only passes), has its state
      logged the first time it's seen with those shaders in that pass (at most 48): context, constant buffers with
      their D3D 11.1 offsets, the instance buffer view and buffer, the pixel shader's views, input, depth test,
      blending, culling, viewport;
    - every 10 s, the passes are listed with their draw counts.
  - **Once, 5 s into a recording:**
    - On the first context that draws one of those into 3 or more targets, up to 24 of them have b0, the first one's
      b2 and the instance buffer copied on the graphics card. The copies go into staging buffers of the tools', in
      the game's command list right at the draw; instance buffers of 4 MB or less at every draw, bigger ones (up to
      32 MB) at the 1st, 8th, 16th and 24th; at most 160 MB in all.
    - Three frames later they're read back on the render thread and logged: the camera position, each draw's start
      and stride, its first 4 rows, its first 4 bones' places, and for a bone-reading draw the recorded person
      nearest its first bone (FaithReplay notes the people's places then).
  - Nothing is drawn, no state of the game's is set, nothing of the game's is written: only D3D Get calls, copies
    into the tools' own buffers, and their reads.
  - Unknown until it runs: whether shaders made before the first Present (none expected) go unnoted, whether copies
    into staging buffers on the game's deferred contexts work there (if not, the snapshot shows zeros), and the cost
    in the sampled frames (several Get calls per draw).

- **Wet Faith and KSec smooth (no triangles); no haze flicker on buildings' windows; no wind in the shut lair** (the
  user, 2026-10-27: "You can see every triangle of wet faith/ksec find a way and smooth it out", "There is still some
  building flickering from haze/fog when looking them at a 45/50 degree angle", "When lair windows are closed there
  shouldn't be any wind inside the building"). Build 13dad25305a7 (another session's build of the shared code at
  15:46, checked: these changes, all 7 shader blobs, no source newer); installed in the replay session's 74c52b5aab24
  (game folder + dist, checked: these changes and the 7 blobs).
  - **Triangles:** the wet bodies' gloss came from the frame's normal, which comes from the depth and is flat over
    each of the model's triangles, so each caught its own patch of light. Now the gloss is the game's own light on
    them made brighter and sharper where it's brightest (their colour cubed): it follows the game's smooth shading.
  - **Haze flicker (from the code; not seen):** the haze leaves alone what's much brighter than the frame round it (the
    HUD, markers, effects). Seen at 45 - 50 degrees a building's windows mirror the sky, and from frame to frame they
    crossed that line and back, the haze on them jumping between all of it and 15 %. Now the haze's own test reads the
    frame a little blurred (a pixel's glint can't count) and only counts what's plainly brighter or plainly coloured.
    The snow's and the ice's test is unchanged.
  - **The lair's wind:** in the lair (the game's own outline of it) with GAME > Close the lair's open windows ticked,
    the wind's sound fades out over a second, and the game's own wind (cloth, dust) isn't raised with the rain there.
    The log says when the camera goes in and out. The tick counts, not whether the sashes were found and shut yet.
  - **Not checked in game.**

- **Replays: the recorded mission's world kept after the mission is over** (the user, 2026-10-27: "build the hold on the
  recorded mission's stream-out (byte-checked hook on 0x143A7A200), with logging, and letting go before loading
  screens, when the recording is forgotten, and from a menu button"). Build EFAC0F682C4C, installed with the game closed
  (game folder + dist, hash checked). It replaced B94EB4BE68C6 and holds its work (built from the shared folder after
  it). **Not tested in game.**
  - **Why not just 0x143A7A200:** that function (the mission's stream-out once it's over) streams the world out at once
    only when nothing makes it wait. With a delay (every Act 1 mission: 15 - 23 s) or a radius, it only notes the time or
    Faith's place; the mission's update sends the StreamOut later, from other places in its code. So 0x143A7A200 is
    hooked for the log, and the world is kept where every one of those sends ends: the mission world's own event handler
    (ServerSubLevelEntity slot 4, 0x14335EFB0). The mission calls it from 21 places, and nothing else calls it.
  - **What's kept:** a StreamOut sent to a mission's world (noted from those 21 calls, or as the world of the mission
    0x143A7A200 is called for) while the replay records, or while it keeps a recording and the world wasn't streamed in
    again after the recording stopped. Stream events sent to that world later in the same server frame are kept too, in
    order (the mission asks for its world back at once when it's available again). The mission itself goes on as if its
    world had gone.
  - **When it goes back to the game:** the kept events are sent as the mission made them, in order, on the server's
    thread, when:
    - a loading screen shows;
    - the recording is forgotten, a new one starts, or the recording stopped too short to play;
    - REPLAY > *Keep the mission's world after it's over* is unticked;
    - REPLAY > *Let the mission's world go* is pressed (shown with how many worlds are kept);
    - the game sends that world a stream event later (it wants it again: the kept ones go first).
    A kept world the game destroys itself (the level it's in unloading) is forgotten (its destructor, slot 7, is
    hooked), and the log names the thread.
  - **Log:** every kept world (where its StreamOut came from), each event kept after it, each let-go (why, how long it
    was kept), and for each mission that's over: completed or aborted, its worlds, and when the game would stream them
    out (at once, by radius, or at a game time with its delays). If the code isn't what the tools know: one warning, and
    nothing is kept.
  - **Byte checks (all checked offline against the game image before building):**
    - the handler's event tests and its StreamOut branch;
    - the sub-level's vtable slots 4 and 7, its destructor, and the vtable the destructor restores;
    - the event's vtable and its 16-byte copy;
    - 0x143A7A200's prologue, its one call (0x143A7736D), and its StreamOut send;
    - all 21 of the mission's calls of the handler.
  - Config: `[Replay] KeepMissionWorld` (on by default).
  - Unknown until tested: whether the mission's end waits on this stream-out; what of its logic runs on in a world it
    thinks is gone; doubled places where the mission's world and free roam overlap; the memory a kept world costs.
    Research_Replay_Mission_World_2026-10-26.md section 15.

- **Puddles no longer vanish when looking ahead; the same at any resolution; wet Faith and people plainly wet; the
  water down slopes in rivulets at its slope's speed; softer edges; fog and haze sliders further** (the user,
  2026-10-27: "wetness is now disappearing when i look ahead only when i look down they appear", "It should be the same
  no matter the screen resolution", "ksec and faith still don't look wet under rain", "improve the water sliding down
  slopes shader it still looks realy bad", "water down slopes should have different speed types depending how steep the
  slope is", "the shader should be more smoothed out around the edges", "lets increase the fog/haze sliders"). Build
  b94eb4be68c6, installed with the game closed (game folder + dist, hash checked).
  - **The bug (in the build before, db82ecc84b91's puddle edges):** points a few pixels round a pixel took the depth of
    the pixel they fell in but were put on the ray through where they were. Looking along the ground the depth changes
    fast from pixel to pixel, so they came out a few per cent under the ground's plane, a drop, and no puddle (nor its
    mirror) within about 6 m. Looking down the depth hardly changes, so it didn't show. At 3840 pixels across the ring
    fell on whole pixels; at the 3406 of this test, between them. Now every such point is taken on its own pixel, and
    the tests' reach is a share of the frame's height: the same at any resolution and window size.
  - The soft outlines (the halo fix) now only work at real outlines (a 25 % jump in distance, was 5 %): the far ground
    counted everywhere, and the wet look's pass cost about 7 ms on the GPU instead of about 4.6.
  - **Faith and the people out in the rain:** x 0.68 darker at full Wetness (was 0.86), their colours a little deeper,
    a glossy sheen on what faces up and, softer, where it turns away from the view (not on their outlines).
  - **The water down slopes:**
    - fine rivulets along the way down (the pattern smeared along it), about 20 cm long on a gentle slope to 60 cm on a
      steep one, no longer round blots;
    - its speed by the slope: a slow sheet under about 18 degrees (0.35 m/s), a steady flow on a middling one (about
      1 m/s), running fast down a steep one (up to about 2.5 m/s);
    - the ground under it darker all over, not in patches; glossier; the rivulets catching the light on their sides;
    - the layers' hand-over slower (every second, not 0.6 s).
  - **Softer edges:**
    - The slope water fades out over several pixels before a crease or a drop, no longer ending in a line.
    - Puddles' shores are twice as soft. Her steps' puddle check uses the same shore.
  - **Fog and haze sliders:**
    - Rain Haze up to 5 (was 2); Snow haze up to 8 (was 3); past their old most the haze also hides more of what's far
      off (up to 97 %).
    - Rainy look's Fog: how thick up to 25 (was 10); Snow fog up to 25 (was 10).
    - Fog: seen through up to from 20 m (was 100).
  - Checked: it builds; the DLL holds all 7 shader blobs and the new menu texts. **Not checked in game.**

- **GAME > Detail: the *Make them solid* tick is gone; *Close the lair's open windows* makes them solid itself.** The
  user, 2026-10-27, after testing build 553D01D84132: "Yes it works perfectly now i don't think we need the tick
  'Make them solid' anymore". Built in 767D14E58CB9 (staged), and in another session's later 741380625565 (14:35:07,
  from the folder), which went in when the game closed (14:41): **installed in 741380625565** (game folder + dist).
  Checked in both DLLs: the tick's text and its config key are gone, and the Havok move
  and the new help text are there.
  - Ticked, both sashes are shut and solid, as with both ticks before: the drawn model, the game's body records, and the
    bodies in Havok. The 4 m clearance round Faith stays, so a sash's collision never swings into her; a nearer one is
    shut drawn-only and turns solid once she's clear. Unticked, as before: the sashes and their bodies go back where the
    game had them (a solid one once Faith is clear).
  - If the game's physics code isn't what the tools know, the log warns once and the windows are only shut, drawn.
  - Config: `[Misc] LairWindowsSolid` is no longer read or written; `[Misc] CloseLairWindows` is the only key.
  - The help text says the shut windows are solid and explains the 4 m wait.
- **Replays: the mission's models drawn with their colour textures** (the user, 2026-10-27: "trace the mission models'
  materials to their textures and draw the replay's shapes with their real colour textures"). Build F9603D1F1B36,
  installed with the game closed (game folder and dist, hash checked). It holds the work of the exit-hang and
  lair-windows sessions too (built from the shared folder after theirs).
  - **Where the textures are:** not in the meshes' own data in this game. A mesh asset's Materials (+ 0x60, MeshMaterial:
    its shader instance and a few parameters such as DiffuseColor or PatternTiling) name no textures. The game binds
    them when it draws. So they're taken from its drawing.
  - **While it records:** the game's indexed draws (DrawIndexed and DrawIndexedInstanced, the immediate context's and a
    deferred context's own functions, hooked the first time a recording keeps a shape) are looked at. A draw with a
    captured level's vertex buffer, its index buffer, and a section's first index and count has its pixel shader's
    texture views read. The first 2D one in an sRGB colour format becomes that section's colour texture, held so it
    stays after the mission is over. The log shows the views seen for the first 24 kept (and for 12 draws that had none).
    When the recording stops it says how many sections got one, and the draws aren't looked at any more.
  - **While it plays:** a section with a kept texture is drawn with it, using its first texture coordinates. The sRGB
    view is read in linear light and written back in the frame's gamma, lit from above as before. One without stays
    light grey. The game's lighting, its normal maps and a material's tiling aren't applied.
  - Checked: it builds with no warnings in the changed files. **Not checked in game:** that the game draws these meshes
    through those two calls, that its colour textures are sRGB views, and how they look.
- **MECMP works with the tools loaded again; a stuck quit ends sooner** (the user, 2026-11-01: "game still hangs after
  quiting even with the safeguard", "check if it prevents MECMP from hooking", "Ensure that MECMP always works even with
  the tools loaded up").
  - MECMP checks a few game functions' first bytes when it loads and stays off on any mismatch. The replay's hook on
    the models' setTransform body (0x1432EB1F0, SpatialEntity::setTransform to MECMP) was one of them (mecmp.log
    2026-10-30). That hook is now off except while a replay holds actors, and never while MECMP is in the game. None
    of the tools' other 101 hooks touches what MECMP checks, and the tools change no vtable slot.
  - usp10.dll (the tools' loader) checked: the same 44 exports as Windows' own, forwarded the same way; it only loads
    the tools.
  - The stuck-quit end no longer needs Faith's level gone or no loading screen; it ends the game 8 s after the
    server's shutdown stops moving on the level unload, 1 s after the window is asked to close during that, or 30 s
    into any quit. What the quit is at goes in CT.log every 5 s.
  - Build c2c1fb6460bf, installed. Not tried in the game yet.
- **A stuck quit ends the game** (the user, 2026-10-28: "make the tools end the game when its quit is stuck
  (quitting, level gone, server waiting on the level unload for 15 s), the same way the game's own watchdog does").
  Build de80f12cba0e, installed. When the quit has been asked, Faith's level is gone, no loading screen is up and the
  game's server has waited 15 s on the same unfinished level unload (a resource manager ticket its queue hasn't
  reached), the tools log why (`Exit: the game's quit has been stuck ...`) and end the process with exit code 42,
  as the game's own watchdog does after the window is closed. Why that unload never finishes is still not known
  (Research_Exit_Hang_2026-10-27.md). Not tried in the game yet.
- **The game's end in CT.log, and a report of where it's stuck when it freezes** (the user, 2026-10-27: the game
  doesn't close after quitting; Windows closed its frozen window at 01:07 and 02:59). Build 553d01d84132, staged (the
  game was running): it installs itself once the game closes. The cause of those freezes is not known yet: CT.log
  had nothing from a session's end, and no dump was made. Details: Research_Exit_Hang_2026-10-27.md.
  - CT.log now has `Exit:` lines: the window asked to close, the window closing, the tools stopping (and how long
    that took), the process ending (DllMain), the tools' objects gone. The last one there shows how far the end got.
  - When the game's window hasn't answered Windows for about 15 s (and again 30 s later), or the game still runs
    20 s after its window closed, the tools write `Cinematic Tools\Hang_<date>_<time>.txt` / `ExitHang_...txt`: every
    thread's address, processor time, start and the code addresses on its stack (read only, nothing suspended), and
    a `[WARNING]` line in CT.log with the window thread's place. At most 4 a session.
  - The tools no longer free their objects once the game's window has closed (that ran their destructors while the
    game was still closing: values put back in game memory, the menu's D3D objects released as the game took down
    its device, and Main freed while the game's threads could still be in a hook that uses it).
  - Quitting while a replay's sound track records or plays no longer ends the game with std::terminate (the two
    threads' std::thread objects were still joinable when the process ended).
  - 15:45 the same day the new lines showed a stuck quit: the game took its level out and kept running (window
    answering) until the window was closed 3 minutes later, which froze it; a working quit ends the process about
    5 s after the level goes. Build 6abd543b5ee3 (installed) also writes `LevelGone_<date>_<time>.txt` 20 s after
    the level went with no loading screen, and `Close_...txt` 2 s after the window was asked to close if it hasn't,
    each with the level streaming's state in CT.log (`Exit (diagnostic):`).

- **No halo round Faith in the wet look; Faith and KSec a little wet; better puddle edges; Random rain with its own
  weather in each spell** (the user, 2026-10-27: "Improve wet faith look implement it better because there is a halo
  around her", "ksec should be a bit wet under rain", "make better puddle edge detection algorithms", "random rain
  doesn't work properly it should A) rainy city look at random values B) thunder and lighting randomly C) wetness
  should be random and depending on the amount of rain as well"). Built in db82ecc84b91; the newer 553d01d84132 (04:30, with the game-exit
  fix too, checked: this change, all 7 shader blobs) is installed (game folder + dist, 04:4x, hash checked).
  - **The halo's causes (from the code; no screenshot of it):**
    - A puddle ended wherever any point 4 pixels round was off its plane either way, so every puddle stopped 4
      pixels short of whatever stood in it or was in front of it: a ring of lighter, unmirrored ground round her legs,
      as plain as Wetness made the puddle.
    - Along her outline the game softens the colour over a pixel or two (antialiasing) while the depth stays sharp:
      the pixels showing some of her over the ground were darkened and mirrored as ground (a dark rim), those showing
      some ground inside her outline were left dry (a light one).
    - In a puddle the wet look blends in the frame a few pixels blurred, which bled her colours into the water round
      her.
  - **Now:**
    - A puddle ends only before the surface drops away (an edge, a ledge, a curb's top, a hole, a step down, the sky
      past a roof's edge), thinning towards it. What rises from it (a wall, a curb up, a post, Faith or a person
      standing in it) doesn't end it: the water lies up against it. Her steps' puddle check uses the same test.
    - What's kept dry has its outline as soft as the frame's.
    - The puddle's blur leaves out what stands in it.
  - **Faith, KSec and the other people out in the rain** are a little wet instead of left exactly as drawn: darker
    (x 0.86 at full Wetness) with a faint sheen on what faces up. Not under a roof, and no puddles, rings or mirroring
    on them. Cars and trains are still left as drawn.
  - **Random rain**, each spell its own weather from the forecast:
    - how hard it rains, a drizzle to a downpour; the rain, its sound and wind, the puddles go with it;
    - how much of the rainy city look, its fog 0.6 - 1.5 x as thick, the sky 65 - 100 % as grey, 55 - 100 % as
      sunless;
    - whether it storms (about 15 % of drizzles, 70 % of downpours), for which part of it and how often.
    - The ground gets wet as it rains (the harder, the faster and the wetter: a downpour soaks it in a minute, a
      drizzle only to 60 %). Puddles fill only in heavier rain (full in about four minutes of a downpour). Both dry
      after it (about 10 and 20 minutes). Wetness and Puddles are the most it can reach.
    - The menu shows the spell's weather and how wet the ground is now.
    - Thunder and lightning ticked itself still comes whenever it rains. Random snow is unchanged.
  - Checked: it builds; the DLL holds all 7 shader blobs and the new menu texts. **Not checked in game.**

- **Replays: the mission's own buildings and bus, Icarus as Icarus, and no more ghosts left in the city** (the user,
  2026-10-27 03:5x: "release doors are the correct ones but theres a lot of mesh and missing textures from dynamic
  meshes that only the mission spawns also use icarus model and bus model is also missing", then "there are random
  beat ghosts around the freeroam city map when they shouldn't"). Build 5F3671C94F27, staged (the game was running); it
  installs itself once the game closes, only over an older DLL.
  - **What the 03:44 log showed:** 23 shapes drawn, the gates included. Only models that moved were recorded, though,
    so the bus area's own mission meshes (csg_area00_release_busarea and the rest, which never move) weren't, and
    neither was the bus. The bus is a plain static model (objects/props/citybus/nonpamp_citybus_01_mesh) in a prefab its
    timeline moves (animations/prefabs/missions/release/pf_anim_release_bus), and it wasn't seen moving within 40 m.
  - **Now:** every static model within 60 m of Faith while it records (it was 40) is in the recording with its shape,
    moving or not (one place for those that don't move). A recorded model the game doesn't have when it plays is drawn
    by the replay; one that never moved is otherwise left to the game. Up to 2048 shapes and 384 MB. The log gives the
    totals.
  - **Icarus:** a runner, earlier in the list, took the free-roam Icarus as "one of his kind", and the recorded Icarus
    got a Black November man. Now everyone gets the same data or template first, then the rest.
  - **The "draws it itself" fallback:** it also counted bodies the replay parks out of sight just before their time
    (six at about 10 km). Now it only counts within each person's recorded time.
  - **The ghosts:** the Release mission's red Runner Vision guides (SpawnTemplate_Projection_Replay) were recorded as
    people. Once the mission was over, the only spawners of that template were the city's social replay markers
    (gameplay/social/pf_skillreplaymarkerwithautoplay and pf_replaymarker). The replay's 6 stand-ins for them came from
    those markers, and that set the markers' own ghosts off: 21 Runner_Projection sounds as the replay stopped and
    took its 6 out. Now no stand-in is ever made of, or taken from, a runner projection or Faith's own template, and a
    recorded projection is simply not shown once it's gone. The ghosts already set off in a running game should go
    with a restart (not checked).
  - **Not done:** textures (the shapes are still one grey).
  - Checked: it builds with no warnings in the changed files. **Not checked in game.**
- **GAME > Detail: the *Open the Shard station's gates and windows* tick is gone.** The station's 8 gates, two side
  windows and exterior window always look as the game has them, which is what the tick's default (off) did, and
  what the config had. The config key `[Misc] OpenShardStation` isn't read or written any more. The code that hides
  them is still there, unused. Build 90281ce92095, installed when the game closed (hash checked). Built without
  errors; not checked in game yet.

- **Replays: the mission's doors from the graphics card, Icarus where he was** (the user, 2026-10-27 03:2x: "read the
  log the doors are missing icarus is missing as well and the guy in front is not a runner"). In the game as part of
  90281CE92095 (another session's 03:35 build of the shared code, installed when the game closed; checked to hold
  these changes). Built here as 09B766B79288, which wasn't installed itself (the newer build has it all).
  - **Doors, what the 03:21 log showed:** of 23 moving models only 3 shapes were kept (the bus's door, the lens and its
    cover). The rest, the three gates included, were refused: "none of level 0's sections has a position the replay
    can draw". Read live: the meshes are fine, but the bytes the game keeps beside each buffer (+ 0x58) don't stay
    there. A car's indices read right, then a minute later past its vertices: the memory goes back to the game once
    the level is on the graphics card.
  - **Now:** while it records, each moving model's two buffers are taken from the graphics card. They're held (one
    reference each), copied on the graphics card into the replay's own buffers at the next frame, then let go.
    Sections with no material name (a depth-only copy of the positions with indices of its own) or named Shadow (a
    car's flat shadow) aren't drawn.
  - **Icarus, what the log showed:** he was played by the free-roam Icarus (the same spawn template, in the Runners'
    Lair 893 m away), but his body stayed in the Lair. Posed by the replay at every check, it was 893 m from the
    replayed place, while every other borrowed body was on it.
  - **Now:** a posed body that stays more than 2 m from the replayed place for 30 frames in a row is drawn by the
    replay itself, the way taken-down KrugerSec are: its pose written each frame and its client entity moved with it.
    The log says "posed, but the game keeps its body where it really is".
  - **The runner:** played by a live man of the city ("a spawn template of the same kind", 591 m away). After the
    mission, no spawner or live person has the runners' template. The log now names the template of whoever plays a
    gone person.
  - Checked: it builds with no warnings in the changed files. **Not checked in game:** the copies from the graphics
    card, the doors drawn, and Icarus drawn by the replay.
- **Faith's own water splash in the puddles, seen** (the user, 2026-10-27: "also on puddles use faiths splash fx
  when she steps on in-game puddles"). Built in a6bf815a08e2; installed in the lair-windows session's later build
  90281ce92095 (03:35, game folder + dist), checked: it holds this change and all 7 current shader blobs.
  - **What was there (checked in CT.log, 2026-10-27):**
    - The game's own puddles (objects/decals/waterpuddle, 1999 in the city) collide as its wet concrete, whose
      effects are the water's. Every water splash the game played at 02:16 - 02:17 (23 steps, a slide, a landing)
      was on one of them.
    - The tools' puddles already played the same effect (FX_Foot_Water and the landings'). At 02:45:10 a hard
      landing on gravel in a tools' puddle played FX_FootLandHard_Water at the very spot.
  - **Why it didn't show:** the splash doesn't write the depth, so the wet look, drawn over the finished frame, took
    it for the ground under it and darkened, blurred and mirrored over it, leaving about a quarter of it.
  - **Now:** for each of her steps and landings whose effect is the game's water splash (the tools' puddles and the
    game's own), the wet look lets what's bright round that spot (the splash's white sprites and drops) show as the
    game drew it, for 0.8 s (a landing's bigger and longer). The tools draw no little splash of their own for those
    steps; KSec's, other people's and the cars' keep theirs (the game has no splash effect for them).
  - The lair's puddles (its after-the-raid zone) gave no splash: that zone is probably not loaded **[not known]**.
  - Checked: it builds; the DLL holds all 7 shader blobs and the new log line. **Not checked in game:** how much of
    the splash the brightness test catches close to the camera; whether bright paint right by her foot looks dry for
    that moment.

- **Fixed: the lair's shut windows weren't solid** (the user, 2026-10-27 03:4x: "they are not solid and faith can walk
  through", both the window by the curtain and "the other by the sofa"). Build F3A61143602C (03:58:49, another
  session's build from the folder after this change was saved; its strings checked), staged (the game was running):
  it installs itself 60 s after the game closes, only over an older DLL. The replay session's later 5F3671C94F27
  (04:06:24, also from the folder; its strings checked) holds it too and is staged in dist. When the game closed
  (04:40) a newer build from the folder, 553D01D84132 (04:30:17), was in place, also holding it (strings checked):
  **installed in 553D01D84132** (game folder + dist). Not checked in game.
  - **Found, live and read-only (03:5x, the lair loaded):** the tools' move reached the sash's drawn model and the game's
    record of its bodies (record + 0xC8 points at the model's shut transform), but **not the bodies in the game's
    physics (Havok)**. The curtain window's body 374 was still swung 45 degrees, (0.195, 0, -0.981), and the sofa
    window's body 2054 still turned 129 degrees, (0.999, 0, -0.015), matching the data's turn. Their effect-world
    copies (254 / 1715) were the same. So the game's path from a moved model to its collision (the physics component's
    slot 30) only updates the spatial queries (record + 0x40), and nothing in the game moves a Fixed body after it's
    made.
  - The sofa window is the "combat area" one. Its server copy is shut in the game's data and Faith still walked
    through it, so she moves by the client's physics. That's the copy the tools now move.
  - **Now:** with *Make them solid*, each of the sash's bodies is also put at the shut rows in Havok, the way a body
    record's own slot 48 (0x1434F1DF0) does it for a body it doesn't keyframe: hknpWorld::setBodyTransform
    (0x143F3A6B0, activation 1). A body is only moved if it's where the model is (within 0.5 m). Unticking puts the
    bodies back where the game made them. The game's code is checked first (the call's start, slot 48's call to it
    with activation 1, how slot 48 finds the body's world and id, the body lookups, the vtable's slot). If it isn't
    what the tools know, the log warns once and the windows are only shut, drawn. Faith's 4 m clearance is kept.
  - The log's lines now say how many Havok bodies moved ("...; 2 Havok bodies moved").
  - **Not verified:** that the call is safe from the camera hook. The tools' camera collision already queries this
    physics world from there; this writes to it. Also not verified: that Faith is then stopped there.
- **New: Make them solid** (under *Close the lair's open windows*; on by default; config `[Misc] LairWindowsSolid`).
  The user, 2026-10-27 (asked in the replay session): "can we make the windows have collision?". Built in
  A6BF815A08E2 (03:24), and in the replay session's later 09B766B79288 (03:32:30, from the same folder; its strings
  checked). When the game closed (03:37) a still newer build from the same folder, 90281CE92095 (03:35:23), was in
  place; it holds this change too (strings checked). **Installed in 90281CE92095** (game folder + dist). Not checked
  in game.
  - **What the data says (offline, read-only):** each sash has two rigid bodies, both `RigidBodyMotionType_Fixed`
    (value 1). One is in `Realm_ClientAndServer` (2), the other `Realm_Client` (0). Its physics component is in both
    realms. The enum values were read from the game's own enum tables. The story's swap of the window by the curtain
    is sent to the server (`EventConnectionTargetType_Server`, 3) and streamed in both realms (`StreamRealm_Both`).
    The broken sash's turn comes from a `Realm_Client` PropertySelect, so even in the game itself its server copy
    stays shut.
  - **What it does:** the shut sash is moved at once with its collision: the setter is called with only flag bit 21
    (queued) cleared, so the change handler (slot 28, 0x143193310) runs at once and its 0x1431CBCD0 passes the
    change to the model's physics component (slot 30, 0x1434E2470) while bit 27 is set. Left queued, the handler
    would skip a queued model's collision (0x143193607: bit 27 cleared round that call). This is the client's
    collision, the kind the replay found Faith runs into (a replayed elevator moved this way killed her,
    2026-10-23). The server's copy isn't changed.
  - **Safe for Faith:** collision only moves while she is 4 m or more (across) from that window's hinge. A nearer
    sash is shut drawn-only at once and turns solid once she moves away. Unticking puts a solid sash's collision
    back the same way, once she's clear (until then the log says "N solid sashes turn back once Faith is 4 m from
    them"). Unticking *Make them solid* alone puts the collision back where the game has it and keeps the sash
    drawn shut.
  - The log's "shut" line now says solid / drawn only and the model's flags (bit 21 = 0x00200000 queued, bit 27 =
    0x08000000: its collision follows; without bit 27 the collision can't move this way).
  - **Not verified:** that the game's physics moves a Fixed body this way (slot 30 updates only bodies whose record
    + 0x40 isn't -1; what that field is wasn't found), that Faith is then stopped by the client's copy alone, and
    whether the server's copy (the old pose) makes her stutter there.
- **Replays: a mission's gates and doors drawn after the mission is over** (the user, 2026-10-27: "research how to show
  the mission's own models (gates, doors, the bus's doors) in a replay after the mission is over, starting with
  capturing their shapes while recording, and build it if it's safe"). Build 655F3F728413, installed with the game
  closed (game folder and dist, hash checked). It also has 65C44E0D6912's sound track, runners and stand-in start, and
  the weather session's slope water at full Wetness (its shaders were in the folder before this build).
  - **Found (read live, read only):** a static model's mesh is reachable from the entity: + 0x30 its data, + 0x98 its
    RigidMeshAsset, + 0x20 its MeshSet. The MeshSet's data has each detail level's sections (triangles, first index,
    vertex start and count, vertex elements, vertex size). For each loaded level, the MeshSet keeps its vertex and
    index buffers at + 0x50 + level × 0x50, each with the bytes it was made from still in memory. Over every loaded
    mesh (4474 levels) every buffer's size matched its level and its bytes were readable. The vertex format codes come
    from the game's own enum tables (position Half3, Half4, Float3 or Float4; index format 0x21 = 16-bit, 0x2E = 32-bit).
  - **While it records:** as each moving model first moves, its mesh is copied once (its most detailed level loaded
    then: its vertex and index bytes and, per section, where its triangles and positions are). Every read is guarded and
    checked: classes by vtable, sizes against each other, every index within its section, the buffers still the same
    after the copy. At most 32 MB per mesh and 256 MB in all. The log says "its shape kept" or why not.
  - **While it plays:** a recorded model the game doesn't have (a mission's, taken out with it) is drawn by the replay
    at its recorded place and time, at Present with the game's camera, only where it's nearer than what the game drew
    (the shared depth copy) and its own nearer parts. Its textures aren't kept, so it's one light grey, lit from above,
    both sides alike, over the game's lighting and HUD, and not solid. REPLAY > Doors... > "Draw the ones a mission
    took (plain shapes)", on by default (`[Replay] ModelShapes`).
  - Nothing of the game is hooked, called or written for this.
  - Checked: it builds with no warnings in the changed files; the chain and the sizes were checked live on the running
    game before it closed. **Not checked:** a capture and a drawing in game (the game closed before the capture's own
    check on live meshes could run), how the shapes look against the game's picture, and whether every mission model
    is a rigid mesh.
- **Water running down slopes only at full Wetness** (the user, 2026-10-27: "Rain down slopes should be visible
  only when wetness is at 100%"). Built as 36e6babc3336; installed in another session's later build 655f3f728413 (02:59, game
  folder + dist), checked: it holds all 7 current shader blobs.
  - The sheet of water flowing down sloped ground (its flow, the wet film under it, the rings and her slide on it,
    and the drips off its lower ends) is now shown only with the weather's Wetness at 100 %: none under 98 %, fading
    in over the last 2 % so the slider doesn't pop. Flat puddles, the wet look and everything else are unchanged.
  - Checked: it builds; the DLL holds all 7 shader blobs. **Not checked in game.**

- **New: Close the lair's open windows** (GAME > Detail and draw distance, after *Open the lair's blinds*; off by
  default; config `[Misc] CloseLairWindows`). The user, 2026-10-27: "find if you can close the open windows from the
  lair". Build 36E6BABC3336 (02:53:47, made by the build another session ran a few seconds after this change was
  saved; this session's own MSBuild then found it up to date, and the file compiled alone shows no warnings),
  staged (the game was running). When the game closed (02:59:50) another session's newer build 655F3F728413 (02:59:40)
  went in instead; it holds this change (its strings checked in the installed DLL). **Installed in 655F3F728413**
  (game folder + dist). Not checked in game.
  - **What the open windows are (game data, read-only):** every window of the Runners' lair (the old train station,
    buildings zone 8) is a frame in the wall with a sash in it: the sash's own frame, its blinds and two glass panes
    (its mesh's sections Frame, Aluminium, windowInner, windowOuter). 20 windows are one model each with the sash shut
    (`lairblinds_256` / `_512`; their glass is the separate `lairblindswindow`). The other two have only the outer
    frame in the zone (`lairblinds_256_frame`, its 2 instances), and their sash is a model of its own
    (`lairblinds_256_open`), put in by the train station's prefab (`pf_dt_trainstation`) according to the story:
    - by the curtain (hinge at -18.28, 69.25, 273.73): swung 45 degrees open outward (#5) or shut (#7). One is
      streamed in and the other out by CompareBool #71 (a progression flag, a mission's status and the install
      progress, through Math #76).
    - by the combat area (frame at -15.24, 69.25, 317.61; `pf_dt_lairblindscombatarea01`): shut, or the "broken"
      sash (`lairblinds_256_broken`), turned into the room by 0, 17 or 129 degrees by progression flags (CompareBool
      #8, PropertySelect #13 on its BlueprintTransform). Decoded, the broken sash has the same vertices (all 467, within
      1 cm) and the same four materials as the intact one. Only its origin differs: on the room's side, 0.33 m in. So,
      shut, it looks like the intact sash.
    - Neither has a closed variant of another kind: the game's "closed" state of each window is this same sash in
      its frame. Nothing else places these models among the 43202 EBX files read (the 502 patched ones weren't read
      for this).
  - **What the tick does:** while the free camera or Faith is within 250 m of these windows, the tools look every
    2 s for the sash models at the two hinges (in the game's own entity lists, as the camera's Attach does). Each one
    that isn't shut is turned shut about its hinge: the rows of the game's own shut sash (#7), or of the combat area
    itself, with its position kept. This uses the static models' setter that replays use to move doors (0x1432E8300,
    with the queued and physics flags cleared for the call). So only what's drawn moves: **the collision stays where
    the game has it** (so, untested: the swung-open sash by the curtain would still be solid where it was, and the
    window it now fills could be passed through). If the game turns or streams a sash again, it's shut again at the next look. Off: each
    sash still as the tools left it gets the game's transform back.
  - The log: "The lair's open windows: the window by the curtain shut (its sash lairblinds_256_open turned 45
    degrees about its hinge at ...; model 0x...)", "N sashes shut by the tools, M already shut as the game has them",
    "no sash of theirs loaded now (...)", and when off "as the game has them (N of the M sashes the tools shut turned
    back)". If the setter isn't the code the tools know, it warns and does nothing.
  - Checked offline: ClientStaticModelEntity (vtable 0x141AFB520) has the getter 0x1432E1440 and setter 0x1432E8300
    as slots 24 / 25, and the setter is `mov r8b, 1; jmp 0x1432EB1F0`, as the replay's mover expects. **Not
    verified:** that the entity lists hold these sashes (the log tells), how it looks, and the curtain's two events
    from the same switch (-642158230 / 283614660, names not found). The curtain may still move as for an open window.
  - Not possible from the data: blinds drawn closed on these two (their sash has the same blinds as the other 20, so
    shut they match them), and the windows' own unused animation (the removed *Play the lair windows' animation*,
    below: its shader parameter exists in no shader).
- **Replays: the game's sound as recorded, runners at once, stand-ins in place from the start** (the user, 2026-10-27,
  after the 02:17 test: "we need the runners to be synced same with the mission assets (doors bus etc.) also ksec
  animations are a bit weird and they have no voicelines"). Build 65C44E0D6912, staged (the game was running); it
  installs itself once the game closes, if dist still holds it.
  - **What the 02:17 log showed:** stand-ins work. 14 were asked and 11 played their person. Each KrugerSec was made at
    once by a free-roam spawner (about 750 m away) and played from about 5 s after it was asked. The two runners
    (SpawnTemplate_AIMale_Runner_02) have no spawner after the mission. The civilian spawners of their kind (MaxCount
    1) were each tried for 15 s and made nothing, so one runner was played by a live man from 70 s on and the other
    never. Of 31 voice lines only 2 were said again, and none of 9781 cutscene sounds: the mission's sound banks had
    gone with it. 6 of 21 mission models were found again; the gates, doors and the bus's doors weren't.
  - **The game's sound as recorded** (REPLAY > "The game's sound as recorded (voices, dialog, music)", on by default,
    `[Replay] SoundTrack`). While it records, Windows captures the game's own sound (process loopback, 48 kHz stereo,
    about 11 MB a minute). While it plays, the game's own sessions in Windows' mixer are muted and the recording is
    played in their place, at the replay's time: paused with it, jumping with seeks, faster or slower with its speed,
    silent with a fixed playback FPS. When it stops the game's sound is unmuted. If the game closes or crashes while
    muted, the next start unmutes it. The KrugerSec's voice lines, a mission's dialog, cutscenes and music are heard as
    they were, after the mission too, from where the camera was then.
  - **Runners and other people with no spawner:** a spawner at its limit (its MaxCount, or its simultaneous count) isn't
    asked any more; the game's own fields are read (+ 0xF4, cells + 0x270 / + 0x280). A gone person whose own template
    no spawner can make now takes a live one of the same kind at once (a man of the city for a runner), and only asks
    for a stand-in when there's none.
  - **In place from the start:** the playback's start is held, up to 10 s, until the stand-ins for the people there at
    that moment are made and playing ("Holding the start for the stand-ins" in the menu); the scan runs twice a second
    meanwhile. People who come later have their stand-ins asked 20 s ahead. Up to 32 stand-ins.
  - **Not done:** the mission's own models (gates, doors, the bus's doors). The game destroyed them with the mission and
    their meshes are in the mission's data. Showing them would mean keeping their shapes while recording and drawing
    them ourselves. KrugerSec animations "a bit weird": not found from the log (the stand-ins' poses land on the
    recorded ones, 0.00 m off); what looks wrong is needed.
  - Checked: it builds with no warnings in the changed files; the new limit byte check (both checks' instructions, the
    property getter, Spawn's calls to them) and the Spawn byte check pass on the decrypted game image. **Not checked in
    game:** the sound capture and its playback, the muting, the start hold and the runners' borrowing.
- **Stand-in KrugerSec for replays, step 2: the game's spawners make them** (the user, 2026-10-27: "read CT.log for
  the borrowed people and the spawner list, then build the stand-in spawning"). Build CE93E0590303, staged (the game
  was running); it installs itself once the game closes, if dist still holds it.
  - **What the 01:13 log showed:** the Release recording (18 people, 14 guns or drones) played after the mission was
    over: 3 people borrowed from 591 - 901 m away, the rest missing. 40 spawners were loaded, all on: 12 with the
    KrugerSec templates (AIRanged_Enforcer01 and 02 and AIMelee_Protector, no limit; AIMelee_Sentinel, 1 each), plus
    civilians, Faith's own and 16 runner ghosts.
  - **While it records:** each person gets the spawn template of the spawner that made them. The server's update
    notes every spawner's characters and where they are once a second; the recorded person is matched to the nearest
    one within 2.5 m. The log shows "made from ..." for each person, and the count at the end.
  - **While it plays:** a gone person with a template takes only a live one of the same kind or template. If there
    is none, the game is asked for a stand-in: the server's next update sends Spawn to an enabled free-roam spawner
    of that template (or of the same folder and kind, AIRanged or AIMelee), through the spawner's own event handler,
    as a level's event link does. The new character in that spawner's list is the stand-in. The live copy nearest it
    plays the recorded person wherever it stands, and it isn't hidden as an "other" meanwhile. A spawner that makes
    nothing in 15 s is given up for another, 3 tries at most. The game doesn't take stand-ins out of the level while
    the replay holds people. When no stand-in can be made, any live one of the class plays the person, as before.
    At most 16 stand-ins are asked at once.
  - **When it stops:** each stand-in is destroyed with the game's own request, on the server's thread, that
    character only. One still on its way is taken out as soon as it's made. A spawner's Unspawn isn't used: it
    would take all of that spawner's characters.
  - REPLAY > KrugerSec and other people > "Stand-ins for people a mission took" (on by default; `[Replay] StandIns`).
    A stand-in is a live KrugerSec with its AI: it may come after the real Faith (she takes no damage while a replay
    plays), and the city may react to it. Only recordings made with this build have the templates.
  - Checked: it builds with no new warnings; the new byte check (the spawner's handler in vtable slot 4, the Spawn
    branches of all three handlers, the class-id range words, the spawn queue call, the spawned list's offsets and
    Unspawn's destroy call) passes on the decrypted game image. **Not checked in game:** whether Spawn sent from
    the start of the server's update makes a KrugerSec, how long it takes, where it appears, and whether the city
    reacts to it.
- **Stand-in KrugerSec for replays, step 1: the game's spawners listed.** The game's KrugerSec spawning was
  traced: a spawner's Spawn event queues one of its own KrugerSec from its spawn template. The tools now keep a
  list of the live spawners as the game makes and removes them. When a replay finds none of its recorded people,
  the log lists the spawners: on or off, template, place. That shows which could make stand-ins after a mission.
  Nothing is spawned yet; making stand-ins comes next, once the list shows usable ones.
- **A mission's people in its replay after the mission is over (borrowed).** When a recorded KrugerSec, gun or
  drone isn't in the level any more (the mission ended and took it with it), the replay now plays it with a live
  one of the same kind from anywhere in the loaded world, within 3 km. It must have the same class and skeleton,
  and the same kind of character (same game data) is chosen first, then the nearest. That live one is seen only
  where the replay draws it while it plays, and is given back when the replay stops, as recorded people always
  are. Nothing is spawned. So a mission's KrugerSec come back as other KrugerSec of their kind from the city, as
  long as some are loaded. When none of a kind are loaded, that person stays missing, and the log says who played
  whom. Not checked in game yet.
- **Footprints and handprints laid the way she faces** (the user, 2026-10-27: "Check if snow/rain footprints and
  handprints get placed where the shoe/hand faces and not the camera if not fix it and implement it"). Build
  9dfc6b9cb23b, staged (the game was running): it goes in 60 s after the game closes, only over an older DLL.
  - **What it was:** none followed the camera. The rain's and the snow's prints, and her handprints on the ground,
    were laid the way her last two steps went (the camera only told her right from her left), so they were stale
    while she stood, turned on the spot, stepped aside, after a jump or a wall run; the snow's prints guessed which
    foot from where they landed. On a wall her handprints point up, as before.
  - **Now:** the way she faces: her transform's forward row (the tracker reads it each frame), the way along it she
    faces told by how she runs (her moves over the ground faster than 2.5 m/s within 45 degrees of it; sure from 20
    more one way than the other, said in the log once: "Faith faces along / against her transform's forward row").
    Until then, as before. The foot's outer side from her right across that way; which foot from the game's own
    impact (its bone) for the snow's prints too, as the rain's had. Her wall-run prints point along the wall the way
    she faces (the camera's before).
  - Checked: it builds; the DLL holds the new log line and all 7 shader blobs. **Not checked in game.**

- **The haze no longer purple** (the user, 2026-10-26: "for som reason city fog is now purple"; the snowy look at
  about 8:00, its morning sky graded pink and purple). Build d606b19448aa, installed with the game closed (game folder
  + dist, hash checked); it also has the snow session's round of 23:39 (its snow's light, edges and piles, the pile
  slider gone). The haze's colour from the sky (2e5c33960ee5) kept 60 % of the sky's hue in the rain and 40 % in the
  snow. Now the snow's haze is a cool white with none of it (snow scatters daylight about evenly), the rain's keeps
  30 %; until the sky has been seen, the snow's is a cool white of the frame's light too. **Not checked in game.**

- **The haze: the sky's own colour, lit by lightning, off the HUD** (the user, 2026-10-26: "Do more research on how
  haze works and fix most issues really importannt"). Build 2e5c33960ee5, installed with the game closed (game folder
  + dist, hash checked).
  - **How it works** (the tools' own, WetLook.hlsl's HazeAt): over the game's finished frame, what's d metres off is
    blended towards the haze's colour by 1 - exp(-d x thickness), at most 85 % (the sky as if 1.5 km off); thickness
    from the rain's strength x the Haze slider (VISUALS > Snow > Snow haze in the snow), halved while a look's fog is
    on, in drifting veils (three samples of a slow noise along the view, x 0.55 - 1.45); over what's out in the open,
    or everything while the camera is.
  - **Its colour followed the camera:** it was the whole frame's average (greyer, x 1.5), so the haze over everything
    far off brightened as the camera looked up at the sky and darkened as it looked down at a street. Now it's the
    light of the sky by the horizon: the frame's sky at 48 points over its upper two thirds (a pixel of the query
    pass, read back), smoothed over about a second, kept while no sky is seen (indoors), greyer, whiter in the snow:
    what's far off goes into the sky's own colour, as in haze. Until the sky has been seen once, as before.
  - **Lightning hidden by it:** a flash brightened the sky under up to 85 % of a haze that didn't flash, so most of
    it was lost. Now the haze lights up with the flash, as the rain round it is.
  - **The HUD hazed:** the pass is drawn over the game's HUD, its markers, the Runner's Vision and its effects, which
    don't write the depth, so they were hazed as the wall or ground far behind them (washed out). Now not where
    they're told from the frame (the snow's OverTheGround: much brighter than round it, or a strong colour where round
    it isn't); lamps and lit windows count too and shine through. Not tested on the sky (a bright sky beside a dark
    building's edge would count).
  - The query pass also runs while it hazes (the snow had none).
  - Checked: it builds; the DLL holds all 7 shader blobs. **Not checked in game.**

- **Replays that cross a loading screen** (you started or left a mission, or fast travelled while recording).
  - The replay now remembers each loading screen it recorded through, and what came after it.
  - Playing starts where the last loading screen ended (REPLAY > *Start after its loading screen*, on by default),
    since what's before it is somewhere else.
  - The REPLAY menu says where it was recorded. The Release mission is named (the tools know it from Faith's
    outfit, which only Release gives her); other places are described by the loading screen's times and the time
    of day after it. Which mission is running isn't read from the game yet, so other missions aren't named.
  - When the recorded people aren't in the level, the message names that place and says to start it again from the
    game's menu, wait until it has loaded, then press Play.
  - Not checked in game yet.
- **Fixed: mission characters and other AI stopped despawning after a recording.** To keep a recording's KrugerSec
  from vanishing, the replay switched off the game's AI despawn checks, but for every AI in the city, and with *Keep
  them in the level until it plays* on (the default) that lasted from the recording until the replay had been played
  or forgotten. Now only AI within the keep radius of the recorded people's places are held (the same places the
  replay already keeps the game's own removal requests by). Everyone else despawns as the game decides. Not checked
  in game yet.
- **Fixed: pressing Play on a replay crashed the game** (since the 22:02 and 22:06 builds of 2026-10-26). When none
  of the recorded people were in the level, the new REPLAY menu message (298 characters) was copied into a 160-character
  buffer with a copy that ends the program instead of cutting the text, so the game closed on the replay's first
  frame. The buffer now holds 512 characters and the text is cut to fit if it ever needs to be.
- **Replays of a mission played after the mission: the menu now says why they're empty.** A recording made during
  a mission (Release in the test) was played after the mission was over. By then the game had unloaded the
  mission's whole world: its KrugerSec, its moving objects and most of its sounds were gone, so the replay had
  nothing to move. Keeping a mission's world loaded after it ends, or loading it again through its progress flags,
  isn't done: the first means holding the whole mission through a load (that kind of hold hung the game before), and
  the second would change your saved progress and restart the mission's scripts. Play a mission's recording
  before you leave the mission. When none of the recorded people are there, the REPLAY menu now says so and why.
- **A mission's people come back when you start the mission again.** Start the mission again from the game's own
  menu, so the game loads its world, then play the recording. A recorded person who isn't in the level any more is
  now played by a live one of the same kind (KrugerSec, gun or drone) standing within 10 m of where it was at
  that moment of the recording: the nearest one, matched once. So a restarted mission's new KrugerSec take over
  the recorded ones instead of being hidden. The log says who took over whom, with the distance. Matching is by
  place, so a KrugerSec the restarted mission puts somewhere else isn't matched and stays hidden. Not checked in
  game yet.
- **Water running down slopes flows down; puddles only on broad flat ground; handprints on walls and poles; downed
  KSec's bodies ring the puddles; the rain's rings only where the drawn rain lands** (the user, 2026-10-26: "water
  still doesn't properly flow downwards on a slope", "add a better detection / mesh detection for what should be wet
  and have a puddle", "if faith's hands are wet they should also leave handprints on the walls/poles she touches",
  "downed ksec should leave a body ripple when they enter ragdoll physics and hit the ground", "water ripples should
  appear only where drawn rain actually hits and not always"). The slope water is in 7eab193cbc2f (another session's
  build, 21:10, which had my shaders); the rest in f6c590848774, installed 21:3x with the game closed (game folder +
  dist, hash checked), which also has the loading-screen session's changes.
  - **The slope water:** its waves were laid out along the slope from a point up to 128 m away and moved by a clock up
    to 1000 s times a speed from each pixel's own slope. That slope comes from the frame's depth, a little different
    from pixel to pixel and frame to frame (the game's jitter), and those differences times hundreds of metres and
    seconds put each pixel's waves anywhere: it shimmered in place, worse the longer the game ran. Now as games draw
    running water: the pattern laid on the ground's own x and z, moved down the slope by at most 1.2 s of flow, two
    layers half a cycle apart fading in and out; the slope only says which way and how fast. Its waves are shaded as
    they catch the light, as the puddles' rings are.
  - **Puddles on broad flat ground only:** the surface 1 m away in 8 directions; where 3 or more drop away (an edge),
    less of a puddle, none from 4: no puddles on boxes, tables, benches, car roofs, ledges and wall tops (they're
    still wet). A curb on one side or a walkway between two drops keeps them. Faith's puddle steps see the same.
  - **Handprints:** out in the rain her hands and shoes stay wet (only what she touched wetted them before). On a wall
    or a pole a print is as strong as her hand is wet (out in the rain a wall was as wet as her hand, and none was
    left), darker on top of the wet look. Her hand impacts are the game's land, climb and scuff (no pole impact of its
    own: a pole's grab is one of those).
  - **Downed bodies:** the game plays ME_Sound/Character/AI/AI_Ragdoll_CollisionVelocityBased for each knock of an
    AI's ragdoll (the sound log at 20:47:13: twice, 2.2 m from Faith). Its voices are told by that name as they're
    made, placed and started; where it's placed, the ground under it (a sweep, game thread) gets a body's ring (2.4)
    and a splash, in a puddle there; a knock within 1 m and 0.5 s of the last is the same.
  - **The rain's rings:** every cell rang every time round, whatever the rain, on every puddle out to 200 m, and in
    the roofs' edges where the game's rain is kept out. Now a cell's drop falls each time round only as often as it
    rains (0.1 + 0.9 x the rain's strength), only within the drawn rain's reach (60 m; 80 where the rain isn't kept
    out of buildings; the last 10 m fading), not inside the boxes the game's rain is kept out of (their 64, sent to
    the shader); each ring starts with its drop's bright speck.
  - Checked: it builds; the DLL holds the new log lines, the ragdoll sound's name and all 7 shader blobs (/O3). **Not
    checked in game.** Not known: whether a ragdoll's knock voice is placed at the body (the ring goes on the ground
    under it either way).
  - A mistake of mine on the way: my build started while another session's was running; its compiler had read
    Hooks.cpp and DryShapes.cpp before my changes landed, so my build took them as up to date. Seen in the DLL (the
    ragdoll sound's name missing), the four files touched and built again; f6c590848774 is that second build.

- **GAME > Loading the world: the district list is always shown.** It isn't a fold-out any more: it sits under a
  small *Districts* label, with the help (?) next to the label, under *Keep the track's path loaded*. It still
  only shows while *Load the world around the camera* is on, as before. Build 7eab193cbc2f, installed when the
  game closed (hash checked). Built without errors; not checked in game yet.

- **Cutscene voice lines in replays** (the KrugerSec talking in a cutscene). A cutscene says its lines differently
  from mission dialog: its sound track hands the line to the speaker's voice as a sound parameter, then starts the
  voice. The replay recorded only the start, so its own voice started with no line and stayed silent. Now each
  sound handed to a recorded voice is recorded too, and while the replay plays it's handed to the replay's voice
  again just before that voice's recorded start. After a seek it's the last one each voice had. A line the game has
  unloaded since is left out. The cutscene speakers' places are recorded as well, so the lines come from where they
  were. Not checked in game yet.
- **KSec, other people and cars ripple the puddles; a little splash at every ripple; landings and rolls ring bigger
  the harder she comes down; ripples on the water running down slopes** (the user, 2026-10-26: "Research how can
  ksec and other people leave ripples / vechicles and implement it high priority no mistakes", "also we need little
  splash of water in every ripple", "Depending from how far faith is jumping and rolling/falling should leave a bigger
  ripple", "Sliding down on a water slope should also leave a ripple"). Build aa7d1a56667d, staged 20:04 (the game was
  running): it goes into the game folder and dist 60 s after the game closes, only over an older DLL.
  - **Found in the game's data** (Characters/AINpc and AINpc_Spec_Enemy, read from the game's files): KSec and the
    game's AI people have the same foot impact sound entity as Faith (FootPlantLeft / Right / Wall / LandHard, the same
    ids) on their own skeleton's feet (LeftFoot 146, RightFoot 155; Faith's are 167, 175), and no impact effect
    entity. Their steps already came through the tools' hook and were left out by the bone. The citizens
    (Characters/AINpc/AICitizen) have no impacts; nor do the crowd and the road's cars.
  - **Now:** KSec's and the AI people's own steps and landings ring a puddle they're in (a landing bigger). The crowd,
    the citizens and the road's cars, which the tools already follow (the people kept dry), from their moves: a step
    every 0.75 m walked, the feet 10 cm to alternate sides; a car's two back wheels every 1.2 m. A step from moves
    isn't made where one of the game's own impacts was within 1.2 m and 1.5 s (KSec aren't rung twice). Within 40 m
    of the camera, the 24 nearest kept. Their sounds and effects stay as they are.
  - **A little splash:** every ring (hers: steps, landings, rolls, hands; the others') throws up 10 drops for its
    first moments, up at 0.8 - 1.6 m/s and out at 0.3 - 0.8 m/s (x the square root of the ring's size: a landing's
    higher and wider), falling back, drawn in front of what's behind them; the 16 nearest.
  - **Landings and rolls by how hard she comes down:** her feet's fastest fall in the 0.3 s before (and a quarter of
    her speed along the ground), as the height that gives it (v^2 / 2g): the ring x (1 + 0.4 sqrt(h - 0.3)), at most
    x 2.5 and size 5 (a running jump's landing about x 1.5, a 5 m drop about x 1.8). Logged when it's bigger.
  - **Slopes:** the step rings and the slide's waves were only where puddles are, and none lies on a slope. Now on the
    water running down a slope too, shaded as on the wet ground, and the slide pushes the running water aside
    (coming back as in a puddle). The slide's waves come from the game's own slide effect (FX/Movement/FX_Slide_*),
    as before.
  - **A mistake of mine fixed:** builds 9adfe19b2804, 3e9d834142c9 and deab62b1a4c3 had their shaders compiled without
    /O3 (the shader file's own build lines have it), so fxc's default /O1; this one is /O3 again.
  - Checked: it builds; the DLL holds the new log lines and all 7 shader blobs (compiled /O3). **Not checked in
    game**: whether KSec's impacts come through (the log's "someone's own step (the game's impact)" says so), the
    splash's look, the ring sizes.

- **Smooth running water and ripples; footprints on wet concrete, stone and gravel, on walls in a wall run, fading
  out, none from barely wet ground; the slide's ripples longer** (the user, 2026-10-26: "Rain down slopes still
  laggy", "footsteps should appear on most concretes/sands if it has not a puddle", "Footsteps should appear on walls
  as well if faith wallrides", "footsteps should fade out instead of getting removed in an instance also indoors there
  is still one footstep spawning", "i meant footprints for all the above", "water slide ripple effect should take a
  while to dissapear and should ripple for a lot longer"). Build deab62b1a4c3, installed 19:31 with the game closed
  (game folder + dist, hash checked); it has everything in the folder then (the Lights session's work too).
  - **The jerks:** the clock all the moving water is drawn by (the slopes' running water, the rain's streaks, the
    ripples, the drips) was GetTickCount64's, which moves in steps of about 15.6 ms. At the game's 100 - 110 frames
    a second (the log's performance lines) it stood still one frame and jumped the next. Now from the steady clock
    (QueryPerformanceCounter), as are the rings' and prints' clock, the snow's clock and the flakes' frame time (the
    same steps), and the haze's drift.
  - **On wet ground with no puddle:** each step leaves a print pressed into it (from ground at least half wet): on
    concrete and stone the water pressed out of the sole's shape (lighter, less of a mirror, the lugs most), in gravel
    pressed in (darker). The rain fills them in over 8 s. Which ground, from the game's own material for the step
    (its footstep sound id: concrete and marble, gravel; not glass, metal, wood, tarp, bin bags, mattresses or
    water). The game's footsteps have no sand; gravel is the nearest. The material is only read: no sound changes.
  - **On walls:** the game's FootPlantWallLeft / Right impacts (her wall-run steps) now leave her wet shoes' print on
    the wall, in its plane, the toe the way she runs, darker as her handprints are. Out in the rain her shoes stay wet.
  - **Fading out:** of the 24 prints kept, the 8 oldest fade out, the oldest faintest, so none goes at once when a new
    one takes its place (handprints: 5 of 16).
  - **No more stray prints indoors:** her shoes took up water in proportion to how wet the ground was, even from ground
    barely wet. A roof square's soft edge round a door wetted them a little, and they left a faint print or two
    indoors (the log at 19:16:44 and 19:17:16: shoes 0.07 and 0.18 wet on ground at 0.00). Now nothing from ground
    under 0.3, and less near it.
  - **The slide's ripples:** the water pushed aside comes back over 4 s (was 2). Its waves go out for 6 s (were 1.6),
    1 m/s at first and slowing, out to about 3 m (was 1.4), with more crests and longer ones as they spread.
  - Checked: it builds; the DLL holds the new log lines and all 7 shader blobs. **Not checked in game**, including
    whether the wall-run impacts' contact normal is the wall's (the first print's line in the log gives it).

- **VISUALS > Lights: easier to use** ("Make them more user friendly"; build 092171129380, installed 18:44, the
  game closed).
  - **Looks to start from** ("Start from"): a candle, a light bulb, a ceiling light, a street lamp or daylight fill
    for a sphere; a torch, a desk lamp, a car headlight, a stage spotlight or a film light for a spot. Each sets the
    color temperature, brightness, reach and cone to about what the real thing gives, and names the light after
    it unless it was given a name.
  - **Names:** each light has one, typed in "Name". They show in the list, by its dot and in the log.
  - **The list:** an On box and a color swatch on each row.
  - **Simpler settings first:** On, Shadows, color (or "Use a color temperature"), Brightness with "About like a
    light bulb" and the like, Reach, and for a spot "Cone width" and "Soft edge" (0 - 100 %, kept when the cone
    changes). The rest (units with candela and lux, size, multiplier, diffuse / specular / bounce, the shadow
    settings, the colour scales) is under "Advanced".
  - **Placing:** "In front of the camera" brings a lost light back 2 m ahead; "At the camera" (was "Move to
    camera"); "Aim like the camera" turns a spot the way the camera looks and leaves it where it is.
  - **On the picture:**
    - the picked light's name beside its dot;
    - its reach: a faint ring round a sphere, a line to where a spot's light ends;
    - an arrow at the picture's edge, with its name, pointing to it when it's out of view;
    - the mouse wheel over a dot makes the light brighter or dimmer (x 1.25 a notch; dragging, the wheel still
      takes it nearer or farther).
  - **Wording:** "Show the lights on the picture", "Keep height" (was "Drag along the ground"), "Forget all" with
    the light count, and the help lists what the mouse does. **Not checked in game.**

- **The ice over the water no longer z-fights, near or far; the rain's falling water the same** (the user,
  2026-10-26: "snow water flickers find any instances for rain as well", "it's z fighting close up or at distance").
  Build 3e9d834142c9, installed 18:35 with the game closed (game folder + dist, hash checked).
  - **Why:** each pixel of the ice was kept only while the frame's surface there (the game's water) was at most 5 cm
    (+ 0.2 % of the distance, + the piece's tolerance: 0 for the 239 rectangles, up to 0.3 m for the sea's and canals'
    triangles) in front of it, measured along the view. Seen at a slant, a surface h off the ice is h / cos(the
    angle) along the view: far off (a shallow angle) a centimetre or two became more than the 5 cm, and close up the
    game's water stands a few cm off the rectangles (each is its mesh's middle), so the test flipped with each
    frame's jitter: the snow laid on the water below showed through in specks.
  - **Now:** measured along the piece's own facing (how far the frame's surface stands off it, however it's seen):
    ice where it's at most 15 cm (+ its tolerance, + 0.1 % of the distance), fading out over the next 10 cm.
    What that lets in: a low edge (under about 15 cm) in front of the water, seen at a slant, takes the ice where it
    hides the water behind.
  - **Rain:** nothing there is drawn flat on a surface as the ice is (the wet look, puddles, rings, prints and trails
    are worked out on the frame's own surface; their height tests, 15 - 30 cm, don't depend on the angle). The one
    test like the ice's was the falling water's (the roofs' drips, the drainpipes' streams, the slopes' ends' drips):
    hidden more than 5 cm of view depth behind the frame's surface, so a stream a few cm from a wall or the ground
    (a roof drip on the 2 m squares' line can be just inside a front) came and went at a slant. Now hidden more than
    5 cm behind the surface along its normal, or 50 cm along the view (so a wall seen edge-on doesn't show what's far
    behind it).
  - Seen on the way, not changed: the snow's pile sides (SnowPileSide) look for their wall within 0.3 / 0.6 m along
    the view, the same kind of test (snow, not water; the snow session's).
  - Checked: it builds; the DLL holds all 7 shader blobs. **Not checked in game.**

- **Rain: both feet leave prints, a graded wet/dry test for them, rain no longer cut into blocks round meshes,
  drips off the ends of the running water** (the user, 2026-10-26: "Rain footsteps leave only 1 footprint behind when
  it should always be 2", "We also need a better algorithm for what is wet and dry detection for the footprints", "rain
  is still blocky around some meshes", "after the water slope ends they should be water dripping from the end of it").
  Build 9adfe19b2804, installed 18:14 with the game closed (hash checked). It also has everything else in the folder
  then (the snow session's 17:50 changes, the images), with all 7 wet-look shader headers rebuilt. The first two were
  already in c96672caf3cb (built by the images session at 18:06, with the shader headers of 16:33).
  - **Both feet:** a step's sound and its effect both ask, and one within 250 ms and 25 cm of the last step was taken
    for the same step asked again: any foot's last. Where both feet come down together (a landing, stopping with the
    feet 20 cm apart) the second foot left no print. Now the game's bone says which foot (167 LeftFoot, 175
    RightFoot), the same-step test is per foot, and the print's side comes from it.
  - **Wet and dry:** how wet the ground is at a step is now a share (0 - 1: the wet look's "outside" there, from the
    roof squares and the game's interiors), where it was wet from half up and dry under it; the camera's distance and
    the fog no longer count (they draw the ground fainter, they don't dry it). Her shoes take up 0.7 of the ground's
    share when that makes them wetter; out in the rain (all wet) they don't dry; where it's less wet (a roof's edge)
    they go on drying. A print wherever they're wetter than the ground by 0.03: on dry ground as before, and fainter
    on damp ground (the shader shows a print only where it's wetter than the wet look, pixel by pixel). Her hands the
    same; a handprint is as strong as the hand is wetter than the surface (it darkens on top of the wet look). Why:
    in the log at 17:23:20 - 22 the camera went "under a roof 1.2 m up" / "under the open sky" and a print was made
    with the shoes 0.68 wet. At a roof's edge the roof squares' 2 m soften the line, the half-wet test flipped from
    step to step, and each "dry" step left a print on ground drawn up to half wet (often every other foot only).
  - **The tools' far rain (its streaks, 8 - 60 m):** it was all or nothing by whole 2 m squares (none over a square
    with any roof over its middle, even far under the drop, nor 1 m round it), and none in the wet look's dry boxes
    round people, cars and trains (boxes cut out of it). Now as much as the squares know it's open at the drop's own
    height (soft between the squares; none inside the game's interiors). The boxes are gone: what's behind the
    frame's surface isn't drawn anyway, and the last metre in front of it fades out.
  - **The game's rain (round the camera):** it's kept out under roofs by boxes that went 1 m past the roof squares on
    every side, so it stopped up to 2 m out from under a roof, in straight lines along the 2 m squares. Now where a
    box meets open sky, its side goes 25 cm past where its roof was found to end (3 sweeps up between the roofed
    square's middle and the open one's, to 25 cm), never farther out than before. Sides beside another roof, squares
    not looked at and the game's named interiors keep the 1 m. Within 12 squares (24 m) of the camera, at most 12
    sweeps a frame, again after 5 s or when the camera's height or that roof changes.
  - **Drips off the slopes' ends:** the query pass (the one for Faith's puddle squares) also looks at 64 x 36 cells
    of the frame, one point each a frame, picked by a hash that moves. Where the running water is drawn it follows
    the slope down its fall line (15 cm steps, up to 3 m, each from the surface found). Where the frame suddenly
    shows something more than 30 cm farther (or the sky), the slope ends at a drop, found to 2 cm. A slope running
    into the ground or under something has no end. The end, 6 cm out, is kept for 20 s from when it was last found
    (64 at most, 60 cm apart). The game thread sweeps down from it (8 a frame) for where its drops land (0.5 - 40 m
    under it), and it drips with the roofs' edges (the 48 nearest drips on the frame, within 30 m). Only slopes seen
    from above are found (the running water is only on the frame); under a roof's edge seen from below, the roofs'
    own drips are there as before. Log every 30 s: "ends of the slopes the rain runs down found lately".
  - Query target: 96 x 36 RGBA32F (was 32 x 32 RG32F).
  - Seen on the way, not changed: the people's rain boxes come after up to 64 roof boxes, and the game uses the first
    32 boxes near a rain emitter, so with many roofs close by the people's boxes may not count (not checked in game).
  - Checked: it builds; the DLL holds the new log lines and all 7 shader blobs; the shaders' constants match the C++
    (g_HazeView at 37984 in both). **Not checked in game.**

- **VISUALS > Images: your own JPG and PNG pictures standing in the world** ("make it possible to add jpg and pngs on
  the map so just like the games intro"). First build c96672caf3cb (18:07). Now in 092171129380 (game folder + dist,
  18:44, built from the whole folder by another session after the dots below; checked it holds them).
  - **What the game does (read from its data):** the pictures in the Release mission are the city's video screens.
    Its sublevel LMSRelease2 has a layer Newscasts with one LinearMedia billboard feed
    (LinearMediaBillboardFeedEntityData: the video newscast_01, two speakers); by its fields (TextureOutCode,
    EmissiveTextureOutCode) the feed gives the screens a texture and an emissive texture (the screens and the video
    decoding: Research_Billboards_2026-10-24.md). A rain effect named for the big screen
    at the release exit (fx_ambient_rain_releaseexitbigscreen) sits in city zones 293, 296, 312 and 313. The game also
    has a 3D title logo (objects/cutscene/mecatalystlogo_01) and a "logo shot" sun flare, but no level or prefab uses
    them. The cutscenes are videos (levels/cutscenes/video_cutscenes).
  - **What it does:** pictures from `Cinematic Tools\Images` next to the game's exe (made on first use; "Open the
    folder", "Look again"). Pick one, then "Put it in front of the camera" (Distance away, facing the camera) or
    "Click where it goes": the next click on the picture outside the menu puts it flat on what was clicked (a wall,
    the ground: the surface's direction from the game's depth round the click), 2 cm out; on the sky it goes Distance
    away. Each placed picture: Show it, move it in front of the camera or by a click, Place (x y z), Out / in (along
    its front), Turn, Tilt, Roll, Stand it upright, Width (its height follows the picture), Opacity, Brightness, Always
    face the camera, Seen from behind too, Mirror it, Make a copy, Remove; Remove all. The open ones get an outline on
    the picture while the menu shows them.
  - **How it's drawn:** over the game's finished frame at Present, after the wet surfaces and before the green screen,
    the lens distortion and the Panini projection (they bend with the picture). Each pixel only where the picture is
    nearer than the game's depth there (the green screen's copy; up to 2 cm + 0.05 % of the distance behind a surface
    still counts, so one on a wall doesn't flicker), so whatever stands in front stays in front. Farthest first,
    premultiplied alpha, all mipmap sizes, 8x anisotropic.
  - **Dots, as the lights'** ("Just like the light manager add a dot so you are able to move the image"). While the
    menu is open each picture has a square amber dot at its middle (grey while it's hidden) with its number from the
    list. Drag it to move the picture across the screen at the same distance; Shift: only up and down; the mouse
    wheel while dragging: nearer or farther (0.25 - 2,000 m from the camera). "Drag along the ground" keeps its
    height. A click on a dot rings it and opens the Images header and that picture in the menu. Only away from the
    menu's windows; the click is taken before the lights' dots and the camera's track path. "Dots on the picture" and
    "Drag along the ground" are saved ([Images] Dots, DragAlongGround). The camera is kept for the dots while the
    menu shows them, so hidden pictures have dots too.
  - **Playing as Faith** ("image should be also be able to be viewed while playing as faith"): they are drawn with
    the free camera on or off. The game's own camera goes to the drawing with the camera off too (CameraManager's
    camera hook, as for the green screen and the wet look), and nothing in the images checks the free camera. Not
    known: whether Faith's first-person arms are in the depth the pictures are tested against (the copy is taken
    before the game's second depth clear in a frame); if they aren't, a picture behind her hands would draw over
    them. The pictures go over the game's HUD where they overlap.
  - **Reading the files:** Windows' own decoders (WIC) on a thread of their own. A JPG's EXIF orientation is followed
    for 90 / 180 / 270 degrees (the mirrored orientations aren't). Bigger than 4096 pixels on a side is scaled down.
  - **Saved** in config.ini, `[Images]`: Distance, Count, and for each ImageNFile / ImageNPlace / ImageNLook.
  - **Limits:** the game's light, fog, exposure, depth of field and motion blur don't touch them (they show the
    picture's own colors; Brightness is the only match). The tools' rain and snow in front of a picture are hidden by
    it. With the Panini projection or the lens distortion on, a click lands a little off. Over the game's HUD when it's
    shown.
  - **Shared files changed:** Main.cpp (Apply, config), UI.cpp (the Images header under Lights, the overlay before the
    lights' dots), Visuals/GreenPlane.h/.cpp (`WantDepth` takes who wants the depth, as bits; the wet look's call is
    unchanged), the vcxproj. New: Visuals/WorldImages.cpp/.h/.hlsl, WorldImagesVs.h / WorldImagesPs.h (fxc).
  - Checked: it builds, and the DLL holds the new menu strings and both shaders. The user's 18:39 - 18:42 run (CT.log):
    a 3840 x 2160 PNG read, put in front of the camera and by clicks (on level ground: tilted 90), no crash note.
    **The user, after trying it: "it looked fine and it works just fine".** Not reported on: Faith's first-person arms
    in front of a picture, the HUD under one.

- **VISUALS > Lights: dots to drag with the mouse, brighter defaults, more of the game's settings** (build
  df34c0006523, installed 17:53 after the game closed). The research: Research_Lights_2026-10-26.md (outside the
  repo).
  - **Dots on the picture** ("being able to move them with your mouse when f5 is open"). With the menu open each light
    has a dot: its colour, grey when it's off, a ring round the picked one, and for a spot a line the way it shines.
    None is drawn for a light behind the camera. Hovering shows its name. A click picks it in the Lights tab (the
    Lights header opens). A drag moves it across the picture at the same distance; Shift moves it only up and down;
    the mouse wheel while dragging takes it nearer or farther. "Drag along the ground" keeps its height instead.
    Only away from the menu's windows. The dots get a click before the camera's drawn track path, which then doesn't
    take it too. The free camera keeps its keys and right-button look, and the Panini view is followed.
  - **Fixed:**
    - "Bounce light color" and "Particle color" changed each other's field (the game's reflection: + 0x80 is
      ParticleColorScale, + 0x90 RadiosityColorScale).
    - New lights were 10 lumens: about 0.8 candela, less than a candle. Now 800 lm (sphere) and 1,500 lm (spot);
      the game's own lights' median is 300 and 650.
    - Their shadow distance was 20 m with an 18 m fade; now 80 m and no fade, as most of the game's own lights.
    - New lights start 2 m in front of the camera (they were at it, where their dot can't be seen).
  - **New settings:** On; color from a temperature (1,000 - 10,000 K, the colours the game's own lights have for
    each); units, lumens or nits (switching keeps the brightness), with candela and lux at 1 m and 3 m shown; size
    (the sphere's radius, the spot's disc); the cone as its whole angle (1 - 179 degrees, as the game keeps it) and
    "full brightness within"; shadow distance, fade, reach and strength; "Show its shape" (the game's
    EmissiveShapeEnable, not checked); Copy.
  - **Found, not fixed:** some graphics settings changes (shadow quality and others the game compares) make the game
    rebuild all its lights from its own lists. The tools' lights aren't in them, so they may vanish then. Not seen in
    game.
  - Shared file changed: UI.cpp (the dots drawn before the camera's overlay; the Lights header opened after a dot's
    click). **Not checked in game.**

- **Wet footprints for 10 s after the wet** ("Increase the seconds that faith can leave footsteps after rain to 10
  seconds"). Her shoes used to dry by a quarter with each print, so prints stopped after 6 - 8 steps (3 - 4 s). Now
  they dry evenly over 10 s from her last step in a puddle or on the wet ground, each print fainter, until there's
  almost nothing left (3 %). The clock waits while the game is paused. Her wet handprints the same ("Do the same for
  handprints as well"): her hands dry evenly over 10 s from the last wet touch (they used to halve in 30 s and lose 40 %
  a print). In the folder for the snow session's next build. **Not checked in game.**

- **Rain/snow: performance, puddles off holed metal again, slope water steadier** (build 4ccac185ac20, staged: it
  installs when the game closes).
  - **Measured first** (the user's 30 s of rain and of snow, 3840 x 2054, 90 - 140 fps).
    - GPU: the wet look's full-screen pass took 1.8 - 4.5 ms in snow and 2.9 - 6.4 ms in rain (most 7 - 12). The
      snow's flakes took 0.13 - 0.18 ms.
    - CPU: the weather took 0.2 - 0.45 ms a frame on the game thread, mostly the roofs' sweeps. The people kept dry
      took about 0.1 ms, but their rescan cost 3 - 5 ms every 400 ms. The render thread took 0.06 - 0.27 ms.
  - **GPU, the same picture:**
    - The interiors' and the dry boxes' tests skip a box the pixel is nowhere near; every pixel went through all of
      them in full.
    - The interiors are worked out once a pixel instead of up to three times.
    - The puddle test puts its 24 points on the frame from two axes instead of a matrix product each, and stops as
      soon as it's sure.
    - Reflections take as many steps as their ray needs: 3 px a step on puddles, 8 on rough wet ground, 8 - 48 steps
      instead of always 48. Ones under 2 % aren't traced.
  - **CPU:** the people are rescanned every 1 s instead of 0.4 s. Roof squares past 20 m are looked at again every 5 s
    instead of 2.
  - **Holed metal** ("Water still gathers on stairs with holes / metal with wholes"). The flicker fix had made the
    hole test soft: holes had to cover a quarter of its points' weight, so metal with fewer holes kept a faint puddle.
    Now an eighth does. Each point reads the farthest of the 2 x 2 depth texels round it (Gather; checked against the
    device for the depth's format, else one texel), so a hole about a pixel wide is found however the game's jitter
    moved the samples. That keeps the flicker fix. PsMain and PsQuery now compile as ps_4_1.
  - **Slope water** ("really laggy and doesn't flow properly it's tweaking"). Its 25 - 29 cm waves tilted the sharp
    reflection's aim by up to 9 degrees, so the reflection jumped between things as they moved. Now the waves are
    40 - 50 cm crests across the slope rolling down. They show as the reflection brightening and dimming (a moving
    sheen), with only a 2-degree tilt, on a slightly blurred reflection (roughness 0.18).
  - **Not checked in game.** The "Weather (performance, diagnostic)" line will show the new costs.

- **Rain: no flicker on puddles, a slide that pushes water aside, haze in rain and snow, wet handprints, roll ripples,
  more fog; performance timing** (build 93c30746e5f1, installed in the game folder and dist with the game closed, hash
  checked; it also has the snow session's 92f8a52021b4 work).
  - **Flicker** ("There is still some water flickering fix any instances"). The puddles' surface test was 0 or 1: any
    one point round a pixel seen through to more than 4 cm under its plane made it 0. The game's jitter moves which
    depth sample each point reads from frame to frame, so along a hole's or an edge's border puddles blinked. Now each
    point counts as much as it lies under the plane (0.75 - 1.5 x that depth). The near ring counts three times, the
    middle twice, the far once, and a quarter of all that takes the puddle away. The stair test is soft the same way.
    The points are turned with the world's x, not the pixel's own normal.
  - **The slide** ("water slide is still not visible it should push water asside and create ripples from faiths
    body"). Before, there were rings only round each slide effect's place, behind her by the time they spread, and a
    faint streak. Now her path pushes the water aside, 30 cm each side: a puddle is shallower there, less of a mirror;
    the wet ground's film is wiped lighter. It comes back over 2 s. Each stretch of the path sends waves out to both
    sides at 0.8 m/s for 1.6 s, so behind her they spread in a V (a wake). Where she is right now, the water is
    churned. On the wet ground the waves show too, more faintly.
  - **Haze** ("Rain/snow should have a haze going on"). What's farther off is greyed out by the falling rain or snow,
    the harder it falls the more. Heavy rain hides what's past about a kilometre, heavy snow what's past a few hundred
    metres. It comes in veils 40 - 80 m across that drift with the wind (the snow's own when it snows) and sink. It's
    lit by the frame's own light, greyer, and whiter in the snow. Only over what's outside, at most 85 %, and half as
    much while a look's fog is on. New *Haze* slider (0 - 2) under the rain's settings, for rain and snow.
  - **Wet handprints** ("Can you make the handprints also work for rain?"). These hang on the snow session's hand hook,
    which now also passes the contact's whole normal. Her hands get wet in a puddle (that also makes a small ring) or
    on the wet ground. They dry by half in 30 s. Wet, a hand leaves its print on dry ground or a dry wall (climbing
    under cover) in the snow's hand shape: fingers the way she goes on the ground, up on a wall, the thumb on her
    inner side. Each print leaves the hand 0.6 x as wet; it fades over 60 s.
  - **Rolls** ("Rolling/jump rolling should also do ripples if there is a puddle"). Her roll (SkillrollForward, the
    snow session's hook) in a puddle makes two big rings: one where she rolls and one 0.5 m on.
  - **Fog: how thick** now goes to 10 ("Slider fog should let you add more fog").
  - **Timing for the performance work** ("find a way to improve the performance of rain/snow ... ultra priority").
    Every 10 s while it rains or snows, the log has a "Weather (performance, diagnostic)" line:
    - game thread: the weather's work (the roofs, the pipes) and the people kept dry;
    - render thread: the wet look's work (the drips, the pipes);
    - GPU: the wet look's pass, the ice and the snow's flakes (timestamp queries, read a few frames later, never
      waited on).

    Each figure is the mean and the most. The fixes come next, from those numbers.
  - **Not checked in game.**

- **Rain: puddles checked on every surface, two landing rings, shoe-shaped prints** (build ffa02df4940a, staged: it
  installs when the game closes).
  - **The check** ("make sure that puddles and waterdown slopes apply properly on every mesh and it's not looking
    bad"). I read every term the wet look's puddles and slope water go through (no screenshots). Two problems would
    show on any mesh:
    - **Puddles on slopes further away.** Near the camera a slope gets the running water and no puddles. That test can
      only tell a slope close up: it fades from about 18 m and stops by 37 m. Past that, anything up to about 32
      degrees could get puddles, which then went as you came near. Now puddles lie only on what's level: none past 18
      degrees, all of them under 11.5. That holds at every distance, and for Faith's steps (the query round her feet)
      too.
    - **A puddle-free ring at 50 m.** Puddles were drawn only where the roof squares had seen open sky, and they reach
      50 m. Past them there were none, so a ring round the camera moved with it. Now past the squares puddles go
      wherever the wet look counts as outside (the game's interiors stay dry), as the snow already did.
  - **Two rings when Faith lands** ("when faith is jumping there should be two puddles coming out of here leg"). A
    landing in a puddle makes a ring from each leg: one where the first foot touched, the other 26 cm to her side.
    When the game reports the second foot's own contact (within 0.3 s and 0.6 m), that ring moves there. Both are a
    landing's bigger rings.
  - **Shoe-shaped prints** ("Rain/snow footsteps should have faiths shoes footprint"). Her wet prints are now a
    trainer's sole: 27 cm long, 10 cm across the ball, a round heel, the arch leaving no mark on the inner side. The
    tread's grooves (chevrons, flex grooves across the ball) are drier than its lugs. Each print is a right or left
    foot, from where it lands across the way she walks (over her last two steps). Her right is taken from the
    camera's own right, so the world's axes can't mirror it. The prints point the way she walks, not along each
    step's zigzag. The snow prints are the snow session's: it has the same sole (`ShoeSole`) and the foot's side to
    use.
  - **Not checked in game.**

- **VISUALS audit: the blur follows the lens distortion, Sprite DoF no longer crashes, enum fields fixed** (build
  6d8dc775118a, installed in the game folder and dist with the game closed, hash checked; it also has every other
  session's work in the folder at 07:19). Asked: "find every bug with visuals tab high priority make no mistakes i found
  one blur doesn't follow lens distortion", then "DoF method 2 crashed the game find out why". Notes:
  `Research_Visuals_Audit_2026-10-26.md` (outside the repo).
  - **The blur didn't follow the lens distortion.** The game bends the picture in its tonemap pass. Its Simple
    (Gaussian) depth of field runs after that pass (0x1435BFB7B) and looks the depth up where each pixel would be
    without the bend, so the blur landed beside what it should blur. The tools' own passes at Present (wet surfaces,
    rain, snow, lightning, green screen) were also drawn straight over the bent picture. Now, while *Filmic effects* is
    changed with *Enable* and *Lens distortion* ticked, the game's own bend is written off. The tools give the same
    bend to the finished frame at Present (new `Visuals/LensDistortion.*`), after their own passes and before the
    Panini projection. The bend is the game's own formula, from its tonemap shader in `systems/shaderprogramdb` and the
    scales its CPU code works out (0x1435CA05D), so the same gains give the same bend.
    - Also bent now, which the game didn't bend: its vignette and film grain (drawn in that same tonemap pass) and its
      HUD when shown. With a negative gain, the corners show a point further in, so the vignette's corners look
      lighter than in the game. Chromatic aberration stays the game's.
  - **"DoF method 2" crashed the game** (CT.log 06:50:27: exception at +0x35C72EE reading 0x8, rax 0). That address is
    the game's Sprite depth of field pass (0x1435C6810). It looks at the first of its render targets (+ 0x3B8) to
    decide whether to make them again, but only makes them after that look. Its constructor leaves them null, so its
    first frame always crashed. The game never runs that pass itself: its PC settings turn SpriteDofEnable off on High
    and Ultra ("Disable sprite dof on high and ultra" in initfs), and with it off the game puts DofMethod back to
    Gaussian (0x143759409). The menu's Sprite method turns it on. The original tools' *Sprite DoF fix* patches exactly
    that instruction. **[not known]** which entry was picked: the crash proves the Sprite pass ran, but how the menu's
    value 2 alone would reach it isn't clear from the code. Value 1 (Sprite) reaches it directly.
    - Fixed: a guard (14 bytes at 0x1435C72E4, written once when the tools start, after checking the game's bytes)
      sends the pass to its own "make the targets" path when there are none yet. It's the same path the *Sprite DoF
      fix* takes every frame, with the same registers. *Sprite DoF fix* now sets a flag in the guard (its old 2-byte
      patch sits under the guard's jump). The log says when the guard caught it.
    - *DoF method* now offers only the game's two values, Simple (Gaussian) and Sprite (bokeh). DofMethod has no third
      value in the game's type info. *Method 2* is gone.
    - The game's own SpriteDofEnable is now kept and put back when the change ends. Before, it was put back as
      "method is Sprite", which could change it.
  - **Enum fields took any number.** *Simple blur filter* (BlurFilter 0 - 6), *Tonemap method* (0 - 7, None is 7),
    *Bloom method* (0 - 2) and *Bloom direction* (0 - 2) are now lists with the game's names, from its type info. What
    an out-of-range value did in the game wasn't checked.
  - **Filmic effects controls that did nothing are out:** Vignetting (3), Frame flash (2), Depth flash (3), Heat ripple
    (9), and the edge blur's depth target scale and two matte sizes. Only three places in the game take that block's
    address (the tonemap's set-up, the blur check 0x1435CE330, the Sprite pass's set-up 0x143757E8E). None of them
    reads those fields, no code reaches them directly, and no system shader has a parameter for them. Edge blur and
    distance blur stay, but they're drawn by the Sprite pass only, so they need *DoF method* Sprite and the depth of
    field's *Enable*. The menu says so.
  - *Stop all changes* now also cancels a *Reset to game values* still waiting. Before, the reset could turn its
    effect back on 0.3 s later.
  - Checked and fine: every component's offsets against the game's type info (Color correction, Depth of field, Film
    grain, Outdoor light, Tonemap, Vignette, Filmic effects).
  - **Not checked in game:** the new lens distortion, the Sprite method with the guard, the lists.

- **Rain: kept out of people, KSec and drones** (build 7c9be91aa176, installed in the game folder and dist with the
  game closed, hash checked; it also has every other session's work up to 06:53). Asked: "rain also shouldn't go
  through people and all the ksec types".
  - **The game's rain.** The tools already keep it out from under roofs with the game's own exclusion volumes (each of
    its rain emitters culls its particles inside them). Now each person near the camera also gets a box round their
    body: KSec of every kind, the city's people and the crowds, since every character class counts. Each box is
    0.7 m wide, from the feet to 2.05 m. Drones get one too (2 m across, 1.6 m high). That's the 24 nearest within
    30 m, after the roofs: the game takes the first 32 boxes near an emitter in slot order, so roofs keep priority.
    The rain stops at them. Faith gets none (the camera is in her head).
  - **The rain the tools draw** (8 - 60 m out) skips drops inside the wet look's boxes round people, KSec, drones and
    vehicles.
  - People and drones are now tracked while it rains, even with *Wet surfaces* off (CameraManager runs DryShapes
    whenever it rains).
  - The log: "Weather: rain kept out of the people and drones near the camera too: N boxes round them", and the roofs'
    line now counts both.
  - **Not checked in game.** Not known yet: whether the game's culling (it clamps each particle to a box with the
    particle's own size) leaves a visible gap round people.

- **Rain: no smears on slopes, a slide trail, holed stairs, water steps only in puddles** (built into 6f65c523af30 and,
  rebuilt by the replay session with its own changes, 4a65c30d9fd0; not installed yet: waiting on that session's work).
  - **The "smear" on slopes.** Asked: "Smear effect still looks bad and buggy ... make no mistakes", with a screenshot:
    white streaks fanned over a sloped roof and wavy white strokes on the slope next to it. Those were the running
    water's rivulets. They were drawn as bright lines, lit by whatever is above them on the frame (a white billboard,
    the sky). Now nothing is drawn on its own. The slope's water is a sheet with low waves (about 25 - 29 cm, two
    trains crossing a little) rolling downhill, and it only bends what the slope reflects. That water is a little
    darker and nearly as smooth and reflective as a puddle. It still only covers near slopes of 14 - 41 degrees, none
    on buildings.
  - **The slide trail** ("the sliding trail doesn't work"). The log showed its rings were made, but only in puddles and
    only round where each slide effect played, behind her as she slid on. Now her slide also leaves a trail on the wet
    ground: darker and glossier, 25 cm each side of her path, coming back over 4 s. The rings in a puddle are a little
    stronger. The log says "Faith slides through a puddle / on the wet ground at ...: its trail drawn".
  - **Holed metal stairs** ("metal stairs with holes still have puddles"). The hole test now also looks at a third ring
    of 8 points, 20 cm out (at least 12 px). Before, where the metal between holes was wider than the second ring,
    every point could land on metal. **[not known]** whether that was the cause on the stairs the user saw. There's no
    screenshot of them.
  - **Water steps** ("water footsteps should play only when walking on puddles not always"). *Steps on the wet ground*
    now defaults to "As the game has them (water only in the puddles)". The other two choices are still there.
  - **Not checked in game.**

- **Fixed: the KrugerSec's guns kept firing while a replay was paused.** Pausing stopped the replay's time, but the
  gun effects it had started (muzzle flashes, tracers, blasts) went on in the game's own time until the moment the
  game had stopped them in the recording, which a paused replay never reaches. Now pausing stops those short
  effects, and lasting ones stay where they are (a VTOL's engines, a drone's lasers, Runner Vision's trail, mission
  and ambient effects). Resuming picks up from the paused moment, as after a seek. Gunshot sounds already playing
  aren't stopped by this. Not checked in game yet.
- **Rain: water on slopes only and steady, bigger ripples for landings, ripples for slides** (build 37905e2f16dd,
  staged: it installs when the game closes).
  - **Slopes.** Asked: "Slopes are tweaking and flickering also when you have puddles at 1 they disappear also they are
    really stretched out ... it should be only on slopes", then "remove it from buildings". The screenshot showed
    blotches over the far building fronts. There were three causes:
    - A wall counted as the steepest slope, so every facade got the water, far ones as a sheen.
    - The water's direction came from a 1-pixel normal, and its lanes were measured from the city's origin. A tiny
      wobble moved them a lot from frame to frame.
    - The water was taken out where puddles were, so with *Puddles* at 1 it went.

    Now the water runs only on a plane leaning 14 - 41 degrees (none under 8 or past 53: no walls or slanted
    fronts). Each pixel is checked against points 4 pixels round it, and the slope must be near enough that the
    rivulets are at least 3 pixels apart. The lanes are measured from a 256 m grid corner, there's no far sheen,
    and no puddles lie on those slopes, so *Puddles* no longer affects it. Faith's steps there aren't counted as in
    a puddle either.
  - **Landings.** Asked: jumping should make "a bigger" ripple. Her soft and hard landings in a puddle make rings
    1.8 and 2.6 times a step's: stronger, wider, longer waves that last longer. A landing's other foot makes the same
    ring bigger rather than making a second one.
  - **Slides.** Asked: "faith sliding should create a slide ripple". While she slides the game plays
    `FX/Movement/FX_Slide_*` at her feet several times a second. Each one within 2 m of her, in a puddle, leaves a
    ring: at most one every 80 ms, each lasting 1.1 s, so they make a wake. The log says "Faith slides through a
    puddle at ...".
  - **Not checked in game.**

- **Rain: drainpipes, no drips or rain inside the game's interiors, running water you can see, wet steps off the
  puddles** (build 3540937b5847, 05:4x, installed in the game folder and dist with the game closed, hash checked).
  - **Water out of the drainpipes** (VISUALS > Weather > Wet surfaces > *Water out of the drainpipes*, on). Asked: "and
    drainpipes", then "go ahead and read them" by a climbable pipe. Read live (read-only): the climbable pipe is
    `pipe_climbable/clustered_new/climbpipe_640_cluster`. It isn't a static model entity, and its model group keeps no
    transforms for it. So the pipes are found in the mesh streaming manager's instances (a sphere's middle and a mesh
    each) by name and by the mesh's own box. That covers the downpipes (`pipebackalley01/dt_backalleys_pipe_*cm`) and
    the climbable pipes. The same straight pieces also lie flat along walls (seen live), so a downpipe counts only
    when it has a second piece stacked on it or a bend under its foot. Climbable pipes stand upright. Water comes out
    of the foot, or out of the bend's mouth. Each outlet is checked once with the collision sweep: not in a named
    interior, open sky over it, not ending in a floor, ground 0.15 - 30 m under it. Up to 16 streams within 45 m are
    drawn. Each is whole at the outlet and breaks into drops further down, with a splash and rings in a puddle where
    it lands. It runs as hard as it rains, easing in over 6 s and out over 15 s. The log: "Weather: drainpipes: water
    out of N of the M pipes found ...".
  - **Droplets indoors** ("there are still some water droplets indoors"). The roof squares never found the lair's
    roof, so they had its inside open to the sky. Only the wet look checked the game's named interiors. The game's
    rain wasn't kept out there, and roof-edge drips could form inside. Now a square inside a named interior (the lair,
    the droneworks, Kingdom, Elysium, the train station's wing, ...) at the camera's height counts as covered up to
    the interior's top. That covers the boxes that keep the game's rain out, the rain the tools draw, the thunder's
    "inside", and the drips. Drips also skip any roof, landing square or fall inside one. The generic
    `ve_generic_interiortheview` volumes stay out of it (they reach over open alleys).
  - **Water flowing down slopes** ("Water isn't flowing on slops"). The first version drew 1 - 2 cm rivulets 7 cm
    apart, lit only by the frame's average light (near black at night). It also left out anything the squares had
    less than half outside, which is most walls. So it was there but couldn't be seen. Now: rivulets 12 cm apart and
    wider, a broader sheen moving down where they'd be under 3 pixels apart, and the water mirrors the blurred frame
    just above it. It only follows the wet look's own wetness now, with no extra cut.
  - **Steps on the wet ground** (Wet surfaces > *Steps on the wet ground*: *The water (as in the puddles)* by default,
    *The game's wet concrete*, or *As the game has them*). Asked: "make the water footsteps work outside as well when
    there is no peddles". Faith's steps and landings on the wet look's wet ground outside, off the puddles, now get
    the water too. The other choice is the game's wet concrete (material 47: sound id 61, concrete with a
    footplant_water layer, and the water's splashes), checked against the game's lookup once.
  - **Not checked in game.** Not known yet: which pipes get water and whether they're the right ones, the outlets' exact
    places (from the meshes' boxes, no rotation known), whether the lair is now dry of the game's rain, and how the
    new running water and the wet-concrete steps look and sound.
  - **Not fixed yet: the halo round Faith in thick fog.** The game's fog pass works per pixel except one screen-space
    glow texture (fog_2's g_forwardScatteringTexture). The tools' own pass keeps the wet look off Faith (a 0.55 m box,
    and 0.6 - 1 m round the camera in first person). Which of these it is needs a screenshot.

- **New: Faith's outfit, free roam's or the Release mission's** (GAME > *Faith* > *Outfit*; "Free roam" by default, not
  saved). Asked: "add a drop down menu to change from free faith to release faith".
  - **What Release Faith is** (read in the game's data): the same character in other clothes. The main city's player
    spawner (pf_spspawn_01) has seven outfits for her and a story flag picks one: Civilian (C_Faith_Prologue: her
    prologue head, top and trousers, with the gridlink), Civilian without the gridlink, Runner, three MagRope stages,
    and Runner with the tattoo. The Release mission sets Civilian at its start and the one without the gridlink
    further on. Free roam after the story is Runner with the tattoo (set at The Shard).
  - **Choices:** *Free roam (as the game has her)*, *Release*, *Release, no gridlink*.
  - **How:** the tools send the outfit's own "Apply" event to its entity in the spawner, as the mission's flag does
    (the game's code finds her through the spawner and calls her character's own customize). They send it at the
    start of the game's server update, on the thread the game runs it on. The story flag and the save aren't touched.
    Every outfit sets the same animation state, so her moves shouldn't change.
  - She keeps the chosen outfit through respawns, level loads and missions: each time the game dresses her itself
    (0.1 s after she spawns, or when a mission changes the flag), the tools dress her again. *Free roam* gives back the
    game's last outfit.
  - Not while a replay records or plays (the menu shows the choice greyed out then). Under the choice, a line says what
    she wears and what the game has her in.
  - The Release clothes have no MagRope on the arm; she can still use it.
  - The log: "Faith's outfit: the game dressed her in ...", "... chosen", "Faith's outfit: dressed in ... (Apply sent
    to the spawner's outfit entity 0x..., server update thread N)". If the game's code isn't what the tools know,
    nothing is hooked and the menu says "not available".
  - New `Camera/FaithOutfit.*`. `util::hooks::CreateGameHook` (end of Hooks.cpp) makes hooks from other files.
    `FaithReplay::IsBusy`. Research: `Research_Faith_Outfit_2026-10-26.md` (outside the repo).
  - Build 97871c0a4caf (04:59:31, built from the folder as it was, so with the other sessions' changes up to then).
    Installed in the game folder and dist with the game closed, hash checked.
  - **Not checked in game.** Not known yet: that the change shows and goes back cleanly, her hair and head with the
    free camera's third-person body, a replay recorded in either outfit, and whether sending the Apply at the start
    of the server update is as safe as the game's own (the two thread numbers in the log will tell).

- **New: Snow** (VISUALS > *Snow*, its own section under Weather; everything off by default). The game has no snow at
  all: no snow effect, texture or sound in its files. Toffiklubitoffi's Snowfall mod makes its snow out of a
  cutscene's camera dust and smoke puffs. The tools draw their own, over the finished frame, like the rain in the
  distance.
  - **Snow or rain:** turning snow on (*Snow* or *Random snow*) turns the rain off, and *Rain* or *Random rain* turns
    the snow off. Thunder and lightning stay under Weather and work with either.
  - **Falling snow:**
    - Flakes fixed in the world, so they stay put as the camera moves past.
    - They fall at about 1 m/s, sway a little and are carried by a wind whose way and strength wander slowly.
    - They're drawn from right in front of the camera out to 50 m, faded into the game's fog, and only in front of what
      the game drew.
    - Settings: *Snow amount*, *Flake size*, *Wind* (m/s), *Blizzard* (three times the wind, more flakes, thicker fog).
  - **Keep snow out of buildings** (on by default): flakes only where the weather's 2 m squares know the sky is open,
    so through a window it snows outside but not in the room. None where a square isn't looked at yet.
  - **Random snow:** the same kind of forecast as Random rain, with *How much of the day it snows*, *New forecast*,
    and the snowy look coming and going with it.
  - **Snow on the ground**, instead of the wet look while snow is on:
    - It settles on what faces up: patchy at first, then white all over.
    - It builds up over *Settles over* (real minutes, 4 by default) while it snows, and melts in three times that
      after.
    - Not under roofs, and not on Faith, people, vehicles or trains.
    - Its brightness comes from the light on the frame round it, so it stays in shadow where the ground is.
    - *Settle now* and *Clear* buttons; *On the ground up to* (400 m by default).
    - Past the 50 m the squares reach, a room seen through a window may get snow on its floor.
  - **Snowy city look:**
    - The fog follows the Snowfall mod's values by the hour: thick and low at night (250 m seen through), a white wall
      at noon (500 m, up to 350 m high), lighter at dawn. Never clearer than the game's own.
    - A whiter fog colour, a brighter sky light than the rainy look's, lighter clouds and less bloom.
    - As the rainy look: no sun and a grey sky. *Snow fog* sets how much of the fog. Grey sky, Less sun and *Fog: seen
      through up to* are shared with the rainy look.
  - **No snow sounds** (the user: "It's snowing we don't really need sounds"). The mod's own is a 24 MB replacement
    of the game's high-altitude wind.
  - **Fixed along the way:** the rainy look's by-the-hour fog only followed the clock with Random rain on. Otherwise it
    stayed at the last hour known (noon at first). The time of day is now read whenever a look is on.
  - Replays don't record the snow (their recorded weather is the rain's settings); a replay leaves the snow as it is.
  - Build f37d05847faf (it also has the weather session's 4bc73495ca90). Installed in the game folder and dist, hash
    checked. Built without errors; not checked in game yet.
  - **Then, after the first test** (the user: the snow "looks a bit confusing and motion draining", it "also goes
    inside the lair"):
    - **Falling snow rebuilt as real flakes.** The first build drew flakes on sheets fixed in the world. Every flake on
      a sheet moved the same way, the level sheets all fell at once, thousands of tiny points shimmered, flakes came
      right up to the lens, and every flake jumped when the camera turned. A whole field of points moving together is
      what makes you feel you're moving. Now each flake is on its own:
      - it falls at its own 0.5 - 1.3 m/s, as real snowflakes do, and wanders its own way through the air (a slow loop
        and a quick flutter) on top of the wind;
      - none nearer than 0.8 m (faded in to 2 m); few and faint far away, where the fog takes over;
      - each one is smeared a little along its own motion as the camera turns, instead of jumping;
      - nothing jumps when the snow's clock or the wind's drift wraps round.
    - **No snow inside the game's interiors.** The roof search doesn't find the lair's roof over much of it (the log
      went from "under a roof" to "under the open sky" and back as Faith walked through). Now the game's own interior
      volumes count as roofs. These are the shapes it fades its interior lighting in by: the Runners' lair, Plastic's
      lair, Kruger's apartment, The View's interiors, the droneworks, Anansi tower, the Kingdom, Elysium, the train
      station's right wing. That covers the falling snow, the snow on the ground and also the rain's wet look (no
      puddles in the lair).
    - **Checked: no puddles or reflections while it's snow.** The wet look, its puddles, ripples and reflections, and
      the water footsteps are all off while snow is on. The snow on the ground has no reflection at all.
    - Build d08ab6067b6d (it also has the weather session's 01:31 rain-sound fix). The game was running: staged, it
      installs when the game closes (after the other sessions' jobs, only over an older build). Built without errors;
      not checked in game yet.
  - **Then: waterfalls and fountains freeze while it snows** (VISUALS > Snow > *Freeze waterfalls and fountains*, on by
    default).
    - The city's waterfalls and fountains are the game's own particle effects (fx/ambient/water). The game multiplies
      every particle effect's time by a TimeScale value on its emitter, found in its code. While snow falls the tools
      set it to 0 for the waterfalls' and fountains' emitters. Their water stays where it is, standing still, and
      flows again (the game's own value back) when the snow stops.
    - A waterfall that has only just come into view, or back into range, flows for 6 seconds first so it has water to
      freeze. The others of the same kind flow with it for that moment.
    - Not frozen: the water drops on the lens near a waterfall, and waterfalls inside the game's interiors (the
      Kingdom's).
    - Their sound keeps playing. Pools and streams (the water planes) aren't frozen: their shaders move them and can't
      be stopped from here (see the research notes).
    - The menu shows how many waterfalls and fountains are near and how many of their emitters are frozen. The log
      says when they're frozen and when they flow again.
    - Build 6968bdcef559 (also the weather session's 01:58 grates and stairs change). Installed at the 02:1x game close
      (game folder and dist, hash checked twice, after the other sessions' jobs). Built without errors; not checked in
      game yet.
  - **Then: snow intensity, the blizzard, the freeze and the snow on the ground** (the user: "lets add a snow intensity
    slider right now it's too much of a light snowing", "blizzard tick doesn't work anymore", "these waterfalls are not
    frozen", the snow on the ground "has no depth now and it looks bland"):
    - **New *Snow intensity*** (0.1 - 3, 1.5 by default) in place of *Snow amount*: under 0.7 light, 1 moderate, from
      2 heavy. There are now up to 84,000 flakes (60,000 within 16 m, 24,000 out to 48 m), up from 34,000; intensity 3
      draws them all, the 1.5 default half. With the snowy city look its fog is also no clearer than 1500 m / the
      intensity (500 m at 3).
    - **The blizzard fixed.** It only multiplied the old amount by 1.5 up to a cap, so from about two thirds of the
      amount up it added no flakes, and its fog came only with the snowy look ticked. Now a blizzard is the intensity x
      1.8 (up to 3), three times the wind, and the whiteout fog (seen through 250 m at most) even when the snowy look
      isn't ticked.
    - **Flakes streak with the wind.** Each flake's smear now follows its own fall and the wind carrying it over the
      frame, as well as the camera's turn. In a blizzard they streak sideways.
    - **The freeze fixed.** A waterfall that went out of range and came back counted as new, so its whole kind kept
      flowing again, and only a few kinds ever froze. While frozen, the waterfalls' and fountains' effects are now kept
      drawn out to 3 km (the game's own distances go back when the snow stops). A kind freezes once its running effects
      have flowed 6 s, or after 20 s at most. Only a waterfall that has just loaded makes its kind flow again for a
      moment.
    - **Not frozen: the waterfalls on the buildings' walls** (in the user's screenshot). They aren't particle effects:
      they're models whose water is moved by their shader, which can't be stopped from here. An ice look over them is
      the next step; it's not in this build.
    - **Snow on the ground with depth.**
      - Drifts a couple of metres across, softer lumps under a metre, and grains of a few centimetres, all in its
        height.
      - Their slopes are lit from the overcast sky: the tops white, the slopes and hollows darker and a little blue,
        like snow in shade.
      - A darker edge where a patch ends, and a little parallax so the drifts stand out as the camera moves.
      - Glints near the camera: now and then a crystal catches the light.
      - The relief and the glints fade with distance, so far snow stays smooth instead of shimmering.
    - Built as 75e5bf562e02 (without errors; all five shaders compiled, and the constants' places are checked against
      the shaders' at build time). Not installed: 793c107f9ccf below has all of it and replaces it in the install job.
  - **Then: Faith's steps crunch in the snow** (the user: "for the snow in the ground can we have a snow walking sound
    effect?").
    - The game has no snow sounds. The nearest it has is its gravel: a crunch for walking, running, sprinting,
      landing, sliding and stopping. Faith's steps and landings where the snow lies on the ground now use it. Only the
      sound changes: no gravel dust or pebbles fly up.
    - Only where the snow is drawn at her feet: on ground that faces up, where enough has settled for a patch to be
      there, and where the weather's squares know the sky is open. Not under roofs or in the game's interiors.
    - The tools check once per level that the game's material for it really is the gravel. If it isn't, the log says
      so and the steps stay as they are.
    - Built on the same hook as the steps in the puddles: the game's own footstep sounds, with a different ground
      under her foot. Wall runs, hands and climbing aren't changed.
    - Also in this build: the freeze's help text says the waterfalls are kept drawn out to 3 km while frozen, and that
      the ones on the buildings' walls aren't frozen.
    - Built as 793c107f9ccf without errors. At the 02:5x game close the weather session's newer 9735bd30d184 went in
      instead (game folder and dist), built from the same folder. It has all of the above: the snow strings, and all
      five shaders' bytes checked in it. Not checked in game yet.
  - **Then: ice over the water while it snows** (forwarded from the user: "these waterfalls are not frozen and frozen
    water should have a froze shader like in real life", with a screenshot of water running down a building's wall).
    - The water down the buildings' walls, the Anchor's running-water walls and the pools aren't effects that can be
      stopped. They're the game's models, and their water is moved by their own shaders. While the waterfalls are
      frozen, the tools now draw ice over them:
      - **down the walls:** ridged ice with uneven icicles at the foot, pale blue where it's thick and white along
        the ridges, catching the light;
      - **on the pools:** clear ice with the water dark under it, frosted white patches and white cracks, the sky
        reflected in it at a low angle. The snow then lies on it.
    - It forms over 20 s and goes 10 s after the water flows again. It's lit like the scene round it and fades into
      the fog.
    - Where they are: read from the game's own city data. That's 64 streams down walls, 8 running-water walls and
      135 pools and fountain basins. Each is a flat rectangle (the meshes' own sizes through each placement).
    - Left as water: the huge plane at the foot of the city (436 x 229 m: open water one snowfall wouldn't freeze),
      the indoor pools (the game's interior water and the Kingdom's), and anything inside the game's named interiors.
      Also the fountains' curved basins and the Anchor's triangular pool, whose shapes aren't rectangles.
    - **The streams' own splashes, mist and drops now freeze too.** Their emitters are named Em_WaterStream_* and
      weren't caught before. The drops on the lens still move.
    - Their own water may still show faintly through the ice. How wide each stream's visible water is within its
      1.28 m isn't known yet, so the ice's sides fade from about half way out.
    - Build 16ff46de416f (with the weather session's 9735bd30d184 work in the folder). Installed in the game folder
      and dist with the game closed, hash checked; all five shaders' bytes checked in it. Built without errors. Not
      checked in game yet.
  - **Then: icier ice on all the water, heavier snow, snow that piles up, footprints, a footstep tick, the wind**
    (the user: "water snow shader is still not icy", "make it find any instance of water", "can we make the snow
    slider more intense and the snow on the ground look like it's piling up?", "can we do the same thing as the water
    footseps but with snow?", "add a tick for the snow sound footsteps", "there should be a wind sound playing when
    outside and muffled when indoors and the wind sound should get more intense if it's a blizzard same with the
    snow").
    - **The ice looked like water, then snow.** The 04:21 log showed the ice working: Faith stepped in the game's
      water at a pool in the table. But the pool's ice let the water show through, darkened, its frost turned grey
      at night, and then the snow lay over all of it, so a frozen pool looked like snowy ground.
      - Now the pools' ice is opaque: frosted white over most of it, clear dark blue-grey patches, fine frost grains,
        white cracks, the sky's light glancing off it. The water's motion no longer shows through.
      - Only a thin dusting of snow lies on it (the wind clears smooth ice).
      - The streams' and walls' ice is opaque too, whiter, with frost grains.
    - **Every piece of water, found.** The table now has every mesh the game draws with one of its water shaders,
      471 pieces in all.
      - Whole water meshes: the streams, pools, running-water walls, the Anchor's triangular pool, The View's small
        splash pools.
      - Water inside bigger meshes, read from those meshes' own triangles: the canals in the Anchor's ground, the
        harbour, the station's water, the plazas' pools (Anansi tower, the Anchor's area 2), the far sea, the
        fountain features' curved falls (as boxes).
      - Also the large water at the foot of the city, left out before.
      - Still left as water: the indoor pools and anything in the game's named interiors; the puddle decals.
      - The shader now takes the nearest 48 pieces, by their true distance.
    - **Heavier snow.** *Snow intensity* now goes to 5: up to 210,000 flakes, 2.5 x the most before.
      - Each place on the slider has 1.5 x the flakes it had.
      - From 2 up the flakes grow, clumped as in heavy snow (x 1.5 at 5).
      - The snowy look's fog goes down to 300 m at 5.
      - A blizzard is still x 1.8, now up to 5.
    - **Snow that piles up.** Once it all lies it keeps deepening, to *Piles up to* (0.25 m by default, up to 1 m),
      over three times *Settles over*. It deepens faster in heavier snow and melts with the rest.
      - As it deepens it fills the small gaps it left (grates, edges), its drifts grow taller and its patches go.
      - Its layer's side shows near the camera: over ledges, curbs and steps, and against the foot of walls where
        the ground's snow lies.
      - *Settle now* makes it full depth at once, *Clear* takes it away. The menu shows how deep it is.
    - **Footprints in the snow.** Faith's steps and landings where the snow lies press a print (her last 64). The
      falling snow fills each one over about 5 minutes at intensity 1, faster in heavier snow; while it doesn't snow
      they stay.
      - A print's floor is packed down, a little greyer and bluer, and its inner wall is in shade, seen deeper as
        the camera looks in.
      - The prints are pressed deeper the deeper the snow is (3 - 20 cm).
    - **New *Footstep sound in the snow* tick** (on by default). Off: her steps sound like the ground under the snow,
      and the prints are still left.
    - **New *Wind sound*** (on by default, with its own volume): a wind heard all round while it snows.
      - It's louder and brighter the harder it snows and the faster the snow's wind blows; the gusts are heard
        swelling and dying.
      - Much more so in a blizzard.
      - Muffled and quieter under a roof or inside the game's named interiors (the lair included).
      - It plays through Windows' audio at the game's volume, only with the game's window in front.
      - The weather session, making a wind for the rain at the same time, overwrote the snow wind's loop (WindSynth.h)
        by mistake, so this build plays the snow's wind with the rain wind's loop. Settled afterwards: there's one
        wind voice (the snow's), which the weather session also drives for the rain. The snow's own loop (a body, a
        howl in the gusts, a whistle in the strongest) is back in the folder and goes in the next build.
    - Build 1f7a197ac284 (with the weather session's work in the folder). Installed in the game folder and dist with
      the game closed, hash checked; all five shaders' bytes checked in it. Built without errors. Not checked in game
      yet.
  - **Then: snow from every side, slide trails, two-foot jumps, deeper and shallower snow, the ice's overreach, no
    snow layer on ceilings** (the user: "Why does snow come from only one direction? and not from above and
    left/right?" "when wind is on"; "there should be a slide trail and jumping should leave two footprints behind",
    "on the snow"; "check if the snow can have different variations of height"; "the shader overextends here" with a
    screenshot; "snow shouldn't be on sealings").
    - **Snow from above and both sides.** Every flake rode the one wind, at 1 m/s about as fast as it falls. That
      wind's way only wandered over minutes and its gusts came every half minute or more, and each flake's own loop was
      too small to cross it. So the snow came at a slant from one side, every streak the same way. Now:
      - every flake swirls in two eddies of its own (a slow one 0.4 - 1.2 m round, a quick one 0.1 - 0.35 m), each
        turned its own way and tipped up and down. Near flakes swirl apart, some cross the wind, some drop straight
        down, a few rise for a moment. Their size grows with the wind and in a blizzard;
      - each flake's streak follows its own motion;
      - the wind gusts every few seconds, swinging its way about 40 degrees either way;
      - *Wind* is 0.5 m/s by default (it was 1), so light snow falls mostly from above.
    - **The ice no longer spreads past the water** (the user's screenshot: a rectangle of ice over the ground beside
      a canal). The water inside the ground's, plazas' and station's meshes was made of rectangles fitted round its
      pieces. Many pieces are a single big triangle, so the rectangle covered as much ground again. Now those are the
      water's own 1,078 triangles (and the Anchor's triangular pool's), so the ice ends where the water does. The
      pools and streams that really are rectangles stay rectangles.
    - **Less speckled pool ice:** from far off its clear patches made dense dark specks, like granite. Now the clear
      patches are big and few, and the fine frost only grains the white.
    - **Slide trails in the snow.** While Faith slides (the game's slide effect plays near her), a groove as wide as
      she is follows her through the snow. It's pressed as deep as her prints, with the snow pushed up along its sides,
      and the falling snow fills it again like the prints.
    - **Jumps leave two prints.** The game has no take-off event, so it's seen from her feet: rising fast just after a
      step in the snow presses that step's other foot beside it. A landing (she was falling fast) presses both feet
      side by side, deeper.
    - **Deeper and shallower snow.** How deep it lies now varies by place, 0.45 - 1.55 x the piled depth over humps and
      hollows 5 - 10 m across. Its layer over ledges, its prints and trails and its drifts follow it. While it's thin,
      the thinner places show the ground through first.
    - **No snow layer on ceilings.** The piled layer's side looked for tops below each pixel and took any level
      surface, a ceiling included. Now a top must face up, seen from above. The snow on the ground itself already
      needed that.
    - **Not found yet: what looks weird after Faith dies.** The 06:08 log shows her death and respawn 13 s later and
      nothing failing. The game's own fog was different after the respawn (seen through 5000 m, no fog floor), so the
      snowy look started from a different place. Needs a screenshot or a description.
    - Build 02b98f7871e9 (with the weather session's work in the folder, its staged 37905e2f16dd's included).
      Installed in the game folder and dist with the game closed, hash checked; all five shaders' bytes checked in it.
      Built without errors. Not checked in game yet.
  - **Then: the snow on the ground rebuilt to look like snow, and smooth slide trails** (the user: "snow shader looks
    really bad find a way to make it look better", with a screenshot, "i think thats the height map"; "The slide
    effect should smooth out if it's back to back the repeated patterns look bad").
    - **What was wrong** (the screenshot: the ground mottled blue-grey every few decimetres, like rippling water):
      - its relief was blocky value noise, taken as up to a metre of height once the snow piled up, and shaded down to
        x 0.72 and blue;
      - its brightness came from the pixel under it, so the ground's own texture and reflections showed through;
      - the prints were flat, saturated blue ovals.
    - **Now:**
      - Smooth wind-shaped drifts 4 - 8 m across with softer swells (gradient noise, its octaves turned against each
        other so no grid shows), 5 - 30 cm tall, more where it lies deeper.
      - Shaded only a few percent, lit from the overcast sky a little more from one side so the drifts show.
      - Bright and even: lit by the light round it (blurred), so no texture shows through, but still in shadow where
        the ground is. A cool white, a little bluer in shadow.
      - Up close a fine grain and the glints, both fading with distance.
      - Prints and trails only a little greyer where packed down, their walls in soft shade, a rim of pushed-up snow
        round them. Their edges are soft.
    - **One smooth slide groove.** Each 0.35 m stretch of a slide was drawn as its own groove, rounded at both ends
      and laid over the next, so every joint showed, and slides run over each other repeated it. Now the whole trail
      is one groove (the nearest of its stretches makes it), with a rounded floor. Back-to-back slides merge into it.
    - Built as cce83ec97dd3 (only WetLook.hlsl changed) without errors. At the 06:5x game close a newer build from the
      folder, 2c5e40b71424 (06:51), was already installed in the game folder and dist, so mine wasn't copied. It
      has this work: the current shaders' bytes (built from the WetLook.hlsl with this rebuilt snow) are checked in
      it. Not checked in game yet.
  - **Then: clean, soft snow edges, smoother noise, ice from the water's own triangles, prints of her hands, body and
    rolls, more fog, real blizzards** (the user: "Snow shader looks pixeladed and bleeds over edges it shouldn't do
    that high priority", "the blur from shader is hiding UI elements", "There is still some water flickering", "faiths
    hands should also leave handprints behind make them model accurate", with a screenshot of an indoor fountain:
    "these waterfalls are still running and the frozen water has some weird alpha issues", "Slider fog should let you
    add more fog same with snowfall", "snow slides should have faiths body outline instead of circles", "Every object
    under snow looks grey" ("object vfx / UI element"), "Blizzards should be like in real life where there is a ton of
    snow going all over the place and its really foggy", "snow should be more smooth around the edge but still no
    bleed or flicker", "snow noise pattern should be more smoothed out", "Rolling/jump rolling should also leave
    faiths shape on the snow"; and from the rain session: "Rain/snow footsteps should have faiths shoes footprint").
    - **No bleed or pixels at edges.** There were three causes:
      - the flatness test was relaxed as the snow piled up, so deep snow went onto the pixels where the depth jumps (an
        edge's), pixel by pixel;
      - the piled layer's side was drawn over whatever was behind a ledge's edge;
      - glints a pixel wide read as noise.
      Now the test stays, the side is drawn only against the surface it lies on, and glints only where they're bigger
      than a pixel.
    - **Soft edges, still no bleed.** The snow fades over the last 3 - 4 pixels inside an edge. Each pixel counts how
      many of 12 points round it are on its own surface, so it only ever takes snow away (none past the edge). A count
      of 12 doesn't flip from frame to frame as one pixel's test did.
    - **Smoother noise:**
      - The snow's light was read from a blurred copy of the frame texel by texel. Those 16-pixel squares and
        diamonds showed in it and crawled as the camera moved. Now it's read as a smooth cubic (B-spline). The ice and
        the piled layer's side use the same read.
      - The patches while it settles are gradient noise now. They were value noise on a square grid, with speckles at
        their edges. Her steps' test uses the same noise.
      - The drifts' smallest ripples are halved and their shading is a little softer. The parallax at a glancing view
        is half what it was, and the fine grain is gradient noise, shown only close up.
    - **The ice is drawn from the water's own triangles.** It was approximated with rectangles, the nearest chosen
      each frame, so it flickered as the choice changed and reached past the water. Now all of the city's water (239
      rectangles and 4,462 triangles, the fountains' falls included) is one vertex buffer drawn every frame, with no
      choice to change. It's ice only under the open sky (the indoor fountain stays as it is), and a fall's icicles
      hang from its own lowest point.
    - **What the game draws over the ground keeps its colour.** Effects, the Runner's Vision and the HUD don't write
      the depth, so the snow and the ice were laid over them as if they were ground. Now a pixel much brighter than
      round it, or strongly coloured where round it isn't, is left as it is. Broad grey things (smoke, steam) still
      can't be told from the ground.
    - **Her shoe's sole in the snow:** the rain session's trainer sole (`ShoeSole`) with each foot's outer side, the
      tread pressed into the floor close up.
    - **Hand prints.** Her hand impacts (HandLand, HandLandLeft / Right, HandClimbLeft / Right, HandScuffLeft / Right,
      on her hands' bones) press a hand where the snow lies: a palm, four fingers and a thumb. They're an adult woman's
      proportions (ANSUR II), not measured from her model. Releasing a hand leaves none.
    - **Slides in her body's shape.** The trough is as wide as her hips (37 cm), from where her hips were as she began.
      Its last 0.5 m is her two legs side by side, ending at her feet.
    - **Rolls.** The game's one impact for a roll (SkillrollForward) presses her back's shape: from the shoulder she
      rolls over (26 cm wide) to the other hip (32 cm), 0.6 m apart, with her shoulder and upper arm rounded out at
      its side. Her feet are followed for 0.9 s for the way she rolled. Not measured from her model or her roll. The
      shoulder is her right, not checked against her animation. The log says, once a while, how far the game's contact
      for a roll was from her feet, so the print can be placed better.
    - **More fog and more snow.** *Snow fog* goes to 3 (its fog up to 3 times thicker), *Snow intensity* to 7.5 (up
      to 310,000 flakes).
    - **Blizzards:**
      - the wind at least 12 m/s, in gusts of 0.2 - 1.8 x;
      - eddies 2.5 - 5 m across;
      - at least intensity 5 (x 1.8 of the slider, up to 7.5);
      - its fog 1.5 x thicker, seen through 120 m at most;
      - the flakes' streaks up to 90 px.
    - Built as 92f8a52021b4 from everything in the folder at 15:52 (so the other sessions' work too). Its seven
      shaders' bytes were checked in it. Installed in the game folder and dist with the game closed, hash checked.
      Not checked in game.
  - **Then: no snow over edges seen from an angle, and Faith's own hand** (the user: "Snow shader still bleeds around
    edges when viewing it from an angle"; and, through the rain session, "hand prints should match faiths hands").
    - **Edges seen from an angle.** To tell which way a pixel's surface faces, the shader used its nearest neighbour
      each way. Seen at a glancing angle, a ledge's front face is stretched far back in depth. So for the face's top
      row, the nearest pixel above was on the ledge's top, across the corner. That row counted as facing up and took
      the top's snow: a strip over the edge you only saw from an angle. Now, for the snow, each pixel takes its facing
      from the side whose next two pixels line up with it, so a pixel at a corner faces the way of its own surface.
      Pixels that line up with neither side (slivers a pixel wide) get no snow. The depth wasn't the cause: it's the
      frame's full size (3840 x 2054 in the log). The rain's wet look keeps its own test.
    - **Hand prints in her hand's shape**, measured from the game's own data instead of average proportions. Her arms
      mesh was laid flat on her skeleton (each finger straightened along its joints, the thumb beside the palm) and its
      outline drawn:
      - her hand is 17 cm from the heel to the middle fingertip and 7.2 cm across the knuckles;
      - her fingers, from the knuckle joint: index 8.3 cm, middle 9.2, ring 8.7, little 6.8; 17 mm wide at the base
        down to 12 mm (the little one 15 to 10), leaning a little to the little finger's side as hers do;
      - her thumb is 10 cm from its base, 30 degrees out from her hand, with the pad at its base.
      The print (a rounded palm, tapered fingers, the thumb and its pad) covers 92.5 % of her outline's area; most of
      the rest is the wrist. The rain's wet handprints use the same shape. Not known: where the game's contact point
      for a hand touch is under her palm (the print is centred on it).
    - Built as c4c9343f75d7 from the whole folder, so it has the rain session's speed-ups and its shoe and hand drying
      too. Its seven shaders' bytes are checked in it. Installed in the game folder and dist at 16:42, after the game
      closed (hash checked). Not checked in game.
  - **Then: far snow that stays, solid piles against walls, haze over every building, a snowy look that isn't too
    bright** (the user: "snow effect disappears when looking straight when it shouldn't", "piled up snow looks weird
    fix it do more research and fix it", "snow haze/fog looks weird" with a screenshot, "ticking snow city look makes
    everything really bright").
    - **Far snow looking straight ahead.** The last build's test for one-pixel slivers measured how far a pixel was
      off its neighbours' line in pixel widths. Looking straight ahead, neighbouring pixels on the far ground are 0.2 -
      1 m apart, so a slight slope of a degree or two failed it and the snow went. It now measures how sharply the
      surface bends at the pixel (none under 17 degrees).
    - **Piled-up snow.** Where snow lies against a wall or a curb, what covers its foot is the ground's snow going on
      up to it. It was drawn as a separate band:
      - lit by the wall's own pixel, so the wall's texture showed through;
      - patterned with diagonal stripes;
      - see-through over its top quarter, with a wobbly edge.
      Now it's the ground's snow itself, lit and coloured the same, solid to a smooth edge on the wall, a little greyer
      in the corner. The snow also no longer fades before every wall: the edge fade now counts only where the surface
      drops away, which took away the bare strip between the ground's snow and the wall.
    - **The haze over buildings with an interior.** The haze was left off anything inside one of the game's interior
      volumes, and their sides reach past each building's walls. So in the screenshot a tall blue tower stood crisp
      in the haze while nearer buildings faded (the Anansi tower's volume is 491 m tall; which tower that was isn't
      confirmed). The haze is the air on the way, so with the camera outdoors everything is hazed by its distance; with
      it indoors, only what's outside.
    - **The snowy city look's brightness.** It used the Snowfall mod's fog colours, 5 - 8 times the game's own at night
      and dawn. The mod tuned them for its own darker exposure, which the tools don't apply, so the city went bright
      grey. The fog now keeps the mod's whiter tint at the game's own brightness for the hour, as much as *Snow fog*
      says. The look's extra outdoor sky light (for the snow's light bouncing back up) is down to the rainy look's,
      since the game's lighting never sees the tools' snow.
    - **Not done:** the haze's colour still comes from the frame's average, so it shifts a little as you look round.
      The water's flicker is the rain session's.
    - In the rain session's build 9adfe19b2804 (18:14), built from the whole folder: all seven shaders' bytes are
      checked in it, the haze's new constant in their layout. It's in the game folder and dist (hash checked). Not
      checked in game.
  - **Then: no stripes up walls, snow on gravel, none on light panels or the map, ice in the haze, haze that doesn't
    flicker, and its own slider** (the user, with a screenshot of white stripes up a stack of boxes: "Snow shader on
    gravel looks weird ... it also shouldn't affect light panels and depth issue is still there"; "blizzard ticked
    makes the map menu all white make rain/snow not affect main menu and map menu"; with a screenshot: "frozen water is
    still visible on fog and doesn't blend in with it"; "fix any haze flickering on buildings ... the same applies for
    rain"; "snow intensity shouldn't add any fog it was a bad idea make a diffrent slider for that").
    - **The stripes up walls and boxes.** To find the top a pile stands on, the snow looked at 4 fixed points straight
      down the screen. A thin top, like the gap between two boxes, was hit only on the rows where one of them landed on
      it: 4 bands, toothed at their ends. Now it follows the wall down to its foot, a march and then 3 halvings. The top
      there must carry on below it, so a thin strip doesn't count, and the wall is covered as high as the snow lies
      there.
      - Your log had the snow 82 - 127 cm deep: *Piles up to* near its 1 m most, and the depth varies x 0.45 - 1.55
        by place.
    - **Gravel.** The snow's edge fade counted every dip, so on gravel it went grey and patchy, solid only round
      what stands on it. A dip smaller than half the snow's depth (at least 3 cm) no longer counts, since snow that
      deep buries the stones. Where the snow is deep, the ground's facing is also judged over 4 pixels, so a single
      stone doesn't break it up.
    - **Light panels.** A pixel whose brightest channel is near full and 2.5 - 3.5 times the frame's average counts as
      glowing (a light panel, a lit sign, a lamp) and gets no snow or ice. Not measured from the game's panels.
    - **The map and the menus.** While the game's menu or map has it paused, the tools draw no rain, snow, ice or haze,
      and the snowy look's fog is taken back, so the map's view of the city doesn't go white in a blizzard. The tools'
      own freeze and game speed don't pause it that way, so photos keep their weather. That the map pauses the game the
      same way the menu does is assumed: the log says when it switches. The drawn clock keeps its base while paused (and
      on the loading screen), so nothing jumps when drawing goes on.
    - **Frozen water in the haze.** The ice is drawn after the pass that hazes everything, so in a whiteout it stood
      out sharp. It's now hazed as much as what's behind it, in the same colour.
    - **The haze flickering on buildings (snow and rain).** The haze's veils came from noise sampled 15, 60 and 200 m
      along the view, but only the samples nearer than the point counted. So at 60 m and 200 m the average jumped,
      the haze up to a fifth thicker or thinner across a line on a wall, and as the camera moved those lines swept
      over the buildings. Now all three always count (one sits at the point itself where that's nearer), so the haze
      changes smoothly with distance. (Earlier, with the camera outdoors, the haze over buildings also stopped
      depending on the weather's 2 m roof squares.)
    - **Snow intensity adds no fog.** The snowy look's fog was capped at 1500 m divided by the intensity, and the
      snow's haze grew with it. Both are gone. The haze now has its own slider, *Snow haze* (0 - 3), in the Snow
      section: at 1 what's past about 1.4 km is hidden, at 3 past about 450 m, and a blizzard is 3.4 times thicker.
      The look's fog stays under *Snow fog*. The Weather section's *Haze* is the rain's now.
    - **The half-frozen channel at the Anchor.** There's no render distance for the ice: all of it is drawn every
      frame, however far. The Anchor's channels are `waterplane_01` quads scaled to 1.28 - 1.3 m wide strips, and the
      ice covers each exactly (the quad is ±2.56 m, centred on its origin). There's no second strip beside that one in
      the table, so the rest of the channel looks like dry tiles under snow. If the water covers the whole channel in
      the game, an instance is missing from the table.
    - Built as ddd980ae6511 from the whole folder (the rain session's 19:56 work included). All seven shaders were
      compiled with /O3 and their bytes checked in it. Installed in the game folder and dist with the game closed, over
      the rain session's aa7d1a56667d (hash checked). Not checked in game.
  - **Then: the ice no longer goes when seen at an angle** (the user: "looking at a certain way makes the iced water
    shader go away"). The light-panel test of the last build was on the ice as well: a pixel near full brightness and
    2.5 times the frame's average counts as glowing. Water reflects the sky most when seen at a glancing angle, so
    seen that way the water under the ice passed the test and the ice was taken off. The ice no longer uses the test
    (water is never a light panel). The snow still does, but not at a glancing angle (less than about 12 - 27
    degrees off the surface), where that brightness is the sky's reflection on glossy ground; a panel glows from
    any angle. Built as 2545ed958ae1 from the whole folder (all seven shaders /O3, their bytes checked in it). A newer
    build from another session, 7eab193cbc2f (21:10), was installed first. It has these shaders (their bytes checked in
    it), so this fix is in the game. Not checked in game.
  - **Then: sharper snow, and no pile slider** (the user: "Snow looks blurry and pile slider is not working at all with
    what we want it to do lets remove it or find better ways").
    - **Sharper.** The snow took its light from the frame blurred over about 16 pixels (at 4K), so the ground's own
      texture wouldn't show through. That also softened every shadow on it by 16 - 32 pixels, and smeared the colour of
      whatever stood on it into a halo round it. Now its light comes from 8 points 8 pixels round it, counting only
      those on its own surface. So shadows on it are about 8 pixels soft, nothing standing on it bleeds into it, and the
      texture still doesn't show (each point is a 2 x 2 average). Its edge fades over 2 - 3 pixels instead of 3 - 4.
      The snow against the foot of a wall is lit from where it lies on the ground, so the two match.
    - **No *Piles up to*.** It let the snow pile up to 1 m (and 1.55 m where it lies deepest), so the snow against walls
      and boxes climbed as high: the stripes, the blobs and the halos round things. The slider is gone. The snow now
      piles to 12 cm by itself as it settles, enough to press Faith's prints deep, fill the ground's small gaps and lie
      in a low line against the foot of walls. It melts as before.
    - The purple city fog the user reported at the same time ("for som reason city fog is now purple", the snowy look
      at about 8:00) is the rain session's fix: the snow's haze is now a cool white with none of the morning sky's pink.
    - In the rain session's build d606b19448aa (23:42), built from the whole folder with this work in it: all seven
      shaders' bytes checked in it, "Piles up to" gone. It's in the game folder and dist. Not checked in game.
  - **Then: no more purple, and *Snow fog* up to 10** (the user: "also everything still looks purple is that fixed?",
    "fog slider should make it more foggy move it up to 10").
    - **The purple.** It was still there with the haze fix in (the user's game at 23:55 had it), so the haze wasn't the
      main cause. Both city looks grey the sky's Rayleigh scattering (what makes a clear sky blue), but the snowy
      look left the game's ozone on. Ozone takes out mostly green and some red, and little blue, which the blue
      scattering hides in a clear sky. With the scattering grey, what was left went magenta: the sky, the fog's light,
      and the sky's light on everything. The rainy look has always taken ozone out (as the Rain mod does), and now the
      snowy look does too. Not checked in game.
    - ***Snow fog* up to 10.** Past 1 it makes the snowy look's fog that many times thicker: seen through that much
      less, reaching that much higher. It also now lowers the fog's floor, the least it lets through. That floor had
      stayed, so at dawn 60 - 70 % of what's behind it always showed through, however thick the fog was. A blizzard
      is 1.5 times thicker still. The rainy look's fog is unchanged.
    - Built as 07eede99cea0 from the whole folder (no shader change; all seven shaders' bytes checked in it).
      Installed in the game folder and dist with the game closed (hash checked). Not checked in game.
  - **Then: the snow holds out to its reach, and frozen water doesn't stand out from far off** (the user, with a
    screenshot from a rooftop: "when i'm really far you can see the frozen water shader / snow layer").
    - **The snow's reach.** *On the ground up to* is 400 m by default, but the snow didn't hold until there. It thinned
      evenly from the camera out: 75 % at 100 m, half at 200 m, a quarter at 300 m. So from a rooftop the ground
      below was nearly bare, and only the ice, drawn however far, stood out as a dark, blotchy layer. Now the snow
      (and the snow against walls) is full to three quarters of its reach and fades over the last quarter, 300 - 400 m
      by default.
    - **The ice.** It fades over the same last quarter, so it never shows where the ground has no snow round it.
    - **Snow on the ice.** It now covers the ice as the snow piles: a light patchy dusting at first, then once about
      8 cm lie, nearly all of a pool, with a few patches the wind keeps clear. Far off, from 80 m and gone by 250 m,
      the dark clear-ice patches even out, so a far frozen pool is frosted white instead of a mottle.
    - **Not found yet:** "Looking up or really up the snow bugs out". Nothing in the flakes' code breaks when looking
      up, so a screenshot is needed.
    - Built as 36d2b20ed37f from the whole folder: all seven shaders /O3, their bytes checked in it, and the rain
      session's footprint facing change in it too. A newer build, 000c67fb447f (01:02), was installed first. It has
      these shaders (their bytes checked in it), so this is in the game. Not checked in game.
  - **Then: no grey snow when looking up, and crisper snow and ice** (the user: "snow/ice looks is foggy and blurry /
    fuzzy also looking up make the shader grey").
    - **Looking up.** The snow worked out how bright it should be, and how much it was in shadow, against the whole
      frame's average. As the camera looked up, the bright sky raised that average, so the snow lower down counted as in
      shadow: darker and bluer, grey. The ice's light had the same floor. Now both go by the light of their own
      surroundings (the frame blurred about 128 px round them at 4K), which the sky only touches near the horizon.
    - **Crisper.** The ice was lit by a 16-pixel blur of everything, so it was fuzzy. It's now lit as the snow is: by
      its own surface's points round it, not what stands on it. Those points are 6 pixels out now (8 before), so
      shadows on the snow and ice are about 6 pixels soft.
    - **"Foggy":** not changed beyond that. A screenshot of where it still looks foggy (near or far, in a blizzard or
      not) would show what's left.
    - In the replay session's build ce93e0590303 (01:35), built from the whole folder after these shaders were in: all
      seven shaders' bytes checked in it. It's in dist, and its install job puts it in the game folder once the game
      closes. Not checked in game.
  - **Then: the snow checks the surface at pixels' centres, and looks the same at any resolution** (the user: "fix
    the snow's between-pixel sampling (RelativeAtTexel) and scale its pixel radii with the resolution", after the rain
    session found the same fault in the puddles).
    - **At pixels' centres.** Three of the snow's tests looked at points between pixels: its soft edge, the light it
      takes from its own surface round it, and the search for a wall's foot. Each such point took the depth of the
      pixel round it but sat on the ray through itself, off the surface by up to half a pixel's step in depth.
      Looking at the ground that's nothing. Looking along it, the depth changes fast from pixel to pixel, so those
      points seemed to drop away and the snow's edges thinned. Each point now sits on its pixel's own centre. The sky
      seen past a top counts as its edge, so the snow fades smoothly there.
    - **Any resolution.** The snow's sizes in pixels were 4K's at every resolution. They now cover the same share of
      the frame at any size: the edge's 1 and 2.5 pixels, the light's 6, the wall's foot's 64 and 3, the wider facing's
      4 (whole pixels), and the flakes' longest streaks. At the user's 3406 x 1908 they're 0.88 of what they were.
    - Built as a235e8724903 from the whole folder (all seven shaders /O3, their bytes checked in it). Staged; a newer
      build (74c52b5aab24) was installed first, and the build below has it too. Not checked in game.
  - **Then: snow that's white, not a grey smear** (the user: "snow still looks blurry and it just looks like a gray
    thing on the ground find a better shader again look up how actual snow is").
    - **How real snow looks.** Fresh snow reflects 80 - 90 % of the light, nearly evenly in every direction, so under
      an overcast sky it's about as bright as the sky near the horizon. That's why snow and sky run together in a
      whiteout (F. Kasten, Polarforschung 1963, on the Greenland ice sheet; MPR News on snow's albedo). Its sparkle is
      single crystals catching the light (80.lv on snow shaders; Deliot and Belcour, HPG 2023, on glints).
    - **Why it was grey.** The snow took its brightness from the ground under it: that ground's own light round it,
      x 1.9. On dark asphalt, which reflects about a fifth of the light, that made a mid grey: a brightened, blurred
      copy of the asphalt, not snow.
    - **Now.** The snow is 0.9 of the sky's brightness by the horizon, as the frame shows it (the rain session's steady
      reading of the frame's sky). It's up to a quarter darker where its own surface is darker than round it (a
      shadow, a corner), and never darker than before, so sunlit snow stays bright. The snow against walls and on the
      ice is lit the same way, and frosted ice is nearly as white. Until the sky has been seen once, it's lit as
      before. The rain session made its sky reading run whenever snow lies, not only with the haze on.
    - **Wind ripples.** Close up, the snow shows the low ripples wind leaves in lying snow (sastrugi), about 28 cm apart,
      wandering. They stay put rather than turning with the gusts, and fade out by 3 cm a pixel, so far snow stays
      smooth.
    - Built as 5e08e53ca29a from the whole folder (all seven shaders /O3, their bytes checked in it). Installed in the
      game folder and dist with the game closed (hash checked). Not checked in game.
  - **Then: softer ripples, no snow on Faith's hands and shoes, gravel snowed over whole** (the user: "We are almost
    there turn down the grey stripes a bit", "Faiths hands get white when she interacts with the ground they shouldn't
    or should get a little snow", "You can also see faiths triangles when snow is at her shoes fix that as well",
    "Also snow on gravel and some surfaces still looks weird do the best research and fix it").
    - **The ripples** (the grey stripes): ±1.5 % instead of 3.5, gone by 2 cm a pixel instead of 3.
    - **Her hands and shoes.** Her own box starts 12 cm above her feet, so the snow and her prints stay round them. So
      her shoes, and a hand on the ground, got the snow: on her shoes it lay on each up-facing triangle, one by one.
      And dips under half the snow's depth count as buried, so a hand on the ground was ground. Now:
      - **Hands:** for 0.8 s after each touch, fading out by 1.5 s, what's 2.5 cm to 0.6 m above the contact, within
        about 30 cm of it, is kept as the game drew it. The game gives each hand's touch (landing, climbing, scuffing).
      - **Shoes:** what's 3 - 20 cm above where her feet are, within 45 cm of them, is kept likewise.
      - **What stays:** the ground under them is level with the contact or her feet, so its snow and her prints stay.
    - **Gravel.** Snow fills the hollows between stones first and lies level once it's a few centimetres deep
      (Homes & Gardens and Ware Landscaping on gravel in snow; the Sierra Avalanche Center on stones under shallow
      snow). Ours was holed at every stone's edge: a pixel's own depth jump still cut it. And deep snow's light
      followed each stone's light and dark, a speckle. Where the snow is deep enough to bury the stones, that cut is
      left out as long as the surface is in line over 4 pixels, and its light comes from 32 pixels round. So it lies
      whole and even, and shadows over it still show. Still to come: thin snow with the stones' tops poking through.
    - First built as 060c22d0c657 (staged, then stopped: it didn't have the weather session's 16:39 halo fix). Now
      in a262cfe344aa (another session's 16:45 build of the shared folder), with all seven shaders /O3 compiled from
      the file holding both rounds, their bytes checked in it. A rebuild here (23e649f5d43e, 16:47) differs from it
      only in the link time stamp (5 bytes), so it isn't staged. a262cfe344aa is staged: it installs once the game
      closes. Not checked in game.

- **The menu's tabs, rearranged.** The tabs are now CAMERA, FOLLOW, GAME, VISUALS, REPLAY, MISC.
  - New **GAME** tab, next to FOLLOW. It has *Time and HUD* (open by default, as before), *Detail and draw distance*
    (with Districts and Vistas), *Sun shadows* and *Lens flares*, moved there from CAMERA unchanged. Everything
    in them is saved to the config as before (the same keys).
  - CAMERA: the camera tracks are no longer a section to open. The track list, Play and the keyframes are always
    shown under the camera buttons, under a small *Camera tracks* label. *Show the track's path* moved there from
    Camera moves, right under the track, with *Playback options* after it.
  - CAMERA: *Keys* is taken out (the Hotkeys window, MISC > *Change hotkeys...*, lists every key). The *Track and
    bookmarks* part of Camera moves is taken out too. The bookmarks still work with their keys (the digit goes to one,
    Shift and the digit saves it) and stay in the config. Both are commented out in the code (`#if 0`), not deleted.
  - VISUALS and LIGHTS are one tab, VISUALS. The lights are its last section, *Lights*, under Green screen, with
    the same buttons and list as the old tab.
  - VISUALS: the *Fog* and *Wind* effects are taken out of the menu. The weather's own fog settings
    (*Rainy city look*) and the replays' weather are unchanged.
  - Help texts that pointed to CAMERA > Time and HUD or to Camera moves > Show the track's path now point to the
    new places.
  - Build 5d8bf83fe1ba. Built without errors; not checked in game yet.
  - Then: CAMERA has no headers to open at all. *Motion blur*, *Camera settings* and *Camera moves* are gone as
    headers, and everything in them is always shown. Each part has a name over it, with a line across the tab
    above the name. In order:
    - *Camera tracks*
    - *Motion blur*
    - *Speed and field of view*: the speeds, mouse sensitivity, field of view, Reset speeds. This was the top of
      Camera settings.
    - *While the camera is on*: was already a label in Camera settings.
    - *Around the focus*: the focus line, *While a replay plays, Faith is the replayed one*, Orbit, Aim at it,
      Dolly zoom, Auto-focus.
    - *How the camera moves*: Heavy camera, Fly level, Stop at walls, Speed by height.
    - *Camera shake*: Hand-held shake, Shake kind, Shake speed, Impact. This was the second half of "How it moves".
    - *Letterbox and guide lines*: was "Composition".
    - *Wide fields of view*: the Panini projection. Was "Lens".
    - *Diagnostics (written to CT.log)*: *Log sounds for 30 s* and *Log the world for 30 s*, moved to the end from
      Camera settings.
  - No setting changed or moved in the config. Help texts that named Camera moves now name the new labels:
    keyframe shake, the auto-focus line in VISUALS > Depth of field.
  - Build c91b10e7fb74. Built without errors; not checked in game yet.
  - Then: GAME has no headers to open either. In order:
    - *Game speed*: Freeze time, Game speed, Replays at this speed.
    - *Time of day*: the clock, Stop the clock, Light catches up with time jumps, Stuck clock fix.
    - *HUD*: Show HUD.
    - *Detail*: Fix LOD to Billboard quality.
    - *Draw distance*: the slider, then the *Vistas* list, moved up here from the bottom.
    - *Loading the world*: Load the world around the camera and everything under it, *Districts* too.
    - *Sun shadows*
    - *Lens flares*
  - The *Districts*, *Vistas*, *Stuck clock fix* and *Playback options* lists stay small fold-outs.
  - Section names on CAMERA and GAME are easier to see: Segoe UI Bold a size bigger, more room above, and a red
    line under the name across the tab. The top one has no room above. If segoeuib.ttf isn't in the Windows
    Fonts folder, the names are in the normal font, still with the room and the line.
  - Districts: new *All to Always* and *All to Never* buttons next to *All back to Normal*. They set every
    district in the list. Districts from the config that aren't in this level stay as they were.
  - *Nearby reflections first* is no longer a setting: it's always on while a district is Always. The Districts
    help says what it does. The config key `[Misc] NearReflectionsFirst` isn't read or written any more.
  - Help texts that named GAME > Time and HUD now name Game speed, or Time of day > Stop the clock.
  - Build 8632c518e712. Built without errors; not checked in game yet.
  - Then: *Lens flares* is gone from GAME. *Fade far lens flares* is no longer a setting: it's always on while the
    free camera is on, as it was with the box ticked. Faith's own camera still sees the game's flares. The config
    key `[Misc] FadeFarLensFlares` isn't read or written any more. Build 2e5fe1fb9a90. Built without errors;
    not checked in game yet.
  - Then:
    - The tabs are CAMERA, GAME, FOLLOW, VISUALS, REPLAY, MISC: GAME is next to CAMERA.
    - GAME: the *Draw distance* section is part of *Detail*. *Draw distance* and *Unbroken Shard and Bauble* now
      come after Billboard quality.
    - The *Vistas* list is gone. Its zone-by-zone switches are commented out in the code (`#if 0`), not deleted.
      Detail has a single *Show the vistas* toggle instead, saved as before (`[Misc] ShowVistas`).
    - *Hide a vista where its buildings are loaded* has no checkbox any more. It keeps the config's value
      (`[Misc] HideVistasOverLoadedBuildings`, on by default and in the current config).
    - Build 791e43b8d19f, installed (game folder and dist, hash checked). Built without errors; not checked in
      game yet.

- **Camera tracks: any keyframe can be an orbit keyframe, and the track finds the best way round.** Each keyframe's
  row now always has *Orbit keyframe* (before, only keyframes made while circling the focus had it). Ticking it:
  - places the keyframe round the focus (what the camera follows, else Faith) from where it is now, so it doesn't
    move; a keyframe made while circling keeps the place it was made at;
  - finds the way round to the orbit keyframes next to it: the way your track already goes round the focus,
    measured along its path, including more than half a turn or whole turns, instead of always the shorter way;
  - checks that arc for walls round the focus where it is now, with the game's collision (as *Stop at walls*
    does). If something's in the way it tries, in order: the arc rising 10 or 20 degrees in the middle, the other
    way round (also risen), then rising 35 or 50 degrees. The first clear one is taken, else the one that goes
    through least.
  The row says the way round to the next (right and left as the camera sees them, looking at the focus), how far
  round, any whole turns and rise, and what the search found. *Find the best way round* looks again round the
  focus where it is now (one undo step). Tracks whose orbit keyframes were never ticked or searched play exactly as
  before (checked bit for bit offline). Not checked in game yet.
  - **Orbit keyframes follow the focus live**, even one on its own: it stays at its place round the focus
    wherever the focus is, looking at it, on the drawn path (its dot and the path move as Faith moves) and when
    the track plays. The moves to and from it go round the focus too, also to a keyframe that isn't an orbit
    keyframe: the camera ends at that keyframe's own place, turning from looking at the focus to its own view on
    the way (the way round found as between orbit keyframes). Unticking puts the ordinary move back. Before, an
    orbit keyframe whose neighbours weren't orbit keyframes stayed where it was made and the moves next to it
    were straight, so ticking one alone changed nothing. A track that moves
    with what the camera follows gets the focus as it was when the track started, then moves with it. Moves next
    to an orbit keyframe aren't wall-checked while editing (where they go depends on the focus); the playback
    guard still keeps the camera out of walls. Checked offline, not in game yet.
  - **Fixed: ticking Orbit keyframe turned the camera round.** An orbit keyframe always looked straight at the
    focus, so a keyframe looking away from Faith right next to her (the test's were 0.8 - 5.3 m from her chest)
    turned to face her, the opposite way. Now ticking keeps the keyframe's own view as a turn on top of looking
    at the focus: nothing moves or turns when it's ticked, and its view turns with it as it goes round the focus
    (the moves to and from it too). *Look at the focus* in its row turns it to look straight at the focus
    instead; keyframes made while circling still do.
  - **Fixed: re-ticking a keyframe jumped to an old place.** Ticked, unticked, moved and ticked again, it went
    back to the place round the focus from the first tick (or from when it was made while circling). Every
    tick now works it out from where the keyframe is. A tick made just as the track started playing waits for it
    to stop instead of being dropped (the row stayed on "Working out...").
  - **Fixed: the log filling with "Camera collision: the selected track's path ... goes through nothing solid"
    while a path point was dragged.** Each frame of a drag changes the keyframes, so the wall check started again
    and wrote its result every frame (dozens of lines a second). Its result is now written once you let go, also
    after dragging a keyframe's values in the menu.
- **New: Unbroken Shard and Bauble** (GAME > Detail and draw distance, under Vistas; off by default; config
  `[Misc] UnbrokenShardAndBauble`). The Shard and the Bauble mall as they were before the story wrecked them.
  - The game streams each of them in one of a few versions, as sub-levels of the city: the Shard's far-away model
    whole (SkyCity_Backdrop) or ruined (SkyCity_RuinBackdrop); the Bauble whole (BaubleMall), collapsing
    (BaubleDestruction) or in ruins with its smoke (BaubleRuins). Each version has its own camera trigger, and the
    story's progress (progression flags) decides which of them are on. The whole versions still ship with the game.
  - With the tick on the tools switch those triggers through the game's own handler: the whole versions' on, the
    broken ones' off. The game then streams the whole ones in and the broken ones out itself, as it did before the
    story. Off: each trigger goes back to what the game had. The story's progress isn't touched, nothing is saved.
  - Up close the Shard is the real tower either way (it isn't one of these versions).
  - The free camera now also counts for the Bauble's triggers (the area around it is a large part of the city), so
    whichever version is on streams in around the free camera too, as the Shard's top already does.
  - The log says what it switched: "Unbroken Shard and Bauble on: ...", one line per switch ("Streaming triggers:
    ... switched on: the game streams it in ..." / "switched off ..."), and if a version's trigger isn't found, what
    was seen instead. "Hooked CameraTriggerInit" at start.
  - Fixed (build a1b0f91bb294): in the first build the tick did nothing. Its check of the game's code ran after the
    tools had hooked that same code, saw the hook's jump instead of the game's bytes, and gave up ("the camera
    triggers' code isn't what the tools know, nothing changes", 20:49). The check now runs once, before the hook.
  - Seen working in the game's log (21:40 - 22:14, build a1b0f91bb294): the switches went both ways each time the
    tick changed, and at the Shard the far-away model the tools hid was the whole one. Not seen by the tools: how
    it looked.
  - Fixed: with the tick saved on, the log said none of the triggers were found at start (before the level had set
    them up; they were found 13 s later). That note now waits until the level has set up its triggers.
- **Fixed (build f44d1cc7ad41): the real Shard loaded far from it.** With the whole city streamed (every district on
  Always, so Default and the real tower's zone too) and the camera past the city's north-west edge, the real tower
  (about 4.6 km away) loaded and the far-away model went (user, 2026-11-03; CT.log 11:11, the camera at -677, 1704).
  The tools let the real tower in whenever the view was out of the city's area, which was only right on the city's
  south side, toward the Shard. Now it's let in only when the view is out of the city's area *and* in the area around
  the Shard the game streams the tower's top by (the same area: it starts at the city's south edge). Past the city's
  other edges the far-away model stays. Just south of the city there's now a strip about 100 m wide where it's still
  the far-away model, until the camera is in that area. The log: "... out of the city's area, in the area around the
  Shard (x, z): its real tower can load ...", or "... out of the city's area (x, z), but not in the area around the
  Shard: its real tower stays kept out". *The real Shard, from anywhere* still loads it wherever the view is.
  - Also seen in that log: "0 of its 2 far-away fans found to move" each time the tower loaded there. A live read a few
    minutes later found both fans (and on 10-26 up close they were found), and the first read had found neither, so
    the game seems to keep them only some of the time; most likely only while the camera isn't far from them (not
    proven). Up close at the Shard they were found before.
- **New: the real Shard, from anywhere** (GAME > Loading the world > Districts, under the explanation; off by default;
  config `[Misc] StreamRealShard`; build b9140e69406e, installed 2026-11-01 01:4x). From the city the game shows a far-away model of Kruger's tower (after the story,
  its ruin) and only loads the real tower once you're out there. Ticked, the real tower and its top load wherever the
  camera or Faith is, camera on or off, and the far-away model is hidden once the tower is in:
  - the tower: its zone of the buildings ("55") gets a focus of its own, like an Always district (the zones next to it
    come with it), and isn't kept out in the city any more;
  - its top (Kruger's office, its own sub-level): the game's own streaming trigger for it counts as entered wherever
    the view is (only that trigger: the Bauble's and the rest are as before);
  - the far-away model's two spinning fans go onto the mission's fan places whenever the real tower is loaded (as
    they do out there), instead of hanging in the sky next to it;
  - the lightning's Shard tip follows: the antenna once the top is in.
  - It's the same tower you see when you fly out to it: whole, even after the story (only the far-away model shows
    the ruin). The mission's own pieces (its broken fan, its people, its logic) only come with the mission. It's held
    back by the same limits as the rest, and with Default on Never it doesn't load.
  - The menu shows "(tower and top loaded)", "(tower loaded, its top not yet)" or "(loading)"; the log: "The real Shard
    streamed from anywhere ...", "The Shard: its top streams in wherever the view is ...", and the usual "The Shard: the
    real tower (zone 55 of the buildings) is loaded; its far-away model is hidden".
  - Asked by the user through the weather session (2026-11-01). Not seen in game yet: how much of the tower draws from
    3 km away (draw distance and the meshes' own cull distances decide that).
- **New: the Shard's tip for lightning** (automatic; builds 0eb1ab1993bb, far-away model; 4bc73495ca90, the real tower too; the strikes themselves are the weather's).
  Every frame the Shard code gives the weather the top of the Shard the game draws now, so a strike can hit it.
  - From the city, the far-away model: ruined, its highest point at 1133.96 4366.72 -2976.79; whole (before the
    story, or with *Unbroken Shard and Bauble*), the centre of its top at 1111.99 4470.72 -2939.67. These come from
    the models' own vertices (read from the game's mesh data; within 2 - 4 m), placed where the game has the
    model (read from the game's model group each time; the ruin's placement was checked live).
  - Out of the city, close up, the real tower: the tip of the 42 m antenna on the roof of its top (Kruger's office),
    at 900.50 1677.73 -2948.62. The antenna, like most of the top's pieces, is placed by the top's physics data, not
    the level data; it was read from there (the same place in both versions of the top, open or closed blinds) and
    checked against the copy the game had loaded, then the antenna's own vertices put on it. The physics data's own
    box tops out 6 mm from that point. Given only while the top is loaded and its antenna is the physics part
    measured; with the tower loaded but not its top, none. Measured live with the user flying the camera there
    (2026-10-26).
  - None while no far-away model is loaded, or both are.
  - The log: "The Shard's tip for lightning: its far-away model, ruined, at x y z", or "... none (the reason)".
  - Not seen in game yet.
- **New: the Shard's fans up close, where the mission has them** (automatic; build 74092bd81298). The real tower has
  no fans of its own: the Shard mission places them itself (one intact fan low on the tower, one broken fan near the
  top). Up close the tools now move the far-away sub-level's two spinning fans onto those two places, at the
  mission's size, instead of taking the far-away sub-level away. They still spin as the game spins them; back in
  the city they go back to their far-away places in the sky.
  - The fan on the top place is an intact fan: the mission's there is a broken one, a different model the game
    only loads with the mission.
  - Only their look moves: their collision stays where the far-away sub-level has it (in the sky).
  - How: the game spins them with a rotation entity that keeps each fan's base place and applies the spin to it
    every frame; the tools write the mission's place there and the game's back afterwards. If that code isn't what
    the tools know, the far-away sub-level is taken away up close as before (the log warns).
  - The log: "The Shard: its far-away fan at 1066 3889 -2911 moved onto the mission's intact fan, 943 553 -2928 ..."
    and "... at 1229 1357 -2812 moved onto the mission's broken fan (an intact one there) ...", then "The Shard: its
    far-away fans back where the game has them".
  - Not checked in game yet. Verified live (read-only): the two fans' rotation records and their places, as above.
  - **Fixed (build fadd090c7b68): the fan under the top was far too big** (user, 2026-10-26: its blade stuck out past
    the ring around the tower). Both fans had been put at the mission's intact fan's size (0.98 of the fan model),
    but the mission's fan at that place is its broken fan, the same model with a blade broken off, at 0.588. The fan
    there is now 0.588 (blades about 78 m long instead of 130 m); the low one stays 0.98, as the mission's intact fan.
    Both spin about the same axis as the mission's (its timelines only turn them, no offset). The log line now ends
    "..., at 0.588 its size (fan 0x...)". Checked in game by the user (2026-10-26): both fans right.
- **New: three windows to open and close** (GAME > Detail and draw distance, after *Unbroken Shard and Bauble*; all
  off by default). Not checked in game yet.
  - *Kruger's office blinds*: as the game has them, Open, or Closed. The Shard's top is streamed by the game in one of
    two versions, chosen by the story: with its blinds open (SkyCityTopOpenBlinds) or closed
    (SkyCityTopClosedBlinds: opaque windows, and three spots Faith can't climb). The choice switches the game's own
    triggers for the two, as the Unbroken tick does. Config `[Misc] KrugersOfficeBlinds` (0, 1, 2).
  - *Open the lair's blinds (train station)*: the Runners' lair has closed blinds in front of its windows (14 large
    and 6 small, models of the buildings' zone there). Ticked, those are hidden; off, as the game has them. The
    windows behind them have an animation of their own in the data that nothing plays and no shader can show (see
    the removed *Play the lair windows' animation*, below). Config `[Misc] OpenLairBlinds`.
  - *Open the Shard station's gates and windows*: the train station inside the Shard. Ticked, its 8 gates, its two
    side windows and its exterior window are hidden (only how they look: walking through them is the game's
    business). Config `[Misc] OpenShardStation`.
  - The log: "Kruger's office blinds: open / closed / as the game has them" with the "Streaming triggers" lines, and
    "The lair's closed blinds (train station) hidden: N models in M model groups" / "... shown again", likewise for
    the Shard station (they're hidden again whenever their zone loads).
- **Removed again: Play the lair windows' animation** (it was in builds 86d64554822d and 0a0761141813; removed in
  35036ad9e8d3 with its config key `[Misc] PlayLairWindows` and its four hooks: TimelineInit, TimelineDeinit,
  StaticModelLinks, EntityDeinit). It can't show anything (see "Found" below). What it was: Each of the lair's 20 windows (14 large, 6 small) comes with its own animation in the
  game's data: a timeline that changes a value on the window's model over one second (its track is named "Dimmer";
  0 to 1.5 on the small windows, 0 to 2.55 on the large ones). The game never plays it: nothing sends it Play.
  - Ticked, the tools send each window's timeline the game's own Play event, so the game plays it to its end. Off,
    the windows the tools played go back to the start, as the game has them, at once: the game's timeline can't play
    backwards, so it's Stop, Play (which restarts at the start and applies it), then Pause. Windows that load later
    (the lair streaming in) are played too while it's ticked. Config `[Misc] PlayLairWindows`.
  - What the value does to the window (blinds, light, glass) isn't known from the data; not seen in game yet.
  - How: the timelines are noted as the game sets each one up and forgotten as it takes it down (two hooks), and
    told apart by their data (its type, Flags and end time). If the timelines' code isn't what the tools know,
    nothing is hooked and the log warns.
  - The log: "The lair windows: 20 of their timelines set up by the game", then "The lair windows: 20 of 20 played
    (Play)" / "... back to their start (Stop, Play, Pause)", each followed by their states and times ("20 stopped, 0
    playing, ...", and again a moment after playing).
  - Verified live (read-only, on the build before): the game has exactly 20 of these timelines with the lair loaded,
    all stopped at time 0; the tools' test picks out those 20, and each has what the game's event handler needs.
  - Tested in 86d64554822d (23:06): the 20 timelines were found and played to their ends (2 and 4 s) and back, as
    the log says, but the windows didn't change.
  - **Fixed (build 0a0761141813): the windows didn't take the played value.** The value reaches the windows' shader
    parameter entities, but a window's model copies them into what it draws with only once, when it loads; the game
    copies them again only in the per-frame update of models it updates, and a static window isn't one. So while
    the tools play the windows (and 1.5 s after), each window model now gets that same copy every frame, done by the
    game's own function. The models are noted as the game links them to their shader parameter (two more hooks). If
    that code isn't what the tools know, the log warns and only the timelines play.
  - The log adds "The lair windows: 20 of their models linked to the animation's value", and after each change
    "... 20 window models, their value 0.00 to 2.55" (the value they now draw with: 0 at the start, 1.5 / 2.55 at
    the end).
  - Not seen in game yet. The cause was found in the game's code offline; the game had closed, so it wasn't read
    live. What the value does to the windows is still unknown, and it could still show nothing if the shader ignores
    it or the renderer keeps an older copy (the log's values tell those apart from this cause).
  - **Found (23:3x): the animation can't show anything, in the game itself either.** Tested in 0a0761141813: the
    window models now take the value (the log: "20 window models, their value 1.50 to 2.55" when played, 0 when
    back), and the user still saw no change. The window model is two glass panes with the game's glass shader
    (`ss_object_glass`, presets sp_lairwindowsinner_01 / outer_01). The game's compiled shaders name their
    parameters by a hash of the lowercase name (every known glass parameter is found that way, e.g. PatternTiling
    162 times). The parameter the windows' animation drives (handle 3792568681) is in none of the game's 4 shader
    databases. The shaders do have an `EmissiveDimmer`, but its hash isn't that one. So it's leftover data aimed at a
    parameter that doesn't exist.
  - Not by time of day either: nothing in the game sends these windows Play. The train-station prefab, the only file
    that places them (checked in all 43202 base and 502 patched EBX files), has no input events at all, so no level
    logic, time of day or mission can reach them, and nothing inside it sends Play. What the train station does
    switch is by story progress: two open-slat blinds (`lairblinds_256_open`) streamed in or out by mission status.
- **Fixed: the Shard was drawn twice, its far-away model over the real tower.** From the city the game shows a
  far-away model of the Shard (after the story, its ruin) and only loads the real tower once Faith has left the
  city. The real tower is one zone of the city's buildings (zone 55, in the Default district), so around the free
  camera, or with Default on Always, it loaded while Faith was still in the city, and both were drawn.
  - Automatic, nothing to switch on: the tools do the game's own swap for the camera. While the camera is in the
    city's area (the same area the game uses) the real tower is kept out, the way a district set to Never is, and
    the far-away model shows. Out there the real tower can load (around the camera, or with Default on Always),
    and once it has, the far-away model is hidden. With the camera off it goes by the game's own camera, next to
    Faith.
  - The tower's top (Kruger's office) isn't part of that zone: the game streams it in as its own sub-level
    (SkyCityTopOpenBlinds / ClosedBlinds) when the game's camera enters the area around the Shard, so with the free
    camera there and Faith in the city the tower had no top. Out of the city the free camera now counts for that
    trigger too, so the game streams the top in for it as it does for Faith, and out again when neither is there.
    Only that trigger: the game's other camera triggers (lifts, map markers, people) still go by Faith's camera.
  - The zones the game itself wants for Faith or a mission are never kept out, so the Shard mission still has its
    tower. While Faith is out there the game has already taken the far-away model away itself.
  - The camera changes sides only 50 m past the edge of the city's area, so flying along the edge doesn't load and
    unload the tower over and over. The area's 2000 m height is left out: high over the city still counts as the city.
  - The log says each change: "The Shard: the free camera is in the city's area ...", "... is out of the city's
    area ...", "The Shard: the real tower (zone 55 of the buildings) is loaded; its far-away model is hidden ...",
    "... its far-away model is shown again ...", "The Shard: its real tower ... unloaded" / "loading again", and
    for the top "The Shard: camera area trigger 0x... is the one on the area around the Shard ...", "The Shard: the
    free camera is in the area around it: its top streams in" / "... left the area around it ...".
  - Fixed (build a1b0f91bb294): the giant fan stayed up close. The far-away sub-level has two spinning fans of its
    own (skycity_building_fan01 at 2.9 and 1.7 times its size, the bigger one just under the tower's top). The real
    tower's zone has no spinning fan (its fan control rooms and one broken fan); the real fans come only with the
    Shard mission (its logic layer). Only the far-away tower was hidden, and a single model like that fan has no
    hide switch the tools know. Now, while the real tower is loaded and the view is out of the city, the tools
    switch off the game's trigger for the far-away sub-level, and the game streams it out whole (the far-away tower,
    its fans, and the ruin's smoke or the whole tower's searchlights), as it does when Faith leaves the city. Back in the city it streams in again,
    and the real tower is only taken away once the far-away one is back (at most 10 s later), so the Shard doesn't
    go missing meanwhile. Log: "The Shard: its real tower is loaded and the view is out of the city: its far-away
    sub-level (the far-away tower, its fans) streams out ..." / "... streams back in", and the "Streaming triggers"
    lines.
  - Not checked in game yet.

- **Fixed: the game froze when a mission started while a replay was recording.** The freeze was in the game's
  own job system, not in the replay. The game keeps the work any thread can pick up in one queue with room for
  1024 jobs. When a burst of work made it overflow, every thread that had finished a job waited to put the next
  ones in, and none took any out. After that the game never drew another frame.
  - Starting the mission changed the time of day. The tools' catch-up after a time jump (more time for the bounce
    light, a whole reflection cubemap a frame), the whole city loaded and the mission's own loading together
    went past what the queue holds. The freeze of 2026-10-24 with the bounce light catch-up was the same thing.
  - The game already has a way out, which it uses only on PCs with 2 processors: a thread that finds the queue
    full runs a queued job itself, which makes room. The tools now use that on every PC, at the one place where
    the game waits for room in the queue. Nothing else changes: the replay, the catch-up and the whole city work
    as before. The game's own setting for it stays as it is (two other waits read it).
  - The log says how full the queue gets (from half full on), and warns each time it was full and threads made
    room themselves ("The game's job queue was full ...").
  - After every time jump (with *Light catches up with time jumps* on, as by default) the log gives the queue's
    peak in the 10 s after it, or after the last of several in a row: "The game's job queue after the time jump
    from 03:11 to 03:03: at most N of 1024 jobs waiting at once (N %), X s after it ...; found full N times".
    The tools look at the queue about every 5 ms, so a shorter peak can be missed.
  - Tested once (2026-10-25 18:23 - 18:50): no hang with a replay recording and a mission start. The queue didn't
    fill, though, so the new way out wasn't needed. A time jump's catch-up alone took it to 666 of 1024. The peak
    after each jump is logged from the next build on (not checked in game yet).

- **Camera tracks: drag the path's keyframes and bend its curve** (CAMERA > *Show the track's path*, under the
  track's keyframes; menu open, track not playing). Drag a keyframe's dot with the left mouse button to move it across the
  view, or hold Shift to move it up and down. Click a keyframe to show its two curve handles (blue squares, the
  way in from the keyframe before and the way out to the next) and drag them to bend the path round it however
  you like. Dragging one turns the other with it so the path stays smooth through the keyframe; tick *Handles
  move on their own* in the keyframe's row for a sharp turn. The row also has the handles in metres and
  *Automatic curve* to go back. Each drag is one undo step, the right mouse button doesn't turn the camera while
  dragging, and a click that moves nothing changes nothing.
  - A Smooth or Ease in/out move with a handle at either end follows the Bezier curve through the handles (the
    end without one keeps its automatic tangent); the first grab starts from the automatic curve's own handles,
    so nothing moves until you drag. Tracks without handles play exactly as before (checked bit for bit).
  - The drawn path, playback, the wall check (*Stop at walls*) and *Keep the track's path loaded* all use the
    same curve. Keyframes put in later (*Insert view after*, *Duplicate*, *Take the track round them*) shorten
    the handles beside them to their part of the move. Two keyframes at the same place still hold still.
  - Tracks still aren't saved to files, so there's no track file to change; the handles go with the keyframes
    (undo, redo, *Duplicate*, *Set from camera*).
  - Not checked in game yet.
- **Time of day and weather in replays** (REPLAY > *Time of day and weather as recorded*, on by default).
  - Replays didn't record the time of day, so a mission's time (missions set the clock, and some stop it) was
    lost once the mission was over, and a replay played in whatever time it was then. Now the clock is recorded
    with each sample. While a replay plays, the time of day is what it was at that moment, and the light catches
    up as after any time jump. A time jump in the recording (a mission setting the clock) stays a jump. When the
    replay stops, the clock goes back to the time it had before. A camera track that sets the time of day still
    wins while it plays.
  - The game's weather as it showed it is recorded too: the sky's cloud layers and the wind, 4 times a second,
    including a mission's own weather (it comes through the game's visual environments). While a replay plays,
    they're set as recorded, blending between samples; when it stops, the game's own come back at its next
    update. A Clouds or Wind change you make in VISUALS still wins.
  - The weather menu's rain (VISUALS > Weather: rain, heavy rain, kept out of buildings, its zones loaded) is set
    as it was when you recorded, and given back when the replay stops, unless you changed it meanwhile.
  - TIME OF DAY now says when the game has stopped its clock (a mission or the level) or runs it at another pace
    (a mission's setting). The time you set still applies then: missions set the same clock the tools do.
  - Not checked in game yet.
- **Mission and ambient effects in replays.** The missions' and collectibles' own effects (FX/UX: a
  collectible's orb, a checkpoint's, a projection mesh) and the level's ambient and weather effects
  (FX/Ambient: smoke, dust, a mission's set piece; not the rain, which the weather menu handles) start when
  their objects stream in, usually long before you record and far from Faith. Most never move, so the replay
  never saw them start and they weren't in it. Now the ones already playing within 80 m of Faith when you start
  recording are taken in. While the replay plays, each one shows only while the game's own is gone (an orb you
  collected, a checkpoint you passed, smoke from a mission that's over); while the game still shows it in the
  same place, the replay doesn't add a second copy. The log lists every kind taken in. Not checked in game yet.
- **Mission dialog and voice lines in replays.** The game says a voice line by handing the line's sound to a part
  of its speaker's voice, then starting it there. The replay used to record only the voice's start and play it on
  a new voice, which had no line, so it stayed silent. A test recording showed every line goes through that path:
  Faith's lines (VO_Narrative_Faith), the one on the radio (VO_Narrative_RadioOnDistance), people talking nearby,
  and Faith's grunts and breaths (VO_Faith_Emotes), 88 lines in two minutes.
  - Now each line is recorded with the moments the game queued it, started it and finished it. During playback,
    the same line is handed to the replay's own voice for that sound at the same moments, through the game's own
    functions, at the place the game had the speaker. When you seek, loop or start, what's playing stops and the
    lines from there on are said; a line already under way isn't joined halfway.
  - A line whose dialog the game has unloaded since (checked by its sound's stream id) is left out; the log says
    how many lines were said and how many were left out.
  - Not checked in game yet.
- **Collected items in replays (first try).** The electric box's key, the kiosk's chip and the checkpoint beacons
  are ordinary static models, not physics objects. The replay already recorded them moving, but the game hides
  them once taken, so they stayed hidden. Now each nearby model's Visible value is recorded at the start and at
  every change while you record. While a replay plays, a model the game now shows or hides differently from the
  recording gets the recorded value, and the game's own comes back when the replay stops. Not checked in game: no
  code reading that value was found in the game, so it may not be what draws them. The log says how often a value
  was given, and lists each recorded item's values over the recording.
- **Faith's steps in the puddles sound like water** (VISUALS > Weather > *Wet surfaces*, automatic). When she
  walks or lands in one of the puddles the tools draw, the game plays its own steps in water: her shallow-water
  footstep sounds and its water splashes (FX_Foot_Water, and the landings' FX_FootLandSoft_Water and
  FX_FootLandHard_Water). Everywhere else her steps are as before.
  - How: the game picks a step's sound and splash from the material of the surface under the foot. For Faith's
    steps and landings in a drawn puddle, the tools hand it the game's own water instead: material 16 of the
    city's material grid. Before using it, the tools ask the game for that material's sound and use it only if it
    is the water's.
  - A puddle counts the way the wet surfaces draw it: *Wet surfaces* on with some *Wetness* and *Puddles*, not
    under a roof, within *Wet up to* of the camera, on ground that faces up, and inside the puddle's edge (the same
    noise). One test can't be made for a step: the drawing also leaves out puddles where the ground isn't flat a few
    pixels round (grates, steps). So a step on a grate where no puddle is drawn can still sound wet.
  - Only Faith's own steps and landings get the water: not her hands, wall runs, slides or rolls, and nobody
    else's. A slide just after a step in a puddle may still sound wet, if the game's slide sound goes by the last
    step's surface (not checked).
  - The log says when her steps are first seen, whether the level's material 16 is the water, and (every 5 s at
    most) a step put in a puddle.
  - Built as 57d6e638d553 (built without errors). Installed in the game folder and dist as 0eb1ab1993bb (another
    session's build from the folder, 2026-10-26 00:01; this feature's strings checked in it). Not checked in game yet.
  - Fixed: no step ever sounded wet (the user's 00:1x test, in the rain). Her steps were found, but the check that
    the game's material 16 is the water read its sound id as a whole number. The game keeps it as a decimal one
    (60.0), so the check failed and every step was left as it was. It now reads it as the game does.
  - Changed with it: a step counts as in a puddle with the same values the last frame drew them with. The puddles
    now go with the rain (none while it doesn't rain), only where the roof squares know it's open sky, and the wet
    look fades into the game's fog; the steps follow all three.
  - Build 4bc73495ca90 (2026-10-26 00:39, from the folder; the fix's messages checked in it), installed in the game
    folder and dist when the game closed (hash checked). Not checked in game yet.
- **Rain sound at the game's volume, a real overcast look, fog fixed after dying** (VISUALS > Weather).
  - The tools' rain and thunder now follow the game's own volume (Options > Audio). The tools read it from the
    game's profile file whenever it changes. The rain plays at a background level under the game's sounds, so the
    *Rain sound volume* slider is gone. The thunder's own volume slider stays and goes on top of the game's volume.
  - No sun when it rains, now really. With the game's physical sky, the sun's final light is worked out at the end
    of the game's mix. That's why turning the sun's scale down changed nothing in the 20:37 test. The look now
    lowers that final light right after each mix: no direct sunlight and no sun shadows (with *Less sun* at 1).
  - Overcast like TheNollz' Rain mod. The mod's overcast environments were compared against the game's, value by
    value, and the look now does the same where the tools can:
    - the sky's scattering and the fog's colour go fully grey (*Grey sky* now 1 by default);
    - the fog's floor drops to a tenth;
    - the fog is 2000 m deep;
    - the fog is seen through half the game's distance by day, about 1600 m at dawn and dusk, and 250 m at night;
    - forward scattering is pushed out to 300 - 7500 m;
    - ozone is off.
  - On top of that: more bloom, and heavier, flatter-lit clouds.
  - *Fog: seen through up to* is now the most you can see through the fog, 6000 m by default; the hour's fog is
    under it.
  - Fixed: the fog misbehaved after Faith died or a loading screen.
    - While the game has no time of day (respawning, loading), the rain is now held as it is. Before, the forecast
      eased it off, and the look and the fog went with it.
    - A view that's gone (not mixed for 2 s) now gives up its place, so the new views after a respawn get the look.
  - Not done: the mod also hides the moon and the stars and turns the lens reflections off. Where those sit in the
    game's mixed state isn't known yet.
  - Build a003c254e4f2, installed. Built without errors; not checked in game yet.
  - New: **The game's wet look** (VISUALS > Weather, under Wet surfaces; 0.2 by default, 0 turns it off). While it
    rains the tools give the game its own rain setting for its materials, the shader parameter RainShaderParam,
    times how hard it rains. The Release mission and TheNollz' Rain mod both use 0.2. It's added to the game's own
    list of shader parameters right after the game mixes its visual environments, the same way the game adds one;
    if the game already has its own (in the Release mission), that one is left alone. The menu says whether it was
    given. What it changes on the city's materials isn't known yet; that's for the test.
  - Build 476aa6d0a9b3, installed. Built without errors; not checked in game yet.
  - Fixed: the rain was too loud because three rain sounds played at once:
    - the tools' own rain;
    - the game's rain-column sound, held just over the camera (made for standing right under a stream of rain);
    - the game's own copy of that sound, at the rain column the tools moved near the camera to get it.
    Now there's only one, the tools' own: a steady rain all round that follows the game's volume and is muffled
    indoors. It's also half as loud as before. The game's rain-column sound isn't played any more, and no rain
    column is moved for it.
  - Build 60e451e33eff, installed when the game closed (hash checked). Built without errors; not checked in game yet.
  - New: with *Random rain*, thunder and lightning and the fog come and go with the rain. There are two new ticks
    under Random rain, both on by default:
    - *Thunder and lightning with it*: strikes only while it rains, and the harder it rains the more often. At full
      rain the strikes come every *Every* on average; as the rain comes in or eases off, up to 4 times less often;
      none in the dry spells. With Random rain on, *Thunder and lightning* ticked works the same way.
    - *Fog, grey sky, no sun with it*: the rainy city look comes in and goes with the rain, as much as it rains,
      without ticking *Rainy city look* itself.
  - Their settings (the thunder's distances, the look's fog sliders) show whenever they're on either way.
  - Build 217cffe45297, installed. Built without errors; not checked in game yet.
  - Changed: the rain sound is now the game's own, not the tools'. The tools' rain was too loud and didn't sound like
    rain. The one sound now is the game's medium rain (the Release district's rain columns' sound).
    - The game plays it, so the game's volume applies.
    - It's held 30 m over the camera. The sound's own fall-off makes that about 6.5 dB quieter than right on top of
      you, where it used to be (1.5 m).
    - Under a roof it's a bit louder, since the roof muffles it: it sits 3 m over the roof, at least 15 m up.
    - It needs *Load the game's rain*, and it starts a few seconds after the rain.
  - Build 1aee1f573c06, staged; it installs when the game closes. Built without errors; not checked in game yet.
  - Fixed: the wet surfaces swam and flickered as the camera moved, with saw-toothed dry patches and black puddles.
    - The camera they're drawn with is now the one of the frame the game's depth comes from. It's taken when the
      game first clears that depth in the frame. Before, it was the game thread's latest, which could already be
      the next frame's while the picture was still this one's, so everything placed in the world slid about. The
      green screen uses the same camera now.
    - Dry where something is over the spot, worked out per pixel from the 2 m squares the weather sweeps: each
      square's floor and roof as found, blended between neighbouring squares, so the edges are soft and stay put.
      Before, it went by the boxes that keep the rain out, which are rebuilt as the camera moves and are made of
      2 m squares merged (the saw teeth). The ground more than 2 m under the first surface found from above (an
      overhang, a bridge) is dry; just under a railing isn't.
    - A reflection that finds nothing on the frame now adds nothing. Before, it took the top edge of the frame for
      the sky, so with a dark wall there the puddles went black.
    - Puddles only where the ground is flat a few pixels round: none on grates, steps or anything with holes.
  - Fixed: the drawn rain's colour. Its drops took their light from three pixels on the top edge of the frame,
    whatever was there, so they changed colour as the camera turned. They now take it from the frame blurred round
    them and the whole frame's average, slightly lighter than what's behind them and nearly grey.
  - Changed: lightning indoors. The flash no longer brightens the exposure and the sky's light the more the camera
    is inside (both lit the whole room). The sky seen through windows still flashes, what's under the open sky
    flashes, and under a roof only the spots within about 4 m of an opening do.
  - Build 9b425037f7ab. It went into the game as part of another session's newer build, 791e43b8d19f (22:18, built
    from the same folder; this work's strings checked in it), installed in the game folder and dist. Built without
    errors; not checked in game yet.
  - Changed: the rain indoors is about 5 % quieter. Under a roof its sound is 18 m over you instead of 15 m (about
    0.6 dB less on the sound's own fall-off), still over the roof and muffled by it.
  - New: lightning comes in bursts. Each time, 1 to 6 strikes (chance decides how many): the first at once, the
    others after short random gaps (0.1 - 1.2 s, mostly short). Each strike has its own place, flash, bolt and
    thunder. *Strike now* makes a burst too, its first strike at the nearest distance. Up to 12 thunders can
    sound at once (was 6). The bolts are drawn only inside their own box on the frame, so six cost about what one
    did.
  - New: lightning hits the tallest thing around. Before each bolt is drawn, the tops round where it comes down
    are looked for, straight down with the game's collision (20 places, then 25 more 3 m apart round the highest,
    for its very top: an antenna, a spire). The bolt ends there. The game only has the city's collision a few
    hundred metres round the camera; farther strikes come down where they would have. Thin antennas can still be
    missed between the places looked at.
  - Changed: puddles show whatever the *Reflections* setting is. The water smooths the ground's detail away and
    darkens it, and its flat top catches the scene's light, the more the flatter you see it, rippling in the rain.
    The reflections come on top.
  - Fixed: with *Reflections* at 1 the wet ground had a hard edge across it, with bits of ripple rings along it.
    - A reflection ray stopped for good the first time it went behind something thin in front, like a railing or a
      pipe, and gave nothing. So the ground past that thing's reflected outline had no reflection at all.
    - It now carries on behind such things.
    - Where it finds nothing it takes what's round where it left the frame (the sky, a wall), blurred and weaker, so
      a hit and a miss side by side differ by little.
  - Changed: the rain's ripples in puddles show whatever the *Reflections* setting is. Their rings catch the scene's
    light on their own.
  - Builds 2beebfd140b7 and then 86d64554822d (22:55, also has the Shard session's work; the new shader's bytes
    checked in it), installed in the game folder and dist. Built without errors; not checked in game yet.
  - Changed: a lightning burst's strikes are spread out, and each thunder comes after its flash.
    - The strikes in a burst now come 1.5 - 6 s apart (they were 0.1 - 1.2 s, so a flash came with the last one's
      thunder). The next burst waits until this one is over.
    - Each thunder comes its distance / 343 m/s after its flash, as real thunder does (about 3 s a kilometre).
    - A strike is never nearer than *Nearest* (300 m by default, about 0.9 s). The tallest-top search could put
      one 30 m away, its thunder almost at once.
  - Build 35036ad9e8d3, staged; it installs when the game closes. Built without errors; not checked in game yet.
  - Changed: the wet ground's reflections are rebuilt the way the game's own are made. Its reflection shaders were
    taken from its shader database and read.
    - A reflection is marched across the frame in pixels, its depth right all along, and its hit found exactly.
      Before, it was in world steps that grew by 12 % each time, which gave steps and hard edges.
    - Each reflection fades softly: by how far behind the surface the hit is (more leeway further away), near the
      frame's border, and for rays coming back towards the camera.
    - Rough wet ground reflects less and blurred. Puddles are sharp.
    - A ray that runs out over the sky drawn on the frame reflects that sky. Where nothing is found, the ground keeps
      its own colour, faded into, never cut off.
  - Changed: better rain ripples in puddles. Drops of two sizes, each ring a short train of three or four waves
    behind its front that widens and dies away as it spreads. They're shaded by which way their slopes face (lighter
    towards the camera), so they show whatever the *Reflections* setting is. Before, it was one thin bright ring per
    drop, all one size on a 0.5 m grid.
  - Removed: *The game's wet look* (RainShaderParam). No shader in the game reads that parameter: across the game's
    four shader databases neither its name nor its hash is found, while the other environment parameters
    (ToD_Windows, skyVis_addValue, ...) are. So it never did anything.
  - Build 77ca75c183b3, staged; it installs when the game closes. Built without errors; not checked in game yet.
  - New: lightning can hit the Shard. About a third of the bursts send one strike to the Shard's tip when it's within
    35 degrees of where the camera looks; its thunder comes as late as its distance says. The tip comes from the
    Shard's own code (`weather::SetShardTop`, from 0eb1ab1993bb): the far-away model's tip from its mesh's vertices
    and its placement. Ruined: 1133.96 4366.72 -2976.79; whole: 1111.99 4470.72 -2939.67. While the real tower is
    drawn (out of the city) no strike goes there: its top's placement isn't in the level data, so its tip isn't
    verified.
  - Changed: the rainy look's fog is mostly kept out of interiors. The game's height fog doesn't know about
    buildings, so the thick rain fog filled rooms too. While the camera is under a roof, the look's fog (how thick,
    how high, its floor) eases back to a fifth of itself over about a second, and comes back as you go out, as the
    game does with its own interior environments.
  - Changed: the wet surfaces, the rain drawn in the distance and the lightning bolts fade into the game's fog the
    way the game's own world does. The fog's formula was read from the game's sky and fog shader and the code that
    sets it up, and is worked out the same way each frame. Before, the wet look darkened the fog itself over wet
    ground, and the distant rain and the bolts were drawn on top of the fog as if it weren't there. A bolt never
    fades completely, since it lights the fog up.
  - Changed: fewer puddles in light rain. The *Puddles* setting is scaled by how hard it rains now (the forecast's
    intensity and the rain's strength); with no rain there are none.
  - Build 9394de2f76ba, staged; it installs when the game closes. Built without errors; not checked in game yet.
  - Fixed: a burst could add no strikes. The test's log said "6 strikes over 0.0 s" with nothing seen: the list of
    strikes was full of ones not started yet. A new burst now replaces the strikes that haven't started, and the
    log says how many it really made. Build 232df1e2fb79, staged.
  - Changed: no puddles behind or under windows. Puddles now go only where the weather's squares have looked and
    found nothing over the spot. Before, a room seen through a window, in a square not looked at yet, counted as
    open and got puddles. The squares reach about 50 m from the camera, so puddles now stop there too. The
    darkened wet look still reaches *Fade out by*.
  - Changed: lightning always comes down on something. The search for the tallest top round a strike now goes down
    to 300 m under the camera or y -150, whichever is lower. With nothing found, the bolt goes down to y -150 (lower
    if the camera is that low), so it ends at the ground or a building in the frame, never in the air. Build
    8f7f4e531cd1, staged.
  - New: lightning hits cranes. The city's construction cranes (crane01 and crane02, the parts that have collision)
    are found by name among the game's loaded models, every 10 s. At a strike, the three highest crane parts in the
    strike's area are each swept straight down on a grid 2 m apart round their middle. The highest hit inside a
    part's bounds is its top. A crane there gets the strike unless the tallest top found otherwise is 20 m or more
    higher. The far-off backdrop cranes have no collision, so they aren't struck. The log says "lightning comes down
    on a crane's top" and how many crane parts are loaded.
  - New: the wet look leaves Faith, people, vehicles and trains alone. It used to puddle and darken anything that
    faces up: Faith's arms and legs in front of her camera (most when she jumps), people's shoulders, car roofs, the
    trains' tops. The game's own stencil can't mark them by then (its sky pass sets it to 0 for the whole world), so
    the pass gets boxes round them instead. They're found the way FOLLOW finds things to follow, every 0.4 s within
    the wet look's reach (175 m at most), and their places are read again every frame:
    - Faith (0.55 m round her, up to 2.5 m high). While the camera is in her head, what's within 0.6 m of it is
      left alone too, and the wet look fades back in from 0.6 to 1 m (above her feet only): her own arms and legs.
    - People: characters and the crowds.
    - Road traffic (cars, trucks and motorcycles, one swarm; which is which isn't known, so each gets a box a truck
      fits in), drones and flying traffic, vehicle entities (the VTOL; its box is a guess, 14 x 18 m).
    - The monorail trains' carriages and vehicle models, by their own meshes' boxes.
    - The boxes start above the ground they stand on: 0.12 m over a person's feet, 0.4 m over a car's record. So the
      puddle at Faith's feet or round a car stays. A box grows by how far it moved since the last frame, and fades
      over its last 0.2 m, so a wall or a curb next to someone doesn't end in a hard edge.
    - The log says what's kept dry, when it changes (at most every 30 s).
  - Changed: thunder is heard only while the game's window is in front. With another window in front, the tools'
    sound fades out over 0.15 s, and it comes back when the game is in front again. Strikes due meanwhile still
    play, unheard, so the flashes and their thunder stay in step.
  - Build 4bc73495ca90 (00:39, has the Shard session's work too; the new strings and the new shader's bytes
    checked in it), staged; it installs when the game closes. Built without errors; not checked in game yet.
  - Fixed: no rain sound (the 01:25 test). The tools' rain sound is a voice of their own, given the events the game
    starts its rain voice with. The game's event ids are the hashes of their names: 0x0DC0EFA5 is "Start" and
    0x7C89BB7D is "Stop". The game stops its voice when the rain column the tools moved over the camera goes back.
    That test caught only Start, Stop (the 00:15 one caught Start, Stop, Start, Start), so the tools' voice was
    stopped the moment it started, every time it was made. Their voice now gets the game's events without its
    Stops, and a Start if there was none. The log says so ("... events given of the game's N: 1 Stop left out").
    Build 91a38cf80c03 (with the snow session's work), staged; it installs when the game closes. Built without
    errors; not checked in game yet.
  - Changed: no puddles on holed metal, stairs or narrow tops. The user's screenshot showed puddles on a
    perforated metal walkway and its stairs. The holes there are 10 - 15 pixels wide, so the old 4-pixel flatness
    test landed on the metal between them. Each puddle now also checks the surface around it:
    - Holes: 16 points on two rings, 5 and 11 cm round it in its plane (never under 3 and 6 pixels). If any of
      them sees through to more than 4 cm behind the surface (or 0.2 % of the distance), there's no puddle. That's a
      hole, a grate or an edge the water would run off.
    - Steps: the surface 40 cm away both ways, in four directions. A drop on one side with the other side not level
      either (a step up, or another drop) is a stair's tread or a narrow top, so no puddle. A curb, with a drop on
      one side and level ground on the other, keeps its puddles.
  - Changed: Faith's steps follow it. Her steps can't see the frame's depth, so a small extra pass works out the
    same tests for 32 x 32 squares of 25 cm round her feet each frame. It's read back a couple of frames later,
    never waited for, and kept for 10 s, since her feet are often out of the frame. Where that says no puddle can
    lie, her steps keep their own sound. Squares the frame hasn't shown lately are as before.
  - New: nothing is drawn over the game's loading screen: no rain, snow, wet look, flash or bolts. The tools read
    the loading screen's own flag from the game's UI system: [[0x1424157F0] + 0x928] + 0x60, the byte its show
    and hide events set.
  - New: thunder waits while the game is paused. The pause menu was decompiled: its entity
    (PamClientUIPauseEntity) takes "Pause" and "Resume" events, and each one adds 1 to or takes 1 from a count in
    the game's UI system ([[0x1424157F0] + 0x8C8] + 8). While that count is above 0, or the loading screen is up,
    the tools' sound engine is stopped. A thunder already playing waits where it is, and one that comes due is
    held. Both are heard once the game goes on. Read live: the count was 1 while the game sat in its pause menu
    and 0 once it was back.
  - Build 09e15e013529 (with the snow session's flakes and interiors; all five shaders' bytes checked in it),
    staged; it installs when the game closes. Built without errors; not checked in game yet.
  - Checked: the fog after dying (two deaths in the game, its fog read live every 0.2 s). With Random rain off the
    rainy look's fog stayed the same through the death and after it. With Random rain on, the fog follows the
    rain's forecast, as asked for before; in that test its dry spell was thinning the fog, which happened to be
    just as Faith died. No code change.
  - Fixed: a dry box on the road round every car (the user's screenshot: a light frame round each car in the rain).
    The wet look leaves vehicles alone by a box round each one, its floor 0.4 m over the car's swarm record. But road
    traffic's records keep the swarm's own height (only x and z change) while the cars are drawn on the road,
    higher there, so the road inside the box stayed dry and the box's soft edge showed as a frame. The swarms'
    cars and people are now put on the ground found under them with the game's collision, straight down from 5 m
    (cars) or 1.5 m (people) over the record. Their boxes start 12 cm over that ground. One whose ground isn't found
    yet gets no box. Looked for again every second, at most 16 a frame.
  - Fixed: the wet look and the puddles flickered like z-fighting in the Runners' lair, and a puddle ended in a
    straight line across an open floor in The View. Both came from the game's interior volumes, which since the snow
    work count as roofs.
    - They were tested all or nothing, and their faces lie on the rooms' own floors and walls (the lair's floor is
      its volume's floor, y 63.37), so pixels there flipped in and out frame by frame. Now each volume counts fully
      up to 0.3 m past its sides (gone 0.4 m further out) and 1 m under its floor (gone 0.5 m further down), and
      fades over 0.6 m across its top. Smooth wherever a surface lies.
    - The puddle's edge (the user's spot, 190 52 -24) was the face of ve_generic_interiortheview #73, a lighting
      volume 29 x 25 x 39 m over a whole block. 38 boxes and one prism are these generic The View volumes, reaching
      into open alleys across the district. They now count only where the tools' roof squares haven't looked (past
      50 m, or not looked at yet); near the camera the roof sweeps decide. The named interiors (the lair, the
      Kingdom, Droneworks, ...) count everywhere, as before. The same on the CPU (Faith's steps, the thunder).
  - New: *Fog: how thick* (VISUALS > Weather, 1.75 by default, 0.5 - 4). The rainy look's fog, as TheNollz' Rain
    mod has it by the hour, is seen through that many times less: about 1700 m by day, 900 m at dawn and dusk and
    140 m at night (the user: "make the rain fog more intense"). *Fog: seen through up to* still caps it.
  - New: Faith's steps in the puddles. Each step or landing in a puddle sends rings out across it (a short train of
    waves 8.4 cm apart at about 0.75 m/s, gone after 1.6 s), rain or not. Her next 8 steps after a puddle leave wet
    prints, a sole about 28 x 11 cm with a wider front, the first as wet as the wet look and each one after less,
    drying over a minute. They show under a roof too, where the ground is dry, so walking in from the rain leaves a
    trail. Her steps are seen where the water footsteps see them (wetlook::InPuddle); a step's sound and its effect
    count once.
  - Build 9735bd30d184 (with the snow session's latest: snow intensity, the freeze, snow footsteps; all five
    shaders' bytes checked in it), installed in the game folder and dist with the game closed. Built without errors;
    not checked in game yet.
  - Fixed: no wet footprints when walking in from the rain (the user: "wet footsteps on dry floor doesn't work").
    Only a step in a puddle wetted Faith's shoes, and puddles cover a part of the ground, so walking in over the wet
    ground left none. Now any step on the wet look's wet ground wets them too (0.7, a puddle 1). Each step on dry
    ground after leaves a print as wet as they are and dries them by a quarter, until they're drier than 0.12 (8
    prints out of a puddle, 7 off the wet ground). The log says when the prints start ("Faith's wet shoes leave
    prints on the dry ground", at most every 30 s).
  - Build e307b4cca9cc (with the snow session's ice), staged; it installs when the game closes. Built without
    errors; not checked in game yet.
  - Changed: the weather waits while the game is paused or its loading screen is up, as the game's own world does.
    Before, lightning bursts went on behind the pause menu, and their thunder all played at once afterwards.
    - No strike starts. What was due waits as long as the pause lasted, so a burst goes on where it was.
    - The forecast's rain doesn't ease in or out.
    - The snow's clock, wind and settling stand still, and the ice with them.
    - The drawn rain, the puddles' ripples, Faith's step rings and her wet prints stand still too.
    - The log says "the game is paused or loading: the weather waits" and when it goes on.
  - Changed: a moving car's box is put back on the road under it every 2 m it drives, as well as every second
    (on a slope the road under a car changes as it drives).
  - Build c21c6f508b44, staged; it installs when the game closes. Built without errors; not checked in game yet.
  - New: *Wind with the rain* (VISUALS > Weather, on by default, x 0 - 2). The harder it rains, the stronger the wind:
    - Its sound: the snow's wind sound (one wind voice for both), louder and brighter the harder it rains, in slow
      gusts, muffled and quieter under a roof or in one of the game's interiors. The game's own wind sound
      (altitude_wind) can't be turned up: its loudness comes from the mixer Wind_Mixer's one input, "WindAmount" (its
      name's hash checked), and nothing in the game's data sends anything to it.
    - The game's own wind (what blows cloth, foliage and smoke): its strength raised to at least 1 + 7 x how hard it
      rains (8 in full rain, the Snowfall mod's storm), its turbulence up to twice the game's. Given after each mix,
      over the game's own values, which come back when the rain stops.
  - New: rain running down slopes and walls. While it rains, what's sloped or upright under the open sky has rivulets
    about 7 cm apart running down its fall line, faster the steeper (straight down a wall). They're faded out where
    they'd be under 2 pixels apart, so they don't shimmer, and they're not drawn in the puddles.
  - New: rain dripping off roof edges. The game has no gutters, so it drips where a roof ends. Where one of the
    tools' 2 m roof squares with a roof meets one open to the sky, three streams along their shared side fall from
    the roof down to the open square's floor. That's the 48 nearest within 30 m, each 0.6 - 2.2 drops a second times
    the rain's strength. Drops fall as they would, each a short streak that stretches as it speeds up, hidden behind
    what's nearer, and they make small rings where they land in a puddle.
  - Not done yet: water out of drainpipes (the climbable pipes): their placements come from the game while it runs,
    and they need a look in the game first.
  - Build 97871c0a4caf, installed in the game folder and dist with the game closed (all five shaders' bytes and the
    new strings checked in it). Built without errors; not checked in game yet.
- **New: MISC > Performance, what the tools cost the game.**
  - A line that updates every second: the game's frame rate and frame time, the tools' own CPU time a frame, and
    the GPU time a frame of what the tools draw.
  - The CPU time is measured inside the game's hooks that run every frame, on the thread the game runs them on:
    the camera update (the camera, the weather, replays, streaming, the lights), the visual environment mix (the
    rainy look, the rain's strength), the depth copy, the drawing at Present and the menu. Hooks that run only now
    and then (sounds, effects, streaming events) aren't in it.
  - The GPU time comes from D3D11 timestamp queries around the tools' drawing and around the menu, read back a few
    frames later, never waited for.
  - *Run a performance test (30 s)*: three phases of 10 s. First everything as set; then the tools' drawing paused
    (the wet surfaces, the rain drawn far off, the lightning and the Panini projection); then everything as set
    again.
  - It logs each phase to CT.log: frames, fps, average, median, 95 % and 99 % and longest frame times, the tools'
    CPU per part and GPU. Then a summary: what the drawing costs, and how much phases 1 and 3 differ by
    themselves (smaller differences aren't the tools').
  - Keep the camera still for it; the menu can be closed once it's started.
  - Build 701dd7714039. It went into the game as part of the Shard session's 0eb1ab1993bb (00:01), with the Shard's
    tip now given for lightning; the performance test, the fog and the Shard strike are checked in it by their
    strings, and the new shader by its bytes. Installed in the game folder and dist. Not checked in game yet. A temporary diagnostic says every 10 s in how many frames the game's
    camera had already moved on by Present (how often the old way was wrong).
  - The user's two tests (2026-10-26, 1920 x 998):
    - Without the weather: about 152 fps. The tools took 0.16 - 0.49 ms of CPU a frame, most of it the open menu,
      and about nothing on the GPU.
    - Outdoors with Rain, Wet surfaces (Reflections 1), Rain in the distance and Thunder on: about 102 - 117 fps.
      The tools took 0.55 - 1.39 ms of CPU a frame, most of it the camera hook (the roof sweeps and the weather),
      less as the squares filled in. Their drawing took 0.50 - 0.66 ms of GPU a frame.
    - The paused-drawing comparison couldn't say more: phases 1 and 3 differed by 1.22 ms by themselves, against
      0.04 ms between with and without the drawing. What the game's own rain effects cost isn't measured apart from
      the rest of the game's frame.
- **Random rain** (VISUALS > Weather > *Random rain*). A forecast makes it rain at random times of the game's day:
  spells of 40 - 150 game minutes with dry spells between them, so it rains about as much of the day as *How much of
  the day it rains* says. The rain comes in and eases off over 20 seconds, and everything goes with it: its
  strength, the rain in the distance, its sound and, with *Rainy city look* ticked, the grey sky, fog and no sun. The
  same time always has the same weather until *New forecast*, so jumping the time of day jumps the weather too. The
  menu says whether it's raining and until when, or when the next rain is.
- **Fixed: the rain sound stopped under a roof and didn't come back.** The tools turned the game's rain sound toward
  the nearest opening under a roof, and it went silent. It's now always above the camera, never turned: higher the
  more you're inside (6 - 36 m), so the game makes it quieter and muffles it through the roof itself. After a stay
  under a roof it's started again on the way out.
- **The rain has a sound of its own too**, for all the rain round you and the rain drawn in the distance: a steady
  rain all round through Windows' audio (*Rain sound volume*), muffled and quieter the more you're inside, under a
  roof or a shed. It plays from the start, before the game's own rain sound is there.
- **Fixed: the rainy look's fog was hardly there in the morning and at noon, and the sun still shone.**
  - The game keeps a floor on how much its fog lets through: 37 % at 6:00, 60 % at 7:00, 73 % at noon, 0 % in the
    evening. However thick the fog, most of the city showed through it by day. The look now takes that floor away,
    as the game's own foggy places do.
  - The sun's light on the city comes from the sky's sun times a scale the time of day sets (0.6 by day). The look
    changed values that are worked out from those afterwards, so they made no difference. It now turns that scale
    down, so there's no direct sunlight and no shadows, and hides the sun's disk and its glow.
  - The look is now given where the game mixes its visual environments, so the sky and the sun are made from it
    (before, part of it came too late for them).
- **Rain and thunder heard from where they are, muffled indoors.**
  - The rain's sound comes from above the camera in the open. Under a roof it now comes from where the rain is: the
    nearest spot open to the sky (or over the roof when there's none near), facing the camera. The game muffles it
    itself through whatever is in between: the rain sound is set up in the game to lose its high frequencies when
    something blocks it.
  - The thunder comes from the strike's direction (left/right, and front/back with more than two speakers) and
    turns with the camera. Indoors it's muffled: the more of the area round the camera is under a roof, the duller and
    quieter it gets, following the camera in and out.
- **Rain in the distance rebuilt to work like the game's rain.** The drops are now fixed in the world, so they stay
  put and the near ones slide past the far ones as the camera moves (before, the rain was drawn round the camera
  and moved with it, so moving didn't change it). They fall at about 30 m/s, stretched as they fall like the game's
  streaks, scattered at random, lit by the sky, from 8 m out to 60 m. Past 60 m rain is only haze, as in the game.
- **No rain indoors.**
  - The tools' rain is only drawn where the area round the camera is known to be open to the sky, and only above
    the first surface under the camera there: none indoors, none under a canopy, none where nothing is known yet.
  - The game's rain: roofs are looked for out to 50 m (the game's farthest rain goes that far), each box now reaches
    into the highest roof over it (before, under the higher part of a ceiling the rain wasn't taken out), and a
    roof that's now under the camera no longer counts. The log says if there are ever more roofs than boxes.
  - The rain column the tools move for a moment so the game starts the rain sound is now put over a spot open to
    the sky, high enough that its drops end above the ground. It used to go 40 m under the camera, which could be a
    room on a floor below (that rain can't be taken out by the game).
- **Rain in the distance, lightning bolts, no sun on a rainy day** (VISUALS > Weather).
  - The game's rain only falls near the camera: its farthest drops are made in a box a few dozen metres ahead, 10 a
    second, so past about 20 m there was hardly any. *Rain in the distance* draws rain on out to the horizon: streaks
    falling at 14, 28, 55, 110 and 220 m, only where nothing nearer is in the way and not under the roofs found
    around the camera.
  - The game has no lightning in its sky to use (its only lightning effects are weapon arcs and sparks a few metres
    long), so *Lightning bolts* draws one at each strike: from the clouds to the ground with a few branches,
    flickering with the flash, hidden behind what's nearer. Strikes then fall where the camera looks.
  - *Rainy city look* now takes the sun away on a rainy day (*Less sun* at 1, the default): its light down to a
    tenth, its disk and its lens flare gone, the clouds covering the sky.
- **Fixed: the thunder sounded bit-crushed.** It was made of hundreds of tiny pulses with sharp edges, which crackle.
  It's now made of smooth pieces (rolls of low rumble that waver, a crack of soft-edged bright grains and a low
  boom close by), with no limiting or clipping.
- **Fixed: the rain sound didn't come back after Faith died** (or after anything else that unloaded the rain's
  zone): the tools kept waiting for the old sound. They now wait for the new one and wake a rain column for it again.
- **Rain kept out of buildings better.** Roofs are looked for in 2 m squares instead of 4 m, out to 33 m, with up to
  64 boxes instead of 24 (the 24 were all in use most of the time), each reaching 1 m past its roof's edge and at
  least 1.5 m over the camera.
- **Fixed: the rain still wasn't seen, and the thunder sounded broken.**
  - The rain was at the camera but made no drops. Every one of the game's rain emitters multiplies how many drops it
    makes by one effect setting (EmitterParameter3). The game's visual environments set it, and in the city the
    always-on one sets it to 0: the rain only falls in the Release mission, which sets 0.7. While *Rain* is on, the
    tools now give it the new *Rain strength* (0.7 by default), right where the game's emitters take it from. Off,
    the game's own value is back. Other rain effects the game placed (splashes, drips, rain walls) that read the same
    setting come back too where they're loaded.
  - *Keep rain out of buildings* now looks for roofs up to 60 m above the camera, not 150 m, so a bridge or a sign
    high up no longer takes the rain away. The log says when the camera itself is under a roof.
  - The thunder was made loud by rounding off its peaks, which crackled. It's now made the way thunder forms: many
    shock waves from along the lightning arriving one after another in a few rolls, a crack when the strike is
    close, duller and longer the farther it is, with a few echoes off the city. It's brought to a steady loudness
    without clipping anything.
- **Rainy city look** (VISUALS > Weather). The game has no grey, rainy daytime look: its only rain look is the
  Release mission's, at night. This makes one on top of whatever time of day it is: fog all over the city (the
  game's height fog made deeper and thicker), a grey sky, less sun and duller colours. *Grey sky*, *Less sun*,
  *Fog: seen through up to* and *Fog: how high* set how much. It's worked out from the game's values every frame,
  so the time of day still moves the sun, and turning it off gives the game's look back.
- **Fixed: no rain in the city, "its zone is being loaded" for ever, wet surfaces that looked like a z-buffer fault,
  no thunder or lightning.** The game starts its placed rain another way than the effects the tools watched, so it
  was never seen (its zone did load): the rain is now found among the game's placed effects and moved to the camera
  the game's own way. The wet surfaces worked their positions out too roughly for the game's depth; they're now worked
  out relative to the camera, with the facing taken from the nearer neighbours. The thunder was too quiet and too deep
  to hear; it's louder and fuller now. The lightning is now also drawn on the frame, so it shows.
- **Weather** (VISUALS > Weather, Clouds, Wind). The game has no weather of its own: its rain is placed in the
  Release district only. Now:
  - *Rain* keeps the game's own rain around the camera (the Release district's camera rain) wherever the camera
    is, free camera or Faith's; *Heavy rain* adds the game's heavier one. The tools keep the city zones that rain
    lives in loaded (*Load the game's rain*), so it's there anywhere in the city.
  - *Keep rain out of buildings*: around the camera the tools look up for roofs and take the rain out under them,
    with the game's own exclusion volumes (how the game keeps it out of the Release district's buildings). Through
    a window it still rains outside.
  - *Clouds*: the sky's two cloud layers (opacity, speed, wind direction, altitude, thickness, colors, light).
  - *Wind*: the game's wind (direction, strength, variation, turbulence), which the game's effects and plants use.
  - *Rain sound*: the game's rain sound (the Release district's rain columns') plays at the camera while it rains;
    under a roof it comes from the nearest place open to the sky.
  - *Thunder and lightning*: now and then (or *Strike now*) a lightning flash (the sky, the sky's light and the
    exposure brighten in a few pulses) and its thunder after it, later the farther away it struck. The game's
    thunder is only in the Release mission, so the thunder is made by the tools and played through Windows' audio
    (its own volume slider).
  - *Wet surfaces*: the ground, roofs and ledges look wet: darker, reflecting what's around them, with puddles that
    mirror and ripple in the rain, and dry under the roofs the rain is kept out of. Drawn by the tools over the
    finished frame from the game's depth (the game has no wet look of its own).
- **Time of day jumps: the light settles much sooner** (CAMERA > Time and HUD, *Light catches up with
  time jumps*, on by default). After a jump (the slider, the buttons, a track's cut), the shade kept the
  old time's light for several seconds, and reflections often kept it until the sun moved on. The game
  lights the time of day in steps: every few seconds it works the bounce light out for a few minutes
  ahead and blends over to it, so after a jump it took two or three steps to get there, and the extra
  work the tools asked for went into a step that wasn't shown yet. Now the tools give it the game's own
  reset at the new time, the one its own time skips use: the bounce light is worked out again from the
  start there, five times over, and shown as it goes. Once that's done, every reflection is captured
  again with the new light. While the time keeps jumping (a slider dragged, a fast time-lapse), the reset
  waits until it stops. The game's lighting reads its own copy of the clock, which gets the new time a
  moment after the tools set it; the reset now waits for that copy (at most 10 s). Before, a single jump
  could find the lighting "already at" the old time and get no reset, so after day → night the city stayed
  lit.
- **Game speed** (CAMERA > Time and HUD): a slider from 0.05x to 2x, with a *1x* button. The game's
  time (its 30 Hz simulation and its frame time) goes slower or faster; the free camera, camera tracks and
  replays keep real speed, and sounds play at their own speed. The game's own slow motion (some missions, a hard hit) still plays, times
  the slider, instead of resetting it. Faster than 1x needs the frame rate: 2x needs 60 fps (the game
  simulates at most one 30 Hz step a frame); the menu says the most the current frame rate allows. *Freeze
  time* works as before, and unfreezing comes back to the slider's speed. Every start is at 1x.
  **Game speed keyframes**: every keyframe keeps the game speed set when it's added (shown and changed in
  the keyframe), and with *Keyframe options > Game speed* ticked the track sets the game's speed between
  them (smooth, linear, eased or held like the shake), so slow motion can ease in and out during a shot.
  The slider's speed comes back when the track stops. The track itself keeps real time: the camera keeps
  its pace in the slowed world, and keyframe times stay what they say. Undo / redo, *Duplicate*, *Insert
  view after* and *Set from camera* carry it. (Tracks aren't saved to files, as before.)
  **Replays at this speed** (on by default): a replay playing goes at the game speed too, so the
  recorded Faith and people slow down with the world, and *Freeze time* holds it; recording keeps real
  time. With a track playing along, the track's slider sends the replay to the same moment of the slowed
  action.
  The video screens slow down and speed up with it too: the ones whose sound is playing used to
  follow their sound (which plays at real speed) and kept full speed. While the speed isn't 1x they
  go by the game's time and their sound plays on; back at 1x they jump to their sound again.
- **Camera moves** (CAMERA, new section, all off by default). The *focus* is what the camera
  follows (FOLLOW tab), or else Faith at chest height.
  - *Orbit*: the camera circles the focus. The mouse and arrow keys go round and over it, the wheel
    and forward / back change the distance, left / right go round, up / down go over and under.
    While following with *Turn with it* on, the circle turns with what's followed. A keyframe
    added while orbiting is an *orbit keyframe*: between two of them, a track goes round the focus
    wherever it is when the track plays, looking at it, instead of along the straight line between
    the two places. The yaw takes the shorter way round, so add a keyframe for every half turn or
    less.
  - *Aim at it*: fly freely, or play a track, while the camera keeps looking at the focus.
  - *Dolly zoom*: the "vertigo" effect. The field of view changes with the distance so the focus
    keeps its size on screen.
  - *Auto-focus*: the depth of field moves with the focus' distance, keeping the set-up made in
    VISUALS > Depth of field (that override has to be on).
  - *Heavy camera*: moving and turning start and stop smoothly.
  - *Fly level*: forward, back and sideways keep the height.
  - *Speed by height*: faster high above Faith, slower near her.
  - *Hand-held shake*: a gentle sway, only in the picture (not in keyframes or bookmarks), on the
    track's clock while one plays.
  - *Panini projection*: for wide fields of view, the sides are squeezed instead of stretched. It's
    drawn over the game's finished picture, HUD included.
  - *Show the track's path*: the selected track's path and numbered keyframes, drawn while the menu
    is open.
  - *Bookmarks*: 9 camera spots, kept in the config. 1 - 9 go to one, Shift + 1 - 9 save it (both
    in the hotkeys).
  - Orbit, aim and dolly zoom are off again each time the game starts.
  - *While a replay plays, Faith is the replayed one* (on by default): following Faith, orbit, aim,
    auto-focus, dolly zoom and speed by height go by the replayed Faith (where she was, and her
    heading from the recording) instead of the real one standing still.
  - *Shake kind*: Hand-held, Walking, Running, Vehicle or Earthquake, plus an *Impact* button for a
    one-off jolt.
  - *Composition*: a letterbox (1.85, 2, 2.20, 2.39, 2.76, 4:3, 1:1 or 9:16, with its bar opacity)
    and guides (thirds, golden ratio, centre, action and title safe). They show with the menu
    closed too; the guides hide while a track plays unless asked.
- **Camera tracks**:
  - Keyframes can be undone and redone (adding, deleting, moving, editing; 100 steps; Backspace /
    Shift + Backspace).
  - Each keyframe can be duplicated, or have the current view inserted after it. *Set from camera*
    keeps orbit too.
  - Keyframes keep the shake strength, and the track can set it (Keyframe options > Hand-held
    shake).
  - A *Shot sequence* plays tracks one after the other, cutting from each to the next, with a
    replay playing along through the cuts. *Add a shot* adds the next track after the last shot's,
    and each shot picks its track from a drop-down list.
- **Filmic effects** in VISUALS: the game's lens distortion, chromatic aberration, vignetting, frame
  flash, depth flash, distance blur, edge blur and heat ripple, which the original tools didn't
  have.
- **Sounds are heard from the free camera** (*Hear from the camera* in CAMERA, on by default): the
  game put its sound listener at its own camera, by Faith, so sounds near the free camera were quiet
  and hers loud wherever the camera was. While the free camera is on, the listener now gets the free
  camera's place and direction, and a velocity from how the free camera moves (the game's own would
  be from Faith to the camera, which pitches every sound up or down). Turned off, it goes back to
  the game's camera without a jump in pitch. What the game starts only when Faith is near stays
  that way: the street traffic, the crowds' spots, the night club's music and the announcements are
  started by the level's own logic (its events) as she comes close, and KrugerSec only talk near
  her. Once started they're heard from the camera too.
- **The replay draws Faith's MagRope line**: where a recording has her using the MagRope, the line
  goes from the anchor to her right hand again, as the game drew it (its muzzle and impact effects
  and its sounds were in the replay already).
- **The replay's people take less than half the memory.** Each bone of a recorded KrugerSec, gun or
  drone is stored in 14 bytes instead of 32 (rotations within 0.004 degrees, positions within 0.5 mm a
  metre; the bones that place them in the world exactly as before), in blocks, so nothing extra is kept
  or copied as a recording grows. Their room is 768 MB (it was 512 MB, used up 222 s into a fight
  with 48 KrugerSec, after which they all froze), which should hold a whole 10 minute recording of
  such a fight. They're also held when Windows has less than 2 GB left for the game, or no more room
  can be had, instead of the game running out of memory.
- **REPLAY > KrugerSec and other people > *Keep them in the level until it plays*** (on by default, as
  before). Untick it to let the game take the recorded KrugerSec out once a recording is done; kept
  ones stay in the world and are met and heard while you play (18 were kept after one fight). The ones
  the game takes out are missing from the replay.
- **While a replay plays, the live game's sounds for the real Faith are left out**: all of her own, the HUD's
  too (the MagRope's alerts, her damage and breath, heard the same everywhere), the guns' and the KrugerSec's
  within 80 m of where she stands, the bullets' impacts and other hits and scrapes there, the bullets flying by
  her and the security cameras' city alert (the fight going on round her was heard over the replay). The
  replay's own recorded sounds still play.
- **Fixed: the MagRope's tooltip sounds playing in the replay.** The alerts and beeps the MagRope makes
  as a hook point comes in reach are the HUD's, so they're no longer recorded with Faith's sounds (nor
  played from a recording made before). The rope's own sounds (firing, attaching, the line) still are.
- **The replay shows mission items as they were**: the game shows and hides them (a delivery's package, handed
  over and put away) and switches their looks (the delivery cabinet) rather than moving them, so replays showed
  them as the game had them at playback. The game's showing, hiding and switching of the models in prefabs is now
  recorded and played again (REPLAY > Doors, elevators, moving objects and mission items), and given back when
  playback stops. Items the game destroyed since can't be shown.
- **The replay plays voice lines**: Faith's, the people she talks with, the missions' and the radio's
  (ME_Sound/VO/...), which the game plays through its voice-over system, not on Faith's sound entities, so
  they weren't recorded. The city's loudspeakers aren't. While a replay plays, the live game's lines are left
  out like Faith's other sounds.
- **Fixed: a crash with the whole city loaded** (exception 0xC0000005 writing far past a buffer, at
  `+0x3F38375`). The game's physics world has room for 16384 bodies, and when it's full the game doesn't make
  more room: the next body was written past the end. Each physics world now has room for 65536 bodies and
  as many moving ones. The log says how many the client world uses as it fills.
- ***Move camera to the replayed Faith*** (CAMERA, under *Move camera to Faith*, and at the top of REPLAY): while a
  replay plays, puts the free camera at the replayed Faith's head, turning it on if it's off.
- **The replay plays the runs' and missions' effects**: Runner Vision's red trail along the route, a
  checkpoint's appearing, passing and going, the collectibles' orbs, the projection meshes and the hack
  effect, recorded within 80 m of Faith with their moves, like the guns' effects.
- **CAMERA > *Stop at walls*** (off by default): flying the free camera into a wall, a floor or anything
  else solid, it stops at it and slides along it, kept *Distance from walls* away (0.25 m by default). It
  sweeps each move through the game's own physics, with the sweep the game's raycasts use. Orbiting and
  following aren't stopped.
  - *Camera tracks too* (on): a playing track slides along what its path grazes, and where its path goes right
    through something, the camera waits at it and cuts to where the path comes out.
  - The selected track's path is checked for walls while it's edited: the places it goes through something solid
    are listed and drawn red on its path (*Show the track's path*), and *Take the track round them* puts keyframes
    in there, over or beside what's in the way (undo takes them out).
- **Fixed: some hooks not being made once there were more than 63 of them**, so Faith's MagRope line was
  missing from the replay and the fix for black buildings with many districts loaded (Enlighten's light
  map ids) was off ("Could not create MagropeUpdate / LightMapId hook. MH_STATUS 0x9" in the log). The
  tools now keep more room next to the game for them, and a hook that still can't be made is shown in red
  at the top of the menu, by name, instead of only in the log.
- **Green screen** (VISUALS, new section, off by default): a flat rectangle of one color (green by default)
  standing in the world, to key out when editing, in place of the fog Greenscreen (that one could only fade
  the distance into the fog: this game draws a physical sky, with which the fog's distance fog isn't drawn).
  *Put it where the camera looks* stands it up straight, facing the camera, at *Distance*; *Width*,
  *Height*, *Turn* and *Up / down* size and move it from there. Whatever stands in front of it (Faith, the
  KrugerSec, a railing) stays in front, everything behind it is hidden. It's drawn over the game's finished
  picture in exactly its color, so the exposure, tonemap and film effects don't change it (and the HUD, if
  it's shown, is covered where it is). *Keep buildings and objects*: nothing in the world is hidden by it, it
  covers only the sky inside its outline. *Green sky*: the whole sky in its color, everywhere on screen, with
  the rectangle shown or not (the sky is where the game drew nothing: its depth past 20 km). In the
  Fog effect, the *Fog gradient* box is now called *Fog gradient (uses the curve)*: the fog's distance curve
  is only used with it on.
- **Fixed: old Shock Protector blasts going off again when the replay's Position slider is dragged.** A
  seek starts again the recorded effects still going at that moment, and the game lets a shot's flash or a
  Crippler discharge run on long after it's seen, so the same old discharge went off at every step of the
  drag. Now only the lasting effects (a VTOL's engine and downwash, a hunter drone's lasers, searchlight
  and downwash) are started again on a seek, and the others only if they began just before it.
- **Fixed: KrugerSec drones invisible in the replay** (their lasers, searchlight and downwash played with no
  drone). A hunter drone keeps its distance in a fight: its effects were recorded within 80 m of Faith but
  its body only once it came within 40 m. Drones are now looked for within 100 m (guns and people within
  40 m as before).
- **Fixed: doors and other recorded models not moving in the replay** when the part of the level they're in
  had streamed out and back in since the recording (the game builds them again: 52 of 77 were new ones). While
  it plays, the missing ones near the replayed Faith are found again by their mesh at their first place.
- **The replay plays Faith's sounds** (REPLAY, *Faith's sounds*, on by default): her footsteps,
  breathing, hands, bumps against walls and ceilings, and the MagRope are recorded with her movement and
  played where the replay is, at the same moments. They're the game's own: each of her sounds keeps one
  voice that the game gives events, so the replay gives those voices the recorded events and holds them
  where they were. While it plays, what the real Faith (standing still) would make with them is left out.
- **FOLLOW's Nearby list, sorted by what things really are.** The groups are now People, Cars,
  Drones, Boats, Trains, Aircraft, Security cameras and Other, named from the game's own models: a
  swarm's appearance items (`Crowds/Cars/...`, `Crowds/Drones/...`, `Crowds/Boats/...`, the people
  folders) and a static model's mesh (`monorail_train_01`, `bd_airplane_01`, `blimpcallaghan01`,
  `securitycamera_body`, ...). The list shows that name, and the people standing still on balconies
  are now people instead of "traffic or crowd". Cars, trucks and motorcycles are one swarm, and which
  member is which model isn't known, so they're all "road traffic". The rows are kept in order of
  distance from the camera as things move (not while the mouse is over the list, so they hold
  still to be clicked).
- **Fixed: Other's rows couldn't be found or used.** Unticked groups still took up the list's 300
  places, so in a crowded street what was ticked often didn't make it in. The filters now pick
  what's listed. Most of Other near a building was its window shutters (they move a few
  centimetres), and following one put the camera inside the wall. They aren't listed any more.
- **Trains and aircraft can be followed**: the monorail trains (up to about 135 m/s), the
  airplane (1.5 km up, 46 m/s), the blimps and the parked VTOLs are listed wherever they are, also
  beyond the range, and can be picked by aim up to 5 km. When a train's route starts again
  somewhere else, the camera goes next to it there instead of letting go. The camera is put far
  enough back to see all of it (a blimp is 109 m long).
- **Security cameras can be followed**: they're listed (and picked by aim) though they never move,
  and following one puts the camera at its lens, looking where it looks.

- **Fixed: the cursor could go invisible** after the menu was open, most of all after a long alt-tab.
  The game sets the cursor's display count in steps of its own (to about 2 to show it, -2 to hide it),
  and the tools counted their own steps against it, also while another window was in front: closing the
  menu could leave the count far below the game's. While the menu is open the game's calls now only change
  a count of the game's own, and the menu closing sets the real count back to exactly the game's.
- **Fixed: black buildings and far too dark shade with many districts loaded.** Enlighten (the bounce
  light) gives every piece with a light map an id, and counts them in 16 bits. The whole city has about
  90000 such pieces, so past 65536 the ids wrapped round to ones in use, and pieces drew other pieces'
  light maps. The ids now stop just short of the limit; the pieces past it (the last to load, usually far
  from the camera) go without bounce light instead of taking another piece's.
- **Nearby reflections first** (LEVEL STREAMING > Districts, on by default, only while a district is
  Always): the game keeps only 32 reflection cubemaps and gives them to the volumes that cover the most
  of the view. With every district loaded, about 1250 volumes want one instead of about 50, and ones
  hundreds of metres away took them from the ones around the camera. Now the farther a volume is from
  the camera, the lower it ranks.
- **Fixed: a crash when a district went to Never while one of its zones was still loading** (say
  Always and right back to Never). The tools unloaded it before the game's own check that drops a
  zone's loading mark when it's asked to unload, so it was left loading without a slot and the
  game's next check read slot -1. A zone that's still loading now unloads once it has loaded, and
  the game's own rule is applied before the tools' step.
- **The tools let go of their zones while Faith isn't spawned** (dead, or a level loading) and ask
  for them again 5 s after she's back: dying with the whole city loaded hung the game on its loading
  screen. The Never districts are let back meanwhile too.
- **Fixed: a crash with the whole city loading** (every district Always): the renderer keeps its
  instances in a list with room for 200000 and takes a new one without a check, so past it the game
  wrote over memory far away and crashed. The list now has room for 1000000 (set when the game
  starts), and the tools hold the camera's zones back by how full it is too (shown under the draw
  batch). The instances' records ran out at 262144 the same way; the tools move the pool's list of
  blocks so it has room for 1048576, and hold back before it too. (*Unrestricted* still ignores
  these limits, and the game can then crash.) The renderer's instance memory is now 128 MB instead of 64, and the tools ask for eight
  times the game's room for model instances instead of four.
- **Room for 512 zones per streamer instead of 128** (the props alone have 238 zones and 15
  regions). The game keeps loaded zones in 128 slots and writes past them when they run out; when
  the game starts, the tools give every streamer its own 512. The game also handles at most 128
  zones loading or unloading in one step without a check (on its stack), so the tools hand it at
  most 120 at a time. Only on the game version the tools know, all or nothing, with a log line.
- **Room for 16384 meshes instead of 4096** (the limit that held the camera's zones back most).
  The game keeps its meshes in a table with room for 4096 and writes past its end without a check.
  When the game starts, the tools move the table, the list of new meshes and the streaming's
  "still wanted" marks to room for 16384, and raise the three places that capped a mesh's number at
  4095. Only on the game version the tools know, and only if every place is the code they read; the
  log says if it's done or why not. The mesh hold-back levels keep the same margins below the room.
- **Fixed: the renderer losing track of what it draws past 65536 instances** (a bug in the game,
  reached when the camera loads a lot of the city). The renderer keeps what it draws in chunks of 64,
  and finds an instance again by a number that keeps its chunk in 10 bits, so it couldn't find
  anything in chunk 1024 or later: it was never taken out, and stayed after its zone's memory was
  freed (seen: 36638 of them). It may be behind some of the crashes with the world loaded around
  the camera (not proven). The tools now widen it to 11
  bits when the game starts (one byte in each of the two places), so all 2048 chunks of a batch work.
  If they can't, the log says so and the tools hold the camera's zones back before 1024 chunks.
- **Fixed: no menu (F5) and no mouse look with Catalyst Clarity, now and then.** The tools draw the
  menu, and start reading the mouse, from the game's Present. Clarity's Present hook (in its
  `dinput8.dll`) sometimes went in over the tools' one and passed on to what was there before it, so
  the tools never saw a frame. They now also hook Present in the game's swapchain's table of
  functions, which the game calls before any of those hooks, and still take each frame once.
- **Fixed: a crash with mods that load more of the city** (seen with the whole-city mod, the free
  camera on and a 5000 m shadow distance). The renderer gathers what each view draws into a list
  with room for 32768 entries and never checks for its end, and the mod ran past it. The list now
  has room for 1.4 million (on the build the tools know only; the log says if it's done).
- **Guard against crashes with the camera's zones streaming in and out** (`+0x3763764` twice and
  `+0x3296FE4`, all with *Load the world around the camera* on): a delayed job of the city's model
  groups searches the world for meshes, props and terrain around a point and still found entities
  whose zone had unloaded, half torn down or already freed. Each one it finds is now checked first
  (its memory, its vtable, its type and what the game reads next) and skipped if it's gone, the
  way the game skips one it can't use. The log says when that happens.
- *Smooth animations far away* (CAMERA, *Detail and draw distance*, on by default): the game
  animates characters 30 times a second and blends that up to the frame rate, but fully only
  within 10 m of the camera. With this on, characters farther away move as smoothly as near ones
  (up to 100 at once, the game's own limit).
- *Billboard quality* (same place): every video billboard has five versions of its video (most
  are 1440 x 720, then 720 x 368, 368 x 176, 176 x 96 and 96 x 48), and the game never uses the
  full-size one, and the bigger ones only when a billboard fills a lot of the screen. The dropdown
  has *Default quality (game quality)*, *Sharper* (the bigger versions 4x sooner) and *Much
  sharper* (and the full size allowed, the default). The game still drops to smaller versions when
  its videos fall behind.
  With the free camera on, billboards are sized from it (where it is, what it sees and how far
  it's zoomed in); the game sized them from its own camera, by Faith, so billboards near the free
  camera stayed blurry.
- **Fog** in VISUALS: the visual environment's fog settings (its distance fog, height fog and light
  scattering).
- The log no longer repeats "Asked the game for room for ... model instances" every time the game
  puts its own number back.
- **Fixed: Faith's hair switching off more than 33 m from her.** The hair fix raised the activation
  radius in the hair's cloth data, but the hair keeps its own copy, taken when it's made (at a level
  load, before the camera was on), so it kept the game's 33 m. That copy is now raised too while
  her body is shown, and put back afterwards. With a replay far from her the hair now stays.
- **Load ahead of the camera works in the busy city.** While the camera's zones were held back (in
  central city nearly always, the renderer's draw batch is that full) only the camera's own point
  could go in, so the point ahead of it never did. It now goes in like the camera's own while the
  rest are held back. When zones are taken away it goes after all the others, and then stays out
  until there's room again, so it doesn't load and unload over and over. (Kept in to the top level
  like the camera's own, it took the draw batch to 95 % in a test flight.)
- **REPLAY tab** (experimental): records Faith's movement, every bone of her body 30 times a second
  (up to 10 minutes), and plays it back on her own body the way the game plays a Dash ghost, so a
  run can be filmed again from other angles. *Play along with the camera track* starts, pauses and
  stops it with the track (and follows a fixed playback FPS). While it plays Faith herself stays
  where she is. The recording isn't saved when the game closes.
- **Fixed: the replayed body disappearing away from where Faith stands.** The game decides whether
  to draw her from a box made of a few bones (feet, legs, arms, head, camera) of a second copy of
  her skeleton, which the replay didn't move, so close to the replayed body only her hair showed.
  That copy now follows the replay too (with the free camera on and her body shown), with her own
  root and trajectory left as they are, so her body shows wherever the replay goes. Her own pose's
  root also goes with the replay, so her hair simulates next to her head.
- **What the free camera looks at loads first.** The game loads the zones waiting to load in order
  of distance from its own camera, next to Faith, so with the free camera far away whatever was in
  front of it (facing away from Faith) came last. Now what's right around Faith still goes first,
  then what the free camera looks at or is near, nearest first, then the rest.
- The camera's streaming points go in the same way: in view first, and when a limit holds them
  back, one behind the camera makes way for one in view (one a second).
- **Fixed: a camera above about 400 m loaded none of the buildings around it.** The game tests
  zones with their heights, and most buildings' zones end at 400 m. A camera point that's in no
  zone's height band now moves into the band of the zone under or over it.
- **Fixed: the camera's zones all going at once near a limit.** Two were taken away on every
  update of the game's streamer, which emptied everything within a second before the counts could
  come down, and then loaded it all again. It's now two every half second (still on every update
  right at the end of the zone slots, or at the very limit).
- *Hold back from* (under *Renderer draw batch*, 70 % by default, up to 78 %): how full the
  renderer's draw batch may get before the camera's extra zones are held back. In busy parts of
  the city what's loaded around Faith alone takes about 75 %, so at 70 % nothing extra loaded
  there. Taking zones away starts 10 % higher and all of them go 18 % higher (never past 86 % and
  92 %); the crash is at 100 %.
- **Fixed: the draw batch running past its room.** Zones that are still loading go on adding to
  it after the camera's zones are held back, and at a high hold-back level that ran it past its
  room. From 20 % below the hold-back level on (half full by default), the next of the camera's
  points now waits until the batch has stopped rising, and the levels end lower (see above).
- **Fixed: streaming stopping for good after that.** The batch never gives its chunks back, so
  once past its room the tools counted it as full forever. They now count what's in it, and the
  menu and log say the game can crash from then on and should be restarted.
- **Fixed: the camera's own zones going in and out every second at the limit.** Once taken away
  at the very limit, the camera's own point only comes back when there's room again. A read of
  the draw batch that fails halfway no longer counts as empty.
- *Districts* (under *Load the world around the camera*): the districts the game streams the city by
  (Rezoning, Anchor, Downtown North, The View, Downtown South, Prison, ...), each *Normal*, *Always*
  or *Never*, with the camera on or off. Saved in the config. *Always* keeps it loaded, within the
  same limits as the rest (it goes before the camera's own extra zones). *Never* unloads it and
  keeps it from loading, whatever wants it, except the zone Faith is in and the zones next to it
  (the ground under her): the game unloads its zones as it unloads any zone, then won't load them
  while they're set to Never. Back to *Normal* they load again as soon as there are zone slots to
  spare.
- *Vistas* (CAMERA, *Detail and draw distance*): the low-detail copies of the city's buildings that
  the game shows further away. All of them on or off, or each zone on or off (nearest the camera
  first). With the camera far from Faith, the vistas around each of them were where the other had
  the real buildings loaded, so both showed at once; *Hide a vista where its buildings are loaded*
  (on by default) hides a zone's vista while its own buildings are loaded.
- The camera's streaming points are the corners of a fixed 200 m grid, not rings around the camera:
  moving on only brings in the corners ahead and lets go of the ones behind, and a corner only goes
  100 m past the stream distance (or 15 degrees outside the view). The rings moved every point each
  time the camera had gone 100 m, which loaded and unloaded zones all the way round.
- The zone count counts the zones that were asked for and haven't loaded yet, so a focus that asks
  for too many is taken away before they take their slots. *Zones loaded or loading* in the menu.
- Far from every limit, the camera's points go in every 300 ms instead of each waiting for the last
  one's zones to load (as long as no more than 4 zones are still waiting).

## Version 1.2.2

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
