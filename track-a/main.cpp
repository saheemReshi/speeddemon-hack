#include <algorithm>
#include <array>
#include <charconv>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <immintrin.h>
#include <mutex>
#include <numeric>
#include <string>
#include <string_view>
#include <sys/mman.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>
#ifndef WORKERS
#define WORKERS 8
#endif

constexpr unsigned Hours = 72, Endpoints = 1842;
struct Dictionary {
  // The spec caps the catalogue at 1,842 keys. Collisions still compare bytes.
  std::array<uint16_t, 4096> slots{};
  std::vector<std::string_view> keys;
  // Addresses stay stable for the worker lifetime; no per-key allocations.
  std::array<char, Endpoints * 125> names;
  size_t used = 0;
  Dictionary() { keys.reserve(Endpoints); }
  unsigned find(std::string_view key, size_t hash) {
    unsigned slot = hash & (slots.size() - 1);
    while (slots[slot]) {
      const unsigned id = slots[slot] - 1;
      if (keys[id] == key)
        return id;
      slot = (slot + 1) & (slots.size() - 1);
    }
    const unsigned id = keys.size();
    memcpy(names.data() + used, key.data(), key.size());
    keys.emplace_back(names.data() + used, key.size());
    used += key.size();
    slots[slot] = id + 1;
    return id;
  }
};

__attribute__((target("avx2"))) static const char *
scan_endpoint(const char *p, const char *end, uint64_t &hash) {
  const char *start = p;
  const auto space = _mm256_set1_epi8(' ');
  // Every SIMD load stays inside the mapped input range.
  while (end - p >= 32) {
    unsigned mask = _mm256_movemask_epi8(_mm256_cmpeq_epi8(
        _mm256_loadu_si256(reinterpret_cast<const __m256i *>(p)), space));
    if (mask) {
      p += __builtin_ctz(mask);
      goto found;
    }
    p += 32;
  }
  while (*p != ' ')
    ++p;
found:
  const unsigned n = p - start;
  uint64_t first, last;
  // An endpoint is followed by at least seven bytes (" 200 0\n").
  memcpy(&first, start, 8);
  if (n < 8)
    first &= (uint64_t(1) << (n * 8)) - 1;
  last = first;
  if (n >= 8)
    memcpy(&last, p - 8, 8);
  // Hashing is deliberately bounded; Dictionary still compares every byte.
  hash = _mm_crc32_u64(_mm_crc32_u64(n, first), last);
  return p;
}

static void append_number(std::string &text, uint64_t value) {
  char buffer[24];
  text.append(buffer,
              std::to_chars(buffer, buffer + sizeof(buffer), value).ptr);
}

struct Span {
  size_t begin, end;
};
using Runs = std::array<std::vector<Span>, Hours>;
static unsigned hour_of(const char *p) {
  const unsigned day = (p[8] - '0') * 10 + p[9] - '0';
  // The frozen spec fixes this date span for every seed.
  return (p[6] == '0' ? day - 30 : day + 1) * 24 + (p[11] - '0') * 10 + p[12] -
         '0' - 18;
}
static uint32_t latency(const char *&p, const char *end) {
  uint32_t value = 0;
  if (end - p >= 8) {
    uint64_t word;
    memcpy(&word, p, 8);
    const uint64_t lf = word ^ 0x0a0a0a0a0a0a0a0aULL;
    const uint64_t mask =
        (lf - 0x0101010101010101ULL) & ~lf & 0x8080808080808080ULL;
    const unsigned n = mask ? __builtin_ctzll(mask) / 8 : 8;
    if (n < 8)
      word = (word << ((8 - n) * 8)) | (0x3030303030303030ULL >> (n * 8));
    word -= 0x3030303030303030ULL;
    word = (word * 10 + (word >> 8)) & 0x00ff00ff00ff00ffULL;
    word = (word * 100 + (word >> 16)) & 0x0000ffff0000ffffULL;
    value = (word * 10000 + (word >> 32)) & 0xffffffffULL;
    p += n;
  }
  while (*p != '\n')
    value = value * 10 + (*p++ - '0');
  ++p;
  return value;
}

__attribute__((target("avx2"))) static const char *newline(const char *p,
                                                           const char *end) {
  const auto lf = _mm256_set1_epi8('\n');
  // Every SIMD load stays inside the mapped input range.
  while (end - p >= 32) {
    unsigned mask = _mm256_movemask_epi8(_mm256_cmpeq_epi8(
        _mm256_loadu_si256(reinterpret_cast<const __m256i *>(p)), lf));
    if (mask)
      return p + __builtin_ctz(mask);
    p += 32;
  }
  return static_cast<const char *>(memchr(p, '\n', end - p));
}

int main(int argc, char **argv) {
  if (argc != 1 && argc != 3)
    return 2;
  const int fd = open(argc == 3 ? argv[1] : "/data/requests.log", O_RDONLY);
  if (fd < 0)
    return 1;
  struct stat st;
  if (fstat(fd, &st))
    return 1;
  const size_t size = st.st_size;
  const char *data = size ? static_cast<const char *>(mmap(
                                nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0))
                          : nullptr;
  close(fd);
  if (size && data == MAP_FAILED)
    return 1;
  FILE *output = fopen(argc == 3 ? argv[2] : "/out/summary.txt", "wb");
  if (!output)
    return 1;
  if (!size)
    return fclose(output) ? 1 : 0;
  const unsigned workers = size >= (8 << 20) ? WORKERS : 1;
  std::vector<Runs> runs(workers);
  std::vector<const char *> boundary(workers + 1, data);
  boundary.back() = data + size;
  for (unsigned i = 1; i < workers; ++i) {
    const char *p = data + size * i / workers;
    while (p < data + size && p[-1] != '\n')
      ++p;
    boundary[i] = p;
  }
  auto parallel = [&](auto action) {
    std::vector<std::thread> threads;
    for (unsigned i = 1; i < workers; ++i)
      threads.emplace_back(action, i);
    action(0);
    for (auto &t : threads)
      t.join();
  };

  parallel([&](unsigned w) {
    const char *p = boundary[w], *end = boundary[w + 1], *start = p;
    unsigned previous = Hours;
    uint64_t last_stamp = ~uint64_t(0);
    while (p < end) {
      uint64_t stamp;
      // Month, day and hour only: minutes/seconds are outside these bytes.
      memcpy(&stamp, p + 6, 8);
      const unsigned h = stamp == last_stamp ? previous : hour_of(p);
      last_stamp = stamp;
      if (h != previous) {
        // Record every transition; arbitrary backwards jumps are legal.
        if (previous != Hours)
          runs[w][previous].push_back({size_t(start - data), size_t(p - data)});
        start = p;
        previous = h;
      }
      // The shortest legal line is 40 bytes including LF.
      p = newline(p + 39, end) + 1;
    }
    if (previous != Hours)
      runs[w][previous].push_back({size_t(start - data), size_t(end - data)});
  });
  std::array<std::array<char, 21>, Hours> hours{};
  for (unsigned h = 0; h < Hours; ++h) {
    unsigned day = (h + 18) / 24 + 30;
    snprintf(hours[h].data(), hours[h].size(), "2026-%02u-%02uT%02u:00:00Z",
             day < 32 ? 10 : 11, day < 32 ? day : day - 31, (h + 18) % 24);
  }
  std::mutex mutex;
  std::condition_variable ready;
  unsigned turn = 0;
  bool success = true;
  parallel([&](unsigned w) {
    Dictionary dictionary;
    std::array<uint64_t, Endpoints> stats{};
    std::array<std::array<uint32_t, 3>, Endpoints> percentiles{};
    std::array<std::vector<uint32_t>, Endpoints> lists;
    std::vector<unsigned> order;
    std::string buffer;
    for (unsigned h = w; h < Hours; h += workers) {

      // Reuse only this worker's current hour; keys persist across hours.
      stats.fill(0);
      for (auto &v : lists)
        v.clear();
      for (const auto &r : runs)
        for (const auto &span : r[h]) {
          const char *p = data + span.begin, *end = data + span.end;
          while (p < end) {
            p += 28;
            switch (*p) {
            case 'G':
              p += 4;
              break;
            case 'H':
              p += 5;
              break;
            case 'D':
              p += 7;
              break;
            case 'O':
              p += 8;
              break;
            default:
              p += p[1] == 'U' ? 4 : p[1] == 'O' ? 5 : 6;
            }
            uint64_t hash = 0;
            const char *ep_end = scan_endpoint(p, end, hash);
            hash ^= size_t(ep_end - p) * 0x9e3779b1ULL;
            unsigned endpoint = dictionary.find({p, size_t(ep_end - p)}, hash);
            // Low/high halves count requests/errors; 3.1M cannot carry.
            stats[endpoint] += 1 + (uint64_t(ep_end[1] == '5') << 32);
            p = ep_end + 5;
            lists[endpoint].push_back(latency(p, end));
          }
        }

      const unsigned endpoints = dictionary.keys.size();

      if (order.size() != endpoints) {
        order.resize(endpoints);
        std::iota(order.begin(), order.end(), 0);
        std::sort(order.begin(), order.end(), [&](unsigned a, unsigned b) {
          return dictionary.keys[a] < dictionary.keys[b];
        });
      }

      for (unsigned e = 0; e < endpoints; ++e) {
        uint64_t n = uint32_t(stats[e]);
        if (!n)
          continue;
        auto *first = lists[e].data(), *end = first + n;
        if (n < 64)
          std::sort(first, end);
        else {
          auto *begin = first;
          for (uint64_t pct : {50, 95, 99}) {
            auto *rank = first + (pct * n + 99) / 100 - 1;
            std::nth_element(begin, rank, end);
            begin = rank + 1;
          }
        }
        unsigned k = 0;
        for (uint64_t pct : {50, 95, 99})
          percentiles[e][k++] = first[(pct * n + 99) / 100 - 1];
      }

      buffer.clear();
      for (unsigned e : order) {
        const uint64_t n = uint32_t(stats[e]), errors = stats[e] >> 32;
        if (!n)
          continue;
        buffer.append(hours[h].data(), 20);
        buffer += ' ';
        buffer.append(dictionary.keys[e]);
        buffer += ' ';
        append_number(buffer, n);
        for (auto value : percentiles[e]) {
          buffer += ' ';
          append_number(buffer, value);
        }
        buffer += ' ';
        append_number(buffer, errors);
        buffer += ' ';
        append_number(buffer, errors * 1000000 / n);
        buffer += '\n';
      }

      std::unique_lock lock(mutex);
      // Selection/formatting is private; only ordered writes synchronize.
      ready.wait(lock, [&] { return turn == h; });
      if (fwrite(buffer.data(), 1, buffer.size(), output) != buffer.size())
        success = false;
      ++turn;
      lock.unlock();
      ready.notify_all();
    }
  });
  if (fclose(output))
    success = false;
  munmap(const_cast<char *>(data), size);
  return success ? 0 : 1;
}
