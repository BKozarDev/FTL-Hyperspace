#include "Global.h"
#include "features/coop/CoopFeature.h"
#include "features/coop/CoopProtocol.h"
#include <algorithm>

// Losing the ship does not end the run while an ally lives: the hull stops at 1 and
// the ship is wrecked. A wrecked ship takes no damage, cannot shoot, and enemies do
// not aim at it; it still follows the host's jumps. A surviving player repairs it for
// scrap after a victory at the current beacon or at a store; it comes back with half
// its hull. When every player is wrecked the host ends the run for everyone.

namespace
{

bool wrecked = false;
bool forcingDeath = false;

// Victory tracking for the current beacon
const Location *trackedLoc = nullptr;
bool sawHostileEnemy = false;
bool victoryHere = false;

bool Connected()
{
    return Coop::GetNet().IsConnected();
}

ShipManager *Me()
{
    return Coop::InRun() ? G_->GetShipManager(0) : nullptr;
}

void SetWrecked(bool value)
{
    if (wrecked == value) return;
    wrecked = value;

    ShipManager *me = Me();
    if (value)
    {
        Coop::Log("Your ship is wrecked. An ally can repair it after a victory or at a store.");
    }
    else if (me)
    {
        me->ship.hullIntegrity.first = std::max(1, me->ship.hullIntegrity.second / 2);
        Coop::Log("An ally repaired your ship");
    }
}

// The real loss: let the hull reach zero and FTL end the run
void ForceGameOver()
{
    ShipManager *me = Me();
    wrecked = false;
    if (!me) return;

    Coop::Log("Every ship is wrecked, the run is over");
    forcingDeath = true;
    me->DamageHull(me->ship.hullIntegrity.first + 100, true);
    forcingDeath = false;
}

void TrackVictory()
{
    StarMap& map = G_->GetWorld()->starMap;
    if (map.currentLoc != trackedLoc)
    {
        trackedLoc = map.currentLoc;
        sawHostileEnemy = false;
        victoryHere = false;
    }

    ShipManager *enemy = G_->GetShipManager(1);
    bool alive = enemy && !enemy->bDestroyed && enemy->ship.hullIntegrity.first > 0;
    if (alive && enemy->hostile_ship) sawHostileEnemy = true;
    // Destroyed, or gone (fled or surrendered and let go) after being fought here
    if (sawHostileEnemy && (!alive || !enemy->hostile_ship)) victoryHere = true;
}

void ApplyRevive()
{
    if (!wrecked) return;
    SetWrecked(false);
}

}

namespace Coop
{
namespace Revive
{

bool IsWrecked()
{
    return wrecked && InRun();
}

int Cost()
{
    int sector = InRun() ? G_->GetWorld()->starMap.worldLevel + 1 : 1;
    return 40 + 15 * sector;
}

bool CanReviveHere()
{
    if (!InRun()) return false;
    StarMap& map = G_->GetWorld()->starMap;
    bool store = map.currentLoc && map.currentLoc->event && map.currentLoc->event->store;
    return victoryHere || store;
}

void RequestRevive(int key)
{
    ShipManager *me = Me();
    if (!me || wrecked || !Connected()) return;
    if (!CanReviveHere())
    {
        Log("Repairs are possible after a victory or at a store");
        return;
    }

    int cost = Cost();
    if (me->currentScrap < cost)
    {
        Log("Repairing your ally needs " + std::to_string(cost) + " scrap");
        return;
    }
    me->ModifyScrapCount(-cost, false);

    Writer msg(MSG_REVIVE);
    msg.U8((uint8_t)key).U8((uint8_t)MyKey());
    if (GetNet().GetRole() == Role::Host) GetNet().SendTo(ClientPeer(key), msg.Data(), msg.Size());
    else GetNet().SendToAll(msg.Data(), msg.Size());
    Log("Repairing your ally for " + std::to_string(cost) + " scrap");
}

void OnPeerDisconnected(int peerId)
{
    (void)peerId;
}

void OnMessage(int peerId, uint8_t type, const uint8_t *payload, size_t size)
{
    Reader in(payload, size);

    switch (type)
    {
    case MSG_REVIVE:
    {
        int target = in.U8();
        int reviver = in.U8();
        if (!in.ok) break;

        if (GetNet().GetRole() == Role::Host)
        {
            if (target == HOST_KEY)
            {
                ApplyRevive();
            }
            else if (ClientPeer(target) != peerId)
            {
                Writer msg(MSG_REVIVE);
                msg.U8((uint8_t)target).U8((uint8_t)reviver);
                GetNet().SendTo(ClientPeer(target), msg.Data(), msg.Size());
            }
        }
        else if (target == MyKey())
        {
            ApplyRevive();
        }
        break;
    }
    case MSG_GAME_OVER:
    {
        if (GetNet().GetRole() == Role::Client && InRun()) ForceGameOver();
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
        wrecked = false;
        trackedLoc = nullptr;
        return;
    }

    TrackVictory();

    ShipManager *me = Me();
    if (!me) return;

    // Hull lost by a path that skips DamageHull
    if (!wrecked && !forcingDeath && Connected() && me->ship.hullIntegrity.first <= 0 && !Ally::AliveKeys().empty())
    {
        me->ship.hullIntegrity.first = 1;
        SetWrecked(true);
    }
    if (wrecked) me->ship.hullIntegrity.first = std::max(1, me->ship.hullIntegrity.first);

    if (!wrecked) return;

    // Nobody left to repair this ship
    if (!Connected())
    {
        ForceGameOver();
        return;
    }
    if (GetNet().GetRole() == Role::Host && Ally::AliveKeys().empty())
    {
        Writer msg(MSG_GAME_OVER);
        GetNet().SendToAll(msg.Data(), msg.Size());
        ForceGameOver();
    }
}

}
}

HOOK_METHOD(ShipManager, DamageHull, (int dmg, bool force) -> int)
{
    LOG_HOOK("HOOK_METHOD -> ShipManager::DamageHull -> Begin (CoopRevive.cpp)\n")
    if (iShipId != 0 || forcingDeath || !Connected() || !Coop::InRun()) return super(dmg, force);

    // No damage and no repair while wrecked: only an ally brings the ship back
    if (wrecked) return super(0, force);

    int hull = ship.hullIntegrity.first;
    if (dmg > 0 && hull - dmg <= 0 && !Coop::Ally::AliveKeys().empty())
    {
        int ret = super(std::max(0, hull - 1), force);
        SetWrecked(true);
        return ret;
    }
    return super(dmg, force);
}

// Hull lost by other paths is caught before the ship's own loop can destroy it
HOOK_METHOD(ShipManager, OnLoop, () -> void)
{
    LOG_HOOK("HOOK_METHOD -> ShipManager::OnLoop -> Begin (CoopRevive.cpp)\n")
    if (iShipId == 0 && !forcingDeath && Connected() && Coop::InRun() && ship.hullIntegrity.first <= 0 &&
        (wrecked || !Coop::Ally::AliveKeys().empty()))
    {
        ship.hullIntegrity.first = 1;
        SetWrecked(true);
    }
    super();
}

HOOK_METHOD(WorldManager, Restart, () -> void)
{
    LOG_HOOK("HOOK_METHOD -> WorldManager::Restart -> Begin (CoopRevive.cpp)\n")
    wrecked = false;
    trackedLoc = nullptr;
    super();
}
