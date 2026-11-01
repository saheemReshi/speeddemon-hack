#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <immintrin.h>
#include <span>
#include <string>
#include <string_view>
#include <sys/mman.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

using U32 = uint32_t;
using PID = uint16_t;
using U64 = uint64_t;
using Clock = std::chrono::steady_clock;

struct Mapping {
  const char *data = "";
  size_t size = 0;
  explicit Mapping(const std::string &path) {
    int fd = open(path.c_str(), O_RDONLY);
    struct stat st{};
    if (fd < 0 || fstat(fd, &st))
      std::exit(1);
    size = st.st_size;
    if (size)
      data = static_cast<const char *>(
          mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0));
    close(fd);
    if (size && data == MAP_FAILED)
      std::exit(1);
  }
  ~Mapping() {
    if (size)
      munmap(const_cast<char *>(data), size);
  }
};

inline unsigned lower(unsigned c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
inline bool word(unsigned c) {
  return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
}
inline bool tokenchar(char c) {
  return word(lower(static_cast<unsigned char>(c)));
}

// Keys are exact; long tokens additionally compare every suffix byte.
struct Key {
  std::array<U64, 2> v{};
  const char *source = nullptr;
  U32 length = 0;
};

__attribute__((always_inline)) inline Key key(const char *s, U32 n,
                                              const char *end) {
  Key k;
  k.length = n;
  k.source = s;
  if (end - s >= 16) {
    auto x = _mm_or_si128(_mm_loadu_si128(reinterpret_cast<const __m128i *>(s)),
                          _mm_set1_epi8(32));
    k.v[0] = _mm_cvtsi128_si64(x);
    if (n < 8)
      k.v[0] &= (U64(1) << (n * 8)) - 1;
    if (n > 8) {
      k.v[1] = _mm_extract_epi64(x, 1);
      if (n < 16)
        k.v[1] &= (U64(1) << ((n - 8) * 8)) - 1;
    }

  } else {
    for (U32 i = 0; i < n; ++i)
      k.v[i / 8] |= U64(lower(static_cast<unsigned char>(s[i])))
                    << ((i % 8) * 8);
  }
  return k;
}

U64 hashkey(const Key &k) {
  U64 h = k.v[0] * 0x9e3779b185ebca87ULL ^
          std::rotl(k.v[1] * 0xc2b2ae3d27d4eb4fULL, 23) ^ k.length;
  return h ^ (h >> 32);
}

// Fold only letters: digit tests use raw bytes so control bytes stay
// separators.
template <class F> void tokenize(const char *p, const char *end, F &&f) {
  // Classify 32 original bytes in parallel. High-bit bytes are separators.
  // A token can span blocks, so keep its first byte until its end is known.
  const char *open = nullptr;
  const __m256i ascii_case = _mm256_set1_epi8(32);
  const __m256i amin = _mm256_set1_epi8('a' - 1);
  const __m256i amax = _mm256_set1_epi8('z' + 1);
  const __m256i nmin = _mm256_set1_epi8('0' - 1);
  const __m256i nmax = _mm256_set1_epi8('9' + 1);
  while (end - p >= 32) {
    __m256i raw = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(p));
    __m256i x = _mm256_or_si256(raw, ascii_case);
    __m256i az = _mm256_and_si256(_mm256_cmpgt_epi8(x, amin),
                                  _mm256_cmpgt_epi8(amax, x));
    __m256i digit = _mm256_and_si256(_mm256_cmpgt_epi8(raw, nmin),
                                     _mm256_cmpgt_epi8(nmax, raw));
    U32 mask = U32(_mm256_movemask_epi8(_mm256_or_si256(az, digit)));
    if (open) {
      U32 run = std::countr_one(mask);
      if (run == 32) {
        p += 32;
        continue;
      }
      f(open, U32(p + run - open));
      open = nullptr;
      mask &= ~((U32(1) << run) - 1); // keep only bits after initial token
    }
    while (mask) {
      U32 begin = std::countr_zero(mask);
      U32 len = std::countr_one(mask >> begin);
      if (begin + len == 32) {
        open = p + begin;
        break;
      }
      f(p + begin, len);
      mask &= ~((U32(1) << (begin + len)) - 1);
    }
    p += 32;
  }
  if (open) {
    const char *s = open;
    while (p < end && tokenchar(*p)) ++p;
    f(s, U32(p - s));
  }
  while (p < end) {
    while (p < end && !tokenchar(*p)) ++p;
    const char *s = p;
    while (p < end && tokenchar(*p)) ++p;
    if (p != s) f(s, U32(p - s));
  }
}

struct Term {
  std::string_view text;
  U32 usage = 0, sample = 0;
  bool boolean = false;
  int bitmap = -1, head = -1, posting = -1;
};

bool tail_equal(const Key &k, std::string_view text) {
  for (U32 i = 16; i < k.length; ++i)
    if (lower(static_cast<unsigned char>(k.source[i])) != unsigned(text[i]))
      return false;
  return true;
}

struct Dictionary {
  struct Slot {
    U64 lo = 0, hi = 0;
    U32 id = 0, length = 0;
  };
  std::vector<Slot> slots;
  std::vector<Term> &terms;
  U32 count = 0;
  explicit Dictionary(std::vector<Term> &t, size_t capacity) : terms(t) {
    slots.resize(std::bit_ceil(std::max<size_t>(16, capacity)));
  }
  U32 find(const Key &k) const {
    size_t i = hashkey(k) & (slots.size() - 1);
    while (slots[i].id) {
      const auto &s = slots[i];
      if (s.lo == k.v[0] && s.hi == k.v[1] && s.length == k.length &&
          tail_equal(k, terms[s.id - 1].text))
        return s.id;
      i = (i + 1) & (slots.size() - 1);
    }
    return 0;
  }
  void insert(U32 id) {
    if ((count + 1) * 10 > slots.size() * 7) {
      auto old = std::move(slots);
      slots.resize(old.size() * 2);
      count = 0;
      for (const auto &s : old)
        if (s.id)
          insert(s.id - 1);
    }
    const auto &text = terms[id].text;
    auto k = key(text.data(), text.size(), text.data() + text.size());
    size_t i = hashkey(k) & (slots.size() - 1);
    while (slots[i].id)
      i = (i + 1) & (slots.size() - 1);
    slots[i] = {k.v[0], k.v[1], id + 1, k.length};
    ++count;
  }
};

struct Query {
  std::array<U32, 8> terms{};
  std::array<char, 7> ops{};
  uint8_t length = 0, anchor = 0;
  bool phrase = false;
  int next = -1;
};

struct Result {
  U64 count = 0, sum = 0;
  std::array<U32, 10> first{};
  void add(U32 d) {
    if (count < 10)
      first[count] = d;
    ++count;
    sum += d;
  }
  void append(const Result &r) {
    U64 pos = count;
    for (U64 i = 0; i < r.count && i < 10 && pos < 10; ++i)
      first[pos++] = r.first[i];
    count += r.count;
    sum += r.sum;
  }
};

U32 docid(const char *p) {
  U32 n = 0;
  while (*p >= '0' && *p <= '9')
    n = n * 10 + (*p++ - '0');
  return n;
}

// Inline lists avoid arena traffic for one or two documents. Larger sorted
// lists live in a worker-owned arena; the descriptor remains eight bytes.
struct Posting {
  std::array<PID, 2> address{};
  U32 count = 0;
  U32 offset() const { return std::bit_cast<U32>(address); }
  void offset(U32 value) { address = std::bit_cast<std::array<PID, 2>>(value); }
};
// Reserve address space, commit only touched pages. Free lists recycle blocks
// by capacity; no locks or individual heap allocations are needed.
struct Arena {
  static constexpr size_t bytes = size_t(1) << 30;
  PID *data = nullptr;
  U32 used = 2;
  std::array<U32, 30> free{};
  void init() {
    data = static_cast<PID *>(mmap(nullptr, bytes, PROT_READ | PROT_WRITE,
                                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE,
                                   -1, 0));
    if (data == MAP_FAILED)
      std::exit(1);
  }
  U32 allocate(U32 capacity) {
    U32 c = std::countr_zero(capacity), p = free[c];
    if (p)
      memcpy(&free[c], data + p, 4);
    else {
      p = used;
      used += capacity;
      if (size_t(used) * sizeof(PID) > bytes)
        std::exit(1);
    }
    return p;
  }
  void push(Posting &p, PID d) {
    if (p.count < 2) {
      p.address[p.count++] = d;
      return;
    }
    if (p.count == 2) {
      U32 offset = allocate(4);
      memcpy(data + offset, p.address.data(), 4);
      p.offset(offset);
    } else if (std::has_single_bit(p.count)) {
      U32 old = p.offset(), c = std::countr_zero(p.count);
      p.offset(allocate(p.count * 2));
      memcpy(data + p.offset(), data + old, size_t(p.count) * sizeof(PID));
      memcpy(data + old, &free[c], 4);
      free[c] = old;
    }
    data[p.offset() + p.count++] = d;
  }
};

// Partitions contain at most 65,472 documents. UINT16_MAX is reserved for
// unseen stamps; bitmap boundaries and bases are multiples of 64.
struct Partition {
  const char *begin = nullptr, *end = nullptr;
  U32 base = 0, limit = 0;
  size_t words = 0;
  std::vector<Posting> lists;
  Arena arena;
  std::span<const PID> list(int id) const {
    const auto &p = lists[id];
    return {p.count <= 2 ? p.address.data() : arena.data + p.offset(), p.count};
  }
  std::vector<U64> bits;
  std::vector<std::pair<U32, U32>> phrase_hits;
  const U64 *bitmap(int id) const { return bits.data() + size_t(id) * words; }
  U64 *bitmap(int id) { return bits.data() + size_t(id) * words; }
};

inline bool contains(const U64 *bits, U32 d) {
  return bits[d / 64] & (U64(1) << (d % 64));
}

void sparse_op(std::span<const PID> a, std::span<const PID> b, char op,
               std::vector<PID> &r) {
  r.clear();
  if (op == 'O') {
    std::set_union(a.begin(), a.end(), b.begin(), b.end(),
                   std::back_inserter(r));
    return;
  }
  if (op == 'A' && a.size() > b.size() * 16) {
    for (U32 d : b)
      if (std::binary_search(a.begin(), a.end(), d))
        r.push_back(d);
  } else if (b.size() > a.size() * 16) {
    for (U32 d : a)
      if (std::binary_search(b.begin(), b.end(), d) == (op == 'A'))
        r.push_back(d);
  } else if (op == 'A')
    std::set_intersection(a.begin(), a.end(), b.begin(), b.end(),
                          std::back_inserter(r));
  else
    std::set_difference(a.begin(), a.end(), b.begin(), b.end(),
                        std::back_inserter(r));
}

struct Scratch {
  std::vector<PID> a, b;
  std::vector<U64> bits;
};

Result boolean(const Query &q, const std::vector<Term> &terms,
               const Partition &p, Scratch &s) {
  if (!p.words)
    return {};
  bool dense = terms[q.terms[0]].bitmap >= 0;
  if (dense) {
    const U64 *v = p.bitmap(terms[q.terms[0]].bitmap);
    s.bits.assign(v, v + p.words);
  } else {
    auto list = p.list(terms[q.terms[0]].posting);
    s.a.assign(list.begin(), list.end());
  }
  for (U32 i = 1; i < q.length; ++i) {
    U32 t = q.terms[i];
    char op = q.ops[i - 1];
    int bi = terms[t].bitmap;
    const auto list =
        bi < 0 ? p.list(terms[t].posting) : std::span<const PID>{};
    if (!dense && bi < 0) {
      sparse_op(s.a, list, op, s.b);
      s.a.swap(s.b);
      if (op == 'O' && s.a.size() > p.words * 2) {
        s.bits.assign(p.words, 0);
        for (U32 d : s.a)
          s.bits[d / 64] |= U64(1) << (d % 64);
        dense = true;
      }
    } else if (!dense) {
      const U64 *b = p.bitmap(bi);
      if (op == 'O') {
        s.bits.assign(b, b + p.words);
        for (U32 d : s.a)
          s.bits[d / 64] |= U64(1) << (d % 64);
        dense = true;
      } else {
        size_t n = 0;
        for (U32 d : s.a)
          if (contains(b, d) == (op == 'A'))
            s.a[n++] = d;
        s.a.resize(n);
      }
    } else if (bi >= 0) {
      const U64 *b = p.bitmap(bi);
      if (op == 'A')
        for (size_t j = 0; j < p.words; ++j)
          s.bits[j] &= b[j];
      else if (op == 'O')
        for (size_t j = 0; j < p.words; ++j)
          s.bits[j] |= b[j];
      else
        for (size_t j = 0; j < p.words; ++j)
          s.bits[j] &= ~b[j];
    } else if (op == 'A') {
      s.a.clear();
      for (U32 d : list)
        if (contains(s.bits.data(), d))
          s.a.push_back(d);
      dense = false;
    } else {
      for (U32 d : list) {
        U64 mask = U64(1) << (d % 64);
        if (op == 'O')
          s.bits[d / 64] |= mask;
        else
          s.bits[d / 64] &= ~mask;
      }
    }
  }
  Result r;
  if (dense) {
    for (size_t j = 0; j < p.words; ++j) {
      U64 b = s.bits[j];
      if (!b) continue;
      unsigned n = std::popcount(b);
      if (n >= 10) {
        if (r.count < 10) {
          U64 first = b;
          U32 dest = U32(r.count);
          while (first && dest < 10) {
            r.first[dest++] = p.base + U32(j * 64 + std::countr_zero(first));
            first &= first - 1;
          }
        }
        // Sum of every set bit's position, calculated with six popcounts.
        U64 weighted = U64(std::popcount(b & 0xAAAAAAAAAAAAAAAAULL)) +
           2ULL * std::popcount(b & 0xCCCCCCCCCCCCCCCCULL) +
           4ULL * std::popcount(b & 0xF0F0F0F0F0F0F0F0ULL) +
           8ULL * std::popcount(b & 0xFF00FF00FF00FF00ULL) +
           16ULL * std::popcount(b & 0xFFFF0000FFFF0000ULL) +
           32ULL * std::popcount(b & 0xFFFFFFFF00000000ULL);
        r.count += n;
        r.sum += (U64(p.base) + U64(j) * 64) * n + weighted;
      } else {
        while (b) {
          r.add(p.base + U32(j * 64 + std::countr_zero(b)));
          b &= b - 1;
        }
      }
    }
  } else
    for (U32 d : s.a)
      r.add(p.base + d);
  return r;
}

void number(std::string &s, U64 n) {
  char b[24];
  auto r = std::to_chars(b, b + sizeof b, n);
  s.append(b, r.ptr);
}

template <class F> void parallel(U32 n, F &&f) {
  std::vector<std::thread> workers;
  for (U32 i = 1; i < n; ++i)
    workers.emplace_back([&, i] { f(i); });
  f(0);
  for (auto &t : workers)
    t.join();
}

int main(int argc, char **argv) {
  std::string data = "/data", out = "/out";
  for (int i = 1; i + 1 < argc; i += 2) {
    if (std::string(argv[i]) == "--data")
      data = argv[i + 1];
    else if (std::string(argv[i]) == "--out")
      out = argv[i + 1];
    else
      return 1;
  }
  auto start = Clock::now();
  Mapping queryfile(data + "/queries.txt");
  std::vector<Term> terms;
  terms.reserve(queryfile.size / 10);
  Dictionary dict(terms, queryfile.size / 16 + 16);
  std::vector<Query> queries;
  queries.reserve(
      std::count(queryfile.data, queryfile.data + queryfile.size, '\n'));
  const char *p = queryfile.data, *qe = p + queryfile.size;
  while (p < qe) {
    Query q;
    q.phrase = *p == 'P';
    p += q.phrase ? 8 : 5;
    while (p < qe && *p != '\n' && *p != '"') {
      const char *s = p;
      while (p < qe && word(*p))
        ++p;
      Key k = key(s, p - s, qe);
      U32 t = dict.find(k);
      if (!t) {
        t = terms.size() + 1;
        Term term;
        term.text = std::string_view(s, p - s);
        terms.push_back(std::move(term));
        dict.insert(t - 1);
      }
      q.terms[q.length++] = t - 1;
      ++terms[t - 1].usage;
      terms[t - 1].boolean |= !q.phrase;
      if (!q.phrase && p < qe && *p == ' ') {
        q.ops[q.length - 1] = *++p;
        while (*p != ' ')
          ++p;
        ++p;
      } else if (p < qe && *p == ' ')
        ++p;
    }
    if (p < qe && *p == '"')
      ++p;
    if (p < qe && *p == '\n')
      ++p;
    queries.push_back(q);
  }
  auto parsed = Clock::now();
  Mapping docs(data + "/docs.txt");
  const char *end = docs.data + docs.size;
  p = docs.data;
  // Sampling selects storage and anchors only; it never excludes a term or
  // match.
  U32 sampledocs = 0;
  std::vector<U32> seen(terms.size(), UINT32_MAX);
  size_t samplebytes = 1 << 20;

  while (p < end && size_t(p - docs.data) < samplebytes) {
    while (*p != '\t')
      ++p;
    const char *s = ++p;
    const char *e = static_cast<const char *>(memchr(s, '\n', end - s));
    tokenize(s, e, [&](const char *t, U32 n) {
      U32 id = dict.find(key(t, n, end));
      if (id && seen[id - 1] != sampledocs) {
        seen[id - 1] = sampledocs;
        ++terms[id - 1].sample;
      }
    });
    ++sampledocs;
    p = e + 1;
  }
  U32 bitmaps = 0, postings = 0;

  for (auto &t : terms)
    if (t.boolean && t.sample > std::max<U32>(4, sampledocs / 32))
      t.bitmap = bitmaps++;
  for (auto &t : terms)
    if (t.boolean && t.bitmap < 0)
      t.posting = postings++;
  U32 pair_count=0;
  struct PairSlot { U64 key=0; int head=-1; };
  std::vector<PairSlot> pairs(std::bit_ceil(std::max<U32>(16, U32(queries.size()*2))));
  std::vector<uint8_t> pair_left(terms.size());
  constexpr U64 pair_mul=0x9e3779b185ebca87ULL;
  auto pairhash = [&](U64 k) { return U32((k ^ (k >> 29)) * pair_mul); };
  for (U32 qi=0;qi<queries.size();++qi) {
    auto &q=queries[qi];
    if(!q.phrase) continue;
    // Find its rarest single-word anchor.
    for (U32 j=1;j<q.length;++j) {
      const auto &a=terms[q.terms[q.anchor]], &b=terms[q.terms[j]];
      if(b.sample<a.sample || (b.sample==a.sample && b.usage<a.usage)) q.anchor=j;
    }
    if (terms[q.terms[q.anchor]].sample <= 8) {
      auto &t=terms[q.terms[q.anchor]];
      q.next=t.head;
      t.head=qi;
      continue;
    }
    // High-frequency single anchors use the rarest adjacent pair instead.
    U64 best=UINT64_MAX;
    for (U32 j=0;j+1<q.length;++j) {
      U64 x=terms[q.terms[j]].sample, y=terms[q.terms[j+1]].sample;
      U64 score=(std::min(x,y)<<32) | std::max(x,y);
      if (score<best) { best=score; q.anchor=j; }
    }
    U32 u=q.terms[q.anchor], v=q.terms[q.anchor+1];
    U64 k=(U64(u+1)<<32)|U64(v+1);
    U32 idx=pairhash(k)&(pairs.size()-1);
    while(pairs[idx].key && pairs[idx].key!=k) idx=(idx+1)&(pairs.size()-1);
    q.next=pairs[idx].head;
    pairs[idx]={k,int(qi)};
    pair_left[u]=1;
    ++pair_count;
  }
  std::fill(dict.slots.begin(), dict.slots.end(), Dictionary::Slot{});
  dict.count = 0;
  for (U32 t = 0; t < terms.size(); ++t)
    dict.insert(t);
  struct Hot {
    int bitmap, head, posting;
  };
  std::vector<Hot> hot;
  hot.reserve(terms.size());
  for (auto &t : terms)
    hot.push_back({t.bitmap, t.head, t.posting});
  std::vector<U32>().swap(seen);
  auto planned = Clock::now();
  U32 workers = 12;
  if (const char *v = std::getenv("B_THREADS"))
    workers = std::clamp(std::atoi(v), 1, 32);
  if (docs.size < (1 << 20))
    workers = 1;

  U32 ndocs = 0;
  if (docs.size) {
    const char *last = end - 1;
    while (last > docs.data && last[-1] != '\n')
      --last;
    ndocs = docid(last) + 1;
  }
  U32 partcount = std::max<U32>(workers, (ndocs + 65471) / 65472);
  U32 chunk =
      std::max<U32>(64, ((ndocs + partcount * 64 - 1) / (partcount * 64)) * 64);
  partcount = std::max<U32>(1, (ndocs + chunk - 1) / chunk);
  auto finddoc = [&](U32 target) {
    size_t lo = 0, hi = docs.size;
    while (lo < hi) {
      size_t mid = lo + (hi - lo) / 2;
      const char *next =
          mid ? static_cast<const char *>(
                    memchr(docs.data + mid - 1, '\n', docs.size - mid + 1)) +
                    1
              : docs.data;
      U32 id = next == end ? ndocs : docid(next);
      if (id == target)
        return next;
      if (id < target)
        lo = next - docs.data;
      else
        hi = mid;
    }
    return docs.data + lo;
  };
  std::vector<const char *> boundaries(partcount + 1);
  boundaries[0] = docs.data;
  boundaries[partcount] = end;
  for (U32 i = 1; i < partcount; ++i)
    boundaries[i] = finddoc(i * chunk);
  std::vector<Partition> parts(partcount);
  parallel(workers, [&](U32 worker) {
    for (U32 pi = worker; pi < partcount; pi += workers) {
      auto &part = parts[pi];
      part.begin = boundaries[pi];
      part.end = boundaries[pi + 1];
      part.base = part.begin == end ? ndocs : docid(part.begin);
      part.limit = part.end == end ? ndocs : docid(part.end);
      part.words = (part.limit - part.base + 63) / 64;
      part.lists.resize(postings);
      part.arena.init();
      part.bits.resize(size_t(bitmaps) * part.words);
      std::vector<PID> last(terms.size(), UINT16_MAX),
          matched(queries.size(), UINT16_MAX);
      // Stage independent lookups before consuming dependent posting metadata.
      // The complete ID stream is retained only for the current document.
      std::array<Key, 32> keys;
      std::array<U32, 32> batch_ids;
      std::vector<U32> ids;
      ids.reserve(2048);
      std::vector<std::pair<U32, int>> anchors;
      std::vector<U32> pair_candidates;
      const char *s = part.begin;
      U32 d = part.base;
      while (s < part.end) {
        while (*s != '\t')
          ++s;
        ++s;
        const char *finish =
            static_cast<const char *>(memchr(s, '\n', part.end - s));

        ids.clear();
        anchors.clear();
        pair_candidates.clear();
        U32 batch_count = 0;
        auto flush = [&]() {
          for (U32 i = 0; i < batch_count; ++i) {
            U32 id = dict.find(keys[i]);
            batch_ids[i] = id ? id - 1 : UINT32_MAX;
            if (id) {
              __builtin_prefetch(&hot[id - 1], 0, 3);
              __builtin_prefetch(&last[id - 1], 1, 3);
            }
          }
          for (U32 i = 0; i < batch_count; ++i) {
            U32 id = batch_ids[i];
            U32 position = ids.size();
            ids.push_back(id);
            if (id == UINT32_MAX)
              continue;
            const auto &term = hot[id];
            if ((term.bitmap >= 0 || term.posting >= 0) &&
                last[id] != d - part.base) {
              last[id] = PID(d - part.base);
              if (term.bitmap >= 0)
                part.bitmap(term.bitmap)[(d - part.base) / 64] |=
                    U64(1) << ((d - part.base) % 64);
              else
                part.arena.push(part.lists[term.posting], PID(d - part.base));
            }
            if (term.head >= 0)
              anchors.emplace_back(position, term.head);
            if (pair_left[id])
              pair_candidates.push_back(position);
          }
          batch_count = 0;
        };
        tokenize(s, finish, [&](const char *t, U32 n) {
          auto k = key(t, n, end);
          keys[batch_count++] = k;
          __builtin_prefetch(&dict.slots[hashkey(k) & (dict.slots.size() - 1)],
                             0, 3);
          if (batch_count == 32)
            flush();
        });
        flush();
        // Unknown tokens keep their own positions (UINT32_MAX), so adjacency
        // cannot collapse across unindexed words or document boundaries.
        for (auto [position, head] : anchors)
          for (int qi = head; qi >= 0; qi = queries[qi].next) {
            const auto &q = queries[qi];
            if (matched[qi] != d - part.base && position >= q.anchor &&
                position - q.anchor + q.length <= ids.size() &&
                std::equal(q.terms.begin(), q.terms.begin() + q.length,
                           ids.begin() + position - q.anchor)) {
              matched[qi] = PID(d - part.base);
              part.phrase_hits.emplace_back(qi, d);
            }
          }
        for (U32 i : pair_candidates) {
          if (i+1 == ids.size()) continue;
          U32 u=ids[i],v=ids[i+1];
          if (v==UINT32_MAX) continue;
          U64 k=(U64(u+1)<<32)|U64(v+1);
          U32 slot=pairhash(k)&(pairs.size()-1);
          while (pairs[slot].key && pairs[slot].key!=k)
            slot=(slot+1)&(pairs.size()-1);
          if (pairs[slot].key!=k) continue;
          for (int qi=pairs[slot].head;qi>=0;qi=queries[qi].next) {
            const auto &q=queries[qi];
            if (matched[qi]!=d-part.base && i>=q.anchor &&
                i-q.anchor+q.length <= ids.size() &&
                std::equal(q.terms.begin(),q.terms.begin()+q.length,
                           ids.begin()+i-q.anchor)) {
              matched[qi]=PID(d-part.base);
              part.phrase_hits.emplace_back(qi,d);
            }
          }
        }
        s = finish + 1;
        ++d;
      }
    }
  });
  auto indexed = Clock::now();
  std::vector<Result> answers(queries.size());
  for (auto &part : parts)
    for (auto [qi, d] : part.phrase_hits)
      answers[qi].add(d);
  parallel(workers, [&](U32 worker) {
    Scratch scratch;
    for (U32 qi = worker; qi < queries.size(); qi += workers) {
      if (queries[qi].phrase)
        continue;
      Result r;
      for (auto &part : parts) {
        r.append(boolean(queries[qi], terms, part, scratch));
      }
      answers[qi] = r;
    }
  });
  auto queried = Clock::now();
  std::string output;
  output.reserve(queries.size() * 40);
  for (auto &r : answers) {
    number(output, r.count);
    output += ' ';
    number(output, r.sum);
    for (size_t i = 0; i < std::min<U64>(10, r.count); ++i) {
      output += ' ';
      number(output, r.first[i]);
    }
    output += '\n';
  }
  std::filesystem::create_directories(out);
  std::ofstream file(out + "/results.txt", std::ios::binary);
  file.write(output.data(), output.size());
  file.close();
  if (!file)
    return 1;
  if (std::getenv("B_PROFILE")) {
    U64 pairs = 0, arena_bytes = 0;
    for (const auto &part : parts)
      arena_bytes += size_t(part.arena.used) * sizeof(PID);
    for (auto &part : parts)
      for (auto &v : part.lists)
        pairs += v.count;
    auto ms = [](auto d) {
      return std::chrono::duration<double, std::milli>(d).count();
    };
    std::fprintf(
        stderr,
        "parse=%.3f plan=%.3f scan_phrase=%.3f boolean=%.3f output=%.3f "
        "total=%.3f ms terms=%zu pairs=%llu bitmaps=%u workers=%u "
        "arena_bytes=%llu\n",
        ms(parsed - start), ms(planned - parsed), ms(indexed - planned),
        ms(queried - indexed), ms(Clock::now() - queried),
        ms(Clock::now() - start), terms.size(),
        static_cast<unsigned long long>(pairs), bitmaps, workers,
        (unsigned long long)arena_bytes);
  }
  // Output is closed above. The kernel reclaims the index without allocator
  // teardown.
  std::fflush(stderr);
  std::_Exit(0);
}
