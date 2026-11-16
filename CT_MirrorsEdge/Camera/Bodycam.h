#pragma once
#include <string>

class INIReader;

// GAME > First person > First camera view: Default (the game's own first-person view) or Bodycam, the tools' own
// bodycam (the user, 2026-11-14: "lets create our own then with better and improved features and better bodycam
// movement"; an earlier port of the BODYCAM mod's logic was discarded at the user's request).
//
// The bodycam is a camera worn on Faith's body (chest, helmet or shoulder): it hangs on a strap, takes her steps
// and landings, shakes while she runs, leans into turns and can keep the horizon level, through a wide lens with
// an optional fisheye bend, REC overlay and vignette (BodycamMotion.h). Each frame it takes the game's own
// first-person camera (where you aim) and her bones' world places (her pose, read only), and writes the main
// render view's transforms and near plane and the game's ForceFov. Nothing of the game's code is called.
// Default writes nothing; on the way back the field of view and the near plane are the values read from the game
// before the bodycam wrote them. It only takes her first-person view (the game's camera on her camera joint):
// cutscenes and other cameras stay the game's; the CAMERA tab's camera wins.
namespace bodycam
{
  enum View { View_Default, View_Bodycam, View_Count };
  const char* ViewName(int view);

  // The choice ([Misc] FirstCameraView); Default at start and by default
  void SetView(int view);
  int GetView();

  // [Misc] FirstCameraView and the bodycam's look and settings (Bodycam*)
  void ReadConfig(INIReader* pReader);
  void WriteConfig(std::string& config);

  // GAME tab: the choice, the bodycam's look and settings, what it's doing. True when something changed.
  bool DrawUI();

  // At the camera hook, the main render view, every frame (CameraManager::OnCameraUpdate), game thread.
  //  pRenderView: the main render view; pRenderSettings: GameRenderSettings (ForceFov at + 0x54), 0 if none
  //  soldier: Faith's client character (0: none); freeCamera: the CAMERA tab's camera is on
  //  worldScale: game time per real second (the game's speed and slow motion; 0 while time is frozen): the
  //  bodycam's movement runs on it, and stops while the game is paused
  void Update(__int64 pRenderView, __int64 pRenderSettings, __int64 soldier, bool freeCamera, float worldScale);

  // Over the picture (UI thread, from CameraManager::DrawOverlay): REC, the time, the camera's number and the
  // vignette, while the bodycam writes the view. WantsOverlay: whether to draw them with the menu closed.
  void DrawOverlay();
  bool WantsOverlay();
}
