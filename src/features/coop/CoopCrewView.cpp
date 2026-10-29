#include "Global.h"
#include "features/coop/CoopCrew.h"
#include "features/coop/CoopFeature.h"
#include "features/coop/CoopRender3D.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <map>
#include <random>

// Crew mode, client side: a full screen first-person view of one crew member of
// the host's ship, drawn from the layout and snapshots the host sends.
// Controls: WASD walk, mouse look (captured), E door, Q/R other crew member,
// Tab big map, Esc release the mouse, F10 leave.

namespace
{

using Clock = std::chrono::steady_clock;
using namespace Coop::Crew;
using Coop::R3D::Vec3;
using Coop::R3D::V;

constexpr float PI = 3.14159265f;
constexpr float WALL_H = 46.f;
constexpr float DOOR_H = 33.f;
constexpr float EYE_H = 23.f;
constexpr float CONSOLE_H = 15.f;
constexpr float FOG_DIST = 520.f;
constexpr auto INPUT_INTERVAL = std::chrono::milliseconds(33);
constexpr float MOUSE_SENSITIVITY = 0.0032f;
constexpr float TURN_SPEED = 2.4f; // radians per second with the arrow keys

// Textures

struct Textures
{
    bool loaded = false;
    GL_Texture *wall, *wallOuter, *floor, *ceiling, *door, *console, *light, *glow, *white;
    GL_Texture *fire, *smoke, *breach;
    std::map<int, GL_Texture*> roomFloors; // by system type
};
Textures tex;

const char *SystemImage(int system)
{
    switch (system)
    {
    case 0: return "shields";
    case 1: return "engines";
    case 2: return "oxygen";
    case 3: return "weapons";
    case 4: return "drones";
    case 5: return "medbay";
    case 6: return "pilot";
    case 7: return "sensors";
    case 8: return "doors";
    case 10: return "cloaking";
    case 11: return "artillery";
    case 12: return "battery";
    case 14: return "mind";
    case 15: return "hacking";
    default: return nullptr;
    }
}

const char *SystemName(int system)
{
    switch (system)
    {
    case 0: return "Shields";
    case 1: return "Engines";
    case 2: return "Oxygen";
    case 3: return "Weapons";
    case 4: return "Drone Control";
    case 5: return "Medbay";
    case 6: return "Piloting";
    case 7: return "Sensors";
    case 8: return "Door Control";
    case 9: return "Teleporter";
    case 10: return "Cloaking";
    case 11: return "Artillery";
    case 12: return "Backup Battery";
    case 13: return "Clone Bay";
    case 14: return "Mind Control";
    case 15: return "Hacking";
    default: return nullptr;
    }
}

GL_Texture *Image(const std::string& path)
{
    return G_->GetResources()->GetImageId(path);
}

void LoadTextures()
{
    if (tex.loaded) return;
    tex.loaded = true;
    tex.wall = Image("coop/wall.png");
    tex.wallOuter = Image("coop/wall_outer.png");
    tex.floor = Image("coop/floor.png");
    tex.ceiling = Image("coop/ceiling.png");
    tex.door = Image("coop/door.png");
    tex.console = Image("coop/console.png");
    tex.light = Image("coop/light.png");
    tex.glow = Image("coop/glow.png");
    tex.white = Image("coop/white.png");
    tex.fire = Image("effects/fire_L1_strip8.png");
    tex.smoke = Image("effects/fire_smoke.png");
    tex.breach = Image("effects/breach.png");
    for (int system = 0; system < 16; ++system)
    {
        const char *name = SystemImage(system);
        if (name) tex.roomFloors[system] = Image(std::string("ship/interior/room_") + name + ".png");
    }
}

// State from the host

struct CrewState
{
    uint32_t id = 0;
    float x = 0.f, y = 0.f;    // latest from the host
    float sx = 0.f, sy = 0.f;  // smoothed for drawing
    float facing = 0.f;
    float walkPhase = 0.f;
    float speed = 0.f;
    int health = 100;
    int flags = 0;
    int owner = -1;
    std::string species, name;
    bool seen = false;
    Clock::time_point lastSeen;
};

struct SystemState
{
    int room, type, health, maxHealth, power;
};

struct Snapshot
{
    bool valid = false;
    uint32_t layoutId = 0;
    int hull = 0, maxHull = 0, shields = 0, flags = 0;
    std::vector<float> doorOpen;
    std::vector<std::pair<int, int>> fires, breaches;
    std::vector<int> oxygen; // per layout room
    std::vector<SystemState> systems;
    Clock::time_point received;
};

bool active = false;
Layout layout;
Snapshot snap;
std::map<uint32_t, CrewState> crew;
uint32_t myCrew = 0;

// Local view state
float yaw = 0.f, pitch = 0.f;
float camX = 0.f, camY = 0.f;
bool camPlaced = false;
std::vector<float> doorAnim;
bool keyDown[512] = {};
uint8_t doorPresses = 0;
bool mouseCaptured = false;
bool skipMouseDelta = true;
bool bigMap = false;
Clock::time_point lastInput, lastFrame;
float shake = 0.f, damageFlash = 0.f, jumpFade = 0.f;
int lastHull = -1, lastMyHealth = -1;
float timeNow = 0.f;
std::mt19937 rng(7);

struct Particle
{
    float x, y, z, vx, vy, vz, life, maxLife;
    GL_Color color;
    float size;
};
std::vector<Particle> particles;

// Geometry built from the layout: surfaces with a room each, lit per frame

enum SurfaceKind
{
    SURF_FLOOR,
    SURF_CEILING,
    SURF_WALL,
    SURF_CONSOLE,
    SURF_CONSOLE_SCREEN,
    SURF_LIGHT
};

struct Surface
{
    GL_Texture *texture;
    Vec3 p[4];
    float u[4], v[4];
    Vec3 normal;
    int room;
    SurfaceKind kind;
};
std::vector<Surface> surfaces;
uint32_t builtLayout = 0;

bool Pressed(int key)
{
    return key >= 0 && key < 512 && keyDown[key];
}

CrewState *MyCrew()
{
    auto it = crew.find(myCrew);
    return it == crew.end() ? nullptr : &it->second;
}

void AddSurface(GL_Texture *t, Vec3 a, Vec3 b, Vec3 c, Vec3 d, float u0, float v0, float u1, float v1, Vec3 normal,
                int room, SurfaceKind kind)
{
    Surface s;
    s.texture = t;
    s.p[0] = a;
    s.p[1] = b;
    s.p[2] = c;
    s.p[3] = d;
    s.u[0] = u0; s.v[0] = v0;
    s.u[1] = u1; s.v[1] = v0;
    s.u[2] = u1; s.v[2] = v1;
    s.u[3] = u0; s.v[3] = v1;
    s.normal = normal;
    s.room = room;
    s.kind = kind;
    surfaces.push_back(s);
}

// A rectangle split into nx * ny parts, so the affine texture mapping stays straight
void AddGrid(GL_Texture *t, Vec3 origin, Vec3 du, Vec3 dv, int nx, int ny, float u0, float v0, float u1, float v1,
             Vec3 normal, int room, SurfaceKind kind)
{
    for (int j = 0; j < ny; ++j)
    {
        for (int i = 0; i < nx; ++i)
        {
            float a = (float)i / nx, b = (float)(i + 1) / nx, c = (float)j / ny, d = (float)(j + 1) / ny;
            auto at = [&](float s, float r) {
                return Vec3{origin.x + du.x * s + dv.x * r, origin.y + du.y * s + dv.y * r, origin.z + du.z * s + dv.z * r};
            };
            AddSurface(t, at(a, c), at(b, c), at(b, d), at(a, d), u0 + (u1 - u0) * a, v0 + (v1 - v0) * c,
                       u0 + (u1 - u0) * b, v0 + (v1 - v0) * d, normal, room, kind);
        }
    }
}

float TileLeft(int tx)
{
    return layout.originX + tx * (float)TILE;
}
float TileTop(int ty)
{
    return layout.originY + ty * (float)TILE;
}

// The wall on one edge of a tile, seen from inside the tile
void AddWall(int tx, int ty, int dir, int room, bool outer, float zFrom, float zTo)
{
    float x0 = TileLeft(tx), y0 = TileTop(ty), x1 = x0 + TILE, y1 = y0 + TILE;
    Vec3 a, b, normal;
    const float inset = 0.4f;
    switch (dir)
    {
    case DIR_X_PLUS: a = {x1 - inset, y0, 0}; b = {x1 - inset, y1, 0}; normal = {-1, 0, 0}; break;
    case DIR_X_MINUS: a = {x0 + inset, y1, 0}; b = {x0 + inset, y0, 0}; normal = {1, 0, 0}; break;
    case DIR_Y_PLUS: a = {x1, y1 - inset, 0}; b = {x0, y1 - inset, 0}; normal = {0, -1, 0}; break;
    default: a = {x0, y0 + inset, 0}; b = {x1, y0 + inset, 0}; normal = {0, 1, 0}; break;
    }
    Vec3 origin = {a.x, a.y, zTo};
    Vec3 du = {b.x - a.x, b.y - a.y, 0};
    Vec3 dv = {0, 0, zFrom - zTo};
    float vTop = 1.f - zTo / WALL_H, vBottom = 1.f - zFrom / WALL_H;
    int rows = std::max(1, (int)std::ceil((zTo - zFrom) / 16.f));
    AddGrid(outer ? tex.wallOuter : tex.wall, origin, du, dv, 2, rows, 0.f, vTop, 1.f, vBottom, normal, room, SURF_WALL);
}

void AddConsole(const LayoutRoom& room, int roomIndex)
{
    if (room.primarySlot < 0 || room.system < 0) return;
    int wTiles = room.w / TILE;
    if (wTiles <= 0) return;
    int tx = (room.x - layout.originX) / TILE + room.primarySlot % wTiles;
    int ty = (room.y - layout.originY) / TILE + room.primarySlot / wTiles;

    // FTL's console direction, or else any wall of that tile
    int dir = -1;
    const int preferred[4] = {room.primaryDir % 4, 0, 1, 2};
    for (int i = 0; i < 4 && dir < 0; ++i)
    {
        int d = i == 0 ? preferred[0] : (preferred[0] + i) % 4;
        if (layout.RoomAt(tx + DirX(d), ty + DirY(d)) != roomIndex) dir = d;
    }
    if (dir < 0) return;

    float cx = TileLeft(tx) + TILE / 2.f + DirX(dir) * (TILE / 2.f - 6.f);
    float cy = TileTop(ty) + TILE / 2.f + DirY(dir) * (TILE / 2.f - 6.f);
    float hx = DirX(dir) != 0 ? 5.f : 11.f;
    float hy = DirY(dir) != 0 ? 5.f : 11.f;
    Vec3 n = {(float)-DirX(dir), (float)-DirY(dir), 0.f};

    // Box: front face (towards the room), sides, top
    float fx = cx - DirX(dir) * 5.f, fy = cy - DirY(dir) * 5.f;
    Vec3 left = DirX(dir) != 0 ? Vec3{0, (float)DirX(dir), 0} : Vec3{(float)-DirY(dir), 0, 0};
    Vec3 fl = {fx - left.x * 11.f, fy - left.y * 11.f, 0}, fr = {fx + left.x * 11.f, fy + left.y * 11.f, 0};
    AddSurface(tex.wall, {fl.x, fl.y, CONSOLE_H - 5.f}, {fr.x, fr.y, CONSOLE_H - 5.f}, {fr.x, fr.y, 0}, {fl.x, fl.y, 0},
               0.1f, 0.6f, 0.9f, 1.f, n, roomIndex, SURF_CONSOLE);
    AddSurface(tex.wall, {cx - hx, cy - hy, CONSOLE_H}, {cx + hx, cy - hy, CONSOLE_H}, {cx + hx, cy + hy, CONSOLE_H},
               {cx - hx, cy + hy, CONSOLE_H}, 0.2f, 0.85f, 0.8f, 0.95f, {0, 0, 1}, roomIndex, SURF_CONSOLE);
    // Slanted screen on top of the front
    Vec3 back = {cx + DirX(dir) * 4.f, cy + DirY(dir) * 4.f, 0};
    Vec3 bl = {back.x - left.x * 10.f, back.y - left.y * 10.f, 0}, br = {back.x + left.x * 10.f, back.y + left.y * 10.f, 0};
    AddSurface(tex.console, {bl.x, bl.y, CONSOLE_H + 9.f}, {br.x, br.y, CONSOLE_H + 9.f}, {fr.x, fr.y, CONSOLE_H - 4.f},
               {fl.x, fl.y, CONSOLE_H - 4.f}, 0.f, 0.f, 1.f, 1.f, n, roomIndex, SURF_CONSOLE_SCREEN);
}

void BuildGeometry()
{
    surfaces.clear();
    builtLayout = layout.id;
    doorAnim.assign(layout.doors.size(), 0.f);

    for (size_t r = 0; r < layout.rooms.size(); ++r)
    {
        const LayoutRoom& room = layout.rooms[r];
        int roomIndex = (int)r;
        int rx = (room.x - layout.originX) / TILE, ry = (room.y - layout.originY) / TILE;
        int rw = room.w / TILE, rh = room.h / TILE;

        // System rooms show FTL's own interior art on the floor
        GL_Texture *art = nullptr;
        auto it = tex.roomFloors.find(room.system);
        if (it != tex.roomFloors.end()) art = it->second;

        for (int ty = ry; ty < ry + rh; ++ty)
        {
            for (int tx = rx; tx < rx + rw; ++tx)
            {
                float x0 = TileLeft(tx), y0 = TileTop(ty);
                if (art)
                {
                    float u0 = (float)(tx - rx) / rw, u1 = (float)(tx - rx + 1) / rw;
                    float v0 = (float)(ty - ry) / rh, v1 = (float)(ty - ry + 1) / rh;
                    AddGrid(art, {x0, y0, 0}, {TILE, 0, 0}, {0, TILE, 0}, 2, 2, u0, v0, u1, v1, {0, 0, 1}, roomIndex, SURF_FLOOR);
                }
                else
                {
                    AddGrid(tex.floor, {x0, y0, 0}, {TILE, 0, 0}, {0, TILE, 0}, 2, 2, 0, 0, 1, 1, {0, 0, 1}, roomIndex, SURF_FLOOR);
                }
                AddGrid(tex.ceiling, {x0, y0 + TILE, WALL_H}, {TILE, 0, 0}, {0, -TILE, 0}, 2, 2, 0, 0, 1, 1, {0, 0, -1}, roomIndex, SURF_CEILING);

                for (int dir = 0; dir < 4; ++dir)
                {
                    int neighbour = layout.RoomAt(tx + DirX(dir), ty + DirY(dir));
                    if (neighbour == roomIndex) continue;
                    if (layout.DoorAt(tx, ty, dir) >= 0)
                    {
                        AddWall(tx, ty, dir, roomIndex, neighbour < 0, DOOR_H, WALL_H); // lintel
                        continue;
                    }
                    AddWall(tx, ty, dir, roomIndex, neighbour < 0, 0.f, WALL_H);
                }
            }
        }

        // Ceiling light in the middle of the room
        float cx = room.x + room.w / 2.f, cy = room.y + room.h / 2.f;
        AddSurface(tex.light, {cx - 14, cy + 7, WALL_H - 0.6f}, {cx + 14, cy + 7, WALL_H - 0.6f},
                   {cx + 14, cy - 7, WALL_H - 0.6f}, {cx - 14, cy - 7, WALL_H - 0.6f}, 0, 0, 1, 1, {0, 0, -1}, roomIndex, SURF_LIGHT);

        AddConsole(room, roomIndex);
    }
}

// Lighting

struct Light
{
    Vec3 pos;
    GL_Color color;
    float radius;
    int room; // -1 lights everything
};
std::vector<Light> lights;

const SystemState *SystemInRoom(int roomIndex)
{
    if (roomIndex < 0) return nullptr;
    int roomId = layout.rooms[roomIndex].id;
    for (const SystemState& s : snap.systems)
    {
        if (s.room == roomId) return &s;
    }
    return nullptr;
}

int OxygenInRoom(int roomIndex)
{
    if (roomIndex < 0 || roomIndex >= (int)snap.oxygen.size()) return 100;
    return snap.oxygen[roomIndex];
}

// Brightness of a room's own light: off without power, flickering when damaged
float RoomLightLevel(int roomIndex)
{
    const SystemState *sys = SystemInRoom(roomIndex);
    if (!sys) return 1.f;
    if (sys->maxHealth > 0 && sys->health <= 0) return 0.25f + 0.35f * (std::sin(timeNow * 23.f + roomIndex) > 0.6f ? 1.f : 0.f);
    if (sys->health < sys->maxHealth) return 0.75f + 0.25f * std::sin(timeNow * 9.f + roomIndex * 1.7f);
    return sys->power > 0 ? 1.f : 0.6f;
}

void BuildLights()
{
    lights.clear();
    for (size_t r = 0; r < layout.rooms.size(); ++r)
    {
        const LayoutRoom& room = layout.rooms[r];
        float level = RoomLightLevel((int)r);
        Light l;
        l.pos = {room.x + room.w / 2.f, room.y + room.h / 2.f, WALL_H - 4.f};
        l.color = GL_Color(1.f * level, 0.95f * level, 0.85f * level, 1.f);
        l.radius = std::max(room.w, room.h) * 0.85f + 40.f;
        l.room = (int)r;
        lights.push_back(l);
    }
    for (auto& f : snap.fires)
    {
        float flicker = 0.8f + 0.2f * std::sin(timeNow * 17.f + f.first * 3.f + f.second * 5.f);
        Light l;
        l.pos = {TileLeft(f.first) + TILE / 2.f, TileTop(f.second) + TILE / 2.f, 12.f};
        l.color = GL_Color(1.4f * flicker, 0.6f * flicker, 0.15f * flicker, 1.f);
        l.radius = 85.f;
        l.room = layout.RoomAt(f.first, f.second);
        lights.push_back(l);
    }
    for (auto& b : snap.breaches)
    {
        Light l;
        l.pos = {TileLeft(b.first) + TILE / 2.f, TileTop(b.second) + TILE / 2.f, 10.f};
        l.color = GL_Color(0.25f, 0.45f, 0.9f, 1.f);
        l.radius = 60.f;
        l.room = layout.RoomAt(b.first, b.second);
        lights.push_back(l);
    }
}

bool HullCritical()
{
    return snap.maxHull > 0 && snap.hull * 3 <= snap.maxHull;
}

GL_Color Lit(Vec3 p, Vec3 n, int room, float camDist)
{
    float r = 0.16f, g = 0.17f, b = 0.2f;

    // Red alert: the hull is low, or the ship is in a fight
    if (HullCritical() || (snap.flags & Coop::CREW_SHIP_COMBAT))
    {
        float pulse = 0.5f + 0.5f * std::sin(timeNow * (HullCritical() ? 5.f : 2.5f));
        r += (HullCritical() ? 0.35f : 0.12f) * pulse;
    }

    for (const Light& l : lights)
    {
        if (l.room >= 0 && room >= 0 && l.room != room) continue;
        float dx = l.pos.x - p.x, dy = l.pos.y - p.y, dz = l.pos.z - p.z;
        float d = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (d >= l.radius) continue;
        float fall = 1.f - d / l.radius;
        fall *= fall;
        float facing = d > 0.01f ? (dx * n.x + dy * n.y + dz * n.z) / d : 1.f;
        float shade = 0.45f + 0.55f * std::max(0.f, facing);
        r += l.color.r * fall * shade;
        g += l.color.g * fall * shade;
        b += l.color.b * fall * shade;
    }

    // Head lamp
    float lamp = std::max(0.f, 1.f - camDist / 170.f) * 0.35f;
    r += lamp;
    g += lamp;
    b += lamp * 0.95f;

    // Thin air turns the room blue
    if (OxygenInRoom(room) < 25)
    {
        r *= 0.7f;
        g *= 0.8f;
        b *= 1.2f;
    }

    float fog = std::max(0.12f, 1.f - camDist / FOG_DIST);
    return GL_Color(std::min(1.f, r * fog), std::min(1.f, g * fog), std::min(1.f, b * fog), 1.f);
}

// Drawing helpers

GL_Color Species(const std::string& species)
{
    if (species == "engi") return GL_Color(0.35f, 0.75f, 0.5f, 1.f);
    if (species == "mantis") return GL_Color(0.55f, 0.8f, 0.3f, 1.f);
    if (species == "rock") return GL_Color(0.8f, 0.45f, 0.25f, 1.f);
    if (species == "slug") return GL_Color(0.55f, 0.75f, 0.65f, 1.f);
    if (species == "energy") return GL_Color(1.f, 0.9f, 0.4f, 1.f);
    if (species == "crystal") return GL_Color(0.55f, 0.75f, 1.f, 1.f);
    if (species == "anaerobic") return GL_Color(0.6f, 0.6f, 0.68f, 1.f);
    if (species == "ghost") return GL_Color(0.8f, 0.85f, 0.95f, 1.f);
    if (species == "battle" || species == "repair") return GL_Color(0.5f, 0.52f, 0.56f, 1.f);
    return GL_Color(0.85f, 0.7f, 0.55f, 1.f); // human and the rest
}

GL_Color Mul(GL_Color a, GL_Color b)
{
    return GL_Color(a.r * b.r, a.g * b.g, a.b * b.b, a.a * b.a);
}

// An upright prism (body, head, arm) around (x, y), lit as it faces
void Prism(Coop::R3D::Frame& frame, float x, float y, float z0, float z1, float radius, int sides, float rotation,
           GL_Color color, int room)
{
    Vec3 cam = {frame.GetCamera().x, frame.GetCamera().y, frame.GetCamera().z};
    Coop::R3D::Vert top[8];
    for (int i = 0; i < sides; ++i)
    {
        float a0 = rotation + 2.f * PI * i / sides, a1 = rotation + 2.f * PI * (i + 1) / sides;
        float am = (a0 + a1) / 2.f;
        Vec3 n = {std::cos(am), std::sin(am), 0.f};
        Vec3 p0 = {x + std::cos(a0) * radius, y + std::sin(a0) * radius, z0};
        Vec3 p1 = {x + std::cos(a1) * radius, y + std::sin(a1) * radius, z0};
        float dist = std::hypot(p0.x - cam.x, p0.y - cam.y);
        GL_Color lo = Mul(Lit({p0.x, p0.y, z0}, n, room, dist), color);
        GL_Color hi = Mul(Lit({p0.x, p0.y, z1}, n, room, dist), color);
        frame.Quad(tex.white, V(p0.x, p0.y, z1, 0, 0, hi), V(p1.x, p1.y, z1, 1, 0, hi), V(p1.x, p1.y, z0, 1, 1, lo),
                   V(p0.x, p0.y, z0, 0, 1, lo));
        top[i] = V(p0.x, p0.y, z1, 0, 0, Mul(Lit({x, y, z1}, {0, 0, 1}, room, dist), color));
    }
    frame.Poly(tex.white, top, sides, -0.05f);
}

void DrawCrewMember(Coop::R3D::Frame& frame, const CrewState& c)
{
    int room = layout.RoomAt(layout.TileX(c.sx), layout.TileY(c.sy));
    bool hostile = (c.flags & Coop::CREW_INTRUDER) != 0;
    GL_Color body = Species(c.species);
    if (hostile) body = GL_Color(std::min(1.f, body.r * 0.6f + 0.45f), body.g * 0.45f, body.b * 0.45f, 1.f);
    GL_Color suit = hostile ? GL_Color(0.45f, 0.18f, 0.18f, 1.f) : GL_Color(0.3f, 0.36f, 0.46f, 1.f);

    float bob = std::sin(c.walkPhase * 2.f) * 0.8f * std::min(1.f, c.speed / 30.f);
    float f = c.facing;
    float fx = std::cos(f), fy = std::sin(f);
    float rx = -fy, ry = fx;

    // Legs, torso, head
    float stride = std::sin(c.walkPhase) * 3.5f * std::min(1.f, c.speed / 30.f);
    Prism(frame, c.sx + rx * 2.6f + fx * stride, c.sy + ry * 2.6f + fy * stride, 0.f, 8.f, 2.1f, 5, f, suit, room);
    Prism(frame, c.sx - rx * 2.6f - fx * stride, c.sy - ry * 2.6f - fy * stride, 0.f, 8.f, 2.1f, 5, f, suit, room);
    Prism(frame, c.sx, c.sy, 8.f + bob, 17.f + bob, 5.5f, 8, f, suit, room);
    Prism(frame, c.sx, c.sy, 17.5f + bob, 24.f + bob, 4.3f, 6, f, body, room);

    // Arms: swing when walking, punch when fighting, hammer when repairing
    float swing = std::sin(c.walkPhase) * 4.f * std::min(1.f, c.speed / 30.f);
    float reach = 0.f, lift = 0.f;
    if (c.flags & Coop::CREW_FIGHTING)
    {
        reach = 5.f * std::max(0.f, std::sin(timeNow * 14.f));
        lift = 4.f;
    }
    else if (c.flags & Coop::CREW_REPAIRING)
    {
        reach = 3.f;
        lift = 3.f + 3.f * std::sin(timeNow * 11.f);
    }
    for (int side = -1; side <= 1; side += 2)
    {
        float s = swing * side;
        float ax = c.sx + rx * 6.8f * side + fx * (s + reach), ay = c.sy + ry * 6.8f * side + fy * (s + reach);
        Prism(frame, ax, ay, 9.f + bob + lift, 16.5f + bob + lift * 0.5f, 1.6f, 4, f, body, room);
    }

    // Visor on the front of the head, glowing
    GL_Color visor = hostile ? GL_Color(1.f, 0.25f, 0.2f, 1.f) : GL_Color(0.4f, 0.95f, 1.f, 1.f);
    float vx = c.sx + fx * 4.4f, vy = c.sy + fy * 4.4f;
    frame.Quad(tex.white, V(vx - rx * 2.8f, vy - ry * 2.8f, 22.5f + bob, 0, 0, visor),
               V(vx + rx * 2.8f, vy + ry * 2.8f, 22.5f + bob, 1, 0, visor),
               V(vx + rx * 2.8f, vy + ry * 2.8f, 20.f + bob, 1, 1, visor),
               V(vx - rx * 2.8f, vy - ry * 2.8f, 20.f + bob, 0, 1, visor), -0.3f);

    // Health bar over the head
    float hp = c.health / 100.f;
    GL_Color hpColor = hp > 0.5f ? GL_Color(0.3f, 1.f, 0.3f, 1.f) : hp > 0.25f ? GL_Color(1.f, 0.8f, 0.2f, 1.f) : GL_Color(1.f, 0.25f, 0.2f, 1.f);
    frame.Billboard(tex.white, {c.sx, c.sy, 28.5f + bob}, 14.f, 1.8f, 0, 0, 1, 1, GL_Color(0.f, 0.f, 0.f, 0.7f), 0.2f);
    // The filled part starts at the bar's left end, as seen from the camera
    float camRightX = -std::sin(frame.GetCamera().yaw), camRightY = std::cos(frame.GetCamera().yaw);
    float shift = (hp - 1.f) * 7.f;
    frame.Billboard(tex.white, {c.sx + camRightX * shift, c.sy + camRightY * shift, 28.5f + bob}, 14.f * hp, 1.8f, 0, 0, 1, 1,
                    hpColor, 0.1f);
}

void DrawFire(Coop::R3D::Frame& frame, int tx, int ty)
{
    float cx = TileLeft(tx) + TILE / 2.f, cy = TileTop(ty) + TILE / 2.f;
    const float offsets[3][2] = {{-6.f, -4.f}, {5.f, 3.f}, {-1.f, 7.f}};
    for (int i = 0; i < 3; ++i)
    {
        int frameIndex = ((int)(timeNow * 12.f) + i * 3 + tx * 5 + ty * 7) % 8;
        float size = 22.f + i * 3.f;
        frame.Billboard(tex.fire, {cx + offsets[i][0], cy + offsets[i][1], 0.f}, size, size, frameIndex / 8.f, 0.f,
                        (frameIndex + 1) / 8.f, 1.f, GL_Color(1.f, 1.f, 1.f, 0.95f), -0.5f);
    }
    // Smoke drifting up to the ceiling
    for (int i = 0; i < 2; ++i)
    {
        float t = std::fmod(timeNow * 0.6f + i * 0.5f + tx * 0.13f, 1.f);
        frame.Billboard(tex.smoke, {cx + std::sin(t * 6.f + i) * 5.f, cy, 14.f + t * 26.f}, 18.f + t * 14.f, 18.f + t * 14.f,
                        0, 0, 1, 1, GL_Color(0.5f, 0.5f, 0.5f, 0.45f * (1.f - t)));
    }
    // Sparks
    if (rng() % 10 == 0)
    {
        std::uniform_real_distribution<float> u(-1.f, 1.f);
        particles.push_back({cx + u(rng) * 8.f, cy + u(rng) * 8.f, 6.f, u(rng) * 12.f, u(rng) * 12.f, 35.f + u(rng) * 15.f,
                             0.8f, 0.8f, GL_Color(1.f, 0.7f, 0.2f, 1.f), 2.5f});
    }
}

void DrawBreach(Coop::R3D::Frame& frame, int tx, int ty)
{
    float cx = TileLeft(tx) + TILE / 2.f, cy = TileTop(ty) + TILE / 2.f;
    int frames = 7;
    int frameIndex = ((int)(timeNow * 10.f)) % frames;
    frame.Billboard(tex.breach, {cx, cy, 6.f}, 26.f, 22.f, frameIndex / (float)frames, 0.f, (frameIndex + 1) / (float)frames, 1.f,
                    GL_Color(0.8f, 0.9f, 1.f, 0.9f), -0.4f);
    // Air rushing into the hole
    if (rng() % 3 == 0)
    {
        std::uniform_real_distribution<float> u(-1.f, 1.f);
        float px = cx + u(rng) * 40.f, py = cy + u(rng) * 40.f, pz = 5.f + std::abs(u(rng)) * 30.f;
        particles.push_back({px, py, pz, (cx - px) * 1.5f, (cy - py) * 1.5f, (6.f - pz) * 1.5f, 0.6f, 0.6f,
                             GL_Color(0.7f, 0.85f, 1.f, 0.6f), 1.8f});
    }
}

void DrawDoors(Coop::R3D::Frame& frame, float dt)
{
    for (size_t i = 0; i < layout.doors.size(); ++i)
    {
        float target = i < snap.doorOpen.size() ? snap.doorOpen[i] : 0.f;
        float& open = doorAnim[i];
        open += std::max(-dt * 5.f, std::min(dt * 5.f, target - open));

        const LayoutDoor& d = layout.doors[i];
        float x0 = TileLeft(d.tx), y0 = TileTop(d.ty);
        // The door plane: the edge of the tile
        Vec3 a, b;
        switch (d.dir)
        {
        case DIR_X_PLUS: a = {x0 + TILE, y0, 0}; b = {x0 + TILE, y0 + TILE, 0}; break;
        case DIR_X_MINUS: a = {x0, y0, 0}; b = {x0, y0 + TILE, 0}; break;
        case DIR_Y_PLUS: a = {x0, y0 + TILE, 0}; b = {x0 + TILE, y0 + TILE, 0}; break;
        default: a = {x0, y0, 0}; b = {x0 + TILE, y0, 0}; break;
        }
        Vec3 n = {DirY(d.dir) != 0 ? 0.f : 1.f, DirY(d.dir) != 0 ? 1.f : 0.f, 0.f};
        int room = layout.RoomAt(d.tx, d.ty);
        Vec3 mid = {(a.x + b.x) / 2.f, (a.y + b.y) / 2.f, DOOR_H / 2.f};
        GL_Color c = Lit(mid, n, room, frame.Distance(mid));

        // Two leaves sliding into the wall
        float leaf = (1.f - open) * 0.5f;
        if (leaf > 0.01f)
        {
            for (int side = 0; side < 2; ++side)
            {
                float s0 = side == 0 ? 0.f : 1.f - leaf, s1 = side == 0 ? leaf : 1.f;
                Vec3 p0 = {a.x + (b.x - a.x) * s0, a.y + (b.y - a.y) * s0, 0}, p1 = {a.x + (b.x - a.x) * s1, a.y + (b.y - a.y) * s1, 0};
                float u0 = side == 0 ? 0.5f - leaf : 0.5f, u1 = side == 0 ? 0.5f : 0.5f + leaf;
                frame.Quad(tex.door, V(p0.x, p0.y, DOOR_H, u0, 0, c), V(p1.x, p1.y, DOOR_H, u1, 0, c), V(p1.x, p1.y, 0, u1, 1, c),
                           V(p0.x, p0.y, 0, u0, 1, c));
            }
        }
        // Door frame: a post on each side
        for (float s : {0.f, 1.f})
        {
            Vec3 p = {a.x + (b.x - a.x) * s, a.y + (b.y - a.y) * s, 0};
            Prism(frame, p.x, p.y, 0.f, DOOR_H + 1.f, 2.2f, 4, PI / 4.f, GL_Color(0.7f, 0.7f, 0.75f, 1.f), room);
        }
    }
}

void UpdateParticles(Coop::R3D::Frame& frame, float dt)
{
    for (Particle& p : particles)
    {
        p.x += p.vx * dt;
        p.y += p.vy * dt;
        p.z += p.vz * dt;
        p.vz -= 30.f * dt * (p.color.b < 0.5f ? 1.f : 0.f); // sparks fall, air does not
        p.life -= dt;
    }
    particles.erase(std::remove_if(particles.begin(), particles.end(), [](const Particle& p) { return p.life <= 0.f || p.z < 0.f; }),
                    particles.end());
    if (particles.size() > 300) particles.erase(particles.begin(), particles.begin() + (particles.size() - 300));

    for (const Particle& p : particles)
    {
        GL_Color c = p.color;
        c.a *= p.life / p.maxLife;
        frame.Billboard(tex.glow, {p.x, p.y, p.z}, p.size, p.size, 0, 0, 1, 1, c, -0.6f);
    }
}

// HUD

void Rect(float x, float y, float w, float h, GL_Color c)
{
    CSurface::GL_DrawRect(x, y, w, h, c);
}

void Text(int font, float x, float y, const std::string& text, GL_Color c = COLOR_WHITE, bool center = false)
{
    CSurface::GL_SetColor(c);
    if (center) freetype::easy_printCenter(font, x, y, text);
    else freetype::easy_print(font, x, y, text);
    CSurface::GL_SetColor(COLOR_WHITE);
}

void Bar(float x, float y, float w, float h, float part, GL_Color color)
{
    Rect(x, y, w, h, GL_Color(0.f, 0.f, 0.f, 0.6f));
    Rect(x + 1, y + 1, (w - 2) * std::max(0.f, std::min(1.f, part)), h - 2, color);
    CSurface::GL_DrawRectOutline((int)x, (int)y, (int)w, (int)h, GL_Color(0.7f, 0.75f, 0.8f, 0.9f), 1.f);
}

std::string RoomName(int roomIndex)
{
    if (roomIndex < 0) return "Outside";
    const char *name = SystemName(layout.rooms[roomIndex].system);
    return name ? name : "Corridor";
}

void DrawMinimap(float mx, float my, float mw, float mh)
{
    if (!layout.Valid()) return;
    Rect(mx - 4, my - 4, mw + 8, mh + 8, GL_Color(0.f, 0.f, 0.f, 0.55f));
    float scale = std::min(mw / (layout.tilesW * TILE), mh / (layout.tilesH * TILE));
    float ox = mx + (mw - layout.tilesW * TILE * scale) / 2.f, oy = my + (mh - layout.tilesH * TILE * scale) / 2.f;
    auto toX = [&](float wx) { return ox + (wx - layout.originX) * scale; };
    auto toY = [&](float wy) { return oy + (wy - layout.originY) * scale; };

    for (size_t r = 0; r < layout.rooms.size(); ++r)
    {
        const LayoutRoom& room = layout.rooms[r];
        bool thin = OxygenInRoom((int)r) < 25;
        GL_Color fill = room.system >= 0 ? GL_Color(0.25f, 0.3f, 0.36f, 0.9f) : GL_Color(0.18f, 0.2f, 0.24f, 0.9f);
        if (thin) fill = GL_Color(0.2f, 0.25f, 0.45f, 0.9f);
        Rect(toX(room.x), toY(room.y), room.w * scale, room.h * scale, fill);
        CSurface::GL_DrawRectOutline((int)toX(room.x), (int)toY(room.y), (int)(room.w * scale), (int)(room.h * scale),
                                     GL_Color(0.6f, 0.65f, 0.7f, 1.f), 1.f);
    }
    for (auto& f : snap.fires) Rect(toX(TileLeft(f.first) + 10), toY(TileTop(f.second) + 10), 15 * scale, 15 * scale, GL_Color(1.f, 0.5f, 0.1f, 1.f));
    for (auto& entry : crew)
    {
        const CrewState& c = entry.second;
        GL_Color dot = c.id == myCrew ? GL_Color(1.f, 0.9f, 0.2f, 1.f)
                     : (c.flags & Coop::CREW_INTRUDER) ? GL_Color(1.f, 0.25f, 0.2f, 1.f)
                     : c.owner >= 0 ? GL_Color(0.4f, 0.8f, 1.f, 1.f) : GL_Color(0.4f, 1.f, 0.4f, 1.f);
        float s = c.id == myCrew ? 5.f : 4.f;
        Rect(toX(c.sx) - s / 2, toY(c.sy) - s / 2, s, s, dot);
    }
    // View cone
    float cx = toX(camX), cy = toY(camY);
    for (float side : {-0.6f, 0.6f})
    {
        CSurface::GL_DrawLine(cx, cy, cx + std::cos(yaw + side) * 28.f, cy + std::sin(yaw + side) * 28.f, 1.f, GL_Color(1.f, 0.9f, 0.3f, 0.7f));
    }
}

void DrawHands(float bobX, float bobY)
{
    // A gloved hand holding a repair tool, bottom right
    float x = 930.f + bobX, y = 560.f + bobY;
    GL_Color glove(0.25f, 0.3f, 0.38f, 1.f), tool(0.55f, 0.57f, 0.6f, 1.f), tip(0.95f, 0.75f, 0.2f, 1.f);
    CrewState *me = MyCrew();
    if (me) glove = Mul(Species(me->species), GL_Color(0.8f, 0.8f, 0.8f, 1.f));
    Rect(x + 40, y + 70, 120, 110, GL_Color(0.2f, 0.25f, 0.32f, 1.f)); // sleeve
    Rect(x + 30, y + 20, 90, 70, glove);                                // hand
    Rect(x + 10, y - 60, 34, 120, tool);                                 // tool shaft
    Rect(x, y - 90, 54, 34, tip);                                        // tool head
    Rect(x + 6, y - 84, 42, 6, GL_Color(1.f, 1.f, 0.85f, 1.f));          // highlight
}

void DrawHud()
{
    CrewState *me = MyCrew();
    int room = layout.RoomAt(layout.TileX(camX), layout.TileY(camY));

    // Crosshair
    Rect(638, 352, 4, 16, GL_Color(1.f, 1.f, 1.f, 0.7f));
    Rect(632, 358, 16, 4, GL_Color(1.f, 1.f, 1.f, 0.7f));

    // Top left: room and air
    Rect(12, 12, 250, 62, GL_Color(0.f, 0.f, 0.f, 0.5f));
    Text(14, 22, 18, RoomName(room));
    int o2 = OxygenInRoom(room);
    bool thin = o2 < 25;
    Text(10, 22, 42, "OXYGEN " + std::to_string(o2) + "%", thin ? GL_Color(0.6f, 0.75f, 1.f, 1.f) : COLOR_WHITE);
    Bar(22, 58, 220, 8, o2 / 100.f, GL_Color(0.35f, 0.6f, 1.f, 1.f));
    if (thin && std::fmod(timeNow, 1.f) < 0.6f) Text(13, 640, 120, "LOW OXYGEN", GL_Color(0.5f, 0.7f, 1.f, 1.f), true);

    // Top right: the ship
    Rect(1018, 12, 250, 62, GL_Color(0.f, 0.f, 0.f, 0.5f));
    Text(12, 1030, 18, "HULL " + std::to_string(snap.hull) + "/" + std::to_string(snap.maxHull));
    float hullPart = snap.maxHull > 0 ? (float)snap.hull / snap.maxHull : 0.f;
    Bar(1030, 38, 225, 10, hullPart, hullPart > 0.5f ? GL_Color(0.4f, 1.f, 0.4f, 1.f) : hullPart > 0.25f ? GL_Color(1.f, 0.8f, 0.2f, 1.f) : GL_Color(1.f, 0.3f, 0.2f, 1.f));
    for (int i = 0; i < snap.shields; ++i) Rect(1030 + i * 18, 54, 14, 12, GL_Color(0.35f, 0.75f, 1.f, 1.f));
    if (snap.flags & Coop::CREW_SHIP_COMBAT) Text(12, 1255, 54, "COMBAT", GL_Color(1.f, 0.35f, 0.3f, 1.f));
    if (HullCritical() && std::fmod(timeNow, 0.8f) < 0.45f) Text(16, 640, 90, "HULL CRITICAL", GL_Color(1.f, 0.3f, 0.25f, 1.f), true);

    // Bottom left: me
    Rect(12, 630, 300, 78, GL_Color(0.f, 0.f, 0.f, 0.5f));
    if (me)
    {
        Text(14, 22, 638, me->name);
        Bar(22, 662, 280, 12, me->health / 100.f, me->health > 50 ? GL_Color(0.3f, 1.f, 0.3f, 1.f) : me->health > 25 ? GL_Color(1.f, 0.8f, 0.2f, 1.f) : GL_Color(1.f, 0.25f, 0.2f, 1.f));
        std::string status = "Standing by";
        if (me->flags & Coop::CREW_FIGHTING) status = "Fighting!";
        else if (me->flags & Coop::CREW_REPAIRING) status = "Repairing";
        else if (me->flags & Coop::CREW_MANNING) status = std::string("Manning ") + RoomName(room);
        if (me->flags & Coop::CREW_ON_FIRE) status += " - ON FIRE";
        if (me->flags & Coop::CREW_SUFFOCATING) status += " - SUFFOCATING";
        Text(10, 22, 684, status, (me->flags & (Coop::CREW_ON_FIRE | Coop::CREW_SUFFOCATING)) ? GL_Color(1.f, 0.5f, 0.3f, 1.f) : GL_Color(0.75f, 0.9f, 1.f, 1.f));
    }
    else
    {
        Text(13, 22, 640, "Spectating");
        Text(10, 22, 664, "No crew member to control. Press R to pick one.");
    }

    // Minimap
    if (bigMap) DrawMinimap(290, 110, 700, 500);
    else DrawMinimap(1040, 560, 220, 140);

    Text(10, 640, 704, "WASD move   Mouse look   E door   Q/R switch crew   Tab map   Esc mouse   F10 leave",
         GL_Color(0.75f, 0.8f, 0.85f, 0.85f), true);
    if (!mouseCaptured) Text(13, 640, 400, "Click to look around with the mouse", GL_Color(1.f, 1.f, 0.7f, 1.f), true);
    if (snap.flags & Coop::CREW_SHIP_PAUSED) Text(18, 640, 200, "PAUSED", GL_Color(1.f, 1.f, 1.f, 0.9f), true);

    if (!snap.valid || Clock::now() - snap.received > std::chrono::seconds(2))
    {
        Rect(0, 0, 1280, 720, GL_Color(0.f, 0.f, 0.f, 0.6f));
        Text(18, 640, 330, "Waiting for the captain's ship...", COLOR_WHITE, true);
    }
    else if (snap.flags & Coop::CREW_SHIP_DESTROYED)
    {
        Rect(0, 0, 1280, 720, GL_Color(0.3f, 0.f, 0.f, 0.6f));
        Text(18, 640, 330, "THE SHIP IS LOST", COLOR_WHITE, true);
    }
}

void Render()
{
    LoadTextures();
    auto now = Clock::now();
    float dt = std::min(0.1f, std::chrono::duration<float>(now - lastFrame).count());
    lastFrame = now;
    timeNow += dt;

    // Background
    Rect(0, 0, 1280, 720, GL_Color(0.01f, 0.01f, 0.02f, 1.f));
    if (!layout.Valid())
    {
        DrawHud();
        return;
    }
    if (builtLayout != layout.id) BuildGeometry();

    // Smooth the crew towards the host's positions
    for (auto& entry : crew)
    {
        CrewState& c = entry.second;
        float k = std::min(1.f, dt * 12.f);
        float nx = c.sx + (c.x - c.sx) * k, ny = c.sy + (c.y - c.sy) * k;
        float moved = std::hypot(nx - c.sx, ny - c.sy);
        c.speed = dt > 0.f ? moved / dt : 0.f;
        if (moved > 0.05f) c.facing = std::atan2(ny - c.sy, nx - c.sx);
        c.walkPhase += moved * 0.35f;
        c.sx = nx;
        c.sy = ny;
    }

    // Camera on my crew member, or a free camera
    CrewState *me = MyCrew();
    float camBob = 0.f, handBobX = 0.f, handBobY = 0.f;
    if (me)
    {
        camX = me->sx;
        camY = me->sy;
        camPlaced = true;
        float amount = std::min(1.f, me->speed / 30.f);
        camBob = std::sin(me->walkPhase * 2.f) * 1.2f * amount;
        handBobX = std::sin(me->walkPhase) * 10.f * amount;
        handBobY = std::abs(std::cos(me->walkPhase)) * 8.f * amount;
    }
    else if (!camPlaced)
    {
        camX = layout.originX + layout.tilesW * TILE / 2.f;
        camY = layout.originY + layout.tilesH * TILE / 2.f;
        camPlaced = true;
    }

    // Hits shake the camera
    shake = std::max(0.f, shake - dt * 2.5f);
    std::uniform_real_distribution<float> jitter(-1.f, 1.f);

    Coop::R3D::Camera camera;
    camera.x = camX + jitter(rng) * shake * 3.f;
    camera.y = camY + jitter(rng) * shake * 3.f;
    camera.z = EYE_H + camBob + jitter(rng) * shake * 2.f;
    camera.yaw = yaw;
    camera.pitch = pitch;
    camera.fov = 1.45f;

    Coop::R3D::Frame frame;
    frame.Begin(camera);
    BuildLights();

    // Static geometry, lit for this frame
    for (const Surface& s : surfaces)
    {
        Coop::R3D::Vert v[4];
        for (int i = 0; i < 4; ++i)
        {
            GL_Color c;
            float dist = frame.Distance(s.p[i]);
            if (s.kind == SURF_LIGHT)
            {
                float level = RoomLightLevel(s.room);
                c = GL_Color(level, level * 0.97f, level * 0.9f, 1.f);
            }
            else if (s.kind == SURF_CONSOLE_SCREEN)
            {
                const SystemState *sys = SystemInRoom(s.room);
                c = GL_Color(0.8f, 1.f, 0.9f, 1.f);
                if (sys && sys->maxHealth > 0 && sys->health <= 0) c = GL_Color(1.f, 0.25f, 0.2f, 1.f);
                else if (sys && sys->health < sys->maxHealth) c = GL_Color(1.f, 0.85f, 0.3f, 1.f);
                else if (sys && sys->power <= 0) c = GL_Color(0.3f, 0.35f, 0.35f, 1.f);
            }
            else
            {
                c = Lit(s.p[i], s.normal, s.room, dist);
            }
            v[i] = V(s.p[i].x, s.p[i].y, s.p[i].z, s.u[i], s.v[i], c);
        }
        frame.Poly(s.texture, v, 4, s.kind == SURF_FLOOR ? 0.5f : 0.f);
    }

    DrawDoors(frame, dt);
    for (auto& entry : crew)
    {
        if (entry.first != myCrew) DrawCrewMember(frame, entry.second);
    }
    for (auto& f : snap.fires) DrawFire(frame, f.first, f.second);
    for (auto& b : snap.breaches) DrawBreach(frame, b.first, b.second);
    UpdateParticles(frame, dt);

    frame.Draw();

    // Names over nearby crew
    for (auto& entry : crew)
    {
        const CrewState& c = entry.second;
        if (c.id == myCrew) continue;
        float sx, sy, depth;
        if (frame.Project({c.sx, c.sy, 32.f}, sx, sy, depth) && depth < 160.f)
        {
            GL_Color color = (c.flags & Coop::CREW_INTRUDER) ? GL_Color(1.f, 0.4f, 0.35f, 1.f) : c.owner >= 0 ? GL_Color(0.5f, 0.85f, 1.f, 1.f) : COLOR_WHITE;
            std::string label = c.name;
            if (c.owner >= 0) label += " (P" + std::to_string(c.owner + 1) + ")";
            Text(10, sx, sy - 12.f, label, color, true);
        }
    }

    // Screen effects
    if (me) DrawHands(handBobX, handBobY);
    if (damageFlash > 0.f)
    {
        Rect(0, 0, 1280, 720, GL_Color(0.8f, 0.f, 0.f, 0.35f * damageFlash));
        damageFlash = std::max(0.f, damageFlash - dt * 2.f);
    }
    bool jumping = (snap.flags & Coop::CREW_SHIP_JUMPING) != 0;
    jumpFade += (jumping ? 1.f : -1.f) * dt * 1.5f;
    jumpFade = std::max(0.f, std::min(1.f, jumpFade));
    if (jumpFade > 0.f) Rect(0, 0, 1280, 720, GL_Color(1.f, 1.f, 1.f, jumpFade * 0.85f));

    DrawHud();
}

// Input

void ReleaseMouse()
{
    mouseCaptured = false;
}

void CaptureMouse()
{
    mouseCaptured = true;
    skipMouseDelta = true;
}

#ifdef _WIN32
// FTL gives absolute mouse positions only, so the cursor is pinned to the window's
// centre and its movement read back each frame
void MouseLook()
{
    if (!mouseCaptured || !G_->GetCApp()->focus) return;
    HWND window = GetForegroundWindow();
    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);
    if (!window || pid != GetCurrentProcessId()) return;

    RECT rect;
    GetClientRect(window, &rect);
    POINT centre = {(rect.right - rect.left) / 2, (rect.bottom - rect.top) / 2};
    ClientToScreen(window, &centre);
    POINT cursor;
    GetCursorPos(&cursor);
    if (!skipMouseDelta)
    {
        yaw += (cursor.x - centre.x) * MOUSE_SENSITIVITY;
        pitch -= (cursor.y - centre.y) * MOUSE_SENSITIVITY;
    }
    skipMouseDelta = false;
    SetCursorPos(centre.x, centre.y);
}
#else
void MouseLook()
{
}
#endif

void SendInput()
{
    float forward = (Pressed(SDLK_w) || Pressed(SDLK_UP) ? 1.f : 0.f) - (Pressed(SDLK_s) || Pressed(SDLK_DOWN) ? 1.f : 0.f);
    float strafe = (Pressed(SDLK_d) ? 1.f : 0.f) - (Pressed(SDLK_a) ? 1.f : 0.f);
    float dx = std::cos(yaw) * forward - std::sin(yaw) * strafe;
    float dy = std::sin(yaw) * forward + std::cos(yaw) * strafe;
    float len = std::hypot(dx, dy);
    if (len > 1.f)
    {
        dx /= len;
        dy /= len;
    }

    // Without a crew member the camera flies on its own
    if (!MyCrew())
    {
        float dt = std::chrono::duration<float>(INPUT_INTERVAL).count();
        camX += dx * 140.f * dt;
        camY += dy * 140.f * dt;
    }

    float wrapped = std::fmod(yaw, 2.f * PI);
    if (wrapped < 0) wrapped += 2.f * PI;

    Coop::Writer msg(Coop::MSG_PAWN_INPUT);
    msg.U8((uint8_t)(int8_t)std::lround(dx * 100.f)).U8((uint8_t)(int8_t)std::lround(dy * 100.f))
       .U16((uint16_t)(wrapped / (2.f * PI) * 65535.f)).U8(doorPresses);
    Coop::GetNet().SendToAll(msg.Data(), msg.Size(), -1, false);
}

void SelectCrew(int step)
{
    Coop::Writer msg(Coop::MSG_PAWN_SELECT);
    msg.U8((uint8_t)(int8_t)step);
    Coop::GetNet().SendToAll(msg.Data(), msg.Size());
}

void OnKey(int key, bool down)
{
    if (key >= 0 && key < 512) keyDown[key] = down;
    if (!down) return;

    switch (key)
    {
    case SDLK_e: ++doorPresses; break;
    case SDLK_q: SelectCrew(-1); break;
    case SDLK_r: SelectCrew(1); break;
    case SDLK_TAB: bigMap = !bigMap; break;
    case SDLK_ESCAPE: if (mouseCaptured) ReleaseMouse(); break;
    case SDLK_F10: Coop::CrewView::Leave(); break;
    default: break;
    }
}

}

namespace Coop
{
namespace CrewView
{

bool Active()
{
    return active;
}

void Enter()
{
    if (active) return;
    active = true;
    myCrew = 0;
    crew.clear();
    snap = Snapshot();
    camPlaced = false;
    yaw = pitch = 0.f;
    particles.clear();
    lastFrame = Clock::now();
    std::fill(std::begin(keyDown), std::end(keyDown), false);
    CaptureMouse();
    SelectCrew(0);
    Log("Crew mode: you are aboard the captain's ship");
}

void Leave()
{
    if (!active) return;
    active = false;
    ReleaseMouse();
    G_->GetMouseControl()->bHideMouse = false;
    if (GetNet().GetRole() == Role::Client) GetNet().Leave();
}

void OnMessage(int peerId, uint8_t type, const uint8_t *payload, size_t size)
{
    (void)peerId;
    if (GetNet().GetRole() != Role::Client) return;
    Reader in(payload, size);

    switch (type)
    {
    case MSG_CREW_LAYOUT:
    {
        Layout incoming;
        if (incoming.Read(in) && incoming.id != layout.id) layout = incoming;
        break;
    }
    case MSG_PAWN_ASSIGN:
    {
        uint32_t id = in.U32();
        if (in.ok) myCrew = id;
        break;
    }
    case MSG_CREW_SNAPSHOT:
    {
        if (!active) break;
        Snapshot next;
        next.layoutId = in.U32();
        next.hull = (int)in.U32();
        next.maxHull = (int)in.U32();
        next.shields = in.U8();
        next.flags = in.U8();

        int count = in.U8();
        auto now = Clock::now();
        for (int i = 0; i < count; ++i)
        {
            uint32_t id = in.U32();
            float x = in.I16() / 4.f, y = in.I16() / 4.f;
            int health = in.U8();
            int flags = in.U8();
            int owner = in.U8();
            std::string species = in.Str();
            std::string name = in.Str();
            if (!in.ok) break;

            CrewState& c = crew[id];
            if (!c.seen)
            {
                c.sx = x;
                c.sy = y;
                c.seen = true;
            }
            c.id = id;
            c.x = x;
            c.y = y;
            c.health = health;
            c.flags = flags;
            c.owner = owner == 255 ? -1 : owner;
            c.species = species;
            c.name = name;
            c.lastSeen = now;
        }

        next.doorOpen.resize(in.U16());
        for (float& d : next.doorOpen) d = in.U8() / 100.f;
        next.fires.resize(in.U16());
        for (auto& f : next.fires)
        {
            f.first = in.U8();
            f.second = in.U8();
        }
        next.breaches.resize(in.U16());
        for (auto& b : next.breaches)
        {
            b.first = in.U8();
            b.second = in.U8();
        }
        next.oxygen.resize(in.U16());
        for (int& o : next.oxygen) o = in.U8();
        next.systems.resize(in.U8());
        for (SystemState& s : next.systems)
        {
            s.room = in.U8();
            s.type = in.U8();
            s.health = in.U8();
            s.maxHealth = in.U8();
            s.power = in.U8();
        }
        if (!in.ok) break;

        // Crew that left or died
        for (auto it = crew.begin(); it != crew.end();)
        {
            if (it->second.lastSeen != now) it = crew.erase(it);
            else ++it;
        }

        // Hits on the ship and on me
        if (lastHull >= 0 && next.hull < lastHull) shake = std::min(1.5f, shake + 0.6f + 0.15f * (lastHull - next.hull));
        lastHull = next.hull;
        CrewState *me = MyCrew();
        if (me)
        {
            if (lastMyHealth >= 0 && me->health < lastMyHealth) damageFlash = 1.f;
            lastMyHealth = me->health;
        }

        next.valid = true;
        next.received = now;
        snap = next;
        break;
    }
    default:
        break;
    }
}

void OnLoop()
{
    if (!active) return;
    if (!GetNet().IsConnected())
    {
        active = false;
        ReleaseMouse();
        G_->GetMouseControl()->bHideMouse = false;
        Log("Left crew mode: the connection to the captain is gone");
        return;
    }

    MouseLook();
    if (!mouseCaptured)
    {
        float dt = 1.f / 60.f;
        if (Pressed(SDLK_LEFT)) yaw -= TURN_SPEED * dt;
        if (Pressed(SDLK_RIGHT)) yaw += TURN_SPEED * dt;
    }
    pitch = std::max(-0.7f, std::min(0.6f, pitch));

    auto now = Clock::now();
    if (now - lastInput >= INPUT_INTERVAL)
    {
        lastInput = now;
        SendInput();
    }
    G_->GetMouseControl()->bHideMouse = mouseCaptured;
}

}
}

// Drawn last, over the menus the client's game is sitting in
HOOK_METHOD(MouseControl, OnRender, () -> void)
{
    LOG_HOOK("HOOK_METHOD -> MouseControl::OnRender -> Begin (CoopCrewView.cpp)\n")
    if (active) Render();
    super();
}

// While the view is open it takes all input

HOOK_METHOD(CApp, OnKeyDown, (SDLKey key) -> void)
{
    LOG_HOOK("HOOK_METHOD -> CApp::OnKeyDown -> Begin (CoopCrewView.cpp)\n")
    if (active) return OnKey((int)key, true);
    super(key);
}

HOOK_METHOD(CApp, OnKeyUp, (SDLKey key) -> void)
{
    LOG_HOOK("HOOK_METHOD -> CApp::OnKeyUp -> Begin (CoopCrewView.cpp)\n")
    if (active) return OnKey((int)key, false);
    super(key);
}

HOOK_METHOD(CApp, OnMouseMove, (int x, int y, int xdiff, int ydiff, bool holdingLMB, bool holdingRMB, bool holdingMMB) -> void)
{
    LOG_HOOK("HOOK_METHOD -> CApp::OnMouseMove -> Begin (CoopCrewView.cpp)\n")
    if (active) return;
    super(x, y, xdiff, ydiff, holdingLMB, holdingRMB, holdingMMB);
}

HOOK_METHOD(CApp, OnLButtonDown, (int x, int y) -> void)
{
    LOG_HOOK("HOOK_METHOD -> CApp::OnLButtonDown -> Begin (CoopCrewView.cpp)\n")
    if (active)
    {
        if (!mouseCaptured) CaptureMouse();
        return;
    }
    super(x, y);
}

HOOK_METHOD(CApp, OnLButtonUp, (int x, int y) -> void)
{
    LOG_HOOK("HOOK_METHOD -> CApp::OnLButtonUp -> Begin (CoopCrewView.cpp)\n")
    if (active) return;
    super(x, y);
}

HOOK_METHOD(CApp, OnRButtonDown, (int x, int y) -> void)
{
    LOG_HOOK("HOOK_METHOD -> CApp::OnRButtonDown -> Begin (CoopCrewView.cpp)\n")
    if (active) return;
    super(x, y);
}

HOOK_METHOD(CApp, OnRButtonUp, (int x, int y) -> void)
{
    LOG_HOOK("HOOK_METHOD -> CApp::OnRButtonUp -> Begin (CoopCrewView.cpp)\n")
    if (active) return;
    super(x, y);
}
