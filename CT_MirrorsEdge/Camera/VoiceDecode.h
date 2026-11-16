#pragma once

// A sound file read into memory as plain 16-bit samples by Windows' own Media
// Foundation (its Source Reader): the voice files of the pose editor.
//
// What is said here about kinds of files and about times was measured on one
// Windows 11 PC only (2026-11); another Windows may hold other decoders.
//
// Kinds of files read on that PC: WAV in its layouts (8 to 32 bits, floating
// point, more than two channels, ADPCM, A-law, mu-law), MP3, MP2, AAC in M4A
// and MP4 and bare (ADTS, .aac), ALAC, FLAC, WMA, AC-3, Opus in WebM, Vorbis
// in MKA, and the sound track of a video. Ogg files (Vorbis, Opus) too, but
// those are read by Windows' Web Media Extensions (a Microsoft Store package
// that was installed there), so only where that is installed; no PC without
// it was tried. AIFF: not read.
//
// Media Foundation's DLLs are loaded the first time Decode runs, by name from
// System32 only, so a tool holding this module starts on a Windows without
// Media Foundation too (the N editions without the Media Feature Pack; none
// was tried): Decode then fails with a plain text.
//
// Decode blocks until the whole file is read: a 10-minute M4A took about 1.5
// to 2 s (1.4 to 1.8 s in the runs made). A damaged file whose decoder stops
// answering fails 15 s after its last block of samples (15.0 s in all on the
// damaged files tried), and from then on Windows' decoder keeps that file
// open and one processor core busy until the program ends. So call Decode on
// a worker thread, never on the game's threads and never from DllMain. It
// may run on several threads at once. It needs no message loop.
//
// Limits: a sound is kept up to maxSeconds (DefaultMaxSeconds when the caller
// gives none; a day at most) and up to 128 * 1024 * 1024 samples (256 MiB of
// memory), whichever comes first. A longer file is cut there, and Note says
// so.
//
// The sound starts where the file's own sound starts: the silence an MP3 or
// AAC encoder puts before it (and Windows' decoders leave in) is dropped when
// the file says how long it is, so times in the sound match the file as an
// editor shows it. Where the file doesn't say, that silence stays in: the
// sound then came late by 1024 samples for bare ADTS AAC and for AAC in MKA
// or in a fragmented MP4, 577 for an MP3 without a tag frame, 288 for AC-3
// and 481 for MP2 (measured).

#include <string>
#include <vector>

namespace voice_decode
{
  // The longest sound kept when the caller gives no limit of its own
  const double DefaultMaxSeconds = 600.0;

  struct Sound
  {
    int Rate = 0;                // samples per second, the file's own
    int Channels = 0;            // 1 or 2 (more than 2 in the file are mixed down to 2)
    std::vector<short> Pcm;      // interleaved 16-bit samples, Channels per moment
    double Seconds = 0.0;        // the length of Pcm
    std::string Note;            // what was done to the sound, in plain words (empty: nothing)

    // More about the file, for logs and tests
    std::string FileKind;        // what the file holds: "MP3", "AAC", "PCM", "FLAC" ...
    int FileChannels = 0;        // the channels in the file
    int FileBits = 0;            // the bits per sample the decoder handed over
    bool FileFloat = false;      // the decoder handed over floating point samples
    double FileSeconds = 0.0;    // the length the file itself claims (0: it doesn't say)
    int LeadIn = 0;              // moments of encoder lead-in dropped at the start
    int TailOut = 0;             // moments of encoder padding dropped at the end
    double FirstStampSeconds = 0.0;  // the time stamp of the first block of samples
    double WorstStampSeconds = 0.0;  // the largest gap between a block's time stamp and the samples read so far
    int Blocks = 0;              // the blocks of samples read
    int Gaps = 0;                // the gaps the reader announced (stream ticks)
    bool Cut = false;            // stopped at a limit (maxSeconds or the sample limit) with more left in the file
    long Result = 0;             // the failing HRESULT (0 when Decode returned true)
  };

  // Reads the file at path into out. maxSeconds: the longest sound to keep
  // (0 or less, or not a number: DefaultMaxSeconds; a day at most); a longer
  // file is cut there, or at the sample limit when that comes first, and Note
  // says so. Returns false with a plain text in error when the file is
  // missing, empty, not a sound, copy-protected or too damaged to give any
  // sound; nothing is thrown.
  bool Decode(std::wstring const& path, double maxSeconds, Sound& out, std::string& error);

  // What to ask the reader for; Decode uses AskPcm16 and falls back by itself
  enum Ask
  {
    AskPcm16 = 0,   // 16-bit PCM, then any PCM, then floating point
    AskPcm = 1,     // any PCM, then floating point
    AskFloat = 2,   // floating point, then any PCM
  };

  // Decode with the first request chosen by the caller, and the encoder's
  // lead-in and padding kept when dropEdges is false (for tests: every sample
  // layout the module converts can be reached this way)
  bool DecodeAs(std::wstring const& path, double maxSeconds, Ask ask, bool dropEdges, Sound& out, std::string& error);

  // Keeps Media Foundation started between decodes: call it once when the
  // voice feature is first used (Decode starts and stops Media Foundation by
  // itself when nobody holds it, which costs DLL loads and, for some kinds of
  // files, handles on every decode). Every Hold that returned true needs one
  // Unhold, at the tools' own orderly end; an Unhold with no Hold open does
  // nothing. Neither from DllMain.
  bool Hold(std::string& error);
  void Unhold();
}
