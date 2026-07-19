// =============================================================================
// benchmark_pingpong：MiniCyber INTRA vs POSIX PIPE 延迟对比
//
// 对比项：
//   INTRA: DataDispatcher + DataNotifier（框架内部零拷贝数据分发）
//   PIPE:  posix pipe() + 双线程（传统 IPC 基线）
//
// 输出：平均/最小/最大延迟（微秒）及每秒消息数
//
// 延迟模型：
//   INTRA 测量 one-way: Writer::Write 同步返回（callback 在 Dispatch 内联执行）
//   PIPE   测量 round-trip/2: 主线程 write → 读线程 echo → 主线程 read
//
// 编译（自动被 CMakeLists.txt examples glob 拾取）：
//   cmake .. && make benchmark_pingpong -j
//
// 运行：
//   ./benchmark_pingpong
// =============================================================================

#include <pthread.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

#include "minicyber/node/node.h"
#include "minicyber/node/reader.h"
#include "minicyber/node/writer.h"
#include "minicyber/time/time.h"

using minicyber::Duration;
using minicyber::Time;

// =============================================================================
// 配置
// =============================================================================
constexpr int kIterations = 100000;
constexpr int kWarmup     = 2000;

// =============================================================================
// 工具：打印统计行
// =============================================================================
struct Stats {
  const char* label;
  uint64_t total_ns;
  uint64_t min_ns;
  uint64_t max_ns;
  int count;
  const char* note;  // e.g. "one-way" or "round-trip/2"
};

static void PrintStats(const Stats& s) {
  double avg_us = static_cast<double>(s.total_ns) / s.count / 1000.0;
  double min_us = static_cast<double>(s.min_ns) / 1000.0;
  double max_us = static_cast<double>(s.max_ns) / 1000.0;
  double msgs_per_sec =
      static_cast<double>(s.count) / (static_cast<double>(s.total_ns) / 1e9);

  printf("  %-5s  (%s)\n", s.label, s.note);
  printf("    Avg: %8.2f us\n", avg_us);
  printf("    Min: %8.2f us\n", min_us);
  printf("    Max: %8.2f us\n", max_us);
  printf("    Msg/s: %10.0f\n", msgs_per_sec);
}

// =============================================================================
// INTRA 基准测试
// =============================================================================
static void RunIntraBenchmark(const std::string& payload) {
  // Node + Writer + Reader on same channel -> Transport picks INTRA
  minicyber::node::Node node("bench_intra");
  auto writer = node.CreateWriter<std::string>("/intra_bench");
  std::atomic<int> cb_count{0};

  auto reader = node.CreateReader<std::string>(
      "/intra_bench", [&](const std::shared_ptr<std::string>& /*msg*/) {
        cb_count.fetch_add(1, std::memory_order_relaxed);
      });

  if (!writer || !reader) {
    std::fprintf(stderr, "[INTRA] failed to create writer/reader\n");
    std::exit(1);
  }

  // --- warmup ---
  auto msg = std::make_shared<std::string>(payload);
  for (int i = 0; i < kWarmup; ++i) {
    writer->Write(msg);
  }
  cb_count.store(0, std::memory_order_relaxed);

  // --- measure ---
  uint64_t total_ns = 0;
  uint64_t min_ns = UINT64_MAX;
  uint64_t max_ns = 0;

  for (int i = 0; i < kIterations; ++i) {
    auto t1 = Time::MonoTime();
    writer->Write(msg);
    auto t2 = Time::MonoTime();
    uint64_t ns = (t2 - t1).ToNanosecond();
    total_ns += ns;
    if (ns < min_ns) min_ns = ns;
    if (ns > max_ns) max_ns = ns;
  }

  PrintStats({"INTRA", total_ns, min_ns, max_ns, kIterations, "one-way"});
}

// =============================================================================
// PIPE 基准测试（传统 IPC 基线）
// =============================================================================
static void RunPipeBenchmark(const std::string& /*payload*/) {
  int to_child[2];   // main -> child
  int to_parent[2];  // child -> main
  if (pipe(to_child) != 0 || pipe(to_parent) != 0) {
    std::fprintf(stderr, "[PIPE] pipe() failed\n");
    std::exit(1);
  }

  std::atomic<bool> running{true};

  // Reader thread: echo one byte back
  std::thread reader([&]() {
    char ch;
    while (running.load(std::memory_order_acquire)) {
      ssize_t n = read(to_child[0], &ch, 1);
      if (n <= 0) break;
      write(to_parent[1], &ch, 1);
    }
  });

  // --- warmup ---
  char ch = 'x';
  for (int i = 0; i < kWarmup; ++i) {
    write(to_child[1], &ch, 1);
    read(to_parent[0], &ch, 1);
  }

  // --- measure (round-trip, divide by 2 for one-way) ---
  int half = kIterations / 2;
  uint64_t total_ns = 0;
  uint64_t min_ns = UINT64_MAX;
  uint64_t max_ns = 0;

  for (int i = 0; i < half; ++i) {
    auto t1 = Time::MonoTime();
    write(to_child[1], &ch, 1);
    read(to_parent[0], &ch, 1);
    auto t2 = Time::MonoTime();
    uint64_t ns = (t2 - t1).ToNanosecond();
    total_ns += ns;
    if (ns < min_ns) min_ns = ns;
    if (ns > max_ns) max_ns = ns;
  }

  // Cleanup
  running.store(false, std::memory_order_release);
  write(to_child[1], &ch, 1);  // wake reader so it can exit
  reader.join();
  close(to_child[0]);
  close(to_child[1]);
  close(to_parent[0]);
  close(to_parent[1]);

  // Round-trip / 2 for one-way
  PrintStats({"PIPE", total_ns / 2, min_ns / 2, max_ns / 2, half * 2,
              "one-way (rtt/2)"});
}

// =============================================================================
// 主函数
// =============================================================================
int main() {
  const std::string payload(64, 'a');

  printf("=== MiniCyber Benchmark: INTRA vs PIPE ===\n");
  printf("Payload: %zu bytes  |  Iterations: %d\n\n", payload.size(),
         kIterations);

  RunIntraBenchmark(payload);
  printf("\n");
  RunPipeBenchmark(payload);

  return 0;
}
