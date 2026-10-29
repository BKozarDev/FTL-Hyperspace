#include "Global.h"
#include "features/coop/CoopFeature.h"
#include "features/coop/CoopProtocol.h"
#include <algorithm>
#include <chrono>
#include <map>

// Allies' ships seen from outside, like an enemy ship without sensors: hull image,
// shield bubble, hull bar, and a red flash when hit. Every game sends its own ship
// a few times a second; the host relays them so everyone sees everyone.
// Placement: in the enemy window's spot while there is no enemy, shrunk into the
// bottom right corner during a fight. A wrecked ally gets a REPAIR button.

namespace
{

using Clock = std::chrono::steady_clock;

constexpr auto SEND_INTERVAL = std::chrono::milliseconds(200);
constexpr auto STALE_AFTER = std::chrono::seconds(3);
constexpr float HIT_FLASH_TIME = 0.5f; // seconds

struct AllyShip
{
    std::string blueprint;
    std::string name;
    int hull = 0;
    int maxHull = 0;
    int shields = 0;
    int maxShields = 0;
    bool wrecked = false;

    Clock::time_point lastUpdate;
    Clock::time_point lastHit;
    GL_Texture *image = nullptr;
    std::string imageFor; // blueprint the image was loaded for
};

struct Box
{
    float x, y, w, h;

    bool Contains(Point p) const
    {
        return p.x >= x && p.x < x + w && p.y >= y && p.y < y + h;
    }
};

struct Area
{
    float cx, cy, w, h;
};

// Key: the ally's player number (Coop::HOST_KEY or a client's key)
std::map<int, AllyShip> allies;
std::map<int, Box> repairButtons; // as drawn last frame
Clock::time_point lastSent;
Point mouse = Point(0, 0);

const Area CALM_AREA = {1060.f, 300.f, 340.f, 300.f};   // the enemy window's spot
const Area COMBAT_AREA = {1080.f, 615.f, 300.f, 130.f}; // bottom right corner

bool Fresh(const AllyShip& ally)
{
    return Clock::now() - ally.lastUpdate <= STALE_AFTER;
}

Coop::Writer OwnState(uint8_t origin)
{
    ShipManager *ship = G_->GetShipManager(0);
    int shields = 0, maxShields = 0;
    if (ship->shieldSystem)
    {
        shields = ship->shieldSystem->shields.power.first;
        maxShields = ship->shieldSystem->shields.power.second;
    }

    Coop::Writer msg(Coop::MSG_SHIP_STATE);
    msg.U8(origin)
       .Str(ship->myBlueprint.blueprintName)
       .Str(ship->myBlueprint.name.GetText())
       .U32((uint32_t)std::max(0, ship->ship.hullIntegrity.first))
       .U32((uint32_t)std::max(0, ship->ship.hullIntegrity.second))
       .U8((uint8_t)std::max(0, shields))
       .U8((uint8_t)std::max(0, maxShields))
       .U8(Coop::Revive::IsWrecked() ? Coop::SHIP_WRECKED : 0);
    return msg;
}

GL_Texture *ShipImage(AllyShip& ally)
{
    if (ally.imageFor == ally.blueprint) return ally.image;

    ally.imageFor = ally.blueprint;
    ally.image = nullptr;
    ShipBlueprint *bp = G_->GetBlueprints()->GetShipBlueprint(ally.blueprint, -1);
    if (bp && !bp->imgFile.empty())
    {
        ally.image = G_->GetResources()->GetImageId("ship/" + bp->imgFile + "_base.png");
    }
    return ally.image;
}

void DrawButton(const Box& box, const std::string& label, bool enabled)
{
    bool hover = enabled && box.Contains(mouse);
    GL_Color fill = !enabled ? GL_Color(0.15f, 0.15f, 0.15f, 0.9f) : hover ? GL_Color(0.3f, 0.55f, 0.3f, 1.f) : GL_Color(0.12f, 0.3f, 0.12f, 1.f);
    CSurface::GL_DrawRect(box.x, box.y, box.w, box.h, fill);
    CSurface::GL_DrawRectOutline((int)box.x, (int)box.y, (int)box.w, (int)box.h, GL_Color(0.6f, 0.8f, 0.6f, 1.f), 1.f);
    CSurface::GL_SetColor(enabled ? COLOR_WHITE : GL_Color(0.6f, 0.6f, 0.6f, 1.f));
    freetype::easy_printCenter(10, box.x + box.w / 2.f, box.y + 5.f, label);
    CSurface::GL_SetColor(COLOR_WHITE);
}

void DrawAlly(int key, AllyShip& ally, const Area& area)
{
    bool destroyed = ally.hull <= 0;
    bool down = destroyed || ally.wrecked;
    GL_Texture *image = ShipImage(ally);

    // Ship image, scaled to fit above the hull bar
    float imageH = area.h - 34.f;
    float scale = 1.f;
    float w = 0.f, h = 0.f;
    if (image && image->width_ > 0 && image->height_ > 0)
    {
        scale = std::min({1.f, area.w / image->width_, imageH / image->height_});
        w = image->width_ * scale;
        h = image->height_ * scale;
    }
    float cx = area.cx;
    float cy = area.cy - area.h / 2.f + imageH / 2.f;

    float sinceHit = std::chrono::duration<float>(Clock::now() - ally.lastHit).count();
    float flash = sinceHit < HIT_FLASH_TIME ? 1.f - sinceHit / HIT_FLASH_TIME : 0.f;

    if (image)
    {
        GL_Color tint = down ? GL_Color(0.35f, 0.35f, 0.35f, 0.8f) : GL_Color(1.f, 1.f - 0.6f * flash, 1.f - 0.6f * flash, 1.f);
        CSurface::GL_BlitImage(image, cx - w / 2.f, cy - h / 2.f, w, h, 0.f, tint, false);
    }

    // One ring per shield layer
    if (!down)
    {
        for (int layer = 0; layer < ally.shields; ++layer)
        {
            float a = w / 2.f + 10.f + 5.f * layer;
            float b = h / 2.f + 10.f + 5.f * layer;
            CSurface::GL_DrawShield((int)cx, (int)cy, a, b, 0, 360, GL_Color(0.35f, 0.7f, 1.f, 0.55f), 2.f);
        }
    }

    // Name and hull bar
    float barW = std::min(area.w, 200.f);
    float barX = area.cx - barW / 2.f;
    float barY = area.cy + area.h / 2.f - 16.f;
    std::string title = ally.name;
    if (destroyed) title += " - DESTROYED";
    else if (ally.wrecked) title += " - WRECKED";
    CSurface::GL_SetColor(down ? GL_Color(1.f, 0.5f, 0.4f, 1.f) : COLOR_WHITE);
    freetype::easy_printCenter(10, area.cx, barY - 16.f, title);
    CSurface::GL_SetColor(COLOR_WHITE);

    CSurface::GL_DrawRect(barX, barY, barW, 10.f, GL_Color(0.f, 0.f, 0.f, 0.6f));
    if (ally.maxHull > 0 && !down)
    {
        float part = std::min(1.f, (float)ally.hull / ally.maxHull);
        GL_Color hullColor = part > 0.5f ? GL_Color(0.4f, 1.f, 0.4f, 1.f) : part > 0.25f ? GL_Color(1.f, 0.85f, 0.3f, 1.f) : GL_Color(1.f, 0.3f, 0.3f, 1.f);
        CSurface::GL_DrawRect(barX + 1.f, barY + 1.f, (barW - 2.f) * part, 8.f, hullColor);
    }
    CSurface::GL_DrawRectOutline((int)barX, (int)barY, (int)barW, 10, GL_Color(0.8f, 0.8f, 0.8f, 1.f), 1.f);
    CSurface::GL_SetColor(COLOR_WHITE);
    freetype::easy_printCenter(10, area.cx, barY + 11.f, std::to_string(ally.hull) + "/" + std::to_string(ally.maxHull));

    // Repair button for a wrecked ally, above its panel
    if (ally.wrecked && !Coop::Revive::IsWrecked())
    {
        bool possible = Coop::Revive::CanReviveHere();
        Box button = {area.cx - 110.f, area.cy - area.h / 2.f - 28.f, 220.f, 22.f};
        std::string label = possible ? "REPAIR ALLY: " + std::to_string(Coop::Revive::Cost()) + " SCRAP"
                                     : "REPAIR AFTER A VICTORY / AT A STORE";
        DrawButton(button, label, possible);
        if (possible) repairButtons[key] = button;
    }
}

}

namespace Coop
{
namespace Ally
{

std::vector<int> AliveKeys()
{
    std::vector<int> keys;
    for (auto& entry : allies)
    {
        if (Fresh(entry.second) && entry.second.hull > 0 && !entry.second.wrecked) keys.push_back(entry.first);
    }
    return keys;
}

std::vector<int> WreckedKeys()
{
    std::vector<int> keys;
    for (auto& entry : allies)
    {
        if (Fresh(entry.second) && entry.second.wrecked) keys.push_back(entry.first);
    }
    return keys;
}

void OnPeerDisconnected(int peerId)
{
    allies.erase(ClientKey(peerId));
    if (GetNet().GetRole() == Role::Client) allies.clear();
}

void OnMessage(int peerId, uint8_t type, const uint8_t *payload, size_t size)
{
    if (type != MSG_SHIP_STATE) return;

    Reader in(payload, size);
    uint8_t origin = in.U8();
    AllyShip update;
    update.blueprint = in.Str();
    update.name = in.Str();
    update.hull = (int)in.U32();
    update.maxHull = (int)in.U32();
    update.shields = in.U8();
    update.maxShields = in.U8();
    update.wrecked = (in.U8() & SHIP_WRECKED) != 0;
    if (!in.ok) return;

    int key = origin == ORIGIN_SENDER ? ClientKey(peerId) : origin;
    if (key == MyKey()) return;

    AllyShip& ally = allies[key];
    if (update.hull < ally.hull && ally.blueprint == update.blueprint) ally.lastHit = Clock::now();
    if (update.wrecked && !ally.wrecked && Fresh(ally)) Log(update.name + " is wrecked! Repair it after a victory or at a store.");

    ally.blueprint = update.blueprint;
    ally.name = update.name;
    ally.hull = update.hull;
    ally.maxHull = update.maxHull;
    ally.shields = update.shields;
    ally.maxShields = update.maxShields;
    ally.wrecked = update.wrecked;
    ally.lastUpdate = Clock::now();

    // The host passes a client's ship on to the other clients, tagged with its key
    if (GetNet().GetRole() == Role::Host && origin == ORIGIN_SENDER)
    {
        std::vector<uint8_t> msg;
        msg.push_back(MSG_SHIP_STATE);
        msg.insert(msg.end(), payload, payload + size);
        msg[1] = (uint8_t)key;
        GetNet().SendToAll(msg.data(), msg.size(), peerId);
    }
}

void OnLoop()
{
    Net& net = GetNet();
    if (!InRun() || !net.IsConnected())
    {
        if (!net.IsConnected()) allies.clear();
        return;
    }

    auto now = Clock::now();
    for (auto it = allies.begin(); it != allies.end();)
    {
        if (now - it->second.lastUpdate > STALE_AFTER) it = allies.erase(it);
        else ++it;
    }

    if (now - lastSent >= SEND_INTERVAL && G_->GetShipManager(0))
    {
        lastSent = now;
        Writer msg = OwnState(net.GetRole() == Role::Host ? ORIGIN_HOST : ORIGIN_SENDER);
        net.SendToAll(msg.Data(), msg.Size());
    }
}

}
}

HOOK_METHOD(CommandGui, RenderPlayerShip, (Point &shipCenter, float jumpScale) -> void)
{
    LOG_HOOK("HOOK_METHOD -> CommandGui::RenderPlayerShip -> Begin (CoopAlly.cpp)\n")
    super(shipCenter, jumpScale);

    repairButtons.clear();

    if (Coop::Revive::IsWrecked())
    {
        CSurface::GL_SetColor(GL_Color(1.f, 0.45f, 0.35f, 1.f));
        freetype::easy_printCenter(13, 640.f, 95.f, "YOUR SHIP IS WRECKED");
        CSurface::GL_SetColor(COLOR_WHITE);
        freetype::easy_printCenter(10, 640.f, 115.f, "An ally can repair it after a victory or at a store");
    }

    if (allies.empty()) return;

    bool enemyPresent = G_->GetShipManager(1) != nullptr;
    const Area& base = enemyPresent ? COMBAT_AREA : CALM_AREA;

    // Several allies share the area side by side
    float count = (float)allies.size();
    float slotW = base.w / count;
    int i = 0;
    for (auto& entry : allies)
    {
        Area slot = {base.cx - base.w / 2.f + slotW * (i + 0.5f), base.cy, slotW, base.h};
        DrawAlly(entry.first, entry.second, slot);
        ++i;
    }
}

HOOK_METHOD(CommandGui, MouseMove, (int mX, int mY) -> void)
{
    LOG_HOOK("HOOK_METHOD -> CommandGui::MouseMove -> Begin (CoopAlly.cpp)\n")
    mouse = Point(mX, mY);
    super(mX, mY);
}

HOOK_METHOD(CommandGui, LButtonDown, (int mX, int mY, bool shiftHeld) -> void)
{
    LOG_HOOK("HOOK_METHOD -> CommandGui::LButtonDown -> Begin (CoopAlly.cpp)\n")
    Point p(mX, mY);
    for (auto& entry : repairButtons)
    {
        if (entry.second.Contains(p))
        {
            Coop::Revive::RequestRevive(entry.first);
            return;
        }
    }
    super(mX, mY, shiftHeld);
}
