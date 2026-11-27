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
    return voy::app::run_headless(o);
}
