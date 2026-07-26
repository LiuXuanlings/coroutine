#ifndef MINICYBER_COMMON_TYPES_H_
#define MINICYBER_COMMON_TYPES_H_

#include <cstdint>

namespace minicyber {

class NullType {};

// Return code definition for minicyber internal function return.
enum ReturnCode {
  SUCC = 0,
  FAIL = 1,
};

/**
 * @brief Describe relation between nodes, writers/readers...
 */
enum Relation : std::uint8_t {
  NO_RELATION = 0,
  DIFF_HOST,  // different host
  DIFF_PROC,  // same host, but different process
  SAME_PROC,  // same process
};

/// RPC Channel 后缀：Service 根据 service_name 自动创建 Request/Response 两个
/// 隐式 Channel。Client 发请求到 Request Channel，Server 发响应到 Response Channel。
/// 与 CyberRT 的 SRV_CHANNEL_REQ_SUFFIX / SRV_CHANNEL_RES_SUFFIX 完全对齐。
static const char SRV_CHANNEL_REQ_SUFFIX[] = "__SRV__REQUEST";
static const char SRV_CHANNEL_RES_SUFFIX[] = "__SRV__RESPONSE";

}  // namespace minicyber

#endif  // MINICYBER_COMMON_TYPES_H_
