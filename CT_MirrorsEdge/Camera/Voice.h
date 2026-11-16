#pragma once
#include "VoiceFace.h"
#include "VoiceLife.h"
#include "VoiceLifeFace.h"
#include <memory>
#include <string>
#include <vector>

// A voice file for a posed character: its sound (decoded by Windows' own Media Foundation, VoiceDecode.cpp), the
// mouth shapes worked out from it (VoiceMouth.cpp) and the life around the mouth: blinks, brow raises and the head's
// own small movement (VoiceLife.cpp), read once on a thread of its own and then only read.
// GAME > Pose window > Voice (PoseEditor.cpp) lays the shapes over the face bones (VoiceFace.cpp, VoiceLifeFace.cpp)
// and has the sound played in step (VoicePlayer.cpp).
namespace voice
{
  const double MaxSeconds = 600.0;         // a longer file is read up to here (ten minutes: 106 MB at 44,100 Hz stereo)
  const int OverviewPoints = 240;          // the menu's small picture of where the mouth moves

  struct Sound
  {
    std::string File;                      // its file's name (UTF-8)
    int Rate = 0, Channels = 0;
    std::shared_ptr<const std::vector<short>> Pcm;   // interleaved 16-bit
    double Seconds = 0.0;
    double MouthRate = 100.0;              // shapes a second
    std::vector<voice_face::Shape> Mouth;  // smoothed as the menu's Mouth smoothing 1 (VoiceMouth.h)
    // The same shapes smoothed as up to 2026-11-13 (Mouth smoothing 0) and softer (2); each as many as Mouth, or
    // empty (then MouthAt keeps to Mouth)
    std::vector<voice_face::Shape> MouthBefore, MouthSofter;
    std::vector<float> Overview;           // the jaw's opening over the whole file, OverviewPoints of them, 0 - 1
                                           // (at Mouth smoothing 1: OverviewOf makes it at another)
    std::string Report;                    // what was read, one line (the log and the menu)
    std::string Warning;                   // empty, or what may be wrong with it
    std::string MouthNotes;                // what the mouth's analysis measured in it, one line (the log)
    // The life around the mouth (VoiceLife.cpp: where the eyes blink, the brows rise and the head moves, from the
    // sound's loudness and pitch). Empty: it couldn't be worked out, the face stays as posed
    double LifeRate = 100.0;               // shapes a second
    std::vector<voice_face::LifeShape> Life;   // (every value a number within the analysis' own limits)
    std::string LifeNotes;                 // what that analysis measured, one line (the log)
    std::string LifeFound;                 // the menu's line: what it found to do ("8 blinks, 4 brow raises, ...")
    std::string LifeNote;                  // empty, or what to know about it (no speech found: blinks only; sound under the voice)
    // [ALIVE] What a living face is worked out of for each posed character (VoiceAlive::Make: its eyes, blinks,
    // brows, breathing and head, a take of its own; about 1.5 ms for 30 s of sound): the analysis' own track and, of
    // its working data, only what VoiceAlive and VoiceFeel read (the phrases, syllables and events, the loudness
    // column). Empty frames: it couldn't be worked out.
    voice_life::Track LifeTrack;
    voice_life::Debug LifeDebug;
    // [ALIVE] The voice's energy for the feeling (VoiceFeel::Energy: 0 .. 1 at 100 a second; empty: none)
    std::vector<float> Energy;
  };

  // Reads and decodes a sound file and works out its mouth shapes (any thread of the tools' own; a few hundred ms a
  // minute of sound). Null, and why, if it can't be used.
  std::shared_ptr<Sound const> Load(std::wstring const& path, std::string& error);

  // The mouth's shape at a time into the sound (between two of its shapes: mixed); before its start and after its
  // end: all zero, the mouth at rest. smoothing: the menu's Mouth smoothing, 0 (as up to 2026-11-13) to 2 (softer);
  // 1 is Mouth alone, and between two of the three smoothings they are mixed.
  voice_face::Shape MouthAt(Sound const& sound, double seconds, float smoothing = 1.f);

  // The menu's picture at a Mouth smoothing (as MouthAt chooses): the most the jaw opens in each of OverviewPoints
  // slices of the file, 0 - 1 (all zero for a sound with no mouth shapes)
  void OverviewOf(Sound const& sound, float smoothing, std::vector<float>& out);

  // The same for the life around the mouth: the blink, the brow raise and the head's nod, turn and tilt at a time
  // into the sound; before its start and after its end: all zero, the face as posed
  voice_face::LifeShape LifeAt(Sound const& sound, double seconds);
}
