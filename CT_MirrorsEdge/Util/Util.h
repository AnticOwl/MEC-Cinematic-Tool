#pragma once

#include <DirectXMath.h>
#include <string>
#include <vector>
#include <Windows.h>

struct ID3D11DeviceContext;

namespace util
{
  namespace hooks
  {
    enum HookType
    {
      MinHook,
      VTable
    };

    struct Hook
    {
      __int64 Address;
      unsigned int Index;
      LPVOID* Original;
      HookType Type;
      bool Enabled;
      bool Keep; // stays on when all hooks are turned off (on exit)
    };

    // Reserves address space next to the game exe for MinHook's trampolines.
    // Call as early as possible, before the game's heap fills that space.
    void ReserveSpaceNearGame();
    void Init();

    // if name is empty, then perform on all hooks
    void SetHookState(bool enabled, std::string const& name = "");

    // A MinHook hook made from outside this file (logged, and turned off on
    // exit with the others). False if it couldn't be made.
    bool CreateGameHook(char const* name, __int64 target, PVOID detour, LPVOID* original);

    // Notes in the log, once, if presents don't pass the tools' jump in
    // dxgi's Present (the tools then draw from the game's present function
    // only). Call about once a second from the tools' thread.
    void CheckPresentHooks();

    // The renderer's draw batches: category 0's instances go on into its
    // unused batches once one is full to this many chunks (0 off, at most
    // 1748; see hBatchAdd). Stays 0 if the add isn't hooked.
    void SetBatchSpread(int chunks);
    int BatchSpread();
    // Whether the add is hooked (without it the spread stays 0)
    bool BatchSpreadHooked();

    // From the window procedure: while hold is on the game's ShowCursor calls
    // on this thread only change a count of its own and the cursor stays
    // shown; off, the real display count is set to the game's. False if
    // ShowCursor isn't hooked.
    bool HoldCursorShown(bool hold);

    // Writes to the log how many Enlighten light map ids were turned down
    // in all, when there are new ones since the last call (see hLightMapId).
    // Call once a second from the tools' thread.
    void LogLightMapIds();
    // Every second: the client physics world's bodies against its room
    void LogPhysicsBodies();
    // The game's job queue (see the hooks' "The game's job queue"): how full
    // it gets, sampled from every pass of the tools' loop; logged (its room,
    // the most waiting past half of it, the times it was found full) once a
    // second from the tools' thread
    void SampleJobQueue();
    void LogJobQueue();
    // The guard on the game's crash quitting during a Dash (see the hooks'
    // "quitting during a Dash"): logged when it answered for a missing world
    // since the last call. Once a second from the tools' thread, and at exit
    void LogMissionCleanupGuard();
    // A time jump (the clock before and after, game seconds), any thread: the
    // job queue's peak in the 10 s after it (after the last of several in a
    // row) is logged
    void NoteTimeJump(float from, float to);
    // The hooks that couldn't be made this game (their names), for the menu
    std::vector<std::string> FailedHooks();

    // The reflection volumes around the camera get the 32 cubemaps first,
    // the farther from the camera the lower a volume ranks (see
    // hReflectionScore). False if the score isn't hooked.
    bool SetNearReflections(bool on);

    // The sun shadows' reach from the camera (the tools' Shadow distance, 0
    // for the game's): also over a reach the level sets itself, which the
    // game takes over WorldRenderSettings' (see hSunShadowSetup). Any thread.
    void SetSunShadowReach(float metres);
    // Diagnostic (temporary): the sun shadow reach's inputs per view, when
    // they change. Call once a second from the tools' thread.
    void LogSunShadowReach();

    // Lens flares: each element's distance curves held at their end, and
    // the element faded out from its distance to twice that (see the
    // hooks' "Lens flares far away"). Any thread.
    void SetFarFlareFade(bool on);

    // Reflection captures: a whole cubemap a frame instead of the game's
    // faces-per-frame setting while on (see the hooks' "Reflection captures
    // after a time jump"). Any thread.
    void SetReflectionCatchUp(bool on);

    // The time of day's lighting after a jump (see the hooks' "The time of
    // day's lighting after a jump"), any thread: whether its update is
    // hooked; the game's own Reset of it asked for, for the time the tools set
    // (done in its update once its own clock is there and it may be); whether
    // one is asked for and not done yet, or its passes still run; true once
    // when the reflections should be captured again: 3 of a reset's first
    // passes done, and again all of them (passes: how many are done, of: of
    // how many).
    bool TodResetAvailable();
    // (returns the request, 0 when not hooked. forced: a time-lapse frame's, done at the time asked however close
    // the light shown is, its clock waited for until within 5 s of it; quiet: no log lines but warnings. Called
    // under VisualsManager's lock only, so two requests never get the same id.)
    long RequestTodReset(float clock, bool forced = false, bool quiet = false);
    bool TodResetBusy();
    bool TakeTodResetSettled(int& passes, int& of);
    // What became of a request (CAMERA > Time-lapse): working; done (its reset's three jobs, lit at litAt, in
    // seconds; clockAt: its lighting's own clock when the reset was done); served without a reset (its lighting
    // can't have one: no clock, no jobs, its countdown); its reset not done in 60 s
    enum TodOutcome { TodOutcome_Working, TodOutcome_Done, TodOutcome_NoReset, TodOutcome_Failed };
    int TodResetOutcome(long id, float& litAt, double& seconds, float* clockAt = nullptr);
    // While on, the game's own steps of its time of day lighting are held (no new step starts), so the light shown
    // stays the last reset's (a time-lapse running); whether the tools know that code; real ms since its lighting
    // last updated (-1: never)
    void SetTodHoldSteps(bool hold);
    bool TodHoldAvailable();
    long long TodSinceUpdateMs();
    // The level's clock went or changed: the lighting managers seen are forgotten at the next update
    void ForgetTodManagers();
    // Whether its lighting may follow a clock that runs fast (a time-lapse:
    // the hooks' "Following a clock that runs fast"), every update (Light
    // catches up with time jumps on, a level, no MECMP)
    void SetTodFollow(bool wanted);

    // Replay guns and drones (see the hooks' "Replay guns and drones"), the
    // game thread only: an entity moved where it's drawn (4x4 rows, its flags'
    // physics and queued bits held off for the call; false if it couldn't be),
    // and the game's own moves of it held back while it's held. ReleaseActors
    // lets them all go and gives how many of the game's moves were held back.
    bool MoveActorDrawn(__int64 entity, float const* transform);
    void HoldActor(__int64 entity, bool hold);
    int ReleaseActors();
    // GAME > Pose's holds (a posed character's weapons), a list of their own
    // that ReleaseActors leaves alone (false: the list is full). The game
    // thread only. PoseActorMovesHeld: the game's moves held back since the
    // last call.
    bool PoseHoldActor(__int64 entity, bool hold);
    int PoseActorMovesHeld();
    bool IsModelSetTransformHooked();
    // The replay's hook on the models' setTransform body turned off once MECMP
    // is in the game (MECMP checks that function). Any thread, once a second
    void KeepBodyHookOffForMecmp();
    // Replay vehicles (see the hooks' "Replay vehicles"), the game thread only:
    // a vehicle moved where it's drawn, its physics body left where it is
    // (false if it couldn't be); held with HoldActor like a gun. Its server
    // entity kept in the level by who it is while the replay holds people
    // (SetKeptVehicles, every frame; ForgetKeptVehicles when the recording
    // goes).
    bool MoveVehicleDrawn(__int64 entity, float const* transform);
    // The same with its physics body moved too (a vehicle is drawn where its
    // physics has it)
    bool MoveVehiclePhysics(__int64 entity, float const* transform);
    bool CanMoveVehicles();
    // A vehicle kept at these rows (4x4, rotation rows then the place) when
    // the game's queued changes of its transform are brought to what's drawn
    // (see the hooks' hTransformQueued); null rows: not any more (false if
    // that isn't hooked, or no room). ReleaseDrawnVehicles: none, and how many
    // of the game's changes were given the replay's place.
    bool DrawVehicleAt(__int64 entity, float const* rows);
    int ReleaseDrawnVehicles();
    void SetKeptVehicles(__int64 const* entities, int count);
    void ForgetKeptVehicles();

    // Replay sounds (see the hooks' "Replay sounds"), game thread only:
    // while the replay records, every frame with its clock (sample: it kept
    // a pose this frame); while it plays, every frame with its time (jumped:
    // a seek or a loop, the events in between aren't given)
    void ReplaySoundsStartRecording();
    void ReplaySoundsRecordFrame(double time, bool sample);
    void ReplaySoundsStopRecording();
    void ReplaySoundsPlay(double time, bool jumped);
    void ReplaySoundsStop();
    // The dialog (2026-11-01): the recorded voice lines' spans on the replay's
    // clock (false: the voice lines aren't hooked); whether a recorded line the
    // replay isn't saying live covers a time (the sound track's is heard then,
    // the span widened by before / after seconds); whether any was recorded
    bool ReplayVoiceLineSpans(std::vector<std::pair<double, double>>& out);
    bool ReplayDialogFromTrack(double time, double before, double after);
    bool ReplayHasVoiceLines();
    // A replay plays (or not any more): meanwhile the live game's sounds near
    // the real Faith (her own, the guns' and the KrugerSec's, and the
    // bullets' impacts) are left out (see the hooks' g_ReplayPlaying)
    void SetReplayPlaying(bool playing);

    // Diagnostic (temporary): records the sounds the game makes and every
    // event their voices get, with callers and distances from Faith (see
    // hVoiceEvent). False if the hooks aren't there.
    bool StartFaithSoundLog(double seconds);
    // Seconds left, 0 when it isn't recording
    double FaithSoundLogRemaining();
    // Every frame from the camera hook: where Faith and the free camera are
    // (null: none); at the end it writes what was recorded to the log
    void UpdateFaithSoundLog(float const* faith, float const* camera);

    // Test: while on, the level's spot sounds near the free camera (the club's
    // music, crowd and traffic spots, which the game starts only for Faith)
    // are started for it, and stopped once it's away (see the hooks' "Start
    // spot sounds near the camera"). Every frame from the camera hook (camera:
    // where the free camera is, null: none).
    void UpdateSpotSounds(bool on, float const* camera);
    // Test, with the spot sounds: while on, the crowd's and the traffic's
    // sounds are put on the lines closest to the free camera instead of to
    // where the level tracks (see the hooks' BoidsProximityTrackerEntity).
    // Every frame from the camera hook (camera: its 4 rows, null: none; faith:
    // her position, for the log).
    void UpdateCrowdTrackers(bool on, float const* camera, float const* faith);

    // The replay's people (see the hooks' "Replay people"): while held, the
    // game doesn't take AI out of the level. False if it isn't hooked.
    bool HoldAIDespawn(bool hold);
    // Diagnostic (temporary): the live KrugerSec spawners, in the log
    void LogSpawners(char const* why);
    // Stand-ins for a replay's people that are gone (see the hooks'
    // AskStandIn): live characters the game's own spawners make.
    // StandInsKnown: the code is what the tools know (the server's update must
    // be hooked too, faith_outfit::IsAvailable). WatchSpawners: while on, the
    // server's update notes the spawners' characters (SpawnTemplateNear).
    // AskStandIn: a stand-in of that spawn template (only: 1:1); its id,
    // 0 if none can be asked. StandInState: StandInReady (place: where its
    // server character is), StandInWaiting, or StandInNone (none made or it's
    // gone). RemoveStandIns: all of them out of the level (the playback
    // stopped). ReplayServerUpdate: from the server's update, on its thread.
    enum { StandInNone, StandInWaiting, StandInReady };
    bool StandInsKnown();
    void WatchSpawners(bool on);
    bool SpawnTemplateNear(float const* place, float radius, char* out, size_t size);
    // StandInPossible: 2 a spawner of that template has room for one, 0 none
    // (its kind isn't asked: another body). SpawnTemplatesMatch: 2 the same template, 1 the
    // same folder and kind, 0 neither.
    int StandInPossible(char const* templateName);
    int SpawnTemplatesMatch(char const* a, char const* b);
    // A template no stand-in is made of nor taken from: the runner projections
    // (the city's replay markers' ghosts) and Faith's own
    bool IsNoStandInTemplate(char const* templateName);
    int AskStandIn(char const* templateName);
    int StandInState(int id, float* place);
    void RemoveStandIns();
    void ReplayServerUpdate();
    // The recorded mission's world kept once the mission is over (see the
    // hooks' "The recorded mission's world"). MissionWorldsKnown: the code is
    // what the tools know (the server's update must be hooked too,
    // faith_outfit::IsAvailable). HoldMissionWorlds: the option.
    // MissionWorldsRecording: 1 the replay records, 2 it keeps a recording
    // (stopped now), 0 none kept (forgotten). LetMissionWorldsGo: the kept
    // ones go to the game at the server's next update (why: for the log, a
    // string that stays). KeptMissionWorlds: how many are kept.
    bool MissionWorldsKnown();
    void HoldMissionWorlds(bool hold);
    void MissionWorldsRecording(int state);
    void LetMissionWorldsGo(char const* why);
    int KeptMissionWorlds();
    // While held: where the recorded people are (x y z each), and how near
    // one a character the game wants out of the level has to be to be kept
    // (see the hooks' KeepGuard); the kept ones go back to the game when the
    // hold ends.
    void SetGuardPlaces(float const* places, int count, float radius);
    // While on, Faith takes no damage (see the hooks' HumanDamage). False if
    // it isn't hooked.
    bool ProtectFaith(bool on);
    // A body (animatable + 0x70) placed by the game where these rows say (4x4,
    // rotation rows then the place) instead of its own rows or physics part
    // (see the hooks' hBodyTransform); null rows: its own again. False if that
    // isn't hooked. ReleasePlacedBodies: all of them their own again.
    bool PlaceBody(__int64 body, float const* rows);
    void ReleasePlacedBodies();
    // The placement the game last gave a body (animatable + 0x70; 4x4, rotation
    // rows then the place), its physics part's when it has one (see the hooks'
    // hBodyTransform): watched from the first call on, false until the game
    // has placed it since. ReleaseWatchedBodies: none watched.
    bool ReadBodyPlace(__int64 body, float* rows);
    void ReleaseWatchedBodies();
    // A client character's culling box (see the hooks' hCharacterBox): rows
    // (3 x 4 floats), centre (x y z 1), then its two corners around the centre
    // (x y z 0 each). Kept over the game's own until null is given (the game's
    // back, as it was when the first one was given) or ReleaseCullBoxes (all
    // of them). gameCentre (3 floats, may be null): the centre of the game's.
    // False if that isn't hooked or it isn't a client character.
    const int CullBoxFloats = 24;
    bool SetCullBox(__int64 character, float const* box, float* gameCentre);
    void ReleaseCullBoxes();
    // The video billboards' sizes on screen measured from this camera (the
    // free camera's transform, 4 rows as the game's camera entry has them,
    // and its vertical field of view in degrees) while on; from the game's
    // camera otherwise (see the hooks' hBillboardSizes). Every frame.
    void SetBillboardCamera(bool on, float const* transform, float fovYDegrees);
    // While the tools' Game speed isn't 1x, the video billboards go by the
    // game's time, not their sound's (see the hooks' hFeedTime). Every frame.
    void SetFeedsAtGameSpeed(bool on);
    // The green screen's depth: the game's depth clears seen (see
    // greenplane::OnDepthClear). Render thread, once; false if it can't be.
    bool HookDepthClears(ID3D11DeviceContext* pContext);
    // Whether a loading screen has been up long enough that a load is taken as
    // waiting on what the replay keeps (see the hooks' hLoadScreenEvent)
    bool LoadScreenWaiting();
    // Whether the game's loading screen is up, by its show and hide events (see
    // the hooks' hLoadScreenEvent; a show with no hide is forgotten after 5
    // minutes). Any thread
    bool LoadScreenShown();
    // While the loading screen has been up a while, what the game's level
    // streaming still waits on goes in the log (see the hooks'
    // WatchLoadScreen). Every frame, from the camera update
    void WatchLoadScreen();
    // Faith's soldier (0: none): when she goes or a new one comes, a
    // recording's sounds play only where the game shows they're still there
    // (see the hooks' g_SoundGen). Every frame, from the camera update
    void NoteFaithSoldier(__int64 soldier);
    // When Faith's soldier went (GetTickCount64; 0 while she's there or before
    // she ever was). Any thread
    ULONGLONG FaithGoneSince();
    // Both level streaming managers' sub-levels by state and the ones not in,
    // in the log with this prefix (see WatchLoadScreen). Any thread, read only
    void LogLevelStreaming(char const* prefix);
    // The sub-levels kept loaded for the replay's people (see the hooks'
    // hSubLevelUnload) unload, each the next time the game asks (a recording
    // forgotten, or a new one started)
    void ReleaseKeptSubLevels();
    // A body (animatable + 0x70) the replay draws itself: the game's own
    // update of it held off while held (see the hooks' hBodyUpdateGate).
    // False if that isn't hooked. ReleaseBodyUpdates lets them all go and
    // gives how many of the game's updates were held off.
    bool HoldBodyUpdate(__int64 body, bool hold);
    int ReleaseBodyUpdates();
    // A body's (animatable + 0x70) "always update" switch (see the hooks'
    // hAlwaysUpdate): whether the game switched it off for the character's
    // death; on for a playback (false: dead, or not hooked); whether the game
    // switched it itself since; back off (if switchOff, and the game didn't
    // switch it meanwhile) and no longer the replay's.
    bool IsBodyDead(__int64 body);
    bool ForceBodyUpdate(__int64 body);
    bool GameSwitchedBody(__int64 body);
    void ReleaseBodyUpdate(__int64 body, bool switchOff);

    // Replay gun effects (see the hooks' "Replay gun effects"), game thread:
    // like ReplaySounds*, on the replay's clock
    void ReplayEffectsStartRecording();
    void ReplayEffectsRecordFrame(double time);
    void ReplayEffectsStopRecording();
    void ReplayEffectsPlay(double time, bool jumped);
    // While the playback is paused: only the lasting effects kept
    void ReplayEffectsHold(double time);
    void ReplayEffectsStop();
    // Models the game shows, hides or switches the look of (mission items: a
    // delivery's package, the cabinet) while it records, set again as they
    // were while it plays; game thread
    void MissionItemsStartRecording();
    void MissionItemsRecordFrame(double time);
    void MissionItemsStopRecording();
    void MissionItemsPlay(double time);
    void MissionItemsStopPlaying();
    // One member's Visible (one of its instances) in a model group
    // (ClientStaticModelGroupEntity), through the group's own handler; game
    // thread. False if the handler isn't hooked, it isn't a group or hasn't
    // that member. The instance isn't checked: below the member's count.
    bool SetModelGroupMemberVisible(__int64 group, int member, int instance, bool visible);
    // Its ObjectVariation the same way: a variation's name hash, or 0 for none (the mesh's own look)
    bool SetModelGroupMemberVariation(__int64 group, int member, int instance, unsigned int nameHash);
    // The free camera for the game's camera area triggers on the areas around
    // the Shard (its top's sub-level, while out of the city) and the Bauble:
    // whether it's on, out of the city's area, and where it is; shardTopAlways:
    // the Shard's top streamed in wherever the view is, camera on or off (the
    // real Shard streamed from anywhere, GAME > Districts)
    void SetAreaTriggerCamera(bool cameraOn, bool outOfCity, float x, float y, float z, bool shardTopAlways);
    // The game's streaming triggers for the Shard's far-away sub-level and the
    // Bauble: unbroken, the whole versions' on and the broken ones' off;
    // shardBackdropOut, both of the Shard's off (its real tower is loaded and
    // the view out of the city); topBlinds 1 / 2, the Shard's top with
    // Kruger's office blinds open / closed (0: as the game has it); else as
    // the game had them. Camera hook, every frame (it looks once a second,
    // and at once on a change)
    void UpdateLandmarkTriggers(bool unbroken, bool shardBackdropOut, int topBlinds);
    // Whether the Bauble's ruins (BaubleRuins) are the version streamed: their
    // camera trigger on 1, off 0, not set up (yet) -1 (Street level makes the
    // ruins' rubble solid only while it's on). Camera hook
    int BaubleRuinsTrigger();
    // The same for the whole mall (BaubleMall): Street level makes the boxes
    // of its roofs only while it's on
    int BaubleMallTrigger();
    // Diagnostic (temporary): a single static model's Visible as the game
    // reads it (false: not one, or not readable); connected: from a property
    // connection rather than its data
    bool ReadStaticModelVisible(__int64 entity, int& visible, bool& connected);
    // Its reader's cell (0: not connected), and pointing the reader at another
    // cell only while it still has the expected one (see the hooks)
    bool ReadStaticModelVisibleCell(__int64 entity, int& visible, __int64& cell);
    bool SwapStaticModelVisibleCell(__int64 entity, __int64 expected, __int64 cell);

    // The replay's MagRope line (see the hooks' hMagropeUpdate), game thread:
    // Faith's line's points kept while it's shown, on the replay's clock, and
    // drawn on her line again while it plays (left to the game while the game
    // shows her own)
    void ReplayRopeStartRecording();
    void ReplayRopeRecordFrame(double time);
    void ReplayRopeStopRecording();
    void ReplayRopePlay(double time);
    void ReplayRopeStop();

    // Diagnostic (temporary): every frame from the camera hook with where
    // Faith is (null: not there); every few seconds the log gets the effects
    // the game played near her (see hEffectPlay)
    void UpdateEffectLog(float const* faith);

    // Faith's footsteps in the wet surfaces' puddles (see the hooks'
    // hImpactSurface): every frame from the camera hook with her soldier (0:
    // none) and where she is (null: not known)
    void SetFaithFeet(__int64 soldier, float const* place);

    // The replay's moving lights (see the hooks' "moving lights"): while on,
    // the lights whose transform changes are noted once each with their
    // transform from before; Take hands them over (entities, 16 floats each)
    void NoteMovingLights(bool on);
    int TakeMovingLights(__int64* entities, float* before, int max);
    // "spot", "sphere", ... for a PBR light's vtable, "?" for anything else
    const char* LightClassName(__int64 vtable);
    // A light's transform (entity + 0x40), while it's still that light
    bool ReadLight(__int64 entity, __int64 vtable, float* transform);
    // Puts a light somewhere, as the game does (the rows, then UpdateLight)
    bool MoveLight(__int64 entity, __int64 vtable, float const* transform);

    // Diagnostic (temporary): for 'seconds' records which of the level's
    // timelines move (doors, elevators) and which of the MagRope line
    // entity's fields change (see the hooks' "world log"). False if the hooks
    // aren't there.
    bool StartWorldLog(double seconds, const char* label);
    // Seconds left, 0 when it isn't recording
    double WorldLogRemaining();
    // Every frame from the camera hook: where Faith is (null: none); at the
    // end it writes what was recorded to the log
    void UpdateWorldLog(float const* faith);

    // Right after the game's code is readable, before the game makes its mesh
    // manager: gives the mesh streaming room for 16384 meshes instead of 4096
    // (see hMeshManagerCtor). Does nothing if it's too late or the code isn't
    // what was read.
    void EnlargeMeshTable();
    // Right after EnlargeMeshTable: room for 512 zones per zone streamer
    // instead of 128 (see hZoneStreamerCtor)
    void EnlargeZoneSlots();
    // How many zone slots a streamer has, and its slots (count: how many to read)
    int ZoneSlotRoom();
    __int64* ZoneSlots(__int64 pStreamer, int& count);
    // Right after EnlargeZoneSlots: room for 1048576 renderer instance records
    // instead of 262144 (see hRecordOwnerCtor), and how many there's room for
    void EnlargeRecordPool();
    unsigned int RecordPoolRoom();
    // The mesh table's room (4096 until it's moved) and where the tools moved
    // it (0 if they didn't)
    int MeshTableRoom();
    __int64 MeshTableBegin();

    // The game's own show / hide for one zone of a vista, past the tools'
    // hook (game thread only). False if it isn't hooked.
    bool ShowVistaZone(__int64 pVista, unsigned int zone, bool show);
  };

  namespace log
  {
    // Always writes Cinematic Tools/CT.log, the console window is optional
    void Init(bool showConsole);

    void Write(const char* format, ...);
    void Warning(const char* format, ...);
    void Error(const char* format, ...);
    void Ok(const char* format, ...);
    // Written straight to the log file, for the crash handler
    void CrashNote(const char* text);
    // With its time and no locks at all, for DllMain at the process's end
    // (Windows has stopped every other thread, one may have held a lock)
    void ExitNote(const char* text);
    // A warning only if no other thread is writing the log right now (false
    // if one is): for the hang watch, the stuck thread may be holding it
    bool TryWarning(const char* format, ...);
  };

  namespace offsets
  {
    struct Signature
    {
      BYTE* Pattern{ nullptr }; // The pattern to search
      std::string Mask;  // Which bytes should be evaluated (x = evaluate, ? = skip)
      
      bool HasReference{ false }; // Interpret the offset from the assembly reference
      int ReferenceOffset{ 0 }; // How far in the signature is the assembly reference
      int ReferenceSize{ 0 }; // How many bytes is the assembly reference (usually 4, obsolete?)
      int AddOffset{ 0 }; // How much bytes should be added to the final result

      __int64 Result{ 0 };

      Signature(std::string const& sig, int offset = 0);
    };

    void Scan();
    __int64 GetOffset(std::string const& name);

    // True once the renderer's draw batches can use all 2048 of their chunks
    // (set during Scan, see WidenBatchChunkIndex in Offsets.cpp); false: only
    // the first 1024 are safe
    bool BatchChunkIndexWidened();
  }

  // MECMP (the multiplayer mod) is in the game: mecmp.dll is loaded, by its
  // injector or its version.dll. The tools leave the time of day alone then.
  // Looked up at most once a second, from any thread.
  bool IsMecmpLoaded();

  // The tools' diagnostics (lines in CT.log and the research-only work behind them: extra hooks, GPU read-backs,
  // dumps): off unless config.ini has [Debug] Diagnostics = true. Read once at start-up (Main::Initialize), before
  // any hook or tools thread exists, so a plain bool is enough.
  extern bool g_DiagnosticsOn;
  inline bool DiagnosticsOn() { return g_DiagnosticsOn; }

  bool GetResource(int, void*&, DWORD&);
  // A resource the build stored packed (Util/PackedResource.h), unpacked; logs why not
  struct UnpackedResource;
  bool GetPackedResource(int ID, UnpackedResource& out);
  std::string VkToString(DWORD vk);
  std::string KeyLparamToString(LPARAM lparam);
  BYTE CharToByte(char c);

  namespace math
  {
    double CatmullRomInterpolate(double y0, double y1, double y2, double y3, double mu);
  }
}