#pragma once
#include <d3d11.h>
#include <string>

// A picture of the finished frame (as it is at Present, after the tools'
// passes, before the menu), saved as a PNG with Windows' own encoder (WIC).
// Used by VISUALS > Depth of field's test pictures.
namespace frameshot
{
  // Any thread: the next frame is saved at this path
  void Request(std::wstring const& path);
  // A request still waiting for its frame or being written
  bool Busy();
  // A request not taken yet is dropped (one being written is finished)
  void Cancel();

  // Render thread, from the Present hook, on the game's swapchain, before the menu
  void OnPresent(IDXGISwapChain* pSwapChain, ID3D11Device* pDevice, ID3D11DeviceContext* pContext);
}
