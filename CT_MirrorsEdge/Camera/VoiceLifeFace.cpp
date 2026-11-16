#include "VoiceLifeFace.h"
#include <cmath>
#include <cstring>
#include <initializer_list>

namespace voice_face
{
  // [CHECK 1] The layer must hold the mouth's 9 bones and the life's 16 (4 lids, 6 brows, 4 fold bones, Head,
  // Neck1). With a smaller MaxDriven the last bones would be left out without a word (with 12: the right lower
  // lid, every brow and the head), so a build with too small a layer stops here.
  static_assert(MaxDriven >= 9 + 16, "VoiceFace.h: MaxDriven must be 25 or more (the mouth's 9 bones and the life's 16)");

  namespace
  {
    // v turned by q
    void LifeRot(float const* q, float const* v, float* out)
    {
      float tx = 2.f * (q[1] * v[2] - q[2] * v[1]);
      float ty = 2.f * (q[2] * v[0] - q[0] * v[2]);
      float tz = 2.f * (q[0] * v[1] - q[1] * v[0]);
      float x = v[0] + q[3] * tx + (q[1] * tz - q[2] * ty);
      float y = v[1] + q[3] * ty + (q[2] * tx - q[0] * tz);
      float z = v[2] + q[3] * tz + (q[0] * ty - q[1] * tx);
      out[0] = x; out[1] = y; out[2] = z;
    }

    void LifeAbout(float const* axis, float angle, float* out)
    {
      float s = sinf(angle * 0.5f);
      out[0] = axis[0] * s; out[1] = axis[1] * s; out[2] = axis[2] * s; out[3] = cosf(angle * 0.5f);
    }

    float LifeDot(float const* a, float const* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

    void LifeCross(float const* a, float const* b, float* out)
    {
      float x = a[1] * b[2] - a[2] * b[1];
      float y = a[2] * b[0] - a[0] * b[2];
      float z = a[0] * b[1] - a[1] * b[0];
      out[0] = x; out[1] = y; out[2] = z;
    }

    bool LifeNormalise(float* v)
    {
      float length = sqrtf(LifeDot(v, v));
      if (!(length > 1e-6f) || !std::isfinite(length))
        return false;
      v[0] /= length; v[1] /= length; v[2] /= length;
      return true;
    }

    // [CHECK 3] A rotation handed in, made unit length (LifeRot turns a vector only with a unit quaternion: one
    // of another length bends the axis). False: it is no rotation (not numbers, or far from unit length).
    bool LifeTurnOf(float const* q, float* out)
    {
      if (!q)
        return false;
      float length = sqrtf(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
      if (!std::isfinite(length) || !(length > 0.5f) || !(length < 2.f))
        return false;
      for (int k = 0; k < 4; ++k)
        out[k] = q[k] / length;
      return true;
    }

    float LifeUnit(float v) { return !(v > 0.f) ? 0.f : (v > 1.f ? 1.f : v); }
    // [MERGE 1] a at t 0, b at t 1, mixed between: exactly a and b at the ends (a + (b - a) x 1 isn't always b in floats)
    float LifeMix(float a, float b, float t) { return t >= 1.f ? b : (!(t > 0.f) ? a : a + (b - a) * t); }
    float LifeAmount(float v) { return !(v > 0.f) ? 0.f : (v > 2.f ? 2.f : v); }
    // An angle from the voice: 0 for what isn't a number, cut at HeadMost
    float LifeAngle(float v) { return !(v == v) ? 0.f : (v > HeadMost ? HeadMost : (v < -HeadMost ? -HeadMost : v)); }

    int LifeFind(int bones, char const* const* names, char const* name)
    {
      for (int i = 0; i < bones; ++i)
        if (names[i] && !strcmp(names[i], name))
          return i;
      return -1;
    }

    // A bone by name that hangs on the given parent (-1: not there, or hung elsewhere)
    int LifeChild(int bones, char const* const* names, short const* parents, char const* name, int parent)
    {
      int bone = LifeFind(bones, names, name);
      return bone >= 0 && parents[bone] == parent ? bone : -1;
    }
  }

  bool FindLifeRig(int bones, char const* const* names, short const* parents, float const* locals, LifeRig& rig)
  {
    rig = LifeRig();
    if (bones <= 0 || !names || !parents || !locals)
      return false;
    int face = LifeFind(bones, names, "Face"), head = LifeFind(bones, names, "Head");
    if (face < 0 || head < 0 || parents[face] != head)
      return false;
    int headEnd = LifeChild(bones, names, parents, "HeadEnd", head);
    int eye[2] = { LifeChild(bones, names, parents, "LeftEye", face), LifeChild(bones, names, parents, "RightEye", face) };
    if (headEnd < 0 || eye[0] < 0 || eye[1] < 0)
      return false;
    auto place = [&](int bone) { return locals + (size_t)bone * FloatsPerBone; };
    for (int bone : { face, head, headEnd, eye[0], eye[1] })
      for (int k = 0; k < FloatsPerBone; ++k)
        if (!std::isfinite(place(bone)[k]))
          return false;

    // The head's own axes in the Face bone's frame. Right: eye to eye (the eye bones are 60 - 64 mm apart).
    float const* leftEye = place(eye[0]);
    float const* rightEye = place(eye[1]);
    for (int k = 0; k < 3; ++k)
      rig.Right[k] = rightEye[k] - leftEye[k];
    float apart = sqrtf(LifeDot(rig.Right, rig.Right));
    if (!LifeNormalise(rig.Right) || !(apart > 0.02f) || !(apart < 0.2f))
      return false;
    // Up: HeadEnd's place is given in the Head bone's frame; the Face bone is turned against the Head by its own
    // local rotation, so that is taken out
    float const* faceQ = place(face) + 4;
    float back[4] = { -faceQ[0], -faceQ[1], -faceQ[2], faceQ[3] };
    float up[3];
    LifeRot(back, place(headEnd), up);
    float along = LifeDot(up, rig.Right);
    for (int k = 0; k < 3; ++k)
      rig.Up[k] = up[k] - along * rig.Right[k];
    if (!LifeNormalise(rig.Up))
      return false;
    // Forward: square to both (the skeletons' model space has the character's left on +X, up on +Y, the front on
    // +Z: up x right is the front), and towards the nose if the skeleton has one
    LifeCross(rig.Up, rig.Right, rig.Forward);
    int nose = LifeChild(bones, names, parents, "NoseTip", face);
    // [CHECK 2] Whether left and right can be trusted: on the game's skeletons up x right points to the nose. If
    // the nose lies the other way, the eye bones are the wrong way round (or the rig is a mirror image): a blink,
    // a brow raise and a nod still do what they say (their signs are taken from their effect), but a turn and a
    // tilt would go to the wrong side (measured: eye bones swapped, Turn +0.05 took the nose 5.9 mm to the LEFT).
    // The head is then left alone.
    bool sidesKnown = true;
    if (nose >= 0)
    {
      float toNose[3];
      for (int k = 0; k < 3; ++k)
        toNose[k] = place(nose)[k] - (leftEye[k] + rightEye[k]) * 0.5f;
      if (LifeDot(rig.Forward, toNose) < 0.f)
      {
        for (int k = 0; k < 3; ++k)
          rig.Forward[k] = -rig.Forward[k];
        sidesKnown = false;
      }
    }
    if (!LifeNormalise(rig.Forward))
      return false;

    // The signs, by what a small turn does: a turn by a small angle about n takes v to v + angle (n x v)
    float moved[3];
    LifeCross(rig.Right, rig.Forward, moved);     // where a turn about Right takes the front
    rig.NodSign = LifeDot(moved, rig.Up) < 0.f ? 1.f : -1.f;        // down
    LifeCross(rig.Up, rig.Forward, moved);        // where a turn about Up takes the front
    rig.TurnSign = LifeDot(moved, rig.Right) > 0.f ? 1.f : -1.f;    // to the right
    LifeCross(rig.Forward, rig.Up, moved);        // where a turn about Forward takes the top
    rig.TiltSign = LifeDot(moved, rig.Right) > 0.f ? 1.f : -1.f;    // to the right
    rig.Face = face;
    rig.EyeBone[0] = eye[0];   // [ALIVE]
    rig.EyeBone[1] = eye[1];

    // The eyelids: all four on the Face bone
    char const* const upLid[2] = { "LeftUpEyelid", "RightUpEyelid" };
    char const* const lowLid[2] = { "LeftLowEyelid", "RightLowEyelid" };
    bool eyes = true;
    for (int s = 0; s < 2; ++s)
    {
      rig.UpLid[s] = LifeChild(bones, names, parents, upLid[s], face);
      rig.LowLid[s] = LifeChild(bones, names, parents, lowLid[s], face);
      eyes = eyes && rig.UpLid[s] >= 0 && rig.LowLid[s] >= 0;
    }
    for (int s = 0; eyes && s < 2; ++s)
    {
      float const* e = place(eye[s]);
      float const* upper = place(rig.UpLid[s]);
      float const* lower = place(rig.LowLid[s]);
      float toUpper[3], toLower[3];
      for (int k = 0; k < 3; ++k)
      {
        if (!std::isfinite(upper[k]) || !std::isfinite(lower[k]))
          eyes = false;
        toUpper[k] = upper[k] - e[k];
        toLower[k] = lower[k] - e[k];
        rig.LidPlace[s][k] = upper[k];
      }
      if (!eyes)
        break;
      float upperAway = sqrtf(LifeDot(toUpper, toUpper)), lowerAway = sqrtf(LifeDot(toLower, toLower));
      // [MERGE 1] How far on the edge, by the nearer of the two lid bones: 0 up to LidEdgeFrom, 1 from LidEdgeTo on
      // (before: on the edge from LidOnEdgeDistance on, both bones). (At LidEdgeTo the quotient is a number divided
      // by itself: exactly 1; a band width written as its own constant gave 0.99999982 there.)
      float nearer = upperAway < lowerAway ? upperAway : lowerAway;
      float edge = nearer >= LidEdgeTo ? 1.f : LifeUnit((nearer - LidEdgeFrom) / (LidEdgeTo - LidEdgeFrom));
      rig.LidEdge[s] = edge;
      rig.LidOnEdge[s] = edge >= 0.5f;
      // At the eye's middle: the line the game's blink has there, by how far from the eye bone the lid bone sits
      // [CHECK 4] (the name was "far": Windows' headers make that word vanish, should one ever be included here)
      float further = LifeUnit((upperAway - LidLineNear) / (LidLineFar - LidLineNear));
      float middleUp = LidLineUp + (LidLineFarUp - LidLineUp) * further;
      float middleForward = LidLineForward + (LidLineFarForward - LidLineForward) * further;
      // (on the edge: the first line as it is)
      float lineUp = LifeMix(middleUp, LidLineUp, edge), lineForward = LifeMix(middleForward, LidLineForward, edge);
      for (int k = 0; k < 3; ++k)
        rig.LidLine[s][k] = e[k] + rig.Up[k] * lineUp + rig.Forward[k] * lineForward;
      rig.LidAngle[s] = LidPivotAngle;
      for (int k = 0; k < 3; ++k)
        rig.LowLidMove[s][k] = 0.f;
      if (edge > 0.f)
      {
        // On the lid's edge: the upper lid bone goes round the line down to the lower lid bone, and a little on.
        // Each bone's height angle seen from the line (both must lie in front of it)
        float fromLineUpper[3], fromLineLower[3];
        for (int k = 0; k < 3; ++k)
        {
          fromLineUpper[k] = upper[k] - rig.LidLine[s][k];
          fromLineLower[k] = lower[k] - rig.LidLine[s][k];
        }
        float upperFront = LifeDot(fromLineUpper, rig.Forward), lowerFront = LifeDot(fromLineLower, rig.Forward);
        float angle = 0.f;
        if (upperFront > 0.001f && lowerFront > 0.001f)
          angle = LidSweep * (atan2f(LifeDot(fromLineUpper, rig.Up), upperFront) - atan2f(LifeDot(fromLineLower, rig.Up), lowerFront));
        float edgeAngle = !(angle > 0.f) ? 0.f : (angle > LidAngleMost ? LidAngleMost : angle);
        rig.LidAngle[s] = LifeMix(LidPivotAngle, edgeAngle, edge);
        // The lower lid bone: towards the nose (the middle between the eyes), not up
        float toMiddle = 0.f;
        for (int k = 0; k < 3; ++k)
          toMiddle += ((leftEye[k] + rightEye[k]) * 0.5f - lower[k]) * rig.Right[k];
        float side = toMiddle > 0.f ? 1.f : -1.f;
        for (int k = 0; k < 3; ++k)
          // (none when the edge's blink has no way down: between the arrangements the upper lid then still turns, and
          // the lower lid's move would vanish in one frame at the band's end, where the eye is left alone, as before)
          rig.LowLidMove[s][k] = !(edgeAngle > 0.f) ? 0.f :
            (edge >= 1.f ? rig.Right[k] * side * LowLidNasal : rig.Right[k] * side * LowLidNasal * edge);
      }
    }
    rig.Eyes = eyes;

    // The eyebrows: the six of the brow line on the Face bone; the fold bones over the upper lid follow if there
    char const* const brow[6] = { "LeftInnerEyebrow", "LeftMiddleEyebrow", "LeftOuterEyebrow", "RightInnerEyebrow",
      "RightMiddleEyebrow", "RightOuterEyebrow" };
    char const* const fold[4] = { "LeftLowMiddleEyebrow", "LeftLowOuterEyebrow", "RightLowMiddleEyebrow", "RightLowOuterEyebrow" };
    bool brows = true;
    for (int k = 0; k < 6; ++k)
    {
      rig.Brow[k] = LifeChild(bones, names, parents, brow[k], face);
      brows = brows && rig.Brow[k] >= 0;
    }
    for (int k = 0; k < 4; ++k)
      rig.Fold[k] = LifeChild(bones, names, parents, fold[k], face);
    rig.Brows = brows;

    // The head: the Head bone, and Neck1 above it for its share
    rig.HeadBone = head;
    rig.Head = sidesKnown && parents[head] >= 0 && parents[head] < bones;   // [CHECK 2]
    if (rig.Head && names[parents[head]] && !strcmp(names[parents[head]], "Neck1"))
    {
      float const* q = place(parents[head]) + 4;
      float length = sqrtf(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
      if (std::isfinite(length) && length > 0.5f && length < 2.f)
      {
        rig.Neck1 = parents[head];
        for (int k = 0; k < 4; ++k)
          rig.Neck1Q[k] = q[k] / length;
      }
    }
    rig.Ok = rig.Eyes || rig.Brows || rig.Head;
    return rig.Ok;
  }

  float LidFollowCut(float follow, float way)
  {
    // (an eye with no way left is shut, by the pose or the feeling: its lid doesn't follow, up or down)
    if (!(follow == follow) || !(way > 0.f))
      return 0.f;
    // [REVIEW 1] Up too at most LidFollowShare of the way left (and LidFollowUpMost): a nearly shut eye is opened
    // only a little and the follow goes to 0 smoothly as the eye shuts. (Before: 17 degrees whatever the way, so a
    // pose taken mid-blink opened from 2 % to 41 - 47 % and a lid being closed snapped 14.8 degrees at its end:
    // the review, wf_f756d95b-0a5.)
    float most = LidFollowShare * way;
    float up = LidFollowUpMost < most ? LidFollowUpMost : most;
    if (follow < -up)
      return -up;
    return follow > most ? most : follow;
  }

  float LifeBlinkOf(LifeShape const& shape, LifeAmounts const& amounts, int eye)
  {
    float blink = shape.Sided && eye >= 0 && eye <= 1 ? shape.BlinkSide[eye] : shape.Blink;
    return LifeUnit(LifeUnit(blink) * LifeAmount(amounts.Blinks));
  }

  void AddLifeOffsets(LifeRig const& rig, LifeShape const& shape, LifeAmounts const& amounts, float const* headLocalQ,
    float const* faceLocalQ, Offsets& out, float const* neck1LocalQ, int parts)
  {
    if (!rig.Ok)
      return;
    if (out.Count < 0)
      out.Count = 0;
    auto add = [&](int bone, float const* turn, float const* move)
    {
      if (bone < 0 || out.Count >= MaxDriven)
        return;
      for (int k = 0; k < 4; ++k)
        if (!std::isfinite(turn[k]) || (k < 3 && !std::isfinite(move[k])))
          return;
      int n = out.Count++;
      out.Bone[n] = bone;
      memcpy(out.Turn[n], turn, 4 * sizeof(float));
      memcpy(out.Move[n], move, 3 * sizeof(float));
    };
    const float identity[4] = { 0.f, 0.f, 0.f, 1.f };
    const float none[3] = { 0.f, 0.f, 0.f };

    // The blink: each upper lid bone turned about its line (its skin goes round the eye), the lower one towards the
    // nose. Never past shut, whatever the amount.
    // [ALIVE] Each eye its own blink when the shape is Sided, and the lid's follow of its eye laid with it: Follow +
    // Blink x (the whole way - Follow). With no follow the turn is the whole way x Blink, as before.
    if (rig.Eyes && (parts & LifePartLids))
      for (int s = 0; s < 2; ++s)
      {
        float blink = LifeBlinkOf(shape, amounts, s);
        float follow = LidFollowCut(shape.LidFollow[s], rig.LidAngle[s]);
        float angle = follow == 0.f ? rig.LidAngle[s] * blink : follow + blink * (rig.LidAngle[s] - follow);
        if (follow == 0.f ? !(angle > 0.f) : !std::isfinite(angle))
          continue;   // (an eye whose lid bones give no way down: left alone)
        // (a nod's sense: the front goes down)
        float turn[4], fromLine[3], turned[3], move[3];
        LifeAbout(rig.Right, rig.NodSign * angle, turn);
        for (int k = 0; k < 3; ++k)
          fromLine[k] = rig.LidPlace[s][k] - rig.LidLine[s][k];
        LifeRot(turn, fromLine, turned);
        for (int k = 0; k < 3; ++k)
          move[k] = turned[k] - fromLine[k];
        add(rig.UpLid[s], turn, move);
        if (rig.LidEdge[s] > 0.f && blink > 0.f)   // [MERGE 1] (its move is scaled by how far on the edge) [ALIVE] (the blink's only)
        {
          for (int k = 0; k < 3; ++k)
            move[k] = rig.LowLidMove[s][k] * blink;
          add(rig.LowLid[s], identity, move);
        }
      }

    // The brow raise: each brow bone up along the head's Up, the fold bones over the upper lid a share of the way
    float brow = LifeUnit(shape.Brow) * LifeAmount(amounts.Brows);
    if (rig.Brows && shape.Sided && (parts & LifePartBrows))
    {
      // [ALIVE] Each side its own raise (a little under rest too: VoiceAlive's slow tone); a side at 0 adds nothing
      for (int side = 0; side < 2; ++side)
      {
        float value = shape.BrowSide[side];
        value = !(value == value) ? 0.f : (value < MinBrowSide ? MinBrowSide : (value > MaxBrowSide ? MaxBrowSide : value));
        float lift = BrowLift * value * LifeAmount(amounts.Brows);
        if (lift == 0.f)
          continue;
        for (int k = side * 3; k < side * 3 + 3; ++k)
        {
          float move[3];
          for (int c = 0; c < 3; ++c)
            move[c] = rig.Up[c] * lift * BrowShare[k % 3];
          add(rig.Brow[k], identity, move);
        }
        for (int k = side * 2; k < side * 2 + 2; ++k)
        {
          float move[3];
          for (int c = 0; c < 3; ++c)
            move[c] = rig.Up[c] * lift * BrowFoldShare;
          add(rig.Fold[k], identity, move);
        }
      }
    }
    else if (rig.Brows && brow > 0.f && (parts & LifePartBrows))
    {
      float lift = BrowLift * brow;
      for (int k = 0; k < 6; ++k)
      {
        float move[3];
        for (int c = 0; c < 3; ++c)
          move[c] = rig.Up[c] * lift * BrowShare[k % 3];
        add(rig.Brow[k], identity, move);
      }
      for (int k = 0; k < 4; ++k)
      {
        float move[3];
        for (int c = 0; c < 3; ++c)
          move[c] = rig.Up[c] * lift * BrowFoldShare;
        add(rig.Fold[k], identity, move);
      }
    }

    // The head: nod, turn and tilt as one turn about one axis (they are small), given in the Face bone's frame and
    // carried into the Head's parent's frame, where the layer's turn is laid on; a share of it on Neck1, in ITS
    // parent's frame
    float amount = LifeAmount(amounts.Head);
    float nod = LifeAngle(shape.Nod) * amount, turn = LifeAngle(shape.Turn) * amount, tilt = LifeAngle(shape.Tilt) * amount;
    // [CHECK 3] The rotations handed in are made unit length here; with one that is no rotation the head is left
    // alone (before: a quaternion of another length gave a turn about a bent axis)
    float headQ[4], faceQ[4];
    if (rig.Head && (parts & LifePartHead) && LifeTurnOf(headLocalQ, headQ) && LifeTurnOf(faceLocalQ, faceQ) &&
        (nod != 0.f || turn != 0.f || tilt != 0.f))
    {
      float axis[3];
      for (int k = 0; k < 3; ++k)
        axis[k] = rig.Right[k] * rig.NodSign * nod + rig.Up[k] * rig.TurnSign * turn + rig.Forward[k] * rig.TiltSign * tilt;
      float angle = sqrtf(LifeDot(axis, axis));
      float inHead[3], inParent[3];
      LifeRot(faceQ, axis, inHead);
      LifeRot(headQ, inHead, inParent);
      if (std::isfinite(angle) && angle > 1e-7f && LifeNormalise(inParent))
      {
        float share = rig.Neck1 >= 0 ? HeadNeckShare : 0.f;
        float q[4];
        LifeAbout(inParent, angle * (1.f - share), q);
        add(rig.HeadBone, q, none);
        if (share > 0.f)
        {
          // [CHECK 3] (Neck1's live rotation made unit length; one that is no rotation: the rig's own is used)
          float inNeck[3], neckQ[4];
          if (!LifeTurnOf(neck1LocalQ, neckQ))
            memcpy(neckQ, rig.Neck1Q, sizeof(neckQ));
          LifeRot(neckQ, inParent, inNeck);
          if (LifeNormalise(inNeck))
          {
            LifeAbout(inNeck, angle * share, q);
            add(rig.Neck1, q, none);
          }
        }
      }
    }
  }
}
