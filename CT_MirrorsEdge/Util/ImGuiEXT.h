#pragma once
#include "../imgui/imgui.h"
#include <functional>

// Extensions and helpers for fancier ImGui elements
namespace ImGui
{
  IMGUI_API bool Checkbox(const char* label, bool* v, const ImVec2& subtractSize);
  IMGUI_API bool ToggleButton(const char* label, const ImVec2& size_arg, bool toggled, bool canUntoggle);
  IMGUI_API void Separator(const ImVec2& size, const ImVec4& color = GetStyleColorVec4(ImGuiCol_Separator));

  IMGUI_API bool ColorEdit3(const char* label, float col[3], float* pIntensity, float intensityCol[4], ImGuiColorEditFlags flags = 0);
  IMGUI_API bool ColorEdit4(const char* label, float col[4], float* pIntensity, float intensityCol[4], ImGuiColorEditFlags flags = 0);
  IMGUI_API bool ColorPicker4(const char* label, float col[4], float* pIntensity, float intensityCol[4], ImGuiColorEditFlags flags = 0, const float* ref_col = NULL);

  IMGUI_API bool BeginPopup(const char* str_id, const ImVec2&, ImGuiWindowFlags flags = 0);
  static bool BeginPopupEx(ImGuiID id, ImGuiWindowFlags extra_flags, const ImVec2& pos);

  void DrawWithBorders(std::function<void()> = nullptr);

  // A grey "(?)" after the previous item that shows text when hovered
  void HelpMarker(const char* text);
  // A tooltip for the previous item, wrapped to a readable width
  void ItemTooltip(const char* text);
  // A button that has to be clicked twice (within 3 seconds) for things
  // that can't be undone. The second click returns true.
  bool ConfirmButton(const char* label, const ImVec2& size = ImVec2(0, 0));
  // Small heading text inside a section
  void SubHeading(const char* text);
}

// UTF-8 for the degree sign, the fonts include it
#define UI_DEG "\xC2\xB0"

