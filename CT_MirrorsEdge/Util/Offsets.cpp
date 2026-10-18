#include "Util.h"
#include "../Main.h"

#include <fstream>
#include <unordered_map>
#include <Psapi.h>

namespace
{
  bool m_UseScannedResults = false;
  std::unordered_map<std::string, util::offsets::Signature> m_Signatures;

  // Module-relative offsets taken from the original CT_MirrorsEdge.dll (2018-02-10).
  // They only match the exact MirrorsEdgeCatalyst.exe build that tool targeted
  // (PE timestamp 0x5762E3B7, 2016-06-16, also the current Steam / EA app build),
  // and a wrong address here means a crash, so they're only used as a fallback
  // when the game's timestamp matches.
  const DWORD g_supportedTimeStamp = 0x5762E3B7;
  bool m_UseHardcodedFallback = false;
  std::unordered_map<std::string, __int64> m_HardcodedOffsets =
  {
    { "OFFSET_DXRENDERER",          0x2563300 },
    { "OFFSET_GAMERENDERER",        0x2563A10 },
    { "OFFSET_SYSTEM",              0x2413A00 },
    { "OFFSET_GAMERENDERSETTINGS",  0x2142A58 },
    { "OFFSET_WORLDRENDERSETTINGS", 0x240C2B8 },
    { "OFFSET_GAMETIMESETTINGS",    0x2142A68 },
    { "OFFSET_UISETTINGS",          0x2142B08 },
    { "OFFSET_CLIENTGAMECONTEXT",   0x24A9F80 },
    { "OFFSET_MAIN",                0x2142A48 },
    { "OFFSET_3PTOGGLE",            0x1B18E58 },
    { "OFFSET_CAMERAUPDATE",        0x352D810 },
    { "OFFSET_ENTRYINPUT",          0x32FD3B0 },
    { "OFFSET_CONTROLLERINPUT",     0x37BABA0 },
    { "OFFSET_MESHSETTINGS",        0x23E3A70 },
    { "OFFSET_VISUALENVUPDATE",     0x36ED460 },
    { "OFFSET_UPDATELIGHT",         0x376A030 },
    { "OFFSET_RESOURCEMANAGER",     0x237DE70 },
    { "OFFSET_DOFFIX",              0x35C72EE },
    // Not from the original tools: the static pointer MEC-TC (Mirror's Edge Catalyst
    // Time Changer) reads the time of day through, [[[p] + 0x8] + 0x28] + 0x30. No signature yet.
    { "OFFSET_TIMEOFDAY",           0x255C2F8 },
    // Pointer to the mesh streaming manager
    { "OFFSET_MESHSTREAMING",       0x2563410 },
    // Not from the original tools: the city's zone streamer
    { "OFFSET_ZONESTREAMERUPDATE",  0x3C81900 },
    { "OFFSET_ZONEFOCUSADD",        0x3C7E7C0 },
    { "OFFSET_ZONEFOCUSREMOVE",     0x3C81390 },
  };

  bool DataCompare(BYTE* pData, BYTE* bSig, const char* szMask)
  {
    for (; *szMask; ++szMask, ++pData, ++bSig)
    {
      if (*szMask == 'x' && *pData != *bSig)
        return false;
    }
    return (*szMask) == 0;
  }

  BYTE* FindPattern(BYTE* dwAddress, __int64 dwSize, BYTE* pbSig, const char* szMask)
  {
    BYTE bFirstByte = *(BYTE*)pbSig;

    __int64 length = (__int64)dwAddress + dwSize - strlen(szMask);

    for (__int64 i = (__int64)dwAddress; i < length; i += 4) // might run faster with 8 bytes but I am too lazy
    {
      unsigned x = *(unsigned*)(i);

      if ((x & 0xFF) == bFirstByte)
        if (DataCompare(reinterpret_cast<BYTE*>(i), pbSig, szMask))
          return reinterpret_cast<BYTE*>(i);

      if ((x & 0xFF00) >> 8 == bFirstByte)
        if (DataCompare(reinterpret_cast<BYTE*>(i + 1), pbSig, szMask))
          return reinterpret_cast<BYTE*>(i + 1);

      if ((x & 0xFF0000) >> 16 == bFirstByte)
        if (DataCompare(reinterpret_cast<BYTE*>(i + 2), pbSig, szMask))
          return reinterpret_cast<BYTE*>(i + 2);

      if ((x & 0xFF000000) >> 24 == bFirstByte)
        if (DataCompare(reinterpret_cast<BYTE*>(i + 3), pbSig, szMask))
          return reinterpret_cast<BYTE*>(i + 3);
    }
    return 0;
  }

  // Page protections can change while the game decrypts itself, so an
  // access violation just counts as "not found" instead of crashing the game.
  BYTE* SafeFindPattern(BYTE* pStart, __int64 size, BYTE* pbSig, const char* szMask)
  {
    __try
    {
      return FindPattern(pStart, size, pbSig, szMask);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      return nullptr;
    }
  }

  // Searches the committed, readable parts of the game module
  __int64 ScanModule(util::offsets::Signature const& sig)
  {
    MODULEINFO info;
    if (!GetModuleInformation(GetCurrentProcess(), g_gameHandle, &info, sizeof(MODULEINFO)))
      return 0;

    BYTE* pEnd = (BYTE*)info.lpBaseOfDll + info.SizeOfImage;
    BYTE* pRunStart = nullptr;
    BYTE* pCurrent = (BYTE*)info.lpBaseOfDll;

    while (pCurrent < pEnd)
    {
      MEMORY_BASIC_INFORMATION mbi;
      if (!VirtualQuery(pCurrent, &mbi, sizeof(mbi)))
        break;

      BYTE* pRegionEnd = (BYTE*)mbi.BaseAddress + mbi.RegionSize;
      if (pRegionEnd > pEnd)
        pRegionEnd = pEnd;

      const DWORD readableFlags = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY |
        PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
      bool readable = mbi.State == MEM_COMMIT && (mbi.Protect & readableFlags) && !(mbi.Protect & PAGE_GUARD);

      // Neighbouring readable regions are scanned as one run so that
      // patterns crossing a region boundary are still found
      if (readable && !pRunStart)
        pRunStart = pCurrent;
      else if (!readable && pRunStart)
      {
        BYTE* pResult = SafeFindPattern(pRunStart, pCurrent - pRunStart, sig.Pattern, sig.Mask.c_str());
        if (pResult)
          return (__int64)pResult;
        pRunStart = nullptr;
      }

      pCurrent = pRegionEnd;
    }

    if (pRunStart)
      return (__int64)SafeFindPattern(pRunStart, pCurrent - pRunStart, sig.Pattern, sig.Mask.c_str());

    return 0;
  }

  DWORD GetGameTimeStamp()
  {
    IMAGE_DOS_HEADER* pDosHeader = (IMAGE_DOS_HEADER*)g_gameHandle;
    if (pDosHeader->e_magic != IMAGE_DOS_SIGNATURE)
      return 0;

    IMAGE_NT_HEADERS* pNtHeaders = (IMAGE_NT_HEADERS*)((BYTE*)g_gameHandle + pDosHeader->e_lfanew);
    if (pNtHeaders->Signature != IMAGE_NT_SIGNATURE)
      return 0;

    return pNtHeaders->FileHeader.TimeDateStamp;
  }

  bool ResolveSignature(std::string const& name, util::offsets::Signature& sig)
  {
    __int64 result = ScanModule(sig);
    if (!result)
      return false;

    if (sig.HasReference)
    {
      // Get the assembly reference
      int* pReference = (int*)(result + sig.ReferenceOffset);
      // Assembly reference is relative to the address after the reference
      sig.Result = ((__int64)pReference + sig.ReferenceSize) + *pReference;
      sig.Result += sig.AddOffset;
    }
    else
      sig.Result = result + sig.AddOffset;

    util::log::Write("%-28s 0x%I64X", name.c_str(), sig.Result);
    return true;
  }
}

util::offsets::Signature::Signature(std::string const& sig, int offset /* = 0 */)
{
  Pattern = new BYTE[sig.size()]();
  AddOffset = offset;

  unsigned int patternOffset = 0;
  for (unsigned int i = 0; i < sig.size(); ++i)
  {
    switch (sig[i])
    {
      case ' ':
      {
        break;
      }
      case '[':
      {
        HasReference = true;
        ReferenceOffset = patternOffset;
        break;
      }
      case ']':
      {
        ReferenceSize = patternOffset - ReferenceOffset;
        break;
      }
      case '?':
      {
        Mask += '?';
        // In signature it's clearer to mark one wildcard byte as ??
        // so skip the next character.
        Pattern[patternOffset++] = 0;
        i += 1;
        break;
      }
      default:
      {
        Mask += 'x';
        // Process 2 characters into a single byte
        Pattern[patternOffset++] = (util::CharToByte(sig[i]) << 4) + util::CharToByte(sig[i+1]);
        i += 1;
      }
    }
  }
}

void util::offsets::Scan()
{
  // Signature example
  // Scan memory and find this pattern. Question marks are wildcard bytes.
  // Brackets mean that the offset is extracted from the assembly reference
  // For example:
  // mov rax,[123456]
  // We want 123456, its place should be marked with wildcard bytes surrounded
  // by brackets in the signature.
  //
  // The last argument is the offset to be added to the result, useful when
  // you need a code offset for byte patches.
  //
  // Signatures are copied from the original CT_MirrorsEdge.dll.
  // Bracketed ones resolve to a static pointer (or a call target for
  // CAMERAUPDATE), the rest are the address of the match itself.

  m_Signatures.emplace("OFFSET_DXRENDERER",          Signature("48 8B 0D [ ?? ?? ?? ?? ] 48 8B 53 10 43 8D 04 76"));
  m_Signatures.emplace("OFFSET_GAMERENDERER",        Signature("48 8B 0D [ ?? ?? ?? ?? ] 48 8B 01 31 D2 FF 50 60"));
  m_Signatures.emplace("OFFSET_SYSTEM",              Signature("74 09 48 89 C1 E8 ?? ?? ?? ?? 90 48 89 05 [ ?? ?? ?? ?? ] E8"));
  m_Signatures.emplace("OFFSET_GAMERENDERSETTINGS",  Signature("48 8B 05 [ ?? ?? ?? ?? ] 80 B8 ?? ?? ?? ?? ?? 74 52"));
  m_Signatures.emplace("OFFSET_WORLDRENDERSETTINGS", Signature("48 89 05 [ ?? ?? ?? ?? ] 0F B6 88 ?? ?? ?? ?? 8B 80"));
  m_Signatures.emplace("OFFSET_GAMETIMESETTINGS",    Signature("74 1A 48 8B 15 [ ?? ?? ?? ?? ]"));
  m_Signatures.emplace("OFFSET_UISETTINGS",          Signature("48 89 05 [ ?? ?? ?? ?? ] 83 78 20 00 75 0D"));
  m_Signatures.emplace("OFFSET_CLIENTGAMECONTEXT",   Signature("48 8D 05 [ ?? ?? ?? ?? ] 48 8B 04 D0 48 85 C0"));
  m_Signatures.emplace("OFFSET_MAIN",                Signature("48 89 3D [ ?? ?? ?? ?? ] C7 87"));
  m_Signatures.emplace("OFFSET_3PTOGGLE",            Signature("48 8D 05 [ ?? ?? ?? ?? ] 48 89 03 31 F6 48 89 B3 ?? ?? ?? ?? 48 89 B3 ?? ?? ?? ?? 0F 57 C0"));
  m_Signatures.emplace("OFFSET_CAMERAUPDATE",        Signature("48 89 D9 E8 [ ?? ?? ?? ?? ] 0F B6 45 07"));
  m_Signatures.emplace("OFFSET_ENTRYINPUT",          Signature("3B 15 ?? ?? ?? ?? 73 2C"));
  m_Signatures.emplace("OFFSET_CONTROLLERINPUT",     Signature("0F 57 DB 30 C9"));
  m_Signatures.emplace("OFFSET_MESHSETTINGS",        Signature("48 89 05 [ ?? ?? ?? ?? ] 8B 40 44"));
  m_Signatures.emplace("OFFSET_VISUALENVUPDATE",     Signature("48 89 C2 E8 [ ?? ?? ?? ?? ] 45 31 C0"));
  m_Signatures.emplace("OFFSET_UPDATELIGHT",         Signature("48 83 EC 48 80 B9 ?? ?? ?? ?? ?? 75 44"));
  m_Signatures.emplace("OFFSET_RESOURCEMANAGER",     Signature("48 8B 14 D8 48 8B 0D [ ?? ?? ?? ?? ]"));
  m_Signatures.emplace("OFFSET_DOFFIX",              Signature("48 8B 48 08 48 8B 01 FF 50 58"));

  // Not from the original tools. ZONESTREAMERUPDATE is the start of ZoneStreamerEntity's
  // update, the other two are the calls ZoneStreamerFocusEntity makes when it's enabled
  // (add) and disabled (remove).
  m_Signatures.emplace("OFFSET_ZONESTREAMERUPDATE",  Signature("57 48 83 EC 50 48 C7 44 24 20 FE FF FF FF 48 89 5C 24 60 48 89 6C 24 68 48 89 74 24 70 0F 29 74 24 40 48 89 CF F3 0F 10 32"));
  m_Signatures.emplace("OFFSET_ZONEFOCUSADD",        Signature("74 0B 4C 8D 43 40 31 D2 E8 [ ?? ?? ?? ?? ]"));
  m_Signatures.emplace("OFFSET_ZONEFOCUSREMOVE",     Signature("48 8B 49 48 48 85 C9 74 2B 4C 8D 43 40 31 D2 E8 [ ?? ?? ?? ?? ]"));

  m_UseScannedResults = true;

  MODULEINFO info;
  if (!GetModuleInformation(GetCurrentProcess(), g_gameHandle, &info, sizeof(MODULEINFO)))
  {
    util::log::Error("GetModuleInformation failed, GetLastError 0x%X", GetLastError());
    util::log::Error("Offset scanning unavailable");
    return;
  }

  // The Steam / EA app exe is wrapped by EA's DRM: its code is encrypted on
  // disk and only decrypted while the game starts. When injected early, wait
  // until a known piece of code shows up before scanning for the rest.
  util::log::Write("Waiting for the game code to be decrypted...");
  Signature& probe = m_Signatures.at("OFFSET_CAMERAUPDATE");
  ULONGLONG waitStart = GetTickCount64();
  bool codeReady = false;
  while (!g_shutdown && GetTickCount64() - waitStart < 5 * 60 * 1000)
  {
    if (ResolveSignature("OFFSET_CAMERAUPDATE", probe))
    {
      codeReady = true;
      break;
    }
    Sleep(250);
  }

  if (!codeReady)
  {
    util::log::Error("The game code never became readable, no game hooks will be installed");
    return;
  }
  util::log::Ok("Game code found after %.1f seconds", (GetTickCount64() - waitStart) / 1000.0);

  DWORD timeStamp = GetGameTimeStamp();
  m_UseHardcodedFallback = (timeStamp == g_supportedTimeStamp);
  util::log::Write("Game build timestamp 0x%X (%s)", timeStamp,
    m_UseHardcodedFallback ? "same build as the original tools" : "unknown build, no hardcoded fallback");

  util::log::Write("Scanning for offsets...");

  // Retry for a few seconds in case some code is decrypted a little later
  bool allFound = false;
  for (int attempt = 0; attempt < 20 && !allFound && !g_shutdown; ++attempt)
  {
    if (attempt > 0)
      Sleep(250);

    allFound = true;
    for (auto& entry : m_Signatures)
    {
      if (entry.second.Result) continue;
      if (!ResolveSignature(entry.first, entry.second))
        allFound = false;
    }
  }

  if (allFound)
  {
    util::log::Ok("All offsets found");
    return;
  }

  for (auto& entry : m_Signatures)
  {
    if (entry.second.Result) continue;
    if (m_UseHardcodedFallback && m_HardcodedOffsets.count(entry.first))
      util::log::Warning("Could not find pattern for %s, using the hardcoded offset", entry.first.c_str());
    else
      util::log::Error("Could not find pattern for %s", entry.first.c_str());
  }
}

__int64 util::offsets::GetOffset(std::string const& name)
{
  // If a scan was done, prefer those results.
  // If something couldn't be found or there was no scan,
  // use the hardcoded offsets.

  if (m_UseScannedResults)
  {
    auto result = m_Signatures.find(name);
    if (result != m_Signatures.end())
    {
      if (result->second.Result)
        return result->second.Result;
    }
  }

  // If the offsets were scanned, their absolute position is known.
  // With hardcoded offsets, use relative offset because it's not
  // 100% guaranteed the game module will load at the same address space.

  auto hardcodedResult = m_HardcodedOffsets.find(name);
  if (m_UseHardcodedFallback && hardcodedResult != m_HardcodedOffsets.end())
    return hardcodedResult->second + (__int64)g_gameHandle;

  util::log::Error("Offset %s does not exist", name.c_str());
  return 0;
}