#include "Weather.h"
#include "../Main.h"
#include "../Util/Util.h"
#include "../Util/GameState.h"
#include "../Camera/CameraCollision.h"
#include "../imgui/imgui.h"
#include "../Util/ImGuiEXT.h"
#include "Thunder.h"
#include "DryShapes.h"
#include "GbufferWet.h"
#include "WetLook.h"
#include "VisualsManager.h"
#include "IceTable.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

// Weather (2026-10-25, offline; Research_Weather_2026-10-25.md). The game has
// no weather system: its rain is effects placed in a few city prop zones of
// the Release district (centralcityprops_zs_zs_293 / 296 / 312 / 315 / 595 /
// 753, layer 0, AutoStart). One of them, FX/Ambient/Rain/FX_Rain_Camprox
// (zone 753), is the rain around the camera: its streak emitters spawn in a
// box around the camera (CameraProximityEvaluatorData) wherever the effect
// is, but the effect is culled 150 m from where it was placed; zone 293's
// FX_Rain_Camprox_Thickener adds more (culled at 60 m). So the tools keep the
// game's own instances at the camera.
//
// A placed effect (EffectEntity) doesn't go through the effect manager's play
// (0x14329B860, what the effect hooks see): its start (0x1430600D0) goes to the
// emitter system itself (0x1430F02D0, 0x1430F9150), so the hooks never saw the
// rain (2026-10-25 17:0x, the game's log: no start of it in a whole session
// with its zone loaded). Its instance (the object at EffectEntity + 0x50, its
// first qword its EffectEntityData) is in the client effect manager's list of
// them, [[0x142416160] + 0x48C0 .. + 0x48C8] (pointers), which the manager's
// culling (0x1432A1FF0 -> 0x1432905B0) goes over: farther from every view
// than its CullDistance (data + 0x78) it's culled (0x14305F350: stopped,
// remembered as playing, + 0x1B8 bit 0x20), and started again when it's back
// in range. Its place is the 4x4 at + 0x100; a Transform property puts the new
// one there, sets + 0x1B8 bit 2 and calls 0x1430606F0(instance) (the
// EffectEntity's handler 0x14305F010). The tools find the rain's instances by
// their first emitter's name (data + 0x60 its components, the first an
// EmitterEntityData: + 0xA0 its emitter document, + 0x28 the high quality
// EmitterTemplateData, + 0x158 its DebugName; the documents themselves have
// no names), move them to the camera the same way while Rain is on (so they're
// never culled and play), and give them back their place when it's off. They
// exist only while their zones are loaded: the tools' streaming loads them
// (ZonePoints, the effects' places).
//
// Rain kept out of buildings: the game culls a rain particle inside an
// emitter exclusion volume. The rain's emitters have ExclusionVolumeCullEnable
// (EmitterTemplateData + 0x165): at the end of each emitter's update
// (0x1430629D0 -> 0x1430F23C0) every particle is tested against the volumes
// near the emitter (gathered by 0x1430E7620, 32 at most). The level's volumes
// are boxes placed by hand in the Release interiors
// (EmitterExclusionVolumeEntity). They live in a manager at [0x1423E4148]:
// + 0x00 / + 0x08 a stack of freed slots (u16; + 0x10 its end), + 0x820 how
// many slots are in use (at most 0x400), a box per slot set by
// 0x1430FBDE0(manager, slot, its 4x4: the axes' lengths are the box's sizes,
// the translation its middle). An entity takes a slot from the stack or the
// next one (0x1430F4DF0) and gives it back with the box at 0x1423E41F0 (a unit
// box at 100000, 100000, 100000) onto the stack (0x1430F4E9C); the emitter
// system copies them every update (0x1430F40F4). The tools do the same with
// boxes of their own: around the camera an upward sweep from each 2 m square
// looks for a roof (the camera collision's sweep), and the squares under one
// become a few boxes from under the camera up to just below it.

// The sound system as the hooks have it (Util/Hooks.cpp): a voice made for a
// sound asset (0x143019860), placed (0x14301DC30), given an event (0x14301D5F0)
typedef __int64(__fastcall* tSoundCreate)(__int64, __int64, __int64, __int64);
typedef void(__fastcall* tVoiceSetTransform)(__int64, float const*);
typedef __int64(__fastcall* tVoiceEvent)(__int64, unsigned int);
extern tSoundCreate oSoundCreate;
extern tVoiceSetTransform oVoiceSetTransform;
extern tVoiceEvent oVoiceEvent;

// The rain's sound. The game's rain ambiences are in the Release mission's
// level only (me_rain_exposed_ambience, me_rain_background_ambience;
// levels/sp/sp_maincity/release/layer13_release_audio), but the rain columns
// in the rain zones (FX_Ambient_Rain_Opening_Large, five in zone 753) carry a
// rain sound of their own (ME_Sound/World/Shared/Rain/SpotRainOpeningLarge,
// me_rain_medium), made when one plays (in its 50 m: one is moved under the
// camera for a moment if none has). The hooks see the game make its voice and
// give it its first events; the tools make a voice of their own for that sound (as the
// replay does, the voice manager [0x1423E29B8]), give it the same events and
// keep it at the camera, or under a roof at the nearest square open to the
// sky. Let go (its slot 9) when the rain goes, before its zone can unload.
//
// Thunder and lightning: the light first, a flash in a few pulses (the
// visual environment update brightens the sky, the sky light and the
// exposure, see VisualsManager), the thunder after it at the speed of sound
// (made by the tools, Thunder.cpp).

namespace
{
  const __int64 EffectManagerRva = 0x2416160;    // [ ]: the client effect manager
  const int AssetNameOffset = 0x10;
  const int RenderViewTransform = 0x40;          // the camera's LinearTransform in the render view

  struct GameRain
  {
    const char* Emitter;      // its first emitter's DebugName (see above)
    const char* Label;
    float ZonePoint[3];       // where the game places it (its zone gets loaded there)
    __int64 Instance;         // its effect instance, 0 not found (its zone not loaded)
    __int64 Data;             // the instance's EffectEntityData
    alignas(16) float Home[16];
    bool Followed;            // moved to the camera by the tools (so put back when let go)
  };
  // FX/Ambient/Rain/FX_Rain_Camprox (zone 753) and FX_Rain_Camprox_Thickener (zone 293)
  GameRain g_Rain[2] = {
    { "Em_Evo_Rain_CamDroplets_02", "rain", { 44.076f, 31.1437f, 831.912f } },
    { "Em_Evo_Rain_Streaks_MidToFar_unAA", "heavy rain", { 40.7707f, 5.94975f, 1187.72f } },
  };
  const size_t RainKinds = 2;

  struct Settings
  {
    bool Rain{ false };
    bool RandomRain{ false };        // rain at random times of the game's day (see the forecast)
    float RainChance{ 0.3f };        // about how much of the day it rains, 0 - 1
    bool ThunderWithRain{ true };    // Random rain: thunder and lightning come and go with it
    bool LookWithRain{ true };       // Random rain: the rainy city look (fog, grey sky, no sun) comes and goes with it
    float Strength{ 0.7f };          // EmitterParameter3 while it rains (the Release mission's ve_release_01: 0.7)
    bool Heavy{ false };
    bool KeepOut{ true };
    bool LoadZones{ true };
    bool RainSound{ true };
    bool Thunder{ false };
    float ThunderEvery{ 25.f };      // seconds between strikes, on average
    float NearestStrike{ 300.f };    // metres
    float FarthestStrike{ 2500.f };
    float ThunderVolume{ 0.8f };
    float LightningBrightness{ 1.f };
    float DistantRain{ 0.6f };       // the rain drawn by the tools past the game's (0 none)
    bool Bolts{ true };              // a lightning bolt drawn at each strike, mostly where the camera looks
    bool RainyLook{ false };
    // The game's RainShaderParam while it rains, x the rain (ve_release_01 and
    // the Rain mod: 0.2). Off and out of the menu (2026-10-25 23:4x): no
    // shader of the game reads it. In all four of its shader databases
    // (systems/shaderprogramdb, levels/sp/sp_maincity/sp_maincity/shaderdb,
    // the crowds' and gameconfigurations/pamplonaconfig's) neither its name nor
    // its djb2 hash (lower case, h = h * 33 ^ c from 5381, how the databases
    // keep parameter names) is found, while ToD_Windows (87), skyVis_addValue
    // (2705), GridlinkOn (4) and SurgeIntensity (63) are.
    float GameWet{ 0.f };
    float GreySky{ 1.f };            // 1: the sky's scattering and the fog's colour at their own grey, as TheNollz' Rain mod has them
    float LessSun{ 1.f };
    float FogSeenUpTo{ 6000.f };     // metres, at most (the city's daytime: 6000, the Release mission's: 500); under it the Rain mod's by the hour
    float FogDepth{ 2000.f };        // metres (the city's daytime: 180 - 250, the Release mission's: 50, TheNollz' Rain mod: 2000)
    // How much thicker the rainy look's fog is than TheNollz' Rain mod's by
    // the hour: what it's seen through divided by it (the user, 2026-10-26:
    // "can we also make the rain fog more intense?")
    float FogThickness{ 1.75f };
    // VISUALS > Snow (2026-10-26; Research_Snow_2026-10-26.md). The game has
    // no snow, so it's drawn by the tools (WetLook.hlsl's SnowAt and its snow
    // on the ground). Snow or rain: turning one on turns the other off.
    bool Snow{ false };
    bool RandomSnow{ false };        // snow at random times of the game's day (the forecast, as Random rain)
    float SnowChance{ 0.3f };        // about how much of the day it snows, 0 - 1
    bool SnowLook{ false };          // the snowy city look
    bool SnowLookWithSnow{ true };   // Random snow: the snowy look comes and goes with it
    // How hard it snows (2026-10-26, the user: "lets add a snow intensity slider right now it's too much of a light
    // snowing"): 0.1 - 3, 1 a moderate snow; the flakes x it (3: two and a half times the first build's most), the
    // snowy look's fog seen through 1500 m / it at most (the National Weather Service calls snow heavy at 1/4 mile,
    // 400 m, and under; light past 5/8 mile, 1 km: Rasmussen et al. 1999). Replaces "Snow amount" (0 - 1, 0.6).
    // Up to 5 (2026-10-26, the user: "can we make the snow slider more intense"): the flakes' budget is 210,000 now
    // (WetLook.cpp), intensity / 5 of it, so each place on the slider has 1.5 x the flakes it had and 5 has 2.5 x
    // the old most; from 2 up the flakes grow too (clumped together, as heavy snow's are: x 1.5 at 5), and the fog
    // is seen through 300 m at 5.
    float SnowIntensity{ 1.5f };
    float FlakeSize{ 1.f };          // x the tools' own (6 - 14 mm)
    // m/s the wind carries the snow at, on average (0.5 since 2026-10-26: at 1 it was as fast as the flakes fall,
    // so the snow came from one side; the user: "Why does snow come from only one direction?")
    float SnowWind{ 0.5f };
    // A blizzard (2026-10-26, the user: "blizzard tick doesn't work anymore": it gave x 1.5 flakes up to a cap of
    // 0.95, so with the amount at its most it added nothing): the intensity x 1.8, at least 5 and up to 7.5
    // (SnowIntensityNow), three times the wind and at least 12 m/s, the flakes streaked by it, the falling snow's
    // haze x 3.4, and the snowy look on with it, its fog 1.5 times thicker and seen through 120 m at most (GiveLook;
    // a blizzard is snow and wind with a quarter mile or less to see).
    bool Blizzard{ false };
    bool SnowKeepOut{ true };        // falls only where the weather's squares know the sky is open
    float SnowCover{ 1.f };          // snow on the ground: how much of what faces up it covers once it's all settled (0.85 until 2026-11-01 18:xx: patchy even once settled, its patches half see-through until 8 cm had piled; the user: "make the snow on the ground effect stronger")
    float SnowBrightness{ weather::SnowBrightnessDefault };   // how bright it is (see weather::SnowFall::Brightness)
    float SnowSettleMinutes{ 4.f };  // real minutes for all of it to settle while it snows hard (it melts in three times that)
    float SnowUpTo{ 400.f };         // metres: no snow on the ground past it
    float SnowFog{ 1.f };            // how much of the snowy look's fog, 0 - 25 (past 1: thicker than the mod's)
    float SnowHaze{ 1.f };           // how thick the falling snow's haze is, 0 - 8 (see GetHaze; not the intensity's)
    bool FreezeWater{ true };        // waterfalls and fountains stand still while it snows (UpdateFrozenWater)
    // Piling up (2026-10-26, the user: "the snow on the ground look like it's piling up"): once it all lies, it
    // deepens to this over three times Settles over (at intensity 1.5; faster in heavier snow), and melts with it
    bool SnowStepSound{ true };      // Faith's steps in it crunch (the user, 2026-10-26: "add a tick for the snow sound footsteps")
    // The wind's sound while it snows (2026-10-26, the user: "there should be a wind sound playing when outside and
    // muffled when indoors and the wind sound should get more intense if it's a blizzard same with the snow")
    bool WindSound{ true };
    float WindVolume{ 1.f };         // x the tools' own level
    // The wind with the rain (2026-10-26, the user: "there should also be
    // wind that gets stronger depending on how much rain there is (should be
    // muffled indoors ofc)"): the same wind sound, and the game's own wind
    // (its strength and turbulence: cloth, foliage, particles) raised as hard
    // as it rains; RainWindAmount x both
    bool RainWind{ true };
    float RainWindAmount{ 1.f };
    // and gusting round it, so the rain's streaks sway with it (see
    // RainGust; 2026-11-01, the user: "add a slight rain turbulance"): how
    // much, 0 steady as before
    float RainTurbulence{ 0.35f };
    // Water out of the drainpipes while it rains (2026-10-26, the user: "and
    // drainpipes"; see UpdatePipes)
    bool PipeWater{ true };
    // Faith's steps on the wet ground outside, off the puddles (2026-10-26,
    // the user: "make the water footsteps work outside as well when there is
    // no peddles"): 0 as the game has them, 1 the water's, as in the puddles,
    // 2 the game's wet concrete (its sound id 61: concrete with a water layer).
    // Off by default since (2026-10-26, the user: "water footsteps should play
    // only when walking on puddles not always")
    int WetGroundSteps{ 0 };
    // The haze in the rain and the snow (2026-10-26, the user: "Rain/snow
    // should have a haze going on"; see GetHaze): x this, 0 - 5
    float Haze{ 1.f };
  };

  std::mutex g_Mutex;                       // everything here
  Settings g_Settings;

  // Snow rather than rain (VISUALS > Snow): g_Mutex held
  bool SnowChosen()
  {
    return g_Settings.Snow || g_Settings.RandomSnow;
  }
  // The snowy city look: ticked, with Random snow, or in a blizzard (its
  // whiteout fog comes with it) (g_Mutex held)
  bool SnowLookWanted()
  {
    return SnowChosen() && (g_Settings.SnowLook || (g_Settings.RandomSnow && g_Settings.SnowLookWithSnow) ||
      g_Settings.Blizzard);
  }
  // How hard it snows, a blizzard's included (0.1 - 7.5; g_Mutex held). Up
  // to 7.5 since 15:xx (the user: "Slider fog should let you add more fog
  // same with snowfall"; WetLook.cpp's flakes x 1.5). A blizzard (15:xx, the
  // user: "Blizzards should be like in real life where there is a ton of
  // snow going all over the place and its really foggy"): x 1.8, at least 5.
  const float MostSnowIntensity = 7.5f;
  float SnowIntensityNow()
  {
    float i = (std::max)(0.1f, (std::min)(g_Settings.SnowIntensity, MostSnowIntensity));
    return g_Settings.Blizzard ? (std::min)((std::max)(i * 1.8f, 5.f), MostSnowIntensity) : i;
  }
  // With Random rain, thunder and lightning and the rainy city look come with
  // the forecast's rain (2026-10-25 21:5x, the user: "thunder sounds/lightings
  // should also follow with the rain and not just rain", "same with the fog"),
  // as well as when they're ticked themselves. g_Mutex held.
  bool ThunderWanted()
  {
    return g_Settings.Thunder || (g_Settings.RandomRain && g_Settings.ThunderWithRain);
  }
  bool LookWanted()
  {
    return g_Settings.RainyLook || (g_Settings.RandomRain && g_Settings.LookWithRain) || SnowLookWanted();
  }
  // How hard it rains (or snows) now, 0 - 1 (see weather::Update): Rain on:
  // 1 at once; the forecast: following it over RainFadeSeconds. Everything
  // the rain does goes with it.
  float g_Intensity = 0.f;
  bool g_RainOn = false;                    // g_Intensity over nothing: the rain is there
  bool g_SnowOn = false;                    // the same with snow chosen: the snow is there

  // How hard it rains now, 0 - 1 (the intensity x the strength against the
  // Release mission's 0.7, heavy 1.3 x, 1 at 1.5 x that): g_Mutex held
  float RainAmountNow()
  {
    if (!g_RainOn)
      return 0.f;
    float strength = (std::min)(1.5f, g_Settings.Strength / 0.7f * (g_Settings.Heavy ? 1.3f : 1.f));
    return (std::max)(0.f, (std::min)(1.f, g_Intensity) * strength / 1.5f);
  }
  float g_LookAmount = 0.f;                 // how much of the rainy city look is given (0 - 1)
  // The snow (see UpdateSnow): how much has settled (0 - 1), its own clock
  // (s) and how far the wind has carried it (world x, z; m), each wrapping
  // without a jump: the flakes' motions are whole cycles over the clock's
  // period (WetLook.hlsl's WholeOverPeriod), and the drift wraps at a whole
  // number of both flake boxes (16 and 48 m, WetLook.cpp)
  float g_SnowCover = 0.f;
  float g_SnowClock = 0.f;
  float g_SnowDrift[2] = {};
  float g_SnowWind[2] = {};                 // how fast the wind carries it now (m/s)
  float g_SnowTurbulence = 0.8f;            // the flakes' eddies' size (see UpdateSnow)
  // The ice over the water (see weather::GetIce): how far it has formed
  // (0 - 1). It forms over IceFormSeconds while the waterfalls are frozen
  // (they freeze within 6 - 20 s), and goes over IceMeltSeconds once they
  // flow again.
  float g_IceAmount = 0.f;
  const float IceFormSeconds = 20.f;
  const float IceMeltSeconds = 10.f;
  // How far the snow has piled up (0 - 1 of SnowPileMost): once it all lies
  // it deepens over three times SnowSettleMinutes at intensity 1.5 (faster in
  // heavier snow, slower in lighter), and melts as fast as it came
  float g_SnowPile = 0.f;
  // How deep it piles (m). It was the "Piles up to" slider, 0 - 1 m (0.25 by
  // default), until 2026-10-26 21:xx (the user: "pile slider is not working
  // at all with what we want it to do lets remove it or find better ways"):
  // deep, its layer's side climbed walls and boxes as far as 1.5 m (the depth
  // varies x 0.45 - 1.55 by place). 12 cm is enough to press Faith's prints
  // deep, fill the ground's small gaps and lie in a low line against the foot
  // of walls.
  const float SnowPileMost = 0.12f;
  const float SnowClockWrap = 2000.f;
  const float SnowDriftWrap = 9600.f;
  const float RainFadeSeconds = 20.f;
  // The tools' rain sound's level at the Release mission's strength, before
  // the game's own volume (thunder.cpp): a background (0.6 in the 20:34
  // build was too loud: the user, 20:4x; 0.25 with the game's sound over it
  // still "really loud", 21:4x, the game's master volume at 0.17)
  const float RainSoundLevel = 0.12f;
  // The wind's sound's level while it snows (see weather::Update), as the
  // rain's: before the game's own volume, a moderate snow in a light wind
  // about half of it, a blizzard up to 1.6 x more
  const float WindSoundLevel = 0.12f;
  // The game's wind (the state's WindComponentData, + 0x6E0: + 0x08 its
  // WindStrength, + 0x18 its TurbulenceMultiplier) with the rain: its
  // strength at least RainWindLeast + RainWindMore x how hard it rains (8 at
  // full rain: the Snowfall mod's storm has WindStrength 8), its turbulence
  // up to twice the game's, over what the game has after each mix; the
  // game's values back when it stops (see GiveRainWind)
  const int WindStrengthOffset = 0x6E0 + 0x08;
  const int WindTurbulenceOffset = 0x6E0 + 0x18;
  const float RainWindLeast = 1.f, RainWindMore = 7.f;
  // Its gusts (RainTurbulence, 0 - 1): its strength x 1 + turbulence x (0.5,
  // 0.3 and 0.2 of three waves 4.7, 1.9 and 0.83 s long), up to 1 +/- that,
  // and its turbulence x 1 + turbulence more. The game's rain streaks blow
  // with its wind (their emitters' WorldWindData, fx/ambient/rain/emitters/
  // em_evo_rain_streaks_*, read offline 2026-11-01), so they sway; so do
  // cloth, foliage and smoke. Given after each mix of the view, every frame.
  float RainGust(float turbulence)
  {
    if (!(turbulence > 0.f))
      return 1.f;
    const float Turn = 6.2831853f;
    float t = (float)(GetTickCount64() % 3600000ULL) / 1000.f;
    float wave = 0.5f * sinf(t * Turn / 4.7f) + 0.3f * sinf(t * Turn / 1.9f + 1.3f) + 0.2f * sinf(t * Turn / 0.83f + 2.6f);
    return (std::max)(0.f, 1.f + (std::min)(turbulence, 1.f) * wave);
  }
  // One rain sound (2026-10-25 21:4x, the user: "does it play more than one
  // rain sound effect? if yes reduce it to one"). In the 21:39 session three
  // played at once: the tools' loop; the tools' voice of the game's
  // SpotRainOpeningLarge 1.5 m over the camera; and the game's own voice of
  // it, made at the rain column moved near the camera to get it. 60e451e33eff
  // kept only the tools' loop; the user (21:5x): "replace the drawn rain sound
  // effect to the ingame one it's still really loud and doesn't sound like
  // rain". So now the game's own sound is the one (its voice of the tools',
  // at the camera) and the tools' loop isn't played (the code stays).
  const bool UseGameRainVoice = true;
  const bool UseToolsRainLoop = false;
  double g_IntensityMs = 0.0;   // (wetlook::TimeNow, the steady clock)

  // The forecast (Random rain): rain spells over the game's day, worked out
  // from a seed so the same hour always has the same weather until it's made
  // again; each spell 40 - 150 game minutes, together exactly RainChance
  // (SnowChance) of the day, the dry time between them shared out at random
  // (see MakeForecast); the rain rises over the first 10 game minutes of a
  // spell and eases off over its last 10.
  // Each spell its own weather (2026-10-27, the user: "random rain doesn't
  // work properly it should A) rainy city look at random values B) thunder
  // and lighting randomly C) wetness should be random and depending on the
  // amount of rain as well"), drawn with it from the forecast's seed:
  // - Rain: how hard it rains at its height, 0.25 (a drizzle) - 1 (a
  //   downpour): the rain, its sound and wind, the puddles go with it, and how
  //   wet the ground gets (see weather::GetWetting);
  // - Look: how much of the rainy city look at its height, about as much as
  //   it rains give or take, 0.3 - 1; Fog: its fog x 0.6 - 1.5 as thick; Grey:
  //   its grey sky x 0.65 - 1; Sun: its "no sun" x 0.55 - 1;
  // - Storm: thunder and lightning in it or not, the likelier the harder it
  //   rains (15 % of drizzles, 70 % of downpours), from StormFrom to StormTo of
  //   the way through it, a strike Storm x as often as Every (0.5 - 1.5; 0 no
  //   storm).
  // A snow spell (Random snow) keeps its snow at its height (Rain not used).
  struct Spell { float Start, End, Rain, Look, Fog, Grey, Sun, Storm, StormFrom, StormTo; };   // Start, End: seconds of the day; End may be past 86400 (over midnight)
  std::vector<Spell> g_Spells;
  unsigned int g_ForecastSeed = 0x2F6E2B1u;
  float g_ForecastChance = -1.f;            // the chance the spells were made for
  float g_TimeOfDay = -1.f;                 // the game's, seconds (-1: none), read at each update
  float g_LookHours = 12.f;                 // the last known time of day, hours (for the rainy look)
  // How much the camera is inside for the look's fog (g_Inside eased over
  // about LookIndoorsSeconds; see the look's fog below)
  float g_LookIndoors = 0.f;
  ULONGLONG g_LookIndoorsTick = 0;
  const float LookIndoorsSeconds = 1.2f;
  // The main view's fog as given to the game (after the look), for the wet
  // surfaces' pass to fog what it draws the same way (weather::GetFog)
  weather::FogParams g_FogNow{};
  const float SpellRamp = 600.f;

  void MakeForecast(float chance)
  {
    g_Spells.clear();
    g_ForecastChance = chance;
    if (chance <= 0.f)
      return;
    unsigned int state = g_ForecastSeed ? g_ForecastSeed : 1u;
    auto next = [&state]() {
      state ^= state << 13;
      state ^= state >> 17;
      state ^= state << 5;
      return (state & 0xFFFFFF) / 16777215.f;
    };
    // The spells add up to c of the day exactly (the rain or snow falling,
    // its first and last 10 game minutes rising and easing off included):
    // each 40 - 150 game minutes, the last what's left (never under 40, so up
    // to 190). The dry time between them is shared out at random, each gap at
    // least 15 game minutes where there's room, from a random place in the
    // day, round midnight. (2026-11-01, the user: "Also check if how much of
    // the day it snows actually works": it didn't. The gaps were drawn on
    // their own, about c of the day only on average over many days, and the
    // same day came back every day; with the first seed 0.1, 0.2 and 0.3 all
    // gave 33 % of the day, 0.05 gave 25 % and 0.9 74 %, and the hours before
    // the first spell, up to 6, were always dry. Checked offline over 5 seeds
    // x 7 settings: now within 0.03 %; scratchpad forecast\sim2.py.)
    const float Day = 86400.f;
    float c = (std::min)(chance, 0.95f);
    float wet = c * Day;
    std::vector<float> lengths;
    float total = 0.f;
    while (wet - total > 1.f)
    {
      float length = (40.f + 110.f * next()) * 60.f;
      float left = wet - total;
      if (left - length < 40.f * 60.f)
        length = left;
      lengths.push_back(length);
      total += length;
    }
    for (float length : lengths)
    {
      Spell sp{ 0.f, length };
      sp.Rain = 0.25f + 0.75f * next();
      sp.Look = (std::min)(1.f, (std::max)(0.3f, 0.35f + 0.5f * sp.Rain + 0.3f * (next() - 0.5f)));
      sp.Fog = 0.6f + 0.9f * next();
      sp.Grey = 0.65f + 0.35f * next();
      sp.Sun = 0.55f + 0.45f * next();
      bool storm = next() < 0.15f + 0.55f * (sp.Rain - 0.25f) / 0.75f;
      sp.Storm = storm ? 0.5f + next() : 0.f;
      sp.StormFrom = 0.1f + 0.4f * next();
      sp.StormTo = (std::min)(1.f, sp.StormFrom + 0.3f + 0.5f * next());
      g_Spells.push_back(sp);
    }
    // The dry time: each gap a random share of it (on top of the least, 15
    // game minutes, or what there is for each), the spells placed one after
    // another from a random time of day (Start in 0 - 86400, End past it over
    // midnight)
    size_t n = g_Spells.size();
    if (!n)
      return;
    float dry = Day - total;
    std::vector<float> weights(n);
    float weightSum = 0.f;
    for (float& w : weights)
    {
      w = -logf(1.f - 0.999f * next()) + 0.05f;
      weightSum += w;
    }
    float least = (std::min)(15.f * 60.f, dry / (float)n);
    float spare = dry - least * (float)n;
    float t = next() * Day;
    for (size_t i = 0; i < n; ++i)
    {
      Spell& sp = g_Spells[i];
      float length = sp.End;
      sp.Start = t >= Day ? t - Day : t;
      sp.End = sp.Start + length;
      t = sp.End + least + spare * weights[i] / weightSum;
    }
  }

  // How far into its spell the forecast is at a time of day (0 - 1: its
  // first and last 10 game minutes rising and easing off, 1 between); spell:
  // which (-1 none), through: how far through it (0 - 1)
  float ForecastAt(float seconds, int* spell = nullptr, float* through = nullptr)
  {
    if (spell)
      *spell = -1;
    if (seconds < 0.f)
      return 0.f;
    float best = 0.f;
    for (size_t i = 0; i < g_Spells.size(); ++i)
      for (float day : { 0.f, 86400.f })
      {
        Spell const& sp = g_Spells[i];
        float t = seconds + day;
        if (t < sp.Start || t > sp.End)
          continue;
        float here = (std::min)(1.f, (std::min)((t - sp.Start) / SpellRamp, (sp.End - t) / SpellRamp));
        if (here >= best)
        {
          best = here;
          if (spell)
            *spell = (int)i;
          if (through)
            *through = (t - sp.Start) / (std::max)(sp.End - sp.Start, 1.f);
        }
      }
    return best;
  }

  // The spell it rains in (see Spell; the last one's while it's dry, its look
  // easing off with the rain), its storm now (0 - its Storm, eased over
  // StormEaseSeconds), and how wet the forecast's rain has made the ground
  // (see weather::GetWetting and the update)
  Spell g_SpellNow{ 0.f, 0.f, 1.f, 1.f, 1.f, 1.f, 1.f, 0.f, 0.f, 0.f };
  bool g_SpellKnown = false;
  float g_StormTarget = 0.f, g_StormNow = 0.f;
  const float StormEaseSeconds = 10.f;
  float g_GroundWet = 0.f, g_PuddleFill = 0.f;
  const float GroundWetSeconds = 60.f, GroundDrySeconds = 600.f;
  const float PuddleFillSeconds = 180.f, PuddleDrySeconds = 1200.f;

  // The rainy look's values now (g_Mutex held): the settings, with Random
  // rain x its spell's own (see Spell)
  weather::RainyLook LookNow()
  {
    bool spell = g_Settings.RandomRain && !SnowChosen() && g_SpellKnown;
    return { LookWanted(), g_Settings.GreySky * (spell ? g_SpellNow.Grey : 1.f),
      g_Settings.LessSun * (spell ? g_SpellNow.Sun : 1.f), g_Settings.FogSeenUpTo, g_Settings.FogDepth, g_LookHours,
      g_LookIndoors, SnowLookWanted(), g_Settings.Blizzard, g_Settings.SnowFog, SnowIntensityNow(),
      g_Settings.FogThickness * (spell ? g_SpellNow.Fog : 1.f) };
  }

  // The snow, each update (g_Mutex held): while it snows its clock runs and
  // the wind carries it (its way and strength wander slowly, a few slow waves
  // added up, so no two minutes are alike; three times as strong in a
  // blizzard), and it settles: all of it over SnowSettleMinutes at full snow,
  // less in lighter snow. When it doesn't snow it melts, three times slower.
  // Real time, a step at most 0.25 s (a loading screen adds nothing).
  void UpdateSnow(float dt)
  {
    float step = (std::max)(0.f, (std::min)(dt, 0.25f));
    if (g_SnowOn)
    {
      g_SnowClock = fmodf(g_SnowClock + step, SnowClockWrap);
      float t = (float)(GetTickCount64() % 100000000ULL) / 1000.f;
      // Its way wanders over minutes and is swung by the gusts, about 40
      // degrees either way every few seconds (every 9 and 3.7 s); its
      // strength gusts every few seconds (8 and 3.3 s) (2026-10-26, the user:
      // with the wind on the snow came "from only one direction": its way
      // only wandered over 6 - 18 minutes, its gusts over 30 - 70 s)
      const float Turn = 6.2832f;
      float angle = 0.7f + 0.9f * sinf(t / 173.f) + 0.4f * sinf(t / 61.f) +
        0.45f * sinf(t * Turn / 9.f) + 0.25f * sinf(t * Turn / 3.7f + 1.3f);
      // A blizzard (15:xx, the user: "Blizzards should be like in real life
      // where there is a ton of snow going all over the place and its really
      // foggy"): a blizzard is wind of 15.6 m/s (35 mph) or more with snow
      // and a quarter mile or less to see (the US weather service). Its wind
      // three times Wind, at least 12 m/s, gusting from 0.2 to 1.8 x (the
      // snow's own swirl, see the turbulence, does the rest); its flakes'
      // streaks longer (WetLook.cpp), its fog to 120 m (the snowy look).
      bool blizzard = g_Settings.Blizzard;
      float gust = blizzard ? 1.f + 0.5f * sinf(t * Turn / 6.f) + 0.3f * sinf(t * Turn / 2.3f + 1.3f) :
        1.f + 0.35f * sinf(t * Turn / 8.f) + 0.2f * sinf(t * Turn / 3.3f + 1.3f);
      float mean = (std::max)(0.f, g_Settings.SnowWind) * (blizzard ? 3.f : 1.f);
      if (blizzard)
        mean = (std::max)(mean, 12.f);
      float speed = mean * gust;
      // The air's turbulence the flakes swirl in (WetLook.hlsl's SnowVsMain):
      // 0.6 in still air, more as the wind blows; in a blizzard 2.5 and more
      // (the snow going every way in its eddies, up to 5), followed over a few
      // seconds (a sudden change would move every flake at once)
      float turbulence = blizzard ? (std::min)(5.f, 2.5f + 0.15f * mean) : (std::min)(3.f, 0.6f + 0.4f * mean);
      g_SnowTurbulence += (turbulence - g_SnowTurbulence) * (std::min)(1.f, step / 4.f);
      g_SnowWind[0] = speed * cosf(angle);
      g_SnowWind[1] = speed * sinf(angle);
      g_SnowDrift[0] = fmodf(g_SnowDrift[0] + g_SnowWind[0] * step, SnowDriftWrap);
      g_SnowDrift[1] = fmodf(g_SnowDrift[1] + g_SnowWind[1] * step, SnowDriftWrap);
    }
    else
      g_SnowWind[0] = g_SnowWind[1] = 0.f;
    float settle = (std::max)(g_Settings.SnowSettleMinutes, 0.1f) * 60.f;
    if (g_SnowOn)
      g_SnowCover = (std::min)(1.f, g_SnowCover + step / settle * (std::min)(1.f, g_Intensity));
    else
      g_SnowCover = (std::max)(0.f, g_SnowCover - step / (3.f * settle));
    if (g_Settings.FreezeWater && g_SnowOn)
      g_IceAmount = (std::min)(1.f, g_IceAmount + step / IceFormSeconds);
    else
      g_IceAmount = (std::max)(0.f, g_IceAmount - step / IceMeltSeconds);
    if (g_SnowOn && g_SnowCover >= 0.999f)
      g_SnowPile = (std::min)(1.f, g_SnowPile + step / (3.f * settle) * (std::min)(1.f, g_Intensity) * SnowIntensityNow() / 1.5f);
    else if (!g_SnowOn)
      g_SnowPile = (std::max)(0.f, g_SnowPile - step / (3.f * settle));
  }

  // The rain's sound of the game under roofs (see weather::Update)
  ULONGLONG g_RainSoundInsideSince = 0;     // the camera's been under a roof since (0: it isn't)
  const ULONGLONG RainSoundRemakeMs = 5000; // after a stay under a roof this long, a new voice once out
  // Under a roof: 18 m over the camera (at least RainSoundOverRoof over the
  // roof), about -4.3 dB on the sound's curve (the open's -6.5 dB at 30 m),
  // the roof in between so the game muffles it. 21:5x the user asked for it
  // a bit louder indoors (15 m, about -3.7 dB); 22:2x "too loud right now
  // indoors make it less", "about 5% less": 18 m, about 0.6 dB less (the
  // curve's points taken as joined straight; between them not checked).
  const float RainSoundInsideNearest = 18.f;   // (with RainSoundOverRoof below)
  alignas(16) float g_Follow[16];           // the camera's place, axes level (the rain falls down)
  bool g_HaveCamera = false;
  std::atomic<bool> g_Following{ false };   // any rain kept at the camera
  const size_t MaxNotRain = 8192;

  // The game's placed effects (see above)
  const int EffectInstancesOffset = 0x48C0;      // [the manager] +: begin, + 8 end
  const size_t MostEffectInstances = 1u << 20;
  const int InstanceTransformOffset = 0x100;
  const int InstanceFlagsOffset = 0x1B8;
  const BYTE InstanceMovedFlag = 0x02;
  const __int64 InstanceMoveRva = 0x30606F0;
  const int EffectDataComponentsOffset = 0x60;
  const int EmitterDataEmitterOffset = 0xA0;
  const int EmitterDocumentHighOffset = 0x28;
  const int EmitterTemplateNameOffset = 0x158;
  const ULONGLONG InstanceScanMs = 1000;         // looked for this often while one's missing
  typedef void(__fastcall* tInstanceMove)(__int64);
  int g_InstanceCode = 0;                        // 0 not checked, 1 the code the tools know, -1 not
  ULONGLONG g_LastScan = 0;
  ULONGLONG g_LookingSince = 0;                  // the rain wanted and not found since

  // Exclusion volumes (see above)
  const __int64 ExclusionManagerRva = 0x23E4148;
  const __int64 ExclusionSetBoxRva = 0x30FBDE0;
  const __int64 ExclusionEmptyBoxRva = 0x23E41F0;
  const int ExclusionFreeBeginOffset = 0x00;
  const int ExclusionFreeTopOffset = 0x08;
  const int ExclusionFreeEndOffset = 0x10;
  const int ExclusionCountOffset = 0x820;
  const unsigned int ExclusionMostSlots = 0x400;
  typedef void(__fastcall* tSetBox)(__int64, unsigned int, float const*);

  // 2 m squares (4 m before 2026-10-25 18:3x: rain was seen inside along the
  // walls, a square half inside counting as open), boxes padded by half a
  // square into the open ones, up to 64 of them (24 before, and the log showed
  // all 24 in use most of the time, the rest of the roofs left open). The game
  // culls a particle whose sphere touches a box (0x1430F25C5 - 0x1430F2682:
  // the particle in each box's space, clamped to the box, culled while that
  // distance is within its size) and looks at the first 32 boxes near an
  // emitter in slot order: the nearest boxes are in the lowest slots.
  // 2026-10-25 20:xx ("not a single drop of rain inside"): out to 50 m (the
  // game's farthest rain, em_evo_rain_streaks_far, is made in a 100 m box
  // round the camera); a box's top at the HIGHEST roof of its squares, into
  // the roof (it was the lowest: under the higher part of a ceiling rain was
  // left); a square whose roof is now under the camera (the camera went up
  // past it) looked at again and not taken as covered. Each square is also
  // looked at downwards for the first surface under the camera (its floor):
  // the rain the tools draw falls only in squares open to the sky, and only
  // down to that floor.
  const float CellSize = 2.f;
  const int GridHalf = 25;                  // 51 x 51 squares: 50 m each way
  const int GridSize = 2 * GridHalf + 1;
  const float RoofSearch = 60.f;            // how far up a roof is looked for (higher: a bridge or a sign far up, not a roof)
  const float FloorSearch = 60.f;           // how far down a floor is looked for
  const float BoxBelow = 40.f;              // the boxes go this far under the camera
  const float BoxStep = 2.f;                // their bottom moves in these steps
  const float RoofOverlap = 0.5f;           // their top is this far into the highest roof of their squares
  const float CameraClearance = 1.5f;       // and at least this far over the camera (a low ceiling right over it)
  const float BoxPad = 1.f;                 // each way into the squares around (half a square)
  const float RoofMergeHeight = 3.f;        // squares go in one box when their roofs are this close
  const int SweepsPerFrame = 64;            // two a place in a square (up and down)
  // The places in a square looked up and down from (from its middle, in
  // squares): its middle and its quarters' middles. The square takes what
  // most of them found, the middle one of the five (2026-10-27, the user:
  // "snow has a weird 'shadow' square that goes away when i move the camera
  // too close", "snow sometimes flicker": from the middle alone, a ledge, a
  // roof's edge or a sign there counted for the whole square, its street half
  // "under" it, bare, until a sweep from another height found the street;
  // squares on such edges flipped between sweeps). The first time, from its
  // middle alone (as quickly as before), then from all five; while they were
  // alike (within PlacesAlike), from its middle alone again, all five only
  // when that's changed
  const int SquarePlaces = 5;
  const float SquarePlace[SquarePlaces][2] = { { 0.f, 0.f }, { -0.25f, -0.25f }, { 0.25f, -0.25f }, { -0.25f, 0.25f },
    { 0.25f, 0.25f } };
  const float PlacesAlike = 0.5f;           // m
  // Something standing there isn't a roof: a roof found less than this over
  // the floor found under the same place (a car, a van, a bench, a planter,
  // a kiosk's counter: the free camera lower than its top looked up into it)
  // counts as none (2026-11-01, the user: "in the ground level you can see a
  // square while using the freecam", "Cars have a weird square around them
  // with rain": the squares round it were covered, so dry, no puddles, the
  // game's rain kept out of them). A real roof, an awning, a balcony, a
  // bridge, a low ceiling is higher than that over the floor under it.
  const float LowestCover = 2.f;            // m
  const ULONGLONG RefreshMs = 2000;         // a square is looked at again after this
  // or after this past RefreshNear squares from the camera (2026-10-26, the
  // performance: the roofs' sweeps were most of the weather's time on the
  // game thread, 0.2 - 0.4 ms a frame)
  const ULONGLONG FarRefreshMs = 5000;
  const int RefreshNear = 10;
  const float RefreshHeight = 3.f;          // or once the camera's height changed this much
  const size_t MaxBoxes = 64;               // the game looks at 32 at most per emitter, the nearest first
  // The boxes' sides where they meet the open sky (2026-10-26, the user:
  // "rain is still blocky around some meshes": each side went BoxPad into
  // the squares round it, so the game's rain stopped up to 2 m out from
  // under a roof, in straight lines along the 2 m squares): from a roofed
  // square's middle towards an open one beside it, how far its roof goes,
  // looked for with EdgeSweeps sweeps up from the camera's height, each
  // halving what's left (to CellSize / 8, 25 cm); the side EdgePad past the
  // first point found open, never farther than BoxPad. Within EdgeReach
  // squares of the camera (the game's rain falls round it), at most
  // EdgeSweepsPerFrame a frame (and within SweepsPerFrame with the
  // squares'), looked for again after FarRefreshMs or when the camera's
  // height or that roof changed. A named interior's top (no roof found)
  // keeps BoxPad.
  const int EdgeSweeps = 3;
  const float EdgePad = 0.25f;
  const int EdgeReach = 12;
  const int EdgeSweepsPerFrame = 12;

  struct Cell
  {
    bool Covered;
    float Roof;     // covered: the roof's height
    float Floor;    // the first surface under the camera (FromY - FloorSearch: none found)
    float FromY;
    ULONGLONG Tick;
    float InteriorTop;   // in one of the game's named interiors at FromY: its top (1e9 none; see NamedInteriorTop)
    int Places;     // how many places it was looked at from (1 its middle alone, or SquarePlaces)
    bool Even;      // all its places found alike (see SquarePlaces)
  };
  std::unordered_map<long long, Cell> g_Cells;
  // A roofed square's side towards an open one (see EdgeSweeps): from its
  // middle, its roof found over Covered metres out, the open sky at Open
  struct Edge
  {
    float Covered, Open;
    float FromY;    // the camera's height then
    float Roof;     // that square's roof then
    ULONGLONG Tick;
  };
  std::unordered_map<long long, Edge> g_Edges;   // game thread, under g_Mutex (see EdgeKey)

  struct Box
  {
    float Min[3], Max[3];
    bool operator==(Box const& o) const { return !memcmp(this, &o, sizeof(Box)); }
  };
  // The wet surfaces' settings as set in the menu (see wetlook)
  wetlook::Settings g_Wet{ false, 0.8f, 0.35f, 2.5f, 0.8f, true, 200.f };
  __int64 g_SlotsManager = 0;               // the manager g_Slots are in
  std::vector<unsigned int> g_Slots;        // the tools' own slots
  std::vector<Box> g_DryBoxes;       // the boxes as last worked out (the wet surfaces stay dry in them)
  std::vector<Box> g_Applied;               // the box each slot has (g_Slots order); fewer: the rest empty
  bool g_SlotsFull = false;
  int g_CodeState = 0;                      // 0 not checked, 1 the code the tools know, -1 not
  int g_LastBoxCount = -1;
  ULONGLONG g_BoxLogTick = 0;
  bool g_CameraCovered = false;             // the camera's own square is under a roof
  float g_Inside = 0.f;                     // how much the camera is inside, 0 - 1 (see UpdateRoofs)
  // The camera in the lair with its windows shut (see InShutLair): no wind
  // there, its sound eased out over LairCalmSeconds (g_LairCalm 0 - 1) and
  // the game's wind not raised with the rain (AfterViewMix)
  bool g_InShutLair = false;
  float g_LairCalm = 0.f;
  double g_LairCalmMs = 0.0;
  const float LairCalmSeconds = 1.f;
  float g_CameraRoofUp = 0.f;               // how far the roof over the camera is above it (0: none)
  float g_ListenerRight[3] = { 1.f, 0.f, 0.f }, g_ListenerBack[3] = { 0.f, 0.f, 1.f };   // the camera's axes
  bool g_RoofsKnown = false;                // the squares round the camera are being looked at
  int g_RainGridOrigin[2] = {};             // the grid's first square (x, z), CellSize each
  std::vector<float> g_RainGrid;            // per square: 1 open to the sky (0 covered or not known yet), its floor
  // The same squares for the wet surfaces and the lightning (2026-10-25
  // 22:xx): per square 1 looked at (0 not yet), the first surface under where
  // it was looked from (-1e9: none within FloorSearch), the first roof over it
  // (1e9: none within RoofSearch), where it was looked from. Kept as found,
  // whatever the camera's height now: the wet surfaces' pass works out per
  // pixel whether something is over it (before, it went by the rain's boxes,
  // which are rebuilt as the camera moves and are 2 m squares merged, and
  // whether a roof was over the camera's own height: the dry and wet patches
  // jumped about and had saw-toothed edges, the user's 22:0x screenshot)
  std::vector<float> g_ShelterGrid;
  // Per square of g_ShelterGrid and side (0 + x, 1 - x, 2 + z, 3 - z): where
  // the open sky begins out from its middle, as the edges' sweeps found it
  // (see EdgeSweeps; m), -1 not known or no edge found within a square: where
  // the roofs' drips fall (WetLook.cpp's MakeDrips)
  std::vector<float> g_DripEdges;

  inline long long CellKey(int x, int z) { return ((long long)x << 32) ^ (unsigned int)z; }
  // A square's side (0 +x, 1 -x, 2 +z, 3 -z): x over bit 34, z's 32 bits from bit 2, the side under
  inline long long EdgeKey(int x, int z, int side) { return ((long long)x << 34) ^ ((long long)(unsigned int)z << 2) ^ side; }

  // The rain's sound (see above), under g_Mutex
  const char* RainSoundName = "ME_Sound/World/Shared/Rain/SpotRainOpeningLarge";
  const __int64 VoiceManagerRva = 0x23E29B8;     // [ ]: the sound system's voices
  const int VoiceReleaseSlot = 9;
  const int MaxRainSoundEvents = 4;
  const ULONGLONG RainSoundEventsMs = 2000;      // the game's first events, this long after it made the voice
  // The game's sound event ids are the djb2-xor hashes of their names (as
  // its parameter handles are): 0x0DC0EFA5 is "Start", 0x7C89BB7D "Stop"
  // (worked out 2026-10-26, and what its sound entities were seen to start
  // and stop with). The game stops its rain voice as soon as the rain column
  // moved over the camera for it goes back. The 01:25 test caught only
  // Start, Stop (the 00:15 one Start, Stop, Start, Start), so the tools'
  // voice was started and stopped at once: no rain heard (the user,
  // 2026-10-26: "Rain sounds are missing now"). The tools' voice is given the
  // game's events without its Stops, and a Start if there was none.
  const unsigned int SoundEventStart = 0x0DC0EFA5;
  const unsigned int SoundEventStop = 0x7C89BB7D;
  // Over the camera in the open. The sound's own fall-off (its
  // ConeOutputNodeData's AttenuationCurve: -1.0 dB at 0.59 m, -10.9 dB at
  // 54.3 m, -34.5 dB at 98.1 m; Loudness 70) made it nearly at its full level
  // at 1.5 m, a stream of rain right on the listener: at 30 m about -6.5 dB
  // (the curve's points joined straight; between them not checked), and under
  // a roof nearer, see RainSoundInsideNearest.
  const float RainSoundHeight = 30.f;
  struct RainSound
  {
    __int64 Asset, AssetVTable;
    __int64 GameVoice;
    ULONGLONG MadeTick;
    unsigned int Events[MaxRainSoundEvents];
    int EventCount;
    __int64 OwnVoice;
    __int64 StaleVoice;   // the tools' voice for an asset made again since, to let go
  };
  RainSound g_RainSound{};
  std::atomic<__int64> g_WatchedVoice{ 0 };      // the game's rain voice whose first events are taken
  // A world of the game's going away (2026-11-02, CT.log 21:47:09 - 21:47:14,
  // the user: "Find out why the game crashed"): a replay of Release ended and
  // the game streamed Release's world out; the rain sound asset the tools'
  // voice was made from had come with it (21:40:35, as Release's bundle came
  // in). 5 s later, out from under a roof, the tools let their voice go and
  // made a new one from that asset, and the game crashed in its sound code
  // (0x143022492, reading 0) with a register inside the asset. Its first
  // bytes and its name read as they were, so RainSoundAssetThere passed: what
  // a world leaves behind can still look the same. Now, told a world is
  // going (WorldGoing: a mission's sub-level streamed out, by the game or the
  // replay), at a loading screen's start, or when a rain of the game's is
  // gone (TrackInstances), the tools' voice is let go and the asset forgotten
  // at the next update, and none is taken or made for RainSoundQuietMs, while
  // that world goes; then one the game makes again is taken, as after a death.
  std::atomic<char const*> g_WorldGoing{ nullptr };   // any thread: why (a literal), taken by Update
  bool g_RainZoneGone = false;                        // under g_Mutex, game thread: a rain instance gone this update
  bool g_WasLoading = false;                          // game thread
  ULONGLONG g_RainSoundQuietUntil = 0;                // under g_Mutex
  const ULONGLONG RainSoundQuietMs = 15000;
  std::unordered_map<__int64, __int64> g_NotRainSound;   // asset -> its name pointer, seen and not the rain's sound
  // The rain columns (their sound is the rain's): when the game hasn't made
  // one's voice yet, one is moved under the camera for a moment so it does
  const char* RainColumnEmitter = "Em_Ambient_Rain_Opening_Large_VertexAnim";
  struct RainColumn { __int64 Instance; __int64 Data; alignas(16) float Home[16]; };
  std::vector<RainColumn> g_Columns;
  const size_t MaxColumns = 8;
  ULONGLONG g_ColumnsSeenTick = 0;
  __int64 g_ColumnWoken = 0;                      // the one moved (its instance), 0 none
  ULONGLONG g_ColumnWokenTick = 0;
  int g_ColumnWakes = 0;
  const ULONGLONG ColumnWaitMs = 5000;            // after the columns start, before one is woken
  const ULONGLONG ColumnWakeMs = 3000;            // how long it's kept there
  // Where: over a square open to the sky (no roof over it up to 60 m over the
  // camera, its floor the first surface under the camera), 15 - 30 m from the
  // camera and within 47 m of it (its CullDistance is 50 m), placed 30 m over
  // that floor (its drops, made in a 6 x 5 x 3 m box, live 0.9 s at 25 m/s:
  // about 23 m of fall, so they end over the floor) and never under the
  // camera's height. The column's emitter (Em_Ambient_Rain_Opening_Large_
  // VertexAnim) isn't culled by exclusion volumes (no ExclusionVolumeCullEnable),
  // so it must never be where it could fall indoors (before 2026-10-25 20:xx:
  // 40 m under the camera, which could be a floor of the building under it).
  const float ColumnWakeNearest = 15.f, ColumnWakeFarthest = 30.f, ColumnWakeOverFloor = 30.f, ColumnWakeReach = 47.f;
  alignas(16) float g_ColumnWakeRows[16];
  const int MostColumnWakes = 3;
  const float RainSoundOpeningHeight = 2.f;       // under a roof: the rain's sound this far over the camera, at the opening
  const float RainSoundOverRoof = 3.f;            // or this far over the roof when there's none

  // Thunder and lightning, under g_Mutex
  ULONGLONG g_NextStrike = 0;
  bool g_StrikeNow = false;
  // Whether lightning could come at the last update (its storm), and whether
  // the camera hook asked weather::ShardTopWanted since, with lightning able
  // to come: the Shard's tip is only looked for then, so a burst waits for an
  // update it was asked before (a storm begun, or Strike now pressed, after
  // the camera hook asked: the next frame)
  bool g_StormWas = false;
  bool g_ShardTopAsked = false;
  // While the game is paused or its loading screen is up (gamestate, read
  // each update): since when (0: it isn't). Nothing of the weather goes on
  // meanwhile, as the game's own world doesn't (2026-10-26, the user: "find
  // any new bugs or inconsistences"): no strike starts and what's due waits
  // as long as the pause lasted (its thunder waits too, Thunder.cpp), the
  // forecast's rain doesn't ease in or out, the snow's clock, wind and
  // settling stand still.
  ULONGLONG g_HeldSince = 0;
  unsigned int g_WeatherRandom = 0x2545F491u;
  float Chance()
  {
    g_WeatherRandom ^= g_WeatherRandom << 13;
    g_WeatherRandom ^= g_WeatherRandom >> 17;
    g_WeatherRandom ^= g_WeatherRandom << 5;
    return (g_WeatherRandom & 0xFFFFFF) / 16777215.f;
  }
  // A flash's pulses: when (s), how bright, how fast they fade (s)
  const float FlashPulses[3][3] = { { 0.f, 1.f, 0.12f }, { 0.16f, 0.6f, 0.1f }, { 0.34f, 0.85f, 0.22f } };
  float FlashPulse(float t)
  {
    float level = 0.f;
    for (auto const& p : FlashPulses)
      if (t >= p[0])
        level = (std::max)(level, p[1] * expf(-(t - p[0]) / p[2]));
    return level;
  }

  // The lightning's bolt (see weather::GetBolt), under g_Mutex. The game has
  // no lightning in its sky: its only lightning effects are the Crippler
  // weapon's arcs (the KrugerSec Protectors') and a spark tower's (fx/weapons/crippler/
  // emitters/em_crippler_*_lightning, fx/destruction/spark/emitters/
  // em_destruction_sparktower_lightningchild; 2026-10-25, the game's file
  // list), all a few metres long. So the bolt is made here: a channel from
  // the clouds down to the ground at the strike, wandering, with a few
  // branches, drawn by the wet surfaces' pass while the flash lasts.
  const int MostBoltSegments = 64;              // a bolt's
  const float BoltSeconds = 0.75f;               // drawn this long after the strike, with the flash's pulses
  const float FlashSeconds = 1.5f;               // a strike's flash, all its pulses
  struct BoltSegment { float A[3], B[3]; float Width, Bright; };

  // Bursts (2026-10-25 22:4x, the user: "spawn more than 1 up to 6 lighting
  // strike and randomize between the numbers"): each time lightning strikes
  // it's 1 - 6 strikes (as many as chance says), the first at once and the
  // others after gaps of 1.5 - 6 s, each at its own distance and way, with
  // its own flash, bolt and thunder, the thunder its distance / 343 m/s after
  // its flash. (23:0x the user: "the lighting strikes should be in intervals
  // between each other and the sound should come a bit later than the
  // lighting strike like a real one": the gaps were 0.12 - 1.2 s, so a flash
  // came with the last one's thunder, and a strike on a tall top could be
  // 30 m away, its thunder at once; now never nearer than Nearest.)
  const int MostStrikes = 6;
  // Lower than any of the city's ground (its streets are at about y 0 - 70,
  // Faith's own places in the logs; the sea lower still): a bolt that finds
  // no top goes down to here
  const float StrikeBelowGround = -150.f;
  struct Strike
  {
    ULONGLONG Tick;                // when it strikes
    bool Started;                  // its bolt made, its thunder sent off
    float Distance, DirX, DirZ;    // from the camera (its distance: where its bolt comes down, once started)
    float Strength;                // its flash's
    std::vector<BoltSegment> Bolt; // empty: no bolt drawn
    bool OnShard;                  // it comes down on the Shard's tip (Target)
    float Target[3];
  };
  std::vector<Strike> g_Strikes;   // the bursts' strikes to come and those still flashing

  // The Shard (2026-10-25 23:5x, the user: "also make it possible for one of
  // the lighting strikes to hit the shard"): its tip in the world as it's
  // drawn now (the far-away model from the city, the real tower out of it),
  // given by the Shard's own code in the camera hook (weather::SetShardTop);
  // not known: no strike on it. Now and then (ShardStrikeChance of the
  // bursts) one of a burst's strikes goes for it, when it's within
  // ShardStrikeAngle of where the camera looks (so its bolt is seen).
  bool g_ShardTopKnown = false;
  float g_ShardTop[3] = {};
  const float ShardStrikeChance = 0.35f;
  const float ShardStrikeAngle = 35.f;   // degrees

  // Which way a strike is from the camera, level (x, z): where the camera
  // looks (within about 26 degrees) when inView, anywhere around it otherwise
  void StrikeDirection(float const* camera, bool inView, float& dx, float& dz)
  {
    float fx = -camera[8], fz = -camera[10];   // where the camera looks, level
    float length = sqrtf(fx * fx + fz * fz);
    if (length > 1e-3f)
    {
      fx /= length;
      fz /= length;
    }
    else
    {
      fx = 0.f;
      fz = 1.f;
    }
    float turn = inView ? (Chance() - 0.5f) * 0.9f : Chance() * 6.2831853f;
    dx = fx * cosf(turn) - fz * sinf(turn);
    dz = fx * sinf(turn) + fz * cosf(turn);
  }

  // Lightning goes for what's tallest (2026-10-25 22:5x, the user: "lighting
  // bolts should hit really toll buildings antenas"): round where a strike
  // would come down (along its way 0.6 - 1.4 x its distance, a third of it to
  // each side, 20 places) the tops are looked for straight down from 800 m
  // over the camera with the camera collision's sweep (as the roofs are);
  // the highest is looked at closer (25 places 3 m apart) for its very top
  // (an antenna, a spire) and the bolt ends there. The game has the city's
  // collision only a few hundred metres round the camera: where none is
  // found, false (the bolt comes down where it was going to). Thin antennas
  // can still be missed between the places looked at.
  bool FindStrikeTop(float const* camera, float dx, float dz, float distance, float nearest, float* top)
  {
    if (!collision::Available())
      return false;
    // Down to below the city's ground whatever the camera's height (a camera
    // high over the city missed the ground under it)
    float fromY = camera[13] + 800.f, toY = (std::min)(camera[13] - 300.f, StrikeBelowGround);
    auto down = [&](float x, float z, float& y)
    {
      float from[3] = { x, fromY, z }, to[3] = { x, toY, z };
      float fraction = 1.f, normal[3] = {};
      if (!collision::Sweep(from, to, 0.f, &fraction, normal))
        return false;
      y = fromY + (toY - fromY) * fraction;
      return true;
    };
    float sideX = -dz, sideZ = dx;
    float best = -1e9f, bestX = 0.f, bestZ = 0.f;
    for (int a = 0; a < 5; ++a)
      for (int b = 0; b < 4; ++b)
      {
        // Never nearer than the menu's nearest strike (its thunder comes that much later)
        float along = (std::max)(distance * (0.6f + 0.2f * a), nearest), side = distance * (-0.33f + 0.22f * b);
        float x = camera[12] + dx * along + sideX * side, z = camera[14] + dz * along + sideZ * side, y = 0.f;
        if (down(x, z, y) && y > best)
        {
          best = y;
          bestX = x;
          bestZ = z;
        }
      }
    if (best < -1e8f)
      return false;
    float topX = bestX, topZ = bestZ;
    for (int a = -2; a <= 2; ++a)
      for (int b = -2; b <= 2; ++b)
      {
        float x = bestX + a * 3.f, z = bestZ + b * 3.f, y = 0.f;
        if ((a || b) && down(x, z, y) && y > best)
        {
          best = y;
          topX = x;
          topZ = z;
        }
      }
    top[0] = topX;
    top[1] = best;
    top[2] = topZ;
    return true;
  }

  // Cranes (2026-10-26, the user: "Lighting strikes should also hit cranes"):
  // the city's construction cranes (objects/props/construction/crane01/:
  // bottom, stand, top, four arm sections, hook, ...; crane02/: base, mid,
  // top and its parts, control room) have collision of their own (their
  // *_physics_win32 assets; the far-off backdrop ones, crane02_bd_*, have
  // none, so the sweep finds nothing on them and they aren't struck) but are
  // thin, and FindStrikeTop's places are tens of metres apart. So they're
  // looked for by name among the mesh streaming manager's model instances
  // (OFFSET_MESHSTREAMING; its mesh table at + 0x50 / + 0x58, records 0x350,
  // the mesh at + 0 and its name at [[mesh + 0x20] + 0x58]; its instances at
  // + 0x350070 / + 0x350078, 32 bytes: bounding sphere radius f32 + 0, mesh
  // number u32 + 4, middle x y z f32 + 0x10: as the streaming watch in
  // CameraManager.cpp has them, the names read live before), kept for
  // CraneRefreshMs. A
  // sphere's own top would be in the air over a crane's arm, so each one's
  // real top is found with the collision sweep straight down over a grid
  // 2 m apart round its middle, a hit counting only inside its sphere.
  struct CraneSpot { float X, Y, Z, Radius; };
  std::vector<CraneSpot> g_Cranes;
  ULONGLONG g_CranesTick = 0;
  const ULONGLONG CraneRefreshMs = 10000;
  const int CraneGridHalf = 4;      // 9 x 9 places, 2 m apart, round each crane part
  const int MostCraneLooks = 3;     // crane parts looked at a strike, the highest first

  // FindCranes' reads of the game's memory (no C++ objects here, for __try):
  // the mesh streaming manager's mesh table and instance list
  bool ReadMeshLists(__int64 pp, __int64& tb, __int64& te, __int64& ib, __int64& ie)
  {
    if (!pp)
      return false;
    __try
    {
      __int64 m = *(__int64 const*)pp;
      if (m < 0x10000)
        return false;
      tb = *(__int64 const*)(m + 0x50);
      te = *(__int64 const*)(m + 0x58);
      ib = *(__int64 const*)(m + 0x350070);
      ie = *(__int64 const*)(m + 0x350078);
      return tb >= 0x10000 && te > tb && ib >= 0x10000 && ie > ib;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      return false;
    }
  }

  // A mesh's name, lower case (false if it can't be read)
  bool ReadMeshName(__int64 tb, size_t k, char* low, size_t size)
  {
    __try
    {
      __int64 asset = *(__int64 const*)(tb + (__int64)k * 0x350);
      if (asset < 0x10000)
        return false;
      __int64 res = *(__int64 const*)(asset + 0x20);
      if (res < 0x10000)
        return false;
      char const* name = *(char const* const*)(res + 0x58);
      if ((__int64)name < 0x10000)
        return false;
      size_t n = 0;
      for (; n + 1 < size && name[n]; ++n)
        low[n] = (char)tolower((unsigned char)name[n]);
      low[n] = 0;
      return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      return false;
    }
  }

  // An instance: its sphere's radius, its mesh's number and its middle
  bool ReadMeshInstance(__int64 ib, size_t i, float& radius, unsigned int& mesh, float* middle)
  {
    __try
    {
      BYTE const* r = (BYTE const*)(ib + (__int64)i * 32);
      radius = *(float const*)r;
      mesh = *(unsigned int const*)(r + 4);
      memcpy(middle, r + 0x10, 12);
      return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      return false;
    }
  }

  // Under g_Mutex, game thread: the crane parts loaded now (see CraneSpot)
  void FindCranes()
  {
    g_Cranes.clear();
    __int64 tb = 0, te = 0, ib = 0, ie = 0;
    if (!ReadMeshLists(util::offsets::GetOffset("OFFSET_MESHSTREAMING"), tb, te, ib, ie))
      return;
    size_t meshes = (std::min)((size_t)((te - tb) / 0x350), (size_t)65536);
    std::vector<unsigned char> crane(meshes, 0);
    size_t craneMeshes = 0;
    char low[192];
    for (size_t k = 0; k < meshes; ++k)
      if (ReadMeshName(tb, k, low, sizeof(low)) &&
          (strstr(low, "construction/crane01/crane01_") || strstr(low, "construction/crane02/crane02_")))
      {
        crane[k] = 1;
        ++craneMeshes;
      }
    if (!craneMeshes)
      return;
    size_t count = (std::min)((size_t)((ie - ib) / 32), (size_t)(4u << 20));
    for (size_t i = 0; i < count; ++i)
    {
      float radius = 0.f, c[3];
      unsigned int mesh = 0;
      if (!ReadMeshInstance(ib, i, radius, mesh, c))
        break;
      if (!(radius > 0.f) || mesh >= meshes || !crane[mesh] || !std::isfinite(c[0] + c[1] + c[2]))
        continue;
      g_Cranes.push_back({ c[0], c[1], c[2], radius });
    }
  }

  // Under g_Mutex, game thread: the top of a crane round where a strike would
  // come down (the same area as FindStrikeTop's), never nearer than nearest
  bool FindCraneTop(float const* camera, float dx, float dz, float distance, float nearest, float* top)
  {
    if (!collision::Available())
      return false;
    ULONGLONG now = GetTickCount64();
    if (!g_CranesTick || now - g_CranesTick > CraneRefreshMs)
    {
      g_CranesTick = now;
      size_t before = g_Cranes.size();
      FindCranes();
      if (g_Cranes.size() != before)
        util::log::Write("Weather: %d crane parts loaded for lightning to strike", (int)g_Cranes.size());
    }
    if (g_Cranes.empty())
      return false;
    // The crane parts in the area, the highest (by their spheres) first
    std::vector<CraneSpot const*> inArea;
    for (CraneSpot const& c : g_Cranes)
    {
      float x = c.X - camera[12], z = c.Z - camera[14];
      float along = x * dx + z * dz, side = -x * dz + z * dx;
      float flat = sqrtf(x * x + z * z);
      if (flat >= nearest && along >= distance * 0.6f - c.Radius && along <= distance * 1.4f + c.Radius &&
          fabsf(side) <= distance * 0.33f + c.Radius)
        inArea.push_back(&c);
    }
    std::sort(inArea.begin(), inArea.end(), [](CraneSpot const* a, CraneSpot const* b) { return a->Y + a->Radius > b->Y + b->Radius; });
    float best = -1e9f;
    for (size_t k = 0; k < inArea.size() && k < (size_t)MostCraneLooks; ++k)
    {
      CraneSpot const& c = *inArea[k];
      float fromY = c.Y + c.Radius + 20.f, toY = c.Y - c.Radius - 5.f;
      for (int a = -CraneGridHalf; a <= CraneGridHalf; ++a)
        for (int b = -CraneGridHalf; b <= CraneGridHalf; ++b)
        {
          float x = c.X + a * 2.f, z = c.Z + b * 2.f;
          float from[3] = { x, fromY, z }, to[3] = { x, toY, z };
          float fraction = 1.f, normal[3] = {};
          if (!collision::Sweep(from, to, 0.f, &fraction, normal))
            continue;
          float y = fromY + (toY - fromY) * fraction;
          // Inside this part's sphere only (not the ground or a building next to it)
          float ox = x - c.X, oy = y - c.Y, oz = z - c.Z;
          if (ox * ox + oy * oy + oz * oz > (c.Radius + 1.f) * (c.Radius + 1.f) || y <= best)
            continue;
          best = y;
          top[0] = x;
          top[1] = y;
          top[2] = z;
        }
    }
    return best > -1e8f;
  }

  // A strike's bolt, from the clouds down to ground (world), its side the way
  // dx, dz from the camera
  void MakeBolt(float const* camera, float const* ground, float dx, float dz, std::vector<BoltSegment>& bolt)
  {
    bolt.clear();
    float topY = (std::max)(camera[13], ground[1]) + 650.f + 350.f * Chance();
    float side = (Chance() - 0.5f) * 0.3f * (topY - ground[1]);
    float top[3] = { ground[0] - dz * side, topY, ground[2] + dx * side };

    // The main channel: down in steps, wandering about the straight line
    const int steps = 36;
    float stepY = (top[1] - ground[1]) / steps;
    std::vector<std::array<float, 3>> points;
    points.push_back({ top[0], top[1], top[2] });
    float wx = 0.f, wz = 0.f;
    for (int i = 1; i <= steps; ++i)
    {
      float f = (float)i / steps;
      wx = wx * 0.6f + (Chance() - 0.5f) * stepY * 1.6f;
      wz = wz * 0.6f + (Chance() - 0.5f) * stepY * 1.6f;
      std::array<float, 3> p = { top[0] + (ground[0] - top[0]) * f + wx, top[1] - stepY * i, top[2] + (ground[2] - top[2]) * f + wz };
      if (i == steps)
        p = { ground[0], ground[1], ground[2] };
      points.push_back(p);
    }
    for (int i = 0; i < steps; ++i)
    {
      BoltSegment s{};
      memcpy(s.A, points[i].data(), sizeof(s.A));
      memcpy(s.B, points[i + 1].data(), sizeof(s.B));
      s.Width = 3.f;
      s.Bright = 1.f;
      bolt.push_back(s);
    }
    // Branches off its upper part, sideways and down, thinner and fading
    int branches = 2 + (int)(Chance() * 3.f);
    for (int b = 0; b < branches; ++b)
    {
      int from = 3 + (int)(Chance() * steps * 0.55f);
      std::array<float, 3> p = points[from];
      float sx = (Chance() - 0.5f) * 2.f, sz = (Chance() - 0.5f) * 2.f;
      int length = 5 + (int)(Chance() * 5.f);
      for (int i = 0; i < length && (int)bolt.size() < MostBoltSegments; ++i)
      {
        std::array<float, 3> q = { p[0] + (sx + (Chance() - 0.5f) * 1.5f) * stepY * 0.8f, p[1] - stepY * (0.5f + 0.5f * Chance()),
          p[2] + (sz + (Chance() - 0.5f) * 1.5f) * stepY * 0.8f };
        BoltSegment s{};
        memcpy(s.A, p.data(), sizeof(s.A));
        memcpy(s.B, q.data(), sizeof(s.B));
        s.Width = 1.4f;
        s.Bright = 0.55f * (1.f - (float)i / length);
        bolt.push_back(s);
        p = q;
      }
    }
    if ((int)bolt.size() > MostBoltSegments)
      bolt.resize(MostBoltSegments);
  }

  // Under g_Mutex: a burst of 1 - MostStrikes strikes from now (see Strike);
  // the first firstDistance away when that's given (>= 0), the others (and
  // the first otherwise) at random distances between the menu's nearest and
  // farthest, nearer ones less likely
  // Returns how long the burst lasts (seconds, from now to its last strike)
  float StartBurst(float const* camera, ULONGLONG now, float firstDistance)
  {
    for (auto it = g_Strikes.begin(); it != g_Strikes.end();)
      it = it->Started && now - it->Tick > (ULONGLONG)(FlashSeconds * 1000.f) ? g_Strikes.erase(it) : std::next(it);
    // A new burst takes over from the strikes still to come of an older one
    // (2026-10-26 00:14:51 in the user's log: Strike now pressed during a
    // burst of 6 over 21 s found the list full, so nothing struck, and the log
    // still said "a burst of 6 strikes")
    for (auto it = g_Strikes.begin(); it != g_Strikes.end();)
      it = !it->Started ? g_Strikes.erase(it) : std::next(it);
    float lo = (std::min)(g_Settings.NearestStrike, g_Settings.FarthestStrike);
    float hi = (std::max)(g_Settings.NearestStrike, g_Settings.FarthestStrike);
    int count = (std::min)(MostStrikes, 1 + (int)(Chance() * MostStrikes));
    ULONGLONG at = now;
    size_t made = 0;   // added to g_Strikes (fewer than count when it's full)
    for (int i = 0; i < count && g_Strikes.size() < (size_t)(2 * MostStrikes); ++i)
    {
      if (i > 0)
        at += (ULONGLONG)(1000.f * (1.5f + 4.5f * Chance()));
      Strike st{};
      st.Tick = at;
      st.Distance = i == 0 && firstDistance >= 0.f ? firstDistance : lo + (hi - lo) * Chance() * Chance();
      StrikeDirection(camera, g_Settings.Bolts, st.DirX, st.DirZ);
      g_Strikes.push_back(std::move(st));
      ++made;
    }
    // One of them on the Shard now and then (see g_ShardTopKnown)
    if (g_ShardTopKnown && made > 0 && Chance() < ShardStrikeChance)
    {
      float x = g_ShardTop[0] - camera[12], z = g_ShardTop[2] - camera[14];
      float flat = sqrtf(x * x + z * z);
      float fx = -camera[8], fz = -camera[10];
      float look = sqrtf(fx * fx + fz * fz);
      if (flat > 1.f && look > 1e-3f && (x * fx + z * fz) / (flat * look) >= cosf(ShardStrikeAngle * 0.01745329f))
      {
        Strike& st = g_Strikes[g_Strikes.size() - made + (size_t)(Chance() * made) % made];
        st.OnShard = true;
        memcpy(st.Target, g_ShardTop, sizeof(st.Target));
        st.DirX = x / flat;
        st.DirZ = z / flat;
        st.Distance = flat;
      }
    }
    util::log::Write("Weather: lightning: a burst of %d strike%s over %.1f s", (int)made, made == 1 ? "" : "s",
      (at - now) / 1000.f);
    return (at - now) / 1000.f;
  }

  // The rain's strength (see weather::AfterViewMix), game thread only but
  // the atomics (the menu shows them)
  const int VeViewStateOffset = 0x550;                       // the view's state (its constructor points it at + 0x560)
  const int VeStateRainParamOffset = 0x12A0 + 2 * 0x1C + 8;  // EmitterParam slot 2's Value: EmitterParameter3
  struct RainParam
  {
    bool Applied;
    __int64 State;      // where it was written
    float Base;         // the game's value under it
    float Written;
  };
  RainParam g_RainParam{};
  std::atomic<int> g_RainParamCode{ 0 };                     // 0 not checked, 1 the code the tools know, -1 not
  std::atomic<bool> g_RainParamShown{ false };               // given to the game now
  std::atomic<float> g_RainParamGameValue{ 0.f };            // what the game had under it

  bool ReadName(__int64 asset, char* out, size_t size, __int64& namePointer)
  {
    __try
    {
      char const* name = *(char const* const*)(asset + AssetNameOffset);
      namePointer = (__int64)name;
      if ((__int64)name < 0x10000)
        return false;
      size_t i = 0;
      for (; i + 1 < size && name[i]; ++i)
        out[i] = name[i];
      out[i] = 0;
      return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      return false;
    }
  }

  __int64 ReadPointer(__int64 at);
  bool CopyRows(void const* from, float* to);

  // An effect instance's first emitter's DebugName (see above); false when it
  // has none that can be read
  bool ReadInstanceEmitter(__int64 instance, __int64& data, char* out, size_t size)
  {
    __try
    {
      data = *(__int64 const*)instance;
      if (data < 0x10000)
        return false;
      __int64 components = *(__int64 const*)(data + EffectDataComponentsOffset);
      if (components < 0x10000)
        return false;
      unsigned int count = *(unsigned int const*)(components - 4);
      if (!count || count > 64)
        return false;
      __int64 emitter = *(__int64 const*)components;
      if (emitter < 0x10000)
        return false;
      __int64 document = *(__int64 const*)(emitter + EmitterDataEmitterOffset);
      if (document < 0x10000)
        return false;
      __int64 high = *(__int64 const*)(document + EmitterDocumentHighOffset);
      if (high < 0x10000)
        return false;
      char const* name = *(char const* const*)(high + EmitterTemplateNameOffset);
      if ((__int64)name < 0x10000)
        return false;
      size_t i = 0;
      for (; i + 1 < size && name[i]; ++i)
        out[i] = name[i];
      out[i] = 0;
      return i > 0;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      return false;
    }
  }

  // The client effect manager's list of effect instances (see above)
  bool EffectInstances(__int64 const*& begin, size_t& count)
  {
    __try
    {
      __int64 manager = *(__int64 const*)((__int64)g_gameHandle + EffectManagerRva);
      if (manager < 0x10000)
        return false;
      __int64 const* first = *(__int64 const* const*)(manager + EffectInstancesOffset);
      __int64 const* last = *(__int64 const* const*)(manager + EffectInstancesOffset + 8);
      if (!first || last < first || (size_t)(last - first) > MostEffectInstances)
        return false;
      begin = first;
      count = (size_t)(last - first);
      return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      return false;
    }
  }

  __int64 ListedInstance(__int64 const* begin, size_t i)
  {
    __try
    {
      return begin[i];
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      return 0;
    }
  }

  // As the game's Transform property does (0x14305F010): the place, its flag, the move
  void MoveInstance(__int64 instance, float const* rows)
  {
    __try
    {
      memcpy((void*)(instance + InstanceTransformOffset), rows, 16 * sizeof(float));
      *(BYTE*)(instance + InstanceFlagsOffset) |= InstanceMovedFlag;
      ((tInstanceMove)((__int64)g_gameHandle + InstanceMoveRva))(instance);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
  }

  // The code that keeps and moves the effect instances, as it was read (see above)
  bool IsEffectInstanceCode(BYTE const* pBase)
  {
    // 0x1432A1FF0: mov rdx, rcx; mov rcx, [0x142416160]; jmp 0x1432905B0 (the manager's culling)
    static const BYTE culling[] = { 0x48, 0x89, 0xCA, 0x48, 0x8B, 0x0D, 0x66, 0x41, 0x17, 0xFF, 0xE9, 0xB1, 0xE5, 0xFE, 0xFF };
    // 0x143290605: mov r15, [rsi + 0x48C0]; mov rax, [rsi + 0x48C8] (its list)
    static const BYTE list[] = { 0x4C, 0x8B, 0xBE, 0xC0, 0x48, 0x00, 0x00, 0x48, 0x8B, 0x86, 0xC8, 0x48, 0x00, 0x00 };
    // 0x14305F051 (the EffectEntity's Transform): mov rcx, [rbx + 0x50]; the 4 rows to + 0x100 .. + 0x130;
    // or byte [rcx + 0x1B8], 2; call 0x1430606F0
    static const BYTE move[] = { 0x48, 0x8B, 0x4B, 0x50, 0x0F, 0x28, 0x00, 0x0F, 0x29, 0x81, 0x00, 0x01, 0x00, 0x00, 0x0F, 0x28,
      0x48, 0x10, 0x0F, 0x29, 0x89, 0x10, 0x01, 0x00, 0x00, 0x0F, 0x28, 0x40, 0x20, 0x0F, 0x29, 0x81, 0x20, 0x01, 0x00, 0x00,
      0x0F, 0x28, 0x48, 0x30, 0x80, 0x89, 0xB8, 0x01, 0x00, 0x00, 0x02, 0x0F, 0x29, 0x89, 0x30, 0x01, 0x00, 0x00, 0xE8, 0x64,
      0x16, 0x00, 0x00 };
    // 0x1430606F0: mov [rsp + 0x18], rbx; push rsi; push rdi; push r14; sub rsp, 0x20
    static const BYTE moveStart[] = { 0x48, 0x89, 0x5C, 0x24, 0x18, 0x56, 0x57, 0x41, 0x56, 0x48, 0x83, 0xEC, 0x20 };
    return !memcmp(pBase + 0x32A1FF0, culling, sizeof(culling)) && !memcmp(pBase + 0x3290605, list, sizeof(list)) &&
      !memcmp(pBase + 0x305F051, move, sizeof(move)) && !memcmp(pBase + InstanceMoveRva, moveStart, sizeof(moveStart));
  }

  // The rain's streaks' camera boxes softened (2026-11-01, the user's
  // screenshot from up high, "this is the square ... can we make it seamless",
  // then "I can still see the square from the rain flickering"): the game's
  // rain streaks live in boxes round the camera, each emitter's
  // UpdateCameraProximityData (its Size, ForwardOffset metres ahead along the
  // view), wrapped at the box's faces with no fade across (FadeDistance 0;
  // fx/ambient/rain/emitters/em_evo_rain_streaks_*, read offline), so from up
  // high a box's outline showed as a square, lighter inside where its
  // streaks were, flickering as they wrapped at its faces. While the tools'
  // rain is on, each such processor of the rain's instances, known by the box
  // the game's data gives it (StreakBoxes: its ForwardOffset and Size, exact),
  // gets a FadeDistance of StreakBoxFade of its box each way (at least the
  // game's); the game's back when the rain's off, only where ours still is;
  // forgotten, unwritten, when an instance's zone unloads. The game's memory
  // layout (its type info in the game's image, 2026-11-01; the template's
  // TimeScale at + 0x44 and the document's High at + 0x28 there as the tools
  // already had them): EmitterTemplateData + 0x10 RootProcessor;
  // ProcessorData + 0x18 NextProcessor; UpdateCameraProximityData + 0x30
  // ForwardOffset, + 0x40 Size, + 0x50 FadeDistance; ScalableEmitterDocument
  // + 0x18 / 0x20 / 0x28 / 0x30 its Low / Medium / High / Ultra templates.
  // Whether the game reads FadeDistance as it plays (not only when an emitter
  // is made) isn't known yet: if it doesn't, nothing changes.
  const int TemplateRootProcessorOffset = 0x10;
  const int ProcessorNextOffset = 0x18;
  const int ProximityForwardOffset = 0x30, ProximitySizeOffset = 0x40, ProximityFadeOffset = 0x50;
  const int EmitterDocumentTemplateOffsets[4] = { 0x18, 0x20, 0x28, 0x30 };
  struct StreakBox { float Forward, Size[3], Fade[3]; };
  const StreakBox StreakBoxes[] = {
    { 2.f, { 2.f, 2.f, 2.f }, { 0.f, 0.f, 0.f } },       // Em_Evo_Rain_Streaks_CloseToMid
    { 2.f, { 5.f, 5.f, 5.f }, { 0.f, 0.f, 0.f } },       // _CloseUp, _CloseUp_Emissive
    { 10.f, { 7.f, 7.f, 7.f }, { 0.f, 0.f, 0.f } },      // _Mid
    { 15.f, { 10.f, 5.f, 10.f }, { 0.f, 2.f, 0.f } },    // _MidToFar
    { 3.f, { 6.f, 4.f, 6.f }, { 0.f, 1.f, 0.f } },       // _MidToFar_unAA
  };
  const int StreakBoxCount = (int)(sizeof(StreakBoxes) / sizeof(StreakBoxes[0]));
  const float StreakBoxFade = 0.25f;
  struct SoftBox
  {
    __int64 Processor;
    int Kind;           // the rain's instance it was found through (g_Rain)
    int Box;            // its StreakBoxes'
    float Written[3];   // the FadeDistance given
  };
  std::vector<SoftBox> g_SoftBoxes;        // under g_Mutex
  __int64 g_SoftenedFor[RainKinds] = {};   // per rain kind: the instance whose boxes were looked for (0 none)

  // A rain instance gone (its zone unloaded): its boxes forgotten, nothing
  // written (their memory may be gone too); the other kinds' looked for
  // again (a box found through both is kept through the one still there)
  void ForgetSoftBoxes(size_t kind)
  {
    for (size_t i = g_SoftBoxes.size(); i-- > 0;)
      if (g_SoftBoxes[i].Kind == (int)kind)
        g_SoftBoxes.erase(g_SoftBoxes.begin() + i);
    for (size_t k = 0; k < RainKinds; ++k)
      g_SoftenedFor[k] = 0;
  }

  // Under g_Mutex, game thread: the rain's instances and the rain columns
  // found in the game's list (when one's missing, at most once a second), and
  // the ones it had dropped when they're no longer in it (their zone unloaded)
  void TrackInstances(bool wantRain, bool wantColumns, ULONGLONG now)
  {
    __int64 const* begin = nullptr;
    size_t count = 0;
    if (g_InstanceCode != 1 || !EffectInstances(begin, count))
      return;
    // Still listed (one pass for all of them)
    bool listed[RainKinds] = {};
    std::vector<bool> columnListed(g_Columns.size(), false);
    for (size_t i = 0; i < count; ++i)
    {
      __int64 instance = ListedInstance(begin, i);
      for (size_t k = 0; k < RainKinds; ++k)
        listed[k] |= instance && instance == g_Rain[k].Instance;
      for (size_t c = 0; c < g_Columns.size(); ++c)
        if (instance && instance == g_Columns[c].Instance)
          columnListed[c] = true;
    }
    for (size_t k = 0; k < RainKinds; ++k)
      if (g_Rain[k].Instance && (!listed[k] || ReadPointer(g_Rain[k].Instance) != g_Rain[k].Data))
      {
        util::log::Write("Weather: the game's %s is gone (its zone unloaded)", g_Rain[k].Label);
        g_RainZoneGone = true;
        g_Rain[k].Instance = 0;
        g_Rain[k].Followed = false;
        ForgetSoftBoxes(k);
      }
    for (size_t c = g_Columns.size(); c-- > 0;)
      if (!columnListed[c] || ReadPointer(g_Columns[c].Instance) != g_Columns[c].Data)
      {
        if (g_ColumnWoken == g_Columns[c].Instance)
          g_ColumnWoken = 0;
        g_Columns.erase(g_Columns.begin() + c);
      }
    if (g_Columns.empty())
      g_ColumnsSeenTick = 0;   // the wait before waking one starts again with the next ones found

    bool missing = (wantRain && (!g_Rain[0].Instance || (g_Settings.Heavy && !g_Rain[1].Instance))) ||
      (wantColumns && g_Columns.empty());
    if (!missing || now - g_LastScan < InstanceScanMs)
      return;
    g_LastScan = now;
    int seen = 0;
    for (size_t i = 0; i < count; ++i)
    {
      __int64 instance = ListedInstance(begin, i);
      __int64 data = 0;
      char name[64];
      if (instance < 0x10000 || !ReadInstanceEmitter(instance, data, name, sizeof(name)))
        continue;
      ++seen;
      for (size_t k = 0; k < RainKinds; ++k)
      {
        GameRain& r = g_Rain[k];
        if (r.Instance || _stricmp(name, r.Emitter))
          continue;
        alignas(16) float home[16];
        if (!CopyRows((void const*)(instance + InstanceTransformOffset), home))
          continue;
        r.Instance = instance;
        r.Data = data;
        r.Followed = false;
        memcpy(r.Home, home, sizeof(home));
        util::log::Write("Weather: the game's %s found (effect instance 0x%I64X, its first emitter %s) at %.1f %.1f %.1f",
          r.Label, instance, name, home[12], home[13], home[14]);
      }
      if (!_stricmp(name, RainColumnEmitter) && g_Columns.size() < MaxColumns)
      {
        bool known = false;
        for (RainColumn const& c : g_Columns)
          known |= c.Instance == instance;
        RainColumn c{ instance, data };
        if (!known && CopyRows((void const*)(instance + InstanceTransformOffset), c.Home))
        {
          g_Columns.push_back(c);
          if (!g_ColumnsSeenTick)
            g_ColumnsSeenTick = now;
        }
      }
    }
    static ULONGLONG s_loggedAt = 0;
    if (now - s_loggedAt > 10000 && wantRain && !g_Rain[0].Instance)
    {
      s_loggedAt = now;
      util::log::Write("Weather: the game's rain not in its %d effect instances yet (%d with a named emitter)", (int)count, seen);
    }
  }

  __int64 ReadPointer(__int64 at)
  {
    __try
    {
      return *(__int64 const*)at;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      return 0;
    }
  }

  bool CopyRows(void const* from, float* to)
  {
    __try
    {
      memcpy(to, from, 16 * sizeof(float));
      return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      return false;
    }
  }

  bool IsRainParamCode(BYTE const* pBase)
  {
    // 0x1430FAE8D (the emitter system takes EmitterParameter3 - 1 from the state):
    // movss xmm0, [rax + 0x12E0]; movss xmm1, [rax + 0x12C4]; movss xmm2, [rax + 0x12A8]
    static const BYTE take[] = { 0xF3, 0x0F, 0x10, 0x80, 0xE0, 0x12, 0x00, 0x00, 0xF3, 0x0F, 0x10, 0x88, 0xC4, 0x12, 0x00,
      0x00, 0xF3, 0x0F, 0x10, 0x90, 0xA8, 0x12, 0x00, 0x00 };
    // 0x1436FC5F3 (the state of the view with ids 0, 0): mov r9, [rax]; cmp [r9], edx; jne; cmp [r9 + 4], r8d;
    // je; add rax, 8; jmp; mov rax, [r9 + 0x550]
    static const BYTE view[] = { 0x4C, 0x8B, 0x08, 0x41, 0x39, 0x11, 0x75, 0x06, 0x45, 0x39, 0x41, 0x04, 0x74, 0x06, 0x48,
      0x83, 0xC0, 0x08, 0xEB, 0xE7, 0x49, 0x8B, 0x81, 0x50, 0x05, 0x00, 0x00 };
    // 0x14328D597 / 0x14328D5D1 (an EmitterParamComponent fills the slot of its Parameter):
    // mov r12d, [rdi + 0x84] ... mov [rdi + r12 * 8 + 0x208], r15
    static const BYTE parameter[] = { 0x44, 0x8B, 0xA7, 0x84, 0x00, 0x00, 0x00 };
    static const BYTE slot[] = { 0x4E, 0x89, 0xBC, 0xE7, 0x08, 0x02, 0x00, 0x00 };
    return !memcmp(pBase + 0x30FAE8D, take, sizeof(take)) && !memcmp(pBase + 0x36FC5F3, view, sizeof(view)) &&
      !memcmp(pBase + 0x328D597, parameter, sizeof(parameter)) && !memcmp(pBase + 0x328D5D1, slot, sizeof(slot));
  }

  // The state of a view of the visual environment manager when it's the one
  // the emitter system reads (ids 0, 0), else 0
  __int64 RainParamState(__int64 view)
  {
    __try
    {
      if (*(int const*)view != 0 || *(int const*)(view + 4) != 0)
        return 0;
      __int64 state = *(__int64 const*)(view + VeViewStateOffset);
      return state >= 0x10000 ? state : 0;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      return 0;
    }
  }

  bool ReadFloat(__int64 at, float& value)
  {
    __try
    {
      value = *(float const*)at;
      return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      return false;
    }
  }

  bool WriteFloat(__int64 at, float value)
  {
    __try
    {
      *(float*)at = value;
      return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      return false;
    }
  }

  // A processor's box (see StreakBoxes): which of them, -1 none (what's read
  // may not be one, so read under __try)
  int StreakBoxOf(__int64 processor)
  {
    __try
    {
      float forward = *(float const*)(processor + ProximityForwardOffset);
      float const* size = (float const*)(processor + ProximitySizeOffset);
      for (int i = 0; i < StreakBoxCount; ++i)
      {
        StreakBox const& b = StreakBoxes[i];
        if (forward == b.Forward && size[0] == b.Size[0] && size[1] == b.Size[1] && size[2] == b.Size[2])
          return i;
      }
      return -1;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      return -1;
    }
  }

  // An array's count (a Frostbite array: its count just before its first)
  bool ReadCount(__int64 at, unsigned int& count)
  {
    __try
    {
      count = *(unsigned int const*)at;
      return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      return false;
    }
  }

  // Under g_Mutex: a rain instance's streaks' camera boxes softened (see
  // StreakBoxes), through its effect's emitters' templates' processors; how
  // many new ones
  int SoftenStreakBoxes(size_t kind)
  {
    GameRain const& r = g_Rain[kind];
    __int64 components = ReadPointer(r.Data + EffectDataComponentsOffset);
    unsigned int count = 0;
    if (components < 0x10000 || !ReadCount(components - 4, count) || !count || count > 64)
      return 0;
    __int64 templates[64];
    int templateCount = 0;
    int found = 0;
    for (unsigned int c = 0; c < count; ++c)
    {
      __int64 emitter = ReadPointer(components + 8 * (__int64)c);
      __int64 document = emitter >= 0x10000 ? ReadPointer(emitter + EmitterDataEmitterOffset) : 0;
      if (document < 0x10000)
        continue;
      for (int q : EmitterDocumentTemplateOffsets)
      {
        __int64 t = ReadPointer(document + q);
        bool seen = t < 0x10000 || templateCount >= 64;
        for (int s = 0; s < templateCount && !seen; ++s)
          seen = templates[s] == t;
        if (seen)
          continue;
        templates[templateCount++] = t;
        __int64 p = ReadPointer(t + TemplateRootProcessorOffset);
        for (int step = 0; step < 64 && p >= 0x10000; ++step, p = ReadPointer(p + ProcessorNextOffset))
        {
          int box = StreakBoxOf(p);
          if (box < 0)
            continue;
          bool known = false;
          for (SoftBox const& s : g_SoftBoxes)
            known |= s.Processor == p;
          if (known)
            continue;
          StreakBox const& b = StreakBoxes[box];
          SoftBox s{ p, (int)kind, box, {} };
          bool ok = true;
          for (int i = 0; i < 3 && ok; ++i)
          {
            s.Written[i] = (std::max)(b.Fade[i], StreakBoxFade * b.Size[i]);
            ok = WriteFloat(p + ProximityFadeOffset + 4 * i, s.Written[i]);
          }
          if (ok)
          {
            g_SoftBoxes.push_back(s);
            ++found;
          }
        }
      }
    }
    return found;
  }

  // Under g_Mutex, game thread, each update: while want (the tools' rain on),
  // each rain instance's boxes softened once (see StreakBoxes); otherwise the
  // game's back where ours still is
  void UpdateSoftBoxes(bool want)
  {
    if (!want)
    {
      if (!g_SoftBoxes.empty())
      {
        int back = 0;
        for (SoftBox const& s : g_SoftBoxes)
        {
          float now = 0.f;
          bool ours = true;
          for (int i = 0; i < 3 && ours; ++i)
            ours = ReadFloat(s.Processor + ProximityFadeOffset + 4 * i, now) && now == s.Written[i];
          if (!ours)
            continue;
          for (int i = 0; i < 3; ++i)
            WriteFloat(s.Processor + ProximityFadeOffset + 4 * i, StreakBoxes[s.Box].Fade[i]);
          ++back;
        }
        util::log::Write("Weather: the rain streaks' camera boxes back as the game had them (%d of %d)", back,
          (int)g_SoftBoxes.size());
        g_SoftBoxes.clear();
      }
      for (size_t k = 0; k < RainKinds; ++k)
        g_SoftenedFor[k] = 0;
      return;
    }
    for (size_t k = 0; k < RainKinds; ++k)
    {
      GameRain const& r = g_Rain[k];
      if (!r.Instance || g_SoftenedFor[k] == r.Instance)
        continue;
      g_SoftenedFor[k] = r.Instance;
      int found = SoftenStreakBoxes(k);
      util::log::Write("Weather: the game's %s: %d of its streaks' camera boxes softened (their faces faded over a "
        "quarter of the box; %d in all)", r.Label, found, (int)g_SoftBoxes.size());
    }
  }

  // Waterfalls and fountains frozen while it snows (2026-10-26, the user:
  // "Build the waterfall and fountain freeze while it snows (TimeScale 0 on
  // their emitters, back to 1 when the snow goes)"; Research_Snow_2026-10-26.md
  // sections 7 and 8). The city's waterfalls and fountains are placed effects
  // (fx/ambient/water: FX_Waterfall*, FX_Water_Fountain_*, in the prop zones
  // and the waterfeature prefabs), their water sprites. Every update the game
  // multiplies an emitter's time step by its template's TimeScale
  // (EmitterTemplateData + 0x44 in the game's type info): 0x1430F1973 for the
  // emitter's clock and 0x1430E2AC5 in its particles' update, the template
  // there [[emitter + 0x280] + 8]. At 0 its particles stay where they are,
  // don't age, and none are made: the waterfall stands still. The tools find
  // the effects in the effect manager's list (as the rain, above), each
  // emitter's four templates (its document + 0x18 / 0x20 / 0x28 / 0x30, Low
  // to Ultra: which one the game uses goes by the quality) by DebugName, and
  // set TimeScale 0; the game's value back when the snow goes, only where
  // ours still is and only to a template that's still the same (its name).
  // A template is shared by every effect using that emitter.
  // An effect that has only just started has nothing to show yet. The game
  // culls an effect farther than its CullDistance (EffectEntityData + 0x78 +
  // quality x 4, Research_Weather section 5) and starts it again when it's
  // back in range, which a frozen emitter would leave empty; the first build
  // also thawed a whole kind whenever one of its effects restarted, so with the
  // small ones culled at 40 m they hardly ever froze (the 02:17 test: frozen
  // only after 107 s, 4 kinds; the user: "these waterfalls are not frozen").
  // Now while it snows the water effects' cull distances are raised to
  // WaterCullWhileFrozen (the game's back after), so they stay running; a
  // kind is frozen once every running one of it has run WaterFillMs (or after
  // WaterFreezeAfterMs whatever); after that only an effect newly in the list
  // (streamed in) thaws its kind for WaterFillMs so it has water to freeze.
  // Not frozen: the ones on the lens (their names have "OnScreen"), and
  // effects inside the game's interiors (the Kingdom's waterfalls).
  const int TemplateTimeScaleOffset = 0x44;
  const int EmitterDocumentTemplates[4] = { 0x18, 0x20, 0x28, 0x30 };
  const int EffectDataCullDistanceOffset = 0x78;
  const float WaterCullWhileFrozen = 3000.f;
  const BYTE InstanceCulledFlag = 0x10;
  const ULONGLONG WaterFillMs = 6000;
  const ULONGLONG WaterFreezeAfterMs = 20000;
  const ULONGLONG WaterScanMs = 1000;
  const int MostEffectTemplates = 64;
  const size_t MostWaterEffects = 256;
  const size_t MostWaterTemplates = 256;
  struct WaterEffect
  {
    __int64 Instance, Data;
    ULONGLONG Started;      // when it was seen (re)started
    bool Culled;
    bool New;               // newly in the list (streamed in) and not yet run WaterFillMs
    std::vector<__int64> Templates;   // its water emitters' templates
  };
  struct WaterTemplate
  {
    __int64 Template;
    float Original;         // the game's TimeScale
    bool Frozen;
    ULONGLONG Seen;         // when first seen
    char Name[64];
  };
  struct WaterData
  {
    __int64 Data;           // an effect's EffectEntityData (shared by its instances)
    float Original[4];      // its cull distances, the game's
  };
  std::vector<WaterEffect> g_WaterEffects;      // game thread, g_Mutex held
  std::vector<WaterTemplate> g_WaterTemplates;
  std::vector<WaterData> g_WaterData;
  ULONGLONG g_WaterScan = 0;
  int g_WaterCode = 0;                          // 0 not checked, 1 the code the tools know, -1 not
  bool InsideInterior(float const* world, bool generic = true);   // (the game's interiors, below)
  bool InShutLair(float const* world);   // (the lair with its windows shut, below)
  bool NamedInteriorTop(float const* world, float& top);
  float InteriorAt(float const* world, float known);

  // The game multiplying an emitter's time step by its template's TimeScale,
  // as it was read
  bool IsTimeScaleCode(BYTE const* pBase)
  {
    static const BYTE holder[] = { 0x48, 0x8B, 0x81, 0x80, 0x02, 0x00, 0x00 };   // 0x1430F1938 mov rax, [rcx + 0x280]
    static const BYTE templ[] = { 0x48, 0x8B, 0x78, 0x08 };                      // 0x1430F1958 mov rdi, [rax + 8]
    static const BYTE scale[] = { 0xF3, 0x0F, 0x59, 0x4F, 0x44 };                // 0x1430F1973 mulss xmm1, [rdi + 0x44]
    static const BYTE particles[] = { 0xF3, 0x41, 0x0F, 0x59, 0x4B, 0x44 };      // 0x1430E2AC5 mulss xmm1, [r11 + 0x44]
    return !memcmp(pBase + 0x30F1938, holder, sizeof(holder)) && !memcmp(pBase + 0x30F1958, templ, sizeof(templ)) &&
      !memcmp(pBase + 0x30F1973, scale, sizeof(scale)) && !memcmp(pBase + 0x30E2AC5, particles, sizeof(particles));
  }

  bool ReadTemplateName(__int64 templ, char* out, size_t size)
  {
    __try
    {
      char const* name = *(char const* const*)(templ + EmitterTemplateNameOffset);
      if ((__int64)name < 0x10000)
        return false;
      size_t i = 0;
      for (; i + 1 < size && name[i]; ++i)
        out[i] = name[i];
      out[i] = 0;
      return i > 0;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      return false;
    }
  }

  // A waterfall's, a fountain's or a wall stream's emitter (the streams'
  // splash, mist, ripples and drops: fx_waterstream_small / _small_splash /
  // _rain_small_long(er), in the stream prefabs), not one on the lens
  // (OnScreen, the streams' CamDroplets)
  bool IsWaterEmitter(char const* name)
  {
    if (_strnicmp(name, "Em_Waterfall", 12) && _strnicmp(name, "Em_Water_Fountain", 17) &&
        _strnicmp(name, "Em_WaterStream", 14))
      return false;
    char lower[64];
    size_t i = 0;
    for (; name[i] && i + 1 < sizeof(lower); ++i)
      lower[i] = (char)tolower((unsigned char)name[i]);
    lower[i] = 0;
    return !strstr(lower, "onscreen") && !strstr(lower, "camdroplets");
  }

  // An effect instance's emitters' templates: each of its components'
  // emitter document's four, without repeats (a component that isn't an
  // emitter gives none that pass the name test); false when it can't be read
  bool ReadEffectTemplates(__int64 instance, __int64& data, __int64* out, int most, int& count)
  {
    count = 0;
    __try
    {
      data = *(__int64 const*)instance;
      if (data < 0x10000)
        return false;
      __int64 components = *(__int64 const*)(data + EffectDataComponentsOffset);
      if (components < 0x10000)
        return false;
      unsigned int n = *(unsigned int const*)(components - 4);
      if (!n || n > 64)
        return false;
      for (unsigned int c = 0; c < n; ++c)
      {
        __int64 component = *(__int64 const*)(components + c * 8);
        if (component < 0x10000)
          continue;
        __int64 document = *(__int64 const*)(component + EmitterDataEmitterOffset);
        if (document < 0x10000)
          continue;
        for (int q = 0; q < 4; ++q)
        {
          __int64 t = *(__int64 const*)(document + EmitterDocumentTemplates[q]);
          if (t < 0x10000)
            continue;
          bool seen = false;
          for (int k = 0; k < count; ++k)
            seen |= out[k] == t;
          if (!seen && count < most)
            out[count++] = t;
        }
      }
      return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      return false;
    }
  }

  BYTE ReadByteAt(__int64 at)
  {
    __try
    {
      return *(BYTE const*)at;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      return 0;
    }
  }

  // A water template's TimeScale back to the game's, if ours is still there
  // and it's still that emitter's template
  void ThawTemplate(WaterTemplate& w)
  {
    char name[64];
    float now = 1.f;
    if (w.Frozen && ReadTemplateName(w.Template, name, sizeof(name)) && !strcmp(name, w.Name) &&
        ReadFloat(w.Template + TemplateTimeScaleOffset, now) && now == 0.f)
      WriteFloat(w.Template + TemplateTimeScaleOffset, w.Original);
    w.Frozen = false;
  }

  // A water effect's data's cull distances back to the game's, where ours
  // still are and one of its effects still has that data
  void LowerCull(WaterData const& w)
  {
    bool live = false;
    for (WaterEffect const& e : g_WaterEffects)
      live |= e.Data == w.Data && ReadPointer(e.Instance) == w.Data;
    if (!live)
      return;
    for (int q = 0; q < 4; ++q)
    {
      float now = 0.f;
      if (ReadFloat(w.Data + EffectDataCullDistanceOffset + q * 4, now) && now == WaterCullWhileFrozen &&
          w.Original[q] != WaterCullWhileFrozen)
        WriteFloat(w.Data + EffectDataCullDistanceOffset + q * 4, w.Original[q]);
    }
  }

  // Under g_Mutex, game thread, every update: the water frozen while wanted
  // (see above), everything given back when not
  void UpdateFrozenWater(bool want, ULONGLONG now)
  {
    if (g_WaterCode == 0)
    {
      g_WaterCode = IsTimeScaleCode((BYTE const*)g_gameHandle) ? 1 : -1;
      if (g_WaterCode < 0)
        util::log::Write("Snow: the game's emitter time code isn't what the tools know: waterfalls and fountains aren't frozen");
    }
    if (!want || g_WaterCode != 1 || g_InstanceCode != 1)
    {
      int thawed = 0;
      for (WaterTemplate& w : g_WaterTemplates)
        if (w.Frozen)
        {
          ThawTemplate(w);
          ++thawed;
        }
      for (WaterData const& w : g_WaterData)
        LowerCull(w);
      if (thawed)
        util::log::Write("Snow: waterfalls and fountains flowing again (%d emitter templates given back their time, %d "
          "effects their cull distances)", thawed, (int)g_WaterData.size());
      g_WaterTemplates.clear();
      g_WaterEffects.clear();
      g_WaterData.clear();
      g_WaterScan = 0;
      return;
    }
    if (g_WaterScan && now - g_WaterScan < WaterScanMs)
      return;
    g_WaterScan = now;
    __int64 const* begin = nullptr;
    size_t count = 0;
    if (!EffectInstances(begin, count))
      return;
    // The water effects in the list now: new ones started now, the ones back
    // from being culled restarted, the ones gone dropped
    std::vector<bool> listed(g_WaterEffects.size(), false);
    for (size_t i = 0; i < count; ++i)
    {
      __int64 instance = ListedInstance(begin, i);
      if (instance < 0x10000)
        continue;
      size_t known = 0;
      while (known < g_WaterEffects.size() && g_WaterEffects[known].Instance != instance)
        ++known;
      __int64 data = 0;
      if (known < g_WaterEffects.size())
      {
        WaterEffect& e = g_WaterEffects[known];
        if (ReadPointer(instance) != e.Data)
          continue;   // another effect in its place: dropped below, looked at again next time
        listed[known] = true;
        bool culled = (ReadByteAt(instance + InstanceFlagsOffset) & InstanceCulledFlag) != 0;
        if (e.Culled && !culled)
          e.Started = now;
        e.Culled = culled;
        continue;
      }
      if (g_WaterEffects.size() >= MostWaterEffects)
        continue;
      __int64 templates[MostEffectTemplates];
      int templateCount = 0;
      if (!ReadEffectTemplates(instance, data, templates, MostEffectTemplates, templateCount) || !templateCount)
        continue;
      WaterEffect e{ instance, data, now, false, true, {} };
      for (int k = 0; k < templateCount; ++k)
      {
        char name[64];
        if (ReadTemplateName(templates[k], name, sizeof(name)) && IsWaterEmitter(name))
          e.Templates.push_back(templates[k]);
      }
      if (e.Templates.empty())
        continue;
      alignas(16) float rows[16];
      if (!CopyRows((void const*)(instance + InstanceTransformOffset), rows) || InsideInterior(rows + 12))
        continue;
      e.Culled = (ReadByteAt(instance + InstanceFlagsOffset) & InstanceCulledFlag) != 0;
      g_WaterEffects.push_back(std::move(e));
      listed.push_back(true);
    }
    for (size_t k = g_WaterEffects.size(); k-- > 0;)
      if (!listed[k])
        g_WaterEffects.erase(g_WaterEffects.begin() + k);
    for (WaterEffect& e : g_WaterEffects)
      if (e.New && !e.Culled && now - e.Started >= WaterFillMs)
        e.New = false;
    // Their cull distances raised (each effect's data once; the game's kept)
    for (WaterEffect const& e : g_WaterEffects)
    {
      bool known = false;
      for (WaterData const& w : g_WaterData)
        known |= w.Data == e.Data;
      if (known)
        continue;
      WaterData w{ e.Data, {} };
      bool read = true;
      for (int q = 0; q < 4 && read; ++q)
        read = ReadFloat(e.Data + EffectDataCullDistanceOffset + q * 4, w.Original[q]);
      if (!read)
        continue;
      for (int q = 0; q < 4; ++q)
        if (w.Original[q] < WaterCullWhileFrozen)
          WriteFloat(e.Data + EffectDataCullDistanceOffset + q * 4, WaterCullWhileFrozen);
      g_WaterData.push_back(w);
    }
    // Each template (a template that's no longer that emitter's, its zone
    // unloaded, forgotten without a write)
    for (size_t k = g_WaterTemplates.size(); k-- > 0;)
    {
      char name[64];
      if (!ReadTemplateName(g_WaterTemplates[k].Template, name, sizeof(name)) || strcmp(name, g_WaterTemplates[k].Name))
        g_WaterTemplates.erase(g_WaterTemplates.begin() + k);
    }
    for (WaterEffect const& e : g_WaterEffects)
      for (__int64 t : e.Templates)
      {
        bool known = false;
        for (WaterTemplate const& w : g_WaterTemplates)
          known |= w.Template == t;
        if (known || g_WaterTemplates.size() >= MostWaterTemplates)
          continue;
        WaterTemplate w{ t, 1.f, false, now, {} };
        if (ReadTemplateName(t, w.Name, sizeof(w.Name)) && ReadFloat(t + TemplateTimeScaleOffset, w.Original))
          g_WaterTemplates.push_back(w);
      }
    int frozenNow = 0, frozeNew = 0;
    for (WaterTemplate& w : g_WaterTemplates)
    {
      bool filled = true, fresh = false, used = false;
      for (WaterEffect const& e : g_WaterEffects)
      {
        if (e.Culled || std::find(e.Templates.begin(), e.Templates.end(), w.Template) == e.Templates.end())
          continue;
        used = true;
        filled &= now - e.Started >= WaterFillMs;
        fresh |= e.New;
      }
      if (!w.Frozen && used && (filled || now - w.Seen >= WaterFreezeAfterMs))
      {
        // (a template the game already holds at 0 is left alone)
        if (w.Original != 0.f && WriteFloat(w.Template + TemplateTimeScaleOffset, 0.f))
        {
          w.Frozen = true;
          ++frozeNew;
        }
      }
      else if (w.Frozen && fresh)
        ThawTemplate(w);   // one just streamed in: its kind flows until it has water
      frozenNow += w.Frozen ? 1 : 0;
    }
    if (frozeNew)
      util::log::Write("Snow: waterfalls and fountains frozen (%d emitter templates now, %d of their effects placed near, "
        "%d with their cull distances raised; the first: %s)", frozenNow, (int)g_WaterEffects.size(), (int)g_WaterData.size(),
        g_WaterTemplates.empty() ? "-" : g_WaterTemplates[0].Name);
  }

  __int64 MakeVoice(__int64 asset)
  {
    __try
    {
      __int64 manager = *(__int64 const*)((__int64)g_gameHandle + VoiceManagerRva);
      return manager > 0x10000 && oSoundCreate ? oSoundCreate(manager, asset, 0, 0) : 0;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      return 0;
    }
  }

  void PlaceVoice(__int64 voice, float const* rows)
  {
    __try
    {
      alignas(16) float aligned[16];
      memcpy(aligned, rows, sizeof(aligned));
      if (oVoiceSetTransform)
        oVoiceSetTransform(voice, aligned);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
  }

  void GiveVoiceEvent(__int64 voice, unsigned int event)
  {
    __try
    {
      if (oVoiceEvent)
        oVoiceEvent(voice, event);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
  }

  void ReleaseVoice(__int64 voice)
  {
    __try
    {
      typedef void(__fastcall* tRelease)(__int64);
      tRelease release = *(tRelease const*)(*(__int64 const*)voice + VoiceReleaseSlot * 8);
      release(voice);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
  }

  // Under g_Mutex: the rain sound's asset still the one it was
  bool RainSoundAssetThere()
  {
    if (!g_RainSound.Asset || ReadPointer(g_RainSound.Asset) != g_RainSound.AssetVTable)
      return false;
    char name[96];
    __int64 namePointer = 0;
    return ReadName(g_RainSound.Asset, name, sizeof(name), namePointer) && !_stricmp(name, RainSoundName);
  }

  bool ReadCamera(__int64 pRenderView, float* rows)
  {
    __try
    {
      memcpy(rows, (void const*)(pRenderView + RenderViewTransform), 16 * sizeof(float));
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      return false;
    }
    // Near the origin: no camera in the world (a loading screen)
    for (int i = 12; i < 15; ++i)
      if (!std::isfinite(rows[i]))
        return false;
    return fabsf(rows[12]) + fabsf(rows[13]) + fabsf(rows[14]) > 2.f;
  }

  // The exclusion code as it was read (see above)
  bool IsExclusionCode(BYTE const* pBase)
  {
    // 0x1430F4E2D (an entity taking a slot): mov rcx, [manager]; movzx ebx, di; mov rax, [rcx + 8]; cmp [rcx], rax;
    // jne; mov eax, [rcx + 0x820]; cmp eax, edi (0x400); jae; movzx ebx, ax; inc eax; mov [rcx + 0x820], eax
    static const BYTE take[] = { 0x48, 0x8B, 0x0D, 0x14, 0xF3, 0x2E, 0xFF, 0x0F, 0xB7, 0xDF, 0x48, 0x8B, 0x41, 0x08, 0x48, 0x39,
      0x01, 0x75, 0x19, 0x8B, 0x81, 0x20, 0x08, 0x00, 0x00, 0x39, 0xF8, 0x73, 0x2D, 0x0F, 0xB7, 0xD8, 0xFF, 0xC0, 0x89, 0x81,
      0x20, 0x08, 0x00, 0x00 };
    // 0x1430F4E59: movzx ebx, word [rax - 2]; add rax, -2; mov [rcx + 8], rax (a freed one)
    static const BYTE pop[] = { 0x0F, 0xB7, 0x58, 0xFE, 0x48, 0x83, 0xC0, 0xFE, 0x48, 0x89, 0x41, 0x08 };
    // 0x1430F4E9C (giving one back): mov rbx, [manager]; lea r8, [the empty box]; movzx edx, bp; mov rcx, rbx; call
    // 0x1430FBDE0; mov rcx, [rbx + 8]; lea rax, [rcx + 2]; mov [rbx + 8], rax; test rcx, rcx; je; mov [rcx], bp
    static const BYTE give[] = { 0x48, 0x8B, 0x1D, 0xA5, 0xF2, 0x2E, 0xFF, 0x4C, 0x8D, 0x05, 0x46, 0xF3, 0x2E, 0xFF, 0x0F, 0xB7,
      0xD5, 0x48, 0x89, 0xD9, 0xE8, 0x2B, 0x6F, 0x00, 0x00, 0x48, 0x8B, 0x4B, 0x08, 0x48, 0x8D, 0x41, 0x02, 0x48, 0x89, 0x43,
      0x08, 0x48, 0x85, 0xC9, 0x74, 0x03, 0x66, 0x89, 0x29 };
    // 0x1430FBDE0 (a slot's box): ...; mov r9, rcx; movzx ecx, dx; cmp ecx, [r9 + 0x820]
    static const BYTE set[] = { 0x49, 0x89, 0xE3, 0x55, 0x49, 0x8D, 0x6B, 0xA1, 0x48, 0x81, 0xEC, 0xC0, 0x00, 0x00, 0x00, 0x49,
      0x89, 0xC9, 0x0F, 0xB7, 0xCA, 0x41, 0x3B, 0x89, 0x20, 0x08, 0x00, 0x00 };
    // 0x1430F40F4 (the emitter system's copy): mov rdi, [manager]; mov eax, [rdi + 0x820]
    static const BYTE copy[] = { 0x48, 0x8B, 0x3D, 0x4D, 0x00, 0x2F, 0xFF, 0x8B, 0x87, 0x20, 0x08, 0x00, 0x00 };
    return !memcmp(pBase + 0x30F4E2D, take, sizeof(take)) && !memcmp(pBase + 0x30F4E59, pop, sizeof(pop)) &&
      !memcmp(pBase + 0x30F4E9C, give, sizeof(give)) && !memcmp(pBase + ExclusionSetBoxRva, set, sizeof(set)) &&
      !memcmp(pBase + 0x30F40F4, copy, sizeof(copy));
  }

  __int64 ExclusionManager()
  {
    __try
    {
      __int64 manager = *(__int64 const*)((__int64)g_gameHandle + ExclusionManagerRva);
      return manager > 0x10000 ? manager : 0;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      return 0;
    }
  }

  bool SetBox(__int64 manager, unsigned int slot, float const* rows)
  {
    __try
    {
      alignas(16) float aligned[16];
      memcpy(aligned, rows, sizeof(aligned));
      ((tSetBox)((__int64)g_gameHandle + ExclusionSetBoxRva))(manager, slot, aligned);
      return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      return false;
    }
  }

  bool SetEmptyBox(__int64 manager, unsigned int slot)
  {
    return SetBox(manager, slot, (float const*)((__int64)g_gameHandle + ExclusionEmptyBoxRva));
  }

  // As the game takes one (0x1430F4DF0): a freed slot, or the next one
  bool TakeSlot(__int64 manager, unsigned int& slot)
  {
    __try
    {
      unsigned short* top = *(unsigned short**)(manager + ExclusionFreeTopOffset);
      unsigned short* begin = *(unsigned short**)(manager + ExclusionFreeBeginOffset);
      if (top != begin)
      {
        slot = top[-1];
        *(unsigned short**)(manager + ExclusionFreeTopOffset) = top - 1;
        return slot < ExclusionMostSlots;
      }
      unsigned int count = *(unsigned int const*)(manager + ExclusionCountOffset);
      if (count >= ExclusionMostSlots)
        return false;
      slot = count;
      *(unsigned int*)(manager + ExclusionCountOffset) = count + 1;
      return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      return false;
    }
  }

  // As the game gives one back (0x1430F4E9C): the empty box, onto the stack
  void GiveSlot(__int64 manager, unsigned int slot)
  {
    SetEmptyBox(manager, slot);
    __try
    {
      unsigned short* top = *(unsigned short**)(manager + ExclusionFreeTopOffset);
      unsigned short* end = *(unsigned short**)(manager + ExclusionFreeEndOffset);
      if (top && top < end)
      {
        *top = (unsigned short)slot;
        *(unsigned short**)(manager + ExclusionFreeTopOffset) = top + 1;
      }
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
    }
  }

  // Under g_Mutex, game thread: the tools' boxes all given back
  void GiveBackBoxes()
  {
    __int64 manager = ExclusionManager();
    if (manager && manager == g_SlotsManager)
      for (unsigned int slot : g_Slots)
        GiveSlot(manager, slot);
    if (!g_Slots.empty())
      util::log::Write("Weather: the %d exclusion boxes of the tools given back", (int)g_Slots.size());
    g_Slots.clear();
    g_Applied.clear();
    g_DryBoxes.clear();
    g_SlotsManager = 0;
    g_SlotsFull = false;
    g_LastBoxCount = -1;
    g_CameraCovered = false;
    g_CameraRoofUp = 0.f;
  }

  // Under g_Mutex, game thread: the roofs around the camera looked for (a
  // few squares a frame) and made into boxes
  void UpdateRoofs(float const* camera, bool applyBoxes)
  {
    wetlook::ScopedTime timed(wetlook::TimeRoofs);
    float cx = camera[12], cy = camera[13], cz = camera[14];
    int ix = (int)floorf(cx / CellSize), iz = (int)floorf(cz / CellSize);
    ULONGLONG now = GetTickCount64();

    // The squares to look at, nearest first: not looked at yet, too long ago,
    // from a height too far from the camera's, or whose roof is now under it
    struct Want { int X, Z; float Distance; };
    static std::vector<Want> wants;   // game thread
    wants.clear();
    for (int gz = -GridHalf; gz <= GridHalf; ++gz)
      for (int gx = -GridHalf; gx <= GridHalf; ++gx)
      {
        auto it = g_Cells.find(CellKey(ix + gx, iz + gz));
        ULONGLONG refresh = gx * gx + gz * gz <= RefreshNear * RefreshNear ? RefreshMs : FarRefreshMs;
        if (it != g_Cells.end() && now - it->second.Tick < refresh && fabsf(it->second.FromY - cy) < RefreshHeight &&
            !(it->second.Covered && it->second.Roof < cy - 0.25f) && it->second.Places == SquarePlaces)
          continue;
        // (those not looked at yet first)
        wants.push_back({ ix + gx, iz + gz, (float)(gx * gx + gz * gz) - (it == g_Cells.end() ? 1e6f : 0.f) });
      }
    std::sort(wants.begin(), wants.end(), [](Want const& a, Want const& b) { return a.Distance < b.Distance; });
    // A place's roof (1e9 none) and floor (-1e9 none), up and down from the
    // camera's height (see SquarePlaces)
    auto lookAt = [&](int x, int z, int p, float& roofY, float& floorY)
    {
      float place[3] = { (x + 0.5f + SquarePlace[p][0]) * CellSize, cy, (z + 0.5f + SquarePlace[p][1]) * CellSize };
      float up[3] = { place[0], cy + RoofSearch, place[2] };
      float down[3] = { place[0], cy - FloorSearch, place[2] };
      float fraction = 1.f, downFraction = 1.f, normal[3] = {};
      bool roofHit = collision::Sweep(place, up, 0.f, &fraction, normal);
      bool floorHit = collision::Sweep(place, down, 0.f, &downFraction, normal);
      roofY = roofHit ? cy + RoofSearch * fraction : 1e9f;
      floorY = floorHit ? cy - FloorSearch * downFraction : -1e9f;
      // (not something standing there, see LowestCover; unless what the floor
      // ray found is itself something standing on a floor under a real roof,
      // a table, a counter, a bed, a shelf: the next surface under it within
      // LowestCover is the floor that counts. 2026-11-01, the user: "You can
      // still see some rain inside buildings": over a table the ceiling was
      // less than LowestCover over its top, so no roof, rain in the room)
      if (roofY < 1e8f && floorY > -1e8f && roofY - floorY < LowestCover)
      {
        // (something over it too, from 25 cm over it: a ceiling, a roof, a
        // bridge; it hangs under that or stands under it, ducts, pipes, beams,
        // a lamp, a car in a car park: covered, the higher one its roof.
        // 2026-11-01, the user: "I can still see some rain dripping indoor from
        // edges": a duct under a ceiling less than LowestCover over the floor
        // made the room open, its edges dripping)
        float over[3] = { place[0], roofY + 0.25f, place[2] };
        float sky[3] = { place[0], cy + RoofSearch, place[2] };
        float upper = 1.f;
        if (over[1] < sky[1] && collision::Sweep(over, sky, 0.f, &upper, normal))
          roofY = over[1] + (sky[1] - over[1]) * upper;
        else
        {
          float below[3] = { place[0], floorY - 0.05f, place[2] };
          float deeper[3] = { place[0], floorY - 0.05f - LowestCover, place[2] };
          float under = 1.f;
          bool onFloor = collision::Sweep(below, deeper, 0.f, &under, normal) &&
            roofY - (below[1] - LowestCover * under) >= LowestCover;
          if (!onFloor)
            roofY = 1e9f;
        }
      }
    };
    int sweeps = 0;
    for (Want const& w : wants)
    {
      if (sweeps + 2 > SweepsPerFrame)
        break;
      auto seen = g_Cells.find(CellKey(w.X, w.Z));
      bool looked = seen != g_Cells.end();
      bool even = looked && seen->second.Places == SquarePlaces && seen->second.Even;
      bool all = looked && !even;
      if (all && sweeps + 2 * SquarePlaces > SweepsPerFrame)
        continue;
      float roofs[SquarePlaces], floors[SquarePlaces];
      lookAt(w.X, w.Z, 0, roofs[0], floors[0]);
      sweeps += 2;
      int places = 1;
      if (even)
      {
        // Alike before: still so where its middle found what the square had
        Cell const& was = seen->second;
        float wasFloor = was.Floor <= was.FromY - FloorSearch + 0.01f ? -1e9f : was.Floor;
        if (fabsf(roofs[0] - (was.Covered ? was.Roof : 1e9f)) < PlacesAlike && fabsf(floors[0] - wasFloor) < PlacesAlike)
          places = SquarePlaces;
        else
        {
          even = false;
          all = sweeps + 2 * (SquarePlaces - 1) <= SweepsPerFrame;
        }
      }
      float roofMiddle = roofs[0], floorMiddle = floors[0];
      if (all)
      {
        for (int p = 1; p < SquarePlaces; ++p)
          lookAt(w.X, w.Z, p, roofs[p], floors[p]);
        sweeps += 2 * (SquarePlaces - 1);
        places = SquarePlaces;
        std::sort(roofs, roofs + SquarePlaces);
        std::sort(floors, floors + SquarePlaces);
        even = roofs[SquarePlaces - 1] - roofs[0] < PlacesAlike && floors[SquarePlaces - 1] - floors[0] < PlacesAlike;
        roofMiddle = roofs[SquarePlaces / 2];
        floorMiddle = floors[SquarePlaces / 2];
      }
      bool hit = roofMiddle < 1e8f;
      float from[3] = { (w.X + 0.5f) * CellSize, cy, (w.Z + 0.5f) * CellSize };
      // In one of the game's named interiors at the camera's height: covered
      // up to its top (2026-10-26, the user: "there are still some water
      // droplets indoors": the lair's roof isn't found by the sweeps, so the
      // squares had its inside open to the sky, the game's rain wasn't kept
      // out there and drips were made in it; the wet look's InteriorAt keeps
      // these interiors dry the same way)
      float interiorTop = 1e9f;
      float at[3] = { from[0], cy, from[2] };
      if (!NamedInteriorTop(at, interiorTop))
        interiorTop = 1e9f;
      g_Cells[CellKey(w.X, w.Z)] = { hit, hit ? roofMiddle : cy + RoofSearch, floorMiddle > -1e8f ? floorMiddle : cy - FloorSearch, cy,
        now, interiorTop, places, even };
    }
    // The ones left far behind forgotten
    if (g_Cells.size() > (size_t)(4 * GridSize * GridSize))
      for (auto it = g_Cells.begin(); it != g_Cells.end();)
      {
        int x = (int)(it->first >> 32), z = (int)(unsigned int)it->first;
        it = (abs(x - ix) > GridHalf + 2 || abs(z - iz) > GridHalf + 2) ? g_Cells.erase(it) : std::next(it);
      }

    // Covered squares into boxes: from each one not yet in a box, nearest
    // first, along x then z while the squares are covered with roofs close to
    // its own (closer near the camera, looser farther out, so far ones take
    // fewer boxes); each box up to just into the highest of its roofs. A
    // square whose roof is now under the camera isn't covered. The squares
    // known to be open (and their floors) go to the rain the tools draw.
    static bool covered[GridSize][GridSize];   // game thread
    static bool known[GridSize][GridSize];
    static float roof[GridSize][GridSize];
    static float floorAt[GridSize][GridSize];
    static bool used[GridSize][GridSize];
    memset(covered, 0, sizeof(covered));
    memset(known, 0, sizeof(known));
    memset(used, 0, sizeof(used));
    for (int gz = 0; gz < GridSize; ++gz)
      for (int gx = 0; gx < GridSize; ++gx)
      {
        auto it = g_Cells.find(CellKey(ix + gx - GridHalf, iz + gz - GridHalf));
        if (it == g_Cells.end())
          continue;
        known[gz][gx] = true;
        floorAt[gz][gx] = it->second.Floor;
        if (it->second.Covered && it->second.Roof >= cy - 0.25f)
        {
          covered[gz][gx] = true;
          roof[gz][gx] = it->second.Roof;
        }
        // No roof found, inside a named interior: its top for its roof
        else if (it->second.InteriorTop < 1e8f && it->second.InteriorTop >= cy - 0.25f)
        {
          covered[gz][gx] = true;
          roof[gz][gx] = it->second.InteriorTop;
        }
      }
    g_RainGridOrigin[0] = ix - GridHalf;
    g_RainGridOrigin[1] = iz - GridHalf;
    g_RainGrid.resize(GridSize * GridSize * 2);
    for (int gz = 0; gz < GridSize; ++gz)
      for (int gx = 0; gx < GridSize; ++gx)
      {
        bool open = known[gz][gx] && !covered[gz][gx];
        g_RainGrid[(gz * GridSize + gx) * 2] = open ? 1.f : 0.f;
        g_RainGrid[(gz * GridSize + gx) * 2 + 1] = open ? floorAt[gz][gx] : 1e9f;
      }
    g_ShelterGrid.assign(GridSize * GridSize * 4, 0.f);
    for (int gz = 0; gz < GridSize; ++gz)
      for (int gx = 0; gx < GridSize; ++gx)
      {
        auto it = g_Cells.find(CellKey(ix + gx - GridHalf, iz + gz - GridHalf));
        float* c = &g_ShelterGrid[(gz * GridSize + gx) * 4];
        if (it == g_Cells.end())
          continue;
        Cell const& cell = it->second;
        c[0] = 1.f;
        c[1] = cell.Floor <= cell.FromY - FloorSearch + 0.01f ? -1e9f : cell.Floor;
        c[2] = cell.Covered ? cell.Roof : cell.InteriorTop < 1e8f ? cell.InteriorTop : 1e9f;
        c[3] = cell.FromY;
      }
    // How much the camera is inside: the share of the squares within 6 m of
    // it under a roof, more when its own is (for the thunder)
    {
      int around = 0, coveredAround = 0;
      for (int dz = -3; dz <= 3; ++dz)
        for (int dx = -3; dx <= 3; ++dx)
        {
          if (dx * dx + dz * dz > 9 || !known[GridHalf + dz][GridHalf + dx])
            continue;
          ++around;
          coveredAround += covered[GridHalf + dz][GridHalf + dx] ? 1 : 0;
        }
      float share = around ? (float)coveredAround / around : 0.f;
      g_Inside = covered[GridHalf][GridHalf] ? 0.5f + 0.5f * share : 0.5f * share;
    }
    // Under a roof the rain around the camera is taken out (said in the log
    // when that changes)
    static int s_CameraCovered = -1;   // game thread
    int cameraCovered = covered[GridHalf][GridHalf] ? 1 : 0;
    g_CameraCovered = cameraCovered != 0;
    g_CameraRoofUp = cameraCovered ? (std::max)(0.f, roof[GridHalf][GridHalf] - cy) : 0.f;
    if (cameraCovered != s_CameraCovered)
    {
      s_CameraCovered = cameraCovered;
      if (cameraCovered)
        util::log::Write("Weather: the camera is under a roof %.1f m up: no rain around it", roof[GridHalf][GridHalf] - cy);
      else
        util::log::Write("Weather: the camera is under the open sky");
    }
    if (!applyBoxes)
    {
      // (the edges aren't looked for then: no drips, see g_DripEdges)
      g_DripEdges.clear();
      if (!g_Slots.empty())
        GiveBackBoxes();
      g_CameraCovered = cameraCovered != 0;   // (giving the boxes back clears it)
      g_CameraRoofUp = cameraCovered ? (std::max)(0.f, roof[GridHalf][GridHalf] - cy) : 0.f;
      return;
    }
    // The roofs' edges near the camera (see EdgeSweeps), nearest first
    const int sideX[4] = { 1, -1, 0, 0 }, sideZ[4] = { 0, 0, 1, -1 };
    {
      struct EdgeWant { int X, Z, Side; float Distance; };
      static std::vector<EdgeWant> edgeWants;   // game thread
      edgeWants.clear();
      for (int gz = GridHalf - EdgeReach; gz <= GridHalf + EdgeReach; ++gz)
        for (int gx = GridHalf - EdgeReach; gx <= GridHalf + EdgeReach; ++gx)
        {
          if (!covered[gz][gx])
            continue;
          auto cell = g_Cells.find(CellKey(ix + gx - GridHalf, iz + gz - GridHalf));
          if (cell == g_Cells.end() || !cell->second.Covered)
            continue;
          for (int side = 0; side < 4; ++side)
          {
            int nx = gx + sideX[side], nz = gz + sideZ[side];
            if (nx < 0 || nz < 0 || nx >= GridSize || nz >= GridSize || !known[nz][nx] || covered[nz][nx])
              continue;
            auto e = g_Edges.find(EdgeKey(ix + gx - GridHalf, iz + gz - GridHalf, side));
            if (e != g_Edges.end() && now - e->second.Tick < FarRefreshMs && fabsf(e->second.FromY - cy) < RefreshHeight &&
                fabsf(e->second.Roof - roof[gz][gx]) < 0.5f)
              continue;
            int ddx = gx - GridHalf, ddz = gz - GridHalf;
            edgeWants.push_back({ gx, gz, side, (float)(ddx * ddx + ddz * ddz) });
          }
        }
      std::sort(edgeWants.begin(), edgeWants.end(), [](EdgeWant const& a, EdgeWant const& b) { return a.Distance < b.Distance; });
      int edgeSweeps = 0;
      for (EdgeWant const& e : edgeWants)
      {
        if (edgeSweeps + EdgeSweeps > EdgeSweepsPerFrame || sweeps + EdgeSweeps > SweepsPerFrame)
          break;
        float mx = (ix + e.X - GridHalf + 0.5f) * CellSize, mz = (iz + e.Z - GridHalf + 0.5f) * CellSize;
        float lo = 0.f, hi = CellSize;
        for (int k = 0; k < EdgeSweeps; ++k)
        {
          float mid = 0.5f * (lo + hi);
          float from[3] = { mx + sideX[e.Side] * mid, cy, mz + sideZ[e.Side] * mid };
          float up[3] = { from[0], cy + RoofSearch, from[2] };
          float fraction = 1.f, normal[3] = {};
          if (collision::Sweep(from, up, 0.f, &fraction, normal))
            lo = mid;
          else
            hi = mid;
        }
        edgeSweeps += EdgeSweeps;
        sweeps += EdgeSweeps;
        g_Edges[EdgeKey(ix + e.X - GridHalf, iz + e.Z - GridHalf, e.Side)] = { lo, hi, cy, roof[e.Z][e.X], now };
      }
      // The ones left far behind forgotten
      if (g_Edges.size() > (size_t)(8 * (2 * EdgeReach + 1) * (2 * EdgeReach + 1)))
        for (auto it = g_Edges.begin(); it != g_Edges.end();)
        {
          int x = (int)(it->first >> 34), z = (int)(unsigned int)(it->first >> 2);
          it = (abs(x - ix) > EdgeReach + 4 || abs(z - iz) > EdgeReach + 4) ? g_Edges.erase(it) : std::next(it);
        }
    }
    // Where the roofs' drips fall (see g_DripEdges): each roofed square's side
    // towards an open one whose edge is known and still holds
    g_DripEdges.assign(GridSize * GridSize * 4, -1.f);
    for (int gz = 0; gz < GridSize; ++gz)
      for (int gx = 0; gx < GridSize; ++gx)
      {
        if (!covered[gz][gx])
          continue;
        for (int side = 0; side < 4; ++side)
        {
          auto e = g_Edges.find(EdgeKey(ix + gx - GridHalf, iz + gz - GridHalf, side));
          if (e != g_Edges.end() && fabsf(e->second.FromY - cy) < RefreshHeight &&
              fabsf(e->second.Roof - roof[gz][gx]) < 0.5f && e->second.Open < CellSize - 0.01f)
            g_DripEdges[(gz * GridSize + gx) * 4 + side] = e->second.Open;
        }
      }
    // How far a box's side goes out past its squares: BoxPad, or where all
    // it meets is open sky (another roof beside it reaches over with its own
    // box), as far as their roofs go (see EdgeSweeps). line: the squares'
    // column (sides 0, 1) or row (2, 3); first, last: those along it
    auto sideOut = [&](int line, int first, int last, int side)
    {
      float most = -1e9f;
      bool any = false;
      for (int a = first; a <= last && most < BoxPad; ++a)
      {
        int gx = side < 2 ? line : a, gz = side < 2 ? a : line;
        int nx = gx + sideX[side], nz = gz + sideZ[side];
        bool inside = nx >= 0 && nz >= 0 && nx < GridSize && nz < GridSize;
        if (inside && covered[nz][nx])
          continue;
        any = true;
        float want = BoxPad;
        if (inside && known[nz][nx])
        {
          auto e = g_Edges.find(EdgeKey(ix + gx - GridHalf, iz + gz - GridHalf, side));
          if (e != g_Edges.end() && fabsf(e->second.FromY - cy) < RefreshHeight && fabsf(e->second.Roof - roof[gz][gx]) < 0.5f)
            want = e->second.Open - 0.5f * CellSize + EdgePad;
        }
        most = (std::max)(most, want);
      }
      return any ? (std::max)(EdgePad - 0.5f * CellSize, (std::min)(most, BoxPad)) : BoxPad;
    };
    static std::vector<Want> seeds;   // game thread
    seeds.clear();
    for (int gz = 0; gz < GridSize; ++gz)
      for (int gx = 0; gx < GridSize; ++gx)
        if (covered[gz][gx])
          seeds.push_back({ gx, gz, (float)((gx - GridHalf) * (gx - GridHalf) + (gz - GridHalf) * (gz - GridHalf)) });
    std::sort(seeds.begin(), seeds.end(), [](Want const& a, Want const& b) { return a.Distance < b.Distance; });
    float bottom = floorf((cy - BoxBelow) / BoxStep) * BoxStep;
    std::vector<Box> boxes;
    int roofsLeft = 0;
    for (Want const& s : seeds)
    {
      if (used[s.Z][s.X])
        continue;
      if (boxes.size() >= MaxBoxes)
      {
        ++roofsLeft;
        continue;
      }
      float seedRoof = roof[s.Z][s.X];
      float tolerance = RoofMergeHeight + (std::max)(0.f, sqrtf(s.Distance) * CellSize - 12.f) * 0.25f;
      auto fits = [&](int x, int z) {
        return x >= 0 && x < GridSize && z >= 0 && z < GridSize && covered[z][x] && !used[z][x] &&
          fabsf(roof[z][x] - seedRoof) <= tolerance;
      };
      int x0 = s.X, x1 = s.X, z0 = s.Z, z1 = s.Z;
      while (fits(x1 + 1, s.Z)) ++x1;
      while (fits(x0 - 1, s.Z)) --x0;
      auto rowFits = [&](int z) {
        for (int x = x0; x <= x1; ++x)
          if (!fits(x, z))
            return false;
        return true;
      };
      while (rowFits(z1 + 1)) ++z1;
      while (rowFits(z0 - 1)) --z0;
      float highest = seedRoof;
      for (int z = z0; z <= z1; ++z)
        for (int x = x0; x <= x1; ++x)
        {
          used[z][x] = true;
          highest = (std::max)(highest, roof[z][x]);
        }
      float top = (std::max)(highest + RoofOverlap, cy + CameraClearance);
      if (top - bottom < 0.5f)
        continue;
      // (each side out past its squares as sideOut has it)
      float out[4] = { sideOut(x1, z0, z1, 0), sideOut(x0, z0, z1, 1), sideOut(z1, x0, x1, 2), sideOut(z0, x0, x1, 3) };
      Box b = { { (ix + x0 - GridHalf) * CellSize - out[1], bottom, (iz + z0 - GridHalf) * CellSize - out[3] },
        { (ix + x1 + 1 - GridHalf) * CellSize + out[0], top, (iz + z1 + 1 - GridHalf) * CellSize + out[2] } };
      boxes.push_back(b);
    }
    static ULONGLONG s_leftLogged = 0;
    if (roofsLeft && now - s_leftLogged > 10000)
    {
      s_leftLogged = now;
      util::log::Write("Weather: more roofs than boxes: %d roofed squares farthest from the camera left without a box (%d boxes)",
        roofsLeft, (int)boxes.size());
    }

    g_DryBoxes = boxes;
    // And round the people (KSec of every kind, everyone else) and drones near
    // the camera (2026-10-26, the user: "rain also shouldn't go through people
    // and all the ksec types"; dry::GetBodies), each its body's box made
    // level (the box round it along the world's axes), after the roofs: the
    // game takes the first 32 of the boxes near a rain emitter in slot order,
    // so the roofs come first
    int bodyBoxes = 0;
    {
      static dry::Box bodies[dry::MostBodies];   // game thread
      int found = dry::GetBodies(bodies, dry::MostBodies);
      for (int i = 0; i < found; ++i)
      {
        dry::Box const& d = bodies[i];
        float half[3];
        for (int j = 0; j < 3; ++j)
          half[j] = fabsf(d.Axis[0][j]) * d.Half[0] + fabsf(d.Axis[1][j]) * d.Half[1] + fabsf(d.Axis[2][j]) * d.Half[2];
        if (!std::isfinite(half[0] + half[1] + half[2] + d.Middle[0] + d.Middle[1] + d.Middle[2]))
          continue;
        boxes.push_back({ { d.Middle[0] - half[0], d.Middle[1] - half[1], d.Middle[2] - half[2] },
          { d.Middle[0] + half[0], d.Middle[1] + half[1], d.Middle[2] + half[2] } });
        ++bodyBoxes;
      }
    }
    // Into the game's exclusion volumes, only what changed
    __int64 manager = ExclusionManager();
    if (!manager)
      return;
    if (manager != g_SlotsManager)
    {
      // A new manager (a level loaded): the old slots aren't in it
      g_Slots.clear();
      g_Applied.clear();
      g_SlotsManager = manager;
      g_SlotsFull = false;
    }
    while (g_Slots.size() < boxes.size() && !g_SlotsFull)
    {
      unsigned int slot = 0;
      if (!TakeSlot(manager, slot))
      {
        g_SlotsFull = true;
        util::log::Write("Weather: the game's exclusion volumes are full (%u slots): rain kept out under %d roofs only",
          ExclusionMostSlots, (int)g_Slots.size());
        break;
      }
      g_Slots.push_back(slot);
    }
    // Nearest boxes in the lowest slots (the game looks at them in slot order)
    std::sort(g_Slots.begin(), g_Slots.end());
    g_Applied.resize(g_Slots.size(), Box{ { 1e30f, 0, 0 }, { 1e30f, 0, 0 } });
    for (size_t i = 0; i < g_Slots.size(); ++i)
    {
      if (i < boxes.size())
      {
        if (g_Applied[i] == boxes[i])
          continue;
        Box const& b = boxes[i];
        float rows[16] = {
          b.Max[0] - b.Min[0], 0.f, 0.f, 0.f,
          0.f, b.Max[1] - b.Min[1], 0.f, 0.f,
          0.f, 0.f, b.Max[2] - b.Min[2], 0.f,
          0.5f * (b.Min[0] + b.Max[0]), 0.5f * (b.Min[1] + b.Max[1]), 0.5f * (b.Min[2] + b.Max[2]), 1.f };
        if (SetBox(manager, g_Slots[i], rows))
          g_Applied[i] = b;
      }
      else if (g_Applied[i].Min[0] != 1e30f)
      {
        if (SetEmptyBox(manager, g_Slots[i]))
          g_Applied[i] = Box{ { 1e30f, 0, 0 }, { 1e30f, 0, 0 } };
      }
    }
    static bool s_bodiesLogged = false;   // game thread
    if (bodyBoxes && !s_bodiesLogged)
    {
      s_bodiesLogged = true;
      util::log::Write("Weather: rain kept out of the people and drones near the camera too: %d boxes round them", bodyBoxes);
    }
    int roofBoxes = (int)boxes.size() - bodyBoxes;
    if (roofBoxes != g_LastBoxCount && now - g_BoxLogTick > 5000)
    {
      g_LastBoxCount = roofBoxes;
      g_BoxLogTick = now;
      util::log::Write("Weather: rain kept out under roofs around the camera: %d boxes, and %d round people and drones (%d slots "
        "of the tools')", roofBoxes, bodyBoxes, (int)g_Slots.size());
    }
  }
}

namespace
{
  // Under g_Mutex: the square open to the sky (see UpdateRoofs) nearest the
  // camera, minDistance - maxDistance from it level: its middle and its floor
  bool NearestOpenSquare(float const* camera, float minDistance, float maxDistance, float& x, float& z, float& floorY)
  {
    if (g_RainGrid.size() != (size_t)(GridSize * GridSize * 2))
      return false;
    float best = 1e30f;
    for (int gz = 0; gz < GridSize; ++gz)
      for (int gx = 0; gx < GridSize; ++gx)
      {
        float const* c = &g_RainGrid[(gz * GridSize + gx) * 2];
        if (c[0] < 0.5f)
          continue;
        float cx = (g_RainGridOrigin[0] + gx + 0.5f) * CellSize, cz = (g_RainGridOrigin[1] + gz + 0.5f) * CellSize;
        float d = sqrtf((cx - camera[12]) * (cx - camera[12]) + (cz - camera[14]) * (cz - camera[14]));
        if (d < minDistance || d > maxDistance || d >= best)
          continue;
        best = d;
        x = cx;
        z = cz;
        floorY = c[1];
      }
    return best < 1e30f;
  }

  // Under g_Mutex: where a rain column can be woken (see ColumnWakeNearest)
  bool ColumnWakeSpot(float const* camera, float& x, float& y, float& z)
  {
    if (g_RainGrid.size() != (size_t)(GridSize * GridSize * 2))
      return false;
    float best = 1e30f;
    for (int gz = 0; gz < GridSize; ++gz)
      for (int gx = 0; gx < GridSize; ++gx)
      {
        float const* c = &g_RainGrid[(gz * GridSize + gx) * 2];
        if (c[0] < 0.5f)
          continue;
        float cx = (g_RainGridOrigin[0] + gx + 0.5f) * CellSize, cz = (g_RainGridOrigin[1] + gz + 0.5f) * CellSize;
        float d = sqrtf((cx - camera[12]) * (cx - camera[12]) + (cz - camera[14]) * (cz - camera[14]));
        if (d < ColumnWakeNearest || d > ColumnWakeFarthest || d >= best)
          continue;
        float cy = (std::max)(c[1] + ColumnWakeOverFloor, camera[13] + 2.f);
        float up = cy - camera[13];
        if (up > RoofSearch - 10.f || d * d + up * up > ColumnWakeReach * ColumnWakeReach)
          continue;
        best = d;
        x = cx;
        y = cy;
        z = cz;
      }
    return best < 1e30f;
  }

  // A voice's place: at source, its cone (the output's local -y) on the
  // listener
  void AimAt(float* rows, float const* source, float const* listener)
  {
    float up[3] = { source[0] - listener[0], source[1] - listener[1], source[2] - listener[2] };
    float length = sqrtf(up[0] * up[0] + up[1] * up[1] + up[2] * up[2]);
    static const float level[12] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0 };
    memcpy(rows, level, sizeof(level));
    if (length > 0.01f)
    {
      for (float& u : up)
        u /= length;
      float ref[3] = { 0.f, 0.f, 0.f };
      ref[fabsf(up[1]) < 0.9f ? 1 : 0] = 1.f;
      // right = up x ref, forward = right x up
      float right[3] = { up[1] * ref[2] - up[2] * ref[1], up[2] * ref[0] - up[0] * ref[2], up[0] * ref[1] - up[1] * ref[0] };
      float r = sqrtf(right[0] * right[0] + right[1] * right[1] + right[2] * right[2]);
      for (float& c : right)
        c /= r;
      float forward[3] = { right[1] * up[2] - right[2] * up[1], right[2] * up[0] - right[0] * up[2], right[0] * up[1] - right[1] * up[0] };
      memcpy(rows + 0, right, sizeof(right));
      memcpy(rows + 4, up, sizeof(up));
      memcpy(rows + 8, forward, sizeof(forward));
    }
    rows[12] = source[0];
    rows[13] = source[1];
    rows[14] = source[2];
    rows[15] = 1.f;
  }
}

namespace
{
  // Water out of the drainpipes (2026-10-26, the user: "and drainpipes", then,
  // by the side of a climbable pipe, "go ahead and read them";
  // Research_Weather_2026-10-25.md section 33). The model groups keep these
  // pipes' places where the tools can't read them (their members'
  // InstanceTransforms are empty: placed by their physics), so they're found
  // among the mesh streaming manager's instances (each its bounding sphere's
  // middle and its mesh; see FindCranes) by their meshes' names and boxes (a
  // mesh set's box at its + 0, its name at + 0x58):
  // - the city's downpipes, objects/props/pipe/pipebackalley01/
  //   dt_backalleys_pipe_32cm / 64cm / 128cm / 384cm (and _nosupport,
  //   _nocolision): straight pieces along their own y, 0 to their length.
  //   The same pieces also lie flat (rows of them along walls, read live), so
  //   a piece is a downpipe's only with another standing right on it at the
  //   same place (x, z within 12 cm, the ends within 8 cm) or one of the
  //   bends (dt_backalleys_pipe_corner*, _45_*) right under its foot;
  // - the climbable pipes, objects/props/runnerobject/pipe_climbable/
  //   clustered_new/*cluster* (5.8 - 15 m, their box round their middle,
  //   along y: they stand upright, Faith climbs up them).
  // An outlet is a pipe's lowest foot (out at the bend when there's one:
  // twice the bend's middle's offset from the pipe, at most 35 cm, 12 cm
  // under it). Each outlet is looked at once with the collision sweep (see
  // LookAtOutlet): not in a named interior, open sky over it, no floor at its
  // foot (the pipe doesn't end in a floor), and the ground 0.15 - 30 m under
  // it. Within PipeReach, rescanned every PipeScanMs; the water runs as hard
  // as it rains, easing in over PipeRiseSeconds and out over PipeFallSeconds.
  // Drawn by WetLook.hlsl's PipesAt.
  const float PipeReach = 45.f;
  const ULONGLONG PipeScanMs = 2000;
  const ULONGLONG PipeMeshMs = 15000;      // the meshes' names read again
  const int PipeSweepsPerFrame = 18;       // an outlet takes 9 at most
  const size_t MostPipeOutlets = 64;
  const float PipeFallLeast = 0.15f, PipeFallMost = 30.f;
  const float PipeRiseSeconds = 6.f, PipeFallSeconds = 15.f;

  // Kind: 0 none, 1 a downpipe's straight piece, 2 a climbable pipe, 3 a bend
  struct PipeMeshKind { unsigned char Kind; float Length; };
  std::vector<PipeMeshKind> g_PipeMeshes;
  __int64 g_PipeTableBegin = 0, g_PipeTableEnd = 0;
  ULONGLONG g_PipeMeshTick = 0, g_PipeScanTick = 0, g_PipeFlowTick = 0, g_PipeLogTick = 0;
  // State: 0 not looked at yet, 1 water, -1 inside, -2 it ends in a floor,
  // -3 no ground under it within PipeFallLeast - PipeFallMost
  // Mouth: a bend's way out from the pipe (x, z; 0 none)
  struct PipeOutlet { float At[3]; float Ground; int State; float Mouth[2]; };
  std::vector<PipeOutlet> g_Pipes;
  float g_PipeFlow = 0.f;
  int g_PipeLogged = -1;

  // A mesh's box (its mesh set's, local: min, max), false if it can't be read
  bool ReadMeshBox(__int64 tb, size_t k, float* box)
  {
    __try
    {
      __int64 asset = *(__int64 const*)(tb + (__int64)k * 0x350);
      if (asset < 0x10000)
        return false;
      __int64 res = *(__int64 const*)(asset + 0x20);
      if (res < 0x10000)
        return false;
      memcpy(box, (void const*)res, 12);
      memcpy(box + 3, (void const*)(res + 0x10), 12);
      return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      return false;
    }
  }

  // What a mesh is by its name (lower case; see PipeMeshKind)
  unsigned char PipeKindOf(char const* low)
  {
    static const char downpipe[] = "pipebackalley01/dt_backalleys_pipe_";
    char const* p = strstr(low, downpipe);
    if (p)
    {
      p += sizeof(downpipe) - 1;
      if (*p >= '0' && *p <= '9')
      {
        while (*p >= '0' && *p <= '9')
          ++p;
        return strncmp(p, "cm", 2) == 0 ? 1 : 0;
      }
      return strncmp(p, "corner", 6) == 0 || strncmp(p, "45_", 3) == 0 ? 3 : 0;
    }
    if (strstr(low, "pipe_climbable/clustered_new/") && strstr(low, "cluster"))
      return 2;
    return 0;
  }

  // Under g_Mutex, game thread: the outlets round the camera (see above),
  // the ones looked at before kept as they were
  void ScanPipes(float const* camera, ULONGLONG now)
  {
    __int64 tb = 0, te = 0, ib = 0, ie = 0;
    if (!ReadMeshLists(util::offsets::GetOffset("OFFSET_MESHSTREAMING"), tb, te, ib, ie))
    {
      g_Pipes.clear();
      return;
    }
    size_t meshes = (std::min)((size_t)((te - tb) / 0x350), (size_t)65536);
    if (tb != g_PipeTableBegin || te != g_PipeTableEnd || !g_PipeMeshTick || now - g_PipeMeshTick >= PipeMeshMs)
    {
      g_PipeTableBegin = tb;
      g_PipeTableEnd = te;
      g_PipeMeshTick = now;
      g_PipeMeshes.assign(meshes, PipeMeshKind{ 0, 0.f });
      char low[192];
      for (size_t k = 0; k < meshes; ++k)
      {
        if (!ReadMeshName(tb, k, low, sizeof(low)))
          continue;
        unsigned char kind = PipeKindOf(low);
        float box[6];
        if (!kind || !ReadMeshBox(tb, k, box))
          continue;
        float length = box[4] - box[1];
        if (length > 0.05f && length < 40.f)
          g_PipeMeshes[k] = { kind, length };
      }
    }
    struct Piece { float X, Y, Z, Lo, Hi; unsigned char Kind; bool Used; };
    static std::vector<Piece> pieces, bends;   // game thread
    pieces.clear();
    bends.clear();
    size_t count = (std::min)((size_t)((ie - ib) / 32), (size_t)(4u << 20));
    for (size_t i = 0; i < count; ++i)
    {
      float radius = 0.f, c[3];
      unsigned int mesh = 0;
      if (!ReadMeshInstance(ib, i, radius, mesh, c))
        break;
      if (mesh >= g_PipeMeshes.size() || !g_PipeMeshes[mesh].Kind || !std::isfinite(c[0] + c[1] + c[2]))
        continue;
      float dx = c[0] - camera[12], dz = c[2] - camera[14];
      if (dx * dx + dz * dz > PipeReach * PipeReach || fabsf(c[1] - camera[13]) > PipeReach)
        continue;
      PipeMeshKind const& k = g_PipeMeshes[mesh];
      Piece piece{ c[0], c[1], c[2], c[1] - k.Length * 0.5f, c[1] + k.Length * 0.5f, k.Kind, false };
      (k.Kind == 3 ? bends : pieces).push_back(piece);
    }
    // Each pipe from its lowest piece up (sorted by their feet: the next one
    // up starts at least where this one does)
    std::sort(pieces.begin(), pieces.end(), [](Piece const& a, Piece const& b) { return a.Lo < b.Lo; });
    std::vector<PipeOutlet> outlets;
    for (size_t i = 0; i < pieces.size(); ++i)
    {
      if (pieces[i].Used)
        continue;
      pieces[i].Used = true;
      Piece const& foot = pieces[i];
      size_t top = i;
      int stacked = 1;
      bool climbable = foot.Kind == 2;
      for (size_t j = i + 1; j < pieces.size(); ++j)
      {
        Piece& o = pieces[j];
        if (o.Lo > pieces[top].Hi + 0.08f)
          break;
        if (o.Used || fabsf(o.X - pieces[top].X) >= 0.12f || fabsf(o.Z - pieces[top].Z) >= 0.12f ||
            fabsf(o.Lo - pieces[top].Hi) >= 0.08f)
          continue;
        o.Used = true;
        top = j;
        ++stacked;
        climbable |= o.Kind == 2;
      }
      float at[3] = { foot.X, foot.Lo, foot.Z };
      float mouth[2] = { 0.f, 0.f };
      bool bend = false;
      for (Piece const& b : bends)
        if (fabsf(b.X - foot.X) < 0.45f && fabsf(b.Z - foot.Z) < 0.45f && b.Y > foot.Lo - 0.6f && b.Y < foot.Lo + 0.05f)
        {
          float ox = 2.f * (b.X - foot.X), oz = 2.f * (b.Z - foot.Z);
          float length = sqrtf(ox * ox + oz * oz);
          if (length > 0.35f)
          {
            ox *= 0.35f / length;
            oz *= 0.35f / length;
          }
          at[0] = foot.X + ox;
          at[1] = b.Y - 0.12f;
          at[2] = foot.Z + oz;
          mouth[0] = ox;
          mouth[1] = oz;
          bend = true;
          break;
        }
      if (!climbable && stacked < 2 && !bend)
        continue;
      PipeOutlet outlet{ { at[0], at[1], at[2] }, 0.f, 0, { mouth[0], mouth[1] } };
      for (PipeOutlet const& old : g_Pipes)
        if (fabsf(old.At[0] - at[0]) < 0.05f && fabsf(old.At[1] - at[1]) < 0.05f && fabsf(old.At[2] - at[2]) < 0.05f)
        {
          outlet = old;
          break;
        }
      outlets.push_back(outlet);
    }
    if (outlets.size() > MostPipeOutlets)
    {
      auto distance = [&](PipeOutlet const& o) {
        float dx = o.At[0] - camera[12], dy = o.At[1] - camera[13], dz = o.At[2] - camera[14];
        return dx * dx + dy * dy + dz * dz;
      };
      std::sort(outlets.begin(), outlets.end(), [&](PipeOutlet const& a, PipeOutlet const& b) { return distance(a) < distance(b); });
      outlets.resize(MostPipeOutlets);
    }
    g_Pipes.swap(outlets);
  }

  // Under g_Mutex, game thread: whether water comes out of an outlet (see
  // PipeOutlet's State), with up to 9 sweeps
  void LookAtOutlet(PipeOutlet& o)
  {
    float const* a = o.At;
    // In a named interior (the lair, the droneworks, ...)
    if (InteriorAt(a, 1.f) > 0.05f)
    {
      o.State = -1;
      return;
    }
    const float sides[4][2] = { { 1.f, 0.f }, { -1.f, 0.f }, { 0.f, 1.f }, { 0.f, -1.f } };
    // Open sky over it: of four places 0.6 m round it (one may be in the
    // wall), at least one with nothing over it for RoofSearch
    int open = 0;
    for (int k = 0; k < 4 && !open; ++k)
    {
      float from[3] = { a[0] + sides[k][0] * 0.6f, a[1] + 0.3f, a[2] + sides[k][1] * 0.6f };
      float to[3] = { from[0], from[1] + RoofSearch, from[2] };
      float fraction = 1.f, normal[3] = {};
      if (!collision::Sweep(from, to, 0.f, &fraction, normal))
        ++open;
    }
    if (!open)
    {
      o.State = -1;
      return;
    }
    // Not ending in a floor: none of four places 25 cm round it has a floor
    // (what faces up) from 50 cm over its foot to 15 cm under it
    for (int k = 0; k < 4; ++k)
    {
      float from[3] = { a[0] + sides[k][0] * 0.25f, a[1] + 0.5f, a[2] + sides[k][1] * 0.25f };
      float to[3] = { from[0], a[1] - PipeFallLeast, from[2] };
      float fraction = 1.f, normal[3] = {};
      if (collision::Sweep(from, to, 0.f, &fraction, normal) && normal[1] >= 0.5f)
      {
        o.State = -2;
        return;
      }
    }
    // The ground under it (from a little past a bend's mouth, not to touch
    // the bend)
    float from[3] = { a[0] + o.Mouth[0] * 0.25f, a[1] - 0.05f, a[2] + o.Mouth[1] * 0.25f };
    float to[3] = { from[0], a[1] - PipeFallMost, from[2] };
    float fraction = 1.f, normal[3] = {};
    if (!collision::Sweep(from, to, 0.f, &fraction, normal))
    {
      o.State = -3;
      return;
    }
    o.Ground = from[1] + (to[1] - from[1]) * fraction;
    o.State = a[1] - o.Ground >= PipeFallLeast ? 1 : -3;
  }

  // Under g_Mutex, game thread, each update: the water's flow eased, the
  // outlets rescanned and looked at a few a frame. wanted: it rains with the
  // wet look on and the setting ticked (the flow eases out without)
  void UpdatePipes(float const* camera, bool wanted, bool held, ULONGLONG now)
  {
    wetlook::ScopedTime timed(wetlook::TimePipes);
    float dt = g_PipeFlowTick && !held ? (now - g_PipeFlowTick) / 1000.f : 0.f;
    g_PipeFlowTick = now;
    float target = wanted ? (std::min)(1.f, RainAmountNow()) : 0.f;
    float step = dt / (target > g_PipeFlow ? PipeRiseSeconds : PipeFallSeconds);
    g_PipeFlow += (std::max)(-step, (std::min)(step, target - g_PipeFlow));
    if (!wanted)
    {
      if (g_PipeFlow <= 0.f)
        g_Pipes.clear();
      return;
    }
    if (!collision::Available())
      return;
    if (!g_PipeScanTick || now - g_PipeScanTick >= PipeScanMs)
    {
      g_PipeScanTick = now;
      ScanPipes(camera, now);
    }
    int sweeps = 0, waiting = 0;
    for (PipeOutlet& o : g_Pipes)
    {
      if (o.State != 0)
        continue;
      if (sweeps + 9 > PipeSweepsPerFrame)
      {
        ++waiting;
        continue;
      }
      LookAtOutlet(o);
      sweeps += 9;
    }
    // Said when the outlets with water change, all looked at
    if (!waiting && sweeps)
    {
      int counts[4] = {};   // water, inside, ending in a floor, no ground
      for (PipeOutlet const& o : g_Pipes)
        ++counts[o.State == 1 ? 0 : o.State == -1 ? 1 : o.State == -2 ? 2 : 3];
      if (counts[0] != g_PipeLogged && now - g_PipeLogTick > 10000)
      {
        g_PipeLogged = counts[0];
        g_PipeLogTick = now;
        util::log::Write("Weather: drainpipes: water out of %d of the %d pipes found within %.0f m (%d inside or under a "
          "roof, %d ending in a floor, %d with no ground under them)", counts[0], (int)g_Pipes.size(), PipeReach,
          counts[1], counts[2], counts[3]);
      }
    }
  }
}

void weather::Update(__int64 pRenderView)
{
  wetlook::ScopedTime timed(wetlook::TimeWeather);
  alignas(16) float camera[16];
  bool haveCamera = pRenderView && ReadCamera(pRenderView, camera);
  // The game's time of day for the forecast and the looks' values by the
  // hour, before this lock (the visuals' own lock is taken first everywhere
  // else). (Before 2026-10-26 it was read only with Random rain on, so the
  // rainy look's hour stayed the last one known, noon at first.)
  float timeOfDay = -1.f;
  bool forecast = false;
  {
    std::lock_guard<std::mutex> lock(g_Mutex);
    forecast = (SnowChosen() ? g_Settings.RandomSnow : g_Settings.RandomRain) || LookWanted();
  }
  // The game paused or loading (see g_HeldSince)
  bool loadingNow = gamestate::Loading();
  bool held = gamestate::Paused() || loadingNow;
  if (forecast && g_mainHandle && g_mainHandle->GetVisualsManager())
    timeOfDay = g_mainHandle->GetVisualsManager()->GetCurrentTimeOfDay();
  float rainVolume = 0.f;
  float windVolume = 0.f, windStrength = 0.f, windInside = 0.f;

  struct Move { __int64 Instance; alignas(16) float Rows[16]; };
  Move moves[RainKinds + 2];
  int moveCount = 0;
  struct SoundWork
  {
    __int64 Release, Stale, Voice, Asset;
    bool Make, Place;
    int EventCount;
    unsigned int Events[MaxRainSoundEvents];
    alignas(16) float Rows[16];
  } sound{};
  struct StrikeWork { float Delay, Distance, Volume, DirX, DirZ; } strikes[2 * MostStrikes];
  int strikeCount = 0;
  {
    std::lock_guard<std::mutex> lock(g_Mutex);
    if (g_CodeState == 0)
      g_CodeState = IsExclusionCode((BYTE const*)g_gameHandle) ? 1 : -1;
    if (g_InstanceCode == 0)
    {
      g_InstanceCode = IsEffectInstanceCode((BYTE const*)g_gameHandle) ? 1 : -1;
      if (g_InstanceCode < 0)
        util::log::Warning("Weather: the game's effect instance code isn't what the tools know: no rain");
    }
    // How hard it rains: Rain at once, the forecast slowly
    {
      // (the steady clock: GetTickCount64 moves in steps of about 15.6 ms, so
      // at 100 frames a second and more the snow's clock stood still one
      // frame and jumped the next; see WetLook.cpp's drawn rain's clock)
      double ms = wetlook::TimeNow();
      float dt = g_IntensityMs > 0.0 ? (float)((ms - g_IntensityMs) / 1000.0) : 0.f;
      g_IntensityMs = ms;
      g_TimeOfDay = timeOfDay;
      if (timeOfDay >= 0.f)
        g_LookHours = timeOfDay / 3600.f;
      // Snow or rain, whichever is chosen (VISUALS > Snow turns the rain off
      // and the other way round): one intensity, one forecast for either
      bool snowChosen = SnowChosen();
      bool random = snowChosen ? g_Settings.RandomSnow : g_Settings.RandomRain;
      float chance = snowChosen ? g_Settings.SnowChance : g_Settings.RainChance;
      if (random)
      {
        if (g_ForecastChance != chance)
        {
          MakeForecast(chance);
          util::log::Write("Weather: forecast made (%d %s spells over the day, about %.0f %% of it)", (int)g_Spells.size(),
            snowChosen ? "snow" : "rain", 100.f * chance);
        }
        // No time of day (a loading screen, Faith respawning): the rain held
        // as it is, not eased off (it took the look and the fog with it)
        if (timeOfDay >= 0.f)
        {
          int spell = -1;
          float through = 0.f;
          float target = ForecastAt(timeOfDay, &spell, &through);
          // (rain: as hard as its spell rains, its storm over its stormy part,
          // coming and going over a tenth of the spell; see Spell)
          g_StormTarget = 0.f;
          if (!snowChosen && spell >= 0)
          {
            Spell const& sp = g_Spells[spell];
            target *= sp.Rain;
            g_SpellNow = sp;
            g_SpellKnown = true;
            float in = (std::min)((through - sp.StormFrom) / 0.1f, (sp.StormTo - through) / 0.1f);
            g_StormTarget = sp.Storm * (std::max)(0.f, (std::min)(1.f, in));
          }
          float step = (held ? 0.f : dt) / RainFadeSeconds;
          g_Intensity += (std::max)(-step, (std::min)(step, target - g_Intensity));
        }
      }
      else
        g_Intensity = (snowChosen ? g_Settings.Snow : g_Settings.Rain) ? 1.f : 0.f;
      bool on = g_Intensity > 0.01f && !g_shutdown;
      bool rainOn = on && !snowChosen, snowOn = on && snowChosen;
      if (rainOn != g_RainOn)
        util::log::Write("Weather: rain %s%s", rainOn ? "starts" : "stops", g_Settings.RandomRain ? " (the forecast)" : "");
      if (snowOn != g_SnowOn)
        util::log::Write("Weather: snow %s%s", snowOn ? "starts" : "stops", g_Settings.RandomSnow ? " (the forecast)" : "");
      g_RainOn = rainOn;
      g_SnowOn = snowOn;
      // (Random rain: as much as it rains of its spell's height, x the
      // spell's Look; see Spell)
      float lookShare = g_Intensity;
      if (g_Settings.RandomRain && !snowChosen && g_SpellKnown)
        lookShare = (std::min)(1.f, g_Intensity / (std::max)(g_SpellNow.Rain, 0.25f)) * g_SpellNow.Look;
      g_LookAmount = LookWanted() ? (g_Settings.RandomRain || g_Settings.RandomSnow ? lookShare : 1.f) : 0.f;
      UpdateSnow(held ? 0.f : dt);
      // Random rain's storm, eased (see Spell), and how wet its rain has made
      // the ground (see weather::GetWetting): the ground wet at the rain now
      // (g_Intensity) / GroundWetSeconds a second (all of it in a minute of a
      // downpour, four of a drizzle), up to 0.4 + 0.8 x it (a drizzle's 0.6);
      // the puddles filling at (it - 0.25) / PuddleFillSeconds (none in a
      // drizzle, full in four minutes of a downpour), up to (it - 0.2) / 0.6;
      // each drying when it's past that, or dry, over GroundDrySeconds and
      // PuddleDrySeconds. Real time; nothing while the game's paused or
      // loading.
      {
        float step = held ? 0.f : (std::max)(0.f, (std::min)(dt, 0.25f));
        float storm = g_Settings.RandomRain && !snowChosen ? g_StormTarget : 0.f;
        float ease = step / StormEaseSeconds;
        g_StormNow += (std::max)(-ease, (std::min)(ease, storm - g_StormNow));
        float rainNow = rainOn ? (std::min)(1.f, g_Intensity) : 0.f;
        float groundMost = rainNow > 0.f ? (std::min)(1.f, 0.4f + 0.8f * rainNow) : 0.f;
        if (g_GroundWet < groundMost)
          g_GroundWet = (std::min)(groundMost, g_GroundWet + step * rainNow / GroundWetSeconds);
        else
          g_GroundWet = (std::max)(groundMost, g_GroundWet - step / GroundDrySeconds);
        float puddleMost = (std::max)(0.f, (std::min)(1.f, (rainNow - 0.2f) / 0.6f));
        if (g_PuddleFill < puddleMost)
          g_PuddleFill = (std::min)(puddleMost, g_PuddleFill + step * (std::max)(0.f, rainNow - 0.25f) / PuddleFillSeconds);
        else
          g_PuddleFill = (std::max)(puddleMost, g_PuddleFill - step / PuddleDrySeconds);
      }
      if (UseToolsRainLoop && g_RainOn && g_Settings.RainSound)
        rainVolume = RainSoundLevel * (std::min)(1.f, g_Intensity) *
          (std::min)(1.5f, g_Settings.Strength / 0.7f * (g_Settings.Heavy ? 1.3f : 1.f));
    }
    bool rain = g_RainOn;
    ULONGLONG now = GetTickCount64();
    TrackInstances(rain && haveCamera, rain && g_Settings.RainSound && UseGameRainVoice && !g_RainSound.Asset, now);
    // (their streaks' camera boxes softened while it rains, see StreakBoxes)
    UpdateSoftBoxes(rain && haveCamera && !g_shutdown);
    UpdateFrozenWater(g_Settings.FreezeWater && g_SnowOn && !g_shutdown, now);
    if (rain && !g_Rain[0].Instance)
    {
      if (!g_LookingSince)
        g_LookingSince = now;
    }
    else
      g_LookingSince = 0;
    if (haveCamera)
    {
      static const float level[12] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0 };
      memcpy(g_Follow, level, sizeof(level));
      g_Follow[12] = camera[12];
      g_Follow[13] = camera[13];
      g_Follow[14] = camera[14];
      g_Follow[15] = 1.f;
    }
    g_HaveCamera = haveCamera;
    if (haveCamera)
    {
      memcpy(g_ListenerRight, camera + 0, sizeof(g_ListenerRight));
      memcpy(g_ListenerBack, camera + 8, sizeof(g_ListenerBack));
    }
    // The wind's sound while it snows (thunder::SetWind): louder and brighter
    // the harder it snows and the faster the snow's wind blows now (its gusts
    // included, three times in a blizzard), much more in a blizzard; inside
    // as the camera is under the roof squares' cover or in one of the game's
    // named interiors (the lair's roof isn't found by the squares)
    if (g_SnowOn && g_Settings.WindSound && !g_shutdown)
    {
      float speed = sqrtf(g_SnowWind[0] * g_SnowWind[0] + g_SnowWind[1] * g_SnowWind[1]);
      float share = SnowIntensityNow() / MostSnowIntensity;
      windStrength = (std::min)(1.f, 0.15f + 0.45f * share + speed / 30.f + (g_Settings.Blizzard ? 0.3f : 0.f));
      windVolume = WindSoundLevel * (std::min)(1.f, g_Intensity) * (0.35f + 0.45f * share + 0.25f * (std::min)(speed / 8.f, 1.5f)) *
        (g_Settings.Blizzard ? 1.6f : 1.f) * (std::max)(0.f, (std::min)(g_Settings.WindVolume, 2.f));
      windInside = (std::max)(g_RoofsKnown ? g_Inside : 0.f, haveCamera && InsideInterior(camera + 12, false) ? 1.f : 0.f);
    }
    // The wind with the rain (see RainWind): louder and brighter the harder it
    // rains, in slow gusts (two slow waves added up, as the snow's wind
    // wanders), inside as the snow's
    else if (g_RainOn && g_Settings.RainWind && !g_shutdown)
    {
      float amount = RainAmountNow();
      float tSec = (float)(GetTickCount64() % 100000000ULL) / 1000.f;
      float gust = 1.f + 0.3f * sinf(tSec / 9.7f) + 0.2f * sinf(tSec / 3.9f + 1.1f);
      windStrength = (std::min)(1.f, 0.15f + 0.55f * amount * gust);
      windVolume = WindSoundLevel * amount * (0.55f + 0.45f * gust) * (std::max)(0.f, (std::min)(g_Settings.RainWindAmount, 2.f));
      windInside = (std::max)(g_RoofsKnown ? g_Inside : 0.f, haveCamera && InsideInterior(camera + 12, false) ? 1.f : 0.f);
    }
    // In the lair with its windows shut no wind at all (see InShutLair): its
    // sound eased out over LairCalmSeconds (real time)
    {
      bool shut = haveCamera && InShutLair(camera + 12);
      double ms = wetlook::TimeNow();
      float step = g_LairCalmMs > 0.0 ? (float)((ms - g_LairCalmMs) / 1000.0) : 0.f;
      g_LairCalmMs = ms;
      step = (std::max)(0.f, (std::min)(step, 0.25f)) / LairCalmSeconds;
      g_LairCalm += (std::max)(-step, (std::min)(step, (shut ? 1.f : 0.f) - g_LairCalm));
      if (shut != g_InShutLair)
        util::log::Write("Weather: the camera is %s the lair with its windows shut: %s", shut ? "in" : "out of",
          shut ? "no wind there (its sound eased out, the game's wind not raised)" : "the wind as outside");
      g_InShutLair = shut;
      windVolume *= 1.f - g_LairCalm;
    }
    bool following = false;
    for (size_t k = 0; k < RainKinds; ++k)
    {
      GameRain& r = g_Rain[k];
      if (!r.Instance)
        continue;
      bool want = rain && haveCamera && (k == 0 || g_Settings.Heavy);
      if (want)
      {
        if (!r.Followed)
          util::log::Write("Weather: the game's %s follows the camera", r.Label);
        r.Followed = true;
        following = true;
        moves[moveCount].Instance = r.Instance;
        memcpy(moves[moveCount++].Rows, g_Follow, sizeof(g_Follow));
      }
      else if (r.Followed)
      {
        r.Followed = false;
        util::log::Write("Weather: the game's %s is back where the game had it", r.Label);
        moves[moveCount].Instance = r.Instance;
        memcpy(moves[moveCount++].Rows, r.Home, sizeof(r.Home));
      }
    }
    g_Following = following;

    // Kept out of buildings: while the tools' rain is on. The roofs round the
    // camera are also looked for for the rain the tools draw (only where the
    // sky is open) and the thunder (muffled inside)
    bool boxesWanted = following && g_Settings.KeepOut && g_CodeState == 1;
    bool roofsWanted = haveCamera && collision::Available() &&
      (boxesWanted || ThunderWanted() || (g_RainOn && g_Settings.RainSound) ||
      (g_RainOn && g_Settings.KeepOut && g_Settings.DistantRain > 0.f) || g_Wet.On ||
      (SnowChosen() && (g_SnowOn || g_SnowCover > 0.f)));
    if (roofsWanted)
      UpdateRoofs(camera, boxesWanted);
    else
    {
      if (!g_Slots.empty())
        GiveBackBoxes();
      g_CameraCovered = false;
      g_CameraRoofUp = 0.f;
      g_Inside = 0.f;
      g_RainGrid.clear();
      g_ShelterGrid.clear();
      g_DripEdges.clear();
    }
    g_RoofsKnown = roofsWanted;
    // The drainpipes' water (see UpdatePipes)
    UpdatePipes(camera, haveCamera && roofsWanted && g_RainOn && g_Wet.On && g_Settings.PipeWater && !g_shutdown, held, now);
    // The look's fog eases off indoors (see LookIndoorFogLeft)
    {
      ULONGLONG tick = GetTickCount64();
      float dt = g_LookIndoorsTick ? (tick - g_LookIndoorsTick) / 1000.f : 0.f;
      g_LookIndoorsTick = tick;
      float target = roofsWanted ? g_Inside : 0.f;
      g_LookIndoors += (target - g_LookIndoors) * (std::min)(1.f, dt / LookIndoorsSeconds);
    }

    // A world going (see g_WorldGoing): the rain's sound let go (below: no
    // asset, no sound wanted) and its asset forgotten before its world is
    {
      char const* going = g_WorldGoing.exchange(nullptr);
      if (!going && loadingNow && !g_WasLoading)
        going = "a loading screen";
      if (!going && g_RainZoneGone)
        going = "a rain of the game's gone";
      g_WasLoading = loadingNow;
      g_RainZoneGone = false;
      if (going)
      {
        g_RainSoundQuietUntil = now + RainSoundQuietMs;
        if (g_RainSound.Asset || g_RainSound.OwnVoice || g_RainSound.StaleVoice)
          util::log::Write("Weather: %s: the rain's sound let go and its asset forgotten before its world may go (none taken "
            "for %.0f s)", going, RainSoundQuietMs / 1000.0);
        g_RainSound.Asset = 0;
        g_RainSound.AssetVTable = 0;
        g_RainSound.GameVoice = 0;
        g_RainSound.EventCount = 0;
        g_WatchedVoice = 0;
        g_ColumnWakes = 0;
      }
    }
    // The rain sound's asset gone (its zone unloaded: Faith's death or a
    // loading screen makes the tools let the zones go): forgotten, so the one
    // the game loads next is taken, a rain column woken for it again (before,
    // the old pointer was kept and the sound never came back: 2026-10-25
    // 18:27:59 in the game's log)
    if (g_RainSound.Asset && !RainSoundAssetThere())
    {
      util::log::Write("Weather: the rain's sound asset is gone (its zone unloaded): the next one is waited for");
      g_RainSound.Asset = 0;
      g_RainSound.AssetVTable = 0;
      g_RainSound.GameVoice = 0;
      g_RainSound.EventCount = 0;
      g_WatchedVoice = 0;
      g_ColumnWakes = 0;
    }
    // The rain's sound: only while the rain follows the camera (so its zone
    // is loaded), the tools load the zones and its asset is still the one
    // the game made a voice for, once the game's first events are in (it
    // gives them over its first RainSoundEventsMs)
    bool soundWanted = UseGameRainVoice && following && g_Settings.RainSound && g_Settings.LoadZones && haveCamera &&
      RainSoundAssetThere() && g_RainSound.EventCount > 0 && now - g_RainSound.MadeTick > RainSoundEventsMs &&
      !g_shutdown;
    if (g_RainSound.StaleVoice)
    {
      sound.Stale = g_RainSound.StaleVoice;
      g_RainSound.StaleVoice = 0;
    }
    if (!soundWanted && g_RainSound.OwnVoice)
    {
      sound.Release = g_RainSound.OwnVoice;
      g_RainSound.OwnVoice = 0;
      util::log::Write("Weather: the rain's sound let go");
    }
    else if (soundWanted)
    {
      if (!g_RainSound.OwnVoice)
      {
        sound.Make = true;
        sound.Asset = g_RainSound.Asset;
        sound.EventCount = g_RainSound.EventCount;
        memcpy(sound.Events, g_RainSound.Events, sizeof(sound.Events));
      }
      sound.Voice = g_RainSound.OwnVoice;
      // Always straight over the camera, its cone (down, unturned) on it: in
      // the open 30 m over it; under a roof 18 m over it (at least 3 m over
      // the roof), nearer than in the open but, the roof being in between,
      // muffled by the sound's own damping (the output, a
      // ConeOutputNodeData, has HFDampingObstruction 0.9, HFDampingOcclusion 1
      // and a low pass; me_sound/world/shared/rain/spotrainopeninglarge). The
      // 20:04 build turned its cone to the nearest opening instead, and the
      // user heard it stop under a roof and not come back (2026-10-25 20:2x),
      // so it isn't turned any more, and after a stay under a roof a new voice
      // is made once out (the game may have stopped the old one).
      memcpy(sound.Rows, g_Follow, sizeof(g_Follow));
      sound.Rows[13] += RainSoundHeight;
      if (g_CameraCovered)
      {
        if (!g_RainSoundInsideSince)
          g_RainSoundInsideSince = now;
        sound.Rows[13] = camera[13] + (std::max)(RainSoundInsideNearest, g_CameraRoofUp + RainSoundOverRoof);
      }
      else if (g_RainSoundInsideSince)
      {
        bool remake = now - g_RainSoundInsideSince > RainSoundRemakeMs && g_RainSound.OwnVoice;
        g_RainSoundInsideSince = 0;
        if (remake)
        {
          sound.Release = g_RainSound.OwnVoice;
          g_RainSound.OwnVoice = 0;
          sound.Voice = 0;
          sound.Make = true;
          sound.Asset = g_RainSound.Asset;
          sound.EventCount = g_RainSound.EventCount;
          memcpy(sound.Events, g_RainSound.Events, sizeof(sound.Events));
          util::log::Write("Weather: out from under a roof: the rain's sound made again");
        }
      }
      sound.Place = true;
    }
    // No voice of the game's for the rain's sound yet: a rain column is
    // moved under the camera for a moment so the game makes one
    if (g_ColumnWoken && (now - g_ColumnWokenTick > ColumnWakeMs || g_RainSound.Asset || !following))
    {
      for (RainColumn const& c : g_Columns)
        if (c.Instance == g_ColumnWoken)
        {
          moves[moveCount].Instance = c.Instance;
          memcpy(moves[moveCount++].Rows, c.Home, sizeof(c.Home));
        }
      util::log::Write("Weather: the rain column moved for the sound is back (%s)",
        g_RainSound.Asset ? "the game made the rain's sound" : "no sound from it");
      g_ColumnWoken = 0;
    }
    else if (g_ColumnWoken && haveCamera && moveCount < (int)RainKinds + 1)
    {
      moves[moveCount].Instance = g_ColumnWoken;
      memcpy(moves[moveCount++].Rows, g_ColumnWakeRows, sizeof(g_ColumnWakeRows));
    }
    else if (UseGameRainVoice && !g_ColumnWoken && following && g_Settings.RainSound && !g_RainSound.Asset && !g_Columns.empty() &&
      now >= g_RainSoundQuietUntil &&
      g_ColumnWakes < MostColumnWakes && now - g_ColumnsSeenTick > ColumnWaitMs &&
      now - g_ColumnWokenTick > ColumnWaitMs && haveCamera && moveCount < (int)RainKinds + 1 && g_RoofsKnown)
    {
      // Only over a square known to be open (else tried again later)
      float x = 0.f, y = 0.f, z = 0.f;
      if (ColumnWakeSpot(camera, x, y, z))
      {
        static const float level[12] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0 };
        memcpy(g_ColumnWakeRows, level, sizeof(level));
        g_ColumnWakeRows[12] = x;
        g_ColumnWakeRows[13] = y;
        g_ColumnWakeRows[14] = z;
        g_ColumnWakeRows[15] = 1.f;
        g_ColumnWoken = g_Columns[g_ColumnWakes % g_Columns.size()].Instance;
        g_ColumnWokenTick = now;
        ++g_ColumnWakes;
        util::log::Write("Weather: no rain sound from the game yet: a rain column moved for a moment over a square open to the "
          "sky %.0f m from the camera (%.1f %.1f %.1f)", sqrtf((x - camera[12]) * (x - camera[12]) + (z - camera[14]) *
          (z - camera[14])), x, y, z);
      }
    }

    // Thunder and lightning: a strike every ThunderEvery on average (or when
    // asked), its flash now and its thunder after the light's head start.
    // With Random rain only in its storms (see Spell): as often as the storm
    // is strong (ThunderEvery / its rate, up to 4 x that as it comes or
    // goes); with Thunder and lightning ticked itself, whenever it rains, the
    // harder the more often
    float stormEvery = g_Settings.ThunderEvery;
    bool storm = ThunderWanted() && haveCamera && !g_shutdown;
    if (storm && g_Settings.RandomRain)
    {
      float stormy = g_Settings.Thunder ? (std::min)(1.f, g_Intensity) : g_StormNow;
      storm = g_RainOn && stormy > 0.05f;
      stormEvery /= (std::max)(0.25f, stormy);
    }
    // The Shard's tip is only looked for while lightning can come (see
    // g_ShardTopAsked): a burst due when the camera hook didn't ask for it
    // before this update waits for the next one
    bool tipAsked = g_ShardTopAsked;
    g_ShardTopAsked = false;
    g_StormWas = storm;
    // Paused or loading: none of it goes on; after, what was due waits as
    // long as the pause lasted (see g_HeldSince)
    if (held && !g_HeldSince)
    {
      g_HeldSince = now;
      util::log::Write("Weather: the game is paused or loading: the weather waits");
    }
    else if (!held && g_HeldSince)
    {
      ULONGLONG lasted = now - g_HeldSince;
      g_HeldSince = 0;
      util::log::Write("Weather: the game goes on after %.1f s: so does the weather", lasted / 1000.0);
      if (g_NextStrike)
        g_NextStrike += lasted;
      for (Strike& st : g_Strikes)
        if (!st.Started)
          st.Tick += lasted;
    }
    if (held)
    {
      // (nothing: g_NextStrike and a Strike now from the menu wait)
    }
    else if (storm)
    {
      if (!g_NextStrike)
        g_NextStrike = now + (ULONGLONG)(1000.f * stormEvery * (0.3f + 0.7f * Chance()));
      if ((g_StrikeNow || now >= g_NextStrike) && tipAsked)
      {
        g_StrikeNow = false;
        // The next burst after this one's last strike
        float burst = StartBurst(camera, now, -1.f);
        g_NextStrike = now + (ULONGLONG)(1000.f * (burst + stormEvery * (0.5f + Chance())));
      }
    }
    else
    {
      g_NextStrike = 0;
      if (g_StrikeNow && haveCamera && !g_shutdown && tipAsked)
      {
        // Strike now, with thunder and lightning off (or no rain for it): a burst anyway
        g_StrikeNow = false;
        StartBurst(camera, now, g_Settings.NearestStrike);
      }
    }
    // Each strike whose time has come: where it comes down (the tallest top
    // found round there, see FindStrikeTop), its bolt, its flash's strength
    // and its thunder (its flash and bolt are read from it as they're drawn).
    // Worked out as each strikes, so a burst's sweeps are spread over it.
    for (Strike& st : g_Strikes)
    {
      if (st.Started || st.Tick > now || g_shutdown || !haveCamera || held)
        continue;
      st.Started = true;
      // With no top found (no collision out there), the bolt goes on down to
      // below the city's ground: what's in front of it (the ground, the
      // buildings) hides its lower part, so it always meets something
      // (2026-10-26, the user: "lighting strikes should always hit the
      // ground/building and not be in the air": it stopped 60 m under the
      // camera's height, often in the air)
      float ground[3] = { camera[12] + st.DirX * st.Distance, (std::min)(camera[13] - 60.f, StrikeBelowGround),
        camera[14] + st.DirZ * st.Distance };
      float top[3];
      float nearest = (std::min)(g_Settings.NearestStrike, g_Settings.FarthestStrike);
      bool onTop = !st.OnShard && FindStrikeTop(camera, st.DirX, st.DirZ, st.Distance, nearest, top);
      // A crane round there goes first unless what was found is 20 m or more higher
      float craneTop[3];
      bool onCrane = !st.OnShard && FindCraneTop(camera, st.DirX, st.DirZ, st.Distance, nearest, craneTop) &&
        (!onTop || craneTop[1] >= top[1] - 20.f);
      if (onCrane)
      {
        memcpy(top, craneTop, sizeof(top));
        onTop = true;
      }
      if (st.OnShard)
      {
        memcpy(ground, st.Target, sizeof(ground));
        util::log::Write("Weather: lightning strikes the Shard's tip (%.0f %.0f %.0f), %.0f m away", ground[0], ground[1],
          ground[2], st.Distance);
      }
      if (onTop)
      {
        memcpy(ground, top, sizeof(ground));
        float x = ground[0] - camera[12], z = ground[2] - camera[14];
        st.Distance = (std::max)(nearest, sqrtf(x * x + z * z));
      }
      float closeness = 1.f - (std::min)(st.Distance / 3000.f, 1.f);
      st.Strength = (0.35f + 0.65f * closeness) * (0.7f + 0.3f * Chance());
      if (g_Settings.Bolts)
        MakeBolt(camera, ground, st.DirX, st.DirZ, st.Bolt);
      if (onTop)
        util::log::Write("Weather: lightning comes down on %s, %.0f m up (%.0f m over the camera), %.0f m away",
          onCrane ? "a crane's top" : "the tallest top found there", ground[1], ground[1] - camera[13], st.Distance);
      if (strikeCount < 2 * MostStrikes)
        strikes[strikeCount++] = { st.Distance / 343.f, st.Distance, g_Settings.ThunderVolume, st.DirX, st.DirZ };
    }
  }
  // Outside the lock: the game's code may play or stop effects, make voices
  for (int i = 0; i < moveCount; ++i)
    MoveInstance(moves[i].Instance, moves[i].Rows);
  if (sound.Release)
    ReleaseVoice(sound.Release);
  if (sound.Stale)
    ReleaseVoice(sound.Stale);
  if (sound.Make)
  {
    __int64 voice = MakeVoice(sound.Asset);
    if (voice)
    {
      PlaceVoice(voice, sound.Rows);
      // The game's events but its Stops, and a Start if it gave none (see
      // SoundEventStop)
      int given = 0, stops = 0;
      bool started = false;
      for (int i = 0; i < sound.EventCount; ++i)
      {
        if (sound.Events[i] == SoundEventStop)
        {
          ++stops;
          continue;
        }
        GiveVoiceEvent(voice, sound.Events[i]);
        started |= sound.Events[i] == SoundEventStart;
        ++given;
      }
      if (!started)
      {
        GiveVoiceEvent(voice, SoundEventStart);
        ++given;
      }
      std::lock_guard<std::mutex> lock(g_Mutex);
      g_RainSound.OwnVoice = voice;
      util::log::Write("Weather: the rain's sound plays at the camera (voice 0x%I64X, %d events given of the game's %d: "
        "%d Stop left out%s)", voice, given, sound.EventCount, stops, started ? "" : ", a Start added");
    }
  }
  else if (sound.Place && sound.Voice)
    PlaceVoice(sound.Voice, sound.Rows);
  for (int i = 0; i < strikeCount; ++i)
  {
    thunder::Play(strikes[i].Delay, strikes[i].Distance, strikes[i].Volume, strikes[i].DirX, strikes[i].DirZ);
    util::log::Write("Weather: lightning %.0f m away, its thunder in %.1f s", strikes[i].Distance, strikes[i].Delay);
  }
  thunder::SetRain(rainVolume);
  thunder::SetWind(windVolume, windStrength, windInside);
}

void weather::WorldGoing(char const* why)
{
  if (why)
    g_WorldGoing.store(why);
}

weather::State weather::GetState()
{
  std::lock_guard<std::mutex> lock(g_Mutex);
  return { g_Settings.Rain, g_Settings.Heavy, g_Settings.KeepOut, g_Settings.LoadZones };
}

void weather::SetState(State const& state)
{
  std::lock_guard<std::mutex> lock(g_Mutex);
  g_Settings.Rain = state.Rain;
  g_Settings.Heavy = state.Heavy;
  g_Settings.KeepOut = state.KeepOut;
  g_Settings.LoadZones = state.LoadZones;
}

bool weather::IsRaining(bool* heavy)
{
  std::lock_guard<std::mutex> lock(g_Mutex);
  if (heavy)
    *heavy = g_Rain[1].Followed;
  return g_Rain[0].Followed || g_Rain[1].Followed;
}

// The rain's strength (2026-10-25, offline). Every streak and droplet emitter
// of the rain scales its spawn rate by the global effect parameter
// EmitterParameter3 (fx/globaleffectparameterlist). The emitter system takes
// it every frame (0x1430FA9B0, from 0x1430E30B0) from the state of the visual
// environment manager's ([0x142568CD8]) view with ids 0, 0 (0x1436FC5E0: the
// view's [+ 0x550], which its constructor 0x1436E0330 points at its own
// + 0x560; with no such view, the manager's + 0x60): the Values of the
// state's three EmitterParam slots (+ 0x12A0, 0x1C each: + 0 Realm, + 4
// Parameter, + 8 Value) are EmitterParameter1 - 3 (emitter system + 0x146D0).
// When they change it hands them to every emitter that reads them
// (0x1430FA0C0, from its update 0x1430F3A50), and an emitter made later gets
// them as it's made (0x1430FAF30). A visual environment's EmitterParamComponent
// fills the slot of its Parameter (0x14328D5D1). In the city the always-on
// ve_tod_posteffects gives slot 2 (EmitterParameter3) 0, so the rain the
// tools keep at the camera spawned nothing; the Release mission's
// ve_release_01 - 04 give it 0.7, 0.5, 0.12 and 0, its interiors 0. The mix
// (0x143703D60, the hook's) writes the slots only when the view is mixed again
// (its + 0x3F00), so right after each mix of that view the tools give slot 2
// the menu's strength while Rain is on; when it's off, the game's value back
// and the view mixed again (true).
namespace
{
  // The rainy city look (2026-10-25 20:xx: moved here from the visual
  // environment update). The world renderer copies the mixed state
  // (0x14373E663) and right after updates the physical sky and the sun from
  // that copy (0x14373E74D -> 0x1436FAB20), before the tools' writes to its
  // copy land; so values the sky and the sun are made from (the sky's
  // scattering, its OutdoorLightScale and DrawSunDisc) never took: the 20:16
  // test still had a sun with shadows in the morning. Given here, in the
  // mixed state right after each mix (as the rain's strength is), every step
  // after sees them. The fields are the state's (SkyComponentData's and the
  // others' data offsets minus 0x80, from their type info).
  //
  // No sun: the time of day's logic (pf_tod_sunmooncelectialandoutdoorlightlogic,
  // TimeOfDaySkyAndCelestialLogicEntityData) drives the sky's OutdoorLightScale
  // (0 at night, 0.6 from 7:00 to 17:00), Light1Intensity (200, the sky's own
  // sun) and DrawSunDisc: the sun's light on the city is the sky's sun times
  // OutdoorLightScale, so that going to 0 takes the direct light and its
  // shadows away while the sky stays lit; DrawSunDisc off hides its disk,
  // MieG towards 0 its glow (the haze scatters evenly).
  //
  // But with the physical sky the mix works out the sun's final light itself
  // at its end (0x1437050D4 - 0x1437051A3: with the sky block's + 0x270 set
  // and SkyType (+ 0x484) 2 it calls 0x143729430, which fills the outdoor
  // light's FinalSunLuminance + 0x10 / FinalSunIlluminance + 0x20 and the
  // outer space ones from the sky's own sun; only otherwise from SunColor x
  // SunIntensity), so OutdoorLightScale 0 left the sun's light as it was (the
  // 20:37 test: read live, FinalSunIlluminance 79 58 46 with OutdoorLightScale 0
  // in both states). Given right after the mix, the final values stay until
  // the next mix: they're taken down too. For the overcast look, also more
  // bloom (Tonemap BloomScale + 0x1D0) and heavier, flatter-lit clouds.
  //
  // Fog everywhere: the sky's MinHeightFogTransmittance (+ 0x24C, read by the
  // fog set-up at 0x143706B24) is a floor on how much the fog lets through:
  // 0.37 at 6:00, 0.6 at 7:00, 0.73 at noon, 0 at 17:00 (the time of day's
  // environments), so in the morning and at noon the fog could hardly be
  // seen (the user, 20:2x: "the fog went away"). The game's own foggy
  // environments (ve_skycity_fog_01 / 02) set it to 0; the look takes it to a
  // tenth, as TheNollz' Rain mod does.
  //
  // TheNollz' Rain mod (Rain.fbmod 2.0.0, read 2026-10-25 21:0x, every asset
  // against the game's own; Research_Weather section 14): its time of day
  // uses eight VE_ToD_*_Rain_Overcast copies of the game's VE_ToD_* in their
  // places, plus VE_Rain_Global (EmitterParameter3 0.7, shader parameter
  // RainShaderParam 0.2, as the Release mission's ve_release_01). Against the
  // game's, the copies change only: the sky's Rayleigh scattering to its own
  // average (1.75e-5 for all three colours), HeightFogColorAdd to a grey (its
  // average at 6:00 and noon, x 0.85 at 18:00, x 0.75 at 20:00, x 0.4 at 4:00,
  // x 0.25 at midnight; 0 at 7:00 and 17:00 as the game's), forward
  // scattering from 300 to 7500 m by day (150 - 3750 m at night, the game's
  // 0 - 15000), UseOzone off, MinHeightFogTransmittance a tenth (0.0849 at
  // 17:00 where the game's is 0), height fog 2000 m deep seen through half
  // the game's by day (3000 m), 1625 m at 6:00 / 18:00 and 250 m at night,
  // the sun flare's elements and the lens reflections off. Its sun logic sets
  // OutdoorLightScale to 0 all day (the game's 0.6), the moon 0.2 (the game's
  // 3) and the sun's angular radius to a twentieth; its sun rotation
  // environments switch the sky's celestial components (sun, moon, stars)
  // off. It leaves bloom, clouds, colour and the exposure as the game's. The
  // look does the same where the tools know the state's place (the celestial
  // and lens reflection blocks aren't known: the moon and the stars stay),
  // and on top more bloom and heavier, flatter-lit clouds.
  enum LookFieldIndex
  {
    LookSkyLuminance, LookSkyLight, LookSunIntensity, LookSunColor, LookSunDisk, LookFogEnable, LookHeightFogEnable,
    LookHeightFogDepth, LookHeightFogSeen, LookHeightFogAdd, LookHeightFogMult, LookMinFogTransmittance, LookRayleigh,
    LookMie, LookMieG, LookOutdoorLightScale, LookDrawSunDisc, LookCloudOpacity1, LookCloudOpacity2, LookCloudSunColor,
    LookCloudColor1, LookCloudColor2, LookSaturation, LookSunFlare, LookFinalSunLuminance, LookFinalSunIlluminance, LookBloom,
    LookCloudThickness1, LookCloudThickness2, LookCloudSunLight1, LookCloudSunLight2, LookCloudAmbient1, LookCloudAmbient2,
    LookScatterStart, LookScatterEnd, LookOzone, LookFogAltitude,
    LookFieldCount
  };
  struct LookField { int Offset; int Floats; bool Byte; };   // state offset; floats (or one byte)
  const LookField LookFields[LookFieldCount] = {
    { 0x390 + 0xF8, 1, false },    // Sky: LuminanceScale
    { 0x000 + 0x60, 3, false },    // Outdoor light: SkyColor
    { 0x000 + 0x94, 1, false },    // Outdoor light: SunIntensity
    { 0x000 + 0x00, 3, false },    // Outdoor light: SunColor
    { 0x000 + 0x9C, 1, false },    // Outdoor light: SunDiskIntensity
    { 0x620 + 0x9C, 1, true },     // Fog: Enable
    { 0x620 + 0xA0, 1, true },     // Fog: HeightFogEnable
    { 0x620 + 0x94, 1, false },    // Fog: HeightFogDepth
    { 0x620 + 0x98, 1, false },    // Fog: HeightFogVisibilityRange
    { 0x390 + 0xD0, 3, false },    // Sky: HeightFogColorAdd
    { 0x390 + 0xE0, 3, false },    // Sky: HeightFogColorMult
    { 0x390 + 0x24C, 1, false },   // Sky: MinHeightFogTransmittance
    { 0x390 + 0x30, 3, false },    // Sky: RayleighScatteringCoefficient
    { 0x390 + 0x1F4, 1, false },   // Sky: MieScatteringCoefficient
    { 0x390 + 0x1F8, 1, false },   // Sky: MieG
    { 0x390 + 0xA0, 3, false },    // Sky: OutdoorLightScale
    { 0x390 + 0x279, 1, true },    // Sky: DrawSunDisc
    { 0x390 + 0x170, 1, false },   // Sky: CloudLayer1AlphaMul
    { 0x390 + 0x1AC, 1, false },   // Sky: CloudLayer2AlphaMul
    { 0x390 + 0x00, 3, false },    // Sky: CloudLayerSunColor
    { 0x390 + 0x10, 3, false },    // Sky: CloudLayer1Color
    { 0x390 + 0x20, 3, false },    // Sky: CloudLayer2Color
    { 0x330 + 0x20, 3, false },    // Color correction: Saturation (only while it's on, + 0x44)
    { 0x710 + 0x234, 1, true },    // Sun flare: Enable (SunFlareComponentData + 0x2B4)
    { 0x000 + 0x10, 3, false },    // Outdoor light: FinalSunLuminance (worked out by the mix)
    { 0x000 + 0x20, 3, false },    // Outdoor light: FinalSunIlluminance (worked out by the mix)
    { 0x1D0 + 0x00, 3, false },    // Tonemap: BloomScale
    { 0x390 + 0x18C, 1, false },   // Sky: CloudLayer1Thickness
    { 0x390 + 0x1C4, 1, false },   // Sky: CloudLayer2Thickness
    { 0x390 + 0x164, 1, false },   // Sky: CloudLayer1SunLightIntensity
    { 0x390 + 0x1A0, 1, false },   // Sky: CloudLayer2SunLightIntensity
    { 0x390 + 0x16C, 1, false },   // Sky: CloudLayer1AmbientLightIntensity
    { 0x390 + 0x1A8, 1, false },   // Sky: CloudLayer2AmbientLightIntensity
    { 0x390 + 0x240, 1, false },   // Sky: ForwardScatteringStartDepth
    { 0x390 + 0x244, 1, false },   // Sky: ForwardScatteringEndDepth
    { 0x390 + 0x273, 1, true },    // Sky: UseOzone
    { 0x620 + 0x90, 1, false },    // Fog: HeightFogAltitude (the snowy look's; the fog set-up reads it, see WetLook.cpp)
  };
  const int ColorCorrectionEnableOffset = 0x330 + 0x44;
  const int LookFloats = 72;
  const int MostLookViews = 8;
  const int VeViewStateOffsetForLook = 0x550;

  // Per view the game mixes (each its own state)
  struct LookTrack
  {
    __int64 State;
    ULONGLONG Seen;   // last mixed
    bool Applied;
    float Base[LookFloats];
    float Written[LookFloats];
    // The look last given was the snowy one (for the take-back's line: by then
    // the look given to GiveLook may say rainy, its Snowy gone with the snow)
    bool Snowy;
  };
  LookTrack g_LookViews[MostLookViews]{};   // game thread
  bool g_LookLogged = false;

  // A field's first float among a view's Base / Written
  int LookFloatIndex(int field)
  {
    int k = 0;
    for (int f = 0; f < field; ++f)
      k += LookFields[f].Floats;
    return k;
  }

  // The Rain mod's overcast environments against the game's, at the time of
  // day's eight hours (see above), in between linear (the game's own blend
  // of two neighbouring hours)
  const float ModHours[8] = { 0.f, 4.f, 6.f, 7.f, 12.f, 17.f, 18.f, 20.f };
  const float ModFogSeen[8] = { 0.05f, 0.05f, 0.40625f, 0.5f, 0.5f, 0.5f, 1.f, 0.05f };   // x the game's (1625 / 1500 at 18:00 kept at 1)
  const float ModFogGrey[8] = { 0.25f, 0.4f, 1.f, 1.f, 1.f, 1.f, 0.85f, 0.75f };         // x the fog colour's average
  const float ModScatterStart[8] = { 150.f, 150.f, 300.f, 300.f, 300.f, 300.f, 150.f, 150.f };
  const float ModScatterEnd[8] = { 3750.f, 3750.f, 7500.f, 7500.f, 7500.f, 7500.f, 3750.f, 3750.f };

  // The snowy look (VISUALS > Snow): Toffiklubitoffi's Snowfall mod
  // (snowfall.fbmod 1.0, read 2026-10-26, every asset against the game's own;
  // Research_Snow_2026-10-26.md) changes four of the time of day's
  // environments, VE_ToD_0000 / 0600 / 0700 / 1200. Its values there, the
  // game's own where it left a field (6:00's depth, 7:00's altitude, midnight's
  // MinHeightFogTransmittance), in between linear round the clock (noon
  // straight on to midnight: the mod leaves 17:00, 18:00 and 20:00 as the
  // game's, which would bring the game's clear evening back while it snows).
  const float SnowHours[4] = { 0.f, 6.f, 7.f, 12.f };
  const float SnowFogSeen[4] = { 250.f, 6000.f, 5000.f, 500.f };     // HeightFogVisibilityRange (m)
  const float SnowFogAltitude[4] = { 30.f, 50.f, 0.f, 350.f };       // HeightFogAltitude (m)
  const float SnowFogDepth[4] = { 90.f, 200.f, 150.f, 300.f };       // HeightFogDepth (m)
  const float SnowFogFloor[4] = { 0.f, 0.6f, 0.7f, 0.f };            // MinHeightFogTransmittance
  const float SnowFogAdd[3][4] = {                                   // HeightFogColorAdd (r, g, b)
    { 0.6f, 0.5f, 0.6f, 1.f }, { 0.65f, 0.5f, 0.6f, 1.f }, { 0.7f, 0.5f, 0.6f, 1.f } };

  float BySnowHour(float const (&values)[4], float hours)
  {
    hours = fmodf(hours, 24.f);
    if (hours < 0.f)
      hours += 24.f;
    for (int i = 0; i < 4; ++i)
    {
      float from = SnowHours[i], to = i < 3 ? SnowHours[i + 1] : 24.f;
      if (hours >= from && hours <= to)
        return values[i] + (values[i < 3 ? i + 1 : 0] - values[i]) * (hours - from) / (to - from);
    }
    return values[3];
  }

  float ByHour(float const (&values)[8], float hours)
  {
    hours = fmodf(hours, 24.f);
    if (hours < 0.f)
      hours += 24.f;
    for (int i = 0; i < 8; ++i)
    {
      float from = ModHours[i], to = i < 7 ? ModHours[i + 1] : 24.f;
      if (hours >= from && hours <= to)
        return values[i] + (values[i < 7 ? i + 1 : 0] - values[i]) * (hours - from) / (to - from);
    }
    return values[4];
  }

  // A color made t of the way to its own grey (its average)
  void Greyer(float* v, float t)
  {
    float grey = (v[0] + v[1] + v[2]) / 3.f;
    for (int i = 0; i < 3; ++i)
      v[i] += (grey - v[i]) * t;
  }

  __int64 ViewState(__int64 view)
  {
    __try
    {
      __int64 state = *(__int64 const*)(view + VeViewStateOffsetForLook);
      return state >= 0x10000 ? state : 0;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      return 0;
    }
  }

  bool CopyFromState(void* to, BYTE const* from, size_t size)
  {
    __try
    {
      memcpy(to, from, size);
      return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      return false;
    }
  }

  bool CopyToState(BYTE* to, void const* from, size_t size)
  {
    __try
    {
      memcpy(to, from, size);
      return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      return false;
    }
  }

  // The look given to (or taken back from) one view's mixed state, amount
  // 0 - 1 of it: on top of what's there (a value the game wrote since is the
  // new one under it); off, each value back where ours still is. True when it
  // was taken back (the view is then mixed again).
  bool GiveLook(LookTrack& t, __int64 state, weather::RainyLook const& look, float amount)
  {
    bool active = look.On && amount > 0.f;
    if (!active && !t.Applied)
      return false;
    if (t.Applied && t.State != state)
      t.Applied = false;   // a new state: what was written is gone with the old
    BYTE* pState = (BYTE*)state;
    BYTE ccEnable = 0;
    if (!CopyFromState(&ccEnable, pState + ColorCorrectionEnableOffset, 1))
      return false;
    float a = (std::max)(0.f, (std::min)(amount, 1.f));
    float grey = (std::max)(0.f, (std::min)(look.GreySky, 1.f)) * a;
    float lessSun = (std::max)(0.f, (std::min)(look.LessSun, 1.f)) * a;
    float hours = look.Hours >= 0.f ? look.Hours : 12.f;
    // Indoors the fog is mostly left as the game has it (the user, 2026-10-25
    // 23:5x: "make the fog not enter buildings or if it does it's not a lot",
    // "i mean indoors"): the game's height fog has no idea of buildings, so
    // the look's thick fog filled rooms too (at night, 250 m seen through,
    // about a quarter of a 20 m room fogged). As the game does with its own
    // interior visual environments, the look's fog is taken down while the
    // camera is under a roof: well inside to LookIndoorFogLeft of it, eased
    // over about a second each way.
    const float LookIndoorFogLeft = 0.2f;
    float fogA = a * (1.f - (1.f - LookIndoorFogLeft) * (std::max)(0.f, (std::min)(1.f, look.Indoors)));
    // The snowy look (VISUALS > Snow): the fog goes to the Snowfall mod's
    // values by the hour (BySnowHour), never seen through farther than the
    // game's own nor its floor higher (the mod's dawn is clearer than the
    // game's; snow doesn't clear the air), as much as Snow fog says; a whiter
    // fog, a brighter sky light (the snow bounces it back up), lighter clouds,
    // less bloom than the rainy look; the rest as the rainy look
    bool snowy = look.Snowy;
    float snowFog = fogA * (std::max)(0.f, (std::min)(look.SnowFog, 1.f));
    // (past 1, Snow fog makes it that many times thicker, up to 10: seen
    // through that much less, reaching that much higher, its floor (the
    // least it lets through) that much lower; a blizzard's x 1.5 more.
    // 2026-10-26, the user: "Slider fog should let you add more fog", then
    // "fog slider should make it more foggy move it up to 10": up to 3 it was,
    // and the floor stayed, so at dawn 60 - 70 % of what's behind it always
    // showed through however thick it was)
    float thicker = (std::max)(1.f, (std::min)(look.SnowFog, 25.f)) * (look.Blizzard ? 1.5f : 1.f);
    int k = 0;
    for (int f = 0; f < LookFieldCount; ++f)
    {
      LookField const& field = LookFields[f];
      if (k + field.Floats > LookFloats)
        break;
      float* base = t.Base + k;
      float* written = t.Written + k;
      k += field.Floats;
      float now[3] = {};
      BYTE byteNow = 0;
      if (field.Byte)
      {
        if (!CopyFromState(&byteNow, pState + field.Offset, 1))
          return false;
        now[0] = byteNow ? 1.f : 0.f;
      }
      else if (!CopyFromState(now, pState + field.Offset, field.Floats * sizeof(float)))
        return false;
      bool changed = !t.Applied;
      for (int i = 0; i < field.Floats; ++i)
        changed |= now[i] != written[i];
      if (changed)
        memcpy(base, now, sizeof(now[0]) * field.Floats);
      if (!active)
      {
        // Off: the value under it back where ours still is
        if (!changed)
        {
          if (field.Byte)
          {
            BYTE b = base[0] > 0.5f ? 1 : 0;
            CopyToState(pState + field.Offset, &b, 1);
          }
          else
            CopyToState(pState + field.Offset, base, field.Floats * sizeof(float));
        }
        continue;
      }
      float v[3] = {};
      memcpy(v, base, sizeof(v[0]) * field.Floats);
      switch (f)
      {
      case LookSkyLuminance: v[0] *= 1.f - (snowy ? 0.f : 0.25f) * grey; break;
      case LookSkyLight:
        Greyer(v, 0.8f * grey);
        // (x 1 + 0.3 less sun, as the rainy look's: the snowy look's 0.5 was
        // for the snow's light bounced back up, but the game's own lighting
        // doesn't see the tools' snow, so it only brightened everything)
        for (float& c : v) c *= 1.f + 0.3f * lessSun;
        break;
      case LookSunIntensity: v[0] *= 1.f - lessSun; break;
      case LookSunColor: Greyer(v, 0.6f * lessSun); break;
      case LookSunDisk: v[0] *= 1.f - lessSun; break;
      case LookFogEnable:
      case LookHeightFogEnable: v[0] = 1.f; break;
      case LookHeightFogDepth:
        if (snowy)
          v[0] += (BySnowHour(SnowFogDepth, hours) * thicker - v[0]) * snowFog;
        else
          v[0] += ((std::max)(v[0], look.FogDepth) - v[0]) * fogA;
        break;
      case LookHeightFogSeen:
        if (snowy)
        {
          // (the Snowfall mod's by the hour, / how much thicker; a blizzard's
          // 120 m: in a real one you see 100 - 400 m, often less. Not the
          // intensity's any more: it was seen through no farther than 1500 m
          // / it, the user, 2026-10-26: "snow intensity shouldn't add any
          // fog it was a bad idea make a diffrent slider for that"; Snow fog
          // is the look's own)
          float seen = BySnowHour(SnowFogSeen, hours) / thicker;
          if (look.Blizzard)
            seen = (std::min)(seen, 120.f);
          v[0] += ((std::min)((std::min)(v[0], seen), look.FogSeenUpTo) - v[0]) * snowFog;
        }
        else
          v[0] += ((std::min)(v[0] * ByHour(ModFogSeen, hours) / (std::max)(look.FogThickness, 0.25f), look.FogSeenUpTo) -
            v[0]) * fogA;
        break;
      case LookHeightFogAdd:
      {
        Greyer(v, grey);
        if (snowy)
        {
          // (the Snowfall mod's hue, whiter than the game's, but as bright
          // as the game's own at the hour, x 1.15, and as much as Snow fog
          // says: the mod's own colours are 5 - 8 times the game's at night
          // and dawn (midnight's 0.05 0.08 0.31 -> 0.6 0.65 0.7), made for its
          // own darker exposure (night compensation 1.5 -> 0.5 EV, dawn and
          // dusk MinEV 0.4 -> 1.2), which the tools don't give; given as they
          // were, the city went bright grey: the user, 2026-10-26, "ticking
          // snow city look makes everything really bright")
          float hue[3], hueLuma = 0.f, gameLuma = 0.2126f * base[0] + 0.7152f * base[1] + 0.0722f * base[2];
          for (int i = 0; i < 3; ++i)
            hue[i] = BySnowHour(SnowFogAdd[i], hours);
          hueLuma = 0.2126f * hue[0] + 0.7152f * hue[1] + 0.0722f * hue[2];
          for (int i = 0; i < 3; ++i)
            v[i] += (hue[i] / (std::max)(hueLuma, 1e-3f) * (std::max)(gameLuma, 0.f) * 1.15f - v[i]) * snowFog;
          break;
        }
        float scale = 1.f + (ByHour(ModFogGrey, hours) - 1.f) * grey;
        for (float& c : v) c *= scale;
        break;
      }
      case LookHeightFogMult:
      case LookRayleigh: Greyer(v, grey); break;
      case LookMinFogTransmittance:
        if (snowy)
          v[0] += ((std::min)(v[0], BySnowHour(SnowFogFloor, hours) / thicker) - v[0]) * snowFog;
        else
          v[0] *= 1.f - 0.9f * fogA;
        break;
      case LookFogAltitude:
        if (snowy)
          v[0] += (BySnowHour(SnowFogAltitude, hours) - v[0]) * snowFog;
        break;
      case LookMie: v[0] *= 1.f + grey; break;
      case LookMieG: v[0] *= 1.f - lessSun; break;
      case LookOutdoorLightScale:
        for (float& c : v) c *= 1.f - lessSun;
        break;
      case LookDrawSunDisc:
      case LookSunFlare:
        if (lessSun >= 0.5f)
          v[0] = 0.f;
        break;
      case LookCloudOpacity1:
      case LookCloudOpacity2: v[0] += (1.f - v[0]) * (std::min)(1.f, 1.5f * grey); break;
      case LookCloudSunColor:
        Greyer(v, grey);
        for (float& c : v) c *= 1.f - (snowy ? 0.15f : 0.4f) * grey;
        break;
      case LookCloudColor1:
      case LookCloudColor2:
        Greyer(v, grey);
        for (float& c : v) c *= 1.f - (snowy ? 0.1f : 0.3f) * grey;
        break;
      case LookSaturation:
        if (ccEnable)
          for (float& c : v) c *= 1.f - 0.3f * grey;
        break;
      case LookFinalSunLuminance:
      case LookFinalSunIlluminance:
        for (float& c : v) c *= 1.f - lessSun;
        break;
      case LookBloom:
        for (float& c : v) c *= 1.f + (snowy ? 1.f : 2.f) * grey;
        break;
      case LookCloudThickness1:
      case LookCloudThickness2: v[0] *= 1.f + grey; break;
      case LookCloudSunLight1:
      case LookCloudSunLight2: v[0] *= 1.f - 0.7f * lessSun; break;
      case LookCloudAmbient1:
      case LookCloudAmbient2: v[0] *= 1.f + grey; break;
      // (the Rain mod's; the Snowfall mod leaves them as the game's)
      case LookScatterStart:
        if (!snowy)
          v[0] += (ByHour(ModScatterStart, hours) - v[0]) * a;
        break;
      case LookScatterEnd:
        if (!snowy)
          v[0] += (ByHour(ModScatterEnd, hours) - v[0]) * a;
        break;
      // (the ozone off in the snowy look too, 2026-10-27, the user: "also
      // everything still looks purple". Both looks grey the sky's Rayleigh
      // scattering, and ozone takes out mostly green and some red, little
      // blue, which the blue scattering hides in a clear sky. Left on with
      // the scattering grey, the sky, the fog's light and the sky's light on
      // everything went magenta. The rainy look took it out from the first,
      // as the Rain mod does. Not checked in game.)
      case LookOzone:
        if (a >= 0.5f)
          v[0] = 0.f;
        break;
      }
      bool ok;
      if (field.Byte)
      {
        BYTE b = v[0] > 0.5f ? 1 : 0;
        ok = CopyToState(pState + field.Offset, &b, 1);
      }
      else
        ok = CopyToState(pState + field.Offset, v, field.Floats * sizeof(float));
      if (!ok)
        return false;
      memcpy(written, v, sizeof(v[0]) * field.Floats);
    }
    if (active && !t.Applied && !g_LookLogged)
    {
      g_LookLogged = true;
      util::log::Write("Weather: %s city look given to the visual environment (state 0x%I64X): the sun's light x %.2f "
        "(its final illuminance %.1f -> %.1f), its disk %s, the fog's floor %.2f -> %.2f, height fog %.0f -> %.0f m deep, "
        "seen through %.0f -> %.0f m", snowy ? "snowy" : "rainy", state, 1.f - lessSun,
        t.Base[LookFloatIndex(LookFinalSunIlluminance)],
        t.Written[LookFloatIndex(LookFinalSunIlluminance)], lessSun >= 0.5f ? "off" : "on",
        t.Base[LookFloatIndex(LookMinFogTransmittance)],
        t.Written[LookFloatIndex(LookMinFogTransmittance)], t.Base[LookFloatIndex(LookHeightFogDepth)],
        t.Written[LookFloatIndex(LookHeightFogDepth)], t.Base[LookFloatIndex(LookHeightFogSeen)],
        t.Written[LookFloatIndex(LookHeightFogSeen)]);
    }
    bool takenBack = !active && t.Applied;
    if (active)
      t.Snowy = snowy;
    t.Applied = active;
    t.State = state;
    if (takenBack)
    {
      g_LookLogged = false;
      util::log::Write("Weather: %s city look taken back (state 0x%I64X), the game's values back", t.Snowy ? "snowy" :
        "rainy", state);
    }
    return takenBack;
  }

  // The game's own wet look (2026-10-25 21:3x, offline): the Release mission's
  // ve_release_01 and TheNollz' Rain mod's VE_Rain_Global give the shader
  // parameter RainShaderParam 0.2 (ve_release_02 0.05, _03 / _04 0) through a
  // ShaderParamsComponent. The mix gathers the visible environments' shader
  // parameters by name (0x143704E0B - 0x143704F62: the names compared as
  // text, a byte loop; at most 16, each blended by the component's mode) and
  // then writes them to the state's list (0x143704FEE - 0x143705054): + 0xFA0,
  // 0x30 each, the value (4 floats) at + 0x00 and the name's pointer at
  // + 0x18, nothing else; the count at + 0x1A44, set to 0 first. The pointer
  // kept is the first environment's own string, so whatever reads the list
  // can't depend on which string it is, only on its text. No code was found
  // reading + 0x1A44 / + 0xFA0 (nor the renderer copy's + 0x86EB4 / + 0x86410,
  // nor + 0xAA4 from the list) by offset: the renderer gets the state whole
  // (the view's copy, 0x14373E670). So while it rains the tools add the entry
  // to each view's list right after its mix, as the mix would, unless the game
  // has its own RainShaderParam there (then it's left as it is); the value
  // follows the rain each frame; when it's not wanted the view is mixed again,
  // which makes the list anew without it. The name is a string of the tools'
  // own on the process heap, never freed. What it changes on the city's
  // materials is not known (not checked in game).
  const int ShaderParamListOffset = 0xFA0;
  const int ShaderParamSize = 0x30;
  const int ShaderParamNameOffset = 0x18;
  const int ShaderParamCountOffset = 0x1A44;
  const unsigned int MostShaderParams = 16;
  const char RainShaderParamName[] = "RainShaderParam";
  char* g_RainShaderParamName = nullptr;   // the tools' copy (game thread)
  std::atomic<int> g_WetParamState{ 0 };   // 0 not given, 1 given, 2 the game's own there, 3 the list full or unknown
  int g_WetParamLogged = 0;               // the results logged (bits), from when it was last wanted

  bool NameIs(char const* name, char const* text)
  {
    __try
    {
      return strcmp(name, text) == 0;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
      return false;
    }
  }

  // Gives (value >= 0) or takes out (value < 0) the tools' RainShaderParam in
  // one view's mixed state. True when the view should be mixed again.
  bool GiveRainShaderParam(__int64 state, float value)
  {
    if (!g_RainShaderParamName)
    {
      if (value < 0.f)
        return false;
      g_RainShaderParamName = (char*)HeapAlloc(GetProcessHeap(), 0, sizeof(RainShaderParamName));
      if (!g_RainShaderParamName)
        return false;
      memcpy(g_RainShaderParamName, RainShaderParamName, sizeof(RainShaderParamName));
    }
    BYTE* s = (BYTE*)state;
    unsigned int count = 0;
    if (!CopyFromState(&count, s + ShaderParamCountOffset, sizeof(count)))
      return false;
    int result = 3;
    bool remix = false;
    if (count <= MostShaderParams)
    {
      int ours = -1, games = -1;
      for (unsigned int i = 0; i < count; ++i)
      {
        char const* name = nullptr;
        if (!CopyFromState(&name, s + ShaderParamListOffset + i * ShaderParamSize + ShaderParamNameOffset, sizeof(name)))
          return false;
        if (name == g_RainShaderParamName)
          ours = (int)i;
        else if (name && games < 0 && NameIs(name, RainShaderParamName))
          games = (int)i;
      }
      if (value < 0.f)
        return ours >= 0;
      if (games >= 0)
      {
        result = 2;
        remix = ours >= 0;
      }
      else if (ours >= 0)
      {
        result = 1;
        CopyToState(s + ShaderParamListOffset + ours * ShaderParamSize, &value, sizeof(value));
      }
      else if (count < MostShaderParams)
      {
        BYTE* entry = s + ShaderParamListOffset + count * ShaderParamSize;
        float v[4] = { value, 0.f, 0.f, 0.f };
        char const* name = g_RainShaderParamName;
        if (CopyToState(entry, v, sizeof(v)) && CopyToState(entry + ShaderParamNameOffset, &name, sizeof(name)))
        {
          MemoryBarrier();
          unsigned int more = count + 1;
          if (CopyToState(s + ShaderParamCountOffset, &more, sizeof(more)))
            result = 1;
        }
      }
    }
    else if (value < 0.f)
      return false;
    g_WetParamState.store(result, std::memory_order_relaxed);
    if (!(g_WetParamLogged & (1 << result)))
    {
      g_WetParamLogged |= 1 << result;
      if (result == 1)
        util::log::Write("Weather: the game's wet look: RainShaderParam %.2f added to the visual environment's shader "
          "parameters (state 0x%I64X, %u of the game's there)", value, state, count);
      else if (result == 2)
        util::log::Write("Weather: the game's own RainShaderParam is there (state 0x%I64X): left as it is", state);
      else
        util::log::Write("Weather: the game's wet look not given: the shader parameter list has %u of at most %u (state 0x%I64X)",
          count, MostShaderParams, state);
    }
    return remix;
  }
}

// After each mix of a view by the visual environment manager (game thread):
// the rain's strength (see above) to the view with ids 0, 0 and the rainy
// city look to every view, each kept track of on its own. True when the view
// should be mixed again (something given back).
namespace
{
  // The game's wind with the rain (see RainWindLeast): over the state's
  // values after each mix, the game's own kept (a value other than the one
  // written is the game's new one), and put back when it's off. amount: how
  // hard it rains x RainWindAmount, < 0 off. True: the view to be mixed again.
  struct WindTrack
  {
    __int64 State;
    bool Applied;
    bool Calm;          // the last written was the lair's stillness (see GiveRainWind's calm)
    float Base[2];
    float Written[2];
  };
  WindTrack g_WindTrack{};

  // calm: the camera in the lair with its windows shut (see InShutLair): the
  // game's own wind stilled, its strength and turbulence 0, rain or not, so
  // the lair's curtains hang still (cloth, its ClothEntityData's WindScale
  // 0.01 of the game's wind, read offline 2026-11-01; nothing else in the
  // lair, pf_dt_trainstation, makes wind; 2026-11-01, the user: "When 'Close
  // the lair's open windows' is enabled the curtains on the lair shouldn't
  // wave like there is air"); the game's own back once it's out
  bool GiveRainWind(__int64 state, float amount, float turbulence, bool calm)
  {
    WindTrack& w = g_WindTrack;
    if (amount < 0.f && !calm && !w.Applied)
      return false;
    if (w.Applied && w.State != state)
      w.Applied = false;
    BYTE* pState = (BYTE*)state;
    float now[2];
    if (!CopyFromState(&now[0], pState + WindStrengthOffset, 4) || !CopyFromState(&now[1], pState + WindTurbulenceOffset, 4))
      return false;
    for (int i = 0; i < 2; ++i)
      if (!w.Applied || now[i] != w.Written[i])
        w.Base[i] = now[i];
    if (amount < 0.f && !calm)
    {
      // Off: the game's back where ours still is
      for (int i = 0; i < 2; ++i)
        if (now[i] == w.Written[i])
          CopyToState(pState + (i ? WindTurbulenceOffset : WindStrengthOffset), &w.Base[i], 4);
      w.Applied = false;
      util::log::Write("Weather: the game's wind back as it was (strength %.2f, turbulence %.2f)", w.Base[0], w.Base[1]);
      return true;
    }
    float want[2] = { 0.f, 0.f };
    if (!calm)
    {
      float a = (std::min)(amount, 2.f);
      // (gusting round it, see RainGust)
      float t = (std::max)(0.f, (std::min)(turbulence, 1.f));
      want[0] = (std::max)(w.Base[0], RainWindLeast + RainWindMore * (std::min)(a, 1.f) * (a > 1.f ? a : 1.f)) * RainGust(t);
      want[1] = w.Base[1] * (1.f + (std::min)(a, 1.f)) * (1.f + t);
    }
    if (!CopyToState(pState + WindStrengthOffset, &want[0], 4) || !CopyToState(pState + WindTurbulenceOffset, &want[1], 4))
      return false;
    if (calm && (!w.Applied || !w.Calm))
      util::log::Write("Weather: the camera in the lair with its windows shut: the game's wind stilled (strength %.2f -> 0, "
        "turbulence %.2f -> 0), its curtains hang still", w.Base[0], w.Base[1]);
    else if (!calm && (!w.Applied || w.Calm))
      util::log::Write("Weather: the game's wind with the rain: strength %.2f -> %.2f, turbulence %.2f -> %.2f", w.Base[0],
        want[0], w.Base[1], want[1]);
    w.Calm = calm;
    memcpy(w.Written, want, sizeof(want));
    w.Applied = true;
    w.State = state;
    return false;
  }
}

bool weather::AfterViewMix(__int64 view)
{
  float want = -1.f;
  RainyLook look{};
  float lookAmount = 0.f;
  float wet = -1.f;   // the game's RainShaderParam (see above), < 0 none
  float rainWind = -1.f;   // the wind with the rain (see GiveRainWind), < 0 none
  float rainTurbulence = 0.f;   // and its gusts (see RainGust)
  bool lairCalm = false;   // the camera in the lair with its windows shut: the game's wind stilled (see GiveRainWind)
  {
    std::lock_guard<std::mutex> lock(g_Mutex);
    if (g_RainOn)
      want = g_Settings.Strength * (std::min)(1.f, g_Intensity);
    look = LookNow();
    // (taken back while the game's menu or map has it paused: the map's view
    // of the city went white in a blizzard's fog, the user, 2026-10-26:
    // "make rain/snow not affect main menu and map menu"; given again when
    // it's resumed)
    lookAmount = gamestate::Paused() ? 0.f : g_LookAmount;
    // (not in the lair with its windows shut, see InShutLair)
    rainWind = g_RainOn && g_Settings.RainWind && !g_InShutLair ?
      RainAmountNow() * (std::max)(0.f, (std::min)(g_Settings.RainWindAmount, 2.f)) : -1.f;
    rainTurbulence = g_Settings.RainTurbulence;
    lairCalm = g_InShutLair;
    if (g_RainOn && g_Settings.GameWet > 0.f)
      wet = g_Settings.GameWet * (std::min)(1.f, g_Intensity);
  }
  if (!g_RainParamCode)
  {
    g_RainParamCode = IsRainParamCode((BYTE const*)g_gameHandle) ? 1 : -1;
    if (g_RainParamCode < 0)
      util::log::Write("Weather: the game's emitter parameter code isn't what the tools know, the rain's strength isn't given");
  }
  bool remix = false;

  // The game's wind with the rain (see GiveRainWind), to the main view
  {
    __int64 state = RainParamState(view);
    if (state)
      remix |= GiveRainWind(state, rainWind, rainTurbulence, lairCalm);
  }

  // The rainy city look, to every view
  {
    __int64 state = ViewState(view);
    if (state)
    {
      // A view not mixed for 2 s is gone (Faith's death and a loading screen
      // can bring new ones): its slot is free again, and nothing is written
      // to the state it had
      ULONGLONG tick = GetTickCount64();
      int slot = -1, empty = -1;
      for (int i = 0; i < MostLookViews; ++i)
      {
        if (g_LookViews[i].State == state)
          slot = i;
        else if (empty < 0 && (!g_LookViews[i].Applied || tick - g_LookViews[i].Seen > 2000))
          empty = i;
      }
      if (slot < 0 && look.On && lookAmount > 0.f && empty >= 0)
      {
        slot = empty;
        g_LookViews[slot].State = state;
        g_LookViews[slot].Applied = false;
      }
      if (slot >= 0)
      {
        g_LookViews[slot].Seen = tick;
        remix |= GiveLook(g_LookViews[slot], state, look, lookAmount);
      }
      remix |= GiveRainShaderParam(state, wet);
      // The main view's fog as the game will draw it (see g_FogNow)
      if (RainParamState(view) == state)
      {
        BYTE* f = (BYTE*)state + 0x620;
        weather::FogParams fog{};
        BYTE enable = 0, heightEnable = 0;
        if (CopyFromState(&enable, f + 0x9C, 1) && CopyFromState(&heightEnable, f + 0xA0, 1) &&
            CopyFromState(&fog.FollowCamera, f + 0x8C, 4) && CopyFromState(&fog.Altitude, f + 0x90, 4) &&
            CopyFromState(&fog.Depth, f + 0x94, 4) && CopyFromState(&fog.SeenUpTo, f + 0x98, 4) &&
            CopyFromState(&fog.MinTransmittance, (BYTE*)state + 0x390 + 0x24C, 4))
        {
          fog.On = enable && heightEnable && fog.Depth > 0.f && fog.SeenUpTo > 0.f;
          std::lock_guard<std::mutex> lock(g_Mutex);
          g_FogNow = fog;
        }
      }
      if (wet < 0.f)
      {
        g_WetParamState.store(0, std::memory_order_relaxed);
        g_WetParamLogged = 0;
      }
    }
  }

  // The rain's strength, to the view with ids 0, 0
  if (g_RainParamCode < 0 || (want < 0.f && !g_RainParam.Applied))
    return remix;
  __int64 state = RainParamState(view);
  if (!state)
    return remix;
  __int64 at = state + VeStateRainParamOffset;
  float now = 0.f;
  if (!ReadFloat(at, now))
    return remix;

  if (want >= 0.f)
  {
    // A value the game wrote since (a mix) is the new one under it
    static float s_logged = -1.f;
    if (!g_RainParam.Applied || state != g_RainParam.State || now != g_RainParam.Written)
    {
      g_RainParam.Base = now;
      g_RainParamGameValue = now;
      if (!g_RainParam.Applied || state != g_RainParam.State)
      {
        s_logged = want;
        util::log::Write("Weather: the rain's strength (EmitterParameter3) %.2f given to the visual environment "
          "(view 0x%I64X, state 0x%I64X; the game had %.2f)", want, view, state, now);
      }
    }
    else if (fabsf(want - s_logged) >= 0.1f)
    {
      s_logged = want;
      util::log::Write("Weather: the rain's strength (EmitterParameter3) now %.2f", want);
    }
    if (!WriteFloat(at, want))
      return remix;
    g_RainParam = { true, state, g_RainParam.Base, want };
    g_RainParamShown = true;
    return remix;
  }

  // Off: the game's value back where ours still is, and the view mixed again
  if (state == g_RainParam.State && now == g_RainParam.Written)
    WriteFloat(at, g_RainParam.Base);
  util::log::Write("Weather: the rain's strength given back (EmitterParameter3 %.2f, the game's)", g_RainParam.Base);
  g_RainParam.Applied = false;
  g_RainParamShown = false;
  return true;
}

void weather::NoteSoundCreated(__int64 voice, __int64 asset)
{
  if (!voice || asset < 0x10000 || g_shutdown)
    return;
  {
    std::lock_guard<std::mutex> lock(g_Mutex);
    auto it = g_NotRainSound.find(asset);
    if (it != g_NotRainSound.end() && ReadPointer(asset + AssetNameOffset) == it->second)
      return;
  }
  char name[96];
  __int64 namePointer = 0;
  if (!ReadName(asset, name, sizeof(name), namePointer))
    return;
  bool rain = !_stricmp(name, RainSoundName);
  __int64 vtable = rain ? ReadPointer(asset) : 0;
  std::lock_guard<std::mutex> lock(g_Mutex);
  // (none taken while a world goes, see g_WorldGoing)
  if (rain && GetTickCount64() < g_RainSoundQuietUntil)
    return;
  if (!rain || !vtable)
  {
    if (g_NotRainSound.size() >= MaxNotRain)
      g_NotRainSound.clear();
    g_NotRainSound[asset] = namePointer;
    return;
  }
  // A new asset (its zone loaded again), or the first: its events taken again
  // (a voice of the tools' for the old one is let go at the next update)
  if (asset != g_RainSound.Asset || !g_RainSound.EventCount)
  {
    if (asset != g_RainSound.Asset && g_RainSound.OwnVoice)
    {
      g_RainSound.StaleVoice = g_RainSound.OwnVoice;
      g_RainSound.OwnVoice = 0;
    }
    g_RainSound.Asset = asset;
    g_RainSound.AssetVTable = vtable;
    g_RainSound.GameVoice = voice;
    g_RainSound.MadeTick = GetTickCount64();
    g_RainSound.EventCount = 0;
    g_WatchedVoice = voice;
    util::log::Write("Weather: the game made a voice for the rain's sound (%s), asset 0x%I64X", RainSoundName, asset);
  }
}

void weather::NoteVoiceEvent(__int64 voice, unsigned int event)
{
  if (!voice || voice != g_WatchedVoice.load(std::memory_order_relaxed))
    return;
  std::lock_guard<std::mutex> lock(g_Mutex);
  if (voice != g_RainSound.GameVoice || GetTickCount64() - g_RainSound.MadeTick > RainSoundEventsMs ||
      g_RainSound.EventCount >= MaxRainSoundEvents)
  {
    g_WatchedVoice = 0;
    return;
  }
  g_RainSound.Events[g_RainSound.EventCount++] = event;
  util::log::Write("Weather: the game gave its rain sound event 0x%08X", event);
}

bool weather::ThunderOn()
{
  std::lock_guard<std::mutex> lock(g_Mutex);
  return ThunderWanted();
}

weather::Wetting weather::GetWetting()
{
  std::lock_guard<std::mutex> lock(g_Mutex);
  if (!g_Settings.RandomRain || SnowChosen())
    return { false, 1.f, 1.f };
  return { true, g_GroundWet, g_PuddleFill };
}

weather::DistantRain weather::GetDistantRain()
{
  std::lock_guard<std::mutex> lock(g_Mutex);
  DistantRain d{};
  if (!g_RainOn || !g_HaveCamera || g_shutdown)
    return d;
  d.Amount = (std::max)(0.f, (std::min)(g_Settings.DistantRain, 1.f)) * (std::min)(1.f, g_Intensity);
  d.Strength = g_Settings.Strength / 0.7f * (g_Settings.Heavy ? 1.4f : 1.f) * (std::min)(1.f, g_Intensity);
  bool roofs = g_Settings.KeepOut && !g_Slots.empty();
  d.Indoors = roofs && g_CameraCovered;
  d.RoofReach = roofs ? GridHalf * CellSize : 0.f;
  return d;
}

int weather::GetBolts(float (*a)[3], float (*b)[3], float* width, float* bright, int* strike, float* strikeDistance,
  int mostSegments, int mostStrikes)
{
  std::lock_guard<std::mutex> lock(g_Mutex);
  ULONGLONG now = GetTickCount64();
  int count = 0, strikes = 0;
  for (Strike const& st : g_Strikes)
  {
    if (!st.Started || st.Bolt.empty() || strikes >= mostStrikes)
      continue;
    float t = (now - st.Tick) / 1000.f;
    if (t > BoltSeconds)
      continue;
    // With its flash's pulses (the return strokes), as bright as the flash is set
    float level = (std::min)(1.f, 1.6f * FlashPulse(t)) * (std::min)(g_Settings.LightningBrightness, 2.f);
    if (level <= 0.f)
      continue;
    for (BoltSegment const& s : st.Bolt)
    {
      if (count >= mostSegments)
        break;
      memcpy(a[count], s.A, sizeof(s.A));
      memcpy(b[count], s.B, sizeof(s.B));
      width[count] = s.Width;
      bright[count] = s.Bright * level;
      strike[count] = strikes;
      ++count;
    }
    strikeDistance[strikes++] = st.Distance;
  }
  return count;
}

bool weather::GetOpenSky(std::vector<float>& cells, int& count, float& size, float& cornerX, float& cornerZ)
{
  std::lock_guard<std::mutex> lock(g_Mutex);
  if (!g_RoofsKnown || g_RainGrid.size() != (size_t)(GridSize * GridSize * 2))
    return false;
  cells = g_RainGrid;
  count = GridSize;
  size = CellSize;
  cornerX = g_RainGridOrigin[0] * CellSize;
  cornerZ = g_RainGridOrigin[1] * CellSize;
  return true;
}

void weather::GetListener(float* right, float* back, float& inside)
{
  std::lock_guard<std::mutex> lock(g_Mutex);
  memcpy(right, g_ListenerRight, sizeof(g_ListenerRight));
  memcpy(back, g_ListenerBack, sizeof(g_ListenerBack));
  inside = g_RoofsKnown ? g_Inside : 0.f;
}

weather::FogParams weather::GetFog()
{
  std::lock_guard<std::mutex> lock(g_Mutex);
  return g_FogNow;
}

void weather::SetShardTop(bool known, float const* top)
{
  std::lock_guard<std::mutex> lock(g_Mutex);
  g_ShardTopKnown = known && top;
  if (g_ShardTopKnown)
    memcpy(g_ShardTop, top, sizeof(g_ShardTop));
}

bool weather::ShardTopWanted()
{
  std::lock_guard<std::mutex> lock(g_Mutex);
  g_ShardTopAsked = g_StormWas || g_StrikeNow;
  return g_ShardTopAsked;
}

bool weather::GetShelter(std::vector<float>& cells, int& count, float& size, float& cornerX, float& cornerZ,
  std::vector<float>* edges)
{
  std::lock_guard<std::mutex> lock(g_Mutex);
  if (!g_RoofsKnown || g_ShelterGrid.size() != (size_t)(GridSize * GridSize * 4))
    return false;
  cells = g_ShelterGrid;
  if (edges)
  {
    if (g_DripEdges.size() == g_ShelterGrid.size())
      *edges = g_DripEdges;
    else
      edges->clear();
  }
  count = GridSize;
  size = CellSize;
  cornerX = g_RainGridOrigin[0] * CellSize;
  cornerZ = g_RainGridOrigin[1] * CellSize;
  return true;
}

namespace
{
  // The game's interiors (2026-10-26, the user: snow "also goes inside the
  // lair"). The game fades each of its interior visual environments in by
  // how near the camera is to its volumes: in levels/sp/sp_maincity/
  // visualenvironments each environment's Visibility is an AreaProximity
  // entity's Out, and that entity is linked to its boxes (OBBData) and prisms
  // (VolumeVectorShapeData: a closed outline and a height) in the world's
  // layer0_ve_volumes. Read offline (Research_Snow_2026-10-26.md, the
  // scratchpad's vevolumes.txt / interiors_cpp.py): these are the volumes of
  // the environments whose names say they're inside (ve_runners_lair_interior,
  // ve_plasticslair, ve_familymatters_krugersapartment_01,
  // ve_generic_interiortheview, ve_droneworks_interior,
  // ve_benefactor_anansitower_interior_01, ve_kingdom_interior,
  // ve_elysium_interior01, ve_trainstation_rightwing), not the districts', the
  // Shard's fog or the others. Inside one nothing falls and nothing is wet or
  // snowed on (WetLook.hlsl's InteriorAt, ShelterAt below): the Runners'
  // lair's roof isn't found by the roof sweeps over much of it (the 01:21 log
  // went from "under a roof" to "under the open sky" and back walking
  // through it), so its squares counted as open; the game's own outline of
  // it has every place Faith walked there inside it.
  struct InteriorBoxData { float Middle[3]; float Half[3]; float Axis[3][3]; bool Generic; };   // axes: right, up, forward (half sizes along them); Generic: see InteriorAmount
  const InteriorBoxData InteriorBoxes[] = {
    { { 425.27f, 91.22f, 389.24f }, { 8.93f, 5.76f, 7.13f }, { { 0.2903f, 0.0000f, -0.9569f }, { 0.0000f, 1.0000f, 0.0000f }, { 0.9569f, 0.0000f, 0.2903f } } },   // ve_plasticslair #52
    { { 424.86f, 91.22f, 400.60f }, { 2.07f, 5.76f, 4.23f }, { { 0.2903f, 0.0000f, -0.9569f }, { 0.0000f, 1.0000f, 0.0000f }, { 0.9569f, 0.0000f, 0.2903f } } },   // ve_plasticslair #108
    { { 879.34f, 223.42f, -453.73f }, { 22.38f, 20.53f, 35.75f }, { { 0.6674f, 0.0000f, -0.7447f }, { 0.0000f, 1.0000f, 0.0000f }, { 0.7447f, 0.0000f, 0.6674f } } },   // ve_familymatters_krugersapartment_01 #63
    { { 630.96f, 290.84f, 242.46f }, { 37.50f, 245.53f, 38.45f }, { { 0.8315f, 0.0000f, -0.5556f }, { 0.0000f, 1.0000f, 0.0000f }, { 0.5556f, 0.0000f, 0.8315f } } },   // ve_benefactor_anansitower_interior_01 #104
    { { 55.22f, 48.09f, 361.33f }, { 25.75f, 7.56f, 4.96f }, { { 0.9040f, 0.0000f, 0.4276f }, { 0.0000f, 1.0000f, 0.0000f }, { -0.4276f, 0.0000f, 0.9040f } } },   // ve_trainstation_rightwing #93
    { { -636.18f, 60.59f, 293.25f }, { 6.85f, 4.69f, 1.67f }, { { -0.9569f, 0.0000f, -0.2903f }, { 0.0000f, 1.0000f, 0.0000f }, { 0.2903f, 0.0000f, -0.9569f } } },   // ve_droneworks_interior #54
    { { -623.59f, 77.24f, 315.07f }, { 25.23f, 21.34f, 15.75f }, { { -0.9569f, 0.0000f, -0.2903f }, { 0.0000f, 1.0000f, 0.0000f }, { 0.2903f, 0.0000f, -0.9569f } } },   // ve_droneworks_interior #13
    { { -619.89f, 107.08f, 317.37f }, { 23.60f, 6.85f, 14.48f }, { { -0.9569f, 0.0000f, -0.2903f }, { 0.0000f, 1.0000f, 0.0000f }, { 0.2903f, 0.0000f, -0.9569f } } },   // ve_droneworks_interior #99
    { { -612.53f, 62.35f, 333.97f }, { 9.40f, 3.01f, 7.76f }, { { -0.9569f, 0.0000f, -0.2903f }, { 0.0000f, 1.0000f, 0.0000f }, { 0.2903f, 0.0000f, -0.9569f } } },   // ve_droneworks_interior #19
    { { -648.54f, 65.05f, 323.95f }, { 3.93f, 4.06f, 4.86f }, { { -0.9569f, 0.0000f, -0.2903f }, { 0.0000f, 1.0000f, 0.0000f }, { 0.2903f, 0.0000f, -0.9569f } } },   // ve_droneworks_interior #91
    { { 205.08f, 55.59f, -18.97f }, { 14.36f, 12.55f, 19.31f }, { { -0.8315f, 0.0000f, -0.5556f }, { 0.0000f, 1.0000f, 0.0000f }, { 0.5556f, 0.0000f, -0.8315f } }, true },   // ve_generic_interiortheview #73
    { { 188.69f, 46.96f, 10.70f }, { 11.51f, 5.40f, 14.21f }, { { -0.8315f, 0.0000f, -0.5556f }, { 0.0000f, 1.0000f, 0.0000f }, { 0.5556f, 0.0000f, -0.8315f } }, true },   // ve_generic_interiortheview #78
    { { -105.87f, 49.30f, 299.55f }, { 42.25f, 7.45f, 5.16f }, { { 0.9714f, 0.0000f, 0.2376f }, { 0.0000f, 1.0000f, 0.0000f }, { -0.2376f, 0.0000f, 0.9714f } }, true },   // ve_generic_interiortheview #77
    { { 85.75f, 57.48f, 376.14f }, { 54.68f, 8.84f, 3.64f }, { { 0.9040f, 0.0000f, 0.4276f }, { 0.0000f, 1.0000f, 0.0000f }, { -0.4276f, 0.0000f, 0.9040f } }, true },   // ve_generic_interiortheview #114
    { { 12.42f, 50.75f, 340.86f }, { 26.91f, 4.38f, 5.89f }, { { 0.8825f, 0.0000f, 0.4703f }, { 0.0000f, 1.0000f, 0.0000f }, { -0.4703f, 0.0000f, 0.8825f } }, true },   // ve_generic_interiortheview #22
    { { -37.60f, 52.37f, 319.65f }, { 29.17f, 4.38f, 5.90f }, { { 0.9588f, 0.0000f, 0.2840f }, { 0.0000f, 1.0000f, 0.0000f }, { -0.2840f, 0.0000f, 0.9588f } }, true },   // ve_generic_interiortheview #79
    { { 149.77f, 57.28f, 403.96f }, { 15.71f, 8.54f, 4.08f }, { { 0.9700f, 0.0000f, 0.2430f }, { 0.0000f, 1.0000f, 0.0000f }, { -0.2430f, 0.0000f, 0.9700f } }, true },   // ve_generic_interiortheview #34
    { { -51.39f, 47.89f, 427.36f }, { 63.27f, 7.56f, 4.85f }, { { 0.3329f, 0.0000f, 0.9429f }, { 0.0000f, 1.0000f, 0.0000f }, { -0.9429f, 0.0000f, 0.3329f } }, true },   // ve_generic_interiortheview #56
    { { -69.69f, 48.70f, 363.35f }, { 8.08f, 8.37f, 6.65f }, { { -0.2620f, 0.0000f, 0.9651f }, { 0.0000f, 1.0000f, 0.0000f }, { -0.9651f, 0.0000f, -0.2620f } }, true },   // ve_generic_interiortheview #74
    { { -64.98f, 55.01f, 328.44f }, { 10.44f, 5.37f, 4.47f }, { { -0.2807f, 0.0000f, 0.9598f }, { 0.0000f, 1.0000f, 0.0000f }, { -0.9598f, 0.0000f, -0.2807f } }, true },   // ve_generic_interiortheview #116
    { { -157.20f, 71.78f, 26.98f }, { 4.93f, 4.84f, 8.58f }, { { -0.5141f, 0.0000f, -0.8577f }, { 0.0000f, 1.0000f, 0.0000f }, { 0.8577f, 0.0000f, -0.5141f } }, true },   // ve_generic_interiortheview #50
    { { 709.80f, 123.36f, 398.12f }, { 7.36f, 1.76f, 5.44f }, { { 0.9954f, 0.0000f, -0.0955f }, { 0.0000f, 1.0000f, 0.0000f }, { 0.0955f, 0.0000f, 0.9954f } }, true },   // ve_generic_interiortheview #35
    { { 733.70f, 131.04f, 430.16f }, { 12.08f, 3.20f, 6.88f }, { { 0.9954f, 0.0000f, -0.0955f }, { 0.0000f, 1.0000f, 0.0000f }, { 0.0955f, 0.0000f, 0.9954f } }, true },   // ve_generic_interiortheview #115
    { { 788.79f, 114.88f, 393.33f }, { 8.32f, 4.96f, 8.96f }, { { 0.9954f, 0.0000f, -0.0955f }, { 0.0000f, 1.0000f, 0.0000f }, { 0.0955f, 0.0000f, 0.9954f } }, true },   // ve_generic_interiortheview #53
    { { 571.52f, 115.52f, 417.24f }, { 2.56f, 5.44f, 7.84f }, { { 0.9954f, 0.0000f, -0.0955f }, { 0.0000f, 1.0000f, 0.0000f }, { 0.0955f, 0.0000f, 0.9954f } }, true },   // ve_generic_interiortheview #24
    { { 510.18f, 78.44f, 183.71f }, { 17.70f, 3.68f, 6.72f }, { { -0.5480f, 0.0000f, -0.8365f }, { 0.0000f, 1.0000f, -0.0000f }, { 0.8365f, -0.0000f, -0.5480f } }, true },   // ve_generic_interiortheview #49
    { { 401.69f, 66.04f, 92.84f }, { 5.76f, 4.08f, 3.84f }, { { -0.5480f, 0.0000f, -0.8365f }, { 0.0000f, 1.0000f, -0.0000f }, { 0.8365f, -0.0000f, -0.5480f } }, true },   // ve_generic_interiortheview #47
    { { 416.98f, 61.64f, 75.15f }, { 3.22f, 4.80f, 16.56f }, { { -0.5480f, 0.0000f, -0.8365f }, { 0.0000f, 1.0000f, -0.0000f }, { 0.8365f, -0.0000f, -0.5480f } }, true },   // ve_generic_interiortheview #6
    { { 539.28f, 67.09f, 22.85f }, { 15.88f, 5.65f, 13.28f }, { { -0.5480f, 0.0000f, -0.8365f }, { 0.0000f, 1.0000f, -0.0000f }, { 0.8365f, -0.0000f, -0.5480f } }, true },   // ve_generic_interiortheview #25
    { { 545.41f, 67.09f, 44.81f }, { 5.84f, 5.66f, 6.81f }, { { -0.5480f, 0.0000f, -0.8365f }, { 0.0000f, 1.0000f, -0.0000f }, { 0.8365f, -0.0000f, -0.5480f } }, true },   // ve_generic_interiortheview #33
    { { 555.43f, 55.98f, 96.79f }, { 2.55f, 9.91f, 12.85f }, { { 0.0000f, 1.0000f, 0.0000f }, { 0.9266f, 0.0000f, -0.3761f }, { -0.3761f, 0.0000f, -0.9266f } }, true },   // ve_generic_interiortheview #31
    { { -451.98f, 50.08f, 460.04f }, { 7.42f, 1.91f, 7.26f }, { { 0.9724f, 0.0000f, 0.2334f }, { 0.0000f, 1.0000f, 0.0000f }, { -0.2334f, 0.0000f, 0.9724f } }, true },   // ve_generic_interiortheview #71
    { { 902.29f, 824.79f, -2950.00f }, { 15.02f, 4.89f, 8.21f }, { { -0.9569f, 0.0000f, -0.2903f }, { 0.0000f, 1.0000f, 0.0000f }, { 0.2903f, 0.0000f, -0.9569f } }, true },   // ve_generic_interiortheview #103
    { { 914.28f, 803.52f, -2952.49f }, { 6.10f, 33.02f, 4.43f }, { { -0.9569f, 0.0000f, -0.2903f }, { 0.0000f, 1.0000f, 0.0000f }, { 0.2903f, 0.0000f, -0.9569f } }, true },   // ve_generic_interiortheview #12
    { { 825.33f, 912.35f, -3042.27f }, { 10.73f, 5.98f, 1.83f }, { { -0.4020f, 0.0000f, -0.9156f }, { 0.0000f, 1.0000f, 0.0000f }, { 0.9156f, 0.0000f, -0.4020f } }, true },   // ve_generic_interiortheview #4
    { { 813.60f, 951.31f, -3068.88f }, { 4.73f, 37.97f, 5.79f }, { { -0.5556f, 0.0000f, -0.8314f }, { 0.0000f, 1.0000f, 0.0000f }, { 0.8314f, 0.0000f, -0.5556f } }, true },   // ve_generic_interiortheview #90
    { { 855.77f, 415.58f, -2905.15f }, { 23.73f, 48.60f, 18.16f }, { { -0.2916f, -0.0117f, 0.9565f }, { -0.9565f, -0.0000f, -0.2916f }, { 0.0034f, -0.9999f, -0.0112f } }, true },   // ve_generic_interiortheview #119
    { { 829.37f, 1435.84f, -3079.93f }, { 16.01f, 11.87f, 16.95f }, { { 0.8255f, 0.0092f, -0.5644f }, { -0.0034f, 0.9999f, 0.0112f }, { 0.5645f, -0.0073f, 0.8254f } }, true },   // ve_generic_interiortheview #111
    { { 901.63f, 730.96f, -2917.26f }, { 33.93f, 40.02f, 19.39f }, { { 0.2916f, 0.0117f, -0.9565f }, { -0.0034f, 0.9999f, 0.0112f }, { 0.9565f, 0.0000f, 0.2916f } }, true },   // ve_generic_interiortheview #62
    { { 625.63f, 78.50f, 74.54f }, { 4.73f, 7.15f, 5.55f }, { { -0.3742f, 0.0000f, -0.9273f }, { -0.0001f, 1.0000f, 0.0001f }, { 0.9273f, 0.0001f, -0.3742f } }, true },   // ve_generic_interiortheview #72
    { { 617.16f, 87.73f, 75.79f }, { 7.62f, 8.07f, 13.92f }, { { -0.3742f, 0.0000f, -0.9273f }, { -0.0001f, 1.0000f, 0.0001f }, { 0.9273f, 0.0001f, -0.3742f } }, true },   // ve_generic_interiortheview #100
    { { 1146.55f, 61.13f, -223.68f }, { 13.07f, 7.66f, 7.90f }, { { 0.1896f, 0.0000f, -0.9819f }, { 0.0000f, 1.0000f, 0.0000f }, { 0.9819f, 0.0000f, 0.1896f } }, true },   // ve_generic_interiortheview #23
    { { 1146.47f, 61.13f, -194.50f }, { 5.10f, 7.66f, 5.45f }, { { -0.5602f, 0.0000f, -0.8283f }, { 0.0000f, 1.0000f, 0.0000f }, { 0.8283f, 0.0000f, -0.5602f } }, true },   // ve_generic_interiortheview #96
    { { 1140.90f, 61.13f, -206.01f }, { 6.35f, 7.66f, 10.08f }, { { 0.1896f, 0.0000f, -0.9819f }, { 0.0000f, 1.0000f, 0.0000f }, { 0.9819f, 0.0000f, 0.1896f } }, true },   // ve_generic_interiortheview #59
    { { 1138.81f, 61.13f, -201.74f }, { 5.31f, 7.66f, 7.73f }, { { -0.5602f, 0.0000f, -0.8283f }, { 0.0000f, 1.0000f, 0.0000f }, { 0.8283f, 0.0000f, -0.5602f } }, true },   // ve_generic_interiortheview #7
    { { 1169.89f, 63.76f, -290.00f }, { 20.47f, 5.03f, 14.97f }, { { 0.1896f, 0.0000f, -0.9819f }, { 0.0000f, 1.0000f, 0.0000f }, { 0.9819f, 0.0000f, 0.1896f } }, true },   // ve_generic_interiortheview #39
    { { 1170.39f, 55.79f, -292.56f }, { 17.86f, 3.12f, 14.97f }, { { 0.1896f, 0.0000f, -0.9819f }, { 0.0000f, 1.0000f, 0.0000f }, { 0.9819f, 0.0000f, 0.1896f } }, true },   // ve_generic_interiortheview #101
    { { 1163.40f, 55.79f, -270.57f }, { 5.38f, 3.12f, 6.40f }, { { 0.1896f, 0.0000f, -0.9819f }, { 0.0000f, 1.0000f, 0.0000f }, { 0.9819f, 0.0000f, 0.1896f } }, true },   // ve_generic_interiortheview #109
    { { -106.41f, -154.40f, 894.16f }, { 4.16f, 6.89f, 8.64f }, { { 0.1699f, 0.0000f, 0.9855f }, { 0.0000f, 1.0000f, 0.0000f }, { -0.9855f, 0.0000f, 0.1699f } } },   // ve_kingdom_interior #86
    { { -212.46f, -165.57f, 1015.52f }, { 22.19f, 12.87f, 31.36f }, { { 0.9383f, 0.0000f, 0.3458f }, { -0.0000f, 1.0000f, -0.0000f }, { -0.3458f, -0.0000f, 0.9383f } } },   // ve_kingdom_interior #88
    { { -121.20f, -173.48f, 881.43f }, { 26.18f, 18.38f, 74.28f }, { { 0.1699f, 0.0000f, 0.9855f }, { 0.0000f, 1.0000f, 0.0000f }, { -0.9855f, 0.0000f, 0.1699f } } },   // ve_kingdom_interior #16
    { { -189.91f, -172.28f, 927.22f }, { 31.57f, 19.58f, 59.28f }, { { 0.9383f, 0.0000f, 0.3458f }, { -0.0000f, 1.0000f, -0.0000f }, { -0.3458f, -0.0000f, 0.9383f } } },   // ve_kingdom_interior #102
    { { -206.99f, 117.55f, -183.78f }, { 26.13f, 90.73f, 70.25f }, { { -0.8893f, 0.0000f, 0.4574f }, { 0.0000f, -1.0000f, 0.0000f }, { 0.4574f, 0.0000f, 0.8893f } } },   // ve_elysium_interior01 #70
    { { 148.46f, 52.86f, 406.57f }, { 16.03f, 2.92f, 5.92f }, { { 0.9700f, 0.0000f, 0.2430f }, { 0.0000f, 1.0000f, 0.0000f }, { -0.2430f, 0.0000f, 0.9700f } } },   // ve_droneworks_interior #81
    { { 150.09f, 61.49f, 407.49f }, { 0.91f, 5.73f, 0.73f }, { { 0.9700f, 0.0000f, 0.2430f }, { 0.0000f, 1.0000f, 0.0000f }, { -0.2430f, 0.0000f, 0.9700f } } },   // ve_droneworks_interior #113
    { { 134.43f, 65.22f, 429.47f }, { 6.57f, 21.40f, 25.88f }, { { 0.9700f, 0.0000f, 0.2430f }, { 0.0000f, 1.0000f, 0.0000f }, { -0.2430f, 0.0000f, 0.9700f } } },   // ve_droneworks_interior #41
    { { 487.59f, 63.04f, 70.71f }, { 9.45f, 2.90f, 11.05f }, { { 0.8365f, 0.0000f, -0.5480f }, { -0.0000f, 1.0000f, -0.0000f }, { 0.5480f, 0.0000f, 0.8365f } } },   // ve_droneworks_interior #36
    { { 1120.75f, 87.02f, -0.76f }, { 12.51f, 7.70f, 10.58f }, { { 0.7697f, 0.0000f, 0.6384f }, { 0.0000f, 1.0000f, 0.0000f }, { -0.6384f, 0.0000f, 0.7697f } } },   // ve_droneworks_interior #37
    { { 1129.32f, 81.75f, 16.79f }, { 5.35f, 2.43f, 2.62f }, { { 0.7697f, 0.0000f, 0.6384f }, { 0.0000f, 1.0000f, 0.0000f }, { -0.6384f, 0.0000f, 0.7697f } } },   // ve_droneworks_interior #117
  };
  struct InteriorPrismData { float Floor; float Height; int Count; float Points[8][2]; bool Generic; };   // points: x, z
  const InteriorPrismData InteriorPrisms[] = {
    { 102.40f, 8.00f, 4, { { 624.91f, 399.21f }, { 627.54f, 426.91f }, { 641.18f, 437.53f }, { 637.46f, 397.99f } }, true },   // ve_generic_interiortheview #65
    { 63.37f, 16.00f, 5, { { -14.02f, 270.88f }, { -39.56f, 287.95f }, { -36.72f, 292.20f }, { -7.60f, 326.63f }, { 13.71f, 312.39f } } },   // ve_runners_lair_interior #97
  };
  const int InteriorBoxCount = (int)(sizeof(InteriorBoxes) / sizeof(InteriorBoxes[0]));
  const int InteriorPrismCount = (int)(sizeof(InteriorPrisms) / sizeof(InteriorPrisms[0]));

  // How much a point (world) is inside one of them, 0 - 1, known: how much
  // the roof squares have looked at it (as WetLook.hlsl's InteriorAt: soft
  // at the faces, the generic ones only where the squares haven't looked)
  const float InteriorSideMargin = 0.3f, InteriorSideFade = 0.4f;
  const float InteriorFloorMargin = 1.f, InteriorFloorFade = 0.5f, InteriorTopFade = 0.6f;
  float InteriorSmooth(float lo, float hi, float x)
  {
    float t = (std::min)((std::max)((x - lo) / (hi - lo), 0.f), 1.f);
    return t * t * (3.f - 2.f * t);
  }
  float InteriorAmount(float side, float floorIn, float topIn)
  {
    return InteriorSmooth(-(InteriorSideMargin + InteriorSideFade), -InteriorSideMargin, side) *
      InteriorSmooth(-(InteriorFloorMargin + InteriorFloorFade), -InteriorFloorMargin, floorIn) *
      InteriorSmooth(-InteriorTopFade * 0.5f, InteriorTopFade * 0.5f, topIn);
  }
  float InteriorAt(float const* world, float known)
  {
    float inside = 0.f;
    for (InteriorBoxData const& b : InteriorBoxes)
    {
      float weight = b.Generic ? 1.f - known : 1.f;
      if (weight <= 0.f)
        continue;
      float q[3] = { world[0] - b.Middle[0], world[1] - b.Middle[1], world[2] - b.Middle[2] };
      int up = 0;
      for (int i = 1; i < 3; ++i)
        if (fabsf(b.Axis[i][1]) > fabsf(b.Axis[up][1]))
          up = i;
      float side = 1e9f, floorIn = 1e9f, topIn = 1e9f;
      for (int i = 0; i < 3; ++i)
      {
        float along = q[0] * b.Axis[i][0] + q[1] * b.Axis[i][1] + q[2] * b.Axis[i][2];
        float plus = b.Half[i] - along, minus = b.Half[i] + along;
        if (i == up)
        {
          topIn = b.Axis[i][1] >= 0.f ? plus : minus;
          floorIn = b.Axis[i][1] >= 0.f ? minus : plus;
        }
        else
          side = (std::min)(side, (std::min)(plus, minus));
      }
      inside = (std::max)(inside, InteriorAmount(side, floorIn, topIn) * weight);
    }
    for (InteriorPrismData const& p : InteriorPrisms)
    {
      float weight = p.Generic ? 1.f - known : 1.f;
      float floorIn = world[1] - p.Floor, topIn = p.Floor + p.Height - world[1];
      if (weight <= 0.f || floorIn < -(InteriorFloorMargin + InteriorFloorFade) || topIn < -InteriorTopFade * 0.5f)
        continue;
      bool within = false;
      float edge = 1e9f;
      for (int i = 0; i < p.Count; ++i)
      {
        float const* a = p.Points[i];
        float const* b = p.Points[i + 1 < p.Count ? i + 1 : 0];
        if ((a[1] > world[2]) != (b[1] > world[2]) && world[0] < (b[0] - a[0]) * (world[2] - a[1]) / (b[1] - a[1]) + a[0])
          within = !within;
        float abx = b[0] - a[0], abz = b[1] - a[1], apx = world[0] - a[0], apz = world[2] - a[1];
        float len = abx * abx + abz * abz;
        float t = len > 1e-6f ? (std::min)((std::max)((apx * abx + apz * abz) / len, 0.f), 1.f) : 0.f;
        float ex = apx - abx * t, ez = apz - abz * t;
        edge = (std::min)(edge, sqrtf(ex * ex + ez * ez));
      }
      inside = (std::max)(inside, InteriorAmount(within ? edge : -edge, floorIn, topIn) * weight);
    }
    return inside;
  }

  // Whether a point (world) is inside one of the named interiors (hard, as
  // InsideInterior with generic false), and the top of the one it's in (the
  // highest if more): a box's highest corner, a prism's top
  bool NamedInteriorTop(float const* world, float& top)
  {
    bool found = false;
    for (InteriorBoxData const& b : InteriorBoxes)
    {
      if (b.Generic)
        continue;
      float q[3] = { world[0] - b.Middle[0], world[1] - b.Middle[1], world[2] - b.Middle[2] };
      bool inside = true;
      for (int i = 0; i < 3 && inside; ++i)
        inside = fabsf(q[0] * b.Axis[i][0] + q[1] * b.Axis[i][1] + q[2] * b.Axis[i][2]) < b.Half[i];
      if (!inside)
        continue;
      float highest = b.Middle[1] + fabsf(b.Axis[0][1]) * b.Half[0] + fabsf(b.Axis[1][1]) * b.Half[1] +
        fabsf(b.Axis[2][1]) * b.Half[2];
      top = found ? (std::max)(top, highest) : highest;
      found = true;
    }
    for (InteriorPrismData const& p : InteriorPrisms)
    {
      if (p.Generic || world[1] < p.Floor || world[1] > p.Floor + p.Height)
        continue;
      bool inside = false;
      for (int i = 0; i < p.Count; ++i)
      {
        float const* a = p.Points[i];
        float const* b = p.Points[i + 1 < p.Count ? i + 1 : 0];
        if ((a[1] > world[2]) != (b[1] > world[2]) && world[0] < (b[0] - a[0]) * (world[2] - a[1]) / (b[1] - a[1]) + a[0])
          inside = !inside;
      }
      if (!inside)
        continue;
      top = found ? (std::max)(top, p.Floor + p.Height) : p.Floor + p.Height;
      found = true;
    }
    return found;
  }

  // Whether a point (world) is inside one of them at all (the whole table,
  // hard: what the frozen water's effects are kept out of). generic false:
  // only the named interiors, not ve_generic_interiortheview's lighting
  // volumes, which reach over open alleys (the ice over the water, GetIce)
  bool InsideInterior(float const* world, bool generic)
  {
    for (InteriorBoxData const& b : InteriorBoxes)
    {
      if (b.Generic && !generic)
        continue;
      float q[3] = { world[0] - b.Middle[0], world[1] - b.Middle[1], world[2] - b.Middle[2] };
      bool inside = true;
      for (int i = 0; i < 3 && inside; ++i)
        inside = fabsf(q[0] * b.Axis[i][0] + q[1] * b.Axis[i][1] + q[2] * b.Axis[i][2]) < b.Half[i];
      if (inside)
        return true;
    }
    for (InteriorPrismData const& p : InteriorPrisms)
    {
      if ((p.Generic && !generic) || world[1] < p.Floor || world[1] > p.Floor + p.Height)
        continue;
      bool inside = false;
      for (int i = 0; i < p.Count; ++i)
      {
        float const* a = p.Points[i];
        float const* b = p.Points[i + 1 < p.Count ? i + 1 : 0];
        if ((a[1] > world[2]) != (b[1] > world[2]) && world[0] < (b[0] - a[0]) * (world[2] - a[1]) / (b[1] - a[1]) + a[0])
          inside = !inside;
      }
      if (inside)
        return true;
    }
    return false;
  }

  // The Runners' lair shut (2026-10-27, the user: "When lair windows are
  // closed there shouldn't be any wind inside the building"): a point in its
  // outline (ve_runners_lair_interior's, as InteriorPrisms has it) while its
  // two windows that open are to be shut (GAME > Close the lair's open
  // windows, CameraManager's LairWindowsShut)
  const InteriorPrismData LairInterior = { 63.37f, 16.00f, 5, { { -14.02f, 270.88f }, { -39.56f, 287.95f },
    { -36.72f, 292.20f }, { -7.60f, 326.63f }, { 13.71f, 312.39f } } };

  bool InShutLair(float const* world)
  {
    CameraManager* cameras = g_mainHandle ? g_mainHandle->GetCameraManager() : nullptr;
    if (!cameras || !cameras->LairWindowsShut())
      return false;
    InteriorPrismData const& p = LairInterior;
    if (world[1] < p.Floor || world[1] > p.Floor + p.Height)
      return false;
    bool inside = false;
    for (int i = 0; i < p.Count; ++i)
    {
      float const* a = p.Points[i];
      float const* b = p.Points[i + 1 < p.Count ? i + 1 : 0];
      if ((a[1] > world[2]) != (b[1] > world[2]) && world[0] < (b[0] - a[0]) * (world[2] - a[1]) / (b[1] - a[1]) + a[0])
        inside = !inside;
    }
    return inside;
  }
}

void weather::GetInteriors(float const* around, float reach, InteriorBox* boxes, int mostBoxes, int& boxCount,
  InteriorPrism* prisms, int mostPrisms, int& prismCount)
{
  // The nearest first, each by the sphere round its corners
  struct Found { int Index; float Distance; };
  Found found[InteriorBoxCount > InteriorPrismCount ? InteriorBoxCount : InteriorPrismCount];
  auto distanceTo = [&](float x, float y, float z, float radius) {
    float dx = x - around[0], dy = y - around[1], dz = z - around[2];
    return sqrtf(dx * dx + dy * dy + dz * dz) - radius;
  };
  int n = 0;
  for (int i = 0; i < InteriorBoxCount; ++i)
  {
    InteriorBoxData const& b = InteriorBoxes[i];
    float d = distanceTo(b.Middle[0], b.Middle[1], b.Middle[2], sqrtf(b.Half[0] * b.Half[0] + b.Half[1] * b.Half[1] + b.Half[2] * b.Half[2]));
    if (d <= reach)
      found[n++] = { i, d };
  }
  std::sort(found, found + n, [](Found const& a, Found const& b) { return a.Distance < b.Distance; });
  boxCount = (std::min)(n, mostBoxes);
  for (int k = 0; k < boxCount; ++k)
  {
    InteriorBoxData const& b = InteriorBoxes[found[k].Index];
    memcpy(boxes[k].Middle, b.Middle, sizeof(b.Middle));
    memcpy(boxes[k].Axis, b.Axis, sizeof(b.Axis));
    memcpy(boxes[k].Half, b.Half, sizeof(b.Half));
    boxes[k].Generic = b.Generic;
  }
  n = 0;
  for (int i = 0; i < InteriorPrismCount; ++i)
  {
    InteriorPrismData const& p = InteriorPrisms[i];
    float mx = 0.f, mz = 0.f;
    for (int j = 0; j < p.Count; ++j)
    {
      mx += p.Points[j][0] / p.Count;
      mz += p.Points[j][1] / p.Count;
    }
    float r = 0.f;
    for (int j = 0; j < p.Count; ++j)
      r = (std::max)(r, sqrtf((p.Points[j][0] - mx) * (p.Points[j][0] - mx) + (p.Points[j][1] - mz) * (p.Points[j][1] - mz)));
    float d = distanceTo(mx, p.Floor + 0.5f * p.Height, mz, sqrtf(r * r + 0.25f * p.Height * p.Height));
    if (d <= reach)
      found[n++] = { i, d };
  }
  std::sort(found, found + n, [](Found const& a, Found const& b) { return a.Distance < b.Distance; });
  prismCount = (std::min)(n, mostPrisms);
  for (int k = 0; k < prismCount; ++k)
  {
    InteriorPrismData const& p = InteriorPrisms[found[k].Index];
    prisms[k].Floor = p.Floor;
    prisms[k].Height = p.Height;
    prisms[k].Count = p.Count;
    prisms[k].Generic = p.Generic;
    memcpy(prisms[k].Points, p.Points, sizeof(p.Points));
  }
}

float weather::ShelterAt(float const* world, float* knownOpen)
{
  // WetLook.hlsl's ShelterAt and ShelterInSquare, and its KnownOpenAt and
  // KnownOpenInSquare, the same sums over the squares GetShelter gives it
  // (none known: open, and not known to be)
  if (knownOpen)
    *knownOpen = 0.f;
  std::lock_guard<std::mutex> lock(g_Mutex);
  if (!g_RoofsKnown || g_ShelterGrid.size() != (size_t)(GridSize * GridSize * 4))
  {
    // No squares: the game's interiors alone (as WetLook.hlsl's ShelterAt)
    return InteriorAt(world, 0.f);
  }
  // Per square: how much is over the point, and how surely it's open (0 not
  // looked at or outside the squares)
  auto inSquare = [&](int gx, int gz, float& open)
  {
    open = 0.f;
    if (gx < 0 || gz < 0 || gx >= GridSize || gz >= GridSize)
      return 0.f;
    float const* square = &g_ShelterGrid[(gz * GridSize + gx) * 4];
    if (square[0] < 0.5f)
      return 0.f;
    float under = (std::min)((std::max)(square[1] - 2.f - world[1], 0.f), 1.f);
    float roofed = square[2] < 1e8f ? (std::min)((std::max)(square[2] + 1.f - world[1], 0.f), 1.f) : 0.f;
    float over = (std::max)(under, roofed);
    open = 1.f - over;
    return over;
  };
  float fx = (world[0] - g_RainGridOrigin[0] * CellSize) / CellSize - 0.5f;
  float fz = (world[2] - g_RainGridOrigin[1] * CellSize) / CellSize - 0.5f;
  float cornerX = floorf(fx), cornerZ = floorf(fz);
  float tx = fx - cornerX, tz = fz - cornerZ;
  int gx = (int)cornerX, gz = (int)cornerZ;
  float oa, ob, od, oe;
  float a = inSquare(gx, gz, oa), b = inSquare(gx + 1, gz, ob), d = inSquare(gx, gz + 1, od), e = inSquare(gx + 1, gz + 1, oe);
  // How much the squares have looked at it (WetLook.hlsl's SquaresKnowAt)
  auto looked = [&](int x, int z)
  {
    return x < 0 || z < 0 || x >= GridSize || z >= GridSize || g_ShelterGrid[(z * GridSize + x) * 4] < 0.5f ? 0.f : 1.f;
  };
  float ka = looked(gx, gz), kb = looked(gx + 1, gz), kd = looked(gx, gz + 1), ke = looked(gx + 1, gz + 1);
  float known = (ka + (kb - ka) * tx) + ((kd + (ke - kd) * tx) - (ka + (kb - ka) * tx)) * tz;
  // Inside one of the game's interiors: as much over it, and not open (as
  // WetLook.hlsl's InteriorAt in its ShelterAt and KnownOpenAt)
  float interior = InteriorAt(world, known);
  if (knownOpen)
  {
    float first = oa + (ob - oa) * tx, second = od + (oe - od) * tx;
    *knownOpen = (std::min)(first + (second - first) * tz, 1.f - interior);
  }
  float first = a + (b - a) * tx, second = d + (e - d) * tx;
  return (std::max)(first + (second - first) * tz, interior);
}

weather::RainyLook weather::GetRainyLook()
{
  std::lock_guard<std::mutex> lock(g_Mutex);
  return LookNow();
}

weather::SnowFall weather::GetSnow()
{
  std::lock_guard<std::mutex> lock(g_Mutex);
  SnowFall s{};
  s.Chosen = SnowChosen() && !g_shutdown;
  if (!s.Chosen)
    return s;
  if (g_SnowOn && g_HaveCamera)
    s.Amount = (std::min)(1.f, g_Intensity);
  // (the share of the flakes' budget, WetLook.cpp: a fifth at intensity 1;
  // from 2 up the flakes grow, clumped as heavy snow's are, x 1.5 at 5)
  float intensity = SnowIntensityNow();
  s.Intensity = intensity;
  s.Density = intensity / MostSnowIntensity;
  s.Size = (std::max)(0.3f, (std::min)(g_Settings.FlakeSize, 3.f)) *
    (1.f + 0.5f * (std::max)(0.f, (std::min)((intensity - 2.f) / 3.f, 1.f)));
  s.Seconds = g_SnowClock;
  s.Period = SnowClockWrap;
  s.Wind[0] = g_SnowWind[0];
  s.Wind[1] = g_SnowWind[1];
  s.Drift[0] = g_SnowDrift[0];
  s.Drift[1] = g_SnowDrift[1];
  s.Cover = g_SnowCover * (std::max)(0.f, (std::min)(g_Settings.SnowCover, 1.f));
  s.Brightness = (std::max)(0.1f, (std::min)(g_Settings.SnowBrightness, 4.f));
  s.CoverUpTo = (std::max)(g_Settings.SnowUpTo, 10.f);
  s.KeepOut = g_Settings.SnowKeepOut;
  s.Turbulence = g_SnowTurbulence;
  s.Blizzard = g_Settings.Blizzard;
  s.Pile = g_SnowPile * SnowPileMost * (std::max)(0.f, (std::min)(g_Settings.SnowCover, 1.f));
  return s;
}

bool weather::SnowStepSound()
{
  std::lock_guard<std::mutex> lock(g_Mutex);
  return g_Settings.SnowStepSound;
}

int weather::WetGroundSteps()
{
  std::lock_guard<std::mutex> lock(g_Mutex);
  return g_Settings.WetGroundSteps;
}

float weather::InteriorAmountAt(float const* world)
{
  // The named ones only (the generic lighting volumes reach over open
  // alleys), soft at their faces as the wet look's InteriorAt where the
  // squares have looked; the table is fixed, so no lock
  return InteriorAt(world, 1.f);
}

weather::Haze weather::GetHaze()
{
  // How thick (per metre: what's let through falls off as exp(-distance x
  // it)): heavy rain hides what's past about a kilometre (3 / 1000 m), heavy
  // snow what's past a few hundred metres; less the lighter it falls; half
  // while a look's fog is on (it hides the distance already). Its veils
  // drift with the wind: the snow's when it snows, the rain's (2 - 8 m/s, a
  // fixed way, the harder it rains the faster) when it rains; their drift
  // wraps at 60 km. Render thread; held while the game is paused.
  static double s_ms = 0.0;   // render thread (wetlook::TimeNow, the steady clock)
  double ms = wetlook::TimeNow();
  float dt = s_ms > 0.0 && !gamestate::Paused() ? (std::min)((float)((ms - s_ms) / 1000.0), 0.25f) : 0.f;
  s_ms = ms;
  std::lock_guard<std::mutex> lock(g_Mutex);
  Haze h{};
  float amount = (std::max)(0.f, (std::min)(g_Settings.Haze, 5.f));
  float wind[2] = { 0.f, 0.f };
  if (g_RainOn)
  {
    float rain = RainAmountNow();
    h.Density = 0.0035f * powf(rain, 1.3f) * amount;
    float speed = 2.f + 6.f * rain;
    wind[0] = 0.77f * speed;
    wind[1] = 0.64f * speed;
  }
  else if (g_SnowOn)
  {
    // (its own slider, VISUALS > Snow's Snow haze, x 3.4 in a blizzard: a
    // real one is seen through under about 400 m. At 1, 0.0022 a metre: what's
    // past about 1.4 km hidden. Not the intensity's, nor the rain's Haze: the
    // user, 2026-10-26, "snow intensity shouldn't add any fog it was a bad
    // idea make a diffrent slider for that")
    h.Density = 0.0022f * (std::max)(0.f, (std::min)(g_Settings.SnowHaze, 8.f)) * (g_Settings.Blizzard ? 3.4f : 1.f) *
      (std::min)(1.f, g_Intensity);
    h.Snow = true;
    wind[0] = g_SnowWind[0];
    wind[1] = g_SnowWind[1];
  }
  if (h.Density > 0.f)
    h.Density *= 1.f - 0.5f * (std::max)(0.f, (std::min)(g_LookAmount, 1.f));
  static float s_drift[2] = {};   // under g_Mutex
  for (int i = 0; i < 2; ++i)
    s_drift[i] = fmodf(s_drift[i] + wind[i] * dt, 60000.f);
  h.Drift[0] = s_drift[0];
  h.Drift[1] = s_drift[1];
  h.Outside = 1.f - (std::max)(0.f, (std::min)(g_LookIndoors, 1.f));
  return h;
}

int weather::GetPipes(float (*outlets)[4], int most, float& flow)
{
  std::lock_guard<std::mutex> lock(g_Mutex);
  flow = g_PipeFlow;
  int n = 0;
  for (PipeOutlet const& o : g_Pipes)
  {
    if (o.State != 1 || n >= most)
      continue;
    outlets[n][0] = o.At[0];
    outlets[n][1] = o.At[1];
    outlets[n][2] = o.At[2];
    outlets[n][3] = o.Ground;
    ++n;
  }
  return n;
}

float weather::IceAmount()
{
  // (its pieces are drawn as they are, all of them, by WetLook.cpp's ice
  // pass: under the open sky only, so the game's interiors and roofs keep
  // their water)
  std::lock_guard<std::mutex> lock(g_Mutex);
  return SnowChosen() && !g_shutdown ? g_IceAmount : 0.f;
}

int weather::DryBoxes(float (*mins)[3], float (*maxs)[3], int most)
{
  std::lock_guard<std::mutex> lock(g_Mutex);
  int count = 0;
  for (Box const& b : g_DryBoxes)
  {
    if (count >= most)
      break;
    memcpy(mins[count], b.Min, sizeof(b.Min));
    memcpy(maxs[count], b.Max, sizeof(b.Max));
    ++count;
  }
  return count;
}

float weather::LightningFlash()
{
  std::lock_guard<std::mutex> lock(g_Mutex);
  // The brightest of the strikes flashing now
  ULONGLONG now = GetTickCount64();
  float level = 0.f;
  for (Strike const& st : g_Strikes)
  {
    if (!st.Started || st.Strength <= 0.f)
      continue;
    float t = (now - st.Tick) / 1000.f;
    if (t <= FlashSeconds)
      level = (std::max)(level, FlashPulse(t) * st.Strength);
  }
  return level * g_Settings.LightningBrightness;
}

int weather::ZonePoints(float (*points)[3], int most)
{
  std::lock_guard<std::mutex> lock(g_Mutex);
  int count = 0;
  if (!g_RainOn || !g_Settings.LoadZones)
    return 0;
  for (size_t k = 0; k < RainKinds && count < most; ++k)
  {
    if (k == 1 && !g_Settings.Heavy)
      continue;
    memcpy(points[count++], g_Rain[k].ZonePoint, sizeof(points[0]));
  }
  return count;
}

namespace
{
  // The forecast now, for the menus (g_Mutex held): raining (snowing) until
  // when, or dry and the next spell
  void ForecastStatus(bool snow)
  {
    if (g_TimeOfDay < 0.f)
    {
      ImGui::TextDisabled("(no time of day: no level loaded)");
      return;
    }
    // Now, or the next spell
    float nowSeconds = g_TimeOfDay;
    float nextStart = 1e9f, nextEnd = 0.f, nowEnd = -1.f, nowStart = 0.f;
    Spell const* nowSpell = nullptr;
    // Yesterday's spells running past midnight, today's, and tomorrow's
    for (Spell const& sp : g_Spells)
      for (float day : { 86400.f, 0.f, -86400.f })
      {
        float st = sp.Start - day, en = sp.End - day;
        if (nowSeconds >= st && nowSeconds <= en)
        {
          nowEnd = en;
          nowStart = st;
          nowSpell = &sp;
        }
        else if (st > nowSeconds && st < nextStart)
        {
          nextStart = st;
          nextEnd = en;
        }
      }
    auto clock = [](float seconds, char* out, size_t size) {
      int m = (int)(seconds / 60.f + 0.5f);
      m = ((m % 1440) + 1440) % 1440;
      sprintf_s(out, size, "%02d:%02d", m / 60, m % 60);
    };
    char a[16], b[16];
    if (nowEnd >= 0.f)
    {
      clock(nowEnd, a, sizeof(a));
      ImGui::TextColored(ImVec4(0.45f, 0.9f, 0.45f, 1.f), "%s (%.0f %%), until about %s", snow ? "Snowing" : "Raining",
        100.f * g_Intensity, a);
      // (the rain's spell's own weather, see Spell)
      if (!snow && nowSpell)
      {
        Spell const& sp = *nowSpell;
        if (sp.Storm > 0.f)
        {
          float length = nowEnd - nowStart;
          clock(nowStart + sp.StormFrom * length, a, sizeof(a));
          clock(nowStart + sp.StormTo * length, b, sizeof(b));
          ImGui::TextDisabled("This spell: rain up to %.0f %%, the look %.0f %%, fog x %.1f, a storm %s - %s (x %.1f)",
            100.f * sp.Rain, 100.f * sp.Look, sp.Fog, a, b, sp.Storm);
        }
        else
          ImGui::TextDisabled("This spell: rain up to %.0f %%, the look %.0f %%, fog x %.1f, no thunder", 100.f * sp.Rain,
            100.f * sp.Look, sp.Fog);
      }
    }
    else if (nextStart < 1e8f)
    {
      clock(nextStart, a, sizeof(a));
      clock(nextEnd, b, sizeof(b));
      ImGui::TextDisabled("Dry%s; next %s %s - %s", g_Intensity > 0.01f ? " (easing off)" : "", snow ? "snow" : "rain", a, b);
    }
    else
      ImGui::TextDisabled("No %s in this forecast", snow ? "snow" : "rain");
  }
}

void weather::DrawUI()
{
  std::lock_guard<std::mutex> lock(g_Mutex);
  bool following = g_Following.load(std::memory_order_relaxed);
  if (!ImGui::CollapsingHeader(following ? "Rain    [on]###Weather" : "Rain###Weather"))
    return;
  ImGui::PushTextWrapPos(0.f);
  ImGui::TextDisabled("The game has no weather of its own: its rain is placed in the Release district. Here it "
    "follows the camera anywhere in the city. The clouds are the effect \"Clouds\" above.");
  ImGui::PopTextWrapPos();
  // Snow or rain (VISUALS > Snow): ticking either rain turns the snow off
  if (SnowChosen())
    ImGui::TextColored(ImVec4(1.f, 0.8f, 0.2f, 1.f), "Snow is on (Snow, below): Rain or Random rain turns it off");

  if (!g_Settings.RandomRain)
  {
    if (ImGui::Checkbox("Rain##Weather", &g_Settings.Rain) && g_Settings.Rain)
      g_Settings.Snow = g_Settings.RandomSnow = false;
  }
  else
    ImGui::TextDisabled("Rain: by the forecast");
  ImGui::SameLine();
  if (ImGui::Checkbox("Random rain##Weather", &g_Settings.RandomRain) && g_Settings.RandomRain)
    g_Settings.Snow = g_Settings.RandomSnow = false;
  if (g_Settings.RandomRain)
  {
    ImGui::Indent();
    ImGui::SliderFloat("How much of the day it rains##Weather", &g_Settings.RainChance, 0.05f, 0.9f, "%.2f");
    ImGui::Checkbox("Thunder and lightning with it##Weather", &g_Settings.ThunderWithRain);
    ImGui::Checkbox("Fog, grey sky, no sun with it##Weather", &g_Settings.LookWithRain);
    if (ImGui::Button("New forecast##Weather"))
    {
      g_ForecastSeed = g_ForecastSeed * 1664525u + 1013904223u + (unsigned int)GetTickCount64();
      g_ForecastChance = -1.f;
    }
    ImGui::SameLine();
    ForecastStatus(false);
    ImGui::Unindent();
  }
  if (g_Settings.Rain || g_Settings.RandomRain)
  {
    ImGui::Indent();
    ImGui::SliderFloat("Rain strength##Weather", &g_Settings.Strength, 0.05f, 1.5f, "%.2f");
    if (g_RainParamCode < 0)
      ImGui::TextColored(ImVec4(1.f, 0.4f, 0.4f, 1.f), "Can't give the rain a strength: the game's code isn't what the tools know");
    else if (g_RainParamShown.load(std::memory_order_relaxed))
      ImGui::TextDisabled("Given to the game (it had %.2f)", g_RainParamGameValue.load(std::memory_order_relaxed));
    ImGui::SliderFloat("Rain in the distance##Weather", &g_Settings.DistantRain, 0.f, 1.f, "%.2f");
    ImGui::SliderFloat("Haze##Weather", &g_Settings.Haze, 0.f, 5.f, "x %.2f");
    ImGui::Checkbox("Heavy rain##Weather", &g_Settings.Heavy);
    ImGui::Checkbox("Keep rain out of buildings##Weather", &g_Settings.KeepOut);
    ImGui::Checkbox("Load the game's rain##Weather", &g_Settings.LoadZones);
    for (size_t k = 0; k < RainKinds; ++k)
    {
      if (k == 1 && !g_Settings.Heavy)
        continue;
      GameRain const& r = g_Rain[k];
      if (g_InstanceCode < 0)
        ImGui::TextColored(ImVec4(1.f, 0.4f, 0.4f, 1.f), "No rain: the game's effect code isn't what the tools know");
      else if (r.Followed)
        ImGui::TextColored(ImVec4(0.45f, 0.9f, 0.45f, 1.f), "The game's %s follows the camera", r.Label);
      else if (r.Instance)
        ImGui::TextColored(ImVec4(0.45f, 0.9f, 0.45f, 1.f), "The game's %s is there", r.Label);
      else if (!g_Settings.LoadZones)
        ImGui::TextColored(ImVec4(1.f, 0.8f, 0.2f, 1.f), "The game's %s isn't loaded: tick \"Load the game's rain\"", r.Label);
      else
        ImGui::TextColored(ImVec4(1.f, 0.8f, 0.2f, 1.f), "Looking for the game's %s (its zone loads first)%s", r.Label,
          g_LookingSince && GetTickCount64() - g_LookingSince > 30000 ? ": not found in 30 s" : "");
    }
    ImGui::Checkbox("Wind with the rain##Weather", &g_Settings.RainWind);
    if (g_Settings.RainWind)
    {
      ImGui::SameLine();
      ImGui::PushItemWidth(120.f);
      ImGui::SliderFloat("##RainWindAmount", &g_Settings.RainWindAmount, 0.f, 2.f, "x %.2f");
      ImGui::PopItemWidth();
      ImGui::SliderFloat("Turbulence##RainWind", &g_Settings.RainTurbulence, 0.f, 1.f, "%.2f");
    }
    ImGui::Checkbox("Rain sound##Weather", &g_Settings.RainSound);

    if (g_Settings.RainSound)
    {
      if (g_RainSound.OwnVoice)
        ImGui::TextColored(ImVec4(0.45f, 0.9f, 0.45f, 1.f), "The rain's sound plays");
      else if (!g_Settings.LoadZones)
        ImGui::TextColored(ImVec4(1.f, 0.8f, 0.2f, 1.f), "The rain's sound needs \"Load the game's rain\"");
      else if (!g_RainSound.Asset)
        ImGui::TextColored(ImVec4(1.f, 0.8f, 0.2f, 1.f), "Waiting for the game to load the rain's sound");
      else if (!g_RainSound.EventCount)
        ImGui::TextColored(ImVec4(1.f, 0.8f, 0.2f, 1.f), "Waiting for the game to start the rain's sound once");
    }
    if (g_Settings.KeepOut)
    {
      if (g_CodeState < 0)
        ImGui::TextColored(ImVec4(1.f, 0.4f, 0.4f, 1.f), "Can't keep it out of buildings: the game's code isn't what the tools know");
      else if (!collision::Available())
        ImGui::TextDisabled("Kept out of buildings once a level is loaded");
      else if (following)
        ImGui::TextDisabled("Kept out under %d roofs%s", g_LastBoxCount < 0 ? 0 : g_LastBoxCount,
          g_SlotsFull ? " (the game's exclusion volumes are full)" : "");
    }
    ImGui::Unindent();
  }

  ImGui::Checkbox("Rainy city look##Weather", &g_Settings.RainyLook);
  // The look's sky, fog and sun follow TheNollz' Rain mod's overcast
  // environments where it changes them (its help text, gone 2026-11-04, said so)
  if (!g_Settings.RainyLook && g_Settings.RandomRain && g_Settings.LookWithRain)
  {
    ImGui::SameLine();
    ImGui::TextDisabled("(comes with the random rain)");
  }
  if (g_Settings.RainyLook || (g_Settings.RandomRain && g_Settings.LookWithRain))
  {
    ImGui::Indent();
    ImGui::SliderFloat("Grey sky##Weather", &g_Settings.GreySky, 0.f, 1.f, "%.2f");
    ImGui::SliderFloat("Less sun##Weather", &g_Settings.LessSun, 0.f, 1.f, "%.2f");
    ImGui::SliderFloat("Fog: seen through up to##Weather", &g_Settings.FogSeenUpTo, 20.f, 6000.f, "%.0f m", 3.f);
    ImGui::ItemTooltip("The farthest you can see through the fog (the game's height fog's visibility range). Under it the "
      "fog thickens with the hour as in TheNollz' Rain mod (half the game's by day, about 3000 m; about 1600 m at dawn and "
      "dusk; 250 m at night), divided by Fog: how thick. At 6000 m only that.");
    // (to 10, 2026-10-26, the user: "Slider fog should let you add more fog")
    ImGui::SliderFloat("Fog: how thick##Weather", &g_Settings.FogThickness, 0.5f, 25.f, "%.2fx", 2.f);
    ImGui::ItemTooltip("How much thicker than TheNollz' Rain mod's the fog is at every hour: what it's seen through is "
      "divided by this (1.75 by default: about 1700 m by day, 900 m at dawn and dusk, 140 m at night; 1 the mod's own; up "
      "to 25).");
    ImGui::SliderFloat("Fog: how high##Weather", &g_Settings.FogDepth, 20.f, 3000.f, "%.0f m", 2.f);
    ImGui::ItemTooltip("How high up the fog goes (the game's height fog's depth); the city's towers are 200 - 400 m.");
    ImGui::Unindent();
  }

  ImGui::Checkbox("Thunder and lightning##Weather", &g_Settings.Thunder);
  ImGui::SameLine();
  if (ImGui::Button("Strike now##Weather"))
    g_StrikeNow = true;
  ImGui::ItemTooltip("Lightning at once: a burst of 1 - 6 strikes, the first at the nearest distance below.");
  bool thunderWithRain = g_Settings.RandomRain && g_Settings.ThunderWithRain;
  if (!g_Settings.Thunder && thunderWithRain)
    ImGui::TextDisabled("Comes with the random rain");
  if (g_Settings.Thunder || thunderWithRain)
  {
    ImGui::Indent();
    ImGui::SliderFloat("Every##Weather", &g_Settings.ThunderEvery, 4.f, 180.f, "%.0f s on average", 2.f);
    if (g_Settings.RandomRain)
      ImGui::ItemTooltip("With Random rain: in a storm, divided by the storm's rate (0.5 - 1.5); less often as it comes and "
        "goes, none outside the storms.");
    ImGui::SliderFloat("Nearest##Weather", &g_Settings.NearestStrike, 50.f, 3000.f, "%.0f m", 2.f);
    ImGui::SliderFloat("Farthest##Weather", &g_Settings.FarthestStrike, 50.f, 3000.f, "%.0f m", 2.f);
    ImGui::Unindent();
  }
  ImGui::Checkbox("Lightning bolts##Weather", &g_Settings.Bolts);
  ImGui::SliderFloat("Thunder volume##Weather", &g_Settings.ThunderVolume, 0.f, 1.f, "%.2f");
  ImGui::SliderFloat("Lightning brightness##Weather", &g_Settings.LightningBrightness, 0.f, 4.f, "%.2f");
  if (thunder::Tried() && !thunder::Available())
    ImGui::TextColored(ImVec4(1.f, 0.4f, 0.4f, 1.f), "Windows' audio couldn't be opened: no thunder heard");

  // The wet surfaces (wetlook): drawn by the tools, the game has no wet look of
  // its own
  bool wetChanged = ImGui::Checkbox("Wet surfaces##Weather", &g_Wet.On);
  if (g_Wet.On)
  {
    ImGui::Indent();
    wetChanged |= ImGui::SliderFloat("Wetness##Weather", &g_Wet.Wetness, 0.f, 1.f, "%.2f");
    wetChanged |= ImGui::SliderFloat("Puddles##Weather", &g_Wet.Puddles, 0.f, 1.f, "%.2f");
    ImGui::ItemTooltip("How much of the ground is puddles. Faith's steps and landings in them are the game's own in water (its "
      "sounds and splashes).");
    if (g_Settings.RandomRain && !SnowChosen())
      ImGui::TextDisabled("Random rain: the ground %.0f %% as wet as Wetness, the puddles %.0f %% of Puddles (as its rain has "
        "made them; they dry after it)", 100.f * g_GroundWet, 100.f * g_PuddleFill);
    wetChanged |= ImGui::SliderFloat("Puddle size##Weather", &g_Wet.PuddleSize, 0.5f, 10.f, "%.1f m");
    wetChanged |= ImGui::SliderFloat("Reflections##Weather", &g_Wet.Reflections, 0.f, 1.f, "%.2f");
    // The wet ground in the game's own G-buffer (GbufferWet.h; 2026-11-02,
    // the user: "Lets go for A option")
    {
      bool inGbuffer = gbufferwet::On();
      if (ImGui::Checkbox("In the game's own lighting##Weather", &inGbuffer))
        gbufferwet::SetOn(inGbuffer);
      ImGui::ItemTooltip("On (the default): the wet ground is written into what the game lights (smoother, darker, the puddles "
        "flat and rippled), so the game's own reflections, cube maps and lights show it, with no halo round what stands in "
        "front. Reflections then sets how smooth the wet ground is: the game reflects the screen on what's smooth enough. "
        "Off: the tools' own reflection drawn over the finished frame, as before.");
      // (diagnostic, 2026-11-03: what the game's G-buffer holds where the wet
      // ground looks wrong, see GbufferWet.h; shown only with the diagnostics on)
      if (util::DiagnosticsOn())
      {
        if (inGbuffer && ImGui::Button("Save the G-buffer (diagnostic)##Weather"))
          gbufferwet::RequestDump();
        ImGui::ItemTooltip("Saves what the game's G-buffer holds this frame, before and after the wet ground goes into it, "
          "into Cinematic Tools\\GbufferDump (about 250 MB at 4K), for finding what looks wrong where you're looking. "
          "Only while it rains and the wet ground is in the game's own lighting.");
      }
    }
    wetChanged |= ImGui::SliderFloat("Wet up to##Weather", &g_Wet.FadeDistance, 30.f, 600.f, "%.0f m");
    ImGui::ItemTooltip("How far from the camera the ground looks wet (it fades out by then).");
    wetChanged |= ImGui::Checkbox("Ripples in the rain##Weather", &g_Wet.Ripples);
    ImGui::Checkbox("Water out of the drainpipes##Weather", &g_Settings.PipeWater);
    ImGui::ItemTooltip("While it rains, water runs out of the foot of the city's downpipes and climbable pipes near the "
      "camera (45 m) and splashes where it lands (rings in a puddle), as hard as it rains. Only pipes standing upright "
      "under the open sky, not inside, that don't end in a floor.");
    ImGui::PushItemWidth(220.f);
    ImGui::Combo("Steps on the wet ground##Weather", &g_Settings.WetGroundSteps,
      "As the game has them (water only in the puddles)\0The water (as in the puddles)\0The game's wet concrete\0");
    ImGui::PopItemWidth();
    ImGui::ItemTooltip("Faith's steps and landings on the wet ground outside, off the puddles: left as the game has them (the "
      "default: only her steps in a puddle sound like water), the game's own steps in water (as in the puddles: its sounds "
      "and splashes), or its wet concrete (concrete with water in the sound, and the water's splashes).");
    if (wetlook::Drawn())
      ImGui::TextColored(ImVec4(0.45f, 0.9f, 0.45f, 1.f), "Drawn");
    else
      ImGui::TextColored(ImVec4(1.f, 0.8f, 0.2f, 1.f), "Not drawn yet (it needs a level loaded and the game's depth)");
    ImGui::Unindent();
  }
  if (wetChanged)
    wetlook::Set(g_Wet);
  if (g_Wet.On && SnowChosen())
    ImGui::TextDisabled("While it's snow, not rain, the snow on the ground is drawn instead (Snow, below)");
  // "The game's wet look" (RainShaderParam) is out of the menu and off
  // (GameWet 0): no shader of the game reads that parameter (see GameWet)
}

// VISUALS > Snow, its own header under Weather (2026-10-26, the user: "this
// will be another header right?"). The game has no snow (no snow effect,
// texture or sound in its files), so the tools draw it over the finished
// frame (WetLook.hlsl: SnowAt, the snow on the ground); the look is the
// weather's (GiveLook, snowy); see Research_Snow_2026-10-26.md.
void weather::DrawSnowUI()
{
  std::lock_guard<std::mutex> lock(g_Mutex);
  if (!ImGui::CollapsingHeader(g_SnowOn ? "Snow    [snowing]###Snow" : "Snow###Snow"))
    return;
  ImGui::PushTextWrapPos(0.f);
  ImGui::TextDisabled("The game has no snow of its own (no snow effect, texture or sound in its files), so the tools draw "
    "it over the finished frame. Snow or rain: turning snow on turns the rain off, and the other way round. Thunder and "
    "lightning stay under Weather.");
  ImGui::PopTextWrapPos();

  if (!g_Settings.RandomSnow)
  {
    if (ImGui::Checkbox("Snow##Snow", &g_Settings.Snow) && g_Settings.Snow)
      g_Settings.Rain = g_Settings.RandomRain = false;
  }
  else
    ImGui::TextDisabled("Snow: by the forecast");
  ImGui::SameLine();
  if (ImGui::Checkbox("Random snow##Snow", &g_Settings.RandomSnow) && g_Settings.RandomSnow)
    g_Settings.Rain = g_Settings.RandomRain = false;
  if (g_Settings.RandomSnow)
  {
    ImGui::Indent();
    ImGui::SliderFloat("How much of the day it snows##Snow", &g_Settings.SnowChance, 0.05f, 0.9f, "%.2f");
    ImGui::Checkbox("Fog, grey sky, no sun with it##Snow", &g_Settings.SnowLookWithSnow);
    if (ImGui::Button("New forecast##Snow"))
    {
      g_ForecastSeed = g_ForecastSeed * 1664525u + 1013904223u + (unsigned int)GetTickCount64();
      g_ForecastChance = -1.f;
    }
    ImGui::SameLine();
    ForecastStatus(true);
    ImGui::Unindent();
  }
  if (SnowChosen())
  {
    ImGui::Indent();
    ImGui::SliderFloat("Snow intensity##Snow", &g_Settings.SnowIntensity, 0.1f, MostSnowIntensity, "%.2f");
    ImGui::ItemTooltip("How hard it snows: under 0.7 light, 1 moderate, from 2 heavy, 7.5 the most. More flakes (up to "
      "310,000 drawn), bigger from 2 up (clumped together, as heavy snow's are) and the wind's sound louder. It adds no "
      "fog or haze: Snow haze below and the snowy city look's Snow fog are their own.");
    ImGui::SliderFloat("Snow haze##Snow", &g_Settings.SnowHaze, 0.f, 8.f, "x %.2f");
    ImGui::ItemTooltip("How thick the falling snow's haze is: what's farther off greyed out more, in slow veils drifting "
      "with the wind, lit by the scene's own light. 1: what's past about 1.4 km hidden, 3: past about 450 m, up to 8; past "
      "3 it hides more of what's far off too; a blizzard "
      "3.4 times thicker. Over everything while you're outside, only over what's out in the open while you're inside; "
      "half as much while the snowy look's fog is on. 0: off.");
    ImGui::SliderFloat("Flake size##Snow", &g_Settings.FlakeSize, 0.5f, 2.5f, "%.2f");
    ImGui::ItemTooltip("1: flakes 6 - 14 mm across. Far ones are never drawn smaller than a pixel (fainter instead).");
    ImGui::SliderFloat("Wind##Snow", &g_Settings.SnowWind, 0.f, 6.f, "%.1f m/s");
    ImGui::ItemTooltip("How fast the wind carries the snow on average (0.5 by default). It gusts every few seconds, "
      "swinging its way about 40 degrees either way, and its way wanders slowly over minutes. On top of it every flake "
      "swirls in the air's eddies its own way (more as the wind blows and in a blizzard), so the snow comes from above "
      "and both sides, not as one sheet. It's the snow's own wind, not the game's (the game's wind's way isn't known "
      "well enough to follow).");
    ImGui::Checkbox("Blizzard##Snow", &g_Settings.Blizzard);
    ImGui::Checkbox("Wind sound##Snow", &g_Settings.WindSound);
    if (g_Settings.WindSound)
    {
      ImGui::SameLine();
      ImGui::PushItemWidth(120.f);
      ImGui::SliderFloat("##WindVolume", &g_Settings.WindVolume, 0.f, 2.f, "x %.2f");
      ImGui::PopItemWidth();
    }
    ImGui::Checkbox("Keep snow out of buildings##Snow", &g_Settings.SnowKeepOut);
    if (g_Settings.SnowKeepOut && !collision::Available())
      ImGui::TextDisabled("Kept out of buildings once a level is loaded");
    ImGui::Checkbox("Freeze waterfalls and fountains##Snow", &g_Settings.FreezeWater);
    if (g_Settings.FreezeWater && g_SnowOn)
    {
      int frozen = 0;
      for (WaterTemplate const& w : g_WaterTemplates)
        frozen += w.Frozen ? 1 : 0;
      if (g_WaterCode < 0)
        ImGui::TextColored(ImVec4(1.f, 0.4f, 0.4f, 1.f), "Can't freeze them: the game's code isn't what the tools know");
      else if (g_WaterEffects.empty())
        ImGui::TextDisabled("No waterfall or fountain near (loaded)");
      else
        ImGui::TextDisabled("%d waterfalls and fountains near, %d of their emitters frozen", (int)g_WaterEffects.size(), frozen);
    }
    ImGui::Unindent();
  }

  ImGui::SliderFloat("Snow on the ground##Snow", &g_Settings.SnowCover, 0.f, 1.f, "%.2f");
  if (g_Settings.SnowCover > 0.f)
  {
    ImGui::Indent();
    // (2026-11-01, the user: "Snow looks good now but it's not as apparent of
    // an effect now can we increase it?")
    ImGui::SliderFloat("Snow brightness##Snow", &g_Settings.SnowBrightness, 0.5f, 4.f, "x %.2f");
    ImGui::ItemTooltip("How bright the snow on the ground is: the light that reaches the ground there (as the game lit it, "
      "without the ground's own colour or its reflections) times this. 1: what fresh snow gives back of it, 85 %; 2.5 by "
      "default. Never over white, so its shading still shows.");
    ImGui::SliderFloat("Settles over##Snow", &g_Settings.SnowSettleMinutes, 0.2f, 30.f, "%.1f min", 2.f);
    ImGui::ItemTooltip("Real minutes for all of it to settle while it snows hard (longer in light snow). It melts in three "
      "times that once the snow stops.");
    ImGui::SliderFloat("On the ground up to##Snow", &g_Settings.SnowUpTo, 50.f, 1000.f, "%.0f m");
    ImGui::ItemTooltip("How far from the camera the ground shows snow (it fades out by then).");
    if (ImGui::Button("Settle now##Snow"))
    {
      g_SnowCover = 1.f;
      g_SnowPile = 1.f;
    }
    ImGui::SameLine();
    if (ImGui::Button("Clear##Snow"))
    {
      g_SnowCover = 0.f;
      g_SnowPile = 0.f;
    }
    ImGui::SameLine();
    if (SnowChosen())
      ImGui::TextDisabled("On the ground now: %.0f %%, %.0f cm deep", 100.f * g_SnowCover * (std::min)(g_Settings.SnowCover, 1.f),
        100.f * g_SnowPile * SnowPileMost * (std::min)(g_Settings.SnowCover, 1.f));
    else
      ImGui::TextDisabled("(only while snow is on)");
    ImGui::Checkbox("Footstep sound in the snow##Snow", &g_Settings.SnowStepSound);
    ImGui::Unindent();
  }

  ImGui::Checkbox("Snowy city look##Snow", &g_Settings.SnowLook);
  // The look follows Toffiklubitoffi's Snowfall mod where it changes the
  // game's (its help text, gone 2026-11-04, said so)
  if (SnowLookWanted() || g_Settings.SnowLook)
  {
    ImGui::Indent();
    if (!SnowChosen())
      ImGui::TextDisabled("(only while snow is on)");
    ImGui::SliderFloat("Snow fog##Snow", &g_Settings.SnowFog, 0.f, 25.f, "%.2f", 2.f);
    ImGui::ItemTooltip("How much of the snowy look's fog: 0 the game's own, 1 the Snowfall mod's, up to 10 that many times "
      "thicker, up to 25 (at 10 seen through a tenth as far, reaching ten times as high, and hiding nearly all of what's "
      "far off at any hour). A blizzard 1.5 times thicker still.");
    ImGui::SliderFloat("Grey sky##Snow", &g_Settings.GreySky, 0.f, 1.f, "%.2f");
    ImGui::SliderFloat("Less sun##Snow", &g_Settings.LessSun, 0.f, 1.f, "%.2f");
    ImGui::SliderFloat("Fog: seen through up to##Snow", &g_Settings.FogSeenUpTo, 20.f, 6000.f, "%.0f m", 3.f);
    ImGui::ItemTooltip("The farthest you can see through the fog, at most. Under it the snowy look's own by the hour.");
    ImGui::Unindent();
  }
}
