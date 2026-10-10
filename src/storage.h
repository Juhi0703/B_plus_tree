// storage.h - the three storage back-ends ("page stores") the B+ tree runs on.
//
// The tree algorithm (bptree.h) never touches RAM or files directly. It asks a store for a
// page by id (pin), works on the returned buffer, and hands it back (unpin, saying whether it
// changed it). That single interface lets the SAME tree code run on three storage designs:
//
//   MemoryStore      every node lives in RAM, there is no file at all        -> memory-only
//   DiskOnlyStore    every node lives on disk. pin = read the page, unpin of a
//                    modified page = write it. Nothing is cached between
//                    operations, and O_DIRECT bypasses the OS page cache     -> disk-only
//   BufferPoolStore  nodes live on disk, a fixed-size RAM cache (buffer pool)
//                    keeps recently used pages; LRU replacement, write-back  -> memory + disk
//
// Store interface (duck-typed, used through a template so calls inline):
//   uint8_t* pin(uint32_t pid)              bring page `pid` into RAM and lock it there
//   void     unpin(uint32_t pid, bool dirty) caller is done with it (dirty = it was modified)
//   uint32_t alloc(uint8_t** page)          create a new zeroed page, returned pinned + dirty
//   void     flush()                        make the on-disk image complete and durable
#pragma once

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#include "layout.h"
#include "sysutil.h"

namespace bpt {

struct IoStats {
  // physical page I/O issued by this process (with O_DIRECT these really reach the device)
  uint64_t page_reads = 0, page_writes = 0;
  uint64_t bytes_read = 0, bytes_written = 0;
  uint64_t io_ns = 0;  // wall time spent inside pread/pwrite/fdatasync
  uint64_t syncs = 0;
  // buffer pool only
  uint64_t hits = 0, misses = 0, evictions = 0, dirty_evictions = 0;
  uint64_t leaf_hits = 0, leaf_misses = 0, internal_hits = 0, internal_misses = 0;
};

inline uint8_t* alloc_aligned(size_t bytes, size_t align) {
  void* p = nullptr;
  if (posix_memalign(&p, align, bytes) != 0 || p == nullptr)
    throw std::runtime_error("out of memory allocating " + std::to_string(bytes) + " bytes");
  return static_cast<uint8_t*>(p);
}

// ---------------------------------------------------------------------------------------------
// PagedFile: a file viewed as an array of fixed-size pages. Page `pid` lives at byte offset
// pid * page_size. All I/O is exactly one page, at a page-aligned offset, from a page-aligned
// buffer - the requirements of O_DIRECT.
// ---------------------------------------------------------------------------------------------
class PagedFile {
 public:
  PagedFile(const std::string& path, uint32_t page_size, bool create, IoStats* stats)
      : path_(path), ps_(page_size), stats_(stats) {
    const int flags = O_RDWR | (create ? (O_CREAT | O_TRUNC) : 0);
#if defined(O_DIRECT)
    fd_ = ::open(path.c_str(), flags | O_DIRECT, 0644);  // Linux: bypass the page cache
    direct_ = fd_ >= 0;
    if (fd_ < 0) fd_ = ::open(path.c_str(), flags, 0644);  // e.g. tmpfs has no O_DIRECT
#else
    fd_ = ::open(path.c_str(), flags, 0644);
#endif
    if (fd_ < 0) throw std::runtime_error("open " + path + ": " + std::strerror(errno));
#if defined(__APPLE__)
    direct_ = ::fcntl(fd_, F_NOCACHE, 1) == 0;  // macOS equivalent of O_DIRECT
#endif
  }
  ~PagedFile() {
    if (fd_ >= 0) ::close(fd_);
  }
  PagedFile(const PagedFile&) = delete;
  PagedFile& operator=(const PagedFile&) = delete;

  void read_page(uint32_t pid, uint8_t* buf) {
    const uint64_t t0 = sysutil::now_ns();
    const off_t off = off_t(pid) * ps_;
    size_t done = 0;
    while (done < ps_) {
      ssize_t r = ::pread(fd_, buf + done, ps_ - done, off + off_t(done));
      if (r < 0) {
        if (errno == EINTR) continue;
        if (errno == EINVAL && direct_) { drop_direct(); continue; }
        throw std::runtime_error("pread " + path_ + ": " + std::strerror(errno));
      }
      if (r == 0) throw std::runtime_error("read of page " + std::to_string(pid) + " past EOF");
      done += size_t(r);
    }
    stats_->io_ns += sysutil::now_ns() - t0;
    stats_->page_reads++;
    stats_->bytes_read += ps_;
  }

  void write_page(uint32_t pid, const uint8_t* buf) {
    const uint64_t t0 = sysutil::now_ns();
    const off_t off = off_t(pid) * ps_;
    size_t done = 0;
    while (done < ps_) {
      ssize_t w = ::pwrite(fd_, buf + done, ps_ - done, off + off_t(done));
      if (w < 0) {
        if (errno == EINTR) continue;
        if (errno == EINVAL && direct_) { drop_direct(); continue; }
        throw std::runtime_error("pwrite " + path_ + ": " + std::strerror(errno));
      }
      done += size_t(w);
    }
    stats_->io_ns += sysutil::now_ns() - t0;
    stats_->page_writes++;
    stats_->bytes_written += ps_;
  }

  void sync() {
    const uint64_t t0 = sysutil::now_ns();
#if defined(__APPLE__)
    ::fsync(fd_);
#else
    ::fdatasync(fd_);
#endif
    stats_->io_ns += sysutil::now_ns() - t0;
    stats_->syncs++;
  }

  uint32_t pages_on_disk() const { return uint32_t(size_bytes() / ps_); }
  uint64_t size_bytes() const {
    struct stat s;
    return ::fstat(fd_, &s) == 0 ? uint64_t(s.st_size) : 0;
  }
  uint64_t allocated_bytes() const {  // blocks actually allocated by the filesystem
    struct stat s;
    return ::fstat(fd_, &s) == 0 ? uint64_t(s.st_blocks) * 512 : 0;
  }
  bool direct() const { return direct_; }

 private:
  // The device refused direct I/O for this request (typically a page smaller than the
  // device's logical block, as in the unit tests). Fall back to buffered I/O, loudly.
  void drop_direct() {
#if defined(O_DIRECT)
    int fl = ::fcntl(fd_, F_GETFL);
    ::fcntl(fd_, F_SETFL, fl & ~O_DIRECT);
#endif
    direct_ = false;
    std::fprintf(stderr, "warning: O_DIRECT rejected for %s (page %u B); using buffered I/O\n",
                 path_.c_str(), ps_);
  }

  std::string path_;
  uint32_t ps_;
  IoStats* stats_;
  int fd_ = -1;
  bool direct_ = false;
};

// ---------------------------------------------------------------------------------------------
// 1) MemoryStore - memory-only B+ tree.
// Pages are carved out of 1 MB-ish page-aligned chunks; page id -> address is two shifts and
// an add, so pin/unpin cost nothing. No file, no I/O.
// ---------------------------------------------------------------------------------------------
class MemoryStore {
 public:
  static constexpr uint32_t kChunkShift = 8;  // 256 pages per chunk
  static constexpr uint32_t kChunkPages = 1u << kChunkShift;
  static constexpr uint32_t kChunkMask = kChunkPages - 1;

  explicit MemoryStore(uint32_t page_size) : ps_(page_size) {
    uint8_t* meta;
    alloc(&meta);  // page 0 = metadata page, keeps page ids identical across the three stores
  }
  ~MemoryStore() {
    for (uint8_t* c : chunks_) std::free(c);
  }
  MemoryStore(const MemoryStore&) = delete;
  MemoryStore& operator=(const MemoryStore&) = delete;

  uint8_t* pin(uint32_t pid) { return chunks_[pid >> kChunkShift] + size_t(pid & kChunkMask) * ps_; }
  void unpin(uint32_t, bool) {}
  uint32_t alloc(uint8_t** out) {
    const uint32_t pid = next_pid_++;
    if ((pid >> kChunkShift) >= chunks_.size())
      chunks_.push_back(alloc_aligned(size_t(kChunkPages) * ps_, ps_));
    uint8_t* p = pin(pid);
    std::memset(p, 0, ps_);
    *out = p;
    return pid;
  }
  void flush() {}

  uint32_t num_pages() const { return next_pid_; }
  uint64_t memory_bytes() const {  // RAM held by the index
    return uint64_t(chunks_.size()) * kChunkPages * ps_ + chunks_.capacity() * sizeof(uint8_t*);
  }
  uint64_t file_bytes() const { return 0; }
  uint64_t file_allocated_bytes() const { return 0; }
  bool direct_io() const { return false; }
  static const char* name() { return "memory"; }

  IoStats stats;

 private:
  uint32_t ps_;
  uint32_t next_pid_ = 0;
  std::vector<uint8_t*> chunks_;
};

// ---------------------------------------------------------------------------------------------
// 2) DiskOnlyStore - disk-only B+ tree.
// No caching at all: every pin reads the page from disk, and a modified page is written back
// the moment it is unpinned (write-through). The only RAM used is a small fixed set of frames
// holding the pages of the ONE operation in progress (a root-to-leaf path plus the new pages
// a split creates) - the CPU cannot operate on bytes that are still on disk.
// ---------------------------------------------------------------------------------------------
class DiskOnlyStore {
 public:
  static constexpr uint32_t kFrames = kMaxHeight + 4;
  static constexpr uint32_t kNoPage = 0xFFFFFFFFu;

  DiskOnlyStore(const std::string& path, uint32_t page_size, bool create)
      : ps_(page_size), file_(path, page_size, create, &stats) {
    buf_ = alloc_aligned(size_t(kFrames) * ps_, ps_);
    frames_.resize(kFrames);
    for (uint32_t i = 0; i < kFrames; ++i) frames_[i] = Frame{kNoPage, 0, false, buf_ + size_t(i) * ps_};
    if (create) {
      std::memset(buf_, 0, ps_);
      file_.write_page(0, buf_);  // page 0 = metadata page
      next_pid_ = 1;
    } else {
      next_pid_ = file_.pages_on_disk();
    }
  }
  ~DiskOnlyStore() { std::free(buf_); }
  DiskOnlyStore(const DiskOnlyStore&) = delete;
  DiskOnlyStore& operator=(const DiskOnlyStore&) = delete;

  uint8_t* pin(uint32_t pid) {
    for (Frame& f : frames_)
      if (f.pins > 0 && f.pid == pid) { ++f.pins; return f.data; }  // already held by this op
    Frame& f = free_frame();
    file_.read_page(pid, f.data);  // every node visit = one disk read
    f.pid = pid;
    f.pins = 1;
    f.dirty = false;
    return f.data;
  }

  void unpin(uint32_t pid, bool dirty) {
    Frame& f = held(pid);
    f.dirty |= dirty;
    if (--f.pins == 0) {
      if (f.dirty) file_.write_page(pid, f.data);  // write-through: disk is never stale
      f.pid = kNoPage;
      f.dirty = false;
    }
  }

  uint32_t alloc(uint8_t** out) {
    const uint32_t pid = next_pid_++;
    Frame& f = free_frame();
    std::memset(f.data, 0, ps_);
    f.pid = pid;
    f.pins = 1;
    f.dirty = true;  // reaches the disk when the caller unpins it
    *out = f.data;
    return pid;
  }

  void flush() { file_.sync(); }  // nothing is ever held dirty between operations

  uint32_t num_pages() const { return next_pid_; }
  uint64_t memory_bytes() const { return uint64_t(kFrames) * ps_ + frames_.capacity() * sizeof(Frame); }
  uint64_t file_bytes() const { return file_.size_bytes(); }
  uint64_t file_allocated_bytes() const { return file_.allocated_bytes(); }
  bool direct_io() const { return file_.direct(); }
  static const char* name() { return "disk"; }

  IoStats stats;

 private:
  struct Frame {
    uint32_t pid;
    uint32_t pins;
    bool dirty;
    uint8_t* data;
  };
  Frame& free_frame() {
    for (Frame& f : frames_)
      if (f.pins == 0) return f;
    throw std::runtime_error("disk-only store: all operation frames are pinned");
  }
  Frame& held(uint32_t pid) {
    for (Frame& f : frames_)
      if (f.pins > 0 && f.pid == pid) return f;
    throw std::runtime_error("unpin of a page that is not pinned: " + std::to_string(pid));
  }

  uint32_t ps_;
  PagedFile file_;
  uint8_t* buf_ = nullptr;
  std::vector<Frame> frames_;
  uint32_t next_pid_ = 0;
};

// ---------------------------------------------------------------------------------------------
// 3) BufferPoolStore - memory + disk B+ tree.
// The index lives in a file; a fixed number of RAM frames cache pages.
//   * page table      : vector indexed by page id -> frame number (-1 = not in RAM), O(1)
//   * replacement     : exact LRU over unpinned frames (intrusive doubly linked list)
//   * write policy    : write-back - a modified page is written only when it is evicted
//                       or at flush(); repeated updates to a hot page cost no I/O
//   * pinned frames   : never evicted (they are in use by the current operation)
// Because every operation starts at the root, internal nodes are touched far more often than
// any single leaf, so LRU naturally keeps the upper levels resident.
// ---------------------------------------------------------------------------------------------
class BufferPoolStore {
 public:
  static constexpr uint32_t kNoPage = 0xFFFFFFFFu;

  BufferPoolStore(const std::string& path, uint32_t page_size, uint64_t pool_bytes, bool create)
      : ps_(page_size), file_(path, page_size, create, &stats) {
    nframes_ = uint32_t(pool_bytes / page_size);
    if (nframes_ < uint32_t(2 * kMaxHeight))
      throw std::runtime_error("buffer pool too small: need at least " +
                               std::to_string(2 * kMaxHeight) + " frames");
    buf_ = alloc_aligned(size_t(nframes_) * ps_, ps_);
    frames_.resize(nframes_);
    free_.reserve(nframes_);
    for (uint32_t i = 0; i < nframes_; ++i) {
      frames_[i] = Frame{kNoPage, 0, false, -1, -1};
      free_.push_back(int32_t(nframes_ - 1 - i));
    }
    if (create) {
      std::memset(buf_, 0, ps_);
      file_.write_page(0, buf_);  // page 0 = metadata page
      next_pid_ = 1;
    } else {
      next_pid_ = file_.pages_on_disk();
    }
    table_.assign(next_pid_, -1);
  }
  ~BufferPoolStore() { std::free(buf_); }
  BufferPoolStore(const BufferPoolStore&) = delete;
  BufferPoolStore& operator=(const BufferPoolStore&) = delete;

  uint8_t* pin(uint32_t pid) {
    int32_t fi = table_[pid];
    if (fi >= 0) {  // hit
      stats.hits++;
      Frame& f = frames_[fi];
      if (f.pins == 0) lru_unlink(fi);
      f.pins++;
      count_kind(fi, true);
      return data(fi);
    }
    stats.misses++;  // miss: find a frame, read the page
    fi = victim();
    file_.read_page(pid, data(fi));
    Frame& f = frames_[fi];
    f.pid = pid;
    f.pins = 1;
    f.dirty = false;
    table_[pid] = fi;
    count_kind(fi, false);
    return data(fi);
  }

  void unpin(uint32_t pid, bool dirty) {
    const int32_t fi = table_[pid];
    Frame& f = frames_[fi];
    f.dirty |= dirty;
    if (--f.pins == 0) lru_push_mru(fi);
  }

  uint32_t alloc(uint8_t** out) {
    const uint32_t pid = next_pid_++;
    if (table_.size() <= pid) table_.resize(size_t(pid) + 1, -1);
    const int32_t fi = victim();
    std::memset(data(fi), 0, ps_);
    Frame& f = frames_[fi];
    f.pid = pid;
    f.pins = 1;
    f.dirty = true;  // a new page reaches the disk on eviction or flush
    table_[pid] = fi;
    *out = data(fi);
    return pid;
  }

  void flush() {  // checkpoint: write every dirty page, then make it durable
    for (uint32_t i = 0; i < nframes_; ++i) {
      Frame& f = frames_[i];
      if (f.pid != kNoPage && f.dirty) {
        file_.write_page(f.pid, data(int32_t(i)));
        f.dirty = false;
      }
    }
    file_.sync();
  }

  uint32_t num_pages() const { return next_pid_; }
  uint32_t frames() const { return nframes_; }
  uint64_t memory_bytes() const {
    return uint64_t(nframes_) * ps_ + frames_.capacity() * sizeof(Frame) +
           table_.capacity() * sizeof(int32_t) + free_.capacity() * sizeof(int32_t);
  }
  uint64_t file_bytes() const { return file_.size_bytes(); }
  uint64_t file_allocated_bytes() const { return file_.allocated_bytes(); }
  bool direct_io() const { return file_.direct(); }
  static const char* name() { return "hybrid"; }

  IoStats stats;

 private:
  struct Frame {
    uint32_t pid;
    uint32_t pins;
    bool dirty;
    int32_t prev, next;  // LRU links (only meaningful while pins == 0 and resident)
  };

  uint8_t* data(int32_t fi) { return buf_ + size_t(fi) * ps_; }

  void count_kind(int32_t fi, bool hit) {
    const uint16_t t = ld16(data(fi));
    if (t == kLeaf) (hit ? stats.leaf_hits : stats.leaf_misses)++;
    else if (t == kInternal) (hit ? stats.internal_hits : stats.internal_misses)++;
  }

  int32_t victim() {
    if (!free_.empty()) {
      const int32_t fi = free_.back();
      free_.pop_back();
      return fi;
    }
    const int32_t fi = lru_head_;  // least recently used unpinned frame
    if (fi < 0) throw std::runtime_error("buffer pool exhausted: every frame is pinned");
    lru_unlink(fi);
    Frame& f = frames_[fi];
    stats.evictions++;
    if (f.dirty) {
      file_.write_page(f.pid, data(fi));
      stats.dirty_evictions++;
    }
    table_[f.pid] = -1;
    f.pid = kNoPage;
    f.dirty = false;
    return fi;
  }

  void lru_unlink(int32_t fi) {
    Frame& f = frames_[fi];
    if (f.prev >= 0) frames_[f.prev].next = f.next; else lru_head_ = f.next;
    if (f.next >= 0) frames_[f.next].prev = f.prev; else lru_tail_ = f.prev;
    f.prev = f.next = -1;
  }
  void lru_push_mru(int32_t fi) {
    Frame& f = frames_[fi];
    f.prev = lru_tail_;
    f.next = -1;
    if (lru_tail_ >= 0) frames_[lru_tail_].next = fi; else lru_head_ = fi;
    lru_tail_ = fi;
  }

  uint32_t ps_;
  PagedFile file_;
  uint32_t nframes_ = 0;
  uint8_t* buf_ = nullptr;
  std::vector<Frame> frames_;
  std::vector<int32_t> table_;
  std::vector<int32_t> free_;
  int32_t lru_head_ = -1, lru_tail_ = -1;
  uint32_t next_pid_ = 0;
};

}  // namespace bpt
