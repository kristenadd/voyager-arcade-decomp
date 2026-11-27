// printf and scanf families over guest memory. Each conversion is handed to
// the host's own printf / scanf with a one-conversion format, so the results
// match glibc's for every flag, width and precision the game uses. i386
// sizes: int, long and pointers are 4 bytes, long long 8, double 8; float
// varargs arrive as doubles.
#include "host/format.hpp"

#include <cstdio>
#include <cstring>
#include <vector>

#include "host/fail.hpp"
#include "host/process.hpp"

namespace voy::host {

using namespace recomp;

std::string guest_vformat(Process& p, std::uint32_t fmt_addr, std::uint32_t args) {
    const std::string fmt = p.str(fmt_addr);
    std::string out;
    std::uint32_t ap = args;
    auto next32 = [&]() { std::uint32_t v = p.u32(ap); ap += 4; return v; };
    auto next64 = [&]() { std::uint64_t v = ld64(p.mem(), ap); ap += 8; return v; };
    char buf[512];
    for (std::size_t i = 0; i < fmt.size();) {
        if (fmt[i] != '%') {
            out += fmt[i++];
            continue;
        }
        std::size_t j = i + 1;
        if (j < fmt.size() && fmt[j] == '%') {
            out += '%';
            i = j + 1;
            continue;
        }
        std::string spec = "%";
        while (j < fmt.size() && std::strchr("-+ #0", fmt[j])) spec += fmt[j++];
        auto number = [&]() {
            if (j < fmt.size() && fmt[j] == '*') {
                spec += std::to_string(std::int32_t(next32()));
                ++j;
                return;
            }
            while (j < fmt.size() && fmt[j] >= '0' && fmt[j] <= '9') spec += fmt[j++];
        };
        number();
        if (j < fmt.size() && fmt[j] == '.') {
            spec += fmt[j++];
            number();
        }
        int longs = 0;
        bool half = false;
        while (j < fmt.size() && std::strchr("hlLqjzt", fmt[j])) {
            if (fmt[j] == 'l' || fmt[j] == 'q' || fmt[j] == 'L') ++longs;
            if (fmt[j] == 'h') half = true;
            ++j;
        }
        if (j >= fmt.size()) break;
        const char conv = fmt[j];
        i = j + 1;
        switch (conv) {
        case 'd': case 'i': case 'u': case 'x': case 'X': case 'o': case 'c': {
            if (longs >= 2 && conv != 'c') {
                std::string s = spec + "ll" + conv;
                std::snprintf(buf, sizeof buf, s.c_str(), (long long)next64());
            } else {
                std::uint32_t v = next32();
                if (half && conv != 'c') v = (conv == 'd' || conv == 'i') ? std::uint32_t(std::int32_t(std::int16_t(v))) : (v & 0xFFFF);
                std::string s = spec + conv;
                std::snprintf(buf, sizeof buf, s.c_str(), (conv == 'd' || conv == 'i') ? int(std::int32_t(v)) : int(v));
            }
            out += buf;
            break;
        }
        case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A': {
            double d;
            if (longs && spec.find('L') == std::string::npos && false) d = 0;
            std::uint64_t bits = next64();
            std::memcpy(&d, &bits, 8);
            std::string s = spec + conv;
            std::snprintf(buf, sizeof buf, s.c_str(), d);
            out += buf;
            break;
        }
        case 's': {
            const std::uint32_t a = next32();
            const std::string v = a ? p.str(a) : std::string("(null)");
            std::string s = spec + 's';
            std::vector<char> big(v.size() + 512);
            std::snprintf(big.data(), big.size(), s.c_str(), v.c_str());
            out += big.data();
            break;
        }
        case 'p': {
            std::snprintf(buf, sizeof buf, "0x%x", next32());
            out += buf;
            break;
        }
        case 'n': {
            p.set_u32(next32(), std::uint32_t(out.size()));
            break;
        }
        default:
            note_once("printf", std::string("conversion %") + conv);
            break;
        }
    }
    return out;
}

namespace {

// One scanf conversion: the literal text before it (whitespace included),
// the conversion, and what it stores.
struct ScanPiece {
    std::string format;  // literal + one conversion (or literal only)
    char conv = 0;
    bool suppress = false;
    int longs = 0;
    bool half = false;
    int width = 0;
};

std::vector<ScanPiece> parse_scan(const std::string& fmt) {
    std::vector<ScanPiece> out;
    std::string lit;
    for (std::size_t i = 0; i < fmt.size();) {
        if (fmt[i] != '%') {
            lit += fmt[i++];
            continue;
        }
        if (i + 1 < fmt.size() && fmt[i + 1] == '%') {
            lit += "%%";
            i += 2;
            continue;
        }
        ScanPiece piece;
        std::size_t j = i + 1;
        std::string spec = "%";
        if (j < fmt.size() && fmt[j] == '*') {
            piece.suppress = true;
            ++j;
        }
        while (j < fmt.size() && fmt[j] >= '0' && fmt[j] <= '9') {
            piece.width = piece.width * 10 + (fmt[j] - '0');
            spec += fmt[j++];
        }
        while (j < fmt.size() && std::strchr("hlLqjzt", fmt[j])) {
            if (fmt[j] == 'l' || fmt[j] == 'L' || fmt[j] == 'q') ++piece.longs;
            if (fmt[j] == 'h') piece.half = true;
            ++j;
        }
        if (j >= fmt.size()) break;
        piece.conv = fmt[j];
        std::string conv(1, fmt[j]);
        if (fmt[j] == '[') {
            std::size_t k = j + 1;
            if (k < fmt.size() && fmt[k] == '^') ++k;
            if (k < fmt.size() && fmt[k] == ']') ++k;
            while (k < fmt.size() && fmt[k] != ']') ++k;
            conv = fmt.substr(j, k - j + 1);
            j = k;
        }
        i = j + 1;
        piece.format = lit + spec + conv;
        lit.clear();
        out.push_back(piece);
    }
    if (!lit.empty()) {
        ScanPiece tail;
        tail.format = lit;
        out.push_back(tail);
    }
    return out;
}

}  // namespace

int guest_vscan(Process& p, ScanSource& src, std::uint32_t fmt_addr, std::uint32_t args) {
    const auto pieces = parse_scan(p.str(fmt_addr));
    std::uint32_t ap = args;
    int assigned = 0;
    bool any_input = false;
    for (const auto& piece : pieces) {
        if (!piece.conv) {
            src.scan_literal(piece.format);
            break;
        }
        union {
            int i;
            unsigned u;
            float f;
            double d;
            long long ll;
            char c[1];
        } v{};
        std::vector<char> text;
        void* target = &v;
        std::string fmt = piece.format;
        const char conv = piece.conv;
        if (conv == 's' || conv == '[' || conv == 'c') {
            text.assign(piece.width ? piece.width + 1 : 65536, 0);
            target = text.data();
            if (conv == 'c' && !piece.width) text.assign(2, 0);
        } else if (conv == 'n') {
            // handled by the source: characters consumed so far
        } else if (std::strchr("eEfgGaA", conv)) {
            if (piece.longs) {
                // %lf: double. Strip the l: the host conversion below is %lf too.
            }
        } else if (piece.longs >= 2) {
            // %lld
        } else if (piece.longs == 1) {
            // %ld is 4 bytes on i386: scan as int.
            std::string f2;
            for (std::size_t k = 0; k < fmt.size(); ++k)
                if (!(fmt[k] == 'l' && k + 1 < fmt.size() && std::strchr("diuxXo", fmt[k + 1]))) f2 += fmt[k];
            fmt = f2;
        }
        if (piece.suppress) {
            // keep the '*' in the host format: nothing is stored
            const std::size_t pct = fmt.rfind('%');
            fmt.insert(pct + 1, "*");
        }
        if (conv == 'n') {
            if (!piece.suppress) p.set_u32(p.u32(ap), std::uint32_t(src.consumed()));
            if (!piece.suppress) ap += 4;
            continue;
        }
        int r = src.scan_one(fmt, piece.suppress ? nullptr : target);
        if (r == EOF) return any_input || assigned ? assigned : EOF;
        any_input = true;
        if (r == 0 && !piece.suppress) return assigned;
        if (r < 0) return assigned;
        if (piece.suppress) continue;
        const std::uint32_t dst = p.u32(ap);
        ap += 4;
        if (conv == 's' || conv == '[') {
            std::memcpy(p.host(dst), text.data(), std::strlen(text.data()) + 1);
        } else if (conv == 'c') {
            std::memcpy(p.host(dst), text.data(), piece.width ? piece.width : 1);
        } else if (std::strchr("eEfgGaA", conv)) {
            if (piece.longs) stf64(p.mem(), dst, v.d);
            else stf32(p.mem(), dst, v.f);
        } else if (piece.longs >= 2) {
            st64(p.mem(), dst, std::uint64_t(v.ll));
        } else if (piece.half) {
            st16(p.mem(), dst, std::uint16_t(v.i));
        } else {
            st32(p.mem(), dst, std::uint32_t(v.i));
        }
        ++assigned;
    }
    return assigned;
}

// ---- sources ----

int StringScanSource::scan_one(const std::string& fmt, void* target) {
    if (pos_ >= s_.size()) {
        // Only whitespace-skipping conversions see the end as EOF.
        return EOF;
    }
    int n = -1;
    std::string f = fmt + "%n";
    int r;
    if (target) r = std::sscanf(s_.c_str() + pos_, f.c_str(), target, &n);
    else r = std::sscanf(s_.c_str() + pos_, f.c_str(), &n);
    if (n >= 0) pos_ += std::size_t(n);
    if (!target) return n >= 0 ? 1 : (r == EOF ? EOF : 0);
    return r;
}

void StringScanSource::scan_literal(const std::string& lit) {
    int n = -1;
    std::string f = lit + "%n";
    std::sscanf(s_.c_str() + pos_, f.c_str(), &n);
    if (n > 0) pos_ += std::size_t(n);
}

int FileScanSource::scan_one(const std::string& fmt, void* target) {
    int n = -1;
    std::string f = fmt + "%n";
    const long before = std::ftell(fp_);
    int r = target ? std::fscanf(fp_, f.c_str(), target, &n) : std::fscanf(fp_, f.c_str(), &n);
    consumed_ += std::size_t(std::ftell(fp_) - before);
    if (!target) return n >= 0 ? 1 : (r == EOF ? EOF : 0);
    return r;
}

void FileScanSource::scan_literal(const std::string& lit) {
    const long before = std::ftell(fp_);
    std::fscanf(fp_, lit.c_str());
    consumed_ += std::size_t(std::ftell(fp_) - before);
}

}
