#pragma once
// Draws gl::Frames with Metal: into an offscreen target at the game's
// 512 x 384 times `scale` (multisampled), then presents that target into a
// drawable, letterboxed to 4:3. Used by the Mac and iOS apps and by vgrun's
// frame dumps.
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "gl/gl.hpp"

namespace voy::gpu {

struct RendererOptions {
    int scale = 3;          // offscreen size = 512 x 384 times this
    int samples = 4;        // MSAA samples (1 = off)
    bool smooth_present = true;
};

class Renderer {
public:
    // `device` is an id<MTLDevice> (nullptr: the system default).
    static std::unique_ptr<Renderer> create(void* device, RendererOptions options = {});
    virtual ~Renderer() = default;

    // Renders a frame into the offscreen target.
    virtual void draw(const gl::Frame& frame) = 0;
    // Presents the offscreen target into `drawable` (an id<CAMetalDrawable>)
    // of size w x h pixels, letterboxed; `x0, y0, x1, y1` receives where the
    // game image landed (for mapping touches and clicks). Nullable.
    virtual void present(void* drawable, int w, int h, float* rect = nullptr) = 0;
    // Crosshairs drawn over the game at present (gamepad and touch aiming):
    // game coordinates (512 x 384), RGB colour. Hidden when visible is false.
    struct Cursor {
        bool visible = false;
        float x = 0, y = 0;
        float r = 1, g = 1, b = 1;
    };
    virtual void set_cursors(const Cursor* cursors, int n) = 0;
    // Waits for the GPU and reads the offscreen target back as RGBA8.
    virtual bool read_rgba(std::vector<std::uint8_t>& out, int* w, int* h) = 0;
};

// Writes RGBA8 pixels as a PNG.
bool write_png(const std::string& path, const std::vector<std::uint8_t>& rgba, int w, int h);

}
