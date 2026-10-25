#include "player.hpp"

#include <AudioToolbox/AudioToolbox.h>
#include <CoreFoundation/CoreFoundation.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace mstts {
namespace {

struct PlayerState {
    const uint8_t* data = nullptr;
    size_t total = 0;
    size_t offset = 0;
    AudioQueueRef queue = nullptr;
    int active = 0;   // 已入队且还没播完的 buffer 数
    bool finished = false;
    bool underrun = false;
};

void fillBuffer(PlayerState* st, AudioQueueBufferRef buf) {
    size_t remain = st->total > st->offset ? st->total - st->offset : 0;
    size_t n = std::min<size_t>(remain, buf->mAudioDataBytesCapacity);
    if (n) std::memcpy(buf->mAudioData, st->data + st->offset, n);
    st->offset += n;
    buf->mAudioDataByteSize = (UInt32)n;
    if (n == 0) st->underrun = true;
}

void outputCallback(void* user, AudioQueueRef queue, AudioQueueBufferRef buffer) {
    auto* st = static_cast<PlayerState*>(user);
    if (st->offset >= st->total) {
        // 数据放完：这一轮不再入队，等剩下的 buffer 播完
        buffer->mAudioDataByteSize = 0;
        if (--st->active <= 0) {
            st->finished = true;
            AudioQueueStop(queue, false);
            CFRunLoopStop(CFRunLoopGetCurrent());
        }
        return;
    }
    fillBuffer(st, buffer);
    AudioQueueEnqueueBuffer(queue, buffer, 0, nullptr);
}

}  // namespace

bool playPcm(const uint8_t* data, size_t bytes, int sampleRate) {
    if (!data || bytes < 2) return false;

    AudioStreamBasicDescription fmt{};
    fmt.mSampleRate = sampleRate;
    fmt.mFormatID = kAudioFormatLinearPCM;
    fmt.mFormatFlags = kLinearPCMFormatFlagIsSignedInteger | kLinearPCMFormatFlagIsPacked;
    fmt.mBytesPerPacket = 2;
    fmt.mFramesPerPacket = 1;
    fmt.mBytesPerFrame = 2;
    fmt.mChannelsPerFrame = 1;
    fmt.mBitsPerChannel = 16;

    PlayerState st;
    st.data = data;
    st.total = bytes;

    AudioQueueRef queue = nullptr;
    if (AudioQueueNewOutput(&fmt, &outputCallback, &st, nullptr, nullptr, 0, &queue) != noErr) {
        std::fprintf(stderr, "AudioQueueNewOutput 失败\n");
        return false;
    }
    st.queue = queue;

    const UInt32 bufBytes = (UInt32)std::min<size_t>(bytes, (size_t)sampleRate * 2 / 5);  // 200ms
    constexpr int kBuffers = 3;
    for (int i = 0; i < kBuffers; ++i) {
        AudioQueueBufferRef buf = nullptr;
        if (AudioQueueAllocateBuffer(queue, bufBytes, &buf) != noErr) {
            AudioQueueDispose(queue, true);
            return false;
        }
        fillBuffer(&st, buf);
        if (buf->mAudioDataByteSize == 0) {
            AudioQueueFreeBuffer(queue, buf);
            break;
        }
        st.active++;
        AudioQueueEnqueueBuffer(queue, buf, 0, nullptr);
    }

    if (AudioQueueStart(queue, nullptr) != noErr) {
        std::fprintf(stderr, "AudioQueueStart 失败\n");
        AudioQueueDispose(queue, true);
        return false;
    }

    while (!st.finished) {
        // 跑一下 run loop 让回调有机会执行（回调也可能在内部线程，二者都行）
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.05, true);
    }

    AudioQueueStop(queue, true);
    AudioQueueDispose(queue, true);
    return true;
}

}  // namespace mstts
