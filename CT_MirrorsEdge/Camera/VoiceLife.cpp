// See voice_life.h. The steps, in order:
//   1. Mix to mono and resample to 8 kHz (windowed sinc), high-pass at 70 Hz.
//   2. Every 10 ms: the level of a 25 ms stretch (once of everything, once
//      of what lies above 300 Hz: the loudness), and pitch candidates from a
//      4 kHz copy (the peaks of the normalised cross-correlation of two 30 ms
//      stretches one pitch period apart, 60 - 400 Hz).
//   3. The pitch track: the cheapest path through the candidates (a jump of
//      pitch costs, going voiced or unvoiced costs), short runs dropped,
//      short gaps filled, short runs an octave too high put back, smoothed.
//   4. The recording's own speech level and floor (as voice_mouth has them);
//      every level after that is relative to them.
//   5. Syllables: the peaks of the smoothed loudness that are voiced and
//      stand 2 dB clear of their neighbours. Phrases: speech with no pause
//      of 250 ms in it.
//   6. A stress score for each syllable from its level, pitch and length
//      against its neighbours. A syllable whose pitch stands still is a
//      held note, not speech, and gets nothing.
//   7. The curves: brow raises and nods on the strongest stresses, the
//      head's drift and pose from the phrases, the blinks by rule.
// Every number that shapes a curve is a constant below, with its unit.
#include "VoiceLife.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <new>

namespace voice_life
{
  namespace
  {
    const double kPi = 3.14159265358979323846;
    const int kRate = 8000;          // the rate everything is worked at
    const int kHop = 80;             // 10 ms
    const int kWindow = 200;         // 25 ms: one level
    const int kZeroCrossings = 8;    // the resampler's filter, a side

    // Pitch, on the 4 kHz copy
    const int kPitchHop = 40;        // 10 ms
    const int kSpan = 120;           // 30 ms: the two stretches compared
    const int kMinLag = 10;          // 400 Hz
    const int kMaxLag = 67;          // 59.7 Hz
    const int kFirstLag = 2;         // the lags worked out (the short ones show the dip before a true peak, and
    const int kLastLag = 75;         // eight to each side are needed for the values between two whole lags)
    const int kHalfTaps = 8;
    const int kPad = 100;            // zeros before and after the 4 kHz copy
    const int kCandidates = 5;
    const float kCandidateFrom = 0.30f;   // a peak this high is a candidate
    const float kValleyShare = 0.40f;     // the correlation must have dipped under this share of the peak before it
    const float kOctaveWeight = 0.07f;    // of two equal peaks the shorter lag wins: 7 % of the peak for each octave
    const float kUnvoicedBias = -0.06f;   // voiced from a correlation of about 0.56
    const float kVoicingStep = 0.12f;     // the cost of going voiced or unvoiced
    const float kPitchStep = 2.5f;        // the cost of a pitch change, for each octave
    const float kPitchStepMost = 0.45f;   // and the most a jump can cost

    // Speech, syllables, phrases (frames are 10 ms)
    const float kSteadyFloor = -6.0f;     // a floor this near the speech level: a steady sound, no speech
    const float kGateAboveFloor = 4.0f;
    const float kGateLowest = -50.0f;
    const float kSyllableDip = 2.0f;      // dB between two syllables
    const float kSyllableOverGate = 2.0f; // a syllable's peak stands this far over the gate (6 dB over the floor)
    const float kSyllableLowest = -30.0f; // and never lower than this against the speech level
    const float kStrongFrame = -15.0f;    // a voiced frame this near the speech level counts for the voice's pitch
                                          // and for the held-note test (soft music in the pauses does not)
    const int kSyllableReach = 40;        // a syllable ends 400 ms from its centre at the latest
    const int kPause = 25;                // 250 ms without speech ends a phrase
    const int kLongPause = 100;           // 1 s: the head goes back to rest
    const float kFlatStep = 0.012f;       // semitones a frame: under this the pitch stands still (1.2 semitones a second)
    // [check 2] What a voice has and beats, a drone or a whisper have not. Measured: over 350,000 frames of
    // speech (clean, fast, slow, noisy, clipped, telephone; one human voice) the voiced share of the speaking
    // time of the 3 s around a frame was never under 0.26, and no file made fewer than 1.24 syllables a second
    // of speech; a made-up drum loop reached 0.14, a made-up engine 0.53 syllables a second. Of the voice
    // frames of those 3 s, the share with a correlation of 0.8 or more was never under 0.32 in ordinary speech
    // (0.21 in synthetic speech at the synthesiser's fastest pace); noise through a mouth's resonances (a
    // whisper-like noise voice) reached 0.31 at most and mostly under 0.2, twelve made-up voices at once 0.11.
    const float kVoicedShareFrom = 0.15f; // under this voiced share of the speaking time: beats or bursts, not a voice
    const float kVoicedShareFull = 0.25f; // from here on: a voice
    const float kSyllableRateFrom = 0.5f; // under this many syllables a second of speech: a drone or a howl, not a voice
    const float kSyllableRateFull = 0.9f; // from here on: a voice
    const float kSureCorrelation = 0.8f;  // a voice frame's pitch is sure from this correlation
    const float kSureShareFrom = 0.20f;   // under this share of sure ones among the voice frames: a whisper, a murmur
    const float kSureShareFull = 0.35f;   // from here on: a voice

    // The stress score's scales
    const float kLevelScale = 3.0f;       // dB for one unit
    const float kPitchScale = 2.0f;       // semitones over the running pitch for one unit, or the voice's own
                                          // pitch spread if that is larger
    const float kRiseScale = 2.5f;        // semitones over the syllable before
    const float kLengthScale = 0.6f;      // octaves of length (1.5 times as long: one unit)
    const float kFinalLength = 0.8f;      // taken off the last syllable of a phrase (it is long by itself)
    const float kStressFrom = 0.5f;       // a syllable counts as stressed from here
    const float kEventLevel = -10.0f;     // a syllable this far under the speech level gets no brow raise and no nod
    const float kNearLevel = 4.0f;        // the neighbours' level is taken within 4 dB of the whole file's
    const float kNearLength = 1.33f;      // and their length within a third of the whole file's

    // Brow (frames)
    const float kBrowFrom = 0.8f;         // the brow score a raise starts at (with no height yet)
    const float kBrowFull = 1.8f;         // and the score of a full raise
    const float kBrowClear = 0.25f;       // a raise this high counts as one in the report
    const int kBrowGap = 200;             // 2 s between two raises
    const int kBrowLead = 16;             // starts 160 ms before the syllable's centre
    const int kBrowRise = 18;             // up in 180 ms
    const int kBrowHoldLeast = 12;        // held 120 to 300 ms
    const int kBrowHoldMost = 30;
    const int kBrowFall = 50;             // down in 500 ms

    // Nod (frames, degrees)
    const float kNodFrom = 0.6f;          // the nod score a nod starts at (with no size yet)
    const float kNodFull = 1.8f;          // and the score of the largest nod
    const float kNodClear = 0.7f;         // a nod this large (degrees) counts as one in the report
    const float kNodStrong = 2.0f;        // a nod this large draws a blink, and a new pose inside a long phrase
    const int kNodGap = 70;               // 0.7 s between two nods
    const int kNodDown = 18;              // down in 180 ms, lowest at the syllable's centre
    const int kNodHold = 3;
    const int kNodUp = 42;                // back in 420 ms
    const float kNodMost = 3.5f;
    const int kEndDown = 30;              // the dip at a phrase's end: down in 300 ms, lowest where the speech ends
    const int kEndHold = 8;
    const int kEndUp = 60;
    const float kEndDip = 1.5f;
    // [check 1] The dip grows from nothing with the pause after the phrase and with the phrase's own length
    const float kEndPauseFrom = 25.0f;    // no dip yet at 250 ms of pause (the shortest pause that ends a phrase)
    const float kEndPauseFull = 45.0f;    // the whole dip from 450 ms
    const float kEndPhraseFrom = 60.0f;   // no dip for a phrase of 0.6 s or less
    const float kEndPhraseFull = 120.0f;  // the whole dip from 1.2 s
    const float kEndClear = 0.3f;         // a dip this large (degrees) counts as one in the report
    const float kDriftMost = 1.2f;        // the slow drift, up or down
    const int kDriftSmooth = 101;         // 1 s
    const int kDriftLead = 10;            // the drift's line starts 100 ms before a phrase
    const int kDriftTail = 50;            // and ends 500 ms after it

    // Turn and tilt (frames, degrees)
    const int kPoseLead = 0;              // a new pose is asked for where its phrase starts: with the smoothing the
                                          // head sets out 0.6 s before the phrase and is there 0.6 s into it
    const int kPoseRest = 50;             // rest is asked for 500 ms into a long pause
    const int kPoseGap = 300;             // 3 s between two poses
    const int kPoseGapInside = 350;       // 3.5 s before a new pose inside a phrase
    const int kPoseSmooth = 121;          // 1.2 s
    const float kTurnMost = 2.2f;         // a pose's turn: anywhere up to this, to either side
    const float kTurnStepLeast = 0.8f;    // and 0.8 to 3 degrees from the pose before
    const float kTurnStepMost = 3.0f;
    const float kTiltMost = 1.4f;
    const float kTiltStepLeast = 0.4f;

    // Blink (frames)
    const int kBlinkClose = 8;            // shut in 80 ms
    const int kBlinkHold = 3;
    const int kBlinkOpen = 17;            // open in 170 ms
    const int kBlinkFrames = kBlinkClose + kBlinkHold + kBlinkOpen;
    const int kBlinkLeast = 200;          // 2 to 6 s from one blink to the next
    const int kBlinkMost = 600;
    const int kBlinkSecond = 42;          // the second of two starts 420 ms after the first
    const double kBlinkTwo = 0.12;        // the share of blinks that come as two
    const int kBlinkFirstFrom = 40;       // the first blink: 0.4 to 4.4 s into the sound

    // The ends of the sound: everything but the blinks fades in and out
    const int kFadeIn = 20;
    const int kFadeOut = 40;

    const float kDegree = 0.01745329252f;

    float Ramp(float x, float a, float b)
    {
      float t = (x - a) / (b - a);
      return t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
    }

    float Clamp(float x, float a, float b)
    {
      return x < a ? a : (x > b ? b : x);
    }

    // 0 to 1 with no corner at either end
    float Ease(float t)
    {
      if (t <= 0.0f)
      {
        return 0.0f;
      }
      if (t >= 1.0f)
      {
        return 1.0f;
      }
      return static_cast<float>(0.5 - 0.5 * cos(kPi * t));
    }

    int Gcd(int a, int b)
    {
      while (b != 0)
      {
        int t = a % b;
        a = b;
        b = t;
      }
      return a;
    }

    // Mixes the channels and resamples to 8 kHz. The filter is a sinc with a
    // Hann window, 8 zero crossings a side, one table row per phase.
    // rightSign is 1, or -1 to turn the right channel over before the mix.
    void ToInternalRate(short const* pcm, size_t samples, int rate, int channels, int rightSign,
      std::vector<float>& out)
    {
      const float scale = 1.0f / (32768.0f * static_cast<float>(channels));
      if (rate == kRate)
      {
        out.resize(samples);
        for (size_t i = 0; i < samples; i++)
        {
          int sum = pcm[i * channels];
          if (channels == 2)
          {
            sum += rightSign * pcm[i * channels + 1];
          }
          out[i] = static_cast<float>(sum) * scale;
        }
        return;
      }

      int g = Gcd(rate, kRate);
      long long up = kRate / g;
      long long down = rate / g;
      bool exact = up <= 640;
      int phases = exact ? static_cast<int>(up) : 512;
      double cutoff = 0.47 * (std::min)(1.0, static_cast<double>(kRate) / rate);
      int half = static_cast<int>(ceil(kZeroCrossings / (2.0 * cutoff)));
      int taps = 2 * half + 1;
      std::vector<float> kernel(static_cast<size_t>(phases) * taps);
      std::vector<double> row(taps);
      for (int p = 0; p < phases; p++)
      {
        double frac = static_cast<double>(p) / phases;
        double sum = 0.0;
        for (int j = 0; j < taps; j++)
        {
          double d = static_cast<double>(j - half) - frac;
          double x = d / (half + 1.0);
          double w = (x <= -1.0 || x >= 1.0) ? 0.0 : 0.5 + 0.5 * cos(kPi * x);
          double a = 2.0 * kPi * cutoff * d;
          double s = fabs(a) < 1e-9 ? 1.0 : sin(a) / a;
          row[j] = s * w;
          sum += row[j];
        }
        for (int j = 0; j < taps; j++)
        {
          kernel[static_cast<size_t>(p) * taps + j] = static_cast<float>(row[j] / sum) * scale;
        }
      }

      size_t count = static_cast<size_t>(static_cast<unsigned long long>(samples) * kRate / rate);
      out.resize(count);
      double step = static_cast<double>(rate) / kRate;
      for (size_t n = 0; n < count; n++)
      {
        long long centre = 0;
        int phase = 0;
        if (exact)
        {
          long long num = static_cast<long long>(n) * down;
          centre = num / up;
          phase = static_cast<int>(num % up);
        }
        else
        {
          double pos = static_cast<double>(n) * step;
          centre = static_cast<long long>(floor(pos));
          phase = static_cast<int>((pos - static_cast<double>(centre)) * phases + 0.5);
          if (phase >= phases)
          {
            phase = 0;
            centre++;
          }
        }
        long long first = centre - half;
        int a = first < 0 ? static_cast<int>(-first) : 0;
        long long room = static_cast<long long>(samples) - first;
        int b = room < taps ? static_cast<int>(room) : taps;
        float const* k = &kernel[static_cast<size_t>(phase) * taps];
        float s0 = 0.0f;
        float s1 = 0.0f;
        if (b > a)
        {
          float const* h = k + a;
          int length = b - a;
          int j = 0;
          if (channels == 1)
          {
            short const* x = pcm + (first + a);
            for (; j + 2 <= length; j += 2)
            {
              s0 += h[j] * static_cast<float>(x[j]);
              s1 += h[j + 1] * static_cast<float>(x[j + 1]);
            }
            for (; j < length; j++)
            {
              s0 += h[j] * static_cast<float>(x[j]);
            }
          }
          else
          {
            short const* x = pcm + (first + a) * 2;
            for (; j + 2 <= length; j += 2)
            {
              s0 += h[j] * static_cast<float>(x[j * 2] + rightSign * x[j * 2 + 1]);
              s1 += h[j + 1] * static_cast<float>(x[j * 2 + 2] + rightSign * x[j * 2 + 3]);
            }
            for (; j < length; j++)
            {
              s0 += h[j] * static_cast<float>(x[j * 2] + rightSign * x[j * 2 + 1]);
            }
          }
        }
        float value = s0 + s1;
        if (a > 0 || b < taps)
        {
          // At the two ends part of the filter hangs over the edge: scale by
          // the part that is inside, so a DC offset stays level to the end.
          float inside = 0.0f;
          for (int j = a; j < b; j++)
          {
            inside += k[j];
          }
          if (inside > 0.25f * scale)
          {
            value *= scale / inside;
          }
        }
        out[n] = value;
      }
    }

    // Second order Butterworth high-pass. It starts as if the first sample's
    // value had been there for ever, so a DC offset makes no click at the
    // start.
    void HighPass(std::vector<float>& x, double hz)
    {
      double w0 = 2.0 * kPi * hz / kRate;
      double alpha = sin(w0) / (2.0 * 0.70710678118654752);
      double a0 = 1.0 + alpha;
      double b0 = (1.0 + cos(w0)) / 2.0 / a0;
      double b1 = -(1.0 + cos(w0)) / a0;
      double b2 = b0;
      double a1 = -2.0 * cos(w0) / a0;
      double a2 = (1.0 - alpha) / a0;
      double x1 = x.empty() ? 0.0 : x[0];
      double x2 = x1;
      double y1 = 0.0;
      double y2 = 0.0;
      for (size_t i = 0; i < x.size(); i++)
      {
        double v = x[i];
        double y = b0 * v + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
        // (a filter left ringing down for ever would slow down on the tiniest numbers)
        if (fabs(y) < 1e-20)
        {
          y = 0.0;
        }
        x2 = x1;
        x1 = v;
        y2 = y1;
        y1 = y;
        x[i] = static_cast<float>(y);
      }
    }

    // The level of the 25 ms around each frame's centre, in dB (a full-scale
    // sine reads -3, nothing reads -120).
    void FrameLevels(std::vector<float> const& x, size_t count, std::vector<float>& out)
    {
      float window[kWindow];
      double weight = 0.0;
      for (int i = 0; i < kWindow; i++)
      {
        window[i] = static_cast<float>(0.5 - 0.5 * cos(2.0 * kPi * (i + 0.5) / kWindow));
        weight += window[i];
      }
      out.assign(count, -120.0f);
      const long long size = static_cast<long long>(x.size());
      for (size_t k = 0; k < count; k++)
      {
        long long first = static_cast<long long>(k) * kHop - kWindow / 2;
        int a = first < 0 ? static_cast<int>(-first) : 0;
        long long room = size - first;
        int b = room < kWindow ? static_cast<int>(room) : kWindow;
        float s0 = 0.0f;
        float s1 = 0.0f;
        if (b > a)
        {
          float const* p = &x[static_cast<size_t>(first + a)];
          float const* w = window + a;
          int length = b - a;
          int i = 0;
          for (; i + 2 <= length; i += 2)
          {
            s0 += w[i] * p[i] * p[i];
            s1 += w[i + 1] * p[i + 1] * p[i + 1];
          }
          for (; i < length; i++)
          {
            s0 += w[i] * p[i] * p[i];
          }
        }
        out[k] = static_cast<float>(10.0 * log10(static_cast<double>(s0 + s1) / weight + 1e-12));
      }
    }

    // A 4 kHz copy (low-pass: half at 1.2 kHz, next to nothing from 1.7 kHz,
    // every second sample) for the pitch, with kPad zeros before and after
    // it. The low harmonics are enough for the pitch, and without the high
    // ones the correlation's peaks are wide enough to be read between two
    // lags.
    void Decimate2(std::vector<float> const& x, std::vector<float>& out)
    {
      const int half = 16;
      float taps[2 * half + 1];
      double sum = 0.0;
      double row[2 * half + 1];
      for (int j = -half; j <= half; j++)
      {
        double a = 2.0 * kPi * 0.15 * j;
        double s = j == 0 ? 1.0 : sin(a) / a;
        double w = 0.5 + 0.5 * cos(kPi * j / (half + 1.0));
        row[j + half] = s * w;
        sum += row[j + half];
      }
      for (int j = 0; j <= 2 * half; j++)
      {
        taps[j] = static_cast<float>(row[j] / sum);
      }
      size_t count = x.size() / 2;
      out.assign(count + 2 * static_cast<size_t>(kPad), 0.0f);
      long long size = static_cast<long long>(x.size());
      for (size_t m = 0; m < count; m++)
      {
        long long first = static_cast<long long>(m) * 2 - half;
        int a = first < 0 ? static_cast<int>(-first) : 0;
        long long room = size - first;
        int b = room < 2 * half + 1 ? static_cast<int>(room) : 2 * half + 1;
        float s = 0.0f;
        for (int j = a; j < b; j++)
        {
          s += taps[j] * x[static_cast<size_t>(first + j)];
        }
        out[m + kPad] = s;
      }
    }

    struct PitchFrame
    {
      float Lag[kCandidates];   // in 4 kHz samples, between two whole lags
      float Value[kCandidates]; // the correlation there
      float Rank[kCandidates];  // the correlation less 7 % for each octave of lag over the shortest: highest first
      float Best = 0.0f;        // the highest correlation of any peak of the frame
      int Count = 0;
    };

    const double kLog2 = 1.4426950408889634;   // 1 / ln 2

    // The pitch candidates of every frame: the peaks of the normalised
    // cross-correlation of two 30 ms stretches a lag apart, placed evenly
    // around the frame's centre. The correlation is worked out at whole
    // lags and filled in half-way between them (a windowed sinc over 16
    // lags): at 4 kHz the peak of a high voice is narrower than one lag, and
    // read at whole lags only it comes out lower than its own copy an octave
    // down. A peak counts only if the correlation has dipped well under it
    // at some shorter lag: a sound that repeats has such a dip, a rumble or
    // a hiss whose correlation just falls away has not.
    void FindCandidates(std::vector<float> const& x4, size_t count, std::vector<float> const& level, float skipBelow,
      std::vector<PitchFrame>& frames)
    {
      frames.assign(count, PitchFrame());
      float between[kHalfTaps];
      {
        double sum = 0.0;
        double row[kHalfTaps];
        for (int m = 0; m < kHalfTaps; m++)
        {
          double x = m + 0.5;
          row[m] = sin(kPi * x) / (kPi * x) * (0.5 + 0.5 * cos(kPi * x / kHalfTaps));
          sum += 2.0 * row[m];
        }
        for (int m = 0; m < kHalfTaps; m++)
        {
          between[m] = static_cast<float>(row[m] / sum);
        }
      }
      // q holds the correlation every half lag from kMinLag - 0.5 to
      // kMaxLag + 0.5: q[j] is at lag kMinLag - 0.5 + j / 2.
      const int halves = 2 * (kMaxLag - kMinLag) + 3;
      float r[kLastLag + 1];
      float q[2 * (kMaxLag - kMinLag) + 3];
      double sq[kLastLag + kSpan + 1];
      for (size_t k = 0; k < count; k++)
      {
        if (level[k] < skipBelow)
        {
          continue;
        }
        float const* p = &x4[k * kPitchHop + (kPad - kSpan / 2 - kLastLag / 2)];
        sq[0] = 0.0;
        for (int i = 0; i < kLastLag + kSpan; i++)
        {
          sq[i + 1] = sq[i] + static_cast<double>(p[i]) * p[i];
        }
        for (int lag = kFirstLag; lag <= kLastLag; lag++)
        {
          int a0 = kLastLag / 2 - lag / 2;
          int b0 = a0 + lag;
          float const* a = p + a0;
          float const* b = p + b0;
          float s0 = 0.0f;
          float s1 = 0.0f;
          float s2 = 0.0f;
          float s3 = 0.0f;
          for (int i = 0; i < kSpan; i += 4)
          {
            s0 += a[i] * b[i];
            s1 += a[i + 1] * b[i + 1];
            s2 += a[i + 2] * b[i + 2];
            s3 += a[i + 3] * b[i + 3];
          }
          double aa = sq[a0 + kSpan] - sq[a0];
          double bb = sq[b0 + kSpan] - sq[b0];
          double n = static_cast<double>((s0 + s1) + (s2 + s3)) / sqrt(aa * bb + 1e-18);
          r[lag] = static_cast<float>(n > 1.0 ? 1.0 : (n < -1.0 ? -1.0 : n));
        }
        float valley = 1.0f;
        for (int lag = kFirstLag; lag < kMinLag; lag++)
        {
          valley = (std::min)(valley, r[lag]);
        }
        for (int j = 0; j < halves; j++)
        {
          int lag = kMinLag - 1 + (j + 1) / 2;
          if (j & 1)
          {
            q[j] = r[lag];
          }
          else
          {
            float v = 0.0f;
            for (int m = 0; m < kHalfTaps; m++)
            {
              v += between[m] * (r[lag - m] + r[lag + 1 + m]);
            }
            q[j] = v;
          }
        }
        PitchFrame& f = frames[k];
        for (int j = 1; j + 1 < halves; j++)
        {
          if (q[j] > q[j - 1] && q[j] >= q[j + 1] && q[j] >= kCandidateFrom)
          {
            // The top of the parabola through the three points
            float bend = q[j - 1] - 2.0f * q[j] + q[j + 1];
            float shift = bend < -1e-9f ? 0.5f * (q[j - 1] - q[j + 1]) / bend : 0.0f;
            shift = Clamp(shift, -0.5f, 0.5f);
            float value = q[j] - 0.25f * (q[j - 1] - q[j + 1]) * shift;
            value = value > 1.0f ? 1.0f : value;
            if (valley <= kValleyShare * value)
            {
              // A sound that repeats every T also repeats every 2 T, 3 T
              // and so on, with peaks as high: the five kept are the best
              // by rank, which takes 7 % off for each octave of lag, so the
              // shortest of equal peaks is never the one left out.
              float lag = static_cast<float>(kMinLag) - 0.5f + 0.5f * (static_cast<float>(j) + shift);
              float octaves = static_cast<float>(log(static_cast<double>(lag) / kMinLag) * kLog2);
              float rank = value * (1.0f - kOctaveWeight * octaves);
              f.Best = (std::max)(f.Best, value);
              int at = f.Count < kCandidates ? f.Count : kCandidates - 1;
              if (f.Count < kCandidates || rank > f.Rank[at])
              {
                while (at > 0 && f.Rank[at - 1] < rank)
                {
                  f.Lag[at] = f.Lag[at - 1];
                  f.Value[at] = f.Value[at - 1];
                  f.Rank[at] = f.Rank[at - 1];
                  at--;
                }
                f.Lag[at] = lag;
                f.Value[at] = value;
                f.Rank[at] = rank;
                if (f.Count < kCandidates)
                {
                  f.Count++;
                }
              }
            }
          }
          if (q[j] < valley)
          {
            valley = q[j];
          }
        }
      }
    }

    // The pitch track: one candidate or "unvoiced" for every frame, so that
    // the sum of the frames' own costs and the costs of the steps between
    // them is the least. Gives the pitch in semitones over 100 Hz and
    // whether the frame is voiced, and the correlation of the candidate
    // taken.
    void TrackPitch(std::vector<PitchFrame> const& frames, std::vector<float>& pitch,
      std::vector<unsigned char>& voiced, std::vector<float>& strength)
    {
      const size_t count = frames.size();
      const int states = kCandidates + 1;
      pitch.assign(count, 0.0f);
      voiced.assign(count, 0);
      strength.assign(count, 0.0f);
      if (count == 0)
      {
        return;
      }
      std::vector<unsigned char> back(count * states, 0);
      float before[kCandidates + 1];
      float beforeLog[kCandidates + 1];
      int beforeCount = 0;
      before[0] = 0.0f;
      beforeLog[0] = 0.0f;
      for (size_t k = 0; k < count; k++)
      {
        PitchFrame const& f = frames[k];
        float now[kCandidates + 1];
        float nowLog[kCandidates + 1];
        float best = f.Best;
        nowLog[0] = 0.0f;
        for (int j = 0; j <= f.Count; j++)
        {
          float own = 0.0f;
          if (j == 0)
          {
            own = best + kUnvoicedBias;
          }
          else
          {
            float lag = f.Lag[j - 1];
            nowLog[j] = static_cast<float>(log(static_cast<double>(lag)) * kLog2);
            own = 1.0f - f.Rank[j - 1];
          }
          float least = 1e30f;
          int from = 0;
          if (k == 0)
          {
            least = 0.0f;
          }
          else
          {
            for (int i = 0; i <= beforeCount; i++)
            {
              float step = 0.0f;
              if ((i == 0) != (j == 0))
              {
                step = kVoicingStep;
              }
              else if (i != 0)
              {
                step = (std::min)(kPitchStep * static_cast<float>(fabs(nowLog[j] - beforeLog[i])), kPitchStepMost);
              }
              float total = before[i] + step;
              if (total < least)
              {
                least = total;
                from = i;
              }
            }
          }
          now[j] = least + own;
          back[k * states + j] = static_cast<unsigned char>(from);
        }
        float lowest = now[0];
        for (int j = 1; j <= f.Count; j++)
        {
          lowest = (std::min)(lowest, now[j]);
        }
        for (int j = 0; j <= f.Count; j++)
        {
          before[j] = now[j] - lowest;
          beforeLog[j] = nowLog[j];
        }
        beforeCount = f.Count;
      }
      int state = 0;
      for (int j = 1; j <= beforeCount; j++)
      {
        if (before[j] < before[state])
        {
          state = j;
        }
      }
      for (size_t k = count; k-- > 0;)
      {
        if (state > 0)
        {
          float lag = frames[k].Lag[state - 1];
          voiced[k] = 1;
          pitch[k] = static_cast<float>(12.0 * log(40.0 / lag) * kLog2);
          strength[k] = frames[k].Value[state - 1];
        }
        state = back[k * states + state];
      }
    }

    float MedianOf(std::vector<float>& values)
    {
      if (values.empty())
      {
        return 0.0f;
      }
      size_t middle = values.size() / 2;
      std::nth_element(values.begin(), values.begin() + middle, values.end());
      return values[middle];
    }

    float Percentile(std::vector<float>& values, double share)
    {
      if (values.empty())
      {
        return 0.0f;
      }
      size_t index = static_cast<size_t>(share * static_cast<double>(values.size() - 1) + 0.5);
      std::nth_element(values.begin(), values.begin() + index, values.end());
      return values[index];
    }

    // Tidies the pitch track: voiced runs under 30 ms go, gaps of up to
    // 20 ms inside a voiced stretch are filled, single odd frames are taken
    // out (median of five), a short run a whole octave above everything
    // around it is put back down, and the track is smoothed a little.
    void TidyPitch(std::vector<float>& pitch, std::vector<unsigned char>& voiced, int& repairs, Debug* debug)
    {
      const long long count = static_cast<long long>(pitch.size());
      repairs = 0;
      // Fill short gaps
      for (long long k = 1; k < count;)
      {
        if (voiced[k] || !voiced[k - 1])
        {
          k++;
          continue;
        }
        long long end = k;
        while (end < count && !voiced[end])
        {
          end++;
        }
        if (end < count && end - k <= 2 && fabs(pitch[end] - pitch[k - 1]) < 2.0f)
        {
          for (long long j = k; j < end; j++)
          {
            float t = static_cast<float>(j - k + 1) / static_cast<float>(end - k + 1);
            pitch[j] = pitch[k - 1] + (pitch[end] - pitch[k - 1]) * t;
            voiced[j] = 1;
          }
        }
        k = end;
      }
      // Drop short runs; median of five inside the others
      std::vector<float> copy;
      struct Run
      {
        long long First;
        long long End;
        float Median;
      };
      std::vector<Run> runs;
      std::vector<float> pool;
      for (long long k = 0; k < count;)
      {
        if (!voiced[k])
        {
          k++;
          continue;
        }
        long long end = k;
        while (end < count && voiced[end])
        {
          end++;
        }
        if (end - k < 3)
        {
          for (long long j = k; j < end; j++)
          {
            voiced[j] = 0;
            pitch[j] = 0.0f;
          }
        }
        else
        {
          copy.assign(pitch.begin() + k, pitch.begin() + end);
          for (long long j = k; j < end; j++)
          {
            long long a = (std::max)(k, j - 2);
            long long b = (std::min)(end - 1, j + 2);
            float five[5];
            int n = 0;
            for (long long i = a; i <= b; i++)
            {
              five[n++] = copy[static_cast<size_t>(i - k)];
            }
            std::sort(five, five + n);
            pitch[j] = five[n / 2];
          }
          pool.assign(pitch.begin() + k, pitch.begin() + end);
          Run run = { k, end, MedianOf(pool) };
          runs.push_back(run);
        }
        k = end;
      }
      // A run of up to 250 ms that sits an octave (0.8 to 1.2) above the
      // median of the 1.5 s of voice to each side of it is moved down an
      // octave. On the synthetic voices these were blips of 30 - 40 ms at
      // a consonant, read at twice the pitch around them. A real leap of an
      // octave that short loses its stress this way: weak, not wrong. A run
      // an octave BELOW its surroundings is left alone: that is what a
      // creaky phrase end is.
      std::vector<float> moves(runs.size(), 0.0f);
      for (size_t r = 0; r < runs.size(); r++)
      {
        Run const& run = runs[r];
        if (run.End - run.First > 25)
        {
          continue;
        }
        pool.clear();
        long long a = (std::max)(0LL, run.First - 150);
        long long b = (std::min)(count, run.End + 150);
        for (long long j = a; j < b; j++)
        {
          if (voiced[j] && (j < run.First || j >= run.End))
          {
            pool.push_back(pitch[j]);
          }
        }
        if (pool.size() < 20)
        {
          continue;
        }
        float away = run.Median - MedianOf(pool);
        if (away >= 9.6f && away <= 14.4f)
        {
          moves[r] = -12.0f;
        }
      }
      for (size_t r = 0; r < runs.size(); r++)
      {
        if (moves[r] != 0.0f)
        {
          repairs++;
          if (debug != nullptr)
          {
            Event e;
            e.Seconds = static_cast<double>(runs[r].First) / 100.0;
            e.Kind = "octave";
            e.Size = moves[r];
            e.Score = static_cast<float>(runs[r].End - runs[r].First) / 100.0f;
            e.Stress = static_cast<float>(100.0 * pow(2.0, runs[r].Median / 12.0));
            debug->Events.push_back(e);
          }
          for (long long j = runs[r].First; j < runs[r].End; j++)
          {
            pitch[j] += moves[r];
          }
        }
      }
      // Smooth inside each run (50 ms)
      const float w[5] = { 0.25f, 0.75f, 1.0f, 0.75f, 0.25f };
      for (Run const& run : runs)
      {
        copy.assign(pitch.begin() + run.First, pitch.begin() + run.End);
        for (long long j = run.First; j < run.End; j++)
        {
          float sum = 0.0f;
          float weight = 0.0f;
          for (int i = -2; i <= 2; i++)
          {
            long long at = j + i;
            if (at >= run.First && at < run.End)
            {
              sum += w[i + 2] * copy[static_cast<size_t>(at - run.First)];
              weight += w[i + 2];
            }
          }
          pitch[j] = sum / weight;
        }
      }
    }

    // Marks the voiced frames whose pitch stands still: the middle one of
    // the frame-to-frame steps over the 120 ms around it is under 1.2
    // semitones a second. A speaking voice's pitch is always on the move; a
    // held note's is not.
    void FindFlat(std::vector<float> const& pitch, std::vector<unsigned char> const& voiced,
      std::vector<unsigned char>& flat)
    {
      const long long count = static_cast<long long>(pitch.size());
      flat.assign(pitch.size(), 0);
      for (long long k = 0; k < count;)
      {
        if (!voiced[k])
        {
          k++;
          continue;
        }
        long long end = k;
        while (end < count && voiced[end])
        {
          end++;
        }
        for (long long j = k; j < end; j++)
        {
          long long a = (std::max)(k, j - 6);
          long long b = (std::min)(end - 1, j + 6);
          float steps[12];
          int n = 0;
          for (long long i = a; i < b; i++)
          {
            steps[n++] = static_cast<float>(fabs(pitch[i + 1] - pitch[i]));
          }
          if (n >= 6)
          {
            std::sort(steps, steps + n);
            flat[j] = steps[n / 2] < kFlatStep ? 1 : 0;
          }
        }
        k = end;
      }
    }

    // Zero-phase smoothing with a Hann kernel of an odd length in frames.
    void Smooth(std::vector<float>& x, int length)
    {
      int half = length / 2;
      std::vector<float> w(length);
      for (int i = 0; i < length; i++)
      {
        w[i] = static_cast<float>(0.5 - 0.5 * cos(2.0 * kPi * (i + 1) / (length + 1)));
      }
      std::vector<float> out(x.size());
      long long size = static_cast<long long>(x.size());
      for (long long k = 0; k < size; k++)
      {
        float sum = 0.0f;
        float weight = 0.0f;
        long long a = (std::max)(0LL, k - half);
        long long b = (std::min)(size - 1, k + half);
        for (long long j = a; j <= b; j++)
        {
          float v = w[static_cast<size_t>(j - k + half)];
          sum += v * x[static_cast<size_t>(j)];
          weight += v;
        }
        out[static_cast<size_t>(k)] = sum / weight;
      }
      x.swap(out);
    }

    // The same, but what lies past the two ends counts as zero (a curve
    // that is at rest outside the sound).
    void SmoothToRest(std::vector<float>& x, int length)
    {
      int half = length / 2;
      std::vector<float> w(length);
      float whole = 0.0f;
      for (int i = 0; i < length; i++)
      {
        w[i] = static_cast<float>(0.5 - 0.5 * cos(2.0 * kPi * (i + 1) / (length + 1)));
        whole += w[i];
      }
      std::vector<float> out(x.size());
      long long size = static_cast<long long>(x.size());
      for (long long k = 0; k < size; k++)
      {
        float sum = 0.0f;
        long long a = (std::max)(0LL, k - half);
        long long b = (std::min)(size - 1, k + half);
        for (long long j = a; j <= b; j++)
        {
          sum += w[static_cast<size_t>(j - k + half)] * x[static_cast<size_t>(j)];
        }
        out[static_cast<size_t>(k)] = sum / whole;
      }
      x.swap(out);
    }

    void AddLine(std::string& text, char const* name, char const* format, double value)
    {
      char number[64];
      _snprintf_s(number, sizeof(number), _TRUNCATE, format, value);
      text += name;
      text += ": ";
      text += number;
      text += "\n";
    }

    // Numbers that are the same every time for the same seed
    struct Random
    {
      unsigned long long State;

      explicit Random(unsigned long long seed) : State(seed)
      {
      }

      // 0 up to, not with, 1
      double Next()
      {
        State += 0x9E3779B97F4A7C15ull;
        unsigned long long z = State;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        z ^= z >> 31;
        return static_cast<double>(z >> 11) * (1.0 / 9007199254740992.0);
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

    // A bump laid into a curve where it is higher than what is there: up
    // from start over rise frames, held, down over fall frames.
    void LayBump(std::vector<float>& curve, long long start, int rise, int hold, int fall, float size)
    {
      const long long count = static_cast<long long>(curve.size());
      for (long long k = (std::max)(0LL, start); k < start + rise + hold + fall && k < count; k++)
      {
        long long t = k - start;
        float v = 1.0f;
        if (t < rise)
        {
          v = Ease(static_cast<float>(t) / static_cast<float>(rise));
        }
        else if (t > rise + hold)
        {
          v = 1.0f - Ease(static_cast<float>(t - rise - hold) / static_cast<float>(fall));
        }
        v *= size;
        if (v > curve[static_cast<size_t>(k)])
        {
          curve[static_cast<size_t>(k)] = v;
        }
      }
    }

    struct Syl
    {
      long long Centre = 0;
      long long Left = 0;
      long long Right = 0;
      float Peak = 0.0f;        // dB against the speech level, of what lies above 300 Hz (what the syllables are found in)
      float Loud = 0.0f;        // dB against the speech level, of everything (what the stress score uses)
      float Length = 0.0f;      // frames within 10 dB of the peak
      float PitchHigh = 0.0f;   // semitones over 100 Hz: the upper part of its pitch
      float PitchMiddle = 0.0f;
      bool HasPitch = false;
      float Speech = 1.0f;
      int Phrase = -1;
      float LevelScore = 0.0f;
      float PitchScore = 0.0f;
      float RiseScore = 0.0f;
      float LengthScore = 0.0f;
      float Stress = 0.0f;
      float BrowScore = 0.0f;
      float NodScore = 0.0f;
    };

    struct Phrase
    {
      long long First = 0;
      long long End = 0;
      int FirstSyl = 0;
      int EndSyl = 0;
    };

    struct Anchor
    {
      long long Frame;
      float Weight;
      int Kind;   // 0 a phrase's end, 1 a strong nod, 2 before a phrase
    };

    bool AnalyseInner(short const* pcm, size_t samples, int rate, int channels, Track& out, std::string& error,
      Debug* debug);
  }

  bool Analyse(short const* pcm, size_t samples, int rate, int channels, Track& out, std::string& error)
  {
    return Analyse(pcm, samples, rate, channels, out, error, nullptr);
  }

  bool Analyse(short const* pcm, size_t samples, int rate, int channels, Track& out, std::string& error, Debug* debug)
  {
    // Running out of memory is not thrown at the caller: Analyse returns
    // false, with the reason in error and an empty track. (The work took
    // 3 MB of memory for each minute of sound when measured, on top of the
    // caller's samples.)
    try
    {
      return AnalyseInner(pcm, samples, rate, channels, out, error, debug);
    }
    catch (std::bad_alloc const&)
    {
      out.Frames.clear();
      out.Report.clear();
      if (debug != nullptr)
      {
        *debug = Debug();
      }
      try
      {
        error = "Not enough memory to analyse this file.";
      }
      catch (std::bad_alloc const&)
      {
        error.clear();
      }
      return false;
    }
  }

  Frame At(Track const& track, double seconds)
  {
    Frame rest = { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f };
    size_t count = track.Frames.size();
    if (count == 0 || !(track.FramesPerSecond > 0.0) || !std::isfinite(seconds) || seconds < 0.0)
    {
      return rest;
    }
    double at = seconds * track.FramesPerSecond;
    if (at >= static_cast<double>(count))
    {
      return rest;
    }
    size_t i = static_cast<size_t>(at);
    Frame const& a = track.Frames[i];
    if (i + 1 >= count)
    {
      return a;
    }
    Frame const& b = track.Frames[i + 1];
    float t = static_cast<float>(at - static_cast<double>(i));
    Frame f;
    f.Blink = a.Blink + (b.Blink - a.Blink) * t;
    f.Brow = a.Brow + (b.Brow - a.Brow) * t;
    f.Nod = a.Nod + (b.Nod - a.Nod) * t;
    f.Turn = a.Turn + (b.Turn - a.Turn) * t;
    f.Tilt = a.Tilt + (b.Tilt - a.Tilt) * t;
    return f;
  }

  namespace
  {
    bool AnalyseInner(short const* pcm, size_t samples, int rate, int channels, Track& out, std::string& error,
      Debug* debug)
    {
      out.FramesPerSecond = 100.0;
      out.Frames.clear();
      out.Report.clear();
      error.clear();
      if (debug != nullptr)
      {
        *debug = Debug();
      }
      if (rate < 8000 || rate > 192000)
      {
        error = "The sample rate must be 8000 to 192000.";
        return false;
      }
      if (channels != 1 && channels != 2)
      {
        error = "The audio must have 1 or 2 channels.";
        return false;
      }
      if (pcm == nullptr && samples != 0)
      {
        error = "No audio data.";
        return false;
      }

      int rightSign = 1;
      if (channels == 2)
      {
        // Two channels that are each other upside down add up to nothing. If
        // the sum holds under a tenth of the energy of the difference, the
        // right channel is turned over.
        double sum = 0.0;
        double difference = 0.0;
        size_t step = samples > 4000000 ? samples / 4000000 : 1;
        for (size_t i = 0; i < samples; i += step)
        {
          double l = pcm[i * 2];
          double r = pcm[i * 2 + 1];
          sum += (l + r) * (l + r);
          difference += (l - r) * (l - r);
        }
        if (sum < 0.1 * difference)
        {
          rightSign = -1;
          out.Report += "stereo: the channels cancel, the right one was turned over\n";
        }
      }

      // ---- The sound at 8 kHz, its levels, its pitch candidates ----
      std::vector<float> x;
      ToInternalRate(pcm, samples, rate, channels, rightSign, x);
      HighPass(x, 70.0);
      const size_t frames = x.size() / kHop + 1;
      const long long count = static_cast<long long>(frames);
      std::vector<float> whole;
      FrameLevels(x, frames, whole);
      std::vector<float> x4;
      Decimate2(x, x4);
      HighPass(x, 300.0);
      std::vector<float> level;
      FrameLevels(x, frames, level);
      x.clear();
      x.shrink_to_fit();

      float loudest = -120.0f;
      for (long long k = 0; k < count; k++)
      {
        loudest = (std::max)(loudest, whole[k]);
      }
      // What lies 65 dB under the loudest frame, or under -100 dB, is not
      // looked at for pitch.
      const float quiet = (std::max)(-100.0f, loudest - 65.0f);
      std::vector<float> pitch;
      std::vector<unsigned char> voiced;
      std::vector<float> strength;
      int repairs = 0;
      {
        std::vector<PitchFrame> candidates;
        FindCandidates(x4, frames, whole, quiet, candidates);
        x4.clear();
        x4.shrink_to_fit();
        TrackPitch(candidates, pitch, voiced, strength);
      }
      TidyPitch(pitch, voiced, repairs, debug);
      std::vector<unsigned char> flat;
      FindFlat(pitch, voiced, flat);

      Frame zero = { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f };
      out.Frames.assign(frames, zero);
      AddLine(out.Report, "seconds", "%.2f", static_cast<double>(samples) / rate);
      AddLine(out.Report, "frames", "%.0f", static_cast<double>(frames));

      // ---- The recording's own speech level ----
      // The 90th percentile of the level of the voiced frames is the level
      // of the vowel peaks. With no voiced frames there is no speech this
      // module can use (silence, noise, a whisper).
      std::vector<float> pool;
      pool.reserve(frames);
      for (long long k = 0; k < count; k++)
      {
        if (voiced[k] && level[k] > quiet)
        {
          pool.push_back(level[k]);
        }
      }
      bool speech = pool.size() >= 10;
      float global = 0.0f;
      std::vector<float> rel(frames, -60.0f);    // the loudness: what lies above 300 Hz, against the speech level
      std::vector<float> loud(frames, -60.0f);   // the level of everything, against the speech level
      float gate = 0.0f;
      if (!speech)
      {
        out.Report += "speech: none (no voiced sound: silence, noise or a whisper)\n";
      }
      else
      {
        global = Percentile(pool, 0.90);
        AddLine(out.Report, "speech level dBFS", "%.1f", global);
        // In a long file the level may drift: each second gets the percentile
        // of the 30 s around it, kept within 12 dB of the whole file's.
        std::vector<float> ref(frames, global);
        if (count > 3000)
        {
          const long long block = 100;
          const long long reach = 1500;
          size_t blocks = static_cast<size_t>((count + block - 1) / block);
          std::vector<float> local(blocks, global);
          for (size_t b = 0; b < blocks; b++)
          {
            long long centre = static_cast<long long>(b) * block + block / 2;
            long long first = (std::max)(0LL, centre - reach);
            long long end = (std::min)(count, centre + reach);
            pool.clear();
            for (long long k = first; k < end; k++)
            {
              if (voiced[k] && level[k] > quiet)
              {
                pool.push_back(level[k]);
              }
            }
            if (pool.size() >= 30)
            {
              local[b] = Clamp(Percentile(pool, 0.90), global - 12.0f, global + 12.0f);
            }
          }
          for (long long k = 0; k < count; k++)
          {
            double pos = (static_cast<double>(k) - block / 2.0) / block;
            long long b0 = static_cast<long long>(floor(pos));
            float t = static_cast<float>(pos - static_cast<double>(b0));
            long long last = static_cast<long long>(blocks) - 1;
            long long i0 = (std::min)((std::max)(b0, 0LL), last);
            long long i1 = (std::min)((std::max)(b0 + 1, 0LL), last);
            ref[k] = local[static_cast<size_t>(i0)] * (1.0f - t) + local[static_cast<size_t>(i1)] * t;
          }
        }
        // The same for the level of everything (with the first one's drift)
        pool.clear();
        for (long long k = 0; k < count; k++)
        {
          if (voiced[k] && level[k] > quiet)
          {
            pool.push_back(whole[k]);
          }
        }
        float wholeGlobal = Percentile(pool, 0.90);
        for (long long k = 0; k < count; k++)
        {
          rel[k] = Clamp(level[k] - ref[k], -60.0f, 30.0f);
          loud[k] = Clamp(whole[k] - wholeGlobal - (ref[k] - global), -60.0f, 30.0f);
        }
        // The quiet 5 % of the frames give the recording's floor. A sound
        // that stays within 6 dB of its own floor has no speech rhythm
        // (a hum, a held tone, a buzz).
        pool.assign(rel.begin(), rel.end());
        float floorLevel = Percentile(pool, 0.05);
        AddLine(out.Report, "floor dB below speech", "%.1f", -floorLevel);
        if (floorLevel > kSteadyFloor)
        {
          out.Report += "speech: none (a steady sound with no pauses)\n";
          speech = false;
        }
        gate = (std::max)(floorLevel + kGateAboveFloor, kGateLowest);
      }

      // A voiced frame counts from the gate up, and as a strong one within
      // 15 dB of the speech level.
      std::vector<unsigned char> voice(frames, 0);
      std::vector<unsigned char> strong(frames, 0);
      long long voiceFrames = 0;
      if (speech)
      {
        for (long long k = 0; k < count; k++)
        {
          voice[k] = voiced[k] && rel[k] > gate ? 1 : 0;
          strong[k] = voice[k] && rel[k] > kStrongFrame ? 1 : 0;
          voiceFrames += voice[k];
        }
        if (voiceFrames < 10)
        {
          out.Report += "speech: none (the voiced sound is all at the floor)\n";
          speech = false;
        }
      }

      // The smoothed loudness the syllables are found in (90 ms). What lies
      // under 300 Hz is left out of it: the hum of m, n and of voiced stops
      // sits there, so the dips between syllables are deeper without it.
      // A syllable's own level for the stress score is taken from
      // everything: an "ee" or an "oo" has most of its sound under 300 Hz
      // and would always lose against an "ah".
      std::vector<float> smooth(rel);
      Smooth(smooth, 9);
      Smooth(loud, 9);

      // ---- Syllables ----
      std::vector<Syl> syllables;
      if (speech)
      {
        const float from = (std::max)(gate + kSyllableOverGate, kSyllableLowest);
        std::vector<long long> kept;
        float lowest = 0.0f;   // the lowest loudness since the last peak kept
        for (long long k = 1; k + 1 < count; k++)
        {
          lowest = (std::min)(lowest, smooth[k]);
          if (!(smooth[k] > smooth[k - 1] && smooth[k] >= smooth[k + 1] && smooth[k] >= from))
          {
            continue;
          }
          bool byVoice = false;
          for (long long j = (std::max)(0LL, k - 3); j <= k + 3 && j < count; j++)
          {
            byVoice = byVoice || voice[j] != 0;
          }
          if (!byVoice)
          {
            continue;
          }
          // Two peaks with no dip of 2 dB between them are one syllable:
          // the higher one stays. (A higher peak that takes the place of
          // the last one kept still has that one's dip between itself and
          // the peak before.)
          if (kept.empty() || (std::min)(smooth[kept.back()], smooth[k]) - lowest >= kSyllableDip)
          {
            kept.push_back(k);
            lowest = smooth[k];
          }
          else if (smooth[k] > smooth[kept.back()])
          {
            kept.back() = k;
            lowest = smooth[k];
          }
        }
        syllables.resize(kept.size());
        for (size_t i = 0; i < kept.size(); i++)
        {
          Syl& s = syllables[i];
          s.Centre = kept[i];
          s.Peak = smooth[s.Centre];
          s.Left = (std::max)(0LL, s.Centre - kSyllableReach);
          s.Right = (std::min)(count - 1, s.Centre + kSyllableReach);
        }
        for (size_t i = 0; i + 1 < syllables.size(); i++)
        {
          // The lowest point between two centres parts them.
          long long a = syllables[i].Centre;
          long long b = syllables[i + 1].Centre;
          long long parting = a;
          for (long long j = a; j <= b; j++)
          {
            if (smooth[j] < smooth[parting])
            {
              parting = j;
            }
          }
          syllables[i].Right = (std::min)(syllables[i].Right, parting);
          syllables[i + 1].Left = (std::max)(syllables[i + 1].Left, parting);
        }
        std::vector<float> high;
        for (Syl& s : syllables)
        {
          // Not below the gate
          while (s.Left < s.Centre && smooth[s.Left] < gate)
          {
            s.Left++;
          }
          while (s.Right > s.Centre && smooth[s.Right] < gate)
          {
            s.Right--;
          }
          // Its length: the stretch around the centre within 10 dB of the peak
          long long a = s.Centre;
          long long b = s.Centre;
          while (a > s.Left && smooth[a - 1] > s.Peak - 10.0f)
          {
            a--;
          }
          while (b < s.Right && smooth[b + 1] > s.Peak - 10.0f)
          {
            b++;
          }
          s.Length = static_cast<float>(b - a + 1);
          // Its level, its pitch, and how much of the pitch stands still
          high.clear();
          int still = 0;
          s.Loud = loud[s.Centre];
          for (long long j = a; j <= b; j++)
          {
            s.Loud = (std::max)(s.Loud, loud[j]);
            if (voice[j])
            {
              high.push_back(pitch[j]);
              still += flat[j];
            }
          }
          s.HasPitch = high.size() >= 3;
          if (s.HasPitch)
          {
            float share = static_cast<float>(still) / static_cast<float>(high.size());
            s.Speech = 1.0f - Ramp(share, 0.5f, 0.8f);
            s.PitchMiddle = MedianOf(high);
            s.PitchHigh = Percentile(high, 0.80);
          }
        }
      }

      // ---- Phrases ----
      // A frame is speech if it is voiced, or loud enough and within 150 ms
      // of a voiced one (the hiss and the gaps of the consonants). Speech
      // with no gap of 250 ms is a phrase, if it holds a syllable.
      std::vector<unsigned char> speaking(frames, 0);
      std::vector<Phrase> phrases;
      if (speech && !syllables.empty())
      {
        long long last = -1000000;
        std::vector<long long> before(frames);
        for (long long k = 0; k < count; k++)
        {
          if (voice[k])
          {
            last = k;
          }
          before[k] = last;
        }
        long long next = 1000000000LL;
        for (long long k = count; k-- > 0;)
        {
          if (voice[k])
          {
            next = k;
          }
          bool byVoice = k - before[k] <= 15 || next - k <= 15;
          speaking[k] = voice[k] || (rel[k] > gate + 3.0f && byVoice) ? 1 : 0;
        }
        size_t syl = 0;
        for (long long k = 0; k < count;)
        {
          if (!speaking[k])
          {
            k++;
            continue;
          }
          long long end = k;
          long long gap = 0;
          for (long long j = k; j < count && gap < kPause; j++)
          {
            if (speaking[j])
            {
              end = j + 1;
              gap = 0;
            }
            else
            {
              gap++;
            }
          }
          Phrase p;
          p.First = k;
          p.End = end;
          while (syl < syllables.size() && syllables[syl].Centre < k)
          {
            syl++;
          }
          p.FirstSyl = static_cast<int>(syl);
          while (syl < syllables.size() && syllables[syl].Centre < end)
          {
            syllables[syl].Phrase = static_cast<int>(phrases.size());
            syl++;
          }
          p.EndSyl = static_cast<int>(syl);
          if (p.EndSyl > p.FirstSyl)
          {
            phrases.push_back(p);
          }
          k = end;
        }
        for (long long k = 0; k < count; k++)
        {
          speaking[k] = 0;
        }
        for (Phrase const& p : phrases)
        {
          for (long long k = p.First; k < p.End; k++)
          {
            speaking[k] = 1;
          }
        }
      }
      if (phrases.empty())
      {
        if (speech)
        {
          out.Report += "speech: none (no syllables found)\n";
        }
        speech = false;
        syllables.clear();
      }

      // ---- How speech-like the sound is around each frame ----
      // Where more than a third of the strong voiced frames of the 3 s
      // around a frame stand still in pitch, the sound is held notes, not
      // a voice: from a third to 60 % everything but the blinks fades out.
      // [check 2] The same way for three more things a voice has: a good
      // part of its speaking time is voiced (a drum beat's is not: a thump
      // with a short ring); its pitch is sure in most voiced frames (noise
      // that sounds through a resonance, as a whisper does, is "voiced" now
      // and then, but never surely); and it makes a syllable a second or
      // more (an engine's drone, a held howl do not). Without these the
      // head took a new pose for every drum bar, and a whisper-like noise
      // voice got brow raises and nods of 3 degrees.
      std::vector<float> likeness(frames, 1.0f);
      double stillShare = 0.0;
      double voicedShare = 0.0;
      double sureShare = 0.0;
      double syllableRate = 0.0;
      if (speech)
      {
        std::vector<int> sumVoice(frames + 1, 0);
        std::vector<int> sumFlat(frames + 1, 0);
        std::vector<int> sumSpeaking(frames + 1, 0);
        std::vector<int> sumVoiced(frames + 1, 0);
        std::vector<int> sumAnyVoice(frames + 1, 0);
        std::vector<int> sumSure(frames + 1, 0);
        for (long long k = 0; k < count; k++)
        {
          sumVoice[k + 1] = sumVoice[k] + strong[k];
          sumFlat[k + 1] = sumFlat[k] + (strong[k] && flat[k] ? 1 : 0);
          sumSpeaking[k + 1] = sumSpeaking[k] + (speaking[k] ? 1 : 0);
          sumVoiced[k + 1] = sumVoiced[k] + (speaking[k] && voice[k] ? 1 : 0);
          sumAnyVoice[k + 1] = sumAnyVoice[k] + voice[k];
          sumSure[k + 1] = sumSure[k] + (voice[k] && strength[k] >= kSureCorrelation ? 1 : 0);
        }
        stillShare = sumVoice[frames] > 0 ? static_cast<double>(sumFlat[frames]) / sumVoice[frames] : 0.0;
        voicedShare = sumSpeaking[frames] > 0 ? static_cast<double>(sumVoiced[frames]) / sumSpeaking[frames] : 0.0;
        sureShare = sumAnyVoice[frames] > 0 ? static_cast<double>(sumSure[frames]) / sumAnyVoice[frames] : 0.0;
        long long inPhrases = 0;
        for (Syl const& s : syllables)
        {
          inPhrases += s.Phrase >= 0 ? 1 : 0;
        }
        syllableRate = sumSpeaking[frames] > 0 ? 100.0 * static_cast<double>(inPhrases) / sumSpeaking[frames] : 0.0;
        const float byRate = Ramp(static_cast<float>(syllableRate), kSyllableRateFrom, kSyllableRateFull);
        for (long long k = 0; k < count; k++)
        {
          long long a = (std::max)(0LL, k - 150);
          long long b = (std::min)(count, k + 151);
          int v = sumVoice[b] - sumVoice[a];
          int f = sumFlat[b] - sumFlat[a];
          if (v >= 10)
          {
            likeness[k] = 1.0f - Ramp(static_cast<float>(f) / static_cast<float>(v), 0.35f, 0.60f);
          }
          int s = sumSpeaking[b] - sumSpeaking[a];
          int u = sumVoiced[b] - sumVoiced[a];
          if (s >= 20)
          {
            likeness[k] *= Ramp(static_cast<float>(u) / static_cast<float>(s), kVoicedShareFrom, kVoicedShareFull);
          }
          int any = sumAnyVoice[b] - sumAnyVoice[a];
          int sure = sumSure[b] - sumSure[a];
          if (any >= 5)
          {
            likeness[k] *= Ramp(static_cast<float>(sure) / static_cast<float>(any), kSureShareFrom, kSureShareFull);
          }
          likeness[k] *= byRate;
        }
        Smooth(likeness, 101);
        char const* none = nullptr;
        if (stillShare >= 0.60)
        {
          none = "speech: none (the pitch stands still: held notes, not a voice)\n";
        }
        else if (voicedShare < kVoicedShareFrom)
        {
          none = "speech: none (hardly any of it is voiced: beats or bursts, not a voice)\n";
        }
        else if (sureShare < kSureShareFrom)
        {
          none = "speech: none (its pitch is nowhere sure: a whisper, a murmur of many voices, or deep in noise)\n";
        }
        else if (syllableRate < kSyllableRateFrom)
        {
          none = "speech: none (fewer syllables than one in two seconds: a drone, not a voice)\n";
        }
        if (none != nullptr)
        {
          out.Report += none;
          speech = false;
          syllables.clear();
          phrases.clear();
          speaking.assign(frames, 0);
        }
      }

      // ---- The stress score ----
      float pitchMiddle = 0.0f;   // the voice's middle pitch, semitones over 100 Hz
      float pitchSpread = 0.0f;   // half of what lies between the 16th and the 84th percentile of its pitch
      float loudMiddle = 0.0f;
      float lengthMiddle = 1.0f;
      if (speech)
      {
        pool.clear();
        for (long long k = 0; k < count; k++)
        {
          if (strong[k])
          {
            pool.push_back(pitch[k]);
          }
        }
        if (pool.size() < 10)
        {
          pool.clear();
          for (long long k = 0; k < count; k++)
          {
            if (voice[k])
            {
              pool.push_back(pitch[k]);
            }
          }
        }
        pitchMiddle = MedianOf(pool);
        pitchSpread = 0.5f * (Percentile(pool, 0.84) - Percentile(pool, 0.16));
        // A lively voice is measured with its own spread, so that it is
        // not all stress; a flat one with 2 semitones, so that its small
        // steps are not blown up.
        const float pitchScale = (std::max)(kPitchScale, pitchSpread);
        pool.clear();
        for (Syl const& s : syllables)
        {
          pool.push_back(s.Loud);
        }
        loudMiddle = MedianOf(pool);
        pool.clear();
        for (Syl const& s : syllables)
        {
          pool.push_back(s.Length);
        }
        lengthMiddle = (std::max)(1.0f, MedianOf(pool));

        std::vector<float> around;
        const int total = static_cast<int>(syllables.size());
        for (int i = 0; i < total; i++)
        {
          Syl& s = syllables[i];
          if (s.Phrase < 0)
          {
            continue;
          }
          Phrase const& p = phrases[s.Phrase];
          // The neighbours: up to four syllables to each side, in the same
          // phrase, within 2 s. With fewer than two of them the whole
          // file's middle values stand in. The neighbours' middle level is
          // kept within 4 dB of the whole file's and their middle length
          // within a third of it: a syllable among whispered ones is not
          // strong for that.
          float loudNear = loudMiddle;
          float lengthNear = lengthMiddle;
          int first = (std::max)(p.FirstSyl, i - 4);
          int end = (std::min)(p.EndSyl, i + 5);
          around.clear();
          for (int j = first; j < end; j++)
          {
            if (j != i && llabs(syllables[j].Centre - s.Centre) <= 200)
            {
              around.push_back(syllables[j].Loud);
            }
          }
          if (around.size() >= 2)
          {
            loudNear = Clamp(MedianOf(around), loudMiddle - kNearLevel, loudMiddle + kNearLevel);
            around.clear();
            for (int j = first; j < end; j++)
            {
              if (j != i && llabs(syllables[j].Centre - s.Centre) <= 200)
              {
                around.push_back(syllables[j].Length);
              }
            }
            lengthNear = Clamp(MedianOf(around), lengthMiddle / kNearLength, lengthMiddle * kNearLength);
            lengthNear = (std::max)(1.0f, lengthNear);
          }
          s.LevelScore = Clamp((s.Loud - loudNear) / kLevelScale, -2.5f, 2.5f);
          float octaves = static_cast<float>(log(static_cast<double>(s.Length) / lengthNear) * kLog2);
          s.LengthScore = octaves / kLengthScale;
          if (i == p.EndSyl - 1 && p.EndSyl - p.FirstSyl >= 2)
          {
            s.LengthScore -= kFinalLength;
          }
          s.LengthScore = Clamp(s.LengthScore, -2.5f, 2.5f);
          if (s.HasPitch)
          {
            // The running pitch: the middle of the strong voiced frames of
            // the 2 s to each side
            around.clear();
            long long a = (std::max)(0LL, s.Centre - 200);
            long long b = (std::min)(count, s.Centre + 201);
            for (long long k = a; k < b; k++)
            {
              if (strong[k])
              {
                around.push_back(pitch[k]);
              }
            }
            float running = around.size() >= 20 ? MedianOf(around) : pitchMiddle;
            s.PitchScore = Clamp((s.PitchHigh - running) / pitchScale, -2.5f, 2.5f);
            if (i > p.FirstSyl && syllables[i - 1].HasPitch && s.Centre - syllables[i - 1].Centre <= 50)
            {
              s.RiseScore = Clamp((s.PitchHigh - syllables[i - 1].PitchMiddle) / (pitchScale * kRiseScale / kPitchScale),
                -1.0f, 2.0f);
            }
          }
          s.Stress = 0.40f * s.LevelScore + 0.30f * s.PitchScore + 0.10f * s.RiseScore + 0.20f * s.LengthScore;
          s.BrowScore = 0.25f * s.LevelScore + 0.45f * s.PitchScore + 0.20f * s.RiseScore + 0.10f * s.LengthScore;
          s.NodScore = 0.50f * s.LevelScore + 0.20f * s.PitchScore + 0.30f * s.LengthScore;
        }
      }

      // What the choices that look like chance are made from: the sound's
      // length in tenths of a second, and for each choice where in the
      // sound it is made (to half a second) and what it is. The same sound
      // gives the same numbers, and so does a louder or a quieter copy of
      // it.
      // [check 3] It was the length in frames and the numbers of syllables
      // and phrases, with one row of numbers for everything: one syllable
      // more or less (1 ms of silence before the same sound did that in 34
      // of 100 files) gave every pose and most blinks of the file anew. Now
      // a small change in the sound changes at most the choice next to it.
      unsigned long long seed = 0xCBF29CE484222325ull;
      Mix(seed, count / 10);
      auto chance = [seed](long long what, long long where, long long shape)
      {
        unsigned long long s = seed;
        Mix(s, what);
        Mix(s, where);
        Mix(s, shape);
        return Random(s);
      };

      std::vector<float> brow(frames, 0.0f);
      std::vector<float> dip(frames, 0.0f);
      std::vector<float> drift(frames, 0.0f);
      std::vector<float> turn(frames, 0.0f);
      std::vector<float> tilt(frames, 0.0f);
      std::vector<float> blink(frames, 0.0f);
      std::vector<Anchor> anchors;
      int stressed = 0;
      int browCount = 0;
      int nodCount = 0;
      int endDipCount = 0;
      int poseCount = 0;
      std::vector<long long> strongNods;

      if (speech)
      {
        const int total = static_cast<int>(syllables.size());
        std::vector<int> order(total);
        std::vector<long long> taken;

        // ---- Brow: the strongest by the brow score, 2 s apart ----
        // The height grows from nothing at a score of 0.8 to a full raise
        // at 1.8, so a syllable just over the line gets a raise too faint
        // to see, not a sudden half one.
        for (int i = 0; i < total; i++)
        {
          order[i] = i;
          stressed += syllables[i].Stress >= kStressFrom && syllables[i].Speech >= 0.5f ? 1 : 0;
        }
        std::sort(order.begin(), order.end(), [&syllables](int a, int b)
        {
          if (syllables[a].BrowScore != syllables[b].BrowScore)
          {
            return syllables[a].BrowScore > syllables[b].BrowScore;
          }
          return a < b;
        });
        for (int i : order)
        {
          Syl const& s = syllables[i];
          float weight = s.Speech * likeness[s.Centre];
          if (s.BrowScore < kBrowFrom || s.Phrase < 0 || weight < 0.5f || s.Loud < kEventLevel)
          {
            continue;
          }
          bool free = true;
          for (long long t : taken)
          {
            free = free && llabs(t - s.Centre) >= kBrowGap;
          }
          if (!free)
          {
            continue;
          }
          taken.push_back(s.Centre);
          float height = Ramp(s.BrowScore, kBrowFrom, kBrowFull) * weight;
          int hold = kBrowHoldLeast + static_cast<int>(Clamp(s.Length * 0.5f, 0.0f,
            static_cast<float>(kBrowHoldMost - kBrowHoldLeast)));
          LayBump(brow, s.Centre - kBrowLead, kBrowRise, hold, kBrowFall, height);
          browCount += height >= kBrowClear ? 1 : 0;
          if (debug != nullptr)
          {
            Event e;
            e.Seconds = static_cast<double>(s.Centre) / 100.0;
            e.Kind = "brow";
            e.Size = height;
            e.Score = s.BrowScore;
            e.Stress = s.Stress;
            debug->Events.push_back(e);
          }
        }

        // ---- Nod: the strongest by the nod score, 0.7 s apart ----
        // The size grows from nothing at a score of 0.6 to 3.5 degrees at
        // 1.8.
        taken.clear();
        std::sort(order.begin(), order.end(), [&syllables](int a, int b)
        {
          if (syllables[a].NodScore != syllables[b].NodScore)
          {
            return syllables[a].NodScore > syllables[b].NodScore;
          }
          return a < b;
        });
        for (int i : order)
        {
          Syl const& s = syllables[i];
          float weight = s.Speech * likeness[s.Centre];
          if (s.NodScore < kNodFrom || s.Phrase < 0 || weight < 0.5f || s.Loud < kEventLevel)
          {
            continue;
          }
          bool free = true;
          for (long long t : taken)
          {
            free = free && llabs(t - s.Centre) >= kNodGap;
          }
          if (!free)
          {
            continue;
          }
          taken.push_back(s.Centre);
          float size = kNodMost * Ramp(s.NodScore, kNodFrom, kNodFull) * weight;
          LayBump(dip, s.Centre - kNodDown, kNodDown, kNodHold, kNodUp, size);
          nodCount += size >= kNodClear ? 1 : 0;
          if (size >= kNodStrong)
          {
            strongNods.push_back(s.Centre);
            Anchor a = { s.Centre - kNodDown + 2, 2.0f, 1 };
            anchors.push_back(a);
          }
          if (debug != nullptr)
          {
            Event e;
            e.Seconds = static_cast<double>(s.Centre) / 100.0;
            e.Kind = "nod";
            e.Size = size;
            e.Score = s.NodScore;
            e.Stress = s.Stress;
            debug->Events.push_back(e);
          }
        }
        std::sort(strongNods.begin(), strongNods.end());

        // ---- The dip at a phrase's end ----
        // Where a phrase ends with the pitch down and a pause follows, the
        // chin goes down into the end and comes back in the pause. An end
        // with the pitch up (a question) gets none.
        // [check 1] The dip grows from nothing with the pause (250 ms: none,
        // 450 ms: all of it) and with the phrase's length (0.6 s: none,
        // 1.2 s: all of it). It was all or nothing at a pause of 300 ms and
        // at three syllables: in the user's recording two pauses are 290 and
        // 320 ms long, and 1 ms of silence put before the same sound took
        // one 1.5 degree dip away and put another one in.
        for (size_t j = 0; j < phrases.size(); j++)
        {
          Phrase const& p = phrases[j];
          long long pause = (j + 1 < phrases.size() ? phrases[j + 1].First : count) - p.End;
          Anchor a = { p.End + 5, 3.0f, 0 };
          if (pause >= 30)
          {
            anchors.push_back(a);
          }
          if (j + 1 < phrases.size() && pause >= 50)
          {
            Anchor b = { phrases[j + 1].First - 30, 1.5f, 2 };
            anchors.push_back(b);
          }
          if (j == 0 && p.First >= 50)
          {
            Anchor b = { p.First - 30, 1.5f, 2 };
            anchors.push_back(b);
          }
          const float byPause = Ramp(static_cast<float>(pause), kEndPauseFrom, kEndPauseFull);
          const float byLength = Ramp(static_cast<float>(p.End - p.First), kEndPhraseFrom, kEndPhraseFull);
          if (byPause * byLength <= 0.0f)
          {
            continue;
          }
          Syl const& lastSyl = syllables[p.EndSyl - 1];
          float size = kEndDip * 0.66f;
          if (lastSyl.HasPitch)
          {
            pool.clear();
            for (int i = p.FirstSyl; i < p.EndSyl; i++)
            {
              if (syllables[i].HasPitch)
              {
                pool.push_back(syllables[i].PitchMiddle);
              }
            }
            size = kEndDip * (1.0f - Ramp(lastSyl.PitchMiddle - MedianOf(pool), 0.0f, 2.0f));
          }
          size *= lastSyl.Speech * likeness[lastSyl.Centre] * byPause * byLength;
          if (size <= 0.0f)
          {
            continue;
          }
          LayBump(dip, p.End - kEndDown, kEndDown, kEndHold, kEndUp, size);
          endDipCount += size >= kEndClear ? 1 : 0;
          if (debug != nullptr)
          {
            Event e;
            e.Seconds = static_cast<double>(p.End) / 100.0;
            e.Kind = "end dip";
            e.Size = size;
            debug->Events.push_back(e);
          }
        }

        // ---- The slow drift ----
        // Each syllable says how high and how loud the voice is against
        // its own middle (4 semitones or 6 dB: all the way); between the
        // syllables of a phrase that is drawn straight, outside the
        // phrases it is rest, and 1 s of smoothing makes it slow.
        for (Phrase const& p : phrases)
        {
          long long a = (std::max)(0LL, p.First - kDriftLead);
          long long b = (std::min)(count, p.End + kDriftTail);
          int i = p.FirstSyl;
          for (long long k = a; k < b; k++)
          {
            while (i + 1 < p.EndSyl && syllables[i + 1].Centre <= k)
            {
              i++;
            }
            auto way = [&](Syl const& s)
            {
              float byPitch = s.HasPitch ? Clamp((s.PitchMiddle - pitchMiddle) / 4.0f, -1.0f, 1.0f) : 0.0f;
              float byLevel = Clamp((s.Loud - loudMiddle) / 6.0f, -1.0f, 1.0f);
              return (0.6f * byPitch + 0.4f * byLevel) * s.Speech;
            };
            float v = way(syllables[i]);
            if (i + 1 < p.EndSyl && k > syllables[i].Centre)
            {
              float t = static_cast<float>(k - syllables[i].Centre) /
                static_cast<float>(syllables[i + 1].Centre - syllables[i].Centre);
              v += (way(syllables[i + 1]) - v) * t;
            }
            drift[k] = v;
          }
        }
        SmoothToRest(drift, kDriftSmooth);
        for (long long k = 0; k < count; k++)
        {
          drift[k] *= -kDriftMost * likeness[k];
        }

        // ---- Turn and tilt: a pose for a phrase ----
        // A new pose is asked for where a phrase starts (not within 3 s of
        // the last one) and at a strong nod inside a long phrase (3.5 s
        // after the last one, 1 s before the phrase ends); rest is asked
        // for 0.5 s into a pause of 1 s or more. 1.2 s of smoothing makes
        // the head go there slowly. (The changes are taken in the order
        // they were asked for: where a pose pushed back to 0.6 s lies after
        // its own phrase's rest, the rest stands.)
        {
          struct Change
          {
            long long Frame;
            float Turn;
            float Tilt;
          };
          std::vector<Change> changes;
          long long lastChange = -1000000;
          float lastTurn = 0.0f;
          float lastTilt = 0.0f;
          auto draw = [&](long long frame, long long at, long long shape)
          {
            // Anywhere in the range, but 0.8 to 3 degrees of turn (0.4 or
            // more of tilt) away from the pose before: seen, never a swing
            // (not asked for in the sound's first 0.6 s: with the smoothing the
            // head then leaves rest at the first frame, not before it)
            // [check 3] This pose's own numbers: from where it is asked for
            // (to half a second) and its phrase's length (to half a second)
            Random random = chance(1, at / 50, shape);
            Change c;
            c.Frame = (std::max)(static_cast<long long>(kPoseSmooth / 2), frame);
            c.Turn = lastTurn + (lastTurn > 0.0f ? -1.2f : 1.2f);
            c.Tilt = lastTilt + (lastTilt > 0.0f ? -0.6f : 0.6f);
            for (int tries = 0; tries < 8; tries++)
            {
              float t = kTurnMost * static_cast<float>(random.Next() * 2.0 - 1.0);
              if (fabs(t - lastTurn) >= kTurnStepLeast && fabs(t - lastTurn) <= kTurnStepMost)
              {
                c.Turn = t;
                break;
              }
            }
            for (int tries = 0; tries < 8; tries++)
            {
              float t = kTiltMost * static_cast<float>(random.Next() * 2.0 - 1.0);
              if (fabs(t - lastTilt) >= kTiltStepLeast)
              {
                c.Tilt = t;
                break;
              }
            }
            changes.push_back(c);
            lastChange = at;
            lastTurn = c.Turn;
            lastTilt = c.Tilt;
            if (likeness[(std::min)(count - 1, at)] > 0.0f)
            {
              poseCount++;
              if (debug != nullptr)
              {
                Event e;
                e.Seconds = static_cast<double>(c.Frame) / 100.0;
                e.Kind = "pose";
                e.Size = c.Turn;
                e.Score = c.Tilt;
                debug->Events.push_back(e);
              }
            }
          };
          size_t nod = 0;
          for (size_t j = 0; j < phrases.size(); j++)
          {
            Phrase const& p = phrases[j];
            bool fresh = j == 0 || p.First - phrases[j - 1].End >= kLongPause;
            if (fresh && j > 0)
            {
              Change rest = { phrases[j - 1].End + kPoseRest, 0.0f, 0.0f };
              changes.push_back(rest);
              lastTurn = 0.0f;
              lastTilt = 0.0f;
            }
            if (fresh || p.First - lastChange >= kPoseGap)
            {
              draw(p.First - kPoseLead, p.First, (p.End - p.First) / 50);
            }
            while (nod < strongNods.size() && strongNods[nod] < p.First)
            {
              nod++;
            }
            for (; nod < strongNods.size() && strongNods[nod] < p.End; nod++)
            {
              long long at = strongNods[nod];
              if (at - lastChange >= kPoseGapInside && p.End - at >= 100)
              {
                draw(at - 10, at, -1);
              }
            }
          }
          Change rest = { phrases.back().End + kPoseRest, 0.0f, 0.0f };
          changes.push_back(rest);
          size_t c = 0;
          float nowTurn = 0.0f;
          float nowTilt = 0.0f;
          for (long long k = 0; k < count; k++)
          {
            while (c < changes.size() && changes[c].Frame <= k)
            {
              nowTurn = changes[c].Turn;
              nowTilt = changes[c].Tilt;
              c++;
            }
            turn[k] = nowTurn;
            tilt[k] = nowTilt;
          }
          SmoothToRest(turn, kPoseSmooth);
          SmoothToRest(tilt, kPoseSmooth);
          for (long long k = 0; k < count; k++)
          {
            turn[k] *= likeness[k];
            tilt[k] *= likeness[k];
          }
        }
      }

      // ---- Blinks ----
      // One blink is 280 ms: shut in 80, shut for 30, open in 170. The next
      // one is due 2 to 6 s after the last (the short gaps more often). It
      // is moved to a place that draws a blink, if one lies in those 2 to
      // 6 s: just after a phrase's end, the start of a strong nod, just
      // before a phrase. With none, it comes when due, but not inside a
      // phrase shorter than 1.5 s (it goes after or before it), and inside
      // a longer phrase at the lowest point between two syllables near it.
      int blinkCount = 0;
      {
        std::sort(anchors.begin(), anchors.end(), [](Anchor const& a, Anchor const& b)
        {
          return a.Frame < b.Frame;
        });
        const long long lastStart = count - 1 - kBlinkFrames;
        long long from = kBlinkFirstFrom;
        long long to = kBlinkFirstFrom + 400;
        // [check 3] The first blink's own number; after that each blink draws
        // from numbers made from where it is (to half a second)
        Random first = chance(2, 0, 0);
        long long due = kBlinkFirstFrom + static_cast<long long>(400.0 * first.Next());
        size_t firstAnchor = 0;
        while (from <= lastStart)
        {
          to = (std::min)(to, lastStart);
          due = (std::min)(due, to);
          while (firstAnchor < anchors.size() && anchors[firstAnchor].Frame < from)
          {
            firstAnchor++;
          }
          int best = -1;
          double bestScore = 0.0;
          for (size_t a = firstAnchor; a < anchors.size() && anchors[a].Frame <= to; a++)
          {
            double away = static_cast<double>(llabs(anchors[a].Frame - due));
            double score = anchors[a].Weight * exp(-away / 150.0);
            if (score > bestScore)
            {
              bestScore = score;
              best = static_cast<int>(a);
            }
          }
          long long at = due;
          char const* why = "when due, in silence";
          bool placed = true;
          if (best >= 0 && (anchors[best].Weight >= 2.0f || llabs(anchors[best].Frame - due) <= 100))
          {
            at = anchors[best].Frame;
            why = anchors[best].Kind == 0 ? "after a phrase's end" :
              (anchors[best].Kind == 1 ? "with a strong nod" : "before a phrase");
          }
          else if (speaking[at])
          {
            Phrase const* inside = nullptr;
            for (Phrase const& p : phrases)
            {
              if (at >= p.First && at < p.End)
              {
                inside = &p;
              }
            }
            if (inside != nullptr && inside->End - inside->First < 150)
            {
              if (inside->End + 5 >= from && inside->End + 5 <= to)
              {
                at = inside->End + 5;
                why = "after a short phrase";
              }
              else if (inside->First - 30 >= from)
              {
                at = inside->First - 30;
                why = "before a short phrase";
              }
              else
              {
                placed = false;
              }
            }
            else if (inside != nullptr)
            {
              long long lowest = -1;
              for (int i = inside->FirstSyl; i + 1 < inside->EndSyl; i++)
              {
                long long edge = syllables[i].Right;
                if (edge - 6 >= from && edge - 6 <= to && llabs(edge - at) <= 60 &&
                    (lowest < 0 || smooth[edge] < smooth[lowest]))
                {
                  lowest = edge;
                }
              }
              if (lowest >= 0)
              {
                at = lowest - 6;
                why = "between two syllables";
              }
              else
              {
                why = "when due, in speech";
              }
            }
          }
          long long lastBlink = due;
          Random next = chance(3, (placed ? at : due) / 50, 0);
          const bool two = next.Next() < kBlinkTwo;
          if (placed)
          {
            LayBump(blink, at, kBlinkClose, kBlinkHold, kBlinkOpen, 1.0f);
            blinkCount++;
            lastBlink = at;
            if (debug != nullptr)
            {
              Event e;
              e.Seconds = static_cast<double>(at) / 100.0;
              e.Kind = "blink";
              e.Size = 1.0f;
              e.Why = why;
              debug->Events.push_back(e);
            }
            // [check 4] The second of two only where its lids can shut
            // before any speech: 420 ms after a blink at a phrase's end
            // the next phrase has often begun (of 62 second blinks measured,
            // 22 started inside a phrase, 16 of them in a loud syllable).
            bool clear = true;
            for (long long j = at + kBlinkSecond; j <= at + kBlinkSecond + kBlinkClose && j < count; j++)
            {
              clear = clear && speaking[j] == 0;
            }
            if (two && clear && at + kBlinkSecond <= lastStart)
            {
              LayBump(blink, at + kBlinkSecond, kBlinkClose, kBlinkHold, kBlinkOpen, 1.0f);
              blinkCount++;
              lastBlink = at + kBlinkSecond;
              if (debug != nullptr)
              {
                Event e;
                e.Seconds = static_cast<double>(lastBlink) / 100.0;
                e.Kind = "blink";
                e.Size = 1.0f;
                e.Why = "the second of two";
                debug->Events.push_back(e);
              }
            }
          }
          from = lastBlink + kBlinkLeast;
          to = lastBlink + kBlinkMost;
          due = lastBlink + kBlinkLeast +
            static_cast<long long>((kBlinkMost - kBlinkLeast) * pow(next.Next(), 1.5));
        }
      }

      // ---- The frames ----
      // Everything but the blinks fades in over the first 0.2 s and out
      // over the last 0.4 s, so the face is at rest where the sound starts
      // and ends. Then the limits: every value inside its range, and no
      // change from one frame to the next larger than the stated speed.
      {
        float before[5] = { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f };
        const float most[5] = { MaxBlinkSpeed / 100.0f, MaxBrowSpeed / 100.0f, MaxNodSpeed / 100.0f,
          MaxTurnSpeed / 100.0f, MaxTiltSpeed / 100.0f };
        for (long long k = 0; k < count; k++)
        {
          float fade = Ease(static_cast<float>(k) / kFadeIn) * Ease(static_cast<float>(count - 1 - k) / kFadeOut);
          float v[5];
          v[0] = Clamp(blink[k], 0.0f, 1.0f);
          v[1] = Clamp(brow[k] * fade, 0.0f, 1.0f);
          v[2] = Clamp((dip[k] + drift[k]) * fade * kDegree, -MaxNodUp, MaxNodDown);
          v[3] = Clamp(turn[k] * fade * kDegree, -MaxTurn, MaxTurn);
          v[4] = Clamp(tilt[k] * fade * kDegree, -MaxTilt, MaxTilt);
          for (int c = 0; c < 5; c++)
          {
            if (!std::isfinite(v[c]))
            {
              v[c] = 0.0f;
            }
            v[c] = Clamp(v[c], before[c] - most[c], before[c] + most[c]);
            before[c] = v[c];
          }
          Frame& f = out.Frames[static_cast<size_t>(k)];
          f.Blink = v[0];
          f.Brow = v[1];
          f.Nod = v[2];
          f.Turn = v[3];
          f.Tilt = v[4];
        }
      }

      // ---- What was measured ----
      double speechSeconds = 0.0;
      for (Phrase const& p : phrases)
      {
        speechSeconds += static_cast<double>(p.End - p.First) / 100.0;
      }
      if (speech)
      {
        AddLine(out.Report, "speech seconds", "%.2f", speechSeconds);
        AddLine(out.Report, "phrases", "%.0f", static_cast<double>(phrases.size()));
        AddLine(out.Report, "voiced share %", "%.1f", 100.0 * static_cast<double>(voiceFrames) / static_cast<double>(count));
        AddLine(out.Report, "median pitch Hz", "%.1f", 100.0 * pow(2.0, pitchMiddle / 12.0));
        AddLine(out.Report, "pitch standing still %", "%.1f", 100.0 * stillShare);
        AddLine(out.Report, "voiced share of speech %", "%.1f", 100.0 * voicedShare);   // [check 2]
        AddLine(out.Report, "sure share of voice %", "%.1f", 100.0 * sureShare);         // [check 2]
        AddLine(out.Report, "syllables a second of speech", "%.2f", syllableRate);       // [check 2]
        AddLine(out.Report, "octave repairs", "%.0f", repairs);
        AddLine(out.Report, "syllables", "%.0f", static_cast<double>(syllables.size()));
        AddLine(out.Report, "stressed syllables", "%.0f", stressed);
        AddLine(out.Report, "brow raises", "%.0f", browCount);
        AddLine(out.Report, "nods", "%.0f", nodCount);
        AddLine(out.Report, "phrase end dips", "%.0f", endDipCount);
        AddLine(out.Report, "poses", "%.0f", poseCount);
      }
      AddLine(out.Report, "blinks", "%.0f", blinkCount);

      if (debug != nullptr)
      {
        std::vector<float> hz(frames, 0.0f);
        std::vector<float> isVoiced(frames, 0.0f);
        std::vector<float> isVoice(frames, 0.0f);
        std::vector<float> isFlat(frames, 0.0f);
        std::vector<float> isSpeaking(frames, 0.0f);
        std::vector<float> centre(frames, 0.0f);
        std::vector<float> stress(frames, 0.0f);
        for (long long k = 0; k < count; k++)
        {
          hz[k] = voiced[k] ? static_cast<float>(100.0 * pow(2.0, pitch[k] / 12.0)) : 0.0f;
          isVoiced[k] = voiced[k] ? 1.0f : 0.0f;
          isVoice[k] = voice[k] ? 1.0f : 0.0f;
          isFlat[k] = flat[k] ? 1.0f : 0.0f;
          isSpeaking[k] = speaking[k] ? 1.0f : 0.0f;
        }
        for (Syl const& s : syllables)
        {
          centre[s.Centre] = 1.0f;
          stress[s.Centre] = s.Stress;
          Syllable d;
          d.Seconds = static_cast<double>(s.Centre) / 100.0;
          d.From = static_cast<double>(s.Left) / 100.0;
          d.To = static_cast<double>(s.Right) / 100.0;
          d.Level = s.Loud;
          d.Pitch = s.HasPitch ? static_cast<float>(100.0 * pow(2.0, s.PitchHigh / 12.0)) : 0.0f;
          d.Length = s.Length / 100.0f;
          d.LevelScore = s.LevelScore;
          d.PitchScore = s.PitchScore;
          d.RiseScore = s.RiseScore;
          d.LengthScore = s.LengthScore;
          d.Stress = s.Stress;
          d.BrowScore = s.BrowScore;
          d.NodScore = s.NodScore;
          d.Speech = s.Speech;
          d.Phrase = s.Phrase;
          debug->Syllables.push_back(d);
        }
        for (Phrase const& p : phrases)
        {
          debug->PhraseFrom.push_back(static_cast<double>(p.First) / 100.0);
          debug->PhraseTo.push_back(static_cast<double>(p.End) / 100.0);
        }
        debug->Names =
        {
          "level_dbfs", "whole_dbfs", "level_rel", "loudness", "pitch_hz", "voiced", "strength", "voice", "flat",
          "speaking", "likeness", "syllable", "stress", "drift_deg"
        };
        debug->Columns =
        {
          level, whole, rel, smooth, hz, isVoiced, strength, isVoice, isFlat, isSpeaking, likeness, centre, stress,
          drift
        };
      }
      return true;
    }
  }
}
