#include "VisualsManager.h"
#include "../Main.h"
#include "../Util/Util.h"
#include "../imgui/imgui.h"
#include "../Util/ImGuiEXT.h"
#include <cstdio>
#include <cstring>

namespace
{
  // MAIN + 0x50 -> +0x90 -> +0x1C0 -> +0x30 is the visual environment state
  // manager. Its current state starts at +0x85470, the DoF method lives in
  // the render settings at +0x138.
  const int StateOffset = 0x85470;
  const int RenderSettingsOffset = 0x138;
  const int DofMethodOffset = 0x154;   // int, in the render settings
  const int SpriteDofOffset = 0x247;   // bool, true when the DoF method is 1

  __int64 ReadPointer(__int64 address)
  {
    if (!address) return 0;
    return *(__int64*)address;
  }

  typedef VisualsManager::FieldType T;

  int FieldSize(T type)
  {
    switch (type)
    {
    case T::Bool:   return 1;
    case T::Float2: return 8;
    case T::Float3:
    case T::Color3: return 12;
    default:        return 4;
    }
  }
}

VisualsManager::VisualsManager() :
  m_DofMethodHasValue(false),
  m_DofMethod(0),
  m_SpriteDofFix(false),
  m_SpriteDofOriginalBytes{ 0, 0 }
{
  m_ppMain = util::offsets::GetOffset("OFFSET_MAIN");
  m_ppClientGameContext = util::offsets::GetOffset("OFFSET_CLIENTGAMECONTEXT");
  m_pSpriteDofFix = util::offsets::GetOffset("OFFSET_DOFFIX");
  m_ppTimeOfDay = util::offsets::GetOffset("OFFSET_TIMEOFDAY");

  // Offsets are relative to each component, taken from the original GUI's
  // Read/Write methods. Component offsets are the GUI's shared memory
  // offsets minus 0x10, which is where they sit in the game's state.
  m_Components.push_back({ "Color correction", 0x330, 0x50, {
    { "Enable", 0x44, T::Bool, 0 },
    { "Brightness", 0x00, T::Float3, 0.005f },
    { "Contrast", 0x10, T::Float3, 0.005f },
    { "Saturation", 0x20, T::Float3, 0.005f },
    { "Hue", 0x34, T::Float, 0.5f },
    { "Color grading", 0x45, T::Bool, 0 },
    { "Color grading max HDR", 0x40, T::Float, 0.05f },
  } });

  m_Components.push_back({ "Depth of field", 0xA50, 0x60, {
    { "Enable", 0x58, T::Bool, 0 },
    { "Focus distance", 0x04, T::Float, 0.05f },
    { "Blur factor", 0x08, T::Float, 0.01f },
    { "Blur add", 0x0C, T::Float, 0.01f },
    { "Simple blur filter", 0x10, T::Int, 0 },
    { "Simple max blur", 0x14, T::Float, 0.01f },
    { "Simple near start", 0x18, T::Float, 0.05f },
    { "Simple near end", 0x1C, T::Float, 0.05f },
    { "Simple far start", 0x20, T::Float, 0.05f },
    { "Simple far end", 0x24, T::Float, 0.05f },
    { "Sprite near start", 0x38, T::Float, 0.05f },
    { "Sprite near end", 0x3C, T::Float, 0.05f },
    { "Sprite far start", 0x40, T::Float, 0.05f },
    { "Sprite far end", 0x44, T::Float, 0.05f },
    { "Sprite max blur", 0x48, T::Float, 0.01f },
    { "Hip to ironsights fade", 0x28, T::Float, 0.01f },
    { "Ironsights DoF start fade", 0x2C, T::Float, 0.01f },
    { "Ironsights focal distance", 0x30, T::Float, 0.05f },
    { "Ironsights circle distance", 0x34, T::Float, 0.05f },
    { "Ironsights DoF active", 0x5B, T::Bool, 0 },
    { "Ironsights circle blur", 0x5C, T::Bool, 0 },
  }, 15 });

  m_Components.push_back({ "Film grain", 0xB00, 0x30, {
    { "Enable", 0x28, T::Bool, 0 },
    { "Color scale", 0x00, T::Float3, 0.005f },
    { "Texture scale", 0x14, T::Float2, 0.01f },
    { "Linear filtering", 0x29, T::Bool, 0 },
    { "Random", 0x2A, T::Bool, 0 },
  } });

  m_Components.push_back({ "Outdoor light", 0x000, 0x120, {
    { "Enable", 0x118, T::Bool, 0 },
    { "Sun color", 0x00, T::Color3, 0 },
    { "Sun intensity", 0x94, T::Float, 0.05f },
    { "Sun rotation X", 0x84, T::Float, 0.5f },
    { "Sun rotation Y", 0x88, T::Float, 0.5f },
    // Frostbite's ShadowSunRotationEnable: on = shadows use their own direction
    // (the two "Shadow direction" values below), off = they follow the sun
    { "Separate shadow direction", 0x119, T::Bool, 0 },
    { "Sun angular radius", 0x98, T::Float, 0.005f },
    { "Sun disk intensity", 0x9C, T::Float, 0.05f },
    { "Sun specular scale", 0xA4, T::Float, 0.01f },
    { "Sun penumbra size", 0xC4, T::Float, 0.01f },
    { "Sky color", 0x60, T::Color3, 0 },
    { "Ground color", 0x70, T::Color3, 0 },
    { "Sky light angle factor", 0xA0, T::Float, 0.01f },
    // From the game's OutdoorLightComponentData (data offset - 0x80 = state offset)
    { "Shadow softness (PCSS)", 0xC8, T::Float, 0.01f },
    { "Shadow direction X", 0x8C, T::Float, 0.5f },
    { "Shadow direction Y", 0x90, T::Float, 0.5f },
    { "Sun shadow height scale", 0xAC, T::Float, 0.01f },
    { "Sky reflection shadow scale", 0xA8, T::Float, 0.01f },
    { "Final sun luminance", 0x10, T::Float3, 0.05f },
    { "Final sun illuminance", 0x20, T::Float3, 0.05f },
    { "Outer space sun luminance", 0x30, T::Float3, 0.05f },
    { "Outer space sun illuminance 1", 0x40, T::Float3, 0.05f },
    { "Outer space sun illuminance 2", 0x50, T::Float3, 0.05f },
    { "Translucency ambient", 0x100, T::Float, 0.01f },
    { "Translucency scale", 0x104, T::Float, 0.01f },
    { "Translucency power", 0x108, T::Float, 0.01f },
    { "Translucency distortion", 0x10C, T::Float, 0.01f },
    { "Particle sun shadow factor", 0x110, T::Float, 0.01f },
    { "Particle sun shadow smoothing", 0x114, T::Float, 0.01f },
    { "Sun PCSS filter adaptive", 0x11A, T::Bool, 0 },
    { "Cloud shadows", 0x11B, T::Bool, 0 },
    { "Cloud shadows top-down", 0x11C, T::Bool, 0 },
  }, 14 }); // Through "Shadow softness"

  m_Components.push_back({ "Tonemap", 0x1D0, 0x150, {
    { "Tonemap method", 0xC0, T::Int, 0 },
    { "Exposure compensation", 0x98, T::Float, 0.01f },
    { "Automatic exposure", 0x148, T::Bool, 0 },
    { "EV", 0x94, T::Float, 0.01f },
    { "Clamp EV", 0x149, T::Bool, 0 },
    { "Min EV", 0xB4, T::Float, 0.01f },
    { "Max EV", 0xB8, T::Float, 0.01f },
    { "Bloom scale", 0x00, T::Float3, 0.005f },
    { "Auto exposure darkest exclude", 0x9C, T::Float, 0.005f },
    { "Auto exposure brightest exclude", 0xA0, T::Float, 0.005f },
    { "Auto exposure higher threshold", 0xAC, T::Float, 0.01f },
    { "Auto exposure lower threshold", 0xB0, T::Float, 0.01f },
    { "Dark adaptation time", 0xA4, T::Float, 0.01f },
    { "Light adaptation time", 0xA8, T::Float, 0.01f },
    { "Spot meter scale", 0xBC, T::Float, 0.01f },
    { "Black point", 0xC4, T::Float, 0.005f },
    { "White point", 0xC8, T::Float, 0.005f },
    { "Cross over point", 0xCC, T::Float, 0.005f },
    { "Toe strength", 0xD0, T::Float, 0.005f },
    { "Shoulder strength", 0xD4, T::Float, 0.005f },
    { "Split screen", 0xD8, T::Float, 0.005f },
    { "Black in", 0xDC, T::Float, 0.005f },
    { "White in", 0xE0, T::Float, 0.005f },
    { "Black out", 0xE4, T::Float, 0.005f },
    { "White out", 0xE8, T::Float, 0.005f },
    { "White level", 0xEC, T::Float, 0.005f },
    { "White clip", 0xF0, T::Float, 0.005f },
    { "Bloom method", 0xF4, T::Int, 0 },
    { "Bloom direction", 0xF8, T::Int, 0 },
    { "Gaussian clamp", 0xFC, T::Float, 0.01f },
    { "Gaussian sharpness", 0x100, T::Float, 0.01f },
    { "FFT threshold", 0x118, T::Float, 0.01f },
    { "FFT cutoff", 0x11C, T::Float, 0.01f },
    { "FFT kernel scale", 0x120, T::Float, 0.01f },
    { "FFT kernel rotation", 0x124, T::Float, 0.5f },
    { "FFT spike scale limit", 0x128, T::Float, 0.01f },
    { "FFT spike scale limit enable", 0x14A, T::Bool, 0 },
    { "Chromostereopsis", 0x14B, T::Bool, 0 },
    { "Chromostereopsis scale", 0x138, T::Float, 0.01f },
    { "Chromostereopsis offset", 0x13C, T::Float, 0.01f },
  }, 8 });

  m_Components.push_back({ "Vignette", 0xAC0, 0x40, {
    { "Enable", 0x24, T::Bool, 0 },
    { "Color", 0x00, T::Color3, 0 },
    { "Scale", 0x14, T::Float2, 0.01f },
    { "Exponent", 0x1C, T::Float, 0.01f },
    { "Opacity", 0x20, T::Float, 0.005f },
  } });

  for (Component& component : m_Components)
    component.Values.resize(component.Size, 0);
}

VisualsManager::~VisualsManager()
{

}

__int64 VisualsManager::GetStateManager()
{
  __int64 p = ReadPointer(m_ppMain);
  if (!p) return 0;
  p = *(__int64*)(p + 0x50);
  if (!p) return 0;
  p = *(__int64*)(p + 0x90);
  if (!p) return 0;
  p = *(__int64*)(p + 0x1C0);
  if (!p) return 0;
  return *(__int64*)(p + 0x30);
}

bool VisualsManager::IsLevelLoaded()
{
  // The original tools only touched the visual state once the player
  // existed (ClientGameContext + 0x68 -> +0x568 -> +0x14A0)
  __int64 p = ReadPointer(m_ppClientGameContext);
  if (!p) return false;
  p = *(__int64*)(p + 0x68);
  if (!p) return false;
  p = *(__int64*)(p + 0x568);
  if (!p) return false;
  return *(__int64*)(p + 0x14A0) != 0;
}

float* VisualsManager::GetTimeOfDay()
{
  // [[[OFFSET_TIMEOFDAY] + 0x8] + 0x28] + 0x30, the path MEC-TC uses
  __int64 p = ReadPointer(m_ppTimeOfDay);
  if (!p) return nullptr;
  p = *(__int64*)(p + 0x8);
  if (!p) return nullptr;
  p = *(__int64*)(p + 0x28);
  if (!p) return nullptr;
  return (float*)(p + 0x30);
}

void VisualsManager::UpdateTimeOfDay()
{
  if (!m_TodFreeze && !m_TodFix && !m_TrackTodActive) return;
  // With MECMP in the game the clock is left as the game has it
  if (util::IsMecmpLoaded()) return;
  if (!IsLevelLoaded()) return;

  float* pTime = GetTimeOfDay();
  if (!pTime) return;

  if (m_TrackTodActive)
    *pTime = m_TrackTod;
  else if (m_TodFreeze)
    *pTime = m_TodFrozenValue;
  else if (m_TodFix && (int)*pTime == m_TodFixAt)
    *pTime = (float)(m_TodFixAt + m_TodFixSkip);
}

float VisualsManager::GetCurrentTimeOfDay()
{
  std::lock_guard<std::mutex> lock(m_Mutex);
  float* pTime = IsLevelLoaded() ? GetTimeOfDay() : nullptr;
  return pTime ? *pTime : -1.f;
}

void VisualsManager::SetTimeOfDay(float seconds)
{
  std::lock_guard<std::mutex> lock(m_Mutex);
  if (util::IsMecmpLoaded()) return;
  float* pTime = IsLevelLoaded() ? GetTimeOfDay() : nullptr;
  if (!pTime || seconds < 0) return;

  *pTime = seconds;
  m_TodFrozenValue = seconds;
}

void VisualsManager::SetTrackTimeOfDay(float seconds)
{
  std::lock_guard<std::mutex> lock(m_Mutex);
  if (util::IsMecmpLoaded()) return;
  m_TrackTod = seconds;
  m_TrackTodActive = true;

  // Also right away, the visual environment update may come before or after
  float* pTime = IsLevelLoaded() ? GetTimeOfDay() : nullptr;
  if (pTime)
    *pTime = seconds;
}

void VisualsManager::KeepClock()
{
  std::lock_guard<std::mutex> lock(m_Mutex);
  UpdateTimeOfDay();
}

void VisualsManager::StopTrackTimeOfDay()
{
  std::lock_guard<std::mutex> lock(m_Mutex);
  if (!m_TrackTodActive) return;

  // The clock goes on from where the track left it, or stays there when stopped
  m_TrackTodActive = false;
  m_TodFrozenValue = m_TrackTod;
}

void VisualsManager::DrawTimeOfDayUI()
{
  std::lock_guard<std::mutex> lock(m_Mutex);

  if (util::IsMecmpLoaded())
  {
    ImGui::PushTextWrapPos(0.f);
    ImGui::TextDisabled("Time of day: MECMP (multiplayer) is open, so these controls are disabled "
      "because MECMP syncs the time of day.");
    ImGui::PopTextWrapPos();
    return;
  }
  float* pTime = IsLevelLoaded() ? GetTimeOfDay() : nullptr;
  if (!pTime)
  {
    ImGui::TextDisabled("Time of day: load a level to change it");
    return;
  }

  float time = (m_TodFreeze && !m_TrackTodActive) ? m_TodFrozenValue : *pTime;
  int seconds = (int)time;
  char label[32];
  sprintf_s(label, "%02d:%02d", (seconds / 3600) % 24, (seconds / 60) % 60);

  if (ImGui::SliderFloat("Time of day", &time, 0.f, 86399.f, label))
  {
    *pTime = time;
    m_TodFrozenValue = time;
  }
  ImGui::HelpMarker("Midnight on the left, noon in the middle. Ctrl+click to type the time in seconds (0 - 86400).");

  // Steps of 10 minutes, round the clock past midnight
  float step = 0.f;
  if (ImGui::Button("- 10 min"))
    step = -600.f;
  ImGui::SameLine();
  if (ImGui::Button("+ 10 min"))
    step = 600.f;
  if (step != 0.f)
  {
    time = fmodf(time + step + 86400.f, 86400.f);
    *pTime = time;
    m_TodFrozenValue = time;
  }

  if (m_TrackTodActive)
    ImGui::TextDisabled("The camera track is setting the time of day");

  if (ImGui::Checkbox("Stop the clock", &m_TodFreeze) && m_TodFreeze)
    m_TodFrozenValue = *pTime;
  ImGui::HelpMarker("Keeps the time of day where it is. Freeze time stops everything else too.");

  if (ImGui::TreeNode("Stuck clock fix"))
  {
    ImGui::Checkbox("Skip the stuck time", &m_TodFix);
    ImGui::HelpMarker("The game's clock can get stuck at one time. When it's at this time, it's moved "
      "ahead (the same fix as Mirror's Edge Catalyst Time Changer).");
    if (m_TodFix)
    {
      float fontSize = ImGui::GetFontSize();
      ImGui::PushItemWidth(fontSize * 6.f);
      ImGui::InputInt("Stuck at (seconds)", &m_TodFixAt, 0, 0);
      ImGui::InputInt("Move ahead by (seconds)", &m_TodFixSkip, 0, 0);
      ImGui::PopItemWidth();
      if (m_TodFixSkip < 1) m_TodFixSkip = 1;
    }
    ImGui::TreePop();
  }
}

void VisualsManager::OnVisualEnvironmentUpdate(bool beforeGameUpdate)
{
  std::lock_guard<std::mutex> lock(m_Mutex);

  // Before and after the game's update, so the clock the game moves on during
  // its update is put back straight away
  UpdateTimeOfDay();

  Component const& dof = m_Components[1];
  bool anyOverride = m_DofMethodApplied;
  for (Component const& component : m_Components)
    anyOverride |= component.Override && component.HasValues;
  if (!anyOverride || !IsLevelLoaded())
    return;

  __int64 pManager = GetStateManager();
  if (!pManager) return;
  __int64 pState = pManager + StateOffset;

  for (Component& component : m_Components)
  {
    if (!component.Override || !component.HasValues) continue;

    // Self-check: what we wrote after the last update should still be there.
    // If not, something other than the visual environment update changes it.
    if (beforeGameUpdate)
      CheckStillApplied(pState, component);

    WriteToGame(pState, component);
  }

  // DoF method, same as the original tools. It isn't part of the state the
  // game recomputes, so the game's own value is put back afterwards.
  __int64 pRenderSettings = *(__int64*)(pManager + RenderSettingsOffset);
  if (pRenderSettings)
  {
    if (dof.Override && m_DofMethodHasValue)
    {
      *(BYTE*)(pRenderSettings + SpriteDofOffset) = (m_DofMethod == 1);
      *(int*)(pRenderSettings + DofMethodOffset) = m_DofMethod;
      m_DofMethodApplied = true;
    }
    else if (m_DofMethodApplied)
    {
      *(BYTE*)(pRenderSettings + SpriteDofOffset) = (m_DofMethodOriginal == 1);
      *(int*)(pRenderSettings + DofMethodOffset) = m_DofMethodOriginal;
      m_DofMethodApplied = false;
    }
  }
}

void VisualsManager::CheckStillApplied(__int64 pState, Component& component)
{
  ULONGLONG now = GetTickCount64();
  if (now - component.EnabledTick < 1000)
    return;

  BYTE* pCurrent = (BYTE*)(pState + component.StateOffset);
  for (Field const& field : component.Fields)
  {
    if (memcmp(pCurrent + field.Offset, component.Values.data() + field.Offset, FieldSize(field.Type)) != 0)
    {
      component.OverwrittenTick = now;
      component.OverwrittenField = field.Name;
      return;
    }
  }
}

void VisualsManager::ReadFromGame(Component& component)
{
  __int64 pManager = GetStateManager();
  if (!pManager || !IsLevelLoaded())
  {
    util::log::Warning("Visual environment not available, is a level loaded?");
    return;
  }

  memcpy(component.Values.data(), (void*)(pManager + StateOffset + component.StateOffset), component.Size);
  component.HasValues = true;

  if (&component == &m_Components[1])
  {
    __int64 pRenderSettings = *(__int64*)(pManager + RenderSettingsOffset);
    if (pRenderSettings)
    {
      m_DofMethod = *(int*)(pRenderSettings + DofMethodOffset);
      if (!m_DofMethodApplied)
        m_DofMethodOriginal = m_DofMethod;
      m_DofMethodHasValue = true;
    }
  }
}

void VisualsManager::WriteToGame(__int64 pState, Component const& component)
{
  // Only the known fields are written, so texture pointers and anything
  // else in between always stay the game's own
  BYTE* pTarget = (BYTE*)(pState + component.StateOffset);
  for (Field const& field : component.Fields)
    memcpy(pTarget + field.Offset, component.Values.data() + field.Offset, FieldSize(field.Type));
}

void VisualsManager::DrawField(Component& component, Field const& field)
{
  void* pValue = component.Values.data() + field.Offset;
  switch (field.Type)
  {
  case T::Bool:
    ImGui::Checkbox(field.Name, (bool*)pValue);
    break;
  case T::Int:
    ImGui::InputInt(field.Name, (int*)pValue);
    break;
  case T::Float:
    ImGui::DragFloat(field.Name, (float*)pValue, field.Speed, 0, 0, "%.3f");
    break;
  case T::Float2:
    ImGui::DragFloat2(field.Name, (float*)pValue, field.Speed, 0, 0, "%.3f");
    break;
  case T::Float3:
    ImGui::DragFloat3(field.Name, (float*)pValue, field.Speed, 0, 0, "%.3f");
    break;
  case T::Color3:
    ImGui::ColorEdit3(field.Name, (float*)pValue, ImGuiColorEditFlags_HDR | ImGuiColorEditFlags_Float);
    break;
  }
}

void VisualsManager::DrawUI()
{
  std::lock_guard<std::mutex> lock(m_Mutex);

  float fontSize = ImGui::GetFontSize();

  ImGui::PushTextWrapPos(0.f);
  ImGui::TextDisabled("Open an effect and tick \"Change this effect\". Its current values are copied "
    "from the game, then what you set here is used every frame until you untick it. Drag a value "
    "left or right to change it, or Ctrl+click it to type a number.");
  ImGui::PopTextWrapPos();

  int overrides = 0;
  for (Component const& component : m_Components)
    overrides += component.Override ? 1 : 0;
  if (overrides > 0)
  {
    char label[64];
    sprintf_s(label, "Stop all changes (%d)", overrides);
    if (ImGui::Button(label))
    {
      for (Component& component : m_Components)
        component.Override = false;
    }
  }

  for (Component& component : m_Components)
  {
    ImGui::PushID(component.Name);
    char header[96];
    sprintf_s(header, "%s%s###Header", component.Name, component.Override ? "    [changed]" : "");
    if (ImGui::CollapsingHeader(header))
    {
      bool wasOverride = component.Override;
      ImGui::Checkbox("Change this effect", &component.Override);
      if (component.Override && !wasOverride)
      {
        component.EnabledTick = GetTickCount64();
        if (!component.HasValues)
          ReadFromGame(component);
      }

      ImGui::SameLine();
      if (ImGui::Button("Reset to game values") && !component.ResetTick)
      {
        // While changed, the state holds our values. Pause the change so the
        // game puts its own back, then read them a moment later (below).
        if (component.Override)
        {
          component.Override = false;
          component.ResetTick = GetTickCount64();
        }
        else
          ReadFromGame(component);
      }
      ImGui::ItemTooltip("Copies the game's own values for this effect again.");

      if (component.ResetTick && GetTickCount64() - component.ResetTick > 300)
      {
        ReadFromGame(component);
        component.ResetTick = 0;
        component.Override = true;
        component.EnabledTick = GetTickCount64();
      }

      if (component.Override && component.HasValues)
      {
        if (GetTickCount64() - component.OverwrittenTick < 2000)
          ImGui::TextColored(ImVec4(1.f, 0.8f, 0.2f, 1.f), "The game keeps changing \"%s\", this may flicker or not show",
            component.OverwrittenField ? component.OverwrittenField : "?");
        else
          ImGui::TextColored(ImVec4(0.45f, 0.9f, 0.45f, 1.f), "Used every frame");
      }

      if (!component.HasValues)
        ImGui::TextDisabled("No values yet. Load a level, then tick \"Change this effect\".");
      else
      {
        ImGui::PushItemWidth(fontSize * 15.f);
        size_t basicCount = component.BasicFieldCount > 0 ? (size_t)component.BasicFieldCount : component.Fields.size();
        for (size_t i = 0; i < basicCount && i < component.Fields.size(); ++i)
          DrawField(component, component.Fields[i]);

        if (&component == &m_Components[1])
        {
          if (m_DofMethodHasValue)
          {
            const char* methods[] = { "Simple", "Sprite (bokeh)", "Method 2" };
            int method = m_DofMethod;
            if (method < 0 || method > 2) method = 0;
            if (ImGui::Combo("DoF method", &method, methods, 3))
              m_DofMethod = method;
            ImGui::HelpMarker("Simple uses the Simple values above, Sprite the Sprite values.");
          }

          bool spriteFix = m_SpriteDofFix;
          if (ImGui::Checkbox("Sprite DoF fix", &spriteFix))
            SetSpriteDofFix(spriteFix);
          ImGui::HelpMarker("From the original tools. Makes the game rebuild sprite DoF every frame "
            "instead of keeping its old settings, so changes to the sprite values show up.");
        }

        if (basicCount < component.Fields.size() && ImGui::TreeNode("More settings"))
        {
          for (size_t i = basicCount; i < component.Fields.size(); ++i)
            DrawField(component, component.Fields[i]);
          ImGui::TreePop();
        }
        ImGui::PopItemWidth();
      }
    }
    ImGui::PopID();
  }
}

void VisualsManager::SetSpriteDofFix(bool enable)
{
  // The original patched OFFSET_DOFFIX with a jump to a stub that always
  // continued at +0x7E, skipping the game's "settings unchanged" checks.
  // A short jump straight to +0x7E does the same.
  if (!m_pSpriteDofFix || enable == m_SpriteDofFix) return;

  BYTE* pCode = (BYTE*)m_pSpriteDofFix;
  DWORD oldProtect;
  if (!VirtualProtect(pCode, 2, PAGE_EXECUTE_READWRITE, &oldProtect))
  {
    util::log::Error("Unable to patch Sprite DoF, VirtualProtect failed 0x%X", GetLastError());
    return;
  }

  if (enable)
  {
    memcpy(m_SpriteDofOriginalBytes, pCode, 2);
    pCode[0] = 0xEB; // jmp short
    pCode[1] = 0x7C; // +2 + 0x7C = +0x7E
  }
  else
    memcpy(pCode, m_SpriteDofOriginalBytes, 2);

  VirtualProtect(pCode, 2, oldProtect, &oldProtect);
  FlushInstructionCache(GetCurrentProcess(), pCode, 2);
  m_SpriteDofFix = enable;
  util::log::Write("Sprite DoF fix %s", enable ? "enabled" : "disabled");
}
