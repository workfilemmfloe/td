#include <cstdio>

#include <benchmark/benchmark.h>
#include <td/utils/Random.h>
#include <td/utils/FlatHashMap.h>
#include <unordered_map>
#include <map>
#include <absl/container/flat_hash_map.h>
#include <absl/hash/hash.h>
#include <folly/container/F14Map.h>
#include "td/utils/Time.h"
#include "td/utils/logging.h"
#include "td/utils/format.h"
#include "td/utils/Hash.h"

template <class TableT>
void reserve(TableT &table, size_t size) {
  table.reserve(size);
}
template <class A, class B>
void reserve(std::map<A, B> &table, size_t size) {
}

template <class KeyT, class ValueT>
class NoOpTable {
 public:
  using key_type = KeyT;
  using value_type = std::pair<const KeyT, ValueT>;
  template <class It>
  NoOpTable(It begin, It end) {
  }

  ValueT &operator[](const KeyT &) const {
    static ValueT dummy;
    return dummy;
  }

  KeyT find(const KeyT &key) const {
    return key;
  }
};

template <class KeyT, class ValueT>
class VectorTable {
 public:
  using key_type = KeyT;
  using value_type = std::pair<const KeyT, ValueT>;
  template <class It>
  VectorTable(It begin, It end) : table_(begin, end) {
  }

  ValueT &operator[](const KeyT &needle) {
    auto it = find(needle);
    if (it == table_.end()) {
      table_.emplace_back(needle, ValueT{});
      return table_.back().second;
    }
    return it->second;
  }
  auto find(const KeyT &needle) {
    return std::find_if(table_.begin(), table_.end(), [&](auto &key) {
      return key.first == needle;
    });
  }

 private:
  using KeyValue = value_type;
  std::vector<KeyValue> table_;
};

template <class KeyT, class ValueT>
class SortedVectorTable {
 public:
  using key_type = KeyT;
  using value_type = std::pair<KeyT, ValueT>;
  template <class It>
  SortedVectorTable(It begin, It end) : table_(begin, end) {
    std::sort(table_.begin(), table_.end());
  }

  ValueT &operator[](const KeyT &needle) {
    auto it = std::lower_bound(table_.begin(), table_.end(), needle, [](auto l, auto r) {
      return l.first < r;
    });
    if (it == table_.end() || it->first != needle) {
      it = table_.insert(it, {needle, ValueT{}});
    }
    return it->second;
  }

  auto find(const KeyT &needle) {
    auto it = std::lower_bound(table_.begin(), table_.end(), needle, [](auto l, auto r) {
      return l.first < r;
    });
    if (it != table_.end() && it->first == needle) {
      return it;
    }
    return table_.end();
  }

 private:
  using KeyValue = value_type;
  std::vector<KeyValue> table_;
};

template <class KeyT, class ValueT, class HashT = td::Hash<KeyT>>
class SimpleHashTable {
 public:
  using key_type = KeyT;
  using value_type = std::pair<KeyT, ValueT>;
  template <class It>
  SimpleHashTable(It begin, It end) {
    nodes_.resize((end - begin) * 2);
    for (; begin != end; ++begin) {
      insert(begin->first, begin->second);
    }
  }

  ValueT &operator[](const KeyT &needle) {
    UNREACHABLE();
  }

  ValueT *find(const KeyT &needle) {
    auto hash = HashT()(needle);
    size_t i = hash % nodes_.size();
    while (true) {
      if (nodes_[i].key == needle) {
        return &nodes_[i].value;
      }
      if (nodes_[i].hash == 0) {
        return nullptr;
      }
      i++;
      if (i == nodes_.size()) {
        i = 0;
      }
    }

  }

 private:
  using KeyValue = value_type;
  struct Node {
    td::uint64 hash{0};
    KeyT key;
    ValueT value;
  };
  std::vector<Node> nodes_;

  void insert(KeyT key, ValueT value) {
    auto hash = HashT()(key);
    size_t i = hash % nodes_.size();
    while (true) {
      if (nodes_[i].hash == 0 || (nodes_[i].hash == hash && nodes_[i].key == key)) {
        nodes_[i].value = value;
        nodes_[i].key = key;
        nodes_[i].hash = hash;
        return;
      }
      i++;
      if (i == nodes_.size()) {
        i = 0;
      }
    }
  }
};

template <typename TableT>
void BM_Get(benchmark::State& state) {
  size_t n = state.range(0);
  constexpr size_t batch_size = 1024;
  td::Random::Xorshift128plus rnd(123);
  using Key = typename TableT::key_type;
  using Value = typename TableT::value_type::second_type;
  using KeyValue = std::pair<Key, Value>;
  std::vector<KeyValue> data;
  std::vector<Key> keys;

  for (size_t i = 0; i < n; i++) {
    auto key = rnd();
    auto value = rnd();
    data.emplace_back(key, value);
    keys.push_back(key);
  }
  TableT table(data.begin(), data.end());

  size_t key_i = 0;
  td::random_shuffle(td::as_mutable_span(keys), rnd);
  auto next_key = [&] {
    key_i++;
    if (key_i == data.size()) {
      key_i = 0;
    }
    return keys[key_i];
  };

  while (state.KeepRunningBatch(batch_size)) {
    for (size_t i = 0; i < batch_size; i++) {
      benchmark::DoNotOptimize(table.find(next_key()));
    }
  }
}

template <typename TableT>
void BM_find_same(benchmark::State& state) {
  td::Random::Xorshift128plus rnd(123);
  TableT table;
  size_t N = 100000;
  size_t batch_size = 1024;
  reserve(table, N);

  for (size_t i = 0; i < N; i++) {
    table.emplace(rnd(), i);
  }

  auto key = td::Random::secure_uint64();
  table[key] = 123;

  while (state.KeepRunningBatch(batch_size)) {
    for (size_t i = 0; i < batch_size; i++) {
      benchmark::DoNotOptimize(table.find(key));
    }
  }
}

template <typename TableT>
void BM_emplace_same(benchmark::State& state) {
  td::Random::Xorshift128plus rnd(123);
  TableT table;
  size_t N = 100000;
  size_t batch_size = 1024;
  reserve(table, N);

  for (size_t i = 0; i < N; i++) {
    table.emplace(rnd(), i);
  }

  auto key = 123743;
  table[key] = 123;

  while (state.KeepRunningBatch(batch_size)) {
    for (size_t i = 0; i < batch_size; i++) {
      benchmark::DoNotOptimize(table.emplace(key, 43784932));
    }
  }
}
template <typename TableT>
void bench_create(td::Slice name) {
  td::Random::Xorshift128plus rnd(123);
  {
    size_t N = 10000000;
    TableT table;
    reserve(table, N);
    auto start = td::Timestamp::now();
    for (size_t i = 0; i < N; i++) {
      table.emplace(rnd(), i);
    }
    auto end = td::Timestamp::now();
    LOG(INFO) << name << ":" << "create " << N << " elements: " << td::format::as_time(end.at() - start.at());

    double res = 0;
    std::vector<std::pair<size_t, td::format::Time>> pauses;
    for (size_t i = 0; i < N; i++) {
      auto emplace_start = td::Timestamp::now();
      table.emplace(rnd(), i);
      auto emplace_end = td::Timestamp::now();
      auto pause = emplace_end.at() - emplace_start.at();
      res = td::max(pause, res);
      if (pause > 0.001) {
        pauses.emplace_back(i, td::format::as_time(pause));
      }
    }

    LOG(INFO) << name << ":" << "create another " << N << " elements, max pause = " << td::format::as_time(res) << " " << pauses;
  }
}

#define FOR_EACH_TABLE(F) \
  F(td::FlatHashMapImpl) \
  F(folly::F14FastMap) \
  F(absl::flat_hash_map) \
  F(std::unordered_map) \
  F(std::map) \

//BENCHMARK(BM_Get<VectorTable<td::uint64, td::uint64>>)->Range(1, 1 << 26);
//BENCHMARK(BM_Get<SortedVectorTable<td::uint64, td::uint64>>)->Range(1, 1 << 26);
//BENCHMARK(BM_Get<NoOpTable<td::uint64, td::uint64>>)->Range(1, 1 << 26);



#define REGISTER_GET_BENCHMARK(HT) \
  BENCHMARK(BM_Get<HT<td::uint64, td::uint64>>)->Range(1, 1 << 23);

#define REGISTER_FIND_BENCHMARK(HT) \
  BENCHMARK(BM_find_same<HT<td::uint64, td::uint64>>)-> \
      ComputeStatistics("max", [](const std::vector<double>& v) -> double { \
        return *(std::max_element(std::begin(v), std::end(v))); \
      })-> \
      ComputeStatistics("min", [](const std::vector<double>& v) -> double { \
        return *(std::min_element(std::begin(v), std::end(v))); \
      })-> \
      Repetitions(20)->DisplayAggregatesOnly(true);

#define REGISTER_EMPLACE_BENCHMARK(HT) \
  BENCHMARK(BM_emplace_same<HT<td::uint64, td::uint64>>);

#define RUN_CREATE_BENCHMARK(HT) \
  bench_create<HT<td::uint64, td::uint64>>(#HT);

FOR_EACH_TABLE(REGISTER_FIND_BENCHMARK)
FOR_EACH_TABLE(REGISTER_EMPLACE_BENCHMARK)
FOR_EACH_TABLE(REGISTER_GET_BENCHMARK)

int main(int argc, char** argv) {
  FOR_EACH_TABLE(RUN_CREATE_BENCHMARK);

  benchmark::Initialize(&argc, argv);
  benchmark::RunSpecifiedBenchmarks();
  benchmark::Shutdown();
}