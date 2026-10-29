#pragma once

// Co-op networking layer. Kept free of ENet and FTL headers: ENet pulls in
// winsock2.h, which must not meet the windows.h that Global.h includes.

#include <cstdint>
#include <functional>
#include <string>

struct _ENetHost;
struct _ENetPeer;

namespace Coop
{

constexpr uint16_t DEFAULT_PORT = 47515;
constexpr uint32_t PROTOCOL_VERSION = 1;

enum class Role
{
    None,
    Host,
    Client
};

class Net
{
public:
    static Net& GetInstance()
    {
        return _instance;
    }

    bool Host(uint16_t port, std::string& error);
    bool Join(const std::string& address, uint16_t port, std::string& error);
    void Leave();

    // Services the connection; call once per frame
    void Poll();

    // Pings every connected peer, the reply is logged with its round trip time
    void SendPing();

    Role GetRole() const
    {
        return role;
    }
    bool IsConnected() const;
    std::string Status() const;

    std::function<void(const std::string&)> onLog;

private:
    enum MsgType : uint8_t
    {
        MSG_HELLO = 1,
        MSG_PING = 2,
        MSG_PONG = 3
    };

    bool Init(std::string& error);
    void Log(const std::string& message);
    void Send(_ENetPeer *peer, const uint8_t *data, size_t size);
    void Broadcast(const uint8_t *data, size_t size);
    void SendHello(_ENetPeer *peer);
    void SendPingTo(_ENetPeer *peer, bool verbose);
    void HandlePacket(_ENetPeer *peer, const uint8_t *data, size_t size);

    static std::string PeerName(_ENetPeer *peer);

    bool initialized = false;
    Role role = Role::None;
    _ENetHost *host = nullptr;
    _ENetPeer *server = nullptr; // Client only: the connection to the host
    int connectedPeers = 0;
    uint32_t pingSeq = 0;
    uint32_t lastPingTime = 0;
    uint32_t lastRtt = 0;

    static Net _instance;
};

}
