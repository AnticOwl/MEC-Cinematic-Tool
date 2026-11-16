// A sound file read into plain 16-bit samples with Windows' Media Foundation:
// see VoiceDecode.h. Where a comment below says "measured", that was on one
// Windows 11 PC, 2026-11.
#include "VoiceDecode.h"

#include <Windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <new>

// The GUIDs only (a static library): no DLL is bound through it
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "ole32.lib")

using Microsoft::WRL::ComPtr;

namespace voice_decode
{
  namespace
  {
    // Media Foundation's functions, found at the first use. The DLLs stay
    // loaded for the life of the process: Media Foundation leaves threads of
    // Windows' thread pool behind that may still run its code.
    struct Platform
    {
      HMODULE Plat = nullptr;
      HMODULE ReadWrite = nullptr;
      HRESULT(STDAPICALLTYPE* Startup)(ULONG, DWORD) = nullptr;
      HRESULT(STDAPICALLTYPE* Shutdown)() = nullptr;
      HRESULT(STDAPICALLTYPE* CreateMediaType)(IMFMediaType**) = nullptr;
      HRESULT(STDAPICALLTYPE* CreateAttributes)(IMFAttributes**, UINT32) = nullptr;
      HRESULT(STDAPICALLTYPE* CreateSourceReaderFromURL)(LPCWSTR, IMFAttributes*, IMFSourceReader**) = nullptr;
      bool Tried = false;
      bool Ok = false;
      DWORD LoadError = 0;
    };

    std::mutex g_PlatformLock;
    Platform g_Platform;
    // The Holds that have no Unhold yet (guarded by g_PlatformLock)
    int g_Holds = 0;

    // The most 16-bit samples one sound may hold (256 MiB), whatever
    // maxSeconds says: 600 s at 192000 Hz stereo would be 439 MiB
    const unsigned long long MaxSamples = 128ull * 1024 * 1024;

    Platform const* GetPlatform(std::string& error)
    {
      std::lock_guard<std::mutex> lock(g_PlatformLock);
      Platform& p = g_Platform;
      if (!p.Tried)
      {
        p.Tried = true;
        // System32 only: never a DLL of the same name beside the game
        p.Plat = LoadLibraryExW(L"mfplat.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!p.Plat)
          p.LoadError = GetLastError();
        if (p.Plat)
        {
          p.ReadWrite = LoadLibraryExW(L"mfreadwrite.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
          if (!p.ReadWrite)
            p.LoadError = GetLastError();
        }
        if (p.Plat && p.ReadWrite)
        {
          p.Startup = (HRESULT(STDAPICALLTYPE*)(ULONG, DWORD))GetProcAddress(p.Plat, "MFStartup");
          p.Shutdown = (HRESULT(STDAPICALLTYPE*)())GetProcAddress(p.Plat, "MFShutdown");
          p.CreateMediaType = (HRESULT(STDAPICALLTYPE*)(IMFMediaType**))GetProcAddress(p.Plat, "MFCreateMediaType");
          p.CreateAttributes = (HRESULT(STDAPICALLTYPE*)(IMFAttributes**, UINT32))
            GetProcAddress(p.Plat, "MFCreateAttributes");
          p.CreateSourceReaderFromURL = (HRESULT(STDAPICALLTYPE*)(LPCWSTR, IMFAttributes*, IMFSourceReader**))
            GetProcAddress(p.ReadWrite, "MFCreateSourceReaderFromURL");
          p.Ok = p.Startup && p.Shutdown && p.CreateMediaType && p.CreateAttributes && p.CreateSourceReaderFromURL;
          if (!p.Ok)
            p.LoadError = ERROR_PROC_NOT_FOUND;
        }
      }
      if (!p.Ok)
      {
        char text[200];
        _snprintf_s(text, sizeof(text), _TRUNCATE,
          "Windows' Media Foundation is not on this PC (mfplat.dll / mfreadwrite.dll, Windows error %lu): "
          "on an N edition of Windows, install the Media Feature Pack.", p.LoadError);
        error = text;
        return nullptr;
      }
      return &p;
    }

    // This thread's COM for the length of a decode. S_OK and S_FALSE (the
    // thread already had COM, in the same mode) both count one more use and
    // need CoUninitialize; RPC_E_CHANGED_MODE (the thread is in the other
    // mode, a single-threaded apartment) counted nothing and needs none: COM
    // is usable as it is.
    struct ComScope
    {
      HRESULT Hr;
      bool Keep = false;   // left started when a decoder is stuck (see Read)
      ComScope() : Hr(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) {}
      ~ComScope()
      {
        if (SUCCEEDED(Hr) && !Keep)
          CoUninitialize();
      }
      bool Usable() const { return SUCCEEDED(Hr) || Hr == RPC_E_CHANGED_MODE; }
      ComScope(ComScope const&) = delete;
      ComScope& operator=(ComScope const&) = delete;
    };

    // Media Foundation started for the length of a decode. Its start is
    // counted by Windows: only the last MFShutdown of the process stops it.
    struct MfScope
    {
      Platform const* Mf;
      HRESULT Hr;
      bool Keep = false;   // left started when a decoder is stuck (see Read)
      explicit MfScope(Platform const* mf) : Mf(mf), Hr(mf->Startup(MF_VERSION, MFSTARTUP_LITE)) {}
      ~MfScope()
      {
        if (SUCCEEDED(Hr) && !Keep)
          Mf->Shutdown();
      }
      MfScope(MfScope const&) = delete;
      MfScope& operator=(MfScope const&) = delete;
    };

    // The reader is used in its asynchronous mode, so that each block of
    // samples can be waited for with a time limit. Windows' AAC decoder
    // (MSAudDecMFT.dll) was seen spinning for ever on some damaged M4A files
    // (measured: one wrong entry in the sample size table is enough; 12
    // minutes at 100 % of a processor core, then stopped by hand), and a
    // plain ReadSample waits for it for ever.
    const DWORD BlockTimeoutMs = 15000;

    // What the reader calls, on a thread of Windows' own, when a block of
    // samples is ready: the block is kept here and Arrived is set
    struct ReaderCallback : IMFSourceReaderCallback
    {
      LONG References = 1;
      HANDLE Arrived;
      CRITICAL_SECTION Lock;
      HRESULT Status = S_OK;
      DWORD Flags = 0;
      LONGLONG Stamp = 0;
      ComPtr<IMFSample> Sample;

      ReaderCallback() : Arrived(CreateEventW(nullptr, FALSE, FALSE, nullptr)) { InitializeCriticalSection(&Lock); }
      virtual ~ReaderCallback()
      {
        DeleteCriticalSection(&Lock);
        if (Arrived)
          CloseHandle(Arrived);
      }
      STDMETHODIMP QueryInterface(REFIID iid, void** out) override
      {
        if (!out)
          return E_POINTER;
        if (iid == __uuidof(IUnknown) || iid == __uuidof(IMFSourceReaderCallback))
        {
          *out = static_cast<IMFSourceReaderCallback*>(this);
          AddRef();
          return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
      }
      STDMETHODIMP_(ULONG) AddRef() override { return (ULONG)InterlockedIncrement(&References); }
      STDMETHODIMP_(ULONG) Release() override
      {
        LONG left = InterlockedDecrement(&References);
        if (!left)
          delete this;
        return (ULONG)left;
      }
      STDMETHODIMP OnReadSample(HRESULT status, DWORD, DWORD flags, LONGLONG stamp, IMFSample* sample) override
      {
        EnterCriticalSection(&Lock);
        Status = status;
        Flags = flags;
        Stamp = stamp;
        Sample = sample;
        LeaveCriticalSection(&Lock);
        SetEvent(Arrived);
        return S_OK;
      }
      STDMETHODIMP OnFlush(DWORD) override { return S_OK; }
      STDMETHODIMP OnEvent(DWORD, IMFMediaEvent*) override { return S_OK; }
      ReaderCallback(ReaderCallback const&) = delete;
      ReaderCallback& operator=(ReaderCallback const&) = delete;
    };

    // One block of samples, waited for at most BlockTimeoutMs; stuck is set
    // when nothing came in that time
    HRESULT ReadBlock(IMFSourceReader* reader, ReaderCallback* callback, DWORD stream, DWORD& flags, LONGLONG& stamp,
      ComPtr<IMFSample>& sample, bool& stuck)
    {
      HRESULT hr = reader->ReadSample(stream, 0, nullptr, nullptr, nullptr, nullptr);
      if (FAILED(hr))
        return hr;
      if (WaitForSingleObject(callback->Arrived, BlockTimeoutMs) != WAIT_OBJECT_0)
      {
        stuck = true;
        return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
      }
      EnterCriticalSection(&callback->Lock);
      hr = callback->Status;
      flags = callback->Flags;
      stamp = callback->Stamp;
      sample = callback->Sample;
      callback->Sample.Reset();
      LeaveCriticalSection(&callback->Lock);
      return hr;
    }

    bool IsDrm(HRESULT hr)
    {
      unsigned int code = (unsigned int)hr;
      // Windows Media's DRM errors (NS_E_DRM_*) sit in this range
      if (code >= 0xC00D2711u && code <= 0xC00D2FFFu)
        return true;
      return hr == MF_E_DRM_UNSUPPORTED || hr == MF_E_LICENSE_INCORRECT_RIGHTS || hr == MF_E_LICENSE_OUTOFDATE ||
        hr == MF_E_LICENSE_REQUIRED;
    }

    // A plain text for a failed step
    std::string Fail(char const* step, HRESULT hr)
    {
      char const* what = "Windows couldn't decode this file";
      if (hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) || hr == HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND) ||
          hr == HRESULT_FROM_WIN32(ERROR_INVALID_NAME) || hr == HRESULT_FROM_WIN32(ERROR_BAD_NETPATH))
        what = "The file was not found";
      else if (hr == HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED) || hr == HRESULT_FROM_WIN32(ERROR_SHARING_VIOLATION) ||
               hr == HRESULT_FROM_WIN32(ERROR_LOCK_VIOLATION) || hr == E_ACCESSDENIED)
        what = "The file can't be opened (no permission, or another program holds it)";
      else if (IsDrm(hr))
        what = "The file is copy-protected (DRM): Windows won't decode it for other programs";
      else if (hr == MF_E_UNSUPPORTED_BYTESTREAM_TYPE || hr == MF_E_INVALID_FILE_FORMAT || hr == MF_E_INVALID_FORMAT ||
               hr == MF_E_UNSUPPORTED_FORMAT || hr == MF_E_UNSUPPORTED_SCHEME)
        what = "This is not a sound file Windows can read (unknown or damaged format)";
      else if (hr == MF_E_INVALIDSTREAMNUMBER || hr == MF_E_MEDIA_SOURCE_NO_STREAMS_SELECTED)
        what = "The file has no sound track";
      else if (hr == MF_E_TOPO_CODEC_NOT_FOUND || hr == MF_E_INVALIDMEDIATYPE || hr == MF_E_NO_MORE_TYPES ||
               hr == MF_E_TRANSFORM_TYPE_NOT_SET)
        what = "This Windows has no decoder for the sound in this file";
      else if (hr == E_OUTOFMEMORY)
        what = "Not enough memory to decode this file";
      char text[300];
      _snprintf_s(text, sizeof(text), _TRUNCATE, "%s (HRESULT 0x%08X while %s).", what, (unsigned int)hr, step);
      return text;
    }

    void AddNote(std::string& note, char const* text)
    {
      if (!note.empty())
        note += "; ";
      note += text;
    }

    // ------------------------------------------------------------ the file

    unsigned int Big32(BYTE const* p)
    {
      return ((unsigned int)p[0] << 24) | ((unsigned int)p[1] << 16) | ((unsigned int)p[2] << 8) | (unsigned int)p[3];
    }

    unsigned long long Big64(BYTE const* p)
    {
      return ((unsigned long long)Big32(p) << 32) | Big32(p + 4);
    }

    // The file open for a look at its bytes (closed again before Media
    // Foundation opens it)
    struct FileScope
    {
      HANDLE Handle;
      explicit FileScope(std::wstring const& path)
        : Handle(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr))
      {
      }
      ~FileScope()
      {
        if (Handle != INVALID_HANDLE_VALUE)
          CloseHandle(Handle);
      }
      bool Ok() const { return Handle != INVALID_HANDLE_VALUE; }
      // Up to count bytes from offset; the number read
      DWORD ReadAt(unsigned long long offset, void* to, DWORD count) const
      {
        LARGE_INTEGER at;
        at.QuadPart = (LONGLONG)offset;
        DWORD got = 0;
        if (!SetFilePointerEx(Handle, at, nullptr, FILE_BEGIN) || !ReadFile(Handle, to, count, &got, nullptr))
          return 0;
        return got;
      }
      FileScope(FileScope const&) = delete;
      FileScope& operator=(FileScope const&) = delete;
    };

    // What the file's own bytes say about the silence its encoder put before
    // and after the sound. Media Foundation's readers and decoders pass that
    // silence on (measured on Windows 11, 2026-11), so the module drops it.
    struct FileFacts
    {
      BYTE Head[16] = {};
      DWORD HeadSize = 0;
      // MP3: the first frame, and its Xing / Info tag when it is one
      bool Mp3 = false;
      bool Mp3InfoTag = false;     // the tag frame is named "Info" (constant bit rate)
      int Mp3FrameSamples = 0;     // 1152 (MPEG-1) or 576 (MPEG-2 and 2.5)
      int Mp3Delay = -1;           // the encoder's lead-in from the LAME part of the tag (-1: not there)
      int Mp3Padding = -1;         // and its padding at the end
      long long Mp3Frames = -1;    // the frame count the tag gives (-1: not there)
      // MP4 / M4A: where the first sound track's edit list starts the sound
      bool Mp4 = false;
      long long Mp4MediaTime = -1; // in Mp4TimeScale units (-1: no edit list)
      unsigned int Mp4TimeScale = 0;
    };

    void ReadMp3Facts(FileScope const& file, FileFacts& facts)
    {
      // Past the ID3v2 tags (cover pictures make them megabytes long)
      unsigned long long at = 0;
      for (int i = 0; i < 4; ++i)
      {
        BYTE h[10];
        if (file.ReadAt(at, h, 10) != 10 || memcmp(h, "ID3", 3) != 0)
          break;
        unsigned int size = ((h[6] & 0x7Fu) << 21) | ((h[7] & 0x7Fu) << 14) | ((h[8] & 0x7Fu) << 7) | (h[9] & 0x7Fu);
        at += 10ull + size + ((h[5] & 0x10) ? 10u : 0u);
      }
      BYTE block[4096 + 1024];
      DWORD got = file.ReadAt(at, block, sizeof(block));
      for (DWORD i = 0; i + 4 <= got && i < 4096; ++i)
      {
        if (block[i] != 0xFF || (block[i + 1] & 0xE0) != 0xE0)
          continue;
        int version = (block[i + 1] >> 3) & 3;     // 3 = MPEG-1, 2 = MPEG-2, 0 = MPEG-2.5
        int layer = (block[i + 1] >> 1) & 3;       // 1 = layer III
        int bitRate = block[i + 2] >> 4;
        int rate = (block[i + 2] >> 2) & 3;
        if (version == 1 || layer != 1 || bitRate == 15 || rate == 3)
          continue;
        bool mono = (block[i + 3] >> 6) == 3;
        DWORD side = version == 3 ? (mono ? 17u : 32u) : (mono ? 9u : 17u);
        DWORD tag = i + 4 + side;
        facts.Mp3 = true;
        facts.Mp3FrameSamples = version == 3 ? 1152 : 576;
        if (tag + 8 <= got && (memcmp(block + tag, "Info", 4) == 0 || memcmp(block + tag, "Xing", 4) == 0))
        {
          facts.Mp3InfoTag = block[tag] == 'I';
          unsigned int flags = Big32(block + tag + 4);
          if ((flags & 1) && tag + 12 <= got)
            facts.Mp3Frames = Big32(block + tag + 8);
          // After the tag's own fields (each there only when its flag is set)
          DWORD lame = tag + 8 + ((flags & 1) ? 4u : 0u) + ((flags & 2) ? 4u : 0u) + ((flags & 4) ? 100u : 0u) +
            ((flags & 8) ? 4u : 0u);
          // The LAME part, written by LAME and by ffmpeg ("Lavc" / "Lavf")
          if (lame + 24 <= got && (memcmp(block + lame, "LAME", 4) == 0 || memcmp(block + lame, "Lavc", 4) == 0 ||
                memcmp(block + lame, "Lavf", 4) == 0))
          {
            facts.Mp3Delay = (block[lame + 21] << 4) | (block[lame + 22] >> 4);
            facts.Mp3Padding = ((block[lame + 22] & 0x0F) << 8) | block[lame + 23];
          }
        }
        break;
      }
    }

    // A box of the given kind among the boxes in data[0 .. size): its payload
    bool FindBox(BYTE const* data, size_t size, char const* kind, size_t start, size_t& payload, size_t& payloadSize,
      size_t& next)
    {
      size_t at = start;
      while (at + 8 <= size)
      {
        unsigned long long boxSize = Big32(data + at);
        size_t head = 8;
        if (boxSize == 1)
        {
          if (at + 16 > size)
            return false;
          boxSize = Big64(data + at + 8);
          head = 16;
        }
        else if (boxSize == 0)
          boxSize = size - at;
        if (boxSize < head || boxSize > size - at)
          return false;
        if (memcmp(data + at + 4, kind, 4) == 0)
        {
          payload = at + head;
          payloadSize = (size_t)boxSize - head;
          next = at + (size_t)boxSize;
          return true;
        }
        at += (size_t)boxSize;
      }
      return false;
    }

    void ReadMp4Facts(FileScope const& file, FileFacts& facts)
    {
      LARGE_INTEGER fileSize{};
      if (!GetFileSizeEx(file.Handle, &fileSize))
        return;
      // The top boxes one by one up to "moov" (it may sit after all the sound)
      unsigned long long at = 0;
      std::vector<BYTE> moov;
      for (int guard = 0; guard < 256 && at + 8 <= (unsigned long long)fileSize.QuadPart; ++guard)
      {
        BYTE h[16];
        if (file.ReadAt(at, h, 8) != 8)
          return;
        unsigned long long boxSize = Big32(h);
        unsigned int head = 8;
        if (boxSize == 1)
        {
          if (file.ReadAt(at, h, 16) != 16)
            return;
          boxSize = Big64(h + 8);
          head = 16;
        }
        else if (boxSize == 0)
          boxSize = (unsigned long long)fileSize.QuadPart - at;
        if (boxSize < head)
          return;
        if (memcmp(h + 4, "moov", 4) == 0)
        {
          unsigned long long payload = boxSize - head;
          if (payload > 64ull * 1024 * 1024)
            return;
          moov.resize((size_t)payload);
          if (file.ReadAt(at + head, moov.data(), (DWORD)payload) != (DWORD)payload)
            return;
          break;
        }
        at += boxSize;
      }
      if (moov.empty())
        return;
      // The first sound track ("trak" whose handler is "soun")
      size_t from = 0;
      size_t trak = 0, trakSize = 0, next = 0;
      while (FindBox(moov.data(), moov.size(), "trak", from, trak, trakSize, next))
      {
        from = next;
        BYTE const* t = moov.data() + trak;
        size_t mdia = 0, mdiaSize = 0, after = 0;
        if (!FindBox(t, trakSize, "mdia", 0, mdia, mdiaSize, after))
          continue;
        size_t hdlr = 0, hdlrSize = 0;
        if (!FindBox(t + mdia, mdiaSize, "hdlr", 0, hdlr, hdlrSize, after) || hdlrSize < 12 ||
            memcmp(t + mdia + hdlr + 8, "soun", 4) != 0)
          continue;
        size_t mdhd = 0, mdhdSize = 0;
        if (FindBox(t + mdia, mdiaSize, "mdhd", 0, mdhd, mdhdSize, after) && mdhdSize >= 16)
        {
          BYTE const* m = t + mdia + mdhd;
          size_t where = m[0] == 1 ? 20u : 12u;
          if (mdhdSize >= where + 4)
            facts.Mp4TimeScale = Big32(m + where);
        }
        size_t edts = 0, edtsSize = 0;
        if (FindBox(t, trakSize, "edts", 0, edts, edtsSize, after))
        {
          size_t elst = 0, elstSize = 0;
          if (FindBox(t + edts, edtsSize, "elst", 0, elst, elstSize, after) && elstSize >= 8)
          {
            BYTE const* e = t + edts + elst;
            bool wide = e[0] == 1;
            unsigned int count = Big32(e + 4);
            size_t entry = wide ? 20u : 12u;
            for (unsigned int i = 0; i < count && 8 + (size_t)(i + 1) * entry <= elstSize; ++i)
            {
              BYTE const* one = e + 8 + (size_t)i * entry;
              long long mediaTime = wide ? (long long)Big64(one + 8) : (long long)(int)Big32(one + 4);
              // -1 is an empty stretch before the sound (left alone here)
              if (mediaTime >= 0)
              {
                facts.Mp4MediaTime = mediaTime;
                break;
              }
            }
          }
        }
        break;
      }
    }

    // false when the file can't be opened at all (hr says why)
    bool ReadFileFacts(std::wstring const& path, FileFacts& facts, HRESULT& hr)
    {
      FileScope file(path);
      if (!file.Ok())
      {
        hr = HRESULT_FROM_WIN32(GetLastError());
        return false;
      }
      facts.HeadSize = file.ReadAt(0, facts.Head, sizeof(facts.Head));
      if (facts.HeadSize >= 12 && memcmp(facts.Head + 4, "ftyp", 4) == 0)
      {
        facts.Mp4 = true;
        ReadMp4Facts(file, facts);
      }
      else if (facts.HeadSize >= 4 && memcmp(facts.Head, "RIFF", 4) != 0 && memcmp(facts.Head, "OggS", 4) != 0 &&
               memcmp(facts.Head, "fLaC", 4) != 0)
        ReadMp3Facts(file, facts);
      return true;
    }

    // A word to add to the error text for kinds of files Windows may not
    // read: Ogg (read only where Windows' Web Media Extensions are installed,
    // see Read) and AIFF (not read on the test PC)
    char const* HintFor(FileFacts const& facts)
    {
      if (facts.HeadSize >= 4 && memcmp(facts.Head, "OggS", 4) == 0)
        return " Ogg files (Vorbis, Opus) need Windows' Web Media Extensions (from the Microsoft Store); or convert the "
          "file to WAV, FLAC, MP3 or M4A.";
      if (facts.HeadSize >= 12 && memcmp(facts.Head, "FORM", 4) == 0 && memcmp(facts.Head + 8, "AIF", 3) == 0)
        return " AIFF files are not read by Windows: convert the file to WAV, FLAC, MP3 or M4A.";
      return "";
    }

    // ------------------------------------------------------- sample layouts

    // How the decoder hands the samples over, and how they become 1 or 2
    // channels of 16 bits
    const int MaxFileChannels = 64;

    struct Layout
    {
      int Rate = 0;
      int Channels = 0;      // in the decoder's blocks
      int OutChannels = 0;   // 1 or 2
      int Bits = 0;
      int Bytes = 0;         // per sample of one channel
      int BlockAlign = 0;    // bytes per moment (all channels)
      bool Float = false;
      DWORD Mask = 0;        // the speaker of each channel (0: not said)
      float Left[MaxFileChannels] = {};
      float Right[MaxFileChannels] = {};
    };

    // The share of each of the file's channels in the left and the right
    // channel, by the speaker it belongs to (the usual stereo mix-down: front
    // pair as it is, centre and surrounds 3 dB down, the bass channel left
    // out), scaled so that all channels at full level can't clip.
    void FillMix(Layout& lay)
    {
      DWORD mask = lay.Mask;
      if (!mask)
      {
        // Windows' default speaker sets for plain multi-channel files
        static const DWORD defaults[9] = { 0, 0x4, 0x3, 0x7, 0x33, 0x37, 0x3F, 0x13F, 0x63F };
        if (lay.Channels <= 8)
          mask = defaults[lay.Channels];
      }
      int bitsSet = 0;
      for (DWORD m = mask; m; m &= m - 1)
        ++bitsSet;
      const float half = 0.70710678f;
      float sumLeft = 0.f, sumRight = 0.f;
      if (bitsSet == lay.Channels)
      {
        int channel = 0;
        for (int bit = 0; bit < 32 && channel < lay.Channels; ++bit)
        {
          DWORD speaker = 1u << bit;
          if (!(mask & speaker))
            continue;
          float l = 0.5f, r = 0.5f;   // a speaker not listed below (overhead): both sides
          switch (speaker)
          {
          case SPEAKER_FRONT_LEFT: l = 1.f; r = 0.f; break;
          case SPEAKER_FRONT_RIGHT: l = 0.f; r = 1.f; break;
          case SPEAKER_FRONT_CENTER: l = half; r = half; break;
          case SPEAKER_LOW_FREQUENCY: l = 0.f; r = 0.f; break;
          case SPEAKER_BACK_LEFT: l = half; r = 0.f; break;
          case SPEAKER_BACK_RIGHT: l = 0.f; r = half; break;
          case SPEAKER_FRONT_LEFT_OF_CENTER: l = half; r = 0.f; break;
          case SPEAKER_FRONT_RIGHT_OF_CENTER: l = 0.f; r = half; break;
          case SPEAKER_BACK_CENTER: l = 0.5f; r = 0.5f; break;
          case SPEAKER_SIDE_LEFT: l = half; r = 0.f; break;
          case SPEAKER_SIDE_RIGHT: l = 0.f; r = half; break;
          case SPEAKER_TOP_FRONT_LEFT: l = half; r = 0.f; break;
          case SPEAKER_TOP_FRONT_RIGHT: l = 0.f; r = half; break;
          case SPEAKER_TOP_BACK_LEFT: l = half; r = 0.f; break;
          case SPEAKER_TOP_BACK_RIGHT: l = 0.f; r = half; break;
          default: break;
          }
          lay.Left[channel] = l;
          lay.Right[channel] = r;
          sumLeft += l;
          sumRight += r;
          ++channel;
        }
      }
      else
      {
        // No usable speaker list: even channels left, odd channels right
        for (int channel = 0; channel < lay.Channels; ++channel)
        {
          lay.Left[channel] = (channel & 1) ? 0.f : 1.f;
          lay.Right[channel] = (channel & 1) ? 1.f : 0.f;
          sumLeft += lay.Left[channel];
          sumRight += lay.Right[channel];
        }
      }
      float scale = (std::max)(sumLeft, sumRight);
      scale = scale > 1.f ? 1.f / scale : 1.f;
      for (int channel = 0; channel < lay.Channels; ++channel)
      {
        lay.Left[channel] *= scale;
        lay.Right[channel] *= scale;
      }
    }

    inline float ReadOne(BYTE const* p, Layout const& lay)
    {
      if (lay.Float)
      {
        if (lay.Bytes == 4)
        {
          float v;
          memcpy(&v, p, 4);
          return v;
        }
        double v;
        memcpy(&v, p, 8);
        return (float)v;
      }
      switch (lay.Bytes)
      {
      case 1:
        return ((int)p[0] - 128) * (1.f / 128.f);   // 8-bit PCM is unsigned
      case 2:
      {
        short v;
        memcpy(&v, p, 2);
        return v * (1.f / 32768.f);
      }
      case 3:
      {
        int v = (int)p[0] | ((int)p[1] << 8) | ((int)(signed char)p[2] << 16);
        return v * (1.f / 8388608.f);
      }
      default:
      {
        int v;
        memcpy(&v, p, 4);
        return (float)(v * (1.0 / 2147483648.0));
      }
      }
    }

    inline short ToShort(float v)
    {
      float s = v * 32768.f;
      if (!(s == s))
        return 0;   // not a number
      if (s >= 32767.f)
        return 32767;
      if (s <= -32768.f)
        return -32768;
      return (short)lrintf(s);
    }

    // One block of the decoder's samples added to the sound
    void Append(Layout const& lay, BYTE const* data, size_t moments, std::vector<short>& pcm)
    {
      if (!lay.Float && lay.Bytes == 2 && lay.Channels <= 2 && lay.BlockAlign == lay.Channels * 2)
      {
        size_t at = pcm.size();
        pcm.resize(at + moments * lay.Channels);
        memcpy(pcm.data() + at, data, moments * lay.Channels * sizeof(short));
        return;
      }
      size_t at = pcm.size();
      pcm.resize(at + moments * lay.OutChannels);
      short* out = pcm.data() + at;
      for (size_t m = 0; m < moments; ++m)
      {
        BYTE const* p = data + m * (size_t)lay.BlockAlign;
        if (lay.Channels <= 2)
        {
          for (int c = 0; c < lay.Channels; ++c)
            out[m * lay.Channels + c] = ToShort(ReadOne(p + (size_t)c * lay.Bytes, lay));
        }
        else
        {
          float l = 0.f, r = 0.f;
          for (int c = 0; c < lay.Channels; ++c)
          {
            float v = ReadOne(p + (size_t)c * lay.Bytes, lay);
            l += v * lay.Left[c];
            r += v * lay.Right[c];
          }
          out[m * 2] = ToShort(l);
          out[m * 2 + 1] = ToShort(r);
        }
      }
    }

    // The reader's current output type read into a layout; false with a text
    // when this module can't take it
    bool ReadLayout(IMFSourceReader* reader, Layout& lay, HRESULT& hr, std::string& error)
    {
      ComPtr<IMFMediaType> type;
      hr = reader->GetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, type.GetAddressOf());
      if (FAILED(hr))
      {
        error = Fail("asking the decoder for its sample layout", hr);
        return false;
      }
      GUID subtype = GUID_NULL;
      type->GetGUID(MF_MT_SUBTYPE, &subtype);
      lay = Layout();
      lay.Float = subtype == MFAudioFormat_Float;
      lay.Rate = (int)MFGetAttributeUINT32(type.Get(), MF_MT_AUDIO_SAMPLES_PER_SECOND, 0);
      lay.Channels = (int)MFGetAttributeUINT32(type.Get(), MF_MT_AUDIO_NUM_CHANNELS, 0);
      lay.Bits = (int)MFGetAttributeUINT32(type.Get(), MF_MT_AUDIO_BITS_PER_SAMPLE, 0);
      lay.BlockAlign = (int)MFGetAttributeUINT32(type.Get(), MF_MT_AUDIO_BLOCK_ALIGNMENT, 0);
      lay.Mask = MFGetAttributeUINT32(type.Get(), MF_MT_AUDIO_CHANNEL_MASK, 0);
      lay.Bytes = lay.Bits / 8;
      if (!lay.BlockAlign)
        lay.BlockAlign = lay.Bytes * lay.Channels;
      bool known =
        (subtype == MFAudioFormat_PCM && (lay.Bits == 8 || lay.Bits == 16 || lay.Bits == 24 || lay.Bits == 32)) ||
        (lay.Float && (lay.Bits == 32 || lay.Bits == 64));
      if (!known || lay.Rate < 1000 || lay.Rate > 768000 || lay.Channels < 1 || lay.Channels > MaxFileChannels ||
          lay.BlockAlign < lay.Bytes * lay.Channels)
      {
        char text[240];
        _snprintf_s(text, sizeof(text), _TRUNCATE,
          "The decoder hands this file over in a layout this tool doesn't read (%d Hz, %d channels, %d bits%s).",
          lay.Rate, lay.Channels, lay.Bits, lay.Float ? " float" : (subtype == MFAudioFormat_PCM ? "" : ", not PCM"));
        error = text;
        hr = MF_E_INVALIDMEDIATYPE;
        return false;
      }
      lay.OutChannels = (std::min)(lay.Channels, 2);
      if (lay.Channels > 2)
        FillMix(lay);
      return true;
    }

    // The kind of sound in the file, as the file's reader names it
    std::string KindOf(IMFSourceReader* reader, GUID& subtype)
    {
      ComPtr<IMFMediaType> native;
      subtype = GUID_NULL;
      if (FAILED(reader->GetNativeMediaType((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, native.GetAddressOf())) ||
          FAILED(native->GetGUID(MF_MT_SUBTYPE, &subtype)))
        return "?";
      if (subtype == MFAudioFormat_PCM)
        return "PCM";
      if (subtype == MFAudioFormat_Float)
        return "PCM float";
      if (subtype == MFAudioFormat_MP3)
        return "MP3";
      if (subtype == MFAudioFormat_AAC)
        return "AAC";
      if (subtype == MFAudioFormat_FLAC)
        return "FLAC";
      if (subtype == MFAudioFormat_ALAC)
        return "ALAC";
      if (subtype == MFAudioFormat_Opus)
        return "Opus";
      if (subtype == MFAudioFormat_WMAudioV8 || subtype == MFAudioFormat_WMAudioV9 ||
          subtype == MFAudioFormat_WMAudio_Lossless)
        return "WMA";
      if (subtype == MFAudioFormat_Dolby_AC3 || subtype == MFAudioFormat_Dolby_DDPlus)
        return "Dolby Digital";
      if (subtype == MFAudioFormat_MPEG)
        return "MPEG audio";
      // Kinds the readers name by a number of their own
      switch (subtype.Data1)
      {
      case 0x616C6163: return "ALAC";      // "alac", from the MP4 reader
      case 0x8D2FD10B: return "Vorbis";    // from the Matroska reader
      case 0x0002: return "ADPCM";
      case 0x0006: return "A-law";
      case 0x0007: return "mu-law";
      default: break;
      }
      char text[40];
      _snprintf_s(text, sizeof(text), _TRUNCATE, "format 0x%04lX", subtype.Data1);
      return text;
    }

    // Asks the reader for uncompressed samples: the wanted kind first, the
    // others after it. The reader adds the decoder by itself.
    HRESULT AskFor(Platform const* mf, IMFSourceReader* reader, Ask ask)
    {
      struct Try
      {
        GUID const* Subtype;
        UINT32 Bits;
      };
      const Try pcm16 = { &MFAudioFormat_PCM, 16 };
      const Try pcm = { &MFAudioFormat_PCM, 0 };
      const Try flt = { &MFAudioFormat_Float, 0 };
      Try order[3];
      int count = 0;
      if (ask == AskPcm16)
        order[count++] = pcm16;
      if (ask == AskFloat)
        order[count++] = flt;
      order[count++] = pcm;
      if (ask != AskFloat)
        order[count++] = flt;
      HRESULT hr = E_FAIL;
      for (int i = 0; i < count; ++i)
      {
        ComPtr<IMFMediaType> want;
        hr = mf->CreateMediaType(want.GetAddressOf());
        if (SUCCEEDED(hr))
          hr = want->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        if (SUCCEEDED(hr))
          hr = want->SetGUID(MF_MT_SUBTYPE, *order[i].Subtype);
        if (SUCCEEDED(hr) && order[i].Bits)
          hr = want->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, order[i].Bits);
        if (SUCCEEDED(hr))
          hr = reader->SetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, nullptr, want.Get());
        if (SUCCEEDED(hr))
          return hr;
      }
      return hr;
    }

    // A block of samples held open for reading, let go on every way out
    struct BufferLock
    {
      IMFMediaBuffer* Buffer;
      BYTE* Data = nullptr;
      DWORD Length = 0;
      HRESULT Hr;
      explicit BufferLock(IMFMediaBuffer* buffer) : Buffer(buffer), Hr(buffer->Lock(&Data, nullptr, &Length)) {}
      ~BufferLock()
      {
        if (SUCCEEDED(Hr))
          Buffer->Unlock();
      }
      BufferLock(BufferLock const&) = delete;
      BufferLock& operator=(BufferLock const&) = delete;
    };

    // The moments to drop at the start and at the end
    struct Edges
    {
      unsigned int Lead = 0;
      unsigned int Tail = 0;
      char const* From = "";
      // MP3 with a frame count in its tag: the samples the decoder must hand
      // over in all for the numbers above to be right (0: nothing to check).
      // Two values, as encoders differ on counting the tag frame itself.
      unsigned long long Expect = 0;
      unsigned long long ExpectOther = 0;
    };

    Edges FindEdges(IMFSourceReader* reader, GUID const& subtype, FileFacts const& facts, int rate)
    {
      Edges edges;
      const DWORD audio = (DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM;
      // 1. Windows' own reader knows (an M4A with iTunes' gapless tag): it
      //    reports the numbers and leaves the samples in.
      PROPVARIANT value;
      PropVariantInit(&value);
      if (SUCCEEDED(reader->GetPresentationAttribute(audio, MF_SD_AUDIO_ENCODER_DELAY, &value)))
      {
        if (value.vt == VT_UI4 && value.ulVal)
        {
          edges.Lead = value.ulVal;
          edges.From = "the file's gapless tag";
        }
        PropVariantClear(&value);
      }
      PropVariantInit(&value);
      if (SUCCEEDED(reader->GetPresentationAttribute(audio, MF_SD_AUDIO_ENCODER_PADDING, &value)))
      {
        if (value.vt == VT_UI4 && value.ulVal && edges.Lead)
          edges.Tail = value.ulVal;
        PropVariantClear(&value);
      }
      if (!edges.Lead)
      {
        if (subtype == MFAudioFormat_MP3 && facts.Mp3)
        {
          // 2. MP3. Windows' reader plays a tag frame named "Info" as one
          //    frame of silence (one named "Xing" it skips), and its decoder
          //    drops the first 528 of the 529 samples every MP3 decoder is
          //    late by: what is left is the tag frame, the encoder's own
          //    lead-in and one sample.
          unsigned int lead = facts.Mp3InfoTag ? (unsigned int)facts.Mp3FrameSamples : 0u;
          if (facts.Mp3Delay >= 0)
            lead += (unsigned int)facts.Mp3Delay + 1u;
          edges.Lead = lead;
          if (facts.Mp3Padding >= 529)
            edges.Tail = (unsigned int)facts.Mp3Padding - 529u;
          edges.From = "the MP3's tag frame";
          if (facts.Mp3Frames > 0)
          {
            unsigned long long frames = (unsigned long long)facts.Mp3Frames + (facts.Mp3InfoTag ? 1u : 0u);
            edges.Expect = frames * (unsigned int)facts.Mp3FrameSamples - 528u;
            edges.ExpectOther = (frames - 1) * (unsigned int)facts.Mp3FrameSamples - 528u;
          }
        }
        else if (subtype == MFAudioFormat_AAC && facts.Mp4 && facts.Mp4MediaTime > 0 && facts.Mp4TimeScale)
        {
          // 3. AAC in MP4 / M4A: the edit list says where the sound starts
          //    (the decoder's first 1024 or 2112 samples are not sound);
          //    Windows' reader does not follow it.
          double lead = (double)facts.Mp4MediaTime * rate / facts.Mp4TimeScale;
          if (lead < 4.0e9)
            edges.Lead = (unsigned int)(lead + 0.5);
          edges.From = "the file's edit list";
        }
      }
      // Anything longer than a second is not an encoder's lead-in
      if (edges.Lead > (unsigned int)rate)
        edges.Lead = 0;
      if (edges.Tail > (unsigned int)rate)
        edges.Tail = 0;
      return edges;
    }

    // The most moments to keep at this layout, by the limit in seconds and
    // by the limit in bytes (MaxSamples); byBytes says which of the two it was
    unsigned long long LimitFor(double maxSeconds, Layout const& lay, bool& byBytes)
    {
      unsigned long long bySeconds = (unsigned long long)(maxSeconds * lay.Rate + 0.5);
      unsigned long long byMemory = MaxSamples / (unsigned long long)lay.OutChannels;
      byBytes = byMemory < bySeconds;
      return byBytes ? byMemory : bySeconds;
    }

    bool Read(std::wstring const& given, double maxSeconds, Ask ask, bool dropEdges, Sound& out, std::string& error)
    {
      // Asked this way round so that "not a number" gets the default too (it
      // fails every comparison, and would otherwise mean "no limit"). A day
      // at most: a huge number would overflow the conversion to a count of
      // moments (LimitFor).
      if (!(maxSeconds > 0.0))
        maxSeconds = DefaultMaxSeconds;
      if (maxSeconds > 86400.0)
        maxSeconds = 86400.0;

      // Media Foundation takes the "\\?\" form of a path for a network name
      // (measured: 0x80070035): hand it the plain form
      std::wstring path = given;
      if (path.compare(0, 8, L"\\\\?\\UNC\\") == 0)
        path = L"\\\\" + path.substr(8);
      else if (path.compare(0, 4, L"\\\\?\\") == 0)
        path = path.substr(4);

      // Media Foundation takes no path of 260 characters or more (measured:
      // "not found"). The file's short name, where the drive keeps short
      // names, is the same file under a path it takes.
      if (path.size() >= MAX_PATH)
      {
        size_t length = path.size();
        std::wstring longForm = path.compare(0, 2, L"\\\\") == 0 ? L"\\\\?\\UNC\\" + path.substr(2) : L"\\\\?\\" + path;
        wchar_t shortForm[MAX_PATH + 8];
        DWORD got = GetShortPathNameW(longForm.c_str(), shortForm, MAX_PATH + 8);
        std::wstring text = got && got < MAX_PATH + 8 ? std::wstring(shortForm, got) : std::wstring();
        if (text.compare(0, 8, L"\\\\?\\UNC\\") == 0)
          text = L"\\\\" + text.substr(8);
        else if (text.compare(0, 4, L"\\\\?\\") == 0)
          text = text.substr(4);
        if (!text.empty() && text.size() < MAX_PATH)
          path = text;
        else
        {
          char text8[200];
          _snprintf_s(text8, sizeof(text8), _TRUNCATE,
            "The file's path is too long for Windows' sound reader (%zu characters, "
            "259 at most): move the file to a folder with a shorter path.", length);
          out.Result = (long)HRESULT_FROM_WIN32(ERROR_FILENAME_EXCED_RANGE);
          error = text8;
          return false;
        }
      }

      // The plain cases first, with texts of their own
      WIN32_FILE_ATTRIBUTE_DATA info{};
      if (path.empty() || !GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &info))
      {
        DWORD last = path.empty() ? (DWORD)ERROR_FILE_NOT_FOUND : GetLastError();
        out.Result = (long)HRESULT_FROM_WIN32(last);
        error = Fail("looking for the file", HRESULT_FROM_WIN32(last));
        return false;
      }
      if (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
      {
        out.Result = (long)E_INVALIDARG;
        error = "This is a folder, not a sound file.";
        return false;
      }
      if (!info.nFileSizeHigh && !info.nFileSizeLow)
      {
        out.Result = (long)MF_E_INVALID_FILE_FORMAT;
        error = "The file is empty (0 bytes).";
        return false;
      }
      FileFacts facts;
      HRESULT hr = S_OK;
      if (!ReadFileFacts(path, facts, hr))
      {
        out.Result = (long)hr;
        error = Fail("reading the file's first bytes", hr);
        return false;
      }

      ComScope com;
      if (!com.Usable())
      {
        out.Result = (long)com.Hr;
        error = Fail("starting COM on this thread", com.Hr);
        return false;
      }
      Platform const* mf = GetPlatform(error);
      if (!mf)
      {
        out.Result = (long)HRESULT_FROM_WIN32(ERROR_MOD_NOT_FOUND);
        return false;
      }
      MfScope started(mf);
      if (FAILED(started.Hr))
      {
        out.Result = (long)started.Hr;
        error = Fail("starting Media Foundation", started.Hr);
        return false;
      }

      // Everything below is released before Media Foundation and COM are let
      // go: the objects are declared after the two scopes above.
      // The reader in its asynchronous mode (see ReaderCallback). The
      // attribute store that carries the callback matters for Ogg files too
      // (measured): opened with no store at all, Media Foundation refused
      // them (0xC00D36C4); with a store they open, read by Windows' Web Media
      // Extensions, which were installed on the test PC (no PC without them
      // was tried).
      ComPtr<ReaderCallback> callback;
      callback.Attach(new ReaderCallback());
      ComPtr<IMFAttributes> attributes;
      hr = callback->Arrived ? mf->CreateAttributes(attributes.GetAddressOf(), 1) : E_OUTOFMEMORY;
      if (SUCCEEDED(hr))
        hr = attributes->SetUnknown(MF_SOURCE_READER_ASYNC_CALLBACK, callback.Get());
      if (FAILED(hr))
      {
        out.Result = (long)hr;
        error = Fail("getting the reader ready", hr);
        return false;
      }
      ComPtr<IMFSourceReader> reader;
      hr = mf->CreateSourceReaderFromURL(path.c_str(), attributes.Get(), reader.GetAddressOf());
      if (FAILED(hr))
      {
        // Media Foundation's WAV reader (not its MP3 and MP4 ones) refuses a
        // file another program has open for writing, and says "unknown
        // format" (measured). Opening the file the same way tells the two
        // apart, so that such a file gets a text of its own.
        if (hr == MF_E_UNSUPPORTED_BYTESTREAM_TYPE)
        {
          HANDLE probe = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
          DWORD last = probe == INVALID_HANDLE_VALUE ? GetLastError() : 0;
          if (probe != INVALID_HANDLE_VALUE)
            CloseHandle(probe);
          if (last == ERROR_SHARING_VIOLATION)
          {
            out.Result = (long)HRESULT_FROM_WIN32(ERROR_SHARING_VIOLATION);
            error = "Another program has this file open for writing (still recording or exporting it?): "
              "Windows can't read it until that program lets go of it.";
            return false;
          }
        }
        out.Result = (long)hr;
        error = Fail("opening the file with Media Foundation", hr) + HintFor(facts);
        return false;
      }

      // The first sound track only; a video's picture is never decoded
      const DWORD audio = (DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM;
      reader->SetStreamSelection((DWORD)MF_SOURCE_READER_ALL_STREAMS, FALSE);
      hr = reader->SetStreamSelection(audio, TRUE);
      if (FAILED(hr))
      {
        out.Result = (long)hr;
        error = Fail("choosing the sound track", hr);
        return false;
      }

      PROPVARIANT value;
      PropVariantInit(&value);
      if (SUCCEEDED(reader->GetPresentationAttribute(audio, MF_SD_PROTECTED, &value)))
      {
        bool isProtected = value.vt == VT_UI4 && value.ulVal != 0;
        PropVariantClear(&value);
        if (isProtected)
        {
          out.Result = (long)MF_E_DRM_UNSUPPORTED;
          error = Fail("checking the sound track", MF_E_DRM_UNSUPPORTED);
          return false;
        }
      }
      PropVariantInit(&value);
      if (SUCCEEDED(reader->GetPresentationAttribute((DWORD)MF_SOURCE_READER_MEDIASOURCE, MF_PD_DURATION, &value)))
      {
        if (value.vt == VT_UI8)
          out.FileSeconds = (double)value.uhVal.QuadPart * 1e-7;
        PropVariantClear(&value);
      }
      GUID subtype = GUID_NULL;
      out.FileKind = KindOf(reader.Get(), subtype);

      hr = AskFor(mf, reader.Get(), ask);
      if (FAILED(hr))
      {
        out.Result = (long)hr;
        error = Fail("asking for plain samples", hr);
        return false;
      }
      Layout lay;
      if (!ReadLayout(reader.Get(), lay, hr, error))
      {
        out.Result = (long)hr;
        return false;
      }

      out.Rate = lay.Rate;
      out.Channels = lay.OutChannels;
      out.FileChannels = lay.Channels;
      out.FileBits = lay.Bits;
      out.FileFloat = lay.Float;

      Edges edges;
      if (dropEdges)
        edges = FindEdges(reader.Get(), subtype, facts, lay.Rate);

      // The limit in moments belongs to the layout, and the layout can still
      // change before the first sample (see the loop)
      bool cutByBytes = false;
      unsigned long long maxMoments = LimitFor(maxSeconds, lay, cutByBytes);
      bool reserved = false;

      unsigned long long moments = 0;   // kept
      unsigned long long seen = 0;      // handed over by the decoder
      unsigned long long skip = edges.Lead;
      bool haveFirst = false;
      LONGLONG firstStamp = 0;
      int emptyReads = 0;
      bool typeChanged = false;
      bool ended = false;
      bool stuck = false;   // a block did not come within BlockTimeoutMs
      for (;;)
      {
        DWORD flags = 0;
        LONGLONG stamp = 0;
        ComPtr<IMFSample> sample;
        // Waited for with a time limit (see ReaderCallback)
        hr = ReadBlock(reader.Get(), callback.Get(), audio, flags, stamp, sample, stuck);
        if (FAILED(hr))
          break;
        if (flags & MF_SOURCE_READERF_ERROR)
        {
          hr = E_FAIL;
          break;
        }
        if (flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED)
        {
          // The sound changes its layout here. Before the first sample this
          // is the real layout; later the sound ends here.
          Layout next;
          if (!ReadLayout(reader.Get(), next, hr, error))
          {
            if (!seen)
            {
              out.Result = (long)hr;
              return false;
            }
            hr = S_OK;
            typeChanged = true;
            break;
          }
          if (seen && (next.Rate != lay.Rate || next.OutChannels != lay.OutChannels))
          {
            typeChanged = true;
            break;
          }
          lay = next;
          if (!seen)
          {
            out.Rate = lay.Rate;
            out.Channels = lay.OutChannels;
            out.FileChannels = lay.Channels;
            out.FileBits = lay.Bits;
            out.FileFloat = lay.Float;
            // For AAC at 24000 Hz and below the reader first claims twice
            // the real rate (and two channels for mono) and corrects itself
            // here, with the first block. So the lead-in and the limit are
            // worked out again from the real layout: counted in the claimed
            // rate, twice too many samples were dropped (measured: the sound
            // 1024 samples early, 43 to 128 ms).
            if (dropEdges)
            {
              edges = FindEdges(reader.Get(), subtype, facts, lay.Rate);
              skip = edges.Lead;
            }
            maxMoments = LimitFor(maxSeconds, lay, cutByBytes);
          }
        }
        if (flags & MF_SOURCE_READERF_STREAMTICK)
          ++out.Gaps;
        bool gotData = false;
        if (sample)
        {
          ComPtr<IMFMediaBuffer> buffer;
          hr = sample->ConvertToContiguousBuffer(buffer.GetAddressOf());
          if (FAILED(hr))
            break;
          BufferLock locked(buffer.Get());
          hr = locked.Hr;
          if (FAILED(hr))
            break;
          BYTE const* data = locked.Data;
          size_t count = (size_t)locked.Length / (size_t)lay.BlockAlign;
          BYTE const* from = data;
          if (count)
          {
            if (!haveFirst)
            {
              haveFirst = true;
              firstStamp = stamp;
              out.FirstStampSeconds = (double)stamp * 1e-7;
            }
            double drift = fabs((double)(stamp - firstStamp) * 1e-7 - (double)seen / lay.Rate);
            out.WorstStampSeconds = (std::max)(out.WorstStampSeconds, drift);
            seen += count;
            ++out.Blocks;
            gotData = true;
          }
          if (skip && count)
          {
            size_t drop = (size_t)(std::min)((unsigned long long)count, skip);
            from += drop * (size_t)lay.BlockAlign;
            count -= drop;
            skip -= drop;
            out.LeadIn += (int)drop;
          }
          unsigned long long room = maxMoments - moments;
          if (count > room)
          {
            // More sound than the limit allows: this is where it is cut
            count = (size_t)room;
            out.Cut = true;
          }
          if (count)
          {
            // Room for the whole sound at once when the file says how long
            // it is (a vector growing by itself holds the sound twice over
            // for a moment each time it grows). Never more than the limit.
            // Done here, with the first samples kept, when the layout is the
            // real one (see the change of layout above).
            if (!reserved)
            {
              reserved = true;
              if (out.FileSeconds > 0.0)
              {
                double wanted = (out.FileSeconds + 1.0) * lay.Rate + 0.5;
                unsigned long long whole = wanted < (double)maxMoments ? (unsigned long long)wanted : maxMoments;
                out.Pcm.reserve((size_t)whole * lay.OutChannels);
              }
            }
            Append(lay, from, count, out.Pcm);
            moments += count;
          }
        }
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM)
        {
          ended = true;
          break;
        }
        if (out.Cut)
          break;
        // A decoder that keeps answering with nothing must not hold the
        // thread for ever
        emptyReads = gotData ? 0 : emptyReads + 1;
        if (emptyReads > 20000)
        {
          hr = MF_E_INVALID_STREAM_DATA;
          break;
        }
      }

      // The decoder never answered. Its thread is still inside Windows'
      // decoder and can't be stopped; releasing the reader would wait for it
      // (measured: not back after 10 s), so the reader and its callback are
      // left unreleased, and Media Foundation and this thread's COM are left
      // as they are. The file stays open and one processor core stays busy
      // until the program ends.
      if (stuck)
      {
        (void)reader.Detach();
        (void)callback.Detach();
        started.Keep = true;
        com.Keep = true;
        out.Result = (long)HRESULT_FROM_WIN32(ERROR_TIMEOUT);
        error =
          "Windows' decoder stopped answering on this file: the file is damaged. Its decoder keeps one processor core "
          "busy until the game is closed; convert or export the sound again and use the new file.";
        return false;
      }
      // Media Foundation stopped under the decode (an MFShutdown too many
      // somewhere in the process) is not a damaged file: no half sound
      if (hr == MF_E_SHUTDOWN || hr == MF_E_PLATFORM_NOT_INITIALIZED)
      {
        out.Result = (long)hr;
        error = Fail("reading the samples (Media Foundation was shut down by another part of the program)", hr);
        return false;
      }
      if (FAILED(hr) && !moments)
      {
        out.Result = (long)hr;
        error = Fail("reading the samples", hr) + HintFor(facts);
        return false;
      }
      // A file that is not sound at all (a program, a picture) but has the
      // extension .mp3 is taken for MPEG audio by Media Foundation when its
      // bytes happen to hold something like a frame header: it then
      // "decodes" to a few milliseconds of full-level noise, in a file said
      // to be seconds to minutes long (measured: 27 of 64 Windows system
      // files, DLLs, two programs and a font, copied as .mp3). Such a file is
      // refused by the share of its stated length that was handed over:
      // under 2 % of a stated 2 s or more. Real files hand over at least a
      // tenth of their stated length (the worst seen: 10.7 % for an MP3 with
      // 1 MB of junk inside).
      // Only for bare MPEG audio: a video file may rightly hold a sound
      // track much shorter than its picture.
      // However the read ended (end of file, an error, a change of layout:
      // 5 of the 27 ended that way), but not when it was cut at the limit.
      if (seen && !out.Cut && (subtype == MFAudioFormat_MPEG || subtype == MFAudioFormat_MP3) &&
          out.FileSeconds >= 2.0 && (double)seen / lay.Rate < 0.02 * out.FileSeconds)
      {
        char text[240];
        _snprintf_s(text, sizeof(text), _TRUNCATE,
          "This is not a sound file, or it is badly damaged: Windows reads it as "
          "%.1f s long but found only %.3f s of sound in it.", out.FileSeconds, (double)seen / lay.Rate);
        out.Result = (long)MF_E_INVALID_FILE_FORMAT;
        error = text;
        return false;
      }
      // The whole file was read: does it hold what its tag says? If not (a
      // file cut short, or a Windows that reads the tag frame another way),
      // the end is left alone and the note says so.
      bool countOff = ended && !out.Cut && !typeChanged && edges.Expect && seen != edges.Expect &&
        seen != edges.ExpectOther;
      if (ended && !out.Cut && !typeChanged && !countOff && edges.Tail && moments > edges.Tail)
      {
        moments -= edges.Tail;
        out.TailOut = (int)edges.Tail;
        out.Pcm.resize((size_t)moments * lay.OutChannels);
      }
      if (!moments)
      {
        out.Result = (long)MF_E_INVALID_FILE_FORMAT;
        error = std::string("The file holds no sound Windows can decode (0 samples).") + HintFor(facts);
        return false;
      }

      char text[240];
      if (FAILED(hr))
      {
        // What was read before the damage is kept
        _snprintf_s(text, sizeof(text), _TRUNCATE,
          "the file is damaged after %.2f s (HRESULT 0x%08X): the sound ends there", (double)moments / lay.Rate,
          (unsigned int)hr);
        AddNote(out.Note, text);
      }
      if (typeChanged)
      {
        _snprintf_s(text, sizeof(text), _TRUNCATE, "the file changes its sound layout at %.2f s: the sound ends there",
          (double)moments / lay.Rate);
        AddNote(out.Note, text);
      }
      if (out.Cut)
      {
        // The length it was really cut at (the limit in bytes may be the
        // shorter one)
        double cutAt = (double)moments / lay.Rate;
        if (out.FileSeconds > cutAt)
          _snprintf_s(text, sizeof(text), _TRUNCATE, "cut at %.1f s%s (the file is %.1f s long)", cutAt,
            cutByBytes ? ", the most this tool keeps in memory" : "", out.FileSeconds);
        else
          _snprintf_s(text, sizeof(text), _TRUNCATE, "cut at %.1f s%s", cutAt,
            cutByBytes ? ", the most this tool keeps in memory" : "");
        AddNote(out.Note, text);
      }
      // The room was reserved from the length the file states plus a second;
      // where that was far too much (measured: 2.9 times for ADPCM, 9.4 times
      // for an MP3 with junk inside, a second for a one-sample file) the
      // sound would keep it for as long as it lives: it is then copied into
      // a vector of its own size
      if (out.Pcm.capacity() - out.Pcm.size() > out.Pcm.size() / 8 + 16384)
      {
        std::vector<short> exact(out.Pcm);
        out.Pcm.swap(exact);
      }
      if (out.LeadIn || out.TailOut)
      {
        _snprintf_s(text, sizeof(text), _TRUNCATE,
          "encoder silence dropped: %d samples at the start, %d at the end (from %s)", out.LeadIn, out.TailOut,
          edges.From);
        AddNote(out.Note, text);
      }
      if (countOff)
      {
        _snprintf_s(text, sizeof(text), _TRUNCATE,
          "the MP3 holds %llu samples where its tag says %llu (cut short, or read another way by this Windows): "
          "the start may be off by one frame, the end is left as it is", seen, edges.Expect);
        AddNote(out.Note, text);
      }
      if (lay.Channels > 2)
      {
        _snprintf_s(text, sizeof(text), _TRUNCATE, "%d channels mixed down to 2", lay.Channels);
        AddNote(out.Note, text);
      }
      if (lay.Float || lay.Bits != 16)
      {
        _snprintf_s(text, sizeof(text), _TRUNCATE, "%d-bit%s samples made 16-bit", lay.Bits, lay.Float ? " float" : "");
        AddNote(out.Note, text);
      }
      out.Seconds = (double)moments / lay.Rate;
      out.Result = 0;
      return true;
    }
  }

  bool DecodeAs(std::wstring const& path, double maxSeconds, Ask ask, bool dropEdges, Sound& out, std::string& error)
  {
    bool ok = false;
    try
    {
      out = Sound();
      error.clear();
      ok = Read(path, maxSeconds, ask, dropEdges, out, error);
    }
    catch (std::bad_alloc const&)
    {
      out.Result = (long)E_OUTOFMEMORY;
      try
      {
        error = "Not enough memory for this sound.";
      }
      catch (...)
      {
      }
    }
    catch (...)
    {
      out.Result = (long)E_UNEXPECTED;
      try
      {
        error = "Reading the sound file failed unexpectedly.";
      }
      catch (...)
      {
      }
    }
    if (!ok)
    {
      // No half-read sound is handed back
      std::vector<short>().swap(out.Pcm);
      out.Rate = 0;
      out.Channels = 0;
      out.Seconds = 0.0;
    }
    return ok;
  }

  bool Decode(std::wstring const& path, double maxSeconds, Sound& out, std::string& error)
  {
    return DecodeAs(path, maxSeconds, AskPcm16, true, out, error);
  }

  bool Hold(std::string& error)
  {
    try
    {
      error.clear();
      Platform const* mf = GetPlatform(error);
      if (!mf)
        return false;
      HRESULT hr = mf->Startup(MF_VERSION, MFSTARTUP_LITE);
      if (FAILED(hr))
      {
        error = Fail("starting Media Foundation", hr);
        return false;
      }
      // Counted, so that an Unhold with no Hold can't stop Media Foundation
      // under a running decode
      std::lock_guard<std::mutex> lock(g_PlatformLock);
      ++g_Holds;
      return true;
    }
    catch (...)
    {
      return false;
    }
  }

  void Unhold()
  {
    HRESULT(STDAPICALLTYPE* shutdown)() = nullptr;
    {
      std::lock_guard<std::mutex> lock(g_PlatformLock);
      // Only a Hold that is still open is undone
      if (g_Platform.Ok && g_Holds > 0)
      {
        --g_Holds;
        shutdown = g_Platform.Shutdown;
      }
    }
    // Outside the lock: the last MFShutdown of the process waits for Media
    // Foundation's own threads
    if (shutdown)
      shutdown();
  }
}
