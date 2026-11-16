#pragma once
#include "VoiceFeelFace.h"
#include "VoiceLife.h"
#include <cstddef>
#include <vector>

// The feeling the user picks for a voice, and how strongly it shows moment by moment. PROTOTYPE (2026-11-13).
//
// What the sound can and can't give (see the research notes): how stirred-up a voice is (loud, high, fast, wide
// pitch) comes through the sound fairly well; WHICH feeling (happy or angry, sad or tender) much less, and without
// the words hardly at all. So the user picks the feeling and its strength, and the sound only moves its strength:
// more on the stressed words, less in the pauses, never gone while the voice is on.
//
// Energy: 0 .. 1 at 100 frames a second, from voice_life's own analysis of the sound (its syllables, their stress
// score and its phrases), worked out once at load. Every number here is a design [d], named below.
namespace voice_feel
{
  enum Feeling
  {
    Calm,        // [d] the default: relaxed lids, a trace of a lower-lid raise (the game's own men hold one)
    Happy,       // AU 6 + 12 (EMFACS)
    Tender,      // [d] a soft smile, lids a little lowered, the head tilted
    Sad,         // AU 1 + 4 + 15 (EMFACS), 17 and a lid droop (Du et al. 2014: 17 in 67 %)
    Worried,     // [d] AU 1 + 4, a little 20 and 24 (the fear and sadness brow without the wide eyes)
    Afraid,      // AU 1 + 2 + 4 + 5 + 7 + 20 (EMFACS), the lips parted (26: Preset::Jaw)
    Surprised,   // AU 1 + 2 + 5 (EMFACS), the jaw dropped (26: Preset::Jaw)
    Angry,       // AU 4 + 5 + 7 + 23 (EMFACS), 24 and 17 (Du et al. 2014: 24 prototypical, 17 in 52 %)
    Tense,       // [d] AU 4 + 7 + 24 at a low level
    Disgusted,   // AU 9 + 15 + 17 (EMFACS), 10 (Du et al. 2014: prototypical)
    Contempt,    // AU 12 + 14 on the right side only (EMFACS: R12A + R14A)
    FeelingCount
  };

  struct Preset
  {
    const char* Name;
    float Au[voice_face::AuCount];          // both sides, at strength 1
    float RightOnly[voice_face::AuCount];   // more on the character's right only
    float Nod, Tilt;                        // the head, radians (> 0: chin down; top to the right)
    float RaiseKeep;                        // the share of the life's brow raises kept (at strength 1 and more; less
                                            // held back below it, none at 0: [REVIEW 5])
    // [JAW] The jaw down (AU 25 / 26), in the mouth's own Open (1: 9 degrees, the lower lip 15 - 16 mm down) at
    // strength 1, laid under the speech's own (the speech still opens it fully; where the lips close for p, b and m
    // it goes: FeelJawShutAt), scaled by the user's Mouth opening as the speech's jaw is: Surprised
    // 1 + 2 + 5B + 26 and Fear 1 + 2 + 4 + 5 + 7 + 20 + 26 (EMFACS, Wikipedia's FACS table); Du, Tao and Martinez 2014
    // list 25 and 26 for surprised and 25 for fearful as prototypical (not re-checked: its page was refused). How
    // much: designed [d]
    float Jaw;
  };
  extern const Preset Presets[FeelingCount];

  // The voice's energy from voice_life's analysis of the same sound (Analyse with a Debug): frames values, one
  // every 1 / 100 s. False (energy empty): the analysis holds no frames.
  bool Energy(voice_life::Debug const& debug, size_t frames, std::vector<float>& energy);
  // The energy at a time (between frames: mixed; outside: 0)
  float EnergyAt(std::vector<float> const& energy, double seconds);

  struct Settings
  {
    Feeling Feel = Calm;
    float Strength = 1.f;        // 0 none .. 1 as designed .. 2
    float FollowVoice = 1.f;     // 0: the same all through .. 1: as designed (weaker in pauses, stronger on stress)
    float Asymmetry = 0.f;       // 0 .. 0.3: one side this much stronger, the other this much weaker
    bool LeftStronger = false;
  };

  // This moment's shape: the preset at the strength, moved by the energy, the lip units eased where the speech
  // needs the lips. mouth: this moment's mouth shape (voice_face::Shape), closure: the user's Closure amount.
  voice_face::FeelShape ShapeAt(Settings const& settings, float energy, voice_face::Shape const& mouth, float closure);
  // [MERGE 1] The same for a preset given (settings.Feel is not used): a mix of presets while the user changes the
  // feeling (the pose editor blends the old one into the new). [SMILE] smile: the smile's own share now (SmileFactor;
  // 1: all of it, as before), on the both-sides cheek raiser and lip corner puller only (AU 6 and 12; Contempt's
  // one-sided smirk is not a smile and keeps its own)
  // [JAW] jawShut: the closure the feeling's jaw goes by (taken over time by the caller, FeelJawSpread); below 0: this
  // moment's JawClosure of mouth.Closed and closure
  voice_face::FeelShape ShapeOf(Preset const& preset, Settings const& settings, float energy, voice_face::Shape const& mouth,
    float closure, float smile = 1.f, float jawShut = -1.f);

  // [SMILE] A smile that comes and goes with the speech, not one held at its fullest for the whole voice. Felt smiles
  // last about 0.5 - 4 s (Ekman and Friesen 1982, as Krumhuber and Kappas 2005 cite them, J Nonverbal Behav 29:3-24,
  // read in full); in Krumhuber and Kappas' own tests (isolated smiles on made-up faces, no speech, linear ramps back
  // to a neutral face) onsets up to 16 frames at 30 fps and offsets up to 23 frames read more genuine the longer they
  // were, and a smile held at its fullest 149 frames (about 5 s) was judged more fake than one of 11 (119 frames,
  // about 4 s: not significantly). So: the smile rises a little before speech (over SmileOnset), is at its fullest at
  // most SmileApexMost, then eases to SmileHold over SmileRelax; a phrase that begins after that brings it back up;
  // after the speech it eases down to SmileRest over SmileOffset. Phrases less than SmileMerge apart are one stretch
  // of smiling. Every
  // level and time here is designed [d] within those findings; how it reads on Faith is unknown until seen.
  struct SmileTimes
  {
    std::vector<double> From, To;      // the stretches of smiling (phrases less than SmileMerge apart joined), sorted
    std::vector<double> Starts;        // every phrase's start, sorted
    std::vector<size_t> FirstStart;    // each stretch's first start in Starts, and the one after its last
    std::vector<size_t> EndStart;
  };
  // From voice_life's phrases (false, out empty: none, and the smile is then always all there)
  bool SmileTimesOf(voice_life::Debug const& debug, SmileTimes& out);
  // How far up the smile is at a time (0: at rest, 1: its fullest); loopSeconds > 0: the voice loops at that length
  // (the first stretch's rise before 0 and the last one's fall after the end carry across the loop's end)
  float SmileLevelAt(SmileTimes const& times, double seconds, double loopSeconds = 0.0);
  // The smile's share of itself shown: comes 0: all of it always (as before 2026-11-16); 1: SmileRest at rest,
  // SmileRest + (1 - SmileRest) x the level otherwise. No phrases: all of it.
  float SmileFactor(SmileTimes const& times, double seconds, double loopSeconds, float comes);
  const double SmileLead = 0.25;     // [d] s: it begins to rise this long before a phrase
  const double SmileOnset = 0.5;     // [d] s: from rest to its fullest (K & K: 4 - 16 frames tested, longer read better)
  const double SmileApexMost = 4.0;  // [d] s: at its fullest at most this long
  const double SmileRelax = 1.0;     // [d] s: ... then eased to SmileHold over this
  const double SmileOffset = 0.75;   // [d] s: down to rest after the speech (K & K: up to 23 frames, 0.77 s, read better)
  const double SmileMerge = 1.0;     // [d] s: phrases closer than this are one stretch of smiling (at least SmileLead +
                                     // SmileOffset, so one stretch's fall ends before the next one's rise begins)
  const float SmileHold = 0.7f;      // [d] its level after the fullest, while the speech goes on
  const float SmileRest = 0.5f;      // [d] its share at rest (between stretches of speech, before, after)

  // [d] the energy's design
  const float SyllableRise = 0.15f;      // s: a syllable's energy grows over this before its centre
  const float SyllableFall = 0.35f;      // s: and fades with this time constant after its end
  const float StressBase = 0.5f;         // a syllable's height at stress score 0
  const float StressGain = 0.25f;        // per unit of stress score (the score runs about -2.5 .. 2.5)
  const float SyllableLeast = 0.2f;      // the least height of a syllable of speech
  const float PhraseFloor = 0.35f;       // inside a phrase the energy is never under this
  const float PhraseRise = 0.2f;         // s: the floor comes up over this before a phrase
  const float PhraseFall = 0.6f;         // s: and fades with this time constant after it
  // [d] how far the feeling eases at zero energy (a share of it), per part of the face
  const float FeelDip = 0.5f;
  const float FollowBrows = 1.f, FollowEyes = 0.6f, FollowMouth = 0.3f;
  // [d] [JAW] a feeling's jaw is all gone where the lips' closure for p, b and m (JawClosure, the most of it within
  // FeelJawSpread frames either way, nearer ones counting more) reaches this, less of it below. [FIX CHECK] The closure
  // taken over time, not this frame's: gated on this frame's alone the jaw snapped shut and open in 20 - 30 ms, faster
  // than the speech's own jaw ever moves (2.26 - 2.89 degrees in 10 ms against its 2.07). Measured on the five voices
  // tried (Emotion_Tests_2026-11-16\jaw_gate.cpp): no closure left apart at Lip closing 0.5, 1 and 2, Mouth smoothing
  // 0, 1 and 2, Surprised and Afraid at Strength 1 and 2; the jaw's largest step in 10 ms the speech's own at Mouth
  // smoothing 0 and 1, and at most 1.76 degrees against its 1.44 at 2.
  const float FeelJawShutAt = 0.3f;
  const int FeelJawSpread = 8;       // [d] frames of 10 ms
  // The closure a feeling's jaw goes by at one moment: the mouth's Closed (how sure the analysis is of a p, b or m), at
  // least as strong as Lip closing makes it (Lip closing under 1 still shuts the speech's own jaw there, the analysis
  // having pushed its opening down); none at Lip closing 0 (p, b and m left out: the jaw stays)
  float JawClosure(float closed, float lipClosing);
}
