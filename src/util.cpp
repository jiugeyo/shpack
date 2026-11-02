// SPDX-License-Identifier: MIT
#include "util.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>

namespace shpack {
namespace {

template <typename T>
T load_le(const uint8_t* p) {
    T v = 0;
    for (size_t i = 0; i < sizeof(T); ++i) v |= static_cast<T>(p[i]) << (8 * i);
    return v;
}

}  // namespace

bool read_file(const std::string& path, std::vector<uint8_t>& out, uint64_t max_size,
               std::string& err) {
    int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        err = errno_msg(errno, "打开 " + path);
        return false;
    }
    struct stat st;
    if (::fstat(fd, &st) != 0) {
        err = errno_msg(errno, "fstat " + path);
        ::close(fd);
        return false;
    }
    if (!S_ISREG(st.st_mode)) {
        err = path + " 不是普通文件";
        ::close(fd);
        return false;
    }
    if (st.st_size < 0 || static_cast<uint64_t>(st.st_size) > max_size) {
        err = path + " 过大（上限 " + human_size(max_size) + "）";
        ::close(fd);
        return false;
    }
    out.clear();
    out.resize(static_cast<size_t>(st.st_size));
    size_t done = 0;
    while (done < out.size()) {
        ssize_t n = ::read(fd, out.data() + done, out.size() - done);
        if (n < 0) {
            if (errno == EINTR) continue;
            err = errno_msg(errno, "读取 " + path);
            ::close(fd);
            return false;
        }
        if (n == 0) break;  // 文件被截断
        done += static_cast<size_t>(n);
    }
    ::close(fd);
    if (done != out.size()) {
        err = path + " 读取不完整";
        return false;
    }
    return true;
}

bool pread_all(int fd, uint64_t off, void* buf, size_t len) {
    uint8_t* p = static_cast<uint8_t*>(buf);
    size_t done = 0;
    while (done < len) {
        ssize_t n = ::pread(fd, p + done, len - done, static_cast<off_t>(off + done));
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (n == 0) return false;
        done += static_cast<size_t>(n);
    }
    return true;
}

bool write_all(int fd, const void* buf, size_t len) {
    const uint8_t* p = static_cast<const uint8_t*>(buf);
    size_t done = 0;
    while (done < len) {
        ssize_t n = ::write(fd, p + done, len - done);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        done += static_cast<size_t>(n);
    }
    return true;
}

std::string path_join(const std::string& dir, const std::string& name) {
    if (dir.empty()) return name;
    if (dir.back() == '/') return dir + name;
    return dir + "/" + name;
}

std::string base_name(const std::string& path) {
    if (path.empty()) return ".";
    size_t end = path.size();
    while (end > 1 && path[end - 1] == '/') --end;
    size_t pos = path.rfind('/', end - 1);
    if (pos == std::string::npos) return path.substr(0, end);
    return path.substr(pos + 1, end - pos - 1);
}

std::string dir_name(const std::string& path) {
    size_t end = path.size();
    while (end > 1 && path[end - 1] == '/') --end;
    size_t pos = path.rfind('/', end - 1);
    if (pos == std::string::npos) return ".";
    if (pos == 0) return "/";
    return path.substr(0, pos);
}

std::string absolute_path(const std::string& path) {
    char buf[PATH_MAX];
    if (::realpath(path.c_str(), buf) != nullptr) return std::string(buf);
    if (!path.empty() && path[0] == '/') return path;
    char cwd[PATH_MAX];
    if (::getcwd(cwd, sizeof(cwd)) == nullptr) return path;
    return path_join(cwd, path);
}

std::string sanitize_component(const std::string& s, const std::string& fallback) {
    std::string out;
    for (unsigned char c : s) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '.' || c == '_' || c == '-') {
            out.push_back(static_cast<char>(c));
        } else if (c == '/' || c == ' ') {
            out.push_back('_');
        }
        if (out.size() >= 64) break;
    }
    while (!out.empty() && out.front() == '.') out.erase(out.begin());
    if (out.empty()) out = fallback;
    return out;
}

void remove_tree(const std::string& dir) {
    DIR* d = ::opendir(dir.c_str());
    if (d == nullptr) {
        ::unlink(dir.c_str());
        return;
    }
    std::vector<std::string> subdirs;
    struct dirent* e;
    while ((e = ::readdir(d)) != nullptr) {
        const char* n = e->d_name;
        if (strcmp(n, ".") == 0 || strcmp(n, "..") == 0) continue;
        const std::string p = path_join(dir, n);
        struct stat st;
        if (::lstat(p.c_str(), &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            subdirs.push_back(p);
        } else {
            ::unlink(p.c_str());
        }
    }
    ::closedir(d);
    for (const auto& s : subdirs) remove_tree(s);
    ::rmdir(dir.c_str());
}

std::string errno_msg(int err, const std::string& what) {
    return what + ": " + ::strerror(err) + " (errno=" + std::to_string(err) + ")";
}

bool elf_inspect(const std::vector<uint8_t>& data, ElfInfo& info, std::string& err) {
    info = ElfInfo{};
    if (data.size() < 52) {
        err = "文件太小，不是有效的 ELF";
        return false;
    }
    if (data[0] != 0x7f || data[1] != 'E' || data[2] != 'L' || data[3] != 'F') {
        err = "缺少 ELF 魔数（不是 ELF 文件）";
        return false;
    }
    info.elf_class = data[4];
    const uint8_t elf_data = data[5];
    if (elf_data != 1) {
        err = "仅支持小端序 ELF（当前目标为 x86_64/aarch64 等）";
        return false;
    }
    if (info.elf_class != 1 && info.elf_class != 2) {
        err = "未知的 ELF class";
        return false;
    }

    uint64_t phoff;
    uint16_t phentsize, phnum;
    if (info.elf_class == 2) {
        if (data.size() < 64) {
            err = "ELF64 头不完整";
            return false;
        }
        info.type = load_le<uint16_t>(data.data() + 16);
        info.machine = load_le<uint16_t>(data.data() + 18);
        phoff = load_le<uint64_t>(data.data() + 32);
        phentsize = load_le<uint16_t>(data.data() + 54);
        phnum = load_le<uint16_t>(data.data() + 56);
    } else {
        info.type = load_le<uint16_t>(data.data() + 16);
        info.machine = load_le<uint16_t>(data.data() + 18);
        phoff = load_le<uint32_t>(data.data() + 28);
        phentsize = load_le<uint16_t>(data.data() + 42);
        phnum = load_le<uint16_t>(data.data() + 44);
    }
    if (info.type != 2 && info.type != 3) {  // ET_EXEC / ET_DYN
        err = "ELF 类型不是可执行文件（ET_EXEC/ET_DYN）";
        return false;
    }
    if (phnum == 0xffff) {
        err = "不支持 PN_XNUM 形式的程序头";
        return false;
    }
    if (phentsize < (info.elf_class == 2 ? 56u : 32u) || phnum == 0) {
        err = "ELF 程序头表异常";
        return false;
    }
    if (phoff > data.size() ||
        static_cast<uint64_t>(phnum) * phentsize > data.size() - phoff) {
        err = "ELF 程序头表越界";
        return false;
    }

    for (uint16_t i = 0; i < phnum; ++i) {
        const uint8_t* ph = data.data() + phoff + static_cast<uint64_t>(i) * phentsize;
        const uint32_t p_type = load_le<uint32_t>(ph);
        if (p_type == 3) {  // PT_INTERP
            info.has_interp = true;
            break;
        }
    }
    return true;
}

bool elf_is_static(const std::vector<uint8_t>& data, std::string& err) {
    ElfInfo info;
    if (!elf_inspect(data, info, err)) return false;
    if (info.has_interp) {
        err = "是动态链接可执行文件（存在 PT_INTERP），需要目标系统提供对应的 ld.so 与 libc";
        return false;
    }
    return true;
}

const char* elf_machine_name(uint16_t machine) {
    switch (machine) {
        case 3: return "i386";
        case 8: return "MIPS";
        case 20: return "PowerPC";
        case 21: return "PowerPC64";
        case 40: return "ARM";
        case 62: return "x86_64";
        case 183: return "aarch64";
        case 243: return "riscv";
        case 258: return "loongarch";
        default: return "unknown";
    }
}

std::string to_hex(const uint8_t* p, size_t n) {
    static const char* d = "0123456789abcdef";
    std::string s;
    s.resize(n * 2);
    for (size_t i = 0; i < n; ++i) {
        s[i * 2] = d[p[i] >> 4];
        s[i * 2 + 1] = d[p[i] & 0x0f];
    }
    return s;
}

std::string to_hex(const std::vector<uint8_t>& v) { return to_hex(v.data(), v.size()); }

std::string human_size(uint64_t n) {
    const char* units[] = {"B", "KiB", "MiB", "GiB"};
    double v = static_cast<double>(n);
    int u = 0;
    while (v >= 1024.0 && u < 3) {
        v /= 1024.0;
        ++u;
    }
    char buf[64];
    if (u == 0) {
        snprintf(buf, sizeof(buf), "%llu B", static_cast<unsigned long long>(n));
    } else {
        snprintf(buf, sizeof(buf), "%.1f %s", v, units[u]);
    }
    return buf;
}

bool parse_u64(const std::string& s, uint64_t& out) {
    if (s.empty()) return false;
    char* end = nullptr;
    errno = 0;
    unsigned long long v = strtoull(s.c_str(), &end, 0);
    if (errno != 0 || end == nullptr || *end != '\0') return false;
    out = static_cast<uint64_t>(v);
    return true;
}

std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == sep) {
            out.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    out.push_back(cur);
    return out;
}

}  // namespace shpack
