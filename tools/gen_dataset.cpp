// gen_dataset.cpp - writes a synthetic dataset in the assignment's format, for testing and for
// reproducing the benchmark without the original file:
//
//   key,payload
//   K026A13E,vwWk6rHLNHGb78UpfkPxeoLWTpUk72dIfP6I7Tv9Ej3yxqanwNUzsXUlqYfBhB9p
//
//   key     = 'K' + 7 upper-case hex digits, unique, rows in random (shuffled) order
//   payload = 64 random characters from [0-9A-Za-z]
//
// usage: gen_dataset N out.csv [seed] [--sparse]
//   default  : the key numbers are a random permutation of 0..N-1 (the sample rows given in the
//              assignment, K026A13E and K00B8035, both decode to numbers below 10,000,000)
//   --sparse : key numbers are drawn without replacement from the whole 16^7 range
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <numeric>
#include <string>
#include <vector>

static uint64_t splitmix(uint64_t& s) {
  uint64_t z = (s += 0x9E3779B97F4A7C15ull);
  z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
  z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
  return z ^ (z >> 31);
}

int main(int argc, char** argv) {
  if (argc < 3) {
    std::fprintf(stderr, "usage: %s N out.csv [seed] [--sparse]\n", argv[0]);
    return 2;
  }
  const uint64_t n = std::strtoull(argv[1], nullptr, 10);
  const char* path = argv[2];
  uint64_t seed = 20261009;
  bool sparse = false;
  for (int i = 3; i < argc; ++i) {
    if (std::strcmp(argv[i], "--sparse") == 0) sparse = true;
    else seed = std::strtoull(argv[i], nullptr, 10);
  }
  const uint64_t space = 1ull << 28;  // 16^7 possible 7-hex-digit numbers
  if (n == 0 || n > space) {
    std::fprintf(stderr, "N must be between 1 and %llu\n", (unsigned long long)space);
    return 2;
  }

  uint64_t s = seed;
  std::vector<uint32_t> keys(n);
  if (!sparse) {
    std::iota(keys.begin(), keys.end(), 0u);
    for (uint64_t i = n - 1; i > 0; --i) std::swap(keys[i], keys[splitmix(s) % (i + 1)]);  // Fisher-Yates
  } else {
    std::vector<uint64_t> used(space / 64, 0);
    for (uint64_t i = 0; i < n;) {
      const uint32_t k = uint32_t(splitmix(s) & (space - 1));
      if (used[k >> 6] & (1ull << (k & 63))) continue;
      used[k >> 6] |= 1ull << (k & 63);
      keys[i++] = k;
    }
  }

  FILE* f = std::fopen(path, "wb");
  if (!f) {
    std::perror(path);
    return 1;
  }
  std::vector<char> obuf(size_t(8) << 20);
  std::setvbuf(f, obuf.data(), _IOFBF, obuf.size());
  std::fputs("key,payload\n", f);
  static const char hex[] = "0123456789ABCDEF";
  static const char alnum[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
  char line[74];
  for (uint64_t i = 0; i < n; ++i) {
    line[0] = 'K';
    for (int d = 0; d < 7; ++d) line[1 + d] = hex[(keys[i] >> (4 * (6 - d))) & 15];
    line[8] = ',';
    for (int w = 0; w < 8; ++w) {  // 8 chars from each 64-bit random number
      uint64_t r = splitmix(s);
      for (int c = 0; c < 8; ++c, r /= 62) line[9 + w * 8 + c] = alnum[r % 62];
    }
    line[73] = '\n';
    std::fwrite(line, 1, sizeof line, f);
  }
  if (std::fclose(f) != 0) {
    std::perror(path);
    return 1;
  }
  std::fprintf(stderr, "wrote %llu rows to %s\n", (unsigned long long)n, path);
  return 0;
}
