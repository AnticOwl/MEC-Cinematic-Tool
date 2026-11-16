#include "PoseClips.h"
#include "PoseRest.h"
#include "../ufbx/ufbx.h"
#include <Windows.h>
#include <algorithm>
#include <cctype>
#include <cwctype>
#include <cmath>
#include <initializer_list>
#include <map>
#include <cstdio>
#include <cstring>

// See PoseClips.h. Checked offline (2026-11-11) on two of Mixamo's own exports (three.js's examples "Samba Dancing.fbx",
// FBX 7.4, and "mixamo.fbx", FBX 7.7): Y up, centimetres, 30 fps, two stacks ("Take 001": the T-pose, still;
// "mixamo.com": the animation), the bones' default pose = the skin's bind pose = a T-pose, only Hips moves (every other
// bone only turns), the character facing +Z with its left at +X (the game's frame), PreRotation on 55 bones in the
// 7.7 file and none in the 7.4 one (ufbx folds it in). A Python model of this retarget onto the four human skeletons
// put every driven bone within 0.0002 deg of the clip's direction over 506 and 547 frames.
namespace
{
  struct Q
  {
    double x, y, z, w;
  };
  struct V
  {
    double x, y, z;
  };

  Q Mul(Q a, Q b)
  {
    return { a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
      a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z };
  }
  Q Conj(Q a) { return { -a.x, -a.y, -a.z, a.w }; }
  Q Norm(Q a)
  {
    double l = sqrt(a.x * a.x + a.y * a.y + a.z * a.z + a.w * a.w);
    if (!(l > 1e-12))
      return { 0.0, 0.0, 0.0, 1.0 };
    return { a.x / l, a.y / l, a.z / l, a.w / l };
  }
  V Sub(V a, V b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
  double Dot(V a, V b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
  V Cross(V a, V b) { return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; }
  double Len(V a) { return sqrt(Dot(a, a)); }
  V Scale(V a, double s) { return { a.x * s, a.y * s, a.z * s }; }
  V Rot(Q q, V v)
  {
    Q p = Mul(Mul(q, Q{ v.x, v.y, v.z, 0.0 }), Conj(q));
    return { p.x, p.y, p.z };
  }

  // A rotation matrix's quaternion: m[r][c], column vectors (m v)
  Q FromMatrix(double const m[3][3])
  {
    double trace = m[0][0] + m[1][1] + m[2][2];
    Q q;
    if (trace > 0.0)
    {
      double s = sqrt(trace + 1.0) * 2.0;
      q = { (m[2][1] - m[1][2]) / s, (m[0][2] - m[2][0]) / s, (m[1][0] - m[0][1]) / s, 0.25 * s };
    }
    else if (m[0][0] > m[1][1] && m[0][0] > m[2][2])
    {
      double s = sqrt(1.0 + m[0][0] - m[1][1] - m[2][2]) * 2.0;
      q = { 0.25 * s, (m[0][1] + m[1][0]) / s, (m[0][2] + m[2][0]) / s, (m[2][1] - m[1][2]) / s };
    }
    else if (m[1][1] > m[2][2])
    {
      double s = sqrt(1.0 + m[1][1] - m[0][0] - m[2][2]) * 2.0;
      q = { (m[0][1] + m[1][0]) / s, 0.25 * s, (m[1][2] + m[2][1]) / s, (m[0][2] - m[2][0]) / s };
    }
    else
    {
      double s = sqrt(1.0 + m[2][2] - m[0][0] - m[1][1]) * 2.0;
      q = { (m[0][2] + m[2][0]) / s, (m[1][2] + m[2][1]) / s, 0.25 * s, (m[1][0] - m[0][1]) / s };
    }
    return Norm(q);
  }

  Q Slerp(Q a, Q b, double t)
  {
    double d = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
    if (d < 0.0)
    {
      b = { -b.x, -b.y, -b.z, -b.w };
      d = -d;
    }
    double ka = 1.0 - t, kb = t;
    if (d < 0.9995)
    {
      double angle = acos(d), s = sin(angle);
      ka = sin((1.0 - t) * angle) / s;
      kb = sin(t * angle) / s;
    }
    return Norm({ a.x * ka + b.x * kb, a.y * ka + b.y * kb, a.z * ka + b.z * kb, a.w * ka + b.w * kb });
  }

  // A frame from a bone's direction and a side axis: its columns d, n, d x n (null if they're parallel)
  bool Frame(V d, V side, double m[3][3])
  {
    double ld = Len(d);
    if (!(ld > 1e-6))
      return false;
    d = Scale(d, 1.0 / ld);
    V n = Sub(side, Scale(d, Dot(side, d)));
    double ln = Len(n);
    if (!(ln > 1e-3 * (Len(side) > 1e-9 ? Len(side) : 1.0)))
      return false;
    n = Scale(n, 1.0 / ln);
    V b = Cross(d, n);
    V cols[3] = { d, n, b };
    for (int c = 0; c < 3; ++c)
    {
      m[0][c] = cols[c].x;
      m[1][c] = cols[c].y;
      m[2][c] = cols[c].z;
    }
    return true;
  }

  // Mixamo's standard bones (the names after the last ':')
  const char* const MixamoBones[] = {
    "Hips", "Spine", "Spine1", "Spine2", "Neck", "Head", "HeadTop_End",
    "LeftShoulder", "LeftArm", "LeftForeArm", "LeftHand",
    "LeftHandThumb1", "LeftHandThumb2", "LeftHandThumb3", "LeftHandThumb4",
    "LeftHandIndex1", "LeftHandIndex2", "LeftHandIndex3", "LeftHandIndex4",
    "LeftHandMiddle1", "LeftHandMiddle2", "LeftHandMiddle3", "LeftHandMiddle4",
    "LeftHandRing1", "LeftHandRing2", "LeftHandRing3", "LeftHandRing4",
    "LeftHandPinky1", "LeftHandPinky2", "LeftHandPinky3", "LeftHandPinky4",
    "LeftUpLeg", "LeftLeg", "LeftFoot", "LeftToeBase", "LeftToe_End",
    "RightShoulder", "RightArm", "RightForeArm", "RightHand",
    "RightHandThumb1", "RightHandThumb2", "RightHandThumb3", "RightHandThumb4",
    "RightHandIndex1", "RightHandIndex2", "RightHandIndex3", "RightHandIndex4",
    "RightHandMiddle1", "RightHandMiddle2", "RightHandMiddle3", "RightHandMiddle4",
    "RightHandRing1", "RightHandRing2", "RightHandRing3", "RightHandRing4",
    "RightHandPinky1", "RightHandPinky2", "RightHandPinky3", "RightHandPinky4",
    "RightUpLeg", "RightLeg", "RightFoot", "RightToeBase", "RightToe_End" };
  const int MixamoCount = sizeof(MixamoBones) / sizeof(MixamoBones[0]);

  // A Mixamo bone's name in the game's human skeletons (the same but for the end bones'; checked on all four)
  std::string GameName(std::string const& mixamo)
  {
    if (mixamo == "HeadTop_End")
      return "HeadEnd";
    if (mixamo == "LeftToe_End")
      return "LeftToe";
    if (mixamo == "RightToe_End")
      return "RightToe";
    return mixamo;
  }

  // The bone a bone aims at (its direction at rest), or "" for none
  std::string AimOf(std::string const& b)
  {
    static const char* const pairs[][2] = {
      { "Hips", "Spine" }, { "Spine", "Spine1" }, { "Spine1", "Spine2" }, { "Spine2", "Neck" }, { "Neck", "Head" },
      { "Head", "HeadTop_End" } };
    for (auto const& p : pairs)
      if (b == p[0])
        return p[1];
    for (const char* side : { "Left", "Right" })
    {
      std::string s = side;
      static const char* const limb[][2] = { { "Shoulder", "Arm" }, { "Arm", "ForeArm" }, { "ForeArm", "Hand" },
        { "Hand", "HandMiddle1" }, { "UpLeg", "Leg" }, { "Leg", "Foot" }, { "Foot", "ToeBase" }, { "ToeBase", "Toe_End" } };
      for (auto const& p : limb)
        if (b == s + p[0])
          return s + p[1];
      for (const char* finger : { "Thumb", "Index", "Middle", "Ring", "Pinky" })
        for (int k = 1; k <= 3; ++k)
          if (b == s + "Hand" + finger + std::to_string(k))
            return s + "Hand" + finger + std::to_string(k + 1);
    }
    return "";
  }

  // The feet and toes copy the clip's turn from rest to rest, not its exact direction: the ankle-to-toe line
  // differs between the rigs by 9 - 13 deg with flat feet (19 deg on the heels rig), so flat stays flat and heels
  // stay heels (offline, 2026-11-11)
  bool FromRestOnly(std::string const& b)
  {
    return b == "LeftFoot" || b == "RightFoot" || b == "LeftToeBase" || b == "RightToeBase";
  }

  // Which side axis a bone's rest frame takes: the hand's knuckles (Pinky1 - Index1) for the arm, hand and fingers,
  // up for the collarbones, the character's left for the rest
  int SideKind(std::string const& b, std::string& side)
  {
    for (const char* s : { "Left", "Right" })
      if (b.compare(0, strlen(s), s) == 0)
      {
        std::string rest = b.substr(strlen(s));
        if (rest == "Shoulder")
          return 1;
        if (rest == "Arm" || rest == "ForeArm" || rest.compare(0, 4, "Hand") == 0)
        {
          side = s;
          return 2;
        }
      }
    return 0;
  }

  std::string ShortName(ufbx_string s)
  {
    std::string n(s.data, s.length);
    size_t c = n.find_last_of(":|");
    return c == std::string::npos ? n : n.substr(c + 1);
  }

  V Axis(ufbx_coordinate_axis a, bool& ok)
  {
    switch (a)
    {
    case UFBX_COORDINATE_AXIS_POSITIVE_X: return { 1, 0, 0 };
    case UFBX_COORDINATE_AXIS_NEGATIVE_X: return { -1, 0, 0 };
    case UFBX_COORDINATE_AXIS_POSITIVE_Y: return { 0, 1, 0 };
    case UFBX_COORDINATE_AXIS_NEGATIVE_Y: return { 0, -1, 0 };
    case UFBX_COORDINATE_AXIS_POSITIVE_Z: return { 0, 0, 1 };
    case UFBX_COORDINATE_AXIS_NEGATIVE_Z: return { 0, 0, -1 };
    default: ok = false; return { 0, 0, 0 };
    }
  }

  struct SceneFree
  {
    ufbx_scene* Scene;
    ~SceneFree()
    {
      if (Scene)
        ufbx_free_scene(Scene);
    }
  };

  // A node's world rotation and place in the game's frame: m the file-to-game basis, unit metres per file unit,
  // yaw the turn that faces the rig +Z
  void WorldOf(ufbx_matrix const& w, double const m[3][3], double unit, Q yaw, Q& q, V& p)
  {
    double r[3][3], g[3][3];
    for (int c = 0; c < 3; ++c)
    {
      V col = { w.cols[c].x, w.cols[c].y, w.cols[c].z };
      double l = Len(col);
      if (l > 1e-12)
        col = Scale(col, 1.0 / l);
      r[0][c] = col.x;
      r[1][c] = col.y;
      r[2][c] = col.z;
    }
    for (int i = 0; i < 3; ++i)
      for (int j = 0; j < 3; ++j)
        g[i][j] = m[i][0] * r[0][j] + m[i][1] * r[1][j] + m[i][2] * r[2][j];
    q = Mul(yaw, FromMatrix(g));
    V t = { w.cols[3].x, w.cols[3].y, w.cols[3].z };
    V gt = { m[0][0] * t.x + m[0][1] * t.y + m[0][2] * t.z, m[1][0] * t.x + m[1][1] * t.y + m[1][2] * t.z,
      m[2][0] * t.x + m[2][1] * t.y + m[2][2] * t.z };
    p = Rot(yaw, Scale(gt, unit));
  }

  // A file axis' name ("+X" .. "-Z")
  std::string AxisName(V v)
  {
    return std::string(v.x > 0.5 ? "+X" : v.x < -0.5 ? "-X" : v.y > 0.5 ? "+Y" : v.y < -0.5 ? "-Y" : v.z > 0.5 ? "+Z" :
      v.z < -0.5 ? "-Z" : "?");
  }

  double AngleDeg(Q a, Q b)
  {
    double d = fabs(a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w);
    return 2.0 * acos(d > 1.0 ? 1.0 : d) * 57.29577951308232;
  }

  std::string Utf8(std::wstring const& w)
  {
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n : 0, '\0');
    if (n > 0)
      WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
  }

  // ---- Which bone is which, in any humanoid rig ----
  //
  // Mixamo's names are the roles (MixamoBones). A rig's bones get them from their names' words and where they hang:
  // the head, hands and feet by name; the hips where the head and both feet hang from; the legs, spine and arms on
  // the paths between those (by name words, else by order); the fingers by name. A twist, corrective or IK bone that
  // isn't on a path is never taken, and one on a path is passed over for a plain one.

  // A name's words: split at anything but a letter or digit, at a lower-to-upper case change, at the end of a run of
  // capitals before a capitalised word ("IKHand"), and between letters and digits; lower case
  std::vector<std::string> Words(std::string const& name)
  {
    std::vector<std::string> out;
    std::string cur;
    auto flush = [&]()
    {
      if (cur.empty())
        return;
      for (char& ch : cur)
        ch = (char)tolower((unsigned char)ch);
      out.push_back(cur);
      cur.clear();
    };
    for (size_t i = 0; i < name.size(); ++i)
    {
      unsigned char c = (unsigned char)name[i];
      bool alpha = c < 128 && isalpha(c), digit = c < 128 && isdigit(c);
      if (!alpha && !digit)
      {
        flush();
        continue;
      }
      if (!cur.empty())
      {
        unsigned char p = (unsigned char)cur.back();
        bool pAlpha = isalpha(p) != 0, pDigit = isdigit(p) != 0;
        bool camel = pAlpha && alpha && islower(p) && isupper(c);
        bool runEnd = pAlpha && alpha && isupper(p) && isupper(c) && i + 1 < name.size() &&
          (unsigned char)name[i + 1] < 128 && islower((unsigned char)name[i + 1]);
        if (camel || runEnd || (pAlpha && digit) || (pDigit && alpha))
          flush();
      }
      cur += (char)c;
    }
    flush();
    return out;
  }

  struct RigBone
  {
    ufbx_node* Node;
    std::string Name;                     // its short name
    std::vector<std::string> Words;       // its name's words, the side's and the rigs' own prefixes taken out
    std::string Digits;                   // its name's last run of digits
    int Side = -1;                        // 0 left, 1 right, -1 neither
    bool End = false;                     // an end (a tip, a nub)
    bool Helper = false;                  // a twist, corrective, IK or control bone
    int Parent = -1;                      // in the list, -1 outside it
    int Depth = 0;
    V Rest;                               // its rest place (the game's axes, metres; not faced yet)
  };

  bool Has(RigBone const& b, char const* w)
  {
    for (std::string const& x : b.Words)
      if (x == w)
        return true;
    return false;
  }
  bool HasAny(RigBone const& b, std::initializer_list<char const*> ws)
  {
    for (char const* w : ws)
      if (Has(b, w))
        return true;
    return false;
  }

  RigBone Describe(ufbx_node* node)
  {
    RigBone b;
    b.Node = node;
    b.Name = ShortName(node->name);
    static char const* const noise[] = { "mixamorig", "mixamo", "bip", "valvebiped", "def", "org", "cc", "base", "j", "jnt",
      "joint", "bn", "b", "bone", "c", "sk", "rig", "armature", "character", "skeleton" };
    static char const* const helper[] = { "twist", "roll", "ik", "fk", "pole", "target", "ctrl", "control", "helper", "hlp",
      "mch", "socket", "attach", "weapon", "prop", "corrective", "correctiveroot", "twistcor", "cor", "fwd", "bck", "out", "in",
      "lwr", "upr", "scap", "pec", "latissimus", "bulge", "jiggle", "dyn", "phys", "offset", "null", "locator", "interaction",
      "mass", "gun" };
    static char const* const ends[] = { "end", "nub", "tip", "top", "leaf" };
    for (std::string const& w : Words(b.Name))
    {
      if (isdigit((unsigned char)w[0]))
      {
        b.Digits = w;
        continue;
      }
      if (b.Side < 0 && (w == "l" || w == "left" || w == "lft" || w == "lt"))
      {
        b.Side = 0;
        continue;
      }
      if (b.Side < 0 && (w == "r" || w == "right" || w == "rgt" || w == "rt"))
      {
        b.Side = 1;
        continue;
      }
      bool skip = false;
      for (char const* n : noise)
        skip = skip || w == n;
      if (skip)
        continue;
      for (char const* h : helper)
        b.Helper = b.Helper || w == h;
      for (char const* e : ends)
        b.End = b.End || w == e;
      b.Words.push_back(w);
    }
    return b;
  }

  // The finger a bone is in (0 the thumb .. 4 the little finger), or -1; metacarpal: the hand's bone below it
  int FingerOf(RigBone const& b, bool& metacarpal)
  {
    metacarpal = HasAny(b, { "metacarpal", "carpal", "meta", "metacarpals" });
    static char const* const names[5][4] = { { "thumb", nullptr }, { "index", "pointer", "forefinger", nullptr },
      { "middle", "mid", nullptr }, { "ring", nullptr }, { "pinky", "little", "small", "pinkie" } };
    for (int f = 0; f < 5; ++f)
      for (int k = 0; k < 4 && names[f][k]; ++k)
        if (Has(b, names[f][k]))
          return f;
    // 3ds Max's Biped: "Finger" and the finger's digit (0 the thumb), then its segment's ("Finger01")
    if (Has(b, "finger") && !b.Digits.empty() && b.Digits[0] >= '0' && b.Digits[0] <= '4')
      return b.Digits[0] - '0';
    return -1;
  }

  // Each role's bone (MixamoBones' order) found in a rig; false and why if its hips, head, hands or feet aren't
  std::vector<int> FindRoles(std::vector<RigBone> const& rig, std::string& why, std::string& notes)
  {
    int n = (int)rig.size();
    std::vector<int> role(MixamoCount, -1);
    auto roleIndex = [](char const* name)
    {
      for (int i = 0; i < MixamoCount; ++i)
        if (!strcmp(MixamoBones[i], name))
          return i;
      return -1;
    };
    auto set = [&](std::string const& name, int bone)
    {
      int r = roleIndex(name.c_str());
      if (r >= 0 && bone >= 0)
        role[r] = bone;
    };
    auto isAncestor = [&](int a, int b)
    {
      for (int p = rig[b].Parent; p >= 0; p = rig[p].Parent)
        if (p == a)
          return true;
      return false;
    };
    // The bones between a and b (b below a), top first, neither of them
    auto path = [&](int a, int b)
    {
      std::vector<int> out;
      for (int p = rig[b].Parent; p >= 0 && p != a; p = rig[p].Parent)
        out.insert(out.begin(), p);
      return out;
    };
    auto descendants = [&](int a)
    {
      int count = 0;
      for (int i = 0; i < n; ++i)
        count += isAncestor(a, i) ? 1 : 0;
      return count;
    };
    // The anchors: a plain bone with the word, on that side; of several, the one with the most bones below it, then
    // the shallowest
    auto anchor = [&](std::initializer_list<char const*> words, int side, std::initializer_list<char const*> notWords)
    {
      int best = -1, bestBelow = -1;
      for (int i = 0; i < n; ++i)
      {
        RigBone const& b = rig[i];
        if (b.End || b.Helper || b.Side != side || !HasAny(b, words) || HasAny(b, notWords))
          continue;
        bool metacarpal;
        if (FingerOf(b, metacarpal) >= 0)
          continue;
        int below = descendants(i);
        if (below > bestBelow || (below == bestBelow && b.Depth < rig[best].Depth))
        {
          best = i;
          bestBelow = below;
        }
      }
      return best;
    };
    int hand[2] = { anchor({ "hand", "wrist" }, 0, {}), anchor({ "hand", "wrist" }, 1, {}) };
    int foot[2] = { anchor({ "foot", "ankle" }, 0, {}), anchor({ "foot", "ankle" }, 1, {}) };
    int head = anchor({ "head" }, -1, {});
    if (head < 0 && hand[0] >= 0 && hand[1] >= 0)
    {
      // Not by name (Rigify's is DEF-spine.006): the highest plain bone not in a hand, its parent if it's an end
      for (int i = 0; i < n; ++i)
        if (!rig[i].Helper && i != hand[0] && i != hand[1] && !isAncestor(hand[0], i) && !isAncestor(hand[1], i) &&
            (head < 0 || rig[i].Rest.y > rig[head].Rest.y))
          head = i;
      if (head >= 0 && rig[head].End && rig[head].Parent >= 0)
        head = rig[head].Parent;
      if (head >= 0)
        notes += std::string(notes.empty() ? "" : "; ") + "the head by its place (" + rig[head].Name + ")";
    }
    if (head < 0 || hand[0] < 0 || hand[1] < 0 || foot[0] < 0 || foot[1] < 0)
    {
      why = std::string("its ") + (head < 0 ? "head" : hand[0] < 0 ? "left hand" : hand[1] < 0 ? "right hand" : foot[0] < 0 ?
        "left foot" : "right foot") + " wasn't found by name (write a bone map: see the menu)";
      return {};
    }
    // The hips: the lowest bone the head and both feet hang from
    int hips = -1;
    for (int p = rig[head].Parent; p >= 0 && hips < 0; p = rig[p].Parent)
      if (isAncestor(p, foot[0]) && isAncestor(p, foot[1]))
        hips = p;
    if (hips < 0)
    {
      why = "its head and feet don't hang from one bone (write a bone map: see the menu)";
      return {};
    }
    set("Hips", hips);
    set("Head", head);

    // The spine and the neck: the bones from the hips to the head; the neck the first with the word, the spine those
    // before it (else those up to where the arms hang from), spread over Spine, Spine1 and Spine2
    std::vector<int> spinePath = path(hips, head);
    auto attachOf = [&](int bone)
    {
      int at = hips;
      for (int s : spinePath)
        if (isAncestor(s, bone))
          at = s;
      return at;
    };
    int chest = attachOf(hand[0]);
    int neckAt = -1;
    for (size_t k = 0; k < spinePath.size() && neckAt < 0; ++k)
      if (!rig[spinePath[k]].Helper && Has(rig[spinePath[k]], "neck"))
        neckAt = (int)k;
    std::vector<int> spine;
    for (size_t k = 0; k < spinePath.size(); ++k)
    {
      int s = spinePath[k];
      if (neckAt >= 0 ? (int)k >= neckAt : (k > 0 && spinePath[k - 1] == chest))
        break;
      if (!rig[s].Helper)
        spine.push_back(s);
    }
    if (neckAt >= 0)
      set("Neck", spinePath[neckAt]);
    else
    {
      for (size_t k = 0; k + 1 < spinePath.size(); ++k)
        if (spinePath[k] == chest)
          set("Neck", spinePath[k + 1]);
    }
    if (!spine.empty())
    {
      set("Spine", spine.front());
      if (spine.size() >= 2)
        set("Spine2", spine.back());
      if (spine.size() >= 3)
        set("Spine1", spine[spine.size() / 2]);
    }
    // The head's end: an end bone named for the head below it, else an end bone right under it with no side
    // (Rigify's "DEF-spine.006_end")
    for (int pass = 0; pass < 2 && role[roleIndex("HeadTop_End")] < 0; ++pass)
      for (int i = 0; i < n; ++i)
        if (rig[i].End && (pass == 0 ? Has(rig[i], "head") && isAncestor(head, i) : rig[i].Parent == head &&
            rig[i].Side < 0))
        {
          set("HeadTop_End", i);
          break;
        }

    char const* sideName[2] = { "Left", "Right" };
    for (int side = 0; side < 2; ++side)
    {
      std::string S = sideName[side];
      // The leg: the thigh and the shin on the path from the hips to the foot (by name; else its last two)
      std::vector<int> leg;
      for (int b : path(hips, foot[side]))
        if (!rig[b].Helper)
          leg.push_back(b);
      int thigh = -1, shin = -1;
      for (int b : leg)
      {
        RigBone const& r = rig[b];
        if (thigh < 0 && (HasAny(r, { "thigh", "upleg", "upperleg", "femur" }) || (HasAny(r, { "up", "upper" }) && Has(r, "leg"))))
          thigh = b;
        else if (thigh >= 0 && shin < 0 && (HasAny(r, { "calf", "shin", "knee", "tibia", "lowerleg" }) ||
            (Has(r, "lower") && Has(r, "leg")) || (Has(r, "leg") && !HasAny(r, { "up", "upper" }))))
          shin = b;
      }
      if (thigh < 0 && leg.size() >= 2)
        thigh = leg[leg.size() - 2];
      if (shin < 0 && !leg.empty() && leg.back() != thigh)
        shin = leg.back();
      set(S + "UpLeg", thigh);
      set(S + "Leg", shin);
      set(S + "Foot", foot[side]);
      // The toes: the shallowest plain bone below the foot with the word; its end below it
      int toe = -1, toeEnd = -1;
      for (int i = 0; i < n; ++i)
        if (!rig[i].Helper && !rig[i].End && HasAny(rig[i], { "toe", "toes", "ball", "toebase" }) && isAncestor(foot[side], i) &&
            (toe < 0 || rig[i].Depth < rig[toe].Depth))
          toe = i;
      if (toe >= 0)
        for (int i = 0; i < n; ++i)
          if (rig[i].Parent == toe && (rig[i].End || HasAny(rig[i], { "toe", "toes" })) && !rig[i].Helper)
          {
            toeEnd = i;
            break;
          }
      set(S + "ToeBase", toe);
      set(S + "Toe_End", toeEnd);

      // The arm: from where it hangs (on the spine or the neck) to the hand: the collarbone, the upper arm, the forearm
      std::vector<int> arm;
      for (int b : path(attachOf(hand[side]), hand[side]))
        if (!rig[b].Helper)
          arm.push_back(b);
      int clavicle = -1, upper = -1, fore = -1;
      for (int b : arm)
      {
        RigBone const& r = rig[b];
        bool isFore = HasAny(r, { "forearm", "lowerarm", "elbow", "radius", "ulna" }) ||
          (HasAny(r, { "fore", "lower" }) && Has(r, "arm"));
        bool isUpper = !isFore && (HasAny(r, { "upperarm", "shldr", "humerus", "bicep" }) || Has(r, "arm"));
        if (clavicle < 0 && upper < 0 && HasAny(r, { "clavicle", "shoulder", "collar", "clav", "collarbone" }))
          clavicle = b;
        else if (upper < 0 && isUpper)
          upper = b;
        else if (upper >= 0 && fore < 0 && isFore)
          fore = b;
      }
      if (upper < 0 || fore < 0)
      {
        // Not by name: the last two plain bones before the hand, and a collarbone before them
        if (arm.size() >= 2)
        {
          upper = arm[arm.size() - 2];
          fore = arm.back();
          if (clavicle < 0 && arm.size() >= 3)
            clavicle = arm[arm.size() - 3];
        }
      }
      set(S + "Shoulder", clavicle);
      set(S + "Arm", upper);
      set(S + "ForeArm", fore);
      set(S + "Hand", hand[side]);

      // The fingers below the hand: each one's chain of plain bones (not its metacarpal), top first: 1, 2, 3, then its tip
      static char const* const fingerName[5] = { "Thumb", "Index", "Middle", "Ring", "Pinky" };
      for (int f = 0; f < 5; ++f)
      {
        std::vector<int> chain;
        for (int i = 0; i < n; ++i)
        {
          bool metacarpal;
          if (rig[i].Helper || (rig[i].Side >= 0 && rig[i].Side != side) || !isAncestor(hand[side], i) ||
              FingerOf(rig[i], metacarpal) != f || metacarpal)
            continue;
          chain.push_back(i);
        }
        std::sort(chain.begin(), chain.end(), [&](int a, int b) { return rig[a].Depth < rig[b].Depth; });
        // Only a chain (each below the one before)
        std::vector<int> kept;
        for (int b : chain)
          if (kept.empty() || isAncestor(kept.back(), b))
            kept.push_back(b);
        for (size_t k = 0; k < kept.size() && k < 4; ++k)
          set(S + "Hand" + fingerName[f] + std::to_string(k + 1), kept[k]);
      }
    }
    return role;
  }

  std::string Trim(std::string const& t)
  {
    size_t a = t.find_first_not_of(" \t\r\n"), b = t.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? std::string() : t.substr(a, b - a + 1);
  }
}

std::shared_ptr<pose_clips::Clip const> pose_clips::Load(std::wstring const& path, std::string& error)
{
  std::wstring fileName = path.substr(path.find_last_of(L"\\/") == std::wstring::npos ? 0 : path.find_last_of(L"\\/") + 1);
  // The file, read here (wide path)
  FILE* f = nullptr;
  if (_wfopen_s(&f, path.c_str(), L"rb") || !f)
  {
    error = "couldn't be opened";
    return nullptr;
  }
  _fseeki64(f, 0, SEEK_END);
  __int64 size = _ftelli64(f);
  _fseeki64(f, 0, SEEK_SET);
  if (size <= 0 || size > (__int64)512 * 1024 * 1024)
  {
    fclose(f);
    error = size <= 0 ? "is empty" : "is over 512 MB";
    return nullptr;
  }
  std::vector<char> data((size_t)size);
  size_t got = fread(data.data(), 1, data.size(), f);
  fclose(f);
  if (got != data.size())
  {
    error = "couldn't be read";
    return nullptr;
  }
  // A Unity clip (.anim): its own reader
  {
    size_t dot = fileName.find_last_of(L'.');
    std::wstring ext = dot == std::wstring::npos ? std::wstring() : fileName.substr(dot);
    for (wchar_t& c : ext)
      c = (wchar_t)towlower(c);
    if (ext == L".anim")
      return LoadUnityAnim(path, data, error);
  }

  ufbx_load_opts opts = {};
  opts.ignore_geometry = true;
  opts.ignore_embedded = true;
  ufbx_error ufbxError;
  SceneFree scene{ ufbx_load_memory(data.data(), data.size(), &opts, &ufbxError) };
  if (!scene.Scene)
  {
    char text[512];
    ufbx_format_error(text, sizeof(text), &ufbxError);
    error = std::string("isn't an FBX file ufbx can read: ") + text;
    return nullptr;
  }
  ufbx_scene* s = scene.Scene;
  auto clip = std::make_shared<Clip>();
  clip->File = Utf8(fileName);
  {
    size_t dot = path.find_last_of(L'.'), slash = path.find_last_of(L"\\/");
    clip->MapPath = (dot != std::wstring::npos && (slash == std::wstring::npos || dot > slash) ? path.substr(0, dot) : path) +
      L".bones.txt";
  }
  std::string warnings;

  // The file's axes and units, to the game's: +X (the character's left), +Y up, +Z front, metres
  bool axesOk = true;
  V right = Axis(s->settings.axes.right, axesOk), up = Axis(s->settings.axes.up, axesOk),
    front = Axis(s->settings.axes.front, axesOk);
  double basis[3][3] = { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } };
  if (axesOk)
  {
    V cols[3] = { right, up, front };
    // m = A^T: a file vector along its declared right / up / front goes to the game's +X / +Y / +Z
    for (int r = 0; r < 3; ++r)
    {
      basis[r][0] = cols[r].x;
      basis[r][1] = cols[r].y;
      basis[r][2] = cols[r].z;
    }
    double det = Dot(right, Cross(up, front));
    if (det < 0.5)
    {
      error = "is in a left-handed or broken coordinate system (its axes)";
      return nullptr;
    }
  }
  else
    warnings += "its axes aren't given (taken as Y up); ";
  double unit = s->settings.unit_meters > 0.0 ? s->settings.unit_meters : 0.01;
  if (!(s->settings.unit_meters > 0.0))
    warnings += "its unit isn't given (taken as cm); ";
  for (int r = 0; r < 3; ++r)
    for (int c = 0; c < 3; ++c)
      clip->Basis[r][c] = basis[r][c];
  clip->Unit = unit;

  // Its rig: its bones (every node but meshes when it has hardly any marked bones), their names' words, where they
  // hang and their rest places
  std::vector<RigBone> rig;
  {
    std::vector<ufbx_node*> picked;
    for (size_t i = 0; i < s->nodes.count; ++i)
      if (s->nodes.data[i]->bone)
        picked.push_back(s->nodes.data[i]);
    if (picked.size() < 10)
    {
      picked.clear();
      for (size_t i = 0; i < s->nodes.count; ++i)
        if (!s->nodes.data[i]->is_root && !s->nodes.data[i]->mesh)
          picked.push_back(s->nodes.data[i]);
    }
    for (ufbx_node* node : picked)
    {
      RigBone b = Describe(node);
      Q q;
      WorldOf(node->node_to_world, basis, unit, Q{ 0, 0, 0, 1 }, q, b.Rest);
      rig.push_back(b);
    }
    for (size_t i = 0; i < rig.size(); ++i)
    {
      for (ufbx_node* up = rig[i].Node->parent; up && rig[i].Parent < 0; up = up->parent)
        for (size_t j = 0; j < rig.size(); ++j)
          if (rig[j].Node == up)
          {
            rig[i].Parent = (int)j;
            break;
          }
    }
    for (size_t i = 0; i < rig.size(); ++i)
      for (int p = rig[i].Parent, guard = 0; p >= 0 && guard < 1000; p = rig[p].Parent, ++guard)
        ++rig[i].Depth;
  }
  // Its bone map, if it has one: each line "<Mixamo's name> = <its bone>"; else found by names and places. "Up = +Y"
  // (the file's axis that is up: +X -X +Y -Y +Z -Z) and "Unit = 0.01" (metres per unit) replace the file's settings
  // and the tools' checks of them; a map with only those leaves the bones to the finder.
  std::vector<int> role;
  V mapUp = { 0, 0, 0 };
  double mapUnit = 0.0;
  {
    FILE* map = nullptr;
    bool fromMap = false;
    if (!_wfopen_s(&map, clip->MapPath.c_str(), L"r") && map)
    {
      role.assign(MixamoCount, -1);
      char line[512];
      int lines = 0;
      std::string unknown;
      bool first = true;
      while (fgets(line, sizeof(line), map))
      {
        std::string t = Trim(line);
        if (first && t.size() >= 3 && (unsigned char)t[0] == 0xEF && (unsigned char)t[1] == 0xBB &&
            (unsigned char)t[2] == 0xBF)
          t = Trim(t.substr(3));   // (a UTF-8 BOM, as some editors save)
        first = false;
        size_t eq = t.find('=');
        if (t.empty() || t[0] == '#' || eq == std::string::npos)
          continue;
        std::string r = Trim(t.substr(0, eq)), name = Trim(t.substr(eq + 1));
        std::string key = r;
        for (char& c : key)
          c = (char)tolower((unsigned char)c);
        if (key == "up")
        {
          std::string v = name;
          for (char& c : v)
            c = (char)toupper((unsigned char)c);
          double sign = v.size() == 2 && v[0] == '-' ? -1.0 : 1.0;
          char axis = v.size() == 2 && (v[0] == '-' || v[0] == '+') ? v[1] : (v.size() == 1 ? v[0] : '?');
          if (axis == 'X' || axis == 'Y' || axis == 'Z')
            mapUp = { axis == 'X' ? sign : 0.0, axis == 'Y' ? sign : 0.0, axis == 'Z' ? sign : 0.0 };
          else
            unknown += (unknown.empty() ? "" : ", ") + t;
          continue;
        }
        if (key == "unit")
        {
          char* end = nullptr;
          double v = strtod(name.c_str(), &end);
          std::string rest = end && end != name.c_str() ? Trim(end) : std::string("?");
          if (end && end != name.c_str() && (rest.empty() || rest == "m" || rest == "M") && std::isfinite(v) && v > 1e-9 &&
              v < 1e9)
            mapUnit = v;
          else
            unknown += (unknown.empty() ? "" : ", ") + t;
          continue;
        }
        int ri = -1;
        for (int k = 0; k < MixamoCount; ++k)
          if (r == MixamoBones[k])
            ri = k;
        int bi = -1;
        for (size_t k = 0; k < rig.size() && bi < 0; ++k)
          if (rig[k].Name == name || std::string(rig[k].Node->name.data, rig[k].Node->name.length) == name)
            bi = (int)k;
        if (ri < 0 || bi < 0)
        {
          unknown += (unknown.empty() ? "" : ", ") + t;
          continue;
        }
        role[ri] = bi;
        ++lines;
      }
      fclose(map);
      fromMap = lines > 0;
      if (fromMap)
        clip->BonesFrom = "its bone map (" + Utf8(clip->MapPath.substr(clip->MapPath.find_last_of(L"\\/") ==
          std::wstring::npos ? 0 : clip->MapPath.find_last_of(L"\\/") + 1)) + ")";
      if (!unknown.empty())
        warnings += "its bone map's lines that name no role or no bone in the file (or no axis or unit): " + unknown + "; ";
    }
    if (!fromMap)
    {
      std::string why, notes;
      role = FindRoles(rig, why, notes);
      if (role.empty())
      {
        error = why;
        return nullptr;
      }
      clip->BonesFrom = "their names and where they hang";
      if (!notes.empty())
        warnings += notes + "; ";
    }
  }
  std::vector<ufbx_node*> nodes;
  for (int r = 0; r < MixamoCount; ++r)
  {
    if (role[r] < 0)
      continue;
    if (r == 0)
      clip->Hips = (int)nodes.size();
    nodes.push_back(rig[role[r]].Node);
    clip->Bones.push_back(MixamoBones[r]);
    clip->SourceNames.push_back(rig[role[r]].Name);
  }
  if (clip->Hips < 0)
  {
    error = "has no hips (its bone map names none)";
    return nullptr;
  }
  int bones = (int)nodes.size();
  auto indexOf = [&](char const* name) -> int
  {
    for (int i = 0; i < bones; ++i)
      if (clip->Bones[i] == name)
        return i;
    return -1;
  };
  const double Pi = 3.14159265358979323846;

  // The animation: "mixamo.com" (Mixamo's own), else the one whose bones turn the most
  ufbx_anim_stack* stack = nullptr;
  double bestScore = -1.0;
  for (size_t k = 0; k < s->anim_stacks.count; ++k)
  {
    ufbx_anim_stack* candidate = s->anim_stacks.data[k];
    std::string name(candidate->name.data, candidate->name.length);
    if (name == "mixamo.com")
    {
      stack = candidate;
      bestScore = 1e30;
      break;
    }
    double score = 0.0;
    for (int i = 0; i < bones; ++i)
    {
      ufbx_quat q0 = ufbx_evaluate_transform(candidate->anim, nodes[i], candidate->time_begin).rotation;
      for (int t = 1; t <= 8; ++t)
      {
        double time = candidate->time_begin + (candidate->time_end - candidate->time_begin) * t / 8.0;
        ufbx_quat q = ufbx_evaluate_transform(candidate->anim, nodes[i], time).rotation;
        double d = AngleDeg({ q0.x, q0.y, q0.z, q0.w }, { q.x, q.y, q.z, q.w });
        score += d;
      }
    }
    if (score > bestScore + 1e-9 || (fabs(score - bestScore) <= 1e-9 && stack &&
        candidate->time_end - candidate->time_begin > stack->time_end - stack->time_begin))
    {
      bestScore = score;
      stack = candidate;
    }
  }
  if (!stack)
  {
    error = "has no animation";
    return nullptr;
  }
  if (bestScore < 1.0)
    warnings += "its animation barely moves; ";
  clip->Take.assign(stack->name.data, stack->name.length);
  double fps = s->settings.frames_per_second;
  if (!(fps > 0.0 && fps <= 1000.0))
  {
    warnings += "its frame rate isn't given or is out of range (taken as 30 fps); ";
    fps = 30.0;
  }
  clip->Fps = fps;
  double duration = stack->time_end - stack->time_begin;
  double count = duration > 0.0 ? floor(duration * fps + 0.5) + 1.0 : 1.0;
  if (!(count >= 1.0))
    count = 1.0;
  if (count > 30000.0)
  {
    warnings += "it's over 30000 frames (cut); ";
    count = 30000.0;
  }
  int frames = (int)count;
  clip->Frames = frames;

  // The rest pose: the skin's bind pose where there is one (its clusters' bind matrices, what a With Skin file's mesh
  // is skinned in; a bone without one hangs from the nearest above it that has one by its default locals), else the
  // bones' default pose. Not the file's BindPose list: in Mixamo's FBX 7.4 export ("Samba Dancing.fbx") that has the
  // legs' frames turned 180 deg from the bones (same places), while the clusters match the default pose exactly; a
  // Blender export with no mesh has neither, its default pose being the frame it was exported at (offline, 2026-11-11).
  // - A bind matrix is where its bone was when the mesh was bound: each is placed by its mesh's move since (a rig moved
  //   or turned after binding moves its mesh with it): the mesh node's world now x the cluster's mesh-to-bone matrix
  //   turned round, the bind matrix itself for a rig not moved. Only for a mesh node that stays still over the clip
  //   and doesn't hang under the rig's bones: one the animation moves isn't the rig moved after binding.
  // - A cluster with no bind matrix (ufbx leaves it zero without a TransformLink) is passed over.
  // - Of several skins, the one with the most of the clip's bones gives its bones first (a coat bound later, in
  //   another pose, gives only the ones it alone has).
  // - A skin that doesn't cover the body (both thighs and both upper arms, or half the bones of a rig with none of
  //   them: a hat on the head) isn't taken as the rest. A body skin without the fingers is (they hang from the hands).
  std::map<ufbx_node const*, ufbx_matrix> binds;
  double placedMove = 0.0, placedTurn = 0.0;
  {
    std::vector<std::pair<int, ufbx_skin_deformer*>> skins;
    for (size_t d = 0; d < s->skin_deformers.count; ++d)
    {
      ufbx_skin_deformer* skin = s->skin_deformers.data[d];
      int has = 0;
      for (size_t k = 0; k < skin->clusters.count; ++k)
        for (int i = 0; i < bones; ++i)
          has += skin->clusters.data[k]->bone_node == nodes[i];
      skins.push_back({ has, skin });
    }
    std::stable_sort(skins.begin(), skins.end(), [](std::pair<int, ufbx_skin_deformer*> const& a,
      std::pair<int, ufbx_skin_deformer*> const& b) { return a.first > b.first; });
    // (the scene at 8 times over the clip: whether a mesh node stays still)
    std::vector<ufbx_scene*> probes;
    struct ProbesFree
    {
      std::vector<ufbx_scene*>& List;
      ~ProbesFree()
      {
        for (ufbx_scene* p : List)
          ufbx_free_scene(p);
      }
    } probesFree{ probes };
    bool probed = false, probesOk = false;
    auto stillAndFree = [&](ufbx_node* mesh)
    {
      for (ufbx_node* up = mesh; up; up = up->parent)
        for (int i = 0; i < bones; ++i)
          if (up == nodes[i])
            return false;   // under the rig's bones
      if (!probed)
      {
        probed = true;
        probesOk = true;
        for (int n = 0; n < 8; ++n)
        {
          ufbx_scene* p = ufbx_evaluate_scene(s, stack->anim, stack->time_begin + (stack->time_end - stack->time_begin) *
            n / 7.0, nullptr, &ufbxError);
          if (!p)
          {
            probesOk = false;
            break;
          }
          probes.push_back(p);
        }
      }
      if (!probesOk)
        return false;
      // (each column against its own size: a far-away mesh node's place doesn't hide its turning)
      ufbx_matrix const& now = mesh->node_to_world;
      for (ufbx_scene* p : probes)
      {
        ufbx_matrix const& m = p->nodes.data[mesh->typed_id]->node_to_world;
        for (int c = 0; c < 4; ++c)
        {
          V was = { now.cols[c].x, now.cols[c].y, now.cols[c].z }, is = { m.cols[c].x, m.cols[c].y, m.cols[c].z };
          double size = Len(was) > 1.0 ? Len(was) : 1.0;
          if (!(Len(Sub(is, was)) <= 1e-5 * size))
            return false;   // the animation moves it
        }
      }
      return true;
    };
    for (auto const& entry : skins)
    {
      ufbx_skin_deformer* skin = entry.second;
      ufbx_node* meshNode = nullptr;
      for (size_t m = 0; m < s->meshes.count && !meshNode; ++m)
        for (size_t k = 0; k < s->meshes.data[m]->skin_deformers.count && !meshNode; ++k)
          if (s->meshes.data[m]->skin_deformers.data[k] == skin && s->meshes.data[m]->instances.count)
            meshNode = s->meshes.data[m]->instances.data[0];
      if (meshNode && meshNode->is_geometry_transform_helper && meshNode->parent)
        meshNode = meshNode->parent;
      if (meshNode && !stillAndFree(meshNode))
        meshNode = nullptr;   // (its binds as they are)
      for (size_t k = 0; k < skin->clusters.count; ++k)
      {
        ufbx_skin_cluster* cluster = skin->clusters.data[k];
        if (!cluster->bone_node || binds.count(cluster->bone_node) ||
            !(fabs(ufbx_matrix_determinant(&cluster->bind_to_world)) > 1e-12))
          continue;
        ufbx_matrix bind = cluster->bind_to_world;
        if (meshNode && fabs(ufbx_matrix_determinant(&cluster->mesh_node_to_bone)) > 1e-12)
        {
          ufbx_matrix toMesh = ufbx_matrix_invert(&cluster->mesh_node_to_bone);
          ufbx_matrix placed = ufbx_matrix_mul(&meshNode->node_to_world, &toMesh);
          if (fabs(ufbx_matrix_determinant(&placed)) > 1e-12)
          {
            Q a, b;
            V pa, pb;
            WorldOf(placed, basis, unit, Q{ 0, 0, 0, 1 }, a, pa);
            WorldOf(bind, basis, unit, Q{ 0, 0, 0, 1 }, b, pb);
            placedTurn = AngleDeg(a, b) > placedTurn ? AngleDeg(a, b) : placedTurn;
            placedMove = Len(Sub(pa, pb)) > placedMove ? Len(Sub(pa, pb)) : placedMove;
            bind = placed;
          }
        }
        binds[cluster->bone_node] = bind;
      }
    }
  }
  int partial = 0;
  {
    int boundRoles = 0;
    for (int i = 0; i < bones; ++i)
      boundRoles += binds.count(nodes[i]) ? 1 : 0;
    bool covers = true;
    int limbs = 0;
    for (char const* limb : { "LeftUpLeg", "RightUpLeg", "LeftArm", "RightArm" })
      if (indexOf(limb) >= 0)
      {
        ++limbs;
        covers = covers && binds.count(nodes[indexOf(limb)]) > 0;
      }
    if (limbs == 0)
      covers = boundRoles * 2 >= bones;
    if (!binds.empty() && !covers)
    {
      partial = boundRoles;
      binds.clear();
    }
  }
  std::vector<ufbx_matrix> restWorld(bones);
  int bound = 0;
  double bindOff = 0.0, bindMoved = 0.0;
  for (int i = 0; i < bones; ++i)
  {
    restWorld[i] = nodes[i]->node_to_world;
    std::vector<ufbx_node*> chain;
    ufbx_node* at = nodes[i];
    while (!binds.count(at) && at->parent)
    {
      chain.push_back(at);
      at = at->parent;
    }
    auto bind = binds.find(at);
    if (bind == binds.end())
    {
      // None above it (the Hips of a rig whose skin doesn't use them): from a child's that has one
      for (size_t c = 0; c < nodes[i]->children.count; ++c)
      {
        auto childBind = binds.find(nodes[i]->children.data[c]);
        if (childBind == binds.end())
          continue;
        ufbx_matrix up = ufbx_matrix_invert(&nodes[i]->children.data[c]->node_to_parent);
        restWorld[i] = ufbx_matrix_mul(&childBind->second, &up);
        break;
      }
      continue;
    }
    ufbx_matrix m = bind->second;
    for (size_t k = chain.size(); k-- > 0;)
      m = ufbx_matrix_mul(&m, &chain[k]->node_to_parent);
    restWorld[i] = m;
    if (chain.empty())
    {
      ++bound;
      Q a, b;
      V pa, pb;
      WorldOf(m, basis, unit, Q{ 0, 0, 0, 1 }, a, pa);
      WorldOf(nodes[i]->node_to_world, basis, unit, Q{ 0, 0, 0, 1 }, b, pb);
      bindOff = AngleDeg(a, b) > bindOff ? AngleDeg(a, b) : bindOff;
      bindMoved = Len(Sub(pa, pb)) > bindMoved ? Len(Sub(pa, pb)) : bindMoved;
    }
  }

  // Faced +Z by the legs (their left onto +X)
  Q yaw = { 0, 0, 0, 1 };
  std::vector<Q> restQ(bones);
  std::vector<V> restP(bones);
  double theta = 0.0;
  auto face = [&]()
  {
    yaw = { 0, 0, 0, 1 };
    theta = 0.0;
    for (int i = 0; i < bones; ++i)
      WorldOf(restWorld[i], basis, unit, yaw, restQ[i], restP[i]);
    int l = indexOf("LeftUpLeg"), r = indexOf("RightUpLeg");
    if (l < 0 || r < 0)
    {
      l = indexOf("LeftArm");
      r = indexOf("RightArm");
    }
    if (l < 0 || r < 0)
      return false;
    V across = Sub(restP[l], restP[r]);
    if (sqrt(across.x * across.x + across.z * across.z) > 1e-3)
    {
      theta = atan2(across.z, across.x);   // turns its left onto +X
      yaw = { 0.0, sin(theta * 0.5), 0.0, cos(theta * 0.5) };
      for (int i = 0; i < bones; ++i)
        WorldOf(restWorld[i], basis, unit, yaw, restQ[i], restP[i]);
    }
    return true;
  };
  // Checks: upright, a T-pose; and whether it's a rest pose at all: standing straight (25 deg), the thighs down (25)
  // and the knees straight (15), the upper arms out to the sides within 60 deg of level and the elbows straight (25):
  // a T- or A-pose (each side's own, so the same with left and right swapped)
  auto unitOf = [&](int a, int b, V& d) -> bool
  {
    if (a < 0 || b < 0)
      return false;
    V e = Sub(restP[b], restP[a]);
    double l = Len(e);
    if (!(l > 1e-9))
      return false;
    d = Scale(e, 1.0 / l);
    return true;
  };
  auto between = [](V a, V b)
  {
    double c = Dot(a, b);
    return acos(c > 1.0 ? 1.0 : (c < -1.0 ? -1.0 : c)) * 57.29577951308232;
  };
  auto restCheck = [&](std::string& checks)
  {
    bool like = true;
    V d;
    if (unitOf(clip->Hips, indexOf("Head"), d))
    {
      double a = between(d, { 0, 1, 0 });
      if (a > 30.0)
      {
        char text[96];
        _snprintf_s(text, sizeof(text), _TRUNCATE, "its rest pose leans %.0f deg off upright; ", a);
        checks += text;
      }
      like = like && a <= 25.0;
    }
    for (const char* side : { "Left", "Right" })
    {
      std::string sd = side;
      V upper, lower;
      if (unitOf(indexOf((sd + "Arm").c_str()), indexOf((sd + "ForeArm").c_str()), upper))
      {
        double down = asin(upper.y > 1.0 ? 1.0 : (upper.y < -1.0 ? -1.0 : upper.y)) * -57.29577951308232;
        if (fabs(down) > 20.0)
        {
          char text[128];
          _snprintf_s(text, sizeof(text), _TRUNCATE, "its rest pose's %s arm is %.0f deg from level (not a T-pose); ", side, down);
          checks += text;
        }
        like = like && fabs(down) <= 60.0 && fabs(upper.z) <= 0.5 * fabs(upper.x);
        if (unitOf(indexOf((sd + "ForeArm").c_str()), indexOf((sd + "Hand").c_str()), lower))
          like = like && between(upper, lower) <= 25.0;
      }
      if (unitOf(indexOf((sd + "UpLeg").c_str()), indexOf((sd + "Leg").c_str()), upper))
      {
        like = like && between(upper, { 0, -1, 0 }) <= 25.0;
        if (unitOf(indexOf((sd + "Leg").c_str()), indexOf((sd + "Foot").c_str()), lower))
          like = like && between(upper, lower) <= 15.0;
      }
    }
    return like;
  };
  // The file's up and unit against its body: some exporters' settings don't say what their data is (ComfyUI's three.js
  // FBX exporter says Z up and centimetres; its bones are in metres, its floor y = 0: the user's "Hit_Knockback_RM"
  // stands along +Y, then lies along -Z, its default pose one of those lying frames; offline 2026-11-11).
  // - Its unit by tens from its legs when they come out under 10 cm or over 10 m long (85 cm thigh and shin; without
  //   legs the Hips to the head, 60 cm). The clip plays the same at any unit (the Hips' move is scaled to the
  //   character's height); the unit counts for the tools' cm-sized checks and the report.
  // - Its up stays its settings' but in two cases, both only when its rest pose doesn't stand by them (head over feet
  //   within 45 deg):
  //   - its skin's bind pose stands as a T- or A-pose along another of its axes (a mesh isn't bound lying down), or,
  //     with its settings shown wrong already (below), its default pose does and is a rest pose kept apart from the
  //     animation (no frame of the clip within 10 deg of it) with a floor along that axis (Flair with wrong axes and
  //     unit; a Blender-style default is a frame of the clip, so a spread-out lying frame isn't taken);
  //   - its settings are shown wrong already (its unit off by tens, or no axes given): the up is taken to be the axis
  //     the body stands along (head over feet within 45 deg) in the most frames (64 at most), of those along which
  //     its lowest point (its floor) moves 1.2 legs at most over the clip (a jump or the knockback's flight; a walk or
  //     a slide moves it along the others), when that beats its settings' up. The knockback: +Y 3 frames and 0.72 m
  //     (-Z 13 frames but 2.97 m; +Z, its settings', 0 frames).
  //   From the motion alone a clip that lies can't always be told from one that stands with wrong axes (a straight
  //   sleeper is a standing idle turned): with its settings shown wrong, a clip that lies more than it stands on a
  //   still floor is stood up. Checks wf_9047616e-e51, wf_a49aabc4-dd4 and wf_29d39494-746 each found such cases
  //   for each rule tried; the bone map's "Up = " line is the way out (it replaces all of this), and the check line
  //   says what was taken.
  // - The bone map's "Unit = " and "Up = " lines replace the unit's and the up's checks.
  //   Its left (+X) is then the file's axis nearest the hips' across, so it faces +Z.
  bool axesFromBody = false, axesFromMap = false;
  {
    auto filePlace = [](ufbx_matrix const& m) { return V{ m.cols[3].x, m.cols[3].y, m.cols[3].z }; };
    auto snap = [](V v)
    {
      V out = { 0, 0, 0 };
      double l = Len(v);
      if (!(l > 1e-12) || !std::isfinite(l))
        return out;
      double ax = fabs(v.x), ay = fabs(v.y), az = fabs(v.z);
      if (ax >= ay && ax >= az)
        out.x = v.x > 0 ? 1.0 : -1.0;
      else if (ay >= az)
        out.y = v.y > 0 ? 1.0 : -1.0;
      else
        out.z = v.z > 0 ? 1.0 : -1.0;
      return out;
    };
    int topI = indexOf("Head") >= 0 ? indexOf("Head") : (indexOf("Neck") >= 0 ? indexOf("Neck") : indexOf("Spine2"));
    int footL = indexOf("LeftFoot"), footR = indexOf("RightFoot");
    auto bodyUp = [&](auto const& placeOf, V& out) -> bool
    {
      if (topI < 0)
        return false;
      V bottom = placeOf(clip->Hips);
      if (footL >= 0 && footR >= 0)
      {
        V l = placeOf(footL), r = placeOf(footR);
        bottom = { (l.x + r.x) * 0.5, (l.y + r.y) * 0.5, (l.z + r.z) * 0.5 };
      }
      V d = Sub(placeOf(topI), bottom);
      if (!(Len(d) > 1e-9) || !std::isfinite(Len(d)))
        return false;
      out = Scale(d, 1.0 / Len(d));
      return true;
    };
    // The legs' length (file units): the unit's check and the floor's test
    double legs = 0.0;
    int legCount = 0;
    for (const char* side : { "Left", "Right" })
    {
      int a = indexOf((std::string(side) + "UpLeg").c_str()), b = indexOf((std::string(side) + "Leg").c_str()),
        c = indexOf((std::string(side) + "Foot").c_str());
      if (a < 0 || b < 0 || c < 0)
        continue;
      legs += Len(Sub(filePlace(restWorld[b]), filePlace(restWorld[a]))) +
        Len(Sub(filePlace(restWorld[c]), filePlace(restWorld[b])));
      ++legCount;
    }
    double length = 0.0, typical = 0.85;
    if (legCount > 0)
      length = legs / legCount;
    else if (topI >= 0)
    {
      length = Len(Sub(filePlace(restWorld[topI]), filePlace(restWorld[clip->Hips])));
      typical = 0.6;
    }
    double legFile = length * 0.85 / typical;   // a leg's length in the file's units
    // The unit (from the map, the settings' unit is still checked: shown wrong, the up's check below still runs)
    double metres = length * unit;
    bool unitWrong = std::isfinite(metres) && metres > 1e-12 && (metres < 0.1 || metres > 10.0);
    if (mapUnit > 0.0)
    {
      unitWrong = unitWrong || mapUnit / unit > 3.0 || unit / mapUnit > 3.0;
      char text[200];
      if (s->settings.unit_meters > 0.0)
        _snprintf_s(text, sizeof(text), _TRUNCATE, "its unit from its bone map: %.3g m (its settings %.3g m; its legs "
          "%.2f m); ", mapUnit, unit, length * mapUnit);
      else
        _snprintf_s(text, sizeof(text), _TRUNCATE, "its unit from its bone map: %.3g m (it gives none; its legs %.2f m); ",
          mapUnit, length * mapUnit);
      warnings += text;
      clip->UnitFromMap = true;
      bindMoved *= mapUnit / unit;
      placedMove *= mapUnit / unit;
      unit = mapUnit;
      clip->Unit = unit;
    }
    else if (unitWrong)
    {
      double tens = floor(log10(typical / metres) + 0.5);
      double newUnit = unit * pow(10.0, tens);
      char text[200];
      _snprintf_s(text, sizeof(text), _TRUNCATE, "its unit (%.3g m) makes its %s %.3g m long: taken as %.3g m, %.2f m; ",
        unit, legCount > 0 ? "legs" : "Hips to head", metres, newUnit, length * newUnit);
      warnings += text;
      bindMoved *= newUnit / unit;   // (measured with the settings' unit above)
      placedMove *= newUnit / unit;
      unit = newUnit;
      clip->Unit = unit;
    }
    // The up
    V restUp, declaredUp = { basis[1][0], basis[1][1], basis[1][2] };
    int acrossL = indexOf("LeftUpLeg"), acrossR = indexOf("RightUpLeg");
    if (acrossL < 0 || acrossR < 0)
    {
      acrossL = indexOf("LeftArm");
      acrossR = indexOf("RightArm");
    }
    V across = acrossL >= 0 && acrossR >= 0 ? Sub(filePlace(restWorld[acrossL]), filePlace(restWorld[acrossR])) :
      V{ 0, 0, 0 };
    // A basis with up u: its left the file's axis nearest the hips' across off u (else its settings' right or front)
    auto basisFor = [&](V u, double out[3][3]) -> bool
    {
      V left = { 0, 0, 0 };
      for (V from : { across, V{ basis[0][0], basis[0][1], basis[0][2] }, V{ basis[2][0], basis[2][1], basis[2][2] } })
      {
        V off = Sub(from, Scale(u, Dot(from, u)));
        if (Len(off) > 0.2 * Len(from) && Len(off) > 1e-12)
        {
          left = snap(off);
          break;
        }
      }
      if (!(Len(left) > 0.5) || fabs(Dot(left, u)) > 0.5)
        return false;
      V front = Cross(left, u);
      V rows[3] = { left, u, front };
      for (int r = 0; r < 3; ++r)
      {
        out[r][0] = rows[r].x;
        out[r][1] = rows[r].y;
        out[r][2] = rows[r].z;
      }
      return true;
    };
    if (Len(mapUp) > 0.5)
    {
      double cb[3][3];
      clip->UpFromMap = true;
      if (axesOk && Dot(mapUp, declaredUp) > 0.5)
        warnings += "its up from its bone map: " + AxisName(mapUp) + ", its settings' own (kept as they are); ";
      else if (basisFor(mapUp, cb))
      {
        memcpy(basis, cb, sizeof(basis));
        for (int r = 0; r < 3; ++r)
          for (int c = 0; c < 3; ++c)
            clip->Basis[r][c] = basis[r][c];
        axesFromBody = true;   // (its left from the hips' across, as from the body)
        axesFromMap = true;
        warnings += "its up from its bone map: " + AxisName(mapUp) + (axesOk ? " (its settings " + AxisName(declaredUp) :
          std::string(" (it gives no axes")) + "; its left " + AxisName({ cb[0][0], cb[0][1], cb[0][2] }) + "); ";
      }
      else
      {
        clip->UpFromMap = false;
        warnings += "its bone map's up (" + AxisName(mapUp) + ") runs along its hips (not used); ";
      }
    }
    else if (bodyUp([&](int i) { return filePlace(restWorld[i]); }, restUp) && Dot(restUp, declaredUp) < 0.7071 &&
        legFile > 1e-9 && (bound > 0 || unitWrong || !axesOk))
    {
      double saved[3][3], cb[3][3];
      memcpy(saved, basis, sizeof(basis));
      std::string ignored, how;
      V take = { 0, 0, 0 };
      bool suspect = unitWrong || !axesOk;
      // The frames' figures (64 frames at most; worked out when a rule needs them): along each of the six axes the
      // spread over the clip of the body's lowest point, and the frames whose head over feet lies within 45 deg of it
      V const ups[6] = { { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 } };
      double lowMin[6], lowMax[6];
      int standing[6] = { 0, 0, 0, 0, 0, 0 }, looked = 0;
      bool figured = false;
      auto figure = [&]()
      {
        if (figured)
          return;
        figured = true;
        for (int u = 0; u < 6; ++u)
        {
          lowMin[u] = 1e300;
          lowMax[u] = -1e300;
        }
        int samples = frames < 64 ? frames : 64;
        for (int n = 0; n < samples; ++n)
        {
          int k = samples > 1 ? (int)((long long)n * (frames - 1) / (samples - 1)) : 0;
          double time = stack->time_begin + k / clip->Fps;
          if (time > stack->time_end)
            time = stack->time_end;
          SceneFree at{ ufbx_evaluate_scene(s, stack->anim, time, nullptr, &ufbxError) };
          if (!at.Scene)
            break;   // (the bake says why)
          auto placeAt = [&](int i) { return filePlace(at.Scene->nodes.data[nodes[i]->typed_id]->node_to_world); };
          double low[6] = { 1e300, 1e300, 1e300, 1e300, 1e300, 1e300 };
          bool finite = true;
          for (int i = 0; i < bones; ++i)
          {
            V pl = placeAt(i);
            finite = finite && std::isfinite(pl.x) && std::isfinite(pl.y) && std::isfinite(pl.z);
            for (int u = 0; u < 6; ++u)
              low[u] = Dot(pl, ups[u]) < low[u] ? Dot(pl, ups[u]) : low[u];
          }
          if (!finite)
            continue;
          ++looked;
          V bu;
          bool hasUp = bodyUp(placeAt, bu);
          for (int u = 0; u < 6; ++u)
          {
            lowMin[u] = low[u] < lowMin[u] ? low[u] : lowMin[u];
            lowMax[u] = low[u] > lowMax[u] ? low[u] : lowMax[u];
            standing[u] += hasUp && Dot(bu, ups[u]) > 0.7071 ? 1 : 0;
          }
        }
      };
      auto axisAt = [&](V v)
      {
        for (int u = 0; u < 6; ++u)
          if (Dot(ups[u], v) > 0.5)
            return u;
        return -1;
      };
      auto spreadOf = [&](int u) { return lowMax[u] - lowMin[u]; };
      // 1. The rest pose stands as a T- or A-pose along another axis: a skin's bind pose; a default pose only with its
      //    settings shown wrong, when it's kept apart from the animation (no frame within 10 deg of it, every bone
      //    against the Hips: a Blender-style default is a frame of the clip) and the body has a floor along that axis
      V cand = snap(restUp);
      if ((bound > 0 || suspect) && Dot(cand, declaredUp) < 0.5 && basisFor(cand, cb))
      {
        face();   // (the rest places by its settings)
        bool standsOld = restCheck(ignored);
        memcpy(basis, cb, sizeof(basis));
        face();
        bool standsNew = restCheck(ignored);
        memcpy(basis, saved, sizeof(basis));
        face();
        if (standsNew && !standsOld && bound > 0)
        {
          take = cand;
          how = "its skin's bind pose stands along it as a T- or A-pose";
        }
        else if (standsNew && !standsOld)
        {
          figure();
          int c = axisAt(cand);
          if (looked > 0 && c >= 0 && spreadOf(c) <= 1.2 * legFile)
          {
            // (the nearest frame: its worst bone's turn against the Hips from the default pose's, over every frame)
            auto against = [&](ufbx_matrix const& hips, ufbx_matrix const& bone)
            {
              Q h, b;
              V ph, pb;
              WorldOf(hips, saved, 1.0, Q{ 0, 0, 0, 1 }, h, ph);
              WorldOf(bone, saved, 1.0, Q{ 0, 0, 0, 1 }, b, pb);
              return Norm(Mul(Conj(h), b));
            };
            std::vector<Q> rest(bones);
            for (int i = 0; i < bones; ++i)
              rest[i] = against(restWorld[clip->Hips], restWorld[i]);
            double nearest = 1e30;
            for (int k = 0; k < frames && nearest > 10.0; ++k)
            {
              double time = stack->time_begin + k / clip->Fps;
              if (time > stack->time_end)
                time = stack->time_end;
              SceneFree at{ ufbx_evaluate_scene(s, stack->anim, time, nullptr, &ufbxError) };
              if (!at.Scene)
              {
                nearest = 0.0;   // (unread: taken as a frame, not as a rest pose)
                break;
              }
              ufbx_matrix const& hips = at.Scene->nodes.data[nodes[clip->Hips]->typed_id]->node_to_world;
              double worst = 0.0;
              for (int i = 0; i < bones && worst <= nearest; ++i)
              {
                double a = AngleDeg(rest[i], against(hips, at.Scene->nodes.data[nodes[i]->typed_id]->node_to_world));
                worst = std::isfinite(a) && a > worst ? a : worst;
              }
              nearest = worst < nearest ? worst : nearest;
            }
            if (nearest > 10.0)
            {
              take = cand;
              char text[240];
              _snprintf_s(text, sizeof(text), _TRUNCATE, "its default pose stands along it as a T- or A-pose, apart from its "
                "frames (the nearest %.0f deg off), and along it its lowest point moves %.2f legs over the clip", nearest,
                spreadOf(c) / legFile);
              how = text;
            }
          }
        }
      }
      // 2. Settings shown wrong already: the up the body stands along in the most frames, of those with a floor
      if (Len(take) < 0.5 && suspect)
      {
        figure();
        int declaredAt = axisAt(declaredUp), best = -1;
        for (int u = 0; u < 6 && looked > 0; ++u)
        {
          if (!(spreadOf(u) <= 1.2 * legFile) || standing[u] == 0)
            continue;
          if (best < 0 || standing[u] > standing[best] || (standing[u] == standing[best] && spreadOf(u) < spreadOf(best)))
            best = u;
        }
        if (best >= 0 && best != declaredAt && (declaredAt < 0 || standing[best] > standing[declaredAt]) &&
            basisFor(ups[best], cb))
        {
          take = ups[best];
          char text[240];
          _snprintf_s(text, sizeof(text), _TRUNCATE, "its body stands along it in %d of %d frames, along its settings' up "
            "in %d, and along it its lowest point moves %.2f legs over the clip; if that's wrong, its bone map's Up = line "
            "sets it", standing[best], looked, declaredAt >= 0 ? standing[declaredAt] : 0, spreadOf(best) / legFile);
          how = text;
        }
        else if (looked > 0 && declaredAt >= 0 && standing[declaredAt] == 0)   // (it never stands by them)
          warnings += "its rest pose doesn't stand along " + AxisName(declaredUp) + (!axesOk ? " (it gives no axes)" :
            " (its settings' up, and their unit is off)") + ", but its frames can't tell another up (kept): if it plays "
            "lying or upside down, its bone map's Up = line sets it; ";
      }
      if (Len(take) > 0.5 && basisFor(take, cb))
      {
        memcpy(basis, cb, sizeof(basis));
        for (int r = 0; r < 3; ++r)
          for (int c = 0; c < 3; ++c)
            clip->Basis[r][c] = basis[r][c];
        axesFromBody = true;
        warnings += (axesOk ? "its settings say " + AxisName(declaredUp) + " is up" : std::string("it gives no axes")) +
          ", but its up is " + AxisName(take) + " (" + how + "; its left " + AxisName({ cb[0][0], cb[0][1], cb[0][2] }) +
          "); ";
      }
    }
  }
  bool faced = face();
  if (!faced)
    warnings += "no legs or arms to tell which way it faces (taken as +Z); ";
  std::string restChecks;
  bool restLike = restCheck(restChecks);
  clip->NoRest = bound == 0 && !restLike;
  // With a rest pose its toes must point forward: if they point back, its left and right were named the other way
  // round, so they're swapped and it's faced again. (Without one, from its standing frames: below.)
  if (faced && !clip->NoRest)
  {
    double ahead = 0.0;
    int feet = 0;
    for (const char* side : { "Left", "Right" })
    {
      int f = indexOf((std::string(side) + "Foot").c_str()), t = indexOf((std::string(side) + "ToeBase").c_str());
      if (f >= 0 && t >= 0)
      {
        ahead += restP[t].z - restP[f].z;
        ++feet;
      }
    }
    if (feet > 0 && ahead < -0.01 * feet)
    {
      for (std::string& b : clip->Bones)
        if (b.compare(0, 4, "Left") == 0)
          b = "Right" + b.substr(4);
        else if (b.compare(0, 5, "Right") == 0)
          b = "Left" + b.substr(5);
      warnings += "its left and right bones were named the other way round (swapped); ";
      face();
      restChecks.clear();
      restCheck(restChecks);
    }
  }
  if (!clip->NoRest)
    warnings += restChecks;
  // No rest pose: kept in the file's own axes (its front +Z), not faced by its default pose: a clip's poses may stand
  // turned anyhow (UE5 fighting clips 55 - 58 deg off their rest on average, offline 2026-11-11)
  if (clip->NoRest && theta != 0.0)
  {
    yaw = { 0, 0, 0, 1 };
    theta = 0.0;
    for (int i = 0; i < bones; ++i)
      WorldOf(restWorld[i], basis, unit, yaw, restQ[i], restP[i]);
  }
  {
    char skinText[64] = "no skin", movedText[96] = "";
    if (partial > 0)
      _snprintf_s(skinText, sizeof(skinText), _TRUNCATE, "its skin covers only %d of its bones", partial);
    if (placedMove > 0.001 || placedTurn > 0.05)
      _snprintf_s(movedText, sizeof(movedText), _TRUNCATE, "; its rig moved %.1f cm and turned %.1f deg after binding (followed)", placedMove * 100.0,
        placedTurn);
    char text[300];
    if (bound > 0)
      _snprintf_s(text, sizeof(text), _TRUNCATE, "its skin's bind pose (%d bones; its default pose %s: %.2f deg, %.1f cm at most%s)", bound,
        bindOff <= 0.5 && bindMoved <= 0.005 ? "the same" : "differs", bindOff, bindMoved * 100.0, movedText);
    else if (clip->NoRest)
      _snprintf_s(text, sizeof(text), _TRUNCATE, "none in the file (%s, and its default pose isn't a T- or A-pose): its default pose with the "
        "bones' frames from the body", skinText);
    else
      _snprintf_s(text, sizeof(text), _TRUNCATE, "its default pose (%s)", skinText);
    clip->RestFrom = text;
  }

  // No rest pose: whether its left and right were named the other way round, from up to 64 of its frames where a
  // leg stands (its shin within 45 deg of upright): there the toes must point ahead of the hips (across them, up)
  if (clip->NoRest)
  {
    int lt = indexOf("LeftUpLeg"), rt = indexOf("RightUpLeg");
    double ahead = 0.0;
    int looked = 0;
    int samples = frames < 64 ? frames : 64;
    for (int n = 0; n < samples && lt >= 0 && rt >= 0; ++n)
    {
      int k = samples > 1 ? (int)((long long)n * (frames - 1) / (samples - 1)) : 0;
      double time = stack->time_begin + k / clip->Fps;
      if (time > stack->time_end)
        time = stack->time_end;
      SceneFree at{ ufbx_evaluate_scene(s, stack->anim, time, nullptr, &ufbxError) };
      if (!at.Scene)
        break;   // (the bake says why)
      auto placeOf = [&](int i)
      {
        Q q;
        V p;
        WorldOf(at.Scene->nodes.data[nodes[i]->typed_id]->node_to_world, basis, unit, yaw, q, p);
        return p;
      };
      V across = Sub(placeOf(lt), placeOf(rt));
      across.y = 0.0;
      if (!(Len(across) > 1e-6))
        continue;
      V forward = { -across.z / Len(across), 0.0, across.x / Len(across) };
      for (const char* side : { "Left", "Right" })
      {
        int f = indexOf((std::string(side) + "Foot").c_str()), toe = indexOf((std::string(side) + "ToeBase").c_str()),
          sh = indexOf((std::string(side) + "Leg").c_str());
        if (f < 0 || toe < 0)
          continue;
        if (sh >= 0)
        {
          V shin = Sub(placeOf(sh), placeOf(f));
          if (!(Len(shin) > 1e-6 && shin.y >= 0.7071 * Len(shin)))
            continue;
        }
        double d = Dot(Sub(placeOf(toe), placeOf(f)), forward);
        if (std::isfinite(d))
        {
          ahead += d;
          ++looked;
        }
      }
    }
    if (looked > 0 && ahead < -0.01 * looked)
    {
      for (std::string& b : clip->Bones)
        if (b.compare(0, 4, "Left") == 0)
          b = "Right" + b.substr(4);
        else if (b.compare(0, 5, "Right") == 0)
          b = "Left" + b.substr(5);
      warnings += "its left and right bones were named the other way round (swapped, by its standing frames); ";
      if (axesFromBody)
      {
        // (its left, +X, was taken from the names: turned round about up so it faces +Z)
        for (int c = 0; c < 3; ++c)
        {
          basis[0][c] = -basis[0][c];
          basis[2][c] = -basis[2][c];
          clip->Basis[0][c] = basis[0][c];
          clip->Basis[2][c] = basis[2][c];
        }
        for (int i = 0; i < bones; ++i)
          WorldOf(restWorld[i], basis, unit, yaw, restQ[i], restP[i]);
        warnings += std::string(axesFromMap ? "its up from its bone map" : "its up from the body") + " turned round with them "
          "(its left " + AxisName({ basis[0][0], basis[0][1], basis[0][2] }) + "); ";
      }
    }
  }

  // Baked: each frame's world rotations and the Hips' place
  clip->Q.resize((size_t)frames * bones * 4);
  clip->HipsP.resize((size_t)frames * 3);
  // Each elbow's hinge: over the frames where the arm bends more than 20 deg, the forearm-and-hand plane's axis in
  // the upper arm's frame (a hinge keeps it there), averaged; and in the forearm's own frame
  // (and each knee's: the thigh, shin and foot)
  int armOf[2] = { indexOf("LeftArm"), indexOf("RightArm") }, foreOf[2] = { indexOf("LeftForeArm"), indexOf("RightForeArm") },
    handOf[2] = { indexOf("LeftHand"), indexOf("RightHand") };
  int thighOf[2] = { indexOf("LeftUpLeg"), indexOf("RightUpLeg") }, shinOf[2] = { indexOf("LeftLeg"), indexOf("RightLeg") },
    footOf[2] = { indexOf("LeftFoot"), indexOf("RightFoot") };
  int toeOf[2] = { indexOf("LeftToeBase"), indexOf("RightToeBase") }, tipOf[2] = { indexOf("LeftToe_End"), indexOf("RightToe_End") };
  V hingeSum[2] = { { 0, 0, 0 }, { 0, 0, 0 } }, kneeSum[2] = { { 0, 0, 0 }, { 0, 0, 0 } };
  V hingeLowerSum[2] = { { 0, 0, 0 }, { 0, 0, 0 } }, kneeLowerSum[2] = { { 0, 0, 0 }, { 0, 0, 0 } };
  int hingeFrames[2] = { 0, 0 }, kneeFrames[2] = { 0, 0 };
  std::vector<Q> frameQ(bones);
  std::vector<V> frameP(bones);
  // Each foot's ankle, ball, toe tip and knee per frame (where it stands flattest, without a rest pose)
  std::vector<V> footTrack[2][4];
  for (int side = 0; side < 2; ++side)
    for (int j = 0; j < 4; ++j)
      footTrack[side][j].assign(frames, V{ 0, 0, 0 });
  // No rest pose: each bone's side axis from the body, per frame: across the hips (the thighs, the hips' own) for the
  // hips and legs, across the chest (the collarbones, the chest's own) for the chest, neck and head, the two blended
  // up the spine, the chest's up for the collarbones, the knuckles (Pinky1 - Index1) for the fingers and the chest's
  // front for an arm or hand (with no hinge or knuckles); added up in the bone's own frame over the clip, so a turned
  // head or a shrug in one pose doesn't set it
  enum { RefHips, RefChest, RefSpine, RefSpine1, RefUp, RefFront, RefKnucklesL, RefKnucklesR };
  struct BodyRefs
  {
    V Hips, Chest, Up, Front, Knuckles[2];
  };
  // (each pair by their places in the default pose: two at one place, a BVH rig's shoulders at the chest's, say
  // nothing, so the next pair)
  auto apart = [&](int a, int b) { return a >= 0 && b >= 0 && Len(Sub(restP[a], restP[b])) > 0.01; };
  int legL = thighOf[0], legR = thighOf[1];
  if (!apart(legL, legR))
    legL = legR = -1;
  int chestL = indexOf("LeftShoulder"), chestR = indexOf("RightShoulder");
  if (!apart(chestL, chestR))
  {
    chestL = armOf[0];
    chestR = armOf[1];
  }
  if (!apart(chestL, chestR))
    chestL = chestR = -1;
  int upFrom = -1, upTo = -1;
  {
    static char const* const ups[][2] = { { "Spine2", "Neck" }, { "Spine2", "Head" }, { "Spine1", "Neck" },
      { "Hips", "Neck" }, { "Hips", "Head" } };
    for (auto const& u : ups)
      if (apart(indexOf(u[0]), indexOf(u[1])))
      {
        upFrom = indexOf(u[0]);
        upTo = indexOf(u[1]);
        clip->UpFrom = u[0];
        clip->UpTo = u[1];
        break;
      }
  }
  int pinkyOf[2] = { indexOf("LeftHandPinky1"), indexOf("RightHandPinky1") },
    indexFingerOf[2] = { indexOf("LeftHandIndex1"), indexOf("RightHandIndex1") };
  auto unitBetween = [](std::vector<V> const& p, int from, int to, V fallback)
  {
    if (from < 0 || to < 0)
      return fallback;
    V d = Sub(p[to], p[from]);
    return Len(d) > 1e-6 ? Scale(d, 1.0 / Len(d)) : fallback;
  };
  auto refsOf = [&](std::vector<V> const& p)
  {
    BodyRefs r;
    bool hips = legL >= 0 && legR >= 0, chest = chestL >= 0 && chestR >= 0;
    r.Hips = unitBetween(p, legR, legL, { 1, 0, 0 });
    r.Chest = unitBetween(p, chestR, chestL, { 1, 0, 0 });
    if (!hips && chest)
      r.Hips = r.Chest;
    if (!chest && hips)
      r.Chest = r.Hips;
    r.Up = unitBetween(p, upFrom, upTo, { 0, 1, 0 });
    r.Front = Cross(r.Chest, r.Up);
    r.Front = Len(r.Front) > 1e-6 ? Scale(r.Front, 1.0 / Len(r.Front)) : V{ 0, 0, 1 };
    for (int side = 0; side < 2; ++side)
      r.Knuckles[side] = unitBetween(p, indexFingerOf[side], pinkyOf[side], r.Front);
    return r;
  };
  auto blend = [](V a, V b, double t)
  {
    V m = { a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t };
    return Len(m) > 1e-6 ? Scale(m, 1.0 / Len(m)) : a;
  };
  auto pick = [&](BodyRefs const& r, int kind) -> V
  {
    switch (kind)
    {
    case RefHips: return r.Hips;
    case RefSpine: return blend(r.Hips, r.Chest, 1.0 / 3.0);
    case RefSpine1: return blend(r.Hips, r.Chest, 2.0 / 3.0);
    case RefUp: return r.Up;
    case RefFront: return r.Front;
    case RefKnucklesL: return r.Knuckles[0];
    case RefKnucklesR: return r.Knuckles[1];
    default: return r.Chest;
    }
  };
  std::vector<int> refKind(bones, RefChest);
  for (int i = 0; i < bones; ++i)
  {
    std::string const& name = clip->Bones[i];
    std::string side;
    int kind = SideKind(name, side);
    bool lower = false;
    for (char const* part : { "UpLeg", "Leg", "Foot", "ToeBase", "Toe_End" })
      lower = lower || name == std::string("Left") + part || name == std::string("Right") + part;
    if (kind == 1)
      refKind[i] = RefUp;
    else if (kind == 2)
      refKind[i] = name.size() > side.size() + 4 && name.compare(side.size(), 4, "Hand") == 0 ?
        (side == "Left" ? RefKnucklesL : RefKnucklesR) : RefFront;
    else if (name == "Hips" || lower)
      refKind[i] = RefHips;
    else if (name == "Spine")
      refKind[i] = RefSpine;
    else if (name == "Spine1")
      refKind[i] = RefSpine1;
  }
  std::vector<V> refSum(clip->NoRest ? bones : 0, V{ 0, 0, 0 });
  int headAim = clip->NoRest && indexOf("Head") >= 0 && indexOf("HeadTop_End") < 0 ? indexOf("Head") : -1;
  // (and each bone whose aim hangs below another of the file's bones: its aim's place in its own frame)
  std::vector<int> aimPast(bones, -1);
  std::vector<V> aimSum(bones, V{ 0, 0, 0 });
  int aimFrames = 0;
  if (clip->NoRest)
    for (int i = 0; i < bones; ++i)
    {
      int a = indexOf(AimOf(clip->Bones[i]).c_str());
      if (a < 0 || nodes[a]->parent == nodes[i])
        continue;
      for (ufbx_node* up = nodes[a]->parent; up; up = up->parent)
        if (up == nodes[i])
        {
          aimPast[i] = a;
          break;
        }
    }
  V headUpSum = { 0, 0, 0 };
  // A joint's bend axis in its upper and lower bones' frames, added up when it bends more than 20 deg
  auto addHinge = [&](int a, int b, int c, V& sum, V& lowerSum, int& count)
  {
    if (a < 0 || b < 0 || c < 0)
      return;
    V upper = Sub(frameP[b], frameP[a]), lower = Sub(frameP[c], frameP[b]);
    double lu = Len(upper), ll = Len(lower);
    if (!(lu > 1e-6 && ll > 1e-6) || Dot(upper, lower) / (lu * ll) > 0.9396926)   // cos 20 deg
      return;
    V hinge = Cross(upper, lower);
    double lh = Len(hinge);
    if (!(lh > 1e-9))
      return;
    V local = Rot(Conj(frameQ[a]), Scale(hinge, 1.0 / lh));
    sum = { sum.x + local.x, sum.y + local.y, sum.z + local.z };
    V lowerLocal = Rot(Conj(frameQ[b]), Scale(hinge, 1.0 / lh));
    lowerSum = { lowerSum.x + lowerLocal.x, lowerSum.y + lowerLocal.y, lowerSum.z + lowerLocal.z };
    ++count;
  };
  for (int k = 0; k < frames; ++k)
  {
    double time = stack->time_begin + k / clip->Fps;
    if (time > stack->time_end)
      time = stack->time_end;
    SceneFree at{ ufbx_evaluate_scene(s, stack->anim, time, nullptr, &ufbxError) };
    if (!at.Scene)
    {
      char text[512];
      ufbx_format_error(text, sizeof(text), &ufbxError);
      error = std::string("its animation couldn't be read at frame ") + std::to_string(k) + ": " + text;
      return nullptr;
    }
    for (int i = 0; i < bones; ++i)
    {
      ufbx_node* node = at.Scene->nodes.data[nodes[i]->typed_id];
      Q q;
      V p;
      WorldOf(node->node_to_world, basis, unit, yaw, q, p);
      frameQ[i] = q;
      frameP[i] = p;
      float* out = &clip->Q[((size_t)k * bones + i) * 4];
      out[0] = (float)q.x; out[1] = (float)q.y; out[2] = (float)q.z; out[3] = (float)q.w;
      if (i == clip->Hips)
      {
        float* h = &clip->HipsP[(size_t)k * 3];
        h[0] = (float)p.x; h[1] = (float)p.y; h[2] = (float)p.z;
      }
    }
    for (int side = 0; side < 2; ++side)
    {
      addHinge(armOf[side], foreOf[side], handOf[side], hingeSum[side], hingeLowerSum[side], hingeFrames[side]);
      addHinge(thighOf[side], shinOf[side], footOf[side], kneeSum[side], kneeLowerSum[side], kneeFrames[side]);
      int track[4] = { footOf[side], toeOf[side], tipOf[side], shinOf[side] };
      for (int j = 0; j < 4; ++j)
        if (track[j] >= 0)
          footTrack[side][j][k] = frameP[track[j]];
    }
    if (clip->NoRest)
    {
      BodyRefs r = refsOf(frameP);
      for (int i = 0; i < bones; ++i)
      {
        V local = Rot(Conj(frameQ[i]), pick(r, refKind[i]));
        refSum[i] = { refSum[i].x + local.x, refSum[i].y + local.y, refSum[i].z + local.z };
      }
      for (int i = 0; i < bones; ++i)
        if (aimPast[i] >= 0)
        {
          V local = Rot(Conj(frameQ[i]), Sub(frameP[aimPast[i]], frameP[i]));
          aimSum[i] = { aimSum[i].x + local.x, aimSum[i].y + local.y, aimSum[i].z + local.z };
        }
      ++aimFrames;
      if (headAim >= 0)
      {
        V local = Rot(Conj(frameQ[headAim]), r.Up);
        headUpSum = { headUpSum.x + local.x, headUpSum.y + local.y, headUpSum.z + local.z };
      }
    }
  }

  if (fabs(theta) > 0.5 * Pi / 180.0)
  {
    char text[128];
    _snprintf_s(text, sizeof(text), _TRUNCATE, "turned %.0f deg about up to face +Z; ", theta * 57.29577951308232);
    warnings += text;
  }
  clip->Yaw[0] = (float)yaw.x;
  clip->Yaw[1] = (float)yaw.y;
  clip->Yaw[2] = (float)yaw.z;
  clip->Yaw[3] = (float)yaw.w;
  clip->RestQ.resize((size_t)bones * 4);
  clip->RestP.resize((size_t)bones * 3);
  for (int i = 0; i < bones; ++i)
  {
    float* q = &clip->RestQ[(size_t)i * 4];
    q[0] = (float)restQ[i].x; q[1] = (float)restQ[i].y; q[2] = (float)restQ[i].z; q[3] = (float)restQ[i].w;
    float* p = &clip->RestP[(size_t)i * 3];
    p[0] = (float)restP[i].x; p[1] = (float)restP[i].y; p[2] = (float)restP[i].z;
  }
  for (int k = 0; k < 3; ++k)
    clip->HipsRest[k] = clip->RestP[(size_t)clip->Hips * 3 + k];

  char hingeText[96] = "the elbows don't bend (the knuckles turn the upper arms)";
  auto put3 = [](float* out, V v) { out[0] = (float)v.x; out[1] = (float)v.y; out[2] = (float)v.z; };
  for (int side = 0; side < 2; ++side)
    if (hingeFrames[side] > 0 && Len(hingeSum[side]) > 1e-6)
    {
      put3(clip->ElbowHinge[side], Rot(restQ[armOf[side]], Scale(hingeSum[side], 1.0 / Len(hingeSum[side]))));
      put3(clip->ElbowHingeLower[side], Len(hingeLowerSum[side]) > 1e-6 ? Rot(restQ[foreOf[side]],
        Scale(hingeLowerSum[side], 1.0 / Len(hingeLowerSum[side]))) : Rot(restQ[armOf[side]], Scale(hingeSum[side],
        1.0 / Len(hingeSum[side]))));
      clip->ElbowHingeOk[side] = true;
    }
  for (int side = 0; side < 2; ++side)
    if (kneeFrames[side] > 0 && Len(kneeSum[side]) > 1e-6)
    {
      put3(clip->KneeHinge[side], Rot(restQ[thighOf[side]], Scale(kneeSum[side], 1.0 / Len(kneeSum[side]))));
      put3(clip->KneeHingeLower[side], Len(kneeLowerSum[side]) > 1e-6 ? Rot(restQ[shinOf[side]],
        Scale(kneeLowerSum[side], 1.0 / Len(kneeLowerSum[side]))) : Rot(restQ[thighOf[side]], Scale(kneeSum[side],
        1.0 / Len(kneeSum[side]))));
      clip->KneeHingeOk[side] = true;
    }
  if (clip->ElbowHingeOk[0] || clip->ElbowHingeOk[1] || clip->KneeHingeOk[0] || clip->KneeHingeOk[1])
    _snprintf_s(hingeText, sizeof(hingeText), _TRUNCATE, "the elbows' hinges from %d and %d bent frames, the knees' from %d and %d", hingeFrames[0],
      hingeFrames[1], kneeFrames[0], kneeFrames[1]);

  // No rest pose: each bone's side axis, its sum over the clip in its own frame (above), carried to the default pose
  // (world; that pose's own if it never points anywhere). The feet and toes take the frame where each foot stands
  // flattest (its ankle, ball and tip nearest their lowest, of the frames where that leg stands) as lying on the
  // ground. The Hips' rest: standing on its legs (the hip joints' drop, the thigh and shin, the ankle at its lowest)
  // over the file's origin, where Mixamo's and UE5's rests stand (the default pose's place is any frame's: 27 cm off
  // in a UE5 clip, offline 2026-11-11).
  char noRestText[320] = "";
  if (clip->NoRest)
  {
    BodyRefs atRest = refsOf(restP);
    clip->RefSide.assign((size_t)bones * 3, 0.f);
    for (int i = 0; i < bones; ++i)
    {
      V local = Len(refSum[i]) > 1e-6 ? Scale(refSum[i], 1.0 / Len(refSum[i])) : Rot(Conj(restQ[i]), pick(atRest, refKind[i]));
      put3(&clip->RefSide[(size_t)i * 3], Rot(restQ[i], local));
    }
    // A bone whose aim hangs below another of the file's bones (UE's spine_03 aims at spine_05 past spine_04, the hand
    // at middle_01 past its metacarpal): its aim's place over the clip in its own frame (the one between may be bent
    // anyhow in the default pose)
    clip->RefDir.assign((size_t)bones * 3, 0.f);
    for (int i = 0; i < bones; ++i)
      if (aimPast[i] >= 0 && aimFrames > 0)
        put3(&clip->RefDir[(size_t)i * 3], Rot(restQ[i], Scale(aimSum[i], 1.0 / aimFrames)));
    clip->RefAim.assign((size_t)bones * 3, 0.f);
    if (headAim >= 0 && Len(headUpSum) > 1e-6)
    {
      V local = Scale(headUpSum, 1.0 / Len(headUpSum));
      put3(&clip->RefAim[(size_t)headAim * 3], Rot(restQ[headAim], local));
    }
    clip->Planted.assign((size_t)bones * 7, 0.f);
    std::string feetText[2] = { "none", "none" }, unplanted;
    double legHeight[2] = { 0.0, 0.0 }, legAnkle[2] = { 0.0, 0.0 };
    bool legOk[2] = { false, false }, legStands[2] = { false, false };
    V hipsUp = { 0, 1, 0 };
    unitOf(clip->Hips, indexOf("Spine"), hipsUp);
    for (int side = 0; side < 2; ++side)
    {
      int track[4] = { footOf[side], toeOf[side], tipOf[side], shinOf[side] };
      if (track[0] < 0)
        continue;
      // (the frames where this leg stands: its knee over its ankle, the shin within 45 deg of upright; a clip lying
      // down has its feet flattest on their sides. A frame with a place that isn't a number is passed over.)
      std::vector<char> stands(frames, 0);
      int standFrames = 0;
      double ankleAll = 1e30;
      for (int k = 0; k < frames; ++k)
      {
        bool finite = true;
        for (int j = 0; j < 4; ++j)
          if (track[j] >= 0)
          {
            V at = footTrack[side][j][k];
            finite = finite && std::isfinite(at.x) && std::isfinite(at.y) && std::isfinite(at.z);
          }
        if (!finite)
          continue;
        if (footTrack[side][0][k].y < ankleAll)
          ankleAll = footTrack[side][0][k].y;
        bool up = true;
        if (track[3] >= 0)
        {
          V shin = Sub(footTrack[side][3][k], footTrack[side][0][k]);
          up = Len(shin) > 1e-6 && shin.y >= 0.7071 * Len(shin);
        }
        stands[k] = up;
        standFrames += up ? 1 : 0;
      }
      double low[3] = { 1e30, 1e30, 1e30 };
      for (int k = 0; k < frames; ++k)
        for (int j = 0; j < 3; ++j)
          if (stands[k] && track[j] >= 0 && footTrack[side][j][k].y < low[j])
            low[j] = footTrack[side][j][k].y;
      int fp = -1;
      double best = 1e30;
      for (int k = 0; k < frames; ++k)
      {
        if (!stands[k])
          continue;
        double off = 0.0;
        for (int j = 0; j < 3; ++j)
          if (track[j] >= 0)
            off += footTrack[side][j][k].y - low[j];
        if (off < best)
        {
          best = off;
          fp = k;
        }
      }
      bool planted = false;
      if (fp >= 0)
      {
        auto plant = [&](int bone, V ahead)
        {
          ahead.y = 0.0;
          if (bone < 0 || !(Len(ahead) > 1e-6))
            return false;
          float* out = &clip->Planted[(size_t)bone * 7];
          memcpy(out, &clip->Q[((size_t)fp * bones + bone) * 4], 4 * sizeof(float));
          put3(out + 4, Scale(ahead, 1.0 / Len(ahead)));
          return true;
        };
        V footAhead = track[1] >= 0 ? Sub(footTrack[side][1][fp], footTrack[side][0][fp]) : V{ 0, 0, 0 };
        footAhead.y = 0.0;
        if (track[2] >= 0)
        {
          V tipAhead = Sub(footTrack[side][2][fp], footTrack[side][0][fp]);
          tipAhead.y = 0.0;
          if (!(Len(footAhead) > 0.25 * Len(tipAhead)))
            footAhead = tipAhead;   // (a ball at or near the ankle: its tip)
        }
        if (plant(track[0], footAhead))
        {
          planted = true;
          V toeAhead = track[1] >= 0 && track[2] >= 0 ? Sub(footTrack[side][2][fp], footTrack[side][1][fp]) : footAhead;
          toeAhead.y = 0.0;
          if (!(Len(toeAhead) > 1e-6))
            toeAhead = footAhead;
          plant(track[1], toeAhead);
          plant(track[2], toeAhead);
          char text[64];
          _snprintf_s(text, sizeof(text), _TRUNCATE, "frame %d, %.1f cm off flat", fp, best * 100.0);
          feetText[side] = text;
        }
      }
      if (!planted)
      {
        feetText[side] = standFrames == 0 ? "none: the leg never stands" : track[1] < 0 && track[2] < 0 ? "none: no toes" :
          "none: its toes don't point ahead where it stands flattest";
        unplanted += std::string(unplanted.empty() ? "" : " and ") + (side == 0 ? "left" : "right");
      }
      // Standing: this leg's length below the Hips, on its ankle at its lowest where it stands (else at its lowest)
      if (thighOf[side] >= 0 && shinOf[side] >= 0 && ankleAll < 1e29)
      {
        legStands[side] = standFrames > 0 && low[0] < 1e29;
        legAnkle[side] = legStands[side] ? low[0] : ankleAll;
        double drop = -Dot(Sub(restP[thighOf[side]], restP[clip->Hips]), hipsUp);
        legHeight[side] = legAnkle[side] + Len(Sub(restP[shinOf[side]], restP[thighOf[side]])) +
          Len(Sub(restP[track[0]], restP[shinOf[side]])) + drop;
        legOk[side] = std::isfinite(legHeight[side]);
      }
    }
    // (the legs that stand; all of them only when none does: a leg held up the whole clip isn't stood on)
    bool anyStands = (legOk[0] && legStands[0]) || (legOk[1] && legStands[1]);
    double standing = 0.0, lowestAnkle = 1e30;
    int legs = 0;
    for (int side = 0; side < 2; ++side)
      if (legOk[side] && (legStands[side] || !anyStands))
      {
        standing += legHeight[side];
        lowestAnkle = fmin(lowestAnkle, legAnkle[side]);
        ++legs;
      }
    clip->HipsRest[0] = clip->HipsRest[2] = 0.f;
    if (legs > 0)
      clip->HipsRest[1] = (float)(standing / legs);
    _snprintf_s(noRestText, sizeof(noRestText), _TRUNCATE, "; no rest pose: the feet's rest where they stand flattest (left: %s; right: %s), the Hips %s "
      "%.2f m high", feetText[0].c_str(), feetText[1].c_str(), legs > 0 ? "standing" : "(no legs) at the default pose's",
      clip->HipsRest[1]);
    warnings += "it has no rest pose (no skin covering its body, and its default pose isn't a T- or A-pose): its bones' "
      "rest frames are worked out from its body and motion; an export with its mesh (skinned) has the exact one; ";
    if (!unplanted.empty())
      warnings += "its " + unplanted + " foot keeps the default pose's turn (its leg never stands, or no toes point "
        "ahead); ";
    if (legs == 0)
      warnings += "its Hips' height is the default pose's (no legs to stand it on); ";
    else if (lowestAnkle > 0.25 || lowestAnkle < -0.25)
    {
      char text[200];
      _snprintf_s(text, sizeof(text), _TRUNCATE, lowestAnkle > 0.0 ? "its %sankles stay %.0f cm or more above the file's "
        "floor (y = 0): the Hips' standing height may be off; " : "its %sankles go %.0f cm below the file's floor (y = 0) at "
        "their lowest: the Hips' standing height may be off; ", anyStands ? "standing " : "", fabs(lowestAnkle) * 100.0);
      warnings += text;
    }
  }

  char report[900];
  std::string creator(s->metadata.creator.data, s->metadata.creator.length);
  _snprintf_s(report, sizeof(report), _TRUNCATE, "\"%s\" (%s): %d frames at %.0f fps (%.2f s), %d of the %d bones a clip can "
    "use, found by %s; %.3g m units; its rest pose: %s; %s%s; FBX %u%s%s", clip->Take.c_str(), clip->File.c_str(), frames,
    clip->Fps, clip->Length(), bones, MixamoCount, clip->BonesFrom.c_str(), unit, clip->RestFrom.c_str(), hingeText,
    noRestText, s->metadata.version, creator.empty() ? "" : " by ", creator.c_str());
  clip->Report = report;
  if (!warnings.empty())
    clip->Warning = warnings.substr(0, warnings.size() - 2);
  return clip;
}

bool pose_clips::SaveBoneMap(Clip const& clip, std::string& message)
{
  if (clip.FromUnity)
  {
    message = "A Unity clip's bones are Unity's human bones: it has no bone map";
    return false;
  }
  FILE* f = nullptr;
  std::string name = Utf8(clip.MapPath.substr(clip.MapPath.find_last_of(L"\\/") == std::wstring::npos ? 0 :
    clip.MapPath.find_last_of(L"\\/") + 1));
  if (_wfopen_s(&f, clip.MapPath.c_str(), L"w") || !f)
  {
    message = name + " couldn't be written";
    return false;
  }
  fprintf(f, "# MEC Cinematic Tools bone map for %s: which of its bones plays each part (the names on the left are\n"
    "# Mixamo's, the tools' names for the parts). Change a name on the right to another of its bones, put # before a line\n"
    "# to leave that part out, and read the clip again. While this file is here, it's used instead of the tools' own\n"
    "# finding; delete it to go back to that. A part needs Hips, the head, hands and feet at least.\n", clip.File.c_str());
  {
    // The file's up and unit as read (the up: the file's axis that goes to the game's up)
    V up = { clip.Basis[1][0], clip.Basis[1][1], clip.Basis[1][2] };
    // (lines in use stay in use)
    fprintf(f, "# The file's up axis and unit as read (up: +X -X +Y -Y +Z -Z; unit: metres per file unit). If the clip\n"
      "# plays lying, upside down, or the wrong size, remove the # before a line, change it and read the clip again:\n"
      "%sUp = %s\n%sUnit = %.6g\n", clip.UpFromMap ? "" : "# ", AxisName(up).c_str(), clip.UnitFromMap ? "" : "# ", clip.Unit);
  }
  for (int r = 0; r < MixamoCount; ++r)
  {
    int found = -1;
    for (size_t k = 0; k < clip.Bones.size() && found < 0; ++k)
      if (clip.Bones[k] == MixamoBones[r])
        found = (int)k;
    if (found >= 0)
      fprintf(f, "%s = %s\n", MixamoBones[r], clip.SourceNames[found].c_str());
    else
      fprintf(f, "# %s = (not found)\n", MixamoBones[r]);
  }
  fclose(f);
  message = "Wrote " + name + " beside the clip: edit it, then pick the clip again";
  return true;
}

bool pose_clips::FitTo(Clip const& clip, char const* asset, int bones, char const* const* names, short const* parents,
  float const* liveLocal, Fit& fit)
{
  fit = Fit();
  for (int i = 0; i < MaxBones; ++i)
  {
    fit.Source[i] = fit.Follows[i] = fit.TwistOf[i] = -1;
    fit.Use[i] = UseTaken;
    fit.TwistKind[i] = 0;
    fit.TwistShare[i] = 0.f;
    for (int k = 0; k < 3; ++k)
      fit.Align[i][k] = fit.FollowTurn[i][k] = fit.RestLocalQ[i][k] = fit.RestLocalT[i][k] = 0.f;
    fit.Align[i][3] = fit.FollowTurn[i][3] = fit.RestLocalQ[i][3] = 1.f;
  }
  if (bones <= 0 || bones > MaxBones)
  {
    fit.Report = "its skeleton is too big";
    return false;
  }
  // The rest pose: the one of its bone list; of two (men's and heeled women's), the one whose bone lengths are the
  // character's own (its local pose's translations)
  double best = 1e30;
  for (int r = 0; r < pose_rest::RestCount; ++r)
  {
    pose_rest::Rest const& rest = pose_rest::Rests[r];
    if (strcmp(rest.Fits, asset) || rest.Bones != bones)
      continue;
    double d = 0.0;
    if (liveLocal)
      for (int i = 0; i < bones; ++i)
        for (int k = 0; k < 3; ++k)
          d += fabs((double)liveLocal[i * 8 + k] - rest.LocalT[i][k]);
    if (d < best)
    {
      best = d;
      fit.Rest = r;
    }
  }
  if (fit.Rest < 0)
  {
    fit.Report = "clips work on Faith, KrugerSec and people with a full skeleton; this one has no rest pose in the tools";
    return false;
  }
  pose_rest::Rest const& rest = pose_rest::Rests[fit.Rest];
  auto target = [&](std::string const& name) -> int
  {
    for (int i = 0; i < bones; ++i)
      if (names[i] && name == names[i])
        return i;
    return -1;
  };
  int clipBones = (int)clip.Bones.size();
  auto source = [&](std::string const& name) -> int
  {
    for (int i = 0; i < clipBones; ++i)
      if (clip.Bones[i] == name)
        return i;
    return -1;
  };
  auto restQ = [&](int s) { float const* q = &clip.RestQ[(size_t)s * 4]; return Q{ q[0], q[1], q[2], q[3] }; };
  auto restP = [&](int s) { float const* p = &clip.RestP[(size_t)s * 3]; return V{ p[0], p[1], p[2] }; };
  // A clip with no rest pose: its bones' side axes from the body, and its feet and toes where they stand flattest
  bool bodySides = clip.NoRest && clip.RefSide.size() == (size_t)clipBones * 3;
  auto refSide = [&](int s) { float const* v = &clip.RefSide[(size_t)s * 3]; return V{ v[0], v[1], v[2] }; };
  auto planted = [&](int s)
  {
    if (!clip.NoRest || clip.Planted.size() != (size_t)clipBones * 7)
      return false;
    float const* v = &clip.Planted[(size_t)s * 7 + 4];
    return v[0] * v[0] + v[1] * v[1] + v[2] * v[2] > 0.25f;
  };
  auto modelQ = [&](int t) { return Q{ rest.ModelQ[t][0], rest.ModelQ[t][1], rest.ModelQ[t][2], rest.ModelQ[t][3] }; };
  auto modelP = [&](int t) { return V{ rest.ModelT[t][0], rest.ModelT[t][1], rest.ModelT[t][2] }; };
  auto put = [](float* out, Q q) { out[0] = (float)q.x; out[1] = (float)q.y; out[2] = (float)q.z; out[3] = (float)q.w; };
  // The game rest's locals (ModelPose = LocalPose x the parent's: local = parent^-1 x model)
  for (int i = 0; i < bones; ++i)
  {
    int p = parents[i];
    put(fit.RestLocalQ[i], p >= 0 && p < bones ? Norm(Mul(Conj(modelQ(p)), modelQ(i))) : modelQ(i));
    for (int k = 0; k < 3; ++k)
      fit.RestLocalT[i][k] = rest.LocalT[i][k];
  }

  // A clip with no rest pose: the game rest chest's up between the same bones as the clip's (Spine2 to Neck...)
  V gameUp = { 0, 1, 0 };
  if (bodySides && !clip.UpFrom.empty())
  {
    int a = target(GameName(clip.UpFrom)), b = target(GameName(clip.UpTo));
    if (a >= 0 && b >= 0 && Len(Sub(modelP(b), modelP(a))) > 1e-6)
      gameUp = Sub(modelP(b), modelP(a));
  }
  std::vector<Q> turnOf(clipBones);
  std::vector<char> hasTurn(clipBones, 0);
  int aligned = 0, byParent = 0, fromRest = 0, hinged = 0, kneeHinged = 0, byFront = 0, byPlanted = 0;
  std::string missing;
  for (int s = 0; s < clipBones; ++s)
  {
    std::string const& name = clip.Bones[s];
    int t = target(GameName(name));
    if (t < 0)
    {
      missing += (missing.empty() ? "" : ", ") + name;
      continue;
    }
    std::string aim = AimOf(name);
    int sa = aim.empty() ? -1 : source(aim), ta = aim.empty() ? -1 : target(GameName(aim));
    bool done = false;
    if (sa >= 0 && ta >= 0 && !FromRestOnly(name))
    {
      // Each rig's rest frame from the bone's direction and a side axis; the turn from the game's onto the clip's
      std::string side;
      int kind = SideKind(name, side);
      V sideS = { 1, 0, 0 }, sideT = { 1, 0, 0 };
      if (kind == 1)
        sideS = sideT = { 0, 1, 0 };
      if (bodySides && kind != 2)
        sideS = refSide(s);   // across the hips or chest, or the chest's up (the game rest's: +X, +Y: its chest's up
                              // put the samba copies' collarbones 12.4 deg off, from 3.8, offline 2026-11-11)
      if (kind == 0)
      {
        // The thigh and the shin: the knee's hinge (the clip's measured; the game rest's from its bent knee), so
        // the knee bends about the game's own hinge
        for (const char* legSide : { "Left", "Right" })
          if (name == std::string(legSide) + "UpLeg" || name == std::string(legSide) + "Leg")
          {
            int k01 = !strcmp(legSide, "Left") ? 0 : 1;
            int tThigh = target(std::string(legSide) + "UpLeg"), tShin = target(std::string(legSide) + "Leg"),
              tFoot = target(std::string(legSide) + "Foot");
            if (!clip.KneeHingeOk[k01] || tThigh < 0 || tShin < 0 || tFoot < 0)
              continue;
            V hinge = Cross(Sub(modelP(tShin), modelP(tThigh)), Sub(modelP(tFoot), modelP(tShin)));
            if (Len(hinge) > 1e-6)
            {
              sideS = { clip.KneeHinge[k01][0], clip.KneeHinge[k01][1], clip.KneeHinge[k01][2] };
              if (clip.NoRest && name == std::string(legSide) + "Leg")
                sideS = { clip.KneeHingeLower[k01][0], clip.KneeHingeLower[k01][1], clip.KneeHingeLower[k01][2] };
              sideT = hinge;
              if (name == std::string(legSide) + "UpLeg")
                ++kneeHinged;
            }
          }
      }
      else if (kind == 2)
      {
        bool set = false;
        int s01 = side == "Left" ? 0 : 1;
        int tArm = target(side + "Arm"), tFore = target(side + "ForeArm"), tHand = target(side + "Hand");
        if ((name == side + "Arm" || name == side + "ForeArm") && clip.ElbowHingeOk[s01] && tArm >= 0 && tFore >= 0 &&
            tHand >= 0)
        {
          // The upper arm and the forearm: the elbow's hinge (the clip's measured; the game rest's from its bent
          // elbow), so the elbow bends about the game's own hinge and keeps the game rest's twist (the wrist takes the
          // rest; a forearm on the knuckles turned 10 deg against the upper arm, offline 2026-11-11)
          V hinge = Cross(Sub(modelP(tFore), modelP(tArm)), Sub(modelP(tHand), modelP(tFore)));
          if (Len(hinge) > 1e-6)
          {
            sideS = { clip.ElbowHinge[s01][0], clip.ElbowHinge[s01][1], clip.ElbowHinge[s01][2] };
            if (clip.NoRest && name == side + "ForeArm")
              sideS = { clip.ElbowHingeLower[s01][0], clip.ElbowHingeLower[s01][1], clip.ElbowHingeLower[s01][2] };
            sideT = hinge;
            set = true;
            if (name == side + "Arm")
              ++hinged;
          }
        }
        int sp = source(side + "HandPinky1"), si = source(side + "HandIndex1");
        int tp = target(side + "HandPinky1"), ti = target(side + "HandIndex1");
        if (!set && sp >= 0 && si >= 0 && tp >= 0 && ti >= 0)
        {
          // (a finger of a clip with no rest pose: the knuckles over the clip in its own frame)
          bool finger = name.size() > side.size() + 4 && name.compare(side.size(), 4, "Hand") == 0;
          sideS = bodySides && finger ? refSide(s) : Sub(restP(sp), restP(si));
          sideT = Sub(modelP(tp), modelP(ti));
          set = true;
        }
        if (!set)
        {
          sideS = sideT = { 0, 0, 1 };   // no knuckles in a rig: the character's front (+Z in both frames)
          if (bodySides)
            sideS = refSide(s);          // (the chest's front)
          ++byFront;
        }
      }
      V dirS = Sub(restP(sa), restP(s));
      if (bodySides && clip.RefDir.size() == (size_t)clipBones * 3)
      {
        float const* d = &clip.RefDir[(size_t)s * 3];
        if (d[0] * d[0] + d[1] * d[1] + d[2] * d[2] > 1e-12f)
          dirS = { d[0], d[1], d[2] };   // (no rest pose: to the next bone it carries)
      }
      double fs[3][3], ft[3][3];
      if (Frame(dirS, sideS, fs) && Frame(Sub(modelP(ta), modelP(t)), sideT, ft))
      {
        double c[3][3];
        for (int i = 0; i < 3; ++i)
          for (int j = 0; j < 3; ++j)
            c[i][j] = fs[i][0] * ft[j][0] + fs[i][1] * ft[j][1] + fs[i][2] * ft[j][2];   // fs ft^T
        Q cq = FromMatrix(c);
        put(fit.Align[t], Norm(Mul(Mul(Conj(restQ(s)), cq), modelQ(t))));
        turnOf[s] = cq;
        hasTurn[s] = 1;
        done = true;
        ++aligned;
      }
    }
    if (!done && bodySides && name == "Head" && sa < 0 && clip.RefAim.size() == (size_t)clipBones * 3)
    {
      // No rest pose and no end to the head: its pose against the chest's over the clip (the chest's up and across in
      // its frame) onto the game rest head's against its chest
      float const* up = &clip.RefAim[(size_t)s * 3];
      V upT = gameUp;
      double fs[3][3], ft[3][3];
      if (up[0] * up[0] + up[1] * up[1] + up[2] * up[2] > 0.25f && Frame({ up[0], up[1], up[2] }, refSide(s), fs) &&
          Frame(upT, { 1, 0, 0 }, ft))
      {
        double c[3][3];
        for (int i = 0; i < 3; ++i)
          for (int j = 0; j < 3; ++j)
            c[i][j] = fs[i][0] * ft[j][0] + fs[i][1] * ft[j][1] + fs[i][2] * ft[j][2];
        Q cq = FromMatrix(c);
        put(fit.Align[t], Norm(Mul(Mul(Conj(restQ(s)), cq), modelQ(t))));
        turnOf[s] = cq;
        hasTurn[s] = 1;
        done = true;
        ++aligned;
      }
    }
    if (!done && !FromRestOnly(name))
    {
      // No aim of its own (a hand without Middle1, a finger's tip, the head's end): the turn of the bone aiming at it
      // (the clip's bones come parents first)
      for (int q = 0; q < s && !done; ++q)
        if (hasTurn[q] && AimOf(clip.Bones[q]) == name)
        {
          put(fit.Align[t], Norm(Mul(Mul(Conj(restQ(s)), turnOf[q]), modelQ(t))));
          done = true;
          ++byParent;
        }
    }
    if (!done && planted(s))
    {
      // No rest pose: the foot (or toe) where it stands flattest taken as lying on the ground, its way ahead there onto
      // the game rest's (the ankle-to-ball line, or the ball-to-tip one, across the ground; a toe's tip the toe's)
      float const* pq = &clip.Planted[(size_t)s * 7];
      std::string from = name;
      for (const char* legSide : { "Left", "Right" })
        if (name == std::string(legSide) + "Toe_End")
          from = std::string(legSide) + "ToeBase";
      int tb = target(GameName(from)), tn = target(GameName(AimOf(from)));
      V aheadT = { 0, 0, 1 };
      if (tb >= 0 && tn >= 0)
      {
        aheadT = Sub(modelP(tn), modelP(tb));
        aheadT.y = 0.0;
        if (!(Len(aheadT) > 1e-6))
          aheadT = { 0, 0, 1 };
      }
      double fs[3][3], ft[3][3];
      if (Frame({ pq[4], pq[5], pq[6] }, { 0, 1, 0 }, fs) && Frame(aheadT, { 0, 1, 0 }, ft))
      {
        double c[3][3];
        for (int i = 0; i < 3; ++i)
          for (int j = 0; j < 3; ++j)
            c[i][j] = fs[i][0] * ft[j][0] + fs[i][1] * ft[j][1] + fs[i][2] * ft[j][2];
        put(fit.Align[t], Norm(Mul(Mul(Conj(Q{ pq[0], pq[1], pq[2], pq[3] }), FromMatrix(c)), modelQ(t))));
        done = true;
        ++byPlanted;
      }
    }
    if (!done)
    {
      // From rest to rest: its turn off the clip's rest, onto the game's rest
      put(fit.Align[t], Norm(Mul(Conj(restQ(s)), modelQ(t))));
      ++fromRest;
    }
    fit.Source[t] = (short)s;
    fit.Use[t] = UseClip;
    ++fit.Driven;
  }
  fit.TargetHips = target("Hips");
  if (fit.TargetHips < 0 || fit.Source[fit.TargetHips] != clip.Hips)
  {
    fit.Report = "its skeleton has no Hips to take the clip's";
    return false;
  }
  // A bone between two the clip turns (Neck1 between Neck and Head, a finger's 0 between the hand and its 1): the
  // game rest's local, as the turns were made on it
  int between = 0;
  for (int i = 0; i < bones; ++i)
  {
    int p = parents[i];
    if (fit.Use[i] != UseTaken || p < 0 || p >= bones || fit.Use[p] != UseClip)
      continue;
    bool drivenChild = false;
    for (int j = 0; j < bones && !drivenChild; ++j)
      drivenChild = parents[j] == i && fit.Use[j] == UseClip;
    if (drivenChild)
    {
      fit.Use[i] = UseRest;
      ++between;
    }
  }
  // The elbow and knee rolls: the game keeps them its forearm's and shin's sibling turn (Faith's ElbowRoll bit for bit
  // in 4,992 recorded frames; the rest offset kept: 0 deg on Faith, 8 deg on the others' elbows; offline 2026-11-11)
  static char const* const rolls[][2] = { { "LeftElbowRoll", "LeftForeArm" }, { "RightElbowRoll", "RightForeArm" },
    { "LeftKneeRoll", "LeftLeg" }, { "RightKneeRoll", "RightLeg" } };
  int follow = 0;
  for (auto const& pair : rolls)
  {
    int r = target(pair[0]), d = target(pair[1]);
    if (r < 0 || d < 0 || parents[r] != parents[d] || fit.Use[d] != UseClip || fit.Use[r] != UseTaken)
      continue;
    bool child = false;
    for (int j = 0; j < bones && !child; ++j)
      child = parents[j] == r;
    if (child)
      continue;
    Q ld = { fit.RestLocalQ[d][0], fit.RestLocalQ[d][1], fit.RestLocalQ[d][2], fit.RestLocalQ[d][3] };
    Q lr = { fit.RestLocalQ[r][0], fit.RestLocalQ[r][1], fit.RestLocalQ[r][2], fit.RestLocalQ[r][3] };
    put(fit.FollowTurn[r], Norm(Mul(Conj(ld), lr)));
    fit.Follows[r] = (short)d;
    fit.Use[r] = UseFollow;
    ++follow;
  }
  // Faith's twist bones: each a share of a driven bone's turn from its rest, about its own X, on its rest local (fitted
  // on her 4,992 recorded frames: the forearm rolls 0.29 / 0.64 / 0.97 of the hand's twist, rms 0.6 / 1.6 / 2.5 deg;
  // ArmRoll and ArmRoll1 -0.72 of the upper arm's Euler X, z y x, rms 1.0 - 1.4; UpLegRoll -0.8 of the thigh's, x y z,
  // rms 0.9 - 1.0; Research_Pose_Editor section 21). The others' fit badly (KrugerSec rms 2 - 9 deg): theirs stay as taken.
  int twists = 0;
  if (!strcmp(rest.Fits, "characters/skeletons/skeleton_faith"))
  {
    struct TwistRule
    {
      char const* Bone;
      char const* Driver;
      unsigned char Kind;
      float Share;
    };
    static TwistRule const rules[] = { { "ForeArmRoll", "Hand", 0, 0.295f }, { "ForeArmRoll1", "Hand", 0, 0.645f },
      { "ForeArmRoll2", "Hand", 0, 0.974f }, { "ArmRoll", "Arm", 1, -0.715f }, { "ArmRoll1", "Arm", 1, -0.715f },
      { "UpLegRoll", "UpLeg", 2, -0.795f } };
    for (const char* side : { "Left", "Right" })
      for (TwistRule const& rule : rules)
      {
        int r = target(std::string(side) + rule.Bone), d = target(std::string(side) + rule.Driver);
        if (r < 0 || d < 0 || fit.Use[d] != UseClip || fit.Use[r] != UseTaken)
          continue;
        fit.TwistOf[r] = (short)d;
        fit.TwistKind[r] = rule.Kind;
        fit.TwistShare[r] = rule.Share;
        fit.Use[r] = UseTwist;
        ++twists;
      }
  }
  fit.RestHipsY = rest.ModelT[fit.TargetHips][1];
  Q rh = modelQ(fit.TargetHips);
  put(fit.RestHipsQ, rh);
  double clipHipsY = clip.HipsRest[1];
  fit.HipsScale = clipHipsY > 0.1 ? (float)(fit.RestHipsY / clipHipsY) : 1.f;
  char text[600];
  const char* restName = strrchr(rest.Asset, '/') ? strrchr(rest.Asset, '/') + 1 : rest.Asset;
  _snprintf_s(text, sizeof(text), _TRUNCATE, "%d of its bones turn %s's (%d by their direction, the arms of %d and the legs of "
    "%d by the joints' hinges; %d by the bone before them; %d where they stand flattest; %d from rest to rest); %d between "
    "them from the rest pose, %d rolls follow, %d twist bones by rule; Hips moves x %.3f%s", fit.Driven, restName, aligned,
    hinged, kneeHinged, byParent, byPlanted, fromRest, between, follow, twists, fit.HipsScale, bodySides ?
    "; no rest pose in the clip: its side axes from its body" : "");
  fit.Report = text;
  if (byFront)
    fit.Report += "; no knuckles in one rig: the arms' roll is set by the character's front";
  if (!missing.empty())
    fit.Report += "; not in this skeleton: " + missing;
  fit.Ok = true;
  return true;
}

void pose_clips::Sample(Clip const& clip, Fit const& fit, double time, bool loop, float (*world)[4], float* hipsMove)
{
  int bones = (int)clip.Bones.size();
  double length = clip.Length();
  double t = time;
  if (!(length > 0.0))
    t = 0.0;
  else if (loop)
  {
    t = fmod(t, length);
    if (t < 0.0)
      t += length;
  }
  else
    t = t < 0.0 ? 0.0 : (t > length ? length : t);
  double f = t * clip.Fps;
  int f0 = (int)floor(f);
  if (f0 < 0)
    f0 = 0;
  if (f0 > clip.Frames - 1)
    f0 = clip.Frames - 1;
  int f1 = f0 + 1 < clip.Frames ? f0 + 1 : f0;
  double a = f - f0;
  a = a < 0.0 ? 0.0 : (a > 1.0 ? 1.0 : a);
  for (int i = 0; i < MaxBones; ++i)
  {
    int s = fit.Source[i];
    if (s < 0)
      continue;
    float const* q0 = &clip.Q[((size_t)f0 * bones + s) * 4];
    float const* q1 = &clip.Q[((size_t)f1 * bones + s) * 4];
    Q q = Slerp({ q0[0], q0[1], q0[2], q0[3] }, { q1[0], q1[1], q1[2], q1[3] }, a);
    Q w = Norm(Mul(q, { fit.Align[i][0], fit.Align[i][1], fit.Align[i][2], fit.Align[i][3] }));
    world[i][0] = (float)w.x; world[i][1] = (float)w.y; world[i][2] = (float)w.z; world[i][3] = (float)w.w;
  }
  float const* p0 = &clip.HipsP[(size_t)f0 * 3];
  float const* p1 = &clip.HipsP[(size_t)f1 * 3];
  float const* rest = clip.HipsRest;
  for (int k = 0; k < 3; ++k)
    hipsMove[k] = (float)((p0[k] + (p1[k] - p0[k]) * a - rest[k]) * fit.HipsScale);
}
