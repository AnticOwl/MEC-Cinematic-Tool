#include "VoiceFeel.h"
#include <algorithm>
#include <cmath>
#include <utility>

namespace voice_feel
{
  using namespace voice_face;

  // The presets at strength 1. Which units: EMFACS (Friesen and Ekman 1983, as the FACS literature lists it) and
  // the prototypical units of Du, Tao and Martinez 2014 (230 people, posed); Calm, Tender, Worried and Tense are
  // designed. How much of each unit: designed [d] (a clear but not extreme face; 1 is the game's own pose at its
  // extreme). Order of the units: 1 2 4 5 6 7 9 10 12 14 15 16 17 20 23 24 41.
  const Preset Presets[FeelingCount] = {
    //            1     2     4     5     6     7     9     10    12    14    15    16    17    20    23    24    41
    { "Calm",   { 0.f,  0.f,  0.f,  0.f,  0.f,  .2f,  0.f,  0.f,  .05f, 0.f,  0.f,  0.f,  0.f,  0.f,  0.f,  0.f,  .15f }, {}, 0.f, 0.f, 1.f },
    { "Happy",  { 0.f,  0.f,  0.f,  0.f,  .5f,  0.f,  0.f,  0.f,  .6f,  0.f,  0.f,  0.f,  0.f,  0.f,  0.f,  0.f,  0.f }, {}, -0.01f, 0.f, 1.f },
    { "Tender", { .15f, 0.f,  0.f,  0.f,  .2f,  0.f,  0.f,  0.f,  .3f,  0.f,  0.f,  0.f,  0.f,  0.f,  0.f,  0.f,  .2f }, {}, 0.f, 0.035f, 1.f },
    { "Sad",    { .6f,  0.f,  .35f, 0.f,  0.f,  0.f,  0.f,  0.f,  0.f,  0.f,  .5f,  0.f,  .25f, 0.f,  0.f,  0.f,  .25f }, {}, 0.05f, 0.f, 0.7f },
    { "Worried",{ .5f,  0.f,  .4f,  .1f,  0.f,  0.f,  0.f,  0.f,  0.f,  0.f,  0.f,  0.f,  0.f,  .2f,  0.f,  .15f, 0.f }, {}, 0.f, 0.f, 0.8f },
    { "Afraid", { .7f,  .4f,  .35f, .6f,  0.f,  .2f,  0.f,  0.f,  0.f,  0.f,  0.f,  0.f,  0.f,  .5f,  0.f,  0.f,  0.f }, {}, -0.02f, 0.f, 1.f,
                0.2f },    // [JAW] the lips parted: the jaw about 1.8 degrees down, the lower lip about 3 mm
    { "Surprised",{ .7f, .7f, 0.f,  .4f,  0.f,  0.f,  0.f,  0.f,  0.f,  0.f,  0.f,  0.f,  0.f,  0.f,  0.f,  0.f,  0.f }, {}, -0.02f, 0.f, 1.f,
                0.35f },   // [JAW] the jaw dropped: about 3.2 degrees, the lower lip about 5 mm
    { "Angry",  { 0.f,  0.f,  .8f,  .25f, 0.f,  .5f,  0.f,  .1f,  0.f,  0.f,  0.f,  0.f,  .2f,  0.f,  .3f,  .4f,  0.f }, {}, 0.f, 0.f, 0.3f },
    { "Tense",  { 0.f,  0.f,  .3f,  0.f,  0.f,  .3f,  0.f,  0.f,  0.f,  0.f,  0.f,  0.f,  0.f,  .1f,  .2f,  .35f, 0.f }, {}, 0.f, 0.f, 0.6f },
    { "Disgusted",{ 0.f, 0.f, .2f,  0.f,  0.f,  0.f,  .6f,  .4f,  0.f,  0.f,  .3f,  0.f,  .3f,  0.f,  0.f,  0.f,  0.f }, {}, 0.f, 0.f, 0.6f },
    { "Contempt",{ 0.f, 0.f,  0.f,  0.f,  0.f,  0.f,  0.f,  0.f,  0.f,  0.f,  0.f,  0.f,  0.f,  0.f,  0.f,  0.f,  0.f },
                 { 0.f, 0.f,  0.f,  0.f,  0.f,  0.f,  0.f,  0.f,  .25f, .35f, 0.f,  0.f,  0.f,  0.f,  0.f,  0.f,  0.f }, -0.015f, 0.f, 1.f },
  };

  namespace
  {
    const double FeelFramesPerSecond = 100.0;
    float Unit(float v) { return !(v > 0.f) ? 0.f : (v > 1.f ? 1.f : v); }
    double Finite(double v) { return std::isfinite(v) ? v : 0.0; }
  }

  bool Energy(voice_life::Debug const& debug, size_t frames, std::vector<float>& energy)
  {
    energy.clear();
    if (frames == 0)
      return false;
    energy.assign(frames, 0.f);
    const double pi = 3.14159265358979;
    // The phrases' floor
    size_t phrases = debug.PhraseFrom.size() < debug.PhraseTo.size() ? debug.PhraseFrom.size() : debug.PhraseTo.size();
    for (size_t k = 0; k < frames; ++k)
    {
      double t = (double)k / FeelFramesPerSecond;
      double best = 0.0;
      for (size_t p = 0; p < phrases; ++p)
      {
        double a = Finite(debug.PhraseFrom[p]), b = Finite(debug.PhraseTo[p]);
        if (b < a)
          b = a;
        double v = 0.0;
        if (t >= a && t <= b)
          v = PhraseFloor;
        else if (t < a && t >= a - PhraseRise)
          v = PhraseFloor * (t - (a - PhraseRise)) / PhraseRise;
        else if (t > b)
          v = PhraseFloor * exp(-(t - b) / PhraseFall);
        if (v > best)
          best = v;
      }
      energy[k] = (float)best;
    }
    // The syllables: up to their height on their centre, held to their end, fading after
    for (voice_life::Syllable const& s : debug.Syllables)
    {
      double speech = Unit(s.Speech);
      double stress = std::isfinite(s.Stress) ? (double)s.Stress : 0.0;
      double h = StressBase + StressGain * stress;
      h = h < SyllableLeast ? SyllableLeast : (h > 1.0 ? 1.0 : h);
      h *= speech;
      if (!(h > 0.0))
        continue;
      double c = Finite(s.Seconds), e = Finite(s.To);
      if (e < c)
        e = c;
      double from = c - SyllableRise, to = e + 6.0 * SyllableFall;
      long long k0 = (long long)floor(from * FeelFramesPerSecond), k1 = (long long)ceil(to * FeelFramesPerSecond);
      if (k0 < 0)
        k0 = 0;
      if (k1 >= (long long)frames)
        k1 = (long long)frames - 1;
      for (long long k = k0; k <= k1; ++k)
      {
        double t = (double)k / FeelFramesPerSecond, v;
        if (t < c)
          v = t < from ? 0.0 : h * 0.5 * (1.0 - cos(pi * (t - from) / SyllableRise));
        else if (t <= e)
          v = h;
        else
          v = h * exp(-(t - e) / SyllableFall);
        if (v > energy[(size_t)k])
          energy[(size_t)k] = (float)v;
      }
    }
    for (float& v : energy)
      v = Unit(v);
    return true;
  }

  float EnergyAt(std::vector<float> const& energy, double seconds)
  {
    if (energy.empty() || !std::isfinite(seconds) || seconds < 0.0)
      return 0.f;
    double at = seconds * FeelFramesPerSecond;
    size_t k = (size_t)at;
    if (k >= energy.size() - 1)
      return k == energy.size() - 1 ? energy[k] : 0.f;
    float t = (float)(at - (double)k);
    return energy[k] + (energy[k + 1] - energy[k]) * t;
  }

  bool SmileTimesOf(voice_life::Debug const& debug, SmileTimes& out)
  {
    out = SmileTimes();
    std::vector<std::pair<double, double>> phrases;
    size_t n = debug.PhraseFrom.size() < debug.PhraseTo.size() ? debug.PhraseFrom.size() : debug.PhraseTo.size();
    for (size_t p = 0; p < n; ++p)
    {
      double a = debug.PhraseFrom[p], b = debug.PhraseTo[p];
      if (!std::isfinite(a) || !std::isfinite(b))
        continue;
      phrases.push_back(std::make_pair(a, b < a ? a : b));
    }
    std::sort(phrases.begin(), phrases.end());
    for (size_t p = 0; p < phrases.size(); ++p)
    {
      out.Starts.push_back(phrases[p].first);
      if (!out.From.empty() && phrases[p].first - out.To.back() < SmileMerge)
      {
        if (phrases[p].second > out.To.back())
          out.To.back() = phrases[p].second;
        continue;
      }
      if (!out.From.empty())
        out.EndStart.push_back(p);
      out.From.push_back(phrases[p].first);
      out.To.push_back(phrases[p].second);
      out.FirstStart.push_back(p);
    }
    if (!out.From.empty())
      out.EndStart.push_back(phrases.size());
    return !out.From.empty();
  }

  namespace
  {
    double Ease(double x) { x = !(x > 0.0) ? 0.0 : (x > 1.0 ? 1.0 : x); return x * x * (3.0 - 2.0 * x); }

    // One stretch of smiling's level at a time (0 outside it: before its rise, after its fall)
    double SmileLevelIn(SmileTimes const& times, size_t e, double t)
    {
      double from = times.From[e], to = times.To[e];
      size_t k = times.FirstStart[e], end = times.EndStart[e];
      double riseAt = from - SmileLead;
      if (!(t > riseAt) || !(t < to + SmileOffset))
        return 0.0;
      // The rises, each from the level it was at, to the fullest; held there at most SmileApexMost; eased to
      // SmileHold over SmileRelax; a phrase that begins once it is there rises again (so it is at its fullest at most
      // SmileApexMost at a time, with SmileRelax of easing between). Worked out up to min(t, to); after the speech,
      // the fall from where it was at its end.
      double until = t < to ? t : to, level = 0.0, start = 0.0;
      ++k;   // (the first phrase's rise is the stretch's own)
      for (;;)
      {
        double onset = SmileOnset * (1.0 - start);
        double apexFrom = riseAt + onset, apexEnd = apexFrom + SmileApexMost;
        while (k < end && times.Starts[k] - SmileLead < apexEnd + SmileRelax)
          ++k;
        double next = k < end ? times.Starts[k] - SmileLead : 1e300;
        auto at = [&](double x)
        {
          if (x < apexFrom)
            return onset > 0.0 ? start + (1.0 - start) * Ease((x - riseAt) / onset) : 1.0;
          if (x < apexEnd)
            return 1.0;
          return 1.0 - (1.0 - SmileHold) * Ease((x - apexEnd) / SmileRelax);
        };
        if (until < next)
        {
          level = at(until);
          break;
        }
        start = at(next);
        riseAt = next;
        ++k;
      }
      if (t <= to)
        return level;
      return level * (1.0 - Ease((t - to) / SmileOffset));
    }

    double SmileLevelOnce(SmileTimes const& times, double t)
    {
      if (times.From.empty() || !std::isfinite(t))
        return 0.0;
      // The stretch whose rise has begun last, and the one before it (its fall may still be going)
      size_t lo = 0, hi = times.From.size();
      while (lo < hi)
      {
        size_t mid = (lo + hi) / 2;
        if (times.From[mid] - SmileLead < t)
          lo = mid + 1;
        else
          hi = mid;
      }
      double best = 0.0;
      for (size_t e = lo > 2 ? lo - 2 : 0; e < lo + 1 && e < times.From.size(); ++e)
      {
        double v = SmileLevelIn(times, e, t);
        if (v > best)
          best = v;
      }
      return best;
    }
  }

  float SmileLevelAt(SmileTimes const& times, double seconds, double loopSeconds)
  {
    if (times.From.empty() || !std::isfinite(seconds))
      return 0.f;
    double v = SmileLevelOnce(times, seconds);
    if (loopSeconds > 0.0 && std::isfinite(loopSeconds))
    {
      double t = fmod(seconds, loopSeconds);
      if (t < 0.0)
        t += loopSeconds;
      v = SmileLevelOnce(times, t);
      double before = SmileLevelOnce(times, t - loopSeconds), after = SmileLevelOnce(times, t + loopSeconds);
      v = v > before ? v : before;
      v = v > after ? v : after;
    }
    return (float)(v < 0.0 ? 0.0 : (v > 1.0 ? 1.0 : v));
  }

  float SmileFactor(SmileTimes const& times, double seconds, double loopSeconds, float comes)
  {
    float c = !(comes > 0.f) ? 0.f : (comes > 1.f ? 1.f : comes);
    if (times.From.empty() || !(c > 0.f))
      return 1.f;
    float level = SmileLevelAt(times, seconds, loopSeconds);
    return 1.f - c * (1.f - SmileRest) * (1.f - level);
  }

  FeelShape ShapeAt(Settings const& settings, float energy, Shape const& mouth, float closure)
  {
    int feel = settings.Feel >= 0 && settings.Feel < FeelingCount ? settings.Feel : Calm;
    return ShapeOf(Presets[feel], settings, energy, mouth, closure);
  }

  float JawClosure(float closed, float lipClosing)
  {
    float c = !(lipClosing > 0.f) ? 0.f : (lipClosing > 2.f ? 2.f : lipClosing);
    return c > 0.f ? Unit(Unit(closed) * (c > 1.f ? c : 1.f)) : 0.f;
  }

  FeelShape ShapeOf(Preset const& preset, Settings const& settings, float energy, Shape const& mouth, float closure,
    float smile, float jawShut)
  {
    FeelShape shape;
    float strength = !(settings.Strength > 0.f) ? 0.f : (settings.Strength > 2.f ? 2.f : settings.Strength);
    float follow = Unit(settings.FollowVoice);
    float e = Unit(energy);
    float a = !(settings.Asymmetry > 0.f) ? 0.f : (settings.Asymmetry > 0.3f ? 0.3f : settings.Asymmetry);
    float leftSide = settings.LeftStronger ? 1.f + a : 1.f - a, rightSide = settings.LeftStronger ? 1.f - a : 1.f + a;
    // The speech's needs: the lips pressed or tightened only where the mouth isn't speaking, the lips never opened
    // where they close for p, b, m
    float talk = Unit(2.f * fmaxf(fmaxf(Unit(mouth.Open), Unit(mouth.Wide)), fmaxf(Unit(mouth.Round), Unit(mouth.Teeth))));
    float smileShare = !(smile >= 0.f) ? 1.f : (smile > 1.f ? 1.f : smile);
    float closed = Unit(Unit(mouth.Closed) * (!(closure > 0.f) ? 0.f : (closure > 2.f ? 2.f : closure)));
    for (int u = 0; u < AuCount; ++u)
    {
      float part = FollowMouth;
      if (u == Au1 || u == Au2 || u == Au4)
        part = FollowBrows;
      else if (u == Au5 || u == Au6 || u == Au7 || u == Au9 || u == Au41)
        part = FollowEyes;
      float k = strength * (1.f - FeelDip * part * follow * (1.f - e));
      float gate = 1.f;
      if (u == Au23 || u == Au24)
        gate = 1.f - talk;
      else if (u == Au10 || u == Au16)
        gate = 1.f - closed;
      else if (u == Au17)
        gate = 1.f - 0.5f * talk;
      else if (u == Au14 || u == Au20)
        gate = 1.f - 0.5f * Unit(mouth.Round);
      float both = preset.Au[u] * k * gate;
      // [SMILE] The smile's own share now (1: as before, the same arithmetic)
      if (u == Au6 || u == Au12)
        both *= smileShare;
      shape.Left[u] = both * leftSide;
      shape.Right[u] = (both + preset.RightOnly[u] * k * gate) * rightSide;
    }
    shape.Nod = preset.Nod * strength;
    shape.Tilt = preset.Tilt * strength;
    // [JAW] The feeling's jaw, eased in the pauses as the mouth's units are, and gone where the lips close ([REVIEW]:
    // the mouth shuts the jaw only by (1 - Closed x Lip closing), and Closed is how sure the analysis is of a p, b or
    // m, not how hard they press; the speech's own Open is already pushed down there, the feeling's wasn't: on the five
    // voices tried, 25 of 38 closures stayed apart with Surprised, 17 of them by over 1 mm, up to 3.4 mm)
    float shut = jawShut >= 0.f ? Unit(jawShut) : JawClosure(mouth.Closed, closure);
    shape.Jaw = Unit(preset.Jaw * strength * (1.f - FeelDip * FollowMouth * follow * (1.f - e))) *
      (1.f - Unit(shut / FeelJawShutAt));
    // [REVIEW 5] The life's brow raises held back by the feeling's own share only as far as the feeling shows: none
    // at strength 0, the preset's share from strength 1 on (before: the preset's share at any strength, so Angry at
    // strength 0 still cut the voice's raises to 30 %: the review)
    float keep = !(preset.RaiseKeep > 0.f) ? 0.f : (preset.RaiseKeep > 1.f ? 1.f : preset.RaiseKeep);
    shape.RaiseKeep = 1.f - (1.f - keep) * (strength < 1.f ? strength : 1.f);
    return shape;
  }
}
