#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <fcntl.h>
#include <functional>
#include <string>
#include <string_view>
#include <sys/mman.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <unordered_map>
#include <utility>
#include <vector>

struct Group {
  std::vector<uint32_t> latencies;
  const char *hour_text;
  uint32_t endpoint_id;
  uint32_t errors = 0;
  uint8_t hour;
};

struct Aggregates {
  std::unordered_map<std::string_view, uint32_t> endpoint_ids;
  std::vector<std::string_view> endpoints;
  std::vector<std::array<int32_t, 72>> group_ids;
  std::vector<Group> groups;

  Aggregates() { endpoint_ids.reserve(2048); }

  uint32_t endpoint_id(std::string_view endpoint) {
    auto [it, inserted] = endpoint_ids.try_emplace(endpoint, endpoints.size());
    if (inserted) {
      endpoints.push_back(endpoint);
      group_ids.emplace_back();
      group_ids.back().fill(-1);
    }
    return it->second;
  }

  Group &group(uint32_t endpoint, uint8_t hour, const char *text) {
    int32_t &id = group_ids[endpoint][hour];
    if (id == -1) {
      id = groups.size();
      groups.push_back({{}, text, endpoint, 0, hour});
    }
    return groups[id];
  }
};

static void parse(const char *p, const char *end, Aggregates &result) {
  while (p < end) {
    const char *hour_text = p;
    const int month = (p[5] - '0') * 10 + p[6] - '0';
    const int day = (p[8] - '0') * 10 + p[9] - '0';
    const int hour_of_day = (p[11] - '0') * 10 + p[12] - '0';
    // The frozen spec fixes the span to Oct 30 18:00 through Nov 2 17:59 UTC.
    const int hour = (month == 10 ? day - 30 : day + 1) * 24 + hour_of_day - 18;
    p += 28;
    while (*p != ' ')
      ++p; // Method is ignored.
    const char *endpoint_start = ++p;
    while (*p != ' ')
      ++p;
    const std::string_view endpoint(endpoint_start, p - endpoint_start);
    ++p;
    const bool error = *p == '5';
    p += 4;
    uint32_t latency = 0;
    while (*p != '\n')
      latency = latency * 10 + static_cast<uint32_t>(*p++ - '0');
    ++p;
    Group &group = result.group(result.endpoint_id(endpoint), hour, hour_text);
    group.latencies.push_back(latency);
    group.errors += error;
  }
}

static void append_number(std::string &out, uint64_t value) {
  char digits[24];
  auto end = std::to_chars(digits, digits + sizeof(digits), value).ptr;
  out.append(digits, end);
}

int main(int argc, char **argv) {
  if (argc != 1 && argc != 3)
    return 2;
  const char *input_path = argc == 3 ? argv[1] : "/data/requests.log";
  const char *output_path = argc == 3 ? argv[2] : "/out/summary.txt";
  int fd = open(input_path, O_RDONLY);
  if (fd < 0)
    return 1;
  struct stat st;
  if (fstat(fd, &st) != 0)
    return 1;
  const size_t size = static_cast<size_t>(st.st_size);
  const char *data = size ? static_cast<const char *>(mmap(
                                nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0))
                          : nullptr;
  close(fd);
  if (size && data == MAP_FAILED)
    return 1;

  // Workers own all mutable state; only the read-only mapping is shared.
  const unsigned workers = size >= (8 << 20) ? 6 : 1;
  std::vector<Aggregates> partial(workers);
  std::vector<const char *> boundaries(workers + 1, data);
  if (size)
    boundaries.back() = data + size;
  for (unsigned i = 1; i < workers; ++i) {
    const char *p = data + size * i / workers;
    while (p < data + size && p[-1] != '\n')
      ++p;
    boundaries[i] = p;
  }
  std::vector<std::thread> threads;
  for (unsigned i = 1; i < workers; ++i)
    threads.emplace_back(parse, boundaries[i], boundaries[i + 1],
                         std::ref(partial[i]));
  parse(boundaries[0], boundaries[1], partial[0]);
  for (auto &thread : threads)
    thread.join();
  Aggregates &total = partial[0];
  for (unsigned i = 1; i < workers; ++i) {
    for (Group &source : partial[i].groups) {
      const auto endpoint =
          total.endpoint_id(partial[i].endpoints[source.endpoint_id]);
      Group &target = total.group(endpoint, source.hour, source.hour_text);
      if (target.latencies.empty())
        target.latencies = std::move(source.latencies);
      else
        target.latencies.insert(target.latencies.end(),
                                source.latencies.begin(),
                                source.latencies.end());
      target.errors += source.errors;
    }
  }
  std::vector<uint32_t> order(total.endpoints.size());
  for (uint32_t i = 0; i < order.size(); ++i)
    order[i] = i;
  std::sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) {
    return total.endpoints[a] < total.endpoints[b];
  });

  std::array<std::string, 72> output_hours;
  auto render = [&](unsigned worker) {
    for (unsigned hour = worker; hour < 72; hour += workers) {
      auto &buffer = output_hours[hour];
      buffer.reserve(std::min<size_t>(160 << 10, size / 72));
      for (uint32_t endpoint : order) {
        const int32_t id = total.group_ids[endpoint][hour];
        if (id == -1)
          continue;
        Group &group = total.groups[id];
        auto &values = group.latencies;
        const uint64_t count = values.size();
        if (count < 64)
          std::sort(values.begin(), values.end());
        else {
          // Each selected rank partitions the remaining higher ranks.
          auto first = values.begin();
          for (uint64_t pct : {50, 95, 99}) {
            auto rank = values.begin() + (pct * count + 99) / 100 - 1;
            std::nth_element(first, rank, values.end());
            first = rank + 1;
          }
        }
        buffer.append(group.hour_text, 13);
        buffer += ":00:00Z ";
        buffer.append(total.endpoints[group.endpoint_id]);
        buffer += ' ';
        append_number(buffer, count);
        for (uint64_t pct : {50, 95, 99}) {
          buffer += ' ';
          append_number(buffer, values[(pct * count + 99) / 100 - 1]);
        }
        buffer += ' ';
        append_number(buffer, group.errors);
        buffer += ' ';
        append_number(buffer,
                      static_cast<uint64_t>(group.errors) * 1000000 / count);
        buffer += '\n';
      }
    }
  };
  threads.clear();
  for (unsigned i = 1; i < workers; ++i)
    threads.emplace_back(render, i);
  render(0);
  for (auto &thread : threads)
    thread.join();
  FILE *output = fopen(output_path, "wb");
  if (!output)
    return 1;
  for (const auto &buffer : output_hours) {
    if (fwrite(buffer.data(), 1, buffer.size(), output) != buffer.size())
      return 1;
  }
  if (fclose(output) != 0)
    return 1;
  if (size)
    munmap(const_cast<char *>(data), size);
  return 0;
}
