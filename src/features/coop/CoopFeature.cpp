#include "Global.h"
#include "CommandConsole.h"
#include "features/coop/CoopFeature.h"
#include <boost/algorithm/string.hpp>
#include <ctime>
#include <fstream>
#include <sstream>

// Co-op prototype: connects game instances over ENet.
// Sessions are normally made in the hangar lobby (CoopLobby.cpp); these console
// commands (press \ in a run) are for testing:
//   COOP_HOST [port]          host a session
//   COOP_JOIN <ip>[:port]     join a host
//   COOP_PING                 ping connected players
//   COOP_STATUS               show the session state
//   COOP_LEAVE                close the session

// Two game copies on one PC share FTL_HS.log, so each role also gets its own file
static void AppendRoleLog(const std::string& message)
{
    const char *name;
    switch (Coop::Net::GetInstance().GetRole())
    {
    case Coop::Role::Host: name = "hs_coop_host.log"; break;
    case Coop::Role::Client: name = "hs_coop_client.log"; break;
    default: name = "hs_coop.log"; break;
    }

    std::string folder = FileHelper::getUserFolder();
    if (!folder.empty() && folder.back() != '/' && folder.back() != '\\') folder += '/';

    char stamp[16];
    time_t now = time(nullptr);
    strftime(stamp, sizeof(stamp), "%H:%M:%S", localtime(&now));

    std::ofstream file(folder + name, std::ios::app);
    file << stamp << " " << message << "\n";
}

void Coop::Log(const std::string& message)
{
    hs_log_file("[coop] %s\n", message.c_str());
    AppendRoleLog(message);
    PrintHelper::GetInstance()->AddMessage("[coop] " + message);
}

Coop::Net& Coop::GetNet()
{
    Coop::Net& net = Coop::Net::GetInstance();
    // Set lazily: static initialization order across files is unspecified
    if (!net.onLog)
    {
        net.onLog = Coop::Log;
        net.onPeerConnected = Coop::Lobby::OnPeerConnected;
        net.onPeerDisconnected = [](int peerId)
        {
            Coop::Lobby::OnPeerDisconnected(peerId);
            Coop::Ally::OnPeerDisconnected(peerId);
        };
        net.onMessage = [](int peerId, uint8_t type, const uint8_t *payload, size_t size)
        {
            Coop::Lobby::OnMessage(peerId, type, payload, size);
            Coop::Run::OnMessage(peerId, type, payload, size);
            Coop::Ally::OnMessage(peerId, type, payload, size);
        };
    }
    return net;
}

using Coop::GetNet;

static void CoopLog(const std::string& message)
{
    Coop::Log(message);
}

static bool ParsePort(const std::string& text, uint16_t& port)
{
    try
    {
        int value = std::stoi(text);
        if (value < 1 || value > 65535) return false;
        port = (uint16_t)value;
        return true;
    }
    catch (const std::exception&)
    {
        return false;
    }
}

// Returns true if the command was a co-op command
static bool RunCoopCommand(const std::string& command)
{
    std::istringstream args(command);
    std::string cmdName;
    args >> cmdName;
    boost::to_upper(cmdName);

    if (cmdName.rfind("COOP_", 0) != 0) return false;

    Coop::Net& net = GetNet();
    std::string error;

    if (cmdName == "COOP_HOST")
    {
        uint16_t port = Coop::DEFAULT_PORT;
        std::string portText;
        if (args >> portText && !ParsePort(portText, port))
        {
            CoopLog("Bad port: " + portText);
            return true;
        }
        if (!net.Host(port, error)) CoopLog("Host failed: " + error);
    }
    else if (cmdName == "COOP_JOIN")
    {
        std::string address;
        if (!(args >> address))
        {
            CoopLog("Usage: COOP_JOIN <ip>[:port]");
            return true;
        }

        uint16_t port = Coop::DEFAULT_PORT;
        size_t colon = address.rfind(':');
        if (colon != std::string::npos)
        {
            std::string portText = address.substr(colon + 1);
            address = address.substr(0, colon);
            if (!ParsePort(portText, port))
            {
                CoopLog("Bad port: " + portText);
                return true;
            }
        }
        if (!net.Join(address, port, error)) CoopLog("Join failed: " + error);
    }
    else if (cmdName == "COOP_PING")
    {
        net.SendPing();
    }
    else if (cmdName == "COOP_STATUS")
    {
        CoopLog(net.Status());
    }
    else if (cmdName == "COOP_LEAVE")
    {
        net.Leave();
    }
    else
    {
        CoopLog("Unknown command " + cmdName + ". Commands: COOP_HOST, COOP_JOIN, COOP_PING, COOP_STATUS, COOP_LEAVE");
    }
    return true;
}

HOOK_METHOD(CommandGui, RunCommand, (std::string& command) -> void)
{
    LOG_HOOK("HOOK_METHOD -> CommandGui::RunCommand -> Begin (CoopFeature.cpp)\n")
    if (!RunCoopCommand(command))
    {
        super(command);
    }
}

HOOK_METHOD(CApp, OnLoop, () -> void)
{
    LOG_HOOK("HOOK_METHOD -> CApp::OnLoop -> Begin (CoopFeature.cpp)\n")
    super();
    GetNet().Poll();
    Coop::Lobby::OnLoop();
    Coop::Run::OnLoop();
    Coop::Ally::OnLoop();
}
