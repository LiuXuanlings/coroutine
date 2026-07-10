#include "minicyber/transport/shm/posix_segment.h"

#include <fcntl.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstring>
#include <mutex>
#include <new>

namespace minicyber {
namespace transport {

// =============================================================================
// 进程内 SHM 名字注册表 + 信号处理器
//
// 设计要点（async-signal-safety）：
//   - 信号处理器中只能调用 async-signal-safe 函数（shm_unlink、write、
//     signal、raise 等），不能调用 malloc/new、std::string、mutex 等。
//   - 因此注册表使用固定容量数组 + 原子计数，名字以 C 字符串存储。
//     注册/注销走 std::mutex（正常流程，非信号路径），但信号路径只读。
//   - 安装处理器使用 std::call_once 保证只装一次。
// =============================================================================

namespace {
constexpr size_t kMaxShmNames = 64;
constexpr size_t kMaxNameLen = 64;  // "minicyber_" + 20 位数字 + 余量

struct ShmNameRegistry {
  char names[kMaxShmNames][kMaxNameLen];
  std::atomic<int> count{0};
  std::mutex mutex;
};

ShmNameRegistry& Registry() {
  static ShmNameRegistry r;
  return r;
}

std::atomic<bool>& HandlerInstalled() {
  static std::atomic<bool> v{false};
  return v;
}

void CrashHandler(int sig) {
  ShmNameRegistry& r = Registry();
  int n = r.count.load(std::memory_order_relaxed);
  for (int i = 0; i < n; ++i) {
    // shm_unlink 是 async-signal-safe（POSIX.1-2008）
    ::shm_unlink(r.names[i]);
  }
  // 恢复默认处置并重新抛出，便于外层观察退出码
  ::signal(sig, SIG_DFL);
  ::raise(sig);
}

bool InstallOnce() {
  if (HandlerInstalled().load(std::memory_order_acquire)) return false;
  static std::once_flag once;
  bool first = false;
  std::call_once(once, [&] {
    struct sigaction sa;
    std::memset(&sa, 0, sizeof(sa));
    sa.sa_handler = CrashHandler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    ::sigaction(SIGINT, &sa, nullptr);
    ::sigaction(SIGTERM, &sa, nullptr);
    ::sigaction(SIGSEGV, &sa, nullptr);
    HandlerInstalled().store(true, std::memory_order_release);
    first = true;
  });
  return first;
}
}  // namespace

void PosixSegment::RegisterShmName(const std::string& name) {
  ShmNameRegistry& r = Registry();
  std::lock_guard<std::mutex> lg(r.mutex);
  int n = r.count.load(std::memory_order_relaxed);
  for (int i = 0; i < n; ++i) {
    if (std::strncmp(r.names[i], name.c_str(), kMaxNameLen) == 0) return;
  }
  if (n >= static_cast<int>(kMaxShmNames)) return;
  std::strncpy(r.names[n], name.c_str(), kMaxNameLen - 1);
  r.names[n][kMaxNameLen - 1] = '\0';
  r.count.store(n + 1, std::memory_order_release);
}

void PosixSegment::UnregisterShmName(const std::string& name) {
  ShmNameRegistry& r = Registry();
  std::lock_guard<std::mutex> lg(r.mutex);
  int n = r.count.load(std::memory_order_relaxed);
  for (int i = 0; i < n; ++i) {
    if (std::strncmp(r.names[i], name.c_str(), kMaxNameLen) == 0) {
      // 用最后一个填当前位置，count--
      std::memmove(r.names[i], r.names[n - 1], kMaxNameLen);
      r.count.store(n - 1, std::memory_order_release);
      return;
    }
  }
}

bool PosixSegment::InstallSignalHandler() { return InstallOnce(); }

std::vector<std::string> PosixSegment::RegisteredShmNames() {
  ShmNameRegistry& r = Registry();
  std::lock_guard<std::mutex> lg(r.mutex);
  std::vector<std::string> out;
  int n = r.count.load(std::memory_order_relaxed);
  out.reserve(n);
  for (int i = 0; i < n; ++i) out.emplace_back(r.names[i]);
  return out;
}

void PosixSegment::ClearRegistryForTest() {
  ShmNameRegistry& r = Registry();
  std::lock_guard<std::mutex> lg(r.mutex);
  r.count.store(0, std::memory_order_release);
}

int PosixSegment::CleanupAllForTest() {
  ShmNameRegistry& r = Registry();
  std::lock_guard<std::mutex> lg(r.mutex);
  int n = r.count.load(std::memory_order_relaxed);
  int unlinked = 0;
  for (int i = 0; i < n; ++i) {
    if (::shm_unlink(r.names[i]) == 0) ++unlinked;
  }
  r.count.store(0, std::memory_order_release);
  return unlinked;
}

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
  // 本进程是创建者：注册名字并安装信号处理器
  InstallSignalHandler();
  RegisterShmName(shm_name_);
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
    UnregisterShmName(shm_name_);
    shm_unlink(shm_name_.c_str());
  }
}

void* PosixSegment::GetMemPtr() { return mem_; }

size_t PosixSegment::GetSize() { return total_size_; }

// 计算第 index 个 block 的 payload 起始地址
uint8_t* PosixSegment::BlockBufAddr(uint32_t index) {
  if (mem_ == nullptr || blocks_ == nullptr) return nullptr;
  char* base = static_cast<char*>(mem_);
  return reinterpret_cast<uint8_t*>(base + sizeof(State) +
                                    block_num_ * sizeof(Block) +
                                    index * ceiling_msg_size_);
}

bool PosixSegment::AcquireBlockToWrite(size_t msg_size, ShmWritableBlock* wb) {
  if (!opened_ || wb == nullptr || msg_size > ceiling_msg_size_) return false;

  // 简单策略：用 State 的 seq 取模决定下一个写块索引
  // 这样多个写者通过原子 seq 自然错开；同进程内单写者时直接递增。
  uint32_t index = 0;
  if (state_ != nullptr) {
    index = state_->FetchAddSeq(1) % block_num_;
  }
  Block* blk = &blocks_[index];
  if (!blk->TryLockForWrite()) return false;
  wb->index = index;
  wb->block = blk;
  wb->buf = BlockBufAddr(index);
  return true;
}

void PosixSegment::ReleaseWrittenBlock(const ShmWritableBlock& wb) {
  if (wb.block == nullptr) return;
  wb.block->ReleaseWriteLock();
}

bool PosixSegment::AcquireBlockToRead(uint32_t index, ShmReadableBlock* rb) {
  if (!opened_ || rb == nullptr || index >= block_num_) return false;
  Block* blk = &blocks_[index];
  if (!blk->TryLockForRead()) return false;
  rb->index = index;
  rb->block = blk;
  rb->buf = BlockBufAddr(index);
  return true;
}

void PosixSegment::ReleaseReadBlock(const ShmReadableBlock& rb) {
  if (rb.block == nullptr) return;
  rb.block->ReleaseReadLock();
}

}  // namespace transport
}  // namespace minicyber