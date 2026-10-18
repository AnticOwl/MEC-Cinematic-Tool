#pragma once
#include <DirectXMath.h>
#include <string>
#include <vector>

// How a keyframe moves on to the next one
enum KeyframeInterpolation
{
  Interpolation_Smooth,    // Catmull-Rom spline through the keyframes
  Interpolation_Linear,    // Straight line, constant speed
  Interpolation_EaseInOut, // Smooth path, cubic bezier ease in and out of the keyframes
  Interpolation_Hold,      // Stay on this keyframe, then cut to the next
  Interpolation_Count
};

struct CatmullRomNode
{
  DirectX::XMFLOAT3 Position{ 0,0,0 };
  DirectX::XMFLOAT4 Rotation{ 0,0,0,1 };
  float FieldOfView{ 60.f };
  float TimeStamp{ 0 }; // Seconds from the start of the track
  int Interpolation{ Interpolation_Smooth }; // For the segment starting at this node
  float MotionBlur{ 0.5f }; // WorldRenderSettings MotionBlurScale (game default 0.5)
  float TimeOfDay{ -1.f };  // Seconds 0 - 86400, -1 = not recorded (no level was loaded)
  bool TimeOfDayBackward{ false }; // The clock runs backwards to the next keyframe
};

struct Camera
{
  DirectX::XMFLOAT3 Position{ 0,0,0 };
  DirectX::XMFLOAT4 Rotation{ 0,0,0,1 };
  float FieldOfView{ 60.f }; // Original MEC tools started at 60 degrees
  float MotionBlur{ 0.5f };  // Motion blur strength while the override is on
  float TimeOfDay{ -1.f };   // The game's time of day, updated every frame while the camera is on

  float MovementSpeed{ 3.0f };
  float RotationSpeed{ DirectX::XM_PI/4 };
  float RollSpeed{ DirectX::XM_PI / 8 };
  float FovSpeed{ 20.0f };
  float MouseSensitivity{ 0.0015f }; // Radians per mouse count

  float dX{ 0 };
  float dY{ 0 };
  float dZ{ 0 };
  float dPitch{ 0 };
  float dYaw{ 0 };
  float dRoll{ 0 };
  float dFov{ 0 };

  // Mouse look, already in radians (not scaled by dt or speed)
  float dMousePitch{ 0 };
  float dMouseYaw{ 0 };

  // Frostbite LinearTransform layout: right, up, forward, translation.
  // The camera looks down -forward (the original tools moved along -row 2 on W).
  DirectX::XMFLOAT4X4 Transform{  1,0,0,0,
                                  0,1,0,0,
                                  0,0,1,0,
                                  0,0,0,1 };
};

struct CameraTrack
{
  explicit CameraTrack(std::string const& name) : Name(name) {}

  std::string Name;
  std::vector<CatmullRomNode> Nodes;
};
