#ifndef MINICYBER_DATA_FUSION_ALL_LATEST_H_
#define MINICYBER_DATA_FUSION_ALL_LATEST_H_

#include <memory>
#include <mutex>
#include <tuple>

#include "minicyber/data/channel_buffer.h"
#include "minicyber/data/fusion/data_fusion.h"

namespace minicyber {
namespace data {
namespace fusion {

template <typename M0, typename M1 = NullType, typename M2 = NullType,
          typename M3 = NullType>
class AllLatest : public DataFusion<M0, M1, M2, M3> {
  using FusionData = std::tuple<std::shared_ptr<M0>, std::shared_ptr<M1>,
                                std::shared_ptr<M2>, std::shared_ptr<M3>>;

 public:
  AllLatest(const ChannelBuffer<M0>& buffer_0,
            const ChannelBuffer<M1>& buffer_1,
            const ChannelBuffer<M2>& buffer_2,
            const ChannelBuffer<M3>& buffer_3)
      : buffer_m0_(buffer_0), buffer_m1_(buffer_1), buffer_m2_(buffer_2),
        buffer_m3_(buffer_3), buffer_fusion_(
            buffer_0.channel_id(),
            std::make_shared<CacheBuffer<std::shared_ptr<FusionData>>>(
                buffer_0.Buffer()->Capacity() - 1)) {
    buffer_m0_.Buffer()->SetFusionCallback([this](const std::shared_ptr<M0>& m0) {
      std::shared_ptr<M1> m1;
      std::shared_ptr<M2> m2;
      std::shared_ptr<M3> m3;
      if (!buffer_m1_.Latest(m1) || !buffer_m2_.Latest(m2) ||
          !buffer_m3_.Latest(m3)) return;
      Fill(m0, m1, m2, m3);
    });
  }

  ~AllLatest() override { buffer_m0_.Buffer()->SetFusionCallback({}); }

  bool Fusion(uint64_t* index, std::shared_ptr<M0>& m0,
              std::shared_ptr<M1>& m1, std::shared_ptr<M2>& m2,
              std::shared_ptr<M3>& m3) override {
    std::shared_ptr<FusionData> data;
    if (!buffer_fusion_.Fetch(index, data)) return false;
    m0 = std::get<0>(*data);
    m1 = std::get<1>(*data);
    m2 = std::get<2>(*data);
    m3 = std::get<3>(*data);
    return true;
  }

 private:
  void Fill(const std::shared_ptr<M0>& m0, const std::shared_ptr<M1>& m1,
            const std::shared_ptr<M2>& m2, const std::shared_ptr<M3>& m3) {
    std::lock_guard<std::mutex> lock(buffer_fusion_.Buffer()->Mutex());
    buffer_fusion_.Buffer()->Fill(
        std::make_shared<FusionData>(m0, m1, m2, m3));
  }

  ChannelBuffer<M0> buffer_m0_;
  ChannelBuffer<M1> buffer_m1_;
  ChannelBuffer<M2> buffer_m2_;
  ChannelBuffer<M3> buffer_m3_;
  ChannelBuffer<FusionData> buffer_fusion_;
};

template <typename M0, typename M1, typename M2>
class AllLatest<M0, M1, M2, NullType> : public DataFusion<M0, M1, M2> {
  using FusionData = std::tuple<std::shared_ptr<M0>, std::shared_ptr<M1>,
                                std::shared_ptr<M2>>;

 public:
  AllLatest(const ChannelBuffer<M0>& buffer_0,
            const ChannelBuffer<M1>& buffer_1,
            const ChannelBuffer<M2>& buffer_2)
      : buffer_m0_(buffer_0), buffer_m1_(buffer_1), buffer_m2_(buffer_2),
        buffer_fusion_(
            buffer_0.channel_id(),
            std::make_shared<CacheBuffer<std::shared_ptr<FusionData>>>(
                buffer_0.Buffer()->Capacity() - 1)) {
    buffer_m0_.Buffer()->SetFusionCallback([this](const std::shared_ptr<M0>& m0) {
      std::shared_ptr<M1> m1;
      std::shared_ptr<M2> m2;
      if (!buffer_m1_.Latest(m1) || !buffer_m2_.Latest(m2)) return;
      std::lock_guard<std::mutex> lock(buffer_fusion_.Buffer()->Mutex());
      buffer_fusion_.Buffer()->Fill(std::make_shared<FusionData>(m0, m1, m2));
    });
  }

  ~AllLatest() override { buffer_m0_.Buffer()->SetFusionCallback({}); }

  bool Fusion(uint64_t* index, std::shared_ptr<M0>& m0,
              std::shared_ptr<M1>& m1, std::shared_ptr<M2>& m2) override {
    std::shared_ptr<FusionData> data;
    if (!buffer_fusion_.Fetch(index, data)) return false;
    m0 = std::get<0>(*data);
    m1 = std::get<1>(*data);
    m2 = std::get<2>(*data);
    return true;
  }

 private:
  ChannelBuffer<M0> buffer_m0_;
  ChannelBuffer<M1> buffer_m1_;
  ChannelBuffer<M2> buffer_m2_;
  ChannelBuffer<FusionData> buffer_fusion_;
};

template <typename M0, typename M1>
class AllLatest<M0, M1, NullType, NullType> : public DataFusion<M0, M1> {
  using FusionData = std::tuple<std::shared_ptr<M0>, std::shared_ptr<M1>>;

 public:
  AllLatest(const ChannelBuffer<M0>& buffer_0,
            const ChannelBuffer<M1>& buffer_1)
      : buffer_m0_(buffer_0), buffer_m1_(buffer_1), buffer_fusion_(
            buffer_0.channel_id(),
            std::make_shared<CacheBuffer<std::shared_ptr<FusionData>>>(
                buffer_0.Buffer()->Capacity() - 1)) {
    buffer_m0_.Buffer()->SetFusionCallback([this](const std::shared_ptr<M0>& m0) {
      std::shared_ptr<M1> m1;
      if (!buffer_m1_.Latest(m1)) return;
      std::lock_guard<std::mutex> lock(buffer_fusion_.Buffer()->Mutex());
      buffer_fusion_.Buffer()->Fill(std::make_shared<FusionData>(m0, m1));
    });
  }

  ~AllLatest() override { buffer_m0_.Buffer()->SetFusionCallback({}); }

  bool Fusion(uint64_t* index, std::shared_ptr<M0>& m0,
              std::shared_ptr<M1>& m1) override {
    std::shared_ptr<FusionData> data;
    if (!buffer_fusion_.Fetch(index, data)) return false;
    m0 = std::get<0>(*data);
    m1 = std::get<1>(*data);
    return true;
  }

 private:
  ChannelBuffer<M0> buffer_m0_;
  ChannelBuffer<M1> buffer_m1_;
  ChannelBuffer<FusionData> buffer_fusion_;
};

}  // namespace fusion
}  // namespace data
}  // namespace minicyber

#endif  // MINICYBER_DATA_FUSION_ALL_LATEST_H_
