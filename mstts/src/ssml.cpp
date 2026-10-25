#include "ssml.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>

namespace mstts {
namespace {

bool isBreakCp(uint32_t cp) {
    switch (cp) {
        case U'。': case U'！': case U'？': case U'!': case U'?':
        case U'\n': case U';': case U'；':
            return true;
        default:
            return false;
    }
}

// 返回从 i 开始的一个 UTF-8 码点长度（字节数），坏字节按 1 处理。
size_t cpLen(std::string_view s, size_t i) {
    unsigned char c = (unsigned char)s[i];
    size_t n = 1;
    if ((c & 0xE0) == 0xC0) n = 2;
    else if ((c & 0xF0) == 0xE0) n = 3;
    else if ((c & 0xF8) == 0xF0) n = 4;
    if (i + n > s.size()) n = 1;
    return n;
}

uint32_t decodeCp(std::string_view s, size_t i) {
    unsigned char c = (unsigned char)s[i];
    size_t n = cpLen(s, i);
    if (n == 1) return c;
    uint32_t cp = 0;
    if (n == 2) cp = c & 0x1F;
    else if (n == 3) cp = c & 0x0F;
    else cp = c & 0x07;
    for (size_t k = 1; k < n; ++k) cp = (cp << 6) | ((unsigned char)s[i + k] & 0x3F);
    return cp;
}

std::string trim(std::string_view s) {
    size_t b = 0, e = s.size();
    while (b < e && (unsigned char)s[b] <= ' ') ++b;
    while (e > b && (unsigned char)s[e - 1] <= ' ') --e;
    return std::string(s.substr(b, e - b));
}

int percent(float v) {
    if (v < 0.1f) v = 0.1f;
    if (v > 10.0f) v = 10.0f;
    // 四舍五入，避免 0.8f 这类浮点误差变成 -19%
    return (int)std::lround((v - 1.0f) * 100.0f);
}

}  // namespace

std::string escapeXml(std::string_view text) {
    std::string out;
    out.reserve(text.size() + 16);
    for (char c : text) {
        switch (c) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            case '\'': out += "&apos;"; break;
            default: out += c;
        }
    }
    return out;
}

std::string buildSsml(const Voice& voice, std::string_view text, float rate, float pitch) {
    char prosody[64];
    std::snprintf(prosody, sizeof(prosody), "<prosody rate='%d%%' pitch='%d%%'>", percent(rate),
                  percent(pitch));
    std::string out;
    out.reserve(text.size() + 256);
    out += "<speak version='1.0' xml:lang='";
    out += voice.locale;
    out += "'><voice name='";
    out += voice.code;
    out += "'>";
    out += prosody;
    out += escapeXml(text);
    out += "</prosody></voice></speak>";
    return out;
}

std::vector<std::string> chunkText(std::string_view text, size_t maxChars) {
    std::vector<std::string> out;
    std::string t = trim(text);
    if (t.empty()) return out;
    if (maxChars < 16) maxChars = 16;
    if (t.size() <= maxChars) {  // 字节数不超过 maxChars 时码点数必然更少
        out.push_back(t);
        return out;
    }

    size_t start = 0;       // 当前块起始字节
    size_t lastBreak = 0;   // 最近一个断句点之后的位置
    bool hasBreak = false;
    size_t count = 0;       // 当前块内已计入的码点数
    size_t i = 0;
    while (i < t.size()) {
        size_t n = cpLen(t, i);
        ++count;
        if (isBreakCp(decodeCp(t, i))) {
            lastBreak = i + n;
            hasBreak = true;
        }
        if (count >= maxChars) {
            size_t cut = hasBreak && lastBreak > start ? lastBreak : i + n;
            std::string piece = trim(std::string_view(t).substr(start, cut - start));
            if (!piece.empty()) out.push_back(std::move(piece));
            start = cut;
            hasBreak = false;
            lastBreak = start;
            count = 0;
        }
        i += n;
    }
    if (start < t.size()) {
        std::string piece = trim(std::string_view(t).substr(start));
        if (!piece.empty()) out.push_back(std::move(piece));
    }
    return out;
}

}  // namespace mstts
