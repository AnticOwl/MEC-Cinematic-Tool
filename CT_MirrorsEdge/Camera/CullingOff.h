#pragma once

// GAME > Level streaming > No culling (2026-11-14; Research_Culling_Off_2026-11-14.md in the project root).
//
// The camera's view gets the loaded meshes its culling left out: the game culls every world view with its flat mesh
// cull while it's on (MeshCullSettings.HiEnable = 0 instead of its tree cull; the game's value saved first and written
// back when it's off), and in the camera's view the occlusion test is skipped, meshes outside the view are let in
// within the view margin (the draw distance kept) and too-small ones within the small objects' range. Shadows and reflections aren't opened up (but are culled by the flat cull's tests
// while on). Particles, decals and lights aren't part of the mesh cull. A guard holds the generic visible list's count
// to its room (genProcessCull would read past it otherwise). Written while on: the cull jobs' own results (on their
// stacks), HiEnable, and that count where it's over the room.
namespace culling_off
{
  // The hooks on the main view's two cull tests (0x1436C7AD0, 0x1436C77E0) after their code and their caller's are
  // checked byte for byte (Main::Initialize). They pass everything through while it's off.
  void Init();
  bool IsAvailable();

  void SetEnabled(bool on);   // any thread (the config, the menu); it acts only where IsAvailable
  bool GetEnabled();          // the choice (saved in the config), also where it can't act
  // Meshes outside the view are let in out to this angle past its edges (0 .. 90 degrees; 89.5 or more = all around;
  // 20 by default), and the ones the game finds too small out to this distance (under 1 m = left to the game, 2000 or
  // more = any distance, the default). Any thread.
  void SetViewMargin(float degrees);
  float GetViewMargin();
  void SetSmallRange(float metres);
  float GetSmallRange();

  // Every frame from the camera update (CameraManager::OnCameraUpdate, the game's camera thread), with the render
  // view's position: the on / off lines and, while on, a line every 10 s in CT.log
  void FrameUpdate(float x, float y, float z);

  // GAME tab, Level streaming: the tick and what it did. True when it changed.
  bool DrawUI();
}
