// LZ4 / SHA-256 自测（仅开发期使用）
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "../src/lz4.h"
#include "../src/sha256.h"

using namespace shpack;

static int test_roundtrip(const std::vector<uint8_t>& src, const char* label) {
    std::vector<uint8_t> comp = lz4_compress(src.data(), src.size());
    if (comp.empty() && !src.empty()) {
        std::printf("FAIL %s: compress returned empty\n", label);
        return 1;
    }
    std::vector<uint8_t> out(src.size() + 16);
    size_t got = 0;
    if (!lz4_decompress(comp.data(), comp.size(), out.data(), out.size(), &got)) {
        std::printf("FAIL %s: decompress error (src=%zu comp=%zu)\n", label, src.size(),
                    comp.size());
        return 1;
    }
    if (got != src.size()) {
        std::printf("FAIL %s: size mismatch got=%zu want=%zu\n", label, got, src.size());
        return 1;
    }
    if (src.empty()) return 0;
    if (std::memcmp(out.data(), src.data(), src.size()) != 0) {
        size_t i = 0;
        while (i < src.size() && out[i] == src[i]) ++i;
        std::printf("FAIL %s: content mismatch at %zu\n", label, i);
        return 1;
    }
    return 0;
}

int main() {
    int fails = 0;
    std::mt19937 rng(12345);
    for (size_t n : {0, 1, 2, 3, 4, 5, 11, 12, 13, 14, 15, 16, 17, 31, 32, 63, 64, 100, 255,
                     256, 1000, 4096, 65535, 65536, 100000, 1 << 20}) {
        std::vector<uint8_t> random(n), rep(n);
        for (size_t i = 0; i < n; ++i) {
            random[i] = static_cast<uint8_t>(rng());
            rep[i] = static_cast<uint8_t>(i % 7);
        }
        const std::string l1 = "random-" + std::to_string(n);
        const std::string l2 = "repeat-" + std::to_string(n);
        fails += test_roundtrip(random, l1.c_str());
        fails += test_roundtrip(rep, l2.c_str());
    }
    // 全零（长匹配，offset=1）
    for (size_t n : {100, 100000}) {
        std::vector<uint8_t> z(n, 0);
        const std::string l = "zero-" + std::to_string(n);
        fails += test_roundtrip(z, l.c_str());
    }
    // 重复文本
    std::string base = "the quick brown fox jumps over the lazy dog\n";
    std::string text;
    for (int i = 0; i < 5000; ++i) text += base;
    fails += test_roundtrip(std::vector<uint8_t>(text.begin(), text.end()), "text");

    // SHA-256 已知向量："abc"
    uint8_t d[32];
    sha256("abc", 3, d);
    const char* want = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
    char hex[65];
    sha256_hex("abc", 3, hex);
    if (std::strcmp(hex, want) != 0) {
        std::printf("FAIL sha256: %s\n", hex);
        ++fails;
    }

    std::printf(fails ? "FAILED (%d)\n" : "ALL OK\n", fails);
    return fails ? 1 : 0;
}
