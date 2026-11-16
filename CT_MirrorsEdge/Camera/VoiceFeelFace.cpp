#include "VoiceFeelFace.h"
#include <cmath>
#include <cstring>

namespace voice_face
{
#ifndef VOICE_FEEL_ALLOW_SMALL_LAYER
  // The layer must hold the mouth's 9 bones, the life's 16 and the feeling's 27 more: with a smaller MaxDriven the
  // feeling's last bones would be left out (AddFeelOffsets says how many), so a build with too small a layer stops
  // here. (The test program builds once with VOICE_FEEL_ALLOW_SMALL_LAYER against the project's 32, to check that
  // a full layer only drops the feeling's own extra bones.)
  static_assert(MaxDriven >= 9 + 16 + FeelNewBones + FeelEyeBones,
    "VoiceFace.h: MaxDriven must be 54 or more (mouth 9, life 16, feeling 27, eyes 2)");   // [MERGE 1] (the eyes)
#endif

  const char* const FeelAuNames[AuCount] = {
    "AU1 inner brow raiser", "AU2 outer brow raiser", "AU4 brow lowerer", "AU5 upper lid raiser", "AU6 cheek raiser",
    "AU7 lid tightener", "AU9 nose wrinkler", "AU10 upper lip raiser", "AU12 lip corner puller", "AU14 dimpler",
    "AU15 lip corner depressor", "AU16 lower lip depressor", "AU17 chin raiser", "AU20 lip stretcher",
    "AU23 lip tightener", "AU24 lip pressor", "AU41 lid droop"
  };

  // [d] Each action unit as the game's poses at weight 1 (designed from what each pose and each unit does; the
  // sizes are then the game's own)
  using namespace voice_feel_tables;
  const FeelAuMix FeelAuMixes[AuCount] = {
    { 2, { { BrowInUp, 1.f }, { BrowMiddleUp, 0.4f } } },                                   // AU1
    { 2, { { BrowOutUp, 1.f }, { BrowMiddleUp, 0.4f } } },                                  // AU2
    { 3, { { BrowInDown, 1.f }, { BrowIn, 1.f }, { BrowMiddleDown, 0.5f } } },              // AU4
    { 1, { { UpperLidUp, 1.f } } },                                                         // AU5
    { 2, { { CheekUp, 1.f }, { LowerLidUp, 0.4f } } },                                      // AU6
    { 2, { { LowerLidUp, 1.f }, { LidSqueeze, 0.3f } } },                                   // AU7
    { 3, { { NoseUp, 1.f }, { BrowInDown, 0.3f }, { UpperLipUp, 0.2f } } },                 // AU9
    { 1, { { UpperLipUp, 1.f } } },                                                         // AU10
    { 1, { { LipCornerUp, 1.f } } },                                                        // AU12
    { 2, { { LipStretch, 0.4f }, { LipCompress, 0.25f } } },                                // AU14
    { 1, { { LipCornerDown, 1.f } } },                                                      // AU15
    { 1, { { LowerLipDown, 1.f } } },                                                       // AU16
    { 2, { { ChinUp, 1.f }, { LowerLipUpMiddle, 0.3f } } },                                 // AU17
    { 1, { { LipStretch, 1.f } } },                                                         // AU20
    { 3, { { LipCompress, 0.3f }, { UpperLipCurlIn, 0.25f }, { LowerLipCurlIn, 0.25f } } }, // AU23
    { 4, { { UpperLipDownMiddle, 0.3f }, { LowerLipUpMiddle, 0.3f }, { UpperLipBack, 0.6f }, { LowerLipBack, 0.6f } } },   // AU24
    { 1, { { LidClose, 0.4f } } },                                                          // AU41
  };

  FeelShape::FeelShape()
  {
    for (int k = 0; k < AuCount; ++k)
      Left[k] = Right[k] = 0.f;
    Nod = Tilt = 0.f;
    RaiseKeep = 1.f;
    Jaw = 0.f;
  }

  namespace
  {
    const float FeelPi = 3.14159265358979f;

    void FeelQMul(float const* a, float const* b, float* out)
    {
      float x = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1];
      float y = a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0];
      float z = a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3];
      float w = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
      out[0] = x; out[1] = y; out[2] = z; out[3] = w;
    }

    void FeelRot(float const* q, float const* v, float* out)
    {
      float tx = 2.f * (q[1] * v[2] - q[2] * v[1]);
      float ty = 2.f * (q[2] * v[0] - q[0] * v[2]);
      float tz = 2.f * (q[0] * v[1] - q[1] * v[0]);
      float x = v[0] + q[3] * tx + (q[1] * tz - q[2] * ty);
      float y = v[1] + q[3] * ty + (q[2] * tx - q[0] * tz);
      float z = v[2] + q[3] * tz + (q[0] * ty - q[1] * tx);
      out[0] = x; out[1] = y; out[2] = z;
    }

    void FeelAbout(float const* axis, float angle, float* out)
    {
      float s = sinf(angle * 0.5f);
      out[0] = axis[0] * s; out[1] = axis[1] * s; out[2] = axis[2] * s; out[3] = cosf(angle * 0.5f);
    }

    float FeelDot(float const* a, float const* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

    void FeelCross(float const* a, float const* b, float* out)
    {
      float x = a[1] * b[2] - a[2] * b[1];
      float y = a[2] * b[0] - a[0] * b[2];
      float z = a[0] * b[1] - a[1] * b[0];
      out[0] = x; out[1] = y; out[2] = z;
    }

    bool FeelQNorm(float* q)
    {
      float length = sqrtf(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
      if (!std::isfinite(length) || !(length > 1e-6f))
        return false;
      for (int k = 0; k < 4; ++k)
        q[k] /= length;
      return true;
    }

    // 0 for what isn't a number or is under 0, cut at most
    float FeelClamp(float v, float most) { return !(v > 0.f) ? 0.f : (v > most ? most : v); }
    float FeelLerp(float a, float b, float t) { return t >= 1.f ? b : (!(t > 0.f) ? a : a + (b - a) * t); }

    int FeelFind(int bones, char const* const* names, char const* name)
    {
      for (int i = 0; i < bones; ++i)
        if (names[i] && !strcmp(names[i], name))
          return i;
      return -1;
    }

    // The bone hangs on the Face bone, at most a few bones down (a lip's inner lip, a corner's crease)
    bool FeelUnder(int bone, int face, int bones, short const* parents)
    {
      for (int step = 0, b = bone; step < 4 && b >= 0 && b < bones; ++step)
      {
        b = parents[b];
        if (b == face)
          return true;
      }
      return false;
    }

    // Each game pose's weight on each side (0 left, 1 right; a pose of the middle: both the same), from the units
    void FeelPoseWeights(FeelShape const& shape, float weights[PoseCount][2])
    {
      for (int p = 0; p < PoseCount; ++p)
        weights[p][0] = weights[p][1] = 0.f;
      for (int a = 0; a < AuCount; ++a)
      {
        float l = FeelClamp(shape.Left[a], 2.f), r = FeelClamp(shape.Right[a], 2.f);
        if (l == 0.f && r == 0.f)
          continue;
        for (int m = 0; m < FeelAuMixes[a].Count; ++m)
        {
          FeelMix const& mix = FeelAuMixes[a].Mix[m];
          weights[mix.Pose][0] += l * mix.Weight;
          weights[mix.Pose][1] += r * mix.Weight;
        }
      }
      Library const& lib = Libraries[0];
      for (int p = 0; p < PoseCount; ++p)
      {
        if (!lib.Poses[p].Sided)
          weights[p][0] = weights[p][1] = 0.5f * (weights[p][0] + weights[p][1]);
        // never a game pose past its own extreme
        for (int s = 0; s < 2; ++s)
          weights[p][s] = FeelClamp(weights[p][s], 1.f);
      }
    }

    // Every bone's sum: mm and degrees along right, up, forward
    struct FeelSum
    {
      float Move[BoneCount][3];
      float Turn[BoneCount][3];
      bool Any[BoneCount];
      float UpClose[2], LowClose[2];   // the lid poses' share of a whole blink, per eye (for lid bones at the middle)
    };

    void FeelAccumulate(int library, float const weights[PoseCount][2], FeelSum& sum)
    {
      memset(&sum, 0, sizeof(sum));
      Library const& lib = Libraries[library];
      for (int p = 0; p < PoseCount; ++p)
      {
        Pose const& pose = lib.Poses[p];
        // A two-sided pose: each side at its own weight (side 1: the twins, and the middle bones mirrored). A pose of
        // the middle: once, at the two sides' mean weight (FeelPoseWeights), its left bones' twins mirrored.
        int sides = pose.Sided ? 2 : 1;
        for (int s = 0; s < sides; ++s)
        {
          float w = weights[p][s];
          if (!(w > 0.f))
            continue;
          if (lib.BlinkAngle > 1.f)
            for (int eye = 0; eye < 2; ++eye)
              if (!pose.Sided || eye == s)
              {
                sum.UpClose[eye] += w * pose.UpLidClose / lib.BlinkAngle;
                sum.LowClose[eye] += w * pose.LowLidClose / lib.BlinkAngle;
              }
          for (int e = pose.First; e < pose.First + pose.Count && e < lib.EntryCount; ++e)
          {
            Entry const& entry = lib.Entries[e];
            float m[3] = { entry.Move[0], entry.Move[1], entry.Move[2] };
            float t[3] = { entry.Turn[0], entry.Turn[1], entry.Turn[2] };
            float mm[3] = { -m[0], m[1], m[2] }, mt[3] = { t[0], -t[1], -t[2] };   // the mirror image
            int targets[2] = { -1, -1 };
            float const* moves[2] = { m, mm };
            float const* turns[2] = { t, mt };
            if (pose.Sided)
            {
              targets[0] = s == 0 ? entry.Bone : (entry.Twin >= 0 ? entry.Twin : entry.Bone);
              if (s == 1)
              {
                moves[0] = mm;
                turns[0] = mt;
              }
            }
            else
            {
              targets[0] = entry.Bone;
              targets[1] = entry.Twin;   // (-1 for a middle bone: made symmetric already)
            }
            for (int k = 0; k < 2; ++k)
            {
              int bone = targets[k];
              if (bone < 0 || bone >= BoneCount)
                continue;
              for (int c = 0; c < 3; ++c)
              {
                sum.Move[bone][c] += w * moves[k][c];
                sum.Turn[bone][c] += w * turns[k][c];
              }
              sum.Any[bone] = true;
            }
          }
        }
      }
    }

    // A rotation vector (degrees along right, up, forward) as a turn in the Face bone's frame
    void FeelTurnOf(LifeRig const& life, float const* rv, float k, float* q)
    {
      float axis[3];
      for (int c = 0; c < 3; ++c)
        axis[c] = (life.Right[c] * rv[0] + life.Up[c] * rv[1] + life.Forward[c] * rv[2]) * k;
      float angle = sqrtf(FeelDot(axis, axis)) * FeelPi / 180.f;
      if (!std::isfinite(angle) || !(angle > 1e-7f))
      {
        q[0] = q[1] = q[2] = 0.f;
        q[3] = 1.f;
        return;
      }
      float length = sqrtf(FeelDot(axis, axis));
      for (int c = 0; c < 3; ++c)
        axis[c] /= length;
      FeelAbout(axis, angle, q);
    }

    // A move (mm along right, up, forward) in the Face bone's frame, metres
    void FeelMoveOf(FeelRig const& rig, float const* mm, float k, float* out)
    {
      float s = 0.001f * rig.Scale * k;
      for (int c = 0; c < 3; ++c)
        out[c] = (rig.Life.Right[c] * mm[0] + rig.Life.Up[c] * mm[1] + rig.Life.Forward[c] * mm[2]) * s;
    }

    // A lid bone turned round the blink's line by angle (in the closing sense: sense +1 the upper lid, -1 the lower)
    void FeelRoundLine(LifeRig const& life, int eye, float const* place, float closing, float sense, float* q, float* move)
    {
      FeelAbout(life.Right, life.NodSign * sense * closing, q);
      float from[3], turned[3];
      for (int c = 0; c < 3; ++c)
        from[c] = place[c] - life.LidLine[eye][c];
      FeelRot(q, from, turned);
      for (int c = 0; c < 3; ++c)
        move[c] = turned[c] - from[c];
    }

    // The two lid bones' height angle seen from the blink's line (the life's measure); false: no way down there
    bool FeelGap(LifeRig const& life, int eye, float const* upper, float const* lower, float& gap)
    {
      float u[3], l[3];
      for (int c = 0; c < 3; ++c)
      {
        u[c] = upper[c] - life.LidLine[eye][c];
        l[c] = lower[c] - life.LidLine[eye][c];
      }
      float uf = FeelDot(u, life.Forward), lf = FeelDot(l, life.Forward);
      if (!(uf > 0.001f) || !(lf > 0.001f))
        return false;
      gap = atan2f(FeelDot(u, life.Up), uf) - atan2f(FeelDot(l, life.Up), lf);
      return std::isfinite(gap);
    }

    // One eye's lid offsets from the sums, the lid parts scaled by k: the upper and the lower lid bone's turn and
    // move, mixed between the two arrangements, and the feeling's closing share of that eye
    struct FeelLids
    {
      float UpQ[4], UpMove[3], LowQ[4], LowMove[3];
      bool UpAny, LowAny;
      float Closing;   // 0 open as posed .. 1 shut (may be under 0: opened)
      float Ratio;     // the share of the eye's way left to a blink (1 - Closing, not under 0)
    };

    void FeelLidsOf(FeelRig const& rig, FeelSum const& sum, int eye, float k, FeelLids& lids)
    {
      LifeRig const& life = rig.Life;
      int up = rig.LidName[eye][0], low = rig.LidName[eye][1];
      float edge = life.LidEdge[eye];
      float const* upPlace = life.LidPlace[eye];
      float const* lowPlace = rig.LowLidPlace[eye];
      // On the edge: the game's own offsets
      float upEdgeQ[4] = { 0.f, 0.f, 0.f, 1.f }, upEdgeMove[3] = { 0.f, 0.f, 0.f };
      float lowEdgeQ[4] = { 0.f, 0.f, 0.f, 1.f }, lowEdgeMove[3] = { 0.f, 0.f, 0.f };
      lids.UpAny = up >= 0 && sum.Any[up];
      lids.LowAny = low >= 0 && sum.Any[low];
      if (lids.UpAny)
      {
        FeelTurnOf(life, sum.Turn[up], k, upEdgeQ);
        FeelMoveOf(rig, sum.Move[up], k, upEdgeMove);
      }
      if (lids.LowAny)
      {
        FeelTurnOf(life, sum.Turn[low], k, lowEdgeQ);
        FeelMoveOf(rig, sum.Move[low], k, lowEdgeMove);
      }
      // At the middle: round the line by the poses' share of a whole blink there
      float upClose = sum.UpClose[eye] * k * LidPivotAngle, lowClose = sum.LowClose[eye] * k * LidPivotAngle;
      float upMidQ[4], upMidMove[3], lowMidQ[4], lowMidMove[3];
      FeelRoundLine(life, eye, upPlace, upClose, 1.f, upMidQ, upMidMove);
      FeelRoundLine(life, eye, lowPlace, lowClose, -1.f, lowMidQ, lowMidMove);
      lids.UpAny = lids.UpAny || upClose != 0.f;
      lids.LowAny = lids.LowAny || lowClose != 0.f;
      // Mixed by how far on the edge the bones sit (nlerp of the turns: they are near each other)
      for (int c = 0; c < 3; ++c)
      {
        lids.UpMove[c] = FeelLerp(upMidMove[c], upEdgeMove[c], edge);
        lids.LowMove[c] = FeelLerp(lowMidMove[c], lowEdgeMove[c], edge);
      }
      float upDot = FeelDot(upMidQ, upEdgeQ) + upMidQ[3] * upEdgeQ[3];
      float lowDot = FeelDot(lowMidQ, lowEdgeQ) + lowMidQ[3] * lowEdgeQ[3];
      for (int c = 0; c < 4; ++c)
      {
        lids.UpQ[c] = FeelLerp(upMidQ[c], upDot < 0.f ? -upEdgeQ[c] : upEdgeQ[c], edge);
        lids.LowQ[c] = FeelLerp(lowMidQ[c], lowDot < 0.f ? -lowEdgeQ[c] : lowEdgeQ[c], edge);
      }
      if (!FeelQNorm(lids.UpQ))
      {
        lids.UpQ[0] = lids.UpQ[1] = lids.UpQ[2] = 0.f; lids.UpQ[3] = 1.f;
      }
      if (!FeelQNorm(lids.LowQ))
      {
        lids.LowQ[0] = lids.LowQ[1] = lids.LowQ[2] = 0.f; lids.LowQ[3] = 1.f;
      }
      // The closing share: at the middle the turns against the whole blink there (LidPivotAngle is LidSweep x the
      // lids' way); on the edge the lid bones' way left between them, seen from the line
      float middle = LidSweep * (upClose + lowClose) / LidPivotAngle;
      float closing = middle;
      if (edge > 0.f)
      {
        float gap0 = 0.f, gap1 = 0.f, upNow[3], lowNow[3];
        for (int c = 0; c < 3; ++c)
        {
          upNow[c] = upPlace[c] + upEdgeMove[c];
          lowNow[c] = lowPlace[c] + lowEdgeMove[c];
        }
        float edgeClosing = middle;
        if (FeelGap(life, eye, upPlace, lowPlace, gap0) && gap0 > 0.01f && FeelGap(life, eye, upNow, lowNow, gap1))
          edgeClosing = 1.f - gap1 / gap0;
        closing = FeelLerp(middle, edgeClosing, edge);
      }
      lids.Closing = std::isfinite(closing) ? closing : 0.f;
      lids.Ratio = 1.f - lids.Closing;
      if (!(lids.Ratio > 0.f))
        lids.Ratio = 0.f;
    }

    // The scale of an eye's lid parts that keeps the feeling's closing within FeelLidMostClosed
    float FeelLidScale(FeelRig const& rig, FeelSum const& sum, int eye)
    {
      FeelLids lids;
      FeelLidsOf(rig, sum, eye, 1.f, lids);
      if (!(lids.Closing > FeelLidMostClosed))
        return 1.f;
      float lo = 0.f, hi = 1.f;
      for (int step = 0; step < 24; ++step)
      {
        float mid = 0.5f * (lo + hi);
        FeelLidsOf(rig, sum, eye, mid, lids);
        if (lids.Closing > FeelLidMostClosed)
          hi = mid;
        else
          lo = mid;
      }
      return lo;
    }

    // An eye the pose under the layer has shut already (the life has no way left to shut it): the feeling leaves its
    // lids alone (a squint would push the lower lid through the upper one, a wide eye would open what the user shut)
    // [SKEPTIC FIX 1] The share of the feeling's lid moves an eye takes, by how far the pose has its upper lid turned
    // shut where its lid bones sit at the middle: 1 up to FeelPoseOpenTurn, 0 at a whole middle blink less
    // FeelShutAngle; mixed with 1 by how far on the edge the bones sit (on the edge the life's own LidAngle sees it)
    float FeelPoseLidShare(FeelRig const& rig, int eye)
    {
      float edge = rig.Life.LidEdge[eye];
      if (edge >= 1.f)
        return 1.f;
      float shutAt = LidPivotAngle - FeelShutAngle;
      float closed = rig.PoseClosed[eye];
      float middle = !(closed > FeelPoseOpenTurn) ? 1.f :
        (closed >= shutAt ? 0.f : 1.f - (closed - FeelPoseOpenTurn) / (shutAt - FeelPoseOpenTurn));
      return FeelLerp(middle, 1.f, edge);
    }

    bool FeelEyeShut(FeelRig const& rig, LifeRig const* lifeUsed, int eye)
    {
      LifeRig const& life = lifeUsed && lifeUsed->Eyes ? *lifeUsed : rig.Life;
      return life.Eyes && (!(life.LidAngle[eye] > FeelShutAngle) || !(FeelPoseLidShare(rig, eye) > 0.f));
    }

    // [FIX CHECK] The share of the feeling's lid moves an eye nearly shut takes: none at FeelShutAngle (where it counts
    // as shut), all from FeelShutAngle + FeelShutBand of way left on, in a line between: the feeling's lids hand over
    // to the eye left alone without a pop (before: a wide-eyed feeling's lid jumped 2 - 4 degrees there as a lid was
    // closed by hand, 4 - 9 with the eyes looking up: the fix check)
    float FeelNearShutShare(FeelRig const& rig, LifeRig const* lifeUsed, int eye)
    {
      LifeRig const& life = lifeUsed && lifeUsed->Eyes ? *lifeUsed : rig.Life;
      if (!life.Eyes)
        return 1.f;
      float share = (life.LidAngle[eye] - FeelShutAngle) / FeelShutBand;
      return !(share > 0.f) ? 0.f : (share > 1.f ? 1.f : share);
    }

    int FeelEntry(Offsets const& out, int bone)
    {
      for (int n = 0; n < out.Count && n < MaxDriven; ++n)
        if (out.Bone[n] == bone)
          return n;
      return -1;
    }

    bool FeelFinite(float const* q, float const* m)
    {
      for (int c = 0; c < 4; ++c)
        if (!std::isfinite(q[c]))
          return false;
      for (int c = 0; c < 3; ++c)
        if (!std::isfinite(m[c]))
          return false;
      return true;
    }

    // Laid on a bone already in the layer (the feeling's turn on top of it, the moves added), or appended
    bool FeelLay(Offsets& out, int bone, float const* q, float const* move, int& dropped)
    {
      if (bone < 0 || !FeelFinite(q, move))
        return false;
      int n = FeelEntry(out, bone);
      if (n >= 0)
      {
        float turned[4];
        FeelQMul(q, out.Turn[n], turned);
        if (!FeelQNorm(turned))
          return false;
        memcpy(out.Turn[n], turned, sizeof(turned));
        for (int c = 0; c < 3; ++c)
          out.Move[n][c] += move[c];
        return true;
      }
      if (out.Count >= MaxDriven)
      {
        ++dropped;
        return false;
      }
      n = out.Count++;
      out.Bone[n] = bone;
      memcpy(out.Turn[n], q, 4 * sizeof(float));
      memcpy(out.Move[n], move, 3 * sizeof(float));
      return true;
    }
  }

  bool FindFeelRig(int bones, char const* const* names, short const* parents, float const* locals, int library, FeelRig& rig)
  {
    rig = FeelRig();
    for (int b = 0; b < BoneCount; ++b)
      rig.Bone[b] = -1;
    if (bones <= 0 || !names || !parents || !locals)
      return false;
    if (!FindLifeRig(bones, names, parents, locals, rig.Life) || rig.Life.Face < 0)
      return false;
    LifeRig& life = rig.Life;
    // Left and right must be known: the life turns Forward round when the nose lies behind the eyes (the eye bones
    // the wrong way round), and then up x right no longer points forward
    float upRight[3];
    FeelCross(life.Up, life.Right, upRight);
    if (!(FeelDot(upRight, life.Forward) > 0.5f))
      return false;
    rig.Library = library >= 0 && library < LibraryCount ? library : LibraryFaith;
    for (int b = 0; b < BoneCount; ++b)
      for (int s = 0; s < 2; ++s)
      {
        if (!strcmp(BoneNames[b], s == 0 ? "LeftUpEyelid" : "RightUpEyelid"))
          rig.LidName[s][0] = b;
        if (!strcmp(BoneNames[b], s == 0 ? "LeftLowEyelid" : "RightLowEyelid"))
          rig.LidName[s][1] = b;
      }
    int face = life.Face;
    auto place = [&](int bone) { return locals + (size_t)bone * FloatsPerBone; };
    for (int b = 0; b < BoneCount; ++b)
    {
      int bone = FeelFind(bones, names, BoneNames[b]);
      if (bone < 0 || !FeelUnder(bone, face, bones, parents))
        continue;
      bool finite = true;
      for (int k = 0; k < FloatsPerBone; ++k)
        finite = finite && std::isfinite(place(bone)[k]);
      if (finite)
        rig.Bone[b] = bone;
    }
    // The face's size: the eye bones' distance and the eyes-to-mouth distance against the library's rest face
    int eye[2] = { FeelFind(bones, names, "LeftEye"), FeelFind(bones, names, "RightEye") };
    int corner[2] = { FeelFind(bones, names, "LeftMouth"), FeelFind(bones, names, "RightMouth") };
    Library const& lib = Libraries[rig.Library];
    float scale = 1.f;
    if (eye[0] >= 0 && eye[1] >= 0 && parents[eye[0]] == face && parents[eye[1]] == face)
    {
      float apart[3], mid[3];
      for (int c = 0; c < 3; ++c)
      {
        apart[c] = place(eye[1])[c] - place(eye[0])[c];
        mid[c] = 0.5f * (place(eye[1])[c] + place(eye[0])[c]);
      }
      float ratio = 1000.f * sqrtf(FeelDot(apart, apart)) / lib.EyesApart;
      scale = ratio;
      if (corner[0] >= 0 && corner[1] >= 0 && parents[corner[0]] == face && parents[corner[1]] == face)
      {
        float down[3];
        for (int c = 0; c < 3; ++c)
          down[c] = 0.5f * (place(corner[0])[c] + place(corner[1])[c]) - mid[c];
        scale = 0.5f * (ratio + 1000.f * sqrtf(FeelDot(down, down)) / lib.EyesToMouth);
      }
    }
    rig.Scale = !std::isfinite(scale) ? 1.f : (scale < FeelScaleLeast ? FeelScaleLeast : (scale > FeelScaleMost ? FeelScaleMost : scale));
    // The lids: as the life has them (both on the Face bone)
    for (int s = 0; s < 2; ++s)
    {
      rig.Lids[s] = life.Eyes && life.UpLid[s] >= 0 && life.LowLid[s] >= 0;
      if (rig.Lids[s])
      {
        memcpy(rig.LowLidPlace[s], place(life.LowLid[s]), 3 * sizeof(float));
        // [SKEPTIC FIX 1] the pose's own turn of the upper lid about the head's right (closing +)
        float const* q = place(life.UpLid[s]) + 4;
        float sign = q[3] < 0.f ? -1.f : 1.f;
        float closed = life.NodSign * 2.f * atan2f(sign * FeelDot(q, life.Right), sign * q[3]);
        rig.PoseClosed[s] = std::isfinite(closed) ? closed : 0.f;
      }
    }
    rig.Ok = true;
    return true;
  }

  float FeelLidClosing(FeelRig const& rig, FeelShape const& shape, int eye)
  {
    if (!rig.Ok || eye < 0 || eye > 1 || !rig.Lids[eye] || FeelEyeShut(rig, nullptr, eye))
      return 0.f;
    float weights[PoseCount][2];
    FeelPoseWeights(shape, weights);
    FeelSum sum;
    FeelAccumulate(rig.Library, weights, sum);
    FeelLids lids;
    FeelLidsOf(rig, sum, eye, FeelLidScale(rig, sum, eye) * FeelPoseLidShare(rig, eye) * FeelNearShutShare(rig, nullptr, eye),
      lids);   // [SKEPTIC FIX 1] [FIX CHECK]
    return lids.Closing;
  }

  float FeelNearShutShareOf(FeelRig const& rig, int eye)
  {
    return rig.Ok && eye >= 0 && eye <= 1 && rig.Lids[eye] ? FeelNearShutShare(rig, nullptr, eye) : 1.f;
  }

  float FeelPoseLidShareOf(FeelRig const& rig, int eye)
  {
    return rig.Ok && eye >= 0 && eye <= 1 && rig.Lids[eye] ? FeelPoseLidShare(rig, eye) : 1.f;
  }

  int AddFeelOffsets(FeelRig const& rig, FeelShape const& shape, LifeRig const* lifeUsed, float const* headLocalQ,
    float const* faceLocalQ, float const* neck1LocalQ, Offsets& out, int parts, float const* blinkGiven,
    float const* followGiven)
  {
    int dropped = 0;
    if (!rig.Ok)
      return 0;
    if (out.Count < 0)
      out.Count = 0;
    if (out.Count > MaxDriven)
      out.Count = MaxDriven;
    float weights[PoseCount][2];
    FeelPoseWeights(shape, weights);
    FeelSum sum;
    FeelAccumulate(rig.Library, weights, sum);
    LifeRig const& life = rig.Life;
    const float identity[4] = { 0.f, 0.f, 0.f, 1.f };

    // The life's brow raises: only RaiseKeep of them
    float keep = !(shape.RaiseKeep >= 0.f) ? 0.f : (shape.RaiseKeep > 1.f ? 1.f : shape.RaiseKeep);
    bool face = (parts & FeelPartFace) != 0, lidsToo = (parts & FeelPartLids) != 0;   // [MERGE 1]
    if (face && keep < 1.f && lifeUsed && lifeUsed->Brows)
    {
      int raised[10];
      for (int k = 0; k < 6; ++k)
        raised[k] = lifeUsed->Brow[k];
      for (int k = 0; k < 4; ++k)
        raised[6 + k] = lifeUsed->Fold[k];
      for (int k = 0; k < 10; ++k)
      {
        int n = raised[k] >= 0 ? FeelEntry(out, raised[k]) : -1;
        if (n >= 0)
          for (int c = 0; c < 3; ++c)
            out.Move[n][c] *= keep;
      }
    }

    // Every bone but the lids
    int lidBone[4] = { life.UpLid[0], life.UpLid[1], life.LowLid[0], life.LowLid[1] };
    for (int b = 0; face && b < BoneCount; ++b)
    {
      int bone = rig.Bone[b];
      if (bone < 0 || !sum.Any[b])
        continue;
      bool lid = false;
      for (int k = 0; k < 4; ++k)
        lid = lid || (bone == lidBone[k] && rig.Lids[k % 2]);
      if (lid)
        continue;
      float q[4], move[3];
      FeelTurnOf(life, sum.Turn[b], 1.f, q);
      FeelMoveOf(rig, sum.Move[b], 1.f, move);
      if (q[3] == 1.f && move[0] == 0.f && move[1] == 0.f && move[2] == 0.f)
        continue;
      FeelLay(out, bone, q, move, dropped);
    }

    // The lids, each eye: the feeling's (within FeelLidMostClosed), and the life's blink laid again from there
    for (int s = 0; lidsToo && s < 2; ++s)
    {
      if (!rig.Lids[s] || FeelEyeShut(rig, lifeUsed, s))
        continue;
      FeelLids lids;
      FeelLidsOf(rig, sum, s, FeelLidScale(rig, sum, s) * FeelPoseLidShare(rig, s) * FeelNearShutShare(rig, lifeUsed, s),
        lids);   // [SKEPTIC FIX 1] [FIX CHECK]
      if (!lids.UpAny && !lids.LowAny)
        continue;
      int up = life.UpLid[s];
      int n = FeelEntry(out, up);
      bool blinkLaid = false;
      if (n >= 0 && lifeUsed && lifeUsed->Eyes && lifeUsed->UpLid[s] == up && lifeUsed->LidAngle[s] > 1e-4f)
      {
        float now;
        if (blinkGiven)
        {
          // [MERGE 1] The life's blink and the lid's follow as given: Follow + Blink x (the way left - Follow), the
          // follow cut against the way the feeling has left (with none: Blink x the way left, as before)
          float blink = FeelClamp(blinkGiven[s], 1.f);
          float wayLeft = lifeUsed->LidAngle[s] * lids.Ratio;
          // [FIX CHECK] (cut against the smaller of the way the feeling leaves and the life's own: the follow hands
          // over to the life's own lid entry where the eye counts as shut, without a jump)
          float wayCut = wayLeft < lifeUsed->LidAngle[s] ? wayLeft : lifeUsed->LidAngle[s];
          float follow = LidFollowCut(followGiven ? followGiven[s] : 0.f, wayCut);
          now = follow == 0.f ? blink * lifeUsed->LidAngle[s] * lids.Ratio : follow + blink * (wayLeft - follow);
          now = !(now == now) ? 0.f : (now < -LidFollowUpMost ? -LidFollowUpMost : (now > LidAngleMost ? LidAngleMost : now));
        }
        else
        {
          // The life's turn is round its Right only: its angle, and the share of the blink it is
          float* q = out.Turn[n];
          float about = FeelDot(q, lifeUsed->Right);
          float angle = lifeUsed->NodSign * 2.f * atan2f(about, q[3]);
          float blink = std::isfinite(angle) ? FeelClamp(angle / lifeUsed->LidAngle[s], 1.f) : 0.f;
          now = blink * lifeUsed->LidAngle[s] * lids.Ratio;
          now = FeelClamp(now, LidAngleMost);
        }
        float blinkQ[4], from[3], turned[3], total[4], move[3];
        FeelAbout(lifeUsed->Right, lifeUsed->NodSign * now, blinkQ);
        for (int c = 0; c < 3; ++c)
          from[c] = lifeUsed->LidPlace[s][c] + lids.UpMove[c] - lifeUsed->LidLine[s][c];
        FeelRot(blinkQ, from, turned);
        for (int c = 0; c < 3; ++c)
          move[c] = lids.UpMove[c] + turned[c] - from[c];
        FeelQMul(blinkQ, lids.UpQ, total);
        if (FeelQNorm(total) && FeelFinite(total, move))
        {
          memcpy(out.Turn[n], total, sizeof(total));
          memcpy(out.Move[n], move, sizeof(move));
          blinkLaid = true;
        }
      }
      if (!blinkLaid && lids.UpAny)
        FeelLay(out, up, lids.UpQ, lids.UpMove, dropped);
      if (lids.LowAny)
        FeelLay(out, life.LowLid[s], lids.LowQ, lids.LowMove, dropped);
    }
    (void)identity;

    // The head: the feeling's nod and tilt, made as the life makes its own, laid on top of the life's
    float nod = !(shape.Nod == shape.Nod) ? 0.f : (shape.Nod > FeelHeadMost ? FeelHeadMost : (shape.Nod < -FeelHeadMost ? -FeelHeadMost : shape.Nod));
    float tilt = !(shape.Tilt == shape.Tilt) ? 0.f : (shape.Tilt > FeelHeadMost ? FeelHeadMost : (shape.Tilt < -FeelHeadMost ? -FeelHeadMost : shape.Tilt));
    if (face && (nod != 0.f || tilt != 0.f) && life.Head && headLocalQ && faceLocalQ)
    {
      LifeShape posture;
      posture.Nod = nod;
      posture.Tilt = tilt;
      LifeAmounts amounts;
      amounts.Blinks = 0.f;
      amounts.Brows = 0.f;
      amounts.Head = 1.f;
      Offsets head;
      head.Count = 0;
      AddLifeOffsets(life, posture, amounts, headLocalQ, faceLocalQ, head, neck1LocalQ);
      for (int k = 0; k < head.Count; ++k)
        FeelLay(out, head.Bone[k], head.Turn[k], head.Move[k], dropped);
    }
    return dropped;
  }
}
