#pragma once
#include "CameraStructs.h"
#include <vector>

// Keyframed camera tracks. Nodes are interpolated with Catmull-Rom splines
// (position, rotation and field of view). Not thread safe on its own,
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
  CatmullRomNode PlayForward(double dt, bool ignoreManual = false);

  void DrawUI(Camera& camera, bool cameraEnabled);

  bool IsPlaying() { return m_IsPlaying; } // paused counts as playing: the track still sets the camera
  bool IsPaused() { return m_IsPlaying && m_IsPaused; }
  bool IsRotationLocked() { return m_LockRotation; }
  bool IsFovLocked() { return m_LockFieldOfView; }
  bool IsMotionBlurLocked() { return m_LockMotionBlur; }
  bool IsTimeOfDayLocked() { return m_LockTimeOfDay; }

  // Points along the selected track for the zone streamer: each keyframe and
  // every `spacing` metres between them. Gives how many were written.
  int GetStreamingPoints(DirectX::XMFLOAT3* points, int max, float spacing);

  // For the status bar
  double GetPlaybackTime() { return m_CurrentTime; }
  float GetTrackLength() { auto& nodes = m_Tracks[m_SelectedTrack].Nodes; return nodes.empty() ? 0.f : nodes.back().TimeStamp; }
  std::string const& GetTrackName() { return m_Tracks[m_SelectedTrack].Name; }
  int GetNodeCount() { return (int)m_Tracks[m_SelectedTrack].Nodes.size(); }

private:
  void CreateTrack();
  void DeleteTrack();

  void UpdateNameList();

private:
  bool m_IsPlaying;
  bool m_IsPaused{ false }; // only means something while playing

  bool m_LockRotation;
  bool m_LockFieldOfView;
  bool m_LockMotionBlur{ true };
  bool m_LockTimeOfDay{ false }; // Off by default: the clock keeps running while keyframes are added
  bool m_ManualPlay;
  float m_PlaybackSpeed;   // 1.0 = node durations in real time
  float m_DefaultDuration; // Seconds between new nodes
  int m_DefaultInterpolation; // KeyframeInterpolation for new nodes

  unsigned int m_CurrentNode;
  double m_CurrentTime;

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
