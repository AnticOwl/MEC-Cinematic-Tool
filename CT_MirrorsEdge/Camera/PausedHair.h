#pragma once
#include <cstdint>

// GAME > Pose > "Her hair moves while paused": Faith's cloths (her hair) keep
// simulating while time is frozen (Research_Pose_Editor_2026-11-08.md,
// section 16).
//
// Why it stops: the game steps every cloth from its physics step
// (EAClothSimUpdater, vtable 0x141AB68D8, slot 3 = 0x142FA2720 -> the cloth
// manager's 0x142FA3A10), and Freeze time sets the time scale to 0, so there
// are no physics steps. Each frame the game only interpolates the last two
// steps (EAClothUpdater, 0x142FA1960), so a head posed while frozen left the
// hair where it was.
//
// What the tools do while frozen: for each cloth of hers that is in the game's
// cloth list, at full detail (LOD 0), with none of the game's cloth jobs
// running, the game's own two per-cloth steps, on the game's thread: the
// pre-update (0x142FA2760: her root's place) and the sim job's body
// (0x142FA07A0, handed a job record that lists her cloths only: anchors from
// her pose as it's drawn, the world's collision, the EA step, the result to
// the drawn mesh). The EA step takes the time since its last step itself
// (0x143B7C5A0 keeps it at + 0xE0), so the tools step at 60 Hz of real time.
namespace paused_hair
{
  // Every frame on the game's thread (from pose_editor::Update): Faith's
  // soldier (0: none), whether time is frozen, whether the tick is on
  void Update(__int64 soldier, bool timeFrozen, bool wanted);

  // For the menu: what it did last ("" when nothing yet)
  void Status(char* out, size_t size);

  // Every frame on the game's thread: her hair reset when a clip jumps
  // (2026-11-11: a looping root-motion clip moved her head up to 3 m in one
  // frame, and her simulated hair trailed after it). jumpNow: the pose jumped
  // this frame. The game's own cloth reset flag (+ 0x18A, what its reset
  // method sets) is held on her cloths for 120 ms, so the game's next cloth
  // step puts the hair at its skinned places, still, before it simulates on.
  // gameScale: the game's time scale (the hold is as long in game time
  // when it's under 1; frozen, 0: the tools' own step runs in real time).
  void KeepReset(__int64 soldier, bool jumpNow, float gameScale);

  // A read-only log of her hair cloths while she's posed, with [Debug]
  // Diagnostics = true only (2026-11-11: on the 24 fps clips her hair "doesn't
  // really follow"): per cloth, its detail, its off and reset flags, how often
  // its root moves and how fast, the EA step's time; next to how far her posed
  // head moved. Once a second while a clip is on, every 5 s otherwise, and when
  // the clip changes; nothing while she isn't posed (or her pose isn't handed).
  struct PoseState
  {
    bool Held = false;            // Faith is posed
    bool ClipOn = false;          // a clip makes the pose
    bool Playing = false;
    char File[96] = "";
    double Fps = 0.0;
    double Time = 0.0;            // s into the clip
    bool HeadOk = false;
    float Head[3] = {};           // her head's place as posed (model frame)
  };
  void Watch(__int64 soldier, bool timeFrozen, PoseState const& pose);
}
