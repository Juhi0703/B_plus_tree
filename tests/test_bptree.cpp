// test_bptree.cpp - randomized correctness test of the B+ tree on all three stores.
//
// Uses tiny nodes (so trees are deep and split all the time), short variable-length keys (so
// duplicates happen and the zero-padding order is exercised), mirrors every insert in a
// std::map, and checks: insert return values, lookups, misses, full scan order, range scans,
// the structural invariants, and - for the two disk stores - that a closed and reopened index
// still holds everything.
//
// usage: test_bptree [scratch_dir]
#include <cstdio>
#include <cstring>
#include <functional>
#include <iterator>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "../src/bptree.h"
#include "../src/storage.h"

using namespace bpt;

static int g_failures = 0;

#define CHECK(cond, what)                                                                  \
  do {                                                                                     \
    if (!(cond)) {                                                                         \
      std::fprintf(stderr, "FAIL [%s] line %d: %s  (%s)\n", label.c_str(), __LINE__, #cond, \
                   std::string(what).c_str());                                             \
      ++g_failures;                                                                        \
      return;                                                                              \
    }                                                                                      \
  } while (0)

struct Case {
  uint32_t page, max_leaf, max_fanout, n;
};

static const char kAlnum[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";

static std::string rand_str(std::mt19937_64& rng, uint32_t min_len, uint32_t max_len) {
  const uint32_t len = min_len + uint32_t(rng() % (max_len - min_len + 1));
  std::string s(len, ' ');
  for (auto& c : s) c = kAlnum[rng() % 8 == 0 ? rng() % 62 : rng() % 16];  // skewed -> duplicates
  return s;
}

template <class Store>
static void run_case(const char* store_name, const Case& c, bool on_disk,
                     const std::function<std::unique_ptr<Store>(bool)>& make) {
  const uint32_t K = 8, V = 12;
  const Layout L = Layout::compute(c.page, K, V, c.max_leaf, c.max_fanout);
  const std::string label = std::string(store_name) + " page=" + std::to_string(c.page) +
                            " leaf_cap=" + std::to_string(L.leaf_cap) +
                            " fanout=" + std::to_string(L.fanout()) + " n=" + std::to_string(c.n);
  auto st = make(true);
  auto tree = std::make_unique<BPlusTree<Store>>(*st, L, true);
  std::map<std::string, std::string> ref;
  std::mt19937_64 rng(uint64_t(c.n) * 1000003u + c.page * 31u + L.leaf_cap);
  std::vector<uint8_t> kb(K), vb(V), out(V), tmp(K);
  auto pack = [](const std::string& s, std::vector<uint8_t>& b) {
    std::fill(b.begin(), b.end(), 0);
    std::memcpy(b.data(), s.data(), s.size());
  };

  for (uint32_t i = 0; i < c.n; ++i) {
    const std::string k = rand_str(rng, 1, K), v = rand_str(rng, 1, V);
    pack(k, kb);
    pack(v, vb);
    const bool fresh = tree->insert(kb.data(), vb.data());
    const bool ref_fresh = ref.insert_or_assign(k, v).second;
    CHECK(fresh == ref_fresh, "insert return value for key " + k);
    if (i % 4999 == 0) {
      const VerifyResult vr = tree->verify();
      CHECK(vr.ok, "mid-build verify: " + vr.error);
    }
  }
  CHECK(tree->size() == ref.size(), "record count");

  auto check_all = [&](const char* phase) {
    for (const auto& kv : ref) {
      pack(kv.first, kb);
      pack(kv.second, vb);
      if (!tree->search(kb.data(), out.data())) {
        std::fprintf(stderr, "FAIL [%s] %s: key %s not found\n", label.c_str(), phase, kv.first.c_str());
        ++g_failures;
        return false;
      }
      if (std::memcmp(out.data(), vb.data(), V) != 0) {
        std::fprintf(stderr, "FAIL [%s] %s: wrong payload for %s\n", label.c_str(), phase, kv.first.c_str());
        ++g_failures;
        return false;
      }
    }
    return true;
  };
  if (!check_all("after build")) return;

  for (int i = 0; i < 3000; ++i) {  // random lookups, mostly misses
    const std::string k = rand_str(rng, 1, K);
    pack(k, kb);
    CHECK(tree->search(kb.data(), nullptr) == (ref.count(k) == 1), "lookup of " + k);
  }

  {  // full scan == map order
    auto it = ref.begin();
    bool ok = true;
    const uint64_t got = tree->scan(nullptr, UINT64_MAX, [&](const uint8_t* k, const uint8_t*) {
      if (it == ref.end()) { ok = false; return; }
      pack(it->first, tmp);
      if (std::memcmp(k, tmp.data(), K) != 0) ok = false;
      ++it;
    });
    CHECK(ok && got == ref.size() && it == ref.end(), "full scan order");
  }

  for (int i = 0; i < 500; ++i) {  // range scans from random (mostly absent) start keys
    const std::string s = rand_str(rng, 1, K);
    pack(s, kb);
    auto it = ref.lower_bound(s);
    const uint64_t limit = rng() % 60;
    const uint64_t expected = std::min<uint64_t>(limit, uint64_t(std::distance(it, ref.end())));
    bool ok = true;
    const uint64_t got = tree->scan(kb.data(), limit, [&](const uint8_t* k, const uint8_t*) {
      if (it == ref.end()) { ok = false; return; }
      pack(it->first, tmp);
      if (std::memcmp(k, tmp.data(), K) != 0) ok = false;
      ++it;
    });
    CHECK(ok && got == expected, "range scan from " + s);
  }

  VerifyResult vr = tree->verify();
  CHECK(vr.ok, "final verify: " + vr.error);
  CHECK(vr.records == ref.size(), "verify record count");
  const uint32_t height = tree->height();

  tree->save_meta();
  st->flush();
  if (on_disk) {  // close, reopen, check again
    tree.reset();
    st.reset();
    st = make(false);
    tree = std::make_unique<BPlusTree<Store>>(*st, L, false);
    CHECK(tree->size() == ref.size() && tree->height() == height, "metadata after reopen");
    vr = tree->verify();
    CHECK(vr.ok, "verify after reopen: " + vr.error);
    if (!check_all("after reopen")) return;
  }
  std::printf("ok  %-62s height=%u records=%zu leaf_fill=%.0f%%\n", label.c_str(), height,
              ref.size(), 100 * vr.leaf_fill);
}

int main(int argc, char** argv) {
  const std::string dir = argc > 1 ? argv[1] : ".";
  const Case cases[] = {
      {256, 2, 3, 20000},   // smallest legal nodes: very deep tree
      {256, 3, 4, 30000},   // order-4 tree
      {256, 0, 0, 30000},   // capacities computed from a 256 B page
      {1024, 0, 7, 30000},  // big leaves, narrow internal nodes
      {4096, 0, 0, 60000},  // real page size (O_DIRECT path for the disk stores)
  };
  for (const Case& c : cases) {
    run_case<MemoryStore>("memory", c, false,
                          [&](bool) { return std::make_unique<MemoryStore>(c.page); });
    const uint32_t n_disk = c.page >= 4096 ? 4000 : c.n;  // O_DIRECT is slow; keep it short
    const Case cd{c.page, c.max_leaf, c.max_fanout, n_disk};
    const std::string dpath = dir + "/test_disk.db", hpath = dir + "/test_hybrid.db";
    run_case<DiskOnlyStore>("disk", cd, true, [&](bool create) {
      return std::make_unique<DiskOnlyStore>(dpath, c.page, create);
    });
    // 128 frames = the minimum pool, far smaller than the tree -> constant eviction
    run_case<BufferPoolStore>("hybrid", c, true, [&](bool create) {
      return std::make_unique<BufferPoolStore>(hpath, c.page, uint64_t(128) * c.page, create);
    });
    std::remove(dpath.c_str());
    std::remove(hpath.c_str());
  }
  if (g_failures) {
    std::printf("%d FAILURE(S)\n", g_failures);
    return 1;
  }
  std::printf("all tests passed\n");
  return 0;
}
