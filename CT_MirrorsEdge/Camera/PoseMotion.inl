// The Pose window's layers over the pose as it's handed to the game each frame (MakeHandedPose, from KeepHeld):
// what the keyframes' playing adds (loose timing, follow-through, feet that stay put between keys), the roll and
// twist bones following their drivers, and the look. Included whole inside PoseEditor.cpp's anonymous namespace,
// after PoseKinematics.inl. Game thread, under g_Mutex (the motion trail and ghost poses' PlaceAt / PoseAt: the
// menu's, under it too).
//
// 2026-11-16, the user: "We also need to expand the kinematic system feels to basic and movements are robotic", then
// "do all" (suggestions 3, 4, 7, 8, 9 here). None of these change the pose as edited (h.Turn, h.Move: what the dots,
// the rings, Undo and the keyframes have); they're laid over it like the idle movement: the dots don't show them.

  const float LooseSecondsPerStep = 0.02f;    // loose timing at 1: each joint above the spine's first this much later
  const float MotionFadeSeconds = 0.25f;      // the playing layers fade in and out this fast (real time)
  const float FootStillDistance = 0.03f;      // m: a foot this near its place at the keys either side (and turned
  const float FootStillTurn = 10.f * KinPi / 180.f;   // less than this) is meant to stand: it's kept there between them
  const double FootLockShortest = 0.05;       // s: keys nearer than this aren't a stand
  const double FootLockEase = 0.15;           // s: the lock eases in after a stand's first key and out before its last
  const float LookFadeSeconds = 0.3f;
  const float MaxLookYaw = 70.f * KinPi / 180.f;      // the most the chest, neck and head turn about up to look,
  const float MaxLookPitch = 40.f * KinPi / 180.f;    // and up or down
  const float LookEase = 0.12f;                       // s: the look's turn follows the place this smoothly
  const float LookBehindFrom = 110.f * KinPi / 180.f; // a place this far round off the face's way: the look fades out,
  const float LookBehindTo = 150.f * KinPi / 180.f;   // gone at this (no swing from side to side behind the head)
  const float MaxEyeTurn = 35.f * KinPi / 180.f;      // the eyes' most from the face's way (the game's own limit)
  const double SpringStep = 1.0 / 240.0;              // s: follow-through's springs are stepped this finely

  // ---- Poses at other times: the keyframes sampled (no layers) ----

  // A bone's edit at a keyframe time: its keys there (laid: keyframes laid in over h.Keys for some bones, the changes
  // that will be keyframed, KinTrail; null: h.Keys alone), or the edit it has now (no keys)
  void EditAtIn(Held const& h, pose_keys::Anim const* laid, int bone, double time, float* turn, float* move)
  {
    pose_keys::Channel const* c = nullptr;
    if (h.Keys.On)
    {
      c = laid ? pose_keys::Find(*laid, bone) : nullptr;
      if (!c)
        c = pose_keys::Find(h.Keys, bone);
    }
    if (!c || !pose_keys::Sample(*c, time, turn, move, h.Keys.Loop ? h.Keys.Length : 0.0))
    {
      memcpy(turn, h.Turn[bone], 4 * sizeof(float));
      memcpy(move, h.Move[bone], 3 * sizeof(float));
    }
    QNorm(turn);
    float limit = MoveLimit(h, bone);
    for (int k = 0; k < 3; ++k)
      move[k] = !std::isfinite(move[k]) ? 0.f : (move[k] < -limit ? -limit : (move[k] > limit ? limit : move[k]));
  }

  void EditAt(Held const& h, int bone, double time, float* turn, float* move)
  {
    EditAtIn(h, nullptr, bone, time, turn, move);
  }

  void LocalFrom(Held const& h, int bone, float const* turn, float const* move, float* out)
  {
    float const* base = BaseOf(h, bone);
    memcpy(out, base, FloatsPerBone * sizeof(float));
    for (int k = 0; k < 3; ++k)
      out[k] += move[k];
    out[3] = 1.f;
    QMul(turn, base + 4, out + 4);
    QNorm(out + 4);
  }

  // A bone's pose at a keyframe time, from the held root down (frame: the held root's parent's; laid: as EditAtIn)
  void PlaceAtIn(Held const& h, pose_keys::Anim const* laid, int bone, double time, float const* frame, float* out)
  {
    int path[64];
    int count = 0;
    for (int i = bone; i >= 0 && i < h.Bones && count < 64 && h.InHeld[i]; i = h.Parents[i])
    {
      path[count++] = i;
      if (i == h.Root)
        break;
    }
    float world[FloatsPerBone];
    memcpy(world, frame, sizeof(world));
    for (int n = count - 1; n >= 0; --n)
    {
      float turn[4], move[3], local[FloatsPerBone], next[FloatsPerBone];
      EditAtIn(h, laid, path[n], time, turn, move);
      LocalFrom(h, path[n], turn, move, local);
      Compose(world, local, next);
      memcpy(world, next, sizeof(world));
    }
    memcpy(out, world, sizeof(world));
  }

  void PlaceAt(Held const& h, int bone, double time, float const* frame, float* out)
  {
    PlaceAtIn(h, nullptr, bone, time, frame, out);
  }

  // Every held bone's pose at a keyframe time (frame: the held root's parent's; laid: as EditAtIn)
  void PoseAtIn(Held const& h, pose_keys::Anim const* laid, double time, float const* frame, float (*out)[FloatsPerBone])
  {
    for (int n = 0; n < h.Count; ++n)
    {
      int i = (int)h.List[n], p = h.Parents[i];
      float turn[4], move[3], local[FloatsPerBone];
      EditAtIn(h, laid, i, time, turn, move);
      LocalFrom(h, i, turn, move, local);
      Compose(i == h.Root || p < 0 || !h.InHeld[p] ? frame : out[p], local, out[i]);
    }
  }

  // ---- The handed locals' own world poses ----

  // The held bones' poses from these locals, from frame (the held root's parent's)
  void ComposeLocals(Held const& h, float (*locals)[FloatsPerBone], float const* frame, float (*out)[FloatsPerBone])
  {
    for (int n = 0; n < h.Count; ++n)
    {
      int i = (int)h.List[n], p = h.Parents[i];
      Compose(i == h.Root || p < 0 || !h.InHeld[p] ? frame : out[p], locals[i], out[i]);
    }
  }

  // A handed local turned by q in the world (about its own place; world: from these locals)
  void TurnLocal(Held const& h, int bone, float const* q, float (*locals)[FloatsPerBone], float (*world)[FloatsPerBone],
    float const* frame)
  {
    int p = h.Parents[bone];
    float parent[4];
    memcpy(parent, (bone == h.Root || p < 0 || !h.InHeld[p] ? frame : world[p]) + 4, sizeof(parent));
    QNorm(parent);
    float inverse[4], a[4], b[4], c[4];
    QConj(parent, inverse);
    QMul(inverse, q, a);
    QMul(a, parent, b);
    QMul(b, locals[bone] + 4, c);
    QNorm(c);
    memcpy(locals[bone] + 4, c, sizeof(c));
  }

  // ---- The layers ----

  // 1 while the keyframes' time moves (played), eased to 0 when it stops (real time); the springs start afresh at 0
  void UpdateMotionWeight(Held& h)
  {
    bool moving = h.Keys.On && PlayingKeys(h) && h.KeysTimeMoved;
    float step = (float)g_ClipRealDt / MotionFadeSeconds;
    float weight = h.MotionWeight + (moving ? step : -step);
    h.MotionWeight = !(weight > 0.f) ? 0.f : (weight > 1.f ? 1.f : weight);
    if (h.MotionWeight <= 0.f)
      for (int i = 0; i < h.Bones; ++i)
        h.SpringOk[i] = false;
  }

  // Loose timing: a bone of the upper body from its keyframes a moment before the playhead, more the farther it is
  // from the spine's first bone (the chest leads, the head and arms come a little later, the hands later still; the
  // legs and the Hips never: the feet stay on their keys). False: not for this bone now.
  bool LooseLocal(Held const& h, int bone, float* local, float* turn)
  {
    if (!(h.MotionWeight > 0.f) || !(g_LooseTiming > 0.f) || !h.Keys.On || h.Kin.Steps[bone] == 0)
      return false;
    pose_keys::Channel const* c = pose_keys::Find(h.Keys, bone);
    if (!c)
      return false;
    double delay = (double)g_LooseTiming * LooseSecondsPerStep * h.Kin.Steps[bone] * h.MotionWeight;
    double length = h.Keys.Length, t = h.Keys.Time - delay;
    if (h.Keys.Loop && length > 0.0)
    {
      t = fmod(t, length);
      if (t < 0.0)
        t += length;
    }
    else if (t < 0.0)
      t = 0.0;
    float move[3];
    if (!pose_keys::Sample(*c, t, turn, move, h.Keys.Loop ? length : 0.0))
      return false;
    QNorm(turn);
    float limit = MoveLimit(h, bone);
    for (int k = 0; k < 3; ++k)
      move[k] = !std::isfinite(move[k]) ? 0.f : (move[k] < -limit ? -limit : (move[k] > limit ? limit : move[k]));
    LocalFrom(h, bone, turn, move, local);
    return true;
  }

  // The roll and twist bones follow their drivers (2026-11-16, laid last: after the feet, the look and the springs, so
  // they follow those too): an elbow or knee roll takes its forearm's or shin's whole turn (the game keeps them so),
  // Faith's twist bones their share of their driver's turn about their own X (her rules, as the clips have them). A
  // driver's turn: its edit as laid (turns: h.Turn, or loose timing's sample) with what the layers added to it since
  // (before: the locals before the layers). With "Roll and twist bones follow".
  void HelpersFollow(Held const& h, float (*locals)[FloatsPerBone], float (*before)[FloatsPerBone], float (*turns)[4])
  {
    KinRig const& k = h.Kin;
    auto driverTurn = [&](int d, float* out)
    {
      float inverse[4], added[4];
      QConj(before[d] + 4, inverse);
      QMul(locals[d] + 4, inverse, added);   // local = added x before (each layer turns a local in its parent's frame)
      QNorm(added);
      QMul(added, turns[d], out);
      QNorm(out);
    };
    for (int r = 0; r < k.RollCount; ++r)
    {
      int roll = k.Rolls[r][0], d = k.Rolls[r][1];
      float t[4], q[4];
      driverTurn(d, t);
      if (QIsIdentity(t))
        continue;
      QMul(t, locals[roll] + 4, q);
      QNorm(q);
      memcpy(locals[roll] + 4, q, sizeof(q));
    }
    for (int i = 0; i < k.TwistCount; ++i)
    {
      KinTwist const& rule = k.Twists[i];
      float t[4];
      driverTurn(rule.Driver, t);
      if (QIsIdentity(t))
        continue;
      // The driver's turn in its own frame (base^-1 x turn x base), its twist as the rule measures it
      float const* base = BaseOf(h, rule.Driver) + 4;
      float inverse[4], a[4], q[4];
      QConj(base, inverse);
      QMul(inverse, t, a);
      QMul(a, base, q);
      QNorm(q);
      float x = q[0], y = q[1], z = q[2], w = q[3], angle;
      if (rule.Kind == 0)
        angle = 2.f * atan2f(w < 0.f ? -x : x, fabsf(w));
      else if (rule.Kind == 1)
        angle = atan2f(2.f * (y * z + x * w), 1.f - 2.f * (x * x + y * y));
      else
        angle = atan2f(2.f * (x * w - y * z), 1.f - 2.f * (x * x + y * y));
      float half = 0.5f * rule.Share * angle, turn[4] = { sinf(half), 0.f, 0.f, cosf(half) }, out[4];
      QMul(locals[rule.Bone] + 4, turn, out);
      QNorm(out);
      memcpy(locals[rule.Bone] + 4, out, sizeof(out));
    }
  }

  // The latest key time at or before t, and the first after it, of the bones that place a bone (it and those above it
  // to the held root). False: none on a side, or they're nearer than FootLockShortest.
  bool KeysAround(Held const& h, int bone, double t, double& before, double& after)
  {
    bool b = false, a = false;
    for (int i = bone; i >= 0 && i < h.Bones && h.InHeld[i]; i = h.Parents[i])
    {
      pose_keys::Channel const* c = pose_keys::Find(h.Keys, i);
      if (c && !c->Keys.empty())
      {
        std::vector<pose_keys::Key>::const_iterator it = std::upper_bound(c->Keys.begin(), c->Keys.end(), t,
          [](double value, pose_keys::Key const& key) { return value < key.Time; });
        if (it != c->Keys.begin())
        {
          double k0 = (it - 1)->Time;
          if (!b || k0 > before)
            before = k0;
          b = true;
        }
        if (it != c->Keys.end())
        {
          double k1 = it->Time;
          if (!a || k1 < after)
            after = k1;
          a = true;
        }
      }
      if (i == h.Root)
        break;
    }
    return a && b && after - before > FootLockShortest;
  }

  // Feet stay put between keys: a foot as good as still at the keys either side of the playhead (FootStillDistance,
  // FootStillTurn: it's meant to stand) is kept on the line between those two places while the keyframes play, its leg
  // bent to it (its own bend plane kept, the knee's limits); eased in after the first key and out before the second
  // (FootLockEase: what else moves the foot, the idle movement, doesn't jump at the keys); not with a clip on (the pose
  // under the keys moves)
  void FootLock(Held& h, float (*locals)[FloatsPerBone], float const* frame, float (*world)[FloatsPerBone])
  {
    if (!g_FootLock || !(h.MotionWeight > 0.f) || h.Clip.On || !h.Keys.On)
      return;
    double t = h.Keys.Time;
    for (int l = Limb_LeftLeg; l <= Limb_RightLeg; ++l)
    {
      KinLimb const& limb = h.Kin.Limbs[l];
      double t0 = 0.0, t1 = 0.0;
      if (limb.Upper < 0 || !KeysAround(h, limb.End, t, t0, t1) || !(t >= t0 && t <= t1))
        continue;
      float a[FloatsPerBone], b[FloatsPerBone];
      PlaceAt(h, limb.End, t0, frame, a);
      PlaceAt(h, limb.End, t1, frame, b);
      float dot = fabsf(a[4] * b[4] + a[5] * b[5] + a[6] * b[6] + a[7] * b[7]);
      if (Distance3(a, b) > FootStillDistance || dot < cosf(0.5f * FootStillTurn))
        continue;
      float s = (float)((t - t0) / (t1 - t0));
      float place[3], turn[4];
      for (int k = 0; k < 3; ++k)
        place[k] = a[k] + (b[k] - a[k]) * s;
      QSlerp(a + 4, b + 4, s, turn);
      // Eased in after t0 unless the stretch before it stands too, out before t1 unless the one after does (a foot
      // standing over several keys doesn't dip at them)
      auto stands = [&](double at)
      {
        double s0 = 0.0, s1 = 0.0;
        if (!KeysAround(h, limb.End, at, s0, s1))
          return false;
        float p0[FloatsPerBone], p1[FloatsPerBone];
        PlaceAt(h, limb.End, s0, frame, p0);
        PlaceAt(h, limb.End, s1, frame, p1);
        return Distance3(p0, p1) <= FootStillDistance &&
          fabsf(p0[4] * p1[4] + p0[5] * p1[5] + p0[6] * p1[6] + p0[7] * p1[7]) >= cosf(0.5f * FootStillTurn);
      };
      double ease = (std::min)(FootLockEase, 0.25 * (t1 - t0));
      auto ramp = [&](double gone)
      {
        double e = gone / ease;
        return e >= 1.0 ? 1.0 : (e <= 0.0 ? 0.0 : e * e * (3.0 - 2.0 * e));
      };
      double inStart = t - t0 >= ease || stands(t0 - 2.0 * pose_keys::TimeEpsilon) ? 1.0 : ramp(t - t0);
      double inEnd = t1 - t >= ease || stands(t1 + 2.0 * pose_keys::TimeEpsilon) ? 1.0 : ramp(t1 - t);
      float in = (float)(std::min)(inStart, inEnd);
      float weight = h.MotionWeight * in;
      if (!(weight > 0.f))
        continue;
      ComposeLocals(h, locals, frame, world);
      float target[3], targetTurn[4];
      for (int k = 0; k < 3; ++k)
        target[k] = world[limb.End][k] + (place[k] - world[limb.End][k]) * weight;
      float now[4];
      memcpy(now, world[limb.End] + 4, sizeof(now));
      QNorm(now);
      QSlerp(now, turn, weight, targetTurn);
      if (Distance3(world[limb.End], target) > HoldSlack)
      {
        float hinge[3];
        bool hingeOk = g_JointHinges && LimbHingeWorld(limb, world, hinge);
        LimbSolve solve;
        SolveTwo(world[limb.Upper], world[limb.Mid], world[limb.End], target, hingeOk ? hinge : nullptr, false,
          g_JointHinges, solve);
        TurnLocal(h, limb.Upper, solve.TurnA, locals, world, frame);
        ComposeLocals(h, locals, frame, world);
        TurnLocal(h, limb.Mid, solve.TurnB, locals, world, frame);
        ComposeLocals(h, locals, frame, world);
      }
      float inverse[4], q[4];
      memcpy(now, world[limb.End] + 4, sizeof(now));
      QNorm(now);
      QConj(now, inverse);
      QMul(targetTurn, inverse, q);
      QNorm(q);
      TurnLocal(h, limb.End, q, locals, world, frame);
    }
  }

  // The way the eyes look in world (each eye to its iris, the eyes' middle): false without eyes
  bool EyesGaze(Held const& h, float (*world)[FloatsPerBone], float* middle, float* gaze)
  {
    KinRig const& k = h.Kin;
    if (k.EyeCount <= 0)
      return false;
    middle[0] = middle[1] = middle[2] = gaze[0] = gaze[1] = gaze[2] = 0.f;
    for (int e = 0; e < k.EyeCount; ++e)
    {
      float g[3] = { world[k.Iris[e]][0] - world[k.Eye[e]][0], world[k.Iris[e]][1] - world[k.Eye[e]][1],
        world[k.Iris[e]][2] - world[k.Eye[e]][2] };
      Normalize3(g);
      for (int i = 0; i < 3; ++i)
      {
        gaze[i] += g[i];
        middle[i] += world[k.Eye[e]][i] / (float)k.EyeCount;
      }
    }
    if (Length3(gaze) < 1e-5f)
      return false;
    Normalize3(gaze);
    return true;
  }

  // The face's own way and the eyes' middle in world, from the head (the rig's FaceGaze, FaceMiddle): false without
  // them
  bool FaceNow(Held const& h, float (*world)[FloatsPerBone], float* middle, float* facing)
  {
    KinRig const& k = h.Kin;
    if (!k.FaceOk || k.Head < 0)
      return false;
    float q[4], t[3];
    memcpy(q, world[k.Head] + 4, sizeof(q));
    QNorm(q);
    QRot(q, k.FaceGaze, facing);
    Normalize3(facing);
    QRot(q, k.FaceMiddle, t);
    for (int i = 0; i < 3; ++i)
      middle[i] = world[k.Head][i] + t[i];
    return true;
  }

  // Look_Point's place at first: 1.5 m in front of the face as posed (needs the pose's world)
  void PlaceLookPoint(Held& h)
  {
    static float w[MaxBones][FloatsPerBone];
    if (!h.WorldOk)
      return;
    ComposeHeld(h, w);
    float middle[3], facing[3];
    if (!FaceNow(h, w, middle, facing))
      return;
    for (int k = 0; k < 3; ++k)
      h.LookPoint[k] = middle[k] + facing[k] * 1.5f;
    h.LookPointOk = true;
  }

  // Look at: the chest, the neck and the head turned a share each towards the place (Amount: how much of the way they
  // take), then the eyes to it. The turn is measured from the face's own way (the head's: FaceNow) in the pose before
  // the voice (plain: before), so a voice's nods and glances ride on top of it: a yaw about the character's up (at
  // most MaxLookYaw) and a pitch across it (MaxLookPitch), eased (LookEase) so a moving camera or point doesn't jump
  // it; a place behind (LookBehindFrom - LookBehindTo off the face's way) fades the turn and the eyes out instead of
  // swinging them from one side to the other. The eyes: each at the place alone, or with a voice moving the eyes both
  // by one turn (its glances kept), at most MaxEyeTurn from the face's way. Every frame from the pose as it is, so it
  // keeps looking while the body moves. Needs the world (worldOk): the camera and the point are places in it.
  void LookAt(Held& h, float (*locals)[FloatsPerBone], float (*plain)[FloatsPerBone], float const* frame,
    float (*world)[FloatsPerBone], bool worldOk)
  {
    KinRig const& k = h.Kin;
    bool can = k.FaceOk && k.EyeCount > 0 && k.Head >= 0 && worldOk;
    if (can && h.LookMode == Look_Point && !h.LookPointOk)
    {
      // (placed where the world first is known: 1.5 m in front of the face)
      float middle[3], facing[3];
      ComposeLocals(h, plain, frame, world);
      if (FaceNow(h, world, middle, facing))
      {
        for (int i = 0; i < 3; ++i)
          h.LookPoint[i] = middle[i] + facing[i] * 1.5f;
        h.LookPointOk = true;
      }
    }
    float target[3];
    bool targetOk = false;
    if (can && h.LookMode == Look_Camera)
    {
      std::lock_guard<std::mutex> lock(g_ViewMutex);
      if (g_View.Ok)
      {
        target[0] = g_View.T[12];
        target[1] = g_View.T[13];
        target[2] = g_View.T[14];
        targetOk = true;
      }
    }
    else if (can && h.LookMode == Look_Point && h.LookPointOk)
    {
      memcpy(target, h.LookPoint, sizeof(target));
      targetOk = true;
    }
    if (targetOk)
    {
      memcpy(h.LookTarget, target, sizeof(target));
      h.LookTargetOk = true;
    }
    float step = (float)g_ClipRealDt / LookFadeSeconds;
    float weight = h.LookWeight + (targetOk ? step : -step);
    h.LookWeight = !(weight > 0.f) ? 0.f : (weight > 1.f ? 1.f : weight);
    if (!(h.LookWeight > 0.f) || !can || !h.LookTargetOk)
    {
      h.LookAngleOk = false;
      return;
    }
    memcpy(target, h.LookTarget, sizeof(target));
    // The face's way before the voice, and the character's up (the held root's parent's Y: the rest pose's up)
    float middle[3], facing[3], up[3], y[3] = { 0.f, 1.f, 0.f };
    ComposeLocals(h, plain, frame, world);
    if (!FaceNow(h, world, middle, facing))
      return;
    QRot(frame + 4, y, up);
    Normalize3(up);
    float want[3] = { target[0] - middle[0], target[1] - middle[1], target[2] - middle[2] };
    if (Length3(want) < 0.05f)
      return;
    Normalize3(want);
    float fu = facing[0] * up[0] + facing[1] * up[1] + facing[2] * up[2];
    float wu = want[0] * up[0] + want[1] * up[1] + want[2] * up[2];
    float fh[3] = { facing[0] - up[0] * fu, facing[1] - up[1] * fu, facing[2] - up[2] * fu };
    float wh[3] = { want[0] - up[0] * wu, want[1] - up[1] * wu, want[2] - up[2] * wu };
    if (Length3(fh) < 0.1f)
      return;   // (the face straight up or down: no way to turn about up from)
    float yaw = 0.f;
    if (Length3(wh) > 1e-4f)
    {
      float c[3];
      Cross(fh, wh, c);
      yaw = atan2f(c[0] * up[0] + c[1] * up[1] + c[2] * up[2], fh[0] * wh[0] + fh[1] * wh[1] + fh[2] * wh[2]);
    }
    float pitch = asinf(wu > 1.f ? 1.f : (wu < -1.f ? -1.f : wu)) - asinf(fu > 1.f ? 1.f : (fu < -1.f ? -1.f : fu));
    float off = fabsf(yaw);
    float ahead = off <= LookBehindFrom ? 1.f : (off >= LookBehindTo ? 0.f :
      (LookBehindTo - off) / (LookBehindTo - LookBehindFrom));
    yaw = yaw > MaxLookYaw ? MaxLookYaw : (yaw < -MaxLookYaw ? -MaxLookYaw : yaw);
    pitch = pitch > MaxLookPitch ? MaxLookPitch : (pitch < -MaxLookPitch ? -MaxLookPitch : pitch);
    yaw *= ahead;
    pitch *= ahead;
    if (!h.LookAngleOk)
    {
      h.LookYaw = yaw;
      h.LookPitch = pitch;
      h.LookAngleOk = true;
    }
    else
    {
      float ease = 1.f - expf(-(float)g_ClipRealDt / LookEase);
      h.LookYaw += (yaw - h.LookYaw) * ease;
      h.LookPitch += (pitch - h.LookPitch) * ease;
    }
    // The whole turn: about up by the yaw, then about the turned face's side by the pitch
    float qYaw[4], turned[3], side[3], qPitch[4], whole[4];
    QAxis(up, h.LookYaw, qYaw);
    QRot(qYaw, facing, turned);
    Cross(turned, up, side);
    if (Length3(side) < 1e-4f)
      return;
    Normalize3(side);
    QAxis(side, h.LookPitch, qPitch);
    QMul(qPitch, qYaw, whole);
    QNorm(whole);
    // The chain's shares (the head most), times Amount and the fade (a turn's direction is the same about any place:
    // the face turns by all the shares together)
    int bones[MaxKinChain * 2 + 1];
    float shares[MaxKinChain * 2 + 1];
    int count = 0;
    if (k.SpineCount > 0)
    {
      bones[count] = k.Spine[k.SpineCount - 1];
      shares[count++] = 0.12f;
    }
    for (int n = 0; n < k.NeckCount; ++n)
    {
      bones[count] = k.Neck[n];
      shares[count++] = 0.33f / (float)k.NeckCount;
    }
    bones[count] = k.Head;
    shares[count++] = 0.55f;
    float amount = !(h.LookAmount > 0.f) ? 0.f : (h.LookAmount > 1.f ? 1.f : h.LookAmount);
    float body = amount * h.LookWeight, all = 0.f;
    ComposeLocals(h, locals, frame, world);
    for (int n = 0; n < count; ++n)
    {
      float part[4];
      QPow(whole, shares[n] * body, part);
      all += shares[n] * body;
      TurnLocal(h, bones[n], part, locals, world, frame);
      ComposeLocals(h, locals, frame, world);
    }
    // The face's way now (before the voice, turned by the chain), and where the eyes are to look
    float bodyTurn[4], faceNow[3];
    QPow(whole, all, bodyTurn);
    QRot(bodyTurn, facing, faceNow);
    Normalize3(faceNow);
    auto limited = [&](float* way)
    {
      // way (unit) kept within MaxEyeTurn of the face's way
      float a = (float)acos((std::max)(-1.f, (std::min)(1.f, faceNow[0] * way[0] + faceNow[1] * way[1] +
        faceNow[2] * way[2])));
      if (a > MaxEyeTurn)
      {
        float q[4], r[4], back[3];
        FromTo(faceNow, way, q);
        QPow(q, MaxEyeTurn / a, r);
        QRot(r, faceNow, back);
        memcpy(way, back, 3 * sizeof(float));
      }
    };
    float eyes = h.LookWeight * ahead;
    bool voiceEyes = h.Voice.Sound && h.Voice.On && h.Voice.EyesNow;
    if (voiceEyes)
    {
      // Both eyes by the turn that takes the face's way to the place (the voice's glances about it kept)
      float eyesMiddle[3], gaze[3];
      if (!EyesGaze(h, world, eyesMiddle, gaze))
        return;
      float way[3] = { target[0] - eyesMiddle[0], target[1] - eyesMiddle[1], target[2] - eyesMiddle[2] };
      if (Length3(way) < 1e-4f)
        return;
      Normalize3(way);
      limited(way);
      float turn[4];
      FromTo(faceNow, way, turn);
      QPow(turn, eyes, turn);
      for (int e = 0; e < k.EyeCount; ++e)
        TurnLocal(h, k.Eye[e], turn, locals, world, frame);
      return;
    }
    for (int e = 0; e < k.EyeCount; ++e)
    {
      float g[3] = { world[k.Iris[e]][0] - world[k.Eye[e]][0], world[k.Iris[e]][1] - world[k.Eye[e]][1],
        world[k.Iris[e]][2] - world[k.Eye[e]][2] };
      float to[3] = { target[0] - world[k.Eye[e]][0], target[1] - world[k.Eye[e]][1], target[2] - world[k.Eye[e]][2] };
      if (Length3(g) < 1e-6f || Length3(to) < 1e-4f)
        continue;
      Normalize3(g);
      Normalize3(to);
      limited(to);
      float turn[4];
      FromTo(g, to, turn);
      QPow(turn, eyes, turn);
      TurnLocal(h, k.Eye[e], turn, locals, world, frame);
    }
  }

  // Follow-through: while the keyframes play, the arms (upper arm, forearm, hand) and the head lag behind where the
  // keys put them and settle, each on a spring about where its parent carries it (in the character's frame, so the game
  // moving the character doesn't swing them); stepped by the keyframes' own time (slow keys: slow springs); each joint
  // at most a little off. The head is left to a voice that moves it. Amount (g_FollowThrough): softer and further.
  void FollowThrough(Held& h, float (*locals)[FloatsPerBone], float (*world)[FloatsPerBone])
  {
    KinRig const& k = h.Kin;
    if (!(g_FollowThrough > 0.f) || !(h.MotionWeight > 0.f) || k.SprungCount <= 0)
    {
      for (int n = 0; n < k.SprungCount; ++n)
        h.SpringOk[k.Sprung[n]] = false;
      return;
    }
    float f = g_FollowThrough > 1.f ? 1.f : g_FollowThrough;
    float omega = 2.f * KinPi * (3.5f - 1.5f * f);     // 3.5 Hz at a little, 2 Hz at 1
    float zeta = 0.65f - 0.3f * f;                     // damped well at a little, an overshoot at 1
    float stiff = omega * omega, damp = 2.f * zeta * omega;
    float cap = (8.f + 12.f * f) * KinPi / 180.f;      // the most a joint lags its parent's carrying
    double dt = h.KeysDt < 0.0 ? 0.0 : (h.KeysDt > 0.1 ? 0.1 : h.KeysDt);
    int steps = dt > 0.0 ? (int)ceil(dt / SpringStep) : 0;
    float sub = steps > 0 ? (float)(dt / steps) : 0.f;
    bool voiceHead = h.Voice.Sound && h.Voice.On && (h.Voice.LifeHeadNow || h.Voice.FeelNow);
    float identity[FloatsPerBone] = { 0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 1.f };
    ComposeLocals(h, locals, identity, world);
    for (int n = 0; n < k.SprungCount; ++n)
    {
      int b = k.Sprung[n], p = h.Parents[b];
      if (b == k.Head && voiceHead)
      {
        h.SpringOk[b] = false;
        continue;
      }
      float want[4];
      memcpy(want, world[b] + 4, sizeof(want));
      QNorm(want);
      float* s = h.SpringTurn[b];
      float* v = h.SpringSpin[b];
      if (!h.SpringOk[b])
      {
        memcpy(s, want, sizeof(want));
        v[0] = v[1] = v[2] = 0.f;
        h.SpringOk[b] = true;
        continue;
      }
      for (int i = 0; i < steps; ++i)
      {
        float inverse[4], d[4], e[3];
        QConj(s, inverse);
        QMul(want, inverse, d);
        QLog(d, e);
        for (int c = 0; c < 3; ++c)
          v[c] += (stiff * e[c] - damp * v[c]) * sub;
        float spin[3] = { v[0] * sub, v[1] * sub, v[2] * sub }, r[4], next[4];
        QExp(spin, r);
        QMul(r, s, next);
        QNorm(next);
        memcpy(s, next, sizeof(next));
      }
      // At most cap off where its parent carries it (the speed kept with it: no swing back from past the cap)
      float inverse[4], off[4];
      QConj(want, inverse);
      QMul(s, inverse, off);
      float angle = TurnAngle(off);
      if (angle > cap)
      {
        float back[4];
        QSlerp(want, s, cap / angle, back);
        memcpy(s, back, sizeof(back));
        float shrink = cap / angle;
        for (int c = 0; c < 3; ++c)
          v[c] *= shrink;
      }
      float shown[4], parent[4], parentInverse[4], local[4];
      QSlerp(want, s, h.MotionWeight, shown);
      memcpy(parent, (b == h.Root || p < 0 || !h.InHeld[p] ? identity : world[p]) + 4, sizeof(parent));
      QNorm(parent);
      QConj(parent, parentInverse);
      QMul(parentInverse, shown, local);
      QNorm(local);
      memcpy(locals[b] + 4, local, sizeof(local));
      ComposeLocals(h, locals, identity, world);
    }
  }

  // This frame's locals to hand: the pose as edited (with loose timing, a keyed bone of the upper body from its
  // keyframes a moment before), the idle movement and the voice over it (AddIdle, AddVoice, as before), then the feet
  // kept between keys, the look and the follow-through, and last the roll and twist bones; into buffer (the override's
  // order) and h.Handed, and h.World's held bones from them when worldOk (h.World's other bones are this frame's
  // game's).
  void MakeHandedPose(Held& h, float* buffer, bool worldOk)
  {
    static float locals[MaxBones][FloatsPerBone], plain[MaxBones][FloatsPerBone], before[MaxBones][FloatsPerBone];
    static float turns[MaxBones][4], world[MaxBones][FloatsPerBone];
    UpdateMotionWeight(h);
    for (int n = 0; n < h.Count; ++n)
    {
      int i = (int)h.List[n];
      if (!LooseLocal(h, i, locals[i], turns[i]))
      {
        EditedLocal(h, i, locals[i]);
        memcpy(turns[i], h.Turn[i], sizeof(turns[i]));
      }
      AddIdle(h, i, locals[i]);
      memcpy(plain[i], locals[i], sizeof(plain[i]));   // (before the voice: the look measures from it)
      AddVoice(h, i, locals[i]);
    }
    if (h.Kin.Found)
    {
      for (int n = 0; n < h.Count; ++n)
      {
        int i = (int)h.List[n];
        memcpy(before[i], locals[i], sizeof(before[i]));
      }
      // The frame the held root hangs from: this frame's game's (the world), else none (the character's own frame)
      float frame[FloatsPerBone] = { 0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 1.f };
      bool frameOk = false;
      int p = h.Root >= 0 && h.Root < h.Bones ? h.Parents[h.Root] : -1;
      if (worldOk && p >= 0)
      {
        float const* f = h.World[p];
        float length = sqrtf(f[4] * f[4] + f[5] * f[5] + f[6] * f[6] + f[7] * f[7]);
        if (length > 0.5f && length < 1.5f && std::isfinite(f[0] + f[1] + f[2]))
        {
          memcpy(frame, f, sizeof(frame));
          QNorm(frame + 4);
          frameOk = true;
        }
      }
      FootLock(h, locals, frame, world);
      LookAt(h, locals, plain, frame, world, frameOk);
      FollowThrough(h, locals, world);
      if (g_HelpersFollow)
        HelpersFollow(h, locals, before, turns);
    }
    for (int n = 0; n < h.Count; ++n)
    {
      int i = (int)h.List[n], p = h.Parents[i];
      memcpy(buffer + n * FloatsPerBone, locals[i], FloatsPerBone * sizeof(float));
      memcpy(h.Handed[i], locals[i], FloatsPerBone * sizeof(float));
      if (worldOk && p >= 0)
        Compose(h.World[p], locals[i], h.World[i]);
    }
  }

  // ---- On the picture (DrawOverlay, the menu's thread) ----

  const int TrailSteps = 64;                  // the motion trail's points over the keyframes' length
  const int TrailDots = 120;                  // its keyframe dots at most

  // The held root's parent's world pose (h.World: this frame's game's); false if it isn't one
  bool HeldFrame(Held const& h, float* frame)
  {
    int p = h.Root >= 0 && h.Root < h.Bones ? h.Parents[h.Root] : -1;
    if (p < 0)
      return false;
    float const* f = h.World[p];
    float length = sqrtf(f[4] * f[4] + f[5] * f[5] + f[6] * f[6] + f[7] * f[7]);
    if (!(length > 0.5f && length < 1.5f) || !std::isfinite(f[0] + f[1] + f[2]))
      return false;
    memcpy(frame, f, FloatsPerBone * sizeof(float));
    QNorm(frame + 4);
    return true;
  }

  // ---- The picked bone's path over the keyframes, its dots dragged (2026-11-16, the user: "make it so that you are
  // able to move the dotted orange nodes of the movement"): DrawOverlay takes a dot like the bone's own, with the
  // playhead put at the dot's time first; let go after a drag, the change is keyframed there (KeyTrailDrag) ----

  struct TrailDot
  {
    double Time;
    float Place[3];
    ImVec2 At;                  // on the picture
    bool Shown;
  };
  struct KinTrail
  {
    bool Ok = false;            // shown: the bone is moved by keyframes (its own, or a bone's above it)
    int Bone = -1;
    float Frame[FloatsPerBone]; // the held root's parent's (HeldFrame)
    double Length = 0.0;        // the line's span
    pose_keys::Anim Laid;       // the channels of the bones whose changes will be keyframed at the playhead, with the
    bool LaidOk = false;        // keys they'll get laid in as they'll be made (MakeTrail's willKey); for the path and the
                                // ghost poses; not made (LaidOk false) when nothing will be keyframed
    std::vector<TrailDot> Dots; // one a keyframe time, thinned evenly to TrailDots
  };

  pose_keys::Anim const* LaidOf(KinTrail const& t)
  {
    return t.LaidOk ? &t.Laid : nullptr;
  }

  // Whether a bone's edit isn't as in a snapshot of the pose
  bool BoneDiffersFrom(Held const& h, Snapshot const& s, int bone)
  {
    if (s.Turns.size() != (size_t)h.Bones * 4 || s.Moves.size() != (size_t)h.Bones * 3)
      return true;
    for (int k = 0; k < 4; ++k)
      if (fabsf(h.Turn[bone][k] - s.Turns[(size_t)bone * 4 + k]) > 1e-6f)
        return true;
    for (int k = 0; k < 3; ++k)
      if (fabsf(h.Move[bone][k] - s.Moves[(size_t)bone * 3 + k]) > 1e-6f)
        return true;
    return false;
  }

  // A path dot pressed at a looping keyframes' end (0 s or their Length): the other end's time (-1: none). There the
  // drag is keyframed too for each bone that was the same at both ends, so the loop stays seamless (the two ends' dots
  // are one place on the path then).
  double LoopOtherEnd(Held const& h, double time)
  {
    if (!h.Keys.Loop || !(h.Keys.Length > pose_keys::TimeEpsilon))
      return -1.0;
    if (fabs(time) <= pose_keys::TimeEpsilon)
      return h.Keys.Length;
    if (fabs(time - h.Keys.Length) <= pose_keys::TimeEpsilon)
      return 0.0;
    return -1.0;
  }

  // Whether a bone's keyed edit is the same at two times (h.Keys as they are; a bone without keys always)
  bool SameEditAt(Held const& h, int bone, double a, double b)
  {
    float ta[4], ma[3], tb[4], mb[3];
    EditAt(h, bone, a, ta, ma);
    EditAt(h, bone, b, tb, mb);
    float dot = ta[0] * tb[0] + ta[1] * tb[1] + ta[2] * tb[2] + ta[3] * tb[3];
    if (fabsf(dot) < 1.f - 1e-6f)
      return false;
    for (int k = 0; k < 3; ++k)
      if (fabsf(ma[k] - mb[k]) > 1e-5f)
        return false;
    return true;
  }

  // The picked bone's path (bone: -1 none) as keyed (no layers), and where its dots are on the picture; none with a
  // clip on (the pose under the keys moves with it). willKey: the changes not keyframed yet will be keyframed at the
  // playhead (auto key, or a dragged path's dot let go): laid in as they'll be made (KeyBone; mirror >= 0: also at that
  // loop end, KeyTrailDrag), so the path, its dragged dot and the ghost poses follow a drag (only: just the bones
  // changed since this snapshot; null: all of them). The dots' times are the keyframes' own (and the playhead's when a
  // bone of the path is laid in): the hollow holds laid in don't add dots until they're made, so the dots don't change
  // under a drag. keepPlayhead: a dot at the playhead's time even where the dots are thinned (the one dragged).
  void MakeTrail(Held const& h, View const& view, int bone, bool willKey, Snapshot const* only, double mirror,
    bool keepPlayhead, KinTrail& out)
  {
    out.Ok = false;
    out.Bone = bone;
    out.Dots.clear();
    out.LaidOk = false;
    if (!h.WorldOk || !h.Keys.On || h.Keys.Channels.empty() || h.Clip.On || !HeldFrame(h, out.Frame))
      return;
    std::vector<int> mirroredBones;   // the bones laid in at the loop's other end too
    if (willKey && (g_ShowTrail || g_ShowGhosts))
    {
      int laid[MaxBones];
      bool mirrored[MaxBones];
      int count = 0;
      for (int n = 0; n < h.Count && count < MaxBones; ++n)
      {
        int b = (int)h.List[n];
        if (KeyRefDiffers(h, b) && (!only || BoneDiffersFrom(h, *only, b)))
        {
          mirrored[count] = mirror >= 0.0 && SameEditAt(h, b, h.Keys.Time, mirror);
          laid[count++] = b;
        }
      }
      if (count > 0)
      {
        out.Laid.Channels.clear();
        out.Laid.On = true;
        out.Laid.Loop = h.Keys.Loop;
        out.Laid.Length = h.Keys.Length;
        out.Laid.Time = h.Keys.Time;
        for (int k = 0; k < count; ++k)
        {
          pose_keys::Channel const* c = pose_keys::Find(h.Keys, laid[k]);
          if (c)
            out.Laid.Channels.push_back(*c);
        }
        std::sort(out.Laid.Channels.begin(), out.Laid.Channels.end(),
          [](pose_keys::Channel const& a, pose_keys::Channel const& b) { return a.Bone < b.Bone; });
        for (int k = 0; k < count; ++k)
        {
          KeyBoneIn(h, h.Keys, out.Laid, laid[k]);
          if (mirrored[k] && pose_keys::SetKey(out.Laid, laid[k], mirror, h.Turn[laid[k]], h.Move[laid[k]], -1) >= 0)
            mirroredBones.push_back(laid[k]);
        }
        out.LaidOk = true;
      }
    }
    if (!g_ShowTrail || bone < 0 || bone >= h.Bones || !h.InHeld[bone])
      return;
    int chain[64];
    int count = 0;
    for (int i = bone; i >= 0 && i < h.Bones && h.InHeld[i] && count < 64; i = h.Parents[i])
    {
      chain[count++] = i;
      if (i == h.Root)
        break;
    }
    std::vector<double> times;
    bool chainLaid = false, chainMirrored = false;
    for (int n = 0; n < count; ++n)
    {
      pose_keys::Channel const* c = pose_keys::Find(h.Keys, chain[n]);
      if (c)
        for (pose_keys::Key const& key : c->Keys)
          times.push_back(key.Time);
      if (out.LaidOk && pose_keys::Find(out.Laid, chain[n]))
        chainLaid = true;
      if (std::find(mirroredBones.begin(), mirroredBones.end(), chain[n]) != mirroredBones.end())
        chainMirrored = true;
    }
    if (chainLaid)
      times.push_back(h.Keys.Time);
    if (chainMirrored)
      times.push_back(mirror);   // (the second review: only when a bone of the path will be keyframed there)
    if (times.empty())
      return;   // nothing above it has keyframes: it doesn't move
    std::sort(times.begin(), times.end());
    out.Length = h.Keys.Length > 0.0 && std::isfinite(h.Keys.Length) ? h.Keys.Length : times.back();
    // (thinned evenly: ImGui 1.61's draw list takes 64k vertices, and a file can have thousands of keys)
    times.erase(std::unique(times.begin(), times.end(), [](double a, double b)
      { return b - a <= pose_keys::TimeEpsilon; }), times.end());
    size_t stride = times.size() > (size_t)TrailDots ? (times.size() + TrailDots - 1) / TrailDots : 1;
    bool playheadDot = false;
    auto addDot = [&](double t)
    {
      TrailDot d;
      d.Time = t;
      float place[FloatsPerBone], x = 0.f, y = 0.f;
      PlaceAtIn(h, LaidOf(out), bone, t, out.Frame, place);
      memcpy(d.Place, place, sizeof(d.Place));
      d.Shown = ToScreen(view, place, x, y);
      d.At = ImVec2(x, y);
      out.Dots.push_back(d);
      if (fabs(t - h.Keys.Time) <= pose_keys::TimeEpsilon)
        playheadDot = true;
    };
    for (size_t n = 0; n < times.size(); n += stride)
      addDot(times[n]);
    if (keepPlayhead && !playheadDot)
      for (double t : times)
        if (fabs(t - h.Keys.Time) <= pose_keys::TimeEpsilon)
        {
          addDot(t);
          break;
        }
    out.Ok = true;
  }

  // The pinned hands' and feet's squares
  void DrawKinPins(Held const& h, View const& view, ImDrawList* draw, float fontSize)
  {
    float frame[FloatsPerBone];
    if (!h.WorldOk || !HeldFrame(h, frame))
      return;
    static float w[MaxBones][FloatsPerBone];
    bool composed = false;
    for (int l = 0; l < LimbCount; ++l)
    {
      KinLimb const& limb = h.Kin.Limbs[l];
      if (!h.Pins[l].On || limb.End < 0)
        continue;
      if (!composed)
      {
        ComposeHeld(h, w);
        composed = true;
      }
      // Where it holds; or, moved by something else since its last hold (Undo, a reset, the keyframes), where its
      // end is now: the next edit takes it from there (StartHold)
      float now[3], t[3], place[3];
      PinLocal(h, l, w, now);
      float const* at = Distance3(now, h.Pins[l].Seen) > PinMovedBy ? now : h.Pins[l].Place;
      QRot(frame + 4, at, t);
      for (int k = 0; k < 3; ++k)
        place[k] = frame[k] + t[k];
      float x, y;
      if (!ToScreen(view, place, x, y))
        continue;
      float half = fontSize * 0.42f;
      draw->AddRect(ImVec2(x - half - 1.f, y - half - 1.f), ImVec2(x + half + 1.f, y + half + 1.f), IM_COL32(0, 0, 0, 160),
        0.f, 0, 4.f);
      draw->AddRect(ImVec2(x - half, y - half), ImVec2(x + half, y + half), IM_COL32(255, 90, 210, 235), 0.f, 0, 2.f);
    }
  }

  // The ghost poses (g_ShowGhosts, as keyed, with what will be keyframed laid in) and the picked bone's path
  // (MakeTrail's; hot: its dot under the cursor or being dragged, -1 none: drawn bigger, ringed)
  void DrawKinTrail(Held const& h, View const& view, ImDrawList* draw, float fontSize, KinTrail const& trail, int hot)
  {
    float frame[FloatsPerBone];
    if (!h.WorldOk || !HeldFrame(h, frame))
      return;
    bool keysUsable = h.Keys.On && !h.Keys.Channels.empty() && !h.Clip.On;
    if (!keysUsable)
      return;
    if (g_ShowGhosts)
    {
      static float ghost[MaxBones][FloatsPerBone];
      double times[2] = { 0.0, 0.0 };
      bool have[2] = { pose_keys::KeyTimeBefore(h.Keys, h.Keys.Time, times[0]),
        pose_keys::KeyTimeAfter(h.Keys, h.Keys.Time, times[1]) };
      const ImU32 colours[2] = { IM_COL32(110, 160, 255, 150), IM_COL32(110, 255, 160, 150) };
      for (int side = 0; side < 2; ++side)
      {
        if (!have[side])
          continue;
        PoseAtIn(h, LaidOf(trail), times[side], frame, ghost);
        for (int n = 0; n < h.Count; ++n)
        {
          int i = (int)h.List[n], p = h.Parents[i];
          if (p < 0 || !h.InHeld[p] || h.Face[i] || h.Finger[i] || h.Helper[i])
            continue;
          float ax, ay, bx, by;
          if (ToScreen(view, ghost[p], ax, ay) && ToScreen(view, ghost[i], bx, by))
            draw->AddLine(ImVec2(ax, ay), ImVec2(bx, by), colours[side], 1.5f);
        }
      }
    }
    if (!trail.Ok || trail.Bone < 0 || trail.Bone >= h.Bones)
      return;
    ImVec2 before(0.f, 0.f);
    bool beforeOk = false;
    for (int s = 0; s <= TrailSteps; ++s)
    {
      float place[FloatsPerBone], x = 0.f, y = 0.f;
      PlaceAtIn(h, LaidOf(trail), trail.Bone, trail.Length * s / TrailSteps, trail.Frame, place);
      bool ok = ToScreen(view, place, x, y);
      if (ok && beforeOk)
      {
        draw->AddLine(before, ImVec2(x, y), IM_COL32(0, 0, 0, 120), 3.5f);
        draw->AddLine(before, ImVec2(x, y), IM_COL32(255, 170, 60, 200), 2.f);
      }
      before = ImVec2(x, y);
      beforeOk = ok;
    }
    for (size_t n = 0; n < trail.Dots.size(); ++n)
    {
      TrailDot const& d = trail.Dots[n];
      if (!d.Shown)
        continue;
      bool isHot = (int)n == hot;
      float size = fontSize * (isHot ? 0.24f : 0.16f);
      draw->AddCircleFilled(d.At, size + 1.f, IM_COL32(0, 0, 0, 160), 12);
      draw->AddCircleFilled(d.At, size, IM_COL32(255, 170, 60, 240), 12);
      if (isHot)
        draw->AddCircle(d.At, size + fontSize * 0.14f, IM_COL32(255, 255, 255, 220), 16, 2.f);
    }
  }

  // Why a dot of bone's path can't be taken now (null: it can): the playhead can't go to its time (the camera track
  // holds the keyframes' time, their Track clock; past their Length: SetPlayhead would stop short), or (Auto key off)
  // going there would drop a change not keyframed yet of a bone with keyframes (KeepKeys puts such a bone back to its
  // keys when the time moves, as the timeline's own click does; here a click meant for a ring nearby would do it). At
  // the playhead's own time (Auto key off): not while a bone of the path with keyframes (the bone or one above it) has
  // such a change: the dot is where its keys put the bone, the bone isn't (the second review, 2026-11-16: a drag from
  // the dot then keyframed the bones it bent but left that change, so the bone went elsewhere once the time moved).
  char const* TrailRefusal(Held const& h, int bone, double time)
  {
    bool autoKeys = g_AutoKey && KeysCurrent(h);   // (SetPlayhead's CommitAutoKey / KeyTrailDrag keyframe them all)
    if (fabs(time - h.Keys.Time) <= pose_keys::TimeEpsilon)
    {
      if (!autoKeys)
        for (int i = bone, n = 0; i >= 0 && i < h.Bones && h.InHeld[i] && n < 64; i = h.Parents[i], ++n)
        {
          if (KeyRefDiffers(h, i) && pose_keys::Find(h.Keys, i) != nullptr)
            return "a bone of this path has a change not keyframed yet: Keyframe it or Undo it first";
          if (i == h.Root)
            break;
        }
      return nullptr;
    }
    if (TrackHoldsTime(h) && h.Keys.Playing)
      return "the camera track holds the time";
    if (!std::isfinite(time) || time < 0.0 || !(h.Keys.Length > 0.0) || time > h.Keys.Length)
      return "past the keyframes' Length";
    if (!autoKeys)
      for (int n = 0; n < h.Count; ++n)
      {
        int b = (int)h.List[n];
        if (KeyRefDiffers(h, b) && pose_keys::Find(h.Keys, b) != nullptr)
          return "a change isn't keyframed yet: Keyframe it or Undo it first";
      }
    return nullptr;
  }

  // A path's dot let go after a drag: what the drag changed keyframed at the playhead (the dot's time) at once, as
  // auto key would; with Auto key off too, then only the bones the drag changed (pressed: the pose when it was
  // pressed). The path is the keyframes': a dot that didn't keep its new place would take the drag back. mirror >= 0
  // (LoopOtherEnd): each of those bones that was the same at both of a loop's ends keyframed there the same. One Undo
  // step with the drag: auto key's merge (PushKeyUndo), and with Auto key off into the drag's own step too while it's
  // the last one (only the drag's bones are keyframed then: none of an older change goes with them).
  void KeyTrailDrag(Held& h, Snapshot const& pressed, double mirror)
  {
    if (!h.Keys.On || PlayingKeys(h) || !KeysCurrent(h))
      return;
    int bones[MaxBones];
    bool mirrored[MaxBones];
    int count = 0;
    for (int n = 0; n < h.Count && count < MaxBones; ++n)
    {
      int b = (int)h.List[n];
      if (KeyRefDiffers(h, b) && (g_AutoKey || BoneDiffersFrom(h, pressed, b)))
      {
        mirrored[count] = mirror >= 0.0 && SameEditAt(h, b, h.Keys.Time, mirror);
        bones[count++] = b;
      }
    }
    if (count == 0)
      return;
    if (!g_AutoKey && !h.History.empty())
    {
      HistoryStep const& last = h.History.back();
      if (last.Pose.Turns == pressed.Turns && last.Pose.Moves == pressed.Moves && last.Length == h.Keys.Length &&
          SameKeys(last.Keys, h.Keys.Channels))
        h.HistoryHasKeys = true;
    }
    KeyBones(h, bones, count, g_AutoKey, true);
    int mirrors = 0;
    for (int k = 0; k < count; ++k)
      if (mirrored[k] && pose_keys::SetKey(h.Keys, bones[k], mirror, h.Turn[bones[k]], h.Move[bones[k]], -1) >= 0)
        ++mirrors;
    if (mirrors > 0)
    {
      KeysMatchPose(h);
      size_t used = strlen(g_KeyNote);
      _snprintf_s(g_KeyNote + used, sizeof(g_KeyNote) - used, _TRUNCATE, "; at %.2f s too (the loop's other end: %d %s)",
        mirror, mirrors, mirrors == 1 ? "bone" : "bones");
    }
  }
