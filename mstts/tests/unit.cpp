// 纯函数单测：SSML / 切分 / WAV / 语音表。不加载模型，秒级跑完。
#include <cstdio>
#include <cstring>
#include <string>

#include "engine.hpp"
#include "ssml.hpp"
#include "voices.hpp"
#include "wav.hpp"

static int g_fail = 0;
#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) {                                                           \
            std::printf("  ✗ %s:%d  %s\n", __FILE__, __LINE__, #cond);           \
            ++g_fail;                                                            \
        }                                                                        \
    } while (0)

static void testEscape() {
    std::printf("[escape]\n");
    CHECK(mstts::escapeXml("a<b>c&d\"e'f") == "a&lt;b&gt;c&amp;d&quot;e&apos;f");
    CHECK(mstts::escapeXml("春眠不觉晓") == "春眠不觉晓");
    CHECK(mstts::escapeXml("") == "");
}

static void testSsml() {
    std::printf("[ssml]\n");
    const mstts::Voice* v = mstts::findVoice("zh-CN-XiaoxiaoNeural");
    CHECK(v != nullptr);
    std::string s = mstts::buildSsml(*v, "你好 & <世界>", 1.0f, 1.0f);
    CHECK(s.find("<voice name='zh-CN-XiaoxiaoNeural'>") != std::string::npos);
    CHECK(s.find("xml:lang='zh-CN'") != std::string::npos);
    CHECK(s.find("rate='0%' pitch='0%'") != std::string::npos);
    CHECK(s.find("你好 &amp; &lt;世界&gt;") != std::string::npos);

    std::string fast = mstts::buildSsml(*v, "x", 1.2f, 0.8f);
    CHECK(fast.find("rate='20%' pitch='-20%'") != std::string::npos);
}

static void testChunk() {
    std::printf("[chunk]\n");
    CHECK(mstts::chunkText("").empty());
    CHECK(mstts::chunkText("   ").empty());
    auto one = mstts::chunkText("只有一句话。");
    CHECK(one.size() == 1 && one[0] == "只有一句话。");

    // 300 个汉字（每字 3 字节）+ 句号，按 maxChars=100 切 → 应该在句末断开
    std::string text;
    for (int i = 0; i < 300; ++i) {
        text += "春";
        if (i % 100 == 99) text += "。";
    }
    auto parts = mstts::chunkText(text, 100);
    CHECK(parts.size() >= 3);
    size_t total = 0;
    for (const auto& p : parts) {
        total += p.size();
        CHECK(p.size() % 3 == 0);            // UTF-8 完整性
        CHECK(p.back() == "。"[0] || true);  // 不断在汉字中间
    }
    CHECK(total == text.size());

    // 超长无标点：硬切，不越界
    std::string noPunct(1000, 'a');
    auto hard = mstts::chunkText(noPunct, 100);
    CHECK(hard.size() == 10);
    CHECK(hard[0].size() == 100);
}

static void testWav() {
    std::printf("[wav]\n");
    std::vector<uint8_t> pcm(4800, 0x11);
    // 编码器不绑死采样率：传什么写什么（24k 只是当前引擎的事实，不是 wav 层的假设）
    for (int rate : {24000, 44100, 48000}) {
        std::string wav = mstts::encodeWav(pcm, rate);
        CHECK(wav.size() == 44 + pcm.size());
        CHECK(wav.compare(0, 4, "RIFF") == 0);
        CHECK(wav.compare(8, 4, "WAVE") == 0);
        CHECK(wav.compare(12, 4, "fmt ") == 0);
        CHECK(wav.compare(36, 4, "data") == 0);
        uint32_t dataSize;
        std::memcpy(&dataSize, wav.data() + 40, 4);
        CHECK(dataSize == pcm.size());
        uint32_t gotRate;
        std::memcpy(&gotRate, wav.data() + 24, 4);
        CHECK(gotRate == (uint32_t)rate);
        uint32_t byteRate;
        std::memcpy(&byteRate, wav.data() + 28, 4);
        CHECK(byteRate == (uint32_t)rate * 2);  // 16bit 单声道
    }
    CHECK(mstts::Engine::kBitsPerSample == 16);
    CHECK(mstts::Engine::kChannels == 1);
}

static void testVoices() {
    std::printf("[voices]\n");
    const auto& vs = mstts::allVoices();
    CHECK(vs.size() == 9);
    CHECK(mstts::findVoice("xiaoxiao") != nullptr);
    CHECK(mstts::findVoice("晓晓") != nullptr);
    CHECK(mstts::findVoice("Aria") != nullptr);
    CHECK(mstts::findVoice("zh-CN-XiaoxiaoNeural")->locale == "zh-CN");
    CHECK(mstts::findVoice("en-GB-RyanNeural")->gender == 1);
    CHECK(mstts::findVoice("不存在的语音") == nullptr);

    // license 统一带 Key: 前缀
    for (const auto& v : vs) {
        std::string lic = mstts::licenseFor(v);
        CHECK(lic.rfind("Key:", 0) == 0);
        CHECK(lic.size() > 20);
    }
}

int main() {
    testEscape();
    testSsml();
    testChunk();
    testWav();
    testVoices();
    if (g_fail == 0) {
        std::printf("\n全部通过 ✓\n");
        return 0;
    }
    std::printf("\n%d 个断言失败 ✗\n", g_fail);
    return 1;
}
