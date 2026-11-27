#include "app/session.hpp"

#include <CommonCrypto/CommonDigest.h>
#include <sys/stat.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <vector>

#include "gl/gl.hpp"
#include "host/fail.hpp"
#include "recomp/dispatch.hpp"

namespace voy::recomp {
std::span<const GuestEntry> translated_entries_trek();
std::span<const GuestEntry> translated_entries_stvsnd();
std::span<const GuestEntry> translated_entries_stvserv();
}

namespace voy::app {

namespace {
const char kManifest[] =
#include "manifest.inc"
    ;

std::string sha256_file(const std::string& path, long long* size) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    CC_SHA256_CTX ctx;
    CC_SHA256_Init(&ctx);
    std::vector<char> buf(1 << 16);
    *size = 0;
    while (f) {
        f.read(buf.data(), std::streamsize(buf.size()));
        const auto n = f.gcount();
        if (n > 0) {
            CC_SHA256_Update(&ctx, buf.data(), CC_LONG(n));
            *size += n;
        }
    }
    unsigned char d[CC_SHA256_DIGEST_LENGTH];
    CC_SHA256_Final(d, &ctx);
    char hex[65];
    for (int i = 0; i < 32; ++i) std::snprintf(hex + 2 * i, 3, "%02x", d[i]);
    return hex;
}
}  // namespace

host::Programs built_programs() {
    return {recomp::translated_entries_trek(), recomp::translated_entries_stvsnd(), recomp::translated_entries_stvserv()};
}

std::string verify_content(const std::string& folder) {
    std::istringstream in(kManifest);
    std::string line, problems;
    int missing = 0, wrong = 0;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#') continue;
        std::istringstream ls(line);
        std::string hash, path;
        long long size = 0;
        ls >> hash >> size;
        std::getline(ls, path);
        if (!path.empty() && path[0] == ' ') path = path.substr(1);
        const std::string full = folder + "/" + path;
        if (hash == "-") {
            struct stat st;
            if (::stat(full.c_str(), &st) != 0) {
                if (++missing <= 5) problems += "missing: " + path + "\n";
            } else if (st.st_size != size) {
                if (++wrong <= 5) problems += "wrong size: " + path + "\n";
            }
            continue;
        }
        long long got_size = 0;
        const std::string got = sha256_file(full, &got_size);
        if (got.empty()) {
            if (++missing <= 5) problems += "missing: " + path + "\n";
        } else if (got != hash || got_size != size) {
            if (++wrong <= 5) problems += "not the e4 build (hash differs): " + path + "\n";
        }
    }
    if (missing > 5 || wrong > 5)
        problems += std::to_string(missing) + " files missing and " + std::to_string(wrong) + " different in all\n";
    return problems;
}

std::string default_container() {
    const char* home = std::getenv("HOME");
    return std::string(home ? home : ".") + "/Library/Application Support/VoyagerArcade";
}

namespace {

class HashSink : public gl::FrameSink {
public:
    HashSink(int every, gl::FrameSink* next) : every_(every), next_(next) {}
    void submit(std::shared_ptr<const gl::Frame> f) override {
        ++frames_;
        if (next_) next_->submit(f);
        if (every_ > 0 && f->index % std::uint64_t(every_) == 0) {
            std::uint64_t draws = 0;
            for (auto& c : f->commands) draws += c.kind == gl::Command::Draw;
            std::printf("frame %llu: %zu vertices, %llu draws\n", (unsigned long long)f->index, f->vertices.size(),
                        (unsigned long long)draws);
            std::fflush(stdout);
        }
    }
    std::uint64_t frames_ = 0;

private:
    int every_;
    gl::FrameSink* next_;
};

}  // namespace

int run_headless(const Options& o) {
    host::Config c = o.config;
    if (c.container.empty()) c.container = default_container() + "/headless";
    const std::string bad = verify_content(c.content);
    if (!bad.empty()) {
        std::fprintf(stderr, "The game folder does not match the e4 build:\n%s", bad.c_str());
        return 3;
    }
    host::Machine m(c, built_programs());
    HashSink sink(o.hash_every, o.sink);
    m.gl().set_sink(&sink);
    const int code = m.run();
    std::printf("exit %d after %llu frames, last frame hash %016llx\n", code, (unsigned long long)m.frames_drawn(),
                (unsigned long long)m.gl().last_frame_hash());
    return code < 0 ? 1 : 0;
}

}
