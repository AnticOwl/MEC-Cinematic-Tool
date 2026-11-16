#pragma once

// A face that is alive between the words: the eyes, the lids, the brows, breathing and the head's never-still
// movement, worked out once from a voice's own analysis (voice_life: its phrases, syllables, stresses, its head
// curves) and from chance that is the same every time for the same sound. Merged 2026-11-13 from the checked prototype: plain
// maths, no game, no ImGui, no Windows headers; no state at playback (frames at 100 a second, At mixes two).
//
// What it adds against voice_life (every number is a named constant in VoiceAlive.cpp, with where it comes from):
//   Eyes. A gaze plan in the world: at the listener the eyes scan small (about 1.6 degrees each side of the
//     middle, 3 down: the listener's two eyes and mouth at a talking distance), fixations of about 0.55 s;
//     at the start of a turn (after 1 s of quiet) they look away 7 - 14 degrees in three of four turns, and
//     come back about 0.75 s after the speech begins (Ho, Foulsham and Kingstone 2015), at the latest 0.25 s
//     before a turn's last sound; inside a turn less often (LookAway scales all of it; measured before the
//     checker's fix 2: 27 % of the user's speaking time away; with it: 37 % at Variation 0, 22 - 56 % over
//     Variations 1 - 99 (mean 41 %), 46 % on average over the synthetic files, against 35 - 80 % for speakers in
//     the studies Ho 2015 cites); while away, small saccades. Saccades take
//     25 ms + 2.7 ms a degree (Baloh 1975's slope) with a smooth start and end. The head takes a share of a
//     look away (HeadFollow, 0.2) and arrives later; the eyes hold their target while the head moves
//     (vestibulo-ocular reflex, gain 1: Collewijn and Smeets 2000), also against voice_life's own nods and
//     turns. During a blink the eyes dip 3 degrees (Collewijn 1985: 1 - 5; the game's own: mean 3.3).
//   Lids. The upper lids follow the eyes' pitch (Becker and Fuchs 1988: equal on average), less going up.
//   Blinks (in place of voice_life's): aimed at 26 a minute while talking, 17 at rest (Bentivoglio 1997's means;
//     measured here: 23 on the user's recording, 29 over 190 synthetic files, 17 - 23 on sounds with no speech),
//     drawn to phrase ends (Nakano and Kitazawa 2010), to large gaze shifts (Evinger 1994), to strong nods;
//     each with its own timing (closing 60 - 120 ms, opening 1.6 - 2.6 times longer: the game's own 71 - 110 /
//     122 - 308 ms), one in six or seven partial (Portello 2013: 16 %), a few long at turn ends (Hoemke 2018),
//     a few doubled.
//   Brows (in place of voice_life's): its raises, each a little different in size and between the two sides,
//     with a spring's small overshoot; small flickers on more stressed syllables (Cave 1996, Swerts and
//     Krahmer 2010 via Berger and Zellers 2022); a flash on some looks back; a slow tone that never stops.
//   Frown: on thinking (a look away after a long pause, a held "uhm"), on loud low emphasis.
//   Breath: a breath in before a breath group (inhalation 0.25 - 0.7 s, longer and deeper before a longer
//     group: Whalen and Kinsella-Shaw 1997; groups of about 3.5 s: Rochet-Capellan and Fuchs 2013), the lungs
//     emptying while speaking, quiet breathing at rest (4 s cycles); Inhale: the lips part for a speech breath.
//   Head: voice_life's Nod / Turn / Tilt plus a slow pink drift that never stops (less in pauses), the share
//     of the looks away, and a small lift with each breath in.
//   Energy: louder and higher than the voice's own middle = livelier (more flickers, more drift, wider lids).
//   Mood (chosen by the user, never guessed from the sound): Warm (a smile that grows at phrase ends), Worried
//     (inner brows up), Stern (a frown's base), Neutral.
// The first and the last frame are at rest. Every frame keeps the limits below for any input (proven by the
// test program on hostile input), and nothing that is not a number comes out.
//
// What is NOT known: whether any of it looks right on these faces (nobody has seen it); whether the speaker's
// listener is where the eyes are posed (the eyes scan about the posed direction); the eyes' look-away side and
// the moods are design, not measurement.

#include "VoiceLife.h"

#include <cstddef>
#include <string>
#include <vector>

namespace voice_alive
{
  struct Frame
  {
    float GazeYaw;      // radians: both eyes in the head, > 0 towards the character's own right
    float GazePitch;    // radians: > 0 the eyes look down
    float LidFollow;    // radians: > 0 the upper lids come down (they follow the eyes' pitch, the blink's dip left out)
    float LidLift;      // radians: > 0 the upper lids open wider (energy, mood), < 0 a little narrower
    float BlinkLeft;    // 0 the eyes as posed .. 1 shut (the character's own left eye)
    float BlinkRight;
    float BrowLeft;     // 0 at rest .. 1 a clear raise (a little under 0: a touch lowered)
    float BrowRight;
    float Frown;        // 0 .. 1: the inner brows down and together
    float InnerUp;      // 0 .. 1: the inner brows up (Worried)
    float Smile;        // 0 .. 1: the mouth's corners up (Warm)
    float Breath;       // 0 breathed out .. 1 a full breath in
    float Inhale;       // 0 .. 1: the lips part for a quick breath in before speech
    float Nod;          // radians, the whole head (voice_life's own included): > 0 the chin goes down
    float Turn;         // radians: > 0 the nose goes to the character's own right
    float Tilt;         // radians: > 0 the top of the head goes to the character's own right
    float Energy;       // 0 calm .. 1 lively (0.5: the voice's own middle)
  };

  enum Mood
  {
    MoodNeutral = 0,
    MoodWarm = 1,
    MoodWorried = 2,
    MoodStern = 3
  };

  struct Settings
  {
    int Mood = MoodNeutral;
    float HeadFollow = 0.2f;   // the head's share of a look away, 0 .. 0.5
    float Vor = 1.0f;          // 1: the eyes hold their target while the voice moves the head; 0: they ride with it
    float LookAway = 1.0f;     // how often the eyes look away, 0 (never: they only scan the listener) .. 2
    // [CHECKER FIX 1] Mixed into the seed: two characters given the same sound, or sounds of the same length (two
    // silent clips, two 3.0 s lines), get faces of their own (different blinks, glances, drift). 0: the seed as
    // before (with fixes 2 and 3 in this copy the curves still differ from the researcher's, by those fixes). The
    // face layer can pass the body's own number.
    int Variation = 0;
  };

  struct Track
  {
    double FramesPerSecond = 100.0;
    std::vector<Frame> Frames;
    std::string Report;        // one "name: value" a line
  };

  // The limits every frame keeps (radians, or units for the 0 .. 1 curves)
  const float MaxGazeYaw = 0.384f;      // 22 degrees to either side
  const float MaxGazeUp = 0.244f;       // 14 degrees up
  const float MaxGazeDown = 0.314f;     // 18 degrees down
  const float MaxLidFollow = 0.297f;    // 17 degrees either way
  const float MaxLidLift = 0.0436f;     // 2.5 degrees either way
  const float MinBrow = -0.15f;
  const float MaxBrow = 1.25f;
  const float MaxNodDown = 0.1134f;     // 6.5 degrees
  const float MaxNodUp = 0.0611f;       // 3.5 degrees
  const float MaxTurn = 0.096f;         // 5.5 degrees
  const float MaxTilt = 0.0524f;        // 3 degrees
  // The largest change in a second
  const float MaxGazeSpeed = 12.2f;     // 700 degrees a second (the eyes' fastest saccades)
  const float MaxLidSpeed = 12.2f;
  const float MaxLidLiftSpeed = 0.2f;
  const float MaxBlinkSpeed = 28.0f;    // shut in 60 ms at the fastest
  const float MaxBrowSpeed = 9.0f;
  const float MaxFrownSpeed = 4.0f;
  const float MaxMoodSpeed = 3.0f;      // InnerUp, Smile
  const float MaxBreathSpeed = 7.0f;
  const float MaxInhaleSpeed = 10.0f;
  const float MaxNodSpeed = 0.70f;      // 40 degrees a second
  const float MaxTurnSpeed = 0.35f;     // 20 degrees a second
  const float MaxTiltSpeed = 0.175f;    // 10 degrees a second
  const float MaxEnergySpeed = 3.0f;    // (it only sets amounts; its fade at the ends is the fastest part)

  // For the test program: what was planned, in the order it was made
  struct Event
  {
    double Seconds = 0.0;      // where it starts
    double Until = 0.0;        // where it ends (a look away: where the eyes come back)
    std::string Kind;          // "look away", "look back", "scan", "away scan", "blink", "breath", "rest breath",
                               // "brow raise", "brow flicker", "brow flash", "frown", "smile"
    float Size = 0.0f;         // degrees (gaze), 0 .. 1 (blink, brow, frown, breath depth)
    float Yaw = 0.0f;          // degrees: the gaze's target (a look away, a scan) or the head's share
    float Pitch = 0.0f;
    std::string Why;
  };

  struct Debug
  {
    std::vector<Event> Events;
    // The curves before the guard, and how many values the guard changed (0 when the design keeps its own limits)
    int GuardChanged = 0;
    int GuardNotFinite = 0;
    std::vector<std::string> GuardNames;   // per curve: its name and how many values the guard changed
    std::vector<int> GuardChangedBy;
  };

  // From voice_life's track and its debug data (the same sound). life.Frames may be empty (no frames out).
  // False, with the reason in error: not enough memory, or a track that is not one (FramesPerSecond not 100).
  bool Make(voice_life::Track const& life, voice_life::Debug const& lifeDebug, Settings const& settings, Track& out,
    std::string& error, Debug* debug = nullptr);

  // voice_life::Analyse, then Make (the arguments as voice_life::Analyse has them)
  bool Analyse(short const* pcm, size_t samples, int rate, int channels, Settings const& settings, Track& out,
    std::string& error, Debug* debug = nullptr);

  // The curves at a time into the sound (between two frames: mixed). Before the start and after the last
  // frame: all zero, the face at rest.
  Frame At(Track const& track, double seconds);
}
