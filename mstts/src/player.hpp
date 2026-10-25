#pragma once

#include <cstdint>
#include <cstddef>

namespace mstts {

/// 通过 CoreAudio 的 AudioQueue 直接播放 16bit 单声道 PCM（阻塞到播完）。
/// 采样率必须显式传入（AudioQueue 不会替我们猜）。不落临时文件。返回 false 表示播放被中止。
bool playPcm(const uint8_t* data, size_t bytes, int sampleRate);

}  // namespace mstts
