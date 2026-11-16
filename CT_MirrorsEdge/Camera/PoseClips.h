#pragma once
#include <memory>
#include <string>
#include <vector>

// GAME > Pose > Animation clip: an animation made for Mixamo's rig (an FBX file, read with ufbx) played on a held
// character through the pose editor. A clip is read once into each of its standard bones' world rotation per frame
// and its Hips' place, in the game's axes (+Y up, +Z the character's front, +X its left; metres). A Fit is that
// clip on one of the game's human skeletons: per game bone the clip bone that turns it and the turn that takes the
// clip bone's rest frame to the game bone's (both rigs' rest poses: the clip's own (its skin's bind pose, else its
// default pose; a file without one gets its rest frames from the body), the game's A-pose from its SkeletonAsset,
// PoseRest.h). See Research_Pose_Editor_2026-11-08.md sections 21 - 23.
namespace pose_clips
{
  const int MaxBones = 512;               // the pose editor's (a skeleton's bones at most)

  struct Clip
  {
    std::string File;                     // its file's name (UTF-8)
    std::wstring MapPath;                 // its bone map's file (<its name>.bones.txt beside it)
    std::string Take;                     // the animation (stack) used
    double Fps = 30.0;
    int Frames = 0;
    std::vector<std::string> Bones;       // the roles found: Mixamo's names for its bones
    std::vector<std::string> SourceNames; // each one's bone in the file (its name there)
    std::string BonesFrom;                // how they were found (the names and places, or its bone map)
    // The file's axes and unit to the game's, and the turn about up that faces it +Z (offline checks)
    double Basis[3][3] = {};
    double Unit = 0.01;
    float Yaw[4] = { 0.f, 0.f, 0.f, 1.f };
    std::vector<float> RestQ;             // per bone: its world rotation at rest (x y z w)
    std::vector<float> RestP;             // per bone: its world place at rest (metres)
    std::vector<float> Q;                 // per frame, per bone: its world rotation
    std::vector<float> HipsP;             // per frame: the Hips' world place
    int Hips = -1;                        // the Hips' bone
    // Each elbow's hinge (left, right) at rest: the axis the forearm bends about, measured in the upper arm's frame
    // over the frames where it bends, carried to the rest pose (world). Not found: the knuckles stand in.
    float ElbowHinge[2][3] = {};
    bool ElbowHingeOk[2] = { false, false };
    // Each knee's hinge, the same way (the thigh's frame, the shin and foot bending)
    float KneeHinge[2][3] = {};
    bool KneeHingeOk[2] = { false, false };
    // The same hinges measured in the forearm's and the shin's own frames (carried to the rest pose, world): what
    // the forearms and shins take in a file with no rest pose
    float ElbowHingeLower[2][3] = {};
    float KneeHingeLower[2][3] = {};
    std::string RestFrom;                 // where its rest pose came from (the report)
    // A file with no rest pose (no skin, and its default pose a moving one: an armature-only export, Blender's): its
    // default pose stands in, but each bone's rest frame is taken from the body there, not from the world's axes
    bool NoRest = false;
    std::vector<float> RefSide;           // NoRest, per bone: its rest frame's side axis (world): across the hips or
                                          // chest, the chest's up or front
    std::vector<float> RefDir;            // NoRest, per bone whose aim hangs below another bone of the file: its
                                          // direction to that next bone (world); zero for the others
    std::vector<float> RefAim;            // NoRest, per bone: a head with no end bone: the chest's up (world), over the
                                          // clip in its frame (it aims at nothing else); zero for the others
    std::string UpFrom, UpTo;             // NoRest: the bones the chest's up runs between (Spine2 to Neck, ...)
    bool UpFromMap = false, UnitFromMap = false;  // its up / unit from its bone map's "Up =" / "Unit =" lines
    std::vector<float> Planted;           // NoRest, per bone: a foot's or toe's world rotation (4) where it stands
                                          // flattest, and its way ahead across the ground there (3); zero for the others
    float HipsRest[3] = {};               // the Hips' rest place (the move is from it, the scale by its height): the rest
                                          // pose's; NoRest: standing on its legs over the file's origin
    std::string Report;                   // what was read, one line (the log and the menu)
    std::string Warning;                  // empty, or what may be wrong with it
    bool FromUnity = false;               // a Unity humanoid clip (.anim): its bones are the reference's (no bone map)
    double Length() const { return Frames > 1 ? (Frames - 1) / Fps : 0.0; }
  };

  // Reads an FBX file and bakes its animation (any thread; a few hundred ms for a long clip). Null, and why, if it
  // can't be used. Its bones are found in any humanoid rig by their names and where they hang (Mixamo's names, Unreal's,
  // Unity's, Biped's, Rigify's, Daz's, VRoid's...), or by the bone map beside it (<its name>.bones.txt) if there is one.
  std::shared_ptr<Clip const> Load(std::wstring const& path, std::string& error);

  // A Unity humanoid clip (.anim, Unity's binary asset; Load hands those here, the file read): its muscles turned into
  // Mixamo's bones on a reference T-pose by an approximate model of Unity's (fitted to clips' own hand and foot IK goals),
  // the arms and legs then bent to those goals, the feet stood on the floor (PoseClipsUnity.cpp). Null and why if not.
  std::shared_ptr<Clip const> LoadUnityAnim(std::wstring const& path, std::vector<char> const& data, std::string& error);

  // The clip's bones as found, written to its bone map (to edit, then read the clip again). False and why if not.
  bool SaveBoneMap(Clip const& clip, std::string& message);

  // What a game bone gets while a clip plays
  enum BoneUse : unsigned char
  {
    UseTaken,                             // the pose taken's local (face, helpers, the weapon grip...)
    UseClip,                              // the clip's turn (its translation the game rest's, but the Hips')
    UseRest,                              // the game rest's local: a bone between two the clip turns (Neck1, a finger's 0)
    UseFollow,                            // a roll bone that follows a sibling the clip turns (ElbowRoll, KneeRoll)
    UseTwist,                             // Faith's twist bone: a share of a driven bone's turn about X (fitted rules)
  };

  struct Fit
  {
    bool Ok = false;
    int Rest = -1;                        // the rest pose used (pose_rest::Rests)
    int Driven = 0;                       // game bones the clip turns
    short Source[MaxBones];               // per game bone: the clip bone that turns it, or -1
    float Align[MaxBones][4];             // per game bone: its world rotation = the clip bone's x this
    unsigned char Use[MaxBones];          // per game bone: a BoneUse
    short Follows[MaxBones];              // UseFollow: the sibling it follows
    float FollowTurn[MaxBones][4];        // UseFollow: its local = the sibling's x this (their rest offset)
    short TwistOf[MaxBones];              // UseTwist: the bone whose turn from rest it takes a share of
    unsigned char TwistKind[MaxBones];    // UseTwist: 0 that turn's twist about X, 1 its Euler X (z y x), 2 (x y z)
    float TwistShare[MaxBones];           // UseTwist: the share
    float RestLocalQ[MaxBones][4];        // the game rest's local rotation
    float RestLocalT[MaxBones][3];        // the game rest's local translation
    int TargetHips = -1;
    float HipsScale = 1.f;                // the game rest Hips' height over the clip's
    float RestHipsY = 0.f;                // the game rest Hips' height (model frame)
    float RestHipsQ[4] = { 0.f, 0.f, 0.f, 1.f };  // the game rest Hips' rotation (model frame)
    std::string Report;
  };

  // The clip on a game skeleton (its SkeletonAsset, bones, names and parents, as pose_skeletons has them).
  // liveLocal (the character's local pose, 8 floats a bone, may be null) picks between rest poses that share a bone
  // list.
  bool FitTo(Clip const& clip, char const* asset, int bones, char const* const* names, short const* parents,
    float const* liveLocal, Fit& fit);

  // The clip at a time (seconds; looped or held at its ends): each driven game bone's world rotation in the rest
  // pose's model frame (into world, per game bone), and the Hips' move from the clip's rest place, scaled to the
  // game skeleton (model frame, metres)
  void Sample(Clip const& clip, Fit const& fit, double time, bool loop, float (*world)[4], float* hipsMove);
}
