#pragma once

#include <string>

namespace mstts {

struct ServerOptions {
    std::string host = "127.0.0.1";
    int port = 8765;
    std::string voiceRoot;   ///< 语音包根目录（已解析好的绝对路径）
};

/// 启动 HTTP 服务（阻塞，Ctrl-C 退出）。返回进程退出码。
int runServer(const ServerOptions& opt);

}  // namespace mstts
