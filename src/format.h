// SPDX-License-Identifier: MIT
//
// shpack 包格式（v1）
// ---------------------------------------------------------------------------
// 一个 shpack 产物就是一个普通 ELF 可执行文件，结构如下：
//
//   +---------------------------+ 0
//   |  静态链接的 C++ stub      |   （由 build.sh 编译出的 launcher）
//   +---------------------------+ stub_size
//   |  bash 负载                |   （可选 LZ4 压缩）
//   +---------------------------+
//   |  busybox 负载             |
//   +---------------------------+
//   |  shell 脚本负载           |
//   +---------------------------+
//   |  配置字符串表 cfg         |   （NUL 分隔的 "key=value"）
//   +---------------------------+
//   |  Footer (256 字节)        |   文件末尾，固定长度
//   +---------------------------+
//
// stub 在运行时读取自身文件末尾的 Footer，校验 magic / 长度 / SHA-256，
// 解压并校验三个负载，然后按策略落盘（或 memfd）后 exec 内置 bash。
//
// Footer 中所有偏移都是相对于文件起始的绝对偏移，整数一律小端序。
// ---------------------------------------------------------------------------
#pragma once

#include <cstddef>
#include <cstdint>

namespace shpack {

// magic 同时承担版本识别：前 8 字节必须完全一致。
constexpr char kMagic[8] = {'S', 'H', 'P', 'A', 'C', 'K', '0', '1'};
constexpr uint32_t kFormatVersion = 1;
constexpr uint32_t kFooterSize = 256;

// Footer::flags 位定义：每个负载单独标记是否 LZ4 压缩。
enum : uint32_t {
    kFlagBashLz4    = 1u << 0,
    kFlagBusyboxLz4 = 1u << 1,
    kFlagScriptLz4  = 1u << 2,
};

// 单个负载在文件中的位置与校验信息。
struct PayloadRef {
    uint64_t off    = 0;  // 文件中起始偏移
    uint64_t stored = 0;  // 落盘字节数（压缩后大小）
    uint64_t raw    = 0;  // 原始字节数（解压后大小）
    uint8_t  sha[32] = {};  // 原始内容的 SHA-256
};

#pragma pack(push, 1)
struct Footer {
    char     magic[8];        // "SHPACK01"
    uint32_t version;         // kFormatVersion
    uint32_t header_size;     // sizeof(Footer)，用于前向兼容
    uint32_t flags;           // kFlag* 位掩码
    uint32_t cfg_count;       // 配置条目数量
    uint64_t total_size;      // 整个 ELF 文件的大小（必须等于 st_size）

    uint64_t bash_off;
    uint64_t bash_stored;
    uint64_t bash_raw;

    uint64_t bb_off;
    uint64_t bb_stored;
    uint64_t bb_raw;

    uint64_t script_off;
    uint64_t script_stored;
    uint64_t script_raw;

    uint64_t cfg_off;
    uint64_t cfg_size;

    uint8_t  bash_sha[32];
    uint8_t  bb_sha[32];
    uint8_t  script_sha[32];

    uint8_t  build_id[8];     // 随机构建号，便于排查
    uint8_t  footer_sha[16];  // 本 Footer（footer_sha 域置零后）SHA-256 的前 16 字节
    uint8_t  reserved[16];    // 预留，写 0
};
#pragma pack(pop)

static_assert(sizeof(Footer) == kFooterSize, "Footer 必须是 256 字节");

// ---------------------------------------------------------------------------
// 配置表
// ---------------------------------------------------------------------------
// cfg 区是一段连续的字节，按 NUL 切分成 cfg_count 个 "key=value"。
// 已定义的 key（未知 key 会被忽略，保证旧 stub 能读新包）：
//
//   name      显示名 / 默认脚本文件名（不含路径）
//   env       额外环境变量 "K=V"，可重复
//   bashopt   传给 bash 的选项，可重复（插入在脚本路径之前）
//   pathmode  busybox | system，决定 PATH 中 busybox 目录的位置
//   mode      auto | dir | memfd，默认运行时策略
//   cleanup   1 | 0，运行结束后是否删除临时目录
//   argv0     预留给脚本的 $0 提示名（写入 SHPACK_ARGV0 环境变量）
//   version   打包器版本
// ---------------------------------------------------------------------------
constexpr const char* kCfgName     = "name";
constexpr const char* kCfgEnv      = "env";
constexpr const char* kCfgBashOpt  = "bashopt";
constexpr const char* kCfgPathMode = "pathmode";
constexpr const char* kCfgMode     = "mode";
constexpr const char* kCfgCleanup  = "cleanup";
constexpr const char* kCfgArgv0    = "argv0";
constexpr const char* kCfgVersion  = "version";

// 运行时通过环境变量暴露给脚本的信息。
constexpr const char* kEnvSelf     = "SHPACK_SELF";
constexpr const char* kEnvDir      = "SHPACK_DIR";
constexpr const char* kEnvBash     = "SHPACK_BASH";
constexpr const char* kEnvBusybox  = "SHPACK_BUSYBOX";
constexpr const char* kEnvBusyboxFd = "SHPACK_BUSYBOX_FD";
constexpr const char* kEnvScript   = "SHPACK_SCRIPT";
constexpr const char* kEnvMode     = "SHPACK_MODE";
constexpr const char* kEnvName     = "SHPACK_NAME";
constexpr const char* kEnvArgv0    = "SHPACK_ARGV0";
constexpr const char* kEnvOrigPath = "SHPACK_ORIG_PATH";
constexpr const char* kEnvVersion  = "SHPACK_VERSION";

constexpr const char* kToolVersion = "1.0.0";

// 负载在内存中的上限，避免恶意/损坏的 Footer 触发天量分配。
constexpr uint64_t kMaxPayload  = 512ull * 1024 * 1024;
constexpr uint64_t kMaxStored   = 512ull * 1024 * 1024;
constexpr uint64_t kMaxConfig   = 1ull * 1024 * 1024;

}  // namespace shpack
