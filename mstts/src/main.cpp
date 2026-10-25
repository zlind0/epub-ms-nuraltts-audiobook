// mstts —— 在 macOS 上跑微软离线（Embedded）神经 TTS 的命令行工具。
//
//   mstts list                        # 语音表
//   mstts say "春眠不觉晓" --play       # 合成并播放
//   mstts say -f book.txt -o book.wav  # 长文本 → wav
//   mstts serve --port 8765            # 本地 HTTP 服务（OpenAI 兼容）
//   mstts doctor                       # 自检

#include <mach-o/dyld.h>
#include <unistd.h>

#include <cctype>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "engine.hpp"
#include "http_server.hpp"
#include "player.hpp"
#include "ssml.hpp"
#include "voices.hpp"
#include "wav.hpp"

namespace {

constexpr const char* kVersion = "1.0.0";

struct Args {
    std::string cmd;
    std::vector<std::string> rest;
    std::string voice = "zh-CN-XiaoxiaoNeural";
    float rate = 1.0f;
    float pitch = 1.0f;
    std::string out;
    std::string inFile;      ///< say -f / ssml 的输入文件
    std::string voicesDir;
    std::string host = "127.0.0.1";
    int port = 8765;
    size_t chunk = 450;
    bool raw = false;
    bool play = false;
    bool json = false;
    bool quiet = false;
};

void usage() {
    std::printf(R"(mstts %s —— 离线微软神经语音（macOS）

用法:
  mstts list [--json]                 列出全部语音
  mstts say <文本...> [选项]          合成一段文本
  mstts say -f <文件> [选项]          从文件读文本
  mstts ssml <文件|-> [选项]          直接合成 SSML
  mstts serve [--port 8765] [选项]    启动本地 HTTP 服务
  mstts doctor                        逐语音自检

输出（默认：终端下直接播放；被管道时写 WAV 到 stdout）:
  -o, --out <路径|->     写 WAV（- = stdout）
      --raw              裸 PCM（%d Hz / 16bit / 单声道）
      --play             用系统音频设备播放

合成参数:
  -v, --voice <id>       语音，可写 code / 短名（晓晓、Aria…），默认 zh-CN-XiaoxiaoNeural
  -r, --rate <倍数>      语速 0.5–2.0，默认 1.0
  -p, --pitch <倍数>     音调 0.5–2.0，默认 1.0
      --chunk <字符数>   长文本切块上限，默认 450

其他:
      --voices <目录>    语音包根目录（默认 $MSTTS_VOICES 或 ~/Library/Application Support/mstts/voices）
      --host/--port      服务监听地址，默认 127.0.0.1:8765
  -q, --quiet            少打印
  -h, --help             本帮助
      --version          版本

例:
  mstts say "春眠不觉晓，处处闻啼鸟。" --play
  mstts say -f chapter1.txt -v 晓晓 -r 1.1 -o chapter1.wav
  echo "The quick brown fox." | mstts say - -v Aria -o fox.wav
  mstts serve --port 8765   # 然后 curl http://127.0.0.1:8765/v1/audio/voices

)",
               kVersion, mstts::Engine::kSampleRate);
}

bool isTty(FILE* f) { return ::isatty(fileno(f)) == 1; }

std::string exeDir() {
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::vector<char> buf(size + 1, 0);
    if (_NSGetExecutablePath(buf.data(), &size) != 0) return ".";
    char resolved[PATH_MAX];
    if (!::realpath(buf.data(), resolved)) return ".";
    std::string p(resolved);
    auto slash = p.find_last_of('/');
    return slash == std::string::npos ? "." : p.substr(0, slash);
}

bool hasAnyVoice(const std::string& dir) {
    for (const auto& v : mstts::allVoices()) {
        std::string probe = dir + "/" + v.code + "/Tokens.xml";
        if (FILE* f = std::fopen(probe.c_str(), "rb")) {
            std::fclose(f);
            return true;
        }
    }
    return false;
}

std::string homeDir() {
    const char* h = std::getenv("HOME");
    return h ? h : ".";
}

std::string defaultVoiceRoot() { return homeDir() + "/Library/Application Support/mstts/voices"; }

std::string resolveVoiceRoot(const std::string& cli) {
    std::vector<std::string> candidates;
    if (!cli.empty()) candidates.push_back(cli);
    if (const char* env = std::getenv("MSTTS_VOICES"); env && *env) candidates.push_back(env);
    candidates.push_back(defaultVoiceRoot());
    const std::string dir = exeDir();
    candidates.push_back(dir + "/voices");
    candidates.push_back(dir + "/../voices");
    candidates.push_back("voices");
    for (const auto& c : candidates)
        if (hasAnyVoice(c)) return c;
    return candidates.empty() ? defaultVoiceRoot() : candidates.front();
}

std::string padRight(const std::string& s, size_t width);

void printVoices(const mstts::Engine& engine, bool json) {
    if (json) {
        std::printf("{\"voices\":[");
        bool first = true;
        for (const auto& v : mstts::allVoices()) {
            if (!first) std::printf(",");
            first = false;
            std::printf("{\"id\":\"%s\",\"short\":\"%s\",\"locale\":\"%s\",\"gender\":\"%s\",\"installed\":%s}",
                        v.code.c_str(), v.shortName.c_str(), v.locale.c_str(),
                        v.gender == 0 ? "Female" : "Male",
                        engine.voiceInstalled(v) ? "true" : "false");
        }
        std::printf("]}\n");
        return;
    }
    std::printf("%s %s %s %s %s\n", padRight("code", 24).c_str(), padRight("短名", 8).c_str(),
                padRight("语言", 6).c_str(), padRight("性别", 6).c_str(), "状态");
    for (const auto& v : mstts::allVoices())
        std::printf("%s %s %s %s %s\n", padRight(v.code, 24).c_str(), padRight(v.shortName, 8).c_str(),
                    padRight(v.locale, 6).c_str(), padRight(v.gender == 0 ? "女" : "男", 6).c_str(),
                    engine.voiceInstalled(v) ? "✓" : "缺（未安装）");
    std::printf("\n语音包目录: %s\n", engine.voiceRoot().c_str());
}

std::string joinArgs(const std::vector<std::string>& v, size_t from) {
    std::string s;
    for (size_t i = from; i < v.size(); ++i) {
        if (i > from) s += ' ';
        s += v[i];
    }
    return s;
}

// 终端列宽（CJK 算 2 列），让中文表格对齐
size_t displayWidth(const std::string& s) {
    size_t w = 0, i = 0;
    while (i < s.size()) {
        unsigned char c = (unsigned char)s[i];
        size_t n = 1;
        uint32_t cp = c;
        if ((c & 0xE0) == 0xC0) { n = 2; cp = c & 0x1F; }
        else if ((c & 0xF0) == 0xE0) { n = 3; cp = c & 0x0F; }
        else if ((c & 0xF8) == 0xF0) { n = 4; cp = c & 0x07; }
        for (size_t k = 1; k < n && i + k < s.size(); ++k) cp = (cp << 6) | ((unsigned char)s[i + k] & 0x3F);
        i += n;
        bool wide = (cp >= 0x1100 && cp <= 0x115F) || (cp >= 0x2E80 && cp <= 0xA4CF) ||
                    (cp >= 0xAC00 && cp <= 0xD7A3) || (cp >= 0xF900 && cp <= 0xFAFF) ||
                    (cp >= 0xFE30 && cp <= 0xFE4F) || (cp >= 0xFF00 && cp <= 0xFF60) ||
                    (cp >= 0xFFE0 && cp <= 0xFFE6) || (cp >= 0x20000 && cp <= 0x3FFFD);
        w += wide ? 2 : 1;
    }
    return w;
}

std::string padRight(const std::string& s, size_t width) {
    size_t w = displayWidth(s);
    return w >= width ? s : s + std::string(width - w, ' ');
}

int cmdDoctor(const mstts::Engine& engine) {
    std::fprintf(stderr, "语音包目录: %s\n", engine.voiceRoot().c_str());
    std::fprintf(stderr, "输出格式: %d Hz / %d bit / %d 声道（离线引擎上限；44.1k/48k 会被拒绝）\n",
                 mstts::Engine::kSampleRate, mstts::Engine::kBitsPerSample, mstts::Engine::kChannels);
    int fail = 0, ok = 0, missing = 0;
    for (const auto& v : mstts::allVoices()) {
        if (!engine.voiceInstalled(v)) {
            std::printf("  %-24s 缺失\n", v.code.c_str());
            ++missing;
            continue;
        }
        const std::string text = v.locale.rfind("zh", 0) == 0 ? "语音自检通过。" : "Voice check passed.";
        try {
            double t0 = (double)clock() / CLOCKS_PER_SEC;
            auto pcm = engine.synthesize(v, text, {});
            double dt = (double)clock() / CLOCKS_PER_SEC - t0;
            std::printf("  %-24s ✓ %.2fs 音频  (%.0f ms)\n", v.code.c_str(), mstts::pcmSeconds(pcm.size()),
                        dt * 1000);
            ++ok;
        } catch (const std::exception& e) {
            std::printf("  %-24s ✗ %s\n", v.code.c_str(), e.what());
            ++fail;
        }
    }
    std::printf("\n合计: 正常 %d / 失败 %d / 缺失 %d\n", ok, fail, missing);
    if (missing) std::printf("安装语音包: scripts/install-voices.sh\n");
    return fail ? 1 : 0;
}

int cmdList(const Args& a) {
    mstts::Engine engine(resolveVoiceRoot(a.voicesDir));
    printVoices(engine, a.json);
    return 0;
}

int cmdSynthesize(const Args& a, bool isSsml) {
    const mstts::Voice* voice = mstts::findVoice(a.voice);
    if (!voice) {
        std::fprintf(stderr, "未知语音: %s（mstts list 查看）\n", a.voice.c_str());
        return 2;
    }
    std::string text;
    if (!a.inFile.empty()) {
        text = mstts::readAll(a.inFile);
    } else if (!a.rest.empty()) {
        text = joinArgs(a.rest, 0);
        if (text == "-") text = mstts::readAll("-");
    } else if (!isTty(stdin)) {
        text = mstts::readAll("-");
    }
    if (text.find_first_not_of(" \t\r\n") == std::string::npos) {
        std::fprintf(stderr, "文本为空（用法：mstts say \"要念的文字\"）\n");
        return 2;
    }

    mstts::Engine engine(resolveVoiceRoot(a.voicesDir));
    mstts::SynthOptions opt;
    opt.rate = a.rate;
    opt.pitch = a.pitch;
    opt.maxChars = a.chunk;

    try {
        const double t0 = (double)clock() / CLOCKS_PER_SEC;
        std::vector<uint8_t> pcm = isSsml ? engine.synthesizeSsml(*voice, text)
                                         : engine.synthesize(*voice, text, opt);
        const double dt = (double)clock() / CLOCKS_PER_SEC - t0;

        const bool toStdout = a.out == "-" || (a.out.empty() && !a.play && !isTty(stdout));
        const bool doPlay = a.play || (a.out.empty() && !toStdout && isTty(stdout));

        if (toStdout || !a.out.empty()) {
            std::string payload =
                a.raw ? std::string((const char*)pcm.data(), pcm.size())
                      : mstts::encodeWav(pcm, mstts::Engine::kSampleRate);
            mstts::writeAll(a.out.empty() ? "-" : a.out, payload.data(), payload.size());
        }
        if (doPlay) mstts::playPcm(pcm.data(), pcm.size(), mstts::Engine::kSampleRate);

        if (!a.quiet) {
            std::string where = toStdout ? " · → stdout" : (a.out.empty() ? "" : " · → " + a.out);
            std::fprintf(stderr, "[mstts] %s · %.2fs 音频 · 合成 %.0f ms%s\n", voice->shortName.c_str(),
                         mstts::pcmSeconds(pcm.size()), dt * 1000, where.c_str());
        }
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "错误: %s\n", e.what());
        return 1;
    }
}

int cmdServe(const Args& a) {
    mstts::ServerOptions opt;
    opt.host = a.host;
    opt.port = a.port;
    opt.voiceRoot = resolveVoiceRoot(a.voicesDir);
    return mstts::runServer(opt);
}

}  // namespace

int main(int argc, char** argv) {
    Args a;
    std::vector<std::string> positional;
    for (int i = 1; i < argc; ++i) {
        std::string s = argv[i];
        auto need = [&](const char* name) -> std::string {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "%s 需要一个参数\n", name);
                std::exit(2);
            }
            return argv[++i];
        };
        if (s == "-h" || s == "--help") {
            usage();
            return 0;
        } else if (s == "--version") {
            std::printf("mstts %s\n", kVersion);
            return 0;
        } else if (s == "--json") {
            a.json = true;
        } else if (s == "-q" || s == "--quiet") {
            a.quiet = true;
        } else if (s == "--raw") {
            a.raw = true;
        } else if (s == "--play") {
            a.play = true;
        } else if (s == "-v" || s == "--voice") {
            a.voice = need("--voice");
        } else if (s == "-r" || s == "--rate") {
            a.rate = std::strtof(need("--rate").c_str(), nullptr);
        } else if (s == "-p" || s == "--pitch") {
            a.pitch = std::strtof(need("--pitch").c_str(), nullptr);
        } else if (s == "-o" || s == "--out") {
            a.out = need("--out");
        } else if (s == "--voices") {
            a.voicesDir = need("--voices");
        } else if (s == "--host") {
            a.host = need("--host");
        } else if (s == "--port") {
            a.port = std::atoi(need("--port").c_str());
        } else if (s == "--chunk") {
            a.chunk = (size_t)std::atoi(need("--chunk").c_str());
        } else if (s == "-f" || s == "--file") {
            a.inFile = need("--file");
        } else {
            positional.push_back(s);
        }
    }

    if (!positional.empty()) {
        a.cmd = positional[0];
        a.rest.assign(positional.begin() + 1, positional.end());
    }
    if (a.cmd.empty() || a.cmd == "help") {
        usage();
        return a.cmd.empty() ? 2 : 0;
    }

    if (a.cmd == "list" || a.cmd == "voices") return cmdList(a);
    if (a.cmd == "doctor") {
        mstts::Engine engine(resolveVoiceRoot(a.voicesDir));
        return cmdDoctor(engine);
    }
    if (a.cmd == "serve") return cmdServe(a);
    if (a.cmd == "ssml") {
        Args aa = a;
        if (aa.inFile.empty() && !aa.rest.empty() && aa.rest[0] != "-") {
            aa.inFile = aa.rest[0];
            aa.rest.clear();
        }
        return cmdSynthesize(aa, /*isSsml=*/true);
    }
    if (a.cmd == "say") return cmdSynthesize(a, /*isSsml=*/false);

    std::fprintf(stderr, "未知命令: %s\n\n", a.cmd.c_str());
    usage();
    return 2;
}
