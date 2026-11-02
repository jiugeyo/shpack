// SPDX-License-Identifier: MIT
//
// shpack 运行时 stub：读取自身尾部 Footer，解出 bash / busybox / 脚本并执行。
//
// 目标环境：Linux 与 Android（arm64-v8a 等），不依赖 ld.so、libc 动态库、
// /bin/sh、coreutils。Android 上 /tmp 不存在、部分挂载点 noexec、SELinux 可能
// 拒绝执行，因此落盘策略按「多候选目录 + 执行预检 + memfd 回退」三级处理。
#include <dirent.h>
#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/auxv.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <string>
#include <cstring>
#include <utility>
#include <vector>

#include "format.h"
#include "lz4.h"
#include "sha256.h"
#include "util.h"

using namespace shpack;

extern char** environ;

namespace {

// 退出码约定
enum {
    kExitUsage   = 2,
    kExitPackage = 100,
    kExitVerify  = 101,
    kExitExtract = 102,
    kExitFork    = 125,
    kExitExec    = 127,
};

// ---------------------------------------------------------------------------
// 数据结构
// ---------------------------------------------------------------------------
struct Blob {
    uint64_t off = 0;
    uint64_t stored = 0;
    uint64_t raw = 0;
    uint8_t  sha[32] = {};
    bool     lz4 = false;
};

struct Config {
    std::string name = "app";
    std::string mode = "auto";
    std::string pathmode = "busybox";
    std::string argv0;
    bool cleanup = true;
    std::vector<std::string> env;
    std::vector<std::string> bashopt;
};

struct Package {
    std::string self;
    Footer footer{};
    Config cfg;
    Blob bash, bb, script;
    int fd = -1;

    ~Package() {
        if (fd >= 0) ::close(fd);
    }
};

struct RunOptions {
    bool help = false;
    bool version = false;
    bool list = false;
    bool verbose = false;
    bool keep = false;
    bool no_preflight = false;
    bool extract = false;
    std::string extract_dir;
    std::string mode;
    std::string pathmode;
    std::vector<std::string> args;
};

struct Layout {
    bool memfd = false;
    bool fn_applets = false;
    std::string dir;         // 临时目录，纯 memfd 函数模式时为空
    std::string bin_dir;
    std::string bash_path;
    std::string bb_path;
    std::string script_path;
    std::vector<int> fds;    // 需要保持打开、可被继承的 memfd
};

// ---------------------------------------------------------------------------
// 小工具
// ---------------------------------------------------------------------------
[[noreturn]] void die(int code, const std::string& msg) {
    std::fprintf(stderr, "shpack: %s\n", msg.c_str());
    std::exit(code);
}

void info(const RunOptions& o, const std::string& msg) {
    if (o.verbose) std::fprintf(stderr, "shpack: %s\n", msg.c_str());
}

long long now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

std::string getenv_str(const char* key) {
    const char* v = ::getenv(key);
    return v ? std::string(v) : std::string();
}

void set_env(std::vector<std::string>& env, const std::string& key, const std::string& val) {
    const std::string prefix = key + "=";
    for (auto& e : env) {
        if (e.compare(0, prefix.size(), prefix) == 0) {
            e = prefix + val;
            return;
        }
    }
    env.push_back(prefix + val);
}

std::string get_env_from(const std::vector<std::string>& env, const std::string& key,
                         const std::string& fallback) {
    const std::string prefix = key + "=";
    for (const auto& e : env) {
        if (e.compare(0, prefix.size(), prefix) == 0) return e.substr(prefix.size());
    }
    return fallback;
}

std::vector<char*> to_cstr(std::vector<std::string>& v) {
    std::vector<char*> out;
    out.reserve(v.size() + 1);
    for (auto& s : v) out.push_back(const_cast<char*>(s.c_str()));
    out.push_back(nullptr);
    return out;
}

// 自路径：优先 /proc/self/exe（Android/Linux 均有），退化到 AT_EXECFN / argv[0]。
// SHPACK_SELF 可在特殊加载器（如 qemu-user 之外的自定义加载）下显式指定。
std::string self_path(const char* argv0) {
    const std::string override_path = getenv_str("SHPACK_SELF");
    if (!override_path.empty() && ::access(override_path.c_str(), R_OK) == 0) return override_path;

    char buf[PATH_MAX];
    ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n > 0) {
        buf[n] = '\0';
        return buf;
    }
#ifndef AT_EXECFN
#define AT_EXECFN 31
#endif
    unsigned long execfn = ::getauxval(AT_EXECFN);
    if (execfn != 0) {
        std::string p(reinterpret_cast<const char*>(execfn));
        if (!p.empty()) {
            if (p[0] == '/') return p;
            return absolute_path(p);
        }
    }
    if (argv0 && std::strchr(argv0, '/')) return absolute_path(argv0);
    std::string path = getenv_str("PATH");
    for (const auto& dir : split(path, ':')) {
        if (dir.empty()) continue;
        std::string cand = path_join(dir, argv0 ? argv0 : "");
        if (::access(cand.c_str(), X_OK) == 0) return absolute_path(cand);
    }
    return absolute_path(argv0 ? argv0 : "");
}

// ---------------------------------------------------------------------------
// 包加载与校验
// ---------------------------------------------------------------------------
bool read_exact(int fd, uint64_t off, void* buf, size_t len) {
    return pread_all(fd, off, buf, len);
}

bool check_blob(const Blob& b, const char* what, uint64_t total, std::string& err) {
    if (b.raw == 0 || b.stored == 0) {
        err = std::string(what) + " 负载为空";
        return false;
    }
    if (b.raw > kMaxPayload || b.stored > kMaxStored) {
        err = std::string(what) + " 负载大小超出上限";
        return false;
    }
    if (b.off < 64 || b.off + b.stored > total - kFooterSize) {
        err = std::string(what) + " 负载偏移越界";
        return false;
    }
    if (!b.lz4 && b.stored != b.raw) {
        err = std::string(what) + " 大小字段不一致";
        return false;
    }
    return true;
}

bool load_config(const std::vector<uint8_t>& raw, uint32_t count, Config& cfg, std::string& err) {
    size_t pos = 0;
    uint32_t n = 0;
    while (pos < raw.size()) {
        size_t end = pos;
        while (end < raw.size() && raw[end] != 0) ++end;
        if (end >= raw.size()) {
            err = "配置表缺少结尾 NUL";
            return false;
        }
        std::string entry(reinterpret_cast<const char*>(raw.data() + pos), end - pos);
        pos = end + 1;
        if (entry.empty()) continue;
        const size_t eq = entry.find('=');
        if (eq == std::string::npos) {
            err = "配置项格式错误: " + entry;
            return false;
        }
        const std::string key = entry.substr(0, eq);
        const std::string val = entry.substr(eq + 1);
        ++n;
        if (key == kCfgName) cfg.name = val;
        else if (key == kCfgMode) cfg.mode = val;
        else if (key == kCfgPathMode) cfg.pathmode = val;
        else if (key == kCfgCleanup) cfg.cleanup = (val != "0");
        else if (key == kCfgArgv0) cfg.argv0 = val;
        else if (key == kCfgEnv) cfg.env.push_back(val);
        else if (key == kCfgBashOpt) cfg.bashopt.push_back(val);
        // 未知 key 忽略，保持向前兼容
    }
    if (n != count) {
        err = "配置表条目数量不符";
        return false;
    }
    cfg.name = sanitize_component(cfg.name, "app");
    if (cfg.mode != "auto" && cfg.mode != "dir" && cfg.mode != "memfd") cfg.mode = "auto";
    if (cfg.pathmode != "system") cfg.pathmode = "busybox";
    return true;
}

Blob make_blob(uint64_t off, uint64_t stored, uint64_t raw, const uint8_t sha[32], bool lz4) {
    Blob b;
    b.off = off;
    b.stored = stored;
    b.raw = raw;
    std::memcpy(b.sha, sha, 32);
    b.lz4 = lz4;
    return b;
}

bool load_package(Package& pkg, const char* argv0, std::string& err) {
    const std::string self = self_path(argv0);
    if (self.empty()) {
        err = "无法定位自身可执行文件路径（/proc 不可用且 argv[0] 无效）";
        return false;
    }
    pkg.self = self;

    const int fd = ::open(self.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        err = errno_msg(errno, "打开自身 " + self);
        return false;
    }
    pkg.fd = fd;

    struct stat st;
    if (::fstat(fd, &st) != 0) {
        err = errno_msg(errno, "fstat 自身");
        return false;
    }
    if (st.st_size < static_cast<off_t>(kFooterSize + 64)) {
        err = "文件太小，不是 shpack 产物";
        return false;
    }
    const uint64_t total = static_cast<uint64_t>(st.st_size);

    Footer f;
    if (!read_exact(fd, total - kFooterSize, &f, sizeof(f))) {
        err = "读取 Footer 失败";
        return false;
    }
    if (std::memcmp(f.magic, kMagic, sizeof(kMagic)) != 0) {
        err = "找不到 shpack 魔数（不是 shpack 产物，或文件被截断）";
        return false;
    }
    if (f.version != kFormatVersion) {
        err = "包格式版本不支持: " + std::to_string(f.version) +
              "（本 stub 支持 " + std::to_string(kFormatVersion) + "）";
        return false;
    }
    if (f.header_size != kFooterSize) {
        err = "Footer 大小字段异常";
        return false;
    }
    if (f.total_size != total) {
        err = "文件大小与 Footer 记录不一致（可能在打包后被追加/截断）";
        return false;
    }

    uint8_t saved[16];
    std::memcpy(saved, f.footer_sha, sizeof(saved));
    std::memset(f.footer_sha, 0, sizeof(f.footer_sha));
    uint8_t digest[32];
    sha256(&f, sizeof(f), digest);
    if (std::memcmp(digest, saved, sizeof(saved)) != 0) {
        err = "Footer 校验失败（文件已损坏或被篡改）";
        return false;
    }
    std::memcpy(f.footer_sha, saved, sizeof(saved));

    if (f.cfg_size > kMaxConfig || f.cfg_off + f.cfg_size > total - kFooterSize ||
        f.cfg_off < 64) {
        err = "配置表偏移越界";
        return false;
    }
    std::vector<uint8_t> cfg_raw(static_cast<size_t>(f.cfg_size));
    if (!read_exact(fd, f.cfg_off, cfg_raw.data(), cfg_raw.size())) {
        err = "读取配置表失败";
        return false;
    }
    if (!load_config(cfg_raw, f.cfg_count, pkg.cfg, err)) return false;

    pkg.footer = f;
    pkg.bash = make_blob(f.bash_off, f.bash_stored, f.bash_raw, f.bash_sha,
                         (f.flags & kFlagBashLz4) != 0);
    pkg.bb = make_blob(f.bb_off, f.bb_stored, f.bb_raw, f.bb_sha,
                       (f.flags & kFlagBusyboxLz4) != 0);
    pkg.script = make_blob(f.script_off, f.script_stored, f.script_raw, f.script_sha,
                           (f.flags & kFlagScriptLz4) != 0);

    for (const auto& item : {std::make_pair(&pkg.bash, "bash"), std::make_pair(&pkg.bb, "busybox"),
                             std::make_pair(&pkg.script, "script")}) {
        if (!check_blob(*item.first, item.second, total, err)) return false;
    }
    return true;
}

bool load_payload(Package& pkg, const Blob& b, const char* what, std::vector<uint8_t>& out,
                  std::string& err) {
    std::vector<uint8_t> stored(static_cast<size_t>(b.stored));
    if (!read_exact(pkg.fd, b.off, stored.data(), stored.size())) {
        err = std::string("读取 ") + what + " 负载失败";
        return false;
    }
    out.clear();
    if (b.lz4) {
        out.resize(static_cast<size_t>(b.raw));
        size_t got = 0;
        if (!lz4_decompress(stored.data(), stored.size(), out.data(), out.size(), &got) ||
            got != out.size()) {
            err = std::string(what) + " 解压失败（数据损坏）";
            return false;
        }
    } else {
        out = std::move(stored);
    }
    uint8_t digest[32];
    sha256(out.data(), out.size(), digest);
    if (std::memcmp(digest, b.sha, sizeof(digest)) != 0) {
        err = std::string(what) + " SHA-256 校验失败（数据损坏或被篡改）";
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// 临时目录选择（Android 适配）
// ---------------------------------------------------------------------------
std::vector<std::string> temp_bases() {
    std::vector<std::string> bases;
    auto add = [&](const std::string& s) {
        if (s.empty()) return;
        for (const auto& b : bases) {
            if (b == s) return;
        }
        bases.push_back(s);
    };
    add(getenv_str("SHPACK_TMPDIR"));
    add(getenv_str("TMPDIR"));
    add("/data/local/tmp");  // Android adb / Termux 常见可执行目录
    add("/dev/shm");
    add("/tmp");
    const std::string home = getenv_str("HOME");
    if (!home.empty() && home != "/") add(home + "/.cache");
    add(".");
    return bases;
}

bool dir_usable(const std::string& base, bool require_exec) {
    struct stat st;
    if (::stat(base.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) return false;
    if (::access(base.c_str(), W_OK | X_OK) != 0) return false;
    if (require_exec) {
#ifdef ST_NOEXEC
        struct statvfs vfs;
        if (::statvfs(base.c_str(), &vfs) == 0 && (vfs.f_flag & ST_NOEXEC) != 0) return false;
#endif
    }
    return true;
}

bool make_temp_dir(const std::string& base, std::string& out, std::string& err) {
    std::vector<char> buf(base.size() + 32);
    std::snprintf(buf.data(), buf.size(), "%s/shpack.XXXXXX", base.c_str());
    if (::mkdtemp(buf.data()) == nullptr) {
        err = errno_msg(errno, "创建临时目录于 " + base);
        return false;
    }
    out = absolute_path(buf.data());  // 必须是绝对路径：applet 符号链接以此为基准
    return true;
}

bool write_new_file(const std::string& path, const void* data, size_t len, mode_t mode,
                    std::string& err) {
    int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, mode);
    if (fd < 0) {
        err = errno_msg(errno, "创建 " + path);
        return false;
    }
    bool ok = write_all(fd, data, len);
    if (!ok) err = errno_msg(errno, "写入 " + path);
    if (::fchmod(fd, mode) != 0 && ok) {
        err = errno_msg(errno, "chmod " + path);
        ok = false;
    }
    ::close(fd);
    return ok;
}

// 执行预检：真正 fork+exec 一次，验证该目录是否可执行（noexec / SELinux / 架构不符）。
bool preflight_exec(const std::string& exe, std::string& err) {
    pid_t pid = ::fork();
    if (pid < 0) {
        err = errno_msg(errno, "fork");
        return false;
    }
    if (pid == 0) {
        int nul = ::open("/dev/null", O_WRONLY);
        if (nul >= 0) {
            ::dup2(nul, STDOUT_FILENO);
            ::dup2(nul, STDERR_FILENO);
        }
        ::signal(SIGINT, SIG_DFL);
        ::signal(SIGQUIT, SIG_DFL);
        char* av[] = {const_cast<char*>(exe.c_str()), const_cast<char*>("--version"), nullptr};
        ::execv(exe.c_str(), av);
        _exit(kExitExec);
    }
    int status = 0;
    while (::waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) {
            err = errno_msg(errno, "waitpid");
            return false;
        }
    }
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) return true;
    if (WIFEXITED(status)) {
        err = "预检执行失败（退出码 " + std::to_string(WEXITSTATUS(status)) +
              "，可能是不允许执行该路径）";
    } else if (WIFSIGNALED(status)) {
        err = "预检执行被信号 " + std::to_string(WTERMSIG(status)) + " 终止";
    } else {
        err = "预检执行失败";
    }
    return false;
}

// 捕获子进程 stdout（用于 busybox --list）。
bool capture_stdout(const std::string& exe, const std::vector<std::string>& args, std::string& out) {
    int pipefd[2];
    if (::pipe(pipefd) != 0) return false;
    pid_t pid = ::fork();
    if (pid < 0) {
        ::close(pipefd[0]);
        ::close(pipefd[1]);
        return false;
    }
    if (pid == 0) {
        ::close(pipefd[0]);
        ::dup2(pipefd[1], STDOUT_FILENO);
        ::close(pipefd[1]);
        int nul = ::open("/dev/null", O_WRONLY);
        if (nul >= 0) ::dup2(nul, STDERR_FILENO);
        std::vector<std::string> av_s;
        av_s.push_back(exe);
        for (const auto& a : args) av_s.push_back(a);
        std::vector<char*> av = to_cstr(av_s);
        ::execv(exe.c_str(), av.data());
        _exit(kExitExec);
    }
    ::close(pipefd[1]);
    out.clear();
    char buf[4096];
    for (;;) {
        ssize_t n = ::read(pipefd[0], buf, sizeof(buf));
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (n == 0) break;
        if (out.size() + static_cast<size_t>(n) > 1024 * 1024) break;  // 防御
        out.append(buf, static_cast<size_t>(n));
    }
    ::close(pipefd[0]);
    int status = 0;
    while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

bool install_applets(const std::string& bb_abs, const std::string& bin_dir, bool verbose,
                     std::string& err) {
    std::string listing;
    if (!capture_stdout(bb_abs, {"--list"}, listing)) {
        err = "busybox --list 执行失败";
        return false;
    }
    size_t created = 0, skipped = 0;
    size_t pos = 0;
    while (pos < listing.size()) {
        size_t end = listing.find('\n', pos);
        if (end == std::string::npos) end = listing.size();
        std::string name = listing.substr(pos, end - pos);
        pos = end + 1;
        if (!name.empty() && name.back() == '\r') name.pop_back();
        if (name.empty() || name == "." || name == ".." || name.find('/') != std::string::npos)
            continue;
        const std::string link = path_join(bin_dir, name);
        if (::symlink(bb_abs.c_str(), link.c_str()) == 0) {
            ++created;
        } else {
            ++skipped;
        }
    }
    if (verbose) {
        std::fprintf(stderr, "shpack: 创建 %zu 个 busybox applet 链接（跳过 %zu）\n", created,
                     skipped);
    }
    return created > 0;
}

// ---------------------------------------------------------------------------
// 三种落盘策略
// ---------------------------------------------------------------------------
bool prepare_dir(const Package& pkg, const std::vector<uint8_t>& bash,
                 const std::vector<uint8_t>& bb, const std::vector<uint8_t>& script,
                 const RunOptions& opt, Layout& l, std::string& err) {
    std::string last_err;
    for (const auto& base : temp_bases()) {
        if (!dir_usable(base, true)) {
            last_err = "目录不可写或位于 noexec 挂载点: " + base;
            continue;
        }
        std::string dir;
        const long long t0 = now_ms();
        if (!make_temp_dir(base, dir, last_err)) continue;
        const long long t1 = now_ms();
        info(opt, "临时目录 " + dir);

        const std::string bin_dir = path_join(dir, "bin");
        const std::string bash_path = path_join(dir, "bash");
        const std::string bb_path = path_join(bin_dir, "busybox");
        const std::string script_path = path_join(dir, pkg.cfg.name + ".sh");

        bool ok = true;
        if (::mkdir(bin_dir.c_str(), 0700) != 0) {
            last_err = errno_msg(errno, "创建 " + bin_dir);
            ok = false;
        }
        if (ok && !write_new_file(bash_path, bash.data(), bash.size(), 0700, last_err)) ok = false;
        if (ok && !write_new_file(bb_path, bb.data(), bb.size(), 0700, last_err)) ok = false;
        if (ok && !write_new_file(script_path, script.data(), script.size(), 0600, last_err))
            ok = false;
        const long long t2 = now_ms();
        if (ok && !opt.no_preflight && !preflight_exec(bash_path, last_err)) ok = false;
        const long long t3 = now_ms();

        if (!ok) {
            remove_tree(dir);
            continue;
        }
        std::string applet_err;
        if (!install_applets(bb_path, bin_dir, opt.verbose, applet_err)) {
            info(opt, "警告: " + applet_err + "（仍可显式调用 busybox）");
        }
        const long long t4 = now_ms();
        info(opt, "耗时: mkdtemp " + std::to_string(t1 - t0) + " ms, 写入 " +
                      std::to_string(t2 - t1) + " ms, 预检 " + std::to_string(t3 - t2) +
                      " ms, applet " + std::to_string(t4 - t3) + " ms");

        l.dir = dir;
        l.bin_dir = bin_dir;
        l.bash_path = bash_path;
        l.bb_path = bb_path;
        l.script_path = script_path;
        return true;
    }
    err = "没有可用的临时目录。最后错误: " + last_err;
    return false;
}

#ifndef MFD_CLOEXEC
#define MFD_CLOEXEC 0x0001U
#endif
#ifndef MFD_EXEC
#define MFD_EXEC 0x0010U
#endif

int memfd_create_compat(const char* name) {
#ifdef SYS_memfd_create
    // 新内核（>= 6.3）在 memfd_noexec 策略下要求携带 MFD_EXEC；
    // 老内核不认识该 flag 会返回 EINVAL，再去掉重试。
    long fd = ::syscall(SYS_memfd_create, name, MFD_CLOEXEC | MFD_EXEC);
    if (fd < 0 && errno == EINVAL) {
        fd = ::syscall(SYS_memfd_create, name, MFD_CLOEXEC);
    }
    return static_cast<int>(fd);
#else
    (void)name;
    errno = ENOSYS;
    return -1;
#endif
}

bool write_anon_file(const char* tag, const void* data, size_t len, mode_t mode, int& fd,
                     std::string& path, std::string& how, std::string& err) {
    // 1) memfd_create：纯内存匿名文件，Android 8+/Linux 3.17+ 可用
    fd = memfd_create_compat(tag);
    how = "memfd";
    if (fd < 0) {
        const int memfd_err = errno;
        // 2) 退化到 /dev/shm 上的匿名文件：写入后立即 unlink，不留目录项，
        //    对 WSL1 等未实现 memfd_create 的内核同样能做到“不落盘”。
        how = "shm";
        if (::access("/dev/shm", W_OK | X_OK) == 0) {
            for (int attempt = 0; attempt < 8 && fd < 0; ++attempt) {
                char p[160];
                std::snprintf(p, sizeof(p), "/dev/shm/shpack.%ld.%08x", static_cast<long>(::getpid()),
                              static_cast<unsigned>(::rand()));
                fd = ::open(p, O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, mode);
                if (fd >= 0) {
                    ::unlink(p);  // 立即删除名字，只保留 fd
                    break;
                }
                if (errno != EEXIST) break;
            }
        }
        if (fd < 0) {
            err = errno_msg(memfd_err, "memfd_create") +
                  "；/dev/shm 匿名文件也不可用（errno=" + std::to_string(errno) + "）";
            return false;
        }
    }

    if (!write_all(fd, data, len) || ::fchmod(fd, mode) != 0) {
        err = errno_msg(errno, std::string("写入匿名文件 ") + tag);
        ::close(fd);
        fd = -1;
        return false;
    }
    path = "/proc/self/fd/" + std::to_string(fd);
    return true;
}

void clear_cloexec(int fd) {
    int flags = ::fcntl(fd, F_GETFD);
    if (flags >= 0) ::fcntl(fd, F_SETFD, flags & ~FD_CLOEXEC);
}

// 纯内存模式：busybox 没有可执行路径目录，用 shell 函数提供 applet。
// 函数只在当前 bash 内有效；同时导出 command_not_found_handle，
// 让子 bash（bash -c）在系统 PATH 找不到命令时也能回落到 busybox。
std::string applet_functions_preamble() {
    static const char* kPreamble = R"SHPACK(# shpack: 纯内存模式，busybox applet 以 shell 函数提供
if [ -n "${SHPACK_BUSYBOX_FD:-}" ] && [ -x "$SHPACK_BUSYBOX_FD" ]; then
  __shpack_list=$(exec -a busybox "$SHPACK_BUSYBOX_FD" --list 2>/dev/null)
  SHPACK_APPLETS=" ${__shpack_list//$'\n'/ } "
  export SHPACK_APPLETS
  busybox() { ( exec -a busybox "$SHPACK_BUSYBOX_FD" "$@" ); }
  export -f busybox 2>/dev/null || true
  for __shpack_a in $__shpack_list; do
    case $__shpack_a in *[!A-Za-z0-9_]*|[0-9]*|'') continue;; esac
    case $__shpack_a in echo|printf|test|pwd|kill|true|false|cd|exec|read|type|ulimit|umask|wait|jobs|fg|bg|export|unset|shift|source|eval|trap|set|return|break|continue|local|declare|typeset|alias|unalias|hash|help|history|let|mapfile|readarray|shopt|time|times|command|builtin|enable|getopts|readonly|suspend|logout|disown|bind|complete|compgen|caller|dirs|pushd|popd|fc) continue;; esac
    eval "$__shpack_a() { ( exec -a busybox \"\$SHPACK_BUSYBOX_FD\" \"$__shpack_a\" \"\$@\" ); }"
  done
  unset __shpack_a __shpack_list
  command_not_found_handle() {
    if [ "$1" = busybox ]; then shift; ( exec -a busybox "$SHPACK_BUSYBOX_FD" "$@" ); return $?; fi
    case " $SHPACK_APPLETS " in
      *" $1 "*) ( exec -a busybox "$SHPACK_BUSYBOX_FD" "$@" );;
      *) printf '%s: command not found\n' "$1" >&2; return 127;;
    esac
  }
  export -f command_not_found_handle 2>/dev/null || true
fi
)SHPACK";
    return kPreamble;
}

bool prepare_memfd(const std::vector<uint8_t>& bash, const std::vector<uint8_t>& bb,
                   const std::vector<uint8_t>& script, const RunOptions& opt, Layout& l,
                   std::string& err) {
    if (::access("/proc/self/fd", X_OK) != 0) {
        err = "纯内存模式需要 /proc/self/fd（当前系统不可用）";
        return false;
    }
    std::srand(static_cast<unsigned>(::getpid() ^ static_cast<unsigned>(now_ms())));
    const long long t0 = now_ms();

    int fd_bash = -1, fd_bb = -1, fd_script = -1;
    std::string bash_path, bb_path, script_path, how, werr;
    if (!write_anon_file("shpack-bash", bash.data(), bash.size(), 0700, fd_bash, bash_path, how,
                         err))
        return false;
    if (!write_anon_file("shpack-busybox", bb.data(), bb.size(), 0700, fd_bb, bb_path, how, err)) {
        ::close(fd_bash);
        return false;
    }
    // 三个 fd 都不能带 CLOEXEC：bash 要能被 /proc/self/fd 解析执行，
    // busybox/脚本要活到 bash 及其子进程里。
    clear_cloexec(fd_bash);
    clear_cloexec(fd_bb);

    std::vector<uint8_t> script_data = script;
    const std::string pre = applet_functions_preamble();
    script_data.insert(script_data.begin(), pre.begin(), pre.end());
    if (!write_anon_file("shpack-script", script_data.data(), script_data.size(), 0600, fd_script,
                         script_path, how, err)) {
        ::close(fd_bash);
        ::close(fd_bb);
        return false;
    }
    clear_cloexec(fd_script);

    if (::access(bash_path.c_str(), X_OK) != 0) {
        err = errno_msg(errno, "匿名 bash 不可执行（noexec 或 SELinux 限制）");
        ::close(fd_bash);
        ::close(fd_bb);
        ::close(fd_script);
        return false;
    }

    const long long t1 = now_ms();
    if (!opt.no_preflight) {
        std::string perr;
        if (!preflight_exec(bash_path, perr)) {
            ::close(fd_bash);
            ::close(fd_bb);
            ::close(fd_script);
            err = "无法从内存执行 bash（内核或 SELinux 限制）: " + perr;
            return false;
        }
    }
    const long long t2 = now_ms();
    info(opt, "匿名文件方式: " + how + "；准备 " + std::to_string(t1 - t0) + " ms，预检 " +
                  std::to_string(t2 - t1) + " ms");

    l.memfd = true;
    l.fn_applets = true;
    l.dir.clear();
    l.bin_dir.clear();
    l.bash_path = bash_path;
    l.bb_path = bb_path;
    l.script_path = script_path;
    l.fds = {fd_bash, fd_bb, fd_script};
    return true;
}

// ---------------------------------------------------------------------------
// 运行
// ---------------------------------------------------------------------------
std::vector<std::string> build_env(const Package& pkg, const Layout& l, const char* mode_name) {
    std::vector<std::string> env;
    for (char** e = environ; e && *e; ++e) env.push_back(*e);

    for (const auto& kv : pkg.cfg.env) {
        const size_t eq = kv.find('=');
        if (eq == std::string::npos) continue;
        set_env(env, kv.substr(0, eq), kv.substr(eq + 1));
    }

    const std::string sys_path =
        "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin:/system/bin:/system/xbin";
    const std::string orig_path = get_env_from(env, "PATH", sys_path);
    std::string new_path = orig_path;
    if (!l.bin_dir.empty()) {
        new_path = (pkg.cfg.pathmode == "system") ? (orig_path + ":" + l.bin_dir)
                                                  : (l.bin_dir + ":" + orig_path);
    }
    set_env(env, "PATH", new_path);

    set_env(env, kEnvSelf, pkg.self);
    set_env(env, kEnvDir, l.dir);
    set_env(env, kEnvBash, l.bash_path);
    // 纯内存模式下 SHPACK_BUSYBOX 是可执行的命令名（shell 函数/包装），
    // 原始 fd 路径放在 SHPACK_BUSYBOX_FD 里。
    set_env(env, kEnvBusybox, l.memfd ? std::string("busybox") : l.bb_path);
    set_env(env, kEnvBusyboxFd, l.bb_path);
    set_env(env, kEnvScript, l.script_path);
    set_env(env, kEnvMode, mode_name);
    set_env(env, kEnvName, pkg.cfg.name);
    set_env(env, kEnvVersion, kToolVersion);
    set_env(env, kEnvOrigPath, orig_path);
    if (!pkg.cfg.argv0.empty()) set_env(env, kEnvArgv0, pkg.cfg.argv0);
    return env;
}

volatile sig_atomic_t g_pending_sig = 0;

void on_forward_signal(int sig) { g_pending_sig = sig; }

[[noreturn]] void exec_bash(const Package& pkg, const Layout& l, std::vector<std::string>& env,
                            const std::vector<std::string>& args) {
    std::vector<std::string> av;
    av.push_back(l.bash_path);
    for (const auto& o : pkg.cfg.bashopt) av.push_back(o);
    av.push_back(l.script_path);
    for (const auto& a : args) av.push_back(a);

    std::vector<char*> avp = to_cstr(av);
    std::vector<char*> evp = to_cstr(env);
    ::execve(l.bash_path.c_str(), avp.data(), evp.data());
    std::fprintf(stderr, "shpack: 无法执行内置 bash (%s): %s\n", l.bash_path.c_str(),
                 std::strerror(errno));
    _exit(kExitExec);
}

int run_and_wait(const Package& pkg, const Layout& l, std::vector<std::string>& env,
                 const std::vector<std::string>& args, const RunOptions& opt) {
    const bool need_cleanup = !opt.keep && pkg.cfg.cleanup && !l.dir.empty();
    if (!need_cleanup) {
        if (opt.keep && !l.dir.empty())
            std::fprintf(stderr, "shpack: 已保留临时目录 %s\n", l.dir.c_str());
        exec_bash(pkg, l, env, args);
    }

    ::fflush(nullptr);
    pid_t pid = ::fork();
    if (pid < 0) die(kExitFork, errno_msg(errno, "fork"));

    if (pid == 0) {
        ::signal(SIGINT, SIG_DFL);
        ::signal(SIGQUIT, SIG_DFL);
        ::signal(SIGTERM, SIG_DFL);
        ::signal(SIGHUP, SIG_DFL);
        exec_bash(pkg, l, env, args);
    }

    for (int fd : l.fds) {
        if (fd > 2) ::close(fd);
    }

    // 终端信号由内核发给整个进程组，父进程忽略即可；其他信号转发给子进程。
    ::signal(SIGINT, SIG_IGN);
    ::signal(SIGQUIT, SIG_IGN);
    struct sigaction sa;
    std::memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_forward_signal;
    ::sigemptyset(&sa.sa_mask);
    ::sigaction(SIGTERM, &sa, nullptr);
    ::sigaction(SIGHUP, &sa, nullptr);

    int status = 0;
    for (;;) {
        const pid_t r = ::waitpid(pid, &status, 0);
        if (r == pid) break;
        if (r < 0 && errno == EINTR) {
            if (g_pending_sig != 0) {
                ::kill(pid, g_pending_sig);
                g_pending_sig = 0;
            }
            continue;
        }
        break;
    }

    if (!l.dir.empty()) remove_tree(l.dir);

    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) {
        const int sig = WTERMSIG(status);
        ::signal(sig, SIG_DFL);
        ::raise(sig);
        return 128 + sig;
    }
    return 1;
}

// ---------------------------------------------------------------------------
// 子命令
// ---------------------------------------------------------------------------
void print_help(const Package& pkg) {
    std::printf(
        "%s —— 由 shpack 打包的自包含可执行文件\n"
        "\n"
        "用法: %s [shpack 选项] [--] [脚本参数...]\n"
        "\n"
        "shpack 选项（遇到第一个非选项参数或 -- 即停止解析，其余全部交给脚本）:\n"
        "  -h, --help          显示本帮助\n"
        "  -V, --version       显示版本\n"
        "  --list              列出包内组件信息\n"
        "  --extract DIR       把 bash/busybox/脚本释放到 DIR 后退出\n"
        "  --keep              保留本次运行的临时目录（仅 dir 模式）\n"
        "  --mode MODE         auto | memfd | dir（默认 auto：优先纯内存，不落盘）\n"
        "  --path MODE         busybox | system（仅 dir 模式影响 PATH）\n"
        "  -v, --verbose       输出调试信息\n"
        "  --no-preflight      跳过执行预检（不推荐）\n"
        "\n"
        "给脚本传参: %s -- --help   # 脚本自己处理 --help\n"
        "环境变量: SHPACK_SELF SHPACK_DIR SHPACK_BASH SHPACK_BUSYBOX\n"
        "          SHPACK_BUSYBOX_FD SHPACK_SCRIPT SHPACK_MODE\n",
        pkg.cfg.name.c_str(), pkg.cfg.name.c_str(), pkg.cfg.name.c_str());
}

void print_list(const Package& pkg) {
    const auto row = [](const char* name, const Blob& b, bool lz4) {
        std::printf("  %-8s %8s -> %8s  %s\n", name, human_size(b.raw).c_str(),
                    human_size(b.stored).c_str(), lz4 ? "LZ4" : "store");
    };
    std::printf("名称    : %s\n", pkg.cfg.name.c_str());
    std::printf("文件    : %s (%s)\n", pkg.self.c_str(),
                human_size(pkg.footer.total_size).c_str());
    std::printf("格式    : shpack v%u, build %s\n", pkg.footer.version,
                to_hex(pkg.footer.build_id, 8).c_str());
    row("bash", pkg.bash, pkg.bash.lz4);
    row("busybox", pkg.bb, pkg.bb.lz4);
    row("script", pkg.script, pkg.script.lz4);
    std::printf("配置    : mode=%s path=%s cleanup=%d\n", pkg.cfg.mode.c_str(),
                pkg.cfg.pathmode.c_str(), pkg.cfg.cleanup ? 1 : 0);
    for (const auto& e : pkg.cfg.env) std::printf("          env %s\n", e.c_str());
    for (const auto& o : pkg.cfg.bashopt) std::printf("          bashopt %s\n", o.c_str());
}

void mkdir_p(const std::string& path) {
    if (path.empty() || path == "/") return;
    std::string cur;
    for (const auto& part : split(path, '/')) {
        if (part.empty()) {
            cur = "/";
            continue;
        }
        cur = cur.empty() ? part : path_join(cur, part);
        if (::mkdir(cur.c_str(), 0755) != 0 && errno != EEXIST) {
            die(kExitExtract, errno_msg(errno, "创建目录 " + cur));
        }
    }
}

int do_extract(Package& pkg, const RunOptions& opt) {
    if (opt.extract_dir.empty()) die(kExitUsage, "--extract 需要非空目录");
    std::vector<uint8_t> bash, bb, script;
    std::string err;
    if (!load_payload(pkg, pkg.bash, "bash", bash, err) ||
        !load_payload(pkg, pkg.bb, "busybox", bb, err) ||
        !load_payload(pkg, pkg.script, "script", script, err)) {
        die(kExitVerify, err);
    }
    const std::string dir = opt.extract_dir;
    mkdir_p(dir);
    const std::string bin_dir = path_join(dir, "bin");
    if (::mkdir(bin_dir.c_str(), 0755) != 0 && errno != EEXIST)
        die(kExitExtract, errno_msg(errno, "创建 " + bin_dir));

    const std::string bash_path = path_join(dir, "bash");
    const std::string bb_path = path_join(bin_dir, "busybox");
    const std::string script_path = path_join(dir, pkg.cfg.name + ".sh");
    const auto put = [&](const std::string& p, const std::vector<uint8_t>& d, mode_t m) {
        int fd = ::open(p.c_str(), O_WRONLY | O_CREAT | O_TRUNC, m);
        if (fd < 0) die(kExitExtract, errno_msg(errno, "创建 " + p));
        if (!write_all(fd, d.data(), d.size())) die(kExitExtract, errno_msg(errno, "写入 " + p));
        ::fchmod(fd, m);
        ::close(fd);
    };
    put(bash_path, bash, 0755);
    put(bb_path, bb, 0755);
    put(script_path, script, 0644);

    std::string applet_err;
    install_applets(bb_path, bin_dir, opt.verbose, applet_err);
    std::printf("已释放到 %s\n", dir.c_str());
    std::printf("  bash    %s\n", bash_path.c_str());
    std::printf("  busybox %s\n", bb_path.c_str());
    std::printf("  脚本    %s\n", script_path.c_str());
    return 0;
}

// ---------------------------------------------------------------------------
// 参数解析
// ---------------------------------------------------------------------------
RunOptions parse_run_options(int argc, char** argv) {
    RunOptions o;
    int i = 1;
    while (i < argc) {
        const std::string a = argv[i];
        if (a == "--") {
            ++i;
            break;
        }
        if (a.empty() || a[0] != '-' || a == "-") break;
        if (a == "-h" || a == "--help") {
            o.help = true;
            ++i;
        } else if (a == "-V" || a == "--version") {
            o.version = true;
            ++i;
        } else if (a == "--list") {
            o.list = true;
            ++i;
        } else if (a == "--keep") {
            o.keep = true;
            ++i;
        } else if (a == "-v" || a == "--verbose") {
            o.verbose = true;
            ++i;
        } else if (a == "--no-preflight") {
            o.no_preflight = true;
            ++i;
        } else if (a == "--extract") {
            if (i + 1 >= argc) die(kExitUsage, "--extract 需要目录参数");
            o.extract = true;
            o.extract_dir = argv[++i];
            ++i;
        } else if (a.rfind("--extract=", 0) == 0) {
            o.extract = true;
            o.extract_dir = a.substr(10);
            ++i;
        } else if (a == "--mode") {
            if (i + 1 >= argc) die(kExitUsage, "--mode 需要取值");
            o.mode = argv[++i];
            ++i;
        } else if (a.rfind("--mode=", 0) == 0) {
            o.mode = a.substr(7);
            ++i;
        } else if (a == "--path") {
            if (i + 1 >= argc) die(kExitUsage, "--path 需要取值");
            o.pathmode = argv[++i];
            ++i;
        } else if (a.rfind("--path=", 0) == 0) {
            o.pathmode = a.substr(7);
            ++i;
        } else {
            break;  // 未知选项视为脚本参数
        }
    }
    for (; i < argc; ++i) o.args.push_back(argv[i]);
    if (!o.mode.empty() && o.mode != "auto" && o.mode != "dir" && o.mode != "memfd")
        die(kExitUsage, "--mode 只支持 auto|dir|memfd");
    if (!o.pathmode.empty() && o.pathmode != "busybox" && o.pathmode != "system")
        die(kExitUsage, "--path 只支持 busybox|system");
    return o;
}

const char* layout_mode_name(const Layout& l) {
    if (!l.memfd) return "dir";
    return "memfd";
}

}  // namespace

int main(int argc, char** argv) {
    ::umask(077);
    Package pkg;
    std::string err;
    const long long t_start = now_ms();
    if (!load_package(pkg, argv[0], err)) die(kExitPackage, err);
    const long long t_loaded = now_ms();

    RunOptions opt = parse_run_options(argc, argv);
    if (!opt.mode.empty()) pkg.cfg.mode = opt.mode;
    if (!opt.pathmode.empty()) pkg.cfg.pathmode = opt.pathmode;

    if (opt.help) {
        print_help(pkg);
        return 0;
    }
    if (opt.version) {
        std::printf("%s (shpack %s, format %u, build %s)\n", pkg.cfg.name.c_str(), kToolVersion,
                    kFormatVersion, to_hex(pkg.footer.build_id, 8).c_str());
        return 0;
    }
    if (opt.list) {
        print_list(pkg);
        return 0;
    }
    if (opt.extract) return do_extract(pkg, opt);
    info(opt, "读取 Footer/配置: " + std::to_string(t_loaded - t_start) + " ms");

    const long long t_payload0 = now_ms();
    std::vector<uint8_t> bash, bb, script;
    if (!load_payload(pkg, pkg.bash, "bash", bash, err) ||
        !load_payload(pkg, pkg.bb, "busybox", bb, err) ||
        !load_payload(pkg, pkg.script, "script", script, err)) {
        die(kExitVerify, err);
    }
    info(opt, "解压+校验: " + std::to_string(now_ms() - t_payload0) + " ms（bash " +
                  human_size(bash.size()) + ", busybox " + human_size(bb.size()) + ", script " +
                  human_size(script.size()) + "）");

    Layout layout;
    std::string dir_err, memfd_err;
    const std::string mode = pkg.cfg.mode;
    // auto：优先纯内存（不落盘），失败再回退到临时目录。
    if (mode == "auto" || mode == "memfd") {
        if (!prepare_memfd(bash, bb, script, opt, layout, memfd_err)) {
            if (mode == "memfd") die(kExitExtract, memfd_err);
            info(opt, "纯内存模式失败: " + memfd_err);
        }
    }
    if (layout.bash_path.empty() && (mode == "auto" || mode == "dir")) {
        if (!prepare_dir(pkg, bash, bb, script, opt, layout, dir_err)) {
            if (mode == "dir") die(kExitExtract, dir_err);
            info(opt, "目录模式失败: " + dir_err);
            die(kExitExtract, "无法准备运行时组件。\n  纯内存模式: " + memfd_err +
                                  "\n  目录模式: " + dir_err +
                                  "\n  可设置 SHPACK_TMPDIR 指向可执行目录后重试。");
        }
    }
    info(opt, std::string("运行策略: ") + layout_mode_name(layout));
    info(opt, "准备完成: " + std::to_string(now_ms() - t_start) + " ms");

    std::vector<std::string> env = build_env(pkg, layout, layout_mode_name(layout));
    return run_and_wait(pkg, layout, env, opt.args, opt);
}
