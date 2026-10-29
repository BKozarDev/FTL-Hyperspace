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

}
