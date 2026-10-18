#include "LightManager.h"
#include "../Main.h"
#include "../Util/Util.h"
#include "../imgui/imgui.h"
#include "../Util/ImGuiEXT.h"
#include <cstring>
#include <Psapi.h>

namespace
{
  // Module-relative addresses the original tools hardcoded (absolute in the
  // original, the exe loads at 0x140000000). They have no signatures, so the
  // light manager is only enabled for the game build they came from.
  const __int64 SphereEntityVTable = 0x1BFF178;
  const __int64 SphereDataVTable   = 0x1BFA8F8;
  const __int64 SpotEntityVTable   = 0x1BFF240;
  const __int64 SpotDataVTable     = 0x1BFA930;
  const __int64 EntityInterface    = 0x1BFF168; // Stored at entity + 0xB0
  const DWORD SupportedTimeStamp   = 0x5762E3B7;

  const int SphereEntitySize = 0x250;
  const int SpotEntitySize = 0x270;
  const int DataSize = 0x110;

  // Light data (entity + 0x28) fields, from the original tools' property commands
  const int ColorOffset             = 0x70; // float3
  const int EnlightenColorOffset    = 0x80; // float3
  const int ParticleColorOffset     = 0x90; // float3
  const int IntensityOffset         = 0xA8;
  const int AttenuationRadiusOffset = 0xAC;
  const int AttenuationOffsetOffset = 0xB0;
  const int ShadowDimmerOffset      = 0xCC;
  const int DimmerOffset            = 0xD0;
  const int DiffuseOffset           = 0xE1; // bool
  const int SpecularOffset          = 0xE2; // bool
  const int CastShadowsOffset       = 0xE3; // bool
  const int RadiosityOffset         = 0xE5; // bool
  const int DiscRadiusOffset        = 0xF0; // spot
  const int InnerAngleOffset        = 0xF4; // spot
  const int OuterAngleOffset        = 0xF8; // spot

  const int RenderViewOffset = 0x58;
  const int RenderViewTransform = 0x40;

  const float Identity[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };

  template <typename T>
  T& At(BYTE* p, int offset) { return *(T*)(p + offset); }

  // The game's light update, false if it faulted (a light whose level is gone)
  bool SafeUpdateLight(__int64 pUpdateLight, BYTE* pEntity)
  {
    typedef void(__fastcall* tUpdateLight)(void* pEntity);
    __try
    {
      ((tUpdateLight)pUpdateLight)(pEntity);
      return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      return false;
    }
  }
}

LightManager::LightManager() :
  m_NextId(1),
  m_Selected(-1),
  m_Supported(false)
{
  m_pUpdateLight = util::offsets::GetOffset("OFFSET_UPDATELIGHT");
  m_ppGameRenderer = util::offsets::GetOffset("OFFSET_GAMERENDERER");
  m_Supported = CheckSupport();

  if (m_Supported)
    util::log::Ok("Light manager available");
  else
    util::log::Warning("Light manager disabled: %s", m_UnsupportedReason.c_str());
}

LightManager::~LightManager()
{
  // Light memory is never freed, the game may still point at it
}

bool LightManager::CheckSupport()
{
  if (!m_pUpdateLight || !m_ppGameRenderer)
  {
    m_UnsupportedReason = "OFFSET_UPDATELIGHT or OFFSET_GAMERENDERER wasn't found";
    return false;
  }

  IMAGE_DOS_HEADER* pDos = (IMAGE_DOS_HEADER*)g_gameHandle;
  IMAGE_NT_HEADERS* pNt = (IMAGE_NT_HEADERS*)((BYTE*)g_gameHandle + pDos->e_lfanew);
  if (pNt->FileHeader.TimeDateStamp != SupportedTimeStamp)
  {
    m_UnsupportedReason = "the game build isn't the one the original tools' light addresses are for";
    return false;
  }

  // The vtables must point at code inside the exe
  MODULEINFO info;
  if (!GetModuleInformation(GetCurrentProcess(), g_gameHandle, &info, sizeof(MODULEINFO)))
  {
    m_UnsupportedReason = "GetModuleInformation failed";
    return false;
  }

  __int64 start = (__int64)info.lpBaseOfDll;
  __int64 end = start + info.SizeOfImage;
  for (__int64 rva : { SphereEntityVTable, SphereDataVTable, SpotEntityVTable, SpotDataVTable, EntityInterface })
  {
    __int64* pVTable = (__int64*)(start + rva);
    for (int i = 0; i < 4; ++i)
    {
      if (pVTable[i] < start || pVTable[i] >= end)
      {
        m_UnsupportedReason = "a light vtable doesn't look right";
        return false;
      }
    }
  }

  return true;
}

bool LightManager::GetCameraTransform(float transform[16])
{
  __int64 pGameRenderer = *(__int64*)m_ppGameRenderer;
  if (!pGameRenderer) return false;
  __int64 pRenderView = *(__int64*)(pGameRenderer + RenderViewOffset);
  if (!pRenderView) return false;

  memcpy(transform, (void*)(pRenderView + RenderViewTransform), 64);
  return true;
}

void LightManager::CreateLight(LightType type)
{
  // Same layout and defaults as the original tools' constructors
  // (sub_18000a8e0 / sub_18000abc0 in CT_MirrorsEdge.dll)
  __int64 base = (__int64)g_gameHandle;
  int entitySize = (type == Spot) ? SpotEntitySize : SphereEntitySize;

  BYTE* e = new BYTE[entitySize]();
  BYTE* d = new BYTE[DataSize]();

  // Light data
  At<__int64>(d, 0x00) = base + ((type == Spot) ? SpotDataVTable : SphereDataVTable);
  memcpy(d + 0x20, Identity, 64);
  At<BYTE>(d, 0x60) = 1;
  for (int offset : { ColorOffset, EnlightenColorOffset, ParticleColorOffset })
  {
    At<float>(d, offset + 0) = 1.f;
    At<float>(d, offset + 4) = 1.f;
    At<float>(d, offset + 8) = 1.f;
  }
  At<int>(d, 0xA4) = 4000;
  At<float>(d, IntensityOffset) = 10.f;
  At<float>(d, AttenuationRadiusOffset) = 10.f;
  At<float>(d, 0xC4) = 20.f;
  At<float>(d, 0xC8) = 18.f;
  At<float>(d, ShadowDimmerOffset) = 1.f;
  At<float>(d, DimmerOffset) = 1.f;
  At<BYTE>(d, DiffuseOffset) = 1;
  At<BYTE>(d, SpecularOffset) = 1;
  At<BYTE>(d, RadiosityOffset) = 1;
  if (type == Spot)
  {
    At<float>(d, OuterAngleOffset) = 80.f;
    At<float>(d, 0x108) = 1.f;
    At<BYTE>(d, 0x10C) = 1;
  }
  else
    At<float>(d, 0x100) = 1.f;

  // Entity
  At<__int64>(e, 0x00) = base + ((type == Spot) ? SpotEntityVTable : SphereEntityVTable);
  At<WORD>(e, 0x18) = 0x121E;
  At<BYTE*>(e, 0x28) = d;
  memcpy(e + 0x40, Identity, 64);
  At<__int64>(e, 0x80) = 0xFFFF;
  At<WORD>(e, 0xA0) = 0x0101;
  At<BYTE>(e, 0xA4) = 1;
  At<float>(e, 0xA8) = 1.f;
  At<__int64>(e, 0xB0) = base + EntityInterface;

  // Property table, pointers into the light data
  const int table[][2] = {
    { 0xC0, 0x70 }, { 0xD0, 0xA8 }, { 0xE0, 0xAC }, { 0xF0, 0xCC }, { 0x100, 0xE1 }, { 0x110, 0xE2 },
    { 0x120, 0xE3 }, { 0x130, 0xE4 }, { 0x140, 0xE5 }, { 0x150, 0x90 }, { 0x160, 0xD0 }, { 0x170, 0x80 },
    { 0x180, 0x20 }, { 0x190, 0xD4 }, { 0x1A0, 0xD8 }, { 0x218, 0xF0 } };
  for (auto const& entry : table)
    At<BYTE*>(e, entry[0]) = d + entry[1];

  if (type == Spot)
  {
    At<BYTE*>(e, 0x228) = d + 0xF4;
    At<BYTE*>(e, 0x238) = d + 0xF8;
    At<BYTE*>(e, 0x248) = d + 0x108;
    At<BYTE>(e, 0x200) = 1;
  }
  else
    At<BYTE*>(e, 0x228) = d + 0x100;

  At<WORD>(e, 0x206) = 0x3F80;
  At<BYTE>(e, 0x20C) = 3;

  Light light{ m_NextId++, type, e, d, false };

  // Put it where the camera is
  float transform[16];
  if (GetCameraTransform(transform))
  {
    memcpy(e + 0x40, transform, 64);
    memcpy(d + 0x20, transform, 64);
  }

  m_Lights.push_back(light);
  m_Selected = (int)m_Lights.size() - 1;
  QueueUpdate(m_Lights.back());

  util::log::Write("Created %s light %d", type == Spot ? "spot" : "sphere", light.Id);
}

void LightManager::RemoveLight(Light& light)
{
  // The original cleared these flags and updated the light once more
  At<DWORD>(light.pEntity, 0xA0) = 0;
  light.Removed = true;
  QueueUpdate(light);
  util::log::Write("Removed light %d", light.Id);
}

void LightManager::MoveToCamera(Light& light)
{
  float transform[16];
  if (!GetCameraTransform(transform))
    return;

  memcpy(light.pEntity + 0x40, transform, 64);
  memcpy(light.pData + 0x20, transform, 64);
  QueueUpdate(light);
}

void LightManager::QueueUpdate(Light& light)
{
  for (BYTE* p : m_PendingUpdates)
    if (p == light.pEntity) return;
  m_PendingUpdates.push_back(light.pEntity);
  m_HasPending = true;
}

void LightManager::OnGameUpdate()
{
  // Called for every render view, so skip the lock when there's nothing to do
  if (!m_Supported || !m_HasPending) return;

  std::lock_guard<std::mutex> lock(m_Mutex);
  m_HasPending = false;
  if (m_PendingUpdates.empty()) return;

  for (BYTE* pEntity : m_PendingUpdates)
  {
    if (!SafeUpdateLight(m_pUpdateLight, pEntity))
    {
      // Most likely a light from before a level change. Stop touching it.
      util::log::Warning("The game couldn't update a light, it was taken off the list");
      for (Light& light : m_Lights)
        if (light.pEntity == pEntity)
          light.Removed = true;
    }
  }
  m_PendingUpdates.clear();

  // Removed lights stay allocated but leave the list once the game has seen them
  for (int i = 0; i < (int)m_Lights.size();)
  {
    if (m_Lights[i].Removed)
    {
      m_Lights.erase(m_Lights.begin() + i);
      // Keep the same light selected
      if (i < m_Selected)
        m_Selected--;
      else if (i == m_Selected)
        m_Selected = -1;
    }
    else
      ++i;
  }
}

void LightManager::DrawUI()
{
  std::lock_guard<std::mutex> lock(m_Mutex);

  if (!m_Supported)
  {
    ImGui::PushTextWrapPos(0.f);
    ImGui::TextDisabled("The light manager is turned off: %s.", m_UnsupportedReason.c_str());
    ImGui::PopTextWrapPos();
    return;
  }

  float fontSize = ImGui::GetFontSize();

  ImGui::PushTextWrapPos(0.f);
  ImGui::TextDisabled("New lights appear where the camera is. Pick a light in the list to change it, "
    "changes show up straight away.");
  ImGui::PopTextWrapPos();

  if (ImGui::Button("New sphere light"))
    CreateLight(Sphere);
  ImGui::ItemTooltip("Shines in every direction, like a bulb.");
  ImGui::SameLine();
  if (ImGui::Button("New spot light"))
    CreateLight(Spot);
  ImGui::ItemTooltip("A cone pointing where the camera looks, like a torch.");

  int lightCount = 0;
  for (Light const& light : m_Lights)
    lightCount += light.Removed ? 0 : 1;

  if (lightCount == 0)
  {
    ImGui::TextDisabled("No lights yet.");
    return;
  }

  ImGui::SameLine();
  if (ImGui::ConfirmButton("Forget all lights"))
  {
    // Only drops them from the list, nothing is sent to the game. Use it
    // after a level change, when the old lights no longer exist there.
    m_Lights.clear();
    m_PendingUpdates.clear();
    m_Selected = -1;
    return;
  }
  ImGui::ItemTooltip("Use this after loading another level or checkpoint: the old lights are gone from "
    "the game, and changing them could crash it.");

  float listHeight = ImGui::GetTextLineHeightWithSpacing() * (lightCount < 5 ? lightCount + 0.5f : 5.5f);
  ImGui::BeginChild("##LightList", ImVec2(0, listHeight), true);
  for (int i = 0; i < (int)m_Lights.size(); ++i)
  {
    Light& light = m_Lights[i];
    if (light.Removed) continue;
    char label[64];
    sprintf_s(label, "%s light %d", light.Type == Spot ? "Spot" : "Sphere", light.Id);
    if (ImGui::Selectable(label, m_Selected == i))
      m_Selected = i;
  }
  ImGui::EndChild();

  if (m_Selected < 0 || m_Selected >= (int)m_Lights.size() || m_Lights[m_Selected].Removed)
  {
    ImGui::TextDisabled("Pick a light in the list to change it.");
    return;
  }

  Light& light = m_Lights[m_Selected];
  BYTE* d = light.pData;
  bool changed = false;

  if (ImGui::Button("Move to camera"))
    MoveToCamera(light);
  ImGui::ItemTooltip("Puts the light where the camera is, pointing the way it looks.");
  ImGui::SameLine();
  if (ImGui::Button("Remove light"))
  {
    RemoveLight(light);
    return;
  }

  ImGui::PushItemWidth(fontSize * 15.f);
  ImGuiColorEditFlags colorFlags = ImGuiColorEditFlags_HDR | ImGuiColorEditFlags_Float;

  ImGui::SubHeading("Light");
  changed |= ImGui::ColorEdit3("Color", &At<float>(d, ColorOffset), colorFlags);
  changed |= ImGui::DragFloat("Intensity", &At<float>(d, IntensityOffset), 0.1f, 0.f, 100000.f, "%.2f");
  changed |= ImGui::DragFloat("Radius", &At<float>(d, AttenuationRadiusOffset), 0.05f, 0.f, 10000.f, "%.2f m");
  ImGui::HelpMarker("How far the light reaches.");
  changed |= ImGui::DragFloat("Brightness multiplier", &At<float>(d, DimmerOffset), 0.01f, 0.f, 100.f, "%.3f");
  ImGui::HelpMarker("The game's \"dimmer\": multiplies the intensity.");

  if (light.Type == Spot)
  {
    changed |= ImGui::DragFloat("Inner angle", &At<float>(d, InnerAngleOffset), 0.25f, 0.f, 180.f, "%.1f" UI_DEG);
    ImGui::HelpMarker("Full brightness inside this angle, fading out to the outer angle.");
    changed |= ImGui::DragFloat("Outer angle", &At<float>(d, OuterAngleOffset), 0.25f, 0.f, 180.f, "%.1f" UI_DEG);
    changed |= ImGui::DragFloat("Source size", &At<float>(d, DiscRadiusOffset), 0.01f, 0.f, 100.f, "%.3f m");
    ImGui::HelpMarker("The size of the light's disc. Bigger gives softer light.");
  }

  ImGui::SubHeading("Lights up");
  changed |= ImGui::Checkbox("Diffuse", (bool*)(d + DiffuseOffset));
  ImGui::SameLine();
  changed |= ImGui::Checkbox("Specular", (bool*)(d + SpecularOffset));
  ImGui::SameLine();
  changed |= ImGui::Checkbox("Bounce light", (bool*)(d + RadiosityOffset));
  ImGui::SameLine();
  changed |= ImGui::Checkbox("Shadows", (bool*)(d + CastShadowsOffset));
  ImGui::HelpMarker("Diffuse: surfaces. Specular: shine and reflections. Bounce light: the game's "
    "radiosity (Enlighten). Shadows: the light casts shadows, which costs performance.");

  if (ImGui::TreeNode("More settings"))
  {
    changed |= ImGui::DragFloat("Fade start", &At<float>(d, AttenuationOffsetOffset), 0.01f, 0.f, 1000.f, "%.3f");
    ImGui::HelpMarker("The game's attenuation offset.");
    changed |= ImGui::DragFloat("Shadow strength", &At<float>(d, ShadowDimmerOffset), 0.01f, 0.f, 100.f, "%.3f");
    changed |= ImGui::ColorEdit3("Bounce light color", &At<float>(d, EnlightenColorOffset), colorFlags);
    changed |= ImGui::ColorEdit3("Particle color", &At<float>(d, ParticleColorOffset), colorFlags);
    ImGui::HelpMarker("The color the light gives smoke and other particles.");
    ImGui::TreePop();
  }
  ImGui::PopItemWidth();

  if (changed)
  {
    // The original never let a color go fully black
    for (int offset : { ColorOffset, EnlightenColorOffset, ParticleColorOffset })
    {
      float* c = &At<float>(d, offset);
      if (c[0] <= 0 && c[1] <= 0 && c[2] <= 0)
        c[0] = c[1] = c[2] = 0.0001f;
    }
    QueueUpdate(light);
  }
}
