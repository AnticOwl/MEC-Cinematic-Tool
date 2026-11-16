#pragma once
#include <d3d11.h>

// VISUALS > Filmic effects > Lens distortion, drawn by the tools. The game
// bends the picture in its tonemap pass, but its depth of field (the Simple
// one) runs after that pass and looks the depth up where the pixel would be
// without the bend, so the blur landed beside what it should blur; and the
// tools' own passes at Present (wet surfaces, rain, snow, lightning, green
// screen) were drawn straight over a bent picture. So while the change here
// wants it, the game's own is off and the same bend (its formula, see
// LensDistortion.hlsl) is given to the finished frame at Present, after the
// tools' passes and before the Panini projection.
namespace lensdistortion
{
  // Game thread, from the visual environment update: on or off, the game's
  // LensDistortionGain and LensDistortionCubicGain
  void Set(bool on, float gain, float cubicGain);

  // Game thread, from the camera hook: GAME > First person > Bodycam's
  // fisheye (Bodycam.cpp), on while the bodycam writes the view. The same
  // bend with this gain (no cubic gain), drawn when the one above is off: the
  // VISUALS one wins.
  void SetBodycam(bool on, float gain);

  // Render thread, from the Present hook, on the game's swapchain only
  void Apply(IDXGISwapChain* pSwapChain, ID3D11Device* pDevice, ID3D11DeviceContext* pContext);
}
