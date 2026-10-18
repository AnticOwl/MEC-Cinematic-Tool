#pragma once
#include <atomic>
#include <mutex>
#include <string>
#include <vector>
#include <Windows.h>

// Adds sphere and spot lights to the scene, like the original tools'
// Light Manager. The original builds the game's light entity structures in
// its own memory (with the game's vtables) and hands them to the game's
// UpdateLight function.
class LightManager
{
public:
  enum LightType { Sphere = 0, Spot = 1 };

  struct Light
  {
    int Id;
    LightType Type;
    BYTE* pEntity;
    BYTE* pData;
    bool Removed;
  };

  LightManager();
  ~LightManager();

  // Game thread, from the camera update hook. Light changes are sent to
  // the game from here, once per frame.
  void OnGameUpdate();
  // Render thread, from the Present hook
  void DrawUI();

private:
  bool CheckSupport();
  void CreateLight(LightType type);
  void RemoveLight(Light& light);
  void MoveToCamera(Light& light);
  bool GetCameraTransform(float transform[16]);
  void QueueUpdate(Light& light);

private:
  std::mutex m_Mutex;
  std::vector<Light> m_Lights;
  std::vector<BYTE*> m_PendingUpdates;
  std::atomic<bool> m_HasPending{ false };
  int m_NextId;
  int m_Selected;

  bool m_Supported;
  std::string m_UnsupportedReason;

  __int64 m_pUpdateLight;
  __int64 m_ppGameRenderer;

public:
  LightManager(LightManager const&) = delete;
  void operator=(LightManager const&) = delete;
};
