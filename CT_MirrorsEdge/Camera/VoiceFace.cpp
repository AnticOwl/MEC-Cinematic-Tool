#include "VoiceFace.h"
#include <cmath>
#include <cstring>

namespace voice_face
{
  namespace
  {
    void QMul(float const* a, float const* b, float* out)
    {
      float x = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1];
      float y = a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0];
      float z = a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3];
      float w = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
      out[0] = x; out[1] = y; out[2] = z; out[3] = w;
    }

    // v turned by q
    void QRot(float const* q, float const* v, float* out)
    {
      float tx = 2.f * (q[1] * v[2] - q[2] * v[1]);
      float ty = 2.f * (q[2] * v[0] - q[0] * v[2]);
      float tz = 2.f * (q[0] * v[1] - q[1] * v[0]);
      float x = v[0] + q[3] * tx + (q[1] * tz - q[2] * ty);
      float y = v[1] + q[3] * ty + (q[2] * tx - q[0] * tz);
      float z = v[2] + q[3] * tz + (q[0] * ty - q[1] * tx);
      out[0] = x; out[1] = y; out[2] = z;
    }

    void QAbout(float const* axis, float angle, float* out)
    {
      float s = sinf(angle * 0.5f);
      out[0] = axis[0] * s; out[1] = axis[1] * s; out[2] = axis[2] * s; out[3] = cosf(angle * 0.5f);
    }

    float Dot(float const* a, float const* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

    bool Normalise(float* v)
    {
      float length = sqrtf(Dot(v, v));
      if (!(length > 1e-6f) || !std::isfinite(length))
        return false;
      v[0] /= length; v[1] /= length; v[2] /= length;
      return true;
    }

    float Clamp01(float v) { return !(v > 0.f) ? 0.f : (v > 1.f ? 1.f : v); }
    float Amount(float v) { return !(v > 0.f) ? 0.f : (v > 2.f ? 2.f : v); }

    int Find(int bones, char const* const* names, char const* name)
    {
      for (int i = 0; i < bones; ++i)
        if (names[i] && !strcmp(names[i], name))
          return i;
      return -1;
    }
  }

  bool FindRig(int bones, char const* const* names, short const* parents, float const* locals, Rig& rig)
  {
    rig = Rig();
    if (bones <= 0 || !names || !parents || !locals)
      return false;
    rig.Face = Find(bones, names, "Face");
    rig.Jaw = Find(bones, names, "Jaw");
    char const* const lower[3] = { "LeftLowerLip", "LowerLip", "RightLowerLip" };
    char const* const upper[3] = { "LeftUpperLip", "UpperLip", "RightUpperLip" };
    char const* const corner[2] = { "LeftMouth", "RightMouth" };
    for (int k = 0; k < 3; ++k)
    {
      rig.Lower[k] = Find(bones, names, lower[k]);
      rig.Upper[k] = Find(bones, names, upper[k]);
    }
    for (int k = 0; k < 2; ++k)
      rig.Corner[k] = Find(bones, names, corner[k]);
    int chin = Find(bones, names, "Chin"), nose = Find(bones, names, "NoseTip");
    if (rig.Face < 0 || rig.Jaw < 0 || chin < 0 || nose < 0 || rig.Corner[0] < 0 || rig.Corner[1] < 0)
      return false;
    // The hanging this is made for: the jaw, the upper lips, the corners and the nose on the Face bone, the lower
    // lips and the chin on the jaw
    if (parents[rig.Jaw] != rig.Face || parents[nose] != rig.Face || parents[chin] != rig.Jaw)
      return false;
    for (int k = 0; k < 3; ++k)
      if (rig.Lower[k] < 0 || rig.Upper[k] < 0 || parents[rig.Lower[k]] != rig.Jaw || parents[rig.Upper[k]] != rig.Face)
        return false;
    for (int k = 0; k < 2; ++k)
      if (parents[rig.Corner[k]] != rig.Face)
        return false;

    // Each bone's place in the Face bone's frame
    float const* jaw = locals + (size_t)rig.Jaw * FloatsPerBone;
    auto onFace = [&](int bone, float* out) { memcpy(out, locals + (size_t)bone * FloatsPerBone, 3 * sizeof(float)); };
    auto onJaw = [&](int bone, float* out)
    {
      float turned[3];
      QRot(jaw + 4, locals + (size_t)bone * FloatsPerBone, turned);
      for (int k = 0; k < 3; ++k)
        out[k] = jaw[k] + turned[k];
    };
    float left[3], right[3], nosePlace[3], chinPlace[3], lowerPlace[3][3], upperPlace[3][3];
    onFace(rig.Corner[0], left);
    onFace(rig.Corner[1], right);
    onFace(nose, nosePlace);
    onJaw(chin, chinPlace);
    for (int k = 0; k < 3; ++k)
    {
      onJaw(rig.Lower[k], lowerPlace[k]);
      onFace(rig.Upper[k], upperPlace[k]);
    }
    for (int k = 0; k < 3; ++k)
      rig.Right[k] = right[k] - left[k];
    rig.Width = sqrtf(Dot(rig.Right, rig.Right));
    if (!Normalise(rig.Right) || !(rig.Width > 0.01f) || !(rig.Width < 0.5f))
      return false;
    float up[3];
    for (int k = 0; k < 3; ++k)
      up[k] = nosePlace[k] - chinPlace[k];
    float along = Dot(up, rig.Right);
    for (int k = 0; k < 3; ++k)
      rig.Up[k] = up[k] - along * rig.Right[k];
    if (!Normalise(rig.Up))
      return false;
    rig.Forward[0] = rig.Right[1] * rig.Up[2] - rig.Right[2] * rig.Up[1];
    rig.Forward[1] = rig.Right[2] * rig.Up[0] - rig.Right[0] * rig.Up[2];
    rig.Forward[2] = rig.Right[0] * rig.Up[1] - rig.Right[1] * rig.Up[0];
    // Out of the face: the nose's tip lies in front of the jaw's hinge
    float toNose[3] = { nosePlace[0] - jaw[0], nosePlace[1] - jaw[1], nosePlace[2] - jaw[2] };
    if (Dot(rig.Forward, toNose) < 0.f)
      for (int k = 0; k < 3; ++k)
        rig.Forward[k] = -rig.Forward[k];
    if (!Normalise(rig.Forward))
      return false;

    for (int k = 0; k < 3; ++k)
      rig.JawToLower[k] = lowerPlace[1][k] - jaw[k];
    // The sign of the jaw's turn that lowers the lower lip
    float q[4], turned[3];
    QAbout(rig.Right, 0.1f, q);
    QRot(q, rig.JawToLower, turned);
    float rise = Dot(turned, rig.Up) - Dot(rig.JawToLower, rig.Up);
    if (!(fabsf(rise) > 1e-5f))
      return false;
    rig.JawSign = rise < 0.f ? 1.f : -1.f;

    // Each bone's side of the mouth's middle
    float middle[3] = { (left[0] + right[0]) * 0.5f, (left[1] + right[1]) * 0.5f, (left[2] + right[2]) * 0.5f };
    auto side = [&](float const* place)
    {
      float d[3] = { place[0] - middle[0], place[1] - middle[1], place[2] - middle[2] };
      float s = Dot(d, rig.Right) / (rig.Width * 0.5f);
      // (the middle lip bones lie within a tenth of the half width of the middle)
      return fabsf(s) < 0.15f ? 0.f : (s > 0.f ? 1.f : -1.f);
    };
    for (int k = 0; k < 3; ++k)
    {
      rig.LowerSide[k] = side(lowerPlace[k]);
      rig.UpperSide[k] = side(upperPlace[k]);
    }
    rig.CornerSide[0] = -1.f;
    rig.CornerSide[1] = 1.f;
    rig.Ok = true;
    return true;
  }

  void MakeOffsets(Rig const& rig, Shape const& shape, Amounts const& amounts, float const* jawLocalQ, Offsets& out)
  {
    out.Count = 0;
    if (!rig.Ok || !jawLocalQ)
      return;
    float open = Clamp01(shape.Open), wide = Clamp01(shape.Wide), round = Clamp01(shape.Round);
    float closed = Clamp01(Clamp01(shape.Closed) * Amount(amounts.Closure)), teeth = Clamp01(shape.Teeth);
    float jawAmount = Amount(amounts.Jaw), lips = Amount(amounts.Lips);
    float w = rig.Width;

    // The jaw: open, shut again as the lips press, and only just apart for the teeth
    float angle = JawOpenAngle * jawAmount * open * (1.f - closed);
    float teethAngle = JawTeethAngle * jawAmount;
    if (angle > teethAngle)
      angle += (teethAngle - angle) * teeth * (1.f - closed);
    else
      angle += (teethAngle - angle) * teeth * (1.f - closed) * 0.5f;
    float jawTurn[4];
    QAbout(rig.Right, rig.JawSign * angle, jawTurn);
    // How far the lower lip went down with the jaw
    float lowered[3];
    QRot(jawTurn, rig.JawToLower, lowered);
    float drop = Dot(rig.JawToLower, rig.Up) - Dot(lowered, rig.Up);
    if (drop < 0.f)
      drop = 0.f;

    auto add = [&](int bone, float const* turn, float const* move)
    {
      if (out.Count >= MaxDriven)
        return;
      int n = out.Count++;
      out.Bone[n] = bone;
      memcpy(out.Turn[n], turn, 4 * sizeof(float));
      memcpy(out.Move[n], move, 3 * sizeof(float));
    };
    const float identity[4] = { 0.f, 0.f, 0.f, 1.f };
    const float none[3] = { 0.f, 0.f, 0.f };
    add(rig.Jaw, jawTurn, none);

    // A move given along the face's axes (right, up, forward), in the Face bone's frame
    auto onFace = [&](float right, float up, float forward, float* move)
    {
      for (int k = 0; k < 3; ++k)
        move[k] = rig.Right[k] * right + rig.Up[k] * up + rig.Forward[k] * forward;
    };

    // The corners: down with the jaw, apart for "ee", in and forward for "oo"
    for (int c = 0; c < 2; ++c)
    {
      float s = rig.CornerSide[c];
      float outward = lips * w * (WideCornerOut * wide - RoundCornerIn * round - OpenCornerIn * open * (1.f - closed) +
        TeethCornerOut * teeth);
      float forward = lips * w * (RoundCornerForward * round - WideCornerBack * wide);
      float move[3];
      onFace(s * outward, -CornerFollowsJaw * drop, forward, move);
      add(rig.Corner[c], identity, move);
    }
    // The upper lips
    for (int k = 0; k < 3; ++k)
    {
      float s = rig.UpperSide[k];
      float sideways = lips * w * s * (WideLipOut * wide - RoundLipIn * round);
      float up = lips * w * (OpenUpperUp * open * (1.f - closed) + TeethUpperUp * teeth * (1.f - closed) - ClosedLipPress * closed);
      float forward = lips * w * RoundLipForward * round;
      float move[3];
      onFace(sideways, up, forward, move);
      add(rig.Upper[k], identity, move);
    }
    // The lower lips ride on the jaw: their moves are given in the turned jaw's frame
    float jawNow[4], back[4];
    QMul(jawTurn, jawLocalQ, jawNow);
    back[0] = -jawNow[0]; back[1] = -jawNow[1]; back[2] = -jawNow[2]; back[3] = jawNow[3];
    for (int k = 0; k < 3; ++k)
    {
      float s = rig.LowerSide[k];
      float sideways = lips * w * s * (WideLipOut * wide - RoundLipIn * round);
      float up = lips * w * (ClosedLipPress * closed - TeethLowerDown * teeth * (1.f - closed));
      float forward = lips * w * RoundLipForward * round;
      float onFaceMove[3], move[3];
      onFace(sideways, up, forward, onFaceMove);
      QRot(back, onFaceMove, move);
      add(rig.Lower[k], identity, move);
    }
  }
}
