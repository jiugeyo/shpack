// SPDX-License-Identifier: MIT
//
// Builder 内嵌 bundle：把各架构的 stub / bash / busybox 追加在 builder ELF 尾部。
// 布局： [各条目数据][条目索引][128 字节 Footer]
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace shpack {

constexpr uint32_t kBundleVersion = 1;
constexpr uint32_t kBundleFooterSize = 128;
constexpr uint32_t kBundleMaxEntries = 256;

struct BundleEntryInfo {
    std::string arch;
    std::string kind;  // stub | bash | busybox
    uint32_t flags = 0;
    uint32_t machine = 0;
    uint64_t raw_size = 0;
    uint64_t stored_size = 0;
    uint64_t offset = 0;  // 相对 bundle 起点
    uint8_t sha[32] = {};
    bool lz4() const { return (flags & 1u) != 0; }
};

class Bundle {
public:
    Bundle() = default;
    ~Bundle();
    Bundle(const Bundle&) = delete;
    Bundle& operator=(const Bundle&) = delete;

    // required=false 时，缺失/无效 bundle 返回 false 且 err 说明原因。
    bool load(const std::string& path, bool required, std::string& err);
    bool has(const std::string& arch, const std::string& kind) const;
    bool read(const std::string& arch, const std::string& kind, std::vector<uint8_t>& out,
              std::string& err) const;

    std::vector<std::string> arches() const;
    const std::vector<BundleEntryInfo>& entries() const { return entries_; }
    std::string build_id;
    std::string path;
    uint64_t base = 0;

private:
    int fd_ = -1;
    uint64_t file_size_ = 0;
    std::vector<BundleEntryInfo> entries_;
};

struct BundleItem {
    std::string arch;
    std::string kind;
    std::vector<uint8_t> raw;
    uint32_t machine = 0;
};

// 生成 bundle 文件（不包含 builder ELF 本身）。
bool write_bundle(const std::string& path, const std::vector<BundleItem>& items, std::string& err,
                  std::string& build_id_hex);

}  // namespace shpack
