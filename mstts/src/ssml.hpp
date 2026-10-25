#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "voices.hpp"

namespace mstts {

/// SSML 文本转义（& < > " '），与 Android 版 SsmlBuilder.escape 一致。
std::string escapeXml(std::string_view text);

/// 组装微软标准 SSML。rate/pitch 为倍数，1.0 = 原速原调。
std::string buildSsml(const Voice& voice, std::string_view text, float rate = 1.0f, float pitch = 1.0f);

/// 按句切分长文本：单块不超过 maxChars 个字符，优先在句末标点处断开。
/// 与 Android 版 Chunker.split 行为一致（maxChars 以 Unicode 码点计）。
std::vector<std::string> chunkText(std::string_view text, size_t maxChars = 450);

}  // namespace mstts
