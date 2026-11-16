#pragma once
// The game's own face poses that the feeling layer is made of. Made by gen_feel_tables.py from the game's data
// (the ANT bank sp_maincity_win32_antstate, FacePoseLibraryAsset, as decoded by the game-data research into
// facepose_tables.tsv): not to be edited by hand. Two libraries whose bind pose is exactly a rest pose of the tools:
// Faith (on her rest pose) and Gabriel (on the man's). Per pose, the LEFT side's bones and the middle's, each as a
// move (mm along the head's Right / Up / Forward, added to the bone's local translation) and a turn (a rotation
// vector in degrees along the same axes; weights add the vectors, as the game does: the bind's local rotation is the
// identity on every face bone). A two-sided pose is the mean of the game's left pose and the mirror image of its
// right one (exactly symmetric); a pose of the middle is made symmetric against its own mirror image. A right bone
// (Twin) takes the left one's move mirrored (-right, up, forward) and turn mirrored (right, -up, -forward).
// UpLidClose / LowLidClose: how far the pose takes the upper / lower lid bone round the blink's line (1 mm under
// the eye bone, 4 mm behind it) on the library's own rest pose, in degrees, > 0 closing. BlinkAngle: the life's
// whole blink on that rest pose (LidSweep 1.1 x the angle between the two lid bones seen from the line).
namespace voice_feel_tables
{
  enum PoseId
  {
    BrowInUp = 0,   // the game's LBrowInUp_01 / RBrowInUp_03
    BrowOutUp,   // the game's LBrowOutUp_02 / RBrowOutUp_04
    BrowMiddleUp,   // the game's LBrowMiddleUp_84 / RBrowMiddleUp_85
    BrowInDown,   // the game's LBrowInDn_05 / RBrowInDn_07
    BrowIn,   // the game's LBrowIn_74 / RBrowIn_75
    BrowMiddleDown,   // the game's LBrowMiddleDn_86 / RBrowMiddleDn_87
    BrowOutDown,   // the game's LBrowOutDn_06 / RBrowOutDn_08
    UpperLidUp,   // the game's LUprLidUp_09 / RUprLidUp_10
    LowerLidUp,   // the game's LLwrLidUp_11 / RLwrLidUp_12
    LidClose,   // the game's LLidClose_13 / RLidClose_14
    LidSqueeze,   // the game's LLidSqueeze_96 / RLidSqueeze_97
    CheekUp,   // the game's LCheekUp_23 / RCheekUp_24
    NoseUp,   // the game's LNoseUp_29 / RNoseUp_30
    UpperLipUp,   // the game's LUprLipUp_33 / RUprLipUp_34
    LipCornerUp,   // the game's LLipCornerUp_35 / RLipCornerUp_36
    LipCornerDown,   // the game's LLipCornerDn_37 / RLipCornerDn_38
    LowerLipDown,   // the game's LLwrLipDn_39 / RLwrLipDn_40
    LipStretch,   // the game's LLipStretch_41 / RLipStretch_42
    LipCompress,   // the game's LLipCompress_43 / RLipCompress_44
    UpperLipDownMiddle,   // the game's UprLipDn_45 (middle)
    LowerLipUpMiddle,   // the game's LwrLipUp_46 (middle)
    UpperLipCurlIn,   // the game's UprLipCurlIn_51 (middle)
    LowerLipCurlIn,   // the game's LwrLipCurlIn_52 (middle)
    UpperLipBack,   // the game's UprLipBk_55 (middle)
    LowerLipBack,   // the game's LwrLipBk_56 (middle)
    ChinUp,   // the game's ChinUp_62 (middle)
    PoseCount
  };

  // Every bone a pose moves (a left or middle bone, then its right twin)
  const char* const BoneNames[] = {
    "LeftInnerEyebrow", "RightInnerEyebrow", "LeftLowOuterEyebrow", "RightLowOuterEyebrow",
    "LeftOuterEyebrow", "RightOuterEyebrow", "LeftLowMiddleEyebrow", "RightLowMiddleEyebrow",
    "LeftMiddleEyebrow", "RightMiddleEyebrow", "LeftLowEyelid", "RightLowEyelid",
    "LeftUpEyelid", "RightUpEyelid", "LeftLowEyelidCrease", "RightLowEyelidCrease",
    "LeftUpCheek", "RightUpCheek", "LeftCheek", "RightCheek",
    "LeftCrease", "RightCrease", "LeftMiddleCrease", "RightMiddleCrease",
    "LeftNose", "RightNose", "LeftUpInnerCheek", "RightUpInnerCheek",
    "LowNose", "NoseTip", "LeftMouth", "RightMouth",
    "LeftUpperInnerLip", "RightUpperInnerLip", "LeftUpperLip", "RightUpperLip",
    "LeftLowCheek", "RightLowCheek", "LeftLowerInnerLip", "RightLowerInnerLip",
    "LeftLowerLip", "RightLowerLip", "LowerInnerLip", "LowerLip",
    "UpperLip", "LeftInnerMouth", "RightInnerMouth", "UpperInnerLip",
    "Chin",
  };
  const int BoneCount = (int)(sizeof(BoneNames) / sizeof(BoneNames[0]));

  struct Entry
  {
    short Bone;         // BoneNames: a Left... bone, or one of the middle
    short Twin;         // its right twin (-1: a bone of the middle)
    float Move[3];      // mm: right, up, forward
    float Turn[3];      // degrees: a rotation vector along right, up, forward
  };

  struct Pose
  {
    const char* Game;   // the game's pose names
    bool Sided;         // a left / right pair (false: a pose of the middle, laid on both sides at once)
    int First, Count;   // its entries
    float UpLidClose, LowLidClose;
  };

  struct Library
  {
    const char* Name;
    const char* Rest;   // the rest pose its bind pose is
    float EyesApart;    // mm between the eye bones on that rest pose
    float EyesToMouth;  // mm from the middle between the eye bones to the middle between the mouth corners
    float BlinkAngle;   // degrees
    const Pose* Poses;
    const Entry* Entries;
    int EntryCount;
  };

  // Faith: the game's library "Faith", bind pose = the Faith rest pose
  const Entry FaithEntries[] = {
    // BrowInUp (LBrowInUp_01 / RBrowInUp_03)
    { 0, 1, { 1.192f, 10.607f, -2.022f }, { 2.548f, 0.001f, 0.000f } },   // LeftInnerEyebrow
    // BrowOutUp (LBrowOutUp_02 / RBrowOutUp_04)
    { 2, 3, { -0.001f, 5.097f, 0.000f }, { 0.000f, 0.000f, 0.000f } },   // LeftLowOuterEyebrow
    { 4, 5, { -0.942f, 11.764f, -1.668f }, { 2.561f, -1.011f, -0.068f } },   // LeftOuterEyebrow
    // BrowMiddleUp (LBrowMiddleUp_84 / RBrowMiddleUp_85)
    { 6, 7, { -0.003f, 6.319f, -0.023f }, { 0.000f, 0.000f, 0.000f } },   // LeftLowMiddleEyebrow
    { 8, 9, { 2.016f, 13.799f, -1.551f }, { 0.000f, 0.000f, 0.000f } },   // LeftMiddleEyebrow
    // BrowInDown (LBrowInDn_05 / RBrowInDn_07)
    { 0, 1, { 3.429f, -4.065f, 1.284f }, { 2.550f, 0.000f, -0.001f } },   // LeftInnerEyebrow
    // BrowIn (LBrowIn_74 / RBrowIn_75)
    { 0, 1, { 1.708f, -0.080f, 0.587f }, { 5.774f, 6.435f, -4.297f } },   // LeftInnerEyebrow
    { 8, 9, { 1.587f, 0.006f, 0.534f }, { 0.000f, 0.000f, 0.000f } },   // LeftMiddleEyebrow
    { 4, 5, { 0.407f, 0.089f, 0.358f }, { 0.000f, -0.601f, 0.000f } },   // LeftOuterEyebrow
    // BrowMiddleDown (LBrowMiddleDn_86 / RBrowMiddleDn_87)
    { 6, 7, { 0.478f, -2.282f, 1.679f }, { -19.747f, 6.554f, -2.311f } },   // LeftLowMiddleEyebrow
    { 8, 9, { 3.495f, -4.093f, 2.354f }, { -5.211f, -2.930f, 1.765f } },   // LeftMiddleEyebrow
    // BrowOutDown (LBrowOutDn_06 / RBrowOutDn_08)
    { 2, 3, { 0.078f, -1.948f, 0.000f }, { 0.639f, 5.838f, -1.950f } },   // LeftLowOuterEyebrow
    { 4, 5, { 2.154f, -2.792f, 2.033f }, { -0.259f, -1.715f, 2.843f } },   // LeftOuterEyebrow
    // UpperLidUp (LUprLidUp_09 / RUprLidUp_10)
    { 10, 11, { 0.000f, -0.065f, -0.028f }, { 0.000f, 0.000f, 0.000f } },   // LeftLowEyelid
    { 6, 7, { 0.068f, 2.215f, 0.005f }, { -1.023f, -1.778f, 11.701f } },   // LeftLowMiddleEyebrow
    { 2, 3, { -0.178f, 1.768f, 0.000f }, { 3.743f, 1.615f, -8.503f } },   // LeftLowOuterEyebrow
    { 12, 13, { 0.025f, 4.880f, -0.710f }, { -0.990f, -0.515f, 5.953f } },   // LeftUpEyelid
    // LowerLidUp (LLwrLidUp_11 / RLwrLidUp_12)
    { 10, 11, { 1.093f, 2.766f, 1.307f }, { -1.342f, -5.020f, -1.091f } },   // LeftLowEyelid
    { 14, 15, { 1.217f, 1.176f, 0.698f }, { -2.204f, -2.977f, -1.458f } },   // LeftLowEyelidCrease
    // LidClose (LLidClose_13 / RLidClose_14)
    { 10, 11, { 1.643f, -0.225f, -0.164f }, { 2.612f, -7.344f, -2.620f } },   // LeftLowEyelid
    { 14, 15, { 1.395f, 0.776f, 1.939f }, { -4.697f, 0.148f, -3.627f } },   // LeftLowEyelidCrease
    { 6, 7, { -0.058f, -0.944f, 0.547f }, { 0.000f, 0.000f, 0.000f } },   // LeftLowMiddleEyebrow
    { 2, 3, { -0.095f, -0.831f, 0.000f }, { 0.000f, 0.000f, 0.000f } },   // LeftLowOuterEyebrow
    { 12, 13, { -1.695f, -12.256f, -1.425f }, { -43.198f, 7.210f, -2.707f } },   // LeftUpEyelid
    // LidSqueeze (LLidSqueeze_96 / RLidSqueeze_97)
    { 0, 1, { 0.000f, -1.408f, 0.000f }, { 0.000f, 0.000f, 0.000f } },   // LeftInnerEyebrow
    { 10, 11, { 0.000f, -0.017f, 0.328f }, { 1.483f, 0.000f, -0.001f } },   // LeftLowEyelid
    { 6, 7, { 1.398f, -6.696f, 0.289f }, { 0.000f, 0.000f, 0.000f } },   // LeftLowMiddleEyebrow
    { 2, 3, { 1.683f, -7.035f, 0.000f }, { 0.000f, 0.000f, 0.000f } },   // LeftLowOuterEyebrow
    { 8, 9, { 0.941f, -0.835f, 0.441f }, { 0.000f, 0.000f, 0.000f } },   // LeftMiddleEyebrow
    { 4, 5, { 0.914f, -1.617f, 0.896f }, { 0.000f, 0.000f, 0.000f } },   // LeftOuterEyebrow
    { 16, 17, { 0.445f, 0.495f, 0.350f }, { 0.000f, 0.000f, 0.000f } },   // LeftUpCheek
    // CheekUp (LCheekUp_23 / RCheekUp_24)
    { 18, 19, { 1.115f, 5.213f, 2.089f }, { -1.015f, 0.544f, -2.195f } },   // LeftCheek
    { 20, 21, { -2.004f, 2.577f, -1.409f }, { -1.614f, -0.005f, 1.994f } },   // LeftCrease
    { 2, 3, { 3.718f, -4.361f, 0.000f }, { 0.000f, 0.000f, 0.000f } },   // LeftLowOuterEyebrow
    { 22, 23, { -1.002f, 1.543f, -0.796f }, { -2.506f, -7.189f, -0.810f } },   // LeftMiddleCrease
    { 24, 25, { -0.095f, 0.029f, -0.036f }, { 0.000f, 0.000f, 0.000f } },   // LeftNose
    { 4, 5, { 3.194f, -6.642f, 0.076f }, { 6.378f, -0.285f, -7.576f } },   // LeftOuterEyebrow
    { 16, 17, { 1.461f, 5.889f, 3.328f }, { -0.574f, 1.840f, 3.904f } },   // LeftUpCheek
    // NoseUp (LNoseUp_29 / RNoseUp_30)
    { 20, 21, { 0.141f, 5.078f, -0.571f }, { 3.159f, 0.867f, 0.595f } },   // LeftCrease
    { 14, 15, { 0.334f, 0.277f, 0.518f }, { -0.001f, 0.000f, -0.807f } },   // LeftLowEyelidCrease
    { 24, 25, { -0.342f, 5.452f, -0.466f }, { -4.627f, -0.875f, 2.066f } },   // LeftNose
    { 26, 27, { 0.000f, 2.661f, 2.577f }, { 4.085f, 0.462f, -0.937f } },   // LeftUpInnerCheek
    { 28, -1, { 0.000f, 0.578f, -0.745f }, { 0.000f, 0.000f, 0.000f } },   // LowNose
    { 29, -1, { 0.000f, 0.000f, 0.000f }, { -2.835f, 0.000f, 0.000f } },   // NoseTip
    // UpperLipUp (LUprLipUp_33 / RUprLipUp_34)
    { 18, 19, { 0.000f, 1.002f, -0.011f }, { 0.000f, 0.000f, 0.000f } },   // LeftCheek
    { 20, 21, { -4.520f, 7.022f, -5.991f }, { -10.989f, 7.994f, 1.548f } },   // LeftCrease
    { 30, 31, { 0.000f, 1.324f, -0.785f }, { 0.354f, 3.351f, 0.470f } },   // LeftMouth
    { 24, 25, { 0.000f, 1.020f, -0.370f }, { -1.517f, 0.000f, 0.000f } },   // LeftNose
    { 16, 17, { -0.776f, 0.671f, 0.256f }, { -0.001f, 0.000f, 1.030f } },   // LeftUpCheek
    { 26, 27, { 0.000f, 0.149f, -0.002f }, { -0.001f, 0.000f, 0.810f } },   // LeftUpInnerCheek
    { 32, 33, { 0.524f, -3.257f, -1.907f }, { 6.002f, 2.288f, -18.361f } },   // LeftUpperInnerLip
    { 34, 35, { -4.126f, 10.246f, -3.638f }, { 15.910f, 12.678f, 5.286f } },   // LeftUpperLip
    // LipCornerUp (LLipCornerUp_35 / RLipCornerUp_36)
    { 20, 21, { -0.438f, 11.773f, -7.648f }, { -32.326f, -30.160f, -3.591f } },   // LeftCrease
    { 36, 37, { 0.000f, 1.542f, -0.718f }, { 0.000f, 0.000f, 0.000f } },   // LeftLowCheek
    { 38, 39, { -0.007f, 0.328f, 0.019f }, { 0.000f, 0.000f, 0.000f } },   // LeftLowerInnerLip
    { 40, 41, { 0.581f, 2.195f, -0.192f }, { -0.366f, 7.718f, -0.916f } },   // LeftLowerLip
    { 22, 23, { -2.814f, 3.462f, 2.452f }, { 9.978f, 1.596f, -1.897f } },   // LeftMiddleCrease
    { 30, 31, { -2.154f, 11.331f, 0.314f }, { -38.258f, 7.258f, 12.223f } },   // LeftMouth
    { 16, 17, { -0.888f, 0.553f, 0.689f }, { 3.807f, 0.300f, 1.921f } },   // LeftUpCheek
    { 32, 33, { 1.117f, -1.890f, 1.286f }, { 8.781f, 2.162f, -17.505f } },   // LeftUpperInnerLip
    { 34, 35, { 1.030f, 3.381f, -2.171f }, { -11.041f, 9.070f, 20.209f } },   // LeftUpperLip
    { 42, -1, { 0.000f, -0.036f, 0.662f }, { 0.000f, 0.000f, 0.000f } },   // LowerInnerLip
    { 43, -1, { 0.000f, 0.315f, -0.649f }, { 0.734f, 0.000f, 0.000f } },   // LowerLip
    { 44, -1, { 0.000f, 0.462f, -0.347f }, { 0.000f, 0.000f, 0.000f } },   // UpperLip
    // LipCornerDown (LLipCornerDn_37 / RLipCornerDn_38)
    { 18, 19, { 0.000f, -2.130f, -0.387f }, { -1.705f, -0.001f, 0.001f } },   // LeftCheek
    { 20, 21, { -0.300f, -3.670f, -0.522f }, { -2.525f, 4.772f, 0.540f } },   // LeftCrease
    { 36, 37, { 0.000f, -4.782f, -0.306f }, { 4.574f, -5.844f, 2.754f } },   // LeftLowCheek
    { 40, 41, { 0.137f, -1.334f, -2.697f }, { -1.672f, -0.176f, -8.413f } },   // LeftLowerLip
    { 22, 23, { -0.754f, -3.000f, -0.658f }, { 2.643f, -0.555f, -6.498f } },   // LeftMiddleCrease
    { 30, 31, { 0.458f, -7.409f, -0.973f }, { 10.801f, 0.030f, 0.013f } },   // LeftMouth
    { 24, 25, { 0.000f, -0.335f, 0.000f }, { 0.000f, 0.000f, 0.000f } },   // LeftNose
    { 16, 17, { 0.000f, -0.138f, -0.008f }, { -1.110f, 0.000f, -0.001f } },   // LeftUpCheek
    { 34, 35, { 0.054f, -1.364f, -0.062f }, { -0.001f, -0.029f, -1.103f } },   // LeftUpperLip
    { 43, -1, { 0.000f, -0.412f, 0.004f }, { 0.000f, 0.000f, 0.000f } },   // LowerLip
    { 44, -1, { 0.000f, -0.147f, -0.010f }, { 0.000f, 0.000f, 0.000f } },   // UpperLip
    // LowerLipDown (LLwrLipDn_39 / RLwrLipDn_40)
    { 45, 46, { -1.983f, 0.000f, 0.000f }, { 0.000f, 0.000f, 0.000f } },   // LeftInnerMouth
    { 36, 37, { -1.305f, -0.581f, -1.004f }, { -4.129f, -3.659f, 1.218f } },   // LeftLowCheek
    { 38, 39, { 0.000f, 0.000f, 0.000f }, { -2.399f, -12.971f, 20.862f } },   // LeftLowerInnerLip
    { 40, 41, { -1.743f, -9.983f, -1.004f }, { 5.175f, 32.043f, 0.223f } },   // LeftLowerLip
    { 30, 31, { 0.000f, -1.030f, 0.012f }, { 0.000f, 0.000f, 0.000f } },   // LeftMouth
    // LipStretch (LLipStretch_41 / RLipStretch_42)
    { 18, 19, { -6.505f, 0.348f, -2.873f }, { 1.188f, 10.388f, -1.339f } },   // LeftCheek
    { 20, 21, { -6.645f, 3.216f, -5.681f }, { -32.752f, -29.483f, -9.153f } },   // LeftCrease
    { 45, 46, { 2.695f, 0.466f, -0.695f }, { -0.001f, 19.608f, 0.000f } },   // LeftInnerMouth
    { 36, 37, { 0.523f, 4.099f, 0.780f }, { -8.468f, -0.558f, -5.094f } },   // LeftLowCheek
    { 38, 39, { -0.185f, -2.149f, 0.377f }, { -6.816f, 0.967f, -11.263f } },   // LeftLowerInnerLip
    { 40, 41, { -3.115f, 4.038f, -0.985f }, { -7.810f, -0.169f, 16.668f } },   // LeftLowerLip
    { 22, 23, { 6.880f, 3.282f, 19.397f }, { 4.215f, -5.115f, 1.306f } },   // LeftMiddleCrease
    { 30, 31, { -7.704f, 2.366f, -17.532f }, { -4.677f, -20.168f, 7.139f } },   // LeftMouth
    { 24, 25, { -0.221f, 0.066f, -0.155f }, { 0.000f, 0.000f, 0.000f } },   // LeftNose
    { 16, 17, { 0.000f, 0.011f, 1.015f }, { 3.538f, -0.220f, 1.710f } },   // LeftUpCheek
    { 32, 33, { 0.824f, -0.145f, 0.087f }, { -0.565f, 1.261f, -10.305f } },   // LeftUpperInnerLip
    { 34, 35, { -1.761f, 0.809f, -0.919f }, { -1.070f, -6.244f, 19.958f } },   // LeftUpperLip
    { 43, -1, { 0.000f, -0.004f, -0.311f }, { 0.000f, 0.000f, 0.000f } },   // LowerLip
    { 47, -1, { 0.000f, 0.000f, 0.728f }, { 0.000f, 0.000f, 0.000f } },   // UpperInnerLip
    { 44, -1, { 0.000f, 0.368f, -0.978f }, { 0.000f, 0.000f, 0.000f } },   // UpperLip
    // LipCompress (LLipCompress_43 / RLipCompress_44)
    { 18, 19, { 2.163f, -0.477f, 2.867f }, { 0.273f, -4.482f, -0.033f } },   // LeftCheek
    { 20, 21, { 1.337f, -0.063f, 1.048f }, { -3.455f, 5.149f, -6.171f } },   // LeftCrease
    { 45, 46, { -0.389f, 0.000f, -8.601f }, { -0.357f, 1.279f, -14.591f } },   // LeftInnerMouth
    { 38, 39, { -0.178f, 2.750f, -0.069f }, { -0.000f, -0.105f, -9.272f } },   // LeftLowerInnerLip
    { 40, 41, { 3.620f, -0.469f, 5.357f }, { 16.735f, -8.521f, 17.666f } },   // LeftLowerLip
    { 22, 23, { -2.800f, 0.465f, -9.280f }, { 6.510f, -15.246f, -0.882f } },   // LeftMiddleCrease
    { 30, 31, { 8.518f, 1.224f, 13.380f }, { 0.000f, 0.000f, 0.000f } },   // LeftMouth
    { 16, 17, { 0.997f, -0.457f, -0.067f }, { -0.001f, 0.000f, -1.223f } },   // LeftUpCheek
    { 32, 33, { 1.357f, -0.040f, -3.262f }, { 1.520f, 0.162f, 14.276f } },   // LeftUpperInnerLip
    { 34, 35, { 4.768f, 0.953f, 2.268f }, { -4.892f, 13.566f, 4.874f } },   // LeftUpperLip
    { 42, -1, { 0.000f, -0.396f, -0.005f }, { 4.698f, 0.000f, 0.000f } },   // LowerInnerLip
    { 43, -1, { 0.000f, 1.106f, -0.027f }, { -0.512f, 0.000f, 0.000f } },   // LowerLip
    { 47, -1, { 0.000f, 0.913f, -0.555f }, { 0.000f, 0.000f, 0.000f } },   // UpperInnerLip
    { 44, -1, { 0.000f, 0.395f, -0.994f }, { 1.318f, 0.000f, 0.000f } },   // UpperLip
    // UpperLipDownMiddle (UprLipDn_45)
    { 28, -1, { 0.000f, -7.605f, 1.752f }, { 0.000f, 0.000f, 0.000f } },   // LowNose
    { 44, -1, { 0.000f, -10.335f, -6.726f }, { -13.664f, 0.000f, 0.000f } },   // UpperLip
    // LowerLipUpMiddle (LwrLipUp_46)
    { 43, -1, { 0.000f, 10.898f, 0.805f }, { 16.319f, 0.000f, 0.000f } },   // LowerLip
    // UpperLipCurlIn (UprLipCurlIn_51)
    { 20, 21, { 0.000f, 0.000f, 0.000f }, { 1.382f, 0.000f, 0.000f } },   // LeftCrease
    { 34, 35, { 0.000f, -0.780f, -1.412f }, { -24.695f, -5.766f, -10.088f } },   // LeftUpperLip
    { 44, -1, { 0.000f, 1.198f, -1.003f }, { -47.146f, 0.000f, 0.000f } },   // UpperLip
    // LowerLipCurlIn (LwrLipCurlIn_52)
    { 38, 39, { -0.784f, 2.537f, 2.192f }, { -28.419f, -25.729f, 26.980f } },   // LeftLowerInnerLip
    { 40, 41, { -0.181f, 0.512f, 1.550f }, { 53.454f, -7.934f, 21.081f } },   // LeftLowerLip
    { 43, -1, { 0.000f, 0.000f, 0.000f }, { 51.040f, 0.000f, 0.000f } },   // LowerLip
    // UpperLipBack (UprLipBk_55)
    { 34, 35, { 0.000f, -0.201f, -2.457f }, { 0.000f, 0.000f, 0.000f } },   // LeftUpperLip
    { 44, -1, { 0.000f, -0.187f, -5.119f }, { 0.000f, 0.000f, 0.000f } },   // UpperLip
    // LowerLipBack (LwrLipBk_56)
    { 40, 41, { 0.000f, 0.447f, -2.464f }, { 0.000f, 0.000f, 0.000f } },   // LeftLowerLip
    { 43, -1, { 0.000f, 0.461f, -5.127f }, { 0.000f, 0.000f, 0.000f } },   // LowerLip
    // ChinUp (ChinUp_62)
    { 48, -1, { 0.000f, 4.892f, 1.804f }, { 11.452f, 0.000f, 0.000f } },   // Chin
  };
  const Pose FaithPoses[PoseCount] = {
    { "LBrowInUp_01 / RBrowInUp_03", true, 0, 1, 0.000f, 0.000f },
    { "LBrowOutUp_02 / RBrowOutUp_04", true, 1, 2, 0.000f, 0.000f },
    { "LBrowMiddleUp_84 / RBrowMiddleUp_85", true, 3, 2, 0.000f, 0.000f },
    { "LBrowInDn_05 / RBrowInDn_07", true, 5, 1, 0.000f, 0.000f },
    { "LBrowIn_74 / RBrowIn_75", true, 6, 3, 0.000f, 0.000f },
    { "LBrowMiddleDn_86 / RBrowMiddleDn_87", true, 9, 2, 0.000f, 0.000f },
    { "LBrowOutDn_06 / RBrowOutDn_08", true, 11, 2, 0.000f, 0.000f },
    { "LUprLidUp_09 / RUprLidUp_10", true, 13, 4, -14.238f, -0.229f },
    { "LLwrLidUp_11 / RLwrLidUp_12", true, 17, 2, 0.000f, 9.584f },
    { "LLidClose_13 / RLidClose_14", true, 19, 5, 37.838f, -0.866f },
    { "LLidSqueeze_96 / RLidSqueeze_97", true, 24, 7, 0.000f, 0.283f },
    { "LCheekUp_23 / RCheekUp_24", true, 31, 7, 0.000f, 0.000f },
    { "LNoseUp_29 / RNoseUp_30", true, 38, 6, 0.000f, 0.000f },
    { "LUprLipUp_33 / RUprLipUp_34", true, 44, 8, 0.000f, 0.000f },
    { "LLipCornerUp_35 / RLipCornerUp_36", true, 52, 12, 0.000f, 0.000f },
    { "LLipCornerDn_37 / RLipCornerDn_38", true, 64, 11, 0.000f, 0.000f },
    { "LLwrLipDn_39 / RLwrLipDn_40", true, 75, 5, 0.000f, 0.000f },
    { "LLipStretch_41 / RLipStretch_42", true, 80, 15, 0.000f, 0.000f },
    { "LLipCompress_43 / RLipCompress_44", true, 95, 14, 0.000f, 0.000f },
    { "UprLipDn_45", false, 109, 2, 0.000f, 0.000f },
    { "LwrLipUp_46", false, 111, 1, 0.000f, 0.000f },
    { "UprLipCurlIn_51", false, 112, 3, 0.000f, 0.000f },
    { "LwrLipCurlIn_52", false, 115, 3, 0.000f, 0.000f },
    { "UprLipBk_55", false, 118, 2, 0.000f, 0.000f },
    { "LwrLipBk_56", false, 120, 2, 0.000f, 0.000f },
    { "ChinUp_62", false, 122, 1, 0.000f, 0.000f },
  };

  // Man: the game's library "Gabriel", bind pose = the man rest pose
  const Entry ManEntries[] = {
    // BrowInUp (LBrowInUp_01 / RBrowInUp_03)
    { 0, 1, { -1.003f, 12.887f, -2.457f }, { 3.096f, 0.001f, 0.000f } },   // LeftInnerEyebrow
    // BrowOutUp (LBrowOutUp_02 / RBrowOutUp_04)
    { 2, 3, { -0.005f, 6.525f, 0.000f }, { 0.000f, 0.000f, 0.000f } },   // LeftLowOuterEyebrow
    { 4, 5, { -2.459f, 14.587f, -3.272f }, { 3.165f, -1.194f, -5.606f } },   // LeftOuterEyebrow
    // BrowMiddleUp (LBrowMiddleUp_84 / RBrowMiddleUp_85)
    { 6, 7, { -0.004f, 6.857f, -0.044f }, { 0.024f, -0.001f, 1.601f } },   // LeftLowMiddleEyebrow
    { 8, 9, { -0.001f, 16.765f, -1.884f }, { 0.000f, 0.000f, 0.000f } },   // LeftMiddleEyebrow
    // BrowInDown (LBrowInDn_05 / RBrowInDn_07)
    { 0, 1, { 1.913f, -4.066f, 1.283f }, { -1.075f, 0.000f, 0.000f } },   // LeftInnerEyebrow
    // BrowIn (LBrowIn_74 / RBrowIn_75)
    { 0, 1, { 1.708f, -0.080f, 0.587f }, { 5.774f, 6.436f, -4.296f } },   // LeftInnerEyebrow
    { 8, 9, { 1.587f, 0.007f, 0.534f }, { 0.000f, 0.000f, 0.000f } },   // LeftMiddleEyebrow
    { 4, 5, { 0.407f, 0.090f, 0.358f }, { 0.001f, -0.601f, 0.000f } },   // LeftOuterEyebrow
    // BrowMiddleDown (LBrowMiddleDn_86 / RBrowMiddleDn_87)
    { 6, 7, { 0.000f, -2.282f, 1.679f }, { -19.745f, 6.555f, -2.310f } },   // LeftLowMiddleEyebrow
    { 8, 9, { 2.433f, -3.974f, 1.864f }, { 0.358f, -2.941f, 5.944f } },   // LeftMiddleEyebrow
    // BrowOutDown (LBrowOutDn_06 / RBrowOutDn_08)
    { 2, 3, { 0.000f, -1.948f, 0.000f }, { 0.639f, 5.838f, -1.950f } },   // LeftLowOuterEyebrow
    { 4, 5, { 0.200f, -2.654f, -0.116f }, { -0.259f, -1.714f, 2.843f } },   // LeftOuterEyebrow
    // UpperLidUp (LUprLidUp_09 / RUprLidUp_10)
    { 10, 11, { 0.000f, -0.313f, -0.283f }, { 0.000f, 0.000f, 0.000f } },   // LeftLowEyelid
    { 6, 7, { 0.000f, 0.569f, 0.000f }, { 0.000f, 0.000f, 0.000f } },   // LeftLowMiddleEyebrow
    { 2, 3, { 0.000f, 0.569f, 0.000f }, { 0.000f, 0.000f, 0.000f } },   // LeftLowOuterEyebrow
    { 12, 13, { -0.005f, 4.645f, -0.331f }, { 4.832f, -0.213f, 5.973f } },   // LeftUpEyelid
    // LowerLidUp (LLwrLidUp_11 / RLwrLidUp_12)
    { 10, 11, { 1.092f, 2.765f, 1.307f }, { -1.342f, -5.021f, -1.091f } },   // LeftLowEyelid
    { 14, 15, { 4.057f, 3.919f, 2.328f }, { -7.036f, -9.692f, -5.298f } },   // LeftLowEyelidCrease
    // LidClose (LLidClose_13 / RLidClose_14)
    { 10, 11, { 1.643f, -0.226f, -0.163f }, { 2.612f, -7.345f, -2.620f } },   // LeftLowEyelid
    { 14, 15, { 1.395f, 0.775f, 1.938f }, { -4.697f, 0.150f, -3.627f } },   // LeftLowEyelidCrease
    { 6, 7, { 0.000f, -2.637f, 1.527f }, { 0.000f, 0.000f, 0.000f } },   // LeftLowMiddleEyebrow
    { 2, 3, { 0.000f, -2.323f, 0.000f }, { 0.000f, 0.000f, 0.000f } },   // LeftLowOuterEyebrow
    { 12, 13, { -1.304f, -12.216f, -0.186f }, { -47.502f, 7.713f, 6.635f } },   // LeftUpEyelid
    // LidSqueeze (LLidSqueeze_96 / RLidSqueeze_97)
    { 14, 15, { -0.009f, 4.887f, 3.403f }, { 0.001f, 0.145f, 12.734f } },   // LeftLowEyelidCrease
    { 6, 7, { 0.000f, -6.184f, 0.000f }, { 0.000f, 0.000f, 0.000f } },   // LeftLowMiddleEyebrow
    { 2, 3, { 0.000f, -8.351f, 0.000f }, { -6.196f, -0.771f, 29.116f } },   // LeftLowOuterEyebrow
    { 8, 9, { -0.221f, -0.419f, -0.127f }, { 0.000f, 0.000f, 0.000f } },   // LeftMiddleEyebrow
    { 16, 17, { 0.260f, 0.350f, 0.237f }, { 0.000f, 0.000f, 0.000f } },   // LeftUpCheek
    // CheekUp (LCheekUp_23 / RCheekUp_24)
    { 18, 19, { 1.530f, 7.157f, 2.868f }, { -1.399f, 0.758f, -3.010f } },   // LeftCheek
    { 20, 21, { -2.751f, 3.538f, -1.934f }, { -2.216f, -0.021f, 2.737f } },   // LeftCrease
    { 22, 23, { -1.374f, 2.119f, -1.093f }, { -3.415f, -9.862f, -1.194f } },   // LeftMiddleCrease
    { 24, 25, { -0.674f, 0.204f, -0.262f }, { 0.000f, 0.000f, 0.000f } },   // LeftNose
    { 16, 17, { -1.700f, 8.084f, 4.569f }, { -0.994f, 5.817f, 2.055f } },   // LeftUpCheek
    // NoseUp (LNoseUp_29 / RNoseUp_30)
    { 20, 21, { 0.141f, 5.079f, -0.571f }, { 3.159f, 0.866f, 0.595f } },   // LeftCrease
    { 14, 15, { 0.333f, 0.278f, 0.518f }, { 0.001f, 0.000f, -0.807f } },   // LeftLowEyelidCrease
    { 24, 25, { -0.342f, 5.452f, -0.466f }, { -4.627f, -0.875f, 2.066f } },   // LeftNose
    { 26, 27, { 0.000f, 2.661f, 2.577f }, { 4.085f, 0.463f, -0.938f } },   // LeftUpInnerCheek
    { 28, -1, { 0.000f, 0.578f, -0.745f }, { 0.000f, 0.000f, 0.000f } },   // LowNose
    { 29, -1, { 0.000f, 0.000f, 0.000f }, { -2.835f, 0.000f, 0.000f } },   // NoseTip
    // UpperLipUp (LUprLipUp_33 / RUprLipUp_34)
    { 18, 19, { 0.000f, 1.116f, -0.013f }, { 0.000f, 0.000f, 0.000f } },   // LeftCheek
    { 20, 21, { -5.038f, 7.828f, -6.678f }, { -12.238f, 8.895f, 1.825f } },   // LeftCrease
    { 30, 31, { 0.000f, 1.476f, -0.875f }, { 0.395f, 3.735f, 0.522f } },   // LeftMouth
    { 24, 25, { 0.000f, 1.137f, -0.412f }, { -1.691f, 0.000f, 0.000f } },   // LeftNose
    { 16, 17, { -0.865f, 0.748f, 0.285f }, { 0.001f, 0.000f, 1.148f } },   // LeftUpCheek
    { 26, 27, { 0.000f, 0.166f, -0.002f }, { 0.001f, 0.000f, 0.903f } },   // LeftUpInnerCheek
    { 32, 33, { 0.674f, -0.994f, -2.604f }, { 7.518f, 20.544f, -5.947f } },   // LeftUpperInnerLip
    { 34, 35, { -4.700f, 8.980f, -4.872f }, { 17.530f, 4.167f, 2.162f } },   // LeftUpperLip
    // LipCornerUp (LLipCornerUp_35 / RLipCornerUp_36)
    { 18, 19, { -3.821f, 2.537f, 4.614f }, { 0.000f, 0.000f, 0.000f } },   // LeftCheek
    { 20, 21, { 0.094f, 6.152f, 1.035f }, { -9.425f, -7.239f, 1.752f } },   // LeftCrease
    { 36, 37, { 2.852f, 7.989f, -3.047f }, { -2.008f, 2.798f, 1.641f } },   // LeftLowCheek
    { 38, 39, { -0.020f, 0.849f, 0.049f }, { 0.000f, 0.000f, 0.000f } },   // LeftLowerInnerLip
    { 40, 41, { -0.116f, 5.690f, 0.084f }, { 3.757f, 20.535f, 1.553f } },   // LeftLowerLip
    { 22, 23, { 1.163f, -1.810f, 3.564f }, { -18.386f, -18.738f, 9.901f } },   // LeftMiddleCrease
    { 30, 31, { -2.800f, 14.705f, -2.275f }, { -5.519f, 13.558f, 5.413f } },   // LeftMouth
    { 16, 17, { 0.000f, 0.633f, 0.000f }, { 0.000f, 0.000f, 0.000f } },   // LeftUpCheek
    { 32, 33, { -0.526f, 1.149f, 0.154f }, { -0.610f, 1.332f, -15.071f } },   // LeftUpperInnerLip
    { 34, 35, { 0.641f, 3.389f, -0.458f }, { 4.332f, 16.098f, 25.222f } },   // LeftUpperLip
    { 42, -1, { 0.000f, -0.094f, 1.714f }, { 0.000f, 0.000f, 0.000f } },   // LowerInnerLip
    { 43, -1, { 0.000f, 0.816f, -1.680f }, { 1.902f, 0.000f, 0.000f } },   // LowerLip
    { 44, -1, { 0.000f, 1.197f, -0.898f }, { 0.000f, 0.000f, 0.000f } },   // UpperLip
    // LipCornerDown (LLipCornerDn_37 / RLipCornerDn_38)
    { 18, 19, { 0.000f, -2.130f, -0.387f }, { -1.706f, 0.000f, 0.001f } },   // LeftCheek
    { 20, 21, { -0.300f, -3.670f, -0.522f }, { -2.523f, 4.771f, 0.539f } },   // LeftCrease
    { 36, 37, { 0.000f, -4.782f, -0.306f }, { 4.575f, -5.845f, 2.753f } },   // LeftLowCheek
    { 40, 41, { 0.138f, -1.335f, -2.696f }, { -1.671f, -0.177f, -8.412f } },   // LeftLowerLip
    { 22, 23, { -0.754f, -3.000f, -0.658f }, { 2.644f, -0.556f, -6.497f } },   // LeftMiddleCrease
    { 30, 31, { 0.458f, -7.409f, -0.973f }, { 14.901f, 6.630f, -7.271f } },   // LeftMouth
    { 24, 25, { 0.000f, -0.335f, 0.000f }, { 0.000f, 0.000f, 0.000f } },   // LeftNose
    { 16, 17, { 0.000f, -0.138f, -0.008f }, { -1.112f, 0.000f, -0.001f } },   // LeftUpCheek
    { 34, 35, { 0.054f, -1.364f, -0.062f }, { 0.001f, -0.030f, -1.103f } },   // LeftUpperLip
    { 43, -1, { 0.000f, -0.412f, 0.004f }, { 0.000f, 0.000f, 0.000f } },   // LowerLip
    { 44, -1, { 0.000f, -0.147f, -0.010f }, { 0.000f, 0.000f, 0.000f } },   // UpperLip
    // LowerLipDown (LLwrLipDn_39 / RLwrLipDn_40)
    { 45, 46, { -2.252f, 0.000f, 0.000f }, { 0.000f, 0.000f, 0.000f } },   // LeftInnerMouth
    { 36, 37, { -1.482f, -0.659f, -1.139f }, { -4.694f, -4.163f, 1.363f } },   // LeftLowCheek
    { 38, 39, { -0.087f, 4.483f, -1.343f }, { -3.999f, -0.372f, 16.574f } },   // LeftLowerInnerLip
    { 40, 41, { -1.979f, -11.337f, -1.140f }, { 5.593f, 5.840f, 1.562f } },   // LeftLowerLip
    { 30, 31, { 0.000f, -1.170f, 0.013f }, { 0.000f, 0.000f, 0.000f } },   // LeftMouth
    // LipStretch (LLipStretch_41 / RLipStretch_42)
    { 18, 19, { -4.129f, 0.024f, 2.063f }, { 1.834f, 5.992f, 2.776f } },   // LeftCheek
    { 20, 21, { -0.256f, 0.217f, 0.211f }, { -3.382f, 0.371f, 3.149f } },   // LeftCrease
    { 45, 46, { 2.073f, 0.001f, -2.897f }, { 0.000f, 0.000f, 0.000f } },   // LeftInnerMouth
    { 36, 37, { 0.901f, 1.978f, 0.603f }, { 0.000f, 0.000f, 0.000f } },   // LeftLowCheek
    { 40, 41, { 0.968f, 0.065f, -1.810f }, { -0.465f, 12.625f, -0.811f } },   // LeftLowerLip
    { 22, 23, { 6.934f, 0.048f, 4.011f }, { 0.001f, -24.982f, 0.000f } },   // LeftMiddleCrease
    { 30, 31, { -7.703f, -0.590f, -7.965f }, { 0.000f, 0.000f, 0.000f } },   // LeftMouth
    { 32, 33, { 0.088f, 0.005f, 0.172f }, { 0.000f, 0.000f, 0.000f } },   // LeftUpperInnerLip
    { 34, 35, { -2.022f, -0.028f, -2.372f }, { 3.327f, -8.047f, 2.748f } },   // LeftUpperLip
    // LipCompress (LLipCompress_43 / RLipCompress_44)
    { 18, 19, { 2.162f, -0.477f, 2.867f }, { 0.274f, -4.482f, -0.033f } },   // LeftCheek
    { 20, 21, { 1.337f, -0.064f, 1.047f }, { -3.455f, 5.149f, -6.171f } },   // LeftCrease
    { 45, 46, { -0.389f, 0.000f, -1.325f }, { -0.356f, 1.278f, -14.589f } },   // LeftInnerMouth
    { 38, 39, { -0.178f, 2.751f, -0.069f }, { 0.001f, -0.105f, -9.271f } },   // LeftLowerInnerLip
    { 40, 41, { 6.114f, -0.879f, 3.616f }, { 0.349f, 3.651f, 11.293f } },   // LeftLowerLip
    { 22, 23, { -3.790f, 0.034f, -5.629f }, { 6.509f, -15.247f, -0.882f } },   // LeftMiddleCrease
    { 30, 31, { 9.432f, 1.004f, 6.556f }, { 0.000f, 0.000f, 0.000f } },   // LeftMouth
    { 16, 17, { 0.997f, -0.457f, -0.067f }, { 0.000f, 0.000f, -1.223f } },   // LeftUpCheek
    { 32, 33, { 0.628f, 0.014f, -2.047f }, { 1.518f, 0.164f, 14.274f } },   // LeftUpperInnerLip
    { 34, 35, { 4.768f, 0.390f, 3.232f }, { -2.073f, 11.120f, -9.966f } },   // LeftUpperLip
    { 42, -1, { 0.000f, 0.278f, -0.026f }, { 4.698f, 0.000f, 0.000f } },   // LowerInnerLip
    { 43, -1, { 0.000f, -1.240f, 0.000f }, { -1.106f, 0.000f, 0.000f } },   // LowerLip
    { 47, -1, { 0.000f, -0.502f, 0.000f }, { 0.000f, 0.000f, 0.000f } },   // UpperInnerLip
    { 44, -1, { 0.000f, -0.613f, 0.386f }, { 8.170f, 0.000f, 0.000f } },   // UpperLip
    // UpperLipDownMiddle (UprLipDn_45)
    { 28, -1, { 0.000f, -7.605f, 1.752f }, { 0.000f, 0.000f, 0.000f } },   // LowNose
    { 44, -1, { 0.000f, -11.131f, -6.298f }, { -7.876f, 0.000f, 0.000f } },   // UpperLip
    // LowerLipUpMiddle (LwrLipUp_46)
    { 43, -1, { 0.000f, 10.899f, 0.806f }, { 16.317f, 0.000f, 0.000f } },   // LowerLip
    // UpperLipCurlIn (UprLipCurlIn_51)
    { 20, 21, { 0.000f, 0.000f, 0.000f }, { 1.383f, 0.000f, 0.000f } },   // LeftCrease
    { 34, 35, { 0.000f, -0.008f, -0.734f }, { -18.344f, -2.709f, -5.353f } },   // LeftUpperLip
    { 44, -1, { 0.000f, 0.943f, -3.156f }, { -47.134f, 0.000f, 0.000f } },   // UpperLip
    // LowerLipCurlIn (LwrLipCurlIn_52)
    { 38, 39, { -0.784f, 2.537f, 2.191f }, { -28.418f, -25.730f, 26.978f } },   // LeftLowerInnerLip
    { 40, 41, { -0.180f, -0.393f, 0.302f }, { 41.639f, -7.713f, 11.517f } },   // LeftLowerLip
    { 43, -1, { 0.000f, -1.142f, 0.013f }, { 51.036f, 0.000f, 0.000f } },   // LowerLip
    // UpperLipBack (UprLipBk_55)
    { 34, 35, { 0.000f, -0.201f, -2.456f }, { 0.000f, 0.000f, 0.000f } },   // LeftUpperLip
    { 44, -1, { 0.000f, -0.187f, -5.119f }, { 0.000f, 0.000f, 0.000f } },   // UpperLip
    // LowerLipBack (LwrLipBk_56)
    { 40, 41, { 0.000f, 0.447f, -2.464f }, { 0.000f, 0.000f, 0.000f } },   // LeftLowerLip
    { 43, -1, { 0.000f, 0.461f, -5.126f }, { 0.000f, 0.000f, 0.000f } },   // LowerLip
    // ChinUp (ChinUp_62)
    { 48, -1, { 0.000f, 4.892f, 1.804f }, { 11.451f, 0.000f, 0.000f } },   // Chin
  };
  const Pose ManPoses[PoseCount] = {
    { "LBrowInUp_01 / RBrowInUp_03", true, 0, 1, 0.000f, 0.000f },
    { "LBrowOutUp_02 / RBrowOutUp_04", true, 1, 2, 0.000f, 0.000f },
    { "LBrowMiddleUp_84 / RBrowMiddleUp_85", true, 3, 2, 0.000f, 0.000f },
    { "LBrowInDn_05 / RBrowInDn_07", true, 5, 1, 0.000f, 0.000f },
    { "LBrowIn_74 / RBrowIn_75", true, 6, 3, 0.000f, 0.000f },
    { "LBrowMiddleDn_86 / RBrowMiddleDn_87", true, 9, 2, 0.000f, 0.000f },
    { "LBrowOutDn_06 / RBrowOutDn_08", true, 11, 2, 0.000f, 0.000f },
    { "LUprLidUp_09 / RUprLidUp_10", true, 13, 4, -13.864f, -1.290f },
    { "LLwrLidUp_11 / RLwrLidUp_12", true, 17, 2, 0.000f, 9.839f },
    { "LLidClose_13 / RLidClose_14", true, 19, 5, 39.277f, -0.892f },
    { "LLidSqueeze_96 / RLidSqueeze_97", true, 24, 5, 0.000f, 0.000f },
    { "LCheekUp_23 / RCheekUp_24", true, 29, 5, 0.000f, 0.000f },
    { "LNoseUp_29 / RNoseUp_30", true, 34, 6, 0.000f, 0.000f },
    { "LUprLipUp_33 / RUprLipUp_34", true, 40, 8, 0.000f, 0.000f },
    { "LLipCornerUp_35 / RLipCornerUp_36", true, 48, 13, 0.000f, 0.000f },
    { "LLipCornerDn_37 / RLipCornerDn_38", true, 61, 11, 0.000f, 0.000f },
    { "LLwrLipDn_39 / RLwrLipDn_40", true, 72, 5, 0.000f, 0.000f },
    { "LLipStretch_41 / RLipStretch_42", true, 77, 9, 0.000f, 0.000f },
    { "LLipCompress_43 / RLipCompress_44", true, 86, 14, 0.000f, 0.000f },
    { "UprLipDn_45", false, 100, 2, 0.000f, 0.000f },
    { "LwrLipUp_46", false, 102, 1, 0.000f, 0.000f },
    { "UprLipCurlIn_51", false, 103, 3, 0.000f, 0.000f },
    { "LwrLipCurlIn_52", false, 106, 3, 0.000f, 0.000f },
    { "UprLipBk_55", false, 109, 2, 0.000f, 0.000f },
    { "LwrLipBk_56", false, 111, 2, 0.000f, 0.000f },
    { "ChinUp_62", false, 113, 1, 0.000f, 0.000f },
  };

  const Library Libraries[] = {
    { "Faith", "Faith", 63.971f, 74.144f, 34.121f, FaithPoses, FaithEntries, (int)(sizeof(FaithEntries) / sizeof(FaithEntries[0])) },
    { "Gabriel", "man", 61.579f, 78.185f, 30.851f, ManPoses, ManEntries, (int)(sizeof(ManEntries) / sizeof(ManEntries[0])) },
  };
  const int LibraryCount = (int)(sizeof(Libraries) / sizeof(Libraries[0]));
  const int LibraryFaith = 0, LibraryMan = 1;
}
