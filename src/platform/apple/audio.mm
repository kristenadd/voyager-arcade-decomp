#include "platform/apple/audio.hpp"

#import <AudioToolbox/AudioToolbox.h>

#include "host/machine.hpp"

namespace voy::platform {

namespace {
constexpr int kRate = 44100, kFrames = 1024, kBuffers = 3;
}

struct AudioOut::Impl {
    host::Machine& m;
    AudioQueueRef queue = nullptr;
    AudioQueueBufferRef buffers[kBuffers] = {};
    explicit Impl(host::Machine& machine) : m(machine) {}

    static void callback(void* user, AudioQueueRef q, AudioQueueBufferRef b) {
        auto* self = static_cast<Impl*>(user);
        self->m.pull_audio(static_cast<std::int16_t*>(b->mAudioData), kFrames, kRate);
        b->mAudioDataByteSize = kFrames * 4;
        AudioQueueEnqueueBuffer(q, b, 0, nullptr);
    }
};

AudioOut::AudioOut(host::Machine& m) : impl_(new Impl(m)) {}
AudioOut::~AudioOut() { stop(); }

bool AudioOut::start() {
    AudioStreamBasicDescription f = {};
    f.mSampleRate = kRate;
    f.mFormatID = kAudioFormatLinearPCM;
    f.mFormatFlags = kLinearPCMFormatFlagIsSignedInteger | kLinearPCMFormatFlagIsPacked;
    f.mBytesPerPacket = 4;
    f.mFramesPerPacket = 1;
    f.mBytesPerFrame = 4;
    f.mChannelsPerFrame = 2;
    f.mBitsPerChannel = 16;
    if (AudioQueueNewOutput(&f, Impl::callback, impl_.get(), nullptr, nullptr, 0, &impl_->queue) != noErr) return false;
    for (auto& b : impl_->buffers) {
        AudioQueueAllocateBuffer(impl_->queue, kFrames * 4, &b);
        std::memset(b->mAudioData, 0, kFrames * 4);
        b->mAudioDataByteSize = kFrames * 4;
        AudioQueueEnqueueBuffer(impl_->queue, b, 0, nullptr);
    }
    return AudioQueueStart(impl_->queue, nullptr) == noErr;
}

void AudioOut::stop() {
    if (impl_ && impl_->queue) {
        AudioQueueStop(impl_->queue, true);
        AudioQueueDispose(impl_->queue, true);
        impl_->queue = nullptr;
    }
}

void AudioOut::set_volume(float v) {
    if (impl_->queue) AudioQueueSetParameter(impl_->queue, kAudioQueueParam_Volume, v);
}

}
