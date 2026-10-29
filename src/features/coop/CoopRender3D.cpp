#include "features/coop/CoopRender3D.h"
#include <algorithm>
#include <cmath>

namespace Coop
{
namespace R3D
{

namespace
{

constexpr float NEAR_Z = 1.5f;
constexpr int MAX_POLY = 8;

// GL_BlitMultiColorImage takes a list of triangles or of quads, depending on the
// renderer; ask FTL how many vertices it makes for one rectangle and follow it
bool probed = false;
bool quadList = false;

void Probe()
{
    if (probed) return;
    probed = true;

    std::vector<GL_ColorTexVertex> test;
    CSurface::AddColorTexVertices(&test, 0.f, 0.f, 10.f, 10.f, 0.f, 1.f, 0.f, 1.f, COLOR_WHITE);
    quadList = test.size() == 4;
    hs_log_file("[coop] 3D view: %d vertices per rectangle, drawing %s\n", (int)test.size(), quadList ? "quads" : "triangles");
}

GL_ColorTexVertex ScreenVert(float x, float y, float u, float v, const GL_Color& c)
{
    GL_ColorTexVertex out;
    out.x = x;
    out.y = y;
    out.u = u;
    out.v = v;
    out.r = c.r;
    out.g = c.g;
    out.b = c.b;
    out.a = c.a;
    return out;
}

GL_Color Lerp(const GL_Color& a, const GL_Color& b, float t)
{
    return GL_Color(a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t);
}

}

void Frame::Begin(const Camera& camera)
{
    Probe();
    cam = camera;
    sinYaw = std::sin(cam.yaw);
    cosYaw = std::cos(cam.yaw);
    sinPitch = std::sin(cam.pitch);
    cosPitch = std::cos(cam.pitch);
    focal = (cam.screenW / 2.f) / std::tan(cam.fov / 2.f);
    verts.clear();
    items.clear();
}

void Frame::ToCamera(const Vert& in, CamVert& out) const
{
    float dx = in.p.x - cam.x;
    float dy = in.p.y - cam.y;
    float dz = in.p.z - cam.z;

    // Forward is (cos, sin); right is forward turned towards +y
    float right = -dx * sinYaw + dy * cosYaw;
    float forward = dx * cosYaw + dy * sinYaw;

    out.x = right;
    out.y = dz * cosPitch - forward * sinPitch;
    out.z = dz * sinPitch + forward * cosPitch;
    out.u = in.u;
    out.v = in.v;
    out.c = in.c;
}

void Frame::Poly(GL_Texture *tex, const Vert *in, int count, float depthBias)
{
    if (!tex || count < 3 || count > MAX_POLY) return;

    CamVert cv[MAX_POLY];
    bool anyFront = false;
    for (int i = 0; i < count; ++i)
    {
        ToCamera(in[i], cv[i]);
        anyFront |= cv[i].z > NEAR_Z;
    }
    if (!anyFront) return;

    // Clip against the near plane
    CamVert clipped[MAX_POLY * 2];
    int n = 0;
    for (int i = 0; i < count; ++i)
    {
        const CamVert& a = cv[i];
        const CamVert& b = cv[(i + 1) % count];
        bool aIn = a.z > NEAR_Z, bIn = b.z > NEAR_Z;
        if (aIn) clipped[n++] = a;
        if (aIn != bIn)
        {
            float t = (NEAR_Z - a.z) / (b.z - a.z);
            CamVert& m = clipped[n++];
            m.x = a.x + (b.x - a.x) * t;
            m.y = a.y + (b.y - a.y) * t;
            m.z = NEAR_Z;
            m.u = a.u + (b.u - a.u) * t;
            m.v = a.v + (b.v - a.v) * t;
            m.c = Lerp(a.c, b.c, t);
        }
    }
    if (n < 3) return;

    // Project, and skip polygons entirely off one side of the screen
    float sx[MAX_POLY * 2], sy[MAX_POLY * 2];
    bool left = true, rightSide = true, top = true, bottom = true;
    float depth = 0.f;
    for (int i = 0; i < n; ++i)
    {
        sx[i] = cam.screenW / 2.f + clipped[i].x * focal / clipped[i].z;
        sy[i] = cam.screenH / 2.f - clipped[i].y * focal / clipped[i].z;
        left &= sx[i] < 0.f;
        rightSide &= sx[i] > cam.screenW;
        top &= sy[i] < 0.f;
        bottom &= sy[i] > cam.screenH;
        depth += clipped[i].z;
    }
    if (left || rightSide || top || bottom) return;

    Item item;
    item.tex = tex;
    item.depth = depth / n + depthBias;
    item.first = (int)verts.size();

    // Fan of triangles (or of degenerate quads)
    for (int i = 1; i + 1 < n; ++i)
    {
        const int idx[3] = {0, i, i + 1};
        for (int k = 0; k < 3; ++k)
        {
            const CamVert& c = clipped[idx[k]];
            verts.push_back(ScreenVert(sx[idx[k]], sy[idx[k]], c.u, c.v, c.c));
        }
        if (quadList) verts.push_back(verts.back());
    }

    item.count = (int)verts.size() - item.first;
    items.push_back(item);
}

void Frame::Billboard(GL_Texture *tex, Vec3 base, float w, float h, float u0, float v0, float u1, float v1,
                      GL_Color color, float depthBias)
{
    // Turned to face the camera around the vertical axis
    float rx = -sinYaw * (w / 2.f), ry = cosYaw * (w / 2.f);
    Quad(tex,
         V(base.x - rx, base.y - ry, base.z + h, u0, v0, color),
         V(base.x + rx, base.y + ry, base.z + h, u1, v0, color),
         V(base.x + rx, base.y + ry, base.z, u1, v1, color),
         V(base.x - rx, base.y - ry, base.z, u0, v1, color),
         depthBias);
}

void Frame::Draw()
{
    // Far to near; equal depths keep their order so decals stay on top of their surface
    std::stable_sort(items.begin(), items.end(), [](const Item& a, const Item& b) { return a.depth > b.depth; });

    size_t i = 0;
    while (i < items.size())
    {
        GL_Texture *tex = items[i].tex;
        batch.clear();
        while (i < items.size() && items[i].tex == tex)
        {
            batch.insert(batch.end(), verts.begin() + items[i].first, verts.begin() + items[i].first + items[i].count);
            ++i;
        }
        CSurface::GL_BlitMultiColorImage(tex, batch, false);
    }
}

bool Frame::Project(Vec3 p, float& sx, float& sy, float& depth) const
{
    CamVert c;
    ToCamera(V(p.x, p.y, p.z, 0.f, 0.f, COLOR_WHITE), c);
    if (c.z <= NEAR_Z) return false;
    sx = cam.screenW / 2.f + c.x * focal / c.z;
    sy = cam.screenH / 2.f - c.y * focal / c.z;
    depth = c.z;
    return true;
}

float Frame::Distance(Vec3 p) const
{
    float dx = p.x - cam.x, dy = p.y - cam.y, dz = p.z - cam.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

}
}
