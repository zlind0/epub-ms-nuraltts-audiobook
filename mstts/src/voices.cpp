#include "voices.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>

namespace mstts {
namespace {

// 设备 license：MultiTTS 每个「设备授权」语音包共用同一把密钥，等价于其
// libspeexdsp.so 里 Java_..._SpeexBridge_getLicense(0) 的返回值。
// 仅本机自用；重新发布或商用请按微软条款自行取得授权。
constexpr const char* kDeviceLicense =
    "ZCjZ7nHDSLvf4gpELteM4AnzaWUjTpn7UkV7D@vvksl0w1SNgon6d1905WANbktDc9S39oaA4r29HJNayXvTq8fJsq";

// zh-TW 云哲用的是包内自带密钥（config.yaml 里 param 的第二行），与其他语音不通用。
constexpr const char* kYunJheLicense =
    "BGJJwTRVfhRYIZq0xtySkIQlJbmBDsX6GsVyDRFHM0AzOjRvZ7ELI5kgzCUWYAKhTk99WDj5aOSWY@KHnffCDqlB008FmEUZHXM2lmKaFnfffnu4r8eiLUyYuH1uf4fSYA39OKQUZ9wY";

std::vector<Voice> buildVoices() {
    return {
        {"zh-CN-XiaoxiaoNeural", "晓晓", "zh-CN", 0,
         "Microsoft Xiaoxiao (Natural) - Chinese (Simplified, China)", ""},
        {"zh-CN-YunxiNeural", "云希", "zh-CN", 1,
         "Microsoft Yunxi (Natural) - Chinese (Simplified, China)", ""},
        {"zh-TW-YunJheNeural", "云哲", "zh-TW", 0,
         "Microsoft Server Speech Text to Speech Voice (zh-TW, YunJheNeural)", kYunJheLicense},
        {"en-US-AriaNeural", "Aria", "en-US", 0,
         "Microsoft Aria (Natural) - English (United States)", ""},
        {"en-US-JennyNeural", "Jenny", "en-US", 0,
         "Microsoft Jenny (Natural) - English (United States)", ""},
        {"en-US-GuyNeural", "Guy", "en-US", 1,
         "Microsoft Guy (Natural) - English (United States)", ""},
        {"en-GB-RyanNeural", "Ryan", "en-GB", 1,
         "Microsoft Ryan (Natural) - English (United Kingdom)", ""},
        {"en-IN-NeerjaNeural", "Neerja", "en-IN", 0,
         "Microsoft Neerja (Natural) - English (India)", ""},
        {"en-IN-PrabhatNeural", "Prabhat", "en-IN", 1,
         "Microsoft Prabhat (Natural) - English (India)", ""},
    };
}

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    return s;
}

std::string envLicenseKey(const std::string& code) {
    std::string key = "MSTTS_LICENSE_";
    for (char c : code) key += std::isalnum((unsigned char)c) ? (char)std::toupper((unsigned char)c) : '_';
    return key;
}

std::string withKeyPrefix(std::string raw) {
    while (!raw.empty() && (raw.back() == '\n' || raw.back() == '\r' || raw.back() == ' ')) raw.pop_back();
    if (raw.rfind("Key:", 0) != 0) raw = "Key:" + raw;
    return raw;
}

}  // namespace

const char* defaultDeviceLicense() { return kDeviceLicense; }

const std::vector<Voice>& allVoices() {
    static const std::vector<Voice> kVoices = buildVoices();
    return kVoices;
}

const Voice* findVoice(const std::string& id) {
    if (id.empty()) return nullptr;
    const auto& vs = allVoices();
    for (const auto& v : vs)
        if (v.code == id) return &v;
    for (const auto& v : vs)
        if (lower(v.shortName) == lower(id)) return &v;
    for (const auto& v : vs)
        if (v.displayName == id) return &v;
    for (const auto& v : vs) {
        if (v.code.size() >= id.size() && lower(v.code).rfind(lower(id), 0) == 0) return &v;
        if (lower(v.displayName).find(lower(id)) != std::string::npos) return &v;
    }
    return nullptr;
}

std::string licenseFor(const Voice& v) {
    if (const char* env = std::getenv(envLicenseKey(v.code).c_str()); env && *env)
        return withKeyPrefix(env);
    if (const char* env = std::getenv("MSTTS_LICENSE"); env && *env)
        return withKeyPrefix(env);
    if (!v.license.empty()) return withKeyPrefix(v.license);
    return withKeyPrefix(kDeviceLicense);
}

}  // namespace mstts
