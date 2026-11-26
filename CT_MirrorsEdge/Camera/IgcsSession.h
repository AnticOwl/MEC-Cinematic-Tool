#pragma once

// The production capture state and camera math. Kept independent of Windows
// and DirectX so the same operations can be exercised by the portable tests.
#include <cmath>
#include <cstdint>

namespace igcs
{
namespace capture
{
  struct Vec3
  {
    float x{ 0.f }, y{ 0.f }, z{ 0.f };
  };

  struct Quaternion
  {
    float x{ 0.f }, y{ 0.f }, z{ 0.f }, w{ 1.f };
  };

  struct Pose
  {
    Vec3 position;
    Quaternion rotation;
    float fovDegrees{ 60.f };
  };

  struct Basis
  {
    Vec3 right;
    Vec3 up;
    // Frostbite / DirectX rotation-matrix row 2. The camera looks down -Z.
    Vec3 forward;
  };

  inline bool ValidCoordinate(float value)
  {
    // Far beyond the city's extent; reject infinities and destructive input
    // before any value can reach the game's view or its streaming system.
    return std::isfinite(value) && std::fabs(value) <= 10000000.f;
  }

  inline bool BuildBasis(Quaternion const& rotation, Basis& basis)
  {
    if (!std::isfinite(rotation.x) || !std::isfinite(rotation.y) ||
        !std::isfinite(rotation.z) || !std::isfinite(rotation.w))
      return false;

    double x = rotation.x, y = rotation.y, z = rotation.z, w = rotation.w;
    const double lengthSquared = x * x + y * y + z * z + w * w;
    if (!std::isfinite(lengthSquared) || lengthSquared < 1e-12)
      return false;
    const double inverseLength = 1.0 / std::sqrt(lengthSquared);
    x *= inverseLength;
    y *= inverseLength;
    z *= inverseLength;
    w *= inverseLength;

    // Same row-vector convention as XMMatrixRotationQuaternion.
    basis.right = { (float)(1.0 - 2.0 * (y * y + z * z)),
                    (float)(2.0 * (x * y + z * w)),
                    (float)(2.0 * (x * z - y * w)) };
    basis.up = { (float)(2.0 * (x * y - z * w)),
                 (float)(1.0 - 2.0 * (x * x + z * z)),
                 (float)(2.0 * (y * z + x * w)) };
    basis.forward = { (float)(2.0 * (x * z + y * w)),
                      (float)(2.0 * (y * z - x * w)),
                      (float)(1.0 - 2.0 * (x * x + y * y)) };
    return true;
  }

  inline bool IsValidPose(Pose const& pose)
  {
    Basis unused;
    return ValidCoordinate(pose.position.x) && ValidCoordinate(pose.position.y) &&
      ValidCoordinate(pose.position.z) && std::isfinite(pose.fovDegrees) &&
      pose.fovDegrees >= 5.f && pose.fovDegrees <= 150.f && BuildBasis(pose.rotation, unused);
  }

  class Session
  {
  public:
    // Return codes are the native IGCS ScreenshotSessionStartResult ABI.
    // Only multishot (1), which IGCSDOF uses, is supported in this version.
    int Start(std::uint8_t type, bool cameraEnabled, bool trackPlaying, Pose const& pose)
    {
      if (type != 1) return 4;
      if (m_Active) return 3;
      if (!cameraEnabled) return 1;
      if (trackPlaying) return 2;
      if (!IsValidPose(pose)) return 5;

      BuildBasis(pose.rotation, m_Basis);
      m_Origin = pose;
      m_Current = pose;
      m_Active = true;
      return 0;
    }

    bool Move(float horizontal, float vertical, float fovDegrees, bool fromStartPosition)
    {
      if (!m_Active || !std::isfinite(horizontal) || !std::isfinite(vertical) ||
          !std::isfinite(fovDegrees) || std::fabs(horizontal) > 100000.f ||
          std::fabs(vertical) > 100000.f)
        return false;
      if (fovDegrees > 0.f && (fovDegrees < 5.f || fovDegrees > 150.f))
        return false;

      Pose next = m_Current;
      const Vec3& start = fromStartPosition ? m_Origin.position : m_Current.position;
      next.position = {
        (float)((double)start.x + (double)m_Basis.right.x * horizontal + (double)m_Basis.up.x * vertical),
        (float)((double)start.y + (double)m_Basis.right.y * horizontal + (double)m_Basis.up.y * vertical),
        (float)((double)start.z + (double)m_Basis.right.z * horizontal + (double)m_Basis.up.z * vertical)
      };
      // Zero or a negative value means "leave the FOV unchanged" in IGCS.
      if (fovDegrees > 0.f)
        next.fovDegrees = fovDegrees;
      if (!IsValidPose(next))
        return false;

      m_Current = next;
      return true;
    }

    // No asynchronous restore: an End followed immediately by Start must
    // snapshot the original camera, including during a ReShade shader reload.
    bool End(Pose& restored)
    {
      if (!m_Active) return false;
      restored = m_Origin;
      m_Current = m_Origin;
      m_Active = false;
      return true;
    }

    bool IsActive() const { return m_Active; }
    Pose const& GetOrigin() const { return m_Origin; }
    Pose const& GetCurrent() const { return m_Current; }
    Basis const& GetBasis() const { return m_Basis; }

  private:
    bool m_Active{ false };
    Pose m_Origin;
    Pose m_Current;
    Basis m_Basis;
  };
}
}
