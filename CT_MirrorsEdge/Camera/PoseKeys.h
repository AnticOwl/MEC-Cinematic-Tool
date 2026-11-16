#pragma once
#include <string>
#include <vector>

// The pose editor's keyframes: a bone's turn and move at a time over a base pose, the maths that samples them,
// the playhead's clock, rebasing an edit onto another base, and the text file. Plain C++: the caller locks.
namespace pose_keys
{
  const double TimeEpsilon = 0.0005;      // s: two keys of a bone nearer than this (<=) are one key
  const double MaxTime = 3600.0;          // s: no key and no Length beyond it
  const int MaxKeysPerBone = 2000;
  const int MaxFileBones = 512;

  // To the NEXT key. Ease_In, Ease_Out, Ease_Overshoot since 2026-11-16 (the user: the movements are robotic): a file
  // with them doesn't load in an older build (its words are unknown there).
  enum Ease : int { Ease_Smooth = 0, Ease_Linear, Ease_EaseInOut, Ease_Hold, Ease_In, Ease_Out, Ease_Overshoot,
    Ease_Count };
  enum Clock : int { Clock_Game = 0, Clock_Real, Clock_Track, Clock_Clip, Clock_Count };
  // "smooth" "linear" "ease" "hold" "easein" "easeout" "overshoot" (the file's words; "smooth" if unknown)
  char const* EaseName(int ease);
  // How far along a segment (0 - 1, Overshoot past 1 and back) an ease is at u (0 - 1 of its time); Smooth: u
  // (its curve is made in Sample). OvershootPast: how far past the next key Overshoot goes (a share of the way).
  double EaseShape(int ease, double u);
  const double OvershootPast = 0.1;
  // The speed an ease starts and ends a segment at, as a factor of the segment's own (its keys' difference over its
  // time): Linear 1 and 1, Ease in and out 0 and 0, Hold 0 and 0, Ease in 0 and 2, Ease out 2 and 0, Overshoot its
  // start's and 0. A Smooth segment next to one takes that speed across their key (see Sample).
  double EaseStartSpeed(int ease);
  double EaseEndSpeed(int ease);
  char const* ClockName(int clock);       // "game" "real" "track" "clip"

  // A bone's edit at a time: the pose editor's Turn (quaternion x y z w, local = Turn x base) and Move (metres)
  struct Key
  {
    double Time = 0.0;
    float Turn[4] = { 0.f, 0.f, 0.f, 1.f };
    float Move[3] = { 0.f, 0.f, 0.f };
    int Ease = Ease_Smooth;
    bool Auto = false;      // made by the pose editor itself (a bone's first keyframe's holds around it): drawn
                            // hollow; not saved (a file's keys load as the user's own)
  };
  struct Channel
  {
    int Bone = -1;                        // the skeleton's bone index
    std::vector<Key> Keys;                // sorted by Time, no two within TimeEpsilon
  };
  struct Anim
  {
    std::vector<Channel> Channels;        // sorted by Bone, none empty
    double Length = 5.0;                  // s: the timeline's end (never under the last key; 0.1 - MaxTime)
    bool On = true;                       // the keys drive their bones
    bool Playing = false;
    bool Loop = false;
    double Time = 0.0;                    // s: the playhead
    float Speed = 1.f;                    // 0.05 - 4
    int Clock = Clock_Game;
    double Start = 0.0;                   // Clock_Track / Clock_Clip: the keys' time at the track's / clip's 0 s
    unsigned Revision = 0;                // +1 at every change of a key (see below)
  };

  Channel* Find(Anim& anim, int bone);                 // null: no keys on that bone. Valid only until the next
  Channel const* Find(Anim const& anim, int bone);     // call that changes the Anim.
  int KeyCount(Anim const& anim);                      // all channels
  double FirstKeyTime(Anim const& anim);               // 0 with no keys
  double LastKeyTime(Anim const& anim);                // 0 with no keys
  int KeyAt(Channel const& channel, double time);      // the nearest key if within TimeEpsilon (<=), or -1
  // The nearest key time of any bone more than TimeEpsilon before / after a time. False: none on that side.
  bool KeyTimeBefore(Anim const& anim, double time, double& found);
  bool KeyTimeAfter(Anim const& anim, double time, double& found);

  // Sets (adds or replaces) a bone's key at a time. ease < 0 or >= Ease_Count: a replaced key keeps its own, a new
  // one takes the ease of the key before it (Ease_Smooth if none). It copies turn and move before it changes
  // anything (they may point into anim), stores turn normalised, keeps the order, and grows Length to the key's
  // time. A replaced key keeps its own time. Returns the key's index, or -1 with nothing changed when: time not
  // finite, < 0 or > MaxTime; bone < 0; any of the 7 numbers not finite; |turn| < 1e-6; or a NEW key would pass
  // MaxKeysPerBone (a replace never does). automatic: the key's Auto (a replaced key takes it too).
  int SetKey(Anim& anim, int bone, double time, float const* turn, float const* move, int ease, bool automatic = false);
  bool SetEase(Anim& anim, int bone, int index, int ease);    // false: no such key or ease out of range
  bool RemoveKey(Anim& anim, int bone, int index);            // the channel too when it was its last
  bool RemoveChannel(Anim& anim, int bone);                   // false if it had none
  int RemoveKeysAt(Anim& anim, double time);                  // per bone the one key KeyAt gives; returns how many
  // Moves a key in time (clamped to [0, MaxTime]; not finite: -1, nothing changed). A key of the same bone already
  // there (within TimeEpsilon) is replaced by the moved one. Returns the key's new index, -1 if there's no such key.
  // Length grows if needed.
  int MoveKey(Anim& anim, int bone, int index, double time);
  // Moves every key within TimeEpsilon of `from`, on all bones, to `to` (same rules). Returns how many keys were
  // found at `from`.
  int MoveKeysAt(Anim& anim, double from, double to);
  // Clamped to [max(0.1, LastKeyTime), MaxTime]; +infinity: MaxTime; NaN: unchanged.
  void SetLength(Anim& anim, double length);

  // A bone's edit at a time (before its first key: the first key; after its last: the last; a time that isn't
  // finite: the first). False: no keys. The keys must be sorted (every function above keeps them so).
  // loopLength > 0: the keyframes loop at that length (a bone keyed at 0 and at it carries on through the wrap)
  bool Sample(Channel const& channel, double time, float* turn, float* move, double loopLength = 0.0);

  // Moves the playhead by one frame of its clock and returns the new time.
  //   realDt: seconds since the last frame; gameScale: the game's time scale (0 while frozen);
  //   trackTime: the camera track's time, < 0 when no track plays; clipTime / clipOn: the animation clip's.
  // Whether this call's clock "set the time":
  //   Clock_Game: Playing: Time += realDt x gameScale x Speed.   Clock_Real: Playing: Time += realDt x Speed.
  //   Clock_Track: Playing and trackTime >= 0: Time = Start + trackTime x Speed.
  //   Clock_Clip: Playing and clipOn: Time = Start + clipTime x Speed.
  // Then: Length <= 0 or not finite: Time = 0. Else if the clock set the time and Loop: Time > Length -> fmod(Time, Length);
  // Time < 0 -> Length + fmod(Time, Length) (Time == Length stays). Else: Time clamped to [0, Length], and on
  // Clock_Game / Clock_Real, Playing with Time >= Length -> Playing = false. Not playing: only the clamp.
  double Advance(Anim& anim, double realDt, float gameScale, double trackTime, double clipTime, bool clipOn);

  // An edit made over one base pose, carried to another: the same absolute local pose over the new base.
  //   absolute rotation = turn x baseQ, absolute place = baseT + move;
  //   new turn = absolute rotation x conj(newBaseQ) (normalised), new move = keepMove ? absolute place - newBaseT : 0.
  void Rebase(float const* baseQ, float const* baseT, float const* newBaseQ, float const* newBaseT, bool keepMove,
    float* turn, float* move);

  // The keyframes' file (text, see 1.2). Bones by name.
  struct FileBone
  {
    std::string Name;
    bool HasBase = false;                 // the bone's base local pose when saved (absent when made over a clip)
    float BaseQ[4] = { 0.f, 0.f, 0.f, 1.f }, BaseT[3] = { 0.f, 0.f, 0.f };
    bool HasStatic = false;               // an edited bone without keys: its edit
    float StaticTurn[4] = { 0.f, 0.f, 0.f, 1.f }, StaticMove[3] = { 0.f, 0.f, 0.f };
    std::vector<Key> Keys;
  };
  struct File
  {
    std::string Label, Skeleton, Clip;    // who it was made on, its SkeletonAsset, the clip's file ("" none)
    bool OverClip = false;                // made with a clip on: the edits are over the clip's moving pose
    double Length = 5.0;
    bool Loop = false;
    float Speed = 1.f;
    int Clock = Clock_Game;
    double Start = 0.0;
    std::vector<FileBone> Bones;
  };
  bool SaveFile(char const* path, File const& file, std::string& error);
  bool LoadFile(char const* path, File& file, std::string& error);
  // The same with a Windows wide path: any folder or name, as the file window gives it (2026-11-16)
  bool SaveFile(wchar_t const* path, File const& file, std::string& error);
  bool LoadFile(wchar_t const* path, File& file, std::string& error);
}
