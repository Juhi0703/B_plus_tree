// bptree.h - the B+ tree algorithm. Written once, run on any of the three stores.
//
//   * every node is one page (layout.h); nodes refer to each other by 32-bit page id
//   * internal nodes hold separator keys + child page ids, leaves hold (key, payload) records
//   * leaves are chained left-to-right (next pointer) for range scans
//   * keys are fixed-width byte strings compared with memcmp (lexicographic order)
//   * insert = descend, insert into leaf, split upward when full; the tree only grows at the
//     root, so all leaves are always at the same depth (the tree stays balanced)
#pragma once

#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "layout.h"

namespace bpt {

struct LevelStats {
  uint64_t nodes = 0;
  uint64_t entries = 0;  // records (leaves) or separator keys (internal nodes)
};

struct VerifyResult {
  bool ok = true;
  std::string error;
  uint32_t height = 0;
  std::vector<LevelStats> levels;  // levels[0] = leaves, levels.back() = root
  uint64_t records = 0;
  double leaf_fill = 0;      // records / (leaves * leaf_cap)
  double internal_fill = 0;  // keys / (internal nodes * internal_cap)
};

template <class Store>
class BPlusTree {
 public:
  // create = true: start an empty tree (one empty leaf as root).
  // create = false: open the tree whose metadata is stored in page 0 of the store.
  BPlusTree(Store& store, const Layout& layout, bool create)
      : st_(store),
        L_(layout),
        K_(layout.key_size),
        V_(layout.val_size),
        sep_a_(layout.key_size),
        sep_b_(layout.key_size),
        tmp_keys_(size_t(layout.internal_cap + 1) * layout.key_size),
        tmp_kids_(size_t(layout.internal_cap) + 2) {
    if (create) {
      uint8_t* pg;
      root_ = st_.alloc(&pg);
      Node(pg, L_).init(kLeaf, 0, root_);
      st_.unpin(root_, true);
      height_ = 1;
      num_records_ = 0;
      save_meta();
    } else {
      load_meta();
    }
  }

  // ---- insert -------------------------------------------------------------------------------
  // Returns true if the key was new, false if it already existed (its payload is replaced).
  bool insert(const uint8_t* key, const uint8_t* val) {
    uint32_t path_pid[kMaxHeight];  // internal nodes on the root-to-leaf path ...
    uint8_t* path_pg[kMaxHeight];   // ... their (pinned) page buffers ...
    uint32_t path_slot[kMaxHeight];  // ... and which child we followed in each
    int depth = 0;

    // 1. Descend from the root to the leaf that must contain `key`.
    uint32_t pid = root_;
    uint8_t* pg = st_.pin(pid);
    while (ld16(pg) == kInternal) {
      Node n(pg, L_);
      const uint32_t slot = child_slot_for(n, key);
      path_pid[depth] = pid;
      path_pg[depth] = pg;
      path_slot[depth] = slot;
      ++depth;
      pid = n.child(slot);
      pg = st_.pin(pid);
    }

    // 2. Put the record into the leaf (keys stay sorted inside the node).
    Node leaf(pg, L_);
    const uint32_t cnt = leaf.count();
    const uint32_t pos = leaf_lower_bound(leaf, key);
    if (pos < cnt && cmp(leaf.lkey(pos), key) == 0) {  // duplicate key -> update payload
      std::memcpy(leaf.lval(pos), val, V_);
      st_.unpin(pid, true);
      unpin_path(path_pid, depth);
      ++updates_;
      return false;
    }
    ++num_records_;
    if (cnt < L_.leaf_cap) {  // common case: there is room
      leaf_insert_at(leaf, pos, key, val);
      st_.unpin(pid, true);
      unpin_path(path_pid, depth);
      return true;
    }

    // 3. The leaf is full: split it. The first key of the new right leaf is COPIED up.
    uint8_t* npg;
    const uint32_t npid = st_.alloc(&npg);
    Node right(npg, L_);
    right.init(kLeaf, 0, npid);
    split_leaf(leaf, right, npid, pos, key, val);
    uint8_t* sep = sep_a_.data();
    uint8_t* up = sep_b_.data();
    std::memcpy(sep, right.lkey(0), K_);
    st_.unpin(pid, true);
    st_.unpin(npid, true);
    ++leaf_splits_;
    uint32_t right_pid = npid;

    // 4. Insert (separator -> new node) into the parent. A full parent splits too and its
    //    middle key is MOVED up; this can repeat all the way to the root.
    while (depth > 0) {
      --depth;
      Node parent(path_pg[depth], L_);
      const uint32_t slot = path_slot[depth];
      if (parent.count() < L_.internal_cap) {
        internal_insert_at(parent, slot, sep, right_pid);
        st_.unpin(path_pid[depth], true);
        unpin_path(path_pid, depth);  // nodes above are unchanged
        return true;
      }
      const uint16_t level = parent.level();
      uint8_t* rpg;
      const uint32_t rpid = st_.alloc(&rpg);
      Node rnode(rpg, L_);
      rnode.init(kInternal, level, rpid);
      split_internal(parent, rnode, slot, sep, right_pid, up);
      st_.unpin(path_pid[depth], true);
      st_.unpin(rpid, true);
      ++internal_splits_;
      std::swap(sep, up);
      right_pid = rpid;
    }

    // 5. The root split: add a new root above it. This is the only way the tree gets taller.
    if (height_ >= uint32_t(kMaxHeight)) throw std::runtime_error("tree height limit reached");
    uint8_t* rootpg;
    const uint32_t new_root = st_.alloc(&rootpg);
    Node r(rootpg, L_);
    r.init(kInternal, uint16_t(height_), new_root);
    std::memcpy(r.ikey(0), sep, K_);
    r.set_child(0, root_);
    r.set_child(1, right_pid);
    r.set_count(1);
    st_.unpin(new_root, true);
    root_ = new_root;
    ++height_;
    ++root_splits_;
    return true;
  }

  // ---- point lookup ---------------------------------------------------------------------------
  // Exactly `height` page accesses: one node per level, binary search inside each node.
  bool search(const uint8_t* key, uint8_t* val_out) {
    uint32_t pid = root_;
    uint8_t* pg = st_.pin(pid);
    while (ld16(pg) == kInternal) {
      Node n(pg, L_);
      const uint32_t c = n.child(child_slot_for(n, key));
      st_.unpin(pid, false);
      pid = c;
      pg = st_.pin(pid);
    }
    Node leaf(pg, L_);
    const uint32_t pos = leaf_lower_bound(leaf, key);
    const bool found = pos < leaf.count() && cmp(leaf.lkey(pos), key) == 0;
    if (found && val_out) std::memcpy(val_out, leaf.lval(pos), V_);
    st_.unpin(pid, false);
    return found;
  }

  // ---- range scan -----------------------------------------------------------------------------
  // Visit up to `limit` records in key order starting at the first key >= start (nullptr =
  // smallest key). One descent, then the leaf chain: one page access per further leaf.
  template <class Visit>
  uint64_t scan(const uint8_t* start, uint64_t limit, Visit&& visit) {
    if (limit == 0) return 0;
    uint32_t pid = root_;
    uint8_t* pg = st_.pin(pid);
    while (ld16(pg) == kInternal) {
      Node n(pg, L_);
      const uint32_t c = n.child(start ? child_slot_for(n, start) : 0);
      st_.unpin(pid, false);
      pid = c;
      pg = st_.pin(pid);
    }
    uint32_t pos = start ? leaf_lower_bound(Node(pg, L_), start) : 0;
    uint64_t done = 0;
    for (;;) {
      Node leaf(pg, L_);
      const uint32_t cnt = leaf.count();
      for (; pos < cnt && done < limit; ++pos, ++done) visit(leaf.lkey(pos), leaf.lval(pos));
      const uint32_t nxt = leaf.next();
      st_.unpin(pid, false);
      if (done >= limit || nxt == kNullPage) break;
      pid = nxt;
      pg = st_.pin(pid);
      pos = 0;
    }
    return done;
  }

  // ---- metadata (page 0) ------------------------------------------------------------------------
  void save_meta() {
    uint8_t* p = st_.pin(0);
    std::memcpy(p, kMagic, 8);
    st32(p + 8, L_.page_size);
    st32(p + 12, L_.key_size);
    st32(p + 16, L_.val_size);
    st32(p + 20, L_.leaf_cap);
    st32(p + 24, L_.internal_cap);
    st32(p + 28, root_);
    st32(p + 32, height_);
    st32(p + 36, st_.num_pages());
    st64(p + 40, num_records_);
    st_.unpin(0, true);
  }

  // ---- full structural check ---------------------------------------------------------------
  // Visits every node once (depth-first, left to right) and checks: node type/level/self id,
  // counts within capacity, keys sorted and inside the range their parent allows, every leaf
  // at the same depth, leaf chain links each leaf to the next one in key order, and the record
  // count. Also collects per-level node counts and fill factors.
  VerifyResult verify() {
    VerifyResult r;
    r.height = height_;
    r.levels.assign(height_, LevelStats{});
    VState vs;
    vs.last_key.assign(K_, 0);
    dfs(root_, height_ - 1, nullptr, nullptr, r, vs);
    if (r.ok && vs.expected_next != kNullPage) fail(r, "last leaf has a non-null next pointer");
    if (r.ok && r.records != num_records_)
      fail(r, "record count mismatch: found " + std::to_string(r.records) + ", expected " +
                  std::to_string(num_records_));
    if (!r.levels.empty() && r.levels[0].nodes)
      r.leaf_fill = double(r.levels[0].entries) / (double(r.levels[0].nodes) * L_.leaf_cap);
    uint64_t in = 0, ik = 0;
    for (size_t l = 1; l < r.levels.size(); ++l) {
      in += r.levels[l].nodes;
      ik += r.levels[l].entries;
    }
    if (in) r.internal_fill = double(ik) / (double(in) * L_.internal_cap);
    return r;
  }

  uint32_t height() const { return height_; }
  uint64_t size() const { return num_records_; }
  uint32_t root() const { return root_; }
  uint64_t leaf_splits() const { return leaf_splits_; }
  uint64_t internal_splits() const { return internal_splits_; }
  uint64_t root_splits() const { return root_splits_; }
  uint64_t updates() const { return updates_; }
  const Layout& layout() const { return L_; }

 private:
  static constexpr char kMagic[8] = {'B', 'P', 'T', 'R', 'E', 'E', '0', '1'};

  int cmp(const uint8_t* a, const uint8_t* b) const { return std::memcmp(a, b, K_); }

  // first i with key[i] >= k (k's position in a leaf)
  uint32_t leaf_lower_bound(const Node& n, const uint8_t* k) const {
    uint32_t lo = 0, hi = n.count();
    while (lo < hi) {
      const uint32_t mid = (lo + hi) >> 1;
      if (cmp(n.lkey(mid), k) < 0) lo = mid + 1;
      else hi = mid;
    }
    return lo;
  }

  // Child to follow = number of separators <= k. Separator i is a lower bound of everything
  // under child i+1, and every key under child i is smaller than it.
  uint32_t child_slot_for(const Node& n, const uint8_t* k) const {
    uint32_t lo = 0, hi = n.count();
    while (lo < hi) {
      const uint32_t mid = (lo + hi) >> 1;
      if (cmp(n.ikey(mid), k) <= 0) lo = mid + 1;
      else hi = mid;
    }
    return lo;
  }

  void unpin_path(const uint32_t* pids, int depth) {
    for (int i = 0; i < depth; ++i) st_.unpin(pids[i], false);
  }

  void leaf_insert_at(Node& n, uint32_t pos, const uint8_t* k, const uint8_t* v) {
    const uint32_t cnt = n.count();
    if (pos < cnt) {  // shift the tail one slot right to open a gap
      std::memmove(n.lkey(pos + 1), n.lkey(pos), size_t(cnt - pos) * K_);
      std::memmove(n.lval(pos + 1), n.lval(pos), size_t(cnt - pos) * V_);
    }
    std::memcpy(n.lkey(pos), k, K_);
    std::memcpy(n.lval(pos), v, V_);
    n.set_count(cnt + 1);
  }

  // `left` is full. Conceptually insert (k, v) at `pos`, giving cap+1 sorted records; the
  // lower half stays in `left`, the upper half moves to the new leaf `right`.
  void split_leaf(Node& left, Node& right, uint32_t right_pid, uint32_t pos, const uint8_t* k,
                  const uint8_t* v) {
    const uint32_t cap = left.count();
    const uint32_t total = cap + 1;
    const uint32_t ln = total / 2;   // records that stay left
    const uint32_t rn = total - ln;  // records that move right
    for (uint32_t j = ln, r = 0; j < total; ++j, ++r) {  // fill right with merged [ln, total)
      if (j == pos) {
        std::memcpy(right.lkey(r), k, K_);
        std::memcpy(right.lval(r), v, V_);
      } else {
        const uint32_t src = j < pos ? j : j - 1;
        std::memcpy(right.lkey(r), left.lkey(src), K_);
        std::memcpy(right.lval(r), left.lval(src), V_);
      }
    }
    if (pos < ln) {  // the new record belongs to the left half (done after filling right)
      std::memmove(left.lkey(pos + 1), left.lkey(pos), size_t(ln - 1 - pos) * K_);
      std::memmove(left.lval(pos + 1), left.lval(pos), size_t(ln - 1 - pos) * V_);
      std::memcpy(left.lkey(pos), k, K_);
      std::memcpy(left.lval(pos), v, V_);
    }
    left.set_count(ln);
    right.set_count(rn);
    right.set_next(left.next());  // left -> right -> left's old right neighbour
    left.set_next(right_pid);
  }

  // Insert separator `sep` at key index `slot` and the new child right after it.
  void internal_insert_at(Node& n, uint32_t slot, const uint8_t* sep, uint32_t right_pid) {
    const uint32_t cnt = n.count();
    std::memmove(n.ikey(slot + 1), n.ikey(slot), size_t(cnt - slot) * K_);
    std::memcpy(n.ikey(slot), sep, K_);
    std::memmove(n.child_slot(slot + 2), n.child_slot(slot + 1), size_t(cnt - slot) * kPtrSize);
    n.set_child(slot + 1, right_pid);
    n.set_count(cnt + 1);
  }

  // `left` is a full internal node. Insert (sep, right_child) at `slot`, then split: left keeps
  // the lower keys, `right` gets the upper keys, and the middle key is written to `up` (it is
  // moved to the parent, not kept in either half).
  void split_internal(Node& left, Node& right, uint32_t slot, const uint8_t* sep,
                      uint32_t right_child, uint8_t* up) {
    const uint32_t cap = left.count();
    const uint32_t total = cap + 1;  // keys after the insert
    uint8_t* tk = tmp_keys_.data();
    uint32_t* tc = tmp_kids_.data();
    std::memcpy(tk, left.ikey(0), size_t(slot) * K_);
    std::memcpy(tk + size_t(slot) * K_, sep, K_);
    std::memcpy(tk + size_t(slot + 1) * K_, left.ikey(slot), size_t(cap - slot) * K_);
    for (uint32_t i = 0; i <= slot; ++i) tc[i] = left.child(i);
    tc[slot + 1] = right_child;
    for (uint32_t i = slot + 1; i <= cap; ++i) tc[i + 1] = left.child(i);

    const uint32_t mid = total / 2;
    std::memcpy(left.ikey(0), tk, size_t(mid) * K_);  // left: keys [0,mid), children [0,mid]
    for (uint32_t i = 0; i <= mid; ++i) left.set_child(i, tc[i]);
    left.set_count(mid);
    std::memcpy(up, tk + size_t(mid) * K_, K_);  // middle key goes up
    const uint32_t rn = total - mid - 1;         // right: keys (mid,total), children [mid+1,total]
    std::memcpy(right.ikey(0), tk + size_t(mid + 1) * K_, size_t(rn) * K_);
    for (uint32_t i = 0; i <= rn; ++i) right.set_child(i, tc[mid + 1 + i]);
    right.set_count(rn);
  }

  void load_meta() {
    uint8_t* p = st_.pin(0);
    const bool ok = std::memcmp(p, kMagic, 8) == 0 && ld32(p + 8) == L_.page_size &&
                    ld32(p + 12) == L_.key_size && ld32(p + 16) == L_.val_size &&
                    ld32(p + 20) == L_.leaf_cap && ld32(p + 24) == L_.internal_cap;
    if (ok) {
      root_ = ld32(p + 28);
      height_ = ld32(p + 32);
      num_records_ = ld64(p + 40);
    }
    st_.unpin(0, false);
    if (!ok) throw std::runtime_error("index file is missing or was built with another layout");
  }

  struct VState {
    bool first_leaf = true;
    bool have_last = false;
    std::vector<uint8_t> last_key;
    uint32_t expected_next = kNullPage;
  };

  static void fail(VerifyResult& r, const std::string& msg) {
    if (r.ok) {
      r.ok = false;
      r.error = msg;
    }
  }

  void dfs(uint32_t pid, uint32_t level, const uint8_t* lo, const uint8_t* hi, VerifyResult& r,
           VState& vs) {
    if (!r.ok) return;
    const std::string where = "page " + std::to_string(pid) + ": ";
    if (pid == kNullPage || pid >= st_.num_pages()) return fail(r, where + "invalid page id");
    uint8_t* pg = st_.pin(pid);
    Node n(pg, L_);
    const uint32_t cnt = n.count();
    auto bail = [&](const std::string& m) {
      st_.unpin(pid, false);
      fail(r, where + m);
    };
    if (n.self() != pid) return bail("self id mismatch");
    if (uint32_t(n.level()) != level) return bail("node at the wrong level (tree not balanced)");

    if (level == 0) {
      if (n.type() != kLeaf || cnt > L_.leaf_cap) return bail("bad leaf header");
      if (cnt == 0 && num_records_ > 0) return bail("empty leaf in a non-empty tree");
      if (!vs.first_leaf && vs.expected_next != pid) return bail("leaf chain does not reach this leaf");
      for (uint32_t i = 0; i < cnt; ++i) {
        const uint8_t* k = n.lkey(i);
        if ((lo && cmp(k, lo) < 0) || (hi && cmp(k, hi) >= 0)) return bail("key outside parent range");
        if (vs.have_last && cmp(vs.last_key.data(), k) >= 0) return bail("keys not strictly increasing");
        std::memcpy(vs.last_key.data(), k, K_);
        vs.have_last = true;
      }
      vs.first_leaf = false;
      vs.expected_next = n.next();
      r.records += cnt;
      r.levels[0].nodes++;
      r.levels[0].entries += cnt;
      st_.unpin(pid, false);
      return;
    }

    if (n.type() != kInternal || cnt == 0 || cnt > L_.internal_cap) return bail("bad internal header");
    std::vector<uint8_t> keys(size_t(cnt) * K_);  // copy, then release the page before recursing
    std::vector<uint32_t> kids(size_t(cnt) + 1);
    std::memcpy(keys.data(), n.ikey(0), keys.size());
    for (uint32_t i = 0; i <= cnt; ++i) kids[i] = n.child(i);
    st_.unpin(pid, false);
    for (uint32_t i = 0; i < cnt; ++i) {
      const uint8_t* k = &keys[size_t(i) * K_];
      if ((lo && cmp(k, lo) < 0) || (hi && cmp(k, hi) >= 0)) return fail(r, where + "separator outside range");
      if (i > 0 && cmp(&keys[size_t(i - 1) * K_], k) >= 0) return fail(r, where + "separators not sorted");
    }
    r.levels[level].nodes++;
    r.levels[level].entries += cnt;
    for (uint32_t i = 0; i <= cnt && r.ok; ++i) {
      const uint8_t* clo = i == 0 ? lo : &keys[size_t(i - 1) * K_];
      const uint8_t* chi = i == cnt ? hi : &keys[size_t(i) * K_];
      dfs(kids[i], level - 1, clo, chi, r, vs);
    }
  }

  Store& st_;
  Layout L_;
  uint32_t K_, V_;
  uint32_t root_ = kNullPage;
  uint32_t height_ = 0;
  uint64_t num_records_ = 0;
  uint64_t leaf_splits_ = 0, internal_splits_ = 0, root_splits_ = 0, updates_ = 0;
  std::vector<uint8_t> sep_a_, sep_b_;  // separator being pushed up (double-buffered)
  std::vector<uint8_t> tmp_keys_;       // scratch for internal-node splits
  std::vector<uint32_t> tmp_kids_;
};

}  // namespace bpt
