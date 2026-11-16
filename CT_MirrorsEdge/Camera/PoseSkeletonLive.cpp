#include "PoseSkeletonLive.h"
#include <Windows.h>
#include <cstdio>
#include <cstring>
#include <deque>
#include <string>
#include <vector>

namespace pose_skeleton_live
{
  namespace
  {
    const int TableCount = (int)(sizeof(pose_skeletons::Skeletons) / sizeof(pose_skeletons::Skeletons[0]));

    bool Pointer(__int64 p)
    {
      return p >= 0x10000 && p < 0x7FFFFFFF0000;
    }

    void Why(Body& out, char const* text)
    {
      strncpy_s(out.Why, text, _TRUNCATE);
    }

    // A name copied if it is plain text without spaces that ends within the room: its length, or -1. (The game's
    // bone names are letters, digits and '_', its asset names those and '/'. Inside the caller's __try.)
    int CopyName(char const* from, char* to, int room)
    {
      for (int n = 0; n < room; ++n)
      {
        char c = from[n];
        if (!c)
        {
          to[n] = 0;
          return n;
        }
        if (c < 0x21 || c > 0x7E)
          break;
        to[n] = c;
      }
      to[0] = 0;
      return -1;
    }

    // The pose manager's parents: what the game's pose pass goes by
    bool ReadParents(__int64 manager, int bones, Body& out)
    {
      __try
      {
        unsigned const* parents = *(unsigned const* const*)(manager + ManagerParentsOffset);
        unsigned const* parentsEnd = *(unsigned const* const*)(manager + ManagerParentsOffset + 8);
        if (!Pointer((__int64)parents))
        {
          Why(out, "its pose manager has no parents list");
          return false;
        }
        // (the list's own length: asked for before the asset is trusted, not for the parents themselves, which
        // the editor has read this way since its first build)
        out.ParentsCounted = parentsEnd >= parents && parentsEnd - parents == bones;
        bool ordered = true;
        int unparented = 0;
        for (int i = 0; i < bones; ++i)
        {
          int p = (int)parents[i];
          if (p != -1 && !(p >= 0 && p < i))
            ordered = false;
          out.Parents[i] = (short)(ordered ? p : -1);
          if (i > 0 && p == -1)
            ++unparented;
        }
        if (!ordered)
        {
          Why(out, "its bones' parents aren't in order (a parent after its child)");
          return false;
        }
        out.Unparented = unparented;
        out.ParentsOk = true;
        return true;
      }
      __except (EXCEPTION_EXECUTE_HANDLER)
      {
        out.ParentsOk = false;
        Why(out, "its pose manager's parents couldn't be read");
        return false;
      }
    }

    // The rig's channels: a bone has one of its own if its entry in the bone -> channel list names a channel whose
    // entry in the channel -> bone list names the bone back (a bone without one has 0 there, the first channel's)
    void ReadChannels(__int64 manager, int bones, Body& out)
    {
      __try
      {
        int channels = *(int const*)(manager + ManagerChannelsOffset);
        unsigned const* boneChannel = *(unsigned const* const*)(manager + ManagerBoneChannelOffset);
        unsigned const* boneChannelEnd = *(unsigned const* const*)(manager + ManagerBoneChannelOffset + 8);
        unsigned const* channelBone = *(unsigned const* const*)(manager + ManagerChannelBoneOffset);
        unsigned const* channelBoneEnd = *(unsigned const* const*)(manager + ManagerChannelBoneOffset + 8);
        if (channels <= 0 || channels > 8192 || !Pointer((__int64)boneChannel) || !Pointer((__int64)channelBone) ||
            boneChannelEnd < boneChannel || channelBoneEnd < channelBone || boneChannelEnd - boneChannel != bones ||
            channelBoneEnd - channelBone != channels)
          return;
        for (int i = 0; i < bones; ++i)
        {
          unsigned channel = boneChannel[i];
          out.Channel[i] = channel < (unsigned)channels && channelBone[channel] == (unsigned)i;
        }
        out.Channels = channels;
        out.ChannelsOk = true;
      }
      __except (EXCEPTION_EXECUTE_HANDLER)
      {
        out.ChannelsOk = false;
        memset(out.Channel, 0, sizeof(out.Channel));
      }
    }

    // The SkeletonAsset the manager was made from, taken only if every check holds
    void ReadAsset(__int64 gameBase, __int64 manager, int bones, __int64& assetOut, Body& out)
    {
      assetOut = 0;
      __try
      {
        if (!out.ParentsCounted)
        {
          Why(out, "its pose manager's parents list isn't as long as its bone count");
          return;
        }
        __int64 asset = *(__int64 const*)(manager + ManagerAssetOffset);
        if (!Pointer(asset) || (asset & 7) != 0)
        {
          Why(out, "its pose manager names no asset");
          return;
        }
        if (*(__int64 const*)asset != gameBase + SkeletonAssetVTableRva)
        {
          Why(out, "what its pose manager names isn't a SkeletonAsset (another vtable)");
          return;
        }
        char const* const* names = *(char const* const* const*)(asset + AssetBoneNamesOffset);
        unsigned const* hashes = *(unsigned const* const*)(asset + AssetBoneHashesOffset);
        int const* hierarchy = *(int const* const*)(asset + AssetHierarchyOffset);
        if (!Pointer((__int64)names) || !Pointer((__int64)hashes) || !Pointer((__int64)hierarchy))
        {
          Why(out, "the asset's bone lists aren't there");
          return;
        }
        unsigned nameCount = *((unsigned const*)names - 1), hashCount = *(hashes - 1);
        unsigned parentCount = *((unsigned const*)hierarchy - 1);
        if (nameCount != (unsigned)bones || hashCount != (unsigned)bones || parentCount != (unsigned)bones)
        {
          _snprintf_s(out.Why, sizeof(out.Why), _TRUNCATE, "the asset has %u names, %u hashes and %u parents for the "
            "body's %d bones", nameCount, hashCount, parentCount, bones);
          return;
        }
        int differ = 0;
        for (int i = 0; i < bones; ++i)
        {
          char const* name = names[i];
          if (!Pointer((__int64)name) || CopyName(name, out.Names[i], NameBytes) <= 0)
          {
            _snprintf_s(out.Why, sizeof(out.Why), _TRUNCATE, "the asset's bone %d has no name in plain text", i);
            return;
          }
          if (NameHash(out.Names[i]) != hashes[i])
          {
            _snprintf_s(out.Why, sizeof(out.Why), _TRUNCATE, "the asset's bone %d's name doesn't hash to the game's "
              "hash of it", i);
            return;
          }
          int p = hierarchy[i];
          if (p != -1 && !(p >= 0 && p < i))
          {
            Why(out, "the asset's parents aren't in order");
            return;
          }
          out.AssetParents[i] = (short)p;
          // The manager's parents are the asset's, or none (a bone the rig lacks)
          if (out.Parents[i] != (short)p)
          {
            if (out.Parents[i] != -1)
            {
              _snprintf_s(out.Why, sizeof(out.Why), _TRUNCATE, "bone %d's parent in the pose manager isn't the asset's", i);
              return;
            }
            ++differ;
          }
        }
        char const* assetName = *(char const* const*)(asset + AssetNameOffset);
        if (!Pointer((__int64)assetName) || CopyName(assetName, out.Asset, AssetBytes) <= 0)
        {
          Why(out, "the asset's name can't be read");
          return;
        }
        out.Differ = differ;
        out.Why[0] = 0;
        out.AssetOk = true;
        assetOut = asset;
      }
      __except (EXCEPTION_EXECUTE_HANDLER)
      {
        out.AssetOk = false;
        Why(out, "the asset couldn't be read (gone?)");
      }
    }

    // Whether the body's own data names that asset (the body's entity keeps its data, and so its skeleton, loaded)
    int DataNames(__int64 animatable, __int64 asset)
    {
      __try
      {
        if (!Pointer(animatable))
          return -1;
        __int64 data = *(__int64 const*)(animatable + EntityDataOffset);
        if (!Pointer(data))
          return -1;
        __int64 master = *(__int64 const*)(data + DataMasterSkeletonOffset);
        if (!Pointer(master))
          return -1;
        return *(__int64 const*)(master + MasterSkeletonOffset) == asset ? 1 : 0;
      }
      __except (EXCEPTION_EXECUTE_HANDLER)
      {
        return -1;
      }
    }

    // Tables that stand for several of the game's assets (the same bones and parents; from the game's data)
    struct Alias
    {
      char const* Asset;
      char const* Table;
    };
    const Alias Aliases[] = {
      { "characters/skeletons/skeleton_female_heels", "characters/skeletons/skeleton_male" },
      { "characters/skeletons/skeleton_female_balc", "characters/skeletons/skeleton_male_balc" },
      { "characters/skeletons/skeleton_female_heels_balc", "characters/skeletons/skeleton_male_balc" },
      { "animations/skeletons/baton_ske", "animations/skeletons/weapon_ske" },
    };

    // Whether a body's parents are a skeleton's: the same, or none where the skeleton has one. exact: all the same.
    bool Fits(Body const& body, int bones, short const* parents, bool& exact)
    {
      exact = true;
      if (bones != body.Bones)
        return false;
      for (int i = 0; i < bones; ++i)
      {
        if (body.Parents[i] == parents[i])
          continue;
        if (body.Parents[i] != -1)
          return false;
        exact = false;
      }
      return true;
    }

    // A skeleton made from the game's own names
    struct Made
    {
      std::string Asset, Label;
      std::vector<std::string> NameText;
      std::vector<char const*> Names;
      std::vector<short> Parents;
      pose_skeletons::Skeleton Table;
    };
    std::deque<Made> g_Made;   // (a deque: what it holds never moves)

    bool IsPlacementBone(char const* name)
    {
      static char const* const placement[] = { "Reference", "AITrajectory", "Trajectory", "TrajectoryEnd", "Connect",
        "ConnectEnd", "CameraBase", "CameraJoint" };
      for (char const* p : placement)
        if (!strcmp(name, p))
          return true;
      // (the tables' own text for the men's bone 161 names both: "BlendshapeDriver / FacePosesAnimation")
      return !strncmp(name, "BlendshapeDriver", 16) || !strncmp(name, "FacePosesAnimation", 18);
    }
  }

  unsigned NameHash(char const* name)
  {
    unsigned h = 0x811C9DC5u;
    for (unsigned char const* p = (unsigned char const*)name; *p; ++p)
    {
      h *= 0x01000193u;
      h ^= *p;
    }
    return h;
  }

  bool Read(__int64 gameBase, __int64 animatable, __int64 manager, int bones, Body& out)
  {
    memset(&out, 0, sizeof(out));
    out.Bones = bones;
    out.DataAgrees = -1;
    if (bones <= 0 || bones > MaxBones || !Pointer(manager))
    {
      Why(out, "it has no pose manager");
      return false;
    }
    if (!ReadParents(manager, bones, out))
      return false;
    ReadChannels(manager, bones, out);
    __int64 asset = 0;
    ReadAsset(gameBase, manager, bones, asset, out);
    if (out.AssetOk)
      out.DataAgrees = DataNames(animatable, asset);
    return true;
  }

  bool MayPose(Body const& body, int bone)
  {
    return body.ParentsOk && bone >= 0 && bone < body.Bones && body.Parents[bone] >= 0 &&
      (!body.ChannelsOk || body.Channel[bone]);
  }

  int HeldRootOf(int bones, char const* const* names, short const* parents)
  {
    if (bones <= 0 || bones > MaxBones || !names || !parents)
      return -1;
    int top = 0;
    for (int i = 0; i < bones; ++i)
      if (names[i] && !strcmp(names[i], "AITrajectory"))
      {
        top = i;
        break;
      }
    // How many bones hang below each (a parent comes before its child)
    int below[MaxBones];
    for (int i = 0; i < bones; ++i)
      below[i] = 0;
    for (int i = bones - 1; i > 0; --i)
    {
      int p = parents[i];
      if (p >= 0 && p < i)
        below[p] += below[i] + 1;
    }
    int best = -1, most = -1;
    for (int i = 1; i < bones; ++i)
      if (parents[i] == top && names[i] && !IsPlacementBone(names[i]) && below[i] > most)
      {
        most = below[i];
        best = i;
      }
    return best;
  }

  int Count()
  {
    return TableCount + (int)g_Made.size();
  }

  bool IsMade(int index)
  {
    return index >= TableCount && index < Count();
  }

  pose_skeletons::Skeleton const& At(int index)
  {
    if (index >= TableCount && index < Count())
      return g_Made[(size_t)(index - TableCount)].Table;
    return pose_skeletons::Skeletons[index >= 0 && index < TableCount ? index : 0];
  }

  int Find(Body const& body)
  {
    if (!body.ParentsOk || body.Bones <= 0 || body.Bones > MaxBones)
      return -1;
    bool exact = false;
    if (body.AssetOk)
    {
      // The table of that asset (or of the asset it shares its bones with)
      char const* name = body.Asset;
      for (Alias const& alias : Aliases)
        if (!_stricmp(body.Asset, alias.Asset))
          name = alias.Table;
      for (int k = 0; k < TableCount; ++k)
      {
        pose_skeletons::Skeleton const& s = pose_skeletons::Skeletons[k];
        if (!_stricmp(s.Asset, name) && Fits(body, s.Bones, s.Parents, exact))
          return k;
      }
      // An asset the tables don't have (or have with other bones): one made from its own names
      for (size_t m = 0; m < g_Made.size(); ++m)
      {
        Made const& made = g_Made[m];
        if (_stricmp(made.Asset.c_str(), body.Asset) || (int)made.Parents.size() != body.Bones)
          continue;
        bool same = true;
        for (int i = 0; i < body.Bones && same; ++i)
          same = made.Parents[(size_t)i] == body.AssetParents[i] && made.NameText[(size_t)i] == body.Names[i];
        if (same)
          return TableCount + (int)m;
      }
      if ((int)g_Made.size() >= MaxMade)
        return -1;
      g_Made.emplace_back();
      Made& made = g_Made.back();
      made.Asset = body.Asset;
      size_t slash = made.Asset.find_last_of('/');
      made.Label = (slash == std::string::npos ? made.Asset : made.Asset.substr(slash + 1)) + " (bones read from the game)";
      made.NameText.resize((size_t)body.Bones);
      made.Names.resize((size_t)body.Bones);
      made.Parents.resize((size_t)body.Bones);
      for (int i = 0; i < body.Bones; ++i)
      {
        made.NameText[(size_t)i] = body.Names[i];
        made.Parents[(size_t)i] = body.AssetParents[i];
      }
      for (int i = 0; i < body.Bones; ++i)
        made.Names[(size_t)i] = made.NameText[(size_t)i].c_str();
      made.Table.Asset = made.Asset.c_str();
      made.Table.Label = made.Label.c_str();
      made.Table.Bones = body.Bones;
      made.Table.Names = made.Names.data();
      made.Table.Parents = made.Parents.data();
      made.Table.HeldRoot = HeldRootOf(body.Bones, made.Names.data(), made.Parents.data());
      return TableCount + (int)g_Made.size() - 1;
    }
    // No proven asset: by bone count and parents. All the same wins. Else the only table the body's parents fit
    // with every unparented bone outside what a pose of that table holds (its held root and the bones below it): a
    // body with unparented bones can fit several tables of its bone count, and inside the held bones the names
    // would have to be right. So this takes the men (their bone 161 hangs on AITrajectory) and nothing doubtful.
    int fitting = -1, fits = 0;
    for (int k = 0; k < TableCount; ++k)
    {
      pose_skeletons::Skeleton const& s = pose_skeletons::Skeletons[k];
      if (!Fits(body, s.Bones, s.Parents, exact))
        continue;
      if (exact)
        return k;
      ++fits;
      bool held[MaxBones];
      bool outside = true;
      for (int i = 0; i < s.Bones; ++i)
      {
        int p = s.Parents[i];
        held[i] = i == s.HeldRoot || (p >= 0 && p < i && held[p]);
        if (held[i] && body.Parents[i] != s.Parents[i])
          outside = false;
      }
      if (outside)
        fitting = k;
    }
    return fits == 1 ? fitting : -1;
  }
}
