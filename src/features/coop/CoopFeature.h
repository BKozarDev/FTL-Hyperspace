#pragma once

#include "features/coop/CoopNet.h"
#include <cstddef>
#include <cstdint>
#include <string>

namespace Coop
{

// The network layer with the game-side callbacks wired up
Net& GetNet();

// Logs to FTL_HS.log and the on-screen message list
void Log(const std::string& message);

namespace Lobby
{
    void OnPeerConnected(int peerId);
    void OnPeerDisconnected(int peerId);
    void OnMessage(int peerId, uint8_t type, const uint8_t *payload, size_t size);
    void OnLoop();
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
}

}
