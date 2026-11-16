#include "FrameShot.h"
#include "../Util/Util.h"

#include <atomic>
#include <mutex>
#include <thread>
#include <vector>
#include <wincodec.h>
#include <wrl/client.h>

#pragma comment(lib, "windowscodecs.lib")

using Microsoft::WRL::ComPtr;

namespace
{
  std::mutex g_Mutex;
  std::wstring g_Path;                 // under g_Mutex: the frame wanted, empty for none
  std::atomic<bool> g_Writing{ false };

  // 24-bit BGR rows (what the PNG is written from) from one of the swapchain's
  // formats; false for any other
  bool ToBgr(DXGI_FORMAT format, BYTE const* pRow, BYTE* pOut, UINT width)
  {
    switch (format)
    {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
      for (UINT x = 0; x < width; ++x, pRow += 4, pOut += 3)
      {
        pOut[0] = pRow[2];
        pOut[1] = pRow[1];
        pOut[2] = pRow[0];
      }
      return true;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8X8_TYPELESS:
    case DXGI_FORMAT_B8G8R8X8_UNORM:
    case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
      for (UINT x = 0; x < width; ++x, pRow += 4, pOut += 3)
      {
        pOut[0] = pRow[0];
        pOut[1] = pRow[1];
        pOut[2] = pRow[2];
      }
      return true;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
    case DXGI_FORMAT_R10G10B10A2_UNORM:
      for (UINT x = 0; x < width; ++x, pRow += 4, pOut += 3)
      {
        UINT v;
        memcpy(&v, pRow, 4);
        pOut[0] = (BYTE)(((v >> 20) & 0x3FF) >> 2);
        pOut[1] = (BYTE)(((v >> 10) & 0x3FF) >> 2);
        pOut[2] = (BYTE)((v & 0x3FF) >> 2);
      }
      return true;
    default:
      return false;
    }
  }

  // On its own thread (a 4K PNG takes a moment)
  void WritePng(std::wstring path, std::vector<BYTE> pixels, UINT width, UINT height)
  {
    HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    HRESULT hr;
    {
      ComPtr<IWICImagingFactory> pFactory;
      ComPtr<IWICStream> pStream;
      ComPtr<IWICBitmapEncoder> pEncoder;
      ComPtr<IWICBitmapFrameEncode> pFrame;
      hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(pFactory.GetAddressOf()));
      if (SUCCEEDED(hr)) hr = pFactory->CreateStream(pStream.GetAddressOf());
      if (SUCCEEDED(hr)) hr = pStream->InitializeFromFilename(path.c_str(), GENERIC_WRITE);
      if (SUCCEEDED(hr)) hr = pFactory->CreateEncoder(GUID_ContainerFormatPng, nullptr, pEncoder.GetAddressOf());
      if (SUCCEEDED(hr)) hr = pEncoder->Initialize(pStream.Get(), WICBitmapEncoderNoCache);
      if (SUCCEEDED(hr)) hr = pEncoder->CreateNewFrame(pFrame.GetAddressOf(), nullptr);
      if (SUCCEEDED(hr)) hr = pFrame->Initialize(nullptr);
      if (SUCCEEDED(hr)) hr = pFrame->SetSize(width, height);
      WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR;
      if (SUCCEEDED(hr)) hr = pFrame->SetPixelFormat(&format);
      if (SUCCEEDED(hr) && format != GUID_WICPixelFormat24bppBGR) hr = E_FAIL;
      if (SUCCEEDED(hr)) hr = pFrame->WritePixels(height, width * 3, (UINT)pixels.size(), pixels.data());
      if (SUCCEEDED(hr)) hr = pFrame->Commit();
      if (SUCCEEDED(hr)) hr = pEncoder->Commit();
    }
    if (SUCCEEDED(co))
      CoUninitialize();
    char name[MAX_PATH * 2];
    WideCharToMultiByte(CP_UTF8, 0, path.c_str(), -1, name, sizeof(name), nullptr, nullptr);
    if (SUCCEEDED(hr))
      util::log::Write("Frame picture: %s saved (%u x %u)", name, width, height);
    else
    {
      // (the stream made the file before anything was written: none left half-written, 2026-11-15)
      DeleteFileW(path.c_str());
      util::log::Warning("Frame picture: %s couldn't be written (HRESULT 0x%X)", name, hr);
    }
    g_Writing = false;
  }
}

void frameshot::Request(std::wstring const& path)
{
  std::lock_guard<std::mutex> lock(g_Mutex);
  g_Path = path;
}

void frameshot::Cancel()
{
  std::lock_guard<std::mutex> lock(g_Mutex);
  g_Path.clear();
}

bool frameshot::Busy()
{
  std::lock_guard<std::mutex> lock(g_Mutex);
  return !g_Path.empty() || g_Writing.load();
}

void frameshot::OnPresent(IDXGISwapChain* pSwapChain, ID3D11Device* pDevice, ID3D11DeviceContext* pContext)
{
  std::wstring path;
  {
    std::lock_guard<std::mutex> lock(g_Mutex);
    if (g_Path.empty() || g_Writing.load())
      return;
    path.swap(g_Path);
    g_Writing = true;
  }
  auto fail = [&](char const* why, HRESULT hr)
  {
    util::log::Warning("Frame picture: not taken, %s (HRESULT 0x%X)", why, hr);
    g_Writing = false;
  };
  if (!pSwapChain || !pDevice || !pContext)
    return fail("no swapchain", E_POINTER);
  ComPtr<ID3D11Texture2D> pBack;
  HRESULT hr = pSwapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)pBack.GetAddressOf());
  if (FAILED(hr) || !pBack)
    return fail("no back buffer", hr);
  D3D11_TEXTURE2D_DESC desc{};
  pBack->GetDesc(&desc);
  if (desc.SampleDesc.Count != 1 || desc.ArraySize != 1)
    return fail("the frame is multisampled or layered", E_FAIL);
  D3D11_TEXTURE2D_DESC staging = desc;
  staging.BindFlags = 0;
  staging.MiscFlags = 0;
  staging.MipLevels = 1;
  staging.Usage = D3D11_USAGE_STAGING;
  staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  ComPtr<ID3D11Texture2D> pCopy;
  hr = pDevice->CreateTexture2D(&staging, nullptr, pCopy.GetAddressOf());
  if (FAILED(hr))
    return fail("no copy could be made", hr);
  pContext->CopyResource(pCopy.Get(), pBack.Get());
  D3D11_MAPPED_SUBRESOURCE mapped{};
  hr = pContext->Map(pCopy.Get(), 0, D3D11_MAP_READ, 0, &mapped);
  if (FAILED(hr))
    return fail("the copy couldn't be read", hr);
  std::vector<BYTE> pixels((size_t)desc.Width * desc.Height * 3);
  bool known = true;
  for (UINT y = 0; y < desc.Height && known; ++y)
    known = ToBgr(desc.Format, (BYTE const*)mapped.pData + (size_t)y * mapped.RowPitch, pixels.data() + (size_t)y * desc.Width * 3,
      desc.Width);
  pContext->Unmap(pCopy.Get(), 0);
  if (!known)
    return fail("the frame's format isn't one the tools read", (HRESULT)desc.Format);
  std::thread(WritePng, path, std::move(pixels), desc.Width, desc.Height).detach();
}
