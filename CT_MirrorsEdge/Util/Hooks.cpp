#include "Util.h"
#include "../Main.h"
#include "../Visuals/GreenPlane.h"
#include "../Visuals/Weather.h"
#include "../Visuals/WetLook.h"
#include "../Visuals/SnowBodies.h"
#include "../Camera/ReplayGameDraws.h"
#include "../Camera/PoseEditor.h"
#include "Perf.h"

#include <MinHook.h>
#include <algorithm>
#include <atomic>
#include <cfloat>
#include <climits>
#include <cmath>
#include <d3d11.h>
#include <DirectXMath.h>
#include <intrin.h>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <Windows.h>
#include <Psapi.h>

#pragma comment(lib, "d3d11.lib")

// Function definitions
typedef HRESULT(WINAPI* tIDXGISwapChain_Present)(IDXGISwapChain*, UINT, UINT);

// Neither of these game functions is fully known, the original tools' detours
// only touched rcx before calling the original. Forward all four integer
// argument registers so nothing gets lost.
typedef __int64(__fastcall* tCameraUpdate)(__int64, __int64, __int64, __int64);
typedef __int64(__fastcall* tEntryInput)(__int64, __int64, __int64, __int64);
typedef __int64(__fastcall* tVisualEnvironmentUpdate)(__int64, __int64, __int64, __int64);
typedef __int64(__fastcall* tZoneStreamerUpdate)(__int64, __int64, __int64, __int64);
// The zone streamer's sorter: (sorter, its foci's positions, how many foci, the
// zones waiting to load, how many), orders the zones in place
typedef void(__fastcall* tZoneLoadOrder)(__int64, float const*, unsigned int, unsigned short*, int);
// The sound listener's setters: (listener, 4 rows / a float4, 16-byte aligned)
typedef void(__fastcall* tListenerSet)(__int64, float const*);
// A vista's show / hide for one of its zones: (vista, zone, show)
typedef void(__fastcall* tVistaShowZone)(__int64, unsigned int, bool);
// A model group's delayed job gathering one entity's mesh: (job, record, out, value)
typedef bool(__fastcall* tMeshGather)(__int64, __int64, __int64, float);

typedef BOOL(WINAPI* tSetCursorPos)(int, int);
typedef BOOL(WINAPI* tClipCursor)(const RECT*);
typedef HCURSOR(WINAPI* tSetCursor)(HCURSOR);

//////////////////////////
////   RENDER HOOKS   ////
//////////////////////////

// Most games today run on DirectX 11. This hook is called just before
// the image is actually drawn on screen so we can draw more stuff
// on top, like the tools UI.

// Present reaches the tools two ways: the game's own present function (see
// hGamePresent), and the jump MinHook puts at the start of dxgi's Present.
// Another mod hooking Present after the tools can leave the second out of
// the way (Catalyst Clarity has); when both are on the way of one present,
// the tools only take it once.
namespace
{
  thread_local bool t_InPresent = false;
  std::atomic<unsigned int> g_PresentCallsFromJump{ 0 };
  std::atomic<unsigned int> g_PresentCallsFromGame{ 0 };

  HRESULT PresentThroughTools(tIDXGISwapChain_Present original, IDXGISwapChain* pSwapchain, UINT SyncInterval, UINT Flags)
  {
    if (t_InPresent)
      return original(pSwapchain, SyncInterval, Flags);

    t_InPresent = true;
    if (!g_shutdown)
      g_mainHandle->OnPresent(pSwapchain);

    // (how long the Present itself takes: MISC > Performance's frame line)
    long long presentStart = util::DiagnosticsOn() ? perf::Now() : 0;
    HRESULT result = original(pSwapchain, SyncInterval, Flags);
    if (util::DiagnosticsOn())   // CT.log's frame line only
      perf::AddPresentTicks(perf::Now() - presentStart);
    t_InPresent = false;
    return result;
  }
}

tIDXGISwapChain_Present oIDXGISwapChain_Present = nullptr;
HRESULT WINAPI hIDXGISwapChain_Present(IDXGISwapChain* pSwapchain, UINT SyncInterval, UINT Flags)
{
  g_PresentCallsFromJump++;
  return PresentThroughTools(oIDXGISwapChain_Present, pSwapchain, SyncInterval, Flags);
}

// The game's present function (0x1436136E0; rcx its renderer, rdx the screen
// whose swapchain is at + 0x280): it works out the sync interval and flags,
// presents nothing when [renderer + 0x820] + 0x6C is set, and calls Present
// through the swapchain's table (0x143613767), with nothing drawn in
// between. The tools draw there, right before: game code no other mod
// changes, so no hook another mod puts on Present can leave the tools out.
// The tools used to put their own Present in dxgi's swapchain table for that
// (a table every swapchain of its kind shares); with MECMP, which makes a
// swapchain of its own at start and takes that entry as its original, the
// Steam overlay's Present hook then called 0 (2026-10-23, three times, one
// frame after MECMP hooked: gameoverlayrenderer64.dll + 0x9C20F, slot 0 of
// the stack, under Catalyst Clarity's and the tools' hooks; MECMP and Clarity
// without the tools didn't crash). Nothing is written in that table any more.
// The game's other present (0x143619C0E, a loop over its screens) only
// reaches the tools through the jump in dxgi.
typedef __int64(__fastcall* tGamePresent)(__int64, __int64, __int64, __int64);
tGamePresent oGamePresent = nullptr;
const __int64 GamePresentRva = 0x36136E0;

static IDXGISwapChain* GamePresentSwapChain(__int64 pRenderer, __int64 pScreen)
{
  __try
  {
    __int64 pSettings = *(__int64*)(pRenderer + 0x820);
    if (pSettings && *(BYTE*)(pSettings + 0x6C))
      return nullptr;   // it presents nothing this time
    return *(IDXGISwapChain**)(pScreen + 0x280);
  }
  __except (EXCEPTION_EXECUTE_HANDLER)
  {
    return nullptr;
  }
}

__int64 __fastcall hGamePresent(__int64 pRenderer, __int64 pScreen, __int64 r8, __int64 r9)
{
  g_PresentCallsFromGame++;
  IDXGISwapChain* pSwapchain = GamePresentSwapChain(pRenderer, pScreen);
  if (!pSwapchain || t_InPresent)
    return oGamePresent(pRenderer, pScreen, r8, r9);

  t_InPresent = true;
  if (!g_shutdown)
    g_mainHandle->OnPresent(pSwapchain);
  // (how long the Present itself takes, see PresentThroughTools)
  long long presentStart = util::DiagnosticsOn() ? perf::Now() : 0;
  __int64 result = oGamePresent(pRenderer, pScreen, r8, r9);
  if (util::DiagnosticsOn())   // CT.log's frame line only
    perf::AddPresentTicks(perf::Now() - presentStart);
  t_InPresent = false;
  return result;
}

static bool IsGamePresentCode(BYTE const* pBase)
{
  // mov rax, rsp; push rdi; sub rsp, 0x260; mov qword ptr [rsp + 0x20], -2 ... and at + 0x7B: mov rcx, [rbx + 0x280];
  // mov rax, [rcx]; mov edx, esi; call [rax + 0x40]
  static const BYTE start[] = { 0x48, 0x89, 0xE0, 0x57, 0x48, 0x81, 0xEC, 0x60, 0x02, 0x00, 0x00, 0x48, 0xC7, 0x44, 0x24, 0x20,
    0xFE, 0xFF, 0xFF, 0xFF };
  static const BYTE call[] = { 0x48, 0x8B, 0x8B, 0x80, 0x02, 0x00, 0x00, 0x48, 0x8B, 0x01, 0x89, 0xF2, 0xFF, 0x50, 0x40 };
  return !memcmp(pBase + GamePresentRva, start, sizeof(start)) && !memcmp(pBase + GamePresentRva + 0x7B, call, sizeof(call));
}

//////////////////////////
////   CAMERA HOOKS   ////
//////////////////////////

// Mirror's Edge Catalyst: OFFSET_CAMERAUPDATE is called once per frame per
// render view. After the game has written its own camera, the free camera
// transform is written over the main view (GameRenderer + 0x58).

// The game's drops and water on the lens left out while the tools' camera is
// on (see hEmitterViewCull): set here every camera update from the free
// camera's state, read by the game's job workers; how many emitter draws
// were left out since the last log line
static void SetFlaresHidden(bool hidden);   // (the lens flares', below)
std::atomic<bool> g_LensHidden{ false };
std::atomic<unsigned> g_LensLeftOut{ 0 };
ULONGLONG g_LensLoggedAt = 0;

tCameraUpdate oCameraUpdate = nullptr;
__int64 __fastcall hCameraUpdate(__int64 pRenderView, __int64 a2, __int64 a3, __int64 a4)
{
  __int64 result = oCameraUpdate(pRenderView, a2, a3, a4);

  bool lensHidden = false;
  if (!g_shutdown)
  {
    lensHidden = g_mainHandle->GetCameraManager()->IsCameraEnabled();
    // (every 10 s while some were left out: how many; diagnostics only)
    ULONGLONG now = GetTickCount64();
    if (util::DiagnosticsOn() && now - g_LensLoggedAt >= 10000)
    {
      unsigned leftOut = g_LensLeftOut.exchange(0, std::memory_order_relaxed);
      if (leftOut)
        util::log::Write("Free camera: the game's drops and water on the lens left out of the picture (%u emitter draws "
          "in the last 10 s)", leftOut);
      g_LensLoggedAt = now;
    }
    perf::Scope timing(perf::SectionCameraHook);
    g_mainHandle->GetCameraManager()->OnCameraUpdate(pRenderView);
    // (the camera hook's parts, see perf::LapStart: the lights' its own, it's
    // updated for every render view)
    static const char* const s_lightsLap = "the lights (every render view)";
    long long lightsStart = util::DiagnosticsOn() ? perf::Now() : 0;
    g_mainHandle->GetLightManager()->OnGameUpdate();
    if (util::DiagnosticsOn())   // CT.log's camera hook line only
      perf::AddLap(s_lightsLap, perf::Now() - lightsStart);
    // GAME > Pose: the main view for the bones' dots, after the free camera is written (as the lights' dots)
    pose_editor::OnViewWritten();
  }
  g_LensHidden.store(lensHidden, std::memory_order_relaxed);
  SetFlaresHidden(lensHidden);

  return result;
}

// OFFSET_ZONESTREAMERUPDATE is ZoneStreamerEntity's update: it asks each of
// its streaming foci where it is and loads the city zones around them. The
// free camera's focus is added or removed right before, on the same thread
// as the game's own focus changes.

tZoneStreamerUpdate oZoneStreamerUpdate = nullptr;
__int64 __fastcall hZoneStreamerUpdate(__int64 pZoneStreamer, __int64 a2, __int64 a3, __int64 a4)
{
  if (!g_shutdown)
    g_mainHandle->GetCameraManager()->OnZoneStreamerUpdate(pZoneStreamer);

  return oZoneStreamerUpdate(pZoneStreamer, a2, a3, a4);
}

// OFFSET_ZONELOADORDER orders the zones a streamer is about to load (nearest
// the game's own camera first). While the free camera has the world loaded
// around it, the tools order them by what it looks at instead.
tZoneLoadOrder oZoneLoadOrder = nullptr;
void __fastcall hZoneLoadOrder(__int64 pSorter, float const* pCache, unsigned int focusCount, unsigned short* pZones, int count)
{
  if (!g_shutdown && g_mainHandle->GetCameraManager()->OnZoneLoadOrder(pSorter, pCache, pZones, count))
    return;

  oZoneLoadOrder(pSorter, pCache, focusCount, pZones, count);
}

// OFFSET_VISTASHOWZONE shows or hides the objects of one zone of a vista (the
// city's buildings seen from far away). A zone the tools hold back stays
// hidden when the game shows it; the game's own count of what it shows goes
// on as before.
tVistaShowZone oVistaShowZone = nullptr;
void __fastcall hVistaShowZone(__int64 pVista, unsigned int zone, bool show)
{
  if (show && !g_shutdown && g_mainHandle->GetCameraManager()->IsVistaZoneHeldBack(pVista, zone))
    show = false;
  oVistaShowZone(pVista, zone, show);
}

bool util::hooks::ShowVistaZone(__int64 pVista, unsigned int zone, bool show)
{
  if (!oVistaShowZone)
    return false;
  oVistaShowZone(pVista, zone, show);
  return true;
}

// OFFSET_LISTENERTRANSFORM / OFFSET_LISTENERVELOCITY: the game puts the sound
// listener where its own camera is, every frame (0x1432C1350). While the free
// camera is heard from, the listener gets the free camera's transform instead,
// and a velocity from how the free camera moves: the game's own would be from
// where the listener was put (the free camera) to its camera, the jump between
// them, and pitch everything up or down.
tListenerSet oListenerSetTransform = nullptr;
void __fastcall hListenerSetTransform(__int64 pListener, float const* pTransform)
{
  alignas(16) float transform[16];
  if (!g_shutdown && pTransform && g_mainHandle->GetCameraManager()->GetListenerTransform(pTransform, transform))
    pTransform = transform;
  oListenerSetTransform(pListener, pTransform);
}

tListenerSet oListenerSetVelocity = nullptr;
void __fastcall hListenerSetVelocity(__int64 pListener, float const* pVelocity)
{
  alignas(16) float velocity[4];
  if (!g_shutdown && pVelocity && g_mainHandle->GetCameraManager()->GetListenerVelocity(velocity))
    pVelocity = velocity;
  oListenerSetVelocity(pListener, pVelocity);
}

// Diagnostic (temporary, for the replay's sounds): while it records, every
// voice the sound system makes (0x143019860, its slot 8's target, with the
// sound asset, whose name is at +0x10), where each is placed (the voice's
// slot 13, 0x14301DC30), and every event a voice is given (its slot 0,
// 0x14301D5F0: (voice, event id); a SoundEntity hands its voice each of its
// own events, 0x14301A274), with what called it and how far from Faith the
// voice was. A SoundEntity (event handler 0x14301A170, its slot 4) keeps its
// voice at +0x88 once its first event made it, so a voice made before the log
// gets its name from the entity's data (+0x28, its Sound asset at +0x18).
// Only reads. Written to the log when it ends.
typedef __int64(__fastcall* tSoundCreate)(__int64, __int64, __int64, __int64);
typedef __int64(__fastcall* tSoundEntityEvent)(__int64, __int64);
tSoundEntityEvent oSoundEntityEvent = nullptr;
typedef void(__fastcall* tVoiceSetTransform)(__int64, float const*);
typedef char(__fastcall* tVoiceSetAsset)(__int64, unsigned int, __int64);
tVoiceSetAsset oVoiceSetAsset = nullptr;
typedef __int64(__fastcall* tVoiceEvent)(__int64, unsigned int);
tSoundCreate oSoundCreate = nullptr;
tVoiceSetTransform oVoiceSetTransform = nullptr;
tVoiceEvent oVoiceEvent = nullptr;

namespace
{
  const __int64 SoundCreateRva = 0x3019860;
  const __int64 VoiceTransformRva = 0x301DC30;
  const __int64 VoiceEventRva = 0x301D5F0;
  const __int64 VoiceTableRva = 0x1ACFAD0;   // slot 0 the event, slot 13 the transform
  const __int64 SoundEntityEventRva = 0x301A170;
  const __int64 SoundEntityTableRva = 0x1AD0338; // slot 4 the event handler
  const int SoundEntityDataOffset = 0x28;
  const int SoundEntityVoiceOffset = 0x88;
  const int SoundEntityDataSoundOffset = 0x18;
  const int VoicePositionalOffset = 0x28;    // 3D state, null for a voice heard the same anywhere
  const int SoundLogFrames = 6;
  const int SoundLogVoiceBits = 15;          // 32768 voices remembered
  const int SoundLogProbe = 64;              // places looked at for a voice at most
  const int SoundLogVoices = 1 << SoundLogVoiceBits;
  const int SoundLogKinds = 1024;
  const float SoundLogNearFaith = 3.f;       // metres from her feet

  struct SoundLogVoice
  {
    __int64 Voice;
    char Name[96];
    float Position[3];
    int Placed;          // transforms given
    bool Positional;
    int MadeKind;        // 1 + its place in g_SoundLogMadeKinds, 0 none
  };
  // One per sound, event and call chain
  struct SoundLogKind
  {
    char Name[96];
    unsigned int Event;
    void* Stack[SoundLogFrames];
    int Frames;
    int Count;
    int NearCount;
    float MinDistance;
    float MaxDistance;
    float MinCamera;     // from the free camera (when it's on)
    float MaxCamera;
    int MaxPlaced;
    bool Positional;
  };
  SoundLogVoice g_SoundLogVoices[SoundLogVoices];
  SoundLogKind g_SoundLogKinds[SoundLogKinds];
  int g_SoundLogKindCount = 0;
  int g_SoundLogKindsDropped = 0;
  int g_SoundLogVoicesMade = 0;
  int g_SoundLogEvents = 0;
  float g_SoundLogFaith[3]{};
  bool g_SoundLogHasFaith = false;
  float g_SoundLogCamera[3]{};
  bool g_SoundLogHasCamera = false;
  // The events sound entities get (by sound, event and sender), and every
  // sound entity there is (made 0x1430170C0, gone 0x143017BA0: the game keeps
  // no list of them), so one can be found before its first event
  const int SoundLogEntityKinds = 512;
  SoundLogKind g_SoundLogEntityKinds[SoundLogEntityKinds];
  int g_SoundLogEntityKindCount = 0;
  // The voices made, by sound and caller (their Event is 0), with where each
  // was first put
  SoundLogKind g_SoundLogMadeKinds[SoundLogEntityKinds];
  int g_SoundLogMadeKindCount = 0;
  const int SoundLogEntityBits = 15;          // 32768 sound entities at most
  const int SoundLogEntitySlots = 1 << SoundLogEntityBits;
  const __int64 SoundLogEntityGone = 1;
  const int SoundEntityPositionOffset = 0xE0; // its transform's position (rows from +0xB0)
  __int64 g_SoundLogEntities[SoundLogEntitySlots];
  int g_SoundLogEntityCount = 0;
  std::atomic_flag g_SoundLogLock = ATOMIC_FLAG_INIT;
  std::atomic<bool> g_SoundLogOn{ false };
  std::atomic<bool> g_SoundLogDone{ false };
  double g_SoundLogUntil = 0;
  double g_SoundLogSeconds = 0;

  double SoundLogNow()
  {
    static LARGE_INTEGER frequency{};
    if (!frequency.QuadPart)
      QueryPerformanceFrequency(&frequency);
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return (double)now.QuadPart / (double)frequency.QuadPart;
  }

  void LockSoundLog() { while (g_SoundLogLock.test_and_set(std::memory_order_acquire)) {} }
  void UnlockSoundLog() { g_SoundLogLock.clear(std::memory_order_release); }

  // Under the lock: the voice's place in the table, or a free one (null
  // with no room left)
  SoundLogVoice* FindSoundLogVoice(__int64 pVoice, bool add)
  {
    unsigned __int64 h = ((unsigned __int64)pVoice >> 4) * 0x9E3779B97F4A7C15ull >> (64 - SoundLogVoiceBits);
    for (int i = 0; i < SoundLogProbe; ++i)
    {
      SoundLogVoice& v = g_SoundLogVoices[(h + i) & (SoundLogVoices - 1)];
      if (v.Voice == pVoice)
        return &v;
      if (!v.Voice)
      {
        if (!add)
          return nullptr;
        v.Voice = pVoice;
        return &v;
      }
    }
    return nullptr;
  }

  void ReadSoundLogName(__int64 pAsset, char* buffer, size_t size)
  {
    buffer[0] = 0;
    __try
    {
      const char* name = pAsset ? *(const char**)(pAsset + 0x10) : nullptr;
      size_t i = 0;
      for (; name && i + 1 < size && name[i]; ++i)
        buffer[i] = name[i] >= 32 && name[i] < 127 ? name[i] : '?';
      buffer[i] = 0;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      buffer[0] = 0;
    }
  }

  // Sounds the game plays for the player: Faith's own (her body, her damage
  // and breath, her gadgets: the MagRope's alerts, hacking), the bullets
  // flying by her, the security cameras' city alert. Many of them are heard
  // the same everywhere (the HUD's sounds), so they're told by name, not by
  // place: their voices are noted as they're made (hSoundCreate), and while a
  // replay plays their live events (the real Faith's) are left out (see
  // ReplaySoundOnEvent). A voice made later at the same place for another
  // sound isn't one.
  std::atomic<int> g_LivePlayerEventsLeftOut{ 0 };
  std::mutex g_PlayerVoiceMutex;
  std::unordered_set<__int64> g_PlayerVoices;   // under g_PlayerVoiceMutex
  const size_t MaxPlayerVoices = 8192;

  bool IsPlayerSoundName(const char* name)
  {
    static const char* const prefixes[] = { "ME_Sound/Character/Faith/", "ME_Sound/Gadgets/", "Sound/Weapons/Whizz_Bys/",
      "ME_Sound/Weapons/Flybys/", "ME_Sound/GameplayObjects/SecurityCamera/ME_Sec_Camera_City_Alert" };
    for (const char* prefix : prefixes)
      if (!_strnicmp(name, prefix, strlen(prefix)))
        return true;
    return false;
  }

  // From hSoundCreate, any thread
  void NotePlayerVoice(__int64 pVoice, __int64 pAsset)
  {
    char name[96];
    ReadSoundLogName(pAsset, name, sizeof(name));
    bool player = IsPlayerSoundName(name);
    std::lock_guard<std::mutex> lock(g_PlayerVoiceMutex);
    if (!player)
      g_PlayerVoices.erase(pVoice);
    else if (g_PlayerVoices.size() < MaxPlayerVoices)
      g_PlayerVoices.insert(pVoice);
  }

  bool IsPlayerVoice(__int64 pVoice)
  {
    std::lock_guard<std::mutex> lock(g_PlayerVoiceMutex);
    return g_PlayerVoices.count(pVoice) != 0;
  }

  bool IsVoicePositional(__int64 pVoice)
  {
    __try { return *(__int64*)(pVoice + VoicePositionalOffset) != 0; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
  }

  // Under the lock: the live sound entities, by pointer
  void AddSoundLogEntity(__int64 pEntity)
  {
    unsigned __int64 h = ((unsigned __int64)pEntity >> 4) * 0x9E3779B97F4A7C15ull >> (64 - SoundLogEntityBits);
    for (int i = 0; i < SoundLogProbe; ++i)
    {
      __int64& slot = g_SoundLogEntities[(h + i) & (SoundLogEntitySlots - 1)];
      if (slot == pEntity)
        return;
      if (!slot || slot == SoundLogEntityGone)
      {
        slot = pEntity;
        ++g_SoundLogEntityCount;
        return;
      }
    }
  }

  void RemoveSoundLogEntity(__int64 pEntity)
  {
    unsigned __int64 h = ((unsigned __int64)pEntity >> 4) * 0x9E3779B97F4A7C15ull >> (64 - SoundLogEntityBits);
    for (int i = 0; i < SoundLogProbe; ++i)
    {
      __int64& slot = g_SoundLogEntities[(h + i) & (SoundLogEntitySlots - 1)];
      if (!slot)
        return;
      if (slot == pEntity)
      {
        slot = SoundLogEntityGone;
        --g_SoundLogEntityCount;
        return;
      }
    }
  }

  // A sound entity's sound, its voice, where it is, and an event's id
  bool ReadSoundEntityInfo(__int64 pEntity, __int64 pEvent, __int64& pAsset, __int64& pVoice, float* position, unsigned int& event)
  {
    __try
    {
      __int64 pData = *(__int64*)(pEntity + SoundEntityDataOffset);
      pAsset = pData ? *(__int64*)(pData + SoundEntityDataSoundOffset) : 0;
      pVoice = *(__int64*)(pEntity + SoundEntityVoiceOffset);
      memcpy(position, (void*)(pEntity + SoundEntityPositionOffset), sizeof(float) * 3);
      event = pEvent ? *(unsigned int*)(pEvent + 8) : 0;
      return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      return false;
    }
  }

  float SoundLogDistance(float const* a, float const* b)
  {
    float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
    return sqrtf(dx * dx + dy * dy + dz * dz);
  }
}

// Test (Start spot sounds near the camera): the level starts its spot sounds
// (the club's music, crowd and traffic spots: sound entities whose sound is
// under Music/Diegetic/, Crowds/ME_Crowd_Spot or World/Shared/Traffic/) only
// for Faith, with the event 0x0DC0EFA5, and stops them with 0x7C89BB7D, from
// its event queue (sound logs, 2026-10-23). They're noted as the game makes
// them. Where one is is read the way the game places its voice (0x143021540):
// none if + 0xF1 is set; its own transform if + 0xE0 isn't 0; else (no link
// at + 0xA0) its property at + 0x90 (getter 0x142A424B0: the link's value
// [[+ 0x90]] if the link's + 0x1C has 0x40, else [+ 0x98]) times its owner's
// (0x14314DF80([+ 0x20]), which only reads), unless the property's w is
// 0xAFAFAFAF. Failing that, its voice's 3D state (voice + 0x28, the position
// at + 0x30; slot 13 0x14301DC30 copies the four rows there), known once it
// has played.
// While it's on and the free camera is near one that the game isn't playing,
// the tools give it the start event (a SoundEntity reads only the id, at
// + 8), and the stop event once the camera is away again; one the game starts
// itself meanwhile is the game's again.
namespace
{
  const unsigned int SpotStartEvent = 0x0DC0EFA5;
  const unsigned int SpotStopEvent = 0x7C89BB7D;
  const float SpotStartRadius = 40.f;
  const float SpotMusicStartRadius = 80.f;
  const float SpotStopMargin = 15.f;
  const int MaxSpotSounds = 512;
  const int VoiceStateOffset = 0x28;       // the 3D state; its transform's position at + 0x30

  struct SpotSound
  {
    __int64 Entity;
    char Name[96];
    float Position[3];
    bool HasPosition;
    bool Music;
    bool GamePlaying;    // the game's last event for it was the start
    bool Ours;           // started by the tools
  };
  SpotSound g_SpotSounds[MaxSpotSounds];
  int g_SpotSoundCount = 0;
  // The thread the game gives spot sounds their events on (0 none yet), and
  // the one it makes and removes them on (mixed: more than one). The tools
  // give theirs only on that thread, outside the spot lock, so an entity
  // can't be removed while it's given one.
  std::atomic<DWORD> g_SpotGameThread{ 0 };
  std::atomic<DWORD> g_SpotLifeThread{ 0 };
  std::atomic<bool> g_SpotLifeMixed{ false };

  void NoteSpotLifeThread()
  {
    DWORD me = GetCurrentThreadId();
    DWORD expected = 0;
    if (!g_SpotLifeThread.compare_exchange_strong(expected, me) && expected != me && !g_SpotLifeMixed.exchange(true))
      util::log::Warning("Spot sounds: the game makes or removes them on threads %u and %u: none are started", expected, me);
  }

  typedef __int64(__fastcall* tOwnerTransform)(__int64, float*);
  const __int64 OwnerTransformRva = 0x314DF80;
  const __int64 SoundPlacementRva = 0x3021540;
  const __int64 PropertyGetterRva = 0x2A424B0;

  // The placement as it was read (see above), checked once
  bool IsSpotPlacementCode()
  {
    static int s_checked = -1;
    if (s_checked < 0)
    {
      BYTE const* pBase = (BYTE const*)g_gameHandle;
      static const BYTE owner[] = { 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0x49, 0x10, 0x48, 0x89, 0xD3, 0xE8 };
      static const BYTE skip[] = { 0x80, 0xB9, 0xF1, 0x00, 0x00, 0x00, 0x00 };
      static const BYTE link[] = { 0x48, 0x8B, 0x89, 0xA0, 0x00, 0x00, 0x00 };
      static const BYTE prop[] = { 0x48, 0x8D, 0x8F, 0x90, 0x00, 0x00, 0x00 };
      static const BYTE unset[] = { 0x81, 0xBD, 0xD0, 0x00, 0x00, 0x00, 0xAF, 0xAF, 0xAF, 0xAF, 0x0F, 0x84, 0x2D, 0x01, 0x00, 0x00,
        0x48, 0x8B, 0x4F, 0x20 };
      static const BYTE getter[] = { 0x48, 0x8B, 0x01, 0x48, 0x85, 0xC0, 0x74, 0x0E, 0xF6, 0x40, 0x1C, 0x40, 0x74, 0x08, 0x48, 0x8B,
        0x00, 0xC3 };
      bool ok = false;
      __try
      {
        ok = !memcmp(pBase + OwnerTransformRva, owner, sizeof(owner)) && !memcmp(pBase + SoundPlacementRva + 0x18, skip, sizeof(skip)) &&
          !memcmp(pBase + SoundPlacementRva + 0xC5, link, sizeof(link)) && !memcmp(pBase + SoundPlacementRva + 0xD5, prop, sizeof(prop)) &&
          !memcmp(pBase + SoundPlacementRva + 0x12C, unset, sizeof(unset)) && !memcmp(pBase + PropertyGetterRva, getter, sizeof(getter)) &&
          pBase[PropertyGetterRva + 0x16] == 0x48 && pBase[PropertyGetterRva + 0x17] == 0x8B && pBase[PropertyGetterRva + 0x18] == 0x41 &&
          pBase[PropertyGetterRva + 0x19] == 0x08;
      }
      __except (EXCEPTION_EXECUTE_HANDLER)
      {
        ok = false;
      }
      s_checked = ok ? 1 : 0;
      if (!ok)
        util::log::Warning("Spot sounds: the sound placement isn't the code the tools know, only spot sounds that have played are found");
    }
    return s_checked == 1;
  }

  // Where the game would put its voice (see above)
  bool ReadSpotPlacement(__int64 pEntity, float* position)
  {
    if (!IsSpotPlacementCode())
      return false;
    __try
    {
      if (*(BYTE const*)(pEntity + 0xF1))
        return false;
      float const* own = (float const*)(pEntity + 0xE0);
      if (own[0] != 0.f || own[1] != 0.f || own[2] != 0.f)
      {
        memcpy(position, own, sizeof(float) * 3);
        return true;
      }
      if (*(__int64 const*)(pEntity + 0xA0))
        return false; // placed by a link, not read
      __int64 pLink = *(__int64 const*)(pEntity + 0x90);
      float const* m = (pLink && (*(BYTE const*)(pLink + 0x1C) & 0x40)) ? *(float const**)pLink : *(float const**)(pEntity + 0x98);
      if (!m)
        return false;
      if (*(unsigned int const*)(m + 15) == 0xAFAFAFAF)
      {
        memcpy(position, m + 12, sizeof(float) * 3);
        return position[0] != 0.f || position[1] != 0.f || position[2] != 0.f;
      }
      __int64 pOwner = *(__int64 const*)(pEntity + 0x20);
      if (!pOwner)
        return false;
      alignas(16) float o[16];
      ((tOwnerTransform)((__int64)g_gameHandle + OwnerTransformRva))(pOwner, o);
      for (int c = 0; c < 3; ++c)
        position[c] = m[12] * o[c] + m[13] * o[4 + c] + m[14] * o[8 + c] + o[12 + c];
      return position[0] != 0.f || position[1] != 0.f || position[2] != 0.f;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      return false;
    }
  }
  // Spin lock the same thread can take again (the game's handler could make
  // or remove a sound entity while the tools give one an event)
  std::atomic<DWORD> g_SpotOwner{ 0 };
  int g_SpotDepth = 0;

  void LockSpots()
  {
    DWORD me = GetCurrentThreadId();
    if (g_SpotOwner.load(std::memory_order_acquire) == me)
    {
      ++g_SpotDepth;
      return;
    }
    DWORD expected = 0;
    while (!g_SpotOwner.compare_exchange_weak(expected, me, std::memory_order_acquire))
    {
      expected = 0;
      YieldProcessor();
    }
    g_SpotDepth = 1;
  }

  void UnlockSpots()
  {
    if (--g_SpotDepth == 0)
      g_SpotOwner.store(0, std::memory_order_release);
  }

  __int64 ReadDataAsset(__int64 pData)
  {
    __try { return pData ? *(__int64*)(pData + SoundEntityDataSoundOffset) : 0; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
  }

  unsigned int ReadEventId(__int64 pEvent)
  {
    __try { return pEvent ? *(unsigned int*)(pEvent + 8) : 0; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
  }

  // Where its voice is, if it has one that has been placed
  bool ReadSpotPosition(__int64 pEntity, float* position)
  {
    __try
    {
      __int64 pVoice = *(__int64*)(pEntity + SoundEntityVoiceOffset);
      __int64 pState = pVoice ? *(__int64*)(pVoice + VoiceStateOffset) : 0;
      if (!pState)
        return false;
      memcpy(position, (void*)(pState + 0x30), sizeof(float) * 3);
      return position[0] != 0.f || position[1] != 0.f || position[2] != 0.f;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      return false;
    }
  }

  // Under the spot lock
  int FindSpotSound(__int64 pEntity)
  {
    for (int i = 0; i < g_SpotSoundCount; ++i)
      if (g_SpotSounds[i].Entity == pEntity)
        return i;
    return -1;
  }

  void NoteSpotSound(__int64 pEntity, __int64 pData)
  {
    __int64 pAsset = ReadDataAsset(pData);
    if (!pAsset)
      return;
    char name[96];
    ReadSoundLogName(pAsset, name, sizeof(name));
    bool music = strstr(name, "/Music/Diegetic/") != nullptr;
    if (!music && !strstr(name, "/Crowds/ME_Crowd_Spot") && !strstr(name, "/World/Shared/Traffic/"))
      return;
    NoteSpotLifeThread();
    LockSpots();
    if (FindSpotSound(pEntity) < 0 && g_SpotSoundCount < MaxSpotSounds)
    {
      SpotSound& spot = g_SpotSounds[g_SpotSoundCount++];
      spot = SpotSound{};
      spot.Entity = pEntity;
      memcpy(spot.Name, name, sizeof(name));
      spot.Music = music;
    }
    UnlockSpots();
  }

  void ForgetSpotSound(__int64 pEntity)
  {
    LockSpots();
    int i = FindSpotSound(pEntity);
    if (i >= 0)
      g_SpotSounds[i] = g_SpotSounds[--g_SpotSoundCount];
    UnlockSpots();
    if (i >= 0)
      NoteSpotLifeThread();
  }

  // From the SoundEntity hook: the game's own start and stop
  void NoteSpotEvent(__int64 pEntity, __int64 pEvent)
  {
    if (!g_SpotSoundCount)
      return;
    unsigned int event = ReadEventId(pEvent);
    if (event != SpotStartEvent && event != SpotStopEvent)
      return;
    LockSpots();
    int i = FindSpotSound(pEntity);
    if (i >= 0)
    {
      g_SpotGameThread.store(GetCurrentThreadId(), std::memory_order_relaxed);
      g_SpotSounds[i].GamePlaying = event == SpotStartEvent;
      if (event == SpotStopEvent)
        g_SpotSounds[i].Ours = false; // the game stopped it: given again while the camera is near
    }
    UnlockSpots();
  }
}

typedef __int64(__fastcall* tSoundEntityCtor)(__int64, __int64, __int64, __int64);
typedef __int64(__fastcall* tSoundEntityDtor)(__int64, __int64);
tSoundEntityCtor oSoundEntityCtor = nullptr;
tSoundEntityDtor oSoundEntityDtor = nullptr;

__int64 __fastcall hSoundEntityCtor(__int64 pEntity, __int64 a2, __int64 pData, __int64 a4)
{
  __int64 result = oSoundEntityCtor(pEntity, a2, pData, a4);
  if (util::DiagnosticsOn())   // the sound log's table only
  {
    LockSoundLog();
    AddSoundLogEntity(pEntity);
    UnlockSoundLog();
  }
  NoteSpotSound(pEntity, pData);
  return result;
}

__int64 __fastcall hSoundEntityDtor(__int64 pEntity, __int64 a2)
{
  if (util::DiagnosticsOn())   // the sound log's table only
  {
    LockSoundLog();
    RemoveSoundLogEntity(pEntity);
    UnlockSoundLog();
  }
  ForgetSpotSound(pEntity);
  return oSoundEntityDtor(pEntity, a2);
}

void util::hooks::UpdateSpotSounds(bool on, float const* camera)
{
  if (!oSoundEntityEvent || !oSoundEntityCtor)
    return;
  struct Event { __int64 Unused; unsigned int Id; unsigned int Pad; };
  static bool s_wasOn = false;
  bool wasOn = s_wasOn;
  s_wasOn = on;
  // Nothing of the tools' is playing while it's been off
  if (!on && !wasOn)
    return;
  // Only on the thread the game gives them theirs on and makes and removes
  // them on
  DWORD me = GetCurrentThreadId();
  DWORD gameThread = g_SpotGameThread.load(std::memory_order_relaxed);
  DWORD lifeThread = g_SpotLifeThread.load(std::memory_order_relaxed);
  bool sameThread = (!gameThread || gameThread == me) && lifeThread == me && !g_SpotLifeMixed.load(std::memory_order_relaxed);
  static bool s_threadWarned = false;
  if (on && !sameThread && !s_threadWarned && lifeThread)
  {
    s_threadWarned = true;
    util::log::Warning("Spot sounds: the game gives them their events on thread %u and makes them on %u, the camera runs on %u: "
      "none are started", gameThread, lifeThread, me);
  }
  struct Send { __int64 Entity; unsigned int Id; char Name[96]; float Position[3]; float Distance; };
  const int MaxSends = 16; // a frame
  Send sends[MaxSends];
  int sendCount = 0;
  int placed = 0;
  LockSpots();
  for (int i = 0; i < g_SpotSoundCount; ++i)
  {
    SpotSound& spot = g_SpotSounds[i];
    // Spot sounds don't move: read once, from the placement or its voice
    float position[3];
    if (!spot.HasPosition && (ReadSpotPlacement(spot.Entity, position) || ReadSpotPosition(spot.Entity, position)))
    {
      memcpy(spot.Position, position, sizeof(position));
      spot.HasPosition = true;
    }
    if (!spot.HasPosition)
      continue;
    ++placed;
    if (!sameThread)
      continue;
    float radius = spot.Music ? SpotMusicStartRadius : SpotStartRadius;
    float distance = camera ? SoundLogDistance(spot.Position, camera) : 1e9f;
    unsigned int send = 0;
    if (spot.Ours && spot.GamePlaying)
      spot.Ours = false; // the game has it now
    else if (on && camera && distance <= radius && !spot.GamePlaying && !spot.Ours)
      send = SpotStartEvent;
    else if (spot.Ours && (!on || !camera || distance > radius + SpotStopMargin))
      send = SpotStopEvent;
    if (!send || sendCount == MaxSends)
      continue; // the rest next frame
    spot.Ours = send == SpotStartEvent;
    Send& out = sends[sendCount++];
    out.Entity = spot.Entity;
    out.Id = send;
    memcpy(out.Name, spot.Name, sizeof(out.Name));
    memcpy(out.Position, spot.Position, sizeof(out.Position));
    out.Distance = camera ? distance : -1.f;
  }
  int count = g_SpotSoundCount;
  UnlockSpots();

  for (int i = 0; i < sendCount; ++i)
  {
    // Still there (it can only be removed on this thread)
    LockSpots();
    bool there = FindSpotSound(sends[i].Entity) >= 0;
    UnlockSpots();
    if (!there)
      continue;
    Event event{ 0, sends[i].Id, 0 };
    oSoundEntityEvent(sends[i].Entity, (__int64)&event);
    // (a Distance below 0: no camera given, the tools' camera is off, so it's
    // a stop and there's no distance to tell. The buffer holds even FLT_MAX's
    // 39 digits, and the text is cut, not failed, if it ever didn't: sprintf_s
    // would end the game through the CRT's invalid-parameter handler, and a
    // stop has no upper bound on the distance)
    char away[64];
    if (sends[i].Distance >= 0.f)
      _snprintf_s(away, _TRUNCATE, "the camera %.0f m away", sends[i].Distance);
    else
      strcpy_s(away, "the camera off");
    util::log::Write("Spot sounds: %s %s at %.1f %.1f %.1f (%s)", sends[i].Id == SpotStartEvent ? "started" : "stopped",
      sends[i].Name, sends[i].Position[0], sends[i].Position[1], sends[i].Position[2], away);
  }
  if (on && !wasOn)
    util::log::Write("Spot sounds: on, %d spot sounds in the level, %d of them with a place", count, placed);
}

// Diagnostic (temporary): the world log, for syncing the replay with the
// world (Research_Replay_Sync_2026-10-23, outside the repo). Doors and
// elevators are moved by the level's TimelineEntity (TypeInfo 0x14283BEB0,
// vtable 0x141BEAB28): constructor 0x14368D590 (entity, info, data),
// destructor body 0x14368DC60 (its subclasses' too); its time at + 0x98, its
// data at + 0x28 (StartTime + 0x34, EndTime + 0x38, PlaybackRate + 0x40,
// AutoPlay + 0x4A, Looping + 0x51, Infinite + 0x52, TimelineData + 0x20 whose
// Children + 0x28 are its tracks), ExternalTime linked when + 0x60 isn't 0.
// The MagRope's line is a PamClientMagropeRenderEntity (vtable 0x141C410F8):
// constructor 0x14391DCD0, deleting destructor 0x14391F2F0 (its slot 7).
// While the log runs (from the menu), every frame reads each timeline's time
// and the line entities' first 0x200 bytes; at the end it writes which
// timelines moved (when, where Faith was, their data and tracks) and which of
// the line's fields changed. It only reads.
typedef __int64(__fastcall* tWorldEntityCtor)(__int64, __int64, __int64, __int64);
typedef __int64(__fastcall* tWorldEntityDtor)(__int64);
typedef __int64(__fastcall* tWorldEntityDeletingDtor)(__int64, __int64);
tWorldEntityCtor oTimelineCtor = nullptr;
tWorldEntityDtor oTimelineDtor = nullptr;
tWorldEntityCtor oRopeCtor = nullptr;
tWorldEntityDeletingDtor oRopeDtor = nullptr;
const __int64 TimelineCtorRva = 0x368D590;
const __int64 TimelineDtorRva = 0x368DC60;
const __int64 TimelineVTableRva = 0x1BEAB28;
const __int64 RopeCtorRva = 0x391DCD0;
const __int64 RopeDtorRva = 0x391F2F0;
const __int64 RopeVTableRva = 0x1C410F8;

namespace
{
  const int TimelineTimeOffset = 0x98;
  const int TimelineExternalTimeOffset = 0x60;
  const int EntityDataOffset = 0x28;
  const int WorldTimelineBits = 16;
  const int WorldTimelineSlots = 1 << WorldTimelineBits;
  const int WorldTimelineProbe = 64;
  const __int64 WorldSlotGone = 1;
  const int MaxWorldRecords = 4096;
  const int MaxWorldRopes = 32;
  const int RopeWatchedBytes = 0x300;
  const int RopeWatchedSlots = RopeWatchedBytes / 4;

  SRWLOCK g_WorldLock = SRWLOCK_INIT;
  // The live timelines, by pointer (all the time), and per slot while the
  // log runs: the time last frame (NaN: not read yet) and its record
  __int64 g_WorldTimelines[WorldTimelineSlots];
  int g_WorldTimelineCount = 0;
  int g_WorldTimelinesMade = 0;
  float g_WorldTimelinePrev[WorldTimelineSlots];
  short g_WorldTimelineRecord[WorldTimelineSlots];

  struct WorldTimelineRecord
  {
    __int64 Entity;
    __int64 Data;
    float First, Last, Min, Max;
    int Changes;            // frames its time changed
    int Starts;             // times it began to change
    double FirstChange, LastChange;
    double StartAt[3];
    float FaithAtStart[3][3];
    bool Moving;            // changed last frame
    bool Gone;              // removed during the log
    bool External;          // ExternalTime linked
    char Class[40];         // the timeline entity's class
  };
  WorldTimelineRecord g_WorldRecords[MaxWorldRecords];
  int g_WorldRecordCount = 0;
  int g_WorldRecordsDropped = 0;

  // The MagRope line entities (all the time), and while the log runs their
  // bytes last frame and what changed
  struct RopeSlotStats
  {
    int Changes;
    double FirstChange;
    unsigned int First, Last;
    float Min, Max;
  };
  __int64 g_WorldRopes[MaxWorldRopes];
  unsigned int g_WorldRopePrev[MaxWorldRopes][RopeWatchedSlots];
  bool g_WorldRopeRead[MaxWorldRopes];
  RopeSlotStats g_WorldRopeStats[MaxWorldRopes][RopeWatchedSlots];
  // While a line entity's fields change: its two ends (+ 0xA0, + 0xB0), +
  // 0x58 and + 0x90 (set during the swing in the first log) and where Faith
  // is, 10 times a second
  struct RopeSample
  {
    float T;
    int Rope;
    float A[3], B[3];
    unsigned int F58, F90;
    float Faith[3];
  };
  const int MaxRopeSamples = 600;
  RopeSample g_WorldRopeSamples[MaxRopeSamples];
  int g_WorldRopeSampleCount = 0;
  double g_WorldRopeLastSample[MaxWorldRopes];
  char g_WorldLabel[32] = "";

  std::atomic<bool> g_WorldLogOn{ false };
  std::atomic<bool> g_WorldLogDone{ false };
  double g_WorldLogStart = 0, g_WorldLogUntil = 0;
  float g_WorldFaith[3];
  bool g_WorldHasFaith = false;

  double WorldLogTime() { return SoundLogNow() - g_WorldLogStart; }

  int WorldTimelineSlot(__int64 pEntity, bool add)
  {
    unsigned __int64 h = ((unsigned __int64)pEntity >> 4) * 0x9E3779B97F4A7C15ull >> (64 - WorldTimelineBits);
    for (int i = 0; i < WorldTimelineProbe; ++i)
    {
      int s = (int)((h + i) & (WorldTimelineSlots - 1));
      __int64 slot = g_WorldTimelines[s];
      if (slot == pEntity)
        return s;
      if (!slot && !add)
        return -1;
      if (add && (!slot || slot == WorldSlotGone))
        return s;
    }
    return -1;
  }

  bool ReadWorldFloat(__int64 address, float& value)
  {
    __try { value = *(float const*)address; return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
  }

  bool ReadWorldBytes(__int64 address, void* out, size_t size)
  {
    __try { memcpy(out, (void const*)address, size); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
  }

  // An object's class name (vtable [0] is getType: lea rax, [rip + x]; ret;
  // TypeInfo + 0 its info, info + 0 its name)
  void WorldClassName(__int64 object, char* buffer, size_t size)
  {
    buffer[0] = 0;
    __try
    {
      __int64 vtable = *(__int64 const*)object;
      BYTE const* code = vtable ? *(BYTE const**)vtable : nullptr;
      if (!code || code[0] != 0x48 || code[1] != 0x8D || code[2] != 0x05 || code[7] != 0xC3)
        return;
      __int64 typeInfo = (__int64)code + 7 + *(int const*)(code + 3);
      __int64 info = *(__int64 const*)typeInfo;
      char const* name = info ? *(char const**)info : nullptr;
      if (name)
        strncpy_s(buffer, size, name, _TRUNCATE);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      buffer[0] = 0;
    }
  }

  // A timeline's tracks by class: "TransformPropertyTrackData x2, EventTrackData"
  void WorldTimelineTracks(__int64 pData, char* buffer, size_t size)
  {
    buffer[0] = 0;
    __int64 tracks[32];
    int count = 0;
    __try
    {
      __int64 pTimeline = *(__int64 const*)(pData + 0x20);
      __int64 pChildren = pTimeline ? *(__int64 const*)(pTimeline + 0x28) : 0;
      unsigned int n = pChildren ? *(unsigned int const*)(pChildren - 4) : 0;
      for (unsigned int i = 0; i < n && count < 32; ++i)
        tracks[count++] = *(__int64 const*)(pChildren + 8 * i);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
    char names[32][64];
    int counts[32];
    int kinds = 0;
    for (int i = 0; i < count; ++i)
    {
      char name[64];
      WorldClassName(tracks[i], name, sizeof(name));
      int k = 0;
      while (k < kinds && strcmp(names[k], name))
        ++k;
      if (k == kinds)
      {
        strcpy_s(names[kinds], name[0] ? name : "?");
        counts[kinds++] = 0;
      }
      ++counts[k];
    }
    size_t len = 0;
    for (int k = 0; k < kinds && len + 80 < size; ++k)
      len += sprintf_s(buffer + len, size - len, counts[k] > 1 ? "%s%s x%d" : "%s%s", k ? ", " : "", names[k], counts[k]);
    if (!count)
      strcpy_s(buffer, size, "none read");
  }

  void ResetWorldRopeStats(int r)
  {
    g_WorldRopeRead[r] = false;
    g_WorldRopeLastSample[r] = -1.0;
    for (int i = 0; i < RopeWatchedSlots; ++i)
      g_WorldRopeStats[r][i] = RopeSlotStats{ 0, 0.0, 0, 0, 1e30f, -1e30f };
  }
}

__int64 __fastcall hTimelineCtor(__int64 pEntity, __int64 a2, __int64 pData, __int64 a4)
{
  __int64 result = oTimelineCtor(pEntity, a2, pData, a4);
  AcquireSRWLockExclusive(&g_WorldLock);
  int s = WorldTimelineSlot(pEntity, true);
  if (s >= 0 && g_WorldTimelines[s] != pEntity)
  {
    g_WorldTimelines[s] = pEntity;
    g_WorldTimelinePrev[s] = NAN;
    g_WorldTimelineRecord[s] = -1;
    ++g_WorldTimelineCount;
    ++g_WorldTimelinesMade;
  }
  ReleaseSRWLockExclusive(&g_WorldLock);
  return result;
}

__int64 __fastcall hTimelineDtor(__int64 pEntity)
{
  AcquireSRWLockExclusive(&g_WorldLock);
  int s = WorldTimelineSlot(pEntity, false);
  if (s >= 0)
  {
    short r = g_WorldTimelineRecord[s];
    if (r >= 0)
      g_WorldRecords[r].Gone = true;
    g_WorldTimelines[s] = WorldSlotGone;
    g_WorldTimelineRecord[s] = -1;
    --g_WorldTimelineCount;
  }
  ReleaseSRWLockExclusive(&g_WorldLock);
  return oTimelineDtor(pEntity);
}

__int64 __fastcall hRopeCtor(__int64 pEntity, __int64 a2, __int64 pData, __int64 a4)
{
  __int64 result = oRopeCtor(pEntity, a2, pData, a4);
  AcquireSRWLockExclusive(&g_WorldLock);
  int r = 0;
  while (r < MaxWorldRopes && g_WorldRopes[r])
    ++r;
  if (r < MaxWorldRopes)
  {
    g_WorldRopes[r] = pEntity;
    if (util::DiagnosticsOn())   // the world log's stats only
      ResetWorldRopeStats(r);
  }
  bool logging = g_WorldLogOn.load();
  double t = WorldLogTime();
  ReleaseSRWLockExclusive(&g_WorldLock);
  if (logging)
    util::log::Write("World log: %.2f s: the MagRope's line entity 0x%I64X made (thread %u)", t, pEntity, GetCurrentThreadId());
  return result;
}

namespace
{
  void ForgetReplayRope(__int64 line);   // the replay's MagRope line (see hMagropeUpdate)
}

__int64 __fastcall hRopeDtor(__int64 pEntity, __int64 flags)
{
  ForgetReplayRope(pEntity);
  AcquireSRWLockExclusive(&g_WorldLock);
  for (int r = 0; r < MaxWorldRopes; ++r)
    if (g_WorldRopes[r] == pEntity)
      g_WorldRopes[r] = 0;
  bool logging = g_WorldLogOn.load();
  double t = WorldLogTime();
  ReleaseSRWLockExclusive(&g_WorldLock);
  if (logging)
    util::log::Write("World log: %.2f s: the MagRope's line entity 0x%I64X removed", t, pEntity);
  return oRopeDtor(pEntity, flags);
}

// The four functions as they were read, each putting its class's table in
// place (lea rax, [rip -> the vtable]) or, for the line's deleting
// destructor, calling its destructor body
static bool IsWorldLogCode(BYTE const* pBase)
{
  static const BYTE timelineCtor[] = { 0x48, 0x89, 0xE0, 0x48, 0x89, 0x48, 0x08, 0x56, 0x57, 0x41, 0x56, 0x48, 0x83, 0xEC, 0x70, 0x48,
    0xC7, 0x40, 0xA8, 0xFE, 0xFF, 0xFF, 0xFF, 0x48, 0x89, 0x58, 0x18, 0x48, 0x89, 0x68, 0x20, 0x4C, 0x89, 0xC7, 0x48, 0x89, 0xD3, 0x48,
    0x89, 0xCE, 0xE8 };
  static const BYTE timelineDtor[] = { 0x48, 0x89, 0x4C, 0x24, 0x08, 0x56, 0x57, 0x41, 0x56, 0x48, 0x83, 0xEC, 0x30, 0x48, 0xC7, 0x44,
    0x24, 0x20, 0xFE, 0xFF, 0xFF, 0xFF, 0x48, 0x89, 0x5C, 0x24, 0x58, 0x48, 0x89, 0x6C, 0x24, 0x60, 0x48, 0x89, 0xCF };
  static const BYTE ropeCtor[] = { 0x48, 0x89, 0x4C, 0x24, 0x08, 0x53, 0x55, 0x56, 0x57, 0x41, 0x56, 0x48, 0x83, 0xEC, 0x60, 0x48, 0xC7,
    0x44, 0x24, 0x30, 0xFE, 0xFF, 0xFF, 0xFF, 0x4C, 0x89, 0xC3, 0x48, 0x89, 0xD7, 0x48, 0x89, 0xCE, 0xE8 };
  static const BYTE ropeDtor[] = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x89, 0xD3, 0x48, 0x89, 0xCF, 0xE8 };
  auto leaTo = [&](__int64 rva, __int64 target) {
    BYTE const* p = pBase + rva;
    return p[0] == 0x48 && p[1] == 0x8D && p[2] == 0x05 && rva + 7 + *(int const*)(p + 3) == target;
  };
  auto callTo = [&](__int64 rva, __int64 target) {
    BYTE const* p = pBase + rva;
    return p[0] == 0xE8 && rva + 5 + *(int const*)(p + 1) == target;
  };
  return !memcmp(pBase + TimelineCtorRva, timelineCtor, sizeof(timelineCtor)) && leaTo(TimelineCtorRva + 0x2E, TimelineVTableRva) &&
    !memcmp(pBase + TimelineDtorRva, timelineDtor, sizeof(timelineDtor)) && leaTo(TimelineDtorRva + 0x23, TimelineVTableRva) &&
    !memcmp(pBase + RopeCtorRva, ropeCtor, sizeof(ropeCtor)) && leaTo(RopeCtorRva + 0x27, RopeVTableRva) &&
    !memcmp(pBase + RopeDtorRva, ropeDtor, sizeof(ropeDtor)) && callTo(RopeDtorRva + 0xF, 0x391ED30) &&
    *(__int64 const*)(pBase + RopeVTableRva + 7 * 8) == (__int64)pBase + RopeDtorRva;
}

bool util::hooks::StartWorldLog(double seconds, const char* label)
{
  if (!oTimelineCtor || !oRopeCtor)
    return false;
  AcquireSRWLockExclusive(&g_WorldLock);
  strncpy_s(g_WorldLabel, label ? label : "", _TRUNCATE);
  g_WorldRopeSampleCount = 0;
  for (int s = 0; s < WorldTimelineSlots; ++s)
  {
    g_WorldTimelinePrev[s] = NAN;
    g_WorldTimelineRecord[s] = -1;
  }
  g_WorldRecordCount = 0;
  g_WorldRecordsDropped = 0;
  for (int r = 0; r < MaxWorldRopes; ++r)
    ResetWorldRopeStats(r);
  g_WorldLogStart = SoundLogNow();
  g_WorldLogUntil = g_WorldLogStart + seconds;
  int alive = g_WorldTimelineCount, made = g_WorldTimelinesMade, ropes = 0;
  for (int r = 0; r < MaxWorldRopes; ++r)
    ropes += g_WorldRopes[r] != 0;
  ReleaseSRWLockExclusive(&g_WorldLock);
  g_WorldLogDone = false;
  g_WorldLogOn = true;
  util::log::Write("World log \"%s\": recording for %.0f s: %d timelines alive (%d made this session), %d MagRope line entities",
    g_WorldLabel, seconds, alive, made, ropes);
  return true;
}

double util::hooks::WorldLogRemaining()
{
  if (!g_WorldLogOn)
    return 0.0;
  double left = g_WorldLogUntil - SoundLogNow();
  return left > 0 ? left : 0.0;
}

void util::hooks::UpdateWorldLog(float const* faith)
{
  if (!g_WorldLogOn)
    return;
  bool ending = SoundLogNow() >= g_WorldLogUntil;
  double t = WorldLogTime();
  AcquireSRWLockExclusive(&g_WorldLock);
  g_WorldHasFaith = faith != nullptr;
  if (faith)
    memcpy(g_WorldFaith, faith, sizeof(g_WorldFaith));

  // The timelines' times
  for (int s = 0; s < WorldTimelineSlots; ++s)
  {
    __int64 e = g_WorldTimelines[s];
    if (e <= WorldSlotGone)
      continue;
    float time;
    if (!ReadWorldFloat(e + TimelineTimeOffset, time))
      continue;
    float prev = g_WorldTimelinePrev[s];
    g_WorldTimelinePrev[s] = time;
    if (std::isnan(prev))
      continue;
    short& index = g_WorldTimelineRecord[s];
    if (time == prev)
    {
      if (index >= 0)
        g_WorldRecords[index].Moving = false;
      continue;
    }
    if (index < 0)
    {
      if (g_WorldRecordCount >= MaxWorldRecords)
      {
        ++g_WorldRecordsDropped;
        continue;
      }
      index = (short)g_WorldRecordCount++;
      WorldTimelineRecord& r = g_WorldRecords[index];
      r = WorldTimelineRecord{};
      r.Entity = e;
      __int64 data = 0, external = 0;
      ReadWorldBytes(e + EntityDataOffset, &data, sizeof(data));
      ReadWorldBytes(e + TimelineExternalTimeOffset, &external, sizeof(external));
      r.Data = data;
      r.External = external != 0;
      r.First = r.Min = r.Max = prev;
      r.FirstChange = t;
      WorldClassName(e, r.Class, sizeof(r.Class));
    }
    WorldTimelineRecord& r = g_WorldRecords[index];
    ++r.Changes;
    r.Last = time;
    if (time < r.Min) r.Min = time;
    if (time > r.Max) r.Max = time;
    r.LastChange = t;
    if (!r.Moving)
    {
      if (r.Starts < 3)
      {
        r.StartAt[r.Starts] = t;
        if (g_WorldHasFaith)
          memcpy(r.FaithAtStart[r.Starts], g_WorldFaith, sizeof(g_WorldFaith));
      }
      ++r.Starts;
      r.Moving = true;
    }
  }

  // The MagRope line entities' bytes
  for (int k = 0; k < MaxWorldRopes; ++k)
  {
    if (!g_WorldRopes[k])
      continue;
    unsigned int now[RopeWatchedSlots];
    if (!ReadWorldBytes(g_WorldRopes[k], now, sizeof(now)))
      continue;
    bool changedNow = false;
    for (int i = 0; i < RopeWatchedSlots; ++i)
    {
      if (g_WorldRopeRead[k] && now[i] != g_WorldRopePrev[k][i])
        changedNow = true;
      RopeSlotStats& st = g_WorldRopeStats[k][i];
      float f;
      memcpy(&f, &now[i], sizeof(f));
      if (!g_WorldRopeRead[k])
        st.First = now[i];
      else if (now[i] != g_WorldRopePrev[k][i])
      {
        if (!st.Changes)
          st.FirstChange = t;
        ++st.Changes;
      }
      st.Last = now[i];
      if (std::isfinite(f))
      {
        if (f < st.Min) st.Min = f;
        if (f > st.Max) st.Max = f;
      }
      g_WorldRopePrev[k][i] = now[i];
    }
    g_WorldRopeRead[k] = true;
    if (changedNow && t - g_WorldRopeLastSample[k] >= 0.1 && g_WorldRopeSampleCount < MaxRopeSamples)
    {
      RopeSample& sample = g_WorldRopeSamples[g_WorldRopeSampleCount++];
      sample.T = (float)t;
      sample.Rope = k;
      memcpy(sample.A, &now[0xA0 / 4], sizeof(sample.A));
      memcpy(sample.B, &now[0xB0 / 4], sizeof(sample.B));
      sample.F58 = now[0x58 / 4];
      sample.F90 = now[0x90 / 4];
      memcpy(sample.Faith, g_WorldFaith, sizeof(sample.Faith));
      g_WorldRopeLastSample[k] = t;
    }
  }
  ReleaseSRWLockExclusive(&g_WorldLock);
  if (!ending || g_WorldLogDone.exchange(true))
    return;
  g_WorldLogOn = false;

  // Copied out under the lock, written after it (only the camera hook gets here)
  static WorldTimelineRecord records[MaxWorldRecords];
  static RopeSlotStats ropeStats[MaxWorldRopes][RopeWatchedSlots];
  static RopeSample samples[MaxRopeSamples];
  __int64 ropes[MaxWorldRopes];
  bool ropeRead[MaxWorldRopes];
  AcquireSRWLockExclusive(&g_WorldLock);
  int count = g_WorldRecordCount, dropped = g_WorldRecordsDropped, alive = g_WorldTimelineCount;
  memcpy(records, g_WorldRecords, sizeof(WorldTimelineRecord) * count);
  memcpy(ropeStats, g_WorldRopeStats, sizeof(ropeStats));
  memcpy(ropes, g_WorldRopes, sizeof(ropes));
  memcpy(ropeRead, g_WorldRopeRead, sizeof(ropeRead));
  int sampleCount = g_WorldRopeSampleCount;
  memcpy(samples, g_WorldRopeSamples, sizeof(RopeSample) * sampleCount);
  ReleaseSRWLockExclusive(&g_WorldLock);

  double length = g_WorldLogUntil - g_WorldLogStart;
  std::sort(records, records + count, [](WorldTimelineRecord const& a, WorldTimelineRecord const& b) { return a.FirstChange < b.FirstChange; });
  // Running the whole time (from the first frames to the end, never
  // stopping): the level's loops, counted apart
  auto always = [&](WorldTimelineRecord const& r) { return r.FirstChange < 0.5 && r.Starts == 1 && r.LastChange > length - 0.5; };
  int loops = 0;
  for (int i = 0; i < count; ++i)
    loops += always(records[i]);
  util::log::Write("World log \"%s\": done, %.0f s: %d timelines alive, %d moved (%d of them the whole time)%s", g_WorldLabel, length,
    alive, count, loops, dropped ? " (more than the log keeps)" : "");
  int shown = 0, loopsShown = 0;
  for (int i = 0; i < count; ++i)
  {
    WorldTimelineRecord const& r = records[i];
    bool loop = always(r);
    if (loop ? loopsShown >= 10 : shown >= 1000)
      continue;
    ++(loop ? loopsShown : shown);
    char tracks[400];
    WorldTimelineTracks(r.Data, tracks, sizeof(tracks));
    float start = 0, end = 0, rate = 0;
    BYTE flags[9] = {};
    ReadWorldFloat(r.Data + 0x34, start);
    ReadWorldFloat(r.Data + 0x38, end);
    ReadWorldFloat(r.Data + 0x40, rate);
    ReadWorldBytes(r.Data + 0x4A, flags, sizeof(flags)); // AutoPlay + 0x4A .. Infinite + 0x52
    char starts[200];
    size_t len = 0;
    starts[0] = 0;
    for (int k = 0; k < r.Starts && k < 3; ++k)
      len += sprintf_s(starts + len, sizeof(starts) - len, "%s%.2f s (Faith %.1f %.1f %.1f)", k ? ", " : "", r.StartAt[k],
        r.FaithAtStart[k][0], r.FaithAtStart[k][1], r.FaithAtStart[k][2]);
    util::log::Write("World log: %s%s 0x%I64X%s: time %.2f -> %.2f (%.2f - %.2f), %d frames moving, began %d times: %s; "
      "data 0x%I64X: %.2f - %.2f, rate %.2f, autoplay %d, looping %d, infinite %d, external time %s; tracks: %s",
      loop ? "the whole time: " : "", r.Class[0] ? r.Class : "timeline", r.Entity, r.Gone ? " (removed)" : "", r.First, r.Last, r.Min,
      r.Max, r.Changes, r.Starts,
      starts, r.Data, start, end, rate, flags[0], flags[7], flags[8], r.External ? "linked" : "no", tracks);
  }

  int unchangedRopes = 0;
  for (int k = 0; k < MaxWorldRopes; ++k)
  {
    if (!ropes[k] || !ropeRead[k])
      continue;
    int changed = 0;
    for (int i = 0; i < RopeWatchedSlots; ++i)
      changed += ropeStats[k][i].Changes != 0;
    if (!changed)
    {
      ++unchangedRopes;
      continue;
    }
    util::log::Write("World log: the MagRope's line entity 0x%I64X, its fields that changed (of the first 0x%X bytes):", ropes[k],
      RopeWatchedBytes);
    for (int i = 0; i < RopeWatchedSlots; ++i)
    {
      RopeSlotStats const& st = ropeStats[k][i];
      if (!st.Changes)
        continue;
      float first, last;
      memcpy(&first, &st.First, sizeof(first));
      memcpy(&last, &st.Last, sizeof(last));
      util::log::Write("World log:   +0x%03X changed %d times (first at %.2f s): 0x%08X -> 0x%08X (as floats %g -> %g, %g - %g)", i * 4,
        st.Changes, st.FirstChange, st.First, st.Last, first, last, st.Min < 1e29f ? st.Min : 0.f, st.Max > -1e29f ? st.Max : 0.f);
    }
    for (int s = 0; s < sampleCount; ++s)
    {
      RopeSample const& x = samples[s];
      if (x.Rope != k)
        continue;
      util::log::Write("World log:   %.2f s: +0xA0 %.2f %.2f %.2f, +0xB0 %.2f %.2f %.2f, +0x58 0x%08X, +0x90 0x%08X, Faith %.2f %.2f %.2f",
        x.T, x.A[0], x.A[1], x.A[2], x.B[0], x.B[1], x.B[2], x.F58, x.F90, x.Faith[0], x.Faith[1], x.Faith[2]);
    }
  }
  util::log::Write("World log: %d other MagRope line entities, nothing in them changed", unchangedRopes);
}

// Test (with the spot sounds): the crowd's and the traffic's sounds are put
// by BoidsProximityTrackerEntity (TypeInfo 0x142893060, vtable 0x141CCEC90),
// which finds the crowd and traffic lines closest to its TrackedPosition
// (data + 0x20, a transform the level links up) and hands their transforms on.
// Its update (0x143B91837) reads TrackedPosition through 0x1447B2060 (data,
// out, context), only ever called from there: out = { the link (null: none),
// the data's own value }, and the getter 0x142A424B0 takes the link's value if
// the link's + 0x1C has 0x40, else out[1]. While it's on, the hook hands the
// trackers the free camera's transform instead (out = { null, the camera });
// it notes what the level gave them for the log.
typedef __int64* (__fastcall* tTrackedPosition)(__int64, __int64*, __int64);
tTrackedPosition oTrackedPosition = nullptr;
const __int64 TrackedPositionRva = 0x47B2060;

namespace
{
  // Written by the camera hook, read by the trackers' updates (any thread):
  // the buffer not being written is the one handed out
  alignas(16) float g_TrackerCamera[2][16];
  std::atomic<int> g_TrackerCameraIndex{ 0 };
  std::atomic<bool> g_TrackersFollowCamera{ false };
  // For the log: updates since the last line, and where the level's last one was
  std::atomic<int> g_TrackerUpdates{ 0 };
  float g_TrackerLevelPosition[3];
  std::atomic<bool> g_TrackerHasLevelPosition{ false };
}

__int64* __fastcall hTrackedPosition(__int64 pData, __int64* pOut, __int64 pContext)
{
  __int64* result = oTrackedPosition(pData, pOut, pContext);
  if (util::DiagnosticsOn())   // for the 10 s line only
  {
    __try
    {
      __int64 pLink = pOut[0];
      float const* m = (pLink && (*(BYTE const*)(pLink + 0x1C) & 0x40)) ? *(float const**)pLink : (float const*)pOut[1];
      if (m)
      {
        memcpy(g_TrackerLevelPosition, m + 12, sizeof(g_TrackerLevelPosition));
        g_TrackerHasLevelPosition.store(true, std::memory_order_relaxed);
      }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
    g_TrackerUpdates.fetch_add(1, std::memory_order_relaxed);
  }
  if (g_TrackersFollowCamera.load(std::memory_order_acquire))
  {
    pOut[0] = 0;
    pOut[1] = (__int64)g_TrackerCamera[g_TrackerCameraIndex.load(std::memory_order_acquire)];
  }
  return result;
}

// The accessor as it was read (TrackedPosition's hash 0xCF65CC52 split into
// 0xC34FA20F + 0x0C162A43) and its one caller, then the getter
static bool IsTrackedPositionCode(BYTE const* pBase)
{
  static const BYTE accessor[] = { 0x53, 0x48, 0x83, 0xEC, 0x30, 0x31, 0xC0, 0x4D, 0x89, 0xC2, 0x48, 0x89, 0xD3, 0x48, 0x89, 0x02,
    0x48, 0x89, 0x42, 0x08, 0x48, 0x8D, 0x41, 0x20, 0x48, 0x89, 0x44, 0x24, 0x28, 0x48, 0x8D, 0x05, 0x54, 0x01, 0x94, 0xFD, 0x49, 0x89,
    0xC8, 0x41, 0xB9, 0x0F, 0xA2, 0x4F, 0xC3, 0x4C, 0x89, 0xD2, 0x48, 0x89, 0xD9, 0x48, 0x89, 0x44, 0x24, 0x20, 0x45, 0x8D, 0x89, 0x43,
    0x2A, 0x16, 0x0C, 0xE8 };
  static const BYTE caller[] = { 0x48, 0x8D, 0x55, 0x90, 0x48, 0x89, 0xD9, 0xE8, 0x24, 0x08, 0xC2, 0x00, 0x48, 0x8D, 0x15, 0x95, 0x09,
    0x56, 0xFE, 0x48, 0x89, 0xC1, 0xE8, 0x65, 0x0C, 0xEB, 0xFE };
  return !memcmp(pBase + TrackedPositionRva, accessor, sizeof(accessor)) && !memcmp(pBase + 0x3B91830, caller, sizeof(caller));
}

// Replay people: the game takes AI out of the level on its own. Each AI has
// an auto-despawn manager; PamServerAIAutoDespawnManager's check (vtable
// 0x141C54720 slot 26, 0x143982AE0) despawns it (its + 0x50's vtable + 0xC0,
// + 0x60 = 1) when it's farther than MinPlayerDistance (data + 0x28) from the
// player and outside MinPlayerAngle (+ 0x2C) of where the player looks
// (0x142A33930), and, with CheckOutsideCombatArea (+ 0x38), outside its
// combat area (0x1439811B0); the base ServerAIAutoDespawnManager's (0x141A5E5F0
// slot 26, 0x142CB2FE0) once MaxLifetime (data + 0x20) has gone by since + 0x5C.
// That's the only despawn in either. A replay with KrugerSec in it lost
// nine of ten of them that way once the real Faith had walked off (2026-10-23),
// so while the replay holds people the checks aren't run.
typedef __int64(__fastcall* tAIAutoDespawn)(__int64);
tAIAutoDespawn oAIAutoDespawnPam = nullptr;
tAIAutoDespawn oAIAutoDespawnBase = nullptr;
const __int64 AIAutoDespawnPamRva = 0x3982AE0;
const __int64 AIAutoDespawnBaseRva = 0x2CB2FE0;

namespace
{
  std::atomic<bool> g_HoldAIDespawn{ false };
  std::atomic<int> g_AIDespawnsHeld{ 0 };
}

void LogContainerCounts();   // diagnostic (temporary), see hContainerDestroyAll
void LetSubLevelsGo();       // see hSubLevelUnload
static void SoundContainerGone(__int64 container);   // see g_SoundGen
static void SoundLevelChanged(char const* why);      // see g_SoundGen

// Only the AI near the recorded people (2026-10-26: mission characters no
// longer despawned while the user played; the hold was on for every AI in the
// level whenever the replay held people, between recording and playing too
// with "Keep them in the level until it plays"). See HoldsAIDespawn.
static bool HoldsAIDespawn(__int64 pManager);

__int64 __fastcall hAIAutoDespawnPam(__int64 pManager)
{
  if (g_HoldAIDespawn.load(std::memory_order_relaxed) && HoldsAIDespawn(pManager))
  {
    g_AIDespawnsHeld.fetch_add(1, std::memory_order_relaxed);
    return 0;
  }
  return oAIAutoDespawnPam(pManager);
}

__int64 __fastcall hAIAutoDespawnBase(__int64 pManager)
{
  if (g_HoldAIDespawn.load(std::memory_order_relaxed) && HoldsAIDespawn(pManager))
  {
    g_AIDespawnsHeld.fetch_add(1, std::memory_order_relaxed);
    return 0;
  }
  return oAIAutoDespawnBase(pManager);
}

// Both checks as they were read, each in its vtable's slot 26
static bool IsAIAutoDespawnCode(BYTE const* pBase)
{
  static const BYTE pam[] = { 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0x41, 0x40, 0x4C, 0x8B, 0x41, 0x28, 0x48, 0x89, 0xCB, 0x48,
    0x8B, 0x90, 0xD0, 0x00, 0x00, 0x00, 0xF3, 0x41, 0x0F, 0x10, 0x50, 0x2C };
  static const BYTE base[] = { 0x53, 0x48, 0x83, 0xEC, 0x30, 0x48, 0x8B, 0x05, 0x84, 0xA6, 0x88, 0xFF, 0x48, 0x89, 0xCB, 0x0F, 0x29,
    0x74, 0x24, 0x20, 0xF3, 0x0F, 0x10, 0x71 };
  return !memcmp(pBase + AIAutoDespawnPamRva, pam, sizeof(pam)) && !memcmp(pBase + AIAutoDespawnBaseRva, base, sizeof(base)) &&
    *(__int64 const*)(pBase + 0x1C54720 + 26 * 8) == (__int64)pBase + AIAutoDespawnPamRva &&
    *(__int64 const*)(pBase + 0x1A5E5F0 + 26 * 8) == (__int64)pBase + AIAutoDespawnBaseRva;
}

void HandBackKeptLater();   // see KeepGuard
void KeepKept();

// The KrugerSec spawners (2026-10-27, stage 1 of stand-ins for a replay whose
// people a mission took with it; read only). PamServerCharacterSpawnEntity
// (vtable 0x141C4B388, TypeInfo 0x1428577C0; base ServerCharacterSpawnEntity
// 0x141B4E0C8, ServerSpawnEntity 0x141B4DDB0) is made by 0x143953860(memory,
// info, data) from one place (0x143957667) and destroyed by 0x1439548A0
// (from its deleting destructor 0x143954F10). Its events go to
// ServerSpawnEntity's handler 0x143383D40: 0x0DC320DE is Spawn (0x143383DB4:
// only with + 0x2A4 set; unless the data's QueueSpawnEvent + 0x180, only when
// 0x1433804A0 says it can and 0x143388DD0 isn't at its limit; then queued on
// the spawn manager by 0x143174360). Its data is a CharacterSpawnReference-
// ObjectData: ControllableTransform + 0xA0 (its row 3 the place, local to its
// blueprint), MaxCount + 0x144, Template + 0x198 (a spawn template, an asset
// named characters/spawntemplates/...). The spawners are noted as they're made
// and dropped as they go, and listed in the log when a playback finds its
// people gone, to see which could make stand-ins. Nothing is sent to them.
typedef __int64(__fastcall* tSpawnerCtor)(__int64, __int64, __int64);
tSpawnerCtor oSpawnerCtor = nullptr;
typedef void(__fastcall* tSpawnerDtor)(__int64);
tSpawnerDtor oSpawnerDtor = nullptr;

namespace
{
  const __int64 SpawnerCtorRva = 0x3953860;
  const __int64 SpawnerCtorCallRva = 0x3957667;
  const __int64 SpawnerDtorRva = 0x39548A0;
  const __int64 SpawnerDtorCallRva = 0x3954F1F;
  const __int64 SpawnerVTableRva = 0x1C4B388;
  const size_t MaxSpawners = 4096;
  std::mutex g_SpawnersMutex;
  std::vector<__int64> g_Spawners;      // under g_SpawnersMutex
}

__int64 __fastcall hSpawnerCtor(__int64 memory, __int64 info, __int64 data)
{
  __int64 entity = oSpawnerCtor(memory, info, data);
  if (entity)
  {
    std::lock_guard<std::mutex> lock(g_SpawnersMutex);
    if (g_Spawners.size() < MaxSpawners)
      g_Spawners.push_back(entity);
  }
  return entity;
}

void __fastcall hSpawnerDtor(__int64 entity)
{
  {
    std::lock_guard<std::mutex> lock(g_SpawnersMutex);
    auto it = std::find(g_Spawners.begin(), g_Spawners.end(), entity);
    if (it != g_Spawners.end())
    {
      *it = g_Spawners.back();
      g_Spawners.pop_back();
    }
  }
  oSpawnerDtor(entity);
}

static bool IsSpawnerLifeCode(BYTE const* pBase)
{
  // mov [rsp + 8], rcx; push rbx; sub rsp, 0x40; mov qword [rsp + 0x30], -2; mov rbx, rcx (the constructor)
  static const BYTE ctor[] = { 0x48, 0x89, 0x4C, 0x24, 0x08, 0x53, 0x48, 0x83, 0xEC, 0x40, 0x48, 0xC7, 0x44, 0x24, 0x30, 0xFE, 0xFF,
    0xFF, 0xFF, 0x48, 0x89, 0xCB };
  // mov [rsp + 8], rcx; push rbx; sub rsp, 0x30; mov qword [rsp + 0x20], -2; mov rbx, rcx; lea rax, [vtable]; mov [rcx], rax
  static const BYTE dtor[] = { 0x48, 0x89, 0x4C, 0x24, 0x08, 0x53, 0x48, 0x83, 0xEC, 0x30, 0x48, 0xC7, 0x44, 0x24, 0x20, 0xFE, 0xFF,
    0xFF, 0xFF, 0x48, 0x89, 0xCB, 0x48, 0x8D, 0x05 };
  auto calls = [pBase](__int64 at, __int64 target) {
    return pBase[at] == 0xE8 && at + 5 + *(int const*)(pBase + at + 1) == target;
  };
  // Each puts the spawner's vtable in place: the constructor at + 0x31, the destructor at + 0x16
  auto vtableAt = [pBase](__int64 lea) { return lea + 7 + *(int const*)(pBase + lea + 3) == SpawnerVTableRva; };
  return !memcmp(pBase + SpawnerCtorRva, ctor, sizeof(ctor)) && !memcmp(pBase + SpawnerDtorRva, dtor, sizeof(dtor)) &&
    calls(SpawnerCtorCallRva, SpawnerCtorRva) && calls(SpawnerDtorCallRva, SpawnerDtorRva) &&
    pBase[SpawnerCtorRva + 0x31] == 0x48 && pBase[SpawnerCtorRva + 0x32] == 0x8D && vtableAt(SpawnerCtorRva + 0x31) &&
    vtableAt(SpawnerDtorRva + 0x16);
}

namespace
{
  void ReadAssetName(__int64 asset, char* out, size_t size);   // below

  struct SpawnerInfo
  {
    BYTE Enabled;
    float Place[3];
    int MaxCount;
    char Template[128];
  };

  bool ReadSpawner(__int64 entity, SpawnerInfo& out)
  {
    __try
    {
      if (*(__int64 const*)entity != (__int64)g_gameHandle + SpawnerVTableRva)
        return false;
      out.Enabled = *(BYTE const*)(entity + 0x2A4);
      __int64 data = *(__int64 const*)(entity + 0x30);
      if (data < 0x10000)
        return false;
      memcpy(out.Place, (void const*)(data + 0xA0 + 0x30), sizeof(out.Place));
      out.MaxCount = *(int const*)(data + 0x144);
      __int64 spawnTemplate = *(__int64 const*)(data + 0x198);
      if (spawnTemplate > 0x10000)
        ReadAssetName(spawnTemplate, out.Template, sizeof(out.Template));
      else
        strcpy_s(out.Template, "(none)");
      return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      return false;
    }
  }
}

// From the replay (game thread): the live KrugerSec spawners, in the log
void util::hooks::LogSpawners(char const* why)
{
  if (!util::DiagnosticsOn())
    return;
  std::vector<__int64> spawners;
  {
    std::lock_guard<std::mutex> lock(g_SpawnersMutex);
    spawners = g_Spawners;
  }
  if (!oSpawnerCtor)
    return;
  util::log::Write("Replay (diagnostic): %s: %zu KrugerSec spawners in the level", why, spawners.size());
  int listed = 0;
  for (__int64 entity : spawners)
  {
    SpawnerInfo s{};
    if (!ReadSpawner(entity, s) || listed++ >= 60)
      continue;
    util::log::Write("Replay (diagnostic):   spawner 0x%I64X: %s, template %s, max %d, data place %.1f %.1f %.1f", entity,
      s.Enabled ? "on" : "off", s.Template, s.MaxCount, s.Place[0], s.Place[1], s.Place[2]);
  }
}

bool util::hooks::HoldAIDespawn(bool hold)
{
  if (g_HoldAIDespawn.exchange(hold) != hold)
  {
    int held = g_AIDespawnsHeld.exchange(0);
    if (util::DiagnosticsOn())
      LogContainerCounts();
    if (hold)
    {
      KeepKept();
      util::log::Write("Replay: the game doesn't take the AI near the recorded people out of the level while the replay holds "
        "them (the others as usual)");
    }
    else
    {
      util::log::Write("Replay: the game takes AI out of the level again (it would have taken some out %d times meanwhile)", held);
      HandBackKeptLater();
      LetSubLevelsGo();
    }
  }
  return oAIAutoDespawnPam && oAIAutoDespawnBase;
}

// Replay: while it plays, Faith takes no damage (the live game goes on under
// it: KrugerSec still there shot the real Faith standing where she was, and
// she died as a replay ended, 2026-10-23). Her health component is a
// PamServerHumanHealthComponent (vtable 0x141C40700; the AI's are another
// class, whose slot 32 0x143995FA0 calls the base's 0x143923A90). Its slot 32,
// 0x143923EA0 (component, result, damage info: + 0x88 the damage), takes the
// damage off its health (+ 0x20) and kills at 0; it's in no other vtable. The
// game's own way past a hit (0x1439240DD, when the component's + 0x128
// property says so) zeroes the 0x24-byte result and returns it: that's what
// a hit gets while the replay plays.
typedef __int64*(__fastcall* tHumanDamage)(__int64, __int64*, __int64);
tHumanDamage oHumanDamage = nullptr;
const __int64 HumanDamageRva = 0x3923EA0;
const __int64 HumanHealthVTableRva = 0x1C40700;
const int HumanDamageSlot = 32;
const size_t HumanDamageResultBytes = 0x24;

namespace
{
  std::atomic<bool> g_NoFaithDamage{ false };
  std::atomic<int> g_FaithHitsIgnored{ 0 };
}

__int64* __fastcall hHumanDamage(__int64 pComponent, __int64* pResult, __int64 pInfo)
{
  if (g_NoFaithDamage.load(std::memory_order_relaxed))
  {
    g_FaithHitsIgnored.fetch_add(1, std::memory_order_relaxed);
    memset(pResult, 0, HumanDamageResultBytes);
    return pResult;
  }
  return oHumanDamage(pComponent, pResult, pInfo);
}

static bool IsHumanDamageCode(BYTE const* pBase)
{
  // mov rax, rsp; push rbp rsi rdi r12 r13 r14 r15; sub rsp, 0x160; mov [rsp + 0x30], -2
  static const BYTE code[] = { 0x48, 0x89, 0xE0, 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x81,
    0xEC, 0x60, 0x01, 0x00, 0x00, 0x48, 0xC7, 0x44, 0x24, 0x30, 0xFE, 0xFF, 0xFF, 0xFF };
  return !memcmp(pBase + HumanDamageRva, code, sizeof(code)) &&
    *(__int64 const*)(pBase + HumanHealthVTableRva + HumanDamageSlot * 8) == (__int64)pBase + HumanDamageRva;
}

bool util::hooks::ProtectFaith(bool on)
{
  if (!oHumanDamage)
    return false;
  if (g_NoFaithDamage.exchange(on) != on)
  {
    int hits = g_FaithHitsIgnored.exchange(0);
    if (on)
      util::log::Write("Replay: Faith takes no damage while it plays");
    else
      util::log::Write("Replay: Faith takes damage again (%d hits on her were let pass meanwhile)", hits);
  }
  return true;
}

// Diagnostic (temporary): the effects the game plays near Faith, for the
// replay's gun effects (muzzle flashes, tracers, impacts). 0x14329B860
// (client effect manager [0x142416160], an EffectBlueprint, its 16-byte
// aligned 4x4 transform, and five more: r9 and four on the stack) -> a handle
// (0: not played) is what a ClientEffectComponent's start (0x1432B8B60)
// calls; 33 places call it (2026-10-24, offline). Hooked to look only: every
// call goes through as it is (the two extra arguments passed along are
// ignored by it), and per blueprint within EffectLogRadius of Faith the log
// gets every EffectLogInterval how often it played, where last and from where.
typedef unsigned int(__fastcall* tEffectPlay)(__int64, __int64, float const*, __int64, __int64, __int64, __int64, __int64,
  __int64, __int64);
tEffectPlay oEffectPlay = nullptr;
const __int64 EffectPlayRva = 0x329B860;
const __int64 EffectPlayCallRva = 0x32B8CD3;   // in 0x1432B8B60
const int AssetNameOffset = 0x10;
const float EffectLogRadius = 60.f;
const double EffectLogInterval = 5.0;

namespace
{
  struct EffectSeen
  {
    int Count;
    int NotPlayed;   // handle 0
    float Last[3];
    __int64 Caller;
    // Diagnostic (temporary): the first call's other arguments, for playing
    // the gun effects again from the replay: r9, the int, the two structs'
    // first bytes (pointers into the caller's frame, alive during the call),
    // the bool
    bool HaveArgs;
    __int64 Context, ContextVTable, Struct1, Struct2;
    int Number;
    unsigned char Flag;
    unsigned char Bytes1[0x40], Bytes2[0x40];
  };
  std::unordered_set<__int64> g_EffectArgsLogged;          // blueprints whose arguments are in the log; under g_EffectLogMutex
  std::mutex g_EffectLogMutex;
  std::unordered_map<__int64, EffectSeen> g_EffectsSeen;   // by blueprint, under g_EffectLogMutex
  int g_EffectsFar = 0;                                    // under g_EffectLogMutex
  // Faith's place, written by the game thread every frame (a torn read only
  // misfiles one effect in the log)
  float g_EffectCenter[3];
  std::atomic<bool> g_EffectCenterOk{ false };
  double g_EffectLogAt = 0.0;
  // A replay plays (see util::hooks::SetReplayPlaying): the live game's
  // sounds within LiveLeaveOutRadius of the real Faith, standing where she
  // is, are left out meanwhile (the KrugerSec still fighting round her: the
  // bullets' impacts were heard over the replay, 2026-10-25 03:1x)
  std::atomic<bool> g_ReplayPlaying{ false };
  const float LiveLeaveOutRadius = 80.f;
  std::atomic<int> g_LiveEffectSoundsLeftOut{ 0 };
  std::atomic<int> g_LiveVoiceEventsLeftOut{ 0 };

  bool ReadEffectPlace(float const* transform, float* place)
  {
    __try
    {
      memcpy(place, transform + 12, 3 * sizeof(float));
      return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      return false;
    }
  }

  bool ReadEffectArgs(__int64 context, __int64 struct1, __int64 struct2, EffectSeen& seen)
  {
    __try
    {
      seen.ContextVTable = context > 0x10000 ? *(__int64 const*)context : 0;
      if (struct1 > 0x10000)
        memcpy(seen.Bytes1, (void const*)struct1, sizeof(seen.Bytes1));
      if (struct2 > 0x10000)
        memcpy(seen.Bytes2, (void const*)struct2, sizeof(seen.Bytes2));
      return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      return false;
    }
  }

  void NoteEffect(__int64 blueprint, float const* transform, __int64 caller, unsigned int handle, __int64 context, __int64 number,
    __int64 struct1, __int64 struct2, __int64 flag)
  {
    float place[3];
    if (!g_EffectCenterOk.load(std::memory_order_relaxed) || !ReadEffectPlace(transform, place))
      return;
    float dx = place[0] - g_EffectCenter[0], dy = place[1] - g_EffectCenter[1], dz = place[2] - g_EffectCenter[2];
    bool isNear = dx * dx + dy * dy + dz * dz <= EffectLogRadius * EffectLogRadius;
    std::lock_guard<std::mutex> lock(g_EffectLogMutex);
    if (!isNear)
    {
      ++g_EffectsFar;
      return;
    }
    EffectSeen& seen = g_EffectsSeen[blueprint];
    if (!seen.HaveArgs && !g_EffectArgsLogged.count(blueprint))
    {
      seen.Context = context;
      seen.Number = (int)number;
      seen.Struct1 = struct1;
      seen.Struct2 = struct2;
      seen.Flag = (unsigned char)flag;
      seen.HaveArgs = ReadEffectArgs(context, struct1, struct2, seen);
    }
    ++seen.Count;
    seen.NotPlayed += handle == 0;
    memcpy(seen.Last, place, sizeof(place));
    seen.Caller = caller;
  }

  // The blueprint's name (Asset + 0x10), or "?"
  void ReadAssetName(__int64 asset, char* out, size_t size)
  {
    strcpy_s(out, size, "?");
    __try
    {
      char const* name = *(char const**)(asset + AssetNameOffset);
      size_t k = 0;
      for (; name && k + 1 < size && name[k]; ++k)
        out[k] = name[k];
      out[k] = 0;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      strcpy_s(out, size, "?");
    }
  }
}

// Replay gun effects: while the replay records, the gun effects the game plays
// within ReplayEffectRadius of Faith (muzzle flashes, tracers, impacts and
// their impact sounds: blueprints named FX/Weapons/..., FX/Impact/... or
// .../BulletImpacts/...) are noted with the replay's clock, their transform and
// their second parameter (16 bytes); while it plays they're played again at
// their times through the same function. Its arguments (2026-10-24, offline):
// r9 a live client container (the effect's owner; its + 0x30 bit 0 and + 0x9C
// are checked), an int of flags (0 from the game's callers; bit 2 makes it
// read the first parameter), two 16-byte parameters (the first read only with
// that bit, the second always, kept on the effect), a bool; the transform is
// copied. A replayed effect gets the container the game used last (the same
// for every effect in the 20:22 log) and a blueprint still that blueprint.
// Effects the game started before the recording that it moves or gives
// parameters while it records (a hunter drone's five scanning lasers and its
// searchlight, a VTOL's engines: started once, then aimed every frame through
// their effect component; the 2026-10-24 22:43 log) are taken in at their first
// move while it records, as if started there and then, when that's near
// Faith. Each effect's parameter sets (an effect component gives one when it
// starts its effect, at 0x1432B8DE2, and one each time its Transform or
// EmitterParameters change, at 0x1432B9C3E) are kept and given to the replayed
// effect at their times: the Shock Protectors' Crippler effects and the
// drone's were played again without them and weren't seen (2026-10-24 22:4x).
const __int64 EffectManagerRva = 0x2416160;        // [0x142416160]: the client effect manager
const __int64 ClientContainerVTableRva = 0x1B961E0;
const float ReplayEffectRadius = 80.f;
const size_t MaxReplayEffects = 20000;
// Moves: one a 60th of a second at most per effect, none that doesn't move it
// (2026-10-24 22:43: the 160000 there were used up by VTOL downwashes moved
// every frame); 80 bytes each
const size_t MaxReplayEffectMoves = 400000;
const double MinReplayEffectMoveInterval = 1.0 / 60.0;
const size_t MaxReplayEffectParamBytes = 32u << 20;
// One the game hadn't stopped that it moved or gave parameters this close to
// the recording's end is taken to be still playing then: the replayed one plays
// on until the playback loops or stops. One started before the recording that
// wasn't is stopped this long after its last update.
const double ReplayEffectAliveAtEnd = 0.5;
const double EarlierEffectTail = 0.25;
// On a seek, a loop or the playback's start, a recorded effect still playing
// then is started again only if it's a lasting one (see IsLastingEffect) or
// it started at most this long before (see util::hooks::ReplayEffectsPlay)
const double OneOffCatchUp = 0.1;
// An effect's parameter list (made by 0x14315FE40, added to by 0x1431667F0;
// 2026-10-24, offline): + 0 how many, + 8 a key for each (8 at most: a
// parameter's definition, its type at + 0x18: 0 a float, 1 a vector, 2 a bool,
// 3 an int, 4 and 5 an object's pointer; 0x14305EC40), + 0x48 each one's place
// in the data, + 0x68 the data's size, + 0x70 the data. 0x14329F850 queues a
// copy of its first (2 x size + 0x7F) & ~0xF bytes. A kept copy is given again
// only while each key still starts with the 8 bytes it had then, and one that
// holds an object never (the object may be gone).
const int EffectParamsKeysOffset = 0x08;
const int EffectParamsDataSizeOffset = 0x68;
const int EffectParamTypeOffset = 0x18;
const unsigned int MaxEffectParamKeys = 8;
const unsigned int MaxEffectParamData = 0x400;
const unsigned int MaxEffectParamListBytes = (2 * MaxEffectParamData + 0x7F) & ~0xFu;
const size_t MaxPendingEffectParams = 4096;

namespace
{
  struct ReplayEffect
  {
    double Time;
    __int64 Blueprint, BlueprintVTable;
    float Rows[16];
    float Param[4];
    unsigned int Id;          // its place in the recording order (the moves refer to it)
    double StopTime;          // when the game stopped it (-1: it didn't)
    double LastUpdate;        // its last move or parameters while it recorded
    bool Earlier;             // started before the recording (taken in at its first move in it)
    bool Static;              // a mission effect playing when the recording started (see TakeInPlayingEffects)
    unsigned int LiveHandle;  // that one's own handle in the game (0: not one)
  };
  // A tracer flies: the bullet moves its effect every frame (0x14329F9A0,
  // from the bullet's update 0x1438FFB40) and stops it when it goes
  // (0x1432A2080, from 0x143901D90; 2026-10-24, offline)
  struct ReplayEffectMove
  {
    unsigned int Id;
    double Time;
    float Rows[16];
  };
  struct ReplayEffectParams
  {
    unsigned int Id;
    double Time;
    unsigned int Offset, Size, Keys;   // in g_ReplayFxParamBytes: the list's bytes, then its keys' first 8 bytes each
  };
  struct EffectParamsCopy
  {
    alignas(16) BYTE Bytes[MaxEffectParamListBytes];
    __int64 Checks[MaxEffectParamKeys];   // each key's first 8 bytes
    unsigned int Size, Keys;
  };
  struct ActiveReplayEffect
  {
    unsigned int Handle;
    size_t NextMove, EndMove;     // its moves in g_ReplayFxMoves
    size_t NextParams, EndParams; // its parameter sets in g_ReplayFxParams
    double StopTime;
    double LastTime;              // of its last move or parameters (one without a stop is let be 2 s after)
    bool Lasting;                 // kept while the playback is paused (see util::hooks::ReplayEffectsHold)
  };
  std::vector<ReplayEffectMove> g_ReplayFxMoves;     // under g_ReplayFxMutex
  std::unordered_map<unsigned int, unsigned int> g_ReplayFxHandles; // recording: the game's handle -> its effect's id; under it
  std::unordered_map<unsigned int, std::pair<size_t, size_t>> g_ReplayFxMoveRanges; // id -> its moves; under it
  std::unordered_map<unsigned int, size_t> g_ReplayFxLastMove;      // recording: id -> its last move; under it
  std::vector<ReplayEffectParams> g_ReplayFxParams;                 // under it
  std::vector<BYTE> g_ReplayFxParamBytes;                           // under it
  std::unordered_map<unsigned int, std::pair<size_t, size_t>> g_ReplayFxParamRanges; // id -> its parameter sets; under it
  std::unordered_map<unsigned int, size_t> g_ReplayFxLastParams;    // recording: id -> its last parameter set; under it
  std::unordered_map<unsigned int, EffectParamsCopy> g_FxPendingParams; // recording: the last parameters of one started before it, until its first move; under it
  std::vector<ActiveReplayEffect> g_ReplayFxActive;  // game thread
  std::mutex g_ReplayFxMutex;
  std::vector<ReplayEffect> g_ReplayFx;              // under g_ReplayFxMutex
  std::unordered_map<__int64, bool> g_GunEffectKinds; // blueprint -> a gun effect; under it
  double g_ReplayFxClock = 0.0;                      // under it
  __int64 g_LiveEffectContext = 0;                   // under it
  std::atomic<bool> g_ReplayFxRecording{ false };
  int g_ReplayFxEarlier = 0, g_ReplayFxMovesDropped = 0, g_ReplayFxParamsDropped = 0, g_ReplayFxParamsWithObjects = 0; // under it
  // Every gun effect's handle -> its blueprint, recording or not, until it's
  // stopped (for the ones started before a recording); two generations, the
  // older dropped when the newer is full. The game's handles are a count that
  // only goes up (0x14329B9BC), so one is never another effect's.
  const size_t MaxTrackedFxHandles = 16384;
  std::unordered_map<unsigned int, __int64> g_FxHandleBlueprint;    // under it
  std::unordered_map<unsigned int, __int64> g_FxHandleBlueprintOld; // under it
  // Diagnostic (temporary): which gun effects were playing before a recording
  // started and were moved or given parameters while it recorded, how many of
  // those were taken in, and how many of each kind were recorded and played.
  // Under g_ReplayFxMutex.
  struct FxUpdates
  {
    int Moves, Params;
    float Last[3];
    bool HasLast;
  };
  std::unordered_map<__int64, FxUpdates> g_FxEarlierUpdates;        // blueprint -> its updates while recording
  std::unordered_map<__int64, int> g_FxEarlierTaken;                // blueprint -> taken in
  std::unordered_map<__int64, int> g_FxRecordedParams;              // blueprint -> parameter sets on recorded ones
  std::unordered_map<__int64, int> g_ReplayFxPlayedBy;              // game thread: blueprint -> played this playback
  size_t g_ReplayFxNext = 0;                         // game thread
  double g_ReplayFxLast = -1.0;                      // game thread
  bool g_ReplayFxHolding = false;                    // game thread: the playback is paused (see ReplayEffectsHold)
  int g_ReplayFxPlayed = 0, g_ReplayFxSkipped = 0;   // game thread
  int g_ReplayFxParamsGiven = 0, g_ReplayFxParamsLeftOut = 0; // game thread

  bool IsGunEffectName(char const* name)
  {
    // + melee hits and takedowns (FX/Combat/...) and the VTOL's downwash (FX/Props/Vtol/...), seen in a fight (2026-10-24 20:5x)
    // + the runs' and missions' own (FX/UX/...: Runner Vision's trail, a
    // checkpoint's place / pass / remove, the collectibles' orbs, the
    // projection meshes, a hack; game data 2026-10-25): the trail is an
    // effect on objects the trail renderer (PamClientRunVisTrailRenderEntity,
    // its RVTrail_InvisibleTriangle_TEMP) moves along the route at 20 m/s
    return !strncmp(name, "FX/Weapons/", 11) || !strncmp(name, "FX/Impact/", 10) || strstr(name, "/BulletImpacts/") != nullptr ||
      !strncmp(name, "FX/Combat/", 10) || !strncmp(name, "FX/Props/Vtol/", 14) || !_strnicmp(name, "FX/UX/", 6);
  }

  bool ReadReplayEffect(__int64 blueprint, float const* transform, __int64 param, ReplayEffect& out)
  {
    __try
    {
      out.BlueprintVTable = *(__int64 const*)blueprint;
      memcpy(out.Rows, transform, sizeof(out.Rows));
      if (param > 0x10000)
        memcpy(out.Param, (void const*)param, sizeof(out.Param));
      else
        memset(out.Param, 0, sizeof(out.Param));
      return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      return false;
    }
  }

  bool IsClientContainer(__int64 context)
  {
    __try
    {
      return context > 0x10000 && *(__int64 const*)context == (__int64)g_gameHandle + ClientContainerVTableRva;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      return false;
    }
  }

  // Under g_ReplayFxMutex: whether a blueprint is a gun effect (see IsGunEffectName)
  bool IsGunEffectKind(__int64 blueprint)
  {
    auto kind = g_GunEffectKinds.find(blueprint);
    if (kind == g_GunEffectKinds.end())
    {
      char name[128];
      ReadAssetName(blueprint, name, sizeof(name));
      kind = g_GunEffectKinds.emplace(blueprint, IsGunEffectName(name)).first;
    }
    return kind->second;
  }

  // Under g_ReplayFxMutex: whether a recorded kind lasts as long as what makes
  // it (a VTOL's engine and downwash, a hunter drone's scanning lasers,
  // searchlight and downwash: started once and moved for as long as it's
  // there; 2026-10-24 logs and game data), unlike a shot's flash, a blast, a
  // tracer or a hit (see OneOffCatchUp)
  std::unordered_map<__int64, bool> g_LastingEffectKinds; // blueprint -> lasting; under it

  bool IsLastingEffect(__int64 blueprint)
  {
    auto kind = g_LastingEffectKinds.find(blueprint);
    if (kind == g_LastingEffectKinds.end())
    {
      static char const* const lasting[] = { "FX/Props/Vtol/", "FX/Combat/Drone/FX_Drone_Laser_Ray",
        "FX/Combat/Drone/FX_Drone_SearchLight", "FX/Weapons/Drone/FX_Drone_Downwash", "FX/UX/RunnersVision/",
        "FX/UX/ProjectionMesh/", "FX/UX/Triggers/FX_UX_Triggers_CompulsionOrb", "FX/UX/Triggers/FX_UX_Triggers_MetaGridOrb" };
      char name[128];
      ReadAssetName(blueprint, name, sizeof(name));
      bool is = false;
      for (char const* prefix : lasting)
        is = is || !_strnicmp(name, prefix, strlen(prefix));
      kind = g_LastingEffectKinds.emplace(blueprint, is).first;
    }
    return kind->second;
  }

  // Under g_ReplayFxMutex: a gun effect's blueprint by its handle (0: not one, or not known)
  __int64 FindEffectBlueprint(unsigned int handle)
  {
    auto it = g_FxHandleBlueprint.find(handle);
    if (it != g_FxHandleBlueprint.end())
      return it->second;
    it = g_FxHandleBlueprintOld.find(handle);
    return it != g_FxHandleBlueprintOld.end() ? it->second : 0;
  }

  // Any thread: every gun effect's handle, recording or not
  void TrackEffectHandle(__int64 blueprint, unsigned int handle)
  {
    std::lock_guard<std::mutex> lock(g_ReplayFxMutex);
    if (!IsGunEffectKind(blueprint))
      return;
    if (g_FxHandleBlueprint.size() >= MaxTrackedFxHandles)
    {
      g_FxHandleBlueprintOld.swap(g_FxHandleBlueprint);
      g_FxHandleBlueprint.clear();
    }
    g_FxHandleBlueprint[handle] = blueprint;
  }

  // Diagnostic (temporary), under g_ReplayFxMutex while recording: an update of
  // an effect that isn't recorded but was started earlier
  void NoteEarlierEffectUpdate(unsigned int handle, float const* rows, bool move)
  {
    __int64 blueprint = FindEffectBlueprint(handle);
    if (!blueprint)
      return;
    FxUpdates& u = g_FxEarlierUpdates[blueprint];
    ++(move ? u.Moves : u.Params);
    if (rows)
    {
      memcpy(u.Last, rows + 12, sizeof(u.Last));
      u.HasLast = true;
    }
  }

  // An effect's parameter list copied (see EffectParamsKeysOffset): 1, or 0
  // when it can't be read or isn't one, or -1 when it holds an object
  int ReadEffectParams(__int64 list, EffectParamsCopy& out)
  {
    __try
    {
      unsigned int keys = *(unsigned int const*)list;
      unsigned int data = *(unsigned int const*)(list + EffectParamsDataSizeOffset);
      if (keys > MaxEffectParamKeys || data > MaxEffectParamData)
        return 0;
      unsigned int size = (2 * data + 0x7F) & ~0xFu;
      memcpy(out.Bytes, (void const*)list, size);
      for (unsigned int i = 0; i < keys; ++i)
      {
        __int64 key = *(__int64 const*)(out.Bytes + EffectParamsKeysOffset + i * 8);
        if (key < 0x10000)
          return 0;
        int type = *(int const*)(key + EffectParamTypeOffset);
        if (type == 4 || type == 5)
          return -1;
        out.Checks[i] = *(__int64 const*)key;
      }
      out.Size = size;
      out.Keys = keys;
      return 1;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      return 0;
    }
  }

  // Under g_ReplayFxMutex while recording: a recorded effect's parameter set,
  // unless it's the same as its one before
  void KeepReplayEffectParams(unsigned int id, EffectParamsCopy const& p)
  {
    if (id < g_ReplayFx.size())
      g_ReplayFx[id].LastUpdate = g_ReplayFxClock;
    auto last = g_ReplayFxLastParams.find(id);
    if (last != g_ReplayFxLastParams.end() && last->second < g_ReplayFxParams.size())
    {
      ReplayEffectParams const& before = g_ReplayFxParams[last->second];
      BYTE const* kept = g_ReplayFxParamBytes.data() + before.Offset;
      if (before.Size == p.Size && before.Keys == p.Keys && !memcmp(kept, p.Bytes, p.Size) &&
          !memcmp(kept + before.Size, p.Checks, p.Keys * sizeof(__int64)))
        return;
    }
    size_t bytes = p.Size + p.Keys * sizeof(__int64);
    if (g_ReplayFxParamBytes.size() + bytes > MaxReplayEffectParamBytes)
    {
      ++g_ReplayFxParamsDropped;
      return;
    }
    ReplayEffectParams s{ id, g_ReplayFxClock, (unsigned int)g_ReplayFxParamBytes.size(), p.Size, p.Keys };
    g_ReplayFxParamBytes.insert(g_ReplayFxParamBytes.end(), p.Bytes, p.Bytes + p.Size);
    g_ReplayFxParamBytes.insert(g_ReplayFxParamBytes.end(), (BYTE const*)p.Checks, (BYTE const*)p.Checks + p.Keys * sizeof(__int64));
    g_ReplayFxLastParams[id] = g_ReplayFxParams.size();
    g_ReplayFxParams.push_back(s);
  }

  // Under g_ReplayFxMutex while recording: a gun effect the game started
  // before the recording, moved now, taken in as if started here and now when
  // that's near Faith, with the last parameters it got before (its id)
  bool TakeInEarlierEffect(unsigned int handle, float const* rows, unsigned int& id)
  {
    __int64 blueprint = FindEffectBlueprint(handle);
    if (!blueprint || !g_EffectCenterOk.load(std::memory_order_relaxed) || g_ReplayFx.size() >= MaxReplayEffects)
      return false;
    float dx = rows[12] - g_EffectCenter[0], dy = rows[13] - g_EffectCenter[1], dz = rows[14] - g_EffectCenter[2];
    if (dx * dx + dy * dy + dz * dz > ReplayEffectRadius * ReplayEffectRadius)
      return false;
    ReplayEffect e{};
    if (!ReadReplayEffect(blueprint, rows, 0, e))
      return false;
    e.Time = g_ReplayFxClock;
    e.Blueprint = blueprint;
    e.Id = (unsigned int)g_ReplayFx.size();
    e.StopTime = -1.0;
    e.LastUpdate = g_ReplayFxClock;
    e.Earlier = true;
    g_ReplayFxHandles[handle] = e.Id;
    g_ReplayFx.push_back(e);
    ++g_ReplayFxEarlier;
    if (util::DiagnosticsOn())
    {
      ++g_FxEarlierTaken[blueprint];
      g_FxEarlierUpdates[blueprint];   // listed at the end even with no other updates
    }
    auto pending = g_FxPendingParams.find(handle);
    if (pending != g_FxPendingParams.end())
    {
      KeepReplayEffectParams(e.Id, pending->second);
      g_FxPendingParams.erase(pending);
    }
    id = e.Id;
    return true;
  }

  // Mission and weather effects already playing when a recording starts
  // (2026-10-25 15:2x test: a mission's effects weren't in its replay, and no
  // FX/UX effect at all was started within 60 m of Faith in the whole 15:24
  // game session, by the effects diagnostic). The runs' and missions' own
  // effects (FX/UX: a collectible's orb, a checkpoint's, a projection mesh)
  // and the level's ambient and weather ones (FX/Ambient: smoke, dust, a
  // mission's set piece) are started when their objects stream in, mostly far
  // from Faith and long before a recording, and most never move, so
  // NoteReplayEffect (started while it records, within ReplayEffectRadius) and
  // TakeInEarlierEffect (taken in at a move) never saw them. Those kinds are
  // kept here from their start (blueprint, transform and second parameter, as
  // NoteReplayEffect reads them) until they're stopped (0x1432A2080,
  // hEffectStop), MaxLiveEffects at most (a full list takes no more, said once
  // in the log), with their last parameter set (see hEffectParams). The
  // game's rain (FX/Ambient/Rain/...) isn't: VISUALS > Weather keeps it at the
  // camera through its moves (weather::FollowRows), and the replay gives the
  // weather menu's settings as recorded instead (FaithReplay::
  // ApplyRecordedWeather). When a recording starts, the ones playing within
  // ReplayEffectRadius of Faith are taken in as if started then (Static: played
  // to the recording's end unless the game stops them first) and listed in the
  // log by kind. While it plays, one that didn't move is played again only
  // while the game's own is gone (a collected orb, a passed checkpoint, a set
  // piece's smoke after the mission): while the game still plays it where it
  // was, a second would double it. Not verified in game: that a collected
  // item's effect is stopped through 0x1432A2080 (one stopped some other way
  // still counts as the game's, and isn't played again).
  struct LiveEffect
  {
    __int64 Blueprint, BlueprintVTable;
    float Rows[16];
    float Param[4];
  };
  const size_t MaxLiveEffects = 32768;
  const size_t MaxLiveEffectParams = 2048;
  std::unordered_map<unsigned int, LiveEffect> g_LiveFx;              // under g_ReplayFxMutex
  std::unordered_map<unsigned int, EffectParamsCopy> g_LiveFxParams;  // their last parameter sets; under it
  std::unordered_map<__int64, bool> g_TakenInKinds;                   // blueprint -> one of those kinds; under it
  bool g_LiveFxFullLogged = false;                                    // under it
  int g_ReplayFxStatic = 0;                                           // under it
  int g_ReplayFxGameStillPlays = 0;                                   // game thread

  // Under g_ReplayFxMutex
  bool IsTakenInKind(__int64 blueprint)
  {
    auto kind = g_TakenInKinds.find(blueprint);
    if (kind == g_TakenInKinds.end())
    {
      char name[128];
      ReadAssetName(blueprint, name, sizeof(name));
      bool is = !_strnicmp(name, "FX/UX/", 6) || (!_strnicmp(name, "FX/Ambient/", 11) && _strnicmp(name, "FX/Ambient/Rain/", 16));
      kind = g_TakenInKinds.emplace(blueprint, is).first;
    }
    return kind->second;
  }

  // Under g_ReplayFxMutex
  LiveEffect const* FindLiveEffect(unsigned int handle)
  {
    auto it = g_LiveFx.find(handle);
    return it != g_LiveFx.end() ? &it->second : nullptr;
  }

  // Any thread, from the hook: an effect the game started
  void NoteLiveEffect(__int64 blueprint, float const* transform, __int64 param, unsigned int handle)
  {
    {
      std::lock_guard<std::mutex> lock(g_ReplayFxMutex);
      if (!IsTakenInKind(blueprint))
        return;
    }
    ReplayEffect read{};
    if (!transform || !ReadReplayEffect(blueprint, transform, param, read))
      return;
    std::lock_guard<std::mutex> lock(g_ReplayFxMutex);
    if (g_LiveFx.size() >= MaxLiveEffects && !g_LiveFx.count(handle))
    {
      if (!g_LiveFxFullLogged)
        util::log::Write("Replay: %zu mission and ambient effects are playing in the level: no room to note more (a recording "
          "won't take in ones started from now on until some stop)", g_LiveFx.size());
      g_LiveFxFullLogged = true;
      return;
    }
    LiveEffect& l = g_LiveFx[handle];
    l.Blueprint = blueprint;
    l.BlueprintVTable = read.BlueprintVTable;
    memcpy(l.Rows, read.Rows, sizeof(l.Rows));
    memcpy(l.Param, read.Param, sizeof(l.Param));
  }

  // From hEffectParams while not recording: such an effect's last parameter
  // set kept (copy: read from the game's list)
  void NoteLiveEffectParams(unsigned int handle, __int64 list)
  {
    {
      std::lock_guard<std::mutex> lock(g_ReplayFxMutex);
      if (!FindLiveEffect(handle) || (g_LiveFxParams.size() >= MaxLiveEffectParams && !g_LiveFxParams.count(handle)))
        return;
    }
    static thread_local EffectParamsCopy copy;
    if (ReadEffectParams(list, copy) <= 0)
      return;
    std::lock_guard<std::mutex> lock(g_ReplayFxMutex);
    if (FindLiveEffect(handle) && (g_LiveFxParams.size() < MaxLiveEffectParams || g_LiveFxParams.count(handle)))
      g_LiveFxParams[handle] = copy;
  }

  // Under g_ReplayFxMutex as the recording starts (its clock at 0): the
  // mission and ambient effects playing near Faith taken in, listed by kind
  void TakeInPlayingEffects()
  {
    struct Near { int Taken; float Closest; };
    std::unordered_map<__int64, Near> nearKinds;
    for (auto const& h : g_LiveFx)
    {
      LiveEffect const& l = h.second;
      float dx = l.Rows[12] - g_EffectCenter[0], dy = l.Rows[13] - g_EffectCenter[1], dz = l.Rows[14] - g_EffectCenter[2];
      float d2 = dx * dx + dy * dy + dz * dz;
      if (d2 > ReplayEffectRadius * ReplayEffectRadius || g_ReplayFx.size() >= MaxReplayEffects || g_ReplayFxHandles.count(h.first))
        continue;
      ReplayEffect e{};
      e.Time = 0.0;
      e.Blueprint = l.Blueprint;
      e.BlueprintVTable = l.BlueprintVTable;
      memcpy(e.Rows, l.Rows, sizeof(e.Rows));
      memcpy(e.Param, l.Param, sizeof(e.Param));
      e.Id = (unsigned int)g_ReplayFx.size();
      e.StopTime = -1.0;
      e.LastUpdate = 0.0;
      e.Earlier = true;
      e.Static = true;
      e.LiveHandle = h.first;
      g_ReplayFxHandles[h.first] = e.Id;
      g_ReplayFx.push_back(e);
      auto params = g_LiveFxParams.find(h.first);
      if (params != g_LiveFxParams.end())
        KeepReplayEffectParams(e.Id, params->second);
      auto n = nearKinds.emplace(l.Blueprint, Near{ 0, 1e9f }).first;
      ++n->second.Taken;
      n->second.Closest = (std::min)(n->second.Closest, sqrtf(d2));
      ++g_ReplayFxStatic;
    }
    util::log::Write("Replay: %d mission and ambient effects (FX/UX/..., FX/Ambient/...) of %d kinds were playing within %.0f m "
      "of Faith as it started recording: taken in (played again while the game's own are gone)", g_ReplayFxStatic,
      (int)nearKinds.size(), ReplayEffectRadius);
    // Diagnostic (temporary): which
    if (!util::DiagnosticsOn())
      return;
    int listed = 0;
    for (auto const& n : nearKinds)
    {
      if (listed++ >= 40)
        break;
      char name[128];
      ReadAssetName(n.first, name, sizeof(name));
      util::log::Write("Replay (diagnostic):   playing as it started: %d x %s, the nearest %.1f m from Faith", n.second.Taken, name,
        n.second.Closest);
    }
  }

  // Under g_ReplayFxMutex while it plays: a mission or ambient effect taken
  // in as the recording started that didn't move, whose own the game still
  // plays where it was (see TakeInPlayingEffects)
  bool IsStillPlayedByGame(ReplayEffect const& e)
  {
    return e.Static && e.LiveHandle && FindLiveEffect(e.LiveHandle) && !g_ReplayFxMoveRanges.count(e.Id);
  }

  // Any thread, from the hook
  void NoteReplayEffect(__int64 blueprint, float const* transform, __int64 context, __int64 param, unsigned int handle)
  {
    if (IsClientContainer(context))
    {
      std::lock_guard<std::mutex> lock(g_ReplayFxMutex);
      g_LiveEffectContext = context;
    }
    if (!g_ReplayFxRecording.load(std::memory_order_relaxed) || !g_EffectCenterOk.load(std::memory_order_relaxed))
      return;
    ReplayEffect e{};
    if (!ReadReplayEffect(blueprint, transform, param, e))
      return;
    float dx = e.Rows[12] - g_EffectCenter[0], dy = e.Rows[13] - g_EffectCenter[1], dz = e.Rows[14] - g_EffectCenter[2];
    if (dx * dx + dy * dy + dz * dz > ReplayEffectRadius * ReplayEffectRadius)
      return;
    std::lock_guard<std::mutex> lock(g_ReplayFxMutex);
    if (!g_ReplayFxRecording.load(std::memory_order_relaxed) || !IsGunEffectKind(blueprint) || g_ReplayFx.size() >= MaxReplayEffects)
      return;
    e.Time = g_ReplayFxClock;
    e.Blueprint = blueprint;
    e.Id = (unsigned int)g_ReplayFx.size();
    e.StopTime = -1.0;
    e.LastUpdate = g_ReplayFxClock;
    e.Earlier = false;
    g_ReplayFxHandles[handle] = e.Id;
    g_ReplayFx.push_back(e);
  }

  // Game thread
  unsigned int PlayReplayEffect(ReplayEffect const& e, __int64 context)
  {
    __try
    {
      if (*(__int64 const*)e.Blueprint != e.BlueprintVTable)
        return 0;
      __int64 manager = *(__int64 const*)((__int64)g_gameHandle + EffectManagerRva);
      if (manager < 0x10000)
        return 0;
      alignas(16) float rows[16];
      alignas(16) float unused[4] = {};
      alignas(16) float param[4];
      memcpy(rows, e.Rows, sizeof(rows));
      memcpy(param, e.Param, sizeof(param));
      return oEffectPlay(manager, e.Blueprint, rows, context, 0, (__int64)unused, (__int64)param, 0, 0, 0);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      return 0;
    }
  }

  bool ReadEffectRows(__int64 rows, float* out)
  {
    __try
    {
      memcpy(out, (void const*)rows, 16 * sizeof(float));
      return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      return false;
    }
  }
}

typedef void(__fastcall* tEffectMove)(__int64, unsigned int, __int64, __int64);
typedef void(__fastcall* tEffectStop)(__int64, unsigned int, __int64, __int64);
tEffectMove oEffectMove = nullptr;
tEffectStop oEffectStop = nullptr;
const __int64 EffectMoveRva = 0x329F9A0;
const __int64 EffectMoveCallRva = 0x38FFD5E;   // in the bullet's update 0x1438FFB40
const __int64 EffectStopRva = 0x32A2080;
const __int64 EffectStopCallRva = 0x3901DBA;   // in the bullet's end 0x143901D90
// An effect's parameters, 0x14329F850(manager, handle, list), as an effect
// component gives them (0x1432B9B00, called at 0x1432B9C3E; 2026-10-24, offline)
typedef void(__fastcall* tEffectParams)(__int64, unsigned int, __int64, __int64);
tEffectParams oEffectParams = nullptr;
const __int64 EffectParamsRva = 0x329F850;
const __int64 EffectParamsCallRva = 0x32B9C3E;

namespace
{
  void MoveReplayEffect(unsigned int handle, float const* rows)
  {
    __try
    {
      __int64 manager = *(__int64 const*)((__int64)g_gameHandle + EffectManagerRva);
      alignas(16) float aligned[16];
      memcpy(aligned, rows, sizeof(aligned));
      if (manager > 0x10000 && oEffectMove)
        oEffectMove(manager, handle, (__int64)aligned, 0);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
  }

  void StopReplayEffect(unsigned int handle)
  {
    __try
    {
      __int64 manager = *(__int64 const*)((__int64)g_gameHandle + EffectManagerRva);
      if (manager > 0x10000 && oEffectStop)
        oEffectStop(manager, handle, 0, 0);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
  }

  // A kept parameter list given to a replayed effect, while its keys are
  // still the ones it had (see EffectParamsKeysOffset)
  bool GiveEffectParams(unsigned int handle, EffectParamsCopy const& p)
  {
    __try
    {
      for (unsigned int i = 0; i < p.Keys; ++i)
      {
        __int64 key = *(__int64 const*)(p.Bytes + EffectParamsKeysOffset + i * 8);
        if (key < 0x10000 || *(__int64 const*)key != p.Checks[i])
          return false;
      }
      __int64 manager = *(__int64 const*)((__int64)g_gameHandle + EffectManagerRva);
      if (manager < 0x10000 || !oEffectParams)
        return false;
      oEffectParams(manager, handle, (__int64)p.Bytes, 0);
      return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      return false;
    }
  }

  // Game thread: a recorded parameter set given to a replayed effect
  void GiveReplayEffectParams(unsigned int handle, size_t index)
  {
    static EffectParamsCopy p;   // game thread only
    {
      std::lock_guard<std::mutex> lock(g_ReplayFxMutex);
      if (index >= g_ReplayFxParams.size())
        return;
      ReplayEffectParams const& s = g_ReplayFxParams[index];
      if (s.Size > sizeof(p.Bytes) || s.Keys > MaxEffectParamKeys ||
          (size_t)s.Offset + s.Size + s.Keys * sizeof(__int64) > g_ReplayFxParamBytes.size())
        return;
      BYTE const* kept = g_ReplayFxParamBytes.data() + s.Offset;
      memcpy(p.Bytes, kept, s.Size);
      memcpy(p.Checks, kept + s.Size, s.Keys * sizeof(__int64));
      p.Size = s.Size;
      p.Keys = s.Keys;
    }
    bool given = GiveEffectParams(handle, p);
    ++(given ? g_ReplayFxParamsGiven : g_ReplayFxParamsLeftOut);
  }

  // Game thread: the replayed effects that move (tracers, a drone's lasers)
  // moved on to time and given their parameters, and stopped when the recorded
  // ones were; all of them stopped (all)
  void UpdateActiveReplayEffects(double time, bool all)
  {
    for (size_t i = 0; i < g_ReplayFxActive.size();)
    {
      ActiveReplayEffect& a = g_ReplayFxActive[i];
      float rows[16];
      bool moved = false;
      size_t firstParams = 0, endParams = 0;
      {
        std::lock_guard<std::mutex> lock(g_ReplayFxMutex);
        while (a.NextMove < a.EndMove && a.NextMove < g_ReplayFxMoves.size() && g_ReplayFxMoves[a.NextMove].Time <= time)
        {
          memcpy(rows, g_ReplayFxMoves[a.NextMove++].Rows, sizeof(rows));
          moved = true;
        }
        firstParams = a.NextParams;
        while (a.NextParams < a.EndParams && a.NextParams < g_ReplayFxParams.size() && g_ReplayFxParams[a.NextParams].Time <= time)
          ++a.NextParams;
        endParams = a.NextParams;
      }
      if (!all)
      {
        if (moved)
          MoveReplayEffect(a.Handle, rows);
        for (size_t p = firstParams; p < endParams; ++p)
          GiveReplayEffectParams(a.Handle, p);
      }
      bool stop = all || (a.StopTime >= 0.0 && time >= a.StopTime);
      bool done = stop || (a.StopTime < 0.0 && a.NextMove >= a.EndMove && a.NextParams >= a.EndParams && time > a.LastTime + 2.0);
      if (stop)
        StopReplayEffect(a.Handle);
      if (done)
      {
        g_ReplayFxActive[i] = g_ReplayFxActive.back();
        g_ReplayFxActive.pop_back();
      }
      else
        ++i;
    }
  }

  // Game thread: a recorded effect played again, and followed from here on
  // when it moved, got parameters or was stopped. Started late (catchUp: a
  // seek, a loop, the playback's start in the middle of it): its moves up to
  // now skipped to the last of them, its first parameter set given and the
  // rest up to now skipped to the last of them.
  void StartReplayEffect(ReplayEffect const& e, __int64 context, double time, bool catchUp)
  {
    unsigned int handle = PlayReplayEffect(e, context);
    ++(handle ? g_ReplayFxPlayed : g_ReplayFxSkipped);
    if (!handle)
      return;
    if (util::DiagnosticsOn())
      ++g_ReplayFxPlayedBy[e.Blueprint];
    ActiveReplayEffect a{};
    size_t giveFirst = (size_t)-1;
    {
      std::lock_guard<std::mutex> lock(g_ReplayFxMutex);
      auto moves = g_ReplayFxMoveRanges.find(e.Id);
      auto params = g_ReplayFxParamRanges.find(e.Id);
      if (moves == g_ReplayFxMoveRanges.end() && params == g_ReplayFxParamRanges.end() && e.StopTime < 0.0)
        return;
      a.Handle = handle;
      a.Lasting = IsLastingEffect(e.Blueprint) || e.Static;
      if (moves != g_ReplayFxMoveRanges.end())
      {
        a.NextMove = moves->second.first;
        a.EndMove = (std::min)(moves->second.second, g_ReplayFxMoves.size());
      }
      if (params != g_ReplayFxParamRanges.end())
      {
        a.NextParams = params->second.first;
        a.EndParams = (std::min)(params->second.second, g_ReplayFxParams.size());
      }
      a.StopTime = e.StopTime;
      a.LastTime = e.Time;
      if (a.EndMove > a.NextMove)
        a.LastTime = (std::max)(a.LastTime, g_ReplayFxMoves[a.EndMove - 1].Time);
      if (a.EndParams > a.NextParams)
        a.LastTime = (std::max)(a.LastTime, g_ReplayFxParams[a.EndParams - 1].Time);
      if (catchUp)
      {
        while (a.NextMove + 1 < a.EndMove && g_ReplayFxMoves[a.NextMove + 1].Time <= time)
          ++a.NextMove;
        if (a.NextParams < a.EndParams && g_ReplayFxParams[a.NextParams].Time <= time)
        {
          giveFirst = a.NextParams++;
          while (a.NextParams + 1 < a.EndParams && g_ReplayFxParams[a.NextParams + 1].Time <= time)
            ++a.NextParams;
        }
      }
    }
    if (giveFirst != (size_t)-1)
      GiveReplayEffectParams(handle, giveFirst);
    g_ReplayFxActive.push_back(a);
  }
}

void __fastcall hEffectMove(__int64 manager, unsigned int handle, __int64 rows, __int64 a4)
{
  oEffectMove(manager, handle, rows, a4);
  if (!g_ReplayFxRecording.load(std::memory_order_relaxed) || !handle)
    return;
  ReplayEffectMove m{};
  if (!ReadEffectRows(rows, m.Rows))
    return;
  std::lock_guard<std::mutex> lock(g_ReplayFxMutex);
  if (!g_ReplayFxRecording.load(std::memory_order_relaxed))
    return;
  auto it = g_ReplayFxHandles.find(handle);
  if (it == g_ReplayFxHandles.end())
  {
    // One started before the recording: taken in here (its place now is
    // where it's played)
    unsigned int id = 0;
    if (!TakeInEarlierEffect(handle, m.Rows, id) && util::DiagnosticsOn())
      NoteEarlierEffectUpdate(handle, m.Rows, true);
    return;
  }
  unsigned int id = it->second;
  if (id >= g_ReplayFx.size())
    return;
  g_ReplayFx[id].LastUpdate = g_ReplayFxClock;
  // Not moved: left out; moved again within MinReplayEffectMoveInterval: the
  // move before takes its place
  auto last = g_ReplayFxLastMove.find(id);
  if (last != g_ReplayFxLastMove.end() && last->second < g_ReplayFxMoves.size())
  {
    ReplayEffectMove& before = g_ReplayFxMoves[last->second];
    if (!memcmp(before.Rows, m.Rows, sizeof(m.Rows)))
      return;
    if (g_ReplayFxClock - before.Time < MinReplayEffectMoveInterval)
    {
      memcpy(before.Rows, m.Rows, sizeof(m.Rows));
      return;
    }
  }
  if (g_ReplayFxMoves.size() >= MaxReplayEffectMoves)
  {
    ++g_ReplayFxMovesDropped;
    return;
  }
  m.Id = id;
  m.Time = g_ReplayFxClock;
  g_ReplayFxLastMove[id] = g_ReplayFxMoves.size();
  g_ReplayFxMoves.push_back(m);
}

void __fastcall hEffectStop(__int64 manager, unsigned int handle, __int64 a3, __int64 a4)
{
  oEffectStop(manager, handle, a3, a4);
  if (!handle)
    return;
  std::lock_guard<std::mutex> lock(g_ReplayFxMutex);
  g_FxHandleBlueprint.erase(handle);
  g_FxHandleBlueprintOld.erase(handle);
  g_LiveFx.erase(handle);
  g_LiveFxParams.erase(handle);
  if (!g_ReplayFxRecording.load(std::memory_order_relaxed))
    return;
  g_FxPendingParams.erase(handle);
  auto it = g_ReplayFxHandles.find(handle);
  if (it == g_ReplayFxHandles.end())
    return;
  if (it->second < g_ReplayFx.size())
    g_ReplayFx[it->second].StopTime = g_ReplayFxClock;
  g_ReplayFxHandles.erase(it);
}

void __fastcall hEffectParams(__int64 manager, unsigned int handle, __int64 list, __int64 a4)
{
  oEffectParams(manager, handle, list, a4);
  if (!handle)
    return;
  if (!g_ReplayFxRecording.load(std::memory_order_relaxed))
  {
    // A mission effect's, kept for when a recording starts while it plays
    // (see TakeInPlayingEffects)
    NoteLiveEffectParams(handle, list);
    return;
  }
  // Copied now (the list is the caller's, on its stack)
  static thread_local EffectParamsCopy copy;
  int read = ReadEffectParams(list, copy);
  std::lock_guard<std::mutex> lock(g_ReplayFxMutex);
  if (!g_ReplayFxRecording.load(std::memory_order_relaxed))
    return;
  auto it = g_ReplayFxHandles.find(handle);
  if (it == g_ReplayFxHandles.end())
  {
    if (util::DiagnosticsOn())
      NoteEarlierEffectUpdate(handle, nullptr, false);
    // One started before the recording, not taken in yet: its last ones kept
    // for when it is (see TakeInEarlierEffect)
    if (read > 0 && FindEffectBlueprint(handle) &&
        (g_FxPendingParams.size() < MaxPendingEffectParams || g_FxPendingParams.count(handle)))
      g_FxPendingParams[handle] = copy;
    return;
  }
  if (it->second >= g_ReplayFx.size())
    return;
  if (util::DiagnosticsOn())
    ++g_FxRecordedParams[g_ReplayFx[it->second].Blueprint];
  if (read < 0)
    ++g_ReplayFxParamsWithObjects;
  else if (read > 0)
    KeepReplayEffectParams(it->second, copy);
}

static bool IsEffectParamsCode(BYTE const* pBase)
{
  // test edx, edx; je ...; push rbp; push r13; push r14
  static const BYTE code[] = { 0x85, 0xD2, 0x0F, 0x84, 0x1B, 0x01, 0x00, 0x00, 0x55, 0x41, 0x55, 0x41, 0x56 };
  return !memcmp(pBase + EffectParamsRva, code, sizeof(code)) && pBase[EffectParamsCallRva] == 0xE8 &&
    EffectParamsCallRva + 5 + *(int const*)(pBase + EffectParamsCallRva + 1) == EffectParamsRva;
}

static bool IsEffectMoveStopCode(BYTE const* pBase)
{
  // test edx, edx; je ...; then push rbp / r14 / r15 (move) or mov [rsp + 0x20], rbp; push r14 (stop)
  static const BYTE move[] = { 0x85, 0xD2, 0x0F, 0x84, 0x11, 0x01, 0x00, 0x00, 0x55, 0x41, 0x56, 0x41, 0x57 };
  static const BYTE stop[] = { 0x85, 0xD2, 0x0F, 0x84, 0xF3, 0x00, 0x00, 0x00, 0x48, 0x89, 0x6C, 0x24, 0x20, 0x41, 0x56 };
  return !memcmp(pBase + EffectMoveRva, move, sizeof(move)) && !memcmp(pBase + EffectStopRva, stop, sizeof(stop)) &&
    pBase[EffectMoveCallRva] == 0xE8 && EffectMoveCallRva + 5 + *(int const*)(pBase + EffectMoveCallRva + 1) == EffectMoveRva &&
    pBase[EffectStopCallRva] == 0xE8 && EffectStopCallRva + 5 + *(int const*)(pBase + EffectStopCallRva + 1) == EffectStopRva;
}

void util::hooks::ReplayEffectsStartRecording()
{
  std::lock_guard<std::mutex> lock(g_ReplayFxMutex);
  g_ReplayFx.clear();
  g_ReplayFxMoves.clear();
  g_ReplayFxHandles.clear();
  g_ReplayFxMoveRanges.clear();
  g_ReplayFxLastMove.clear();
  g_ReplayFxParams.clear();
  g_ReplayFxParamBytes.clear();
  g_ReplayFxParamRanges.clear();
  g_ReplayFxLastParams.clear();
  g_FxPendingParams.clear();
  g_FxEarlierUpdates.clear();
  g_FxEarlierTaken.clear();
  g_FxRecordedParams.clear();
  g_ReplayFxEarlier = g_ReplayFxMovesDropped = g_ReplayFxParamsDropped = g_ReplayFxParamsWithObjects = 0;
  g_ReplayFxStatic = 0;
  g_ReplayFxClock = 0.0;
  if (oEffectPlay && g_EffectCenterOk.load(std::memory_order_relaxed))
    TakeInPlayingEffects();
  g_ReplayFxRecording.store(oEffectPlay != nullptr);
}

void util::hooks::ReplayEffectsRecordFrame(double time)
{
  std::lock_guard<std::mutex> lock(g_ReplayFxMutex);
  g_ReplayFxClock = time;
}

void util::hooks::ReplayEffectsStopRecording()
{
  if (!g_ReplayFxRecording.exchange(false))
    return;
  std::lock_guard<std::mutex> lock(g_ReplayFxMutex);
  // The ones still playing at the end (see ReplayEffectAliveAtEnd), before
  // the ids stop being places in g_ReplayFx (sorted below)
  double end = g_ReplayFxClock;
  int aliveAtEnd = 0;
  for (auto const& h : g_ReplayFxHandles)
  {
    if (h.second >= g_ReplayFx.size())
      continue;
    ReplayEffect& e = g_ReplayFx[h.second];
    if (e.StopTime >= 0.0)
      continue;
    if (e.LastUpdate >= end - ReplayEffectAliveAtEnd)
    {
      e.StopTime = end;
      ++aliveAtEnd;
    }
    else if (e.Static)
      e.StopTime = end;   // a mission effect taken in as it started (see TakeInPlayingEffects): on to the end
    else if (e.Earlier)
      e.StopTime = e.LastUpdate + EarlierEffectTail;
  }
  // Noted from several threads: in time order for playing
  std::stable_sort(g_ReplayFx.begin(), g_ReplayFx.end(), [](ReplayEffect const& a, ReplayEffect const& b) { return a.Time < b.Time; });
  // Each effect's moves together, in time order, and its parameter sets
  std::stable_sort(g_ReplayFxMoves.begin(), g_ReplayFxMoves.end(),
    [](ReplayEffectMove const& a, ReplayEffectMove const& b) { return a.Id != b.Id ? a.Id < b.Id : a.Time < b.Time; });
  g_ReplayFxMoveRanges.clear();
  for (size_t i = 0; i < g_ReplayFxMoves.size(); ++i)
  {
    auto& range = g_ReplayFxMoveRanges[g_ReplayFxMoves[i].Id];
    if (range.second == 0)
      range.first = i;
    range.second = i + 1;
  }
  std::stable_sort(g_ReplayFxParams.begin(), g_ReplayFxParams.end(),
    [](ReplayEffectParams const& a, ReplayEffectParams const& b) { return a.Id != b.Id ? a.Id < b.Id : a.Time < b.Time; });
  g_ReplayFxParamRanges.clear();
  for (size_t i = 0; i < g_ReplayFxParams.size(); ++i)
  {
    auto& range = g_ReplayFxParamRanges[g_ReplayFxParams[i].Id];
    if (range.second == 0)
      range.first = i;
    range.second = i + 1;
  }
  g_ReplayFxHandles.clear();
  g_ReplayFxLastMove.clear();
  g_ReplayFxLastParams.clear();
  g_FxPendingParams.clear();
  util::log::Write("Replay: recorded %d gun effects near Faith (muzzle flashes, tracers, impacts), %d of them started before the "
    "recording (taken in at their first move in it), %d moving (%d moves, %d more left out: no room), %d with parameters (%d sets, "
    "%.1f MB; %d holding an object and %d more left out: no room), %d still playing at the end",
    (int)g_ReplayFx.size(), g_ReplayFxEarlier, (int)g_ReplayFxMoveRanges.size(), (int)g_ReplayFxMoves.size(), g_ReplayFxMovesDropped,
    (int)g_ReplayFxParamRanges.size(), (int)g_ReplayFxParams.size(), g_ReplayFxParamBytes.size() / 1048576.0,
    g_ReplayFxParamsWithObjects, g_ReplayFxParamsDropped, aliveAtEnd);
  // Diagnostic (temporary): by kind, and the ones that were playing before
  if (!util::DiagnosticsOn())
    return;
  std::unordered_map<__int64, int> byKind;
  for (ReplayEffect const& e : g_ReplayFx)
    ++byKind[e.Blueprint];
  std::vector<std::pair<int, __int64>> kinds;
  for (auto const& k : byKind)
    kinds.push_back({ k.second, k.first });
  std::sort(kinds.begin(), kinds.end(), [](std::pair<int, __int64> const& a, std::pair<int, __int64> const& b) {
    return a.first > b.first; });
  for (size_t i = 0; i < kinds.size() && i < 40; ++i)
  {
    char name[128];
    ReadAssetName(kinds[i].second, name, sizeof(name));
    auto params = g_FxRecordedParams.find(kinds[i].second);
    util::log::Write("Replay (diagnostic):   recorded %d x %s (%d parameter sets on them)", kinds[i].first, name,
      params != g_FxRecordedParams.end() ? params->second : 0);
  }
  int earlierLogged = 0;
  for (auto const& u : g_FxEarlierUpdates)
  {
    if (earlierLogged++ >= 30)
      break;
    char name[128];
    ReadAssetName(u.first, name, sizeof(name));
    float distance = -1.f;
    if (u.second.HasLast && g_EffectCenterOk.load(std::memory_order_relaxed))
      distance = sqrtf((u.second.Last[0] - g_EffectCenter[0]) * (u.second.Last[0] - g_EffectCenter[0]) +
        (u.second.Last[1] - g_EffectCenter[1]) * (u.second.Last[1] - g_EffectCenter[1]) +
        (u.second.Last[2] - g_EffectCenter[2]) * (u.second.Last[2] - g_EffectCenter[2]));
    auto taken = g_FxEarlierTaken.find(u.first);
    util::log::Write("Replay (diagnostic):   started before the recording, updated while it recorded: %s, %d taken in, %d "
      "updates of ones not taken in (%d moves, %d parameter sets), last at %.1f %.1f %.1f (%.0f m from Faith at the end)", name,
      taken != g_FxEarlierTaken.end() ? taken->second : 0, u.second.Moves + u.second.Params, u.second.Moves, u.second.Params,
      u.second.Last[0], u.second.Last[1], u.second.Last[2], distance);
  }
}

void util::hooks::ReplayEffectsPlay(double time, bool jumped)
{
  if (!oEffectPlay)
    return;
  std::vector<ReplayEffect> due;
  __int64 context = 0;
  bool resync = false;
  // Going on after a pause: from the paused moment, as after a seek (see
  // ReplayEffectsHold, which left g_ReplayFxLast at -1)
  g_ReplayFxHolding = false;
  {
    std::lock_guard<std::mutex> lock(g_ReplayFxMutex);
    if (g_ReplayFx.empty())
      return;
    context = g_LiveEffectContext;
    resync = jumped || time < g_ReplayFxLast || g_ReplayFxLast < 0.0;
    if (resync)
    {
      // A seek, a loop or the start: from here on, the ones in between aren't
      // played, but the ones playing on at this time (stopped later, or still
      // playing at the end) are started here: the lasting ones (see
      // IsLastingEffect) whenever they started, the others (a shot's flash, a
      // blast, a tracer, a hit) only when they started just before. The game
      // lets a one-off's effect run on long after it's seen (2026-10-25 04:40:
      // a Shock Protector's Crippler discharge was never stopped, its muzzle
      // flashes ran 10 - 78 s, until its next shot), so dragging the replay's
      // Position slider fired the same old discharge 43 times in a second.
      g_ReplayFxNext = std::upper_bound(g_ReplayFx.begin(), g_ReplayFx.end(), time,
        [](double t, ReplayEffect const& e) { return t < e.Time; }) - g_ReplayFx.begin();
      for (size_t i = 0; i < g_ReplayFxNext; ++i)
        if (g_ReplayFx[i].StopTime > time && (time - g_ReplayFx[i].Time <= OneOffCatchUp ||
            IsLastingEffect(g_ReplayFx[i].Blueprint) || g_ReplayFx[i].Static))
          due.push_back(g_ReplayFx[i]);
    }
    else
      while (g_ReplayFxNext < g_ReplayFx.size() && g_ReplayFx[g_ReplayFxNext].Time <= time)
        due.push_back(g_ReplayFx[g_ReplayFxNext++]);
    g_ReplayFxLast = time;
    // A mission effect the game still plays where it was isn't doubled
    // (see TakeInPlayingEffects)
    size_t kept = 0;
    for (ReplayEffect const& e : due)
      if (IsStillPlayedByGame(e))
        ++g_ReplayFxGameStillPlays;
      else
        due[kept++] = e;
    due.resize(kept);
  }
  if (resync)
    UpdateActiveReplayEffects(time, true);
  if (!context)
    return;
  for (ReplayEffect const& e : due)
    StartReplayEffect(e, context, time, resync);
  UpdateActiveReplayEffects(time, false);
}

// While the playback is paused (2026-10-26: the KrugerSec's guns kept firing
// while a replay was paused). The replay's time stands still, but the effects
// it started go on in the game's time: a muzzle flash, a tracer or a blast the
// game stopped later in the recording kept playing, since that stop time never
// came. At the pause, the replay's effects that aren't lasting kinds (see
// IsLastingEffect: a VTOL's engines, a drone's lasers, Runner Vision's trail,
// the mission and ambient ones taken in as it started) are stopped; the lasting
// ones stay where they were. Going on (ReplayEffectsPlay) syncs again from the
// paused moment as after a seek: the lasting ones start again there, a one-off
// only when it started just before.
void util::hooks::ReplayEffectsHold(double time)
{
  if (!oEffectPlay)
    return;
  if (!g_ReplayFxHolding)
  {
    g_ReplayFxHolding = true;
    for (size_t i = 0; i < g_ReplayFxActive.size();)
    {
      if (g_ReplayFxActive[i].Lasting)
      {
        ++i;
        continue;
      }
      StopReplayEffect(g_ReplayFxActive[i].Handle);
      g_ReplayFxActive[i] = g_ReplayFxActive.back();
      g_ReplayFxActive.pop_back();
    }
    std::lock_guard<std::mutex> lock(g_ReplayFxMutex);
    g_ReplayFxLast = -1.0;
  }
  UpdateActiveReplayEffects(time, false);
}

void util::hooks::ReplayEffectsStop()
{
  g_ReplayFxHolding = false;
  UpdateActiveReplayEffects(0.0, true);
  if (g_ReplayFxLast >= 0.0 && (g_ReplayFxPlayed || g_ReplayFxSkipped))
    util::log::Write("Replay: %d gun effects played again (%d skipped: their blueprint went), %d parameter sets given (%d left out: "
      "their parameters went)", g_ReplayFxPlayed, g_ReplayFxSkipped, g_ReplayFxParamsGiven, g_ReplayFxParamsLeftOut);
  if (g_ReplayFxGameStillPlays)
    util::log::Write("Replay: %d times a mission effect wasn't played again: the game still plays its own where it was",
      g_ReplayFxGameStillPlays);
  g_ReplayFxGameStillPlays = 0;
  // Diagnostic (temporary): by kind
  if (util::DiagnosticsOn())
  {
    std::vector<std::pair<int, __int64>> kinds;
    for (auto const& k : g_ReplayFxPlayedBy)
      kinds.push_back({ k.second, k.first });
    std::sort(kinds.begin(), kinds.end(), [](std::pair<int, __int64> const& a, std::pair<int, __int64> const& b) {
      return a.first > b.first; });
    for (size_t i = 0; i < kinds.size() && i < 40; ++i)
    {
      char name[128];
      ReadAssetName(kinds[i].second, name, sizeof(name));
      util::log::Write("Replay (diagnostic):   played %d x %s", kinds[i].first, name);
    }
  }
  g_ReplayFxPlayedBy.clear();
  g_ReplayFxLast = -1.0;
  g_ReplayFxNext = 0;
  g_ReplayFxPlayed = g_ReplayFxSkipped = 0;
  g_ReplayFxParamsGiven = g_ReplayFxParamsLeftOut = 0;
}

namespace
{
  // While a replay plays: a sound the game plays as an effect, a bullet's
  // impact or a thing hitting or scraping something (ME_Sound/Materials/...,
  // all from 0x14329BCD0: the 2026-10-25 logs), within LiveLeaveOutRadius of
  // the real Faith, isn't played. 0x14329BCD0 hands the handle to the
  // effect's parameters (0x14329F850) and a scrape's tracker (0x1432C0120,
  // which keeps it for moving and stopping the scrape), and the parameters,
  // move and stop functions all pass a handle of 0 by (test edx, edx at
  // their starts), as for an effect the game couldn't play. The replay's own
  // (past the hook) aren't touched; nor are the world's sounds played as
  // effects otherwise (ME_Sound/World/...: a prop's air duct, from
  // 0x14328F7B0), which may be started once.
  std::unordered_map<__int64, bool> g_LiveSoundKinds;   // blueprint -> one of those; under g_ReplayFxMutex

  bool IsLiveSoundNearFaith(__int64 blueprint, float const* transform)
  {
    if (!g_EffectCenterOk.load(std::memory_order_relaxed))
      return false;
    float place[3];
    if (!ReadEffectPlace(transform, place))
      return false;
    float dx = place[0] - g_EffectCenter[0], dy = place[1] - g_EffectCenter[1], dz = place[2] - g_EffectCenter[2];
    if (dx * dx + dy * dy + dz * dz > LiveLeaveOutRadius * LiveLeaveOutRadius)
      return false;
    std::lock_guard<std::mutex> lock(g_ReplayFxMutex);
    auto kind = g_LiveSoundKinds.find(blueprint);
    if (kind == g_LiveSoundKinds.end())
    {
      char name[128];
      ReadAssetName(blueprint, name, sizeof(name));
      kind = g_LiveSoundKinds.emplace(blueprint, !strncmp(name, "ME_Sound/Materials/", 19)).first;
    }
    return kind->second;
  }
}

namespace
{
  // Faith sliding (2026-10-26, the user: "also faith sliding should create a
  // slide ripple"): while she slides the game plays FX/Movement/FX_Slide_*
  // at her feet (its effect grid's slide against the surface, several a
  // second: the effects diagnostic, 2026-10-26 05:19:26, 30 FX_Slide_Clean
  // in 5 s, 0.3 m from her). Each one within SlideReach of her goes to the
  // wet look, which rings the puddle there (wetlook::NoteSlide). Not while a
  // replay plays (the real Faith stands still then).
  const float SlideReach = 2.f;
  std::mutex g_SlideKindsMutex;
  std::unordered_map<__int64, bool> g_SlideKinds;   // blueprint -> a slide's; under g_SlideKindsMutex

  void NoteSlideEffect(__int64 blueprint, float const* transform)
  {
    if (!transform || !g_EffectCenterOk.load(std::memory_order_relaxed) || g_ReplayPlaying.load(std::memory_order_relaxed))
      return;
    float place[3];
    if (!ReadEffectPlace(transform, place))
      return;
    float dx = place[0] - g_EffectCenter[0], dy = place[1] - g_EffectCenter[1], dz = place[2] - g_EffectCenter[2];
    if (dx * dx + dy * dy + dz * dz > SlideReach * SlideReach)
      return;
    {
      std::lock_guard<std::mutex> lock(g_SlideKindsMutex);
      auto kind = g_SlideKinds.find(blueprint);
      if (kind == g_SlideKinds.end())
      {
        char name[128];
        ReadAssetName(blueprint, name, sizeof(name));
        kind = g_SlideKinds.emplace(blueprint, !_strnicmp(name, "FX/Movement/FX_Slide", 20)).first;
      }
      if (!kind->second)
        return;
    }
    wetlook::NoteSlide(place);
  }
}

unsigned int __fastcall hEffectPlay(__int64 manager, __int64 blueprint, float const* transform, __int64 a4, __int64 a5,
  __int64 a6, __int64 a7, __int64 a8, __int64 a9, __int64 a10)
{
  if (g_ReplayPlaying.load(std::memory_order_relaxed) && transform && IsLiveSoundNearFaith(blueprint, transform))
  {
    g_LiveEffectSoundsLeftOut.fetch_add(1, std::memory_order_relaxed);
    return 0;
  }
  unsigned int handle = oEffectPlay(manager, blueprint, transform, a4, a5, a6, a7, a8, a9, a10);
  if (util::DiagnosticsOn())
    NoteEffect(blueprint, transform, (__int64)_ReturnAddress(), handle, a4, a5, a6, a7, a8);
  if (handle)
    NoteSlideEffect(blueprint, transform);
  if (handle)
  {
    NoteReplayEffect(blueprint, transform, a4, a7, handle);
    TrackEffectHandle(blueprint, handle);
    NoteLiveEffect(blueprint, transform, a7, handle);
  }
  return handle;
}

static bool IsEffectPlayCode(BYTE const* pBase)
{
  // mov rax, rsp; mov [rax + 0x18], r8; mov [rax + 0x10], rdx; push rbp rsi rdi r12 r13 r14 r15; sub rsp, 0x50;
  // mov [rax - 0x58], -2
  static const BYTE code[] = { 0x48, 0x89, 0xE0, 0x4C, 0x89, 0x40, 0x18, 0x48, 0x89, 0x50, 0x10, 0x55, 0x56, 0x57, 0x41, 0x54,
    0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x83, 0xEC, 0x50, 0x48, 0xC7, 0x40, 0xA8 };
  return !memcmp(pBase + EffectPlayRva, code, sizeof(code)) && pBase[EffectPlayCallRva] == 0xE8 &&
    EffectPlayCallRva + 5 + *(int const*)(pBase + EffectPlayCallRva + 1) == EffectPlayRva;
}

void util::hooks::UpdateEffectLog(float const* faith)
{
  if (!oEffectPlay)
    return;
  if (faith)
    memcpy(g_EffectCenter, faith, sizeof(g_EffectCenter));
  g_EffectCenterOk.store(faith != nullptr, std::memory_order_relaxed);
  if (!util::DiagnosticsOn())   // the rest is the effects log
    return;
  double now = GetTickCount64() / 1000.0;
  if (now - g_EffectLogAt < EffectLogInterval)
    return;
  g_EffectLogAt = now;
  std::vector<std::pair<__int64, EffectSeen>> seen;
  int farCount = 0;
  {
    std::lock_guard<std::mutex> lock(g_EffectLogMutex);
    seen.assign(g_EffectsSeen.begin(), g_EffectsSeen.end());
    g_EffectsSeen.clear();
    farCount = g_EffectsFar;
    g_EffectsFar = 0;
  }
  if (seen.empty())
    return;
  std::sort(seen.begin(), seen.end(), [](auto const& a, auto const& b) { return a.second.Count > b.second.Count; });
  util::log::Write("Effects (diagnostic): %d kinds played within %.0f m of Faith in the last %.0f s (%d farther away):",
    (int)seen.size(), EffectLogRadius, EffectLogInterval, farCount);
  int shown = 0;
  for (auto const& s : seen)
  {
    if (++shown > 25)
      break;
    char name[128];
    ReadAssetName(s.first, name, sizeof(name));
    util::log::Write("Effects (diagnostic):   %dx %s (blueprint 0x%I64X)%s, last at %.2f %.2f %.2f, %.1f m from Faith, from 0x%I64X",
      s.second.Count, name, s.first, s.second.NotPlayed ? " (some not played)" : "", s.second.Last[0], s.second.Last[1],
      s.second.Last[2], sqrtf((s.second.Last[0] - g_EffectCenter[0]) * (s.second.Last[0] - g_EffectCenter[0]) +
        (s.second.Last[1] - g_EffectCenter[1]) * (s.second.Last[1] - g_EffectCenter[1]) +
        (s.second.Last[2] - g_EffectCenter[2]) * (s.second.Last[2] - g_EffectCenter[2])), s.second.Caller);
    // Its arguments, once a blueprint
    bool logArgs = false;
    {
      std::lock_guard<std::mutex> lock(g_EffectLogMutex);
      logArgs = s.second.HaveArgs && g_EffectArgsLogged.insert(s.first).second;
    }
    if (logArgs)
    {
      char hex1[0x40 * 3 + 1], hex2[0x40 * 3 + 1];
      for (int k = 0; k < 0x40; ++k)
      {
        sprintf_s(hex1 + k * 3, 4, "%02X ", s.second.Bytes1[k]);
        sprintf_s(hex2 + k * 3, 4, "%02X ", s.second.Bytes2[k]);
      }
      util::log::Write("Effects (diagnostic):     %s: r9 0x%I64X (vtable 0x%I64X), int %d, bool %d; struct 1 at 0x%I64X: %s; struct 2 "
        "at 0x%I64X: %s", name, s.second.Context, s.second.ContextVTable, s.second.Number, s.second.Flag, s.second.Struct1, hex1,
        s.second.Struct2, hex2);
    }
  }
}

// VISUALS > Weather > Wet surfaces: Faith's footsteps in the puddles drawn
// there are the game's own steps in water (2026-10-25, offline; see
// Research_Weather_2026-10-25.md section 21). A foot impact of her character
// (FootPlantLeft / Right, the landings: events of her animation) goes to two
// entities of Characters/FaithCharacter linked to her AntAnimatableEntity:
// a PamMovementImpactSoundEntity (0x143924CE0, its only vtable slot at
// 0x141C3F520) gives Faith_Movement_Audio's footplant sound the surface's
// MaterialPropertySoundData.MaterialSoundId as SoundMaterialId, and a
// PamMovementImpactEffectEntity (0x143924A90, slot at 0x141C3F3E0) plays the
// material grid's effect for the foot's material against the surface's
// (FX/Movement/FX_Foot_*). Both take the surface from 0x1439AE870, they're its
// only callers: (their owner's + 0x158, an id, out, the bone's place, two
// axes or none) -> true, and at out the surface's MaterialDecl, then a
// transform of the contact: its point at + 0x40, its normal at + 0x20 (axes
// given, the effect's) or + 0x30 (none, the sound's). A MaterialDecl's byte 2
// is its material in the grid (MaterialGridData.MaterialIndexMap) for its
// sound data and its effects alike; the city's grid has the water at 16: its
// sound id 60 is the footplant's me_faith_water_shallow_* waves, and against
// the steps' and landings' foot materials it plays FX_Foot_Water,
// FX_FootLandSoft_Water and FX_FootLandHard_Water. So for Faith's own steps
// and landings whose contact is in a drawn puddle (wetlook::InPuddle) the
// surface's byte 2 is made 16, when the game's own lookup (0x143282B80, the
// sound entity's, on [[0x142401CB0] + 0x38]) says 16's sound id is 60.
// The game's own puddle decals (objects/decals/waterpuddle) collide as the
// wet concrete, map 47, with the same water effects: her steps on them
// splash already. Where her step's effect is either, the wet look lets the
// game's splash through (wetlook::NoteHerSplash; 2026-10-27, the user: "also
// on puddles use faiths splash fx when she steps on in-game puddles").
// VISUALS > Snow (2026-10-26, the user: "for the snow in the ground can we
// have a snow walking sound effect?"): the game has no snow sounds; the
// nearest is its gravel's crunch, sound id 5 in Faith_Footplant's wave
// switchers (me_footsteps_gravel_walk / run / sprint / land_heavy /
// slide_*, sprint_release). The city's grid has it at 10 (grid 8; 75 too).
// For her steps and landings in the snow drawn on the ground
// (wetlook::InSnow) the sound's surface (the call without axes) gets byte 2
// 10, when the lookup says 10's sound id is 5. Not the effect's: the gravel's
// is pebbles and dust (FX_Foot_Gravel), wrong on snow.
typedef bool(__fastcall* tImpactSurface)(__int64, __int64, unsigned int*, float const*, float const*, float const*);
typedef __int64(__fastcall* tImpactHandler)(__int64, __int64, __int64, __int64);
typedef __int64(__fastcall* tMaterialSoundData)(__int64, unsigned int*);
tImpactSurface oImpactSurface = nullptr;
tImpactHandler oImpactSoundHandler = nullptr;
tImpactHandler oImpactEffectHandler = nullptr;
const __int64 ImpactSurfaceRva = 0x39AE870;
const __int64 ImpactSoundHandlerRva = 0x3924CE0;
const __int64 ImpactEffectHandlerRva = 0x3924A90;
const __int64 ImpactSoundHandlerSlotRva = 0x1C3F520;
const __int64 ImpactEffectHandlerSlotRva = 0x1C3F3E0;
const __int64 ImpactSoundSurfaceCallRva = 0x3924DA8;     // the handlers' calls of 0x1439AE870
const __int64 ImpactEffectSurfaceCallRva = 0x3924BBA;
const __int64 ImpactSoundHolderLoadRva = 0x3924DB9;      // mov rax, [rip + x]: the material manager's holder
const __int64 ImpactSoundLookupCallRva = 0x3924DE3;      // its call of 0x143282B80
const __int64 MaterialSoundDataRva = 0x3282B80;
const __int64 MaterialHolderRva = 0x2401CB0;
const int MaterialManagerOffset = 0x38;
const int MaterialManagerGridOffset = 0x08;              // MaterialGridData
const int MaterialGridIndexMapOffset = 0x28;             // MaterialIndexMap (its count 4 bytes before it)
// MaterialPropertySoundData.MaterialSoundId, a float (its type is Softness's
// and ScrapeLength's; the 2026-10-26 00:15 log read 16's as 0x42700000, 60.0)
const int MaterialSoundIdOffset = 0x34;
const unsigned int WaterMaterial = 16;
const float WaterSoundId = 60.f;
// Faith's steps on the wet ground outside, off the puddles (2026-10-26, the
// user: "make the water footsteps work outside as well when there is no
// peddles"), as VISUALS > Weather > Wet surfaces > Steps on the wet ground
// has it: the water above, or the game's wet concrete, map 47 (grid 34): sound
// id 61, concrete with a me_footplant_water_slow layer, and the water's effects
const unsigned int WetConcreteMaterial = 47;
const float WetConcreteSoundId = 61.f;
const unsigned int SnowStepMaterial = 10;    // the gravel (see above)
const float SnowStepSoundId = 5.f;
const unsigned int MaterialValid = 0x8000;
// The impacts whose surface may be the water: Faith's steps and landings
// (ImpactId, Characters/FaithCharacter's PamMovementImpactSound / Effect
// EntityData), with her feet's bones (LeftFoot, RightFoot). The sound data
// has the id at + 0x08 and the bone at + 0x18, the effect data at + 0x20 and
// + 0x10 (the handlers' own reads of the bone).
const int FootImpactIds[] = {
  -313593505,    // FootPlantLeft
  -1629160350,   // FootPlantRight
  6266043,       // FootPlantLandSoft
  -727632327,    // FootPlantLandingSoft (the effect's)
  1331134984,    // FootPlantLandHard
  // Her steps on a wall she runs along (Characters/FaithCharacter's impacts,
  // the sound's and the effect's alike, on the same bones; 2026-10-26, the
  // user: "Footsteps should appear on walls as well if faith wallrides"):
  // their surface goes to wetlook::NoteWallStep (her print there) only
  -1048844861,   // FootPlantWallLeft
  1278658430     // FootPlantWallRight
};
const int FootBones[] = { 167, 175 };
// KSec's and the game's AI people's feet (Characters/AINpc and
// AINpc_Spec_Enemy: their PamMovementImpactSoundEntityData has the same
// FootPlantLeft / Right / Wall / LandHard ids as Faith's, on their
// skeleton's LeftFoot 146 and RightFoot 155; no effect entity): their steps
// ring the puddles (wetlook::OtherStep, 2026-10-26, the user: "Research how
// can ksec and other people leave ripples")
const int OtherFootBones[] = { 146, 155 };
const int ImpactSoundOwnerOffset = 0x08;    // the handlers' own reads (their owner's + 0x158 goes to 0x1439AE870)
const int ImpactEffectOwnerOffset = 0x10;
// Faith's animatables (the replay's, see FaithReplay.cpp): her soldier's
// + 0x248 and + 0x340, and her recording component's (+ 0x4B0) + 0x50
const int SoldierBodyAnimatableOffset = 0x248;
const int SoldierBoxAnimatableOffset = 0x340;
const int SoldierRecordingComponentOffset = 0x4B0;
const int RecordingComponentAnimatableOffset = 0x50;
const float FaithFeetReach = 3.f;            // a contact of hers is within this of where she is (m)
// Her hands' touches (2026-10-26, the snow session; the user: "faiths hands
// should also leave handprints behind"): Characters/FaithCharacter's hand
// impacts (their ImpactIds are the FNV-1 hashes of these names, checked
// against FootPlantLeft's -313593505): HandLand (both), HandLandLeft / Right,
// HandClimbLeft / Right, HandScuffLeft / Right (not the releases: the hand
// leaving), on her hands' bones (LeftHand 15, RightHand 123). Their surface
// goes to wetlook::NoteHand (a print where the snow lies).
const int HandImpactIds[] = {
  481714765,     // HandLand
  86534698,      // HandLandLeft
  -938062431,    // HandLandRight
  -792823598,    // HandClimbLeft
  -1011724695,   // HandClimbRight
  -1872667002,   // HandScuffLeft
  -1677175227    // HandScuffRight
};
const int HandBones[] = { 15, 123 };
// Her roll (the user: "Rolling/jump rolling should also leave faiths shape on
// the snow"): the same character's SkillrollForward, an effect's impact only
// (PamMovementImpactEffectEntityData, its bone LeftFoot's), the game's one
// impact for a roll. Its surface goes to wetlook::NoteRoll.
const int RollImpactId = -862150820;

namespace
{
  // Inside one of the handlers for a step or landing: whose (its owner)
  thread_local bool t_FootImpact = false;
  thread_local int t_HandSide = 0;   // inside one for a hand's touch: 1 her right, -1 her left
  thread_local bool t_RollImpact = false;   // inside one for her roll
  thread_local int t_FootKind = 0;   // see FootImpactKind
  thread_local int t_FootSide = 0;   // which foot: 1 her right (RightFoot, bone 175), -1 her left (LeftFoot, 167)
  thread_local bool t_OtherFeet = false;   // the step is another's (OtherFootBones), not hers
  thread_local __int64 t_FootOwner = 0;
  // Faith's soldier and animatables, and where she is (camera hook; a torn
  // read only misjudges one step)
  std::atomic<__int64> g_FaithFeetOwners[4];
  std::atomic<bool> g_FaithFeetPlaceOk{ false };
  float g_FaithFeetPlace[3];
  std::atomic<int> g_WetSteps{ 0 };
  std::atomic<bool> g_FaithFeetSeen{ false };
  std::atomic<bool> g_OtherFeetLogged{ false };
  std::atomic<__int64> g_WaterCheckedManager{ 0 };
  std::atomic<bool> g_WaterOk{ false };
  std::atomic<__int64> g_WetConcreteCheckedManager{ 0 };
  std::atomic<bool> g_WetConcreteOk{ false };
  std::atomic<ULONGLONG> g_WetStepLoggedAt{ 0 };
  std::atomic<int> g_SnowSteps{ 0 };
  std::atomic<__int64> g_SnowCheckedManager{ 0 };
  std::atomic<bool> g_SnowOk{ false };
  std::atomic<ULONGLONG> g_SnowStepLoggedAt{ 0 };

  // A step or landing of the feet's (FootImpactIds, a foot's bone): 1 a step,
  // 2 a soft landing, 3 a hard one, 4 a step on a wall; 0 none. foot: which (see t_FootSide;
  // MEC_Reverse_Notes' skeleton: 167 LeftFoot, 175 RightFoot)
  int FootImpactKind(__int64 data, int idOffset, int boneOffset, int* foot, bool* other)
  {
    *foot = 0;
    *other = false;
    __try
    {
      int id = *(int const*)(data + idOffset), bone = *(int const*)(data + boneOffset);
      *other = bone == OtherFootBones[0] || bone == OtherFootBones[1];
      if (bone != FootBones[0] && bone != FootBones[1] && !*other)
        return 0;
      *foot = bone == FootBones[1] || bone == OtherFootBones[1] ? 1 : -1;
      for (int i = 0; i < (int)(sizeof(FootImpactIds) / sizeof(FootImpactIds[0])); ++i)
        if (id == FootImpactIds[i])
          return i < 2 ? 1 : i < 4 ? 2 : i < 5 ? 3 : 4;
      return 0;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      return 0;
    }
  }

  // A hand's touch (HandImpactIds, a hand's bone): 1 her right hand, -1 her
  // left; 0 none
  int HandImpactSide(__int64 data, int idOffset, int boneOffset)
  {
    __try
    {
      int id = *(int const*)(data + idOffset), bone = *(int const*)(data + boneOffset);
      if (bone != HandBones[0] && bone != HandBones[1])
        return 0;
      for (int handId : HandImpactIds)
        if (id == handId)
          return bone == HandBones[1] ? 1 : -1;
      return 0;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      return 0;
    }
  }

  // Her roll's impact (RollImpactId)
  bool IsRollImpact(__int64 data, int idOffset)
  {
    __try
    {
      return *(int const*)(data + idOffset) == RollImpactId;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      return false;
    }
  }

  __int64 ReadOwner(__int64 at)
  {
    __try
    {
      return *(__int64 const*)at;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      return 0;
    }
  }

  // A surface's sound id by the game's own lookup (-1: none, not known)
  float MaterialSoundId(unsigned int material)
  {
    __int64 base = (__int64)g_gameHandle;
    __try
    {
      __int64 holder = *(__int64 const*)(base + MaterialHolderRva);
      __int64 manager = holder ? *(__int64 const*)(holder + MaterialManagerOffset) : 0;
      if (!manager)
        return -1.f;
      // Only an index its map has (the lookup doesn't check)
      __int64 grid = *(__int64 const*)(manager + MaterialManagerGridOffset);
      __int64 map = grid ? *(__int64 const*)(grid + MaterialGridIndexMapOffset) : 0;
      if (!map || ((material >> 16) & 0xFF) >= *(unsigned int const*)(map - 4))
        return -1.f;
      unsigned int copy = material;
      __int64 sound = ((tMaterialSoundData)(base + MaterialSoundDataRva))(manager, &copy);
      return sound ? *(float const*)(sound + MaterialSoundIdOffset) : -1.f;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      return -1.f;
    }
  }

  __int64 MaterialManager()
  {
    __try
    {
      __int64 holder = *(__int64 const*)((__int64)g_gameHandle + MaterialHolderRva);
      return holder ? *(__int64 const*)(holder + MaterialManagerOffset) : 0;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      return 0;
    }
  }

  // Which ground a surface is, for her prints pressed into the wet ground
  // (wetlook::StepWater's surface): its sound id (MaterialSoundId) in
  // Faith_Footplant's wave switchers (their wave names, the snow session's
  // dump): concrete 0 - 3, 6 - 9, 30, 31, 33, 35 - 39, 42 - 49, 61 (the wet
  // concrete), marble 4: 1; gravel 5: 2; glass 10 - 19, metal 20 - 29, tarp
  // 32, bin bags 34, mattresses 40 - 41, wood 50 - 59, water 60: 0. Not
  // known (no material manager): 1, the city's most.
  int GroundKind(float soundId)
  {
    if (!(soundId >= 0.f))
      return 1;
    int id = (int)(soundId + 0.5f);
    if (id == 5)
      return 2;
    if (id <= 9 || id == 30 || id == 31 || id == 33 || (id >= 35 && id <= 39) || (id >= 42 && id <= 49) || id == 61)
      return 1;
    return 0;
  }

  // The contact the surface lookup wrote: its point and its normal's y
  // The contact's whole normal (the rain's handprints on a wall need its
  // plane): at + 0x20 with axes given (the effect's call), + 0x30 without
  // (the sound's), its y what ReadContact gives as up
  bool ReadContactNormal(unsigned int const* out, bool axesGiven, float* normal)
  {
    __try
    {
      memcpy(normal, (BYTE const*)out + (axesGiven ? 0x20 : 0x30), 3 * sizeof(float));
      return std::isfinite(normal[0] + normal[1] + normal[2]);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      return false;
    }
  }

  bool ReadContact(unsigned int const* out, bool axesGiven, float* point, float& up)
  {
    __try
    {
      BYTE const* p = (BYTE const*)out;
      memcpy(point, p + 0x40, 3 * sizeof(float));
      up = *(float const*)(p + (axesGiven ? 0x24 : 0x34));
      return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      return false;
    }
  }

  // A step's sound in the snow on the ground: the level's gravel for the
  // surface (byte 2; an unset surface given it whole). Not on glass or metal:
  // their own sound stays (sound ids 10 - 19, every one of Faith_Footplant's
  // wave switchers' glass waves, me_footstep_glasspane* / me_footsteps_glass_*,
  // and 20 - 29, its metal ones, me_footstep_metal* / me_footsteps_metal*;
  // the snow session's dump, steps\waveids.txt. 2026-10-27, the user: "Here is
  // a list where the snow footsteps sound shouldn't be played: Glass / Glass
  // panes, Metal objects"; CT.log 17:38 had her snow steps crunch on sound ids
  // 11, 13, 24, 26, 27 and 29.)
  const float SnowStepKeptFirst = 10.f, SnowStepKeptLast = 29.f;

  void SnowStep(unsigned int* out, float const* point)
  {
    unsigned int surface = *out;
    float own = MaterialSoundId(surface);
    if (own >= SnowStepKeptFirst - 0.5f && own <= SnowStepKeptLast + 0.5f)
      return;
    unsigned int gravel = (surface & MaterialValid) ? (surface & 0xFF00FFFF) | (SnowStepMaterial << 16) :
      (SnowStepMaterial << 24) | (SnowStepMaterial << 16) | MaterialValid;
    if (gravel == surface)
      return;
    // The level's gravel, checked once per material grid
    __int64 manager = MaterialManager();
    if (!manager)
      return;
    if (g_SnowCheckedManager.exchange(manager) != manager)
    {
      float id = MaterialSoundId(gravel);
      g_SnowOk = id == SnowStepSoundId;
      if (id == SnowStepSoundId)
        util::log::Write("Snow: the level's material %u is the gravel (sound id %g): Faith's steps in the snow crunch",
          SnowStepMaterial, id);
      else
        util::log::Warning("Snow: the level's material %u isn't the gravel (sound id %g, not %g): Faith's steps in the snow "
          "stay as they are", SnowStepMaterial, id, SnowStepSoundId);
    }
    if (!g_SnowOk.load())
      return;
    int count = ++g_SnowSteps;
    ULONGLONG now = GetTickCount64();
    ULONGLONG last = g_SnowStepLoggedAt.load();
    if (now - last > 5000 && g_SnowStepLoggedAt.compare_exchange_strong(last, now))
      util::log::Write("Snow: Faith's step at %.2f %.2f %.2f is in the snow: the gravel's sound (material %u) for the "
        "surface's (material %u, sound id %g); %d so far", point[0], point[1], point[2], SnowStepMaterial,
        (surface >> 16) & 0xFF, own, count);
    *out = gravel;
  }

  // A step of hers in a puddle: the water. On the wet ground outside, off
  // the puddles: as Steps on the wet ground has it (see WetConcreteMaterial)
  void WaterStep(unsigned int* out, bool axesGiven, float const* point, float up)
  {
    int wetness = wetlook::StepWater(point, up, t_FootKind, t_FootSide, GroundKind(MaterialSoundId(*out)));
    if (wetness == 0)
      return;
    unsigned int material = WaterMaterial;
    if (wetness == 1)
    {
      int choice = weather::WetGroundSteps();
      if (choice == 2)
        material = WetConcreteMaterial;
      else if (choice != 1)
        return;
    }
    bool concrete = material == WetConcreteMaterial;
    unsigned int surface = *out;
    unsigned int water = (surface & MaterialValid) ? (surface & 0xFF00FFFF) | (material << 16) :
      (material << 24) | (material << 16) | MaterialValid;
    if (water == surface)
      return;
    // The level's water or wet concrete, each checked once per material grid
    __int64 manager = MaterialManager();
    if (!manager)
      return;
    std::atomic<__int64>& checked = concrete ? g_WetConcreteCheckedManager : g_WaterCheckedManager;
    std::atomic<bool>& ok = concrete ? g_WetConcreteOk : g_WaterOk;
    float wantId = concrete ? WetConcreteSoundId : WaterSoundId;
    char const* what = concrete ? "the wet concrete" : "the water";
    if (checked.exchange(manager) != manager)
    {
      float id = MaterialSoundId(water);
      ok = id == wantId;
      if (id == wantId)
        util::log::Write("Wet surfaces: the level's material %u is %s (sound id %g)", material, what, id);
      else
        util::log::Warning("Wet surfaces: the level's material %u isn't %s (sound id %g, not %g): Faith's steps %s stay as "
          "they are", material, what, id, wantId, concrete ? "on the wet ground" : "in the water");
    }
    if (!ok.load())
      return;
    int count = ++g_WetSteps;
    ULONGLONG now = GetTickCount64();
    ULONGLONG last = g_WetStepLoggedAt.load();
    if (now - last > 5000 && g_WetStepLoggedAt.compare_exchange_strong(last, now))
      util::log::Write("Wet surfaces: Faith's %s at %.2f %.2f %.2f is %s: %s (material %u) for the surface's "
        "(material %u, sound id %g); %d so far", axesGiven ? "step's effect" : "step's sound", point[0], point[1], point[2],
        wetness == 2 ? "in a puddle" : "on the wet ground", what, material, (surface >> 16) & 0xFF, MaterialSoundId(surface),
        count);
    *out = water;
  }

  // Whether the game plays the water's splash for a surface (her step's
  // effect: FX_Foot_Water, the landings'): the water (WaterMaterial) and the
  // wet concrete (WetConcreteMaterial, the game's puddle decals' collision),
  // each when the level's lookup says it's that (its sound id)
  bool SplashesWater(unsigned int surface)
  {
    if (!(surface & MaterialValid))
      return false;
    unsigned int material = (surface >> 16) & 0xFF;
    if (material == WaterMaterial)
      return MaterialSoundId(surface) == WaterSoundId;
    if (material == WetConcreteMaterial)
      return MaterialSoundId(surface) == WetConcreteSoundId;
    return false;
  }

  void WetStep(unsigned int* out, bool axesGiven)
  {
    float point[3], up;
    if (!ReadContact(out, axesGiven, point, up))
      return;
    // Another's step or landing (KSec's, the game's AI people's): its ring
    // in a puddle, nothing else (its sound and effect as they are; not a
    // step on a wall)
    if (t_OtherFeet)
    {
      if (t_FootKind >= 1 && t_FootKind <= 3)
      {
        wetlook::OtherStep(point, up, t_FootKind, false);
        // (and in the snow on the ground, their boot's print: once a step,
        // from its sound's call; the snow session's, 2026-10-27, the user:
        // "Ksec should leave behind snow footsteps")
        if (!axesGiven)
          wetlook::OtherSnowStep(point, up, t_FootSide);
      }
      return;
    }
    __int64 owner = t_FootOwner;
    bool hers = owner != 0;
    if (hers)
    {
      hers = false;
      for (auto const& o : g_FaithFeetOwners)
        hers |= o.load(std::memory_order_relaxed) == owner;
    }
    float d = 1e9f;
    if (g_FaithFeetPlaceOk.load(std::memory_order_relaxed))
    {
      float dx = point[0] - g_FaithFeetPlace[0], dy = point[1] - g_FaithFeetPlace[1], dz = point[2] - g_FaithFeetPlace[2];
      d = sqrtf(dx * dx + dy * dy + dz * dz);
    }
    if (!hers)
    {
      // Not one of hers: said once if it was at her feet anyway (then who's
      // it from wants looking at)
      if (d < 1.f && !g_OtherFeetLogged.exchange(true))
        util::log::Warning("Wet surfaces: a foot's impact %.2f m from Faith came from 0x%I64X, not her soldier (0x%I64X) nor her "
          "animatables (0x%I64X, 0x%I64X, 0x%I64X): only hers sound like water in the puddles", d, owner,
          g_FaithFeetOwners[0].load(), g_FaithFeetOwners[1].load(), g_FaithFeetOwners[2].load(), g_FaithFeetOwners[3].load());
      return;
    }
    if (!g_FaithFeetSeen.exchange(true))
      util::log::Write("Wet surfaces: Faith's steps seen (from 0x%I64X, %.2f m from her), the ones in the puddles sound like water",
        owner, d);
    if (d > FaithFeetReach)
      return;
    // A step on a wall she runs along: her print there, nothing else (its
    // sound and effect as they are)
    if (t_FootKind == 4)
    {
      float normal[3];
      if (ReadContactNormal(out, axesGiven, normal))
        wetlook::NoteWallStep(point, normal, t_FootSide);
      return;
    }
    // In the snow on the ground: the gravel, for the sound only (see above).
    // (The wet look is off while snow is chosen, so never both.)
    if (!axesGiven && wetlook::InSnow(point, up, t_FootSide))
    {
      // (InSnow also leaves her print there; the crunch only with VISUALS >
      // Snow's Footstep sound in the snow ticked)
      if (weather::SnowStepSound())
        SnowStep(out, point);
      return;
    }
    // In a puddle: the water. On the wet ground outside, off the puddles: as
    // Steps on the wet ground has it (see WaterStep)
    unsigned int asTheGame = *out;
    WaterStep(out, axesGiven, point, up);
    // Her own splash, when the game plays the water's for this step or
    // landing (its effect's call, with axes): the wet look lets it through
    // (wetlook::NoteHerSplash); on the game's own water (its puddle decals,
    // the surface as the game gave it), a ring there too
    if (axesGiven && t_FootKind >= 1 && t_FootKind <= 3 && SplashesWater(*out))
      wetlook::NoteHerSplash(point, t_FootKind, (*out >> 16) & 0xFF, asTheGame == *out || SplashesWater(asTheGame));
  }

  // One of her hands' touches (t_HandSide) or her roll (t_RollImpact): its
  // print where the snow lies (the surface itself isn't changed)
  void HandTouch(unsigned int const* out, bool axesGiven)
  {
    float point[3], up;
    if (!ReadContact(out, axesGiven, point, up))
      return;
    __int64 owner = t_FootOwner;
    bool hers = false;
    if (owner)
      for (auto const& o : g_FaithFeetOwners)
        hers |= o.load(std::memory_order_relaxed) == owner;
    if (!hers || !g_FaithFeetPlaceOk.load(std::memory_order_relaxed))
      return;
    float dx = point[0] - g_FaithFeetPlace[0], dy = point[1] - g_FaithFeetPlace[1], dz = point[2] - g_FaithFeetPlace[2];
    if (dx * dx + dy * dy + dz * dz > FaithFeetReach * FaithFeetReach)
      return;
    // (and in the rain: her roll's ripples in a puddle, her wet hands' prints,
    // wetlook::NoteRainRoll / NoteRainHand)
    float normal[3];
    bool haveNormal = ReadContactNormal(out, axesGiven, normal);
    if (t_RollImpact)
    {
      wetlook::NoteRoll(point, up);
      wetlook::NoteRainRoll(point, up);
    }
    else
    {
      // (her own hand pressed there, the prints' shape both in the snow and
      // in the rain: SnowBodies.cpp's hand stamps; 2026-10-28, the user:
      // "handprints for snow/rain should be faithful to faiths hand")
      int stamp = haveNormal ? snowbody::NoteHandTouch(point, normal, t_HandSide) : -1;
      wetlook::NoteHand(point, up, t_HandSide, stamp);
      if (haveNormal)
        wetlook::NoteRainHand(point, normal, t_HandSide, stamp);
    }
  }
}

bool __fastcall hImpactSurface(__int64 context, __int64 id, unsigned int* out, float const* place, float const* axisA,
  float const* axisB)
{
  bool found = oImpactSurface(context, id, out, place, axisA, axisB);
  if (found && out && t_FootImpact)
    WetStep(out, axisA != nullptr);
  else if (found && out && (t_HandSide || t_RollImpact))
    HandTouch(out, axisA != nullptr);
  return found;
}

// The two handlers (self: their owner at + 0x08 / + 0x10; data: the impact's
// sound or effect data), only to say whose step the surface lookup is for
__int64 __fastcall hImpactSoundHandler(__int64 self, __int64 a2, __int64 event, __int64 data)
{
  bool wasFoot = t_FootImpact;
  int wasKind = t_FootKind, wasHand = t_HandSide, wasFootSide = t_FootSide;
  bool wasOther = t_OtherFeet;
  __int64 wasOwner = t_FootOwner;
  t_FootKind = FootImpactKind(data, 0x08, 0x18, &t_FootSide, &t_OtherFeet);
  t_FootImpact = t_FootKind != 0;
  t_HandSide = t_FootImpact ? 0 : HandImpactSide(data, 0x08, 0x18);
  t_FootOwner = t_FootImpact || t_HandSide ? ReadOwner(self + ImpactSoundOwnerOffset) : 0;
  __int64 result = oImpactSoundHandler(self, a2, event, data);
  t_FootImpact = wasFoot;
  t_FootKind = wasKind;
  t_FootSide = wasFootSide;
  t_OtherFeet = wasOther;
  t_HandSide = wasHand;
  t_FootOwner = wasOwner;
  return result;
}

__int64 __fastcall hImpactEffectHandler(__int64 self, __int64 a2, __int64 event, __int64 data)
{
  bool wasFoot = t_FootImpact, wasRoll = t_RollImpact;
  int wasKind = t_FootKind, wasHand = t_HandSide, wasFootSide = t_FootSide;
  bool wasOther = t_OtherFeet;
  __int64 wasOwner = t_FootOwner;
  t_FootKind = FootImpactKind(data, 0x20, 0x10, &t_FootSide, &t_OtherFeet);
  t_FootImpact = t_FootKind != 0;
  t_HandSide = t_FootImpact ? 0 : HandImpactSide(data, 0x20, 0x10);
  t_RollImpact = !t_FootImpact && !t_HandSide && IsRollImpact(data, 0x20);
  t_FootOwner = t_FootImpact || t_HandSide || t_RollImpact ? ReadOwner(self + ImpactEffectOwnerOffset) : 0;
  __int64 result = oImpactEffectHandler(self, a2, event, data);
  t_FootImpact = wasFoot;
  t_RollImpact = wasRoll;
  t_FootKind = wasKind;
  t_FootSide = wasFootSide;
  t_OtherFeet = wasOther;
  t_HandSide = wasHand;
  t_FootOwner = wasOwner;
  return result;
}

static bool CallsAt(BYTE const* pBase, __int64 callRva, __int64 targetRva)
{
  return pBase[callRva] == 0xE8 && callRva + 5 + *(int const*)(pBase + callRva + 1) == targetRva;
}

static bool IsWetStepsCode(BYTE const* pBase)
{
  // 0x1439AE870: mov [rsp + 0x20], r9; push rbp r14 r15; lea rbp, [rsp - 0x37]; sub rsp, 0x100; mov r14, r8; mov r15d, edx
  static const BYTE surface[] = { 0x4C, 0x89, 0x4C, 0x24, 0x20, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8D, 0x6C, 0x24, 0xC9, 0x48,
    0x81, 0xEC, 0x00, 0x01, 0x00, 0x00, 0x4D, 0x89, 0xC6, 0x41, 0x89, 0xD7 };
  // 0x143924CE0: mov [rsp + 0x20], rbx; push rsi; sub rsp, 0xA0; mov rax, [r8]; mov rbx, rcx
  static const BYTE sound[] = { 0x48, 0x89, 0x5C, 0x24, 0x20, 0x56, 0x48, 0x81, 0xEC, 0xA0, 0x00, 0x00, 0x00, 0x49, 0x8B, 0x00,
    0x48, 0x89, 0xCB };
  // 0x143924A90: push rbp rsi rdi r14 r15; lea rbp, [rsp - 0x80]; sub rsp, 0x180
  static const BYTE effect[] = { 0x55, 0x56, 0x57, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8D, 0x6C, 0x24, 0x80, 0x48, 0x81, 0xEC, 0x80,
    0x01, 0x00, 0x00 };
  // 0x143282B80: mov [rsp + 0x10], rbx; push rdi; sub rsp, 0x20; mov r9d, [rdx]; mov rdi, rcx
  static const BYTE lookup[] = { 0x48, 0x89, 0x5C, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x44, 0x8B, 0x0A, 0x48, 0x89, 0xCF };
  // The sound handler's movss xmm0, [rax + 0x34]: the sound id it gives
  static const BYTE soundId[] = { 0xF3, 0x0F, 0x10, 0x40, 0x34 };
  BYTE const* load = pBase + ImpactSoundHolderLoadRva;
  return !memcmp(pBase + ImpactSurfaceRva, surface, sizeof(surface)) &&
    !memcmp(pBase + ImpactSoundHandlerRva, sound, sizeof(sound)) &&
    !memcmp(pBase + ImpactEffectHandlerRva, effect, sizeof(effect)) &&
    !memcmp(pBase + MaterialSoundDataRva, lookup, sizeof(lookup)) &&
    !memcmp(pBase + ImpactSoundLookupCallRva + 10, soundId, sizeof(soundId)) &&
    *(__int64 const*)(pBase + ImpactSoundHandlerSlotRva) == (__int64)pBase + ImpactSoundHandlerRva &&
    *(__int64 const*)(pBase + ImpactEffectHandlerSlotRva) == (__int64)pBase + ImpactEffectHandlerRva &&
    CallsAt(pBase, ImpactSoundSurfaceCallRva, ImpactSurfaceRva) && CallsAt(pBase, ImpactEffectSurfaceCallRva, ImpactSurfaceRva) &&
    CallsAt(pBase, ImpactSoundLookupCallRva, MaterialSoundDataRva) &&
    load[0] == 0x48 && load[1] == 0x8B && load[2] == 0x05 &&
    ImpactSoundHolderLoadRva + 7 + *(int const*)(load + 3) == MaterialHolderRva;
}

void util::hooks::SetFaithFeet(__int64 soldier, float const* place)
{
  __int64 owners[4] = { soldier, 0, 0, 0 };
  if (soldier > 0x10000)
  {
    __try
    {
      owners[1] = *(__int64 const*)(soldier + SoldierBodyAnimatableOffset);
      owners[2] = *(__int64 const*)(soldier + SoldierBoxAnimatableOffset);
      __int64 component = *(__int64 const*)(soldier + SoldierRecordingComponentOffset);
      if (component > 0x10000)
        owners[3] = *(__int64 const*)(component + RecordingComponentAnimatableOffset);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
  }
  for (int i = 0; i < 4; ++i)
    g_FaithFeetOwners[i].store(owners[i] > 0x10000 ? owners[i] : 0, std::memory_order_relaxed);
  if (place)
    memcpy(g_FaithFeetPlace, place, sizeof(g_FaithFeetPlace));
  g_FaithFeetPlaceOk.store(place != nullptr, std::memory_order_relaxed);
}

// Replay people, kept (see KeepGuard below): the destroy requests kept, and
// the places of the recorded people they're matched with
namespace
{
  struct KeptRequest
  {
    __int64 Entity;
    __int64 Owner;
    DWORD Thread;           // the game's thread that asked
  };
  std::mutex g_KeptMutex;
  std::vector<KeptRequest> g_Kept;        // under g_KeptMutex
  std::vector<float> g_GuardPlaces;       // x y z each, under g_KeptMutex
  float g_GuardRadius = 0.f;              // under g_KeptMutex
  // The server vehicles behind the recorded ones, kept by who they are (a
  // VTOL flies too fast to be found by place), and the ones destroyed since
  // (their memory may be another's: never kept again), under g_KeptMutex
  std::vector<__int64> g_KeptVehicles;
  std::vector<__int64> g_DeadVehicles;
  std::atomic<bool> g_HandBackKept{ false };
  // The replay's stand-ins (server characters it had the game spawn, see
  // AskStandIn), kept by who they are too, under g_KeptMutex
  std::vector<__int64> g_StandInEntities;
}

// The server characters' deleting destructor (vtable slot 7:
// PamServerCharacterEntity 0x14391F540; the client's is 0x14390DBE0) drops a
// kept request for a character destroyed another way (a level unloading), so
// one is never handed back for a character that's gone. (Both destructors
// logged the game's code on the stack for a while: it was only ever the
// realm's destroy-list flush, see hContainerDestroyAll.)
typedef __int64(__fastcall* tCharacterDtor)(__int64, unsigned int);
tCharacterDtor oServerCharacterDtor = nullptr;
const __int64 ClientCharacterDtorRva = 0x390DBE0;
const __int64 ServerCharacterDtorRva = 0x391F540;
const __int64 ClientCharacterVTableRva = 0x1C3C760;
const __int64 ServerCharacterVTableRva = 0x1C3FCB0;

// Diagnostic (temporary): the game's code on the stack, as addresses in the
// game (0x14xxxxxxx), the tools' own frames left out
static void GameStack(char* line, size_t size)
{
  void* frames[28];
  USHORT count = RtlCaptureStackBackTrace(1, 28, frames, nullptr);
  __int64 base = (__int64)g_gameHandle;
  IMAGE_NT_HEADERS const* pNt = (IMAGE_NT_HEADERS const*)(base + ((IMAGE_DOS_HEADER const*)base)->e_lfanew);
  __int64 end = base + pNt->OptionalHeader.SizeOfImage;
  size_t at = 0;
  line[0] = 0;
  for (USHORT i = 0; i < count && at + 12 < size; ++i)
  {
    __int64 address = (__int64)frames[i];
    if (address >= base && address < end)
      at += sprintf_s(line + at, size - at, " %I64X", address - base + 0x140000000);
  }
}

// A kept request for an entity destroyed another way (a level unloading) is
// dropped, so none is ever handed back for one that's gone
static void DropKept(__int64 entity)
{
  std::lock_guard<std::mutex> lock(g_KeptMutex);
  for (size_t i = 0; i < g_Kept.size(); ++i)
    if (g_Kept[i].Entity == entity)
    {
      g_Kept.erase(g_Kept.begin() + i);
      util::log::Write("Replay: the character, drone or vehicle 0x%I64X kept in the level was destroyed another way (a level "
        "unloading?), it isn't handed back", entity);
      break;
    }
}

static void NoteCharacterDtor(__int64 entity);   // diagnostic (temporary), see hContainerDestroyAll
static void DropStandIn(__int64 entity);         // see AskStandIn

__int64 __fastcall hServerCharacterDtor(__int64 entity, unsigned int flags)
{
  if (util::DiagnosticsOn() && g_HoldAIDespawn.load(std::memory_order_relaxed))
    NoteCharacterDtor(entity);
  DropKept(entity);
  DropStandIn(entity);
  return oServerCharacterDtor(entity, flags);
}

// The server drones' deleting destructor (PamServerDroneActorEntity slot 7,
// 0x143954FC0), for the same: drones are kept too (see ServerCharacterPlace)
tCharacterDtor oServerDroneDtor = nullptr;
const __int64 ServerDroneDtorRva = 0x3954FC0;

static bool IsKeptVehicle(__int64 entity);

// Diagnostic (temporary): a drone the replay keeps in the level, destroyed.
// 2026-10-25 03:47:40: one kept by who it is went 22 s after the game asked
// for it to be taken out (held), and no kept sub-level held it: neither the
// destroy request nor a sub-level unload. Its code on the stack shows which
// way it went (at most DroneDtorLogsMax a game).
namespace
{
  std::atomic<int> g_DroneDtorLogs{ 0 };
  const int DroneDtorLogsMax = 16;
}

static void NoteKeptDroneDtor(__int64 entity)
{
  bool kept = IsKeptVehicle(entity);
  {
    std::lock_guard<std::mutex> lock(g_KeptMutex);
    for (auto const& request : g_Kept)
      kept = kept || request.Entity == entity;
  }
  if (!kept || g_DroneDtorLogs.fetch_add(1, std::memory_order_relaxed) >= DroneDtorLogsMax)
    return;
  char stack[28 * 12 + 1];
  GameStack(stack, sizeof(stack));
  util::log::Write("Replay (diagnostic): the drone 0x%I64X kept in the level is destroyed; its code on the stack:%s", entity, stack);
}

__int64 __fastcall hServerDroneDtor(__int64 entity, unsigned int flags)
{
  if (util::DiagnosticsOn())
    NoteKeptDroneDtor(entity);
  DropKept(entity);
  return oServerDroneDtor(entity, flags);
}

// The server vehicles' deleting destructor (ServerVehicleEntity slot 7,
// 0x143396B00), for the same: a recorded vehicle's server entity is kept
// too (see SetKeptVehicles)
tCharacterDtor oServerVehicleDtor = nullptr;
const __int64 ServerVehicleDtorRva = 0x3396B00;
const __int64 ServerVehicleVTableRva = 0x1B51608;

__int64 __fastcall hServerVehicleDtor(__int64 entity, unsigned int flags)
{
  DropKept(entity);
  {
    std::lock_guard<std::mutex> lock(g_KeptMutex);
    auto it = std::find(g_KeptVehicles.begin(), g_KeptVehicles.end(), entity);
    if (it != g_KeptVehicles.end())
    {
      g_KeptVehicles.erase(it);
      g_DeadVehicles.push_back(entity);
    }
  }
  return oServerVehicleDtor(entity, flags);
}

static bool IsServerVehicleDtorCode(BYTE const* pBase)
{
  // mov [rsp + 8], rbx; push rdi; sub rsp, 0x20; mov ebx, edx; mov rdi, rcx; call
  static const BYTE code[] = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x89, 0xD3, 0x48, 0x89, 0xCF, 0xE8 };
  return !memcmp(pBase + ServerVehicleDtorRva, code, sizeof(code)) &&
    *(__int64 const*)(pBase + ServerVehicleVTableRva + 7 * 8) == (__int64)pBase + ServerVehicleDtorRva;
}

static bool IsKeptVehicle(__int64 entity)
{
  std::lock_guard<std::mutex> lock(g_KeptMutex);
  return std::find(g_KeptVehicles.begin(), g_KeptVehicles.end(), entity) != g_KeptVehicles.end();
}

void util::hooks::SetKeptVehicles(__int64 const* entities, int count)
{
  std::lock_guard<std::mutex> lock(g_KeptMutex);
  g_KeptVehicles.clear();
  for (int i = 0; i < count; ++i)
    if (entities[i] && std::find(g_DeadVehicles.begin(), g_DeadVehicles.end(), entities[i]) == g_DeadVehicles.end())
      g_KeptVehicles.push_back(entities[i]);
}

void util::hooks::ForgetKeptVehicles()
{
  std::lock_guard<std::mutex> lock(g_KeptMutex);
  g_KeptVehicles.clear();
  g_DeadVehicles.clear();
}

static bool IsServerDroneDtorCode(BYTE const* pBase)
{
  // mov [rsp + 8], rbx; push rdi; sub rsp, 0x20; lea rax, [rip + its vtable]
  static const BYTE code[] = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8D, 0x05 };
  const __int64 vtableRva = 0x1C4B8E8;   // PamServerDroneActorEntity
  return !memcmp(pBase + ServerDroneDtorRva, code, sizeof(code)) &&
    ServerDroneDtorRva + 10 + 7 + *(int const*)(pBase + ServerDroneDtorRva + 13) == vtableRva &&
    *(__int64 const*)(pBase + vtableRva + 7 * 8) == (__int64)pBase + ServerDroneDtorRva;
}

static bool IsCharacterDtorCode(BYTE const* pBase)
{
  // mov [rsp + 8], rbx; push rdi; sub rsp, 0x20; lea rax, [rip + its vtable]
  static const BYTE code[] = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8D, 0x05 };
  for (__int64 rva : { ClientCharacterDtorRva, ServerCharacterDtorRva })
    if (memcmp(pBase + rva, code, sizeof(code)))
      return false;
  auto leaTarget = [pBase](__int64 rva) { return rva + 10 + 7 + *(int const*)(pBase + rva + 13); };
  return leaTarget(ClientCharacterDtorRva) == ClientCharacterVTableRva && leaTarget(ServerCharacterDtorRva) == ServerCharacterVTableRva &&
    *(__int64 const*)(pBase + ClientCharacterVTableRva + 7 * 8) == (__int64)pBase + ClientCharacterDtorRva &&
    *(__int64 const*)(pBase + ServerCharacterVTableRva + 7 * 8) == (__int64)pBase + ServerCharacterDtorRva;
}

// Replay people, kept: the game takes KrugerSec out of the level in more ways
// than the auto-despawn (2026-10-23: a whole group went at once as the real
// Faith ran off, with the auto-despawn checks seeing nothing). All the ways
// found (Research_Spawn_System_2026-10-23.md) ask for the character to be
// destroyed with 0x143167700 (entity, owner: unless + 0x18 bit 10 says it's
// going already, [owner]'s vtable slot 2 is given the entity): a spawner's
// Unspawn (0x143383F4F; the level's own event links unspawning a group end
// there) and the AI coordinator's despawn (PamServerAICoordinator slot 24,
// 0x143933580). While the replay holds people, a request for a
// PamServerCharacterEntity near one of the recorded people (SetGuardPlaces) is
// kept instead. The owner's slot 2 (Entity slot 28, 0x143125180) takes the
// entity out of its owner's list (0x14316BC90) and adds it to its realm's
// list of entities to destroy ([0x1423E5710 + realm * 8] + 0x10 / + 0x18, the
// 0x400 "going" flag set), neither under a lock: so once the replay lets go of
// them, each is handed to the game on the thread that asked, when that thread
// next asks for a character to be destroyed. Both ways traced (a spawner's
// Unspawn, on a copy of its list, and the AI coordinator's, one AI) do that
// outside any walk of an owner's list; the auto-despawn checks aren't a
// place for it (2026-10-23: not run once in two minutes).
typedef __int64(__fastcall* tDestroyRequest)(__int64, __int64);
tDestroyRequest oDestroyRequest = nullptr;
const __int64 DestroyRequestRva = 0x3167700;
const int EntityFlagsOffset = 0x18;
const unsigned int EntityGoingFlag = 1u << 10;

// A server character's place, read the way the game's own despawn check reads
// it (0x143982B03): its components (+ 0x38), the translation at + 0x40 + (byte
// 9 + 2 * byte 0xA) * 0x20; false if it isn't a PamServerCharacterEntity (or a
// PamServerDroneActorEntity, vtable 0x141C4B8E8: a drone killed in a recorded
// fight went before its replay, 2026-10-23) on its way to staying (already going)
const __int64 ServerDroneVTableRva = 0x1C4B8E8;
static bool ServerCharacterPlace(__int64 entity, float* place)
{
  __int64 base = (__int64)g_gameHandle;
  __try
  {
    // A drone or a vehicle only with its destructor hooked (DropKept)
    if (entity < 0x10000)
      return false;
    __int64 vtable = *(__int64 const*)entity;
    if (vtable != base + ServerCharacterVTableRva && !(vtable == base + ServerDroneVTableRva && oServerDroneDtor) &&
        !(vtable == base + ServerVehicleVTableRva && oServerVehicleDtor))
      return false;
    if (*(unsigned int const*)(entity + EntityFlagsOffset) & EntityGoingFlag)
      return false;
    BYTE const* components = *(BYTE const**)(entity + 0x38);
    if ((__int64)components < 0x10000)
      return false;
    memcpy(place, components + 0x40 + ((__int64)components[9] + 2 * components[0xA]) * 0x20, 3 * sizeof(float));
    return true;
  }
  __except (EXCEPTION_EXECUTE_HANDLER)
  {
    return false;
  }
}

static bool KeepGuard(__int64 entity, __int64 owner, float const* place)
{
  std::lock_guard<std::mutex> lock(g_KeptMutex);
  // A recorded vehicle's and a stand-in by who it is (see SetKeptVehicles,
  // AskStandIn), the others by place
  bool close = std::find(g_KeptVehicles.begin(), g_KeptVehicles.end(), entity) != g_KeptVehicles.end() ||
    std::find(g_StandInEntities.begin(), g_StandInEntities.end(), entity) != g_StandInEntities.end();   // (near is a Windows macro)
  float radius2 = g_GuardRadius * g_GuardRadius;
  for (size_t i = 0; i + 2 < g_GuardPlaces.size() && !close; i += 3)
  {
    float dx = place[0] - g_GuardPlaces[i], dy = place[1] - g_GuardPlaces[i + 1], dz = place[2] - g_GuardPlaces[i + 2];
    close = dx * dx + dy * dy + dz * dz <= radius2;
  }
  if (!close)
    return false;
  for (auto const& kept : g_Kept)
    if (kept.Entity == entity)
      return true;                        // asked again: still kept
  g_Kept.push_back({ entity, owner, GetCurrentThreadId() });
  __int64 vtable = *(__int64 const*)entity;
  char const* what = vtable == (__int64)g_gameHandle + ServerDroneVTableRva ? "drone" :
    vtable == (__int64)g_gameHandle + ServerVehicleVTableRva ? "vehicle" : "character";
  util::log::Write("Replay: the game asked for the %s 0x%I64X (server) at %.1f %.1f %.1f to be taken out of the level; "
    "kept while the replay holds people (%d kept)", what, entity, place[0], place[1], place[2], (int)g_Kept.size());
  return true;
}

// The kept requests this thread made, to the game
static void HandBackKept()
{
  DWORD thread = GetCurrentThreadId();
  std::vector<KeptRequest> mine;
  {
    std::lock_guard<std::mutex> lock(g_KeptMutex);
    for (size_t i = 0; i < g_Kept.size();)
      if (g_Kept[i].Thread == thread)
      {
        mine.push_back(g_Kept[i]);
        g_Kept.erase(g_Kept.begin() + i);
      }
      else
        ++i;
    if (g_Kept.empty())
      g_HandBackKept = false;
  }
  int handed = 0;
  for (auto const& kept : mine)
  {
    // Still the character it was (and not going yet)
    float place[3];
    if (!ServerCharacterPlace(kept.Entity, place))
    {
      util::log::Warning("Replay: the character 0x%I64X kept in the level isn't the one it was, not handed back", kept.Entity);
      continue;
    }
    oDestroyRequest(kept.Entity, kept.Owner);
    ++handed;
  }
  if (handed)
    util::log::Write("Replay: %d characters kept in the level handed back to the game", handed);
}

__int64 __fastcall hDestroyRequest(__int64 entity, __int64 owner)
{
  bool hold = g_HoldAIDespawn.load(std::memory_order_relaxed);
  bool handBack = g_HandBackKept.load(std::memory_order_relaxed);
  float place[3];
  if ((hold || handBack) && ServerCharacterPlace(entity, place))
  {
    if (handBack)
      HandBackKept();
    if (hold && KeepGuard(entity, owner, place))
      return 0;
  }
  return oDestroyRequest(entity, owner);
}

static bool IsDestroyRequestCode(BYTE const* pBase)
{
  // mov eax, [rcx + 0x18]; mov r8, rdx; mov rdx, rcx; shr eax, 0xA; test al, 1; jne +9; mov rcx, [r8]; mov rax, [rcx];
  // jmp [rax + 0x10]; ret
  static const BYTE code[] = { 0x8B, 0x41, 0x18, 0x49, 0x89, 0xD0, 0x48, 0x89, 0xCA, 0xC1, 0xE8, 0x0A, 0xA8, 0x01, 0x75, 0x09,
    0x49, 0x8B, 0x08, 0x48, 0x8B, 0x01, 0xFF, 0x60, 0x10, 0xC3 };
  return !memcmp(pBase + DestroyRequestRva, code, sizeof(code));
}

// The AI a despawn check is for, where it is: the manager's AI (+ 0x40, what
// both checks give the despawn as rdx), its character (+ 0xD0), the
// translation in its components (+ 0x38) at + 0x40 + (byte 9 + 2 * byte 0xA) *
// 0x20, read the way PamServerAIAutoDespawnManager's check reads it
// (0x143982AE5 - 0x143982B1D). The base manager's check (0x142CB2FE0, by
// lifetime) reads no place; its AI is at + 0x40 the same (rdx at 0x142CB3014).
static bool AIManagerPlace(__int64 manager, float* place)
{
  __try
  {
    __int64 ai = *(__int64 const*)(manager + 0x40);
    if (ai < 0x10000)
      return false;
    __int64 character = *(__int64 const*)(ai + 0xD0);
    if (character < 0x10000)
      return false;
    BYTE const* components = *(BYTE const**)(character + 0x38);
    if ((__int64)components < 0x10000)
      return false;
    memcpy(place, components + 0x40 + ((__int64)components[9] + 2 * components[0xA]) * 0x20, 3 * sizeof(float));
    return place[0] == place[0] && place[1] == place[1] && place[2] == place[2];
  }
  __except (EXCEPTION_EXECUTE_HANDLER)
  {
    return false;
  }
}

// The character of a despawn check's AI (+ 0x40, + 0xD0, as AIManagerPlace)
static __int64 AIManagerCharacter(__int64 manager)
{
  __try
  {
    __int64 ai = *(__int64 const*)(manager + 0x40);
    return ai < 0x10000 ? 0 : *(__int64 const*)(ai + 0xD0);
  }
  __except (EXCEPTION_EXECUTE_HANDLER)
  {
    return 0;
  }
}

// While the replay holds people: a despawn check is skipped only for an AI
// within the guard radius of a recorded person's places (SetGuardPlaces, the
// same places KeepGuard keeps destroy requests by) or one of the replay's
// stand-ins (by who it is); every other AI is checked by the game as usual,
// and so is one whose place can't be read
static bool HoldsAIDespawn(__int64 pManager)
{
  float place[3];
  if (!AIManagerPlace(pManager, place))
    return false;
  __int64 character = AIManagerCharacter(pManager);
  std::lock_guard<std::mutex> lock(g_KeptMutex);
  if (character && std::find(g_StandInEntities.begin(), g_StandInEntities.end(), character) != g_StandInEntities.end())
    return true;
  float radius2 = g_GuardRadius * g_GuardRadius;
  for (size_t i = 0; i + 2 < g_GuardPlaces.size(); i += 3)
  {
    float dx = place[0] - g_GuardPlaces[i], dy = place[1] - g_GuardPlaces[i + 1], dz = place[2] - g_GuardPlaces[i + 2];
    if (dx * dx + dy * dy + dz * dz <= radius2)
      return true;
  }
  return false;
}

void util::hooks::SetGuardPlaces(float const* places, int count, float radius)
{
  std::lock_guard<std::mutex> lock(g_KeptMutex);
  g_GuardPlaces.assign(places, places + (size_t)count * 3);
  g_GuardRadius = radius;
}

// Stand-ins (2026-10-27, stage 2 of the spawning work; Research_Replay_
// Mission_World_2026-10-26.md section 8). A recording played after its
// mission was over has lost its people with the mission's containers, and
// borrowing (FaithReplay::BorrowForGone) finds only the few of their kind left
// in the loaded world (3 of 18 at 01:13). The free-roam spawners stay (40 then,
// all on: 12 with the Enemy templates, Enforcer01 / 02 and Protector with no
// limit, Sentinel 1 each). So:
// - While the replay records or plays (WatchSpawners), the server's update
//   notes each spawner's characters (its list at + 0x148 / + 0x150, the one
//   its Unspawn destroys, 0x143383EB3) with their places and its template, so
//   a recorded person gets the template it was spawned from
//   (SpawnTemplateNear) and a live one its kind.
// - A playback asks for a stand-in of that template (AskStandIn); the server's
//   next update sends Spawn (0x0DC320DE) to an enabled spawner of it (or of
//   its folder and kind, AIRanged, AIMelee...) through the spawner's own
//   handler (slot 4, 0x143958790), as a level's event link does: it only
//   queues the spawn (0x143174360), the spawn manager makes the character.
//   The new one in that spawner's list is the stand-in, which the replay plays
//   the recorded person on, wherever it stands (FaithReplay::BorrowForGone).
//   A spawner that makes none in SpawnWaitMs (at its limit, waiting to spawn
//   out of sight, or turned down by the AI coordinator's pool, 0x14392D710)
//   is given up for another, SpawnTries times.
// - While the replay holds people, the game doesn't take a stand-in out of the
//   level (KeepGuard, HoldsAIDespawn: by who it is).
// - When the playback stops (RemoveStandIns), each one is destroyed with the
//   game's own request 0x143167700(character, [character + 0x28]), that
//   character only, on the server's thread (the request isn't thread safe,
//   see HandBackKept); not Unspawn, which would take all of a spawner's.
// The event: on Spawn's way only its id (+ 8) is read, except by
// ServerCharacterSpawnEntity's handler (0x143382D20), which asks its type
// (slot 0) for the class id (+ 0x28) and takes a ServerPlayerEvent (class ids
// [0x1427FF808] - [0x1427FF80A], 0x5DB - 0x5DD) as a player's spawn: the
// tools' type has a class id outside them. A stand-in is a live character
// with its AI (it may fight the real Faith, who takes no damage while a
// replay plays); the option is REPLAY > "Stand-ins".
namespace
{
  const __int64 SpawnerHandlerRva = 0x3958790;       // PamServerCharacterSpawnEntity slot 4
  const __int64 SpawnerBaseHandlerRva = 0x3382D20;   // ServerCharacterSpawnEntity's
  const __int64 SpawnHandlerRva = 0x3383D40;         // ServerSpawnEntity's
  const __int64 SpawnQueueRva = 0x3174360;           // the spawn manager's queue
  const __int64 PlayerEventFirstRva = 0x27FF808;     // ServerPlayerEvent's class ids, first and last (words)
  const __int64 PlayerEventLastRva = 0x27FF80A;
  const unsigned int SpawnEventId = 0x0DC320DE;      // "Spawn"
  const int SpawnerActiveOffset = 0x2A4;
  const int SpawnedListOffset = 0x148;               // its characters: begin, end (+ 8)
  const int EventTypeClassIdOffset = 0x28;
  const int MaxSpawnedRead = 64;
  const int MaxStandIns = 32;
  // A spawner's room for one more (2026-10-27 02:18: the civilians' spawners,
  // MaxCount 1, each took 15 s to be given up): its own checks, 0x1433804A0
  // (after its listeners' say, which isn't read: its total so far, + 0xF4,
  // under MaxCount) and 0x143388DD0 (its characters under
  // MaxCountSimultaneously), each -1 for no limit, their values through the
  // property cells at + 0x270 / + 0x280 read the way 0x142A424B0 does (the
  // link's value when its + 0x1C has 0x40, else the default at + 8)
  const __int64 SpawnerCanSpawnRva = 0x33804A0;
  const __int64 SpawnerAtLimitRva = 0x3388DD0;
  // (PropertyGetterRva, 0x2A424B0, is the spot sounds' above)
  const int SpawnedTotalOffset = 0xF4;
  const int MaxCountCellOffset = 0x270;
  const int MaxAliveCellOffset = 0x280;
  bool g_SpawnerLimitsCode = false;
  const int MaxTemplates = 256;
  const int SpawnTries = 3;
  const ULONGLONG SpawnWaitMs = 15000;
  const ULONGLONG SpawnedNoteMs = 1000;

  __declspec(align(16)) BYTE g_SpawnEventType[0x40] = {};
  __int64 __fastcall SpawnEventGetType(void*) { return (__int64)g_SpawnEventType; }
  void* g_SpawnEventVTable[8] = { (void*)&SpawnEventGetType, (void*)&SpawnEventGetType, (void*)&SpawnEventGetType,
    (void*)&SpawnEventGetType, (void*)&SpawnEventGetType, (void*)&SpawnEventGetType, (void*)&SpawnEventGetType,
    (void*)&SpawnEventGetType };
  typedef void(__fastcall* tEntityEvent)(__int64, __int64);
  bool g_StandInCode = false;   // the code above read as known (IsStandInCode)

  struct TemplateName
  {
    char Name[128];
  };
  struct SpawnerNote
  {
    __int64 Entity;
    bool Enabled;
    bool Room;       // it can make one more now (see SpawnerRoom)
    int Template;
  };
  struct SpawnedNote
  {
    __int64 Entity;
    __int64 Spawner;
    int Template;
    float Place[3];
  };
  // StandIn_Cancelled: asked, then the playback stopped: taken out as soon as
  // its spawner makes it (or forgotten after SpawnWaitMs)
  enum { StandIn_Wanted, StandIn_Asked, StandIn_Ready, StandIn_Failed, StandIn_Gone, StandIn_Cancelled };
  struct StandIn
  {
    int Id;
    int Template;
    int State;
    int Tries;
    __int64 Spawner;                 // the one asked (StandIn_Asked)
    std::vector<__int64> Before;     // its characters then
    std::vector<__int64> Tried;      // the ones given up
    ULONGLONG AskedAt;
    ULONGLONG WantedAt;
    __int64 Entity;                  // the stand-in (StandIn_Ready)
    float Place[3];
  };

  std::mutex g_StandInMutex;         // everything below
  std::vector<TemplateName> g_TemplateNames;
  std::vector<SpawnerNote> g_SpawnerNotes;
  std::vector<SpawnedNote> g_SpawnedNotes;
  ULONGLONG g_SpawnedNotedAt = 0;
  std::vector<StandIn> g_StandIns;
  int g_NextStandIn = 1;
  bool g_RemoveStandIns = false;
  std::atomic<bool> g_WatchSpawners{ false };
  std::atomic<bool> g_StandInWork{ false };   // the server's update has something to do

  char const* ShortTemplateName(char const* name)
  {
    char const* slash = strrchr(name, '/');
    return slash ? slash + 1 : name;
  }

  // "Characters/SpawnTemplates/Enemy/SpawnTemplate_AIRanged_Enforcer01":
  // AIRanged (empty if it isn't named that way)
  void TemplateKind(char const* name, char* out, size_t size)
  {
    out[0] = 0;
    char const* first = strchr(ShortTemplateName(name), '_');
    if (!first)
      return;
    char const* second = strchr(first + 1, '_');
    size_t length = second ? (size_t)(second - first - 1) : strlen(first + 1);
    if (length > 0 && length < size)
    {
      memcpy(out, first + 1, length);
      out[length] = 0;
    }
  }

  // A spawn template no stand-in is ever made of, nor taken from (2026-10-27
  // 03:50, the user: "random beat ghosts around the freeroam city map"): the
  // runner projections (characters/spawntemplates/projection/...) and Faith's
  // own. The Release mission's red Runner Vision guides were recorded as
  // people (SpawnTemplate_Projection_Replay); once the mission was over, the
  // only spawners of that template were the city's social replay markers
  // (gameplay/social/pf_skillreplaymarkerwithautoplay, pf_replaymarker: a
  // PamReplayMarker, a PamProjectionSpawner, a CharacterSpawnReference with
  // QueueSpawnEvent), and spawning through them set the markers' own ghosts
  // off: 21 Runner_Projection sounds as the replay stopped and took its 6 out.
  bool IsNoStandInTemplate(char const* name)
  {
    char lower[160];
    size_t i = 0;
    for (; name[i] && i + 1 < sizeof(lower); ++i)
      lower[i] = (char)tolower((unsigned char)name[i]);
    lower[i] = 0;
    return strstr(lower, "/projection/") || strstr(lower, "/faith/") || strstr(lower, "spawntemplate_projection") ||
      strstr(lower, "spawntemplate_faith");
  }

  // 2: the same template; 1: the same folder and kind (not an
  // "ErrorTemplate"); 0: neither
  int TemplateMatch(char const* wanted, char const* have)
  {
    if (!_stricmp(ShortTemplateName(wanted), ShortTemplateName(have)))
      return 2;
    size_t folder = ShortTemplateName(wanted) - wanted;
    if (folder == 0 || folder != (size_t)(ShortTemplateName(have) - have) || _strnicmp(wanted, have, folder) ||
        strstr(have, "ErrorTemplate"))
      return 0;
    char kindWanted[64], kindHave[64];
    TemplateKind(wanted, kindWanted, sizeof(kindWanted));
    TemplateKind(have, kindHave, sizeof(kindHave));
    return kindWanted[0] && !_stricmp(kindWanted, kindHave) ? 1 : 0;
  }

  // Under g_StandInMutex: a template's index in g_TemplateNames, added if new
  // (-1 if there's no room)
  int TemplateIndex(char const* name)
  {
    for (size_t i = 0; i < g_TemplateNames.size(); ++i)
      if (!_stricmp(g_TemplateNames[i].Name, name))
        return (int)i;
    if (g_TemplateNames.size() >= MaxTemplates)
      return -1;
    TemplateName added{};
    strncpy_s(added.Name, name, _TRUNCATE);
    g_TemplateNames.push_back(added);
    return (int)g_TemplateNames.size() - 1;
  }

  char const* TemplateNameOf(int index)
  {
    return index >= 0 && index < (int)g_TemplateNames.size() ? ShortTemplateName(g_TemplateNames[index].Name) : "?";
  }

  // A spawner's characters (on the server's thread, where the list changes)
  int ReadSpawned(__int64 spawner, __int64* out, int max)
  {
    __try
    {
      __int64 const* begin = *(__int64 const* const*)(spawner + SpawnedListOffset);
      __int64 const* end = *(__int64 const* const*)(spawner + SpawnedListOffset + 8);
      if (!begin || end < begin)
        return 0;
      int count = 0;
      for (__int64 const* at = begin; at < end && count < max; ++at)
        out[count++] = *at;
      return count;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      return 0;
    }
  }

  // Whether a spawner with alive characters has room for one more (true when
  // its limits can't be read: the game's own checks still decide)
  bool SpawnerRoom(__int64 spawner, int alive)
  {
    if (!g_SpawnerLimitsCode)
      return true;
    auto cell = [](__int64 at) {
      __int64 link = *(__int64 const*)at;
      int const* value = link && (*(BYTE const*)(link + 0x1C) & 0x40) ? *(int const* const*)link : *(int const* const*)(at + 8);
      return value ? *value : -1;
    };
    __try
    {
      int total = *(int const*)(spawner + SpawnedTotalOffset);
      int maxCount = cell(spawner + MaxCountCellOffset), maxAlive = cell(spawner + MaxAliveCellOffset);
      return (maxCount < 0 || total < maxCount) && (maxAlive < 0 || alive < maxAlive);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      return true;
    }
  }

  bool IsLiveSpawner(__int64 spawner)
  {
    std::lock_guard<std::mutex> lock(g_SpawnersMutex);
    return std::find(g_Spawners.begin(), g_Spawners.end(), spawner) != g_Spawners.end();
  }

  // Spawn, through the spawner's own handler (the game's code: nothing of
  // the tools' is hooked on its way)
  bool SendSpawn(__int64 spawner)
  {
    __int64 base = (__int64)g_gameHandle;
    __declspec(align(16)) BYTE event[0x40] = {};
    *(void**)event = g_SpawnEventVTable;
    *(unsigned int*)(event + 8) = SpawnEventId;
    __try
    {
      if (*(__int64 const*)spawner != base + SpawnerVTableRva || !*(BYTE const*)(spawner + SpawnerActiveOffset))
        return false;
      // A class id outside ServerPlayerEvent's (as the handler compares them)
      unsigned int first = *(WORD const*)(base + PlayerEventFirstRva), last = *(WORD const*)(base + PlayerEventLastRva);
      unsigned int classId = first > 0 ? 0 : last + 1;
      if (classId > 0xFFFF || classId - first <= last - first)
        return false;
      *(WORD*)(g_SpawnEventType + EventTypeClassIdOffset) = (WORD)classId;
      ((tEntityEvent)(base + SpawnerHandlerRva))(spawner, (__int64)event);
      return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      return false;
    }
  }

  __int64 EntityOwner(__int64 entity)
  {
    __try
    {
      return *(__int64 const*)(entity + 0x28);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      return 0;
    }
  }

  void UpdateStandInWork()
  {
    g_StandInWork = g_WatchSpawners || !g_StandIns.empty() || g_RemoveStandIns;
  }
}

static bool IsStandInCode(BYTE const* pBase)
{
  // mov [rsp + 8], rbx; push rdi; sub rsp, 0x20; cmp dword [rdx + 8], 0x0DC320DE; mov rdi, rdx; mov rbx, rcx; jne
  static const BYTE pam[] = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x81, 0x7A, 0x08, 0xDE, 0x20,
    0xC3, 0x0D, 0x48, 0x89, 0xD7, 0x48, 0x89, 0xCB, 0x75, 0x33 };
  // + 0x19 of ServerCharacterSpawnEntity's: cmp byte [rcx + 0x2A4], 0; je
  static const BYTE active[] = { 0x80, 0xB9, 0xA4, 0x02, 0x00, 0x00, 0x00, 0x0F, 0x84 };
  // + 0x34: cmp eax, 0x0DC320DE; je
  static const BYTE spawn[] = { 0x3D, 0xDE, 0x20, 0xC3, 0x0D, 0x74, 0x2D };
  // + 0x75: mov rax, [rdi]; mov rcx, rdi; call [rax]; movzx edx, word [rax + 0x28]; movzx ecx, word [rip + ...]
  static const BYTE classId[] = { 0x48, 0x8B, 0x07, 0x48, 0x89, 0xF9, 0xFF, 0x10, 0x0F, 0xB7, 0x50, 0x28, 0x0F, 0xB7, 0x0D };
  // ServerSpawnEntity's + 0x50: cmp eax, 0x0DC320DE; je; + 0x74: cmp byte [rsi + 0x2A4], 0; je
  static const BYTE baseSpawn[] = { 0x3D, 0xDE, 0x20, 0xC3, 0x0D, 0x74, 0x1D };
  static const BYTE baseActive[] = { 0x80, 0xBE, 0xA4, 0x02, 0x00, 0x00, 0x00, 0x0F, 0x84 };
  // + 0x173 (Unspawn): mov rbx, [rsi + 0x150]; sub rbx, [rsi + 0x148]
  static const BYTE list[] = { 0x48, 0x8B, 0x9E, 0x50, 0x01, 0x00, 0x00, 0x48, 0x2B, 0x9E, 0x48, 0x01, 0x00, 0x00 };
  // + 0x208: mov rcx, [rbx]; mov rdx, [rcx + 0x28]; call 0x143167700
  static const BYTE unspawn[] = { 0x48, 0x8B, 0x0B, 0x48, 0x8B, 0x51, 0x28, 0xE8 };
  auto branch = [pBase](__int64 at, BYTE op, __int64 target) {
    return pBase[at] == op && at + 5 + *(int const*)(pBase + at + 1) == target;
  };
  auto ripWord = [pBase](__int64 at, __int64 target) { return at + 7 + *(int const*)(pBase + at + 3) == target; };
  __int64 pamHandler = SpawnerHandlerRva, charHandler = SpawnerBaseHandlerRva, handler = SpawnHandlerRva;
  return *(__int64 const*)(pBase + SpawnerVTableRva + 4 * 8) == (__int64)pBase + SpawnerHandlerRva &&
    !memcmp(pBase + pamHandler, pam, sizeof(pam)) && branch(pamHandler + 0x52, 0xE8, charHandler) &&
    !memcmp(pBase + charHandler + 0x19, active, sizeof(active)) && !memcmp(pBase + charHandler + 0x34, spawn, sizeof(spawn)) &&
    !memcmp(pBase + charHandler + 0x75, classId, sizeof(classId)) && ripWord(charHandler + 0x81, PlayerEventFirstRva) &&
    pBase[charHandler + 0x8A] == 0x0F && pBase[charHandler + 0x8B] == 0xB7 && ripWord(charHandler + 0x8A, PlayerEventLastRva) &&
    branch(charHandler + 0x181, 0xE9, handler) &&
    !memcmp(pBase + handler + 0x50, baseSpawn, sizeof(baseSpawn)) && !memcmp(pBase + handler + 0x74, baseActive, sizeof(baseActive)) &&
    branch(handler + 0x11A, 0xE8, SpawnQueueRva) &&
    !memcmp(pBase + handler + 0x173, list, sizeof(list)) && !memcmp(pBase + handler + 0x208, unspawn, sizeof(unspawn)) &&
    branch(handler + 0x20F, 0xE8, DestroyRequestRva);
}

static bool IsSpawnerLimitCode(BYTE const* pBase)
{
  // 0x1433804A0 + 0x50: lea rcx, [rsi + 0x270]; lea rdx, [...]; call 0x142A424B0; mov ecx, [rax]; cmp [rsi + 0xF4], ecx;
  // jl; test ecx, ecx; js
  static const BYTE maxCount[] = { 0x48, 0x8D, 0x8E, 0x70, 0x02, 0x00, 0x00 };
  static const BYTE total[] = { 0x8B, 0x08, 0x39, 0x8E, 0xF4, 0x00, 0x00, 0x00, 0x7C, 0x0D, 0x85, 0xC9, 0x78, 0x09 };
  // 0x143388DD0 + 0x05: mov rbx, [rcx + 0x150]; lea rdx, [...]; add rcx, 0x280; sub rbx, [rcx - 0x138]; sar rbx, 3; call
  static const BYTE alive1[] = { 0x48, 0x8B, 0x99, 0x50, 0x01, 0x00, 0x00 };
  static const BYTE alive2[] = { 0x48, 0x81, 0xC1, 0x80, 0x02, 0x00, 0x00, 0x48, 0x2B, 0x99, 0xC8, 0xFE, 0xFF, 0xFF, 0x48, 0xC1,
    0xFB, 0x03 };
  // 0x142A424B0: mov rax, [rcx]; test rax, rax; je; test byte [rax + 0x1C], 0x40; je; mov rax, [rax]; ret; ... mov rax, [rcx + 8]; ret
  static const BYTE getter[] = { 0x48, 0x8B, 0x01, 0x48, 0x85, 0xC0, 0x74, 0x0E, 0xF6, 0x40, 0x1C, 0x40, 0x74, 0x08, 0x48, 0x8B,
    0x00, 0xC3 };
  static const BYTE getterDefault[] = { 0x48, 0x8B, 0x41, 0x08, 0xC3 };
  auto calls = [pBase](__int64 at, __int64 target) {
    return pBase[at] == 0xE8 && at + 5 + *(int const*)(pBase + at + 1) == target;
  };
  return !memcmp(pBase + SpawnerCanSpawnRva + 0x50, maxCount, sizeof(maxCount)) && calls(SpawnerCanSpawnRva + 0x5E, PropertyGetterRva) &&
    !memcmp(pBase + SpawnerCanSpawnRva + 0x63, total, sizeof(total)) &&
    !memcmp(pBase + SpawnerAtLimitRva + 0x05, alive1, sizeof(alive1)) &&
    !memcmp(pBase + SpawnerAtLimitRva + 0x13, alive2, sizeof(alive2)) && calls(SpawnerAtLimitRva + 0x25, PropertyGetterRva) &&
    !memcmp(pBase + PropertyGetterRva, getter, sizeof(getter)) && !memcmp(pBase + PropertyGetterRva + 0x16, getterDefault,
    sizeof(getterDefault)) &&
    calls(SpawnHandlerRva + 0x93, SpawnerCanSpawnRva) && calls(SpawnHandlerRva + 0xA3, SpawnerAtLimitRva);
}

// A stand-in destroyed by the game (a level unloading, or killed and cleared)
static void DropStandIn(__int64 entity)
{
  {
    std::lock_guard<std::mutex> lock(g_KeptMutex);
    auto it = std::find(g_StandInEntities.begin(), g_StandInEntities.end(), entity);
    if (it == g_StandInEntities.end())
      return;
    g_StandInEntities.erase(it);
  }
  std::lock_guard<std::mutex> lock(g_StandInMutex);
  for (StandIn& s : g_StandIns)
    if (s.Entity == entity && s.State == StandIn_Ready)
    {
      s.State = StandIn_Gone;
      s.Entity = 0;
      util::log::Write("Replay: stand-in #%d (the %s 0x%I64X) was destroyed by the game", s.Id, TemplateNameOf(s.Template), entity);
    }
}

static void ServeKeptWorlds();   // see "The recorded mission's world"

// From the server's update (Server::update, hooked by faith_outfit), on its
// thread, before its frame
void util::hooks::ReplayServerUpdate()
{
  if (!g_shutdown)
    ServeKeptWorlds();
  if (!g_StandInWork.load(std::memory_order_relaxed) || !g_StandInCode || g_shutdown)
    return;
  ULONGLONG now = GetTickCount64();
  bool asked = false;
  {
    std::lock_guard<std::mutex> lock(g_StandInMutex);
    for (StandIn const& s : g_StandIns)
      asked = asked || s.State == StandIn_Asked || s.State == StandIn_Wanted || s.State == StandIn_Cancelled;
    if (!asked && !g_RemoveStandIns && g_SpawnedNotedAt && now - g_SpawnedNotedAt < SpawnedNoteMs)
      return;
  }
  // The spawners and their characters, read on this thread
  std::vector<__int64> spawners;
  {
    std::lock_guard<std::mutex> lock(g_SpawnersMutex);
    spawners = g_Spawners;
  }
  struct Read
  {
    __int64 Entity;
    bool Enabled;
    bool Room;
    char Template[128];
    int Count;
    __int64 Spawned[MaxSpawnedRead];
  };
  static std::vector<Read> reads;   // this thread's only
  reads.clear();
  for (__int64 spawner : spawners)
  {
    SpawnerInfo info{};
    if (!ReadSpawner(spawner, info))
      continue;
    Read r{};
    r.Entity = spawner;
    r.Enabled = info.Enabled != 0;
    strncpy_s(r.Template, info.Template, _TRUNCATE);
    r.Count = ReadSpawned(spawner, r.Spawned, MaxSpawnedRead);
    r.Room = SpawnerRoom(spawner, r.Count);
    reads.push_back(r);
  }
  std::vector<SpawnedNote> spawned;
  for (Read const& r : reads)
    for (int i = 0; i < r.Count; ++i)
    {
      SpawnedNote note{ r.Spawned[i], r.Entity, -1, {} };
      if (ServerCharacterPlace(note.Entity, note.Place))
        spawned.push_back(note);
    }

  struct Send
  {
    int Id;
    __int64 Spawner;
  };
  std::vector<Send> sends;
  std::vector<std::pair<int, __int64>> removals, ready;
  {
    std::lock_guard<std::mutex> lock(g_StandInMutex);
    g_SpawnerNotes.clear();
    for (Read const& r : reads)
      g_SpawnerNotes.push_back({ r.Entity, r.Enabled, r.Room, TemplateIndex(r.Template) });
    for (SpawnedNote& note : spawned)
      for (SpawnerNote const& s : g_SpawnerNotes)
        if (s.Entity == note.Spawner)
          note.Template = s.Template;
    g_SpawnedNotes.swap(spawned);
    g_SpawnedNotedAt = now;

    if (g_RemoveStandIns)
    {
      // The made ones go now, the ones on their way once made
      std::vector<StandIn> left;
      for (StandIn& s : g_StandIns)
        if (s.State == StandIn_Ready && s.Entity)
          removals.push_back({ s.Id, s.Entity });
        else if (s.State == StandIn_Asked || s.State == StandIn_Cancelled)
        {
          s.State = StandIn_Cancelled;
          left.push_back(std::move(s));
        }
      g_StandIns.swap(left);
      g_RemoveStandIns = false;
    }
    auto owned = [](__int64 entity) {
      for (StandIn const& s : g_StandIns)
        if (s.Entity == entity)
          return true;
      return false;
    };
    auto busy = [](__int64 spawner) {
      for (StandIn const& s : g_StandIns)
        if ((s.State == StandIn_Asked || s.State == StandIn_Cancelled) && s.Spawner == spawner)
          return true;
      return false;
    };
    for (StandIn& s : g_StandIns)
    {
      if (s.State == StandIn_Asked || s.State == StandIn_Cancelled)
      {
        // The new character in that spawner's list
        for (SpawnedNote const& note : g_SpawnedNotes)
          if (note.Spawner == s.Spawner && !owned(note.Entity) &&
              std::find(s.Before.begin(), s.Before.end(), note.Entity) == s.Before.end())
          {
            bool cancelled = s.State == StandIn_Cancelled;
            s.State = cancelled ? StandIn_Gone : StandIn_Ready;
            s.Entity = cancelled ? 0 : note.Entity;
            memcpy(s.Place, note.Place, sizeof(s.Place));
            if (cancelled)
              removals.push_back({ s.Id, note.Entity });
            else
              ready.push_back({ s.Id, s.Entity });
            util::log::Write("Replay: stand-in #%d: the spawner 0x%I64X made the %s 0x%I64X (server) at %.1f %.1f %.1f, %.1f s after "
              "it was asked%s", s.Id, s.Spawner, TemplateNameOf(note.Template), note.Entity, s.Place[0], s.Place[1], s.Place[2],
              (now - s.AskedAt) / 1000.0, cancelled ? " (the playback is over: taken out again)" : "");
            break;
          }
        if (s.State == StandIn_Cancelled && now - s.AskedAt > SpawnWaitMs)
        {
          util::log::Write("Replay: stand-in #%d: its spawner 0x%I64X made none in %.0f s, forgotten", s.Id, s.Spawner,
            SpawnWaitMs / 1000.0);
          s.State = StandIn_Gone;
        }
        if (s.State == StandIn_Asked && now - s.AskedAt > SpawnWaitMs)
        {
          util::log::Write("Replay: stand-in #%d: the spawner 0x%I64X made no %s in %.0f s (at its limit, waiting to spawn out of "
            "sight, or turned down by the AI's pool)%s", s.Id, s.Spawner, TemplateNameOf(s.Template), SpawnWaitMs / 1000.0,
            s.Tries < SpawnTries ? ": another is tried" : "");
          s.Tried.push_back(s.Spawner);
          s.Spawner = 0;
          s.State = s.Tries < SpawnTries ? StandIn_Wanted : StandIn_Failed;
        }
      }
      if (s.State == StandIn_Wanted)
      {
        // An enabled spawner of its own template only (2026-10-27 16:39: one of
        // its folder and kind gave another body, a Sentinel for a Protector;
        // the replay is a 1:1 copy, the user: no stand-in of another model)
        char const* wanted = s.Template >= 0 && s.Template < (int)g_TemplateNames.size() ? g_TemplateNames[s.Template].Name : "";
        SpawnerNote const* best = nullptr;
        bool anyMatch = false;
        for (SpawnerNote const& n : g_SpawnerNotes)
        {
          if (!n.Enabled || !n.Room || n.Template < 0 || IsNoStandInTemplate(g_TemplateNames[n.Template].Name) ||
              TemplateMatch(wanted, g_TemplateNames[n.Template].Name) < 2)
            continue;
          anyMatch = true;
          if (busy(n.Entity) || std::find(s.Tried.begin(), s.Tried.end(), n.Entity) != s.Tried.end())
            continue;
          best = &n;
          break;
        }
        if (best)
        {
          s.State = StandIn_Asked;
          s.Spawner = best->Entity;
          s.AskedAt = now;
          ++s.Tries;
          s.Before.clear();
          for (SpawnedNote const& note : g_SpawnedNotes)
            if (note.Spawner == best->Entity)
              s.Before.push_back(note.Entity);
          sends.push_back({ s.Id, best->Entity });
        }
        else if (!anyMatch || now - s.WantedAt > SpawnWaitMs)
        {
          // None of its template (the notes are this update's), or all busy or tried for a while
          util::log::Write("Replay: stand-in #%d: %s", s.Id, anyMatch ? "every spawner of its template was tried or is busy: none is "
            "made" : "no spawner of its template is on in the level with room for one more: none is made");
          s.State = StandIn_Failed;
        }
      }
      if (s.State == StandIn_Ready)
      {
        bool found = false;
        for (SpawnedNote const& note : g_SpawnedNotes)
          if (note.Entity == s.Entity)
          {
            memcpy(s.Place, note.Place, sizeof(s.Place));
            found = true;
            break;
          }
        // Out of its spawner's list (dead?): where it is, while it's still a live character
        if (!found && !ServerCharacterPlace(s.Entity, s.Place))
        {
          util::log::Write("Replay: stand-in #%d (the %s 0x%I64X) isn't a live character any more", s.Id, TemplateNameOf(s.Template),
            s.Entity);
          s.State = StandIn_Gone;
        }
      }
    }
    // Those done with (StandInState says StandInNone for an id not listed)
    g_StandIns.erase(std::remove_if(g_StandIns.begin(), g_StandIns.end(),
      [](StandIn const& s) { return s.State == StandIn_Failed || s.State == StandIn_Gone; }), g_StandIns.end());
    UpdateStandInWork();
  }

  // Kept by who they are while the replay holds people
  if (!ready.empty())
  {
    std::lock_guard<std::mutex> lock(g_KeptMutex);
    for (auto const& r : ready)
      if (std::find(g_StandInEntities.begin(), g_StandInEntities.end(), r.second) == g_StandInEntities.end())
        g_StandInEntities.push_back(r.second);
  }
  // The game's code, outside the tools' locks
  for (Send const& send : sends)
  {
    if (!IsLiveSpawner(send.Spawner) || !SendSpawn(send.Spawner))
    {
      util::log::Warning("Replay: stand-in #%d: Spawn couldn't be sent to the spawner 0x%I64X (gone, or turned off)", send.Id,
        send.Spawner);
      std::lock_guard<std::mutex> lock(g_StandInMutex);
      for (StandIn& s : g_StandIns)
        if (s.Id == send.Id && s.State == StandIn_Asked)
        {
          s.Tried.push_back(s.Spawner);
          s.Spawner = 0;
          s.State = s.Tries < SpawnTries ? StandIn_Wanted : StandIn_Failed;
        }
      continue;
    }
    util::log::Write("Replay: stand-in #%d: Spawn sent to the spawner 0x%I64X (server update thread %u)", send.Id, send.Spawner,
      GetCurrentThreadId());
  }
  for (auto const& removal : removals)
  {
    __int64 entity = removal.second;
    {
      std::lock_guard<std::mutex> lock(g_KeptMutex);
      g_StandInEntities.erase(std::remove(g_StandInEntities.begin(), g_StandInEntities.end(), entity), g_StandInEntities.end());
      // A kept request for it isn't handed back later
      g_Kept.erase(std::remove_if(g_Kept.begin(), g_Kept.end(), [entity](KeptRequest const& k) { return k.Entity == entity; }),
        g_Kept.end());
    }
    float place[3];
    __int64 owner = EntityOwner(entity);
    if (!ServerCharacterPlace(entity, place) || owner < 0x10000)
    {
      util::log::Write("Replay: stand-in #%d (0x%I64X) was already going or gone", removal.first, entity);
      continue;
    }
    oDestroyRequest(entity, owner);
    util::log::Write("Replay: stand-in #%d, the KrugerSec 0x%I64X (server) at %.1f %.1f %.1f, taken out of the level (the game's "
      "destroy request, server update thread %u)", removal.first, entity, place[0], place[1], place[2], GetCurrentThreadId());
  }
}

bool util::hooks::StandInsKnown()
{
  return g_StandInCode && oSpawnerCtor && oDestroyRequest;
}

void util::hooks::WatchSpawners(bool on)
{
  std::lock_guard<std::mutex> lock(g_StandInMutex);
  g_WatchSpawners = on && StandInsKnown();
  if (!on)
  {
    g_SpawnerNotes.clear();
    g_SpawnedNotes.clear();
    g_SpawnedNotedAt = 0;
  }
  UpdateStandInWork();
}

// The template of the spawner whose character is nearest the place, within
// radius (the server's update notes them once a second)
bool util::hooks::SpawnTemplateNear(float const* place, float radius, char* out, size_t size)
{
  out[0] = 0;
  std::lock_guard<std::mutex> lock(g_StandInMutex);
  float best = radius * radius;
  int bestTemplate = -1;
  for (SpawnedNote const& note : g_SpawnedNotes)
  {
    float dx = note.Place[0] - place[0], dy = note.Place[1] - place[1], dz = note.Place[2] - place[2];
    float d2 = dx * dx + dy * dy + dz * dz;
    if (note.Template >= 0 && d2 <= best)
    {
      best = d2;
      bestTemplate = note.Template;
    }
  }
  if (bestTemplate < 0)
    return false;
  strncpy_s(out, size, g_TemplateNames[bestTemplate].Name, _TRUNCATE);
  return true;
}

// 2: an enabled spawner of that template has room for one more (in the
// server's latest notes); 0: none (one of its folder and kind isn't asked any
// more: its body is another, see ReplayServerUpdate)
int util::hooks::StandInPossible(char const* templateName)
{
  if (!StandInsKnown() || !templateName || !templateName[0] || IsNoStandInTemplate(templateName))
    return 0;
  std::lock_guard<std::mutex> lock(g_StandInMutex);
  for (SpawnerNote const& n : g_SpawnerNotes)
    if (n.Enabled && n.Room && n.Template >= 0 && !IsNoStandInTemplate(g_TemplateNames[n.Template].Name) &&
        TemplateMatch(templateName, g_TemplateNames[n.Template].Name) == 2)
      return 2;
  return 0;
}

bool util::hooks::IsNoStandInTemplate(char const* templateName)
{
  return templateName && templateName[0] && ::IsNoStandInTemplate(templateName);
}

// 2: the same spawn template; 1: the same folder and kind (AIMale, AIRanged...);
// 0: neither
int util::hooks::SpawnTemplatesMatch(char const* a, char const* b)
{
  return a && b && a[0] && b[0] ? TemplateMatch(a, b) : 0;
}

int util::hooks::AskStandIn(char const* templateName)
{
  if (!StandInsKnown() || !templateName || !templateName[0] || ::IsNoStandInTemplate(templateName))
    return 0;
  std::lock_guard<std::mutex> lock(g_StandInMutex);
  int live = 0;
  for (StandIn const& s : g_StandIns)
    live += s.State != StandIn_Failed && s.State != StandIn_Gone;
  if (live >= MaxStandIns || g_RemoveStandIns)
    return 0;
  StandIn s{};
  s.Id = g_NextStandIn++;
  s.Template = TemplateIndex(templateName);
  if (s.Template < 0)
    return 0;
  s.State = StandIn_Wanted;
  s.WantedAt = GetTickCount64();
  g_StandIns.push_back(s);
  UpdateStandInWork();
  return s.Id;
}

int util::hooks::StandInState(int id, float* place)
{
  std::lock_guard<std::mutex> lock(g_StandInMutex);
  for (StandIn const& s : g_StandIns)
    if (s.Id == id)
    {
      if (s.State == StandIn_Ready)
      {
        if (place)
          memcpy(place, s.Place, sizeof(s.Place));
        return StandInReady;
      }
      return s.State == StandIn_Wanted || s.State == StandIn_Asked ? StandInWaiting : StandInNone;
    }
  return StandInNone;
}

// The playback stopped: the stand-ins go at the server's next update
void util::hooks::RemoveStandIns()
{
  std::lock_guard<std::mutex> lock(g_StandInMutex);
  if (g_StandIns.empty())
    return;
  g_RemoveStandIns = true;
  UpdateStandInWork();
}

// The replay let go of its people: the kept requests go back to the game
// (HandBackKept, from the hook), and nothing new is matched
void HandBackKeptLater()
{
  std::lock_guard<std::mutex> lock(g_KeptMutex);
  g_GuardPlaces.clear();
  if (g_Kept.empty())
    return;
  g_HandBackKept = true;
  util::log::Write("Replay: %d characters kept in the level go back to the game (each the next time the game's thread that "
    "asked for it asks for a character to be destroyed)", (int)g_Kept.size());
}

// Held again before all went back: those still kept stay kept, and go back
// with the ones of this hold
void KeepKept()
{
  std::lock_guard<std::mutex> lock(g_KeptMutex);
  g_HandBackKept = false;
}

// The recorded mission's world, kept (2026-10-27; Research_Replay_Mission_
// World_2026-10-26.md section 15). A mission's own world (its gates, doors,
// bus, KrugerSec spawners, its buildings) is a sub-level the mission streams
// in and out: PamServerMissionEntity sends StreamOut (0x21F3F677) or StreamIn
// (0xD27BC17E) to the ServerSubLevelEntity at [[mission + 0xD0]] - 8 or
// [[mission + 0xD8]] - 8, calling its handler (slot 4, 0x14335EFB0) itself,
// from 21 places in its code (MissionWorldCallRvas). Once it's completed or
// aborted, 0x143A7A200 (mission, completed, aborted; called from its update
// 0x143A76D60 at 0x143A7736D) sends StreamOut to + 0xD0's at once, or only
// notes when: its data's StreamOutOnCompletedRadius (+ 0x30) has it wait for
// Faith to be that far (+ 0x120 set, her place at + 0xE8 / + 0xEC), and
// StreamOutOnCompletedDelay / StreamOutOnAbortedDelay (+ 0x34 / + 0x38) for
// the game's time at + 0x118; the update sends it then (Act 1's missions all
// wait 15 - 23 s). So hooking 0x143A7A200 alone would only see the note: it
// is hooked for the log, and the world is kept in the sub-level's handler,
// where every one of those sends ends:
// - A sub-level a mission sends a stream event to (the call's return address
//   is one of the 21), or that 0x143A7A200's mission has, is noted as a
//   mission's world.
// - While the replay records, or keeps a recording and the world wasn't
//   streamed in again after it was stopped, a StreamOut to a mission's world
//   that has something in (+ 0x58, the handler's own test) is kept instead of
//   passed on; so are the stream events sent to it after that in the same
//   server frame (the mission's update re-reads its state at once and may ask
//   for it in again: StreamIn, or Preload 0xCE6D3F44).
// - They go to the game, in their order, as the game made them (an
//   EntityEvent, vtable 0x141A4A8B0, 16 bytes: its id at + 8 and + 0xC, all
//   the handler's copy 0x143144F90 takes) on the server's thread, when a
//   loading screen shows (a load waits for the sub-levels it unloads, see
//   LoadScreenWaitMs), the recording is forgotten or a new one starts, the
//   option is turned off or the menu's button pressed (the server's next
//   update), or the game sends that world a stream event in a later frame
//   (it wants it again: the kept ones first, then that one).
// - A kept world the game destroys (the level it's in unloading) is forgotten
//   (ServerSubLevelEntity's deleting destructor, slot 7, 0x14335D790).
// The mission itself goes on as if its world had gone. The option is REPLAY
// > "Keep the mission's world after it's over".
namespace
{
  const __int64 SubLevelEventRva = 0x335EFB0;      // ServerSubLevelEntity slot 4
  const __int64 SubLevelDtorRva = 0x335D790;       // its slot 7, the deleting destructor
  const __int64 SubLevelDtorBodyRva = 0x335D360;
  const __int64 SubLevelVTableRva = 0x1B47BC0;
  const __int64 EntityEventVTableRva = 0x1A4A8B0;
  const __int64 MissionStreamOutRva = 0x3A7A200;   // PamServerMissionEntity's, once it's over
  const __int64 MissionStreamOutCallRva = 0x3A7736D;
  const __int64 MissionWorldCallRvas[] = { 0x3A76F1C, 0x3A76F89, 0x3A76FF1, 0x3A770AF, 0x3A7725A, 0x3A77290, 0x3A774B6,
    0x3A77585, 0x3A77606, 0x3A7935D, 0x3A794C3, 0x3A79582, 0x3A7A041, 0x3A7A07D, 0x3A7A0FC, 0x3A7A13C, 0x3A7A30A, 0x3A7A558,
    0x3A7A6C1, 0x3A7A73C, 0x3A7A92A };
  const unsigned int StreamOutId = 0x21F3F677;
  const unsigned int StreamInId = 0xD27BC17E;
  const unsigned int PreloadId = 0xCE6D3F44;
  const size_t MaxKeptWorlds = 16;
  const size_t MaxWorldEvents = 8;

  struct WorldEvent
  {
    unsigned int Id;
    unsigned int Extra;   // + 0xC
  };
  struct MissionWorld
  {
    __int64 Entity;
    ULONGLONG InAt;                 // the last stream-in passed to it, 0: none seen
    bool Kept;
    ULONGLONG KeptAt;
    unsigned __int64 KeptFrame;     // the server frame it was kept in
    std::vector<WorldEvent> Events; // kept, in their order
  };
  std::mutex g_WorldsMutex;
  std::vector<MissionWorld> g_Worlds;
  int g_WorldsRecording = 0;          // 0 none kept, 1 recording, 2 kept (stopped at g_WorldsStoppedAt)
  ULONGLONG g_WorldsStoppedAt = 0;
  char const* g_LetWorldsGoWhy = "";
  bool g_WorldsCode = false;
  std::atomic<bool> g_HoldWorlds{ false };
  std::atomic<bool> g_LetWorldsGo{ false };
  std::atomic<int> g_KeptWorlds{ 0 };
  std::atomic<unsigned __int64> g_ServerFrame{ 0 };
  thread_local bool t_SendingWorldEvents = false;   // what the tools send goes through

  struct EntityEvent
  {
    __int64 VTable;
    unsigned int Id;
    unsigned int Extra;
  };

  bool ReadWorldEvent(__int64 event, __int64& vtable, WorldEvent& e)
  {
    __try
    {
      vtable = *(__int64 const*)event;
      e.Id = *(unsigned int const*)(event + 8);
      e.Extra = *(unsigned int const*)(event + 0xC);
      return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      return false;
    }
  }

  bool WorldHasSomethingIn(__int64 entity)
  {
    __try
    {
      return *(__int64 const*)(entity + 0x58) != 0;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      return false;
    }
  }

  bool IsMissionWorldCall(__int64 returnAddress)
  {
    __int64 rva = returnAddress - (__int64)g_gameHandle - 5;
    for (__int64 call : MissionWorldCallRvas)
      if (call == rva)
        return true;
    return false;
  }

  char const* StreamEventName(unsigned int id)
  {
    return id == StreamOutId ? "StreamOut" : id == StreamInId ? "StreamIn" : id == PreloadId ? "Preload" : "another";
  }

  // g_WorldsMutex held
  MissionWorld* FindWorld(__int64 entity)
  {
    for (MissionWorld& world : g_Worlds)
      if (world.Entity == entity)
        return &world;
    return nullptr;
  }

  MissionWorld& NoteWorld(__int64 entity)
  {
    if (MissionWorld* world = FindWorld(entity))
      return *world;
    g_Worlds.push_back({ entity, 0, false, 0, 0, {} });
    return g_Worlds.back();
  }

  bool KeepsWorldNow(MissionWorld const& world)
  {
    if (!g_HoldWorlds.load() || util::hooks::LoadScreenShown())
      return false;
    return g_WorldsRecording == 1 || (g_WorldsRecording == 2 && world.InAt <= g_WorldsStoppedAt);
  }

  int CountKeptWorlds()
  {
    int kept = 0;
    for (MissionWorld const& world : g_Worlds)
      kept += world.Kept;
    return kept;
  }
}

typedef void(__fastcall* tSubLevelEvent)(__int64, __int64);
tSubLevelEvent oSubLevelEvent = nullptr;
typedef __int64(__fastcall* tSubLevelDtor)(__int64, unsigned int);
tSubLevelDtor oSubLevelDtor = nullptr;
typedef void(__fastcall* tMissionStreamOut)(__int64, bool, bool);
tMissionStreamOut oMissionStreamOut = nullptr;

// On the server's thread (its update, or the handler the game called)
static void SendWorldEvents(__int64 entity, std::vector<WorldEvent> const& events)
{
  t_SendingWorldEvents = true;
  for (WorldEvent const& e : events)
  {
    // As the mission makes it (on its stack; its destructor 0x142D4E9A0 only
    // puts the base vtable back)
    EntityEvent event{ (__int64)g_gameHandle + EntityEventVTableRva, e.Id, e.Extra };
    oSubLevelEvent(entity, (__int64)&event);
  }
  t_SendingWorldEvents = false;
}

void __fastcall hSubLevelEvent(__int64 entity, __int64 event)
{
  __int64 vtable = 0;
  WorldEvent e{};
  if (t_SendingWorldEvents || g_shutdown || !ReadWorldEvent(event, vtable, e) ||
      (e.Id != StreamOutId && e.Id != StreamInId && e.Id != PreloadId))
  {
    oSubLevelEvent(entity, event);
    return;
  }
  bool fromMission = IsMissionWorldCall((__int64)_ReturnAddress());
  bool plain = vtable == (__int64)g_gameHandle + EntityEventVTableRva;
  ULONGLONG now = GetTickCount64();
  unsigned __int64 frame = g_ServerFrame.load();
  enum { WorldEvent_Passed, WorldEvent_Kept, WorldEvent_KeptToo, WorldEvent_NotKept } what = WorldEvent_Passed;
  std::vector<WorldEvent> letGo;
  ULONGLONG keptFor = 0;
  int kept = 0;
  {
    std::lock_guard<std::mutex> lock(g_WorldsMutex);
    MissionWorld* world = fromMission ? &NoteWorld(entity) : FindWorld(entity);
    if (world && world->Kept)
    {
      if (frame == world->KeptFrame && plain && world->Events.size() < MaxWorldEvents)
      {
        world->Events.push_back(e);
        what = WorldEvent_KeptToo;
      }
      else
      {
        letGo.swap(world->Events);
        world->Kept = false;
        keptFor = now - world->KeptAt;
      }
    }
    else if (world && e.Id == StreamOutId && KeepsWorldNow(*world) && WorldHasSomethingIn(entity))
    {
      if (plain && CountKeptWorlds() < (int)MaxKeptWorlds)
      {
        world->Kept = true;
        world->KeptAt = now;
        world->KeptFrame = frame;
        world->Events.assign(1, e);
        what = WorldEvent_Kept;
      }
      else
        what = WorldEvent_NotKept;
    }
    if (world && what == WorldEvent_Passed && e.Id != StreamOutId)
      world->InAt = now;
    kept = CountKeptWorlds();
    g_KeptWorlds.store(kept);
  }
  __int64 from = (__int64)_ReturnAddress() - (__int64)g_gameHandle + 0x140000000;
  if (what == WorldEvent_Kept)
    util::log::Write("Replay: the mission's world 0x%I64X is kept for the replay: its StreamOut (sent from 0x%I64X%s) waits "
      "(%d kept)", entity, from, fromMission ? ", the mission's code" : "", kept);
  else if (what == WorldEvent_KeptToo)
    util::log::Write("Replay: the kept mission world 0x%I64X: its %s (from 0x%I64X) waits too, after the StreamOut", entity,
      StreamEventName(e.Id), from);
  else if (what == WorldEvent_NotKept)
    util::log::Write("Replay: the mission's world 0x%I64X streams out (its StreamOut from 0x%I64X): %s", entity, from,
      plain ? "already as many kept as the tools keep" : "the event isn't the plain one the tools know");
  if (!letGo.empty())
  {
    util::log::Write("Replay: the kept mission world 0x%I64X goes: the game sent it %s (from 0x%I64X) %.0f s after; its %d kept "
      "stream event(s) first", entity, StreamEventName(e.Id), from, keptFor / 1000.0, (int)letGo.size());
    SendWorldEvents(entity, letGo);
  }
  if (what != WorldEvent_Kept && what != WorldEvent_KeptToo)
    oSubLevelEvent(entity, event);
}

__int64 __fastcall hSubLevelDtor(__int64 entity, unsigned int flags)
{
  bool kept = false;
  size_t events = 0;
  {
    std::lock_guard<std::mutex> lock(g_WorldsMutex);
    for (size_t i = 0; i < g_Worlds.size(); ++i)
      if (g_Worlds[i].Entity == entity)
      {
        kept = g_Worlds[i].Kept;
        events = g_Worlds[i].Events.size();
        g_Worlds.erase(g_Worlds.begin() + i);
        g_KeptWorlds.store(CountKeptWorlds());
        break;
      }
  }
  if (kept)
    util::log::Write("Replay: the kept mission world 0x%I64X was destroyed by the game (the level it's in unloading?); its %d "
      "kept stream event(s) aren't sent (thread %u)", entity, (int)events, GetCurrentThreadId());
  return oSubLevelDtor(entity, flags);
}

namespace
{
  struct MissionEnd
  {
    __int64 World, OtherWorld;
    float Radius, CompletedDelay, AbortedDelay;
    float At;      // + 0x118, the game's time it's sent at
    BYTE WaitFar;  // + 0x120
  };

  __int64 MissionWorldAt(__int64 mission, int offset)
  {
    __int64 holder = *(__int64 const*)(mission + offset);
    __int64 world = holder ? *(__int64 const*)holder : 0;
    return world ? world - 8 : 0;
  }

  bool ReadMissionEnd(__int64 mission, MissionEnd& end)
  {
    __try
    {
      __int64 data = *(__int64 const*)(mission + 0x28);
      end.Radius = *(float const*)(data + 0x30);
      end.CompletedDelay = *(float const*)(data + 0x34);
      end.AbortedDelay = *(float const*)(data + 0x38);
      end.At = *(float const*)(mission + 0x118);
      end.WaitFar = *(BYTE const*)(mission + 0x120);
      end.World = MissionWorldAt(mission, 0xD0);
      end.OtherWorld = MissionWorldAt(mission, 0xD8);
      return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      return false;
    }
  }
}

// For the log: what the mission does with its world once it's over
void __fastcall hMissionStreamOut(__int64 mission, bool completed, bool aborted)
{
  MissionEnd before{};
  bool read = ReadMissionEnd(mission, before);
  int recording = 0;
  if (read && !g_shutdown)
  {
    std::lock_guard<std::mutex> lock(g_WorldsMutex);
    for (__int64 world : { before.World, before.OtherWorld })
      if (world)
        NoteWorld(world);
    recording = g_WorldsRecording;
  }
  oMissionStreamOut(mission, completed, aborted);
  MissionEnd after{};
  if (g_shutdown || !read || !ReadMissionEnd(mission, after))
    return;
  char when[160];
  if (after.WaitFar)
    sprintf_s(when, "once Faith is %.0f m away (its StreamOutOnCompletedRadius)", after.Radius);
  else
    when[0] = 0;
  if (after.At > 0.f && after.At != before.At)
    sprintf_s(when + strlen(when), sizeof(when) - strlen(when), "%sat the game's time %.1f (its delay: %.0f s completed, %.0f s "
      "aborted)", when[0] ? ", and " : "", after.At, after.CompletedDelay, after.AbortedDelay);
  util::log::Write("Replay: the mission 0x%I64X is over (%s): its world 0x%I64X (the other 0x%I64X) streams out %s; the replay "
    "%s", mission, completed ? "completed" : aborted ? "aborted" : "neither completed nor aborted", after.World, after.OtherWorld,
    when[0] ? when : "now (the lines before say if it was kept) or not at all", g_HoldWorlds.load() ? (recording ?
    "keeps it when it does (if it has something in)" : "has no recording: it isn't kept") : "doesn't keep it (the option is off)");
}

static bool IsMissionWorldCode(BYTE const* pBase)
{
  auto calls = [pBase](__int64 at, __int64 target) {
    return pBase[at] == 0xE8 && at + 5 + *(int const*)(pBase + at + 1) == target;
  };
  auto slot = [pBase](__int64 vtable, int index, __int64 target) {
    return *(__int64 const*)(pBase + vtable + index * 8) == (__int64)pBase + target;
  };
  // 0x14335EFB0: mov [rsp + 0x18], rbx; mov [rsp + 0x20], rsi; push rdi; sub rsp, 0x30; mov eax, [rdx + 8]; mov rsi, rdx;
  // mov rbx, rcx; cmp eax, Preload; je; cmp eax, StreamIn; je; cmp eax, StreamOut; je + 0x6C
  static const BYTE handler[] = { 0x48, 0x89, 0x5C, 0x24, 0x18, 0x48, 0x89, 0x74, 0x24, 0x20, 0x57, 0x48, 0x83, 0xEC, 0x30, 0x8B,
    0x42, 0x08, 0x48, 0x89, 0xD6, 0x48, 0x89, 0xCB, 0x3D, 0x44, 0x3F, 0x6D, 0xCE, 0x0F, 0x84, 0x15, 0x01, 0x00, 0x00, 0x3D, 0x7E,
    0xC1, 0x7B, 0xD2, 0x0F, 0x84, 0x11, 0x01, 0x00, 0x00, 0x3D, 0x77, 0xF6, 0xF3, 0x21, 0x74, 0x37 };
  // + 0x6C (StreamOut): cmp qword [rcx + 0x58], 0; je
  static const BYTE streamOut[] = { 0x48, 0x83, 0x79, 0x58, 0x00, 0x0F, 0x84 };
  // 0x14335D790: mov [rsp + 8], rbx; push rdi; sub rsp, 0x20; mov ebx, edx; mov rdi, rcx; call the body
  static const BYTE dtor[] = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x89, 0xD3, 0x48, 0x89, 0xCF };
  // The body puts the vtable back: + 0x24 lea rax, [rip + vtable]; mov [rcx], rax
  // 0x143144F90 + 0x27: lea rcx, [rip + EntityEvent's vtable]; mov [rax], rcx; mov edx, [rbx + 8]; mov [rax + 8], edx;
  // mov ecx, [rbx + 0xC]; mov [rax + 0xC], ecx (the event's copy: 16 bytes)
  static const BYTE copy[] = { 0x48, 0x89, 0x08, 0x8B, 0x53, 0x08, 0x89, 0x50, 0x08, 0x8B, 0x4B, 0x0C, 0x89, 0x48, 0x0C };
  const __int64 eventCopyRva = 0x3144F90;
  // 0x143A7A200: push rdi; sub rsp, 0x50; mov qword [rsp + 0x20], -2; mov [rsp + 0x60], rbx; mov [rsp + 0x68], rsi;
  // movaps [rsp + 0x40], xmm6; movzx esi, r8b; mov rbx, rcx; mov dil, 1; xorps xmm6, xmm6; test dl, dl; je;
  // mov rax, [rcx + 0x28]; movss xmm0, [rax + 0x30]
  static const BYTE missionEnd[] = { 0x57, 0x48, 0x83, 0xEC, 0x50, 0x48, 0xC7, 0x44, 0x24, 0x20, 0xFE, 0xFF, 0xFF, 0xFF, 0x48, 0x89,
    0x5C, 0x24, 0x60, 0x48, 0x89, 0x74, 0x24, 0x68, 0x0F, 0x29, 0x74, 0x24, 0x40, 0x41, 0x0F, 0xB6, 0xF0, 0x48, 0x89, 0xCB, 0x40,
    0xB7, 0x01, 0x0F, 0x57, 0xF6, 0x84, 0xD2, 0x0F, 0x84, 0x8C, 0x00, 0x00, 0x00, 0x48, 0x8B, 0x41, 0x28, 0xF3, 0x0F, 0x10, 0x40,
    0x30 };
  // + 0xD6: mov rax, [rbx + 0xD0]; test rax, rax; je; mov rcx, [rax]; test rcx, rcx; je; add rcx, -8; je; lea rax, [EntityEvent's
  // vtable]; mov [rsp + 0x28], rax; mov qword [rsp + 0x30], StreamOut; lea rdx, [rsp + 0x28]; call the handler
  static const BYTE send1[] = { 0x48, 0x8B, 0x83, 0xD0, 0x00, 0x00, 0x00, 0x48, 0x85, 0xC0, 0x74, 0x38, 0x48, 0x8B, 0x08, 0x48, 0x85,
    0xC9, 0x74, 0x30, 0x48, 0x83, 0xC1, 0xF8, 0x74, 0x2A, 0x48, 0x8D, 0x05 };
  static const BYTE send2[] = { 0x48, 0x89, 0x44, 0x24, 0x28, 0x48, 0xC7, 0x44, 0x24, 0x30, 0x77, 0xF6, 0xF3, 0x21, 0x48, 0x8D, 0x54,
    0x24, 0x28 };
  auto ripTarget = [pBase](__int64 at) { return at + 7 + *(int const*)(pBase + at + 3); };
  if (memcmp(pBase + SubLevelEventRva, handler, sizeof(handler)) || memcmp(pBase + SubLevelEventRva + 0x6C, streamOut,
      sizeof(streamOut)) || !slot(SubLevelVTableRva, 4, SubLevelEventRva))
    return false;
  if (memcmp(pBase + SubLevelDtorRva, dtor, sizeof(dtor)) || !calls(SubLevelDtorRva + 0xF, SubLevelDtorBodyRva) ||
      !slot(SubLevelVTableRva, 7, SubLevelDtorRva) || pBase[SubLevelDtorBodyRva + 0x24] != 0x48 ||
      pBase[SubLevelDtorBodyRva + 0x25] != 0x8D || ripTarget(SubLevelDtorBodyRva + 0x24) != SubLevelVTableRva)
    return false;
  if (*(__int64 const*)(pBase + EntityEventVTableRva + 2 * 8) != (__int64)pBase + eventCopyRva ||
      ripTarget(eventCopyRva + 0x27) != EntityEventVTableRva || memcmp(pBase + eventCopyRva + 0x2E, copy, sizeof(copy)))
    return false;
  if (memcmp(pBase + MissionStreamOutRva, missionEnd, sizeof(missionEnd)) || !calls(MissionStreamOutCallRva, MissionStreamOutRva) ||
      memcmp(pBase + MissionStreamOutRva + 0xD6, send1, sizeof(send1)) || ripTarget(MissionStreamOutRva + 0xF0) != EntityEventVTableRva ||
      memcmp(pBase + MissionStreamOutRva + 0xF7, send2, sizeof(send2)) || !calls(MissionStreamOutRva + 0x10A, SubLevelEventRva))
    return false;
  for (__int64 call : MissionWorldCallRvas)
    if (!calls(call, SubLevelEventRva))
      return false;
  return true;
}

// From the server's update, on its thread: the kept worlds go when asked to
// or when a loading screen shows
static void ServeKeptWorlds()
{
  g_ServerFrame.fetch_add(1);
  if (!g_WorldsCode || g_KeptWorlds.load() == 0)
  {
    g_LetWorldsGo.store(false);
    return;
  }
  char const* why = nullptr;
  if (g_LetWorldsGo.exchange(false))
  {
    std::lock_guard<std::mutex> lock(g_WorldsMutex);
    why = g_LetWorldsGoWhy;
  }
  else if (util::hooks::LoadScreenShown())
    why = "a loading screen showed";
  if (!why)
    return;
  ULONGLONG now = GetTickCount64();
  for (;;)
  {
    __int64 entity = 0;
    std::vector<WorldEvent> events;
    ULONGLONG keptAt = 0;
    {
      std::lock_guard<std::mutex> lock(g_WorldsMutex);
      for (MissionWorld& world : g_Worlds)
        if (world.Kept)
        {
          entity = world.Entity;
          events.swap(world.Events);
          keptAt = world.KeptAt;
          world.Kept = false;
          break;
        }
      g_KeptWorlds.store(CountKeptWorlds());
    }
    if (!entity)
      break;
    util::log::Write("Replay: the kept mission world 0x%I64X goes (%s), kept %.0f s: its %d stream event(s) to the game", entity,
      why, (now - keptAt) / 1000.0, (int)events.size());
    SendWorldEvents(entity, events);
  }
}

bool util::hooks::MissionWorldsKnown()
{
  return g_WorldsCode;
}

void util::hooks::HoldMissionWorlds(bool hold)
{
  if (g_HoldWorlds.exchange(hold) == hold || !g_WorldsCode)
    return;
  util::log::Write("Replay: the mission's world is %s", hold ? "kept after it's over while a recording is kept" :
    "no longer kept after it's over");
  if (!hold)
    LetMissionWorldsGo("the option was turned off");
}

void util::hooks::MissionWorldsRecording(int state)
{
  if (!g_WorldsCode)
    return;
  {
    std::lock_guard<std::mutex> lock(g_WorldsMutex);
    g_WorldsRecording = state;
    if (state == 2)
      g_WorldsStoppedAt = GetTickCount64();
  }
  // None kept from a recording that went on (LetMissionWorldsGo: only if any are)
  if (state != 2)
    LetMissionWorldsGo(state == 1 ? "a new recording started" : "no recording is kept: forgotten, or too short to play");
}

void util::hooks::LetMissionWorldsGo(char const* why)
{
  if (!g_WorldsCode)
    return;
  std::lock_guard<std::mutex> lock(g_WorldsMutex);
  if (!CountKeptWorlds())
    return;
  g_LetWorldsGoWhy = why;
  g_LetWorldsGo.store(true);
}

int util::hooks::KeptMissionWorlds()
{
  return g_KeptWorlds.load();
}

// Diagnostic (temporary): what takes the replay's people out of the level in
// a way the destroy request (0x143167700) doesn't see. A whole KrugerSec
// squad, with the ones kept, went in one frame as a chase ended (2026-10-23
// 22:44:19, 317.9 s into a recording), with no destroy request for any of
// them. The characters' destructors only ever had the realm's destroy-list
// flush (0x14311EA50, from each realm's update) on the stack, which destroys
// what was put on that list earlier. Entities go on it from their container
// (vtables 0x141B962C8, one per character, its + 0x50 the character;
// 0x141B96470 / 0x141B42D58 / 0x141AF6560: a character's own container sits in
// one of those, the squad's, with the characters in its list too; + 0x10 the
// list, an entity's node at its + 0x08, the next at node + 0x08; + 0x48 a
// reference count, 0x14316BC50 the release; [entity + 0x28] points at a
// pointer to the entity's container): slot 2 (0x143125180, container,
// entity) takes one entity out (the destroy request calls it), 0x14311E9B0
// (container, realm) takes all of them out at once, e.g. a ServerBlueprintEntity
// taking down its content (slot 17 0x1433648B0 -> 0x14335E820), or an entity's
// slot 6 while the flush destroys it (the flush calls slot 6 first, at
// 0x14311EAEB). While the replay holds people both log the game's code on the
// stack when they take out a character near one of the recorded people (16
// each a hold); one taken out from inside the flush also lists the entities
// queued shortly before it (one of them is what took it along).
typedef void(__fastcall* tContainerDestroyAll)(__int64, int);
typedef void(__fastcall* tContainerDestroyOne)(__int64, __int64);
tContainerDestroyAll oContainerDestroyAll = nullptr;
tContainerDestroyOne oContainerDestroyOne = nullptr;
const __int64 ContainerDestroyAllRva = 0x311E9B0;
const __int64 ContainerDestroyOneRva = 0x3125180;
const __int64 ContainerVTableRva = 0x1B962C8;      // a character's own container
const __int64 FlushSlot6ReturnRva = 0x311EAEE;     // in the destroy-list flush, after its call of an entity's slot 6
const int ContainerLogsPerHold = 16;

namespace
{
  struct QueuedEntity
  {
    __int64 Entity;
    __int64 TypeInfo;
    char Stack[28 * 12 + 1];
  };
  std::mutex g_QueuedMutex;
  QueuedEntity g_Queued[24];      // under g_QueuedMutex, the latest slot 2 calls while the hold is on
  int g_QueuedNext = 0;
  std::atomic<int> g_AllLogs{ 0 }, g_OneLogs{ 0 };
  std::atomic<int> g_AllWithPeople{ 0 }, g_AllNearRecorded{ 0 }, g_OneNearRecorded{ 0 };
}

// An object's class name from its getType (vtable [0]: lea rax, [rip + its
// TypeInfo]; ret; TypeInfo + 0 -> its info, info + 0 -> the name), or "?"
static char const* ClassNameOf(__int64 object, __int64* pTypeInfo = nullptr)
{
  __int64 base = (__int64)g_gameHandle;
  IMAGE_NT_HEADERS const* pNt = (IMAGE_NT_HEADERS const*)(base + ((IMAGE_DOS_HEADER const*)base)->e_lfanew);
  __int64 end = base + pNt->OptionalHeader.SizeOfImage;
  __try
  {
    if (object < 0x10000)
      return "-";
    __int64 vtable = *(__int64 const*)object;
    if (vtable < base || vtable >= end)
      return "?";
    BYTE const* code = *(BYTE const**)vtable;
    if ((__int64)code < base || (__int64)code >= end || code[0] != 0x48 || code[1] != 0x8D || code[2] != 0x05 || code[7] != 0xC3)
      return "?";
    __int64 typeInfo = (__int64)code + 7 + *(int const*)(code + 3);
    if (pTypeInfo)
      *pTypeInfo = typeInfo;
    char const* name = *(char const**)*(__int64 const*)typeInfo;
    return (__int64)name >= base && (__int64)name < end ? name : "?";
  }
  __except (EXCEPTION_EXECUTE_HANDLER)
  {
    return "?";
  }
}

// A server or client character (0: neither, 1 server, 2 client) and its place
static int CharacterOf(__int64 entity, float* place)
{
  __int64 base = (__int64)g_gameHandle;
  __try
  {
    __int64 vtable = *(__int64 const*)entity;
    int kind = vtable == base + ServerCharacterVTableRva ? 1 : vtable == base + ClientCharacterVTableRva ? 2 : 0;
    if (!kind)
      return 0;
    BYTE const* components = *(BYTE const**)(entity + 0x38);
    if ((__int64)components < 0x10000)
      return 0;
    memcpy(place, components + 0x40 + ((__int64)components[9] + 2 * components[0xA]) * 0x20, 3 * sizeof(float));
    return kind;
  }
  __except (EXCEPTION_EXECUTE_HANDLER)
  {
    return 0;
  }
}

static bool NearRecorded(float const* place)
{
  std::lock_guard<std::mutex> lock(g_KeptMutex);
  float radius2 = g_GuardRadius * g_GuardRadius;
  for (size_t i = 0; i + 2 < g_GuardPlaces.size(); i += 3)
  {
    float dx = place[0] - g_GuardPlaces[i], dy = place[1] - g_GuardPlaces[i + 1], dz = place[2] - g_GuardPlaces[i + 2];
    if (dx * dx + dy * dy + dz * dz <= radius2)
      return true;
  }
  return false;
}

static __int64 ContainerRoot(__int64 container)
{
  __try
  {
    return *(__int64 const*)(container + 0x50);
  }
  __except (EXCEPTION_EXECUTE_HANDLER)
  {
    return 0;
  }
}

void __fastcall hContainerDestroyOne(__int64 container, __int64 entity)
{
  // (a container's own + 0x50 is handed on to the container above it: logged there)
  if (g_HoldAIDespawn.load(std::memory_order_relaxed) && ContainerRoot(container) != entity)
  {
    QueuedEntity queued{ entity, 0, {} };
    ClassNameOf(entity, &queued.TypeInfo);
    GameStack(queued.Stack, sizeof(queued.Stack));
    {
      std::lock_guard<std::mutex> lock(g_QueuedMutex);
      g_Queued[g_QueuedNext] = queued;
      g_QueuedNext = (g_QueuedNext + 1) % (int)_countof(g_Queued);
    }
    float place[3];
    int kind = CharacterOf(entity, place);
    if (kind && NearRecorded(place))
    {
      g_OneNearRecorded.fetch_add(1, std::memory_order_relaxed);
      if (g_OneLogs.fetch_add(1, std::memory_order_relaxed) < ContainerLogsPerHold)
        util::log::Write("Replay (diagnostic): the game takes the %s character 0x%I64X at %.1f %.1f %.1f (near a recorded person) "
          "out of its container 0x%I64X; its code on the stack:%s", kind == 1 ? "server" : "client", entity, place[0], place[1],
          place[2], container, queued.Stack);
    }
  }
  oContainerDestroyOne(container, entity);
}

// The characters in a container's list (their places, up to 64), read inside
// __try (game memory)
struct ContainerPeople
{
  int Entities;
  int Characters;
  int Placed;
  __int64 Entity[64];
  float Place[64][3];
};

static void ReadContainerPeople(__int64 container, ContainerPeople& out)
{
  out.Entities = out.Characters = out.Placed = 0;
  __try
  {
    __int64 node = *(__int64 const*)(container + 0x10);
    for (; node >= 0x10000 && out.Entities < 4096; node = *(__int64 const*)(node + 8))
    {
      ++out.Entities;
      float place[3];
      if (!CharacterOf(node - 8, place))
        continue;
      ++out.Characters;
      if (out.Placed < 64)
      {
        out.Entity[out.Placed] = node - 8;
        memcpy(out.Place[out.Placed++], place, sizeof(place));
      }
    }
  }
  __except (EXCEPTION_EXECUTE_HANDLER) {}
}

static void ReadContainerFields(__int64 container, __int64& vtable, __int64& data, __int64& root)
{
  vtable = data = root = 0;
  __try
  {
    vtable = *(__int64 const*)container;
    data = *(__int64 const*)(container + 0x40);
    root = *(__int64 const*)(container + 0x50);
  }
  __except (EXCEPTION_EXECUTE_HANDLER) {}
}

static char const* TypeInfoName(__int64 typeInfo)
{
  __try
  {
    return typeInfo ? *(char const**)*(__int64 const*)typeInfo : "?";
  }
  __except (EXCEPTION_EXECUTE_HANDLER)
  {
    return "?";
  }
}

void __fastcall hContainerDestroyAll(__int64 container, int realm)
{
  // The recorded and noted sounds whose entities go with it (see g_SoundGen)
  SoundContainerGone(container);
  if (util::DiagnosticsOn() && g_HoldAIDespawn.load(std::memory_order_relaxed))
  {
    ContainerPeople people;
    ReadContainerPeople(container, people);
    int nearRecorded = 0, first = -1;
    for (int i = 0; i < people.Placed; ++i)
      if (NearRecorded(people.Place[i]))
      {
        ++nearRecorded;
        if (first < 0)
          first = i;
      }
    if (people.Characters)
      g_AllWithPeople.fetch_add(1, std::memory_order_relaxed);
    if (nearRecorded)
    {
      g_AllNearRecorded.fetch_add(1, std::memory_order_relaxed);
      if (g_AllLogs.fetch_add(1, std::memory_order_relaxed) < ContainerLogsPerHold)
      {
        char stack[28 * 12 + 1];
        GameStack(stack, sizeof(stack));
        __int64 base = (__int64)g_gameHandle;
        __int64 vtable, data, root;
        ReadContainerFields(container, vtable, data, root);
        util::log::Write("Replay (diagnostic): the game takes all %d entities of the container 0x%I64X (vtable 0x%I64X, realm %d, + 0x40 "
          "0x%I64X %s, + 0x50 0x%I64X %s) out of the level: %d characters, %d of them near the recorded people (0x%I64X at %.1f "
          "%.1f %.1f); its code on the stack:%s", people.Entities, container, vtable ? vtable - base + 0x140000000 : 0, realm, data,
          ClassNameOf(data), root, ClassNameOf(root), people.Characters, nearRecorded, people.Entity[first],
          people.Place[first][0], people.Place[first][1], people.Place[first][2], stack);
        // From inside the flush: an entity queued before took this container
        // along (its slot 6). The ones queued lately, newest first.
        char flushReturn[20];
        sprintf_s(flushReturn, " %I64X", FlushSlot6ReturnRva + 0x140000000);
        if (strstr(stack, flushReturn))
        {
          std::lock_guard<std::mutex> lock(g_QueuedMutex);
          int n = (int)_countof(g_Queued);
          for (int k = 1; k <= n; ++k)
          {
            QueuedEntity const& queued = g_Queued[(g_QueuedNext - k + n) % n];
            if (!queued.Entity)
              break;
            util::log::Write("Replay (diagnostic):   queued before it%s: %s 0x%I64X, its code on the stack:%s",
              queued.Entity == root ? " (the container's + 0x50)" : "", TypeInfoName(queued.TypeInfo), queued.Entity, queued.Stack);
          }
        }
      }
    }
  }
  oContainerDestroyAll(container, realm);
}

// And a character near the recorded people destroyed other than by the
// destroy-list flush (its call of the destructor returns to 0x14311EB0A): its
// destructor logs the game's code on the stack (16 a hold)
const __int64 FlushDtorReturnRva = 0x311EB0A;
namespace
{
  std::atomic<int> g_DtorLogs{ 0 }, g_DtorOutsideFlush{ 0 };
}

static void NoteCharacterDtor(__int64 entity)
{
  float place[3];
  int kind = CharacterOf(entity, place);
  if (!kind || !NearRecorded(place))
    return;
  char stack[28 * 12 + 1];
  GameStack(stack, sizeof(stack));
  char flushReturn[20];
  sprintf_s(flushReturn, " %I64X", FlushDtorReturnRva + 0x140000000);
  if (strstr(stack, flushReturn))
    return;
  g_DtorOutsideFlush.fetch_add(1, std::memory_order_relaxed);
  if (g_DtorLogs.fetch_add(1, std::memory_order_relaxed) < ContainerLogsPerHold)
    util::log::Write("Replay (diagnostic): the %s character 0x%I64X at %.1f %.1f %.1f (near a recorded person) is destroyed "
      "outside the destroy-list flush; its code on the stack:%s", kind == 1 ? "server" : "client", entity, place[0], place[1],
      place[2], stack);
}

tCharacterDtor oClientCharacterDtor = nullptr;
static void DropCullBox(__int64 character);   // see util::hooks::SetCullBox

__int64 __fastcall hClientCharacterDtor(__int64 entity, unsigned int flags)
{
  if (util::DiagnosticsOn() && g_HoldAIDespawn.load(std::memory_order_relaxed))
    NoteCharacterDtor(entity);
  DropCullBox(entity);
  return oClientCharacterDtor(entity, flags);
}

// When a hold ends: how often the game took people out those ways meanwhile
void LogContainerCounts()
{
  int all = g_AllWithPeople.exchange(0), allNear = g_AllNearRecorded.exchange(0), oneNear = g_OneNearRecorded.exchange(0);
  int outside = g_DtorOutsideFlush.exchange(0);
  g_AllLogs = 0;
  g_OneLogs = 0;
  g_DtorLogs = 0;
  {
    std::lock_guard<std::mutex> lock(g_QueuedMutex);
    for (auto& queued : g_Queued)
      queued.Entity = 0;
    g_QueuedNext = 0;
  }
  if (all || oneNear || outside)
    util::log::Write("Replay (diagnostic): while the replay held people, the game took %d containers with characters in them out of "
      "the level (%d with some near the recorded people), %d characters near them one by one outside the destroy request, and "
      "destroyed %d near them outside the destroy-list flush", all, allNear, oneNear, outside);
}

// Replay people, kept: a chase's own sub-level. The KrugerSec a chase (the
// game's City Alert) brings are in a sub-level that's unloaded when it ends
// (2026-10-24 00:29:30, from the hooks above: the server's sub-level streaming
// update 0x1432F1D60, called by 0x1432F6390 from the server update
// 0x1433445B0, gave its manager's slot 3, 0x143340000 (manager, request: + 0x00
// the sub-level's id, + 0x10 its container), the request; that took the
// container's entities out at once (its sub-level object's slot 12 -> the
// container's slot 11 -> 0x14311E9B0), 16 recorded KrugerSec with them and
// the 8 kept ones; their client bodies went after them, as the network copies
// of what the server destroyed). Each tick that update calls slot 3 for every
// sub-level waiting to unload, and when slot 3 says "not done" (al 0, the
// test at 0x1432F2246) it leaves that sub-level as it is and asks again next
// tick (nothing else of it is unloaded before: its container, data and
// bundle are only let go after a "done"). The game's own slot 3 always says
// done. While the replay holds people, a sub-level whose container holds a
// live character or drone near a recorded person (its own container,
// [[entity + 0x28]], or one above it, + 0x18) is told "not done", only when
// asked from that update (its return address); decided the first time it's
// asked. Once the replay lets go of its people (or Faith goes: dead, a level
// load), or its recording is forgotten or a new one starts
// (ReleaseKeptSubLevels), it unloads as it would have, the kept ones with it
// (DropKept). Not known:
// what the game does if it wants that sub-level loaded again meanwhile (a new
// chase while the replay holds people).
typedef bool(__fastcall* tSubLevelUnload)(__int64, __int64);
tSubLevelUnload oSubLevelUnload = nullptr;
const __int64 SubLevelUnloadRva = 0x3340000;
const __int64 SubLevelUnloadReturnRva = 0x32F2246;   // in the streaming update, after its call of slot 3
const __int64 StreamingManagerVTableRva = 0x1B42E40;
struct KeptPeopleType
{
  __int64 TypeInfoRva;
  char const* Name;
};
const KeptPeopleType KeptPeopleTypes[] = {
  { 0x28544B0, "PamServerCharacterEntity" },
  { 0x2853B00, "PamClientCharacterEntity" },
  { 0x28575E0, "PamServerDroneActorEntity" },
  { 0x27FF880, "ServerVehicleEntity" },         // a recorded one by who it is (see SetKeptVehicles)
};
const int InstanceListsOffset = 0x60;     // a TypeInfo's 4 lists of its instances, 0x10 apart
const int InstanceLinkOffset = 0x40;      // an instance's link in its list
const int MaxInstancesPerList = 4096;
const ULONGLONG SubLevelLogEveryMs = 30000;

namespace
{
  struct KeptSubLevel
  {
    __int64 Container;
    unsigned int Id;
    ULONGLONG Since, LoggedAt;
    int Asks;
  };
  std::mutex g_SubLevelMutex;
  std::vector<KeptSubLevel> g_SubLevels;              // under g_SubLevelMutex
  bool g_KeptPeopleTypeOk[_countof(KeptPeopleTypes)] = {};  // their TypeInfo names checked, at Init
}

static bool TypeInfoNamed(__int64 typeInfo, char const* name)
{
  __try
  {
    return !strcmp(*(char const**)*(__int64 const*)typeInfo, name);
  }
  __except (EXCEPTION_EXECUTE_HANDLER)
  {
    return false;
  }
}

// Whether a container is an entity's own or one above that
static bool ContainerHolds(__int64 container, __int64 entity)
{
  __try
  {
    __int64 handle = *(__int64 const*)(entity + 0x28);
    if (handle < 0x10000)
      return false;
    __int64 c = *(__int64 const*)handle;
    for (int depth = 0; c >= 0x10000 && depth < 16; ++depth, c = *(__int64 const*)(c + 0x18))
      if (c == container)
        return true;
    return false;
  }
  __except (EXCEPTION_EXECUTE_HANDLER)
  {
    return false;
  }
}

// The instances of a class (one of its TypeInfo's lists) that a container
// holds, with their places (up to 64 in all)
struct PeopleIn
{
  int Count;
  __int64 Entity[64];
  int Type[64];
  float Place[64][3];
};

static void WalkPeopleIn(__int64 head, __int64 typeInfo, int type, __int64 container, PeopleIn& out)
{
  __try
  {
    __int64 link = *(__int64 const*)head;
    for (int n = 0; link >= 0x10000 && n < MaxInstancesPerList && out.Count < 64; ++n, link = *(__int64 const*)link)
    {
      __int64 entity = link - InstanceLinkOffset;
      BYTE const* code = *(BYTE const**)*(__int64 const*)entity;
      if (code[0] != 0x48 || code[1] != 0x8D || code[2] != 0x05 || (__int64)code + 7 + *(int const*)(code + 3) != typeInfo ||
          !ContainerHolds(container, entity))
        continue;
      BYTE const* components = *(BYTE const**)(entity + 0x38);
      if ((__int64)components < 0x10000)
        continue;
      memcpy(out.Place[out.Count], components + 0x40 + ((__int64)components[9] + 2 * components[0xA]) * 0x20, 3 * sizeof(float));
      out.Type[out.Count] = type;
      out.Entity[out.Count++] = entity;
    }
  }
  __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// The first character or drone near a recorded person that a container holds
// (index into people), -1 for none; nearCount: how many
static int FindRecordedIn(__int64 container, PeopleIn& people, int& nearCount)
{
  __int64 base = (__int64)g_gameHandle;
  people.Count = 0;
  for (int t = 0; t < (int)_countof(KeptPeopleTypes); ++t)
    if (g_KeptPeopleTypeOk[t])
      for (int list = 0; list < 4; ++list)
        WalkPeopleIn(base + KeptPeopleTypes[t].TypeInfoRva + InstanceListsOffset + list * 0x10, base + KeptPeopleTypes[t].TypeInfoRva,
          t, container, people);
  int first = -1;
  nearCount = 0;
  for (int i = 0; i < people.Count; ++i)
    if (NearRecorded(people.Place[i]) || IsKeptVehicle(people.Entity[i]))
    {
      ++nearCount;
      if (first < 0)
        first = i;
    }
  return first;
}

static bool ReadUnloadRequest(__int64 request, unsigned int& id, __int64& container)
{
  __try
  {
    id = *(unsigned int const*)request;
    container = *(__int64 const*)(request + 0x10);
    return container >= 0x10000;
  }
  __except (EXCEPTION_EXECUTE_HANDLER)
  {
    return false;
  }
}

// A body's place as the game takes it for its placement: 0x1432E06B0(body,
// out 4x4) reads the body's rows, or its physics part's transform when it has
// one (a dropped gun), and its only caller is the placement 0x1432EA220 in the
// body's update (2026-10-24). The replay's guns: holding their update left
// them drawn where they lie (19:55 log: the drawn bone 1 stayed on the ground
// while the replayed one was 4.5 m away in a hand), so instead the game keeps
// updating them and this gives the placement the replay's rotation and place
// (the rows' fourth floats as the game made them).
typedef bool(__fastcall* tBodyTransform)(__int64, float*);
tBodyTransform oBodyTransform = nullptr;
const __int64 BodyTransformRva = 0x32E06B0;
const __int64 BodyTransformCallRva = 0x32EA29D;   // in 0x1432EA220

namespace
{
  struct PlacedBody { float Rows[16]; };
  std::mutex g_PlacedMutex;
  std::unordered_map<__int64, PlacedBody> g_PlacedBodies;   // under g_PlacedMutex
  std::atomic<int> g_PlacedCount{ 0 };
  // Bodies whose placement the replay reads as the game makes it (a drone's:
  // see util::hooks::ReadBodyPlace)
  struct WatchedBody { float Rows[16]; bool Seen; };
  const size_t MaxWatchedBodies = 256;
  std::unordered_map<__int64, WatchedBody> g_WatchedBodies; // under g_PlacedMutex
  std::atomic<int> g_WatchedCount{ 0 };
}

bool __fastcall hBodyTransform(__int64 body, float* out)
{
  bool ok = oBodyTransform(body, out);
  if (ok && g_WatchedCount.load(std::memory_order_acquire) > 0)
  {
    std::lock_guard<std::mutex> lock(g_PlacedMutex);
    auto it = g_WatchedBodies.find(body);
    if (it != g_WatchedBodies.end())
    {
      memcpy(it->second.Rows, out, sizeof(it->second.Rows));
      it->second.Seen = true;
    }
  }
  if (ok && g_PlacedCount.load(std::memory_order_acquire) > 0)
  {
    std::lock_guard<std::mutex> lock(g_PlacedMutex);
    auto it = g_PlacedBodies.find(body);
    if (it != g_PlacedBodies.end())
    {
      for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c)
          out[r * 4 + c] = it->second.Rows[r * 4 + c];
      for (int c = 0; c < 3; ++c)
        out[12 + c] = it->second.Rows[12 + c];
    }
  }
  return ok;
}

static bool IsBodyTransformCode(BYTE const* pBase)
{
  // sub rsp, 0x28; cmp byte [rcx + 0xB8], 0; mov r9, rdx; jne +8; xor al, al
  static const BYTE code[] = { 0x48, 0x83, 0xEC, 0x28, 0x80, 0xB9, 0xB8, 0x00, 0x00, 0x00, 0x00, 0x49, 0x89, 0xD1, 0x75, 0x08, 0x30,
    0xC0 };
  return !memcmp(pBase + BodyTransformRva, code, sizeof(code)) && pBase[BodyTransformCallRva] == 0xE8 &&
    BodyTransformCallRva + 5 + *(int const*)(pBase + BodyTransformCallRva + 1) == BodyTransformRva;
}

bool util::hooks::PlaceBody(__int64 body, float const* rows)
{
  if (!oBodyTransform)
    return false;
  std::lock_guard<std::mutex> lock(g_PlacedMutex);
  if (rows)
    memcpy(g_PlacedBodies[body].Rows, rows, sizeof(PlacedBody::Rows));
  else
    g_PlacedBodies.erase(body);
  g_PlacedCount.store((int)g_PlacedBodies.size(), std::memory_order_release);
  return true;
}

void util::hooks::ReleasePlacedBodies()
{
  std::lock_guard<std::mutex> lock(g_PlacedMutex);
  g_PlacedBodies.clear();
  g_PlacedCount.store(0, std::memory_order_release);
}

bool util::hooks::ReadBodyPlace(__int64 body, float* rows)
{
  if (!oBodyTransform)
    return false;
  std::lock_guard<std::mutex> lock(g_PlacedMutex);
  auto it = g_WatchedBodies.find(body);
  if (it == g_WatchedBodies.end())
  {
    if (g_WatchedBodies.size() < MaxWatchedBodies)
    {
      g_WatchedBodies[body] = WatchedBody{};
      g_WatchedCount.store((int)g_WatchedBodies.size(), std::memory_order_release);
    }
    return false;
  }
  if (!it->second.Seen)
    return false;
  memcpy(rows, it->second.Rows, sizeof(it->second.Rows));
  return true;
}

void util::hooks::ReleaseWatchedBodies()
{
  std::lock_guard<std::mutex> lock(g_PlacedMutex);
  g_WatchedBodies.clear();
  g_WatchedCount.store(0, std::memory_order_release);
}

// A character's culling box (2026-10-24, offline). PamClientCharacterEntity
// (vtable 0x141C3C760) gives it in slot 23 (0x14390FDE0): rows + 0x2D0, + 0x2E0,
// + 0x2F0 and centre + 0x300 (a transform), corners + 0x310 / + 0x320 around
// the centre, 2 or 4 m added on each side; with no box (+ 0x2F0 all 0) its
// physics' or a fixed one. Its update (slot 84, 0x143919040) makes the box in
// 0x14391C700 from its BoundingBoxBones (data + 0xE0), each where the
// PamMovementImpactManager ([0x14257C180], keyed by the pose [+ 0x340] +
// 0x158) last had it; only the game's own update of the body moves those, so
// a body the replay draws itself (the game doesn't update it) kept its box
// where it lay, and with the camera close to the replayed body that box was
// out of view and the body culled (2026-10-24: downed KrugerSec vanishing up
// close; Faith's body did the same until her box pose got the replay). The
// replay gives such a character a box around its drawn pose: written when it
// draws it and again right after the game makes its own (this hook); the
// game's box (as it was when the replay first wrote one) is put back when the
// replay lets go, and a character destroyed meanwhile is dropped (the client
// character's destructor hook).
typedef void(__fastcall* tCharacterBox)(__int64);
tCharacterBox oCharacterBox = nullptr;
const __int64 CharacterBoxRva = 0x391C700;
const __int64 CharacterBoxJumpRva = 0x3919055;   // its only caller: jmp from slot 84, 0x143919040
const __int64 CharacterBoxGetterRva = 0x390FDE0;
const int CharacterBoxGetterSlot = 23;
const int CharacterBoxUpdateSlot = 84;
const __int64 CharacterBoxUpdateRva = 0x3919040;
const int CharacterBoxOffset = 0x2D0;

namespace
{
  struct CullBox
  {
    float Box[util::hooks::CullBoxFloats];
    float Game[util::hooks::CullBoxFloats];
    bool HaveGame;
  };
  std::mutex g_CullBoxMutex;
  std::unordered_map<__int64, CullBox> g_CullBoxes;   // under g_CullBoxMutex
  std::atomic<int> g_CullBoxCount{ 0 };

  bool IsClientCharacter(__int64 entity)
  {
    __try
    {
      return entity >= 0x10000 && *(__int64 const*)entity == (__int64)g_gameHandle + ClientCharacterVTableRva;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      return false;
    }
  }

  bool ReadCharacterBox(__int64 character, float* box)
  {
    __try
    {
      memcpy(box, (void const*)(character + CharacterBoxOffset), util::hooks::CullBoxFloats * sizeof(float));
      return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      return false;
    }
  }

  // 16 bytes at a time, as the game writes it
  bool WriteCharacterBox(__int64 character, float const* box)
  {
    __try
    {
      __m128* to = (__m128*)(character + CharacterBoxOffset);
      for (int i = 0; i < util::hooks::CullBoxFloats / 4; ++i)
        _mm_store_ps((float*)(to + i), _mm_loadu_ps(box + i * 4));
      return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      return false;
    }
  }
}

void __fastcall hCharacterBox(__int64 character)
{
  oCharacterBox(character);
  if (g_CullBoxCount.load(std::memory_order_acquire) > 0)
  {
    std::lock_guard<std::mutex> lock(g_CullBoxMutex);
    auto it = g_CullBoxes.find(character);
    if (it != g_CullBoxes.end())
      WriteCharacterBox(character, it->second.Box);
  }
}

static bool IsCharacterBoxCode(BYTE const* pBase)
{
  // mov r11, rsp; push rbx; sub rsp, 0x110; cmp qword [rcx + 0x340], 0
  static const BYTE code[] = { 0x49, 0x89, 0xE3, 0x53, 0x48, 0x81, 0xEC, 0x10, 0x01, 0x00, 0x00, 0x48, 0x83, 0xB9, 0x40, 0x03, 0x00,
    0x00, 0x00 };
  // movaps [rbx + 0x320], xmm4; movaps [rbx + 0x310], xmm0 (the corners it makes)
  static const BYTE corners[] = { 0x0F, 0x29, 0xA3, 0x20, 0x03, 0x00, 0x00, 0x0F, 0x29, 0x83, 0x10, 0x03, 0x00, 0x00 };
  // the getter's first read: movaps xmm0, [rcx + 0x2F0]
  static const BYTE getter[] = { 0x0F, 0x28, 0x81, 0xF0, 0x02, 0x00, 0x00 };
  __int64 const* vtable = (__int64 const*)(pBase + ClientCharacterVTableRva);
  return !memcmp(pBase + CharacterBoxRva, code, sizeof(code)) && !memcmp(pBase + 0x391C838, corners, sizeof(corners)) &&
    !memcmp(pBase + CharacterBoxGetterRva + 6, getter, sizeof(getter)) && pBase[CharacterBoxJumpRva] == 0xE9 &&
    CharacterBoxJumpRva + 5 + *(int const*)(pBase + CharacterBoxJumpRva + 1) == CharacterBoxRva &&
    vtable[CharacterBoxGetterSlot] == (__int64)pBase + CharacterBoxGetterRva &&
    vtable[CharacterBoxUpdateSlot] == (__int64)pBase + CharacterBoxUpdateRva;
}

bool util::hooks::SetCullBox(__int64 character, float const* box, float* gameCentre)
{
  if (!oCharacterBox || !oClientCharacterDtor)
    return false;
  std::lock_guard<std::mutex> lock(g_CullBoxMutex);
  auto it = g_CullBoxes.find(character);
  if (!box)
  {
    // The game's back (it makes its own again if it updates the character)
    if (it != g_CullBoxes.end())
    {
      if (it->second.HaveGame && IsClientCharacter(character))
        WriteCharacterBox(character, it->second.Game);
      g_CullBoxes.erase(it);
    }
    g_CullBoxCount.store((int)g_CullBoxes.size(), std::memory_order_release);
    return true;
  }
  if (!IsClientCharacter(character))
    return false;
  if (it == g_CullBoxes.end())
  {
    CullBox entry{};
    entry.HaveGame = ReadCharacterBox(character, entry.Game);
    it = g_CullBoxes.emplace(character, entry).first;
  }
  memcpy(it->second.Box, box, sizeof(it->second.Box));
  if (gameCentre)
  {
    for (int i = 0; i < 3; ++i)
      gameCentre[i] = it->second.HaveGame ? it->second.Game[12 + i] : 0.f;
  }
  g_CullBoxCount.store((int)g_CullBoxes.size(), std::memory_order_release);
  return WriteCharacterBox(character, box);
}

void util::hooks::ReleaseCullBoxes()
{
  std::lock_guard<std::mutex> lock(g_CullBoxMutex);
  for (auto& entry : g_CullBoxes)
    if (entry.second.HaveGame && IsClientCharacter(entry.first))
      WriteCharacterBox(entry.first, entry.second.Game);
  g_CullBoxes.clear();
  g_CullBoxCount.store(0, std::memory_order_release);
}

// A character destroyed: its box is never written again
static void DropCullBox(__int64 character)
{
  if (g_CullBoxCount.load(std::memory_order_acquire) == 0)
    return;
  std::lock_guard<std::mutex> lock(g_CullBoxMutex);
  g_CullBoxes.erase(character);
  g_CullBoxCount.store((int)g_CullBoxes.size(), std::memory_order_release);
}

// The loading screen: PamClientUILoadScreenControlEntity (vtable 0x141CBB228)
// takes its events in slot 4, 0x143B2E130 (entity, event: + 8 the id). Event
// 0x62B097AE sets the loading screen's flag ([UI + 0x928] + 0x60) to 1 (shown),
// 0xF4FD48C7 sets it to 0 (hidden) (0x143B11A30 / 0x143B11A60, 2026-10-24).
// With the loading screen's widget there ([UI + 0x928] + 0x40) the show only
// goes to the widget (0x143AEC540) and the flag stays 0 (a mission restart's,
// 2026-10-26 20:50); nothing else calls either, so these events are the one
// sure way to tell (see util::hooks::LoadScreenShown).
// A fast travel waits behind its loading screen until the sub-levels it unloads
// are gone (2026-10-24 19:07 - 19:10: stuck until the replay let go of the five
// it kept, then done within a minute), while a respawn doesn't (8.5 s with them
// kept). So once a loading screen has been up LoadScreenWaitMs, the replay lets
// go of its people (see util::hooks::LoadScreenWaiting).
typedef void(__fastcall* tLoadScreenEvent)(__int64, __int64);
tLoadScreenEvent oLoadScreenEvent = nullptr;
const __int64 LoadScreenEventRva = 0x3B2E130;
const __int64 LoadScreenVTableRva = 0x1CBB228;
const int LoadScreenEventSlot = 4;
const unsigned int LoadScreenShowId = 0x62B097AE;
const unsigned int LoadScreenHideId = 0xF4FD48C7;
const ULONGLONG LoadScreenWaitMs = 15000;
const ULONGLONG LoadScreenForgetMs = 300000;   // a show with no hide after it isn't held on to longer

namespace
{
  std::atomic<ULONGLONG> g_LoadScreenSince{ 0 };   // when it was shown, 0: not shown

  bool ReadEventId(__int64 event, unsigned int& id)
  {
    __try
    {
      id = *(unsigned int const*)(event + 8);
      return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      return false;
    }
  }
}

void __fastcall hLoadScreenEvent(__int64 entity, __int64 event)
{
  unsigned int id = 0;
  if (ReadEventId(event, id))
  {
    if (id == LoadScreenShowId)
    {
      ULONGLONG expected = 0;
      if (g_LoadScreenSince.compare_exchange_strong(expected, GetTickCount64()))
      {
        util::log::Write("Loading screen: shown");
        SoundLevelChanged("a loading screen");
      }
    }
    else if (id == LoadScreenHideId)
    {
      ULONGLONG since = g_LoadScreenSince.exchange(0);
      if (since)
        util::log::Write("Loading screen: gone after %.1f s", (GetTickCount64() - since) / 1000.0);
    }
  }
  oLoadScreenEvent(entity, event);
}

static bool IsLoadScreenEventCode(BYTE const* pBase)
{
  // mov [rsp + 8], rbx; mov [rsp + 0x10], rsi; push rdi; sub rsp, 0x50; mov rdi, rdx; mov rsi, rcx
  static const BYTE code[] = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x50, 0x48, 0x89,
    0xD7, 0x48, 0x89, 0xCE };
  return !memcmp(pBase + LoadScreenEventRva, code, sizeof(code)) &&
    *(__int64 const*)(pBase + LoadScreenVTableRva + LoadScreenEventSlot * 8) == (__int64)pBase + LoadScreenEventRva;
}

bool util::hooks::LoadScreenWaiting()
{
  ULONGLONG since = g_LoadScreenSince.load();
  if (!since)
    return false;
  ULONGLONG up = GetTickCount64() - since;
  if (up >= LoadScreenForgetMs)
  {
    g_LoadScreenSince.store(0);
    util::log::Write("Loading screen: shown %.0f s ago and no hide seen since: taken as gone", up / 1000.0);
    return false;
  }
  return up >= LoadScreenWaitMs;
}

bool util::hooks::LoadScreenShown()
{
  ULONGLONG since = g_LoadScreenSince.load();
  return since && GetTickCount64() - since < LoadScreenForgetMs;
}

// A loading screen up a while: what the game's level streaming still waits
// on, in the log, StuckLoadFirstMs after the show and every StuckLoadEveryMs
// after that until it goes (or is forgotten, LoadScreenForgetMs). 2026-10-26
// 20:50: a mission restart stayed behind its loading screen. Read live then,
// the server's and the client's level streaming each had three Prison Yard
// zone sub-levels waiting for their bundles (state 1) and eleven more not
// started behind them, and the game read nothing from disk: it never started
// loading those three bundles (why isn't known). Offline from the game's code,
// checked on that live read:
// - The sub-level streaming managers are statics: the server's 0x14253D680
//   (its update 0x1432F6390 is given it at 0x143344633) and the client's
//   0x142401CF0 (at 0x1431A126B), vtables 0x141B42E40 / 0x141AFE908.
// - Each keeps its sub-levels by state in red-black trees: state s's header at
//   + 0x18 + s * 0x38, its leftmost node at header + 8, their count at header
//   + 0x20; a node's + 0x28 is the sub-level, its + 0 / + 8 / + 0x10 the right
//   and left child and the parent (the successor 0x142A34820).
// - A sub-level: + 8 its state, + 0xC its id, + 0x30 the sub-level it's in,
//   + 0x90 its container once made (its name at [+ 0x10]). 0x1432F0EC0 moves
//   sub-levels to state 1; in state 1 each update asks the bundle loader
//   whether its bundle is in (0x1432F1980 -> 0x14314B8F0, which writes the
//   bundle's index to + 0x84, 0xBBD when it doesn't know the bundle); the ones
//   in (named, with a container) were all in state 3.
// - Bundles by index: [0x14237DEB8 + index * 8] (0x142ABAB50), + 0x70 the
//   name. + 0x24 was 1 for every zone bundle in and 0 for the three that never
//   loaded (not known for other kinds of bundle).
// Read-only, inside __try; nothing of the game's is called.
namespace
{
  const __int64 ServerStreamingRva = 0x253D680;
  const __int64 ServerStreamingLeaRva = 0x3344633;   // lea rcx, [rip + x] before the update's call
  const __int64 ClientStreamingRva = 0x2401CF0;
  const __int64 ClientStreamingLeaRva = 0x31A126B;
  const __int64 ClientStreamingVTableRva = 0x1AFE908;
  const __int64 BundleByIndexRva = 0x2ABAB50;
  const __int64 BundleTableRva = 0x237DEB8;
  const int StreamingStates = 5;
  const int StreamingStateStride = 0x38;
  const int StreamingListedMax = 16;
  const unsigned short BundleUnknown = 0xBBD;
  const unsigned short BundlesMax = 0x1000;          // an index past this isn't one
  const ULONGLONG StuckLoadFirstMs = 30000;
  const ULONGLONG StuckLoadEveryMs = 60000;

  struct SubLevelSeen
  {
    int State;
    int Id;
    int In;              // the id of the sub-level it's in, -1 none
    int Bundle;          // state 1: the bundle it waits for, -1 not known
    int BundleIn;        // that bundle's + 0x24, -1 not read
    char Name[112];      // its bundle's (state 1), else the name of the sub-level it's in
  };

  bool LeaTargets(BYTE const* pBase, __int64 leaRva, __int64 targetRva)
  {
    BYTE const* p = pBase + leaRva;
    return p[0] == 0x48 && p[1] == 0x8D && p[2] == 0x0D && leaRva + 7 + *(int const*)(p + 3) == targetRva;
  }

  bool IsStreamingWatchCode(BYTE const* pBase)
  {
    // movzx eax, cx; lea rcx, [rip + table]; mov rax, [rcx + rax * 8]
    BYTE const* p = pBase + BundleByIndexRva;
    return LeaTargets(pBase, ServerStreamingLeaRva, ServerStreamingRva) &&
      LeaTargets(pBase, ClientStreamingLeaRva, ClientStreamingRva) && p[0] == 0x0F && p[1] == 0xB7 && p[2] == 0xC1 &&
      LeaTargets(pBase, BundleByIndexRva + 3, BundleTableRva) && p[10] == 0x48 && p[11] == 0x8B && p[12] == 0x04 &&
      p[13] == 0xC1;
  }

  // A name the game keeps as text, if it's one
  void ReadName(__int64 text, char* out, size_t size)
  {
    out[0] = 0;
    if (text < 0x10000)
      return;
    __try
    {
      size_t i = 0;
      for (; i + 1 < size; ++i)
      {
        char c = ((char const*)text)[i];
        if (!c)
          break;
        if (c < 0x20 || c > 0x7E)
        {
          i = 0;
          break;
        }
        out[i] = c;
      }
      out[i] = 0;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      out[0] = 0;
    }
  }

  // The tree's next node, as the game's 0x142A34820 (a bad pointer throws to
  // the caller's __try)
  __int64 TreeNext(__int64 node)
  {
    __int64 right = *(__int64 const*)node;
    if (right)
    {
      for (int i = 0; i < 64 && *(__int64 const*)(right + 8); ++i)
        right = *(__int64 const*)(right + 8);
      return right;
    }
    __int64 parent = *(__int64 const*)(node + 0x10);
    for (int i = 0; i < 64 && node == *(__int64 const*)parent; ++i)
    {
      node = parent;
      parent = *(__int64 const*)(parent + 0x10);
    }
    return *(__int64 const*)node != parent ? parent : node;
  }

  // A manager's count of sub-levels in each state, and the ones not in (up to
  // max); false if it isn't the manager known
  bool ReadStreaming(__int64 manager, __int64 vtable, int* counts, SubLevelSeen* seen, int max, int& listed)
  {
    listed = 0;
    __int64 bundles = (__int64)g_gameHandle + BundleTableRva;
    __try
    {
      if (*(__int64 const*)manager != vtable)
        return false;
      for (int s = 0; s < StreamingStates; ++s)
      {
        __int64 header = manager + 0x18 + s * StreamingStateStride;
        counts[s] = *(int const*)(header + 0x20);
        if (s == 3)
          continue;
        __int64 node = *(__int64 const*)(header + 8);
        for (int steps = 0; node && node != header && steps < 64 && listed < max; ++steps)
        {
          __int64 sub = *(__int64 const*)(node + 0x28);
          SubLevelSeen& one = seen[listed++];
          one.State = *(int const*)(sub + 8);
          one.Id = *(unsigned short const*)(sub + 0xC);
          one.In = -1;
          one.Bundle = -1;
          one.BundleIn = -1;
          one.Name[0] = 0;
          __int64 parent = *(__int64 const*)(sub + 0x30);
          if (parent >= 0x10000)
            one.In = *(unsigned short const*)(parent + 0xC);
          unsigned short index = *(unsigned short const*)(sub + 0x84);
          if (one.State == 1 && index != BundleUnknown && index < BundlesMax)
          {
            one.Bundle = index;
            __int64 bundle = *(__int64 const*)(bundles + index * 8);
            if (bundle >= 0x10000)
            {
              one.BundleIn = *(int const*)(bundle + 0x24);
              ReadName(*(__int64 const*)(bundle + 0x70), one.Name, sizeof(one.Name));
            }
          }
          else if (parent >= 0x10000)
          {
            __int64 container = *(__int64 const*)(parent + 0x90);
            if (container >= 0x10000)
              ReadName(*(__int64 const*)(container + 0x10), one.Name, sizeof(one.Name));
          }
          node = TreeNext(node);
        }
      }
      return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      return false;
    }
  }

  void LogStreaming(char const* whose, __int64 manager, __int64 vtable,
    char const* prefix = "Loading screen (diagnostic)")
  {
    int counts[StreamingStates] = {};
    SubLevelSeen seen[StreamingListedMax];
    int listed = 0;
    if (!ReadStreaming(manager, vtable, counts, seen, StreamingListedMax, listed))
    {
      util::log::Write("%s: the %s level streaming couldn't be read", prefix, whose);
      return;
    }
    util::log::Write("%s: the %s level streaming: %d sub-levels not started (state 0), %d waiting "
      "for their bundle (state 1), %d in (state 3), %d in state 2 and %d in state 4", prefix, whose, counts[0], counts[1], counts[3],
      counts[2], counts[4]);
    for (int i = 0; i < listed; ++i)
    {
      SubLevelSeen const& one = seen[i];
      if (one.State == 1 && one.Bundle >= 0)
        util::log::Write("%s:   sub-level 0x%X (in 0x%X) waits for its bundle 0x%X %s (its + 0x24: %d)",
          prefix, one.Id, one.In, one.Bundle, one.Name[0] ? one.Name : "(no name read)", one.BundleIn);
      else
        util::log::Write("%s:   sub-level 0x%X in state %d, in sub-level 0x%X %s", prefix, one.Id, one.State,
          one.In, one.Name);
    }
    int total = counts[0] + counts[1] + counts[2] + counts[4];
    if (total > listed)
      util::log::Write("%s:   (%d more not listed)", prefix, total - listed);
  }

  ULONGLONG ReadBytesSoFar()
  {
    IO_COUNTERS io{};
    return GetProcessIoCounters(GetCurrentProcess(), &io) ? io.ReadTransferCount : 0;
  }
}

void util::hooks::WatchLoadScreen()
{
  if (!util::DiagnosticsOn())
    return;
  // Camera update thread only
  static ULONGLONG s_shownAt = 0, s_nextAt = 0, s_loggedAt = 0, s_read = 0;
  static int s_known = 0;
  ULONGLONG since = g_LoadScreenSince.load();
  if (!since || g_shutdown || !g_gameHandle)
    return;
  ULONGLONG now = GetTickCount64();
  if (since != s_shownAt)
  {
    s_shownAt = since;
    s_nextAt = since + StuckLoadFirstMs;
    s_loggedAt = now;
    s_read = ReadBytesSoFar();
  }
  if (now < s_nextAt || now - since >= LoadScreenForgetMs)
    return;
  s_nextAt = now + StuckLoadEveryMs;
  if (!s_known)
  {
    s_known = IsStreamingWatchCode((BYTE const*)g_gameHandle) ? 1 : -1;
    if (s_known < 0)
      util::log::Warning("Loading screen: the game's level streaming isn't where the tools know it: a loading screen up a "
        "long time can't be explained in the log");
  }
  if (s_known < 0)
    return;
  ULONGLONG read = ReadBytesSoFar();
  util::log::Write("Loading screen (diagnostic): up %.0f s; the game read %.1f MB (all its reads) in the last %.0f s", (now - since)
    / 1000.0, (read - s_read) / (1024.0 * 1024.0), (now - s_loggedAt) / 1000.0);
  s_read = read;
  s_loggedAt = now;
  __int64 base = (__int64)g_gameHandle;
  LogStreaming("server's", base + ServerStreamingRva, base + StreamingManagerVTableRva);
  LogStreaming("client's", base + ClientStreamingRva, base + ClientStreamingVTableRva);
}

void util::hooks::LogLevelStreaming(char const* prefix)
{
  // Any thread: it only reads (inside __try). For the hang watch (Main.cpp):
  // a quit that never ends the game (2026-10-27) may wait on a sub-level
  if (!util::DiagnosticsOn())
    return;
  if (!g_gameHandle || !IsStreamingWatchCode((BYTE const*)g_gameHandle))
  {
    util::log::Write("%s: the game's level streaming isn't where the tools know it", prefix);
    return;
  }
  __int64 base = (__int64)g_gameHandle;
  LogStreaming("server's", base + ServerStreamingRva, base + StreamingManagerVTableRva, prefix);
  LogStreaming("client's", base + ClientStreamingRva, base + ClientStreamingVTableRva, prefix);
}

bool __fastcall hSubLevelUnload(__int64 manager, __int64 request)
{
  unsigned int id = 0;
  __int64 container = 0;
  if (!g_HoldAIDespawn.load(std::memory_order_relaxed) ||
      (__int64)_ReturnAddress() != (__int64)g_gameHandle + SubLevelUnloadReturnRva || !ReadUnloadRequest(request, id, container))
    return oSubLevelUnload(manager, request);
  ULONGLONG now = GetTickCount64();
  bool keep = true;
  {
    std::lock_guard<std::mutex> lock(g_SubLevelMutex);
    KeptSubLevel* kept = nullptr;
    for (auto& entry : g_SubLevels)
      if (entry.Container == container && entry.Id == id)
        kept = &entry;
    // Decided once: kept until the replay lets go of its people (2026-10-24:
    // looked at again every 0.5 s, one was found with none of them near any
    // more as a playback started, and unloaded with three recorded
    // KrugerSec in it)
    if (!kept)
    {
      static PeopleIn s_people;   // under g_SubLevelMutex
      int nearCount = 0;
      int first = FindRecordedIn(container, s_people, nearCount);
      if (first < 0)
        keep = false;
      else
      {
        g_SubLevels.push_back({ container, id, now, now, 0 });
        kept = &g_SubLevels.back();
        util::log::Write("Replay: the game wants to unload a sub-level (id 0x%X, container 0x%I64X) that holds %d of the people near "
          "the recorded ones (the %s 0x%I64X at %.1f %.1f %.1f): kept loaded while the replay holds people", id, container, nearCount,
          KeptPeopleTypes[s_people.Type[first]].Name, s_people.Entity[first], s_people.Place[first][0], s_people.Place[first][1],
          s_people.Place[first][2]);
      }
    }
    if (kept)
    {
      ++kept->Asks;
      if (now - kept->LoggedAt >= SubLevelLogEveryMs)
      {
        kept->LoggedAt = now;
        util::log::Write("Replay: the sub-level 0x%X is still kept loaded (%.0f s now, the game asked %d times)", id,
          (now - kept->Since) / 1000.0, kept->Asks);
      }
    }
  }
  return keep ? false : oSubLevelUnload(manager, request);
}

// The replay let go of its people: the sub-levels kept loaded unload (each the
// next time the game asks, unless it's decided to keep it again)
void util::hooks::ReleaseKeptSubLevels()
{
  LetSubLevelsGo();
}

void LetSubLevelsGo()
{
  std::lock_guard<std::mutex> lock(g_SubLevelMutex);
  if (g_SubLevels.empty())
    return;
  ULONGLONG now = GetTickCount64();
  for (auto const& kept : g_SubLevels)
    util::log::Write("Replay: the sub-level 0x%X kept loaded for %.1f s (the game asked %d times) unloads now", kept.Id,
      (now - kept.Since) / 1000.0, kept.Asks);
  g_SubLevels.clear();
}

static bool IsSubLevelUnloadCode(BYTE const* pBase)
{
  // push rdi; sub rsp, 0x60; mov qword [rsp + 0x20], -2; mov [rsp + 0x70], rbx
  static const BYTE code[] = { 0x57, 0x48, 0x83, 0xEC, 0x60, 0x48, 0xC7, 0x44, 0x24, 0x20, 0xFE, 0xFF, 0xFF, 0xFF, 0x48, 0x89,
    0x5C, 0x24, 0x70 };
  // the streaming update: mov rcx, r15; call [rax + 0x18]; test al, al; jne
  static const BYTE caller[] = { 0x4C, 0x89, 0xF9, 0xFF, 0x50, 0x18, 0x84, 0xC0, 0x75 };
  return !memcmp(pBase + SubLevelUnloadRva, code, sizeof(code)) &&
    !memcmp(pBase + SubLevelUnloadReturnRva - 6, caller, sizeof(caller)) &&
    *(__int64 const*)(pBase + StreamingManagerVTableRva + 3 * 8) == (__int64)pBase + SubLevelUnloadRva;
}

// Replay people: a body's "always update" switch, 0x1432E7EA0 (body =
// animatable + 0x70, on): + 0x5C3, the body put on or taken off the list of
// bodies its manager updates (0x142FD21C0 / 0x142FD1B50). Its only callers
// (offline, 2026-10-24): 0x143936820 (on), 0x143934B10 (off with its argument
// set, on without; both take the body from [component + 0x80] + 0x70), and
// 0x143981A80 (off, once: its flag + 0x128), which only
// PamClientAIBrainComponent slot 65 (0x1439336D0) calls, when [[brain + 0xE8]
// + 0x550] < 0: the character died or was taken down. So a body the game
// switched off from there is dead; one switched off any other way is alive,
// only not animated (live, 2026-10-24: 24 of 27 recorded KrugerSec 560 - 740
// m from the real Faith had it off), and the replay can switch it on for a
// playback: the game then animates it, and the replay's override poses it (a
// body the replay draws itself isn't seen). A dead body is never switched on
// (2026-10-23 18:28: the game crashed in an animation job as six dead ones
// were). The hook, on from the start, notes the dead ones, and which bodies
// the replay switched on the game switched itself since.
typedef __int64(__fastcall* tAlwaysUpdate)(__int64, unsigned char);
tAlwaysUpdate oAlwaysUpdate = nullptr;
const __int64 AlwaysUpdateRva = 0x32E7EA0;
const __int64 DeathSwitchCallRva = 0x3981AA5;   // its call in 0x143981A80
// The distance switch (0x143934B10) turns it off with its call at 0x143934B43.
// Only a body that switch turned off is switched on for a playback: one held
// through Faith's death came back from the respawn not animated (switched off
// another way), was switched on, and the game crashed in an animation job
// (2026-10-24 18:43), as with the dead ones before.
const __int64 DistanceSwitchOffCallRva = 0x3934B43;

namespace
{
  std::mutex g_BodySwitchMutex;
  std::unordered_set<__int64> g_DeadBodies;          // under g_BodySwitchMutex
  std::unordered_set<__int64> g_DistanceOffBodies;   // switched off last by the distance switch; under it too
  std::unordered_map<__int64, bool> g_ForcedBodies;  // switched on by the replay -> the game switched it since; under it too
  thread_local bool t_SwitchingBody = false;
}

__int64 __fastcall hAlwaysUpdate(__int64 body, unsigned char on)
{
  if (!t_SwitchingBody)
  {
    __int64 from = (__int64)_ReturnAddress() - (__int64)g_gameHandle;
    bool death = !on && from == DeathSwitchCallRva + 5;
    bool distance = !on && from == DistanceSwitchOffCallRva + 5;
    std::lock_guard<std::mutex> lock(g_BodySwitchMutex);
    if (death)
      g_DeadBodies.insert(body);
    else if (on)
      g_DeadBodies.erase(body);
    if (distance)
      g_DistanceOffBodies.insert(body);
    else
      g_DistanceOffBodies.erase(body);
    auto it = g_ForcedBodies.find(body);
    if (it != g_ForcedBodies.end())
      it->second = true;
  }
  return oAlwaysUpdate(body, on);
}

static bool IsAlwaysUpdateCode(BYTE const* pBase)
{
  // mov [rsp + 8], rbx; push rdi; sub rsp, 0x20; movzx edi, dl; mov rbx, rcx; cmp dl, [rcx + 0x5C3]
  static const BYTE code[] = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x0F, 0xB6, 0xFA, 0x48, 0x89, 0xCB, 0x3A,
    0x91, 0xC3, 0x05, 0x00, 0x00 };
  return !memcmp(pBase + AlwaysUpdateRva, code, sizeof(code)) && pBase[DeathSwitchCallRva] == 0xE8 &&
    DeathSwitchCallRva + 5 + *(int const*)(pBase + DeathSwitchCallRva + 1) == AlwaysUpdateRva &&
    pBase[DistanceSwitchOffCallRva] == 0xE8 &&
    DistanceSwitchOffCallRva + 5 + *(int const*)(pBase + DistanceSwitchOffCallRva + 1) == AlwaysUpdateRva;
}

bool util::hooks::IsBodyDead(__int64 body)
{
  std::lock_guard<std::mutex> lock(g_BodySwitchMutex);
  return g_DeadBodies.count(body) != 0;
}

bool util::hooks::ForceBodyUpdate(__int64 body)
{
  if (!oAlwaysUpdate)
    return false;
  {
    std::lock_guard<std::mutex> lock(g_BodySwitchMutex);
    if (g_DeadBodies.count(body) || !g_DistanceOffBodies.count(body))
      return false;
    g_ForcedBodies[body] = false;
  }
  t_SwitchingBody = true;
  oAlwaysUpdate(body, 1);
  t_SwitchingBody = false;
  return true;
}

bool util::hooks::GameSwitchedBody(__int64 body)
{
  std::lock_guard<std::mutex> lock(g_BodySwitchMutex);
  auto it = g_ForcedBodies.find(body);
  return it != g_ForcedBodies.end() && it->second;
}

void util::hooks::ReleaseBodyUpdate(__int64 body, bool switchOff)
{
  bool switched = true;
  {
    std::lock_guard<std::mutex> lock(g_BodySwitchMutex);
    auto it = g_ForcedBodies.find(body);
    if (it == g_ForcedBodies.end())
      return;
    switched = it->second || g_DeadBodies.count(body) != 0;
    g_ForcedBodies.erase(it);
  }
  // Back off only if the game left it as the replay had it
  if (switchOff && !switched && oAlwaysUpdate)
  {
    t_SwitchingBody = true;
    oAlwaysUpdate(body, 0);
    t_SwitchingBody = false;
  }
}

// Replay people the replay draws itself (their pose buffers written each
// frame, FaithReplay's DrawBody / DrawParked): the game's own update of their
// body is held off. The body update gate 0x1432E86D0 (body = animatable +
// 0x70; true: leave it as it is), asked by the body's vtable (0x141B34500)
// slots 1 (0x1432DDF90) and 3 (0x1432DE310, its update). A KrugerSec taken
// down has "always update" off, but + 0xFC is on for part of every frame (read
// live 2026-10-24: on and off about every 5 ms on every body, live ones too)
// and lets an update through then: their ragdoll's pose went into the same
// buffers once a frame, a few ms after the replay's (of each frame about 6 ms
// the replayed pose, 4 ms the lying one), so up close they flickered; from
// far away the game left them alone. While held, the gate says "leave it",
// the way it does for the bodies it doesn't animate (they keep their pose
// buffers, and their pose slot).
typedef bool(__fastcall* tBodyUpdateGate)(__int64);
tBodyUpdateGate oBodyUpdateGate = nullptr;
const __int64 BodyUpdateGateRva = 0x32E86D0;
const __int64 BodyUpdateGateCallRvas[] = { 0x32DE346, 0x32DDFB1 };

namespace
{
  std::mutex g_HeldBodiesMutex;
  std::vector<__int64> g_HeldBodies;        // under g_HeldBodiesMutex
  std::atomic<int> g_HeldBodyCount{ 0 };
  std::atomic<int> g_BodyUpdatesHeld{ 0 };
}

bool __fastcall hBodyUpdateGate(__int64 body)
{
  if (g_HeldBodyCount.load(std::memory_order_acquire) > 0)
  {
    std::lock_guard<std::mutex> lock(g_HeldBodiesMutex);
    if (std::find(g_HeldBodies.begin(), g_HeldBodies.end(), body) != g_HeldBodies.end())
    {
      g_BodyUpdatesHeld.fetch_add(1, std::memory_order_relaxed);
      return true;
    }
  }
  return oBodyUpdateGate(body);
}

bool util::hooks::HoldBodyUpdate(__int64 body, bool hold)
{
  if (!oBodyUpdateGate)
    return false;
  std::lock_guard<std::mutex> lock(g_HeldBodiesMutex);
  auto it = std::find(g_HeldBodies.begin(), g_HeldBodies.end(), body);
  if (hold && it == g_HeldBodies.end())
    g_HeldBodies.push_back(body);
  else if (!hold && it != g_HeldBodies.end())
    g_HeldBodies.erase(it);
  g_HeldBodyCount.store((int)g_HeldBodies.size(), std::memory_order_release);
  return true;
}

int util::hooks::ReleaseBodyUpdates()
{
  {
    std::lock_guard<std::mutex> lock(g_HeldBodiesMutex);
    g_HeldBodies.clear();
    g_HeldBodyCount.store(0, std::memory_order_release);
  }
  return g_BodyUpdatesHeld.exchange(0);
}

static bool IsBodyUpdateGateCode(BYTE const* pBase)
{
  // mov [rsp + 0x10], rbx; mov [rsp + 0x18], rdi; push rbp; lea rbp, [rsp - 0xF0]; sub rsp, 0x1F0
  static const BYTE code[] = { 0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x7C, 0x24, 0x18, 0x55, 0x48, 0x8D, 0xAC, 0x24, 0x10,
    0xFF, 0xFF, 0xFF, 0x48, 0x81, 0xEC, 0xF0, 0x01, 0x00, 0x00 };
  if (memcmp(pBase + BodyUpdateGateRva, code, sizeof(code)))
    return false;
  for (__int64 call : BodyUpdateGateCallRvas)
    if (pBase[call] != 0xE8 || call + 5 + *(int const*)(pBase + call + 1) != BodyUpdateGateRva)
      return false;
  return true;
}

static bool IsContainerDestroyCode(BYTE const* pBase)
{
  // push rbx; push rbp; push rsi; sub rsp, 0x20; movsxd rsi, edx; mov rbp, rcx; call 0x14316C0B0
  static const BYTE all[] = { 0x53, 0x55, 0x56, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x63, 0xF2, 0x48, 0x89, 0xCD, 0xE8 };
  // mov [rsp + 0x18], rbx; push rdi; sub rsp, 0x20; mov rbx, rdx; mov rdi, rcx; cmp rdx, [rcx + 0x50]
  static const BYTE one[] = { 0x48, 0x89, 0x5C, 0x24, 0x18, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x89, 0xD3, 0x48, 0x89, 0xCF,
    0x48, 0x3B, 0x51, 0x50 };
  return !memcmp(pBase + ContainerDestroyAllRva, all, sizeof(all)) &&
    ContainerDestroyAllRva + 0xD + 5 + *(int const*)(pBase + ContainerDestroyAllRva + 0xE) == 0x316C0B0 &&
    !memcmp(pBase + ContainerDestroyOneRva, one, sizeof(one)) &&
    *(__int64 const*)(pBase + ContainerVTableRva + 2 * 8) == (__int64)pBase + ContainerDestroyOneRva &&
    pBase[FlushSlot6ReturnRva - 3] == 0xFF && pBase[FlushSlot6ReturnRva - 2] == 0x50 && pBase[FlushSlot6ReturnRva - 1] == 0x30;
}

// Replay (moving lights): the level moves a light by changing its Transform
// property, or, for a light in a blueprint that moves (an elevator lamp's),
// its BlueprintTransform. The PBR lights' property handler (vtable slot 2,
// 0x14376A8D0, in all five classes; entity, change; the change's first u32
// the property's hash, read with the entity by 0x14376A310 too) takes both,
// Transform 0x87525469 and BlueprintTransform 0xE790F446, the same way
// (0x14376ABCC): it works the world transform out (0x143165880) and has
// 0x14376BF80 write it at entity + 0x40 .. + 0x7F if it differs and call
// UpdateLight (slot 23, 0x14376A030). The light vtables are 25 - 27 slots
// each, back to back. While the replay records, the lights whose transform
// changes are noted here once each, with their transform from before (+ 0x40
// is only written after).
typedef __int64(__fastcall* tLightPropertyChanged)(__int64, unsigned int const*);
tLightPropertyChanged oLightPropertyChanged = nullptr;
const __int64 LightPropertyChangedRva = 0x376A8D0;
const __int64 UpdateLightRva = 0x376A030;
const __int64 LightVTableRvas[] = { 0x1BFF0A0, 0x1BFF178, 0x1BFF240, 0x1BFF308, 0x1BFF3D0 }; // Analytic, Sphere, Spot, Tube, Rectangular
const int LightTransformOffset = 0x40;

namespace
{
  SRWLOCK g_LightNoteLock = SRWLOCK_INIT;
  std::atomic<bool> g_LightNoteOn{ false };
  struct NotedLight
  {
    __int64 Entity;
    float Before[16];
  };
  std::vector<NotedLight> g_NotedLights;          // not taken yet
  std::unordered_map<__int64, bool> g_LightsNoted; // this recording

  bool IsLightVTable(__int64 vtable)
  {
    for (__int64 rva : LightVTableRvas)
      if (vtable == (__int64)g_gameHandle + rva)
        return true;
    return false;
  }

  bool ReadLightTransform(__int64 pEntity, float* out)
  {
    __try
    {
      if (!IsLightVTable(*(__int64 const*)pEntity))
        return false;
      memcpy(out, (void const*)(pEntity + LightTransformOffset), sizeof(float) * 16);
      return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      return false;
    }
  }

  unsigned int ReadChangeHash(unsigned int const* pChange)
  {
    __try { return pChange ? *pChange : 0; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
  }
}

__int64 __fastcall hLightPropertyChanged(__int64 pEntity, unsigned int const* pChange)
{
  if (g_LightNoteOn.load(std::memory_order_relaxed))
  {
    unsigned int hash = ReadChangeHash(pChange);
    NotedLight noted;
    if ((hash == 0x87525469 || hash == 0xE790F446) && ReadLightTransform(pEntity, noted.Before))
    {
      noted.Entity = pEntity;
      AcquireSRWLockExclusive(&g_LightNoteLock);
      if (g_LightsNoted.size() < 4096 && g_LightsNoted.emplace(pEntity, true).second)
        g_NotedLights.push_back(noted);
      ReleaseSRWLockExclusive(&g_LightNoteLock);
    }
  }
  return oLightPropertyChanged(pEntity, pChange);
}

// The handler and UpdateLight as they were read, slots 2 and 23 of the five
// light vtables
static bool IsLightPropertyCode(BYTE const* pBase)
{
  static const BYTE handler[] = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x70, 0x48, 0x89, 0xD7, 0x48, 0x89, 0xCB,
    0xE8, 0x2B, 0xFA, 0xFF, 0xFF, 0x8B, 0x07, 0x3D, 0xEA, 0xBC, 0xAA, 0xE4 };
  static const BYTE update[] = { 0x48, 0x83, 0xEC, 0x48, 0x80, 0xB9, 0xA2, 0x00, 0x00, 0x00, 0x00, 0x75, 0x44, 0xC6, 0x81, 0xA2, 0x00,
    0x00, 0x00, 0x01 };
  if (memcmp(pBase + LightPropertyChangedRva, handler, sizeof(handler)) || memcmp(pBase + UpdateLightRva, update, sizeof(update)))
    return false;
  for (__int64 rva : LightVTableRvas)
    if (*(__int64 const*)(pBase + rva + 23 * 8) != (__int64)pBase + UpdateLightRva ||
        *(__int64 const*)(pBase + rva + 2 * 8) != (__int64)pBase + LightPropertyChangedRva)
      return false;
  return true;
}

void util::hooks::NoteMovingLights(bool on)
{
  AcquireSRWLockExclusive(&g_LightNoteLock);
  g_NotedLights.clear();
  g_LightsNoted.clear();
  ReleaseSRWLockExclusive(&g_LightNoteLock);
  g_LightNoteOn = on && oLightPropertyChanged;
}

int util::hooks::TakeMovingLights(__int64* entities, float* before, int max)
{
  AcquireSRWLockExclusive(&g_LightNoteLock);
  int count = (int)g_NotedLights.size() < max ? (int)g_NotedLights.size() : max;
  for (int i = 0; i < count; ++i)
  {
    entities[i] = g_NotedLights[i].Entity;
    memcpy(before + 16 * i, g_NotedLights[i].Before, sizeof(float) * 16);
  }
  g_NotedLights.erase(g_NotedLights.begin(), g_NotedLights.begin() + count);
  ReleaseSRWLockExclusive(&g_LightNoteLock);
  return count;
}

const char* util::hooks::LightClassName(__int64 vtable)
{
  static const char* const names[] = { "analytic", "sphere", "spot", "tube", "rectangular" };
  for (int i = 0; i < 5; ++i)
    if (vtable == (__int64)g_gameHandle + LightVTableRvas[i])
      return names[i];
  return "?";
}

bool util::hooks::ReadLight(__int64 pEntity, __int64 vtable, float* transform)
{
  __try
  {
    if (*(__int64 const*)pEntity != vtable)
      return false;
  }
  __except (EXCEPTION_EXECUTE_HANDLER)
  {
    return false;
  }
  return ReadLightTransform(pEntity, transform);
}

bool util::hooks::MoveLight(__int64 pEntity, __int64 vtable, float const* transform)
{
  typedef void(__fastcall* tUpdateLight)(__int64);
  __try
  {
    if (*(__int64 const*)pEntity != vtable || !IsLightVTable(vtable))
      return false;
    // As the game's handler does (0x14376A896): the rows, then UpdateLight
    memcpy((void*)(pEntity + LightTransformOffset), transform, sizeof(float) * 16);
    ((tUpdateLight)((__int64)g_gameHandle + UpdateLightRva))(pEntity);
    return true;
  }
  __except (EXCEPTION_EXECUTE_HANDLER)
  {
    return false;
  }
}

void util::hooks::UpdateCrowdTrackers(bool on, float const* camera, float const* faith)
{
  if (!oTrackedPosition)
    return;
  if (on && camera)
  {
    int next = 1 - g_TrackerCameraIndex.load(std::memory_order_relaxed);
    memcpy(g_TrackerCamera[next], camera, sizeof(g_TrackerCamera[next]));
    g_TrackerCameraIndex.store(next, std::memory_order_release);
  }
  bool follow = on && camera;
  bool was = g_TrackersFollowCamera.exchange(follow, std::memory_order_acq_rel);
  if (follow != was)
    util::log::Write("Crowd and traffic sounds: %s", follow ? "placed from the free camera" : "placed by the level again");

  // Every 10 s while it's on: how many tracker updates, and where the level put them
  if (!util::DiagnosticsOn())
    return;
  static ULONGLONG s_lastLog = 0;
  ULONGLONG now = GetTickCount64();
  if (!follow || now - s_lastLog < 10000)
    return;
  s_lastLog = now;
  int updates = g_TrackerUpdates.exchange(0, std::memory_order_relaxed);
  if (g_TrackerHasLevelPosition.load(std::memory_order_relaxed))
  {
    float p[3];
    memcpy(p, g_TrackerLevelPosition, sizeof(p));
    util::log::Write("Crowd and traffic sounds: %d tracker updates in 10 s; the level tracks %.1f %.1f %.1f (%.0f m from Faith, "
      "%.0f m from the camera)", updates, p[0], p[1], p[2], faith ? SoundLogDistance(p, faith) : -1.f, SoundLogDistance(p, camera + 12));
  }
  else
    util::log::Write("Crowd and traffic sounds: %d tracker updates in 10 s", updates);
}

// Replay sounds. Faith's sounds are SoundEntities on her, each keeping one
// voice that's moved along with her every frame, and her logic plays them by
// giving those voices events (the Faith sound log, 2026-10-23: footsteps,
// breathing, hands, walls, the MagRope). Her sound entities (their sound under
// ME_Sound/Character/Faith/ or ME_Sound/Gadgets/) are noted as they get
// events. While the replay records, every event their voices get is kept with
// the replay's clock, and their transforms once per replay frame. While it
// plays, those voices are held at the recorded transforms and given the
// recorded events at their times with the voice's own event function (the
// original, past the hook); the events the game gives them meanwhile (the
// real Faith, standing where she is) are left out. A voice is only used while
// its entity still has it and the same sound; after a respawn her new voices
// are found by the sound's name. Game thread: the replay calls in from the
// camera hook, the hooks are called from the game's updates.
namespace
{
  enum { ReplaySoundIdle, ReplaySoundRecording, ReplaySoundPlaying };
  // Faith alone used 64 voices in one recording (2026-10-24), and the
  // guns' sounds come on top (see IsFaithSoundName)
  // 2026-10-24 22:43: all 256 voices taken in a 326 s fight recording (the
  // KrugerSec's own sounds came in), so later ones (a drone's) weren't recorded
  const int MaxFaithSoundEntities = 1024;
  const int MaxReplaySoundVoices = 1024;
  // Sounds that aren't Faith's (guns, the KrugerSec's own) are recorded only
  // while their voice is this near her
  const float ReplaySoundRadius = 150.f;
  const int VoiceStatePlaceOffset = 0x30;   // in the voice's 3D state (VoiceStateOffset; null: heard the same everywhere):   // its place (the rows' fourth, as slot 13 0x14301DC30 writes them)
  const int MaxReplaySoundFires = 128;     // events given in one frame at most (the rest wait for the next)

  struct FaithSoundEntity
  {
    __int64 Entity;
    __int64 Voice;
    __int64 Asset;
    char Name[96];
    alignas(16) float Transform[16];       // the last one the game gave its voice
    bool HasTransform;
    bool Nearby;                           // not Faith's own: recorded only near her, given up first when the list is full
    unsigned int SeenGen;                  // the generation the game last showed it in (see g_SoundGen)
  };
  struct ReplaySoundVoice
  {
    __int64 Entity;
    __int64 Voice;
    __int64 Asset;
    char Name[96];
    __int64 AssetVTable;     // its sound's vtable when recorded (0: unread)
  };
  struct ReplaySoundEvent
  {
    double Time;
    int Voice;
    unsigned int Event;
  };
  struct ReplaySoundPose
  {
    int Voice;
    float Transform[16];
  };

  FaithSoundEntity g_FaithSoundEntities[MaxFaithSoundEntities];
  int g_FaithSoundEntityCount = 0;
  int g_FaithSoundEntityNext = 0;          // replaced in turn once full
  std::atomic<int> g_ReplaySoundState{ ReplaySoundIdle };
  std::atomic_flag g_ReplaySoundLock = ATOMIC_FLAG_INIT;
  double g_ReplaySoundClock = 0;           // the replay's clock while it records
  std::vector<ReplaySoundVoice> g_ReplaySoundVoices;
  std::vector<ReplaySoundEvent> g_ReplaySoundEvents;
  std::vector<double> g_ReplaySoundFrameTimes;
  std::vector<int> g_ReplaySoundFrameStarts; // each frame's first pose
  std::vector<ReplaySoundPose> g_ReplaySoundPoses;
  // While it plays: the voice each recorded one is now (0: not there), where
  // it's held, and the next event
  __int64 g_ReplayPlayVoices[MaxReplaySoundVoices]{};
  // Voices the replay made itself for recorded ones whose voice is gone (after
  // a respawn Faith's are new ones, and so may a gun's be: 2026-10-24 20:54, 3
  // of 120 found). Made like a SoundEntity makes its voice (0x143019860 with
  // the voice manager [0x1423E29B8] and the sound, as the sound system's slot
  // 8 does), let go like it (the voice's slot 9, 0x14301D360, as a disabled
  // SoundEntity does in 0x14301A170); game thread only
  __int64 g_ReplayOwnVoices[MaxReplaySoundVoices]{};
  alignas(16) float g_ReplayPlayPoses[MaxReplaySoundVoices][16];
  bool g_ReplayPlayHasPose[MaxReplaySoundVoices]{};
  size_t g_ReplayPlayNext = 0;

  // A cutscene's lines (2026-10-26: the KrugerSec in a cutscene had no voice
  // lines in its replay). A timeline's sound track says a line on a voice of
  // its own (ME_Sound/VO/TimeLineVO_*: the 20:46 sound log, made from
  // 0x143901D68): its keyframe's trigger 0x14301C4E0 hands the voice the line,
  // the keyframe's asset (+ 0x18), through the voice's slot 2 (0x14301DA80:
  // [voice + 0x18], its graph, 0x143006180(graph, id, asset)) with the track's
  // parameter id, then, when the keyframe says so (+ 0x24), fires the sound's
  // DefaultStartEvent (patch + 0x68, its hash at + 0x14) through slot 0. Game
  // data: TimeLineVO_Player_Cone_wLPF plays through a SamplerNode whose
  // ExternalWave is the patch's one public asset parameter. The replay
  // recorded the start and fired it on a voice of its own that had no line.
  // Now each asset a recorded voice is given while it records is kept (its
  // time, voice, id, the asset and its name), and while it plays it's given to
  // that voice again just before the events of that time, only while the
  // asset still has the recorded vtable and name (a cutscene's lines may be
  // unloaded since: then it's left out).
  struct ReplaySoundAsset
  {
    double Time;
    int Voice;
    unsigned int Id;
    __int64 Asset, AssetVTable;
    char Name[96];
  };
  std::vector<ReplaySoundAsset> g_ReplaySoundAssets;     // under the lock
  size_t g_ReplayPlayAssetNext = 0;                      // under it
  int g_ReplaySoundAssetsGiven = 0, g_ReplaySoundAssetsLeftOut = 0; // game thread

  // Changes of level since a recording (2026-10-26 21:26:26: the game crashed
  // in its sound job building a voice from freed memory, 1 s after a playback
  // that came 15 s after the game took the recorded fight's containers out and
  // brought Faith back elsewhere; the replay had made 59 voices of its own
  // from recorded sounds trusted by their first 8 bytes, and took recorded
  // entities as there by reading their memory). Each change starts a new
  // generation: a loading screen, Faith gone or a new Faith, or a container
  // taken out with a recorded sound's entity in it (see SoundContainerGone).
  // Played in a later generation than the one its recording started in, a
  // recording uses a recorded entity, sound or line only once the game has
  // shown it's there in this one: an event of that entity
  // (NoteFaithSoundEntity), a voice made with that sound (hSoundCreate), that
  // sound given to a voice (hVoiceSetAsset), that line queued
  // (hVoiceLineQueue). The rest stay silent. Under the lock
  unsigned int g_SoundGen = 1;
  unsigned int g_RecordGen = 0;          // the generation the recording started in
  char const* g_SoundGenReason = "";     // what started this generation
  const int SeenDataSlots = 8192;        // a power of 2
  __int64 g_SeenData[SeenDataSlots]{};   // sounds and lines the game used in this generation (open addressing, 0 empty)
  int g_SeenDataCount = 0;
  // The generation each of the replay's own voices was made in; game thread only
  unsigned int g_ReplayOwnGens[MaxReplaySoundVoices]{};

  void LockReplaySounds() { while (g_ReplaySoundLock.test_and_set(std::memory_order_acquire)) {} }
  void UnlockReplaySounds() { g_ReplaySoundLock.clear(std::memory_order_release); }

  // Under the lock: a sound or line the game used in this generation (past
  // half full, no more are noted: those then stay silent)
  unsigned int SeenDataSlot(__int64 p)
  {
    return (unsigned int)(((unsigned __int64)p * 0x9E3779B97F4A7C15ull) >> 51);   // 13 bits
  }
  void NoteSeenData(__int64 p)
  {
    if (p < 0x10000 || g_SeenDataCount >= SeenDataSlots / 2)
      return;
    for (unsigned int k = 0, h = SeenDataSlot(p); k < (unsigned int)SeenDataSlots; ++k)
    {
      __int64& slot = g_SeenData[(h + k) & (SeenDataSlots - 1)];
      if (slot == p)
        return;
      if (!slot)
      {
        slot = p;
        ++g_SeenDataCount;
        return;
      }
    }
  }
  bool SeenData(__int64 p)
  {
    if (p < 0x10000)
      return false;
    for (unsigned int k = 0, h = SeenDataSlot(p); k < (unsigned int)SeenDataSlots; ++k)
    {
      __int64 slot = g_SeenData[(h + k) & (SeenDataSlots - 1)];
      if (slot == p)
        return true;
      if (!slot)
        return false;
    }
    return false;
  }
  // From the hooks, any thread
  void NoteSoundDataSeen(__int64 p)
  {
    if (p < 0x10000 || g_shutdown)
      return;
    LockReplaySounds();
    NoteSeenData(p);
    UnlockReplaySounds();
  }

  // Faith's sounds, and the weapons' (ME_Sound/Weapons/...: the KrugerSec
  // guns' shots, the drones', the VTOL's, the Crippler, the baton, the sniper
  // turret), which are SoundEntities on the guns played the same way (the
  // gun's firing component gives its sound an event, 2026-10-24 game data):
  // recorded and played again with hers
  bool IsFaithOwnSoundName(const char* name)
  {
    return !strncmp(name, "ME_Sound/Character/Faith/", 25) || !strncmp(name, "ME_Sound/Gadgets/", 17);
  }

  // Others near her: guns, drones' and VTOLs' weapons (ME_Sound/Weapons/...),
  // the Shock Protector's taser shotgun and charge (Sound/Weapons/..., the
  // 2026-10-24 22:04 sound log), the KrugerSec's footsteps, gear and melee
  // (ME_Sound/Character/AI/...) and the hunter drone's engine
  // (ME_Sound/Vehicles/HunterDrone/HunterDrone, started when it switches on;
  // Research_HunterDrone_2026-10-24.md), all sound entities there
  bool IsNearbySoundName(const char* name)
  {
    return !strncmp(name, "ME_Sound/Weapons/", 17) || !strncmp(name, "Sound/Weapons/", 14) ||
      !strncmp(name, "ME_Sound/Character/AI/", 22) || !strncmp(name, "ME_Sound/Vehicles/HunterDrone/", 30);
  }

  bool IsFaithSoundName(const char* name)
  {
    return IsFaithOwnSoundName(name) || IsNearbySoundName(name);
  }

  // A voice's place within radius of the real Faith (false when either
  // isn't known, or the voice is heard the same everywhere)
  bool IsVoiceWithin(__int64 pVoice, float radius)
  {
    if (!g_EffectCenterOk.load(std::memory_order_relaxed))
      return false;
    float place[3];
    __try
    {
      __int64 state = *(__int64 const*)(pVoice + VoiceStateOffset);
      if (state < 0x10000)
        return false;
      memcpy(place, (void const*)(state + VoiceStatePlaceOffset), sizeof(place));
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      return false;
    }
    float dx = place[0] - g_EffectCenter[0], dy = place[1] - g_EffectCenter[1], dz = place[2] - g_EffectCenter[2];
    return dx * dx + dy * dy + dz * dz <= radius * radius;
  }

  // A voice's place within ReplaySoundRadius of Faith (true when her place
  // isn't known; false for a voice heard the same everywhere)
  bool IsVoiceNearFaith(__int64 pVoice)
  {
    if (!g_EffectCenterOk.load(std::memory_order_relaxed))
      return true;
    float place[3];
    __try
    {
      __int64 state = *(__int64 const*)(pVoice + VoiceStateOffset);
      if (state < 0x10000)
        return false;
      memcpy(place, (void const*)(state + VoiceStatePlaceOffset), sizeof(place));
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      return false;
    }
    float dx = place[0] - g_EffectCenter[0], dy = place[1] - g_EffectCenter[1], dz = place[2] - g_EffectCenter[2];
    return dx * dx + dy * dy + dz * dz <= ReplaySoundRadius * ReplaySoundRadius;
  }

  // The MagRope's cues, heard as a hook point comes in reach (the HUD's, so
  // the replay leaves them out; they were heard while it played, 2026-10-25):
  // events of Faith's ME_Sound/Gadgets/MagRope/MagRopeGenElements, its four
  // alerts (the swing, pull up, pull down and line connector waves; which
  // event picks which isn't decoded) and its beeps, started by her animation's
  // tags (sound/logic/faith_movement_audio and the patch's wave switchers,
  // 2026-10-25 game data). The rope's own sounds on that voice stay: its
  // firing 0xDD7929E1, Attach 0x95107A6E, the line's 0x37AE542E / 0xE2A83156
  // and 0x2518DF46.
  bool IsMagRopeCue(const char* name, unsigned int event)
  {
    static const unsigned int cues[] = {
      0x75AC9230, 0x7590C44B, 0x1CCAFDDC,   // alerts: tag 0x15444E68, then game state 0x9A7926E6 = 0, 1, 2
      0x2103FAE6,                           // alert: her animation's event 0xA7C9D576
      0xDBBC206C, 0x1E32DAB7 };             // beeps: tag 0x7C53B6F1, then game state 0x9A7926E6 = 0, 1
    if (_stricmp(name, "ME_Sound/Gadgets/MagRope/MagRopeGenElements"))
      return false;
    for (unsigned int cue : cues)
      if (event == cue)
        return true;
    return false;
  }
  int g_MagRopeCuesNotRecorded = 0;        // under the replay sounds' lock
  int g_MagRopeCuesNotPlayed = 0;          // game thread

  // Temporary diagnostic (2026-10-25: "random sounds" heard at the real Faith
  // while a replay plays): what's heard while it plays, the live game's events
  // that aren't left out and the replay's own, by sound and event, logged when
  // it stops. The voices' sounds are noted as they're made (hSoundCreate), the
  // tools' camera's place from SetBillboardCamera.
  const int HeardVoiceBits = 15;           // 32768 voices remembered
  const int HeardVoiceSlots = 1 << HeardVoiceBits;
  const int HeardVoiceProbe = 32;
  const int MaxHeardKinds = 256;
  const int MaxHeardLines = 40;
  struct HeardVoice
  {
    __int64 Voice;
    __int64 Asset;
  };
  struct HeardKind
  {
    char Name[96];
    unsigned int Event;
    int Count;
    bool Positional;
    bool Made;                             // the replay's, on a voice it made
    float MinFaith;                        // metres from the real Faith (1e9: not known)
    float MinCamera;                       // from the tools' camera (1e9: not known)
  };
  HeardVoice g_HeardVoices[HeardVoiceSlots];
  HeardKind g_HeardLive[MaxHeardKinds];
  HeardKind g_HeardReplay[MaxHeardKinds];
  int g_HeardLiveCount = 0, g_HeardReplayCount = 0, g_HeardLiveEvents = 0, g_HeardReplayEvents = 0, g_HeardDropped = 0;
  float g_HeardCamera[3]{};                // a torn read only misplaces one line
  std::atomic<bool> g_HeardCameraOk{ false };
  std::atomic_flag g_HeardLock = ATOMIC_FLAG_INIT;

  void LockHeard() { while (g_HeardLock.test_and_set(std::memory_order_acquire)) {} }
  void UnlockHeard() { g_HeardLock.clear(std::memory_order_release); }

  unsigned __int64 HeardVoiceHome(__int64 pVoice)
  {
    return ((unsigned __int64)pVoice >> 4) * 0x9E3779B97F4A7C15ull >> (64 - HeardVoiceBits);
  }

  // From hSoundCreate, any thread: a voice made at an address replaces what
  // was there (none are taken out, so a full run gives up its first place)
  void NoteHeardVoice(__int64 pVoice, __int64 pAsset)
  {
    unsigned __int64 home = HeardVoiceHome(pVoice);
    LockHeard();
    HeardVoice* slot = &g_HeardVoices[home];
    for (int i = 0; i < HeardVoiceProbe; ++i)
    {
      HeardVoice& v = g_HeardVoices[(home + i) & (HeardVoiceSlots - 1)];
      if (v.Voice == pVoice || !v.Voice)
      {
        slot = &v;
        break;
      }
    }
    slot->Voice = pVoice;
    slot->Asset = pAsset;
    UnlockHeard();
  }

  // Under g_HeardLock (0: not known)
  __int64 HeardVoiceAsset(__int64 pVoice)
  {
    unsigned __int64 home = HeardVoiceHome(pVoice);
    for (int i = 0; i < HeardVoiceProbe; ++i)
    {
      HeardVoice const& v = g_HeardVoices[(home + i) & (HeardVoiceSlots - 1)];
      if (v.Voice == pVoice)
        return v.Asset;
      if (!v.Voice)
        return 0;
    }
    return 0;
  }

  // A voice's place (false when it's heard the same everywhere or can't be read)
  bool ReadVoicePlace(__int64 pVoice, float* place)
  {
    __try
    {
      __int64 state = *(__int64 const*)(pVoice + VoiceStateOffset);
      if (state < 0x10000)
        return false;
      memcpy(place, (void const*)(state + VoiceStatePlaceOffset), 3 * sizeof(float));
      return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      return false;
    }
  }

  float HeardDistance(float const* a, float const* b)
  {
    float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
    return sqrtf(dx * dx + dy * dy + dz * dz);
  }

  // Under g_HeardLock
  void AddHeard(HeardKind* kinds, int& count, const char* name, unsigned int event, bool positional, bool made,
    float const* place)
  {
    HeardKind* kind = nullptr;
    for (int i = 0; i < count && !kind; ++i)
      if (kinds[i].Event == event && kinds[i].Made == made && !strcmp(kinds[i].Name, name))
        kind = &kinds[i];
    if (!kind)
    {
      if (count >= MaxHeardKinds)
      {
        ++g_HeardDropped;
        return;
      }
      kind = &kinds[count++];
      *kind = HeardKind{};
      strncpy_s(kind->Name, name, _TRUNCATE);
      kind->Event = event;
      kind->Made = made;
      kind->MinFaith = kind->MinCamera = 1e9f;
    }
    ++kind->Count;
    kind->Positional |= positional;
    if (place && g_EffectCenterOk.load(std::memory_order_relaxed))
      kind->MinFaith = (std::min)(kind->MinFaith, HeardDistance(place, g_EffectCenter));
    if (place && g_HeardCameraOk.load(std::memory_order_relaxed))
      kind->MinCamera = (std::min)(kind->MinCamera, HeardDistance(place, g_HeardCamera));
  }

  // From hVoiceEvent while a replay plays: a live event that isn't left out
  void NoteHeardLive(__int64 pVoice, unsigned int event)
  {
    LockHeard();
    __int64 asset = HeardVoiceAsset(pVoice);
    UnlockHeard();
    char name[96];
    ReadSoundLogName(asset, name, sizeof(name));
    if (!name[0])
      strcpy_s(name, "(sound not known)");
    float place[3];
    bool placed = ReadVoicePlace(pVoice, place);
    bool positional = IsVoicePositional(pVoice);
    LockHeard();
    ++g_HeardLiveEvents;
    AddHeard(g_HeardLive, g_HeardLiveCount, name, event, positional, false, placed ? place : nullptr);
    UnlockHeard();
  }

  // From ReplaySoundsPlay under the replay sounds' lock: an event it gives
  void NoteHeardReplay(int index, __int64 voice, unsigned int event)
  {
    float const* place = g_ReplayPlayHasPose[index] ? &g_ReplayPlayPoses[index][12] : nullptr;
    bool positional = IsVoicePositional(voice);
    bool made = voice == g_ReplayOwnVoices[index];
    LockHeard();
    ++g_HeardReplayEvents;
    AddHeard(g_HeardReplay, g_HeardReplayCount, g_ReplaySoundVoices[index].Name, event, positional, made, place);
    UnlockHeard();
  }

  void ResetHeard()
  {
    LockHeard();
    g_HeardLiveCount = g_HeardReplayCount = g_HeardLiveEvents = g_HeardReplayEvents = g_HeardDropped = 0;
    UnlockHeard();
  }

  // Heard everywhere first, then the nearest to the real Faith
  void WriteHeard(const char* what, HeardKind* kinds, int count, int events)
  {
    std::sort(kinds, kinds + count, [](HeardKind const& a, HeardKind const& b) {
      if (a.Positional != b.Positional)
        return !a.Positional;
      return a.MinFaith < b.MinFaith;
    });
    util::log::Write("Replay (diagnostic): %s while it played: %d events of %d kinds (sound and event)%s", what, events,
      count, count > MaxHeardLines ? ", the first 40 listed" : "");
    for (int i = 0; i < count && i < MaxHeardLines; ++i)
    {
      HeardKind const& k = kinds[i];
      char where[96];
      if (!k.Positional)
        strcpy_s(where, "heard the same everywhere");
      else if (k.MinFaith < 1e8f && k.MinCamera < 1e8f)
        sprintf_s(where, "%.1f m from the real Faith, %.1f m from the camera at the nearest", k.MinFaith, k.MinCamera);
      else if (k.MinFaith < 1e8f)
        sprintf_s(where, "%.1f m from the real Faith at the nearest", k.MinFaith);
      else
        strcpy_s(where, "placed, where not known");
      util::log::Write("Replay (diagnostic):   %4dx %s event 0x%08X%s, %s", k.Count, k.Name, k.Event,
        k.Made ? " (a voice the replay made)" : "", where);
    }
  }

  void LogHeard()
  {
    static HeardKind live[MaxHeardKinds], replay[MaxHeardKinds];
    LockHeard();
    int liveCount = g_HeardLiveCount, replayCount = g_HeardReplayCount;
    int liveEvents = g_HeardLiveEvents, replayEvents = g_HeardReplayEvents, dropped = g_HeardDropped;
    memcpy(live, g_HeardLive, sizeof(HeardKind) * liveCount);
    memcpy(replay, g_HeardReplay, sizeof(HeardKind) * replayCount);
    UnlockHeard();
    WriteHeard("the live game's sounds still heard", live, liveCount, liveEvents);
    WriteHeard("the recorded sounds it gave", replay, replayCount, replayEvents);
    if (dropped)
      util::log::Write("Replay (diagnostic):   %d events of more kinds not counted", dropped);
  }

  bool ReadSoundEntity(__int64 pEntity, __int64& pVoice, __int64& pAsset)
  {
    __try
    {
      pVoice = *(__int64*)(pEntity + SoundEntityVoiceOffset);
      __int64 pData = *(__int64*)(pEntity + SoundEntityDataOffset);
      pAsset = pData ? *(__int64*)(pData + SoundEntityDataSoundOffset) : 0;
      return pVoice && pAsset;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      return false;
    }
  }

  // The entity still has that voice and that sound
  bool IsSoundEntityLive(__int64 pEntity, __int64 pVoice, __int64 pAsset)
  {
    __int64 voice = 0, asset = 0;
    return ReadSoundEntity(pEntity, voice, asset) && voice == pVoice && asset == pAsset;
  }

  // Under the lock: a noted one that's there (after a change of level since
  // the recording, stale, only once the game has shown it since: see
  // g_SoundGen)
  bool NotedSoundThere(FaithSoundEntity const& e, bool stale)
  {
    return e.Entity && (!stale || e.SeenGen == g_SoundGen) && IsSoundEntityLive(e.Entity, e.Voice, e.Asset);
  }

  // Under the lock: a recorded voice whose own entity still has it (when
  // stale, only as a noted one the game has shown since)
  bool RecordedSoundThere(ReplaySoundVoice const& rv, bool stale)
  {
    if (!stale)
      return IsSoundEntityLive(rv.Entity, rv.Voice, rv.Asset);
    for (int k = 0; k < g_FaithSoundEntityCount; ++k)
    {
      FaithSoundEntity const& e = g_FaithSoundEntities[k];
      if (e.Entity == rv.Entity && e.Voice == rv.Voice && e.Asset == rv.Asset)
        return NotedSoundThere(e, true);
    }
    return false;
  }

  // Voice lines (ME_Sound/VO/...: Faith's, the people she talks with, the
  // missions' and the radio's) are played by the voice-over system on voices
  // of their own, not by sound entities, so NoteFaithSoundEntity never saw
  // them and a recording's lines weren't in its replay (2026-10-25). Their
  // voices are taken in as they're made (hSoundCreate), as Faith's own
  // (recorded anywhere, and left out live while a replay plays), the voice
  // standing for its entity: while it plays the replay gives them voices of
  // its own (no sound entity has them, see IsSoundEntityLive). The city's
  // loudspeakers (ME_Sound/VO/Ambient/) aren't.
  bool IsVoiceOverName(const char* name)
  {
    return !_strnicmp(name, "ME_Sound/VO/", 12) && _strnicmp(name, "ME_Sound/VO/Ambient/", 20);
  }

  // From hSoundCreate, any thread
  void NoteVoiceOverVoice(__int64 pVoice, __int64 pAsset)
  {
    char name[96];
    ReadSoundLogName(pAsset, name, sizeof(name));
    bool voiceOver = IsVoiceOverName(name);
    LockReplaySounds();
    int slot = -1;
    for (int i = 0; i < g_FaithSoundEntityCount && slot < 0; ++i)
      if (g_FaithSoundEntities[i].Entity == pVoice && g_FaithSoundEntities[i].Voice == pVoice)
        slot = i;
    if (!voiceOver)
    {
      // A voice line's voice made again for another sound: not one any more
      if (slot >= 0)
        g_FaithSoundEntities[slot].Entity = g_FaithSoundEntities[slot].Voice = 0;
      UnlockReplaySounds();
      return;
    }
    if (slot < 0)
    {
      if (g_FaithSoundEntityCount < MaxFaithSoundEntities)
        slot = g_FaithSoundEntityCount++;
      else
      {
        // The oldest that isn't Faith's own goes first (as NoteFaithSoundEntity)
        for (int k = 0; k < MaxFaithSoundEntities && slot < 0; ++k)
          if (g_FaithSoundEntities[(g_FaithSoundEntityNext + k) % MaxFaithSoundEntities].Nearby)
            slot = (g_FaithSoundEntityNext + k) % MaxFaithSoundEntities;
        if (slot < 0)
          slot = g_FaithSoundEntityNext;
        g_FaithSoundEntityNext = (slot + 1) % MaxFaithSoundEntities;
      }
    }
    FaithSoundEntity& e = g_FaithSoundEntities[slot];
    e.Entity = pVoice;
    e.Voice = pVoice;
    e.Asset = pAsset;
    memcpy(e.Name, name, sizeof(name));
    e.HasTransform = false;
    e.Nearby = false;
    e.SeenGen = g_SoundGen;
    UnlockReplaySounds();
  }

  // From the SoundEntity hook: one of Faith's sound entities, with its voice
  void NoteFaithSoundEntity(__int64 pEntity)
  {
    __int64 pVoice = 0, pAsset = 0;
    if (!ReadSoundEntity(pEntity, pVoice, pAsset))
      return;
    LockReplaySounds();
    for (int i = 0; i < g_FaithSoundEntityCount; ++i)
    {
      FaithSoundEntity& e = g_FaithSoundEntities[i];
      if (e.Entity == pEntity && e.Voice == pVoice && e.Asset == pAsset)
      {
        e.SeenGen = g_SoundGen;
        UnlockReplaySounds();
        return;
      }
    }
    UnlockReplaySounds();
    char name[96];
    ReadSoundLogName(pAsset, name, sizeof(name));
    if (!IsFaithSoundName(name))
      return;
    LockReplaySounds();
    int slot = -1;
    for (int i = 0; i < g_FaithSoundEntityCount && slot < 0; ++i)
      if (g_FaithSoundEntities[i].Entity == pEntity)
        slot = i;
    if (slot < 0)
    {
      if (g_FaithSoundEntityCount < MaxFaithSoundEntities)
        slot = g_FaithSoundEntityCount++;
      else
      {
        // The oldest that isn't Faith's own goes first
        for (int k = 0; k < MaxFaithSoundEntities && slot < 0; ++k)
          if (g_FaithSoundEntities[(g_FaithSoundEntityNext + k) % MaxFaithSoundEntities].Nearby)
            slot = (g_FaithSoundEntityNext + k) % MaxFaithSoundEntities;
        if (slot < 0)
          slot = g_FaithSoundEntityNext;
        g_FaithSoundEntityNext = (slot + 1) % MaxFaithSoundEntities;
      }
    }
    FaithSoundEntity& e = g_FaithSoundEntities[slot];
    e.Entity = pEntity;
    e.Voice = pVoice;
    e.Asset = pAsset;
    memcpy(e.Name, name, sizeof(name));
    e.HasTransform = false;
    e.Nearby = !IsFaithOwnSoundName(name);
    e.SeenGen = g_SoundGen;
    UnlockReplaySounds();
  }

  __int64 ReadAssetVTable(__int64 asset)
  {
    __try
    {
      return asset > 0x10000 ? *(__int64 const*)asset : 0;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      return 0;
    }
  }

  // Under the lock: the recorded voice for one of her entities, added if new
  // Under the lock: one of her entities is among the recorded voices already
  bool IsRecordedSoundVoice(FaithSoundEntity const& e)
  {
    for (ReplaySoundVoice const& v : g_ReplaySoundVoices)
      if (v.Voice == e.Voice && v.Entity == e.Entity)
        return true;
    return false;
  }

  int ReplaySoundVoiceIndex(FaithSoundEntity const& e)
  {
    for (size_t i = 0; i < g_ReplaySoundVoices.size(); ++i)
      if (g_ReplaySoundVoices[i].Voice == e.Voice && g_ReplaySoundVoices[i].Entity == e.Entity)
        return (int)i;
    if (g_ReplaySoundVoices.size() >= (size_t)MaxReplaySoundVoices)
      return -1;
    ReplaySoundVoice v{ e.Entity, e.Voice, e.Asset, {}, ReadAssetVTable(e.Asset) };
    memcpy(v.Name, e.Name, sizeof(v.Name));
    g_ReplaySoundVoices.push_back(v);
    return (int)g_ReplaySoundVoices.size() - 1;
  }

  // From the voice event hook: records it, or says to leave it out (true)
  bool ReplaySoundOnEvent(__int64 pVoice, unsigned int event)
  {
    int state = g_ReplaySoundState.load(std::memory_order_relaxed);
    bool live = g_ReplayPlaying.load(std::memory_order_relaxed);
    if (state == ReplaySoundIdle && !live)
      return false;
    bool leaveOut = false;
    LockReplaySounds();
    if (state == ReplaySoundRecording)
    {
      for (int i = 0; i < g_FaithSoundEntityCount; ++i)
      {
        if (g_FaithSoundEntities[i].Voice != pVoice)
          continue;
        // Not hers: only near her (so the far ones don't take the voices)
        if (g_FaithSoundEntities[i].Nearby && !IsVoiceNearFaith(pVoice))
          break;
        // The MagRope's cues (the HUD's) aren't kept
        if (IsMagRopeCue(g_FaithSoundEntities[i].Name, event))
        {
          ++g_MagRopeCuesNotRecorded;
          break;
        }
        int index = ReplaySoundVoiceIndex(g_FaithSoundEntities[i]);
        if (index >= 0)
          g_ReplaySoundEvents.push_back({ g_ReplaySoundClock, index, event });
        break;
      }
    }
    else
    {
      if (state == ReplaySoundPlaying)
        for (int i = 0; i < MaxReplaySoundVoices && !leaveOut; ++i)
          leaveOut = g_ReplayPlayVoices[i] == pVoice;
      // And while a replay plays, the live ones of the same kinds (hers, the
      // guns', the KrugerSec's; see IsFaithSoundName) within
      // LiveLeaveOutRadius of the real Faith: her own where she stands, and
      // the fight going on round her
      if (live && !leaveOut)
      {
        // Faith's own wherever they're heard (her HUD's sounds are heard the
        // same everywhere), the guns' and the KrugerSec's near her
        for (int i = 0; i < g_FaithSoundEntityCount; ++i)
        {
          if (g_FaithSoundEntities[i].Voice != pVoice)
            continue;
          leaveOut = !g_FaithSoundEntities[i].Nearby || IsVoiceWithin(pVoice, LiveLeaveOutRadius);
          if (leaveOut)
            g_LiveVoiceEventsLeftOut.fetch_add(1, std::memory_order_relaxed);
          break;
        }
        // The other sounds played for the player (see IsPlayerSoundName)
        if (!leaveOut && IsPlayerVoice(pVoice))
        {
          leaveOut = true;
          g_LivePlayerEventsLeftOut.fetch_add(1, std::memory_order_relaxed);
        }
      }
    }
    UnlockReplaySounds();
    return leaveOut;
  }

  // From the transform hook: keeps the game's transform for her voices and,
  // while the replay plays, gives the recorded one instead (true: use held)
  bool ReplaySoundOnTransform(__int64 pVoice, float const* pTransform, float* held)
  {
    int state = g_ReplaySoundState.load(std::memory_order_relaxed);
    if (state == ReplaySoundIdle)
      return false;
    bool hold = false;
    LockReplaySounds();
    for (int i = 0; i < g_FaithSoundEntityCount; ++i)
    {
      FaithSoundEntity& e = g_FaithSoundEntities[i];
      if (e.Voice == pVoice)
      {
        memcpy(e.Transform, pTransform, sizeof(e.Transform));
        e.HasTransform = true;
      }
    }
    if (state == ReplaySoundPlaying)
    {
      for (int i = 0; i < MaxReplaySoundVoices && !hold; ++i)
      {
        if (g_ReplayPlayVoices[i] == pVoice && g_ReplayPlayHasPose[i])
        {
          memcpy(held, g_ReplayPlayPoses[i], sizeof(g_ReplayPlayPoses[i]));
          hold = true;
        }
      }
    }
    UnlockReplaySounds();
    return hold;
  }
}

void util::hooks::SetReplayPlaying(bool playing)
{
  if (g_ReplayPlaying.exchange(playing) == playing || playing)
  {
    if (playing)
    {
      g_LiveEffectSoundsLeftOut = 0;
      g_LiveVoiceEventsLeftOut = 0;
      g_LivePlayerEventsLeftOut = 0;
      ResetHeard();
    }
    return;
  }
  if (util::DiagnosticsOn())
    LogHeard();
  int effects = g_LiveEffectSoundsLeftOut.exchange(0), events = g_LiveVoiceEventsLeftOut.exchange(0),
    player = g_LivePlayerEventsLeftOut.exchange(0);
  if (effects || events || player)
    util::log::Write("Replay: the live game's sounds for the real Faith left out while it played: %d impact and collision sounds "
      "near her, %d events of her own (her HUD's too), the guns' and the KrugerSec's near her, %d of the bullets flying by her "
      "and the city alert", effects, events, player);
}

// Voice lines in the replay (2026-10-25). NoteVoiceOverVoice took the
// voice-over voices in as sounds of Faith's, so the replay recorded the events
// they got (the 15:30 recording: 2 starts 0x0DC0EFA5 on
// ME_Sound/VO/TimeLineVO_Player_Faith, 3 events on ..._Cone_wLPF) and played
// them on voices of its own made from the same sound. But the voice-over
// system doesn't say a line through events: its line update 0x14303C5E0 keeps
// a few voices per speaker (made through the sound system's slot 8, kept at
// [entry + 8], 0x14303C6B0) and, for each line, looks up a node of the voice's
// sound graph (the voice's slot 7, 0x14301A920 -> 0x143003340(graph, the
// node's id [speaker + 0x48]), at 0x14303C925), queues the line's wave on that
// node (0x143027E90(node, wave, language from 0x143035C90, &slot), at
// 0x14303CA3F: the wave's stream looked up by the 16 bytes before it
// (0x14302BE00), the slot (two at most, node + 0x88 bits 6..8 the count)
// filled by 0x143027C80), starts it (0x14302C110(node, slot): slot flags |=
// 0x800, at 0x14303CBCC) and clears it when it's done (0x143023200(node,
// slot), at 0x14303CD93). Each of the three is called from there only. A start
// event on a voice the replay made reached no wave, so the replay was silent.
// The 18:46 recording's diagnostic showed the missions' dialog on that path:
// ME_Sound/VO/VO_Narrative_Faith (Faith's lines) and
// VO_Narrative_RadioOnDistance (the one on the radio), with the people's
// (VO_Ambient, VO_AI_DistantAmbient) and Faith's emotes (VO_Faith_Emotes_*), 88
// lines in 125 s; each narrative line on a voice of its own, the node's id the
// same for each sound (game data: VO_Narrative_Faith is a persistent patch
// whose graph plays through a DialogSamplerNode, Triggered not connected, so
// it needs no event; its output is placed, radius 0.5 m).
// Recording: each line the system queues while the replay records (the
// queue, start and clear hooked after their code was checked; the node's voice
// and id from the voice's slot 7 when the voice-over system calls it, return
// address 0x14303C928), with the times the game started and cleared it, the
// voice taken in among the recorded ones (ReplaySoundVoiceIndex: the replay
// makes a voice of its own for it from the same sound when it plays, and
// holds it where the game had the line's voice, its places recorded with the
// others'). Playing: at each line's times, on that voice, the node looked up
// with the same id (the voice's slot 7), the same wave queued in the same
// language, started and cleared, through the game's own functions (past the
// hooks); only while the 16 bytes before the wave are still the recorded
// stream id (a mission's dialog may be unloaded since: then it's left out).
// A seek, a loop or the start clears what's playing, and lines from there on
// are said; one under way then isn't joined halfway.
typedef __int64(__fastcall* tVoiceNode)(__int64, __int64);
tVoiceNode oVoiceNode = nullptr;
typedef char(__fastcall* tVoiceLineQueue)(__int64, __int64, __int64, int*);
tVoiceLineQueue oVoiceLineQueue = nullptr;
typedef void(__fastcall* tVoiceLineSlot)(__int64, int);
tVoiceLineSlot oVoiceLineStart = nullptr;
tVoiceLineSlot oVoiceLineClear = nullptr;

namespace
{
  const __int64 VoiceNodeRva = 0x301A920;          // the voice's slot 7
  const __int64 VoiceVTableRva = 0x1ACFAD0;
  const __int64 VoiceOverNodeReturnRva = 0x303C928; // after call [rax + 0x38] in 0x14303C5E0
  const __int64 VoiceLineQueueRva = 0x3027E90;
  const __int64 VoiceLineQueueCallRva = 0x303CA3F;
  const __int64 VoiceLineStartRva = 0x302C110;
  const __int64 VoiceLineStartCallRva = 0x303CBCC;
  const __int64 VoiceLineClearRva = 0x3023200;
  const __int64 VoiceLineClearCallRva = 0x303CD93;
  const int MaxVoiceOverNodes = 256;
  const size_t MaxVoiceLines = 4096;

  struct VoiceOverNode
  {
    __int64 Node, Voice, Id;
  };
  struct VoiceLine
  {
    double Time;           // queued, on the replay's clock
    double Start, End;     // started and cleared (-1: not while it recorded)
    __int64 Voice, Id, Node, Wave, Language;
    BYTE Stream[16];
    int Slot;              // the node's slot the game got (0: not queued)
    int Index;             // its voice among the recorded ones (-1: none)
    char Name[96];
  };
  struct VoiceLinePlay
  {
    __int64 Node;          // the replay's voice's node it's queued on (0: not)
    int Slot;
    bool Done, Started, Cleared;
    bool Track;            // heard from the sound track (not said live: its wave unloaded, or joined halfway after a seek)
  };
  // A line's span on the replay's clock (2026-11-01, the sound track keeps
  // only the dialog): queued until done, or until started + LineSeconds when
  // it wasn't done while it recorded
  const double LineSeconds = 8.0;
  double LineEnd(VoiceLine const& l)
  {
    return l.End >= 0.0 ? l.End : (l.Start >= 0.0 ? l.Start : l.Time) + LineSeconds;
  }
  // The mission's dialog (2026-11-01: the 04:37 recording of Release kept
  // 148.2 s of 149.7 s, its 63 lines mostly KrugerSec's distant chatter
  // (VO_AI_DistantAmbient), their barks (VO_AI_Talk) and the prison's
  // loudspeakers (VO_PA_LoFi), one after another): a mission's narrative
  // (ME_Sound/VO/VO_Narrative, VO_Narrative_Faith) and a cutscene's
  // voice-over (ME_Sound/VO/TimeLineVO_*). The others are still said live by
  // the replay; the sound track left them out until 2026-11-04 (now it keeps
  // them too, heard only when the replay can't say one live: see
  // ReplayVoiceLineSpans). A sound given to a
  // recorded voice (ReplaySoundAsset) is a cutscene's voice-over when its
  // name is under /VO/ and no voice line was said on that voice (a voice-over
  // system's voice follows its lines' rule). 2026-11-02 (the user: "kruger sec
  // annoucnment doesn't show up"): the loudspeakers' announcements
  // (ME_Sound/VO/VO_PA_LoFi, 31.9 s and 41.2 s of the 19:40 recording) too.
  // Like the others, one is heard from the sound track only when the replay
  // can't say it live (the 19:42 playback had 11 of the 39 recorded voices:
  // the mission's sounds were gone).
  bool MissionDialog(char const* name)
  {
    return strstr(name, "/VO/VO_Narrative") || strstr(name, "/VO/TimeLineVO") || strstr(name, "/VO/VO_PA");
  }
  VoiceOverNode g_VoiceOverNodes[MaxVoiceOverNodes]{};   // replaced in turn; under g_ReplaySoundLock
  int g_VoiceOverNodeNext = 0;
  std::vector<VoiceLine> g_VoiceLines;                   // the recording's; under it (not changed while it plays)
  int g_VoiceLinesDropped = 0;                           // under it
  std::vector<VoiceLinePlay> g_VoiceLinePlay;            // game thread, while it plays
  int g_VoiceLinesPlayed = 0, g_VoiceLinesLeftOut = 0;   // game thread

  // A sound given to a recorded voice: a cutscene's voice-over (see
  // MissionDialog); under the replay's sound lock
  bool CutsceneVoiceOver(ReplaySoundAsset const& a)
  {
    if ((!strstr(a.Name, "/VO/") && !strstr(a.Name, "/vo/")) || strstr(a.Name, "/VO/Ambient/"))   // the loudspeakers' aren't
      return false;
    for (VoiceLine const& l : g_VoiceLines)
      if (l.Index >= 0 && l.Index == a.Voice)
        return false;
    return true;
  }

  bool ReadVoiceLineStream(__int64 wave, BYTE* out)
  {
    __try
    {
      memcpy(out, (void const*)(wave - 0x10), 16);
      return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      memset(out, 0, 16);
      return false;
    }
  }

  // Game thread: the game's own functions (past the hooks) on a voice the
  // replay made
  __int64 VoiceLineNode(__int64 voice, __int64 id)
  {
    __try
    {
      return oVoiceNode(voice, id);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      return 0;
    }
  }

  bool QueueVoiceLine(__int64 node, __int64 wave, __int64 language, int& slot)
  {
    __try
    {
      slot = 0;
      return oVoiceLineQueue(node, wave, language, &slot) != 0 && slot > 0;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      return false;
    }
  }

  void VoiceLineSlotCall(tVoiceLineSlot call, __int64 node, int slot)
  {
    __try
    {
      call(node, slot);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
  }

  // Under the replay sounds' lock: the recorded line the game queued last on
  // a node's slot that hasn't had this time yet (Start or End)
  VoiceLine* FindVoiceLine(__int64 node, int slot, bool start)
  {
    for (size_t i = g_VoiceLines.size(); i-- > 0;)
    {
      VoiceLine& l = g_VoiceLines[i];
      if (l.Node == node && l.Slot == slot && (start ? l.Start : l.End) < 0.0)
        return &l;
    }
    return nullptr;
  }

  // Game thread: what the replay has queued cleared, and the lines forgotten
  void StopVoiceLines()
  {
    for (VoiceLinePlay& p : g_VoiceLinePlay)
      if (p.Node && !p.Cleared)
      {
        VoiceLineSlotCall(oVoiceLineClear, p.Node, p.Slot);
        p.Cleared = true;
      }
    g_VoiceLinePlay.clear();
  }

  // Game thread, from ReplaySoundsPlay (outside the lock): the recorded lines
  // at their times (see above); restart: a seek, a loop or the start
  // stale: a change of level since the recording (see g_SoundGen)
  void PlayVoiceLines(double time, bool restart, bool stale)
  {
    if (!oVoiceNode || !oVoiceLineQueue || !oVoiceLineStart || !oVoiceLineClear)
      return;
    size_t count = g_VoiceLines.size();
    if (g_VoiceLinePlay.size() != count)
    {
      StopVoiceLines();
      g_VoiceLinePlay.assign(count, VoiceLinePlay{});
      restart = true;
    }
    if (restart)
    {
      for (size_t i = 0; i < count; ++i)
      {
        VoiceLinePlay& p = g_VoiceLinePlay[i];
        if (p.Node && !p.Cleared)
          VoiceLineSlotCall(oVoiceLineClear, p.Node, p.Slot);
        p = VoiceLinePlay{};
        p.Done = g_VoiceLines[i].Time < time && time > 0.0;
        // One under way there isn't said live from halfway: the track has it
        p.Track = p.Done && time <= LineEnd(g_VoiceLines[i]);
      }
    }
    for (size_t i = 0; i < count; ++i)
    {
      VoiceLine const& l = g_VoiceLines[i];
      VoiceLinePlay& p = g_VoiceLinePlay[i];
      if (!p.Done && l.Time <= time)
      {
        p.Done = true;
        __int64 voice = l.Index >= 0 && l.Index < MaxReplaySoundVoices ? g_ReplayPlayVoices[l.Index] : 0;
        if (voice && stale)
        {
          LockReplaySounds();
          if (!SeenData(l.Wave))
            voice = 0;
          UnlockReplaySounds();
        }
        BYTE stream[16];
        __int64 node = 0;
        int slot = 0;
        if (!voice || l.Slot <= 0 || l.Start < 0.0 || !ReadVoiceLineStream(l.Wave, stream) || memcmp(stream, l.Stream, 16) ||
            !(node = VoiceLineNode(voice, l.Id)) || !QueueVoiceLine(node, l.Wave, l.Language, slot))
        {
          ++g_VoiceLinesLeftOut;
          p.Track = true;   // heard from the sound track instead
          continue;
        }
        p.Node = node;
        p.Slot = slot;
        ++g_VoiceLinesPlayed;
      }
      if (p.Node && !p.Started && time >= l.Start)
      {
        VoiceLineSlotCall(oVoiceLineStart, p.Node, p.Slot);
        p.Started = true;
      }
      if (p.Node && p.Started && !p.Cleared && l.End >= 0.0 && time >= l.End)
      {
        VoiceLineSlotCall(oVoiceLineClear, p.Node, p.Slot);
        p.Cleared = true;
      }
    }
  }
}

__int64 __fastcall hVoiceNode(__int64 pVoice, __int64 id)
{
  __int64 node = oVoiceNode(pVoice, id);
  if (node && (__int64)_ReturnAddress() == (__int64)g_gameHandle + VoiceOverNodeReturnRva)
  {
    LockReplaySounds();
    int slot = -1;
    for (int i = 0; i < MaxVoiceOverNodes && slot < 0; ++i)
      if (g_VoiceOverNodes[i].Node == node)
        slot = i;
    if (slot < 0)
    {
      slot = g_VoiceOverNodeNext;
      g_VoiceOverNodeNext = (g_VoiceOverNodeNext + 1) % MaxVoiceOverNodes;
    }
    g_VoiceOverNodes[slot] = VoiceOverNode{ node, pVoice, id };
    UnlockReplaySounds();
  }
  return node;
}

char __fastcall hVoiceLineQueue(__int64 node, __int64 wave, __int64 language, int* slot)
{
  char queued = oVoiceLineQueue(node, wave, language, slot);
  // The game used that line: there in this generation (see g_SoundGen)
  if (queued)
    NoteSoundDataSeen(wave);
  if (g_ReplaySoundState.load(std::memory_order_relaxed) != ReplaySoundRecording)
    return queued;
  VoiceLine line{};
  line.Node = node;
  line.Wave = wave;
  line.Language = language;
  line.Slot = queued && slot ? *slot : 0;
  line.Start = line.End = -1.0;
  line.Index = -1;
  ReadVoiceLineStream(wave, line.Stream);
  LockReplaySounds();
  for (int i = 0; i < MaxVoiceOverNodes; ++i)
    if (g_VoiceOverNodes[i].Node == node)
    {
      line.Voice = g_VoiceOverNodes[i].Voice;
      line.Id = g_VoiceOverNodes[i].Id;
      break;
    }
  line.Time = g_ReplaySoundClock;
  UnlockReplaySounds();
  __int64 asset = 0;
  if (line.Voice)
  {
    LockHeard();
    asset = HeardVoiceAsset(line.Voice);
    UnlockHeard();
  }
  ReadSoundLogName(asset, line.Name, sizeof(line.Name));
  if (!line.Name[0])
    strcpy_s(line.Name, "(sound not known)");
  LockReplaySounds();
  // Its voice among the recorded ones (noted as a voice-over voice when made,
  // see NoteVoiceOverVoice)
  for (int i = 0; i < g_FaithSoundEntityCount && line.Voice && line.Index < 0; ++i)
    if (g_FaithSoundEntities[i].Voice == line.Voice && g_FaithSoundEntities[i].Entity == line.Voice)
      line.Index = ReplaySoundVoiceIndex(g_FaithSoundEntities[i]);
  if (g_VoiceLines.size() < MaxVoiceLines)
    g_VoiceLines.push_back(line);
  else
    ++g_VoiceLinesDropped;
  UnlockReplaySounds();
  return queued;
}

void __fastcall hVoiceLineStart(__int64 node, int slot)
{
  oVoiceLineStart(node, slot);
  if (g_ReplaySoundState.load(std::memory_order_relaxed) != ReplaySoundRecording)
    return;
  LockReplaySounds();
  if (VoiceLine* line = FindVoiceLine(node, slot, true))
    line->Start = g_ReplaySoundClock;
  UnlockReplaySounds();
}

void __fastcall hVoiceLineClear(__int64 node, int slot)
{
  oVoiceLineClear(node, slot);
  if (g_ReplaySoundState.load(std::memory_order_relaxed) != ReplaySoundRecording)
    return;
  LockReplaySounds();
  if (VoiceLine* line = FindVoiceLine(node, slot, false))
    line->End = g_ReplaySoundClock;
  UnlockReplaySounds();
}

static bool IsVoiceLineCode(BYTE const* pBase)
{
  // The voice's slot 7: mov rcx, [rcx + 0x20]; test rcx, rcx; jne 0x143003340; xor eax, eax; ret
  static const BYTE node[] = { 0x48, 0x8B, 0x49, 0x20, 0x48, 0x85, 0xC9, 0x0F, 0x85 };
  // Its call in the line update: mov rdx, [r14 + 0x48]; mov rcx, rsi; call [rax + 0x38]
  static const BYTE nodeCall[] = { 0x49, 0x8B, 0x56, 0x48, 0x48, 0x89, 0xF1, 0xFF, 0x50, 0x38 };
  // The queue: mov [rsp + 8], rbx; mov [rsp + 0x10], rsi; push rdi; sub rsp, 0x20; mov rsi, rcx; mov rcx, [rip + ...]
  static const BYTE queue[] = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x74, 0x24, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x20, 0x48,
    0x89, 0xCE, 0x48, 0x8B, 0x0D };
  // The start: lea eax, [rdx - 1]; lea rax, [rax + rax * 4]; or dword [rcx + rax * 8 + 0x24], 0x800; ret
  static const BYTE start[] = { 0x8D, 0x42, 0xFF, 0x48, 0x8D, 0x04, 0x80, 0x81, 0x4C, 0xC1, 0x24, 0x00, 0x08, 0x00, 0x00, 0xC3 };
  // The clear: lea eax, [rdx - 1]; lea rax, [rax + rax * 4]; and dword [rcx + rax * 8 + 0x24], ~0x4000;
  // mov eax, [rcx + 0x88] (the slots' count then taken down)
  static const BYTE clear[] = { 0x8D, 0x42, 0xFF, 0x48, 0x8D, 0x04, 0x80, 0x81, 0x64, 0xC1, 0x24, 0xFF, 0xBF, 0xFF, 0xFF, 0x8B,
    0x81, 0x88, 0x00, 0x00, 0x00 };
  auto calls = [pBase](__int64 at, __int64 target) {
    return pBase[at] == 0xE8 && at + 5 + *(int const*)(pBase + at + 1) == target;
  };
  BYTE const* n = pBase + VoiceNodeRva;
  return !memcmp(n, node, sizeof(node)) && VoiceNodeRva + 13 + *(int const*)(n + 9) == 0x3003340 && n[13] == 0x31 &&
    n[14] == 0xC0 && n[15] == 0xC3 && *(__int64 const*)(pBase + VoiceVTableRva + 7 * 8) == (__int64)pBase + VoiceNodeRva &&
    !memcmp(pBase + VoiceOverNodeReturnRva - sizeof(nodeCall), nodeCall, sizeof(nodeCall)) &&
    !memcmp(pBase + VoiceLineQueueRva, queue, sizeof(queue)) && calls(VoiceLineQueueCallRva, VoiceLineQueueRva) &&
    !memcmp(pBase + VoiceLineStartRva, start, sizeof(start)) && calls(VoiceLineStartCallRva, VoiceLineStartRva) &&
    !memcmp(pBase + VoiceLineClearRva, clear, sizeof(clear)) && calls(VoiceLineClearCallRva, VoiceLineClearRva);
}

// A change of level (see g_SoundGen): a new generation, said in the log while
// there's a recording
static void SoundLevelChanged(char const* why)
{
  LockReplaySounds();
  bool recorded = !g_ReplaySoundVoices.empty() || !g_VoiceLines.empty();
  ++g_SoundGen;
  g_SoundGenReason = why;
  memset(g_SeenData, 0, sizeof(g_SeenData));
  g_SeenDataCount = 0;
  UnlockReplaySounds();
  if (recorded)
    util::log::Write("Replay: %s: a recording's sounds play only where the game shows they're still there", why);
}

// A container's entities, as ReadContainerPeople walks them (game memory)
static int ReadContainerEntities(__int64 container, __int64* out, int max)
{
  int count = 0;
  __try
  {
    for (__int64 node = *(__int64 const*)(container + 0x10); node >= 0x10000 && count < max; node = *(__int64 const*)(node + 8))
      out[count++] = node - 8;
  }
  __except (EXCEPTION_EXECUTE_HANDLER)
  {
  }
  return count;
}

// A container taken out (hContainerDestroyAll, before the game does it): the
// noted sound entities in it are let go (never matched again), and a recorded
// one in it starts a new generation (see g_SoundGen). Voice-over voices
// aren't entities (see NoteVoiceOverVoice)
static void SoundContainerGone(__int64 container)
{
  const int ContainerEntitiesMax = 4096;
  if (g_shutdown || container < 0x10000)
    return;
  std::vector<__int64> entities(ContainerEntitiesMax);
  int count = ReadContainerEntities(container, entities.data(), ContainerEntitiesMax);
  if (!count)
    return;
  entities.resize(count);
  std::sort(entities.begin(), entities.end());
  auto in = [&](__int64 entity) { return entity && std::binary_search(entities.begin(), entities.end(), entity); };
  bool recorded = false;
  LockReplaySounds();
  for (int i = 0; i < g_FaithSoundEntityCount; ++i)
  {
    FaithSoundEntity& e = g_FaithSoundEntities[i];
    if (e.Entity != e.Voice && in(e.Entity))
    {
      e.Entity = e.Voice = e.Asset = 0;
      e.HasTransform = false;
    }
  }
  for (ReplaySoundVoice const& rv : g_ReplaySoundVoices)
    if (rv.Entity != rv.Voice && in(rv.Entity))
    {
      recorded = true;
      break;
    }
  UnlockReplaySounds();
  if (recorded)
    SoundLevelChanged("parts of the level with recorded sounds in them were taken out");
}

// When Faith's soldier went (0: she's there, or never was), for the hang watch
std::atomic<ULONGLONG> g_FaithGoneSince{ 0 };

ULONGLONG util::hooks::FaithGoneSince()
{
  return g_FaithGoneSince.load();
}

void util::hooks::NoteFaithSoldier(__int64 soldier)
{
  static __int64 s_last = 0;   // camera update thread only
  if (soldier == s_last)
    return;
  __int64 last = s_last;
  s_last = soldier;
  g_FaithGoneSince = soldier ? 0 : GetTickCount64();
  if (last)
    SoundLevelChanged(soldier ? "a new Faith (a respawn)" : "Faith gone (dead or loading)");
}

void util::hooks::ReplaySoundsStartRecording()
{
  if (!oVoiceEvent || !oVoiceSetTransform || !oSoundEntityEvent)
    return;
  LockReplaySounds();
  g_RecordGen = g_SoundGen;
  g_VoiceLines.clear();
  g_VoiceLinesDropped = 0;
  g_ReplaySoundVoices.clear();
  g_ReplaySoundEvents.clear();
  g_ReplaySoundAssets.clear();
  g_ReplaySoundFrameTimes.clear();
  g_ReplaySoundFrameStarts.clear();
  g_ReplaySoundPoses.clear();
  g_ReplaySoundVoices.reserve(MaxReplaySoundVoices);
  g_ReplaySoundEvents.reserve(16384);
  for (int i = 0; i < g_FaithSoundEntityCount; ++i)
    g_FaithSoundEntities[i].HasTransform = false;
  g_ReplaySoundClock = 0;
  g_MagRopeCuesNotRecorded = 0;
  UnlockReplaySounds();
  g_ReplaySoundState = ReplaySoundRecording;
}

void util::hooks::ReplaySoundsRecordFrame(double time, bool sample)
{
  if (g_ReplaySoundState.load() != ReplaySoundRecording)
    return;
  LockReplaySounds();
  g_ReplaySoundClock = time;
  if (sample)
  {
    g_ReplaySoundFrameTimes.push_back(time);
    g_ReplaySoundFrameStarts.push_back((int)g_ReplaySoundPoses.size());
    for (int i = 0; i < g_FaithSoundEntityCount; ++i)
    {
      FaithSoundEntity const& e = g_FaithSoundEntities[i];
      // A voice-over voice (no sound entity: the voice is its own, see
      // NoteVoiceOverVoice) once it has said a line in the recording (see
      // hVoiceLineQueue): its place as the game last gave it
      bool voiceOver = e.Entity == e.Voice;
      if (!e.HasTransform || (voiceOver ? !IsRecordedSoundVoice(e) : !IsSoundEntityLive(e.Entity, e.Voice, e.Asset)))
        continue;
      int index = ReplaySoundVoiceIndex(e);
      if (index < 0)
        continue;
      ReplaySoundPose pose;
      pose.Voice = index;
      memcpy(pose.Transform, e.Transform, sizeof(pose.Transform));
      g_ReplaySoundPoses.push_back(pose);
    }
  }
  UnlockReplaySounds();
}

// The sound track keeps only the dialog (2026-11-01; the user: "The game's
// sound as recorded should only record dialog of the mission unless we find a
// better way and replays play the dialog of the npc/character/mission
// character live"): the spans of the voice lines (a mission's narrative,
// Faith's among them, and the others: KrugerSec's, the people's) and of a
// cutscene's voice-over sounds; false when the voice lines aren't hooked (then
// nothing says where the dialog is). The others are still said live when the
// replay can; the track is heard during one only when it can't (see
// ReplayDialogFromTrack). Until 2026-11-04 the others' spans were left out
// (the user: "Dialog recording is also broken": in the 01:57 recording 143
// lines were KrugerSec's and the people's, and at its playback 142 of them
// couldn't be said live, their people gone from the level: silent)
bool util::hooks::ReplayVoiceLineSpans(std::vector<std::pair<double, double>>& out)
{
  out.clear();
  if (!oVoiceLineQueue)
    return false;
  LockReplaySounds();
  int lines = 0, others = 0;
  for (VoiceLine const& l : g_VoiceLines)
  {
    out.push_back({ l.Time, LineEnd(l) });
    ++(MissionDialog(l.Name) ? lines : others);
  }
  int cutscene = 0;
  for (ReplaySoundAsset const& a : g_ReplaySoundAssets)
    if (CutsceneVoiceOver(a))
    {
      out.push_back({ a.Time, a.Time + LineSeconds });
      ++cutscene;
    }
  UnlockReplaySounds();
  util::log::Write("Replay: the sound track's dialog: %d voice lines of a mission's narrative or its loudspeakers, %d other lines "
    "(KrugerSec's, the people's: heard from the track only when the replay can't say them live) and %d cutscene voice-over sounds",
    lines, others, cutscene);
  std::sort(out.begin(), out.end());
  return true;
}

// Game thread, while it plays: whether a recorded line the replay isn't
// saying live covers this time (the sound track's is heard then), any line
// (KrugerSec's and the people's too since 2026-11-04, see
// ReplayVoiceLineSpans). While the lines aren't played (Faith's sounds off),
// every line's span counts
bool util::hooks::ReplayDialogFromTrack(double time, double before, double after)
{
  LockReplaySounds();
  size_t count = g_VoiceLines.size();
  bool playing = g_VoiceLinePlay.size() == count;
  bool heard = false;
  for (size_t i = 0; i < count && !heard; ++i)
  {
    VoiceLine const& l = g_VoiceLines[i];
    if (time < l.Time - before || time > LineEnd(l) + after)
      continue;
    heard = !playing || g_VoiceLinePlay[i].Track;
  }
  for (ReplaySoundAsset const& a : g_ReplaySoundAssets)
    if (!heard && time >= a.Time - before && time <= a.Time + LineSeconds + after && CutsceneVoiceOver(a))
      heard = true;
  UnlockReplaySounds();
  return heard;
}

bool util::hooks::ReplayHasVoiceLines()
{
  LockReplaySounds();
  bool any = !g_VoiceLines.empty();
  UnlockReplaySounds();
  return any;
}

void util::hooks::ReplaySoundsStopRecording()
{
  if (g_ReplaySoundState.load() != ReplaySoundRecording)
    return;
  g_ReplaySoundState = ReplaySoundIdle;
  LockReplaySounds();
  size_t events = g_ReplaySoundEvents.size(), voices = g_ReplaySoundVoices.size();
  int cues = g_MagRopeCuesNotRecorded;
  // Noted from several threads: in time order for playing
  std::stable_sort(g_ReplaySoundAssets.begin(), g_ReplaySoundAssets.end(),
    [](ReplaySoundAsset const& a, ReplaySoundAsset const& b) { return a.Time < b.Time; });
  size_t assets = g_ReplaySoundAssets.size();
  UnlockReplaySounds();
  util::log::Write("Replay: recorded %zu of Faith's sound events on %zu of her voices (%d of the MagRope's cues, the HUD's, "
    "left out), and %zu sounds given to them (a cutscene's lines)", events, voices, cues, assets);
  // The voice lines said meanwhile (see hVoiceLineQueue)
  LockReplaySounds();
  std::vector<VoiceLine> lines = g_VoiceLines;
  int dropped = g_VoiceLinesDropped;
  UnlockReplaySounds();
  int started = 0, cleared = 0, withVoice = 0;
  for (VoiceLine const& l : lines)
  {
    started += l.Start >= 0.0;
    cleared += l.End >= 0.0;
    withVoice += l.Index >= 0;
  }
  if (oVoiceLineQueue)
    util::log::Write("Replay: recorded %zu voice lines (missions' dialog, the people's; %d started and %d done while it "
      "recorded, %d with their voice among the recorded ones; %d more left out: no room)", lines.size(), started, cleared,
      withVoice, dropped);
  // Diagnostic (temporary): the first of them
  if (!util::DiagnosticsOn())
    return;
  for (size_t i = 0; i < lines.size() && i < 30; ++i)
  {
    VoiceLine const& l = lines[i];
    util::log::Write("Replay (diagnostic):   %.2f s: %s (voice 0x%I64X, recorded voice %d, node 0x%I64X, id 0x%I64X, slot %d), wave "
      "0x%I64X, started at %.2f s, done at %.2f s", l.Time, l.Name, l.Voice, l.Index, l.Node, l.Id, l.Slot, l.Wave, l.Start, l.End);
  }
}

namespace
{
  const __int64 VoiceManagerRva = 0x23E29B8;   // [0x1423E29B8]
  const int VoiceReleaseSlot = 9;

  __int64 MakeReplayVoice(__int64 asset)
  {
    __try
    {
      __int64 manager = *(__int64 const*)((__int64)g_gameHandle + VoiceManagerRva);
      return manager > 0x10000 && oSoundCreate ? oSoundCreate(manager, asset, 0, 0) : 0;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      return 0;
    }
  }

  // A sound given to a voice through the voice's slot 2 (past the hook; see
  // ReplaySoundAsset)
  bool GiveVoiceAsset(__int64 voice, unsigned int id, __int64 asset)
  {
    __try
    {
      return oVoiceSetAsset(voice, id, asset) != 0;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      return false;
    }
  }

  void ReleaseReplayVoice(__int64 voice)
  {
    __try
    {
      typedef void(__fastcall* tRelease)(__int64);
      tRelease release = *(tRelease const*)(*(__int64 const*)voice + VoiceReleaseSlot * 8);
      release(voice);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
  }

  // The playback's start: a voice of the replay's own for each recorded one
  // that isn't there (game thread; outside the lock, the game's voice code
  // calls the hooks)
  int MakeMissingReplayVoices()
  {
    struct Want { int Index; __int64 Asset; };
    static Want wants[MaxReplaySoundVoices];   // game thread only
    int wantCount = 0;
    LockReplaySounds();
    // After a change of level since the recording, only sounds the game has
    // used since (see g_SoundGen)
    bool stale = g_RecordGen != g_SoundGen;
    unsigned int gen = g_SoundGen;
    size_t count = (std::min)(g_ReplaySoundVoices.size(), (size_t)MaxReplaySoundVoices);
    for (size_t i = 0; i < count; ++i)
    {
      ReplaySoundVoice const& rv = g_ReplaySoundVoices[i];
      bool there = RecordedSoundThere(rv, stale);
      for (int k = 0; k < g_FaithSoundEntityCount && !there; ++k)
      {
        FaithSoundEntity const& e = g_FaithSoundEntities[k];
        there = !strcmp(e.Name, rv.Name) && NotedSoundThere(e, stale);
      }
      if (!there && !g_ReplayOwnVoices[i] && rv.AssetVTable && ReadAssetVTable(rv.Asset) == rv.AssetVTable &&
          (!stale || SeenData(rv.Asset)))
        wants[wantCount++] = { (int)i, rv.Asset };
    }
    UnlockReplaySounds();
    int made = 0;
    for (int w = 0; w < wantCount; ++w)
    {
      __int64 voice = MakeReplayVoice(wants[w].Asset);
      if (!voice)
        continue;
      g_ReplayOwnVoices[wants[w].Index] = voice;
      g_ReplayOwnGens[wants[w].Index] = gen;
      ++made;
    }
    return made;
  }
}

void util::hooks::ReplaySoundsPlay(double time, bool jumped)
{
  if (!oVoiceEvent || !oVoiceSetTransform)
    return;
  int made = g_ReplaySoundState.load() != ReplaySoundPlaying ? MakeMissingReplayVoices() : 0;
  struct Fire { __int64 Voice; unsigned int Event; };
  Fire fires[MaxReplaySoundFires];
  int fireCount = 0;
  // Game thread only: kept off the stack (64 KB)
  static __int64 poseVoices[MaxReplaySoundVoices];
  alignas(16) static float poses[MaxReplaySoundVoices][16];
  int poseCount = 0;
  bool first = g_ReplaySoundState.load() != ReplaySoundPlaying;
  int found = 0;
  if (first)
    g_MagRopeCuesNotPlayed = 0;

  LockReplaySounds();
  // A change of level since the recording (see g_SoundGen)
  bool stale = g_RecordGen != g_SoundGen;
  char const* staleReason = g_SoundGenReason;
  size_t count = (std::min)(g_ReplaySoundVoices.size(), (size_t)MaxReplaySoundVoices);
  // The voice each recorded one is now
  for (size_t i = 0; i < (size_t)MaxReplaySoundVoices; ++i)
  {
    __int64 now = 0;
    if (i < count)
    {
      ReplaySoundVoice const& rv = g_ReplaySoundVoices[i];
      if (RecordedSoundThere(rv, stale))
        now = rv.Voice;
      for (int k = 0; k < g_FaithSoundEntityCount && !now; ++k)
      {
        FaithSoundEntity const& e = g_FaithSoundEntities[k];
        if (!strcmp(e.Name, rv.Name) && NotedSoundThere(e, stale))
          now = e.Voice;
      }
      // Else the replay's own (see MakeMissingReplayVoices), unless the level
      // changed since it was made
      if (!now && (!stale || g_ReplayOwnGens[i] == g_SoundGen))
        now = g_ReplayOwnVoices[i];
    }
    g_ReplayPlayVoices[i] = now;
    g_ReplayPlayHasPose[i] = false;
    found += now ? 1 : 0;
  }
  // Where they were at this time: in a straight line between two recorded
  // frames, turned as in the nearer one
  size_t frames = g_ReplaySoundFrameTimes.size();
  if (frames)
  {
    size_t next = std::upper_bound(g_ReplaySoundFrameTimes.begin(), g_ReplaySoundFrameTimes.end(), time) -
      g_ReplaySoundFrameTimes.begin();
    size_t prev = next == 0 ? 0 : next - 1;
    if (next >= frames)
      next = frames - 1;
    double span = g_ReplaySoundFrameTimes[next] - g_ReplaySoundFrameTimes[prev];
    float f = span > 0 ? (float)((time - g_ReplaySoundFrameTimes[prev]) / span) : 0.f;
    if (f < 0.f) f = 0.f;
    if (f > 1.f) f = 1.f;
    auto frameEnd = [&](size_t frame) {
      return frame + 1 < frames ? (size_t)g_ReplaySoundFrameStarts[frame + 1] : g_ReplaySoundPoses.size();
    };
    for (size_t p = g_ReplaySoundFrameStarts[prev]; p < frameEnd(prev); ++p)
    {
      ReplaySoundPose const& a = g_ReplaySoundPoses[p];
      ReplaySoundPose const* b = &a;
      for (size_t q = g_ReplaySoundFrameStarts[next]; q < frameEnd(next); ++q)
        if (g_ReplaySoundPoses[q].Voice == a.Voice)
          b = &g_ReplaySoundPoses[q];
      float* out = g_ReplayPlayPoses[a.Voice];
      memcpy(out, f < 0.5f ? a.Transform : b->Transform, sizeof(a.Transform));
      for (int k = 12; k < 15; ++k)
        out[k] = a.Transform[k] + (b->Transform[k] - a.Transform[k]) * f;
      g_ReplayPlayHasPose[a.Voice] = true;
    }
  }
  for (int i = 0; i < MaxReplaySoundVoices; ++i)
  {
    if (!g_ReplayPlayVoices[i] || !g_ReplayPlayHasPose[i])
      continue;
    poseVoices[poseCount] = g_ReplayPlayVoices[i];
    memcpy(poses[poseCount], g_ReplayPlayPoses[i], sizeof(poses[0]));
    ++poseCount;
  }
  // The events up to this time (from here after a jump or at the start)
  if (first || jumped)
    g_ReplayPlayNext = std::lower_bound(g_ReplaySoundEvents.begin(), g_ReplaySoundEvents.end(), time,
      [](ReplaySoundEvent const& e, double t) { return e.Time < t; }) - g_ReplaySoundEvents.begin();
  if (first && time <= 0.0)
    g_ReplayPlayNext = 0;
  // Past the most one frame gives, the rest wait for the next (they were
  // dropped before)
  while (g_ReplayPlayNext < g_ReplaySoundEvents.size() && g_ReplaySoundEvents[g_ReplayPlayNext].Time <= time &&
    fireCount < MaxReplaySoundFires)
  {
    ReplaySoundEvent const& e = g_ReplaySoundEvents[g_ReplayPlayNext++];
    __int64 voice = e.Voice < MaxReplaySoundVoices ? g_ReplayPlayVoices[e.Voice] : 0;
    // The MagRope's cues in a recording made before they were left out
    if (voice && IsMagRopeCue(g_ReplaySoundVoices[e.Voice].Name, e.Event))
    {
      ++g_MagRopeCuesNotPlayed;
      continue;
    }
    if (voice)
    {
      fires[fireCount++] = { voice, e.Event };
      if (util::DiagnosticsOn())
        NoteHeardReplay(e.Voice, voice, e.Event);
    }
  }
  // The sounds given to voices up to this time (a cutscene's lines, see
  // ReplaySoundAsset): from a jump or the start, the last one each voice got
  // before; then each as its time comes, given before the events
  struct Give { __int64 Voice; ReplaySoundAsset const* Asset; };
  static Give gives[MaxReplaySoundFires];   // game thread only
  int giveCount = 0;
  if (first || jumped)
  {
    g_ReplayPlayAssetNext = std::upper_bound(g_ReplaySoundAssets.begin(), g_ReplaySoundAssets.end(), time,
      [](double t, ReplaySoundAsset const& a) { return t < a.Time; }) - g_ReplaySoundAssets.begin();
    if (first && time <= 0.0)
      g_ReplayPlayAssetNext = 0;
    for (size_t i = g_ReplayPlayAssetNext; i-- > 0 && giveCount < MaxReplaySoundFires;)
    {
      ReplaySoundAsset const& a = g_ReplaySoundAssets[i];
      bool later = false;
      for (size_t k = i + 1; k < g_ReplayPlayAssetNext && !later; ++k)
        later = g_ReplaySoundAssets[k].Voice == a.Voice && g_ReplaySoundAssets[k].Id == a.Id;
      __int64 voice = a.Voice < MaxReplaySoundVoices ? g_ReplayPlayVoices[a.Voice] : 0;
      if (!later && voice && stale && !SeenData(a.Asset))
        ++g_ReplaySoundAssetsLeftOut;
      else if (!later && voice)
        gives[giveCount++] = { voice, &a };
    }
  }
  while (g_ReplayPlayAssetNext < g_ReplaySoundAssets.size() && g_ReplaySoundAssets[g_ReplayPlayAssetNext].Time <= time &&
    giveCount < MaxReplaySoundFires)
  {
    ReplaySoundAsset const& a = g_ReplaySoundAssets[g_ReplayPlayAssetNext++];
    __int64 voice = a.Voice < MaxReplaySoundVoices ? g_ReplayPlayVoices[a.Voice] : 0;
    // After a change of level, only a sound the game has given since
    if (voice && stale && !SeenData(a.Asset))
      ++g_ReplaySoundAssetsLeftOut;
    else if (voice)
      gives[giveCount++] = { voice, &a };
  }
  size_t events = g_ReplaySoundEvents.size();
  g_ReplaySoundState = ReplaySoundPlaying;
  UnlockReplaySounds();

  // Outside the lock: the game's code calls the hooks again
  for (int i = 0; i < poseCount; ++i)
    oVoiceSetTransform(poseVoices[i], poses[i]);
  for (int i = 0; i < giveCount; ++i)
  {
    // Only while it's still that sound (the recording's vectors aren't
    // changed while it plays)
    ReplaySoundAsset const& a = *gives[i].Asset;
    char name[96];
    ReadAssetName(a.Asset, name, sizeof(name));
    if (oVoiceSetAsset && ReadAssetVTable(a.Asset) == a.AssetVTable && !strcmp(name, a.Name) &&
        GiveVoiceAsset(gives[i].Voice, a.Id, a.Asset))
      ++g_ReplaySoundAssetsGiven;
    else
      ++g_ReplaySoundAssetsLeftOut;
  }
  for (int i = 0; i < fireCount; ++i)
    oVoiceEvent(fires[i].Voice, fires[i].Event);
  // The voice lines (missions' dialog, the people's) at their times
  PlayVoiceLines(time, first || jumped, stale);
  if (first)
  {
    if (stale && count)
      util::log::Write("Replay: since it was recorded, %s: only the sounds the game has used since play (the rest may be "
        "unloaded)", staleReason);
    if (count && found < (int)count)
      util::log::Warning("Replay: %d of the %zu voices of the recorded sounds are there (%d of them made by the replay), the rest "
        "stay silent", found, count, made);
    else if (count)
      util::log::Write("Replay: playing the recorded sounds (%zu events on %zu voices, %d of them made by the replay)", events, count,
        made);
  }
}

void util::hooks::ReplaySoundsStop()
{
  if (g_ReplaySoundState.load() != ReplaySoundPlaying)
    return;
  static __int64 voices[MaxReplaySoundVoices];   // game thread only
  alignas(16) static float transforms[MaxReplaySoundVoices][16];
  int count = 0;
  LockReplaySounds();
  g_ReplaySoundState = ReplaySoundIdle;
  // Back where the game last put them
  for (int i = 0; i < MaxReplaySoundVoices; ++i)
  {
    __int64 voice = g_ReplayPlayVoices[i];
    g_ReplayPlayVoices[i] = 0;
    g_ReplayPlayHasPose[i] = false;
    if (!voice)
      continue;
    for (int k = 0; k < g_FaithSoundEntityCount; ++k)
    {
      FaithSoundEntity const& e = g_FaithSoundEntities[k];
      if (e.Voice == voice && e.HasTransform && IsSoundEntityLive(e.Entity, e.Voice, e.Asset))
      {
        voices[count] = voice;
        memcpy(transforms[count], e.Transform, sizeof(transforms[0]));
        ++count;
        break;
      }
    }
  }
  UnlockReplaySounds();
  for (int i = 0; i < count; ++i)
    oVoiceSetTransform(voices[i], transforms[i]);
  // The voice lines it queued cleared before their voices go
  StopVoiceLines();
  if (g_ReplaySoundAssetsGiven || g_ReplaySoundAssetsLeftOut)
    util::log::Write("Replay: %d sounds given to the replay's voices again (a cutscene's lines; %d left out: not loaded any "
      "more)", g_ReplaySoundAssetsGiven, g_ReplaySoundAssetsLeftOut);
  g_ReplaySoundAssetsGiven = g_ReplaySoundAssetsLeftOut = 0;
  if (g_VoiceLinesPlayed || g_VoiceLinesLeftOut)
    util::log::Write("Replay: %d voice lines said again (%d left out: their voice wasn't made, the game hadn't started them, "
      "or their dialog isn't loaded any more)", g_VoiceLinesPlayed, g_VoiceLinesLeftOut);
  g_VoiceLinesPlayed = g_VoiceLinesLeftOut = 0;
  // The replay's own voices let go (game thread, outside the lock)
  int released = 0;
  for (int i = 0; i < MaxReplaySoundVoices; ++i)
    if (g_ReplayOwnVoices[i])
    {
      ReleaseReplayVoice(g_ReplayOwnVoices[i]);
      g_ReplayOwnVoices[i] = 0;
      ++released;
    }
  if (released)
    util::log::Write("Replay: the %d voices it made for the recorded sounds are let go", released);
  if (g_MagRopeCuesNotPlayed)
    util::log::Write("Replay: %d of the MagRope's cues (the HUD's) in the recording left out", g_MagRopeCuesNotPlayed);
  g_MagRopeCuesNotPlayed = 0;
}

namespace
{
  // Downed people hitting the ground as ragdolls (2026-10-26, the user:
  // "downed ksec should leave a body ripple when they enter ragdoll physics
  // and hit the ground"): the game plays RagdollSoundName for each knock of
  // an AI's ragdoll (the sound log, 2026-10-26 20:47:13: twice, 2.2 m from
  // Faith, its voice started with 0x0DC0EFA5 and placed once). Its voices are
  // known as they're made (by their sound's name), placed and started: where
  // it's placed goes to the wet look (wetlook::NoteRagdollKnock), whichever
  // of the two comes first. MostRagdollVoices kept.
  const char RagdollSoundName[] = "ME_Sound/Character/AI/AI_Ragdoll_CollisionVelocityBased";
  const unsigned int VoiceStartEvent = 0x0DC0EFA5;
  const int MostRagdollVoices = 16;
  struct RagdollVoice { __int64 Voice; float At[3]; bool Placed, Started; };
  RagdollVoice g_RagdollVoices[MostRagdollVoices] = {};   // under g_RagdollMutex
  int g_RagdollVoiceNext = 0;
  std::atomic<int> g_RagdollVoicesKept{ 0 };
  std::mutex g_RagdollMutex;

  void NoteRagdollVoice(__int64 pVoice, __int64 pAsset)
  {
    char name[96];
    ReadSoundLogName(pAsset, name, sizeof(name));
    bool ragdoll = !strcmp(name, RagdollSoundName);
    if (!ragdoll && g_RagdollVoicesKept.load(std::memory_order_relaxed) == 0)
      return;
    std::lock_guard<std::mutex> lock(g_RagdollMutex);
    // (a voice's memory can be reused for another sound)
    for (RagdollVoice& r : g_RagdollVoices)
      if (r.Voice == pVoice)
      {
        r.Voice = 0;
        --g_RagdollVoicesKept;
      }
    if (!ragdoll)
      return;
    RagdollVoice& r = g_RagdollVoices[g_RagdollVoiceNext];
    g_RagdollVoiceNext = (g_RagdollVoiceNext + 1) % MostRagdollVoices;
    if (!r.Voice)
      ++g_RagdollVoicesKept;
    r = RagdollVoice{ pVoice, {}, false, false };
  }

  // A ragdoll voice placed (place) or started (place null): its knock once
  // it's both
  void RagdollVoiceSeen(__int64 pVoice, float const* place)
  {
    if (g_RagdollVoicesKept.load(std::memory_order_relaxed) == 0)
      return;
    float at[3];
    bool knock = false;
    {
      std::lock_guard<std::mutex> lock(g_RagdollMutex);
      for (RagdollVoice& r : g_RagdollVoices)
      {
        if (r.Voice != pVoice)
          continue;
        if (place)
        {
          memcpy(r.At, place, sizeof(r.At));
          r.Placed = true;
        }
        else
          r.Started = true;
        if (r.Placed && r.Started)
        {
          memcpy(at, r.At, sizeof(at));
          knock = std::isfinite(at[0] + at[1] + at[2]);
          r.Voice = 0;
          --g_RagdollVoicesKept;
        }
        break;
      }
    }
    if (knock)
      wetlook::NoteRagdollKnock(at);
  }
}

__int64 __fastcall hSoundCreate(__int64 pSystem, __int64 pAsset, __int64 a3, __int64 a4)
{
  __int64 pVoice = oSoundCreate(pSystem, pAsset, a3, a4);
  if (pVoice && !g_shutdown)
  {
    NotePlayerVoice(pVoice, pAsset);
    NoteHeardVoice(pVoice, pAsset);
    NoteVoiceOverVoice(pVoice, pAsset);
    // The game used that sound: there in this generation (see g_SoundGen)
    NoteSoundDataSeen(pAsset);
    // VISUALS > Weather: the game's rain sound, to play at the camera
    weather::NoteSoundCreated(pVoice, pAsset);
    // and the downed bodies' knocks (see RagdollSoundName)
    NoteRagdollVoice(pVoice, pAsset);
  }
  if (pVoice && g_SoundLogOn.load(std::memory_order_relaxed))
  {
    char name[96];
    ReadSoundLogName(pAsset, name, sizeof(name));
    void* stack[SoundLogFrames];
    int frames = RtlCaptureStackBackTrace(1, SoundLogFrames, stack, nullptr);
    LockSoundLog();
    // By sound and caller
    int made = -1;
    for (int i = 0; i < g_SoundLogMadeKindCount && made < 0; ++i)
    {
      SoundLogKind const& k = g_SoundLogMadeKinds[i];
      if (k.Frames == frames && !strcmp(k.Name, name) && !memcmp(k.Stack, stack, sizeof(void*) * frames))
        made = i;
    }
    if (made < 0 && g_SoundLogMadeKindCount < SoundLogEntityKinds)
    {
      made = g_SoundLogMadeKindCount++;
      SoundLogKind& k = g_SoundLogMadeKinds[made];
      k = SoundLogKind{};
      strncpy_s(k.Name, name, _TRUNCATE);
      k.Frames = frames;
      memcpy(k.Stack, stack, sizeof(void*) * frames);
      k.MinDistance = 1e9f;
      k.MinCamera = 1e9f;
    }
    if (made >= 0)
      ++g_SoundLogMadeKinds[made].Count;
    if (SoundLogVoice* v = FindSoundLogVoice(pVoice, true))
    {
      // A voice's memory can be reused for another one
      memcpy(v->Name, name, sizeof(name));
      v->Placed = 0;
      v->Positional = false;
      v->MadeKind = made + 1;
    }
    ++g_SoundLogVoicesMade;
    UnlockSoundLog();
  }
  return pVoice;
}

// A sound given to a voice (see ReplaySoundAsset): kept while the replay
// records, for its recorded voices
char __fastcall hVoiceSetAsset(__int64 pVoice, unsigned int id, __int64 asset)
{
  char given = oVoiceSetAsset(pVoice, id, asset);
  // The game used that sound: there in this generation (see g_SoundGen)
  if (given)
    NoteSoundDataSeen(asset);
  if (!given || asset < 0x10000 || g_shutdown || g_ReplaySoundState.load(std::memory_order_relaxed) != ReplaySoundRecording)
    return given;
  ReplaySoundAsset a{};
  a.Id = id;
  a.Asset = asset;
  a.AssetVTable = ReadAssetVTable(asset);
  ReadAssetName(asset, a.Name, sizeof(a.Name));
  if (!a.AssetVTable || !strcmp(a.Name, "?") || !a.Name[0])
    return given;
  LockReplaySounds();
  for (int i = 0; i < g_FaithSoundEntityCount; ++i)
  {
    if (g_FaithSoundEntities[i].Voice != pVoice)
      continue;
    int index = ReplaySoundVoiceIndex(g_FaithSoundEntities[i]);
    if (index >= 0 && g_ReplaySoundAssets.size() < 16384)
    {
      a.Time = g_ReplaySoundClock;
      a.Voice = index;
      g_ReplaySoundAssets.push_back(a);
    }
    break;
  }
  UnlockReplaySounds();
  return given;
}

static bool IsVoiceSetAssetCode(BYTE const* pBase)
{
  // The voice's slot 2: push rbx; sub rsp, 0x20; mov rbx, rcx; mov rcx, [rcx + 0x18]; test rcx, rcx; je; call 0x143006180
  static const BYTE code[] = { 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x89, 0xCB, 0x48, 0x8B, 0x49, 0x18, 0x48, 0x85, 0xC9, 0x74, 0x20,
    0xE8 };
  // In a sound track keyframe's trigger 0x14301C4E0: mov edx, [r9]; ...; mov r8, [rcx + 0x18]; ...; call [rax + 0x10]
  static const BYTE trigger[] = { 0x48, 0x8B, 0x02, 0x48, 0x89, 0xD7, 0x41, 0x8B, 0x11, 0x4C, 0x89, 0xC6, 0x4C, 0x8B, 0x41, 0x18, 0x48,
    0x89, 0xCB, 0x48, 0x89, 0xF9, 0xFF, 0x50, 0x10 };
  const __int64 VoiceSetAssetRva = 0x301DA80;
  return !memcmp(pBase + VoiceSetAssetRva, code, sizeof(code)) &&
    VoiceSetAssetRva + 0x16 + *(int const*)(pBase + VoiceSetAssetRva + 0x12) == 0x3006180 &&
    *(__int64 const*)(pBase + 0x1ACFAD0 + 2 * 8) == (__int64)pBase + VoiceSetAssetRva &&
    !memcmp(pBase + 0x301C4EF, trigger, sizeof(trigger));
}

void __fastcall hVoiceSetTransform(__int64 pVoice, float const* pTransform)
{
  alignas(16) float held[16];
  if (pTransform && !g_shutdown && ReplaySoundOnTransform(pVoice, pTransform, held))
    pTransform = held;
  oVoiceSetTransform(pVoice, pTransform);
  if (pTransform && !g_shutdown)
    RagdollVoiceSeen(pVoice, pTransform + 12);
  if (pTransform && g_SoundLogOn.load(std::memory_order_relaxed))
  {
    bool positional = IsVoicePositional(pVoice);
    LockSoundLog();
    if (SoundLogVoice* v = FindSoundLogVoice(pVoice, true))
    {
      v->Position[0] = pTransform[12];
      v->Position[1] = pTransform[13];
      v->Position[2] = pTransform[14];
      // Where a voice made during the log was first put
      if (v->Placed == 0 && v->MadeKind > 0 && positional)
      {
        SoundLogKind& k = g_SoundLogMadeKinds[v->MadeKind - 1];
        k.Positional = true;
        if (g_SoundLogHasFaith)
        {
          float d = SoundLogDistance(v->Position, g_SoundLogFaith);
          if (d < k.MinDistance) k.MinDistance = d;
          if (d > k.MaxDistance) k.MaxDistance = d;
        }
        if (g_SoundLogHasCamera)
        {
          float d = SoundLogDistance(v->Position, g_SoundLogCamera);
          if (d < k.MinCamera) k.MinCamera = d;
          if (d > k.MaxCamera) k.MaxCamera = d;
        }
      }
      ++v->Placed;
      v->Positional = positional;
    }
    UnlockSoundLog();
  }
}

__int64 __fastcall hSoundEntityEvent(__int64 pEntity, __int64 pEvent)
{
  if (g_SoundLogOn.load(std::memory_order_relaxed))
  {
    // The event itself, by sound, event and sender (also the ones the
    // entity handles without its voice)
    __int64 asset = 0, voice = 0;
    float position[3]{};
    unsigned int event = 0;
    if (ReadSoundEntityInfo(pEntity, pEvent, asset, voice, position, event))
    {
      void* stack[SoundLogFrames];
      int frames = RtlCaptureStackBackTrace(1, SoundLogFrames, stack, nullptr);
      char name[96];
      ReadSoundLogName(asset, name, sizeof(name));
      bool placed = position[0] != 0.f || position[1] != 0.f || position[2] != 0.f;
      LockSoundLog();
      SoundLogKind* kind = nullptr;
      for (int i = 0; i < g_SoundLogEntityKindCount && !kind; ++i)
      {
        SoundLogKind& k = g_SoundLogEntityKinds[i];
        if (k.Event == event && k.Frames == frames && !strcmp(k.Name, name) && !memcmp(k.Stack, stack, sizeof(void*) * frames))
          kind = &k;
      }
      if (!kind && g_SoundLogEntityKindCount < SoundLogEntityKinds)
      {
        kind = &g_SoundLogEntityKinds[g_SoundLogEntityKindCount++];
        *kind = SoundLogKind{};
        strncpy_s(kind->Name, name, _TRUNCATE);
        kind->Event = event;
        kind->Frames = frames;
        memcpy(kind->Stack, stack, sizeof(void*) * frames);
        kind->MinDistance = 1e9f;
        kind->MinCamera = 1e9f;
      }
      if (kind)
      {
        ++kind->Count;
        kind->Positional |= placed;
        if (placed && g_SoundLogHasFaith)
        {
          float d = SoundLogDistance(position, g_SoundLogFaith);
          if (d < kind->MinDistance) kind->MinDistance = d;
          if (d > kind->MaxDistance) kind->MaxDistance = d;
          if (d <= SoundLogNearFaith) ++kind->NearCount;
        }
        if (placed && g_SoundLogHasCamera)
        {
          float d = SoundLogDistance(position, g_SoundLogCamera);
          if (d < kind->MinCamera) kind->MinCamera = d;
          if (d > kind->MaxCamera) kind->MaxCamera = d;
        }
      }
      UnlockSoundLog();
    }
    __int64 pVoice = 0, pAsset = 0;
    __try
    {
      pVoice = *(__int64*)(pEntity + SoundEntityVoiceOffset);
      __int64 pData = *(__int64*)(pEntity + SoundEntityDataOffset);
      pAsset = pData ? *(__int64*)(pData + SoundEntityDataSoundOffset) : 0;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { pVoice = 0; }
    if (pVoice && pAsset)
    {
      char name[96];
      ReadSoundLogName(pAsset, name, sizeof(name));
      LockSoundLog();
      SoundLogVoice* v = FindSoundLogVoice(pVoice, true);
      if (v && !v->Name[0])
        memcpy(v->Name, name, sizeof(name));
      UnlockSoundLog();
    }
  }
  if (!g_shutdown)
    NoteSpotEvent(pEntity, pEvent);
  // Noted before (so this event is recorded) and after (its first event
  // makes its voice)
  if (!g_shutdown)
    NoteFaithSoundEntity(pEntity);
  __int64 result = oSoundEntityEvent(pEntity, pEvent);
  if (!g_shutdown)
    NoteFaithSoundEntity(pEntity);
  return result;
}

__int64 __fastcall hVoiceEvent(__int64 pVoice, unsigned int event)
{
  // VISUALS > Weather: the events the game starts its rain sound with
  weather::NoteVoiceEvent(pVoice, event);
  // and a downed body's knock starting (see RagdollSoundName)
  if (event == VoiceStartEvent && !g_shutdown)
    RagdollVoiceSeen(pVoice, nullptr);
  if (!g_shutdown && ReplaySoundOnEvent(pVoice, event))
    return 0;
  if (util::DiagnosticsOn() && !g_shutdown && g_ReplayPlaying.load(std::memory_order_relaxed))
    NoteHeardLive(pVoice, event);
  if (g_SoundLogOn.load(std::memory_order_relaxed))
  {
    void* stack[SoundLogFrames];
    int frames = RtlCaptureStackBackTrace(1, SoundLogFrames, stack, nullptr);
    LockSoundLog();
    ++g_SoundLogEvents;
    SoundLogVoice* v = FindSoundLogVoice(pVoice, false);
    const char* name = v && v->Name[0] ? v->Name : "(made before the log)";
    float distance = -1.f, cameraDistance = -1.f;
    if (v && v->Placed && v->Positional && g_SoundLogHasFaith)
    {
      float dx = v->Position[0] - g_SoundLogFaith[0], dy = v->Position[1] - g_SoundLogFaith[1], dz = v->Position[2] - g_SoundLogFaith[2];
      distance = sqrtf(dx * dx + dy * dy + dz * dz);
    }
    if (v && v->Placed && v->Positional && g_SoundLogHasCamera)
    {
      float dx = v->Position[0] - g_SoundLogCamera[0], dy = v->Position[1] - g_SoundLogCamera[1], dz = v->Position[2] - g_SoundLogCamera[2];
      cameraDistance = sqrtf(dx * dx + dy * dy + dz * dz);
    }
    SoundLogKind* kind = nullptr;
    for (int i = 0; i < g_SoundLogKindCount && !kind; ++i)
    {
      SoundLogKind& k = g_SoundLogKinds[i];
      if (k.Event == event && k.Frames == frames && !strcmp(k.Name, name) && !memcmp(k.Stack, stack, sizeof(void*) * frames))
        kind = &k;
    }
    if (!kind && g_SoundLogKindCount < SoundLogKinds)
    {
      kind = &g_SoundLogKinds[g_SoundLogKindCount++];
      *kind = SoundLogKind{};
      strncpy_s(kind->Name, name, _TRUNCATE);
      kind->Event = event;
      kind->Frames = frames;
      memcpy(kind->Stack, stack, sizeof(void*) * frames);
      kind->MinDistance = 1e9f;
      kind->MinCamera = 1e9f;
    }
    if (kind)
    {
      ++kind->Count;
      if (cameraDistance >= 0.f)
      {
        if (cameraDistance < kind->MinCamera) kind->MinCamera = cameraDistance;
        if (cameraDistance > kind->MaxCamera) kind->MaxCamera = cameraDistance;
      }
      if (v)
      {
        kind->Positional |= v->Positional;
        if (v->Placed > kind->MaxPlaced)
          kind->MaxPlaced = v->Placed;
      }
      if (distance >= 0.f)
      {
        if (distance < kind->MinDistance) kind->MinDistance = distance;
        if (distance > kind->MaxDistance) kind->MaxDistance = distance;
        if (distance <= SoundLogNearFaith) ++kind->NearCount;
      }
    }
    else
      ++g_SoundLogKindsDropped;
    UnlockSoundLog();
  }
  return oVoiceEvent(pVoice, event);
}

// The sound entity's constructor and destructor as they were read, each
// putting its table in place (lea rax, [rip -> 0x141AD0338])
static bool IsSoundEntityLifeCode(BYTE const* pBase)
{
  static const BYTE ctor[] = { 0x48, 0x89, 0xE0, 0x48, 0x89, 0x48, 0x08, 0x41, 0x56, 0x48, 0x83, 0xEC, 0x60, 0x48, 0xC7, 0x40 };
  static const BYTE dtor[] = { 0x48, 0x89, 0x4C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x30, 0x48, 0xC7, 0x44, 0x24, 0x20, 0xFE };
  auto leaTo = [&](__int64 rva, __int64 target) {
    BYTE const* p = pBase + rva;
    return p[0] == 0x48 && p[1] == 0x8D && p[2] == 0x05 && rva + 7 + *(int const*)(p + 3) == target;
  };
  return !memcmp(pBase + 0x30170C0, ctor, sizeof(ctor)) && !memcmp(pBase + 0x3017BA0, dtor, sizeof(dtor)) &&
    leaTo(0x30170FE, 0x1AD0338) && leaTo(0x3017BBB, 0x1AD0338);
}

// The three functions as they were read, and the voice's table using them
static bool IsSoundLogCode(BYTE const* pBase)
{
  static const BYTE create[] = { 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x81, 0xEC, 0xB0, 0x00 };
  static const BYTE transform[] = { 0x48, 0x8B, 0x41, 0x28, 0x48, 0x85, 0xC0, 0x74, 0x1E, 0x0F, 0x28, 0x02, 0x0F, 0x29, 0x00, 0x0F };
  static const BYTE event[] = { 0x89, 0x54, 0x24, 0x10, 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x89, 0xCB, 0x48, 0x8B, 0x49, 0x18,
    0x48, 0x85, 0xC9, 0x0F, 0x84, 0x3D, 0x01, 0x00 };
  static const BYTE entityEvent[] = { 0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x6C, 0x24, 0x18, 0x48, 0x89, 0x7C, 0x24, 0x20,
    0x41, 0x56, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x8B, 0xB9 };
  __int64 const* pTable = (__int64 const*)(pBase + VoiceTableRva);
  __int64 const* pEntityTable = (__int64 const*)(pBase + SoundEntityTableRva);
  return !memcmp(pBase + SoundCreateRva, create, sizeof(create)) &&
    !memcmp(pBase + SoundEntityEventRva, entityEvent, sizeof(entityEvent)) &&
    pEntityTable[4] == (__int64)(pBase + SoundEntityEventRva) &&
    !memcmp(pBase + VoiceTransformRva, transform, sizeof(transform)) &&
    !memcmp(pBase + VoiceEventRva, event, sizeof(event)) &&
    pTable[0] == (__int64)(pBase + VoiceEventRva) && pTable[13] == (__int64)(pBase + VoiceTransformRva);
}

bool util::hooks::StartFaithSoundLog(double seconds)
{
  if (!util::DiagnosticsOn() || !oSoundCreate || !oVoiceSetTransform || !oVoiceEvent || !oSoundEntityEvent)
    return false;
  LockSoundLog();
  memset(g_SoundLogVoices, 0, sizeof(g_SoundLogVoices));
  g_SoundLogKindCount = 0;
  g_SoundLogKindsDropped = 0;
  g_SoundLogEntityKindCount = 0;
  g_SoundLogMadeKindCount = 0;
  g_SoundLogVoicesMade = 0;
  g_SoundLogEvents = 0;
  g_SoundLogUntil = SoundLogNow() + seconds;
  g_SoundLogSeconds = seconds;
  UnlockSoundLog();
  g_SoundLogDone = false;
  g_SoundLogOn = true;
  util::log::Write("Sound log: recording the sounds and their events for %.0f s", seconds);
  return true;
}

double util::hooks::FaithSoundLogRemaining()
{
  if (!g_SoundLogOn)
    return 0.0;
  double left = g_SoundLogUntil - SoundLogNow();
  return left > 0 ? left : 0.0;
}

void util::hooks::UpdateFaithSoundLog(float const* faith, float const* camera)
{
  if (!g_SoundLogOn)
    return;
  bool ending = SoundLogNow() >= g_SoundLogUntil;
  LockSoundLog();
  g_SoundLogHasFaith = faith != nullptr;
  if (faith)
    memcpy(g_SoundLogFaith, faith, sizeof(g_SoundLogFaith));
  g_SoundLogHasCamera = camera != nullptr;
  if (camera)
    memcpy(g_SoundLogCamera, camera, sizeof(g_SoundLogCamera));
  UnlockSoundLog();
  if (!ending || g_SoundLogDone.exchange(true))
    return;
  g_SoundLogOn = false;

  // Copied out under the lock, written after it (only the camera hook gets here)
  static SoundLogKind kinds[SoundLogKinds];
  LockSoundLog();
  int count = g_SoundLogKindCount, dropped = g_SoundLogKindsDropped, made = g_SoundLogVoicesMade, events = g_SoundLogEvents;
  memcpy(kinds, g_SoundLogKinds, sizeof(SoundLogKind) * count);
  UnlockSoundLog();
  std::sort(kinds, kinds + count, [](SoundLogKind const& a, SoundLogKind const& b) { return a.Count > b.Count; });

  util::log::Write("Sound log: done, %.0f s: %d voices made, %d events in %d kinds (by sound, event and caller)%s",
    g_SoundLogSeconds, made, events, count, dropped ? ", more kinds than it keeps" : "");
  auto chain = [](SoundLogKind const& k, char* buffer, size_t size) {
    buffer[0] = 0;
    size_t used = 0;
    for (int f = 0; f < k.Frames && used + 24 < size; ++f)
      used += sprintf_s(buffer + used, size - used, "%s0x%I64X", f ? " < " : "", (__int64)k.Stack[f]);
  };
  char from[SoundLogFrames * 24];
  int nearShown = 0, farShown = 0, flatShown = 0;
  for (int pass = 0; pass < 3; ++pass)
  {
    for (int i = 0; i < count; ++i)
    {
      SoundLogKind const& k = kinds[i];
      bool isNear = k.NearCount > 0;
      bool isFlat = !k.Positional;
      if ((pass == 0 && !isNear) || (pass == 1 && (isNear || isFlat)) || (pass == 2 && (isNear || !isFlat)))
        continue;
      int& shown = pass == 0 ? nearShown : pass == 1 ? farShown : flatShown;
      if (shown >= (pass == 0 ? 80 : 25))
        continue;
      ++shown;
      chain(k, from, sizeof(from));
      float camMin = k.MinCamera < 1e8f ? k.MinCamera : -1.f;
      if (pass == 0)
        util::log::Write("Sound log: near Faith: %s, event 0x%08X x%d (%d within %.0f m, %.1f - %.1f m from her, %.0f - %.0f m from "
          "the camera), placed up to %d times (from %s)", k.Name, k.Event, k.Count, k.NearCount, SoundLogNearFaith, k.MinDistance,
          k.MaxDistance, camMin, k.MaxCamera, k.MaxPlaced, from);
      else if (pass == 1)
        util::log::Write("Sound log: elsewhere: %s, event 0x%08X x%d (%.0f - %.0f m from her, %.0f - %.0f m from the camera) (from %s)",
          k.Name, k.Event, k.Count, k.MinDistance < 1e8f ? k.MinDistance : -1.f, k.MaxDistance, camMin, k.MaxCamera, from);
      else
        util::log::Write("Sound log: not positional: %s, event 0x%08X x%d (from %s)", k.Name, k.Event, k.Count, from);
    }
  }

  // The sound entities' own events: music, voices, crowds, traffic
  static SoundLogKind entityKinds[SoundLogEntityKinds];
  LockSoundLog();
  int entityCount = g_SoundLogEntityKindCount;
  memcpy(entityKinds, g_SoundLogEntityKinds, sizeof(SoundLogKind) * entityCount);
  UnlockSoundLog();
  std::sort(entityKinds, entityKinds + entityCount, [](SoundLogKind const& a, SoundLogKind const& b) { return a.Count > b.Count; });
  auto interesting = [](const char* name) {
    return strstr(name, "Music") || strstr(name, "/VO/") || strstr(name, "Crowd") || strstr(name, "Traffic") ||
      strstr(name, "Pedestrian") || strstr(name, "KrugerSec") || strstr(name, "Ksec");
  };
  int shownEntities = 0;
  for (int i = 0; i < entityCount && shownEntities < 80; ++i)
  {
    SoundLogKind const& k = entityKinds[i];
    if (!interesting(k.Name))
      continue;
    ++shownEntities;
    chain(k, from, sizeof(from));
    util::log::Write("Sound log: entity event: %s, event 0x%08X x%d (%.0f - %.0f m from Faith, %.0f - %.0f m from the camera) (from %s)",
      k.Name, k.Event, k.Count, k.MinDistance < 1e8f ? k.MinDistance : -1.f, k.MaxDistance,
      k.MinCamera < 1e8f ? k.MinCamera : -1.f, k.MaxCamera, from);
  }

  // The voices made that sound like voice lines, by sound and caller
  static SoundLogKind madeKinds[SoundLogEntityKinds];
  LockSoundLog();
  int madeCount = g_SoundLogMadeKindCount;
  memcpy(madeKinds, g_SoundLogMadeKinds, sizeof(SoundLogKind) * madeCount);
  UnlockSoundLog();
  std::sort(madeKinds, madeKinds + madeCount, [](SoundLogKind const& a, SoundLogKind const& b) { return a.Count > b.Count; });
  int shownMade = 0, otherMade = 0;
  for (int i = 0; i < madeCount; ++i)
  {
    SoundLogKind const& k = madeKinds[i];
    bool line = strstr(k.Name, "VO") || strstr(k.Name, "Vo/") || strstr(k.Name, "Voice") || strstr(k.Name, "Crowd") ||
      strstr(k.Name, "Civilian") || strstr(k.Name, "Pedestrian") || strstr(k.Name, "Kruger");
    if (!line || shownMade >= 60)
    {
      ++otherMade;
      continue;
    }
    ++shownMade;
    chain(k, from, sizeof(from));
    util::log::Write("Sound log: made: %s x%d (%s, first put %.0f - %.0f m from Faith, %.0f - %.0f m from the camera) (from %s)",
      k.Name, k.Count, k.Positional ? "positional" : "not positional", k.MinDistance < 1e8f ? k.MinDistance : -1.f, k.MaxDistance,
      k.MinCamera < 1e8f ? k.MinCamera : -1.f, k.MaxCamera, from);
  }
  util::log::Write("Sound log: %d other kinds of voices made", otherMade);

  // Every music sound entity there is now, whether it has been started or not
  const int MaxListed = 48;
  __int64 entities[MaxListed];
  int listed = 0, alive = 0;
  LockSoundLog();
  alive = g_SoundLogEntityCount;
  for (int i = 0; i < SoundLogEntitySlots && listed < MaxListed; ++i)
  {
    __int64 e = g_SoundLogEntities[i];
    if (!e || e == SoundLogEntityGone)
      continue;
    __int64 asset = 0, voice = 0;
    float position[3];
    unsigned int unused = 0;
    char name[96];
    if (!ReadSoundEntityInfo(e, 0, asset, voice, position, unused))
      continue;
    ReadSoundLogName(asset, name, sizeof(name));
    if (strstr(name, "Music") || strstr(name, "Club"))
      entities[listed++] = e;
  }
  UnlockSoundLog();
  util::log::Write("Sound log: %d sound entities are alive, the music ones:", alive);
  for (int i = 0; i < listed; ++i)
  {
    __int64 asset = 0, voice = 0;
    float position[3];
    unsigned int unused = 0;
    char name[96];
    if (!ReadSoundEntityInfo(entities[i], 0, asset, voice, position, unused))
      continue;
    ReadSoundLogName(asset, name, sizeof(name));
    util::log::Write("Sound log: music entity 0x%I64X: %s at %.1f %.1f %.1f, voice 0x%I64X (%.0f m from Faith, %.0f m from the camera)",
      entities[i], name, position[0], position[1], position[2], voice,
      g_SoundLogHasFaith ? SoundLogDistance(position, g_SoundLogFaith) : -1.f,
      g_SoundLogHasCamera ? SoundLogDistance(position, g_SoundLogCamera) : -1.f);
  }
}

// OFFSET_MESHGATHER: a delayed job (ClientStaticModelGroupEntity vtable slot
// 98, 0x14318D540 -> 0x143191800) searches a spatial index of the world
// (0x143179BC0, which nothing else calls) for composite and rigid meshes,
// static models, physics entities and terrain around a point, then gathers the
// mesh of each one it found with this: (job, record, out, value), the record
// { the entity's interface, its class id at +8 }. With the camera's zones
// streaming in and out, the index still had entities whose zone had unloaded,
// and the game crashed on them three times: on a rigid mesh whose mesh was
// gone (+0x3763764, twice) and on a static model whose memory was released
// (+0x3296FE4). Such an entity is skipped the way the game skips one it can't
// use (false).
//
// The function has a branch for each kind of entity, with the class id range
// it tests and how far the entity starts before its interface. Both are read
// from its own code (two movzx eax, word ptr [rip + x], then lea r, [rcx - n]),
// so the guard is only on when the code is what was seen. An entity is taken
// as gone when its memory can't be read, its vtable or the vtable's first
// function isn't in the game's image, its type (vtable[0] is GetType:
// lea rax, [rip + TypeInfo]; ret, the TypeInfo's class id at +0x28) is outside
// the branch's range, or what the branch reads next is missing. A type the
// guard can't read that way isn't held against the entity (every class in
// these ranges has such a GetType but ClientPhysicsEntityWithPoseProvider).
// All of it is read in the tools under __try, so a bad pointer faults here and
// not in the game.
// Faults that still happen in the function itself or in the getter it calls
// (neither takes a lock) are caught too.
tMeshGather oMeshGather = nullptr;

namespace
{
  enum GatherKind { Gather_Composite, Gather_Rigid, Gather_Component, Gather_Terrain, Gather_Kinds };

  struct GatherBranch
  {
    int FirstIdAt;  // movzx eax, word ptr [rip + x]: the first class id of the range
    int LastIdAt;   // the same for the last
    int LeaAt;      // lea r, [rcx - n]: from the interface back to the entity
    const char* Name;
    unsigned short const* pFirst;
    unsigned short const* pLast;
    int Adjust;
  };
  // In the order the game tests them (terrain's range is inside the third)
  GatherBranch g_GatherBranches[Gather_Kinds] = {
    { 0x47, 0x61, 0x7E, "composite mesh" },
    { 0x453, 0x460, 0x47D, "rigid mesh" },
    { 0x6CD, 0x6DA, 0x6F7, "static model or physics" },
    { 0x893, 0x8A0, 0x8BD, "terrain" },
  };
  const int GatherFunctionSize = 0xA39;  // to its end (.pdata, its three parts)
  const int GatherGetterCallAt = 0x8E;   // call to the getter that crashed (+0x3763760)
  const int GatherGetterSize = 0xC;
  const int GatherVirtualOffset = 0x1D0; // what the third branch calls on the entity

  __int64 g_GameBegin = 0, g_GameEnd = 0;
  __int64 g_GatherBegin = 0, g_GetterBegin = 0;
  volatile LONG g_GatherSkips = 0;
  volatile LONG g_GatherFaults = 0;

  bool InGame(__int64 address)
  {
    return address >= g_GameBegin && address < g_GameEnd;
  }

  // What a "movzx eax, word ptr [rip + x]" at pCode reads, or nullptr if it isn't one
  unsigned short const* RipRelativeWord(BYTE const* pCode)
  {
    if (pCode[0] != 0x0F || pCode[1] != 0xB7 || pCode[2] != 0x05)
      return nullptr;
    return (unsigned short const*)(pCode + 7 + *(int const*)(pCode + 3));
  }

  // n of a "lea r64, [rcx - n]" with an 8-bit displacement, 0 if it isn't one
  int LeaRcxMinus(BYTE const* pCode)
  {
    if (pCode[0] != 0x48 || pCode[1] != 0x8D || (pCode[2] & 0xC7) != 0x41)
      return 0;
    int displacement = (signed char)pCode[3];
    return displacement < 0 ? -displacement : 0;
  }

  bool SetUpMeshGatherGuard(__int64 pFunction)
  {
    MODULEINFO info;
    if (!GetModuleInformation(GetCurrentProcess(), g_gameHandle, &info, sizeof(MODULEINFO)))
      return false;
    g_GameBegin = (__int64)info.lpBaseOfDll;
    g_GameEnd = g_GameBegin + info.SizeOfImage;

    BYTE const* pCode = (BYTE const*)pFunction;
    for (GatherBranch& branch : g_GatherBranches)
    {
      branch.pFirst = RipRelativeWord(pCode + branch.FirstIdAt);
      branch.pLast = RipRelativeWord(pCode + branch.LastIdAt);
      branch.Adjust = LeaRcxMinus(pCode + branch.LeaAt);
      if (!branch.pFirst || !branch.pLast || !branch.Adjust || !InGame((__int64)branch.pFirst))
        return false;
    }
    BYTE const* pCall = pCode + GatherGetterCallAt;
    if (pCall[0] != 0xE8)
      return false;
    g_GatherBegin = pFunction;
    g_GetterBegin = (__int64)pCall + 5 + *(int const*)(pCall + 1);
    return InGame(g_GetterBegin);
  }

  // kind: the branch the game takes for this record, -1 for none
  bool IsGatherEntityGone(__int64 pRecord, int& kind, __int64& pEntity, const char*& why)
  {
    kind = -1;
    why = "its memory is gone";
    __try
    {
      __int64 pInterface = *(__int64*)pRecord;
      unsigned short id = *(unsigned short*)(pRecord + 8);
      for (int k = 0; k < Gather_Kinds; ++k)
      {
        GatherBranch const& branch = g_GatherBranches[k];
        if (id < *branch.pFirst || id > *branch.pLast)
          continue;
        kind = k;
        if (!pInterface)
          return false; // the game checks that one itself
        pEntity = pInterface - branch.Adjust;

        __int64 pVtable = *(__int64*)pEntity;
        BYTE const* pGetType = InGame(pVtable) ? *(BYTE const**)pVtable : nullptr;
        if (!pGetType || !InGame((__int64)pGetType))
        {
          why = "its vtable isn't the game's";
          return true;
        }
        // Only a type that looks like one (in the image, its member info too)
        __int64 pType = 0;
        if (pGetType[0] == 0x48 && pGetType[1] == 0x8D && pGetType[2] == 0x05 && pGetType[7] == 0xC3)
          pType = (__int64)pGetType + 7 + *(int const*)(pGetType + 3);
        if (pType && InGame(pType) && InGame(*(__int64*)pType))
        {
          unsigned short classId = *(unsigned short const*)(pType + 0x28);
          if (classId < *branch.pFirst || classId > *branch.pLast)
          {
            why = "it isn't that kind of entity any more";
            return true;
          }
        }

        why = "its mesh is gone";
        if (k == Gather_Composite || k == Gather_Rigid)
        {
          // The getter: [[entity + 0x60]] + 0x20, then its bounds
          __int64 pMesh = *(__int64*)(pEntity + 0x60);
          if (!pMesh) return true;
          __int64 pFirst = *(__int64*)pMesh;
          if (!pFirst) return true;
          __int64 pBounds = *(__int64*)(pFirst + 0x20);
          if (!pBounds) return true;
          volatile float bounds = *(float*)pBounds + *(float*)(pBounds + 0x10);
          (void)bounds;
          if (k == Gather_Composite)
          {
            // It goes on to the parts, and their count
            __int64 pParts = *(__int64*)(pBounds + 0x20);
            if (!pParts) return true;
            volatile int parts = *(int*)(pParts + 0xA4);
            (void)parts;
          }
        }
        else if (k == Gather_Component && !InGame(*(__int64*)(pVtable + GatherVirtualOffset)))
        {
          why = "its vtable isn't the game's";
          return true;
        }
        return false;
      }
      return false;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      return true;
    }
  }

  // Only access violations in the gather function or its getter
  int GatherFaultFilter(EXCEPTION_POINTERS* pInfo)
  {
    if (pInfo->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION)
      return EXCEPTION_CONTINUE_SEARCH;
    __int64 at = (__int64)pInfo->ExceptionRecord->ExceptionAddress;
    bool inGather = at >= g_GatherBegin && at < g_GatherBegin + GatherFunctionSize;
    bool inGetter = at >= g_GetterBegin && at < g_GetterBegin + GatherGetterSize;
    return inGather || inGetter ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH;
  }

  bool CallMeshGather(__int64 pJob, __int64 pRecord, __int64 pOut, float value, bool& faulted)
  {
    __try
    {
      return oMeshGather(pJob, pRecord, pOut, value);
    }
    __except (GatherFaultFilter(GetExceptionInformation()))
    {
      faulted = true;
      return false;
    }
  }
}

bool __fastcall hMeshGather(__int64 pJob, __int64 pRecord, __int64 pOut, float value)
{
  int kind = -1;
  __int64 pEntity = 0;
  const char* why = "";
  if (IsGatherEntityGone(pRecord, kind, pEntity, why))
  {
    LONG skips = InterlockedIncrement(&g_GatherSkips);
    if (skips <= 5 || skips % 100 == 0)
      util::log::Warning("Skipped a %s entity the game would have crashed on (0x%I64X, %s; %d so far)",
        kind >= 0 ? g_GatherBranches[kind].Name : "?", pEntity, why, (int)skips);
    return false;
  }

  bool faulted = false;
  bool result = CallMeshGather(pJob, pRecord, pOut, value, faulted);
  if (faulted)
  {
    LONG faults = InterlockedIncrement(&g_GatherFaults);
    if (faults <= 5 || faults % 100 == 0)
      util::log::Warning("The game's mesh gather faulted on a %s entity (0x%I64X) and was stopped there (%d so far)",
        kind >= 0 ? g_GatherBranches[kind].Name : "?", pEntity, (int)faults);
  }
  return result;
}

// The renderer's add (exe + 0x36C3960, called from 0x1436C8DA6) files one
// instance into its category's draw batch: (context, the four categories,
// the handles, the spatial tree's records, the instance, its category). A
// category has room for 9 batches of 0x4808 bytes, the game only ever uses
// the first (the add reads the category as its batch), and everything that
// reads a batch goes through all of them by the category's count (+0x28978).
// With the spread on, category 0's instances go into the first batch with
// room within the spread's chunks, then the next one: the add is given that
// batch as the category, finds the category's header at its start (the
// header is at the category's start, re-made every frame, so it's copied
// there each time), and the batch goes into the handle's bits 16 - 27, which
// the update and the take-out read. Batches only go up to 1748 chunks: past
// that a batch's fill bytes are at the start of the next one.
typedef void(__fastcall* tBatchAdd)(__int64, __int64*, unsigned int*, __int64, __int64, int);
tBatchAdd oBatchAdd = nullptr;

namespace
{
  const int AddBatchStride = 0x4808;
  const int AddBatchChunkCount = 0x4130;
  const int AddBatchFill = 0x4134;
  const int AddBatchFillRoom = 1748;
  const int AddCategoryBatchCount = 0x28978;
  const int AddMaxBatches = 9;
  const int AddInstanceHandleIndex = 0x90; // the instance's place in the handles, -1 for none

  std::atomic<int> g_BatchSpreadChunks{ 0 }; // 0: off
  std::atomic<bool> g_BatchSpreadLogged{ false };
  // The add's hook made and enabled (Init; MinHook gives the original's
  // pointer before it enables the hook, so that alone doesn't say)
  bool g_BatchAddHooked = false;
  // The spread's chunks when it was last on: a category already in its
  // other batches when it's switched off (an add between the tools' read
  // and the switch, see CameraManager's BatchSpreadSwitchSafe) goes on as
  // it was, so its first batch never grows past 1748 chunks over the next
  // one's start
  std::atomic<int> g_BatchSpreadLast{ 0 };

  // The first batch of the category that can take an instance: one with a
  // chunk that isn't full or fewer chunks than the spread allows, else the
  // next unused one. One past its fill bytes' room is kept on (the next one's
  // start holds them), and so is the last.
  int PickBatch(BYTE* pCategory, int limit)
  {
    int count = *(int*)(pCategory + AddCategoryBatchCount);
    if (count < 1 || count > AddMaxBatches)
      return 0;
    for (int b = 0; b < AddMaxBatches; ++b)
    {
      if (b >= count)
        return b;
      BYTE* pBatch = pCategory + (__int64)b * AddBatchStride;
      int chunks = *(int*)(pBatch + AddBatchChunkCount);
      if (chunks < limit || chunks > AddBatchFillRoom || b == AddMaxBatches - 1)
        return b;
      for (int i = 0; i < chunks; ++i)
      {
        if (pBatch[AddBatchFill + i] < 64)
          return b;
      }
    }
    return AddMaxBatches - 1;
  }
}

void __fastcall hBatchAdd(__int64 pContext, __int64* pCategories, unsigned int* pHandles, __int64 pRecords, __int64 pInstance, int category)
{
  int limit = g_BatchSpreadChunks;
  if (limit <= 0 && category == 0 && *(int*)((BYTE*)pCategories[0] + AddCategoryBatchCount) > 1)
    limit = g_BatchSpreadLast;   // (already in its other batches: they go on as they were)
  if (limit <= 0 || category != 0 || *(int*)(pInstance + AddInstanceHandleIndex) == -1)
  {
    oBatchAdd(pContext, pCategories, pHandles, pRecords, pInstance, category);
    return;
  }

  BYTE* pCategory = (BYTE*)pCategories[0];
  int batch = PickBatch(pCategory, limit);
  if (batch <= 0)
  {
    oBatchAdd(pContext, pCategories, pHandles, pRecords, pInstance, category);
    return;
  }

  BYTE* pBatch = pCategory + (__int64)batch * AddBatchStride;
  int* pCount = (int*)(pCategory + AddCategoryBatchCount);
  if (*pCount < batch + 1)
    *pCount = batch + 1;
  *(__int64*)pBatch = *(__int64*)pCategory;

  __int64 categories[4] = { pCategories[0], pCategories[1], pCategories[2], pCategories[3] };
  categories[0] = (__int64)pBatch;
  oBatchAdd(pContext, categories, pHandles, pRecords, pInstance, category);

  unsigned int index = *(unsigned int*)(pInstance + AddInstanceHandleIndex);
  pHandles[index] = (pHandles[index] & ~0x0FFF0000u) | ((unsigned int)batch << 16);

  if (!g_BatchSpreadLogged.exchange(true))
    util::log::Write("The renderer's first draw batch is full to %d chunks, instances go on into the next ones", limit);
}

// The add as it was read (exe + 0x36C3960): its start, where it takes the
// category as the batch and its header, and where it makes the handle
static bool IsBatchAddCode(BYTE const* pAdd)
{
  static const BYTE start[] = { 0x48, 0x89, 0xE0, 0x4C, 0x89, 0x40, 0x18, 0x41, 0x55, 0x41, 0x57, 0x48, 0x83, 0xEC, 0x38,
    0x4C, 0x8B, 0x7C, 0x24, 0x70, 0x4D, 0x89, 0xCA, 0x49, 0x89, 0xCD, 0x41, 0x83, 0xBF, 0x90, 0x00, 0x00, 0x00, 0xFF };
  static const BYTE batch[] = { 0x44, 0x8B, 0x64, 0x24, 0x78, 0x4C, 0x89, 0x70, 0xD8, 0x4E, 0x8B, 0x34, 0xE2, 0x49, 0x8B,
    0x0E, 0x48, 0x85, 0xC9 };
  static const BYTE handle[] = { 0x41, 0xC1, 0xE4, 0x1C, 0x41, 0x09, 0xDC, 0x48, 0x8B, 0x5C, 0x24, 0x50, 0x40, 0xFE, 0xC5,
    0x44, 0x89, 0x24, 0x81 };
  return !memcmp(pAdd, start, sizeof(start)) && !memcmp(pAdd + 0x3C, batch, sizeof(batch)) &&
    !memcmp(pAdd + 0x168, handle, sizeof(handle));
}

void util::hooks::SetBatchSpread(int chunks)
{
  if (chunks < 0)
    chunks = 0;
  if (chunks > AddBatchFillRoom)
    chunks = AddBatchFillRoom;
  g_BatchSpreadChunks = g_BatchAddHooked ? chunks : 0;
  if (g_BatchSpreadChunks > 0)
    g_BatchSpreadLast = g_BatchSpreadChunks.load();
}

int util::hooks::BatchSpread()
{
  return g_BatchSpreadChunks;
}

bool util::hooks::BatchSpreadHooked()
{
  return g_BatchAddHooked;
}

// The reflection volumes' cubemaps: only 32 are kept. Every frame the
// game scores each active volume (0x1436EA670, from 0x143707710), sorts
// them by the score's +0x08 (0x1436DA180, 0x1436D8435) and gives the top 32
// a cubemap; a waiting one among them takes the cubemap of the lowest-
// ranked holder (0x1436E4620). +0x08 is how much of the view the volume
// covers (0 - 1), 0.1 if it's ahead, and a base (0; 100 / 1000 for the
// game's own special ones). With every district loaded about 1250
// volumes are active instead of about 50, and ones far away (big, or
// straight ahead) took the cubemaps from those around the camera, whose
// surfaces then go without their reflections and fill light. While on,
// a volume ranks one point lower for every NearReflectionFalloff metres
// from the camera to its box, so those around the camera come first.
typedef __int64(__fastcall* tReflectionScore)(__int64, BYTE*, float const*, float const*, float, unsigned short,
  BYTE const*, float, float, unsigned int);
tReflectionScore oReflectionScore = nullptr;

namespace
{
  const __int64 ReflectionScoreRva = 0x36EA670;
  const __int64 ReflectionSortCompareRva = 0x36D842F; // the sort's compare of +0x08
  const int ScorePriority = 0x08;     // float, what the 32 go by
  const int VolumeRows = 0x00;        // three float4 rows: the box's half axes
  const int VolumePosition = 0x30;
  const int VolumeExtents = 0x40;     // half sizes (the rows' lengths); a sphere's radius in x
  const int VolumeShape = 0x98;       // 0 sphere, 1 box
  const float NearReflectionFalloff = 100.f;

  std::atomic<bool> g_NearReflections{ false };

  // From the camera to the volume, 0 inside it
  float DistanceToVolume(BYTE const* pVolume, float const* pCamera)
  {
    float const* pPosition = (float const*)(pVolume + VolumePosition);
    float const* pExtents = (float const*)(pVolume + VolumeExtents);
    float d[3] = { pCamera[0] - pPosition[0], pCamera[1] - pPosition[1], pCamera[2] - pPosition[2] };
    if (*(int const*)(pVolume + VolumeShape) == 0)
    {
      float out = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]) - pExtents[0];
      return out > 0.f ? out : 0.f;
    }
    float sum = 0.f;
    for (int i = 0; i < 3; ++i)
    {
      float const* pRow = (float const*)(pVolume + VolumeRows + 0x10 * i);
      float length = sqrtf(pRow[0] * pRow[0] + pRow[1] * pRow[1] + pRow[2] * pRow[2]);
      if (length <= 0.f)
        continue;
      float out = fabsf((d[0] * pRow[0] + d[1] * pRow[1] + d[2] * pRow[2]) / length) - pExtents[i];
      if (out > 0.f)
        sum += out * out;
    }
    return sqrtf(sum);
  }
}

__int64 __fastcall hReflectionScore(__int64 pScheduler, BYTE* pScore, float const* pCamera, float const* pViewDirection,
  float viewSize, unsigned short volume, BYTE const* pVolume, float base, float waitingBonus, unsigned int frame)
{
  __int64 result = oReflectionScore(pScheduler, pScore, pCamera, pViewDirection, viewSize, volume, pVolume, base,
    waitingBonus, frame);
  if (g_NearReflections.load(std::memory_order_relaxed))
  {
    __try
    {
      *(float*)(pScore + ScorePriority) -= DistanceToVolume(pVolume, pCamera) / NearReflectionFalloff;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
  }
  return result;
}

// The score as it was read: its start, where it reads the shape, where it
// writes +0x08, and the sort comparing +0x08
static bool IsReflectionScoreCode(BYTE const* pScore, BYTE const* pCompare)
{
  static const BYTE start[] = { 0x48, 0x89, 0xE0, 0x48, 0x89, 0x58, 0x08, 0x48, 0x89, 0x70, 0x10, 0x57, 0x48, 0x83, 0xEC, 0x70 };
  static const BYTE shape[] = { 0x8B, 0x8F, 0x98, 0x00, 0x00, 0x00 };
  static const BYTE store[] = { 0xF3, 0x0F, 0x11, 0x43, 0x08 };
  static const BYTE compare[] = { 0xF3, 0x0F, 0x10, 0x4C, 0x24, 0x08, 0x0F, 0x2F, 0x49, 0xF4 };
  return !memcmp(pScore, start, sizeof(start)) && !memcmp(pScore + 0x29, shape, sizeof(shape)) &&
    !memcmp(pScore + 0x258, store, sizeof(store)) && !memcmp(pCompare, compare, sizeof(compare));
}

// Enlighten's light map ids: every piece with a light map gets one from
// 0x143791D70 (Enlighten's manager, [0x14256AF40]): a free one off the stack
// at +0x108C0 (u16s, its top at +0x108C8) or, with none free, a new slot at
// the end of the table at +0x10868 (0x10 bytes each), whose number is cut to
// 16 bits (0x143791F3C). The game's own zones stay far below that. With every
// district loaded the city has about 90000 such pieces, so past 65536 slots
// the numbers wrapped round to ones in use, and pieces drew other pieces'
// light maps: black buildings, the shade far too dark. Near the end of the
// table the tools hand out 0xFFFF instead, the game's own "no light map" (the
// lookup 0x143791CF0 and the release 0x143795A30 skip it), and those pieces go
// without bounce light. Checked before the allocator takes its lock (it takes
// it only after two lookups that may take others), so LightMapSlotsMost
// leaves room for several threads at once.
typedef unsigned int(__fastcall* tLightMapId)(__int64, __int64, __int64, int);
tLightMapId oLightMapId = nullptr;

namespace
{
  const __int64 LightMapIdRva = 0x3791D70;
  const int LightMapTable = 0x10868;            // begin, end, capacity of 0x10-byte slots
  const int LightMapFreeIds = 0x108C0;          // begin, top of the free ids (u16)
  const __int64 LightMapSlotsMost = 0xFFF0;     // ids up to 0xFFEF
  const unsigned int NoLightMapId = 0xFFFF;

  std::atomic<unsigned int> g_LightMapIdsRefused{ 0 };
  std::atomic<unsigned int> g_LightMapIdsRefusedLogged{ 0 };
}

unsigned int __fastcall hLightMapId(__int64 pManager, __int64 a2, __int64 pPiece, int a4)
{
  __try
  {
    if (*(__int64*)(pManager + LightMapFreeIds) == *(__int64*)(pManager + LightMapFreeIds + 8))
    {
      __int64 slots = (*(__int64*)(pManager + LightMapTable + 8) - *(__int64*)(pManager + LightMapTable)) / 0x10;
      if (slots >= LightMapSlotsMost)
      {
        ++g_LightMapIdsRefused;
        return NoLightMapId;
      }
    }
  }
  __except (EXCEPTION_EXECUTE_HANDLER) {}
  return oLightMapId(pManager, a2, pPiece, a4);
}

// The allocator as it was read: its start, its check for a free id, where it
// makes the new id from the table's size, and its "no light map" return
static bool IsLightMapIdCode(BYTE const* pAlloc)
{
  static const BYTE start[] = { 0x48, 0x89, 0xE0, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x83, 0xEC, 0x50 };
  static const BYTE freeCheck[] = { 0x48, 0x8B, 0x8E, 0xC8, 0x08, 0x01, 0x00, 0x48, 0x39, 0x8E, 0xC0, 0x08, 0x01, 0x00 };
  static const BYTE newId[] = { 0x44, 0x8B, 0xB6, 0x70, 0x08, 0x01, 0x00, 0x44, 0x2B, 0xB6, 0x68, 0x08, 0x01, 0x00, 0x49,
    0xC1, 0xFE, 0x04, 0x66, 0x41, 0xFF, 0xCE };
  static const BYTE none[] = { 0xB8, 0xFF, 0xFF, 0x00, 0x00 };
  return !memcmp(pAlloc, start, sizeof(start)) && !memcmp(pAlloc + 0x9F, freeCheck, sizeof(freeCheck)) &&
    !memcmp(pAlloc + 0x1BA, newId, sizeof(newId)) && !memcmp(pAlloc + 0x325, none, sizeof(none));
}

// Replay: mission items shown and hidden, and models' looks switched. The
// level's logic shows and hides them rather than moving them away: a
// delivery's package is pf_deliveryobject01, a prefab whose Visibility goes
// to its model's Visible (StaticModelEntityData + 0xDC, True by default), and
// the handover's timeline switches it (an AndEntity, in
// pf_opportunitymission_delivery*_animations_*). The models in a prefab are
// members of a ClientStaticModelGroupEntity (vtable 0x141AFBE90), and the
// game gives a member's Visible, ObjectVariation or Enabled to the group's
// property handler 0x14318DC70 (its vtable slot 2) as an event { hash, value
// pointer } whose hash says which (2026-10-25, offline):
//   hash & 3: the field (0 Enabled, 1 ObjectVariation, 2 Visible);
//   (hash >> 2) & 0x7FF: the member + 1 (its record [group + 0xE0] + 0x130 a
//     member, [group + 0xE8] of them);
//   (hash >> 13) & 0x7FF: which of the member's instances.
// Visible goes to 0x14318F730(member, the value's byte, instance) and
// ObjectVariation to 0x14318FB30 -> 0x14376C4B0, which reads only the
// variation's name hash (its + 0x18) and looks its mesh up from that. The
// group's own BlueprintTransform (0xE790F446) and ObjectVariation
// (0xD806FCC1) come as plain hashes. So a recording kept moving the package
// but it stayed as the game had it at playback (hidden, or gone). While the
// replay records, each member's Visible and ObjectVariation the game sets is
// kept (which group, which hash, when, the value); while it plays, each is
// set as it was at that moment by giving the group's handler the same event,
// and when playback stops each gets back what the game last gave it. Enabled
// isn't replayed (it makes and takes away the member's parts). A group's
// destructor (0x14317CE50, called only from its deleting destructor at
// 0x14317E15F) drops what's kept of it, so nothing is given to a group that's
// gone, or to another one made where it was.
typedef void(__fastcall* tGroupProperty)(__int64, __int64);
tGroupProperty oGroupProperty = nullptr;
typedef void(__fastcall* tGroupDtor)(__int64);
tGroupDtor oGroupDtor = nullptr;

namespace
{
  const __int64 GroupPropertyRva = 0x318DC70;
  const __int64 GroupDtorRva = 0x317CE50;
  const __int64 GroupVTableRva = 0x1AFBE90;
  const unsigned int GroupTransformHash = 0xE790F446;
  const unsigned int GroupVariationHash = 0xD806FCC1;
  const int GroupMemberCountOffset = 0xE8;
  const unsigned int MemberVariation = 1, MemberVisible = 2;
  const int VariationNameHashOffset = 0x18;
  const size_t MaxMemberChanges = 16384;
  const size_t MaxMemberLive = 16384;

  struct MemberKey
  {
    __int64 Group;
    unsigned int Hash;
    bool operator==(MemberKey const& o) const { return Group == o.Group && Hash == o.Hash; }
  };
  struct MemberKeyHash
  {
    size_t operator()(MemberKey const& k) const { return std::hash<__int64>()(k.Group) ^ ((size_t)k.Hash * 0x9E3779B97F4A7C15ull); }
  };
  // A member's value: Visible 0 / 1; ObjectVariation 1 << 32 | its name
  // hash, or 0 for none
  typedef unsigned long long MemberValue;
  struct MemberChange
  {
    double Time;
    MemberKey Key;
    MemberValue Value;
  };
  struct MemberTrack
  {
    MemberKey Key;
    bool HasBefore;     // what the game had given it when the recording started
    MemberValue Before;
    std::vector<std::pair<double, MemberValue>> Changes;   // in time order
    bool Gone;          // its group was destroyed
  };

  std::mutex g_MemberMutex;
  bool g_MemberRecording = false;                    // under g_MemberMutex, as everything here
  double g_MemberClock = 0.0;
  std::vector<MemberChange> g_MemberChanges;         // while it records
  int g_MemberDropped = 0;
  std::unordered_map<MemberKey, MemberValue, MemberKeyHash> g_MemberLive;      // what the game last gave each
  std::unordered_map<MemberKey, MemberValue, MemberKeyHash> g_MemberAtStart;   // g_MemberLive as the recording started
  std::unordered_set<__int64> g_MemberGroups;        // the groups any of these name
  std::vector<MemberTrack> g_MemberTracks;           // the recording's, made when it ends
  std::unordered_map<MemberKey, MemberValue, MemberKeyHash> g_MemberApplied;   // what the replay gave each
  int g_MemberSets = 0, g_MemberSetsFailed = 0;      // this playback's

  bool ReadMemberEvent(__int64 event, unsigned int& hash, MemberValue& value)
  {
    __try
    {
      hash = *(unsigned int const*)event;
      if (hash == GroupTransformHash || hash == GroupVariationHash || !((hash >> 2) & 0x7FF))
        return false;
      __int64 pValue = *(__int64 const*)(event + 8);
      if ((hash & 3) == MemberVisible)
      {
        if (!pValue)
          return false;
        value = *(BYTE const*)pValue ? 1 : 0;
        return true;
      }
      if ((hash & 3) == MemberVariation)
      {
        value = pValue ? (1ull << 32) | *(unsigned int const*)(pValue + VariationNameHashOffset) : 0;
        return true;
      }
      return false;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      return false;
    }
  }

  // As the game does: the same event to the group's handler. False if it isn't
  // a group any more or hasn't that member (nothing given)
  bool SetMember(MemberKey const& key, MemberValue value)
  {
    __try
    {
      if (*(__int64 const*)key.Group != (__int64)g_gameHandle + GroupVTableRva)
        return false;
      int member = (int)((key.Hash >> 2) & 0x7FF) - 1;
      if (member < 0 || member >= *(int const*)(key.Group + GroupMemberCountOffset))
        return false;
      BYTE visible = (BYTE)(value & 1);
      __declspec(align(16)) BYTE variation[0x20] = {};
      *(unsigned int*)(variation + VariationNameHashOffset) = (unsigned int)value;
      struct { unsigned int Hash, Pad; void* Value; } event = { key.Hash, 0, nullptr };
      if ((key.Hash & 3) == MemberVisible)
        event.Value = &visible;
      else
        event.Value = (value >> 32) ? variation : nullptr;
      oGroupProperty(key.Group, (__int64)&event);
      return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      return false;
    }
  }
}

void __fastcall hGroupProperty(__int64 group, __int64 event)
{
  unsigned int hash = 0;
  MemberValue value = 0;
  bool member = !g_shutdown && ReadMemberEvent(event, hash, value);
  oGroupProperty(group, event);
  if (!member)
    return;
  MemberKey key = { group, hash };
  std::lock_guard<std::mutex> lock(g_MemberMutex);
  auto live = g_MemberLive.find(key);
  if (live != g_MemberLive.end())
    live->second = value;
  else if (g_MemberLive.size() < MaxMemberLive)
  {
    g_MemberLive[key] = value;
    g_MemberGroups.insert(group);
  }
  // The game set one the replay had set: given again next frame
  g_MemberApplied.erase(key);
  if (!g_MemberRecording)
    return;
  if (g_MemberChanges.size() >= MaxMemberChanges)
  {
    ++g_MemberDropped;
    return;
  }
  g_MemberChanges.push_back({ g_MemberClock, key, value });
  g_MemberGroups.insert(group);
}

void __fastcall hGroupDtor(__int64 group)
{
  {
    std::lock_guard<std::mutex> lock(g_MemberMutex);
    if (g_MemberGroups.erase(group))
    {
      for (auto it = g_MemberLive.begin(); it != g_MemberLive.end();)
        it = it->first.Group == group ? g_MemberLive.erase(it) : std::next(it);
      for (auto it = g_MemberAtStart.begin(); it != g_MemberAtStart.end();)
        it = it->first.Group == group ? g_MemberAtStart.erase(it) : std::next(it);
      for (auto it = g_MemberApplied.begin(); it != g_MemberApplied.end();)
        it = it->first.Group == group ? g_MemberApplied.erase(it) : std::next(it);
      for (MemberChange& c : g_MemberChanges)
        if (c.Key.Group == group)
          c.Key.Group = 0;
      for (MemberTrack& t : g_MemberTracks)
        if (t.Key.Group == group)
          t.Gone = true;
    }
  }
  oGroupDtor(group);
}

static bool IsGroupPropertyCode(BYTE const* pBase)
{
  // push rbx; push rdi; push r14; sub rsp, 0x70; mov ebx, [rdx]; mov r14, rdx; mov rdi, rcx; cmp ebx, 0xE790F446
  static const BYTE start[] = { 0x53, 0x57, 0x41, 0x56, 0x48, 0x83, 0xEC, 0x70, 0x8B, 0x1A, 0x49, 0x89, 0xD6, 0x48, 0x89, 0xCF,
    0x81, 0xFB, 0x46, 0xF4, 0x90, 0xE7 };
  // cmp ebx, 0xD806FCC1; je; mov eax, ebx; shr ebx, 2
  static const BYTE plain[] = { 0x81, 0xFB, 0xC1, 0xFC, 0x06, 0xD8, 0x0F, 0x84, 0x7F, 0x01, 0x00, 0x00, 0x89, 0xD8, 0xC1, 0xEB,
    0x02 };
  // and r15d, 0x7FF; and ebx, 0x7FF; dec r15d; and eax, 3
  static const BYTE fields[] = { 0x41, 0x81, 0xE7, 0xFF, 0x07, 0x00, 0x00, 0x81, 0xE3, 0xFF, 0x07, 0x00, 0x00, 0x41, 0xFF, 0xCF,
    0x83, 0xE0, 0x03 };
  // imul rbp, rax, 0x130; add rbp, [rdi + 0xE0]
  static const BYTE members[] = { 0x48, 0x69, 0xE8, 0x30, 0x01, 0x00, 0x00, 0x48, 0x03, 0xAF, 0xE0, 0x00, 0x00, 0x00 };
  // Visible: mov rax, [r14 + 8]; mov r8d, ebx; mov rcx, rbp; movzx edx, byte [rax]; call 0x14318F730
  static const BYTE visible[] = { 0x49, 0x8B, 0x46, 0x08, 0x41, 0x89, 0xD8, 0x48, 0x89, 0xE9, 0x0F, 0xB6, 0x10, 0xE8 };
  // ObjectVariation: mov r8, [r14 + 8]; mov edx, ebx; mov rcx, rbp; call 0x14318FB30
  static const BYTE variation[] = { 0x4D, 0x8B, 0x46, 0x08, 0x89, 0xDA, 0x48, 0x89, 0xE9, 0xE8 };
  // in 0x14376C4B0: test rdx, rdx; je; mov r8d, [rdx + 0x18] (the variation's name hash)
  static const BYTE applyVariation[] = { 0x48, 0x85, 0xD2, 0x74, 0x08, 0x44, 0x8B, 0x42, 0x18 };
  BYTE const* p = pBase + GroupPropertyRva;
  return !memcmp(p, start, sizeof(start)) && !memcmp(p + 0x76, plain, sizeof(plain)) &&
    !memcmp(p + 0xB0, fields, sizeof(fields)) && !memcmp(p + 0x101, members, sizeof(members)) &&
    !memcmp(p + 0x142, visible, sizeof(visible)) && !memcmp(p + 0x1DB, variation, sizeof(variation)) &&
    !memcmp(pBase + 0x376C4BD, applyVariation, sizeof(applyVariation)) &&
    *(__int64 const*)(pBase + GroupVTableRva + 2 * 8) == (__int64)pBase + GroupPropertyRva;
}

static bool IsGroupDtorCode(BYTE const* pBase)
{
  // mov [rsp + 8], rcx; push rsi; push rdi; push r14; sub rsp, 0x30; mov qword [rsp + 0x20], -2; ...; mov rsi, rcx;
  // lea rax, [rip + the group's vtable]; mov [rcx], rax
  static const BYTE start[] = { 0x48, 0x89, 0x4C, 0x24, 0x08, 0x56, 0x57, 0x41, 0x56, 0x48, 0x83, 0xEC, 0x30 };
  static const BYTE vtable[] = { 0x48, 0x89, 0xCE, 0x48, 0x8D, 0x05 };
  BYTE const* p = pBase + GroupDtorRva;
  if (memcmp(p, start, sizeof(start)) || memcmp(p + 0x20, vtable, sizeof(vtable)))
    return false;
  return p + 0x2A + *(int const*)(p + 0x26) == pBase + GroupVTableRva;
}

// A member's Visible given to a model group's handler the way the game gives
// it (the hash above: field 2, member + 1, the instance), past the tools' hook
// so the replay doesn't take it for the game's. The handler doesn't check the
// instance: the caller keeps it below the member's instance count.
bool util::hooks::SetModelGroupMemberVisible(__int64 group, int member, int instance, bool visible)
{
  if (!oGroupProperty || member < 0 || member >= 0x7FE || instance < 0 || instance > 0x7FF)
    return false;
  unsigned int hash = ((unsigned int)instance << 13) | ((unsigned int)(member + 1) << 2) | MemberVisible;
  return SetMember({ group, hash }, visible ? 1 : 0);
}

// A member's ObjectVariation the same way (field 1): the variation's name hash, or 0 for none (the mesh's own look;
// the handler 0x14318FB30 -> 0x14376C4B0 takes a null variation as none). Also past the tools' hook.
bool util::hooks::SetModelGroupMemberVariation(__int64 group, int member, int instance, unsigned int nameHash)
{
  if (!oGroupProperty || member < 0 || member >= 0x7FE || instance < 0 || instance > 0x7FF)
    return false;
  unsigned int hash = ((unsigned int)instance << 13) | ((unsigned int)(member + 1) << 2) | MemberVariation;
  return SetMember({ group, hash }, nameHash ? (1ull << 32) | nameHash : 0);
}

// The Shard's top (Kruger's office, pf_skycity_building_05, 1570 m up) isn't
// in the real tower's zone: it's in the sub-levels SkyCityTopOpenBlinds and
// SkyCityTopClosedBlinds, which the main level streams in when the game's
// camera enters the area around the Shard (layer1_streaming's volume 15) and
// out when it leaves, through camera area triggers (gameplay/logic/
// pf_cameraareatrigger: a CameraEnterAreaTriggerEntity, vtable 0x141B11290,
// then OnEnter / OnLeave to the sub-level's StreamIn / StreamOut;
// layer48_districtstreaming). The trigger's update (0x143230DD0) takes the
// camera registry's transform (0x143278E40), which the free camera never
// changes, and asks 0x14330D320(links, { data, 0x31FB3B9F }, owner, the
// camera's position) whether it's in the linked volume, at 0x143230EBE and
// 0x143231121 (2026-10-25, offline). So with the free camera at the Shard and
// Faith in the city the tower came without its top. Past those two calls
// only, when the game's camera isn't in the volume the free camera's position
// is asked too, while the tools say it counts (CameraManager: the free camera
// is out of the city's area), and only for the triggers on that volume: told
// apart by the game's own test on points just inside and just outside its 8
// corners (camera triggers on other volumes, lifts, map markers, character
// streaming, are left alone). In: the game's own update fires OnEnter and the
// top streams in as it does for Faith; neither in: OnLeave, out again.
// The Bauble (the mall the story destroys) is streamed the same way: the
// sub-levels BaubleMall (the whole mall), BaubleDestruction (its collapse) and
// BaubleRuins (the ruins and their smoke), each by a camera trigger on
// layer1_streaming's volume 19, a large part of the city around it (layer48
// #16, #32, #17). The free camera counts for those whenever it's on.
typedef bool(__fastcall* tLinkVolumeContains)(__int64, __int64, __int64, float const*);
tLinkVolumeContains oLinkVolumeContains = nullptr;

namespace
{
  const __int64 LinkVolumeContainsRva = 0x330D320;
  const __int64 CameraTriggerTestReturnRva[2] = { 0x3230EC3, 0x3231126 };
  // The volumes, x and z of their corners (layer1_streaming; no height)
  const float ShardArea[][2] = { { 2406.72f, -1968.19f }, { 2723.83f, -3548.88f }, { 1756.24f, -4646.43f },
    { 326.239f, -4850.4f }, { -872.062f, -3680.05f }, { -781.582f, -2490.59f }, { 210.563f, -1424.52f },
    { 1429.53f, -1162.08f } };   // 15, around the Shard
  const float BaubleArea[][2] = { { 59.51f, 673.79f }, { -127.89f, 675.53f }, { -278.67f, 529.01f },
    { -264.76f, 447.9f }, { -221.31f, 427.66f }, { -146.02f, 224.62f }, { -211.52f, 218.69f }, { -158.74f, 94.49f },
    { -100.39f, 63.19f }, { -14.51f, 35.82f }, { 9.29f, 8.63f }, { 8.23f, -50.17f }, { 132.95f, -75.82f },
    { 175.24f, -137.22f }, { 209.76f, -606.6f }, { 463.55f, -798.76f }, { 1358.93f, -750.1f }, { 1374.35f, -31.47f },
    { 1091.51f, 266.03f }, { 510.67f, 449.84f }, { 149.26f, 327.52f } };   // 19, around the Bauble
  enum AreaKind { Area_Other = 0, Area_ShardTop = 1, Area_Bauble = 2 };
  struct KnownArea
  {
    AreaKind Kind;
    float const (*Corners)[2];
    int Count;
  };
  const KnownArea KnownAreas[] = { { Area_ShardTop, ShardArea, (int)_countof(ShardArea) },
    { Area_Bauble, BaubleArea, (int)_countof(BaubleArea) } };
  // A trigger's volume is told by the game's own test on a grid of points over
  // the area's box (8 x 8), none this close to an edge, all as the outline says
  const int AreaProbeGrid = 8;
  const float AreaProbeEdge = 15.f;
  const float AreaProbeY = 100.f;
  const int MaxAreaTriggers = 64;

  std::atomic<bool> g_AreaCameraOn{ false };          // the free camera is on
  std::atomic<bool> g_AreaCameraOutOfCity{ false };   // and out of the city's area (CameraManager)
  std::atomic<float> g_AreaCamera[3]{};
  std::atomic<bool> g_ShardTopAlways{ false };        // the Shard's top streamed in wherever the view is (camera on or off)
  struct AreaTrigger
  {
    __int64 Links, Data, Owner;   // what the game's test is given (the trigger's links, its data, its owner)
    AreaKind Kind;                // the volume it's on
    bool CameraIn;                // in by the free camera only, last time
  };
  std::mutex g_AreaMutex;
  AreaTrigger g_AreaTriggers[MaxAreaTriggers]{};   // under g_AreaMutex
  int g_AreaTriggerCount = 0, g_AreaNext = 0;

  __int64 ReadLinkKeyData(__int64 key)
  {
    __try
    {
      return *(__int64 const*)key;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      return 0;
    }
  }

  // Whether (x, z) is inside an outline, and how far from its nearest edge
  bool InOutline(float const (*corners)[2], int count, float x, float z, float& edge)
  {
    bool inside = false;
    edge = FLT_MAX;
    for (int i = 0, j = count - 1; i < count; j = i++)
    {
      float ax = corners[j][0], az = corners[j][1], bx = corners[i][0], bz = corners[i][1];
      if ((bz > z) != (az > z) && x < (ax - bx) * (z - bz) / (az - bz) + bx)
        inside = !inside;
      float ex = ax - bx, ez = az - bz;
      float t = ((x - bx) * ex + (z - bz) * ez) / (ex * ex + ez * ez);
      t = t < 0.f ? 0.f : (t > 1.f ? 1.f : t);
      float dx = x - (bx + t * ex), dz = z - (bz + t * ez);
      edge = (std::min)(edge, sqrtf(dx * dx + dz * dz));
    }
    return inside;
  }

  // Which known volume a trigger is on: the game's own test agrees with the
  // outline on every probe point (at least 4 in and 4 out)
  AreaKind ClassifyArea(__int64 links, __int64 key, __int64 owner)
  {
    for (KnownArea const& area : KnownAreas)
    {
      float minX = FLT_MAX, minZ = FLT_MAX, maxX = -FLT_MAX, maxZ = -FLT_MAX;
      for (int c = 0; c < area.Count; ++c)
      {
        minX = (std::min)(minX, area.Corners[c][0]);
        maxX = (std::max)(maxX, area.Corners[c][0]);
        minZ = (std::min)(minZ, area.Corners[c][1]);
        maxZ = (std::max)(maxZ, area.Corners[c][1]);
      }
      float padX = (maxX - minX) * 0.1f, padZ = (maxZ - minZ) * 0.1f;
      int in = 0, out = 0;
      bool agrees = true;
      for (int i = 0; i < AreaProbeGrid && agrees; ++i)
      {
        for (int j = 0; j < AreaProbeGrid && agrees; ++j)
        {
          float x = minX - padX + (maxX - minX + 2.f * padX) * (i + 0.5f) / AreaProbeGrid;
          float z = minZ - padZ + (maxZ - minZ + 2.f * padZ) * (j + 0.5f) / AreaProbeGrid;
          float edge;
          bool expected = InOutline(area.Corners, area.Count, x, z, edge);
          if (edge < AreaProbeEdge)
            continue;
          __declspec(align(16)) float point[4] = { x, AreaProbeY, z, 1.f };
          agrees = oLinkVolumeContains(links, key, owner, point) == expected;
          ++(expected ? in : out);
        }
      }
      if (agrees && in >= 4 && out >= 4)
        return area.Kind;
    }
    return Area_Other;
  }

  // In by the free camera, for a trigger on one of the known volumes
  bool AreaCameraIn(__int64 links, __int64 key, __int64 owner)
  {
    __int64 data = ReadLinkKeyData(key);
    std::lock_guard<std::mutex> lock(g_AreaMutex);
    AreaTrigger* pTrigger = nullptr;
    for (int i = 0; i < g_AreaTriggerCount; ++i)
    {
      AreaTrigger& trigger = g_AreaTriggers[i];
      if (trigger.Links == links && trigger.Data == data && trigger.Owner == owner)
        pTrigger = &trigger;
    }
    if (!pTrigger)
    {
      int slot = g_AreaTriggerCount < MaxAreaTriggers ? g_AreaTriggerCount++ : g_AreaNext++ % MaxAreaTriggers;
      pTrigger = &g_AreaTriggers[slot];
      *pTrigger = { links, data, owner, ClassifyArea(links, key, owner), false };
      if (pTrigger->Kind == Area_ShardTop)
        util::log::Write("The Shard: camera area trigger 0x%I64X is one on the area around the Shard (its top's "
          "sub-level); the free camera counts for it while it's out of the city", links - 0x30);
      else if (pTrigger->Kind == Area_Bauble)
        util::log::Write("The Bauble: camera area trigger 0x%I64X is one on the area around the Bauble (a version of the "
          "mall); the free camera counts for it", links - 0x30);
    }
    // The real Shard streamed from anywhere (GAME > Districts): its top's
    // trigger counts as entered, with the free camera on or off
    bool always = pTrigger->Kind == Area_ShardTop && g_ShardTopAlways;
    bool counts = g_AreaCameraOn && (pTrigger->Kind == Area_Bauble ||
      (pTrigger->Kind == Area_ShardTop && g_AreaCameraOutOfCity));
    bool in = always;
    if (!in && counts)
    {
      __declspec(align(16)) float camera[4] = { g_AreaCamera[0], g_AreaCamera[1], g_AreaCamera[2], 1.f };
      in = oLinkVolumeContains(links, key, owner, camera);
    }
    if (in != pTrigger->CameraIn && pTrigger->Kind != Area_Other)
    {
      pTrigger->CameraIn = in;
      if (pTrigger->Kind == Area_ShardTop && always)
        util::log::Write("The Shard: its top streams in wherever the view is (the real Shard is streamed from anywhere, "
          "trigger 0x%I64X)", links - 0x30);
      else if (pTrigger->Kind == Area_ShardTop)
        util::log::Write(in ? "The Shard: the free camera is in the area around it: its top streams in (trigger 0x%I64X)" :
          "The Shard: the free camera left the area around it: its top streams out unless Faith is there (trigger 0x%I64X)",
          links - 0x30);
      else
        util::log::Write(in ? "The Bauble: the free camera is in the area around it: the trigger's version of it (the mall, "
          "its collapse or its ruins) streams in (trigger 0x%I64X)" : "The Bauble: the free camera left the area around it: "
          "the trigger's version of it streams out unless Faith is there (trigger 0x%I64X)", links - 0x30);
    }
    return in;
  }
}

bool __fastcall hLinkVolumeContains(__int64 links, __int64 key, __int64 owner, float const* position)
{
  bool inside = oLinkVolumeContains(links, key, owner, position);
  if (inside || g_shutdown || (!g_AreaCameraOn.load(std::memory_order_relaxed) &&
    !g_ShardTopAlways.load(std::memory_order_relaxed)))
    return inside;
  __int64 from = (__int64)_ReturnAddress() - (__int64)g_gameHandle;
  if (from != CameraTriggerTestReturnRva[0] && from != CameraTriggerTestReturnRva[1])
    return inside;
  return AreaCameraIn(links, key, owner);
}

static bool IsLinkVolumeTestCode(BYTE const* pBase)
{
  // push rbp; push rsi; push rdi; push r14; push r15; lea rbp, [rsp - 0x16F0]; mov eax, 0x17F0
  static const BYTE code[] = { 0x55, 0x56, 0x57, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8D, 0xAC, 0x24, 0x10, 0xE9, 0xFF, 0xFF,
    0xB8, 0xF0, 0x17, 0x00, 0x00 };
  // the camera trigger's update: lea rcx, [rsi + 0x30]; lea r9, [rbp + 0x17] (the camera's position);
  // mov r8, [rsi + 0x20]; lea rdx, [rsp + ..]; call; then test al, al
  static const BYTE caller[] = { 0x48, 0x8D, 0x4E, 0x30, 0x4C, 0x8D, 0x4D, 0x17, 0x4C, 0x8B, 0x46, 0x20, 0x48, 0x8D, 0x54, 0x24 };
  if (memcmp(pBase + LinkVolumeContainsRva, code, sizeof(code)))
    return false;
  for (__int64 ret : CameraTriggerTestReturnRva)
  {
    BYTE const* p = pBase + ret;
    if (memcmp(p - 0x16, caller, sizeof(caller)) || p[-5] != 0xE8 || p[0] != 0x84 || p[1] != 0xC0 ||
      p + *(int const*)(p - 4) != pBase + LinkVolumeContainsRva)
      return false;
  }
  return true;
}

void util::hooks::SetAreaTriggerCamera(bool cameraOn, bool outOfCity, float x, float y, float z, bool shardTopAlways)
{
  g_AreaCamera[0] = x;
  g_AreaCamera[1] = y;
  g_AreaCamera[2] = z;
  g_AreaCameraOutOfCity = outOfCity;
  g_AreaCameraOn = cameraOn;
  g_ShardTopAlways = shardTopAlways;
}

// The Shard and the Bauble as before the story. Which version of each the
// game streams is chosen by which of its camera triggers is enabled: their
// Enabled comes from MathEntities on progression flags (layer48: the Shard's
// #83 / #18 from flag #34, the Bauble's #53 / #1 / #37 from flags #2 and #47).
// The trigger's event handler (vtable slot 4, 0x143232310) takes Enable
// (0x8BA423E4: its update registered, 0x1432306F0, + 0x69 set) and Disable
// (0xF90216B1: unregistered and, with the prefab's FireOnLeaveWhenDisable,
// OnLeave for each player inside), reading nothing of the event but its id
// (+ 8). UpdateLandmarkTriggers gives the whole versions' triggers Enable and
// the broken ones' Disable, so the game's own triggers stream the whole ones
// in and the broken ones out; off, each gets back what the game had. It also
// takes the Shard's far-away sub-level out (both its triggers Disabled) while
// the real tower is loaded and the view is out of the city, as the game does
// when Faith leaves the city: that sub-level has two fans of its own (a
// PamStaticModelEntity skycity_building_fan01 at 2.9 and 1.7 times its size,
// layer0_skycity_backdrop / layer0_skycityruinbackdrop_art), which stayed in
// view up close (user, 2026-10-25 20:5x), and no way to hide one alone is
// known. A trigger
// is told by its prefab reference: the data the prefab's bus sends OnEnter /
// OnLeave from to the level ([bus + 0x58] & ~3, 0x14314E80F with r13 = bus -
// 8; a LogicPrefabReferenceObjectData, vtable 0x141AEFBF0), whose DataBusPeer
// Flags (+ 0x10) are unique in the level's data (2026-10-25, offline; read
// live 20:2x: trigger 0x63992B50, vtable 0x141B11290, its bus + 0x58 a
// reference with Flags 1900523863, layer48 #25's). The class keeps no instance
// list (its ClassInfo's lists were empty live), so the triggers are noted as
// the game sets each one up (its vtable slot 15, 0x143236BD0, which links its
// volume and starts it if AutoStart).
namespace
{
  const __int64 CameraTriggerVTableRva = 0x1B11290;     // CameraEnterAreaTriggerEntity (slot 0 its getType 0x143232D90)
  const __int64 CameraTriggerTypeRva = 0x3232D90;
  const __int64 CameraTriggerEventRva = 0x3232310;      // slot 4
  const __int64 CameraTriggerInitRva = 0x3236BD0;       // slot 15
  const __int64 PrefabReferenceVTableRva = 0x1AEFBF0;   // LogicPrefabReferenceObjectData
  const __int64 PrefabReferenceTypeRva = 0x4651A50;     // its getType, slot 0
  const size_t MaxNotedTriggers = 1024;
  const int TriggerBusOffset = 0x20;
  const int TriggerRegisteredOffset = 0x69;
  const int BusReferenceOffsets[2] = { 0x58, 0x60 };    // the first is the one read from the code
  const int PeerFlagsOffset = 0x10;
  const unsigned int TriggerEnable = 0x8BA423E4, TriggerDisable = 0xF90216B1;
  const ULONGLONG LandmarkCheckMs = 1000;
  const int MaxCameraTriggers = 64;

  enum LandmarkKind { Landmark_ShardBackdrop, Landmark_Bauble, Landmark_TopBlinds };
  struct LandmarkVersion
  {
    unsigned int Flags;   // its prefab reference's, in layer48_districtstreaming
    LandmarkKind Kind;
    bool Whole;           // the whole version; for the Shard's top, the one with its blinds open
    const char* Name;
  };
  // The top's two: SkyCityTopOpenBlinds has pf_skycity_building_05, and
  // SkyCityTopClosedBlinds pf_skycity_building_05_opaque and three movement
  // exclusions (their layers' own names are the other way round)
  const LandmarkVersion LandmarkVersions[] = {
    { 1907539190u, Landmark_ShardBackdrop, true, "the Shard's far-away model, whole (SkyCity_Backdrop)" },
    { 1887457432u, Landmark_ShardBackdrop, false, "the Shard's far-away model, ruined (SkyCity_RuinBackdrop)" },
    { 1907684677u, Landmark_Bauble, true, "the Bauble mall (BaubleMall)" },
    { 1908867187u, Landmark_Bauble, false, "the Bauble's collapse (BaubleDestruction)" },
    { 1892491847u, Landmark_Bauble, false, "the Bauble's ruins (BaubleRuins)" },
    { 1900523863u, Landmark_TopBlinds, true, "Kruger's office, blinds open (SkyCityTopOpenBlinds)" },
    { 1910111990u, Landmark_TopBlinds, false, "Kruger's office, blinds closed (SkyCityTopClosedBlinds)" } };

  struct CameraTriggerSeen
  {
    __int64 Entity;
    unsigned int Flags;   // its prefab reference's (0: none found)
    int Offset;           // where in its bus
    bool On;              // its update registered
  };

  bool CameraTriggerCodeKnown()
  {
    BYTE const* pBase = (BYTE const*)g_gameHandle;
    // push rdi; sub rsp, 0x40; ...; mov rdi, rcx; mov eax, [rdx + 8]; cmp eax, 0x8BA423E4
    static const BYTE code[] = { 0x57, 0x48, 0x83, 0xEC, 0x40, 0x48, 0xC7, 0x44, 0x24, 0x20, 0xFE, 0xFF, 0xFF, 0xFF, 0x48,
      0x89, 0x5C, 0x24, 0x50, 0x48, 0x89, 0x6C, 0x24, 0x60, 0x48, 0x89, 0x74, 0x24, 0x68, 0x48, 0x89, 0xCF, 0x8B, 0x42, 0x08,
      0x3D, 0xE4, 0x23, 0xA4, 0x8B };
    static const BYTE disable[] = { 0x3D, 0xB1, 0x16, 0x02, 0xF9 };   // cmp eax, 0xF90216B1
    static const BYTE registered[] = { 0x80, 0x79, 0x69, 0x00 };       // cmp byte [rcx + 0x69], 0
    // the set-up: push rbx; sub rsp, 0x30; mov rbx, rcx; call; mov rax, [rbx + 0x28]; cmp byte [rax + 0x27], 0 (AutoStart)
    static const BYTE init[] = { 0x53, 0x48, 0x83, 0xEC, 0x30, 0x48, 0x89, 0xCB, 0xE8 };
    static const BYTE autoStart[] = { 0x48, 0x8B, 0x43, 0x28, 0x80, 0x78, 0x27, 0x00 };
    BYTE const* p = pBase + CameraTriggerEventRva;
    BYTE const* vtable = pBase + CameraTriggerVTableRva;
    return !memcmp(p, code, sizeof(code)) && !memcmp(p + 0x2E, disable, sizeof(disable)) &&
      !memcmp(p + 0x39, registered, sizeof(registered)) &&
      !memcmp(pBase + CameraTriggerInitRva, init, sizeof(init)) &&
      !memcmp(pBase + CameraTriggerInitRva + 0xD, autoStart, sizeof(autoStart)) &&
      *(__int64 const*)vtable == (__int64)pBase + CameraTriggerTypeRva &&
      *(__int64 const*)(vtable + 4 * 8) == (__int64)p &&
      *(__int64 const*)(vtable + 15 * 8) == (__int64)pBase + CameraTriggerInitRva &&
      *(__int64 const*)(pBase + PrefabReferenceVTableRva) == (__int64)pBase + PrefabReferenceTypeRva;
  }

  // CameraTriggerCodeKnown, read once before the set-up is hooked: the hook's
  // jump takes the place of the set-up's first bytes, so read after that the
  // check fails (it did, build 120a8c6f160f, 2026-10-25 20:49: nothing switched).
  // -1: not checked yet; 0: not the code the tools know, or its hook couldn't
  // be made; 1: known and hooked. Given by util::hooks::Init, on its own
  // thread, only once the set-up's hook is made: the CameraUpdate hook is made
  // before it there, so the camera's update (UpdateLandmarkTriggers, through
  // CameraManager) can run first and reads -1 meanwhile, which is start-up,
  // not a failure (read as a plain false, that race gave a warning that the
  // triggers' code wasn't known, though later in the same run their streaming
  // switched, and the blinds' first line was never logged)
  std::atomic<int> g_CameraTriggerCodeOk{ -1 };

  // The camera area triggers the game has set up (some may be gone since)
  std::mutex g_NotedTriggersMutex;
  std::vector<__int64> g_NotedTriggers;   // under g_NotedTriggersMutex

  // One noted trigger as it is now: false if it isn't one any more
  bool ReadCameraTrigger(__int64 entity, CameraTriggerSeen& trigger)
  {
    __int64 base = (__int64)g_gameHandle;
    __try
    {
      if (*(__int64*)entity != base + CameraTriggerVTableRva)
        return false;
      trigger = { entity, 0, 0, *(BYTE*)(entity + TriggerRegisteredOffset) != 0 };
      __int64 bus = *(__int64*)(entity + TriggerBusOffset);
      if (bus < 0x10000)
        return true;
      for (int offset : BusReferenceOffsets)
      {
        __int64 reference = *(__int64*)(bus + offset) & ~3ll;
        if (reference >= 0x10000 && *(__int64*)reference == base + PrefabReferenceVTableRva)
        {
          trigger.Flags = *(unsigned int*)(reference + PeerFlagsOffset);
          trigger.Offset = offset;
          break;
        }
      }
      return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      return false;
    }
  }

  // The noted triggers that are still there, with their prefab references'
  // Flags; the rest are forgotten
  int ReadCameraTriggers(CameraTriggerSeen* seen, int max)
  {
    std::lock_guard<std::mutex> lock(g_NotedTriggersMutex);
    int count = 0;
    g_NotedTriggers.erase(std::remove_if(g_NotedTriggers.begin(), g_NotedTriggers.end(), [&](__int64 entity) {
      CameraTriggerSeen trigger;
      if (!ReadCameraTrigger(entity, trigger))
        return true;
      if (count < max)
        seen[count++] = trigger;
      return false;
    }), g_NotedTriggers.end());
    return count;
  }

  // Enable or Disable through the trigger's own handler
  bool SendTriggerEvent(__int64 entity, unsigned int id)
  {
    __declspec(align(16)) BYTE event[0x40] = {};
    *(unsigned int*)(event + 8) = id;
    __try
    {
      if (*(__int64*)entity != (__int64)g_gameHandle + CameraTriggerVTableRva)
        return false;
      typedef void(__fastcall* tTriggerEvent)(__int64, void*);
      ((tTriggerEvent)((__int64)g_gameHandle + CameraTriggerEventRva))(entity, event);
      return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      return false;
    }
  }

  struct LandmarkChange
  {
    __int64 Entity;
    unsigned int Flags;
    bool GameOn;   // what the game had before the tools switched it
  };
  std::vector<LandmarkChange> g_LandmarkChanges;   // camera hook only
}

// Each camera area trigger, as the game sets it up
typedef __int64(__fastcall* tCameraTriggerInit)(__int64, __int64, __int64, __int64);
tCameraTriggerInit oCameraTriggerInit = nullptr;
__int64 __fastcall hCameraTriggerInit(__int64 entity, __int64 a2, __int64 a3, __int64 a4)
{
  __int64 result = oCameraTriggerInit(entity, a2, a3, a4);
  if (!g_shutdown)
  {
    std::lock_guard<std::mutex> lock(g_NotedTriggersMutex);
    if (g_NotedTriggers.size() < MaxNotedTriggers &&
      std::find(g_NotedTriggers.begin(), g_NotedTriggers.end(), entity) == g_NotedTriggers.end())
      g_NotedTriggers.push_back(entity);
  }
  return result;
}

namespace
{
  // A landmark version's camera trigger (its prefab reference's Flags): on 1, off 0, not set up (yet) -1
  int LandmarkTriggerOn(unsigned int flags)
  {
    if (g_CameraTriggerCodeOk.load() != 1)
      return -1;
    CameraTriggerSeen seen[MaxCameraTriggers];
    int count = ReadCameraTriggers(seen, MaxCameraTriggers);
    for (int i = 0; i < count; ++i)
      if (seen[i].Flags == flags)
        return seen[i].On ? 1 : 0;
    return -1;
  }
}

int util::hooks::BaubleRuinsTrigger()
{
  return LandmarkTriggerOn(1892491847u);   // LandmarkVersions: the Bauble's ruins (BaubleRuins)
}

int util::hooks::BaubleMallTrigger()
{
  return LandmarkTriggerOn(1907684677u);   // LandmarkVersions: the Bauble mall (BaubleMall)
}

void util::hooks::UpdateLandmarkTriggers(bool unbroken, bool shardBackdropOut, int topBlinds)
{
  // Not checked yet (start-up, see g_CameraTriggerCodeOk): nothing done and
  // nothing kept, so the first pass once it's known sees the settings as
  // changes and logs them
  int codeOk = g_CameraTriggerCodeOk.load();
  if (codeOk < 0)
    return;
  // Not the code the tools know (or not hooked): Init's warning says what
  // can't switch. The one thing only seen here, said once: the Shard's
  // far-away sub-level wanted out (its fans can't be moved onto the real
  // tower, see CameraManager, whose line says it streams out instead) but
  // staying, so those fans stay in view up close
  if (codeOk == 0)
  {
    static bool s_backdropWarned = false;
    if (shardBackdropOut && !s_backdropWarned)
    {
      s_backdropWarned = true;
      util::log::Warning("The Shard: its far-away sub-level can't be streamed out after all (the camera triggers can't be "
        "switched, see the camera area triggers' set-up warning at start-up): its far-away fans stay in view up close");
    }
    return;
  }
  static ULONGLONG s_last = 0;
  static bool s_unbroken = false, s_out = false, s_logged = false;
  static int s_blinds = 0;
  ULONGLONG now = GetTickCount64();
  bool changed = unbroken != s_unbroken || shardBackdropOut != s_out || topBlinds != s_blinds;
  if (!changed && now - s_last < LandmarkCheckMs)
    return;
  s_last = now;
  if (unbroken != s_unbroken)
    util::log::Write("Unbroken Shard and Bauble %s", unbroken ? "on: their whole versions' streaming triggers on, the "
      "broken ones' off" : "off: their streaming triggers back as the game had them");
  if (shardBackdropOut != s_out)
    util::log::Write(shardBackdropOut ? "The Shard: its real tower is loaded and its far-away fans can't be moved onto it: "
      "its far-away sub-level (the far-away tower, its fans and smoke) streams out, as the game does when Faith leaves the "
      "city" : "The Shard: the real tower isn't shown in its place any more: its far-away sub-level streams back in");
  if (topBlinds != s_blinds)
    util::log::Write("Kruger's office blinds: %s", topBlinds == 1 ? "open (the game's SkyCityTopOpenBlinds)" :
      topBlinds == 2 ? "closed (the game's SkyCityTopClosedBlinds)" : "as the game has them");
  s_unbroken = unbroken;
  s_out = shardBackdropOut;
  s_blinds = topBlinds;
  if (!unbroken && !shardBackdropOut && !topBlinds && g_LandmarkChanges.empty())
    return;

  CameraTriggerSeen seen[MaxCameraTriggers];
  int count = ReadCameraTriggers(seen, MaxCameraTriggers);
  auto versionOf = [](unsigned int flags) {
    for (int v = 0; v < (int)_countof(LandmarkVersions); ++v)
      if (LandmarkVersions[v].Flags == flags) return v;
    return -1;
  };

  // Forgotten: the triggers that went (a level load)
  g_LandmarkChanges.erase(std::remove_if(g_LandmarkChanges.begin(), g_LandmarkChanges.end(),
    [&](LandmarkChange const& change) {
      for (int i = 0; i < count; ++i)
        if (seen[i].Entity == change.Entity && seen[i].Flags == change.Flags) return false;
      return true;
    }), g_LandmarkChanges.end());

  // Each version's trigger: off while the Shard's far-away sub-level is to be
  // out (for its two), else the whole ones on and the broken ones off with
  // the tick, else what the game had before the tools switched it
  int found = 0;
  bool versions[_countof(LandmarkVersions)]{};
  std::vector<__int64> settled;   // back as the game had them: the game's again
  for (int i = 0; i < count; ++i)
  {
    int v = versionOf(seen[i].Flags);
    if (v < 0)
      continue;
    ++found;
    versions[v] = true;
    LandmarkChange* pChange = nullptr;
    for (LandmarkChange& change : g_LandmarkChanges)
      if (change.Entity == seen[i].Entity) pChange = &change;
    bool gameOn = pChange ? pChange->GameOn : seen[i].On;
    LandmarkVersion const& version = LandmarkVersions[v];
    bool want = gameOn;
    if (version.Kind == Landmark_TopBlinds)
      want = topBlinds == 1 ? version.Whole : topBlinds == 2 ? !version.Whole : gameOn;
    else if (version.Kind == Landmark_ShardBackdrop && shardBackdropOut)
      want = false;
    else if (unbroken)
      want = version.Whole;
    bool on = seen[i].On;
    if (on != want)
    {
      if (!pChange)
        g_LandmarkChanges.push_back({ seen[i].Entity, seen[i].Flags, seen[i].On });
      if (SendTriggerEvent(seen[i].Entity, want ? TriggerEnable : TriggerDisable))
      {
        on = want;
        util::log::Write("Streaming triggers: %s %s (trigger 0x%I64X)", LandmarkVersions[v].Name,
          want ? "switched on: the game streams it in where its camera is" : "switched off: the game streams it out",
          seen[i].Entity);
      }
    }
    if (want == gameOn && on == gameOn)
      settled.push_back(seen[i].Entity);
  }
  g_LandmarkChanges.erase(std::remove_if(g_LandmarkChanges.begin(), g_LandmarkChanges.end(),
    [&](LandmarkChange const& change) {
      return std::find(settled.begin(), settled.end(), change.Entity) != settled.end();
    }), g_LandmarkChanges.end());

  // What was seen, once, when a version wasn't found (for telling them apart).
  // Not before the level has set up any trigger: a setting on in the config
  // is read before that (all missing at 21:52:26, all found 13 s later)
  if (count > 0 && found < (int)_countof(LandmarkVersions) && !s_logged)
  {
    s_logged = true;
    util::log::Write("Streaming triggers: %d of the %d for the Shard (far-away model, top) and the Bauble found", found,
      (int)_countof(LandmarkVersions));
    for (int v = 0; v < (int)_countof(LandmarkVersions); ++v)
      if (!versions[v])
        util::log::Write("Streaming triggers: no trigger found for %s (Flags %u)", LandmarkVersions[v].Name,
          LandmarkVersions[v].Flags);
    for (int i = 0; i < count && i < 24; ++i)
      util::log::Write("  camera trigger 0x%I64X: %s, prefab reference Flags %u (bus + 0x%X)", seen[i].Entity,
        seen[i].On ? "on" : "off", seen[i].Flags, seen[i].Offset);
  }
}

void util::hooks::MissionItemsStartRecording()
{
  std::lock_guard<std::mutex> lock(g_MemberMutex);
  g_MemberChanges.clear();
  g_MemberTracks.clear();
  g_MemberDropped = 0;
  g_MemberClock = 0.0;
  g_MemberAtStart = g_MemberLive;
  g_MemberRecording = oGroupProperty != nullptr && oGroupDtor != nullptr;
}

void util::hooks::MissionItemsRecordFrame(double time)
{
  std::lock_guard<std::mutex> lock(g_MemberMutex);
  g_MemberClock = time;
}

void util::hooks::MissionItemsStopRecording()
{
  std::lock_guard<std::mutex> lock(g_MemberMutex);
  if (!g_MemberRecording)
    return;
  g_MemberRecording = false;
  std::unordered_map<MemberKey, size_t, MemberKeyHash> index;
  int visible = 0, variation = 0;
  for (MemberChange const& c : g_MemberChanges)
  {
    if (!c.Key.Group)
      continue;   // its group was destroyed
    ++((c.Key.Hash & 3) == MemberVisible ? visible : variation);
    auto it = index.find(c.Key);
    if (it == index.end())
    {
      MemberTrack t = { c.Key, false, 0, {}, false };
      auto before = g_MemberAtStart.find(c.Key);
      if (before != g_MemberAtStart.end())
      {
        t.HasBefore = true;
        t.Before = before->second;
      }
      it = index.emplace(c.Key, g_MemberTracks.size()).first;
      g_MemberTracks.push_back(t);
    }
    g_MemberTracks[it->second].Changes.push_back({ c.Time, c.Value });
  }
  std::unordered_set<__int64> groups;
  for (MemberTrack const& t : g_MemberTracks)
    groups.insert(t.Key.Group);
  util::log::Write("Replay: the game showed or hid models %d times and switched their looks %d times while it recorded "
    "(%d members of %d model groups; %d more left out: no room)", visible, variation, (int)g_MemberTracks.size(),
    (int)groups.size(), g_MemberDropped);
  g_MemberChanges.clear();
  g_MemberChanges.shrink_to_fit();
  g_MemberAtStart.clear();
}

void util::hooks::MissionItemsPlay(double time)
{
  struct Give
  {
    MemberKey Key;
    MemberValue Value;
    bool Back;   // the game's own value, given back
  };
  static std::vector<Give> gives;   // game thread
  gives.clear();
  {
    std::lock_guard<std::mutex> lock(g_MemberMutex);
    for (MemberTrack const& t : g_MemberTracks)
    {
      if (t.Gone)
        continue;
      auto at = std::upper_bound(t.Changes.begin(), t.Changes.end(), time,
        [](double tm, std::pair<double, MemberValue> const& c) { return tm < c.first; });
      auto applied = g_MemberApplied.find(t.Key);
      if (at != t.Changes.begin() || t.HasBefore)
      {
        MemberValue value = at != t.Changes.begin() ? std::prev(at)->second : t.Before;
        if (applied == g_MemberApplied.end() || applied->second != value)
          gives.push_back({ t.Key, value, false });
      }
      else if (applied != g_MemberApplied.end())
      {
        // Before its first change, and what it had then isn't known: as the game has it
        auto live = g_MemberLive.find(t.Key);
        if (live != g_MemberLive.end() && live->second != applied->second)
          gives.push_back({ t.Key, live->second, true });
        g_MemberApplied.erase(applied);
      }
    }
  }
  for (Give const& g : gives)
  {
    bool ok = SetMember(g.Key, g.Value);
    std::lock_guard<std::mutex> lock(g_MemberMutex);
    ++(ok ? g_MemberSets : g_MemberSetsFailed);
    if (ok && !g.Back)
      g_MemberApplied[g.Key] = g.Value;
    else if (!ok)
      for (MemberTrack& t : g_MemberTracks)
        if (t.Key == g.Key)
          t.Gone = true;
  }
}

void util::hooks::MissionItemsStopPlaying()
{
  std::vector<std::pair<MemberKey, MemberValue>> back;
  int sets = 0, failed = 0;
  {
    std::lock_guard<std::mutex> lock(g_MemberMutex);
    sets = g_MemberSets;
    failed = g_MemberSetsFailed;
    g_MemberSets = g_MemberSetsFailed = 0;
    for (auto const& a : g_MemberApplied)
    {
      auto live = g_MemberLive.find(a.first);
      if (live != g_MemberLive.end())
      {
        if (live->second != a.second)
          back.push_back({ a.first, live->second });
        continue;
      }
      // Not known (no room): as the recording left it
      for (MemberTrack const& t : g_MemberTracks)
        if (t.Key == a.first && !t.Gone && !t.Changes.empty() && t.Changes.back().second != a.second)
          back.push_back({ a.first, t.Changes.back().second });
    }
    g_MemberApplied.clear();
  }
  int given = 0;
  for (auto const& b : back)
    given += SetMember(b.first, b.second) ? 1 : 0;
  if (sets || failed || given)
    util::log::Write("Replay: models shown, hidden or switched as they were %d times while it played (%d not: their "
      "group gone), %d given back as the game has them", sets, failed, given);
}

// Diagnostic (temporary, 2026-10-25 after the 15:2x test): mission items that
// are single static models, not model group members. The 15:30 recording
// had the game show or hide group members 0 times (the group hook above)
// while the collectables Faith took were hidden: an electric box's key, in
// objects/props/missionitem/collectableobject01/pf_collectable_electricbox
// (game data), is an ObjectReference whose Visible comes from a BoolEntity
// (True) the take's timeline sets False when it ends, and it's moved by the
// prefab's animatable (BlueprintTransform), so the game builds it as a
// single ClientStaticModelEntity (vtable 0x141AFB520; the replay recorded it
// moving, 'collectable_electricbox_key'). Its look switch (the box's base,
// ObjectVariationSwitch -> ObjectVariation) did go through a group (1 switch
// recorded). A single static model doesn't take Visible in its property
// handler (0x14318DA10: Transform 0x87525469, ObjectVariation, BlueprintTransform,
// 0x0608E888 only); its slot 1, 0x14318E400(entity, hash), binds Visible
// (0x35BC69AB) to a reader at entity + 0x148 through 0x1446C07D0 ->
// 0x142A46F80: + 0x148 the connected value's cell (0 when not connected),
// + 0x150 the data's default (data + 0xDC). The game's reader 0x142A424B0
// takes [cell] when the cell's byte + 0x1C has 0x40, else + 0x150. What reads
// that reader to show or hide the model wasn't found (no read of + 0x148 /
// + 0x150 as a reader in the image's code, by raw scans). This reads it the
// way 0x142A424B0 does, for the log: whether a recorded item's Visible went
// to 0 while it recorded and is 0 when it plays, before anything is set
// through it.
namespace
{
  const __int64 StaticModelVisibleBindRva = 0x318E400;
  const __int64 PropertyReaderGetRva = 0x2A424B0;
  const int StaticModelVisibleReaderOffset = 0x148;
  const int PropertyCellFlagsOffset = 0x1C;
  const BYTE PropertyCellConnectedFlag = 0x40;

  bool IsStaticModelVisibleCode(BYTE const* pBase)
  {
    // cmp edx, 0x35BC69AB; jne
    static const BYTE bind[] = { 0x81, 0xFA, 0xAB, 0x69, 0xBC, 0x35, 0x75 };
    // movups xmm0, [rax]; movups [rbx + 0x148], xmm0
    static const BYTE store[] = { 0x0F, 0x10, 0x00, 0x0F, 0x11, 0x83, 0x48, 0x01, 0x00, 0x00 };
    // mov rax, [rcx]; test rax, rax; je; test byte [rax + 0x1C], 0x40; je; mov rax, [rax]; ret; ... mov rax, [rcx + 8]; ret
    static const BYTE get[] = { 0x48, 0x8B, 0x01, 0x48, 0x85, 0xC0, 0x74, 0x0E, 0xF6, 0x40, 0x1C, 0x40, 0x74, 0x08, 0x48, 0x8B,
      0x00, 0xC3 };
    static const BYTE getDefault[] = { 0x48, 0x8B, 0x41, 0x08, 0xC3 };
    return !memcmp(pBase + StaticModelVisibleBindRva, bind, sizeof(bind)) &&
      !memcmp(pBase + StaticModelVisibleBindRva + 0x31, store, sizeof(store)) &&
      !memcmp(pBase + PropertyReaderGetRva, get, sizeof(get)) &&
      !memcmp(pBase + PropertyReaderGetRva + 0x16, getDefault, sizeof(getDefault));
  }
}

// The reader's cell (+ 0x148: 0 when not connected) and the value it gives;
// false when not a single static model (see ReadStaticModelVisible)
bool util::hooks::ReadStaticModelVisibleCell(__int64 entity, int& visible, __int64& cell)
{
  bool connected = false;
  if (!ReadStaticModelVisible(entity, visible, connected))
    return false;
  __try
  {
    cell = *(__int64 const*)(entity + StaticModelVisibleReaderOffset);
    return true;
  }
  __except (EXCEPTION_EXECUTE_HANDLER)
  {
    return false;
  }
}

// The replay's collected items (2026-10-25 evening): a single static model's
// reader pointed at another cell, only while it's still that model (its
// vtable's slot 1 the binder) and its reader still has the cell expected, so
// nothing is written into a model the game has made again since. A cell the
// replay gives is the game's layout as 0x142A424B0 reads it: + 0x00 a pointer
// to the value, byte + 0x1C with 0x40. The binder (0x14318E400) overwrites the
// reader with movups and releases nothing, so the reader owns no cell.
bool util::hooks::SwapStaticModelVisibleCell(__int64 entity, __int64 expected, __int64 cell)
{
  static int codeKnown = -1;
  if (codeKnown < 0)
    codeKnown = IsStaticModelVisibleCode((BYTE const*)g_gameHandle) ? 1 : 0;
  if (!codeKnown || entity < 0x10000)
    return false;
  __try
  {
    __int64 vtable = *(__int64 const*)entity;
    if (vtable < 0x10000 || *(__int64 const*)(vtable + 8) != (__int64)g_gameHandle + StaticModelVisibleBindRva)
      return false;
    __int64* reader = (__int64*)(entity + StaticModelVisibleReaderOffset);
    if (*reader != expected)
      return false;
    *reader = cell;
    return true;
  }
  __except (EXCEPTION_EXECUTE_HANDLER)
  {
    return false;
  }
}

bool util::hooks::ReadStaticModelVisible(__int64 entity, int& visible, bool& connected)
{
  static int codeKnown = -1;
  if (codeKnown < 0)
    codeKnown = IsStaticModelVisibleCode((BYTE const*)g_gameHandle) ? 1 : 0;
  if (!codeKnown || entity < 0x10000)
    return false;
  __try
  {
    // Only an entity whose slot 1 is that binder has the reader there
    __int64 vtable = *(__int64 const*)entity;
    if (vtable < 0x10000 || *(__int64 const*)(vtable + 8) != (__int64)g_gameHandle + StaticModelVisibleBindRva)
      return false;
    __int64 cell = *(__int64 const*)(entity + StaticModelVisibleReaderOffset);
    __int64 value = *(__int64 const*)(entity + StaticModelVisibleReaderOffset + 8);
    connected = cell > 0x10000 && (*(BYTE const*)(cell + PropertyCellFlagsOffset) & PropertyCellConnectedFlag);
    if (connected)
      value = *(__int64 const*)cell;
    if (value < 0x10000)
      return false;
    visible = *(BYTE const*)value;
    return true;
  }
  __except (EXCEPTION_EXECUTE_HANDLER)
  {
    return false;
  }
}

// The physics worlds' room for bodies. Each world (the client's, and the
// client effects') is made by 0x1434E4DA0(world, desc) from PhysicsSettings
// (0x1434F3440: desc + 0x64 = BodyBufferCapacity, + 0x68 MotionBufferCapacity,
// + 0x6C ConstraintBufferCapacity), and Havok's buffer gets desc + 0x64 + 1
// bodies (0x1434E51F2, 0x1434D11BA). PhysicsSettings' constructor
// (0x1446D8500) has 8192 bodies and motions, but the game runs with 16384
// bodies and 8192 motions (the log, 2026-10-25 06:59), and the handler it
// gives Havok for "buffer full" is the empty stub 0x142A1C590 (0x1434E5508,
// 0x1434E5529), so nothing makes more room: with the whole city loaded the
// next body (0x143F41880) got id 0x7FFFFFFF ("Body buffer is full.",
// 0x143F5E3E7) and Havok wrote it at bodies + id * 0x90, past the buffer
// (2026-10-25 06:25: write to 0x4A416A2D70 at +0x3F38375, the bodies at
// 0x2416A2E00). Each world gets at least WorldBodiesMost / WorldMotionsMost
// (0x90 bytes a body).
typedef __int64(__fastcall* tPhysicsWorldInit)(__int64, __int64);
tPhysicsWorldInit oPhysicsWorldInit = nullptr;

namespace
{
  const __int64 PhysicsWorldInitRva = 0x34E4DA0;
  const int WorldDescBodies = 0x64;
  const int WorldDescMotions = 0x68;
  const unsigned int WorldBodiesMost = 65535;   // + 1 in Havok's buffer
  // Moving bodies run out the same way (the same empty handler, "Motion
  // buffer is full." 0x141D5E130): as many
  const unsigned int WorldMotionsMost = 65535;
  // The client world's bodies (its Havok world + 0x10 is the body manager:
  // + 0x58 how many, + 0x60 the highest id given), for the log
  std::atomic<unsigned int> g_WorldBodiesRoom{ 0 };
  unsigned int g_WorldBodiesLogged = 0;   // the tools' loop

  bool RaiseWorldRoom(__int64 desc, unsigned int& bodies, unsigned int& motions)
  {
    __try
    {
      unsigned int* pBodies = (unsigned int*)(desc + WorldDescBodies);
      unsigned int* pMotions = (unsigned int*)(desc + WorldDescMotions);
      bodies = *pBodies;
      motions = *pMotions;
      if (*pBodies < WorldBodiesMost)
        *pBodies = WorldBodiesMost;
      if (*pMotions < WorldMotionsMost)
        *pMotions = WorldMotionsMost;
      return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      return false;
    }
  }

  bool ReadClientWorldBodies(unsigned int& count, unsigned int& highest)
  {
    __try
    {
      __int64 p = *(__int64 const*)((__int64)g_gameHandle + 0x2401CA0);
      if (p < 0x10000 || (p = *(__int64 const*)(p + 0x50)) < 0x10000 || (p = *(__int64 const*)(p + 0xF0)) < 0x10000 ||
          (p = *(__int64 const*)p) < 0x10000 || (p = *(__int64 const*)(p + 0x18)) < 0x10000 ||
          (p = *(__int64 const*)(p + 0x10)) < 0x10000)
        return false;
      count = *(unsigned int const*)(p + 0x10 + 0x58);
      highest = *(unsigned int const*)(p + 0x10 + 0x60);
      return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      return false;
    }
  }
}

__int64 __fastcall hPhysicsWorldInit(__int64 world, __int64 desc)
{
  unsigned int bodies = 0, motions = 0;
  if (RaiseWorldRoom(desc, bodies, motions))
  {
    unsigned int room = (bodies < WorldBodiesMost ? WorldBodiesMost : bodies) + 1;
    if (!g_WorldBodiesRoom.load())
      g_WorldBodiesRoom = room;
    util::log::Write("Physics: a world made with room for %u bodies and %u moving ones (the game's settings: %u and %u)", room,
      motions < WorldMotionsMost ? WorldMotionsMost : motions, bodies + 1, motions);
  }
  return oPhysicsWorldInit(world, desc);
}

static bool IsPhysicsWorldInitCode(BYTE const* pBase)
{
  // mov rax, rsp; mov [rax + 8], rcx; push rsi; push rdi; push r12; push r14; push r15; sub rsp, 0x80
  static const BYTE start[] = { 0x48, 0x89, 0xE0, 0x48, 0x89, 0x48, 0x08, 0x56, 0x57, 0x41, 0x54, 0x41, 0x56, 0x41, 0x57,
    0x48, 0x81, 0xEC, 0x80, 0x00, 0x00, 0x00 };
  // mov edi, [rsi + 0x64]; inc edi (the body capacity, desc + 0x64 + 1)
  static const BYTE bodies[] = { 0x8B, 0x7E, 0x64, 0xFF, 0xC7 };
  // in 0x1434F3440: mov eax, [rcx + 0x20]; mov [rbp - 5], eax; mov eax, [rcx + 0x24]; mov [rbp - 1], eax
  static const BYTE fromSettings[] = { 0x8B, 0x41, 0x20, 0x89, 0x45, 0xFB, 0x8B, 0x41, 0x24, 0x89, 0x45, 0xFF };
  return !memcmp(pBase + PhysicsWorldInitRva, start, sizeof(start)) && !memcmp(pBase + 0x34E51F2, bodies, sizeof(bodies)) &&
    !memcmp(pBase + 0x34F3474, fromSettings, sizeof(fromSettings));
}

// Every second from the tools' loop: the client world's bodies, logged when
// they pass another tenth of its room
void util::hooks::LogPhysicsBodies()
{
  unsigned int room = g_WorldBodiesRoom.load();
  unsigned int count = 0, highest = 0;
  if (!room || !ReadClientWorldBodies(count, highest))
    return;
  unsigned int tenth = (unsigned int)((unsigned long long)highest * 10 / room);
  if (tenth > g_WorldBodiesLogged)
  {
    g_WorldBodiesLogged = tenth;
    util::log::Write("Physics: the client world has %u bodies (the highest id %u) of room for %u", count, highest, room);
  }
}

void util::hooks::LogLightMapIds()
{
  unsigned int refused = g_LightMapIdsRefused;
  unsigned int logged = g_LightMapIdsRefusedLogged.exchange(refused);
  if (refused != logged)
    util::log::Write("Enlighten's light map ids are all in use (%u, the game counts them in 16 bits): %u asked for "
      "in all were turned down, those pieces go without bounce light", (unsigned int)LightMapSlotsMost, refused);
}

// The sun shadows' reach. Each view's sun shadow setup (0x1437356A0: the
// world renderer, its + 0x3C1708, the view's context = world renderer + 0xB00
// + view * 0x89400, called for each view by 0x14374B750) works out how far
// from the camera its cascades go (0x143735CA1): the view's far minus near
// plane (context + 0xCC0 - + 0xCBC), at most WorldRenderSettings'
// ShadowmapViewDistance (+ 0x64, the tools' Shadow distance; WorldRenderSettings
// is context + 0x882B8), at least 0; but a reach the level sets itself, its
// CameraParams' SunShadowmapViewDistance in the context's blend of the level's
// visual environments (context + 0x8552C, filled for each view from the
// visual environment manager, 0x14373E663), is taken instead whenever it's 0
// or more; then times context + 0x26C8 when WorldRenderSettings + 0x133. So
// with a level reach the tools' Shadow distance did nothing (2026-10-23: the
// shadows went at y 441 with it at 1500 m). With the tools' set, it's put
// over the level's for the call, and the level's put back after.
typedef __int64(__fastcall* tSunShadowSetup)(__int64, __int64, __int64, __int64);
tSunShadowSetup oSunShadowSetup = nullptr;

namespace
{
  const __int64 SunShadowSetupRva = 0x37356A0;
  const int ViewNearOffset = 0xCBC;
  const int ViewFarOffset = 0xCC0;
  const int ViewReachScaleOffset = 0x26C8;
  const int ViewLevelReachOffset = 0x8552C;
  const int ViewWorldRenderSettingsOffset = 0x882B8;
  const int WrsShadowDistanceOffset = 0x64;
  const int WrsReachScaleEnableOffset = 0x133;

  std::atomic<float> g_SunShadowReach{ 0.f };

  // Diagnostic (temporary): the inputs, the views seen first
  struct SunShadowInputs
  {
    __int64 View;
    float Near, Far, Wrs, LevelReach, Scale;
    bool ScaleOn;
    float Ours;        // what the tools put over the level's reach, 0 for nothing
  };
  const int SunShadowViews = 6;
  std::mutex g_SunShadowMutex;
  SunShadowInputs g_SunShadowSeen[SunShadowViews]{};
  SunShadowInputs g_SunShadowLogged[SunShadowViews]{};
  int g_SunShadowSeenCount = 0;
}

static bool ReadSunShadowInputs(__int64 view, SunShadowInputs& in)
{
  __try
  {
    BYTE const* v = (BYTE const*)view;
    BYTE const* wrs = *(BYTE const**)(v + ViewWorldRenderSettingsOffset);
    in.View = view;
    in.Near = *(float const*)(v + ViewNearOffset);
    in.Far = *(float const*)(v + ViewFarOffset);
    in.LevelReach = *(float const*)(v + ViewLevelReachOffset);
    in.Scale = *(float const*)(v + ViewReachScaleOffset);
    in.Wrs = *(float const*)(wrs + WrsShadowDistanceOffset);
    in.ScaleOn = wrs[WrsReachScaleEnableOffset] != 0;
    return true;
  }
  __except (EXCEPTION_EXECUTE_HANDLER)
  {
    return false;
  }
}

static void NoteSunShadowInputs(SunShadowInputs const& in)
{
  std::lock_guard<std::mutex> lock(g_SunShadowMutex);
  for (int i = 0; i < g_SunShadowSeenCount; ++i)
    if (g_SunShadowSeen[i].View == in.View)
    {
      g_SunShadowSeen[i] = in;
      return;
    }
  if (g_SunShadowSeenCount < SunShadowViews)
    g_SunShadowSeen[g_SunShadowSeenCount++] = in;
}

// The tools' reach over the level's, for the call: where it was put, or 0
static float* PutSunShadowReach(__int64 view, float reach, float& levelReach)
{
  __try
  {
    float* p = (float*)(view + ViewLevelReachOffset);
    levelReach = *p;
    if (!(levelReach >= 0.f))
      return nullptr;
    *p = reach;
    return p;
  }
  __except (EXCEPTION_EXECUTE_HANDLER)
  {
    return nullptr;
  }
}

static void PutBackSunShadowReach(float* p, float levelReach)
{
  __try { *p = levelReach; }
  __except (EXCEPTION_EXECUTE_HANDLER) {}
}

__int64 __fastcall hSunShadowSetup(__int64 pRenderer, __int64 a2, __int64 pView, __int64 a4)
{
  float reach = g_SunShadowReach.load(std::memory_order_relaxed);
  SunShadowInputs in{};
  bool read = util::DiagnosticsOn() && ReadSunShadowInputs(pView, in);   // for the log only
  float levelReach = -1.f;
  float* put = reach > 0.f ? PutSunShadowReach(pView, reach, levelReach) : nullptr;
  if (read)
  {
    in.Ours = put ? reach : 0.f;
    NoteSunShadowInputs(in);
  }
  __int64 result = oSunShadowSetup(pRenderer, a2, pView, a4);
  if (put)
    PutBackSunShadowReach(put, levelReach);
  return result;
}

// The setup as it was read: its start, where it takes the view's context and
// WorldRenderSettings, and the reach (0x143735CA1 - 0x143735CF1)
static bool IsSunShadowSetupCode(BYTE const* pSetup)
{
  static const BYTE start[] = { 0x55, 0x53, 0x48, 0x8D, 0xAC, 0x24, 0x68, 0xBB, 0xFF, 0xFF, 0xB8, 0xC8, 0x45, 0x00, 0x00 };
  static const BYTE context[] = { 0x4D, 0x8B, 0xB8, 0xB8, 0x82, 0x08, 0x00 };        // mov r15, [r8 + 0x882B8]
  static const BYTE keep[] = { 0x4D, 0x89, 0xC6 };                                   // mov r14, r8
  static const BYTE reach[] = { 0xF3, 0x45, 0x0F, 0x10, 0x9E, 0xC0, 0x0C, 0x00, 0x00, 0xF3, 0x41, 0x0F, 0x10, 0x47, 0x64,
    0xF3, 0x45, 0x0F, 0x5C, 0x9E, 0xBC, 0x0C, 0x00, 0x00, 0x41, 0x0F, 0x2F, 0xC3, 0x73, 0x04, 0x44, 0x0F, 0x28, 0xD8, 0x45,
    0x0F, 0x2F, 0xDC, 0x77, 0x04, 0x45, 0x0F, 0x28, 0xDC, 0xF3, 0x41, 0x0F, 0x10, 0x8E, 0x2C, 0x55, 0x08, 0x00, 0x41, 0x0F,
    0x2F, 0xCC, 0x72, 0x04, 0x44, 0x0F, 0x28, 0xD9, 0x41, 0x38, 0x87, 0x33, 0x01, 0x00, 0x00, 0x74, 0x09, 0xF3, 0x45, 0x0F,
    0x59, 0x9E, 0xC8, 0x26, 0x00, 0x00 };
  return !memcmp(pSetup, start, sizeof(start)) && !memcmp(pSetup + 0x72, context, sizeof(context)) &&
    !memcmp(pSetup + 0x89, keep, sizeof(keep)) && !memcmp(pSetup + 0x601, reach, sizeof(reach));
}

void util::hooks::SetSunShadowReach(float metres)
{
  g_SunShadowReach.store(metres > 0.f ? metres : 0.f, std::memory_order_relaxed);
}

void util::hooks::LogSunShadowReach()
{
  if (!util::DiagnosticsOn())
    return;
  SunShadowInputs seen[SunShadowViews];
  int count;
  {
    std::lock_guard<std::mutex> lock(g_SunShadowMutex);
    count = g_SunShadowSeenCount;
    memcpy(seen, g_SunShadowSeen, sizeof(seen));
  }
  auto differs = [](float a, float b) { return fabsf(a - b) > 0.01f * (std::max)(1.f, fabsf(b)); };
  for (int i = 0; i < count; ++i)
  {
    SunShadowInputs const& in = seen[i];
    SunShadowInputs& last = g_SunShadowLogged[i];
    if (last.View == in.View && !differs(in.Far - in.Near, last.Far - last.Near) && !differs(in.Wrs, last.Wrs) &&
        !differs(in.LevelReach, last.LevelReach) && !differs(in.Scale, last.Scale) && in.ScaleOn == last.ScaleOn &&
        !differs(in.Ours, last.Ours))
      continue;
    last = in;
    // The game's working (see above), with what the tools put in
    float reach = (std::max)((std::min)(in.Far - in.Near, in.Wrs), 0.f);
    float level = in.Ours > 0.f ? in.Ours : in.LevelReach;
    if (level >= 0.f)
      reach = level;
    if (in.ScaleOn)
      reach *= in.Scale;
    if (in.Wrs < 0.f)
      reach = -in.Wrs;
    util::log::Write("Sun shadows (diagnostic), view %d (0x%I64X): far - near %.0f m, WorldRenderSettings %.0f m, the level's own "
      "reach %.0f m%s, scale %.3f (%s): the cascades reach %.0f m%s", i, in.View, in.Far - in.Near, in.Wrs, in.LevelReach,
      in.LevelReach >= 0.f ? " (taken over WorldRenderSettings')" : " (none)", in.Scale, in.ScaleOn ? "on" : "off", reach,
      in.Ours > 0.f ? " (the tools' Shadow distance over the level's)" : "");
  }
}

// Lens flares far away. Each frame, for each view, the renderer works out
// every lens flare element's look (0x1436FD9E0; its elements are copies of
// LensFlareElement, 0xE0 each, made by 0x1436ED640): with d the camera's
// distance from the flare (xmm12, from 0x1436FE025, put back for each element
// at 0x1436FED16), each of its curves is a cubic (a t^3 + b t^2 + c t + d of
// the four floats) of t = d / SizeCamDistMax (+ 0x60, for the size curve at
// + 0x50) or d / AlphaCamDistMax (+ 0xB0, the alpha curve at + 0xA0),
// clamped to 0 - 1 (xmm9 = 0 and xmm11 = 1.0 then) - but t itself isn't:
// the curves are made for 0 - 1 (most are (1 - t)^3, gone at the distance),
// and past it some climb back to full (2026-10-23, live: 415 flares, curves
// up to 346 at five times their distance). The quad is in screen space, the
// element's Size times the curves, so a flare far away is as big on screen
// as close up: from high over the city they looked huge. With SetFarFlareFade
// on, three caves (one jump each, put in by MinHook, the code there checked
// first) hold t at 1, and fade the element's alpha out from AlphaCamDistMax
// to twice that; off, they do exactly what the game's code does.
namespace
{
  const __int64 FlareFunctionRva = 0x36FD9E0;
  const __int64 FlareOneLoadRva = 0x36FDACB;      // movss xmm11, [rip + ..] (1.0)
  const __int64 FlareZeroRva = 0x36FDE92;         // xorps xmm9, xmm9
  const __int64 FlareDistanceRva = 0x36FE021;     // xorps xmm12, xmm12; sqrtss xmm12, xmm3; movss [rsp + 0x60], xmm12
  const __int64 FlareAlphaTRva = 0x36FE259;       // movaps xmm1, xmm12; divss xmm1, [rdi + 0xB0]
  const __int64 FlareAlphaClampRva = 0x36FE29B;   // clamp xmm2 to xmm9 .. xmm11
  const __int64 FlareSizeTRva = 0x36FE34D;        // movaps xmm1, xmm12; divss xmm1, [rdi + 0x60]
  const __int64 FlareNextElementRva = 0x36FECEC;  // xorps xmm9, xmm9 ... movss xmm12, [rsp + 0x60] ... jmp the next element
  const int FlareCaveFlag = 0x100, FlareCaveOne = 0x104, FlareCaveTwo = 0x108;
  // No lens flare drawn while the tools' camera is on (2026-11-09, "Hide them
  // too"): past its occlusion read-back the function has counted the
  // elements to draw (edi) and goes to its draws, or, with none, to its own
  // way out (0x1436FDE7C test edi, edi; je 0x1436FED6C). A fourth cave at
  // its draws' start (the movabs at 0x1436FDE84, the game's own, done there
  // again) takes that way out while the byte at FlareCaveHide is set: no
  // vertex, constant or draw, and the read-back, the visible list and the
  // occlusion queries go on as the game runs them, so the next frame after
  // the camera goes off is the game's. The sun's flare is one of the
  // renderer's own flare entities, drawn here too. Hooked on its own: the
  // far fade keeps working if this can't be.
  const __int64 FlareDrawStartRva = 0x36FDE84;   // movabs rax, 0x0F80000000000000 (edi isn't 0 here)
  const __int64 FlareDrawEndRva = 0x36FED6C;     // lea rcx, [rbp + 0x110]; call (the way out with nothing to draw)
  const int FlareCaveHide = 0x101, FlareCaveSkip = 0x110;

  BYTE* g_FlareCave = nullptr;
  LPVOID g_FlareTrampolines[3] = {};
  LPVOID g_FlareSkipTrampoline = nullptr;
  bool g_FlareSkipHooked = false;
}

static bool IsFlareCode(BYTE const* pBase)
{
  static const BYTE start[] = { 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57 };
  static const BYTE oneLoad[] = { 0xF3, 0x44, 0x0F, 0x10, 0x1D };
  static const BYTE zero[] = { 0x45, 0x0F, 0x57, 0xC9 };
  static const BYTE distance[] = { 0x45, 0x0F, 0x57, 0xE4, 0xF3, 0x44, 0x0F, 0x51, 0xE3, 0xF3, 0x44, 0x0F, 0x11, 0x64, 0x24, 0x60 };
  static const BYTE alphaT[] = { 0x41, 0x0F, 0x28, 0xCC, 0xF3, 0x0F, 0x5E, 0x8F, 0xB0, 0x00, 0x00, 0x00 };
  static const BYTE alphaClamp[] = { 0x41, 0x0F, 0x2F, 0xD1, 0x73, 0x07, 0x41, 0x0F, 0x28, 0xD1, 0xEB, 0x0B, 0x95, 0x41, 0x0F,
    0x2F, 0xD3, 0x76, 0x04, 0x41, 0x0F, 0x28, 0xD3 };
  static const BYTE sizeT[] = { 0x41, 0x0F, 0x28, 0xCC, 0xF3, 0x0F, 0x5E, 0x4F, 0x60 };
  // xorps xmm9, xmm9 (0x36FECEC) and, 0x2A on, movss xmm12, [rsp + 0x60]; movss xmm13, [rsp + 0x50]; jmp 0x1436FE1B1
  static const BYTE nextElement[] = { 0xF3, 0x44, 0x0F, 0x10, 0x64, 0x24, 0x60, 0xF3, 0x44, 0x0F, 0x10, 0x6C, 0x24, 0x50, 0xE9,
    0x88, 0xF4, 0xFF, 0xFF };
  if (memcmp(pBase + FlareFunctionRva, start, sizeof(start)) || memcmp(pBase + FlareOneLoadRva, oneLoad, sizeof(oneLoad)) ||
      memcmp(pBase + FlareZeroRva, zero, sizeof(zero)) || memcmp(pBase + FlareDistanceRva, distance, sizeof(distance)) ||
      memcmp(pBase + FlareAlphaTRva, alphaT, sizeof(alphaT)) || memcmp(pBase + FlareAlphaClampRva, alphaClamp, sizeof(alphaClamp)) ||
      memcmp(pBase + FlareSizeTRva, sizeT, sizeof(sizeT)) || memcmp(pBase + FlareNextElementRva, zero, sizeof(zero)) ||
      memcmp(pBase + FlareNextElementRva + 0x2A, nextElement, sizeof(nextElement)))
    return false;
  // xmm11's constant is 1.0
  BYTE const* pLoad = pBase + FlareOneLoadRva;
  float const* pOne = (float const*)(pLoad + 9 + *(int const*)(pLoad + 5));
  return *pOne == 1.f;
}

// The draws' start and the way out the skip cave takes (see FlareDrawStartRva)
static bool IsFlareSkipCode(BYTE const* pBase)
{
  // test edi, edi; je 0x1436FED6C; movabs rax, 0x0F80000000000000
  static const BYTE drawStart[] = { 0x85, 0xFF, 0x0F, 0x84, 0xE8, 0x0E, 0x00, 0x00, 0x48, 0xB8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x80, 0x0F };
  static const BYTE drawEnd[] = { 0x48, 0x8D, 0x8D, 0x10, 0x01, 0x00, 0x00, 0xE8 };   // lea rcx, [rbp + 0x110]; call
  return !memcmp(pBase + FlareDrawStartRva - 8, drawStart, sizeof(drawStart)) &&
    FlareDrawStartRva + *(int const*)(pBase + FlareDrawStartRva - 4) == FlareDrawEndRva &&
    !memcmp(pBase + FlareDrawEndRva, drawEnd, sizeof(drawEnd));
}

// The caves (see above): at pCave + 0 the alpha t, + 0x40 the alpha clamp
// and fade, + 0xA0 the size t; the flag and the constants from FlareCaveFlag
static void BuildFlareCaves(BYTE* pCave, BYTE const* pBase)
{
  auto rel = [](BYTE const* pNext, BYTE const* pTarget) { return (int)(pTarget - pNext); };
  auto put = [](BYTE*& p, std::initializer_list<BYTE> bytes) { for (BYTE b : bytes) *p++ = b; };
  auto put32 = [](BYTE*& p, int v) { memcpy(p, &v, 4); p += 4; };
  auto jumpBack = [&](BYTE*& p, BYTE const* pTo) { put(p, { 0xFF, 0x25, 0x00, 0x00, 0x00, 0x00 }); __int64 a = (__int64)pTo; memcpy(p, &a, 8); p += 8; };
  BYTE* pFlag = pCave + FlareCaveFlag;
  BYTE* pOne = pCave + FlareCaveOne;
  BYTE* pTwo = pCave + FlareCaveTwo;

  // A t held at 1 with the flag on: the game's two instructions, then minss
  auto caveT = [&](BYTE* p, std::initializer_list<BYTE> divss, BYTE const* pBack)
  {
    put(p, { 0x41, 0x0F, 0x28, 0xCC });                                 // movaps xmm1, xmm12
    put(p, divss);                                                      // divss xmm1, [rdi + max]
    put(p, { 0x80, 0x3D }); put32(p, rel(p + 5, pFlag)); put(p, { 0x00 }); // cmp byte [flag], 0
    put(p, { 0x74, 0x08 });                                             // je (past the minss)
    put(p, { 0xF3, 0x0F, 0x5D, 0x0D }); put32(p, rel(p + 4, pOne));      // minss xmm1, [1.0]
    jumpBack(p, pBack);
  };
  caveT(pCave, { 0xF3, 0x0F, 0x5E, 0x8F, 0xB0, 0x00, 0x00, 0x00 }, pBase + FlareAlphaTRva + 12);
  caveT(pCave + 0xA0, { 0xF3, 0x0F, 0x5E, 0x4F, 0x60 }, pBase + FlareSizeTRva + 9);

  // The alpha clamped as the game does, then with the flag on times
  // clamp(2 - d / AlphaCamDistMax, 0, 1) (xmm0 and xmm1 are written before
  // they're read again after it)
  BYTE* p = pCave + 0x40;
  put(p, { 0x41, 0x0F, 0x2F, 0xD1, 0x73, 0x06, 0x41, 0x0F, 0x28, 0xD1, 0xEB, 0x0A });   // comiss xmm2, xmm9; jae; movaps xmm2, xmm9; jmp
  put(p, { 0x41, 0x0F, 0x2F, 0xD3, 0x76, 0x04, 0x41, 0x0F, 0x28, 0xD3 });               // comiss xmm2, xmm11; jbe; movaps xmm2, xmm11
  put(p, { 0x80, 0x3D }); put32(p, rel(p + 5, pFlag)); put(p, { 0x00 });               // cmp byte [flag], 0
  put(p, { 0x74, 0x26 });                                                                // je (to the jump back)
  put(p, { 0x41, 0x0F, 0x28, 0xCC });                                                   // movaps xmm1, xmm12
  put(p, { 0xF3, 0x0F, 0x5E, 0x8F, 0xB0, 0x00, 0x00, 0x00 });                           // divss xmm1, [rdi + 0xB0]
  put(p, { 0xF3, 0x0F, 0x10, 0x05 }); put32(p, rel(p + 4, pTwo));                        // movss xmm0, [2.0]
  put(p, { 0xF3, 0x0F, 0x5C, 0xC1 });                                                   // subss xmm0, xmm1
  put(p, { 0xF3, 0x41, 0x0F, 0x5F, 0xC1 });                                             // maxss xmm0, xmm9
  put(p, { 0xF3, 0x41, 0x0F, 0x5D, 0xC3 });                                             // minss xmm0, xmm11
  put(p, { 0xF3, 0x0F, 0x59, 0xD0 });                                                   // mulss xmm2, xmm0
  jumpBack(p, pBase + FlareAlphaClampRva + 0x17);

  // The draws skipped with the hide byte set (see FlareDrawStartRva)
  BYTE* pHide = pCave + FlareCaveHide;
  p = pCave + FlareCaveSkip;
  put(p, { 0x80, 0x3D }); put32(p, rel(p + 5, pHide)); put(p, { 0x00 });              // cmp byte [hide], 0
  put(p, { 0x75, 0x18 });                                                                // jne (to the way out)
  put(p, { 0x48, 0xB8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x0F });                // the game's movabs
  jumpBack(p, pBase + FlareDrawStartRva + 10);
  jumpBack(p, pBase + FlareDrawEndRva);                                                  // the way out
  *pHide = 0;

  *pFlag = 0;
  const float one = 1.f, two = 2.f;
  memcpy(pOne, &one, 4);
  memcpy(pTwo, &two, 4);
}

void util::hooks::SetFarFlareFade(bool on)
{
  if (g_FlareCave)
    *(volatile BYTE*)(g_FlareCave + FlareCaveFlag) = on ? 1 : 0;
}

// Every lens flare left out while the tools' camera is on (hCameraUpdate's,
// on the game thread; false once the tools shut down)
static void SetFlaresHidden(bool hidden)
{
  if (g_FlareCave && g_FlareSkipHooked)
    *(volatile BYTE*)(g_FlareCave + FlareCaveHide) = hidden ? 1 : 0;
}

// Replay guns and drones. A gun's (and a drone's) body is moved with its
// entity (ActorEntityData MoveAnimatableWithActor, + 0x98, is 1): each update
// bone 1 of its local pose is set to the entity's transform, and every other
// bone hangs off that (2026-10-23, live: all 26 bones of a gun), so the
// replay's override of the body alone left the guns where the live ones were
// (on the ground). So while it plays the replay moves the entities
// themselves, the way it moves doors (see ReplayModels): the static models'
// setter 0x1432E8300 (mov r8b, 1; jmp 0x1432EB1F0, also what the gun's own
// slot 25 0x143903F10 ends in), with the entity's flag bits 21 (queued) and 27
// (physics) cleared for the call, so what's drawn moves and no collision does.
// The game's own calls of that setter for those entities (a held gun follows
// its hand) are held back while they're the replay's, so they stay where it
// puts them.
typedef __int64(__fastcall* tModelSetTransform)(__int64, float const*);
tModelSetTransform oModelSetTransform = nullptr;

namespace
{
  const __int64 ModelSetTransformRva = 0x32E8300;
  const int ActorFlagsOffset = 0x18;
  const unsigned int ActorDeferredFlag = 1u << 21;
  const unsigned int ActorPhysicsFlag = 1u << 27;
  const int MaxHeldActors = 256;
  std::atomic<__int64> g_HeldActors[MaxHeldActors];
  std::atomic<int> g_HeldActorCount{ 0 };
  std::atomic<int> g_ActorMovesHeld{ 0 };
  // GAME > Pose's (a posed character's weapons): a list of its own, so the
  // replay's ReleaseActors and the pose's let-go never take the other's
  const int MaxPoseHeldActors = 32;
  std::atomic<__int64> g_PoseHeldActors[MaxPoseHeldActors];
  std::atomic<int> g_PoseHeldActorCount{ 0 };
  std::atomic<int> g_PoseActorMovesHeld{ 0 };
  thread_local bool t_MovingActor = false;
}

static bool IsInList(std::atomic<__int64> const* list, std::atomic<int> const& listed, __int64 entity)
{
  int count = listed.load(std::memory_order_acquire);
  for (int i = 0; i < count; ++i)
    if (list[i].load(std::memory_order_relaxed) == entity)
      return true;
  return false;
}

static bool AnyHeldActors()
{
  return g_HeldActorCount.load(std::memory_order_relaxed) || g_PoseHeldActorCount.load(std::memory_order_relaxed);
}

// Held by the replay or the pose; the count of held-back moves is the one's
// that holds it
static bool HoldsBack(__int64 entity)
{
  if (IsInList(g_HeldActors, g_HeldActorCount, entity))
  {
    g_ActorMovesHeld.fetch_add(1, std::memory_order_relaxed);
    return true;
  }
  if (IsInList(g_PoseHeldActors, g_PoseHeldActorCount, entity))
  {
    g_PoseActorMovesHeld.fetch_add(1, std::memory_order_relaxed);
    return true;
  }
  return false;
}

__int64 __fastcall hModelSetTransform(__int64 entity, float const* transform)
{
  if (!t_MovingActor && AnyHeldActors() && HoldsBack(entity))
    return 0;
  return oModelSetTransform(entity, transform);
}

static bool IsModelSetTransformCode(BYTE const* pBase)
{
  BYTE const* p = pBase + ModelSetTransformRva;
  __int64 target = ModelSetTransformRva + 3 + 5 + *(int const*)(p + 4);
  return p[0] == 0x41 && p[1] == 0xB0 && p[2] == 0x01 && p[3] == 0xE9 && target == 0x32EB1F0;
}

static void SetBodyHook(bool on);

// Only the game's thread that runs the replay changes the list
void util::hooks::HoldActor(__int64 entity, bool hold)
{
  int count = g_HeldActorCount.load(std::memory_order_relaxed);
  for (int i = 0; i < count; ++i)
    if (g_HeldActors[i].load(std::memory_order_relaxed) == entity)
    {
      if (!hold)
      {
        // The last one in its place, then one fewer
        g_HeldActors[i].store(g_HeldActors[count - 1].load(std::memory_order_relaxed), std::memory_order_relaxed);
        g_HeldActorCount.store(count - 1, std::memory_order_release);
        if (count == 1)
          SetBodyHook(AnyHeldActors());
      }
      return;
    }
  if (hold && count < MaxHeldActors)
  {
    g_HeldActors[count].store(entity, std::memory_order_relaxed);
    g_HeldActorCount.store(count + 1, std::memory_order_release);
    SetBodyHook(true);
  }
}

int util::hooks::ReleaseActors()
{
  g_HeldActorCount.store(0, std::memory_order_release);
  SetBodyHook(AnyHeldActors());
  return g_ActorMovesHeld.exchange(0);
}

// The pose's list, the same way (the game's thread only)
bool util::hooks::PoseHoldActor(__int64 entity, bool hold)
{
  int count = g_PoseHeldActorCount.load(std::memory_order_relaxed);
  for (int i = 0; i < count; ++i)
    if (g_PoseHeldActors[i].load(std::memory_order_relaxed) == entity)
    {
      if (!hold)
      {
        g_PoseHeldActors[i].store(g_PoseHeldActors[count - 1].load(std::memory_order_relaxed), std::memory_order_relaxed);
        g_PoseHeldActorCount.store(count - 1, std::memory_order_release);
        if (count == 1)
          SetBodyHook(AnyHeldActors());
      }
      return true;
    }
  if (!hold)
    return true;
  if (count >= MaxPoseHeldActors)
    return false;
  g_PoseHeldActors[count].store(entity, std::memory_order_relaxed);
  g_PoseHeldActorCount.store(count + 1, std::memory_order_release);
  SetBodyHook(true);
  return true;
}

int util::hooks::PoseActorMovesHeld()
{
  return g_PoseActorMovesHeld.exchange(0);
}

static bool MoveActorRows(__int64 entity, float const* transform)
{
  unsigned int* flags = (unsigned int*)(entity + ActorFlagsOffset);
  unsigned int before = 0;
  bool cleared = false;
  __try
  {
    before = *flags;
    *flags = before & ~(ActorDeferredFlag | ActorPhysicsFlag);
    cleared = true;
    oModelSetTransform(entity, transform);
    *flags = (*flags & ~(ActorDeferredFlag | ActorPhysicsFlag)) | (before & (ActorDeferredFlag | ActorPhysicsFlag));
    return true;
  }
  __except (EXCEPTION_EXECUTE_HANDLER)
  {
    if (cleared)
      *flags = (*flags & ~(ActorDeferredFlag | ActorPhysicsFlag)) | (before & (ActorDeferredFlag | ActorPhysicsFlag));
    return false;
  }
}

bool util::hooks::IsModelSetTransformHooked()
{
  return oModelSetTransform != nullptr;
}

bool util::hooks::MoveActorDrawn(__int64 entity, float const* transform)
{
  if (!oModelSetTransform)
    return false;
  // The setter reads the rows with movaps: 16-byte aligned
  alignas(16) float rows[16];
  memcpy(rows, transform, sizeof(rows));
  t_MovingActor = true;
  bool moved = MoveActorRows(entity, rows);
  t_MovingActor = false;
  return moved;
}

// Replay vehicles (a KrugerSec VTOL is a ClientVehicleEntity, vtable
// 0x141B12D80): their slot 25 is the same setter, 0x1432E8300, but their slots
// 15 (0x1431C7E20) and 35 (0x1431CAFE0) reach its body 0x1432EB1F0 (entity,
// transform, notify) directly, through 0x1432E2E80, past the hook above. So
// the body is hooked too, and holds the game's own moves of a held entity that
// way as well. The vehicle's change handler (slot 28, 0x14324C810) puts its
// physics body there too when notify is on and there's no queue (the
// setter's own call: 0x1434F8930 on its physics component, + 0x60), then
// moves what hangs off it either way (+ 0x448 / + 0x450 / + 0x470 / + 0x474,
// then the base's 0x143216A70). The replay moves a vehicle with notify off:
// what's drawn moves, its physics body doesn't (offline, 2026-10-24).
typedef __int64(__fastcall* tModelSetTransformBody)(__int64, float const*, unsigned char);
tModelSetTransformBody oModelSetTransformBody = nullptr;
const __int64 ModelSetTransformBodyRva = 0x32EB1F0;
const __int64 ModelSetTransformBodyCallRva = 0x32E2EA1;   // its one direct call, in 0x1432E2E80

__int64 __fastcall hModelSetTransformBody(__int64 entity, float const* transform, unsigned char notify)
{
  if (!t_MovingActor && AnyHeldActors() && HoldsBack(entity))
    return 0;
  return oModelSetTransformBody(entity, transform, notify);
}

// MECMP (the multiplayer mod) checks this body's first bytes when it loads, as
// SpatialEntity::setTransform, and stays off if they aren't the game's
// (C:\MECMP\mecmp.log 2026-10-30 03:19: "starts with E9 E0 5A C8 FC ...,
// expected 48 83 EC 38 ...": this hook). So the hook is on only while the
// replay holds actors (the only time it does anything), and never while MECMP
// is in the game; the replay's own moves go through its trampoline, which
// works either way. Turned on and off on the replay's thread (HoldActor,
// ReleaseActors) and off from the hang watch's when MECMP comes in.
std::atomic<bool> g_BodyHookOn{ false };

static void SetBodyHook(bool on)
{
  if (!oModelSetTransformBody)
    return;
  on = on && !util::IsMecmpLoaded();
  if (g_BodyHookOn.exchange(on) == on)
    return;
  LPVOID target = (LPVOID)((__int64)g_gameHandle + ModelSetTransformBodyRva);
  MH_STATUS status = on ? MH_EnableHook(target) : MH_DisableHook(target);
  if (status != MH_OK && status != MH_ERROR_ENABLED && status != MH_ERROR_DISABLED)
    util::log::Warning("The models' setTransform body hook couldn't be turned %s (MH_STATUS 0x%X)", on ? "on" : "off", status);
}

void util::hooks::KeepBodyHookOffForMecmp()
{
  if (g_BodyHookOn.load() && util::IsMecmpLoaded())
  {
    SetBodyHook(false);
    util::log::Write("MECMP is in the game: the replay's hook on the models' setTransform body is off (MECMP checks that "
      "function), the game may move a replayed vehicle meanwhile");
  }
}

static bool IsModelSetTransformBodyCode(BYTE const* pBase)
{
  // sub rsp, 0x38; mov eax, [rcx + 0x18]; movzx r9d, r8b; mov r11, rdx; shr eax, 0x15; mov r10, rcx
  static const BYTE code[] = { 0x48, 0x83, 0xEC, 0x38, 0x8B, 0x41, 0x18, 0x45, 0x0F, 0xB6, 0xC8, 0x49, 0x89, 0xD3, 0xC1, 0xE8, 0x15,
    0x49, 0x89, 0xCA };
  return !memcmp(pBase + ModelSetTransformBodyRva, code, sizeof(code)) && pBase[ModelSetTransformBodyCallRva] == 0xE8 &&
    ModelSetTransformBodyCallRva + 5 + *(int const*)(pBase + ModelSetTransformBodyCallRva + 1) == ModelSetTransformBodyRva;
}

static bool MoveVehicleRows(__int64 entity, float const* rows)
{
  unsigned int* flags = (unsigned int*)(entity + ActorFlagsOffset);
  unsigned int before = 0;
  bool cleared = false;
  __try
  {
    before = *flags;
    *flags = before & ~(ActorDeferredFlag | ActorPhysicsFlag);
    cleared = true;
    oModelSetTransformBody(entity, rows, 0);
    *flags = (*flags & ~(ActorDeferredFlag | ActorPhysicsFlag)) | (before & (ActorDeferredFlag | ActorPhysicsFlag));
    return true;
  }
  __except (EXCEPTION_EXECUTE_HANDLER)
  {
    if (cleared)
      *flags = (*flags & ~(ActorDeferredFlag | ActorPhysicsFlag)) | (before & (ActorDeferredFlag | ActorPhysicsFlag));
    return false;
  }
}

// A vehicle moved with its physics body: the setter's notify on and its queued
// bit off, its physics bit as it is (the vehicle's slot 28, 0x14324C810, then
// teleports the body). Taken as how a vehicle is seen where the replay puts it:
// a vehicle is drawn where its physics has it, as a dropped gun is (2026-10-24
// 18:52, the VTOL's entity was moved exactly and it wasn't seen there).
static bool MoveVehicleBody(__int64 entity, float const* rows)
{
  unsigned int* flags = (unsigned int*)(entity + ActorFlagsOffset);
  unsigned int before = 0;
  bool cleared = false;
  __try
  {
    before = *flags;
    *flags = before & ~ActorDeferredFlag;
    cleared = true;
    oModelSetTransformBody(entity, rows, 1);
    *flags = (*flags & ~ActorDeferredFlag) | (before & ActorDeferredFlag);
    return true;
  }
  __except (EXCEPTION_EXECUTE_HANDLER)
  {
    if (cleared)
      *flags = (*flags & ~ActorDeferredFlag) | (before & ActorDeferredFlag);
    return false;
  }
}

bool util::hooks::MoveVehiclePhysics(__int64 entity, float const* transform)
{
  if (!oModelSetTransformBody)
    return false;
  alignas(16) float rows[16];
  memcpy(rows, transform, sizeof(rows));
  t_MovingActor = true;
  bool moved = MoveVehicleBody(entity, rows);
  t_MovingActor = false;
  return moved;
}

bool util::hooks::CanMoveVehicles()
{
  return oModelSetTransformBody != nullptr;
}

bool util::hooks::MoveVehicleDrawn(__int64 entity, float const* transform)
{
  if (!oModelSetTransformBody)
    return false;
  alignas(16) float rows[16];
  memcpy(rows, transform, sizeof(rows));
  t_MovingActor = true;
  bool moved = MoveVehicleRows(entity, rows);
  t_MovingActor = false;
  return moved;
}

// A vehicle the replay plays, kept at its place whatever else moves it. The
// 2026-10-24 22:43 log: before each frame's move the two VTOLs' entities were
// back 667 m and 981 m from where the replay had put them, though the game's
// calls of the setter and its body were held. A vehicle's transform changes
// are queued (its flag bit 21): what changes it writes its transform and
// queues the entity (the vehicle's slot 30, 0x1431C3210, then 0x1431CA500),
// and for each queued entity (+ 0x55 set) 0x1431C3290(entity, queue) (from
// 0x1431C2FB0 / 0x1431C5790 / 0x1431C7050 / 0x1431C9100) brings its parts to
// its transform (0x143162060) and calls its change handler (slot 28,
// 0x14324C810: its physics, what's drawn of it). So a vehicle the replay plays
// gets the replay's place in its transform right before that (the rows' fourth
// floats left as they are). Offline, 2026-10-24.
typedef void(__fastcall* tTransformQueued)(__int64, __int64);
tTransformQueued oTransformQueued = nullptr;
const __int64 TransformQueuedRva = 0x31C3290;
const __int64 TransformQueuedCallRva = 0x31C2FC4;   // in 0x1431C2FB0

namespace
{
  const int MaxDrawnVehicles = 16;
  std::atomic<__int64> g_DrawnVehicles[MaxDrawnVehicles];   // 0: a free entry
  __int64 g_DrawnVehicleVTables[MaxDrawnVehicles];          // under g_DrawnVehicleMutex
  float g_DrawnVehicleRows[MaxDrawnVehicles][16];           // under g_DrawnVehicleMutex
  std::mutex g_DrawnVehicleMutex;
  std::atomic<int> g_DrawnVehicleCount{ 0 };                // entries in use (0: the hook doesn't look)
  std::atomic<int> g_VehicleChangesPlaced{ 0 };

  __int64 ReadEntityVTable(__int64 entity)
  {
    __try
    {
      return entity > 0x10000 ? *(__int64 const*)entity : 0;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      return 0;
    }
  }

  // Its transform (where the getter, slot 24 0x1432E1440, reads it) given the
  // rows' rotation and place, while it's still that entity
  bool PlaceVehicleRows(__int64 entity, __int64 vtable, float const* rows)
  {
    __try
    {
      if (*(__int64 const*)entity != vtable)
        return false;
      BYTE* components = *(BYTE**)(entity + 0x38);
      if ((__int64)components < 0x10000)
        return false;
      float* transform = (float*)(components + 0x10 + ((__int64)components[9] + 2 * components[0xA]) * 0x20);
      for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 3; ++c)
          transform[r * 4 + c] = rows[r * 4 + c];
      return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      return false;
    }
  }

  // Under g_DrawnVehicleMutex
  void CountDrawnVehicles()
  {
    int count = 0;
    for (int i = 0; i < MaxDrawnVehicles; ++i)
      count += g_DrawnVehicles[i].load(std::memory_order_relaxed) != 0;
    g_DrawnVehicleCount.store(count, std::memory_order_release);
  }
}

void __fastcall hTransformQueued(__int64 entity, __int64 queue)
{
  if (!t_MovingActor && g_DrawnVehicleCount.load(std::memory_order_acquire) > 0)
  {
    for (int i = 0; i < MaxDrawnVehicles; ++i)
    {
      if (g_DrawnVehicles[i].load(std::memory_order_relaxed) != entity)
        continue;
      float rows[16];
      __int64 vtable = 0;
      {
        std::lock_guard<std::mutex> lock(g_DrawnVehicleMutex);
        if (g_DrawnVehicles[i].load(std::memory_order_relaxed) == entity)
        {
          memcpy(rows, g_DrawnVehicleRows[i], sizeof(rows));
          vtable = g_DrawnVehicleVTables[i];
        }
      }
      if (vtable && PlaceVehicleRows(entity, vtable, rows))
        g_VehicleChangesPlaced.fetch_add(1, std::memory_order_relaxed);
      break;
    }
  }
  oTransformQueued(entity, queue);
}

static bool IsTransformQueuedCode(BYTE const* pBase)
{
  // mov [rsp + 0x18], rbp; mov [rsp + 0x20], rsi; push rdi; push r14; push r15; sub rsp, 0x30; movzx r8d, word [rcx + 0x58]
  static const BYTE code[] = { 0x48, 0x89, 0x6C, 0x24, 0x18, 0x48, 0x89, 0x74, 0x24, 0x20, 0x57, 0x41, 0x56, 0x41, 0x57, 0x48, 0x83,
    0xEC, 0x30, 0x44, 0x0F, 0xB7, 0x41, 0x58 };
  return !memcmp(pBase + TransformQueuedRva, code, sizeof(code)) && pBase[TransformQueuedCallRva] == 0xE8 &&
    TransformQueuedCallRva + 5 + *(int const*)(pBase + TransformQueuedCallRva + 1) == TransformQueuedRva;
}

bool util::hooks::DrawVehicleAt(__int64 entity, float const* rows)
{
  if (!oTransformQueued || !entity)
    return false;
  __int64 vtable = rows ? ReadEntityVTable(entity) : 0;
  std::lock_guard<std::mutex> lock(g_DrawnVehicleMutex);
  int slot = -1, freeSlot = -1;
  for (int i = 0; i < MaxDrawnVehicles; ++i)
  {
    __int64 e = g_DrawnVehicles[i].load(std::memory_order_relaxed);
    if (e == entity)
      slot = i;
    else if (!e && freeSlot < 0)
      freeSlot = i;
  }
  if (!rows || !vtable)
  {
    if (slot >= 0)
    {
      g_DrawnVehicles[slot].store(0, std::memory_order_release);
      CountDrawnVehicles();
    }
    return !rows;
  }
  if (slot < 0)
    slot = freeSlot;
  if (slot < 0)
    return false;
  memcpy(g_DrawnVehicleRows[slot], rows, sizeof(g_DrawnVehicleRows[slot]));
  g_DrawnVehicleVTables[slot] = vtable;
  g_DrawnVehicles[slot].store(entity, std::memory_order_release);
  CountDrawnVehicles();
  return true;
}

int util::hooks::ReleaseDrawnVehicles()
{
  std::lock_guard<std::mutex> lock(g_DrawnVehicleMutex);
  for (int i = 0; i < MaxDrawnVehicles; ++i)
    g_DrawnVehicles[i].store(0, std::memory_order_relaxed);
  g_DrawnVehicleCount.store(0, std::memory_order_release);
  return g_VehicleChangesPlaced.exchange(0);
}

// The MagRope's line in the replay (Research_MagRope_2026-10-25.md, outside
// the repo; offline, 2026-10-25). Faith's line (PamClientMagropeRenderEntity,
// in the registry the line's constructor / destructor hooks keep, above) has a
// mesh only while her animation is in one of its three MagRope states: then
// the game puts it on the MagRope update list ([0x142577788]) and each frame
// 0x143926CE0(line, update) makes a straight line from the anchor through her
// right index finger to her forearm (its 4 points, + 0xC8), spreads it evenly
// over the rope's joints (+ 0x108) and poses the mesh with them (0x1439B3350).
// While the replay records, those 4 points are kept right after that update;
// while it plays, the line gets its mesh from the rope base's own show
// (0x1439B2D00), goes on that list as the line's show puts it there
// (0x1439200A0), and is posed from the recorded points in place of the game's
// update; then the base's own hide (0x1439B3300) and the list's removal
// (0x143174D30), as the line's hide does. The line's own show and hide aren't
// called: they start the muzzle and impact effects, which the effect replay
// plays already. Game thread: the replay calls in from the camera hook, the
// update comes from the game's update.
typedef void(__fastcall* tMagropeUpdate)(__int64, __int64);
typedef void(__fastcall* tRopeShowHide)(__int64);
typedef void(__fastcall* tRopePose)(__int64, float const*, unsigned int, float);
typedef void(__fastcall* tMagropeUpdaterAdd)(__int64, unsigned int);
typedef void(__fastcall* tUpdaterRemove)(__int64, __int64);
tMagropeUpdate oMagropeUpdate = nullptr;
const __int64 MagropeUpdateRva = 0x3926CE0;
const __int64 MagropeUpdateCallRva = 0x3925CAF;   // in the MagRope list's update 0x143925C10
const __int64 MagropeShowBaseCallRva = 0x39261BB; // the line's show (slot 23, 0x143926160) calls the base's show
const __int64 MagropeShowAddCallRva = 0x39261D2;  // and puts it on the list
const __int64 MagropeHideBaseCallRva = 0x39267E8; // its hide (slot 24, 0x1439267E0) calls the base's hide
const __int64 MagropeHideRemoveCallRva = 0x39267FC; // and takes it off the list
const __int64 MagropePoseCallRva = 0x392735C;     // the update poses the mesh here
const __int64 MagropeUpdaterLoadRva = 0x39200BD;  // the list's add reads the list (mov rbx, [rip -> it])
const __int64 RopeShowRva = 0x39B2D00;            // the rope base's show: makes the mesh (+ 0x58) when there's none
const __int64 RopeHideRva = 0x39B3300;            // the base's hide: the mesh destroyed, + 0x58 / + 0x90 cleared
const __int64 RopePoseRva = 0x39B3350;            // (line, points, count, scale)
const __int64 MagropeUpdaterAddRva = 0x39200A0;   // (line + 0xC0, how often: 1 = every frame)
const __int64 UpdaterRemoveRva = 0x3174D30;       // (list, line)
const __int64 MagropeUpdaterRva = 0x2577788;      // [0x142577788]: the MagRope update list (made at the first show)

namespace
{
  const int RopeMeshOffset = 0x58;            // the rope's mesh entity (0: not shown)
  const int MagropeInPointsOffset = 0xC8;     // vector of 16-byte points: the anchor, then the 3 on her hand and arm
  const int MagropeOutPointsOffset = 0x108;   // vector of 16-byte points: one for each rope joint (201 on Faith's)
  const int MagropeUpdaterLinkOffset = 0xC0;
  const int MagropeDataScaleOffset = 0x30;    // PamMagropeRenderEntityData Scale
  const int MagropePointCount = 4;
  const unsigned int MaxRopeOutPoints = 512;
  const size_t MaxMagropeSamples = 40000;        // 11 minutes of the line shown at 60 a second (a recording is 10 at most)
  const double RopeSampleStep = 1.0 / 120.0;  // closer ones take the one before's place
  const double RopeSampleGap = 0.25;          // farther apart: the line wasn't shown in between
  const double RopeLastSampleHold = 1.0 / 30.0; // after a shown stretch's last sample it's still shown this long
  const float RopeNearFaith = 3.f;            // its finger point this near her feet: her line

  struct MagropeSample
  {
    double Time;
    float Points[MagropePointCount][4];
  };

  std::mutex g_RopeMutex;
  std::atomic<bool> g_RopeRecording{ false };
  double g_RopeClock = 0.0;                   // under g_RopeMutex
  std::vector<MagropeSample> g_RopeSamples;      // under it
  __int64 g_RopeRecordedLine = 0;             // under it: the line recorded, and its data (to find her line again after a respawn)
  __int64 g_RopeRecordedData = 0;
  int g_RopeDropped = 0, g_RopeOthers = 0;    // under it
  // While it plays: the line drawn from the replay (0: none), whether the
  // recording has it shown now and its points then (under g_RopeMutex), and
  // what was done (game thread)
  std::atomic<__int64> g_RopePlayLine{ 0 };
  bool g_RopeWanted = false;
  alignas(16) float g_RopeWantedPoints[MagropePointCount][4];
  std::atomic<bool> g_RopeMeshMade{ false };  // the replay made its mesh
  int g_RopeFramesPosed = 0, g_RopeShownTimes = 0;
  bool g_RopeTakenLogged = false, g_RopeMissingLogged = false, g_RopeFailedLogged = false;

  // Its 4 points and whether it has a mesh, read as the game left them (false: unreadable, or not 4)
  bool ReadMagropePoints(__int64 line, float (*points)[4], bool& shown, __int64& data)
  {
    __try
    {
      shown = *(__int64 const*)(line + RopeMeshOffset) != 0;
      data = *(__int64 const*)(line + EntityDataOffset);
      __int64 begin = *(__int64 const*)(line + MagropeInPointsOffset);
      __int64 end = *(__int64 const*)(line + MagropeInPointsOffset + 8);
      if (begin < 0x10000 || end - begin != MagropePointCount * 16)
        return false;
      memcpy(points, (void const*)begin, MagropePointCount * 16);
      return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      return false;
    }
  }

  // Game thread, right after the game's update of a line: Faith's line's
  // points kept while the replay records
  void RecordMagrope(__int64 line)
  {
    float points[MagropePointCount][4];
    bool shown = false;
    __int64 data = 0;
    if (!ReadMagropePoints(line, points, shown, data) || !shown || !g_EffectCenterOk.load(std::memory_order_relaxed))
      return;
    float dx = points[1][0] - g_EffectCenter[0], dy = points[1][1] - g_EffectCenter[1], dz = points[1][2] - g_EffectCenter[2];
    std::lock_guard<std::mutex> lock(g_RopeMutex);
    if (!g_RopeRecording.load(std::memory_order_relaxed))
      return;
    if (dx * dx + dy * dy + dz * dz > RopeNearFaith * RopeNearFaith || (g_RopeRecordedData && data != g_RopeRecordedData))
    {
      ++g_RopeOthers;
      return;
    }
    g_RopeRecordedData = data;
    g_RopeRecordedLine = line;
    if (!g_RopeSamples.empty() && g_RopeClock - g_RopeSamples.back().Time < RopeSampleStep && g_RopeClock >= g_RopeSamples.back().Time)
    {
      memcpy(g_RopeSamples.back().Points, points, sizeof(points));
      return;
    }
    if (g_RopeSamples.size() >= MaxMagropeSamples)
    {
      ++g_RopeDropped;
      return;
    }
    MagropeSample s;
    s.Time = g_RopeClock;
    memcpy(s.Points, points, sizeof(points));
    g_RopeSamples.push_back(s);
  }

  // As the game's update spreads them (0x143926CE0): count points evenly
  // along the line's length, each between the two points around it (past the
  // end, as there: the last segment's start)
  void SpreadRopePoints(float const (*in)[4], float (*out)[4], unsigned int count)
  {
    const int segments = MagropePointCount - 1;
    float length[segments];
    float total = 0.f;
    for (int i = 0; i < segments; ++i)
    {
      float dx = in[i + 1][0] - in[i][0], dy = in[i + 1][1] - in[i][1], dz = in[i + 1][2] - in[i][2];
      length[i] = sqrtf(dx * dx + dy * dy + dz * dz);
      total += length[i];
    }
    float perLength = total > 0.f ? 1.f / total : 0.f;
    float perPoint = count > 1 ? 1.f / (float)(count - 1) : 0.f;
    int i = 0;
    float before = 0.f;
    for (unsigned int k = 0; k < count; ++k)
    {
      float part = length[i] * perLength;
      float at = (float)k * perPoint;
      float f;
      while (at > before + part && i < segments - 1)
      {
        before += part;
        ++i;
        part = length[i] * perLength;
      }
      if (at > before + part)
        f = 0.f;   // past the last segment's end (rounding)
      else
        f = part > 0.f ? (at - before) / part : 0.f;
      for (int c = 0; c < 4; ++c)
        out[k][c] = in[i][c] + (in[i + 1][c] - in[i][c]) * f;
    }
  }

  // The recording's line at a time: false when it wasn't shown then
  bool RecordedRopeAt(double time, float (*points)[4])
  {
    if (g_RopeSamples.empty())
      return false;
    auto next = std::upper_bound(g_RopeSamples.begin(), g_RopeSamples.end(), time,
      [](double t, MagropeSample const& s) { return t < s.Time; });
    if (next == g_RopeSamples.begin())
      return false;
    MagropeSample const& a = *(next - 1);
    if (next != g_RopeSamples.end() && next->Time - a.Time <= RopeSampleGap)
    {
      float f = next->Time > a.Time ? (float)((time - a.Time) / (next->Time - a.Time)) : 0.f;
      for (int p = 0; p < MagropePointCount; ++p)
        for (int c = 0; c < 4; ++c)
          points[p][c] = a.Points[p][c] + (next->Points[p][c] - a.Points[p][c]) * f;
      return true;
    }
    if (time - a.Time > RopeLastSampleHold)
      return false;
    memcpy(points, a.Points, sizeof(a.Points));
    return true;
  }

  bool ReadRopeMesh(__int64 line, bool& shown)
  {
    __try
    {
      shown = *(__int64 const*)(line + RopeMeshOffset) != 0;
      return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      return false;
    }
  }

  // A line still alive (in the registry, or the recorded one when the
  // registry has none) and still a MagRope line with that data
  bool IsMagropeLine(__int64 line, __int64 data)
  {
    __try
    {
      return line > 0x10000 && *(__int64 const*)line == (__int64)g_gameHandle + RopeVTableRva &&
        (!data || *(__int64 const*)(line + EntityDataOffset) == data);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      return false;
    }
  }

  // Faith's line: one in the registry with the recorded one's data, else the
  // recorded one itself while it's there (made before the tools loaded, so
  // not in the registry; its destructor's hook forgets it, see
  // ForgetReplayRope)
  __int64 FindMagropeLine(__int64 data, __int64 recorded, bool& inRegistry)
  {
    __int64 found = 0;
    AcquireSRWLockShared(&g_WorldLock);
    for (int r = 0; r < MaxWorldRopes && !found; ++r)
    {
      __int64 e = g_WorldRopes[r];
      if (e && IsMagropeLine(e, data))
        found = e;
    }
    ReleaseSRWLockShared(&g_WorldLock);
    inRegistry = found != 0;
    if (!found && recorded && IsMagropeLine(recorded, data))
      found = recorded;
    return found;
  }

  // From the line's destructor's hook: a line gone isn't used again
  void ForgetReplayRope(__int64 line)
  {
    __int64 playing = line;
    if (g_RopePlayLine.compare_exchange_strong(playing, 0))
      g_RopeMeshMade.store(false);   // its mesh goes with it
    std::lock_guard<std::mutex> lock(g_RopeMutex);
    if (g_RopeRecordedLine == line)
      g_RopeRecordedLine = 0;
  }

  // The game's own calls (see above)
  bool CallRopeShow(__int64 line)
  {
    __try
    {
      ((tRopeShowHide)((__int64)g_gameHandle + RopeShowRva))(line);
      return *(__int64 const*)(line + RopeMeshOffset) != 0;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      return false;
    }
  }

  void CallRopeHide(__int64 line)
  {
    __try
    {
      ((tRopeShowHide)((__int64)g_gameHandle + RopeHideRva))(line);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
  }

  bool CallMagropeUpdaterAdd(__int64 line)
  {
    __try
    {
      ((tMagropeUpdaterAdd)((__int64)g_gameHandle + MagropeUpdaterAddRva))(line + MagropeUpdaterLinkOffset, 1);
      return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      return false;
    }
  }

  void CallMagropeUpdaterRemove(__int64 line)
  {
    __try
    {
      __int64 updater = *(__int64 const*)((__int64)g_gameHandle + MagropeUpdaterRva);
      if (updater > 0x10000)
        ((tUpdaterRemove)((__int64)g_gameHandle + UpdaterRemoveRva))(updater, line);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
  }

  // The rope's joints from the points, as the game's update does (false: unreadable)
  bool PoseRope(__int64 line, float const (*points)[4])
  {
    alignas(16) static float out[MaxRopeOutPoints][4];   // game thread only
    __try
    {
      __int64 begin = *(__int64 const*)(line + MagropeOutPointsOffset);
      __int64 end = *(__int64 const*)(line + MagropeOutPointsOffset + 8);
      __int64 data = *(__int64 const*)(line + EntityDataOffset);
      if (begin < 0x10000 || end <= begin || data < 0x10000)
        return false;
      unsigned int count = (unsigned int)((end - begin) / 16);
      if (count > MaxRopeOutPoints)
        return false;
      float scale = *(float const*)(data + MagropeDataScaleOffset);
      SpreadRopePoints(points, out, count);
      ((tRopePose)((__int64)g_gameHandle + RopePoseRva))(line, &out[0][0], count, scale);
      return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      return false;
    }
  }

  // Game thread: the line drawn from the replay let go (its mesh, when the
  // replay made it, destroyed again; off the list)
  void ReleaseReplayRope()
  {
    __int64 line = g_RopePlayLine.exchange(0);
    if (line && IsMagropeLine(line, 0))
    {
      bool shown = false;
      if (g_RopeMeshMade.load() && ReadRopeMesh(line, shown) && shown)
        CallRopeHide(line);
      CallMagropeUpdaterRemove(line);
    }
    g_RopeMeshMade.store(false);
    std::lock_guard<std::mutex> lock(g_RopeMutex);
    g_RopeWanted = false;
  }
}

void __fastcall hMagropeUpdate(__int64 line, __int64 update)
{
  // The line the replay draws: posed from the recording instead
  if (line && line == g_RopePlayLine.load(std::memory_order_acquire))
  {
    float points[MagropePointCount][4];
    bool wanted;
    {
      std::lock_guard<std::mutex> lock(g_RopeMutex);
      wanted = g_RopeWanted;
      memcpy(points, g_RopeWantedPoints, sizeof(points));
    }
    if (!wanted)
      return;
    bool shown = false;
    if (!ReadRopeMesh(line, shown))
      return;
    if (!shown)
    {
      if (g_RopeMeshMade.load())
      {
        // The game took it back meanwhile (its hide): not drawn again this time
        if (!g_RopeTakenLogged)
        {
          g_RopeTakenLogged = true;
          util::log::Warning("Replay: the game hid the MagRope line the replay drew (Faith used her own?), it's let go");
        }
        return;
      }
      if (!CallRopeShow(line))
      {
        if (!g_RopeFailedLogged)
        {
          g_RopeFailedLogged = true;
          util::log::Warning("Replay: the MagRope line's mesh couldn't be made, the line isn't drawn");
        }
        return;
      }
      g_RopeMeshMade.store(true);
    }
    if (PoseRope(line, points))
      ++g_RopeFramesPosed;
    return;
  }
  oMagropeUpdate(line, update);
  if (g_RopeRecording.load(std::memory_order_relaxed))
    RecordMagrope(line);
}

static bool IsMagropeReplayCode(BYTE const* pBase)
{
  // push rbp; push rbx; push rdi; lea rbp, [rsp - 0x90]; sub rsp, 0x190
  static const BYTE update[] = { 0x55, 0x53, 0x57, 0x48, 0x8D, 0xAC, 0x24, 0x70, 0xFF, 0xFF, 0xFF, 0x48, 0x81, 0xEC, 0x90, 0x01, 0x00, 0x00 };
  // mov rax, rsp; push rdi; sub rsp, 0x1D0 / push rbx; sub rsp, 0x20; mov rbx, rcx; mov rcx, [rcx + 0x58] /
  // mov rax, rsp; movss [rax + 0x20], xmm3; mov [rax + 0x18], r8d / push rdi; sub rsp, 0x40 /
  // push rbx; sub rsp, 0x30; mov rax, [rcx + 0x28]
  static const BYTE show[] = { 0x48, 0x89, 0xE0, 0x57, 0x48, 0x81, 0xEC, 0xD0, 0x01, 0x00, 0x00 };
  static const BYTE hide[] = { 0x53, 0x48, 0x83, 0xEC, 0x20, 0x48, 0x89, 0xCB, 0x48, 0x8B, 0x49, 0x58 };
  static const BYTE pose[] = { 0x48, 0x89, 0xE0, 0xF3, 0x0F, 0x11, 0x58, 0x20, 0x44, 0x89, 0x40, 0x18 };
  static const BYTE add[] = { 0x57, 0x48, 0x83, 0xEC, 0x40 };
  static const BYTE remove[] = { 0x53, 0x48, 0x83, 0xEC, 0x30, 0x48, 0x8B, 0x41, 0x28 };
  auto callTo = [&](__int64 rva, __int64 target) {
    BYTE const* p = pBase + rva;
    return p[0] == 0xE8 && rva + 5 + *(int const*)(p + 1) == target;
  };
  BYTE const* load = pBase + MagropeUpdaterLoadRva;
  return !memcmp(pBase + MagropeUpdateRva, update, sizeof(update)) && !memcmp(pBase + RopeShowRva, show, sizeof(show)) &&
    !memcmp(pBase + RopeHideRva, hide, sizeof(hide)) && !memcmp(pBase + RopePoseRva, pose, sizeof(pose)) &&
    !memcmp(pBase + MagropeUpdaterAddRva, add, sizeof(add)) && !memcmp(pBase + UpdaterRemoveRva, remove, sizeof(remove)) &&
    callTo(MagropeUpdateCallRva, MagropeUpdateRva) && callTo(MagropeShowBaseCallRva, RopeShowRva) &&
    callTo(MagropeShowAddCallRva, MagropeUpdaterAddRva) && callTo(MagropeHideBaseCallRva, RopeHideRva) &&
    callTo(MagropeHideRemoveCallRva, UpdaterRemoveRva) && callTo(MagropePoseCallRva, RopePoseRva) &&
    load[0] == 0x48 && load[1] == 0x8B && load[2] == 0x1D && MagropeUpdaterLoadRva + 7 + *(int const*)(load + 3) == MagropeUpdaterRva;
}

void util::hooks::ReplayRopeStartRecording()
{
  std::lock_guard<std::mutex> lock(g_RopeMutex);
  g_RopeSamples.clear();
  g_RopeRecordedLine = g_RopeRecordedData = 0;
  g_RopeDropped = g_RopeOthers = 0;
  g_RopeClock = 0.0;
  g_RopeRecording.store(oMagropeUpdate != nullptr);
}

void util::hooks::ReplayRopeRecordFrame(double time)
{
  std::lock_guard<std::mutex> lock(g_RopeMutex);
  g_RopeClock = time;
}

void util::hooks::ReplayRopeStopRecording()
{
  if (!g_RopeRecording.exchange(false))
    return;
  std::lock_guard<std::mutex> lock(g_RopeMutex);
  int stretches = 0;
  for (size_t i = 0; i < g_RopeSamples.size(); ++i)
    stretches += i == 0 || g_RopeSamples[i].Time - g_RopeSamples[i - 1].Time > RopeSampleGap;
  if (!g_RopeSamples.empty() || g_RopeOthers)
    util::log::Write("Replay: recorded Faith's MagRope line (0x%I64X) shown %d times, %d samples (%d left out: no room; %d updates of "
      "other lines)", g_RopeRecordedLine, stretches, (int)g_RopeSamples.size(), g_RopeDropped, g_RopeOthers);
}

void util::hooks::ReplayRopePlay(double time)
{
  if (!oMagropeUpdate)
    return;
  float points[MagropePointCount][4];
  bool wanted;
  __int64 data, recorded;
  {
    std::lock_guard<std::mutex> lock(g_RopeMutex);
    wanted = RecordedRopeAt(time, points);
    data = g_RopeRecordedData;
    recorded = g_RopeRecordedLine;
  }
  // (cleared by its destructor's hook when her line goes, a respawn)
  __int64 line = g_RopePlayLine.load(std::memory_order_relaxed);
  if (!wanted)
  {
    if (line)
      ReleaseReplayRope();
    return;
  }
  if (!line)
  {
    bool inRegistry = false;
    __int64 found = FindMagropeLine(data, recorded, inRegistry);
    bool shown = false;
    if (!found || !ReadRopeMesh(found, shown))
    {
      if (!g_RopeMissingLogged)
      {
        g_RopeMissingLogged = true;
        util::log::Warning("Replay: Faith's MagRope line wasn't found, the recorded one isn't drawn");
      }
      return;
    }
    // The game shows it itself (Faith using her own): left to the game
    if (shown)
      return;
    if (!CallMagropeUpdaterAdd(found))
      return;
    g_RopeMeshMade.store(false);
    g_RopeTakenLogged = false;
    ++g_RopeShownTimes;
    if (g_RopeShownTimes == 1)
      util::log::Write("Replay: Faith's MagRope line (0x%I64X%s) drawn from the recording", found,
        inRegistry ? "" : ", not in the registry: the recorded one");
    line = found;
    std::lock_guard<std::mutex> lock(g_RopeMutex);
    g_RopeWanted = true;
    memcpy(g_RopeWantedPoints, points, sizeof(points));
    g_RopePlayLine.store(line, std::memory_order_release);
    return;
  }
  std::lock_guard<std::mutex> lock(g_RopeMutex);
  g_RopeWanted = true;
  memcpy(g_RopeWantedPoints, points, sizeof(points));
}

void util::hooks::ReplayRopeStop()
{
  if (g_RopePlayLine.load())
    ReleaseReplayRope();
  if (g_RopeShownTimes)
    util::log::Write("Replay: Faith's MagRope line drawn %d times from the recording, posed in %d frames", g_RopeShownTimes,
      g_RopeFramesPosed);
  g_RopeShownTimes = g_RopeFramesPosed = 0;
  g_RopeMissingLogged = g_RopeFailedLogged = false;
}

bool util::hooks::SetNearReflections(bool on)
{
  if (!oReflectionScore)
    return false;
  if (g_NearReflections.exchange(on) != on)
    util::log::Write(on ? "Reflection volumes: the ones around the camera get their cubemaps first" :
      "Reflection volumes: the game's own order for their cubemaps");
  return true;
}

// The mesh streaming manager ([0x142563410], made once while the game starts
// by 0x14353B8F0) keeps its meshes in a table with room for 4096 records of
// 0x350 bytes inside itself (+0x70; begin / end / capacity at +0x50), and the
// add (0x143551D50) moves its end on without looking at the capacity: past
// 4096 it writes over the model instance lists. Only the constructor knows
// that room; everything else goes by the begin (the jobs copy it every frame,
// 0x1435458EF), and nothing frees or moves it. So right after the
// constructor, before the game even keeps the manager, the table is moved to
// a block with room for MeshesRoom, and so is the list of new meshes
// (+0x384230, room for 4096 u16 inside, no check either). The streaming
// pass's bitset of 4096 meshes (+0x386250, 0x143550870) is moved by sending
// its five uses through a few lines next to the game, and its four checks and
// the three caps of a mesh number at 0xFFF (0x1435554D4, 0x1435555E9,
// 0x1435556F9) go up to the new room. Mesh numbers are 16 bits wherever they're
// kept (instance records, batch entries).
namespace
{
  const __int64 MeshManagerGlobalRva = 0x2563410;
  const __int64 MeshManagerCtorRva = 0x353B8F0;
  const int MeshTableVector = 0x50;      // begin, end, capacity
  const int MeshTableInline = 0x70;      // the manager's own room: 4096 records
  const int MeshRecordBytes = 0x350;
  const int MeshTableInlineCount = 4096;
  const int MeshListVector = 0x384230;   // begin, cursor, end of the new meshes' numbers
  const int MeshListInline = 0x384250;   // 0x2000 bytes
  const int MeshesRoom = 16384;       // the caps become 0x3FFF
  const int MeshListRoom = 2 * MeshesRoom;
  const int MeshBitsetBytes = MeshesRoom / 8;

  std::atomic<int> g_MeshTableRoom{ 0 };  // set once the table is moved
  std::atomic<__int64> g_MeshTableBegin{ 0 };
  bool g_MeshCodeChanged = false;          // the bitset and caps
}

typedef __int64(__fastcall* tMeshManagerCtor)(__int64, __int64, __int64, __int64);
tMeshManagerCtor oMeshManagerCtor = nullptr;
__int64 __fastcall hMeshManagerCtor(__int64 pManager, __int64 a2, __int64 a3, __int64 a4)
{
  __int64 result = oMeshManagerCtor(pManager, a2, a3, a4);
  if (g_MeshTableRoom != 0 || !g_MeshCodeChanged)
    return result;

  // As the constructor left them: the table in its own room with record 0
  // (the "none" mesh), the list empty in its own
  __int64* pTable = (__int64*)(pManager + MeshTableVector);
  __int64* pList = (__int64*)(pManager + MeshListVector);
  __int64 inlineTable = pManager + MeshTableInline;
  __int64 used = pTable[1] - pTable[0];
  bool tableOk = pTable[0] == inlineTable && pTable[2] == inlineTable + (__int64)MeshTableInlineCount * MeshRecordBytes &&
    used >= 0 && used % MeshRecordBytes == 0 && used <= 16 * MeshRecordBytes;
  bool listOk = pList[0] == pManager + MeshListInline && pList[1] == pList[0] && pList[2] == pList[0] + 0x2000;
  if (!tableOk || !listOk)
  {
    util::log::Warning("The mesh manager isn't as the tools know it (table %s, list %s), it keeps room for 4096 meshes",
      tableOk ? "as known" : "not", listOk ? "as known" : "not");
    return result;
  }

  BYTE* pNewTable = (BYTE*)VirtualAlloc(NULL, (SIZE_T)MeshesRoom * MeshRecordBytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
  BYTE* pNewList = (BYTE*)VirtualAlloc(NULL, (SIZE_T)MeshListRoom * 2, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
  if (!pNewTable || !pNewList)
  {
    if (pNewTable) VirtualFree(pNewTable, 0, MEM_RELEASE);
    if (pNewList) VirtualFree(pNewList, 0, MEM_RELEASE);
    util::log::Warning("No memory for a bigger mesh table, the game keeps room for 4096 meshes");
    return result;
  }

  memcpy(pNewTable, (void*)pTable[0], (size_t)used);
  pTable[0] = (__int64)pNewTable;
  pTable[1] = (__int64)pNewTable + used;
  pTable[2] = (__int64)pNewTable + (__int64)MeshesRoom * MeshRecordBytes;
  pList[0] = (__int64)pNewList;
  pList[1] = (__int64)pNewList;
  pList[2] = (__int64)pNewList + (__int64)MeshListRoom * 2;
  g_MeshTableBegin = (__int64)pNewTable;
  g_MeshTableRoom = MeshesRoom;
  util::log::Ok("The game's mesh table has room for %d meshes instead of 4096", MeshesRoom);
  return result;
}

int util::hooks::MeshTableRoom()
{
  int room = g_MeshTableRoom;
  return room ? room : MeshTableInlineCount;
}

__int64 util::hooks::MeshTableBegin()
{
  return g_MeshTableBegin;
}

// Each zone streamer (made by 0x143C7DB00; ZoneStreamerEntity and its base,
// not RealmProxy) keeps the zones it has loaded in 128 slots inside itself
// (+0x2F8, 8 bytes each), and the props' streamer has 238 zones and 15
// regions. The load (0x143C80180) looks for a free slot among 128 and, with
// none, writes far past them; two readers (0x143C81703, 0x143C81C97) and the
// release (0x143C81F46, 0x143C81FD8) go by a zone's slot number. They're all
// sent to a block of ZoneSlotsRoom slots of the streamer's own, whose address
// the constructor's hook keeps where the first slot was. The release
// (0x143C81E60) and the load pass (0x143C81D20) also gather the zones they
// handle into 128 places on their stack without a check, so their hooks hand
// them at most ZonePassBatch zones a call: the other zones' request bits are
// hidden meanwhile, and the regions' after the first call (regions are
// handled one by one, but only once).
namespace
{
  const __int64 ZoneStreamerCtorRva = 0x3C7DB00;
  const __int64 ZoneReleaseRva = 0x3C81E60;
  const __int64 ZoneLoadPassRva = 0x3C81D20;
  const int ZoneSlotsField = 0x2F8;
  const int ZoneTableField = 0x2F0;
  const int ZoneDataField = 0x28;
  const int ZoneDataInfos = 0x70;        // the zones (count at - 4)
  const int ZoneDataBundles = 0x80;      // zones then regions (count at - 4)
  const int ZoneSlotsInline = 128;
  const int ZoneSlotsRoom = 512;
  const int ZonePassBatch = 120;         // of the 128 places
  const __int64 ZoneStreamerClassRva = 0x289EC20;

  std::atomic<bool> g_ZoneSlotsMoved{ false };
  __int64 g_ZoneSlotsFallback[ZoneSlotsRoom] = {}; // only if a block can't be had
}

typedef __int64(__fastcall* tZoneStreamerCtor)(__int64, __int64, __int64, __int64);
tZoneStreamerCtor oZoneStreamerCtor = nullptr;
__int64 __fastcall hZoneStreamerCtor(__int64 pStreamer, __int64 a2, __int64 a3, __int64 a4)
{
  __int64 result = oZoneStreamerCtor(pStreamer, a2, a3, a4);
  // The constructor zeroed the 128 slots; the first one now holds the block
  void* pSlots = VirtualAlloc(NULL, ZoneSlotsRoom * sizeof(__int64), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
  if (!pSlots)
  {
    util::log::Error("No memory for zone streamer 0x%I64X's zone slots, it shares a spare block: the game can crash", pStreamer);
    pSlots = g_ZoneSlotsFallback;
  }
  *(__int64*)(pStreamer + ZoneSlotsField) = (__int64)pSlots;
  return result;
}

typedef void(__fastcall* tZonePass)(__int64);
tZonePass oZoneRelease = nullptr;
tZonePass oZoneLoadPass = nullptr;

// Calls a pass with at most ZonePassBatch of the zones it would handle:
// those whose request bits (flags & 3) are want (2: unload asked, 1: load asked)
static void RunZonePassInBatches(tZonePass original, __int64 pStreamer, BYTE want)
{
  BYTE* pTable = *(BYTE**)(pStreamer + ZoneTableField);
  __int64 pData = *(__int64*)(pStreamer + ZoneDataField);
  __int64 pInfos = pData ? *(__int64*)(pData + ZoneDataInfos) : 0;
  __int64 pBundles = pData ? *(__int64*)(pData + ZoneDataBundles) : 0;
  int zones = pInfos ? *(int*)(pInfos - 4) : 0;
  int bundles = pBundles ? *(int*)(pBundles - 4) : 0;
  if (!pTable || zones <= 0 || bundles < zones || bundles > 4096)
  {
    original(pStreamer);
    return;
  }

  int asked = 0;
  for (int k = 0; k < zones; ++k)
    asked += (pTable[(__int64)k * 8] & 3) == want ? 1 : 0;
  if (asked <= ZonePassBatch)
  {
    original(pStreamer);
    return;
  }

  std::vector<int> askedZones, askedRegions;
  askedZones.reserve(asked);
  for (int k = 0; k < bundles; ++k)
  {
    if ((pTable[(__int64)k * 8] & 3) == want)
      (k < zones ? askedZones : askedRegions).push_back(k);
  }
  for (int z : askedZones)
    pTable[(__int64)z * 8] &= (BYTE)~want;
  for (size_t start = 0; start < askedZones.size(); start += ZonePassBatch)
  {
    size_t end = start + ZonePassBatch < askedZones.size() ? start + ZonePassBatch : askedZones.size();
    for (size_t i = start; i < end; ++i)
      pTable[(__int64)askedZones[i] * 8] |= want;
    if (start > 0)
    {
      for (int r : askedRegions)
        pTable[(__int64)r * 8] &= (BYTE)~want;
    }
    original(pStreamer);
    for (size_t i = start; i < end; ++i)
      pTable[(__int64)askedZones[i] * 8] &= (BYTE)~want;
    if (start > 0)
    {
      for (int r : askedRegions)
        pTable[(__int64)r * 8] |= want;
    }
  }
  // As they were, for what the game does next (it clears them itself)
  for (int z : askedZones)
    pTable[(__int64)z * 8] |= want;
}

void __fastcall hZoneRelease(__int64 pStreamer)
{
  RunZonePassInBatches(oZoneRelease, pStreamer, 2);
}

void __fastcall hZoneLoadPass(__int64 pStreamer)
{
  RunZonePassInBatches(oZoneLoadPass, pStreamer, 1);
}

int util::hooks::ZoneSlotRoom()
{
  return g_ZoneSlotsMoved ? ZoneSlotsRoom : ZoneSlotsInline;
}

__int64* util::hooks::ZoneSlots(__int64 pStreamer, int& count)
{
  if (g_ZoneSlotsMoved)
  {
    count = ZoneSlotsRoom;
    return *(__int64**)(pStreamer + ZoneSlotsField);
  }
  count = ZoneSlotsInline;
  return (__int64*)(pStreamer + ZoneSlotsField);
}


//////////////////////////
////  VISUALS HOOKS   ////
//////////////////////////

// Like the original tools, overrides are written both before and after the
// game updates its visual environment, so the game's own blending neither
// starts from nor ends up with its own values.

tVisualEnvironmentUpdate oVisualEnvironmentUpdate = nullptr;
__int64 __fastcall hVisualEnvironmentUpdate(__int64 a1, __int64 a2, __int64 a3, __int64 a4)
{
  if (!g_shutdown)
    g_mainHandle->GetVisualsManager()->OnVisualEnvironmentUpdate(true);

  __int64 result = oVisualEnvironmentUpdate(a1, a2, a3, a4);

  if (!g_shutdown)
    g_mainHandle->GetVisualsManager()->OnVisualEnvironmentUpdate(false);

  return result;
}

// Faith's hit effects, left out of the picture while the tools' camera is on
// (2026-10-24, offline). Her character (characters/faithcharacter) has the
// prefab FX/_Old/VisualEnviroments/Fullscreen/Faith_HitEffect_Prefab: six
// visual environments (VE_FS_Damage, _Damage_Add, _Damage_Sub, _Critical,
// _Critical_Sub, _Shield: a red frame drawn by a screen effect, a vignette,
// colour and exposure) its logic switches on (Enabled) and fades (Visibility)
// when she's hurt; the electric fence's is FX/VE/Combat/General/
// VE_Hit_Tazer_Additive. The visual environment manager ([0x142568CD8]) mixes
// each view's in 0x143703D60(view, ...), called for its views ([+ 0x2358, +
// 0x2360)) from its update 0x143705B30 in the client update (game thread), and
// only for a view whose + 0x3F00 is set (cleared at its end). The view's list
// [+ 0x530, + 0x538) holds each VisualEnvironmentEntity's (vtable 0x141AFA440)
// record at entity + 0xA0: + 0 visibility, + 4 the future one, + 0xD set: left
// out (the ctor writes !Enabled there), + 0x30 blend mode, then its
// components. One made from a VisualEnvironmentReferenceObjectData keeps that
// at entity + 0x60 (its Blueprint + 0x18, the blueprint's Name + 0x10). While
// the camera is on, the hit ones get + 0xD set for the mix and the game's
// value back right after it; a view is mixed again when that changes.
typedef __int64(__fastcall* tVisualEnvironmentBlend)(__int64, __int64, __int64, __int64);
tVisualEnvironmentBlend oVisualEnvironmentBlend = nullptr;
const __int64 VisualEnvironmentBlendRva = 0x3703D60;
const __int64 VisualEnvironmentBlendCallRva = 0x3705C4E;   // in the manager's update 0x143705B30
const __int64 VisualEnvironmentEntityVTableRva = 0x1AFA440;
const __int64 VisualEnvironmentEntityGetTypeRva = 0x3187C40;   // its slot 0
const int VeViewDirtyOffset = 0x3F00;
const int VeViewListOffset = 0x530;   // begin, end at + 0x538
const int VeRecordOffset = 0xA0;      // in the entity
const int VeRecordLeftOutOffset = 0xD;
const int VeRecordVisibilityOffset = 0x0;
const int VeReferenceOffset = 0x60;   // in the entity
const int ReferenceBlueprintOffset = 0x18;
const int MaxVeRecordsRead = 1024;
const int MaxHitEnvironmentsHidden = 64;   // (28 at most at once found in the game's data: 32 until 2026-11-09)
const int MaxVeViews = 16;
const int MaxVeNamesLogged = 40;

namespace
{
  char const* const HitEnvironmentPrefix = "FX/_Old/VisualEnviroments/Fullscreen/VE_FS_";
  // (and the heavy landing's: the screen-damage shader and colour fringes when
  // she lands hard, Faith's movement environments' (FaithCharacter_Movement_VEs);
  // 2026-11-09, the user: "Freecam shouldn't get affected by on screen effects
  // like rain droplets")
  // (and, since "Hide them too", the rest of her movement environments: the
  // speed's motion blur, the wallrun's, slide's and roll's depth of field, the
  // close focus, the fall's and the dash's, the MagRope's and the runner's-flow
  // colour and tonemap (VE_RunnersFlow_Add and _Sub, both, so they stay
  // balanced); the names are their blueprints', read from
  // FaithCharacter_Movement_VEs, whose 15 are these 13, VE_FS_Damage_Sub (the
  // prefix) and VE_RunnersFlow, kept: its only part feeds SurgeIntensity, read
  // only by the world's and people's material shaders (a glow on them while
  // she's in flow, not on the screen; the review of 2026-11-09))
  char const* const HitEnvironmentNames[] = { "FX/VE/Combat/General/VE_Hit_Tazer_Additive",
    "VisualEnvironments/Movement/VE_Movement_HeavyLanding", "VisualEnvironments/Movement/VE_Movement_Speed",
    "VisualEnvironments/Movement/VE_Movement_WallRun", "VisualEnvironments/Movement/VE_Movement_WallRun_Add",
    "VisualEnvironments/Movement/VE_Movement_CloseFocusDOF", "VisualEnvironments/Movement/VE_Movement_FallSpeed",
    "VisualEnvironments/Movement/VE_Movement_Dash_Add", "FX/Movement/VE/VE_FX_Slide", "FX/Movement/VE/VE_FX_Skillroll",
    "FX/Movement/VE/VE_FX_MagRope", "FX/Movement/VE/VE_FX_MagRope_Add", "FX/VE/VE_RunnersFlow_Add",
    "FX/VE/VE_RunnersFlow_Sub" };

  // Game thread only (the manager's update)
  struct VeViewState { __int64 View; bool Hidden; };
  VeViewState g_VeViews[MaxVeViews]{};
  int g_VeViewCount = 0;
  __int64 g_HitHidden[MaxHitEnvironmentsHidden]{};
  int g_HitHiddenCount = 0;
  bool g_HitHiddenLogged = false;
  // Diagnostic (temporary): the environments in a mix while the camera is on,
  // each name once
  __int64 g_VeNamesLogged[MaxVeNamesLogged]{};
  int g_VeNamesLoggedCount = 0;

  bool IsHitEnvironmentName(char const* name)
  {
    if (!_strnicmp(name, HitEnvironmentPrefix, strlen(HitEnvironmentPrefix)))
      return true;
    for (char const* hit : HitEnvironmentNames)
      if (!_stricmp(name, hit))
        return true;
    return false;
  }

  // A record's blueprint name, when it's a VisualEnvironmentEntity's made
  // from a reference object (else null)
  char const* VeRecordName(__int64 record)
  {
    __try
    {
      __int64 entity = record - VeRecordOffset;
      if (*(__int64 const*)entity != (__int64)g_gameHandle + VisualEnvironmentEntityVTableRva)
        return nullptr;
      __int64 reference = *(__int64 const*)(entity + VeReferenceOffset);
      if (reference < 0x10000)
        return nullptr;
      __int64 blueprint = *(__int64 const*)(reference + ReferenceBlueprintOffset);
      if (blueprint < 0x10000)
        return nullptr;
      char const* name = *(char const* const*)(blueprint + AssetNameOffset);
      if ((__int64)name < 0x10000 || !name[0])
        return nullptr;
      return name;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      return nullptr;
    }
  }

  bool ReadVeView(__int64 view, bool& dirty, __int64*& begin, __int64*& end)
  {
    __try
    {
      dirty = *(BYTE const*)(view + VeViewDirtyOffset) != 0;
      begin = *(__int64**)(view + VeViewListOffset);
      end = *(__int64**)(view + VeViewListOffset + 8);
      return begin && end >= begin && end - begin <= MaxVeRecordsRead;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      return false;
    }
  }

  void MarkVeViewDirty(__int64 view)
  {
    __try
    {
      *(BYTE*)(view + VeViewDirtyOffset) = 1;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
  }

  // Sets a record's "left out" byte when it isn't set yet: true if it did
  bool LeaveOut(__int64 record)
  {
    __try
    {
      BYTE* p = (BYTE*)(record + VeRecordLeftOutOffset);
      if (*p)
        return false;
      *p = 1;
      return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      return false;
    }
  }

  // The game's value back: only where ours still is (the game sets it itself
  // when the environment is switched off)
  void PutBackLeftOut(__int64 record)
  {
    __try
    {
      BYTE* p = (BYTE*)(record + VeRecordLeftOutOffset);
      if (*p == 1)
        *p = 0;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
  }

  float ReadVeVisibility(__int64 record)
  {
    __try
    {
      return *(float const*)(record + VeRecordVisibilityOffset);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      return 0.f;
    }
  }

  // Diagnostic (temporary): an environment seen in a mix while the camera is
  // on, logged once by its name
  void LogVeName(char const* name, float visibility, bool hit)
  {
    for (int i = 0; i < g_VeNamesLoggedCount; ++i)
      if (g_VeNamesLogged[i] == (__int64)name)
        return;
    if (g_VeNamesLoggedCount >= MaxVeNamesLogged)
      return;
    g_VeNamesLogged[g_VeNamesLoggedCount++] = (__int64)name;
    util::log::Write("Free camera (diagnostic): visual environment %s in the picture (visibility %.2f)%s", name, visibility,
      hit ? ": one of Faith's hit or hard-landing effects, left out" : "");
  }

  // Diagnostic: a visual environment in a mix while the camera is on that
  // VeRecordName can't name (its entity's + 0x60 not set: not made from a
  // reference object, or one inside a prefab, unknown), logged once by its
  // data while it shows. Faith's movement and hit environments sit inside her
  // prefabs: if they're among these, they aren't left out (2026-11-09: no
  // log so far had a hit effect left out to tell). To tell which it is: the
  // record's blend mode (+ 0x30, the data's + 0x8C, the game's 0x14317C24C)
  // and which of the parts the mix 0x143703D60 reads are there: bit n of the
  // first mask for record + 0x38 + n * 8 (to + 0xE0), of the second for
  // record + 0x238 + n * 8 (to + 0x260; the 4 lens reflections first).
  const int MaxUnnamedVesLogged = 24;
  __int64 g_UnnamedVesLogged[MaxUnnamedVesLogged]{};
  int g_UnnamedVesLoggedCount = 0;
  void LogUnnamedVe(__int64 record)
  {
    __int64 data = 0;
    int blendMode = 0;
    unsigned partsLow = 0, partsHigh = 0;
    __try
    {
      __int64 entity = record - VeRecordOffset;
      if (*(__int64 const*)entity != (__int64)g_gameHandle + VisualEnvironmentEntityVTableRva)
        return;
      data = *(__int64 const*)(entity + 0x30);
      blendMode = *(int const*)(record + 0x30);
      for (int n = 0; 0x38 + n * 8 <= 0xE0; ++n)
        if (*(__int64 const*)(record + 0x38 + n * 8))
          partsLow |= 1u << n;
      for (int n = 0; 0x238 + n * 8 <= 0x260; ++n)
        if (*(__int64 const*)(record + 0x238 + n * 8))
          partsHigh |= 1u << n;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      return;
    }
    float visibility = ReadVeVisibility(record);
    if (!(visibility > 0.f) || g_UnnamedVesLoggedCount >= MaxUnnamedVesLogged)
      return;
    for (int i = 0; i < g_UnnamedVesLoggedCount; ++i)
      if (g_UnnamedVesLogged[i] == data)
        return;
    g_UnnamedVesLogged[g_UnnamedVesLoggedCount++] = data;
    util::log::Write("Free camera (diagnostic): a visual environment in the picture the tools can't name (its data 0x%I64X, "
      "visibility %.2f, blend mode %d, parts 0x%06X / 0x%02X): not left out", data, visibility, blendMode, partsLow, partsHigh);
  }

  // Before a view's mix: the hit environments in its list left out
  void HideHitEnvironments(__int64 view, bool diagnose)
  {
    g_HitHiddenCount = 0;
    bool dirty = false;
    __int64* begin = nullptr;
    __int64* end = nullptr;
    if (!ReadVeView(view, dirty, begin, end) || !dirty)
      return;
    for (__int64* p = begin; p < end; ++p)
    {
      __int64 record = 0;
      __try
      {
        record = *p;
      }
      __except (EXCEPTION_EXECUTE_HANDLER)
      {
        return;
      }
      if (record < 0x10000)
        continue;
      char const* name = VeRecordName(record);
      if (!name)
      {
        if (diagnose)
          LogUnnamedVe(record);
        continue;
      }
      bool hit = IsHitEnvironmentName(name);
      if (diagnose)
      {
        float visibility = ReadVeVisibility(record);
        if (visibility > 0.f)
          LogVeName(name, visibility, hit);
      }
      if (!hit || g_HitHiddenCount >= MaxHitEnvironmentsHidden || !LeaveOut(record))
        continue;
      g_HitHidden[g_HitHiddenCount++] = record;
      if (!g_HitHiddenLogged)
      {
        g_HitHiddenLogged = true;
        util::log::Write("Free camera: Faith's hit and hard-landing effects are left out of the picture (first: %s)", name);
      }
    }
  }

  void ShowHitEnvironments()
  {
    for (int i = 0; i < g_HitHiddenCount; ++i)
      PutBackLeftOut(g_HitHidden[i]);
    g_HitHiddenCount = 0;
  }

  // The bloom's lens dirt and the lens reflections out of a view's mixed
  // state (view + 0x550, state 0) while the tools' camera is on (2026-11-09,
  // "Hide them too"; read from the game's code, offline): the mix puts the
  // lens dirt's texture (the city's FX_DirtyLens) at state + 0x1AF0, and with
  // none there the renderer takes the game's own tonemap without dirt; the
  // four lens reflection slots are at state + 0x16F0 + i x 0x60, their Enable
  // at + 0x40, and with none on the renderer runs no lens reflection at all.
  // Written after every call (the state keeps them until the next mix); the
  // game's own come back with the mix the view gets when the camera goes off
  // (MarkVeViewDirty), so nothing is kept to put back (a texture's pointer
  // could be freed meanwhile). Only for the views in g_VeViews: one past
  // MaxVeViews isn't marked for a mix when the camera goes off.
  const int VeViewStateOffset = 0x550;
  const int VeStateLensDirtTextureOffset = 0x1AF0;
  const int VeStateLensReflectionsOffset = 0x16F0, VeLensReflectionStride = 0x60, VeLensReflectionEnableOffset = 0x40;
  void HideLensLook(__int64 view)
  {
    __try
    {
      __int64 state = *(__int64 const*)(view + VeViewStateOffset);
      if (state < 0x10000)
        return;
      *(__int64*)(state + VeStateLensDirtTextureOffset) = 0;
      for (int i = 0; i < 4; ++i)
        *(BYTE*)(state + VeStateLensReflectionsOffset + i * VeLensReflectionStride + VeLensReflectionEnableOffset) = 0;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
  }
}

__int64 __fastcall hVisualEnvironmentBlend(__int64 view, __int64 a2, __int64 a3, __int64 a4)
{
  long long toolsTicks = 0;   // the tools' own time in here, not the game's mix (MISC > Performance)
  LARGE_INTEGER t0, t1;
  QueryPerformanceCounter(&t0);
  bool hide = !g_shutdown && g_mainHandle && g_mainHandle->GetCameraManager()->IsCameraEnabled();
  // A view is mixed again when what's left out changes
  int k = 0;
  while (k < g_VeViewCount && g_VeViews[k].View != view)
    ++k;
  if (k == g_VeViewCount && k < MaxVeViews)
    g_VeViews[g_VeViewCount++] = { view, false };
  if (k < g_VeViewCount)
  {
    if (g_VeViews[k].Hidden != hide)
      MarkVeViewDirty(view);
    g_VeViews[k].Hidden = hide;
  }
  else if (hide)
    MarkVeViewDirty(view);
  if (hide)
    HideHitEnvironments(view, util::DiagnosticsOn());
  QueryPerformanceCounter(&t1);
  toolsTicks += t1.QuadPart - t0.QuadPart;
  __int64 result = oVisualEnvironmentBlend(view, a2, a3, a4);
  {
    perf::Scope timing(perf::SectionVeMix);
    if (hide)
      ShowHitEnvironments();
    // The rain's strength (EmitterParameter3), see weather::AfterViewMix
    if (!g_shutdown && weather::AfterViewMix(view))
      MarkVeViewDirty(view);
    // (the lens dirt and the lens reflections out, last, see HideLensLook)
    if (hide && k < g_VeViewCount)
      HideLensLook(view);
  }
  perf::AddTicks(perf::SectionVeMix, toolsTicks);
  return result;
}

// The game's drops and water on the lens left out of the picture while the
// tools' free camera is on (2026-11-09, the user, through the weather session:
// "Freecam shouldn't get affected by on screen effects like rain droplets").
// They're 16 of the game's 863 emitter templates, every one whose DebugName
// has "OnScreen" or "CamDroplets" (read from the game's data, 2026-11-09):
// the rain's drops (Em_Evo_Rain_CamDroplets_02 / 03, FX_Rain_Camprox's, kept at
// the camera by the weather), the waterfalls' and streams' water, foam,
// splash and drops on the screen, the drips' and leaks' drops, and the drops
// from Faith's steps, landings and slides in water. 14 are in the camera's
// space (their template's CameraSpace, + 0x16D), the waterfall's foam and
// splash in a small box just in front of it. The game asks every emitter, for
// every view, whether it's drawn (0x1430E79D0, from the emitterCull and
// emitterMeshBuild jobs: both only build an emitter it returned 0 for). This
// returns 5, the game's own 'nothing to draw', for these while the camera is
// on: drops already on the lens go at once, nothing of the game's is written,
// and with the camera off the game's own answer is given as before. (The
// game's answer first: only what it would draw is left out, and counted;
// the shadow and reflection views it already leaves them out of. The
// review of 2026-11-09.) On the
// job workers: only the flag (an atomic) and reads under __try; the name
// read every time (a template's place can be another's once its zone goes).
// (emitter + 0x280 its template's holder, [holder + 8] its EmitterTemplateData,
// + 0x158 its DebugName: Weather.cpp's)
// And, since "Hide them too" (2026-11-09), five more near the camera, by
// their whole names: the heat haze when running through hot air
// (Em_Ambient_Heat_L / M_RunThrogh, not their _Outside world kinds), the
// glass shards from one tutorial wall (Em_Glass_GPMWindow_SecondaryHits_
// Shards_1_Release_Green, not its _0 kind) and the runner's-flow streaks
// (Em_RunnersFlow_Camprox_Flow / _Streak): 21 of the 863 in all, checked
// against the game's data.
const __int64 EmitterViewCullRva = 0x30E79D0;
typedef int(__fastcall* tEmitterViewCull)(__int64, __int64, __int64, float, float, int, unsigned short, unsigned short, __int64,
  __int64);
tEmitterViewCull oEmitterViewCull = nullptr;

namespace
{
  char const* const CameraEmitterNames[] = { "em_ambient_heat_l_runthrogh", "em_ambient_heat_m_runthrogh",
    "em_glass_gpmwindow_secondaryhits_shards_1_release_green", "em_runnersflow_camprox_flow", "em_runnersflow_camprox_streak" };

  bool IsLensEmitter(__int64 emitter)
  {
    __try
    {
      __int64 holder = *(__int64 const*)(emitter + 0x280);
      if (holder < 0x10000)
        return false;
      __int64 templ = *(__int64 const*)(holder + 8);
      if (templ < 0x10000)
        return false;
      char const* name = *(char const* const*)(templ + 0x158);
      if ((__int64)name < 0x10000)
        return false;
      char lower[96];
      int i = 0;
      for (; i + 1 < (int)sizeof(lower) && name[i]; ++i)
        lower[i] = name[i] >= 'A' && name[i] <= 'Z' ? (char)(name[i] + ('a' - 'A')) : name[i];
      lower[i] = 0;
      if (strstr(lower, "onscreen") || strstr(lower, "camdroplets"))
        return true;
      for (char const* other : CameraEmitterNames)
        if (!strcmp(lower, other))
          return true;
      return false;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      return false;
    }
  }
}

int __fastcall hEmitterViewCull(__int64 emitter, __int64 a2, __int64 a3, float a4, float a5, int a6, unsigned short a7,
  unsigned short a8, __int64 a9, __int64 a10)
{
  int drawn = oEmitterViewCull(emitter, a2, a3, a4, a5, a6, a7, a8, a9, a10);
  if (drawn == 0 && !g_shutdown && g_LensHidden.load(std::memory_order_relaxed) && IsLensEmitter(emitter))
  {
    if (util::DiagnosticsOn())
      g_LensLeftOut.fetch_add(1, std::memory_order_relaxed);
    return 5;
  }
  return drawn;
}

// The video billboards' sizes on screen, from the tools' camera (2026-10-24,
// offline; Research_Billboards_2026-10-24.md). Each frame the billboard
// manager's job (0x1437DEB30) has 0x1437E2190(manager, feeds) give each of
// its 32 feeds (0x40 each) a size on screen (+ 0x10): per screen
// SizeInMeterSqr / (distance^2 x 1.4), less facing away or out of view,
// the largest of its screens; 0x1437E2760 picks the video's LOD from it. The
// distance and view are the camera registry entry (0, 0)'s ([0x142415E50]):
// its transform from 0x143278E40 (the lookup by ids, 0x143278E10, jumps
// there), its view from 0x143278C30 with the vertical field of view in degrees
// from the entry's slot 3, 0x143279C00 (what the game's render view gets too,
// 0x143536A90). The free camera writes only the render view, so the
// billboards kept being sized from Faith's camera. While the tools' camera is
// on, for that pass only (it runs in job threads: a thread-local flag), those
// two give the tools' camera's transform and field of view, and the sizes are
// scaled by how much it's zoomed in from the game's field of view (the size
// is from the distance only).
typedef void(__fastcall* tBillboardSizes)(__int64, __int64);
typedef __int64(__fastcall* tCameraEntryTransform)(__int64, float*);
typedef float(__fastcall* tCameraEntryFov)(__int64);
tBillboardSizes oBillboardSizes = nullptr;
tCameraEntryTransform oCameraEntryTransform = nullptr;
tCameraEntryFov oCameraEntryFov = nullptr;
const __int64 BillboardSizesRva = 0x37E2190;
const __int64 BillboardSizesCallRva = 0x37DEBA5;       // in the billboard manager's job 0x1437DEB30
const __int64 CameraEntryTransformRva = 0x3278E40;
const __int64 CameraEntryTransformJumpRva = 0x3278E37; // jmp from the lookup 0x143278E10
const __int64 CameraEntryFovRva = 0x3279C00;
const __int64 CameraEntryVTableRva = 0x1B17D28;        // the entry's vtable, slot 3
const int BillboardFeeds = 32;
const int BillboardFeedStride = 0x40;
const int BillboardFeedSizeOffset = 0x10;

namespace
{
  thread_local bool t_BillboardPass = false;
  thread_local float t_BillboardGameFov = 0.f;   // the game's, seen in this pass
  thread_local float t_BillboardCameraAt[3] = {}; // the camera the sizes were measured from, in this pass
  thread_local bool t_BillboardCameraSeen = false;
  SRWLOCK g_BillboardCameraLock = SRWLOCK_INIT;
  bool g_BillboardCameraOn = false;              // under g_BillboardCameraLock
  alignas(16) float g_BillboardCamera[16]{};     // under it
  float g_BillboardCameraFov = 0.f;              // vertical, degrees; under it
  std::atomic<bool> g_BillboardCameraLogged{ false };

  bool ReadBillboardCamera(float* transform, float* fov)
  {
    AcquireSRWLockShared(&g_BillboardCameraLock);
    bool on = g_BillboardCameraOn;
    if (on)
    {
      if (transform)
        memcpy(transform, g_BillboardCamera, sizeof(g_BillboardCamera));
      if (fov)
        *fov = g_BillboardCameraFov;
    }
    ReleaseSRWLockShared(&g_BillboardCameraLock);
    return on;
  }

  // The replay's video screens (replay_gamedraws::FeedSizes): a channel the
  // replay draws gets at least the size its screens would have there; how
  // many were raised, listed (channel: the game's size -> the replay's)
  std::atomic<ULONGLONG> g_FeedSizesLoggedAt{ 0 };

  int RaiseBillboardSizes(__int64 feeds, float const* sizes, char* list, size_t listSize)
  {
    int raised = 0;
    size_t at = 0;
    list[0] = 0;
    __try
    {
      for (int i = 0; i < BillboardFeeds; ++i)
      {
        float* size = (float*)(feeds + i * BillboardFeedStride + BillboardFeedSizeOffset);
        if (sizes[i] <= *size)
          continue;
        if (at + 32 < listSize)
          at += sprintf_s(list + at, listSize - at, "%s%d: %.3f -> %.3f", raised ? ", " : "", i, *size, sizes[i]);
        *size = sizes[i];
        ++raised;
      }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
    return raised;
  }

  void ScaleBillboardSizes(__int64 feeds, float scale)
  {
    __try
    {
      for (int i = 0; i < BillboardFeeds; ++i)
        *(float*)(feeds + i * BillboardFeedStride + BillboardFeedSizeOffset) *= scale;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
  }
}

void util::hooks::SetBillboardCamera(bool on, float const* transform, float fovYDegrees)
{
  AcquireSRWLockExclusive(&g_BillboardCameraLock);
  g_BillboardCameraOn = on && transform && fovYDegrees > 1.f && fovYDegrees < 179.f;
  if (g_BillboardCameraOn)
  {
    memcpy(g_BillboardCamera, transform, sizeof(g_BillboardCamera));
    g_BillboardCameraFov = fovYDegrees;
  }
  ReleaseSRWLockExclusive(&g_BillboardCameraLock);
  // For the heard-sounds diagnostic (NoteHeardLive)
  if (util::DiagnosticsOn())
  {
    if (on && transform)
      memcpy(g_HeardCamera, transform + 12, sizeof(g_HeardCamera));
    g_HeardCameraOk = on && transform;
  }
}

void __fastcall hBillboardSizes(__int64 manager, __int64 feeds)
{
  t_BillboardPass = true;
  t_BillboardGameFov = 0.f;
  t_BillboardCameraSeen = false;
  oBillboardSizes(manager, feeds);
  t_BillboardPass = false;
  float fov = 0.f;
  if (t_BillboardGameFov > 1.f && t_BillboardGameFov < 179.f && ReadBillboardCamera(nullptr, &fov))
  {
    // How much bigger the tools' camera shows things than the game's
    const float DegToRad = 3.14159265f / 180.f;
    float ratio = tanf(t_BillboardGameFov * 0.5f * DegToRad) / tanf(fov * 0.5f * DegToRad);
    float scale = (std::min)((std::max)(ratio * ratio, 0.01f), 400.f);
    if (scale != 1.f)
      ScaleBillboardSizes(feeds, scale);
    if (!g_BillboardCameraLogged.exchange(true))
      util::log::Write("Billboards: sized from the tools' camera (field of view %.1f deg, the game's %.1f deg: sizes x %.2f)", fov,
        t_BillboardGameFov, scale);
  }
  // The replay's video screens (2026-11-01; see replay_gamedraws::
  // NoteFeedTextures): the channels' textures now (the table 0x1437DF8D0 just
  // filled), and the channels it draws sized from the same camera
  replay_gamedraws::NoteFeedTextures(feeds);
  if (t_BillboardCameraSeen)
  {
    float sizes[BillboardFeeds];
    replay_gamedraws::FeedSizes(t_BillboardCameraAt, sizes);
    char list[400];
    int raised = RaiseBillboardSizes(feeds, sizes, list, sizeof(list));
    // In the log every 20 s while it does (the game's LOD rows: 0.005 level 4 ... 0.25 level 1)
    ULONGLONG now = GetTickCount64(), last = g_FeedSizesLoggedAt.load();
    if (raised && now - last >= 20000 && g_FeedSizesLoggedAt.compare_exchange_strong(last, now))
      util::log::Write("Billboards: %d video channels sized for the replay's screens (channel: the game's size -> the replay's): %s",
        raised, list);
  }
}

__int64 __fastcall hCameraEntryTransform(__int64 entry, float* out)
{
  __int64 result = oCameraEntryTransform(entry, out);
  if (t_BillboardPass && out)
  {
    ReadBillboardCamera(out, nullptr);
    // Its place (the fourth row), for the replay's screens (see hBillboardSizes)
    memcpy(t_BillboardCameraAt, out + 12, sizeof(t_BillboardCameraAt));
    t_BillboardCameraSeen = true;
  }
  return result;
}

float __fastcall hCameraEntryFov(__int64 entry)
{
  float fov = oCameraEntryFov(entry);
  if (!t_BillboardPass)
    return fov;
  t_BillboardGameFov = fov;
  float camera = 0.f;
  return ReadBillboardCamera(nullptr, &camera) ? camera : fov;
}

static bool IsBillboardCameraCode(BYTE const* pBase)
{
  // mov rax, rsp; push rbp; push rsi; push r14; sub rsp, 0x2A0
  static const BYTE sizes[] = { 0x48, 0x89, 0xE0, 0x55, 0x56, 0x41, 0x56, 0x48, 0x81, 0xEC, 0xA0, 0x02, 0x00, 0x00 };
  // mov [rsp + 8], rbx; push rdi; sub rsp, 0x30; mov rdi, rdx; mov rbx, rcx; lea rdx, [rcx + 0x30]
  static const BYTE transform[] = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x30, 0x48, 0x89, 0xD7, 0x48, 0x89, 0xCB,
    0x48, 0x8D, 0x51, 0x30 };
  // mov rax, [rcx + 0x208]; test rax, rax; je
  static const BYTE fov[] = { 0x48, 0x8B, 0x81, 0x08, 0x02, 0x00, 0x00, 0x48, 0x85, 0xC0, 0x74, 0x10 };
  return !memcmp(pBase + BillboardSizesRva, sizes, sizeof(sizes)) &&
    !memcmp(pBase + CameraEntryTransformRva, transform, sizeof(transform)) && !memcmp(pBase + CameraEntryFovRva, fov, sizeof(fov)) &&
    pBase[BillboardSizesCallRva] == 0xE8 &&
    BillboardSizesCallRva + 5 + *(int const*)(pBase + BillboardSizesCallRva + 1) == BillboardSizesRva &&
    pBase[CameraEntryTransformJumpRva] == 0xEB &&
    CameraEntryTransformJumpRva + 2 + (signed char)pBase[CameraEntryTransformJumpRva + 1] == CameraEntryTransformRva &&
    *(__int64 const*)(pBase + CameraEntryVTableRva + 3 * 8) == (__int64)pBase + CameraEntryFovRva;
}

// The video billboards at the tools' game speed (2026-10-25, offline and a
// read-only look at the live feeds; Research_GameSpeed_2026-10-25.md). Each
// frame the feeds' update (0x1437DE7B0, a job of update pass 2, which gets the
// game's scaled frame time) has 0x1437E29F0(feed, frame time in ns, ...)
// move each feed's play time (+ 0x278) on. A feed whose sound is playing
// follows the sound instead: + 0x288 is its link to its SoundEntity, whose
// "PlayPosition" it reads and goes to (forward, or back past 0.5 s). Sounds
// play at real speed, so the screens one hears kept full speed while the
// game was slowed; silent ones (their sound's position doesn't move) already
// went by the frame time. While the tools' game speed isn't 1x, that
// per-frame call (from 0x1437DE8E4) runs without the link, the game's own path
// for a feed without one: the video goes by the game's time and its sound
// plays on at its own. Back at 1x the feed goes back to its sound's position
// the game's own way. The seek (0x1437E1680, an event) is left alone.
typedef void(__fastcall* tFeedTime)(__int64, unsigned __int64, __int64);
tFeedTime oFeedTime = nullptr;
const __int64 FeedTimeRva = 0x37E29F0;
const __int64 FeedTimeCallRva = 0x37DE8E4;          // in the feeds' update 0x1437DE7B0
const __int64 FeedSoundLinkReadRva = 0x37E2A15;     // mov rax, [rcx + 0x288]
const int FeedSoundLinkOffset = 0x288;

namespace
{
  std::atomic<bool> g_FeedsAtGameSpeed{ false };
}

void util::hooks::SetFeedsAtGameSpeed(bool on)
{
  if (g_FeedsAtGameSpeed.exchange(on) != on)
    util::log::Write(on ? "Billboards: the videos go by the game speed (their sound plays on at its own)"
      : "Billboards: the videos follow their sound again");
}

void __fastcall hFeedTime(__int64 feed, unsigned __int64 frameNs, __int64 a3)
{
  if (!g_FeedsAtGameSpeed.load(std::memory_order_relaxed) ||
    (__int64)_ReturnAddress() != (__int64)g_gameHandle + FeedTimeCallRva + 5)
  {
    oFeedTime(feed, frameNs, a3);
    return;
  }
  // The feed is this job's alone while it runs
  __int64* link = (__int64*)(feed + FeedSoundLinkOffset);
  __int64 sound = *link;
  *link = 0;
  __try
  {
    oFeedTime(feed, frameNs, a3);
  }
  __finally
  {
    *link = sound;
  }
}

static bool IsFeedTimeCode(BYTE const* pBase)
{
  // mov [rsp + 0x18], rbx; mov [rsp + 0x20], rbp; push rdi; sub rsp, 0x30; cmp byte ptr [rcx + 0x280], 0
  static const BYTE start[] = { 0x48, 0x89, 0x5C, 0x24, 0x18, 0x48, 0x89, 0x6C, 0x24, 0x20, 0x57, 0x48, 0x83, 0xEC, 0x30, 0x80, 0xB9,
    0x80, 0x02, 0x00, 0x00, 0x00 };
  static const BYTE link[] = { 0x48, 0x8B, 0x81, 0x88, 0x02, 0x00, 0x00 };
  return !memcmp(pBase + FeedTimeRva, start, sizeof(start)) && !memcmp(pBase + FeedSoundLinkReadRva, link, sizeof(link)) &&
    pBase[FeedTimeCallRva] == 0xE8 && FeedTimeCallRva + 5 + *(int const*)(pBase + FeedTimeCallRva + 1) == FeedTimeRva;
}

static bool IsEmitterViewCullCode(BYTE const* pBase)
{
  // mov [rsp + 0x18], rbx; mov [rsp + 0x20], rbp; push rsi, rdi, r12, r14, r15; sub rsp, 0x190;
  // mov r14, [rsp + 0x200]; mov r12, [rsp + 0x208]; mov rbx, [rcx + 0x280]
  static const BYTE start[] = { 0x48, 0x89, 0x5C, 0x24, 0x18, 0x48, 0x89, 0x6C, 0x24, 0x20, 0x56, 0x57, 0x41, 0x54, 0x41, 0x56,
    0x41, 0x57, 0x48, 0x81, 0xEC, 0x90, 0x01, 0x00, 0x00, 0x4C, 0x8B, 0xB4, 0x24, 0x00, 0x02, 0x00, 0x00, 0x4C, 0x8B, 0xA4, 0x24,
    0x08, 0x02, 0x00, 0x00, 0x48, 0x8B, 0x99, 0x80, 0x02, 0x00, 0x00 };
  static const BYTE cameraSpace[] = { 0x80, 0xBB, 0x6D, 0x01, 0x00, 0x00, 0x00 };   // 0x1430E7AFC cmp byte [rbx + 0x16D], 0
  // its two callers (the emitterMeshBuild and emitterCull jobs' 0x1430E3815, 0x1430E7447), each testing only for 0
  static const __int64 calls[] = { 0x30E3815, 0x30E7447 };
  if (memcmp(pBase + EmitterViewCullRva, start, sizeof(start)) || memcmp(pBase + 0x30E7AFC, cameraSpace, sizeof(cameraSpace)))
    return false;
  for (__int64 call : calls)
    if (pBase[call] != 0xE8 || call + 5 + *(int const*)(pBase + call + 1) != EmitterViewCullRva)
      return false;
  static const BYTE testAfterMesh[] = { 0x8B, 0x54, 0x24, 0x54, 0x85, 0xC0 };   // mov edx, [rsp + 0x54]; test eax, eax
  static const BYTE testAfterCull[] = { 0x85, 0xC0, 0x75, 0x03 };              // test eax, eax; jne
  return !memcmp(pBase + 0x30E381A, testAfterMesh, sizeof(testAfterMesh)) &&
    !memcmp(pBase + 0x30E744C, testAfterCull, sizeof(testAfterCull));
}

static bool IsVisualEnvironmentBlendCode(BYTE const* pBase)
{
  // push rbp, rsi, rdi, r12 - r15; lea rbp, [rsp - 0x1000]; mov eax, 0x1100
  static const BYTE start[] = { 0x55, 0x56, 0x57, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x8D, 0xAC, 0x24, 0x00, 0xF0,
    0xFF, 0xFF, 0xB8, 0x00, 0x11, 0x00, 0x00 };
  static const BYTE dirtyCheck[] = { 0x41, 0x80, 0xBC, 0x24, 0x00, 0x3F, 0x00, 0x00, 0x00 };          // cmp byte [r12 + 0x3F00], 0
  static const BYTE listRead[] = { 0x49, 0x8B, 0x94, 0x24, 0x38, 0x05, 0x00, 0x00, 0x49, 0x8B, 0x8C, 0x24, 0x30, 0x05, 0x00, 0x00 };
  static const BYTE leftOutCheck[] = { 0x80, 0x7E, 0x0D, 0x00 };                                    // cmp byte [rsi + 0xD], 0
  static const BYTE dirtyClear[] = { 0x41, 0x88, 0x9C, 0x24, 0x00, 0x3F, 0x00, 0x00 };             // mov [r12 + 0x3F00], bl
  static const BYTE ctorLeftOut[] = { 0x88, 0x87, 0xAD, 0x00, 0x00, 0x00 };                        // mov [rdi + 0xAD], al
  static const BYTE ctorReference[] = { 0x48, 0x89, 0x5F, 0x60 };                                  // mov [rdi + 0x60], rbx
  return !memcmp(pBase + VisualEnvironmentBlendRva, start, sizeof(start)) &&
    !memcmp(pBase + 0x3703E14, dirtyCheck, sizeof(dirtyCheck)) && !memcmp(pBase + 0x3703E5D, listRead, sizeof(listRead)) &&
    !memcmp(pBase + 0x370403B, leftOutCheck, sizeof(leftOutCheck)) && !memcmp(pBase + 0x3705AB3, dirtyClear, sizeof(dirtyClear)) &&
    !memcmp(pBase + 0x317C240, ctorLeftOut, sizeof(ctorLeftOut)) && !memcmp(pBase + 0x317C29F, ctorReference, sizeof(ctorReference)) &&
    pBase[VisualEnvironmentBlendCallRva] == 0xE8 &&
    VisualEnvironmentBlendCallRva + 5 + *(int const*)(pBase + VisualEnvironmentBlendCallRva + 1) == VisualEnvironmentBlendRva &&
    *(__int64 const*)(pBase + VisualEnvironmentEntityVTableRva) == (__int64)pBase + VisualEnvironmentEntityGetTypeRva;
}

//////////////////////////
////   INPUT HOOKS    ////
//////////////////////////

// Returning 0 from OFFSET_ENTRYINPUT stops the player from receiving any
// input, which is what "disableAllInput" did in the original tools.

tEntryInput oEntryInput = nullptr;
__int64 __fastcall hEntryInput(__int64 a1, __int64 a2, __int64 a3, __int64 a4)
{
  if (g_mainHandle->GetCameraManager()->IsKbmDisabled() || g_mainHandle->GetUI()->IsEnabled())
    return 0;

  return oEntryInput(a1, a2, a3, a4);
}

//////////////////////////
////   CURSOR HOOKS   ////
//////////////////////////

// During gameplay the game keeps the cursor clipped and re-centered for
// mouse look. While the tools UI is open those calls are ignored so the
// cursor can move freely over the UI.

tSetCursorPos oSetCursorPos = nullptr;
BOOL WINAPI hSetCursorPos(int x, int y)
{
  if (!g_shutdown && g_mainHandle->GetUI()->IsEnabled())
    return TRUE;

  return oSetCursorPos(x, y);
}

tClipCursor oClipCursor = nullptr;
BOOL WINAPI hClipCursor(const RECT* pRect)
{
  if (!g_shutdown && g_mainHandle->GetUI()->IsEnabled())
    return oClipCursor(nullptr);

  return oClipCursor(pRect);
}

// The game sets the cursor's display count to where it wants it: up to 64
// ShowCursor calls at a time until the count is about 2 (shown) or -2 (hidden),
// 0x143241C80 / 0x143249CFA. While the menu is open (with the Windows cursor)
// its calls on the window's thread only change a count of its own, which they
// get back as usual, and the real count stays at 0 or more; when the menu
// closes the real count is set to the game's (HoldCursorShown, from the window
// procedure: the display count is the calling thread's).
typedef int(WINAPI* tShowCursor)(BOOL);
tShowCursor oShowCursor = nullptr;
namespace
{
  bool g_cursorHeld = false;   // window thread only
  int g_gameCursorCount = 0;   // the game's own count while held
  DWORD g_cursorThread = 0;    // the window's thread
  std::atomic<bool> g_cursorOtherThreadLogged{ false };
  const int MaxCursorSteps = 1024;

  // The calling thread's display count as it is
  int ReadCursorCount()
  {
    oShowCursor(FALSE);
    return oShowCursor(TRUE);
  }
}

int WINAPI hShowCursor(BOOL show)
{
  if (g_cursorHeld)
  {
    if (GetCurrentThreadId() == g_cursorThread)
      return show ? ++g_gameCursorCount : --g_gameCursorCount;
    if (!g_cursorOtherThreadLogged.exchange(true))
      util::log::Warning("The game set the cursor's display count from thread %u, not the window's (%u): the menu "
        "may not show the cursor", GetCurrentThreadId(), g_cursorThread);
  }
  return oShowCursor(show);
}

bool util::hooks::HoldCursorShown(bool hold)
{
  if (!oShowCursor)
    return false;
  if (hold == g_cursorHeld)
    return true;
  if (hold)
  {
    int count = ReadCursorCount();
    g_gameCursorCount = count;
    g_cursorThread = GetCurrentThreadId();
    g_cursorHeld = true;
    for (int i = 0; count < 0 && i < MaxCursorSteps; ++i)
      count = oShowCursor(TRUE);
  }
  else
  {
    g_cursorHeld = false;
    int target = g_gameCursorCount;
    int count = ReadCursorCount();
    for (int i = 0; count != target && i < MaxCursorSteps; ++i)
      count = oShowCursor(count < target ? TRUE : FALSE);
  }
  return true;
}

// The game hides the Windows cursor by setting none. While the menu is open
// (and uses the Windows cursor) it gets the arrow instead.
tSetCursor oSetCursor = nullptr;
HCURSOR WINAPI hSetCursor(HCURSOR hCursor)
{
  if (!hCursor && !g_shutdown && g_mainHandle->GetUI()->IsEnabled() && g_mainHandle->GetUI()->UsesWindowsCursor())
  {
    static HCURSOR s_arrow = LoadCursor(NULL, IDC_ARROW);
    hCursor = s_arrow;
  }
  return oSetCursor(hCursor);
}

//////////////////////////
////   OTHER HOOKS    ////
//////////////////////////

// These could be hooks on AI to disable them, object iterators
// to find certain components, something to disable the usual
// outlines on friendly players/characters etc...


/*----------------------------------------------------------------*/

namespace
{
  // The hooks made. Added to by the tools' start-up thread and, at a first
  // use, by the game's thread and the drawing thread (CreateGameHook, and
  // HookDepthClears' CreateHook), so only under m_CreatedHooksMutex: two
  // threads adding to a map at once can break it (2026-11-13 CT.log: the
  // drawing thread's first-frame hooks and the start-up thread's road
  // traffic ones in the same second)
  std::unordered_map<std::string, util::hooks::Hook> m_CreatedHooks;
  std::mutex m_CreatedHooksMutex;

  // A hook made, added to the list: false when one of that name is in it already
  bool NoteCreatedHook(std::string const& name, util::hooks::Hook const& info)
  {
    std::lock_guard<std::mutex> lock(m_CreatedHooksMutex);
    return m_CreatedHooks.emplace(name, info).second;
  }

  bool IsHookCreated(std::string const& name)
  {
    std::lock_guard<std::mutex> lock(m_CreatedHooksMutex);
    return m_CreatedHooks.count(name) != 0;
  }

  // Placeholder reservation next to the game exe, released right before the
  // game hooks are created so MinHook's VirtualAlloc can land in it
  LPVOID m_pReservedHookSpace = nullptr;
  // And spare ones beside it, released one at a time when MinHook finds no
  // room for a hook's trampoline (see MinHookCreate). MinHook keeps its
  // trampolines in 4 KB blocks of 63, each taking one allocation unit (64 KB)
  // within reach of the hooked code, and the game fills the space round its
  // exe with heap soon after start-up: past 63 hooks there was nowhere for a
  // second block (2026-10-25: the LightMapId and MagropeUpdate hooks weren't
  // made, MH_ERROR_MEMORY_ALLOC). Released only when needed, so the game's
  // heap can't take them meanwhile.
  const int SpareHookUnits = 7;
  LPVOID m_SpareHookSpace[SpareHookUnits] = {};
  std::mutex m_SpareHookMutex;
  // The hooks that couldn't be made, for the menu to show (a failed one used
  // to be only a log line: the LightMapId hook was off for hours and the
  // black buildings came back, 2026-10-25)
  std::mutex m_FailedHooksMutex;
  std::vector<std::string> m_FailedHooks;   // under m_FailedHooksMutex

  void NoteFailedHook(std::string const& name)
  {
    std::lock_guard<std::mutex> lock(m_FailedHooksMutex);
    if (std::find(m_FailedHooks.begin(), m_FailedHooks.end(), name) == m_FailedHooks.end())
      m_FailedHooks.push_back(name);
  }

  // dxgi's Present (0 if it wasn't found)
  __int64 m_PresentAddress = 0;
  // Calls through the jump when the game's present function was first seen,
  // to see whether they still come
  unsigned int m_JumpCallsAtGameStart = 0;
  bool m_PresentJumpCounted = false;
  bool m_PresentJumpChecked = false;

  // IDXGISwapChain::Present lives in dxgi.dll and is shared by every
  // swapchain, so creating a throwaway one is enough to find its address.
  __int64 GetPresentAddress()
  {
    WNDCLASSEXA wc{ 0 };
    wc.cbSize = sizeof(WNDCLASSEXA);
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "CT_DummyDx11Window";
    RegisterClassExA(&wc);

    HWND hwnd = CreateWindowA(wc.lpszClassName, "", WS_OVERLAPPEDWINDOW, 0, 0, 100, 100, NULL, NULL, wc.hInstance, NULL);
    if (!hwnd)
    {
      util::log::Error("Failed to create dummy window, GetLastError 0x%X", GetLastError());
      UnregisterClassA(wc.lpszClassName, wc.hInstance);
      return 0;
    }

    DXGI_SWAP_CHAIN_DESC desc{ 0 };
    desc.BufferCount = 1;
    desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.OutputWindow = hwnd;
    desc.SampleDesc.Count = 1;
    desc.Windowed = TRUE;
    desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    IDXGISwapChain* pSwapChain = nullptr;
    ID3D11Device* pDevice = nullptr;
    ID3D11DeviceContext* pContext = nullptr;

    __int64 address = 0;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL, 0, D3D11_SDK_VERSION,
      &desc, &pSwapChain, &pDevice, NULL, &pContext);

    if (SUCCEEDED(hr) && pSwapChain)
      address = (*(__int64**)pSwapChain)[8]; // Present
    else
      util::log::Error("D3D11CreateDeviceAndSwapChain failed, HRESULT 0x%X", hr);

    if (pSwapChain) pSwapChain->Release();
    if (pContext) pContext->Release();
    if (pDevice) pDevice->Release();

    DestroyWindow(hwnd);
    UnregisterClassA(wc.lpszClassName, wc.hInstance);
    return address;
  }
}

// MH_CreateHook, and when MinHook has no room left for the trampoline, again
// with one more spare unit next to the game released (see m_SpareHookSpace)
static MH_STATUS MinHookCreate(LPVOID target, LPVOID detour, LPVOID* original)
{
  MH_STATUS status = MH_CreateHook(target, detour, original);
  while (status == MH_ERROR_MEMORY_ALLOC)
  {
    int left = 0;
    LPVOID unit = nullptr;
    {
      std::lock_guard<std::mutex> lock(m_SpareHookMutex);
      for (int i = SpareHookUnits - 1; i >= 0 && !unit; --i)
        if (m_SpareHookSpace[i])
        {
          unit = m_SpareHookSpace[i];
          m_SpareHookSpace[i] = nullptr;
          left = i;
        }
    }
    if (!unit)
      break;
    VirtualFree(unit, 0, MEM_RELEASE);
    util::log::Write("Hook memory: MinHook was out of room, a spare 64 KB unit at 0x%I64X released for it (%d left)",
      (__int64)unit, left);
    status = MH_CreateHook(target, detour, original);
  }
  return status;
}

// Creates a normal function hook with MinHook,
// which places a jmp instruction at the start of the function.
// True when it's made and added to the list (what CreateGameHook gives back)
template <typename T>
static bool CreateHook(std::string const& name, __int64 target, PVOID hook, T original, bool enable = true)
{
  if (!target)
  {
    util::log::Error("Could not create %s hook, target address is unknown", name.c_str());
    NoteFailedHook(name);
    return false;
  }

  LPVOID* pOriginal = reinterpret_cast<LPVOID*>(original);
  MH_STATUS result = MinHookCreate((LPVOID)target, hook, pOriginal);
  if (result != MH_OK)
  {
    util::log::Error("Could not create %s hook. MH_STATUS 0x%X error code 0x%X", name.c_str(), result, GetLastError());
    NoteFailedHook(name);
    return false;
  }

  // enable false: made, the game's code left as it is until it's turned on
  // (its trampoline, *original, works either way)
  result = enable ? MH_EnableHook((LPVOID)target) : MH_OK;
  if (result != MH_OK)
  {
    util::log::Error("Could not enable %s hook. MH_STATUS 0x%X error code 0x%X", name.c_str(), result, GetLastError());
    NoteFailedHook(name);
    return false;
  }

  util::hooks::Hook hookInfo{ 0 };
  hookInfo.Address = target;
  hookInfo.Type = util::hooks::HookType::MinHook;
  hookInfo.Enabled = enable;

  bool listed = NoteCreatedHook(name, hookInfo);
  util::log::Ok("Hooked %s at 0x%I64X%s", name.c_str(), target, enable ? "" : " (off until it's needed)");
  return listed;
}

std::vector<std::string> util::hooks::FailedHooks()
{
  std::lock_guard<std::mutex> lock(m_FailedHooksMutex);
  return m_FailedHooks;
}

// Write to VTable
static PBYTE WINAPI WriteToVTable(PDWORD64* ppVTable, PVOID hook, SIZE_T iIndex)
{
  DWORD dwOld = 0;
  VirtualProtect((void*)((*ppVTable) + iIndex), sizeof(PDWORD64), PAGE_EXECUTE_READWRITE, &dwOld);

  PBYTE pOrig = ((PBYTE)(*ppVTable)[iIndex]);
  (*ppVTable)[iIndex] = (DWORD64)hook;

  VirtualProtect((void*)((*ppVTable) + iIndex), sizeof(PDWORD64), dwOld, &dwOld);
  return pOrig;
}

// Hooks a function by changing the address at given index
// in the virtual function table.
template <typename T>
static void CreateVTableHook(std::string const& name, PDWORD64* ppVTable, PVOID hook, SIZE_T iIndex, T original)
{
  LPVOID* pOriginal = reinterpret_cast<LPVOID*>(original);
  *pOriginal = reinterpret_cast<LPVOID>(WriteToVTable(ppVTable, hook, iIndex));

  util::hooks::Hook hookInfo{ 0 };
  hookInfo.Address = (__int64)ppVTable;
  hookInfo.Index = iIndex;
  hookInfo.Type = util::hooks::HookType::VTable;
  hookInfo.Original = pOriginal;
  hookInfo.Enabled = true;

  NoteCreatedHook(name, hookInfo);
}

void util::hooks::ReserveSpaceNearGame()
{
  // MinHook needs its trampolines within reach of a rel32 jump from the
  // hooked function. Mirror's Edge fills the space around its exe with heap
  // allocations soon after start-up, so grab one allocation unit early.
  MODULEINFO info;
  if (!GetModuleInformation(GetCurrentProcess(), g_gameHandle, &info, sizeof(MODULEINFO)))
    return;

  SYSTEM_INFO si;
  GetSystemInfo(&si);
  const ULONG_PTR granularity = si.dwAllocationGranularity;
  const ULONG_PTR imageStart = (ULONG_PTR)info.lpBaseOfDll;
  const ULONG_PTR imageEnd = imageStart + info.SizeOfImage;
  const ULONG_PTR firstAbove = (imageEnd + granularity - 1) & ~(granularity - 1);
  const ULONG_PTR firstBelow = (imageStart & ~(granularity - 1)) - granularity;

  for (ULONG_PTR distance = 0; distance < 0x20000000 && !m_pReservedHookSpace; distance += granularity)
  {
    m_pReservedHookSpace = VirtualAlloc((LPVOID)(firstAbove + distance), granularity, MEM_RESERVE, PAGE_NOACCESS);
    if (!m_pReservedHookSpace && firstBelow > distance)
      m_pReservedHookSpace = VirtualAlloc((LPVOID)(firstBelow - distance), granularity, MEM_RESERVE, PAGE_NOACCESS);
  }

  // The spares, the nearest free units after it
  int spares = 0;
  {
    std::lock_guard<std::mutex> lock(m_SpareHookMutex);
    for (ULONG_PTR distance = 0; distance < 0x20000000 && spares < SpareHookUnits; distance += granularity)
    {
      LPVOID unit = VirtualAlloc((LPVOID)(firstAbove + distance), granularity, MEM_RESERVE, PAGE_NOACCESS);
      if (unit)
        m_SpareHookSpace[spares++] = unit;
      if (spares < SpareHookUnits && firstBelow > distance)
      {
        unit = VirtualAlloc((LPVOID)(firstBelow - distance), granularity, MEM_RESERVE, PAGE_NOACCESS);
        if (unit)
          m_SpareHookSpace[spares++] = unit;
      }
    }
  }

  if (m_pReservedHookSpace)
    util::log::Write("Reserved hook memory at 0x%I64X (and %d spare units for more than 63 hooks)", m_pReservedHookSpace, spares);
  else
    util::log::Warning("No free memory next to the game exe, hooks will look further away");
}

// Executable memory the game's code can reach with a rel32 jump (within
// 1 GB of the exe), or null
static BYTE* AllocNearGame(SIZE_T size)
{
  MODULEINFO info;
  if (!GetModuleInformation(GetCurrentProcess(), g_gameHandle, &info, sizeof(MODULEINFO)))
    return nullptr;
  SYSTEM_INFO si;
  GetSystemInfo(&si);
  const ULONG_PTR granularity = si.dwAllocationGranularity;
  const ULONG_PTR imageStart = (ULONG_PTR)info.lpBaseOfDll;
  const ULONG_PTR imageEnd = imageStart + info.SizeOfImage;
  const ULONG_PTR firstAbove = (imageEnd + granularity - 1) & ~(granularity - 1);
  const ULONG_PTR firstBelow = (imageStart & ~(granularity - 1)) - granularity;
  for (ULONG_PTR distance = 0; distance < 0x40000000; distance += granularity)
  {
    LPVOID p = VirtualAlloc((LPVOID)(firstAbove + distance), size, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    if (!p && firstBelow > distance)
      p = VirtualAlloc((LPVOID)(firstBelow - distance), size, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
    if (p)
      return (BYTE*)p;
  }
  return nullptr;
}

static bool Rel32(BYTE const* pFrom, BYTE const* pTo, int& rel)
{
  __int64 d = (__int64)pTo - (__int64)pFrom;
  if (d < INT_MIN || d > INT_MAX)
    return false;
  rel = (int)d;
  return true;
}

// The lens flare caves put in (see "Lens flares far away"): MinHook writes
// the jumps (with the game's threads held, one in the middle moved on), and
// each cave does the game's own instructions itself and jumps back after them
static void HookFarFlares()
{
  BYTE* pBase = (BYTE*)g_gameHandle;
  if (!IsFlareCode(pBase))
  {
    util::log::Warning("The lens flares' code isn't what the tools know, far lens flares can't be faded");
    return;
  }
  // Anywhere does: the caves jump back absolutely and reach their constants
  // rip-relative within themselves, and MinHook relays a far detour itself
  // (2026-10-23 21:54: nothing was left within 1 GB of the exe by then)
  BYTE* pCave = AllocNearGame(0x1000);
  if (!pCave)
    pCave = (BYTE*)VirtualAlloc(nullptr, 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
  if (!pCave)
  {
    util::log::Warning("No memory for the lens flare code, far lens flares can't be faded");
    return;
  }
  BuildFlareCaves(pCave, pBase);
  FlushInstructionCache(GetCurrentProcess(), pCave, 0x1000);
  struct Site { char const* Name; BYTE* pTarget; BYTE* pCave; };
  Site sites[] = { { "FlareAlphaDistance", pBase + FlareAlphaTRva, pCave }, { "FlareAlphaClamp", pBase + FlareAlphaClampRva, pCave + 0x40 },
    { "FlareSizeDistance", pBase + FlareSizeTRva, pCave + 0xA0 } };
  int made = 0;
  MH_STATUS status = MH_OK;
  for (; made < (int)_countof(sites); ++made)
    if ((status = MinHookCreate(sites[made].pTarget, sites[made].pCave, &g_FlareTrampolines[made])) != MH_OK)
      break;
  int enabled = 0;
  if (made == (int)_countof(sites))
    for (; enabled < made; ++enabled)
      if ((status = MH_EnableHook(sites[enabled].pTarget)) != MH_OK)
        break;
  if (enabled < (int)_countof(sites))
  {
    for (int i = 0; i < made; ++i)
    {
      MH_DisableHook(sites[i].pTarget);
      MH_RemoveHook(sites[i].pTarget);
    }
    util::log::Warning("The lens flare code couldn't be changed (MH_STATUS 0x%X), far lens flares can't be faded", status);
    return;
  }
  for (Site const& site : sites)
  {
    util::hooks::Hook info{ 0 };
    info.Address = (__int64)site.pTarget;
    info.Type = util::hooks::HookType::MinHook;
    info.Enabled = true;
    NoteCreatedHook(site.Name, info);
    util::log::Ok("Hooked %s at 0x%I64X", site.Name, (__int64)site.pTarget);
  }
  g_FlareCave = pCave;
  // The draws' skip, on its own (see FlareDrawStartRva)
  if (!IsFlareSkipCode(pBase))
  {
    util::log::Warning("The lens flares' draw code isn't what the tools know: lens flares show on the free camera");
    return;
  }
  BYTE* pSkipAt = pBase + FlareDrawStartRva;
  status = MinHookCreate(pSkipAt, pCave + FlareCaveSkip, &g_FlareSkipTrampoline);
  if (status == MH_OK)
  {
    status = MH_EnableHook(pSkipAt);
    if (status != MH_OK)
      MH_RemoveHook(pSkipAt);
  }
  if (status != MH_OK)
  {
    util::log::Warning("The lens flares' draws couldn't be changed (MH_STATUS 0x%X): lens flares show on the free camera", status);
    return;
  }
  util::hooks::Hook info{ 0 };
  info.Address = (__int64)pSkipAt;
  info.Type = util::hooks::HookType::MinHook;
  info.Enabled = true;
  NoteCreatedHook("FlareDrawSkip", info);
  util::log::Ok("Hooked FlareDrawSkip at 0x%I64X", (__int64)pSkipAt);
  g_FlareSkipHooked = true;
}

// Reflection captures after a time jump. When the sun moves past
// LocalIBLSunUpdateThreshold, 0x1436F80C0 has 0x1436FC9E0 mark every
// reflection volume waiting (+ 0xC0 |= 4), and the scheduler 0x143707710
// recaptures them one at a time (a state per GPU at manager + 0x64 + i * 0x10:
// 3 - 8 the six faces). The faces done a frame are worked out at 0x143702B90:
// min(6, the caller's room for views, the manager's copy of
// LocalIBLMaxFaceCapture (+ 0x96DC, 1 live), the faces left) (2026-10-23).
// The game's settings copy allows 1, 2, 3 or 6 (0x1437599A3); nothing else
// reads the face count (the per-face array at manager + 0x128 has all six,
// the resources rebuilt when the setting changes, 0x143709170, don't depend
// on it). With SetReflectionCatchUp on, one cave (MinHook's jump at
// 0x143702B9C, the code there checked first) uses 6 for that copy: a whole
// cubemap a frame, still no more than the caller's room. Off, it does
// exactly what the game's code does.
namespace
{
  const __int64 ReflectionFacesRva = 0x3702B9C;   // cmp eax, ecx; cmovb ecx, eax
  const int ReflectionCaveFlag = 0x40;

  BYTE* g_ReflectionCave = nullptr;
  LPVOID g_ReflectionTrampoline = nullptr;
}

static bool IsReflectionFacesCode(BYTE const* pBase)
{
  // mov eax, [r10 + 0x96DC]; cmovb ecx, [rsp + 0x70]; cmp eax, ecx; cmovb ecx, eax; sub edx, r8d; mov eax, edx
  static const BYTE code[] = { 0x41, 0x8B, 0x82, 0xDC, 0x96, 0x00, 0x00, 0x0F, 0x42, 0x4C, 0x24, 0x70, 0x39, 0xC8, 0x0F, 0x42,
    0xC8, 0x44, 0x29, 0xC2, 0x89, 0xD0 };
  return memcmp(pBase + ReflectionFacesRva - 12, code, sizeof(code)) == 0;
}

void util::hooks::SetReflectionCatchUp(bool on)
{
  if (g_ReflectionCave)
    *(volatile BYTE*)(g_ReflectionCave + ReflectionCaveFlag) = on ? 1 : 0;
}

// The cave (see above): with the flag on eax = 6, then the game's two
// instructions and back. eax is written again two instructions after
// (0x143702BA4), the flags come from the game's own cmp.
static void HookReflectionFaces()
{
  BYTE* pBase = (BYTE*)g_gameHandle;
  if (!IsReflectionFacesCode(pBase))
  {
    util::log::Warning("The reflection captures' code isn't what the tools know, reflections catch up at the game's pace");
    return;
  }
  BYTE* pCave = AllocNearGame(0x1000);
  if (!pCave)
    pCave = (BYTE*)VirtualAlloc(nullptr, 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
  if (!pCave)
  {
    util::log::Warning("No memory for the reflection captures' code, reflections catch up at the game's pace");
    return;
  }
  BYTE* p = pCave;
  auto put = [&](std::initializer_list<BYTE> bytes) { for (BYTE b : bytes) *p++ = b; };
  BYTE* pFlag = pCave + ReflectionCaveFlag;
  put({ 0x80, 0x3D }); int rel = (int)(pFlag - (p + 5)); memcpy(p, &rel, 4); p += 4; put({ 0x00 });  // cmp byte [flag], 0
  put({ 0x74, 0x05 });                                                                              // je (past the mov)
  put({ 0xB8, 0x06, 0x00, 0x00, 0x00 });                                                            // mov eax, 6
  put({ 0x39, 0xC8 });                                                                              // cmp eax, ecx
  put({ 0x0F, 0x42, 0xC8 });                                                                        // cmovb ecx, eax
  put({ 0xFF, 0x25, 0x00, 0x00, 0x00, 0x00 });                                                      // jmp [back]
  __int64 back = (__int64)(pBase + ReflectionFacesRva + 5);
  memcpy(p, &back, 8);
  *pFlag = 0;
  FlushInstructionCache(GetCurrentProcess(), pCave, 0x1000);

  BYTE* pTarget = pBase + ReflectionFacesRva;
  MH_STATUS status = MinHookCreate(pTarget, pCave, &g_ReflectionTrampoline);
  if (status == MH_OK && (status = MH_EnableHook(pTarget)) != MH_OK)
    MH_RemoveHook(pTarget);
  if (status != MH_OK)
  {
    util::log::Warning("The reflection captures' code couldn't be changed (MH_STATUS 0x%X), reflections catch up at the game's pace", status);
    return;
  }
  util::hooks::Hook info{ 0 };
  info.Address = (__int64)pTarget;
  info.Type = util::hooks::HookType::MinHook;
  info.Enabled = true;
  NoteCreatedHook("ReflectionFaces", info);
  util::log::Ok("Hooked ReflectionFaces at 0x%I64X", (__int64)pTarget);
  g_ReflectionCave = pCave;
}

// The game's own crash quitting during a Dash (found by GridNode,
// github.com/CodeNameMeteor/Gridnode, 2026-11-05: it crashed twice there;
// read here offline from the game's code, 2026-11-06). 0x1439E08F0 (its
// clean-up in the mission code, as GridNode reads it; called from four
// places) takes the server world from its global (0x1425469E0, null between
// levels) and asks 0x143164D50(class 0x142864420, world) for the first entity
// of a class in that world: movsxd rax, [rdx + 0x28] (the world's index),
// then the list head in the class. Quitting during a Dash the world is
// already gone, and that reads address 0x28. With no world there's no such
// entity: 0 skips its loop (test rax, rax; je 0x1439E0966), as when there's
// none. MinHook's jump replaces that one call (0x1439E0938; the other 239
// callers of 0x143164D50 are left alone) with a cave that makes the game's
// call when there's a world, else counts and answers 0, then goes back
// after the call. The cave calls nothing of the tools', and the hook stays
// while the game closes (Keep), when the crash comes.
namespace
{
  const __int64 CleanupCallRva = 0x39E0938;     // call 0x143164D50
  const __int64 WorldListRva = 0x3164D50;
  const int CleanupCaveCall = 0x40;             // the game's 0x143164D50
  const int CleanupCaveCount = 0x48;            // answers for a missing world (LONG)
  const int CleanupCaveBack = 0x50;             // after the call
  BYTE* g_CleanupCave = nullptr;
  LPVOID g_CleanupTrampoline = nullptr;
  LONG g_CleanupLogged = 0;
}

static bool IsMissionCleanupCode(BYTE const* pBase)
{
  // mov rdx, [0x1425469E0]; lea rcx, [0x142864420]; call 0x143164D50; test rax, rax; je 0x1439E0966
  static const BYTE site[] = { 0x48, 0x8B, 0x15, 0xAF, 0x60, 0xB6, 0xFE, 0x48, 0x8D, 0x0D, 0xE8, 0x3A, 0xE8, 0xFE, 0xE8, 0x13,
    0x44, 0x78, 0xFF, 0x48, 0x85, 0xC0, 0x74, 0x24 };
  // movsxd rax, [rdx + 0x28]; add rax, 6; add rax, rax; mov rax, [rcx + rax * 8]; ret
  static const BYTE list[] = { 0x48, 0x63, 0x42, 0x28, 0x48, 0x83, 0xC0, 0x06, 0x48, 0x01, 0xC0, 0x48, 0x8B, 0x04, 0xC1, 0xC3 };
  return !memcmp(pBase + CleanupCallRva - 14, site, sizeof(site)) && !memcmp(pBase + WorldListRva, list, sizeof(list));
}

static void HookMissionCleanup()
{
  BYTE* pBase = (BYTE*)g_gameHandle;
  if (!IsMissionCleanupCode(pBase))
  {
    util::log::Warning("The game's mission clean-up isn't the code the tools know, quitting during a Dash isn't guarded");
    return;
  }
  BYTE* pCave = AllocNearGame(0x1000);
  if (!pCave)
    pCave = (BYTE*)VirtualAlloc(nullptr, 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
  if (!pCave)
  {
    util::log::Warning("No memory for the mission clean-up's guard, quitting during a Dash isn't guarded");
    return;
  }
  // Entered by MinHook's jump in place of the call, so the stack is as the
  // call had it (the game's own shadow space below it); flags are the game's
  // to set again (its test rax, rax)
  BYTE* p = pCave;
  auto put = [&](std::initializer_list<BYTE> bytes) { for (BYTE b : bytes) *p++ = b; };
  auto rip = [&](int slot) { int rel = (int)(pCave + slot - (p + 4)); memcpy(p, &rel, 4); p += 4; };
  put({ 0x48, 0x81, 0xFA, 0x00, 0x00, 0x01, 0x00 });    // cmp rdx, 0x10000
  put({ 0x72, 0x00 });                                  // jb (no world; its distance below)
  BYTE* pJumpEnd = p;
  put({ 0xFF, 0x15 }); rip(CleanupCaveCall);            // call [the game's]
  put({ 0xFF, 0x25 }); rip(CleanupCaveBack);            // jmp [back]
  BYTE* pNoWorld = p;
  put({ 0xF0, 0xFF, 0x05 }); rip(CleanupCaveCount);     // no world: lock inc dword [count]
  put({ 0x31, 0xC0 });                                  // xor eax, eax (none)
  put({ 0xFF, 0x25 }); rip(CleanupCaveBack);            // jmp [back]
  pJumpEnd[-1] = (BYTE)(pNoWorld - pJumpEnd);
  if (p - pCave > CleanupCaveCall || pNoWorld - pJumpEnd != 12)
  {
    VirtualFree(pCave, 0, MEM_RELEASE);
    util::log::Warning("The mission clean-up's guard didn't fit its cave, quitting during a Dash isn't guarded");
    return;
  }
  __int64 call = (__int64)(pBase + WorldListRva);
  __int64 back = (__int64)(pBase + CleanupCallRva + 5);
  memcpy(pCave + CleanupCaveCall, &call, 8);
  *(LONG*)(pCave + CleanupCaveCount) = 0;
  memcpy(pCave + CleanupCaveBack, &back, 8);
  FlushInstructionCache(GetCurrentProcess(), pCave, 0x1000);

  BYTE* pTarget = pBase + CleanupCallRva;
  MH_STATUS status = MinHookCreate(pTarget, pCave, &g_CleanupTrampoline);
  if (status == MH_OK && (status = MH_EnableHook(pTarget)) != MH_OK)
    MH_RemoveHook(pTarget);
  if (status != MH_OK)
  {
    util::log::Warning("The game's mission clean-up couldn't be changed (MH_STATUS 0x%X), quitting during a Dash isn't guarded",
      status);
    return;
  }
  util::hooks::Hook info{ 0 };
  info.Address = (__int64)pTarget;
  info.Type = util::hooks::HookType::MinHook;
  info.Enabled = true;
  info.Keep = true;
  NoteCreatedHook("MissionCleanupGuard", info);
  util::log::Ok("Hooked MissionCleanupGuard at 0x%I64X (quitting during a Dash: no server world is taken as none)",
    (__int64)pTarget);
  g_CleanupCave = pCave;
}

void util::hooks::LogMissionCleanupGuard()
{
  if (!g_CleanupCave)
    return;
  LONG count = *(LONG const volatile*)(g_CleanupCave + CleanupCaveCount);
  if (count == g_CleanupLogged)
    return;
  util::log::Warning("The game's mission clean-up ran with no server world (quitting during a Dash): answered none, %d time%s "
    "in all (without the guard the game crashes there)", (int)count, count == 1 ? "" : "s");
  g_CleanupLogged = count;
}

// The game's job queue (the hang of 2026-10-25 17:23, offline from the hung
// threads' stacks; Research_JobQueue_Hang_2026-10-25.md). The job manager
// ([[0x142373E70]], made by 0x142A94750 -> 0x142FBA890 -> 0x142FB7890) keeps
// the jobs any thread may run in one queue (manager + 0x20, 0x142FBA9B0: + 0x00
// how many are in it, + 0x08 its slots, + 0x10 its room, + 0x20 its free
// nodes), room for 1024 (0x142A94841: mov r8d, 0x400). When a job is done, the
// thread that ran it hands on the jobs that waited for it (0x142FB9DB0 ->
// 0x142FBB2F0 -> 0x142FB9F00 -> 0x142FB8F20), one for any thread through
// 0x142FB9250: when the queue is full (0x142FBC0C0 < 0) that tries again
// after SwitchToThread until it isn't. Only with the manager's help byte
// (+ 0x8F0, set when Windows reports 2 processors, 0x142A94851) does the thread
// first run a queued job itself (0x142FB8800 / 0x142FBB400 / 0x142FBA400, what
// the game's waits use), making room. Without it, once every running thread
// is handing jobs on into a full queue nobody takes one out: the game hung at
// 17:23:38 (a mission's time of day change during a replay recording, the
// catch-up's bigger bounce light budget, a whole cubemap a frame, the whole
// city loaded: six threads in 0x142FB9250, the main thread on
// "waitRenderSync"), and at the freeze of 2026-10-24 01:56 (the same frames,
// 0x142FB9051 -> 0x142FBC0E4). The cave at 0x142FB92D0 (the help byte's cmp,
// the code around it checked first) counts each time and goes to the help as
// the game does on 2 processors. The help byte stays the game's: two job waits
// read it too (0x142C3E690, 0x142C3E740).
//
// That help hung the game itself (2026-10-28 04:30, the loading screen of
// Release while the replay recorded, after 541 helps; read live, read-only):
// a worker running a job was handing a job on into the full queue
// (0x1435CE4D3 -> 0x142FB8F20 -> 0x142FB9250), the help ran another queued job
// on that thread in its place (0x142FB92F9 -> 0x142FBB400) and that job waits
// (0x142A9AFF8's loop) for a job that never finishes, with the thread under it
// still owing its hand-on; a second thread waits there too. The quit that hung
// at 04:28 had a thread in the same wait. So the full queue's jobs are now
// kept by the tools instead: 0x142FB9250 has one caller, 0x142FB8F20 (hand a
// job on: the right queue, then 0x142FB97F0 wakes a worker), which doesn't
// look at what it returns. The cave keeps (manager, job) and leaves through
// the game's own way out (0x142FB9384), and a thread of the tools hands each
// kept job on with 0x142FB8F20 once the queue has room again, the oldest
// first. No thread runs another's job in its place and none waits on the full
// queue. The queue can't simply be made bigger: every take looks through all
// of its slots for the best job (0x142FBA210), so a bigger queue slows each.
namespace
{
  const __int64 JobQueueHelpRva = 0x2FB92D0;    // cmp byte [rbp + 0x8F0], r13b; je (the help)
  const __int64 JobQueueOutRva = 0x2FB9384;     // the push's way out: mov rsi, [rsp + 0x48]; ...; ret
  const __int64 JobHandOnRva = 0x2FB8F20;       // hand a job on (manager, job): the queue it goes in, a worker woken
  const __int64 JobSystemRva = 0x2373E70;       // [ ]: the job system, its + 0 the job manager
  const int JobQueueCountOffset = 0x20;         // in the manager: the queue's how many
  const int JobQueueRoomOffset = 0x30;          // and its room
  const int JobQueueCaveCount = 0x40;
  const int JobQueueCaveKeep = 0x48;            // the cave's data: KeepJob, the trampoline, the way out
  const int JobQueueCaveTrampoline = 0x50;
  const int JobQueueCaveOut = 0x58;
  // A kept job is handed on while the queue has at least this many free
  const int JobQueueHandOnRoom = 16;

  BYTE* g_JobQueueCave = nullptr;
  LPVOID g_JobQueueTrampoline = nullptr;
  std::atomic<int> g_JobQueueMost{ 0 };         // the most seen waiting (SampleJobQueue)
  // The jobs kept while the queue was full (KeepJob, any game thread), handed
  // on by HandOnKeptJobs (the tools' thread); how many in all, the most at once
  SRWLOCK g_KeptJobsLock = SRWLOCK_INIT;
  std::vector<std::pair<__int64, __int64>> g_KeptJobs;   // manager, job; the oldest first
  size_t g_KeptJobsFirst = 0;                   // the next to hand on (the ones before it handed on)
  HANDLE g_KeptJobsEvent = nullptr;
  std::atomic<long> g_KeptJobsTotal{ 0 }, g_HandedOnTotal{ 0 }, g_HandOnFaults{ 0 };
  std::atomic<int> g_KeptJobsMost{ 0 };
  long g_KeptJobsLogged = 0;                    // the tools' loop's
  // The tools' loop's
  LONG g_JobQueueFullLogged = 0;
  ULONGLONG g_JobQueueFullLoggedTick = 0;
  int g_JobQueueRoom = 0;
  int g_JobQueueMostLogged = 0;

  // The time jumps (NoteTimeJump, the game's thread): how many, the last one's
  // time, the clock before and after it
  std::atomic<long> g_TimeJumps{ 0 };
  std::atomic<ULONGLONG> g_TimeJumpTick{ 0 };
  std::atomic<float> g_TimeJumpFrom{ -1.f };
  std::atomic<float> g_TimeJumpTo{ -1.f };
  // The queue's peak looked for until this long after the last jump (the
  // catch-up's 3 s, the reset's passes, the reflections captured again)
  const ULONGLONG QueueJumpWindowMs = 10000;
  // That window, the tools' loop's (SampleJobQueue, LogJobQueue)
  long g_QueueJumpsSeen = 0;
  bool g_QueueJumpOpen = false;
  ULONGLONG g_QueueJumpStart = 0, g_QueueJumpLast = 0, g_QueueJumpPeakTick = 0;
  int g_QueueJumpCount = 0, g_QueueJumpMost = 0;
  float g_QueueJumpFrom = -1.f, g_QueueJumpTo = -1.f;
  LONG g_QueueJumpFullStart = 0;

  void FormatQueueClock(float seconds, char* out, size_t size)
  {
    if (!(seconds >= 0.f))
    {
      strcpy_s(out, size, "?");
      return;
    }
    int s = (int)seconds;
    sprintf_s(out, size, "%02d:%02d", (s / 3600) % 24, (s / 60) % 60);
  }

  LONG JobQueueFullCount()
  {
    return g_JobQueueCave ? *(LONG const volatile*)(g_JobQueueCave + JobQueueCaveCount) : 0;
  }

  bool ReadJobQueue(int& count, int& room)
  {
    __try
    {
      __int64 system = *(__int64 const*)((__int64)g_gameHandle + JobSystemRva);
      if (system < 0x10000)
        return false;
      __int64 manager = *(__int64 const*)system;
      if (manager < 0x10000)
        return false;
      count = *(int const volatile*)(manager + JobQueueCountOffset);
      room = *(int const*)(manager + JobQueueRoomOffset);
      return room > 0;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      return false;
    }
  }
}

static bool IsJobQueueCode(BYTE const* pBase)
{
  // 0x142FB9250: push rbp; push r12; push r13; push r14; push r15; sub rsp, 0x50; mov r15, rdx; mov rbp, rcx;
  // movzx eax, byte [rcx + 0x10]; test al, al; jne
  static const BYTE start[] = { 0x55, 0x41, 0x54, 0x41, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x83, 0xEC, 0x50, 0x49, 0x89, 0xD7,
    0x48, 0x89, 0xCD, 0x0F, 0xB6, 0x41, 0x10, 0x84, 0xC0, 0x75, 0xF8 };
  // the first try: lea rcx, [rbp + 0x20]; mov rdx, r15; call 0x142FBC0C0; test eax, eax; jns (done)
  static const BYTE push[] = { 0x48, 0x8D, 0x4D, 0x20, 0x4C, 0x89, 0xFA };
  static const BYTE pushed[] = { 0x85, 0xC0, 0x0F, 0x89, 0xFC, 0x00, 0x00, 0x00 };
  // cmp byte [rbp + 0x8F0], r13b; je +0x37; mov rsi, [rbp + 8]; mov rcx, rsi; call 0x142FB8800
  static const BYTE help[] = { 0x44, 0x38, 0xAD, 0xF0, 0x08, 0x00, 0x00, 0x74, 0x37, 0x48, 0x8B, 0x75, 0x08, 0x48, 0x89, 0xF1 };
  // the try again: lea rcx, [rbp + 0x20]; mov rdx, r15; call 0x142FBC0C0; test eax, eax; js 0x142FB92D0
  static const BYTE again[] = { 0x48, 0x8D, 0x4D, 0x20, 0x4C, 0x89, 0xFA };
  static const BYTE againFull[] = { 0x85, 0xC0, 0x0F, 0x88, 0x4C, 0xFF, 0xFF, 0xFF };
  // The way out: mov rsi, [rsp + 0x48]; mov rdi, [rsp + 0x40]; mov rbx, [rsp + 0x88]; movaps xmm6, [rsp + 0x30];
  // movaps xmm7, [rsp + 0x20]; add rsp, 0x50; pop r15; pop r14; pop r13; pop r12; pop rbp; ret
  static const BYTE out[] = { 0x48, 0x8B, 0x74, 0x24, 0x48, 0x48, 0x8B, 0x7C, 0x24, 0x40, 0x48, 0x8B, 0x9C, 0x24, 0x88, 0x00,
    0x00, 0x00, 0x0F, 0x28, 0x74, 0x24, 0x30, 0x0F, 0x28, 0x7C, 0x24, 0x20, 0x48, 0x83, 0xC4, 0x50, 0x41, 0x5F, 0x41, 0x5E,
    0x41, 0x5D, 0x41, 0x5C, 0x5D, 0xC3 };
  // 0x142FB8F20, hand a job on: mov rax, rsp; push rbp; push r14; push r15; sub rsp, 0x100; and its push
  // (mov rcx, rbx; call 0x142FB9250) and the worker woken after it (call 0x142FB97F0)
  static const BYTE handOn[] = { 0x48, 0x89, 0xE0, 0x55, 0x41, 0x56, 0x41, 0x57, 0x48, 0x81, 0xEC, 0x00, 0x01, 0x00, 0x00 };
  static const BYTE handOnPush[] = { 0x48, 0x89, 0xD9 };
  auto at = [&](__int64 rva, BYTE const* code, size_t size) { return memcmp(pBase + rva, code, size) == 0; };
  auto goesTo = [&](__int64 rva, __int64 target)
  {
    int rel;
    memcpy(&rel, pBase + rva + 1, 4);
    return pBase[rva] == 0xE8 && rva + 5 + rel == target;
  };
  IMAGE_NT_HEADERS const* pNt = (IMAGE_NT_HEADERS const*)(pBase + ((IMAGE_DOS_HEADER const*)pBase)->e_lfanew);
  return pNt->FileHeader.TimeDateStamp == 0x5762E3B7 && at(0x2FB9250, start, sizeof(start)) &&
    at(0x2FB9290, push, sizeof(push)) && goesTo(0x2FB9297, 0x2FBC0C0) && at(0x2FB929C, pushed, sizeof(pushed)) &&
    at(JobQueueHelpRva, help, sizeof(help)) && goesTo(0x2FB92E0, 0x2FB8800) && goesTo(0x2FB92F9, 0x2FBB400) &&
    goesTo(0x2FB9307, 0x2FBA400) && at(0x2FB9370, again, sizeof(again)) && goesTo(0x2FB9377, 0x2FBC0C0) &&
    at(0x2FB937C, againFull, sizeof(againFull)) && goesTo(0x2FB904C, 0x2FB9250) && at(JobQueueOutRva, out, sizeof(out)) &&
    at(JobHandOnRva, handOn, sizeof(handOn)) && at(0x2FB9049, handOnPush, sizeof(handOnPush)) &&
    goesTo(0x2FB9088, 0x2FB97F0);
}

// A job that found the queue full (from the cave, on the game's thread that
// was handing it on): kept to be handed on later. False: it couldn't be (no
// memory), the game's own code goes on (it waits for room)
static bool KeepJob(__int64 manager, __int64 job)
{
  size_t now = 0;
  AcquireSRWLockExclusive(&g_KeptJobsLock);
  try
  {
    g_KeptJobs.emplace_back(manager, job);
    now = g_KeptJobs.size() - g_KeptJobsFirst;
  }
  catch (...)
  {
    ReleaseSRWLockExclusive(&g_KeptJobsLock);
    return false;
  }
  ReleaseSRWLockExclusive(&g_KeptJobsLock);
  ++g_KeptJobsTotal;
  int most = g_KeptJobsMost.load(std::memory_order_relaxed);
  while ((int)now > most && !g_KeptJobsMost.compare_exchange_weak(most, (int)now, std::memory_order_relaxed))
  {
  }
  SetEvent(g_KeptJobsEvent);
  return true;
}

static void HandOnJob(__int64 manager, __int64 job)
{
  typedef void(__fastcall* tHandOn)(__int64 manager, __int64 job);
  __try
  {
    ((tHandOn)((BYTE*)g_gameHandle + JobHandOnRva))(manager, job);
  }
  __except (EXCEPTION_EXECUTE_HANDLER)
  {
    ++g_HandOnFaults;
  }
}

// The tools' thread that hands the kept jobs on, the oldest first, each once
// the queue has JobQueueHandOnRoom free (one that finds it full again is kept
// again, at the end). It never stops, the tools closing too (the game may wait
// for a kept job while it closes)
static DWORD WINAPI HandOnKeptJobs(LPVOID)
{
  for (;;)
  {
    WaitForSingleObject(g_KeptJobsEvent, 100);
    for (;;)
    {
      std::pair<__int64, __int64> next(0, 0);
      AcquireSRWLockExclusive(&g_KeptJobsLock);
      bool any = g_KeptJobsFirst < g_KeptJobs.size();
      if (!any)
      {
        g_KeptJobs.clear();
        g_KeptJobsFirst = 0;
      }
      ReleaseSRWLockExclusive(&g_KeptJobsLock);
      if (!any)
        break;
      int count = 0, room = 0;
      if (!ReadJobQueue(count, room) || count > room - JobQueueHandOnRoom)
      {
        Sleep(1);
        continue;
      }
      AcquireSRWLockExclusive(&g_KeptJobsLock);
      if (g_KeptJobsFirst < g_KeptJobs.size())
        next = g_KeptJobs[g_KeptJobsFirst++];
      ReleaseSRWLockExclusive(&g_KeptJobsLock);
      if (next.second)
      {
        HandOnJob(next.first, next.second);
        ++g_HandedOnTotal;
      }
    }
  }
}

// The cave (see above): counts, keeps the job (KeepJob) and leaves through the
// push's way out; when it couldn't be kept, the game's own code (the
// trampoline: the help byte's cmp and je)
static void HookJobQueueFull()
{
  BYTE* pBase = (BYTE*)g_gameHandle;
  if (!IsJobQueueCode(pBase))
  {
    util::log::Warning("The game's job queue code isn't what the tools know: a full job queue can still hang the game");
    return;
  }
  BYTE* pCave = AllocNearGame(0x1000);
  if (!pCave)
    pCave = (BYTE*)VirtualAlloc(nullptr, 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
  if (!pCave)
  {
    util::log::Warning("No memory for the job queue's code: a full job queue can still hang the game");
    return;
  }
  // The thread that hands the kept jobs on, before any is kept
  g_KeptJobsEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  HANDLE handOn = g_KeptJobsEvent ? CreateThread(nullptr, 0, HandOnKeptJobs, nullptr, 0, nullptr) : nullptr;
  if (!handOn)
  {
    util::log::Warning("No thread for the job queue's kept jobs (GetLastError 0x%X): a full job queue can still hang the game",
      GetLastError());
    return;
  }
  CloseHandle(handOn);

  BYTE* p = pCave;
  auto put = [&](std::initializer_list<BYTE> bytes) { for (BYTE b : bytes) *p++ = b; };
  auto rip = [&](int data) { int rel = (int)(pCave + data - (p + 4)); memcpy(p, &rel, 4); p += 4; };
  put({ 0xF0, 0xFF, 0x05 }); rip(JobQueueCaveCount);         // lock inc dword [count]
  put({ 0x48, 0x89, 0xE9 });                                  // mov rcx, rbp (the manager)
  put({ 0x4C, 0x89, 0xFA });                                  // mov rdx, r15 (the job)
  put({ 0x48, 0x83, 0xEC, 0x20 });                            // sub rsp, 0x20 (rsp is 16-aligned there)
  put({ 0xFF, 0x15 }); rip(JobQueueCaveKeep);                 // call [KeepJob]
  put({ 0x48, 0x83, 0xC4, 0x20 });                            // add rsp, 0x20
  put({ 0x84, 0xC0 });                                        // test al, al
  put({ 0x74, 0x06 });                                        // je (not kept: the trampoline)
  put({ 0xFF, 0x25 }); rip(JobQueueCaveOut);                  // jmp [the way out]
  put({ 0xFF, 0x25 }); rip(JobQueueCaveTrampoline);           // jmp [the trampoline]
  *(LONG*)(pCave + JobQueueCaveCount) = 0;
  *(void**)(pCave + JobQueueCaveKeep) = (void*)&KeepJob;
  *(__int64*)(pCave + JobQueueCaveOut) = (__int64)(pBase + JobQueueOutRva);

  BYTE* pTarget = pBase + JobQueueHelpRva;
  MH_STATUS status = MinHookCreate(pTarget, pCave, &g_JobQueueTrampoline);
  if (status == MH_OK)
  {
    *(LPVOID*)(pCave + JobQueueCaveTrampoline) = g_JobQueueTrampoline;
    FlushInstructionCache(GetCurrentProcess(), pCave, 0x1000);
    if ((status = MH_EnableHook(pTarget)) != MH_OK)
      MH_RemoveHook(pTarget);
  }
  if (status != MH_OK)
  {
    util::log::Warning("The job queue's code couldn't be changed (MH_STATUS 0x%X): a full job queue can still hang the game", status);
    NoteFailedHook("JobQueueFull");
    return;
  }
  util::hooks::Hook info{ 0 };
  info.Address = (__int64)pTarget;
  info.Type = util::hooks::HookType::MinHook;
  info.Enabled = true;
  NoteCreatedHook("JobQueueFull", info);
  util::log::Ok("Hooked JobQueueFull at 0x%I64X (a job that finds the game's job queue full is kept by the tools and handed on "
    "once there's room; no thread runs another job in its place or waits for room)", (__int64)pTarget);
  g_JobQueueCave = pCave;
}

void util::hooks::NoteTimeJump(float from, float to)
{
  if (!util::DiagnosticsOn())   // it only feeds the job queue's log
    return;
  g_TimeJumpFrom = from;
  g_TimeJumpTo = to;
  g_TimeJumpTick = GetTickCount64();
  ++g_TimeJumps;
}

void util::hooks::SampleJobQueue()
{
  if (!util::DiagnosticsOn())
    return;
  // A time jump opens a window (or keeps it open) for the queue's peak after it
  long jumps = g_TimeJumps.load();
  if (jumps != g_QueueJumpsSeen)
  {
    ULONGLONG tick = g_TimeJumpTick.load();
    if (!g_QueueJumpOpen)
    {
      g_QueueJumpOpen = true;
      g_QueueJumpStart = tick;
      g_QueueJumpPeakTick = tick;
      g_QueueJumpCount = 0;
      g_QueueJumpMost = 0;
      g_QueueJumpFrom = g_TimeJumpFrom.load();
      g_QueueJumpFullStart = JobQueueFullCount();
    }
    g_QueueJumpCount += (int)(jumps - g_QueueJumpsSeen);
    g_QueueJumpLast = tick;
    g_QueueJumpTo = g_TimeJumpTo.load();
    g_QueueJumpsSeen = jumps;
  }

  int count = 0, room = 0;
  if (!ReadJobQueue(count, room))
    return;
  int most = g_JobQueueMost.load(std::memory_order_relaxed);
  while (count > most && !g_JobQueueMost.compare_exchange_weak(most, count, std::memory_order_relaxed))
  {
  }
  if (g_QueueJumpOpen && count > g_QueueJumpMost)
  {
    g_QueueJumpMost = count;
    g_QueueJumpPeakTick = GetTickCount64();
  }
}

void util::hooks::LogJobQueue()
{
  int count = 0, room = 0;
  if (!ReadJobQueue(count, room))
    return;
  if (util::DiagnosticsOn())   // the room and the peaks: diagnostics only
  {
    if (room != g_JobQueueRoom)
    {
      g_JobQueueRoom = room;
      g_JobQueueMostLogged = 0;
      util::log::Write("The game's job queue (jobs for any thread) has room for %d", room);
    }
    // The most waiting, logged when past another tenth of the room from half of it
    int most = g_JobQueueMost.exchange(0);
    int tenth = (int)((long long)most * 10 / room);
    if (tenth >= 5 && tenth > g_JobQueueMostLogged)
    {
      g_JobQueueMostLogged = tenth;
      util::log::Write("The game's job queue: up to %d of %d jobs waiting at once", most, room);
    }
  }
  // The peak after a time jump, once QueueJumpWindowMs passed without another
  ULONGLONG now = GetTickCount64();
  if (util::DiagnosticsOn() && g_QueueJumpOpen && now - g_QueueJumpLast >= QueueJumpWindowMs)
  {
    g_QueueJumpOpen = false;
    char from[16], to[16], jumps[32] = "";
    FormatQueueClock(g_QueueJumpFrom, from, sizeof(from));
    FormatQueueClock(g_QueueJumpTo, to, sizeof(to));
    if (g_QueueJumpCount > 1)
      sprintf_s(jumps, ", %d jumps in a row", g_QueueJumpCount);
    LONG fullThen = JobQueueFullCount() - g_QueueJumpFullStart;
    util::log::Write("The game's job queue after the time jump from %s to %s%s: at most %d of %d jobs waiting at once (%.0f %%), "
      "%.1f s after %s (sampled every ~5 ms by the tools); found full %d times", from, to, jumps, g_QueueJumpMost, room,
      100.0 * g_QueueJumpMost / room, (g_QueueJumpPeakTick - g_QueueJumpStart) / 1000.0,
      g_QueueJumpCount > 1 ? "the first" : "it", (int)fullThen);
  }
  // Found full (the cave's count), at most every 5 s
  if (!g_JobQueueCave)
    return;
  LONG full = *(LONG const volatile*)(g_JobQueueCave + JobQueueCaveCount);
  if (full != g_JobQueueFullLogged && now - g_JobQueueFullLoggedTick >= 5000)
  {
    long kept = g_KeptJobsTotal.load();
    AcquireSRWLockShared(&g_KeptJobsLock);
    size_t keptNow = g_KeptJobs.size() - g_KeptJobsFirst;
    ReleaseSRWLockShared(&g_KeptJobsLock);
    util::log::Warning("The game's job queue was full (%d jobs): %d jobs kept by the tools to be handed on once there's room (%d in "
      "all, %d handed on, at most %d kept at once, %d kept now%s; no thread ran another job in its place or waited), %d waiting "
      "now", room, (int)(kept - g_KeptJobsLogged), (int)kept, (int)g_HandedOnTotal.load(), g_KeptJobsMost.load(), (int)keptNow,
      g_HandOnFaults.load() ? ", some handed on with a fault" : "", count);
    g_KeptJobsLogged = kept;
    g_JobQueueFullLogged = full;
    g_JobQueueFullLoggedTick = now;
  }
}

// The time of day's lighting after a jump. TimeOfDayManagerEntity (data:
// SolveIterations + 0x18, UpdateInterval + 0x1C; 5 and 240 in the main city's
// lighting/visualenvironments/timeofday) takes the clock (+ 0x48) and gives
// Enlighten the time it works the bounce light out for (+ 0x38: the Future
// visual environments' sun and sky). Its update 0x143950920 (from the
// managers' update 0x14394DB80) goes in steps of three jobs (+ 0x78 / + 0x94
// / + 0xB0, 0x1C each: request id, state (4 idle), time, passes, done, frame,
// flag), each an Enlighten pass (0x143950750) into the next of three output
// buffers. The two not being written are shown, blended (0x14378D100; the
// blend from 0x143797350), and a new step (for the clock + 2 intervals) only
// starts when the blend has run UpdateInterval updates and the last pass is
// done. So after a jump the shade showed the old time's light for two or
// three steps whatever else was done (passes filed by the tools went into the
// buffer not shown yet). The game's own time skips send it Reset (0x14394AF80,
// event 0x0DB4B330): 0x14394F050 sets the jobs to the clock's time,
// SolveIterations passes there (the first starts the bounce light again from
// nothing, 0x143794AC0, so every system's first FullResolveLateAddIterations
// (5 live) solves go into all three buffers, 0x143790060: shown at once), then
// one pass at + 1 interval and one at + 2. RequestTodReset has the hook give it
// that Reset right after one of its updates, once no pass of its own is with
// Enlighten (job states 1 - 3), so no request is left unreleased. While a
// reset of ours is in its first jobs (+ 0x5C set, the game's Reset would be
// skipped) a newer one goes straight through 0x14394F050 the same way, at a
// pass boundary; a reset of the game's own is waited for. Nothing while its
// countdown to a reset runs (+ 0x60, -1 otherwise), without its Enlighten jobs
// (+ 0x68), or when what it lights is within one interval of the clock already
// (its first job's time: the clock at its last step). Its clock is the
// client's (ClientTimeOfDayClockEntity, [[0x142598AC0] + 0x28] + 0x30), while
// the tools set the server's ([[[0x14255C2F8] + 8] + 0x28] + 0x30), which the
// client's follows a little later: one update after a day -> night jump it
// still had the old time, so the reset was found not needed and dropped
// (2026-10-25 04:35:28). So a request carries the time the tools set, and its
// own clock is waited for until it's within TodArrivedSeconds of that, at most
// TodClockWaitMs.
// "Already there" goes by the light shown (TodShown), not by its first job's
// time: a step of its own sets job 0 to the clock with no pass (job 2's is
// the step's only one, shown two steps later). After a slider drag on
// 2026-11-13 13:43:41 a step had just started at 08:54 while the light shown
// was still the reset's (11:21 - 11:29), and the reset at 08:54 was dropped as
// "already at 08:54". So each pass into a new output buffer is noted as it's
// filed: a job's request id changed (+ 0, 0x1439507E1) with its flag set
// (+ 0x18, the request's buffer flag, 0x1439507C5; 0x14379A8B1 moves the
// buffer written, + 0x10858, on it). In Enlighten's mode 1 (+ 0x10780, live)
// the light shown is the two buffers after the one written last ((i + 1) % 3
// and (i + 2) % 3, 0x14378D10A): the two passes before the newest. (The
// reset's first SolveIterations passes go into all three buffers, and they're
// noted as that many at its time, so they fill all three here too.) No reset
// when both are within 2 of the game's own intervals + 120 s of the clock: a
// step of its own lights 2 intervals ahead, so at its own pace (~330 game s a
// step) the light shown runs up to ~510 game s off the clock, 480 with the
// clock stopped.
namespace
{
  const __int64 TodStepRva = 0x3950920;
  const __int64 TodStepCallRva = 0x394DC1F;
  const __int64 TodEventRva = 0x394AF80;
  const __int64 TodEventInitJumpRva = 0x394AFEE;
  const __int64 TodInitRva = 0x394F050;
  const __int64 TodPropertyRva = 0x2A424B0;   // a property's value: its connection's when it has one, else its own
  const __int64 TodFloatTypeRva = 0x27B0CB8;
  const unsigned int TodResetEvent = 0x0DB4B330;   // "Reset"
  const int TodDataOffset = 0x28;
  const int TodSolveIterationsOffset = 0x18;   // in the data
  const int TodUpdateIntervalOffset = 0x1C;    // in the data
  const int TodClockOffset = 0x48;
  const int TodResetPendingOffset = 0x5C;
  const int TodCountdownOffset = 0x60;
  const int TodJobsOnOffset = 0x68;
  const int TodJobsOffset = 0x78;
  const int TodJobSize = 0x1C;
  const int TodJobStateOffset = 4;
  const int TodJobTimeOffset = 8;
  const int TodJobIdle = 4;
  const ULONGLONG TodWatchMs = 60000;
  const ULONGLONG TodManagerGoneMs = 2000;
  const ULONGLONG TodFreshRequestMs = 2000;
  const float TodArrivedSeconds = 120.f;
  const ULONGLONG TodClockWaitMs = 10000;
  const int TodJobIdOffset = 0;
  const int TodJobFlagOffset = 0x18;          // set: its passes go into a new output buffer
  const unsigned int TodNoRequest = 0xFFFFFFFF;
  const __int64 TodEnlightenRva = 0x256AF40;
  const int TodEnlightenModeOffset = 0x10780;
  // Reflections after a reset (2026-11-13, measured, report section 10): they
  // were final 4 - 5 s after a jump. They're captured again (TakeTodResetSettled)
  // once TodEarlyPasses of the reset's first job are done (its count + 0x10,
  // 0x143950827) and again when all are, so they show the new light about a
  // second sooner and end with all of it. And the reset's later two jobs are
  // given the reset's own time, not + 1 and + 2 intervals: each time the
  // manager's output time moved (the third job, + 8 minutes), the Future sky
  // changed, the game's sky cubemap was captured again and that marked every
  // reflection to be captured again (0x1436FAB20 -> 0x1436FC9E0,
  // LocalIBLUpdateWithEnlightenSkyBoxChange): the round going on started over,
  // 1.1 - 1.3 s after the tools' recapture, both jumps measured (taken from the
  // timing, not traced through the VE versions; the tools' log now says what
  // marks them, VisualsManager::NoteReflectionMarks).
  const int TodJobDoneOffset = 0x10;
  const int TodEarlyPasses = 3;

  typedef void(__fastcall* tTodStep)(__int64 manager, __int64 info);
  typedef void(__fastcall* tTodEvent)(__int64 manager, __int64 event);
  typedef void(__fastcall* tTodInit)(__int64 manager, float time, float interval);
  typedef float*(__fastcall* tTodProperty)(__int64 property, __int64 type);
  tTodStep oTodStep = nullptr;

  // The managers seen, the game thread's (their update)
  struct TodManager
  {
    __int64 Manager;
    ULONGLONG Seen;
    long Done;            // the last request done (or not needed) for it
    bool Running;         // a reset of ours whose jobs aren't all done
    bool FirstDone;       // its first job (SolveIterations passes) done
    bool EarlyDone;       // TodEarlyPasses of them done
    ULONGLONG Started;
    float Time;
    int Passes;
    long Waiting;         // the request its clock is waited for (see TodClockWaitMs), since
    ULONGLONG WaitStart;
    float WaitClock;      // its clock when that request was first looked at (WaitClockSet)
    bool WaitClockSet;
    long ResetFor;        // the request the reset running (or last run) is for (TodResetOutcome)
    bool Quiet;           // its lines left out (a time-lapse frame's, see g_TodQuietId)
    float ClockAtReset;   // its clock when the reset was done (the time lit is t.Time)
    // + 0x5C set by a reset of ours and not seen clear since: a newer one goes through 0x14394F050 as while it runs
    // (what clears + 0x5C isn't known: maybe one of its own steps, which a time-lapse holds)
    bool OursPending;
    long WarnedFor;       // the request its clock's "didn't get to" warning was written for (once each)
    // The reset running is a time-lapse frame's: no TodWatchMs on it (that's real time, and the frame may wait
    // through a pause, a freeze or a low game speed; the time-lapse gives up after its own game time instead)
    bool Forced;
    // Its passes into a new output buffer as they're filed (see TodShown):
    // each job's request id when last looked at; the times of the last three,
    // newest last; how many of those are known
    bool JobsSeen;
    unsigned int JobIds[3];
    float Lit[3];
    int LitCount;
  };
  TodManager g_TodManagers[4] = {};
  std::atomic<long> g_TodWanted{ 0 };
  std::atomic<float> g_TodTarget{ -1.f };   // the time the last request was for, -1 for none
  std::atomic<ULONGLONG> g_TodWantedTick{ 0 };
  std::atomic<bool> g_TodBusy{ false };
  // A reset's first job: its passes done when the reflections should be
  // captured again (0: not now), and of how many (TakeTodResetSettled)
  std::atomic<int> g_TodSettled{ 0 };
  std::atomic<int> g_TodSettledOf{ 0 };
  bool g_TodDoneKnown = false;   // IsTodDoneCode
  std::atomic<ULONGLONG> g_TodStepTick{ 0 };
  bool g_TodHooked = false;
  bool g_TodShownKnown = false;   // IsTodShownCode

  // Following a clock that runs fast (2026-11-06, the user: "Improve more the
  // Light catches up with time jumps"; offline, the update 0x143950920). Its
  // steps don't go by the clock: each update adds its frames (dt x 30,
  // 0x143950A78; dt = frames / 30, 0x14394DC12) to + 0x58, and a new step
  // starts once that's grown UpdateInterval since the blend began (+ 0x6C,
  // its progress x + 0x70 = 1 / interval, 0x143950A9E) and job 2's pass is
  // done; the step lights the clock, + 1 interval and + 2 (job 2's pass).
  // Built for the clock's own pace, 30 game seconds a game second: a step
  // every 8 s, 240 s ahead. A time-lapse faster than that kept one step's
  // light for 8 s (the 15:24:44 track: 00:02 - 20:19 in 5 s, one step at
  // 20:19). While the clock runs forward faster than TodFollowStart game
  // seconds a real second, + 0x58 grows by how far the manager's own clock
  // went instead (the update's dt x 30 taken off before it adds it), and
  // UpdateInterval is how far the clock goes in a step's time (as the steps
  // come, at least TodFollowShortest real seconds: about a pass with the
  // catch-up's budget), at least the game's own, at most TodFollowLongest:
  // its steps keep up, each lighting where the clock will be. Below
  // TodFollowStop, on a cut (more than TodFollowCut in one update: the reset
  // takes those) or a pause, its own interval goes back on its data at once.
  const int TodAccOffset = 0x58;
  const __int64 TodStepRateRva = 0x1A6A454;   // the 30 its frames are counted by
  const float TodStepRate = 30.f;
  const float TodFollowStart = 600.f;
  const float TodFollowStop = 300.f;
  const float TodFollowCut = 7200.f;
  const float TodFollowLongest = 21600.f;
  const double TodFollowShortest = 0.3;
  const double TodFollowSlowest = 2.0;
  const double TodFollowPause = 0.5;   // real seconds between its updates: past that a pause, not a pace
  struct TodFollow
  {
    __int64 Manager;
    __int64 Data;         // whose UpdateInterval was set (driving)
    float Original;       // its own
    float LastClock;
    float LastLit;        // job 0's time: a new one is a step
    double LastTime;      // real seconds (TodNow), -1: none yet
    float Speed;          // game seconds a real second, smoothed
    bool Driving;
    double Started;
    double LastStep;
    double StepPeriod;    // real seconds between its steps, smoothed
    int Steps;
  };
  TodFollow g_TodFollow = { 0, 0, 0.f, 0.f, 0.f, -1.0, 0.f, false, 0.0, 0.0, 0.5, 0 };
  std::atomic<bool> g_TodFollowWanted{ false };
  bool g_TodFollowKnown = false;

  // CAMERA > Time-lapse's resets (2026-11-14, Camera/Timelapse.cpp): a request may be forced (no "already there"
  // test: every frame gets the same reset at its own time, however close to the light shown; its clock waited for
  // until within TodForcedArrivedSeconds of the time asked, not the 120 s a jump takes, since a frame's step can be
  // shorter than that) and quiet (no lines but warnings: one a frame would fill the log). What became of one is
  // kept for TodResetOutcome: the request a reset ran for once its three jobs were done (at what time, in how
  // long), one served without a reset, one whose reset wasn't done in TodWatchMs. And while the time-lapse runs the
  // game's own steps are held (SetTodHoldSteps): the update's own add to its step count is taken off before it
  // (TodBeforeStep), so the light shown stays the reset's until the frame is taken.
  const float TodForcedArrivedSeconds = 5.f;
  std::atomic<long> g_TodForcedId{ 0 };
  std::atomic<long> g_TodQuietId{ 0 };
  std::atomic<long> g_TodDoneFor{ 0 };
  std::atomic<float> g_TodDoneAt{ -1.f };
  std::atomic<float> g_TodDoneClock{ -1.f };
  std::atomic<double> g_TodDoneSeconds{ 0.0 };
  std::atomic<long> g_TodNoResetFor{ 0 };
  std::atomic<long> g_TodFailedFor{ 0 };
  std::atomic<bool> g_TodHoldSteps{ false };
  std::atomic<bool> g_TodHoldLogged{ false };
  // The level's clock went or changed (ForgetTodManagers): the managers seen are forgotten at the next update, so a
  // new level's manager (even at an old one's address) starts with nothing of the old one's (a reset running, ours
  // pending)
  std::atomic<bool> g_TodForget{ false };
  // The time-lapse ended: a reset of its still running gets the TodWatchMs watchdog again (TodManager::Forced)
  std::atomic<bool> g_TodUnforce{ false };
}

static bool IsTodCode(BYTE const* pBase)
{
  // push rbp; push rbx; push r14; lea rbp, [rsp - 0x47]; sub rsp, 0x90; cmp dword [rdx + 0x10], 1
  static const BYTE step[] = { 0x55, 0x53, 0x41, 0x56, 0x48, 0x8D, 0x6C, 0x24, 0xB9, 0x48, 0x81, 0xEC, 0x90, 0x00, 0x00, 0x00, 0x83,
    0x7A, 0x10, 0x01 };
  // its countdown: mov eax, [rcx + 0x60]; test eax, eax; jle; dec eax; mov [rcx + 0x60], eax; cmp dword [rcx + 0x60], 0
  static const BYTE countdown[] = { 0x8B, 0x41, 0x60, 0x85, 0xC0, 0x7E, 0x05, 0xFF, 0xC8, 0x89, 0x41, 0x60, 0x83, 0x79, 0x60, 0x00 };
  static const BYTE jobsOn[] = { 0x80, 0x7B, 0x68, 0x00 };   // cmp byte [rbx + 0x68], 0
  // the jobs, rbx + 0x78 + i * 0x1C, through 0x143950750
  static const BYTE jobs[] = { 0x48, 0x63, 0xC7, 0x48, 0x8D, 0x53, 0x64, 0x48, 0x6B, 0xC8, 0x1C, 0x48, 0x83, 0xC1, 0x78, 0x48, 0x01,
    0xD9, 0xE8, 0x2F, 0xFD, 0xFF, 0xFF };
  static const BYTE job1Idle[] = { 0x83, 0xBB, 0x98, 0x00, 0x00, 0x00, 0x04 };   // cmp dword [rbx + 0x98], 4
  // its events: [event + 8] 0x0B57304D, Reset, 0x37C667AC
  static const BYTE event[] = { 0x53, 0x48, 0x83, 0xEC, 0x30, 0x8B, 0x42, 0x08, 0x48, 0x89, 0xCB, 0x3D, 0x4D, 0x30, 0x57, 0x0B, 0x74,
    0x61, 0x3D, 0x30, 0xB3, 0xB4, 0x0D, 0x74, 0x1D };
  // Reset: only with + 0x5C clear; the data's + 0x1C, the clock through the property getter
  static const BYTE eventReset[] = { 0x80, 0x79, 0x5C, 0x00, 0x75, 0x44, 0x48, 0x8B, 0x41, 0x28, 0x48, 0x8D, 0x15, 0xF1, 0x5C, 0xE6,
    0xFE, 0x48, 0x83, 0xC1, 0x48, 0x0F, 0x29, 0x74, 0x24, 0x20, 0xF3, 0x0F, 0x10, 0x70, 0x1C, 0xE8, 0xD6, 0x74, 0x0F, 0xFF };
  static const BYTE init[] = { 0x48, 0x89, 0x5C, 0x24, 0x08, 0x48, 0x89, 0x7C, 0x24, 0x10, 0x55, 0x48, 0x89, 0xE5, 0x48, 0x83, 0xEC,
    0x60, 0x48, 0x8B, 0x41, 0x28 };
  static const BYTE initEnd[] = { 0xC6, 0x43, 0x5C, 0x01, 0x66, 0x89, 0x7B, 0x74 };   // + 0x5C = 1; + 0x74 / + 0x75 = 0
  static const BYTE property[] = { 0x48, 0x8B, 0x01, 0x48, 0x85, 0xC0, 0x74, 0x0E, 0xF6, 0x40, 0x1C, 0x40, 0x74, 0x08, 0x48, 0x8B,
    0x00, 0xC3 };
  auto at = [&](__int64 rva, BYTE const* code, size_t size) { return memcmp(pBase + rva, code, size) == 0; };
  auto goesTo = [&](__int64 rva, BYTE op, __int64 target)
  {
    int rel;
    memcpy(&rel, pBase + rva + 1, 4);
    return pBase[rva] == op && rva + 5 + rel == target;
  };
  IMAGE_NT_HEADERS const* pNt = (IMAGE_NT_HEADERS const*)(pBase + ((IMAGE_DOS_HEADER const*)pBase)->e_lfanew);
  return pNt->FileHeader.TimeDateStamp == 0x5762E3B7 && at(TodStepRva, step, sizeof(step)) &&
    at(0x3950944, countdown, sizeof(countdown)) && at(0x39509D8, jobsOn, sizeof(jobsOn)) && at(0x3950A0A, jobs, sizeof(jobs)) &&
    at(0x3950A2C, job1Idle, sizeof(job1Idle)) && at(TodEventRva, event, sizeof(event)) && at(0x394AFB6, eventReset, sizeof(eventReset)) &&
    at(TodInitRva, init, sizeof(init)) && at(0x394F17C, initEnd, sizeof(initEnd)) && at(TodPropertyRva, property, sizeof(property)) &&
    goesTo(TodStepCallRva, 0xE8, TodStepRva) && goesTo(TodEventInitJumpRva, 0xE9, TodInitRva);
}

// What following a fast clock relies on (see TodFollowStart), on top of
// IsTodCode: the step reads UpdateInterval from its data, adds dt x 30 to
// + 0x58 and compares that with + 0x6C x + 0x70; the 30 is 30
static bool IsTodFollowCode(BYTE const* pBase)
{
  // mov rax, [rbx + 0x28]; movss xmm9, [rax + 0x1c]
  static const BYTE interval[] = { 0x48, 0x8B, 0x43, 0x28, 0xF3, 0x44, 0x0F, 0x10, 0x48, 0x1C };
  // movss xmm0, [r14]; movss xmm10, [86400]; mulss xmm0, [30]; addss xmm0, [rbx + 0x58]; movss [rbx + 0x58], xmm0
  static const BYTE count[] = { 0xF3, 0x41, 0x0F, 0x10, 0x06, 0xF3, 0x44, 0x0F, 0x10, 0x15, 0xFA, 0x87, 0x2F, 0xFE, 0xF3, 0x0F,
    0x59, 0x05, 0xC6, 0x99, 0x11, 0xFE, 0xF3, 0x0F, 0x58, 0x43, 0x58, 0xF3, 0x0F, 0x11, 0x43, 0x58 };
  // movss xmm1, [rbx + 0x6c]; comiss xmm1, xmm0; jbe; subss xmm1, xmm10; subss xmm0, xmm1; mulss xmm0, [rbx + 0x70]; comiss xmm0, xmm7
  static const BYTE progress[] = { 0xF3, 0x0F, 0x10, 0x4B, 0x6C, 0x0F, 0x2F, 0xC8, 0x76, 0x05, 0xF3, 0x41, 0x0F, 0x5C, 0xCA, 0xF3,
    0x0F, 0x5C, 0xC1, 0xF3, 0x0F, 0x59, 0x43, 0x70, 0x0F, 0x2F, 0xC7 };
  return memcmp(pBase + 0x39509E4, interval, sizeof(interval)) == 0 && memcmp(pBase + 0x3950A78, count, sizeof(count)) == 0 &&
    memcmp(pBase + 0x3950A9E, progress, sizeof(progress)) == 0 && *(float const*)(pBase + TodStepRateRva) == TodStepRate;
}

static int TodJobState(__int64 manager, int job)
{
  return *(int*)(manager + TodJobsOffset + job * TodJobSize + TodJobStateOffset);
}

// Game seconds between two times of day, round the clock
static float TodClockDistance(float a, float b)
{
  float d = fmodf(fabsf(a - b), 86400.f);
  return d > 43200.f ? 86400.f - d : d;
}

static void TodFormat(float seconds, char* out, size_t size)
{
  int s = (int)seconds;
  sprintf_s(out, size, "%02d:%02d", (s / 3600) % 24, (s / 60) % 60);
}

// What the light shown relies on (see TodShown): a job's flag is its
// request's buffer flag and its request id is kept in it; the request
// processor moves the buffer written on that flag; the pair shown in mode 1
static bool IsTodShownCode(BYTE const* pBase)
{
  // movzx edx, byte [rbx + 0x18]; movzx eax, word [rsp + 0x30]; mov [rsp + 0x32], dl
  static const BYTE flag[] = { 0x0F, 0xB6, 0x53, 0x18, 0x0F, 0xB7, 0x44, 0x24, 0x30, 0x88, 0x54, 0x24, 0x32 };
  // mov [rbx], eax (the request id); mov eax, [rbx + 8]; mov [rsi], eax; mov dword [rbx + 4], 1
  static const BYTE id[] = { 0x89, 0x03, 0x8B, 0x43, 0x08, 0x89, 0x06, 0xC7, 0x43, 0x04, 0x01, 0x00, 0x00, 0x00 };
  // cmp byte [rsi + 2], r14b; je; mov ecx, [r15 + 0x10858]; inc ecx
  static const BYTE turn[] = { 0x44, 0x38, 0x76, 0x02, 0x74, 0x32, 0x41, 0x8B, 0x8F, 0x58, 0x08, 0x01, 0x00, 0xFF, 0xC1 };
  // cmp dword [rcx + 0x10780], 1; ...; jne; mov ecx, [rcx + 0x10858]; lea r8d, [rcx + 1]; add ecx, 2
  static const BYTE shown[] = { 0x83, 0xB9, 0x80, 0x07, 0x01, 0x00, 0x01, 0x48, 0x89, 0xCE, 0xB8, 0x0F, 0xA2, 0x90, 0xC3, 0x8D, 0x80,
    0x9C, 0x08, 0x1A, 0xE7, 0x75, 0x37, 0x8B, 0x89, 0x58, 0x08, 0x01, 0x00, 0x44, 0x8D, 0x41, 0x01, 0x83, 0xC1, 0x02 };
  // the same pair handed to the views (0x14379B690): cmp dword [rbx + 0x10780], 1; ...; jne; mov r9d, [rbx + 0x10858];
  // lea r8d, [r9 + 1]
  static const BYTE viewsMode[] = { 0x83, 0xBB, 0x80, 0x07, 0x01, 0x00, 0x01 };
  static const BYTE views[] = { 0x75, 0x43, 0x44, 0x8B, 0x8B, 0x58, 0x08, 0x01, 0x00, 0x45, 0x8D, 0x41, 0x01 };
  return memcmp(pBase + 0x39507C5, flag, sizeof(flag)) == 0 && memcmp(pBase + 0x39507E1, id, sizeof(id)) == 0 &&
    memcmp(pBase + 0x379A8B1, turn, sizeof(turn)) == 0 && memcmp(pBase + 0x378D10A, shown, sizeof(shown)) == 0 &&
    memcmp(pBase + 0x379B78A, viewsMode, sizeof(viewsMode)) == 0 && memcmp(pBase + 0x379B7AB, views, sizeof(views)) == 0;
}

// Its passes into a new output buffer, noted as they're filed (see TodShown):
// after each of its updates, a job whose request id changed since the last
// look (not to none) with its flag set. The ids count up, so two filed
// between looks go in that order.
static void TodNotePasses(TodManager& t, __int64 manager)
{
  unsigned int ids[3];
  float times[3];
  int filed = 0;
  for (int job = 0; job < 3; ++job)
  {
    BYTE const* pJob = (BYTE const*)(manager + TodJobsOffset + job * TodJobSize);
    unsigned int id = *(unsigned int const*)(pJob + TodJobIdOffset);
    bool fresh = t.JobsSeen && id != t.JobIds[job] && id != TodNoRequest && pJob[TodJobFlagOffset];
    t.JobIds[job] = id;
    if (!fresh)
      continue;
    int at = filed++;
    while (at > 0 && ids[at - 1] > id)
    {
      ids[at] = ids[at - 1];
      times[at] = times[at - 1];
      --at;
    }
    ids[at] = id;
    times[at] = *(float const*)(pJob + TodJobTimeOffset);
  }
  t.JobsSeen = true;
  for (int i = 0; i < filed; ++i)
  {
    t.Lit[0] = t.Lit[1];
    t.Lit[1] = t.Lit[2];
    t.Lit[2] = times[i];
    if (t.LitCount < 3)
      ++t.LitCount;
  }
}

// The light shown, as the times its two buffers were worked out for (see
// above); false when that isn't known (fewer than three passes noted since it
// was first seen, another mode, or code not the tools'): then only its first
// job's time is there to go by
static bool TodShown(TodManager const& t, float& from, float& to)
{
  if (!g_TodShownKnown || t.LitCount < 3)
    return false;
  __int64 enlighten = *(__int64 const*)((__int64)g_gameHandle + TodEnlightenRva);
  if (!enlighten || *(int const*)(enlighten + TodEnlightenModeOffset) != 1)
    return false;
  from = t.Lit[0];
  to = t.Lit[1];
  return true;
}

// The Reset for one manager (see above) if it may have it now; false: again
// after its next update. forced / quiet: a time-lapse frame's (see
// TodForcedArrivedSeconds); reset: whether one was done (true returned
// without one: not needed, or it can't have one)
static bool TryTodReset(TodManager& t, __int64 manager, ULONGLONG now, float target, bool forced, bool quiet, bool& reset)
{
  reset = false;
  BYTE const* p = (BYTE const*)manager;
  __int64 data = *(__int64*)(manager + TodDataOffset);
  __int64 clockLink = *(__int64*)(manager + TodClockOffset);
  // Its update does nothing without the clock; its countdown ends in a reset of its own; no jobs, no passes to redo
  if (!data || !clockLink || !(*(BYTE const*)(clockLink + 0x1C) & 0x40) || *(int const*)(manager + TodCountdownOffset) != -1 ||
      !p[TodJobsOnOffset])
    return true;
  float clock = *((tTodProperty)((__int64)g_gameHandle + TodPropertyRva))(manager + TodClockOffset,
    (__int64)g_gameHandle + TodFloatTypeRva);
  float interval = *(float const*)(data + TodUpdateIntervalOffset);
  float lit = *(float const*)(manager + TodJobsOffset + TodJobTimeOffset);
  // Its clock at the time asked for first (see TodClockWaitMs), or gone on by
  // more than TodArrivedSeconds from where it was when this request was first
  // looked at: a clock that keeps moving (a time-lapse, a track) never gets
  // back to the time asked for, so its reset waited until a newer request
  // took its place (2026-11-06 15:24:44: a track's jump to 00:02 at its start
  // got none). (Not against the light's time: that trails the clock by more
  // than a step between steps, so a request would have gone through before
  // its clock had the new time, the 2026-10-25 04:35:28 case.)
  if (!t.WaitClockSet)
  {
    t.WaitClock = clock;
    t.WaitClockSet = true;
  }
  // (a forced one only at the time asked: a time-lapse's frames can be closer together than TodArrivedSeconds, so
  // a looser test could light the frame before's time)
  bool arrived = !(target >= 0.f) || (forced ? TodClockDistance(clock, target) <= TodForcedArrivedSeconds :
    (TodClockDistance(clock, target) <= TodArrivedSeconds || TodClockDistance(clock, t.WaitClock) > TodArrivedSeconds));
  if (!arrived && now - t.WaitStart < TodClockWaitMs)
    return false;
  char wanted[16], seen[16];
  TodFormat(target, wanted, sizeof(wanted));
  TodFormat(clock, seen, sizeof(seen));
  if (!arrived && t.WarnedFor != t.Waiting)
  {
    t.WarnedFor = t.Waiting;
    util::log::Warning("Time of day lighting: its clock didn't get to %s in %.0f s (it's at %s), %s", wanted,
      TodClockWaitMs / 1000.0, seen, forced ? "lit at the time asked anyway" : "its own time taken");
  }
  for (int job = 0; job < 3; ++job)
  {
    int state = TodJobState(manager, job);
    if (state >= 1 && state <= 3)
      return false;   // a pass of its own still with Enlighten
  }
  bool pending = p[TodResetPendingOffset] != 0;
  if (pending && !t.Running && !t.OursPending)
    return false;   // the game's own reset still in its first jobs
  int passes = *(int const*)(data + TodSolveIterationsOffset);
  // The light shown (see TodShown), else its first job's time as before; its
  // own steps light 2 of the game's own intervals ahead (not the following's)
  float shownFrom = lit, shownTo = lit;
  bool shownKnown = TodShown(t, shownFrom, shownTo);
  float own = (g_TodFollow.Driving && g_TodFollow.Manager == manager) ? g_TodFollow.Original : interval;
  float tolerance = shownKnown ? 2.f * own + 120.f : (std::max)(interval, 120.f);
  char from[40], to[16];
  TodFormat(shownFrom, from, sizeof(from));
  if (shownKnown)
  {
    char later[16];
    TodFormat(shownTo, later, sizeof(later));
    strcat_s(from, " - ");
    strcat_s(from, later);
  }
  TodFormat(clock, to, sizeof(to));
  if (!forced && TodClockDistance(clock, shownFrom) <= tolerance && TodClockDistance(clock, shownTo) <= tolerance)
  {
    if (shownKnown)
      util::log::Write("Time of day lighting: the light shown is for %s (the clock %s), no reset", from, to);
    else
      util::log::Write("Time of day lighting: already at %s (the clock %s; the light shown isn't known yet), no reset", from, to);
    return true;
  }
  if (pending)
    ((tTodInit)((__int64)g_gameHandle + TodInitRva))(manager, clock, interval);
  else
  {
    __declspec(align(16)) BYTE event[0x20] = {};
    *(unsigned int*)(event + 8) = TodResetEvent;
    ((tTodEvent)((__int64)g_gameHandle + TodEventRva))(manager, (__int64)event);
  }
  if (!p[TodResetPendingOffset] || TodJobState(manager, 0) != 0)
  {
    util::log::Warning("Time of day lighting: the reset at %s didn't take", to);
    return true;
  }
  // Its later two jobs at the reset's own time (see TodEarlyPasses): set up,
  // not filed yet (state 0; filed after job 0's passes). A time-lapse frame's
  // all three at the frame's own time, job 0 too (it's at state 0 as well, its
  // time put in its requests when it's filed): the light is that time's even
  // when its clock is still a second or two behind the tools' (frames can be
  // closer together than TodForcedArrivedSeconds)
  float resetTime = *(float const*)(manager + TodJobsOffset + TodJobTimeOffset);
  if (forced && target >= 0.f && target < 86400.f)
    resetTime = target;
  for (int job = forced ? 0 : 1; job < 3; ++job)
    *(float*)(manager + TodJobsOffset + job * TodJobSize + TodJobTimeOffset) = resetTime;
  t.Running = true;
  t.FirstDone = false;
  t.EarlyDone = false;
  t.Started = now;
  t.Time = forced ? resetTime : clock;
  t.ClockAtReset = clock;
  t.Passes = passes;
  t.Quiet = quiet;
  t.OursPending = true;
  t.Forced = forced;
  reset = true;
  if (!quiet)
  {
    char lit[16];
    TodFormat(t.Time, lit, sizeof(lit));
    util::log::Write("Time of day lighting: the game's own reset at %s (the light was for %s)%s%s, %.1f s after it was asked for: "
      "%d passes there, the bounce light started again, then two more there (not %.0f and %.0f game s ahead, so the sky the "
      "reflections go by stays put)", lit, from, pending ? ", again" : "", forced ? ", a time-lapse frame's, lit at "
      "the frame's own time" : "", (now - t.WaitStart) / 1000.0, passes, interval, 2 * interval);
  }
  return true;
}

static void TodAfterStep(__int64 manager)
{
  ULONGLONG now = GetTickCount64();
  g_TodStepTick = now;
  long wanted = g_TodWanted.load();
  if (g_TodForget.exchange(false))
    for (TodManager& m : g_TodManagers)
      m = TodManager{};
  if (g_TodUnforce.exchange(false))
    for (TodManager& m : g_TodManagers)
      m.Forced = false;
  TodManager* pT = nullptr;
  TodManager* pOldest = &g_TodManagers[0];
  for (TodManager& t : g_TodManagers)
  {
    if (t.Manager == manager)
    {
      pT = &t;
      break;
    }
    if (t.Seen < pOldest->Seen)
      pOldest = &t;
  }
  if (!pT)
  {
    // (from its first update, before anything is asked: its passes are noted)
    pT = pOldest;
    *pT = TodManager{};
    pT->Manager = manager;
    // One that turns up (a level loaded) takes only a request just made, or a
    // running time-lapse's frame's (it waits for its outcome)
    bool fresh = now - g_TodWantedTick.load() < TodFreshRequestMs || (g_TodHoldSteps.load() && g_TodForcedId.load() == wanted);
    pT->Done = wanted && fresh ? wanted - 1 : wanted;
  }
  TodManager& t = *pT;
  t.Seen = now;
  // (see TodManager::OursPending)
  if (!((BYTE const*)manager)[TodResetPendingOffset])
    t.OursPending = false;
  TodNotePasses(t, manager);
  if (t.Running)
  {
    int s0 = TodJobState(manager, 0), s1 = TodJobState(manager, 1), s2 = TodJobState(manager, 2);
    char at[16];
    TodFormat(t.Time, at, sizeof(at));
    // Reflections captured again after TodEarlyPasses of its first job's
    // passes (when it has more than that), and once all are done
    int early = (std::min)(TodEarlyPasses, t.Passes);
    if (g_TodDoneKnown && !t.EarlyDone && !t.FirstDone && early < t.Passes && s0 != TodJobIdle &&
        *(int const*)(manager + TodJobsOffset + TodJobDoneOffset) >= early)
    {
      t.EarlyDone = true;
      g_TodSettledOf = t.Passes;
      g_TodSettled = early;
      if (!t.Quiet)
        util::log::Write("Time of day lighting: %d of its %d passes at %s done in %.1f s", early, t.Passes, at,
          (now - t.Started) / 1000.0);
    }
    if (!t.FirstDone && s0 == TodJobIdle)
    {
      t.FirstDone = true;
      g_TodSettledOf = t.Passes;
      g_TodSettled = t.Passes > 0 ? t.Passes : 1;
      if (!t.Quiet)
        util::log::Write("Time of day lighting: its %d passes at %s done in %.1f s", t.Passes, at, (now - t.Started) / 1000.0);
    }
    if (s0 == TodJobIdle && s1 == TodJobIdle && s2 == TodJobIdle)
    {
      t.Running = false;
      // (the values first: TodResetOutcome reads the request, then them)
      g_TodDoneAt = t.Time;
      g_TodDoneClock = t.ClockAtReset;
      g_TodDoneSeconds = (now - t.Started) / 1000.0;
      g_TodDoneFor = t.ResetFor;
      if (!t.Quiet)
        util::log::Write("Time of day lighting: the reset at %s done in %.1f s (its three steps)", at, (now - t.Started) / 1000.0);
    }
    else if (!t.Forced && now - t.Started > TodWatchMs)
    {
      t.Running = false;
      g_TodFailedFor = t.ResetFor;
      util::log::Warning("Time of day lighting: the reset at %s isn't done after %.0f s (Enlighten paused?), not watched any more", at,
        TodWatchMs / 1000.0);
    }
  }
  if (t.Done != wanted)
  {
    if (t.Waiting != wanted)
    {
      t.Waiting = wanted;
      t.WaitStart = now;
      t.WaitClockSet = false;
    }
    // A time-lapse frame's (see TodForcedArrivedSeconds): forced, and quiet after its first frames
    bool forced = g_TodForcedId.load() == wanted;
    bool quiet = g_TodQuietId.load() == wanted;
    bool reset = false;
    if (TryTodReset(t, manager, now, g_TodTarget.load(), forced, quiet, reset))
    {
      t.Done = wanted;
      if (reset)
        t.ResetFor = wanted;
      else if (forced)
        g_TodNoResetFor = wanted;
    }
  }
  bool busy = false;
  for (TodManager const& m : g_TodManagers)
    if (m.Manager && now - m.Seen < TodManagerGoneMs && (m.Running || m.Done != wanted))
      busy = true;
  g_TodBusy = busy;
}

// Real seconds, for the clock's pace (see TodFollowStart)
static double TodNow()
{
  static LARGE_INTEGER frequency = {};
  if (!frequency.QuadPart)
    QueryPerformanceFrequency(&frequency);
  LARGE_INTEGER now;
  QueryPerformanceCounter(&now);
  return (double)now.QuadPart / (double)frequency.QuadPart;
}

// Following the clock ends: its own UpdateInterval back on its data (the
// manager's update, so its data is still there)
static void TodFollowEnd(TodFollow& f, char const* why)
{
  if (!f.Driving)
    return;
  f.Driving = false;
  if (f.Data)
    *(float*)(f.Data + TodUpdateIntervalOffset) = f.Original;
  double took = TodNow() - f.Started;
  util::log::Write("Time of day lighting: followed the clock for %.1f s (%d steps, one every %.2f s); its own steps again, %.0f "
    "game s apart (%s)", took, f.Steps, f.Steps > 0 ? took / f.Steps : 0.0, f.Original, why);
  f.Data = 0;
}

// Before the manager's update: following a clock that runs fast (see
// TodFollowStart). info: the update's, + 0 its dt (frames / 30)
static void TodBeforeStep(__int64 manager, __int64 info)
{
  if (!g_TodFollowKnown)
    return;
  TodFollow& f = g_TodFollow;
  // Another manager (a level loaded): the last one and its data are gone,
  // nothing to put back on them
  if (f.Manager != manager)
  {
    if (f.Driving)
      util::log::Write("Time of day lighting: stopped following the clock (its lighting was replaced)");
    f = TodFollow{ manager, 0, 0.f, 0.f, 0.f, -1.0, 0.f, false, 0.0, 0.0, 0.5, 0 };
  }
  BYTE const* p = (BYTE const*)manager;
  __int64 data = *(__int64*)(manager + TodDataOffset);
  __int64 clockLink = *(__int64*)(manager + TodClockOffset);
  // As TryTodReset: its update steps nothing without these
  bool stepping = data && clockLink && (*(BYTE const*)(clockLink + 0x1C) & 0x40) &&
    *(int const*)(manager + TodCountdownOffset) == -1 && p[TodJobsOnOffset];
  bool usable = g_TodFollowWanted.load() && stepping;
  // A time-lapse running (SetTodHoldSteps): no step of its own (the update's own add of dt x 30 to its count taken
  // off before it, never below 0), so the light shown stays the last reset's; its jobs (the reset's) go on as ever
  if (stepping && g_TodHoldSteps.load())
  {
    float* pCount = (float*)(manager + TodAccOffset);
    float held = *pCount - *(float const*)info * TodStepRate;
    *pCount = held > 0.f ? held : 0.f;
    if (!g_TodHoldLogged)
    {
      g_TodHoldLogged = true;
      util::log::Write("Time of day lighting: its own steps held for the time-lapse (the light stays each frame's reset's)");
    }
  }
  if (usable && f.Driving && data != f.Data)
  {
    // (its data was swapped: the one set went with it)
    f.Data = 0;
    TodFollowEnd(f, "its data was replaced");
  }
  if (!usable)
  {
    // (its own interval back only on the data it still has)
    if (data != f.Data)
      f.Data = 0;
    TodFollowEnd(f, "the catch-up is off, or its lighting isn't stepping");
    f.LastTime = -1.0;
    f.Speed = 0.f;
    return;
  }
  float clock = *((tTodProperty)((__int64)g_gameHandle + TodPropertyRva))(manager + TodClockOffset,
    (__int64)g_gameHandle + TodFloatTypeRva);
  float lit = *(float const*)(manager + TodJobsOffset + TodJobTimeOffset);
  double now = TodNow();
  if (f.LastTime < 0.0)
  {
    f.LastTime = now;
    f.LastClock = clock;
    f.LastLit = lit;
    return;
  }
  double dt = now - f.LastTime;
  float delta = clock - f.LastClock;
  if (delta > 43200.f)
    delta -= 86400.f;
  else if (delta < -43200.f)
    delta += 86400.f;
  f.LastTime = now;
  f.LastClock = clock;
  // A step of its own since the last update (job 0 set to the clock): how often they come
  if (f.Driving && lit != f.LastLit)
  {
    ++f.Steps;
    if (f.LastStep > 0.0)
      f.StepPeriod = 0.5 * f.StepPeriod + 0.5 * (now - f.LastStep);
    f.LastStep = now;
  }
  f.LastLit = lit;
  bool cut = fabsf(delta) > TodFollowCut;
  if (!(dt > 0.0) || dt > TodFollowPause)
    f.Speed = 0.f;
  else
    f.Speed = 0.7f * f.Speed + 0.3f * (cut ? 0.f : (float)(delta / dt));
  if (!f.Driving && f.Speed > TodFollowStart)
  {
    float own = *(float const*)(data + TodUpdateIntervalOffset);
    if (!(own > 0.f) || own >= 86400.f)
      return;   // (not an interval it could step by: left alone)
    f.Driving = true;
    f.Data = data;
    f.Original = own;
    f.Started = now;
    f.LastStep = 0.0;
    f.StepPeriod = 0.5;
    f.Steps = 0;
    util::log::Write("Time of day lighting: the clock runs %.0f game minutes a second: the light follows it, each step as far "
      "ahead as the clock goes in a step's time (instead of one every %.0f s of game time, %.0f game s ahead)", f.Speed / 60.f,
      own / TodStepRate, own);
  }
  else if (f.Driving && f.Speed < TodFollowStop)
    TodFollowEnd(f, cut ? "a cut" : "the clock slowed down");
  if (!f.Driving)
    return;
  double period = f.StepPeriod < TodFollowShortest ? TodFollowShortest : (f.StepPeriod > TodFollowSlowest ? TodFollowSlowest :
    f.StepPeriod);
  float interval = (std::min)((std::max)(f.Speed * (float)period, f.Original), TodFollowLongest);
  *(float*)(data + TodUpdateIntervalOffset) = interval;
  // Its count grows by how far its clock went (the update adds dt x 30 itself)
  if (!cut && delta > 0.f)
    *(float*)(manager + TodAccOffset) += delta - *(float const*)info * TodStepRate;
}

static void __fastcall hTodStep(__int64 manager, __int64 info)
{
  __try
  {
    TodBeforeStep(manager, info);
  }
  __except (EXCEPTION_EXECUTE_HANDLER)
  {
  }
  oTodStep(manager, info);
  __try
  {
    TodAfterStep(manager);
  }
  __except (EXCEPTION_EXECUTE_HANDLER)
  {
  }
}

// The green screen's depth (VISUALS > Green screen, see greenplane): the
// game's depth clears, seen just before they're done (greenplane::OnDepthClear
// takes the scene's depth before a second clear in a frame). Made the first
// time the green screen is on: ID3D11DeviceContext's ClearDepthStencilView
// (vtable slot 53), the function the game's contexts call.
typedef void(STDMETHODCALLTYPE* tClearDepthStencilView)(ID3D11DeviceContext*, ID3D11DepthStencilView*, UINT, FLOAT, UINT8);
tClearDepthStencilView oClearDepthStencilView = nullptr;
const int ClearDepthStencilViewSlot = 53;

void STDMETHODCALLTYPE hClearDepthStencilView(ID3D11DeviceContext* pContext, ID3D11DepthStencilView* pView, UINT flags,
  FLOAT depth, UINT8 stencil)
{
  if (!g_shutdown)
  {
    perf::Scope timing(perf::SectionDepthClear);
    greenplane::OnDepthClear(pContext, pView, flags);
  }
  oClearDepthStencilView(pContext, pView, flags, depth, stencil);
}

bool util::hooks::HookDepthClears(ID3D11DeviceContext* pContext)
{
  if (oClearDepthStencilView)
    return true;
  if (!pContext)
    return false;
  void** pTable = *(void***)pContext;
  CreateHook("DepthClear", (__int64)pTable[ClearDepthStencilViewSlot], hClearDepthStencilView, &oClearDepthStencilView);
  return oClearDepthStencilView != nullptr;
}

static void HookTodLighting()
{
  if (!IsTodCode((BYTE*)g_gameHandle))
  {
    util::log::Warning("The time of day lighting's code isn't what the tools know, after a time jump its light catches up a step at "
      "a time");
    return;
  }
  // (checked before the hook, so it's set before the hook's first call)
  g_TodFollowKnown = IsTodFollowCode((BYTE*)g_gameHandle);
  if (!g_TodFollowKnown)
    util::log::Warning("The time of day lighting's step code isn't what the tools know: a time-lapse's light catches up only "
      "at its start and end");
  // A job's passes done at + 0x10: inc dword [rbx + 0x10]; mov eax, [rbx + 0x10]; cmp eax, [rbx + 0xc] (0x143950827)
  static const BYTE done[] = { 0xFF, 0x43, 0x10, 0x8B, 0x43, 0x10, 0x3B, 0x43, 0x0C };
  g_TodDoneKnown = memcmp((BYTE*)g_gameHandle + 0x3950827, done, sizeof(done)) == 0;
  if (!g_TodDoneKnown)
    util::log::Warning("The time of day lighting's pass count isn't the code the tools know: reflections are captured again only "
      "once all of a reset's passes are done");
  g_TodShownKnown = IsTodShownCode((BYTE*)g_gameHandle);
  if (!g_TodShownKnown)
    util::log::Warning("Enlighten's output buffers aren't the code the tools know: a reset after a time jump is skipped by its "
      "last step's time, not by the light shown");
  CreateHook("TodLightingStep", (__int64)g_gameHandle + TodStepRva, hTodStep, &oTodStep);
  g_TodHooked = IsHookCreated("TodLightingStep");
}

void util::hooks::SetTodFollow(bool wanted)
{
  g_TodFollowWanted = wanted;
}

bool util::hooks::TodResetAvailable()
{
  return g_TodHooked;
}

long util::hooks::RequestTodReset(float clock, bool forced, bool quiet)
{
  if (!g_TodHooked)
    return 0;
  g_TodTarget = clock;
  g_TodWantedTick = GetTickCount64();
  // (its flags before its id is made the one wanted: the hook reads the id, then them)
  long id = g_TodWanted.load() + 1;
  g_TodForcedId = forced ? id : 0;
  g_TodQuietId = quiet ? id : 0;
  g_TodWanted = id;
  g_TodBusy = true;
  return id;
}

int util::hooks::TodResetOutcome(long id, float& litAt, double& seconds, float* clockAt)
{
  litAt = -1.f;
  seconds = 0.0;
  if (clockAt)
    *clockAt = -1.f;
  if (!id)
    return TodOutcome_Failed;
  if (g_TodDoneFor.load() == id)
  {
    litAt = g_TodDoneAt.load();
    seconds = g_TodDoneSeconds.load();
    if (clockAt)
      *clockAt = g_TodDoneClock.load();
    return TodOutcome_Done;
  }
  if (g_TodNoResetFor.load() == id)
    return TodOutcome_NoReset;
  if (g_TodFailedFor.load() == id)
    return TodOutcome_Failed;
  return TodOutcome_Working;
}

void util::hooks::SetTodHoldSteps(bool hold)
{
  bool was = g_TodHoldSteps.exchange(hold);
  if (hold && !was)
    g_TodHoldLogged = false;
  // The time-lapse ended: a frame's request still waiting isn't forced or quiet any more (a level loaded later would
  // otherwise be lit at its time)
  if (!hold && was)
  {
    g_TodForcedId = 0;
    g_TodQuietId = 0;
    g_TodUnforce = true;
  }
}

void util::hooks::ForgetTodManagers()
{
  g_TodForget = true;
}

bool util::hooks::TodHoldAvailable()
{
  return g_TodHooked && g_TodFollowKnown;
}

long long util::hooks::TodSinceUpdateMs()
{
  ULONGLONG last = g_TodStepTick.load();
  return last ? (long long)(GetTickCount64() - last) : -1;
}

bool util::hooks::TodResetBusy()
{
  return g_TodHooked && g_TodBusy && GetTickCount64() - g_TodStepTick.load() < TodManagerGoneMs;
}

bool util::hooks::TakeTodResetSettled(int& passes, int& of)
{
  passes = g_TodSettled.exchange(0);
  of = g_TodSettledOf.load();
  return passes > 0;
}

// A jump at pSite (rel32, 5 bytes) to pTarget, the rest of the site's
// length NOPs
static bool WriteJump(BYTE* pSite, int length, BYTE const* pTarget)
{
  int rel;
  if (!Rel32(pSite + 5, pTarget, rel))
    return false;
  pSite[0] = 0xE9;
  memcpy(pSite + 1, &rel, 4);
  for (int i = 5; i < length; ++i)
    pSite[i] = 0x90;
  return true;
}

void util::hooks::EnlargeMeshTable()
{
  BYTE* pBase = (BYTE*)g_gameHandle;
  BYTE* pCtor = pBase + MeshManagerCtorRva;
  BYTE* pClear = pBase + 0x3550A21;
  BYTE* pSet1 = pBase + 0x3550B09;
  BYTE* pTest1 = pBase + 0x3550C93;
  BYTE* pTest2 = pBase + 0x3550ED0;
  BYTE* pSet2 = pBase + 0x3550FA0;
  BYTE* pCap1 = pBase + 0x35554CF;
  BYTE* pCap2 = pBase + 0x35555E4;
  BYTE* pCap3 = pBase + 0x35556F3;

  // The code as it was read: the constructor's start, the pass's clear of the
  // bitset (lea rcx, [rdi + 0x386250] and its size 0xC346050F + 0x3CB9FCF1),
  // its two sets and two tests (each from its check, cmp ..., 0x1000), and
  // the three caps (mov, then lea ... + disp to 0xFFF)
  static const BYTE kCtor[] = { 0x55, 0x57, 0x41, 0x56, 0x48, 0x8D, 0xAC, 0x24, 0xF0, 0x3B, 0xFF, 0xFF };
  static const BYTE kClear[] = { 0x48, 0x8D, 0x8F, 0x50, 0x62, 0x38, 0x00, 0x31, 0xD2, 0x41, 0xB8, 0x0F, 0x05, 0x46, 0xC3,
    0x45, 0x8D, 0x80, 0xF1, 0xFC, 0xB9, 0x3C };
  static const BYTE kSet1[] = { 0x48, 0x3D, 0x00, 0x10, 0x00, 0x00, 0x73, 0x1C, 0x41, 0x89, 0xC0, 0x49, 0xC1, 0xE8, 0x06,
    0x24, 0x3F, 0xBA, 0x01, 0x00, 0x00, 0x00, 0x0F, 0xB6, 0xC8, 0x48, 0xD3, 0xE2, 0x4A, 0x09, 0x94, 0xC7, 0x50, 0x62,
    0x38, 0x00 };
  static const BYTE kTest1[] = { 0x49, 0x81, 0xFA, 0x00, 0x10, 0x00, 0x00, 0x73, 0x1E, 0x44, 0x89, 0xD0, 0x48, 0xC1, 0xE8,
    0x06, 0x48, 0x8B, 0x94, 0xC7, 0x50, 0x62, 0x38, 0x00 };
  static const BYTE kTest2[] = { 0x49, 0x81, 0xFB, 0x00, 0x10, 0x00, 0x00, 0x73, 0x22, 0x44, 0x89, 0xD8, 0x48, 0xC1, 0xE8,
    0x06, 0x48, 0x8B, 0x94, 0xC7, 0x50, 0x62, 0x38, 0x00 };
  static const BYTE kSet2[] = { 0x49, 0x81, 0xFB, 0x00, 0x10, 0x00, 0x00, 0x73, 0x1E, 0x4D, 0x89, 0xD8, 0x49, 0xC1, 0xE8,
    0x06, 0x41, 0x0F, 0xB6, 0xCB, 0x80, 0xE1, 0x3F, 0xBA, 0x01, 0x00, 0x00, 0x00, 0x48, 0xD3, 0xE2, 0x4A, 0x09, 0x94,
    0xC7, 0x50, 0x62, 0x38, 0x00 };
  static const BYTE kCap1[] = { 0xB9, 0x0F, 0xA2, 0xC3, 0x90, 0x8D, 0x89, 0xF0, 0x6D, 0x3C, 0x6F };
  static const BYTE kCap2[] = { 0xB9, 0x0F, 0xA2, 0xFF, 0xE6, 0x8D, 0x89, 0xF0, 0x6D, 0x00, 0x19 };
  static const BYTE kCap3[] = { 0x41, 0xB9, 0x0F, 0xA2, 0x47, 0xC3, 0x45, 0x8D, 0x89, 0xF0, 0x6D, 0xB8, 0x3C };

  if (*(__int64*)(pBase + MeshManagerGlobalRva))
  {
    util::log::Warning("The mesh manager was already made, too late to give it room for more than 4096 meshes");
    return;
  }
  if (memcmp(pCtor, kCtor, sizeof(kCtor)) || memcmp(pClear, kClear, sizeof(kClear)) || memcmp(pSet1, kSet1, sizeof(kSet1)) ||
    memcmp(pTest1, kTest1, sizeof(kTest1)) || memcmp(pTest2, kTest2, sizeof(kTest2)) || memcmp(pSet2, kSet2, sizeof(kSet2)) ||
    memcmp(pCap1, kCap1, sizeof(kCap1)) || memcmp(pCap2, kCap2, sizeof(kCap2)) || memcmp(pCap3, kCap3, sizeof(kCap3)))
  {
    util::log::Warning("The mesh streaming code isn't what the tools know, it keeps room for 4096 meshes");
    return;
  }

  // The bitset, and next to the game: its pointer, then the four uses
  BYTE* pBitset = (BYTE*)VirtualAlloc(NULL, MeshBitsetBytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
  BYTE* pNear = AllocNearGame(0x1000);
  if (!pBitset || !pNear)
  {
    if (pBitset) VirtualFree(pBitset, 0, MEM_RELEASE);
    if (pNear) VirtualFree(pNear, 0, MEM_RELEASE);
    util::log::Warning("No memory for the mesh streaming's bigger bitset, the game keeps room for 4096 meshes");
    return;
  }
  BYTE* pSlot = pNear;
  memcpy(pSlot, &pBitset, sizeof(pBitset));
  BYTE* pCode = pNear + 0x10;
  BYTE* pCaveSet1 = pCode;
  BYTE* pCaveSet2 = pCode + 0x20;
  BYTE* pCaveTest1 = pCode + 0x40;
  BYTE* pCaveTest2 = pCode + 0x60;
  bool ok = true;
  int rel = 0;
  // or [rdi + r8*8 + 0x386250], rdx -> push rax; mov rax, [slot]; or [rax + r8*8], rdx; pop rax; jmp back
  for (int k = 0; k < 2; ++k)
  {
    BYTE* pCave = k == 0 ? pCaveSet1 : pCaveSet2;
    BYTE* pBack = k == 0 ? pSet1 + 0x24 : pSet2 + 0x27;
    static const BYTE body[] = { 0x50, 0x48, 0x8B, 0x05, 0, 0, 0, 0, 0x4A, 0x09, 0x14, 0xC0, 0x58, 0xE9, 0, 0, 0, 0 };
    memcpy(pCave, body, sizeof(body));
    ok &= Rel32(pCave + 8, pSlot, rel);
    memcpy(pCave + 4, &rel, 4);
    ok &= Rel32(pCave + 18, pBack, rel);
    memcpy(pCave + 14, &rel, 4);
  }
  // mov rdx, [rdi + rax*8 + 0x386250] -> mov rdx, [slot]; mov rdx, [rdx + rax*8]; jmp back
  for (int k = 0; k < 2; ++k)
  {
    BYTE* pCave = k == 0 ? pCaveTest1 : pCaveTest2;
    BYTE* pBack = k == 0 ? pTest1 + 0x18 : pTest2 + 0x18;
    static const BYTE body[] = { 0x48, 0x8B, 0x15, 0, 0, 0, 0, 0x48, 0x8B, 0x14, 0xC2, 0xE9, 0, 0, 0, 0 };
    memcpy(pCave, body, sizeof(body));
    ok &= Rel32(pCave + 7, pSlot, rel);
    memcpy(pCave + 3, &rel, 4);
    ok &= Rel32(pCave + 16, pBack, rel);
    memcpy(pCave + 12, &rel, 4);
  }
  int clearRel = 0;
  ok &= Rel32(pClear + 7, pSlot, clearRel);
  if (!ok)
  {
    VirtualFree(pBitset, 0, MEM_RELEASE);
    VirtualFree(pNear, 0, MEM_RELEASE);
    util::log::Warning("The mesh streaming's bitset couldn't be put within reach of the game, it keeps room for 4096 meshes");
    return;
  }
  DWORD nearProtect;
  VirtualProtect(pNear, 0x1000, PAGE_EXECUTE_READ, &nearProtect);

  // Both pages of the game's code writable first, so it's all or nothing
  BYTE* pPassPage = (BYTE*)((ULONG_PTR)pClear & ~(ULONG_PTR)0xFFF);
  BYTE* pCapPage = (BYTE*)((ULONG_PTR)pCap1 & ~(ULONG_PTR)0xFFF);
  if (pPassPage != (BYTE*)((ULONG_PTR)(pSet2 + sizeof(kSet2)) & ~(ULONG_PTR)0xFFF) ||
    pCapPage != (BYTE*)((ULONG_PTR)(pCap3 + sizeof(kCap3)) & ~(ULONG_PTR)0xFFF))
  {
    util::log::Warning("The mesh streaming code isn't laid out as the tools know, it keeps room for 4096 meshes");
    return;
  }
  DWORD passProtect = 0, capProtect = 0;
  if (!VirtualProtect(pPassPage, 0x1000, PAGE_EXECUTE_READWRITE, &passProtect))
  {
    util::log::Warning("The mesh streaming code couldn't be changed, it keeps room for 4096 meshes");
    return;
  }
  if (!VirtualProtect(pCapPage, 0x1000, PAGE_EXECUTE_READWRITE, &capProtect))
  {
    VirtualProtect(pPassPage, 0x1000, passProtect, &passProtect);
    util::log::Warning("The mesh streaming code couldn't be changed, it keeps room for 4096 meshes");
    return;
  }

  const unsigned int checkRoom = MeshesRoom;
  // The clear: mov rcx, [slot] instead of the lea, and the new size
  pClear[0] = 0x48; pClear[1] = 0x8B; pClear[2] = 0x0D;
  memcpy(pClear + 3, &clearRel, 4);
  const unsigned int clearSize = (unsigned int)MeshBitsetBytes - 0xC346050Fu;
  memcpy(pClear + 0x12, &clearSize, 4);
  // The checks, then the uses
  memcpy(pSet1 + 2, &checkRoom, 4);
  memcpy(pTest1 + 3, &checkRoom, 4);
  memcpy(pTest2 + 3, &checkRoom, 4);
  memcpy(pSet2 + 3, &checkRoom, 4);
  WriteJump(pSet1 + 0x1C, 8, pCaveSet1);
  WriteJump(pTest1 + 0x10, 8, pCaveTest1);
  WriteJump(pTest2 + 0x10, 8, pCaveTest2);
  WriteJump(pSet2 + 0x1F, 8, pCaveSet2);
  // The caps: mov value + disp = MeshesRoom - 1
  const unsigned int cap1 = (unsigned int)(MeshesRoom - 1) - 0x90C3A20Fu;
  const unsigned int cap2 = (unsigned int)(MeshesRoom - 1) - 0xE6FFA20Fu;
  const unsigned int cap3 = (unsigned int)(MeshesRoom - 1) - 0xC347A20Fu;
  memcpy(pCap1 + 7, &cap1, 4);
  memcpy(pCap2 + 7, &cap2, 4);
  memcpy(pCap3 + 9, &cap3, 4);

  VirtualProtect(pPassPage, 0x1000, passProtect, &passProtect);
  VirtualProtect(pCapPage, 0x1000, capProtect, &capProtect);
  FlushInstructionCache(GetCurrentProcess(), pPassPage, 0x1000);
  FlushInstructionCache(GetCurrentProcess(), pCapPage, 0x1000);
  g_MeshCodeChanged = true;
  util::log::Ok("The mesh streaming's bitset and caps have room for %d meshes (bitset at 0x%I64X, code at 0x%I64X)",
    MeshesRoom, (__int64)pBitset, (__int64)pNear);

  // Then the constructor, which moves the table once it has made it
  MH_STATUS status = MH_Initialize();
  if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED)
  {
    util::log::Warning("MinHook couldn't start (MH_STATUS 0x%X), the mesh table keeps room for 4096", status);
    return;
  }
  // Next to the game, where the placeholder was (Init frees it otherwise)
  if (m_pReservedHookSpace)
  {
    VirtualFree(m_pReservedHookSpace, 0, MEM_RELEASE);
    m_pReservedHookSpace = nullptr;
  }
  CreateHook("MeshManagerConstructor", (__int64)pCtor, hMeshManagerCtor, &oMeshManagerCtor);
}

// Called right after EnlargeMeshTable, before the game makes any zone
// streamer: gives every streamer ZoneSlotsRoom zone slots (see
// hZoneStreamerCtor). All or nothing: only if every place is the code that was
// read, and its three hooks stay on when the tools close (a level unloading
// then can release more than 128 zones at once).
void util::hooks::EnlargeZoneSlots()
{
  BYTE* pBase = (BYTE*)g_gameHandle;
  BYTE* pCtor = pBase + ZoneStreamerCtorRva;
  BYTE* pClear = pBase + 0x3C7DBEE;
  BYTE* pSearch = pBase + 0x3C80255;
  BYTE* pReader1 = pBase + 0x3C816F3;
  BYTE* pReader2 = pBase + 0x3C81C87;
  BYTE* pReleaseA = pBase + 0x3C81F39;
  BYTE* pReleaseB = pBase + 0x3C81FCB;
  BYTE* pRelease = pBase + ZoneReleaseRva;
  BYTE* pLoadPass = pBase + ZoneLoadPassRva;

  // The code as it was read: the constructor's start and its clear of the
  // slots (0x400 bytes); the load's search (mov r8d, 0x80, the compare, the
  // write); the two readers; the release's two lea rbx, [rsi + rax*8]; the
  // release's and the load pass's starts
  static const BYTE kSlotCtor[] = { 0x48, 0x89, 0x4C, 0x24, 0x08, 0x56, 0x57, 0x41, 0x56, 0x48, 0x83, 0xEC, 0x30 };
  static const BYTE kSlotClear[] = { 0x48, 0x8D, 0x8B, 0xF8, 0x02, 0x00, 0x00, 0x31, 0xD2, 0x41, 0xB8, 0x0F, 0xA2, 0xFF, 0xE7,
    0x45, 0x8D, 0x80, 0xF1, 0x61, 0x00, 0x18, 0xE8, 0x43, 0xAB, 0x00, 0x00 };
  static const BYTE kSearch[] = { 0x41, 0xB8, 0x80, 0x00, 0x00, 0x00, 0x8D, 0x92, 0xF0, 0xFA, 0xB2, 0x3C, 0x0F, 0xB7, 0xC1,
    0x48, 0x83, 0xBC, 0xC5, 0xF8, 0x02, 0x00, 0x00, 0x00, 0x74, 0x0B, 0x66, 0xFF, 0xC1, 0x66,
    0x44, 0x39, 0xC1, 0x72, 0xE9, 0xEB, 0x03, 0x0F, 0xB7, 0xD1, 0x0F, 0xB7, 0xC2, 0x48, 0x89,
    0x9C, 0xC5, 0xF8, 0x02, 0x00, 0x00, 0x66, 0xFF, 0xC2, 0x66, 0x89, 0x56, 0x02 };
  static const BYTE kReader1[] = { 0x48, 0x8B, 0x8B, 0xE0, 0x02, 0x00, 0x00, 0x4C, 0x8B, 0x01, 0x66, 0xFF, 0xC8, 0x0F, 0xB7,
    0xC0, 0x48, 0x8B, 0x94, 0xC3, 0xF8, 0x02, 0x00, 0x00, 0x41, 0xFF, 0x50, 0x30 };
  static const BYTE kReader2[] = { 0x48, 0x8B, 0x8D, 0xE0, 0x02, 0x00, 0x00, 0x4C, 0x8B, 0x01, 0x66, 0xFF, 0xC8, 0x0F, 0xB7,
    0xC0, 0x48, 0x8B, 0x94, 0xC5, 0xF8, 0x02, 0x00, 0x00, 0x41, 0xFF, 0x50, 0x30 };
  static const BYTE kRelease2[] = { 0x48, 0x8B, 0x8E, 0xE0, 0x02, 0x00, 0x00, 0x66, 0xFF, 0xC8, 0x0F, 0xB7, 0xC0, 0x48, 0x8D,
    0x1C, 0xC6, 0x48, 0x8B, 0x01, 0x48, 0x8B, 0xBB, 0xF8, 0x02, 0x00, 0x00, 0x48, 0x89, 0xFA,
    0xFF, 0x50, 0x20 };
  static const BYTE kRelease[] = { 0x53, 0x56, 0x41, 0x54, 0x48, 0x81, 0xEC, 0x50, 0x01, 0x00, 0x00, 0x48, 0x8B, 0x05, 0xCE,
    0xDD, 0x49, 0xFE };
  static const BYTE kLoadPass[] = { 0x48, 0x89, 0x5C, 0x24, 0x18, 0x48, 0x89, 0x74, 0x24, 0x20, 0x57, 0x48, 0x81, 0xEC, 0x40,
    0x01, 0x00, 0x00 };

  // No streamer yet (ZoneStreamerEntity's four instance lists empty)
  __int64 pClass = (__int64)pBase + ZoneStreamerClassRva;
  for (int list = 0; list < 4; ++list)
  {
    if (*(__int64*)(pClass + 0x60 + list * 0x10))
    {
      util::log::Warning("A zone streamer already exists, too late to give the streamers more than 128 zone slots");
      return;
    }
  }
  if (memcmp(pCtor, kSlotCtor, sizeof(kSlotCtor)) || memcmp(pClear, kSlotClear, sizeof(kSlotClear)) ||
    memcmp(pSearch, kSearch, sizeof(kSearch)) || memcmp(pReader1, kReader1, sizeof(kReader1)) ||
    memcmp(pReader2, kReader2, sizeof(kReader2)) || memcmp(pReleaseA, kRelease2, sizeof(kRelease2)) ||
    memcmp(pReleaseB, kRelease2, sizeof(kRelease2)) || memcmp(pRelease, kRelease, sizeof(kRelease)) ||
    memcmp(pLoadPass, kLoadPass, sizeof(kLoadPass)))
  {
    util::log::Warning("The zone streaming code isn't what the tools know, the streamers keep 128 zone slots");
    return;
  }

  // The stubs, next to the game
  BYTE* pNear = AllocNearGame(0x1000);
  if (!pNear)
  {
    util::log::Warning("No memory next to the game for the zone slots' code, the streamers keep 128 zone slots");
    return;
  }
  struct Stub { BYTE* pCave; BYTE const* pBack; std::vector<BYTE> Body; };
  // cmp qword ptr [rbp + rax*8 + 0x2f8], 0 -> mov r11, [rbp + 0x2f8]; cmp qword ptr [r11 + rax*8], 0
  // mov [rbp + rax*8 + 0x2f8], rbx -> mov r11, [rbp + 0x2f8]; mov [r11 + rax*8], rbx
  // mov rdx, [rbx / rbp + rax*8 + 0x2f8] -> mov rdx, [rbx / rbp + 0x2f8]; mov rdx, [rdx + rax*8]
  // lea rbx, [rsi + rax*8]; mov rax, [rcx] -> mov rbx, [rsi + 0x2f8]; lea rbx, [rbx + rax*8 - 0x2f8]; mov rax, [rcx]
  Stub stubs[] = {
    { pNear + 0x00, pSearch + 0x18, { 0x4C, 0x8B, 0x9D, 0xF8, 0x02, 0x00, 0x00, 0x49, 0x83, 0x3C, 0xC3, 0x00 } },
    { pNear + 0x20, pSearch + 0x33, { 0x4C, 0x8B, 0x9D, 0xF8, 0x02, 0x00, 0x00, 0x49, 0x89, 0x1C, 0xC3 } },
    { pNear + 0x40, pReader1 + 0x18, { 0x48, 0x8B, 0x93, 0xF8, 0x02, 0x00, 0x00, 0x48, 0x8B, 0x14, 0xC2 } },
    { pNear + 0x60, pReader2 + 0x18, { 0x48, 0x8B, 0x95, 0xF8, 0x02, 0x00, 0x00, 0x48, 0x8B, 0x14, 0xC2 } },
    { pNear + 0x80, pReleaseA + 0x14, { 0x48, 0x8B, 0x9E, 0xF8, 0x02, 0x00, 0x00, 0x48, 0x8D, 0x9C, 0xC3, 0x08, 0xFD, 0xFF, 0xFF,
      0x48, 0x8B, 0x01 } },
    { pNear + 0xA0, pReleaseB + 0x14, { 0x48, 0x8B, 0x9E, 0xF8, 0x02, 0x00, 0x00, 0x48, 0x8D, 0x9C, 0xC3, 0x08, 0xFD, 0xFF, 0xFF,
      0x48, 0x8B, 0x01 } },
  };
  bool ok = true;
  for (Stub& stub : stubs)
  {
    memcpy(stub.pCave, stub.Body.data(), stub.Body.size());
    BYTE* pJump = stub.pCave + stub.Body.size();
    int rel = 0;
    ok &= Rel32(pJump + 5, stub.pBack, rel);
    pJump[0] = 0xE9;
    memcpy(pJump + 1, &rel, 4);
  }
  if (!ok)
  {
    VirtualFree(pNear, 0, MEM_RELEASE);
    util::log::Warning("The zone slots' code couldn't be put within reach of the game, the streamers keep 128 zone slots");
    return;
  }
  DWORD nearProtect;
  VirtualProtect(pNear, 0x1000, PAGE_EXECUTE_READ, &nearProtect);

  // The hooks, made but not on until the code is changed
  MH_STATUS status = MH_Initialize();
  if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED)
  {
    util::log::Warning("MinHook couldn't start (MH_STATUS 0x%X), the streamers keep 128 zone slots", status);
    return;
  }
  if (m_pReservedHookSpace)
  {
    VirtualFree(m_pReservedHookSpace, 0, MEM_RELEASE);
    m_pReservedHookSpace = nullptr;
  }
  struct NewHook { const char* Name; BYTE* pTarget; LPVOID pDetour; LPVOID* ppOriginal; };
  NewHook hooks[] = {
    { "ZoneStreamerConstructor", pCtor, (LPVOID)hZoneStreamerCtor, (LPVOID*)&oZoneStreamerCtor },
    { "ZoneRelease", pRelease, (LPVOID)hZoneRelease, (LPVOID*)&oZoneRelease },
    { "ZoneLoadPass", pLoadPass, (LPVOID)hZoneLoadPass, (LPVOID*)&oZoneLoadPass },
  };
  int made = 0;
  for (; made < (int)_countof(hooks); ++made)
  {
    status = MinHookCreate(hooks[made].pTarget, hooks[made].pDetour, hooks[made].ppOriginal);
    if (status != MH_OK)
      break;
  }
  if (made < (int)_countof(hooks))
  {
    util::log::Warning("Could not hook %s (MH_STATUS 0x%X), the streamers keep 128 zone slots", hooks[made].Name, status);
    for (int i = 0; i < made; ++i)
      MH_RemoveHook(hooks[i].pTarget);
    return;
  }

  // Both pages of the game's code writable first, so it's all or nothing
  BYTE* pPageA = (BYTE*)((ULONG_PTR)pSearch & ~(ULONG_PTR)0xFFF);
  BYTE* pPageB = (BYTE*)((ULONG_PTR)pReader1 & ~(ULONG_PTR)0xFFF);
  bool laidOut = pPageA == (BYTE*)((ULONG_PTR)(pSearch + sizeof(kSearch)) & ~(ULONG_PTR)0xFFF);
  for (BYTE* p : { pReader1, pReader2, pReleaseA, pReleaseB })
    laidOut &= (BYTE*)((ULONG_PTR)p & ~(ULONG_PTR)0xFFF) == pPageB && (BYTE*)((ULONG_PTR)(p + 0x21) & ~(ULONG_PTR)0xFFF) == pPageB;
  DWORD protectA = 0, protectB = 0;
  bool writable = laidOut && VirtualProtect(pPageA, 0x1000, PAGE_EXECUTE_READWRITE, &protectA);
  if (writable && !VirtualProtect(pPageB, 0x1000, PAGE_EXECUTE_READWRITE, &protectB))
  {
    VirtualProtect(pPageA, 0x1000, protectA, &protectA);
    writable = false;
  }
  if (!writable)
  {
    for (NewHook const& hook : hooks)
      MH_RemoveHook(hook.pTarget);
    util::log::Warning("The zone streaming code couldn't be changed, the streamers keep 128 zone slots");
    return;
  }
  const unsigned int room = ZoneSlotsRoom;
  memcpy(pSearch + 2, &room, 4);
  WriteJump(pSearch + 0x0F, 9, stubs[0].pCave);
  WriteJump(pSearch + 0x2B, 8, stubs[1].pCave);
  WriteJump(pReader1 + 0x10, 8, stubs[2].pCave);
  WriteJump(pReader2 + 0x10, 8, stubs[3].pCave);
  WriteJump(pReleaseA + 0x0D, 7, stubs[4].pCave);
  WriteJump(pReleaseB + 0x0D, 7, stubs[5].pCave);

  // Then the hooks on; if one can't be, the code goes back as it was
  bool enabled = true;
  for (NewHook const& hook : hooks)
    enabled &= MH_EnableHook(hook.pTarget) == MH_OK;
  if (!enabled)
  {
    for (NewHook const& hook : hooks)
    {
      MH_DisableHook(hook.pTarget);
      MH_RemoveHook(hook.pTarget);
    }
    memcpy(pSearch, kSearch, sizeof(kSearch));
    memcpy(pReader1, kReader1, sizeof(kReader1));
    memcpy(pReader2, kReader2, sizeof(kReader2));
    memcpy(pReleaseA, kRelease2, sizeof(kRelease2));
    memcpy(pReleaseB, kRelease2, sizeof(kRelease2));
  }
  VirtualProtect(pPageA, 0x1000, protectA, &protectA);
  VirtualProtect(pPageB, 0x1000, protectB, &protectB);
  FlushInstructionCache(GetCurrentProcess(), pPageA, 0x1000);
  FlushInstructionCache(GetCurrentProcess(), pPageB, 0x1000);
  if (!enabled)
  {
    util::log::Warning("The zone streaming hooks couldn't be turned on, the streamers keep 128 zone slots");
    return;
  }

  for (NewHook const& hook : hooks)
  {
    util::hooks::Hook info{ 0 };
    info.Address = (__int64)hook.pTarget;
    info.Type = util::hooks::HookType::MinHook;
    info.Enabled = true;
    info.Keep = true;
    NoteCreatedHook(hook.Name, info);
    util::log::Ok("Hooked %s at 0x%I64X", hook.Name, (__int64)hook.pTarget);
  }
  g_ZoneSlotsMoved = true;
  util::log::Ok("The zone streamers have room for %d zones each instead of 128 (code at 0x%I64X)", ZoneSlotsRoom, (__int64)pNear);
}

// The renderer's instances' 0xA0-byte records come from a pool in one object
// (made once by 0x143567680, kept at 0x142563A30; the pool at + 0x40) that
// keeps its blocks' addresses in room for 256 inside itself (+ 0x40 ..
// + 0x840, the count right after) and takes a new block of 1024 records
// without a check (0x143577780): block 257 goes over the count. A record's
// number gives its block as number >> 10 and its place as number & 0x3FF, so
// the blocks stay 1024 records and the list of them moves: the constructor's
// hook puts the address of a list with room for RecordBlocksRoom where the
// first block's was, and the ten reads by number (all [owner + block * 8 +
// 0x40]), the grow and the clear that frees every block (0x143568C90) go
// through a few lines next to the game that use it. They were found by their
// bytes over the whole mesh and renderer code (a disassembly from each
// function's start missed one read and the clear).
namespace
{
  const __int64 RecordOwnerGlobalRva = 0x2563A30;
  const __int64 RecordOwnerCtorRva = 0x3567680;
  const __int64 RecordGrowRva = 0x35777BC;
  const int RecordBlocksField = 0x40;
  const int RecordBlocksRoom = 1024;     // 1048576 records, past the renderer's 1000000 instances
  const unsigned int RecordsPerBlock = 1024;
  std::atomic<bool> g_RecordBlocksMoved{ false };
  __int64 g_RecordBlocksFallback[RecordBlocksRoom] = {}; // only if a list can't be had (there's one owner)
}

typedef __int64(__fastcall* tRecordOwnerCtor)(__int64, __int64, __int64, __int64);
tRecordOwnerCtor oRecordOwnerCtor = nullptr;
__int64 __fastcall hRecordOwnerCtor(__int64 pOwner, __int64 a2, __int64 a3, __int64 a4)
{
  __int64 result = oRecordOwnerCtor(pOwner, a2, a3, a4);
  // The constructor zeroed the 256 places; the first one now holds the list
  void* pBlocks = VirtualAlloc(NULL, RecordBlocksRoom * sizeof(__int64), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
  if (!pBlocks)
  {
    util::log::Error("No memory for the renderer's instance record list, it uses a spare one");
    pBlocks = g_RecordBlocksFallback;
  }
  *(__int64*)(pOwner + RecordBlocksField) = (__int64)pBlocks;
  return result;
}

unsigned int util::hooks::RecordPoolRoom()
{
  return (g_RecordBlocksMoved ? RecordBlocksRoom : 256) * RecordsPerBlock;
}

// Called with the other enlargements, before the game makes the pool's
// object. All or nothing, like EnlargeZoneSlots.
void util::hooks::EnlargeRecordPool()
{
  BYTE* pBase = (BYTE*)g_gameHandle;
  BYTE* pCtor = pBase + RecordOwnerCtorRva;
  BYTE* pClear = pBase + 0x35676BA;
  BYTE* pGrow = pBase + RecordGrowRva;

  // The constructor's start and its clear of the pool (count, block, cursor,
  // end, allocator, then the 256 places: 0x800 bytes); the grow from
  // mov ecx, [rbx + 0x800] to mov rax, [rbx + rax*8]
  static const BYTE kCtor[] = { 0x48, 0x89, 0x4C, 0x24, 0x08, 0x57, 0x48, 0x83, 0xEC, 0x50 };
  static const BYTE kClear[] = { 0x48, 0x8D, 0x4F, 0x40, 0x89, 0xB1, 0x00, 0x08, 0x00, 0x00, 0x48, 0x89, 0xB1, 0x08, 0x08,
    0x00, 0x00, 0x48, 0x89, 0xB1, 0x10, 0x08, 0x00, 0x00, 0x48, 0x89, 0xB1, 0x18, 0x08, 0x00, 0x00, 0x48, 0x89, 0x99,
    0x20, 0x08, 0x00, 0x00, 0x31, 0xD2, 0x41, 0xB8, 0x0F, 0xA2, 0x47, 0xC3, 0x45, 0x8D, 0x80, 0xF1, 0x65, 0xB8, 0x3C };
  static const BYTE kGrow[] = { 0x8B, 0x8B, 0x00, 0x08, 0x00, 0x00, 0x48, 0x89, 0x04, 0xCB, 0xFF, 0x83, 0x00, 0x08, 0x00,
    0x00, 0x8B, 0x83, 0x00, 0x08, 0x00, 0x00, 0xFF, 0xC8, 0x48, 0x8B, 0x04, 0xC3 };
  // The clear's loop (rdi is the pool): for each block below the count,
  // lea rsi, [rdi + rax*8]; mov rdx, [rsi]; free it; mov [rsi], 0
  BYTE* pClearLoop = pBase + 0x3568CA6;
  static const BYTE kClearLoop[] = { 0x39, 0x99, 0x00, 0x08, 0x00, 0x00, 0x76, 0x31, 0x48, 0x89, 0x74, 0x24, 0x30, 0x89,
    0xD8, 0x48, 0x8D, 0x34, 0xC7, 0x48, 0x8B, 0x16, 0x48, 0x85, 0xD2, 0x74, 0x0F, 0x48, 0x8B, 0x8F, 0x20, 0x08, 0x00, 0x00,
    0xE8, 0x53, 0xF5, 0x50, 0xFF, 0x48, 0x89, 0x2E, 0xFF, 0xC3, 0x3B, 0x9F, 0x00, 0x08, 0x00, 0x00, 0x72, 0xD9 };
  // lea rsi, [rdi + rax*8]; mov rdx, [rsi] -> mov rsi, [rdi]; lea rsi, [rsi + rax*8]; mov rdx, [rsi]
  static const BYTE clearBody[] = { 0x48, 0x8B, 0x37, 0x48, 0x8D, 0x34, 0xC6, 0x48, 0x8B, 0x16 };

  // The nine reads (5 bytes each) and what each becomes:
  // op reg, [owner + block * 8 + 0x40] -> mov tmp, [owner + 0x40]; op reg, [tmp + block * 8]
  struct Read { __int64 Rva; BYTE Code[5]; std::vector<BYTE> Body; };
  Read reads[] = {
    // add r14, [rbx + rcx*8 + 0x40]: push rax; mov rax, [rbx + 0x40]; add r14, [rax + rcx*8]; pop rax
    { 0x356AE4B, { 0x4C, 0x03, 0x74, 0xCB, 0x40 }, { 0x50, 0x48, 0x8B, 0x43, 0x40, 0x4C, 0x03, 0x34, 0xC8, 0x58 } },
    // add rdx, [rsi + rcx*8 + 0x40]: push rax; mov rax, [rsi + 0x40]; add rdx, [rax + rcx*8]; pop rax
    { 0x35730F1, { 0x48, 0x03, 0x54, 0xCE, 0x40 }, { 0x50, 0x48, 0x8B, 0x46, 0x40, 0x48, 0x03, 0x14, 0xC8, 0x58 } },
    { 0x3573310, { 0x48, 0x03, 0x54, 0xCE, 0x40 }, { 0x50, 0x48, 0x8B, 0x46, 0x40, 0x48, 0x03, 0x14, 0xC8, 0x58 } },
    // mov rcx, [rdi + r8*8 + 0x40]: mov rcx, [rdi + 0x40]; mov rcx, [rcx + r8*8]
    { 0x35735CA, { 0x4A, 0x8B, 0x4C, 0xC7, 0x40 }, { 0x48, 0x8B, 0x4F, 0x40, 0x4A, 0x8B, 0x0C, 0xC1 } },
    { 0x35736D7, { 0x4A, 0x8B, 0x4C, 0xC7, 0x40 }, { 0x48, 0x8B, 0x4F, 0x40, 0x4A, 0x8B, 0x0C, 0xC1 } },
    // mov rax, [rdi + rdx*8 + 0x40]: mov rax, [rdi + 0x40]; mov rax, [rax + rdx*8]
    { 0x35737CC, { 0x48, 0x8B, 0x44, 0xD7, 0x40 }, { 0x48, 0x8B, 0x47, 0x40, 0x48, 0x8B, 0x04, 0xD0 } },
    // mov rax, [rbx + rdx*8 + 0x40]: mov rax, [rbx + 0x40]; mov rax, [rax + rdx*8]
    { 0x3573937, { 0x48, 0x8B, 0x44, 0xD3, 0x40 }, { 0x48, 0x8B, 0x43, 0x40, 0x48, 0x8B, 0x04, 0xD0 } },
    // mov rax, [rdi + rax*8 + 0x40]: push rdx; mov rdx, [rdi + 0x40]; mov rax, [rdx + rax*8]; pop rdx
    { 0x357B9BF, { 0x48, 0x8B, 0x44, 0xC7, 0x40 }, { 0x52, 0x48, 0x8B, 0x57, 0x40, 0x48, 0x8B, 0x04, 0xC2, 0x5A } },
    // add rcx, [rdi + rax*8 + 0x40]: push rdx; mov rdx, [rdi + 0x40]; add rcx, [rdx + rax*8]; pop rdx
    { 0x357BA47, { 0x48, 0x03, 0x4C, 0xC7, 0x40 }, { 0x52, 0x48, 0x8B, 0x57, 0x40, 0x48, 0x03, 0x0C, 0xC2, 0x5A } },
    // mov rax, [rbx + rdx*8 + 0x40]: mov rax, [rbx + 0x40]; mov rax, [rax + rdx*8]
    { 0x357BB2E, { 0x48, 0x8B, 0x44, 0xD3, 0x40 }, { 0x48, 0x8B, 0x43, 0x40, 0x48, 0x8B, 0x04, 0xD0 } },
  };
  // The grow (rbx is the pool, owner + 0x40): mov ecx, [rbx + 0x800]; mov r8, [rbx];
  // mov [r8 + rcx*8], rax; inc dword [rbx + 0x800]; mov eax, [rbx + 0x800]; dec eax;
  // mov rax, [r8 + rax*8]
  static const BYTE growBody[] = { 0x8B, 0x8B, 0x00, 0x08, 0x00, 0x00, 0x4C, 0x8B, 0x03, 0x49, 0x89, 0x04, 0xC8, 0xFF, 0x83,
    0x00, 0x08, 0x00, 0x00, 0x8B, 0x83, 0x00, 0x08, 0x00, 0x00, 0xFF, 0xC8, 0x49, 0x8B, 0x04, 0xC0 };

  if (*(__int64*)(pBase + RecordOwnerGlobalRva))
  {
    util::log::Warning("The renderer's instance record pool was already made, too late to give it more than 262144");
    return;
  }
  bool same = !memcmp(pCtor, kCtor, sizeof(kCtor)) && !memcmp(pClear, kClear, sizeof(kClear)) &&
    !memcmp(pGrow, kGrow, sizeof(kGrow)) && !memcmp(pClearLoop, kClearLoop, sizeof(kClearLoop));
  for (Read const& read : reads)
    same &= !memcmp(pBase + read.Rva, read.Code, sizeof(read.Code));
  if (!same)
  {
    util::log::Warning("The renderer's instance record code isn't what the tools know, it keeps room for 262144");
    return;
  }

  BYTE* pNear = AllocNearGame(0x1000);
  if (!pNear)
  {
    util::log::Warning("No memory next to the game for the instance records' code, they keep room for 262144");
    return;
  }
  bool ok = true;
  auto writeStub = [&](BYTE* pStub, BYTE const* pBody, size_t size, BYTE const* pBack)
  {
    memcpy(pStub, pBody, size);
    int rel = 0;
    ok &= Rel32(pStub + size + 5, pBack, rel);
    pStub[size] = 0xE9;
    memcpy(pStub + size + 1, &rel, 4);
  };
  for (size_t i = 0; i < _countof(reads); ++i)
    writeStub(pNear + i * 0x20, reads[i].Body.data(), reads[i].Body.size(), pBase + reads[i].Rva + 5);
  BYTE* pGrowStub = pNear + _countof(reads) * 0x20;
  writeStub(pGrowStub, growBody, sizeof(growBody), pGrow + sizeof(kGrow));
  BYTE* pClearStub = pGrowStub + 0x40;
  writeStub(pClearStub, clearBody, sizeof(clearBody), pClearLoop + 0x16);
  if (!ok)
  {
    VirtualFree(pNear, 0, MEM_RELEASE);
    util::log::Warning("The instance records' code couldn't be put within reach of the game, they keep room for 262144");
    return;
  }
  DWORD nearProtect;
  VirtualProtect(pNear, 0x1000, PAGE_EXECUTE_READ, &nearProtect);

  // The constructor's hook, made but not on until the code is changed
  MH_STATUS status = MH_Initialize();
  if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED)
  {
    util::log::Warning("MinHook couldn't start (MH_STATUS 0x%X), the instance records keep room for 262144", status);
    return;
  }
  if (m_pReservedHookSpace)
  {
    VirtualFree(m_pReservedHookSpace, 0, MEM_RELEASE);
    m_pReservedHookSpace = nullptr;
  }
  status = MinHookCreate(pCtor, (LPVOID)hRecordOwnerCtor, (LPVOID*)&oRecordOwnerCtor);
  if (status != MH_OK)
  {
    util::log::Warning("Could not hook the instance record pool's constructor (MH_STATUS 0x%X), it keeps room for 262144", status);
    return;
  }

  // Every page of the game's code that changes writable first, so it's all or nothing
  std::vector<BYTE*> pages;
  auto addPage = [&](BYTE* p, size_t size)
  {
    for (BYTE* page : { (BYTE*)((ULONG_PTR)p & ~(ULONG_PTR)0xFFF), (BYTE*)((ULONG_PTR)(p + size - 1) & ~(ULONG_PTR)0xFFF) })
    {
      if (std::find(pages.begin(), pages.end(), page) == pages.end())
        pages.push_back(page);
    }
  };
  for (Read const& read : reads)
    addPage(pBase + read.Rva, sizeof(read.Code));
  addPage(pGrow, sizeof(kGrow));
  addPage(pClearLoop, sizeof(kClearLoop));
  std::vector<DWORD> protects(pages.size(), 0);
  size_t made = 0;
  for (; made < pages.size(); ++made)
  {
    if (!VirtualProtect(pages[made], 0x1000, PAGE_EXECUTE_READWRITE, &protects[made]))
      break;
  }
  if (made < pages.size())
  {
    for (size_t i = 0; i < made; ++i)
      VirtualProtect(pages[i], 0x1000, protects[i], &protects[i]);
    MH_RemoveHook(pCtor);
    util::log::Warning("The instance records' code couldn't be changed, they keep room for 262144");
    return;
  }
  for (size_t i = 0; i < _countof(reads); ++i)
    WriteJump(pBase + reads[i].Rva, 5, pNear + i * 0x20);
  WriteJump(pGrow, (int)sizeof(kGrow), pGrowStub);
  WriteJump(pClearLoop + 0x0F, 7, pClearStub);

  // Then the hook on; if it can't be, the code goes back as it was
  bool enabled = MH_EnableHook(pCtor) == MH_OK;
  if (!enabled)
  {
    MH_RemoveHook(pCtor);
    for (Read const& read : reads)
      memcpy(pBase + read.Rva, read.Code, sizeof(read.Code));
    memcpy(pGrow, kGrow, sizeof(kGrow));
    memcpy(pClearLoop, kClearLoop, sizeof(kClearLoop));
  }
  for (size_t i = 0; i < pages.size(); ++i)
  {
    VirtualProtect(pages[i], 0x1000, protects[i], &protects[i]);
    FlushInstructionCache(GetCurrentProcess(), pages[i], 0x1000);
  }
  if (!enabled)
  {
    util::log::Warning("The instance record pool's hook couldn't be turned on, it keeps room for 262144");
    return;
  }

  util::hooks::Hook info{ 0 };
  info.Address = (__int64)pCtor;
  info.Type = util::hooks::HookType::MinHook;
  info.Enabled = true;
  info.Keep = true;
  NoteCreatedHook("RecordPoolConstructor", info);
  util::log::Ok("Hooked RecordPoolConstructor at 0x%I64X", (__int64)pCtor);
  g_RecordBlocksMoved = true;
  util::log::Ok("The renderer's instance records have room for %u instead of 262144 (code at 0x%I64X)",
    util::hooks::RecordPoolRoom(), (__int64)pNear);
}

void util::hooks::Init()
{
  // Already started for the mesh manager's constructor (EnlargeMeshTable)
  MH_STATUS status = MH_Initialize();
  if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED)
  {
    util::log::Error("Failed to initialize MinHook, MH_STATUS 0x%X", status);
    return;
  }

  m_PresentAddress = GetPresentAddress();
  CreateHook("SwapChainPresent", m_PresentAddress, hIDXGISwapChain_Present, &oIDXGISwapChain_Present);

  HMODULE hUser32 = GetModuleHandleA("user32.dll");
  CreateHook("SetCursorPos", (__int64)GetProcAddress(hUser32, "SetCursorPos"), hSetCursorPos, &oSetCursorPos);
  CreateHook("ClipCursor", (__int64)GetProcAddress(hUser32, "ClipCursor"), hClipCursor, &oClipCursor);
  CreateHook("SetCursor", (__int64)GetProcAddress(hUser32, "SetCursor"), hSetCursor, &oSetCursor);
  CreateHook("ShowCursor", (__int64)GetProcAddress(hUser32, "ShowCursor"), hShowCursor, &oShowCursor);

  // Free the placeholder so MinHook can put the game hooks' trampolines there
  if (m_pReservedHookSpace)
  {
    VirtualFree(m_pReservedHookSpace, 0, MEM_RELEASE);
    m_pReservedHookSpace = nullptr;
  }

  if (IsGamePresentCode((BYTE*)g_gameHandle))
    CreateHook("GamePresent", (__int64)g_gameHandle + GamePresentRva, hGamePresent, &oGamePresent);
  else
    util::log::Warning("The game's present function isn't the code the tools know, the tools draw from dxgi's Present only");
  CreateHook("CameraUpdate", util::offsets::GetOffset("OFFSET_CAMERAUPDATE"), hCameraUpdate, &oCameraUpdate);
  CreateHook("EntryInput", util::offsets::GetOffset("OFFSET_ENTRYINPUT"), hEntryInput, &oEntryInput);
  CreateHook("VisualEnvironmentUpdate", util::offsets::GetOffset("OFFSET_VISUALENVUPDATE"), hVisualEnvironmentUpdate, &oVisualEnvironmentUpdate);
  // Faith's hit effects left out of the picture while the tools' camera is on,
  // and the rain's strength given (weather::AfterViewMix)
  if (IsVisualEnvironmentBlendCode((BYTE*)g_gameHandle))
    CreateHook("VisualEnvironmentBlend", (__int64)g_gameHandle + VisualEnvironmentBlendRva, hVisualEnvironmentBlend,
      &oVisualEnvironmentBlend);
  else
    util::log::Warning("The game's visual environment mix isn't the code the tools know: Faith's hit effects show on the free camera, "
      "and the weather's rain can't be given a strength (it stays unseen in the city)");
  // The game's drops and water on the lens left out while the tools' camera is on
  if (IsEmitterViewCullCode((BYTE*)g_gameHandle))
    CreateHook("EmitterViewCull", (__int64)g_gameHandle + EmitterViewCullRva, hEmitterViewCull, &oEmitterViewCull);
  else
    util::log::Warning("The game's emitter view cull isn't the code the tools know: the game's drops and water on the lens show on "
      "the free camera");
  // The video billboards sized from the tools' camera
  if (IsBillboardCameraCode((BYTE*)g_gameHandle))
  {
    CreateHook("BillboardSizes", (__int64)g_gameHandle + BillboardSizesRva, hBillboardSizes, &oBillboardSizes);
    CreateHook("CameraEntryTransform", (__int64)g_gameHandle + CameraEntryTransformRva, hCameraEntryTransform, &oCameraEntryTransform);
    CreateHook("CameraEntryFov", (__int64)g_gameHandle + CameraEntryFovRva, hCameraEntryFov, &oCameraEntryFov);
  }
  else
    util::log::Warning("The billboards' code isn't what the tools know: they're sized from Faith's camera, not the free camera");
  // And their videos at the tools' game speed
  if (IsFeedTimeCode((BYTE*)g_gameHandle))
    CreateHook("FeedTime", (__int64)g_gameHandle + FeedTimeRva, hFeedTime, &oFeedTime);
  else
    util::log::Warning("The billboards' video clock isn't the code the tools know: the ones with sound play at full speed in Game speed");
  CreateHook("ZoneStreamerUpdate", util::offsets::GetOffset("OFFSET_ZONESTREAMERUPDATE"), hZoneStreamerUpdate, &oZoneStreamerUpdate);
  CreateHook("ZoneLoadOrder", util::offsets::GetOffset("OFFSET_ZONELOADORDER"), hZoneLoadOrder, &oZoneLoadOrder);
  CreateHook("VistaShowZone", util::offsets::GetOffset("OFFSET_VISTASHOWZONE"), hVistaShowZone, &oVistaShowZone);
  CreateHook("ListenerTransform", util::offsets::GetOffset("OFFSET_LISTENERTRANSFORM"), hListenerSetTransform, &oListenerSetTransform);
  CreateHook("ListenerVelocity", util::offsets::GetOffset("OFFSET_LISTENERVELOCITY"), hListenerSetVelocity, &oListenerSetVelocity);
  // Diagnostic (temporary): only on the code that was read
  if (IsSoundLogCode((BYTE*)g_gameHandle))
  {
    CreateHook("SoundCreate", (__int64)g_gameHandle + SoundCreateRva, hSoundCreate, &oSoundCreate);
    CreateHook("VoiceTransform", (__int64)g_gameHandle + VoiceTransformRva, hVoiceSetTransform, &oVoiceSetTransform);
    // A cutscene's lines in the replay (see ReplaySoundAsset)
    if (IsVoiceSetAssetCode((BYTE*)g_gameHandle))
      CreateHook("VoiceSetAsset", (__int64)g_gameHandle + 0x301DA80, hVoiceSetAsset, &oVoiceSetAsset);
    else
      util::log::Warning("The voices' sound parameter code isn't what the tools know, a cutscene's lines aren't in replays");
    CreateHook("VoiceEvent", (__int64)g_gameHandle + VoiceEventRva, hVoiceEvent, &oVoiceEvent);
    CreateHook("SoundEntityEvent", (__int64)g_gameHandle + SoundEntityEventRva, hSoundEntityEvent, &oSoundEntityEvent);
    if (IsSoundEntityLifeCode((BYTE*)g_gameHandle))
    {
      CreateHook("SoundEntityCtor", (__int64)g_gameHandle + 0x30170C0, hSoundEntityCtor, &oSoundEntityCtor);
      CreateHook("SoundEntityDtor", (__int64)g_gameHandle + 0x3017BA0, hSoundEntityDtor, &oSoundEntityDtor);
    }
    else
      util::log::Warning("The sound entity's constructor isn't the code the tools know, sound entities aren't listed");
    if (IsTrackedPositionCode((BYTE*)g_gameHandle))
      CreateHook("TrackedPosition", (__int64)g_gameHandle + TrackedPositionRva, hTrackedPosition, &oTrackedPosition);
    else
      util::log::Warning("The crowd's proximity tracker isn't the code the tools know, crowd and traffic sounds stay with Faith");
    // The voice lines a recording had, said again while it plays (see hVoiceLineQueue)
    if (IsVoiceLineCode((BYTE*)g_gameHandle))
    {
      CreateHook("VoiceNode", (__int64)g_gameHandle + VoiceNodeRva, hVoiceNode, &oVoiceNode);
      if (oVoiceNode)
        CreateHook("VoiceLineQueue", (__int64)g_gameHandle + VoiceLineQueueRva, hVoiceLineQueue, &oVoiceLineQueue);
      if (oVoiceLineQueue)
        CreateHook("VoiceLineStart", (__int64)g_gameHandle + VoiceLineStartRva, hVoiceLineStart, &oVoiceLineStart);
      if (oVoiceLineStart)
        CreateHook("VoiceLineClear", (__int64)g_gameHandle + VoiceLineClearRva, hVoiceLineClear, &oVoiceLineClear);
    }
    else
      util::log::Warning("The voice-over system's line code isn't what the tools know, a recording's voice lines aren't played again");
  }
  else
    util::log::Warning("The sound system's voices aren't the code the tools know, Faith's sounds can't be logged");
  // The replay's people
  if (IsAIAutoDespawnCode((BYTE*)g_gameHandle))
  {
    CreateHook("AIAutoDespawnPam", (__int64)g_gameHandle + AIAutoDespawnPamRva, hAIAutoDespawnPam, &oAIAutoDespawnPam);
    CreateHook("AIAutoDespawnBase", (__int64)g_gameHandle + AIAutoDespawnBaseRva, hAIAutoDespawnBase, &oAIAutoDespawnBase);
    // The KrugerSec spawners, listed for stand-ins (see hSpawnerCtor)
    if (IsSpawnerLifeCode((BYTE*)g_gameHandle))
    {
      CreateHook("SpawnerDtor", (__int64)g_gameHandle + SpawnerDtorRva, hSpawnerDtor, &oSpawnerDtor);
      if (oSpawnerDtor)
        CreateHook("SpawnerCtor", (__int64)g_gameHandle + SpawnerCtorRva, hSpawnerCtor, &oSpawnerCtor);
      // Their Spawn, for the replay's stand-ins (see AskStandIn; nothing hooked)
      g_StandInCode = IsStandInCode((BYTE*)g_gameHandle);
      if (!g_StandInCode)
        util::log::Warning("The KrugerSec spawners' Spawn isn't what the tools know, the replay makes no stand-ins");
      g_SpawnerLimitsCode = g_StandInCode && IsSpawnerLimitCode((BYTE*)g_gameHandle);
      if (g_StandInCode && !g_SpawnerLimitsCode)
        util::log::Warning("The spawners' limits aren't read the way the tools know: a stand-in may wait on a spawner at its limit");
    }
    else
      util::log::Warning("The KrugerSec spawners' code isn't what the tools know, they aren't listed");
  }
  else
    util::log::Warning("The AI's despawn code isn't what the tools know, the game may take replayed people out of the level");
  if (IsHumanDamageCode((BYTE*)g_gameHandle))
    CreateHook("HumanDamage", (__int64)g_gameHandle + HumanDamageRva, hHumanDamage, &oHumanDamage);
  else
    util::log::Warning("Faith's health code isn't what the tools know, she can still be hurt while a replay plays");
  // A body's place for its placement, so the replay's guns are drawn where it has them
  if (IsBodyTransformCode((BYTE*)g_gameHandle))
    CreateHook("BodyTransform", (__int64)g_gameHandle + BodyTransformRva, hBodyTransform, &oBodyTransform);
  else
    util::log::Warning("The game's body placement isn't the code the tools know: replayed guns stay where they lie");
  // The loading screen, so the replay lets go of its people when a load waits on them
  if (IsLoadScreenEventCode((BYTE*)g_gameHandle))
    CreateHook("LoadScreenEvent", (__int64)g_gameHandle + LoadScreenEventRva, hLoadScreenEvent, &oLoadScreenEvent);
  else
    util::log::Warning("The game's loading screen code isn't what the tools know: a load may wait while the replay keeps people");
  // The recorded mission's world, kept after the mission is over (let go at a
  // loading screen, so only with that hooked)
  if (oLoadScreenEvent && IsMissionWorldCode((BYTE*)g_gameHandle))
  {
    CreateHook("SubLevelDtor", (__int64)g_gameHandle + SubLevelDtorRva, hSubLevelDtor, &oSubLevelDtor);
    if (oSubLevelDtor)
    {
      CreateHook("SubLevelEvent", (__int64)g_gameHandle + SubLevelEventRva, hSubLevelEvent, &oSubLevelEvent);
      CreateHook("MissionStreamOut", (__int64)g_gameHandle + MissionStreamOutRva, hMissionStreamOut, &oMissionStreamOut);
    }
    g_WorldsCode = oSubLevelDtor && oSubLevelEvent;
  }
  if (!g_WorldsCode)
    util::log::Warning("The game's mission and sub-level streaming code isn't what the tools know: a mission's world isn't kept "
      "for the replay once it's over");
  // Diagnostic (temporary): the effects the game plays near Faith
  if (IsEffectPlayCode((BYTE*)g_gameHandle))
    CreateHook("EffectPlay", (__int64)g_gameHandle + EffectPlayRva, hEffectPlay, &oEffectPlay);
  else
    util::log::Warning("The game's effect code isn't what the tools know: the replay's gun effects and the puddle rings "
      "from slides are off");
  // An effect's moves and stop, so the replay's tracers fly as the recorded ones did
  if (oEffectPlay && IsEffectMoveStopCode((BYTE*)g_gameHandle))
  {
    CreateHook("EffectMove", (__int64)g_gameHandle + EffectMoveRva, hEffectMove, &oEffectMove);
    CreateHook("EffectStop", (__int64)g_gameHandle + EffectStopRva, hEffectStop, &oEffectStop);
    // Diagnostic (temporary): effects given parameters while the replay records
    if (IsEffectParamsCode((BYTE*)g_gameHandle))
      CreateHook("EffectParams", (__int64)g_gameHandle + EffectParamsRva, hEffectParams, &oEffectParams);
  }
  else
    util::log::Warning("The game's effect move / stop code isn't what the tools know: replayed tracers don't fly");
  // Faith's footsteps in the wet surfaces' puddles: the game's steps in water
  if (IsWetStepsCode((BYTE*)g_gameHandle))
  {
    CreateHook("ImpactSurface", (__int64)g_gameHandle + ImpactSurfaceRva, hImpactSurface, &oImpactSurface);
    if (oImpactSurface)
    {
      CreateHook("ImpactSoundHandler", (__int64)g_gameHandle + ImpactSoundHandlerRva, hImpactSoundHandler, &oImpactSoundHandler);
      CreateHook("ImpactEffectHandler", (__int64)g_gameHandle + ImpactEffectHandlerRva, hImpactEffectHandler, &oImpactEffectHandler);
    }
  }
  else
    util::log::Warning("The game's footstep code isn't what the tools know: Faith's steps in the wet surfaces' puddles sound as "
      "they always do");
  // The replay's people kept in the level (the server destructor is what keeps
  // that safe)
  if (IsCharacterDtorCode((BYTE*)g_gameHandle))
  {
    CreateHook("ServerCharacterDtor", (__int64)g_gameHandle + ServerCharacterDtorRva, hServerCharacterDtor, &oServerCharacterDtor);
    if (IsServerDroneDtorCode((BYTE*)g_gameHandle))
      CreateHook("ServerDroneDtor", (__int64)g_gameHandle + ServerDroneDtorRva, hServerDroneDtor, &oServerDroneDtor);
    else
      util::log::Warning("The server drones' destructor isn't the code the tools know, recorded drones aren't kept in the level");
    if (IsServerVehicleDtorCode((BYTE*)g_gameHandle))
      CreateHook("ServerVehicleDtor", (__int64)g_gameHandle + ServerVehicleDtorRva, hServerVehicleDtor, &oServerVehicleDtor);
    else
      util::log::Warning("The server vehicles' destructor isn't the code the tools know, recorded vehicles aren't kept in the level");
    if (IsDestroyRequestCode((BYTE*)g_gameHandle))
      CreateHook("DestroyRequest", (__int64)g_gameHandle + DestroyRequestRva, hDestroyRequest, &oDestroyRequest);
    else
      util::log::Warning("The game's destroy request isn't the code the tools know, the game may take replayed people out of the level");
    // The client characters' destructor, so a box the replay gives one is never
    // written once it's gone; then the box
    CreateHook("ClientCharacterDtor", (__int64)g_gameHandle + ClientCharacterDtorRva, hClientCharacterDtor, &oClientCharacterDtor);
    if (oClientCharacterDtor && IsCharacterBoxCode((BYTE*)g_gameHandle))
      CreateHook("CharacterBox", (__int64)g_gameHandle + CharacterBoxRva, hCharacterBox, &oCharacterBox);
    else
      util::log::Warning("The characters' culling box isn't the code the tools know: KrugerSec the replay draws itself may vanish up "
        "close");
  }
  else
    util::log::Warning("The characters' destructors aren't the code the tools know, the game may take replayed people out of the level");
  // The replay's people kept in the level when a chase ends (its sub-level kept loaded)
  if (IsSubLevelUnloadCode((BYTE*)g_gameHandle))
  {
    int types = 0;
    for (int t = 0; t < (int)_countof(KeptPeopleTypes); ++t)
      types += g_KeptPeopleTypeOk[t] = TypeInfoNamed((__int64)g_gameHandle + KeptPeopleTypes[t].TypeInfoRva, KeptPeopleTypes[t].Name);
    if (types == (int)_countof(KeptPeopleTypes))
      CreateHook("SubLevelUnload", (__int64)g_gameHandle + SubLevelUnloadRva, hSubLevelUnload, &oSubLevelUnload);
    else
      util::log::Warning("The game's character classes aren't the ones the tools know, a chase's KrugerSec go from the replay when it ends");
  }
  else
    util::log::Warning("The game's sub-level unloading isn't the code the tools know, a chase's KrugerSec go from the replay when it ends");
  // The bodies' "always update" switch: the dead ones noted, live ones the game doesn't animate switched on for playbacks
  if (IsAlwaysUpdateCode((BYTE*)g_gameHandle))
    CreateHook("AlwaysUpdate", (__int64)g_gameHandle + AlwaysUpdateRva, hAlwaysUpdate, &oAlwaysUpdate);
  else
    util::log::Warning("The bodies' update switch isn't the code the tools know, KrugerSec far from Faith may not show in the replay");
  // The bodies the replay draws itself, left to it
  if (IsBodyUpdateGateCode((BYTE*)g_gameHandle))
    CreateHook("BodyUpdateGate", (__int64)g_gameHandle + BodyUpdateGateRva, hBodyUpdateGate, &oBodyUpdateGate);
  else
    util::log::Warning("The game's body update isn't the code the tools know, taken-down KrugerSec may flicker in the replay");
  // Diagnostic (temporary): what takes the replay's people out of the level past the destroy request
  if (IsContainerDestroyCode((BYTE*)g_gameHandle))
  {
    CreateHook("ContainerDestroyAll", (__int64)g_gameHandle + ContainerDestroyAllRva, hContainerDestroyAll, &oContainerDestroyAll);
    if (util::DiagnosticsOn())   // only for the diagnostic
      CreateHook("ContainerDestroyOne", (__int64)g_gameHandle + ContainerDestroyOneRva, hContainerDestroyOne, &oContainerDestroyOne);
  }
  else
    util::log::Warning("The game's entity containers aren't the code the tools know, the replay doesn't log what takes people out");
  // The replay's moving lights
  if (IsLightPropertyCode((BYTE*)g_gameHandle))
    CreateHook("LightPropertyChanged", (__int64)g_gameHandle + LightPropertyChangedRva, hLightPropertyChanged, &oLightPropertyChanged);
  else
    util::log::Warning("The lights' code isn't what the tools know, the replay doesn't move lights");
  // Diagnostic (temporary): the world log's timelines and MagRope line. The
  // line's constructor / destructor (its registry) stay for the replay's
  // MagRope line when the world log goes.
  if (IsWorldLogCode((BYTE*)g_gameHandle))
  {
    if (util::DiagnosticsOn())   // the world log's only
    {
      CreateHook("TimelineCtor", (__int64)g_gameHandle + TimelineCtorRva, hTimelineCtor, &oTimelineCtor);
      CreateHook("TimelineDtor", (__int64)g_gameHandle + TimelineDtorRva, hTimelineDtor, &oTimelineDtor);
    }
    CreateHook("RopeCtor", (__int64)g_gameHandle + RopeCtorRva, hRopeCtor, &oRopeCtor);
    CreateHook("RopeDtor", (__int64)g_gameHandle + RopeDtorRva, hRopeDtor, &oRopeDtor);
  }
  else
    util::log::Warning("The timelines' and the MagRope line's code isn't what the tools know, the world can't be logged");

  // Only when its code is what was seen (see hMeshGather)
  __int64 pMeshGather = util::offsets::GetOffset("OFFSET_MESHGATHER");
  if (pMeshGather)
  {
    if (SetUpMeshGatherGuard(pMeshGather))
      CreateHook("MeshGather", pMeshGather, hMeshGather, &oMeshGather);
    else
      util::log::Warning("MeshGather isn't the code the tools know, the guard for entities of unloaded zones is off");
  }

  // Only on the build whose code was read, and with the wider handle (every
  // batch then has all of its chunks)
  if (util::offsets::BatchChunkIndexWidened())
  {
    BYTE* pAdd = (BYTE*)g_gameHandle + 0x36C3960;
    if (IsBatchAddCode(pAdd))
    {
      CreateHook("BatchAdd", (__int64)pAdd, hBatchAdd, &oBatchAdd);
      g_BatchAddHooked = oBatchAdd && IsHookCreated("BatchAdd");
    }
    else
      util::log::Warning("The renderer's add isn't the code the tools know, its draw batches can't be spread");
  }

  BYTE* pReflectionScore = (BYTE*)g_gameHandle + ReflectionScoreRva;
  if (IsReflectionScoreCode(pReflectionScore, (BYTE*)g_gameHandle + ReflectionSortCompareRva))
    CreateHook("ReflectionScore", (__int64)pReflectionScore, hReflectionScore, &oReflectionScore);
  else
    util::log::Warning("The reflection volumes' score isn't the code the tools know, they keep the game's order");

  BYTE* pSunShadowSetup = (BYTE*)g_gameHandle + SunShadowSetupRva;
  if (IsSunShadowSetupCode(pSunShadowSetup))
    CreateHook("SunShadowSetup", (__int64)pSunShadowSetup, hSunShadowSetup, &oSunShadowSetup);
  else
    util::log::Warning("The sun shadows' setup isn't the code the tools know, Shadow distance can't go past a level's own");
  HookFarFlares();
  HookReflectionFaces();
  HookMissionCleanup();
  HookTodLighting();
  HookJobQueueFull();
  if (IsModelSetTransformCode((BYTE*)g_gameHandle))
    CreateHook("ModelSetTransform", (__int64)g_gameHandle + ModelSetTransformRva, hModelSetTransform, &oModelSetTransform);
  else
    util::log::Warning("The models' setTransform isn't the code the tools know, the replay doesn't move guns and drones");
  // The replay's vehicles: the body of that setter, which their own slots reach directly
  if (oModelSetTransform && IsModelSetTransformBodyCode((BYTE*)g_gameHandle))
    CreateHook("ModelSetTransformBody", (__int64)g_gameHandle + ModelSetTransformBodyRva, hModelSetTransformBody,
      &oModelSetTransformBody, false);
  else
    util::log::Warning("The models' setTransform body isn't the code the tools know, the replay doesn't move vehicles");
  if (oModelSetTransformBody && IsTransformQueuedCode((BYTE*)g_gameHandle))
    CreateHook("TransformQueued", (__int64)g_gameHandle + TransformQueuedRva, hTransformQueued, &oTransformQueued);
  else
    util::log::Warning("The entities' queued transform change isn't the code the tools know, a replayed vehicle may be drawn where "
      "the game has it");
  // The replay's MagRope line: its update, and the line registry (the world log's)
  if (oRopeCtor && IsMagropeReplayCode((BYTE*)g_gameHandle))
    CreateHook("MagropeUpdate", (__int64)g_gameHandle + MagropeUpdateRva, hMagropeUpdate, &oMagropeUpdate);
  else
    util::log::Warning("The MagRope line's code isn't what the tools know, the replay doesn't draw it");

  if (IsGroupPropertyCode((BYTE*)g_gameHandle) && IsGroupDtorCode((BYTE*)g_gameHandle))
  {
    CreateHook("ModelGroupDtor", (__int64)g_gameHandle + GroupDtorRva, hGroupDtor, &oGroupDtor);
    if (oGroupDtor)
      CreateHook("ModelGroupProperty", (__int64)g_gameHandle + GroupPropertyRva, hGroupProperty, &oGroupProperty);
  }
  else
    util::log::Warning("The model groups' property code isn't what the tools know: the replay won't show mission items as they were");

  // The Shard's top streamed in around the free camera too (its camera area trigger)
  if (IsLinkVolumeTestCode((BYTE*)g_gameHandle))
    CreateHook("CameraTriggerVolumeTest", (__int64)g_gameHandle + LinkVolumeContainsRva, hLinkVolumeContains, &oLinkVolumeContains);
  else
    util::log::Warning("The camera area triggers' code isn't what the tools know: the Shard's top only streams in around Faith");
  // The camera area triggers noted as they're set up (Unbroken Shard and
  // Bauble, Kruger's office blinds, Street level's Bauble boxes). Checked
  // before the hook (its jump changes the bytes checked), the result given
  // only once the hook is made: the camera's update may already be running
  // (see g_CameraTriggerCodeOk)
  bool triggerCodeKnown = CameraTriggerCodeKnown();
  if (triggerCodeKnown)
    CreateHook("CameraTriggerInit", (__int64)g_gameHandle + CameraTriggerInitRva, hCameraTriggerInit, &oCameraTriggerInit);
  bool triggersHooked = triggerCodeKnown && oCameraTriggerInit && IsHookCreated("CameraTriggerInit");
  g_CameraTriggerCodeOk = triggersHooked ? 1 : 0;
  if (!triggersHooked)
    util::log::Warning("The camera area triggers' set-up %s: Unbroken Shard and Bauble and the Shard office blinds (Kruger's "
      "office) can't switch the game's streaming, and Street level makes no boxes for the Bauble's ruins or the whole mall's roofs",
      triggerCodeKnown ? "couldn't be hooked" : "isn't the code the tools know");

  if (IsPhysicsWorldInitCode((BYTE*)g_gameHandle))
    CreateHook("PhysicsWorldInit", (__int64)g_gameHandle + PhysicsWorldInitRva, hPhysicsWorldInit, &oPhysicsWorldInit);
  else
    util::log::Warning("The physics world set-up isn't the code the tools know: its room for bodies stays the game's (16384)");

  BYTE* pLightMapId = (BYTE*)g_gameHandle + LightMapIdRva;
  if (IsLightMapIdCode(pLightMapId))
    CreateHook("LightMapId", (__int64)pLightMapId, hLightMapId, &oLightMapId);
  else
    util::log::Warning("Enlighten's light map id allocator isn't the code the tools know, with the whole city loaded its ids can run over");
}

// Every second from the tools' loop: whether presents still pass the jump at
// the start of dxgi's Present, once the game's present function has been
// seen a while (another mod's hook put there after the tools' can go past
// it; then the tools draw from the game's present function only)
void util::hooks::CheckPresentHooks()
{
  if (!m_PresentAddress || !oGamePresent || m_PresentJumpChecked)
    return;
  if (!m_PresentJumpCounted)
  {
    if (g_PresentCallsFromGame == 0)
      return;
    m_PresentJumpCounted = true;
    m_JumpCallsAtGameStart = g_PresentCallsFromJump;
    return;
  }
  if (g_PresentCallsFromGame < 300)
    return;
  m_PresentJumpChecked = true;
  if (g_PresentCallsFromJump == m_JumpCallsAtGameStart)
    util::log::Write("Present doesn't pass the tools' hook in dxgi.dll, another mod's Present hook goes past it: the "
      "tools draw from the game's present function");
}

// In some cases it's useful or even required to disable all hooks or just certain ones
void util::hooks::SetHookState(bool enable, std::string const& name)
{
  // (the list under its lock while it's gone through: a hook another thread
  // makes meanwhile waits for this to be added. MinHook's own lock is never
  // held by a thread that asks for this one, so the two can't wait on each other)
  std::lock_guard<std::mutex> lock(m_CreatedHooksMutex);
  if (name.empty())
  {
    if (enable)
    {
      MH_STATUS status = MH_EnableHook(MH_ALL_HOOKS);
      if (status != MH_OK)
        util::log::Error("MinHook failed to enable all hooks, MH_STATUS 0x%X", status);
    }
    else
    {
      // One by one, leaving the ones that must stay (the zone slots')
      for (auto& entry : m_CreatedHooks)
      {
        Hook& hook = entry.second;
        if (hook.Type != HookType::MinHook || hook.Keep) continue;
        MH_STATUS status = MH_DisableHook((LPVOID)hook.Address);
        if (status != MH_OK && status != MH_ERROR_DISABLED)
          util::log::Error("MinHook failed to disable hook %s, MH_STATUS 0x%X", entry.first.c_str(), status);
      }
    }

    for (auto& entry : m_CreatedHooks)
    {
      Hook& hook = entry.second;
      if (hook.Type == HookType::MinHook) continue;
      if (hook.Enabled != enable)
      {
        *hook.Original = WriteToVTable((PDWORD64*)hook.Address, *hook.Original, hook.Index);
        hook.Enabled = enable;
      }
      else
        util::log::Warning("VTable hook %s is already %s", entry.first.c_str(), enable ? "enabled" : "disabled");
    }
  }
  else
  {
    auto result = m_CreatedHooks.find(name);
    if (result == m_CreatedHooks.end())
    {
      util::log::Error("Hook %s does not exit", name.c_str());
      return;
    }

    Hook& hook = result->second;
    if (hook.Type == HookType::MinHook)
    {
      MH_STATUS status = enable ? MH_EnableHook((LPVOID)hook.Address) : MH_DisableHook((LPVOID)hook.Address);
      if (status != MH_OK)
        util::log::Error("MinHook failed to %s hook %s, MH_STATUS 0x%X", (enable ? "enable" : "disable"), name.c_str(), status);
    }
    else
    {
      if (hook.Enabled != enable)
      {
        *hook.Original = WriteToVTable((PDWORD64*)hook.Address, *hook.Original, hook.Index);
        hook.Enabled = enable;
      }
      else
        util::log::Warning("VTable hook %s is already %s", name.c_str(), enable ? "enabled" : "disabled");
    }
  }
}

bool util::hooks::CreateGameHook(char const* name, __int64 target, PVOID detour, LPVOID* original)
{
  // Its own hook's result: the list's size compared before and after counted
  // another thread's hook made meanwhile as this one (the start-up thread and
  // the drawing thread make theirs in the same seconds)
  bool timed = util::DiagnosticsOn();
  ULONGLONG from = timed ? GetTickCount64() : 0;
  bool made = CreateHook(name, target, detour, original);
  // Diagnostic: a hook that took a second or more to make, the wait for
  // MinHook's lock included
  if (timed)
  {
    ULONGLONG took = GetTickCount64() - from;
    if (took >= 1000)
      util::log::Write("Hook timing (diagnostic): %s took %.1f s to make, on thread %u", name, took / 1000.0,
        (unsigned)GetCurrentThreadId());
  }
  return made;
}
