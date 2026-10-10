// main.cpp - benchmark driver. Builds ONE B+ tree variant over the dataset and measures:
// insertion time, search time (hits, misses, range scans), memory usage, disk usage, tree
// height and shape, and disk I/O. Results go to stdout and to a JSON file.
//
//   bptree_bench --mode memory|disk|hybrid --data data.csv [options]
//
// Each variant is meant to run in its own process (scripts/run_all.sh does that), so the
// memory numbers of one run are not polluted by another.
#if defined(__GLIBC__) || defined(__linux__)
#include <malloc.h>
#endif

#include <algorithm>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

#include "bptree.h"
#include "dataset.h"
#include "layout.h"
#include "storage.h"
#include "sysutil.h"

using namespace bpt;

namespace {

struct Options {
  std::string mode = "memory";  // memory | disk | hybrid
  std::string data;             // CSV file
  std::string dir = ".";        // directory for index files
  std::string json;             // where to write results (optional)
  uint32_t page_size = 0;       // 0 = use the OS page size
  double pool_mb = 64;          // hybrid: buffer pool size
  uint64_t limit = UINT64_MAX;  // use only the first N rows
  uint64_t queries = 1000000;
  uint64_t miss_queries = 100000;
  uint64_t range_scans = 1000;
  uint64_t range_len = 100;
  uint64_t seed = 42;
  uint32_t checkpoints = 20;
  uint32_t max_leaf = 0, max_fanout = 0;  // optional caps below the computed capacity
  bool verify = true;
  bool cleanup = false;  // delete the index file at the end
};

[[noreturn]] void usage() {
  std::fprintf(stderr,
               "usage: bptree_bench --mode memory|disk|hybrid --data FILE.csv [options]\n"
               "  --page-size N     node size in bytes (default: OS page size via sysconf)\n"
               "  --pool-mb N       buffer pool size for --mode hybrid (default 64)\n"
               "  --dir DIR         directory for index files (default .)\n"
               "  --limit N         only load the first N rows\n"
               "  --queries N       random point lookups of existing keys (default 1000000)\n"
               "  --miss-queries N  lookups of keys that do not exist (default 100000)\n"
               "  --range-scans N   range scans (default 1000) of --range-len records (100)\n"
               "  --seed N          RNG seed for the query workload (default 42)\n"
               "  --json FILE       write all results as JSON\n"
               "  --no-verify       skip the full structural check\n"
               "  --cleanup         delete the index file when done\n"
               "  --max-leaf N / --max-fanout N   lower node capacities (testing)\n");
  std::exit(2);
}

Options parse(int argc, char** argv) {
  Options o;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    auto val = [&]() -> std::string {
      if (i + 1 >= argc) usage();
      return argv[++i];
    };
    if (a == "--mode") o.mode = val();
    else if (a == "--data") o.data = val();
    else if (a == "--dir") o.dir = val();
    else if (a == "--json") o.json = val();
    else if (a == "--page-size") o.page_size = uint32_t(std::stoul(val()));
    else if (a == "--pool-mb") o.pool_mb = std::stod(val());
    else if (a == "--limit") o.limit = std::stoull(val());
    else if (a == "--queries") o.queries = std::stoull(val());
    else if (a == "--miss-queries") o.miss_queries = std::stoull(val());
    else if (a == "--range-scans") o.range_scans = std::stoull(val());
    else if (a == "--range-len") o.range_len = std::stoull(val());
    else if (a == "--seed") o.seed = std::stoull(val());
    else if (a == "--checkpoints") o.checkpoints = uint32_t(std::stoul(val()));
    else if (a == "--max-leaf") o.max_leaf = uint32_t(std::stoul(val()));
    else if (a == "--max-fanout") o.max_fanout = uint32_t(std::stoul(val()));
    else if (a == "--no-verify") o.verify = false;
    else if (a == "--cleanup") o.cleanup = true;
    else usage();
  }
  if (o.data.empty() || (o.mode != "memory" && o.mode != "disk" && o.mode != "hybrid")) usage();
  return o;
}

// ---- minimal JSON writer ---------------------------------------------------------------------
class Json {
 public:
  void open(const char* key = nullptr) { item(key); out_ += '{'; first_ = true; }
  void close() { out_ += '}'; first_ = false; }
  void open_array(const char* key) { item(key); out_ += '['; first_ = true; }
  void close_array() { out_ += ']'; first_ = false; }
  void num(const char* key, double v) {
    item(key);
    char b[64];
    if (std::isfinite(v)) std::snprintf(b, sizeof b, "%.9g", v);
    else std::snprintf(b, sizeof b, "null");
    out_ += b;
  }
  void num(const char* key, uint64_t v) { item(key); out_ += std::to_string(v); }
  void num(const char* key, int64_t v) { item(key); out_ += std::to_string(v); }
  void num(const char* key, uint32_t v) { num(key, uint64_t(v)); }
  void boolean(const char* key, bool v) { item(key); out_ += v ? "true" : "false"; }
  void str(const char* key, const std::string& v) {
    item(key);
    out_ += '"';
    for (char c : v) {
      if (c == '"' || c == '\\') { out_ += '\\'; out_ += c; }
      else if (static_cast<unsigned char>(c) < 0x20) out_ += ' ';
      else out_ += c;
    }
    out_ += '"';
  }
  const std::string& text() const { return out_; }

 private:
  void item(const char* key) {
    if (!first_) out_ += ',';
    first_ = false;
    if (key) { out_ += '"'; out_ += key; out_ += "\":"; }
  }
  std::string out_;
  bool first_ = true;
};

IoStats diff(const IoStats& a, const IoStats& b) {
  IoStats d;
  d.page_reads = b.page_reads - a.page_reads;
  d.page_writes = b.page_writes - a.page_writes;
  d.bytes_read = b.bytes_read - a.bytes_read;
  d.bytes_written = b.bytes_written - a.bytes_written;
  d.io_ns = b.io_ns - a.io_ns;
  d.syncs = b.syncs - a.syncs;
  d.hits = b.hits - a.hits;
  d.misses = b.misses - a.misses;
  d.evictions = b.evictions - a.evictions;
  d.dirty_evictions = b.dirty_evictions - a.dirty_evictions;
  d.leaf_hits = b.leaf_hits - a.leaf_hits;
  d.leaf_misses = b.leaf_misses - a.leaf_misses;
  d.internal_hits = b.internal_hits - a.internal_hits;
  d.internal_misses = b.internal_misses - a.internal_misses;
  return d;
}

double ratio(uint64_t a, uint64_t b) { return b ? double(a) / double(b) : 0.0; }

// Snapshot of everything we measure around a phase.
struct Snap {
  uint64_t t = 0;
  IoStats io;
  sysutil::ProcIo os;
  sysutil::CpuTimes cpu;
};

template <class Store>
Snap snap(const Store& st) {
  Snap s;
  s.io = st.stats;
  s.os = sysutil::proc_io();
  s.cpu = sysutil::cpu_times();
  s.t = sysutil::now_ns();
  return s;
}

// Writes the I/O + CPU part of a phase (ops = operations in the phase, for per-op averages).
void put_phase_io(Json& js, const Snap& a, const Snap& b, uint64_t ops, bool pool) {
  const IoStats d = diff(a.io, b.io);
  js.num("page_reads", d.page_reads);
  js.num("page_writes", d.page_writes);
  js.num("bytes_read", d.bytes_read);
  js.num("bytes_written", d.bytes_written);
  js.num("reads_per_op", ratio(d.page_reads, ops));
  js.num("writes_per_op", ratio(d.page_writes, ops));
  js.num("io_seconds", double(d.io_ns) * 1e-9);
  js.num("os_read_bytes", int64_t(b.os.read_bytes - a.os.read_bytes));
  js.num("os_write_bytes", int64_t(b.os.write_bytes - a.os.write_bytes));
  js.num("cpu_user_seconds", b.cpu.user - a.cpu.user);
  js.num("cpu_sys_seconds", b.cpu.sys - a.cpu.sys);
  if (pool) {
    js.num("hits", d.hits);
    js.num("misses", d.misses);
    js.num("hit_ratio", ratio(d.hits, d.hits + d.misses));
    js.num("internal_hit_ratio", ratio(d.internal_hits, d.internal_hits + d.internal_misses));
    js.num("leaf_hit_ratio", ratio(d.leaf_hits, d.leaf_hits + d.leaf_misses));
    js.num("evictions", d.evictions);
    js.num("dirty_evictions", d.dirty_evictions);
  }
}

double pct(std::vector<uint32_t>& v, double p) {
  if (v.empty()) return 0;
  size_t i = std::min(v.size() - 1, size_t(p * double(v.size())));
  std::nth_element(v.begin(), v.begin() + long(i), v.end());
  return double(v[i]) * 1e-3;  // ns -> us
}

uint32_t levels_for(double leaves, double fanout) {  // levels of a tree with this many leaves
  uint32_t h = 1;
  double nodes = std::ceil(leaves);
  while (nodes > 1) {
    nodes = std::ceil(nodes / fanout);
    ++h;
  }
  return h;
}

std::string mb(uint64_t b) {
  char s[32];
  std::snprintf(s, sizeof s, "%.1f MB", double(b) / (1024.0 * 1024.0));
  return s;
}

// ---------------------------------------------------------------------------------------------
template <class Store, class MakeStore>
int bench(const Options& o, const Dataset& ds, const Layout& L, MakeStore make_store,
          bool on_disk, const std::string& index_path, Json& js) {
  const bool pool = std::string(Store::name()) == "hybrid";
  const uint64_t N = ds.n;

  // ---------------- build ----------------
  const sysutil::MemInfo mem0 = sysutil::mem_info();
  std::unique_ptr<Store> st = make_store(true);
  auto tree = std::make_unique<BPlusTree<Store>>(*st, L, true);

  struct Checkpoint { uint64_t n; double sec; uint64_t reads, writes, hits, misses; uint32_t height; };
  std::vector<Checkpoint> cps;
  const uint64_t step = std::max<uint64_t>(1, N / std::max<uint32_t>(1, o.checkpoints));

  std::printf("[%s] inserting %" PRIu64 " records...\n", Store::name(), N);
  std::fflush(stdout);
  const Snap b0 = snap(*st);
  uint64_t dup = 0;
  for (uint64_t i = 0; i < N; ++i) {
    if (!tree->insert(ds.key(i), ds.val(i))) ++dup;
    if ((i + 1) % step == 0 || i + 1 == N) {
      const IoStats& s = st->stats;
      cps.push_back({i + 1, double(sysutil::now_ns() - b0.t) * 1e-9, s.page_reads - b0.io.page_reads,
                     s.page_writes - b0.io.page_writes, s.hits - b0.io.hits, s.misses - b0.io.misses,
                     tree->height()});
      if (on_disk) {
        std::printf("  %3.0f%%  %10" PRIu64 " records  %8.1f s  height %u\n", 100.0 * double(i + 1) / double(N),
                    i + 1, cps.back().sec, tree->height());
        std::fflush(stdout);
      }
    }
  }
  const Snap b1 = snap(*st);
  const sysutil::MemInfo mem1 = sysutil::mem_info();
  const uint64_t index_mem = st->memory_bytes();

  // flush: write remaining dirty pages + metadata, then fdatasync (no-op in memory)
  tree->save_meta();
  st->flush();
  const Snap f1 = snap(*st);

  const uint64_t num_pages = st->num_pages();
  const uint64_t file_bytes = st->file_bytes();
  const uint64_t file_alloc = st->file_allocated_bytes();
  const bool direct = st->direct_io();
  const uint64_t leaf_splits = tree->leaf_splits(), internal_splits = tree->internal_splits(),
                 root_splits = tree->root_splits();

  // ---------------- reopen (disk variants): prove the index persists, start queries cold ----
  double reopen_sec = 0;
  if (on_disk) {
    tree.reset();
    st.reset();  // closes the file; the buffer pool is gone
    const uint64_t t = sysutil::now_ns();
    st = make_store(false);
    tree = std::make_unique<BPlusTree<Store>>(*st, L, false);
    reopen_sec = double(sysutil::now_ns() - t) * 1e-9;
    if (tree->size() != N - dup) throw std::runtime_error("reopened index has the wrong record count");
  }

  // ---------------- point lookups of existing keys ----------------
  std::mt19937_64 rng(o.seed);
  std::vector<uint32_t> q(o.queries);
  for (auto& x : q) x = uint32_t(rng() % N);
  std::vector<uint32_t> lat(o.queries);
  std::vector<uint8_t> out(L.val_size);
  uint64_t found = 0, mismatch = 0;
  std::printf("[%s] %" PRIu64 " point lookups...\n", Store::name(), o.queries);
  std::fflush(stdout);
  const Snap s0 = snap(*st);
  for (uint64_t i = 0; i < o.queries; ++i) {
    const uint64_t a = sysutil::now_ns();
    const bool f = tree->search(ds.key(q[i]), out.data());
    lat[i] = uint32_t(std::min<uint64_t>(sysutil::now_ns() - a, 0xFFFFFFFFu));
    if (f) {
      ++found;
      if (std::memcmp(out.data(), ds.val(q[i]), L.val_size) != 0) ++mismatch;
    }
  }
  const Snap s1 = snap(*st);

  // ---------------- lookups of keys that do not exist ----------------
  // A real key with its last byte replaced by '~' cannot exist (keys are alphanumeric) and
  // sorts right next to that real key, so these misses land on random leaves all over the
  // tree and cost a full root-to-leaf descent, like a hit. (Random strings would mostly sort
  // before or after every key and always hit the first or last leaf.)
  std::vector<uint8_t> miss(size_t(o.miss_queries) * L.key_size);
  for (uint64_t i = 0; i < o.miss_queries; ++i) {
    uint8_t* m = &miss[size_t(i) * L.key_size];
    std::memcpy(m, ds.key(rng() % N), L.key_size);
    m[L.key_size - 1] = '~';
  }
  uint64_t miss_found = 0;
  const Snap m0 = snap(*st);
  for (uint64_t i = 0; i < o.miss_queries; ++i)
    if (tree->search(&miss[size_t(i) * L.key_size], nullptr)) ++miss_found;
  const Snap m1 = snap(*st);

  // ---------------- range scans ----------------
  uint64_t scanned = 0, scan_bad = 0;
  std::vector<uint32_t> rs(o.range_scans);
  for (auto& x : rs) x = uint32_t(rng() % N);
  const Snap r0 = snap(*st);
  for (uint64_t i = 0; i < o.range_scans; ++i) {
    const uint8_t* start = ds.key(rs[i]);
    bool first = true;
    scanned += tree->scan(start, o.range_len, [&](const uint8_t* k, const uint8_t*) {
      if (first && std::memcmp(k, start, L.key_size) != 0) ++scan_bad;
      first = false;
    });
  }
  const Snap r1 = snap(*st);

  // ---------------- full structural check (not timed) ----------------
  VerifyResult vr;
  vr.height = tree->height();
  double verify_sec = 0;
  if (o.verify) {
    std::printf("[%s] verifying the whole tree...\n", Store::name());
    std::fflush(stdout);
    const uint64_t t = sysutil::now_ns();
    vr = tree->verify();
    verify_sec = double(sysutil::now_ns() - t) * 1e-9;
  }
  const sysutil::MemInfo mem_end = sysutil::mem_info();

  // ---------------- report ----------------
  const double build_sec = double(b1.t - b0.t) * 1e-9;
  const double search_sec = double(s1.t - s0.t) * 1e-9;
  const double miss_sec = double(m1.t - m0.t) * 1e-9;
  const double scan_sec = double(r1.t - r0.t) * 1e-9;
  std::vector<uint32_t> lat_sorted = lat;
  const double p50 = pct(lat_sorted, 0.50), p90 = pct(lat_sorted, 0.90), p99 = pct(lat_sorted, 0.99),
               pmax = lat.empty() ? 0 : double(*std::max_element(lat.begin(), lat.end())) * 1e-3;

  js.open("store");
  js.str("name", Store::name());
  js.boolean("on_disk", on_disk);
  js.boolean("direct_io", direct);
  js.str("index_file", on_disk ? index_path : "");
  if (pool) {
    js.num("pool_mb", o.pool_mb);
    js.num("pool_frames", uint64_t(o.pool_mb * 1024 * 1024) / L.page_size);
  }
  js.close();

  js.open("build");
  js.num("records", N);
  js.num("duplicates", dup);
  js.num("seconds", build_sec);
  js.num("inserts_per_sec", double(N) / std::max(build_sec, 1e-12));
  js.num("us_per_insert", build_sec * 1e6 / double(N));
  put_phase_io(js, b0, b1, N, pool);
  js.num("leaf_splits", leaf_splits);
  js.num("internal_splits", internal_splits);
  js.num("root_splits", root_splits);
  js.num("flush_seconds", double(f1.t - b1.t) * 1e-9);
  js.num("flush_page_writes", f1.io.page_writes - b1.io.page_writes);
  js.open_array("checkpoints");
  for (const auto& c : cps) {
    js.open();
    js.num("records", c.n);
    js.num("seconds", c.sec);
    js.num("page_reads", c.reads);
    js.num("page_writes", c.writes);
    js.num("hits", c.hits);
    js.num("misses", c.misses);
    js.num("height", c.height);
    js.close();
  }
  js.close_array();
  js.close();

  js.open("memory");
  js.num("index_bytes", index_mem);
  js.num("rss_before_build", mem0.rss);
  js.num("rss_after_build", mem1.rss);
  js.num("rss_delta_build", mem1.rss > mem0.rss ? mem1.rss - mem0.rss : 0);
  js.num("peak_rss", std::max(mem1.peak, mem_end.peak));
  js.num("dataset_bytes", ds.bytes());
  js.close();

  js.open("disk");
  js.num("num_pages", num_pages);
  js.num("index_file_bytes", file_bytes);
  js.num("index_file_allocated_bytes", file_alloc);
  js.num("reopen_seconds", reopen_sec);
  js.close();

  js.open("tree");
  js.num("height", vr.height);
  js.boolean("verified", o.verify);
  js.boolean("verify_ok", vr.ok);
  js.str("verify_error", vr.error);
  js.num("verify_seconds", verify_sec);
  js.num("leaf_fill", vr.leaf_fill);
  js.num("internal_fill", vr.internal_fill);
  js.open_array("levels");
  for (size_t l = 0; l < vr.levels.size(); ++l) {
    js.open();
    js.num("level", uint64_t(l));
    js.num("nodes", vr.levels[l].nodes);
    js.num("entries", vr.levels[l].entries);
    js.close();
  }
  js.close_array();
  js.close();

  js.open("search");
  js.num("queries", o.queries);
  js.num("found", found);
  js.num("payload_mismatches", mismatch);
  js.num("seconds", search_sec);
  js.num("us_per_query", search_sec * 1e6 / std::max<double>(1, double(o.queries)));
  js.num("queries_per_sec", double(o.queries) / std::max(search_sec, 1e-12));
  js.num("p50_us", p50);
  js.num("p90_us", p90);
  js.num("p99_us", p99);
  js.num("max_us", pmax);
  put_phase_io(js, s0, s1, o.queries, pool);
  js.close();

  js.open("search_miss");
  js.num("queries", o.miss_queries);
  js.num("found", miss_found);
  js.num("seconds", miss_sec);
  js.num("us_per_query", miss_sec * 1e6 / std::max<double>(1, double(o.miss_queries)));
  put_phase_io(js, m0, m1, o.miss_queries, pool);
  js.close();

  js.open("range_scan");
  js.num("scans", o.range_scans);
  js.num("range_len", o.range_len);
  js.num("records_returned", scanned);
  js.num("bad_start", scan_bad);
  js.num("seconds", scan_sec);
  js.num("us_per_scan", scan_sec * 1e6 / std::max<double>(1, double(o.range_scans)));
  put_phase_io(js, r0, r1, o.range_scans, pool);
  js.close();

  // human-readable summary
  const IoStats bd = diff(b0.io, b1.io), sd = diff(s0.io, s1.io);
  std::printf("\n==== %s B+ tree, %" PRIu64 " records ====\n", Store::name(), N);
  std::printf("insert : %.2f s total, %.2f us/insert, %.0f inserts/s\n", build_sec,
              build_sec * 1e6 / double(N), double(N) / build_sec);
  if (on_disk)
    std::printf("         page reads %" PRIu64 " (%.2f/insert), page writes %" PRIu64
                " (%.2f/insert), I/O time %.1f s; flush %.2f s\n",
                bd.page_reads, ratio(bd.page_reads, N), bd.page_writes, ratio(bd.page_writes, N),
                double(bd.io_ns) * 1e-9, double(f1.t - b1.t) * 1e-9);
  if (pool)
    std::printf("         buffer pool hit ratio %.2f%% (internal %.2f%%, leaf %.2f%%)\n",
                100 * ratio(bd.hits, bd.hits + bd.misses),
                100 * ratio(bd.internal_hits, bd.internal_hits + bd.internal_misses),
                100 * ratio(bd.leaf_hits, bd.leaf_hits + bd.leaf_misses));
  std::printf("search : %.3f us/lookup avg (p50 %.2f, p99 %.2f us), found %" PRIu64 "/%" PRIu64
              ", payload mismatches %" PRIu64 "\n",
              search_sec * 1e6 / std::max<double>(1, double(o.queries)), p50, p99, found, o.queries,
              mismatch);
  if (on_disk && pool)
    std::printf("         page reads %.2f/lookup, buffer pool hit ratio %.2f%%\n",
                ratio(sd.page_reads, o.queries), 100 * ratio(sd.hits, sd.hits + sd.misses));
  else if (on_disk)
    std::printf("         page reads %.2f/lookup\n", ratio(sd.page_reads, o.queries));
  std::printf("miss   : %.3f us/lookup for absent keys, %" PRIu64 " of %" PRIu64 " found\n",
              miss_sec * 1e6 / std::max<double>(1, double(o.miss_queries)), miss_found, o.miss_queries);
  std::printf("scan   : %.1f us per %" PRIu64 "-record range scan\n",
              scan_sec * 1e6 / std::max<double>(1, double(o.range_scans)), o.range_len);
  std::printf("memory : index %s, RSS +%s during build, peak RSS %s (incl. %s dataset)\n",
              mb(index_mem).c_str(), mb(mem1.rss > mem0.rss ? mem1.rss - mem0.rss : 0).c_str(),
              mb(std::max(mem1.peak, mem_end.peak)).c_str(), mb(ds.bytes()).c_str());
  std::printf("disk   : %s file (%" PRIu64 " pages)%s\n", mb(file_bytes).c_str(), num_pages,
              on_disk ? (direct ? ", O_DIRECT" : ", buffered I/O (O_DIRECT unavailable)") : "");
  std::printf("tree   : height %u", vr.height);
  for (size_t l = vr.levels.size(); l-- > 0;)
    std::printf("%s L%zu=%" PRIu64, l + 1 == vr.levels.size() ? "  nodes per level:" : ",", l,
                vr.levels[l].nodes);
  if (o.verify)
    std::printf("\n         leaf fill %.1f%%, internal fill %.1f%%, verify %s%s\n", 100 * vr.leaf_fill,
                100 * vr.internal_fill, vr.ok ? "OK" : "FAILED: ", vr.ok ? "" : vr.error.c_str());
  else
    std::printf("\n");

  if (o.cleanup && on_disk) {
    tree.reset();
    st.reset();
    std::remove(index_path.c_str());
  }
  const bool ok = (!o.verify || vr.ok) && mismatch == 0 && found == o.queries && scan_bad == 0;
  return ok ? 0 : 3;
}

int run(const Options& o) {
  Json js;
  js.open();

  // ---- dataset ----
  std::printf("loading %s ...\n", o.data.c_str());
  std::fflush(stdout);
  uint64_t t = sysutil::now_ns();
  const Dataset ds = load_csv(o.data, o.limit);
  const double load_sec = double(sysutil::now_ns() - t) * 1e-9;
#if defined(__GLIBC__)
  malloc_trim(0);  // return the CSV read buffers to the OS so the RSS baseline is clean
#endif

  // ---- node size from the system page size, fan-out from node size ----
  const uint32_t os_ps = sysutil::os_page_size();
  const uint32_t ps = o.page_size ? o.page_size : os_ps;
  const Layout L = Layout::compute(ps, ds.key_size, ds.val_size, o.max_leaf, o.max_fanout);

  const double rf = std::log(2.0);  // ~69% average fill under random insertion
  const uint32_t h_best = levels_for(std::ceil(double(ds.n) / L.leaf_cap), L.fanout());
  const uint32_t h_rand = levels_for(double(ds.n) / (L.leaf_cap * rf), L.fanout() * rf);

  std::printf("loaded %" PRIu64 " records in %.1f s (key %u B, payload %u B)\n", ds.n, load_sec,
              ds.key_size, ds.val_size);
  std::printf("OS page size %u B -> node size %u B%s\n", os_ps, ps, o.page_size ? " (overridden)" : "");
  std::printf("leaf: %u records/node (%u of %u B used)   internal: %u keys, fan-out %u (%u of %u B used)\n",
              L.leaf_cap, L.leaf_bytes_used(), ps, L.internal_cap, L.fanout(), L.internal_bytes_used(), ps);
  std::printf("expected height: %u if 100%% full (bulk load), ~%u with random inserts (~69%% full)\n",
              h_best, h_rand);
  std::fflush(stdout);

  js.str("mode", o.mode);
  js.open("system");
  js.num("os_page_size", os_ps);
  js.num("fs_block_size", sysutil::fs_block_size(o.dir));
  js.num("ram_bytes", sysutil::physical_ram_bytes());
  js.str("cpu", sysutil::cpu_model());
  js.close();
  js.open("dataset");
  js.str("path", o.data);
  js.num("records", ds.n);
  js.num("key_size", ds.key_size);
  js.num("val_size", ds.val_size);
  js.num("load_seconds", load_sec);
  js.close();
  js.open("layout");
  js.num("page_size", L.page_size);
  js.num("header_bytes", kHeaderSize);
  js.num("pointer_bytes", kPtrSize);
  js.num("key_size", L.key_size);
  js.num("val_size", L.val_size);
  js.num("leaf_cap", L.leaf_cap);
  js.num("internal_cap", L.internal_cap);
  js.num("fanout", L.fanout());
  js.num("leaf_bytes_used", L.leaf_bytes_used());
  js.num("internal_bytes_used", L.internal_bytes_used());
  js.num("expected_height_full", h_best);
  js.num("expected_height_random", h_rand);
  js.close();
  js.open("workload");
  js.num("queries", o.queries);
  js.num("miss_queries", o.miss_queries);
  js.num("range_scans", o.range_scans);
  js.num("range_len", o.range_len);
  js.num("seed", o.seed);
  js.close();

  int rc;
  if (o.mode == "memory") {
    auto make = [&](bool) { return std::make_unique<MemoryStore>(ps); };
    rc = bench<MemoryStore>(o, ds, L, make, false, "", js);
  } else if (o.mode == "disk") {
    const std::string path = o.dir + "/index_disk.db";
    auto make = [&](bool create) { return std::make_unique<DiskOnlyStore>(path, ps, create); };
    rc = bench<DiskOnlyStore>(o, ds, L, make, true, path, js);
  } else {
    const std::string path = o.dir + "/index_hybrid.db";
    const uint64_t pool = uint64_t(o.pool_mb * 1024.0 * 1024.0);
    auto make = [&](bool create) { return std::make_unique<BufferPoolStore>(path, ps, pool, create); };
    rc = bench<BufferPoolStore>(o, ds, L, make, true, path, js);
  }
  js.close();

  if (!o.json.empty()) {
    FILE* f = std::fopen(o.json.c_str(), "w");
    if (!f) throw std::runtime_error("cannot write " + o.json);
    std::fputs(js.text().c_str(), f);
    std::fputc('\n', f);
    std::fclose(f);
    std::printf("results written to %s\n", o.json.c_str());
  }
  return rc;
}

}  // namespace

int main(int argc, char** argv) {
  try {
    return run(parse(argc, argv));
  } catch (const std::exception& e) {
    std::fprintf(stderr, "error: %s\n", e.what());
    return 1;
  }
}
