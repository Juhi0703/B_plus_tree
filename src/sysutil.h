// sysutil.h - system queries used to size nodes and to measure the benchmarks.
#pragma once

#include <sys/resource.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>

#if defined(__APPLE__)
#include <mach/mach.h>
#endif

namespace sysutil {

inline uint64_t now_ns() {
  return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                      std::chrono::steady_clock::now().time_since_epoch())
                      .count());
}

// Virtual-memory page size of this machine (4096 on x86-64 Linux, 16384 on Apple Silicon).
// This is what the node size is derived from.
inline uint32_t os_page_size() {
  long p = ::sysconf(_SC_PAGESIZE);
  return p > 0 ? uint32_t(p) : 4096u;
}

// Block size of the filesystem holding `dir` (the unit the filesystem allocates in).
inline uint64_t fs_block_size(const std::string& dir) {
  struct statvfs s;
  if (::statvfs(dir.c_str(), &s) == 0) return uint64_t(s.f_bsize);
  return 0;
}

inline uint64_t physical_ram_bytes() {
  long pages = ::sysconf(_SC_PHYS_PAGES);
  long ps = ::sysconf(_SC_PAGESIZE);
  return (pages > 0 && ps > 0) ? uint64_t(pages) * uint64_t(ps) : 0;
}

inline std::string cpu_model() {
#if defined(__linux__)
  std::ifstream f("/proc/cpuinfo");
  std::string line;
  while (std::getline(f, line))
    if (line.rfind("model name", 0) == 0) {
      auto p = line.find(':');
      if (p != std::string::npos) return line.substr(p + 2);
    }
#endif
  return "unknown";
}

struct MemInfo {
  uint64_t rss = 0;   // resident set size now
  uint64_t peak = 0;  // peak resident set size of the process so far
};

inline MemInfo mem_info() {
  MemInfo m;
#if defined(__linux__)
  std::ifstream f("/proc/self/status");
  std::string line;
  while (std::getline(f, line)) {
    unsigned long long kb = 0;
    if (std::sscanf(line.c_str(), "VmRSS: %llu kB", &kb) == 1) m.rss = kb * 1024;
    if (std::sscanf(line.c_str(), "VmHWM: %llu kB", &kb) == 1) m.peak = kb * 1024;
  }
#elif defined(__APPLE__)
  mach_task_basic_info info;
  mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
  if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, (task_info_t)&info, &count) == KERN_SUCCESS) {
    m.rss = info.resident_size;
    m.peak = info.resident_size_max;
  }
#endif
  return m;
}

// Bytes this process caused to be read from / written to the storage layer, as accounted
// by the kernel (/proc/self/io). An independent cross-check of the tree's own I/O counters.
struct ProcIo {
  int64_t read_bytes = -1, write_bytes = -1;
};

inline ProcIo proc_io() {
  ProcIo io;
#if defined(__linux__)
  std::ifstream f("/proc/self/io");
  std::string line;
  while (std::getline(f, line)) {
    long long v = 0;
    if (std::sscanf(line.c_str(), "read_bytes: %lld", &v) == 1) io.read_bytes = v;
    if (std::sscanf(line.c_str(), "write_bytes: %lld", &v) == 1) io.write_bytes = v;
  }
#endif
  return io;
}

struct CpuTimes {
  double user = 0, sys = 0;
};

inline CpuTimes cpu_times() {
  struct rusage r;
  ::getrusage(RUSAGE_SELF, &r);
  CpuTimes c;
  c.user = double(r.ru_utime.tv_sec) + double(r.ru_utime.tv_usec) * 1e-6;
  c.sys = double(r.ru_stime.tv_sec) + double(r.ru_stime.tv_usec) * 1e-6;
  return c;
}

}  // namespace sysutil
