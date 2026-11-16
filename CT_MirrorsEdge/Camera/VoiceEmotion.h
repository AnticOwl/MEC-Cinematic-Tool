#pragma once
#include "VoiceFeel.h"
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace voice { struct Sound; }

// GAME > Pose window > Voice > "From the voice": the feeling read from the voice itself, stretch by stretch (the
// user, 2026-11-13: "emotions should apply automatically depending on the sound file expressions").
//
// A trained model reads it (the optional download, Cinematic Tools\EmotionModel: EmotionHelper.exe and
// emotion_feel.onnx): audeering's wav2vec2 emotion model (CC BY-NC-SA 4.0) and a small head trained on the RAVDESS
// recordings that gives the chances of 8 feelings. It runs in EmotionHelper.exe, outside the game, at idle priority,
// once per voice (the answers are kept in EmotionModel\cache, so the same voice is read once).
//
// What it gets right, measured offline on short acted studio recordings, one whole recording at a time
// (Research_Pose_Editor section 32; how right it is on game dialogue hasn't been measured): on English actors it never
// heard, 77 % of recordings get the right one of the 8 feelings, about 9 in 10 when it is 80 % sure or more; on German
// actors (recordings it never saw) 54 %, about 3 in 4 when 90 % sure or more (4 in 5 at 95 %). So a stretch's feeling
// shows only as far as the model is sure of it (smoothstep from FeelSure to FeelSure + SureSpan of its chance, at most
// to a chance of 1), the rest of the face keeps the feeling the user picked; the voice's "neutral" keeps the feeling
// picked; each stretch can be set by hand. Every number here is a design [d] or a measure [m], named below.
namespace voice_emotion
{
  // The model's feelings, in its order
  enum Class { Neutral, Calm, Happy, Sad, Angry, Fearful, Disgust, Surprised, ClassCount };
  extern const char* const ClassNames[ClassCount];   // "neutral", "calm", ... (the menu, the log)
  // The preset each shows (-1: neutral, the feeling picked)
  int PresetOf(int cls);

  // A stretch of the voice with a feeling of its own: a phrase (voice_life's: speech with no pause of 250 ms), or a
  // part of a long one; and what the model hears for it
  const double LongPhrase = 6.0;     // [d] s: a longer phrase is split into parts
  const double PartLength = 4.5;     // [d] s: ... of at most this (and over 3 s: 6 s split in 2 gives 2 x 3.0)
  const double ShortestHeard = 3.0;  // [d] s: a shorter phrase is heard with the voice around it, this long in all
                                     // (the feeling head was trained on 2.9 - 5.3 s recordings and tested on 1.2 - 9 s
                                     // ones [m]; a 0.66 s phrase of the user's test voice, gman_rise_and_shine.mp3,
                                     // a game line, read alone gave arousal 0.13, within 3 s 0.23 [m])
  // [HEARD TOGETHER] Short phrases the model hears together are heard as one: where the next phrase begins inside what
  // a short phrase (or a group of them) is heard as, and is itself shorter than ShortestHeard, it joins the group (up
  // to LongPhrase from the group's start), and every phrase of the group is heard as the group's own window (its span,
  // or ShortestHeard around it). Each phrase keeps its own stretch (its own time and its own setting in the list); the
  // helper hears each window once, so they share one reading. Why: in the game voices tried (Faith's lines), two
  // phrases' windows that shared 98 % of their samples were heard as different feelings (fearful 38 % against calm
  // 29 %: the model's sensitivity to where a window begins, research 32.6), and one 3 s window was read twice [m].
  struct Stretch
  {
    double From = 0.0, To = 0.0;           // where it is in the voice (seconds)
    double HeardFrom = 0.0, HeardTo = 0.0; // what the model hears for it (the same for the phrases heard together)
    int Phrase = 0;                        // its phrase (0 = the first), and its part of it (0 = the first)
    int Part = 0;
    int Parts = 1;
  };
  // The stretches of a voice from its phrases (sorted by where they begin; empty or backwards phrases left out).
  std::vector<Stretch> StretchesOf(std::vector<double> const& phraseFrom, std::vector<double> const& phraseTo, double seconds);
  // The windows the helper hears for them, each once (stretches next to each other heard alike share one), and each
  // stretch's window. The readings come back one per window.
  void WindowsOf(std::vector<Stretch> const& stretches, std::vector<std::pair<double, double>>& windows,
    std::vector<size_t>& windowOf);

  // What the model heard in a stretch
  struct Reading
  {
    bool Ok = false;                       // false: too short to hear (under 0.1 s), no answer
    float Arousal = 0.f, Dominance = 0.f, Valence = 0.f;   // about 0 .. 1 (audeering's scale)
    float Chance[ClassCount] = {};         // sum 1
  };

  // A stretch's own setting (the menu): from the voice, the feeling picked, none, or a preset (>= 0)
  const int FromVoice = -3, AsPicked = -2, NoFeeling = -1;

  const float SureDefault = 0.55f;   // [d] the chance at which a feeling starts to show: chosen from the measured trade-off
                                     // (section 32.5: on unseen English actors 57 % of the face right, 5 % wrong; German
                                     // 44 / 18)
  const float SureSpan = 0.4f;       // [d] ... and is all there this much above it, or at a chance of 1 if that's less
  const float SureMost = 0.95f;      // [d] the setting's range
  const float SureLeast = 0.3f;
  // How strongly a lively feeling heard in the voice shows, by how stirred-up the voice is (arousal): RAVDESS's strong
  // takes of happy, angry, fearful, surprised and disgust are 0.09 - 0.24 higher than its normal ones [m]. Only that
  // feeling's own part of the face is scaled (not the feeling picked under it).
  const float StrengthLeast = 0.85f; // [d] at arousal ArousalLow and under
  const float StrengthMost = 1.15f;  // [d] at ArousalHigh and over
  const float ArousalLow = 0.45f;    // [d] (RAVDESS normal happy 0.55, angry 0.64; strong 0.73, 0.88 [m])
  const float ArousalHigh = 0.85f;

  // What a stretch shows: the presets' weights ([0] none, [1 + f] feeling f; they sum to 1), the weights its units are
  // laid with (the same, but the voice's lively feeling's part scaled by arousal), and for the menu what it shows.
  // picked: the feeling picked as it is shown now (the same layout; it needn't sum to 1)
  const int LookSlots = voice_feel::FeelingCount + 1;
  struct Look
  {
    float Weights[LookSlots] = {};
    float Units[LookSlots] = {};
    int Shown = -1;      // the preset the voice (or the stretch's own setting) puts on (-1: none put on)
    float Share = 0.f;   // ... and its share of the face (a stretch set to none: 1, the face without a feeling)
    float Scale = 1.f;   // the voice's lively feeling's own part scaled by this (arousal; 1 otherwise)
    bool ByHand = false; // the stretch's own setting, not the voice
  };
  Look LookOf(Reading const* reading, int setting, float const* picked, float sure);
  // [WHY] Why a stretch shows what it shows (the menu): its own setting; too short to hear; heard as neutral (the
  // feeling picked); its feeling on half the face or more; on less; or not sure enough to show at all
  enum Why { WhyByHand, WhyTooShort, WhyNeutral, WhyShown, WhyPartly, WhyNotSure };
  Why WhyOf(Reading const* reading, Look const& look);
  // The 8 classes by their chance, the likeliest first (equal chances: in the model's order)
  void ChancesInOrder(Reading const& reading, int order[ClassCount]);

  // The looks of the stretches mixed at a time into the voice: each stretch's look holds from a little before its
  // start (Lead, never before the middle of the pause before it) to the next one's; a change takes changeSeconds,
  // centred there; before the first and after the last, theirs. loopLength > 0: the voice loops at that length, and the
  // last stretch changes into the first across the loop's end in the same way (each change then kept within the loop,
  // its centre at least changeSeconds / 2 from either end, so none is cut at the loop's end; a voice shorter than
  // changeSeconds isn't blended across it). A time, not a frame count: the same time always gives the same face
  // (scrubbed, looped, played slow). share: the looks' shares mixed; stretch: the stretch with the most weight, and
  // stretchWeight: its weight (under 1: changing). False: no stretches.
  const double Lead = 0.25;          // [d] s
  bool MixAt(std::vector<Stretch> const& stretches, std::vector<Look> const& looks, double at, double changeSeconds,
    double loopLength, float* weights, float* units, float& share, int& stretch, float& stretchWeight);

  // ---- Reading a voice (EmotionHelper.exe, a thread of the tools' own; one at a time)
  // The optional download's files are there and Windows ML is on this Windows (looked at again at most every 2 s); why
  // not, in why (a whole sentence for the menu). Its first call also clears what a game that ended mid-reading left in
  // EmotionModel\cache.
  bool Available(std::string* why = nullptr);
  enum State { Waiting, Running, Done, Failed, Cancelled };
  struct Request;
  std::shared_ptr<Request> Start(std::shared_ptr<voice::Sound const> const& sound, std::vector<Stretch> const& stretches);
  // Stops it (a helper running is ended; one still waiting lets its sound go at once, or once the 8 MB of it being
  // hashed are done: a few milliseconds)
  void Cancel(std::shared_ptr<Request> const& request);
  struct Status
  {
    State Now = Waiting;
    int Done = 0, Total = 0;               // stretches read so far (the helper's progress file)
    bool FromCache = false;                // the answers were kept from an earlier reading
    bool CanRetry = true;                  // Failed: trying again may help (false: the sound itself can't be read, or
                                           // this Windows' Windows ML can't run the helper)
    double Seconds = 0.0;                  // how long it took (Done, Failed)
    std::string Message;                   // Failed: why
    std::shared_ptr<std::vector<Reading> const> Readings;   // Done: one per stretch
    // [PROGRESS] While Running: how long the helper has run, and whether its model has started (its first progress
    // written: before that, it reads the job, loads the model and checks it)
    double Elapsed = 0.0;
    bool ModelReady = false;
  };
  Status StatusOf(Request const& request);

  const int RateLeast = 8000, RateMost = 200000;   // [d] the sound rates read (the helper's own limits; the loader
                                                   // takes up to 200,000 Hz). 8,000: the phone band; the model was
                                                   // measured only on 16 and 48 kHz recordings, so voices under 16 kHz
                                                   // are unmeasured too

  // [MEMORY] The helper holds about 1.7 GB [m] (research 32.2) while it runs, and the game ends itself when Windows has
  // no memory left to give (its VirtualAlloc failing: the 11-11 crash, Research_CPU_Memory_2026-11-13): a reading
  // isn't started with less than this left to give (Windows' commit: what's left of its commit limit), and a helper
  // running is stopped when it falls under RunFloorBytes. Both are Try again.
  const unsigned long long HelperCommitBytes = 1700ull << 20;   // [m]
  const unsigned long long StartReserveBytes = 4096ull << 20;   // [d] left over for the game and everything else
  const unsigned long long RunFloorBytes = 2048ull << 20;       // [d]

  // ---- For the offline tests
  // A helper result file's text read into readings (false: not one, or an error, in why)
  bool ParseResult(std::string const& text, size_t stretches, std::vector<Reading>& readings, std::string& why);
  // What Windows has left to give (bytes of commit; ~0ull: not known, which never stops a reading). The tests replace it.
  extern unsigned long long (*CommitLeft)();
}
