#pragma once
// OpenGL 1.x, the subset trek and stvserv call, emulated on the CPU.
//
// The game draws in immediate mode (glBegin / glVertex / glEnd) with the
// fixed-function pipeline. The Context keeps GL's state machine, transforms
// and lights each vertex as GL 1.x specifies, assembles primitives (culling,
// flat shading, quads and polygons into triangles) and records a Frame: clip-
// space vertices plus a list of clears and draws, each draw carrying the
// state it needs (texture image, blend, depth, masks, viewport, scissor).
// At glutSwapBuffers the Frame goes to a FrameSink: the Metal renderer on the
// Mac and iOS, or a hashing sink for headless tests.
//
// Textures are kept as RGBA8 images on the CPU. A TexImage is immutable once
// a Frame refers to it; changing a texture after that copies it first, so a
// frame always draws with the contents the game had at that draw.
#include <array>
#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace voy::gl {

// The framebuffer size the game is told (its GLUT window / game mode).
constexpr int kWidth = 512, kHeight = 384;  // the cabinet ran X at 512 x 384 (mj_set_numlines takes 240 or 384 lines)

struct TexImage {
    int width = 0, height = 0;
    std::vector<std::uint8_t> rgba;  // width * height * 4
    // The base format's components (GL 1.x texture environment rules differ):
    // 0 RGBA, 1 RGB, 2 ALPHA, 3 LUMINANCE, 4 LUMINANCE_ALPHA, 5 INTENSITY.
    int format = 0;
    std::uint32_t min_filter = 0x2601, mag_filter = 0x2601;  // GL_LINEAR
    std::uint32_t wrap_s = 0x2901, wrap_t = 0x2901;          // GL_REPEAT
    bool mipmaps = false;
    std::uint64_t serial = 0;  // unique per image version (renderer cache key)
};

struct Vertex {
    float x, y, z, w;   // clip space, z already in Metal's 0..w range
    float r, g, b, a;
    float s, t;
};

enum class Prim : std::uint8_t { Triangles, Lines, Points };

struct DrawState {
    std::shared_ptr<const TexImage> tex;  // null: untextured
    std::uint32_t env_mode = 0x2100;      // GL_MODULATE
    bool blend = false;
    std::uint32_t blend_src = 1, blend_dst = 0;  // GL_ONE, GL_ZERO
    bool depth_test = false;
    std::uint32_t depth_func = 0x0201;  // GL_LESS
    bool depth_write = true;
    std::uint8_t color_mask = 0xF;      // r g b a bits
    bool scissor = false;
    int scissor_box[4] = {0, 0, kWidth, kHeight};  // GL: x, y (bottom-up), w, h
    int viewport[4] = {0, 0, kWidth, kHeight};
    bool polygon_offset = false;
    float offset_factor = 0, offset_units = 0;
    bool alpha_test = false;
    std::uint32_t alpha_func = 0x0207;  // GL_ALWAYS
    float alpha_ref = 0;

    bool same_as(const DrawState& o) const;
};

struct Command {
    enum Kind : std::uint8_t { Clear, Draw } kind = Draw;
    Prim prim = Prim::Triangles;
    DrawState state;
    std::uint32_t first = 0, count = 0;  // Draw: vertex range
    std::uint32_t clear_mask = 0;        // Clear: GL_COLOR_BUFFER_BIT / GL_DEPTH_BUFFER_BIT
    float clear_color[4] = {0, 0, 0, 0};
    float clear_depth = 1;
};

struct Frame {
    std::vector<Vertex> vertices;
    std::vector<Command> commands;
    std::uint64_t index = 0;
};

class FrameSink {
public:
    virtual ~FrameSink() = default;
    virtual void submit(std::shared_ptr<const Frame> frame) = 0;
};

class Context {
public:
    Context();
    void set_sink(FrameSink* sink) { sink_ = sink; }
    // Resets to the state of a new GL context (a program starting).
    void reset();
    // glutSwapBuffers / glXSwapBuffers: ends the frame.
    void swap();
    std::uint64_t frames() const { return frame_index_; }
    // A hash of the last swapped frame's commands and vertices (tests).
    std::uint64_t last_frame_hash() const { return last_hash_; }

    // ---- GL entry points (guest pointers already resolved to host memory) ----
    void Enable(std::uint32_t cap, bool on);
    void MatrixMode(std::uint32_t mode) { matrix_mode_ = mode; }
    void LoadIdentity();
    void PushMatrix();
    void PopMatrix();
    void MultMatrix(const double m[16]);
    void Translate(double x, double y, double z);
    void Rotate(double angle, double x, double y, double z);
    void Scale(double x, double y, double z);
    void Frustum(double l, double r, double b, double t, double n, double f);
    void Ortho(double l, double r, double b, double t, double n, double f);
    void Perspective(double fovy, double aspect, double n, double f);
    void LookAt(const double e[3], const double c[3], const double u[3]);
    void Viewport(int x, int y, int w, int h);
    void Scissor(int x, int y, int w, int h);

    void Begin(std::uint32_t mode);
    void End();
    void Vertex4(float x, float y, float z, float w);
    void Color(float r, float g, float b, float a) { color_ = {r, g, b, a}; }
    void Normal(float x, float y, float z) { normal_ = {x, y, z}; }
    void TexCoord(float s, float t) { texcoord_ = {s, t}; }
    void Rect(float x1, float y1, float x2, float y2);

    void ClearColor(float r, float g, float b, float a) { clear_color_ = {r, g, b, a}; }
    void ClearDepth(double d) { clear_depth_ = float(d); }
    void Clear(std::uint32_t mask);
    void ColorMask(bool r, bool g, bool b, bool a) { state_.color_mask = std::uint8_t(r | g << 1 | b << 2 | a << 3); }
    void DepthMask(bool on) { state_.depth_write = on; }
    void DepthFunc(std::uint32_t f) { state_.depth_func = f; }
    void BlendFunc(std::uint32_t s, std::uint32_t d) { state_.blend_src = s; state_.blend_dst = d; }
    void CullFace(std::uint32_t mode) { cull_face_ = mode; }
    void ShadeModel(std::uint32_t mode) { flat_ = mode == 0x1D00; }
    void PolygonOffset(float factor, float units) { state_.offset_factor = factor; state_.offset_units = units; }
    void AlphaFunc(std::uint32_t f, float ref) { state_.alpha_func = f; state_.alpha_ref = ref; }

    void Light(std::uint32_t light, std::uint32_t pname, const float* v);
    void LightModel(std::uint32_t pname, const float* v);
    void Material(std::uint32_t face, std::uint32_t pname, const float* v);

    void BindTexture(std::uint32_t target, std::uint32_t id);
    void DeleteTextures(int n, const std::uint32_t* ids);
    void TexParameter(std::uint32_t target, std::uint32_t pname, std::uint32_t value);
    void TexEnv(std::uint32_t pname, float value);
    void PixelStore(std::uint32_t pname, int value);
    // level 0 only; other levels are generated. Returns false for a format
    // the port does not convert (reported by the caller).
    bool TexImage(std::uint32_t target, int level, int internal, int w, int h, std::uint32_t format, std::uint32_t type,
                  const std::uint8_t* pixels, bool build_mipmaps);
    bool TexSubImage(std::uint32_t target, int level, int x, int y, int w, int h, std::uint32_t format, std::uint32_t type,
                     const std::uint8_t* pixels);

    // glGetFloatv / glGetIntegerv: false for an unknown pname.
    bool GetFloat(std::uint32_t pname, float* out, int* count);

private:
    using Mat = std::array<double, 16>;  // column-major, as GL
    struct LightState {
        bool on = false;
        std::array<float, 4> ambient{0, 0, 0, 1}, diffuse{0, 0, 0, 1}, specular{0, 0, 0, 1};
        std::array<float, 4> position{0, 0, 1, 0};  // eye space
        std::array<float, 3> spot_dir{0, 0, -1};
        float spot_exp = 0, spot_cutoff = 180, att[3] = {1, 0, 0};
    };
    struct MaterialState {
        std::array<float, 4> ambient{0.2f, 0.2f, 0.2f, 1}, diffuse{0.8f, 0.8f, 0.8f, 1}, specular{0, 0, 0, 1},
            emission{0, 0, 0, 1};
        float shininess = 0;
    };
    struct Pending {
        float cx, cy, cz, cw;
        float r, g, b, a;
        float s, t;
    };

    Mat& top();
    void mult(const Mat& m);
    std::array<float, 4> light_vertex(const std::array<float, 3>& eye, const std::array<float, 3>& n) const;
    void emit(Prim prim, const Pending* v, int n);
    void flush_primitive();
    void begin_frame_if_needed();
    std::shared_ptr<gl::TexImage>& tex_slot(std::uint32_t id);
    std::shared_ptr<gl::TexImage> writable(std::uint32_t id);
    DrawState current_draw_state() const;

    FrameSink* sink_ = nullptr;
    std::shared_ptr<Frame> frame_;
    std::uint64_t frame_index_ = 0, last_hash_ = 0, serial_ = 1;

    std::uint32_t matrix_mode_ = 0x1700;  // GL_MODELVIEW
    std::vector<Mat> modelview_, projection_, texture_;
    DrawState state_;
    std::array<float, 4> clear_color_{0, 0, 0, 0};
    float clear_depth_ = 1;
    bool cull_ = false, lighting_ = false, normalize_ = false, flat_ = false, tex2d_ = false, tex1d_ = false;
    bool two_side_ = false, local_viewer_ = false;
    std::uint32_t cull_face_ = 0x0405;  // GL_BACK
    std::array<float, 4> color_{1, 1, 1, 1};
    std::array<float, 3> normal_{0, 0, 1};
    std::array<float, 2> texcoord_{0, 0};
    std::array<LightState, 8> lights_;
    std::array<float, 4> model_ambient_{0.2f, 0.2f, 0.2f, 1};
    MaterialState material_[2];  // front, back
    int unpack_alignment_ = 4;
    std::uint32_t bound_2d_ = 0, bound_1d_ = 0;
    std::unordered_map<std::uint32_t, std::shared_ptr<gl::TexImage>> textures_;
    std::uint32_t env_mode_ = 0x2100;

    std::uint32_t begin_mode_ = 0xFFFFFFFF;
    std::vector<Pending> prim_;
};

}
