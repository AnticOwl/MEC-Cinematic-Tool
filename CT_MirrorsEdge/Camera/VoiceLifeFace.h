#pragma once
#include "VoiceFace.h"

// The life around the mouth laid on a character's bones: a blink on the eyelid bones, a brow raise on the eyebrow
// bones, and the head's own small movement on the Head bone (a part of it on Neck1), as more offsets of the kind
// VoiceFace makes for the mouth. Plain maths: no game, no ImGui.
//
// What it is made from (2026-11-12, measured in the tools' own saved recordings of the game, people recorded with
// their face bones: two men and four KrugerSec, five blinks; and tried on those people's own skins):
// - The game's blink is the UPPER lid alone: its bone's skin turns 26 - 39 degrees about a line that runs along the
//   face's right, 4 - 7 mm BEHIND the eye bone and 1 - 4 mm under it. The lower lid bone does not rise (in one
//   arrangement it goes 1.4 mm towards the nose). Closing takes about 70 - 110 ms, opening 120 - 310 ms.
// - The game keeps the lid bones in one of two arrangements, and changes between them on the same body: ON THE
//   LID'S EDGE (13 - 15 mm from the eye bone, as in the skeletons' rest poses: Faith's face in play is always so),
//   where a blink takes the upper lid bone round the eye down to the lower lid bone; or AT THE EYE'S MIDDLE (2 - 5 mm
//   from the eye bone), where the lid bone only turns. Both are handled: the blink is given as one turn about the
//   line, which moves the lid's skin the same way wherever the bone sits.
// - The head's own axes are NOT the mouth's (VoiceFace's Up leans 23 - 38 degrees forward of the head's): here Right
//   runs from the left eye bone to the right one, Up from the Head bone to HeadEnd (straight up in the rest
//   poses), Forward to the nose.
namespace voice_face
{
  struct LifeRig
  {
    bool Ok = false;                       // at least one part below is there
    bool Eyes = false;                     // the four eyelid bones: blinks
    bool Brows = false;                    // the six eyebrow bones: brow raises
    // [CHECK 2] (off too when the eye bones lie the other way round against the nose: left and right not known)
    bool Head = false;                     // the Head bone: nods, turns, tilts
    int Face = -1, HeadBone = -1;
    int Neck1 = -1;                        // the Head's parent when it is Neck1 (-1: the Head takes the whole turn)
    int EyeBone[2] = { -1, -1 };           // [ALIVE] LeftEye, RightEye (on the Face bone; the axes are taken from them)
    int UpLid[2] = { -1, -1 };             // LeftUpEyelid, RightUpEyelid
    int LowLid[2] = { -1, -1 };            // LeftLowEyelid, RightLowEyelid
    int Brow[6] = { -1, -1, -1, -1, -1, -1 };   // Left Inner, Middle, Outer; Right Inner, Middle, Outer
    int Fold[4] = { -1, -1, -1, -1 };      // Left LowMiddle, LowOuter; Right LowMiddle, LowOuter Eyebrow (-1: not there)
    // The head's own axes in the Face bone's frame, from the pose the rig was found in:
    float Right[3] = { 0.f, 0.f, 0.f };    // from the left eye bone to the right one: the character's own right
    float Up[3] = { 0.f, 0.f, 0.f };       // from the Head bone to HeadEnd, square to Right
    float Forward[3] = { 0.f, 0.f, 0.f };  // out of the face
    // The sign of a turn about Right / Up / Forward that does what the shape's value says (proven by its effect)
    float NodSign = 1.f;                   // about Right: the front (chin, nose) goes down
    float TurnSign = 1.f;                  // about Up: the nose goes to the character's right
    float TiltSign = 1.f;                  // about Forward: the top of the head goes to the character's right
    // Each eye's blink (left, right), in the Face bone's frame:
    bool LidOnEdge[2] = { false, false };  // its lid bones sit on the lid's edge (not at the eye's middle)
    // [MERGE 1] How far on the edge (0 at the eye's middle .. 1 on the lid's edge): the two ways of blinking are mixed
    // over LidEdgeFrom - LidEdgeTo about LidOnEdgeDistance, not switched at it (the rig is found again every frame, and a switch
    // made the lid jump 6 - 12 degrees in one frame where its bones cross 8 mm). LidOnEdge: this is 0.5 or more.
    float LidEdge[2] = { 0.f, 0.f };
    float LidPlace[2][3] = {};             // the upper lid bone's place when the rig was found
    float LidLine[2][3] = {};              // a point of the line (along Right) the upper lid's skin turns about
    float LidAngle[2] = { 0.f, 0.f };      // that turn for a whole blink (radians)
    float LowLidMove[2][3] = {};           // the lower lid bone's move for a whole blink
    float Neck1Q[4] = { 0.f, 0.f, 0.f, 1.f };   // Neck1's local rotation when the rig was found
  };

  // The bones found by name (names and parents as the skeleton has them; locals: bones x 8, the pose the character
  // has). A part whose bones are missing is off; false, Ok false: no part is there (a drone, the low-detail people:
  // no Face bone, no eye bones to take the axes from), or the bones lie where no axes can be made of them.
  bool FindLifeRig(int bones, char const* const* names, short const* parents, float const* locals, LifeRig& rig);

  // What the voice's analysis gives for one moment (voice_life::Frame)
  struct LifeShape
  {
    float Blink = 0.f;    // 0 the eyes as posed .. 1 shut
    float Brow = 0.f;     // 0 at rest .. 1 a clear raise
    float Nod = 0.f;      // radians: > 0 the chin goes down
    float Turn = 0.f;     // radians: > 0 the nose goes to the character's own right
    float Tilt = 0.f;     // radians: > 0 the top of the head goes to the character's own right
    // [ALIVE] Each side its own (VoiceAlive: no two blinks or raises alike, the two sides a little apart): used in
    // place of Blink and Brow when Sided. [0] the character's own left, [1] its right.
    bool Sided = false;
    float BlinkSide[2] = { 0.f, 0.f };    // 0 .. 1
    float BrowSide[2] = { 0.f, 0.f };     // MinBrowSide (a touch lowered) .. MaxBrowSide
    // [ALIVE] The upper lid following its eye (radians about the blink's line, in the blink's sense: > 0 down with the
    // eye looking down, < 0 up), laid with the blink: the lid turns Follow + Blink x (its whole way - Follow), so a
    // blink still ends shut from wherever the look has put the lid. Cut by LidFollowCut. 0: none (as before).
    float LidFollow[2] = { 0.f, 0.f };
  };

  // The user's amounts: 1 as designed, 0 none, up to 2 (a blink never closes past shut)
  struct LifeAmounts
  {
    float Blinks = 1.f;
    float Brows = 1.f;
    float Head = 1.f;
  };

  // The shape's offsets APPENDED to out (from out.Count on, never over MaxDriven; never one of the mouth's bones; a
  // part that does nothing this moment adds nothing). headLocalQ, faceLocalQ: the Head's and the Face bone's local
  // rotations as they are posed now, before this layer. neck1LocalQ: Neck1's, if the caller has it (else the one
  // the rig was found with is used: it only carries the axis of Neck1's small share).
  // [CHECK 3] The rotations are made unit length here; a head or face rotation that is no rotation (not numbers,
  // or far from unit length) leaves the head alone, a Neck1 rotation that is none is taken as not handed.
  // [CHECK 5] HAND Neck1's live rotation whenever the neck can be posed after the rig was found: measured with
  // Neck1 turned 25 degrees by the pose and not handed, a nod came out 3.5 degrees off its axis, a turn 6.4, a
  // tilt 5.9, and 0.6 - 1.8 % short; handed, exact.
  // [ALIVE] parts: which of the three to lay (the lids are laid after the eyes are turned: the lids follow them)
  const int LifePartLids = 1, LifePartBrows = 2, LifePartHead = 4, LifePartsAll = 7;
  void AddLifeOffsets(LifeRig const& rig, LifeShape const& shape, LifeAmounts const& amounts, float const* headLocalQ,
    float const* faceLocalQ, Offsets& out, float const* neck1LocalQ = nullptr, int parts = LifePartsAll);

  // [ALIVE] What AddLifeOffsets takes of the shape for an eye (0 left, 1 right): the blink (0 .. 1, after the
  // amount), and the lid's follow cut against that eye's whole way (radians)
  float LifeBlinkOf(LifeShape const& shape, LifeAmounts const& amounts, int eye);
  // A lid's follow cut: down at most LidFollowShare of the eye's way (no look down alone shuts an eye); up (< 0) at
  // most LidFollowUpMost and at most LidFollowShare of the way too (a nearly shut eye isn't opened: [REVIEW 1]); 0 for
  // what isn't a number, and for an eye with no way left (shut). way: the eye's whole way to shut (radians)
  float LidFollowCut(float follow, float way);

  // What the designed moves are made of (metres, radians). [m]: measured in the recordings, [d]: designed.
  // [m] A lid bone this far from the eye bone sits on the lid's edge (13 - 15 mm there, 2 - 5 mm at the middle)
  const float LidOnEdgeDistance = 0.008f;
  // [d] [MERGE 1] The band over which the two arrangements' blinks are mixed: 6.5 - 9.5 mm, about LidOnEdgeDistance
  // (the game's own lid bones: 2.1 - 5.1 mm at the middle, 12.8 - 15.3 mm on the edge, so no recorded pose is in
  // it). Under its start: the middle's blink exactly; from its end on: the edge's exactly.
  const float LidEdgeFrom = 0.0065f;
  const float LidEdgeTo = 0.0095f;
  // [m] The blink's line against the eye bone: 1 mm under it and 4 mm behind it (two men, both arrangements)
  const float LidLineUp = -0.001f;
  const float LidLineForward = -0.004f;
  // [m] KrugerSec with the lid bone 5 mm from the eye bone (a mix of the two arrangements): 4.3 mm under, 7 mm behind
  const float LidLineFarUp = -0.0043f;
  const float LidLineFarForward = -0.007f;
  // [d] Lid bone at the middle: the first line up to LidLineNear from the eye bone, the second from LidLineFar on,
  // mixed in between
  const float LidLineNear = 0.0025f;
  const float LidLineFar = 0.0045f;
  // [m] On the edge: the turn is this much of the angle between the two lid bones seen from the line (the game's
  // upper lid bone went 1.07 - 1.19 of the angle between them round the eye bone)
  const float LidSweep = 1.1f;
  // [m] At the middle: 35 degrees (the game: 33 - 39)
  const float LidPivotAngle = 0.611f;
  // [d] No blink turns a lid further than 60 degrees
  const float LidAngleMost = 1.05f;
  // [m] On the edge: the lower lid bone goes this far towards the nose (it does not rise)
  const float LowLidNasal = 0.0014f;
  // [d] Brow 1 lifts the middle eyebrow bone 3 mm (the game's own raises: 1.3 - 3.5 mm)
  const float BrowLift = 0.003f;
  // [m] Inner, middle, outer: their share of that (the means of two kinds of people)
  const float BrowShare[3] = { 0.65f, 1.f, 0.9f };
  // [m] The fold bones over the upper lid follow by this share (KrugerSec 0.29 - 0.55, a man 0)
  const float BrowFoldShare = 0.35f;
  // [d] Of the head's turn given to Neck1 (in the game's idle the Head bone carries 36 - 49 % of what Neck, Neck1
  // and Head turn together)
  const float HeadNeckShare = 0.3f;
  // [d] A nod, turn or tilt is cut at 20 degrees before the amount
  const float HeadMost = 0.35f;
  // [ALIVE] Each side's brow (VoiceAlive's own range: its slow tone goes a little under rest)
  const float MinBrowSide = -0.15f, MaxBrowSide = 1.25f;
  // [ALIVE] [d] The lid's follow: up at most 17 degrees (VoiceAlive's MaxLidFollow), down at most 0.8 of the way
  const float LidFollowUpMost = 0.297f;
  const float LidFollowShare = 0.8f;
}
