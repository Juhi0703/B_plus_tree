# B+ Tree on 10 Million Records: Memory-only vs Disk-only vs Memory + Disk

One B+ tree implementation (C++17, no external libraries) running on three storage back-ends,
benchmarked on 10,000,000 `key,payload` records. Node size is taken from the system page size at
run time and the fan-out is computed from it.

## Results

Run `scripts/run_all.sh data.csv` on your machine. It writes one JSON file and one log per
variant to `results/`, and a comparison table to `results/results.md` (insertion time, search
time, memory usage, disk usage, tree height, disk I/O). Times depend on your CPU and disk;
page I/O counts per operation and the tree shape depend only on the data and the page size.

## Node size, fan-out and order

`scripts/sysinfo.sh` reports the memory page size (`getconf PAGESIZE`), the filesystem block size,
the disk's sector sizes and the cache line, and passes the page size to the benchmark as
`--page-size`. Without that flag the program calls `sysconf(_SC_PAGESIZE)` itself. It also scans
the dataset first and uses the longest key (K) and payload (V) as fixed slot widths.

With P = page size, H = 16-byte node header, p = 4-byte child pointer (32-bit page id):

```
leaf capacity = floor((P - H) / (K + V))       = floor((4096 - 16) / (8 + 64)) = 56 records
fan-out       = floor((P - H - p) / (K + p)) + 1 = floor((4096 - 20) / 12) + 1  = 340 children
```

So one node = one 4 KB page = one disk I/O and the B+ tree order is 340. With every node full,
10M records need 4 levels (178,572 leaves); random insertion leaves nodes about 69% full
(ln 2), which still fits in 4 levels.

## How the indexing works

* **Node layout.** Leaf: header, 56 sorted keys, 56 payloads, `next` pointer to the right
  sibling. Internal: header, 340 child page ids, 339 sorted separator keys. Keys are stored in
  their own contiguous array so binary search touches only them.
* **Search.** From the root, binary-search the separators, follow the child, repeat down to the
  leaf, binary-search the leaf. Cost: one node per level (4 nodes).
* **Insert.** Descend and remember the path. If the leaf has room, shift and insert. If it is
  full, split it 28/29, link the new leaf into the leaf chain and copy its first key up. A full
  parent splits 170/169 and moves its middle key up. A root split adds a new root, the only way
  the tree grows taller, so all leaves stay at the same depth. Duplicate keys replace the payload.
* **Range scan.** Descend once to the start key, then walk the leaf chain.
* **Checks.** After each build a full pass verifies ordering, separator bounds, equal leaf depth,
  the leaf chain and the record count; every lookup compares the payload with the dataset.
  `make test` runs randomized tests against `std::map` on all three stores with tiny nodes
  (trees up to 12 levels), also under AddressSanitizer/UBSan (`make test-san`).

## Storage structure

The tree code only calls `pin(page_id)` / `unpin(page_id, dirty)`; the store underneath decides
where pages live. Page 0 of an index file holds metadata; page *i* is at byte offset *i* × 4096.

| Variant | Where nodes live | Node read | Node write | RAM held |
|---|---|---|---|---|
| Memory-only (`MemoryStore`) | RAM, 1 MiB page-aligned chunks | pointer arithmetic | in place | whole tree |
| Disk-only (`DiskOnlyStore`) | file opened with `O_DIRECT` (no OS page cache) | `pread` on every visit | `pwrite` immediately (write-through) | 68 frames for the current operation |
| Memory + Disk (`BufferPoolStore`) | file + fixed buffer pool | pool hit, or `pread` on a miss | in the pool; written on eviction or flush (write-back) | pool size (`--pool-mb`, default 64) |

The buffer pool uses an array page table, exact LRU replacement over unpinned frames, and
never evicts pinned frames. `O_DIRECT` matters: without it Linux would cache the "disk-only"
tree in RAM. The kernel's `/proc/self/io` counters match the program's page counters, so every
counted node access reached the device.

## Build and run

Requirements: Linux or WSL2 (macOS works, using `F_NOCACHE`), a C++17 compiler, `make`,
Python 3 for the summary table.

```bash
make                               # build/bptree_bench, build/gen_dataset, build/test_bptree
make test                          # randomized correctness tests
scripts/sysinfo.sh                 # page size, block/sector sizes, derived node capacities
scripts/run_all.sh data.csv        # all three variants -> results/*.json, results/results.md
scripts/run_sweeps.sh data.csv     # optional: buffer-pool and node-size experiments
```

Single runs:

```bash
./build/bptree_bench --mode memory --data data.csv --json results/memory.json
./build/bptree_bench --mode hybrid --pool-mb 64 --data data.csv
./build/bptree_bench --mode disk   --data data.csv --dir /path/on/a/real/disk
./build/bptree_bench --help        # all options
```

`run_all.sh` options via environment: `POOL_MB`, `QUERIES`, `MISS_QUERIES`, `LIMIT` (first N
rows, for a quick trial), `MODES`, `INDEX_DIR`. Index files must be on a real disk, not tmpfs.
Disk-only needs ~1 GiB free and makes about 5 synchronous disk I/Os per insert, so on a slow disk
try `LIMIT=1000000` first.

No dataset file? `./build/gen_dataset 10000000 data.csv` writes one in the same format
(`K` + 7 hex digits, 64 random alphanumeric characters, unique keys in shuffled order).

## Repository layout

| Path | Contents |
|---|---|
| `src/layout.h` | node layout and capacity formulas |
| `src/bptree.h` | the B+ tree: insert, search, range scan, verification |
| `src/storage.h` | `MemoryStore`, `DiskOnlyStore`, `BufferPoolStore`, O_DIRECT page file |
| `src/sysutil.h` | page size, RSS, kernel I/O counters, CPU time |
| `src/dataset.h` | CSV loader that measures key and payload widths |
| `src/main.cpp` | benchmark driver, JSON output |
| `tests/test_bptree.cpp` | randomized tests against `std::map` |
| `tools/gen_dataset.cpp` | dataset generator in the assignment's format |
| `scripts/` | `sysinfo.sh`, `run_all.sh`, `run_sweeps.sh`, `make_report.py` |
| `results/` | JSON results, logs and summary tables of the runs above |

## Assumptions and limitations

* Node size = OS page size; `--page-size` overrides it for experiments.
* Fixed-width key and payload slots sized to the longest value in the file, zero-padded
  (keeps byte order equal to string order).
* Clustered tree: payloads live in the leaves. A secondary index (key → record id) would have
  340-entry leaves and height 3 but need one extra I/O per lookup to fetch the payload.
* Records are inserted one by one in file order (random), as the assignment asks; no bulk loading.
* Single-threaded; no latching, write-ahead log or crash recovery; disk indexes are durable after
  the final flush + `fdatasync`. Deletion is not implemented (not required).
* All I/O is synchronous, one request at a time, so absolute times scale with the disk's latency;
  I/O counts per operation do not.
