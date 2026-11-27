#pragma once
// Plays the sound program's /dev/dsp output through an AudioQueue.
#include <memory>

namespace voy::host {
class Machine;
}

namespace voy::platform {

class AudioOut {
public:
    explicit AudioOut(host::Machine& m);
    ~AudioOut();
    bool start();
    void stop();
    void set_volume(float v);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}
