// 内置语音表：与 multitts-instance/app/src/main/assets/msvoice 下的 9 个包一一对应。
// displayName 即 EmbeddedSpeechConfig::SetSpeechSynthesisVoice 的第一个参数，
// 逐字照抄语音包内的语音注册名；license 是该语音包的模型解密密钥。
#pragma once

#include <string>
#include <vector>

namespace mstts {

struct Voice {
    std::string code;          ///< 目录名 / SSML voice 名，如 zh-CN-XiaoxiaoNeural
    std::string shortName;     ///< 中文短名，命令行里可以直接用
    std::string locale;        ///< zh-CN / en-US ...
    int gender = 0;            ///< 0 女 1 男
    std::string displayName;   ///< 传给 MS SDK 的语音名
    std::string license;       ///< 该包的密钥（空则用默认设备密钥）
};

/// 全部 9 个语音（顺序固定，便于 UI 展示）。
const std::vector<Voice>& allVoices();

/// 按 code / 短名 / displayName 前缀查找；找不到返回 nullptr。
const Voice* findVoice(const std::string& id);

/// 该语音实际使用的 license 字符串（含 "Key:" 前缀）。
/// 可用环境变量 MSTTS_LICENSE 全局覆盖，MSTTS_LICENSE_<CODE>（非字母数字换成 _）单条覆盖。
std::string licenseFor(const Voice& v);

/// 默认设备 license（从 MultiTTS 的 libspeexdsp.so 中取出，见 docs/原理.md）。
const char* defaultDeviceLicense();

}  // namespace mstts
