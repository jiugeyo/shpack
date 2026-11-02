// SPDX-License-Identifier: MIT
// LZ4 块格式（https://github.com/lz4/lz4/blob/dev/doc/lz4_Block_format.md）
// 自实现编解码器，避免任何第三方依赖，保证 stub 可以纯静态、零依赖。
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace shpack {

// 压缩为 LZ4 块。失败（输入过大/内存不足）返回空 vector。
// 注意：即使没有收益也可能比原文更大（LZ4 最坏膨胀约 0.4%）。
std::vector<uint8_t> lz4_compress(const uint8_t* src, size_t len);

// 解压到 dst（容量 cap）。成功时写入 *out_len 并返回 true。
bool lz4_decompress(const uint8_t* src, size_t src_len, uint8_t* dst, size_t cap,
                    size_t* out_len);

}  // namespace shpack
