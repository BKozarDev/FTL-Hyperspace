#include "Global.h"
#include "features/coop/CoopFeature.h"
#include "features/coop/CoopProtocol.h"
#include <chrono>
#include <cmath>
#include <ctime>
#include <deque>
#include <random>

// Keeps a co-op run in step once it has started from the lobby:
// - the host leads jumps: when its StarMap hands out a new location, the beacon or
//   sector is sent to the clients, which jump there the way Hyperspace forces the
//   boss jump; clients cannot start jumps of their own while connected
// - when the host restarts the run, the clients restart with the same seed
// - the host picks event choices: the clients' event windows take the same choice
// - pausing (space) on any side pauses everyone

namespace
{

int lastSelectedSector = -1;
const Location *lastSentTarget = nullptr;
int lastSentLevel = -1;
bool wasPaused = false;
bool seedHookInstalled = false;

bool pendingRestart = false;
uint32_t pendingRestartSeed = 0;
bool followingRestart = false;

struct PendingChoice
{
    int index;
    std::string text;
    std::chrono::steady_clock::time_point received;
};
constexpr auto CHOICE_TIMEOUT = std::chrono::seconds(20);
std::deque<PendingChoice> pendingChoices;
// A choice goes to an event window only once: wait for it to close or change
std::string appliedToText;
bool eventWindowClosedSinceApply = true;

bool InRun()
{
    CApp *app = G_->GetCApp();
    return app && !app->menu.bOpen && app->world && app->gui;
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

// The window of text events, not other choice boxes such as dialogs
ChoiceBox *EventWindow()
{
    CApp *app = G_->GetCApp();
    return app && app->gui ? &app->gui->choiceBox : nullptr;
}

void SendHostChoice(ChoiceBox& box)
{
    int index = box.selectedChoice;
    std::string text = index < (int)box.choices.size() ? box.choices[index].text : "";

    Coop::Writer msg(Coop::MSG_EVENT_CHOICE);
    msg.U32((uint32_t)index).Str(text);
    Coop::GetNet().SendToAll(msg.Data(), msg.Size());
}

// The host's choice by its text (blue options differ between ships), else by position
int MatchChoice(const ChoiceBox& box, const PendingChoice& choice)
{
    int count = (int)box.choices.size();
    for (int i = 0; i < count; ++i)
    {
        if (!choice.text.empty() && box.choices[i].text == choice.text) return i;
    }
    if (count == 0) return -1;
    return choice.index < count ? choice.index : count - 1;
}

void ApplyPendingChoices()
{
    ChoiceBox *box = EventWindow();
    bool open = G_->GetCApp()->gui->choiceBoxOpen;
    if (!open) eventWindowClosedSinceApply = true;

    auto now = std::chrono::steady_clock::now();
    while (!pendingChoices.empty() && now - pendingChoices.front().received > CHOICE_TIMEOUT)
    {
        Coop::Log("Dropped the host's event choice, no event window appeared");
        pendingChoices.pop_front();
    }
    if (pendingChoices.empty() || !open || box->selectedChoice >= 0) return;
    if (!eventWindowClosedSinceApply && box->mainText == appliedToText) return;

    int index = MatchChoice(*box, pendingChoices.front());
    pendingChoices.pop_front();
    if (index < 0) return;

    box->potentialChoice = index;
    box->selectedChoice = index;
    appliedToText = box->mainText;
    eventWindowClosedSinceApply = false;
}

uint32_t RandomSeed()
{
    std::mt19937 rng((uint32_t)time(nullptr) ^ (uint32_t)clock() ^ (uint32_t)(uintptr_t)&rng);
    return rng();
}

void SendHostJump(bool sectorJump, const Location *target, int worldLevel)
{
    Coop::Writer msg(Coop::MSG_JUMP);
    if (sectorJump)
    {
        msg.U8(Coop::JUMP_SECTOR).U32((uint32_t)lastSelectedSector);
        Coop::Log("Jumping to the next sector, the others follow");
    }
    else if (target)
    {
        msg.U8(Coop::JUMP_BEACON).U32((uint32_t)(int32_t)std::lround(target->loc.x)).U32((uint32_t)(int32_t)std::lround(target->loc.y));
        Coop::Log("Jumping, the others follow");
    }
    else
    {
        return;
    }
    msg.U32((uint32_t)worldLevel);
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

// Lets the C++ side pick the run seed through Hyperspace's GET_RUN_SEED event
void InstallSeedHook()
{
    if (seedHookInstalled) return;
    seedHookInstalled = true;

    Global::GetInstance()->getLuaContext()->runLuaString(R"(
        script.on_internal_event(Defines.InternalEvents.GET_RUN_SEED, function(isCustomSeed, seed)
            local forced = _G.hs_coop_next_seed
            if forced ~= nil then
                _G.hs_coop_next_seed = nil
                return isCustomSeed, forced
            end
        end)
    )");
}

}

namespace Coop
{
namespace Run
{

void ForceNextSeed(uint32_t seed)
{
    InstallSeedHook();
    Global::GetInstance()->getLuaContext()->runLuaString("_G.hs_coop_next_seed = " + std::to_string(seed));
}

void OnMessage(int peerId, uint8_t type, const uint8_t *payload, size_t size)
{
    Reader in(payload, size);

    switch (type)
    {
    case MSG_JUMP:
    {
        uint8_t kind = in.U8();
        int a = (int32_t)in.U32();
        int b = kind == JUMP_BEACON ? (int32_t)in.U32() : 0;
        int worldLevel = (int32_t)in.U32();
        if (!in.ok || GetNet().GetRole() != Role::Client) break;
        if (!InRun())
        {
            Log("Got the host's jump, but you are not in a run");
            break;
        }

        if (kind == JUMP_BEACON) FollowBeaconJump(a, b, worldLevel);
        else if (kind == JUMP_SECTOR) FollowSectorJump(a, worldLevel);
        break;
    }
    case MSG_EVENT_CHOICE:
    {
        PendingChoice choice;
        choice.index = (int32_t)in.U32();
        choice.text = in.Str();
        choice.received = std::chrono::steady_clock::now();
        if (in.ok && GetNet().GetRole() == Role::Client && InRun()) pendingChoices.push_back(choice);
        break;
    }
    case MSG_RESTART:
    {
        uint32_t seed = in.U32();
        if (!in.ok || GetNet().GetRole() != Role::Client) break;
        // Restarted from OnLoop, not from inside the network poll
        pendingRestart = true;
        pendingRestartSeed = seed;
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
    if (pendingRestart)
    {
        pendingRestart = false;
        if (InRun())
        {
            Log("Host restarted the run, seed " + std::to_string(pendingRestartSeed));
            ForceNextSeed(pendingRestartSeed);
            followingRestart = true;
            G_->GetWorld()->Restart();
            followingRestart = false;
        }
        else
        {
            Log("Host restarted the run, but you are not in a run");
        }
    }

    if (!InRun())
    {
        wasPaused = false;
        lastSentTarget = nullptr;
        lastSentLevel = -1;
        pendingChoices.clear();
        eventWindowClosedSinceApply = true;
        return;
    }

    if (IsFollower()) ApplyPendingChoices();
    else pendingChoices.clear();

    // The jump has been taken, the next one is new even if it reuses the same location
    if (!Map().readyToTravel)
    {
        lastSentTarget = nullptr;
        lastSentLevel = -1;
    }

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

// The host's map hands out the location it is jumping to: tell the clients
HOOK_METHOD(StarMap, GetNewLocation, () -> Location*)
{
    LOG_HOOK("HOOK_METHOD -> StarMap::GetNewLocation -> Begin (CoopRun.cpp)\n")
    // In crew mode the others have no ship of their own to move
    if (!readyToTravel || !IsLeader() || Coop::Lobby::CrewMode()) return super();

    bool sectorJump = potentialLoc == &dummyNewSector;
    Location *target = potentialLoc;
    int level = worldLevel;

    Location *ret = super();
    // Sent once per jump, even if the game asks again
    if (target != lastSentTarget || level != lastSentLevel)
    {
        lastSentTarget = target;
        lastSentLevel = level;
        SendHostJump(sectorJump, target, level);
    }
    return ret;
}

HOOK_METHOD(WorldManager, Restart, () -> void)
{
    LOG_HOOK("HOOK_METHOD -> WorldManager::Restart -> Begin (CoopRun.cpp)\n")
    if (IsLeader() && !followingRestart && !Coop::Lobby::CrewMode())
    {
        uint32_t seed = RandomSeed();
        Coop::Run::ForceNextSeed(seed);
        lastSentTarget = nullptr;
        lastSentLevel = -1;

        Coop::Writer msg(Coop::MSG_RESTART);
        msg.U32(seed);
        Coop::GetNet().SendToAll(msg.Data(), msg.Size());
        Coop::Log("Restarting the co-op run, seed " + std::to_string(seed));
    }

    super();
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

// The host picks event choices; on clients the event window waits for the host
HOOK_METHOD(ChoiceBox, MouseClick, (int x, int y) -> void)
{
    LOG_HOOK("HOOK_METHOD -> ChoiceBox::MouseClick -> Begin (CoopRun.cpp)\n")
    if (this != EventWindow() || !InRun()) return super(x, y);

    if (IsFollower())
    {
        Coop::Log("The host picks the choice");
        return;
    }

    int before = selectedChoice;
    super(x, y);
    if (IsLeader() && !Coop::Lobby::CrewMode() && selectedChoice >= 0 && selectedChoice != before) SendHostChoice(*this);
}

HOOK_METHOD(ChoiceBox, KeyDown, (SDLKey key) -> bool)
{
    LOG_HOOK("HOOK_METHOD -> ChoiceBox::KeyDown -> Begin (CoopRun.cpp)\n")
    if (this != EventWindow() || !InRun()) return super(key);

    // Swallowed, so number keys cannot pick a choice either
    if (IsFollower()) return true;

    int before = selectedChoice;
    bool ret = super(key);
    if (IsLeader() && !Coop::Lobby::CrewMode() && selectedChoice >= 0 && selectedChoice != before) SendHostChoice(*this);
    return ret;
}
