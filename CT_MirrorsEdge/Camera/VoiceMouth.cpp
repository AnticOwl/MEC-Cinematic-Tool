// See VoiceMouth.h. The steps, in order:
//   1. Mix to mono and resample to 16 kHz (windowed sinc), high-pass at 70 Hz.
//   2. Every 10 ms, a 25 ms spectrum: band levels, a voicing measure (the
//      best normalised cross-correlation at pitch lags), a tone test.
//   3. The recording's own speech level (90th percentile of the voiced
//      frames) and floor; every level after that is relative to them.
//   4. Jaw from the 500 - 1300 Hz level (where the first formant of an open
//      vowel sits), Wide and Round from band ratios (second formant high or
//      low), Teeth from high-band noise, Closed from short closures scored
//      for lip evidence.
//   5. Zero-phase smoothing, so the curves do not lag the sound, and a lead
//      of 10 ms: three smoothings of the same curves (the menu's Mouth
//      smoothing goes from one to the next).
// What it does well and what it does not is in VoiceMouth.h.
#include "VoiceMouth.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <initializer_list>
#include <new>

namespace voice_mouth
{
  namespace
  {
    const double kPi = 3.14159265358979323846;
    const int kRate = 16000;
    const int kHop = 160;
    const int kWindow = 400;
    const int kFft = 512;

    // Band edges in Hz. The levels the shapes use are sums of these.
    const int kBandCount = 11;
    const double kBandEdges[kBandCount + 1] =
    {
      100.0, 330.0, 500.0, 600.0, 1300.0, 1700.0, 1850.0, 2600.0, 3300.0, 3800.0, 4500.0, 7600.0
    };

    // The mouth moves this much ahead of the sound.
    const int kLeadFrames = 1;

    float Ramp(float x, float a, float b)
    {
      float t = (x - a) / (b - a);
      return t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
    }

    float Clamp(float x, float a, float b)
    {
      return x < a ? a : (x > b ? b : x);
    }

    float ToDb(double power)
    {
      return static_cast<float>(10.0 * log10(power + 1e-12));
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

    // Mixes the channels and resamples to 16 kHz. The filter is a sinc with
    // a Hann window, 12 zero crossings a side, one table row per phase.
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
      int half = static_cast<int>(ceil(12.0 / (2.0 * cutoff)));
      int taps = 2 * half + 1;
      std::vector<float> kernel(static_cast<size_t>(phases) * taps);
      for (int p = 0; p < phases; p++)
      {
        double frac = static_cast<double>(p) / phases;
        double sum = 0.0;
        std::vector<double> row(taps);
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
          if (channels == 1)
          {
            short const* x = pcm + (first + a);
            float const* h = k + a;
            int length = b - a;
            int j = 0;
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
            float const* h = k + a;
            int length = b - a;
            for (int j = 0; j < length; j++)
            {
              s0 += h[j] * static_cast<float>(x[j * 2]);
              s1 += h[j] * static_cast<float>(x[j * 2 + 1]);
            }
          }
        }
        float value = channels == 2 ? s0 + static_cast<float>(rightSign) * s1 : s0 + s1;
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

    // Second order Butterworth high-pass: removes a DC offset and rumble.
    // It starts as if the first sample's value had been there for ever, so a
    // DC offset makes no click at the start.
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
        x2 = x1;
        x1 = v;
        y2 = y1;
        y1 = y;
        x[i] = static_cast<float>(y);
      }
    }

    // A 4 kHz copy (low-pass at 1.6 kHz, every fourth sample) for the
    // voicing measure.
    void Decimate4(std::vector<float> const& x, std::vector<float>& out)
    {
      const int half = 24;
      float taps[2 * half + 1];
      double sum = 0.0;
      double row[2 * half + 1];
      for (int j = -half; j <= half; j++)
      {
        double a = 2.0 * kPi * 0.1 * j;
        double s = j == 0 ? 1.0 : sin(a) / a;
        double w = 0.5 + 0.5 * cos(kPi * j / (half + 1.0));
        row[j + half] = s * w;
        sum += row[j + half];
      }
      for (int j = 0; j <= 2 * half; j++)
      {
        taps[j] = static_cast<float>(row[j] / sum);
      }
      size_t count = x.size() / 4;
      out.resize(count);
      long long size = static_cast<long long>(x.size());
      for (size_t m = 0; m < count; m++)
      {
        long long first = static_cast<long long>(m) * 4 - half;
        int a = first < 0 ? static_cast<int>(-first) : 0;
        long long room = size - first;
        int b = room < 2 * half + 1 ? static_cast<int>(room) : 2 * half + 1;
        float s = 0.0f;
        for (int j = a; j < b; j++)
        {
          s += taps[j] * x[static_cast<size_t>(first + j)];
        }
        out[m] = s;
      }
    }

    // Complex radix-2 FFT of a fixed size with its tables made once.
    class Fft
    {
    public:
      explicit Fft(int size) : Size(size), Reverse(size), Cos(size / 2), Sin(size / 2)
      {
        int bits = 0;
        while ((1 << bits) < size)
        {
          bits++;
        }
        for (int i = 0; i < size; i++)
        {
          int r = 0;
          for (int b = 0; b < bits; b++)
          {
            if (i & (1 << b))
            {
              r |= 1 << (bits - 1 - b);
            }
          }
          Reverse[i] = r;
        }
        for (int i = 0; i < size / 2; i++)
        {
          Cos[i] = static_cast<float>(cos(2.0 * kPi * i / size));
          Sin[i] = static_cast<float>(-sin(2.0 * kPi * i / size));
        }
      }

      void Forward(float* re, float* im) const
      {
        for (int i = 0; i < Size; i++)
        {
          int j = Reverse[i];
          if (i < j)
          {
            std::swap(re[i], re[j]);
            std::swap(im[i], im[j]);
          }
        }
        for (int len = 2; len <= Size; len <<= 1)
        {
          int halfLen = len / 2;
          int stride = Size / len;
          for (int i = 0; i < Size; i += len)
          {
            for (int k = 0; k < halfLen; k++)
            {
              float wr = Cos[k * stride];
              float wi = Sin[k * stride];
              float xr = re[i + k + halfLen];
              float xi = im[i + k + halfLen];
              float vr = xr * wr - xi * wi;
              float vi = xr * wi + xi * wr;
              float ur = re[i + k];
              float ui = im[i + k];
              re[i + k] = ur + vr;
              im[i + k] = ui + vi;
              re[i + k + halfLen] = ur - vr;
              im[i + k + halfLen] = ui - vi;
            }
          }
        }
      }

    private:
      int Size;
      std::vector<int> Reverse;
      std::vector<float> Cos;
      std::vector<float> Sin;
    };

    // Per-frame measurements. Levels are in dB: absolute after Measure,
    // relative to the speech level after Normalise.
    struct Measures
    {
      size_t Count = 0;
      std::vector<float> Tot;     // 100 - 7600 Hz
      std::vector<float> Lo;      // 100 - 500
      std::vector<float> F1;      // 500 - 1300
      std::vector<float> F1w;     // 330 - 1300
      std::vector<float> Lm;      // 600 - 1700
      std::vector<float> F2;      // 1300 - 3300
      std::vector<float> Fr;      // 1850 - 3300
      std::vector<float> LoB;     // 100 - 1300
      std::vector<float> Hf;      // 3300 - 7600
      std::vector<float> HfT;     // 2600 - 7600
      std::vector<float> Hn;      // 2600 - 3800
      std::vector<float> Hs;      // 3800 - 7600
      std::vector<float> Top;     // 4500 - 7600
      std::vector<float> Ncc;     // voicing, 0 to 1
      std::vector<float> Purity;  // share of the energy in one spectral line
      std::vector<float> Cog;     // centre of the 700 - 2900 Hz energy, Hz
    };

    void Measure(std::vector<float> const& x, std::vector<float> const& x4, Measures& m)
    {
      size_t count = x.size() / kHop + 1;
      m.Count = count;
      std::vector<float>* all[] =
      {
        &m.Tot, &m.Lo, &m.F1, &m.F1w, &m.Lm, &m.F2, &m.Fr, &m.LoB, &m.Hf, &m.HfT, &m.Hn, &m.Hs, &m.Top, &m.Ncc,
        &m.Purity, &m.Cog
      };
      for (std::vector<float>* v : all)
      {
        v->assign(count, 0.0f);
      }

      float window[kWindow];
      double windowPower = 0.0;
      for (int i = 0; i < kWindow; i++)
      {
        window[i] = static_cast<float>(0.5 - 0.5 * cos(2.0 * kPi * (i + 0.5) / kWindow));
        windowPower += static_cast<double>(window[i]) * window[i];
      }
      // With this factor a band sum is the band's mean square: a full-scale
      // sine reads -3 dB.
      const double norm = 1.0 / (0.5 * kFft * windowPower);

      int bandFirst[kBandCount];
      int bandLast[kBandCount];
      const double binHz = static_cast<double>(kRate) / kFft;
      for (int b = 0; b < kBandCount; b++)
      {
        bandFirst[b] = static_cast<int>(ceil(kBandEdges[b] / binHz));
        bandLast[b] = static_cast<int>(ceil(kBandEdges[b + 1] / binHz)) - 1;
      }
      const int firstBin = bandFirst[0];
      const int lastBin = bandLast[kBandCount - 1];
      const int cogFirst = static_cast<int>(ceil(700.0 / binHz));
      const int cogLast = static_cast<int>(ceil(2900.0 / binHz)) - 1;

      Fft fft(kFft);
      float re[kFft];
      float im[kFft];
      float power[kFft / 2 + 1];
      const long long size = static_cast<long long>(x.size());
      const long long size4 = static_cast<long long>(x4.size());

      for (size_t k = 0; k < count; k++)
      {
        long long centre = static_cast<long long>(k) * kHop;
        long long first = centre - kWindow / 2;
        for (int i = 0; i < kFft; i++)
        {
          re[i] = 0.0f;
          im[i] = 0.0f;
        }
        int a = first < 0 ? static_cast<int>(-first) : 0;
        long long room = size - first;
        int b = room < kWindow ? static_cast<int>(room) : kWindow;
        for (int i = a; i < b; i++)
        {
          re[i] = x[static_cast<size_t>(first + i)] * window[i];
        }
        fft.Forward(re, im);
        for (int i = firstBin; i <= lastBin; i++)
        {
          power[i] = re[i] * re[i] + im[i] * im[i];
        }

        double band[kBandCount];
        double total = 0.0;
        int peakBin = firstBin;
        for (int n = 0; n < kBandCount; n++)
        {
          double s = 0.0;
          for (int i = bandFirst[n]; i <= bandLast[n]; i++)
          {
            s += power[i];
            if (power[i] > power[peakBin])
            {
              peakBin = i;
            }
          }
          band[n] = s * norm;
          total += band[n];
        }
        double peak = 0.0;
        for (int i = (std::max)(firstBin, peakBin - 3); i <= (std::min)(lastBin, peakBin + 3); i++)
        {
          peak += power[i];
        }
        double cogSum = 0.0;
        double cogWeight = 0.0;
        for (int i = cogFirst; i <= cogLast; i++)
        {
          cogSum += power[i] * (i * binHz);
          cogWeight += power[i];
        }

        m.Tot[k] = ToDb(total);
        m.Lo[k] = ToDb(band[0] + band[1]);
        m.F1[k] = ToDb(band[2] + band[3]);
        m.F1w[k] = ToDb(band[1] + band[2] + band[3]);
        m.Lm[k] = ToDb(band[3] + band[4]);
        m.F2[k] = ToDb(band[4] + band[5] + band[6] + band[7]);
        m.Fr[k] = ToDb(band[6] + band[7]);
        m.LoB[k] = ToDb(band[0] + band[1] + band[2] + band[3]);
        m.Hf[k] = ToDb(band[8] + band[9] + band[10]);
        m.HfT[k] = ToDb(band[7] + band[8] + band[9] + band[10]);
        m.Hn[k] = ToDb(band[7] + band[8]);
        m.Hs[k] = ToDb(band[9] + band[10]);
        m.Top[k] = ToDb(band[10]);
        m.Purity[k] = total > 0.0 ? static_cast<float>(peak * norm / total) : 0.0f;
        m.Cog[k] = cogWeight > 0.0 ? static_cast<float>(cogSum / cogWeight) : 0.0f;

        // Voicing: two 30 ms stretches of the 4 kHz copy, one pitch period
        // apart, are nearly the same in voiced speech. Lags cover 70 - 400 Hz.
        long long c4 = static_cast<long long>(k) * (kHop / 4);
        const int span = 120;
        const int minLag = 10;
        const int maxLag = 57;
        long long lo = c4 - span / 2 - maxLag / 2;
        long long hi = lo + maxLag + span;
        float best = 0.0f;
        if (lo >= 0 && hi <= size4)
        {
          float const* p = &x4[static_cast<size_t>(lo)];
          double sq[maxLag + span + 1];
          sq[0] = 0.0;
          for (int i = 0; i < maxLag + span; i++)
          {
            sq[i + 1] = sq[i] + static_cast<double>(p[i]) * p[i];
          }
          for (int lag = minLag; lag <= maxLag; lag++)
          {
            int a0 = maxLag / 2 - lag / 2;
            int b0 = a0 + lag;
            float ab = 0.0f;
            for (int i = 0; i < span; i++)
            {
              ab += p[a0 + i] * p[b0 + i];
            }
            double aa = sq[a0 + span] - sq[a0];
            double bb = sq[b0 + span] - sq[b0];
            float n = static_cast<float>(ab / sqrt(aa * bb + 1e-14));
            if (n > best)
            {
              best = n;
            }
          }
        }
        m.Ncc[k] = best > 1.0f ? 1.0f : best;
      }
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
        for (int i = -half; i <= half; i++)
        {
          long long j = k + i;
          weight += w[i + half];
          if (j >= 0 && j < size)
          {
            sum += w[i + half] * x[static_cast<size_t>(j)];
          }
        }
        out[static_cast<size_t>(k)] = sum / weight;
      }
      x.swap(out);
    }

    // Zero-phase Gaussian smoothing, sigma in ms (a frame is 10 ms), cut at 3.5 sigma. Outside the sound the curve
    // counts as zero, and the sum is divided by the whole kernel's weight (as Smooth does).
    void SmoothGauss(std::vector<float>& x, double sigmaMs)
    {
      double s = sigmaMs / 10.0;
      int half = static_cast<int>(ceil(3.5 * s));
      std::vector<double> w(2 * half + 1);
      double total = 0.0;
      for (int i = -half; i <= half; i++)
      {
        w[i + half] = exp(-0.5 * (i / s) * (i / s));
        total += w[i + half];
      }
      std::vector<float> out(x.size());
      long long size = static_cast<long long>(x.size());
      for (long long k = 0; k < size; k++)
      {
        double sum = 0.0;
        for (int i = -half; i <= half; i++)
        {
          long long j = k + i;
          if (j >= 0 && j < size)
          {
            sum += w[i + half] * x[static_cast<size_t>(j)];
          }
        }
        out[static_cast<size_t>(k)] = static_cast<float>(sum / total);
      }
      x.swap(out);
    }

    // Penalised least squares (Whittaker - Henderson, second differences): the y that makes
    // sum weight (y - x)^2 + lambda sum (y[k-1] - 2 y[k] + y[k+1])^2 least, solved at once over the whole curve
    // (zero phase) with a banded Cholesky. The curve rests at zero outside the sound: two frames of zero are added
    // at each end and held there. The weights must be positive (the matrix is then positive definite).
    void Whittaker2(std::vector<float>& x, std::vector<double> const& weight, double lambda)
    {
      const size_t pad = 2;
      size_t n0 = x.size();
      size_t n = n0 + 2 * pad;
      std::vector<double> w(n), b(n, 0.0), a0(n, 0.0), a1(n, 0.0), a2(n, 0.0);
      for (size_t i = 0; i < n; i++)
      {
        w[i] = (i < pad || i >= n0 + pad) ? 1e6 : weight[i - pad];
        b[i] = (i < pad || i >= n0 + pad) ? 0.0 : w[i] * x[i - pad];
      }
      const double c[3] = { 1.0, -2.0, 1.0 };
      for (size_t r = 0; r + 2 < n; r++)
      {
        for (int p = 0; p < 3; p++)
        {
          for (int q = p; q < 3; q++)
          {
            double v = lambda * c[p] * c[q];
            size_t i = r + static_cast<size_t>(p);
            if (q == p)
            {
              a0[i] += v;
            }
            else if (q == p + 1)
            {
              a1[i] += v;
            }
            else
            {
              a2[i] += v;
            }
          }
        }
      }
      for (size_t i = 0; i < n; i++)
      {
        a0[i] += w[i];
      }
      // A = L L': l0[i] = L(i, i), l1[i] = L(i, i - 1), l2[i] = L(i, i - 2); A(i, i - 1) = a1[i - 1], A(i, i - 2) = a2[i - 2]
      std::vector<double> l0(n, 0.0), l1(n, 0.0), l2(n, 0.0);
      for (size_t i = 0; i < n; i++)
      {
        if (i >= 2)
        {
          l2[i] = a2[i - 2] / l0[i - 2];
        }
        if (i >= 1)
        {
          l1[i] = (a1[i - 1] - (i >= 2 ? l2[i] * l1[i - 1] : 0.0)) / l0[i - 1];
        }
        l0[i] = sqrt(a0[i] - l2[i] * l2[i] - l1[i] * l1[i]);
      }
      std::vector<double> z(n, 0.0), y(n, 0.0);
      for (size_t i = 0; i < n; i++)
      {
        double s = b[i];
        if (i >= 1)
        {
          s -= l1[i] * z[i - 1];
        }
        if (i >= 2)
        {
          s -= l2[i] * z[i - 2];
        }
        z[i] = s / l0[i];
      }
      for (size_t i = n; i-- > 0;)
      {
        double s = z[i];
        if (i + 1 < n)
        {
          s -= l1[i + 1] * y[i + 1];
        }
        if (i + 2 < n)
        {
          s -= l2[i + 2] * y[i + 2];
        }
        y[i] = s / l0[i];
      }
      for (size_t i = 0; i < n0; i++)
      {
        x[i] = static_cast<float>(y[i + pad]);
      }
    }

    // ---- The three smoothings of the raw curves ----
    // The menu's "Mouth smoothing" (GAME > Pose window > Voice) goes from one to the next: 0 Before, 1 Eased (the
    // default), 2 Softer; between two of them Voice.cpp's MouthAt mixes them. The numbers below are from the
    // 2026-11-14 study (Research_Pose_Editor section 33), measured offline on 31 recorded voices (the user's 4 files,
    // EmoDB and RAVDESS), the jaw as VoiceFace turns it at Mouth opening 1: its speed's 95th percentile and maximum.

    // As up to 2026-11-13 (146 and 368 degrees a second): symmetric Hann kernels, no lag, attack equals release. A
    // step goes from 10 % to 90 % in about 40 ms for the teeth (70 ms kernel), 50 ms for Wide and Round (90 ms
    // kernel) and 35 ms for the jaw (its own smoothing, just below). The closures are multiplied in after the
    // smoothing, at the closures' own speed.
    void SmoothBefore(std::vector<float>& open, std::vector<float>& wide, std::vector<float>& round,
      std::vector<float>& teeth, std::vector<float>& closed, std::vector<float>& presence)
    {
      long long count = static_cast<long long>(open.size());
      {
        // The jaw curve is the lower of a 70 ms and a 30 ms smoothing, so that
        // a short closure between two vowels (an m or a b in running speech)
        // shows. The 70 ms kernel alone fills such a dip in: a 30 ms dip from 1
        // down to 0 only gets down to about 0.3 with it, and to about 0.1 this
        // way. A short peak is still rounded off. 30 ms more takes the corners
        // off.
        std::vector<float> quick(open);
        Smooth(open, 7);
        Smooth(quick, 3);
        for (long long k = 0; k < count; k++)
        {
          open[k] = (std::min)(open[k], quick[k]);
        }
        Smooth(open, 3);
      }
      Smooth(wide, 9);
      Smooth(round, 9);
      Smooth(teeth, 7);
      Smooth(closed, 3);
      Smooth(presence, 5);
      for (long long k = 0; k < count; k++)
      {
        // During any closure (lips or tongue) the vowel shapes fade and the
        // jaw shuts by 80 % (less where the closure is less sure). A voiced
        // closure (m, n, ng) still has sound in it, so the jaw curve does not
        // fall by itself there: this is what shuts it. Pressed lips shut the
        // jaw curve fully.
        float rest = 1.0f - 0.8f * presence[k];
        wide[k] *= rest;
        round[k] *= rest;
        open[k] *= 1.0f - 0.8f * presence[k];
        open[k] *= 1.0f - closed[k];
        teeth[k] *= 1.0f - closed[k];
      }
    }

    // Eased and Softer: the closures go into the raw jaw first (shut by 80 % in any closure, fully where the lips
    // press), and the whole jaw is then smoothed by Whittaker2, its frames held to the raw curve by their weight:
    // 1 + ClosureWeight x the closure's amount, + DipWeight x (0.1 - the jaw as smoothed before) / 0.1 where that jaw
    // is under 0.1 (the whole DipWeight where it was shut). So the jaw still gets down almost everywhere it got down
    // before (p / b / m, the dips between syllables, pauses), and moves there smoothly. At 1 it gets under 0.1 again in
    // every such dip of 40 ms or more; of those of 30 ms or less it stays above 0.1 (a median 0.12 - 0.17) in 9 of 34
    // on the 31 recorded voices, 25 of 127 in fast synthetic speech; at 2 in more (12 of 34, 46 of 127, and 1 of 668
    // longer ones in slow speech). Wide, Round, Teeth, Closed and the closures' presence: Gaussians.
    struct Easing
    {
      double Lambda;            // Whittaker2's: how stiff the jaw is
      double ClosureWeight;
      double DipWeight;
      double WideRound, Teeth, Closed, Presence;   // Gaussians' sigma, ms
    };
    // 1 (106 and 225 degrees a second; the jaw's moves of 0.3 or more from 10 to 90 % in a median 59 ms, before
    // 43 - 49). On the synthetic test voices p / b / m are seen as shut about as often as before (92 against 91 % on
    // clean ones, 82 against 84 % in fast speech), but fewer vowels open the jaw to 0.2: 87 against 91 % of them on
    // clean voices, 75 against 84 % in fast speech.
    const Easing kEased = { 60.0, 100.0, 30.0, 35.0, 30.0, 20.0, 15.0 };
    // 2 (85 and 149 degrees a second, a median 70 ms): the closures held less hard, so the jaw also eases into them;
    // p / b / m seen as shut a little less often (89 % on clean voices, 80 % in fast speech), and 81 % of the vowels
    // on clean voices, 65 % in fast speech, open the jaw to 0.2.
    const Easing kSofter = { 120.0, 30.0, 30.0, 45.0, 40.0, 25.0, 20.0 };

    float Finite(float x)
    {
      return std::isfinite(x) ? x : 0.0f;
    }

    // jawBefore: SmoothBefore's jaw curve for the same sound (its dips hold this one down)
    void SmoothEased(std::vector<float>& open, std::vector<float>& wide, std::vector<float>& round,
      std::vector<float>& teeth, std::vector<float>& closed, std::vector<float>& presence,
      std::vector<float> const& jawBefore, Easing const& e)
    {
      size_t n = open.size();
      // (a value that isn't a number would spread through the whole jaw in Whittaker2)
      for (std::vector<float>* curve : { &open, &wide, &round, &teeth, &closed, &presence })
      {
        for (float& v : *curve)
        {
          v = Finite(v);
        }
      }
      std::vector<float> jaw(n);
      std::vector<double> weight(n);
      for (size_t k = 0; k < n; k++)
      {
        float before = Clamp(Finite(jawBefore[k]), 0.0f, 1.0f);
        jaw[k] = open[k] * (1.0f - 0.8f * presence[k]) * (1.0f - closed[k]);
        weight[k] = 1.0 + e.ClosureWeight * (std::max)(presence[k], closed[k]) +
          e.DipWeight * Clamp((0.1f - before) / 0.1f, 0.0f, 1.0f);
      }
      Whittaker2(jaw, weight, e.Lambda);
      SmoothGauss(closed, e.Closed);
      SmoothGauss(presence, e.Presence);
      SmoothGauss(wide, e.WideRound);
      SmoothGauss(round, e.WideRound);
      SmoothGauss(teeth, e.Teeth);
      for (size_t k = 0; k < n; k++)
      {
        float rest = 1.0f - 0.8f * presence[k];
        wide[k] *= rest;
        round[k] *= rest;
        teeth[k] *= 1.0f - closed[k];
      }
      open.swap(jaw);
    }

    // The curves into frames, kLeadFrames ahead of the sound, each from 0 to 1
    void Store(std::vector<float> const& open, std::vector<float> const& wide, std::vector<float> const& round,
      std::vector<float> const& closed, std::vector<float> const& teeth, std::vector<Frame>& frames)
    {
      long long count = static_cast<long long>(frames.size());
      for (long long k = 0; k < count; k++)
      {
        long long from = (std::min)(k + kLeadFrames, count - 1);
        Frame& f = frames[static_cast<size_t>(k)];
        f.Open = Clamp(open[from], 0.0f, 1.0f);
        f.Wide = Clamp(wide[from], 0.0f, 1.0f);
        f.Round = Clamp(round[from], 0.0f, 1.0f);
        f.Closed = Clamp(closed[from], 0.0f, 1.0f);
        f.Teeth = Clamp(teeth[from], 0.0f, 1.0f);
      }
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

    // One closure candidate: a short stretch with almost nothing above
    // 500 Hz, between or beside louder sounds.
    struct Closure
    {
      long long First = 0;   // first frame
      long long End = 0;     // one past the last frame
      float Amount = 0.0f;   // lip evidence, 0 to 1
      float Presence = 0.0f; // how surely it is a closure of any kind
    };
  }

  bool Analyse(short const* pcm, size_t samples, int rate, int channels, Track& out, std::string& error)
  {
    return Analyse(pcm, samples, rate, channels, out, error, nullptr);
  }

  namespace
  {
    bool AnalyseInner(short const* pcm, size_t samples, int rate, int channels, Track& out, std::string& error,
      Debug* debug);
  }

  bool Analyse(short const* pcm, size_t samples, int rate, int channels, Track& out, std::string& error, Debug* debug)
  {
    // Running out of memory is not thrown at the caller: Analyse returns
    // false, with the reason in error and an empty track. (The work takes
    // about 5 MB of memory for each minute of sound.)
    try
    {
      return AnalyseInner(pcm, samples, rate, channels, out, error, debug);
    }
    catch (std::bad_alloc const&)
    {
      out.Frames.clear();
      out.Before.clear();
      out.Softer.clear();
      out.Report.clear();
      error = "Not enough memory to analyse this file.";
      return false;
    }
  }

  namespace
  {
    bool AnalyseInner(short const* pcm, size_t samples, int rate, int channels, Track& out, std::string& error,
      Debug* debug)
    {
      out.FramesPerSecond = 100.0;
      out.Frames.clear();
      out.Before.clear();
      out.Softer.clear();
      out.Report.clear();
      error.clear();
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
      std::vector<float> x;
      ToInternalRate(pcm, samples, rate, channels, rightSign, x);
      HighPass(x, 70.0);
      std::vector<float> x4;
      Decimate4(x, x4);

      Measures m;
      Measure(x, x4, m);
      x.clear();
      x.shrink_to_fit();
      x4.clear();
      x4.shrink_to_fit();
      const long long count = static_cast<long long>(m.Count);
      Frame zero = { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f };
      out.Frames.assign(m.Count, zero);
      out.Before.assign(m.Count, zero);
      out.Softer.assign(m.Count, zero);
      AddLine(out.Report, "seconds", "%.2f", static_cast<double>(samples) / rate);
      AddLine(out.Report, "frames", "%.0f", static_cast<double>(m.Count));

      // ---- The recording's own speech level ----
      // Voiced frames that are not a pure tone are taken as speech; their 90th
      // percentile is the level of the vowel peaks. With no voiced frames (a
      // whisper, or no speech at all) the loudest frames stand in.
      const float absoluteFloor = -75.0f;
      std::vector<float> pool;
      pool.reserve(m.Count);
      for (long long k = 0; k < count; k++)
      {
        if (m.Ncc[k] > 0.6f && m.Purity[k] < 0.99f && m.Tot[k] > absoluteFloor)
        {
          pool.push_back(m.Tot[k]);
        }
      }
      bool voicedSpeech = pool.size() >= 10;
      float global = 0.0f;
      if (voicedSpeech)
      {
        global = Percentile(pool, 0.90);
      }
      else
      {
        pool.clear();
        for (long long k = 0; k < count; k++)
        {
          if (m.Purity[k] < 0.99f && m.Tot[k] > absoluteFloor)
          {
            pool.push_back(m.Tot[k]);
          }
        }
        if (pool.size() < 10)
        {
          out.Report += "speech: none (silence, or a steady tone)\n";
          return true;
        }
        global = Percentile(pool, 0.95);
      }
      AddLine(out.Report, "speech level dBFS", "%.1f", global);
      out.Report += voicedSpeech ? "voiced speech: yes\n" :
        "voiced speech: no (whisper or noise: the mouth moves less)\n";

      // In a long file the level may drift: each second gets the percentile of
      // the 30 s around it, kept within 12 dB of the whole file's.
      std::vector<float> ref(m.Count, global);
      if (voicedSpeech && count > 3000)
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
            if (m.Ncc[k] > 0.6f && m.Purity[k] < 0.99f && m.Tot[k] > absoluteFloor)
            {
              pool.push_back(m.Tot[k]);
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

      // Is there anything above 4.5 kHz (a telephone-band file has nothing)?
      double sumTop = 0.0;
      double sumAll = 0.0;
      for (long long k = 0; k < count; k++)
      {
        if (m.Tot[k] > ref[k] - 30.0f)
        {
          sumTop += pow(10.0, m.Top[k] / 10.0);
          sumAll += pow(10.0, m.Tot[k] / 10.0);
        }
      }
      bool narrow = sumAll <= 0.0 || sumTop < sumAll * 3.2e-5;
      out.Report += narrow ? "band: narrow (nothing above 4.5 kHz)\n" : "band: wide\n";

      std::vector<float>* levels[] =
      {
        &m.Tot, &m.Lo, &m.F1, &m.F1w, &m.Lm, &m.F2, &m.Fr, &m.LoB, &m.Hf, &m.HfT, &m.Hn, &m.Hs, &m.Top
      };
      for (std::vector<float>* v : levels)
      {
        for (long long k = 0; k < count; k++)
        {
          (*v)[k] = (std::max)((*v)[k] - ref[k], -100.0f);
        }
      }
      if (narrow)
      {
        // The hiss of s / sh is only seen up to 3.8 kHz: lift it to stand in
        // for the missing band.
        for (long long k = 0; k < count; k++)
        {
          m.Hf[k] = m.Hn[k] + 8.0f;
          m.HfT[k] = m.Hn[k] + 8.0f;
        }
      }

      // ---- The floor ----
      // The quiet 5 % of the frames give the recording's floor. A sound that
      // stays within 6 dB of its own floor has no speech rhythm (steady noise,
      // a hum, a buzz): the mouth stays shut. Otherwise a frame counts from
      // 4 dB above the floor, fully from 10 dB above, and never below 50 dB
      // under the speech level.
      pool.assign(m.Tot.begin(), m.Tot.end());
      float floorLevel = Percentile(pool, 0.05);
      AddLine(out.Report, "floor dB below speech", "%.1f", -floorLevel);
      if (floorLevel > -6.0f)
      {
        out.Report += "speech: none (a steady sound with no pauses)\n";
        return true;
      }
      float gate = (std::max)(floorLevel + 4.0f, -50.0f);
      std::vector<float> active(m.Count);
      std::vector<float> voiced(m.Count);
      long long activeFrames = 0;
      for (long long k = 0; k < count; k++)
      {
        active[k] = Ramp(m.Tot[k], gate, gate + 6.0f) * (1.0f - Ramp(m.Purity[k], 0.990f, 0.997f));
        voiced[k] = Ramp(m.Ncc[k], 0.35f, 0.65f);
        if (active[k] > 0.5f)
        {
          activeFrames++;
        }
      }
      AddLine(out.Report, "sound share %", "%.1f",
        100.0 * static_cast<double>(activeFrames) / static_cast<double>(count));

      // ---- Raw shapes, frame by frame ----
      std::vector<float> open(m.Count);
      std::vector<float> wide(m.Count);
      std::vector<float> round(m.Count);
      std::vector<float> teeth(m.Count);
      std::vector<float> closed(m.Count, 0.0f);
      for (long long k = 0; k < count; k++)
      {
        float a = active[k];
        float v = voiced[k];
        // Jaw: the level where an open vowel's first formant sits. A close
        // vowel, a nasal, a hiss leave this band nearly empty.
        open[k] = a * static_cast<float>(pow(Ramp(m.F1[k], -36.0f, -4.0f), 1.3)) * (0.35f + 0.65f * v);
        // How far the 100 - 500 Hz band stands above the 500 - 1300 Hz band:
        // high for a close vowel (ee, oo), below zero for an open one (ah).
        float low = m.Lo[k] - m.F1[k];
        // Wide: the 1850 - 3300 Hz band (second and third formant of a front
        // vowel) stands above the 600 - 1700 Hz band.
        wide[k] = a * v * Ramp(m.Tot[k], -30.0f, -15.0f) * Ramp(m.Fr[k] - m.Lm[k], -12.0f, 8.0f);
        // Round: nearly everything sits below 1300 Hz (both formants low). An
        // open "ah" has both formants under 1300 Hz too, but there the lower
        // band is the weaker one, so Round is kept off it: the last factor
        // fades Round out as the 100 - 500 Hz band goes from level with the
        // 500 - 1300 Hz band to 6 dB under it.
        round[k] = a * v * Ramp(m.F1w[k], -18.0f, -8.0f) * Ramp(m.LoB[k] - m.F2[k], 10.0f, 24.0f) *
          Ramp(low, -6.0f, 0.0f);
        // Teeth: noise above 2.6 kHz stronger than everything below 1.3 kHz.
        float t = a * Ramp(m.Tot[k], -40.0f, -30.0f) * Ramp(m.HfT[k] - m.LoB[k], -3.0f, 9.0f);
        // An f or a th is weak noise with a flat spectrum: half a shape at
        // most, because breath and the puff after p / t / k look the same.
        float soft = 0.5f * a * (1.0f - v) * Ramp(m.Tot[k], -42.0f, -32.0f) *
          Ramp(m.HfT[k] - m.LoB[k], -10.0f, -2.0f) * (1.0f - Ramp(m.F1[k], -30.0f, -20.0f));
        teeth[k] = (std::max)(t, soft);
        if (!narrow)
        {
          // An s is brightest above 3.8 kHz and is said with the lips spread;
          // an sh is brightest below it and is said with the lips pushed out.
          float bright = Ramp(m.Hs[k] - m.Fr[k], 0.0f, 10.0f);
          wide[k] = (std::max)(wide[k], 0.4f * t * bright);
          round[k] = (std::max)(round[k], 0.5f * t * (1.0f - bright));
        }
      }

      // ---- Lip closures ----
      // A candidate frame has no hiss, has a real sound within 120 ms, and is
      // either next to nothing at all (1: a stop's gap, down at the floor) or
      // a voiced hum with its energy at the bottom (2: m, n, the voice bar of
      // b), which can be nearly as loud as a vowel. Weak noise that is neither
      // (f, th, breath) is not a closure.
      std::vector<unsigned char> candidate(m.Count, 0);
      const float gapLevel = (std::max)(-36.0f, gate + 1.0f);
      for (long long k = 0; k < count; k++)
      {
        float peak = -100.0f;
        long long first = (std::max)(0LL, k - 12);
        long long end = (std::min)(count, k + 13);
        for (long long j = first; j < end; j++)
        {
          peak = (std::max)(peak, m.Tot[j]);
        }
        bool near = peak > -25.0f && m.Hf[k] < -27.0f;
        bool gap = near && m.Tot[k] < gapLevel;
        bool murmur = near && m.Ncc[k] > 0.6f && m.F1[k] < -15.0f && m.F2[k] < -23.0f &&
          m.F2[k] - m.Lo[k] < -12.0f && m.Tot[k] < peak - 1.5f;
        candidate[k] = gap ? 1 : (murmur ? 2 : 0);
      }
      // Silence counts only as a short gap (up to 200 ms) between sounds. In a
      // longer silence the mouth is at rest, which is shut already.
      const float silent = (std::max)(-45.0f, gate);
      for (long long k = 0; k < count;)
      {
        if (m.Tot[k] >= silent)
        {
          k++;
          continue;
        }
        long long end = k;
        while (end < count && m.Tot[end] < silent)
        {
          end++;
        }
        long long keepFrom = k;
        if (end == count || k == 0 || end - k > 20)
        {
          keepFrom = end;
        }
        for (long long j = k; j < keepFrom; j++)
        {
          candidate[j] = 0;
        }
        k = end;
      }

      std::vector<Closure> closures;
      for (long long k = 0; k < count;)
      {
        if (!candidate[k])
        {
          k++;
          continue;
        }
        long long end = k;
        while (end < count && candidate[end])
        {
          end++;
        }
        if (end - k >= 2)
        {
          Closure c;
          c.First = k;
          c.End = end;
          closures.push_back(c);
        }
        k = end;
      }

      std::vector<float> presence(m.Count, 0.0f);
      int closureKinds[3] = { 0, 0, 0 };
      int closuresPressed = 0;
      for (Closure& c : closures)
      {
        long long r0 = c.First;
        long long r1 = c.End;
        float duration = static_cast<float>(r1 - r0);
        float deepest = 0.0f;
        int gapFrames = 0;
        for (long long k = r0; k < r1; k++)
        {
          gapFrames += candidate[k] == 1 ? 1 : 0;
          deepest = (std::min)(deepest, m.Tot[k]);
        }
        // Two near-silent frames make it a stop's gap (0); a voiced stretch
        // that drops 26 dB is the weak voice bar of b / d / g (1); otherwise
        // it is a loud hum (2).
        int kind = gapFrames >= 2 ? 0 : (deepest < -26.0f ? 1 : 2);
        closureKinds[kind]++;

        // A voiced sound right before and right after: where its 700 - 2900 Hz
        // energy is centred at the edge of the closure, and for the sound
        // after, how far that is from where it is 30 ms later.
        bool before = false;
        bool after = false;
        float cogBefore = 0.0f;
        float cogAfter = 0.0f;
        float cogAfterMove = 0.0f;
        if (r0 >= 5)
        {
          float loud = (std::max)((std::max)(m.Tot[r0 - 1], m.Tot[r0 - 2]), m.Tot[r0 - 3]);
          float pitch = (std::max)((std::max)(m.Ncc[r0 - 1], m.Ncc[r0 - 2]), m.Ncc[r0 - 3]);
          if (loud > -20.0f && pitch > 0.6f)
          {
            before = true;
            cogBefore = 0.5f * (m.Cog[r0 - 1] + m.Cog[r0 - 2]);
          }
        }
        if (r1 + 5 < count)
        {
          float loud = -100.0f;
          float pitch = 0.0f;
          for (long long k = r1; k < r1 + 4; k++)
          {
            loud = (std::max)(loud, m.Tot[k]);
            pitch = (std::max)(pitch, m.Ncc[k]);
          }
          if (loud > -20.0f && pitch > 0.6f)
          {
            after = true;
            cogAfter = 0.5f * (m.Cog[r1] + m.Cog[r1 + 1]);
            cogAfterMove = cogAfter - 0.5f * (m.Cog[r1 + 3] + m.Cog[r1 + 4]);
          }
        }
        // Hiss in the 30 ms after the release, against the low band.
        float releaseHiss = -100.0f;
        for (long long k = r1; k < r1 + 3 && k < count; k++)
        {
          releaseHiss = (std::max)(releaseHiss, m.Hf[k] - m.Lo[k]);
        }
        // How sharply the formant bands (500 - 3300 Hz) drop into the closure
        // and come back out of it, over 20 ms.
        auto formants = [&m, count](long long k)
        {
          if (k < 0 || k >= count)
          {
            return -100.0f;
          }
          return ToDb(pow(10.0, m.F1[k] / 10.0) + pow(10.0, m.F2[k] / 10.0));
        };
        float edgeIn = formants(r0 - 2) - formants(r0);
        float edgeOut = formants(r1 + 1) - formants(r1 - 1);

        // The lip evidence. Every closure of the mouth (p b m, t d n, k g)
        // looks alike in the level; what points at the lips is indirect. The
        // weights are a logistic fit on the tuning recordings (two synthetic
        // voices), one fit per kind of closure. What they say:
        //   hiss right after the release is a t, an s or a ch, not the lips;
        //   lips pull the formants down, so the neighbouring vowel's energy
        //   sits lower at the edge of the closure;
        //   a sharp-edged, longer closure is a real stop, not a tap.
        // The term on how the energy centre moves after the release came out
        // of the fit; it has no simple reason behind it.
        float hissTerm = Clamp((releaseHiss + 10.0f) / 15.0f, -2.0f, 2.0f);
        float beforeTerm = before ? Clamp((cogBefore - 1450.0f) / 400.0f, -2.0f, 2.0f) : 0.0f;
        float afterTerm = after ? Clamp((cogAfter - 1500.0f) / 400.0f, -2.0f, 2.0f) : 0.0f;
        float moveTerm = after ? Clamp(cogAfterMove / 280.0f, -2.0f, 2.0f) : 0.0f;
        float lengthTerm = Clamp((duration - 5.0f) / 3.0f, -2.0f, 2.0f);
        float inTerm = Clamp((edgeIn - 10.0f) / 13.0f, -2.0f, 2.0f);
        float outTerm = Clamp((edgeOut - 11.0f) / 16.0f, -2.0f, 2.0f);
        float sharp = (std::max)(edgeIn, edgeOut);
        float z = 0.0f;
        if (kind == 0)
        {
          // A stop's silent gap: p / b against t / k. This works fairly well.
          z = -0.38f - 1.10f * hissTerm - 0.63f * beforeTerm - 0.10f * afterTerm + 0.65f * moveTerm +
            0.57f * lengthTerm + 1.01f * inTerm;
          c.Amount = 1.0f / (1.0f + static_cast<float>(exp(-2.5 * (z + 0.125))));
          c.Presence = 1.0f;
        }
        else if (kind == 1)
        {
          // A voiced stop with no silent gap: b against d / g. Works less well.
          z = -1.02f - 0.42f * beforeTerm - 0.71f * afterTerm + 0.17f * moveTerm + 0.39f * lengthTerm +
            0.20f * inTerm + 0.59f * outTerm;
          c.Amount = 1.0f / (1.0f + static_cast<float>(exp(-2.5 * (z + 0.375))));
          c.Presence = 1.0f;
        }
        else
        {
          // A loud voiced hum: m against n / ng / v / the edge of a close
          // vowel. The evidence is thin, so most of these stay under one half
          // (the lips come near). A hum with soft edges is a glide or a vowel,
          // not a closure.
          z = -1.38f - 0.60f * afterTerm + 0.22f * moveTerm + 0.23f * lengthTerm;
          c.Presence = Ramp(sharp, 6.0f, 12.0f);
          c.Amount = Ramp(sharp, 4.0f, 8.0f) / (1.0f + static_cast<float>(exp(-2.5 * (z + 0.75))));
        }
        closuresPressed += c.Amount > 0.5f ? 1 : 0;

        if (debug != nullptr)
        {
          char line[512];
          _snprintf_s(line, sizeof(line), _TRUNCATE,
            "%lld %lld kind %d dur %.0f deep %.1f before %d cogb %.0f after %d coga %.0f cogam %.0f "
            "relhisslo %.1f edgein %.1f edgeout %.1f z %.3f amount %.3f\n",
            r0, r1, kind, duration, deepest, before ? 1 : 0, cogBefore, after ? 1 : 0, cogAfter, cogAfterMove,
            releaseHiss, edgeIn, edgeOut, z, c.Amount);
          debug->Closures += line;
        }

        // The lips close over the 30 ms before the closure, hold, and part
        // over the 20 ms after it.
        for (long long k = r0 - 3; k < r1 + 2; k++)
        {
          if (k < 0 || k >= count)
          {
            continue;
          }
          float shape = 1.0f;
          if (k < r0)
          {
            shape = static_cast<float>(k - (r0 - 4)) / 4.0f;
          }
          else if (k >= r1)
          {
            shape = static_cast<float>(r1 + 2 - k) / 3.0f;
          }
          closed[k] = (std::max)(closed[k], c.Amount * shape);
          presence[k] = (std::max)(presence[k], c.Presence * shape);
        }
      }

      // ---- Smoothing and the links between the shapes ----
      // The same raw curves smoothed three ways (see SmoothBefore and Easing above): the menu's Mouth smoothing goes
      // from Before (0) to Frames (1, the default) to Softer (2).
      std::vector<float> jawBefore;
      {
        std::vector<float> o(open), w(wide), r(round), t(teeth), c(closed), p(presence);
        SmoothBefore(o, w, r, t, c, p);
        Store(o, w, r, c, t, out.Before);
        jawBefore.swap(o);
      }
      {
        std::vector<float> o(open), w(wide), r(round), t(teeth), c(closed), p(presence);
        SmoothEased(o, w, r, t, c, p, jawBefore, kSofter);
        Store(o, w, r, c, t, out.Softer);
      }
      SmoothEased(open, wide, round, teeth, closed, presence, jawBefore, kEased);
      Store(open, wide, round, closed, teeth, out.Frames);
      AddLine(out.Report, "closures: stop gaps", "%.0f", closureKinds[0]);
      AddLine(out.Report, "closures: voiced stops", "%.0f", closureKinds[1]);
      AddLine(out.Report, "closures: hums", "%.0f", closureKinds[2]);
      AddLine(out.Report, "closures with Closed over 0.5", "%.0f", closuresPressed);

      if (debug != nullptr)
      {
        debug->Names =
        {
          "tot", "lo", "f1", "f1w", "lm", "f2", "fr", "lob", "hf", "hft", "hn", "hs", "ncc", "purity", "cog",
          "active", "candidate"
        };
        std::vector<float> cand(candidate.begin(), candidate.end());
        debug->Columns =
        {
          m.Tot, m.Lo, m.F1, m.F1w, m.Lm, m.F2, m.Fr, m.LoB, m.Hf, m.HfT, m.Hn, m.Hs, m.Ncc, m.Purity, m.Cog, active,
          cand
        };
      }
      return true;
    }
  }
}
