// Differential test of the decompiled functions (src/decomp): each one runs
// beside the translation it replaced, on the same random inputs in a loaded
// trek's guest memory, and their outputs (memory, eax, st(0)) must agree.
// Floats may differ by the last bits where x87 kept 64-bit intermediates.
// Needs the game folder (VOY_GAME_DIR); skipped (77) without it.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "app/session.hpp"
#include "host/machine.hpp"
#include "host/process.hpp"
#include "recomp/dispatch.hpp"
#include "recomp/x86ops.hpp"

namespace voy::recomp {
std::span<const DecompPair> decomp_pairs_trek();
}

using namespace voy;

namespace {

// Argument kinds: p = pointer to a 64-byte block of floats, f = a float.
// Return kinds: v = none, f = st(0), i = eax.
struct Sig {
    const char* name;
    const char* args;
    char ret;
};
const Sig kSigs[] = {
    {"dot_product", "pp", 'f'},       {"vector_magnitude", "p", 'f'},   {"stuff_a_to_b", "ppp", 'v'},
    {"accumulate_xyz", "pp", 'v'},    {"stuff_scaled_vector", "pfp", 'v'}, {"stuff_cross_product", "ppp", 'v'},
    {"d_squared", "pp", 'f'},         {"dist_a_to_b", "pp", 'f'},       {"stuff_xyz", "pfff", 'v'},
    {"zero_xyz", "p", 'v'},           {"copyxyz", "pp", 'v'},           {"stuff_sum_xyz3", "pppp", 'v'},
    {"stuff_sum_xyz4", "ppppp", 'v'}, {"absval", "f", 'f'},             {"abs_float", "f", 'f'},
    {"stuff_mtrx_identity", "p", 'v'}, {"stuff_normalized_vector", "pp", 'v'},
};

bool close(float a, float b) {
    if (std::memcmp(&a, &b, 4) == 0) return true;
    if (std::isnan(a) && std::isnan(b)) return true;
    return std::fabs(double(a) - b) <= 1e-6 * std::max(1.0, std::fabs(double(a)));
}

struct Result {
    std::vector<std::uint8_t> mem;
    double st0 = 0;
    std::uint32_t eax = 0;
};

Result call(host::Process& p, recomp::GuestFn fn, const Sig& sig, const std::vector<std::uint32_t>& blocks,
            const std::vector<std::uint8_t>& init, const std::vector<std::uint32_t>& scalars) {
    recomp::Cpu& c = p.cpu();
    for (std::size_t i = 0; i < blocks.size(); ++i) std::memcpy(p.host(blocks[i]), init.data() + 64 * i, 64);
    std::vector<std::uint32_t> args;
    std::size_t bi = 0, si = 0;
    for (const char* a = sig.args; *a; ++a) args.push_back(*a == 'p' ? blocks[bi++] : scalars[si++]);
    const std::uint32_t saved = c.r[recomp::Cpu::ESP];
    std::uint32_t esp = saved;
    for (auto it = args.rbegin(); it != args.rend(); ++it) {
        esp -= 4;
        recomp::st32(c.mem, esp, *it);
    }
    esp -= 4;
    recomp::st32(c.mem, esp, 0);
    c.r[recomp::Cpu::ESP] = esp;
    const unsigned top = c.fpu.top;
    fn(c);
    Result r;
    r.eax = c.r[recomp::Cpu::EAX];
    if (sig.ret == 'f' && c.fpu.top != top) r.st0 = c.fpu.pop();
    c.r[recomp::Cpu::ESP] = saved;
    for (auto b : blocks) {
        const auto* m = static_cast<const std::uint8_t*>(p.host(b));
        r.mem.insert(r.mem.end(), m, m + 64);
    }
    return r;
}

}  // namespace

int main() {
    const char* game = std::getenv("VOY_GAME_DIR");
    if (!game || !*game) {
        std::printf("VOY_GAME_DIR is not set: skipped\n");
        return 77;
    }
    host::Config cfg;
    cfg.content = game;
    cfg.container = std::string(std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "/tmp") + "/voy-decomp-diff";
    cfg.headless = true;
    host::Machine m(cfg, app::built_programs());
    host::Process p("trek", m);
    std::string err;
    if (!p.load(std::string(game) + "/trek", app::built_programs().trek, &err)) {
        std::fprintf(stderr, "cannot load trek: %s\n", err.c_str());
        return 1;
    }
    std::mt19937 rng(1234);
    std::uniform_real_distribution<float> val(-1000.0f, 1000.0f);
    int failures = 0, checked = 0;
    for (const auto& pair : recomp::decomp_pairs_trek()) {
        const Sig* sig = nullptr;
        for (const auto& s : kSigs)
            if (std::strcmp(s.name, pair.name) == 0) sig = &s;
        if (!sig) {
            std::fprintf(stderr, "%s: no signature in the test table\n", pair.name);
            ++failures;
            continue;
        }
        recomp::GuestFn decomp = recomp::find_entry(p.cpu(), pair.address);  // f_<address>: decompiled
        int nb = 0, ns = 0;
        for (const char* a = sig->args; *a; ++a) (*a == 'p' ? nb : ns)++;
        std::vector<std::uint32_t> blocks;
        for (int i = 0; i < nb; ++i) blocks.push_back(p.malloc(64));
        for (int round = 0; round < 2000; ++round) {
            std::vector<std::uint8_t> init(64 * std::size_t(nb));
            for (std::size_t i = 0; i < init.size(); i += 4) {
                float f = (round % 50 == 0) ? 0.0f : val(rng);
                std::memcpy(init.data() + i, &f, 4);
            }
            std::vector<std::uint32_t> scalars;
            for (int i = 0; i < ns; ++i) {
                float f = val(rng);
                std::uint32_t b;
                std::memcpy(&b, &f, 4);
                scalars.push_back(b);
            }
            const Result a = call(p, pair.translated, *sig, blocks, init, scalars);
            const Result b = call(p, decomp, *sig, blocks, init, scalars);
            bool ok = a.mem.size() == b.mem.size();
            for (std::size_t i = 0; ok && i < a.mem.size(); i += 4) {
                float x, y;
                std::memcpy(&x, a.mem.data() + i, 4);
                std::memcpy(&y, b.mem.data() + i, 4);
                ok = close(x, y);
            }
            if (sig->ret == 'f') ok = ok && close(float(a.st0), float(b.st0));
            if (sig->ret == 'i') ok = ok && a.eax == b.eax;
            ++checked;
            if (!ok) {
                std::fprintf(stderr, "%s differs (round %d): st0 %.9g vs %.9g\n", pair.name, round, a.st0, b.st0);
                ++failures;
                break;
            }
        }
    }
    std::printf("%zu decompiled functions, %d calls compared, %d failures\n", recomp::decomp_pairs_trek().size(), checked,
                failures);
    return failures ? 1 : 0;
}
