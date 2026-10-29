// ENet must come first: it includes winsock2.h, which breaks once windows.h is in
#include <enet/enet.h>

#include "CoopNet.h"
#include <cstdlib>
#include <cstring>

namespace Coop
{

Net Net::_instance = Net();

static constexpr size_t MAX_CLIENTS = 3;
static constexpr size_t CHANNELS = 2;
static constexpr uint32_t AUTO_PING_INTERVAL = 2000; // ms

static void WriteU32(uint8_t *out, uint32_t value)
{
    out[0] = value & 0xFF;
    out[1] = (value >> 8) & 0xFF;
    out[2] = (value >> 16) & 0xFF;
    out[3] = (value >> 24) & 0xFF;
}

static uint32_t ReadU32(const uint8_t *in)
{
    return in[0] | (in[1] << 8) | (in[2] << 16) | ((uint32_t)in[3] << 24);
}

bool Net::Init(std::string& error)
{
    if (initialized) return true;

    if (enet_initialize() != 0)
    {
        error = "enet_initialize failed";
        return false;
    }
    atexit(enet_deinitialize);
    initialized = true;
    return true;
}

void Net::Log(const std::string& message)
{
    if (onLog) onLog(message);
}

std::string Net::PeerName(ENetPeer *peer)
{
    char ip[64] = "?";
    enet_address_get_host_ip(&peer->address, ip, sizeof(ip));
    return std::string(ip) + ":" + std::to_string(peer->address.port);
}

bool Net::Host(uint16_t port, std::string& error)
{
    if (role != Role::None)
    {
        error = "already in a co-op session, use COOP_LEAVE first";
        return false;
    }
    if (!Init(error)) return false;

    ENetAddress address;
    address.host = ENET_HOST_ANY;
    address.port = port;

    host = enet_host_create(&address, MAX_CLIENTS, CHANNELS, 0, 0);
    if (!host)
    {
        error = "could not open port " + std::to_string(port) + " (already in use?)";
        return false;
    }

    role = Role::Host;
    connectedPeers = 0;
    Log("Hosting on port " + std::to_string(port));
    return true;
}

bool Net::Join(const std::string& addressText, uint16_t port, std::string& error)
{
    if (role != Role::None)
    {
        error = "already in a co-op session, use COOP_LEAVE first";
        return false;
    }
    if (!Init(error)) return false;

    ENetAddress address;
    if (enet_address_set_host(&address, addressText.c_str()) != 0)
    {
        error = "unknown address " + addressText;
        return false;
    }
    address.port = port;

    host = enet_host_create(nullptr, 1, CHANNELS, 0, 0);
    if (!host)
    {
        error = "could not create client socket";
        return false;
    }

    server = enet_host_connect(host, &address, CHANNELS, PROTOCOL_VERSION);
    if (!server)
    {
        enet_host_destroy(host);
        host = nullptr;
        error = "no free peer to connect with";
        return false;
    }

    role = Role::Client;
    connectedPeers = 0;
    Log("Connecting to " + addressText + ":" + std::to_string(port) + "...");
    return true;
}

void Net::Leave()
{
    if (role == Role::None) return;

    for (size_t i = 0; i < host->peerCount; ++i)
    {
        ENetPeer *peer = &host->peers[i];
        if (peer->state != ENET_PEER_STATE_DISCONNECTED)
        {
            enet_peer_disconnect_now(peer, 0);
        }
    }
    enet_host_flush(host);
    enet_host_destroy(host);

    host = nullptr;
    server = nullptr;
    role = Role::None;
    connectedPeers = 0;
    lastRtt = 0;
    Log("Left the co-op session");
}

bool Net::IsConnected() const
{
    return connectedPeers > 0;
}

std::string Net::Status() const
{
    switch (role)
    {
    case Role::Host:
        return "Hosting, " + std::to_string(connectedPeers) + " player(s) connected";
    case Role::Client:
        if (!IsConnected()) return "Connecting...";
        return "Connected to host, ping " + std::to_string(lastRtt) + " ms";
    default:
        return "Not in a co-op session";
    }
}

void Net::Send(ENetPeer *peer, const uint8_t *data, size_t size)
{
    ENetPacket *packet = enet_packet_create(data, size, ENET_PACKET_FLAG_RELIABLE);
    enet_peer_send(peer, 0, packet);
}

void Net::Broadcast(const uint8_t *data, size_t size)
{
    ENetPacket *packet = enet_packet_create(data, size, ENET_PACKET_FLAG_RELIABLE);
    enet_host_broadcast(host, 0, packet);
}

void Net::SendHello(ENetPeer *peer)
{
    uint8_t msg[5];
    msg[0] = MSG_HELLO;
    WriteU32(msg + 1, PROTOCOL_VERSION);
    Send(peer, msg, sizeof(msg));
}

void Net::SendPingTo(ENetPeer *peer, bool verbose)
{
    uint8_t msg[10];
    msg[0] = MSG_PING;
    WriteU32(msg + 1, ++pingSeq);
    WriteU32(msg + 5, enet_time_get());
    msg[9] = verbose ? 1 : 0;
    Send(peer, msg, sizeof(msg));
}

void Net::SendPing()
{
    if (!IsConnected())
    {
        Log("Nobody to ping");
        return;
    }

    for (size_t i = 0; i < host->peerCount; ++i)
    {
        ENetPeer *peer = &host->peers[i];
        if (peer->state == ENET_PEER_STATE_CONNECTED)
        {
            SendPingTo(peer, true);
        }
    }
}

void Net::HandlePacket(ENetPeer *peer, const uint8_t *data, size_t size)
{
    if (size < 1) return;

    switch (data[0])
    {
    case MSG_HELLO:
    {
        if (size < 5) return;
        uint32_t version = ReadU32(data + 1);
        if (version != PROTOCOL_VERSION)
        {
            Log("Protocol mismatch with " + PeerName(peer) + ": theirs " + std::to_string(version) + ", ours " + std::to_string(PROTOCOL_VERSION));
            enet_peer_disconnect(peer, 0);
            return;
        }
        if (role == Role::Host)
        {
            SendHello(peer);
        }
        else
        {
            Log("Handshake with host OK");
        }
        break;
    }
    case MSG_PING:
    {
        if (size < 10) return;
        // Echo back unchanged, the sender measures the round trip
        uint8_t reply[10];
        std::memcpy(reply, data, sizeof(reply));
        reply[0] = MSG_PONG;
        Send(peer, reply, sizeof(reply));
        if (data[9]) Log("Ping from " + PeerName(peer));
        break;
    }
    case MSG_PONG:
    {
        if (size < 10) return;
        uint32_t seq = ReadU32(data + 1);
        uint32_t rtt = enet_time_get() - ReadU32(data + 5);
        lastRtt = rtt;
        if (data[9]) Log("Pong #" + std::to_string(seq) + " from " + PeerName(peer) + ": " + std::to_string(rtt) + " ms");
        break;
    }
    default:
        Log("Unknown message " + std::to_string(data[0]) + " from " + PeerName(peer));
        break;
    }
}

void Net::Poll()
{
    if (!host) return;

    ENetEvent event;
    while (host && enet_host_service(host, &event, 0) > 0)
    {
        switch (event.type)
        {
        case ENET_EVENT_TYPE_CONNECT:
            ++connectedPeers;
            if (role == Role::Host)
            {
                Log("Player connected: " + PeerName(event.peer));
            }
            else
            {
                Log("Connected to host " + PeerName(event.peer));
                SendHello(event.peer);
                lastPingTime = enet_time_get();
            }
            break;

        case ENET_EVENT_TYPE_RECEIVE:
            HandlePacket(event.peer, event.packet->data, event.packet->dataLength);
            enet_packet_destroy(event.packet);
            break;

        case ENET_EVENT_TYPE_DISCONNECT:
            if (role == Role::Host)
            {
                if (connectedPeers > 0) --connectedPeers;
                Log("Player disconnected: " + PeerName(event.peer));
            }
            else
            {
                bool wasConnected = IsConnected();
                connectedPeers = 0;
                Log(wasConnected ? "Lost connection to host" : "Could not connect to host");
                enet_host_destroy(host);
                host = nullptr;
                server = nullptr;
                role = Role::None;
            }
            break;

        default:
            break;
        }
    }

    // Keep the round trip time fresh for COOP_STATUS
    if (role == Role::Client && server && IsConnected() && enet_time_get() - lastPingTime >= AUTO_PING_INTERVAL)
    {
        lastPingTime = enet_time_get();
        SendPingTo(server, false);
    }
}

}
