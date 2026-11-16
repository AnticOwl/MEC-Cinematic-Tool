#include "VoiceGaze.h"
#include <cmath>
#include <cstring>

namespace voice_face
{
  namespace
  {
    // a x b (quaternions x, y, z, w)
    void GazeMul(float const* a, float const* b, float* out)
    {
      float x = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1];
      float y = a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0];
      float z = a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3];
      float w = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
      out[0] = x; out[1] = y; out[2] = z; out[3] = w;
    }

    void GazeConj(float const* q, float* out)
    {
      out[0] = -q[0]; out[1] = -q[1]; out[2] = -q[2]; out[3] = q[3];
    }

    // v turned by the unit quaternion q
    void GazeRot(float const* q, float const* v, float* out)
    {
      float tx = 2.f * (q[1] * v[2] - q[2] * v[1]);
      float ty = 2.f * (q[2] * v[0] - q[0] * v[2]);
      float tz = 2.f * (q[0] * v[1] - q[1] * v[0]);
      float x = v[0] + q[3] * tx + (q[1] * tz - q[2] * ty);
      float y = v[1] + q[3] * ty + (q[2] * tx - q[0] * tz);
      float z = v[2] + q[3] * tz + (q[0] * ty - q[1] * tx);
      out[0] = x; out[1] = y; out[2] = z;
    }

    void GazeAbout(float const* axis, float angle, float* out)
    {
      float s = sinf(angle * 0.5f);
      out[0] = axis[0] * s; out[1] = axis[1] * s; out[2] = axis[2] * s; out[3] = cosf(angle * 0.5f);
    }

    float GazeDot(float const* a, float const* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

    // A rotation handed in, made unit length (as VoiceLifeFace takes them). False: it is none.
    bool GazeUnit(float const* q, float* out)
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

    float GazeClampUnit(float v) { return !(v > -1.f) ? -1.f : (v > 1.f ? 1.f : v); }

    // How far a look (a direction in the Face bone's frame) is from the head's straight ahead, and how far down
    float GazeAway(LifeRig const& rig, float const* look) { return acosf(GazeClampUnit(GazeDot(look, rig.Forward))); }
    float GazeDown(LifeRig const& rig, float const* look) { return -asinf(GazeClampUnit(GazeDot(look, rig.Up))); }

    // How far a look is to the head's right (radians, its side angle about Up)
    float GazeSide(LifeRig const& rig, float const* look) { return atan2f(GazeDot(look, rig.Right), GazeDot(look, rig.Forward)); }

    // [REVIEW 2] [FIX CHECK] A look past the limit brought back onto it. While its height is well inside the limit
    // (15 degrees and more under it) by its side angle only, its height kept: both eyes keep the same up-down move and a
    // nod's counter-turn isn't lost along the limit (before the review the whole turn was shortened to where it
    // crossed the limit: an eye posed near 35 degrees lost every turn with any part outward, up to 7.6 degrees off,
    // and two eyes posed at different side angles ended 4 - 7 degrees apart in height). Nearer the limit in height
    // (and for looks more than 75 degrees to the side) mixed, over 10 (15) degrees, into the nearest point on the limit,
    // straight towards the front: the height-kept cut alone drops all of the side as the height reaches the limit,
    // with no bound on its slope (the fix check: eyes posed 28 - 34 degrees up flicked 5 - 6 degrees in one frame of
    // a slow nod, converged eyes went parallel, a look behind the head flipped 65 degrees).
    // out: the look on the limit; share: how far the nearest point was taken (0 .. 1); sideFrom, sideTo: the side
    // angles of the height-kept cut (the caller turns about Up by their difference times 1 - share: no roll).
    const float GazeKeptBelow = 0.2618f;   // [d] 15 degrees under the limit in height: the height kept in full
    const float GazeKeptBand = 0.1745f;    // [d] mixed into the nearest point over the next 10 degrees
    const float GazeSideFrom = 1.309f;     // [d] and from 75 degrees to the side
    const float GazeSideBand = 0.2618f;    // [d] over 15 more (all the nearest point from 90: a look behind)
    float GazeSmooth(float x) { x = !(x > 0.f) ? 0.f : (x > 1.f ? 1.f : x); return x * x * (3.f - 2.f * x); }
    void GazeOntoLimit(LifeRig const& rig, float const* look, float limit, float* out, float& share, float& sideFrom,
      float& sideTo)
    {
      share = 1.f;
      sideFrom = sideTo = 0.f;
      float r = GazeDot(look, rig.Right), u = GazeDot(look, rig.Up), f = GazeDot(look, rig.Forward);
      float length = sqrtf(r * r + u * u + f * f);
      if (!(length > 1e-6f) || !std::isfinite(length))
      {
        memcpy(out, rig.Forward, 3 * sizeof(float));
        return;
      }
      r /= length;
      u /= length;
      f /= length;
      // The nearest point on the limit: on the great circle from straight ahead through the look
      float across = sqrtf(r * r + u * u);
      float cl = cosf(limit), sl = sinf(limit);
      float nearest[3];
      for (int k = 0; k < 3; ++k)
        nearest[k] = across > 1e-6f ? rig.Forward[k] * cl + (rig.Right[k] * r + rig.Up[k] * u) / across * sl :
          rig.Forward[k] * cl + rig.Right[k] * sl;
      float height = asinf(GazeClampUnit(u)), side = atan2f(r, f);
      float byHeight = GazeSmooth((fabsf(height) - (limit - GazeKeptBelow)) / GazeKeptBand);
      float bySide = GazeSmooth((fabsf(side) - GazeSideFrom) / GazeSideBand);
      share = byHeight > bySide ? byHeight : bySide;
      if (share >= 1.f)
      {
        memcpy(out, nearest, sizeof(nearest));
        return;
      }
      // The height kept: the side cut to where the limit is at this height (|height| < limit - 5 degrees here)
      float most = acosf(GazeClampUnit(cl / cosf(height)));
      float kept = side > most ? most : (side < -most ? -most : side);
      sideFrom = side;
      sideTo = kept;
      float ch = cosf(height), sh = sinf(height), cs = cosf(kept), ss = sinf(kept);
      float keptLook[3];
      for (int k = 0; k < 3; ++k)
        keptLook[k] = rig.Right[k] * ch * ss + rig.Up[k] * sh + rig.Forward[k] * ch * cs;
      if (!(share > 0.f))
      {
        memcpy(out, keptLook, sizeof(keptLook));
        return;
      }
      float mixed[3];
      for (int k = 0; k < 3; ++k)
        mixed[k] = keptLook[k] * (1.f - share) + nearest[k] * share;
      float m = sqrtf(GazeDot(mixed, mixed));
      for (int k = 0; k < 3; ++k)
        out[k] = m > 1e-6f ? mixed[k] / m : nearest[k];
    }

    // The shortest turn taking the unit direction a to the unit direction b
    void GazeArc(float const* a, float const* b, float* out)
    {
      float axis[3] = { a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0] };
      float s = sqrtf(GazeDot(axis, axis)), c = GazeDot(a, b);
      if (!(s > 1e-9f) || !std::isfinite(s))
      {
        out[0] = out[1] = out[2] = 0.f;
        out[3] = 1.f;
        return;
      }
      for (int k = 0; k < 3; ++k)
        axis[k] /= s;
      GazeAbout(axis, atan2f(s, c), out);
    }

    int GazeEntry(Offsets const& out, int bone)
    {
      for (int n = 0; n < out.Count && n < MaxDriven; ++n)
        if (out.Bone[n] == bone)
          return n;
      return -1;
    }
  }

  float LidFollowOf(float lookDown)
  {
    if (!std::isfinite(lookDown))
      return 0.f;
    return lookDown > 0.f ? LidFollowDown * lookDown : LidFollowUp * lookDown;
  }

  bool AddGazeOffsets(LifeRig const& rig, float yaw, float pitch, bool hold, bool lay, float const* neck1Q,
    float const* headQ, float const* faceQ, float const* const eyeQ[2], float const* const takenQ[2], Offsets& out,
    float lookDown[2], float* lookRight)
  {
    lookDown[0] = lookDown[1] = 0.f;
    if (lookRight)
      lookRight[0] = lookRight[1] = 0.f;
    if (out.Count < 0)
      out.Count = 0;
    if (out.Count > MaxDriven)
      out.Count = MaxDriven;
    if (rig.Face < 0 || rig.EyeBone[0] < 0 || rig.EyeBone[1] < 0 || !eyeQ || !eyeQ[0] || !eyeQ[1])
      return false;
    float eye[2][4], taken[2][4];
    for (int s = 0; s < 2; ++s)
    {
      if (!GazeUnit(eyeQ[s], eye[s]))
        return false;
      if (!takenQ || !GazeUnit(takenQ[s], taken[s]))
        memcpy(taken[s], eye[s], sizeof(taken[s]));
    }
    if (!std::isfinite(yaw))
      yaw = 0.f;
    if (!std::isfinite(pitch))
      pitch = 0.f;

    // The gaze in the Face bone's frame: about the head's Up for the yaw, then about its Right for the pitch (the
    // signs proven by their effect, as the head's are: VoiceLifeFace)
    float yawQ[4], pitchQ[4], gaze[4];
    GazeAbout(rig.Up, rig.TurnSign * yaw, yawQ);
    GazeAbout(rig.Right, rig.NodSign * pitch, pitchQ);
    GazeMul(yawQ, pitchQ, gaze);

    // Held in the world: the layer's own turn of the Face bone's frame (D, in that frame) taken back out. The Face
    // bone's frame against Neck1's parent (or the Head's, without Neck1) is N x H x F as posed and
    // (Tn x N) x (Th x H) x F with the layer's turns Tn, Th: D = (N H F)^-1 (Tn N)(Th H) F, and the eye's turn
    // (laid as Turn x its local, in the Face bone's frame) is D^-1 x the gaze.
    float turn[4];
    memcpy(turn, gaze, sizeof(turn));
    if (lay && hold)
    {
      float h[4], f[4];
      if (!GazeUnit(headQ, h) || !GazeUnit(faceQ, f))
        return false;
      const float identity[4] = { 0.f, 0.f, 0.f, 1.f };
      float th[4], tn[4], n[4];
      memcpy(th, identity, sizeof(th));
      memcpy(tn, identity, sizeof(tn));
      memcpy(n, identity, sizeof(n));
      int e = GazeEntry(out, rig.HeadBone);
      if (e >= 0 && !GazeUnit(out.Turn[e], th))
        memcpy(th, identity, sizeof(th));
      if (rig.Neck1 >= 0)
      {
        if (!GazeUnit(neck1Q, n) && !GazeUnit(rig.Neck1Q, n))
          memcpy(n, identity, sizeof(n));
        e = GazeEntry(out, rig.Neck1);
        if (e >= 0 && !GazeUnit(out.Turn[e], tn))
          memcpy(tn, identity, sizeof(tn));
      }
      float before[4], after[4], a[4], b[4];
      GazeMul(n, h, a);
      GazeMul(a, f, before);
      GazeMul(tn, n, a);
      GazeMul(th, h, b);
      float c[4];
      GazeMul(a, b, c);
      GazeMul(c, f, after);
      // D^-1 = after^-1 x before
      float afterInv[4], back[4];
      GazeConj(after, afterInv);
      GazeMul(afterInv, before, back);
      GazeMul(back, gaze, turn);
      float length = sqrtf(turn[0] * turn[0] + turn[1] * turn[1] + turn[2] * turn[2] + turn[3] * turn[3]);
      if (!std::isfinite(length) || !(length > 0.5f))
        return false;
      for (int k = 0; k < 4; ++k)
        turn[k] /= length;
    }

    for (int s = 0; s < 2; ++s)
    {
      // The eye's look as posed (its front: the head's Forward at rest, where the eye bones' local rotation is the
      // identity) and after the turn; brought back onto EyeMost (or the pose's own look, if that is further) where
      // it would pass it, its height kept [REVIEW 2]
      float front[3], look[3], took[3];
      GazeRot(eye[s], rig.Forward, front);
      GazeRot(taken[s], rig.Forward, took);
      float q[4] = { 0.f, 0.f, 0.f, 1.f };   // (not laid: the eyes as posed)
      if (lay)
        memcpy(q, turn, sizeof(q));
      GazeRot(q, front, look);
      // (each eye its own limit: one shared by both let an eye go as far as the other was posed, in hostile poses up to
      // 70 degrees past its own; two eyes both posed past EyeMost at different angles may end up a few degrees apart in
      // height: measured, up to 6 in the fix check's worst random case)
      float limit = GazeAway(rig, front);
      if (limit < EyeMost)
        limit = EyeMost;
      if (GazeAway(rig, look) > limit)
      {
        // (first about the head's Up by the height-kept cut's side change, times 1 - share, then the shortest turn to
        // the look on the limit: continuous over the mixing, and a side-only cut rolls the eye not at all [FIX CHECK])
        float onto[3], side[4], arc[4], back[4], total[4], turned[3], share = 1.f, sideFrom = 0.f, sideTo = 0.f;
        GazeOntoLimit(rig, look, limit, onto, share, sideFrom, sideTo);
        GazeAbout(rig.Up, rig.TurnSign * (sideTo - sideFrom) * (1.f - share), side);
        GazeRot(side, look, turned);
        GazeArc(turned, onto, arc);
        GazeMul(arc, side, back);
        GazeMul(back, q, total);
        float length = sqrtf(total[0] * total[0] + total[1] * total[1] + total[2] * total[2] + total[3] * total[3]);
        if (std::isfinite(length) && length > 0.5f)
        {
          for (int k = 0; k < 4; ++k)
            q[k] = total[k] / length;
          GazeRot(q, front, look);
        }
      }
      float down = GazeDown(rig, look) - GazeDown(rig, took);
      lookDown[s] = std::isfinite(down) ? down : 0.f;
      if (lookRight)
      {
        float right = GazeSide(rig, look) - GazeSide(rig, front);
        lookRight[s] = std::isfinite(right) ? right : 0.f;
      }
      if (!lay)
        continue;
      bool finite = true;
      for (int k = 0; k < 4; ++k)
        finite = finite && std::isfinite(q[k]);
      // (a turn of nothing isn't laid: a voice at rest, before its start or after its end, leaves the eyes out of
      // the layer, as the life leaves the lids; "nothing": under 2e-7 radians, the float residue of taking the
      // head's own turn back out of a head that wasn't turned)
      bool none = q[0] * q[0] + q[1] * q[1] + q[2] * q[2] < 1e-14f;
      if (!finite || none || out.Count >= MaxDriven || GazeEntry(out, rig.EyeBone[s]) >= 0)
        continue;
      int n = out.Count++;
      out.Bone[n] = rig.EyeBone[s];
      memcpy(out.Turn[n], q, sizeof(q));
      out.Move[n][0] = out.Move[n][1] = out.Move[n][2] = 0.f;
    }
    return true;
  }
}
