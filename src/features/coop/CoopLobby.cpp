#include "Global.h"
#include "Seeds.h"
#include "features/coop/CoopFeature.h"
#include "features/coop/CoopProtocol.h"
#include <boost/lexical_cast.hpp>
#include <chrono>
#include <ctime>
#include <fstream>
#include <map>
#include <random>

// Co-op lobby in the hangar: a CO-OP button opens a window to host or join.
// Every player picks a ship; only the host can press START, which starts the run
// for everyone with the same seed.

namespace
{

struct RemotePlayer
{
    bool inHangar = false;
    std::string blueprint;
    std::string shipName;
};

struct Box
{
    int x, y, w, h;

    bool Contains(Point p) const
    {
        return p.x >= x && p.x < x + w && p.y >= y && p.y < y + h;
    }
};

const Box TOGGLE_BOX = {1079, 100, 158, 24}; // Under Hyperspace's seed box
const Box WINDOW_BOX = {440, 170, 400, 320};
const Box HOST_BOX = {WINDOW_BOX.x + 20, WINDOW_BOX.y + 70, 360, 28};
const Box IP_BOX = {WINDOW_BOX.x + 20, WINDOW_BOX.y + 132, 250, 28};
const Box JOIN_BOX = {WINDOW_BOX.x + 280, WINDOW_BOX.y + 132, 100, 28};
const Box MODE_BOX = {WINDOW_BOX.x + 20, WINDOW_BOX.y + 232, 360, 28};
const Box LEAVE_BOX = {WINDOW_BOX.x + 20, WINDOW_BOX.y + 274, 170, 28};
const Box CLOSE_BOX = {WINDOW_BOX.x + 210, WINDOW_BOX.y + 274, 170, 28};

constexpr auto STATE_RESEND = std::chrono::seconds(1);

std::map<int, RemotePlayer> players;
bool windowOpen = false;
TextInput *ipInput = nullptr;
Point mouse = Point(0, 0);

bool sentInHangar = false;
std::string sentBlueprint;
std::string sentShipName;
std::chrono::steady_clock::time_point lastStateSent;
bool forceStateSend = false;

bool pendingStart = false;
uint32_t pendingSeed = 0;
bool simulatingStart = false;

// Host's difficulty (0 easy, 1 normal, 2 hard) and Advanced Edition switch
struct LobbySettings
{
    int difficulty = 1;
    bool advancedEdition = true;
    uint8_t mode = Coop::MODE_FLEET;

    bool operator!=(const LobbySettings& other) const
    {
        return difficulty != other.difficulty || advancedEdition != other.advancedEdition || mode != other.mode;
    }
};
uint8_t hostMode = Coop::MODE_FLEET; // chosen in the window when hosting
LobbySettings sentSettings;
LobbySettings hostSettings;
bool haveHostSettings = false;

ShipBuilder& Builder()
{
    return G_->GetCApp()->menu.shipBuilder;
}

std::string LastIpFile()
{
    std::string folder = FileHelper::getUserFolder();
    if (!folder.empty() && folder.back() != '/' && folder.back() != '\\') folder += '/';
    return folder + "hs_coop_last_ip.txt";
}

std::string LoadLastIp()
{
    std::ifstream file(LastIpFile());
    std::string ip;
    std::getline(file, ip);
    return ip;
}

void SaveLastIp(const std::string& ip)
{
    std::ofstream file(LastIpFile(), std::ios::trunc);
    file << ip << "\n";
}

TextInput *IpInput()
{
    if (!ipInput)
    {
        // FTL draws the prompt in front of the text, so the placeholder is drawn by hand
        ipInput = new TextInput(64, TextInput::AllowedCharType::ALLOW_ANY, "");
        ipInput->SetText(LoadLastIp());
    }
    return ipInput;
}

bool TextActive()
{
    return ipInput && ipInput->GetActive();
}

uint32_t PickSeed()
{
    // Keep a seed the host typed in
    std::string typed = SeedInputBox::seedInput->GetText();
    if (!typed.empty())
    {
        try
        {
            return boost::lexical_cast<uint32_t>(typed);
        }
        catch (const boost::bad_lexical_cast&)
        {
        }
    }

    std::mt19937 rng((uint32_t)time(nullptr) ^ (uint32_t)clock() ^ (uint32_t)(uintptr_t)&rng);
    return rng();
}

// Only valid in the hangar: once a run starts the builder's ship belongs to the world
std::string LocalBlueprint()
{
    ShipBuilder& builder = Builder();
    if (!builder.bOpen || !builder.currentShip) return sentBlueprint;
    return builder.currentShip->myBlueprint.blueprintName;
}

std::string Describe(const std::string& shipName, const std::string& blueprint)
{
    if (shipName.empty()) return blueprint.empty() ? "?" : blueprint;
    return shipName + " (" + blueprint + ")";
}

void SendLocalState(bool inHangar)
{
    ShipBuilder& builder = Builder();
    Coop::Writer msg(Coop::MSG_LOBBY_STATE);
    msg.U8(inHangar ? 1 : 0).Str(LocalBlueprint()).Str(builder.currentName);
    Coop::GetNet().SendToAll(msg.Data(), msg.Size());

    sentInHangar = inHangar;
    sentBlueprint = LocalBlueprint();
    sentShipName = builder.currentName;
    lastStateSent = std::chrono::steady_clock::now();
    forceStateSend = false;
}

LobbySettings LocalSettings()
{
    LobbySettings settings;
    settings.difficulty = *Global::difficulty;
    settings.advancedEdition = G_->GetSettings()->bDlcEnabled;
    settings.mode = hostMode;
    return settings;
}

void SendSettings()
{
    sentSettings = LocalSettings();
    Coop::Writer msg(Coop::MSG_LOBBY_SETTINGS);
    msg.U8((uint8_t)sentSettings.difficulty).U8(sentSettings.advancedEdition ? 1 : 0).U8(sentSettings.mode);
    Coop::GetNet().SendToAll(msg.Data(), msg.Size());
}

// Same changes the hangar buttons make (a selected button is inactive)
void ApplySettings(const LobbySettings& settings)
{
    ShipBuilder& builder = Builder();
    if (*Global::difficulty != settings.difficulty)
    {
        *Global::difficulty = settings.difficulty;
        builder.easyButton.SetActive(settings.difficulty != 0);
        builder.normalButton.SetActive(settings.difficulty != 1);
        builder.hardButton.SetActive(settings.difficulty != 2);
    }
    if (G_->GetSettings()->bDlcEnabled != settings.advancedEdition)
    {
        G_->GetSettings()->bDlcEnabled = settings.advancedEdition;
        if (builder.currentShip) builder.currentShip->CheckDlcEnabled();
        builder.advancedOffButton.SetActive(settings.advancedEdition);
        builder.advancedOnButton.SetActive(!settings.advancedEdition);
    }
}

template <class Button>
bool Hit(const Button& button, int x, int y)
{
    const Globals::Rect& r = button.hitbox;
    return button.bActive && x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}

// Clicks START the way a player would, so the run starts through FTL's own path
void PressStart()
{
    ShipBuilder& builder = Builder();
    windowOpen = false;
    if (ipInput) ipInput->Stop();

    Globals::Rect hitbox = builder.startButton.hitbox;
    int x = hitbox.x + hitbox.w / 2;
    int y = hitbox.y + hitbox.h / 2;

    simulatingStart = true;
    builder.MouseMove(x, y);
    builder.MouseClick(x, y);
    simulatingStart = false;
}

// Returns false to block the click
bool OnStartClicked()
{
    Coop::Net& net = Coop::GetNet();

    if (net.GetRole() == Coop::Role::Client)
    {
        Coop::Log("Only the host can start the run");
        return false;
    }
    if (net.GetRole() != Coop::Role::Host || players.empty()) return true;

    for (auto& entry : players)
    {
        if (!entry.second.inHangar)
        {
            Coop::Log("Waiting for " + Describe(entry.second.shipName, entry.second.blueprint) + " to open the hangar");
            return false;
        }
    }
    if (!SeedInputBox::seedsEnabled)
    {
        Coop::Log("Seeds are disabled by a mod, the maps will not match");
    }

    uint32_t seed = PickSeed();
    Coop::Run::ForceNextSeed(seed);

    LobbySettings settings = LocalSettings();
    Coop::Writer msg(Coop::MSG_START_RUN);
    msg.U32(seed).U8((uint8_t)settings.difficulty).U8(settings.advancedEdition ? 1 : 0).U8(settings.mode);
    net.SendToAll(msg.Data(), msg.Size());
    Coop::Log(std::string(settings.mode == Coop::MODE_CREW ? "Starting the crew run" : "Starting the co-op run") + ", seed " + std::to_string(seed));
    return true;
}

void Join()
{
    std::string address = IpInput()->GetText();
    uint16_t port = Coop::DEFAULT_PORT;

    size_t colon = address.rfind(':');
    if (colon != std::string::npos)
    {
        try
        {
            int value = std::stoi(address.substr(colon + 1));
            if (value < 1 || value > 65535) throw std::out_of_range("port");
            port = (uint16_t)value;
        }
        catch (const std::exception&)
        {
            Coop::Log("Bad port in " + address);
            return;
        }
        address = address.substr(0, colon);
    }
    if (address.empty())
    {
        Coop::Log("Type the host's IP address first");
        return;
    }

    SaveLastIp(IpInput()->GetText());
    std::string error;
    if (!Coop::GetNet().Join(address, port, error)) Coop::Log("Join failed: " + error);
}

void OnWindowClick(Point p)
{
    Coop::Net& net = Coop::GetNet();
    bool inSession = net.GetRole() != Coop::Role::None;

    if (!inSession && IP_BOX.Contains(p))
    {
        IpInput()->Start();
        return;
    }
    IpInput()->Stop();

    if (!inSession && HOST_BOX.Contains(p))
    {
        std::string error;
        if (!net.Host(Coop::DEFAULT_PORT, error)) Coop::Log("Host failed: " + error);
    }
    else if (!inSession && JOIN_BOX.Contains(p))
    {
        Join();
    }
    else if (net.GetRole() != Coop::Role::Client && MODE_BOX.Contains(p))
    {
        hostMode = hostMode == Coop::MODE_FLEET ? Coop::MODE_CREW : Coop::MODE_FLEET;
    }
    else if (inSession && LEAVE_BOX.Contains(p))
    {
        net.Leave();
    }
    else if (CLOSE_BOX.Contains(p))
    {
        windowOpen = false;
    }
}

// Drawing

void DrawPanel(const Box& box, GL_Color fill)
{
    CSurface::GL_DrawRect(box.x, box.y, box.w, box.h, fill);
    CSurface::GL_DrawRectOutline(box.x, box.y, box.w, box.h, GL_Color(0.6f, 0.75f, 0.8f, 1.f), 2.f);
}

void DrawButton(const Box& box, const std::string& label)
{
    bool hover = box.Contains(mouse);
    DrawPanel(box, hover ? GL_Color(0.3f, 0.55f, 0.6f, 1.f) : GL_Color(0.1f, 0.2f, 0.22f, 1.f));
    CSurface::GL_SetColor(COLOR_WHITE);
    freetype::easy_printCenter(12, box.x + box.w / 2, box.y + 7, label);
}

void PrintText(int x, int y, const std::string& text, GL_Color color = COLOR_WHITE)
{
    CSurface::GL_SetColor(color);
    freetype::easy_print(10, x, y, text);
    CSurface::GL_SetColor(COLOR_WHITE);
}

std::string ToggleLabel()
{
    switch (Coop::GetNet().GetRole())
    {
    case Coop::Role::Host:
        return "CO-OP: HOST (" + std::to_string(players.size() + 1) + ")";
    case Coop::Role::Client:
        return Coop::GetNet().IsConnected() ? "CO-OP: JOINED" : "CO-OP: ...";
    default:
        return "CO-OP";
    }
}

void DrawWindow()
{
    Coop::Net& net = Coop::GetNet();
    const Box& w = WINDOW_BOX;

    DrawPanel(w, GL_Color(0.02f, 0.05f, 0.06f, 0.92f));
    CSurface::GL_SetColor(COLOR_WHITE);
    freetype::easy_printCenter(13, w.x + w.w / 2, w.y + 10, "CO-OP");
    PrintText(w.x + 20, w.y + 40, net.Status(), GL_Color(0.7f, 0.9f, 1.f, 1.f));

    if (net.GetRole() == Coop::Role::None)
    {
        DrawButton(HOST_BOX, "HOST A GAME");
        PrintText(w.x + 20, w.y + 112, "Or join by IP (ip or ip:port):");

        DrawPanel(IP_BOX, TextActive() ? GL_Color(0.15f, 0.25f, 0.3f, 1.f) : GL_Color(0.05f, 0.1f, 0.12f, 1.f));
        if (IpInput()->GetText().empty() && !TextActive())
        {
            CSurface::GL_SetColor(GL_Color(0.5f, 0.6f, 0.65f, 1.f));
            freetype::easy_printCenter(10, IP_BOX.x + IP_BOX.w / 2, IP_BOX.y + 8, "click to type the IP");
            CSurface::GL_SetColor(COLOR_WHITE);
        }
        else
        {
            IpInput()->OnRender(0, Point(IP_BOX.x + IP_BOX.w / 2, IP_BOX.y + 8));
        }
        DrawButton(JOIN_BOX, "JOIN");
    }
    else
    {
        int y = w.y + 70;
        PrintText(w.x + 20, y, "You: " + Describe(Builder().currentName, LocalBlueprint()));
        for (auto& entry : players)
        {
            y += 20;
            const RemotePlayer& player = entry.second;
            PrintText(w.x + 20, y, "Ally: " + Describe(player.shipName, player.blueprint),
                     player.inHangar ? COLOR_WHITE : GL_Color(1.f, 0.6f, 0.4f, 1.f));
            if (!player.inHangar) PrintText(w.x + 280, y, "not in hangar", GL_Color(1.f, 0.6f, 0.4f, 1.f));
        }

        bool crew = Coop::Lobby::Mode() == Coop::MODE_CREW;
        std::string hint;
        if (net.GetRole() == Coop::Role::Host)
        {
            hint = players.empty() ? "Waiting for players to join..." : "Press START when everyone is ready";
        }
        else if (!net.IsConnected())
        {
            hint = "Connecting...";
        }
        else
        {
            hint = crew ? "You will be crew on the host's ship. The host starts." : "Pick your ship. The host starts the run.";
        }
        PrintText(w.x + 20, w.y + 205, hint, GL_Color(0.7f, 0.9f, 1.f, 1.f));
        DrawButton(LEAVE_BOX, "LEAVE");
    }

    // Game mode: the host picks it, the others see it
    std::string modeLabel = Coop::Lobby::Mode() == Coop::MODE_CREW ? "MODE: CREW - one ship, first person" : "MODE: FLEET - a ship each";
    if (net.GetRole() == Coop::Role::Client) PrintText(w.x + 20, MODE_BOX.y + 8, modeLabel, GL_Color(1.f, 0.9f, 0.5f, 1.f));
    else DrawButton(MODE_BOX, modeLabel);

    DrawButton(CLOSE_BOX, "CLOSE");
}

// The hangar has its own popups; stay out of their way
bool BuilderBusy()
{
    ShipBuilder& builder = Builder();
    return builder.shipSelect.bOpen || builder.bRenaming || builder.bCustomizingCrew;
}

}

namespace Coop
{
namespace Lobby
{

uint8_t Mode()
{
    if (GetNet().GetRole() == Role::Client) return haveHostSettings ? hostSettings.mode : MODE_FLEET;
    return hostMode;
}

void OnPeerConnected(int peerId)
{
    players[peerId] = RemotePlayer();
    forceStateSend = true;
    if (GetNet().GetRole() == Role::Host) SendSettings();
}

void OnPeerDisconnected(int peerId)
{
    players.erase(peerId);
}

void OnMessage(int peerId, uint8_t type, const uint8_t *payload, size_t size)
{
    Reader in(payload, size);

    switch (type)
    {
    case MSG_LOBBY_STATE:
    {
        RemotePlayer player;
        player.inHangar = in.U8() != 0;
        player.blueprint = in.Str();
        player.shipName = in.Str();
        if (in.ok) players[peerId] = player;
        break;
    }
    case MSG_START_RUN:
    {
        uint32_t seed = in.U32();
        LobbySettings settings;
        settings.difficulty = in.U8();
        settings.advancedEdition = in.U8() != 0;
        settings.mode = in.U8();
        if (in.ok && GetNet().GetRole() == Role::Client)
        {
            // Started from OnLoop, not from inside the network poll
            pendingSeed = seed;
            pendingStart = true;
            hostSettings = settings;
            haveHostSettings = true;
        }
        break;
    }
    case MSG_LOBBY_SETTINGS:
    {
        LobbySettings settings;
        settings.difficulty = in.U8();
        settings.advancedEdition = in.U8() != 0;
        settings.mode = in.U8();
        if (in.ok && GetNet().GetRole() == Role::Client && settings.difficulty >= 0 && settings.difficulty <= 2)
        {
            hostSettings = settings;
            haveHostSettings = true;
        }
        break;
    }
    default:
        break;
    }
}

void OnLoop()
{
    Net& net = GetNet();
    ShipBuilder& builder = Builder();
    bool inHangar = builder.bOpen;

    if (!inHangar) windowOpen = false;
    if (windowOpen && ipInput) ipInput->OnLoop();
    if (net.GetRole() == Role::None)
    {
        players.clear();
        haveHostSettings = false;
    }

    // The host's difficulty and Advanced Edition switch apply to everyone
    if (inHangar && net.IsConnected())
    {
        if (net.GetRole() == Role::Host && LocalSettings() != sentSettings) SendSettings();
        if (net.GetRole() == Role::Client && haveHostSettings) ApplySettings(hostSettings);
    }

    if (net.IsConnected())
    {
        bool changed = inHangar != sentInHangar || LocalBlueprint() != sentBlueprint || builder.currentName != sentShipName;
        if (forceStateSend || changed || std::chrono::steady_clock::now() - lastStateSent >= STATE_RESEND)
        {
            SendLocalState(inHangar);
        }
    }

    if (pendingStart)
    {
        pendingStart = false;
        if (haveHostSettings && hostSettings.mode == MODE_CREW)
        {
            // Crew members do not fly a ship of their own: they board the host's
            Log("The captain started the run");
            windowOpen = false;
            CrewView::Enter();
        }
        else if (inHangar)
        {
            Log("Host started the run, seed " + std::to_string(pendingSeed));
            if (haveHostSettings) ApplySettings(hostSettings);
            Run::ForceNextSeed(pendingSeed);
            PressStart();
        }
        else
        {
            Log("Host started the run, but you are not in the hangar");
        }
    }
}

}
}

HOOK_METHOD(ShipBuilder, OnRender, () -> void)
{
    LOG_HOOK("HOOK_METHOD -> ShipBuilder::OnRender -> Begin (CoopLobby.cpp)\n")
    super();

    if (BuilderBusy()) return;

    DrawButton(TOGGLE_BOX, ToggleLabel());
    if (windowOpen) DrawWindow();
}

HOOK_METHOD(ShipBuilder, MouseMove, (int x, int y) -> void)
{
    LOG_HOOK("HOOK_METHOD -> ShipBuilder::MouseMove -> Begin (CoopLobby.cpp)\n")
    mouse = Point(x, y);
    if (windowOpen) return;

    super(x, y);
}

HOOK_METHOD(ShipBuilder, MouseClick, (int x, int y) -> void)
{
    LOG_HOOK("HOOK_METHOD -> ShipBuilder::MouseClick -> Begin (CoopLobby.cpp)\n")
    if (simulatingStart) return super(x, y);

    Point p(x, y);
    if (windowOpen)
    {
        OnWindowClick(p);
        return;
    }
    if (!BuilderBusy() && TOGGLE_BOX.Contains(p))
    {
        windowOpen = true;
        return;
    }

    if (Hit(startButton, x, y) && !OnStartClicked()) return;

    if (Coop::GetNet().GetRole() == Coop::Role::Client &&
        (Hit(easyButton, x, y) || Hit(normalButton, x, y) || Hit(hardButton, x, y) ||
         Hit(advancedOffButton, x, y) || Hit(advancedOnButton, x, y)))
    {
        Coop::Log("The host picks the difficulty and Advanced Edition");
        return;
    }

    super(x, y);
}

HOOK_METHOD(ShipBuilder, OnKeyDown, (SDLKey key) -> void)
{
    LOG_HOOK("HOOK_METHOD -> ShipBuilder::OnKeyDown -> Begin (CoopLobby.cpp)\n")
    if (windowOpen)
    {
        if (key == SDLK_ESCAPE && !TextActive()) windowOpen = false;
        return;
    }

    super(key);
}

HOOK_METHOD(CApp, OnTextInput, (int charCode) -> void)
{
    LOG_HOOK("HOOK_METHOD -> CApp::OnTextInput -> Begin (CoopLobby.cpp)\n")
    if (TextActive())
    {
        ipInput->OnTextInput(charCode);
        return;
    }

    super(charCode);
}

HOOK_METHOD(CApp, OnTextEvent, (CEvent::TextEvent te) -> void)
{
    LOG_HOOK("HOOK_METHOD -> CApp::OnTextEvent -> Begin (CoopLobby.cpp)\n")
    if (TextActive())
    {
        ipInput->OnTextEvent(te);
        return;
    }

    super(te);
}
