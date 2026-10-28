#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

// Binary interface of Frans Bouma's IGCS Connector, CameraToolsData.h:
// https://github.com/FransBouma/IgcsConnector/blob/e0130adce9093e3d00ed6181cd212c6b9d54c2c2/src/CameraToolsData.h
// This declaration uses plain scalar arrays; no DirectX SIMD alignment may be
// introduced here. The receiving addon owns the memory containing this data.
namespace igcs
{
  struct CameraData
  {
    std::uint8_t cameraEnabled;
    std::uint8_t cameraMovementLocked;
    std::uint8_t reserved1;
    std::uint8_t reserved2;
    float fov;                              // degrees
    float coordinates[3];                   // x, y, z
    float lookQuaternion[4];                // x, y, z, w
    float rotationMatrixUpVector[3];
    float rotationMatrixRightVector[3];
    float rotationMatrixForwardVector[3];
    float pitch;                            // radians
    float yaw;
    float roll;
  };

  static_assert(std::is_standard_layout<CameraData>::value, "IGCS data must have a standard layout");
  static_assert(std::is_trivially_copyable<CameraData>::value, "IGCS data must be trivially copyable");
  static_assert(sizeof(float) == 4, "IGCS requires 32-bit floats");
  static_assert(sizeof(CameraData) == 84, "IGCS data size must match the addon ABI");
  static_assert(alignof(CameraData) == 4, "IGCS data must not use SIMD alignment");
  static_assert(offsetof(CameraData, cameraEnabled) == 0, "IGCS flag offset mismatch");
  static_assert(offsetof(CameraData, cameraMovementLocked) == 1, "IGCS flag offset mismatch");
  static_assert(offsetof(CameraData, reserved1) == 2, "IGCS reserved offset mismatch");
  static_assert(offsetof(CameraData, reserved2) == 3, "IGCS reserved offset mismatch");
  static_assert(offsetof(CameraData, fov) == 4, "IGCS FOV offset mismatch");
  static_assert(offsetof(CameraData, coordinates) == 8, "IGCS position offset mismatch");
  static_assert(offsetof(CameraData, lookQuaternion) == 20, "IGCS quaternion offset mismatch");
  static_assert(offsetof(CameraData, rotationMatrixUpVector) == 36, "IGCS up offset mismatch");
  static_assert(offsetof(CameraData, rotationMatrixRightVector) == 48, "IGCS right offset mismatch");
  static_assert(offsetof(CameraData, rotationMatrixForwardVector) == 60, "IGCS forward offset mismatch");
  static_assert(offsetof(CameraData, pitch) == 72, "IGCS pitch offset mismatch");
  static_assert(offsetof(CameraData, yaw) == 76, "IGCS yaw offset mismatch");
  static_assert(offsetof(CameraData, roll) == 80, "IGCS roll offset mismatch");
}
