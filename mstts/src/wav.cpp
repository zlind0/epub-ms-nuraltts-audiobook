#include "wav.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <stdexcept>

namespace mstts {
namespace {
void put16(std::string& s, uint16_t v) {
    s.push_back((char)(v & 0xFF));
    s.push_back((char)((v >> 8) & 0xFF));
}
void put32(std::string& s, uint32_t v) {
    s.push_back((char)(v & 0xFF));
    s.push_back((char)((v >> 8) & 0xFF));
    s.push_back((char)((v >> 16) & 0xFF));
    s.push_back((char)((v >> 24) & 0xFF));
}
}  // namespace

std::string encodeWav(const uint8_t* pcm, size_t bytes, int sampleRate, int channels,
                      int bitsPerSample) {
    const uint32_t dataSize = (uint32_t)bytes;
    const uint16_t blockAlign = (uint16_t)(channels * bitsPerSample / 8);
    std::string out;
    out.reserve(44 + bytes);
    out += "RIFF";
    put32(out, 36 + dataSize);
    out += "WAVE";
    out += "fmt ";
    put32(out, 16);
    put16(out, 1);  // PCM
    put16(out, (uint16_t)channels);
    put32(out, (uint32_t)sampleRate);
    put32(out, (uint32_t)(sampleRate * blockAlign));
    put16(out, blockAlign);
    put16(out, (uint16_t)bitsPerSample);
    out += "data";
    put32(out, dataSize);
    out.append((const char*)pcm, bytes);
    return out;
}

void writeAll(const std::string& path, const void* data, size_t bytes) {
    if (path == "-") {
        if (bytes && std::fwrite(data, 1, bytes, stdout) != bytes)
            throw std::runtime_error("写 stdout 失败");
        std::fflush(stdout);
        return;
    }
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) throw std::runtime_error("无法写入文件: " + path + " (" + std::strerror(errno) + ")");
    size_t n = bytes ? std::fwrite(data, 1, bytes, f) : 0;
    std::fclose(f);
    if (n != bytes) throw std::runtime_error("写入不完整: " + path);
}

std::string readAll(const std::string& path) {
    std::string out;
    if (path == "-") {
        char buf[8192];
        size_t n;
        while ((n = std::fread(buf, 1, sizeof(buf), stdin)) > 0) out.append(buf, n);
        return out;
    }
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) throw std::runtime_error("无法读取文件: " + path + " (" + std::strerror(errno) + ")");
    char buf[65536];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
    std::fclose(f);
    return out;
}

}  // namespace mstts
