// SPDX-License-Identifier: MIT
//
// 打包核心：把 stub + bash + busybox + 脚本组装成 shpack 产物。
// packer（文件版）与 builder（单文件交互版）共用。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace shpack {

struct PackageOptions {
    std::string name = "app";
    std::string mode = "auto";         // auto | memfd | dir
    std::string pathmode = "busybox";  // busybox | system
    std::string argv0;
    std::string compress = "auto";     // auto | none | lz4
    bool cleanup = true;
    bool allow_dynamic = false;
    std::vector<std::string> envs;
    std::vector<std::string> bashopts;
};

struct PackageInputs {
    std::vector<uint8_t> stub;
    std::vector<uint8_t> bash;
    std::vector<uint8_t> busybox;
    std::vector<uint8_t> script;
};

struct BlobStat {
    uint64_t raw = 0;
    uint64_t stored = 0;
    bool lz4 = false;
    std::string desc;
};

struct PackageResult {
    std::vector<uint8_t> image;
    BlobStat bash;
    BlobStat busybox;
    BlobStat script;
    uint8_t build_id[8] = {};
    uint32_t cfg_count = 0;
    uint64_t cfg_size = 0;
    std::string name;
};

// 校验输入并组装；失败时返回 false 并填充 err。
bool assemble_package(const PackageInputs& in, const PackageOptions& opt, PackageResult& out,
                      std::string& err);

}  // namespace shpack
