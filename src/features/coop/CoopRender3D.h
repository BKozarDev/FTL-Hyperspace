#pragma once

// A small software 3D projector on top of FTL's 2D drawing: world polygons are
// clipped and projected on the CPU, sorted back to front (painter's algorithm, FTL
// has no depth buffer for us) and drawn as textured, vertex-coloured triangles with
// CSurface::GL_BlitMultiColorImage, batched per texture.

#include "Global.h"
#include <vector>

namespace Coop
{
namespace R3D
{

// World space: x, y are FTL ship pixels (y points down the screen map), z points up
struct Vec3
{
    float x, y, z;
};

struct Vert
{
    Vec3 p;
    float u, v;
    GL_Color c;
};

struct Camera
{
    float x = 0.f, y = 0.f, z = 20.f;
    float yaw = 0.f;   // radians, 0 looks along +x, turning towards +y
    float pitch = 0.f; // radians, up is positive
    float fov = 1.4f;  // horizontal, radians
    float screenW = 1280.f, screenH = 720.f;
};

class Frame
{
public:
    void Begin(const Camera& camera);

    // A convex polygon in world space (up to 8 vertices)
    void Poly(GL_Texture *tex, const Vert *verts, int count, float depthBias = 0.f);
    void Quad(GL_Texture *tex, const Vert& a, const Vert& b, const Vert& c, const Vert& d, float depthBias = 0.f)
    {
        Vert v[4] = {a, b, c, d};
        Poly(tex, v, 4, depthBias);
    }

    // A sprite standing upright and turned to the camera, its bottom centre at base
    void Billboard(GL_Texture *tex, Vec3 base, float w, float h, float u0, float v0, float u1, float v1,
                   GL_Color color, float depthBias = 0.f);

    // Sorts and draws everything added since Begin
    void Draw();

    // World point to screen; false when behind the camera
    bool Project(Vec3 p, float& sx, float& sy, float& depth) const;

    float Distance(Vec3 p) const;

    const Camera& GetCamera() const
    {
        return cam;
    }
    int PolyCount() const
    {
        return (int)items.size();
    }

private:
    struct CamVert
    {
        float x, y, z, u, v;
        GL_Color c;
    };

    struct Item
    {
        GL_Texture *tex;
        float depth;
        int first;
        int count;
    };

    void ToCamera(const Vert& in, CamVert& out) const;

    Camera cam;
    float sinYaw = 0.f, cosYaw = 1.f, sinPitch = 0.f, cosPitch = 1.f, focal = 1.f;
    std::vector<GL_ColorTexVertex> verts; // screen space, triangles
    std::vector<Item> items;
    std::vector<GL_ColorTexVertex> batch;
};

inline Vert V(float x, float y, float z, float u, float v, GL_Color c)
{
    Vert vert;
    vert.p = {x, y, z};
    vert.u = u;
    vert.v = v;
    vert.c = c;
    return vert;
}

}
}
