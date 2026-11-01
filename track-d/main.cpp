#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <limits>
#include <stdexcept>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <thread>
#include <type_traits>
#include <unistd.h>
#include <utility>
#include <vector>

using U8 = uint8_t;
using U32 = uint32_t;
using U64 = uint64_t;
constexpr U64 INF = std::numeric_limits<U64>::max() / 4;
constexpr U32 NODE_MASK = (1U << 21) - 1;

U32 number(const char *&p) {
  // Well-formed ASCII input has only digits, SP and LF. The first byte is a
  // digit.
  U32 value = *p++ - '0';
  while (*p >= '0')
    value = value * 10 + (*p++ - '0');
  ++p;
  return value;
}

struct Input {
  const char *begin, *end;
  size_t size;
  explicit Input(const std::string &path) {
    int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0)
      throw std::runtime_error("open " + path);
    struct stat st;
    if (fstat(fd, &st)) {
      close(fd);
      throw std::runtime_error("fstat " + path);
    }
    size = st.st_size;
    void *mapping =
        size ? mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0) : nullptr;
    close(fd);
    if (mapping == MAP_FAILED)
      throw std::runtime_error("mmap " + path);
    begin = static_cast<const char *>(mapping);
    end = size ? begin + size : begin;
  }
  ~Input() {
    if (size)
      munmap(const_cast<char *>(begin), size);
  }
};

int workers(const char *name) {
  const char *value = std::getenv(name);
  return value ? std::clamp(std::stoi(value), 1, 12) : 6;
}

template <class Work> void parallel(int count, Work work) {
  std::vector<std::thread> threads;
  threads.reserve(count - 1);
  for (int i = 1; i < count; ++i)
    threads.emplace_back(work, i);
  work(0);
  for (auto &thread : threads)
    thread.join();
}

struct Arc {
  U32 from, to, weight;
};
constexpr U32 INLINED = 2;
constexpr U32 DEGREE_BITS = std::bit_width(INLINED + 1);
constexpr U32 DEGREE_MASK = (1U << DEGREE_BITS) - 1;
// Low metadata bits encode degree 0, 1, 2, or "more". High bits are a
// CSR prefix into the spill array; nodes[n] supplies the final spill endpoint.
struct Graph {
  struct Node {
    U32 meta;
    std::array<U32, INLINED> edge;
  };
  std::vector<Node> nodes;
  std::vector<U32> extra;
  const std::vector<Arc> *large = nullptr;
  bool reverse = false;
  std::pair<U32, U32> decode(U32 code) const {
    if (code & 0x80000000U) {
      auto a = (*large)[code & 0x7fffffffU];
      return {reverse ? a.from : a.to, a.weight};
    }
    return {code & NODE_MASK, code >> 21};
  }
  template <class Visit> void visit(U32 u, Visit work) const {
    const auto &node = nodes[u];
    U32 tag = node.meta & DEGREE_MASK;
    for (U32 j = 0; j < std::min(tag, INLINED); ++j)
      work(node.edge[j]);
    if (tag > INLINED)
      for (U32 i = node.meta >> DEGREE_BITS;
           i < (nodes[u + 1].meta >> DEGREE_BITS); ++i)
        work(extra[i]);
  }
  void store(U32 u, U32 position, U32 code) {
    if (position < INLINED)
      nodes[u].edge[position] = code;
    else
      extra[(nodes[u].meta >> DEGREE_BITS) + position - INLINED] = code;
  }
};
void build(const std::string &path, Graph &forward, Graph &reverse,
           std::vector<Arc> &large) {
  Input input(path);
  const char *start = input.begin;
  U32 n = number(start), m = number(start);
  if (n > (1U << 21) || m >= (1U << (32 - DEGREE_BITS)))
    throw std::runtime_error("graph exceeds specified size");
  int count = workers("D_BUILD");
  std::vector<const char *> cuts(count + 1);
  cuts[0] = start;
  cuts[count] = input.end;
  for (int i = 1; i < count; ++i) {
    const char *p = start + (input.end - start) * i / count;
    while (p < input.end && p[-1] != '\n')
      ++p;
    cuts[i] = p;
  }
  // Shared byte degrees fit in cache. Wrap records preserve arbitrary degrees.
  std::vector<std::array<U8, 2>> bytes(n);
  std::vector<std::vector<U32>> overflow(count);
  std::vector<U32> big(count + 1);
  parallel(count, [&](int part) {
    const char *p = cuts[part];
    U32 large_count = 0;
    while (p < cuts[part + 1]) {
      U32 u = number(p), v = number(p), w = number(p);
      if (std::atomic_ref<U8>(bytes[u][0])
              .fetch_add(1, std::memory_order_relaxed) == 255)
        overflow[part].push_back(u * 2);
      if (std::atomic_ref<U8>(bytes[v][1])
              .fetch_add(1, std::memory_order_relaxed) == 255)
        overflow[part].push_back(v * 2 + 1);
      large_count += w >= 1024;
    }
    big[part + 1] = large_count;
  });
  // Reuse counts as relative insertion cursors. Any byte overflow selects
  // full-width cursors for this run, so no maximum-degree assumption is needed.
  bool small = std::all_of(overflow.begin(), overflow.end(),
                           [](const auto &a) { return a.empty(); });
  std::vector<std::array<U32, 2>> wide;
  if (!small) {
    wide.resize(n);
    for (U32 u = 0; u < n; ++u)
      wide[u] = {bytes[u][0], bytes[u][1]};
    for (auto &list : overflow)
      for (U32 key : list)
        wide[key >> 1][key & 1] += 256;
    decltype(bytes){}.swap(bytes);
  }
  // Prefix metadata and inline slots replace separate node-sized offset arrays.
  forward.nodes.resize(n + 1);
  reverse.nodes.resize(n + 1);
  U32 a = 0, b = 0;
  for (U32 u = 0; u < n; ++u) {
    U32 x = small ? bytes[u][0] : wide[u][0],
        y = small ? bytes[u][1] : wide[u][1];
    forward.nodes[u].meta = (a << DEGREE_BITS) | std::min(x, INLINED + 1);
    reverse.nodes[u].meta = (b << DEGREE_BITS) | std::min(y, INLINED + 1);
    a += x > INLINED ? x - INLINED : 0;
    b += y > INLINED ? y - INLINED : 0;
    if (small)
      bytes[u] = {0, 0};
    else
      wide[u] = {0, 0};
  }
  forward.nodes[n].meta = a << DEGREE_BITS;
  reverse.nodes[n].meta = b << DEGREE_BITS;
  forward.extra.resize(a);
  reverse.extra.resize(b);
  for (int i = 0; i < count; ++i)
    big[i + 1] += big[i];
  large.resize(big.back());
  forward.large = reverse.large = &large;
  reverse.reverse = true;
  // Phase joins synchronize relaxed counters; reserved slots never overlap.
  auto fill = [&]<bool Small>() {
    parallel(count, [&](int part) {
      const char *p = cuts[part];
      U32 next = big[part];
      while (p < cuts[part + 1]) {
        U32 u = number(p), v = number(p), w = number(p), x, y;
        if (w < 1024) {
          x = (w << 21) | v;
          y = (w << 21) | u;
        } else {
          large[next] = {u, v, w};
          x = y = 0x80000000U | next++;
        }
        U32 i, j;
        if constexpr (Small) {
          i = std::atomic_ref<U8>(bytes[u][0])
                  .fetch_add(1, std::memory_order_relaxed);
          j = std::atomic_ref<U8>(bytes[v][1])
                  .fetch_add(1, std::memory_order_relaxed);
        } else {
          i = std::atomic_ref<U32>(wide[u][0])
                  .fetch_add(1, std::memory_order_relaxed);
          j = std::atomic_ref<U32>(wide[v][1])
                  .fetch_add(1, std::memory_order_relaxed);
        }
        forward.store(u, i, x);
        reverse.store(v, j, y);
      }
    });
  };
  if (small)
    fill.template operator()<true>();
  else
    fill.template operator()<false>();
}

template <class Word> struct RadixHeap {
  using Item = std::pair<Word, U32>;
  std::array<std::vector<Item>, sizeof(Word) * 8 + 1> buckets;
  Word last = 0;
  size_t count = 0;
  bool empty() const { return count == 0; }
  size_t size() const { return count; }
  void clear() {
    for (auto &b : buckets)
      b.clear();
    count = 0;
    last = 0;
  }
  void push(Word d, U32 v) {
    buckets[std::bit_width(Word(d ^ last))].emplace_back(d, v);
    ++count;
  }
  Item top() {
    if (buckets[0].empty()) {
      size_t i = 1;
      while (buckets[i].empty())
        ++i;
      last = std::numeric_limits<Word>::max();
      for (auto x : buckets[i])
        last = std::min(last, x.first);
      // Keys only increase: redistribution always moves to a lower bucket.
      for (auto x : buckets[i])
        buckets[std::bit_width(Word(x.first ^ last))].push_back(x);
      buckets[i].clear();
    }
    return buckets[0].back();
  }
  Item pop() {
    auto x = top();
    buckets[0].pop_back();
    --count;
    return x;
  }
};

template <class Word> struct Search {
  const Graph &forward, &reverse;
  static constexpr U64 LIMIT = sizeof(Word) == 4 ? NODE_MASK : (U64{1} << 53);
  struct Wide {
    U64 forward, backward;
    U32 key;
  };
  using State = std::conditional_t<sizeof(Word) == 4, U64, Wide>;
  std::vector<U32> head, next;
  std::vector<State> states;
  std::array<RadixHeap<Word>, 2> heap;
  Search(const Graph &f, const Graph &r)
      : forward(f), reverse(r), head(1U << 16), next(1), states(1) {}
  U32 key(const State &s) const {
    if constexpr (sizeof(Word) == 4)
      return U32(s) & NODE_MASK;
    else
      return s.key;
  }
  State &access(U32 vertex) {
    U32 bucket = (U64(vertex) * 11400714819323198485ULL) >> 48;
    U32 id = head[bucket];
    while (id && key(states[id]) != vertex)
      id = next[id];
    if (!id) {
      id = states.size();
      // One node id and two 21-bit distances fit in eight bytes. All-ones
      // distances mean absent; the wide fallback retains full 64-bit values.
      if constexpr (sizeof(Word) == 4)
        states.push_back((~U64(NODE_MASK)) | vertex);
      else
        states.push_back({INF, INF, vertex});
      next.push_back(head[bucket]);
      head[bucket] = id;
    }
    return states[id];
  }
  U64 distance(const State &s, int side) const {
    if constexpr (sizeof(Word) == 4)
      return (s >> (side ? 42 : 21)) & NODE_MASK;
    else
      return side ? s.backward : s.forward;
  }
  void set(State &s, int side, U64 d) {
    if constexpr (sizeof(Word) == 4) {
      int shift = side ? 42 : 21;
      s = (s & ~(U64(NODE_MASK) << shift)) | (d << shift);
    } else
      (side ? s.backward : s.forward) = d;
  }
  void put(int side, U32 vertex, U64 d) {
    auto &state = access(vertex);
    set(state, side, d);
    heap[side].push(Word(d), vertex);
  }
  U64 run(U32 source, U32 target) {
    // Only the small directory is cleared. Arena capacities survive queries;
    // active state is contiguous and grows with visited nodes, not graph size.
    std::fill(head.begin(), head.end(), 0);
    states.resize(1);
    next.resize(1);
    for (auto &h : heap)
      h.clear();
    if (source == target)
      return 0;
    if ((forward.nodes[source].meta & DEGREE_MASK) == 0 ||
        (reverse.nodes[target].meta & DEGREE_MASK) == 0)
      return INF;
    put(0, source, 0);
    put(1, target, 0);
    U64 best = INF;
    bool tie = false;
    while (!heap[0].empty() && !heap[1].empty()) {
      // A meeting is only an upper bound. Queue minima certify termination.
      if (U64(heap[0].top().first) + heap[1].top().first >= best)
        break;
      int side = heap[0].size() == heap[1].size()
                     ? (tie = !tie)
                     : heap[0].size() > heap[1].size();
      auto [d, u] = heap[side].pop();
      if (d != distance(access(u), side))
        continue;
      const Graph &graph = side ? reverse : forward;
      graph.visit(u, [&](U32 code) {
        auto [v, w] = graph.decode(code);
        U64 nd = U64(d) + w; // Widen before adding a uint32 weight.
        if (nd >= LIMIT)
          return;
        auto &state = access(v);
        if (nd < distance(state, side)) {
          set(state, side, nd);
          heap[side].push(Word(nd), v);
          U64 other = distance(state, 1 - side);
          if (other < LIMIT)
            best = std::min(best, nd + other);
        }
      });
    }
    // Every prefix of a nonnegative path shorter than LIMIT is also below it.
    // Only certified short answers return here; all others get the exact pass.
    return best < LIMIT ? best : INF;
  }
};

int main(int argc, char **argv) {
  try {
    std::string data = argc > 1 ? argv[1] : "/data";
    std::string output = argc > 2 ? argv[2] : "/out";
    Graph forward, reverse;
    std::vector<Arc> large;
    build(data + "/graph.txt", forward, reverse, large);
    std::vector<std::pair<U32, U32>> queries;
    {
      Input input(data + "/queries.txt");
      const char *p = input.begin;
      while (p != input.end) {
        U32 source = number(p), target = number(p);
        queries.emplace_back(source, target);
      }
    }
    std::vector<U64> answers(queries.size());
    if (!queries.empty()) {
      std::atomic<size_t> next{0};
      int count =
          std::min(workers("D_THREADS"), static_cast<int>(queries.size()));
      parallel(count, [&](int) {
        Search<U32> search(forward, reverse);
        for (;;) {
          size_t i = next.fetch_add(1, std::memory_order_relaxed);
          if (i >= queries.size())
            break;
          answers[i] = search.run(queries[i].first, queries[i].second);
        }
      });
      // Compact worker arenas are destroyed before the wide fallback is built.
      // n <= 2^21 proves every simple path sum is strictly below 2^53.
      if (std::find(answers.begin(), answers.end(), INF) != answers.end()) {
        Search<U64> exact(forward, reverse);
        for (size_t i = 0; i < queries.size(); ++i)
          if (answers[i] == INF)
            answers[i] = exact.run(queries[i].first, queries[i].second);
      }
    }
    FILE *file = fopen((output + "/distances.txt").c_str(), "w");
    if (!file)
      throw std::runtime_error("open output");
    bool ok = true;
    for (U64 answer : answers)
      if (fprintf(file, "%lld\n",
                  answer == INF ? -1LL : static_cast<long long>(answer)) < 0)
        ok = false;
    if (fclose(file) || !ok)
      throw std::runtime_error("write output");
    return 0;
  } catch (const std::exception &error) {
    fprintf(stderr, "%s\n", error.what());
    return 1;
  }
}
