// Dum - software renderer: raycast walls, textured floors/ceilings, billboard sprites,
// dynamic light with tile-based occlusion, and 2D drawing for the UI.
#pragma once

#include <string>
#include <vector>

#include "client/textures.h"
#include "game/map.h"

struct SpriteDraw {
    const Texture* tex = nullptr;
    Vec2 pos;
    float height = 1.0f;      // world units (1 = wall height)
    float zoff = 0.0f;        // lift above floor
    bool fullbright = false;
    bool additive = false;    // glowing magic
    bool dither = false;      // semi transparent (ghosts)
    uint32_t tint = 0;        // flash color
    float tintAmt = 0;
    float brightness = 1.0f;  // multiplier (fading effects)
    float dist2 = 0;          // filled in by renderer
    int screenX0 = 0, screenX1 = 0, screenY0 = 0, screenY1 = 0;  // filled in: on-screen box
    bool visible = false;
    uint32_t ent = 0;
    float light = 1.0f;       // filled in: brightness at its position (GPU renderer)
};

struct LightSource {
    Vec2 pos;
    float radius = 4;
    float intensity = 1;
};

// What the GPU renderer needs to draw a frame of the world (filled in by drawWorld in GPU mode).
struct WorldScene {
    bool valid = false;
    const Map* map = nullptr;
    const Assets* assets = nullptr;
    Vec2 cam, dir, plane;
    float horizon = 0;                 // in Renderer pixels (W x H)
    std::vector<SpriteDraw> sprites;   // far to near, visible ones only
};

class Renderer {
public:
    int W, H;
    std::vector<uint32_t> fb;
    std::vector<float> zbuf;

    Renderer(int w, int h);
    void resize(int w, int h);

    // ---- UI layer ----
    // The HUD and menus are always laid out for 640x360. When the 3D view is rendered at a
    // higher resolution, 2D drawing between beginUi() and endUi() goes to a separate
    // transparent 640x360 layer that is scaled up over the world (W/H are the layer's size
    // meanwhile). At 640x360 there is just one buffer and these do nothing.
    static constexpr int UI_W = 640, UI_H = 360;
    bool layered() const { return layered_; }
    void beginFrame();
    // GPU mode: drawWorld() only works out what is visible (and the lighting); the pictures are
    // drawn by GpuRenderer from scene(). 2D drawing outside beginUi/endUi then goes to a
    // transparent layer (the weapon, target brackets, screen tints) shown over the 3D view.
    void setGpu(bool on);
    bool gpu() const { return gpu_; }
    const WorldScene& scene() const { return scene_; }
    // The part of the world layer that changed since the last frame (GPU uploads only that).
    void takeWorldDirty(int& x0, int& y0, int& x1, int& y1);
    const std::vector<uint32_t>& worldLayer() const { return inUi_ ? uiFb_ : fb; }
    void beginUi();
    void endUi();
    bool uiUsed() const { return uiUsed_; }
    const std::vector<uint32_t>& uiLayer() const { return uiFb_; }
    // World + UI layer in one picture at the world resolution (screenshots).
    std::vector<uint32_t> composite() const;
    // Scale from window/world pixel coordinates to UI coordinates.
    float uiScale() const { return layered_ ? (float)UI_W / worldW_ : 1.0f; }

    // ---- world ----
    void setupLights(const Map& m, const std::vector<LightSource>& lights);
    float lightAt(Vec2 p, int tile) const;
    float tileLight(int tile) const { return tile >= 0 && tile < (int)tileLight_.size() ? tileLight_[tile] : 0; }
    // Lighting as set up for this frame (the GPU renderer does the same sums per pixel).
    struct LightInfo { LightSource s; float invR2; std::vector<uint8_t> vis; };
    const std::vector<float>& ambient() const { return ambient_; }
    const std::vector<LightInfo>& lights() const { return lights_; }
    // Renders the 3D view. Marks tiles the player can see in map.flags (F_SEEN).
    void drawWorld(Map& m, const Assets& a, Vec2 cam, float angle, float horizon, std::vector<SpriteDraw>& sprites);
    // Project a world point; returns false if behind the camera.
    bool project(Vec2 p, float& sx, float& depth) const;

    // ---- 2D ----
    void fill(int x, int y, int w, int h, uint32_t c, int alpha = 255);
    void frame(int x, int y, int w, int h, uint32_t c);
    void text(int x, int y, const std::string& s, uint32_t c, int scale = 1, bool shadow = true);
    int textW(const std::string& s, int scale = 1) const { return (int)s.size() * 6 * scale; }
    void blit(const Texture& t, int x, int y, int w, int h, float light = 1.0f, bool additive = false);
    void darken(float f);  // whole screen
    void tintScreen(uint32_t c, int alpha);

private:
    bool layered_ = false, inUi_ = false, uiUsed_ = false, gpu_ = false;
    WorldScene scene_;
    int dirty_[4] = {0, 0, 0, 0}, shown_[4] = {0, 0, 0, 0};   // x0 y0 x1 y1 of the world layer
    bool overlay() const { return inUi_ || gpu_; }               // drawing onto a transparent layer
    void markDirty(int x0, int y0, int x1, int y1);
    int worldW_ = 0, worldH_ = 0;
    std::vector<uint32_t> uiFb_;
    std::vector<LightInfo> lights_;
    std::vector<float> ambient_, tileLight_;
    int mapW_ = 0, mapH_ = 0;
    Vec2 cam_, dir_, plane_;
    float horizon_ = 0;
    void drawSprites(std::vector<SpriteDraw>& sprites);
    void spriteVisibility(std::vector<SpriteDraw>& sprites);   // GPU mode: boxes and light only
};
