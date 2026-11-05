// Dum - OpenGL renderer implementation. See gpu.h.
#include "client/gpu.h"

#include <SDL_opengl.h>

#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>

#ifndef APIENTRY
#define APIENTRY
#endif

namespace {

// Every GL function we use, looked up through SDL (no link-time dependency on OpenGL).
#define DUM_GL_FUNCS(X)                                                                         \
    X(void, Viewport, (GLint, GLint, GLsizei, GLsizei))                                         \
    X(void, ClearColor, (GLfloat, GLfloat, GLfloat, GLfloat))                                   \
    X(void, Clear, (GLbitfield))                                                                \
    X(void, Enable, (GLenum))                                                                   \
    X(void, Disable, (GLenum))                                                                  \
    X(void, BlendFunc, (GLenum, GLenum))                                                        \
    X(void, DepthMask, (GLboolean))                                                             \
    X(void, DepthFunc, (GLenum))                                                                \
    X(void, GenTextures, (GLsizei, GLuint*))                                                    \
    X(void, DeleteTextures, (GLsizei, const GLuint*))                                           \
    X(void, BindTexture, (GLenum, GLuint))                                                      \
    X(void, TexImage2D, (GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void*)) \
    X(void, TexSubImage2D, (GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, const void*)) \
    X(void, TexParameteri, (GLenum, GLenum, GLint))                                             \
    X(void, TexParameterf, (GLenum, GLenum, GLfloat))                                           \
    X(void, PixelStorei, (GLenum, GLint))                                                       \
    X(void, ActiveTexture, (GLenum))                                                            \
    X(void, GenerateMipmap, (GLenum))                                                           \
    X(void, DrawArrays, (GLenum, GLint, GLsizei))                                               \
    X(void, ReadPixels, (GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void*))                \
    X(void, ReadBuffer, (GLenum))                                                               \
    X(const GLubyte*, GetString, (GLenum))                                                      \
    X(void, GetFloatv, (GLenum, GLfloat*))                                                      \
    X(GLenum, GetError, (void))                                                                 \
    X(GLuint, CreateShader, (GLenum))                                                           \
    X(void, ShaderSource, (GLuint, GLsizei, const GLchar* const*, const GLint*))                \
    X(void, CompileShader, (GLuint))                                                            \
    X(void, GetShaderiv, (GLuint, GLenum, GLint*))                                              \
    X(void, GetShaderInfoLog, (GLuint, GLsizei, GLsizei*, GLchar*))                             \
    X(GLuint, CreateProgram, (void))                                                            \
    X(void, AttachShader, (GLuint, GLuint))                                                     \
    X(void, LinkProgram, (GLuint))                                                              \
    X(void, GetProgramiv, (GLuint, GLenum, GLint*))                                             \
    X(void, GetProgramInfoLog, (GLuint, GLsizei, GLsizei*, GLchar*))                            \
    X(void, DeleteShader, (GLuint))                                                             \
    X(void, DeleteProgram, (GLuint))                                                            \
    X(void, UseProgram, (GLuint))                                                               \
    X(void, BindAttribLocation, (GLuint, GLuint, const GLchar*))                                \
    X(GLint, GetUniformLocation, (GLuint, const GLchar*))                                       \
    X(void, Uniform1i, (GLint, GLint))                                                          \
    X(void, Uniform1f, (GLint, GLfloat))                                                        \
    X(void, Uniform2f, (GLint, GLfloat, GLfloat))                                               \
    X(void, Uniform3f, (GLint, GLfloat, GLfloat, GLfloat))                                      \
    X(void, Uniform4f, (GLint, GLfloat, GLfloat, GLfloat, GLfloat))                             \
    X(void, Uniform4fv, (GLint, GLsizei, const GLfloat*))                                       \
    X(void, GenVertexArrays, (GLsizei, GLuint*))                                                \
    X(void, DeleteVertexArrays, (GLsizei, const GLuint*))                                       \
    X(void, BindVertexArray, (GLuint))                                                          \
    X(void, GenBuffers, (GLsizei, GLuint*))                                                     \
    X(void, DeleteBuffers, (GLsizei, const GLuint*))                                            \
    X(void, BindBuffer, (GLenum, GLuint))                                                       \
    X(void, BufferData, (GLenum, GLsizeiptr, const void*, GLenum))                              \
    X(void, EnableVertexAttribArray, (GLuint))                                                  \
    X(void, VertexAttribPointer, (GLuint, GLint, GLenum, GLboolean, GLsizei, const void*))

struct GLFns {
#define DUM_GL_DECL(ret, name, args) ret(APIENTRY* name) args = nullptr;
    DUM_GL_FUNCS(DUM_GL_DECL)
#undef DUM_GL_DECL
    bool load(std::string& missing) {
#define DUM_GL_LOAD(ret, name, args)                                                  \
    name = reinterpret_cast<ret(APIENTRY*) args>(SDL_GL_GetProcAddress("gl" #name)); \
    if (!name) { missing = "gl" #name; return false; }
        DUM_GL_FUNCS(DUM_GL_LOAD)
#undef DUM_GL_LOAD
        return true;
    }
};
GLFns gl;

constexpr int MAX_LIGHTS = 32;
constexpr float NEAR_Z = 0.03f, FAR_Z = 200.0f;

// ---------------------------------------------------------------- shaders --

// World geometry: walls, floors and ceilings, lit like the software renderer.
const char* kWorldVS = R"(#version 330 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec2 aUV;
layout(location = 2) in vec2 aTile;
layout(location = 3) in float aShade;
uniform vec2 uCam, uDir, uRight;
uniform float uPlaneLen, uShift;
uniform vec2 uNearFar;
out vec2 vUV;
out vec2 vWorld;
flat out ivec2 vTile;
out float vShade;
void main() {
    vec2 d = aPos.xy - uCam;
    float ty = dot(d, uDir), tx = dot(d, uRight);
    float n = uNearFar.x, f = uNearFar.y;
    gl_Position = vec4(tx / uPlaneLen, (aPos.z - 0.5) * 2.0 + uShift * ty, (f + n) / (f - n) * ty - 2.0 * f * n / (f - n), ty);
    vUV = aUV;
    vWorld = aPos.xy;
    vTile = ivec2(aTile);
    vShade = aShade;
}
)";

const char* kLightFn = R"(
uniform sampler2D uAmb;
uniform usampler2D uVis;
uniform vec4 uLights[32];   // x, y, 1/radius^2, intensity
uniform int uNumLights;
float lightAt(vec2 p, ivec2 tile) {
    float l = texelFetch(uAmb, tile, 0).r;
    uint m = texelFetch(uVis, tile, 0).r;
    for (int i = 0; i < uNumLights; i++) {
        if ((m & (1u << uint(i))) == 0u) continue;
        vec2 d = p - uLights[i].xy;
        float f = 1.0 - dot(d, d) * uLights[i].z;
        if (f > 0.0) l += f * uLights[i].w;
    }
    return min(l, 1.4);
}
)";

const char* kWorldFS = R"(
in vec2 vUV;
in vec2 vWorld;
flat in ivec2 vTile;
in float vShade;
uniform sampler2D uTex;
out vec4 oColor;
void main() {
    vec3 c = texture(uTex, vUV).rgb;
    oColor = vec4(c * lightAt(vWorld, vTile) * vShade, 1.0);
}
)";

// Sprites: camera-facing pictures with a light level, tint, see-through checkerboard.
const char* kSpriteVS = R"(#version 330 core
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec2 aUV;
uniform vec2 uCam, uDir, uRight;
uniform float uPlaneLen, uShift;
uniform vec2 uNearFar;
out vec2 vUV;
void main() {
    vec2 d = aPos.xy - uCam;
    float ty = dot(d, uDir), tx = dot(d, uRight);
    float n = uNearFar.x, f = uNearFar.y;
    gl_Position = vec4(tx / uPlaneLen, (aPos.z - 0.5) * 2.0 + uShift * ty, (f + n) / (f - n) * ty - 2.0 * f * n / (f - n), ty);
    vUV = aUV;
}
)";

const char* kSpriteFS = R"(#version 330 core
in vec2 vUV;
uniform sampler2D uTex;
uniform float uLight;
uniform vec4 uTint;       // rgb, amount
uniform int uDither;
out vec4 oColor;
void main() {
    vec4 c = texture(uTex, vUV);
    if (c.a < 0.5) discard;
    if (uDither == 1 && ((int(gl_FragCoord.x) + int(gl_FragCoord.y)) & 1) == 1) discard;
    vec3 col = c.rgb * uLight;
    col = mix(col, uTint.rgb, uTint.a);
    oColor = vec4(col, 1.0);
}
)";

// Sky above the town: a gradient, lighter toward the horizon.
const char* kSkyVS = R"(#version 330 core
layout(location = 0) in vec2 aPos;
uniform float uH, uHorizon;
out float vY;
void main() {
    gl_Position = vec4(aPos, 0.999, 1.0);
    vY = (1.0 - aPos.y) * 0.5 * uH;   // in Renderer pixels, 0 = top
}
)";
const char* kSkyFS = R"(#version 330 core
in float vY;
uniform float uH, uHorizon;
out vec4 oColor;
void main() {
    float k = clamp((uHorizon - vY) / (uH * 0.5), 0.0, 1.0);
    oColor = vec4(mix(vec3(0.604, 0.722, 0.847), vec3(0.165, 0.290, 0.541), k), 1.0);
}
)";

// 2D layers (weapon/brackets/tints, HUD/menus): a textured rectangle with alpha.
const char* kLayerVS = R"(#version 330 core
layout(location = 0) in vec2 aPos;
out vec2 vUV;
void main() {
    gl_Position = vec4(aPos, 0.0, 1.0);
    vUV = vec2(aPos.x * 0.5 + 0.5, 0.5 - aPos.y * 0.5);
}
)";
const char* kLayerFS = R"(#version 330 core
in vec2 vUV;
uniform sampler2D uTex;
out vec4 oColor;
void main() { oColor = texture(uTex, vUV); }
)";

GLuint compile(GLenum type, const std::string& src, std::string& err) {
    GLuint s = gl.CreateShader(type);
    const char* c = src.c_str();
    gl.ShaderSource(s, 1, &c, nullptr);
    gl.CompileShader(s);
    GLint ok = 0;
    gl.GetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024] = {0};
        gl.GetShaderInfoLog(s, sizeof log, nullptr, log);
        err = std::string("shader: ") + log;
        gl.DeleteShader(s);
        return 0;
    }
    return s;
}

GLuint program(const std::string& vs, const std::string& fs, std::string& err) {
    GLuint v = compile(GL_VERTEX_SHADER, vs, err);
    if (!v) return 0;
    GLuint f = compile(GL_FRAGMENT_SHADER, fs, err);
    if (!f) { gl.DeleteShader(v); return 0; }
    GLuint p = gl.CreateProgram();
    gl.AttachShader(p, v);
    gl.AttachShader(p, f);
    gl.LinkProgram(p);
    gl.DeleteShader(v);
    gl.DeleteShader(f);
    GLint ok = 0;
    gl.GetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[1024] = {0};
        gl.GetProgramInfoLog(p, sizeof log, nullptr, log);
        err = std::string("shader link: ") + log;
        gl.DeleteProgram(p);
        return 0;
    }
    return p;
}

// Scale2x (EPX): doubles a pixel-art picture, rounding off diagonal stair-steps.
Texture scale2x(const Texture& t) {
    Texture o(t.w * 2, t.h * 2);
    for (int y = 0; y < t.h; y++)
        for (int x = 0; x < t.w; x++) {
            uint32_t E = t.px[y * t.w + x];
            uint32_t B = y > 0 ? t.px[(y - 1) * t.w + x] : E, H = y + 1 < t.h ? t.px[(y + 1) * t.w + x] : E;
            uint32_t D = x > 0 ? t.px[y * t.w + x - 1] : E, F = x + 1 < t.w ? t.px[y * t.w + x + 1] : E;
            uint32_t e0 = E, e1 = E, e2 = E, e3 = E;
            if (B != H && D != F) {
                if (D == B) e0 = D;
                if (B == F) e1 = F;
                if (D == H) e2 = D;
                if (H == F) e3 = F;
            }
            o.px[(2 * y) * o.w + 2 * x] = e0;
            o.px[(2 * y) * o.w + 2 * x + 1] = e1;
            o.px[(2 * y + 1) * o.w + 2 * x] = e2;
            o.px[(2 * y + 1) * o.w + 2 * x + 1] = e3;
        }
    return o;
}

struct Vtx { float x, y, z, u, v, tx, ty, shade; };

}  // namespace

// ------------------------------------------------------------------ state --

struct GpuRenderer::Impl {
    GLuint worldProg = 0, spriteProg = 0, skyProg = 0, layerProg = 0;
    GLuint worldVao = 0, worldVbo = 0, spriteVao = 0, spriteVbo = 0, quadVao = 0, quadVbo = 0;
    GLuint ambTex = 0, visTex = 0, worldLayerTex = 0, uiTex = 0;
    int worldLayerW = 0, worldLayerH = 0, uiW = 0, uiH = 0;
    std::map<const Texture*, GLuint> textures;
    bool smooth = false;
    float maxAniso = 0;
    // The level mesh: one range of vertices per texture.
    struct Range { const Texture* tex; int first, count; };
    std::vector<Range> ranges;
    const Map* meshMap = nullptr;
    const Assets* meshAssets = nullptr;
    int meshW = -1, meshH = -1;
    uint32_t meshSeed = 0;
    int meshDepth = -1;
    std::vector<uint8_t> meshTiles;
    bool hasSky = false;
    int lastW = 0, lastH = 0;   // drawable size of the last frame (for readPixels)

    // uniform locations
    struct Cam { GLint cam, dir, right, planeLen, shift, nearFar; };
    Cam wCam{}, sCam{};
    GLint wTex = -1, wAmb = -1, wVis = -1, wLights = -1, wNum = -1;
    GLint sTex = -1, sLight = -1, sTint = -1, sDither = -1;
    GLint skyH = -1, skyHorizon = -1, lTex = -1;

    GLuint texture(const Texture& t);
    void buildMesh(const Map& m, const Assets& a);
    void uploadLights(const Renderer& r, const Map& m);
    void setCam(const Cam& c, const WorldScene& sc, float H);
    void drawWorld(const Renderer& r, const WorldScene& sc);
    void drawLayer(GLuint& tex, int& tw, int& th, const std::vector<uint32_t>& px, int w, int h, int dx0, int dy0, int dx1, int dy1);
};

GLuint GpuRenderer::Impl::texture(const Texture& t) {
    auto it = textures.find(&t);
    if (it != textures.end()) return it->second;
    GLuint id;
    gl.GenTextures(1, &id);
    gl.BindTexture(GL_TEXTURE_2D, id);
    const Texture* src = &t;
    Texture big;
    if (smooth && t.w > 0 && t.w <= 128 && t.h <= 128) {
        // Smooth: upscale small pixel art (2x, twice for 64 and smaller) before filtering.
        big = scale2x(t);
        if (big.w <= 128 && big.h <= 128) big = scale2x(big);
        src = &big;
    }
    gl.PixelStorei(GL_UNPACK_ALIGNMENT, 4);
    gl.PixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    gl.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, src->w, src->h, 0, GL_BGRA, GL_UNSIGNED_BYTE, src->px.data());
    gl.GenerateMipmap(GL_TEXTURE_2D);
    // Pixelated: crisp up close, mipmaps far away (no shimmering). Smooth: filtered everywhere.
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, smooth ? GL_LINEAR : GL_NEAREST);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, smooth ? GL_LINEAR_MIPMAP_LINEAR : GL_NEAREST_MIPMAP_LINEAR);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    if (maxAniso > 1) gl.TexParameterf(GL_TEXTURE_2D, 0x84FE /* MAX_ANISOTROPY */, std::min(8.0f, maxAniso));
    textures[&t] = id;
    return id;
}

void GpuRenderer::Impl::buildMesh(const Map& m, const Assets& a) {
    std::map<const Texture*, std::vector<Vtx>> groups;
    auto quad = [&](const Texture* t, Vtx v0, Vtx v1, Vtx v2, Vtx v3) {
        auto& g = groups[t];
        g.insert(g.end(), {v0, v1, v2, v0, v2, v3});
    };
    hasSky = false;
    for (int y = 0; y < m.h; y++)
        for (int x = 0; x < m.w; x++) {
            int idx = y * m.w + x;
            uint8_t tile = m.tiles[idx];
            float fx = (float)x, fy = (float)y, X = fx + 1, Y = fy + 1;
            if (!Map::tileSolid(tile)) {
                // Floor (u along x, v along y, like the software renderer).
                const Texture* ft = tile == T_STAIRS_DOWN ? &a.stairsDown : tile == T_STAIRS_UP ? &a.stairsUp : &a.floors[m.floorTex[idx] & 7];
                quad(ft, {fx, fy, 0, 0, 0, fx, fy, 1}, {X, fy, 0, 1, 0, fx, fy, 1}, {X, Y, 0, 1, 1, fx, fy, 1}, {fx, Y, 0, 0, 1, fx, fy, 1});
                if (m.flags[idx] & F_OUTDOOR) {
                    hasSky = true;
                } else {
                    const Texture* ct = tile == T_STAIRS_UP ? &a.stairsUpCeil : (m.floorTex[idx] == FT_DIRT ? &a.ceilingCave : &a.ceiling);
                    quad(ct, {fx, fy, 1, 0, 0, fx, fy, 0.8f}, {fx, Y, 1, 0, 1, fx, fy, 0.8f}, {X, Y, 1, 1, 1, fx, fy, 0.8f}, {X, fy, 1, 1, 0, fx, fy, 0.8f});
                }
                continue;
            }
            // Wall faces toward each open neighbour, lit from that neighbour's tile.
            const Texture* wt = tile == T_DOOR ? &a.door : tile == T_DOOR_LOCKED ? &a.doorLocked : tile == T_RUBBLE ? &a.rubble
                              : tile == T_DOOR_KEY ? &a.keyDoors[m.keyDoorColor(x, y)]
                              : tile == T_SHOP ? &a.shopDoors[m.floorTex[idx] & 7] : &a.walls[m.wallTex[idx] & 7];
            auto open = [&](int nx, int ny) { return m.inside(nx, ny) && !Map::tileSolid(m.tiles[ny * m.w + nx]); };
            // u runs the way the software renderer maps it (so pictures aren't mirrored); v = 0 at the top.
            if (open(x + 1, y)) {   // east face, seen looking west: u = y fraction
                float lx = fx + 1, ly = fy;
                quad(wt, {X, fy, 1, 0, 0, lx, ly, 1}, {X, Y, 1, 1, 0, lx, ly, 1}, {X, Y, 0, 1, 1, lx, ly, 1}, {X, fy, 0, 0, 1, lx, ly, 1});
            }
            if (open(x - 1, y)) {   // west face: u = 1 - y fraction
                float lx = fx - 1, ly = fy;
                quad(wt, {fx, Y, 1, 0, 0, lx, ly, 1}, {fx, fy, 1, 1, 0, lx, ly, 1}, {fx, fy, 0, 1, 1, lx, ly, 1}, {fx, Y, 0, 0, 1, lx, ly, 1});
            }
            if (open(x, y + 1)) {   // south face (y+1 side): u = 1 - x fraction, darker side
                float lx = fx, ly = fy + 1;
                quad(wt, {X, Y, 1, 0, 0, lx, ly, 0.78f}, {fx, Y, 1, 1, 0, lx, ly, 0.78f}, {fx, Y, 0, 1, 1, lx, ly, 0.78f}, {X, Y, 0, 0, 1, lx, ly, 0.78f});
            }
            if (open(x, y - 1)) {   // north face: u = x fraction, darker side
                float lx = fx, ly = fy - 1;
                quad(wt, {fx, fy, 1, 0, 0, lx, ly, 0.78f}, {X, fy, 1, 1, 0, lx, ly, 0.78f}, {X, fy, 0, 1, 1, lx, ly, 0.78f}, {fx, fy, 0, 0, 1, lx, ly, 0.78f});
            }
        }
    std::vector<Vtx> all;
    ranges.clear();
    for (auto& [t, v] : groups) {
        ranges.push_back({t, (int)all.size(), (int)v.size()});
        all.insert(all.end(), v.begin(), v.end());
    }
    gl.BindVertexArray(worldVao);
    gl.BindBuffer(GL_ARRAY_BUFFER, worldVbo);
    gl.BufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(all.size() * sizeof(Vtx)), all.data(), GL_STATIC_DRAW);
    meshMap = &m;
    meshAssets = &a;
    meshW = m.w; meshH = m.h; meshSeed = m.seed; meshDepth = m.depth;
    meshTiles = m.tiles;
}

void GpuRenderer::Impl::uploadLights(const Renderer& r, const Map& m) {
    const auto& amb = r.ambient();
    const auto& lights = r.lights();
    std::vector<float> a(m.w * m.h, 0.0f);
    if (amb.size() == a.size()) a = amb;
    std::vector<uint32_t> vis(m.w * m.h, 0);
    int n = std::min((int)lights.size(), MAX_LIGHTS);
    std::vector<float> L(4 * MAX_LIGHTS, 0.0f);
    for (int i = 0; i < n; i++) {
        const auto& li = lights[i];
        L[4 * i] = li.s.pos.x; L[4 * i + 1] = li.s.pos.y; L[4 * i + 2] = li.invR2; L[4 * i + 3] = li.s.intensity;
        if (li.vis.size() != vis.size()) continue;
        for (size_t k = 0; k < vis.size(); k++)
            if (li.vis[k]) vis[k] |= 1u << i;
    }
    gl.PixelStorei(GL_UNPACK_ALIGNMENT, 4);
    gl.PixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    gl.ActiveTexture(GL_TEXTURE1);
    gl.BindTexture(GL_TEXTURE_2D, ambTex);
    gl.TexImage2D(GL_TEXTURE_2D, 0, GL_R32F, m.w, m.h, 0, GL_RED, GL_FLOAT, a.data());
    gl.ActiveTexture(GL_TEXTURE2);
    gl.BindTexture(GL_TEXTURE_2D, visTex);
    gl.TexImage2D(GL_TEXTURE_2D, 0, GL_R32UI, m.w, m.h, 0, GL_RED_INTEGER, GL_UNSIGNED_INT, vis.data());
    gl.ActiveTexture(GL_TEXTURE0);
    gl.UseProgram(worldProg);
    gl.Uniform4fv(wLights, MAX_LIGHTS, L.data());
    gl.Uniform1i(wNum, n);
}

void GpuRenderer::Impl::setCam(const Cam& c, const WorldScene& sc, float H) {
    float planeLen = sc.plane.len();
    Vec2 right = sc.plane * (1.0f / std::max(1e-6f, planeLen));
    gl.Uniform2f(c.cam, sc.cam.x, sc.cam.y);
    gl.Uniform2f(c.dir, sc.dir.x, sc.dir.y);
    gl.Uniform2f(c.right, right.x, right.y);
    gl.Uniform1f(c.planeLen, planeLen);
    gl.Uniform1f(c.shift, 1.0f - 2.0f * sc.horizon / H);
    gl.Uniform2f(c.nearFar, NEAR_Z, FAR_Z);
}

void GpuRenderer::Impl::drawWorld(const Renderer& r, const WorldScene& sc) {
    const Map& m = *sc.map;
    const Assets& a = *sc.assets;
    if (meshMap != &m || meshAssets != &a || meshW != m.w || meshH != m.h || meshSeed != m.seed || meshDepth != m.depth || meshTiles != m.tiles)
        buildMesh(m, a);
    float H = (float)r.H;

    if (hasSky) {
        gl.Disable(GL_DEPTH_TEST);
        gl.UseProgram(skyProg);
        gl.Uniform1f(skyH, H);
        gl.Uniform1f(skyHorizon, sc.horizon);
        gl.BindVertexArray(quadVao);
        gl.DrawArrays(GL_TRIANGLES, 0, 6);
    }

    gl.Enable(GL_DEPTH_TEST);
    gl.DepthFunc(GL_LESS);
    gl.DepthMask(GL_TRUE);
    gl.Disable(GL_BLEND);
    uploadLights(r, m);
    gl.UseProgram(worldProg);
    setCam(wCam, sc, H);
    gl.Uniform1i(wTex, 0);
    gl.Uniform1i(wAmb, 1);
    gl.Uniform1i(wVis, 2);
    gl.ActiveTexture(GL_TEXTURE1);
    gl.BindTexture(GL_TEXTURE_2D, ambTex);
    gl.ActiveTexture(GL_TEXTURE2);
    gl.BindTexture(GL_TEXTURE_2D, visTex);
    gl.ActiveTexture(GL_TEXTURE0);
    gl.BindVertexArray(worldVao);
    for (auto& rg : ranges) {
        gl.BindTexture(GL_TEXTURE_2D, texture(*rg.tex));
        gl.DrawArrays(GL_TRIANGLES, rg.first, rg.count);
    }

    // Sprites, far to near. Glowing ones are added like light and don't hide what's behind.
    if (sc.sprites.empty()) return;
    std::vector<Vtx> v;
    v.reserve(sc.sprites.size() * 6);
    Vec2 right = sc.plane * (1.0f / std::max(1e-6f, sc.plane.len()));
    for (auto& s : sc.sprites) {
        float hgt = s.height, wid = hgt * s.tex->w / s.tex->h;
        Vec2 l = s.pos - right * (wid / 2), rr = s.pos + right * (wid / 2);
        float z0 = s.zoff, z1 = s.zoff + hgt;
        Vtx a0{l.x, l.y, z1, 0, 0, 0, 0, 0}, a1{rr.x, rr.y, z1, 1, 0, 0, 0, 0}, a2{rr.x, rr.y, z0, 1, 1, 0, 0, 0}, a3{l.x, l.y, z0, 0, 1, 0, 0, 0};
        v.insert(v.end(), {a0, a1, a2, a0, a2, a3});
    }
    gl.BindVertexArray(spriteVao);
    gl.BindBuffer(GL_ARRAY_BUFFER, spriteVbo);
    gl.BufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(v.size() * sizeof(Vtx)), v.data(), GL_STREAM_DRAW);
    gl.UseProgram(spriteProg);
    setCam(sCam, sc, H);
    gl.Uniform1i(sTex, 0);
    for (size_t i = 0; i < sc.sprites.size(); i++) {
        const SpriteDraw& s = sc.sprites[i];
        GLuint t = texture(*s.tex);
        gl.BindTexture(GL_TEXTURE_2D, t);
        float light = s.fullbright ? s.brightness : s.light;
        gl.Uniform1f(sLight, light);
        float tr = ((s.tint >> 16) & 255) / 255.0f, tg = ((s.tint >> 8) & 255) / 255.0f, tb = (s.tint & 255) / 255.0f;
        gl.Uniform4f(sTint, tr, tg, tb, s.additive ? 0.0f : s.tintAmt);
        gl.Uniform1i(sDither, s.dither ? 1 : 0);
        if (s.additive) {
            gl.Enable(GL_BLEND);
            gl.BlendFunc(GL_ONE, GL_ONE);
            gl.DepthMask(GL_FALSE);
        }
        gl.DrawArrays(GL_TRIANGLES, (GLint)(i * 6), 6);
        if (s.additive) {
            gl.Disable(GL_BLEND);
            gl.DepthMask(GL_TRUE);
        }
    }
}

// Upload the changed part of a 2D layer (or all of it, when its size changed) and draw it.
void GpuRenderer::Impl::drawLayer(GLuint& tex, int& tw, int& th, const std::vector<uint32_t>& px, int w, int h,
                                  int dx0, int dy0, int dx1, int dy1) {
    if (!tex) gl.GenTextures(1, &tex);
    gl.ActiveTexture(GL_TEXTURE0);
    gl.BindTexture(GL_TEXTURE_2D, tex);
    gl.PixelStorei(GL_UNPACK_ALIGNMENT, 4);
    if (tw != w || th != h) {
        gl.PixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        gl.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_BGRA, GL_UNSIGNED_BYTE, px.data());
        gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        tw = w; th = h;
    } else if (dx1 > dx0 && dy1 > dy0) {
        gl.PixelStorei(GL_UNPACK_ROW_LENGTH, w);
        gl.TexSubImage2D(GL_TEXTURE_2D, 0, dx0, dy0, dx1 - dx0, dy1 - dy0, GL_BGRA, GL_UNSIGNED_BYTE, &px[dy0 * w + dx0]);
        gl.PixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    }
    gl.Disable(GL_DEPTH_TEST);
    gl.Enable(GL_BLEND);
    gl.BlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    gl.UseProgram(layerProg);
    gl.Uniform1i(lTex, 0);
    gl.BindVertexArray(quadVao);
    gl.DrawArrays(GL_TRIANGLES, 0, 6);
    gl.Disable(GL_BLEND);
}

// -------------------------------------------------------------- interface --

GpuRenderer::GpuRenderer() = default;

GpuRenderer::~GpuRenderer() {
    if (ctx_) {
        SDL_GL_MakeCurrent(win_, ctx_);
        if (p_) {
            for (auto& [t, id] : p_->textures) gl.DeleteTextures(1, &id);
            GLuint tx[4] = {p_->ambTex, p_->visTex, p_->worldLayerTex, p_->uiTex};
            gl.DeleteTextures(4, tx);
            GLuint b[3] = {p_->worldVbo, p_->spriteVbo, p_->quadVbo};
            gl.DeleteBuffers(3, b);
            GLuint va[3] = {p_->worldVao, p_->spriteVao, p_->quadVao};
            gl.DeleteVertexArrays(3, va);
            for (GLuint pr : {p_->worldProg, p_->spriteProg, p_->skyProg, p_->layerProg})
                if (pr) gl.DeleteProgram(pr);
        }
        SDL_GL_DeleteContext(ctx_);
    }
}

bool GpuRenderer::init(SDL_Window* win, std::string& err) {
    win_ = win;
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    ctx_ = SDL_GL_CreateContext(win);
    if (!ctx_) { err = std::string("OpenGL 3.3 not available: ") + SDL_GetError(); return false; }
    SDL_GL_MakeCurrent(win, ctx_);
    std::string missing;
    if (!gl.load(missing)) { err = "OpenGL function missing: " + missing; return false; }
    if (SDL_GL_SetSwapInterval(-1) != 0) SDL_GL_SetSwapInterval(1);   // vsync (adaptive if possible)
    const char* ven = (const char*)gl.GetString(GL_RENDERER);
    const char* ver = (const char*)gl.GetString(GL_VERSION);
    info_ = std::string(ven ? ven : "?") + ", OpenGL " + (ver ? ver : "?");

    p_ = std::make_unique<Impl>();
    Impl& I = *p_;
    std::string worldFS = std::string("#version 330 core\n") + kLightFn + kWorldFS;
    I.worldProg = program(kWorldVS, worldFS, err);
    if (I.worldProg) I.spriteProg = program(kSpriteVS, kSpriteFS, err);
    if (I.spriteProg) I.skyProg = program(kSkyVS, kSkyFS, err);
    if (I.skyProg) I.layerProg = program(kLayerVS, kLayerFS, err);
    if (!I.layerProg) return false;
    auto camLocs = [](GLuint p) {
        return Impl::Cam{gl.GetUniformLocation(p, "uCam"), gl.GetUniformLocation(p, "uDir"), gl.GetUniformLocation(p, "uRight"),
                         gl.GetUniformLocation(p, "uPlaneLen"), gl.GetUniformLocation(p, "uShift"), gl.GetUniformLocation(p, "uNearFar")};
    };
    I.wCam = camLocs(I.worldProg);
    I.sCam = camLocs(I.spriteProg);
    I.wTex = gl.GetUniformLocation(I.worldProg, "uTex");
    I.wAmb = gl.GetUniformLocation(I.worldProg, "uAmb");
    I.wVis = gl.GetUniformLocation(I.worldProg, "uVis");
    I.wLights = gl.GetUniformLocation(I.worldProg, "uLights");
    I.wNum = gl.GetUniformLocation(I.worldProg, "uNumLights");
    I.sTex = gl.GetUniformLocation(I.spriteProg, "uTex");
    I.sLight = gl.GetUniformLocation(I.spriteProg, "uLight");
    I.sTint = gl.GetUniformLocation(I.spriteProg, "uTint");
    I.sDither = gl.GetUniformLocation(I.spriteProg, "uDither");
    I.skyH = gl.GetUniformLocation(I.skyProg, "uH");
    I.skyHorizon = gl.GetUniformLocation(I.skyProg, "uHorizon");
    I.lTex = gl.GetUniformLocation(I.layerProg, "uTex");

    // Vertex layouts: world and sprites share Vtx; the screen quad is two triangles.
    auto vtxLayout = [](GLuint vao, GLuint vbo) {
        gl.BindVertexArray(vao);
        gl.BindBuffer(GL_ARRAY_BUFFER, vbo);
        gl.EnableVertexAttribArray(0);
        gl.VertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vtx), (void*)offsetof(Vtx, x));
        gl.EnableVertexAttribArray(1);
        gl.VertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(Vtx), (void*)offsetof(Vtx, u));
        gl.EnableVertexAttribArray(2);
        gl.VertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(Vtx), (void*)offsetof(Vtx, tx));
        gl.EnableVertexAttribArray(3);
        gl.VertexAttribPointer(3, 1, GL_FLOAT, GL_FALSE, sizeof(Vtx), (void*)offsetof(Vtx, shade));
    };
    gl.GenVertexArrays(1, &I.worldVao); gl.GenBuffers(1, &I.worldVbo); vtxLayout(I.worldVao, I.worldVbo);
    gl.GenVertexArrays(1, &I.spriteVao); gl.GenBuffers(1, &I.spriteVbo); vtxLayout(I.spriteVao, I.spriteVbo);
    gl.GenVertexArrays(1, &I.quadVao);
    gl.GenBuffers(1, &I.quadVbo);
    gl.BindVertexArray(I.quadVao);
    gl.BindBuffer(GL_ARRAY_BUFFER, I.quadVbo);
    const float q[12] = {-1, -1, 1, -1, 1, 1, -1, -1, 1, 1, -1, 1};
    gl.BufferData(GL_ARRAY_BUFFER, sizeof q, q, GL_STATIC_DRAW);
    gl.EnableVertexAttribArray(0);
    gl.VertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), nullptr);

    // Lighting textures (one texel per map tile, read exactly with texelFetch).
    for (GLuint* t : {&I.ambTex, &I.visTex}) {
        gl.GenTextures(1, t);
        gl.BindTexture(GL_TEXTURE_2D, *t);
        gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    }
    gl.GetFloatv(0x84FF /* MAX_TEXTURE_MAX_ANISOTROPY */, &I.maxAniso);
    gl.GetError();   // not supported everywhere; that's fine
    I.smooth = smooth_;
    if (gl.GetError() != GL_NO_ERROR) { err = "OpenGL error during setup"; return false; }
    return true;
}

void GpuRenderer::setSmooth(bool on) {
    if (on == smooth_) return;
    smooth_ = on;
    if (p_) { p_->smooth = on; dropTextures(); }
}

void GpuRenderer::dropTextures() {
    if (!p_) return;
    SDL_GL_MakeCurrent(win_, ctx_);
    for (auto& [t, id] : p_->textures) gl.DeleteTextures(1, &id);
    p_->textures.clear();
    p_->meshMap = nullptr;   // the mesh refers to textures by pointer: rebuild it too
}

void GpuRenderer::viewRect(int& x, int& y, int& w, int& h) const {
    int ww, wh;
    SDL_GetWindowSize(win_, &ww, &wh);
    // Same shape as the Renderer: 16:9 for every resolution offered. Letterbox the rest.
    int rw = p_ ? p_->worldLayerW : 16, rh = p_ ? p_->worldLayerH : 9;
    if (rw <= 0 || rh <= 0) { rw = 16; rh = 9; }
    if ((long)ww * rh > (long)wh * rw) { h = wh; w = wh * rw / rh; }
    else { w = ww; h = ww * rh / rw; }
    x = (ww - w) / 2;
    y = (wh - h) / 2;
}

void GpuRenderer::present(Renderer& r, bool swap) {
    if (!p_) return;
    Impl& I = *p_;
    SDL_GL_MakeCurrent(win_, ctx_);
    r.endUi();
    int dw, dh, ww, wh;
    SDL_GL_GetDrawableSize(win_, &dw, &dh);
    SDL_GetWindowSize(win_, &ww, &wh);
    I.lastW = dw; I.lastH = dh;
    // Letterbox to the Renderer's shape (in drawable pixels, which may be more than window pixels).
    int vw, vh;
    if ((long)dw * r.H > (long)dh * r.W) { vh = dh; vw = dh * r.W / r.H; }
    else { vw = dw; vh = dw * r.H / r.W; }
    int vx = (dw - vw) / 2, vy = (dh - vh) / 2;
    gl.Viewport(0, 0, dw, dh);
    gl.ClearColor(0, 0, 0, 1);
    gl.DepthMask(GL_TRUE);
    gl.Clear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    gl.Viewport(vx, vy, vw, vh);

    const WorldScene& sc = r.scene();
    if (sc.valid && sc.map && sc.assets && sc.map->w > 0) I.drawWorld(r, sc);

    // The world layer (weapon, brackets, tints) at the 3D view's resolution...
    int x0, y0, x1, y1;
    r.takeWorldDirty(x0, y0, x1, y1);
    I.drawLayer(I.worldLayerTex, I.worldLayerW, I.worldLayerH, r.worldLayer(), r.W, r.H, x0, y0, x1, y1);
    // ...and the HUD/menus, laid out at 640x360.
    if (r.layered() && r.uiUsed()) {
        I.drawLayer(I.uiTex, I.uiW, I.uiH, r.uiLayer(), Renderer::UI_W, Renderer::UI_H, 0, 0, Renderer::UI_W, Renderer::UI_H);
    }
    if (swap) SDL_GL_SwapWindow(win_);
}

std::vector<uint32_t> GpuRenderer::readPixels(int& w, int& h) {
    w = p_ ? p_->lastW : 0;
    h = p_ ? p_->lastH : 0;
    std::vector<uint32_t> out(w * h);
    if (!p_ || w == 0) return out;
    std::vector<uint32_t> raw(w * h);
    gl.PixelStorei(GL_PACK_ALIGNMENT, 4);
    gl.ReadBuffer(GL_BACK);
    gl.ReadPixels(0, 0, w, h, GL_BGRA, GL_UNSIGNED_BYTE, raw.data());
    for (int y = 0; y < h; y++) std::memcpy(&out[y * w], &raw[(h - 1 - y) * w], w * 4);   // GL rows go up
    return out;
}
