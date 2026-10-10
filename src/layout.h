// layout.h - on-page format of a B+ tree node, shared by all three variants.
//
// One B+ tree node == one page. The page size is NOT hard-coded: it is detected at run time
// (sysconf(_SC_PAGESIZE), see sysutil.h) or passed with --page-size, and the capacity of each
// node type (the B+ tree "order" / fan-out) is derived from it together with the key and
// payload widths found in the dataset. Change the page size and the fan-out follows.
#pragma once

#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>

namespace bpt {

constexpr uint32_t kHeaderSize = 16;  // every node starts with a 16-byte header
constexpr uint32_t kPtrSize = 4;      // a child pointer is a 32-bit page id (max 2^32 pages)
constexpr uint32_t kNullPage = 0;     // page 0 holds tree metadata, so 0 also means "no page"
constexpr int kMaxHeight = 64;

enum : uint16_t { kLeaf = 1, kInternal = 2 };

// memcpy-based loads/stores: no alignment or strict-aliasing issues, compile to a single mov.
inline uint16_t ld16(const uint8_t* p) { uint16_t v; std::memcpy(&v, p, 2); return v; }
inline uint32_t ld32(const uint8_t* p) { uint32_t v; std::memcpy(&v, p, 4); return v; }
inline uint64_t ld64(const uint8_t* p) { uint64_t v; std::memcpy(&v, p, 8); return v; }
inline void st16(uint8_t* p, uint16_t v) { std::memcpy(p, &v, 2); }
inline void st32(uint8_t* p, uint32_t v) { std::memcpy(p, &v, 4); }
inline void st64(uint8_t* p, uint64_t v) { std::memcpy(p, &v, 8); }

// ---------------------------------------------------------------------------------------------
// Node layout (P = page size, H = 16, K = key width, V = payload width, ptr = 4)
//
//   Header, both node types (16 bytes)
//     [0,2)   type    1 = leaf, 2 = internal
//     [2,4)   level   0 for leaves; a parent is one level above its children
//     [4,8)   count   leaf: number of records;  internal: number of keys (children = keys + 1)
//     [8,12)  self    the node's own page id (sanity check while verifying)
//     [12,16) next    leaf: page id of the right sibling (0 = none); unused in internal nodes
//
//   Leaf     : header | key[0 .. leaf_cap)            | payload[0 .. leaf_cap)
//   Internal : header | child[0 .. internal_cap] (u32) | key[0 .. internal_cap)
//
// Keys and payloads are stored as two separate arrays ("struct of arrays"), so the binary
// search inside a node only touches the contiguous key array.
// ---------------------------------------------------------------------------------------------
struct Layout {
  uint32_t page_size = 0;
  uint32_t key_size = 0;
  uint32_t val_size = 0;
  uint32_t leaf_cap = 0;      // max records per leaf
  uint32_t internal_cap = 0;  // max keys per internal node -> fan-out = internal_cap + 1
  uint32_t leaf_keys_off = 0, leaf_vals_off = 0;
  uint32_t int_child_off = 0, int_keys_off = 0;

  uint32_t fanout() const { return internal_cap + 1; }
  uint32_t leaf_bytes_used() const { return kHeaderSize + leaf_cap * (key_size + val_size); }
  uint32_t internal_bytes_used() const {
    return kHeaderSize + (internal_cap + 1) * kPtrSize + internal_cap * key_size;
  }

  // Derive node capacities from the page size:
  //   leaf     : H + n*(K + V)          <= P  ->  n = floor((P - H) / (K + V))
  //   internal : H + (n+1)*ptr + n*K    <= P  ->  n = floor((P - H - ptr) / (K + ptr))
  // max_leaf / max_fanout (0 = off) can only LOWER the capacities. The tests use them to force
  // tiny nodes, which gives deep trees with lots of splits.
  static Layout compute(uint32_t page_size, uint32_t key_size, uint32_t val_size,
                        uint32_t max_leaf = 0, uint32_t max_fanout = 0) {
    if (page_size < 128 || page_size > (1u << 20) || (page_size & (page_size - 1)) != 0)
      throw std::runtime_error("page size must be a power of two between 128 B and 1 MB, got " +
                               std::to_string(page_size));
    if (key_size == 0 || val_size == 0)
      throw std::runtime_error("key and payload widths must be at least 1 byte");
    Layout L;
    L.page_size = page_size;
    L.key_size = key_size;
    L.val_size = val_size;
    const int64_t P = page_size, H = kHeaderSize, K = key_size, V = val_size, ptr = kPtrSize;
    const int64_t leaf = (P - H) / (K + V);
    const int64_t internal = (P - H - ptr) / (K + ptr);
    if (leaf < 2 || internal < 2)
      throw std::runtime_error("page of " + std::to_string(page_size) +
                               " B is too small for key=" + std::to_string(key_size) +
                               " B, payload=" + std::to_string(val_size) + " B");
    L.leaf_cap = uint32_t(leaf);
    L.internal_cap = uint32_t(internal);
    if (max_leaf >= 2 && max_leaf < L.leaf_cap) L.leaf_cap = max_leaf;
    if (max_fanout >= 3 && max_fanout - 1 < L.internal_cap) L.internal_cap = max_fanout - 1;
    L.leaf_keys_off = kHeaderSize;
    L.leaf_vals_off = kHeaderSize + L.leaf_cap * key_size;
    L.int_child_off = kHeaderSize;  // 4-byte aligned because the header is 16 bytes
    L.int_keys_off = kHeaderSize + (L.internal_cap + 1) * kPtrSize;
    return L;
  }
};

// A typed view over one page buffer. It owns nothing; the page belongs to a store.
class Node {
 public:
  Node(uint8_t* page, const Layout& L) : p_(page), L_(&L) {}

  uint16_t type() const { return ld16(p_); }
  bool is_leaf() const { return type() == kLeaf; }
  uint16_t level() const { return ld16(p_ + 2); }
  uint32_t count() const { return ld32(p_ + 4); }
  uint32_t self() const { return ld32(p_ + 8); }
  uint32_t next() const { return ld32(p_ + 12); }
  void set_count(uint32_t c) { st32(p_ + 4, c); }
  void set_next(uint32_t pid) { st32(p_ + 12, pid); }
  void init(uint16_t type, uint16_t level, uint32_t self) {
    st16(p_, type);
    st16(p_ + 2, level);
    st32(p_ + 4, 0);
    st32(p_ + 8, self);
    st32(p_ + 12, kNullPage);
  }

  // leaf entries
  uint8_t* lkey(uint32_t i) const { return p_ + L_->leaf_keys_off + size_t(i) * L_->key_size; }
  uint8_t* lval(uint32_t i) const { return p_ + L_->leaf_vals_off + size_t(i) * L_->val_size; }

  // internal entries
  uint8_t* ikey(uint32_t i) const { return p_ + L_->int_keys_off + size_t(i) * L_->key_size; }
  uint8_t* child_slot(uint32_t i) const { return p_ + L_->int_child_off + size_t(i) * kPtrSize; }
  uint32_t child(uint32_t i) const { return ld32(child_slot(i)); }
  void set_child(uint32_t i, uint32_t pid) { st32(child_slot(i), pid); }

 private:
  uint8_t* p_;
  const Layout* L_;
};

}  // namespace bpt
