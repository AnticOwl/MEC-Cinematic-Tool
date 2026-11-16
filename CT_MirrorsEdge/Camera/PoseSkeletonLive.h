#pragma once
#include "PoseSkeletons.h"

// A body's skeleton as the game itself has it, for the pose editor (the user, 2026-11-12: "find a way to find every
// skeleton ... so we can fix the 'Skeleton not know: can't be posed' issue"; Research_Pose_Editor section 29).
//
// What was wrong: the editor knew a body's skeleton by comparing the body's parent list (its pose manager's, + 0xF0)
// with its own tables. But the game doesn't keep the skeleton's parents as they are: when it makes a pose manager it
// writes "no parent" (-1) for every bone the body's animation rig has no channel for (decompiled: 0x142FC9197).
// A body with such a bone never matched, though its skeleton was in the tables. By the game's data that is every
// man and every woman in heels (their bone 161, BlendshapeDriver / FacePosesAnimation, is in no rig).
//
// What this does: reads, once, when a body is listed or taken,
//  - the body's own parent list (what the game's pose pass uses: the editor must use the same);
//  - which bones have a rig channel. Only those may be listed in a pose override: the game's blend writes a
//    channel-less bone's pose into channel 0, over another bone (decompiled: 0x142FCF743, 0x142FC921E);
//  - the SkeletonAsset the manager was made from (manager + 0x00): its name and its bones' names. They are taken
//    only when proven: the asset's vtable, as many names as bones, and every name hashing (FNV-1) to the hash the
//    game keeps beside it. This was decompiled, not yet read from a running game: if anything doesn't hold the
//    names are left out and the tables are used alone (the log says which check failed).
// Everything is copied: nothing here points into the game afterwards. Reads are bounded and guarded.
namespace pose_skeleton_live
{
  const int MaxBones = 512;              // the pose editor's
  const int NameBytes = 64;              // the longest bone name in the game's data has 38 characters
  const int AssetBytes = 160;

  struct Body
  {
    int Bones;
    bool ParentsOk;                      // readable, every parent -1 or a bone before its child
    bool ParentsCounted;                 // the manager's parents list is as long as the bone count (begin / end)
    short Parents[MaxBones];             // the pose manager's
    // The rig's channels (manager + 0x88, + 0xB0, + 0xD0)
    bool ChannelsOk;                     // the two maps were readable and as long as the bone and channel counts
    int Channels;
    bool Channel[MaxBones];              // the bone has a channel of its own (all false unless ChannelsOk)
    int Unparented;                      // bones the manager has no parent for but bone 0
    // The SkeletonAsset
    bool AssetOk;                        // proven (see above): Asset, Names and AssetParents are its
    char Asset[AssetBytes];              // its name, e.g. "characters/skeletons/skeleton_male"
    char Names[MaxBones][NameBytes];
    short AssetParents[MaxBones];        // its Hierarchy
    int Differ;                          // bones whose manager parent isn't the asset's
    int DataAgrees;                      // the body's own data names the same asset: 1 yes, 0 no, -1 not readable
    char Why[AssetBytes];                // why the asset wasn't taken ("" if it was)
  };

  // Reads a body's skeleton. gameBase: the game's image. animatable: the body (0: its data isn't compared).
  // manager, bones: its pose manager and bone count as the editor's SlotOf found them. False: the manager's parents
  // couldn't be read at all (out.ParentsOk false); the body can't be posed.
  bool Read(__int64 gameBase, __int64 animatable, __int64 manager, int bones, Body& out);

  // Whether bone i may be listed in a pose override on this body: it has a parent in the manager (the game gives a
  // bone a parent only if the rig has a channel for it) and, where the channel maps were read, a channel of its own
  bool MayPose(Body const& body, int bone);

  // The skeleton for a body: an index for At(), or -1 (not known: can't be posed). By the asset's name when it was
  // proven, else by bone count and parents; a body's -1 where a table has a parent is the same skeleton. An asset
  // the tables don't have gets a skeleton made from its own names (kept for the session, MaxMade at most).
  // The caller holds the pose editor's lock (the made skeletons are shared).
  const int MaxMade = 32;
  int Find(Body const& body);
  int Count();                                        // the tables' and the made ones
  pose_skeletons::Skeleton const& At(int index);      // (index within Count())
  bool IsMade(int index);

  // The bone a pose holds on a skeleton the tables don't have (with every bone below it): of the children of
  // AITrajectory (of bone 0 where there is none) that aren't the game's own placement bones, the one with the most
  // bones below it; -1 if there is none. On the tables' own skeletons this gives their HeldRoot (checked offline).
  int HeldRootOf(int bones, char const* const* names, short const* parents);

  // The game's hash of a bone name (FNV-1, 32 bits)
  unsigned NameHash(char const* name);

  // Where things are (decompiled 2026-11-12; * = also read live on earlier days)
  const int ManagerAssetOffset = 0x00;                // the SkeletonAsset (0x142FC8879)
  const int ManagerChannelsOffset = 0x88;             // * the rig's channel count
  const int ManagerBoneChannelOffset = 0xB0;          // u32 per bone: its channel (0 for none too), begin / end
  const int ManagerChannelBoneOffset = 0xD0;          // u32 per channel: its bone or -1, begin / end
  const int ManagerParentsOffset = 0xF0;              // * u32 per bone, begin (end + 8)
  const __int64 SkeletonAssetVTableRva = 0x1AF1688;
  const int AssetNameOffset = 0x10;
  const int AssetBoneNamesOffset = 0x18;              // char* per bone; every array's count is the u32 before it
  const int AssetBoneHashesOffset = 0x20;
  const int AssetHierarchyOffset = 0x28;
  const int EntityDataOffset = 0x28;                  // an animatable's AntAnimatableEntityData
  const int DataMasterSkeletonOffset = 0x88;          // its MasterSkeletonAsset
  const int MasterSkeletonOffset = 0x18;              // that one's SkeletonAsset
}
