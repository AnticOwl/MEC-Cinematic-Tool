#include "IgcsBridge.h"
#include "IgcsCameraData.h"
#include "CameraManager.h"
#include "../Util/Util.h"

#include <Windows.h>
#include <Psapi.h>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#pragma comment(lib, "psapi.lib")

// Exact native entry points consumed by the x64 IGCS Connector. In particular,
// its screenshot-session result is an int, not a byte.
extern "C" __declspec(dllexport) int __cdecl IGCS_StartScreenshotSession(std::uint8_t type) noexcept;
extern "C" __declspec(dllexport) void __cdecl IGCS_EndScreenshotSession() noexcept;
extern "C" __declspec(dllexport) void __cdecl IGCS_MoveCameraMultishot(float leftRight, float upDown, float fovDegrees, bool fromStartPosition) noexcept;
extern "C" __declspec(dllexport) void __cdecl IGCS_MoveCameraPanorama(float stepAngle) noexcept;

namespace
{
  // Lock order is bridge -> CameraManager. CameraManager never calls back here.
  // Foreign addon functions and FreeLibrary are always called outside this lock.
  std::mutex g_lifecycleMutex;
  CameraManager* g_camera = nullptr;
  HMODULE g_addon = nullptr;                 // owned reference, not GetModuleHandle
  igcs::CameraData* g_sharedData = nullptr;  // owned by that referenced addon
  std::atomic<bool> g_connected{ false };
  std::uint64_t g_generation = 0;
  bool g_discovering = false;
  unsigned int g_attempts = 0;
  ULONGLONG g_nextAttempt = 0;
  const unsigned int MaxDiscoveryAttempts = 120;
  const ULONGLONG DiscoveryIntervalMs = 1000;

  enum class DiscoveryStatus
  {
    None,
    Connected,
    NoAddon,
    EnumerationFailed,
    ProviderConflict,
    ProviderMissing,
    HandshakeFailed,
    Exception
  };
  DiscoveryStatus g_lastStatus = DiscoveryStatus::None;

  // A scoped reference prevents an addon unload from invalidating a function or
  // its shared buffer. Its destructor must never run under g_lifecycleMutex.
  class ModuleReference
  {
  public:
    ModuleReference() = default;
    ~ModuleReference() { if (m_module) FreeLibrary(m_module); }
    ModuleReference(ModuleReference const&) = delete;
    ModuleReference& operator=(ModuleReference const&) = delete;

    ModuleReference(ModuleReference&& other) noexcept : m_module(other.Release()) {}
    ModuleReference& operator=(ModuleReference&& other) noexcept
    {
      if (this != &other)
      {
        if (m_module) FreeLibrary(m_module);
        m_module = other.Release();
      }
      return *this;
    }

    bool Acquire(HMODULE observed)
    {
      return GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
        reinterpret_cast<LPCWSTR>(observed), &m_module) != FALSE;
    }
    HMODULE Get() const { return m_module; }
    HMODULE Release() noexcept
    {
      HMODULE result = m_module;
      m_module = nullptr;
      return result;
    }

  private:
    HMODULE m_module = nullptr;
  };

  struct ConnectionCandidate
  {
    ModuleReference module;
    igcs::CameraData* data = nullptr;
    std::string name;
    DiscoveryStatus status = DiscoveryStatus::NoAddon;
  };

  std::string ModuleName(HMODULE module)
  {
    char name[MAX_PATH]{};
    if (GetModuleBaseNameA(GetCurrentProcess(), module, name, MAX_PATH))
    {
      name[MAX_PATH - 1] = '\0';
      return name;
    }
    return "unknown module";
  }

  bool EnumerateModules(std::vector<HMODULE>& modules)
  {
    modules.resize(128);
    // Modules can appear while enumerating. Retry growth without ever trusting
    // cbNeeded as an array index or allocating an unbounded module list.
    for (unsigned int retry = 0; retry < 4; ++retry)
    {
      DWORD bytesNeeded = 0;
      const DWORD capacity = static_cast<DWORD>(modules.size() * sizeof(HMODULE));
      if (!EnumProcessModules(GetCurrentProcess(), modules.data(), capacity, &bytesNeeded))
        return false;
      if (bytesNeeded <= capacity)
      {
        modules.resize(bytesNeeded / sizeof(HMODULE));
        return true;
      }
      const size_t required = (bytesNeeded + sizeof(HMODULE) - 1) / sizeof(HMODULE);
      // The currently supported official connector itself enumerates into a
      // 512-module array. Do not invoke its handshake with an oversized list.
      if (required > 512)
        return false;
      modules.resize(required);
    }
    return false;
  }

  ConnectionCandidate DiscoverConnection()
  {
    ConnectionCandidate candidate;
    std::vector<HMODULE> modules;
    if (!EnumerateModules(modules))
    {
      candidate.status = DiscoveryStatus::EnumerationFailed;
      return candidate;
    }

    using ConnectFunction = bool(__cdecl*)();
    using BufferFunction = unsigned char*(__cdecl*)();
    ConnectFunction connect = nullptr;
    BufferFunction buffer = nullptr;
    bool providerFound = false;
    const FARPROC ourStart = reinterpret_cast<FARPROC>(&IGCS_StartScreenshotSession);

    for (HMODULE observed : modules)
    {
      ModuleReference module;
      if (!module.Acquire(observed))
        continue; // unloaded since enumeration

      // The official addon selects the FIRST module exporting this symbol. A
      // second provider (for example an existing relay) can otherwise receive
      // commands while this camera misleadingly reports a working connection.
      if (!providerFound)
      {
        FARPROC start = GetProcAddress(module.Get(), "IGCS_StartScreenshotSession");
        if (start)
        {
          providerFound = true;
          if (start != ourStart)
          {
            candidate.status = DiscoveryStatus::ProviderConflict;
            candidate.name = ModuleName(module.Get());
            return candidate;
          }
        }
      }

      if (candidate.module.Get())
        continue;
      auto foundConnect = reinterpret_cast<ConnectFunction>(GetProcAddress(module.Get(), "connectFromCameraTools"));
      auto foundBuffer = reinterpret_cast<BufferFunction>(GetProcAddress(module.Get(), "getDataFromCameraToolsBuffer"));
      if (foundConnect && foundBuffer)
      {
        connect = foundConnect;
        buffer = foundBuffer;
        candidate.name = ModuleName(module.Get());
        candidate.module = std::move(module);
      }
    }

    if (!providerFound)
    {
      candidate.status = DiscoveryStatus::ProviderMissing;
      return candidate;
    }
    if (!candidate.module.Get())
      return candidate;

    // Neither lifecycle nor camera mutex is held here: the addon scans exports
    // and may call us back. The owned module reference keeps both calls valid.
    if (!connect())
    {
      candidate.status = DiscoveryStatus::HandshakeFailed;
      return candidate;
    }
    candidate.data = reinterpret_cast<igcs::CameraData*>(buffer());
    candidate.status = candidate.data ? DiscoveryStatus::Connected : DiscoveryStatus::HandshakeFailed;
    return candidate;
  }

  void ReportDiscovery(DiscoveryStatus status, const std::string& name, bool exhausted) noexcept
  {
    try
    {
      switch (status)
      {
      case DiscoveryStatus::Connected:
        util::log::Ok("IGCSDOF: connected to %s", name.c_str());
        break;
      case DiscoveryStatus::NoAddon:
        util::log::Write("IGCSDOF: waiting for IGCS Connector during startup (up to 120 seconds)");
        break;
      case DiscoveryStatus::ProviderConflict:
        util::log::Warning("IGCSDOF: %s is the first camera provider; remove the other provider/relay to use this camera's native connection", name.c_str());
        break;
      case DiscoveryStatus::ProviderMissing:
        util::log::Warning("IGCSDOF: this camera's exported interface was not found in the module list");
        break;
      case DiscoveryStatus::EnumerationFailed:
        util::log::Warning("IGCSDOF: unable to obtain a bounded module list");
        break;
      case DiscoveryStatus::HandshakeFailed:
        util::log::Warning("IGCSDOF: %s did not provide a valid camera data buffer", name.c_str());
        break;
      case DiscoveryStatus::Exception:
        util::log::Warning("IGCSDOF: connection attempt raised an exception");
        break;
      default:
        break;
      }
      if (exhausted)
        util::log::Write("IGCSDOF: startup discovery ended without a connection; the camera remains usable");
    }
    catch (...) {} // logging must not escape the bridge or an exported API
  }

  void ReportException() noexcept
  {
    OutputDebugStringA("MEC IGCSDOF: an exception was contained at the camera bridge boundary.\n");
  }
}

void igcs::Initialize(CameraManager* camera)
{
  std::lock_guard<std::mutex> lock(g_lifecycleMutex);
  if (!camera || g_camera)
    return;
  ++g_generation;
  g_camera = camera;
  g_camera->SetIgcsConnected(false);
  g_connected.store(false);
  g_discovering = false;
  g_attempts = 0;
  g_nextAttempt = 0;
  g_lastStatus = DiscoveryStatus::None;
}

void igcs::PollConnection()
{
  std::uint64_t generation = 0;
  {
    std::lock_guard<std::mutex> lock(g_lifecycleMutex);
    const ULONGLONG now = GetTickCount64();
    if (!g_camera || g_connected.load() || g_discovering ||
        g_attempts >= MaxDiscoveryAttempts || now < g_nextAttempt)
      return;
    generation = g_generation;
    g_discovering = true;
    ++g_attempts;
    g_nextAttempt = now + DiscoveryIntervalMs;
  }

  ConnectionCandidate candidate;
  try
  {
    candidate = DiscoverConnection();
  }
  catch (...)
  {
    candidate.status = DiscoveryStatus::Exception;
  }

  bool report = false;
  bool exhausted = false;
  {
    std::lock_guard<std::mutex> lock(g_lifecycleMutex);
    // Shutdown may have run while a foreign function was being called. Never
    // publish to a replaced/destroyed manager, or resurrect a stopped bridge.
    if (generation != g_generation || !g_camera)
      return;
    g_discovering = false;
    if (candidate.status == DiscoveryStatus::Connected)
    {
      g_addon = candidate.module.Release();
      g_sharedData = candidate.data;
      g_camera->SetIgcsConnected(true);
      g_connected.store(true);
    }
    report = candidate.status != g_lastStatus;
    g_lastStatus = candidate.status;
    exhausted = !g_connected.load() && g_attempts >= MaxDiscoveryAttempts;
  }
  if (report || exhausted)
    ReportDiscovery(report ? candidate.status : DiscoveryStatus::None, candidate.name, exhausted);
  // Any unaccepted candidate releases its module here, outside the lock.
}

void igcs::PublishCameraData()
{
  try
  {
    std::lock_guard<std::mutex> lock(g_lifecycleMutex);
    if (!g_camera || !g_sharedData || !g_connected.load())
      return;
    const CameraData data = g_camera->GetIgcsCameraData();
    std::memcpy(g_sharedData, &data, sizeof(data));
  }
  catch (...)
  {
    ReportException();
  }
}

bool igcs::IsConnected()
{
  return g_connected.load();
}

void igcs::Shutdown()
{
  HMODULE addonToRelease = nullptr;
  {
    std::lock_guard<std::mutex> lock(g_lifecycleMutex);
    ++g_generation;
    CameraData finalData{};
    if (g_camera)
    {
      try
      {
        g_camera->EndIgcsSession();
        finalData = g_camera->GetIgcsCameraData();
      }
      catch (...) { ReportException(); }
      g_camera->SetIgcsConnected(false);
    }
    // The retained module reference makes this buffer valid until after these
    // writes. The addon has no disconnect API; live DLL hot-unload is not offered.
    if (g_sharedData)
    {
      finalData.cameraEnabled = 0;
      finalData.cameraMovementLocked = 0;
      std::memcpy(g_sharedData, &finalData, sizeof(finalData));
    }
    g_sharedData = nullptr;
    g_camera = nullptr;
    addonToRelease = g_addon;
    g_addon = nullptr;
    g_connected.store(false);
    g_discovering = false;
  }
  if (addonToRelease)
    FreeLibrary(addonToRelease);
}

extern "C" __declspec(dllexport) int __cdecl IGCS_StartScreenshotSession(std::uint8_t type) noexcept
{
  try
  {
    std::lock_guard<std::mutex> lock(g_lifecycleMutex);
    if (!g_camera || !g_connected.load())
      return 4; // camera feature unavailable before initialization/after shutdown
    return g_camera->StartIgcsSession(type);
  }
  catch (...)
  {
    ReportException();
    return 5;
  }
}

extern "C" __declspec(dllexport) void __cdecl IGCS_EndScreenshotSession() noexcept
{
  try
  {
    std::lock_guard<std::mutex> lock(g_lifecycleMutex);
    if (g_camera)
      g_camera->EndIgcsSession();
  }
  catch (...)
  {
    ReportException();
  }
}

extern "C" __declspec(dllexport) void __cdecl IGCS_MoveCameraMultishot(float leftRight, float upDown, float fovDegrees, bool fromStartPosition) noexcept
{
  try
  {
    std::lock_guard<std::mutex> lock(g_lifecycleMutex);
    if (g_camera && g_connected.load())
      g_camera->MoveIgcsCamera(leftRight, upDown, fovDegrees, fromStartPosition);
  }
  catch (...)
  {
    ReportException();
  }
}

extern "C" __declspec(dllexport) void __cdecl IGCS_MoveCameraPanorama(float) noexcept
{
  // Required discovery symbol. Start(type=0) returns unavailable: this adapter
  // implements the multishot interface used by IGCSDOF, not panorama rotation.
}
