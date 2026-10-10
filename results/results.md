# B+ tree benchmark results

- CPU: 13th Gen Intel(R) Core(TM) i5-13420H, RAM 7.6 GiB
- OS page size: 4096 B, filesystem block size: 4096 B
- Dataset: 10,000,000 records, key 8 B, payload 64 B, inserted in file order (already shuffled)
- Workload per variant: 1,000,000 random point lookups, 100,000 absent-key lookups, 1,000 range scans of 100 records

## Node layout (derived from the page size at run time)

| Parameter | Value |
|---|---|
| Node size = page size P | 4096 B |
| Node header H / child pointer | 16 B / 4 B (32-bit page id) |
| Leaf capacity = floor((P-H)/(K+V)) | 56 records (4048 B used) |
| Internal capacity = floor((P-H-ptr)/(K+ptr)) | 339 keys, fan-out 340 (4088 B used) |
| Expected height (100% full / ~69% full) | 4 / 4 |

## Comparison

| Metric | Memory-only | Disk-only | Memory + Disk |
|---|---|---|---|
| Storage | RAM only | file, O_DIRECT | buffer pool 64 MiB + file |
| **Insert** total time | 12.43 s | 4,207 s (70.1 min) | 1,813 s (30.2 min) |
| Insert: time per insert | 1.24 µs | 420.74 µs | 181.26 µs |
| Insert: throughput | 804,692 /s | 2,377 /s | 5,517 /s |
| Insert: page reads per insert | 0 | 3.67 | 0.76 |
| Insert: page writes per insert | 0 | 1.05 | 0.78 |
| Insert: time inside I/O calls | 0 | 4,191 s (69.8 min) | 1,791 s (29.9 min) |
| Insert: CPU time (user+sys) | 12.34 s | 322 s (5.4 min) | 169 s (2.8 min) |
| Final flush (dirty pages + fdatasync) | 0 | 0.00 s, 1 pages | 0.99 s, 16,385 pages |
| **Search** avg time per lookup | 3.10 µs | 341.37 µs | 131.62 µs |
| Search: p50 / p99 latency | 2.15 / 5.59 µs | 322.04 / 647.32 µs | 129.17 / 273.01 µs |
| Search: throughput | 322,694 /s | 2,929 /s | 7,598 /s |
| Search: page reads per lookup | 0 | 4.00 | 0.94 |
| Search: keys found / payload errors | 1,000,000 / 0 | 1,000,000 / 0 | 1,000,000 / 0 |
| Absent-key lookup avg | 1.35 µs | 340.12 µs | 125.00 µs |
| Range scan of 100 records | 2.7 µs | 633.0 µs | 443.4 µs |
| Buffer pool hit ratio: insert | - | - | 79.3 % |
| Buffer pool hit ratio: search | - | - | 76.5 % |
| Hit ratio internal / leaf pages (search) | - | - | 100.0 % / 6.2 % |
| Dirty evictions (write-backs) during insert | - | - | 7,832,464 |
| **Memory** held by the index | 1,005.0 MiB | 0.3 MiB | 65.4 MiB |
| RSS growth while building | 1,008.1 MiB | 0.0 MiB | 66.0 MiB |
| Peak RSS of process (incl. dataset) | 1,706.7 MiB (dataset 686.6 MiB) | 698.6 MiB (dataset 686.6 MiB) | 763.7 MiB (dataset 686.6 MiB) |
| **Disk usage** (index file) | 0 | 1,004.3 MiB (257,097 pages) | 1,004.3 MiB (257,097 pages) |
| **Disk I/O** during insert | 0 | 36,653,869 reads (143,179.2 MiB), 10,514,184 writes (41,071.0 MiB) | 7,591,751 reads (29,655.3 MiB), 7,832,464 writes (30,595.6 MiB) |
| Kernel-reported I/O during insert (/proc/self/io) | 0 | 143,179.2 MiB read, 41,071.0 MiB written | 29,655.3 MiB read, 30,595.6 MiB written |
| Disk I/O during 1,000,000 lookups | 0 | 4,000,000 reads | 939,139 reads |
| **Tree height** (levels incl. leaves) | 4 | 4 | 4 |
| Nodes per level (root ... leaves) | 1 / 4 / 1,024 / 256,067 | 1 / 4 / 1,024 / 256,067 | 1 / 4 / 1,024 / 256,067 |
| Leaf / internal fill factor | 69.7 % / 73.4 % | 69.7 % / 73.4 % | 69.7 % / 73.4 % |
| Leaf / internal splits | 256,066 / 1,026 | 256,066 / 1,026 | 256,066 / 1,026 |
| Full structural verification | passed | passed | passed |

