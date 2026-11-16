#pragma once

// The life around a talking mouth, worked out from the sound alone: blinks, brow raises on stressed words and the
// head's own small movements. No recogniser, no training data, no libraries. Analyse takes a whole sound as 16-bit
// samples and gives five curves at 100 frames a second. Frame k is the face at k / FramesPerSecond seconds from the
// first sample. Everything is worked out once; nothing is kept between frames at playback, so the sound may be
// scrubbed, looped or played slow.
//   Blink  0 the eyes as posed .. 1 shut
//   Brow   0 at rest .. 1 a clear raise
//   Nod    the head's turn that lowers the chin, in radians (above 0: the chin goes down)
//   Turn   the head's turn that takes the nose towards the character's own right, in radians
//   Tilt   the head's lean that takes the top of the head towards the character's own right, in radians
// Where there is no speech (silence, noise, a steady tone, held notes, a whisper, drum beats, a drone, a murmur of
// many voices) Brow, Nod, Turn and Tilt are 0 or fade to it, and only the blinks go on. The first and the last
// frame are at rest. The same sound always gives the same curves, and so does a louder or quieter copy of it
// (what looks like chance is drawn from the sound's length in tenths of a second and from where in the sound each
// choice is made, to half a second: a small change in the sound changes at most the choice next to it).
//
// How it is made (voice_life.cpp; every number is a named constant there):
//   A loudness track and a pitch track (60 - 400 Hz) of the voice. Syllables: the voiced peaks of the loudness.
//   Phrases: speech with no pause of 250 ms. A stress score for each syllable: its level (3 dB: one unit), its
//   pitch over the running pitch (2 semitones, or the voice's own pitch spread if larger) and over the syllable
//   before, its length (1.5 times: one unit), each against its neighbours.
//   Brow: a raise on the strongest stresses by a score that counts the pitch most, 2 s apart at least. It starts
//   160 ms before the syllable's centre, is up after 180 ms, holds 120 - 300 ms, lets go over 500 ms. Its height
//   grows with the score from nothing, so there are faint ones.
//   Nod: a dip on the strongest stresses by a score that counts level and length most, 0.7 s apart at least: down
//   over 180 ms, lowest at the syllable's centre, back over 420 ms, up to 3.5 degrees. A dip of up to 1.5 degrees
//   into a phrase's end when the pitch falls there and a pause follows; it grows from nothing with the pause
//   (250 ms: none, 450 ms: all of it) and with the phrase's length (0.6 s: none, 1.2 s: all of it). A slow drift
//   of up to 1.2 degrees, up when the voice is high and loud against its own middle, down when low and soft.
//   Turn, Tilt: a new small pose (up to 2.2 and 1.4 degrees) for a phrase, not more often than every 3 s, reached
//   over 1.2 s; back to rest from 0.5 s into a pause of 1 s or more.
//   Blink: shut in 80 ms, shut for 30 ms, open in 170 ms. 2 to 6 s from one to the next; drawn to just after a
//   phrase's end, to the start of a strong nod and to just before a phrase; none inside a phrase shorter than
//   1.5 s unless a strong nod is there; about one in eight is followed by a second one 420 ms later, where no
//   speech has begun by then.
//
// Analyse may run on any thread, and on several at once: it holds no global state.
//
// Its limits. The pitch tracker was measured on made-up signals with a known pitch and held against two other
// estimators on one real recording; the syllables and the stress score were checked on synthetic voices (vowels
// from the synthesiser's own timeline, one word made louder, higher or longer). Whether the result LOOKS natural
// was not judged by anyone. A whisper gets blinks only. A singing voice or an instrument with vibrato is taken for
// speech. Music under the voice: soft music (20 dB under) changes little, louder music stills the face and can
// put a nod in a wrong place. Noise under the voice makes the moves fewer; one or two land in other places.
// The head sets out for its pose up to 0.6 s before a phrase's first sound and is back at rest 1.1 s after the
// last sound before a long pause.

#include <cstddef>
#include <string>
#include <vector>

namespace voice_life
{
  struct Frame
  {
    float Blink;
    float Brow;
    float Nod;
    float Turn;
    float Tilt;
  };

  struct Track
  {
    double FramesPerSecond = 100.0;
    std::vector<Frame> Frames;
    // Plain text, one "name: value" a line: what was measured in the sound.
    std::string Report;
  };

  // The limits every frame keeps, for any input (radians; the degrees are in the comments). The largest values
  // the design itself reaches are a little under the speed limits: blink 19.6, brow 8.7, nod about 35 degrees,
  // turn about 7.2 degrees, tilt about 4.6 degrees a second.
  const float MaxNodDown = 0.0820f;    // 4.7 degrees: the largest dip (3.5) plus the largest drift (1.2)
  const float MaxNodUp = 0.0210f;      // 1.2 degrees: the drift alone
  const float MaxTurn = 0.0384f;       // 2.2 degrees to either side
  const float MaxTilt = 0.0245f;       // 1.4 degrees to either side
  // The largest change in a second (radians, or units for Blink and Brow)
  const float MaxBlinkSpeed = 20.0f;   // shut in 80 ms
  const float MaxBrowSpeed = 9.0f;     // up in 180 ms
  const float MaxNodSpeed = 0.63f;     // 36 degrees a second
  const float MaxTurnSpeed = 0.14f;    // 8 degrees a second
  const float MaxTiltSpeed = 0.09f;    // 5.16 degrees a second

  // pcm holds samples * channels values (left, right, left, ... when channels is 2). rate is 8000 to 192000,
  // channels is 1 or 2. Two channels are mixed into one; if they cancel each other (one is the other upside
  // down), the right one is turned over first.
  // True: out holds floor(seconds * 100) + 1 frames (as many as voice_mouth gives for the same sound).
  // False, with the reason in error and no frames: bad arguments, or not enough memory.
  bool Analyse(short const* pcm, size_t samples, int rate, int channels, Track& out, std::string& error);

  // The curves at a time into the sound (between two frames: mixed). Before the start and after the last frame:
  // all zero, the face at rest.
  Frame At(Track const& track, double seconds);

  // For the test program: what the curves were made from.
  struct Syllable
  {
    double Seconds = 0.0;      // its centre (the loudness peak)
    double From = 0.0;         // where it begins and ends
    double To = 0.0;
    float Level = 0.0f;        // dB against the recording's speech level
    float Pitch = 0.0f;        // Hz, 0 if none was found
    float Length = 0.0f;       // seconds within 10 dB of its peak
    float LevelScore = 0.0f;   // each against the neighbours, about -2.5 to 2.5
    float PitchScore = 0.0f;
    float RiseScore = 0.0f;
    float LengthScore = 0.0f;
    float Stress = 0.0f;       // the stress score
    float BrowScore = 0.0f;
    float NodScore = 0.0f;
    float Speech = 1.0f;       // 0 for a held note (the pitch stands still), 1 for speech
    int Phrase = -1;
  };

  struct Event
  {
    double Seconds = 0.0;      // brow, nod: the syllable's centre; blink: where the lids start to close
    std::string Kind;          // "brow", "nod", "end dip", "blink", "pose", "octave" (a pitch run put down an octave)
    float Size = 0.0f;         // brow: its height; nod, end dip: degrees; pose: the turn in degrees; octave: -12
                               // (faint ones are listed too: a brow under 0.25, a nod under 0.7, a dip under 0.3)
    float Score = 0.0f;        // brow, nod: the score that chose it; pose: the tilt in degrees; octave: the run's seconds
    float Stress = 0.0f;       // brow, nod: the syllable's stress score; octave: the run's pitch before, Hz
    std::string Why;           // blink: what placed it
  };

  struct Debug
  {
    std::vector<std::string> Names;              // per-frame columns
    std::vector<std::vector<float>> Columns;
    std::vector<Syllable> Syllables;
    std::vector<Event> Events;                   // in the order they were made, not by time
    std::vector<double> PhraseFrom;              // seconds
    std::vector<double> PhraseTo;
  };

  // The same, and fills debug unless it is null.
  bool Analyse(short const* pcm, size_t samples, int rate, int channels, Track& out, std::string& error, Debug* debug);
}
