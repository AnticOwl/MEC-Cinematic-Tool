#include "AttachTargets.h"
#include <Windows.h>
#include <cmath>
#include <cstring>

using namespace DirectX;

namespace
{
  // Every entity is linked into an instance list of its class (ClassInfo +
  // 0x60 / 0x70 / 0x80 / 0x90, the first link). The link is at entity + 0x40:
  // the next link, then the address of whatever points to this one (the head
  // or the link before).
  const int EntityLinkOffset         = 0x40;
  const int EntityComponentsOffset   = 0x38; // GameComponentEntity -> components, then their transforms
  const int ClassSuperOffset         = 0x18; // ClassInfo -> super class
  const int ClassFirstDerivedOffset  = 0x30; // ClassInfo -> first subclass
  const int ClassNextSiblingOffset   = 0x38; // ClassInfo -> next subclass of the same super class
  const int ClassInstanceListsOffset = 0x60; // ClassInfo -> 4 instance lists, 0x10 bytes apart
  const __int64 ClientPhysicsEntityRva = 0x27EE290; // its ClassInfo on PE timestamp 0x5762E3B7
  const int MaxInstancesPerList = 20000;

  // A swarm's members are records in its CrowdBoidsComponent's list (begin
  // and end at +0x70 / +0x78), 0x20 bytes each: two positions (float4) a
  // frame apart.
  const int BoidsAgentsBeginOffset = 0x70;
  const int BoidsAgentsEndOffset   = 0x78;
  const int BoidsAgentSize         = 0x20;
  const int MaxAgentsPerSwarm      = 8192;
  const int MaxComponents          = 32;

  const float PickAngle    = 0.21f;  // about 12 degrees from the middle of the view
  const float PickRadius   = 1.f;    // or this close to the line of sight, for objects near the camera
  const float PickDistance = 1000.f;

  // GameComponentEntity's transform function (vtable + 0xC0, the one Faith's
  // soldier has too): the matrix after the component array at entity + 0x38.
  // Entities whose vtable has this function are read the same way directly.
  const BYTE ComponentTransformCode[] = {
    0x4C, 0x8B, 0x49, 0x38,             // mov r9, [rcx+38h]
    0x41, 0x0F, 0xB6, 0x49, 0x0A,       // movzx ecx, byte [r9+0Ah]
    0x41, 0x0F, 0xB6, 0x41, 0x09,       // movzx eax, byte [r9+9]
    0x4C, 0x8D, 0x04, 0x48,             // lea r8, [rax+rcx*2]
    0x49, 0xC1, 0xE0, 0x05,             // shl r8, 5
    0x43, 0x0F, 0x28, 0x44, 0x08, 0x10  // movaps xmm0, [r8+r9+10h]
  };

  struct EntityClass
  {
    __int64 ClassInfo;
    char Name[64];
    const char* Kind;
    float AimHeight;  // characters' origin is at their feet
    bool Static;      // static models: only picked when they move
    bool Swarm;       // SwarmEntity: its members are the targets
  };
  EntityClass g_Classes[attach::MaxClasses];
  int g_ClassCount = 0;
  __int64 g_BoidsClassInfo = 0; // CrowdBoidsComponent, found in the first swarm

  // The class an object's getType() returns (vtable slot 0 is "lea rax,
  // [ClassInfo]; ret"), read from its code instead of calling it. 0 if slot 0
  // isn't that. Inside a __try, like everything that reads game objects.
  __int64 ClassInfoOfVTable(__int64 vtable)
  {
    const BYTE* code = *(const BYTE**)vtable;
    if (code[0] != 0x48 || code[1] != 0x8D || code[2] != 0x05 || code[7] != 0xC3)
      return 0;
    return (__int64)(code + 7 + *(const int*)(code + 3));
  }

  // ClassInfo -> its type data, whose first field is the name
  bool ReadClassName(__int64 classInfo, char* buffer, size_t size)
  {
    buffer[0] = 0;
    __try
    {
      const char* name = *(const char**)*(__int64*)classInfo;
      size_t i = 0;
      for (; i + 1 < size && name[i]; ++i)
      {
        char c = name[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == ':'))
          return false;
        buffer[i] = c;
      }
      buffer[i] = 0;
      return i > 0;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      buffer[0] = 0;
      return false;
    }
  }

  bool ClassNameIs(__int64 classInfo, const char* name)
  {
    char buffer[64];
    return ReadClassName(classInfo, buffer, sizeof(buffer)) && strcmp(buffer, name) == 0;
  }

  __int64 ReadQword(__int64 address)
  {
    __try { return *(__int64*)address; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
  }

  // ClientPhysicsEntity's ClassInfo: up from Faith's own class, or where it
  // is on the build the tools were made for
  __int64 FindPhysicsEntityClass(__int64 pSoldier, bool& fromFaith)
  {
    fromFaith = false;
    __try
    {
      __int64 classInfo = pSoldier ? ClassInfoOfVTable(*(__int64*)pSoldier) : 0;
      for (int i = 0; classInfo && i < 12; ++i)
      {
        if (ClassNameIs(classInfo, "ClientPhysicsEntity"))
        {
          fromFaith = true;
          return classInfo;
        }
        classInfo = *(__int64*)(classInfo + ClassSuperOffset);
      }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }

    __int64 classInfo = (__int64)GetModuleHandleA(nullptr) + ClientPhysicsEntityRva;
    return ClassNameIs(classInfo, "ClientPhysicsEntity") ? classInfo : 0;
  }

  // Adds a class and its subclasses. Terrain, water, held weapons, bullets,
  // static model groups, the map screen and the menu's character previews are
  // left out.
  void AddEntityClasses(__int64 classInfo, const char* kind, float aimHeight, bool isStatic, int depth)
  {
    static const char* const skipped[] = { "ClientStaticModelGroupEntity", "ClientTerrainEntity",
      "ClientWaterEntity", "AppearanceEntity", "ClientBulletEntity", "PamClientCharacterWeaponEntity",
      "PamClientUIMapEntity" };

    for (int guard = 0; classInfo && guard < 256 && g_ClassCount < attach::MaxClasses; ++guard)
    {
      // A class whose name can't be read is left out, with its subclasses,
      // but its siblings still count
      EntityClass& entry = g_Classes[g_ClassCount];
      bool skip = !ReadClassName(classInfo, entry.Name, sizeof(entry.Name));
      for (const char* name : skipped)
        skip |= strcmp(entry.Name, name) == 0;

      if (!skip)
      {
        const char* childKind = kind;
        float childAim = aimHeight;
        bool childStatic = isStatic;
        if (strcmp(entry.Name, "ClientCharacterEntity") == 0) { childKind = "character"; childAim = 1.f; }
        else if (strcmp(entry.Name, "ClientVehicleEntity") == 0) childKind = "vehicle";
        else if (strcmp(entry.Name, "PamClientDroneActorEntity") == 0) childKind = "drone";
        else if (strcmp(entry.Name, "ClientDynamicModelEntity") == 0) childKind = "moving model";
        else if (strcmp(entry.Name, "ClientBangerEntity") == 0) childKind = "physics prop";
        else if (strcmp(entry.Name, "ClientProjectileEntity") == 0) childKind = "projectile";
        else if (strcmp(entry.Name, "ClientStaticModelEntity") == 0) { childKind = "model"; childStatic = true; }

        entry.ClassInfo = classInfo;
        entry.Kind = childKind;
        entry.AimHeight = childAim;
        entry.Static = childStatic;
        entry.Swarm = false;
        ++g_ClassCount;

        __int64 firstDerived = ReadQword(classInfo + ClassFirstDerivedOffset);
        if (firstDerived && depth < 8)
          AddEntityClasses(firstDerived, childKind, childAim, childStatic, depth + 1);
      }

      classInfo = ReadQword(classInfo + ClassNextSiblingOffset);
    }
  }

  // SwarmEntity is a sibling of ClientPhysicsEntity (both under ClientGameComponentEntity)
  void AddSwarmClass(__int64 physicsEntity)
  {
    __int64 gameComponentEntity = ReadQword(physicsEntity + ClassSuperOffset);
    __int64 classInfo = gameComponentEntity ? ReadQword(gameComponentEntity + ClassFirstDerivedOffset) : 0;
    for (int guard = 0; classInfo && guard < 64 && g_ClassCount < attach::MaxClasses; ++guard)
    {
      if (ClassNameIs(classInfo, "SwarmEntity"))
      {
        EntityClass& entry = g_Classes[g_ClassCount++];
        strcpy_s(entry.Name, "SwarmEntity");
        entry.ClassInfo = classInfo;
        entry.Kind = "traffic or crowd";
        entry.AimHeight = 0.5f;
        entry.Static = false;
        entry.Swarm = true;
        return;
      }
      classInfo = ReadQword(classInfo + ClassNextSiblingOffset);
    }
  }

  bool IsFiniteVector(const float* values, int count)
  {
    for (int i = 0; i < count; ++i)
      if (!(fabsf(values[i]) < 1e6f))
        return false;
    return true;
  }

  bool ReadComponentTransform(__int64 pEntity, XMFLOAT4X4& transform)
  {
    __int64 pComponents = *(__int64*)(pEntity + EntityComponentsOffset);
    if (!pComponents)
      return false;
    __int64 index = *(BYTE*)(pComponents + 9) + 2 * *(BYTE*)(pComponents + 0xA);
    memcpy(&transform, (void*)(pComponents + index * 0x20 + 0x10), sizeof(transform));
    return IsFiniteVector(&transform._11, 16); // not NaN or somewhere no level is
  }

  // Still in its class's instance list: whatever points to its link still does
  bool IsEntityLinked(__int64 pEntity)
  {
    __int64 link = pEntity + EntityLinkOffset;
    __int64 next = *(__int64*)link;
    __int64 previous = *(__int64*)(link + 8);
    return previous && *(__int64*)previous == link && (!next || *(__int64*)(next + 8) == link);
  }

  // A swarm's CrowdBoidsComponent, from the swarm's components (at +0x10 of
  // each 0x20-byte entry of entity + 0x38; the count is byte +9)
  __int64 FindBoidsComponent(__int64 pSwarm)
  {
    __int64 pComponents = *(__int64*)(pSwarm + EntityComponentsOffset);
    if (!pComponents)
      return 0;
    int count = *(BYTE*)(pComponents + 9);
    for (int i = 0; i < count && i < MaxComponents; ++i)
    {
      __int64 pComponent = *(__int64*)(pComponents + 0x10 + i * 0x20 + 0x10);
      if (!pComponent)
        continue;
      __int64 classInfo = ClassInfoOfVTable(*(__int64*)pComponent);
      if (!classInfo)
        continue;
      if (g_BoidsClassInfo ? classInfo == g_BoidsClassInfo : ClassNameIs(classInfo, "CrowdBoidsComponent"))
      {
        g_BoidsClassInfo = classInfo;
        return pComponent;
      }
    }
    return 0;
  }

  // Keeps the best candidates, lowest score first. A character is two
  // entities at the same spot: only the first one counts.
  void Insert(attach::Target* list, int& count, attach::Target const& candidate)
  {
    for (int i = 0; i < count; ++i)
      if (candidate.Agent < 0 && list[i].Agent < 0 && list[i].Class == candidate.Class &&
        fabsf(list[i].Position.x - candidate.Position.x) + fabsf(list[i].Position.y - candidate.Position.y) +
        fabsf(list[i].Position.z - candidate.Position.z) < 0.02f)
        return;

    int at = count;
    while (at > 0 && candidate.Score < list[at - 1].Score)
      --at;
    if (at >= attach::MaxCandidates)
      return;
    int last = count < attach::MaxCandidates ? count : attach::MaxCandidates - 1;
    for (int i = last; i > at; --i)
      list[i] = list[i - 1];
    list[at] = candidate;
    if (count < attach::MaxCandidates)
      ++count;
  }

  // How well a point is in the middle of the view. False if it isn't in it at all.
  bool ScorePoint(attach::View const& view, float x, float y, float z, attach::Target& candidate)
  {
    float v[3] = { x - view.Camera.x, y - view.Camera.y, z - view.Camera.z };
    float distance = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    float along = v[0] * view.Look.x + v[1] * view.Look.y + v[2] * view.Look.z;
    if (along < 0.3f || distance > PickDistance)
      return false;
    float across = sqrtf(fmaxf(0.f, distance * distance - along * along));
    float angle = atan2f(across, along);
    if (angle > PickAngle && across > PickRadius)
      return false;

    // Closest to the middle, and nearer ones first when that's about the same
    candidate.Distance = distance;
    candidate.Angle = angle;
    candidate.Score = angle + distance * 0.00005f;
    return true;
  }

  // Where what's found goes: the best in the middle of the view (Scan's
  // lists), or everything within a radius (Out)
  struct Collector
  {
    attach::View const* View;
    attach::Scan* Scan;
    float Radius;           // > 0: everything this close goes into Out
    attach::Target* Out;
    int OutMax;
    int OutCount;
  };

  void Offer(Collector& collector, attach::Target& candidate, float aimHeight, bool isStatic)
  {
    attach::View const& view = *collector.View;
    float x = candidate.Position.x, y = candidate.Position.y + aimHeight, z = candidate.Position.z;
    if (collector.Radius > 0)
    {
      float v[3] = { x - view.Camera.x, y - view.Camera.y, z - view.Camera.z };
      float distance = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
      if (distance > collector.Radius)
        return;
      if (collector.OutCount >= collector.OutMax)
      {
        ++collector.Scan->Dropped;
        return;
      }
      float along = v[0] * view.Look.x + v[1] * view.Look.y + v[2] * view.Look.z;
      candidate.Distance = distance;
      candidate.Angle = distance > 0.01f ? acosf(fmaxf(-1.f, fminf(1.f, along / distance))) : 0.f;
      candidate.Score = distance;
      collector.Out[collector.OutCount++] = candidate;
    }
    else if (ScorePoint(view, x, y, z, candidate))
    {
      attach::Scan& scan = *collector.Scan;
      if (isStatic)
        Insert(scan.Static, scan.StaticCount, candidate);
      else
        Insert(scan.Best, scan.BestCount, candidate);
    }
  }

  void ConsiderEntity(__int64 pEntity, int classIndex, Collector& collector)
  {
    attach::View const& view = *collector.View;
    attach::Scan& scan = *collector.Scan;
    __try
    {
      EntityClass const& entityClass = g_Classes[classIndex];
      __int64 vtable = *(__int64*)pEntity;
      if (ClassInfoOfVTable(vtable) != entityClass.ClassInfo)
        return;
      if (memcmp(*(void**)(vtable + 0xC0), ComponentTransformCode, sizeof(ComponentTransformCode)) != 0)
      {
        ++scan.NoTransform;
        return;
      }

      XMFLOAT4X4 transform;
      if (!ReadComponentTransform(pEntity, transform))
        return;
      ++scan.Found[classIndex];

      float x = transform._41, y = transform._42, z = transform._43;
      if (pEntity == view.Skip || fabsf(x) + fabsf(y) + fabsf(z) < 0.01f)
        return;
      if (view.HasSkipPosition && fabsf(x - view.SkipPosition.x) + fabsf(y - view.SkipPosition.y) +
        fabsf(z - view.SkipPosition.z) < 0.01f)
        return;

      attach::Target candidate{};
      candidate.Entity = pEntity;
      candidate.VTable = vtable;
      candidate.Agent = -1;
      candidate.Class = classIndex;
      candidate.Position = XMFLOAT3(x, y, z);
      Offer(collector, candidate, entityClass.AimHeight, entityClass.Static);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      ++scan.Errors;
    }
  }

  void ConsiderSwarm(__int64 pSwarm, int classIndex, Collector& collector)
  {
    attach::Scan& scan = *collector.Scan;
    __try
    {
      EntityClass const& swarmClass = g_Classes[classIndex];
      __int64 vtable = *(__int64*)pSwarm;
      if (ClassInfoOfVTable(vtable) != swarmClass.ClassInfo)
        return;
      __int64 pBoids = FindBoidsComponent(pSwarm);
      if (!pBoids)
        return;
      ++scan.Swarms;

      __int64 begin = *(__int64*)(pBoids + BoidsAgentsBeginOffset);
      __int64 end = *(__int64*)(pBoids + BoidsAgentsEndOffset);
      if (!begin || end <= begin)
        return;
      int count = (int)((end - begin) / BoidsAgentSize);
      if (count > MaxAgentsPerSwarm)
        count = MaxAgentsPerSwarm;

      // Traffic or a crowd: how far its moving members got since the swarm's
      // last update (a car at 50 km/h does about half a metre, someone walking
      // a few centimetres)
      float stepSum = 0.f;
      int moving = 0;
      for (int i = 0; i < count && moving < 256; ++i)
      {
        const float* record = (const float*)(begin + (__int64)i * BoidsAgentSize);
        float dx = record[0] - record[4], dy = record[1] - record[5], dz = record[2] - record[6];
        float step = sqrtf(dx * dx + dy * dy + dz * dz);
        if (step > 0.005f && step < 20.f)
        {
          stepSum += step;
          ++moving;
        }
      }
      unsigned char swarmType = moving == 0 ? 0 : (stepSum / moving > 0.12f ? 1 : 2);

      for (int i = 0; i < count; ++i)
      {
        const float* position = (const float*)(begin + (__int64)i * BoidsAgentSize);
        if (!IsFiniteVector(position, 3) || fabsf(position[0]) + fabsf(position[1]) + fabsf(position[2]) < 0.01f)
          continue;
        ++scan.Found[classIndex];

        attach::Target candidate{};
        candidate.Entity = pSwarm;
        candidate.VTable = vtable;
        candidate.Boids = pBoids;
        candidate.BoidsVTable = *(__int64*)pBoids;
        candidate.Agent = i;
        candidate.Class = classIndex;
        candidate.SwarmType = swarmType;
        candidate.Position = XMFLOAT3(position[0], position[1], position[2]);
        Offer(collector, candidate, swarmType == 2 ? 1.f : 0.6f, false);
      }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      ++scan.Errors;
    }
  }

  // Walks one instance list. The game can add and remove entities while it's
  // read, so a bad pointer ends the walk and the count stops runaway lists.
  void ScanInstanceList(__int64 head, int classIndex, Collector& collector)
  {
    __try
    {
      __int64 link = *(__int64*)head;
      for (int n = 0; link && n < MaxInstancesPerList; ++n)
      {
        if (g_Classes[classIndex].Swarm)
          ConsiderSwarm(link - EntityLinkOffset, classIndex, collector);
        else
          ConsiderEntity(link - EntityLinkOffset, classIndex, collector);
        link = *(__int64*)link;
      }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      ++collector.Scan->Errors;
    }
  }

  void ScanAll(Collector& collector)
  {
    for (int i = 0; i < g_ClassCount; ++i)
      for (int list = 0; list < 4; ++list)
        ScanInstanceList(g_Classes[i].ClassInfo + ClassInstanceListsOffset + list * 0x10, i, collector);
  }

  double Seconds()
  {
    static LARGE_INTEGER frequency{};
    if (!frequency.QuadPart)
      QueryPerformanceFrequency(&frequency);
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return (double)now.QuadPart / (double)frequency.QuadPart;
  }

  // A swarm member: the swarm still exists, still has the same boids
  // component, and the record is still in its list
  bool ReadAgent(attach::Target const& target, float position[3], float previous[3])
  {
    __try
    {
      if (*(__int64*)target.Entity != target.VTable || !IsEntityLinked(target.Entity))
        return false;
      if (FindBoidsComponent(target.Entity) != target.Boids || *(__int64*)target.Boids != target.BoidsVTable)
        return false;
      __int64 begin = *(__int64*)(target.Boids + BoidsAgentsBeginOffset);
      __int64 end = *(__int64*)(target.Boids + BoidsAgentsEndOffset);
      if (!begin || target.Agent < 0 || (__int64)(target.Agent + 1) * BoidsAgentSize > end - begin)
        return false;
      const float* record = (const float*)(begin + (__int64)target.Agent * BoidsAgentSize);
      memcpy(position, record, sizeof(float) * 3);
      memcpy(previous, record + 4, sizeof(float) * 3);
      return IsFiniteVector(position, 3) && IsFiniteVector(previous, 3) &&
        fabsf(position[0]) + fabsf(position[1]) + fabsf(position[2]) > 0.01f;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      return false;
    }
  }

  bool ReadEntity(attach::Target const& target, XMFLOAT4X4& transform)
  {
    __try
    {
      if (*(__int64*)target.Entity != target.VTable || !IsEntityLinked(target.Entity))
        return false;
      return ReadComponentTransform(target.Entity, transform);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      return false;
    }
  }
}

namespace attach
{
  bool FindClasses(__int64 pFaithSoldier, bool& fromFaith)
  {
    fromFaith = false;
    if (g_ClassCount > 0)
      return true;

    __int64 physicsEntity = FindPhysicsEntityClass(pFaithSoldier, fromFaith);
    if (!physicsEntity)
      return false;

    // Its subclasses. ClientPhysicsEntity's own lists are left out: they hold
    // unnamed subclasses that share its getType, purpose unknown.
    AddEntityClasses(ReadQword(physicsEntity + ClassFirstDerivedOffset), "object", 0.f, false, 0);
    AddSwarmClass(physicsEntity);
    return g_ClassCount > 0;
  }

  int ClassCount() { return g_ClassCount; }
  const char* ClassName(int index) { return index >= 0 && index < g_ClassCount ? g_Classes[index].Name : "?"; }
  const char* Kind(int index) { return index >= 0 && index < g_ClassCount ? g_Classes[index].Kind : "object"; }
  bool IsStatic(int index) { return index >= 0 && index < g_ClassCount && g_Classes[index].Static; }
  bool IsSwarm(int index) { return index >= 0 && index < g_ClassCount && g_Classes[index].Swarm; }

  const char* KindOf(Target const& target)
  {
    if (target.Agent >= 0)
      return target.SwarmType == 1 ? "traffic" : target.SwarmType == 2 ? "crowd" : "traffic or crowd";
    return Kind(target.Class);
  }

  Group GroupOf(Target const& target)
  {
    if (target.Agent >= 0)
      return target.SwarmType == 2 ? Group::People : Group::Traffic;
    return strcmp(Kind(target.Class), "character") == 0 ? Group::People : Group::Other;
  }

  float AimHeight(Target const& target)
  {
    if (target.Agent >= 0)
      return target.SwarmType == 2 ? 1.f : 0.6f;
    return target.Class >= 0 && target.Class < g_ClassCount ? g_Classes[target.Class].AimHeight : 0.f;
  }

  void ScanView(View const& view, Scan& scan)
  {
    Collector collector = { &view, &scan, 0.f, nullptr, 0, 0 };
    ScanAll(collector);
  }

  int ScanNearby(View const& view, float radius, Target* out, int max, Scan& scan)
  {
    Collector collector = { &view, &scan, radius > 1.f ? radius : 1.f, out, max, 0 };
    ScanAll(collector);
    return collector.OutCount;
  }

  bool ReadTransform(Target& target, XMFLOAT4X4& transform)
  {
    if (target.Agent < 0)
      return ReadEntity(target, transform);

    float position[3], previous[3];
    if (!ReadAgent(target, position, previous))
      return false;

    // The swarm doesn't update every rendered frame. Between its updates the
    // member glides from its previous position to the newest one, over the
    // time the updates usually take, so the camera doesn't move in steps.
    double now = Seconds();
    if (fabsf(position[0] - target.Seen.x) + fabsf(position[1] - target.Seen.y) + fabsf(position[2] - target.Seen.z) > 1e-5f)
    {
      if (target.SeenTime > 0)
      {
        float since = (float)(now - target.SeenTime);
        since = since < 0.005f ? 0.005f : (since > 0.25f ? 0.25f : since);
        target.Interval = target.Interval > 0 ? target.Interval * 0.7f + since * 0.3f : since;
      }
      target.Seen = XMFLOAT3(position[0], position[1], position[2]);
      target.SeenTime = now;
    }
    float t = target.Interval > 0 ? (float)((now - target.SeenTime) / target.Interval) : 1.f;
    t = t < 0.f ? 0.f : (t > 1.f ? 1.f : t);
    XMVECTOR newest = XMVectorSet(position[0], position[1], position[2], 0);
    XMVECTOR before = XMVectorSet(previous[0], previous[1], previous[2], 0);
    XMVECTOR where = XMVectorLerp(before, newest, t);

    // The way it's going, smoothed, and kept while it stands still. Which of
    // the two positions is the newer one doesn't matter for following: the
    // camera only uses how this changes.
    XMVECTOR moved = newest - before;
    XMVECTOR heading = XMLoadFloat3(&target.Heading);
    if (XMVectorGetX(XMVector3LengthSq(moved)) > 1e-6f)
    {
      moved = XMVector3Normalize(moved);
      heading = XMVectorGetX(XMVector3LengthSq(heading)) < 0.5f ? moved :
        XMVector3Normalize(XMVectorLerp(heading, moved, 0.15f));
      XMStoreFloat3(&target.Heading, heading);
    }

    // Rows: right, up, forward, position, like the game's transforms
    XMVECTOR forward = XMVectorGetX(XMVector3LengthSq(heading)) < 0.5f ? XMVectorSet(0, 0, 1, 0) : heading;
    XMVECTOR worldUp = XMVectorSet(0, 1, 0, 0);
    XMVECTOR right = XMVector3Cross(worldUp, forward);
    if (XMVectorGetX(XMVector3LengthSq(right)) < 1e-6f) // going straight up or down
      right = XMVectorSet(1, 0, 0, 0);
    right = XMVector3Normalize(right);
    XMVECTOR up = XMVector3Cross(forward, right);

    XMMATRIX matrix;
    matrix.r[0] = right;
    matrix.r[1] = up;
    matrix.r[2] = forward;
    matrix.r[3] = XMVectorSetW(where, 1);
    XMStoreFloat4x4(&transform, matrix);
    return true;
  }

  float JumpLimit(Target const& target)
  {
    // Cars and people cover a few metres in a frame at most; a record that's
    // given to a new member somewhere else moves much further
    return target.Agent >= 0 ? 15.f : 100.f;
  }
}
