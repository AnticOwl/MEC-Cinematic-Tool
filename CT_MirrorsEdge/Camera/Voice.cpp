#include "Voice.h"
#include "VoiceDecode.h"
#include "VoiceLife.h"
#include "VoiceFeel.h"
#include "VoiceMouth.h"
#include <Windows.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace voice
{
  namespace
  {
    std::string Utf8Of(std::wstring const& w)
    {
      int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
      std::string s(n > 0 ? n : 0, '\0');
      if (n > 0)
        WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
      return s;
    }

    float Unit(float v)
    {
      return !(v > 0.f) ? 0.f : (v > 1.f ? 1.f : v);
    }

    float Mix(float a, float b, float t)
    {
      return a + (b - a) * t;
    }

    voice_face::Shape MixShape(voice_face::Shape const& a, voice_face::Shape const& b, float t)
    {
      voice_face::Shape shape;
      shape.Open = Mix(a.Open, b.Open, t);
      shape.Wide = Mix(a.Wide, b.Wide, t);
      shape.Round = Mix(a.Round, b.Round, t);
      shape.Closed = Mix(a.Closed, b.Closed, t);
      shape.Teeth = Mix(a.Teeth, b.Teeth, t);
      return shape;
    }

    // Which of the sound's three smoothings the menu's Mouth smoothing shows, or which two mixed and how much of the
    // second (1, or anything that isn't a number: Mouth alone; 0 or less: MouthBefore alone; 2 or more: MouthSofter
    // alone; a set that isn't as long as Mouth is left out). MouthAt and OverviewOf both choose by this.
    struct Smoothings
    {
      std::vector<voice_face::Shape> const* First;
      std::vector<voice_face::Shape> const* Second;   // null: First alone
      float Share;

      voice_face::Shape At(size_t k) const
      {
        return Second ? MixShape((*First)[k], (*Second)[k], Share) : (*First)[k];
      }
    };

    Smoothings SmoothingsOf(Sound const& sound, float smoothing)
    {
      Smoothings s = { &sound.Mouth, nullptr, 0.f };
      size_t count = sound.Mouth.size();
      bool before = sound.MouthBefore.size() == count, softer = sound.MouthSofter.size() == count;
      if (smoothing <= 0.f && before)
        s.First = &sound.MouthBefore;
      else if (smoothing > 0.f && smoothing < 1.f && before)
      {
        s.First = &sound.MouthBefore;
        s.Second = &sound.Mouth;
        s.Share = smoothing;
      }
      else if (smoothing >= 2.f && softer)
        s.First = &sound.MouthSofter;
      else if (smoothing > 1.f && smoothing < 2.f && softer)
      {
        s.Second = &sound.MouthSofter;
        s.Share = smoothing - 1.f;
      }
      return s;
    }

    // An analysis' frames as shapes (0 for anything that isn't a number)
    void ShapesOf(std::vector<voice_mouth::Frame> const& frames, std::vector<voice_face::Shape>& shapes)
    {
      shapes.resize(frames.size());
      for (size_t i = 0; i < frames.size(); ++i)
      {
        voice_mouth::Frame const& f = frames[i];
        voice_face::Shape& s = shapes[i];
        s.Open = Unit(f.Open);
        s.Wide = Unit(f.Wide);
        s.Round = Unit(f.Round);
        s.Closed = Unit(f.Closed);
        s.Teeth = Unit(f.Teeth);
      }
    }

    // A value kept within from .. to (0 for anything that isn't a number)
    float Within(float v, float from, float to)
    {
      return !(v == v) ? 0.f : (v < from ? from : (v > to ? to : v));
    }

    // A line's value in an analysis' report (lines of "name: value"); empty: no such line
    std::string ReportValue(std::string const& report, char const* name)
    {
      std::string key = std::string(name) + ": ";
      size_t at = 0;
      while (at < report.size())
      {
        size_t end = report.find('\n', at);
        if (end == std::string::npos)
          end = report.size();
        if (report.compare(at, key.size(), key) == 0)
        {
          std::string value = report.substr(at + key.size(), end - at - key.size());
          while (!value.empty() && (value.back() == '\r' || value.back() == ' '))
            value.pop_back();
          return value;
        }
        at = end + 1;
      }
      return std::string();
    }

    // A report's lines as one line
    std::string OneLine(std::string const& report)
    {
      std::string line;
      for (char c : report)
      {
        if (c == '\r')
          continue;
        if (c == '\n')
          line += "; ";
        else
          line += c;
      }
      while (line.size() >= 2 && line.compare(line.size() - 2, 2, "; ") == 0)
        line.resize(line.size() - 2);
      return line;
    }

    // The sound's first 2 ms faded in from nothing and its last 5 ms out to nothing. The player fades its own stops
    // and starts, but a sound that plays to its end is cut as its samples are (measured offline), and samples that
    // don't end at zero would click.
    void FadeEdges(std::vector<short>& pcm, int rate, int channels)
    {
      size_t moments = pcm.size() / (size_t)channels;
      size_t in = (size_t)(rate * 0.002), out = (size_t)(rate * 0.005);
      if (!in || !out || moments < (in + out) * 2)
        return;
      for (size_t m = 0; m < in; ++m)
        for (int c = 0; c < channels; ++c)
        {
          short& s = pcm[m * (size_t)channels + (size_t)c];
          s = (short)lrintf((float)s * (float)m / (float)in);
        }
      for (size_t m = 0; m < out; ++m)
        for (int c = 0; c < channels; ++c)
        {
          short& s = pcm[(moments - 1 - m) * (size_t)channels + (size_t)c];
          s = (short)lrintf((float)s * (float)m / (float)out);
        }
    }
  }

  std::shared_ptr<Sound const> Load(std::wstring const& path, std::string& error)
  {
    error.clear();
    voice_decode::Sound decoded;
    if (!voice_decode::Decode(path, MaxSeconds, decoded, error))
    {
      if (error.empty())
        error = "couldn't be read";
      return nullptr;
    }
    if (decoded.Rate <= 0 || (decoded.Channels != 1 && decoded.Channels != 2) || decoded.Pcm.empty() ||
        decoded.Pcm.size() % (size_t)decoded.Channels != 0)
    {
      error = "was decoded into a sound this tool can't use";
      return nullptr;
    }
    if (decoded.Rate > 200000)
    {
      error = "has more samples a second than the tools can play (200,000 at most): convert it";
      return nullptr;
    }

    auto sound = std::make_shared<Sound>();
    size_t slash = path.find_last_of(L"\\/");
    sound->File = Utf8Of(slash == std::wstring::npos ? path : path.substr(slash + 1));
    sound->Rate = decoded.Rate;
    sound->Channels = decoded.Channels;
    size_t moments = decoded.Pcm.size() / (size_t)decoded.Channels;
    sound->Seconds = (double)moments / (double)decoded.Rate;
    FadeEdges(decoded.Pcm, decoded.Rate, decoded.Channels);

    // The mouth's shapes from the sound (the same samples the player plays, so mouth and sound share one time).
    // Without them the sound still plays. (The analyser turns running out of memory into false itself; anything
    // else it might throw is caught by the caller's thread.)
    voice_mouth::Track track;
    std::string mouthError;
    bool mouth = false;
    if (decoded.Rate >= 8000 && decoded.Rate <= 192000)
      mouth = voice_mouth::Analyse(decoded.Pcm.data(), moments, decoded.Rate, decoded.Channels, track, mouthError) &&
        track.FramesPerSecond > 0.0 && !track.Frames.empty();
    else
      mouthError = "its sample rate is outside 8,000 - 192,000 a second";
    size_t moving = 0;
    if (mouth)
    {
      sound->MouthRate = track.FramesPerSecond;
      ShapesOf(track.Frames, sound->Mouth);
      // (the other two smoothings only if there are as many: MouthAt then keeps to Mouth)
      if (track.Before.size() == track.Frames.size())
        ShapesOf(track.Before, sound->MouthBefore);
      if (track.Softer.size() == track.Frames.size())
        ShapesOf(track.Softer, sound->MouthSofter);
      for (voice_face::Shape const& s : sound->Mouth)
        if (s.Open > 0.1f || s.Wide > 0.1f || s.Round > 0.1f || s.Closed > 0.1f || s.Teeth > 0.1f)
          ++moving;
    }

    char text[400];
    // The life around the mouth from the same samples: where the eyes blink, the brows rise and the head moves
    // (VoiceLife.cpp; it turns running out of memory into false itself). Without it the face stays as posed.
    // Every value is kept inside the analysis' own stated limits, so nothing else can reach a bone.
    if (decoded.Rate >= 8000 && decoded.Rate <= 192000)
    {
      voice_life::Track lifeTrack;
      voice_life::Debug lifeDebug;   // [ALIVE] (its working data: phrases, syllables, events, columns)
      std::string lifeError;
      if (voice_life::Analyse(decoded.Pcm.data(), moments, decoded.Rate, decoded.Channels, lifeTrack, lifeError, &lifeDebug) &&
          lifeTrack.FramesPerSecond > 0.0 && !lifeTrack.Frames.empty())
      {
        sound->LifeRate = lifeTrack.FramesPerSecond;
        sound->Life.resize(lifeTrack.Frames.size());
        for (size_t i = 0; i < lifeTrack.Frames.size(); ++i)
        {
          voice_life::Frame const& f = lifeTrack.Frames[i];
          voice_face::LifeShape& s = sound->Life[i];
          s.Blink = Unit(f.Blink);
          s.Brow = Unit(f.Brow);
          s.Nod = Within(f.Nod, -voice_life::MaxNodUp, voice_life::MaxNodDown);
          s.Turn = Within(f.Turn, -voice_life::MaxTurn, voice_life::MaxTurn);
          s.Tilt = Within(f.Tilt, -voice_life::MaxTilt, voice_life::MaxTilt);
        }
        sound->LifeNotes = OneLine(lifeTrack.Report);
        // The menu's line: what was found to do (the counts are the analysis' own: its clear ones)
        std::string const& report = lifeTrack.Report;
        std::string blinks = ReportValue(report, "blinks"), none = ReportValue(report, "speech");
        std::string brows = ReportValue(report, "brow raises"), nods = ReportValue(report, "nods");
        std::string dips = ReportValue(report, "phrase end dips"), poses = ReportValue(report, "poses");
        // (one of a kind in the singular: the report writes its counts as whole numbers, "1")
        auto counted = [](std::string const& count, char const* one, char const* more) { return count + (count == "1" ? one : more); };
        // Poses are counted by the analysis wherever one was asked for, also where the turn is then faded to
        // nothing (measured by the review: a drone counted 13 with 0.14 degrees at most, half-second lines 1 with
        // none): named only when the head really turns or tilts half a degree or more somewhere
        float headMost = 0.f;
        for (voice_face::LifeShape const& l : sound->Life)
          headMost = fmaxf(headMost, fmaxf(fabsf(l.Turn), fabsf(l.Tilt)));
        sound->LifeFound = counted(blinks.empty() ? std::string("0") : blinks, " blink", " blinks");
        if (!brows.empty() && !nods.empty())
        {
          sound->LifeFound += ", " + counted(brows, " brow raise", " brow raises") + ", " +
            counted(nods, " nod on a stressed syllable", " nods on stressed syllables");
          if (!dips.empty())
            sound->LifeFound += ", " + counted(dips, " dip of the chin at a phrase end", " dips of the chin at phrase ends");
          if (!poses.empty() && headMost >= 0.0087f)
            sound->LifeFound += ", " + counted(poses, " small turn of the head", " small turns of the head");
        }
        // ("speech: none (why)": only blinks were made)
        if (none.compare(0, 4, "none") == 0)
          // [REVIEW 9] (the living face then still gives the eyes, blinks, breathing and the head's slow drift)
          sound->LifeNote = "no speech was found in this sound" + (none.size() > 5 ? " " + none.substr(5) : std::string()) +
            ": no brow raises or nods; the eyes, blinks, breathing and the head's slow drift go on";
        else
        {
          // How far the sound's quiet parts lie under the voice. Measured offline (2026-11-12): 58 - 60 dB on clean
          // voices, 14 - 34 dB with noise, music or a hum 10 - 20 dB under the voice, where brow raises and nods
          // landed in other places than on the clean voice; but also 25 - 34 dB on clean lines cut with no silence
          // or pause in them, where they landed right (the review's measure): the sound can't tell the two apart.
          std::string floor = ReportValue(report, "floor dB below speech");
          double under = floor.empty() ? 0.0 : atof(floor.c_str());
          if (!floor.empty() && under > 0.0 && under < 35.0)
          {
            _snprintf_s(text, sizeof(text), _TRUNCATE, "this sound's quiet parts are only %.0f dB under the voice: if that is "
              "noise, music or a hum under it, brow raises and nods may land in wrong places (a clean line with no silence "
              "or pause in it reads the same, and is fine)", under);
            sound->LifeNote = text;
          }
        }
        // [ALIVE] Kept for the living face (VoiceAlive) and the feeling's energy (VoiceFeel): the track, and of the
        // working data only what those two read (the other columns are 4 bytes a frame each, unused)
        sound->LifeDebug.PhraseFrom.swap(lifeDebug.PhraseFrom);
        sound->LifeDebug.PhraseTo.swap(lifeDebug.PhraseTo);
        sound->LifeDebug.Syllables.swap(lifeDebug.Syllables);
        sound->LifeDebug.Events.swap(lifeDebug.Events);
        for (size_t c = 0; c < lifeDebug.Names.size() && c < lifeDebug.Columns.size(); ++c)
          if (lifeDebug.Names[c] == "loudness")
          {
            sound->LifeDebug.Names.push_back(lifeDebug.Names[c]);
            sound->LifeDebug.Columns.push_back(std::move(lifeDebug.Columns[c]));
            break;
          }
        if (!voice_feel::Energy(sound->LifeDebug, lifeTrack.Frames.size(), sound->Energy))
          sound->Energy.clear();
        sound->LifeTrack = std::move(lifeTrack);
      }
      else
        sound->LifeNote = "blinks, brow raises and head movement couldn't be worked out" +
          (lifeError.empty() ? std::string() : " (" + lifeError + ")");
    }
    else
      sound->LifeNote = "blinks, brow raises and head movement couldn't be worked out (its sample rate is outside "
        "8,000 - 192,000 a second)";

    // The menu's picture: the most the jaw opens in each slice of the file (at Mouth smoothing 1; the menu makes its
    // own at another setting)
    if (mouth)
      OverviewOf(*sound, 1.f, sound->Overview);
    else
      sound->Overview.assign(OverviewPoints, 0.f);

    sound->Pcm = std::make_shared<const std::vector<short>>(std::move(decoded.Pcm));

    int minutes = (int)(sound->Seconds / 60.0);
    double rest = sound->Seconds - minutes * 60.0;
    if (minutes > 0 && rest >= 59.95)   // (it would be written "60.0 s")
    {
      ++minutes;
      rest = 0.0;
    }
    if (minutes > 0)
      _snprintf_s(text, sizeof(text), _TRUNCATE, "%s: %d min %.1f s", sound->File.c_str(), minutes, rest);
    else
      _snprintf_s(text, sizeof(text), _TRUNCATE, "%s: %.2f s", sound->File.c_str(), sound->Seconds);
    sound->Report = text;
    // (the kind as Windows' reader names it; "PCM" says nothing: a WAV, or a kind its add-on decoders hand over so)
    if (!decoded.FileKind.empty() && decoded.FileKind != "PCM")
      sound->Report += ", " + decoded.FileKind;
    _snprintf_s(text, sizeof(text), _TRUNCATE, ", %d samples a second, %s", sound->Rate, sound->Channels == 2 ? "stereo" : "mono");
    sound->Report += text;
    if (!decoded.Note.empty())
      sound->Report += " (" + decoded.Note + ")";
    if (mouth)
    {
      double share = 100.0 * (double)moving / (double)sound->Mouth.size();
      _snprintf_s(text, sizeof(text), _TRUNCATE, "; the mouth moves in %.0f %% of it", share);
      sound->Report += text;
      // What the analyser measured (its lines, as one line for the log)
      for (char c : track.Report)
      {
        if (c == '\r')
          continue;
        if (c == '\n')
          sound->MouthNotes += "; ";
        else
          sound->MouthNotes += c;
      }
      while (sound->MouthNotes.size() >= 2 && sound->MouthNotes.compare(sound->MouthNotes.size() - 2, 2, "; ") == 0)
        sound->MouthNotes.resize(sound->MouthNotes.size() - 2);
      if (moving * 50 < sound->Mouth.size())
        sound->Warning = "hardly any speech was found in this sound (silence, or a voice too deep in noise or music): the "
          "mouth will hardly move";
    }
    else
      sound->Warning = "the mouth's movements couldn't be worked out" + (mouthError.empty() ? std::string() : " (" + mouthError + ")") +
        ": the mouth stays still";
    if (decoded.Cut)
      sound->Warning += std::string(sound->Warning.empty() ? "" : "; ") + "the file is longer than the tools take: its end is left out";
    return sound;
  }

  voice_face::Shape MouthAt(Sound const& sound, double seconds, float smoothing)
  {
    voice_face::Shape shape;
    size_t count = sound.Mouth.size();
    if (!count || !(sound.MouthRate > 0.0) || !std::isfinite(seconds) || seconds < 0.0 || seconds >= sound.Seconds)
      return shape;
    // Shape i is the mouth at i / MouthRate seconds; after the last one: at rest
    double at = seconds * sound.MouthRate;
    if (!(at >= 0.0) || at >= (double)count)
      return shape;
    Smoothings shown = SmoothingsOf(sound, smoothing);
    size_t i = (size_t)at;
    voice_face::Shape a = shown.At(i);
    if (i + 1 >= count)
      return a;
    voice_face::Shape b = shown.At(i + 1);
    return MixShape(a, b, (float)(at - (double)i));
  }

  void OverviewOf(Sound const& sound, float smoothing, std::vector<float>& out)
  {
    out.assign(OverviewPoints, 0.f);
    if (!(sound.MouthRate > 0.0) || !(sound.Seconds > 0.0))
      return;
    Smoothings shown = SmoothingsOf(sound, smoothing);
    for (size_t i = 0; i < sound.Mouth.size(); ++i)
    {
      double share = ((double)i / sound.MouthRate) / sound.Seconds;
      if (!(share >= 0.0) || share >= 1.0)
        continue;
      float& point = out[(size_t)(share * OverviewPoints)];
      float open = shown.At(i).Open;
      if (open > point)
        point = open;
    }
  }

  voice_face::LifeShape LifeAt(Sound const& sound, double seconds)
  {
    voice_face::LifeShape shape;
    size_t count = sound.Life.size();
    if (!count || !(sound.LifeRate > 0.0) || !std::isfinite(seconds) || seconds < 0.0 || seconds >= sound.Seconds)
      return shape;
    // Shape i is the face at i / LifeRate seconds; after the last one: as posed (and nothing for a product that isn't
    // a number: a rate of infinity at 0 s)
    double at = seconds * sound.LifeRate;
    if (!(at >= 0.0) || at >= (double)count)
      return shape;
    size_t i = (size_t)at;
    voice_face::LifeShape const& a = sound.Life[i];
    if (i + 1 >= count)
      return a;
    voice_face::LifeShape const& b = sound.Life[i + 1];
    float t = (float)(at - (double)i);
    shape.Blink = Mix(a.Blink, b.Blink, t);
    shape.Brow = Mix(a.Brow, b.Brow, t);
    shape.Nod = Mix(a.Nod, b.Nod, t);
    shape.Turn = Mix(a.Turn, b.Turn, t);
    shape.Tilt = Mix(a.Tilt, b.Tilt, t);
    return shape;
  }
}
