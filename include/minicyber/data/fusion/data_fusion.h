#ifndef MINICYBER_DATA_FUSION_DATA_FUSION_H_
#define MINICYBER_DATA_FUSION_DATA_FUSION_H_

#include <cstdint>
#include <memory>

#include "minicyber/common/types.h"

// =============================================================================
// DataFusion：多通道数据融合纯虚基类（对齐 CyberRT data::fusion::DataFusion）
//
// 在 CyberRT 中融合策略是全四元组偏特化 (M0, M1, M2, M3)。
// MiniCyber 当前聚焦最常用的场景（M0 = primary, M1 = secondary），
// 实现 2 通道版本。后续按需拓展 3/4 通道偏特化。
//
// 职责：定义 Fusion() 契约。派生类（AllLatest）实现具体的时间戳对齐/最新值
//   匹配策略。实际使用中，上层（DataVisitor/Component）持有 DataFusion 并
//   通过虚函数多态调用，对融合策略无感知（Strategy 模式）。
// =============================================================================

namespace minicyber {
namespace data {
namespace fusion {

// 2 通道融合基类（M0 = primary, M1 = secondary）
template <typename M0, typename M1 = NullType>
class DataFusion {
 public:
  virtual ~DataFusion() = default;

  /// 从融合缓冲区取出一组对齐后的消息对。
  /// @param[in,out] index 消费游标。*index==0 ⇒ 取最新；否则取 index 位置。
  ///                      调用成功后 *index 会被推进。
  /// @param[out]    m0    primary 通道消息
  /// @param[out]    m1    secondary 通道消息
  /// @return true  成功取到对齐的消息对
  /// @return false 缓冲区为空或消费者已追平
  virtual bool Fusion(uint64_t* index, std::shared_ptr<M0>& m0,
                      std::shared_ptr<M1>& m1) = 0;
};

}  // namespace fusion
}  // namespace data
}  // namespace minicyber

#endif  // MINICYBER_DATA_FUSION_DATA_FUSION_H_