#pragma once

// A voice's mouth shapes laid on a character's face bones: which bones make the mouth, the face's own axes from where
// those bones are, and each shape as a turn of the jaw and moves of the lips and the mouth's corners. Plain maths:
// no game, no ImGui. A local pose is 8 floats a bone: translation x y z, 1, rotation x y z w (the pose editor's).
//
// The rig (2026-11-12, the game's SkeletonAssets): Faith, KrugerSec, the men and the women share the same 57 face
// bones. The Jaw hangs on the Face bone at the real hinge (the lower lip 98 - 104 mm from it: 10 degrees of jaw move
// the lower lip 16 - 18 mm) and carries LeftLowerLip, LowerLip, RightLowerLip, the tongue and the chin. LeftUpperLip,
// UpperLip, RightUpperLip and the corners LeftMouth, RightMouth hang on the Face bone itself. The inner lips follow
// their lips. The mouth is 46 - 52 mm wide.
namespace voice_face
{
  const int FloatsPerBone = 8;
  // (the mouth's 9 bones, VoiceLifeFace's 16: four eyelids, six eyebrows, four folds, Head, Neck1; the feeling's 27
  // more, VoiceFeelFace; and the two eyes, VoiceGaze: 54. The layer is only kept in memory, never saved: its size
  // can change freely)
  const int MaxDriven = 64;

  struct Rig
  {
    bool Ok = false;
    int Face = -1, Jaw = -1;
    int Lower[3] = { -1, -1, -1 };     // LeftLowerLip, LowerLip, RightLowerLip (on the jaw)
    int Upper[3] = { -1, -1, -1 };     // LeftUpperLip, UpperLip, RightUpperLip (on the face)
    int Corner[2] = { -1, -1 };        // LeftMouth, RightMouth (on the face)
    // In the Face bone's frame, from the pose the rig was found in:
    float Right[3] = { 0.f, 0.f, 0.f };    // from the left corner to the right one
    float Up[3] = { 0.f, 0.f, 0.f };       // from the chin to the nose, square to Right
    float Forward[3] = { 0.f, 0.f, 0.f };  // out of the face
    float Width = 0.f;                     // corner to corner (m)
    float JawSign = 1.f;                   // the jaw's turn about Right by JawSign x angle lowers the chin
    float JawToLower[3] = { 0.f, 0.f, 0.f };  // from the jaw's hinge to the middle of the lower lip
    float LowerSide[3] = { 0.f, 0.f, 0.f };   // each lower lip bone's side along Right: -1 left, 0 middle, +1 right
    float UpperSide[3] = { 0.f, 0.f, 0.f };
    float CornerSide[2] = { 0.f, 0.f };
  };

  // The mouth's bones found by name (names and parents as the skeleton has them; locals: bones x 8, the pose the
  // character has, the mouth near rest). False, Ok false: this skeleton has no such mouth (a drone, the low-detail
  // people), or its bones lie where no axes can be made of them.
  bool FindRig(int bones, char const* const* names, short const* parents, float const* locals, Rig& rig);

  // How much of each shape, each 0 - 1 (see VoiceMouth.h: what the analysis gives)
  struct Shape
  {
    float Open = 0.f;     // the jaw down: "ah"
    float Wide = 0.f;     // the corners apart: "ee"
    float Round = 0.f;    // the lips rounded and forward: "oo", "w"
    float Closed = 0.f;   // the lips pressed together: p, b, m
    float Teeth = 0.f;    // the teeth near, the lips drawn apart: f, v, s, sh
  };

  // The user's amounts: 1 as designed, 0 none, up to 2
  struct Amounts
  {
    float Jaw = 1.f;       // how far the jaw opens
    float Lips = 1.f;      // the lips' and the corners' own moves (wide, rounded, teeth, the press)
    float Closure = 1.f;   // how much of the shape's Closed is taken (it shuts the jaw and presses the lips)
  };

  struct Offsets
  {
    int Count = 0;
    int Bone[MaxDriven];
    float Turn[MaxDriven][4];   // in the bone's parent's frame: the local rotation becomes Turn x it
    float Move[MaxDriven][3];   // added to the bone's local translation
  };

  // A shape's offsets for the rig's bones. jawLocalQ: the jaw's local rotation as it is posed now, before this layer
  // (the lower lips' moves are given in the turned jaw's frame).
  void MakeOffsets(Rig const& rig, Shape const& shape, Amounts const& amounts, float const* jawLocalQ, Offsets& out);

  // What the designed shapes are made of: the jaw's angle for Open = 1 (radians), and moves as shares of the mouth's
  // width
  const float JawOpenAngle = 0.157f;      // 9 degrees: the lower lip 15 - 16 mm down
  const float JawTeethAngle = 0.026f;     // 1.5 degrees: the teeth just apart
  const float CornerFollowsJaw = 0.45f;   // the corners go down this share of the lower lip's way
  const float OpenCornerIn = 0.04f, OpenUpperUp = 0.02f;
  const float WideCornerOut = 0.11f, WideCornerBack = 0.03f, WideLipOut = 0.04f;
  const float RoundCornerIn = 0.16f, RoundCornerForward = 0.06f, RoundLipForward = 0.10f, RoundLipIn = 0.05f;
  const float ClosedLipPress = 0.015f;
  const float TeethUpperUp = 0.04f, TeethLowerDown = 0.03f, TeethCornerOut = 0.03f;
}
