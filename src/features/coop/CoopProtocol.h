#pragma once

// Game message types and (de)serialization helpers shared by the co-op features.
// Types below MSG_GAME_BASE are handled inside Coop::Net.

#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

namespace Coop
{

enum GameMsg : uint8_t
{
    MSG_GAME_BASE = 16,
    MSG_LOBBY_STATE = 16, // u8 inHangar, str blueprint, str shipName
    MSG_START_RUN = 17,   // u32 seed, u8 difficulty, u8 advancedEdition, u8 mode (host -> clients)
    MSG_JUMP = 18,        // u8 kind (JUMP_BEACON: u32 x, u32 y | JUMP_SECTOR: u32 sector), u32 worldLevel (host -> clients)
    MSG_PAUSE = 19,       // u8 paused
    MSG_RESTART = 20,     // u32 seed (host -> clients)
    MSG_EVENT_CHOICE = 21, // u32 index, str choiceText (host -> clients)
    MSG_LOBBY_SETTINGS = 22, // u8 difficulty, u8 advancedEdition, u8 mode (host -> clients)
    MSG_SHIP_STATE = 23,  // u8 origin, str blueprint, str name, u32 hull, u32 maxHull, u8 shields, u8 maxShields, u8 flags
    MSG_ASSIGN_KEY = 24,  // u8 key (host -> client): the client's player number
    // Host -> clients, while the host has an enemy: str blueprint, u32 worldLevel, u32 locX, u32 locY,
    // u32 hull, u8 systemCount, [u8 systemType, u8 health], u8 weaponCount, [u8 targetKey]
    MSG_ENEMY_STATE = 25,
    MSG_ENEMY_HIT = 26,   // u32 hullDamage, u8 systemCount, [u8 systemType, u8 damage] (client -> host)
    MSG_ENEMY_PRESENCE = 27, // u8 matched: the client fights the host's enemy (client -> host)
    MSG_WEAPON_FIRED = 28, // u8 weapon, u8 shooterKey (client -> host: shooter ignored; host -> clients)
    MSG_REVIVE = 29,      // u8 targetKey, u8 reviverKey (to the host, which passes it on)
    MSG_GAME_OVER = 30,   // everyone is wrecked (host -> clients)

    // Crew mode: one ship, the host is the captain, clients walk crew members in first person
    // Host -> clients: u32 layoutId, u16 rooms [u8 id, i16 x, i16 y, i16 w, i16 h, u8 system (255 none), u8 primarySlot (255 none), u8 primaryDir],
    // u16 doors [u8 tileX, u8 tileY, u8 dir (0 +x, 1 +y, 2 -x, 3 -y)], i16 originX, i16 originY (tiles count from the origin)
    MSG_CREW_LAYOUT = 31,
    // Host -> clients, unreliable: u32 layoutId, u32 hull, u32 maxHull, u8 shields, u8 flags (CREW_SHIP_*),
    // u8 crew [u32 id, i16 x*4, i16 y*4, u8 health%, u8 flags (CREW_*), u8 ownerKey (255 none), str species, str name],
    // u16 doors [u8 open%], u16 fires [u8 tileX, u8 tileY], u16 breaches [u8 tileX, u8 tileY],
    // u16 rooms [u8 oxygen%], u8 systems [u8 roomId, u8 type, u8 health, u8 maxHealth, u8 power]
    MSG_CREW_SNAPSHOT = 32,
    MSG_PAWN_INPUT = 33,  // client -> host, unreliable: i8 dirX*100, i8 dirY*100, u16 yaw (0..65535 = 0..360), u8 doorPresses
    MSG_PAWN_SELECT = 34, // client -> host: i8 step (0 = any free crew member, +1/-1 = next/previous)
    MSG_PAWN_ASSIGN = 35, // host -> client: u32 crew id (0 = none)
};

// MSG_CREW_SNAPSHOT ship flags
constexpr uint8_t CREW_SHIP_COMBAT = 1;
constexpr uint8_t CREW_SHIP_JUMPING = 2;
constexpr uint8_t CREW_SHIP_DESTROYED = 4;
constexpr uint8_t CREW_SHIP_PAUSED = 8;

// MSG_CREW_SNAPSHOT crew flags
constexpr uint8_t CREW_INTRUDER = 1;
constexpr uint8_t CREW_FIGHTING = 2;
constexpr uint8_t CREW_REPAIRING = 4;
constexpr uint8_t CREW_MANNING = 8;
constexpr uint8_t CREW_ON_FIRE = 16;
constexpr uint8_t CREW_SUFFOCATING = 32;

constexpr uint8_t TILE_OUTSIDE = 255;

// MSG_SHIP_STATE origin: the sender's own ship, or one the host relays (the client's key)
constexpr uint8_t ORIGIN_SENDER = 255;
constexpr uint8_t ORIGIN_HOST = 0;

// MSG_SHIP_STATE flags
constexpr uint8_t SHIP_WRECKED = 1;

// MSG_ENEMY_STATE target for a weapon with no living target
constexpr uint8_t TARGET_NONE = 255;

// Game modes chosen by the host in the lobby
enum GameMode : uint8_t
{
    MODE_FLEET = 0, // every player flies a ship
    MODE_CREW = 1,  // one ship: the host commands, the others are crew in first person
};

enum JumpKind : uint8_t
{
    JUMP_BEACON = 0,
    JUMP_SECTOR = 1,
};

class Writer
{
public:
    explicit Writer(uint8_t type)
    {
        buf.push_back(type);
    }

    Writer& U8(uint8_t value)
    {
        buf.push_back(value);
        return *this;
    }

    Writer& U16(uint16_t value)
    {
        buf.push_back(value & 0xFF);
        buf.push_back(value >> 8);
        return *this;
    }

    Writer& I16(int value)
    {
        value = value < -32768 ? -32768 : value > 32767 ? 32767 : value;
        return U16((uint16_t)(int16_t)value);
    }

    Writer& U32(uint32_t value)
    {
        for (int i = 0; i < 4; ++i) buf.push_back((value >> (8 * i)) & 0xFF);
        return *this;
    }

    Writer& Str(const std::string& value)
    {
        uint16_t len = value.size() > 0xFFFF ? 0xFFFF : (uint16_t)value.size();
        buf.push_back(len & 0xFF);
        buf.push_back(len >> 8);
        buf.insert(buf.end(), value.begin(), value.begin() + len);
        return *this;
    }

    const uint8_t *Data() const
    {
        return buf.data();
    }
    size_t Size() const
    {
        return buf.size();
    }

private:
    std::vector<uint8_t> buf;
};

// Reads the payload after the type byte; ok turns false on a short packet
class Reader
{
public:
    Reader(const uint8_t *data, size_t size) : data(data), size(size)
    {
    }

    uint8_t U8()
    {
        if (!Need(1)) return 0;
        return data[pos++];
    }

    uint16_t U16()
    {
        if (!Need(2)) return 0;
        uint16_t value = data[pos] | (data[pos + 1] << 8);
        pos += 2;
        return value;
    }

    int I16()
    {
        return (int16_t)U16();
    }

    uint32_t U32()
    {
        if (!Need(4)) return 0;
        uint32_t value = data[pos] | (data[pos + 1] << 8) | (data[pos + 2] << 16) | ((uint32_t)data[pos + 3] << 24);
        pos += 4;
        return value;
    }

    std::string Str()
    {
        if (!Need(2)) return "";
        size_t len = data[pos] | (data[pos + 1] << 8);
        pos += 2;
        if (!Need(len)) return "";
        std::string value((const char*)data + pos, len);
        pos += len;
        return value;
    }

    bool ok = true;

private:
    bool Need(size_t count)
    {
        if (!ok || pos + count > size) ok = false;
        return ok;
    }

    const uint8_t *data;
    size_t size;
    size_t pos = 0;
};

}
