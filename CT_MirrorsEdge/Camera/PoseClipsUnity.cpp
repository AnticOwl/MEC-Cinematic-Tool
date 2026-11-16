#include "PoseClips.h"
#include "PoseClipsUnityData.h"
#include <Windows.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

// GAME > Pose > Animation clip, Unity humanoid clips (.anim): see PoseClips.h (LoadUnityAnim) and
// Research_Pose_Editor_2026-11-08.md section 24. A humanoid clip holds no bones: it holds Unity's "muscles" (95 curves,
// each a turn of a human bone between its limits; 89 are the body's, the other 6 the eyes' and jaw's, not played), the
// body's place and turn (RootT/RootQ: the mass centre, in units of the source character's size) and each hand's and
// foot's IK goal (place and turn, body space). Unity turns muscles
// into bone turns with numbers it keeps per character (each bone's zero-muscle pose, preQ/postQ); those aren't in the
// file, so they were fitted here (2026-11-11, offline, on the user's 36 clips) against the clips' own IK goals on a
// reference skeleton (Mixamo's T-pose): the hands then sit 8 cm and the feet 10 cm from their goals (rms), turned 8 deg.
// The arms and legs are then bent (two-bone IK) to reach the goals exactly, the hands and feet take the goals' turns,
// and the body stands its feet on the floor. The result is an ordinary clip of Mixamo's bones (rest: that T-pose), so
// the FBX clips' fit onto the game's skeletons plays it.
namespace
{
  namespace UD = pose_clips_unity_data;

  struct Q
  {
    double x, y, z, w;
  };
  struct V
  {
    double x, y, z;
  };

  Q Mul(Q a, Q b)
  {
    return { a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
      a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z };
  }
  Q Conj(Q a) { return { -a.x, -a.y, -a.z, a.w }; }
  Q Norm(Q a)
  {
    double l = sqrt(a.x * a.x + a.y * a.y + a.z * a.z + a.w * a.w);
    if (!(l > 1e-12) || !std::isfinite(l))
      return { 0.0, 0.0, 0.0, 1.0 };
    return { a.x / l, a.y / l, a.z / l, a.w / l };
  }
  V Add(V a, V b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
  V Sub(V a, V b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
  double Dot(V a, V b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
  V Cross(V a, V b) { return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; }
  double Len(V a) { return sqrt(Dot(a, a)); }
  V Scale(V a, double s) { return { a.x * s, a.y * s, a.z * s }; }
  // Unit length, or zero for a zero vector
  V Unit(V a)
  {
    double l = Len(a);
    return Scale(a, 1.0 / (l > 1e-12 ? l : 1e-12));
  }
  // The part of v across unit d
  V Perp(V v, V d) { return Sub(v, Scale(d, Dot(v, d))); }
  V Rot(Q q, V v)
  {
    Q p = Mul(Mul(q, Q{ v.x, v.y, v.z, 0.0 }), Conj(q));
    return { p.x, p.y, p.z };
  }
  // The shortest turn taking unit u to unit v (opposite: half a turn about an axis across u)
  Q Between(V u, V v)
  {
    V c = Cross(u, v);
    double w = 1.0 + Dot(u, v);
    if (w < 1e-8)
    {
      V a = Cross(u, V{ 1.0, 0.0, 0.0 });
      if (Len(a) < 1e-6)
        a = Cross(u, V{ 0.0, 1.0, 0.0 });
      a = Unit(a);
      return { a.x, a.y, a.z, 0.0 };
    }
    return Norm({ c.x, c.y, c.z, w });
  }
  // A turn by angle (radians) about unit axis a
  Q AxisAngle(V a, double angle)
  {
    double s = sin(angle * 0.5);
    return { a.x * s, a.y * s, a.z * s, cos(angle * 0.5) };
  }
  V ToV(double const* p) { return { p[0], p[1], p[2] }; }
  Q ToQ(double const* p) { return { p[0], p[1], p[2], p[3] }; }

  std::string Utf8(std::wstring const& w)
  {
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n : 0, '\0');
    if (n > 0)
      WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
  }

  // ---- Unity's binary SerializedFile (an asset as Unity writes it with its type trees), read by its own type tree

  // Unity's common strings: a type tree name whose offset has the top bit set is one of these (the table only grows
  // between versions, so the offsets hold)
  const char CommonStrings[] = "AABB\0" "AnimationClip\0" "AnimationCurve\0" "AnimationState\0" "Array\0" "Base\0"
    "BitField\0" "bitset\0" "bool\0" "char\0" "ColorRGBA\0" "Component\0" "data\0" "deque\0" "double\0"
    "dynamic_array\0" "FastPropertyName\0" "first\0" "float\0" "Font\0" "GameObject\0" "Generic Mono\0" "GradientNEW\0"
    "GUID\0" "GUIStyle\0" "int\0" "list\0" "long long\0" "map\0" "Matrix4x4f\0" "MdFour\0" "MonoBehaviour\0"
    "MonoScript\0" "m_ByteSize\0" "m_Curve\0" "m_EditorClassIdentifier\0" "m_EditorHideFlags\0" "m_Enabled\0"
    "m_ExtensionPtr\0" "m_GameObject\0" "m_Index\0" "m_IsArray\0" "m_IsStatic\0" "m_MetaFlag\0" "m_Name\0"
    "m_ObjectHideFlags\0" "m_PrefabInternal\0" "m_PrefabParentObject\0" "m_Script\0" "m_StaticEditorFlags\0" "m_Type\0"
    "m_Version\0" "Object\0" "pair\0" "PPtr<Component>\0" "PPtr<GameObject>\0" "PPtr<Material>\0"
    "PPtr<MonoBehaviour>\0" "PPtr<MonoScript>\0" "PPtr<Object>\0" "PPtr<Prefab>\0" "PPtr<Sprite>\0"
    "PPtr<TextAsset>\0" "PPtr<Texture>\0" "PPtr<Texture2D>\0" "PPtr<Transform>\0" "Prefab\0" "Quaternionf\0"
    "Rectf\0" "RectInt\0" "RectOffset\0" "second\0" "set\0" "short\0" "size\0" "SInt16\0" "SInt32\0" "SInt64\0"
    "SInt8\0" "staticvector\0" "string\0" "TextAsset\0" "TextMesh\0" "Texture\0" "Texture2D\0" "Transform\0"
    "TypelessData\0" "UInt16\0" "UInt32\0" "UInt64\0" "UInt8\0" "unsigned int\0" "unsigned long long\0"
    "unsigned short\0" "vector\0" "Vector2f\0" "Vector3f\0" "Vector4f\0" "m_ScriptingClassIdentifier\0" "Gradient\0"
    "Type*\0" "int2_storage\0" "int3_storage\0" "BoundsInt\0" "m_CorrespondingSourceObject\0" "m_PrefabInstance\0"
    "m_PrefabAsset\0" "FileSize\0" "Hash128\0";

  class Reader
  {
  public:
    Reader(unsigned char const* data, size_t size, bool little) : Data(data), Size(size), Little(little) {}
    unsigned char const* Data;
    size_t Size;
    size_t Pos = 0;
    bool Little;
    bool Bad = false;

    template <typename T> T Get()
    {
      T v{};
      if (Size - Pos < sizeof(T) || Pos > Size)
      {
        Bad = true;
        Pos = Size;
        return v;
      }
      unsigned char b[sizeof(T)];
      memcpy(b, Data + Pos, sizeof(T));
      if (!Little)
        std::reverse(b, b + sizeof(T));
      memcpy(&v, b, sizeof(T));
      Pos += sizeof(T);
      return v;
    }
    void Skip(unsigned long long n)
    {
      if (n > Size - Pos)
      {
        Bad = true;
        Pos = Size;
      }
      else
        Pos += (size_t)n;
    }
    void Align()
    {
      size_t p = (Pos + 3) & ~(size_t)3;
      if (p > Size)
      {
        Bad = true;
        Pos = Size;
      }
      else
        Pos = p;
    }
    std::string CStr()
    {
      size_t e = Pos;
      while (e < Size && Data[e])
        ++e;
      if (e >= Size)
      {
        Bad = true;
        Pos = Size;
        return std::string();
      }
      std::string s((char const*)Data + Pos, e - Pos);
      Pos = e + 1;
      return s;
    }
  };

  struct TreeNode
  {
    std::string Type, Name;
    int Level = 0;
    bool IsArray = false;
    unsigned Meta = 0;
    std::vector<int> Kids;
    bool AllPrim = false;                 // only primitives below it (no arrays or strings): its items are read flat
    std::vector<int> Leaves;              // AllPrim: its primitives, in read order
  };

  struct UnityType
  {
    int ClassId = 0;
    std::vector<TreeNode> Nodes;
  };

  struct UnityObject
  {
    unsigned long long Start = 0, Size = 0;
    int ClassId = 0;
    int Type = -1;                        // its type (UnityType) or -1
  };

  // A field's value as read: a number, a string, named fields, or array items (only kept where asked for). An array of
  // primitives, or of structs of primitives only (a curve's keys), is kept flat: Stride numbers an item, the element's
  // node FlatElem in FlatTree naming them (its Leaves). Names point into the type tree (it outlives the values).
  struct Value
  {
    double Num = 0.0;
    std::string Str;
    std::vector<std::string const*> Names;
    std::vector<Value> Fields;
    std::vector<Value> Items;
    std::vector<double> Flat;
    std::vector<TreeNode> const* FlatTree = nullptr;
    int FlatElem = -1;
    size_t Stride = 0;
    Value const* Field(char const* name) const
    {
      for (size_t i = 0; i < Names.size(); ++i)
        if (*Names[i] == name)
          return &Fields[i];
      return nullptr;
    }
    // A flat item's slot for a name, or -1
    int Slot(char const* name) const
    {
      if (!FlatTree || FlatElem < 0)
        return -1;
      std::vector<int> const& leaves = (*FlatTree)[(size_t)FlatElem].Leaves;
      for (size_t s = 0; s < leaves.size() && s < Stride; ++s)
        if ((*FlatTree)[(size_t)leaves[s]].Name == name)
          return (int)s;
      return -1;
    }
    double NumOf(char const* name, double otherwise) const
    {
      Value const* v = Field(name);
      return v ? v->Num : otherwise;
    }
  };

  // A primitive's size and kind ('u' unsigned, 'i' signed, 'f' float), 0 if the type isn't one
  int Primitive(std::string const& t, char& kind)
  {
    struct P { char const* Name; int Size; char Kind; };
    static const P prims[] = { { "bool", 1, 'u' }, { "char", 1, 'u' }, { "UInt8", 1, 'u' }, { "SInt8", 1, 'i' },
      { "SInt16", 2, 'i' }, { "short", 2, 'i' }, { "UInt16", 2, 'u' }, { "unsigned short", 2, 'u' }, { "int", 4, 'i' },
      { "SInt32", 4, 'i' }, { "UInt32", 4, 'u' }, { "unsigned int", 4, 'u' }, { "Type*", 4, 'u' }, { "float", 4, 'f' },
      { "double", 8, 'f' }, { "long long", 8, 'i' }, { "SInt64", 8, 'i' }, { "UInt64", 8, 'u' },
      { "unsigned long long", 8, 'u' }, { "FileSize", 8, 'u' } };
    for (auto const& p : prims)
      if (t == p.Name)
      {
        kind = p.Kind;
        return p.Size;
      }
    return 0;
  }

  double ReadPrimitive(Reader& r, int size, char kind)
  {
    switch (size)
    {
    case 1: return kind == 'i' ? (double)r.Get<signed char>() : (double)r.Get<unsigned char>();
    case 2: return kind == 'i' ? (double)r.Get<short>() : (double)r.Get<unsigned short>();
    case 4: return kind == 'f' ? (double)r.Get<float>() : kind == 'i' ? (double)r.Get<int>() : (double)r.Get<unsigned>();
    default: return kind == 'f' ? r.Get<double>() : kind == 'i' ? (double)r.Get<long long>() :
      (double)r.Get<unsigned long long>();
    }
  }

  // What one object's read may keep: a crafted file can't make it hold more (a real clip keeps a few thousand values
  // and its keys' numbers flat: Orange justice 1,697 frames, 1.1 M numbers)
  struct Budget
  {
    size_t Values = 0, Flat = 0;
    bool Over = false;
    bool Take(size_t values, size_t flat)
    {
      Values += values;
      Flat += flat;
      if (Values > 1000000 || Flat > 40000000)
        Over = true;
      return !Over;
    }
  };

  // A struct of primitives only, its numbers appended to out (the same alignment as ReadNode)
  bool ReadFlat(Reader& r, std::vector<TreeNode> const& nodes, int i, std::vector<double>& out, int depth)
  {
    if (depth > 64 || r.Bad)
      return false;
    TreeNode const& n = nodes[i];
    if (n.Kids.empty())
    {
      char kind = 0;
      int prim = Primitive(n.Type, kind);
      if (!prim)
        return false;
      out.push_back(ReadPrimitive(r, prim, kind));
    }
    else
      for (int k : n.Kids)
        if (!ReadFlat(r, nodes, k, out, depth + 1))
          return false;
    if (n.Meta & 0x4000)
      r.Align();
    return !r.Bad;
  }

  // One field by its type tree node (into out, or skipped if out is null). False if the data runs out or doesn't fit,
  // an array's item reads no bytes (a broken layout: it could loop for hours), or the read keeps more than its budget.
  bool ReadNode(Reader& r, std::vector<TreeNode> const& nodes, int i, Value* out, int depth, Budget& budget)
  {
    if (depth > 64 || r.Bad || budget.Over)
      return false;
    TreeNode const& n = nodes[i];
    char kind = 0;
    int prim = Primitive(n.Type, kind);
    if (prim && n.Kids.empty())
    {
      double v = ReadPrimitive(r, prim, kind);
      if (out)
        out->Num = v;
    }
    else if (n.Type == "string")
    {
      int len = r.Get<int>();
      if (len < 0 || (size_t)len > r.Size - r.Pos)
        return false;
      if (out)
        out->Str.assign((char const*)r.Data + r.Pos, (size_t)len);
      r.Skip((unsigned)len);
      r.Align();                          // (its Array child is aligned)
    }
    else if (n.Type == "TypelessData")
    {
      int len = r.Get<int>();
      if (len < 0)
        return false;
      r.Skip((unsigned)len);
    }
    else if (!n.Kids.empty() && nodes[n.Kids[0]].IsArray)
    {
      TreeNode const& arr = nodes[n.Kids[0]];
      if (arr.Kids.size() < 2)
        return false;
      int len = r.Get<int>();
      int elem = arr.Kids[1];
      TreeNode const& en = nodes[elem];
      char ek = 0;
      int esize = Primitive(en.Type, ek);
      if (len < 0 || (size_t)len > r.Size - r.Pos)
        return false;
      if (esize && en.Kids.empty())
      {
        if ((unsigned long long)len * (unsigned long long)esize > r.Size - r.Pos)
          return false;
        if (out)
        {
          if (!budget.Take(0, (size_t)len))
            return false;
          out->Stride = 1;
          out->FlatTree = &nodes;
          out->FlatElem = elem;
          out->Flat.reserve((size_t)len);
          for (int k = 0; k < len; ++k)
            out->Flat.push_back(ReadPrimitive(r, esize, ek));
        }
        else
          r.Skip((unsigned long long)len * (unsigned long long)esize);
      }
      else if (en.AllPrim && !en.Leaves.empty())
      {
        // structs of primitives (a curve's keys): kept flat
        if (out)
        {
          if (!budget.Take(0, (size_t)len * en.Leaves.size()))
            return false;
          out->Stride = en.Leaves.size();
          out->FlatTree = &nodes;
          out->FlatElem = elem;
          out->Flat.reserve((size_t)len * en.Leaves.size());
        }
        for (int k = 0; k < len; ++k)
        {
          size_t at = r.Pos;
          if (!(out ? ReadFlat(r, nodes, elem, out->Flat, depth + 1) : ReadNode(r, nodes, elem, nullptr, depth + 1, budget)) ||
            r.Pos == at)
            return false;
        }
      }
      else
      {
        for (int k = 0; k < len; ++k)
        {
          Value* item = nullptr;
          if (out)
          {
            if (!budget.Take(1, 0))
              return false;
            out->Items.emplace_back();
            item = &out->Items.back();
          }
          size_t at = r.Pos;
          if (!ReadNode(r, nodes, elem, item, depth + 1, budget) || r.Pos == at)
            return false;
        }
      }
      if (arr.Meta & 0x4000)
        r.Align();
    }
    else if (n.IsArray)
    {
      if (n.Kids.size() < 2)
        return false;
      int len = r.Get<int>();
      if (len < 0 || (size_t)len > r.Size - r.Pos)
        return false;
      for (int k = 0; k < len; ++k)
      {
        Value* item = nullptr;
        if (out)
        {
          if (!budget.Take(1, 0))
            return false;
          out->Items.emplace_back();
          item = &out->Items.back();
        }
        size_t at = r.Pos;
        if (!ReadNode(r, nodes, n.Kids[1], item, depth + 1, budget) || r.Pos == at)
          return false;
      }
    }
    else
    {
      if (out)
      {
        if (!budget.Take(n.Kids.size(), 0))
          return false;
        out->Names.reserve(n.Kids.size());
        out->Fields.reserve(n.Kids.size());
      }
      for (int k : n.Kids)
      {
        Value* f = nullptr;
        if (out)
        {
          out->Names.push_back(&nodes[k].Name);
          out->Fields.emplace_back();
          f = &out->Fields.back();
        }
        if (!ReadNode(r, nodes, k, f, depth + 1, budget))
          return false;
      }
    }
    if (n.Meta & 0x4000)
      r.Align();
    return !r.Bad;
  }

  // The file's types and objects. False and why if it isn't one this reads.
  bool ReadSerializedFile(std::vector<char> const& data, unsigned& version, std::string& unityVersion, bool& little,
    std::vector<UnityType>& types, std::vector<UnityObject>& objects, std::string& error)
  {
    unsigned char const* d = (unsigned char const*)data.data();
    if (data.size() >= 5 && !memcmp(d, "%YAML", 5))
    {
      error = "is a text (YAML) Unity asset: only binary ones are read (Unity saves .anim files binary with Edit > "
        "Project Settings > Editor > Asset Serialization: Force Binary)";
      return false;
    }
    Reader h(d, data.size(), false);
    unsigned metaSize = h.Get<unsigned>();
    unsigned long long fileSize = h.Get<unsigned>();
    version = h.Get<unsigned>();
    unsigned long long dataOffset = h.Get<unsigned>();
    (void)metaSize;
    if (h.Bad || version < 14 || version > 30)
    {
      error = "isn't a Unity binary asset this reads (" + (h.Bad ? std::string("too short") : "format " +
        std::to_string(version)) + "; text .anim files (YAML) aren't read yet)";
      return false;
    }
    little = h.Get<unsigned char>() == 0;
    h.Skip(3);
    if (version >= 22)
    {
      h.Get<unsigned>();                  // the metadata's size
      fileSize = h.Get<unsigned long long>();
      dataOffset = h.Get<unsigned long long>();
      h.Get<unsigned long long>();
    }
    if (h.Bad || fileSize > data.size() || dataOffset > data.size())
    {
      char text[200];
      _snprintf_s(text, sizeof(text), _TRUNCATE, "is cut short or isn't a Unity asset (its header says %llu bytes, the file "
        "has %llu)", fileSize, (unsigned long long)data.size());
      error = text;
      return false;
    }
    Reader r(d, data.size(), little);
    r.Pos = h.Pos;
    unityVersion = r.CStr();
    r.Get<int>();                         // the target platform
    bool treesOn = r.Get<unsigned char>() != 0;
    if (!treesOn)
    {
      error = "has no type trees (a built game's asset): its layout can't be read";
      return false;
    }
    int typeCount = r.Get<int>();
    if (r.Bad || typeCount < 0 || typeCount > 4096)
    {
      error = "has a broken type list";
      return false;
    }
    for (int t = 0; t < typeCount && !r.Bad; ++t)
    {
      UnityType type;
      type.ClassId = r.Get<int>();
      if (version >= 16)
        r.Get<unsigned char>();           // stripped
      if (version >= 17)
        r.Get<short>();                   // its script's type
      if ((version < 16 && type.ClassId < 0) || (version >= 16 && type.ClassId == 114))
        r.Skip(16);                       // the script's id
      r.Skip(16);                         // the type's hash
      int count = r.Get<int>();
      int stringSize = r.Get<int>();
      if (r.Bad || count <= 0 || count > 100000 || stringSize < 0 || (size_t)stringSize > r.Size - r.Pos)
      {
        error = "has a broken type tree";
        return false;
      }
      size_t nodeBytes = (size_t)(version >= 19 ? 32 : 24);
      if ((size_t)count * nodeBytes > r.Size - r.Pos)
      {
        error = "has a broken type tree";
        return false;
      }
      size_t stringsAt = r.Pos + (size_t)count * nodeBytes;
      if ((size_t)stringSize > r.Size - stringsAt)
      {
        error = "has a broken type tree";
        return false;
      }
      char const* local = (char const*)d + stringsAt;
      auto nameAt = [&](unsigned off, std::string& out) -> bool
      {
        if (off & 0x80000000u)
        {
          unsigned o = off & 0x7FFFFFFFu;
          if (o >= sizeof(CommonStrings) || (o > 0 && CommonStrings[o - 1] != '\0'))
            return false;
          out = CommonStrings + o;
          return true;
        }
        if (off >= (unsigned)stringSize)
          return false;
        size_t e = off;
        while (e < (size_t)stringSize && e - off <= 256 && local[e])
          ++e;
        if (e >= (size_t)stringSize || e - off > 256)
          return false;
        out.assign(local + off, e - off);
        return true;
      };
      type.Nodes.resize((size_t)count);
      std::vector<int> stack;
      for (int k = 0; k < count; ++k)
      {
        TreeNode& n = type.Nodes[(size_t)k];
        r.Get<unsigned short>();          // version
        n.Level = r.Get<unsigned char>();
        n.IsArray = (r.Get<unsigned char>() & 1) != 0;
        unsigned typeOff = r.Get<unsigned>(), nameOff = r.Get<unsigned>();
        r.Get<int>();                     // byte size
        r.Get<int>();                     // index
        n.Meta = r.Get<unsigned>();
        if (version >= 19)
          r.Get<unsigned long long>();    // the referenced type's hash
        if (!nameAt(typeOff, n.Type) || !nameAt(nameOff, n.Name))
        {
          error = "has a broken type tree name (outside its tables, or over 256 characters)";
          return false;
        }
        // the parent: the last node one level up
        while (!stack.empty() && type.Nodes[(size_t)stack.back()].Level >= n.Level)
          stack.pop_back();
        if (k > 0)
        {
          if (stack.empty() || n.Level != type.Nodes[(size_t)stack.back()].Level + 1)
          {
            error = "has a broken type tree (its levels)";
            return false;
          }
          type.Nodes[(size_t)stack.back()].Kids.push_back(k);
        }
        stack.push_back(k);
      }
      r.Pos = stringsAt + (size_t)stringSize;
      if (version >= 21)
      {
        int deps = r.Get<int>();
        if (deps < 0 || deps > 100000)
        {
          error = "has a broken type list";
          return false;
        }
        r.Skip((unsigned long long)deps * 4);
      }
      for (size_t k = type.Nodes.size(); k-- > 0;)
      {
        TreeNode& n = type.Nodes[k];
        char kind = 0;
        if (n.Kids.empty())
        {
          n.AllPrim = !n.IsArray && Primitive(n.Type, kind) > 0;
          if (n.AllPrim)
            n.Leaves.push_back((int)k);
        }
        else
        {
          n.AllPrim = !n.IsArray && n.Type != "string" && n.Type != "TypelessData" && !type.Nodes[(size_t)n.Kids[0]].IsArray;
          for (int c : n.Kids)
            n.AllPrim = n.AllPrim && type.Nodes[(size_t)c].AllPrim;
          if (n.AllPrim)
            for (int c : n.Kids)
            {
              std::vector<int>& kidLeaves = type.Nodes[(size_t)c].Leaves;
              n.Leaves.insert(n.Leaves.end(), kidLeaves.begin(), kidLeaves.end());
              std::vector<int>().swap(kidLeaves);
            }
        }
      }
      types.push_back(std::move(type));
    }
    int objectCount = r.Get<int>();
    if (r.Bad || objectCount < 0 || objectCount > 1000000)
    {
      error = "has a broken object list";
      return false;
    }
    for (int k = 0; k < objectCount && !r.Bad; ++k)
    {
      UnityObject o;
      r.Align();
      r.Get<long long>();                 // its id
      o.Start = version >= 22 ? r.Get<unsigned long long>() : r.Get<unsigned>();
      o.Size = r.Get<unsigned>();
      int typeId = r.Get<int>();
      if (version < 16)
      {
        o.ClassId = r.Get<unsigned short>();
        for (size_t t = 0; t < types.size(); ++t)
          if (types[t].ClassId == typeId)
            o.Type = (int)t;
      }
      else if (typeId >= 0 && (size_t)typeId < types.size())
      {
        o.Type = typeId;
        o.ClassId = types[(size_t)typeId].ClassId;
      }
      if (version >= 11 && version < 17)
        r.Get<short>();                   // its script's type
      if (version == 15 || version == 16)
        r.Get<unsigned char>();           // stripped
      o.Start += dataOffset;
      objects.push_back(o);
    }
    if (r.Bad)
    {
      error = "has a broken object list";
      return false;
    }
    return true;
  }

  // ---- curves

  struct Key
  {
    double Time, Val, In, Out;
  };

  // Unity's float curve at time t: Hermite between keys (a key with an infinite slope steps); held at its ends
  double Evaluate(std::vector<Key> const& keys, double t)
  {
    if (keys.empty())
      return 0.0;
    if (t <= keys.front().Time)
      return keys.front().Val;
    if (t >= keys.back().Time)
      return keys.back().Val;
    size_t lo = 0, hi = keys.size() - 1;
    while (hi - lo > 1)
    {
      size_t mid = (lo + hi) / 2;
      if (keys[mid].Time <= t)
        lo = mid;
      else
        hi = mid;
    }
    Key const& a = keys[lo];
    Key const& b = keys[hi];
    double dt = b.Time - a.Time;
    if (!(dt > 0.0))
      return b.Val;
    double m0 = a.Out, m1 = b.In;
    if (!std::isfinite(m0) || !std::isfinite(m1) || fabs(m0) > 1e6 || fabs(m1) > 1e6)
      return a.Val;
    double s = (t - a.Time) / dt, s2 = s * s, s3 = s2 * s;
    return (2 * s3 - 3 * s2 + 1) * a.Val + (s3 - 2 * s2 + s) * dt * m0 + (-2 * s3 + 3 * s2) * b.Val + (s3 - s2) * dt * m1;
  }

  // Unity's vectors and turns to the game's axes (Unity's +X is the character's right: x mirrored)
  V UnityV(double x, double y, double z) { return { -x, y, z }; }
  Q UnityQ(double x, double y, double z, double w) { return Norm({ x, -y, -z, w }); }

  double Percentile(std::vector<double> v, double p)
  {
    if (v.empty())
      return 0.0;
    std::sort(v.begin(), v.end());
    double pos = p / 100.0 * (double)(v.size() - 1);
    size_t lo = (size_t)floor(pos);
    if (lo + 1 >= v.size())
      return v.back();
    return v[lo] + (pos - (double)lo) * (v[lo + 1] - v[lo]);
  }

  // Unity's names for the reference's bones (the log's list; null for the fingers, named by the caller)
  char const* UnityBoneName(std::string const& b)
  {
    static const char* const pairs[][2] = { { "Hips", "Hips" }, { "Spine", "Spine" }, { "Spine1", "Chest" },
      { "Spine2", "UpperChest" }, { "Neck", "Neck" }, { "Head", "Head" }, { "HeadTop_End", "(the head's top)" },
      { "LeftShoulder", "LeftShoulder" }, { "LeftArm", "LeftUpperArm" }, { "LeftForeArm", "LeftLowerArm" },
      { "LeftHand", "LeftHand" }, { "LeftUpLeg", "LeftUpperLeg" }, { "LeftLeg", "LeftLowerLeg" },
      { "LeftFoot", "LeftFoot" }, { "LeftToeBase", "LeftToes" }, { "LeftToe_End", "(the left toes' tip)" },
      { "RightShoulder", "RightShoulder" }, { "RightArm", "RightUpperArm" }, { "RightForeArm", "RightLowerArm" },
      { "RightHand", "RightHand" }, { "RightUpLeg", "RightUpperLeg" }, { "RightLeg", "RightLowerLeg" },
      { "RightFoot", "RightFoot" }, { "RightToeBase", "RightToes" }, { "RightToe_End", "(the right toes' tip)" } };
    for (auto const& p : pairs)
      if (b == p[0])
        return p[1];
    return nullptr;
  }
}

std::shared_ptr<pose_clips::Clip const> pose_clips::LoadUnityAnim(std::wstring const& path, std::vector<char> const& data,
  std::string& error)
{
  std::wstring fileName = path.substr(path.find_last_of(L"\\/") == std::wstring::npos ? 0 : path.find_last_of(L"\\/") + 1);
  unsigned version = 0;
  std::string unityVersion;
  bool little = true;
  std::vector<UnityType> types;
  std::vector<UnityObject> objects;
  if (!ReadSerializedFile(data, version, unityVersion, little, types, objects, error))
    return nullptr;

  // Its animation clip (class 74): the fields used, read by the file's own layout to the object's end
  Value clipValue;
  bool found = false;
  for (UnityObject const& o : objects)
  {
    if (o.ClassId != 74 || o.Type < 0)
      continue;
    if (o.Start > data.size() || o.Size > data.size() - o.Start)
    {
      error = "has an animation clip past its end";
      return nullptr;
    }
    std::vector<TreeNode> const& nodes = types[(size_t)o.Type].Nodes;
    Reader r((unsigned char const*)data.data() + o.Start, (size_t)o.Size, little);
    Budget budget;
    static const char* const wanted[] = { "m_Name", "m_FloatCurves", "m_EditorCurves", "m_SampleRate",
      "m_AnimationClipSettings" };
    bool ok = true;
    for (int k : nodes[0].Kids)
    {
      bool want = false;
      for (char const* w : wanted)
        if (nodes[(size_t)k].Name == w)
          want = true;
      Value* f = nullptr;
      if (want)
      {
        clipValue.Names.push_back(&nodes[(size_t)k].Name);
        clipValue.Fields.emplace_back();
        f = &clipValue.Fields.back();
      }
      if (!ReadNode(r, nodes, k, f, 1, budget))
      {
        ok = false;
        break;
      }
    }
    if (nodes[0].Meta & 0x4000)
      r.Align();
    if (budget.Over)
    {
      error = "has an animation clip too big to read (over a million fields or 40 million numbers)";
      return nullptr;
    }
    if (!ok || r.Pos != r.Size)
    {
      char text[160];
      _snprintf_s(text, sizeof(text), _TRUNCATE, "has an animation clip whose layout didn't read to its end (format %u, "
        "Unity %s): it can't be trusted", version, unityVersion.c_str());
      error = text;
      return nullptr;
    }
    found = true;
    break;
  }
  if (!found)
  {
    error = "has no animation clip in it";
    return nullptr;
  }

  // Its curves on the character itself (path ""), by attribute: the float curves, then the editor's copies for any
  // attribute those lack
  std::vector<std::string> attrs;
  std::vector<std::vector<Key>> curves;
  std::vector<std::string> otherSeen;     // other objects' curves (path, attribute, type), each once
  std::vector<int> curveDropped;          // per curve taken: its keys left out as broken
  std::vector<std::string> brokenAttrs;   // attributes whose every key was broken (no copy stood in)
  int weightedKeys = 0;
  auto takeCurves = [&](char const* field, bool editor)
  {
    Value const* list = clipValue.Field(field);
    if (!list)
      return;
    for (Value const& c : list->Items)
    {
      Value const* attr = c.Field("attribute");
      Value const* pathV = c.Field("path");
      Value const* curve = c.Field("curve");
      Value const* keysV = curve ? curve->Field("m_Curve") : nullptr;
      if (!attr || !keysV)
        continue;
      if (pathV && !pathV->Str.empty())
      {
        Value const* script = c.Field("script");
        std::string key = pathV->Str + '\n' + attr->Str + '\n' + std::to_string((long long)c.NumOf("classID", 0.0)) + '\n' +
          std::to_string((long long)(script ? script->NumOf("m_FileID", 0.0) : 0.0)) + '\n' +
          std::to_string((long long)(script ? script->NumOf("m_PathID", 0.0) : 0.0));
        if (std::find(otherSeen.begin(), otherSeen.end(), key) == otherSeen.end())
          otherSeen.push_back(key);
        continue;
      }
      // taken already: an editor copy only stands in for a float curve that lost keys
      int taken = (int)(std::find(attrs.begin(), attrs.end(), attr->Str) - attrs.begin());
      if (taken == (int)attrs.size())
        taken = -1;
      if (taken >= 0 && !(editor && curveDropped[(size_t)taken] > 0))
        continue;
      // each key's numbers: kept flat (its struct is all numbers), else as fields
      size_t count = keysV->Stride ? keysV->Flat.size() / keysV->Stride : keysV->Items.size();
      auto slot = [&](char const* name) -> int { return keysV->Slot(name); };
      int sTime = slot("time"), sValue = slot("value"), sIn = slot("inSlope"), sOut = slot("outSlope"),
        sMode = slot("weightedMode"), sInW = slot("inWeight"), sOutW = slot("outWeight");
      auto num = [&](size_t k, int s, char const* name, double otherwise) -> double
      {
        if (keysV->Stride)
          return s >= 0 ? keysV->Flat[k * keysV->Stride + (size_t)s] : otherwise;
        return keysV->Items[k].NumOf(name, otherwise);
      };
      std::vector<Key> raw;
      raw.reserve(count);
      for (size_t k = 0; k < count; ++k)
      {
        double time = num(k, sTime, "time", 0.0), value = num(k, sValue, "value", 0.0);
        if (!std::isfinite(time) || !std::isfinite(value) || fabs(time) > 1e6 || fabs(value) > 1e6)
          continue;
        // Unity 2018+: a weighted side (its bit: In 1, Out 2) whose handle isn't the default third is played unweighted
        double mode = num(k, sMode, "weightedMode", 0.0);
        bool weightIn = mode == 1.0 || mode == 3.0, weightOut = mode == 2.0 || mode == 3.0;
        if ((weightIn && fabs(num(k, sInW, "inWeight", 1.0 / 3.0) - 1.0 / 3.0) > 1e-3) ||
          (weightOut && fabs(num(k, sOutW, "outWeight", 1.0 / 3.0) - 1.0 / 3.0) > 1e-3))
          ++weightedKeys;
        raw.push_back({ time, value, num(k, sIn, "inSlope", 0.0), num(k, sOut, "outSlope", 0.0) });
      }
      // keys out of time order: the longest run in order is kept (one bad time loses one key, not those after it)
      std::vector<size_t> tail;
      std::vector<long long> before(raw.size(), -1);
      for (size_t k = 0; k < raw.size(); ++k)
      {
        size_t lo = 0, hi = tail.size();
        while (lo < hi)
        {
          size_t mid = (lo + hi) / 2;
          if (raw[tail[mid]].Time <= raw[k].Time)
            lo = mid + 1;
          else
            hi = mid;
        }
        if (lo > 0)
          before[k] = (long long)tail[lo - 1];
        if (lo == tail.size())
          tail.push_back(k);
        else
          tail[lo] = k;
      }
      std::vector<Key> keys(tail.size());
      for (long long k = tail.empty() ? -1 : (long long)tail.back(), j = (long long)tail.size() - 1; k >= 0 && j >= 0;
        k = before[(size_t)k], --j)
        keys[(size_t)j] = raw[(size_t)k];
      int dropped = (int)(count - keys.size());
      if (keys.empty() && count > 0)
      {
        // every key broken: missing (the editor's copy, if any, stands in)
        if (taken < 0 && std::find(brokenAttrs.begin(), brokenAttrs.end(), attr->Str) == brokenAttrs.end())
          brokenAttrs.push_back(attr->Str);
        continue;
      }
      if (taken >= 0)
      {
        if (dropped < curveDropped[(size_t)taken])
        {
          curves[(size_t)taken] = std::move(keys);
          curveDropped[(size_t)taken] = dropped;
        }
        continue;
      }
      brokenAttrs.erase(std::remove(brokenAttrs.begin(), brokenAttrs.end(), attr->Str), brokenAttrs.end());
      attrs.push_back(attr->Str);
      curves.push_back(std::move(keys));
      curveDropped.push_back(dropped);
    }
  };
  takeCurves("m_FloatCurves", false);
  takeCurves("m_EditorCurves", true);
  int otherPaths = (int)otherSeen.size();
  int badKeys = 0;
  for (int d : curveDropped)
    badKeys += d;
  auto curveOf = [&](std::string const& a) -> int
  {
    for (size_t i = 0; i < attrs.size(); ++i)
      if (attrs[i] == a)
        return (int)i;
    return -1;
  };

  // The body (RootT, RootQ) is needed; the muscles found are counted
  static const char* const rootNames[7] = { "RootT.x", "RootT.y", "RootT.z", "RootQ.x", "RootQ.y", "RootQ.z", "RootQ.w" };
  int rootCurve[7];
  for (int k = 0; k < 7; ++k)
    rootCurve[k] = curveOf(rootNames[k]);
  int muscleCurve[UD::MuscleCount];
  int muscles = 0;
  for (int m = 0; m < UD::MuscleCount; ++m)
    if ((muscleCurve[m] = curveOf(UD::Muscles[m].Name)) >= 0)
      ++muscles;
  bool rootOk = true;
  for (int k = 0; k < 7; ++k)
    rootOk = rootOk && rootCurve[k] >= 0;
  if (!rootOk || muscles < 20)
  {
    std::string broken;
    for (int k = 0; k < 7; ++k)
      if (rootCurve[k] < 0 && std::find(brokenAttrs.begin(), brokenAttrs.end(), rootNames[k]) != brokenAttrs.end())
        broken += (broken.empty() ? "" : ", ") + std::string(rootNames[k]);
    int brokenMuscles = 0;
    for (int m = 0; m < UD::MuscleCount; ++m)
      if (muscleCurve[m] < 0 && std::find(brokenAttrs.begin(), brokenAttrs.end(), UD::Muscles[m].Name) != brokenAttrs.end())
        ++brokenMuscles;
    if (!broken.empty() || (muscles < 20 && muscles + brokenMuscles >= 20))
    {
      std::string what = broken;
      if (brokenMuscles > 0)
        what += (what.empty() ? "" : " and ") + std::to_string(brokenMuscles) + " muscle curves";
      error = "has broken curves: " + what + " had no key that's a usable number (not numbers, over a million, or out of "
        "time order)";
      return nullptr;
    }
    char text[200];
    _snprintf_s(text, sizeof(text), _TRUNCATE, "isn't a humanoid clip (%d of the %d body muscle curves, %s; a generic or "
      "legacy clip's bones aren't read)", muscles, UD::MuscleCount, rootOk ? "its body's curves" : "no body curves");
    error = text;
    return nullptr;
  }
  // The hands' and feet's IK goals (each needs all 7 curves)
  static const char* const goalNames[4] = { "LeftHand", "RightHand", "LeftFoot", "RightFoot" };
  int goalCurve[4][7];
  bool goalOk[4];
  for (int g = 0; g < 4; ++g)
  {
    goalOk[g] = true;
    static const char* const parts[7] = { "T.x", "T.y", "T.z", "Q.x", "Q.y", "Q.z", "Q.w" };
    for (int k = 0; k < 7; ++k)
      goalOk[g] = (goalCurve[g][k] = curveOf(std::string(goalNames[g]) + parts[k])) >= 0 && goalOk[g];
  }

  // Its time range and rate
  Value const* settings = clipValue.Field("m_AnimationClipSettings");
  double start = settings ? settings->NumOf("m_StartTime", 0.0) : 0.0;
  double stop = settings ? settings->NumOf("m_StopTime", 0.0) : 0.0;
  bool loopTime = settings && settings->NumOf("m_LoopTime", 0.0) != 0.0;
  bool mirror = settings && settings->NumOf("m_Mirror", 0.0) != 0.0;
  if (!(stop > start))
  {
    // (no settings: the keys' own span)
    start = 1e30;
    stop = -1e30;
    for (auto const& c : curves)
      if (!c.empty())
      {
        start = (std::min)(start, c.front().Time);
        stop = (std::max)(stop, c.back().Time);
      }
    if (!(stop >= start))
      start = stop = 0.0;
  }
  double rate = clipValue.NumOf("m_SampleRate", 30.0);
  if (!(rate >= 1.0 && rate <= 1000.0))
    rate = 30.0;
  double framesD = floor((stop - start) * rate + 0.5) + 1.0;
  if (!(framesD >= 1.0) || !std::isfinite(framesD))
  {
    error = "has a time range that can't be played";
    return nullptr;
  }
  std::string warnings;
  if (framesD > 30000.0)
  {
    warnings += "it's over 30000 frames (cut); ";
    framesD = 30000.0;
  }
  int frames = (int)framesD;

  // The reference skeleton
  const int B = UD::BoneCount;
  V refT[UD::BoneCount];
  Q zero[UD::BoneCount];
  int parent[UD::BoneCount];
  double massSum = 0.0;
  for (int b = 0; b < B; ++b)
  {
    refT[b] = ToV(UD::Bones[b].T);
    zero[b] = ToQ(UD::Bones[b].Zero);
    parent[b] = UD::Bones[b].Parent;
    massSum += UD::Bones[b].Mass;
  }
  auto boneOf = [&](char const* name) -> int
  {
    for (int b = 0; b < B; ++b)
      if (!strcmp(UD::Bones[b].Name, name))
        return b;
    return -1;
  };
  int spine2 = boneOf("Spine2");
  // the bones below each bone (its subtree), for the hands' and feet's children after the IK
  auto isBelow = [&](int b, int top) -> bool
  {
    for (int p = parent[b]; p >= 0; p = parent[p])
      if (p == top)
        return true;
    return false;
  };

  // Pass 1, every frame: the muscles' pose on the reference (each bone's turn from its T-pose and place, the Hips at
  // its T-pose place, unturned), the body's frame (half way between the Hips' and the upper chest's turns, Unity's
  // "average of the lower and upper body orientation") and mass centre, and the clip's body and goals (game axes)
  std::vector<Q> poseD((size_t)frames * B);
  std::vector<V> poseP((size_t)frames * B);
  std::vector<Q> bodyQ((size_t)frames), rootQ((size_t)frames);
  std::vector<V> com((size_t)frames), rootT((size_t)frames);
  std::vector<V> goalT((size_t)frames * 4);
  std::vector<Q> goalQ((size_t)frames * 4);
  for (int k = 0; k < frames; ++k)
  {
    double t = (std::min)(stop, start + k / rate);
    double v[UD::MuscleCount];
    for (int m = 0; m < UD::MuscleCount; ++m)
      v[m] = muscleCurve[m] >= 0 ? Evaluate(curves[(size_t)muscleCurve[m]], t) : 0.0;
    Q* D = &poseD[(size_t)k * B];
    V* P = &poseP[(size_t)k * B];
    for (int b = 0; b < B; ++b)
    {
      int p = parent[b];
      if (p < 0)
      {
        D[b] = { 0.0, 0.0, 0.0, 1.0 };
        P[b] = refT[b];
        continue;
      }
      // the bone's turn: its zero pose, then its swing x twist (Unity's ZYRoll), each the sum of its muscles' half
      // angle tangents along their axes
      V tw = { 0.0, 0.0, 0.0 }, sw = { 0.0, 0.0, 0.0 };
      for (int m = 0; m < UD::MuscleCount; ++m)
      {
        if (UD::Muscles[m].Bone != b)
          continue;
        double x = v[m];
        double deg = x > 0.0 ? x * UD::Muscles[m].Max : -x * UD::Muscles[m].Min;
        double h = tan(deg * (3.14159265358979323846 / 180.0) * 0.5);
        V ax = ToV(UD::Muscles[m].Axis);
        if (UD::Muscles[m].Twist)
          tw = Add(tw, Scale(ax, h));
        else
          sw = Add(sw, Scale(ax, h));
      }
      Q qt = Norm({ tw.x, tw.y, tw.z, 1.0 }), qs = Norm({ sw.x, sw.y, sw.z, 1.0 });
      Q local = Mul(zero[b], Mul(qs, qt));
      D[b] = Mul(D[p], local);
      P[b] = Add(P[p], Rot(D[p], Sub(refT[b], refT[p])));
    }
    Q a = D[0], c = D[spine2];
    if (a.x * c.x + a.y * c.y + a.z * c.z + a.w * c.w < 0.0)
      c = { -c.x, -c.y, -c.z, -c.w };
    bodyQ[(size_t)k] = Norm({ a.x + c.x, a.y + c.y, a.z + c.z, a.w + c.w });
    V m = { 0.0, 0.0, 0.0 };
    for (int b = 0; b < B; ++b)
      m = Add(m, Scale(P[b], UD::Bones[b].Mass / massSum));
    com[(size_t)k] = m;
    auto cv = [&](int curve) { return Evaluate(curves[(size_t)curve], t); };
    rootT[(size_t)k] = UnityV(cv(rootCurve[0]), cv(rootCurve[1]), cv(rootCurve[2]));
    rootQ[(size_t)k] = UnityQ(cv(rootCurve[3]), cv(rootCurve[4]), cv(rootCurve[5]), cv(rootCurve[6]));
    for (int g = 0; g < 4; ++g)
      if (goalOk[g])
      {
        goalT[(size_t)k * 4 + g] = UnityV(cv(goalCurve[g][0]), cv(goalCurve[g][1]), cv(goalCurve[g][2]));
        goalQ[(size_t)k * 4 + g] = UnityQ(cv(goalCurve[g][3]), cv(goalCurve[g][4]), cv(goalCurve[g][5]),
          cv(goalCurve[g][6]));
      }
  }

  // The clip's own scale (the reference's metres per body unit: the clip is in units of its character's size): the
  // muscles' hands and feet against the goals, over every frame (least squares); no goals: the fit's
  int goalBone[4] = { boneOf("LeftHand"), boneOf("RightHand"), boneOf("LeftFoot"), boneOf("RightFoot") };
  V shift = ToV(UD::FitShift);
  double scale = UD::FitScale;
  {
    double num = 0.0, den = 0.0;
    for (int k = 0; k < frames; ++k)
    {
      Q inv = Conj(bodyQ[(size_t)k]);
      for (int g = 0; g < 4; ++g)
        if (goalOk[g])
        {
          V pm = Rot(inv, Sub(poseP[(size_t)k * B + goalBone[g]], com[(size_t)k]));
          V gs = Add(goalT[(size_t)k * 4 + g], shift);
          num += Dot(pm, gs);
          den += Dot(gs, gs);
        }
    }
    if (den > 1e-9)
    {
      double s = num / den;
      if (s > UD::FitScale * 0.5 && s < UD::FitScale * 2.0)
        scale = s;
      else
        warnings += "its goals don't match its muscles' size (the reference's used); ";
    }
  }

  // Pass 2, every frame: the pose put in the world by the clip's body; the body moved the least that lets each leg
  // reach its foot's goal; each arm and leg bent to its goal (two-bone IK: the knee and elbow bend about their hinge as
  // the muscles turn it), the hand or foot turned as its goal; their children after them
  const int L1Of[4][3] = { { boneOf("LeftArm"), boneOf("LeftForeArm"), boneOf("LeftHand") },
    { boneOf("RightArm"), boneOf("RightForeArm"), boneOf("RightHand") },
    { boneOf("LeftUpLeg"), boneOf("LeftLeg"), boneOf("LeftFoot") },
    { boneOf("RightUpLeg"), boneOf("RightLeg"), boneOf("RightFoot") } };
  // each chain's middle joint's bend axis (the stretch muscle's axis through its zero pose; a positive turn bends)
  V hingeLocal[4];
  for (int g = 0; g < 4; ++g)
  {
    char const* stretch = g == 0 ? "Left Forearm Stretch" : g == 1 ? "Right Forearm Stretch" : g == 2 ?
      "Left Lower Leg Stretch" : "Right Lower Leg Stretch";
    V axis = { 0.0, 0.0, 0.0 };
    for (int m = 0; m < UD::MuscleCount; ++m)
      if (!strcmp(UD::Muscles[m].Name, stretch))
        axis = ToV(UD::Muscles[m].Axis);
    hingeLocal[g] = Scale(Rot(zero[L1Of[g][1]], axis), -1.0);
  }
  Q goalTurn[4];
  for (int g = 0; g < 4; ++g)
    goalTurn[g] = ToQ(UD::GoalTurn[g]);
  double reachOf[4][2];
  for (int g = 0; g < 4; ++g)
  {
    reachOf[g][0] = Len(Sub(refT[L1Of[g][1]], refT[L1Of[g][0]]));
    reachOf[g][1] = Len(Sub(refT[L1Of[g][2]], refT[L1Of[g][1]]));
  }
  std::vector<Q> worldQ((size_t)frames * B);
  std::vector<V> worldP((size_t)frames * B);
  int shortFrames[4] = { 0, 0, 0, 0 };
  double offSum[2] = { 0.0, 0.0 };
  int offCount[2] = { 0, 0 };
  double shortMost[4] = { 0.0, 0.0, 0.0, 0.0 };
  double movedMost = 0.0;
  for (int k = 0; k < frames; ++k)
  {
    Q const* D = &poseD[(size_t)k * B];
    V const* P = &poseP[(size_t)k * B];
    Q* W = &worldQ[(size_t)k * B];
    V* X = &worldP[(size_t)k * B];
    Q rq = rootQ[(size_t)k];
    V rt = rootT[(size_t)k];
    Q A = Mul(rq, Conj(bodyQ[(size_t)k]));
    V base = Sub(Scale(rt, scale), Scale(Rot(rq, shift), scale));
    for (int b = 0; b < B; ++b)
    {
      W[b] = Mul(A, D[b]);
      X[b] = Add(base, Rot(A, Sub(P[b], com[(size_t)k])));
    }
    auto goalPlace = [&](int g) { return Add(Scale(rt, scale), Scale(Rot(rq, goalT[(size_t)k * 4 + g]), scale)); };
    for (int g = 0; g < 4; ++g)
      if (goalOk[g])
      {
        V e = Sub(X[L1Of[g][2]], goalPlace(g));
        offSum[g < 2 ? 0 : 1] += Dot(e, e);
        ++offCount[g < 2 ? 0 : 1];
      }
    // the body moved toward feet out of reach (a few projections onto each leg's reach)
    V delta = { 0.0, 0.0, 0.0 };
    for (int it = 0; it < 6; ++it)
      for (int g = 2; g < 4; ++g)
      {
        if (!goalOk[g])
          continue;
        double R = (reachOf[g][0] + reachOf[g][1]) * 0.995;
        V d = Sub(goalPlace(g), Add(X[L1Of[g][0]], delta));
        double dist = Len(d);
        if (dist > R)
          delta = Add(delta, Scale(d, (dist - R) / dist));
      }
    movedMost = (std::max)(movedMost, Len(delta));
    for (int b = 0; b < B; ++b)
      X[b] = Add(X[b], delta);
    for (int g = 0; g < 4; ++g)
    {
      if (!goalOk[g])
        continue;
      int root = L1Of[g][0], mid = L1Of[g][1], end = L1Of[g][2];
      double L1 = reachOf[g][0], L2 = reachOf[g][1];
      V a = X[root], b = X[mid];
      V t = goalPlace(g);
      V d = Sub(t, a);
      double dist = Len(d);
      V dh = Scale(d, 1.0 / (std::max)(dist, 1e-9));
      double lo = fabs(L1 - L2) * 1.001 + 1e-4, hi = (L1 + L2) * 0.9995;
      double dc = (std::min)((std::max)(dist, lo), hi);
      if (dist - hi > 1e-4)
      {
        ++shortFrames[g];
        shortMost[g] = (std::max)(shortMost[g], dist - hi);
      }
      V h = Rot(W[root], hingeLocal[g]);
      V mhRaw = Perp(Cross(dh, h), dh);
      double wgt = (std::min)((std::max)((Len(mhRaw) - 0.15) / 0.2, 0.0), 1.0);
      V m = Unit(Add(Add(Scale(Unit(mhRaw), wgt), Scale(Unit(Perp(Sub(b, a), dh)), 1.0 - wgt)), Scale(mhRaw, 1e-9)));
      double x = (L1 * L1 - L2 * L2 + dc * dc) / (2.0 * dc);
      double y = sqrt((std::max)(0.0, L1 * L1 - x * x));
      V midNew = Add(Add(a, Scale(dh, x)), Scale(m, y));
      V endNew = Add(a, Scale(dh, dc));
      V u1 = Unit(Sub(midNew, a));
      Q W1 = Mul(Between(Unit(Sub(b, a)), u1), W[root]);
      V hn = Unit(Perp(Rot(W1, hingeLocal[g]), u1));
      V ht = Unit(Perp(Cross(m, dh), u1));
      double ang = atan2(Dot(Cross(hn, ht), u1), Dot(hn, ht));
      W1 = Mul(AxisAngle(u1, ang), W1);
      Q localMid = Mul(Conj(D[root]), D[mid]);
      Q W2 = Mul(W1, localMid);
      V u2 = Unit(Rot(W2, Sub(refT[end], refT[mid])));
      W2 = Mul(Between(u2, Unit(Sub(endNew, midNew))), W2);
      W[root] = W1;
      W[mid] = W2;
      W[end] = Mul(Mul(rq, goalQ[(size_t)k * 4 + g]), Conj(goalTurn[g]));
      X[mid] = midNew;
      X[end] = endNew;
      for (int c = end + 1; c < B; ++c)
        if (isBelow(c, end))
        {
          int p = parent[c];
          W[c] = Mul(W[p], Mul(Conj(D[p]), D[c]));
          X[c] = Add(X[p], Rot(W[p], Sub(refT[c], refT[p])));
        }
    }
  }

  // The floor: the feet's goals where they stand still at their lowest (within 1.5 cm of the foot's own 5th
  // percentile, or of the other foot's when its own is 5 cm or more higher: a foot held up all along doesn't count;
  // moving under 25 cm/s) are put at the reference's ankle height. 5 such foot frames are needed (a clip of fewer
  // frames: as many as it has).
  double lift = 0.0;
  int still = 0;
  {
    std::vector<V> y[2];
    std::vector<double> ys[2];
    double low = 1e30, p5[2] = { 1e30, 1e30 };
    for (int g = 2; g < 4; ++g)
    {
      if (!goalOk[g])
        continue;
      y[g - 2].resize((size_t)frames);
      ys[g - 2].resize((size_t)frames);
      for (int k = 0; k < frames; ++k)
      {
        y[g - 2][(size_t)k] = Scale(Add(rootT[(size_t)k], Rot(rootQ[(size_t)k], goalT[(size_t)k * 4 + g])), scale);
        ys[g - 2][(size_t)k] = y[g - 2][(size_t)k].y;
      }
      p5[g - 2] = Percentile(ys[g - 2], 5.0);
      low = (std::min)(low, p5[g - 2]);
    }
    std::vector<double> heights;
    for (int f = 0; f < 2; ++f)
      for (int k = 0; k < (int)ys[f].size(); ++k)
      {
        double base = p5[f] < low + 0.05 ? p5[f] : low;
        double speed = 0.0;
        if (frames > 1)
        {
          int k0 = k > 0 ? k - 1 : 0, k1 = k > 0 ? k : 1;
          V dv = Sub(y[f][(size_t)k1], y[f][(size_t)k0]);
          speed = sqrt(dv.x * dv.x + dv.z * dv.z) * rate;
        }
        if (ys[f][(size_t)k] < base + 0.015 && speed < 0.25)
          heights.push_back(ys[f][(size_t)k]);
      }
    still = (int)heights.size();
    if (still > 0 && still >= (std::min)(5, frames))
    {
      std::sort(heights.begin(), heights.end());
      size_t n = heights.size();
      double median = n % 2 ? heights[n / 2] : 0.5 * (heights[n / 2 - 1] + heights[n / 2]);
      lift = UD::FootAnkle - median;
    }
    else if (goalOk[2] || goalOk[3])
      warnings += "its feet never stand still: its height is the file's; ";
    else
      warnings += "it has no foot goals: its height is the file's (its feet may stand a few cm below the floor); ";
  }
  for (V& p : worldP)
    p.y += lift;

  auto clip = std::make_shared<Clip>();
  clip->File = Utf8(fileName);
  clip->FromUnity = true;
  clip->Take = clipValue.Field("m_Name") ? clipValue.Field("m_Name")->Str : std::string();
  clip->Fps = rate;
  clip->Frames = frames;
  for (int b = 0; b < B; ++b)
  {
    clip->Bones.push_back(UD::Bones[b].Name);
    char const* u = UnityBoneName(UD::Bones[b].Name);
    std::string name = u ? u : "";
    if (!u)
    {
      // fingers: Unity's LeftThumbProximal / Intermediate / Distal, the 4th the tip
      std::string n = UD::Bones[b].Name;
      std::string side = n.compare(0, 4, "Left") == 0 ? "Left" : "Right";
      std::string rest = n.substr(side.size() + 4);   // after "Hand"
      std::string finger = rest.substr(0, rest.size() - 1);
      char k = rest.back();
      if (finger == "Pinky")
        finger = "Little";
      name = k == '1' ? side + finger + "Proximal" : k == '2' ? side + finger + "Intermediate" : k == '3' ? side + finger +
        "Distal" : "(the " + side + " " + finger + "'s tip)";
    }
    clip->SourceNames.push_back(name);
  }
  clip->BonesFrom = "Unity's human bones";
  clip->Basis[0][0] = clip->Basis[1][1] = clip->Basis[2][2] = 1.0;
  clip->Unit = 1.0;
  clip->Hips = 0;
  clip->RestQ.assign((size_t)B * 4, 0.f);
  clip->RestP.resize((size_t)B * 3);
  for (int b = 0; b < B; ++b)
  {
    clip->RestQ[(size_t)b * 4 + 3] = 1.f;
    clip->RestP[(size_t)b * 3 + 0] = (float)refT[b].x;
    clip->RestP[(size_t)b * 3 + 1] = (float)refT[b].y;
    clip->RestP[(size_t)b * 3 + 2] = (float)refT[b].z;
  }
  for (int k = 0; k < 3; ++k)
    clip->HipsRest[k] = clip->RestP[(size_t)k];
  clip->Q.resize((size_t)frames * B * 4);
  clip->HipsP.resize((size_t)frames * 3);
  // the elbows' and knees' hinges as the FBX clips measure them (the frames bent 20 deg or more; rest = the world)
  V hingeSum[4] = {}, hingeLowerSum[4] = {};
  int hingeFrames[4] = { 0, 0, 0, 0 };
  for (int k = 0; k < frames; ++k)
  {
    Q const* W = &worldQ[(size_t)k * B];
    V const* X = &worldP[(size_t)k * B];
    for (int b = 0; b < B; ++b)
    {
      float* out = &clip->Q[((size_t)k * B + b) * 4];
      out[0] = (float)W[b].x; out[1] = (float)W[b].y; out[2] = (float)W[b].z; out[3] = (float)W[b].w;
    }
    clip->HipsP[(size_t)k * 3 + 0] = (float)X[0].x;
    clip->HipsP[(size_t)k * 3 + 1] = (float)X[0].y;
    clip->HipsP[(size_t)k * 3 + 2] = (float)X[0].z;
    for (int g = 0; g < 4; ++g)
    {
      int a = L1Of[g][0], b = L1Of[g][1], c = L1Of[g][2];
      V upper = Sub(X[b], X[a]), lower = Sub(X[c], X[b]);
      double lu = Len(upper), ll = Len(lower);
      if (!(lu > 1e-6 && ll > 1e-6) || Dot(upper, lower) / (lu * ll) > 0.9396926)   // cos 20 deg
        continue;
      V hinge = Cross(upper, lower);
      double lh = Len(hinge);
      if (!(lh > 1e-9))
        continue;
      hingeSum[g] = Add(hingeSum[g], Rot(Conj(W[a]), Scale(hinge, 1.0 / lh)));
      hingeLowerSum[g] = Add(hingeLowerSum[g], Rot(Conj(W[b]), Scale(hinge, 1.0 / lh)));
      ++hingeFrames[g];
    }
  }
  auto put3 = [](float* out, V v) { out[0] = (float)v.x; out[1] = (float)v.y; out[2] = (float)v.z; };
  for (int side = 0; side < 2; ++side)
  {
    int arm = side, leg = 2 + side;
    if (hingeFrames[arm] > 0 && Len(hingeSum[arm]) > 1e-6)
    {
      put3(clip->ElbowHinge[side], Unit(hingeSum[arm]));
      put3(clip->ElbowHingeLower[side], Len(hingeLowerSum[arm]) > 1e-6 ? Unit(hingeLowerSum[arm]) : Unit(hingeSum[arm]));
      clip->ElbowHingeOk[side] = true;
    }
    if (hingeFrames[leg] > 0 && Len(hingeSum[leg]) > 1e-6)
    {
      put3(clip->KneeHinge[side], Unit(hingeSum[leg]));
      put3(clip->KneeHingeLower[side], Len(hingeLowerSum[leg]) > 1e-6 ? Unit(hingeLowerSum[leg]) : Unit(hingeSum[leg]));
      clip->KneeHingeOk[side] = true;
    }
  }
  clip->RestFrom = "a reference T-pose (Mixamo's; a Unity humanoid clip has no rig)";
  for (float v : clip->Q)
    if (!std::isfinite(v))
    {
      error = "gives turns that aren't numbers (a broken curve)";
      return nullptr;
    }
  for (float v : clip->HipsP)
    if (!std::isfinite(v) || fabs(v) > 1e5f)
    {
      error = "gives places that aren't numbers or are over 100 km away (a broken curve)";
      return nullptr;
    }

  // What may be off
  if (badKeys)
  {
    char text[120];
    _snprintf_s(text, sizeof(text), _TRUNCATE, "%d broken keys (not numbers, over a million, or out of time order) left "
      "out; ", badKeys);
    warnings += text;
  }
  if (!brokenAttrs.empty())
  {
    std::string names;
    for (size_t k = 0; k < brokenAttrs.size() && k < 3; ++k)
      names += (k ? ", " : "") + brokenAttrs[k];
    char text[300];
    _snprintf_s(text, sizeof(text), _TRUNCATE, "%d curves had no usable key and were left out (%s%s); ",
      (int)brokenAttrs.size(), names.c_str(), brokenAttrs.size() > 3 ? "..." : "");
    warnings += text;
  }
  if (weightedKeys)
  {
    char text[160];
    _snprintf_s(text, sizeof(text), _TRUNCATE, "%d keys have weighted tangents (Unity 2018+), played unweighted here: "
      "their curves' shape may differ a little; ", weightedKeys);
    warnings += text;
  }
  if (mirror)
    warnings += "it's set to play mirrored in Unity: it plays as recorded here; ";
  if (muscles < UD::MuscleCount)
  {
    char text[120];
    _snprintf_s(text, sizeof(text), _TRUNCATE, "%d of the %d muscle curves are missing (those bones keep their zero "
      "pose); ", UD::MuscleCount - muscles, UD::MuscleCount);
    warnings += text;
  }
  int goals = 0;
  for (int g = 0; g < 4; ++g)
    goals += goalOk[g] ? 1 : 0;
  if (goals < 4)
    warnings += "it lacks some hand or foot goals (those limbs take the muscles' approximate turns alone); ";
  for (int g = 0; g < 4; ++g)
    if (shortFrames[g] > 0 && shortMost[g] > 0.02)
    {
      char text[160];
      _snprintf_s(text, sizeof(text), _TRUNCATE, "the %s can't reach its goal in %d frames (up to %.0f cm short); ",
        g == 0 ? "left hand" : g == 1 ? "right hand" : g == 2 ? "left foot" : "right foot", shortFrames[g],
        shortMost[g] * 100.0);
      warnings += text;
    }

  std::string offText;
  if (goals == 0)
    offText = "it has no hand or foot goals: the model's pose alone";
  else
  {
    char part[64];
    offText = "the model put its ";
    if (offCount[0])
    {
      _snprintf_s(part, sizeof(part), _TRUNCATE, "hands %.0f cm", sqrt(offSum[0] / offCount[0]) * 100.0);
      offText += part;
    }
    if (offCount[1])
    {
      _snprintf_s(part, sizeof(part), _TRUNCATE, "%sfeet %.0f cm", offCount[0] ? " and " : "",
        sqrt(offSum[1] / offCount[1]) * 100.0);
      offText += part;
    }
    bool arms = goalOk[0] && goalOk[1], legs = goalOk[2] && goalOk[3];
    offText += arms && legs ? " off their goals (rms), then the arms and legs were bent to them" :
      " off their goals (rms), then the arms and legs that have goals were bent to them";
  }
  char report[1000];
  _snprintf_s(report, sizeof(report), _TRUNCATE, "\"%s\" (%s): %d frames at %.0f fps (%.2f s)%s; a Unity humanoid clip "
    "(%d of the %d body muscles, the eyes' and jaw's not played; %d of 4 hand/foot goals; Unity %s, format %u): its "
    "muscles turned into bones by an approximate model of Unity's on a reference body; %s; its body units %.3f m, its "
    "floor lifted %.1f cm (%d still foot frames), its body moved up to %.0f cm for the feet; %d curves on other objects "
    "(props, sounds...) not played", clip->Take.c_str(), clip->File.c_str(), frames, rate, clip->Length(),
    loopTime ? ", looping in Unity" : "", muscles, UD::MuscleCount, goals, unityVersion.c_str(), version, offText.c_str(), scale,
    lift * 100.0, still, movedMost * 100.0, otherPaths);
  clip->Report = report;
  if (!warnings.empty())
    clip->Warning = warnings.substr(0, warnings.size() - 2);
  return clip;
}
