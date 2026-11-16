#include "TrackPlayer.h"
#include "../Main.h"
#include "../Util/Util.h"
#include "../imgui/imgui.h"
#include "../Util/ImGuiEXT.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

using namespace DirectX;

namespace
{
  const float g_minSegmentDuration = 0.05f;
  const size_t MaxUndoSteps = 100;

  // Pitch / yaw / roll in degrees, matching XMQuaternionRotationRollPitchYaw
  // (roll, then pitch, then yaw). The camera looks down -Z, so positive pitch
  // looks up and positive yaw turns left.
  void QuaternionToAngles(XMFLOAT4 const& rotation, float angles[3])
  {
    XMFLOAT4X4 m;
    XMStoreFloat4x4(&m, XMMatrixRotationQuaternion(XMLoadFloat4(&rotation)));

    float sinPitch = -m._32;
    if (sinPitch > 1.f) sinPitch = 1.f;
    if (sinPitch < -1.f) sinPitch = -1.f;

    float pitch = asinf(sinPitch);
    float yaw, roll;
    if (cosf(pitch) > 0.0001f)
    {
      yaw = atan2f(m._31, m._33);
      roll = atan2f(m._12, m._22);
    }
    else
    {
      yaw = atan2f(-m._13, m._11);
      roll = 0;
    }

    angles[0] = XMConvertToDegrees(pitch);
    angles[1] = XMConvertToDegrees(yaw);
    angles[2] = XMConvertToDegrees(roll);
  }

  XMFLOAT4 AnglesToQuaternion(float const angles[3])
  {
    XMFLOAT4 rotation;
    XMStoreFloat4(&rotation, XMQuaternionRotationRollPitchYaw(
      XMConvertToRadians(angles[0]), XMConvertToRadians(angles[1]), XMConvertToRadians(angles[2])));
    return rotation;
  }

  const char* g_interpolationNames[Interpolation_Count] = { "Smooth", "Linear", "Ease in/out (bezier)", "Hold" };

  // Cubic Hermite from p1 to p2. Tangents come from the neighbouring keyframes
  // in units per second (a Catmull-Rom spline that knows the keyframe times),
  // then get limited to 3x the segment's own slope. That limit is what stops
  // two keyframes at the same spot from swinging out and back when a third one
  // is added elsewhere: a segment that doesn't move gets zero tangents.
  XMVECTOR ClampedHermite(XMVECTOR const& p0, XMVECTOR const& p1, XMVECTOR const& p2, XMVECTOR const& p3,
    float t0, float t1, float t2, float t3, float s)
  {
    float duration = t2 - t1;
    float length = XMVectorGetX(XMVector4Length(p2 - p1));
    if (length < 1e-6f || duration <= 0.f)
      return p1;

    XMVECTOR m1 = (p2 - p0) / (t2 - t0);
    XMVECTOR m2 = (p3 - p1) / (t3 - t1);

    float limit = 3.f * length / duration;
    float m1Length = XMVectorGetX(XMVector4Length(m1));
    float m2Length = XMVectorGetX(XMVector4Length(m2));
    if (m1Length > limit) m1 = m1 * (limit / m1Length);
    if (m2Length > limit) m2 = m2 * (limit / m2Length);

    float s2 = s * s;
    float s3 = s2 * s;
    float h00 = 2.f * s3 - 3.f * s2 + 1.f;
    float h10 = s3 - 2.f * s2 + s;
    float h01 = -2.f * s3 + 3.f * s2;
    float h11 = s3 - s2;

    return p1 * h00 + m1 * (h10 * duration) + p2 * h01 + m2 * (h11 * duration);
  }

  // The tangents ClampedHermite gives the two ends of the segment p1 -> p2
  // (units per second): zero when the segment doesn't move
  void AutoTangents(XMVECTOR const& p0, XMVECTOR const& p1, XMVECTOR const& p2, XMVECTOR const& p3,
    float t0, float t1, float t2, float t3, XMVECTOR& m1, XMVECTOR& m2)
  {
    m1 = m2 = XMVectorZero();
    float duration = t2 - t1;
    float length = XMVectorGetX(XMVector4Length(p2 - p1));
    if (length < 1e-6f || duration <= 0.f)
      return;

    m1 = (p2 - p0) / (t2 - t0);
    m2 = (p3 - p1) / (t3 - t1);

    float limit = 3.f * length / duration;
    float m1Length = XMVectorGetX(XMVector4Length(m1));
    float m2Length = XMVectorGetX(XMVector4Length(m2));
    if (m1Length > limit) m1 = m1 * (limit / m1Length);
    if (m2Length > limit) m2 = m2 * (limit / m2Length);
  }

  // A Smooth or Ease in/out segment's place at s (0 - 1, its eased time) when
  // n1 or n2 has handles set by hand: the cubic Bezier from n1 through n1's
  // way-out handle and n2's way-in handle to n2. An end without handles gets
  // its automatic tangent's (a Hermite tangent m is the Bezier handle
  // m * duration / 3), so it joins the automatic curve on that side. Two
  // keyframes at the same place hold still between them, as the automatic
  // curve does (a duplicated keyframe doesn't loop out on its handles).
  XMVECTOR HandledPosition(CatmullRomNode const& n0, CatmullRomNode const& n1, CatmullRomNode const& n2,
    CatmullRomNode const& n3, float s)
  {
    XMVECTOR p1 = XMLoadFloat3(&n1.Position);
    XMVECTOR p2 = XMLoadFloat3(&n2.Position);
    float duration = n2.TimeStamp - n1.TimeStamp;
    if (duration <= 0.f || XMVectorGetX(XMVector3Length(p2 - p1)) < 1e-6f)
      return p1;
    XMVECTOR m1, m2;
    AutoTangents(XMLoadFloat3(&n0.Position), p1, p2, XMLoadFloat3(&n3.Position), n0.TimeStamp, n1.TimeStamp,
      n2.TimeStamp, n3.TimeStamp, m1, m2);
    XMVECTOR b1 = n1.Handles ? p1 + XMLoadFloat3(&n1.HandleOut) : p1 + m1 * (duration / 3.f);
    XMVECTOR b2 = n2.Handles ? p2 + XMLoadFloat3(&n2.HandleIn) : p2 - m2 * (duration / 3.f);
    float u = 1.f - s;
    return p1 * (u * u * u) + b1 * (3.f * u * u * s) + b2 * (3.f * u * s * s) + p2 * (s * s * s);
  }

  // A Smooth or Ease in/out segment's place at s, as it plays and as its
  // path is drawn: the automatic curve (exactly as before handles existed),
  // or the Bezier through the handles set by hand
  XMVECTOR CurvePosition(CatmullRomNode const& n0, CatmullRomNode const& n1, CatmullRomNode const& n2,
    CatmullRomNode const& n3, float s)
  {
    if (n1.Handles || n2.Handles)
      return HandledPosition(n0, n1, n2, n3, s);
    return ClampedHermite(XMLoadFloat3(&n0.Position), XMLoadFloat3(&n1.Position), XMLoadFloat3(&n2.Position),
      XMLoadFloat3(&n3.Position), n0.TimeStamp, n1.TimeStamp, n2.TimeStamp, n3.TimeStamp, s);
  }

  // The segment from nodes[i] to the next, with the neighbours the curve uses
  // (the ends stand in for missing ones)
  struct SegmentNodes
  {
    CatmullRomNode const& n0;
    CatmullRomNode const& n1;
    CatmullRomNode const& n2;
    CatmullRomNode const& n3;
  };
  SegmentNodes Segment(std::vector<CatmullRomNode> const& nodes, size_t i)
  {
    CatmullRomNode const& n1 = nodes[i];
    CatmullRomNode const& n2 = nodes[i + 1];
    return { i > 0 ? nodes[i - 1] : n1, n1, n2, i + 2 < nodes.size() ? nodes[i + 2] : n2 };
  }

  // The handles nodes[i]'s automatic curve has on each side (see
  // HandledPosition), for when its handles are first set by hand, so the
  // curve stays as it was until they're moved
  void AutomaticHandles(std::vector<CatmullRomNode> const& nodes, size_t i, XMFLOAT3& in, XMFLOAT3& out)
  {
    in = out = XMFLOAT3(0.f, 0.f, 0.f);
    XMVECTOR m1, m2;
    if (i + 1 < nodes.size())
    {
      SegmentNodes s = Segment(nodes, i);
      AutoTangents(XMLoadFloat3(&s.n0.Position), XMLoadFloat3(&s.n1.Position), XMLoadFloat3(&s.n2.Position),
        XMLoadFloat3(&s.n3.Position), s.n0.TimeStamp, s.n1.TimeStamp, s.n2.TimeStamp, s.n3.TimeStamp, m1, m2);
      XMStoreFloat3(&out, m1 * ((s.n2.TimeStamp - s.n1.TimeStamp) / 3.f));
    }
    if (i > 0 && i < nodes.size())
    {
      SegmentNodes s = Segment(nodes, i - 1);
      AutoTangents(XMLoadFloat3(&s.n0.Position), XMLoadFloat3(&s.n1.Position), XMLoadFloat3(&s.n2.Position),
        XMLoadFloat3(&s.n3.Position), s.n0.TimeStamp, s.n1.TimeStamp, s.n2.TimeStamp, s.n3.TimeStamp, m1, m2);
      XMStoreFloat3(&in, m2 * (-(s.n2.TimeStamp - s.n1.TimeStamp) / 3.f));
    }
  }

  // A keyframe goes in at time between nodes[i] and nodes[i + 1]: the
  // handles set by hand at those two ends are shortened to their part of the
  // segment, in proportion to its time (as splitting a Bezier curve shortens
  // them), so the curve doesn't swing out past the new keyframe
  void ShortenHandlesForInsert(std::vector<CatmullRomNode>& nodes, size_t i, float time)
  {
    if (i + 1 >= nodes.size())
      return;
    float duration = nodes[i + 1].TimeStamp - nodes[i].TimeStamp;
    if (!(duration > 0.f))
      return;
    float before = (time - nodes[i].TimeStamp) / duration;
    before = before < 0.f ? 0.f : (before > 1.f ? 1.f : before);
    if (nodes[i].Handles)
      XMStoreFloat3(&nodes[i].HandleOut, XMLoadFloat3(&nodes[i].HandleOut) * before);
    if (nodes[i + 1].Handles)
      XMStoreFloat3(&nodes[i + 1].HandleIn, XMLoadFloat3(&nodes[i + 1].HandleIn) * (1.f - before));
  }

  bool CurvedInterpolation(int interpolation)
  {
    return interpolation == Interpolation_Smooth || interpolation == Interpolation_EaseInOut;
  }

  // CSS-style cubic-bezier(0.42, 0, 0.58, 1): starts and ends at zero speed
  float BezierEaseInOut(float x)
  {
    const float x1 = 0.42f, x2 = 0.58f;
    float t = x;
    for (int i = 0; i < 8; ++i)
    {
      float u = 1.f - t;
      float bx = 3.f * u * u * t * x1 + 3.f * u * t * t * x2 + t * t * t;
      float dbx = 3.f * u * u * x1 + 6.f * u * t * (x2 - x1) + 3.f * t * t * (1.f - x2);
      if (fabsf(dbx) < 1e-6f) break;
      t -= (bx - x) / dbx;
      if (t < 0.f) t = 0.f;
      if (t > 1.f) t = 1.f;
    }
    float u = 1.f - t;
    return 3.f * u * t * t + t * t * t;
  }

  const float g_daySeconds = 86400.f;

  // How far the clock moves from one keyframe's time of day to the next:
  // forward (through midnight if needed), or backward when asked
  float ClockStep(float from, float to, bool backward)
  {
    float step = fmodf(to - from, g_daySeconds);
    if (step < 0) step += g_daySeconds;
    if (backward && step > 0) step -= g_daySeconds;
    return step;
  }

  float WrapTimeOfDay(float seconds)
  {
    seconds = fmodf(seconds, g_daySeconds);
    return seconds < 0 ? seconds + g_daySeconds : seconds;
  }

  void FormatTimeOfDay(float seconds, char* buffer, size_t size)
  {
    int s = (int)WrapTimeOfDay(seconds);
    sprintf_s(buffer, size, "%02d:%02d", s / 3600, (s / 60) % 60);
  }

  // q and -q are the same rotation. Keep neighbouring nodes in the same
  // hemisphere so the spline doesn't spin the long way around.
  void FixRotationSigns(std::vector<CatmullRomNode>& nodes)
  {
    for (size_t i = 1; i < nodes.size(); ++i)
    {
      XMVECTOR qPrevious = XMLoadFloat4(&nodes[i - 1].Rotation);
      XMVECTOR qCurrent = XMLoadFloat4(&nodes[i].Rotation);
      if (XMVectorGetX(XMQuaternionDot(qPrevious, qCurrent)) < 0)
        XMStoreFloat4(&nodes[i].Rotation, XMVectorNegate(qCurrent));
    }
  }

  bool SameNode(CatmullRomNode const& a, CatmullRomNode const& b)
  {
    return a.Position.x == b.Position.x && a.Position.y == b.Position.y && a.Position.z == b.Position.z &&
      a.Rotation.x == b.Rotation.x && a.Rotation.y == b.Rotation.y && a.Rotation.z == b.Rotation.z &&
      a.Rotation.w == b.Rotation.w && a.FieldOfView == b.FieldOfView && a.TimeStamp == b.TimeStamp &&
      a.Interpolation == b.Interpolation && a.MotionBlur == b.MotionBlur && a.TimeOfDay == b.TimeOfDay &&
      a.TimeOfDayBackward == b.TimeOfDayBackward && a.Orbit == b.Orbit && a.OrbitYaw == b.OrbitYaw &&
      a.OrbitPitch == b.OrbitPitch && a.OrbitDistance == b.OrbitDistance && a.Shake == b.Shake &&
      a.GameSpeed == b.GameSpeed && a.Handles == b.Handles && a.HandlesFree == b.HandlesFree &&
      a.HandleIn.x == b.HandleIn.x && a.HandleIn.y == b.HandleIn.y && a.HandleIn.z == b.HandleIn.z &&
      a.HandleOut.x == b.HandleOut.x && a.HandleOut.y == b.HandleOut.y && a.HandleOut.z == b.HandleOut.z &&
      a.OrbitWay == b.OrbitWay && a.OrbitTurns == b.OrbitTurns && a.OrbitLift == b.OrbitLift &&
      a.OrbitBulge == b.OrbitBulge && a.OrbitFocusSet == b.OrbitFocusSet && a.OrbitFocus.x == b.OrbitFocus.x &&
      a.OrbitFocus.y == b.OrbitFocus.y && a.OrbitFocus.z == b.OrbitFocus.z &&
      a.OrbitView.x == b.OrbitView.x && a.OrbitView.y == b.OrbitView.y && a.OrbitView.z == b.OrbitView.z &&
      a.OrbitView.w == b.OrbitView.w;
  }

  bool SameNodes(std::vector<CatmullRomNode> const& a, std::vector<CatmullRomNode> const& b)
  {
    if (a.size() != b.size())
      return false;
    for (size_t i = 0; i < a.size(); ++i)
      if (!SameNode(a[i], b[i]))
        return false;
    return true;
  }

  // The shake strength at mu between n1 and n2 (as the field of view goes),
  // -1 when either has none
  float InterpolateShake(CatmullRomNode const& n0, CatmullRomNode const& n1, CatmullRomNode const& n2,
    CatmullRomNode const& n3, float mu)
  {
    if (n1.Shake < 0.f || n2.Shake < 0.f)
      return -1.f;
    if (n1.Interpolation == Interpolation_Hold)
      return n1.Shake;
    if (n1.Interpolation == Interpolation_Linear)
      return n1.Shake + (n2.Shake - n1.Shake) * mu;
    CatmullRomNode const& m0 = (&n0 != &n1 && n0.Shake >= 0.f) ? n0 : n1;
    CatmullRomNode const& m3 = (&n3 != &n2 && n3.Shake >= 0.f) ? n3 : n2;
    float s = (n1.Interpolation == Interpolation_EaseInOut) ? BezierEaseInOut(mu) : mu;
    float shake = XMVectorGetX(ClampedHermite(XMVectorReplicate(m0.Shake), XMVectorReplicate(n1.Shake),
      XMVectorReplicate(n2.Shake), XMVectorReplicate(m3.Shake), m0.TimeStamp, n1.TimeStamp, n2.TimeStamp, m3.TimeStamp, s));
    return shake < 0.f ? 0.f : (shake > 1.f ? 1.f : shake);
  }

  // The game speed at mu between n1 and n2, as the shake goes; -1 when
  // either has none
  float InterpolateGameSpeed(CatmullRomNode const& n0, CatmullRomNode const& n1, CatmullRomNode const& n2,
    CatmullRomNode const& n3, float mu)
  {
    if (!(n1.GameSpeed > 0.f) || !(n2.GameSpeed > 0.f))
      return -1.f;
    if (n1.Interpolation == Interpolation_Hold)
      return n1.GameSpeed;
    float speed;
    if (n1.Interpolation == Interpolation_Linear)
      speed = n1.GameSpeed + (n2.GameSpeed - n1.GameSpeed) * mu;
    else
    {
      CatmullRomNode const& m0 = (&n0 != &n1 && n0.GameSpeed > 0.f) ? n0 : n1;
      CatmullRomNode const& m3 = (&n3 != &n2 && n3.GameSpeed > 0.f) ? n3 : n2;
      float s = (n1.Interpolation == Interpolation_EaseInOut) ? BezierEaseInOut(mu) : mu;
      speed = XMVectorGetX(ClampedHermite(XMVectorReplicate(m0.GameSpeed), XMVectorReplicate(n1.GameSpeed),
        XMVectorReplicate(n2.GameSpeed), XMVectorReplicate(m3.GameSpeed), m0.TimeStamp, n1.TimeStamp, n2.TimeStamp,
        m3.TimeStamp, s));
    }
    return speed < GameSpeedMin ? GameSpeedMin : (speed > GameSpeedMax ? GameSpeedMax : speed);
  }

  // The keyframes' game speed at a time, the way PlayForward finds it
  // (fallback where they have none)
  float NodesGameSpeedAt(std::vector<CatmullRomNode> const& nodes, double time, float fallback)
  {
    if (nodes.size() < 2)
      return fallback;
    float speed;
    if (time <= 0)
      speed = nodes.front().GameSpeed;
    else if (time >= nodes.back().TimeStamp)
      speed = nodes.back().GameSpeed;
    else
    {
      size_t i = 0;
      while (i + 2 < nodes.size() && time >= nodes[i + 1].TimeStamp)
        ++i;
      CatmullRomNode const& n1 = nodes[i];
      CatmullRomNode const& n2 = nodes[i + 1];
      CatmullRomNode const& n0 = i > 0 ? nodes[i - 1] : n1;
      CatmullRomNode const& n3 = i + 2 < nodes.size() ? nodes[i + 2] : n2;
      speed = InterpolateGameSpeed(n0, n1, n2, n3, (float)((time - n1.TimeStamp) / (n2.TimeStamp - n1.TimeStamp)));
    }
    return speed > 0.f ? speed : fallback;
  }

  bool IsOrbitKeyframe(CatmullRomNode const& node)
  {
    return node.Orbit && node.OrbitDistance > 0.f;
  }

  // How far round (radians of yaw) the track goes from orbit keyframe n1 to
  // n2: the shorter way (OrbitWay 0, as before), or the way n1 says, with its
  // whole turns more
  float OrbitStep(CatmullRomNode const& n1, CatmullRomNode const& n2)
  {
    float step = XMScalarModAngle(n2.OrbitYaw - n1.OrbitYaw);
    if (n1.OrbitWay == 0)
      return step;
    if (n1.OrbitWay > 0 && step < 0.f)
      step += XM_2PI;
    else if (n1.OrbitWay < 0 && step > 0.f)
      step -= XM_2PI;
    int turns = n1.OrbitTurns > 0 ? n1.OrbitTurns : 0;
    return step + (n1.OrbitWay > 0 ? 1.f : -1.f) * XM_2PI * (float)turns;
  }

  // The least distance from the focus an arc's bulge takes it to (a bulge
  // in further than that is held there, not through the focus)
  const float OrbitBulgeLeast = 0.05f;

  // An arc's bulge at s (0 - 1 along it): out by n1's bulge in the middle,
  // none at the keyframes; distance as the arc has it there without it
  float BulgedDistance(CatmullRomNode const& n1, float distance, float s)
  {
    if (n1.OrbitBulge == 0.f)
      return distance;
    distance += n1.OrbitBulge * sinf(XM_PI * s);
    return distance < OrbitBulgeLeast ? OrbitBulgeLeast : distance;
  }

  // Where round the focus at mu between two orbit keyframes n1 and n2,
  // interpolated as the segment is (Hold, Linear, or smooth / eased with its
  // neighbours when they're orbit keyframes too, else as at the track's
  // ends). The yaw goes from each keyframe to the next the way it has (see
  // OrbitStep), the pitch rises by its lift in the middle and the distance by
  // its bulge, and the own view (OrbitView) turns from n1's to n2's. The point
  // it goes round goes from n1's to n2's the same way (OrbitFocus; set when
  // both have one, as their copies made by LiveSegment all do).
  // out.Orbit is false when n1 and n2 aren't both orbit keyframes.
  void InterpolateOrbit(CatmullRomNode const& n0, CatmullRomNode const& n1, CatmullRomNode const& n2,
    CatmullRomNode const& n3, float mu, CatmullRomNode& out)
  {
    out.Orbit = IsOrbitKeyframe(n1) && IsOrbitKeyframe(n2);
    if (!out.Orbit)
      return;
    out.OrbitFocusSet = n1.OrbitFocusSet && n2.OrbitFocusSet;
    auto pivot = [&](float t)
    {
      XMStoreFloat3(&out.OrbitFocus, XMVectorLerp(XMLoadFloat3(&n1.OrbitFocus), XMLoadFloat3(&n2.OrbitFocus), t));
    };
    // The own view: none when neither has one (exactly, as before views existed)
    auto view = [&](float t)
    {
      if (IsPlainOrbitView(n1.OrbitView) && IsPlainOrbitView(n2.OrbitView))
        out.OrbitView = XMFLOAT4(0.f, 0.f, 0.f, 1.f);
      else
        XMStoreFloat4(&out.OrbitView, XMQuaternionNormalize(XMQuaternionSlerp(XMLoadFloat4(&n1.OrbitView),
          XMLoadFloat4(&n2.OrbitView), t)));
    };
    bool has0 = &n0 != &n1 && IsOrbitKeyframe(n0);
    bool has3 = &n3 != &n2 && IsOrbitKeyframe(n3);
    CatmullRomNode const& m0 = has0 ? n0 : n1;
    CatmullRomNode const& m3 = has3 ? n3 : n2;
    float y1 = n1.OrbitYaw;
    float y2 = y1 + OrbitStep(n1, n2);
    float y0 = has0 ? y1 - OrbitStep(n0, n1) : y1;
    float y3 = has3 ? y2 + OrbitStep(n2, n3) : y2;

    if (n1.Interpolation == Interpolation_Hold)
    {
      out.OrbitYaw = n1.OrbitYaw;
      out.OrbitPitch = n1.OrbitPitch;
      out.OrbitDistance = n1.OrbitDistance;
      out.OrbitView = n1.OrbitView;
      pivot(0.f);
      return;
    }
    if (n1.Interpolation == Interpolation_Linear)
    {
      pivot(mu);
      out.OrbitYaw = y1 + (y2 - y1) * mu;
      out.OrbitPitch = n1.OrbitPitch + (n2.OrbitPitch - n1.OrbitPitch) * mu;
      if (n1.OrbitLift != 0.f)
        out.OrbitPitch += n1.OrbitLift * sinf(XM_PI * mu);
      out.OrbitDistance = BulgedDistance(n1, n1.OrbitDistance + (n2.OrbitDistance - n1.OrbitDistance) * mu, mu);
      view(mu);
      return;
    }
    float s = (n1.Interpolation == Interpolation_EaseInOut) ? BezierEaseInOut(mu) : mu;
    pivot(s);
    float t0 = m0.TimeStamp, t1 = n1.TimeStamp, t2 = n2.TimeStamp, t3 = m3.TimeStamp;
    auto hermite = [&](float a, float b, float c, float d)
    {
      return XMVectorGetX(ClampedHermite(XMVectorReplicate(a), XMVectorReplicate(b), XMVectorReplicate(c),
        XMVectorReplicate(d), t0, t1, t2, t3, s));
    };
    out.OrbitYaw = hermite(y0, y1, y2, y3);
    out.OrbitPitch = hermite(m0.OrbitPitch, n1.OrbitPitch, n2.OrbitPitch, m3.OrbitPitch);
    if (n1.OrbitLift != 0.f)
      out.OrbitPitch += n1.OrbitLift * sinf(XM_PI * s);
    out.OrbitDistance = BulgedDistance(n1, hermite(m0.OrbitDistance, n1.OrbitDistance, n2.OrbitDistance,
      m3.OrbitDistance), s);
    view(s);
  }

  // The camera at direction from the focus (OrbitDirection), looking at it, level
  XMVECTOR LookAtFocus(XMVECTOR direction)
  {
    // Rows right, up and back: the camera looks down -back, at the focus
    XMVECTOR back = XMVector3Normalize(direction);
    XMVECTOR right = XMVector3Cross(XMVectorSet(0.f, 1.f, 0.f, 0.f), back);
    if (XMVectorGetX(XMVector3LengthSq(right)) < 1e-8f)
      right = XMVectorSet(1.f, 0.f, 0.f, 0.f);   // straight above or below
    right = XMVector3Normalize(right);
    XMVECTOR up = XMVector3Cross(back, right);
    XMMATRIX rows(right, up, back, XMVectorSet(0.f, 0.f, 0.f, 1.f));
    return XMQuaternionNormalize(XMQuaternionRotationMatrix(rows));
  }

  // An orbit keyframe's view at direction from the focus: its own view
  // (OrbitView) on top of looking at the focus
  XMVECTOR OrbitViewAt(CatmullRomNode const& node, XMVECTOR direction)
  {
    XMVECTOR look = LookAtFocus(direction);
    if (IsPlainOrbitView(node.OrbitView))
      return look;
    return XMQuaternionNormalize(XMQuaternionMultiply(XMLoadFloat4(&node.OrbitView), look));
  }

  // An orbit keyframe that can be placed: round its own point
  // (CatmullRomNode::OrbitFocus), or, without one, round the focus where it is
  // (liveFocus; null: none, then it can't)
  bool Placeable(CatmullRomNode const& node, XMFLOAT3 const* liveFocus)
  {
    return IsOrbitKeyframe(node) && (node.OrbitFocusSet || liveFocus);
  }

  // A placeable orbit keyframe (a copy, or one being moved) in terms of the
  // point it goes round: one without its own gets the focus where it is now,
  // its yaw taken from the heading the circle turns with (liveBaseYaw) to the
  // world's, as one with its own has it
  void NormalizeOrbit(CatmullRomNode& node, XMFLOAT3 const* liveFocus, float liveBaseYaw)
  {
    if (node.OrbitFocusSet || !liveFocus)
      return;
    node.OrbitFocus = *liveFocus;
    node.OrbitYaw = XMScalarModAngle(node.OrbitYaw + liveBaseYaw);
    node.OrbitFocusSet = true;
  }

  // Where a normalized orbit keyframe (or what InterpolateOrbit gives between
  // two) is: round its point
  XMVECTOR OrbitPlaceOf(CatmullRomNode const& node)
  {
    return XMLoadFloat3(&node.OrbitFocus) + OrbitDirection(node.OrbitYaw, node.OrbitPitch) * node.OrbitDistance;
  }

  // A normalized orbit keyframe where it is: round its point, at its place
  // round it, with its own view there (as CameraManager places the camera
  // between orbit keyframes)
  void PlaceRoundFocus(CatmullRomNode& node)
  {
    XMVECTOR direction = OrbitDirection(node.OrbitYaw, node.OrbitPitch);
    XMStoreFloat3(&node.Position, XMLoadFloat3(&node.OrbitFocus) + direction * node.OrbitDistance);
    XMStoreFloat4(&node.Rotation, OrbitViewAt(node, direction));
  }

  // A keyframe that isn't an orbit keyframe (or can't be placed), next to one:
  // where it is round the point the move goes round (its own place exactly,
  // as yaw from the world's heading, pitch and distance), so the move
  // between them goes round it too. Its rotation stays its own.
  void MakePolar(CatmullRomNode& node, XMFLOAT3 const& pivot)
  {
    XMFLOAT3 offset;
    XMStoreFloat3(&offset, XMLoadFloat3(&node.Position) - XMLoadFloat3(&pivot));
    float distance = sqrtf(offset.x * offset.x + offset.y * offset.y + offset.z * offset.z);
    node.Orbit = true;
    node.OrbitFocus = pivot;
    node.OrbitFocusSet = true;
    node.OrbitYaw = offset.x * offset.x + offset.z * offset.z > 1e-8f ? XMScalarModAngle(atan2f(offset.x, offset.z)) : 0.f;
    float sine = distance > 1e-4f ? offset.y / distance : 0.f;
    node.OrbitPitch = asinf(sine < -1.f ? -1.f : (sine > 1.f ? 1.f : sine));
    node.OrbitDistance = distance > 1e-4f ? distance : 1e-4f;
  }

  // A segment's keyframes (p0 - p3, as Segment has them) as they play: the
  // placeable orbit keyframes round their points (NormalizeOrbit,
  // PlaceRoundFocus), in copies in live (an end standing in for a missing
  // neighbour stays the same keyframe), their rotations kept in one
  // hemisphere as FixRotationSigns keeps the keyframes'. When an end is one,
  // the move goes round its point (the first end's, else the second's), so
  // the others are taken as where they are round that (MakePolar). False, and
  // the pointers left alone, when none of them can be placed.
  bool LiveSegment(CatmullRomNode const*& p0, CatmullRomNode const*& p1, CatmullRomNode const*& p2,
    CatmullRomNode const*& p3, CatmullRomNode (&live)[4], XMFLOAT3 const* liveFocus, float liveBaseYaw)
  {
    if (!Placeable(*p0, liveFocus) && !Placeable(*p1, liveFocus) && !Placeable(*p2, liveFocus) && !Placeable(*p3, liveFocus))
      return false;
    bool same0 = p0 == p1, same3 = p3 == p2;
    bool round1 = Placeable(*p1, liveFocus), round2 = Placeable(*p2, liveFocus);
    live[0] = *p0;
    live[1] = *p1;
    live[2] = *p2;
    live[3] = *p3;
    bool placed[4];
    for (int k = 0; k < 4; ++k)
    {
      placed[k] = Placeable(live[k], liveFocus);
      if (placed[k])
      {
        NormalizeOrbit(live[k], liveFocus, liveBaseYaw);
        PlaceRoundFocus(live[k]);
      }
    }
    if (round1 || round2)
    {
      XMFLOAT3 pivot = round1 ? live[1].OrbitFocus : live[2].OrbitFocus;
      for (int k = 0; k < 4; ++k)
        if (!placed[k])
          MakePolar(live[k], pivot);
    }
    for (int k = 1; k < 4; ++k)
    {
      XMVECTOR q = XMLoadFloat4(&live[k].Rotation);
      if (XMVectorGetX(XMQuaternionDot(XMLoadFloat4(&live[k - 1].Rotation), q)) < 0.f)
        XMStoreFloat4(&live[k].Rotation, XMVectorNegate(q));
    }
    p1 = &live[1];
    p2 = &live[2];
    p0 = same0 ? &live[1] : &live[0];
    p3 = same3 ? &live[2] : &live[3];
    return true;
  }

  // Same position, rotation and FOV (within a centimetre / a few hundredths
  // of a degree), so the camera holds still between the two
  bool IsSameView(CatmullRomNode const& a, CatmullRomNode const& b)
  {
    float distance = XMVectorGetX(XMVector3Length(XMLoadFloat3(&a.Position) - XMLoadFloat3(&b.Position)));
    float dot = fabsf(XMVectorGetX(XMQuaternionDot(XMLoadFloat4(&a.Rotation), XMLoadFloat4(&b.Rotation))));
    return distance < 0.01f && dot > 0.9999999f && fabsf(a.FieldOfView - b.FieldOfView) < 0.01f;
  }
}

TrackPlayer::TrackPlayer() :
  m_IsPlaying(false),
  m_LockRotation(true),
  m_LockFieldOfView(true),
  m_ManualPlay(false),
  m_PlaybackSpeed(1.0f),
  m_DefaultDuration(3.0f),
  m_DefaultInterpolation(Interpolation_Smooth),
  m_CurrentNode(0),
  m_CurrentTime(0),
  m_SelectedTrack(0),
  m_RunningId(2)
{
  m_Tracks.emplace_back("Track #1");
  UpdateNameList();
}

TrackPlayer::~TrackPlayer()
{

}

CatmullRomNode TrackPlayer::NodeFromCamera(Camera const& camera)
{
  CatmullRomNode node;
  node.FieldOfView = camera.FieldOfView;
  node.Rotation = camera.Rotation;
  node.Position = camera.Position;
  node.TimeStamp = 0;
  node.Interpolation = m_DefaultInterpolation;
  node.MotionBlur = camera.MotionBlur;
  node.TimeOfDay = camera.TimeOfDay;
  node.Orbit = camera.Orbiting && camera.OrbitDistance > 0.f;
  // (round the focus where it was: see CatmullRomNode::OrbitFocus)
  node.OrbitFocusSet = node.Orbit;
  node.OrbitFocus = camera.OrbitFocus;
  node.OrbitYaw = camera.OrbitYaw;
  node.OrbitPitch = camera.OrbitPitch;
  node.OrbitDistance = node.Orbit ? camera.OrbitDistance : 0.f;
  node.Shake = camera.ShakeStrength < 0.f ? 0.f : (camera.ShakeStrength > 1.f ? 1.f : camera.ShakeStrength);
  node.GameSpeed = !(camera.GameSpeed > GameSpeedMin) ? GameSpeedMin :
    (camera.GameSpeed > GameSpeedMax ? GameSpeedMax : camera.GameSpeed);
  return node;
}

void TrackPlayer::CreateNode(Camera const& camera)
{
  if (m_IsPlaying) return;

  std::vector<CatmullRomNode>& nodes = m_Tracks[m_SelectedTrack].Nodes;
  PushUndo();

  CatmullRomNode newNode = NodeFromCamera(camera);

  if (!nodes.empty())
  {
    CatmullRomNode const& lastNode = nodes.back();
    newNode.TimeStamp = lastNode.TimeStamp + m_DefaultDuration;

    // q and -q are the same rotation. Keep neighbouring nodes in the same
    // hemisphere so the spline doesn't spin the long way around.
    XMVECTOR qLast = XMLoadFloat4(&lastNode.Rotation);
    XMVECTOR qNew = XMLoadFloat4(&newNode.Rotation);
    if (XMVectorGetX(XMQuaternionDot(qLast, qNew)) < 0)
      XMStoreFloat4(&newNode.Rotation, XMVectorNegate(qNew));
  }

  nodes.push_back(newNode);
  SyncShadow();
  util::log::Write("Node created, total nodes: %d%s", (int)nodes.size(),
    newNode.Orbit ? " (made orbiting: to and from other orbit keyframes the track goes round the point it was made round)" : "");
  if (nodes.size() >= 2 && IsSameView(nodes[nodes.size() - 2], newNode))
    util::log::Warning("Keyframe %d is the same view as the one before it, the camera holds still between them", (int)nodes.size());
}

void TrackPlayer::DeleteNode()
{
  if (m_IsPlaying) return;

  std::vector<CatmullRomNode>& nodes = m_Tracks[m_SelectedTrack].Nodes;
  if (nodes.size() == 0) return;

  PushUndo();
  nodes.pop_back();
  SyncShadow();
  util::log::Write("Deleted node, remaining nodes %d", (int)nodes.size());
}

void TrackPlayer::PushUndo()
{
  m_Undo.push_back({ m_SelectedTrack, m_Tracks[m_SelectedTrack].Nodes });
  if (m_Undo.size() > MaxUndoSteps)
    m_Undo.erase(m_Undo.begin());
  m_Redo.clear();
}

void TrackPlayer::SyncShadow()
{
  m_Shadow = m_Tracks[m_SelectedTrack].Nodes;
  m_ShadowTrack = m_SelectedTrack;
  m_Changing = false;
}

void TrackPlayer::NoteChanges()
{
  std::vector<CatmullRomNode> const& nodes = m_Tracks[m_SelectedTrack].Nodes;
  // Another track picked: nothing changed
  if (m_ShadowTrack != m_SelectedTrack)
  {
    SyncShadow();
    return;
  }
  if (!SameNodes(nodes, m_Shadow))
  {
    if (!m_Changing)
    {
      m_Undo.push_back({ m_SelectedTrack, m_Shadow });   // as it was before the change
      if (m_Undo.size() > MaxUndoSteps)
        m_Undo.erase(m_Undo.begin());
      m_Redo.clear();
      m_Changing = true;
    }
    m_Shadow = nodes;
  }
  if (!ImGui::IsAnyItemActive())
    m_Changing = false;
}

bool TrackPlayer::Undo()
{
  if (m_IsPlaying || m_Undo.empty())
    return false;
  UndoStep step = std::move(m_Undo.back());
  m_Undo.pop_back();
  if (step.Track < 0 || step.Track >= (int)m_Tracks.size())
    return false;
  m_Redo.push_back({ step.Track, m_Tracks[step.Track].Nodes });
  m_Tracks[step.Track].Nodes = std::move(step.Nodes);
  m_SelectedTrack = step.Track;
  SyncShadow();
  EndPathDrag();   // a drag on the path going on carries on from these, as a step of its own
  util::log::Write("Track: undone (%s, %d keyframes)", m_Tracks[step.Track].Name.c_str(), (int)m_Tracks[step.Track].Nodes.size());
  return true;
}

bool TrackPlayer::Redo()
{
  if (m_IsPlaying || m_Redo.empty())
    return false;
  UndoStep step = std::move(m_Redo.back());
  m_Redo.pop_back();
  if (step.Track < 0 || step.Track >= (int)m_Tracks.size())
    return false;
  m_Undo.push_back({ step.Track, m_Tracks[step.Track].Nodes });
  m_Tracks[step.Track].Nodes = std::move(step.Nodes);
  m_SelectedTrack = step.Track;
  SyncShadow();
  EndPathDrag();
  util::log::Write("Track: redone (%s, %d keyframes)", m_Tracks[step.Track].Name.c_str(), (int)m_Tracks[step.Track].Nodes.size());
  return true;
}

void TrackPlayer::PlaySequence()
{
  if (m_IsPlaying)
  {
    Stop();
    return;
  }
  size_t pos = 0;
  while (pos < m_Sequence.size() && (m_Sequence[pos] < 0 || m_Sequence[pos] >= (int)m_Tracks.size() ||
      m_Tracks[m_Sequence[pos]].Nodes.size() < 2))
    ++pos;
  if (pos >= m_Sequence.size())
  {
    util::log::Warning("The shot sequence has no track with 2 keyframes or more");
    return;
  }
  m_SequencePos = pos;
  m_SelectedTrack = m_Sequence[pos];
  m_SequencePlaying = true;
  m_SequenceOffset = 0.0;
  m_Cut = false;
  m_IsPlaying = true;
  m_IsPaused = false;
  m_CurrentTime = 0;
  m_CurrentNode = 0;
  util::log::Write("Playing the shot sequence (%d shots), shot %d: %s", (int)m_Sequence.size(), (int)pos + 1,
    m_Tracks[m_SelectedTrack].Name.c_str());
}

bool TrackPlayer::NextShot(double endTime)
{
  double over = m_CurrentTime - endTime;
  for (size_t pos = m_SequencePos + 1; pos < m_Sequence.size(); ++pos)
  {
    int track = m_Sequence[pos];
    if (track < 0 || track >= (int)m_Tracks.size() || m_Tracks[track].Nodes.size() < 2)
      continue;
    m_SequenceOffset += endTime;
    m_SequencePos = pos;
    m_SelectedTrack = track;
    m_CurrentTime = over > 0.0 ? over : 0.0;
    m_CurrentNode = 0;
    m_Cut = true;
    util::log::Write("Shot sequence: cut to shot %d, %s", (int)pos + 1, m_Tracks[track].Name.c_str());
    return true;
  }
  m_SequencePlaying = false;
  return false;
}

bool TrackPlayer::TakeCut()
{
  bool cut = m_Cut;
  m_Cut = false;
  return cut;
}

void TrackPlayer::Toggle()
{
  if (m_IsPlaying)
  {
    Stop();
    return;
  }
  if (m_SequenceOnKey && !m_Sequence.empty())
  {
    PlaySequence();
    return;
  }

  std::vector<CatmullRomNode>& nodes = m_Tracks[m_SelectedTrack].Nodes;
  if (nodes.size() <= 1)
  {
    util::log::Warning("Can't play a camera track with less than 2 nodes");
    return;
  }

  m_IsPlaying = true;
  m_IsPaused = false;
  m_SequencePlaying = false;
  m_CurrentTime = 0;
  m_CurrentNode = 0;
  util::log::Write("Playing %s, %.2f seconds", m_Tracks[m_SelectedTrack].Name.c_str(), nodes.back().TimeStamp / m_PlaybackSpeed);
}

void TrackPlayer::Stop()
{
  if (!m_IsPlaying) return;

  m_IsPlaying = false;
  m_IsPaused = false;
  m_SequencePlaying = false;
  util::log::Write("Track stopped");
}

void TrackPlayer::TogglePause()
{
  if (!m_IsPlaying) return;

  m_IsPaused = !m_IsPaused;
  util::log::Write("Track %s at %.2f s", m_IsPaused ? "paused" : "resumed", m_CurrentTime);
}

void TrackPlayer::Seek(double time)
{
  std::vector<CatmullRomNode>& nodes = m_Tracks[m_SelectedTrack].Nodes;
  if (nodes.size() < 2)
    return;
  double endTime = nodes.back().TimeStamp;
  m_CurrentTime = time < 0 ? 0 : time > endTime ? endTime : time;
  if (!m_IsPlaying)
  {
    // PlayForward finds the segment from the time, from either side
    m_IsPlaying = true;
    m_IsPaused = true;
    m_SequencePlaying = false;
    m_CurrentNode = 0;
    util::log::Write("Playing %s from %.2f s, paused", m_Tracks[m_SelectedTrack].Name.c_str(), m_CurrentTime);
  }
  m_Sought = true;
  m_SoughtTime = m_CurrentTime + (m_SequencePlaying ? m_SequenceOffset : 0.0);
  m_SoughtTrack = m_SelectedTrack;
  m_SoughtLocal = m_CurrentTime;
  m_SoughtSequence = m_SequencePlaying;
  m_SoughtSequencePos = m_SequencePos;
}

bool TrackPlayer::TakeSeek(double& time, double& gameTime, float sliderSpeed)
{
  if (!m_Sought)
    return false;
  m_Sought = false;
  time = m_SoughtTime;
  // A sequence's earlier shots (the ones it plays: 2 keyframes or more), then this one
  gameTime = 0.0;
  if (m_SoughtSequence)
    for (size_t pos = 0; pos < m_SoughtSequencePos && pos < m_Sequence.size(); ++pos)
    {
      int track = m_Sequence[pos];
      if (track >= 0 && track < (int)m_Tracks.size() && m_Tracks[track].Nodes.size() >= 2)
        gameTime += GameTimeAt(track, m_Tracks[track].Nodes.back().TimeStamp, sliderSpeed);
    }
  gameTime += GameTimeAt(m_SoughtTrack, m_SoughtLocal, sliderSpeed);
  return true;
}

// The game speed summed over the track's time (240 steps a second, at most
// 4000), as the camera manager uses it while the track plays (see
// CameraManager::GameSpeedNow)
double TrackPlayer::GameTimeAt(int track, double time, float sliderSpeed)
{
  if (time <= 0.0)
    return 0.0;
  if (!m_LockGameSpeed || track < 0 || track >= (int)m_Tracks.size() || m_Tracks[track].Nodes.size() < 2)
    return time * sliderSpeed;
  std::vector<CatmullRomNode> const& nodes = m_Tracks[track].Nodes;
  int steps = (int)std::ceil(time * 240.0);
  steps = steps < 1 ? 1 : (steps > 4000 ? 4000 : steps);
  double step = time / steps, sum = 0.0;
  for (int i = 0; i < steps; ++i)
    sum += NodesGameSpeedAt(nodes, (i + 0.5) * step, sliderSpeed);
  return sum * step;
}

// The track at time (seconds of track time) between n1 and n2 (n0 and n3
// their neighbours, or the ends standing in for them), as it plays
static CatmullRomNode InterpolateNodes(CatmullRomNode const& n0, CatmullRomNode const& n1, CatmullRomNode const& n2,
  CatmullRomNode const& n3, double time)
{
  float mu = (float)((time - n1.TimeStamp) / (n2.TimeStamp - n1.TimeStamp));

  CatmullRomNode resultNode;
  resultNode.TimeStamp = (float)time;

  // Time of day, unwrapped around n1 so it moves the way the clock runs
  // (u2 can be past 86400 or below 0). Only between two recorded times.
  bool hasTimeOfDay = n1.TimeOfDay >= 0 && n2.TimeOfDay >= 0;
  float u1 = n1.TimeOfDay;
  float u2 = hasTimeOfDay ? u1 + ClockStep(n1.TimeOfDay, n2.TimeOfDay, n1.TimeOfDayBackward) : u1;
  float u0 = (hasTimeOfDay && &n0 != &n1 && n0.TimeOfDay >= 0) ? u1 - ClockStep(n0.TimeOfDay, n1.TimeOfDay, n0.TimeOfDayBackward) : u1;
  float u3 = (hasTimeOfDay && &n3 != &n2 && n3.TimeOfDay >= 0) ? u2 + ClockStep(n2.TimeOfDay, n3.TimeOfDay, n2.TimeOfDayBackward) : u2;
  resultNode.TimeOfDay = hasTimeOfDay ? n1.TimeOfDay : -1.f;
  // Between two orbit keyframes: where round the focus (see CatmullRomNode::Orbit)
  InterpolateOrbit(n0, n1, n2, n3, mu, resultNode);
  resultNode.Shake = InterpolateShake(n0, n1, n2, n3, mu);
  resultNode.GameSpeed = InterpolateGameSpeed(n0, n1, n2, n3, mu);

  if (n1.Interpolation == Interpolation_Hold)
  {
    resultNode.Position = n1.Position;
    resultNode.Rotation = n1.Rotation;
    resultNode.FieldOfView = n1.FieldOfView;
    resultNode.MotionBlur = n1.MotionBlur;
    return resultNode;
  }

  XMVECTOR qRot1 = XMLoadFloat4(&n1.Rotation);
  XMVECTOR qRot2 = XMLoadFloat4(&n2.Rotation);
  XMVECTOR vPos1 = XMLoadFloat3(&n1.Position);
  XMVECTOR vPos2 = XMLoadFloat3(&n2.Position);

  if (n1.Interpolation == Interpolation_Linear)
  {
    XMStoreFloat3(&resultNode.Position, XMVectorLerp(vPos1, vPos2, mu));
    XMStoreFloat4(&resultNode.Rotation, XMQuaternionSlerp(qRot1, qRot2, mu));
    resultNode.FieldOfView = n1.FieldOfView + (n2.FieldOfView - n1.FieldOfView) * mu;
    resultNode.MotionBlur = n1.MotionBlur + (n2.MotionBlur - n1.MotionBlur) * mu;
    if (hasTimeOfDay)
      resultNode.TimeOfDay = WrapTimeOfDay(u1 + (u2 - u1) * mu);
    return resultNode;
  }

  // Smooth, optionally with the timing eased in and out of the keyframes
  float s = (n1.Interpolation == Interpolation_EaseInOut) ? BezierEaseInOut(mu) : mu;

  XMVECTOR qRot0 = XMLoadFloat4(&n0.Rotation);
  XMVECTOR qRot3 = XMLoadFloat4(&n3.Rotation);

  float t0 = n0.TimeStamp, t1 = n1.TimeStamp, t2 = n2.TimeStamp, t3 = n3.TimeStamp;

  // The place: the automatic curve, or the one through handles set by hand
  XMVECTOR resultPos = CurvePosition(n0, n1, n2, n3, s);
  XMVECTOR resultRot = ClampedHermite(qRot0, qRot1, qRot2, qRot3, t0, t1, t2, t3, s);
  XMVECTOR resultFov = ClampedHermite(XMVectorReplicate(n0.FieldOfView), XMVectorReplicate(n1.FieldOfView),
    XMVectorReplicate(n2.FieldOfView), XMVectorReplicate(n3.FieldOfView), t0, t1, t2, t3, s);
  XMVECTOR resultBlur = ClampedHermite(XMVectorReplicate(n0.MotionBlur), XMVectorReplicate(n1.MotionBlur),
    XMVectorReplicate(n2.MotionBlur), XMVectorReplicate(n3.MotionBlur), t0, t1, t2, t3, s);

  XMStoreFloat3(&resultNode.Position, resultPos);
  XMStoreFloat4(&resultNode.Rotation, XMQuaternionNormalize(resultRot));
  resultNode.FieldOfView = XMVectorGetX(resultFov);
  resultNode.MotionBlur = XMVectorGetX(resultBlur);

  if (hasTimeOfDay)
  {
    XMVECTOR resultTime = ClampedHermite(XMVectorReplicate(u0), XMVectorReplicate(u1),
      XMVectorReplicate(u2), XMVectorReplicate(u3), t0, t1, t2, t3, s);
    resultNode.TimeOfDay = WrapTimeOfDay(XMVectorGetX(resultTime));
  }

  return resultNode;
}

// The track at time (seconds of track time), in the segment that starts at
// nodes[segment], as it plays. With a focus (liveFocus), orbit keyframes
// are round it where it is now (see LiveSegment), and a move between an
// orbit keyframe and one that isn't goes round it too: from the orbit
// keyframe's place to the other's own, turning from the orbit keyframe's
// view there (OrbitViewAt) to the other's own. Between two orbit keyframes, out.Orbit says to
// place it round the focus (CameraManager::UpdateCamera does).
static CatmullRomNode InterpolateSegment(std::vector<CatmullRomNode> const& nodes, unsigned int segment, double time,
  XMFLOAT3 const* liveFocus, float liveBaseYaw)
{
  CatmullRomNode const* p1 = &nodes[segment];
  CatmullRomNode const* p2 = &nodes[segment + 1];
  CatmullRomNode const* p0 = segment > 0 ? &nodes[segment - 1] : p1;
  CatmullRomNode const* p3 = segment + 2 < nodes.size() ? &nodes[segment + 2] : p2;
  CatmullRomNode live[4];
  bool placed = LiveSegment(p0, p1, p2, p3, live, liveFocus, liveBaseYaw);
  CatmullRomNode result = InterpolateNodes(*p0, *p1, *p2, *p3, time);

  bool orbit1 = Placeable(nodes[segment], liveFocus), orbit2 = Placeable(nodes[segment + 1], liveFocus);
  if (placed && orbit1 != orbit2)
  {
    // A Hold stays on its first keyframe as it is (placed or its own)
    if (p1->Interpolation != Interpolation_Hold && result.Orbit && result.OrbitFocusSet)
    {
      float mu = (float)((time - p1->TimeStamp) / (p2->TimeStamp - p1->TimeStamp));
      float s = p1->Interpolation == Interpolation_EaseInOut ? BezierEaseInOut(mu) : mu;
      // (round the orbit keyframe's point: the other end was taken round it, MakePolar)
      XMVECTOR direction = OrbitDirection(result.OrbitYaw, result.OrbitPitch);
      XMStoreFloat3(&result.Position, XMLoadFloat3(&result.OrbitFocus) + direction * result.OrbitDistance);
      // From the orbit keyframe's view to the other's, gently at both ends
      float toOwn = s * s * (3.f - 2.f * s);
      if (orbit2)
        toOwn = 1.f - toOwn;
      XMVECTOR own = XMLoadFloat4(&(orbit1 ? p2 : p1)->Rotation);
      XMVECTOR orbitView = OrbitViewAt(*(orbit1 ? p1 : p2), direction);
      XMStoreFloat4(&result.Rotation, XMQuaternionNormalize(XMQuaternionSlerp(orbitView, own, toOwn)));
    }
    result.Orbit = false;   // placed here
  }
  return result;
}

CatmullRomNode TrackPlayer::PlayForward(double dt, bool ignoreManual /* = false */)
{
  std::vector<CatmullRomNode>& nodes = m_Tracks[m_SelectedTrack].Nodes;
  if (nodes.size() < 2)
  {
    m_IsPlaying = false;
    m_IsPaused = false;
    return nodes.empty() ? CatmullRomNode() : nodes[0];
  }

  // If manual play is enabled, time is moved with the Up/Down
  // actions (triggers on a gamepad). IgnoreManual is for previews.
  // Paused, it stays where it is and the camera holds on that frame.
  if (!m_IsPaused)
  {
    if (!m_ManualPlay || ignoreManual)
      m_CurrentTime += dt * m_PlaybackSpeed;
    else
    {
      InputSystem* pInput = g_mainHandle->GetInputSystem();
      float input = pInput->GetActionState(Action::Camera_Up) - pInput->GetActionState(Action::Camera_Down);
      // Quadratic response that keeps the direction, so it can go backwards too
      m_CurrentTime += dt * input * std::fabs(input) * m_PlaybackSpeed;
    }
  }

  // Clamp to the ends of the track
  double endTime = nodes.back().TimeStamp;
  // At the ends, an orbit keyframe is round its point (or, without one, the
  // focus where it is now while there's one), as the segment from it starts
  // (see LiveSegment)
  XMFLOAT3 const* liveFocus = m_LiveFocusOk ? &m_LiveFocus : nullptr;
  if (m_CurrentTime <= 0)
  {
    m_CurrentTime = 0;
    m_CurrentNode = 0;
    CatmullRomNode front = nodes.front();
    front.Orbit = Placeable(front, liveFocus);
    if (front.Orbit)
      NormalizeOrbit(front, liveFocus, m_LiveBaseYaw);
    return front;
  }

  if (m_CurrentTime >= endTime)
  {
    if (m_SequencePlaying && !m_ManualPlay && !m_IsPaused && NextShot(endTime))
      return PlayForward(0.0, true);
    m_CurrentTime = endTime;
    m_CurrentNode = (unsigned int)nodes.size() - 2;
    // Paused (sent to the end with the slider) it holds the last frame
    if (!m_ManualPlay && !m_IsPaused)
    {
      m_IsPlaying = false;
      m_IsPaused = false;
      util::log::Write("Track finished");
    }
    CatmullRomNode back = nodes.back();
    back.Orbit = Placeable(back, liveFocus);
    if (back.Orbit)
      NormalizeOrbit(back, liveFocus, m_LiveBaseYaw);
    return back;
  }

  // Move to the segment that contains the current time
  unsigned int lastSegment = (unsigned int)nodes.size() - 2;
  if (m_CurrentNode > lastSegment)
    m_CurrentNode = lastSegment;

  while (m_CurrentNode < lastSegment && m_CurrentTime >= nodes[m_CurrentNode + 1].TimeStamp)
    m_CurrentNode++;

  while (m_CurrentNode > 0 && m_CurrentTime < nodes[m_CurrentNode].TimeStamp)
    m_CurrentNode--;

  return InterpolateSegment(nodes, m_CurrentNode, m_CurrentTime, m_LiveFocusOk ? &m_LiveFocus : nullptr, m_LiveBaseYaw);
}

void TrackPlayer::DrawUI(Camera& camera, bool cameraEnabled)
{
  InputSystem* pInput = g_mainHandle->GetInputSystem();
  float fontSize = ImGui::GetFontSize();
  char label[256];
  // Changes made outside the menu since (the hotkeys) are their own steps already
  if (m_ShadowTrack != m_SelectedTrack)
    SyncShadow();

  // Track picker, or the name box while renaming
  static bool s_focusRename = false;
  ImGui::PushItemWidth(fontSize * 10.f);
  if (m_RenamingTrack == m_SelectedTrack)
  {
    if (s_focusRename)
    {
      ImGui::SetKeyboardFocusHere();
      s_focusRename = false;
    }
    bool done = ImGui::InputText("##TrackName", m_RenameBuffer, sizeof(m_RenameBuffer),
      ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
    ImGui::SameLine();
    done |= ImGui::Button("Done");
    if (done)
    {
      if (m_RenameBuffer[0])
        m_Tracks[m_SelectedTrack].Name = m_RenameBuffer;
      m_RenamingTrack = -1;
      UpdateNameList();
    }
  }
  else
  {
    m_RenamingTrack = -1;
    if (ImGui::Combo("##CameraTrackList", &m_SelectedTrack, m_TrackNames.data(), (int)m_TrackNames.size()))
    {
      Stop();
      m_CurrentTime = 0;
      m_CurrentNode = 0;
    }
    ImGui::SameLine();
    if (ImGui::Button("Rename") && !m_IsPlaying)
    {
      strcpy_s(m_RenameBuffer, m_Tracks[m_SelectedTrack].Name.c_str());
      m_RenamingTrack = m_SelectedTrack;
      s_focusRename = true;
    }
  }
  ImGui::PopItemWidth();
  ImGui::SameLine();
  if (ImGui::Button("New track"))
    CreateTrack();
  if (m_Tracks.size() > 1)
  {
    ImGui::SameLine();
    if (ImGui::ConfirmButton("Delete track"))
      DeleteTrack();
  }

  std::vector<CatmullRomNode>& nodes = m_Tracks[m_SelectedTrack].Nodes;
  float trackLength = nodes.empty() ? 0.f : nodes.back().TimeStamp;

  if (IsSequencePlaying())
    ImGui::TextColored(ImVec4(0.45f, 0.9f, 0.45f, 1.f), "Shot sequence: shot %d of %d", (int)m_SequencePos + 1,
      (int)m_Sequence.size());

  // Play / stop and where the track is
  if (cameraEnabled && nodes.size() >= 2)
  {
    sprintf_s(label, "%s  (%s)###PlayTrack", m_IsPlaying ? "Stop" : "Play", pInput->GetKeyName(Action::Track_Play).c_str());
    if (ImGui::Button(label, ImVec2(fontSize * 8.f, 0)))
      Toggle();
    ImGui::SameLine();
    // Pause / resume, always there so the row doesn't move, faint until a track plays
    bool canPause = m_IsPlaying;
    if (!canPause)
      ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * 0.4f);
    if (ImGui::Button(IsPaused() ? "Resume###PauseTrack" : "Pause###PauseTrack", ImVec2(fontSize * 5.f, 0)) && canPause)
      TogglePause();
    if (!canPause)
      ImGui::PopStyleVar();
    if (canPause)
      ImGui::ItemTooltip(IsPaused() ? "Carries on playing from here."
        : "Holds the track where it is: the camera stays on this frame until Resume (Stop ends the track).");
    ImGui::SameLine();
    // Where the track is; dragged, it goes there
    float shownTime = (float)(m_IsPlaying ? m_CurrentTime : 0.0);
    char format[64];
    sprintf_s(format, "%%.2f / %.2f s%s", trackLength, IsPaused() ? "  (paused)" : "");
    ImGui::PushItemWidth(-1);
    if (ImGui::SliderFloat("##TrackPosition", &shownTime, 0.f, trackLength, format))
      Seek(shownTime);
    ImGui::PopItemWidth();
    ImGui::ItemTooltip("Drag to go to any moment of the track. A stopped track starts there, paused: Resume "
      "plays on from there. A replay playing along with the track (REPLAY) goes to the same moment.");
  }

  ImGui::SliderFloat("Playback speed", &m_PlaybackSpeed, 0.1f, 4.f, "%.2fx");
  // A value typed in (Ctrl+click) isn't held to the slider's range: at 0 or
  // below the track would never move on, or run backwards and never end
  if (!(m_PlaybackSpeed >= 0.1f))
    m_PlaybackSpeed = 0.1f;
  ImGui::HelpMarker("1x plays the keyframes at their own times. At 0.5x the track takes twice as long.");

  // Adding and removing keyframes
  if (!cameraEnabled)
  {
    ImGui::PushTextWrapPos(0.f);
    ImGui::TextDisabled("Turn the camera on to add keyframes and play tracks.");
    ImGui::PopTextWrapPos();
  }
  else if (!m_IsPlaying)
  {
    sprintf_s(label, "Add keyframe  (%s)###AddKeyframe", pInput->GetKeyName(Action::Track_CreateNode).c_str());
    if (ImGui::Button(label))
      CreateNode(camera);
    ImGui::SameLine();
    sprintf_s(label, "Delete last  (%s)###DeleteKeyframe", pInput->GetKeyName(Action::Track_DeleteNode).c_str());
    if (ImGui::Button(label))
      DeleteNode();
    ImGui::SameLine();
    sprintf_s(label, "Undo (%d)###UndoKeyframes", (int)m_Undo.size());
    if (!m_Undo.empty() && ImGui::Button(label))
      Undo();
    else if (m_Undo.empty())
      ImGui::TextDisabled("Undo (0)");
    ImGui::SameLine();
    sprintf_s(label, "Redo (%d)###RedoKeyframes", (int)m_Redo.size());
    if (!m_Redo.empty() && ImGui::Button(label))
      Redo();
    else if (m_Redo.empty())
      ImGui::TextDisabled("Redo (0)");
    sprintf_s(label, "Undoes or redoes a change to the keyframes: adding, deleting, moving or editing them (%s / %s, up "
      "to 100 steps).", pInput->GetKeyName(Action::Track_Undo).c_str(), pInput->GetKeyName(Action::Track_Redo).c_str());
    ImGui::HelpMarker(label);
    if (!nodes.empty())
    {
      ImGui::SameLine();
      if (ImGui::ConfirmButton("Clear all"))
        nodes.clear();
    }

    ImGui::PushTextWrapPos(0.f);
    ImGui::TextDisabled(nodes.size() < 2
      ? "Fly to the first shot and add a keyframe, then fly to the next shot and add another. "
        "With this menu open, hold the right mouse button outside the window to look around."
      : "Expand a keyframe to change its position, angles, timing or how it moves to the next one. "
        "The arrows on a keyframe move it earlier or later in the track.");
    ImGui::PopTextWrapPos();
  }

  // The keyframes, as a red bar like the menu tabs so the list is easy to
  // find, with one tinted row per keyframe. The one playing is brighter.
  int deleteIndex = -1;
  int moveFrom = -1, moveTo = -1;
  int insertAfter = -1;
  CatmullRomNode insertNode;
  const ImVec4 red = ImGui::GetStyleColorVec4(ImGuiCol_Button);
  const ImVec4 redLight = ImGui::GetStyleColorVec4(ImGuiCol_ButtonHovered);
  const ImVec4 redBright = ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive);
  auto withAlpha = [](ImVec4 color, float alpha) { color.w = alpha; return color; };

  ImGui::Dummy(ImVec2(0, ImGui::GetStyle().ItemSpacing.y));
  sprintf_s(label, "KEYFRAMES    %d###TrackKeyframes", (int)nodes.size());
  ImGui::PushStyleColor(ImGuiCol_Header, red);
  ImGui::PushStyleColor(ImGuiCol_HeaderHovered, redLight);
  ImGui::PushStyleColor(ImGuiCol_HeaderActive, redBright);
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(ImGui::GetStyle().FramePadding.x, ImGui::GetStyle().FramePadding.y * 1.8f));
  bool showKeyframes = ImGui::TreeNodeEx(label, ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_Framed);
  ImGui::PopStyleVar();
  ImGui::PopStyleColor(3);

  if (showKeyframes && nodes.empty())
  {
    ImGui::PushTextWrapPos(0.f);
    ImGui::TextDisabled("No keyframes yet. Turn the camera on, fly to the first shot and press %s (or Add keyframe).",
      pInput->GetKeyName(Action::Track_CreateNode).c_str());
    ImGui::PopTextWrapPos();
  }

  for (size_t i = 0; showKeyframes && i < nodes.size(); ++i)
  {
    CatmullRomNode& node = nodes[i];

    ImGui::PushID((int)i);
    const char* interpolationName = (i + 1 < nodes.size() && node.Interpolation >= 0 && node.Interpolation < Interpolation_Count)
      ? g_interpolationNames[node.Interpolation] : "last keyframe";
    bool sameView = i > 0 && IsSameView(nodes[i - 1], node);
    bool playingThis = m_IsPlaying && i == m_CurrentNode;
    // Picked on the drawn path: brighter, and opened once so its curve's handles are here
    bool pickedThis = (int)i == GetPickedKeyframe();
    if (pickedThis && m_OpenPicked == (int)i)
      ImGui::SetNextTreeNodeOpen(true);
    if (m_OpenPicked == (int)i)
      m_OpenPicked = -1;
    char timeOfDay[16] = "";
    if (m_LockTimeOfDay && node.TimeOfDay >= 0)
    {
      timeOfDay[0] = ' '; timeOfDay[1] = ' '; timeOfDay[2] = ' '; timeOfDay[3] = ' ';
      FormatTimeOfDay(node.TimeOfDay, timeOfDay + 4, sizeof(timeOfDay) - 4);
    }
    ImGui::PushStyleColor(ImGuiCol_Header, playingThis ? withAlpha(redBright, 0.9f) :
      (pickedThis ? withAlpha(redLight, 0.7f) : withAlpha(red, 0.35f)));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, withAlpha(redLight, playingThis ? 1.f : 0.6f));
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, redBright);
    bool open = ImGui::TreeNodeEx("##Keyframe", ImGuiTreeNodeFlags_Framed | ImGuiTreeNodeFlags_AllowItemOverlap,
      "%s%d    %.2f s    %s%s%s%s", playingThis ? "> Keyframe " : "Keyframe ", (int)i + 1,
      node.TimeStamp, interpolationName, timeOfDay, sameView ? "    (same view as the one before)" : "",
      pickedThis ? "    (picked on the path)" : "");
    ImGui::PopStyleColor(3);

    // Move up / down, at the right end of the row. Both are always there (faint
    // when there's nowhere to move) so they line up from row to row.
    if (!m_IsPlaying && nodes.size() > 1)
    {
      float arrowSize = ImGui::GetFrameHeight();
      ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - arrowSize * 2.f);
      if (ImGui::MoveArrowButton("##Up", ImGuiDir_Up, i > 0))
      {
        moveFrom = (int)i;
        moveTo = (int)i - 1;
      }
      if (i > 0)
        ImGui::ItemTooltip("Move this keyframe earlier (its time stays)");
      ImGui::SameLine(0.f, 0.f);
      if (ImGui::MoveArrowButton("##Down", ImGuiDir_Down, i + 1 < nodes.size()))
      {
        moveFrom = (int)i;
        moveTo = (int)i + 1;
      }
      if (i + 1 < nodes.size())
        ImGui::ItemTooltip("Move this keyframe later (its time stays)");
    }

    if (open)
    {
      if (m_IsPlaying)
        ImGui::TextDisabled("Stop the track to edit keyframes");
      else
      {
        ImGui::PushItemWidth(fontSize * 15.f);

        // Going round the focus. Any keyframe can be ticked: its place round
        // the focus and the ways round to the orbit keyframes beside it are
        // worked out on the game thread (CameraManager::ProcessOrbitRequests)
        {
          bool canTick = cameraEnabled && m_OrbitFocusKnown;
          bool ticked = node.Orbit;
          if (!canTick)
            ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * 0.4f);
          if (ImGui::Checkbox("Orbit keyframe", &ticked) && canTick)
          {
            node.Orbit = ticked;
            // Ticked: worked out again from where it is now (a place kept from
            // an earlier tick, or from when it was made, would move it)
            if (ticked)
              node.OrbitDistance = 0.f;
          }
          if (!canTick)
            ImGui::PopStyleVar();
          ImGui::HelpMarker("An orbit keyframe goes round the point where the focus (what the camera follows, else "
            "Faith) was when it was ticked: the moves to and from it go round that point too, instead of along a "
            "line, and to a keyframe that isn't one the camera ends at that keyframe's own place and view. It stays "
            "there when Faith moves on, on the drawn path and when the track plays. For a shot that follows her, use "
            "FOLLOW: a track can move with what's followed.\n\n"
            "Any keyframe can be ticked: where round the focus it is, and its view, come from where it is now, so "
            "ticking doesn't move or turn it. The way round to the keyframes next to it is the way your track already "
            "goes round that point, more than half a turn or whole turns if it does. That arc is checked for walls: "
            "if something's in the way, it rises a little in the middle, or goes the other way round. Its dot on the "
            "drawn path can be dragged round the point, and each arc has a diamond to raise or widen it.\n\nUntick to "
            "use this keyframe's position and rotation, and ordinary moves, again; tick again to go round the focus "
            "where it is then.");
          if (!canTick && !node.Orbit)
            ImGui::TextDisabled(cameraEnabled ? "No focus to go round: Faith, or what the camera follows (FOLLOW)."
              : "Turn the camera on to tick it.");
          if (IsOrbitKeyframe(node))
          {
            ImGui::TextDisabled("%.0f" UI_DEG " round, %.0f" UI_DEG " up, %.1f m from its point", XMConvertToDegrees(node.OrbitYaw),
              XMConvertToDegrees(node.OrbitPitch), node.OrbitDistance);
            // Its own view, or looking at the focus as made while circling
            if (!IsPlainOrbitView(node.OrbitView))
            {
              if (ImGui::Button("Look at its point"))
                node.OrbitView = XMFLOAT4(0.f, 0.f, 0.f, 1.f);
              ImGui::ItemTooltip("This keyframe keeps the view it had when it was ticked, turning with it round its "
                "point. This turns it to look straight at that point instead (as a keyframe made while circling does).");
            }
          }
          else if (node.Orbit)
          {
            ImGui::TextDisabled("Working out where round the focus it is...");
            // Once for it (again if the last try was dropped: the track played, another was picked)
            bool queued = false;
            for (OrbitRequest const& request : m_OrbitRequests)
              queued |= request.Track == m_SelectedTrack && request.Keyframe == (int)i && request.Ticked;
            if (!queued && canTick)
              m_OrbitRequests.push_back({ m_SelectedTrack, (int)i, true });
          }
          // The way round to the next, as it plays, when that move goes round
          // a point (this keyframe or the next is an orbit keyframe: round its
          // own point, else round the focus where it is)
          if (IsOrbitSegment((int)i) && (m_LiveFocusOk || Placeable(node, nullptr) || Placeable(nodes[i + 1], nullptr)))
          {
            char way[320];
            int turns = node.OrbitWay != 0 && node.OrbitTurns > 0 ? node.OrbitTurns : 0;
            char turnsText[48] = "", liftText[64] = "", bulgeText[96] = "";
            if (turns > 0)
              sprintf_s(turnsText, ", %d whole turn%s more", turns, turns > 1 ? "s" : "");
            if (node.OrbitLift != 0.f)
              sprintf_s(liftText, ", %s %.0f" UI_DEG " in the middle", node.OrbitLift > 0.f ? "rising" : "dipping",
                fabsf(XMConvertToDegrees(node.OrbitLift)));
            if (node.OrbitBulge != 0.f)
              sprintf_s(bulgeText, ", %.1f m %s its point in the middle", fabsf(node.OrbitBulge),
                node.OrbitBulge > 0.f ? "further from" : "nearer");
            float step = OrbitStepWith((int)i, node.OrbitWay, node.OrbitTurns, m_LiveFocusOk ? &m_LiveFocus : nullptr,
              m_LiveBaseYaw);
            sprintf_s(way, "To keyframe %d, round its point: %s, %.0f" UI_DEG " round%s%s%s", (int)i + 2,
              node.OrbitWay == 0 ? "the shorter way" : (node.OrbitWay > 0 ? "round to the right" : "round to the left"),
              fabsf(XMConvertToDegrees(step)), turnsText, liftText, bulgeText);
            ImGui::PushTextWrapPos(0.f);
            ImGui::TextDisabled("%s", way);
            ImGui::PopTextWrapPos();
            if (cameraEnabled && m_OrbitFocusKnown)
            {
              if (ImGui::Button("Find the best way round"))
                m_OrbitRequests.push_back({ m_SelectedTrack, (int)i, false });
              ImGui::ItemTooltip("Looks again for the way round to the next keyframe, round the point the move goes "
                "round (where the focus was when they were ticked): the way the track goes round it, checked for "
                "walls (rising a little, or the other way round, when something's in the way). Right and left are as "
                "the camera sees them, looking at that point. One undo step.");
            }
            // Its arch as dragged on the drawn path, back to the plain arc
            if (node.OrbitLift != 0.f || node.OrbitBulge != 0.f)
            {
              if (cameraEnabled && m_OrbitFocusKnown)
                ImGui::SameLine();
              if (ImGui::Button("Flatten the arch"))
              {
                node.OrbitLift = 0.f;
                node.OrbitBulge = 0.f;
              }
              ImGui::ItemTooltip("The arc to the next keyframe without its rise or dip and without going further out "
                "or nearer in the middle: the plain arc between the two keyframes' places round their point.");
            }
          }
          // What the last search found here
          if (m_OrbitNotesTrack == m_SelectedTrack && m_OrbitNotesCount == (int)nodes.size())
          {
            auto note = m_OrbitNotes.find((int)i);
            if (note != m_OrbitNotes.end() && !note->second.empty())
            {
              ImGui::PushTextWrapPos(0.f);
              ImGui::TextDisabled("%s", note->second.c_str());
              ImGui::PopTextWrapPos();
            }
          }
        }

        // While one of these is being changed the camera shows the keyframe
        // live (with the preview on the path, the preview does: the camera stays)
        bool preview = false;
        preview |= ImGui::DragFloat3("Position", &node.Position.x, 0.05f, 0, 0, "%.2f");
        preview |= ImGui::IsItemActive();

        float angles[3];
        QuaternionToAngles(node.Rotation, angles);
        if (ImGui::DragFloat3("Pitch / yaw / roll", angles, 0.25f, 0, 0, "%.1f" UI_DEG))
        {
          node.Rotation = AnglesToQuaternion(angles);
          FixRotationSigns(nodes);
          preview = true;
        }
        preview |= ImGui::IsItemActive();

        // Values typed in (Ctrl+click) aren't held to the ranges, so they are here
        preview |= ImGui::DragFloat("Field of view", &node.FieldOfView, 0.1f, 5.f, 150.f, "%.1f" UI_DEG);
        preview |= ImGui::IsItemActive();
        if (!(node.FieldOfView >= 5.f)) node.FieldOfView = 5.f;
        if (node.FieldOfView > 150.f) node.FieldOfView = 150.f;
        if (node.Shake >= 0.f)
        {
          ImGui::SliderFloat("Hand-held shake", &node.Shake, 0.f, 1.f, "%.2f");
          if (!(node.Shake >= 0.f)) node.Shake = 0.f;
          if (node.Shake > 1.f) node.Shake = 1.f;
          ImGui::HelpMarker("The shake's strength at this keyframe, used while the track sets the shake (Keyframe "
            "options). The shake's kind and speed are in CAMERA > Camera shake.");
        }
        if (node.GameSpeed > 0.f)
        {
          bool speedChanged = ImGui::SliderFloat("Game speed##Keyframe", &node.GameSpeed, GameSpeedMin, GameSpeedMax,
            "%.2fx", 2.f);
          if (!(node.GameSpeed >= GameSpeedMin)) node.GameSpeed = GameSpeedMin;
          if (node.GameSpeed > GameSpeedMax) node.GameSpeed = GameSpeedMax;
          ImGui::HelpMarker("The game's speed at this keyframe, used while the track sets the game speed (Keyframe "
            "options): 1x is normal, below is slow motion. Changing it here ticks that by itself.");
          // Changing a keyframe's game speed means the track should use it, so
          // the game speed channel goes on by itself, as the time of day's does
          // (2026-11-06, the user: "Check if game speed keyframes actually
          // work": off, every keyframe's speed was left out without a word)
          if (speedChanged && !m_LockGameSpeed)
          {
            m_LockGameSpeed = true;
            g_mainHandle->OnConfigChanged();
            util::log::Write("The track now sets the game speed");
          }
          if (!m_LockGameSpeed)
            ImGui::TextDisabled("Not used now: Keyframe options > Game speed is off.");
        }
        ImGui::DragFloat("Motion blur", &node.MotionBlur, 0.01f, 0.f, 10.f, "%.2f");
        if (!(node.MotionBlur >= 0.f)) node.MotionBlur = 0.f;
        if (node.MotionBlur > 10.f) node.MotionBlur = 10.f;
        ImGui::HelpMarker("Motion blur strength at this keyframe. Used while \"Control motion blur\" is on "
          "and the track controls motion blur (Keyframe options).");

        if (node.TimeOfDay >= 0 && util::IsMecmpLoaded())
          ImGui::TextDisabled("Time of day: disabled while MECMP is open (it syncs the time of day)");
        else if (node.TimeOfDay >= 0)
        {
          char clock[16];
          FormatTimeOfDay(node.TimeOfDay, clock, sizeof(clock));
          bool timeChanged = ImGui::SliderFloat("Time of day", &node.TimeOfDay, 0.f, 86399.f, clock);
          bool timeActive = ImGui::IsItemActive();
          // Typed in, it can be anything: below 0 the keyframe would count as
          // having no time of day at all, so it's taken round the clock
          if (timeChanged)
            node.TimeOfDay = WrapTimeOfDay(node.TimeOfDay);
          ImGui::HelpMarker("The time of day at this keyframe. Ctrl+click to type it in seconds (0 - 86400).");

          // Changing a keyframe's time means the track should use it, so
          // the time of day channel goes on by itself
          if (timeChanged && !m_LockTimeOfDay)
          {
            m_LockTimeOfDay = true;
            util::log::Write("The track now sets the time of day");
          }
          // And the game shows it live while it's being changed
          if ((timeChanged || timeActive) && cameraEnabled)
            g_mainHandle->GetVisualsManager()->SetTimeOfDay(node.TimeOfDay);

          ImGui::Checkbox("Track sets the time of day", &m_LockTimeOfDay);
          ImGui::HelpMarker("The same as Time of day in Keyframe options. Off: the keyframes keep their "
            "times, but playing the track leaves the game's clock alone.");
          if (i + 1 < nodes.size())
          {
            ImGui::Checkbox("Clock runs backwards to the next", &node.TimeOfDayBackward);
            ImGui::HelpMarker("Normally the clock runs forward to the next keyframe's time, through midnight "
              "if it has to (06:00 to 05:00 is 23 hours). Tick this to turn it back instead.");
          }
        }
        else if (camera.TimeOfDay >= 0)
        {
          if (ImGui::Button("Use the current time of day"))
            node.TimeOfDay = camera.TimeOfDay;
          ImGui::ItemTooltip("This keyframe was added without a level loaded, so it has no time of day.");
        }

        // Edit the time to the next node, later nodes move with it
        if (i + 1 < nodes.size())
        {
          ImGui::Combo("Move to the next", &node.Interpolation, g_interpolationNames, Interpolation_Count);

          float currentDuration = nodes[i + 1].TimeStamp - node.TimeStamp;
          float duration = currentDuration;
          if (ImGui::InputFloat("Seconds to the next", &duration, 0.1f, 1.f, 2))
          {
            if (duration < g_minSegmentDuration)
              duration = g_minSegmentDuration;

            float delta = duration - currentDuration;
            for (size_t j = i + 1; j < nodes.size(); ++j)
              nodes[j].TimeStamp += delta;
          }
        }

        // The path's curve round it: automatic, or its handles as dragged on
        // the drawn path
        if (node.Handles)
        {
          if (i > 0)
            ImGui::DragFloat3("Handle in", &node.HandleIn.x, 0.05f, 0, 0, "%.2f");
          if (i + 1 < nodes.size())
            ImGui::DragFloat3("Handle out", &node.HandleOut.x, 0.05f, 0, 0, "%.2f");
          ImGui::HelpMarker("The curve's handles at this keyframe, in metres from its place: the way in from the "
            "keyframe before and the way out to the next. The path leaves along a handle and the longer it is, the "
            "further the path keeps going its way. They shape Smooth and Ease in/out moves; their timing stays the "
            "keyframes'.");
          ImGui::Checkbox("Handles move on their own", &node.HandlesFree);
          ImGui::HelpMarker("Off, dragging one handle on the path turns the other with it (in line, its length kept), "
            "so the path goes smoothly through the keyframe. On, each one moves alone, for a sharp turn here.");
          if (ImGui::Button("Automatic curve"))
          {
            node.Handles = false;
            node.HandlesFree = false;
            node.HandleIn = XMFLOAT3(0.f, 0.f, 0.f);
            node.HandleOut = XMFLOAT3(0.f, 0.f, 0.f);
          }
          ImGui::ItemTooltip("Forgets this keyframe's handles: the path round it goes back to the automatic curve.");
        }
        else
        {
          ImGui::PushTextWrapPos(0.f);
          ImGui::TextDisabled("Curve: automatic. To shape it, click this keyframe on the drawn path (CAMERA > Show "
            "the track's path) and drag its handles.");
          ImGui::PopTextWrapPos();
        }
        ImGui::PopItemWidth();

        if (preview && cameraEnabled && m_PreviewOnPath)
        {
          m_PreviewHeld = true;
          m_PreviewHeldTime = node.TimeStamp;
        }
        else if (preview && cameraEnabled)
        {
          camera.Position = node.Position;
          camera.Rotation = node.Rotation;
          camera.FieldOfView = node.FieldOfView;
        }

        if (cameraEnabled)
        {
          if (ImGui::Button("Go to"))
          {
            camera.Position = node.Position;
            camera.Rotation = node.Rotation;
            camera.FieldOfView = node.FieldOfView;
            camera.MotionBlur = node.MotionBlur;
            if (m_LockTimeOfDay && node.TimeOfDay >= 0)
            {
              camera.TimeOfDay = node.TimeOfDay;
              g_mainHandle->GetVisualsManager()->SetTimeOfDay(node.TimeOfDay);
            }
          }
          ImGui::ItemTooltip("Moves the camera to this keyframe (and the clock, when the track sets the time of day).");
          ImGui::SameLine();
          if (ImGui::Button("Set from camera"))
          {
            // Its time and how it moves on stay
            CatmullRomNode now = NodeFromCamera(camera);
            now.TimeStamp = node.TimeStamp;
            now.Interpolation = node.Interpolation;
            now.TimeOfDayBackward = node.TimeOfDayBackward;
            if (now.TimeOfDay < 0)
              now.TimeOfDay = node.TimeOfDay;
            now.Handles = node.Handles;
            now.HandlesFree = node.HandlesFree;
            now.HandleIn = node.HandleIn;
            now.HandleOut = node.HandleOut;
            now.OrbitWay = node.OrbitWay;
            now.OrbitTurns = node.OrbitTurns;
            now.OrbitLift = node.OrbitLift;
            now.OrbitBulge = node.OrbitBulge;
            node = now;
            FixRotationSigns(nodes);
          }
          ImGui::ItemTooltip("Replaces this keyframe with where the camera is now (orbiting: where it is round the "
            "focus too). Its time and its curve's handles stay.");
          ImGui::SameLine();
          if (ImGui::Button("Insert view after"))
          {
            insertAfter = (int)i;
            insertNode = NodeFromCamera(camera);
          }
          ImGui::ItemTooltip("A new keyframe with where the camera is now, halfway between this keyframe and the next "
            "(or after it, when it's the last).");
          ImGui::SameLine();
        }
        if (ImGui::Button("Duplicate"))
        {
          insertAfter = (int)i;
          insertNode = node;
        }
        ImGui::ItemTooltip("A copy of this keyframe right after it (halfway to the next, or after it when it's the last).");
        ImGui::SameLine();
        if (ImGui::Button("Delete"))
          deleteIndex = (int)i;
      }
      ImGui::TreePop();
    }
    ImGui::PopID();
  }
  if (showKeyframes)
    ImGui::TreePop();

  if (moveFrom >= 0 && moveTo >= 0 && moveTo < (int)nodes.size() && !m_IsPlaying)
  {
    // The shots change order, the timing doesn't: the two keyframes swap
    // everything except their times
    std::swap(nodes[moveFrom], nodes[moveTo]);
    std::swap(nodes[moveFrom].TimeStamp, nodes[moveTo].TimeStamp);
    FixRotationSigns(nodes);
    util::log::Write("Moved keyframe %d %s", moveFrom + 1, moveTo < moveFrom ? "up" : "down");
  }
  else if (insertAfter >= 0 && insertAfter < (int)nodes.size() && !m_IsPlaying)
  {
    // Halfway to the next keyframe, with room made when that's too close;
    // after the last, the time between new keyframes
    size_t at = (size_t)insertAfter;
    float time = nodes[at].TimeStamp + m_DefaultDuration;
    if (at + 1 < nodes.size())
    {
      if (nodes[at + 1].TimeStamp - nodes[at].TimeStamp < 2.f * g_minSegmentDuration)
        for (size_t j = at + 1; j < nodes.size(); ++j)
          nodes[j].TimeStamp += 2.f * g_minSegmentDuration;
      time = (nodes[at].TimeStamp + nodes[at + 1].TimeStamp) * 0.5f;
      ShortenHandlesForInsert(nodes, at, time);
    }
    insertNode.TimeStamp = time;
    nodes.insert(nodes.begin() + at + 1, insertNode);
    FixRotationSigns(nodes);
    util::log::Write("Keyframe %d added after keyframe %d, at %.2f s", (int)at + 2, (int)at + 1, time);
  }
  else if (deleteIndex >= 0)
  {
    nodes.erase(nodes.begin() + deleteIndex);
    // Playback starts at 0, keep the first keyframe there
    if (!nodes.empty())
    {
      float start = nodes[0].TimeStamp;
      for (CatmullRomNode& node : nodes)
        node.TimeStamp -= start;
    }
    FixRotationSigns(nodes);
    util::log::Write("Deleted keyframe %d, remaining %d", deleteIndex + 1, (int)nodes.size());
  }

  if (ImGui::TreeNode("Keyframe options"))
  {
    ImGui::TextDisabled("Keyframes set the camera's position and its:");
    ImGui::Checkbox("Rotation", &m_LockRotation);
    ImGui::SameLine();
    ImGui::Checkbox("Field of view", &m_LockFieldOfView);
    ImGui::SameLine();
    ImGui::Checkbox("Motion blur", &m_LockMotionBlur);
    ImGui::SameLine();
    if (util::IsMecmpLoaded())
      ImGui::TextDisabled("(Time of day: disabled, MECMP syncs it)");
    else
      ImGui::Checkbox("Time of day", &m_LockTimeOfDay);
    ImGui::HelpMarker("Untick one to control it yourself during playback, like turning the camera "
      "with the mouse while the track moves it.\n\nTime of day: every keyframe stores the time when it's "
      "added, and with this ticked the track sets the game's clock between them, so a track can go from "
      "day to night. The clock runs forward to the next keyframe (06:00, 18:00, 06:00 is a full day and "
      "night). Freeze the clock (GAME > Time of day > Stop the clock) while adding keyframes to choose each time yourself.");

    ImGui::Checkbox("Hand-held shake##LockShake", &m_LockShake);
    ImGui::HelpMarker("Every keyframe keeps the shake strength set when it's added (CAMERA > Camera shake > Hand-held "
      "shake). With this ticked the track sets the shake's strength between them, so it can calm down or grow during "
      "a shot.");

    // (kept in the config: CameraManager's TrackSetsGameSpeed)
    if (ImGui::Checkbox("Game speed##LockGameSpeed", &m_LockGameSpeed))
      g_mainHandle->OnConfigChanged();
    ImGui::HelpMarker("Every keyframe keeps the game speed set when it's added (GAME > Game speed; a keyframe's own "
      "can be changed in its row, which ticks this). "
      "With this ticked the track sets the game's speed between them, so slow motion can ease in and out during a "
      "shot; Game speed's own comes back when the track stops. The track itself keeps real time: the camera keeps its "
      "pace while the world slows (keyframes further apart slow the camera too), and its times stay what they say. A "
      "replay playing goes at the game speed too (GAME > Game speed > Replays at this speed), and the track's slider sends "
      "it to the same moment of the slowed action.");

    ImGui::Checkbox("Play manually", &m_ManualPlay);
    sprintf_s(label, "Instead of playing by itself, the track moves forward and back with Move up / Move down "
      "(%s / %s, or the gamepad triggers).", pInput->GetKeyName(Action::Camera_Up).c_str(), pInput->GetKeyName(Action::Camera_Down).c_str());
    ImGui::HelpMarker(label);

    ImGui::InputFloat("Time between new keyframes", &m_DefaultDuration, 0.5f, 1.f, 2);
    if (m_DefaultDuration < g_minSegmentDuration)
      m_DefaultDuration = g_minSegmentDuration;
    ImGui::HelpMarker("Seconds. Each keyframe can be changed afterwards (Seconds to the next).");

    ImGui::Combo("New keyframes move", &m_DefaultInterpolation, g_interpolationNames, Interpolation_Count);
    ImGui::HelpMarker("Smooth: a curve through the keyframes.\nLinear: straight lines at a constant speed.\n"
      "Ease in/out: the smooth curve, slowing down into and out of each keyframe.\n"
      "Hold: stays on the keyframe, then cuts to the next one.");
    ImGui::SameLine();
    if (ImGui::Button("Apply to all") && !m_IsPlaying)
    {
      for (CatmullRomNode& node : nodes)
        node.Interpolation = m_DefaultInterpolation;
    }
    ImGui::TreePop();
  }

  DrawSequenceUI(cameraEnabled);
  NoteChanges();
}

int TrackPlayer::GetStreamingPoints(XMFLOAT3* points, int max, float spacing)
{
  std::vector<CatmullRomNode> const& nodes = m_Tracks[m_SelectedTrack].Nodes;
  int count = 0;
  auto add = [&](XMVECTOR point)
  {
    // Keyframes at the same spot need only one
    if (count > 0 && XMVectorGetX(XMVector3Length(point - XMLoadFloat3(&points[count - 1]))) < 1.f)
      return;
    if (count < max)
      XMStoreFloat3(&points[count++], point);
  };

  for (size_t i = 0; i < nodes.size() && count < max; ++i)
  {
    XMVECTOR a = XMLoadFloat3(&nodes[i].Position);
    add(a);
    if (i + 1 < nodes.size())
    {
      XMVECTOR b = XMLoadFloat3(&nodes[i + 1].Position);
      SegmentNodes segment = Segment(nodes, i);
      if ((segment.n1.Handles || segment.n2.Handles) && CurvedInterpolation(segment.n1.Interpolation))
      {
        // Handles set by hand can take the curve far off the straight line:
        // points along the curve itself (its length from 32 steps)
        float length = 0.f;
        XMVECTOR last = a;
        for (int k = 1; k <= 32; ++k)
        {
          XMVECTOR point = CurvePosition(segment.n0, segment.n1, segment.n2, segment.n3, k / 32.f);
          length += XMVectorGetX(XMVector3Length(point - last));
          last = point;
        }
        int steps = (int)(length / spacing);
        for (int k = 1; k <= steps && count < max; ++k)
          add(CurvePosition(segment.n0, segment.n1, segment.n2, segment.n3, (float)k / (steps + 1)));
        continue;
      }
      // Straight lines are close enough, zones are much bigger than the curve
      int steps = (int)(XMVectorGetX(XMVector3Length(b - a)) / spacing);
      for (int k = 1; k <= steps && count < max; ++k)
        add(XMVectorLerp(a, b, (float)k / (steps + 1)));
    }
  }
  return count;
}

bool TrackPlayer::SampleAt(double time, CatmullRomNode& out)
{
  std::vector<CatmullRomNode> const& nodes = m_Tracks[m_SelectedTrack].Nodes;
  if (nodes.size() < 2)
    return false;
  double end = nodes.back().TimeStamp;
  if (time < 0.0)
    time = 0.0;
  if (time > end)
    time = end;
  unsigned int segment = 0;
  while (segment + 2 < nodes.size() && time >= nodes[segment + 1].TimeStamp)
    ++segment;
  if (!(nodes[segment + 1].TimeStamp > nodes[segment].TimeStamp))
    return false;
  // Next to an orbit keyframe that can be placed, it goes round its point: not
  // a place of its own between them
  XMFLOAT3 const* liveFocus = m_LiveFocusOk ? &m_LiveFocus : nullptr;
  if (Placeable(nodes[segment], liveFocus) || Placeable(nodes[segment + 1], liveFocus))
    return false;
  out = InterpolateSegment(nodes, segment, time, nullptr, 0.f);
  return !out.Orbit;
}

// As PlayForward gives it: the ends' own, else the segment's (the time of day
// doesn't depend on orbits or the focus, InterpolateNodes)
bool TrackPlayer::TimeOfDayAt(double time, float& clock)
{
  std::vector<CatmullRomNode> const& nodes = m_Tracks[m_SelectedTrack].Nodes;
  clock = -1.f;
  if (nodes.size() < 2)
    return false;
  double end = nodes.back().TimeStamp;
  if (time <= 0.0)
    clock = nodes.front().TimeOfDay;
  else if (time >= end)
    clock = nodes.back().TimeOfDay;
  else
  {
    unsigned int segment = 0;
    while (segment + 2 < nodes.size() && time >= nodes[segment + 1].TimeStamp)
      ++segment;
    if (!(nodes[segment + 1].TimeStamp > nodes[segment].TimeStamp))
      return false;
    CatmullRomNode const& n1 = nodes[segment];
    CatmullRomNode const& n2 = nodes[segment + 1];
    CatmullRomNode const& n0 = segment > 0 ? nodes[segment - 1] : n1;
    CatmullRomNode const& n3 = segment + 2 < nodes.size() ? nodes[segment + 2] : n2;
    clock = InterpolateNodes(n0, n1, n2, n3, time).TimeOfDay;
  }
  return clock >= 0.f;
}

int TrackPlayer::InsertNodes(std::vector<std::pair<double, XMFLOAT3>> const& inserts)
{
  std::vector<CatmullRomNode>& nodes = m_Tracks[m_SelectedTrack].Nodes;
  if (m_IsPlaying || inserts.empty() || nodes.size() < 2)
    return 0;
  // Made from the keyframes as they are, then put in together (one undo step)
  std::vector<CatmullRomNode> made;
  for (auto const& insert : inserts)
  {
    double time = insert.first;
    size_t i = 0;
    while (i + 2 < nodes.size() && time >= nodes[i + 1].TimeStamp)
      ++i;
    // Not on top of a keyframe, nor in a cut (Hold) or round a focus
    if (time - nodes[i].TimeStamp < MinInsertGap || nodes[i + 1].TimeStamp - time < MinInsertGap ||
        nodes[i].Interpolation == Interpolation_Hold)
      continue;
    CatmullRomNode node;
    if (!SampleAt(time, node))
      continue;
    node.Position = insert.second;
    node.TimeStamp = (float)time;
    node.Interpolation = nodes[i].Interpolation;
    node.TimeOfDayBackward = nodes[i].TimeOfDayBackward;
    node.Orbit = false;
    node.OrbitFocusSet = false;
    node.OrbitDistance = 0.f;
    made.push_back(node);
  }
  if (made.empty())
    return 0;
  PushUndo();
  for (CatmullRomNode const& node : made)
  {
    auto at = std::upper_bound(nodes.begin(), nodes.end(), node.TimeStamp,
      [](float t, CatmullRomNode const& n) { return t < n.TimeStamp; });
    if (at != nodes.begin() && at != nodes.end())
      ShortenHandlesForInsert(nodes, (size_t)(at - nodes.begin()) - 1, node.TimeStamp);
    nodes.insert(at, node);
  }
  FixRotationSigns(nodes);
  SyncShadow();
  return (int)made.size();
}

int TrackPlayer::GetPathPoints(XMFLOAT3* points, bool* breaks, int max, int perSegment, XMFLOAT3 const* focus,
  float baseYaw)
{
  std::vector<CatmullRomNode> const& nodes = m_Tracks[m_SelectedTrack].Nodes;
  if (perSegment < 1)
    perSegment = 1;
  int count = 0;
  bool cut = false;
  auto add = [&](XMVECTOR point)
  {
    if (count >= max)
      return;
    XMStoreFloat3(&points[count], point);
    breaks[count] = count == 0 || cut;
    cut = false;
    ++count;
  };
  // Each segment as PlayForward plays it (its position only), orbit
  // keyframes round their points (or, without one, the focus where it is)
  for (size_t i = 0; i + 1 < nodes.size() && count < max; ++i)
  {
    CatmullRomNode const* p1 = &nodes[i];
    CatmullRomNode const* p2 = &nodes[i + 1];
    CatmullRomNode const* p0 = i > 0 ? &nodes[i - 1] : p1;
    CatmullRomNode const* p3 = i + 2 < nodes.size() ? &nodes[i + 2] : p2;
    CatmullRomNode live[4];
    LiveSegment(p0, p1, p2, p3, live, focus, baseYaw);
    CatmullRomNode const& n1 = *p1;
    CatmullRomNode const& n2 = *p2;
    CatmullRomNode const& n0 = *p0;
    CatmullRomNode const& n3 = *p3;
    XMVECTOR vPos1 = XMLoadFloat3(&n1.Position);
    XMVECTOR vPos2 = XMLoadFloat3(&n2.Position);
    CatmullRomNode orbit;
    if (n1.Interpolation == Interpolation_Hold)
    {
      InterpolateOrbit(n0, n1, n2, n3, 0.f, orbit);
      add(orbit.Orbit && orbit.OrbitFocusSet ? OrbitPlaceOf(orbit) : vPos1);
      cut = true;
      continue;
    }
    // Handles set by hand can bend it more sharply: more points along it
    int steps = (n1.Handles || n2.Handles) ? perSegment * 3 : perSegment;
    for (int k = 0; k < steps; ++k)
    {
      float mu = (float)k / steps;
      InterpolateOrbit(n0, n1, n2, n3, mu, orbit);
      if (orbit.Orbit && orbit.OrbitFocusSet)
        add(OrbitPlaceOf(orbit));
      else if (n1.Interpolation == Interpolation_Linear)
        add(XMVectorLerp(vPos1, vPos2, mu));
      else
      {
        float s = (n1.Interpolation == Interpolation_EaseInOut) ? BezierEaseInOut(mu) : mu;
        add(CurvePosition(n0, n1, n2, n3, s));
      }
    }
  }
  if (!nodes.empty())
  {
    CatmullRomNode last = nodes.back();
    bool orbit = Placeable(last, focus);
    if (orbit)
      NormalizeOrbit(last, focus, baseYaw);
    add(orbit ? OrbitPlaceOf(last) : XMLoadFloat3(&last.Position));
  }
  return count;
}

bool TrackPlayer::GetOrbitPivot(int index, XMFLOAT3 const* focus, float baseYaw, XMFLOAT3& pivot)
{
  std::vector<CatmullRomNode> const& nodes = m_Tracks[m_SelectedTrack].Nodes;
  if (index < 0 || index >= (int)nodes.size() || !Placeable(nodes[index], focus))
    return false;
  CatmullRomNode node = nodes[index];
  NormalizeOrbit(node, focus, baseYaw);
  pivot = node.OrbitFocus;
  return true;
}

bool TrackPlayer::GetKeyframePlace(int index, XMFLOAT3 const* focus, float baseYaw, XMFLOAT3& place)
{
  std::vector<CatmullRomNode> const& nodes = m_Tracks[m_SelectedTrack].Nodes;
  if (index < 0 || index >= (int)nodes.size())
    return false;
  CatmullRomNode node = nodes[index];
  if (Placeable(node, focus))
  {
    NormalizeOrbit(node, focus, baseYaw);
    PlaceRoundFocus(node);
  }
  place = node.Position;
  return true;
}

bool TrackPlayer::SegmentPivot(int index, XMFLOAT3 const* focus, float baseYaw, XMFLOAT3& pivot)
{
  // As LiveSegment takes it: its first end's point, else its second's
  return GetOrbitPivot(index, focus, baseYaw, pivot) || GetOrbitPivot(index + 1, focus, baseYaw, pivot);
}

int TrackPlayer::GetKeyframePoints(XMFLOAT3* points, int max, XMFLOAT3 const* focus, float baseYaw, bool* roundFocus,
  XMFLOAT4* rotations, float* fieldsOfView)
{
  std::vector<CatmullRomNode> const& nodes = m_Tracks[m_SelectedTrack].Nodes;
  int count = 0;
  for (size_t i = 0; i < nodes.size() && count < max; ++i)
  {
    // An orbit keyframe round its point (or, without one, the focus where it
    // is), with its view there (as it plays)
    CatmullRomNode const& node = nodes[i];
    bool orbit = Placeable(node, focus);
    if (roundFocus)
      roundFocus[count] = orbit;
    if (fieldsOfView)
      fieldsOfView[count] = node.FieldOfView;
    if (orbit)
    {
      CatmullRomNode placed = node;
      NormalizeOrbit(placed, focus, baseYaw);
      PlaceRoundFocus(placed);
      points[count] = placed.Position;
      if (rotations)
        rotations[count] = placed.Rotation;
    }
    else
    {
      points[count] = node.Position;
      if (rotations)
        rotations[count] = node.Rotation;
    }
    ++count;
  }
  return count;
}

bool TrackPlayer::SetOrbitKeyframePlace(int index, float yaw, float pitch, float distance, XMFLOAT3 const* focus,
  float baseYaw)
{
  std::vector<CatmullRomNode>& nodes = m_Tracks[m_SelectedTrack].Nodes;
  if (m_IsPlaying || index < 0 || index >= (int)nodes.size() || !Placeable(nodes[index], focus) || !(distance > 0.f))
    return false;
  StepPathDrag();
  CatmullRomNode& node = nodes[index];
  // (one without a point of its own gets the focus where it is now, kept)
  NormalizeOrbit(node, focus, baseYaw);
  node.OrbitYaw = XMScalarModAngle(yaw);
  node.OrbitPitch = pitch;
  node.OrbitDistance = distance;
  // (its own view, OrbitView, stays: it turns with it round its point)
  PlaceRoundFocus(node);
  FixRotationSigns(nodes);
  SyncShadow();
  return true;
}

bool TrackPlayer::GetOrbitArch(int index, XMFLOAT3 const* focus, float baseYaw, XMFLOAT3& place, float& yaw,
  float& pitch, float& distance, XMFLOAT3& pivot)
{
  std::vector<CatmullRomNode> const& nodes = m_Tracks[m_SelectedTrack].Nodes;
  if (!IsOrbitSegment(index))
    return false;
  // The segment round its point as it plays (copies: see LiveSegment)
  CatmullRomNode const* p1 = &nodes[index];
  CatmullRomNode const* p2 = &nodes[index + 1];
  CatmullRomNode const* p0 = index > 0 ? &nodes[index - 1] : p1;
  CatmullRomNode const* p3 = index + 2 < (int)nodes.size() ? &nodes[index + 2] : p2;
  CatmullRomNode live[4];
  if (!LiveSegment(p0, p1, p2, p3, live, focus, baseYaw))
    return false;
  CatmullRomNode middle;
  InterpolateOrbit(*p0, *p1, *p2, *p3, 0.5f, middle);
  if (!middle.Orbit || !middle.OrbitFocusSet)
    return false;
  XMStoreFloat3(&place, OrbitPlaceOf(middle));
  // The plain arc's middle: the same with no rise and no bulge (a first
  // keyframe standing in for a missing one before it stays the same one)
  CatmullRomNode plain = *p1;
  plain.OrbitLift = 0.f;
  plain.OrbitBulge = 0.f;
  CatmullRomNode flat;
  InterpolateOrbit(p0 == p1 ? plain : *p0, plain, *p2, *p3, 0.5f, flat);
  // (its heading from the world's, round its point there)
  yaw = flat.OrbitYaw;
  pitch = flat.OrbitPitch;
  distance = flat.OrbitDistance;
  pivot = flat.OrbitFocus;
  return true;
}

bool TrackPlayer::SetOrbitArch(int index, float lift, float bulge, XMFLOAT3 const* focus, float baseYaw,
  float minDistance, float maxPitch)
{
  std::vector<CatmullRomNode>& nodes = m_Tracks[m_SelectedTrack].Nodes;
  if (m_IsPlaying || !IsOrbitSegment(index))
    return false;
  CatmullRomNode const* p1 = &nodes[index];
  CatmullRomNode const* p2 = &nodes[index + 1];
  CatmullRomNode const* p0 = index > 0 ? &nodes[index - 1] : p1;
  CatmullRomNode const* p3 = index + 2 < (int)nodes.size() ? &nodes[index + 2] : p2;
  CatmullRomNode live[4];
  if (!LiveSegment(p0, p1, p2, p3, live, focus, baseYaw))
    return false;
  // Along the arc with a rise and bulge tried: the nearest it comes to the
  // focus and the furthest from level (the rise moves only the pitch, the
  // bulge only the distance)
  const int Steps = 32;
  auto extremes = [&](float tryLift, float tryBulge, float& nearest, float& steepest)
  {
    CatmullRomNode trial = *p1;
    trial.OrbitLift = tryLift;
    trial.OrbitBulge = tryBulge;
    nearest = 1e9f;
    steepest = 0.f;
    CatmullRomNode at;
    for (int k = 0; k <= Steps; ++k)
    {
      InterpolateOrbit(p0 == p1 ? trial : *p0, trial, *p2, *p3, (float)k / Steps, at);
      nearest = (std::min)(nearest, at.OrbitDistance);
      steepest = (std::max)(steepest, fabsf(at.OrbitPitch));
    }
  };
  // The plain arc's own: an end nearer or steeper than the limits keeps
  // that much allowed
  float plainNearest, plainSteepest;
  extremes(0.f, 0.f, plainNearest, plainSteepest);
  float least = (std::min)(minDistance, plainNearest);
  float most = (std::max)(maxPitch, plainSteepest);
  // Each eased back towards none (halving the gap 12 times) as far as needed
  float nearest, steepest;
  extremes(lift, 0.f, nearest, steepest);
  if (steepest > most)
  {
    float lo = 0.f, hi = 1.f;
    for (int i = 0; i < 12; ++i)
    {
      float mid = (lo + hi) * 0.5f;
      extremes(lift * mid, 0.f, nearest, steepest);
      (steepest > most ? hi : lo) = mid;
    }
    lift *= lo;
  }
  extremes(0.f, bulge, nearest, steepest);
  if (nearest < least)
  {
    float lo = 0.f, hi = 1.f;
    for (int i = 0; i < 12; ++i)
    {
      float mid = (lo + hi) * 0.5f;
      extremes(0.f, bulge * mid, nearest, steepest);
      (nearest < least ? hi : lo) = mid;
    }
    bulge *= lo;
  }
  StepPathDrag();
  nodes[index].OrbitLift = lift;
  nodes[index].OrbitBulge = bulge;
  SyncShadow();
  return true;
}

bool TrackPlayer::DeleteKeyframe(int index)
{
  std::vector<CatmullRomNode>& nodes = m_Tracks[m_SelectedTrack].Nodes;
  if (m_IsPlaying || index < 0 || index >= (int)nodes.size())
    return false;
  PushUndo();
  nodes.erase(nodes.begin() + index);
  // (as the list's Delete: playback starts at 0, the first keyframe kept there)
  if (!nodes.empty())
  {
    float start = nodes[0].TimeStamp;
    for (CatmullRomNode& node : nodes)
      node.TimeStamp -= start;
  }
  FixRotationSigns(nodes);
  SyncShadow();
  util::log::Write("Track: keyframe %d deleted on the drawn path, remaining %d", index + 1, (int)nodes.size());
  return true;
}

bool TrackPlayer::ResetKeyframeHandles(int index)
{
  std::vector<CatmullRomNode>& nodes = m_Tracks[m_SelectedTrack].Nodes;
  if (m_IsPlaying || index < 0 || index >= (int)nodes.size() || !nodes[index].Handles)
    return false;
  PushUndo();
  CatmullRomNode& node = nodes[index];
  node.Handles = false;
  node.HandlesFree = false;
  node.HandleIn = XMFLOAT3(0.f, 0.f, 0.f);
  node.HandleOut = XMFLOAT3(0.f, 0.f, 0.f);
  SyncShadow();
  return true;
}

bool TrackPlayer::TakePreviewHold(double& time)
{
  if (!m_PreviewHeld)
    return false;
  m_PreviewHeld = false;
  time = m_PreviewHeldTime;
  return true;
}

bool TrackPlayer::PreviewAt(double time, XMFLOAT3 const* focus, float baseYaw, XMFLOAT3& place, XMFLOAT4& rotation,
  float& fieldOfView)
{
  std::vector<CatmullRomNode> const& nodes = m_Tracks[m_SelectedTrack].Nodes;
  if (nodes.size() < 2)
    return false;
  // As PlayForward has it: an end keyframe as it is (an orbit one round its
  // point), else the segment there
  CatmullRomNode at;
  if (time <= 0.0 || time >= nodes.back().TimeStamp)
  {
    at = time <= 0.0 ? nodes.front() : nodes.back();
    at.Orbit = Placeable(at, focus);
    if (at.Orbit)
      NormalizeOrbit(at, focus, baseYaw);
  }
  else
  {
    unsigned int segment = 0;
    while (segment + 2 < nodes.size() && time >= nodes[segment + 1].TimeStamp)
      ++segment;
    at = InterpolateSegment(nodes, segment, time, focus, baseYaw);
  }
  // Round its point, looking at it with its own view on top (as
  // CameraManager places the camera; one that can't be placed, as kept)
  if (at.Orbit && at.OrbitFocusSet)
  {
    XMVECTOR direction = OrbitDirection(at.OrbitYaw, at.OrbitPitch);
    XMStoreFloat3(&place, XMLoadFloat3(&at.OrbitFocus) + direction * at.OrbitDistance);
    XMStoreFloat4(&rotation, OrbitViewAt(at, direction));
  }
  else
  {
    place = at.Position;
    rotation = at.Rotation;
  }
  fieldOfView = at.FieldOfView;
  return true;
}

void TrackPlayer::GoToKeyframe(int index, Camera& camera, XMFLOAT3 const* focus, float baseYaw)
{
  std::vector<CatmullRomNode> const& nodes = m_Tracks[m_SelectedTrack].Nodes;
  if (m_IsPlaying || index < 0 || index >= (int)nodes.size())
    return;
  CatmullRomNode node = nodes[index];
  if (Placeable(node, focus))
  {
    NormalizeOrbit(node, focus, baseYaw);
    PlaceRoundFocus(node);
  }
  camera.Position = node.Position;
  camera.Rotation = node.Rotation;
  camera.FieldOfView = node.FieldOfView;
  camera.MotionBlur = node.MotionBlur;
  if (m_LockTimeOfDay && node.TimeOfDay >= 0)
  {
    camera.TimeOfDay = node.TimeOfDay;
    g_mainHandle->GetVisualsManager()->SetTimeOfDay(node.TimeOfDay);
  }
}

bool TrackPlayer::FlattenOrbitArch(int index)
{
  std::vector<CatmullRomNode>& nodes = m_Tracks[m_SelectedTrack].Nodes;
  if (m_IsPlaying || index < 0 || index >= (int)nodes.size() ||
      (nodes[index].OrbitLift == 0.f && nodes[index].OrbitBulge == 0.f))
    return false;
  PushUndo();
  nodes[index].OrbitLift = 0.f;
  nodes[index].OrbitBulge = 0.f;
  SyncShadow();
  return true;
}

void TrackPlayer::BeginPathDrag()
{
  m_PathDragging = true;
  m_PathDragStepped = false;
  m_PathDragTrack = m_SelectedTrack;
  m_PathDragBefore = m_Tracks[m_SelectedTrack].Nodes;
}

bool TrackPlayer::EndPathDrag()
{
  bool changed = m_PathDragging && m_PathDragStepped;
  m_PathDragging = false;
  m_PathDragStepped = false;
  m_PathDragBefore.clear();
  return changed;
}

void TrackPlayer::StepPathDrag()
{
  // Undone, redone or another track since the drag began: a drag from here
  if (!m_PathDragging || m_PathDragTrack != m_SelectedTrack)
    BeginPathDrag();
  if (m_PathDragStepped)
    return;
  m_Undo.push_back({ m_PathDragTrack, m_PathDragBefore });   // as they were before the drag
  if (m_Undo.size() > MaxUndoSteps)
    m_Undo.erase(m_Undo.begin());
  m_Redo.clear();
  m_PathDragStepped = true;
}

bool TrackPlayer::SetKeyframePosition(int index, XMFLOAT3 const& position)
{
  std::vector<CatmullRomNode>& nodes = m_Tracks[m_SelectedTrack].Nodes;
  if (m_IsPlaying || index < 0 || index >= (int)nodes.size())
    return false;
  StepPathDrag();
  nodes[index].Position = position;
  SyncShadow();
  return true;
}

bool TrackPlayer::GetKeyframeHandles(int index, bool hasFocus, XMFLOAT3& in, XMFLOAT3& out, bool& hasIn, bool& hasOut)
{
  std::vector<CatmullRomNode> const& nodes = m_Tracks[m_SelectedTrack].Nodes;
  hasIn = hasOut = false;
  if (index < 0 || index >= (int)nodes.size())
    return false;
  // A side's handle shapes the path where that segment curves (Smooth or
  // Ease in/out), moves, and isn't drawn round a point (an orbit keyframe at
  // either end that can be placed: its own point, or the focus)
  auto round = [&](CatmullRomNode const& n) { return IsOrbitKeyframe(n) && (n.OrbitFocusSet || hasFocus); };
  auto shapes = [&](size_t a)
  {
    CatmullRomNode const& n1 = nodes[a];
    CatmullRomNode const& n2 = nodes[a + 1];
    if (!CurvedInterpolation(n1.Interpolation) || !(n2.TimeStamp > n1.TimeStamp) || round(n1) || round(n2))
      return false;
    return XMVectorGetX(XMVector3Length(XMLoadFloat3(&n2.Position) - XMLoadFloat3(&n1.Position))) >= 1e-6f;
  };
  hasOut = index + 1 < (int)nodes.size() && shapes((size_t)index);
  hasIn = index > 0 && shapes((size_t)index - 1);
  CatmullRomNode const& node = nodes[index];
  XMFLOAT3 handleIn = node.HandleIn, handleOut = node.HandleOut;
  if (!node.Handles)
    AutomaticHandles(nodes, (size_t)index, handleIn, handleOut);
  XMStoreFloat3(&in, XMLoadFloat3(&node.Position) + XMLoadFloat3(&handleIn));
  XMStoreFloat3(&out, XMLoadFloat3(&node.Position) + XMLoadFloat3(&handleOut));
  return hasIn || hasOut;
}

bool TrackPlayer::SetKeyframeHandle(int index, bool out, XMFLOAT3 const& place)
{
  std::vector<CatmullRomNode>& nodes = m_Tracks[m_SelectedTrack].Nodes;
  if (m_IsPlaying || index < 0 || index >= (int)nodes.size())
    return false;
  StepPathDrag();
  CatmullRomNode& node = nodes[index];
  // The first time: the handles its automatic curve has, so only the one
  // dragged changes it
  if (!node.Handles)
  {
    XMFLOAT3 in, outHandle;
    AutomaticHandles(nodes, (size_t)index, in, outHandle);
    node.HandleIn = in;
    node.HandleOut = outHandle;
    node.Handles = true;
  }
  XMVECTOR offset = XMLoadFloat3(&place) - XMLoadFloat3(&node.Position);
  XMStoreFloat3(out ? &node.HandleOut : &node.HandleIn, offset);
  // The other one in line with it, its own length kept, so the path goes
  // smoothly through the keyframe (unless they're free: a corner)
  if (!node.HandlesFree)
  {
    XMFLOAT3& other = out ? node.HandleIn : node.HandleOut;
    float length = XMVectorGetX(XMVector3Length(XMLoadFloat3(&other)));
    float dragged = XMVectorGetX(XMVector3Length(offset));
    if (dragged > 1e-4f)
      XMStoreFloat3(&other, offset * (-length / dragged));
  }
  SyncShadow();
  return true;
}

int TrackPlayer::GetPickedKeyframe()
{
  // Another track, or keyframes added or deleted since: none (the number
  // would be another keyframe's now)
  if (m_PickedTrack != m_SelectedTrack || m_PickedCount != (int)m_Tracks[m_SelectedTrack].Nodes.size() ||
      m_PickedKeyframe >= m_PickedCount)
    return -1;
  return m_PickedKeyframe;
}

void TrackPlayer::PickKeyframe(int index)
{
  if (index >= 0 && index != GetPickedKeyframe())
    m_OpenPicked = index;
  m_PickedKeyframe = index;
  m_PickedTrack = m_SelectedTrack;
  m_PickedCount = (int)m_Tracks[m_SelectedTrack].Nodes.size();
}

bool TrackPlayer::TakeOrbitRequest(OrbitRequest& request)
{
  if (m_OrbitRequests.empty())
    return false;
  request = m_OrbitRequests.front();
  m_OrbitRequests.erase(m_OrbitRequests.begin());
  return true;
}

bool TrackPlayer::IsOrbitSegment(int index)
{
  std::vector<CatmullRomNode> const& nodes = m_Tracks[m_SelectedTrack].Nodes;
  if (index < 0 || index + 1 >= (int)nodes.size())
    return false;
  CatmullRomNode const& n1 = nodes[index];
  CatmullRomNode const& n2 = nodes[index + 1];
  return (IsOrbitKeyframe(n1) || IsOrbitKeyframe(n2)) && n1.Interpolation != Interpolation_Hold &&
    n2.TimeStamp > n1.TimeStamp;
}

bool TrackPlayer::SetOrbitPlace(int index, float yaw, float pitch, float distance, XMFLOAT3 const& pivot)
{
  std::vector<CatmullRomNode>& nodes = m_Tracks[m_SelectedTrack].Nodes;
  if (m_IsPlaying || index < 0 || index >= (int)nodes.size())
    return false;
  CatmullRomNode& node = nodes[index];
  node.Orbit = true;
  // (round the focus where it is now, kept: see CatmullRomNode::OrbitFocus)
  node.OrbitFocus = pivot;
  node.OrbitFocusSet = true;
  node.OrbitYaw = yaw;
  node.OrbitPitch = pitch;
  node.OrbitDistance = distance;
  // Its own view kept: the turn from looking at its point there to it, so
  // OrbitViewAt gives its rotation back (view = OrbitView * looking at it)
  XMVECTOR look = LookAtFocus(OrbitDirection(yaw, pitch));
  XMVECTOR own = XMQuaternionNormalize(XMLoadFloat4(&node.Rotation));
  XMStoreFloat4(&node.OrbitView, XMQuaternionNormalize(XMQuaternionMultiply(own, XMQuaternionInverse(look))));
  SyncShadow();   // part of the tick's undo step
  return true;
}

void TrackPlayer::UntickOrbit(int index)
{
  std::vector<CatmullRomNode>& nodes = m_Tracks[m_SelectedTrack].Nodes;
  if (m_IsPlaying || index < 0 || index >= (int)nodes.size())
    return;
  nodes[index].Orbit = false;
  SyncShadow();
}

float TrackPlayer::PathSweep(int index, XMFLOAT3 const& focus)
{
  std::vector<CatmullRomNode> const& nodes = m_Tracks[m_SelectedTrack].Nodes;
  if (index < 0 || index + 1 >= (int)nodes.size())
    return 0.f;
  SegmentNodes segment = Segment(nodes, index);
  // The angle round the focus' upright line, summed in small steps along the
  // path as it would go between the two places (its shape: an eased one's
  // eased time makes no difference to where it goes)
  float sweep = 0.f, last = 0.f;
  bool hasLast = false;
  for (int k = 0; k <= 64; ++k)
  {
    float mu = k / 64.f;
    XMVECTOR place;
    if (segment.n1.Interpolation == Interpolation_Hold)
      place = XMLoadFloat3(&segment.n1.Position);
    else if (segment.n1.Interpolation == Interpolation_Linear)
      place = XMVectorLerp(XMLoadFloat3(&segment.n1.Position), XMLoadFloat3(&segment.n2.Position), mu);
    else
      place = CurvePosition(segment.n0, segment.n1, segment.n2, segment.n3, mu);
    XMFLOAT3 at;
    XMStoreFloat3(&at, place);
    float dx = at.x - focus.x, dz = at.z - focus.z;
    if (dx * dx + dz * dz < 1e-6f)
      continue;   // right over or under it: no way round
    float angle = atan2f(dx, dz);   // as OrbitYaw is measured (CameraManager::OrbitFocus)
    if (hasLast)
      sweep += XMScalarModAngle(angle - last);
    last = angle;
    hasLast = true;
  }
  return sweep;
}

float TrackPlayer::OrbitStepWith(int index, int way, int turns, XMFLOAT3 const* focus, float baseYaw)
{
  std::vector<CatmullRomNode> const& nodes = m_Tracks[m_SelectedTrack].Nodes;
  if (index < 0 || index + 1 >= (int)nodes.size())
    return 0.f;
  // An end that isn't an orbit keyframe: where it is round the other's point
  CatmullRomNode const* p1 = &nodes[index];
  CatmullRomNode const* p2 = &nodes[index + 1];
  CatmullRomNode const* p0 = index > 0 ? &nodes[index - 1] : p1;
  CatmullRomNode const* p3 = index + 2 < (int)nodes.size() ? &nodes[index + 2] : p2;
  CatmullRomNode live[4];
  LiveSegment(p0, p1, p2, p3, live, focus, baseYaw);
  CatmullRomNode n1 = *p1;
  n1.OrbitWay = way;
  n1.OrbitTurns = turns;
  return OrbitStep(n1, *p2);
}

int TrackPlayer::OrbitArcPoints(int index, int way, int turns, float lift, XMFLOAT3 const* focus, float baseYaw,
  XMFLOAT3* points, int max)
{
  std::vector<CatmullRomNode> const& nodes = m_Tracks[m_SelectedTrack].Nodes;
  if (max < 2 || !IsOrbitSegment(index))
    return 0;
  // The segment as it would be with that way, round its point as it plays
  // (copies: see LiveSegment), its first keyframe with the way tried
  CatmullRomNode const* p1 = &nodes[index];
  CatmullRomNode const* p2 = &nodes[index + 1];
  CatmullRomNode const* n0 = index > 0 ? &nodes[index - 1] : p1;
  CatmullRomNode const* n3 = index + 2 < (int)nodes.size() ? &nodes[index + 2] : p2;
  CatmullRomNode live[4];
  if (!LiveSegment(n0, p1, p2, n3, live, focus, baseYaw))
    return 0;
  CatmullRomNode& n1 = live[1];   // p1 is it
  n1.OrbitWay = way;
  n1.OrbitTurns = turns;
  n1.OrbitLift = lift;
  CatmullRomNode const& n2 = *p2;
  // About one point every 0.75 m along it (its length from the ends' and the
  // lift's), at least 8 steps
  float distance = (n1.OrbitDistance + n2.OrbitDistance) * 0.5f;
  float pitch = (n1.OrbitPitch + n2.OrbitPitch) * 0.5f;
  float length = fabsf(OrbitStep(n1, n2)) * distance * cosf(pitch) + fabsf(n2.OrbitPitch - n1.OrbitPitch) * distance +
    2.f * fabsf(lift) * distance + fabsf(n2.OrbitDistance - n1.OrbitDistance) + 2.f * fabsf(n1.OrbitBulge);
  int steps = (int)ceilf(length / 0.75f);
  steps = steps < 8 ? 8 : (steps > max - 1 ? max - 1 : steps);
  CatmullRomNode orbit;
  for (int k = 0; k <= steps; ++k)
  {
    InterpolateOrbit(*n0, n1, n2, *n3, (float)k / steps, orbit);
    if (!orbit.Orbit || !orbit.OrbitFocusSet)
      return 0;
    XMStoreFloat3(&points[k], OrbitPlaceOf(orbit));
  }
  return steps + 1;
}

bool TrackPlayer::SetOrbitWay(int index, int way, int turns, float lift, bool ownStep)
{
  std::vector<CatmullRomNode>& nodes = m_Tracks[m_SelectedTrack].Nodes;
  if (m_IsPlaying || index < 0 || index >= (int)nodes.size())
    return false;
  CatmullRomNode& node = nodes[index];
  if (node.OrbitWay == way && node.OrbitTurns == turns && node.OrbitLift == lift)
    return true;
  if (ownStep)
    PushUndo();
  node.OrbitWay = way;
  node.OrbitTurns = turns;
  node.OrbitLift = lift;
  SyncShadow();
  return true;
}

void TrackPlayer::SetOrbitNote(int index, std::string const& note)
{
  int count = (int)m_Tracks[m_SelectedTrack].Nodes.size();
  if (m_OrbitNotesTrack != m_SelectedTrack || m_OrbitNotesCount != count)
  {
    m_OrbitNotes.clear();
    m_OrbitNotesTrack = m_SelectedTrack;
    m_OrbitNotesCount = count;
  }
  m_OrbitNotes[index] = note;
}

void TrackPlayer::DrawSequenceUI(bool cameraEnabled)
{
  // A red bar like KEYFRAMES', with a tinted row for each shot (the shot
  // playing brighter), so it's as easy to find
  float fontSize = ImGui::GetFontSize();
  const ImVec4 red = ImGui::GetStyleColorVec4(ImGuiCol_Button);
  const ImVec4 redLight = ImGui::GetStyleColorVec4(ImGuiCol_ButtonHovered);
  const ImVec4 redBright = ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive);
  auto withAlpha = [](ImVec4 color, float alpha) { color.w = alpha; return color; };

  ImGui::Dummy(ImVec2(0, ImGui::GetStyle().ItemSpacing.y));
  char header[64];
  sprintf_s(header, "SHOT SEQUENCE    %d###ShotSequence", (int)m_Sequence.size());
  ImGui::PushStyleColor(ImGuiCol_Header, red);
  ImGui::PushStyleColor(ImGuiCol_HeaderHovered, redLight);
  ImGui::PushStyleColor(ImGuiCol_HeaderActive, redBright);
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(ImGui::GetStyle().FramePadding.x, ImGui::GetStyle().FramePadding.y * 1.8f));
  bool open = ImGui::TreeNodeEx(header, ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_Framed);
  ImGui::PopStyleVar();
  ImGui::PopStyleColor(3);
  if (!open)
    return;

  ImGui::PushTextWrapPos(0.f);
  ImGui::TextDisabled(m_Sequence.empty()
    ? "No shots yet. Add a shot, then pick its track from its list. The tracks play one after the other, cutting "
      "from each to the next; a replay playing along with the track goes on through the cuts."
    : "The tracks play one after the other, cutting from each to the next. A replay playing along with the track "
      "goes on through the cuts.");
  ImGui::PopTextWrapPos();
  int moveFrom = -1, moveTo = -1, removeAt = -1;
  for (size_t k = 0; k < m_Sequence.size(); ++k)
  {
    ImGui::PushID((int)k);
    int track = m_Sequence[k];
    bool valid = track >= 0 && track < (int)m_Tracks.size();
    std::vector<CatmullRomNode> const* shot = valid ? &m_Tracks[track].Nodes : nullptr;
    bool playingThis = IsSequencePlaying() && k == m_SequencePos;

    // The row's tint, as a keyframe's (drawn here: a framed tree row would
    // show an arrow, and a shot has nothing to open)
    ImVec2 rowMin = ImGui::GetCursorScreenPos();
    ImVec2 rowMax(rowMin.x + ImGui::GetContentRegionAvailWidth(), rowMin.y + ImGui::GetFrameHeight());
    ImGui::GetWindowDrawList()->AddRectFilled(rowMin, rowMax,
      ImGui::GetColorU32(playingThis ? withAlpha(redBright, 0.9f) : withAlpha(red, 0.35f)), ImGui::GetStyle().FrameRounding);
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetStyle().FramePadding.x);
    ImGui::AlignTextToFramePadding();
    ImGui::Text("%sShot %d", playingThis ? "> " : "", (int)k + 1);

    // Its track, picked from the list (while nothing plays), and its length;
    // the list at the same place on every row (from the row's start, past "> Shot 10")
    ImGui::SameLine(rowMin.x - ImGui::GetWindowPos().x + ImGui::GetScrollX() + fontSize * 6.f);
    if (!m_IsPlaying)
    {
      int choice = valid ? track : -1;
      ImGui::PushItemWidth(fontSize * 9.f);
      if (ImGui::Combo("##ShotTrack", &choice, m_TrackNames.data(), (int)m_TrackNames.size()) && choice >= 0 &&
          choice < (int)m_Tracks.size())
        m_Sequence[k] = choice;
      ImGui::PopItemWidth();
    }
    else
      ImGui::TextUnformatted(valid ? m_Tracks[track].Name.c_str() : "(a deleted track)");
    ImGui::SameLine();
    if (!valid)
      ImGui::TextDisabled("skipped");
    else if (shot->size() < 2)
      ImGui::TextDisabled("skipped: fewer than 2 keyframes");
    else
      ImGui::TextDisabled("%.1f s", shot->back().TimeStamp);

    // Move up / down and remove, at the row's right end, as a keyframe's arrows
    if (!m_IsPlaying)
    {
      float arrowSize = ImGui::GetFrameHeight();
      ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - arrowSize * 3.f);
      if (ImGui::MoveArrowButton("##Up", ImGuiDir_Up, k > 0))
      {
        moveFrom = (int)k;
        moveTo = (int)k - 1;
      }
      ImGui::SameLine(0.f, 0.f);
      if (ImGui::MoveArrowButton("##Down", ImGuiDir_Down, k + 1 < m_Sequence.size()))
      {
        moveFrom = (int)k;
        moveTo = (int)k + 1;
      }
      ImGui::SameLine(0.f, 0.f);
      if (ImGui::Button("x##RemoveShot", ImVec2(arrowSize, arrowSize)))
        removeAt = (int)k;
      ImGui::ItemTooltip("Takes this shot out of the sequence (the track stays).");
    }
    ImGui::PopID();
  }
  if (moveFrom >= 0 && moveTo >= 0 && moveTo < (int)m_Sequence.size())
    std::swap(m_Sequence[moveFrom], m_Sequence[moveTo]);
  else if (removeAt >= 0)
    m_Sequence.erase(m_Sequence.begin() + removeAt);

  if (!m_IsPlaying)
  {
    // The next track after the last shot's (round to the first after the
    // last track); the first track for the first shot
    if (ImGui::Button("Add a shot"))
    {
      int next = 0;
      if (!m_Sequence.empty() && m_Sequence.back() >= 0 && m_Sequence.back() < (int)m_Tracks.size())
        next = (m_Sequence.back() + 1) % (int)m_Tracks.size();
      m_Sequence.push_back(next);
    }
    ImGui::ItemTooltip("Adds a shot with the next track after the last shot's. Pick another track for any shot from "
      "its list (a track can be in it more than once).");
    if (!m_Sequence.empty())
    {
      ImGui::SameLine();
      if (ImGui::ConfirmButton("Clear##Sequence"))
        m_Sequence.clear();
    }
  }
  if (cameraEnabled && !m_Sequence.empty())
  {
    if (ImGui::Button(IsSequencePlaying() ? "Stop the sequence" : (m_IsPlaying ? "Stop the track" : "Play the sequence")))
      PlaySequence();
    ImGui::SameLine();
  }
  ImGui::Checkbox("The play key plays the sequence", &m_SequenceOnKey);
  ImGui::HelpMarker("With this ticked (and shots in the sequence), the track's play key plays the whole sequence "
    "instead of the track picked above.");
  ImGui::TreePop();
}

void TrackPlayer::CreateTrack()
{
  if (m_IsPlaying) return;
  m_RenamingTrack = -1;

  m_Tracks.emplace_back("Track #" + std::to_string(m_RunningId++));
  m_SelectedTrack = (int)m_Tracks.size() - 1;

  UpdateNameList();
}

void TrackPlayer::DeleteTrack()
{
  if (m_IsPlaying || m_Tracks.size() <= 1) return;
  // Otherwise the rename box could open on the track that takes its place
  m_RenamingTrack = -1;

  int deleted = m_SelectedTrack;
  m_Tracks.erase(m_Tracks.begin() + m_SelectedTrack);
  if (m_SelectedTrack >= (int)m_Tracks.size())
    m_SelectedTrack -= 1;
  // The other tracks' places moved: the steps can't be undone any more, and
  // the sequence's shots of it go
  m_Undo.clear();
  m_Redo.clear();
  m_ShadowTrack = -1;
  for (size_t k = 0; k < m_Sequence.size();)
  {
    if (m_Sequence[k] == deleted)
      m_Sequence.erase(m_Sequence.begin() + k);
    else
    {
      if (m_Sequence[k] > deleted)
        --m_Sequence[k];
      ++k;
    }
  }

  UpdateNameList();
}

void TrackPlayer::UpdateNameList()
{
  // Must be rebuilt whenever m_Tracks changes, the strings can move
  m_TrackNames.clear();
  for (auto& track : m_Tracks)
    m_TrackNames.push_back(track.Name.c_str());
}
