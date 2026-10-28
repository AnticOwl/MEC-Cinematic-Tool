#pragma once

class CameraManager;

namespace igcs
{
  // Call after CameraManager construction, outside DllMain/loader lock.
  void Initialize(CameraManager* camera);

  // Called from the tools worker loop. Discovery is rate limited and bounded;
  // a successful connection is retained without repeated handshake calls.
  void PollConnection();

  // Called from the game's main Present, even while the tools menu is closed.
  void PublishCameraData();

  bool IsConnected();

  // Call before destroying CameraManager, outside DllMain/loader lock.
  // Restores any capture session and disables the addon's camera data.
  void Shutdown();
}
