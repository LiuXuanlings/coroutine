// SPSC one-way latency baseline for MiniCyber INTRA and POSIX pipe.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <unistd.h>

#include "minicyber/node/node.h"

namespace {

struct Options {
  size_t payload_bytes = 64;
  size_t warmup = 2000;
  size_t samples = 100000;
};

struct Sample {
  uint64_t sent_ns;
  uint64_t sequence;
};

struct Result {
  const char* backend;
  std::vector<uint64_t> latencies_ns;
  uint64_t elapsed_ns;
};

uint64_t NowNs() {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

bool WriteAll(int fd, const void* data, size_t size) {
  const auto* cursor = static_cast<const char*>(data);
  while (size > 0) {
    const ssize_t written = ::write(fd, cursor, size);
    if (written <= 0) return false;
    cursor += written;
    size -= static_cast<size_t>(written);
  }
  return true;
}

bool ReadAll(int fd, void* data, size_t size) {
  auto* cursor = static_cast<char*>(data);
  while (size > 0) {
    const ssize_t read_size = ::read(fd, cursor, size);
    if (read_size <= 0) return false;
    cursor += read_size;
    size -= static_cast<size_t>(read_size);
  }
  return true;
}

Result RunIntra(const Options& options) {
  minicyber::node::Node subscriber("benchmark_intra_subscriber");
  std::vector<uint64_t> latencies;
  latencies.reserve(options.samples);
  bool measuring = false;

  auto reader = subscriber.CreateReader<std::string>(
      "/benchmark/intra", [&](const std::shared_ptr<std::string>& frame) {
        Sample sample;
        std::memcpy(&sample, frame->data(), sizeof(sample));
        if (measuring) latencies.push_back(NowNs() - sample.sent_ns);
      });
  minicyber::node::Node publisher("benchmark_intra_publisher");
  auto writer = publisher.CreateWriter<std::string>("/benchmark/intra");
  if (!reader || !writer) std::exit(1);

  auto frame = std::make_shared<std::string>(sizeof(Sample) + options.payload_bytes,
                                             'i');
  for (size_t index = 0; index < options.warmup; ++index) {
    const Sample sample{NowNs(), index};
    std::memcpy(frame->data(), &sample, sizeof(sample));
    if (!writer->Write(frame)) std::exit(1);
  }

  measuring = true;
  const uint64_t start_ns = NowNs();
  for (size_t index = 0; index < options.samples; ++index) {
    const Sample sample{NowNs(), index};
    std::memcpy(frame->data(), &sample, sizeof(sample));
    if (!writer->Write(frame)) std::exit(1);
  }
  const uint64_t elapsed_ns = NowNs() - start_ns;
  measuring = false;
  return {"intra", std::move(latencies), elapsed_ns};
}

Result RunPipe(const Options& options) {
  int fds[2];
  int acknowledgements[2];
  if (::pipe(fds) != 0 || ::pipe(acknowledgements) != 0) std::exit(1);

  std::vector<uint64_t> latencies;
  latencies.reserve(options.samples);
  std::atomic<bool> consumer_ok{true};
  const size_t frame_size = sizeof(Sample) + options.payload_bytes;
  std::thread consumer([&] {
    std::vector<char> frame(frame_size);
    for (size_t index = 0; index < options.warmup + options.samples; ++index) {
      if (!ReadAll(fds[0], frame.data(), frame.size())) {
        consumer_ok.store(false);
        return;
      }
      if (index >= options.warmup) {
        Sample sample;
        std::memcpy(&sample, frame.data(), sizeof(sample));
        latencies.push_back(NowNs() - sample.sent_ns);
      }
      const char acknowledgement = 'a';
      if (!WriteAll(acknowledgements[1], &acknowledgement,
                    sizeof(acknowledgement))) {
        consumer_ok.store(false);
        return;
      }
    }
  });

  std::vector<char> frame(frame_size, 'p');
  uint64_t start_ns = 0;
  for (size_t index = 0; index < options.warmup + options.samples; ++index) {
    if (index == options.warmup) start_ns = NowNs();
    Sample sample{NowNs(), index};
    std::memcpy(frame.data(), &sample, sizeof(sample));
    if (!WriteAll(fds[1], frame.data(), frame.size())) std::exit(1);
    char acknowledgement;
    if (!ReadAll(acknowledgements[0], &acknowledgement,
                 sizeof(acknowledgement))) {
      std::exit(1);
    }
  }
  const uint64_t elapsed_ns = NowNs() - start_ns;
  consumer.join();
  ::close(fds[0]);
  ::close(fds[1]);
  ::close(acknowledgements[0]);
  ::close(acknowledgements[1]);
  if (!consumer_ok.load()) std::exit(1);
  return {"pipe", std::move(latencies), elapsed_ns};
}

void PrintResult(const Result& result, const Options& options) {
  auto values = result.latencies_ns;
  std::sort(values.begin(), values.end());
  uint64_t total = 0;
  for (uint64_t value : values) total += value;
  const auto percentile = [&values](size_t numerator) {
    return values[(values.size() - 1) * numerator / 100];
  };
  const double throughput =
      static_cast<double>(values.size()) * 1e9 /
      static_cast<double>(result.elapsed_ns);
  std::cout << result.backend << ",spsc_one_way," << options.payload_bytes
            << ',' << options.warmup << ',' << values.size() << ',' << values.front()
            << ',' << percentile(50) << ',' << percentile(95) << ','
            << percentile(99) << ',' << values.back() << ',' << total << ','
            << throughput << '\n';
}

Options ParseOptions(int argc, char** argv) {
  Options options;
  for (int index = 1; index < argc; ++index) {
    const std::string arg(argv[index]);
    if (arg == "--payload-bytes" && index + 1 < argc) {
      options.payload_bytes = std::strtoull(argv[++index], nullptr, 10);
    } else if (arg == "--warmup" && index + 1 < argc) {
      options.warmup = std::strtoull(argv[++index], nullptr, 10);
    } else if (arg == "--samples" && index + 1 < argc) {
      options.samples = std::strtoull(argv[++index], nullptr, 10);
    } else {
      std::cerr << "Usage: " << argv[0]
                << " [--payload-bytes N] [--warmup N] [--samples N]\n";
      std::exit(2);
    }
  }
  if (options.payload_bytes == 0 || options.samples == 0) std::exit(2);
  return options;
}

}  // namespace

int main(int argc, char** argv) {
  const Options options = ParseOptions(argc, argv);
  std::cout << "backend,model,payload_bytes,warmup,samples,min_ns,p50_ns,p95_ns,"
               "p99_ns,max_ns,total_ns,throughput_msg_s\n";
  PrintResult(RunIntra(options), options);
  PrintResult(RunPipe(options), options);
  return 0;
}
