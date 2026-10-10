#!/usr/bin/env python3
"""make_report.py - turn the benchmark JSON files into Markdown tables.

    python3 scripts/make_report.py results > results/results.md

Reads results/{memory,disk,hybrid}.json and, if present, results/sweep/*.json.
Standard library only.
"""
import glob
import json
import os
import sys

MB = 1024.0 * 1024.0


def load(path):
    try:
        with open(path) as f:
            return json.load(f)
    except (OSError, ValueError):
        return None


def mb(b):
    return f"{b / MB:,.1f} MiB"


def num(x, digits=2):
    return f"{x:,.{digits}f}"


def secs(s):
    if s >= 120:
        return f"{s:,.0f} s ({s / 60:.1f} min)"
    return f"{s:,.2f} s"


def levels(d):
    lv = d["tree"]["levels"]
    return " / ".join(f"{l['nodes']:,}" for l in reversed(lv))


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "results"
    runs = [(name, load(os.path.join(out, f"{key}.json")))
            for key, name in (("memory", "Memory-only"), ("disk", "Disk-only"), ("hybrid", "Memory + Disk"))]
    runs = [(n, d) for n, d in runs if d]
    if not runs:
        sys.exit(f"no result files in {out}/")
    d0 = runs[0][1]
    L, ds, sysd = d0["layout"], d0["dataset"], d0["system"]
    p = print

    p("# B+ tree benchmark results\n")
    p(f"- CPU: {sysd['cpu']}, RAM {sysd['ram_bytes'] / 2**30:.1f} GiB")
    p(f"- OS page size: {sysd['os_page_size']} B, filesystem block size: {sysd['fs_block_size']} B")
    p(f"- Dataset: {ds['records']:,} records, key {ds['key_size']} B, payload {ds['val_size']} B, "
      f"inserted in file order (already shuffled)")
    p(f"- Workload per variant: {d0['workload']['queries']:,} random point lookups, "
      f"{d0['workload']['miss_queries']:,} absent-key lookups, "
      f"{d0['workload']['range_scans']:,} range scans of {d0['workload']['range_len']} records\n")

    p("## Node layout (derived from the page size at run time)\n")
    p("| Parameter | Value |")
    p("|---|---|")
    p(f"| Node size = page size P | {L['page_size']} B |")
    p(f"| Node header H / child pointer | {L['header_bytes']} B / {L['pointer_bytes']} B (32-bit page id) |")
    p(f"| Leaf capacity = floor((P-H)/(K+V)) | {L['leaf_cap']} records ({L['leaf_bytes_used']} B used) |")
    p(f"| Internal capacity = floor((P-H-ptr)/(K+ptr)) | {L['internal_cap']} keys, fan-out {L['fanout']} "
      f"({L['internal_bytes_used']} B used) |")
    p(f"| Expected height (100% full / ~69% full) | {L['expected_height_full']} / {L['expected_height_random']} |\n")

    names = [n for n, _ in runs]
    p("## Comparison\n")
    p("| Metric | " + " | ".join(names) + " |")
    p("|---" * (len(names) + 1) + "|")

    def row(label, fn):
        cells = []
        for _, d in runs:
            try:
                cells.append(fn(d))
            except (KeyError, ZeroDivisionError, TypeError):
                cells.append("-")
        p(f"| {label} | " + " | ".join(cells) + " |")

    def disk_only(fn):
        return lambda d: fn(d) if d["store"]["on_disk"] else "0"

    def pool_only(fn):
        return lambda d: fn(d) if d["store"]["name"] == "hybrid" else "-"

    row("Storage", lambda d: ("buffer pool " + num(d["store"]["pool_mb"], 0) + " MiB + file"
                              if d["store"]["name"] == "hybrid" else
                              ("file, O_DIRECT" if d["store"]["direct_io"] else "file") if d["store"]["on_disk"]
                              else "RAM only"))
    row("**Insert** total time", lambda d: secs(d["build"]["seconds"]))
    row("Insert: time per insert", lambda d: num(d["build"]["us_per_insert"]) + " µs")
    row("Insert: throughput", lambda d: f"{d['build']['inserts_per_sec']:,.0f} /s")
    row("Insert: page reads per insert", disk_only(lambda d: num(d["build"]["reads_per_op"])))
    row("Insert: page writes per insert", disk_only(lambda d: num(d["build"]["writes_per_op"])))
    row("Insert: time inside I/O calls", disk_only(lambda d: secs(d["build"]["io_seconds"])))
    row("Insert: CPU time (user+sys)", lambda d: secs(d["build"]["cpu_user_seconds"] + d["build"]["cpu_sys_seconds"]))
    row("Final flush (dirty pages + fdatasync)", disk_only(lambda d: f"{secs(d['build']['flush_seconds'])}, "
                                                          f"{d['build']['flush_page_writes']:,} pages"))
    row("**Search** avg time per lookup", lambda d: num(d["search"]["us_per_query"]) + " µs")
    row("Search: p50 / p99 latency", lambda d: f"{num(d['search']['p50_us'])} / {num(d['search']['p99_us'])} µs")
    row("Search: throughput", lambda d: f"{d['search']['queries_per_sec']:,.0f} /s")
    row("Search: page reads per lookup", disk_only(lambda d: num(d["search"]["reads_per_op"])))
    row("Search: keys found / payload errors", lambda d: f"{d['search']['found']:,} / {d['search']['payload_mismatches']}")
    row("Absent-key lookup avg", lambda d: num(d["search_miss"]["us_per_query"]) + " µs")
    row(f"Range scan of {d0['workload']['range_len']} records", lambda d: num(d["range_scan"]["us_per_scan"], 1) + " µs")
    row("Buffer pool hit ratio: insert", pool_only(lambda d: num(100 * d["build"]["hit_ratio"], 1) + " %"))
    row("Buffer pool hit ratio: search", pool_only(lambda d: num(100 * d["search"]["hit_ratio"], 1) + " %"))
    row("Hit ratio internal / leaf pages (search)",
        pool_only(lambda d: f"{num(100 * d['search']['internal_hit_ratio'], 1)} % / {num(100 * d['search']['leaf_hit_ratio'], 1)} %"))
    row("Dirty evictions (write-backs) during insert", pool_only(lambda d: f"{d['build']['dirty_evictions']:,}"))
    row("**Memory** held by the index", lambda d: mb(d["memory"]["index_bytes"]))
    row("RSS growth while building", lambda d: mb(d["memory"]["rss_delta_build"]))
    row("Peak RSS of process (incl. dataset)", lambda d: f"{mb(d['memory']['peak_rss'])} (dataset {mb(d['memory']['dataset_bytes'])})")
    row("**Disk usage** (index file)", disk_only(lambda d: f"{mb(d['disk']['index_file_bytes'])} ({d['disk']['num_pages']:,} pages)"))
    row("**Disk I/O** during insert", disk_only(lambda d: f"{d['build']['page_reads']:,} reads ({mb(d['build']['bytes_read'])}), "
                                                     f"{d['build']['page_writes']:,} writes ({mb(d['build']['bytes_written'])})"))
    row("Kernel-reported I/O during insert (/proc/self/io)",
        disk_only(lambda d: f"{mb(d['build']['os_read_bytes'])} read, {mb(d['build']['os_write_bytes'])} written"))
    row(f"Disk I/O during {d0['workload']['queries']:,} lookups",
        disk_only(lambda d: f"{d['search']['page_reads']:,} reads"))
    row("**Tree height** (levels incl. leaves)", lambda d: str(d["tree"]["height"]))
    row("Nodes per level (root ... leaves)", levels)
    row("Leaf / internal fill factor", lambda d: f"{num(100 * d['tree']['leaf_fill'], 1)} % / {num(100 * d['tree']['internal_fill'], 1)} %")
    row("Leaf / internal splits", lambda d: f"{d['build']['leaf_splits']:,} / {d['build']['internal_splits']:,}")
    row("Full structural verification", lambda d: ("passed" if d["tree"]["verify_ok"] else "FAILED: " + d["tree"]["verify_error"])
        if d["tree"]["verified"] else "skipped")
    p()

    sweep = os.path.join(out, "sweep")
    pools = sorted((load(f) for f in glob.glob(os.path.join(sweep, "pool_*.json"))), key=lambda d: d["store"]["pool_mb"])
    hyb = load(os.path.join(out, "hybrid.json"))
    if hyb and not any(d["store"]["pool_mb"] == hyb["store"]["pool_mb"] for d in pools):
        pools = sorted(pools + [hyb], key=lambda d: d["store"]["pool_mb"])
    if len(pools) > 1:
        p("## Memory + Disk: effect of buffer pool size\n")
        p("| Pool | % of index | Insert total | µs / insert | Hit ratio (insert) | I/O per insert (r+w) "
          "| Search µs | Hit ratio (search) | Reads / lookup |")
        p("|---|---|---|---|---|---|---|---|---|")
        for d in pools:
            b, s = d["build"], d["search"]
            pct = 100.0 * d["store"]["pool_mb"] * MB / max(1, d["disk"]["index_file_bytes"])
            p(f"| {d['store']['pool_mb']:,.0f} MiB | {min(pct, 100):.0f}% | {secs(b['seconds'])} | {num(b['us_per_insert'])} "
              f"| {100 * b['hit_ratio']:.1f}% | {b['reads_per_op'] + b['writes_per_op']:.2f} "
              f"| {num(s['us_per_query'])} | {100 * s['hit_ratio']:.1f}% | {s['reads_per_op']:.2f} |")
        p()

    pages = sorted((load(f) for f in glob.glob(os.path.join(sweep, "page_*.json"))), key=lambda d: d["layout"]["page_size"])
    mem = load(os.path.join(out, "memory.json"))
    if mem and not any(d["layout"]["page_size"] == mem["layout"]["page_size"] for d in pages):
        pages = sorted(pages + [mem], key=lambda d: d["layout"]["page_size"])
    if len(pages) > 1:
        p("## Memory-only: effect of node (page) size\n")
        p("| Node size | Leaf cap | Fan-out | Height | Nodes (root ... leaves) | Index memory | µs / insert | µs / lookup |")
        p("|---|---|---|---|---|---|---|---|")
        for d in pages:
            l = d["layout"]
            p(f"| {l['page_size']:,} B | {l['leaf_cap']} | {l['fanout']:,} | {d['tree']['height']} | {levels(d)} "
              f"| {mb(d['memory']['index_bytes'])} | {num(d['build']['us_per_insert'])} | {num(d['search']['us_per_query'])} |")
        p()


if __name__ == "__main__":
    main()
