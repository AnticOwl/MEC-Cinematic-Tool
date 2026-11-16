// The Pose window's kinematics: how a dragged bone bends the bones above it, and what stays put meanwhile.
// Included whole inside PoseEditor.cpp's anonymous namespace, before TwoBone (it uses that file's maths, Held,
// ComposeHeld, ApplyWorldTurn, FromTo). Everything here runs under g_Mutex (the menu's thread, or the game's in
// DragWithWalls).
//
// 2026-11-16. The user (2026-11-15): "We also need to expand the kinematic system feels to basic and movements are
// robotic"; then "do all" of the twelve suggestions (PoseKeys_Work_2026-11-15\kinematics_suggestions.md). Here:
//   1  elbows and knees bend like real joints: about their hinge (from the game's rest pose, else the pose taken), to
//      their natural side, within limits (straight to MaxJointBend); a dragged hand or foot keeps its angle; the old
//      two-bone bend's side flipped when the drag passed it (a standing leg's foot lifted forward over the hip bent
//      the knee backwards): every two-bone bend now keeps its plane's normal instead
//   2  the feet stay where they stand when the Hips are moved or turned; a hand or foot can be pinned
//   5  the back and neck bend in a curve (a chest or head drag), not at one bone
//   6  the collarbone joins an arm raise
//   12 a hand dragged past the arm's reach bends the back towards it (off by default)
// The layers laid while the keyframes play, the helper bones and the look are in PoseMotion.inl.

  const float KinPi = 3.14159265f;
  const float MaxJointBend = 150.f * KinPi / 180.f;    // an elbow's or knee's most bend from straight
  const float StraightBend = 3.f * KinPi / 180.f;      // a joint bent less has no bend plane of its own
  const float CollarShareTick = 0.3f;                  // the collarbone's share of an arm's swing (the tick)
  const float CollarShareChain = 0.5f;                 // when "A dragged bone bends" takes the collarbone in
  const float MaxCollarTurn = 25.f * KinPi / 180.f;    // the most a collarbone is turned from the pose taken this way
  const float MaxFarReachBend = 35.f * KinPi / 180.f;  // the most the back bends for a far reach
  const float HoldSlack = 0.0005f;                     // m: a held hand or foot this near its place isn't solved again

  // q^t: the same turn, t times its angle (the shortest way)
  void QPow(float const* q, float t, float* out)
  {
    float a[4] = { q[0], q[1], q[2], q[3] };
    if (a[3] < 0.f)
      for (int k = 0; k < 4; ++k)
        a[k] = -a[k];
    float s = sqrtf(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
    if (!(s > 1e-7f) || !std::isfinite(t))
    {
      QIdentity(out);
      return;
    }
    float angle = 2.f * atan2f(s, a[3]) * t;
    float half = 0.5f * angle, k = sinf(half) / s;
    out[0] = a[0] * k;
    out[1] = a[1] * k;
    out[2] = a[2] * k;
    out[3] = cosf(half);
  }

  // From a to b by t (the shortest way; t past 0 - 1 carries on along the same turn)
  void QSlerp(float const* a, float const* b, float t, float* out)
  {
    float inverse[4], d[4], p[4];
    QConj(a, inverse);
    QMul(inverse, b, d);
    QNorm(d);
    QPow(d, t, p);
    QMul(a, p, out);
    QNorm(out);
  }

  // The turn's rotation vector (axis times angle, radians; the shortest way)
  void QLog(float const* q, float* out)
  {
    float a[4] = { q[0], q[1], q[2], q[3] };
    if (a[3] < 0.f)
      for (int k = 0; k < 4; ++k)
        a[k] = -a[k];
    float s = sqrtf(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
    if (!(s > 1e-7f))
    {
      out[0] = out[1] = out[2] = 0.f;
      return;
    }
    float k = 2.f * atan2f(s, a[3]) / s;
    out[0] = a[0] * k;
    out[1] = a[1] * k;
    out[2] = a[2] * k;
  }

  // The turn of a rotation vector
  void QExp(float const* v, float* out)
  {
    float angle = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (!(angle > 1e-9f) || !std::isfinite(angle))
    {
      QIdentity(out);
      return;
    }
    float k = sinf(0.5f * angle) / angle;
    out[0] = v[0] * k;
    out[1] = v[1] * k;
    out[2] = v[2] * k;
    out[3] = cosf(0.5f * angle);
  }

  // ---- The rig: limbs, spine, neck, eyes, helpers (found at Take) ----

  pose_rest::Rest const* RestOf(Held const& h)
  {
    for (int r = 0; r < pose_rest::RestCount; ++r)
      if (!_stricmp(pose_rest::Rests[r].Asset, h.Asset) && pose_rest::Rests[r].Bones == h.Bones)
        return &pose_rest::Rests[r];
    for (int r = 0; r < pose_rest::RestCount; ++r)
      if (!_stricmp(pose_rest::Rests[r].Fits, h.Asset) && pose_rest::Rests[r].Bones == h.Bones)
        return &pose_rest::Rests[r];
    return nullptr;
  }

  // The hinge of a joint at b (a to b to c): ab x bc (the side it bends to, by the right hand), unit, in the frame
  // whose rotation is qa. False: bent less than StraightBend (or folded flat).
  bool HingeOf(float const* pa, float const* pb, float const* pc, float const* qa, float* out)
  {
    float ab[3] = { pb[0] - pa[0], pb[1] - pa[1], pb[2] - pa[2] };
    float bc[3] = { pc[0] - pb[0], pc[1] - pb[1], pc[2] - pb[2] };
    float lab = Length3(ab), lbc = Length3(bc);
    if (lab < 1e-4f || lbc < 1e-4f)
      return false;
    float n[3];
    Cross(ab, bc, n);
    if (!(Length3(n) >= sinf(StraightBend) * lab * lbc))
      return false;
    float q[4] = { qa[0], qa[1], qa[2], qa[3] }, inverse[4];
    QNorm(q);
    QConj(q, inverse);
    QRot(inverse, n, out);
    Normalize3(out);
    return true;
  }

  // A limb's hinge in its upper bone's own frame: from the game's rest pose of this skeleton (its knees and elbows are
  // bent; the clips' hinges come from it too, PoseClips.cpp), else from the pose taken; none if both are straight
  void FindHinge(Held const& h, KinLimb& limb)
  {
    limb.HingeFrom = 0;
    pose_rest::Rest const* rest = RestOf(h);
    if (rest && HingeOf(rest->ModelT[limb.Upper], rest->ModelT[limb.Mid], rest->ModelT[limb.End],
        rest->ModelQ[limb.Upper], limb.Hinge))
    {
      limb.HingeFrom = 1;
      return;
    }
    float zero[3] = { 0.f, 0.f, 0.f }, identity[4] = { 0.f, 0.f, 0.f, 1.f }, mid[3], end[3], t[3];
    memcpy(mid, h.Base[limb.Mid], sizeof(mid));
    QRot(h.Base[limb.Mid] + 4, h.Base[limb.End], t);
    for (int k = 0; k < 3; ++k)
      end[k] = mid[k] + t[k];
    if (HingeOf(zero, mid, end, identity, limb.Hinge))
      limb.HingeFrom = 2;
  }

  void FindKinRig(Held& h)
  {
    KinRig& k = h.Kin;
    k = KinRig();
    auto bone = [&](char const* name) -> int
    {
      int b = BoneByName(h.Skeleton, name);
      return b >= 0 && b < h.Bones && h.InHeld[b] ? b : -1;
    };
    static char const* const limbs[LimbCount][4] = { { "LeftArm", "LeftForeArm", "LeftHand", "LeftShoulder" },
      { "RightArm", "RightForeArm", "RightHand", "RightShoulder" }, { "LeftUpLeg", "LeftLeg", "LeftFoot", nullptr },
      { "RightUpLeg", "RightLeg", "RightFoot", nullptr } };
    for (int l = 0; l < LimbCount; ++l)
    {
      KinLimb& limb = k.Limbs[l];
      int u = bone(limbs[l][0]), m = bone(limbs[l][1]), e = bone(limbs[l][2]);
      if (u < 0 || m < 0 || e < 0 || u == h.Root || h.Parents[m] != u || h.Parents[e] != m)
        continue;
      limb.Upper = u;
      limb.Mid = m;
      limb.End = e;
      limb.Arm = l < 2;
      if (limbs[l][3])
      {
        int c = bone(limbs[l][3]);
        if (c >= 0 && c != h.Root && h.Parents[u] == c)
          limb.Collar = c;
      }
      FindHinge(h, limb);
    }
    // The spine from the Hips up, the neck above it, the head
    static char const* const spine[] = { "Spine", "Spine1", "Spine2", "Spine3" };
    int below = h.Root;
    for (char const* name : spine)
    {
      int b = bone(name);
      if (b < 0 || h.Parents[b] != below || k.SpineCount >= MaxKinChain)
        break;
      k.Spine[k.SpineCount++] = b;
      below = b;
    }
    static char const* const neck[] = { "Neck", "Neck1", "Neck2" };
    below = k.SpineCount > 0 ? k.Spine[k.SpineCount - 1] : -2;
    for (char const* name : neck)
    {
      int b = bone(name);
      if (b < 0 || h.Parents[b] != below || k.NeckCount >= MaxKinChain)
        break;
      k.Neck[k.NeckCount++] = b;
      below = b;
    }
    int head = bone("Head");
    k.Head = head >= 0 && k.NeckCount > 0 && h.Parents[head] == below ? head : -1;
    // The eyes, each with its iris (its child): the look
    static char const* const eyes[2][2] = { { "LeftEye", "LeftIris" }, { "RightEye", "RightIris" } };
    for (int e = 0; e < 2; ++e)
    {
      int eye = bone(eyes[e][0]), iris = bone(eyes[e][1]);
      if (eye >= 0 && iris >= 0 && h.Parents[iris] == eye)
      {
        k.Eye[k.EyeCount] = eye;
        k.Iris[k.EyeCount] = iris;
        ++k.EyeCount;
      }
    }
    // The face's way and the eyes' middle in the head's own frame: the eyes' gaze (each eye to its iris) in the game's
    // rest pose (eyes ahead), else in the pose taken. The look turns the head from these, so the eyes' own movements (a
    // voice's glances, the eyes as posed) don't move it.
    if (k.Head >= 0 && k.EyeCount > 0)
    {
      pose_rest::Rest const* rest = RestOf(h);
      float gaze[3] = { 0.f, 0.f, 0.f }, middle[3] = { 0.f, 0.f, 0.f };
      int got = 0;
      for (int e = 0; e < k.EyeCount; ++e)
      {
        float eyeAt[3], irisAt[3];
        if (rest)
        {
          float inverse[4], q[4], d[3];
          memcpy(q, rest->ModelQ[k.Head], sizeof(q));
          QNorm(q);
          QConj(q, inverse);
          for (int i = 0; i < 3; ++i)
            d[i] = rest->ModelT[k.Eye[e]][i] - rest->ModelT[k.Head][i];
          QRot(inverse, d, eyeAt);
          for (int i = 0; i < 3; ++i)
            d[i] = rest->ModelT[k.Iris[e]][i] - rest->ModelT[k.Head][i];
          QRot(inverse, d, irisAt);
        }
        else
        {
          // The pose taken's locals from the head down to the eye (its own frame: the head at the origin, unturned)
          int path[16];
          int n = 0;
          bool reached = false;
          for (int i = k.Eye[e]; i >= 0 && n < 16; i = h.Parents[i])
          {
            if (i == k.Head)
            {
              reached = true;
              break;
            }
            path[n++] = i;
          }
          if (!reached)
            continue;
          float at[FloatsPerBone] = { 0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f, 1.f }, next[FloatsPerBone], iris[FloatsPerBone];
          for (int j = n - 1; j >= 0; --j)
          {
            Compose(at, h.Base[path[j]], next);
            memcpy(at, next, sizeof(at));
          }
          Compose(at, h.Base[k.Iris[e]], iris);
          memcpy(eyeAt, at, sizeof(eyeAt));
          memcpy(irisAt, iris, sizeof(irisAt));
        }
        float g[3] = { irisAt[0] - eyeAt[0], irisAt[1] - eyeAt[1], irisAt[2] - eyeAt[2] };
        if (Length3(g) < 1e-5f)
          continue;
        Normalize3(g);
        for (int i = 0; i < 3; ++i)
        {
          gaze[i] += g[i];
          middle[i] += eyeAt[i];
        }
        ++got;
      }
      if (got == k.EyeCount && Length3(gaze) > 1e-5f)
      {
        Normalize3(gaze);
        for (int i = 0; i < 3; ++i)
        {
          k.FaceGaze[i] = gaze[i];
          k.FaceMiddle[i] = middle[i] / (float)got;
        }
        k.FaceOk = true;
      }
    }
    // The elbow and knee rolls: siblings of the forearm and the shin with nothing below them, kept their turn by the
    // game (Faith's ElbowRoll bit for bit in 4,992 recorded frames, PoseClips.cpp)
    static char const* const rolls[4][2] = { { "LeftElbowRoll", "LeftForeArm" }, { "RightElbowRoll", "RightForeArm" },
      { "LeftKneeRoll", "LeftLeg" }, { "RightKneeRoll", "RightLeg" } };
    for (auto const& pair : rolls)
    {
      int roll = bone(pair[0]), d = bone(pair[1]);
      if (roll < 0 || d < 0 || h.Parents[roll] != h.Parents[d] || k.RollCount >= MaxKinRolls)
        continue;
      bool child = false;
      for (int c = 0; c < h.Bones && !child; ++c)
        child = h.Parents[c] == roll;
      if (child)
        continue;
      k.Rolls[k.RollCount][0] = roll;
      k.Rolls[k.RollCount][1] = d;
      ++k.RollCount;
    }
    // Faith's twist bones: a share of their driver's turn about their own X (her rules, fitted on her recordings,
    // PoseClips.cpp; the other skeletons' fit badly there: theirs stay as posed)
    if (!_stricmp(h.Asset, "characters/skeletons/skeleton_faith"))
    {
      struct Rule
      {
        char const* Bone;
        char const* Driver;
        unsigned char Kind;
        float Share;
      };
      static Rule const rules[] = { { "ForeArmRoll", "Hand", 0, 0.295f }, { "ForeArmRoll1", "Hand", 0, 0.645f },
        { "ForeArmRoll2", "Hand", 0, 0.974f }, { "ArmRoll", "Arm", 1, -0.715f }, { "ArmRoll1", "Arm", 1, -0.715f },
        { "UpLegRoll", "UpLeg", 2, -0.795f } };
      for (char const* side : { "Left", "Right" })
        for (Rule const& rule : rules)
        {
          char name[64], driver[64];
          _snprintf_s(name, sizeof(name), _TRUNCATE, "%s%s", side, rule.Bone);
          _snprintf_s(driver, sizeof(driver), _TRUNCATE, "%s%s", side, rule.Driver);
          int r = bone(name), d = bone(driver);
          if (r < 0 || d < 0 || k.TwistCount >= MaxKinTwists)
            continue;
          KinTwist& t = k.Twists[k.TwistCount++];
          t.Bone = r;
          t.Driver = d;
          t.Kind = rule.Kind;
          t.Share = rule.Share;
        }
    }
    // Loose timing's steps: how many joints a bone of the upper body is above the spine's first bone (the face's bones
    // all as many as the Face bone, so the face stays together; at most MaxLooseSteps)
    for (int n = 0; n < h.Count; ++n)
    {
      int i = (int)h.List[n], p = h.Parents[i];
      k.Steps[i] = 0;
      k.UpperBody[i] = false;
      if (k.SpineCount == 0 || i == h.Root || p < 0)
        continue;
      if (i == k.Spine[0])
      {
        k.UpperBody[i] = true;
        continue;
      }
      if (!k.UpperBody[p])
        continue;
      k.UpperBody[i] = true;
      if (h.Face[i] && h.Face[p])
        k.Steps[i] = k.Steps[p];
      else
        k.Steps[i] = (unsigned char)(k.Steps[p] < MaxLooseSteps ? k.Steps[p] + 1 : MaxLooseSteps);
    }
    // Follow-through's bones: the arms' (upper arm, forearm, hand) and the head, parents first
    for (int n = 0; n < h.Count; ++n)
    {
      int i = (int)h.List[n];
      bool sprung = i == k.Head;
      for (int l = 0; l < 2 && !sprung; ++l)
        sprung = k.Limbs[l].Upper >= 0 && (i == k.Limbs[l].Upper || i == k.Limbs[l].Mid || i == k.Limbs[l].End);
      if (sprung && k.SprungCount < MaxKinSprung)
        k.Sprung[k.SprungCount++] = i;
    }
    k.Found = true;
  }

  // A pose's kinematics as at Take: its rig found, no pins, the playing layers and the look off
  void ResetKinematics(Held& h)
  {
    FindKinRig(h);
    for (int l = 0; l < LimbCount; ++l)
      h.Pins[l] = KinPin();
    h.MotionWeight = 0.f;
    h.KeysDt = 0.0;
    for (int i = 0; i < MaxBones; ++i)
    {
      h.SpringOk[i] = false;
      QIdentity(h.SpringTurn[i]);
      h.SpringSpin[i][0] = h.SpringSpin[i][1] = h.SpringSpin[i][2] = 0.f;
    }
    h.LookMode = Look_Off;
    h.LookPoint[0] = h.LookPoint[1] = h.LookPoint[2] = 0.f;
    h.LookPointOk = false;
    h.LookAmount = 0.7f;
    h.LookWeight = 0.f;
    h.LookTarget[0] = h.LookTarget[1] = h.LookTarget[2] = 0.f;
    h.LookTargetOk = false;
    h.LookYaw = h.LookPitch = 0.f;
    h.LookAngleOk = false;
  }

  // The limb a bone is in (-1: none) and its place in it (0 the upper bone, 1 the elbow's or knee's, 2 the hand or foot)
  int LimbOf(Held const& h, int bone, int* role)
  {
    for (int l = 0; l < LimbCount; ++l)
    {
      KinLimb const& limb = h.Kin.Limbs[l];
      if (limb.Upper < 0)
        continue;
      int r = bone == limb.Upper ? 0 : (bone == limb.Mid ? 1 : (bone == limb.End ? 2 : -1));
      if (r >= 0)
      {
        if (role)
          *role = r;
        return l;
      }
    }
    return -1;
  }

  bool InSpineOrNeck(Held const& h, int bone)
  {
    for (int k = 0; k < h.Kin.SpineCount; ++k)
      if (h.Kin.Spine[k] == bone)
        return true;
    for (int k = 0; k < h.Kin.NeckCount; ++k)
      if (h.Kin.Neck[k] == bone)
        return true;
    return false;
  }

  // ---- Two bones bent so the third's place is the target ----

  struct LimbSolve
  {
    float TurnA[4];          // the upper bone's turn in the world (about its place)
    float TurnB[4];          // then the mid bone's (about its place, after TurnA)
  };

  // pa, pb, pc: the upper, mid and end bones' places; target: where the end is wanted.
  // hinge (world, unit; null: none): the upper bone's hinge. byHinge: the bend's plane is the hinge's and the upper
  // bone is rolled about its own length to it, so the mid bone only bends about its hinge (a dragged hand or foot);
  // else the plane the bones are bent in now is kept (a held hand or foot doesn't jump; with exactly the target it
  // was at, nothing turns), the hinge giving only the side when they're straight. Either way the plane's normal is
  // carried, not its side (the side of before 2026-11-16 flipped when the drag direction passed it).
  // limits: the joint bends at most MaxJointBend (eased over the last part), never more than it already is past that.
  void SolveTwo(float const* pa, float const* pb, float const* pc, float const* target, float const* hinge, bool byHinge,
    bool limits, LimbSolve& out)
  {
    QIdentity(out.TurnA);
    QIdentity(out.TurnB);
    float ab[3] = { pb[0] - pa[0], pb[1] - pa[1], pb[2] - pa[2] };
    float bc[3] = { pc[0] - pb[0], pc[1] - pb[1], pc[2] - pb[2] };
    float ac[3] = { pc[0] - pa[0], pc[1] - pa[1], pc[2] - pa[2] };
    float at[3] = { target[0] - pa[0], target[1] - pa[1], target[2] - pa[2] };
    float lab = Length3(ab), lbc = Length3(bc), lat = Length3(at), lac = Length3(ac);
    if (lab < 1e-4f || lbc < 1e-4f || lat < 1e-4f || !std::isfinite(lat))
      return;
    float d[3] = { at[0] / lat, at[1] / lat, at[2] / lat };
    float lo = fabsf(lab - lbc) + 1e-4f, hi = lab + lbc - 1e-4f;
    if (limits)
    {
      // The joint's inner angle at least 180 - MaxJointBend degrees: the end no nearer the upper bone than least
      float least = sqrtf((std::max)(0.f, lab * lab + lbc * lbc - 2.f * lab * lbc * cosf(KinPi - MaxJointBend)));
      float soft = (lab + lbc) / 30.f;
      float want = lat;
      if (lat < least + soft)
        lat = least + soft * expf((lat - least - soft) / soft);   // eased: the same slope at least + soft, never under least
      if (lac < least + soft)
        lat = (std::min)(lat, (std::max)(want, lac));               // already bent past the easing: no further
      lo = (std::max)(lo, (std::min)(least, lac));
    }
    lat = lat < lo ? lo : (lat > hi ? hi : lat);
    // The bend's plane's normal (ab x bc as the bones would be)
    float normal[3] = { 0.f, 0.f, 0.f };
    bool normalOk = false;
    if (hinge && byHinge)
    {
      memcpy(normal, hinge, sizeof(normal));
      normalOk = true;
    }
    else
    {
      float n[3];
      Cross(ab, bc, n);
      if (Length3(n) >= sinf(StraightBend) * lab * lbc)
      {
        memcpy(normal, n, sizeof(normal));
        normalOk = true;
      }
      else if (hinge)
      {
        memcpy(normal, hinge, sizeof(normal));
        normalOk = true;
      }
    }
    // The normal carried by the limb's swing (the shortest turn from where the end is to the way to the target), so it
    // turns with the drag and doesn't flip: projected straight onto the plane across d instead (the first version of
    // 2026-11-16, found by the review) it flipped where d passed the normal's own axis, e.g. a hand raised out to the
    // side past shoulder height. Only a drag straight back through the limb (d opposite ac) has no single swing.
    float bend[3] = { 0.f, 0.f, 0.f }, np[3] = { 0.f, 0.f, 0.f };
    bool bendOk = false;
    if (normalOk)
    {
      Normalize3(normal);
      float way[3] = { ac[0], ac[1], ac[2] };
      if (Length3(way) < 1e-5f)
        memcpy(way, d, sizeof(way));
      Normalize3(way);
      float swing[4], carried[3];
      FromTo(way, d, swing);
      QRot(swing, normal, carried);
      float along = carried[0] * d[0] + carried[1] * d[1] + carried[2] * d[2];
      for (int k = 0; k < 3; ++k)
        np[k] = carried[k] - d[k] * along;
      if (Length3(np) > 0.1f)
      {
        Normalize3(np);
        Cross(d, np, bend);   // b's side of the line from a to the target, the plane's normal carried
        Normalize3(bend);
        bendOk = true;
      }
    }
    if (!bendOk)
    {
      // No plane (straight, no hinge, or the target along the normal): b's side of the line as it is
      float acn[3] = { ac[0], ac[1], ac[2] };
      if (Length3(acn) < 1e-5f)
        memcpy(acn, d, sizeof(acn));
      Normalize3(acn);
      float along = ab[0] * acn[0] + ab[1] * acn[1] + ab[2] * acn[2];
      for (int k = 0; k < 3; ++k)
        bend[k] = ab[k] - acn[k] * along;
      float off = bend[0] * d[0] + bend[1] * d[1] + bend[2] * d[2];
      for (int k = 0; k < 3; ++k)
        bend[k] -= d[k] * off;
      if (Length3(bend) < 1e-5f)
        AnyPerpendicular(d, bend);
      Normalize3(bend);
    }
    float x = (lab * lab - lbc * lbc + lat * lat) / (2.f * lat);
    float height = sqrtf((std::max)(0.f, lab * lab - x * x));
    float b2[3], c2[3];
    for (int k = 0; k < 3; ++k)
    {
      b2[k] = pa[k] + d[k] * x + bend[k] * height;
      c2[k] = pa[k] + d[k] * lat;
    }
    float u[3] = { ab[0] / lab, ab[1] / lab, ab[2] / lab };
    float v[3] = { b2[0] - pa[0], b2[1] - pa[1], b2[2] - pa[2] };
    if (Length3(v) < 1e-6f)
      return;
    Normalize3(v);
    float turnA[4];
    FromTo(u, v, turnA);
    if (hinge && byHinge && bendOk)
    {
      // The upper bone rolled about its own length until its hinge is the plane's normal (the bones would bend
      // about it: ab x bc of the solved bones is along np)
      float now[3];
      QRot(turnA, hinge, now);
      float a1 = now[0] * v[0] + now[1] * v[1] + now[2] * v[2], a2 = np[0] * v[0] + np[1] * v[1] + np[2] * v[2];
      float p1[3] = { now[0] - v[0] * a1, now[1] - v[1] * a1, now[2] - v[2] * a1 };
      float p2[3] = { np[0] - v[0] * a2, np[1] - v[1] * a2, np[2] - v[2] * a2 };
      if (Length3(p1) > 1e-4f && Length3(p2) > 1e-4f)
      {
        float c[3];
        Cross(p1, p2, c);
        float angle = atan2f(c[0] * v[0] + c[1] * v[1] + c[2] * v[2], p1[0] * p2[0] + p1[1] * p2[1] + p1[2] * p2[2]);
        float roll[4], rolled[4];
        QAxis(v, angle, roll);
        QMul(roll, turnA, rolled);
        QNorm(rolled);
        memcpy(turnA, rolled, sizeof(turnA));
      }
    }
    memcpy(out.TurnA, turnA, sizeof(turnA));
    // The mid bone: where the end is after TurnA, turned to c2 about b2
    float pc1[3], off[3] = { pc[0] - pa[0], pc[1] - pa[1], pc[2] - pa[2] }, t[3];
    QRot(turnA, off, t);
    for (int k = 0; k < 3; ++k)
      pc1[k] = pa[k] + t[k];
    float now[3] = { pc1[0] - b2[0], pc1[1] - b2[1], pc1[2] - b2[2] };
    float want[3] = { c2[0] - b2[0], c2[1] - b2[1], c2[2] - b2[2] };
    if (Length3(now) < 1e-6f || Length3(want) < 1e-6f)
      return;
    Normalize3(now);
    Normalize3(want);
    FromTo(now, want, out.TurnB);
  }

  // The limb's hinge in the world now (w: the pose's world)
  bool LimbHingeWorld(KinLimb const& limb, float (*w)[FloatsPerBone], float* out)
  {
    if (limb.HingeFrom == 0 || limb.Upper < 0)
      return false;
    float q[4];
    memcpy(q, w[limb.Upper] + 4, sizeof(q));
    QNorm(q);
    QRot(q, limb.Hinge, out);
    Normalize3(out);
    return true;
  }

  // The limb's end (hand, foot) to the target by its upper and mid bones. byHinge: see SolveTwo (a dragged end);
  // the hinge and the limits only with "Elbows and knees bend like real joints"
  void SolveLimb(Held& h, KinLimb const& limb, float const* target, bool byHinge, float (*w)[FloatsPerBone])
  {
    ComposeHeld(h, w);
    float hinge[3];
    bool hingeOk = g_JointHinges && LimbHingeWorld(limb, w, hinge);
    LimbSolve s;
    SolveTwo(w[limb.Upper], w[limb.Mid], w[limb.End], target, hingeOk ? hinge : nullptr, byHinge, g_JointHinges, s);
    ApplyWorldTurn(h, limb.Upper, s.TurnA, w);
    ComposeHeld(h, w);
    ApplyWorldTurn(h, limb.Mid, s.TurnB, w);
  }

  // The bone turned so its world rotation is want
  void SetWorldTurn(Held& h, int bone, float const* want, float (*w)[FloatsPerBone])
  {
    ComposeHeld(h, w);
    float now[4], inverse[4], q[4];
    memcpy(now, w[bone] + 4, sizeof(now));
    QNorm(now);
    QConj(now, inverse);
    QMul(want, inverse, q);
    QNorm(q);
    ApplyWorldTurn(h, bone, q, w);
  }

  // ---- What stays put while another bone is edited: pins, and the feet when the Hips move ----

  // The frame a pin is kept in: the held root's parent's world pose (where the game has the character); the world's
  // own if it isn't one
  void PinFrame(Held const& h, float (*w)[FloatsPerBone], float* frame)
  {
    int p = h.Root >= 0 && h.Root < h.Bones ? h.Parents[h.Root] : -1;
    frame[0] = frame[1] = frame[2] = 0.f;
    frame[3] = 1.f;
    QIdentity(frame + 4);
    if (p < 0)
      return;
    float const* f = w[p];
    float length = sqrtf(f[4] * f[4] + f[5] * f[5] + f[6] * f[6] + f[7] * f[7]);
    if (!(length > 0.5f && length < 1.5f) || !std::isfinite(f[0] + f[1] + f[2]))
      return;
    memcpy(frame, f, FloatsPerBone * sizeof(float));
    QNorm(frame + 4);
  }

  // A pin taken where its hand or foot is in w (composed)
  void TakePin(Held& h, int l, float (*w)[FloatsPerBone])
  {
    KinLimb const& limb = h.Kin.Limbs[l];
    if (limb.End < 0)
      return;
    KinPin& pin = h.Pins[l];
    float frame[FloatsPerBone], inverse[4];
    PinFrame(h, w, frame);
    QConj(frame + 4, inverse);
    float d[3] = { w[limb.End][0] - frame[0], w[limb.End][1] - frame[1], w[limb.End][2] - frame[2] };
    QRot(inverse, d, pin.Place);
    memcpy(pin.Seen, pin.Place, sizeof(pin.Seen));
    QMul(inverse, w[limb.End] + 4, pin.Turn);
    QNorm(pin.Turn);
  }

  void PinWorld(Held const& h, int l, float (*w)[FloatsPerBone], float* place, float* turn)
  {
    KinPin const& pin = h.Pins[l];
    float frame[FloatsPerBone], t[3];
    PinFrame(h, w, frame);
    QRot(frame + 4, pin.Place, t);
    for (int k = 0; k < 3; ++k)
      place[k] = frame[k] + t[k];
    QMul(frame + 4, pin.Turn, turn);
    QNorm(turn);
  }

  struct KinHold
  {
    int Count = 0;
    int Limb[LimbCount];
    float Place[LimbCount][3];
    float Turn[LimbCount][4];
    bool Planted[LimbCount];  // a foot held because the Hips move (not a pin): let go past the leg's reach
    int Retake[LimbCount];   // pins in the edited bone's own limb: taken again where it ends up
    int RetakeCount = 0;
  };
  const float PlantedLetGo = 0.25f;   // m: a planted foot farther than its leg's length and this from its hip goes with
                                      // the Hips (a big move of the whole body: the Move field takes up to 100 km)

  // Where a limb's end is in a pin's frame (w composed)
  void PinLocal(Held const& h, int l, float (*w)[FloatsPerBone], float* out)
  {
    KinLimb const& limb = h.Kin.Limbs[l];
    float frame[FloatsPerBone], inverse[4];
    PinFrame(h, w, frame);
    QConj(frame + 4, inverse);
    float d[3] = { w[limb.End][0] - frame[0], w[limb.End][1] - frame[1], w[limb.End][2] - frame[2] };
    QRot(inverse, d, out);
  }

  const float PinMovedBy = 0.001f;   // m: a pinned end this far from where the last hold left it was moved by something
                                     // else (Undo, Redo, a reset, a load, the keyframes, a clip): its pin goes with it

  // Before an edit of bone (w: the pose before it, composed): the pinned hands and feet, and both feet when the Hips
  // are moved or turned (Feet stay where they stand; plantFeet false: the Hips' Move field, which moves the whole body);
  // a pin in the edited bone's own limb goes with it, and so does one whose end something else moved since its last hold
  void StartHold(Held& h, int bone, float (*w)[FloatsPerBone], KinHold& hold, bool plantFeet = true)
  {
    hold.Count = hold.RetakeCount = 0;
    for (int l = 0; l < LimbCount; ++l)
    {
      KinLimb const& limb = h.Kin.Limbs[l];
      if (limb.Upper < 0)
        continue;
      bool pinned = h.Pins[l].On;
      if (pinned)
      {
        float now[3];
        PinLocal(h, l, w, now);
        if (Distance3(now, h.Pins[l].Seen) > PinMovedBy)
          TakePin(h, l, w);
      }
      bool planted = !pinned && !limb.Arm && bone == h.Root && g_FeetPlanted && plantFeet;
      if (!pinned && !planted)
        continue;
      // (its own bones, and a toe or a finger's first bone: a toe's drag moves its foot)
      if (bone == limb.Upper || bone == limb.Mid || bone == limb.End || (bone >= 0 && h.Parents[bone] == limb.End))
      {
        if (pinned)
          hold.Retake[hold.RetakeCount++] = l;
        continue;
      }
      int n = hold.Count++;
      hold.Limb[n] = l;
      hold.Planted[n] = planted;
      if (pinned)
        PinWorld(h, l, w, hold.Place[n], hold.Turn[n]);
      else
      {
        memcpy(hold.Place[n], w[limb.End], sizeof(hold.Place[n]));
        memcpy(hold.Turn[n], w[limb.End] + 4, sizeof(hold.Turn[n]));
        QNorm(hold.Turn[n]);
      }
    }
  }

  // After it: the held ones solved back to their places and turns, the pins in the edited limb taken again
  void FinishHold(Held& h, KinHold const& hold, float (*w)[FloatsPerBone])
  {
    for (int n = 0; n < hold.Count; ++n)
    {
      KinLimb const& limb = h.Kin.Limbs[hold.Limb[n]];
      ComposeHeld(h, w);
      if (hold.Planted[n])
      {
        // Past the leg's reach (and PlantedLetGo more) the foot goes with the Hips, its leg as it was; up to there the
        // leg straightens towards it
        float full = Distance3(w[limb.Upper], w[limb.Mid]) + Distance3(w[limb.Mid], w[limb.End]);
        if (Distance3(w[limb.Upper], hold.Place[n]) > full + PlantedLetGo)
          continue;
      }
      if (Distance3(w[limb.End], hold.Place[n]) > HoldSlack)
        SolveLimb(h, limb, hold.Place[n], false, w);
      ComposeHeld(h, w);
      float const* q = w[limb.End] + 4;
      float const* want = hold.Turn[n];
      if (fabsf(q[0] * want[0] + q[1] * want[1] + q[2] * want[2] + q[3] * want[3]) < 0.9999999f)
        SetWorldTurn(h, limb.End, want, w);
    }
    ComposeHeld(h, w);
    for (int r = 0; r < hold.RetakeCount; ++r)
      TakePin(h, hold.Retake[r], w);
    // Where each held pin's end is now (it may fall short of its place): the next edit's StartHold compares
    for (int n = 0; n < hold.Count; ++n)
      if (h.Pins[hold.Limb[n]].On && !hold.Planted[n])
        PinLocal(h, hold.Limb[n], w, h.Pins[hold.Limb[n]].Seen);
  }

  // A bone's edit made by the menu (a ring, a slider) with what stays put kept: StartHold before it, FinishHold after.
  // Not while the keyframes play (their bones are put back each frame: a hold would fight them).
  void HoldAround(Held& h, int bone, std::function<void()> const& edit, bool plantFeet = true)
  {
    static float w[MaxBones][FloatsPerBone];
    if (!h.Kin.Found || bone < 0 || bone >= h.Bones || PlayingKeys(h))
    {
      edit();
      return;
    }
    ComposeHeld(h, w);
    KinHold hold;
    StartHold(h, bone, w, hold, plantFeet);
    edit();
    if (hold.Count > 0 || hold.RetakeCount > 0)
      FinishHold(h, hold, w);
  }

  // ---- The dragged bone's own solves ----

  // The collarbone turned by a share of the upper arm's swing from before (its direction then) to now, about its
  // own place, at most MaxCollarTurn from the pose taken (or as far as it was turned already)
  void CollarJoins(Held& h, KinLimb const& limb, float const* before, float share, float angleBefore,
    float (*w)[FloatsPerBone])
  {
    ComposeHeld(h, w);
    float after[3] = { w[limb.Mid][0] - w[limb.Upper][0], w[limb.Mid][1] - w[limb.Upper][1],
      w[limb.Mid][2] - w[limb.Upper][2] };
    if (Length3(after) < 1e-6f)
      return;
    Normalize3(after);
    float swing[4], part[4];
    FromTo(before, after, swing);
    QPow(swing, share, part);
    if (TurnAngle(part) < 1e-5f)
      return;
    ApplyWorldTurn(h, limb.Collar, part, w);
    float most = (std::max)(MaxCollarTurn, angleBefore);
    float angle = TurnAngle(h.Turn[limb.Collar]);
    if (angle > most && angle > 1e-6f)
      ScaleTurn(h.Turn[limb.Collar], most / angle, most);
  }

  // A far reach: the spine bent in a curve (its bones each the same share) so the shoulder goes extra metres towards
  // the target, at most MaxFarReachBend in all
  void BendBackToward(Held& h, KinLimb const& limb, float const* target, float extra, float (*w)[FloatsPerBone])
  {
    KinRig const& k = h.Kin;
    if (k.SpineCount <= 0)
      return;
    ComposeHeld(h, w);
    float way[3] = { target[0] - w[limb.Upper][0], target[1] - w[limb.Upper][1], target[2] - w[limb.Upper][2] };
    if (Length3(way) < 1e-5f)
      return;
    Normalize3(way);
    float want[3];
    for (int i = 0; i < 3; ++i)
      want[i] = w[limb.Upper][i] + way[i] * extra;
    float bent = 0.f;
    for (int pass = 0; pass < 4 && bent < MaxFarReachBend - 1e-4f; ++pass)
    {
      ComposeHeld(h, w);
      float const* base = w[k.Spine[0]];
      float u[3] = { w[limb.Upper][0] - base[0], w[limb.Upper][1] - base[1], w[limb.Upper][2] - base[2] };
      float v[3] = { want[0] - base[0], want[1] - base[1], want[2] - base[2] };
      if (Length3(u) < 1e-4f || Length3(v) < 1e-4f)
        break;
      Normalize3(u);
      Normalize3(v);
      float r[4];
      FromTo(u, v, r);
      float angle = TurnAngle(r);
      if (angle < 1e-4f)
        break;
      if (bent + angle > MaxFarReachBend)
      {
        ScaleTurn(r, (MaxFarReachBend - bent) / angle, MaxFarReachBend);
        angle = MaxFarReachBend - bent;
      }
      bent += angle;
      float part[4];
      QPow(r, 1.f / (float)k.SpineCount, part);
      for (int j = 0; j < k.SpineCount; ++j)
      {
        ComposeHeld(h, w);
        ApplyWorldTurn(h, k.Spine[j], part, w);
      }
    }
  }

  // A hand or a foot dragged to the target (count: the bones "A dragged bone bends" takes in, 2 or more): the limb
  // about its hinge; an arm's collarbone takes a share of the arm's swing (Collarbone joins an arm raise, or the 3 bones
  // above the hand taken in); past the arm's reach the back bends towards it (A far reach bends the back)
  void ReachLimb(Held& h, int l, float const* target, int count, float (*w)[FloatsPerBone])
  {
    KinLimb const& limb = h.Kin.Limbs[l];
    ComposeHeld(h, w);
    float before[3] = { w[limb.Mid][0] - w[limb.Upper][0], w[limb.Mid][1] - w[limb.Upper][1],
      w[limb.Mid][2] - w[limb.Upper][2] };
    Normalize3(before);
    float collarBefore = limb.Collar >= 0 ? TurnAngle(h.Turn[limb.Collar]) : 0.f;
    SolveLimb(h, limb, target, true, w);
    if (limb.Arm && limb.Collar >= 0 && (count >= 3 || g_CollarFollows))
    {
      CollarJoins(h, limb, before, count >= 3 ? CollarShareChain : CollarShareTick, collarBefore, w);
      SolveLimb(h, limb, target, true, w);
    }
    if (limb.Arm && g_FarReach && h.Kin.SpineCount > 0)
    {
      ComposeHeld(h, w);
      float full = Distance3(w[limb.Upper], w[limb.Mid]) + Distance3(w[limb.Mid], w[limb.End]);
      float reach = Distance3(w[limb.Upper], target);
      if (reach > full + 1e-3f)
      {
        BendBackToward(h, limb, target, reach - full, w);
        SolveLimb(h, limb, target, true, w);
      }
    }
  }

  // An elbow or a knee dragged: the upper bone aims it at the target (its forearm or shin goes with it); an arm's
  // collarbone takes a share (Collarbone joins an arm raise, or the collarbone taken in: 2 bones or more)
  void AimLimbJoint(Held& h, int l, float const* target, int count, float (*w)[FloatsPerBone])
  {
    KinLimb const& limb = h.Kin.Limbs[l];
    ComposeHeld(h, w);
    float before[3] = { w[limb.Mid][0] - w[limb.Upper][0], w[limb.Mid][1] - w[limb.Upper][1],
      w[limb.Mid][2] - w[limb.Upper][2] };
    Normalize3(before);
    float collarBefore = limb.Collar >= 0 ? TurnAngle(h.Turn[limb.Collar]) : 0.f;
    auto aim = [&]()
    {
      ComposeHeld(h, w);
      float u[3] = { w[limb.Mid][0] - w[limb.Upper][0], w[limb.Mid][1] - w[limb.Upper][1], w[limb.Mid][2] - w[limb.Upper][2] };
      float v[3] = { target[0] - w[limb.Upper][0], target[1] - w[limb.Upper][1], target[2] - w[limb.Upper][2] };
      if (Length3(u) < 1e-4f || Length3(v) < 1e-4f)
        return;
      Normalize3(u);
      Normalize3(v);
      float q[4];
      FromTo(u, v, q);
      ApplyWorldTurn(h, limb.Upper, q, w);
    };
    aim();
    if (limb.Arm && limb.Collar >= 0 && (count >= 2 || g_CollarFollows))
    {
      CollarJoins(h, limb, before, count >= 2 ? CollarShareChain : CollarShareTick, collarBefore, w);
      aim();
    }
  }

  // A toe (a foot's child) with 3 bones or more taken in: the foot moved by as much as the toe has to go, the leg about
  // its knee, the foot keeping its turn; then the foot turned to point the toe at the target
  void ReachToe(Held& h, int l, int toe, float const* target, float (*w)[FloatsPerBone])
  {
    KinLimb const& limb = h.Kin.Limbs[l];
    ComposeHeld(h, w);
    float footTurn[4];
    memcpy(footTurn, w[limb.End] + 4, sizeof(footTurn));
    QNorm(footTurn);
    float place[3];
    for (int k = 0; k < 3; ++k)
      place[k] = w[limb.End][k] + target[k] - w[toe][k];
    SolveLimb(h, limb, place, true, w);
    SetWorldTurn(h, limb.End, footTurn, w);
    ComposeHeld(h, w);
    float u[3] = { w[toe][0] - w[limb.End][0], w[toe][1] - w[limb.End][1], w[toe][2] - w[limb.End][2] };
    float v[3] = { target[0] - w[limb.End][0], target[1] - w[limb.End][1], target[2] - w[limb.End][2] };
    if (Length3(u) < 1e-4f || Length3(v) < 1e-4f)
      return;
    Normalize3(u);
    Normalize3(v);
    float q[4];
    FromTo(u, v, q);
    ApplyWorldTurn(h, limb.End, q, w);
  }

  // The spine or the neck bent in a curve so the dragged bone above them goes towards the target: each of the chain's
  // bones turned the same share, about its own place, again until it points there from the chain's foot (chain[0] is
  // the bone just above the dragged one, chain[count - 1] the lowest)
  void CurveChain(Held& h, int bone, int const* chain, int count, float const* target, float (*w)[FloatsPerBone])
  {
    for (int pass = 0; pass < 8; ++pass)
    {
      ComposeHeld(h, w);
      float const* base = w[chain[count - 1]];
      float u[3] = { w[bone][0] - base[0], w[bone][1] - base[1], w[bone][2] - base[2] };
      float v[3] = { target[0] - base[0], target[1] - base[1], target[2] - base[2] };
      if (Length3(u) < 1e-4f || Length3(v) < 1e-4f)
        break;
      Normalize3(u);
      Normalize3(v);
      float r[4];
      FromTo(u, v, r);
      if (TurnAngle(r) < 1e-4f)
        break;
      float part[4];
      QPow(r, 1.f / (float)count, part);
      for (int k = count - 1; k >= 0; --k)
      {
        ComposeHeld(h, w);
        ApplyWorldTurn(h, chain[k], part, w);
      }
    }
  }

  // A limb's elbow or knee swung about the line from its upper bone to its end by angle (radians): the hand or foot
  // stays where it is, its turn kept (the Bone tab's Elbow / Knee direction)
  void SwivelLimb(Held& h, int l, float angle)
  {
    static float w[MaxBones][FloatsPerBone];
    KinLimb const& limb = h.Kin.Limbs[l];
    if (limb.Upper < 0 || !(fabsf(angle) > 1e-7f) || !std::isfinite(angle))
      return;
    ComposeHeld(h, w);
    float axis[3] = { w[limb.End][0] - w[limb.Upper][0], w[limb.End][1] - w[limb.Upper][1],
      w[limb.End][2] - w[limb.Upper][2] };
    if (Length3(axis) < 1e-4f)
      return;
    Normalize3(axis);
    float endTurn[4];
    memcpy(endTurn, w[limb.End] + 4, sizeof(endTurn));
    QNorm(endTurn);
    float q[4];
    QAxis(axis, angle, q);
    ApplyWorldTurn(h, limb.Upper, q, w);
    SetWorldTurn(h, limb.End, endTurn, w);
  }
