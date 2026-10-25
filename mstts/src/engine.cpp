#include "engine.hpp"

#include <cstdio>
#include <mutex>
#include <stdexcept>

#include "ssml.hpp"
#include "speechapi_cxx.h"

namespace mstts {
namespace {

using namespace Microsoft::CognitiveServices::Speech;
using namespace Microsoft::CognitiveServices::Speech::Audio;

// 微软 SDK 内部有全局状态，串行化最稳妥（实测单次合成约 40x 实时，串行完全够用）。
std::mutex& sdkMutex() {
    static std::mutex m;
    return m;
}

bool fileExists(const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    std::fclose(f);
    return true;
}

std::string describeFailure(const std::shared_ptr<SpeechSynthesisResult>& result) {
    auto details = SpeechSynthesisCancellationDetails::FromResult(result);
    std::string msg = "语音合成被取消（reason=" + std::to_string((int)details->Reason) + "）";
    if (!details->ErrorDetails.empty()) msg += "：" + details->ErrorDetails;
    return msg;
}

}  // namespace

Engine::Engine(std::string voiceRoot) : root_(std::move(voiceRoot)) {
    while (root_.size() > 1 && root_.back() == '/') root_.pop_back();
}

bool Engine::voiceInstalled(const Voice& v) const {
    // 只认关键模型文件：目录建了但没解包完整时也能挡住。
    // 注意 vocoder 有 v5 / v6 两种（如 Aria 是 v5），不能写死。
    const std::string dir = root_ + "/" + v.code;
    return fileExists(dir + "/Tokens.xml") && fileExists(dir + "/am_v5_encoder.bin") &&
           (fileExists(dir + "/device_vocoder_v6_streaming.bin") ||
            fileExists(dir + "/device_vocoder_v5_streaming.bin"));
}

std::vector<const Voice*> Engine::installedVoices() const {
    std::vector<const Voice*> out;
    for (const auto& v : allVoices())
        if (voiceInstalled(v)) out.push_back(&v);
    return out;
}

std::vector<uint8_t> Engine::synthesize(const Voice& v, const std::string& text,
                                        const SynthOptions& opt, const PcmSink& onPcm) const {
    auto chunks = chunkText(text, opt.maxChars);
    if (chunks.empty()) throw std::runtime_error("文本为空");
    std::vector<uint8_t> pcm;
    for (const auto& c : chunks) {
        auto part = synthesizeSsml(v, buildSsml(v, c, opt.rate, opt.pitch), onPcm);
        pcm.insert(pcm.end(), part.begin(), part.end());
    }
    if (pcm.empty())
        throw std::runtime_error("没有合成出音频：语音 " + v.code + "（" + v.locale +
                                 "）可能读不了这段文本（文本语言与语音语言不匹配时会发生，"
                                 "例如用英文语音读中文）");
    return pcm;
}

std::vector<uint8_t> Engine::synthesizeSsml(const Voice& v, const std::string& ssml,
                                            const PcmSink& onPcm) const {
    std::lock_guard<std::mutex> lock(sdkMutex());
    if (!voiceInstalled(v))
        throw std::runtime_error("语音包缺失：" + root_ + "/" + v.code +
                                 "（用 scripts/install-voices.sh 或 mstts doctor 检查）");
    try {
        auto cfg = EmbeddedSpeechConfig::FromPath(root_ + "/" + v.code);
        cfg->SetSpeechSynthesisOutputFormat(SpeechSynthesisOutputFormat::Raw24Khz16BitMonoPcm);
        cfg->SetSpeechSynthesisVoice(v.displayName, licenseFor(v));

        auto synth = SpeechSynthesizer::FromConfig(cfg, nullptr);
        auto result = synth->SpeakSsml(ssml);
        if (result->Reason != ResultReason::SynthesizingAudioCompleted)
            throw std::runtime_error(describeFailure(result));

        std::vector<uint8_t> pcm;
        auto stream = AudioDataStream::FromResult(result);
        std::vector<uint8_t> buf(4800);
        uint32_t n = 0;
        while ((n = stream->ReadData(buf.data(), (uint32_t)buf.size())) > 0) {
            pcm.insert(pcm.end(), buf.begin(), buf.begin() + n);
            if (onPcm) onPcm(buf.data(), n);
        }
        return pcm;
    } catch (const std::exception& e) {
        throw std::runtime_error(std::string("合成失败：") + e.what());
    }
}

double pcmSeconds(size_t bytes) { return bytes / (double)(Engine::kSampleRate * 2); }

}  // namespace mstts
