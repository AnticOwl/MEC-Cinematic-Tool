#pragma once
#include <Windows.h>
#include <vector>

// VISUALS > Weather: the game's own rain anywhere in the city, kept out of
// buildings (see Weather.cpp). The clouds and the wind are VISUALS effects
// like the others (VisualsManager).
namespace weather
{
  // Camera hook, every rendered frame, with the frame's final camera (game
  // thread)
  void Update(__int64 pRenderView);
  // Streaming (camera hook): the places to keep loaded for the rain, up to most
  int ZonePoints(float (*points)[3], int most);
  // VISUALS menu (render thread)
  void DrawUI();
  // The sound system's hooks (any thread): the game's rain sound, to play at
  // the camera
  void NoteSoundCreated(__int64 voice, __int64 asset);
  void NoteVoiceEvent(__int64 voice, unsigned int event);
  // Any thread, before a world of the game's goes (a mission's sub-level
  // streamed out: the game's, the replay's; why: what, a literal): the rain's
  // sound the tools made from its asset let go first, at the next update
  // (see Weather.cpp's g_WorldGoing)
  void WorldGoing(char const* why);
  // The visual environment update (game thread): how bright a lightning
  // flash is now (0 none, 1 a near strike's brightest)
  float LightningFlash();
  // Whether thunder and lightning are on (the frame's flash keeps the depth ready)
  bool ThunderOn();
  // The rainy city look's settings (the visual environment update gives it,
  // see VisualsManager::ApplyWeatherLook); any thread
  struct RainyLook
  {
    bool On;
    float GreySky;      // 0 - 1
    float LessSun;      // 0 - 1
    float FogSeenUpTo;  // metres
    float FogDepth;     // metres
    float Hours;        // the game's time of day, hours (the last known)
    float Indoors;      // how much the camera is inside, eased (0 - 1): the look's fog eases off
    bool Snowy;         // the snowy look instead (VISUALS > Snow): its own fog, whiter
    bool Blizzard;      // snowy: its fog thicker still
    float SnowFog;      // snowy: how much of its fog (0 - 1)
    float SnowIntensity;   // snowy: how hard it snows (1 moderate; a blizzard's included): the fog seen through less
    float FogThickness;    // rainy: the hour's fog seen through this many times less (1 TheNollz' Rain mod's)
  };
  RainyLook GetRainyLook();
  // VISUALS > Snow (render thread): its own header, under Weather
  void DrawSnowUI();
  // The snow (render thread, for the wet surfaces' pass, which draws it: see
  // WetLook.hlsl's SnowAt and its snow on the ground). Any thread.
  // Snow brightness's default (VISUALS > Snow; see SnowFall::Brightness)
  // (1.75 until 2026-11-01 18:xx, the user: "make the snow on the ground
  // effect stronger no mistakes")
  const float SnowBrightnessDefault = 2.5f;
  struct SnowFall
  {
    bool Chosen;        // snow, not rain (the wet look gives way to the snow on the ground)
    float Amount;       // how much falls now, 0 - 1 (0: none)
    float Density;      // how many flakes, 0 - 1
    float Size;         // the flakes' size (1 the tools' own)
    float Seconds;      // the snow's own clock (s; it wraps)
    float Period;       // when it wraps (s): the flakes' motions are whole cycles over it
    float Wind[2];      // how fast the wind carries it now (world x, z; m/s), for the flakes' streaks
    float Drift[2];     // how far the wind has carried it (world x, z; metres; it wraps)
    float Cover;        // how much of what faces up is under snow now, 0 - 1
    float CoverUpTo;    // metres: none past it
    bool KeepOut;       // only where the weather's squares know the sky is open
    float Intensity;    // how hard it snows (0.1 - 5, a blizzard's included)
    float Pile;         // metres it has piled up on the ground (0 until it all lies)
    float Turbulence;   // the flakes' eddies' size (0.6 in still air, more in wind and a blizzard)
    bool Blizzard;      // a blizzard (the flakes' streaks longer)
    float Brightness;   // Snow brightness: the snow's light on the ground x it, in linear light (1 fresh snow's own 0.85 of it)
  };
  SnowFall GetSnow();
  // Whether Faith's steps in the snow crunch (VISUALS > Snow; any thread)
  bool SnowStepSound();
  // VISUALS > Weather > Wet surfaces > Steps on the wet ground (any thread):
  // 0 as the game has them, 1 the water's (as in the puddles), 2 the game's
  // wet concrete
  int WetGroundSteps();
  // The ice over the city's water while it snows (VISUALS > Snow > Freeze
  // waterfalls and fountains; WetLook.cpp's ice pass, IceTable.h): how far
  // it has formed, 0 - 1 (0: none). Any thread.
  float IceAmount();
  // The game's interiors (any thread): the volumes the game fades its
  // interior visual environments in by (Weather.cpp), the ones within reach
  // of a point (world), nearest first: boxes (their middle, three unit axes,
  // half sizes along them) and prisms (a floor, a height, up to 8 points x,
  // z); how many of each
  // Generic: one of ve_generic_interiortheview's (it counts only where the
  // roof squares haven't looked, see WetLook.hlsl's InteriorAt)
  struct InteriorBox { float Middle[3]; float Axis[3][3]; float Half[3]; bool Generic; };
  struct InteriorPrism { float Floor; float Height; int Count; float Points[8][2]; bool Generic; };
  void GetInteriors(float const* around, float reach, InteriorBox* boxes, int mostBoxes, int& boxCount,
    InteriorPrism* prisms, int mostPrisms, int& prismCount);
  // The main view's height fog as the game draws it this frame (after the
  // rainy look), for the wet surfaces' pass to fog what it adds the same way
  // (any thread). The game's fog (its sky / fog shader, read 2026-10-25):
  // see WetLook.cpp.
  struct FogParams
  {
    bool On;                  // fog and height fog on
    float FollowCamera;       // HeightFogFollowCamera (fog block + 0x8C)
    float Altitude;           // HeightFogAltitude (+ 0x90)
    float Depth;              // HeightFogDepth (+ 0x94)
    float SeenUpTo;           // HeightFogVisibilityRange (+ 0x98)
    float MinTransmittance;   // the sky's MinHeightFogTransmittance (sky block + 0x24C)
  };
  FogParams GetFog();
  // The wet surfaces (render thread): where it's dry, under the roofs found
  // around the camera (boxes, world min / max); how many
  int DryBoxes(float (*mins)[3], float (*maxs)[3], int most);
  // The rain drawn by the tools past the game's (render thread): how much (0
  // none), the rain's strength against the Release mission's 0.7, whether
  // the camera is under a roof, and how far around it the roofs are known
  // (metres; 0 not looked for)
  struct DistantRain { float Amount; float Strength; bool Indoors; float RoofReach; };
  DistantRain GetDistantRain();
  // How wet the forecast's rain has made the ground (VISUALS > Weather >
  // Random rain; any thread): with Random rain the wet look's Wetness and
  // Puddles are the most there is, Ground and Puddles (0 - 1) how much of
  // them now; without it (Random false) both 1, the sliders as they are
  struct Wetting { bool Random; float Ground; float Puddles; };
  Wetting GetWetting();
  // The squares round the camera open to the sky (render thread): count x
  // count squares of size metres from the corner (x, z), per square 1 open
  // (0 covered or not looked at yet) and the first surface under the camera
  // there; false: not known (the roofs aren't being looked for)
  bool GetOpenSky(std::vector<float>& cells, int& count, float& size, float& cornerX, float& cornerZ);
  // The same squares for the wet surfaces and the lightning (render thread):
  // per square 4 floats: 1 looked at (0 not yet), the first surface under
  // where it was looked from (-1e9 none), the first roof over it (1e9 none),
  // where it was looked from; false: not known
  // edges (if given): per square and side, where the open sky begins out from
  // its middle (m), -1 not known: where the roofs' drips fall (empty: none known)
  bool GetShelter(std::vector<float>& cells, int& count, float& size, float& cornerX, float& cornerZ,
    std::vector<float>* edges = nullptr);
  // The same squares at a point (world; any thread): how much is over it, 0
  // open to the sky - 1 under a roof, as WetLook.hlsl's ShelterAt has it (0
  // when they aren't known)
  // knownOpen: how surely it's open to the sky as they know it (its
  // KnownOpenAt: a square not looked at yet, or past them, is 0)
  float ShelterAt(float const* world, float* knownOpen = nullptr);
  // How much a point (world) is inside one of the game's named interiors
  // (the lair, the droneworks, ...), 0 - 1, soft at their faces as the wet
  // look's InteriorAt (any thread)
  float InteriorAmountAt(float const* world);
  // The drainpipes' outlets with water now (render thread; see Weather.cpp's
  // UpdatePipes): per outlet where it comes out (world x, y, z) and the
  // ground's height under it; flow: how hard it runs, 0 - 1. How many.
  int GetPipes(float (*outlets)[4], int most, float& flow);
  // The haze in the rain or the snow (render thread; see Weather.cpp's
  // GetHaze): how thick (per metre, 0 none), whether it's the snow's, and how
  // far the wind has carried its veils (world x, z; metres, it wraps)
  // Outside: how much the camera is out in the open (eased, 0 - 1): all of
  // what it sees hazed then, only what's out in the open while it's inside
  struct Haze { float Density; bool Snow; float Drift[2]; float Outside; };
  Haze GetHaze();
  // For the thunder (any thread): the camera's right and back axes (world),
  // how much it's inside (0 out in the open - 1 well inside)
  void GetListener(float* right, float* back, float& inside);
  // The lightning bolts drawn now, a burst's strikes (render thread): their
  // segments in the world (up to mostSegments), each with its core's width
  // (m), how bright it's drawn now (the strike's flash in it) and which of the
  // bolts returned it's from (0 - mostStrikes - 1, in order); per bolt how far
  // from the camera it struck (metres). Returns how many segments.
  int GetBolts(float (*a)[3], float (*b)[3], float* width, float* bright, int* strike, float* strikeDistance,
    int mostSegments, int mostStrikes);

  // The menu's weather settings, read and set from elsewhere (a replay
  // giving the weather it was recorded with); any thread. The rain itself
  // follows on the next camera update.
  struct State
  {
    bool Rain;
    bool Heavy;
    bool KeepOut;     // Keep rain out of buildings
    bool LoadZones;   // Load the game's rain
  };
  State GetState();
  void SetState(State const& state);
  // Whether the game's rain is kept at the camera right now (it's loaded and
  // Rain is on); heavy: the heavy rain too
  bool IsRaining(bool* heavy);
  // The Shard's tip in the world as it's drawn now (the far-away model from
  // the city, the real tower out of it), for lightning to strike it now and
  // then; known false when neither is drawn or its tip isn't known. Any
  // thread (the Shard's code calls it from the camera hook).
  void SetShardTop(bool known, float const* top);
  // Whether lightning can come now (a storm at the last update, or Strike
  // now pressed): only then does the Shard's code look for its tip in the
  // city (camera hook, every frame before weather::Update; a burst waits for
  // an update this was asked before with true)
  bool ShardTopWanted();
  // The visual environment manager's mix of a view, right after it (game
  // thread): the rain's strength given to the game's emitters while Rain is
  // on; true when the view should be mixed again (the game's value back)
  bool AfterViewMix(__int64 view);
}
