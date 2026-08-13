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

#include <sys/wait.h>
#include <unistd.h>

#include "minicyber/node/node.h"
#include "minicyber/transport/receiver/shm_receiver.h"
#include "minicyber/transport/transmitter/shm_transmitter.h"

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

Result RunShm(const Options& options) {
  int producer_ready[2];
  int consumer_ready[2];
  int producer_start[2];
  int acknowledgements[2];
  int results[2];
  int elapsed[2];
  if (::pipe(producer_ready) != 0 || ::pipe(consumer_ready) != 0 ||
      ::pipe(producer_start) != 0 || ::pipe(acknowledgements) != 0 ||
      ::pipe(results) != 0 || ::pipe(elapsed) != 0) {
    std::exit(1);
  }

  const uint64_t channel_id =
      (static_cast<uint64_t>(::getpid()) << 32) ^ NowNs();
  const size_t frame_size = sizeof(Sample) + options.payload_bytes;
  const uint64_t ceiling_msg_size = static_cast<uint64_t>(frame_size);
  pid_t producer = ::fork();
  if (producer < 0) std::exit(1);
  if (producer == 0) {
    ::close(producer_ready[0]);
    ::close(consumer_ready[0]);
    ::close(consumer_ready[1]);
    ::close(producer_start[1]);
    ::close(acknowledgements[1]);
    ::close(results[0]);
    ::close(results[1]);
    ::close(elapsed[0]);

    minicyber::transport::ShmTransmitter transmitter(channel_id,
                                                       ceiling_msg_size);
    transmitter.Enable();
    const char ready = transmitter.enabled() ? 'r' : 'e';
    if (!WriteAll(producer_ready[1], &ready, sizeof(ready)) || ready != 'r') {
      _exit(1);
    }
    char start;
    if (!ReadAll(producer_start[0], &start, sizeof(start))) _exit(1);

    auto frame = std::make_shared<std::string>(frame_size, 's');
    uint64_t start_ns = 0;
    for (size_t index = 0; index < options.warmup + options.samples; ++index) {
      if (index == options.warmup) start_ns = NowNs();
      const Sample sample{NowNs(), index};
      std::memcpy(frame->data(), &sample, sizeof(sample));
      if (!transmitter.Transmit(frame)) _exit(1);
      char acknowledgement;
      if (!ReadAll(acknowledgements[0], &acknowledgement,
                   sizeof(acknowledgement))) {
        _exit(1);
      }
    }
    const uint64_t elapsed_ns = NowNs() - start_ns;
    transmitter.Disable();
    if (!WriteAll(elapsed[1], &elapsed_ns, sizeof(elapsed_ns))) _exit(1);
    _exit(0);
  }

  ::close(producer_ready[1]);
  char ready;
  if (!ReadAll(producer_ready[0], &ready, sizeof(ready)) || ready != 'r') {
    std::exit(1);
  }

  pid_t consumer = ::fork();
  if (consumer < 0) std::exit(1);
  if (consumer == 0) {
    ::close(producer_ready[0]);
    ::close(consumer_ready[0]);
    ::close(producer_start[0]);
    ::close(producer_start[1]);
    ::close(acknowledgements[0]);
    ::close(results[0]);
    ::close(elapsed[0]);
    ::close(elapsed[1]);

    std::vector<uint64_t> latencies;
    latencies.reserve(options.samples);
    minicyber::transport::ShmReceiver receiver(
        channel_id, [&](const std::shared_ptr<std::string>& frame) {
          Sample sample;
          std::memcpy(&sample, frame->data(), sizeof(sample));
          if (sample.sequence >= options.warmup) {
            latencies.push_back(NowNs() - sample.sent_ns);
          }
          const char acknowledgement = 'a';
          WriteAll(acknowledgements[1], &acknowledgement,
                   sizeof(acknowledgement));
        });
    receiver.Enable();
    const char receiver_ready = receiver.enabled() ? 'r' : 'e';
    if (!WriteAll(consumer_ready[1], &receiver_ready, sizeof(receiver_ready)) ||
        receiver_ready != 'r') {
      _exit(1);
    }

    while (latencies.size() < options.samples) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    receiver.Disable();
    if (!WriteAll(results[1], latencies.data(),
                  latencies.size() * sizeof(latencies.front()))) {
      _exit(1);
    }
    _exit(0);
  }

  ::close(consumer_ready[1]);
  ::close(producer_start[0]);
  ::close(acknowledgements[0]);
  ::close(acknowledgements[1]);
  ::close(results[1]);
  ::close(elapsed[1]);
  if (!ReadAll(consumer_ready[0], &ready, sizeof(ready)) || ready != 'r') {
    std::exit(1);
  }
  const char start = 's';
  if (!WriteAll(producer_start[1], &start, sizeof(start))) std::exit(1);

  std::vector<uint64_t> latencies(options.samples);
  if (!ReadAll(results[0], latencies.data(),
               latencies.size() * sizeof(latencies.front()))) {
    std::exit(1);
  }
  uint64_t elapsed_ns = 0;
  if (!ReadAll(elapsed[0], &elapsed_ns, sizeof(elapsed_ns))) std::exit(1);
  int producer_status = 0;
  int consumer_status = 0;
  ::waitpid(producer, &producer_status, 0);
  ::waitpid(consumer, &consumer_status, 0);
  if (!WIFEXITED(producer_status) || WEXITSTATUS(producer_status) != 0 ||
      !WIFEXITED(consumer_status) || WEXITSTATUS(consumer_status) != 0) {
    std::exit(1);
  }
  return {"shm", std::move(latencies), elapsed_ns};
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
  PrintResult(RunShm(options), options);
  return 0;
}
