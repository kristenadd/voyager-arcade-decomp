// Tests that need no game files: the libc pieces the port reimplements and
// the GL transform math, checked against known glibc / OpenGL results.
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

#include "gl/gl.hpp"
#include "host/format.hpp"
#include "host/machine.hpp"
#include "host/os.hpp"
#include "host/process.hpp"

using namespace voy;

static int g_failed = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            ++g_failed;                                                          \
        }                                                                        \
    } while (0)

namespace {

class NullSink : public gl::FrameSink {
public:
    std::shared_ptr<const gl::Frame> last;
    void submit(std::shared_ptr<const gl::Frame> f) override { last = std::move(f); }
};

void test_rand() {
    // glibc: srand(1); rand() → 1804289383, 846930886, 1681692777.
    host::OsState os;
    os.srand(1);
    CHECK(os.rand() == 1804289383);
    CHECK(os.rand() == 846930886);
    CHECK(os.rand() == 1681692777);
}

void test_gl() {
    gl::Context g;
    NullSink sink;
    g.set_sink(&sink);
    g.MatrixMode(0x1701);
    g.LoadIdentity();
    g.Ortho(-256, 256, -192, 192, -10, 50);
    g.MatrixMode(0x1700);
    g.LoadIdentity();
    g.Begin(8);  // GL_QUAD_STRIP
    g.Vertex4(-256, -192, 0, 1);
    g.Vertex4(0, -192, 0, 1);
    g.Vertex4(-256, 192, 0, 1);
    g.Vertex4(0, 192, 0, 1);
    g.End();
    g.swap();
    CHECK(sink.last && sink.last->vertices.size() == 6);
    if (sink.last && sink.last->vertices.size() == 6) {
        const auto& v = sink.last->vertices[0];
        CHECK(std::fabs(v.x + 1) < 1e-6 && std::fabs(v.y + 1) < 1e-6 && std::fabs(v.w - 1) < 1e-6);
        // z = 0 maps to GL NDC -10/60*... then Metal's 0..1: (z + w) / 2
        const double zndc = -(2.0 / 60.0) * 0 - (50.0 - 10.0) / 60.0;
        CHECK(std::fabs(v.z - float((zndc + 1) / 2)) < 1e-6);
    }
    // Back faces are culled when GL_CULL_FACE is on (counter-clockwise is front).
    g.Enable(0x0B44, true);
    g.Begin(4);
    g.Vertex4(0, 0, 0, 1);
    g.Vertex4(0, 100, 0, 1);  // clockwise
    g.Vertex4(100, 0, 0, 1);
    g.End();
    g.swap();
    CHECK(!sink.last || sink.last->vertices.empty());
    // A rotation of 90 degrees about z takes x to y.
    g.Enable(0x0B44, false);
    g.MatrixMode(0x1701);
    g.LoadIdentity();
    g.MatrixMode(0x1700);
    g.LoadIdentity();
    g.Rotate(90, 0, 0, 1);
    g.Begin(0);
    g.Vertex4(0.5f, 0, 0, 1);
    g.End();
    g.swap();
    CHECK(sink.last && sink.last->vertices.size() == 1 && std::fabs(sink.last->vertices[0].y - 0.5f) < 1e-6);
}

void test_textures() {
    gl::Context g;
    NullSink sink;
    g.set_sink(&sink);
    g.BindTexture(0x0DE1, 7);
    // BGRA 1555_REV: alpha set, pure red.
    const std::uint8_t px[2] = {0x00, 0xFC};
    CHECK(g.TexImage(0x0DE1, 0, 4, 1, 1, 0x80E1, 0x8366, px, false));
    g.Enable(0x0DE1, true);
    g.Begin(0);
    g.Vertex4(0, 0, 0, 1);
    g.End();
    g.swap();
    CHECK(sink.last && !sink.last->commands.empty() && sink.last->commands[0].state.tex);
    if (sink.last && !sink.last->commands.empty() && sink.last->commands[0].state.tex) {
        const auto& t = *sink.last->commands[0].state.tex;
        CHECK(t.rgba[0] == 255 && t.rgba[1] == 0 && t.rgba[2] == 0 && t.rgba[3] == 255);
    }
    // Changing a texture a frame still holds copies it first.
    auto held = sink.last;
    const std::uint8_t px2[2] = {0x1F, 0x80};  // blue
    g.TexImage(0x0DE1, 0, 4, 1, 1, 0x80E1, 0x8366, px2, false);
    CHECK(held->commands[0].state.tex->rgba[0] == 255);
}

void test_scan() {
    host::StringScanSource s("  42 3.5 hello,world");
    int n = -1;
    CHECK(s.scan_one(" %d", &n) == 1 && n == 42);
    float f = 0;
    CHECK(s.scan_one(" %f", &f) == 1 && std::fabs(f - 3.5f) < 1e-6);
    char buf[32] = {};
    CHECK(s.scan_one(" %[^,]", buf) == 1 && std::strcmp(buf, "hello") == 0);
}

}  // namespace

int main() {
    test_rand();
    test_gl();
    test_textures();
    test_scan();
    if (g_failed) {
        std::fprintf(stderr, "%d checks failed\n", g_failed);
        return 1;
    }
    std::printf("all unit checks passed\n");
    return 0;
}
