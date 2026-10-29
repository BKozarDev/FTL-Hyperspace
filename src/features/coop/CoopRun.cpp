#include "Global.h"
#include "features/coop/CoopFeature.h"
#include "features/coop/CoopProtocol.h"
#include <cmath>

// Keeps a co-op run in step once it has started from the lobby:
// - the host leads jumps: a beacon or sector it jumps to is sent to the clients,
//   which jump there the same way Hyperspace forces the boss jump;
//   clients cannot start jumps of their own while connected
// - pausing (space) on any side pauses everyone

namespace
{

bool wasReadyToTravel = false;
int lastSelectedSector = -1;
bool wasPaused = false;

bool InRun()
{
    CApp *app = G_->GetCApp();
    return app && !app->menu.bOpen && app->world && app->world->bStartedGame && app->gui;
}

bool IsFollower()
{
    Coop::Net& net = Coop::GetNet();
    return net.GetRole() == Coop::Role::Client && net.IsConnected();
}

bool IsLeader()
{
    Coop::Net& net = Coop::GetNet();
    return net.GetRole() == Coop::Role::Host && net.IsConnected();
}

StarMap& Map()
{
    return G_->GetWorld()->starMap;
}

void SendHostJump(StarMap& map)
{
    Coop::Writer msg(Coop::MSG_JUMP);
    if (map.potentialLoc == &map.dummyNewSector)
    {
        msg.U8(Coop::JUMP_SECTOR).U32((uint32_t)lastSelectedSector);
        Coop::Log("Jumping to the next sector, the others follow");
    }
    else if (map.potentialLoc)
    {
        msg.U8(Coop::JUMP_BEACON).U32((uint32_t)(int32_t)std::lround(map.potentialLoc->loc.x)).U32((uint32_t)(int32_t)std::lround(map.potentialLoc->loc.y));
    }
    else
    {
        return;
    }
    msg.U32((uint32_t)map.worldLevel);
    Coop::GetNet().SendToAll(msg.Data(), msg.Size());
}

// Same as Hyperspace's forced boss jump
void StartJump(StarMap& map, Location *dest)
{
    map.potentialLoc = dest;
    map.readyToTravel = true;
    map.outOfFuel = false;
    G_->GetCApp()->gui->waitLocation = false;
    if (map.bOpen) map.Close();
}

void FollowBeaconJump(int x, int y, int worldLevel)
{
    StarMap& map = Map();
    if (map.worldLevel != worldLevel)
    {
        Coop::Log("Host jumped in sector " + std::to_string(worldLevel + 1) + ", you are in " + std::to_string(map.worldLevel + 1));
        return;
    }

    for (Location *loc : map.locations)
    {
        if (std::abs(loc->loc.x - x) <= 1.f && std::abs(loc->loc.y - y) <= 1.f)
        {
            if (loc == map.currentLoc) return;
            Coop::Log("Following the host's jump");
            StartJump(map, loc);
            return;
        }
    }
    Coop::Log("Could not find the host's beacon, the maps differ");
}

void FollowSectorJump(int sector, int worldLevel)
{
    StarMap& map = Map();
    // The host has already advanced its world level
    if (map.worldLevel + 1 != worldLevel)
    {
        Coop::Log("Host entered sector " + std::to_string(worldLevel + 1) + ", you are in " + std::to_string(map.worldLevel + 1));
        return;
    }
    if (sector < 0 || !map.NeighboringSector(sector))
    {
        Coop::Log("Could not follow the host to the next sector");
        return;
    }

    Coop::Log("Following the host to the next sector");
    // Mirrors what the sector map does when the player picks a sector
    map.bChoosingNewSector = true;
    map.SelectNewSector(sector);
    map.AdvanceWorldLevel();
    map.finalSectorChoice = -1;
    StartJump(map, &map.dummyNewSector);
}

}

namespace Coop
{
namespace Run
{

void OnMessage(int peerId, uint8_t type, const uint8_t *payload, size_t size)
{
    Reader in(payload, size);

    switch (type)
    {
    case MSG_JUMP:
    {
        uint8_t kind = in.U8();
        if (kind == JUMP_BEACON)
        {
            int x = (int32_t)in.U32();
            int y = (int32_t)in.U32();
            int worldLevel = (int32_t)in.U32();
            if (in.ok && InRun() && GetNet().GetRole() == Role::Client) FollowBeaconJump(x, y, worldLevel);
        }
        else if (kind == JUMP_SECTOR)
        {
            int sector = (int32_t)in.U32();
            int worldLevel = (int32_t)in.U32();
            if (in.ok && InRun() && GetNet().GetRole() == Role::Client) FollowSectorJump(sector, worldLevel);
        }
        break;
    }
    case MSG_PAUSE:
    {
        bool paused = in.U8() != 0;
        if (!in.ok || !InRun()) break;

        CommandGui *gui = G_->GetCApp()->gui;
        if (gui->bPaused != paused) gui->SetPaused(paused, false);
        // Not an own change, so do not send it back
        wasPaused = gui->bPaused;

        // A host relays a client's pause to the other clients
        if (GetNet().GetRole() == Role::Host)
        {
            Writer msg(MSG_PAUSE);
            msg.U8(paused ? 1 : 0);
            GetNet().SendToAll(msg.Data(), msg.Size(), peerId);
        }
        break;
    }
    default:
        break;
    }
}

void OnLoop()
{
    if (!InRun())
    {
        wasReadyToTravel = false;
        wasPaused = false;
        return;
    }

    StarMap& map = Map();
    if (IsLeader() && map.readyToTravel && !wasReadyToTravel)
    {
        SendHostJump(map);
    }
    wasReadyToTravel = map.readyToTravel;

    bool paused = G_->GetCApp()->gui->bPaused;
    if (paused != wasPaused)
    {
        wasPaused = paused;
        if (GetNet().IsConnected())
        {
            Writer msg(MSG_PAUSE);
            msg.U8(paused ? 1 : 0);
            GetNet().SendToAll(msg.Data(), msg.Size());
        }
    }
}

}
}

HOOK_METHOD(StarMap, SelectNewSector, (int sector) -> void)
{
    LOG_HOOK("HOOK_METHOD -> StarMap::SelectNewSector -> Begin (CoopRun.cpp)\n")
    lastSelectedSector = sector;
    super(sector);
}

// Clients may look at the map, but the host picks where everyone jumps
HOOK_METHOD(StarMap, MouseClick, (int x, int y) -> void)
{
    LOG_HOOK("HOOK_METHOD -> StarMap::MouseClick -> Begin (CoopRun.cpp)\n")
    if (!IsFollower()) return super(x, y);

    bool wasReady = readyToTravel;
    int wasSectorChoice = finalSectorChoice;
    super(x, y);

    if (readyToTravel && !wasReady)
    {
        readyToTravel = false;
        Coop::Log("The host chooses where to jump");
    }
    if (finalSectorChoice != wasSectorChoice && finalSectorChoice >= 0)
    {
        finalSectorChoice = -1;
        Coop::Log("The host chooses the next sector");
    }
}

HOOK_METHOD(StarMap, KeyDown, (int key) -> void)
{
    LOG_HOOK("HOOK_METHOD -> StarMap::KeyDown -> Begin (CoopRun.cpp)\n")
    if (!IsFollower()) return super(key);

    bool wasReady = readyToTravel;
    int wasSectorChoice = finalSectorChoice;
    super(key);

    if (readyToTravel && !wasReady)
    {
        readyToTravel = false;
        Coop::Log("The host chooses where to jump");
    }
    if (finalSectorChoice != wasSectorChoice && finalSectorChoice >= 0)
    {
        finalSectorChoice = -1;
        Coop::Log("The host chooses the next sector");
    }
}
