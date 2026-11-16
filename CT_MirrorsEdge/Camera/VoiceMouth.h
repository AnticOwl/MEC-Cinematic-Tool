#pragma once

// Speech turned into mouth shapes by signal processing alone: no phoneme recogniser, no training data, no
// libraries. Analyse takes a whole sound as 16-bit samples and gives five curves, each from 0 to 1, at 100 frames
// a second. Frame k is the mouth at k / FramesPerSecond seconds from the first sample.
//   Open    how far the jaw is open (0 shut, 1 a wide "ah")
//   Wide    the mouth's corners pulled apart ("ee")
//   Round   the lips rounded and pushed forward ("oo", "w")
//   Closed  the lips pressed together (p / b / m). The amount is how sure the analysis is that a closure is made
//           with the lips, not how hard they press. The jaw curve is already multiplied down where this is high.
//   Teeth   the teeth together with the lips apart (s / z / sh); half at most for the weak hiss of f / th
// A resting, silent mouth is all zeros. The curves are smoothed evenly to both sides, so they do not lag the
// sound, and they lead it by 10 ms. They are smoothed three ways (Track: Frames, Before, Softer), for the menu's
// Mouth smoothing 1, 0 and 2.
//
// Analyse may run on any thread, and on several at once: it holds no global state.
//
// Its limits, as measured on synthetic voices: the jaw follows speech well; Wide and Teeth are fair; Round is weak;
// Closed is a hint, not a fact. On recordings of people only the speeds were measured, and on EmoDB's hand-labelled
// German speech whether p / b / m are seen as shut (84 %, and 82 % of t / d / n / k / g / l too, before 2026-11-14;
// 83 and 81 % at Mouth smoothing 1). Music or noise under the voice moves the mouth too.

#include <cstddef>
#include <string>
#include <vector>

namespace voice_mouth
{
  struct Frame
  {
    float Open;
    float Wide;
    float Round;
    float Closed;
    float Teeth;
  };

  struct Track
  {
    double FramesPerSecond = 100.0;
    // Mouth smoothing 1 (the default since 2026-11-14): the jaw eases in and out of each shape and still shuts
    // almost everywhere it shut before. Its moves of 0.3 or more go from 10 to 90 % in a median 59 ms (measured on 31 recorded
    // voices, Research_Pose_Editor section 33).
    std::vector<Frame> Frames;
    // The same frames smoothed as up to 2026-11-13 (Mouth smoothing 0: quicker, a median 43 ms opening and 49 ms
    // shutting), and softer still (2: about 70 ms, the closures eased too). As many as Frames.
    std::vector<Frame> Before;
    std::vector<Frame> Softer;
    // Plain text, one "name: value" a line: what was measured in the sound.
    std::string Report;
  };

  // pcm holds samples * channels values (left, right, left, ... when channels is 2). rate is 8000 to 192000,
  // channels is 1 or 2. Two channels are mixed into one; if they cancel each other (one is the other upside
  // down), the right one is turned over first.
  // True: out holds floor(seconds * 100) + 1 frames (in each of Frames, Before and Softer). A sound with no speech
  // in it gives frames of zeros.
  // False, with the reason in error and no frames: bad arguments, or not enough memory.
  bool Analyse(short const* pcm, size_t samples, int rate, int channels, Track& out, std::string& error);

  // For the test program: the per-frame measurements behind the curves, as named columns, and one text line for
  // each lip-closure candidate.
  struct Debug
  {
    std::vector<std::string> Names;
    std::vector<std::vector<float>> Columns;
    std::string Closures;
  };

  // The same, and fills debug unless it is null.
  bool Analyse(short const* pcm, size_t samples, int rate, int channels, Track& out, std::string& error, Debug* debug);
}
