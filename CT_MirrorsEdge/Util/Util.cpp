#include "Util.h"
#include "../Main.h"

#include <atomic>
#include <codecvt>
#include <locale>

bool util::IsMecmpLoaded()
{
  static std::atomic<ULONGLONG> s_checkedAt{ 0 };
  static std::atomic<bool> s_loaded{ false };
  ULONGLONG now = GetTickCount64();
  if (now - s_checkedAt >= 1000)
  {
    s_checkedAt = now;
    bool loaded = GetModuleHandleW(L"mecmp.dll") != nullptr;
    if (loaded != s_loaded.exchange(loaded))
      util::log::Write(loaded ? "MECMP is open: the time of day controls are disabled, MECMP syncs the time of day"
        : "MECMP is closed: the time of day controls are back");
  }
  return s_loaded;
}

// Loads resource data from the .dll file based on resource IDs in resource.h
bool util::GetResource(int ID, void* &pData, DWORD& size)
{
  HRSRC rc = FindResource(g_dllHandle, MAKEINTRESOURCE(ID), RT_RCDATA);
  if (!rc)
  {
    util::log::Error("FindResource failed, GetLastError 0x%X resource ID %d", GetLastError(), ID);
    return false;
  }

  HGLOBAL hglobal = LoadResource(g_dllHandle, rc);
  if (!hglobal)
  {
    util::log::Error("LoadResource failed, GetLastError 0x%X resource ID %d", GetLastError(), ID);
    return false;
  }

  pData = LockResource(hglobal);
  size = SizeofResource(g_dllHandle, rc);

  if (pData == nullptr || size == 0)
  {
    util::log::Error("GetResource returned empty data, GetLastError 0x%X resource ID %d", GetLastError(), ID);
    return false;
  }

  return true;
}

double util::math::CatmullRomInterpolate(double y0, double y1, double y2, double y3, double mu)
{
  double mu2 = mu * mu;
  double a0 = -0.5 * y0 + 1.5 * y1 - 1.5 * y2 + 0.5 * y3;
  double a1 = y0 - 2.5 * y1 + 2 * y2 - 0.5 * y3;
  double a2 = -0.5 * y0 + 0.5 * y2;
  double a3 = y1;

  return a0 * mu * mu2 + a1 * mu2 + a2 * mu + a3;
}

std::string util::VkToString(DWORD vk)
{
  unsigned int scanCode = MapVirtualKey(vk, MAPVK_VK_TO_VSC);

  switch (vk)
  {
    case VK_LEFT: case VK_UP: case VK_RIGHT: case VK_DOWN: // arrow keys
    case VK_PRIOR: case VK_NEXT: // page up and page down
    case VK_END: case VK_HOME:
    case VK_INSERT: case VK_DELETE:
    case VK_DIVIDE: // numpad slash
    case VK_NUMLOCK:
    {
      scanCode |= 0x100; // set extended bit
      break;
    }
  }

  wchar_t wKeyName[50];
  memset(wKeyName, 0, 100);

  int length = GetKeyNameTextW(scanCode << 16, (LPWSTR)&wKeyName, 50);
  wKeyName[length] = L'\0';

  std::wstring_convert<std::codecvt_utf8<wchar_t>> utf8_conv;
  return utf8_conv.to_bytes(wKeyName);
}

std::string util::KeyLparamToString(LPARAM lparam)
{
  unsigned int scanCode = (lparam >> 16) & 0xFF;
  if ((lparam >> 24) & 1)
    scanCode |= 0x100;

  wchar_t wKeyName[50];
  memset(wKeyName, 0, 100);

  int length = GetKeyNameTextW(scanCode << 16, (LPWSTR)&wKeyName, 50);
  wKeyName[length] = L'\0';

  std::wstring_convert<std::codecvt_utf8<wchar_t>> utf8_conv;
  return utf8_conv.to_bytes(wKeyName);
}

BYTE util::CharToByte(char c)
{
  // Converts a single hex digit. (sscanf on &c read past the
  // character since it isn't null terminated.)
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return 0;
}
