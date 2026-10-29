#include "Global.h"
#include "CommandConsole.h"
#include "features/coop/CoopNet.h"
#include <boost/algorithm/string.hpp>
#include <sstream>

// Co-op prototype: connects game instances over ENet.
// Console commands (press \ in a run):
//   COOP_HOST [port]          host a session
//   COOP_JOIN <ip>[:port]     join a host
//   COOP_PING                 ping connected players
//   COOP_STATUS               show the session state
//   COOP_LEAVE                close the session

static void CoopLog(const std::string& message)
{
    hs_log_file("[coop] %s\n", message.c_str());
    PrintHelper::GetInstance()->AddMessage("[coop] " + message);
}

static Coop::Net& GetNet()
{
    Coop::Net& net = Coop::Net::GetInstance();
    // Set lazily: static initialization order across files is unspecified
    if (!net.onLog) net.onLog = CoopLog;
    return net;
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
}
