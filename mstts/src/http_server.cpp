#include "http_server.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <vector>

#include "engine.hpp"
#include "json.hpp"
#include "ssml.hpp"
#include "wav.hpp"

namespace mstts {
namespace {

struct Request {
    std::string method;
    std::string target;
    std::string path;
    std::string query;
    std::string version;
    std::map<std::string, std::string> headers;
    std::string body;
};

std::string lower(std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

std::string urlDecode(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        if (in[i] == '+') {
            out += ' ';
        } else if (in[i] == '%' && i + 2 < in.size()) {
            auto hex = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return -1;
            };
            int h = hex(in[i + 1]), l = hex(in[i + 2]);
            if (h >= 0 && l >= 0) {
                out += (char)((h << 4) | l);
                i += 2;
            } else {
                out += in[i];
            }
        } else {
            out += in[i];
        }
    }
    return out;
}

std::map<std::string, std::string> parseQuery(const std::string& q) {
    std::map<std::string, std::string> out;
    size_t pos = 0;
    while (pos < q.size()) {
        size_t amp = q.find('&', pos);
        std::string kv = q.substr(pos, amp == std::string::npos ? std::string::npos : amp - pos);
        size_t eq = kv.find('=');
        if (eq != std::string::npos) out[urlDecode(kv.substr(0, eq))] = urlDecode(kv.substr(eq + 1));
        else if (!kv.empty()) out[urlDecode(kv)] = "";
        if (amp == std::string::npos) break;
        pos = amp + 1;
    }
    return out;
}

std::string jsonEscape(const std::string& s) {
    std::string out;
    for (char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if ((unsigned char)c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", (unsigned char)c);
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    return out;
}

bool sendAll(int fd, const void* data, size_t n) {
    const char* p = static_cast<const char*>(data);
    while (n) {
        ssize_t w = ::send(fd, p, n, 0);
        if (w <= 0) return false;
        p += w;
        n -= (size_t)w;
    }
    return true;
}

void respond(int fd, int code, const std::string& contentType, const std::string& body,
             const char* extraHeaders = nullptr) {
    const char* reason = code == 200 ? "OK" : code == 400 ? "Bad Request" : code == 404 ? "Not Found"
                       : code == 405 ? "Method Not Allowed" : code == 500 ? "Internal Server Error"
                       : "Error";
    std::ostringstream h;
    h << "HTTP/1.1 " << code << ' ' << reason << "\r\n";
    h << "Content-Type: " << contentType << "\r\n";
    h << "Content-Length: " << body.size() << "\r\n";
    h << "Access-Control-Allow-Origin: *\r\n";
    h << "Access-Control-Allow-Headers: Content-Type, Authorization\r\n";
    h << "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n";
    h << "Connection: close\r\n";
    if (extraHeaders) h << extraHeaders;
    h << "\r\n";
    std::string head = h.str();
    if (!sendAll(fd, head.data(), head.size())) return;
    if (!body.empty()) sendAll(fd, body.data(), body.size());
}

void respondJson(int fd, int code, const std::string& json) {
    respond(fd, code, "application/json; charset=utf-8", json);
}

std::string errorJson(const std::string& msg) {
    return "{\"error\":{\"message\":\"" + jsonEscape(msg) + "\",\"type\":\"invalid_request_error\"}}";
}

bool readRequest(int fd, Request& req) {
    std::string buf;
    char tmp[8192];
    size_t headerEnd = std::string::npos;
    while ((headerEnd = buf.find("\r\n\r\n")) == std::string::npos) {
        ssize_t n = ::recv(fd, tmp, sizeof(tmp), 0);
        if (n <= 0) return false;
        buf.append(tmp, (size_t)n);
        if (buf.size() > 64 * 1024) return false;
    }
    std::istringstream hs(buf.substr(0, headerEnd));
    std::string line;
    if (!std::getline(hs, line)) return false;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    {
        std::istringstream ls(line);
        ls >> req.method >> req.target >> req.version;
        if (req.method.empty() || req.target.empty()) return false;
        size_t q = req.target.find('?');
        req.path = q == std::string::npos ? req.target : req.target.substr(0, q);
        req.query = q == std::string::npos ? "" : req.target.substr(q + 1);
    }
    while (std::getline(hs, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) break;
        size_t c = line.find(':');
        if (c == std::string::npos) continue;
        std::string k = lower(line.substr(0, c));
        std::string v = line.substr(c + 1);
        while (!v.empty() && v.front() == ' ') v.erase(v.begin());
        req.headers[k] = v;
    }
    size_t contentLength = 0;
    if (auto it = req.headers.find("content-length"); it != req.headers.end())
        contentLength = (size_t)std::strtoull(it->second.c_str(), nullptr, 10);
    if (contentLength > 4 * 1024 * 1024) return false;
    req.body = buf.substr(headerEnd + 4);
    while (req.body.size() < contentLength) {
        ssize_t n = ::recv(fd, tmp, sizeof(tmp), 0);
        if (n <= 0) break;
        req.body.append(tmp, (size_t)n);
    }
    return true;
}

const char* kIndexHtml = R"HTML(<!doctype html><meta charset="utf-8">
<title>mstts</title>
<style>
 body{font:15px/1.6 -apple-system,system-ui,sans-serif;max-width:760px;margin:40px auto;padding:0 16px;color:#222}
 textarea{width:100%;height:120px;font:inherit;padding:8px;box-sizing:border-box}
 select,input{padding:4px 6px;font:inherit}
 button{padding:6px 16px;font:inherit;cursor:pointer}
 .row{margin:12px 0;display:flex;gap:12px;align-items:center;flex-wrap:wrap}
 audio{width:100%;margin-top:16px}
 code{background:#f4f4f4;padding:1px 4px;border-radius:3px}
</style>
<h2>mstts · 离线微软神经语音</h2>
<textarea id="t">春眠不觉晓，处处闻啼鸟。夜来风雨声，花落知多少。</textarea>
<div class="row">
 <label>语音 <select id="v"></select></label>
 <label>语速 <input id="r" type="number" value="1.0" step="0.05" min="0.5" max="2" size="4"></label>
 <label>音调 <input id="p" type="number" value="1.0" step="0.05" min="0.5" max="2" size="4"></label>
 <button id="go">合成</button>
 <span id="s"></span>
</div>
<audio id="a" controls></audio>
<script>
fetch('/v1/audio/voices').then(r=>r.json()).then(vs=>{
  const v=document.getElementById('v');
  vs.voices.forEach(x=>{const o=document.createElement('option');o.value=x.id;o.textContent=x.name+' · '+x.locale;v.appendChild(o);});
});
document.getElementById('go').onclick=async()=>{
  const s=document.getElementById('s'); s.textContent='合成中…';
  const t0=performance.now();
  const r=await fetch('/v1/audio/speech',{method:'POST',headers:{'Content-Type':'application/json'},
    body:JSON.stringify({input:document.getElementById('t').value,voice:document.getElementById('v').value,
      speed:parseFloat(document.getElementById('r').value),pitch:parseFloat(document.getElementById('p').value)})});
  if(!r.ok){ s.textContent='失败：'+await r.text(); return; }
  const b=await r.blob();
  const a=document.getElementById('a'); a.src=URL.createObjectURL(b); a.play();
  s.textContent=((performance.now()-t0)/1000).toFixed(2)+'s / '+(b.size/48000).toFixed(1)+'s 音频';
};
</script>
)HTML";

class Server {
public:
    explicit Server(const ServerOptions& opt) : opt_(opt), engine_(opt.voiceRoot) {}

    int run();
    void handle(int fd);

private:
    void route(const Request& req, int fd);
    std::string voicesJson() const;

    ServerOptions opt_;
    Engine engine_;
};

std::string Server::voicesJson() const {
    std::ostringstream o;
    o << "{\"voices\":[";
    bool first = true;
    for (const auto& v : allVoices()) {
        if (!first) o << ',';
        first = false;
        o << "{\"id\":\"" << v.code << "\",\"name\":\"" << jsonEscape(v.shortName) << "\",\"locale\":\""
          << v.locale << "\",\"gender\":\"" << (v.gender == 0 ? "Female" : "Male") << "\",\"installed\":"
          << (engine_.voiceInstalled(v) ? "true" : "false") << "}";
    }
    o << "],\"engine\":\"microsoft-embedded\",\"sampleRate\":" << Engine::kSampleRate << "}";
    return o.str();
}

void Server::route(const Request& req, int fd) {
    if (req.method == "OPTIONS") return respond(fd, 200, "text/plain", "");
    if (req.method == "GET" && (req.path == "/" || req.path == "/index.html"))
        return respond(fd, 200, "text/html; charset=utf-8", kIndexHtml);
    if (req.method == "GET" && req.path == "/health")
        return respondJson(fd, 200,
                           "{\"status\":\"ok\",\"engine\":\"microsoft-embedded\",\"voices\":" +
                               std::to_string(engine_.installedVoices().size()) + "}");
    if (req.method == "GET" && req.path == "/v1/audio/voices") return respondJson(fd, 200, voicesJson());

    auto synthesizeAndSend = [&](const std::string& text, const std::string& voiceId, float rate,
                                 float pitch, bool raw) {
        if (text.empty()) return respondJson(fd, 400, errorJson("input 不能为空"));
        const Voice* v = findVoice(voiceId.empty() ? "zh-CN-XiaoxiaoNeural" : voiceId);
        if (!v) return respondJson(fd, 400, errorJson("未知语音: " + voiceId + "（GET /v1/audio/voices 查看）"));
        SynthOptions o;
        o.rate = rate;
        o.pitch = pitch;
        try {
            auto pcm = engine_.synthesize(*v, text, o);
            const std::string rate = std::to_string(Engine::kSampleRate);
            const std::string header = "X-Sample-Rate: " + rate + "\r\n";
            if (raw)
                respond(fd, 200, "audio/L16;rate=" + rate + ";channels=1",
                        std::string((const char*)pcm.data(), pcm.size()), header.c_str());
            else
                respond(fd, 200, "audio/wav", encodeWav(pcm, Engine::kSampleRate), header.c_str());
        } catch (const std::exception& e) {
            respondJson(fd, 500, errorJson(e.what()));
        }
    };

    if (req.method == "GET" && (req.path == "/say" || req.path == "/v1/audio/speech")) {
        auto q = parseQuery(req.query);
        auto get = [&](const char* k, const std::string& def = "") {
            auto it = q.find(k);
            return it == q.end() ? def : it->second;
        };
        float rate = 1.0f, pitch = 1.0f;
        if (!get("rate").empty()) rate = std::strtof(get("rate").c_str(), nullptr);
        if (!get("speed").empty()) rate = std::strtof(get("speed").c_str(), nullptr);
        if (!get("pitch").empty()) pitch = std::strtof(get("pitch").c_str(), nullptr);
        return synthesizeAndSend(get("text", get("input")), get("voice"), rate, pitch,
                                 get("format") == "pcm");
    }

    if (req.method == "POST" && req.path == "/v1/audio/speech") {
        std::map<std::string, std::string> body;
        if (!jsonParseFlatObject(req.body, body))
            return respondJson(fd, 400, errorJson("请求体不是合法 JSON 对象"));
        std::string input = body.count("input") ? body["input"] : "";
        std::string voice = body.count("voice") ? body["voice"] : "";
        float rate = 1.0f, pitch = 1.0f;
        if (body.count("speed")) rate = std::strtof(body["speed"].c_str(), nullptr);
        if (body.count("rate")) rate = std::strtof(body["rate"].c_str(), nullptr);
        if (body.count("pitch")) pitch = std::strtof(body["pitch"].c_str(), nullptr);
        bool raw = body.count("response_format") && body["response_format"] == "pcm";
        return synthesizeAndSend(input, voice, rate, pitch, raw);
    }

    respondJson(fd, 404, errorJson("没有这个接口: " + req.path));
}

void Server::handle(int fd) {
    struct timeval tv { 15, 0 };
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    Request req;
    if (readRequest(fd, req)) {
        try {
            route(req, fd);
        } catch (const std::exception& e) {
            respondJson(fd, 500, errorJson(e.what()));
        }
    }
    ::close(fd);
}

int Server::run() {
    int srv = ::socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) {
        std::perror("socket");
        return 1;
    }
    int one = 1;
    ::setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)opt_.port);
    if (::inet_pton(AF_INET, opt_.host.c_str(), &addr.sin_addr) != 1) {
        std::fprintf(stderr, "无效监听地址: %s\n", opt_.host.c_str());
        ::close(srv);
        return 1;
    }
    if (::bind(srv, (sockaddr*)&addr, sizeof(addr)) < 0) {
        std::perror("bind");
        std::fprintf(stderr, "端口 %d 可能被占用\n", opt_.port);
        ::close(srv);
        return 1;
    }
    if (::listen(srv, 64) < 0) {
        std::perror("listen");
        ::close(srv);
        return 1;
    }

    const auto installed = engine_.installedVoices();
    std::fprintf(stderr, "mstts serve: http://%s:%d  (语音包 %s，已装 %zu/%zu)\n",
                 opt_.host.c_str(), opt_.port, opt_.voiceRoot.c_str(), installed.size(),
                 allVoices().size());
    if (installed.empty())
        std::fprintf(stderr, "  ⚠️  没有找到语音包，先跑 scripts/install-voices.sh\n");

    while (true) {
        sockaddr_in peer{};
        socklen_t len = sizeof(peer);
        int fd = ::accept(srv, (sockaddr*)&peer, &len);
        if (fd < 0) {
            if (errno == EINTR) continue;
            std::perror("accept");
            break;
        }
        std::thread([this, fd]() { handle(fd); }).detach();
    }
    ::close(srv);
    return 0;
}

}  // namespace

int runServer(const ServerOptions& opt) {
    Server s(opt);
    return s.run();
}

}  // namespace mstts
