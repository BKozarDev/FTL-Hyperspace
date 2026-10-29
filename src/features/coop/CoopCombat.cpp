#include "Global.h"
#include "features/coop/CoopFeature.h"
#include "features/coop/CoopProtocol.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <ctime>
#include <map>
#include <random>

// One enemy for everyone. The same seed, beacon and choices give every game the
// same enemy (ship generation is seeded per event), so each player fights a copy:
// - the host's copy is the real one: clients report what their hits took off the
//   hull and systems, the host applies it and sends its enemy's state, and clients
//   only ever lower their copy towards it
// - the host picks a living player for each enemy weapon; only that player's copy
//   may fire it, the other copies hold it charged. After a volley the host picks
//   again and the other copies restart that weapon's charge, so it keeps one rate.

namespace
{

using Clock = std::chrono::steady_clock;

constexpr auto STATE_INTERVAL = std::chrono::milliseconds(100);
constexpr auto PRESENCE_INTERVAL = std::chrono::milliseconds(500);
constexpr auto PRESENCE_TIMEOUT = std::chrono::milliseconds(1500);
constexpr auto HOST_STATE_TIMEOUT = std::chrono::seconds(1);
constexpr auto VOLLEY_WINDOW = std::chrono::milliseconds(1500); // burst shots after the first
constexpr float HOLD_GRACE = 6.f; // seconds past a full charge before a silent target is replaced

int assignedKey = -1;

// Host
ShipManager *hostEnemy = nullptr;
std::vector<int> targets;
std::vector<Clock::time_point> targetSince;
std::map<int, Clock::time_point> presence; // client key -> last time it fought the host's enemy
Clock::time_point lastStateSent;
std::mt19937 rng((uint32_t)time(nullptr));

// Client
struct HostEnemy
{
    bool valid = false;
    Clock::time_point received;
    std::string blueprint;
    int worldLevel = 0;
    int locX = 0, locY = 0;
    int hull = 0;
    std::map<int, int> systemHealth;
    std::vector<int> targets;
};
HostEnemy hostState;
bool matched = false;
ShipManager *clientEnemy = nullptr;
int lastHull = 0;
std::map<int, int> lastSystemHealth;
Clock::time_point lastPresenceSent;

// Both
std::map<int, Clock::time_point> volleyStart;

Coop::Net& Session()
{
    return Coop::GetNet();
}

bool IsHost()
{
    return Session().GetRole() == Coop::Role::Host && Session().IsConnected();
}

bool IsClient()
{
    return Session().GetRole() == Coop::Role::Client && Session().IsConnected();
}

ShipManager *Enemy()
{
    if (!Coop::InRun()) return nullptr;
    return G_->GetShipManager(1);
}

bool EnemyAlive(ShipManager *enemy)
{
    return enemy && !enemy->bDestroyed && enemy->ship.hullIntegrity.first > 0;
}

std::vector<ProjectileFactory*> *EnemyWeapons(ShipManager *enemy)
{
    return enemy && enemy->weaponSystem ? &enemy->weaponSystem->weapons : nullptr;
}

int WeaponIndex(ShipManager *enemy, ProjectileFactory *weapon)
{
    auto *weapons = EnemyWeapons(enemy);
    if (!weapons) return -1;
    auto it = std::find(weapons->begin(), weapons->end(), weapon);
    return it == weapons->end() ? -1 : (int)(it - weapons->begin());
}

void CurrentLocation(int& level, int& x, int& y)
{
    StarMap& map = G_->GetWorld()->starMap;
    level = map.worldLevel;
    x = map.currentLoc ? (int)std::lround(map.currentLoc->loc.x) : 0;
    y = map.currentLoc ? (int)std::lround(map.currentLoc->loc.y) : 0;
}

// Combat sync applies: the host has an enemy, or a client fights the host's enemy
bool CombatActive()
{
    if (IsHost()) return Enemy() != nullptr;
    if (IsClient()) return matched;
    return false;
}

int TargetOf(int weapon)
{
    const std::vector<int>& list = IsHost() ? targets : hostState.targets;
    return weapon < (int)list.size() ? list[weapon] : Coop::TARGET_NONE;
}

void RestartCharge(ShipManager *enemy, int weapon)
{
    auto *weapons = EnemyWeapons(enemy);
    if (!weapons || weapon >= (int)weapons->size()) return;
    ProjectileFactory *w = (*weapons)[weapon];
    w->cooldown.first = 0.f;
    w->chargeLevel = 0;
}

// Host

std::vector<int> Candidates()
{
    std::vector<int> keys;
    if (!Coop::Revive::IsWrecked()) keys.push_back(Coop::HOST_KEY);

    auto now = Clock::now();
    for (int key : Coop::Ally::AliveKeys())
    {
        auto it = presence.find(key);
        if (it != presence.end() && now - it->second < PRESENCE_TIMEOUT) keys.push_back(key);
    }
    return keys;
}

void PickTarget(int weapon, const std::vector<int>& candidates)
{
    targets[weapon] = candidates.empty() ? Coop::TARGET_NONE : candidates[rng() % candidates.size()];
    targetSince[weapon] = Clock::now();
}

void SendEnemyState(ShipManager *enemy)
{
    int level, x, y;
    CurrentLocation(level, x, y);

    Coop::Writer msg(Coop::MSG_ENEMY_STATE);
    msg.Str(enemy->myBlueprint.blueprintName).U32((uint32_t)level).U32((uint32_t)x).U32((uint32_t)y)
       .U32((uint32_t)std::max(0, enemy->ship.hullIntegrity.first));

    msg.U8((uint8_t)std::min<size_t>(enemy->vSystemList.size(), 255));
    for (size_t i = 0; i < enemy->vSystemList.size() && i < 255; ++i)
    {
        ShipSystem *sys = enemy->vSystemList[i];
        msg.U8((uint8_t)sys->iSystemType).U8((uint8_t)std::max(0, sys->healthState.first));
    }

    msg.U8((uint8_t)std::min<size_t>(targets.size(), 255));
    for (size_t i = 0; i < targets.size() && i < 255; ++i) msg.U8((uint8_t)targets[i]);

    Session().SendToAll(msg.Data(), msg.Size());
    lastStateSent = Clock::now();
}

void HostLoop()
{
    ShipManager *enemy = Enemy();
    if (enemy != hostEnemy)
    {
        hostEnemy = enemy;
        targets.clear();
        targetSince.clear();
        volleyStart.clear();
    }
    if (!enemy) return;

    auto *weapons = EnemyWeapons(enemy);
    size_t count = weapons ? weapons->size() : 0;
    if (targets.size() != count)
    {
        targets.resize(count, Coop::TARGET_NONE);
        targetSince.resize(count, Clock::now());
    }

    // Replace targets that died, left, or stay silent long past a full charge
    std::vector<int> candidates = Candidates();
    auto now = Clock::now();
    bool changed = false;
    for (size_t i = 0; i < count; ++i)
    {
        bool valid = std::find(candidates.begin(), candidates.end(), targets[i]) != candidates.end();
        float waited = std::chrono::duration<float>(now - targetSince[i]).count();
        bool silent = targets[i] != Coop::HOST_KEY && waited > (*weapons)[i]->cooldown.second + HOLD_GRACE;
        if (!valid || silent)
        {
            int before = targets[i];
            PickTarget(i, candidates);
            changed |= targets[i] != before;
        }
    }

    if (changed || now - lastStateSent >= STATE_INTERVAL) SendEnemyState(enemy);
}

void HostOnFired(int weapon, int shooter)
{
    ShipManager *enemy = Enemy();
    if (!enemy || weapon >= (int)targets.size()) return;

    if (shooter != Coop::HOST_KEY) RestartCharge(enemy, weapon);
    PickTarget(weapon, Candidates());

    Coop::Writer msg(Coop::MSG_WEAPON_FIRED);
    msg.U8((uint8_t)weapon).U8((uint8_t)shooter);
    Session().SendToAll(msg.Data(), msg.Size());
    SendEnemyState(enemy);
}

void HostOnHit(int hullDamage, const std::vector<std::pair<int, int>>& systems)
{
    ShipManager *enemy = Enemy();
    if (!EnemyAlive(enemy)) return;

    if (hullDamage > 0) enemy->DamageHull(hullDamage, true);
    for (auto& hit : systems)
    {
        ShipSystem *sys = enemy->GetSystem(hit.first);
        if (sys && hit.second > 0) sys->AddDamage(hit.second);
    }
}

// Client

void Snapshot(ShipManager *enemy)
{
    lastSystemHealth.clear();
    if (!enemy)
    {
        lastHull = 0;
        return;
    }
    lastHull = enemy->ship.hullIntegrity.first;
    for (ShipSystem *sys : enemy->vSystemList) lastSystemHealth[sys->iSystemType] = sys->healthState.first;
}

bool MatchesHost(ShipManager *enemy)
{
    if (!enemy || !hostState.valid || Clock::now() - hostState.received > HOST_STATE_TIMEOUT) return false;

    int level, x, y;
    CurrentLocation(level, x, y);
    return hostState.blueprint == enemy->myBlueprint.blueprintName && hostState.worldLevel == level &&
           std::abs(hostState.locX - x) <= 1 && std::abs(hostState.locY - y) <= 1;
}

void ClientLoop()
{
    ShipManager *enemy = Enemy();
    if (enemy != clientEnemy)
    {
        clientEnemy = enemy;
        volleyStart.clear();
        Snapshot(enemy);
    }

    bool wasMatched = matched;
    matched = MatchesHost(enemy);
    if (matched && !wasMatched) Coop::Log("Fighting the same enemy as the host");
    if (!matched && wasMatched && enemy) Coop::Log("Lost the host's enemy, fighting alone");

    auto now = Clock::now();
    if (now - lastPresenceSent >= PRESENCE_INTERVAL)
    {
        lastPresenceSent = now;
        Coop::Writer msg(Coop::MSG_ENEMY_PRESENCE);
        msg.U8(matched ? 1 : 0);
        Session().SendToAll(msg.Data(), msg.Size());
    }

    if (!matched)
    {
        Snapshot(enemy);
        return;
    }

    // 1. What this player's own attack took off since the last frame
    int hull = enemy->ship.hullIntegrity.first;
    int hullDamage = std::max(0, lastHull - hull);
    std::vector<std::pair<int, int>> systemDamage;
    for (ShipSystem *sys : enemy->vSystemList)
    {
        auto it = lastSystemHealth.find(sys->iSystemType);
        if (it != lastSystemHealth.end() && it->second > sys->healthState.first)
        {
            systemDamage.emplace_back(sys->iSystemType, it->second - sys->healthState.first);
        }
    }
    if (hullDamage > 0 || !systemDamage.empty())
    {
        Coop::Writer msg(Coop::MSG_ENEMY_HIT);
        msg.U32((uint32_t)hullDamage).U8((uint8_t)systemDamage.size());
        for (auto& hit : systemDamage) msg.U8((uint8_t)hit.first).U8((uint8_t)hit.second);
        Session().SendToAll(msg.Data(), msg.Size());
    }

    // 2. Move down to the host's enemy: damage from the others
    if (hostState.hull < hull) enemy->DamageHull(hull - hostState.hull, true);
    for (ShipSystem *sys : enemy->vSystemList)
    {
        auto it = hostState.systemHealth.find(sys->iSystemType);
        if (it != hostState.systemHealth.end() && it->second < sys->healthState.first)
        {
            sys->AddDamage(sys->healthState.first - it->second);
        }
    }

    // 3. Only changes after this point count as this player's own
    Snapshot(enemy);
}

}

int Coop::MyKey()
{
    switch (GetNet().GetRole())
    {
    case Role::Host: return HOST_KEY;
    case Role::Client: return assignedKey;
    default: return -1;
    }
}

namespace Coop
{
namespace Combat
{

void OnPeerConnected(int peerId)
{
    if (GetNet().GetRole() != Role::Host) return;
    Writer msg(MSG_ASSIGN_KEY);
    msg.U8((uint8_t)ClientKey(peerId));
    GetNet().SendTo(peerId, msg.Data(), msg.Size());
}

void OnPeerDisconnected(int peerId)
{
    presence.erase(ClientKey(peerId));
    if (GetNet().GetRole() != Role::Host)
    {
        assignedKey = -1;
        hostState = HostEnemy();
        matched = false;
    }
}

void OnMessage(int peerId, uint8_t type, const uint8_t *payload, size_t size)
{
    Reader in(payload, size);
    bool host = GetNet().GetRole() == Role::Host;

    switch (type)
    {
    case MSG_ASSIGN_KEY:
    {
        uint8_t key = in.U8();
        if (in.ok && !host) assignedKey = key;
        break;
    }
    case MSG_ENEMY_STATE:
    {
        if (host) break;
        HostEnemy state;
        state.blueprint = in.Str();
        state.worldLevel = (int32_t)in.U32();
        state.locX = (int32_t)in.U32();
        state.locY = (int32_t)in.U32();
        state.hull = (int32_t)in.U32();
        int systems = in.U8();
        for (int i = 0; i < systems; ++i)
        {
            int sysType = in.U8();
            state.systemHealth[sysType] = in.U8();
        }
        int weapons = in.U8();
        for (int i = 0; i < weapons; ++i) state.targets.push_back(in.U8());
        if (!in.ok) break;

        state.valid = true;
        state.received = Clock::now();
        hostState = state;
        break;
    }
    case MSG_ENEMY_PRESENCE:
    {
        bool fighting = in.U8() != 0;
        if (!in.ok || !host) break;
        if (fighting) presence[ClientKey(peerId)] = Clock::now();
        else presence.erase(ClientKey(peerId));
        break;
    }
    case MSG_ENEMY_HIT:
    {
        int hullDamage = (int32_t)in.U32();
        int systems = in.U8();
        std::vector<std::pair<int, int>> systemDamage;
        for (int i = 0; i < systems; ++i)
        {
            int sysType = in.U8();
            systemDamage.emplace_back(sysType, in.U8());
        }
        if (in.ok && host) HostOnHit(hullDamage, systemDamage);
        break;
    }
    case MSG_WEAPON_FIRED:
    {
        int weapon = in.U8();
        int shooter = in.U8();
        if (!in.ok) break;
        if (host)
        {
            HostOnFired(weapon, ClientKey(peerId));
        }
        else if (shooter != MyKey() && matched)
        {
            RestartCharge(Enemy(), weapon);
        }
        break;
    }
    default:
        break;
    }
}

void OnLoop()
{
    if (!InRun() || !GetNet().IsConnected())
    {
        hostEnemy = nullptr;
        clientEnemy = nullptr;
        matched = false;
        targets.clear();
        volleyStart.clear();
        return;
    }

    if (IsHost()) HostLoop();
    else if (IsClient()) ClientLoop();
}

}
}

// Enemy weapons fire only on the copy of the player they are aimed at.
// A wrecked ship's own weapons stay quiet.
HOOK_METHOD(ProjectileFactory, FireNextShot, () -> bool)
{
    LOG_HOOK("HOOK_METHOD -> ProjectileFactory::FireNextShot -> Begin (CoopCombat.cpp)\n")
    if (iShipId == 0) return Coop::Revive::IsWrecked() ? false : super();
    if (iShipId != 1 || !CombatActive()) return super();

    ShipManager *enemy = Enemy();
    int weapon = WeaponIndex(enemy, this);
    if (weapon < 0) return super();

    auto now = Clock::now();
    auto volley = volleyStart.find(weapon);
    bool inVolley = volley != volleyStart.end() && now - volley->second < VOLLEY_WINDOW;
    if (!inVolley && TargetOf(weapon) != Coop::MyKey()) return false;

    bool fired = super();
    if (fired && !inVolley)
    {
        volleyStart[weapon] = now;
        if (IsHost())
        {
            HostOnFired(weapon, Coop::HOST_KEY);
        }
        else
        {
            Coop::Writer msg(Coop::MSG_WEAPON_FIRED);
            msg.U8((uint8_t)weapon).U8((uint8_t)Coop::MyKey());
            Session().SendToAll(msg.Data(), msg.Size());
        }
    }
    return fired;
}
