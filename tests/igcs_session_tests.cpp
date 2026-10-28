// Exercises the same portable capture state that CameraManager uses. These
// tests need no game process, Windows APIs, ReShade, or graphics device.
#include "Camera/IgcsSession.h"
#include "Camera/IgcsCameraData.h" // Compiles the complete native ABI assertions.

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <stdexcept>

#ifdef _WIN32
#include <DirectXMath.h>
static_assert(sizeof(void*) == 8, "The native IGCS integration targets x64.");
#endif

using igcs::capture::Basis;
using igcs::capture::Pose;
using igcs::capture::Quaternion;
using igcs::capture::Session;
using igcs::capture::Vec3;

namespace
{
  void Require(bool result, const char* expression, int line)
  {
    if (!result)
    {
      char message[512];
      std::snprintf(message, sizeof(message), "line %d: %s", line, expression);
      throw std::runtime_error(message);
    }
  }

#define REQUIRE(expression) Require(!!(expression), #expression, __LINE__)

  bool Near(float actual, float expected, float tolerance = 0.00001f)
  {
    return std::isfinite(actual) && std::fabs(actual - expected) <= tolerance;
  }

  bool NearVector(Vec3 const& actual, Vec3 const& expected, float tolerance = 0.00001f)
  {
    return Near(actual.x, expected.x, tolerance) && Near(actual.y, expected.y, tolerance) &&
      Near(actual.z, expected.z, tolerance);
  }

  std::uint32_t FloatBits(float value)
  {
    std::uint32_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
  }

  bool ExactPose(Pose const& left, Pose const& right)
  {
    // Compare fields individually so padding cannot affect exact restoration.
    return FloatBits(left.position.x) == FloatBits(right.position.x) &&
      FloatBits(left.position.y) == FloatBits(right.position.y) &&
      FloatBits(left.position.z) == FloatBits(right.position.z) &&
      FloatBits(left.rotation.x) == FloatBits(right.rotation.x) &&
      FloatBits(left.rotation.y) == FloatBits(right.rotation.y) &&
      FloatBits(left.rotation.z) == FloatBits(right.rotation.z) &&
      FloatBits(left.rotation.w) == FloatBits(right.rotation.w) &&
      FloatBits(left.fovDegrees) == FloatBits(right.fovDegrees);
  }

  float Dot(Vec3 const& a, Vec3 const& b)
  {
    return a.x * b.x + a.y * b.y + a.z * b.z;
  }

  void RequireOrthonormal(Basis const& basis)
  {
    REQUIRE(Near(Dot(basis.right, basis.right), 1.f));
    REQUIRE(Near(Dot(basis.up, basis.up), 1.f));
    REQUIRE(Near(Dot(basis.forward, basis.forward), 1.f));
    REQUIRE(Near(Dot(basis.right, basis.up), 0.f));
    REQUIRE(Near(Dot(basis.right, basis.forward), 0.f));
    REQUIRE(Near(Dot(basis.up, basis.forward), 0.f));
  }

  Pose InitialPose()
  {
    Pose pose;
    pose.position = { 12.25f, -7.f, 100.5f };
    pose.fovDegrees = 43.f;
    return pose;
  }

  void IdentityMovesAndFov()
  {
    Session session;
    const Pose origin = InitialPose();
    REQUIRE(session.Start(1, true, false, origin) == 0);
    REQUIRE(session.IsActive());
    REQUIRE(session.Move(0.25f, -0.5f, 0.f, true));
    REQUIRE(NearVector(session.GetCurrent().position, { 12.5f, -7.5f, 100.5f }));
    REQUIRE(session.GetCurrent().fovDegrees == 43.f);
    REQUIRE(session.Move(-0.5f, 0.25f, 27.f, true));
    REQUIRE(NearVector(session.GetCurrent().position, { 11.75f, -6.75f, 100.5f }));
    REQUIRE(session.GetCurrent().fovDegrees == 27.f);
    REQUIRE(session.Move(0.f, 0.f, 0.f, true));
    REQUIRE(session.GetCurrent().fovDegrees == 27.f);
    REQUIRE(session.Move(0.f, 0.f, -1.f, true));
    REQUIRE(session.GetCurrent().fovDegrees == 27.f);
    REQUIRE(NearVector(session.GetCurrent().position, origin.position));
    Pose restored;
    REQUIRE(session.End(restored));
    REQUIRE(ExactPose(restored, origin));
  }

  void AbsoluteSamplesNeverAccumulate()
  {
    Session session;
    const Pose origin = InitialPose();
    REQUIRE(session.Start(1, true, false, origin) == 0);
    REQUIRE(session.Move(0.125f, -0.25f, 0.f, true));
    const Pose expected = session.GetCurrent();
    // Returning to the same sample must give exactly the same pose after an
    // arbitrarily long sampling sequence, not the sum of preceding samples.
    for (int i = 0; i < 10000; ++i)
    {
      REQUIRE(session.Move(-0.5f, 0.75f, 0.f, true));
      REQUIRE(session.Move(0.125f, -0.25f, 0.f, true));
      REQUIRE(ExactPose(session.GetCurrent(), expected));
    }
    REQUIRE(session.Move(0.f, 0.f, 0.f, true));
    REQUIRE(ExactPose(session.GetCurrent(), origin));
  }

  void RolledCameraUsesScreenAxes()
  {
    const float halfSqrtTwo = std::sqrt(0.5f);
    Pose origin = InitialPose();
    origin.rotation = { 0.f, 0.f, halfSqrtTwo, halfSqrtTwo }; // +90 degrees about Z.
    Session session;
    REQUIRE(session.Start(1, true, false, origin) == 0);
    REQUIRE(NearVector(session.GetBasis().right, { 0.f, 1.f, 0.f }));
    REQUIRE(NearVector(session.GetBasis().up, { -1.f, 0.f, 0.f }));
    REQUIRE(NearVector(session.GetBasis().forward, { 0.f, 0.f, 1.f }));
    REQUIRE(session.Move(2.f, 3.f, 0.f, true));
    REQUIRE(NearVector(session.GetCurrent().position, { 9.25f, -5.f, 100.5f }));
    RequireOrthonormal(session.GetBasis());
  }

  void RotatedCameraUsesDepthAxis()
  {
    const float halfSqrtTwo = std::sqrt(0.5f);
    Pose origin = InitialPose();
    origin.rotation = { 0.f, halfSqrtTwo, 0.f, halfSqrtTwo }; // +90 degrees about Y.
    Session session;
    REQUIRE(session.Start(1, true, false, origin) == 0);
    REQUIRE(NearVector(session.GetBasis().right, { 0.f, 0.f, -1.f }));
    REQUIRE(NearVector(session.GetBasis().up, { 0.f, 1.f, 0.f }));
    REQUIRE(session.Move(2.f, -3.f, 0.f, true));
    REQUIRE(NearVector(session.GetCurrent().position, { 12.25f, -10.f, 98.5f }));
    RequireOrthonormal(session.GetBasis());
  }

  void NonUnitQuaternionDoesNotScaleOffsets()
  {
    Pose origin = InitialPose();
    // This is a 120-degree turn around the (1,1,1) axis, scaled by 2.
    // It maps camera right to world Y and camera up to world Z.
    origin.rotation = { 1.f, 1.f, 1.f, 1.f };
    Session session;
    REQUIRE(session.Start(1, true, false, origin) == 0);
    REQUIRE(NearVector(session.GetBasis().right, { 0.f, 1.f, 0.f }));
    REQUIRE(NearVector(session.GetBasis().up, { 0.f, 0.f, 1.f }));
    REQUIRE(session.Move(2.f, 3.f, 0.f, true));
    REQUIRE(NearVector(session.GetCurrent().position, { 12.25f, -5.f, 103.5f }));
    RequireOrthonormal(session.GetBasis());
    Pose restored;
    REQUIRE(session.End(restored));
    REQUIRE(ExactPose(restored, origin)); // Original quaternion is preserved.
  }

  void RelativeMovesUseCurrentPosition()
  {
    Session session;
    Pose origin = InitialPose();
    origin.rotation = { 1.f, 1.f, 1.f, 1.f };
    REQUIRE(session.Start(1, true, false, origin) == 0);
    REQUIRE(session.Move(1.f, 2.f, 0.f, false));
    REQUIRE(session.Move(3.f, -1.f, 0.f, false));
    REQUIRE(NearVector(session.GetCurrent().position, { 12.25f, -3.f, 101.5f }));
    // Absolute mode still refers to the session origin after relative moves.
    REQUIRE(session.Move(0.f, 0.f, 0.f, true));
    REQUIRE(ExactPose(session.GetCurrent(), origin));
  }

  void StartRejectionsAreNonDestructive()
  {
    Session session;
    const Pose origin = InitialPose();
    REQUIRE(session.Start(0, true, false, origin) == 4); // Panorama unsupported.
    REQUIRE(session.Start(255, true, false, origin) == 4);
    REQUIRE(session.Start(1, false, false, origin) == 1);
    REQUIRE(session.Start(1, true, true, origin) == 2);
    REQUIRE(!session.IsActive());
    REQUIRE(!session.Move(1.f, 1.f, 0.f, true));
    REQUIRE(session.Start(1, true, false, origin) == 0);
    REQUIRE(session.Move(0.5f, -0.25f, 30.f, true));
    const Pose activePose = session.GetCurrent();
    REQUIRE(session.Start(1, true, false, Pose{}) == 3);
    REQUIRE(ExactPose(session.GetCurrent(), activePose));
    REQUIRE(ExactPose(session.GetOrigin(), origin));
  }

  void InvalidPosesAreRejected()
  {
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float infinity = std::numeric_limits<float>::infinity();
    const Pose good = InitialPose();
    Pose bad[] = { good, good, good, good, good, good, good, good };
    bad[0].position.x = nan;
    bad[1].position.y = infinity;
    bad[2].position.z = 10000001.f;
    bad[3].rotation = { 0.f, 0.f, 0.f, 0.f };
    bad[4].rotation.w = nan;
    bad[5].fovDegrees = infinity;
    bad[6].fovDegrees = 4.9f;
    bad[7].fovDegrees = 151.f;
    for (const Pose& pose : bad)
    {
      Session session;
      REQUIRE(session.Start(1, true, false, pose) == 5);
      REQUIRE(!session.IsActive());
      REQUIRE(session.Start(1, true, false, good) == 0);
    }
  }

  void InvalidSamplesLeavePoseUnchanged()
  {
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float infinity = std::numeric_limits<float>::infinity();
    Session session;
    REQUIRE(session.Start(1, true, false, InitialPose()) == 0);
    REQUIRE(session.Move(0.5f, -0.25f, 30.f, true));
    const Pose before = session.GetCurrent();
    const float samples[][3] = {
      { nan, 0.f, 0.f }, { 0.f, nan, 0.f }, { 0.f, 0.f, nan },
      { infinity, 0.f, 0.f }, { 0.f, -infinity, 0.f }, { 0.f, 0.f, infinity },
      { 100001.f, 0.f, 0.f }, { 0.f, -100001.f, 0.f },
      { 1.f, 1.f, 4.9f }, { 1.f, 1.f, 151.f }
    };
    for (const auto& sample : samples)
    {
      REQUIRE(!session.Move(sample[0], sample[1], sample[2], true));
      REQUIRE(ExactPose(session.GetCurrent(), before));
    }

    Pose limit = InitialPose();
    limit.position.x = 10000000.f;
    Session atLimit;
    REQUIRE(atLimit.Start(1, true, false, limit) == 0);
    REQUIRE(!atLimit.Move(10.f, 0.f, 20.f, true));
    REQUIRE(ExactPose(atLimit.GetCurrent(), limit));
  }

  void EndRestoresExactlyAndIsIdempotent()
  {
    Pose origin = InitialPose();
    origin.position.y = -0.f;
    origin.rotation = { 0.3f, -0.4f, 0.1f, 0.7f };
    origin.fovDegrees = 37.12345f;
    Session session;
    REQUIRE(session.Start(1, true, false, origin) == 0);
    REQUIRE(session.Move(10.f, -5.f, 8.f, true));
    Pose restored;
    REQUIRE(session.End(restored)); // Same exit path for completion and cancellation.
    REQUIRE(ExactPose(restored, origin));
    REQUIRE(ExactPose(session.GetCurrent(), origin));
    REQUIRE(!session.IsActive());
    REQUIRE(!session.Move(10.f, 10.f, 60.f, true));
    Pose untouched = InitialPose();
    const Pose before = untouched;
    REQUIRE(!session.End(untouched));
    REQUIRE(ExactPose(untouched, before));

    // ReShade can finish and start a new capture before the next game frame.
    REQUIRE(session.Start(1, true, false, restored) == 0);
    REQUIRE(ExactPose(session.GetOrigin(), origin));
    REQUIRE(session.Move(0.f, 0.f, 0.f, true));
    REQUIRE(NearVector(session.GetCurrent().position, origin.position));
    REQUIRE(session.GetCurrent().fovDegrees == origin.fovDegrees);
    REQUIRE(session.End(restored));
    REQUIRE(ExactPose(restored, origin));
  }

#ifdef _WIN32
  void CameraBasisMatchesDirectXMath()
  {
    // Check against the actual engine-facing math library as well as the
    // portable tests' independently known axis rotations.
    const Quaternion rotations[] = {
      { 0.f, 0.f, 0.f, 1.f },
      { 0.f, 0.f, 1.f, 1.f },
      { 0.f, 1.f, 0.f, 1.f },
      { 1.f, 1.f, 1.f, 1.f },
      { 0.3f, -0.4f, 0.1f, 0.7f },
      { -0.1f, 0.9f, -0.25f, -0.15f }
    };
    for (const Quaternion& rotation : rotations)
    {
      Basis basis;
      REQUIRE(igcs::capture::BuildBasis(rotation, basis));
      const DirectX::XMFLOAT4 input(rotation.x, rotation.y, rotation.z, rotation.w);
      const auto normalized = DirectX::XMQuaternionNormalize(DirectX::XMLoadFloat4(&input));
      DirectX::XMFLOAT4X4 matrix;
      DirectX::XMStoreFloat4x4(&matrix, DirectX::XMMatrixRotationQuaternion(normalized));
      REQUIRE(NearVector(basis.right, { matrix._11, matrix._12, matrix._13 }));
      REQUIRE(NearVector(basis.up, { matrix._21, matrix._22, matrix._23 }));
      REQUIRE(NearVector(basis.forward, { matrix._31, matrix._32, matrix._33 }));
      RequireOrthonormal(basis);
    }
  }
#endif
}

int main()
{
  struct Test { const char* name; void (*run)(); };
  const Test tests[] = {
    { "identity movement and FOV preservation", IdentityMovesAndFov },
    { "absolute sampling without accumulated drift", AbsoluteSamplesNeverAccumulate },
    { "rolled camera screen axes", RolledCameraUsesScreenAxes },
    { "rotated camera depth axis", RotatedCameraUsesDepthAxis },
    { "non-unit quaternion and exact restoration", NonUnitQuaternionDoesNotScaleOffsets },
    { "relative and absolute sampling", RelativeMovesUseCurrentPosition },
    { "session start rejection", StartRejectionsAreNonDestructive },
    { "invalid pose rejection", InvalidPosesAreRejected },
    { "invalid sample atomic rejection", InvalidSamplesLeavePoseUnchanged },
    { "end, cancel, repeated end and immediate restart", EndRestoresExactlyAndIsIdempotent },
#ifdef _WIN32
    { "camera basis agrees with DirectXMath", CameraBasisMatchesDirectXMath }
#endif
  };
  unsigned int passed = 0;
  for (const Test& test : tests)
  {
    try
    {
      test.run();
      std::printf("PASS: %s\n", test.name);
      ++passed;
    }
    catch (const std::exception& error)
    {
      std::fprintf(stderr, "FAIL: %s: %s\n", test.name, error.what());
      return 1;
    }
  }
  std::printf("%u capture tests passed; IGCS CameraData ABI assertions compiled (84 bytes).\n", passed);
  return 0;
}
