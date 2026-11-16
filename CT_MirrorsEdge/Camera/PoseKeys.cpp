#include "PoseKeys.h"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <locale.h>
#include <stdarg.h>

// The keyframes' maths, sampler, playhead clock, rebasing and text file. See PoseKeys.h and PLAN.md section 1.
namespace pose_keys
{
  namespace
  {
    bool Finite(double value)
    {
      return std::isfinite(value) != 0;
    }

    // The files must read and write the same in any process locale: every number uses this "C" locale.
    // Made once and never freed.
    _locale_t NumberLocale()
    {
      static _locale_t locale = _create_locale(LC_NUMERIC, "C");
      return locale;
    }

    bool WriteText(FILE* f, char const* format, ...)
    {
      _locale_t locale = NumberLocale();
      char text[2048];
      va_list args;
      va_start(args, format);
      int length = locale ? _vsnprintf_s_l(text, sizeof(text), _TRUNCATE, format, locale, args) :
        _vsnprintf_s(text, sizeof(text), _TRUNCATE, format, args);
      va_end(args);
      if (length < 0 || std::fputs(text, f) == EOF)
        return false;
      return true;
    }

    struct Quat
    {
      double X, Y, Z, W;
    };

    Quat MakeQuat(float const* values)
    {
      Quat q;
      q.X = values[0];
      q.Y = values[1];
      q.Z = values[2];
      q.W = values[3];
      return q;
    }

    void PutQuat(Quat const& q, float* values)
    {
      values[0] = (float)q.X;
      values[1] = (float)q.Y;
      values[2] = (float)q.Z;
      values[3] = (float)q.W;
    }

    double DotQuat(Quat const& a, Quat const& b)
    {
      return a.X * b.X + a.Y * b.Y + a.Z * b.Z + a.W * b.W;
    }

    Quat NegQuat(Quat const& q)
    {
      Quat out;
      out.X = -q.X;
      out.Y = -q.Y;
      out.Z = -q.Z;
      out.W = -q.W;
      return out;
    }

    Quat MulQuat(Quat const& a, Quat const& b)
    {
      Quat out;
      out.X = a.W * b.X + a.X * b.W + a.Y * b.Z - a.Z * b.Y;
      out.Y = a.W * b.Y - a.X * b.Z + a.Y * b.W + a.Z * b.X;
      out.Z = a.W * b.Z + a.X * b.Y - a.Y * b.X + a.Z * b.W;
      out.W = a.W * b.W - a.X * b.X - a.Y * b.Y - a.Z * b.Z;
      return out;
    }

    Quat ConjQuat(Quat const& q)
    {
      Quat out;
      out.X = -q.X;
      out.Y = -q.Y;
      out.Z = -q.Z;
      out.W = q.W;
      return out;
    }

    double QuatMagnitude(Quat const& q)
    {
      double maxAbs = std::fabs(q.X);
      if (std::fabs(q.Y) > maxAbs)
        maxAbs = std::fabs(q.Y);
      if (std::fabs(q.Z) > maxAbs)
        maxAbs = std::fabs(q.Z);
      if (std::fabs(q.W) > maxAbs)
        maxAbs = std::fabs(q.W);
      if (!(maxAbs > 0.0))
        return 0.0;
      double x = q.X / maxAbs;
      double y = q.Y / maxAbs;
      double z = q.Z / maxAbs;
      double w = q.W / maxAbs;
      return maxAbs * std::sqrt(x * x + y * y + z * z + w * w);
    }

    void NormalizeQuat(Quat& q)
    {
      double maxAbs = std::fabs(q.X);
      if (std::fabs(q.Y) > maxAbs)
        maxAbs = std::fabs(q.Y);
      if (std::fabs(q.Z) > maxAbs)
        maxAbs = std::fabs(q.Z);
      if (std::fabs(q.W) > maxAbs)
        maxAbs = std::fabs(q.W);
      if (!(maxAbs > 0.0))
      {
        q.X = 0.0;
        q.Y = 0.0;
        q.Z = 0.0;
        q.W = 1.0;
        return;
      }
      double x = q.X / maxAbs;
      double y = q.Y / maxAbs;
      double z = q.Z / maxAbs;
      double w = q.W / maxAbs;
      double length = std::sqrt(x * x + y * y + z * z + w * w);
      q.X = x / length;
      q.Y = y / length;
      q.Z = z / length;
      q.W = w / length;
    }

    Quat SlerpQuat(Quat a, Quat b, double s)
    {
      double dot = DotQuat(a, b);
      if (dot < 0.0)
      {
        b = NegQuat(b);
        dot = -dot;
      }
      if (dot > 1.0)
        dot = 1.0;
      Quat out;
      if (dot > 0.9995)
      {
        out.X = a.X + (b.X - a.X) * s;
        out.Y = a.Y + (b.Y - a.Y) * s;
        out.Z = a.Z + (b.Z - a.Z) * s;
        out.W = a.W + (b.W - a.W) * s;
        NormalizeQuat(out);
        return out;
      }
      double theta = std::acos(dot);
      double sinTheta = std::sin(theta);
      if (std::fabs(sinTheta) < 1e-12)
      {
        out.X = a.X + (b.X - a.X) * s;
        out.Y = a.Y + (b.Y - a.Y) * s;
        out.Z = a.Z + (b.Z - a.Z) * s;
        out.W = a.W + (b.W - a.W) * s;
        NormalizeQuat(out);
        return out;
      }
      double wa = std::sin((1.0 - s) * theta) / sinTheta;
      double wb = std::sin(s * theta) / sinTheta;
      out.X = a.X * wa + b.X * wb;
      out.Y = a.Y * wa + b.Y * wb;
      out.Z = a.Z * wa + b.Z * wb;
      out.W = a.W * wa + b.W * wb;
      NormalizeQuat(out);
      return out;
    }

    void FlipIntoHemisphere(Quat& q, Quat const& reference)
    {
      if (DotQuat(q, reference) < 0.0)
        q = NegQuat(q);
    }

    // A turn as its rotation vector (axis x angle in radians, the short way: 0 - pi) and back
    void LogQuat(Quat q, double* v)
    {
      if (q.W < 0.0)
        q = NegQuat(q);
      double s = std::sqrt(q.X * q.X + q.Y * q.Y + q.Z * q.Z);
      double f = s < 1e-12 ? 2.0 : 2.0 * std::atan2(s, q.W) / s;   // (small: 2 sin(a/2) / s ~ 2)
      v[0] = q.X * f;
      v[1] = q.Y * f;
      v[2] = q.Z * f;
    }

    Quat ExpQuat(double const* v)
    {
      double angle = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
      Quat q;
      if (angle < 1e-12)
      {
        q.X = v[0] * 0.5;
        q.Y = v[1] * 0.5;
        q.Z = v[2] * 0.5;
        q.W = 1.0;
        NormalizeQuat(q);
        return q;
      }
      double f = std::sin(angle * 0.5) / angle;
      q.X = v[0] * f;
      q.Y = v[1] * f;
      q.Z = v[2] * f;
      q.W = std::cos(angle * 0.5);
      return q;
    }

    // The turn from a to b as a rotation vector, the short way
    void TurnBetween(Quat const& a, Quat const& b, double* v)
    {
      LogQuat(MulQuat(ConjQuat(a), b), v);
    }

    // For a turn q1 x exp(h): the rate of h that turns at w (a turning rate in the turn's own frame, radians a second)
    // where h = phi: the inverse of the right Jacobian at phi, w + (phi x w) / 2 + k phi x (phi x w),
    // k = 1 / a^2 - (1 + cos a) / (2 a sin a) with a = |phi| (1/12 near 0; it stays finite up to a = pi)
    void RateAt(double const* phi, double const* w, double* out)
    {
      double a2 = phi[0] * phi[0] + phi[1] * phi[1] + phi[2] * phi[2];
      double a = std::sqrt(a2);
      double c1[3] = { phi[1] * w[2] - phi[2] * w[1], phi[2] * w[0] - phi[0] * w[2], phi[0] * w[1] - phi[1] * w[0] };
      double c2[3] = { phi[1] * c1[2] - phi[2] * c1[1], phi[2] * c1[0] - phi[0] * c1[2], phi[0] * c1[1] - phi[1] * c1[0] };
      double k;
      if (a < 1e-3)
        k = 1.0 / 12.0 + a2 / 720.0;
      else if (a > 3.14159265358979323846 - 1e-6)
        k = 1.0 / a2;
      else
        k = 1.0 / a2 - (1.0 + std::cos(a)) / (2.0 * a * std::sin(a));
      for (int c = 0; c < 3; ++c)
        out[c] = w[c] + 0.5 * c1[c] + k * c2[c];
    }

    double HermiteValue(double p1, double p2, double m1, double m2, double distance, double u)
    {
      double u2 = u * u;
      double u3 = u2 * u;
      return (2.0 * u3 - 3.0 * u2 + 1.0) * p1 + (u3 - 2.0 * u2 + u) * distance * m1 +
        (-2.0 * u3 + 3.0 * u2) * p2 + (u3 - u2) * distance * m2;
    }

    // A Smooth segment's slope (units a second) at one of its two keys. thisSlope: the segment's own (its two keys'
    // difference over its time); otherOk: there is a segment on the key's other side, otherSlope its own slope,
    // otherEase its ease (the ease of the key it starts at), otherEnds: that segment ends at the key (it's the one
    // before it); spanSlope: across both (the Catmull-Rom slope).
    double KeySlope(double thisSlope, bool otherOk, double otherSlope, int otherEase, bool otherEnds, double spanSlope)
    {
      if (!otherOk)
        return 0.0;                          // a bone's first or last key: it eases in, or settles
      double m;
      if (otherEase != Ease_Smooth)
      {
        // The other segment's own speed at the key carries on (within the tests below): Linear's, Ease in's arrival
        // and Ease out's or Overshoot's start (2026-11-16); Ease in and out, Hold, and an ease that is still there: 0
        double factor = otherEnds ? EaseEndSpeed(otherEase) : EaseStartSpeed(otherEase);
        if (!(factor > 0.0))
          return 0.0;
        m = factor * otherSlope;
      }
      else
        m = spanSlope;
      if (thisSlope * otherSlope <= 0.0)
        return 0.0;                          // a hold, or the motion turns back here
      double most = 3.0 * (std::min)(std::fabs(thisSlope), std::fabs(otherSlope));
      if (std::fabs(m) > most)
        m = m < 0.0 ? -most : most;
      return m;
    }

    int ChannelIndex(Anim const& anim, int bone)
    {
      std::vector<Channel>::const_iterator it = std::lower_bound(anim.Channels.begin(), anim.Channels.end(), bone,
        [](Channel const& channel, int value) { return channel.Bone < value; });
      if (it == anim.Channels.end() || it->Bone != bone)
        return -1;
      return (int)(it - anim.Channels.begin());
    }

    int NearestKeyIndex(std::vector<Key> const& keys, double time)
    {
      if (keys.empty())
        return -1;
      std::vector<Key>::const_iterator it = std::lower_bound(keys.begin(), keys.end(), time,
        [](Key const& key, double value) { return key.Time < value; });
      int best = -1;
      double bestDistance = 0.0;
      if (it != keys.begin())
      {
        int index = (int)(it - keys.begin()) - 1;
        double distance = std::fabs(time - keys[(size_t)index].Time);
        if (distance <= TimeEpsilon)
        {
          best = index;
          bestDistance = distance;
        }
      }
      if (it != keys.end())
      {
        int index = (int)(it - keys.begin());
        double distance = std::fabs(time - keys[(size_t)index].Time);
        if (distance <= TimeEpsilon && (best < 0 || distance < bestDistance))
          best = index;
      }
      return best;
    }

    int InsertKeySorted(std::vector<Key>& keys, Key const& key)
    {
      std::vector<Key>::iterator it = std::lower_bound(keys.begin(), keys.end(), key.Time,
        [](Key const& other, double value) { return other.Time < value; });
      int index = (int)(it - keys.begin());
      keys.insert(it, key);
      return index;
    }

    int InsertChannelSorted(Anim& anim, Channel const& channel)
    {
      std::vector<Channel>::iterator it = std::lower_bound(anim.Channels.begin(), anim.Channels.end(), channel.Bone,
        [](Channel const& other, int value) { return other.Bone < value; });
      int index = (int)(it - anim.Channels.begin());
      anim.Channels.insert(it, channel);
      return index;
    }

    double ClampTime(double time)
    {
      if (time < 0.0)
        return 0.0;
      if (time > MaxTime)
        return MaxTime;
      return time;
    }

    int MoveKeyInternal(Anim& anim, int bone, int index, double time, bool& changed)
    {
      changed = false;
      if (!Finite(time))
        return -1;
      int channelIndex = ChannelIndex(anim, bone);
      if (channelIndex < 0)
        return -1;
      Channel& channel = anim.Channels[(size_t)channelIndex];
      if (index < 0 || (size_t)index >= channel.Keys.size())
        return -1;

      double target = ClampTime(time);
      Key moved = channel.Keys[(size_t)index];
      double oldTime = moved.Time;
      bool removed = false;
      std::vector<Key> kept;
      kept.reserve(channel.Keys.size());
      for (size_t k = 0; k < channel.Keys.size(); ++k)
      {
        if (k == (size_t)index)
          continue;
        if (std::fabs(channel.Keys[k].Time - target) <= TimeEpsilon)
        {
          removed = true;
          continue;
        }
        kept.push_back(channel.Keys[k]);
      }

      moved.Time = target;
      if (target > anim.Length)
        anim.Length = target;
      changed = (target != oldTime) || removed;
      std::vector<Key>::iterator it = std::lower_bound(kept.begin(), kept.end(), target,
        [](Key const& key, double value) { return key.Time < value; });
      int newIndex = (int)(it - kept.begin());
      kept.insert(it, moved);
      channel.Keys.swap(kept);
      return newIndex;
    }

    char const* FileHeader()
    {
      return "# MEC Cinematic Tools pose keyframes 1";
    }

    void LineError(std::string& error, int line, char const* message)
    {
      char text[512];
      _snprintf_s(text, sizeof(text), _TRUNCATE, "line %d: %s", line, message);
      error = text;
    }

    void OpenError(std::string& error, char const* path, char const* mode)
    {
      char text[1024];
      _snprintf_s(text, sizeof(text), _TRUNCATE, "cannot open \"%s\" for %s", path, mode);
      error = text;
    }

    bool AtLineEnd(char const* p)
    {
      while (*p == ' ' || *p == '\t')
        ++p;
      return *p == '\0';
    }

    char const* SkipSpace(char const* p)
    {
      while (*p == ' ' || *p == '\t')
        ++p;
      return p;
    }

    bool ReadToken(char const*& p, char const*& begin, char const*& end)
    {
      p = SkipSpace(p);
      if (*p == '\0')
        return false;
      begin = p;
      while (*p != '\0' && *p != ' ' && *p != '\t')
        ++p;
      end = p;
      return true;
    }

    bool WordIs(char const* begin, char const* end, char const* word)
    {
      size_t length = std::strlen(word);
      return (size_t)(end - begin) == length && std::memcmp(begin, word, length) == 0;
    }

    bool ParseNumber(char const*& p, double& value)
    {
      p = SkipSpace(p);
      if (*p == '\0')
        return false;
      char* end = nullptr;
      _locale_t locale = NumberLocale();
      double parsed = locale ? _strtod_l(p, &end, locale) : std::strtod(p, &end);
      if (end == p)
        return false;
      if (*end != '\0' && *end != ' ' && *end != '\t')
        return false;
      if (!Finite(parsed))
        return false;
      value = parsed;
      p = end;
      return true;
    }

    bool ToFloat(double value, float& out)
    {
      if (!Finite(value))
        return false;
      double magnitude = std::fabs(value);
      if (magnitude > 3.4028236e+38)
        return false;
      if (magnitude > (double)std::numeric_limits<float>::max())
        out = value < 0.0 ? -std::numeric_limits<float>::max() : std::numeric_limits<float>::max();
      else
        out = (float)value;
      return true;
    }

    bool NormalizeLoadedQuat(double const input[4], float output[4])
    {
      Quat q;
      q.X = input[0];
      q.Y = input[1];
      q.Z = input[2];
      q.W = input[3];
      double length = QuatMagnitude(q);
      if (!(length >= 1e-6))
        return false;
      NormalizeQuat(q);
      PutQuat(q, output);
      return true;
    }

    bool ParseEaseWord(char const* begin, char const* end, int& ease)
    {
      if (WordIs(begin, end, "smooth"))
        ease = Ease_Smooth;
      else if (WordIs(begin, end, "linear"))
        ease = Ease_Linear;
      else if (WordIs(begin, end, "ease"))
        ease = Ease_EaseInOut;
      else if (WordIs(begin, end, "hold"))
        ease = Ease_Hold;
      else if (WordIs(begin, end, "easein"))
        ease = Ease_In;
      else if (WordIs(begin, end, "easeout"))
        ease = Ease_Out;
      else if (WordIs(begin, end, "overshoot"))
        ease = Ease_Overshoot;
      else
        return false;
      return true;
    }

    bool ParseClockWord(char const* begin, char const* end, int& clock)
    {
      if (WordIs(begin, end, "game"))
        clock = Clock_Game;
      else if (WordIs(begin, end, "real"))
        clock = Clock_Real;
      else if (WordIs(begin, end, "track"))
        clock = Clock_Track;
      else if (WordIs(begin, end, "clip"))
        clock = Clock_Clip;
      else
        return false;
      return true;
    }

    bool AddLoadedKey(FileBone& bone, Key const& key)
    {
      int existing = NearestKeyIndex(bone.Keys, key.Time);
      if (existing >= 0)
      {
        Key& target = bone.Keys[(size_t)existing];
        std::memcpy(target.Turn, key.Turn, sizeof(target.Turn));
        std::memcpy(target.Move, key.Move, sizeof(target.Move));
        target.Ease = key.Ease;
        return true;
      }
      if (bone.Keys.size() >= (size_t)MaxKeysPerBone)
        return false;
      InsertKeySorted(bone.Keys, key);
      return true;
    }

    void TrimLine(char* line)
    {
      size_t length = std::strlen(line);
      while (length > 0)
      {
        char c = line[length - 1];
        if (c != '\r' && c != '\n' && c != ' ' && c != '\t')
          break;
        line[--length] = '\0';
      }
    }

    std::string TrimEdges(std::string const& text)
    {
      size_t begin = 0;
      size_t end = text.size();
      while (begin < end && (text[begin] == ' ' || text[begin] == '\t'))
        ++begin;
      while (end > begin && (text[end - 1] == ' ' || text[end - 1] == '\t'))
        --end;
      return text.substr(begin, end - begin);
    }

    bool TextFieldHasBadChar(std::string const& text)
    {
      for (size_t i = 0; i < text.size(); ++i)
      {
        unsigned char c = (unsigned char)text[i];
        if (c == 0 || c == 127 || (c < 32 && c != '\t'))
          return true;
      }
      return false;
    }

    bool FileLineFits(size_t prefix, size_t content)
    {
      return prefix + content < 1023;
    }

    bool BoneNameIsValid(std::string const& name)
    {
      if (name.empty())
        return false;
      for (size_t i = 0; i < name.size(); ++i)
      {
        unsigned char c = (unsigned char)name[i];
        if (c <= 32 || c == 127)
          return false;
      }
      return true;
    }

    bool PrepareFileForSave(File const& file, std::string& label, std::string& skeleton, std::string& clip,
      std::string& error)
    {
      if (TextFieldHasBadChar(file.Label))
      {
        error = "the label holds a control character";
        return false;
      }
      if (TextFieldHasBadChar(file.Skeleton))
      {
        error = "the skeleton holds a control character";
        return false;
      }
      if (TextFieldHasBadChar(file.Clip))
      {
        error = "the clip holds a control character";
        return false;
      }
      label = TrimEdges(file.Label);
      skeleton = TrimEdges(file.Skeleton);
      clip = TrimEdges(file.Clip);
      if (!FileLineFits(6, label.size()))
      {
        error = "a label line would be 1023 characters or longer";
        return false;
      }
      if (!FileLineFits(9, skeleton.size()))
      {
        error = "a skeleton line would be 1023 characters or longer";
        return false;
      }
      // Every other line has fixed text and at most eight %.17g numbers, far below the 1023 limit.
      if (file.Bones.size() > (size_t)MaxFileBones)
      {
        error = "there are more than MaxFileBones bones";
        return false;
      }
      if (file.Clock < 0 || file.Clock >= Clock_Count)
      {
        error = "the clock is not a known clock";
        return false;
      }
      if (!Finite(file.Length) || !Finite((double)file.Speed) || !Finite(file.Start))
      {
        error = "a timeline number is not finite";
        return false;
      }
      for (size_t b = 0; b < file.Bones.size(); ++b)
      {
        FileBone const& bone = file.Bones[b];
        if (!BoneNameIsValid(bone.Name))
        {
          error = "a bone's name is empty or holds a space or control character";
          return false;
        }
        for (size_t earlier = 0; earlier < b; ++earlier)
          if (file.Bones[earlier].Name == bone.Name)
          {
            error = "two bones have the same name";
            return false;
          }
        if (!FileLineFits(5, bone.Name.size()))
        {
          error = "a bone line would be 1023 characters or longer";
          return false;
        }
        if (bone.Keys.size() > (size_t)MaxKeysPerBone)
        {
          error = "a bone has more than MaxKeysPerBone keys";
          return false;
        }
        if (bone.HasBase)
        {
          for (int i = 0; i < 4; ++i)
          {
            if (!Finite((double)bone.BaseQ[i]))
            {
              error = "a base quaternion number is not finite";
              return false;
            }
          }
          for (int i = 0; i < 3; ++i)
          {
            if (!Finite((double)bone.BaseT[i]))
            {
              error = "a base translation number is not finite";
              return false;
            }
          }
          if (!(QuatMagnitude(MakeQuat(bone.BaseQ)) >= 1e-6))
          {
            error = "a base quaternion is shorter than 1e-6";
            return false;
          }
        }
        if (bone.HasStatic)
        {
          for (int i = 0; i < 4; ++i)
          {
            if (!Finite((double)bone.StaticTurn[i]))
            {
              error = "a static quaternion number is not finite";
              return false;
            }
          }
          for (int i = 0; i < 3; ++i)
          {
            if (!Finite((double)bone.StaticMove[i]))
            {
              error = "a static translation number is not finite";
              return false;
            }
          }
          if (!(QuatMagnitude(MakeQuat(bone.StaticTurn)) >= 1e-6))
          {
            error = "a static quaternion is shorter than 1e-6";
            return false;
          }
        }
        for (size_t k = 0; k < bone.Keys.size(); ++k)
        {
          Key const& key = bone.Keys[k];
          if (!Finite(key.Time) || key.Time < 0.0 || key.Time > MaxTime)
          {
            error = "a key time is not finite or is outside 0 - MaxTime";
            return false;
          }
          if (k > 0)
          {
            double previous = bone.Keys[k - 1].Time;
            if (!(key.Time > previous) || !(key.Time - previous > TimeEpsilon))
            {
              error = "a bone's keys are not in strictly increasing order, more than TimeEpsilon apart";
              return false;
            }
          }
          if (key.Ease < 0 || key.Ease >= Ease_Count)
          {
            error = "a key ease is not a known ease";
            return false;
          }
          for (int i = 0; i < 4; ++i)
          {
            if (!Finite((double)key.Turn[i]))
            {
              error = "a key quaternion number is not finite";
              return false;
            }
          }
          if (!(QuatMagnitude(MakeQuat(key.Turn)) >= 1e-6))
          {
            error = "a key quaternion is shorter than 1e-6";
            return false;
          }
          for (int i = 0; i < 3; ++i)
          {
            if (!Finite((double)key.Move[i]))
            {
              error = "a key translation number is not finite";
              return false;
            }
          }
        }
      }
      if (clip.empty())
        clip = "-";
      if (!FileLineFits(5, clip.size()))
      {
        error = "a clip line would be 1023 characters or longer";
        return false;
      }
      // A clip that is exactly "-" after trimming is written as the format's no-clip word: it reads back as no clip.
      return true;
    }

    bool ReadPoseFile(FILE* f, File& file, std::string& error)
    {
      file = File();
      char line[1024];
      int lineNumber = 0;
      int currentBone = -1;
      if (std::fgets(line, sizeof(line), f) == nullptr)
      {
        LineError(error, 1, "the file is empty");
        return false;
      }
      ++lineNumber;
      if (std::strchr(line, '\n') == nullptr && !std::feof(f))
      {
        LineError(error, lineNumber, "the line is too long");
        return false;
      }
      if (std::strlen(line) >= 3 && std::memcmp(line, "\xEF\xBB\xBF", 3) == 0)
        std::memmove(line, line + 3, std::strlen(line + 3) + 1);
      TrimLine(line);
      if (std::strcmp(line, FileHeader()) != 0)
      {
        LineError(error, lineNumber, "the file does not start with the pose keyframes header");
        return false;
      }

      while (std::fgets(line, sizeof(line), f) != nullptr)
      {
        ++lineNumber;
        if (std::strchr(line, '\n') == nullptr && !std::feof(f))
        {
          LineError(error, lineNumber, "the line is too long");
          return false;
        }
        TrimLine(line);
        char const* p = SkipSpace(line);
        if (*p == '\0' || *p == '#')
          continue;

        char const* wordBegin = p;
        while (*p != '\0' && *p != ' ' && *p != '\t')
          ++p;
        char const* wordEnd = p;

        if (WordIs(wordBegin, wordEnd, "label"))
        {
          file.Label = SkipSpace(p);
          continue;
        }
        if (WordIs(wordBegin, wordEnd, "clip"))
        {
          p = SkipSpace(p);
          file.Clip = p;
          if (file.Clip == "-")
            file.Clip.clear();
          continue;
        }
        if (WordIs(wordBegin, wordEnd, "skeleton"))
        {
          file.Skeleton = SkipSpace(p);
          continue;
        }
        if (WordIs(wordBegin, wordEnd, "overclip"))
        {
          double value = 0.0;
          if (!ParseNumber(p, value) || !AtLineEnd(p) || (value != 0.0 && value != 1.0))
          {
            LineError(error, lineNumber, "bad overclip line");
            return false;
          }
          file.OverClip = value != 0.0;
          continue;
        }
        if (WordIs(wordBegin, wordEnd, "length"))
        {
          double value = 0.0;
          if (!ParseNumber(p, value) || !AtLineEnd(p))
          {
            LineError(error, lineNumber, "bad length line");
            return false;
          }
          file.Length = value;
          continue;
        }
        if (WordIs(wordBegin, wordEnd, "loop"))
        {
          double value = 0.0;
          if (!ParseNumber(p, value) || !AtLineEnd(p) || (value != 0.0 && value != 1.0))
          {
            LineError(error, lineNumber, "bad loop line");
            return false;
          }
          file.Loop = value != 0.0;
          continue;
        }
        if (WordIs(wordBegin, wordEnd, "speed"))
        {
          double value = 0.0;
          if (!ParseNumber(p, value) || !AtLineEnd(p) || !ToFloat(value, file.Speed))
          {
            LineError(error, lineNumber, "bad speed line");
            return false;
          }
          continue;
        }
        if (WordIs(wordBegin, wordEnd, "clock"))
        {
          char const* begin = nullptr;
          char const* end = nullptr;
          int clock = Clock_Game;
          if (!ReadToken(p, begin, end) || !AtLineEnd(p) || !ParseClockWord(begin, end, clock))
          {
            LineError(error, lineNumber, "bad clock word");
            return false;
          }
          file.Clock = clock;
          continue;
        }
        if (WordIs(wordBegin, wordEnd, "start"))
        {
          double value = 0.0;
          if (!ParseNumber(p, value) || !AtLineEnd(p))
          {
            LineError(error, lineNumber, "bad start line");
            return false;
          }
          file.Start = value;
          continue;
        }
        if (WordIs(wordBegin, wordEnd, "bone"))
        {
          char const* begin = nullptr;
          char const* end = nullptr;
          if (!ReadToken(p, begin, end) || !AtLineEnd(p))
          {
            LineError(error, lineNumber, "bad bone line");
            return false;
          }
          std::string name(begin, end);
          int found = -1;
          for (size_t b = 0; b < file.Bones.size(); ++b)
          {
            if (file.Bones[b].Name == name)
            {
              found = (int)b;
              break;
            }
          }
          if (found < 0)
          {
            if (file.Bones.size() >= (size_t)MaxFileBones)
            {
              LineError(error, lineNumber, "too many bones");
              return false;
            }
            FileBone bone;
            bone.Name = name;
            file.Bones.push_back(bone);
            found = (int)file.Bones.size() - 1;
          }
          currentBone = found;
          continue;
        }
        if (WordIs(wordBegin, wordEnd, "base") || WordIs(wordBegin, wordEnd, "static"))
        {
          bool isBase = WordIs(wordBegin, wordEnd, "base");
          if (currentBone < 0)
          {
            LineError(error, lineNumber, isBase ? "a base line comes before any bone" :
              "a static line comes before any bone");
            return false;
          }
          double values[7];
          for (int i = 0; i < 7; ++i)
          {
            if (!ParseNumber(p, values[i]))
            {
              LineError(error, lineNumber, isBase ? "bad base line" : "bad static line");
              return false;
            }
          }
          if (!AtLineEnd(p))
          {
            LineError(error, lineNumber, isBase ? "bad base line" : "bad static line");
            return false;
          }
          float q[4];
          if (!NormalizeLoadedQuat(values, q))
          {
            LineError(error, lineNumber, isBase ? "the base quaternion is too short" :
              "the static quaternion is too short");
            return false;
          }
          float position[3];
          bool positionOk = ToFloat(values[4], position[0]) && ToFloat(values[5], position[1]) &&
            ToFloat(values[6], position[2]);
          if (!positionOk)
          {
            LineError(error, lineNumber, isBase ? "bad base line" : "bad static line");
            return false;
          }
          FileBone& bone = file.Bones[(size_t)currentBone];
          if (isBase)
          {
            bone.HasBase = true;
            std::memcpy(bone.BaseQ, q, sizeof(bone.BaseQ));
            std::memcpy(bone.BaseT, position, sizeof(bone.BaseT));
          }
          else
          {
            bone.HasStatic = true;
            std::memcpy(bone.StaticTurn, q, sizeof(bone.StaticTurn));
            std::memcpy(bone.StaticMove, position, sizeof(bone.StaticMove));
          }
          continue;
        }
        if (WordIs(wordBegin, wordEnd, "key"))
        {
          if (currentBone < 0)
          {
            LineError(error, lineNumber, "a key comes before any bone");
            return false;
          }
          double time = 0.0;
          if (!ParseNumber(p, time))
          {
            LineError(error, lineNumber, "bad key line");
            return false;
          }
          if (time < 0.0 || time > MaxTime)
          {
            LineError(error, lineNumber, "the key time is out of range");
            return false;
          }
          double values[7];
          for (int i = 0; i < 7; ++i)
          {
            if (!ParseNumber(p, values[i]))
            {
              LineError(error, lineNumber, "bad key line");
              return false;
            }
          }
          char const* easeBegin = nullptr;
          char const* easeEnd = nullptr;
          int ease = Ease_Smooth;
          if (!ReadToken(p, easeBegin, easeEnd) || !AtLineEnd(p) || !ParseEaseWord(easeBegin, easeEnd, ease))
          {
            LineError(error, lineNumber, "bad key ease word");
            return false;
          }
          float q[4];
          if (!NormalizeLoadedQuat(values, q))
          {
            LineError(error, lineNumber, "the key quaternion is too short");
            return false;
          }
          float move[3];
          bool moveOk = ToFloat(values[4], move[0]) && ToFloat(values[5], move[1]) &&
            ToFloat(values[6], move[2]);
          if (!moveOk)
          {
            LineError(error, lineNumber, "bad key line");
            return false;
          }
          Key key;
          key.Time = time;
          std::memcpy(key.Turn, q, sizeof(key.Turn));
          std::memcpy(key.Move, move, sizeof(key.Move));
          key.Ease = ease;
          if (!AddLoadedKey(file.Bones[(size_t)currentBone], key))
          {
            LineError(error, lineNumber, "too many keys on a bone");
            return false;
          }
          continue;
        }
      }
      if (std::ferror(f))
      {
        error = "the file could not be read";
        return false;
      }
      return true;
    }
  }

  char const* EaseName(int ease)
  {
    switch (ease)
    {
    case Ease_Smooth:
      return "smooth";
    case Ease_Linear:
      return "linear";
    case Ease_EaseInOut:
      return "ease";
    case Ease_Hold:
      return "hold";
    case Ease_In:
      return "easein";
    case Ease_Out:
      return "easeout";
    case Ease_Overshoot:
      return "overshoot";
    default:
      return "smooth";
    }
  }

  // Overshoot: an "ease out back" curve, 1 + c3 (u - 1)^3 + c1 (u - 1)^2 with c3 = c1 + 1: it leaves at speed, passes
  // the key at u = 1 - c1 / c3 (0.37), goes on to its peak at u = 1 - 2 c1 / (3 c3) (0.58), 1 + 4 c1^3 / (27 c3^2)
  // (1.100: OvershootPast of the way past the key), and comes back to settle on it with no speed at u = 1
  // (c1 = 1.70158, the usual value for 10 %)
  namespace
  {
    const double OvershootC1 = 1.70158;
    const double OvershootC3 = OvershootC1 + 1.0;
  }

  double EaseShape(int ease, double u)
  {
    if (!(u > 0.0))
      return 0.0;
    if (u >= 1.0)
      return 1.0;
    switch (ease)
    {
    case Ease_EaseInOut:
      return u * u * (3.0 - 2.0 * u);
    case Ease_Hold:
      return 0.0;
    case Ease_In:
      return u * u;
    case Ease_Out:
      return 1.0 - (1.0 - u) * (1.0 - u);
    case Ease_Overshoot:
    {
      double v = u - 1.0;
      return 1.0 + OvershootC3 * v * v * v + OvershootC1 * v * v;
    }
    default:
      return u;
    }
  }

  double EaseStartSpeed(int ease)
  {
    switch (ease)
    {
    case Ease_Linear:
      return 1.0;
    case Ease_Out:
      return 2.0;
    case Ease_Overshoot:
      return 3.0 * OvershootC3 - 2.0 * OvershootC1;   // the curve's slope at 0 (4.70)
    default:
      return 0.0;
    }
  }

  double EaseEndSpeed(int ease)
  {
    switch (ease)
    {
    case Ease_Linear:
      return 1.0;
    case Ease_In:
      return 2.0;
    default:
      return 0.0;
    }
  }

  char const* ClockName(int clock)
  {
    switch (clock)
    {
    case Clock_Game:
      return "game";
    case Clock_Real:
      return "real";
    case Clock_Track:
      return "track";
    case Clock_Clip:
      return "clip";
    default:
      return "game";
    }
  }

  Channel* Find(Anim& anim, int bone)
  {
    int index = ChannelIndex(anim, bone);
    if (index < 0)
      return nullptr;
    return &anim.Channels[(size_t)index];
  }

  Channel const* Find(Anim const& anim, int bone)
  {
    int index = ChannelIndex(anim, bone);
    if (index < 0)
      return nullptr;
    return &anim.Channels[(size_t)index];
  }

  int KeyCount(Anim const& anim)
  {
    int count = 0;
    for (size_t c = 0; c < anim.Channels.size(); ++c)
      count += (int)anim.Channels[c].Keys.size();
    return count;
  }

  double FirstKeyTime(Anim const& anim)
  {
    double first = 0.0;
    bool found = false;
    for (size_t c = 0; c < anim.Channels.size(); ++c)
    {
      Channel const& channel = anim.Channels[c];
      for (size_t k = 0; k < channel.Keys.size(); ++k)
      {
        if (!found || channel.Keys[k].Time < first)
        {
          first = channel.Keys[k].Time;
          found = true;
        }
      }
    }
    return first;
  }

  double LastKeyTime(Anim const& anim)
  {
    double last = 0.0;
    bool found = false;
    for (size_t c = 0; c < anim.Channels.size(); ++c)
    {
      Channel const& channel = anim.Channels[c];
      for (size_t k = 0; k < channel.Keys.size(); ++k)
      {
        if (!found || channel.Keys[k].Time > last)
        {
          last = channel.Keys[k].Time;
          found = true;
        }
      }
    }
    return last;
  }

  int KeyAt(Channel const& channel, double time)
  {
    return NearestKeyIndex(channel.Keys, time);
  }

  bool KeyTimeBefore(Anim const& anim, double time, double& found)
  {
    bool have = false;
    double best = 0.0;
    for (size_t c = 0; c < anim.Channels.size(); ++c)
    {
      Channel const& channel = anim.Channels[c];
      for (size_t k = 0; k < channel.Keys.size(); ++k)
      {
        double keyTime = channel.Keys[k].Time;
        if (time - keyTime > TimeEpsilon && (!have || keyTime > best))
        {
          best = keyTime;
          have = true;
        }
      }
    }
    if (have)
      found = best;
    return have;
  }

  bool KeyTimeAfter(Anim const& anim, double time, double& found)
  {
    bool have = false;
    double best = 0.0;
    for (size_t c = 0; c < anim.Channels.size(); ++c)
    {
      Channel const& channel = anim.Channels[c];
      for (size_t k = 0; k < channel.Keys.size(); ++k)
      {
        double keyTime = channel.Keys[k].Time;
        if (keyTime - time > TimeEpsilon && (!have || keyTime < best))
        {
          best = keyTime;
          have = true;
        }
      }
    }
    if (have)
      found = best;
    return have;
  }

  int SetKey(Anim& anim, int bone, double time, float const* turn, float const* move, int ease, bool automatic)
  {
    float copiedTurn[4];
    float copiedMove[3];
    std::memcpy(copiedTurn, turn, sizeof(copiedTurn));
    std::memcpy(copiedMove, move, sizeof(copiedMove));

    if (!Finite(time) || time < 0.0 || time > MaxTime)
      return -1;
    if (bone < 0)
      return -1;
    for (int i = 0; i < 4; ++i)
    {
      if (!Finite((double)copiedTurn[i]))
        return -1;
    }
    for (int i = 0; i < 3; ++i)
    {
      if (!Finite((double)copiedMove[i]))
        return -1;
    }

    Quat q;
    q.X = copiedTurn[0];
    q.Y = copiedTurn[1];
    q.Z = copiedTurn[2];
    q.W = copiedTurn[3];
    double turnLength = QuatMagnitude(q);
    if (turnLength < 1e-6)
      return -1;
    NormalizeQuat(q);

    Key key;
    key.Time = time;
    PutQuat(q, key.Turn);
    std::memcpy(key.Move, copiedMove, sizeof(key.Move));
    key.Ease = ease;
    key.Auto = automatic;

    int channelIndex = ChannelIndex(anim, bone);
    if (channelIndex >= 0)
    {
      Channel& channel = anim.Channels[(size_t)channelIndex];
      int existing = KeyAt(channel, time);
      if (existing >= 0)
      {
        Key& replaced = channel.Keys[(size_t)existing];
        std::memcpy(replaced.Turn, key.Turn, sizeof(replaced.Turn));
        std::memcpy(replaced.Move, key.Move, sizeof(replaced.Move));
        if (ease >= 0 && ease < Ease_Count)
          replaced.Ease = ease;
        replaced.Auto = automatic;
        ++anim.Revision;
        return existing;
      }
      else
      {
        if (channel.Keys.size() >= (size_t)MaxKeysPerBone)
          return -1;
        if (ease < 0 || ease >= Ease_Count)
        {
          std::vector<Key>::iterator at = std::lower_bound(channel.Keys.begin(), channel.Keys.end(), time,
            [](Key const& other, double value) { return other.Time < value; });
          if (at != channel.Keys.begin())
            key.Ease = (at - 1)->Ease;
          else
            key.Ease = Ease_Smooth;
        }
        int newIndex = InsertKeySorted(channel.Keys, key);
        if (time > anim.Length)
          anim.Length = time;
        ++anim.Revision;
        return newIndex;
      }
    }

    if (ease < 0 || ease >= Ease_Count)
      key.Ease = Ease_Smooth;
    Channel channel;
    channel.Bone = bone;
    channel.Keys.push_back(key);
    InsertChannelSorted(anim, channel);
    if (time > anim.Length)
      anim.Length = time;
    ++anim.Revision;
    return 0;
  }

  bool SetEase(Anim& anim, int bone, int index, int ease)
  {
    if (ease < 0 || ease >= Ease_Count)
      return false;
    Channel* channel = Find(anim, bone);
    if (channel == nullptr || index < 0 || (size_t)index >= channel->Keys.size())
      return false;
    Key& key = channel->Keys[(size_t)index];
    if (key.Ease != ease)
    {
      key.Ease = ease;
      ++anim.Revision;
    }
    return true;
  }

  bool RemoveKey(Anim& anim, int bone, int index)
  {
    int channelIndex = ChannelIndex(anim, bone);
    if (channelIndex < 0)
      return false;
    Channel& channel = anim.Channels[(size_t)channelIndex];
    if (index < 0 || (size_t)index >= channel.Keys.size())
      return false;
    channel.Keys.erase(channel.Keys.begin() + index);
    ++anim.Revision;
    if (channel.Keys.empty())
      anim.Channels.erase(anim.Channels.begin() + channelIndex);
    return true;
  }

  bool RemoveChannel(Anim& anim, int bone)
  {
    int channelIndex = ChannelIndex(anim, bone);
    if (channelIndex < 0)
      return false;
    anim.Channels.erase(anim.Channels.begin() + channelIndex);
    ++anim.Revision;
    return true;
  }

  int RemoveKeysAt(Anim& anim, double time)
  {
    if (!Finite(time))
      return 0;
    int removed = 0;
    for (size_t c = 0; c < anim.Channels.size();)
    {
      Channel& channel = anim.Channels[c];
      int index = KeyAt(channel, time);
      if (index >= 0)
      {
        channel.Keys.erase(channel.Keys.begin() + index);
        ++removed;
      }
      if (channel.Keys.empty())
        anim.Channels.erase(anim.Channels.begin() + c);
      else
        ++c;
    }
    if (removed > 0)
      ++anim.Revision;
    return removed;
  }

  int MoveKey(Anim& anim, int bone, int index, double time)
  {
    bool changed = false;
    int result = MoveKeyInternal(anim, bone, index, time, changed);
    if (result >= 0 && changed)
      ++anim.Revision;
    return result;
  }

  int MoveKeysAt(Anim& anim, double from, double to)
  {
    if (!Finite(from) || !Finite(to))
      return 0;
    struct MoveRef
    {
      int Bone;
      int Index;
    };
    std::vector<MoveRef> moves;
    for (size_t c = 0; c < anim.Channels.size(); ++c)
    {
      int key = KeyAt(anim.Channels[c], from);
      if (key >= 0)
      {
        MoveRef ref;
        ref.Bone = anim.Channels[c].Bone;
        ref.Index = key;
        moves.push_back(ref);
      }
    }
    int found = (int)moves.size();
    bool changed = false;
    for (size_t m = 0; m < moves.size(); ++m)
    {
      bool keyChanged = false;
      MoveKeyInternal(anim, moves[m].Bone, moves[m].Index, to, keyChanged);
      if (keyChanged)
        changed = true;
    }
    if (changed)
      ++anim.Revision;
    return found;
  }

  void SetLength(Anim& anim, double length)
  {
    if (std::isnan(length))
      return;
    double low = LastKeyTime(anim);
    if (low < 0.1)
      low = 0.1;
    if (std::isinf(length))
      anim.Length = length > 0.0 ? MaxTime : low;
    else
    {
      if (length < low)
        length = low;
      if (length > MaxTime)
        length = MaxTime;
      anim.Length = length;
    }
  }

  bool Sample(Channel const& channel, double time, float* turn, float* move, double loopLength)
  {
    size_t count = channel.Keys.size();
    if (count == 0)
      return false;
    if (!Finite(time) || time <= channel.Keys[0].Time)
    {
      std::memcpy(turn, channel.Keys[0].Turn, 4 * sizeof(float));
      std::memcpy(move, channel.Keys[0].Move, 3 * sizeof(float));
      return true;
    }
    if (time >= channel.Keys[count - 1].Time)
    {
      std::memcpy(turn, channel.Keys[count - 1].Turn, 4 * sizeof(float));
      std::memcpy(move, channel.Keys[count - 1].Move, 3 * sizeof(float));
      return true;
    }

    size_t i = 0;
    std::vector<Key>::const_iterator it = std::upper_bound(channel.Keys.begin(), channel.Keys.end(), time,
      [](double value, Key const& key) { return value < key.Time; });
    if (it != channel.Keys.begin())
      i = (size_t)(it - channel.Keys.begin()) - 1;
    Key const& a = channel.Keys[i];
    Key const& b = channel.Keys[i + 1];
    double u = (time - a.Time) / (b.Time - a.Time);

    if (a.Ease == Ease_Hold)
    {
      std::memcpy(turn, a.Turn, 4 * sizeof(float));
      std::memcpy(move, a.Move, 3 * sizeof(float));
      return true;
    }

    if (a.Ease == Ease_Linear || a.Ease == Ease_EaseInOut || a.Ease == Ease_In || a.Ease == Ease_Out ||
        a.Ease == Ease_Overshoot)
    {
      // (Overshoot's s goes past 1 and back: the slerp carries on along the same turn past b's, the move past b's)
      double s = EaseShape(a.Ease, u);
      Quat q = SlerpQuat(MakeQuat(a.Turn), MakeQuat(b.Turn), s);
      PutQuat(q, turn);
      for (int component = 0; component < 3; ++component)
        move[component] = (float)((double)a.Move[component] + ((double)b.Move[component] - a.Move[component]) * s);
      return true;
    }

    // Smooth: a cubic through the keys whose slope at each key is "auto-clamped", as in Blender's, Maya's and Unity's
    // default keys: 0 at a bone's first and last key (it eases in and settles), at a key equal to a neighbour (a hold
    // stays still) and where the motion turns back; elsewhere the Catmull-Rom slope across the key, kept within 3
    // times the slower of its two segments (Fritsch - Carlson: then no segment passes its keys); next to a Linear
    // segment that segment's speed (it carries on across the key, per component: where the Smooth segment turns or
    // moves the same way, up to 3 times the slower one; a turn the Linear one adds about another axis starts at the
    // key, a kink in the turning speed there), next to an Ease in and out or a Hold one 0; next to the eases of
    // 2026-11-16 their own speed at the key the same way (Ease in arrives at twice its segment's speed, Ease out and
    // Overshoot leave at twice and 4.7 times theirs; Ease out's arrival and Ease in's start are 0).
    // The move per component; the turn as one rotation: its rotation vector from the segment's first key (the
    // neighbours' added up, so a turn of more than half a circle over a few keys stays one way), the same rules on
    // its three components, then turned back into a quaternion (a steady spin stays steady: the quaternion's four
    // components each ran the rules alone until the review of 2026-11-15, and a fast spin pulsed). Up to 2026-11-14
    // a bone's first and last key took its segment's own slope and nothing kept a segment within its keys: a two-key
    // move started and stopped at full speed, a hold between two equal keys drifted 7 - 11 degrees, a fast move into a
    // rest went past it.
    // With loopLength (the keyframes loop at that length): a bone keyed at 0 and at the loop's end (3 keys or more) is
    // a cycle, so its first key's neighbour before is the one before its last (a loop earlier) and its last key's
    // after is its second (a loop later): the motion carries on through the wrap instead of stopping there.
    bool cycle = loopLength > 0.0 && count >= 3 && std::fabs(channel.Keys[0].Time) <= TimeEpsilon &&
      std::fabs(channel.Keys[count - 1].Time - loopLength) <= TimeEpsilon;
    bool hasBefore = i > 0 || cycle;
    bool hasAfter = i + 2 < count || cycle;
    Key const* k0 = &a;
    double t0 = a.Time;
    if (i > 0)
    {
      k0 = &channel.Keys[i - 1];
      t0 = k0->Time;
    }
    else if (cycle)
    {
      k0 = &channel.Keys[count - 2];
      t0 = k0->Time - loopLength;
    }
    Key const* k3 = &b;
    double t3 = b.Time;
    int afterEase = b.Ease;                    // the segment that starts at b
    if (i + 2 < count)
    {
      k3 = &channel.Keys[i + 2];
      t3 = k3->Time;
    }
    else if (cycle)
    {
      k3 = &channel.Keys[1];
      t3 = k3->Time + loopLength;
      afterEase = channel.Keys[0].Ease;        // after the wrap the segment from the first key follows
    }
    int beforeEase = hasBefore ? k0->Ease : Ease_Smooth;   // the segment that ends at a
    double t1 = a.Time;
    double t2 = b.Time;
    double d = t2 - t1;

    Quat q1 = MakeQuat(a.Turn);
    Quat q2 = MakeQuat(b.Turn);
    double r0[3] = { 0.0, 0.0, 0.0 }, r2[3], r3[3];
    TurnBetween(q1, q2, r2);
    if (hasBefore)
    {
      TurnBetween(MakeQuat(k0->Turn), q1, r0);
      for (int c = 0; c < 3; ++c)
        r0[c] = -r0[c];
    }
    if (hasAfter)
    {
      TurnBetween(q2, MakeQuat(k3->Turn), r3);   // the turn to the next key in b's frame, put after r2 (v3 - v2 is
      for (int c = 0; c < 3; ++c)                // it, and v3 - v1 the two together, as for the move)
        r3[c] += r2[c];
    }
    else
      std::memcpy(r3, r2, sizeof(r3));
    // v: the turn's 3 (rotation vectors from a's turn: a's is 0) and the move's 3
    double v0[6] = { r0[0], r0[1], r0[2], k0->Move[0], k0->Move[1], k0->Move[2] };
    double v1[6] = { 0.0, 0.0, 0.0, a.Move[0], a.Move[1], a.Move[2] };
    double v2[6] = { r2[0], r2[1], r2[2], b.Move[0], b.Move[1], b.Move[2] };
    double v3[6] = { r3[0], r3[1], r3[2], k3->Move[0], k3->Move[1], k3->Move[2] };
    // The slopes: the rule's, per component. For the turn they are turning rates in each key's own frame (the turns
    // to and from a key are the same vectors in its frame and its neighbour's), so the segments either side of a key
    // agree on its rate; at this segment's end the rate is made the rotation vector's (RateAt): between Smooth
    // segments the speed carries on across the key for turns about several axes too (measured: curve_test check 11).
    double m1[6], m2[6], out[6];
    for (int c = 0; c < 6; ++c)
    {
      double slope = (v2[c] - v1[c]) / d;
      m1[c] = KeySlope(slope, hasBefore, hasBefore ? (v1[c] - v0[c]) / (t1 - t0) : 0.0, beforeEase, true,
        hasBefore ? (v2[c] - v0[c]) / (t2 - t0) : 0.0);
      m2[c] = KeySlope(slope, hasAfter, hasAfter ? (v3[c] - v2[c]) / (t3 - t2) : 0.0, afterEase, false,
        hasAfter ? (v3[c] - v1[c]) / (t3 - t1) : 0.0);
    }
    double endRate[3];
    RateAt(r2, m2, endRate);
    for (int c = 0; c < 3; ++c)
      m2[c] = endRate[c];
    for (int c = 0; c < 6; ++c)
      out[c] = HermiteValue(v1[c], v2[c], m1[c], m2[c], d, u);
    Quat q = MulQuat(q1, ExpQuat(out));
    NormalizeQuat(q);
    PutQuat(q, turn);
    for (int component = 0; component < 3; ++component)
      move[component] = (float)out[3 + component];
    return true;
  }

  double Advance(Anim& anim, double realDt, float gameScale, double trackTime, double clipTime, bool clipOn)
  {
    if (!Finite(anim.Time))
      anim.Time = 0.0;
    bool timeSet = false;
    switch (anim.Clock)
    {
    case Clock_Game:
      if (anim.Playing)
      {
        anim.Time += realDt * (double)gameScale * (double)anim.Speed;
        timeSet = true;
      }
      break;
    case Clock_Real:
      if (anim.Playing)
      {
        anim.Time += realDt * (double)anim.Speed;
        timeSet = true;
      }
      break;
    case Clock_Track:
      if (anim.Playing && trackTime >= 0.0)
      {
        anim.Time = anim.Start + trackTime * (double)anim.Speed;
        timeSet = true;
      }
      break;
    case Clock_Clip:
      if (anim.Playing && clipOn)
      {
        anim.Time = anim.Start + clipTime * (double)anim.Speed;
        timeSet = true;
      }
      break;
    default:
      break;
    }
    if (!Finite(anim.Time))
      anim.Time = 0.0;

    if (!Finite(anim.Length) || anim.Length <= 0.0)
    {
      anim.Time = 0.0;
    }
    else if (timeSet && anim.Loop)
    {
      if (anim.Time > anim.Length)
        anim.Time = std::fmod(anim.Time, anim.Length);
      if (anim.Time < 0.0)
        anim.Time = anim.Length + std::fmod(anim.Time, anim.Length);
    }
    else
    {
      if (anim.Time < 0.0)
        anim.Time = 0.0;
      if (anim.Time > anim.Length)
        anim.Time = anim.Length;
      if ((anim.Clock == Clock_Game || anim.Clock == Clock_Real) && anim.Playing && anim.Time >= anim.Length)
        anim.Playing = false;
    }
    return anim.Time;
  }

  void Rebase(float const* baseQ, float const* baseT, float const* newBaseQ, float const* newBaseT, bool keepMove,
    float* turn, float* move)
  {
    Quat turnQ = MakeQuat(turn);
    Quat baseRotation = MakeQuat(baseQ);
    Quat newBaseRotation = MakeQuat(newBaseQ);
    double baseTranslation[3];
    double newBaseTranslation[3];
    double offset[3];
    for (int component = 0; component < 3; ++component)
    {
      baseTranslation[component] = baseT[component];
      newBaseTranslation[component] = newBaseT[component];
      offset[component] = move[component];
    }

    Quat absoluteRotation = MulQuat(turnQ, baseRotation);
    Quat newTurn = MulQuat(absoluteRotation, ConjQuat(newBaseRotation));
    NormalizeQuat(newTurn);
    PutQuat(newTurn, turn);
    if (keepMove)
    {
      for (int component = 0; component < 3; ++component)
        move[component] = (float)(baseTranslation[component] + offset[component] - newBaseTranslation[component]);
    }
    else
    {
      move[0] = 0.f;
      move[1] = 0.f;
      move[2] = 0.f;
    }
  }

  // A file written to temporary put in path's place in one step (MoveFileEx with REPLACE_EXISTING: the old file isn't
  // deleted first, so nothing is lost if it fails; a few tries a moment apart when something else, a virus scanner
  // or a sync program, holds one of them just then). False: the old file as it was, the new one still in temporary.
  static bool ReplaceWithTemporary(char const* temporary, char const* path)
  {
    // (a read-only file there, or a folder of that name, won't change: no tries again; the final review, 2026-11-16)
    DWORD there = GetFileAttributesA(path);
    if (there != INVALID_FILE_ATTRIBUTES && (there & (FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_DIRECTORY)))
      return false;
    for (int attempt = 0; attempt < 4; ++attempt)
    {
      if (attempt > 0)
        Sleep(25);
      if (MoveFileExA(temporary, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        return true;
      DWORD why = GetLastError();
      if (why != ERROR_SHARING_VIOLATION && why != ERROR_ACCESS_DENIED && why != ERROR_LOCK_VIOLATION)
        return false;
    }
    return false;
  }

  static bool ReplaceWithTemporary(wchar_t const* temporary, wchar_t const* path)
  {
    // (a read-only file there, or a folder of that name, won't change: no tries again; the final review, 2026-11-16)
    DWORD there = GetFileAttributesW(path);
    if (there != INVALID_FILE_ATTRIBUTES && (there & (FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_DIRECTORY)))
      return false;
    for (int attempt = 0; attempt < 4; ++attempt)
    {
      if (attempt > 0)
        Sleep(25);
      if (MoveFileExW(temporary, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        return true;
      DWORD why = GetLastError();
      if (why != ERROR_SHARING_VIOLATION && why != ERROR_ACCESS_DENIED && why != ERROR_LOCK_VIOLATION)
        return false;
    }
    return false;
  }

  // The file's text, into an open file (SaveFile's, both kinds of path); false if a write failed
  static bool WriteKeysText(FILE* f, File const& file, std::string const& label, std::string const& skeleton,
    std::string const& clip)
  {
    bool ok = true;
    if (!WriteText(f, "%s\n", FileHeader()))
      ok = false;
    if (!WriteText(f, "label %s\n", label.c_str()))
      ok = false;
    if (!WriteText(f, "skeleton %s\n", skeleton.c_str()))
      ok = false;
    if (!WriteText(f, "clip %s\n", clip.c_str()))
      ok = false;
    if (!WriteText(f, "overclip %d\n", file.OverClip ? 1 : 0))
      ok = false;
    if (!WriteText(f, "length %.17g\n", file.Length))
      ok = false;
    if (!WriteText(f, "loop %d\n", file.Loop ? 1 : 0))
      ok = false;
    if (!WriteText(f, "speed %.9g\n", (double)file.Speed))
      ok = false;
    if (!WriteText(f, "clock %s\n", ClockName(file.Clock)))
      ok = false;
    if (!WriteText(f, "start %.17g\n", file.Start))
      ok = false;

    for (size_t b = 0; b < file.Bones.size(); ++b)
    {
      FileBone const& bone = file.Bones[b];
      if (!WriteText(f, "bone %s\n", bone.Name.c_str()))
        ok = false;
      if (bone.HasBase)
      {
        if (!WriteText(f, "base %.9g %.9g %.9g %.9g %.9g %.9g %.9g\n", (double)bone.BaseQ[0],
          (double)bone.BaseQ[1], (double)bone.BaseQ[2], (double)bone.BaseQ[3], (double)bone.BaseT[0],
          (double)bone.BaseT[1], (double)bone.BaseT[2]))
          ok = false;
      }
      if (bone.HasStatic)
      {
        if (!WriteText(f, "static %.9g %.9g %.9g %.9g %.9g %.9g %.9g\n", (double)bone.StaticTurn[0],
          (double)bone.StaticTurn[1], (double)bone.StaticTurn[2], (double)bone.StaticTurn[3],
          (double)bone.StaticMove[0], (double)bone.StaticMove[1], (double)bone.StaticMove[2]))
          ok = false;
      }
      for (size_t k = 0; k < bone.Keys.size(); ++k)
      {
        Key const& key = bone.Keys[k];
        if (!WriteText(f, "key %.17g %.9g %.9g %.9g %.9g %.9g %.9g %.9g %s\n", key.Time,
          (double)key.Turn[0], (double)key.Turn[1], (double)key.Turn[2], (double)key.Turn[3],
          (double)key.Move[0], (double)key.Move[1], (double)key.Move[2], EaseName(key.Ease)))
          ok = false;
      }
    }

    return ok;
  }

  bool SaveFile(char const* path, File const& file, std::string& error)
  {
    error.clear();
    if (path == nullptr || path[0] == '\0')
    {
      error = "the pose keyframes path is empty";
      return false;
    }
    std::string label;
    std::string skeleton;
    std::string clip;
    if (!PrepareFileForSave(file, label, skeleton, clip, error))
      return false;
    std::string temporary = std::string(path) + ".tmp";
    FILE* f = nullptr;
    errno_t opened = fopen_s(&f, temporary.c_str(), "w");
    if (opened != 0 || f == nullptr)
    {
      OpenError(error, path, "writing");
      return false;
    }

    bool ok = WriteKeysText(f, file, label, skeleton, clip);
    if (std::fclose(f) != 0)
      ok = false;
    if (!ok)
    {
      std::remove(temporary.c_str());
      error = "the pose keyframes file could not be written";
      return false;
    }

    // (the new file takes the old one's place in one step: the old one isn't deleted first, so a failed replace loses
    // neither; then the new keyframes stay in the .tmp beside it; the review, 2026-11-16)
    if (!ReplaceWithTemporary(temporary.c_str(), path))
    {
      char text[1024];
      _snprintf_s(text, sizeof(text), _TRUNCATE, "cannot replace \"%s\" (the new keyframes are in \"%s\")", path,
        temporary.c_str());
      error = text;
      return false;
    }
    return true;
  }

  bool LoadFile(char const* path, File& file, std::string& error)
  {
    error.clear();
    if (path == nullptr || path[0] == '\0')
    {
      error = "the pose keyframes path is empty";
      return false;
    }
    FILE* f = nullptr;
    errno_t opened = fopen_s(&f, path, "r");
    if (opened != 0 || f == nullptr)
    {
      OpenError(error, path, "reading");
      return false;
    }
    File loaded;
    bool ok = ReadPoseFile(f, loaded, error);
    std::fclose(f);
    if (!ok)
      return false;
    file = loaded;
    error.clear();
    return true;
  }

  bool SaveFile(wchar_t const* path, File const& file, std::string& error)
  {
    error.clear();
    if (path == nullptr || path[0] == L'\0')
    {
      error = "the pose keyframes path is empty";
      return false;
    }
    std::string label;
    std::string skeleton;
    std::string clip;
    if (!PrepareFileForSave(file, label, skeleton, clip, error))
      return false;
    std::wstring temporary = std::wstring(path) + L".tmp";
    FILE* f = nullptr;
    errno_t opened = _wfopen_s(&f, temporary.c_str(), L"w");
    if (opened != 0 || f == nullptr)
    {
      error = "the file can't be written there (the folder isn't there or can't be written to)";
      return false;
    }
    bool ok = WriteKeysText(f, file, label, skeleton, clip);
    if (std::fclose(f) != 0)
      ok = false;
    if (!ok)
    {
      _wremove(temporary.c_str());
      error = "the pose keyframes file could not be written";
      return false;
    }
    if (!ReplaceWithTemporary(temporary.c_str(), path))
    {
      error = "the file there can't be replaced (the new keyframes are in the same name with .tmp added, beside it)";
      return false;
    }
    return true;
  }

  bool LoadFile(wchar_t const* path, File& file, std::string& error)
  {
    error.clear();
    if (path == nullptr || path[0] == L'\0')
    {
      error = "the pose keyframes path is empty";
      return false;
    }
    FILE* f = nullptr;
    errno_t opened = _wfopen_s(&f, path, L"r");
    if (opened != 0 || f == nullptr)
    {
      error = "the file can't be opened";
      return false;
    }
    File loaded;
    bool ok = ReadPoseFile(f, loaded, error);
    std::fclose(f);
    if (!ok)
      return false;
    file = loaded;
    error.clear();
    return true;
  }
}
