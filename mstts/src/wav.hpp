#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace mstts {

/// 裸 PCM → WAV (RIFF) 字节流。采样率必须由调用方给出（不要在这里假设 24k）。
std::string encodeWav(const uint8_t* pcm, size_t bytes, int sampleRate, int channels = 1,
                      int bitsPerSample = 16);

inline std::string encodeWav(const std::vector<uint8_t>& pcm, int sampleRate) {
    return encodeWav(pcm.data(), pcm.size(), sampleRate);
}

/// 写文件；path 为 "-" 时写 stdout。失败抛 std::runtime_error。
void writeAll(const std::string& path, const void* data, size_t bytes);

/// 读整个文件 / 读 stdin（path 为 "-"）。
std::string readAll(const std::string& path);

}  // namespace mstts
