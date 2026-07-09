#include <gtest/gtest.h>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>
#include "minicyber/transport/shm/block.h"
#include "minicyber/transport/shm/segment.h"
#include "minicyber/transport/shm/state.h"

using minicyber::transport::Block;
using minicyber::transport::Segment;
using minicyber::transport::SegmentPtr;
using minicyber::transport::State;
using minicyber::transport::WritableBlock;

// =============================================================================
// MockSegment：用进程内 malloc 的内存模拟共享内存段，验证 Segment 契约
// =============================================================================

namespace minicyber::transport {

class MockSegment : public Segment {
 public:
  explicit MockSegment(uint64_t channel_id, size_t size)
      : Segment(channel_id), size_(size) {}
  ~MockSegment() override { Close(); }

  bool Open() override {
    if (opened_) return true;
    mem_ = std::malloc(size_);
    if (!mem_) return false;
    std::memset(mem_, 0, size_);
    opened_ = true;
    return true;
  }

  void Close() override {
    if (!opened_) return;
    std::free(mem_);
    mem_ = nullptr;
    opened_ = false;
  }

  void Destroy() override { Close(); }

  void* GetMemPtr() override { return mem_; }
  size_t GetSize() override { return size_; }

 private:
  size_t size_;
  void* mem_ = nullptr;
  bool opened_ = false;
};

}  // namespace minicyber::transport
using minicyber::transport::MockSegment;

// channel_id 在构造时记录
TEST(ShmSegmentTest, ChannelIdInit) {
  MockSegment seg(42, 4096);
  EXPECT_EQ(seg.channel_id(), 42u);
}

// Open 后 GetMemPtr 非空，GetSize 等于构造大小
TEST(ShmSegmentTest, OpenProvidesMemPtrAndSize) {
  MockSegment seg(1, 4096);
  ASSERT_TRUE(seg.Open());
  EXPECT_NE(seg.GetMemPtr(), nullptr);
  EXPECT_EQ(seg.GetSize(), 4096u);
}

// 未 Open 时 GetMemPtr 为空
TEST(ShmSegmentTest, NullMemPtrBeforeOpen) {
  MockSegment seg(1, 4096);
  EXPECT_EQ(seg.GetMemPtr(), nullptr);
}

// Open 后内存被清零
TEST(ShmSegmentTest, OpenedMemoryIsZero) {
  MockSegment seg(1, 256);
  ASSERT_TRUE(seg.Open());
  auto* p = static_cast<uint8_t*>(seg.GetMemPtr());
  for (size_t i = 0; i < seg.GetSize(); ++i) {
    EXPECT_EQ(p[i], 0);
  }
}

// Open 幂等：重复 Open 不重新分配
TEST(ShmSegmentTest, OpenIsIdempotent) {
  MockSegment seg(1, 256);
  ASSERT_TRUE(seg.Open());
  void* first = seg.GetMemPtr();
  ASSERT_TRUE(seg.Open());
  EXPECT_EQ(seg.GetMemPtr(), first);
}

// Close 后 GetMemPtr 失效
TEST(ShmSegmentTest, CloseInvalidatesMemPtr) {
  MockSegment seg(1, 256);
  ASSERT_TRUE(seg.Open());
  EXPECT_NE(seg.GetMemPtr(), nullptr);
  seg.Close();
  EXPECT_EQ(seg.GetMemPtr(), nullptr);
}

// Close 幂等：重复 Close 安全
TEST(ShmSegmentTest, CloseIsIdempotent) {
  MockSegment seg(1, 256);
  ASSERT_TRUE(seg.Open());
  seg.Close();
  seg.Close();
  SUCCEED();
}

// Close 后可重新 Open
TEST(ShmSegmentTest, ReopenAfterClose) {
  MockSegment seg(1, 256);
  ASSERT_TRUE(seg.Open());
  void* first = seg.GetMemPtr();
  ASSERT_NE(first, nullptr);
  seg.Close();
  ASSERT_TRUE(seg.Open());
  EXPECT_NE(seg.GetMemPtr(), nullptr);
  EXPECT_NE(seg.GetMemPtr(), first);
}

// Destroy 等价于 Close
TEST(ShmSegmentTest, DestroyReleasesMemory) {
  MockSegment seg(1, 256);
  ASSERT_TRUE(seg.Open());
  seg.Destroy();
  EXPECT_EQ(seg.GetMemPtr(), nullptr);
}

// 写入 MockSegment 的内存可被读回
TEST(ShmSegmentTest, WriteReadThroughMemPtr) {
  MockSegment seg(1, 1024);
  ASSERT_TRUE(seg.Open());
  auto* p = static_cast<uint8_t*>(seg.GetMemPtr());
  for (size_t i = 0; i < 1024; ++i) p[i] = static_cast<uint8_t>(i & 0xff);
  for (size_t i = 0; i < 1024; ++i) EXPECT_EQ(p[i], static_cast<uint8_t>(i & 0xff));
}

// WritableBlock 默认值
TEST(ShmSegmentTest, WritableBlockDefaults) {
  WritableBlock wb;
  EXPECT_EQ(wb.index, 0u);
  EXPECT_EQ(wb.block, nullptr);
  EXPECT_EQ(wb.buf, nullptr);
}

// 多个 MockSegment 互不干扰
TEST(ShmSegmentTest, IndependentSegments) {
  MockSegment a(1, 128);
  MockSegment b(2, 256);
  ASSERT_TRUE(a.Open());
  ASSERT_TRUE(b.Open());
  auto* pa = static_cast<uint8_t*>(a.GetMemPtr());
  auto* pb = static_cast<uint8_t*>(b.GetMemPtr());
  pa[0] = 0xAA;
  pb[0] = 0xBB;
  EXPECT_EQ(pa[0], 0xAA);
  EXPECT_EQ(pb[0], 0xBB);
  EXPECT_NE(a.GetMemPtr(), b.GetMemPtr());
  EXPECT_EQ(a.GetSize(), 128u);
  EXPECT_EQ(b.GetSize(), 256u);
}

// SegmentPtr (shared_ptr) 可正常管理生命周期
TEST(ShmSegmentTest, SharedPtrLifecycle) {
  SegmentPtr seg = std::make_shared<MockSegment>(1, 64);
  ASSERT_TRUE(seg->Open());
  EXPECT_NE(seg->GetMemPtr(), nullptr);
  seg.reset();
  SUCCEED();
}