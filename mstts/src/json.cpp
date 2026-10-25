#include "json.hpp"

#include <cstdlib>

namespace mstts {
namespace {

struct Parser {
    const std::string& s;
    size_t i = 0;
    explicit Parser(const std::string& text) : s(text) {}

    void ws() {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) ++i;
    }
    bool eof() const { return i >= s.size(); }

    static void appendUtf8(std::string& out, unsigned cp) {
        if (cp < 0x80) {
            out += (char)cp;
        } else if (cp < 0x800) {
            out += (char)(0xC0 | (cp >> 6));
            out += (char)(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += (char)(0xE0 | (cp >> 12));
            out += (char)(0x80 | ((cp >> 6) & 0x3F));
            out += (char)(0x80 | (cp & 0x3F));
        } else {
            out += (char)(0xF0 | (cp >> 18));
            out += (char)(0x80 | ((cp >> 12) & 0x3F));
            out += (char)(0x80 | ((cp >> 6) & 0x3F));
            out += (char)(0x80 | (cp & 0x3F));
        }
    }

    bool hex4(unsigned& v) {
        if (i + 4 > s.size()) return false;
        v = 0;
        for (int k = 0; k < 4; ++k) {
            char c = s[i++];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= (unsigned)(c - '0');
            else if (c >= 'a' && c <= 'f') v |= (unsigned)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= (unsigned)(c - 'A' + 10);
            else return false;
        }
        return true;
    }

    bool string(std::string& out) {
        if (eof() || s[i] != '"') return false;
        ++i;
        out.clear();
        while (i < s.size()) {
            char c = s[i++];
            if (c == '"') return true;
            if (c != '\\') {
                out += c;
                continue;
            }
            if (eof()) return false;
            char e = s[i++];
            switch (e) {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'u': {
                    unsigned cp = 0;
                    if (!hex4(cp)) return false;
                    if (cp >= 0xD800 && cp <= 0xDBFF && i + 6 <= s.size() && s[i] == '\\' &&
                        s[i + 1] == 'u') {
                        size_t save = i;
                        i += 2;
                        unsigned lo = 0;
                        if (hex4(lo) && lo >= 0xDC00 && lo <= 0xDFFF)
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        else
                            i = save;
                    }
                    appendUtf8(out, cp);
                    break;
                }
                default: return false;
            }
        }
        return false;
    }

    // 跳过一个值（可用于数组/嵌套对象）
    bool skipValue(int depth) {
        if (depth > 64) return false;
        ws();
        if (eof()) return false;
        char c = s[i];
        if (c == '"') {
            std::string tmp;
            return string(tmp);
        }
        if (c == '{') {
            ++i;
            ws();
            if (!eof() && s[i] == '}') { ++i; return true; }
            while (true) {
                std::string k;
                ws();
                if (!string(k)) return false;
                ws();
                if (eof() || s[i] != ':') return false;
                ++i;
                if (!skipValue(depth + 1)) return false;
                ws();
                if (eof()) return false;
                if (s[i] == ',') { ++i; continue; }
                if (s[i] == '}') { ++i; return true; }
                return false;
            }
        }
        if (c == '[') {
            ++i;
            ws();
            if (!eof() && s[i] == ']') { ++i; return true; }
            while (true) {
                if (!skipValue(depth + 1)) return false;
                ws();
                if (eof()) return false;
                if (s[i] == ',') { ++i; continue; }
                if (s[i] == ']') { ++i; return true; }
                return false;
            }
        }
        // 数字 / true / false / null
        size_t start = i;
        while (i < s.size() && s[i] != ',' && s[i] != '}' && s[i] != ']' && s[i] != ' ' &&
               s[i] != '\n' && s[i] != '\r' && s[i] != '\t')
            ++i;
        return i > start;
    }

    // 标量值 → 文本；容器类型返回 false（由调用方 skipValue）
    bool scalar(std::string& out) {
        ws();
        if (eof()) return false;
        char c = s[i];
        if (c == '"') return string(out);
        size_t start = i;
        while (i < s.size() && s[i] != ',' && s[i] != '}' && s[i] != ']' && s[i] != ' ' &&
               s[i] != '\n' && s[i] != '\r' && s[i] != '\t')
            ++i;
        if (i == start) return false;
        out = s.substr(start, i - start);
        return true;
    }
};

}  // namespace

bool jsonParseFlatObject(const std::string& text, std::map<std::string, std::string>& out) {
    Parser p(text);
    p.ws();
    if (p.eof() || p.s[p.i] != '{') return false;
    ++p.i;
    p.ws();
    if (!p.eof() && p.s[p.i] == '}') return true;
    while (true) {
        std::string key;
        p.ws();
        if (!p.string(key)) return false;
        p.ws();
        if (p.eof() || p.s[p.i] != ':') return false;
        ++p.i;
        p.ws();
        if (!p.eof() && (p.s[p.i] == '{' || p.s[p.i] == '[')) {
            if (!p.skipValue(0)) return false;
        } else {
            std::string val;
            if (!p.scalar(val)) return false;
            out[key] = val;
        }
        p.ws();
        if (p.eof()) return false;
        if (p.s[p.i] == ',') {
            ++p.i;
            continue;
        }
        if (p.s[p.i] == '}') return true;
        return false;
    }
}

}  // namespace mstts
