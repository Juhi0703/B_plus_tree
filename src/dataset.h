// dataset.h - loads the CSV (key,payload) into two packed, fixed-width arrays.
//
// The file is read twice: pass 1 finds the number of rows and the longest key and payload,
// which become the fixed slot widths used by the node layout; pass 2 fills the arrays.
// Shorter keys are zero-padded, which keeps memcmp order identical to string order.
#pragma once

#include <strings.h>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace bpt {

struct Dataset {
  uint64_t n = 0;
  uint32_t key_size = 0;  // longest key in the file (bytes)
  uint32_t val_size = 0;  // longest payload in the file (bytes)
  std::vector<uint8_t> keys, vals;
  const uint8_t* key(uint64_t i) const { return keys.data() + i * key_size; }
  const uint8_t* val(uint64_t i) const { return vals.data() + i * val_size; }
  uint64_t bytes() const { return uint64_t(keys.size()) + uint64_t(vals.size()); }
};

// Calls on_row(line_number, key, key_len, payload, payload_len) for each data row. Handles a
// "key,payload" header line, Windows line endings and a missing final newline.
template <class F>
void for_each_row(const std::string& path, uint64_t limit, F&& on_row) {
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) throw std::runtime_error("cannot open dataset " + path + ": " + std::strerror(errno));
  std::vector<char> buf(size_t(4) << 20);
  std::string carry;  // a line split across two reads
  uint64_t lineno = 0, rows = 0;
  bool done = false;
  auto handle = [&](const char* s, size_t len) {
    ++lineno;
    if (len && s[len - 1] == '\r') --len;
    if (len == 0) return;
    const char* comma = static_cast<const char*>(std::memchr(s, ',', len));
    if (!comma) throw std::runtime_error(path + ":" + std::to_string(lineno) + ": no comma");
    const size_t klen = size_t(comma - s), vlen = len - klen - 1;
    if (lineno == 1 && klen == 3 && strncasecmp(s, "key", 3) == 0) return;  // header
    if (rows >= limit) {
      done = true;
      return;
    }
    on_row(lineno, s, klen, comma + 1, vlen);
    ++rows;
  };
  size_t got;
  while (!done && (got = std::fread(buf.data(), 1, buf.size(), f)) > 0) {
    size_t start = 0;
    while (!done) {
      const char* nl = static_cast<const char*>(std::memchr(buf.data() + start, '\n', got - start));
      if (!nl) break;
      const size_t end = size_t(nl - buf.data());
      if (!carry.empty()) {
        carry.append(buf.data() + start, end - start);
        handle(carry.data(), carry.size());
        carry.clear();
      } else {
        handle(buf.data() + start, end - start);
      }
      start = end + 1;
    }
    if (!done) carry.append(buf.data() + start, got - start);
  }
  if (!done && !carry.empty()) handle(carry.data(), carry.size());
  std::fclose(f);
}

inline Dataset load_csv(const std::string& path, uint64_t limit) {
  uint64_t rows = 0;
  size_t maxk = 0, maxv = 0;
  for_each_row(path, limit, [&](uint64_t lineno, const char*, size_t kl, const char*, size_t vl) {
    if (kl == 0) throw std::runtime_error(path + ":" + std::to_string(lineno) + ": empty key");
    ++rows;
    maxk = std::max(maxk, kl);
    maxv = std::max(maxv, vl);
  });
  if (rows == 0) throw std::runtime_error("dataset " + path + " has no rows");
  if (rows > 0xFFFFFFFFull) throw std::runtime_error("more than 2^32 rows is not supported");
  Dataset d;
  d.n = rows;
  d.key_size = uint32_t(maxk);
  d.val_size = uint32_t(std::max<size_t>(maxv, 1));
  d.keys.assign(size_t(rows) * d.key_size, 0);
  d.vals.assign(size_t(rows) * d.val_size, 0);
  uint64_t i = 0;
  for_each_row(path, limit, [&](uint64_t, const char* k, size_t kl, const char* v, size_t vl) {
    std::memcpy(&d.keys[size_t(i) * d.key_size], k, kl);
    std::memcpy(&d.vals[size_t(i) * d.val_size], v, vl);
    ++i;
  });
  return d;
}

}  // namespace bpt
