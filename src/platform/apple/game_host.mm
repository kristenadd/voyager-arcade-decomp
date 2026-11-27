#include "platform/apple/game_host.hpp"

#include <pthread.h>

#include "app/session.hpp"
#include "host/fail.hpp"

namespace voy::platform {

GameHost::GameHost(host::Config config) {
    machine_ = std::make_unique<host::Machine>(std::move(config), app::built_programs());
    machine_->gl().set_sink(this);
}

GameHost::~GameHost() { stop(); }

void GameHost::start() {
    if (started_) return;
    started_ = true;
    running_ = true;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 64u << 20);  // translated code recurses as deep as the game
    pthread_create(&thread_, &attr, [](void* self) -> void* {
        pthread_setname_np("game");
        auto* h = static_cast<GameHost*>(self);
        h->exit_code_ = h->machine_->run();
        h->running_ = false;
        return nullptr;
    }, this);
    pthread_attr_destroy(&attr);
}

void GameHost::stop() {
    if (!started_) return;
    machine_->request_quit();
    pthread_join(thread_, nullptr);
    started_ = false;
}

void GameHost::submit(std::shared_ptr<const gl::Frame> frame) {
    std::lock_guard<std::mutex> lock(m_);
    latest_ = std::move(frame);
}

std::shared_ptr<const gl::Frame> GameHost::take_new() {
    std::lock_guard<std::mutex> lock(m_);
    if (!latest_ || latest_->index == taken_) return nullptr;
    taken_ = latest_->index;
    return latest_;
}

void GameHost::pointer_shot(int gun, float gx, float gy, bool inside) {
    if (inside) machine_->input().shot(gun, int(gx), int(gy));
    else machine_->input().shot(gun, -1, -1);
}

bool to_game(const float rect[4], float px, float py, float* gx, float* gy) {
    const float w = rect[2] - rect[0], h = rect[3] - rect[1];
    if (w <= 0 || h <= 0) return false;
    *gx = (px - rect[0]) / w * float(gl::kWidth);
    *gy = (py - rect[1]) / h * float(gl::kHeight);
    return *gx >= 0 && *gy >= 0 && *gx < float(gl::kWidth) && *gy < float(gl::kHeight);
}

}
