#include "BodycamMotion.h"
#include <cmath>
#include <cstring>

// See BodycamMotion.h. The numbers below are the tools' own design choices (tuned offline), not the game's data.
namespace
{
  const float Pi = 3.14159265358979f;
  const double MaxStep = 1.0 / 240.0;     // the springs are integrated in steps no longer than this
  const float MaxSlack = 0.12f;           // m: the strap never lets the camera further from its mount
  const float MaxDeviation = 1.2f;        // rad: how far from your aim the body's part may turn the view
  const float SnapDistance = 3.f;         // m in one frame: she was moved (respawn, a cutscene's end): start again
  const float SnapTurn = 1.2f;            // rad in one frame, and
  const float SnapTurnRate = 60.f;        // rad/s faster than any hand: the game's camera was cut
  const float MaxYawRate = 12.f;          // rad/s: the aim's turn rate the lean follows, at most
  const float MaxBodyLag = 0.8f;          // rad: the smoothed body never further than this behind her body
  const double MinDt = 1e-5;              // s: a shorter frame counts as none
  const float MinFallTime = 0.1f;         // s faster than 3 m/s down, and
  const float FallRefTime = 0.06f;        // (s into it: the speed it's measured from)
  const float MinFallGain = 0.3f;         // m/s sped up since: a real fall (not stairs, not a pop of her hips)
  const int FacingVotesNeeded = 15;       // frames the "front" sign is checked on before it's kept,
  const float FacingVoteTime = 0.25f;     // over at least this long
  // The jolts: a critically damped dip, x(t) = v t e^(-w t), at its deepest 1 / w after the kick (80 ms)
  const float KickRate = 12.5f;
  const float E = 2.718281828f;

  typedef bodycam_motion::Vec3 V;
  typedef bodycam_motion::Quat Qt;
  inline V Make(float const* p) { return { p[0], p[1], p[2] }; }
  inline V Add(V a, V b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
  inline V Sub(V a, V b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
  inline V Mul(V a, float s) { return { a.x * s, a.y * s, a.z * s }; }
  inline float Dot(V a, V b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
  inline V Cross(V a, V b) { return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; }
  inline float Length(V a) { return sqrtf(Dot(a, a)); }
  inline bool Finite(V a) { return std::isfinite(a.x) && std::isfinite(a.y) && std::isfinite(a.z); }
  inline bool Normalize(V& a)
  {
    float l = Length(a);
    if (!(l > 1e-6f) || !std::isfinite(l))
      return false;
    a = Mul(a, 1.f / l);
    return true;
  }
  inline float Clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
  inline float Lerp(float a, float b, float t) { return a + (b - a) * t; }

  inline Qt QMul(Qt a, Qt b)
  {
    return { a.w * b.x + b.w * a.x + a.y * b.z - a.z * b.y, a.w * b.y + b.w * a.y + a.z * b.x - a.x * b.z,
      a.w * b.z + b.w * a.z + a.x * b.y - a.y * b.x, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z };
  }
  inline Qt QConj(Qt q) { return { -q.x, -q.y, -q.z, q.w }; }
  inline Qt QNormalize(Qt q)
  {
    float l = sqrtf(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    if (!(l > 1e-8f))
      return { 0.f, 0.f, 0.f, 1.f };
    return { q.x / l, q.y / l, q.z / l, q.w / l };
  }
  // v rotated by q (q v q*)
  inline V Rotate(V v, Qt q)
  {
    V u = { q.x, q.y, q.z };
    V t = Mul(Cross(u, v), 2.f);
    return Add(Add(v, Mul(t, q.w)), Cross(u, t));
  }
  // The rotation vector (axis x angle) of q, and back
  inline V QLog(Qt q)
  {
    if (q.w < 0.f)
      q = { -q.x, -q.y, -q.z, -q.w };
    V v = { q.x, q.y, q.z };
    float s = Length(v);
    if (s < 1e-7f)
      return Mul(v, 2.f);
    float angle = 2.f * atan2f(s, q.w);
    return Mul(v, angle / s);
  }
  inline Qt QExp(V r)
  {
    float angle = Length(r);
    if (angle < 1e-7f)
      return QNormalize({ r.x * 0.5f, r.y * 0.5f, r.z * 0.5f, 1.f });
    float s = sinf(angle * 0.5f) / angle;
    return { r.x * s, r.y * s, r.z * s, cosf(angle * 0.5f) };
  }
  // The rotation taking the standard axes to x, y, z (an orthonormal, right-handed basis)
  Qt FromBasis(V x, V y, V z)
  {
    float m00 = x.x, m10 = x.y, m20 = x.z, m01 = y.x, m11 = y.y, m21 = y.z, m02 = z.x, m12 = z.y, m22 = z.z;
    float trace = m00 + m11 + m22;
    Qt q;
    if (trace > 0.f)
    {
      float s = sqrtf(trace + 1.f) * 2.f;
      q = { (m21 - m12) / s, (m02 - m20) / s, (m10 - m01) / s, 0.25f * s };
    }
    else if (m00 > m11 && m00 > m22)
    {
      float s = sqrtf(1.f + m00 - m11 - m22) * 2.f;
      q = { 0.25f * s, (m01 + m10) / s, (m02 + m20) / s, (m21 - m12) / s };
    }
    else if (m11 > m22)
    {
      float s = sqrtf(1.f + m11 - m00 - m22) * 2.f;
      q = { (m01 + m10) / s, 0.25f * s, (m12 + m21) / s, (m02 - m20) / s };
    }
    else
    {
      float s = sqrtf(1.f + m22 - m00 - m11) * 2.f;
      q = { (m02 + m20) / s, (m12 + m21) / s, 0.25f * s, (m10 - m01) / s };
    }
    return QNormalize(q);
  }
  // A camera basis looking along forward with up as its up (row 2 backwards, like the game's)
  bool CameraBasis(V forward, V up, Qt& q)
  {
    V z = Mul(forward, -1.f);
    if (!Normalize(z))
      return false;
    V y = Sub(up, Mul(z, Dot(up, z)));
    if (!Normalize(y))
      return false;
    V x = Cross(y, z);
    q = FromBasis(x, y, z);
    return true;
  }
  float Smooth(float value, float target, double dt, float tau)
  {
    return value + (target - value) * (1.f - expf(-(float)dt / tau));
  }
  // Smooth noise in about -1 .. 1: three sines at unrelated frequencies
  float Noise(float t, float const* freq, float const* phase)
  {
    return 0.5f * sinf(2.f * Pi * freq[0] * t + phase[0]) + 0.35f * sinf(2.f * Pi * freq[1] * t + phase[1]) +
      0.15f * sinf(2.f * Pi * freq[2] * t + phase[2]);
  }
  const float NoiseFreq[3][3] = { { 1.3f, 2.7f, 5.9f }, { 1.1f, 3.1f, 6.7f }, { 0.9f, 2.3f, 4.1f } };

  // In the Preset enum's order (Custom's place holds the chest's values, never used as a look of its own)
  const bodycam_motion::Settings g_Presets[bodycam_motion::Preset_Count] = {
    // Mount, Follow, Bounce, Inertia, Damping, Steps, Landing, Shake, Lean, Level, Forward, Up, Side, Fov, Fisheye,
    // Overlay, Vignette
    { bodycam_motion::Mount_Chest, 0.75f, 0.45f, 0.55f, 0.55f, 1.0f, 1.0f, 0.8f, 0.4f, 0.0f, 0.14f, 0.03f, 0.0f, 80.f, 0.30f, true, 0.35f },
    { bodycam_motion::Mount_Helmet, 0.55f, 0.25f, 0.35f, 0.65f, 0.6f, 0.8f, 0.5f, 0.25f, 0.0f, 0.04f, 0.07f, 0.06f, 85.f, 0.25f, false, 0.2f },
    { bodycam_motion::Mount_Shoulder, 0.8f, 0.4f, 0.5f, 0.55f, 0.8f, 1.0f, 0.7f, 0.35f, 0.0f, 0.06f, 0.07f, 0.02f, 80.f, 0.30f, true, 0.3f },
    { bodycam_motion::Mount_Helmet, 0.9f, 0.15f, 0.6f, 0.95f, 0.2f, 0.35f, 0.15f, 0.15f, 0.85f, 0.04f, 0.07f, 0.0f, 92.f, 0.45f, false, 0.0f },
    { bodycam_motion::Mount_Chest, 0.75f, 0.45f, 0.55f, 0.55f, 1.0f, 1.0f, 0.8f, 0.4f, 0.0f, 0.14f, 0.03f, 0.0f, 80.f, 0.30f, true, 0.35f },
    // GoPro (head mount): on her forehead, the widest field of view and the strongest fisheye, a stiff mount, the
    // shake and jolts damped and the horizon half levelled (as an action camera's stabilization does), no overlay
    { bodycam_motion::Mount_Helmet, 0.85f, 0.15f, 0.3f, 0.75f, 0.5f, 0.8f, 0.35f, 0.1f, 0.5f, 0.05f, 0.10f, 0.0f, 100.f, 0.60f, false, 0.1f },
  };
}

const char* bodycam_motion::MountName(int mount)
{
  static const char* names[Mount_Count] = { "Chest", "Helmet", "Shoulder" };
  return (mount >= 0 && mount < Mount_Count) ? names[mount] : names[0];
}

const char* bodycam_motion::PresetName(int preset)
{
  static const char* names[Preset_Count] = { "Chest bodycam", "Helmet cam", "Shoulder cam", "Action cam (steady)",
    "Custom", "GoPro (head mount)" };
  return (preset >= 0 && preset < Preset_Count) ? names[preset] : names[0];
}

bodycam_motion::Settings bodycam_motion::PresetSettings(int preset)
{
  return g_Presets[(preset >= 0 && preset < Preset_Count && preset != Preset_Custom) ? preset : 0];
}

int bodycam_motion::PresetInMenu(int index)
{
  static const int order[Preset_Count] = { Preset_Chest, Preset_Helmet, Preset_Shoulder, Preset_Action, Preset_GoPro,
    Preset_Custom };
  return (index >= 0 && index < Preset_Count) ? order[index] : Preset_Chest;
}

void bodycam_motion::Clamp(Settings& s)
{
  auto c = [](float& v, float lo, float hi, float fallback) { v = std::isfinite(v) ? Clampf(v, lo, hi) : fallback; };
  if (s.Mount < 0 || s.Mount >= Mount_Count)
    s.Mount = Mount_Chest;
  c(s.Follow, 0.f, 1.f, 0.75f);
  c(s.Bounce, 0.f, 1.f, 0.45f);
  c(s.Inertia, 0.f, 1.f, 0.55f);
  c(s.Damping, 0.2f, 1.2f, 0.55f);
  c(s.Steps, 0.f, 2.f, 1.f);
  c(s.Landing, 0.f, 2.f, 1.f);
  c(s.Shake, 0.f, 2.f, 0.8f);
  c(s.Lean, 0.f, 2.f, 0.4f);
  c(s.Level, 0.f, 1.f, 0.f);
  c(s.Forward, -0.1f, 0.4f, 0.14f);
  c(s.Up, -0.2f, 0.3f, 0.03f);
  c(s.Side, -0.25f, 0.25f, 0.f);
  c(s.Fov, 50.f, 120.f, 80.f);
  c(s.Fisheye, 0.f, 0.6f, 0.3f);
  c(s.Vignette, 0.f, 1.f, 0.35f);
}

bodycam_motion::Motion::Motion()
{
  Reset();
}

void bodycam_motion::Motion::Reset()
{
  memset(this, 0, sizeof(*this));
  m_LastAim = { 0.f, 0.f, 0.f, 1.f };
  // The noise's phases: fixed, so a run is the same each time
  for (int i = 0; i < 9; ++i)
    m_Phase[i] = 0.7f + 1.9f * (float)i;
}

bool bodycam_motion::Motion::Update(Input const& in, Settings const& settings, double dtIn, float out[16])
{
  Settings s = settings;
  Clamp(s);
  // A frame too short to measure counts as none (one that underflows as a float would poison the springs)
  double dt = (std::isfinite(dtIn) && dtIn >= MinDt) ? dtIn : 0.0;

  // The game's camera: its rows, made orthonormal (z backwards, y up); its row 0 is +-(y x z)
  V r0 = Make(in.Aim), r1 = Make(in.Aim + 4), r2 = Make(in.Aim + 8), aimPos = Make(in.Aim + 12);
  V za = r2;
  if (!Finite(r0) || !Finite(r1) || !Finite(r2) || !Finite(aimPos) || !Normalize(za))
    return false;
  V ya = Sub(r1, Mul(za, Dot(r1, za)));
  if (!Normalize(ya))
    return false;
  V xa = Cross(ya, za);
  float handed = Dot(xa, r0);
  if (!(fabsf(handed) > 0.5f))
    return false;
  float sign = handed > 0.f ? 1.f : -1.f;
  Qt aim = FromBasis(xa, ya, za);
  V view = Mul(za, -1.f);

  V hips = Make(in.Hips), neck = Make(in.Neck), lArm = Make(in.LeftArm), rArm = Make(in.RightArm);
  V spine2 = Make(in.Spine2), head = Make(in.Head), headEnd = Make(in.HeadEnd), lEye = Make(in.LeftEye),
    rEye = Make(in.RightEye), lFoot = Make(in.LeftFoot), rFoot = Make(in.RightFoot);
  V all[] = { hips, neck, lArm, rArm, spine2, head, headEnd, lEye, rEye, lFoot, rFoot };
  for (V const& p : all)
    if (!Finite(p))
      return false;

  // Her torso and head frames from places only: up, her right, and front = +-(up x right), the sign checked against
  // the game's view (she faces about where it looks) over her first frames
  V up = Sub(neck, hips);
  if (!Normalize(up))
    return false;
  V right = Sub(rArm, lArm);
  right = Sub(right, Mul(up, Dot(right, up)));
  if (!Normalize(right))
    return false;
  V front = Cross(up, right);
  float facing = Dot(front, view);
  if (m_Facing == 0)
  {
    m_VoteTime += (float)dt;
    if (fabsf(facing) > 0.3f && m_Votes < 4 * FacingVotesNeeded)
    {
      ++m_Votes;
      if (facing > 0.f)
        ++m_Agree;
    }
    if (m_Votes >= FacingVotesNeeded && m_VoteTime >= FacingVoteTime &&
      (m_Agree * 4 >= m_Votes * 3 || m_Agree * 4 <= m_Votes))
      m_Facing = (m_Agree * 2 >= m_Votes) ? 1 : -1;
  }
  float frontSign = m_Facing != 0 ? (float)m_Facing : (fabsf(facing) > 0.3f ? (facing > 0.f ? 1.f : -1.f) : 1.f);
  front = Mul(front, frontSign);

  V mountUp = up, mountRight = right, mountFront = front, base = spine2;
  if (s.Mount == Mount_Helmet)
  {
    V hUp = Sub(headEnd, head);
    V hRight = Sub(rEye, lEye);
    if (Normalize(hUp))
    {
      hRight = Sub(hRight, Mul(hUp, Dot(hRight, hUp)));
      if (Normalize(hRight))
      {
        mountUp = hUp;
        mountRight = hRight;
        mountFront = Mul(Cross(hUp, hRight), frontSign);
      }
    }
    base = Mul(Add(lEye, rEye), 0.5f);
  }
  else if (s.Mount == Mount_Shoulder)
    base = rArm;
  V target = Add(Add(Add(base, Mul(mountFront, s.Forward)), Mul(mountUp, s.Up)), Mul(mountRight, s.Side));
  Qt body;
  if (!CameraBasis(mountFront, mountUp, body))
    return false;

  // Start afresh: the first frame, a long frame, she was moved far, or the game's camera was cut (a turn no hand
  // makes in one frame)
  float aimTurn = m_Started ? Length(QLog(QMul(QConj(m_LastAim), aim))) : 0.f;
  bool snap = !m_Started || dt > 0.25 || Length(Sub(hips, m_LastHips)) > SnapDistance ||
    aimTurn > (SnapTurn > SnapTurnRate * (float)dt ? SnapTurn : SnapTurnRate * (float)dt);
  if (snap)
  {
    if (m_Started)
      ++m_Stats.Resets;
    m_Started = true;
    m_Offset = m_OffsetVel = m_TargetVel = { 0.f, 0.f, 0.f };
    m_VelKnown = false;
    m_LastTarget = target;
    m_BodyQ = m_LastBody = body;
    m_BodyW = { 0.f, 0.f, 0.f };
    m_Kick = m_KickVel = { 0.f, 0.f, 0.f };
    m_FastVy = m_TrendVy = 0.f;
    m_LastHips = hips;
    m_HipsVel = { 0.f, 0.f, 0.f };
    m_LeftFootY = lFoot.y;
    m_RightFootY = rFoot.y;
    m_LeftFootVel = m_RightFootVel = 0.f;
    m_LeftDown = m_RightDown = m_Falling = false;
    m_FallPeak = m_FallStart = m_FallTime = m_Burst = m_YawRate = m_Lean = 0.f;
    m_SinceStep = 1.f;
    m_LastAim = aim;
    dt = 0.0;
  }
  else if (dt <= 0.0)
  {
    // Paused (or frozen): nothing moves, but she may be moved or posed meanwhile: the camera stays on its mount (its
    // place is kept relative to it) and turns with her body; the movement's baselines follow her
    m_LastTarget = target;
    m_BodyQ = QNormalize(QMul(QMul(body, QConj(m_LastBody)), m_BodyQ));
    m_LastBody = body;
    m_LastHips = hips;
    m_LeftFootY = lFoot.y;
    m_RightFootY = rFoot.y;
    m_LastAim = aim;
  }

  if (dt > 0.0)
  {
    float fdt = (float)dt;
    // Her movement: the hips' speed, the turn of your aim, her feet
    V hipsVel = Mul(Sub(hips, m_LastHips), 1.f / fdt);
    m_HipsVel.x = Smooth(m_HipsVel.x, hipsVel.x, dt, 0.05f);
    m_HipsVel.y = Smooth(m_HipsVel.y, hipsVel.y, dt, 0.05f);
    m_HipsVel.z = Smooth(m_HipsVel.z, hipsVel.z, dt, 0.05f);
    m_LastHips = hips;
    float speed = sqrtf(m_HipsVel.x * m_HipsVel.x + m_HipsVel.z * m_HipsVel.z);
    m_Stats.Speed = speed;
    V turn = QLog(QMul(QConj(m_LastAim), aim));
    m_LastAim = aim;
    m_YawRate = Smooth(m_YawRate, Clampf(turn.y / fdt, -MaxYawRate, MaxYawRate), dt, 0.12f);

    // A fall: her hips going down faster than 3 m/s (lightly smoothed: a landing is felt the frame it happens). It's a
    // real fall (not stairs, not a pop of her hips) when it lasted and sped up, as falling does
    m_FastVy = Smooth(m_FastVy, hipsVel.y, dt, 0.012f);
    if (m_FastVy < -3.f)
    {
      if (!m_Falling)
      {
        m_Falling = true;
        m_FallStart = -1.f;
        m_FallPeak = 0.f;
        m_FallTime = 0.f;
      }
      m_FallTime += fdt;
      m_FallPeak = -m_FastVy > m_FallPeak ? -m_FastVy : m_FallPeak;
      // (its speed a moment in, when a sudden start has settled: falling keeps speeding up from there, stairs don't)
      if (m_FallStart < 0.f && m_FallTime >= FallRefTime)
        m_FallStart = -m_FastVy;
    }
    bool realFall = m_Falling && m_FallStart >= 0.f && m_FallTime >= MinFallTime &&
      m_FallPeak - m_FallStart >= MinFallGain;
    if (m_Falling && m_FastVy >= -3.f && m_FastVy > -0.4f * m_FallPeak)
    {
      // Most of its speed gone: landed
      m_Falling = false;
      float impact = m_FallPeak;
      if (realFall && impact < 40.f)
      {
        float k = s.Landing * Clampf((impact - 3.f) / 6.f, 0.f, 1.5f);
        float dip = Clampf(0.075f * k, 0.f, 0.25f);          // rad at its deepest
        m_KickVel.x -= dip * KickRate * E;                     // the view dips (about its own right)
        m_KickVel.z += ((m_Stats.Landings & 1) ? 0.15f : -0.15f) * dip * KickRate * E;
        m_OffsetVel.y -= 0.9f * k;                             // and the camera drops on its strap
        m_Burst = m_Burst > 0.035f * k ? m_Burst : 0.035f * k;
        ++m_Stats.Landings;
        m_Stats.LastImpact = impact;
        // (her feet coming down with it aren't a step too)
        m_LeftDown = m_RightDown = false;
        m_SinceStep = 0.f;
      }
      realFall = false;
    }
    // Steps: a foot coming down while she moves (not while she's in the air). Its speed is taken against her body's
    // slow up and down (stairs, slopes), not her steps' own bob
    m_TrendVy = Smooth(m_TrendVy, hipsVel.y, dt, 0.25f);
    m_SinceStep += fdt;
    float lv = (lFoot.y - m_LeftFootY) / fdt - m_TrendVy, rv = (rFoot.y - m_RightFootY) / fdt - m_TrendVy;
    m_LeftFootY = lFoot.y;
    m_RightFootY = rFoot.y;
    m_LeftFootVel = Smooth(m_LeftFootVel, lv, dt, 0.03f);
    m_RightFootVel = Smooth(m_RightFootVel, rv, dt, 0.03f);
    for (int foot = 0; foot < 2; ++foot)
    {
      float fv = foot == 0 ? m_LeftFootVel : m_RightFootVel;
      bool& down = foot == 0 ? m_LeftDown : m_RightDown;
      if (fv < -0.3f)
        down = true;
      else if (down && fv > -0.05f)
      {
        down = false;
        if (speed > 1.2f && m_SinceStep > 0.16f && !realFall)
        {
          m_SinceStep = 0.f;
          float k = s.Steps * Clampf(speed / 7.f, 0.4f, 1.4f);
          m_KickVel.x -= 0.012f * k * KickRate * E;
          m_KickVel.z += (foot == 0 ? 0.009f : -0.009f) * k * KickRate * E;   // her weight onto that foot
          m_OffsetVel.y -= 0.25f * k;
          ++m_Stats.Steps;
        }
      }
    }
    // Leaning into turns: the roll follows the aim's yaw rate (the same sign whichever way the game's row 0 points:
    // both are about the aim's own axes)
    float leanTarget = Clampf(s.Lean * 0.035f * m_YawRate * Clampf(speed / 6.f, 0.f, 1.3f), -0.25f, 0.25f);
    m_Lean = Smooth(m_Lean, leanTarget, dt, 0.15f);

    // The springs, in steps of at most MaxStep
    float wp = 2.f * Pi * Lerp(18.f, 3.f, s.Bounce);
    float wr = 2.f * Pi * Lerp(14.f, 2.f, s.Inertia);
    float zeta = s.Damping;
    int steps = (int)ceil(dt / MaxStep);
    if (steps < 1)
      steps = 1;
    float h = fdt / (float)steps;
    // The strap: the camera's offset from its mount, pushed by the mount's (smoothed) acceleration. At a steady speed
    // it stays on the mount whatever the frame times are; it swings when she speeds up, stops, steps or lands
    V rawVel = Mul(Sub(target, m_LastTarget), 1.f / fdt);
    V accel = { 0.f, 0.f, 0.f };
    if (m_VelKnown)
    {
      V before = m_TargetVel;
      m_TargetVel.x = Smooth(m_TargetVel.x, rawVel.x, dt, 0.05f);
      m_TargetVel.y = Smooth(m_TargetVel.y, rawVel.y, dt, 0.05f);
      m_TargetVel.z = Smooth(m_TargetVel.z, rawVel.z, dt, 0.05f);
      accel = Mul(Sub(m_TargetVel, before), 1.f / fdt);
    }
    else
    {
      m_TargetVel = rawVel;
      m_VelKnown = true;
    }
    m_LastTarget = target;
    // Her body's facing, smoothed: a rotation spring toward it, damped against its own turn (no lag in a steady
    // turn; it sways when she starts, stops or jolts)
    V bodyTurn = QLog(QMul(QConj(m_LastBody), body));
    V bodyRate = Mul(bodyTurn, 1.f / fdt);
    for (int k = 0; k < steps; ++k)
    {
      V a = Sub(Sub(Mul(m_Offset, -wp * wp), Mul(m_OffsetVel, 2.f * zeta * wp)), accel);
      m_OffsetVel = Add(m_OffsetVel, Mul(a, h));
      m_Offset = Add(m_Offset, Mul(m_OffsetVel, h));
      float l = Length(m_Offset);
      if (l > MaxSlack)
      {
        m_Offset = Mul(m_Offset, MaxSlack / l);
        // (no more speed outward)
        V n = Mul(m_Offset, 1.f / MaxSlack);
        float outward = Dot(m_OffsetVel, n);
        if (outward > 0.f)
          m_OffsetVel = Sub(m_OffsetVel, Mul(n, outward));
      }
      // (pulled toward where her body faces at the step's start: a body turning steadily is matched, not chased)
      Qt b0 = QMul(m_LastBody, QExp(Mul(bodyTurn, (float)k / (float)steps)));
      V e = QLog(QMul(QConj(m_BodyQ), b0));
      V aw = Sub(Mul(e, wr * wr), Mul(Sub(m_BodyW, bodyRate), 2.f * zeta * wr));
      m_BodyW = Add(m_BodyW, Mul(aw, h));
      m_BodyQ = QNormalize(QMul(m_BodyQ, QExp(Mul(m_BodyW, h))));
      V ak = Sub(Mul(m_Kick, -KickRate * KickRate), Mul(m_KickVel, 2.f * KickRate));
      m_KickVel = Add(m_KickVel, Mul(ak, h));
      m_Kick = Add(m_Kick, Mul(m_KickVel, h));
    }
    // Never further than MaxBodyLag behind her body
    V lag = QLog(QMul(QConj(m_BodyQ), body));
    float lagAngle = Length(lag);
    if (lagAngle > MaxBodyLag)
    {
      m_BodyQ = QNormalize(QMul(body, QExp(Mul(lag, -MaxBodyLag / lagAngle))));
      m_BodyW = bodyRate;
    }
    m_LastBody = body;
    m_Burst *= expf(-fdt / 0.3f);
    m_Time += fdt;
    if (m_Time > 3600.f)
      m_Time -= 3600.f;
  }

  // Shake: more while she runs, and after a landing
  float speed = m_Stats.Speed;
  float amount = s.Shake * (0.003f + 0.010f * Clampf(speed / 7.f, 0.f, 1.5f)) + m_Burst;
  V shake = { amount * Noise(m_Time, NoiseFreq[0], m_Phase), 0.8f * amount * Noise(m_Time, NoiseFreq[1], m_Phase + 3),
    0.6f * amount * Noise(m_Time, NoiseFreq[2], m_Phase + 6) };
  float shakePlace = s.Shake * 0.0015f * Clampf(speed / 7.f, 0.f, 1.5f);
  float unit = amount > 1e-6f ? 1.f / amount : 0.f;
  V placeShake = { shakePlace * shake.y * unit, shakePlace * shake.x * unit, shakePlace * shake.z * unit };

  // Where it looks: your aim, turned (1 - Follow) of the way toward where her (smoothed) body faces. Computed afresh
  // each frame: the mouse's part is immediate and nothing moves on after it stops. Near opposite ways (where the
  // turn toward her body isn't defined) it fades to your aim alone
  V dev = QLog(QMul(QConj(aim), m_BodyQ));
  float devAngle = Length(dev);
  dev = Mul(dev, (1.f - s.Follow) * Clampf((2.6f - devAngle) / 0.6f, 0.f, 1.f));
  float devLength = Length(dev);
  if (devLength > MaxDeviation)
    dev = Mul(dev, MaxDeviation / devLength);
  V turnBy = Add(Add(dev, m_Kick), shake);
  turnBy.z += m_Lean;
  Qt q = QNormalize(QMul(aim, QExp(turnBy)));
  // Level horizon: its roll against the world's up (+Y) taken out (a part of it); less of it looking near straight up
  // or down (where the roll isn't defined) and none near upside down (where it would flip)
  if (s.Level > 0.f)
  {
    V worldUp = Rotate({ 0.f, 1.f, 0.f }, QConj(q));
    float across = sqrtf(worldUp.x * worldUp.x + worldUp.y * worldUp.y);
    if (across > 1e-3f)
    {
      float roll = atan2f(worldUp.x, worldUp.y);
      float weight = s.Level * Clampf(across / 0.35f, 0.f, 1.f) * Clampf((2.8f - fabsf(roll)) / 0.6f, 0.f, 1.f);
      q = QNormalize(QMul(q, QExp({ 0.f, 0.f, -weight * roll })));
    }
  }
  V x = Rotate({ 1.f, 0.f, 0.f }, q), y = Rotate({ 0.f, 1.f, 0.f }, q), z = Rotate({ 0.f, 0.f, 1.f }, q);
  V place = Add(Add(target, m_Offset), placeShake);
  float result[16] = { sign * x.x, sign * x.y, sign * x.z, 0.f, y.x, y.y, y.z, 0.f, z.x, z.y, z.z, 0.f, place.x, place.y,
    place.z, 1.f };
  bool stateOk = Finite(m_Offset) && Finite(m_OffsetVel) && Finite(m_TargetVel) && Finite(m_BodyW) && Finite(m_Kick) &&
    Finite(m_KickVel) && std::isfinite(m_BodyQ.x) && std::isfinite(m_BodyQ.y) && std::isfinite(m_BodyQ.z) &&
    std::isfinite(m_BodyQ.w) && std::isfinite(m_Lean) && std::isfinite(m_Burst);
  for (float v : result)
    if (!std::isfinite(v))
      stateOk = false;
  if (!stateOk)
  {
    // (never seen offline; started afresh rather than carried)
    Stats keep = m_Stats;
    Reset();
    m_Stats = keep;
    ++m_Stats.Resets;
    return false;
  }
  memcpy(out, result, sizeof(result));
  m_Stats.Facing = m_Facing;
  m_Stats.FacingVotes = m_Votes;
  m_Stats.FacingAgree = m_Agree;
  return true;
}
