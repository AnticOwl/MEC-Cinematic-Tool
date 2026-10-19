#include "TrackPlayer.h"
#include "../Main.h"
#include "../Util/Util.h"
#include "../imgui/imgui.h"
#include "../Util/ImGuiEXT.h"
#include <cmath>
#include <cstdio>
#include <string>

using namespace DirectX;

namespace
{
  const float g_minSegmentDuration = 0.05f;

  // Pitch / yaw / roll in degrees, matching XMQuaternionRotationRollPitchYaw
  // (roll, then pitch, then yaw). The camera looks down -Z, so positive pitch
  // looks up and positive yaw turns left.
  void QuaternionToAngles(XMFLOAT4 const& rotation, float angles[3])
  {
    XMFLOAT4X4 m;
    XMStoreFloat4x4(&m, XMMatrixRotationQuaternion(XMLoadFloat4(&rotation)));

    float sinPitch = -m._32;
    if (sinPitch > 1.f) sinPitch = 1.f;
    if (sinPitch < -1.f) sinPitch = -1.f;

    float pitch = asinf(sinPitch);
    float yaw, roll;
    if (cosf(pitch) > 0.0001f)
    {
      yaw = atan2f(m._31, m._33);
      roll = atan2f(m._12, m._22);
    }
    else
    {
      yaw = atan2f(-m._13, m._11);
      roll = 0;
    }

    angles[0] = XMConvertToDegrees(pitch);
    angles[1] = XMConvertToDegrees(yaw);
    angles[2] = XMConvertToDegrees(roll);
  }

  XMFLOAT4 AnglesToQuaternion(float const angles[3])
  {
    XMFLOAT4 rotation;
    XMStoreFloat4(&rotation, XMQuaternionRotationRollPitchYaw(
      XMConvertToRadians(angles[0]), XMConvertToRadians(angles[1]), XMConvertToRadians(angles[2])));
    return rotation;
  }

  const char* g_interpolationNames[Interpolation_Count] = { "Smooth", "Linear", "Ease in/out (bezier)", "Hold" };

  // Cubic Hermite from p1 to p2. Tangents come from the neighbouring keyframes
  // in units per second (a Catmull-Rom spline that knows the keyframe times),
  // then get limited to 3x the segment's own slope. That limit is what stops
  // two keyframes at the same spot from swinging out and back when a third one
  // is added elsewhere: a segment that doesn't move gets zero tangents.
  XMVECTOR ClampedHermite(XMVECTOR const& p0, XMVECTOR const& p1, XMVECTOR const& p2, XMVECTOR const& p3,
    float t0, float t1, float t2, float t3, float s)
  {
    float duration = t2 - t1;
    float length = XMVectorGetX(XMVector4Length(p2 - p1));
    if (length < 1e-6f || duration <= 0.f)
      return p1;

    XMVECTOR m1 = (p2 - p0) / (t2 - t0);
    XMVECTOR m2 = (p3 - p1) / (t3 - t1);

    float limit = 3.f * length / duration;
    float m1Length = XMVectorGetX(XMVector4Length(m1));
    float m2Length = XMVectorGetX(XMVector4Length(m2));
    if (m1Length > limit) m1 = m1 * (limit / m1Length);
    if (m2Length > limit) m2 = m2 * (limit / m2Length);

    float s2 = s * s;
    float s3 = s2 * s;
    float h00 = 2.f * s3 - 3.f * s2 + 1.f;
    float h10 = s3 - 2.f * s2 + s;
    float h01 = -2.f * s3 + 3.f * s2;
    float h11 = s3 - s2;

    return p1 * h00 + m1 * (h10 * duration) + p2 * h01 + m2 * (h11 * duration);
  }

  // CSS-style cubic-bezier(0.42, 0, 0.58, 1): starts and ends at zero speed
  float BezierEaseInOut(float x)
  {
    const float x1 = 0.42f, x2 = 0.58f;
    float t = x;
    for (int i = 0; i < 8; ++i)
    {
      float u = 1.f - t;
      float bx = 3.f * u * u * t * x1 + 3.f * u * t * t * x2 + t * t * t;
      float dbx = 3.f * u * u * x1 + 6.f * u * t * (x2 - x1) + 3.f * t * t * (1.f - x2);
      if (fabsf(dbx) < 1e-6f) break;
      t -= (bx - x) / dbx;
      if (t < 0.f) t = 0.f;
      if (t > 1.f) t = 1.f;
    }
    float u = 1.f - t;
    return 3.f * u * t * t + t * t * t;
  }

  const float g_daySeconds = 86400.f;

  // How far the clock moves from one keyframe's time of day to the next:
  // forward (through midnight if needed), or backward when asked
  float ClockStep(float from, float to, bool backward)
  {
    float step = fmodf(to - from, g_daySeconds);
    if (step < 0) step += g_daySeconds;
    if (backward && step > 0) step -= g_daySeconds;
    return step;
  }

  float WrapTimeOfDay(float seconds)
  {
    seconds = fmodf(seconds, g_daySeconds);
    return seconds < 0 ? seconds + g_daySeconds : seconds;
  }

  void FormatTimeOfDay(float seconds, char* buffer, size_t size)
  {
    int s = (int)WrapTimeOfDay(seconds);
    sprintf_s(buffer, size, "%02d:%02d", s / 3600, (s / 60) % 60);
  }

  // q and -q are the same rotation. Keep neighbouring nodes in the same
  // hemisphere so the spline doesn't spin the long way around.
  void FixRotationSigns(std::vector<CatmullRomNode>& nodes)
  {
    for (size_t i = 1; i < nodes.size(); ++i)
    {
      XMVECTOR qPrevious = XMLoadFloat4(&nodes[i - 1].Rotation);
      XMVECTOR qCurrent = XMLoadFloat4(&nodes[i].Rotation);
      if (XMVectorGetX(XMQuaternionDot(qPrevious, qCurrent)) < 0)
        XMStoreFloat4(&nodes[i].Rotation, XMVectorNegate(qCurrent));
    }
  }

  // Same position, rotation and FOV (within a centimetre / a few hundredths
  // of a degree), so the camera holds still between the two
  bool IsSameView(CatmullRomNode const& a, CatmullRomNode const& b)
  {
    float distance = XMVectorGetX(XMVector3Length(XMLoadFloat3(&a.Position) - XMLoadFloat3(&b.Position)));
    float dot = fabsf(XMVectorGetX(XMQuaternionDot(XMLoadFloat4(&a.Rotation), XMLoadFloat4(&b.Rotation))));
    return distance < 0.01f && dot > 0.9999999f && fabsf(a.FieldOfView - b.FieldOfView) < 0.01f;
  }
}

TrackPlayer::TrackPlayer() :
  m_IsPlaying(false),
  m_LockRotation(true),
  m_LockFieldOfView(true),
  m_ManualPlay(false),
  m_PlaybackSpeed(1.0f),
  m_DefaultDuration(3.0f),
  m_DefaultInterpolation(Interpolation_Smooth),
  m_CurrentNode(0),
  m_CurrentTime(0),
  m_SelectedTrack(0),
  m_RunningId(2)
{
  m_Tracks.emplace_back("Track #1");
  UpdateNameList();
}

TrackPlayer::~TrackPlayer()
{

}

void TrackPlayer::CreateNode(Camera const& camera)
{
  if (m_IsPlaying) return;

  std::vector<CatmullRomNode>& nodes = m_Tracks[m_SelectedTrack].Nodes;

  CatmullRomNode newNode;
  newNode.FieldOfView = camera.FieldOfView;
  newNode.Rotation = camera.Rotation;
  newNode.Position = camera.Position;
  newNode.TimeStamp = 0;
  newNode.Interpolation = m_DefaultInterpolation;
  newNode.MotionBlur = camera.MotionBlur;
  newNode.TimeOfDay = camera.TimeOfDay;

  if (!nodes.empty())
  {
    CatmullRomNode const& lastNode = nodes.back();
    newNode.TimeStamp = lastNode.TimeStamp + m_DefaultDuration;

    // q and -q are the same rotation. Keep neighbouring nodes in the same
    // hemisphere so the spline doesn't spin the long way around.
    XMVECTOR qLast = XMLoadFloat4(&lastNode.Rotation);
    XMVECTOR qNew = XMLoadFloat4(&newNode.Rotation);
    if (XMVectorGetX(XMQuaternionDot(qLast, qNew)) < 0)
      XMStoreFloat4(&newNode.Rotation, XMVectorNegate(qNew));
  }

  nodes.push_back(newNode);
  util::log::Write("Node created, total nodes: %d", (int)nodes.size());
  if (nodes.size() >= 2 && IsSameView(nodes[nodes.size() - 2], newNode))
    util::log::Warning("Keyframe %d is the same view as the one before it, the camera holds still between them", (int)nodes.size());
}

void TrackPlayer::DeleteNode()
{
  if (m_IsPlaying) return;

  std::vector<CatmullRomNode>& nodes = m_Tracks[m_SelectedTrack].Nodes;
  if (nodes.size() == 0) return;

  nodes.pop_back();
  util::log::Write("Deleted node, remaining nodes %d", (int)nodes.size());
}

void TrackPlayer::Toggle()
{
  if (m_IsPlaying)
  {
    Stop();
    return;
  }

  std::vector<CatmullRomNode>& nodes = m_Tracks[m_SelectedTrack].Nodes;
  if (nodes.size() <= 1)
  {
    util::log::Warning("Can't play a camera track with less than 2 nodes");
    return;
  }

  m_IsPlaying = true;
  m_CurrentTime = 0;
  m_CurrentNode = 0;
  util::log::Write("Playing %s, %.2f seconds", m_Tracks[m_SelectedTrack].Name.c_str(), nodes.back().TimeStamp / m_PlaybackSpeed);
}

void TrackPlayer::Stop()
{
  if (!m_IsPlaying) return;

  m_IsPlaying = false;
  util::log::Write("Track stopped");
}

CatmullRomNode TrackPlayer::PlayForward(double dt, bool ignoreManual /* = false */)
{
  std::vector<CatmullRomNode>& nodes = m_Tracks[m_SelectedTrack].Nodes;
  if (nodes.size() < 2)
  {
    m_IsPlaying = false;
    return nodes.empty() ? CatmullRomNode() : nodes[0];
  }

  // If manual play is enabled, time is moved with the Up/Down
  // actions (triggers on a gamepad). IgnoreManual is for previews.
  if (!m_ManualPlay || ignoreManual)
    m_CurrentTime += dt * m_PlaybackSpeed;
  else
  {
    InputSystem* pInput = g_mainHandle->GetInputSystem();
    float input = pInput->GetActionState(Action::Camera_Up) - pInput->GetActionState(Action::Camera_Down);
    // Quadratic response that keeps the direction, so it can go backwards too
    m_CurrentTime += dt * input * std::fabs(input) * m_PlaybackSpeed;
  }

  // Clamp to the ends of the track
  double endTime = nodes.back().TimeStamp;
  if (m_CurrentTime <= 0)
  {
    m_CurrentTime = 0;
    m_CurrentNode = 0;
    return nodes.front();
  }

  if (m_CurrentTime >= endTime)
  {
    m_CurrentTime = endTime;
    m_CurrentNode = (unsigned int)nodes.size() - 2;
    if (!m_ManualPlay)
    {
      m_IsPlaying = false;
      util::log::Write("Track finished");
    }
    return nodes.back();
  }

  // Move to the segment that contains the current time
  unsigned int lastSegment = (unsigned int)nodes.size() - 2;
  if (m_CurrentNode > lastSegment)
    m_CurrentNode = lastSegment;

  while (m_CurrentNode < lastSegment && m_CurrentTime >= nodes[m_CurrentNode + 1].TimeStamp)
    m_CurrentNode++;

  while (m_CurrentNode > 0 && m_CurrentTime < nodes[m_CurrentNode].TimeStamp)
    m_CurrentNode--;

  CatmullRomNode const& n1 = nodes[m_CurrentNode];
  CatmullRomNode const& n2 = nodes[m_CurrentNode + 1];
  CatmullRomNode const& n0 = m_CurrentNode > 0 ? nodes[m_CurrentNode - 1] : n1;
  CatmullRomNode const& n3 = m_CurrentNode + 2 < nodes.size() ? nodes[m_CurrentNode + 2] : n2;

  float mu = (float)((m_CurrentTime - n1.TimeStamp) / (n2.TimeStamp - n1.TimeStamp));

  CatmullRomNode resultNode;
  resultNode.TimeStamp = (float)m_CurrentTime;

  // Time of day, unwrapped around n1 so it moves the way the clock runs
  // (u2 can be past 86400 or below 0). Only between two recorded times.
  bool hasTimeOfDay = n1.TimeOfDay >= 0 && n2.TimeOfDay >= 0;
  float u1 = n1.TimeOfDay;
  float u2 = hasTimeOfDay ? u1 + ClockStep(n1.TimeOfDay, n2.TimeOfDay, n1.TimeOfDayBackward) : u1;
  float u0 = (hasTimeOfDay && &n0 != &n1 && n0.TimeOfDay >= 0) ? u1 - ClockStep(n0.TimeOfDay, n1.TimeOfDay, n0.TimeOfDayBackward) : u1;
  float u3 = (hasTimeOfDay && &n3 != &n2 && n3.TimeOfDay >= 0) ? u2 + ClockStep(n2.TimeOfDay, n3.TimeOfDay, n2.TimeOfDayBackward) : u2;
  resultNode.TimeOfDay = hasTimeOfDay ? n1.TimeOfDay : -1.f;

  if (n1.Interpolation == Interpolation_Hold)
  {
    resultNode.Position = n1.Position;
    resultNode.Rotation = n1.Rotation;
    resultNode.FieldOfView = n1.FieldOfView;
    resultNode.MotionBlur = n1.MotionBlur;
    return resultNode;
  }

  XMVECTOR qRot1 = XMLoadFloat4(&n1.Rotation);
  XMVECTOR qRot2 = XMLoadFloat4(&n2.Rotation);
  XMVECTOR vPos1 = XMLoadFloat3(&n1.Position);
  XMVECTOR vPos2 = XMLoadFloat3(&n2.Position);

  if (n1.Interpolation == Interpolation_Linear)
  {
    XMStoreFloat3(&resultNode.Position, XMVectorLerp(vPos1, vPos2, mu));
    XMStoreFloat4(&resultNode.Rotation, XMQuaternionSlerp(qRot1, qRot2, mu));
    resultNode.FieldOfView = n1.FieldOfView + (n2.FieldOfView - n1.FieldOfView) * mu;
    resultNode.MotionBlur = n1.MotionBlur + (n2.MotionBlur - n1.MotionBlur) * mu;
    if (hasTimeOfDay)
      resultNode.TimeOfDay = WrapTimeOfDay(u1 + (u2 - u1) * mu);
    return resultNode;
  }

  // Smooth, optionally with the timing eased in and out of the keyframes
  float s = (n1.Interpolation == Interpolation_EaseInOut) ? BezierEaseInOut(mu) : mu;

  XMVECTOR qRot0 = XMLoadFloat4(&n0.Rotation);
  XMVECTOR qRot3 = XMLoadFloat4(&n3.Rotation);
  XMVECTOR vPos0 = XMLoadFloat3(&n0.Position);
  XMVECTOR vPos3 = XMLoadFloat3(&n3.Position);

  float t0 = n0.TimeStamp, t1 = n1.TimeStamp, t2 = n2.TimeStamp, t3 = n3.TimeStamp;

  XMVECTOR resultPos = ClampedHermite(vPos0, vPos1, vPos2, vPos3, t0, t1, t2, t3, s);
  XMVECTOR resultRot = ClampedHermite(qRot0, qRot1, qRot2, qRot3, t0, t1, t2, t3, s);
  XMVECTOR resultFov = ClampedHermite(XMVectorReplicate(n0.FieldOfView), XMVectorReplicate(n1.FieldOfView),
    XMVectorReplicate(n2.FieldOfView), XMVectorReplicate(n3.FieldOfView), t0, t1, t2, t3, s);
  XMVECTOR resultBlur = ClampedHermite(XMVectorReplicate(n0.MotionBlur), XMVectorReplicate(n1.MotionBlur),
    XMVectorReplicate(n2.MotionBlur), XMVectorReplicate(n3.MotionBlur), t0, t1, t2, t3, s);

  XMStoreFloat3(&resultNode.Position, resultPos);
  XMStoreFloat4(&resultNode.Rotation, XMQuaternionNormalize(resultRot));
  resultNode.FieldOfView = XMVectorGetX(resultFov);
  resultNode.MotionBlur = XMVectorGetX(resultBlur);

  if (hasTimeOfDay)
  {
    XMVECTOR resultTime = ClampedHermite(XMVectorReplicate(u0), XMVectorReplicate(u1),
      XMVectorReplicate(u2), XMVectorReplicate(u3), t0, t1, t2, t3, s);
    resultNode.TimeOfDay = WrapTimeOfDay(XMVectorGetX(resultTime));
  }

  return resultNode;
}

void TrackPlayer::DrawUI(Camera& camera, bool cameraEnabled)
{
  InputSystem* pInput = g_mainHandle->GetInputSystem();
  float fontSize = ImGui::GetFontSize();
  char label[160];

  // Track picker, or the name box while renaming
  static bool s_focusRename = false;
  ImGui::PushItemWidth(fontSize * 10.f);
  if (m_RenamingTrack == m_SelectedTrack)
  {
    if (s_focusRename)
    {
      ImGui::SetKeyboardFocusHere();
      s_focusRename = false;
    }
    bool done = ImGui::InputText("##TrackName", m_RenameBuffer, sizeof(m_RenameBuffer),
      ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
    ImGui::SameLine();
    done |= ImGui::Button("Done");
    if (done)
    {
      if (m_RenameBuffer[0])
        m_Tracks[m_SelectedTrack].Name = m_RenameBuffer;
      m_RenamingTrack = -1;
      UpdateNameList();
    }
  }
  else
  {
    m_RenamingTrack = -1;
    if (ImGui::Combo("##CameraTrackList", &m_SelectedTrack, m_TrackNames.data(), (int)m_TrackNames.size()))
    {
      Stop();
      m_CurrentTime = 0;
      m_CurrentNode = 0;
    }
    ImGui::SameLine();
    if (ImGui::Button("Rename") && !m_IsPlaying)
    {
      strcpy_s(m_RenameBuffer, m_Tracks[m_SelectedTrack].Name.c_str());
      m_RenamingTrack = m_SelectedTrack;
      s_focusRename = true;
    }
  }
  ImGui::PopItemWidth();
  ImGui::SameLine();
  if (ImGui::Button("New track"))
    CreateTrack();
  if (m_Tracks.size() > 1)
  {
    ImGui::SameLine();
    if (ImGui::ConfirmButton("Delete track"))
      DeleteTrack();
  }

  std::vector<CatmullRomNode>& nodes = m_Tracks[m_SelectedTrack].Nodes;
  float trackLength = nodes.empty() ? 0.f : nodes.back().TimeStamp;

  // Play / stop and where the track is
  if (cameraEnabled && nodes.size() >= 2)
  {
    sprintf_s(label, "%s  (%s)###PlayTrack", m_IsPlaying ? "Stop" : "Play", pInput->GetKeyName(Action::Track_Play).c_str());
    if (ImGui::Button(label, ImVec2(fontSize * 8.f, 0)))
      Toggle();
    ImGui::SameLine();
    double shownTime = m_IsPlaying ? m_CurrentTime : 0.0;
    sprintf_s(label, "%.2f / %.2f s", shownTime, trackLength);
    ImGui::ProgressBar(trackLength > 0 ? (float)(shownTime / trackLength) : 0.f, ImVec2(-1, 0), label);
  }

  ImGui::SliderFloat("Playback speed", &m_PlaybackSpeed, 0.1f, 4.f, "%.2fx");
  ImGui::HelpMarker("1x plays the keyframes at their own times. At 0.5x the track takes twice as long.");

  // Adding and removing keyframes
  if (!cameraEnabled)
  {
    ImGui::PushTextWrapPos(0.f);
    ImGui::TextDisabled("Turn the camera on to add keyframes and play tracks.");
    ImGui::PopTextWrapPos();
  }
  else if (!m_IsPlaying)
  {
    sprintf_s(label, "Add keyframe  (%s)###AddKeyframe", pInput->GetKeyName(Action::Track_CreateNode).c_str());
    if (ImGui::Button(label))
      CreateNode(camera);
    ImGui::SameLine();
    sprintf_s(label, "Delete last  (%s)###DeleteKeyframe", pInput->GetKeyName(Action::Track_DeleteNode).c_str());
    if (ImGui::Button(label))
      DeleteNode();
    if (!nodes.empty())
    {
      ImGui::SameLine();
      if (ImGui::ConfirmButton("Clear all"))
        nodes.clear();
    }

    ImGui::PushTextWrapPos(0.f);
    ImGui::TextDisabled(nodes.size() < 2
      ? "Fly to the first shot and add a keyframe, then fly to the next shot and add another. "
        "With this menu open, hold the right mouse button outside the window to look around."
      : "Expand a keyframe to change its position, angles, timing or how it moves to the next one. "
        "The arrows on a keyframe move it earlier or later in the track.");
    ImGui::PopTextWrapPos();
  }

  // The keyframes, as a red bar like the menu tabs so the list is easy to
  // find, with one tinted row per keyframe. The one playing is brighter.
  int deleteIndex = -1;
  int moveFrom = -1, moveTo = -1;
  const ImVec4 red = ImGui::GetStyleColorVec4(ImGuiCol_Button);
  const ImVec4 redLight = ImGui::GetStyleColorVec4(ImGuiCol_ButtonHovered);
  const ImVec4 redBright = ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive);
  auto withAlpha = [](ImVec4 color, float alpha) { color.w = alpha; return color; };

  ImGui::Dummy(ImVec2(0, ImGui::GetStyle().ItemSpacing.y));
  sprintf_s(label, "KEYFRAMES    %d###TrackKeyframes", (int)nodes.size());
  ImGui::PushStyleColor(ImGuiCol_Header, red);
  ImGui::PushStyleColor(ImGuiCol_HeaderHovered, redLight);
  ImGui::PushStyleColor(ImGuiCol_HeaderActive, redBright);
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(ImGui::GetStyle().FramePadding.x, ImGui::GetStyle().FramePadding.y * 1.8f));
  bool showKeyframes = ImGui::TreeNodeEx(label, ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_Framed);
  ImGui::PopStyleVar();
  ImGui::PopStyleColor(3);

  if (showKeyframes && nodes.empty())
  {
    ImGui::PushTextWrapPos(0.f);
    ImGui::TextDisabled("No keyframes yet. Turn the camera on, fly to the first shot and press %s (or Add keyframe).",
      pInput->GetKeyName(Action::Track_CreateNode).c_str());
    ImGui::PopTextWrapPos();
  }

  for (size_t i = 0; showKeyframes && i < nodes.size(); ++i)
  {
    CatmullRomNode& node = nodes[i];

    ImGui::PushID((int)i);
    const char* interpolationName = (i + 1 < nodes.size() && node.Interpolation >= 0 && node.Interpolation < Interpolation_Count)
      ? g_interpolationNames[node.Interpolation] : "last keyframe";
    bool sameView = i > 0 && IsSameView(nodes[i - 1], node);
    bool playingThis = m_IsPlaying && i == m_CurrentNode;
    char timeOfDay[16] = "";
    if (m_LockTimeOfDay && node.TimeOfDay >= 0)
    {
      timeOfDay[0] = ' '; timeOfDay[1] = ' '; timeOfDay[2] = ' '; timeOfDay[3] = ' ';
      FormatTimeOfDay(node.TimeOfDay, timeOfDay + 4, sizeof(timeOfDay) - 4);
    }
    ImGui::PushStyleColor(ImGuiCol_Header, playingThis ? withAlpha(redBright, 0.9f) : withAlpha(red, 0.35f));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, withAlpha(redLight, playingThis ? 1.f : 0.6f));
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, redBright);
    bool open = ImGui::TreeNodeEx("##Keyframe", ImGuiTreeNodeFlags_Framed | ImGuiTreeNodeFlags_AllowItemOverlap,
      "%s%d    %.2f s    %s%s%s", playingThis ? "> Keyframe " : "Keyframe ", (int)i + 1,
      node.TimeStamp, interpolationName, timeOfDay, sameView ? "    (same view as the one before)" : "");
    ImGui::PopStyleColor(3);

    // Move up / down, at the right end of the row. Both are always there (faint
    // when there's nowhere to move) so they line up from row to row.
    if (!m_IsPlaying && nodes.size() > 1)
    {
      float arrowSize = ImGui::GetFrameHeight();
      ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - arrowSize * 2.f);
      if (ImGui::MoveArrowButton("##Up", ImGuiDir_Up, i > 0))
      {
        moveFrom = (int)i;
        moveTo = (int)i - 1;
      }
      if (i > 0)
        ImGui::ItemTooltip("Move this keyframe earlier (its time stays)");
      ImGui::SameLine(0.f, 0.f);
      if (ImGui::MoveArrowButton("##Down", ImGuiDir_Down, i + 1 < nodes.size()))
      {
        moveFrom = (int)i;
        moveTo = (int)i + 1;
      }
      if (i + 1 < nodes.size())
        ImGui::ItemTooltip("Move this keyframe later (its time stays)");
    }

    if (open)
    {
      if (m_IsPlaying)
        ImGui::TextDisabled("Stop the track to edit keyframes");
      else
      {
        ImGui::PushItemWidth(fontSize * 15.f);

        // While one of these is being changed the camera shows the keyframe live
        bool preview = false;
        preview |= ImGui::DragFloat3("Position", &node.Position.x, 0.05f, 0, 0, "%.2f");
        preview |= ImGui::IsItemActive();

        float angles[3];
        QuaternionToAngles(node.Rotation, angles);
        if (ImGui::DragFloat3("Pitch / yaw / roll", angles, 0.25f, 0, 0, "%.1f" UI_DEG))
        {
          node.Rotation = AnglesToQuaternion(angles);
          FixRotationSigns(nodes);
          preview = true;
        }
        preview |= ImGui::IsItemActive();

        preview |= ImGui::DragFloat("Field of view", &node.FieldOfView, 0.1f, 5.f, 150.f, "%.1f" UI_DEG);
        preview |= ImGui::IsItemActive();
        ImGui::DragFloat("Motion blur", &node.MotionBlur, 0.01f, 0.f, 10.f, "%.2f");
        ImGui::HelpMarker("Motion blur strength at this keyframe. Used while \"Control motion blur\" is on "
          "and the track controls motion blur (Keyframe options).");

        if (node.TimeOfDay >= 0 && util::IsMecmpLoaded())
          ImGui::TextDisabled("Time of day: disabled while MECMP is open (it syncs the time of day)");
        else if (node.TimeOfDay >= 0)
        {
          char clock[16];
          FormatTimeOfDay(node.TimeOfDay, clock, sizeof(clock));
          bool timeChanged = ImGui::SliderFloat("Time of day", &node.TimeOfDay, 0.f, 86399.f, clock);
          bool timeActive = ImGui::IsItemActive();
          ImGui::HelpMarker("The time of day at this keyframe. Ctrl+click to type it in seconds (0 - 86400).");

          // Changing a keyframe's time means the track should use it, so
          // the time of day channel goes on by itself
          if (timeChanged && !m_LockTimeOfDay)
          {
            m_LockTimeOfDay = true;
            util::log::Write("The track now sets the time of day");
          }
          // And the game shows it live while it's being changed
          if ((timeChanged || timeActive) && cameraEnabled)
            g_mainHandle->GetVisualsManager()->SetTimeOfDay(node.TimeOfDay);

          ImGui::Checkbox("Track sets the time of day", &m_LockTimeOfDay);
          ImGui::HelpMarker("The same as Time of day in Keyframe options. Off: the keyframes keep their "
            "times, but playing the track leaves the game's clock alone.");
          if (i + 1 < nodes.size())
          {
            ImGui::Checkbox("Clock runs backwards to the next", &node.TimeOfDayBackward);
            ImGui::HelpMarker("Normally the clock runs forward to the next keyframe's time, through midnight "
              "if it has to (06:00 to 05:00 is 23 hours). Tick this to turn it back instead.");
          }
        }
        else if (camera.TimeOfDay >= 0)
        {
          if (ImGui::Button("Use the current time of day"))
            node.TimeOfDay = camera.TimeOfDay;
          ImGui::ItemTooltip("This keyframe was added without a level loaded, so it has no time of day.");
        }

        // Edit the time to the next node, later nodes move with it
        if (i + 1 < nodes.size())
        {
          ImGui::Combo("Move to the next", &node.Interpolation, g_interpolationNames, Interpolation_Count);

          float currentDuration = nodes[i + 1].TimeStamp - node.TimeStamp;
          float duration = currentDuration;
          if (ImGui::InputFloat("Seconds to the next", &duration, 0.1f, 1.f, 2))
          {
            if (duration < g_minSegmentDuration)
              duration = g_minSegmentDuration;

            float delta = duration - currentDuration;
            for (size_t j = i + 1; j < nodes.size(); ++j)
              nodes[j].TimeStamp += delta;
          }
        }
        ImGui::PopItemWidth();

        if (preview && cameraEnabled)
        {
          camera.Position = node.Position;
          camera.Rotation = node.Rotation;
          camera.FieldOfView = node.FieldOfView;
        }

        if (cameraEnabled)
        {
          if (ImGui::Button("Go to"))
          {
            camera.Position = node.Position;
            camera.Rotation = node.Rotation;
            camera.FieldOfView = node.FieldOfView;
            camera.MotionBlur = node.MotionBlur;
            if (m_LockTimeOfDay && node.TimeOfDay >= 0)
            {
              camera.TimeOfDay = node.TimeOfDay;
              g_mainHandle->GetVisualsManager()->SetTimeOfDay(node.TimeOfDay);
            }
          }
          ImGui::ItemTooltip("Moves the camera to this keyframe (and the clock, when the track sets the time of day).");
          ImGui::SameLine();
          if (ImGui::Button("Set from camera"))
          {
            node.Position = camera.Position;
            node.Rotation = camera.Rotation;
            node.FieldOfView = camera.FieldOfView;
            node.MotionBlur = camera.MotionBlur;
            if (camera.TimeOfDay >= 0)
              node.TimeOfDay = camera.TimeOfDay;
            FixRotationSigns(nodes);
          }
          ImGui::ItemTooltip("Replaces this keyframe with where the camera is now.");
          ImGui::SameLine();
        }
        if (ImGui::Button("Delete"))
          deleteIndex = (int)i;
      }
      ImGui::TreePop();
    }
    ImGui::PopID();
  }
  if (showKeyframes)
    ImGui::TreePop();

  if (moveFrom >= 0 && moveTo >= 0 && moveTo < (int)nodes.size() && !m_IsPlaying)
  {
    // The shots change order, the timing doesn't: the two keyframes swap
    // everything except their times
    std::swap(nodes[moveFrom], nodes[moveTo]);
    std::swap(nodes[moveFrom].TimeStamp, nodes[moveTo].TimeStamp);
    FixRotationSigns(nodes);
    util::log::Write("Moved keyframe %d %s", moveFrom + 1, moveTo < moveFrom ? "up" : "down");
  }
  else if (deleteIndex >= 0)
  {
    nodes.erase(nodes.begin() + deleteIndex);
    // Playback starts at 0, keep the first keyframe there
    if (!nodes.empty())
    {
      float start = nodes[0].TimeStamp;
      for (CatmullRomNode& node : nodes)
        node.TimeStamp -= start;
    }
    FixRotationSigns(nodes);
    util::log::Write("Deleted keyframe %d, remaining %d", deleteIndex + 1, (int)nodes.size());
  }

  if (ImGui::TreeNode("Keyframe options"))
  {
    ImGui::TextDisabled("Keyframes set the camera's position and its:");
    ImGui::Checkbox("Rotation", &m_LockRotation);
    ImGui::SameLine();
    ImGui::Checkbox("Field of view", &m_LockFieldOfView);
    ImGui::SameLine();
    ImGui::Checkbox("Motion blur", &m_LockMotionBlur);
    ImGui::SameLine();
    if (util::IsMecmpLoaded())
      ImGui::TextDisabled("(Time of day: disabled, MECMP syncs it)");
    else
      ImGui::Checkbox("Time of day", &m_LockTimeOfDay);
    ImGui::HelpMarker("Untick one to control it yourself during playback, like turning the camera "
      "with the mouse while the track moves it.\n\nTime of day: every keyframe stores the time when it's "
      "added, and with this ticked the track sets the game's clock between them, so a track can go from "
      "day to night. The clock runs forward to the next keyframe (06:00, 18:00, 06:00 is a full day and "
      "night). Freeze the clock (Time and HUD) while adding keyframes to choose each time yourself.");

    ImGui::Checkbox("Play manually", &m_ManualPlay);
    sprintf_s(label, "Instead of playing by itself, the track moves forward and back with Move up / Move down "
      "(%s / %s, or the gamepad triggers).", pInput->GetKeyName(Action::Camera_Up).c_str(), pInput->GetKeyName(Action::Camera_Down).c_str());
    ImGui::HelpMarker(label);

    ImGui::InputFloat("Time between new keyframes", &m_DefaultDuration, 0.5f, 1.f, 2);
    if (m_DefaultDuration < g_minSegmentDuration)
      m_DefaultDuration = g_minSegmentDuration;
    ImGui::HelpMarker("Seconds. Each keyframe can be changed afterwards (Seconds to the next).");

    ImGui::Combo("New keyframes move", &m_DefaultInterpolation, g_interpolationNames, Interpolation_Count);
    ImGui::HelpMarker("Smooth: a curve through the keyframes.\nLinear: straight lines at a constant speed.\n"
      "Ease in/out: the smooth curve, slowing down into and out of each keyframe.\n"
      "Hold: stays on the keyframe, then cuts to the next one.");
    ImGui::SameLine();
    if (ImGui::Button("Apply to all") && !m_IsPlaying)
    {
      for (CatmullRomNode& node : nodes)
        node.Interpolation = m_DefaultInterpolation;
    }
    ImGui::TreePop();
  }
}

int TrackPlayer::GetStreamingPoints(XMFLOAT3* points, int max, float spacing)
{
  std::vector<CatmullRomNode> const& nodes = m_Tracks[m_SelectedTrack].Nodes;
  int count = 0;
  auto add = [&](XMVECTOR point)
  {
    // Keyframes at the same spot need only one
    if (count > 0 && XMVectorGetX(XMVector3Length(point - XMLoadFloat3(&points[count - 1]))) < 1.f)
      return;
    if (count < max)
      XMStoreFloat3(&points[count++], point);
  };

  for (size_t i = 0; i < nodes.size() && count < max; ++i)
  {
    XMVECTOR a = XMLoadFloat3(&nodes[i].Position);
    add(a);
    if (i + 1 < nodes.size())
    {
      // Straight lines are close enough, zones are much bigger than the curve
      XMVECTOR b = XMLoadFloat3(&nodes[i + 1].Position);
      int steps = (int)(XMVectorGetX(XMVector3Length(b - a)) / spacing);
      for (int k = 1; k <= steps && count < max; ++k)
        add(XMVectorLerp(a, b, (float)k / (steps + 1)));
    }
  }
  return count;
}

void TrackPlayer::CreateTrack()
{
  if (m_IsPlaying) return;
  m_RenamingTrack = -1;

  m_Tracks.emplace_back("Track #" + std::to_string(m_RunningId++));
  m_SelectedTrack = (int)m_Tracks.size() - 1;

  UpdateNameList();
}

void TrackPlayer::DeleteTrack()
{
  if (m_IsPlaying || m_Tracks.size() <= 1) return;
  // Otherwise the rename box could open on the track that takes its place
  m_RenamingTrack = -1;

  m_Tracks.erase(m_Tracks.begin() + m_SelectedTrack);
  if (m_SelectedTrack >= (int)m_Tracks.size())
    m_SelectedTrack -= 1;

  UpdateNameList();
}

void TrackPlayer::UpdateNameList()
{
  // Must be rebuilt whenever m_Tracks changes, the strings can move
  m_TrackNames.clear();
  for (auto& track : m_Tracks)
    m_TrackNames.push_back(track.Name.c_str());
}
