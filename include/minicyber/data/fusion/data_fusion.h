#ifndef MINICYBER_DATA_FUSION_DATA_FUSION_H_
#define MINICYBER_DATA_FUSION_DATA_FUSION_H_

#include <cstdint>
#include <memory>

namespace minicyber {
namespace data {
namespace fusion {

// DataFusion 是双通道访问的最小抽象。本项目只保留唯一主干需要的
// AllLatest，不再保留三/四输入策略或把多输入接口伪装成通用能力。
template <typename M0, typename M1>
class DataFusion {
 public:
  virtual ~DataFusion() = default;

  // index 是访问器唯一拥有的绝对游标；成功读取后由访问器恰好前进一次，
  // 防止融合队列跳过消息。CyberRT 对应 data::fusion::DataFusion::Fusion。
  virtual bool Fusion(uint64_t* index, std::shared_ptr<M0>& m0,
                      std::shared_ptr<M1>& m1) = 0;
};

}  // namespace fusion
}  // namespace data
}  // namespace minicyber

#endif  // MINICYBER_DATA_FUSION_DATA_FUSION_H_
