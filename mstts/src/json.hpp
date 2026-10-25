#pragma once

#include <map>
#include <string>

namespace mstts {

/// 极简 JSON 对象解析：只把顶层 key 的标量值取出来（字符串/数字/布尔），
/// 嵌套对象/数组整段跳过。够 HTTP 接口用，不引入第三方依赖。
/// 解析失败返回 false。
bool jsonParseFlatObject(const std::string& text, std::map<std::string, std::string>& out);

}  // namespace mstts
