#pragma once
#include "VoiceFace.h"
#include "VoiceLifeFace.h"
#include "VoiceFeelTables.h"

// A feeling on the talking face: facial action units (FACS, Ekman and Friesen) made of the GAME'S OWN face poses
// (its FacePoseLibraryAsset, VoiceFeelTables.h), laid as more offsets of the kind VoiceFace makes for the mouth and
// VoiceLifeFace for the lids, brows and head. Plain maths: no game, no ImGui. PROTOTYPE (2026-11-13), not in the
// project.
//
// How the game's poses are laid (as the game does it, measured by the game-data research: a face = its library's
// bind pose + the sum of weight x each pose's per-bone offsets, rotations as exp(sum of weight x log); bind
// rotations are the identity on every face bone): per bone, the moves of the poses add (mm along the head's Right /
// Up / Forward, scaled by the face's size against the library's rest face) and their rotation vectors add. A move
// is a LOCAL one (in the bone's parent's frame, as the game's are): a bone on the jaw moves with the jaw.
// Axes: VoiceLifeFace's (Right eye to eye, Up Head to HeadEnd, Forward to the nose), never the mouth's leaning ones.
//
// The lids: the game's lid poses are made on lid bones that sit on the lid's EDGE (Faith's and the man's rest
// poses). On a face whose lid bones sit at the eye's MIDDLE (the game's other arrangement), the same pose is laid as
// a turn round the blink's line by the share of a whole blink it makes on its own rest face; between the two
// arrangements both are mixed by VoiceLifeFace's LidEdge. The feeling never closes an eye past FeelLidMostClosed of
// its way; where the life's blink is in the layer, the blink is laid again from where the feeling put the lids, so a
// squint still shuts in a blink and a wide eye shuts too (and never further than the lids' own meeting x LidSweep).
//
// Laid with the mouth's and the life's offsets (AddFeelOffsets after MakeOffsets and AddLifeOffsets): a bone already
// in the layer takes the feeling in the SAME entry, never a second one (the pose editor keeps a bone's first entry
// only): the mouth's corners and lips: the speech move + the feeling's move, the feeling's turn on the speech's;
// the life's brows and folds: the feeling's set + RaiseKeep x the life's raise; the life's lids: see above; the
// life's Head and Neck1: the feeling's nod and tilt turned on top of the life's.
namespace voice_face
{
  // The action units made here. The jaw (AU 25 / 26 / 27) is the mouth's (a feeling's own jaw: FeelShape::Jaw, which
  // PoseEditor lays through the mouth's offsets); the eyes' turns are another layer's.
  enum FeelAu
  {
    Au1,    // inner brow raiser
    Au2,    // outer brow raiser
    Au4,    // brow lowerer (down and together)
    Au5,    // upper lid raiser
    Au6,    // cheek raiser (the lower lid pushed up, the outer brow pulled down a little)
    Au7,    // lid tightener (the lower lid up and tight)
    Au9,    // nose wrinkler
    Au10,   // upper lip raiser
    Au12,   // lip corner puller (the smile)
    Au14,   // dimpler (corners back and tight: an approximation, the game has no such pose)
    Au15,   // lip corner depressor
    Au16,   // lower lip depressor
    Au17,   // chin raiser
    Au20,   // lip stretcher
    Au23,   // lip tightener
    Au24,   // lip pressor
    Au41,   // lid droop (the upper lid lowered, relaxed)
    AuCount
  };
  extern const char* const FeelAuNames[AuCount];

  // One game pose in an action unit at weight 1
  struct FeelMix
  {
    int Pose;      // voice_feel_tables::PoseId
    float Weight;
  };
  // An action unit's game poses (at most FeelMixMost)
  const int FeelMixMost = 4;
  struct FeelAuMix
  {
    int Count;
    FeelMix Mix[FeelMixMost];
  };
  extern const FeelAuMix FeelAuMixes[AuCount];

  struct FeelShape
  {
    float Left[AuCount];     // each 0 none .. 1 the unit at its designed full size, up to 2 (never a game pose past 1)
    float Right[AuCount];    // the character's own right
    float Nod;               // radians, > 0 the chin goes down (laid on the life's head turn); cut at FeelHeadMost
    float Tilt;              // radians, > 0 the top of the head to the character's own right
    float RaiseKeep;         // 0 - 1: the share of the life's brow raises kept (an angry face keeps few)
    float Jaw;               // [JAW] 0 - 1: the jaw down under the speech's own, in the mouth's Open (PoseEditor lays it)
    FeelShape();
  };

  struct FeelRig
  {
    bool Ok = false;
    int Library = 0;                         // voice_feel_tables::LibraryFaith / LibraryMan
    float Scale = 1.f;                       // this face's size against the library's rest face (moves x this)
    LifeRig Life;                            // the head's axes, the lids, the brows, the head (found on the same pose)
    int Bone[voice_feel_tables::BoneCount];  // each of the tables' bones on this skeleton (-1: not there)
    bool Lids[2] = { false, false };         // this eye's two lid bones are there on the Face bone
    float LowLidPlace[2][3] = {};            // the lower lid bones' places (the upper ones: Life.LidPlace)
    int LidName[2][2] = { { -1, -1 }, { -1, -1 } };   // the tables' BoneNames of each eye's upper and lower lid
    // [SKEPTIC FIX 1] How far the POSE itself has each upper lid turned shut (radians about the head's right, in the
    // blink's sense, against the identity: the game's bind rotation of every face bone). Lid bones at the eye's
    // middle are shut by turning, which their places don't show, so the life's LidAngle stays a whole 35 degrees on
    // an eye the taken pose has shut (measured: the recorded people r0_p00 / r2_p02 at the peak of a game blink,
    // 32 - 35 degrees; open samples 3.3 degrees at most).
    float PoseClosed[2] = { 0.f, 0.f };
  };

  // The bones found by name (names and parents as the skeleton has them; locals: bones x 8, the pose the character
  // has). library: which of the game's libraries to take the shapes from (Faith's for Faith, the man's for the
  // others). False, Ok false: no face (no Face bone, no eye bones, no HeadEnd), or left and right can't be told.
  bool FindFeelRig(int bones, char const* const* names, short const* parents, float const* locals, int library, FeelRig& rig);

  // The feeling's offsets laid into out (see above). lifeUsed: the LifeRig the life's offsets in out were made
  // with this frame (its blink is laid again on the lids the feeling moves), null if the life made none.
  // headLocalQ, faceLocalQ, neck1LocalQ: as AddLifeOffsets takes them (for the nod and tilt only; null: no head).
  // Returns how many bones could not be laid because the layer was full (MaxDriven).
  // [MERGE 1] parts: FeelPartFace (every bone but the lids, the brows' kept share, the head) and / or FeelPartLids
  // (the lids: laid after the life's lids, which follow the eyes). blink, follow (each 2, left and right; null: as
  // before): the life's own blink of each eye (0 .. 1, after its amount: LifeBlinkOf) and the lid's follow of its eye
  // (radians, LifeShape::LidFollow): the blink is laid again from where the feeling put the lid as Follow + Blink x
  // (the way left - Follow), Follow cut by LidFollowCut against the way left. With null the blink is read back from
  // the life's lid entry (a pure turn about its Right: right only while that entry has no follow in it).
  const int FeelPartFace = 1, FeelPartLids = 2, FeelPartsAll = 3;
  int AddFeelOffsets(FeelRig const& rig, FeelShape const& shape, LifeRig const* lifeUsed, float const* headLocalQ,
    float const* faceLocalQ, float const* neck1LocalQ, Offsets& out, int parts = FeelPartsAll,
    float const* blink = nullptr, float const* follow = nullptr);

  // For the test program: the feeling's lid closing of one eye (0 open as posed .. 1 shut), as AddFeelOffsets
  // limits it.
  float FeelLidClosing(FeelRig const& rig, FeelShape const& shape, int eye);

  // [MERGE 1] For the test program and the menu: the share of the feeling's lid moves an eye takes, by how far the
  // pose has its upper lid turned shut where its lid bones sit at the middle (1 all, 0 none: the eye counts as shut;
  // [SKEPTIC FIX 1])
  float FeelPoseLidShareOf(FeelRig const& rig, int eye);
  // [FIX CHECK] For the test program: the share of the feeling's lid moves an eye nearly shut takes (1: all)
  float FeelNearShutShareOf(FeelRig const& rig, int eye);

  // [d] designed limits
  const float FeelLidMostClosed = 0.65f;   // the feeling alone never closes an eye more than this share of its way
  const float FeelShutAngle = 0.02f;       // an eye whose blink has less way left than this (radians) is shut by the pose:
                                           // the feeling leaves its lids alone
  const float FeelShutBand = 0.1f;         // [FIX CHECK] [d] and over this much more way (5.7 degrees) its lid moves fade
                                           // in from none, so they hand over to the eye left alone without a jump
  // [SKEPTIC FIX 1] [d] lid bones at the middle: the feeling's lid moves are laid in full while the pose has the
  // upper lid turned shut by no more than this (8 degrees; open samples: 3.3 at most), fade to none at a whole
  // middle blink (LidPivotAngle) less FeelShutAngle, and the eye counts as shut from there
  const float FeelPoseOpenTurn = 0.14f;
  const float FeelHeadMost = 0.07f;        // 4 degrees: the feeling's nod or tilt
  const float FeelScaleLeast = 0.7f, FeelScaleMost = 1.4f;   // the face's size against the library's (real: 0.84 - 1.06)
  // [d] the bones this layer can add to the mouth's 9 and the life's 16 (the tables' 49 bones, 22 of them shared)
  const int FeelNewBones = 27;
  // [MERGE 1] and the two eye bones VoiceGaze turns
  const int FeelEyeBones = 2;
}
