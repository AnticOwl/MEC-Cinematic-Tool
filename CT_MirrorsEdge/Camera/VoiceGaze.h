#pragma once
#include "VoiceLifeFace.h"

// The eyes of a talking face: both eye bones turned to a gaze (VoiceAlive's: where the eyes look, against the head as
// posed), held in the world while the voice's own layer turns the head (the vestibulo-ocular reflex: the eyes turn
// against the head and keep their target), and how far each eye then looks down more than as the pose was taken, for
// the upper lids to follow it. Plain maths: no game, no ImGui.
//
// Why (research 2026-11-13): eyes fixed in the head, riding every nod and turn with it, read as an animatronic's
// (the human reflex: gain 1, under 10 ms: Collewijn and Smeets 2000); the game's own animation has no such
// counter-turn (eye-in-head yaw against the head's: 0.00 on 16 recorded people). The counter-turn is worked out from
// the turns the layer has ALREADY put on Neck1 and Head (the life's and the feeling's, at the user's amounts), so the
// eyes hold exactly against what is laid, not against a plan of it (a plan missed by up to 3.6 degrees at the Head
// amount 0 or 2, the research check measured). The head's other movement (the user's edits, keyframes, a clip, the
// idle movement) is the pose's: the eyes go with it, as posed.
// The target is taken as far away: a point closer than about a metre is missed by up to 1.5 - 2 degrees while the
// head turns (the eyes' places move with it; measured by the research check).
namespace voice_face
{
  // [d] An eye is turned at most this far from the head's straight ahead, the pose's turn and the voice's together
  // (the game's own look-at limits, PA2RigFeatureAsset: yaw 34 - 35 degrees); a pose already further keeps its own
  // look and isn't turned further out
  const float EyeMost = 0.611f;   // 35 degrees
  // [read: Becker and Fuchs 1988: the upper lid and the eye hold the same places on average; upward lid moves are
  // often smaller] [d] the split, as VoiceAlive's kLidFollowDown / kLidFollowUp
  const float LidFollowDown = 0.9f, LidFollowUp = 0.7f;

  // yaw, pitch: radians, the gaze against the head as posed under the layer (> 0 to the character's own right; > 0
  // down). hold: true: kept in the world against the layer's turns of Neck1 and Head already in out; false: the eyes
  // turn with the head. lay: false: nothing is laid, only lookDown is given (the eyes as posed now).
  // neck1Q, headQ, faceQ: those bones' local rotations as posed under the layer, as they are HANDED (the user's
  // edits and the idle movement in them; for the Face bone too: the eyes hang on it so [REVIEW 3]) (neck1Q: null if
  // not handed; the rig's own is used then). eyeQ[2]: each eye's local rotation as posed now (left, right);
  // takenQ[2]: the look lookDown is measured against (null: as now; the pose editor hands the eyes as posed now, so
  // the lids follow only what the voice does to the eyes [REVIEW 4]).
  // Appends the two eye bones' turns to out (never over MaxDriven), and gives each eye's look down after the turn
  // against its look as taken (radians, > 0 down): lookDown[2]; and, if lookRight isn't null, how far the turn took
  // each eye's look to the character's right against its look as posed (radians, its side angle). False: no eyes (a
  // rig without them, or a rotation that is none: not numbers, or far from unit length): nothing laid, lookDown 0.
  // A turned look past the limit is brought back onto it by its side-to-side part, its height kept (both eyes keep
  // the same up-down move) [REVIEW 2]; nearer the limit in height, mixed smoothly into the nearest point on the
  // limit, towards the front [FIX CHECK].
  bool AddGazeOffsets(LifeRig const& rig, float yaw, float pitch, bool hold, bool lay, float const* neck1Q,
    float const* headQ, float const* faceQ, float const* const eyeQ[2], float const* const takenQ[2], Offsets& out,
    float lookDown[2], float* lookRight = nullptr);

  // The upper lid's follow of an eye's look down (radians, > 0 down): LidFollowDown of it down, LidFollowUp of it up;
  // 0 for what isn't a number
  float LidFollowOf(float lookDown);
}
