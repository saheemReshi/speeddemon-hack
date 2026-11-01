#include <algorithm>
#include <array>
#include <charconv>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <memory>
#include <mutex>
#include <nmmintrin.h>
#include <string>
#include <string_view>
#include <sys/mman.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>
#ifndef WORKERS
#define WORKERS 6
#endif

constexpr unsigned Hours = 72, Endpoints = 1842, Groups = Hours * Endpoints;
static unsigned local_group(unsigned endpoint, unsigned hour) {
  return endpoint * Hours + hour;
}

static size_t hash_endpoint(std::string_view key) {
  uint64_t hash = key.size();
  const char *p = key.data();
  size_t n = key.size();
  while (n >= 8) {
    uint64_t word;
    memcpy(&word, p, 8);
    hash = _mm_crc32_u64(hash, word);
    p += 8;
    n -= 8;
  }
  while (n--)
    hash = _mm_crc32_u8(hash, *p++);
  return hash;
}

struct Dictionary {
  // The spec caps the catalogue at 1,842 keys. Collisions still compare bytes.
  std::array<uint16_t, 4096> slots{};
  std::vector<std::string_view> keys;
  Dictionary() { keys.reserve(Endpoints); }
  unsigned find(std::string_view key) { return find(key, hash_endpoint(key)); }
  unsigned find(std::string_view key, size_t hash) {
    unsigned slot = hash & (slots.size() - 1);
    while (slots[slot]) {
      const unsigned id = slots[slot] - 1;
      if (keys[id] == key)
        return id;
      slot = (slot + 1) & (slots.size() - 1);
    }
    const unsigned id = keys.size();
    keys.push_back(key);
    slots[slot] = id + 1;
    return id;
  }
};

static const char *scan_endpoint(const char *p, const char *end,
                                 uint64_t &hash) {
  while (end - p >= 8) {
    uint64_t word;
    memcpy(&word, p, 8);
    // Detect the first space using the standard zero-byte word test.
    const uint64_t spaces = word ^ 0x2020202020202020ULL;
    const uint64_t mask =
        (spaces - 0x0101010101010101ULL) & ~spaces & 0x8080808080808080ULL;
    if (mask) {
      const unsigned n = __builtin_ctzll(mask) / 8;
      word &= n ? (uint64_t(1) << (n * 8)) - 1 : 0;
      hash = _mm_crc32_u64(hash, word);
      return p + n;
    }
    hash = _mm_crc32_u64(hash, word);
    p += 8;
  }
  uint64_t word = 0;
  unsigned n = 0;
  while (*p != ' ')
    word |= uint64_t(uint8_t(*p++)) << (8 * n++);
  hash = _mm_crc32_u64(hash, word);
  return p;
}

struct Worker {
  Dictionary dictionary;
  std::unique_ptr<uint64_t[]> stats;
  std::unique_ptr<uint32_t[]> cursors;
};
struct Group {
  uint32_t begin = 0, count = 0, errors = 0;
};

template <bool Count>
static void parse(const char *p, const char *end, Worker &worker,
                  uint32_t *values) {
  while (p < end) {
    const unsigned month = (p[5] - '0') * 10 + p[6] - '0';
    const unsigned day = (p[8] - '0') * 10 + p[9] - '0';
    const unsigned hour = (month == 10 ? day - 30 : day + 1) * 24 +
                          (p[11] - '0') * 10 + p[12] - '0' - 18;
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
    const char *endpoint_end = scan_endpoint(p, end, hash);
    hash ^= size_t(endpoint_end - p) * 0x9e3779b1ULL;
    const unsigned endpoint =
        worker.dictionary.find({p, size_t(endpoint_end - p)}, hash);
    const unsigned group = local_group(endpoint, hour);
    if constexpr (Count) {
      // Low 32 bits count requests, high 32 bits count errors; 3.1M cannot
      // carry.
      worker.stats[group] += 1 + (uint64_t(endpoint_end[1] == '5') << 32);
      p = static_cast<const char *>(
              memchr(endpoint_end + 5, '\n', end - endpoint_end - 5)) +
          1;
    } else {
      p = endpoint_end + 5;
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
        // Combine digits into pairs, then quads, then one value.
        word -= 0x3030303030303030ULL;
        word = (word * 10 + (word >> 8)) & 0x00ff00ff00ff00ffULL;
        word = (word * 100 + (word >> 16)) & 0x0000ffff0000ffffULL;
        value = (word * 10000 + (word >> 32)) & 0xffffffffULL;
        p += n;
      }
      while (*p != '\n')
        value = value * 10 + (*p++ - '0');
      ++p;
      values[worker.cursors[group]++] = value;
    }
  }
}

static void append_number(std::string &text, uint64_t value) {
  char buffer[24];
  text.append(buffer,
              std::to_chars(buffer, buffer + sizeof(buffer), value).ptr);
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
  std::vector<Worker> partial(workers);
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
    for (auto &thread : threads)
      thread.join();
  };
  parallel([&](unsigned i) {
    partial[i].stats = std::make_unique<uint64_t[]>(Groups);
    parse<true>(boundary[i], boundary[i + 1], partial[i], nullptr);
  });
  Dictionary dictionary;
  std::vector<Group> groups(Groups);
  std::vector<std::vector<unsigned>> translations(workers);
  for (unsigned i = 0; i < workers; ++i) {
    auto &worker = partial[i];
    for (unsigned id = 0; id < worker.dictionary.keys.size(); ++id) {
      const unsigned global = dictionary.find(worker.dictionary.keys[id]);
      translations[i].push_back(global);
      for (unsigned h = 0; h < Hours; ++h) {
        const uint64_t stats = worker.stats[local_group(id, h)];
        auto &group = groups[h * Endpoints + global];
        group.count += uint32_t(stats);
        group.errors += stats >> 32;
      }
    }
  }
  uint32_t total = 0;
  std::vector<uint32_t> next(Groups);
  for (unsigned g = 0; g < Groups; ++g) {
    groups[g].begin = next[g] = total;
    total += groups[g].count;
  }
  for (unsigned i = 0; i < workers; ++i) {
    auto &worker = partial[i];
    worker.cursors = std::make_unique<uint32_t[]>(Groups);
    // Prefix sums give workers disjoint slices, even for shuffled input.
    for (unsigned id = 0; id < worker.dictionary.keys.size(); ++id) {
      for (unsigned h = 0; h < Hours; ++h) {
        const unsigned local = local_group(id, h);
        auto &cursor = next[h * Endpoints + translations[i][id]];
        worker.cursors[local] = cursor;
        cursor += uint32_t(worker.stats[local]);
      }
    }
    worker.stats.reset();
  }
  next.clear();
  next.shrink_to_fit();
  const size_t latency_bytes = size_t(total) * sizeof(uint32_t);
  auto *values = static_cast<uint32_t *>(
      mmap(nullptr, latency_bytes, PROT_READ | PROT_WRITE,
           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
  if (values == MAP_FAILED)
    return 1;
  parallel([&](unsigned i) {
    parse<false>(boundary[i], boundary[i + 1], partial[i], values);
  });
  for (auto &worker : partial)
    worker.cursors.reset();

  std::vector<unsigned> order(dictionary.keys.size());
  for (unsigned i = 0; i < order.size(); ++i)
    order[i] = i;
  std::sort(order.begin(), order.end(), [&](unsigned a, unsigned b) {
    return dictionary.keys[a] < dictionary.keys[b];
  });
  std::array<std::array<char, 21>, Hours> hours{};
  for (unsigned h = 0; h < Hours; ++h) {
    unsigned day = (h + 18) / 24 + 30;
    snprintf(hours[h].data(), hours[h].size(), "2026-%02u-%02uT%02u:00:00Z",
             day < 32 ? 10 : 11, day < 32 ? day : day - 31, (h + 18) % 24);
  }
  const size_t page = sysconf(_SC_PAGESIZE);
  std::mutex mutex;
  std::condition_variable ready;
  unsigned turn = 0;
  bool success = true;
  parallel([&](unsigned worker) {
    std::string buffer;
    buffer.reserve(std::min<size_t>(160 << 10, size / Hours));
    for (unsigned h = worker; h < Hours; h += workers) {
      buffer.clear();
      for (unsigned endpoint : order) {
        const auto &group = groups[h * Endpoints + endpoint];
        const uint64_t n = group.count;
        if (!n)
          continue;
        auto *first = values + group.begin, *end = first + n;
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
        buffer.append(hours[h].data(), 20);
        buffer += ' ';
        buffer.append(dictionary.keys[endpoint]);
        buffer += ' ';
        append_number(buffer, n);
        for (uint64_t pct : {50, 95, 99}) {
          buffer += ' ';
          append_number(buffer, first[(pct * n + 99) / 100 - 1]);
        }
        buffer += ' ';
        append_number(buffer, group.errors);
        buffer += ' ';
        append_number(buffer, uint64_t(group.errors) * 1000000 / n);
        buffer += '\n';
      }
      // Discard only whole pages owned entirely by this completed hour.
      const size_t start =
          (size_t(groups[h * Endpoints].begin) * 4 + page - 1) / page * page;
      const size_t stop =
          h + 1 < Hours
              ? size_t(groups[(h + 1) * Endpoints].begin) * 4 / page * page
              : latency_bytes / page * page;
      if (stop > start)
        madvise(reinterpret_cast<char *>(values) + start, stop - start,
                MADV_DONTNEED);
      std::unique_lock lock(mutex);
      // Keep one live hour buffer per worker; serialize only the file writes.
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
  munmap(values, latency_bytes);
  munmap(const_cast<char *>(data), size);
  return success ? 0 : 1;
}
