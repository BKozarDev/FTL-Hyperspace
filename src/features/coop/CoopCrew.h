#pragma once

// Ship layout shared by the crew mode host (which builds and sends it) and the
// first-person view (which receives it): rooms on a grid of 35 px tiles and doors
// as tile edges. Coordinates are FTL's ship space in pixels; tiles count from origin.

#include "features/coop/CoopProtocol.h"
#include <algorithm>
#include <cstdint>
#include <vector>

namespace Coop
{
namespace Crew
{

constexpr int TILE = 35;

// Directions of a tile edge
constexpr int DIR_X_PLUS = 0;
constexpr int DIR_Y_PLUS = 1;
constexpr int DIR_X_MINUS = 2;
constexpr int DIR_Y_MINUS = 3;

inline int DirX(int dir)
{
    return dir == DIR_X_PLUS ? 1 : dir == DIR_X_MINUS ? -1 : 0;
}
inline int DirY(int dir)
{
    return dir == DIR_Y_PLUS ? 1 : dir == DIR_Y_MINUS ? -1 : 0;
}

struct LayoutRoom
{
    int id = 0;
    int x = 0, y = 0, w = 0, h = 0; // pixels
    int system = -1;
    int primarySlot = -1;
    int primaryDir = 0;
};

struct LayoutDoor
{
    int tx = 0, ty = 0; // tile on one side
    int dir = 0;        // the edge of that tile the door sits on
};

struct Layout
{
    uint32_t id = 0;
    int originX = 0, originY = 0;
    int tilesW = 0, tilesH = 0;
    std::vector<LayoutRoom> rooms;
    std::vector<LayoutDoor> doors;
    std::vector<int> tileRoom; // index into rooms per tile, -1 outside the ship

    bool Valid() const
    {
        return !rooms.empty();
    }

    int RoomAt(int tx, int ty) const
    {
        if (tx < 0 || ty < 0 || tx >= tilesW || ty >= tilesH) return -1;
        return tileRoom[ty * tilesW + tx];
    }

    int TileX(float px) const
    {
        return (int)((px - originX) / TILE + 1000) - 1000;
    }
    int TileY(float py) const
    {
        return (int)((py - originY) / TILE + 1000) - 1000;
    }

    // Door on the edge between (tx, ty) and its neighbour in dir, or -1
    int DoorAt(int tx, int ty, int dir) const
    {
        int nx = tx + DirX(dir), ny = ty + DirY(dir);
        for (size_t i = 0; i < doors.size(); ++i)
        {
            const LayoutDoor& d = doors[i];
            if (d.tx == tx && d.ty == ty && d.dir == dir) return (int)i;
            if (d.tx == nx && d.ty == ny && d.dir == (dir + 2) % 4) return (int)i;
        }
        return -1;
    }

    // Walking from a tile to its neighbour in dir: same room, or through a door
    bool CanStep(int tx, int ty, int dir) const
    {
        int from = RoomAt(tx, ty);
        int to = RoomAt(tx + DirX(dir), ty + DirY(dir));
        if (from < 0 || to < 0) return false;
        return from == to || DoorAt(tx, ty, dir) >= 0;
    }

    void BuildTiles()
    {
        tilesW = tilesH = 0;
        for (const LayoutRoom& r : rooms)
        {
            tilesW = std::max(tilesW, (r.x + r.w - originX) / TILE);
            tilesH = std::max(tilesH, (r.y + r.h - originY) / TILE);
        }
        tileRoom.assign(tilesW * tilesH, -1);
        for (size_t i = 0; i < rooms.size(); ++i)
        {
            const LayoutRoom& r = rooms[i];
            for (int ty = (r.y - originY) / TILE; ty < (r.y + r.h - originY) / TILE; ++ty)
            {
                for (int tx = (r.x - originX) / TILE; tx < (r.x + r.w - originX) / TILE; ++tx)
                {
                    if (tx >= 0 && ty >= 0 && tx < tilesW && ty < tilesH) tileRoom[ty * tilesW + tx] = (int)i;
                }
            }
        }
    }

    int RoomIndexById(int roomId) const
    {
        for (size_t i = 0; i < rooms.size(); ++i)
        {
            if (rooms[i].id == roomId) return (int)i;
        }
        return -1;
    }

    void Write(Writer& msg) const
    {
        msg.U32(id).U16((uint16_t)rooms.size());
        for (const LayoutRoom& r : rooms)
        {
            msg.U8((uint8_t)r.id).I16(r.x).I16(r.y).I16(r.w).I16(r.h)
               .U8(r.system < 0 ? 255 : (uint8_t)r.system)
               .U8(r.primarySlot < 0 ? 255 : (uint8_t)r.primarySlot)
               .U8((uint8_t)r.primaryDir);
        }
        msg.U16((uint16_t)doors.size());
        for (const LayoutDoor& d : doors) msg.U8((uint8_t)d.tx).U8((uint8_t)d.ty).U8((uint8_t)d.dir);
        msg.I16(originX).I16(originY);
    }

    bool Read(Reader& in)
    {
        id = in.U32();
        rooms.resize(in.U16());
        for (LayoutRoom& r : rooms)
        {
            r.id = in.U8();
            r.x = in.I16();
            r.y = in.I16();
            r.w = in.I16();
            r.h = in.I16();
            int system = in.U8();
            int slot = in.U8();
            r.system = system == 255 ? -1 : system;
            r.primarySlot = slot == 255 ? -1 : slot;
            r.primaryDir = in.U8();
        }
        doors.resize(in.U16());
        for (LayoutDoor& d : doors)
        {
            d.tx = in.U8();
            d.ty = in.U8();
            d.dir = in.U8() % 4;
        }
        originX = in.I16();
        originY = in.I16();
        if (!in.ok) return false;
        BuildTiles();
        return true;
    }
};

}
}
