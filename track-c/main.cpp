#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <immintrin.h>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/mman.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <unordered_map>
#include <vector>

// The mapping outlives every worker and every key stored in the name table.
struct Input {
  const char *begin;
  size_t size;
  explicit Input(const std::string &path) {
    int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0)
      throw std::runtime_error("cannot open " + path);
    struct stat st{};
    if (fstat(fd, &st) || st.st_size == 0) {
      close(fd);
      throw std::runtime_error("cannot stat nonempty input " + path);
    }
    size = st.st_size;
    auto mapping = mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (mapping == MAP_FAILED)
      throw std::runtime_error("mmap failed");
    begin = static_cast<const char *>(mapping);
    madvise(const_cast<char *>(begin), size, MADV_SEQUENTIAL);
  }
  ~Input() { munmap(const_cast<char *>(begin), size); }
  Input(const Input &) = delete;
  Input &operator=(const Input &) = delete;
};

struct Parser {
  const char *p, *end;
  void bom() {
    if (end - p >= 3 && std::memcmp(p, "\xef\xbb\xbf", 3) == 0)
      p += 3;
  }
  uint64_t number() {
    uint64_t n = 0;
    // Every use is followed by ',' or '.', including the final record.
    while (unsigned(*p - '0') < 10)
      n = n * 10 + (*p++ - '0');
    return n;
  }
  const char *quote() const {
    if (end - p >= 16) {
      auto block = _mm_loadu_si128(reinterpret_cast<const __m128i *>(p));
      unsigned mask =
          _mm_movemask_epi8(_mm_cmpeq_epi8(block, _mm_set1_epi8('"')));
      if (mask)
        return p + __builtin_ctz(mask);
      return static_cast<const char *>(memchr(p + 16, '"', end - p - 16));
    }
    return static_cast<const char *>(memchr(p, '"', end - p));
  }
  // Closing quotes are distinguished from doubled literal quotes. Embedded
  // newlines and BOMs are ordinary bytes here, never record boundaries.
  size_t text(std::string_view *value) {
    const char *start = p;
    if (*p != '"') {
      while (p < end && *p != ',' && *p != '\r' && *p != '\n')
        ++p;
      if (value)
        *value = std::string_view(start, p - start);
      size_t length = p - start;
      ++p; // the caller uses text only for comma-terminated fields
      return length;
    }
    start = ++p;
    size_t length = 0;
    for (;;) {
      auto q = quote();
      if (!q)
        throw std::runtime_error("unterminated quoted field");
      length += q - p;
      if (q + 1 == end || q[1] != '"') {
        if (value)
          *value = std::string_view(start, q - start);
        p = q + 1;
        if (p == end || *p != ',')
          throw std::runtime_error("missing comma");
        ++p;
        return length;
      }
      ++length;
      p = q + 2;
    }
  }
  void terminator() {
    if (p < end && *p == '\r')
      ++p;
    if (p < end && *p == '\n')
      ++p;
  }
};

struct alignas(64) Group {
  uint64_t count = 0, ids = 0, name_bytes = 0, note_bytes = 0, distinct = 0;
  int64_t sum = 0, min = INT64_MAX, max = INT64_MIN;
};

struct Regions {
  std::vector<uint8_t> values{0};
  void set(uint64_t id, uint8_t country) {
    if (values.size() <= id)
      values.resize(id + 1);
    values[id] = country;
  }
  uint8_t get(uint64_t id) const { return id < values.size() ? values[id] : 0; }
};

class Names {
  struct Slot {
    std::atomic<uintptr_t> key{0};
    std::array<std::atomic<uint64_t>, 4> countries{};
  };
  size_t capacity;
  std::unique_ptr<Slot[]> slots;
  const char *end;
  struct Hash {
    using is_transparent = void;
    size_t operator()(std::string_view s) const {
      return std::hash<std::string_view>{}(s);
    }
  };
  std::mutex overflow_mutex;
  std::unordered_map<std::string, std::array<uint64_t, 4>, Hash,
                     std::equal_to<>>
      overflow;

  static uint32_t hash_of(std::string_view name) {
#ifdef NAME_FORCE_COLLISIONS
    return 0;
#endif
    uint32_t hash = _mm_crc32_u32(0x9e3779b9, name.size());
    const char *p = name.data();
    size_t n = name.size();
    if (n >= 8) {
      uint64_t first, last;
      memcpy(&first, p, 8);
      memcpy(&last, p + n - 8, 8);
      return _mm_crc32_u64(_mm_crc32_u64(hash, first), last);
    }
    if (n & 4) {
      uint32_t word;
      memcpy(&word, p, 4);
      hash = _mm_crc32_u32(hash, word);
      p += 4;
    }
    if (n & 2) {
      uint16_t word;
      memcpy(&word, p, 2);
      hash = _mm_crc32_u16(hash, word);
      p += 2;
    }
    if (n & 1)
      hash = _mm_crc32_u8(hash, *p);
    return hash;
  }

  bool equal(uintptr_t key, std::string_view name) const {
    const char *raw = reinterpret_cast<const char *>(key);
    bool quoted = *raw == '"';
    raw += quoted;
    size_t available = end - raw;
    if (available < name.size() || available - name.size() < (quoted ? 2u : 1u))
      return false;
    const char *tail = raw + name.size();
    bool boundary = quoted ? tail[0] == '"' && tail[1] == ',' : tail[0] == ',';
    if (!boundary)
      return false;
    size_t n = name.size();
    const char *value = name.data();
    if (n >= 8 && n <= 16) {
      uint64_t a, b, c, d;
      memcpy(&a, raw, 8);
      memcpy(&b, value, 8);
      memcpy(&c, raw + n - 8, 8);
      memcpy(&d, value + n - 8, 8);
      return a == b && c == d;
    }
    if (n >= 4 && n < 8) {
      uint32_t a, b, c, d;
      memcpy(&a, raw, 4);
      memcpy(&b, value, 4);
      memcpy(&c, raw + n - 4, 4);
      memcpy(&d, value + n - 4, 4);
      return a == b && c == d;
    }
    if (n >= 2 && n < 4) {
      uint16_t a, b, c, d;
      memcpy(&a, raw, 2);
      memcpy(&b, value, 2);
      memcpy(&c, raw + n - 2, 2);
      memcpy(&d, value + n - 2, 2);
      return a == b && c == d;
    }
    if (n < 2)
      return n == 0 || *raw == *value;
    return name.empty() || memcmp(raw, name.data(), name.size()) == 0;
  }

public:
#ifndef NAME_CAPACITY
#define NAME_CAPACITY 262144
#endif
  explicit Names(const char *end, size_t capacity = NAME_CAPACITY)
      : capacity(capacity), slots(std::make_unique<Slot[]>(capacity)),
        end(end) {}
  bool add(std::string_view name, uint8_t country, const char *field) {
    uint32_t hash = hash_of(name);
    size_t pos = hash & (capacity - 1);
    for (size_t probe = 0; probe < std::min(capacity, size_t{64}); ++probe) {
      auto &slot = slots[pos];
      uintptr_t key = slot.key.load(std::memory_order_relaxed);
      if (!key) {
        auto desired = reinterpret_cast<uintptr_t>(field);
        if (slot.key.compare_exchange_strong(key, desired,
                                             std::memory_order_relaxed))
          key = desired;
      }
      if (key == reinterpret_cast<uintptr_t>(field) || equal(key, name)) {
        uint64_t bit = uint64_t{1} << (country % 64);
        if (slot.countries[country / 64].load(std::memory_order_relaxed) & bit)
          return false;
        auto previous = slot.countries[country / 64].fetch_or(
            bit, std::memory_order_relaxed);
        return !(previous & bit);
      }
      pos = (pos + 1) & (capacity - 1);
    }
    // Slots never become empty. Once a key's probe prefix fills, later copies
    // must also reach overflow, so no key can be duplicated across the tables.
    std::lock_guard lock(overflow_mutex);
    auto it = overflow.find(name);
    if (it == overflow.end())
      it = overflow.try_emplace(std::string(name)).first;
    auto &bits = it->second[country / 64];
    uint64_t bit = uint64_t{1} << (country % 64);
    bool fresh = !(bits & bit);
    bits |= bit;
    return fresh;
  }
};

struct Worker {
  std::array<Group, 256> groups{};
  void run(const char *start, const char *end, Names &names,
           const Regions &regions) {
    Parser csv{start, end};
    while (csv.p < csv.end) {
      csv.bom();
      uint64_t record_id = csv.number();
      ++csv.p;
      const char *field = csv.p;
      std::string_view name;
      size_t name_bytes = csv.text(&name);
      size_t note_bytes = csv.text(nullptr);
      uint64_t region = csv.number();
      ++csv.p;
      bool negative = *csv.p == '-';
      csv.p += negative;
      int64_t cents = csv.number() * 100;
      ++csv.p;
      cents += (csv.p[0] - '0') * 10 + csv.p[1] - '0';
      csv.p += 2;
      if (negative)
        cents = -cents;
      csv.terminator();
      uint8_t country = regions.get(region);
      auto &g = groups[country];
      ++g.count;
      g.ids += record_id;
      g.sum += cents;
      g.min = std::min(g.min, cents);
      g.max = std::max(g.max, cents);
      g.name_bytes += name_bytes;
      g.note_bytes += note_bytes;
      g.distinct += names.add(name, country, field);
    }
  }
};

// Quote parity is sufficient even for doubled quotes: each pair toggles twice.
// SIMD counts quotes before each target; only the boundary search is scalar.
std::vector<const char *> partitions(const char *begin, const char *end,
                                     unsigned n) {
  std::vector<const char *> cuts{begin};
  const char *p = begin;
  bool quoted = false;
  for (unsigned i = 1; i < n; ++i) {
    const char *target = begin + (end - begin) * i / n;
    while (target - p >= 32) {
      auto block = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(p));
      unsigned quotes =
          _mm256_movemask_epi8(_mm256_cmpeq_epi8(block, _mm256_set1_epi8('"')));
      quoted ^= __builtin_parity(quotes);
      p += 32;
    }
    while (p < end && (p < target || quoted || p[-1] != '\n')) {
      quoted ^= *p == '"';
      ++p;
    }
    cuts.push_back(p);
  }
  cuts.push_back(end);
  return cuts;
}

std::string money(int64_t n) {
  bool negative = n < 0;
  uint64_t magnitude = negative ? -n : n;
  std::string s = (negative ? "-" : "") + std::to_string(magnitude / 100);
  s += '.';
  s += char('0' + magnitude / 10 % 10);
  s += char('0' + magnitude % 10);
  return s;
}

int main(int argc, char **argv) {
  try {
    std::string data = "/data", out = "/out";
#ifndef JOIN_THREADS
#define JOIN_THREADS 12
#endif
    unsigned thread_count = JOIN_THREADS;
    for (int i = 1; i < argc; i += 2) {
      if (i + 1 == argc)
        throw std::runtime_error("missing argument value");
      if (std::string_view(argv[i]) == "--data")
        data = argv[i + 1];
      else if (std::string_view(argv[i]) == "--out")
        out = argv[i + 1];
      else if (std::string_view(argv[i]) == "--threads")
        thread_count = std::stoul(argv[i + 1]);
      else
        throw std::runtime_error("unknown argument");
    }
    Regions regions;
    std::vector<std::string> countries{"_UNMATCHED"};
    std::array<uint8_t, 26 * 26 * 26> country_ids{};
    {
      Input input(data + "/regions.csv");
      Parser csv{input.begin, input.begin + input.size};
      while (csv.p < csv.end && *csv.p++ != '\n') {
      }
      while (csv.p < csv.end) {
        auto region = csv.number();
        ++csv.p;
        unsigned code =
            (csv.p[0] - 'A') * 676 + (csv.p[1] - 'A') * 26 + csv.p[2] - 'A';
        auto &id = country_ids[code];
        if (!id) {
          if (countries.size() == 256)
            throw std::runtime_error("too many countries");
          id = countries.size();
          countries.emplace_back(csv.p, 3);
        }
        regions.set(region, id);
        csv.p += 4;
      }
    }
    // The CSV interior is a canonical key: literal quotes must be doubled,
    // including in any quoted representation of an otherwise unquoted value.
    // Delimiters are excluded; quoted and unquoted empty values share one key.
    // Hashes select buckets only; exact bytes resolve all collisions.
    Input input(data + "/records.csv");
    Parser csv{input.begin, input.begin + input.size};
    while (csv.p < csv.end && *csv.p++ != '\n') {
    }
    if (!thread_count || thread_count > 64)
      throw std::runtime_error("invalid thread count");
    auto cuts = partitions(csv.p, csv.end, thread_count);
    Names names(csv.end);
    std::vector<Worker> workers(thread_count);
    std::vector<std::exception_ptr> errors(thread_count);
    auto run = [&](unsigned i) {
      try {
        workers[i].run(cuts[i], cuts[i + 1], names, regions);
      } catch (...) {
        errors[i] = std::current_exception();
      }
    };
    std::vector<std::jthread> threads;
    for (unsigned i = 1; i < thread_count; ++i)
      threads.emplace_back(run, i);
    run(0);
    threads.clear();
    for (auto error : errors)
      if (error)
        std::rethrow_exception(error);
    auto &groups = workers[0].groups;
    for (unsigned i = 1; i < thread_count; ++i) {
      for (unsigned c = 0; c < countries.size(); ++c) {
        auto &g = groups[c];
        const auto &part = workers[i].groups[c];
        g.count += part.count;
        g.ids += part.ids;
        g.sum += part.sum;
        g.name_bytes += part.name_bytes;
        g.note_bytes += part.note_bytes;
        g.distinct += part.distinct;
        g.min = std::min(g.min, part.min);
        g.max = std::max(g.max, part.max);
      }
    }
    std::vector<unsigned> order;
    for (unsigned i = 0; i < countries.size(); ++i)
      if (groups[i].count)
        order.push_back(i);
    std::sort(order.begin(), order.end(),
              [&](auto a, auto b) { return countries[a] < countries[b]; });
    std::string result;
    for (auto i : order) {
      auto &g = groups[i];
      result += countries[i] + " " + std::to_string(g.count) + " " +
                std::to_string(g.ids) + " " + money(g.sum) + " " +
                money(g.min) + " " + money(g.max) + " " +
                std::to_string(g.distinct) + " " +
                std::to_string(g.name_bytes) + " " +
                std::to_string(g.note_bytes) + "\n";
    }
    FILE *file = fopen((out + "/aggregates.txt").c_str(), "wb");
    if (!file)
      throw std::runtime_error("cannot open output");
    bool written =
        fwrite(result.data(), 1, result.size(), file) == result.size();
    if (fclose(file) || !written)
      throw std::runtime_error("cannot write output");
  } catch (const std::exception &e) {
    fprintf(stderr, "%s\n", e.what());
    return 1;
  }
}
