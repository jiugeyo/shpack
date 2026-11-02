// SPDX-License-Identifier: MIT
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <sys/types.h>
#include <vector>

namespace shpack {

// ---- 文件 / 路径 ----------------------------------------------------------

// 读取整个普通文件；超过 max_size 或非普通文件时失败。
bool read_file(const std::string& path, std::vector<uint8_t>& out, uint64_t max_size,
               std::string& err);

bool pread_all(int fd, uint64_t off, void* buf, size_t len);
bool write_all(int fd, const void* buf, size_t len);

// 原子写文件：临时文件 + fsync + rename，成功后权限为 mode。
bool write_file_atomic(const std::string& path, const void* data, size_t len, mode_t mode,
                       std::string& err);

std::string path_join(const std::string& dir, const std::string& name);
std::string base_name(const std::string& path);
std::string dir_name(const std::string& path);
std::string absolute_path(const std::string& path);

// 定位当前可执行文件（/proc/self/exe → AT_EXECFN → argv[0]/PATH）。
std::string executable_path(const char* argv0);

// 去掉 '/'、控制字符等危险字符，返回可用于文件名的字符串。
std::string sanitize_component(const std::string& s, const std::string& fallback);

// 递归删除目录（仅用于自己创建的临时目录）。
void remove_tree(const std::string& dir);

std::string errno_msg(int err, const std::string& what);

// ---- ELF -----------------------------------------------------------------

struct ElfInfo {
    int      elf_class = 0;       // 1=32 位, 2=64 位
    uint16_t type = 0;            // ET_EXEC / ET_DYN / ET_REL ...
    uint16_t machine = 0;
    bool     has_interp = false;  // 存在 PT_INTERP => 动态链接
};

// 解析 ELF 头与程序头，判断是否可在无加载器环境直接运行。
bool elf_inspect(const std::vector<uint8_t>& data, ElfInfo& info, std::string& err);
bool elf_is_static(const std::vector<uint8_t>& data, std::string& err);
const char* elf_machine_name(uint16_t machine);

// ---- 杂项 ----------------------------------------------------------------

std::string to_hex(const uint8_t* p, size_t n);
std::string to_hex(const std::vector<uint8_t>& v);
std::string human_size(uint64_t n);
bool parse_u64(const std::string& s, uint64_t& out);
std::vector<std::string> split(const std::string& s, char sep);

}  // namespace shpack
