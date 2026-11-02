// SPDX-License-Identifier: MIT
#include "lz4.h"

#include <cstring>

namespace shpack {
namespace {

constexpr size_t kMaxInput = 0x7E000000u;  // LZ4 块格式上限
constexpr int    kHashLog  = 16;
constexpr size_t kHashSize = size_t(1) << kHashLog;

inline uint32_t read32(const uint8_t* p) {
    uint32_t v;
    std::memcpy(&v, p, 4);  // 仅用于哈希/比较，不要求网络序
    return v;
}

inline uint32_t hash4(uint32_t v) { return (v * 2654435761u) >> (32 - kHashLog); }

inline void put_len(std::vector<uint8_t>& out, size_t len, uint8_t& token, bool high) {
    if (len >= 15) {
        if (high) token |= 0xF0;
        else token |= 0x0F;
        size_t rest = len - 15;
        while (rest >= 255) {
            out.push_back(255);
            rest -= 255;
        }
        out.push_back(static_cast<uint8_t>(rest));
    } else if (high) {
        token |= static_cast<uint8_t>(len << 4);
    } else {
        token |= static_cast<uint8_t>(len);
    }
}

}  // namespace

std::vector<uint8_t> lz4_compress(const uint8_t* src, size_t len) {
    std::vector<uint8_t> out;
    if (len > kMaxInput) return {};
    out.reserve(len + len / 255 + 32);

    if (len == 0) {
        out.push_back(0);  // 一个空序列
        return out;
    }

    std::vector<int32_t> table(kHashSize, -1);
    size_t anchor = 0;
    size_t ip = 0;
    const size_t mflimit = (len > 12) ? len - 12 : 0;
    const size_t matchlimit = (len > 5) ? len - 5 : 0;

    if (len >= 13) {
        while (ip < mflimit) {
            const uint32_t seq = read32(src + ip);
            const uint32_t h = hash4(seq);
            const int32_t ref = table[h];
            table[h] = static_cast<int32_t>(ip);

            if (ref < 0 || ip - static_cast<size_t>(ref) > 65535 ||
                read32(src + ref) != seq) {
                ++ip;
                continue;
            }

            // 向前回溯，把匹配起点尽量左移，提高压缩率。
            size_t mip = ip;
            size_t mref = static_cast<size_t>(ref);
            while (mip > anchor && mref > 0 && src[mip - 1] == src[mref - 1]) {
                --mip;
                --mref;
            }

            size_t match_len = 4;
            const size_t max_len = (matchlimit > mip) ? matchlimit - mip : 0;
            if (max_len < 4) {
                ++ip;
                continue;
            }
            while (match_len < max_len && src[mip + match_len] == src[mref + match_len]) {
                ++match_len;
            }
            if (match_len < 4) {  // 理论上不会发生，防御性处理
                ++ip;
                continue;
            }

            const size_t lit_len = mip - anchor;
            const size_t offset = mip - mref;
            const size_t token_pos = out.size();
            out.push_back(0);  // token 占位，稍后回填
            uint8_t token = 0;
            put_len(out, lit_len, token, true);
            out.insert(out.end(), src + anchor, src + mip);
            out.push_back(static_cast<uint8_t>(offset & 0xFF));
            out.push_back(static_cast<uint8_t>((offset >> 8) & 0xFF));
            put_len(out, match_len - 4, token, false);
            out[token_pos] = token;

            ip = mip + match_len;
            anchor = ip;
            if (ip >= mflimit) break;
            table[hash4(read32(src + ip - 2))] = static_cast<int32_t>(ip - 2);
        }
    }

    // 收尾：剩余字节全部作为字面量，不带 offset（解码器据此判断结束）。
    const size_t lit_len = len - anchor;
    const size_t token_pos = out.size();
    out.push_back(0);  // 先占位 token，否则会被第一个字面量覆盖
    uint8_t token = 0;
    put_len(out, lit_len, token, true);
    out.insert(out.end(), src + anchor, src + len);
    out[token_pos] = token;

    return out;
}

bool lz4_decompress(const uint8_t* src, size_t src_len, uint8_t* dst, size_t cap,
                    size_t* out_len) {
    if (src_len == 0) return false;

    const uint8_t* ip = src;
    const uint8_t* const iend = src + src_len;
    uint8_t* op = dst;
    uint8_t* const oend = dst + cap;

    while (ip < iend) {
        const uint8_t token = *ip++;
        size_t lit_len = token >> 4;
        if (lit_len == 15) {
            for (;;) {
                if (ip >= iend) return false;
                const uint8_t b = *ip++;
                lit_len += b;
                if (b != 255) break;
            }
        }
        if (lit_len > static_cast<size_t>(iend - ip)) return false;
        if (lit_len > static_cast<size_t>(oend - op)) return false;
        std::memcpy(op, ip, lit_len);
        op += lit_len;
        ip += lit_len;

        if (ip == iend) {  // 正常结束：最后一段只有字面量
            *out_len = static_cast<size_t>(op - dst);
            return true;
        }
        if (iend - ip < 2) return false;

        const size_t offset = static_cast<size_t>(ip[0]) | (static_cast<size_t>(ip[1]) << 8);
        ip += 2;
        if (offset == 0 || offset > static_cast<size_t>(op - dst)) return false;

        size_t match_len = token & 0x0F;
        if (match_len == 15) {
            for (;;) {
                if (ip >= iend) return false;
                const uint8_t b = *ip++;
                match_len += b;
                if (b != 255) break;
            }
        }
        match_len += 4;
        if (match_len > static_cast<size_t>(oend - op)) return false;

        // 允许重叠复制（offset 可以小于 match_len）。
        uint8_t* m = op - offset;
        for (size_t i = 0; i < match_len; ++i) *op++ = m[i];
    }

    *out_len = static_cast<size_t>(op - dst);
    return true;
}

}  // namespace shpack
