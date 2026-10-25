#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "voices.hpp"

namespace mstts {

struct SynthOptions {
    float rate = 1.0f;       ///< 语速倍数
    float pitch = 1.0f;      ///< 音调倍数
    size_t maxChars = 450;   ///< 单块最大字符数（长文本按句切分）
};

/// 微软离线（Embedded）神经 TTS 的 C++ 封装。
/// 每次合成都会新建 EmbeddedSpeechConfig / SpeechSynthesizer（实测开销 ~100ms，
/// 其中首次包含模型加载；不缓存模型，避免上 GB 常驻内存）。
class Engine {
public:
    using PcmSink = std::function<void(const uint8_t*, size_t)>;

    explicit Engine(std::string voiceRoot);

    const std::string& voiceRoot() const { return root_; }
    bool voiceInstalled(const Voice& v) const;
    std::vector<const Voice*> installedVoices() const;

    /// 文本 → PCM（格式见 Engine::kSampleRate 等常量；按句切分、多块拼接）。
    /// onPcm 非空时每读到一块（≈100ms）回调一次，可边合成边播放/推流。
    std::vector<uint8_t> synthesize(const Voice& v, const std::string& text,
                                    const SynthOptions& opt = {}, const PcmSink& onPcm = {}) const;

    /// 原样喂入 SSML（调用方负责 voice 名与 xml:lang 正确）。
    std::vector<uint8_t> synthesizeSsml(const Voice& v, const std::string& ssml,
                                        const PcmSink& onPcm = {}) const;

    // 引擎的物理输出格式，全工程只在这里写死一次。
    // 离线（Embedded）引擎只接受 24kHz PCM：请求 44.1k/48k 会被
    // EMBEDDED_TTS_ERROR_AUDIO_FORMAT_NOT_SUPPORTED 直接拒绝
    // （逐格式实测：make probe-formats）。语音包本身也是按 24k 训练的，
    // 上采样到 48k 不会多出任何信息，所以不去伪造高采样率。
    static constexpr int kSampleRate = 24000;
    static constexpr int kChannels = 1;
    static constexpr int kBitsPerSample = 16;

private:
    std::string root_;
};

/// 合成结果时长（秒）。
double pcmSeconds(size_t bytes);

}  // namespace mstts
