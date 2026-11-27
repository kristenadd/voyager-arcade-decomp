// The devices the programs open: the MegaJamma I/O board (/dev/mjf), the OSS
// sound card (/dev/dsp) and the pipe from trek to stvsnd.
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <vector>

#include "host/fail.hpp"
#include "host/machine.hpp"
#include "host/os.hpp"
#include "host/process.hpp"

namespace voy::host {

using namespace recomp;

namespace {

// ---- MegaJamma ----
// read() returns one 0xA8-byte packet per mj_update call (then 0 bytes, which
// ends mj_update's loop). Packet words, as mj_do_packet reads them:
//   0  shot bits (shotmask[gun])        1  switches (start 0x80000 / 0x40000, test 0x800000)
//   3  irq count (strictly increasing)  4 + gun  the shot in game coordinates (natives.cpp)
//   12, 13  coin counters               14  board version
class MegaJamma : public Device {
public:
    explicit MegaJamma(Machine& m) : m_(m) {}

    int read(Process& p, std::uint32_t buf, std::uint32_t n) override {
        if (n < 0xA8) return 0;
        if (sent_) {
            sent_ = false;
            return 0;
        }
        sent_ = true;
        auto snap = m_.input().take();
        for (auto& s : snap.shots) pending_.push_back(s);
        std::uint32_t w[42] = {};
        const std::uint32_t shotmask = p.sym("shotmask");
        bool used[2] = {false, false};
        for (auto it = pending_.begin(); it != pending_.end();) {
            const int g = it->gun & 1;
            if (used[g]) {
                ++it;
                continue;
            }
            used[g] = true;
            w[0] |= shotmask ? p.u32(shotmask + 4 * g) : (1u << g);
            w[4 + g] = std::uint32_t(std::uint16_t(std::int16_t(it->x))) | std::uint32_t(std::uint16_t(std::int16_t(it->y))) << 16;
            it = pending_.erase(it);
        }
        std::uint32_t sw = 0;
        if (snap.buttons[Input::Start1]) sw |= 0x80000;
        if (snap.buttons[Input::Start2]) sw |= 0x40000;
        if (snap.buttons[Input::Test]) sw |= 0x800000;
        w[1] = sw;
        w[3] = ++irq_;
        w[12] = snap.coins[0];
        w[13] = snap.coins[1];
        w[14] = 0x0105;
        std::memcpy(p.host(buf), w, 0xA8);
        return 0xA8;
    }

    int ioctl(Process& p, std::uint32_t request, std::uint32_t arg) override {
        if (request == 0x40044D07 && arg) p.set_u32(arg, 0);  // error status: none
        return 0;
    }

private:
    Machine& m_;
    bool sent_ = false;
    std::uint32_t irq_ = 0;
    std::deque<GunShot> pending_;
};

// ---- the pipe from trek's popen to stvsnd's stdin ----
class Pipe : public Device {
public:
    int write(Process& p, std::uint32_t buf, std::uint32_t n) override {
        std::lock_guard<std::mutex> lock(m_);
        const auto* b = static_cast<const std::uint8_t*>(p.host(buf));
        data_.insert(data_.end(), b, b + n);
        cv_.notify_all();
        return int(n);
    }
    int read(Process& p, std::uint32_t buf, std::uint32_t n) override {
        std::unique_lock<std::mutex> lock(m_);
        cv_.wait(lock, [&] { return !data_.empty() || closed_ || p.stop_requested(); });
        if (data_.empty()) throw GuestStop{-3, "sound pipe closed"};
        const std::size_t k = std::min<std::size_t>(n, data_.size());
        auto* b = static_cast<std::uint8_t*>(p.host(buf));
        for (std::size_t i = 0; i < k; ++i) b[i] = data_[i];
        data_.erase(data_.begin(), data_.begin() + std::ptrdiff_t(k));
        return int(k);
    }
    bool wait_readable(long timeout_us) override {
        std::unique_lock<std::mutex> lock(m_);
        auto ready = [&] { return !data_.empty() || closed_; };
        if (timeout_us < 0) cv_.wait(lock, ready);
        else cv_.wait_for(lock, std::chrono::microseconds(timeout_us), ready);
        if (closed_ && data_.empty()) throw GuestStop{-3, "sound pipe closed"};
        return ready();
    }
    void close() override {
        std::lock_guard<std::mutex> lock(m_);
        closed_ = true;
        cv_.notify_all();
    }

private:
    std::mutex m_;
    std::condition_variable cv_;
    std::deque<std::uint8_t> data_;
    bool closed_ = false;
};

// ---- OSS /dev/dsp ----
// Linux OSS ioctls stvsnd uses.
enum : std::uint32_t {
    SNDCTL_DSP_RESET = 0x5000, SNDCTL_DSP_SYNC = 0x5001, SNDCTL_DSP_SPEED = 0xC0045002, SNDCTL_DSP_STEREO = 0xC0045003,
    SNDCTL_DSP_GETBLKSIZE = 0xC0045004, SNDCTL_DSP_SETFMT = 0xC0045005, SNDCTL_DSP_CHANNELS = 0xC0045006,
    SNDCTL_DSP_SETFRAGMENT = 0xC004500A, SNDCTL_DSP_GETFMTS = 0x8004500B, SNDCTL_DSP_GETOSPACE = 0x8010500C,
    SNDCTL_DSP_NONBLOCK = 0x0000500E, SNDCTL_DSP_GETCAPS = 0x8004500F, SNDCTL_DSP_GETODELAY = 0x80045017,
    AFMT_S16_LE = 0x10, AFMT_U8 = 0x08,
};

class Dsp : public Device {
public:
    explicit Dsp(Machine& m) : m_(m) {}

    int ioctl(Process& p, std::uint32_t request, std::uint32_t arg) override {
        std::lock_guard<std::mutex> lock(mu_);
        auto get = [&]() { return arg ? p.u32(arg) : 0u; };
        auto put = [&](std::uint32_t v) { if (arg) p.set_u32(arg, v); };
        switch (request) {
        case SNDCTL_DSP_RESET: case SNDCTL_DSP_SYNC: case SNDCTL_DSP_NONBLOCK: return 0;
        case SNDCTL_DSP_SPEED: rate_ = int(get()); if (rate_ <= 0) rate_ = 22050; put(std::uint32_t(rate_)); return 0;
        case SNDCTL_DSP_STEREO: channels_ = get() ? 2 : 1; put(channels_ == 2); return 0;
        case SNDCTL_DSP_CHANNELS: channels_ = get() >= 2 ? 2 : 1; put(std::uint32_t(channels_)); return 0;
        case SNDCTL_DSP_SETFMT: {
            const std::uint32_t f = get();
            if (f == AFMT_U8) bits_ = 8;
            else bits_ = 16;
            put(bits_ == 8 ? AFMT_U8 : AFMT_S16_LE);
            return 0;
        }
        case SNDCTL_DSP_GETFMTS: put(AFMT_S16_LE | AFMT_U8); return 0;
        case SNDCTL_DSP_SETFRAGMENT: {
            const std::uint32_t v = get();
            frag_size_ = 1 << std::min<std::uint32_t>(v & 0xFFFF, 16);
            if (frag_size_ < 128) frag_size_ = 128;
            frag_count_ = int(std::min<std::uint32_t>(std::max<std::uint32_t>(v >> 16, 2), 64));
            return 0;
        }
        case SNDCTL_DSP_GETBLKSIZE: put(std::uint32_t(frag_size_)); return 0;
        case SNDCTL_DSP_GETCAPS: put(0x00000100); return 0;  // DSP_CAP_TRIGGER-ish: nothing special
        case SNDCTL_DSP_GETOSPACE: {
            drain_by_clock();
            const int total = frag_size_ * frag_count_;
            const int free_bytes = std::max(0, total - int(ring_.size()));
            if (arg) {
                p.set_u32(arg, std::uint32_t(free_bytes / frag_size_));
                p.set_u32(arg + 4, std::uint32_t(frag_count_));
                p.set_u32(arg + 8, std::uint32_t(frag_size_));
                p.set_u32(arg + 12, std::uint32_t(free_bytes));
            }
            return 0;
        }
        case SNDCTL_DSP_GETODELAY: drain_by_clock(); put(std::uint32_t(ring_.size())); return 0;
        default: {
            char buf[48];
            std::snprintf(buf, sizeof buf, "/dev/dsp ioctl 0x%08X", request);
            note_once("dsp", buf);
            return 0;
        }
        }
    }

    int write(Process& p, std::uint32_t buf, std::uint32_t n) override {
        std::unique_lock<std::mutex> lock(mu_);
        const auto* b = static_cast<const std::uint8_t*>(p.host(buf));
        const std::size_t cap = std::size_t(frag_size_) * frag_count_;
        std::size_t done = 0;
        while (done < n) {
            drain_by_clock();
            if (ring_.size() >= cap) {
                if (p.stop_requested() || m_.quitting()) throw GuestStop{-3, "sound stopped"};
                cv_.wait_for(lock, std::chrono::milliseconds(2));
                continue;
            }
            const std::size_t k = std::min<std::size_t>(n - done, cap - ring_.size());
            ring_.insert(ring_.end(), b + done, b + done + k);
            done += k;
        }
        return int(n);
    }

    void close() override {
        std::lock_guard<std::mutex> lock(mu_);
        closed_ = true;
        cv_.notify_all();
    }

    Machine::AudioFormat format() {
        std::lock_guard<std::mutex> lock(mu_);
        return {rate_, channels_, bits_};
    }

    // The platform's audio callback: interleaved stereo int16 at `out_rate`,
    // resampled linearly from the rate the sound program chose.
    void pull(std::int16_t* out, int frames, int out_rate) {
        std::lock_guard<std::mutex> lock(mu_);
        pulled_ = true;
        const int in_frame = channels_ * (bits_ / 8);
        const double step = double(rate_) / double(out_rate > 0 ? out_rate : rate_);
        auto sample = [&](std::size_t o) -> std::int16_t {
            if (bits_ == 8) return std::int16_t((int(ring_[o]) - 128) << 8);
            return std::int16_t(ring_[o] | ring_[o + 1] << 8);
        };
        for (int f = 0; f < frames; ++f) {
            // Advance whole source frames as the phase passes them.
            while (phase_ >= 1.0 && ring_.size() >= std::size_t(in_frame)) {
                prev_[0] = cur_[0];
                prev_[1] = cur_[1];
                cur_[0] = sample(0);
                cur_[1] = channels_ == 2 ? sample(std::size_t(bits_ / 8)) : cur_[0];
                ring_.erase(ring_.begin(), ring_.begin() + in_frame);
                phase_ -= 1.0;
            }
            if (phase_ >= 1.0) phase_ = 1.0;  // underrun: hold the last sample
            const double t = phase_;
            out[2 * f] = std::int16_t(prev_[0] + (cur_[0] - prev_[0]) * t);
            out[2 * f + 1] = std::int16_t(prev_[1] + (cur_[1] - prev_[1]) * t);
            phase_ += step;
        }
        cv_.notify_all();
    }

private:
    // Without a platform consumer (headless), audio is consumed at its own
    // rate so the sound program paces itself as it did on the cabinet.
    void drain_by_clock() {
        const auto now = std::chrono::steady_clock::now();
        if (pulled_) {
            last_ = now;
            return;
        }
        if (last_.time_since_epoch().count() == 0) last_ = now;
        const double secs = std::chrono::duration<double>(now - last_).count();
        const std::size_t bytes = std::size_t(secs * rate_ * channels_ * (bits_ / 8));
        if (bytes >= 64) {
            ring_.erase(ring_.begin(), ring_.begin() + std::ptrdiff_t(std::min(bytes, ring_.size())));
            last_ = now;
        }
    }

    Machine& m_;
    std::mutex mu_;
    std::condition_variable cv_;
    std::deque<std::uint8_t> ring_;
    int rate_ = 22050, channels_ = 2, bits_ = 16;
    int frag_size_ = 2048, frag_count_ = 8;
    bool closed_ = false, pulled_ = false;
    double phase_ = 1.0;
    int prev_[2] = {0, 0}, cur_[2] = {0, 0};
    std::chrono::steady_clock::time_point last_{};
};

}  // namespace

std::shared_ptr<Device> make_megajamma(Machine& m) { return std::make_shared<MegaJamma>(m); }
std::shared_ptr<Device> make_dsp(Machine& m) { return std::make_shared<Dsp>(m); }
std::shared_ptr<Device> make_pipe() { return std::make_shared<Pipe>(); }
Machine::AudioFormat dsp_format(Device& dsp) { return static_cast<Dsp&>(dsp).format(); }
void dsp_pull(Device& dsp, std::int16_t* out, int frames, int out_rate) { static_cast<Dsp&>(dsp).pull(out, frames, out_rate); }

}
