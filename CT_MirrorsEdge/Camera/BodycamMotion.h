#pragma once

// The tools' own bodycam movement (GAME > First person > Bodycam; Bodycam.cpp; the user, 2026-11-14: "lets create our
// own then with better and improved features and better bodycam movement"). Nothing here reads or writes the game:
// Bodycam.cpp hands in the game's own first-person camera and Faith's bone places each frame, and takes the bodycam's
// camera out.
//
// How it moves:
//  - A mount on her body (chest, helmet or shoulder). Its place and its facing come from her bones' world places only
//    (hips, neck, upper arms, head, its top, the eyes), so no bone's axes matter. Which way "front" is, is checked
//    against the game's own view direction over her first frames (Research_Bodycam_View_2026-11-14.md).
//  - Where it looks: your aim turned part of the way (1 - Follow) toward where the mount faces. The mount's facing is
//    smoothed (a rotation spring: Inertia, Damping), the blend is made afresh each frame: the mouse's part is
//    immediate and nothing moves on after the mouse stops; only her body's own turns sway.
//  - A strap: the camera hangs on a spring at the mount (Bounce, Damping), pushed by the mount's acceleration. It
//    doesn't trail behind while she runs steadily; it gives when she speeds up, stops, steps and lands.
//  - Her steps (a foot coming down while she moves) and landings (her hips' fall stopping) kick it; running shakes it;
//    it leans into turns; Level horizon takes its roll out.
namespace bodycam_motion
{
  enum Mount { Mount_Chest, Mount_Helmet, Mount_Shoulder, Mount_Count };
  const char* MountName(int mount);

  struct Settings
  {
    int Mount;
    float Follow;       // 0 .. 1: how much it looks where you aim (1: exactly; 0: where the mount faces)
    float Bounce;       // 0 .. 1: the strap (0 rigid .. 1 loose)
    float Inertia;      // 0 .. 1: how much the body's turns lag (0 none .. 1 a lot)
    float Damping;      // 0.2 .. 1.2: how fast bounce and sway settle (1: no overshoot)
    float Steps;        // 0 .. 2: step jolts
    float Landing;      // 0 .. 2: landing impacts
    float Shake;        // 0 .. 2: shake (more while running)
    float Lean;         // 0 .. 2: leaning into turns
    float Level;        // 0 .. 1: horizon levelling (1: never rolls)
    float Forward, Up, Side;   // m from the mount point, in the mount's own directions (Side: toward her right)
    float Fov;          // degrees, top to bottom
    float Fisheye;      // 0 .. 0.6: the lens bend (the tools' lens distortion)
    bool Overlay;       // REC, the time and the camera's number over the picture
    float Vignette;     // 0 .. 1: dark corners
  };

  // The numbers are saved in configs ([Misc] BodycamPreset): new looks are added at the end
  enum Preset { Preset_Chest, Preset_Helmet, Preset_Shoulder, Preset_Action, Preset_Custom, Preset_GoPro, Preset_Count };
  const char* PresetName(int preset);
  Settings PresetSettings(int preset);   // Preset_Custom gives Preset_Chest's
  int PresetInMenu(int index);           // the looks in the menu's order (Custom last)
  // Brings every value into its range (a config file can hold anything)
  void Clamp(Settings& s);

  // One frame of input: the game's first-person camera (its transform's rows, as the render view has them: row 2
  // points backwards) and Faith's bones' world places
  struct Input
  {
    float Aim[16];
    float Hips[3], Neck[3], LeftArm[3], RightArm[3], Spine2[3];
    float Head[3], HeadEnd[3], LeftEye[3], RightEye[3];
    float LeftFoot[3], RightFoot[3];
  };

  struct Stats
  {
    int Facing;          // the sign found for "front" (0: not decided yet)
    int FacingVotes, FacingAgree;
    unsigned Steps, Landings, Resets;
    float Speed;         // m/s, her hips across the ground
    float LastImpact;    // m/s, the last landing's fall speed
  };

  struct Vec3 { float x, y, z; };
  struct Quat { float x, y, z, w; };

  class Motion
  {
  public:
    Motion();
    void Reset();
    // The bodycam's camera transform for this frame (rows like Aim's, the place in row 3, w column 0 0 0 1). False
    // if the input isn't usable (then out is untouched). dt: seconds of game time since the last call (0: paused,
    // nothing moves).
    bool Update(Input const& in, Settings const& s, double dt, float out[16]);
    Stats const& GetStats() const { return m_Stats; }

  private:
    typedef Vec3 V3;
    typedef Quat Q;
    bool m_Started;
    // The strap: the camera's offset from its mount and its speed; the mount's (smoothed) speed
    V3 m_Offset, m_OffsetVel, m_LastTarget, m_TargetVel;
    bool m_VelKnown;
    // Her body's facing, smoothed (a rotation spring), its turn rate (in its own frame), and the last one read
    Q m_BodyQ, m_LastBody;
    V3 m_BodyW;
    // Step and landing jolts (also in the aim's frame): a critically damped dip, its peak a moment after the kick
    V3 m_Kick, m_KickVel;
    float m_FastVy;     // her hips' fall speed, smoothed only a little (a landing is felt at once)
    float m_TrendVy;    // her hips' slow up and down (stairs, slopes), taken out of her feet's speed
    Q m_LastAim;
    // Her movement
    V3 m_LastHips, m_HipsVel;
    float m_LeftFootY, m_RightFootY, m_LeftFootVel, m_RightFootVel;
    bool m_LeftDown, m_RightDown, m_Falling;
    float m_FallPeak, m_FallStart, m_FallTime, m_SinceStep, m_Burst, m_YawRate, m_Lean, m_Time;
    float m_Phase[9];
    int m_Facing, m_Votes, m_Agree;
    float m_VoteTime;
    Stats m_Stats;
  };
}
