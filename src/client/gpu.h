// Dum - OpenGL renderer: draws the same world as the software Renderer, on the graphics card.
//
// The software Renderer still works out what is visible (and marks the automap), sets up the
// lighting and draws all 2D (weapon, target brackets, HUD, menus) into its layers. In GPU
// mode it leaves the 3D pictures to this class, which draws walls, floors, ceilings and
// sprites at the full window resolution and puts the 2D layers on top.
//
// Only SDL2 and the system's OpenGL 3.3 driver are needed: every GL function is looked up
// at run time with SDL_GL_GetProcAddress, so nothing extra is linked.
#pragma once

#include <SDL.h>

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "client/renderer.h"

class GpuRenderer {
public:
    GpuRenderer();
    ~GpuRenderer();
    GpuRenderer(const GpuRenderer&) = delete;
    GpuRenderer& operator=(const GpuRenderer&) = delete;

    // Create the OpenGL context on win (made with SDL_WINDOW_OPENGL). False + err if the
    // driver can't do OpenGL 3.3; then use the software renderer.
    bool init(SDL_Window* win, std::string& err);
    // Textures: crisp pixels (false) or smoothed (upscaled with Scale2x, then filtered).
    void setSmooth(bool on);
    bool smooth() const { return smooth_; }
    // Forget all textures (the art was rebuilt or reloaded).
    void dropTextures();
    // Draw the frame: the world scene (if drawWorld ran this frame), the world layer and the
    // UI layer. swap = show it on screen (false for screenshots: read it with readPixels).
    void present(Renderer& r, bool swap = true);
    // Where the picture is in the window (letterboxed to the Renderer's shape), window pixels.
    void viewRect(int& x, int& y, int& w, int& h) const;
    // The picture just drawn, ARGB, top row first (screenshots).
    std::vector<uint32_t> readPixels(int& w, int& h);
    std::string info() const { return info_; }

private:
    struct Impl;
    std::unique_ptr<Impl> p_;
    SDL_Window* win_ = nullptr;
    SDL_GLContext ctx_ = nullptr;
    bool smooth_ = false;
    std::string info_;
};
