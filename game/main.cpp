// game.exe — M-GAME0 standalone playable game (GAME-DEV-ORDER step 7 minimum
// playable: drive inside the real CG2 Zhujiang New Town city + impact feedback).
//
// Architecture (STEP7-PLAN.md):
//   - viewer stays pure showcase: this is a SEPARATE exe, viewer source untouched.
//   - Reuses core public APIs only: GameClock/TickScheduler, physics::DebugScene,
//     CameraDirector/ChaseCamera, scene::World::deserialise, ContactEvent*,
//     audio::AudioWorld, renderer::Renderer, CameraTrauma.
//   - core additive-only: DebugScene::addCityBoxes/impactState (no existing
//     signature changed); DriveViewState layout untouched.
//   - Feedback = shake (CameraTrauma) + audio (WAV source) + event log/HUD.
//   - No debugLines visualization (per ruling), no AI traffic, no streaming.
//
// Exit: 0 = pass, 1 = runtime, 2 = usage. Same contract as other tools.

#include "nl/physics/World.hpp"
#include "nl/physics/query.hpp"
#include "nl/core/game_clock.hpp"
#include "nl/core/weather.hpp"  // M-CITY-LIT: day/weather chain (viewer M-WEA1 same)
#include "nl/core/tick_groups.hpp"
#include "nl/core/camera_director.hpp"
#include "nl/core/camera_smoother.hpp"
#include "nl/core/window.hpp"
#include "nl/core/log.hpp"
#include "nl/core/crash.hpp"
#include "nl/audio/AudioWorld.hpp"
#include "nl/scene/World.hpp"
#include "nl/scene/Component.hpp"
#include "nl/scene/TrafficCarComponent.hpp"  // M-GAME1: AI traffic cars
#include "nl/scene/SignalLampComponent.hpp"  // M-GAME1: signal lamp entities
#include "nl/traffic/TrafficCarSystem.hpp"   // M-GAME1: lane-graph traffic sim
#include "nl/renderer/renderer.hpp"

#include <nlohmann/json.hpp>

// Multi-GPU machines (e.g. RTX 4060 laptop + Intel iGPU): Windows defaults NEW
// exe paths to the power-saving iGPU. This machine's Intel drivers crash with
// this engine on BOTH backends (DX12: AV inside igd12um64xel CreateCommand-
// Signature; Vulkan: Intel ICD exposes no Vulkan-1.3 vkCmdPipelineBarrier2 ->
// null dispatch call at Hud::init). NVIDIA/AMD's documented per-process
// mechanism: export these two symbols to request the high-performance GPU.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
extern "C" __declspec(dllexport) DWORD NvOptimusEnablement = 0x00000001;
extern "C" __declspec(dllexport) int AmdPowerXpressRequestHighPerformance = 0x00000001;

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <string>
#include <vector>

namespace {

// M-CITY-LIT: color temperature K -> linear RGB (Krystof fit; viewer-side
// sole impl, shared here so the injected sun light and fp.weatherSunCol
// always agree).
void sunColorFromTempK(double tempK, float out[3]) {
    const double tk = tempK / 100.0;
    const double r = tk <= 66.0 ? 1.0
        : 1.292936186062745 * std::pow(tk - 60.0, -0.1332047592);
    const double g = tk <= 66.0
        ? 0.3900815787690196 * std::log(std::max(tk, 1e-9)) - 0.631841443788627
        : 1.129890860895294 * std::pow(tk - 60.0, -0.0755148492);
    const double b = tk >= 66.0 ? 1.0
        : (tk <= 19.0 ? 0.0
                      : 0.5432067891102761 * std::log(tk - 10.0) - 1.19625408914);
    out[0] = static_cast<float>(std::clamp(r, 0.0, 1.0));
    out[1] = static_cast<float>(std::clamp(g, 0.0, 1.0));
    out[2] = static_cast<float>(std::clamp(b, 0.0, 1.0));
}

struct Options {
    bool selftest = false;
    bool verbose = false;
    bool noHud = false;
    bool audioRealtime = false;
    std::string city = "example/cg2-m0";
    std::string cityScene;        // resolved city/city.scene.engine
    std::string citySceneName = "city.scene.real.engine";  // --city-scene override
    std::string roadnetName = "roadnet.json";              // --roadnet override
    // M-CITY-ROAD: "auto" = every controlled junction gets a signal (M-GAME1
    // convention, keeps game-m0/m1 baselines intact); "osm" = only junctions
    // OSM marks with traffic_signals (real roadnet: 71 of 2295 junctions).
    std::string signalsMode = "auto";                      // --signals
    std::string playScript;       // optional frame-script file
    std::string screenshot;
    int screenshotFrame = -1;     // which rendered frame writes the PNG (-1 = last)
    int width = 960, height = 540;
    int frames = 8;               // rendered frames before screenshot
    int steps = 240;              // logic frames total
    std::uint32_t seed = 1;
    // M-CITY-LIT: day/weather chain (viewer --weather-* same semantics).
    int weatherSeed = -1;              // >=0 enables the chain
    double weatherT = 43200.0;         // fixed time-of-day seconds (noon default)
    std::string weatherPreset = "clear";
    bool skyBg = false;                // analytic sky background pass
    bool ground = false;               // M-CITY-LIT: ground slab (see loadCity)
    bool gi = false;                   // M-CITY-LIT: probe GI chain (--gi)
    float yaw = 28.0f, pitch = -10.0f, dist = 30.0f;  // free-cam fallback
    // M-GAME1: AI traffic (kinematic lane-graph sim, NOT Jolt bodies — cars
    // pass through the player chassis by design; see GAME1-PLAN.md §0).
    // Default 0 keeps the M-GAME0 baseline bytes untouched; --traffic 1 opts in.
    int traffic = 0;
    unsigned long long trafficSeed = 12345;
    std::string cam = "chase";   // chase | free (M-GAME1 aerial verification)
    float camAtX = 0.0f, camAtZ = 0.0f;   // --cam-at X Z: free eye ground pos
};

void usage() {
    std::printf(
        "game.exe - M-GAME0 standalone playable (city driving + impact feedback)\n"
        "  game.exe --selftest [--verbose]\n"
        "  game.exe [--city DIR] [--screenshot PATH] [--frames N] [--steps N]\n"
        "           [--size WxH] [--seed N] [--play-script FILE] [--no-hud]\n"
        "           [--audio-realtime]  (default audio = offline, headless-safe)\n"
        "           [--traffic 0|1] [--traffic-seed N]  (M-GAME1 AI traffic)\n"
        "exit: 0 pass / 1 runtime / 2 usage\n");
}

bool parseSize(const std::string& s, int* w, int* h) {
    const auto x = s.find('x');
    if (x == std::string::npos) return false;
    try {
        *w = std::stoi(s.substr(0, x));
        *h = std::stoi(s.substr(x + 1));
    } catch (...) { return false; }
    return *w > 0 && *h > 0;
}

int parseArgs(int argc, char** argv, Options* o) {
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto need = [&](const char* name, std::string* out) -> bool {
            if (i + 1 >= argc) { std::printf("missing value for %s\n", name); return false; }
            *out = argv[++i]; return true;
        };
        if (a == "--selftest") o->selftest = true;
        else if (a == "--verbose" || a == "-v") o->verbose = true;
        else if (a == "--no-hud") o->noHud = true;
        else if (a == "--audio-realtime") o->audioRealtime = true;
        else if (a == "--city") { if (!need("--city", &o->city)) return 2; }
        else if (a == "--city-scene") {   // M-CITY-DENS: override scene file name
            if (!need("--city-scene", &o->citySceneName)) return 2; }
        else if (a == "--roadnet") {      // M-CITY-ROAD: override roadnet file name
            if (!need("--roadnet", &o->roadnetName)) return 2; }
        else if (a == "--signals") {      // M-CITY-ROAD: auto | osm
            if (!need("--signals", &o->signalsMode)) return 2;
            if (o->signalsMode != "auto" && o->signalsMode != "osm") return 2; }
        else if (a == "--screenshot") { if (!need("--screenshot", &o->screenshot)) return 2; }
        else if (a == "--screenshot-frame") {
            std::string v; if (!need("--screenshot-frame", &v)) return 2;
            o->screenshotFrame = std::atoi(v.c_str());
        }
        else if (a == "--play-script") { if (!need("--play-script", &o->playScript)) return 2; }
        else if (a == "--frames") {
            std::string v; if (!need("--frames", &v)) return 2;
            o->frames = std::atoi(v.c_str()); if (o->frames < 1) return 2;
        } else if (a == "--steps") {
            std::string v; if (!need("--steps", &v)) return 2;
            o->steps = std::atoi(v.c_str()); if (o->steps < 1) return 2;
        } else if (a == "--size") {
            std::string v; if (!need("--size", &v)) return 2;
            if (!parseSize(v, &o->width, &o->height)) return 2;
        } else if (a == "--seed") {
            std::string v; if (!need("--seed", &v)) return 2;
            o->seed = static_cast<std::uint32_t>(std::strtoul(v.c_str(), nullptr, 10));
        } else if (a == "--traffic") {
            std::string v; if (!need("--traffic", &v)) return 2;
            o->traffic = std::atoi(v.c_str());
            if (o->traffic != 0 && o->traffic != 1) return 2;
        } else if (a == "--traffic-seed") {
            std::string v; if (!need("--traffic-seed", &v)) return 2;
            o->trafficSeed = std::strtoull(v.c_str(), nullptr, 10);
        } else if (a == "--cam") {
            if (!need("--cam", &o->cam)) return 2;
            if (o->cam != "chase" && o->cam != "free") return 2;
        } else if (a == "--cam-at") {
            std::string v; if (!need("--cam-at", &v)) return 2;
            if (std::sscanf(v.c_str(), "%f,%f", &o->camAtX, &o->camAtZ) != 2) return 2;
        } else if (a == "--day") {
            // M-CITY-LIT shortcut: noon clear sky (kDayLength=600s -> t=300)
            // + analytic sky background + ground slab.
            o->weatherSeed = 7;
            o->weatherT = 300.0;
            o->weatherPreset = "clear";
            o->skyBg = true;
            o->ground = true;
        } else if (a == "--ground") {
            o->ground = true;
        } else if (a == "--weather-seed") {
            std::string v; if (!need("--weather-seed", &v)) return 2;
            o->weatherSeed = std::atoi(v.c_str());
        } else if (a == "--weather-t") {
            std::string v; if (!need("--weather-t", &v)) return 2;
            o->weatherT = std::atof(v.c_str());
        } else if (a == "--weather-preset") {
            if (!need("--weather-preset", &o->weatherPreset)) return 2;
            if ((int)nl::core::weatherKindFromName(o->weatherPreset.c_str()) ==
                (int)nl::core::WeatherKind::kCount) return 2;
        } else if (a == "--sky-bg") {
            o->skyBg = true;
        } else if (a == "--gi") {
            o->gi = true;   // M-CITY-LIT: probe GI chain (place->trace->gather->compose)
        } else if (a == "--yaw") {
            std::string v; if (!need("--yaw", &v)) return 2; o->yaw = std::atof(v.c_str());
        } else if (a == "--pitch") {
            std::string v; if (!need("--pitch", &v)) return 2; o->pitch = std::atof(v.c_str());
        } else if (a == "--dist") {
            std::string v; if (!need("--dist", &v)) return 2; o->dist = std::atof(v.c_str());
        } else if (a == "--help" || a == "-h") { usage(); return 2; }
        else { std::printf("unknown option: %s\n", a.c_str()); return 2; }
    }
    return 0;
}

// ---- selftest (headless, no window/renderer): verify the whole logic chain ----
int g_fail = 0;
void expect(bool ok, const char* what) {
    if (!ok) { ++g_fail; std::printf("  FAIL: %s\n", what); }
}

// Deterministic frame-script: forward for a while then brake. Used both by
// --selftest (no script file) and by headless replay (same function -> same path).
nl::physics::DriveInputState scriptInput(int frame) {
    nl::physics::DriveInputState in;
    if (frame < 120) in.forward = 1.0f;      // accelerate toward the buildings
    else if (frame < 150) in.brake = 1.0f;
    return in;
}

int runSelftest(bool verbose) {
    // 1) core city static box + impact (the real physics anchor lives in phys-tools;
    //    here we re-verify the exact chain game.exe relies on).
    {
        nl::physics::DebugScene ds;
        std::string err;
        expect(ds.build(2, 1, &err), "game: DebugScene scenario 2 builds");
        expect(ds.cityBoxCount() == 0, "game: no city boxes by default");
        auto s = ds.impactState();
        expect(!s.hit && s.hits == 0, "game: impact zero before contact");
        // A building wall 8m ahead, wide enough to catch the chassis (same
        // conservative geometry as the phys-tools city-impact anchor).
        const float pos[2][3] = {{0.0f, 5.0f, 8.0f}, {6.0f, 5.0f, 16.0f}};
        const float half[2][3] = {{8.0f, 5.0f, 1.0f}, {8.0f, 5.0f, 1.0f}};
        const std::uint64_t ids[2] = {101, 202};
        expect(ds.addCityBoxes(pos, half, ids, 2, nl::physics::layers::kDefault) == 2,
               "game: two city static boxes added");
        expect(ds.cityBoxCount() == 2, "game: cityBoxCount = 2");
        for (int f = 0; f < 240; ++f) {
            ds.setDriveInput(scriptInput(f));
            ds.step(nl::core::GameClock::kStep);
        }
        s = ds.impactState();
        expect(s.hit && s.hits > 0, "game: driving forward hits a city building");
        expect(s.owner == 101 || s.owner == 202, "game: impact owner is a building entity");
        expect(s.strength > 0.0f && s.impulse > 0.0f, "game: impact strength/impulse positive");
        expect(s.pos[2] > 6.0f && s.pos[2] < 18.0f, "game: impact near the building row");
        ds.clearCityBoxes();
        expect(ds.cityBoxCount() == 0, "game: clear removes city boxes");
        if (verbose) {
            std::printf("[game-selftest] hits=%u owner=%llu strength=%.3f posZ=%.2f\n",
                        s.hits, (unsigned long long)s.owner, s.strength, s.pos[2]);
        }
    }

    // 2) GameClock fixed-step: 240 steps @ kStep advances tickCount deterministically.
    {
        nl::core::GameClock clock;
        long long ticks = 0;
        const double ms = nl::core::GameClock::kStep * 1000.0;
        for (int i = 0; i < 240; ++i) clock.tick(ms, [&](double, std::uint64_t) { ++ticks; });
        expect(ticks == 240, "game: GameClock runs 240 fixed steps");
        expect(clock.tickCount() == 240, "game: GameClock tickCount = 240");
        expect(std::abs(clock.simTime() - 240.0 * nl::core::GameClock::kStep) < 1e-6,
               "game: GameClock simTime matches fixed step");
    }

    // 3) CameraTrauma deterministic shake.
    {
        nl::core::CameraTrauma t1, t2;
        t1.addTrauma(1.0f); t2.addTrauma(1.0f);
        for (int i = 0; i < 30; ++i) { t1.step(1.0f/60.0f); t2.step(1.0f/60.0f); }
        float a1,b1,c1,a2,b2,c2;
        t1.offsets(a1,b1,c1); t2.offsets(a2,b2,c2);
        expect(a1==a2 && b1==b2 && c1==c2, "game: trauma shake deterministic");
        expect(t1.trauma01() < 1.0f && t1.trauma01() >= 0.0f, "game: trauma decays");
    }

    if (g_fail) { std::printf("game-m0 selftest: FAIL (%d)\n", g_fail); return 1; }
    std::printf("game-m0 selftest: PASS (city static collision + impact, GameClock, trauma determinism)\n");
    return 0;
}

// ---- Scene load: city.scene.engine -> ECS World + render descs + city boxes ----
struct CityScene {
    nl::scene::World world;
    std::vector<nl::renderer::SceneObjectDesc> objects;
    std::vector<nl::renderer::SceneLightDesc> lights;
    std::vector<std::string> meshFiles;   // stable storage for desc.d.meshFile
    float ambient = 0.28f;
    int buildingBoxes = 0;
    int playerObject = -1;   // index into objects[] driven by the live chassis
    // M-GAME1: AI traffic entity -> objects[] index maps (parallel vectors).
    std::vector<nl::scene::EntityID> carEntities;   // car_* entities (TrafficCarComponent)
    std::vector<int> carObjects;                    // objects[] index per car
    std::vector<nl::scene::EntityID> lampEntities;  // signal_* entities
    std::vector<int> lampObjects;                   // objects[] index per lamp
    bool ok = false;
};

bool loadCity(const Options& o, CityScene* cs, std::string* err) {
    std::ifstream f(o.cityScene, std::ios::binary);
    if (!f) { *err = "cannot open " + o.cityScene; return false; }
    nlohmann::json doc;
    try { f >> doc; }
    catch (const std::exception& e) { *err = std::string("malformed json: ") + e.what(); return false; }
    cs->world.getComponentLibrary().registerComponent<nl::scene::TransformComponent>();
    cs->world.getComponentLibrary().registerComponent<nl::scene::ModelComponent>();
    cs->world.getComponentLibrary().registerComponent<nl::scene::LightComponent>();
    if (!cs->world.deserialise(doc, *err)) return false;
    if (doc.contains("ambient") && doc["ambient"].is_number())
        cs->ambient = doc["ambient"].get<float>();

    // Entity -> render desc (same collection logic as viewer runScene).
    // Reserve meshFiles UP FRONT: desc.meshFile stores c_str() into these
    // strings, so any later push_back that reallocates would dangle every
    // existing pointer. 256 covers 150 boxes + tower + 8 cars + player +
    // ~30 signal lamps with headroom (M-GAME1 appends lamps post-load).
    cs->meshFiles.reserve(256);
    std::vector<nl::scene::EntityID> modelIds;
    for (nl::scene::EntityID id : cs->world.getEntityIDs()) {
        const auto* light = cs->world.getComponent<nl::scene::LightComponent>(id);
        if (light) {
            nl::renderer::SceneLightDesc l{};
            const float len = std::sqrt(light->direction.x*light->direction.x +
                                        light->direction.y*light->direction.y +
                                        light->direction.z*light->direction.z);
            if (len > 1e-8f) {
                l.dir[0]=light->direction.x/len; l.dir[1]=light->direction.y/len;
                l.dir[2]=light->direction.z/len;
            }
            l.color[0]=light->color.x; l.color[1]=light->color.y; l.color[2]=light->color.z;
            l.intensity=light->intensity; l.type=light->type;
            l.position[0]=light->position.x; l.position[1]=light->position.y; l.position[2]=light->position.z;
            l.range=light->range; l.shadow=light->shadow;
            cs->lights.push_back(l);
        }
        const auto* model = cs->world.getComponent<nl::scene::ModelComponent>(id);
        if (model && cs->world.getComponent<nl::scene::TransformComponent>(id)) {
            cs->meshFiles.push_back(model->meshFile);
            modelIds.push_back(id);
        }
    }
    // Player chassis has no render mesh in the physics scene; give it a visible
    // body by appending one extra ECS object driven per-frame by chassisState.
    // sedan.glb is already a scene mesh (traffic cars), so the GLB is on the
    // renderer's whitelist.
    const int kPlayerObjectIndex = static_cast<int>(modelIds.size());
    cs->meshFiles.push_back("sedan.glb");
    // Reserve BEFORE taking c_str() pointers: vector growth would dangle every
    // desc.meshFile below (classic realloc hazard).
    cs->objects.reserve(cs->meshFiles.size());
    for (std::size_t k = 0; k < modelIds.size(); ++k) {
        const auto* model = cs->world.getComponent<nl::scene::ModelComponent>(modelIds[k]);
        const auto* t = cs->world.getComponent<nl::scene::TransformComponent>(modelIds[k]);
        nl::renderer::SceneObjectDesc d{};
        if (t) {
            const auto wm = t->getWorldMatrix();
            std::memcpy(d.world, wm.m, sizeof(d.world));
        }
        d.tint[0]=model->tint.x; d.tint[1]=model->tint.y; d.tint[2]=model->tint.z;
        d.roughness=model->roughness; d.metallic=model->metallic;
        d.hasPbrOverride=model->hasPbrOverride;
        d.meshFile=cs->meshFiles[k].c_str();
        cs->objects.push_back(d);
    }
    {
        // Player placeholder: identity world, overwritten every frame from the
        // live chassis pose (see render loop).
        nl::renderer::SceneObjectDesc d{};
        d.meshFile = cs->meshFiles.back().c_str();
        cs->objects.push_back(d);
        cs->playerObject = kPlayerObjectIndex;
    }
    // M-CITY-LIT: ground plane. CG2 has zero ground entities (buildings float
    // over the clear color) -> append one slab using the SAME shared box mesh
    // (scale is full-size per Transform convention). Thin box: top face at
    // y=0 exactly where the implicit physics ground plane lives. Bounding box
    // from the building transforms (+20% margin so the horizon never ends
    // mid-block). Append AFTER the player placeholder -> car/lamp indices
    // (recorded from modelIds positions) stay valid.
    if (o.ground) {
        float minx = 0.f, maxx = 0.f, minz = 0.f, maxz = 0.f;
        bool any = false;
        for (nl::scene::EntityID id : modelIds) {
            const auto* t = cs->world.getComponent<nl::scene::TransformComponent>(id);
            if (!t) continue;
            const float sx = std::abs(t->local.scale.x) * 0.5f;
            const float sz = std::abs(t->local.scale.z) * 0.5f;
            minx = any ? std::min(minx, t->local.position.x - sx) : t->local.position.x - sx;
            maxx = any ? std::max(maxx, t->local.position.x + sx) : t->local.position.x + sx;
            minz = any ? std::min(minz, t->local.position.z - sz) : t->local.position.z - sz;
            maxz = any ? std::max(maxz, t->local.position.z + sz) : t->local.position.z + sz;
            any = true;
        }
        if (any) {
            const float cx = (minx + maxx) * 0.5f, cz = (minz + maxz) * 0.5f;
            // M-CITY-ROAD: 1.2x span (was 0.6x = *half* the bbox, a leftover of
            // the 150-box training-ground era) — a 6.7 km real roadnet poked out
            // past the slab edge and left a hard diagonal cut in aerials.
            const float w = (maxx - minx) * 1.2f + 100.0f;
            const float dpt = (maxz - minz) * 1.2f + 100.0f;
            cs->meshFiles.push_back(cs->meshFiles.front());  // shared box mesh
            nl::renderer::SceneObjectDesc g{};
            // Column-major T*S: scale (w,2,dpt) then translate (cx,-1,cz).
            g.world[0] = w; g.world[5] = 2.0f; g.world[10] = dpt;
            g.world[12] = cx; g.world[13] = -1.0f; g.world[14] = cz;
            g.world[15] = 1.0f;
            g.tint[0] = 0.42f; g.tint[1] = 0.43f; g.tint[2] = 0.45f;
            g.meshFile = cs->meshFiles.back().c_str();
            cs->objects.push_back(g);
            std::printf("game: ground slab %.0fx%.0f at (%.0f, %.0f)\n",
                        w, dpt, cx, cz);
        }
    }
    // M-GAME1: record car_* entities -> objects[] indices (objects index ==
    // modelIds index for every entry before the player placeholder).
    for (std::size_t k = 0; k < modelIds.size(); ++k) {
        if (cs->world.getName(modelIds[k]).rfind("car_", 0) != 0) continue;
        cs->carEntities.push_back(modelIds[k]);
        cs->carObjects.push_back(static_cast<int>(k));
    }
    // NOTE: no key-light injection. CG2 is a night city (emissive windows baked
    // in the facade textures); viewer --traffic-city renders it with ZERO lights
    // and that is the target look. The earlier "all black" screenshot was the
    // camera INSIDE a giant slab (pre-spawn-fix), not a lighting problem.
    cs->ok = true;
    return true;
}

// ---- M-GAME1: AI traffic activation (viewer --traffic-city parity) ----
// Kinematic lane-graph sim via nl::traffic::TrafficCarSystem (NOT Jolt): cars
// pass through the player chassis by design (GAME1-PLAN.md §0). Wires the
// cg2-m0 car_* entities, registers the ECS system, builds signal lamps, and
// appends lamp render descs. Caller ticks via world.tick() in PostPhysics.
bool setupTraffic(const Options& o, CityScene* cs, std::string* err) {
    cs->world.getComponentLibrary().registerComponent<nl::scene::TrafficCarComponent>();
    int carsTagged = 0;
    for (nl::scene::EntityID id : cs->carEntities) {
        cs->world.addComponent(
            id, std::make_unique<nl::scene::TrafficCarComponent>(
                    nl::scene::Entity{id, &cs->world}));
        ++carsTagged;
    }
    if (carsTagged == 0) { *err = "traffic: no car_* entities in scene"; return false; }
    cs->world.flushEntityCreationAndRemoval();
    cs->world.registerSystem(std::make_unique<nl::traffic::TrafficCarSystem>(cs->world));
    auto* sys = static_cast<nl::traffic::TrafficCarSystem*>(
        cs->world.findSystem(nl::traffic::TrafficCarSystem::kName));

    // roadnet.json lives next to city.scene.engine (citygen interface output).
    // --roadnet picks a different file name in the same city dir (M-CITY-ROAD:
    // roadnet.real.json = real OSM topology).
    const std::string roadnetPath =
        (std::filesystem::path(o.city) / o.roadnetName).generic_string();
    std::ifstream rf(roadnetPath, std::ios::binary);
    if (!rf) { *err = "traffic: cannot open " + roadnetPath; return false; }
    std::string jsonBytes((std::istreambuf_iterator<char>(rf)),
                          std::istreambuf_iterator<char>());
    nl::traffic::LaneGraph laneGraph;
    std::string gerr;
    if (!nl::traffic::buildLaneGraphFromRoadnetJson(
            jsonBytes.data(), jsonBytes.size(), &laneGraph, &gerr)) {
        *err = "traffic: lane graph failed: " + gerr;
        return false;
    }
    nl::traffic::TrafficSimParams simParams{};
    simParams.loop = true;   // closed-route perpetual traffic (TRAFFIC-PLAN §11.2)
    std::string ierr;
    if (!sys->init(laneGraph, simParams, o.trafficSeed, ierr)) {
        *err = "traffic: system init failed: " + ierr;
        return false;
    }
    // Signal phases + lamp entities (M-T4 parity with viewer --traffic-city).
    const bool autoSignal = (o.signalsMode == "auto");
    if (!sys->initControl(jsonBytes.data(), jsonBytes.size(), autoSignal, ierr)) {
        *err = "traffic: control init failed: " + ierr;
        return false;
    }
    cs->world.getComponentLibrary().registerComponent<nl::scene::SignalLampComponent>();
    int lampsCreated = 0;
    for (const nl::traffic::NodeControl& nc : sys->control().nodes) {
        if (!nc.signal) continue;   // lock-only node has no lamp
        for (int grp = 0; grp < 2; ++grp) {
            for (std::int32_t edgeId : nc.groups[grp]) {
                // Rightmost lane of this entry edge = offset-max (RHT rule).
                const nl::traffic::Lane* lane = nullptr;
                for (const nl::traffic::Lane& L : laneGraph.lanes) {
                    if (L.edgeId != edgeId) continue;
                    if (!lane || L.offset > lane->offset) lane = &L;
                }
                if (!lane || lane->points.size() < 2) continue;
                const auto& pts = lane->points;
                const float ex = pts.back().x, ez = pts.back().z;
                float fx = ex - pts[pts.size() - 2].x;
                float fz = ez - pts[pts.size() - 2].z;
                const float fl = std::sqrt(fx * fx + fz * fz);
                if (fl < 1e-6f) continue;
                fx /= fl; fz /= fl;               // travel direction (towards node)
                const float rx = fz, rz = -fx;    // RHT right = (fz, -fx)
                const float off = lane->offset > 0.0f ? lane->offset : 0.0f;
                // Stop line (entryStop before node) right side: curb + 1.2m.
                const float px = ex - fx * 2.5f + rx * (off + 1.2f);
                const float pz = ez - fz * 2.5f + rz * (off + 1.2f);
                // +Z panel faces oncoming traffic = model +Z towards -forward.
                const float vyaw = std::atan2(-fx, -fz) * 0.5f;
                auto ent = cs->world.newEntity(
                    "signal_" + std::to_string(nc.nodeId) + "_" +
                    std::to_string(edgeId));
                auto* tr = static_cast<nl::scene::TransformComponent*>(
                    &ent.addComponent(
                        std::make_unique<nl::scene::TransformComponent>(ent)));
                tr->local.position = {px, 0.0f, pz};
                tr->local.rotation = {0.0f, std::sin(vyaw), 0.0f, std::cos(vyaw)};
                auto* mo = static_cast<nl::scene::ModelComponent*>(
                    &ent.addComponent(
                        std::make_unique<nl::scene::ModelComponent>(ent)));
                mo->meshFile = "signal_post.glb";
                mo->tint = {0.10f, 0.85f, 0.15f};   // green until first tick
                ent.addComponent(
                    std::make_unique<nl::scene::SignalLampComponent>(ent));
                if (!sys->registerLamp(ent.getID(), nc.nodeId, edgeId, ierr)) {
                    *err = "traffic: registerLamp failed: " + ierr;
                    return false;
                }
                ++lampsCreated;
            }
        }
    }
    cs->world.flushEntityCreationAndRemoval();   // lamp entities hit the tables
    // Collect lamp render descs (meshFiles was reserved up front in loadCity,
    // so this push_back cannot reallocate and dangle earlier desc.meshFile).
    cs->meshFiles.push_back("signal_post.glb");
    const char* lampMesh = cs->meshFiles.back().c_str();
    for (nl::scene::EntityID id : cs->world.getEntityIDs()) {
        const auto* lamp = cs->world.getComponent<nl::scene::SignalLampComponent>(id);
        if (!lamp) continue;
        const auto* mo = cs->world.getComponent<nl::scene::ModelComponent>(id);
        const auto* tr = cs->world.getComponent<nl::scene::TransformComponent>(id);
        if (!mo || !tr) continue;
        nl::renderer::SceneObjectDesc d{};
        const auto wm = tr->getWorldMatrix();
        std::memcpy(d.world, wm.m, sizeof(d.world));
        d.tint[0] = mo->tint.x; d.tint[1] = mo->tint.y; d.tint[2] = mo->tint.z;
        d.meshFile = lampMesh;
        cs->lampObjects.push_back(static_cast<int>(cs->objects.size()));
        cs->lampEntities.push_back(id);
        cs->objects.push_back(d);
    }
    std::printf("game: traffic ready: cars=%d lanes=%zu signal lamps=%d seed=%llu\n",
                carsTagged, laneGraph.laneCount(), lampsCreated, o.trafficSeed);
    return true;
}

// Build Jolt static boxes from the SAME scene JSON building transforms. We read
// the E1 World JSON entities (Transform + Model) directly: a box entity is one
// whose mesh is a city_box_tierN.glb and has a non-unit scale[1] (height). The
// unit box is 1x1x1 centered at origin in the GLB, so world half-extents =
// |scale|/2 and center = Transform.t + y offset scale[1]/2.
bool addCityCollision(const Options& o, nl::physics::DebugScene* phys, int* outCount,
                      float outCenter[3],
                      std::vector<std::array<float,3>>* outBoxPos,
                      std::vector<std::array<float,3>>* outBoxHalf,
                      std::string* err) {
    std::ifstream f(o.cityScene, std::ios::binary);
    if (!f) { *err = "cannot reopen scene for collision"; return false; }
    nlohmann::json doc;
    try { f >> doc; } catch (...) { *err = "malformed scene json"; return false; }
    if (!doc.contains("entities") || !doc["entities"].is_array()) {
        *err = "scene has no entities[]"; return false;
    }
    std::vector<std::uint64_t> owners;
    std::vector<std::array<float,3>> positions, halves;
    for (const auto& ent : doc["entities"]) {
        if (!ent.is_object() || !ent.contains("components")) continue;
        const auto& comps = ent["components"];
        if (!comps.is_object() || !comps.contains("Model")) continue;
        const auto& m = comps["Model"];
        const std::string mesh = m.value("mesh", std::string(""));
        if (mesh.rfind("city_box_tier", 0) != 0) continue;  // lm_*/cars are not buildings
        float t[3] = {0,0,0}, s[3] = {1,1,1};
        if (comps.contains("Transform")) {
            const auto& tr = comps["Transform"];
            if (tr.contains("position")) {
                const auto& v = tr["position"];
                if (v.is_array() && v.size() >= 3) {
                    t[0]=v[0].get<float>(); t[1]=v[1].get<float>(); t[2]=v[2].get<float>();
                }
            }
            if (tr.contains("scale")) {
                const auto& v = tr["scale"];
                if (v.is_array() && v.size() >= 3) {
                    s[0]=std::fabs(v[0].get<float>()); s[1]=std::fabs(v[1].get<float>());
                    s[2]=std::fabs(v[2].get<float>());
                }
            }
        }
        // CG2 box position is already the CENTER (e.g. West Tower y=220.1 for a
        // 440.2m height = half). scale is the full extent, so half = scale/2.
        // Buildings are axis-aligned; rotation is identity in CG2 -> ignored for
        // collision (documented scope: no rotated facades this milestone).
        positions.push_back({t[0], t[1], t[2]});
        halves.push_back({s[0]*0.5f, s[1]*0.5f, s[2]*0.5f});
        owners.push_back(static_cast<std::uint64_t>(ent.value("id", 0)));
    }
    if (positions.empty()) { *err = "no city_box_tier boxes found in scene"; return false; }
    // CG2 coordinates are km-scale (e.g. West Tower x=-1047). The training-ground
    // physics world is 80x80m and the chassis spawns at origin, so normalize the
    // city to its median x/z (same trick as citygen2engine.py) -> city sits around
    // origin and the streets are reachable. Y is kept (building heights 0..620m).
    std::vector<float> xs, zs;
    xs.reserve(positions.size()); zs.reserve(positions.size());
    for (const auto& p : positions) { xs.push_back(p[0]); zs.push_back(p[2]); }
    std::sort(xs.begin(), xs.end()); std::sort(zs.begin(), zs.end());
    const float cx = xs[xs.size()/2], cz = zs[zs.size()/2];
    if (outCenter) { outCenter[0] = cx; outCenter[1] = 0.0f; outCenter[2] = cz; }
    for (auto& p : positions) { p[0] -= cx; p[2] -= cz; }
    // Flatten to C arrays (contiguous float[3] rows) for the core API.
    std::vector<float> flatP(positions.size()*3), flatH(positions.size()*3);
    for (std::size_t i = 0; i < positions.size(); ++i) {
        for (int k = 0; k < 3; ++k) {
            flatP[i*3+k] = positions[i][k];
            flatH[i*3+k] = std::max(halves[i][k], 0.05f);
        }
    }
    const int added = phys->addCityBoxes(
        reinterpret_cast<const float(*)[3]>(flatP.data()),
        reinterpret_cast<const float(*)[3]>(flatH.data()),
        owners.data(), static_cast<int>(positions.size()),
        nl::physics::layers::kDefault);
    if (outBoxPos)  *outBoxPos  = positions;   // shifted (median-centered)
    if (outBoxHalf) *outBoxHalf = halves;
    *outCount = added;
    return added > 0;
}

// ---- Feedback state: audio source + shake + structured event log ----
struct Feedback {
    std::unique_ptr<nl::audio::AudioWorld> audio;
    nl::core::CameraTrauma trauma;
    std::uint32_t hitSource = 0;
    int audioReady = 0;
    int hits = 0;
    std::uint64_t lastOwner = 0;
    float lastStrength = 0.0f;
    float lastPos[3] = {0,0,0};

    void init(bool realtime) {
        // Default offline (no audio device needed): deterministic, headless-safe.
        // createSource still decodes the WAV and yields a real source id, so the
        // feedback chain is verifiable without a playback device. --audio-realtime
        // opts into the default device for a human playable session.
        nl::audio::AudioConfig cfg; cfg.offline = !realtime; cfg.spatialization = false;
        audio = std::make_unique<nl::audio::AudioWorld>(cfg);
        if (!audio->ok()) return;
        std::string err;
        hitSource = audio->createSource("assets/audio/test-tone.wav", &err);
        audioReady = hitSource ? 1 : 0;
    }
    void onImpact(const nl::physics::ImpactState& s) {
        ++hits;
        lastOwner = s.owner; lastStrength = s.strength;
        lastPos[0]=s.pos[0]; lastPos[1]=s.pos[1]; lastPos[2]=s.pos[2];
        // Shake amplitude scales with impact strength, capped at 1.
        trauma.addTrauma(std::min(1.0f, 0.15f + s.strength * 0.05f));
        if (audioReady && hitSource && audio) {
            std::string err;
            audio->stopSource(hitSource, &err);
            audio->startSource(hitSource, &err);
        }
        std::printf("GAME_IMPACT frame_hits=%d owner=%llu strength=%.4f impulse=%.4f pos=(%.2f,%.2f,%.2f)\n",
                    hits, (unsigned long long)lastOwner, lastStrength, s.impulse,
                    lastPos[0], lastPos[1], lastPos[2]);
    }
};

int runGame(const Options& o) {
    // Resolve scene path.
    Options resolved = o;
    resolved.cityScene = (std::filesystem::path(o.city) / o.citySceneName).generic_string();
    {
        std::error_code ec;
        if (!std::filesystem::is_regular_file(resolved.cityScene, ec) || ec) {
            std::printf("game: scene not found: %s\n", resolved.cityScene.c_str());
            return 2;
        }
    }

    // 1) Load city scene (render + ECS world).
    CityScene city;
    std::string err;
    if (!loadCity(resolved, &city, &err)) {
        std::printf("game: loadCity failed: %s\n", err.c_str());
        return 1;
    }
    std::printf("game: city loaded: %zu objects, %zu lights, ambient %.2f\n",
                city.objects.size(), city.lights.size(), city.ambient);

    // 1a) M-CITY-LIT: inject the weather sun as a directional shadow light.
    //     ECS scene chain direct light = the lights array (pscene g_lights);
    //     the weather chain only modulates ambient/sky/fog. Direction = light
    //     propagation (SceneLightDesc convention) = -(towards-sun). First in
    //     the array so it takes shadow slot 0 (kMaxShadowLights prefix rule).
    if (resolved.weatherSeed >= 0) {
        const nl::core::WeatherState ws = nl::core::evaluateWeather(
            static_cast<std::uint64_t>(resolved.weatherSeed),
            resolved.weatherT,
            nl::core::weatherKindFromName(resolved.weatherPreset.c_str()));
        const double ce = std::cos(ws.sunElevation);
        const double se = std::sin(ws.sunElevation);
        nl::renderer::SceneLightDesc sun{};
        sun.dir[0] = static_cast<float>(-(ce * std::sin(ws.sunAzimuth)));
        sun.dir[1] = static_cast<float>(-se);
        sun.dir[2] = static_cast<float>(-(ce * std::cos(ws.sunAzimuth)));
        sunColorFromTempK(ws.sunColorTempK, sun.color);
        // ECS chain brightness carrier = light COLOR (shader multiplies
        // Lo_dir by 1/sumIntensity, cancelling `intensity`; E1b anchor
        // comment: "ambient 0.55 / light color x2.8"). Gain 8 -> effective
        // ~2.5x albedo direct light = daylight key.
        constexpr float kSunGain = 8.0f;
        sun.color[0] *= kSunGain; sun.color[1] *= kSunGain; sun.color[2] *= kSunGain;
        sun.intensity = 2.2f * static_cast<float>(ws.sunIntensity);
        sun.type = 0;        // directional
        sun.shadow = true;   // M-E1b.4 cast-shadow directional light
        // Daytime ambient floor: scene JSON carries the night value 0.28 and
        // weatherAmbientScale can only attenuate, never raise.
        city.ambient = std::max(city.ambient, 0.55f);
        city.lights.insert(city.lights.begin(), sun);
        std::printf("game: day sun injected: elev=%.1fdeg az=%.1fdeg I=%.2f\n",
                    ws.sunElevation * 57.2957795, ws.sunAzimuth * 57.2957795,
                    sun.intensity);
    }

    // 1b) M-GAME1: AI traffic (opt-in via --traffic 1; default 0 keeps the
    //     M-GAME0 baseline bytes untouched). Lamp descs enter city.objects
    //     BEFORE loadEcsScene upload and BEFORE the post-collision median
    //     shift loop, which then shifts lamp matrices like every other object.
    //     Lamp/car component coordinates stay in roadnet space (already
    //     median-normalised by citygen2engine); the render sync below applies
    //     the same cityCenter shift as the buildings.
    const bool trafficOn = resolved.traffic != 0;
    if (trafficOn && !setupTraffic(resolved, &city, &err)) {
        std::printf("game: %s\n", err.c_str());
        return 1;
    }

    // 2) Physics scene (scenario 2 driving ground) + city static boxes.
    //    Built AFTER the renderer in the final layout; see below. Declared here
    //    only so lambdas can capture it.
    nl::physics::DebugScene phys;
    int boxes = 0;
    float cityCenter[3] = {0,0,0};
    std::vector<std::array<float,3>> boxPos, boxHalf;

    // 3) Feedback object created here but audio device initialized AFTER the
    //    renderer (see below): miniaudio touches COM/device state and must not
    //    precede D3D12 device creation on this machine (Intel driver crash).
    Feedback fb;

    // 4) Camera director (Chase, fed by chassisState each logic tick).
    //    M-GAME1: --cam free = static aerial pose (city-scale traffic/lamp
    //    verification shot); default chase keeps the M-GAME0 baseline bytes.
    nl::core::CameraDirector camera;
    camera.setMode(resolved.cam == "free" ? nl::core::CameraDirector::Mode::Free
                                          : nl::core::CameraDirector::Mode::Chase);
    camera.setAspect(float(resolved.width) / float(resolved.height));
    camera.setFovYDegrees(60.0f);
    camera.setFarZ(4000.0f);
    camera.chase().setDistance(12.0f);      // default 6m clips the player glb
    camera.chase().setHeightOffset(3.0f);   // raise the boom: see over tier1 roofs
    camera.chase().setPitchDegrees(-14.0f); // slight downward: skyline in frame
    if (resolved.cam == "free") {
        // Aerial: eye above --cam-at (dist doubles as altitude), pitch from
        // --pitch (clamped by FreeCamera), yaw from --yaw. Static pose.
        const float alt = std::max(resolved.dist, 120.0f);
        const float eye[3] = {resolved.camAtX, alt, resolved.camAtZ};
        camera.free().setPose(eye, resolved.yaw, resolved.pitch);
        // M-DEPTH0：远景航拍近裁面放大（口径同 OrbitCamera kNearGate/kNearRatio）。
        // 门控 alt > 200：街景/低空卡 alt=max(dist,120) ≤ 200 时 near 恒 0.1，
        // M-GAME1 街景基线逐位不变（实测未门控时 near=1.875 会裁掉 2 像素）。
        const float nearZ = (alt > 200.0f) ? std::max(0.1f, alt / 64.0f) : 0.1f;
        camera.free().setNearZ(nearZ);
    }

    // 5) Window + renderer (sceneMode, like viewer runScene).
    nl::core::WindowDesc wd;
    wd.width = resolved.width; wd.height = resolved.height;
    wd.title = "nl-engine game | city drive";
    wd.visible = !resolved.screenshot.empty();
    nl::core::Window window(wd);
    if (!window.valid()) { std::printf("game: window creation failed\n"); return 1; }
    const auto native = window.framebufferSize();
    const nl::renderer::Extent extent{static_cast<std::uint32_t>(native.width),
                                      static_cast<std::uint32_t>(native.height)};
    auto renderer = nl::renderer::Renderer::create(
        window.nativeHandle(), extent, false, nullptr,
        false, false, false, false, false, false,
        /*sceneMode=*/true, false, false, false, false, false, false, false, false, false, false,
        false);
    if (!renderer || !renderer->valid()) {
        std::printf("game: renderer init failed: %s\n",
                    renderer ? renderer->lastError().c_str() : "null");
        return 1;
    }
    if (!renderer->loadEcsScene(city.objects.data(),
                                static_cast<std::uint32_t>(city.objects.size()),
                                city.lights.empty() ? nullptr : city.lights.data(),
                                static_cast<std::uint32_t>(city.lights.size()),
                                city.ambient, "city.scene.engine")) {
        std::printf("game: loadEcsScene failed: %s\n", renderer->lastError().c_str());
        return 1;
    }

    // 3b) Initialize audio feedback only AFTER the D3D12 device exists.
    fb.init(resolved.audioRealtime);
    std::printf("game: feedback ready: audio=%s\n", fb.audioReady ? "on" : "off (graceful)");

    // 2b) Build physics AFTER the D3D12 device exists (Jolt bootstrap must not
    //     precede nvrhi device init on this Intel driver -- see cdb stack).
    {
        if (!phys.build(2, resolved.seed, &err)) {
            std::printf("game: physics build failed: %s\n", err.c_str());
            return 1;
        }
        if (!addCityCollision(resolved, &phys, &boxes, cityCenter, &boxPos, &boxHalf, &err)) {
            std::printf("game: addCityCollision failed: %s\n", err.c_str());
            return 1;
        }
        // Shift render objects by the same -center so visuals line up with physics.
        // Mat4 column-major: translation at m[12], m[13], m[14]. Render objects
        // were already uploaded by loadEcsScene above, so re-upload after shift.
        for (auto& d : city.objects) {
            d.world[12] -= cityCenter[0];
            d.world[13] -= cityCenter[1];
            d.world[14] -= cityCenter[2];
        }
        if (!renderer->updateEcsObjects(city.objects.data(),
                                        static_cast<std::uint32_t>(city.objects.size()))) {
            std::printf("game: updateEcsObjects (post-shift) failed: %s\n",
                        renderer->lastError().c_str());
            return 1;
        }
        // Spawn the player chassis on a street, NOT inside a building: CG2 has
        // huge OSM slabs (200m-wide malls) that swallow the median origin, and
        // a chassis spawned in overlap jams at speed 0. Scan candidates outward
        // from the city center until clear of every box (2D xz, 3m margin).
        float spawn[3] = {0.0f, 0.6f, 0.0f};
        const float cand[16][2] = {
            {0,0}, {0,10}, {0,-10}, {10,0}, {-10,0}, {0,20}, {0,-20},
            {20,0}, {-20,0}, {0,35}, {0,-35}, {35,0}, {-35,0},
            {0,50}, {50,0}, {-50,0}};
        for (const auto& c : cand) {
            bool free = true;
            for (std::size_t i = 0; i < boxPos.size() && free; ++i) {
                const float hx = std::max(boxHalf[i][0], 0.0f) + 3.0f;
                const float hz = std::max(boxHalf[i][2], 0.0f) + 3.0f;
                if (std::fabs(c[0] - boxPos[i][0]) <= hx &&
                    std::fabs(c[1] - boxPos[i][2]) <= hz) free = false;
            }
            if (free) { spawn[0] = c[0]; spawn[2] = c[1]; break; }
        }
        const float p[3] = {spawn[0], 0.6f, spawn[2]};
        const float q[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        phys.teleportVehicle(0, p, q);
        std::printf("game: physics ready: %d city collision boxes, chassis spawn (%.1f, %.1f)\n",
                    boxes, spawn[0], spawn[2]);
    }

    // 6) GameClock + TickScheduler: 4 groups, deterministic order.
    nl::core::GameClock clock;
    nl::core::TickScheduler& sched = clock.scheduler();
    int logicFrame = 0;
    bool lastImpactHit = false;
    nl::physics::DriveChassisState latestChassis{};   // read by the render loop
    sched.set(nl::core::TickGroup::PrePhysics, [&](double, std::uint64_t) {
        const auto in = scriptInput(logicFrame);
        phys.setDriveInput(in);
    });
    sched.set(nl::core::TickGroup::Physics, [&](double dt, std::uint64_t) {
        phys.step(dt);
    });
    sched.set(nl::core::TickGroup::PostPhysics, [&](double, std::uint64_t) {
        const nl::physics::ImpactState s = phys.impactState();
        if (s.hit && (!lastImpactHit || s.hits > fb.hits)) fb.onImpact(s);
        lastImpactHit = s.hit;
        // Feed chase camera with the live chassis state.
        const nl::physics::DriveChassisState d = phys.chassisState();
        nl::core::ChassisState cs{};
        if (d.valid) {
            cs.pos[0]=d.pos[0]; cs.pos[1]=d.pos[1]; cs.pos[2]=d.pos[2];
            cs.yawDegrees=d.yawDegrees;
            cs.vel[0]=d.vel[0]; cs.vel[1]=d.vel[1]; cs.vel[2]=d.vel[2];
            cs.valid=true;
        }
        nl::core::DirectorInputs di{};
        di.chassis = cs.valid ? &cs : nullptr;
        camera.update(static_cast<float>(nl::core::GameClock::kStep), di);
        // M-GAME1: advance AI traffic AFTER physics (same fixed step, same
        // determinism contract). Car poses + lamp tints land in ECS components;
        // the render loop syncs them into city.objects with the cityCenter
        // shift applied (component space = roadnet space, already normalised).
        if (trafficOn) city.world.tick(nl::core::GameClock::kStep);
        latestChassis = d;
        ++logicFrame;
    });
    sched.set(nl::core::TickGroup::PostUpdateWork, [&](double dt, std::uint64_t) {
        fb.trauma.step(static_cast<float>(dt));
    });

    // 7) Render loop: fixed clock steps per rendered frame (screenshot replay path).
    const int stepsPerFrame = std::max(1, resolved.steps / resolved.frames);
    int rendered = 0;
    std::string shot;
    for (; rendered < resolved.frames && !window.shouldClose(); ++rendered) {
        window.pollEvents();
        // Advance the fixed-step clock for this rendered frame.
        for (int s = 0; s < stepsPerFrame && logicFrame < resolved.steps; ++s)
            clock.tick(nl::core::GameClock::kStep * 1000.0);
        // M-GAME1: traffic log anchor (viewer --traffic-city parity; run.bat
        // findstr source; odometer must grow monotonically frame over frame).
        if (trafficOn) {
            auto* sys = static_cast<nl::traffic::TrafficCarSystem*>(
                city.world.findSystem(nl::traffic::TrafficCarSystem::kName));
            std::printf("traffic: frame=%d ticks=%llu odometer=%.2f arrived=%d\n",
                        rendered,
                        static_cast<unsigned long long>(sys->stepCount()),
                        sys->totalOdometer(), sys->arrivedCount());
        }
        // Shake offsets apply to the view (deterministic, no wall-clock).
        float sy=0, sp=0, sr=0;
        fb.trauma.offsets(sy, sp, sr);
        // Player car mesh follows the live chassis pose (column-major Mat4:
        // rotation about Y, translation m[12..14]; physics and render share the
        // same median-shifted space so no extra offset).
        if (city.playerObject >= 0 && city.playerObject < static_cast<int>(city.objects.size())
            && latestChassis.valid) {
            const float yawRad = latestChassis.yawDegrees * 3.14159265358979f / 180.0f;
            const float cy2 = std::cos(yawRad), sy2 = std::sin(yawRad);
            float* w = city.objects[static_cast<std::size_t>(city.playerObject)].world;
            w[0]=cy2;  w[1]=0;  w[2]=-sy2; w[3]=0;
            w[4]=0;    w[5]=1;  w[6]=0;   w[7]=0;
            w[8]=sy2;  w[9]=0;  w[10]=cy2; w[11]=0;
            w[12]=latestChassis.pos[0]; w[13]=latestChassis.pos[1]; w[14]=latestChassis.pos[2];
            w[15]=1;
            if (!renderer->updateEcsObjects(city.objects.data(),
                                            static_cast<std::uint32_t>(city.objects.size()))) {
                std::printf("game: updateEcsObjects (player car) failed\n");
                return 1;
            }
        }
        // M-GAME1: sync AI traffic into render objects. Car matrices come from
        // the ECS Transform (roadnet space) minus the cityCenter shift used by
        // every other object; lamp tints come from ModelComponent (traffic sim
        // writes green/amber/red per phase each tick).
        if (trafficOn) {
            for (std::size_t i = 0; i < city.carObjects.size(); ++i) {
                const auto* t = city.world.getComponent<nl::scene::TransformComponent>(
                    city.carEntities[i]);
                if (!t) continue;
                const auto wm = t->getWorldMatrix();
                float* w = city.objects[static_cast<std::size_t>(city.carObjects[i])].world;
                std::memcpy(w, wm.m, sizeof(float) * 16);
                w[12] -= cityCenter[0];
                w[14] -= cityCenter[2];
            }
            for (std::size_t i = 0; i < city.lampObjects.size(); ++i) {
                const auto* mo = city.world.getComponent<nl::scene::ModelComponent>(
                    city.lampEntities[i]);
                if (!mo) continue;
                auto& d = city.objects[static_cast<std::size_t>(city.lampObjects[i])];
                d.tint[0] = mo->tint.x; d.tint[1] = mo->tint.y; d.tint[2] = mo->tint.z;
            }
            if (!renderer->updateEcsObjects(city.objects.data(),
                                            static_cast<std::uint32_t>(city.objects.size()))) {
                std::printf("game: updateEcsObjects (traffic) failed\n");
                return 1;
            }
        }
        auto vp = camera.viewProjection();
        nl::renderer::FrameParams fp{};
        std::memcpy(fp.mvp, vp.m, sizeof(fp.mvp));
        std::memcpy(fp.model, vp.m, sizeof(fp.model));
        fp.nearPlane = camera.nearPlane();   // M-DEPTH0：远景放大 near 同步进 dag 误差投影
        fp.cullBack = true;
        fp.shading = 0;        // textured (ECS scene GLBs carry baked facade tiers)
        // M-CITY-LIT: weather chain (viewer M-WEA1 same fields; fixed t =
        // deterministic bytes). Sun dir/energy/col + sky two colors + fog.
        if (resolved.weatherSeed >= 0) {
            nl::core::WeatherState ws = nl::core::evaluateWeather(
                static_cast<std::uint64_t>(resolved.weatherSeed),
                resolved.weatherT,
                nl::core::weatherKindFromName(resolved.weatherPreset.c_str()));
            const double ce = std::cos(ws.sunElevation);
            const double se = std::sin(ws.sunElevation);
            fp.lightDir[0] = static_cast<float>(ce * std::sin(ws.sunAzimuth));
            fp.lightDir[1] = static_cast<float>(se);
            fp.lightDir[2] = static_cast<float>(ce * std::cos(ws.sunAzimuth));
            fp.lightDir[3] = 0.28f * static_cast<float>(ws.sunIntensity);
            fp.weatherAmbientScale =
                static_cast<float>(std::clamp(ws.sunIntensity, 0.0, 1.0));
            fp.weatherSunIntensity = static_cast<float>(ws.sunIntensity);
            float sunCol[3];
            sunColorFromTempK(ws.sunColorTempK, sunCol);
            fp.weatherSunCol[0] = sunCol[0];
            fp.weatherSunCol[1] = sunCol[1];
            fp.weatherSunCol[2] = sunCol[2];
            fp.weatherZenith[0] = static_cast<float>(ws.skyZenithR);
            fp.weatherZenith[1] = static_cast<float>(ws.skyZenithG);
            fp.weatherZenith[2] = static_cast<float>(ws.skyZenithB);
            fp.weatherHorizon[0] = static_cast<float>(ws.skyHorizonR);
            fp.weatherHorizon[1] = static_cast<float>(ws.skyHorizonG);
            fp.weatherHorizon[2] = static_cast<float>(ws.skyHorizonB);
            fp.weatherFogDensity = static_cast<float>(ws.fogDensity);
            fp.weatherFogHeight  = static_cast<float>(ws.fogHeight);
            fp.weatherWetness    = static_cast<float>(ws.groundWetness);
            fp.sceneSkyBg = resolved.skyBg;
            fp.cityGi = resolved.gi;   // M-CITY-LIT: --gi probe GI chain
        }
        nl::renderer::HudFrame hud{};
        hud.backend = "game";
        hud.panelShow = !resolved.noHud;
        hud.deterministic = true;
        char modeTag[64];
        std::snprintf(modeTag, sizeof(modeTag), "city | hits=%d", fb.hits);
        hud.mode = modeTag;
        shot = resolved.screenshot;
        const int shotAt = resolved.screenshotFrame >= 0
                               ? resolved.screenshotFrame : resolved.frames - 1;
        if (!shot.empty() && rendered == shotAt) {
            // final frame writes screenshot
            if (!renderer->renderFrame({0.05f,0.07f,0.11f,1.0f}, fp, shot.c_str(),
                                       resolved.noHud ? nullptr : &hud)) {
                std::printf("game: renderFrame failed: %s\n", renderer->lastError().c_str());
                return 1;
            }
        } else {
            if (!renderer->renderFrame({0.05f,0.07f,0.11f,1.0f}, fp, nullptr,
                                       resolved.noHud ? nullptr : &hud)) {
                std::printf("game: renderFrame failed: %s\n", renderer->lastError().c_str());
                return 1;
            }
        }
        renderer->drainMessages();
    }

    const auto ds = phys.driveState();
    std::printf("game-m0: frames=%d steps=%d objects=%zu boxes=%d hits=%d "
                "speed=%.3f odometer_pos=(%.2f,%.2f,%.2f) shake=%.4f\n",
                rendered, logicFrame, city.objects.size(), boxes, fb.hits,
                ds.valid ? ds.speed : 0.0f,
                ds.valid ? ds.pos[0] : 0.0f, ds.valid ? ds.pos[1] : 0.0f,
                ds.valid ? ds.pos[2] : 0.0f, fb.trauma.trauma01());
    if (!shot.empty()) std::printf("game: screenshot written: %s\n", shot.c_str());
    // M-GAME1: traffic summary anchor (deterministic: identical across backends).
    if (trafficOn) {
        auto* sys = static_cast<nl::traffic::TrafficCarSystem*>(
            city.world.findSystem(nl::traffic::TrafficCarSystem::kName));
        std::printf("game-m1: traffic cars=%zu lamps=%zu ticks=%llu odometer=%.2f arrived=%d\n",
                    city.carObjects.size(), city.lampObjects.size(),
                    static_cast<unsigned long long>(sys->stepCount()),
                    sys->totalOdometer(), sys->arrivedCount());
    }
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    // M-CRASH0: process-level crash handler, installed first (viewer parity).
    nl::crash::install();
    Options o;
    const int pr = parseArgs(argc, argv, &o);
    if (pr != 0) return pr;
    if (o.selftest) {
        g_fail = 0;
        return runSelftest(o.verbose);
    }
    return runGame(o);
}
