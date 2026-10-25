// 逐格式探测离线（Embedded）TTS 引擎的真实输出。
//
//   make probe-formats
//   ./build/probe_formats [语音包根目录] [语音 code]
//
// 结论（2026-10，SDK 1.53 + 本工程语音包）：只有 24kHz/16bit/单声道 PCM 可用。
// 44.1k / 48k 会返回 EMBEDDED_TTS_ERROR_AUDIO_FORMAT_NOT_SUPPORTED：
//   Request sample rate (48000) is not supported, supported sample rate is 24000.
// 传 SPEECH-SynthOfflineIgnoreFormatCheckAndResampling=true 只会绕过检查，
// 拿到的仍是同样的 24k 字节流（不会真的重采样）。
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "speechapi_cxx.h"
#include "voices.hpp"

using namespace Microsoft::CognitiveServices::Speech;
using namespace Microsoft::CognitiveServices::Speech::Audio;

namespace {

struct FormatName {
    int id;
    const char* name;
};

// 顺序与 speechapi_cxx_enums.h 的 SpeechSynthesisOutputFormat 一致
const FormatName kFormats[] = {
    {1, "Raw8Khz8BitMonoMULaw"},
    {2, "Riff16Khz16KbpsMonoSiren"},
    {3, "Audio16Khz16KbpsMonoSiren"},
    {4, "Audio16Khz32KBitRateMonoMp3"},
    {5, "Audio16Khz128KBitRateMonoMp3"},
    {6, "Audio16Khz64KBitRateMonoMp3"},
    {7, "Audio24Khz48KBitRateMonoMp3"},
    {8, "Audio24Khz96KBitRateMonoMp3"},
    {9, "Audio24Khz160KBitRateMonoMp3"},
    {10, "Raw16Khz16BitMonoTrueSilk"},
    {11, "Riff16Khz16BitMonoPcm"},
    {12, "Riff8Khz16BitMonoPcm"},
    {13, "Riff24Khz16BitMonoPcm"},
    {14, "Riff8Khz8BitMonoMULaw"},
    {15, "Raw16Khz16BitMonoPcm"},
    {16, "Raw24Khz16BitMonoPcm"},
    {17, "Raw8Khz16BitMonoPcm"},
    {18, "Ogg16Khz16BitMonoOpus"},
    {19, "Ogg24Khz16BitMonoOpus"},
    {20, "Raw48Khz16BitMonoPcm"},
    {21, "Riff48Khz16BitMonoPcm"},
    {22, "Audio48Khz96KBitRateMonoMp3"},
    {23, "Audio48Khz192KBitRateMonoMp3"},
    {24, "Ogg48Khz16BitMonoOpus"},
    {25, "Webm16Khz16BitMonoOpus"},
    {26, "Webm24Khz16BitMonoOpus"},
    {27, "Raw24Khz16BitMonoTrueSilk"},
    {28, "Raw8Khz8BitMonoALaw"},
    {29, "Riff8Khz8BitMonoALaw"},
    {30, "Webm24Khz16Bit24KbpsMonoOpus"},
    {31, "Audio16Khz16Bit32KbpsMonoOpus"},
    {32, "Audio24Khz16Bit48KbpsMonoOpus"},
    {33, "Audio24Khz16Bit24KbpsMonoOpus"},
    {34, "Raw22050Hz16BitMonoPcm"},
    {35, "Riff22050Hz16BitMonoPcm"},
    {36, "Raw44100Hz16BitMonoPcm"},
    {37, "Riff44100Hz16BitMonoPcm"},
    {38, "AmrWb16000Hz"},
    {39, "G72216Khz64Kbps"},
};

uint32_t le32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
uint16_t le16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }

std::string failure(const std::shared_ptr<SpeechSynthesisResult>& r) {
    auto d = SpeechSynthesisCancellationDetails::FromResult(r);
    return d->ErrorDetails.empty() ? "reason=" + std::to_string((int)d->Reason) : d->ErrorDetails;
}

std::string defaultRoot() {
    const char* env = std::getenv("MSTTS_VOICES");
    if (env && *env) return env;
    return std::string(std::getenv("HOME") ? std::getenv("HOME") : ".") +
           "/Library/Application Support/mstts/voices";
}

}  // namespace

int main(int argc, char** argv) {
    const std::string root = argc > 1 ? argv[1] : defaultRoot();
    const std::string code = argc > 2 ? argv[2] : "zh-CN-XiaoxiaoNeural";
    const mstts::Voice* voice = mstts::findVoice(code);
    if (!voice) {
        std::fprintf(stderr, "未知语音: %s（mstts list 查看）\n", code.c_str());
        return 2;
    }

    const std::string text = voice->locale.rfind("zh", 0) == 0 ? "春眠不觉晓，处处闻啼鸟。"
                                                               : "The quick brown fox jumps over the lazy dog.";
    const std::string ssml = "<speak version='1.0' xmlns='http://www.w3.org/2001/10/synthesis' xml:lang='" +
                             voice->locale + "'><voice name='" + voice->displayName + "'>" + text +
                             "</voice></speak>";

    std::printf("语音: %s (%s)\n目录: %s\n\n", voice->code.c_str(), voice->displayName.c_str(),
                root.c_str());
    std::printf("%-3s %-34s %-8s %s\n", "id", "format", "结果", "数据");

    std::vector<int> okFormats;
    for (const auto& f : kFormats) {
        auto cfg = EmbeddedSpeechConfig::FromPath(root + "/" + voice->code);
        cfg->SetSpeechSynthesisOutputFormat((SpeechSynthesisOutputFormat)f.id);
        cfg->SetSpeechSynthesisVoice(voice->displayName, mstts::licenseFor(*voice));
        auto synth = SpeechSynthesizer::FromConfig(cfg, nullptr);
        auto result = synth->SpeakSsml(ssml);

        if (result->Reason != ResultReason::SynthesizingAudioCompleted) {
            std::printf("%-3d %-34s %-8s %s\n", f.id, f.name, "拒绝", failure(result).c_str());
            continue;
        }

        std::vector<uint8_t> data;
        auto stream = AudioDataStream::FromResult(result);
        std::vector<uint8_t> buf(8192);
        uint32_t n = 0;
        while ((n = stream->ReadData(buf.data(), (uint32_t)buf.size())) > 0)
            data.insert(data.end(), buf.begin(), buf.begin() + n);

        std::string info = std::to_string(data.size()) + " B";
        if (data.size() >= 44 && !std::memcmp(data.data(), "RIFF", 4)) {
            info += "  RIFF " + std::to_string(le32(data.data() + 24)) + "Hz/" +
                    std::to_string(le16(data.data() + 22)) + "ch/" +
                    std::to_string(le16(data.data() + 34)) + "bit";
        } else if (data.size() >= 44 && !std::memcmp(data.data(), "OggS", 4)) {
            info += "  OGG";
        } else if (data.size() >= 4 && !std::memcmp(data.data(), "\x1a\x45\xdf\xa3", 4)) {
            info += "  WEBM";
        } else {
            // 裸 PCM 没有头，按 24k/16bit/mono 折算时长
            info += "  裸 PCM ≈ " + std::to_string(data.size() / 48000.0).substr(0, 4) + "s @24k";
        }
        std::printf("%-3d %-34s %-8s %s\n", f.id, f.name, "OK", info.c_str());
        okFormats.push_back(f.id);
    }

    std::printf("\n可用的格式 id:");
    for (int id : okFormats) std::printf(" %d", id);
    std::printf("\n");
    if (okFormats == std::vector<int>{13, 16})
        std::printf("结论: 引擎只出 24kHz/16bit/单声道 PCM（13=带 RIFF 头，16=裸 PCM）。\n");
    return 0;
}
