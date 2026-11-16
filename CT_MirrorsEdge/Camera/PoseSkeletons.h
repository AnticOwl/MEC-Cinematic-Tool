#pragma once
// The game's skeletons: each bone's name and parent (-1: none), in the game's bone order, from its
// SkeletonAssets' BoneNames and Hierarchy. EVERY SkeletonAsset of the game's data is here (22 assets, found by
// type over all 43,704 ebx, patched files included, read 2026-11-12 offline), as 18 tables: one for each
// different (bone count, parent array), which is all the tools can see of a live body. Made by a script
// (gen_all_skeletons_h.py) from the game's data: not to be edited by hand.
// The first 9 entries are the ones of the first header (2026-11-08), in the same order.
// Tables that stand for several assets (same bones, same parents):
//   Male: skeleton_female_heels, skeleton_male (skeleton_female_heels names bone 161 FacePosesAnimation; the
//     table's text for that bone is a label for both names: never compare it with a bone's name or its hash)
//   PersonLow: skeleton_female_balc, skeleton_female_heels_balc, skeleton_male_balc
//   Weapon: baton_ske, weapon_ske
// HeldRoot: the child of AITrajectory (of bone 0 where a skeleton has no AITrajectory) that is not one of the
// game's own placement bones (Trajectory, Connect, CameraBase, BlendshapeDriver / FacePosesAnimation) and has the
// most bones below it. On two skeletons (Rope, Prop) the movable bones do not hang on one bone: see
// SeveralTopJoints at the end.
namespace pose_skeletons
{
  struct Skeleton
  {
    const char* Asset;          // the game's SkeletonAsset
    const char* Label;          // for the menu
    int Bones;
    const char* const* Names;
    const short* Parents;
    int HeldRoot;               // the bone a pose holds, with every bone below it (Hips on people)
  };

  const char* const FaithNames[182] = {
    "Reference", "AITrajectory", "Trajectory", "TrajectoryEnd", "Hips", "Spine",
    "Spine1", "Spine2", "LeftShoulder", "LeftArm", "LeftArmRoll", "LeftElbowRoll",
    "LeftForeArm", "LeftForeArmRoll", "LeftForeArmRoll1", "LeftHand", "LeftHandRing0", "LeftHandRing1",
    "LeftHandRing2", "LeftHandRing3", "LeftHandRing4", "LeftHandPinky0", "LeftHandPinky1", "LeftHandPinky2",
    "LeftHandPinky3", "LeftHandPinky4", "LeftHandIndex0", "LeftHandIndex1", "LeftHandIndex2", "LeftHandIndex3",
    "LeftHandIndex4", "LeftHandThumb1", "LeftHandThumb2", "LeftHandThumb3", "LeftHandThumb4", "LeftHandMiddle0",
    "LeftHandMiddle1", "LeftHandMiddle2", "LeftHandMiddle3", "LeftHandMiddle4", "LeftHandThumbBase", "LeftHandIndexBase",
    "LeftHandPinkyBase", "LeftForeArmRoll2", "LeftRadiusTwistHolder", "LeftUlnaTwistHolder", "LeftRadiusTwist", "LeftUlnaTwist",
    "LeftArmRoll1", "LeftArmBend", "Neck", "Neck1", "Head", "HeadEnd",
    "Face", "Jaw", "LeftLowerLip", "LeftLowerInnerLip", "LowerLip", "LowerInnerLip",
    "RightLowerLip", "RightLowerInnerLip", "Tongue", "TongueTip", "Chin", "LeftLowCheek",
    "RightLowCheek", "LeftEye", "LeftIris", "RightEye", "RightIris", "LeftUpCheek",
    "LeftUpInnerCheek", "RightUpInnerCheek", "RightUpCheek", "LeftCheek", "RightCheek", "LeftMouth",
    "LeftInnerMouth", "LeftMiddleCrease", "LeftUpperLip", "LeftUpperInnerLip", "UpperLip", "UpperInnerLip",
    "RightUpperLip", "RightUpperInnerLip", "RightMouth", "RightInnerMouth", "RightMiddleCrease", "LeftUpEyelid",
    "RightUpEyelid", "LeftLowEyelid", "RightLowEyelid", "LeftInnerEyebrow", "LeftOuterEyebrow", "RightInnerEyebrow",
    "RightOuterEyebrow", "LeftNose", "RightNose", "LeftCrease", "RightCrease", "LeftLowMiddleEyebrow",
    "RightMiddleEyebrow", "LeftLowEyelidCrease", "LeftLowOuterEyebrow", "NoseTip", "RightLowOuterEyebrow", "LeftMiddleEyebrow",
    "RightLowMiddleEyebrow", "RightLowEyelidCrease", "LowNose", "Throat", "LeftArmPit", "LeftArmPitLow",
    "NeckCollar", "LeftDeltoidBulge", "RightShoulder", "RightArm", "RightArmRoll", "RightElbowRoll",
    "RightForeArm", "RightForeArmRoll", "RightForeArmRoll1", "RightHand", "RightHandRing0", "RightHandRing1",
    "RightHandRing2", "RightHandRing3", "RightHandRing4", "RightHandPinky0", "RightHandPinky1", "RightHandPinky2",
    "RightHandPinky3", "RightHandPinky4", "RightHandIndex0", "RightHandIndex1", "RightHandIndex2", "RightHandIndex3",
    "RightHandIndex4", "RightHandThumb1", "RightHandThumb2", "RightHandThumb3", "RightHandThumb4", "RightHandMiddle0",
    "RightHandMiddle1", "RightHandMiddle2", "RightHandMiddle3", "RightHandMiddle4", "RightHandThumbBase", "RightHandIndexBase",
    "RightHandPinkyBase", "Wep_Root", "RightForeArmRoll2", "RightRadiusTwistHolder", "RightUlnaTwistHolder", "RightRadiusTwist",
    "RightUlnaTwist", "RightArmRoll1", "RightArmBend", "RightArmPit", "RightArmPitLow", "RightDeltoidBulge",
    "LeftUpLeg", "LeftUpLegRoll", "LeftKneeRoll", "LeftHipsRoll", "LeftLeg", "LeftFoot",
    "LeftToeBase", "LeftToe", "RightUpLeg", "RightUpLegRoll", "RightKneeRoll", "RightHipsRoll",
    "RightLeg", "RightFoot", "RightToeBase", "RightToe", "CameraBase", "CameraJoint",
    "Connect", "ConnectEnd",
  };
  const short FaithParents[182] = {
    -1, 0, 1, 2, 1, 4, 5, 6, 7, 8, 9, 9, 9, 12, 12, 12, 15, 16, 17, 18, 19, 15, 21, 22,
    23, 24, 15, 26, 27, 28, 29, 15, 31, 32, 33, 15, 35, 36, 37, 38, 15, 15, 15, 12, 43, 43, 12, 12,
    9, 9, 7, 50, 51, 52, 52, 54, 55, 56, 55, 58, 55, 60, 55, 62, 55, 55, 55, 54, 67, 54, 69, 54,
    54, 54, 54, 54, 54, 54, 77, 77, 54, 80, 54, 82, 54, 84, 54, 86, 86, 54, 54, 54, 54, 54, 54, 54,
    54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 54, 51, 7, 112, 7, 7, 7, 116, 117, 117,
    117, 120, 120, 120, 123, 124, 125, 126, 127, 123, 129, 130, 131, 132, 123, 134, 135, 136, 137, 123, 139, 140, 141, 123,
    143, 144, 145, 146, 123, 123, 123, 123, 120, 152, 152, 120, 120, 117, 117, 7, 159, 7, 4, 162, 162, 162, 162, 166,
    167, 168, 4, 170, 170, 170, 170, 174, 175, 176, 1, 178, 1, 180,
  };

  const char* const EnemyNames[163] = {
    "Reference", "AITrajectory", "Trajectory", "TrajectoryEnd", "Hips", "Spine",
    "Spine1", "Spine2", "LeftShoulder", "LeftArm", "LeftArmRoll", "LeftElbowRoll",
    "LeftForeArm", "LeftForeArmRoll", "LeftForeArmRoll1", "LeftHand", "LeftHandRing0", "LeftHandRing1",
    "LeftHandRing2", "LeftHandRing3", "LeftHandRing4", "LeftHandPinky0", "LeftHandPinky1", "LeftHandPinky2",
    "LeftHandPinky3", "LeftHandPinky4", "LeftHandIndex0", "LeftHandIndex1", "LeftHandIndex2", "LeftHandIndex3",
    "LeftHandIndex4", "LeftHandThumb1", "LeftHandThumb2", "LeftHandThumb3", "LeftHandThumb4", "LeftHandMiddle0",
    "LeftHandMiddle1", "LeftHandMiddle2", "LeftHandMiddle3", "LeftHandMiddle4", "LeftForeArmRoll2", "LeftArmRoll1",
    "Neck", "Neck1", "Head", "HeadEnd", "Face", "Jaw",
    "LeftLowerLip", "LeftLowerInnerLip", "LowerLip", "LowerInnerLip", "RightLowerLip", "RightLowerInnerLip",
    "Tongue", "TongueTip", "Chin", "LeftLowCheek", "RightLowCheek", "LeftEye",
    "LeftIris", "RightEye", "RightIris", "LeftUpCheek", "LeftUpInnerCheek", "RightUpInnerCheek",
    "RightUpCheek", "LeftCheek", "RightCheek", "LeftMouth", "LeftInnerMouth", "LeftMiddleCrease",
    "LeftUpperLip", "LeftUpperInnerLip", "UpperLip", "UpperInnerLip", "RightUpperLip", "RightUpperInnerLip",
    "RightMouth", "RightInnerMouth", "RightMiddleCrease", "LeftUpEyelid", "RightUpEyelid", "LeftLowEyelid",
    "RightLowEyelid", "LeftInnerEyebrow", "LeftOuterEyebrow", "RightInnerEyebrow", "RightOuterEyebrow", "LeftNose",
    "RightNose", "LeftCrease", "RightCrease", "LeftLowMiddleEyebrow", "RightMiddleEyebrow", "LeftLowEyelidCrease",
    "LeftLowOuterEyebrow", "NoseTip", "RightLowOuterEyebrow", "LeftMiddleEyebrow", "RightLowMiddleEyebrow", "RightLowEyelidCrease",
    "LowNose", "Throat", "LeftArmPit", "RightShoulder", "RightArm", "RightArmRoll",
    "RightElbowRoll", "RightForeArm", "RightForeArmRoll", "RightForeArmRoll1", "RightHand", "RightHandRing0",
    "RightHandRing1", "RightHandRing2", "RightHandRing3", "RightHandRing4", "RightHandPinky0", "RightHandPinky1",
    "RightHandPinky2", "RightHandPinky3", "RightHandPinky4", "RightHandIndex0", "RightHandIndex1", "RightHandIndex2",
    "RightHandIndex3", "RightHandIndex4", "RightHandThumb1", "RightHandThumb2", "RightHandThumb3", "RightHandThumb4",
    "RightHandMiddle0", "RightHandMiddle1", "RightHandMiddle2", "RightHandMiddle3", "RightHandMiddle4", "Wep_Root",
    "RightForeArmRoll2", "RightArmRoll1", "RightArmPit", "LeftUpLeg", "LeftUpLegRoll", "LeftKneeRoll",
    "LeftHipsRoll", "LeftLeg", "LeftFoot", "LeftToeBase", "LeftToe", "LeftAnkle",
    "RightUpLeg", "RightUpLegRoll", "RightKneeRoll", "RightHipsRoll", "RightLeg", "RightFoot",
    "RightToeBase", "RightToe", "RightAnkle", "CameraBase", "CameraJoint", "Connect",
    "ConnectEnd",
  };
  const short EnemyParents[163] = {
    -1, 0, 1, 2, 1, 4, 5, 6, 7, 8, 9, 9, 9, 12, 12, 12, 15, 16, 17, 18, 19, 15, 21, 22,
    23, 24, 15, 26, 27, 28, 29, 15, 31, 32, 33, 15, 35, 36, 37, 38, 12, 9, 7, 42, 43, 44, 44, 46,
    47, 48, 47, 50, 47, 52, 47, 54, 47, 47, 47, 46, 59, 46, 61, 46, 46, 46, 46, 46, 46, 46, 69, 69,
    46, 72, 46, 74, 46, 76, 46, 78, 78, 46, 46, 46, 46, 46, 46, 46, 46, 46, 46, 46, 46, 46, 46, 46,
    46, 46, 46, 46, 46, 46, 46, 43, 7, 7, 105, 106, 106, 106, 109, 109, 109, 112, 113, 114, 115, 116, 112, 118,
    119, 120, 121, 112, 123, 124, 125, 126, 112, 128, 129, 130, 112, 132, 133, 134, 135, 112, 109, 106, 7, 4, 141, 141,
    141, 141, 145, 146, 147, 145, 4, 150, 150, 150, 150, 154, 155, 156, 154, 1, 159, 1, 161,
  };

  const char* const MaleNames[162] = {
    "Reference", "AITrajectory", "Trajectory", "TrajectoryEnd", "Hips", "Spine",
    "Spine1", "Spine2", "LeftShoulder", "LeftArm", "LeftArmRoll", "LeftElbowRoll",
    "LeftForeArm", "LeftForeArmRoll", "LeftForeArmRoll1", "LeftHand", "LeftHandRing0", "LeftHandRing1",
    "LeftHandRing2", "LeftHandRing3", "LeftHandRing4", "LeftHandPinky0", "LeftHandPinky1", "LeftHandPinky2",
    "LeftHandPinky3", "LeftHandPinky4", "LeftHandIndex0", "LeftHandIndex1", "LeftHandIndex2", "LeftHandIndex3",
    "LeftHandIndex4", "LeftHandThumb1", "LeftHandThumb2", "LeftHandThumb3", "LeftHandThumb4", "LeftHandMiddle0",
    "LeftHandMiddle1", "LeftHandMiddle2", "LeftHandMiddle3", "LeftHandMiddle4", "LeftForeArmRoll2", "LeftArmRoll1",
    "Neck", "Neck1", "Head", "HeadEnd", "Face", "Jaw",
    "LeftLowerLip", "LeftLowerInnerLip", "LowerLip", "LowerInnerLip", "RightLowerLip", "RightLowerInnerLip",
    "Tongue", "TongueTip", "Chin", "LeftLowCheek", "RightLowCheek", "LeftEye",
    "LeftIris", "RightEye", "RightIris", "LeftUpCheek", "LeftUpInnerCheek", "RightUpInnerCheek",
    "RightUpCheek", "LeftCheek", "RightCheek", "LeftMouth", "LeftInnerMouth", "LeftMiddleCrease",
    "LeftUpperLip", "LeftUpperInnerLip", "UpperLip", "UpperInnerLip", "RightUpperLip", "RightUpperInnerLip",
    "RightMouth", "RightInnerMouth", "RightMiddleCrease", "LeftUpEyelid", "RightUpEyelid", "LeftLowEyelid",
    "RightLowEyelid", "LeftInnerEyebrow", "LeftOuterEyebrow", "RightInnerEyebrow", "RightOuterEyebrow", "LeftNose",
    "RightNose", "LeftCrease", "RightCrease", "LeftLowMiddleEyebrow", "RightMiddleEyebrow", "LeftLowEyelidCrease",
    "LeftLowOuterEyebrow", "NoseTip", "RightLowOuterEyebrow", "LeftMiddleEyebrow", "RightLowMiddleEyebrow", "RightLowEyelidCrease",
    "LowNose", "Throat", "LeftArmPit", "RightShoulder", "RightArm", "RightArmRoll",
    "RightElbowRoll", "RightForeArm", "RightForeArmRoll", "RightForeArmRoll1", "RightHand", "RightHandRing0",
    "RightHandRing1", "RightHandRing2", "RightHandRing3", "RightHandRing4", "RightHandPinky0", "RightHandPinky1",
    "RightHandPinky2", "RightHandPinky3", "RightHandPinky4", "RightHandIndex0", "RightHandIndex1", "RightHandIndex2",
    "RightHandIndex3", "RightHandIndex4", "RightHandThumb1", "RightHandThumb2", "RightHandThumb3", "RightHandThumb4",
    "RightHandMiddle0", "RightHandMiddle1", "RightHandMiddle2", "RightHandMiddle3", "RightHandMiddle4", "Wep_Root",
    "RightForeArmRoll2", "RightArmRoll1", "RightArmPit", "LeftUpLeg", "LeftUpLegRoll", "LeftKneeRoll",
    "LeftHipsRoll", "LeftLeg", "LeftFoot", "LeftToeBase", "LeftToe", "RightUpLeg",
    "RightUpLegRoll", "RightKneeRoll", "RightHipsRoll", "RightLeg", "RightFoot", "RightToeBase",
    "RightToe", "CameraBase", "CameraJoint", "Connect", "ConnectEnd", "BlendshapeDriver / FacePosesAnimation",
  };
  const short MaleParents[162] = {
    -1, 0, 1, 2, 1, 4, 5, 6, 7, 8, 9, 9, 9, 12, 12, 12, 15, 16, 17, 18, 19, 15, 21, 22,
    23, 24, 15, 26, 27, 28, 29, 15, 31, 32, 33, 15, 35, 36, 37, 38, 12, 9, 7, 42, 43, 44, 44, 46,
    47, 48, 47, 50, 47, 52, 47, 54, 47, 47, 47, 46, 59, 46, 61, 46, 46, 46, 46, 46, 46, 46, 69, 69,
    46, 72, 46, 74, 46, 76, 46, 78, 78, 46, 46, 46, 46, 46, 46, 46, 46, 46, 46, 46, 46, 46, 46, 46,
    46, 46, 46, 46, 46, 46, 46, 43, 7, 7, 105, 106, 106, 106, 109, 109, 109, 112, 113, 114, 115, 116, 112, 118,
    119, 120, 121, 112, 123, 124, 125, 126, 112, 128, 129, 130, 112, 132, 133, 134, 135, 112, 109, 106, 7, 4, 141, 141,
    141, 141, 145, 146, 147, 4, 149, 149, 149, 149, 153, 154, 155, 1, 157, 1, 159, 1,
  };

  const char* const FemaleNames[169] = {
    "Reference", "AITrajectory", "Trajectory", "TrajectoryEnd", "Hips", "Spine",
    "Spine1", "Spine2", "LeftShoulder", "LeftArm", "LeftArmRoll", "LeftElbowRoll",
    "LeftForeArm", "LeftForeArmRoll", "LeftForeArmRoll1", "LeftHand", "LeftHandRing0", "LeftHandRing1",
    "LeftHandRing2", "LeftHandRing3", "LeftHandRing4", "LeftHandPinky0", "LeftHandPinky1", "LeftHandPinky2",
    "LeftHandPinky3", "LeftHandPinky4", "LeftHandIndex0", "LeftHandIndex1", "LeftHandIndex2", "LeftHandIndex3",
    "LeftHandIndex4", "LeftHandThumb1", "LeftHandThumb2", "LeftHandThumb3", "LeftHandThumb4", "LeftHandMiddle0",
    "LeftHandMiddle1", "LeftHandMiddle2", "LeftHandMiddle3", "LeftHandMiddle4", "LeftForeArmRoll2", "LeftArmRoll1",
    "Neck", "Neck1", "Head", "HeadEnd", "Face", "Jaw",
    "LeftLowerLip", "LeftLowerInnerLip", "LowerLip", "LowerInnerLip", "RightLowerLip", "RightLowerInnerLip",
    "Tongue", "TongueTip", "Chin", "LeftLowCheek", "RightLowCheek", "LeftEye",
    "LeftIris", "RightEye", "RightIris", "LeftUpCheek", "LeftUpInnerCheek", "RightUpInnerCheek",
    "RightUpCheek", "LeftCheek", "RightCheek", "LeftMouth", "LeftInnerMouth", "LeftMiddleCrease",
    "LeftUpperLip", "LeftUpperInnerLip", "UpperLip", "UpperInnerLip", "RightUpperLip", "RightUpperInnerLip",
    "RightMouth", "RightInnerMouth", "RightMiddleCrease", "LeftUpEyelid", "RightUpEyelid", "LeftLowEyelid",
    "RightLowEyelid", "LeftInnerEyebrow", "LeftOuterEyebrow", "RightInnerEyebrow", "RightOuterEyebrow", "LeftNose",
    "RightNose", "LeftCrease", "RightCrease", "LeftLowMiddleEyebrow", "RightMiddleEyebrow", "LeftLowEyelidCrease",
    "LeftLowOuterEyebrow", "NoseTip", "RightLowOuterEyebrow", "LeftMiddleEyebrow", "RightLowMiddleEyebrow", "RightLowEyelidCrease",
    "LowNose", "ponyTail_ROOT", "ponyTail_01", "ponyTail_02", "ponyTail_03", "ponyTail_04",
    "ponyTail_05", "Throat", "LeftArmPit", "RightShoulder", "RightArm", "RightArmRoll",
    "RightElbowRoll", "RightForeArm", "RightForeArmRoll", "RightForeArmRoll1", "RightHand", "RightHandRing0",
    "RightHandRing1", "RightHandRing2", "RightHandRing3", "RightHandRing4", "RightHandPinky0", "RightHandPinky1",
    "RightHandPinky2", "RightHandPinky3", "RightHandPinky4", "RightHandIndex0", "RightHandIndex1", "RightHandIndex2",
    "RightHandIndex3", "RightHandIndex4", "RightHandThumb1", "RightHandThumb2", "RightHandThumb3", "RightHandThumb4",
    "RightHandMiddle0", "RightHandMiddle1", "RightHandMiddle2", "RightHandMiddle3", "RightHandMiddle4", "Wep_Root",
    "RightForeArmRoll2", "RightArmRoll1", "RightArmPit", "LeftUpLeg", "LeftUpLegRoll", "LeftKneeRoll",
    "LeftHipsRoll", "LeftLeg", "LeftFoot", "LeftToeBase", "LeftToe", "LeftAnkle",
    "RightUpLeg", "RightUpLegRoll", "RightKneeRoll", "RightHipsRoll", "RightLeg", "RightFoot",
    "RightToeBase", "RightToe", "RightAnkle", "CameraBase", "CameraJoint", "Connect",
    "ConnectEnd",
  };
  const short FemaleParents[169] = {
    -1, 0, 1, 2, 1, 4, 5, 6, 7, 8, 9, 9, 9, 12, 12, 12, 15, 16, 17, 18, 19, 15, 21, 22,
    23, 24, 15, 26, 27, 28, 29, 15, 31, 32, 33, 15, 35, 36, 37, 38, 12, 9, 7, 42, 43, 44, 44, 46,
    47, 48, 47, 50, 47, 52, 47, 54, 47, 47, 47, 46, 59, 46, 61, 46, 46, 46, 46, 46, 46, 46, 69, 69,
    46, 72, 46, 74, 46, 76, 46, 78, 78, 46, 46, 46, 46, 46, 46, 46, 46, 46, 46, 46, 46, 46, 46, 46,
    46, 46, 46, 46, 46, 46, 46, 44, 103, 104, 105, 106, 107, 43, 7, 7, 111, 112, 112, 112, 115, 115, 115, 118,
    119, 120, 121, 122, 118, 124, 125, 126, 127, 118, 129, 130, 131, 132, 118, 134, 135, 136, 118, 138, 139, 140, 141, 118,
    115, 112, 7, 4, 147, 147, 147, 147, 151, 152, 153, 151, 4, 156, 156, 156, 156, 160, 161, 162, 160, 1, 165, 1,
    167,
  };

  const char* const EnemyLowNames[83] = {
    "Reference", "AITrajectory", "Trajectory", "TrajectoryEnd", "Hips", "Spine",
    "Spine1", "Spine2", "LeftShoulder", "LeftArm", "LeftArmRoll", "LeftElbowRoll",
    "LeftForeArm", "LeftForeArmRoll", "LeftForeArmRoll1", "LeftHand", "LeftHandRing0", "LeftHandRing1",
    "LeftHandRing2", "LeftHandRing3", "LeftHandPinky0", "LeftHandPinky1", "LeftHandPinky2", "LeftHandPinky3",
    "LeftHandIndex0", "LeftHandIndex1", "LeftHandIndex2", "LeftHandIndex3", "LeftHandThumb1", "LeftHandThumb2",
    "LeftHandThumb3", "LeftHandMiddle0", "LeftHandMiddle1", "LeftHandMiddle2", "LeftHandMiddle3", "LeftForeArmRoll2",
    "Neck", "Neck1", "Head", "HeadEnd", "RightShoulder", "RightArm",
    "RightArmRoll", "RightElbowRoll", "RightForeArm", "RightForeArmRoll", "RightForeArmRoll1", "RightHand",
    "RightHandRing0", "RightHandRing1", "RightHandRing2", "RightHandRing3", "RightHandPinky0", "RightHandPinky1",
    "RightHandPinky2", "RightHandPinky3", "RightHandIndex0", "RightHandIndex1", "RightHandIndex2", "RightHandIndex3",
    "RightHandThumb1", "RightHandThumb2", "RightHandThumb3", "RightHandMiddle0", "RightHandMiddle1", "RightHandMiddle2",
    "RightHandMiddle3", "Wep_Root", "RightForeArmRoll2", "LeftUpLeg", "LeftUpLegRoll", "LeftKneeRoll",
    "LeftLeg", "LeftFoot", "LeftToeBase", "RightUpLeg", "RightUpLegRoll", "RightKneeRoll",
    "RightLeg", "RightFoot", "RightToeBase", "Connect", "ConnectEnd",
  };
  const short EnemyLowParents[83] = {
    -1, 0, 1, 2, 1, 4, 5, 6, 7, 8, 9, 9, 9, 12, 12, 12, 15, 16, 17, 18, 15, 20, 21, 22,
    15, 24, 25, 26, 15, 28, 29, 15, 31, 32, 33, 12, 7, 36, 37, 38, 7, 40, 41, 41, 41, 44, 44, 44,
    47, 48, 49, 50, 47, 52, 53, 54, 47, 56, 57, 58, 47, 60, 61, 47, 63, 64, 65, 47, 44, 4, 69, 69,
    69, 72, 73, 4, 75, 75, 75, 78, 79, 1, 81,
  };

  const char* const PersonLowNames[82] = {
    "Reference", "AITrajectory", "Trajectory", "TrajectoryEnd", "Hips", "Spine",
    "Spine1", "Spine2", "LeftShoulder", "LeftArm", "LeftArmRoll", "LeftElbowRoll",
    "LeftForeArm", "LeftForeArmRoll", "LeftForeArmRoll1", "LeftHand", "LeftHandRing0", "LeftHandRing1",
    "LeftHandRing2", "LeftHandRing3", "LeftHandPinky0", "LeftHandPinky1", "LeftHandPinky2", "LeftHandPinky3",
    "LeftHandIndex0", "LeftHandIndex1", "LeftHandIndex2", "LeftHandIndex3", "LeftHandThumb1", "LeftHandThumb2",
    "LeftHandThumb3", "LeftHandMiddle0", "LeftHandMiddle1", "LeftHandMiddle2", "LeftHandMiddle3", "LeftForeArmRoll2",
    "Neck", "Neck1", "Head", "HeadEnd", "RightShoulder", "RightArm",
    "RightArmRoll", "RightElbowRoll", "RightForeArm", "RightForeArmRoll", "RightForeArmRoll1", "RightHand",
    "RightHandRing0", "RightHandRing1", "RightHandRing2", "RightHandRing3", "RightHandPinky0", "RightHandPinky1",
    "RightHandPinky2", "RightHandPinky3", "RightHandIndex0", "RightHandIndex1", "RightHandIndex2", "RightHandIndex3",
    "RightHandThumb1", "RightHandThumb2", "RightHandThumb3", "RightHandMiddle0", "RightHandMiddle1", "RightHandMiddle2",
    "RightHandMiddle3", "RightForeArmRoll2", "LeftUpLeg", "LeftUpLegRoll", "LeftKneeRoll", "LeftLeg",
    "LeftFoot", "LeftToeBase", "RightUpLeg", "RightUpLegRoll", "RightKneeRoll", "RightLeg",
    "RightFoot", "RightToeBase", "Connect", "ConnectEnd",
  };
  const short PersonLowParents[82] = {
    -1, 0, 1, 2, 1, 4, 5, 6, 7, 8, 9, 9, 9, 12, 12, 12, 15, 16, 17, 18, 15, 20, 21, 22,
    15, 24, 25, 26, 15, 28, 29, 15, 31, 32, 33, 12, 7, 36, 37, 38, 7, 40, 41, 41, 41, 44, 44, 44,
    47, 48, 49, 50, 47, 52, 53, 54, 47, 56, 57, 58, 47, 60, 61, 47, 63, 64, 65, 44, 4, 68, 68, 68,
    71, 72, 4, 74, 74, 74, 77, 78, 1, 80,
  };

  const char* const CrowdNames[28] = {
    "Reference", "AITrajectory", "Trajectory", "TrajectoryEnd", "Hips", "Spine",
    "Spine1", "Spine2", "LeftShoulder", "LeftArm", "LeftForeArm", "LeftHand",
    "Neck", "Neck1", "RightShoulder", "RightArm", "RightForeArm", "RightHand",
    "LeftUpLeg", "LeftLeg", "LeftFoot", "RightUpLeg", "RightLeg", "RightFoot",
    "CameraBase", "CameraJoint", "Connect", "ConnectEnd",
  };
  const short CrowdParents[28] = {
    -1, 0, 1, 2, 1, 4, 5, 6, 7, 8, 9, 10, 7, 12, 7, 14, 15, 16, 4, 18, 19, 4, 21, 22,
    1, 24, 1, 26,
  };

  const char* const DroneNames[49] = {
    "Reference", "AITrajectory", "Drone_Base_jt", "SensorMount_01_jt", "SensorHead_jt", "Arm1_LegJoint_01_jt",
    "Arm1_LegJoint_02_jt", "Arm1_Foot_jt", "Arm1_PneumaticEnd_jt", "Arm1_Pneumatic_jt", "Arm1_JointBody_jt", "Arm1_Arm_jt",
    "Arm1_RotorConnection_jt", "Arm1_Rotor1_buffGrp", "Arm1_Rotor1_jt", "Arm1_Rotor_buffGrp", "Arm1_Rotor_jt", "Arm1_ChasisRot_jt",
    "Arm2_LegJoint_01_jt", "Arm2_LegJoint_02_jt", "Arm2_Foot_jt", "Arm2_PneumaticEnd_jt", "Arm2_Pneumatic_jt", "Arm2_JointBody_jt",
    "Arm2_Arm_jt", "Arm2_RotorConnection_jt", "Arm2_Rotor1_buffGrp", "Arm2_Rotor1_jt", "Arm2_Rotor_buffGrp", "Arm2_Rotor_jt",
    "Arm2_ChasisRot_jt", "Arm3_LegJoint_01_jt", "Arm3_LegJoint_02_jt", "Arm3_Foot_jt", "Arm3_PneumaticEnd_jt", "Arm3_Pneumatic_jt",
    "Arm3_JointBody_jt", "Arm3_Arm_jt", "Arm3_RotorConnection_jt", "Arm3_Rotor1_buffGrp", "Arm3_Rotor1_jt", "Arm3_Rotor_buffGrp",
    "Arm3_Rotor_jt", "Arm3_ChasisRot_jt", "SensorHead_aim_jt", "Connect", "ConnectEnd", "Trajectory",
    "TrajectoryEnd",
  };
  const short DroneParents[49] = {
    -1, 0, 1, 2, 3, 2, 5, 6, 6, 5, 2, 2, 11, 12, 13, 12, 15, 2, 2, 18, 19, 19, 18, 2,
    2, 24, 25, 26, 25, 28, 2, 2, 31, 32, 32, 31, 2, 2, 37, 38, 39, 38, 41, 2, 2, 1, 45, 1,
    47,
  };

  const char* const BearNames[47] = {
    "Reference", "AITrajectory", "Trajectory", "TrajectoryEnd", "Connect", "ConnectEnd",
    "Body_Main", "BackFrame_base", "BackFrame_hinge", "BackFrame_end", "Back_Wheel", "Body_01",
    "Body_02", "Neck_01", "Neck_02", "Head", "optic01", "optic02",
    "Right_Hear", "antenna_01", "antenna_02", "antenna_03", "antenna_04", "Left_Hear",
    "Right_Upperarm", "Right_Forearm", "Right_Hand_01", "Right_Hand_02", "Right_Hand_03", "Right_finge02_01",
    "Right_finge02_02", "Right_finge01_01", "Right_finge01_02", "chestGuard_01", "chestGuard_02", "chestGuard_03",
    "chestGuard_04", "Left_Upperarm", "Left_Forearm", "Left_Hand_01", "Left_Hand_02", "Left_Hand_03",
    "Left_finge02_01", "Left_finge02_02", "Left_finge01_01", "Left_finge01_02", "Front_Wheel",
  };
  const short BearParents[47] = {
    -1, 0, 1, 2, 1, 4, 1, 6, 7, 8, 9, 7, 11, 12, 13, 14, 15, 16, 15, 15, 19, 20, 21, 15,
    12, 24, 25, 26, 27, 28, 29, 28, 31, 12, 33, 34, 35, 12, 37, 38, 39, 40, 41, 42, 41, 44, 6,
  };

  // ---- The skeletons the first header did not have ----

  // weapon (gun, baton or knife): weapon_ske: weapons/wepbp_apr_11, wepbp_apr_11_bn, wepbp_automaticgun; baton_ske:
  //   weapons/wepbp_baton, wepbp_baton_echo, wepbp_knife
  const char* const WeaponNames[26] = {
    "Reference", "AITrajectory", "Wep_Root", "Wep_Extra1", "Wep_Trigger", "Wep_Slide",
    "Wep_Grenade1", "Wep_Grenade2", "Wep_Mag", "Wep_Mag_Ammo", "Wep_Physic1", "Wep_Physic2",
    "Wep_Physic3", "Wep_Belt1", "Wep_Belt2", "Wep_Belt3", "Wep_Belt4", "Wep_Belt5",
    "Wep_Bipod1", "Wep_Bipod2", "Wep_Bipod3", "IK_Joint_LeftHand", "IK_Joint_RightHand", "Wep_Extra2",
    "Wep_Extra3", "Wep_Aim",
  };
  const short WeaponParents[26] = {
    -1, 0, 1, 2, 2, 2, 2, 6, 2, 8, 2, 2, 2, 2, 13, 14, 15, 16, 2, 18, 18, 2, 2, 2,
    2, 2,
  };

  // pigeon: characters/actorentities/pigeon01 (an ActorEntityData)
  const char* const PigeonNames[83] = {
    "Reference", "AITrajectory", "Trajectory", "TrajectoryEnd", "Connect", "ConnectEnd",
    "pigeon_ROOTSHJnt", "pigeon_l_leg_HipSHJnt", "pigeon_l_leg_Knee1SHJnt", "pigeon_l_leg_Knee2SHJnt", "pigeon_l_leg_AnkleSHJnt", "pigeon_l_leg_BallSHJnt",
    "pigeon_l_leg_ToeSHJnt", "pigeon_l_innerToe_01_01SHJnt", "pigeon_l_innerToe_01_02SHJnt", "pigeon_l_innerToe_01_03SHJnt", "pigeon_l_innerToe_01_04SHJnt", "pigeon_l_middleToe_01_01SHJnt",
    "pigeon_l_middleToe_01_02SHJnt", "pigeon_l_middleToe_01_03SHJnt", "pigeon_l_middleToe_01_04SHJnt", "pigeon_l_outerToe_01_01SHJnt", "pigeon_l_outerToe_01_02SHJnt", "pigeon_l_outerToe_01_03SHJnt",
    "pigeon_l_outerToe_01_04SHJnt", "pigeon_l_backToe_01_01SHJnt", "pigeon_l_backToe_01_02SHJnt", "pigeon_r_leg_HipSHJnt", "pigeon_r_leg_Knee1SHJnt", "pigeon_r_leg_Knee2SHJnt",
    "pigeon_r_leg_AnkleSHJnt", "pigeon_r_leg_BallSHJnt", "pigeon_r_leg_ToeSHJnt", "pigeon_r_innerToe_01_01SHJnt", "pigeon_r_innerToe_01_02SHJnt", "pigeon_r_innerToe_01_03SHJnt",
    "pigeon_r_innerToe_01_04SHJnt", "pigeon_r_middleToe_01_01SHJnt", "pigeon_r_middleToe_01_02SHJnt", "pigeon_r_middleToe_01_03SHJnt", "pigeon_r_middleToe_01_04SHJnt", "pigeon_r_outerToe_01_01SHJnt",
    "pigeon_r_outerToe_01_02SHJnt", "pigeon_r_outerToe_01_03SHJnt", "pigeon_r_outerToe_01_04SHJnt", "pigeon_r_backToe_01_01SHJnt", "pigeon_r_backToe_01_02SHJnt", "pigeon_tail_01_01SHJnt",
    "pigeon_tail_01_02SHJnt", "pigeon_l_tailFeatherSpread_01_01SHJnt", "pigeon_r_tailFeatherSpread_01_01SHJnt", "pigeon_spine_01_01SHJnt", "pigeon_spine_01_02SHJnt", "pigeon_spine_01_03SHJnt",
    "pigeon_neck_01SHJnt", "pigeon_neck_02SHJnt", "pigeon_neck_03SHJnt", "pigeon_neck_TopSHJnt", "pigeon_jaw_01_01SHJnt", "pigeon_l_wing_ClavicleSHJnt",
    "pigeon_l_wing_ShoulderSHJnt", "pigeon_l_wing_ElbowSHJnt", "pigeon_l_wing_WristSHJnt", "pigeon_l_finger_01_01SHJnt", "pigeon_l_finger_01_02SHJnt", "pigeon_l_wingFeather_finger_01_02SHJnt",
    "pigeon_l_wingFeather_finger_01_01SHJnt", "pigeon_l_wingFeather_WristSHJnt", "pigeon_l_wingFeather_ElbowSHJnt", "pigeon_l_wingFeather_ShoulderSHJnt1", "pigeon_l_covertFeathers_ShoulderSHJnt", "pigeon_r_wing_ClavicleSHJnt",
    "pigeon_r_wing_ShoulderSHJnt", "pigeon_r_wing_ElbowSHJnt", "pigeon_r_wing_WristSHJnt", "pigeon_r_finger_01_01SHJnt", "pigeon_r_finger_01_02SHJnt", "pigeon_r_wingFeather_finger_01_02SHJnt",
    "pigeon_r_wingFeather_finger_01_01SHJnt", "pigeon_r_wingFeather_WristSHJnt", "pigeon_r_wingFeather_ElbowSHJnt", "pigeon_r_wingFeather_ShoulderSHJnt", "pigeon_r_covertFeathers_ShoulderSHJnt",
  };
  const short PigeonParents[83] = {
    -1, 0, 1, 2, 1, 4, 1, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 12, 17, 18, 19, 12, 21, 22,
    23, 12, 25, 6, 27, 28, 29, 30, 31, 32, 33, 34, 35, 32, 37, 38, 39, 32, 41, 42, 43, 32, 45, 6,
    47, 48, 48, 6, 51, 52, 53, 54, 55, 56, 57, 52, 59, 60, 61, 62, 63, 64, 63, 62, 61, 60, 60, 52,
    71, 72, 73, 74, 75, 76, 75, 74, 73, 72, 72,
  };

  // rat: characters/misc/rat/pf_rat, pf_rat_scruffy (ModelAnimationEntityData, through rat_run_animset: no
  //   AntAnimatableEntityData)
  const char* const RatNames[23] = {
    "Reference", "AITrajectory", "Hips", "Shoulder", "Neck", "Head",
    "FrontUpLeg", "FrontLeg", "FrontFoot", "FrontToe", "Hip", "Tail01",
    "Tail02", "Tail03", "Tail04", "BackUpLeg", "BackLeg", "BackFoot",
    "BackToe", "Trajectory", "TrajectoryEnd", "Connect", "ConnectEnd",
  };
  const short RatParents[23] = {
    -1, 0, 1, 2, 3, 4, 3, 6, 7, 8, 2, 10, 11, 12, 13, 10, 15, 16, 17, 1, 19, 1, 21,
  };

  // animated prop (door, collectible box...): gameplay/interactive/door/pf_doorlogic, the collectible electric boxes
  //   and kiosks, pf_openelevatordoors, pf_opendoors, a pendulum
  const char* const PropNames[14] = {
    "Reference", "AITrajectory", "Trajectory", "TrajectoryEnd", "Connect", "ConnectEnd",
    "PropJoint1", "PropJointHir1", "PropJointHir2", "PropJointHir3", "PropJointHir4", "PropJoint2",
    "FxJoint1", "FxJoint2",
  };
  const short PropParents[14] = {
    -1, 0, 1, 2, 1, 4, 1, 1, 7, 8, 9, 1, 1, 1,
  };

  // mechanical arm (Kingdom cells): objects/architecture/unique/kingdom/cells/mecharm_01/pf_mecharm_01
  //   (ModelAnimationEntityData, through mecharm_01_animset)
  const char* const MechArmNames[17] = {
    "Reference", "AITrajectory", "Trajectory", "TrajectoryEnd", "Connect", "ConnectEnd",
    "MechArm", "Collar", "BaseArm", "MidArm", "OuterArm", "GripWrist",
    "GripHub", "GripClaw1", "GripClaw2", "GripClaw3", "GripClaw4",
  };
  const short MechArmParents[17] = {
    -1, 0, 1, 2, 1, 4, 1, 6, 7, 8, 9, 10, 11, 12, 12, 12, 12,
  };

  // MagRope or zip line rope: PamMagropeRenderEntityData (Faith's MagRope, the VTOL's ropes) and
  //   PamGameSplineRenderEntityData (zip lines): rope renderers, no AntAnimatableEntityData
  const char* const RopeNames[207] = {
    "Reference", "AITrajectory", "Trajectory", "TrajectoryEnd", "Connect", "ConnectEnd",
    "joint1", "joint2", "joint3", "joint4", "joint5", "joint6",
    "joint7", "joint8", "joint9", "joint10", "joint11", "joint12",
    "joint13", "joint14", "joint15", "joint16", "joint17", "joint18",
    "joint19", "joint20", "joint21", "joint22", "joint23", "joint24",
    "joint25", "joint26", "joint27", "joint28", "joint29", "joint30",
    "joint31", "joint32", "joint33", "joint34", "joint35", "joint36",
    "joint37", "joint38", "joint39", "joint40", "joint41", "joint42",
    "joint43", "joint44", "joint45", "joint46", "joint47", "joint48",
    "joint49", "joint50", "joint51", "joint52", "joint53", "joint54",
    "joint55", "joint56", "joint57", "joint58", "joint59", "joint60",
    "joint61", "joint62", "joint63", "joint64", "joint65", "joint66",
    "joint67", "joint68", "joint69", "joint70", "joint71", "joint72",
    "joint73", "joint74", "joint75", "joint76", "joint77", "joint78",
    "joint79", "joint80", "joint81", "joint82", "joint83", "joint84",
    "joint85", "joint86", "joint87", "joint88", "joint89", "joint90",
    "joint91", "joint92", "joint93", "joint94", "joint95", "joint96",
    "joint97", "joint98", "joint99", "joint100", "joint101", "joint102",
    "joint103", "joint104", "joint105", "joint106", "joint107", "joint108",
    "joint109", "joint110", "joint111", "joint112", "joint113", "joint114",
    "joint115", "joint116", "joint117", "joint118", "joint119", "joint120",
    "joint121", "joint122", "joint123", "joint124", "joint125", "joint126",
    "joint127", "joint128", "joint129", "joint130", "joint131", "joint132",
    "joint133", "joint134", "joint135", "joint136", "joint137", "joint138",
    "joint139", "joint140", "joint141", "joint142", "joint143", "joint144",
    "joint145", "joint146", "joint147", "joint148", "joint149", "joint150",
    "joint151", "joint152", "joint153", "joint154", "joint155", "joint156",
    "joint157", "joint158", "joint159", "joint160", "joint161", "joint162",
    "joint163", "joint164", "joint165", "joint166", "joint167", "joint168",
    "joint169", "joint170", "joint171", "joint172", "joint173", "joint174",
    "joint175", "joint176", "joint177", "joint178", "joint179", "joint180",
    "joint181", "joint182", "joint183", "joint184", "joint185", "joint186",
    "joint187", "joint188", "joint189", "joint190", "joint191", "joint192",
    "joint193", "joint194", "joint195", "joint196", "joint197", "joint198",
    "joint199", "joint200", "joint201",
  };
  const short RopeParents[207] = {
    -1, 0, 1, 2, 1, 4, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
  };

  // small ash tree (skinned): no file refers to the asset;
  //   objects/props/vegetation/ashtree_01/treeash_small_01_skinned (VegetationTreeEntityData) carries the same
  //   parents itself
  const char* const AshTreeNames[17] = {
    "TreeAsh_Small_01_Skeleton", "Root", "joint3", "Break1", "joint5", "joint6",
    "joint7", "joint8", "joint9", "joint10", "joint11", "Break2",
    "joint13", "joint14", "joint15", "joint16", "joint17",
  };
  const short AshTreeParents[17] = {
    -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 6, 10, 11, 12, 13, 14, 15,
  };

  // leaning tree 1 (skinned): no file refers to the asset; leaning_tree_l_01_skinned (VegetationTreeEntityData)
  //   carries the same parents itself
  const char* const LeaningTree1Names[13] = {
    "Leaning_Tree_L_01_Skinned_Skeleton", "joint", "joint1", "joint2", "joint3", "joint4",
    "joint5", "joint6", "joint7", "joint8", "joint9", "joint10",
    "joint11",
  };
  const short LeaningTree1Parents[13] = {
    -1, 0, 1, 2, 3, 4, 5, 6, 3, 8, 9, 10, 11,
  };

  // leaning tree 2 (skinned): no file refers to the asset; leaning_tree_l_02_skinned (VegetationTreeEntityData)
  //   carries the same parents itself
  const char* const LeaningTree2Names[28] = {
    "Skeleton", "Stem0", "Stem1", "Stem3", "Stem2", "Stem4",
    "Joint0", "Joint1", "Joint2", "Joint3", "Joint4", "Joint5",
    "Joint6", "Joint7", "Joint8", "Joint9", "Joint11", "Joint12",
    "Joint13", "Joint14", "Joint17", "Joint18", "Joint23", "Joint24",
    "Joint25", "Joint26", "Joint27", "Joint28",
  };
  const short LeaningTree2Parents[28] = {
    -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 8, 11, 5, 13, 14, 14, 16, 17, 13, 19, 20, 5, 22,
    23, 24, 25, 23,
  };

  const Skeleton Skeletons[] = {
    { "characters/skeletons/skeleton_faith", "Faith", 182, FaithNames, FaithParents, 4 },
    { "characters/skeletons/skeleton_enemy", "KrugerSec", 163, EnemyNames, EnemyParents, 4 },
    { "characters/skeletons/skeleton_male", "man (or woman in heels)", 162, MaleNames, MaleParents, 4 },
    { "characters/skeletons/skeleton_female", "woman", 169, FemaleNames, FemaleParents, 4 },
    { "characters/skeletons/skeleton_enemy_balc", "KrugerSec (low detail)", 83, EnemyLowNames, EnemyLowParents, 4 },
    { "characters/skeletons/skeleton_male_balc", "person (low detail)", 82, PersonLowNames, PersonLowParents, 4 },
    { "characters/skeletons/skeleton_pedestrian", "crowd person", 28, CrowdNames, CrowdParents, 4 },
    { "characters/skeletons/skeleton_drone", "drone", 49, DroneNames, DroneParents, 2 },
    { "characters/skeletons/skeleton_kumabear01", "bear mascot", 47, BearNames, BearParents, 6 },
    { "animations/skeletons/weapon_ske", "weapon (gun, baton or knife)", 26, WeaponNames, WeaponParents, 2 },
    { "characters/skeletons/skeleton_pigeon", "pigeon", 83, PigeonNames, PigeonParents, 6 },
    { "characters/misc/rat/skeleton_rat", "rat", 23, RatNames, RatParents, 2 },
    { "animations/skeletons/propscommon_ske", "animated prop (door, collectible box...)", 14, PropNames, PropParents, 7 },
    { "objects/architecture/unique/kingdom/cells/mecharm_01/mecharm_01_ske", "mechanical arm (Kingdom cells)", 17, MechArmNames, MechArmParents, 6 },
    { "animations/skeletons/magrope_ske", "MagRope or zip line rope", 207, RopeNames, RopeParents, 6 },
    { "objects/props/vegetation/ashtree_01/ashtree_01_skeleton", "small ash tree (skinned)", 17, AshTreeNames, AshTreeParents, 1 },
    { "objects/props/vegetation/leaningtree_01/leaning_tree_l_01_skinned_skeleton", "leaning tree 1 (skinned)", 13, LeaningTree1Names, LeaningTree1Parents, 1 },
    { "objects/props/vegetation/leaningtree_02/leaning_tree_l_02_skeleton", "leaning tree 2 (skinned)", 28, LeaningTree2Names, LeaningTree2Parents, 1 },
  };

  // The skeletons whose movable bones sit on AITrajectory side by side: HeldRoot above is only the one with the
  // most bones below it. Holding the whole thing means holding each of these and the bones below it.
  struct TopJoints
  {
    int Skeleton;               // index in Skeletons
    const short* Bones;
    int Count;
  };
  const short PropTopJoints[5] = {
    6, 7, 11, 12, 13,
  };
  const short RopeTopJoints[201] = {
    6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29,
    30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 52, 53,
    54, 55, 56, 57, 58, 59, 60, 61, 62, 63, 64, 65, 66, 67, 68, 69, 70, 71, 72, 73, 74, 75, 76, 77,
    78, 79, 80, 81, 82, 83, 84, 85, 86, 87, 88, 89, 90, 91, 92, 93, 94, 95, 96, 97, 98, 99, 100, 101,
    102, 103, 104, 105, 106, 107, 108, 109, 110, 111, 112, 113, 114, 115, 116, 117, 118, 119, 120, 121, 122, 123, 124, 125,
    126, 127, 128, 129, 130, 131, 132, 133, 134, 135, 136, 137, 138, 139, 140, 141, 142, 143, 144, 145, 146, 147, 148, 149,
    150, 151, 152, 153, 154, 155, 156, 157, 158, 159, 160, 161, 162, 163, 164, 165, 166, 167, 168, 169, 170, 171, 172, 173,
    174, 175, 176, 177, 178, 179, 180, 181, 182, 183, 184, 185, 186, 187, 188, 189, 190, 191, 192, 193, 194, 195, 196, 197,
    198, 199, 200, 201, 202, 203, 204, 205, 206,
  };
  const TopJoints SeveralTopJoints[] = {
    { 12, PropTopJoints, 5 },
    { 14, RopeTopJoints, 201 },
  };
}
