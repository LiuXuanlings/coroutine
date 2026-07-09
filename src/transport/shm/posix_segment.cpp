#include "minicyber/transport/shm/posix_segment.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <new>

namespace minicyber {
namespace transport {

PosixSegment::PosixSegment(uint64_t channel_id, uint64_t ceiling_msg_size,
                           uint32_t block_num)
    : Segment(channel_id),
      ceiling_msg_size_(ceiling_msg_size),
      block_num_(block_num) {
  shm_name_ = "minicyber_" + std::to_string(channel_id);
  total_size_ = TotalSize();
}

PosixSegment::~PosixSegment() { Destroy(); }

size_t PosixSegment::TotalSize() const {
  // [State][Block[block_num]][block_buf_size * block_num]
  return sizeof(State) + sizeof(Block) * block_num_ +
         ceiling_msg_size_ * block_num_;
}

bool PosixSegment::Open() {
  if (opened_) return true;
  return OpenOrCreate();
}

bool PosixSegment::OpenOrCreate() {
  // 尝试独占创建
  // 创建/打开POSIX命名共享内存
  // 参数1：共享内存名称，string转C风格字符串
  // 参数2：打开标志组合
  //      O_RDWR：可读可写模式
  //      O_CREAT：共享内存不存在则创建
  //      O_EXCL：配合O_CREAT使用，若共享内存已存在则直接返回失败，避免重复创建冲突
  // 参数3：新建共享内存时的权限0644(八进制)，所有者读写，同组用户/其他用户只读，仅创建时生效
  // 返回值：成功返回共享内存文件描述符fd；失败返回-1，可通过errno判断错误类型
  int fd = shm_open(shm_name_.c_str(), O_RDWR | O_CREAT | O_EXCL, 0644);
  if (fd < 0) {
    if (errno == EEXIST) {
      return OpenOnly();
    }
    return false;
  }

  // 设置段大小
  // 说明：shm_open 仅创建一个空的共享内存内核对象，初始大小为 0；
  // 必须通过 ftruncate 主动扩容，分配对应大小的物理内存页，后续 mmap 才能获得合法可读写的地址空间。
  if (ftruncate(fd, total_size_) < 0) {
    close(fd);
    shm_unlink(shm_name_.c_str());
    return false;
  }

  // 映射
  mem_ = mmap(nullptr, total_size_, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  close(fd);
  if (mem_ == MAP_FAILED) {
    mem_ = nullptr;
    shm_unlink(shm_name_.c_str());
    return false;
  }

  // 放置构造 State
  state_ = new (mem_) State(ceiling_msg_size_);
  if (state_ == nullptr) {
    munmap(mem_, total_size_);
    mem_ = nullptr;
    shm_unlink(shm_name_.c_str());
    return false;
  }

  // 放置构造 Block 数组
  blocks_ = reinterpret_cast<Block*>(static_cast<char*>(mem_) + sizeof(State));
  for (uint32_t i = 0; i < block_num_; ++i) {
    new (&blocks_[i]) Block();
  }

  state_->IncreaseReferenceCounts();
  opened_ = true;
  return true;
}

bool PosixSegment::OpenOnly() {
  int fd = shm_open(shm_name_.c_str(), O_RDWR, 0644);
  if (fd < 0) {
    return false;
  }

  // 读取已有段大小
  struct stat file_attr;
  if (fstat(fd, &file_attr) < 0) {
    close(fd);
    return false;
  }
  total_size_ = static_cast<size_t>(file_attr.st_size);

  mem_ = mmap(nullptr, total_size_, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  close(fd);
  if (mem_ == MAP_FAILED) {
    mem_ = nullptr;
    return false;
  }

  // 复用已有 State
  state_ = reinterpret_cast<State*>(mem_);
  if (state_ == nullptr) {
    munmap(mem_, total_size_);
    mem_ = nullptr;
    return false;
  }

  // 复用已有 Block 数组
  blocks_ = reinterpret_cast<Block*>(static_cast<char*>(mem_) + sizeof(State));

  // 同步 ceiling_msg_size / block_num 从已有 State
  ceiling_msg_size_ = state_->ceiling_msg_size();
  // block_num_ 从段大小反推：block_num = (total - sizeof(State)) /
  // (sizeof(Block) + ceiling_msg_size)
  if (ceiling_msg_size_ > 0) {
    block_num_ = static_cast<uint32_t>(
        (total_size_ - sizeof(State)) / (sizeof(Block) + ceiling_msg_size_));
  }

  state_->IncreaseReferenceCounts();
  opened_ = true;
  return true;
}

void PosixSegment::Close() {
  if (!opened_) return;
  if (state_ != nullptr) {
    state_->DecreaseReferenceCounts();
  }
  if (mem_ != nullptr) {
    munmap(mem_, total_size_);
    mem_ = nullptr;
  }
  state_ = nullptr;
  blocks_ = nullptr;
  opened_ = false;
}

void PosixSegment::Destroy() {
  Close();
  if (!shm_name_.empty()) {
    shm_unlink(shm_name_.c_str());
  }
}

void* PosixSegment::GetMemPtr() { return mem_; }

size_t PosixSegment::GetSize() { return total_size_; }

}  // namespace transport
}  // namespace minicyber