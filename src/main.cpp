// Coral Bay: a small Vice City-style 3D sandbox in C++ with raylib.
// Play as Nico Salazar in a neon beach city, 1986.
//
// What's in here
//   - Tile-map city: roads, sidewalks, art deco hotels, parks, car parks, beach
//   - Optional 3D models + animations from assets/ (falls back to block models)
//   - Lighting: low sunset sun, sky light, soft shadow map, fog, neon glow
//   - Procedural audio: synthwave radio, engine, sirens, sound effects
//   - Missions: phone calls from Marco, markers, objectives, rewards
//   - Peds run a task state machine; traffic follows lanes; wanted level + cops
//
// Controls
//   WASD move / drive      Mouse look          Shift sprint
//   F enter/exit/carjack   Space handbrake     H horn
//   Enter next to a pedestrian: AWS AI Practitioner quiz question
//   R radio station        Tab show AI         G graphics (fancy / simple)
//   Esc pause

#include "raylib.h"
#if defined(PLATFORM_WEB)
#include <emscripten/emscripten.h>
#endif
#include "raymath.h"
#include "rlgl.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <random>
#include <string>
#include <vector>

#include "audio.hpp"
#include "quiz.hpp"
#include "shaders.hpp"

// ---------------------------------------------------------------- tuning
// If downloaded models face the wrong way, change these (in degrees).
static const float CHAR_YAW_FIX = 0.0f;
static const float CAR_YAW_FIX = 0.0f;     // try 180 if cars drive backwards
static const float ANIM_FPS = 60.0f;       // raylib samples glTF animations at 60 fps
static const int SHADOW_RES = 2048;

// ---------------------------------------------------------------- helpers
static std::mt19937 rng(1986);
static float Rand(float a, float b) { std::uniform_real_distribution<float> d(a, b); return d(rng); }
static int RandInt(int a, int b) { std::uniform_int_distribution<int> d(a, b); return d(rng); }
template <class T> static const T& Pick(const std::vector<T>& v) { return v[RandInt(0, (int)v.size() - 1)]; }
static float Clampf(float v, float a, float b) { return v < a ? a : (v > b ? b : v); }
static float AngDiff(float a, float b) {
    float d = std::fmod(a - b, 2.0f * PI);
    if (d > PI) d -= 2.0f * PI;
    if (d < -PI) d += 2.0f * PI;
    return d;
}
static float TurnToward(float a, float target, float maxStep) { return a + Clampf(AngDiff(target, a), -maxStep, maxStep); }
static Color Hex(unsigned v, unsigned char alpha = 255) {
    return Color{(unsigned char)(v >> 16), (unsigned char)((v >> 8) & 255), (unsigned char)(v & 255), alpha};
}
static Color Mix(Color a, Color b, float t) {
    auto f = [&](unsigned char x, unsigned char y) { return (unsigned char)(x + (y - x) * t); };
    return Color{f(a.r, b.r), f(a.g, b.g), f(a.b, b.b), 255};
}
static std::string Lower(std::string s) { for (char& c : s) c = (char)std::tolower((unsigned char)c); return s; }
static Synth gSynth;

// ---------------------------------------------------------------- map
constexpr int MW = 64, MH = 64;
constexpr float TS = 4.0f;
constexpr float LANE = TS * 0.5f;
enum Tile : uint8_t { GRASS, ROAD, WALK, BLD, SAND, SEA, PARK, LOT };
static uint8_t gMap[MW * MH];
static float gRoofH[MW * MH];
static bool IsPark(int id) { return id == 6 || id == 13 || id == 17; }
static bool IsLot(int id) { return id == 8 || id == 11; }

static void BuildMap() {
    for (int y = 0; y < MH; y++)
        for (int x = 0; x < MW; x++) {
            uint8_t t;
            if (x >= 58) t = SEA;
            else if (x >= 51) t = SAND;
            else if (x == 50 || y >= 62) t = WALK;
            else {
                int mx = x % 12, my = y % 12;
                if (mx < 2 || my < 2) t = ROAD;
                else if (mx == 2 || mx == 11 || my == 2 || my == 11) t = WALK;
                else {
                    int id = x / 12 + (y / 12) * 5;
                    t = IsPark(id) ? PARK : IsLot(id) ? LOT : BLD;
                }
            }
            gMap[y * MW + x] = t;
            gRoofH[y * MW + x] = 0;
        }
}
static uint8_t TileAt(float wx, float wz) {
    int x = (int)std::floor(wx / TS), y = (int)std::floor(wz / TS);
    if (x < 0 || y < 0 || x >= MW || y >= MH) return BLD;
    return gMap[y * MW + x];
}
static bool SolidAt(float x, float z) { uint8_t t = TileAt(x, z); return t == BLD || t == SEA; }
static bool Blocked(float x, float z, float r) {
    return SolidAt(x - r, z - r) || SolidAt(x + r, z - r) || SolidAt(x - r, z + r) || SolidAt(x + r, z + r);
}
static bool FootOK(uint8_t t) { return t == WALK || t == PARK || t == SAND; }
static float GroundY(float x, float z) {
    switch (TileAt(x, z)) {
        case WALK: return 0.18f;
        case PARK: return 0.38f;
        case LOT: return 0.28f;
        case SAND: return 0.10f;
        default: return 0.0f;
    }
}

// ---------------------------------------------------------------- scenery
struct Box { Vector3 c, s; Color col; };
static std::vector<Box> gStatic, gEmis;   // gEmis = neon, drawn glowing
static void AddBox(std::vector<Box>& v, float x0, float y0, float z0, float x1, float y1, float z1, Color col) {
    v.push_back({{(x0 + x1) / 2, (y0 + y1) / 2, (z0 + z1) / 2}, {x1 - x0, y1 - y0, z1 - z0}, col});
}
static void AddBox(float x0, float y0, float z0, float x1, float y1, float z1, Color col) { AddBox(gStatic, x0, y0, z0, x1, y1, z1, col); }
struct Palm { float x, z, y, h, lx, lz, ph; };
static std::vector<Palm> gPalms;

static const Color C_ASPHALT = Hex(0x4b4459), C_WALK = Hex(0xe6d6c2), C_GRASS = Hex(0x7dbb62),
                   C_SAND = Hex(0xf2d9a6), C_SEA = Hex(0x2aa3b8), C_LINE = Hex(0xf2d36b), C_WHITE = Hex(0xf7f3ea),
                   C_PINK = Hex(0xff5f8f), C_TEAL = Hex(0x1fb3a6), C_GOLD = Hex(0xffd35a);
static const Color SKY_TOP = Hex(0x3b2a6b), SKY_HORIZON = Hex(0xff9a76), FOG = Hex(0xf29a86);

// Mission locations
static const Vector2 FLAMINGO = {53.0f * TS, 38.5f * TS};
static const Vector2 LOCKUP = {(12 * 1 + 3) * TS + 16.0f, (12 * 2 + 3) * TS + 17.0f};

static void BuildScenery() {
    const float landW = 51 * TS, H = MH * TS;
    AddBox(0, -2, 0, landW, 0, H, C_ASPHALT);
    AddBox(landW, -2, 0, 58 * TS + 2, 0.10f, H, C_SAND);
    for (int by = 0; by < 5; by++)
        for (int bx = 0; bx < 4; bx++)
            AddBox((12 * bx + 2) * TS, 0, (12 * by + 2) * TS, (12 * bx + 12) * TS, 0.18f, (12 * by + 12) * TS, C_WALK);
    AddBox(0, 0, 62 * TS, 50 * TS, 0.18f, H, C_WALK);
    AddBox(50 * TS, 0, 0, 51 * TS, 0.18f, H, C_WALK);

    for (int i = 0; i < 5; i++)
        for (int j = 0; j < 5; j++) {
            float x = (12 * i + 1) * TS, z0 = (12 * j + 2) * TS, z1 = (12 * j + 12) * TS;
            for (float z = z0 + 2; z + 2.5f < z1 - 1; z += 5) AddBox(x - 0.08f, 0, z, x + 0.08f, 0.1f, z + 2.5f, C_LINE);
            for (float zc : {z0 + 1.0f, z1 - 1.0f})
                for (int k = 0; k < 6; k++) {
                    float xs = 12 * i * TS + 0.6f + k * 1.25f;
                    AddBox(xs, 0, zc - 1.2f, xs + 0.6f, 0.1f, zc + 1.2f, C_WHITE);
                }
        }
    for (int j = 0; j < 6; j++)
        for (int i = 0; i < 4; i++) {
            float z = (12 * j + 1) * TS, x0 = (12 * i + 2) * TS, x1 = (12 * i + 12) * TS;
            for (float x = x0 + 2; x + 2.5f < x1 - 1; x += 5) AddBox(x, 0, z - 0.08f, x + 2.5f, 0.1f, z + 0.08f, C_LINE);
            for (float xc : {x0 + 1.0f, x1 - 1.0f})
                for (int k = 0; k < 6; k++) {
                    float zs = 12 * j * TS + 0.6f + k * 1.25f;
                    AddBox(xc - 1.2f, 0, zs, xc + 1.2f, 0.1f, zs + 0.6f, C_WHITE);
                }
        }

    const std::vector<Color> pastel = {Hex(0xf7b2c4), Hex(0x9edbd3), Hex(0xf6dca8), Hex(0xc7b7ec),
                                       Hex(0xf9c49a), Hex(0xb8e0a6), Hex(0xf3eee2)};
    const std::vector<Color> neon = {C_PINK, C_TEAL, C_GOLD, Hex(0x8d5cf6), Hex(0x7ad3ff)};
    for (int by = 0; by < 5; by++)
        for (int bx = 0; bx < 4; bx++) {
            int id = bx + by * 5;
            float x0 = (12 * bx + 3) * TS, z0 = (12 * by + 3) * TS, S = 8 * TS;
            if (IsPark(id)) {
                float cx = x0 + S / 2, cz = z0 + S / 2;
                AddBox(x0, 0, z0, x0 + S, 0.38f, z0 + S, C_GRASS);
                AddBox(cx - 1.5f, 0, z0, cx + 1.5f, 0.42f, z0 + S, Hex(0xe8d8b8));
                AddBox(x0, 0, cz - 1.5f, x0 + S, 0.42f, cz + 1.5f, Hex(0xe8d8b8));
                AddBox(cx - 4, 0, cz - 4, cx + 4, 0.9f, cz + 4, Hex(0xd8ccb6));
                AddBox(cx - 3.3f, 0, cz - 3.3f, cx + 3.3f, 0.95f, cz + 3.3f, Hex(0x6fd6d0));
                AddBox(cx - 0.4f, 0, cz - 0.4f, cx + 0.4f, 2.4f, cz + 0.4f, C_WHITE);
                for (int k = 0; k < 7; k++) {
                    float px = Rand(x0 + 2, x0 + S - 2), pz = Rand(z0 + 2, z0 + S - 2);
                    if (std::fabs(px - cx) < 5 && std::fabs(pz - cz) < 5) continue;
                    gPalms.push_back({px, pz, 0.38f, Rand(6, 9), Rand(-0.8f, 0.8f), Rand(-0.8f, 0.8f), Rand(0, 6)});
                }
                continue;
            }
            if (IsLot(id)) {
                AddBox(x0, 0, z0, x0 + S, 0.28f, z0 + S, Hex(0x575069));
                for (int k = 0; k <= 8; k++) {
                    AddBox(x0 + k * TS - 0.06f, 0, z0 + 0.3f, x0 + k * TS + 0.06f, 0.33f, z0 + 6, C_WHITE);
                    AddBox(x0 + k * TS - 0.06f, 0, z0 + S - 6, x0 + k * TS + 0.06f, 0.33f, z0 + S - 0.3f, C_WHITE);
                }
                if (id == 11) {   // Marco's lockup: a garage at the back of the car park
                    AddBox(x0 + 10, 0, z0 + S - 7, x0 + 22, 4.2f, z0 + S - 0.5f, Hex(0xc9b6a0));
                    AddBox(x0 + 12, 0.28f, z0 + S - 7.05f, x0 + 20, 3.2f, z0 + S - 6.95f, Hex(0x6b5f73));
                    AddBox(gEmis, x0 + 11.5f, 3.5f, z0 + S - 7.1f, x0 + 20.5f, 3.7f, z0 + S - 6.9f, C_TEAL);
                }
                continue;
            }
            struct R { float x, z, w, d; };
            std::vector<R> rects;
            float r = Rand(0, 1);
            if (r < 0.5f) rects = {{x0, z0, S, S}};
            else if (r < 0.75f) rects = {{x0, z0, S / 2, S}, {x0 + S / 2, z0, S / 2, S}};
            else rects = {{x0, z0, S, S / 2}, {x0, z0 + S / 2, S, S / 2}};
            for (const R& b : rects) {
                bool beachfront = (bx == 3);
                float h = beachfront ? Rand(18, 40) : Rand(8, 24);
                Color col = Pick(pastel), glass = Mix(Hex(0x2b4e6b), col, 0.25f), trim = Pick(neon);
                AddBox(b.x, 0, b.z, b.x + b.w, h, b.z + b.d, col);
                for (float y = 2.2f; y < h - 1.8f; y += 3.0f)
                    AddBox(b.x - 0.05f, y, b.z - 0.05f, b.x + b.w + 0.05f, y + 1.1f, b.z + b.d + 0.05f, glass);
                AddBox(gEmis, b.x - 0.15f, h, b.z - 0.15f, b.x + b.w + 0.15f, h + 0.45f, b.z + b.d + 0.15f, trim);
                AddBox(b.x + 1, h, b.z + 1, b.x + 3, h + 1.2f, b.z + 2.6f, Hex(0xcfccd6));
                if (beachfront) {
                    float zc = b.z + b.d / 2;
                    AddBox(gEmis, b.x + b.w, 1.5f, zc - 0.25f, b.x + b.w + 0.2f, h * 0.85f, zc + 0.25f, trim);
                }
                int tx0 = (int)(b.x / TS), tz0 = (int)(b.z / TS), tx1 = (int)((b.x + b.w) / TS), tz1 = (int)((b.z + b.d) / TS);
                for (int ty = tz0; ty < tz1; ty++)
                    for (int tx = tx0; tx < tx1; tx++) gRoofH[ty * MW + tx] = h;
            }
        }

    // The Pink Flamingo beach bar
    {
        float cx = FLAMINGO.x, cz = FLAMINGO.y;
        for (int sx : {-1, 1})
            for (int sz : {-1, 1}) AddBox(cx + sx * 3.2f - 0.12f, 0, cz + sz * 2.4f - 0.12f, cx + sx * 3.2f + 0.12f, 3.0f, cz + sz * 2.4f + 0.12f, C_WHITE);
        AddBox(cx - 3.8f, 3.0f, cz - 3.0f, cx + 3.8f, 3.3f, cz + 3.0f, Hex(0xf7b2c4));
        AddBox(gEmis, cx - 3.9f, 3.3f, cz - 3.1f, cx + 3.9f, 3.45f, cz + 3.1f, C_PINK);
        AddBox(cx + 2.2f, 0, cz - 2.0f, cx + 2.8f, 1.1f, cz + 2.0f, Hex(0x8a5a35));
        AddBox(gEmis, cx - 0.1f, 3.45f, cz - 1.8f, cx + 0.1f, 4.8f, cz + 1.8f, C_PINK);        // sign board
        AddBox(gEmis, cx - 0.15f, 4.8f, cz - 0.5f, cx + 0.15f, 6.2f, cz - 0.3f, C_PINK);       // flamingo neck
        AddBox(gEmis, cx - 0.15f, 5.9f, cz - 0.3f, cx + 0.15f, 6.2f, cz + 0.4f, C_PINK);       // flamingo head
    }

    for (int y = 0; y < MH; y += 3) gPalms.push_back({50.5f * TS, (y + 0.5f) * TS, 0.18f, 8, 0.3f, 0, Rand(0, 6)});
    for (int k = 0; k < 18; k++) {
        float z = Rand(1 * TS, 63 * TS);
        if (std::fabs(z - FLAMINGO.y) < 6) continue;
        gPalms.push_back({Rand(52.5f * TS, 56.5f * TS), z, 0.1f, Rand(6, 10), Rand(0.2f, 1.4f), Rand(-0.6f, 0.6f), Rand(0, 6)});
    }
}

// Merge boxes into one mesh so the whole city is a single draw call.
static Model BuildBoxModel(const std::vector<Box>& boxes) {
    Mesh mesh{};
    int vc = (int)boxes.size() * 5 * 6;
    mesh.vertexCount = vc;
    mesh.triangleCount = vc / 3;
    mesh.vertices = (float*)MemAlloc(vc * 3 * sizeof(float));
    mesh.normals = (float*)MemAlloc(vc * 3 * sizeof(float));
    mesh.texcoords = (float*)MemAlloc(vc * 2 * sizeof(float));
    mesh.colors = (unsigned char*)MemAlloc(vc * 4);
    int v = 0;
    auto quad = [&](Vector3 a, Vector3 b, Vector3 c, Vector3 d, Vector3 n, Color col) {
        Vector3 q[6] = {a, b, c, a, c, d};
        for (const Vector3& p : q) {
            mesh.vertices[v * 3] = p.x; mesh.vertices[v * 3 + 1] = p.y; mesh.vertices[v * 3 + 2] = p.z;
            mesh.normals[v * 3] = n.x; mesh.normals[v * 3 + 1] = n.y; mesh.normals[v * 3 + 2] = n.z;
            mesh.colors[v * 4] = col.r; mesh.colors[v * 4 + 1] = col.g; mesh.colors[v * 4 + 2] = col.b; mesh.colors[v * 4 + 3] = 255;
            v++;
        }
    };
    for (const Box& b : boxes) {
        float x0 = b.c.x - b.s.x / 2, x1 = b.c.x + b.s.x / 2, y0 = b.c.y - b.s.y / 2, y1 = b.c.y + b.s.y / 2;
        float z0 = b.c.z - b.s.z / 2, z1 = b.c.z + b.s.z / 2;
        quad({x0, y1, z0}, {x0, y1, z1}, {x1, y1, z1}, {x1, y1, z0}, {0, 1, 0}, b.col);
        quad({x1, y0, z0}, {x1, y1, z0}, {x1, y1, z1}, {x1, y0, z1}, {1, 0, 0}, b.col);
        quad({x0, y0, z1}, {x0, y1, z1}, {x0, y1, z0}, {x0, y0, z0}, {-1, 0, 0}, b.col);
        quad({x1, y0, z1}, {x1, y1, z1}, {x0, y1, z1}, {x0, y0, z1}, {0, 0, 1}, b.col);
        quad({x0, y0, z0}, {x0, y1, z0}, {x1, y1, z0}, {x1, y0, z0}, {0, 0, -1}, b.col);
    }
    UploadMesh(&mesh, false);
    return LoadModelFromMesh(mesh);
}

// ---------------------------------------------------------------- 3D models
struct AnimModel {
    Model model{};
    ModelAnimation* anims = nullptr;
    int animCount = 0;
    int idle = -1, walk = -1, run = -1, death = -1;
    float scale = 1, yoff = 0, yaw = 0, cx = 0, cz = 0;
    std::string name;
};
static std::vector<AnimModel> gCharModels, gCarModels;
static std::vector<int> gCivModels;
static int gNicoModel = -1, gCopModel = -1;

static int FindAnim(const AnimModel& m, std::initializer_list<const char*> keys) {
    for (const char* k : keys)
        for (int i = 0; i < m.animCount; i++)
            if (Lower(m.anims[i].name).find(k) != std::string::npos) return i;
    return -1;
}
static bool LoadAsset(const char* path, bool isCar, AnimModel& out) {
    Model m = LoadModel(path);
    if (m.meshCount <= 0) return false;
    BoundingBox bb = GetModelBoundingBox(m);
    float dx = bb.max.x - bb.min.x, dy = bb.max.y - bb.min.y, dz = bb.max.z - bb.min.z;
    out.model = m;
    out.name = Lower(GetFileName(path));
    if (isCar) {
        float len = std::max(dx, dz);
        if (len <= 0) { UnloadModel(m); return false; }
        out.scale = 4.4f / len;
        out.yaw = (dx > dz ? 90.0f : 0.0f) + CAR_YAW_FIX;
    } else {
        if (dy <= 0) { UnloadModel(m); return false; }
        out.scale = 1.8f / dy;
        out.yaw = CHAR_YAW_FIX;
        out.anims = LoadModelAnimations(path, &out.animCount);
        out.idle = FindAnim(out, {"idle"});
        out.walk = FindAnim(out, {"walk"});
        out.run = FindAnim(out, {"run", "sprint", "jog"});
        out.death = FindAnim(out, {"death", "die", "dead"});
    }
    out.yoff = -bb.min.y * out.scale;
    out.cx = (bb.min.x + bb.max.x) / 2 * out.scale;
    out.cz = (bb.min.z + bb.max.z) / 2 * out.scale;
    return true;
}
static void LoadAssets() {
    const char* dirs[2] = {"assets/characters", "assets/cars"};
    for (int d = 0; d < 2; d++) {
        if (!DirectoryExists(dirs[d])) continue;
        FilePathList files = LoadDirectoryFiles(dirs[d]);
        for (unsigned i = 0; i < files.count; i++) {
            if (!IsFileExtension(files.paths[i], ".glb;.gltf")) continue;
            AnimModel am;
            if (!LoadAsset(files.paths[i], d == 1, am)) continue;
            if (d == 1) { gCarModels.push_back(am); continue; }
            int idx = (int)gCharModels.size();
            gCharModels.push_back(am);
            if (am.name.find("nico") != std::string::npos) gNicoModel = idx;
            else if (am.name.find("cop") != std::string::npos || am.name.find("police") != std::string::npos) gCopModel = idx;
            else gCivModels.push_back(idx);
        }
        UnloadDirectoryFiles(files);
    }
    if (gNicoModel < 0 && !gCivModels.empty()) gNicoModel = gCivModels[0];
    TraceLog(LOG_INFO, "CORALBAY: %d character models, %d car models", (int)gCharModels.size(), (int)gCarModels.size());
}
static void SetAllModelShaders(Shader s) {
    auto set = [&](Model& m) { for (int i = 0; i < m.materialCount; i++) m.materials[i].shader = s; };
    for (auto& a : gCharModels) set(a.model);
    for (auto& a : gCarModels) set(a.model);
}
static void DrawAnimModel(AnimModel& am, float x, float y, float z, float aRad, Color tint, int anim, int frame, bool lying) {
    if (anim >= 0 && anim < am.animCount && am.anims[anim].frameCount > 0)
        UpdateModelAnimation(am.model, am.anims[anim], frame % am.anims[anim].frameCount);
    float deg = aRad * RAD2DEG + am.yaw, t = deg * DEG2RAD;
    float ox = am.cx * std::cos(t) + am.cz * std::sin(t), oz = -am.cx * std::sin(t) + am.cz * std::cos(t);
    Matrix saved = am.model.transform;
    if (lying) am.model.transform = MatrixMultiply(saved, MatrixRotateX(-PI / 2));
    DrawModelEx(am.model, {x - ox, y + am.yoff + (lying ? 0.2f : 0.0f), z - oz}, {0, 1, 0}, deg, {am.scale, am.scale, am.scale}, tint);
    am.model.transform = saved;
}

// ---------------------------------------------------------------- people & cars
enum class Task { Wander, Idle, Flee, Down, Chase };
struct Ped {
    float x = 0, z = 0, y = 0, a = 0, spd = 0, base = 1.3f, walk = 0, r = 0.35f;
    Color skin{}, shirt{}, pants{}, hair{}, tint = WHITE;
    bool cop = false, temp = false, dropped = false, chain = false;
    Task task = Task::Wander;
    float timer = 0, tx = 0, tz = 0, fx = 0, fz = 0, jit = 0;
    const char* label = "";
    int model = -1, animState = -1;
    float animT = 0;
};
struct Car {
    float x = 0, z = 0, y = 0, a = 0, speed = 0;
    Color col{};
    bool ai = false, target = false;
    int ni = 0, nj = 0, ti = 0, tj = 0, model = -1;
    float wait = 0, honk = 0;
    const char* why = nullptr;
};
struct Pickup { float x, z; int value; float ph; };

static const std::vector<Color> SHIRTS = {Hex(0xff8fab), Hex(0xffd166), Hex(0x06d6a0), Hex(0x118ab2), Hex(0xef476f),
                                          Hex(0xf78c6b), Hex(0xf5f0e6), Hex(0x9b5de5), Hex(0xffb4a2)};
static const std::vector<Color> SKINS = {Hex(0xf1c9a5), Hex(0xd9a37a), Hex(0xa86b45), Hex(0x6b4128), Hex(0xe8b894)};
static const std::vector<Color> HAIRS = {Hex(0x2b1d14), Hex(0x5a3a22), Hex(0xd9b36b), Hex(0x141014), Hex(0x9a9a9a), Hex(0xb5442c)};
static const std::vector<Color> PANTS = {Hex(0x2d3142), Hex(0x4f5d75), Hex(0xe0d5c1), Hex(0x6b4f3a), Hex(0xf5f0e6)};
static const std::vector<Color> CARCOLS = {C_PINK, Hex(0xf7f3e3), Hex(0x2ec4b6), Hex(0x8d5cf6), Hex(0xe63946), Hex(0x7ad3ff)};
static const std::vector<const char*> IDLE_LABELS = {"on the phone", "smoking", "people-watching", "checking watch", "eating ice cream"};

static std::vector<Ped> gPeds;
static std::vector<Car> gCars;
static std::vector<Pickup> gPickups;
static Ped gPlayer;
static int gInCar = -1;
static const float SPAWN_X = 50.5f * TS, SPAWN_Z = 19.5f * TS;

static Ped MakePed(float x, float z, bool cop) {
    Ped p;
    p.x = x; p.z = z; p.y = GroundY(x, z); p.a = Rand(0, 2 * PI); p.base = Rand(1.1f, 1.6f);
    p.cop = cop; p.skin = Pick(SKINS);
    p.shirt = cop ? Hex(0x27418f) : Pick(SHIRTS);
    p.pants = cop ? Hex(0x1b2547) : Pick(PANTS);
    p.hair = cop ? Hex(0x16244f) : Pick(HAIRS);
    p.tx = x; p.tz = z;
    if (cop) {
        p.model = gCopModel >= 0 ? gCopModel : (gCivModels.empty() ? -1 : gCivModels[0]);
        p.tint = gCopModel >= 0 ? WHITE : Hex(0x7f95d8);
    } else if (!gCivModels.empty()) {
        p.model = Pick(gCivModels);
        p.tint = Mix(WHITE, p.shirt, 0.3f);
    }
    return p;
}
static bool Move(Ped& e, float dx, float dz) {
    bool m = false;
    if (!Blocked(e.x + dx, e.z, e.r)) { e.x += dx; m = true; }
    if (!Blocked(e.x, e.z + dz, e.r)) { e.z += dz; m = true; }
    return m;
}
static bool AnySpot(float& ox, float& oz) {
    for (int i = 0; i < 400; i++) {
        float x = Rand(TS, 57 * TS), z = Rand(TS, 63 * TS);
        if (FootOK(TileAt(x, z)) && !Blocked(x, z, 0.5f)) { ox = x; oz = z; return true; }
    }
    return false;
}
static bool SpotNear(float cx, float cz, float r0, float r1, float& ox, float& oz) {
    for (int i = 0; i < 40; i++) {
        float a = Rand(0, 2 * PI), d = Rand(r0, r1), x = cx + std::sin(a) * d, z = cz + std::cos(a) * d;
        if (FootOK(TileAt(x, z)) && !Blocked(x, z, 0.5f)) { ox = x; oz = z; return true; }
    }
    return false;
}

static float NodeX(int i) { return (12 * i + 1) * TS; }
static float NodeZ(int j) { return (12 * j + 1) * TS; }
static std::vector<std::pair<int, int>> Neighbors(int i, int j) {
    std::vector<std::pair<int, int>> r;
    if (i > 0) r.push_back({i - 1, j});
    if (i < 4) r.push_back({i + 1, j});
    if (j > 0) r.push_back({i, j - 1});
    if (j < 5) r.push_back({i, j + 1});
    return r;
}
struct Lane { float dx, dz, sx, sz, ex, ez; };
static Lane LaneOf(const Car& c) {
    Lane L;
    L.dx = (float)((c.ti > c.ni) - (c.ti < c.ni));
    L.dz = (float)((c.tj > c.nj) - (c.tj < c.nj));
    L.sx = NodeX(c.ni) - L.dz * LANE; L.sz = NodeZ(c.nj) + L.dx * LANE;
    L.ex = NodeX(c.ti) - L.dz * LANE; L.ez = NodeZ(c.tj) + L.dx * LANE;
    return L;
}
static void PickNext(Car& c) {
    auto all = Neighbors(c.ti, c.tj);
    std::vector<std::pair<int, int>> opts;
    for (auto& n : all) if (!(n.first == c.ni && n.second == c.nj)) opts.push_back(n);
    auto n = Pick(opts.empty() ? all : opts);
    c.ni = c.ti; c.nj = c.tj; c.ti = n.first; c.tj = n.second;
}
static void PlaceOnRoute(Car& c, int i, int j, float frac) {
    c.ni = i; c.nj = j;
    auto n = Pick(Neighbors(i, j));
    c.ti = n.first; c.tj = n.second;
    Lane L = LaneOf(c);
    c.x = L.sx + (L.ex - L.sx) * frac; c.z = L.sz + (L.ez - L.sz) * frac;
    c.a = std::atan2(L.dx, L.dz);
}
static bool CarBlocked(float x, float z, float a) {
    float s = std::sin(a), c = std::cos(a);
    const float pts[][2] = {{-0.95f, 2.1f}, {0.95f, 2.1f}, {-0.95f, -2.1f}, {0.95f, -2.1f}, {0, 2.3f}, {-1.0f, 0}, {1.0f, 0}};
    for (auto& p : pts) {
        float wx = x + s * p[1] + c * p[0], wz = z + c * p[1] - s * p[0];
        if (SolidAt(wx, wz)) return true;
    }
    return false;
}
static Car MakeCar(bool ai) {
    Car c;
    c.ai = ai; c.col = Pick(CARCOLS);
    if (!gCarModels.empty()) c.model = RandInt(0, (int)gCarModels.size() - 1);
    return c;
}

// ---------------------------------------------------------------- game state
enum class Mode { Title, Play, Pause, Busted, Quiz };
static Mode gMode = Mode::Title;
static int gWanted = 0, gCash = 0;
static float gSeenT = 0, gSpawnT = 0, gBustT = 0, gShake = 0, gTime = 0, gClock = 18 * 60;
static float gStarFlash = 0, gToastT = 0, gLastMouse = 0, gCrashCD = 0;
static bool gShowAI = false;
static const char* gToast = "";
static char gToastBuf[96];
static float gCamYaw = PI * 1.5f, gCamPitch = 0.32f;
static Vector3 gCamTarget = {SPAWN_X, 1.5f, SPAWN_Z};

static void Toast(const char* m) { gToast = m; gToastT = 2.0f; }
static void PlayerPos(float& x, float& z) {
    if (gInCar >= 0) { x = gCars[gInCar].x; z = gCars[gInCar].z; }
    else { x = gPlayer.x; z = gPlayer.z; }
}
static void AddWanted(int n) {
    int o = gWanted;
    gWanted = std::max(0, std::min(5, gWanted + n));
    gSeenT = 0;
    if (gWanted > o) gStarFlash = 1.2f;
}
static bool CopNear(float x, float z, float r) {
    for (auto& p : gPeds)
        if (p.cop && p.task != Task::Down && std::hypot(p.x - x, p.z - z) < r) return true;
    return false;
}
static void Scare(float x, float z, float r) {
    for (auto& p : gPeds) {
        if (p.cop || p.task == Task::Down) continue;
        if (std::hypot(p.x - x, p.z - z) < r) { p.task = Task::Flee; p.fx = x; p.fz = z; p.timer = Rand(4, 7); p.jit = Rand(-0.4f, 0.4f); }
    }
}
static void Knock(Ped& p, bool byCar) {
    if (p.task == Task::Down) return;
    p.task = Task::Down; p.timer = Rand(3, 5); p.spd = 0;
    if (p.cop) AddWanted(1);
    else {
        if (!p.dropped) { p.dropped = true; gPickups.push_back({p.x + Rand(-0.6f, 0.6f), p.z + Rand(-0.6f, 0.6f), RandInt(10, 80), Rand(0, 6)}); }
        if (CopNear(p.x, p.z, 40)) AddWanted(1);
        else if (Rand(0, 1) < (byCar ? 0.5f : 0.3f)) AddWanted(1);
    }
    float px = p.x, pz = p.z;
    Scare(px, pz, 25);
}

// ---------------------------------------------------------------- missions
struct Line { const char* who; const char* text; };
enum class MS { Wait, Ringing, Talking, GoFlamingo, StealCar, DeliverCar, GetStars, LoseCops, BringCash, Done };
static const std::vector<Line> CALL_1 = {
    {"Marco", "Nico Salazar. Six years, and not even a postcard."},
    {"Marco", "You still owe me. Come see me at the Pink Flamingo, on the beach."}};
static const std::vector<Line> MEET_1 = {
    {"Marco", "Look at you. Still got that jaw."},
    {"Marco", "A client of mine forgot who he owes. He drives a gold car."},
    {"Marco", "Take it, and bring it to my lockup behind the car park."},
    {"Nico", "And then we're square?"},
    {"Marco", "Then we're talking."}};
static const std::vector<Line> CALL_2 = {
    {"Marco", "The cops are asking about me. I need them looking at somebody else."},
    {"Marco", "Make some noise. Two stars. Then lose them."}};
static const std::vector<Line> CALL_3 = {
    {"Marco", "Beautiful work. Now bring me what you owe."},
    {"Marco", "Fifteen hundred. The Pink Flamingo. Don't make me wait."}};
static const std::vector<Line> END_LINES = {
    {"Marco", "Paid in full. Coral Bay's all yours, champ."},
    {"Nico", "It always was."}};

static struct {
    MS stage = MS::Wait, after = MS::Wait;
    float t = 5.0f;
    const std::vector<Line>* lines = nullptr;
    const std::vector<Line>* pending = nullptr;
    MS pendingAfter = MS::Wait;
    int line = 0;
    float lineT = 0;
    int targetCar = -1;
    int nextCall = 0;
    const char* objective = "";
    float objT = 0, denyT = 0;
    const char* banner = "";
    char bannerSub[32] = "";
    float bannerT = 0;
} M;

static void Objective(const char* o) { M.objective = o; M.objT = 6.0f; }
static void Say(const std::vector<Line>& lines, MS after) { M.lines = &lines; M.line = 0; M.lineT = 0; M.stage = MS::Talking; M.after = after; }
static void Call(const std::vector<Line>& lines, MS after) { M.pending = &lines; M.pendingAfter = after; M.stage = MS::Ringing; M.t = 2.4f; gSynth.Play(SFX_RING); }
static void Passed(const char* sub, int reward) {
    gCash += reward;
    M.banner = "MISSION PASSED";
    snprintf(M.bannerSub, sizeof M.bannerSub, "%s", sub);
    M.bannerT = 4.0f;
    gSynth.Play(SFX_PASS);
}
static void EnterStage(MS s) {
    M.stage = s;
    switch (s) {
        case MS::GoFlamingo: Objective("Go to the Pink Flamingo on the beach."); break;
        case MS::StealCar: {
            bool valid = M.targetCar >= 0 && M.targetCar < (int)gCars.size();
            if (!valid) {
                float px, pz; PlayerPos(px, pz);
                int best = -1; float bd = 1e9f;
                for (int i = 0; i < (int)gCars.size(); i++) {
                    if (!gCars[i].ai) continue;
                    float d = std::fabs(std::hypot(gCars[i].x - px, gCars[i].z - pz) - 60.0f);
                    if (d < bd) { bd = d; best = i; }
                }
                M.targetCar = best;
                if (best >= 0) { gCars[best].target = true; gCars[best].col = C_GOLD; }
                Objective("Steal the gold car.");
            } else Objective("Get back in the gold car.");
            break;
        }
        case MS::DeliverCar: Objective("Drive it to Marco's lockup."); break;
        case MS::GetStars: Objective("Cause trouble until you have 2 wanted stars."); break;
        case MS::LoseCops: Objective("Lose the cops."); break;
        case MS::BringCash: Objective("Bring $1500 to Marco at the Pink Flamingo."); break;
        case MS::Done: Objective("Coral Bay is yours. Free roam."); break;
        default: break;
    }
}
static bool MarkerActive(Vector2& pos) {
    switch (M.stage) {
        case MS::GoFlamingo: case MS::BringCash: pos = FLAMINGO; return true;
        case MS::DeliverCar: pos = LOCKUP; return true;
        case MS::StealCar:
            if (M.targetCar >= 0) { pos = {gCars[M.targetCar].x, gCars[M.targetCar].z}; return true; }
            return false;
        default: return false;
    }
}
static void UpdateMission(float dt) {
    float px, pz; PlayerPos(px, pz);
    if (M.objT > 0) M.objT -= dt;
    if (M.bannerT > 0) M.bannerT -= dt;
    if (M.denyT > 0) M.denyT -= dt;
    switch (M.stage) {
        case MS::Wait:
            M.t -= dt;
            if (M.t <= 0) {
                if (M.nextCall == 0) Call(CALL_1, MS::GoFlamingo);
                else if (M.nextCall == 1) Call(CALL_2, MS::GetStars);
                else if (M.nextCall == 2) Call(CALL_3, MS::BringCash);
            }
            break;
        case MS::Ringing:
            M.t -= dt;
            if (M.t <= 0) Say(*M.pending, M.pendingAfter);
            break;
        case MS::Talking:
            M.lineT += dt;
            if (M.lineT > 3.4f) { M.line++; M.lineT = 0; }
            if (M.line >= (int)M.lines->size()) EnterStage(M.after);
            break;
        case MS::GoFlamingo:
            if (std::hypot(px - FLAMINGO.x, pz - FLAMINGO.y) < 3.5f) Say(MEET_1, MS::StealCar);
            break;
        case MS::StealCar:
            if (M.targetCar >= 0 && gInCar == M.targetCar) EnterStage(MS::DeliverCar);
            break;
        case MS::DeliverCar:
            if (gInCar != M.targetCar) { EnterStage(MS::StealCar); break; }
            if (std::hypot(px - LOCKUP.x, pz - LOCKUP.y) < 5.0f) {
                gCars[M.targetCar].target = false;
                M.targetCar = -1;
                Passed("$500", 500);
                M.nextCall = 1; M.stage = MS::Wait; M.t = 7;
            }
            break;
        case MS::GetStars:
            if (gWanted >= 2) EnterStage(MS::LoseCops);
            break;
        case MS::LoseCops:
            if (gWanted == 0) { Passed("$1000", 1000); M.nextCall = 2; M.stage = MS::Wait; M.t = 6; }
            break;
        case MS::BringCash:
            if (std::hypot(px - FLAMINGO.x, pz - FLAMINGO.y) < 3.5f) {
                if (gCash >= 1500) {
                    gCash -= 1500;
                    Passed("Debt paid", 0);
                    Say(END_LINES, MS::Done);
                } else if (M.denyT <= 0) {
                    snprintf(gToastBuf, sizeof gToastBuf, "Marco wants $1500. You have $%d.", gCash);
                    Toast(gToastBuf);
                    M.denyT = 5;
                }
            }
            break;
        default: break;
    }
}

// ---------------------------------------------------------------- world setup
static void InitWorld() {
    BuildMap();
    BuildScenery();
    gPlayer = MakePed(SPAWN_X, SPAWN_Z, false);
    gPlayer.skin = Hex(0xc98d62); gPlayer.shirt = C_TEAL; gPlayer.pants = Hex(0x5d6070);
    gPlayer.hair = Hex(0x1b120c); gPlayer.chain = true; gPlayer.a = -PI / 2;
    gPlayer.model = gNicoModel;
    gPlayer.tint = (gNicoModel >= 0 && gCharModels[gNicoModel].name.find("nico") != std::string::npos) ? WHITE : Mix(WHITE, C_TEAL, 0.45f);
    for (int i = 0; i < 60; i++) { float x, z; if (AnySpot(x, z)) gPeds.push_back(MakePed(x, z, false)); }
    for (int i = 0; i < 4; i++) { float x, z; if (AnySpot(x, z)) gPeds.push_back(MakePed(x, z, true)); }
    const int lots[2] = {8, 11};
    for (int id : lots) {
        float x0 = (12 * (id % 5) + 3) * TS, z0 = (12 * (id / 5) + 3) * TS;
        for (int s : {1, 3, 6}) {
            Car c = MakeCar(false);
            c.x = x0 + s * TS + TS / 2; c.z = z0 + 3.2f; c.a = 0; c.y = 0.28f;
            gCars.push_back(c);
        }
    }
    for (int k = 0; k < 10; k++) {
        Car c = MakeCar(true);
        c.speed = 8;
        PlaceOnRoute(c, RandInt(0, 4), RandInt(0, 5), Rand(0.1f, 0.8f));
        gCars.push_back(c);
    }
}

// ---------------------------------------------------------------- actions
// ---------------------------------------------------------------- quiz
static struct {
    int ped = -1, q = 0, picked = -1, asked = 0, right = 0;
    int order[4] = {0, 1, 2, 3};   // shuffled option order for the current question
    std::vector<int> deck;         // question indexes not asked yet
} Q;

static int NearestTalker() {
    int best = -1; float bd = 2.6f;
    for (int i = 0; i < (int)gPeds.size(); i++) {
        const Ped& p = gPeds[i];
        if (p.cop || p.task == Task::Down || p.task == Task::Flee) continue;
        float d = std::hypot(p.x - gPlayer.x, p.z - gPlayer.z);
        if (d < bd) { best = i; bd = d; }
    }
    return best;
}
static void StartQuiz(int pi) {
    if (Q.deck.empty()) {
        for (int i = 0; i < QUIZ_COUNT; i++) Q.deck.push_back(i);
        std::shuffle(Q.deck.begin(), Q.deck.end(), rng);
    }
    Q.q = Q.deck.back(); Q.deck.pop_back();
    std::shuffle(std::begin(Q.order), std::end(Q.order), rng);
    Q.ped = pi; Q.picked = -1;
    Ped& p = gPeds[pi];
    p.task = Task::Idle; p.label = "quizzing Nico"; p.timer = 2; p.spd = 0;
    p.a = std::atan2(gPlayer.x - p.x, gPlayer.z - p.z);
    gPlayer.a = std::atan2(p.x - gPlayer.x, p.z - gPlayer.z); gPlayer.spd = 0;
    gMode = Mode::Quiz;
}
static void UpdateQuiz() {
    if (Q.picked < 0) {
        const int keys[4][2] = {{KEY_ONE, KEY_A}, {KEY_TWO, KEY_B}, {KEY_THREE, KEY_C}, {KEY_FOUR, KEY_D}};
        for (int i = 0; i < 4; i++)
            if (IsKeyPressed(keys[i][0]) || IsKeyPressed(keys[i][1])) {
                Q.picked = i; Q.asked++;
                if (Q.order[i] == QUIZ[Q.q].answer) { Q.right++; gCash += 100; gSynth.Play(SFX_CASH); }
                else gSynth.Play(SFX_CRASH);
            }
        if (IsKeyPressed(KEY_ESCAPE)) gMode = Mode::Play;   // walk away without answering
    } else if (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_SPACE) || IsKeyPressed(KEY_ESCAPE)) {
        if (Q.ped >= 0 && Q.ped < (int)gPeds.size()) gPeds[Q.ped].timer = 1.5f;
        gMode = Mode::Play;
    }
}

static int NearestCar() {
    int best = -1; float bd = 3.6f;
    for (int i = 0; i < (int)gCars.size(); i++) {
        float d = std::hypot(gCars[i].x - gPlayer.x, gCars[i].z - gPlayer.z);
        if (d < bd) { best = i; bd = d; }
    }
    return best;
}
static void ToggleCar() {
    gSynth.Play(SFX_DOOR);
    if (gInCar >= 0) {
        Car& c = gCars[gInCar];
        float s = std::sin(c.a), co = std::cos(c.a);
        for (int side : {1, -1}) {
            float x = c.x + co * 2.0f * side, z = c.z - s * 2.0f * side;
            if (!Blocked(x, z, 0.4f)) { gPlayer.x = x; gPlayer.z = z; break; }
        }
        if (std::fabs(c.speed) > 5) Toast("Bailed out");
        c.speed = 0; gPlayer.a = c.a; gInCar = -1;
        gSynth.radio = false;
        return;
    }
    int ci = NearestCar();
    if (ci < 0) return;
    Car& c = gCars[ci];
    if (c.ai) {
        c.ai = false;
        float s = std::sin(c.a), co = std::cos(c.a);
        Ped d = MakePed(c.x - co * 2.0f, c.z + s * 2.0f, false);
        bool place = !Blocked(d.x, d.z, 0.4f);
        float cx = c.x, cz = c.z;
        if (CopNear(cx, cz, 45)) AddWanted(1);
        if (place) { d.task = Task::Flee; d.fx = cx; d.fz = cz; d.timer = 6; gPeds.push_back(d); }
        Toast("Carjacked");
    }
    gCars[ci].speed = std::min(gCars[ci].speed, 2.0f);
    gInCar = ci;
    gSynth.radio = true;
    snprintf(gToastBuf, sizeof gToastBuf, "%s", STATIONS[gSynth.station].name);
    if (gToastT <= 0) Toast(gToastBuf);
}
static void Horn() {
    if (gInCar < 0) return;
    Car& c = gCars[gInCar];
    c.honk = 0.5f;
    gSynth.Play(SFX_HORN);
    float s = std::sin(c.a), co = std::cos(c.a);
    for (auto& p : gPeds) {
        float rx = p.x - c.x, rz = p.z - c.z, f = rx * s + rz * co, l = std::fabs(rx * co - rz * s);
        if (f > 0 && f < 14 && l < 5 && !p.cop && p.task != Task::Down) { p.task = Task::Flee; p.fx = c.x; p.fz = c.z; p.timer = 3; }
    }
}
static void CycleRadio() {
    if (gInCar < 0) return;
    if (!gSynth.radio) { gSynth.radio = true; gSynth.station = 0; }
    else if (gSynth.station == 0) gSynth.station = 1;
    else gSynth.radio = false;
    Toast(gSynth.radio ? STATIONS[gSynth.station].name : "Radio off");
}
static void Respawn() {
    if (gInCar >= 0) { gCars[gInCar].speed = 0; gInCar = -1; }
    gSynth.radio = false;
    gPlayer.x = SPAWN_X; gPlayer.z = SPAWN_Z;
    int lost = gCash / 4; gCash -= lost; gWanted = 0;
    for (int i = (int)gPeds.size() - 1; i >= 0; i--) {
        if (gPeds[i].temp) gPeds.erase(gPeds.begin() + i);
        else if (gPeds[i].task == Task::Chase) { gPeds[i].task = Task::Wander; gPeds[i].timer = 0; }
    }
    if (M.stage == MS::LoseCops) { Toast("Mission failed. Try again."); EnterStage(MS::GetStars); return; }
    if (lost) { snprintf(gToastBuf, sizeof gToastBuf, "Lost $%d in the fine", lost); Toast(gToastBuf); }
    else Toast("Released with a warning");
}

// ---------------------------------------------------------------- AI updates
static int AnimStateOf(const Ped& p) {
    if (p.task == Task::Down) return 4;
    if (p.spd > 3.0f) return 2;
    if (p.spd > 0.1f) return 1;
    return 0;
}
static void TickAnim(Ped& p, float dt) {
    int s = AnimStateOf(p);
    if (s != p.animState) { p.animState = s; p.animT = 0; } else p.animT += dt;
}
static void UpdatePed(Ped& p, float dt) {
    p.timer -= dt;
    float plx, plz; PlayerPos(plx, plz);
    switch (p.task) {
        case Task::Wander: {
            float dx = p.tx - p.x, dz = p.tz - p.z, d = std::hypot(dx, dz);
            if (d < 0.6f || p.timer <= 0) {
                if (Rand(0, 1) < 0.3f && p.timer > -1) { p.task = Task::Idle; p.label = Pick(IDLE_LABELS); p.timer = Rand(2, 6); p.spd = 0; break; }
                float sx, sz;
                if (SpotNear(p.x, p.z, 3, 30, sx, sz)) { p.tx = sx; p.tz = sz; }
                p.timer = Rand(6, 12);
                break;
            }
            p.a = TurnToward(p.a, std::atan2(dx, dz), dt * 8);
            p.spd = p.base;
            if (!Move(p, std::sin(p.a) * p.spd * dt, std::cos(p.a) * p.spd * dt)) p.timer = 0;
            break;
        }
        case Task::Idle:
            p.spd = 0;
            if (p.timer <= 0) { p.task = Task::Wander; p.timer = -2; }
            break;
        case Task::Flee: {
            p.a = TurnToward(p.a, std::atan2(p.x - p.fx, p.z - p.fz) + p.jit, dt * 10);
            p.spd = 4.2f;
            if (!Move(p, std::sin(p.a) * p.spd * dt, std::cos(p.a) * p.spd * dt)) p.jit = Rand(-1.6f, 1.6f);
            if (p.timer <= 0) { p.task = Task::Wander; p.timer = 0; }
            break;
        }
        case Task::Down:
            p.spd = 0;
            if (p.timer <= 0) {
                if (p.cop && gWanted > 0) p.task = Task::Chase;
                else { p.task = Task::Flee; p.fx = plx; p.fz = plz; p.timer = 5; p.jit = 0; }
            }
            break;
        case Task::Chase: {
            p.a = TurnToward(p.a, std::atan2(plx - p.x, plz - p.z) + p.jit, dt * 10);
            p.spd = 5.2f;
            if (!Move(p, std::sin(p.a) * p.spd * dt, std::cos(p.a) * p.spd * dt)) p.jit = Rand(-1.3f, 1.3f);
            else p.jit *= std::pow(0.05f, dt);
            if (gWanted <= 0) { p.task = Task::Wander; p.timer = 0; }
            break;
        }
    }
    if (p.spd > 0) p.walk += p.spd * dt * 3.2f;
    p.y += (GroundY(p.x, p.z) - p.y) * std::min(1.0f, dt * 12);
    TickAnim(p, dt);
}

static const char* ObstacleAhead(const Car& c) {
    float s = std::sin(c.a), co = std::cos(c.a);
    auto hit = [&](float ox, float oz) {
        float rx = ox - c.x, rz = oz - c.z, f = rx * s + rz * co, l = std::fabs(rx * co - rz * s);
        return f > 1.5f && f < 10 && l < 2.2f;
    };
    if (gInCar < 0 && hit(gPlayer.x, gPlayer.z)) return "player";
    for (auto& o : gCars) if (&o != &c && hit(o.x, o.z)) return "car";
    for (auto& p : gPeds) if (hit(p.x, p.z)) return "pedestrian";
    return nullptr;
}
static void UpdateTraffic(Car& c, float dt) {
    Lane L = LaneOf(c);
    float len = std::hypot(L.ex - L.sx, L.ez - L.sz);
    float along = Clampf((c.x - L.sx) * L.dx + (c.z - L.sz) * L.dz, 0, len);
    if (len - along < c.speed * dt + 0.8f) { PickNext(c); return; }
    float aim = std::min(len, along + 10), ax = L.sx + L.dx * aim, az = L.sz + L.dz * aim;
    c.a += AngDiff(std::atan2(ax - c.x, az - c.z), c.a) * std::min(1.0f, 6 * dt);
    const char* ob = ObstacleAhead(c);
    c.wait = ob ? c.wait + dt : 0;
    c.why = ob;
    if (ob && ob[0] == 'p' && ob[1] == 'l' && c.wait > 0.8f && c.honk <= 0 && Rand(0, 1) < 0.02f) {
        c.honk = 0.5f;
        if (std::hypot(c.x - gPlayer.x, c.z - gPlayer.z) < 20) gSynth.Play(SFX_HORN);
    }
    float tgt = (ob && c.wait < 4) ? 0.0f : 9.0f;
    c.speed += (tgt - c.speed) * std::min(1.0f, (ob ? 6.0f : 1.2f) * dt);
    c.x += std::sin(c.a) * c.speed * dt;
    c.z += std::cos(c.a) * c.speed * dt;
}
static void Crash(float amount) {
    gShake = std::max(gShake, amount);
    if (gCrashCD <= 0) { gSynth.Play(SFX_CRASH); gCrashCD = 0.35f; }
}
static void UpdatePlayerCar(Car& c, float dt) {
    float thr = (IsKeyDown(KEY_W) || IsKeyDown(KEY_UP) ? 1.f : 0.f) - (IsKeyDown(KEY_S) || IsKeyDown(KEY_DOWN) ? 1.f : 0.f);
    float steer = (IsKeyDown(KEY_A) || IsKeyDown(KEY_LEFT) ? 1.f : 0.f) - (IsKeyDown(KEY_D) || IsKeyDown(KEY_RIGHT) ? 1.f : 0.f);
    bool hb = IsKeyDown(KEY_SPACE);
    if (thr > 0) c.speed += (c.speed < 0 ? 25.f : 12.f) * dt;
    else if (thr < 0) c.speed -= (c.speed > 0 ? 25.f : 8.f) * dt;
    else c.speed *= std::pow(0.55f, dt);
    if (hb) c.speed *= std::pow(0.15f, dt);
    c.speed = Clampf(c.speed, -9, 30);
    float grip = Clampf(std::fabs(c.speed) / 8.f, 0, 1);
    c.a += steer * 1.9f * grip * (hb ? 1.6f : 1.f) * dt * (c.speed >= 0 ? 1.f : -1.f);
    gSynth.engine = 1; gSynth.engineRev = std::fabs(c.speed); gSynth.throttle = thr > 0 ? 1.0f : 0.2f;

    float nx = c.x + std::sin(c.a) * c.speed * dt, nz = c.z + std::cos(c.a) * c.speed * dt;
    if (CarBlocked(nx, nz, c.a)) {
        if (std::fabs(c.speed) > 8) Crash(std::min(0.6f, std::fabs(c.speed) * 0.02f));
        c.speed *= -0.3f;
    } else { c.x = nx; c.z = nz; }

    for (auto& o : gCars) {
        if (&o == &c) continue;
        float d = std::hypot(o.x - c.x, o.z - c.z);
        if (d < 3.2f && d > 0.01f) {
            float px = (o.x - c.x) / d, pz = (o.z - c.z) / d, push = 3.2f - d;
            if (std::fabs(c.speed) > 8) Crash(0.35f);
            c.x -= px * push * 0.6f; c.z -= pz * push * 0.6f;
            if (!CarBlocked(o.x + px * push * 0.4f, o.z + pz * push * 0.4f, o.a)) { o.x += px * push * 0.4f; o.z += pz * push * 0.4f; }
            c.speed *= -0.3f;
        }
    }
    if (std::fabs(c.speed) > 4) {
        for (size_t i = 0; i < gPeds.size(); i++) {
            if (gPeds[i].task == Task::Down) continue;
            if (std::hypot(gPeds[i].x - c.x, gPeds[i].z - c.z) < 1.5f) {
                gSynth.Play(SFX_PUNCH);
                Knock(gPeds[i], true);
                Move(gPeds[i], std::sin(c.a) * 1.5f, std::cos(c.a) * 1.5f);
                c.speed *= 0.85f; gShake = 0.2f;
            }
        }
    }
    if (c.speed > 12)
        for (auto& p : gPeds) {
            if (p.cop || p.task == Task::Down || p.task == Task::Flee) continue;
            float d = std::hypot(p.x - c.x, p.z - c.z);
            if (d < 8 && d > 1.5f) { p.task = Task::Flee; p.fx = c.x; p.fz = c.z; p.timer = 2; p.jit = Rand(-0.8f, 0.8f); }
        }
    c.y += (GroundY(c.x, c.z) - c.y) * std::min(1.0f, dt * 10);
}

static void UpdatePlay(float dt) {
    gTime += dt; gClock += dt; if (gClock >= 24 * 60) gClock -= 24 * 60;
    if (gCrashCD > 0) gCrashCD -= dt;
    Vector2 md = GetMouseDelta();
    gCamYaw -= md.x * 0.0035f;
    gCamPitch = Clampf(gCamPitch + md.y * 0.0025f, 0.05f, 1.2f);
    gLastMouse = std::fabs(md.x) > 0.5f ? 0 : gLastMouse + dt;

    if (IsKeyPressed(KEY_ENTER) && gInCar < 0 && NearestTalker() >= 0) { StartQuiz(NearestTalker()); return; }
    if (IsKeyPressed(KEY_F) || IsKeyPressed(KEY_ENTER)) ToggleCar();
    if (IsKeyPressed(KEY_H)) Horn();
    if (IsKeyPressed(KEY_R)) CycleRadio();
    if (IsKeyPressed(KEY_TAB)) gShowAI = !gShowAI;

    gSynth.engine = 0;
    if (gInCar >= 0) {
        Car& c = gCars[gInCar];
        UpdatePlayerCar(c, dt);
        gPlayer.x = c.x; gPlayer.z = c.z;
        if (gLastMouse > 1.2f && c.speed > 2) gCamYaw += AngDiff(c.a, gCamYaw) * std::min(1.0f, 2.5f * dt);
    } else {
        float fw = (IsKeyDown(KEY_W) || IsKeyDown(KEY_UP) ? 1.f : 0.f) - (IsKeyDown(KEY_S) || IsKeyDown(KEY_DOWN) ? 1.f : 0.f);
        float rt = (IsKeyDown(KEY_D) || IsKeyDown(KEY_RIGHT) ? 1.f : 0.f) - (IsKeyDown(KEY_A) || IsKeyDown(KEY_LEFT) ? 1.f : 0.f);
        float cfx = std::sin(gCamYaw), cfz = std::cos(gCamYaw);
        float mx = cfx * fw - cfz * rt, mz = cfz * fw + cfx * rt, m = std::hypot(mx, mz);
        if (m > 0.1f) {
            gPlayer.a = TurnToward(gPlayer.a, std::atan2(mx, mz), dt * 14);
            gPlayer.spd = IsKeyDown(KEY_LEFT_SHIFT) ? 7.5f : 4.8f;
            Move(gPlayer, mx / m * gPlayer.spd * dt, mz / m * gPlayer.spd * dt);
            gPlayer.walk += gPlayer.spd * dt * 3.2f;
        } else gPlayer.spd = 0;
        gPlayer.y += (GroundY(gPlayer.x, gPlayer.z) - gPlayer.y) * std::min(1.0f, dt * 12);
    }
    TickAnim(gPlayer, dt);

    for (int i = 0; i < (int)gCars.size(); i++) {
        Car& c = gCars[i];
        if (c.honk > 0) c.honk -= dt;
        if (c.ai) UpdateTraffic(c, dt);
        else if (i != gInCar) c.speed = 0;
    }
    for (size_t i = 0; i < gPeds.size(); i++) UpdatePed(gPeds[i], dt);

    float px, pz; PlayerPos(px, pz);
    for (int i = (int)gPeds.size() - 1; i >= 0; i--) {
        Ped& p = gPeds[i];
        float d = std::hypot(p.x - px, p.z - pz);
        if (p.temp && gWanted == 0 && d > 80) { gPeds.erase(gPeds.begin() + i); continue; }
        if (!p.cop && d > 110) {
            float sx, sz;
            if (SpotNear(px, pz, 60, 100, sx, sz)) { p.x = sx; p.z = sz; p.task = Task::Wander; p.timer = 0; p.dropped = false; }
        }
    }
    for (int ci = 0; ci < (int)gCars.size(); ci++) {
        Car& c = gCars[ci];
        if (!c.ai || std::hypot(c.x - px, c.z - pz) < 150) continue;
        int bi = 0, bj = 0; float bd = 1e9f;
        for (int i = 0; i < 5; i++)
            for (int j = 0; j < 6; j++) {
                float d = std::fabs(std::hypot(NodeX(i) - px, NodeZ(j) - pz) - 80);
                if (d < bd) { bd = d; bi = i; bj = j; }
            }
        PlaceOnRoute(c, bi, bj, 0);
        c.speed = 6;
    }

    gSynth.siren = 0;
    if (gWanted > 0) {
        int chasing = 0;
        for (auto& p : gPeds)
            if (p.cop) {
                if (p.task != Task::Down && p.task != Task::Chase && std::hypot(p.x - px, p.z - pz) < 90) { p.task = Task::Chase; p.jit = 0; }
                if (p.task == Task::Chase) chasing++;
            }
        gSpawnT -= dt;
        if (chasing < std::min(8, gWanted * 2) && gSpawnT <= 0) {
            float sx, sz;
            if (SpotNear(px, pz, 45, 65, sx, sz)) { Ped c = MakePed(sx, sz, true); c.temp = true; c.task = Task::Chase; gPeds.push_back(c); }
            gSpawnT = 1.3f;
        }
        gSynth.siren = 0.35f + 0.13f * gWanted;
        if (CopNear(px, pz, 35)) gSeenT = 0; else gSeenT += dt;
        if (gSeenT > 8) {
            gWanted--; gSeenT = 0;
            Toast(gWanted ? "They're losing you" : "Lost them");
        }
        for (auto& p : gPeds) {
            if (p.task != Task::Chase) continue;
            float d = std::hypot(p.x - px, p.z - pz);
            bool caught = (gInCar < 0 && d < 1.0f) || (gInCar >= 0 && std::fabs(gCars[gInCar].speed) < 1.5f && d < 2.4f);
            if (caught) {
                gMode = Mode::Busted; gBustT = 3.0f;
                if (gInCar >= 0) gCars[gInCar].speed = 0;
                gSynth.Play(SFX_BUSTED); gSynth.siren = 0; gSynth.engine = 0; gSynth.radio = false;
                break;
            }
        }
    }

    for (int i = (int)gPickups.size() - 1; i >= 0; i--) {
        if (std::hypot(gPickups[i].x - px, gPickups[i].z - pz) < (gInCar >= 0 ? 2.5f : 1.2f)) {
            gCash += gPickups[i].value;
            snprintf(gToastBuf, sizeof gToastBuf, "+$%d", gPickups[i].value);
            Toast(gToastBuf);
            gSynth.Play(SFX_CASH);
            gPickups.erase(gPickups.begin() + i);
        }
    }
    UpdateMission(dt);
    gShake *= std::pow(0.02f, dt);
}

// ---------------------------------------------------------------- rendering
static Shader gDefShader{};
static Shader shLight{}, shDepth{}, shExtract{}, shBlur{}, shComp{};
static bool gFX = true, gFXok = false;
static int L_view, L_sun, L_sunCol, L_sky, L_ground, L_fogCol, L_fogDen, L_emis, L_shadowMap, L_lightVP, L_shadowsOn;
static int B_dir, C_glow, C_amt;
static RenderTexture2D rtScene{}, rtGlowA{}, rtGlowB{}, rtShadow{};
static int rtW = 0, rtH = 0;
static Model mStatic{}, mEmis{};
static Matrix gLightVP;
static Vector3 gSun;
enum Pass { PASS_DEPTH, PASS_COLOR, PASS_PLAIN };

static void Flush() { rlDrawRenderBatchActive(); }
static void V3(Shader s, int loc, Vector3 v) { SetShaderValue(s, loc, &v, SHADER_UNIFORM_VEC3); }
static Vector3 CV(Color c, float k = 1.0f) { return {c.r / 255.f * k, c.g / 255.f * k, c.b / 255.f * k}; }
static void SetEmissive(bool on) {
    Flush();
    float e = on ? 1.0f : 0.0f;
    SetShaderValue(shLight, L_emis, &e, SHADER_UNIFORM_FLOAT);
}
static void ResetBatchUniforms(Shader s) {
    Flush();
    SetShaderValueMatrix(s, s.locs[SHADER_LOC_MATRIX_MODEL], MatrixIdentity());
    float white[4] = {1, 1, 1, 1};
    SetShaderValue(s, s.locs[SHADER_LOC_COLOR_DIFFUSE], white, SHADER_UNIFORM_VEC4);
}

static void InitGraphics() {
    gDefShader.id = rlGetShaderIdDefault();
    gDefShader.locs = rlGetShaderLocsDefault();
    mStatic = BuildBoxModel(gStatic);
    mEmis = BuildBoxModel(gEmis);
    gSun = Vector3Normalize({-0.62f, 0.42f, 0.28f});
    shLight = LoadShaderFromMemory(LIGHT_VS, LIGHT_FS);
    shDepth = LoadShaderFromMemory(LIGHT_VS, DEPTH_FS);
    shExtract = LoadShaderFromMemory(nullptr, EXTRACT_FS);
    shBlur = LoadShaderFromMemory(nullptr, BLUR_FS);
    shComp = LoadShaderFromMemory(nullptr, COMPOSITE_FS);
    unsigned def = rlGetShaderIdDefault();
    gFXok = shLight.id != def && shDepth.id != def && shExtract.id != def && shBlur.id != def && shComp.id != def;
    if (!gFXok) { gFX = false; TraceLog(LOG_WARNING, "CORALBAY: shaders unavailable, using simple graphics"); return; }
    L_view = GetShaderLocation(shLight, "viewPos");
    L_sun = GetShaderLocation(shLight, "sunDir");
    L_sunCol = GetShaderLocation(shLight, "sunColor");
    L_sky = GetShaderLocation(shLight, "skyColor");
    L_ground = GetShaderLocation(shLight, "groundColor");
    L_fogCol = GetShaderLocation(shLight, "fogColor");
    L_fogDen = GetShaderLocation(shLight, "fogDensity");
    L_emis = GetShaderLocation(shLight, "emissive");
    L_shadowMap = GetShaderLocation(shLight, "shadowMap");
    L_lightVP = GetShaderLocation(shLight, "lightVP");
    L_shadowsOn = GetShaderLocation(shLight, "shadowsOn");
    B_dir = GetShaderLocation(shBlur, "dir");
    C_glow = GetShaderLocation(shComp, "glowTex");
    C_amt = GetShaderLocation(shComp, "glowAmt");
    V3(shLight, L_sun, gSun);
    V3(shLight, L_sunCol, {1.05f, 0.78f, 0.58f});
    V3(shLight, L_sky, {0.55f, 0.48f, 0.72f});
    V3(shLight, L_ground, {0.42f, 0.30f, 0.30f});
    V3(shLight, L_fogCol, CV(FOG));
    float fd = 0.0055f;
    SetShaderValue(shLight, L_fogDen, &fd, SHADER_UNIFORM_FLOAT);
    rtShadow = LoadRenderTexture(SHADOW_RES, SHADOW_RES);
    SetTextureFilter(rtShadow.texture, TEXTURE_FILTER_POINT);
    SetTextureWrap(rtShadow.texture, TEXTURE_WRAP_CLAMP);
}
static void EnsureTargets(int w, int h) {
    if (!gFXok || (w == rtW && h == rtH)) return;
    if (rtW) { UnloadRenderTexture(rtScene); UnloadRenderTexture(rtGlowA); UnloadRenderTexture(rtGlowB); }
    rtW = w; rtH = h;
    rtScene = LoadRenderTexture(w, h);
    rtGlowA = LoadRenderTexture(std::max(1, w / 2), std::max(1, h / 2));
    rtGlowB = LoadRenderTexture(std::max(1, w / 2), std::max(1, h / 2));
    SetTextureFilter(rtScene.texture, TEXTURE_FILTER_BILINEAR);
    SetTextureFilter(rtGlowA.texture, TEXTURE_FILTER_BILINEAR);
    SetTextureFilter(rtGlowB.texture, TEXTURE_FILTER_BILINEAR);
}

// Block-built fallbacks (used when no models are installed)
static void DrawPersonBoxes(const Ped& p) {
    rlPushMatrix();
    rlTranslatef(p.x, p.y, p.z);
    rlRotatef(p.a * RAD2DEG, 0, 1, 0);
    if (p.task == Task::Down) { rlTranslatef(0, 0.15f, 0.3f); rlRotatef(-90, 1, 0, 0); }
    float sw = (p.spd > 0.05f && p.task != Task::Down) ? std::sin(p.walk) * 32.f : 0.f;
    for (int side : {-1, 1}) {
        rlPushMatrix(); rlTranslatef(0.12f * side, 0.85f, 0); rlRotatef(sw * side, 1, 0, 0);
        DrawCube({0, -0.4f, 0}, 0.18f, 0.8f, 0.2f, p.pants);
        DrawCube({0, -0.78f, 0.05f}, 0.19f, 0.08f, 0.3f, Hex(0x2a2230));
        rlPopMatrix();
    }
    DrawCube({0, 1.18f, 0}, 0.5f, 0.66f, 0.28f, p.shirt);
    for (int side : {-1, 1}) {
        rlPushMatrix(); rlTranslatef(0.33f * side, 1.45f, 0);
        rlRotatef(-sw * side * 0.8f, 1, 0, 0);
        DrawCube({0, -0.12f, 0}, 0.17f, 0.26f, 0.19f, p.shirt);
        DrawCube({0, -0.42f, 0}, 0.12f, 0.38f, 0.13f, p.skin);
        rlPopMatrix();
    }
    DrawCube({0, 1.66f, 0}, 0.28f, 0.3f, 0.28f, p.skin);
    if (p.cop) {
        DrawCube({0, 1.86f, 0}, 0.32f, 0.12f, 0.32f, p.hair);
        DrawCube({0, 1.81f, 0.2f}, 0.3f, 0.03f, 0.14f, Hex(0x0f1a3d));
    } else {
        DrawCube({0, 1.85f, -0.02f}, 0.3f, 0.1f, 0.3f, p.hair);
        DrawCube({0, 1.7f, -0.14f}, 0.3f, 0.26f, 0.04f, p.hair);
    }
    if (p.chain) DrawCube({0, 1.38f, 0.145f}, 0.22f, 0.04f, 0.02f, C_GOLD);
    rlPopMatrix();
}
static void DrawCarBoxes(const Car& c) {
    rlPushMatrix();
    rlTranslatef(c.x, c.y, c.z);
    rlRotatef(c.a * RAD2DEG, 0, 1, 0);
    DrawCube({0, 0.6f, 0}, 2.0f, 0.62f, 4.4f, c.col);
    DrawCube({0, 1.2f, -0.25f}, 1.72f, 0.52f, 2.1f, Hex(0x22283d));
    DrawCube({0, 1.48f, -0.3f}, 1.76f, 0.07f, 1.8f, Mix(c.col, WHITE, 0.15f));
    for (int sx : {-1, 1})
        for (int sz : {-1, 1}) DrawCube({0.92f * sx, 0.34f, 1.35f * sz}, 0.28f, 0.62f, 0.66f, Hex(0x18161f));
    rlPopMatrix();
}
static void DrawCarLights(const Car& c) {
    rlPushMatrix();
    rlTranslatef(c.x, c.y, c.z);
    rlRotatef(c.a * RAD2DEG, 0, 1, 0);
    for (int sx : {-1, 1}) {
        DrawCube({0.65f * sx, 0.68f, 2.22f}, 0.4f, 0.16f, 0.05f, Hex(0xfff3b0));
        DrawCube({0.65f * sx, 0.68f, -2.22f}, 0.4f, 0.16f, 0.05f, Hex(0xff3355));
    }
    if (c.honk > 0) DrawSphere({0, 1.0f, 2.8f}, 0.25f + (0.5f - c.honk), C_GOLD);
    rlPopMatrix();
}
static void DrawPalm(const Palm& p) {
    float sway = std::sin(gTime * 1.3f + p.ph) * 0.25f;
    Vector3 base = {p.x, p.y, p.z}, top = {p.x + p.lx + sway, p.y + p.h, p.z + p.lz};
    DrawCylinderEx(base, top, 0.24f, 0.16f, 6, Hex(0x8a5a35));
    rlPushMatrix();
    rlTranslatef(top.x, top.y, top.z);
    for (int i = 0; i < 7; i++) {
        rlPushMatrix();
        rlRotatef(i * 360.f / 7.f + p.ph * 57.f, 0, 1, 0);
        rlRotatef(22.f + std::sin(gTime * 1.7f + p.ph + i) * 4.f, 1, 0, 0);
        DrawCube({0, 0, 1.4f}, 0.55f, 0.07f, 2.8f, i % 2 ? Hex(0x2f8f5b) : Hex(0x3fa86a));
        rlPopMatrix();
    }
    DrawSphere({0.15f, -0.2f, 0.1f}, 0.16f, Hex(0x6b4a2a));
    rlPopMatrix();
}
static void PedAnim(const Ped& p, const AnimModel& am, int& anim, int& frame, bool& lying) {
    lying = false;
    int want = -1; bool loop = true;
    if (p.task == Task::Down) {
        if (am.death >= 0) { want = am.death; loop = false; } else { lying = true; want = am.idle; }
    } else if (p.spd > 3.0f && am.run >= 0) want = am.run;
    else if (p.spd > 0.1f) want = am.walk >= 0 ? am.walk : am.run;
    else want = am.idle;
    anim = want; frame = 0;
    if (want < 0) return;
    int fc = am.anims[want].frameCount, f = (int)(p.animT * ANIM_FPS);
    frame = loop ? f % std::max(1, fc) : std::min(f, fc - 1);
}

static void DrawPedAny(Ped& p, bool modelsPass) {
    bool hasModel = p.model >= 0 && p.model < (int)gCharModels.size();
    if (hasModel != modelsPass) return;
    if (!hasModel) { DrawPersonBoxes(p); return; }
    AnimModel& am = gCharModels[p.model];
    int anim, frame; bool lying;
    PedAnim(p, am, anim, frame, lying);
    DrawAnimModel(am, p.x, p.y, p.z, p.a, p.tint, anim, frame, lying);
}
static void DrawCarAny(Car& c, bool modelsPass) {
    bool hasModel = c.model >= 0 && c.model < (int)gCarModels.size();
    if (hasModel != modelsPass) return;
    if (!hasModel) { DrawCarBoxes(c); return; }
    DrawAnimModel(gCarModels[c.model], c.x, c.y, c.z, c.a, c.target ? C_GOLD : WHITE, -1, 0, false);
}

// Draw everything that isn't glowing. modelsPass selects loaded models vs block-built objects.
static void DrawSolids(const Camera3D& cam, Pass pass, bool modelsPass) {
    float range = pass == PASS_DEPTH ? 70.0f : 130.0f;
    auto inRange = [&](float x, float z, float r) { return std::hypot(x - cam.target.x, z - cam.target.z) < r; };
    if (!modelsPass) {
        if (pass != PASS_DEPTH) {
            DrawPlane({128, -0.35f, 128}, {3000, 3000}, C_SEA);
            for (int i = 0; i < 40; i++) {
                float x = 58 * TS + 6 + (i % 8) * 9.f, z = (i / 8) * 55.f + std::fmod(gTime * 2 + i * 7, 55.f);
                DrawCube({x + std::sin(gTime + i) * 1.5f, -0.3f, z}, 1.6f, 0.02f, 0.18f, Hex(0xbfe9ec));
            }
        }
        for (const Palm& p : gPalms) if (inRange(p.x, p.z, range + 30)) DrawPalm(p);
    }
    for (int i = 0; i < (int)gCars.size(); i++) if (inRange(gCars[i].x, gCars[i].z, range + 10)) DrawCarAny(gCars[i], modelsPass);
    for (Ped& p : gPeds) if (inRange(p.x, p.z, pass == PASS_DEPTH ? 45.0f : 110.0f)) DrawPedAny(p, modelsPass);
    if (gInCar < 0) DrawPedAny(gPlayer, modelsPass);
}
// Neon, lights, pickups and mission markers
static void DrawGlowing() {
    for (const Car& c : gCars) DrawCarLights(c);
    for (const Pickup& q : gPickups) {
        rlPushMatrix();
        rlTranslatef(q.x, GroundY(q.x, q.z) + 0.6f + std::sin(gTime * 4 + q.ph) * 0.12f, q.z);
        rlRotatef(gTime * 120.f, 0, 1, 0);
        DrawCube({0, 0, 0}, 0.55f, 0.06f, 0.28f, Hex(0x4fcf85));
        rlPopMatrix();
    }
    Vector2 mk;
    if (MarkerActive(mk)) {
        float pulse = 0.5f + 0.5f * std::sin(gTime * 4);
        if (M.stage == MS::StealCar) {
            float bob = std::sin(gTime * 3) * 0.3f;
            DrawCylinderEx({mk.x, 4.4f + bob, mk.y}, {mk.x, 3.2f + bob, mk.y}, 0.55f, 0.0f, 8, C_GOLD);
        } else {
            float gy = GroundY(mk.x, mk.y);
            DrawCylinder({mk.x, gy + 0.05f, mk.y}, 1.4f + pulse * 0.2f, 1.4f + pulse * 0.2f, 0.12f, 24, C_PINK);
            DrawCylinder({mk.x, gy, mk.y}, 0.08f, 0.08f, 8.0f, 6, C_PINK);
        }
    }
}

static void RenderShadowMap(Vector3 focus) {
    Camera3D lc{};
    lc.target = focus;
    lc.position = {focus.x + gSun.x * 150, focus.y + gSun.y * 150, focus.z + gSun.z * 150};
    lc.up = {0, 1, 0}; lc.fovy = 160; lc.projection = CAMERA_ORTHOGRAPHIC;
    BeginTextureMode(rtShadow);
    ClearBackground(WHITE);
    BeginMode3D(lc);
    gLightVP = MatrixMultiply(rlGetMatrixModelview(), rlGetMatrixProjection());
    Flush(); rlDisableColorBlend();
    mStatic.materials[0].shader = shDepth; mEmis.materials[0].shader = shDepth;
    SetAllModelShaders(shDepth);
    BeginShaderMode(shDepth);
    DrawModel(mStatic, {0, 0, 0}, 1, WHITE);
    DrawModel(mEmis, {0, 0, 0}, 1, WHITE);
    Camera3D focusCam{}; focusCam.target = focus;
    DrawSolids(focusCam, PASS_DEPTH, false);
    Flush();
    DrawSolids(focusCam, PASS_DEPTH, true);
    EndShaderMode();
    Flush(); rlEnableColorBlend();
    EndMode3D();
    EndTextureMode();
}
static void DrawSky(int w, int h, const Camera3D& cam) {
    DrawRectangleGradientV(0, 0, w, h, SKY_TOP, SKY_HORIZON);
    Vector3 sunPos = {cam.position.x + gSun.x * 600, cam.position.y + 40, cam.position.z + gSun.z * 600};
    Vector3 f = {cam.target.x - cam.position.x, 0, cam.target.z - cam.position.z};
    if (f.x * gSun.x + f.z * gSun.z > 0) {
        Vector2 s = GetWorldToScreen(sunPos, cam);
        DrawCircleGradient((int)s.x, (int)s.y, h * 0.22f, Hex(0xffe08a), Fade(SKY_HORIZON, 0));
        DrawCircle((int)s.x, (int)s.y, h * 0.06f, Hex(0xfff1c2));
    }
}
static void RenderSceneFX(const Camera3D& cam) {
    RenderShadowMap({cam.target.x, 0, cam.target.z});
    BeginTextureMode(rtScene);
    ClearBackground(FOG);
    DrawSky(rtW, rtH, cam);
    BeginMode3D(cam);
    Flush(); rlDisableColorBlend();
    mStatic.materials[0].shader = shLight; mEmis.materials[0].shader = shLight;
    SetAllModelShaders(shLight);
    int slot = 13, on = 1;
    rlActiveTextureSlot(slot); rlEnableTexture(rtShadow.texture.id); rlActiveTextureSlot(0);
    SetShaderValue(shLight, L_shadowMap, &slot, SHADER_UNIFORM_INT);
    SetShaderValue(shLight, L_shadowsOn, &on, SHADER_UNIFORM_INT);
    SetShaderValueMatrix(shLight, L_lightVP, gLightVP);
    V3(shLight, L_view, cam.position);
    SetEmissive(false);
    BeginShaderMode(shLight);
    DrawModel(mStatic, {0, 0, 0}, 1, WHITE);
    ResetBatchUniforms(shLight);
    DrawSolids(cam, PASS_COLOR, false);
    Flush();
    DrawSolids(cam, PASS_COLOR, true);
    ResetBatchUniforms(shLight);
    SetEmissive(true);
    DrawModel(mEmis, {0, 0, 0}, 1, WHITE);
    ResetBatchUniforms(shLight);
    DrawGlowing();
    SetEmissive(false);
    EndShaderMode();
    Flush(); rlEnableColorBlend();
    rlActiveTextureSlot(slot); rlDisableTexture(); rlActiveTextureSlot(0);
    EndMode3D();
    EndTextureMode();

    int gw = rtGlowA.texture.width, gh = rtGlowA.texture.height;
    BeginTextureMode(rtGlowA);
    ClearBackground(BLACK);
    BeginShaderMode(shExtract);
    DrawTexturePro(rtScene.texture, {0, 0, (float)rtW, (float)-rtH}, {0, 0, (float)gw, (float)gh}, {0, 0}, 0, WHITE);
    EndShaderMode();
    EndTextureMode();
    for (int it = 0; it < 2; it++) {
        Vector2 dh = {1.0f / gw, 0}, dv = {0, 1.0f / gh};
        SetShaderValue(shBlur, B_dir, &dh, SHADER_UNIFORM_VEC2);
        BeginTextureMode(rtGlowB); BeginShaderMode(shBlur);
        DrawTextureRec(rtGlowA.texture, {0, 0, (float)gw, (float)-gh}, {0, 0}, WHITE);
        EndShaderMode(); EndTextureMode();
        SetShaderValue(shBlur, B_dir, &dv, SHADER_UNIFORM_VEC2);
        BeginTextureMode(rtGlowA); BeginShaderMode(shBlur);
        DrawTextureRec(rtGlowB.texture, {0, 0, (float)gw, (float)-gh}, {0, 0}, WHITE);
        EndShaderMode(); EndTextureMode();
    }
}
static void CompositeFX() {
    float amt = 1.6f;
    SetShaderValue(shComp, C_amt, &amt, SHADER_UNIFORM_FLOAT);
    SetShaderValueTexture(shComp, C_glow, rtGlowA.texture);
    BeginShaderMode(shComp);
    DrawTextureRec(rtScene.texture, {0, 0, (float)rtW, (float)-rtH}, {0, 0}, WHITE);
    EndShaderMode();
}
static void RenderScenePlain(const Camera3D& cam, int w, int h) {
    ClearBackground(FOG);
    DrawSky(w, h, cam);
    BeginMode3D(cam);
    mStatic.materials[0].shader = gDefShader; mEmis.materials[0].shader = gDefShader;
    SetAllModelShaders(gDefShader);
    DrawModel(mStatic, {0, 0, 0}, 1, WHITE);
    DrawModel(mEmis, {0, 0, 0}, 1, WHITE);
    DrawSolids(cam, PASS_PLAIN, false);
    DrawSolids(cam, PASS_PLAIN, true);
    DrawGlowing();
    EndMode3D();
}

static Camera3D MakeCamera(float dt, bool orbit) {
    Camera3D cam{};
    cam.up = {0, 1, 0}; cam.fovy = 60; cam.projection = CAMERA_PERSPECTIVE;
    float px, pz; PlayerPos(px, pz);
    float yaw = gCamYaw, pitch = gCamPitch, dist = gInCar >= 0 ? 9.5f : 5.5f;
    Vector3 tgt = {px, (gInCar >= 0 ? 1.8f : gPlayer.y + 1.5f), pz};
    if (orbit) { yaw = (float)GetTime() * 0.12f; pitch = 0.35f; dist = 26; tgt.y = 4; }
    float k = std::min(1.0f, dt * 10);
    gCamTarget.x += (tgt.x - gCamTarget.x) * k;
    gCamTarget.y += (tgt.y - gCamTarget.y) * k;
    gCamTarget.z += (tgt.z - gCamTarget.z) * k;
    Vector3 t = gCamTarget;
    if (gShake > 0.01f) { t.x += Rand(-gShake, gShake); t.y += Rand(-gShake, gShake); }
    Vector3 want = {t.x - std::sin(yaw) * dist * std::cos(pitch), t.y + dist * std::sin(pitch), t.z - std::cos(yaw) * dist * std::cos(pitch)};
    Vector3 pos = want;
    for (int i = 1; i <= 24; i++) {
        float f = i / 24.f;
        Vector3 q = {t.x + (want.x - t.x) * f, t.y + (want.y - t.y) * f, t.z + (want.z - t.z) * f};
        int tx = (int)std::floor(q.x / TS), tz = (int)std::floor(q.z / TS);
        if (tx >= 0 && tz >= 0 && tx < MW && tz < MH && gMap[tz * MW + tx] == BLD && q.y < gRoofH[tz * MW + tx] + 0.6f) {
            float f2 = (i - 1) / 24.f;
            pos = {t.x + (want.x - t.x) * f2, t.y + (want.y - t.y) * f2, t.z + (want.z - t.z) * f2};
            break;
        }
    }
    cam.position = pos; cam.target = t;
    return cam;
}

// ---------------------------------------------------------------- HUD
static void Tri(Vector2 a, Vector2 b, Vector2 c, Color col) { DrawTriangle(a, b, c, col); DrawTriangle(a, c, b, col); }
static void DrawStar(float cx, float cy, float r, Color fill) {
    Vector2 pts[10];
    for (int i = 0; i < 10; i++) {
        float ang = -PI / 2 + i * PI / 5, rr = (i % 2) ? r * 0.45f : r;
        pts[i] = {cx + std::cos(ang) * rr, cy + std::sin(ang) * rr};
    }
    for (int i = 0; i < 10; i++) Tri({cx, cy}, pts[i], pts[(i + 1) % 10], fill);
}
static void Outlined(const char* t, int x, int y, int size, Color col) {
    for (int dx = -2; dx <= 2; dx += 2)
        for (int dy = -2; dy <= 2; dy += 2) DrawText(t, x + dx, y + dy, size, Hex(0x1a1224));
    DrawText(t, x, y, size, col);
}
static void Centered(const char* t, int cx, int y, int size, Color col) { Outlined(t, cx - MeasureText(t, size) / 2, y, size, col); }

static Texture2D gRadarTex;
static void BuildRadar() {
    Image img = GenImageColor(MW * 4, MH * 4, C_SEA);
    for (int y = 0; y < MH; y++)
        for (int x = 0; x < MW; x++) {
            Color c;
            switch (gMap[y * MW + x]) {
                case ROAD: c = Hex(0x3d3850); break;
                case WALK: c = Hex(0xcfc3b3); break;
                case BLD: c = Hex(0x9a8fb0); break;
                case PARK: c = Hex(0x6fae5a); break;
                case LOT: c = Hex(0x4d475e); break;
                case SAND: c = Hex(0xe9cf96); break;
                default: c = Hex(0x2a8fae); break;
            }
            ImageDrawRectangle(&img, x * 4, y * 4, 4, 4, c);
        }
    gRadarTex = LoadTextureFromImage(img);
    UnloadImage(img);
}
static void DrawRadar(int sh) {
    const int R = 190, X = 24, Y = sh - R - 24;
    const float span = 160;
    float px, pz; PlayerPos(px, pz);
    float cx = px / TS * 4, cz = pz / TS * 4;
    float sx = Clampf(cx - span / 2, 0, MW * 4 - span), sz = Clampf(cz - span / 2, 0, MH * 4 - span);
    DrawRectangle(X - 5, Y - 5, R + 10, R + 10, Fade(Hex(0x1a1224), 0.8f));
    DrawTexturePro(gRadarTex, {sx, sz, span, span}, {(float)X, (float)Y, (float)R, (float)R}, {0, 0}, 0, WHITE);
    float sc = R / span;
    auto toR = [&](float wx, float wz) { return Vector2{X + (wx / TS * 4 - sx) * sc, Y + (wz / TS * 4 - sz) * sc}; };
    BeginScissorMode(X, Y, R, R);
    for (auto& q : gPickups) { Vector2 v = toR(q.x, q.z); DrawCircle((int)v.x, (int)v.y, 3, Hex(0x4fcf85)); }
    bool flash = std::fmod(gTime, 0.5f) < 0.25f;
    for (auto& p : gPeds)
        if (p.cop && (p.task == Task::Chase || gWanted > 0)) {
            Vector2 v = toR(p.x, p.z);
            DrawCircle((int)v.x, (int)v.y, 4, flash ? Hex(0x3b6cff) : Hex(0xff3355));
        }
    Vector2 me = toR(px, pz);
    float a = gInCar >= 0 ? gCars[gInCar].a : gPlayer.a, s = std::sin(a), c = std::cos(a);
    Vector2 tip = {me.x + s * 9, me.y + c * 9}, l = {me.x - s * 6 + c * 6, me.y - c * 6 - s * 6}, r = {me.x - s * 6 - c * 6, me.y - c * 6 + s * 6};
    Tri(tip, l, r, WHITE);
    EndScissorMode();
    Vector2 mk;
    if (MarkerActive(mk)) {   // mission blip, pinned to the radar edge when off-screen
        Vector2 v = toR(mk.x, mk.y);
        v.x = Clampf(v.x, (float)X + 6, (float)X + R - 6);
        v.y = Clampf(v.y, (float)Y + 6, (float)Y + R - 6);
        DrawCircle((int)v.x, (int)v.y, 7, Hex(0x1a1224));
        DrawCircle((int)v.x, (int)v.y, 5, M.stage == MS::StealCar ? C_GOLD : C_PINK);
    }
    DrawRectangleLinesEx({(float)X - 5, (float)Y - 5, (float)R + 10, (float)R + 10}, 3, C_PINK);
    if (M.objective[0] && M.stage != MS::Talking && M.stage != MS::Ringing && M.stage != MS::Wait)
        Outlined(M.objective, X - 4, Y - 34, 18, Hex(0xb9f0dc));
}
static void DrawHUD(const Camera3D& cam) {
    int sw = GetScreenWidth(), sh = GetScreenHeight();
    if (gShowAI) {
        auto label = [&](Vector3 w, const char* txt, Color col) {
            float dx = w.x - cam.position.x, dz = w.z - cam.position.z;
            float fx = cam.target.x - cam.position.x, fz = cam.target.z - cam.position.z;
            if (dx * fx + dz * fz <= 0 || std::hypot(dx, dz) > 45) return;
            Vector2 s = GetWorldToScreen(w, cam);
            int tw = MeasureText(txt, 14);
            DrawRectangle((int)s.x - tw / 2 - 6, (int)s.y - 10, tw + 12, 20, Fade(Hex(0x241b33), 0.85f));
            DrawText(txt, (int)s.x - tw / 2, (int)s.y - 7, 14, col);
        };
        for (auto& p : gPeds) {
            const char* t = "";
            Color col = Hex(0xefe6d6);
            switch (p.task) {
                case Task::Wander: t = "wander"; break;
                case Task::Idle: t = p.label; col = Hex(0xb9f0dc); break;
                case Task::Flee: t = "flee"; col = Hex(0xffb4c8); break;
                case Task::Down: t = "knocked down"; col = C_GOLD; break;
                case Task::Chase: t = "chase suspect"; col = Hex(0xa9ccff); break;
            }
            label({p.x, p.y + 2.3f, p.z}, p.cop ? TextFormat("cop: %s", t) : t, col);
        }
        for (const Car& c : gCars) {
            if (!c.ai) continue;
            label({c.x, c.y + 2.4f, c.z}, (c.why && c.speed < 1) ? TextFormat("traffic: waiting for %s", c.why) : "traffic: follow lane", Hex(0xffe3b3));
        }
    }
    int h = (int)gClock / 60, m = (int)gClock % 60;
    const char* clk = TextFormat("%02d:%02d", h, m);
    Outlined(clk, sw - MeasureText(clk, 34) - 28, 20, 34, WHITE);
    const char* cash = TextFormat("$%08d", gCash);
    Outlined(cash, sw - MeasureText(cash, 34) - 28, 60, 34, Hex(0x8ff0bf));
    bool blink = gStarFlash > 0 && std::fmod(gStarFlash, 0.3f) < 0.15f;
    for (int i = 0; i < 5; i++) {
        float cx = sw - 40 - (4 - i) * 34.f, cy = 122;
        DrawStar(cx, cy, 16, Hex(0x1a1224));
        DrawStar(cx, cy, 13, i < gWanted ? (blink ? WHITE : C_GOLD) : Fade(WHITE, 0.18f));
    }
    DrawRadar(sh);
    if (gToastT > 0) Centered(gToast, sw / 2, 40, 24, WHITE);
    if (gMode == Mode::Play && gInCar < 0 && NearestTalker() >= 0) Centered("Enter  talk", sw / 2, sh - 60, 20, C_GOLD);
    else if (gInCar < 0 && NearestCar() >= 0) Centered(gCars[NearestCar()].ai ? "F  carjack" : "F  get in", sw / 2, sh - 60, 20, Hex(0xb9f0dc));

    // missions: phone, subtitles, objective, banner
    if (M.stage == MS::Ringing && std::fmod(gTime, 0.6f) < 0.4f) Centered("Incoming call: Marco", sw / 2, 90, 26, C_GOLD);
    if (M.stage == MS::Talking && M.line < (int)M.lines->size()) {
        const Line& L = (*M.lines)[M.line];
        const char* who = TextFormat("%s: ", L.who);
        int w1 = MeasureText(who, 24), w2 = MeasureText(L.text, 24), x = sw / 2 - (w1 + w2) / 2, y = sh - 110;
        Outlined(who, x, y, 24, L.who[0] == 'N' ? C_TEAL : C_PINK);
        Outlined(L.text, x + w1, y, 24, WHITE);
    } else if (M.objT > 0 && M.objective[0]) {
        Centered(M.objective, sw / 2, sh - 110, 24, Hex(0xb9f0dc));
    }
    if (M.bannerT > 0) {
        Centered(M.banner, sw / 2, sh / 2 - 90, 60, C_GOLD);
        Centered(M.bannerSub, sw / 2, sh / 2 - 22, 36, WHITE);
    }
    DrawText(gFX ? "G: simple graphics" : "G: fancy graphics", sw - 190, sh - 30, 16, Fade(WHITE, 0.55f));
}

// Word-wraps text into a box of width w; returns the y just below the last line.
// With draw = false it only measures.
static int DrawWrapped(const char* text, int x, int y, int w, int size, Color col, bool draw = true) {
    std::string line, word;
    auto flush = [&]() { if (draw) DrawText(line.c_str(), x, y, size, col); y += size + 6; line.clear(); };
    for (const char* c = text;; c++) {
        if (*c == ' ' || *c == 0) {
            std::string trial = line.empty() ? word : line + " " + word;
            if (!line.empty() && MeasureText(trial.c_str(), size) > w) { flush(); line = word; }
            else line = trial;
            word.clear();
            if (*c == 0) break;
        } else word += *c;
    }
    if (!line.empty()) flush();
    return y;
}
static int QuizBody(int x, int y, int w, bool draw) {
    const QuizQ& qq = QUIZ[Q.q];
    int inner = w - 72;
    auto text = [&](const char* t, int tx, int ty, int size, Color col) { if (draw) DrawText(t, tx, ty, size, col); };
    text("AWS CERTIFIED AI PRACTITIONER", x + 36, y + 30, 22, C_TEAL);
    text(qq.domain, x + 36, y + 58, 18, Fade(WHITE, 0.6f));
    const char* score = TextFormat("Score %d / %d", Q.right, Q.asked);
    text(score, x + w - 36 - MeasureText(score, 20), y + 30, 20, Hex(0x8ff0bf));
    int cy = DrawWrapped(qq.q, x + 36, y + 96, inner, 24, WHITE, draw) + 14;
    const char* letters = "ABCD";
    for (int i = 0; i < 4; i++) {
        int oi = Q.order[i];
        Color col = Hex(0xb9f0dc);
        if (Q.picked >= 0) {
            if (oi == qq.answer) col = Hex(0x4fcf85);
            else if (i == Q.picked) col = C_PINK;
            else col = Fade(WHITE, 0.4f);
        }
        text(TextFormat("%c", letters[i]), x + 36, cy, 22, col);
        cy = DrawWrapped(qq.opts[oi], x + 70, cy, inner - 34, 22, col, draw) + 8;
    }
    cy += 10;
    if (Q.picked < 0) {
        text("Press A-D or 1-4 to answer.   Esc: walk away", x + 36, cy, 18, C_GOLD);
        cy += 18;
    } else {
        bool ok = Q.order[Q.picked] == qq.answer;
        text(ok ? "Correct!  +$100" : "Not quite.", x + 36, cy, 26, ok ? Hex(0x4fcf85) : C_PINK);
        cy = DrawWrapped(qq.why, x + 36, cy + 36, inner, 20, Hex(0xe8dcf0), draw) + 12;
        if (std::fmod(GetTime(), 1.0) < 0.6) text("Press ENTER to continue", x + 36, cy, 20, C_GOLD);
        cy += 20;
    }
    return cy + 30 - y;   // panel height
}
static void DrawQuiz(int sw, int sh) {
    DrawRectangle(0, 0, sw, sh, Fade(Hex(0x241b33), 0.45f));
    int w = std::min(820, sw - 60), x = sw / 2 - w / 2;
    int h = QuizBody(x, 0, w, false), y = std::max(20, sh / 2 - h / 2);
    DrawRectangle(x, y, w, h, Fade(Hex(0x1f1730), 0.94f));
    DrawRectangle(x, y, w, 10, C_TEAL);
    QuizBody(x, y, w, true);
}

static void DrawTitle(int sw, int sh) {
    DrawRectangle(0, 0, sw, sh, Fade(Hex(0x241b33), 0.45f));
    int w = std::min(640, sw - 60), x = sw / 2 - w / 2, y = sh / 2 - 205;
    DrawRectangle(x, y, w, 410, Fade(Hex(0x1f1730), 0.92f));
    DrawRectangle(x, y, w, 10, C_PINK);
    DrawText("CORAL BAY", x + 36, y + 36, 64, C_PINK);
    DrawText("1986", x + 36 + MeasureText("CORAL BAY", 64) + 16, y + 66, 28, C_TEAL);
    const char* rows[] = {"WASD  move / drive        Mouse  look", "Shift  sprint",
                          "F  get in, get out, carjack    Enter  talk (AWS AI quiz)", "Space  handbrake   H  horn   R  radio",
                          "Tab  see what every pedestrian is thinking", "G  switch fancy / simple graphics", "Esc  pause"};
    for (int i = 0; i < 7; i++) DrawText(rows[i], x + 36, y + 130 + i * 26, 18, Hex(0xb9f0dc));
    const char* models = gCharModels.empty() ? "Block models (add models to assets/ for full 3D)" : TextFormat("%d character and %d car models loaded", (int)gCharModels.size(), (int)gCarModels.size());
    DrawText(models, x + 36, y + 320, 16, Fade(WHITE, 0.6f));
    if (std::fmod(GetTime(), 1.0) < 0.6) DrawText("Press ENTER to hit the streets", x + 36, y + 356, 24, C_GOLD);
}

// ---------------------------------------------------------------- main
// One frame of the game. The browser build calls this from its animation loop.
static void Frame() {
    float dt = std::min(GetFrameTime(), 0.05f);
    if (gToastT > 0) gToastT -= dt;
    if (gStarFlash > 0) gStarFlash -= dt;
    if (IsKeyPressed(KEY_G) && gFXok) gFX = !gFX;

    switch (gMode) {
        case Mode::Title:
            gTime += dt;
            for (auto& c : gCars) if (c.ai) UpdateTraffic(c, dt);
            for (size_t i = 0; i < gPeds.size(); i++) UpdatePed(gPeds[i], dt);
            if (IsKeyPressed(KEY_ENTER)) { gMode = Mode::Play; DisableCursor(); gCamYaw = PI * 1.5f; }
            break;
        case Mode::Play:
            UpdatePlay(dt);
            if (IsKeyPressed(KEY_ESCAPE)) { gMode = Mode::Pause; EnableCursor(); }
            break;
        case Mode::Pause:
            gSynth.engine = 0;
            if (IsKeyPressed(KEY_ESCAPE) || IsKeyPressed(KEY_ENTER) || IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) { gMode = Mode::Play; DisableCursor(); }
            break;
        case Mode::Quiz:
            gSynth.engine = 0;
            UpdateQuiz();
            break;
        case Mode::Busted:
            gTime += dt;
            gBustT -= dt;
            if (gBustT <= 0) { Respawn(); gMode = Mode::Play; }
            break;
    }
    gSynth.Update();

    Camera3D cam = MakeCamera(dt, gMode == Mode::Title);
    int sw = GetScreenWidth(), sh = GetScreenHeight();
    if (gFX) { EnsureTargets(sw, sh); RenderSceneFX(cam); }
    BeginDrawing();
    if (gFX) CompositeFX(); else RenderScenePlain(cam, sw, sh);
    if (gMode == Mode::Title) DrawTitle(sw, sh);
    else {
        DrawHUD(cam);
        if (gMode == Mode::Pause) {
            DrawRectangle(0, 0, sw, sh, Fade(Hex(0x241b33), 0.6f));
            Centered("Paused", sw / 2, sh / 2 - 50, 60, C_PINK);
            Centered("Click or press Esc to resume. Close the window to quit.", sw / 2, sh / 2 + 24, 20, WHITE);
        }
        if (gMode == Mode::Quiz) DrawQuiz(sw, sh);
        if (gMode == Mode::Busted) {
            DrawRectangle(0, 0, sw, sh, Fade(Hex(0x241b33), 0.35f));
            Centered("BUSTED", sw / 2, sh / 2 - 60, 110, C_PINK);
        }
    }
    EndDrawing();
}

int main() {
    SetConfigFlags(FLAG_MSAA_4X_HINT | FLAG_WINDOW_RESIZABLE | FLAG_VSYNC_HINT);
    InitWindow(1280, 720, "Coral Bay");
    SetExitKey(KEY_NULL);
    SetTargetFPS(60);
    gSynth.Init();
    LoadAssets();
    InitWorld();
    InitGraphics();
    BuildRadar();

#if defined(PLATFORM_WEB)
    emscripten_set_main_loop(Frame, 0, 1);   // never returns
#else
    while (!WindowShouldClose()) Frame();
#endif
    UnloadTexture(gRadarTex);
    SetAllModelShaders(gDefShader);
    mStatic.materials[0].shader = gDefShader;
    mEmis.materials[0].shader = gDefShader;
    for (auto& a : gCharModels) { if (a.anims) UnloadModelAnimations(a.anims, a.animCount); UnloadModel(a.model); }
    for (auto& a : gCarModels) UnloadModel(a.model);
    UnloadModel(mStatic);
    UnloadModel(mEmis);
    if (gFXok) {
        UnloadShader(shLight); UnloadShader(shDepth); UnloadShader(shExtract); UnloadShader(shBlur); UnloadShader(shComp);
        UnloadRenderTexture(rtShadow);
        if (rtW) { UnloadRenderTexture(rtScene); UnloadRenderTexture(rtGlowA); UnloadRenderTexture(rtGlowB); }
    }
    gSynth.Close();
    CloseWindow();
    return 0;
}
