// SPDX-License-Identifier: MIT
// SHA-256（FIPS 180-4）最小实现，供打包器与 stub 共用。
#pragma once

#include <cstddef>
#include <cstdint>

namespace shpack {

class Sha256 {
public:
    Sha256() { reset(); }
    void reset();
    void update(const void* data, size_t len);
    void final(uint8_t out[32]);

private:
    void block(const uint8_t* p);

    uint32_t h_[8];
    uint64_t bits_;
    uint8_t  buf_[64];
    size_t   used_;
};

void sha256(const void* data, size_t len, uint8_t out[32]);

// 返回 64 个小写十六进制字符。
void sha256_hex(const void* data, size_t len, char out[65]);

}  // namespace shpack
