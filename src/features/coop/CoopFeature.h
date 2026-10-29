#pragma once

#include "features/coop/CoopNet.h"
#include "features/coop/CoopProtocol.h"
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace Coop
{

// The network layer with the game-side callbacks wired up
Net& GetNet();

// Logs to FTL_HS.log and the on-screen message list
void Log(const std::string& message);

// Players are numbered by the host: the host is 0, a client is its peer id + 1.
// -1 until a client has been told its number.
constexpr int HOST_KEY = 0;
int MyKey();
inline int ClientKey(int peerId)
{
    return peerId + 1;
}
inline int ClientPeer(int key)
{
    return key - 1;
}

// True in a run (not in the menus or the hangar)
bool InRun();

namespace Lobby
{
    void OnPeerConnected(int peerId);
    void OnPeerDisconnected(int peerId);
    void OnMessage(int peerId, uint8_t type, const uint8_t *payload, size_t size);
    void OnLoop();

    // The host's game mode (MODE_FLEET or MODE_CREW) for this session
    uint8_t Mode();
    inline bool CrewMode()
    {
        return Mode() == MODE_CREW && GetNet().IsConnected();
    }
}

// Keeps a started run in step: host-led jumps, restarts and shared pause
namespace Run
{
    void OnMessage(int peerId, uint8_t type, const uint8_t *payload, size_t size);
    void OnLoop();

    // The next run (new game or restart) uses this seed instead of a random one
    void ForceNextSeed(uint32_t seed);
}

// Allies' ships, seen from outside next to your own
namespace Ally
{
    void OnPeerDisconnected(int peerId);
    void OnMessage(int peerId, uint8_t type, const uint8_t *payload, size_t size);
    void OnLoop();

    // Allies (by player key) whose ship is neither destroyed nor wrecked
    std::vector<int> AliveKeys();
    // Allies (by player key) whose ship is wrecked and can be repaired
    std::vector<int> WreckedKeys();
}

// One enemy for everyone: the host's copy decides, hits on any copy count,
// and the host picks which player each enemy weapon shoots at
namespace Combat
{
    void OnPeerConnected(int peerId);
    void OnPeerDisconnected(int peerId);
    void OnMessage(int peerId, uint8_t type, const uint8_t *payload, size_t size);
    void OnLoop();
}

// Crew mode, host side: sends the ship's layout and state, walks the crew members
// the other players control, and keeps the captain's hands off them
namespace CrewHost
{
    void OnPeerConnected(int peerId);
    void OnPeerDisconnected(int peerId);
    void OnMessage(int peerId, uint8_t type, const uint8_t *payload, size_t size);
    void OnLoop();
}

// Crew mode, client side: the full screen first-person view of a crew member
namespace CrewView
{
    void Enter();
    void Leave();
    bool Active();
    void OnMessage(int peerId, uint8_t type, const uint8_t *payload, size_t size);
    void OnLoop();
}

// A ship at zero hull is wrecked instead of lost while an ally lives;
// the survivor repairs it for scrap after a victory or at a store
namespace Revive
{
    void OnPeerDisconnected(int peerId);
    void OnMessage(int peerId, uint8_t type, const uint8_t *payload, size_t size);
    void OnLoop();

    bool IsWrecked();
    int Cost();
    // Victory at this beacon, or a store here
    bool CanReviveHere();
    void RequestRevive(int key);
}

}
