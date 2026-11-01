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
#include <unistd.h>
#include <utility>
#include <vector>

using U32 = uint32_t;
using U64 = uint64_t;
constexpr U64 INF = std::numeric_limits<U64>::max() / 4;

struct Input {
  const char *begin, *p, *end;
  size_t size;
  explicit Input(const std::string &path) {
    int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0)
      throw std::runtime_error("open " + path);
    struct stat st;
    if (fstat(fd, &st) != 0) {
      close(fd);
      throw std::runtime_error("fstat " + path);
    }
    size = st.st_size;
    void *mem =
        size ? mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0) : nullptr;
    close(fd);
    if (mem == MAP_FAILED)
      throw std::runtime_error("mmap " + path);
    begin = p = static_cast<const char *>(mem);
    end = size ? begin + size : begin;
  }
  ~Input() {
    if (size)
      munmap(const_cast<char *>(begin), size);
  }
  U32 number() {
    // The frozen grammar guarantees a single delimiter after every integer.
    U32 x = 0;
    while (*p >= '0' && *p <= '9')
      x = x * 10 + (*p++ - '0');
    ++p;
    return x;
  }
};

struct Edge {
  U32 v, w;
};
struct Arc {
  U32 u, v, w;
};
struct Graph {
  std::vector<U32> offsets;
  std::vector<Edge> edges;
};

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
  const Graph &g, &r;
  std::array<std::vector<Word>, 2> dist;
  std::array<RadixHeap<Word>, 2> heap;
  U64 generation = 0;
  // 11 high bits tag the query; 21/53 low bits hold an exact distance.
  static constexpr U64 LIMIT = U64{1} << (sizeof(Word) * 8 - 11);
  Search(const Graph &g, const Graph &r) : g(g), r(r) {
    for (auto &d : dist)
      d.resize(g.offsets.size() - 1);
  }
  U64 get(int side, U32 v) const {
    U64 word = dist[side][v];
    // Older generations compare smaller; a wrapped generation clears the
    // arrays.
    return word >= generation ? word & (LIMIT - 1) : INF;
  }
  void put(int side, U32 v, U64 d) {
    dist[side][v] = static_cast<Word>(generation + d);
    heap[side].push(static_cast<Word>(d), v);
  }
  U64 run(U32 s, U32 t) {
    generation = (generation + LIMIT) & std::numeric_limits<Word>::max();
    if (!generation) {
      for (auto &d : dist)
        std::fill(d.begin(), d.end(), 0);
      generation = LIMIT;
    }
    for (auto &h : heap)
      h.clear();
    if (s == t)
      return 0;
    if (g.offsets[s] == g.offsets[s + 1] || r.offsets[t] == r.offsets[t + 1])
      return INF;
    put(0, s, 0);
    put(1, t, 0);
    U64 best = INF;
    bool tie = false;
    while (!heap[0].empty() && !heap[1].empty()) {
      // Both minima are lower bounds, even when an entry is stale. Meeting
      // alone is insufficient: stop only when no shorter complete path can
      // remain.
      if (U64(heap[0].top().first) + heap[1].top().first >= best)
        break;
      int side = heap[0].size() == heap[1].size()
                     ? (tie = !tie)
                     : heap[0].size() > heap[1].size();
      auto [d, u] = heap[side].pop();
      if (d != get(side, u))
        continue;
      const auto &graph = side ? r : g;
      for (U32 i = graph.offsets[u]; i < graph.offsets[u + 1]; ++i) {
        auto [v, w] = graph.edges[i];
        U64 nd = U64(d) + w;
        if (nd >= LIMIT)
          continue;
        if (nd < get(side, v)) {
          put(side, v, nd);
          U64 other = get(1 - side, v);
          if (other != INF)
            best = std::min(best, nd + other);
        }
      }
    }
    // With non-negative weights, every prefix of a path shorter than LIMIT is
    // also below LIMIT. Such answers are exact; all others require the
    // fallback.
    return best < LIMIT ? best : INF;
  }
};

void parallel_queries(const Graph &g, const Graph &r,
                      const std::vector<std::pair<U32, U32>> &queries,
                      std::vector<U64> &answers, int workers) {
  std::atomic<size_t> next{0};
  auto work = [&]() {
    Search<U32> search(g, r);
    for (;;) {
      size_t i = next.fetch_add(1, std::memory_order_relaxed);
      if (i >= queries.size())
        break;
      answers[i] = search.run(queries[i].first, queries[i].second);
    }
  };
  std::vector<std::thread> threads;
  for (int i = 1; i < workers; ++i)
    threads.emplace_back(work);
  work();
  for (auto &t : threads)
    t.join();
  // Joining releases the compact worker arrays before any 64-bit allocation.
}

int main(int argc, char **argv) {
  try {
    std::string data = argc > 1 ? argv[1] : "/data";
    std::string out = argc > 2 ? argv[2] : "/out";
    Graph g, r;
    {
      Input input(data + "/graph.txt");
      U32 n = input.number(), m = input.number();
      // The frozen full graph has 1,740,512 nodes. This also proves that every
      // possible shortest path, (n-1)*UINT32_MAX, fits the fallback's 53 bits.
      if (n > (1U << 21))
        throw std::runtime_error("graph exceeds specified size");
      g.offsets.resize(n + 1);
      r.offsets.resize(n + 1);
      std::vector<Arc> arcs(m);
      for (auto &a : arcs) {
        a = {input.number(), input.number(), input.number()};
        ++g.offsets[a.u + 1];
        ++r.offsets[a.v + 1];
      }
      for (U32 u = 0; u < n; ++u) {
        g.offsets[u + 1] += g.offsets[u];
        r.offsets[u + 1] += r.offsets[u];
      }
      g.edges.resize(m);
      r.edges.resize(m);
      auto gc = g.offsets, rc = r.offsets;
      for (auto a : arcs) {
        g.edges[gc[a.u]++] = {a.v, a.w};
        r.edges[rc[a.v]++] = {a.u, a.w};
      }
    }
    std::vector<std::pair<U32, U32>> queries;
    {
      Input input(data + "/queries.txt");
      while (input.p != input.end) {
        U32 s = input.number(), t = input.number();
        queries.emplace_back(s, t);
      }
    }
    std::vector<U64> answers(queries.size());
    if (!queries.empty()) {
      const char *setting = std::getenv("D_THREADS");
      int workers = setting ? std::clamp(std::stoi(setting), 1, 12) : 6;
      workers = std::min(workers, static_cast<int>(queries.size()));
      parallel_queries(g, r, queries, answers, workers);
      if (std::find(answers.begin(), answers.end(), INF) != answers.end()) {
        Search<U64> exact(g, r);
        for (size_t i = 0; i < queries.size(); ++i)
          if (answers[i] == INF)
            answers[i] = exact.run(queries[i].first, queries[i].second);
      }
    }
    FILE *file = fopen((out + "/distances.txt").c_str(), "w");
    if (!file)
      throw std::runtime_error("open output");
    bool ok = true;
    for (U64 answer : answers)
      if (fprintf(file, "%lld\n",
                  answer == INF ? -1LL : static_cast<long long>(answer)) < 0)
        ok = false;
    if (fclose(file) != 0 || !ok)
      throw std::runtime_error("write output");
    return 0;
  } catch (const std::exception &e) {
    fprintf(stderr, "%s\n", e.what());
    return 1;
  }
}
