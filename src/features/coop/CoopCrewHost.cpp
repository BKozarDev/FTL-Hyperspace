#include "Global.h"
#include "features/coop/CoopCrew.h"
#include "features/coop/CoopFeature.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <map>

// Crew mode, host side. The host plays the captain as usual; every other player
// controls one crew member of the host's ship in first person. The host:
// - sends the ship's layout (on change and every few seconds) and a state snapshot
//   20 times a second: crew, doors, fires, breaches, oxygen, systems
// - walks each player's crew member tile by tile in the direction the player
//   pushes, opens and closes the door they face, and lets FTL do the rest
//   (repairing, manning, fighting happen where the crew member stands)
// - ignores the captain's orders for those crew members and labels them

namespace
{

using Clock = std::chrono::steady_clock;
using namespace Coop::Crew;

constexpr auto SNAPSHOT_INTERVAL = std::chrono::milliseconds(50);
constexpr auto LAYOUT_INTERVAL = std::chrono::seconds(3);
constexpr float ARRIVED = 8.f; // px from the goal at which the next step may start
constexpr float PI = 3.14159265f;

struct Pawn
{
    uint32_t crewId = 0;
    float dirX = 0.f, dirY = 0.f;
    float yaw = 0.f;
    uint8_t doorPresses = 0;
    bool doorPressesKnown = false;
    int goalTx = -1, goalTy = -1;
};

std::map<int, Pawn> pawns; // client key -> pawn
Layout layout;
std::vector<Door*> layoutDoors; // FTL doors in the order of layout.doors
ShipGraph *layoutGraph = nullptr;
Clock::time_point lastSnapshot, lastLayout;
bool drivingPawn = false;

bool Active()
{
    return Coop::GetNet().GetRole() == Coop::Role::Host && Coop::Lobby::CrewMode() && Coop::InRun() && G_->GetShipManager(0);
}

uint32_t CrewId(CrewMember *crew)
{
    return (uint32_t)(uintptr_t)crew;
}

// Crew on the host's ship: its own crew aboard, and boarders
std::vector<CrewMember*> CrewAboard()
{
    std::vector<CrewMember*> list;
    for (int shipId = 0; shipId < 2; ++shipId)
    {
        ShipManager *ship = G_->GetShipManager(shipId);
        if (!ship) continue;
        for (CrewMember *crew : ship->vCrewList)
        {
            if (crew && !crew->bDead && !crew->bOutOfGame && crew->currentShipId == 0) list.push_back(crew);
        }
    }
    return list;
}

CrewMember *FindCrew(uint32_t id)
{
    if (!id) return nullptr;
    for (CrewMember *crew : CrewAboard())
    {
        if (CrewId(crew) == id) return crew;
    }
    return nullptr;
}

int OwnerOf(uint32_t crewId)
{
    for (auto& entry : pawns)
    {
        if (entry.second.crewId == crewId) return entry.first;
    }
    return -1;
}

bool IsPawn(CrewMember *crew)
{
    return Active() && OwnerOf(CrewId(crew)) >= 0;
}

// Layout

uint32_t Hash(uint32_t h, int value)
{
    return (h ^ (uint32_t)value) * 16777619u;
}

// The tile edge between a door's two rooms closest to the door's position.
// Where FTL keeps that position (pixels or tiles, centre or corner) is not known
// for sure, so every reading is tried and the closest edge wins.
bool PlaceDoor(Door *door, LayoutDoor& out)
{
    int r1 = layout.RoomIndexById(door->iRoom1);
    int r2 = layout.RoomIndexById(door->iRoom2);
    if (r1 < 0 && r2 < 0) return false;
    if (r1 < 0) std::swap(r1, r2);

    Point pos = door->GetPosition();
    std::vector<std::pair<float, float>> points = {
        {(float)pos.x, (float)pos.y},
        {pos.x + door->width / 2.f, pos.y + door->height / 2.f},
        {(float)door->x, (float)door->y},
        {door->x + door->width / 2.f, door->y + door->height / 2.f},
        {(float)door->x * TILE, (float)door->y * TILE},
    };

    float best = 1e9f;
    bool found = false;
    for (int ty = 0; ty < layout.tilesH; ++ty)
    {
        for (int tx = 0; tx < layout.tilesW; ++tx)
        {
            if (layout.RoomAt(tx, ty) != r1) continue;
            for (int dir = 0; dir < 4; ++dir)
            {
                if (layout.RoomAt(tx + DirX(dir), ty + DirY(dir)) != r2) continue;
                // A vertical door sits between tiles side by side
                bool vertical = DirX(dir) != 0;
                float orientationPenalty = vertical == door->bVertical ? 0.f : 4.f;

                float mx = layout.originX + (tx + 0.5f + DirX(dir) * 0.5f) * TILE;
                float my = layout.originY + (ty + 0.5f + DirY(dir) * 0.5f) * TILE;
                for (auto& p : points)
                {
                    float d = std::hypot(mx - p.first, my - p.second) + orientationPenalty;
                    if (d < best)
                    {
                        best = d;
                        out.tx = tx;
                        out.ty = ty;
                        out.dir = dir;
                        found = true;
                    }
                }
            }
        }
    }
    // Rooms joined by a single possible edge need no position at all
    return found && best < TILE * 1.5f;
}

void BuildLayout()
{
    ShipGraph *graph = ShipGraph::GetShipInfo(0);
    ShipManager *ship = G_->GetShipManager(0);
    layoutGraph = graph;
    layout = Layout();
    layoutDoors.clear();
    if (!graph || !ship) return;

    layout.originX = 1 << 20;
    layout.originY = 1 << 20;
    for (Room *room : graph->rooms)
    {
        LayoutRoom r;
        r.id = room->iRoomId;
        r.x = room->rect.x;
        r.y = room->rect.y;
        r.w = room->rect.w;
        r.h = room->rect.h;
        r.primarySlot = room->primarySlot;
        r.primaryDir = room->primaryDirection;
        for (ShipSystem *sys : ship->vSystemList)
        {
            if (sys->roomId == room->iRoomId) r.system = sys->iSystemType;
        }
        layout.rooms.push_back(r);
        layout.originX = std::min(layout.originX, r.x);
        layout.originY = std::min(layout.originY, r.y);
    }
    // Room tiles start on a tile boundary of the origin
    layout.originX -= ((layout.originX % TILE) + TILE) % TILE;
    layout.originY -= ((layout.originY % TILE) + TILE) % TILE;
    layout.BuildTiles();

    for (Door *door : graph->doors)
    {
        LayoutDoor d;
        if (PlaceDoor(door, d))
        {
            layout.doors.push_back(d);
            layoutDoors.push_back(door);
        }
    }

    uint32_t h = 2166136261u;
    for (const LayoutRoom& r : layout.rooms)
    {
        h = Hash(Hash(Hash(Hash(Hash(h, r.id), r.x), r.y), r.w), r.h);
        h = Hash(h, r.system);
    }
    for (const LayoutDoor& d : layout.doors) h = Hash(Hash(Hash(h, d.tx), d.ty), d.dir);
    layout.id = h ? h : 1;
}

void SendLayout()
{
    Coop::Writer msg(Coop::MSG_CREW_LAYOUT);
    layout.Write(msg);
    Coop::GetNet().SendToAll(msg.Data(), msg.Size());
    lastLayout = Clock::now();
}

// Snapshot

void SendSnapshot()
{
    ShipManager *ship = G_->GetShipManager(0);
    ShipManager *enemy = G_->GetShipManager(1);

    uint8_t flags = 0;
    if (enemy && enemy->hostile_ship && !enemy->bDestroyed && enemy->ship.hullIntegrity.first > 0) flags |= Coop::CREW_SHIP_COMBAT;
    if (ship->bJumping) flags |= Coop::CREW_SHIP_JUMPING;
    if (ship->ship.hullIntegrity.first <= 0) flags |= Coop::CREW_SHIP_DESTROYED;
    if (G_->GetCApp()->gui->bPaused) flags |= Coop::CREW_SHIP_PAUSED;

    int shields = ship->shieldSystem ? ship->shieldSystem->shields.power.first : 0;

    Coop::Writer msg(Coop::MSG_CREW_SNAPSHOT);
    msg.U32(layout.id)
       .U32((uint32_t)std::max(0, ship->ship.hullIntegrity.first))
       .U32((uint32_t)std::max(0, ship->ship.hullIntegrity.second))
       .U8((uint8_t)std::max(0, shields))
       .U8(flags);

    std::vector<CrewMember*> crewList = CrewAboard();
    if (crewList.size() > 255) crewList.resize(255);
    msg.U8((uint8_t)crewList.size());
    for (CrewMember *crew : crewList)
    {
        uint8_t crewFlags = 0;
        if (crew->intruder || crew->iShipId != 0) crewFlags |= Coop::CREW_INTRUDER;
        if (crew->bFighting) crewFlags |= Coop::CREW_FIGHTING;
        if (crew->iRepairId >= 0) crewFlags |= Coop::CREW_REPAIRING;
        if (crew->iManningId >= 0 && crew->bActiveManning) crewFlags |= Coop::CREW_MANNING;
        if (crew->iOnFire > 0) crewFlags |= Coop::CREW_ON_FIRE;
        if (crew->bSuffocating) crewFlags |= Coop::CREW_SUFFOCATING;

        float health = crew->health.second > 0.f ? crew->health.first / crew->health.second : 0.f;
        int owner = OwnerOf(CrewId(crew));

        msg.U32(CrewId(crew))
           .I16((int)std::lround(crew->x * 4.f))
           .I16((int)std::lround(crew->y * 4.f))
           .U8((uint8_t)std::max(0.f, std::min(100.f, health * 100.f)))
           .U8(crewFlags)
           .U8(owner < 0 ? 255 : (uint8_t)owner)
           .Str(crew->species)
           .Str(crew->GetName());
    }

    msg.U16((uint16_t)layoutDoors.size());
    for (Door *door : layoutDoors) msg.U8(door->bOpen ? 100 : 0);

    // Fires burn on tiles; the grid counts tiles of FTL's ship space
    std::vector<std::pair<int, int>> fires;
    const auto& grid = ship->fireSpreader.grid;
    for (size_t gx = 0; gx < grid.size(); ++gx)
    {
        for (size_t gy = 0; gy < grid[gx].size(); ++gy)
        {
            if (grid[gx][gy].fDamage > 0.f)
            {
                int tx = (int)gx - layout.originX / TILE;
                int ty = (int)gy - layout.originY / TILE;
                if (layout.RoomAt(tx, ty) >= 0) fires.emplace_back(tx, ty);
            }
        }
    }
    msg.U16((uint16_t)fires.size());
    for (auto& f : fires) msg.U8((uint8_t)f.first).U8((uint8_t)f.second);

    // Hull breaches; their location may be in pixels or tiles
    std::vector<std::pair<int, int>> breaches;
    for (OuterHull *hull : ship->ship.vOuterWalls)
    {
        if (!hull || hull->fDamage <= 0.f) continue;
        Point p = hull->pLoc;
        int tx, ty;
        if (p.x > layout.tilesW + layout.originX / TILE + 4 || p.y > layout.tilesH + layout.originY / TILE + 4)
        {
            tx = layout.TileX((float)p.x);
            ty = layout.TileY((float)p.y);
        }
        else
        {
            tx = p.x - layout.originX / TILE;
            ty = p.y - layout.originY / TILE;
        }
        if (layout.RoomAt(tx, ty) >= 0) breaches.emplace_back(tx, ty);
    }
    msg.U16((uint16_t)breaches.size());
    for (auto& b : breaches) msg.U8((uint8_t)b.first).U8((uint8_t)b.second);

    msg.U16((uint16_t)layout.rooms.size());
    for (const LayoutRoom& r : layout.rooms)
    {
        float o2 = 100.f;
        if (ship->oxygenSystem && r.id >= 0 && r.id < (int)ship->oxygenSystem->oxygenLevels.size())
        {
            o2 = ship->oxygenSystem->oxygenLevels[r.id];
        }
        msg.U8((uint8_t)std::max(0.f, std::min(100.f, o2)));
    }

    msg.U8((uint8_t)std::min<size_t>(ship->vSystemList.size(), 255));
    for (size_t i = 0; i < ship->vSystemList.size() && i < 255; ++i)
    {
        ShipSystem *sys = ship->vSystemList[i];
        msg.U8((uint8_t)sys->roomId).U8((uint8_t)sys->iSystemType)
           .U8((uint8_t)std::max(0, sys->healthState.first)).U8((uint8_t)std::max(0, sys->healthState.second))
           .U8((uint8_t)std::max(0, sys->powerState.first));
    }

    Coop::GetNet().SendToAll(msg.Data(), msg.Size(), -1, false);
    lastSnapshot = Clock::now();
}

// Pawns

void Assign(int key, uint32_t crewId)
{
    Pawn& pawn = pawns[key];
    pawn.crewId = crewId;
    pawn.goalTx = pawn.goalTy = -1;

    Coop::Writer msg(Coop::MSG_PAWN_ASSIGN);
    msg.U32(crewId);
    Coop::GetNet().SendTo(Coop::ClientPeer(key), msg.Data(), msg.Size());

    CrewMember *crew = FindCrew(crewId);
    if (crew) Coop::Log("Player " + std::to_string(key + 1) + " now controls " + crew->GetName());
}

// The captain's own crew that nobody else controls, in vCrewList order
std::vector<CrewMember*> FreeCrew(int key)
{
    std::vector<CrewMember*> list;
    ShipManager *ship = G_->GetShipManager(0);
    for (CrewMember *crew : ship->vCrewList)
    {
        if (!crew || crew->bDead || crew->bOutOfGame || crew->currentShipId != 0) continue;
        int owner = OwnerOf(CrewId(crew));
        if (owner < 0 || owner == key) list.push_back(crew);
    }
    return list;
}

void SelectPawn(int key, int step)
{
    std::vector<CrewMember*> free = FreeCrew(key);
    if (free.empty())
    {
        Assign(key, 0);
        return;
    }

    uint32_t current = pawns.count(key) ? pawns[key].crewId : 0;
    int index = -1;
    for (size_t i = 0; i < free.size(); ++i)
    {
        if (CrewId(free[i]) == current) index = (int)i;
    }

    if (step == 0 && index >= 0) return Assign(key, current);
    if (index < 0) index = step < 0 ? 0 : -1;
    index = ((index + step) % (int)free.size() + (int)free.size()) % (int)free.size();
    Assign(key, CrewId(free[index]));
}

bool StepFrom(int tx, int ty, int sx, int sy, int& outX, int& outY)
{
    auto can = [&](int x, int y, int dx, int dy)
    {
        if (dx == 0 && dy == 0) return false;
        if (dx != 0 && dy != 0)
        {
            // Diagonal steps stay inside one room
            int room = layout.RoomAt(x, y);
            return room >= 0 && layout.RoomAt(x + dx, y + dy) == room && layout.RoomAt(x + dx, y) == room &&
                   layout.RoomAt(x, y + dy) == room;
        }
        int dir = dx > 0 ? DIR_X_PLUS : dx < 0 ? DIR_X_MINUS : dy > 0 ? DIR_Y_PLUS : DIR_Y_MINUS;
        return layout.CanStep(x, y, dir);
    };

    const int options[3][2] = {{sx, sy}, {sx, 0}, {0, sy}};
    for (auto& o : options)
    {
        if (can(tx, ty, o[0], o[1]))
        {
            outX = tx + o[0];
            outY = ty + o[1];
            return true;
        }
    }
    return false;
}

void ToggleFacingDoor(CrewMember *crew, float yaw)
{
    float fx = std::cos(yaw), fy = std::sin(yaw);
    Door *best = nullptr;
    float bestScore = 0.f;
    for (size_t i = 0; i < layout.doors.size(); ++i)
    {
        const LayoutDoor& d = layout.doors[i];
        float mx = layout.originX + (d.tx + 0.5f + DirX(d.dir) * 0.5f) * TILE;
        float my = layout.originY + (d.ty + 0.5f + DirY(d.dir) * 0.5f) * TILE;
        float dx = mx - crew->x, dy = my - crew->y;
        float dist = std::hypot(dx, dy);
        if (dist > TILE * 1.4f || dist < 0.01f) continue;
        float facing = (dx * fx + dy * fy) / dist;
        float score = facing - dist / (TILE * 4.f);
        if (facing > 0.4f && score > bestScore)
        {
            bestScore = score;
            best = layoutDoors[i];
        }
    }
    if (!best) return;
    if (best->bOpen) best->Close();
    else best->Open();
}

void DrivePawn(int key, Pawn& pawn)
{
    CrewMember *crew = FindCrew(pawn.crewId);
    if (!crew)
    {
        if (pawn.crewId) SelectPawn(key, 0);
        return;
    }

    int tx = layout.TileX(crew->x), ty = layout.TileY(crew->y);
    float len = std::hypot(pawn.dirX, pawn.dirY);
    if (len < 0.3f)
    {
        pawn.goalTx = pawn.goalTy = -1;
        return;
    }

    // The next step starts when the crew member has (nearly) reached the last one
    if (pawn.goalTx >= 0)
    {
        float gx = layout.originX + (pawn.goalTx + 0.5f) * TILE;
        float gy = layout.originY + (pawn.goalTy + 0.5f) * TILE;
        if (std::hypot(gx - crew->x, gy - crew->y) > ARRIVED && (pawn.goalTx != tx || pawn.goalTy != ty)) return;
    }

    float nx = pawn.dirX / len, ny = pawn.dirY / len;
    int sx = nx > 0.38f ? 1 : nx < -0.38f ? -1 : 0;
    int sy = ny > 0.38f ? 1 : ny < -0.38f ? -1 : 0;

    int gx, gy;
    if (!StepFrom(tx, ty, sx, sy, gx, gy)) return;

    int roomIndex = layout.RoomAt(gx, gy);
    const LayoutRoom& room = layout.rooms[roomIndex];
    int localX = gx - (room.x - layout.originX) / TILE;
    int localY = gy - (room.y - layout.originY) / TILE;
    int slot = localX + (room.w / TILE) * localY;

    drivingPawn = true;
    bool moved = crew->MoveToRoom(room.id, slot, true);
    drivingPawn = false;
    if (moved)
    {
        pawn.goalTx = gx;
        pawn.goalTy = gy;
    }
}

}

namespace Coop
{
namespace CrewHost
{

// A player joining while the captain's run is under way boards straight away
void OnPeerConnected(int peerId)
{
    if (!Active()) return;
    Writer msg(MSG_START_RUN);
    msg.U32(0).U8((uint8_t)*Global::difficulty).U8(G_->GetSettings()->bDlcEnabled ? 1 : 0).U8(MODE_CREW);
    GetNet().SendTo(peerId, msg.Data(), msg.Size());
}

void OnPeerDisconnected(int peerId)
{
    pawns.erase(ClientKey(peerId));
}

void OnMessage(int peerId, uint8_t type, const uint8_t *payload, size_t size)
{
    if (GetNet().GetRole() != Role::Host) return;
    Reader in(payload, size);
    int key = ClientKey(peerId);

    switch (type)
    {
    case MSG_PAWN_INPUT:
    {
        int dx = (int8_t)in.U8();
        int dy = (int8_t)in.U8();
        uint16_t yaw = in.U16();
        uint8_t presses = in.U8();
        if (!in.ok) break;

        Pawn& pawn = pawns[key];
        pawn.dirX = dx / 100.f;
        pawn.dirY = dy / 100.f;
        pawn.yaw = yaw / 65536.f * 2.f * PI;

        if (pawn.doorPressesKnown && presses != pawn.doorPresses && Active())
        {
            CrewMember *crew = FindCrew(pawn.crewId);
            if (crew) ToggleFacingDoor(crew, pawn.yaw);
        }
        pawn.doorPresses = presses;
        pawn.doorPressesKnown = true;
        break;
    }
    case MSG_PAWN_SELECT:
    {
        int step = (int8_t)in.U8();
        if (!in.ok || !Active()) break;
        if (layout.Valid()) SendLayout();
        SelectPawn(key, step < 0 ? -1 : step > 0 ? 1 : 0);
        break;
    }
    default:
        break;
    }
}

void OnLoop()
{
    if (!Active())
    {
        layoutGraph = nullptr;
        for (auto& entry : pawns) entry.second.goalTx = entry.second.goalTy = -1;
        return;
    }

    auto now = Clock::now();
    ShipGraph *graph = ShipGraph::GetShipInfo(0);
    uint32_t oldId = layout.id;
    if (graph != layoutGraph || now - lastLayout >= LAYOUT_INTERVAL)
    {
        BuildLayout();
        if (layout.id != oldId)
        {
            size_t ftlDoors = graph ? graph->doors.size() : 0;
            hs_log_file("[coop] Crew layout: %d rooms, %d of %d doors placed, %dx%d tiles\n", (int)layout.rooms.size(),
                        (int)layout.doors.size(), (int)ftlDoors, layout.tilesW, layout.tilesH);
        }
        SendLayout();
    }

    for (auto& entry : pawns) DrivePawn(entry.first, entry.second);

    if (now - lastSnapshot >= SNAPSHOT_INTERVAL) SendSnapshot();
}

}
}

// The captain cannot order crew members that players control
HOOK_METHOD(CrewMember, MoveToRoom, (int roomId, int slotId, bool forceMove) -> bool)
{
    LOG_HOOK("HOOK_METHOD -> CrewMember::MoveToRoom -> Begin (CoopCrewHost.cpp)\n")
    if (!drivingPawn && IsPawn(this)) return false;
    return super(roomId, slotId, forceMove);
}

// Label the players' crew members on the captain's screen
HOOK_METHOD(CrewMember, OnRender, (bool outlineOnly) -> void)
{
    LOG_HOOK("HOOK_METHOD -> CrewMember::OnRender -> Begin (CoopCrewHost.cpp)\n")
    super(outlineOnly);
    if (outlineOnly || !IsPawn(this)) return;

    int owner = OwnerOf(CrewId(this));
    CSurface::GL_SetColor(GL_Color(1.f, 0.85f, 0.2f, 1.f));
    freetype::easy_printCenter(10, x, y - 30.f, "P" + std::to_string(owner + 1));
    CSurface::GL_SetColor(COLOR_WHITE);
}
