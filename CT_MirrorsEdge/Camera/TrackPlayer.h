#pragma once
#include "CameraStructs.h"
#include <map>
#include <string>
#include <utility>
#include <vector>

// Keyframed camera tracks. Nodes are interpolated with Catmull-Rom splines
// (position, rotation and field of view); where a keyframe's curve handles
// were set by hand, the position follows the Bezier curve through them
// (CatmullRomNode::Handles). Not thread safe on its own,
// CameraManager guards every call with its mutex.
class TrackPlayer
{
public:
  TrackPlayer();
  ~TrackPlayer();

  void CreateNode(Camera const& camera);
  void DeleteNode();

  void Toggle();
  void Stop();
  void TogglePause(); // while playing: holds the track where it is, or carries on
  // Goes to a moment of the track (seconds of track time). A stopped track
  // starts there, paused, so the camera shows that frame.
  void Seek(double time);
  // Whether the track was sent to another moment since the last call, and
  // where: its time (seconds of track time, a shot sequence's earlier shots
  // in it), and how much game time the track makes up to there (while it
  // sets the game speed, its keyframes' speeds summed; else the time times
  // sliderSpeed, Game speed's own), for a replay that goes at the game speed
  bool TakeSeek(double& time, double& gameTime, float sliderSpeed);
  CatmullRomNode PlayForward(double dt, bool ignoreManual = false);

  void DrawUI(Camera& camera, bool cameraEnabled);

  bool IsPlaying() { return m_IsPlaying; } // paused counts as playing: the track still sets the camera
  bool IsPaused() { return m_IsPlaying && m_IsPaused; }
  // The keyframes being dragged right now, in the menu or on the drawn path
  // (they change every frame until let go)
  bool IsBeingEdited() { return m_Changing || m_PathDragging; }
  bool IsRotationLocked() { return m_LockRotation; }
  bool IsFovLocked() { return m_LockFieldOfView; }
  bool IsMotionBlurLocked() { return m_LockMotionBlur; }
  bool IsTimeOfDayLocked() { return m_LockTimeOfDay; }
  bool IsShakeLocked() { return m_LockShake; }
  bool IsGameSpeedLocked() { return m_LockGameSpeed; }
  void SetGameSpeedLocked(bool locked) { m_LockGameSpeed = locked; }   // from the config

  // Undo / redo of the keyframes' changes (adding, deleting, moving and
  // editing them; MaxUndoSteps of them): false when there's none
  bool Undo();
  bool Redo();

  // The shot sequence: tracks played one after the other, cutting from each
  // to the next (see DrawSequenceUI)
  void PlaySequence();
  bool IsSequencePlaying() { return m_IsPlaying && m_SequencePlaying; }
  // Whether a playing sequence cut to its next shot since the last call
  bool TakeCut();

  // Points along the selected track for the zone streamer: each keyframe and
  // every `spacing` metres between them. Gives how many were written.
  int GetStreamingPoints(DirectX::XMFLOAT3* points, int max, float spacing);
  // The selected track's path, for drawing it: perSegment points between each
  // two keyframes, where the camera goes when it plays (the same
  // interpolation), the last keyframe last; breaks[i]: no line from the point
  // before (a Hold keyframe cuts to the next). Gives how many were written.
  // Orbit keyframes are round their own points (CatmullRomNode::OrbitFocus),
  // as they play. Everywhere here focus is the focus where it is now (null:
  // none) and baseYaw the heading the circle turns with, for an orbit
  // keyframe without a point of its own only.
  int GetPathPoints(DirectX::XMFLOAT3* points, bool* breaks, int max, int perSegment, DirectX::XMFLOAT3 const* focus,
    float baseYaw);
  // Its keyframes' places (an orbit keyframe's round its point, the same
  // way; roundFocus[i], when given, says which are), and when given, their
  // views there (rotations: an orbit keyframe's turned with it round its
  // point, as it plays) and fields of view (degrees, vertical)
  int GetKeyframePoints(DirectX::XMFLOAT3* points, int max, DirectX::XMFLOAT3 const* focus, float baseYaw,
    bool* roundFocus = nullptr, DirectX::XMFLOAT4* rotations = nullptr, float* fieldsOfView = nullptr);
  // One keyframe's place as it plays (an orbit keyframe's round its point)
  bool GetKeyframePlace(int index, DirectX::XMFLOAT3 const* focus, float baseYaw, DirectX::XMFLOAT3& place);
  // The point an orbit keyframe goes round (its own, or the focus where it is
  // for one without); false when it can't be placed. SegmentPivot: the point
  // the move from index to the next goes round (its first end's, else its
  // second's, as it plays)
  bool GetOrbitPivot(int index, DirectX::XMFLOAT3 const* focus, float baseYaw, DirectX::XMFLOAT3& pivot);
  bool SegmentPivot(int index, DirectX::XMFLOAT3 const* focus, float baseYaw, DirectX::XMFLOAT3& pivot);
  // An orbit keyframe dragged round its point on the drawn path: its place
  // round it (yaw from the world's heading, pitch, distance) with its own
  // view kept, so it turns with it; its kept place and rotation as it plays
  // there. Part of the drag's undo step.
  bool SetOrbitKeyframePlace(int index, float yaw, float pitch, float distance, DirectX::XMFLOAT3 const* focus,
    float baseYaw);
  // The arch of the move from index to the next round its point (as it
  // plays): its middle with its rise and bulge (place), and the plain arc's
  // there, without them: its heading (radians, from the world's), pitch,
  // distance and the point it goes round there (pivot). False when that move
  // doesn't go round a point.
  bool GetOrbitArch(int index, DirectX::XMFLOAT3 const* focus, float baseYaw, DirectX::XMFLOAT3& place, float& yaw,
    float& pitch, float& distance, DirectX::XMFLOAT3& pivot);
  // Its rise (radians, CatmullRomNode::OrbitLift) and bulge (metres,
  // OrbitBulge) from the drawn path's arch handle: each eased back towards
  // none as far as needed for the whole arc to stay at least minDistance from
  // its point and within maxPitch of level (or as near as the plain arc is).
  // Part of the drag's undo step; false when it isn't a move round a point.
  bool SetOrbitArch(int index, float lift, float bulge, DirectX::XMFLOAT3 const* focus, float baseYaw,
    float minDistance, float maxPitch);
  // From the drawn path's menu (right click), one undo step each: a keyframe
  // deleted (as the list's Delete does), its curve handles forgotten (the
  // automatic curve again), the arch to the next flattened
  bool DeleteKeyframe(int index);
  bool ResetKeyframeHandles(int index);
  bool FlattenOrbitArch(int index);
  // The camera to a keyframe's view, as its row's Go to does (an orbit
  // keyframe's round its point, as it plays)
  void GoToKeyframe(int index, Camera& camera, DirectX::XMFLOAT3 const* focus, float baseYaw);

  // The track previewed on its drawn path (CAMERA > Preview the track on its
  // path; CameraManager::DrawTrackPath's preview camera), set every menu
  // frame: while it's on, a keyframe being changed in the list doesn't move
  // the camera there, the preview goes to that keyframe instead (its time,
  // TakePreviewHold, taken once)
  void SetPreviewOnPath(bool on) { m_PreviewOnPath = on; }
  bool TakePreviewHold(double& time);
  // The selected track at a moment (seconds of track time) as it plays, for
  // the preview, nothing set: where the camera would be, its view and field
  // of view; orbit keyframes round focus where it is now (null: as kept).
  // False with fewer than two keyframes.
  bool PreviewAt(double time, DirectX::XMFLOAT3 const* focus, float baseYaw, DirectX::XMFLOAT3& place,
    DirectX::XMFLOAT4& rotation, float& fieldOfView);

  // Dragging the drawn path's keyframes and curve handles in the view
  // (CameraManager::DrawTrackPath): each drag is one undo step, made by its
  // first change (a click that moves nothing makes none)
  void BeginPathDrag();
  bool EndPathDrag();   // whether the drag changed anything
  bool SetKeyframePosition(int index, DirectX::XMFLOAT3 const& position);
  // Where a keyframe's curve handles are (in: the way in from the keyframe
  // before; out: the way out to the next), its own or its automatic curve's;
  // hasIn / hasOut whether that side's handle shapes the path (a Smooth or
  // Ease in/out segment that moves, not drawn round the focus). False when
  // neither does.
  bool GetKeyframeHandles(int index, bool hasFocus, DirectX::XMFLOAT3& in, DirectX::XMFLOAT3& out, bool& hasIn,
    bool& hasOut);
  // One of its handles dragged to place: from then on its handles are set by
  // hand; the other one turns with it unless they move on their own
  bool SetKeyframeHandle(int index, bool out, DirectX::XMFLOAT3 const& place);
  // The keyframe picked on the drawn path (-1: none), shown in the list
  int GetPickedKeyframe();
  void PickKeyframe(int index);

  // Orbit keyframes ticked in the menu, and Find the best way round pressed:
  // worked out on the game thread (CameraManager::ProcessOrbitRequests),
  // which knows the focus and can check for walls
  struct OrbitRequest
  {
    int Track;
    int Keyframe;
    bool Ticked;   // its place round the focus (if it has none) and the ways round on both sides; else the way to the next
  };
  bool TakeOrbitRequest(OrbitRequest& request);
  void SetOrbitFocusKnown(bool known) { m_OrbitFocusKnown = known; }  // whether there's a focus to tick keyframes round
  // Where the focus is now, each frame (CameraManager::UpdateCamera; null:
  // none): an orbit keyframe without a point of its own plays round it where
  // it is, live (one with its own, round that: CatmullRomNode::OrbitFocus)
  void SetLiveFocus(DirectX::XMFLOAT3 const* focus, float baseYaw)
  {
    m_LiveFocusOk = focus != nullptr;
    if (focus)
      m_LiveFocus = *focus;
    m_LiveBaseYaw = baseYaw;
  }
  int GetSelectedTrackIndex() { return m_SelectedTrack; }
  // A move from index to the next that goes round the focus: an orbit
  // keyframe at either end, moving (not Hold)
  bool IsOrbitSegment(int index);
  // A ticked keyframe placed round pivot, the focus where it is when it's
  // ticked, kept (CatmullRomNode::OrbitFocus; yaw from the world's heading),
  // its own view kept (CatmullRomNode::OrbitView), or unticked again when it
  // can't be (no focus); both part of the tick's step
  bool SetOrbitPlace(int index, float yaw, float pitch, float distance, DirectX::XMFLOAT3 const& pivot);
  void UntickOrbit(int index);
  // How far round the track's path goes about pivot's upright line from
  // keyframe index to the next (radians of yaw, more than a turn if it
  // loops; + to the right), along the path between their own places
  float PathSweep(int index, DirectX::XMFLOAT3 const& pivot);
  // How far round (radians of yaw) the orbit from index to the next would go
  // that way with those turns (see CatmullRomNode::OrbitWay); an end that
  // isn't an orbit keyframe where it is round the other's point
  float OrbitStepWith(int index, int way, int turns, DirectX::XMFLOAT3 const* focus, float baseYaw);
  // That orbit's places round its point as it would play with that way, turns
  // and lift, from index to the next: max points at most, more for a longer
  // arc. Gives how many were written.
  int OrbitArcPoints(int index, int way, int turns, float lift, DirectX::XMFLOAT3 const* focus, float baseYaw,
    DirectX::XMFLOAT3* points, int max);
  // The way found for the orbit from index to the next: a step of its own,
  // or part of the tick's (ownStep false)
  bool SetOrbitWay(int index, int way, int turns, float lift, bool ownStep);
  // What the last search found, shown in the keyframe's row
  void SetOrbitNote(int index, std::string const& note);
  // The selected track at a moment (seconds of track time), as it plays;
  // false where that isn't a place of its own (round a focus, or no track)
  bool SampleAt(double time, CatmullRomNode& out);
  // The selected track's time of day at time (seconds of track time) as it
  // plays it; false where its keyframes have none (CAMERA > Time-lapse)
  bool TimeOfDayAt(double time, float& clock);
  // The selected track's keyframes
  std::vector<CatmullRomNode> const& GetNodes() { return m_Tracks[m_SelectedTrack].Nodes; }
  // Keyframes put in at those moments with those places (the rest as the
  // track has it there), one undo step; not within MinInsertGap of another,
  // nor in a Hold or orbit segment. Gives how many went in.
  int InsertNodes(std::vector<std::pair<double, DirectX::XMFLOAT3>> const& inserts);
  static constexpr double MinInsertGap = 0.05;

  // For the status bar
  double GetPlaybackTime() { return m_CurrentTime; }
  // The time from the start of what plays: a playing shot sequence's earlier shots added (as Seek gives a replay)
  double GetSequenceTime() { return m_CurrentTime + (m_SequencePlaying ? m_SequenceOffset : 0.0); }
  float GetPlaybackSpeed() { return m_PlaybackSpeed; }
  float GetTrackLength() { auto& nodes = m_Tracks[m_SelectedTrack].Nodes; return nodes.empty() ? 0.f : nodes.back().TimeStamp; }
  std::string const& GetTrackName() { return m_Tracks[m_SelectedTrack].Name; }
  int GetNodeCount() { return (int)m_Tracks[m_SelectedTrack].Nodes.size(); }

private:
  void CreateTrack();
  void DeleteTrack();

  void PushUndo();       // the selected track's keyframes as they are, before a change
  void SyncShadow();     // after a change made outside the menu (already a step)
  void NoteChanges();    // the menu's changes since last frame: one step for each (a drag is one)
  void StepPathDrag();   // a drag on the path changes them: its undo step, once
  bool NextShot(double endTime);   // a playing sequence's next shot (false: that was the last)
  // The game time a track makes from its start to time (see TakeSeek)
  double GameTimeAt(int track, double time, float sliderSpeed);
  void DrawSequenceUI(bool cameraEnabled);
  CatmullRomNode NodeFromCamera(Camera const& camera);

  void UpdateNameList();

private:
  bool m_IsPlaying;
  bool m_IsPaused{ false }; // only means something while playing

  bool m_LockRotation;
  bool m_LockFieldOfView;
  bool m_LockMotionBlur{ true };
  bool m_LockTimeOfDay{ false }; // Off by default: the clock keeps running while keyframes are added
  bool m_LockShake{ false };     // the keyframes' shake strength (off: the menu's own)
  bool m_LockGameSpeed{ false }; // the keyframes' game speed (off: Game speed's own)

  struct UndoStep
  {
    int Track;
    std::vector<CatmullRomNode> Nodes;
  };
  std::vector<UndoStep> m_Undo, m_Redo;
  std::vector<CatmullRomNode> m_Shadow;  // the selected track's keyframes as the menu last saw them
  int m_ShadowTrack{ -1 };
  bool m_Changing{ false };              // a change in the menu is going on (a drag): one step for all of it
  // A drag on the drawn path: the keyframes as they were before it (its undo step)
  bool m_PathDragging{ false };
  bool m_PathDragStepped{ false };
  int m_PathDragTrack{ -1 };
  std::vector<CatmullRomNode> m_PathDragBefore;
  int m_PickedKeyframe{ -1 };            // picked on the drawn path, in m_PickedTrack
  int m_PickedTrack{ -1 };
  int m_PickedCount{ 0 };                // its keyframes then
  int m_OpenPicked{ -1 };                // its row in the list opens once
  std::vector<OrbitRequest> m_OrbitRequests;
  bool m_PreviewOnPath{ false };
  bool m_PreviewHeld{ false };            // a keyframe changed in the list this frame, with the preview on
  double m_PreviewHeldTime{ 0.0 };        // its time
  bool m_OrbitFocusKnown{ false };
  bool m_LiveFocusOk{ false };
  DirectX::XMFLOAT3 m_LiveFocus{};
  float m_LiveBaseYaw{ 0.f };
  // The orbit searches' results by keyframe, for m_OrbitNotesTrack while it
  // has m_OrbitNotesCount keyframes (else they'd be other keyframes' now)
  std::map<int, std::string> m_OrbitNotes;
  int m_OrbitNotesTrack{ -1 };
  int m_OrbitNotesCount{ 0 };

  std::vector<int> m_Sequence;           // the shots: track indices
  bool m_SequencePlaying{ false };
  bool m_SequenceOnKey{ false };         // the play key plays the sequence
  size_t m_SequencePos{ 0 };
  double m_SequenceOffset{ 0.0 };        // the earlier shots' length (for a replay playing along)
  bool m_Cut{ false };
  bool m_ManualPlay;
  float m_PlaybackSpeed;   // 1.0 = node durations in real time
  float m_DefaultDuration; // Seconds between new nodes
  int m_DefaultInterpolation; // KeyframeInterpolation for new nodes

  unsigned int m_CurrentNode;
  double m_CurrentTime;
  bool m_Sought{ false };   // Seek since the last TakeSeek
  double m_SoughtTime{ 0 };
  int m_SoughtTrack{ 0 };            // where it was sent: the track, its time, the sequence's shot
  double m_SoughtLocal{ 0 };
  bool m_SoughtSequence{ false };
  size_t m_SoughtSequencePos{ 0 };

  std::vector<CameraTrack> m_Tracks;
  int m_SelectedTrack;

  std::vector<const char*> m_TrackNames;
  int m_RunningId;

  // Track rename box
  char m_RenameBuffer[64]{};
  int m_RenamingTrack{ -1 };

public:
  TrackPlayer(TrackPlayer const&) = delete;
  void operator=(TrackPlayer const&) = delete;

};
