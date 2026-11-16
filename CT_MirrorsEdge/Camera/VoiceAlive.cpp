// See VoiceAlive.h. The steps, in order:
//   1. The speech's structure from voice_life's debug data: phrases (frames), the zone of every frame
//      (speaking, a pause inside a turn, quiet: listening or at rest), the syllables.
//   2. Energy: the syllables' level and pitch against the voice's own middle, mixed over about 3 s.
//   3. The gaze plan in the world: looks away (turn starts, phrase starts, long phrases, held "uhm"s, rests),
//      the scan at the listener between them, small saccades while away; rendered with timed saccades.
//      The head's share of each look away, through a critically damped spring.
//   4. Blinks: wanted ones (gaze shifts, phrase ends, strong nods) by chance, a background rhythm between
//      them, each with its own timing, some partial, long or doubled.
//   5. Brows: voice_life's raises varied per raise and per side, flickers, flashes, a slow tone, a spring.
//      Frown, inner brows, smile.
//   6. Breathing: a breath in before each breath group, the lungs emptying while speaking, quiet breathing.
//   7. The head: voice_life's + a pink drift + the gaze's share + the breath's lift.
//   8. The eyes in the head: the world gaze minus the head (the reflex), the blink's dip; the lids follow.
//   9. Fade in and out; the guard: every value a number, in range, within its speed.
// The tags in the comments: [read: ...] a source opened for this work, [data: ...] measured in the game's own
// recordings, [from memory] only the direction is vouched for, [designed] a choice, not a measurement.
#include "VoiceAlive.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

namespace voice_alive
{
  namespace
  {
    const double kPi = 3.14159265358979323846;
    const float kDeg = 0.01745329252f;
    const int kFps = 100;

    // ---- The speech's structure
    const int kTurnPause = 100;            // [designed] 1 s of quiet before a phrase starts a turn (voice_life: 1 s
                                           // takes the head back to rest)
    const int kRestFrom = 200;             // [designed] 2 s without speech: listening or at rest

    // ---- The scan at the listener (degrees about the posed gaze, which stands for the listener's face)
    // [from memory] an adult's eyes are about 6.3 cm apart, so at a talking distance of 1 m they are 3.6 degrees
    // apart, and the mouth about 6 cm (3.4 degrees) under them
    const float kScanEyeYaw = 1.6f;
    const float kScanEyePitch = -0.4f;
    const float kScanMouthPitch = 3.0f;
    const float kScanEyeWeightTalk = 0.4f;     // [designed] each eye; the mouth the rest
    const float kScanEyeWeightListen = 0.35f;  // [designed] [from memory: a listener looks at the mouth more]
    const float kFixMedianTalk = 55.0f;        // [designed] frames; [data: the game's own people: steps of 2 deg or
                                               // more 0.27 - 2.2 a second, medians 0.12 - 1.1 s apart]
    const float kFixMedianListen = 80.0f;      // [designed]
    const float kFixSpread = 0.45f;            // [designed] lognormal spread (natural log)
    const float kFixLeast = 20.0f;
    const float kFixMost = 200.0f;

    // ---- Looks away
    const float kAvertTurn = 0.75f;            // [designed from read: Ho, Foulsham, Kingstone 2015 (PLOS ONE): the
                                               // speaker's gaze reaches the listener 0.736 - 0.780 s after the
                                               // speech begins, on average; Kendon 1967 as cited there]
    // [CHECKER FIX 2] The researcher's values gave 27 % of the user's speaking time looking away, and only 32 % at
    // LookAway 2: under the 35 - 80 % that the speakers' 20 - 65 % at the listener (Kendon 1967 as cited by Ho 2015)
    // leaves, so the speaker stared. Inside a turn more phrases start with a look away, they come back later, long
    // phrases look away sooner, more often and for longer, and LookAway also lengthens them (x 0.6 + 0.4 LookAway:
    // 1 at the default). [designed] Measured: see the checker's out\skeptic_fixed.txt.
    const float kAvertPhrase = 0.65f;          // [designed] inside a turn, a 1 s phrase; more, up to a 4 s phrase
    const float kAvertPhraseLong = 0.30f;      // (aimed at a third or more of the speaking time away: [read: Ho 2015,
                                               // citing earlier work: speakers look at listeners 20 - 65 % of the time])
    const float kAvertStern = 0.7f;            // [designed] Stern looks away less
    const float kAwayLeadLeast = 5.0f;         // [designed] the eyes go 50 - 350 ms before the first sound
    const float kAwayLeadMost = 35.0f;
    const float kBackMeanTurn = 75.0f;         // [read: Ho 2015] back 0.75 s after the speech begins (SD 0.72 -
    const float kBackSdTurn = 50.0f;           // 0.92 s there; cut to 0.5 s here [designed])
    const float kBackMeanPhrase = 110.0f;      // [designed] [CHECKER FIX 2] (was 80)
    const float kBackSdPhrase = 50.0f;         // (was 40)
    const float kBackLeast = 30.0f;
    const float kBackMost = 250.0f;
    const int kLookBackLead = 25;              // [designed from read: Ho 2015: the partner starts 0.42 - 0.43 s after
                                               // the speaker's gaze comes back] back 0.25 s before a turn's end
    const int kAwayLeast = 35;                 // [designed] a look away lasts 0.35 s at least
    const int kAwayGap = 40;                   // [designed] 0.4 s at the listener between two
    const float kAwayYawLeast = 7.0f;          // [designed inside read: Stahl 1999: the eye-only range is 35.8 deg
    const float kAwayYawMost = 14.0f;          // wide on average (SD 31.9): under it the head need not move]
    const float kSameSide = 0.7f;              // [designed] a side of its own, 7 of 10
    const float kUpShare = 0.55f;              // [designed] up to a side; level; the rest down to a side
    const float kLevelShare = 0.30f;
    const float kUpLeast = 3.0f;
    const float kUpMost = 8.0f;
    const float kLevelMost = 2.0f;
    const float kDownLeast = 4.0f;
    const float kDownMost = 8.0f;
    // [CHECKER FIX 2] long phrases: a look away 1.2 - 2.4 s in, then every 1.6 - 2.8 s, 8 times in 10, 0.7 - 1.8 s
    // long, 5 - 10 deg, never in a phrase's last 1.2 s [designed] (were 1.8 - 3.2 s, 2.5 - 4 s, 7 in 10, 0.5 - 1.4 s,
    // the last 1.5 s)
    const float kMidFirstLeast = 120.0f;
    const float kMidFirstMost = 240.0f;
    const float kMidNextLeast = 160.0f;
    const float kMidNextMost = 280.0f;
    const float kMidChance = 0.8f;
    const float kMidLeast = 70.0f;
    const float kMidMost = 180.0f;
    const float kMidYawLeast = 5.0f;
    const float kMidYawMost = 10.0f;
    const int kMidRoom = 120;
    const float kRestEveryLeast = 400.0f;      // [designed] at rest / listening: every 4 - 7 s, 45 %, 0.5 - 1.2 s,
    const float kRestEveryMost = 700.0f;       // 4 - 9 deg, clear of the next phrase by 0.6 s
    const float kRestChance = 0.45f;
    const float kRestLeast = 50.0f;
    const float kRestMost = 120.0f;
    const float kRestYawLeast = 4.0f;
    const float kRestYawMost = 9.0f;
    const int kRestClear = 60;
    const float kHeldLength = 0.25f;           // [designed] a held syllable 0.25 - 1 s long inside the speech, after
    const float kHeldLongest = 1.0f;           // 150 ms with no syllable or at a phrase's start: an "uhm"; one in 4 s
    const int kHeldQuietBefore = 15;           // at most (music under a voice makes held notes too)
    const int kHeldGap = 400;
    const float kAwayScanLeast = 35.0f;        // [designed] while away: a small saccade every 0.35 - 0.8 s
    const float kAwayScanMost = 80.0f;
    const float kAwayScanYaw = 2.5f;
    const float kAwayScanPitch = 1.5f;

    // ---- Saccades
    const float kSaccadeBase = 0.025f;         // s [from memory: the main sequence's intercept, 20 - 30 ms]
    const float kSaccadePerDegree = 0.0027f;   // s [read: Baloh, Sills, Kumley, Honrubia 1975 (Neurology): duration
                                               // grows 2.7 ms a degree on average, 25 people]

    // ---- The head's share of a look away
    const float kFollowLag = 10.0f;            // frames [from memory: the eyes start first; the head's lag not vouched]
    const float kFollowOmega = 6.0f;           // [designed] rad/s, critically damped: 90 % of the way in 0.65 s
    const float kFollowPitchShare = 0.6f;      // [designed] of HeadFollow, for the pitch
    const float kFollowMost = 3.0f;            // degrees

    // ---- Eyes and lids
    const float kBlinkEyeDown = 3.0f;          // degrees [read: Collewijn, van der Steen, Steinman 1985: the eyes go
                                               // down and towards the nose 1 - 5 deg in a blink] [data: the game's
                                               // eye bone 0 - 6 deg down in its blinks, mean 3.3]
    const float kLidFollowDown = 0.9f;         // [read: Becker and Fuchs 1988: lid and eye at the same positions on
    const float kLidFollowUp = 0.7f;           // average; upward lid saccades often smaller and slower] [data: the
                                               // game's KrugerSec left lids 1.15 - 1.30, a man's 0.51 - 0.54]
    const float kLidLiftEnergy = 1.5f;         // [designed] degrees at energy 1 (and -1.5 at 0)

    // ---- Blinks
    const float kBlinkBackTalk = 300.0f;       // frames: the background's mean while talking, set so that with the
                                               // wanted ones the whole is about [read: Bentivoglio et al. 1997: 26 a
                                               // minute in conversation, 17 at rest, 150 people]
    const float kBlinkBackRest = 360.0f;
    const float kBlinkWorried = 0.85f;         // [designed] Worried blinks more
    const float kBlinkFirstLeast = 40.0f;      // [designed] the first background blink 0.4 - 2.4 s in
    const float kBlinkFirstMost = 240.0f;
    const int kBlinkGapLeast = 45;             // [designed] 0.45 s between two (a double aside)
    const int kBlinkShift = 15;                // [designed] a background blink moves up to 150 ms to a dip of loudness
    const float kBlinkGazeFrom = 8.0f;         // degrees [designed from read: Evinger et al. 1994: lid-closing
    const float kBlinkGazeLeast = 0.15f;       // activity with 97 % of gaze shifts over 33 deg]
    const float kBlinkGazePerDegree = 0.025f;
    const float kBlinkGazeMost = 0.6f;
    const float kBlinkPhraseEnd = 0.45f;       // [designed from read: Nakano and Kitazawa 2010: speakers' blinks at
    const float kBlinkEndLeast = 5.0f;         // the ends and pauses of speech]
    const float kBlinkEndMost = 25.0f;
    const int kBlinkEndPause = 30;
    const float kBlinkNod = 0.5f;              // [designed] a strong nod (voice_life: 2 deg)
    const float kBlinkNodSize = 2.0f;
    const float kBlinkNodLead = 18.0f;         // the nod's start (voice_life's kNodDown)
    const float kBlinkClose = 8.0f;            // frames [data: the game's own closing 71 - 110 ms, median 73]
    const float kBlinkCloseSpread = 0.15f;
    const float kBlinkCloseLeast = 6.0f;
    const float kBlinkCloseMost = 12.0f;
    const float kBlinkHoldMost = 4.0f;         // [data: shut 0 - 35 ms]
    const float kBlinkOpenLeast = 1.6f;        // x closing [data: opening 122 - 308 ms, median 149]
    const float kBlinkOpenMost = 2.6f;
    const float kBlinkOpenFrom = 12.0f;
    const float kBlinkOpenTo = 32.0f;
    const float kPartial = 0.15f;              // [read: Portello, Rosenfield, Chu 2013: incomplete blinks 16.1 % on
    const float kPartialLeast = 0.55f;         // average (21 people, 15 min at a computer)] size [designed]
    const float kPartialMost = 0.85f;
    const float kLongAtTurnEnd = 0.25f;        // [designed from read: Hoemke, Holler, Levinson 2018: long blinks
    const float kLongHoldLeast = 15.0f;        // (607 ms against 208) are read as a signal]
    const float kLongHoldMost = 35.0f;
    const float kDouble = 0.10f;               // [designed] (voice_life: one in eight, 420 ms)
    const float kDoubleLeast = 30.0f;
    const float kDoubleMost = 45.0f;
    const float kBlinkSides = 0.03f;           // [designed] the two lids up to 3 % apart

    // ---- Brows
    const float kBrowSizeSpread = 0.15f;       // [designed] each raise its own size
    const float kBrowSizeLeast = 0.75f;
    const float kBrowSizeMost = 1.3f;
    const float kBrowSideBias = 0.03f;         // [designed; direction from read: Sackeim, Gur, Saucy 1978: emotions
    const float kBrowSideBiasSpread = 0.03f;   // look more intense on the face's left half]
    const float kBrowSideEvent = 0.08f;        // [designed] the weaker side 0 - 16 % lower
    const float kBrowSideMost = 0.16f;
    const float kBrowKnee = 0.15f;             // [designed] the range's ends are eased into over this much
    const float kFlickerFrom = 0.35f;          // brow score [designed; read: Cave et al. 1996: 71 % of rapid brow
    const float kFlickerTo = 0.8f;             // movements with an F0 rise; Swerts and Krahmer 2010 via Berger and
    const float kFlickerStress = 0.3f;         // Zellers 2022: 70.1 % of strong accents with a brow movement]
    const float kFlickerLevel = -10.0f;        // dB
    const float kFlickerChance = 0.55f;
    const float kFlickerLeast = 0.15f;
    const float kFlickerMost = 0.35f;
    const float kFlickerLead = 12.0f;          // [read via Berger and Zellers 2022: Flecha-Garcia 2010: a raise starts
    const float kFlickerRise = 12.0f;          // about 60 ms before the accented syllable] (the syllable's centre is
    const float kFlickerHoldLeast = 5.0f;      // later) shape [designed]
    const float kFlickerHoldMost = 12.0f;
    const float kFlickerFall = 30.0f;
    const int kFlickerGap = 80;
    const float kFlashChance = 0.25f;          // [designed; from memory: a brow flash when looking back at someone]
    const float kFlashLeast = 0.25f;
    const float kFlashMost = 0.40f;
    const float kBrowTone = 0.05f;             // [designed] the slow tone's spread
    const float kBrowToneEnergy = 0.12f;       // [designed]
    const float kBrowZeta = 0.65f;             // [designed] a spring: about 7 % overshoot
    const float kBrowHz = 3.0f;

    // ---- Frown and the moods
    const float kThinkChance = 0.4f;           // [designed; from memory: the brows draw together with thinking]
    const float kFrownLeast = 0.2f;
    const float kFrownMost = 0.45f;
    const float kHeldFrownLeast = 0.25f;
    const float kHeldFrownMost = 0.4f;
    const float kEmphLevel = 1.2f;             // [designed] a loud, low syllable
    const float kEmphPitch = -0.3f;
    const float kEmphStress = 1.0f;
    const float kEmphChance = 0.5f;
    const float kEmphLeast = 0.15f;
    const float kEmphMost = 0.3f;
    const int kEmphGap = 300;
    const float kFrownZeta = 0.8f;
    const float kFrownHz = 2.0f;
    const float kMoodKnee = 0.05f;
    const float kSternFrown = 0.22f;           // [designed] the moods
    const float kSternLid = -1.0f;             // degrees
    const float kSternFix = 1.3f;
    const float kWarmSmile = 0.2f;
    const float kWarmSmileEnergy = 0.15f;
    const float kWarmSmileEnd = 0.2f;
    const float kWarmLid = -0.4f;
    const float kWorriedInner = 0.25f;
    const float kWorriedInnerStress = 0.25f;
    const float kWorriedLid = -0.4f;

    // ---- Breathing
    const int kBreathPauseLeast = 25;          // [designed] a pause of 0.25 s can hold a breath in
    const int kBreathAlways = 50;              // [designed] a pause from 0.5 s always does
    const int kBreathAfter = 250;              // [designed] 2.5 s after the last one, any pause does (groups of
    const int kBreathLongGroup = 250;          // about [read: Rochet-Capellan and Fuchs 2013: 3.5 s on average,
                                               // 26 speakers of German])
    const int kBreathGroupMost = 800;
    const float kInLeast = 0.25f;              // s [designed from read: Whalen and Kinsella-Shaw 1997: the breath in
    const float kInPerSecond = 0.05f;          // is longer before a longer utterance] [from memory: 0.3 - 0.8 s]
    const float kInMost = 0.7f;
    const int kInEndBefore = 4;                // [designed] the breath ends 40 ms before the first sound
    const int kInRoomLeast = 15;
    const float kDepthLeast = 0.4f;            // [designed; read: Winkworth et al. 1995: the lung volume a talk starts
    const float kDepthPerSecond = 0.1f;        // at varies]
    const float kDepthPerInSecond = 4.0f;      // [designed] keeps the breath in under 6.3 a second
    const float kSpeechEmpty = 0.1f;           // [designed]
    const float kRestEmpty = 0.15f;
    const float kRestTau = 0.6f;               // s
    const float kRestStart = 80.0f;            // [designed] quiet breathing 0.8 s after the speech
    const float kRestPeriod = 400.0f;          // frames [from memory: 12 - 20 breaths a minute at rest]
    const float kRestPeriodSpread = 0.12f;
    const float kRestInShare = 0.4f;
    const float kRestDepth = 0.3f;
    const float kRestClearOfSpeech = 30.0f;
    const float kBreathHeadLift = 0.3f;        // [designed, no source] degrees, up, a full breath in against a mid one
    const float kBreathMid = 0.35f;
    const float kInhaleRise = 0.35f;           // [designed] of the breath's time
    const float kInhaleFall = 0.3f;
    const float kInhaleSpeedShare = 0.9f;      // [designed] of the speed limit at the steepest

    // ---- The head's drift
    const float kDriftLowest = 0.15f;          // Hz; four octaves, amplitude as 1 / sqrt(frequency) (pink)
    const float kDriftNod = 0.5f;              // [designed] degrees of spread while talking; [read: Hadar et al. 1984:
    const float kDriftTurn = 0.7f;             // the head moves almost all the time in speech] [data: the game's
    const float kDriftTilt = 0.45f;            // calm idle man: Head spread 0.7 / 3.6 / 1.0 deg]
    const float kHeadKnee = 0.0175f;           // [designed] 1 degree: the head's range is eased into
    const float kDriftQuiet = 0.45f;           // [designed] the share left in pauses
    const int kDriftGate = 50;
    const float kValueNoiseSpread = 0.497f;    // [derived] the spread of smoothstep value noise on -1 .. 1 knots

    // ---- Energy
    const float kEnergyLevel = 0.06f;          // [designed] per dB over the voice's own middle [from memory: livelier
    const float kEnergyPitch = 0.07f;          // speech is louder and higher] per semitone
    const float kEnergyReach = 150.0f;         // frames: syllables within 1.5 s count, less with distance
    const float kEnergyPull = 0.3f;            // towards 0.5 where there are few syllables
    const int kEnergySmooth = 40;

    // ---- The ends
    const int kFadeIn = 30;
    const int kFadeOut = 40;

    // What each draw is made for
    enum What
    {
      WhatSide = 1, WhatAvert, WhatAvertMid, WhatAvertRest, WhatHeld, WhatScan, WhatAwayScan, WhatBlinkWant,
      WhatBlinkGap, WhatBlinkShape, WhatBlinkDouble, WhatBrow, WhatFlicker, WhatFlash, WhatFrown, WhatEmph,
      WhatBreath, WhatRestBreath, WhatDrift, WhatBrowSide
    };

    struct Random
    {
      unsigned long long State;

      explicit Random(unsigned long long seed) : State(seed)
      {
      }

      double Next()
      {
        State += 0x9E3779B97F4A7C15ull;
        unsigned long long z = State;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        z ^= z >> 31;
        return static_cast<double>(z >> 11) * (1.0 / 9007199254740992.0);
      }

      float Uniform(float a, float b)
      {
        return a + (b - a) * static_cast<float>(Next());
      }

      float Gauss()
      {
        double u = Next();
        double v = Next();
        if (u < 1e-12)
          u = 1e-12;
        return static_cast<float>(std::sqrt(-2.0 * std::log(u)) * std::cos(2.0 * kPi * v));
      }
    };

    void Mix(unsigned long long& hash, long long value)
    {
      unsigned long long v = static_cast<unsigned long long>(value);
      for (int i = 0; i < 8; i++)
      {
        hash ^= (v >> (i * 8)) & 0xFF;
        hash *= 0x100000001B3ull;
      }
    }

    // The same numbers for the same sound: the seed is the sound's length in tenths of a second, each draw is
    // made from what it is for and where it is (as voice_life does)
    struct Chance
    {
      unsigned long long Seed = 0;

      Random At(long long what, long long where, long long shape = 0) const
      {
        unsigned long long s = Seed;
        Mix(s, what);
        Mix(s, where);
        Mix(s, shape);
        return Random(s);
      }
    };

    bool Finite(float v)
    {
      return std::isfinite(v);
    }

    bool Finite(double v)
    {
      return std::isfinite(v);
    }

    float Clamp(float v, float lo, float hi)
    {
      if (!(v >= lo))
        return lo;
      return v > hi ? hi : v;
    }

    float Smooth01(float u)
    {
      u = Clamp(u, 0.0f, 1.0f);
      return u * u * (3.0f - 2.0f * u);
    }

    float MinJerk(float u)
    {
      u = Clamp(u, 0.0f, 1.0f);
      return u * u * u * (10.0f + u * (-15.0f + 6.0f * u));
    }

    float RaisedCos(float u)
    {
      u = Clamp(u, 0.0f, 1.0f);
      return 0.5f - 0.5f * static_cast<float>(std::cos(kPi * u));
    }

    // Within lo + knee .. hi - knee the value itself; beyond, eased towards lo / hi (never past them; the
    // change is never steeper than the value's own, so no speed limit is broken by it)
    float SoftRange(float x, float lo, float hi, float knee)
    {
      float top = hi - knee, bottom = lo + knee;
      if (x > top)
        return top + knee * static_cast<float>(std::tanh((x - top) / knee));
      if (x < bottom)
        return bottom + knee * static_cast<float>(std::tanh((x - bottom) / knee));
      return x;
    }

    // A blink's opening: quick at first, a slow tail, no jump in speed where it starts
    float OpenShape(float u)
    {
      u = Clamp(u, 0.0f, 1.0f);
      float w = 1.0f - static_cast<float>(std::pow(1.0f - u, 1.6f));
      return 1.0f - Smooth01(w);
    }

    // A bump laid into a curve: up over rise frames, held, down over fall
    void Bump(std::vector<float>& c, double start, double rise, double hold, double fall, float size, bool add)
    {
      if (c.empty() || !Finite(start) || !Finite(size) || size == 0.0f)
        return;
      rise = std::max(rise, 1.0);
      fall = std::max(fall, 1.0);
      hold = std::max(hold, 0.0);
      long long n = static_cast<long long>(c.size());
      long long a = static_cast<long long>(std::floor(start));
      long long b = static_cast<long long>(std::ceil(start + rise + hold + fall));
      for (long long k = std::max(a, 0ll); k <= std::min(b, n - 1); ++k)
      {
        double t = static_cast<double>(k) - start;
        float v;
        if (t <= 0.0)
          v = 0.0f;
        else if (t < rise)
          v = RaisedCos(static_cast<float>(t / rise));
        else if (t < rise + hold)
          v = 1.0f;
        else if (t < rise + hold + fall)
          v = 1.0f - RaisedCos(static_cast<float>((t - rise - hold) / fall));
        else
          v = 0.0f;
        v *= size;
        size_t i = static_cast<size_t>(k);
        if (add)
          c[i] += v;
        else
          c[i] = std::max(c[i], v);
      }
    }

    // A spring (second order) that follows a curve; with lead, the curve is taken that much earlier, so the
    // spring's own lag (2 zeta / omega for slow moves) is made up and only its overshoot and settling stay
    void Spring(std::vector<float>& c, float zeta, float hz, bool lead)
    {
      if (c.size() < 2)
        return;
      float omega = static_cast<float>(2.0 * kPi) * hz;
      int ahead = lead ? static_cast<int>(std::lround(2.0f * zeta / omega * kFps)) : 0;
      std::vector<float> in(c);
      float x = in[0], v = 0.0f;
      const int steps = 4;
      float h = 1.0f / (kFps * steps);
      for (size_t k = 0; k < c.size(); ++k)
      {
        size_t from = std::min(c.size() - 1, k + static_cast<size_t>(ahead));
        float u = in[from];
        for (int s = 0; s < steps; ++s)
        {
          v += h * (omega * omega * (u - x) - 2.0f * zeta * omega * v);
          x += h * v;
        }
        c[k] = x;
      }
    }

    // Pink drift of spread 1: four octaves of smooth value noise, knots on a fixed grid of times
    void Drift(Chance const& chance, long long what, size_t n, std::vector<float>& out)
    {
      out.assign(n, 0.0f);
      double sum = 0.0;
      for (int j = 0; j < 4; ++j)
      {
        double f = kDriftLowest * std::pow(2.0, j);
        double amp = 1.0 / std::sqrt(f / kDriftLowest);
        sum += amp * amp;
        double spacing = kFps / f;
        size_t knots = static_cast<size_t>(std::ceil(static_cast<double>(n) / spacing)) + 2;
        std::vector<float> value(knots);
        for (size_t m = 0; m < knots; ++m)
          value[m] = chance.At(WhatDrift, what * 8 + j, static_cast<long long>(m)).Uniform(-1.0f, 1.0f);
        for (size_t k = 0; k < n; ++k)
        {
          double x = static_cast<double>(k) / spacing;
          size_t m = static_cast<size_t>(x);
          float u = static_cast<float>(x - static_cast<double>(m));
          if (m + 1 >= knots)
            m = knots - 2;
          out[k] += static_cast<float>(amp) * (value[m] + (value[m + 1] - value[m]) * Smooth01(u));
        }
      }
      float norm = static_cast<float>(kValueNoiseSpread * std::sqrt(sum));
      for (float& v : out)
        v /= norm;
    }

    // The column voice_life's debug has by this name, if it has one frame a frame
    std::vector<float> const* Column(voice_life::Debug const& d, char const* name, size_t n)
    {
      for (size_t i = 0; i < d.Names.size() && i < d.Columns.size(); ++i)
        if (d.Names[i] == name && d.Columns[i].size() == n)
          return &d.Columns[i];
      return nullptr;
    }

    struct Span
    {
      int From;   // the first frame of speech
      int To;     // one past the last
    };

    struct Away
    {
      int From, To;
      float Yaw, Pitch;     // degrees
      bool Thinking;
      int Kind;             // 0 a turn's start, 1 a phrase's start, 2 inside a long phrase, 3 at rest, 4 an "uhm"
    };

    struct Fix
    {
      int Frame;
      float Yaw, Pitch;     // degrees in the world, about the posed gaze
      int Kind;             // 0 scan, 1 look away, 2 away scan, 3 look back
    };

    struct BlinkPlan
    {
      int Start;
      float Close, Hold, Open, Size, Left, Right;
      bool Long, Partial;
      std::string Why;
    };

    struct Inhalation
    {
      int From, To;
      float Depth;
      bool Speech;
      int Speaking;         // the speaking frames this breath serves
    };

    char const* AwayName(int kind)
    {
      switch (kind)
      {
      case 0: return "a turn's start";
      case 1: return "a phrase's start";
      case 2: return "inside a long phrase";
      case 3: return "at rest";
      default: return "an uhm (a held syllable)";
      }
    }

    void Line(std::string& text, char const* name, double value, char const* format = "%.3f")
    {
      char number[64];
      std::snprintf(number, sizeof(number), format, value);
      text += name;
      text += ": ";
      text += number;
      text += "\n";
    }

    // The guard: a number, in range, within its speed; the first and last frame at rest when they were
    void Guard(std::vector<float>& c, float lo, float hi, float speed, int& changed, int& notFinite)
    {
      size_t n = c.size();
      if (n == 0)
        return;
      std::vector<float> before(c);
      for (float& v : c)
      {
        if (!Finite(v))
        {
          v = 0.0f;
          ++notFinite;
        }
        v = Clamp(v, lo, hi);
      }
      float step = speed / kFps;
      bool lastAtRest = before[n - 1] == 0.0f;
      for (size_t k = 1; k < n; ++k)
        c[k] = Clamp(c[k], c[k - 1] - step, c[k - 1] + step);
      if (lastAtRest)
        c[n - 1] = 0.0f;
      for (size_t k = n - 1; k-- > 0;)
        c[k] = Clamp(c[k], c[k + 1] - step, c[k + 1] + step);
      for (size_t k = 0; k < n; ++k)
        if (c[k] != before[k])
          ++changed;
    }
  }

  bool Make(voice_life::Track const& life, voice_life::Debug const& lifeDebug, Settings const& settingsIn, Track& out,
    std::string& error, Debug* debug)
  {
    out = Track();
    if (debug)
      *debug = Debug();
    error.clear();
    if (!(life.FramesPerSecond == 100.0))
    {
      error = "the voice's track is not at 100 frames a second";
      return false;
    }
    try
    {
      Settings settings = settingsIn;
      if (settings.Mood < MoodNeutral || settings.Mood > MoodStern)
        settings.Mood = MoodNeutral;
      settings.HeadFollow = Clamp(Finite(settings.HeadFollow) ? settings.HeadFollow : 0.2f, 0.0f, 0.5f);
      settings.Vor = Clamp(Finite(settings.Vor) ? settings.Vor : 1.0f, 0.0f, 1.0f);
      settings.LookAway = Clamp(Finite(settings.LookAway) ? settings.LookAway : 1.0f, 0.0f, 2.0f);

      size_t n = life.Frames.size();
      out.FramesPerSecond = 100.0;
      out.Frames.assign(n, Frame());
      if (n == 0)
      {
        out.Report = "frames: 0\n";
        return true;
      }
      int count = n > 0x7FFFFFF0u ? 0x7FFFFFF0 : static_cast<int>(n);
      std::vector<Event> events;
      auto add = [&](double seconds, double until, char const* kind, float size, float yaw, float pitch, std::string why)
      {
        if (!debug)
          return;
        Event e;
        e.Seconds = seconds;
        e.Until = until;
        e.Kind = kind;
        e.Size = size;
        e.Yaw = yaw;
        e.Pitch = pitch;
        e.Why = why;
        events.push_back(e);
      };

      Chance chance;
      chance.Seed = 0xCBF29CE484222325ull;
      Mix(chance.Seed, static_cast<long long>(n / 10));
      Mix(chance.Seed, 0x5EE1);   // not voice_life's numbers
      if (settings.Variation != 0)
        Mix(chance.Seed, 0x7A71000000ll ^ static_cast<long long>(settings.Variation));   // [CHECKER FIX 1]

      // ---- 1. The structure
      std::vector<Span> phrases;
      size_t pc = std::min(lifeDebug.PhraseFrom.size(), lifeDebug.PhraseTo.size());
      for (size_t i = 0; i < pc; ++i)
      {
        double a = lifeDebug.PhraseFrom[i] * kFps, b = lifeDebug.PhraseTo[i] * kFps;
        if (!Finite(a) || !Finite(b))
          continue;
        a = std::max(0.0, std::min(a, static_cast<double>(count)));
        b = std::max(0.0, std::min(b, static_cast<double>(count)));
        Span s{ static_cast<int>(std::lround(a)), static_cast<int>(std::lround(b)) };
        if (s.To - s.From >= 5)
          phrases.push_back(s);
      }
      std::sort(phrases.begin(), phrases.end(), [](Span const& x, Span const& y) { return x.From < y.From; });
      {
        std::vector<Span> merged;
        for (Span const& s : phrases)
          if (!merged.empty() && s.From <= merged.back().To)
            merged.back().To = std::max(merged.back().To, s.To);
          else
            merged.push_back(s);
        phrases.swap(merged);
      }
      int pn = static_cast<int>(phrases.size());
      // zone: 2 speaking, 1 a pause inside a turn, 0 quiet (listening, at rest)
      std::vector<unsigned char> zone(n, 0);
      for (int i = 0; i < pn; ++i)
      {
        for (int k = phrases[i].From; k < phrases[i].To; ++k)
          zone[k] = 2;
        if (i + 1 < pn && phrases[i + 1].From - phrases[i].To < kTurnPause)
          for (int k = phrases[i].To; k < phrases[i + 1].From; ++k)
            zone[k] = 1;
      }
      auto gapBefore = [&](int i) { return i == 0 ? 1000000 : phrases[i].From - phrases[i - 1].To; };
      auto gapAfter = [&](int i) { return i + 1 >= pn ? 1000000 : phrases[i + 1].From - phrases[i].To; };
      struct Syl
      {
        int Centre, From, To;
        float Level, Pitch, Length, LevelScore, PitchScore, Stress, BrowScore, Speech;
      };
      std::vector<Syl> syls;
      for (voice_life::Syllable const& s : lifeDebug.Syllables)
      {
        double c = s.Seconds * kFps;
        if (!Finite(c) || c < 0.0 || c >= count)
          continue;
        Syl y;
        y.Centre = std::min(count - 1, static_cast<int>(std::lround(c)));
        y.From = Finite(s.From) ? static_cast<int>(std::lround(std::max(0.0, std::min(s.From * kFps, c)))) : y.Centre;
        y.To = Finite(s.To) ? static_cast<int>(std::lround(std::max(c, std::min(s.To * kFps, count - 1.0)))) : y.Centre;
        y.Level = Finite(s.Level) ? s.Level : -100.0f;
        y.Pitch = Finite(s.Pitch) && s.Pitch > 0.0f ? s.Pitch : 0.0f;
        y.Length = Finite(s.Length) ? s.Length : 0.0f;
        y.LevelScore = Finite(s.LevelScore) ? s.LevelScore : 0.0f;
        y.PitchScore = Finite(s.PitchScore) ? s.PitchScore : 0.0f;
        y.Stress = Finite(s.Stress) ? s.Stress : 0.0f;
        y.BrowScore = Finite(s.BrowScore) ? s.BrowScore : 0.0f;
        y.Speech = Finite(s.Speech) ? s.Speech : 0.0f;
        syls.push_back(y);
      }
      std::sort(syls.begin(), syls.end(), [](Syl const& x, Syl const& y) { return x.Centre < y.Centre; });
      std::vector<float> const* loudness = Column(lifeDebug, "loudness", n);

      // voice_life's own curves, made numbers within its limits
      std::vector<float> lifeNod(n), lifeTurn(n), lifeTilt(n), lifeBrow(n);
      for (size_t k = 0; k < n; ++k)
      {
        voice_life::Frame const& f = life.Frames[k];
        lifeNod[k] = Finite(f.Nod) ? Clamp(f.Nod, -voice_life::MaxNodUp, voice_life::MaxNodDown) : 0.0f;
        lifeTurn[k] = Finite(f.Turn) ? Clamp(f.Turn, -voice_life::MaxTurn, voice_life::MaxTurn) : 0.0f;
        lifeTilt[k] = Finite(f.Tilt) ? Clamp(f.Tilt, -voice_life::MaxTilt, voice_life::MaxTilt) : 0.0f;
        lifeBrow[k] = Finite(f.Brow) ? Clamp(f.Brow, 0.0f, 1.0f) : 0.0f;
      }

      // ---- 2. Energy
      std::vector<float> energy(n, 0.5f);
      {
        std::vector<float> levels, pitches;
        for (Syl const& s : syls)
          if (s.Speech >= 0.5f && s.Level > -60.0f)
          {
            levels.push_back(s.Level);
            if (s.Pitch > 0.0f)
              pitches.push_back(s.Pitch);
          }
        auto median = [](std::vector<float> v)
        {
          if (v.empty())
            return 0.0f;
          std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
          return v[v.size() / 2];
        };
        float midLevel = median(levels), midPitch = median(pitches);
        std::vector<float> value;
        std::vector<int> at;
        for (Syl const& s : syls)
          if (s.Speech >= 0.5f && s.Level > -60.0f)
          {
            float e = 0.5f + kEnergyLevel * (s.Level - midLevel);
            if (s.Pitch > 0.0f && midPitch > 0.0f)
              e += kEnergyPitch * 12.0f * static_cast<float>(std::log2(s.Pitch / midPitch));
            value.push_back(Clamp(e, 0.0f, 1.0f));
            at.push_back(s.Centre);
          }
        size_t first = 0;
        std::vector<float> raw(n, 0.5f);
        for (size_t k = 0; k < n; ++k)
        {
          while (first < at.size() && at[first] < static_cast<int>(k) - kEnergyReach)
            ++first;
          double sum = kEnergyPull * 0.5, weight = kEnergyPull;
          for (size_t j = first; j < at.size() && at[j] <= static_cast<int>(k) + kEnergyReach; ++j)
          {
            double w = 1.0 - std::fabs(static_cast<double>(at[j]) - static_cast<double>(k)) / kEnergyReach;
            if (w > 0.0)
            {
              sum += w * value[j];
              weight += w;
            }
          }
          raw[k] = static_cast<float>(sum / weight);
        }
        // a moving middle over 0.8 s
        double run = 0.0;
        for (size_t k = 0; k < n + kEnergySmooth; ++k)
        {
          if (k < n)
            run += raw[k];
          if (k >= static_cast<size_t>(2 * kEnergySmooth + 1))
            run -= raw[k - 2 * kEnergySmooth - 1];
          if (k >= static_cast<size_t>(kEnergySmooth))
          {
            size_t c = k - kEnergySmooth;
            size_t lo = c >= static_cast<size_t>(kEnergySmooth) ? c - kEnergySmooth : 0;
            size_t hi = std::min(n - 1, c + kEnergySmooth);
            energy[c] = Clamp(static_cast<float>(run / static_cast<double>(hi - lo + 1)), 0.0f, 1.0f);
          }
        }
      }

      // ---- 3. The gaze plan
      bool stern = settings.Mood == MoodStern, worried = settings.Mood == MoodWorried;
      float side = chance.At(WhatSide, 0).Next() < 0.5 ? 1.0f : -1.0f;
      auto direction = [&](Random& r, float yawLeast, float yawMost, bool thinking, int kind, Away& a)
      {
        float s = r.Next() < kSameSide ? side : -side;
        a.Yaw = s * r.Uniform(yawLeast, yawMost);
        double c = r.Next();
        float upShare = thinking ? 1.0f : kUpShare;
        float levelShare = kLevelShare;
        if (worried || kind == 3)
          upShare *= 0.6f;   // [designed] more level or down at rest, and Worried
        if (c < upShare)
          a.Pitch = -r.Uniform(kUpLeast, kUpMost);
        else if (c < upShare + levelShare)
          a.Pitch = r.Uniform(-kLevelMost, kLevelMost);
        else
          a.Pitch = r.Uniform(kDownLeast, kDownMost);
      };
      std::vector<Away> aways;
      for (int i = 0; i < pn; ++i)
      {
        int a = phrases[i].From, b = phrases[i].To;
        bool turn = gapBefore(i) >= kTurnPause;
        bool turnEnd = gapAfter(i) >= kTurnPause;
        float len = static_cast<float>(b - a);
        float p = turn ? kAvertTurn : kAvertPhrase + kAvertPhraseLong * Clamp((len - 100.0f) / 300.0f, 0.0f, 1.0f);
        p *= settings.LookAway;
        if (stern)
          p *= kAvertStern;
        Random r = chance.At(WhatAvert, a / 50);
        if (r.Next() < p)
        {
          Away w;
          w.Kind = turn ? 0 : 1;
          w.Thinking = turn && gapBefore(i) >= kTurnPause && i > 0 && r.Next() < kThinkChance;
          float lead = r.Uniform(kAwayLeadLeast, kAwayLeadMost);
          w.From = std::max(0, a - static_cast<int>(lead));
          float back = turn ? kBackMeanTurn + kBackSdTurn * r.Gauss() : kBackMeanPhrase + kBackSdPhrase * r.Gauss();
          back = Clamp(back * (0.6f + 0.4f * settings.LookAway), kBackLeast, kBackMost);   // [CHECKER FIX 2]
          w.To = a + static_cast<int>(back);
          if (turnEnd)
            w.To = std::min(w.To, b - kLookBackLead);
          else
            w.To = std::min(w.To, b + 10);
          w.To = std::max(w.To, w.From + kAwayLeast);
          direction(r, kAwayYawLeast, kAwayYawMost, w.Thinking, w.Kind, w);
          aways.push_back(w);
        }
        // a long phrase: more, later in it
        if (b - a > kMidRoom + static_cast<int>(kMidFirstLeast))
        {
          Random m = chance.At(WhatAvertMid, a / 50);
          float t = static_cast<float>(a) + m.Uniform(kMidFirstLeast, kMidFirstMost);
          int guard = 0;
          while (t < static_cast<float>(b - kMidRoom) && guard++ < 1000)
          {
            Random q = chance.At(WhatAvertMid, static_cast<long long>(t) / 50, 1);
            if (q.Next() < kMidChance * settings.LookAway * (stern ? kAvertStern : 1.0f))
            {
              Away w;
              w.Kind = 2;
              w.Thinking = false;
              w.From = static_cast<int>(t);
              w.To = w.From + static_cast<int>(q.Uniform(kMidLeast, kMidMost) * (0.6f + 0.4f * settings.LookAway));   // [CHECKER FIX 2]
              direction(q, kMidYawLeast, kMidYawMost, false, 2, w);
              // [CHECKER FIX 2] back at the listener before a turn's last sound, as the phrase-start ones are
              w.To = std::min(w.To, turnEnd ? b - kLookBackLead : b + 10);
              if (w.To - w.From >= kAwayLeast)
                aways.push_back(w);
            }
            t += q.Uniform(kMidNextLeast, kMidNextMost);
          }
        }
      }
      // held syllables inside the speech ("uhm"): a thinking look away (up to a side) with a frown
      int lastHeld = -100000;
      for (size_t si = 0; si < syls.size(); ++si)
      {
        Syl const& s = syls[si];
        if (!(s.Speech < 0.5f && s.Length >= kHeldLength && s.Length <= kHeldLongest && zone[s.Centre] >= 1 &&
          s.Centre - lastHeld >= kHeldGap))
          continue;
        bool phraseStart = false;
        for (int i = 0; i < pn && !phraseStart; ++i)
          phraseStart = s.From <= phrases[i].From + 5 && s.To >= phrases[i].From;
        bool quietBefore = si == 0 || syls[si - 1].To <= s.From - kHeldQuietBefore;
        if ((!phraseStart && !quietBefore) || chance.At(WhatHeld, s.Centre / 50, 1).Next() >= settings.LookAway)
          continue;
        lastHeld = s.Centre;
        {
          Random r = chance.At(WhatHeld, s.Centre / 50);
          Away w;
          w.Kind = 4;
          w.Thinking = true;
          w.From = std::max(0, s.From - 5);
          w.To = std::min(count - 1, s.To + 20);
          if (w.To - w.From < kAwayLeast)
            w.To = w.From + kAwayLeast;
          direction(r, kAwayYawLeast, kAwayYawMost, true, 4, w);
          aways.push_back(w);
        }
      }
      // quiet stretches (listening, at rest, or a sound with no speech): now and then a short look away
      {
        int k = 0;
        while (k < count)
        {
          if (zone[k] != 0)
          {
            ++k;
            continue;
          }
          int e = k;
          while (e < count && zone[e] == 0)
            ++e;
          if (e - k >= kRestFrom || pn == 0)
          {
            int limit = e < count ? e - kRestClear - 40 : count;
            Random r0 = chance.At(WhatAvertRest, k / 50);
            float t = static_cast<float>(k) + r0.Uniform(kRestEveryLeast * 0.5f, kRestEveryMost * 0.5f);
            int guard = 0;
            while (t < static_cast<float>(limit) && guard++ < 100000)
            {
              Random r = chance.At(WhatAvertRest, static_cast<long long>(t) / 50, 1);
              if (r.Next() < kRestChance * settings.LookAway * (stern ? kAvertStern : 1.0f))
              {
                Away w;
                w.Kind = 3;
                w.Thinking = false;
                w.From = static_cast<int>(t);
                w.To = std::min(w.From + static_cast<int>(r.Uniform(kRestLeast, kRestMost)), std::max(w.From + kAwayLeast, limit));
                direction(r, kRestYawLeast, kRestYawMost, false, 3, w);
                aways.push_back(w);
              }
              t += r.Uniform(kRestEveryLeast, kRestEveryMost);
            }
          }
          k = e;
        }
      }
      std::sort(aways.begin(), aways.end(), [](Away const& x, Away const& y) { return x.From < y.From; });
      {
        // no two at once: a later one that starts too soon after the one before is dropped (a held "uhm" inside a
        // look away makes that one a thinking one)
        std::vector<Away> kept;
        for (Away const& w : aways)
        {
          if (w.From >= count - 10)
            continue;
          if (!kept.empty() && w.From < kept.back().To + kAwayGap)
          {
            if (w.Kind == 4 && w.From < kept.back().To)
              kept.back().Thinking = true;
            continue;
          }
          Away c = w;
          c.To = std::min(c.To, count - 1);
          if (c.To - c.From >= kAwayLeast / 2)
            kept.push_back(c);
        }
        aways.swap(kept);
      }

      // The fixations: the scan at the listener between the looks away, small saccades while away
      std::vector<Fix> fixes;
      {
        float points[3][2] = { { -kScanEyeYaw, kScanEyePitch }, { kScanEyeYaw, kScanEyePitch }, { 0.0f, kScanMouthPitch } };
        int at = -1;
        int t = 0;
        size_t ai = 0;
        bool back = false;
        int guard = 0;
        while (t < count && guard++ < 10000000)
        {
          int until = ai < aways.size() ? aways[ai].From : count;
          while (t < until)
          {
            Random r = chance.At(WhatScan, t);
            bool talking = zone[t] >= 1;
            float eyeWeight = talking ? kScanEyeWeightTalk : kScanEyeWeightListen;
            float w[3] = { eyeWeight, eyeWeight, 1.0f - 2.0f * eyeWeight };
            if (back)
              w[2] = 0.0f;   // back from a look away: to the listener's eyes
            if (at >= 0)
              w[at] = 0.0f;
            float sum = w[0] + w[1] + w[2];
            float c = static_cast<float>(r.Next()) * sum;
            int q = c < w[0] ? 0 : (c < w[0] + w[1] ? 1 : 2);
            fixes.push_back(Fix{ t, points[q][0], points[q][1], back ? 3 : 0 });
            back = false;
            at = q;
            float median = talking ? kFixMedianTalk : kFixMedianListen;
            if (stern)
              median *= kSternFix;
            median /= 0.85f + 0.3f * energy[t];
            float d = Clamp(median * static_cast<float>(std::exp(kFixSpread * r.Gauss())), kFixLeast, kFixMost);
            t += std::max(1, static_cast<int>(d));
          }
          if (ai >= aways.size())
            break;
          Away const& w = aways[ai++];
          t = std::max(t, w.From);
          fixes.push_back(Fix{ w.From, w.Yaw, w.Pitch, 1 });
          Random r = chance.At(WhatAwayScan, w.From / 50);
          float u = static_cast<float>(w.From) + r.Uniform(kAwayScanLeast, kAwayScanMost);
          while (u < static_cast<float>(w.To - 15))
          {
            fixes.push_back(Fix{ static_cast<int>(u), w.Yaw + r.Uniform(-kAwayScanYaw, kAwayScanYaw),
              w.Pitch + r.Uniform(-kAwayScanPitch, kAwayScanPitch), 2 });
            u += r.Uniform(kAwayScanLeast, kAwayScanMost);
          }
          // back where the look away was planned to end (after its last small saccade)
          t = std::max(w.To, fixes.back().Frame + 1);
          at = -1;
          back = true;
        }
      }
      std::vector<float> worldYaw(n, 0.0f), worldPitch(n, 0.0f);
      std::vector<float> sacStart;   // the frames where gaze shifts of kBlinkGazeFrom or more start, and their sizes
      std::vector<float> sacSize;
      {
        float y = 0.0f, p = 0.0f;
        for (size_t i = 0; i < fixes.size(); ++i)
        {
          int from = std::max(0, std::min(fixes[i].Frame, count - 1));
          int to = i + 1 < fixes.size() ? std::max(from, std::min(fixes[i + 1].Frame, count)) : count;
          float y0 = y, p0 = p;
          float dy = fixes[i].Yaw - y0, dp = fixes[i].Pitch - p0;
          float size = static_cast<float>(std::sqrt(dy * dy + dp * dp));
          float frames = (kSaccadeBase + kSaccadePerDegree * size) * kFps;
          for (int k = from; k < to; ++k)
          {
            float u = static_cast<float>(k - from + 1) / frames;
            float s = MinJerk(u);
            worldYaw[k] = y0 + dy * s;
            worldPitch[k] = p0 + dp * s;
          }
          if (to > from)
          {
            // where the eyes are at the end of this fixation (a fixation cut short ends inside its saccade)
            float u = static_cast<float>(to - from) / frames;
            float s = MinJerk(u);
            y = y0 + dy * s;
            p = p0 + dp * s;
          }
          if (size >= kBlinkGazeFrom)
          {
            sacStart.push_back(static_cast<float>(from));
            sacSize.push_back(size);
          }
          static char const* const kinds[4] = { "scan", "look away", "away scan", "look back" };
          if (fixes[i].Kind != 1)
            add(from / 100.0, to / 100.0, kinds[fixes[i].Kind], size, fixes[i].Yaw, fixes[i].Pitch,
              fixes[i].Kind == 3 ? "back to the listener" : "");
        }
        for (Away const& w : aways)
        {
          char why[96];
          std::snprintf(why, sizeof(why), "%s%s", AwayName(w.Kind), w.Thinking ? ", thinking" : "");
          add(w.From / 100.0, w.To / 100.0, "look away", static_cast<float>(std::sqrt(w.Yaw * w.Yaw + w.Pitch * w.Pitch)),
            w.Yaw, w.Pitch, why);
        }
      }
      // The head's share of each look away, through a critically damped spring, starting after the eyes
      std::vector<float> followYaw(n, 0.0f), followPitch(n, 0.0f);
      if (settings.HeadFollow > 0.0f)
      {
        std::vector<float> ty(n, 0.0f), tp(n, 0.0f);
        for (Away const& w : aways)
        {
          int a = std::min(count, w.From + static_cast<int>(kFollowLag));
          int b = std::min(count, w.To + static_cast<int>(kFollowLag));
          float yy = Clamp(settings.HeadFollow * w.Yaw, -kFollowMost, kFollowMost);
          float pp = Clamp(settings.HeadFollow * kFollowPitchShare * w.Pitch, -kFollowMost, kFollowMost);
          for (int k = a; k < b; ++k)
          {
            ty[k] = yy;
            tp[k] = pp;
          }
        }
        float x = 0.0f, v = 0.0f, x2 = 0.0f, v2 = 0.0f;
        const int steps = 4;
        float h = 1.0f / (kFps * steps);
        for (size_t k = 0; k < n; ++k)
        {
          for (int s = 0; s < steps; ++s)
          {
            v += h * (kFollowOmega * kFollowOmega * (ty[k] - x) - 2.0f * kFollowOmega * v);
            x += h * v;
            v2 += h * (kFollowOmega * kFollowOmega * (tp[k] - x2) - 2.0f * kFollowOmega * v2);
            x2 += h * v2;
          }
          followYaw[k] = x;
          followPitch[k] = x2;
        }
      }

      // ---- 4. Blinks
      std::vector<BlinkPlan> blinks;
      {
        struct Want
        {
          int Frame;
          int Kind;       // 0 a gaze shift, 1 a phrase's end, 2 a strong nod
          bool LongOk;
          std::string Why;
        };
        std::vector<Want> wants;
        for (size_t i = 0; i < sacStart.size(); ++i)
        {
          float p = Clamp(kBlinkGazeLeast + (sacSize[i] - kBlinkGazeFrom) * kBlinkGazePerDegree, 0.0f, kBlinkGazeMost);
          int f = std::max(0, static_cast<int>(sacStart[i]) - 1);
          if (chance.At(WhatBlinkWant, f / 50, 0).Next() < p)
          {
            char why[64];
            std::snprintf(why, sizeof(why), "a gaze shift of %.0f deg", sacSize[i]);
            wants.push_back(Want{ f, 0, false, why });
          }
        }
        for (int i = 0; i < pn; ++i)
          if (gapAfter(i) >= kBlinkEndPause)
          {
            Random r = chance.At(WhatBlinkWant, phrases[i].To / 50, 1);
            if (r.Next() < kBlinkPhraseEnd)
            {
              int f = phrases[i].To + static_cast<int>(r.Uniform(kBlinkEndLeast, kBlinkEndMost));
              if (f < count)
                wants.push_back(Want{ f, 1, gapAfter(i) >= kTurnPause, gapAfter(i) >= kTurnPause ? "a turn's end" : "a phrase's end" });
            }
          }
        for (voice_life::Event const& e : lifeDebug.Events)
          if (e.Kind == "nod" && Finite(e.Size) && e.Size >= kBlinkNodSize && Finite(e.Seconds))
          {
            double f = e.Seconds * kFps - kBlinkNodLead;
            if (f < 0.0 || f >= count)
              continue;
            int fi = static_cast<int>(f);
            if (chance.At(WhatBlinkWant, fi / 50, 2).Next() < kBlinkNod)
              wants.push_back(Want{ fi, 2, false, "a strong nod" });
          }
        std::sort(wants.begin(), wants.end(), [](Want const& x, Want const& y) { return x.Frame < y.Frame; });
        // The background rhythm between them: gamma-like gaps (three exponentials), its clock set again by
        // every blink
        auto gap = [&](int last)
        {
          Random r = chance.At(WhatBlinkGap, last / 50);
          bool talking = last >= 0 && last < count && zone[last] >= 1;
          float mean = talking ? kBlinkBackTalk : kBlinkBackRest;
          if (worried)
            mean *= kBlinkWorried;
          double s = 0.0;
          for (int j = 0; j < 3; ++j)
            s -= std::log(std::max(1e-9, r.Next()));
          return Clamp(static_cast<float>(s * mean / 3.0), static_cast<float>(kBlinkGapLeast), 3.0f * mean);
        };
        auto plan = [&](int start, int kind, bool longOk, std::string const& why)
        {
          Random r = chance.At(WhatBlinkShape, start / 50, kind);
          BlinkPlan b;
          b.Start = start;
          b.Close = Clamp(kBlinkClose * static_cast<float>(std::exp(kBlinkCloseSpread * r.Gauss())), kBlinkCloseLeast, kBlinkCloseMost);
          b.Hold = r.Uniform(0.0f, kBlinkHoldMost);
          b.Open = Clamp(b.Close * r.Uniform(kBlinkOpenLeast, kBlinkOpenMost), kBlinkOpenFrom, kBlinkOpenTo);
          b.Partial = r.Next() < kPartial;
          b.Size = b.Partial ? r.Uniform(kPartialLeast, kPartialMost) : 1.0f;
          b.Long = !b.Partial && longOk && r.Next() < kLongAtTurnEnd;
          if (b.Long)
            b.Hold = r.Uniform(kLongHoldLeast, kLongHoldMost);
          float lr = r.Uniform(-kBlinkSides, kBlinkSides);
          b.Left = 1.0f - std::max(0.0f, lr);
          b.Right = 1.0f - std::max(0.0f, -lr);
          b.Why = why;
          return b;
        };
        int last = -1;
        size_t wi = 0;
        // [CHECKER FIX 3] A wanted blink soon after the last one is kept only by chance, from none at kBlinkGapLeast to
        // all at kBlinkRefractory: without it, sounds dense in phrase ends and glances blinked 54 a minute or more
        // (one file in ten of the speech files, against Bentivoglio 1997's 26 in conversation) [designed]
        const int kBlinkRefractory = 150;
        auto keepWant = [&](Want const& w)
        {
          if (last < 0)
            return true;
          int since = w.Frame - last;
          if (since < kBlinkGapLeast)
            return false;
          if (since >= kBlinkRefractory)
            return true;
          float share = static_cast<float>(since - kBlinkGapLeast) / static_cast<float>(kBlinkRefractory - kBlinkGapLeast);
          return chance.At(WhatBlinkWant, w.Frame / 50, 7).Next() < share;
        };
        float firstDue = chance.At(WhatBlinkGap, -1).Uniform(kBlinkFirstLeast, kBlinkFirstMost);
        float nextGap = 0.0f;
        int guard = 0;
        while (guard++ < 10000000)
        {
          double due = last < 0 ? firstDue : last + nextGap;
          if (wi < wants.size() && wants[wi].Frame <= due)
          {
            Want const& w = wants[wi++];
            if (keepWant(w))   // [CHECKER FIX 3]
            {
              blinks.push_back(plan(w.Frame, w.Kind, w.LongOk, w.Why));
              last = w.Frame;
              nextGap = gap(last);
            }
            continue;
          }
          if (due >= count - 10)
          {
            if (wi < wants.size())
            {
              // only the wanted ones are left
              Want const& w = wants[wi++];
              if (keepWant(w))   // [CHECKER FIX 3]
              {
                blinks.push_back(plan(w.Frame, w.Kind, w.LongOk, w.Why));
                last = w.Frame;
                nextGap = gap(last);
              }
              continue;
            }
            break;
          }
          int at = static_cast<int>(due);
          if (loudness)
          {
            int best = at;
            for (int k = std::max(0, at - kBlinkShift); k <= std::min(count - 1, at + kBlinkShift); ++k)
              if (Finite((*loudness)[k]) && (!Finite((*loudness)[best]) || (*loudness)[k] < (*loudness)[best]))
                best = k;
            at = best;
          }
          if (last >= 0 && at - last < kBlinkGapLeast)
            at = last + kBlinkGapLeast;
          if (at >= count - 5)
            break;
          blinks.push_back(plan(at, 3, false, zone[std::min(at, count - 1)] >= 1 ? "rhythm (talking)" : "rhythm (quiet)"));
          last = at;
          nextGap = gap(last);
        }
        // doubles
        std::vector<BlinkPlan> withDoubles;
        for (size_t i = 0; i < blinks.size(); ++i)
        {
          withDoubles.push_back(blinks[i]);
          BlinkPlan const& b = blinks[i];
          if (b.Partial || b.Long)
            continue;
          Random r = chance.At(WhatBlinkDouble, b.Start / 50);
          if (r.Next() >= kDouble)
            continue;
          int second = b.Start + static_cast<int>(r.Uniform(kDoubleLeast, kDoubleMost));
          bool clear = second < count - 30 && (i + 1 >= blinks.size() || blinks[i + 1].Start >= second + kBlinkGapLeast);
          for (int k = b.Start; clear && k < std::min(count, second + 20); ++k)
            if (k > 0 && zone[k] == 2 && zone[k - 1] != 2)
              clear = false;   // a phrase starts
          if (clear)
          {
            BlinkPlan d = plan(second, 4, false, "the second of two");
            d.Partial = false;
            d.Long = false;
            d.Size = 1.0f;
            withDoubles.push_back(d);
          }
        }
        blinks.swap(withDoubles);
      }
      std::vector<float> blinkL(n, 0.0f), blinkR(n, 0.0f);
      for (BlinkPlan const& b : blinks)
      {
        float total = b.Close + b.Hold + b.Open;
        for (int k = std::max(0, b.Start); k < std::min(count, b.Start + static_cast<int>(std::ceil(total)) + 1); ++k)
        {
          float t = static_cast<float>(k - b.Start);
          float v;
          if (t < b.Close)
            v = RaisedCos(t / b.Close);
          else if (t < b.Close + b.Hold)
            v = 1.0f;
          else if (t < total)
            v = OpenShape((t - b.Close - b.Hold) / b.Open);
          else
            v = 0.0f;
          v *= b.Size;
          blinkL[k] = std::max(blinkL[k], v * b.Left);
          blinkR[k] = std::max(blinkR[k], v * b.Right);
        }
        char why[160];
        std::snprintf(why, sizeof(why), "%s; closing %.0f ms, shut %.0f ms, opening %.0f ms%s%s", b.Why.c_str(),
          b.Close * 10.0f, b.Hold * 10.0f, b.Open * 10.0f, b.Partial ? ", partial" : "", b.Long ? ", long" : "");
        add(b.Start / 100.0, (b.Start + total) / 100.0, "blink", b.Size, 0.0f, 0.0f, why);
      }

      // ---- 5. Brows, frown, the moods
      std::vector<float> browL(n, 0.0f), browR(n, 0.0f);
      {
        struct Raise
        {
          int Centre;
          float Scale, Side;
        };
        std::vector<Raise> raises;
        float bias = kBrowSideBias + chance.At(WhatBrowSide, 0).Uniform(-kBrowSideBiasSpread, kBrowSideBiasSpread);
        for (voice_life::Event const& e : lifeDebug.Events)
          if (e.Kind == "brow" && Finite(e.Seconds) && e.Seconds >= 0.0 && e.Seconds * kFps < count)
          {
            int c = std::min(count - 1, static_cast<int>(std::lround(e.Seconds * kFps)));
            Random r = chance.At(WhatBrow, c / 50);
            Raise q;
            q.Centre = c;
            q.Scale = Clamp(static_cast<float>(std::exp(kBrowSizeSpread * r.Gauss())), kBrowSizeLeast, kBrowSizeMost);
            q.Side = Clamp(bias + r.Uniform(-kBrowSideEvent, kBrowSideEvent), -kBrowSideMost, kBrowSideMost);
            raises.push_back(q);
            add(c / 100.0, c / 100.0, "brow raise", Finite(e.Size) ? e.Size * q.Scale : 0.0f, q.Side, 0.0f,
              "voice_life's raise; size x, left share +");
          }
        std::sort(raises.begin(), raises.end(), [](Raise const& x, Raise const& y) { return x.Centre < y.Centre; });
        size_t j = 0;
        for (size_t k = 0; k < n; ++k)
        {
          float l = lifeBrow[k], r = lifeBrow[k];
          if (!raises.empty())
          {
            while (j + 1 < raises.size() && std::abs(raises[j + 1].Centre - static_cast<int>(k)) <= std::abs(raises[j].Centre - static_cast<int>(k)))
              ++j;
            // the stronger side as the raise's own size, the other a little less
            l *= raises[j].Scale * (1.0f - std::max(0.0f, -raises[j].Side));
            r *= raises[j].Scale * (1.0f - std::max(0.0f, raises[j].Side));
          }
          browL[k] = l;
          browR[k] = r;
        }
        // flickers on more stressed syllables, clear of the raises and of each other
        int lastFlicker = -100000;
        for (Syl const& s : syls)
        {
          if (s.Speech < 0.5f || s.BrowScore < kFlickerFrom || s.BrowScore >= kFlickerTo || s.Stress < kFlickerStress ||
            s.Level < kFlickerLevel)
            continue;
          bool clear = s.Centre - lastFlicker >= kFlickerGap;
          for (Raise const& q : raises)
            if (std::abs(q.Centre - s.Centre) < kFlickerGap)
              clear = false;
          if (!clear)
            continue;
          Random r = chance.At(WhatFlicker, s.Centre / 50);
          float e = energy[s.Centre];
          if (r.Next() >= kFlickerChance * (0.7f + 0.6f * e))
            continue;
          float h = r.Uniform(kFlickerLeast, kFlickerMost) * (0.8f + 0.4f * e);
          float sd = Clamp(bias + r.Uniform(-kBrowSideEvent, kBrowSideEvent), -kBrowSideMost, kBrowSideMost);
          float hold = r.Uniform(kFlickerHoldLeast, kFlickerHoldMost);
          Bump(browL, s.Centre - kFlickerLead, kFlickerRise, hold, kFlickerFall, h * (1.0f - std::max(0.0f, -sd)), true);
          Bump(browR, s.Centre - kFlickerLead, kFlickerRise, hold, kFlickerFall, h * (1.0f - std::max(0.0f, sd)), true);
          lastFlicker = s.Centre;
          add((s.Centre - kFlickerLead) / 100.0, (s.Centre - kFlickerLead + kFlickerRise + hold + kFlickerFall) / 100.0,
            "brow flicker", h, sd, 0.0f, "a stressed syllable");
        }
        // a flash with some looks back at a turn's end
        for (Away const& w : aways)
        {
          bool turnEnd = false;
          for (int i = 0; i < pn; ++i)
            if (gapAfter(i) >= kTurnPause && w.To >= phrases[i].From && w.To <= phrases[i].To)
              turnEnd = true;
          if (!turnEnd)
            continue;
          Random r = chance.At(WhatFlash, w.To / 50);
          if (r.Next() >= kFlashChance)
            continue;
          float h = r.Uniform(kFlashLeast, kFlashMost);
          Bump(browL, w.To, 10.0, 10.0, 35.0, h * (1.0f - std::max(0.0f, -bias)), true);
          Bump(browR, w.To, 10.0, 10.0, 35.0, h * (1.0f - std::max(0.0f, bias)), true);
          add(w.To / 100.0, (w.To + 55) / 100.0, "brow flash", h, 0.0f, 0.0f, "looking back near a turn's end");
        }
        // the slow tone: never quite still, a little higher when lively
        std::vector<float> tone;
        Drift(chance, 4, n, tone);
        for (size_t k = 0; k < n; ++k)
        {
          float g = zone[k] >= 1 ? 1.0f : 0.5f;
          float t = kBrowTone * tone[k] * g + kBrowToneEnergy * (energy[k] - 0.5f) * g;
          browL[k] += t;
          browR[k] += t;
        }
        Spring(browL, kBrowZeta, kBrowHz, true);
        Spring(browR, kBrowZeta, kBrowHz, true);
        // the ends of the range eased into (a raise and a flicker on top of each other, the spring's overshoot)
        for (size_t k = 0; k < n; ++k)
        {
          browL[k] = SoftRange(browL[k], MinBrow, MaxBrow, kBrowKnee);
          browR[k] = SoftRange(browR[k], MinBrow, MaxBrow, kBrowKnee);
        }
      }
      std::vector<float> frown(n, 0.0f), inner(n, 0.0f), smile(n, 0.0f);
      {
        for (Away const& w : aways)
          if (w.Thinking)
          {
            Random r = chance.At(WhatFrown, w.From / 50);
            float h = w.Kind == 4 ? r.Uniform(kHeldFrownLeast, kHeldFrownMost) : r.Uniform(kFrownLeast, kFrownMost);
            Bump(frown, w.From, 25.0, std::max(0, w.To - w.From - 25), 40.0, h, false);
            add(w.From / 100.0, (w.To + 40) / 100.0, "frown", h, 0.0f, 0.0f, w.Kind == 4 ? "an uhm" : "thinking, looking away");
          }
        int lastEmph = -100000;
        for (Syl const& s : syls)
          if (s.Speech >= 0.5f && s.LevelScore >= kEmphLevel && s.PitchScore <= kEmphPitch && s.Stress >= kEmphStress &&
            s.Centre - lastEmph >= kEmphGap)
          {
            Random r = chance.At(WhatEmph, s.Centre / 50);
            if (r.Next() >= kEmphChance)
              continue;
            float h = r.Uniform(kEmphLeast, kEmphMost);
            Bump(frown, s.Centre - 15, 15.0, 10.0, 35.0, h, false);
            lastEmph = s.Centre;
            add((s.Centre - 15) / 100.0, (s.Centre + 45) / 100.0, "frown", h, 0.0f, 0.0f, "a loud, low stress");
          }
        if (stern)
          for (size_t k = 0; k < n; ++k)
            frown[k] = std::max(frown[k], kSternFrown);
        Spring(frown, kFrownZeta, kFrownHz, true);
        for (float& v : frown)
          v = std::max(0.0f, SoftRange(v, -1.0f, 1.0f, kMoodKnee));   // the spring's small dip under 0 after a frown: 0
        if (worried)
        {
          for (size_t k = 0; k < n; ++k)
            inner[k] = kWorriedInner;
          for (Syl const& s : syls)
            if (s.Speech >= 0.5f && s.Stress >= 1.0f)
              Bump(inner, s.Centre - 15, 15.0, 10.0, 40.0, kWorriedInnerStress, true);
          Spring(inner, kFrownZeta, kFrownHz, true);
          for (float& v : inner)
            v = std::max(0.0f, SoftRange(v, -1.0f, 1.0f, kMoodKnee));
        }
        if (settings.Mood == MoodWarm)
        {
          for (size_t k = 0; k < n; ++k)
            smile[k] = kWarmSmile + kWarmSmileEnergy * energy[k];
          for (int i = 0; i < pn; ++i)
          {
            Bump(smile, phrases[i].To - 30, 30.0, 40.0, 80.0, kWarmSmileEnd, true);
            add((phrases[i].To - 30) / 100.0, (phrases[i].To + 120) / 100.0, "smile", kWarmSmileEnd, 0.0f, 0.0f, "a phrase's end");
          }
          Spring(smile, kFrownZeta, kFrownHz, true);
          for (float& v : smile)
            v = std::max(0.0f, SoftRange(v, -1.0f, 1.0f, kMoodKnee));
        }
      }

      // ---- 6. Breathing
      std::vector<float> breath(n, 0.0f), inhale(n, 0.0f);
      {
        std::vector<Inhalation> ins;
        int lastEnd = -1000000;
        for (int i = 0; i < pn; ++i)
        {
          int a = phrases[i].From;
          int pause = i == 0 ? a : a - phrases[i - 1].To;
          if (pause < kBreathPauseLeast && i > 0)
            continue;
          int j = i;
          while (j + 1 < pn && phrases[j + 1].From - phrases[j].To < kBreathAlways && phrases[j + 1].From - a < kBreathGroupMost)
            ++j;
          float group = static_cast<float>(phrases[j].To - a) / kFps;
          bool need = pause >= kBreathAlways || a - lastEnd >= kBreathAfter || group * kFps >= kBreathLongGroup || ins.empty();
          if (!need)
            continue;
          float ti = Clamp(kInLeast + kInPerSecond * group, kInLeast, kInMost) * kFps;
          float room = static_cast<float>(i == 0 ? a - 2 : pause - 6);
          ti = std::min(ti, room);
          if (ti < kInRoomLeast)
            continue;
          Inhalation in;
          in.To = a - kInEndBefore;
          in.From = in.To - static_cast<int>(ti);
          if (in.From < 0)
            continue;
          in.Depth = Clamp(kDepthLeast + kDepthPerSecond * group, kDepthLeast, 1.0f);
          in.Depth = std::min(in.Depth, kDepthPerInSecond * ti / kFps);
          in.Speech = true;
          in.Speaking = 0;
          ins.push_back(in);
          lastEnd = in.To;
        }
        // quiet breathing in the long quiet stretches (and through a sound with no speech)
        {
          int k = 0;
          while (k < count)
          {
            if (zone[k] != 0)
            {
              ++k;
              continue;
            }
            int e = k;
            while (e < count && zone[e] == 0)
              ++e;
            if (e - k >= kRestFrom || pn == 0)
            {
              int stop = e;
              for (Inhalation const& in : ins)
                if (in.From >= k && in.From < stop + 1)
                  stop = std::min(stop, in.From - static_cast<int>(kRestClearOfSpeech));
              float t = static_cast<float>(k) + (k == 0 && pn == 0 ? 0.0f : kRestStart);
              int guard = 0;
              while (guard++ < 1000000)
              {
                Random r = chance.At(WhatRestBreath, static_cast<long long>(t) / 50);
                float period = kRestPeriod * static_cast<float>(std::exp(kRestPeriodSpread * r.Gauss()));
                float tin = period * kRestInShare;
                if (t + tin >= static_cast<float>(stop))
                  break;
                Inhalation in;
                in.From = static_cast<int>(t);
                in.To = static_cast<int>(t + tin);
                in.Depth = kRestDepth * r.Uniform(0.85f, 1.15f);
                in.Speech = false;
                in.Speaking = 0;
                if (in.To > in.From)
                  ins.push_back(in);
                t += period;
              }
            }
            k = e;
          }
        }
        std::sort(ins.begin(), ins.end(), [](Inhalation const& x, Inhalation const& y) { return x.From < y.From; });
        // each speech breath serves the speaking frames up to the next speech breath
        for (size_t i = 0; i < ins.size(); ++i)
          if (ins[i].Speech)
          {
            int end = count;
            for (size_t j = i + 1; j < ins.size(); ++j)
              if (ins[j].Speech)
              {
                end = ins[j].From;
                break;
              }
            int s = 0;
            for (int k = ins[i].To; k < end; ++k)
              s += zone[k] == 2 ? 1 : 0;
            ins[i].Speaking = s;
          }
        float v = kBreathMid;
        // speech that starts with the sound (no room for a breath before it) spends the breath it starts with
        int speakingFirst = 0;
        {
          int end = count;
          for (Inhalation const& in : ins)
            if (in.Speech)
            {
              end = in.From;
              break;
            }
          for (int k = 0; k < end; ++k)
            speakingFirst += zone[k] == 2 ? 1 : 0;
        }
        float rate = (v - kSpeechEmpty) / static_cast<float>(std::max(50, speakingFirst));
        size_t ii = 0;
        float v0 = v, vt = v;
        int active = -1;
        float restDecay = 1.0f - static_cast<float>(std::exp(-1.0 / (kRestTau * kFps)));
        for (int k = 0; k < count; ++k)
        {
          while (ii < ins.size() && ins[ii].To <= k && active != static_cast<int>(ii))
            ++ii;   // (none skipped in a well-formed plan; overlap guard)
          if (active < 0 && ii < ins.size() && ins[ii].From <= k)
          {
            active = static_cast<int>(ii);
            v0 = v;
            vt = ins[ii].Speech ? std::max(v0, ins[ii].Depth) : std::min(v0 + ins[ii].Depth, 0.6f);
          }
          if (active >= 0)
          {
            Inhalation const& in = ins[static_cast<size_t>(active)];
            float u = static_cast<float>(k - in.From + 1) / static_cast<float>(std::max(1, in.To - in.From));
            v = v0 + (vt - v0) * RaisedCos(u);
            if (in.Speech)
            {
              // the lips part over the first share of the breath and close over the last; a short breath parts
              // them less (smoothstep's steepest is 1.5 / its time: kept under the speed limit)
              float span = static_cast<float>(std::max(1, in.To - in.From));
              float t = static_cast<float>(k - in.From);
              float rise = kInhaleRise * span, fall = kInhaleFall * span;
              float most = kInhaleSpeedShare * MaxInhaleSpeed / kFps * std::min(rise, fall) / 1.5f;
              float shape = std::min(Smooth01(t / rise), Smooth01((span - t) / fall));
              inhale[k] = shape * std::min(0.5f + 0.5f * in.Depth, most);
            }
            if (k + 1 >= in.To)
            {
              rate = in.Speech ? (vt - kSpeechEmpty) / static_cast<float>(std::max(50, in.Speaking)) : 0.0f;
              active = -1;
              ++ii;
            }
          }
          else if (zone[k] == 2)
            v = std::max(kSpeechEmpty, v - rate);
          else if (zone[k] == 0)
            v += (kRestEmpty - v) * restDecay;
          breath[k] = Clamp(v, 0.0f, 1.0f);
        }
        for (Inhalation const& in : ins)
          add(in.From / 100.0, in.To / 100.0, in.Speech ? "breath" : "rest breath", in.Depth, 0.0f, 0.0f,
            in.Speech ? "before a breath group" : "quiet breathing");
      }

      // ---- 7. The head
      std::vector<float> nod(n), turn(n), tilt(n);
      {
        std::vector<float> dn, dt, dl;
        Drift(chance, 1, n, dn);
        Drift(chance, 2, n, dt);
        Drift(chance, 3, n, dl);
        // the drift's amount: full while talking, less in quiet, eased over 0.5 s
        std::vector<float> gate(n);
        for (size_t k = 0; k < n; ++k)
          gate[k] = zone[k] >= 1 ? 1.0f : kDriftQuiet;
        std::vector<float> eased(n);
        double run = 0.0;
        for (size_t k = 0; k < n + kDriftGate; ++k)
        {
          if (k < n)
            run += gate[k];
          if (k >= static_cast<size_t>(2 * kDriftGate + 1))
            run -= gate[k - 2 * kDriftGate - 1];
          if (k >= static_cast<size_t>(kDriftGate))
          {
            size_t c = k - kDriftGate;
            size_t lo = c >= static_cast<size_t>(kDriftGate) ? c - kDriftGate : 0;
            size_t hi = std::min(n - 1, c + kDriftGate);
            eased[c] = static_cast<float>(run / static_cast<double>(hi - lo + 1));
          }
        }
        for (size_t k = 0; k < n; ++k)
        {
          float g = eased[k] * (0.8f + 0.4f * energy[k]);
          float lift = -kBreathHeadLift * (breath[k] - kBreathMid);
          // the sum eased into the head's range (voice_life's largest pose + the drift + a look away's share can
          // pass it)
          nod[k] = SoftRange(lifeNod[k] + kDeg * (kDriftNod * g * dn[k] + followPitch[k] + lift), -MaxNodUp, MaxNodDown, kHeadKnee);
          turn[k] = SoftRange(lifeTurn[k] + kDeg * (kDriftTurn * g * dt[k] + followYaw[k]), -MaxTurn, MaxTurn, kHeadKnee);
          tilt[k] = SoftRange(lifeTilt[k] + kDeg * (kDriftTilt * g * dl[k]), -MaxTilt, MaxTilt, kHeadKnee);
        }
      }

      // ---- 8. The eyes in the head, the lids
      std::vector<float> gazeYaw(n), gazePitch(n), lidFollow(n), lidLift(n);
      {
        float moodLid = settings.Mood == MoodStern ? kSternLid : (settings.Mood == MoodWarm ? kWarmLid : (worried ? kWorriedLid : 0.0f));
        for (size_t k = 0; k < n; ++k)
        {
          float y = worldYaw[k] * kDeg - settings.Vor * turn[k];
          float p = worldPitch[k] * kDeg - settings.Vor * nod[k];
          float blink = 0.5f * (blinkL[k] + blinkR[k]);
          gazeYaw[k] = y;
          gazePitch[k] = p + kBlinkEyeDown * kDeg * blink;
          lidFollow[k] = p * (p > 0.0f ? kLidFollowDown : kLidFollowUp);
          lidLift[k] = kDeg * (kLidLiftEnergy * 2.0f * (energy[k] - 0.5f) + moodLid);
        }
      }

      // ---- 9. The ends and the guard
      std::vector<float> fade(n, 1.0f);
      for (size_t k = 0; k < n; ++k)
      {
        float a = static_cast<float>(k) / kFadeIn;
        float b = static_cast<float>(n - 1 - k) / kFadeOut;
        fade[k] = RaisedCos(std::min(a, 1.0f)) * RaisedCos(std::min(b, 1.0f));
      }
      int changed = 0, notFinite = 0;
      struct Curve
      {
        std::vector<float>* Values;
        float Lo, Hi, Speed;
        float Frame::*Field;
        char const* Name;
      };
      Curve curves[] = {
        { &gazeYaw, -MaxGazeYaw, MaxGazeYaw, MaxGazeSpeed, &Frame::GazeYaw, "GazeYaw" },
        { &gazePitch, -MaxGazeUp, MaxGazeDown, MaxGazeSpeed, &Frame::GazePitch, "GazePitch" },
        { &lidFollow, -MaxLidFollow, MaxLidFollow, MaxLidSpeed, &Frame::LidFollow, "LidFollow" },
        { &lidLift, -MaxLidLift, MaxLidLift, MaxLidLiftSpeed, &Frame::LidLift, "LidLift" },
        { &blinkL, 0.0f, 1.0f, MaxBlinkSpeed, &Frame::BlinkLeft, "BlinkLeft" },
        { &blinkR, 0.0f, 1.0f, MaxBlinkSpeed, &Frame::BlinkRight, "BlinkRight" },
        { &browL, MinBrow, MaxBrow, MaxBrowSpeed, &Frame::BrowLeft, "BrowLeft" },
        { &browR, MinBrow, MaxBrow, MaxBrowSpeed, &Frame::BrowRight, "BrowRight" },
        { &frown, 0.0f, 1.0f, MaxFrownSpeed, &Frame::Frown, "Frown" },
        { &inner, 0.0f, 1.0f, MaxMoodSpeed, &Frame::InnerUp, "InnerUp" },
        { &smile, 0.0f, 1.0f, MaxMoodSpeed, &Frame::Smile, "Smile" },
        { &breath, 0.0f, 1.0f, MaxBreathSpeed, &Frame::Breath, "Breath" },
        { &inhale, 0.0f, 1.0f, MaxInhaleSpeed, &Frame::Inhale, "Inhale" },
        { &nod, -MaxNodUp, MaxNodDown, MaxNodSpeed, &Frame::Nod, "Nod" },
        { &turn, -MaxTurn, MaxTurn, MaxTurnSpeed, &Frame::Turn, "Turn" },
        { &tilt, -MaxTilt, MaxTilt, MaxTiltSpeed, &Frame::Tilt, "Tilt" },
        { &energy, 0.0f, 1.0f, MaxEnergySpeed, &Frame::Energy, "Energy" },
      };
      std::vector<std::string> guardNames;
      std::vector<int> guardBy;
      for (Curve& c : curves)
      {
        int changedBefore = changed;
        std::vector<float>& v = *c.Values;
        bool isEnergy = c.Values == &energy;
        for (size_t k = 0; k < n; ++k)
          v[k] = isEnergy ? 0.5f + (v[k] - 0.5f) * fade[k] : v[k] * fade[k];
        if (isEnergy)
        {
          // energy rests at 0.5: guard its offset from 0.5
          for (float& x : v)
            x -= 0.5f;
          Guard(v, -0.5f, 0.5f, c.Speed, changed, notFinite);
          for (float& x : v)
            x += 0.5f;
        }
        else
          Guard(v, c.Lo, c.Hi, c.Speed, changed, notFinite);
        for (size_t k = 0; k < n; ++k)
          out.Frames[k].*c.Field = v[k];
        guardNames.push_back(c.Name);
        guardBy.push_back(changed - changedBefore);
      }

      // ---- The report
      {
        std::string& r = out.Report;
        double seconds = static_cast<double>(n - 1) / kFps;
        int speaking = 0, speakingAway = 0, scans = 0, aw = 0, rhythm = 0, partial = 0, longs = 0, doubles = 0;
        for (size_t k = 0; k < n; ++k)
          speaking += zone[k] == 2 ? 1 : 0;
        for (Away const& w : aways)
        {
          ++aw;
          for (int k = w.From; k < w.To && k < count; ++k)
            speakingAway += zone[k] == 2 ? 1 : 0;
        }
        for (Fix const& f : fixes)
          scans += f.Kind == 0 || f.Kind == 2 || f.Kind == 3 ? 1 : 0;
        int talkingBlinks = 0, quietBlinks = 0, talkingFrames = 0;
        for (size_t k = 0; k < n; ++k)
          talkingFrames += zone[k] >= 1 ? 1 : 0;
        for (BlinkPlan const& b : blinks)
        {
          partial += b.Partial ? 1 : 0;
          longs += b.Long ? 1 : 0;
          doubles += b.Why == "the second of two" ? 1 : 0;
          rhythm += b.Why.compare(0, 6, "rhythm") == 0 ? 1 : 0;
          if (b.Start >= 0 && b.Start < count && zone[b.Start] >= 1)
            ++talkingBlinks;
          else
            ++quietBlinks;
        }
        Line(r, "seconds", seconds, "%.2f");
        Line(r, "phrases", pn, "%.0f");
        Line(r, "looks away", aw, "%.0f");
        Line(r, "share of the speaking time looking away", speaking > 0 ? static_cast<double>(speakingAway) / speaking : 0.0, "%.2f");
        Line(r, "small saccades (scan, away, back)", scans, "%.0f");
        Line(r, "blinks", static_cast<double>(blinks.size()), "%.0f");
        Line(r, "blinks a minute while talking", talkingFrames > 0 ? talkingBlinks * 6000.0 / talkingFrames : 0.0, "%.1f");
        Line(r, "blinks a minute in quiet", static_cast<int>(n) - talkingFrames > 0 ? quietBlinks * 6000.0 / (static_cast<int>(n) - talkingFrames) : 0.0, "%.1f");
        Line(r, "blinks by rhythm", rhythm, "%.0f");
        Line(r, "partial blinks", partial, "%.0f");
        Line(r, "long blinks", longs, "%.0f");
        Line(r, "double blinks", doubles, "%.0f");
        Line(r, "guard changed values", changed, "%.0f");
        Line(r, "guard not-a-number values", notFinite, "%.0f");
      }
      if (debug)
      {
        debug->Events.swap(events);
        debug->GuardChanged = changed;
        debug->GuardNotFinite = notFinite;
        debug->GuardNames.swap(guardNames);
        debug->GuardChangedBy.swap(guardBy);
      }
      return true;
    }
    catch (std::bad_alloc const&)
    {
      out = Track();
      if (debug)
        *debug = Debug();
      error = "not enough memory";
      return false;
    }
  }

  bool Analyse(short const* pcm, size_t samples, int rate, int channels, Settings const& settings, Track& out,
    std::string& error, Debug* debug)
  {
    out = Track();
    voice_life::Track life;
    voice_life::Debug lifeDebug;
    if (!voice_life::Analyse(pcm, samples, rate, channels, life, error, &lifeDebug))
      return false;
    return Make(life, lifeDebug, settings, out, error, debug);
  }

  Frame At(Track const& track, double seconds)
  {
    Frame zero = {};
    size_t n = track.Frames.size();
    double fps = track.FramesPerSecond;
    if (n == 0 || !std::isfinite(seconds) || !std::isfinite(fps) || !(fps > 0.0))
      return zero;
    double x = seconds * fps;
    if (!std::isfinite(x) || x < 0.0 || x > static_cast<double>(n - 1))
      return zero;
    size_t a = static_cast<size_t>(x);
    if (a >= n - 1)
      return track.Frames[n - 1];
    float u = static_cast<float>(x - static_cast<double>(a));
    Frame const& p = track.Frames[a];
    Frame const& q = track.Frames[a + 1];
    static float Frame::* const fields[] = { &Frame::GazeYaw, &Frame::GazePitch, &Frame::LidFollow, &Frame::LidLift,
      &Frame::BlinkLeft, &Frame::BlinkRight, &Frame::BrowLeft, &Frame::BrowRight, &Frame::Frown, &Frame::InnerUp,
      &Frame::Smile, &Frame::Breath, &Frame::Inhale, &Frame::Nod, &Frame::Turn, &Frame::Tilt, &Frame::Energy };
    static_assert(sizeof(fields) / sizeof(fields[0]) * sizeof(float) == sizeof(Frame), "a field of Frame is not mixed");
    Frame f = zero;
    for (float Frame::* m : fields)
      f.*m = p.*m + (q.*m - p.*m) * u;
    return f;
  }
}
