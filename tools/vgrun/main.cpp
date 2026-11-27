// vgrun: runs the cabinet without a window (tests, scripted runs).
//
//   vgrun --content <game folder> [--container <folder>] [--frames N]
//         [--no-sound] [--stdout] [--script <input script>] [--hash-every N]
//
// Time is virtual: the 60 Hz vblank advances only as the game waits for it,
// so a run is deterministic and as fast as the CPU allows.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "app/session.hpp"
#include "gpu/renderer.hpp"

namespace {

// --frame-dump <prefix> --hash-every N: every Nth frame drawn with Metal and
// written as <prefix>_<frame>.png.
class DumpSink : public voy::gl::FrameSink {
public:
    DumpSink(std::string prefix, int every) : prefix_(std::move(prefix)), every_(every > 0 ? every : 60) {
        voy::gpu::RendererOptions ro;
        ro.scale = 2;
        renderer_ = voy::gpu::Renderer::create(nullptr, ro);
    }
    void submit(std::shared_ptr<const voy::gl::Frame> f) override {
        if (!renderer_ || f->index % std::uint64_t(every_) != 0) return;
        renderer_->draw(*f);
        std::vector<std::uint8_t> px;
        int w = 0, h = 0;
        if (renderer_->read_rgba(px, &w, &h)) {
            char name[64];
            std::snprintf(name, sizeof name, "_%06llu.png", (unsigned long long)f->index);
            voy::gpu::write_png(prefix_ + name, px, w, h);
        }
    }

private:
    std::string prefix_;
    int every_;
    std::unique_ptr<voy::gpu::Renderer> renderer_;
};

}  // namespace

int main(int argc, char** argv) {
    voy::app::Options o;
    o.config.headless = true;
    o.config.sound = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "%s needs a value\n", a.c_str());
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "--content") o.config.content = next();
        else if (a == "--container") o.config.container = next();
        else if (a == "--frames") o.config.max_frames = std::atoi(next().c_str());
        else if (a == "--sound") o.config.sound = true;
        else if (a == "--no-sound") o.config.sound = false;
        else if (a == "--stdout") o.config.stdout_echo = true;
        else if (a == "--free-play") o.config.free_play = true;
        else if (a == "--script") o.config.input_script = next();
        else if (a == "--hash-every") o.hash_every = std::atoi(next().c_str());
        else if (a == "--frame-dump") o.frame_dump = next();
        else {
            std::fprintf(stderr, "usage: vgrun --content <game folder> [--container <dir>] [--frames N] [--sound] "
                                 "[--stdout] [--free-play] [--script <file>] [--hash-every N] [--frame-dump <prefix>]\n");
            return 2;
        }
    }
    if (o.config.content.empty()) {
        std::fprintf(stderr, "vgrun: --content is required\n");
        return 2;
    }
    std::unique_ptr<DumpSink> dump;
    if (!o.frame_dump.empty()) {
        dump = std::make_unique<DumpSink>(o.frame_dump, o.hash_every);
        o.sink = dump.get();
    }
    return voy::app::run_headless(o);
}
