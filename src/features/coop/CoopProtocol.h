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
    MSG_START_RUN = 17,   // u32 seed (host -> clients)
    MSG_JUMP = 18,        // u8 kind (JUMP_BEACON: u32 x, u32 y | JUMP_SECTOR: u32 sector), u32 worldLevel (host -> clients)
    MSG_PAUSE = 19,       // u8 paused
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
