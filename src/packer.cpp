// SPDX-License-Identifier: MIT
//
// shpack：把「静态 bash + 静态 busybox + 任意 shell 脚本」打包成单个静态 ELF。
//
// 产物 = 静态 stub + 三个负载 + 配置表 + 256 字节 Footer。
// 本程序只在构建机运行，不进入产物。
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#include "format.h"
#include "lz4.h"
#include "sha256.h"
#include "util.h"

using namespace shpack;

namespace {

struct Options {
    std::string stub;
    std::string bash;
    std::string busybox;
    std::string script;
    std::string output;
    std::string name;
    std::string compress = "auto";   // auto | none | lz4
    std::string pathmode = "busybox";  // busybox | system
    std::string mode = "auto";         // auto | dir | memfd
    std::vector<std::string> envs;
    std::vector<std::string> bashopts;
    bool cleanup = true;
    bool allow_dynamic = false;
    bool verbose = false;
};

struct Blob {
    std::string path;
    std::vector<uint8_t> raw;
    std::vector<uint8_t> stored;
    bool lz4 = false;
    uint16_t machine = 0;
    std::string elf_desc;
};

[[noreturn]] void die(const std::string& msg) {
    std::fprintf(stderr, "shpack: 错误: %s\n", msg.c_str());
    std::exit(2);
}

void ensure_output_not_input(const std::string& out,
                             const std::vector<std::string>& inputs) {
    struct stat so;
    if (::stat(out.c_str(), &so) != 0) return;  // 输出不存在，不会覆盖输入
    for (const auto& in : inputs) {
        struct stat si;
        if (::stat(in.c_str(), &si) == 0 && si.st_dev == so.st_dev && si.st_ino == so.st_ino) {
            die("输出路径与输入文件相同（会覆盖输入）: " + out);
        }
    }
}

void usage(std::FILE* out) {
    std::fprintf(out,
        "shpack %s - 将 shell 脚本打包为内嵌 bash/busybox 的静态 ELF\n"
        "\n"
        "用法:\n"
        "  shpack --stub STUB --bash BASH --busybox BUSYBOX --script SCRIPT -o OUT [选项]\n"
        "\n"
        "必需参数:\n"
        "  --stub PATH       静态 launcher（make 生成 build/shpack-stub）\n"
        "  --bash PATH       静态链接的 bash\n"
        "  --busybox PATH    静态链接的 busybox\n"
        "  --script PATH     shell 脚本\n"
        "  -o, --output PATH 输出 ELF 路径\n"
        "\n"
        "可选参数:\n"
        "  --name NAME       包名，同时作为运行时脚本文件名（默认取脚本文件名）\n"
        "  --env K=V         追加环境变量，可重复\n"
        "  --bash-opt OPT    追加 bash 选项（放在脚本路径之前），可重复\n"
        "  --path-mode M     busybox | system（默认 busybox，busybox 目录排在 PATH 前面）\n"
        "  --mode M          auto | dir | memfd（运行时默认策略，默认 auto）\n"
        "  --no-cleanup      运行结束后保留临时目录\n"
        "  --compress M      auto | none | lz4（默认 auto：仅有收益时压缩）\n"
        "  --allow-dynamic   跳过静态链接检查（仅用于调试，产物可能无法“到处运行”）\n"
        "  -v, --verbose     输出更多信息\n"
        "  -h, --help        显示帮助\n"
        "  --version         显示版本\n"
        "\n"
        "示例:\n"
        "  shpack --stub build/shpack-stub --bash /opt/static/bash \\\n"
        "         --busybox /opt/static/busybox --script deploy.sh -o deploy\n"
        "  ./deploy --help          # 查看运行时选项\n"
        "  ./deploy -- --dry-run    # '--' 之后全部传给脚本\n",
        kToolVersion);
}

std::string need_value(int argc, char** argv, int& i, const std::string& opt) {
    std::string a = argv[i];
    const std::string prefix = opt + "=";
    if (a.rfind(prefix, 0) == 0) return a.substr(prefix.size());
    if (i + 1 >= argc) die("参数 " + opt + " 缺少取值");
    return argv[++i];
}

Options parse_args(int argc, char** argv) {
    Options opt;
    bool want_help = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "-h" || a == "--help") {
            want_help = true;
        } else if (a == "--version") {
            std::printf("shpack %s\n", kToolVersion);
            std::exit(0);
        } else if (a == "--stub" || a.rfind("--stub=", 0) == 0) {
            opt.stub = need_value(argc, argv, i, "--stub");
        } else if (a == "--bash" || a.rfind("--bash=", 0) == 0) {
            opt.bash = need_value(argc, argv, i, "--bash");
        } else if (a == "--busybox" || a.rfind("--busybox=", 0) == 0) {
            opt.busybox = need_value(argc, argv, i, "--busybox");
        } else if (a == "--script" || a.rfind("--script=", 0) == 0) {
            opt.script = need_value(argc, argv, i, "--script");
        } else if (a == "-o" || a == "--output" || a.rfind("--output=", 0) == 0) {
            opt.output = need_value(argc, argv, i, "--output");
        } else if (a == "--name" || a.rfind("--name=", 0) == 0) {
            opt.name = need_value(argc, argv, i, "--name");
        } else if (a == "--env" || a.rfind("--env=", 0) == 0) {
            opt.envs.push_back(need_value(argc, argv, i, "--env"));
        } else if (a == "--bash-opt" || a.rfind("--bash-opt=", 0) == 0) {
            opt.bashopts.push_back(need_value(argc, argv, i, "--bash-opt"));
        } else if (a == "--path-mode" || a.rfind("--path-mode=", 0) == 0) {
            opt.pathmode = need_value(argc, argv, i, "--path-mode");
        } else if (a == "--mode" || a.rfind("--mode=", 0) == 0) {
            opt.mode = need_value(argc, argv, i, "--mode");
        } else if (a == "--compress" || a.rfind("--compress=", 0) == 0) {
            opt.compress = need_value(argc, argv, i, "--compress");
        } else if (a == "--no-cleanup") {
            opt.cleanup = false;
        } else if (a == "--allow-dynamic") {
            opt.allow_dynamic = true;
        } else if (a == "-v" || a == "--verbose") {
            opt.verbose = true;
        } else {
            die("未知参数: " + a + "（--help 查看用法）");
        }
    }
    if (want_help) {
        usage(stdout);
        std::exit(0);
    }
    if (opt.stub.empty() || opt.bash.empty() || opt.busybox.empty() || opt.script.empty() ||
        opt.output.empty()) {
        usage(stderr);
        die("缺少必需参数");
    }
    if (opt.compress != "auto" && opt.compress != "none" && opt.compress != "lz4")
        die("--compress 只支持 auto|none|lz4");
    if (opt.pathmode != "busybox" && opt.pathmode != "system")
        die("--path-mode 只支持 busybox|system");
    if (opt.mode != "auto" && opt.mode != "dir" && opt.mode != "memfd")
        die("--mode 只支持 auto|dir|memfd");
    if (opt.name.empty()) {
        std::string n = base_name(opt.script);
        if (n.size() > 3 && n.compare(n.size() - 3, 3, ".sh") == 0) n.resize(n.size() - 3);
        opt.name = n;
    }
    opt.name = sanitize_component(opt.name, "app");
    return opt;
}

Blob load_blob(const std::string& path, const char* what, bool require_static,
               bool allow_dynamic) {
    Blob b;
    b.path = path;
    std::string err;
    if (!read_file(path, b.raw, kMaxPayload, err)) die(std::string(what) + ": " + err);
    if (b.raw.empty()) die(std::string(what) + " 是空文件: " + path);

    ElfInfo info;
    if (!elf_inspect(b.raw, info, err)) die(std::string(what) + " (" + path + "): " + err);
    if (!info.has_interp) {
        b.elf_desc = std::string(elf_machine_name(info.machine)) + " 静态 ELF";
    } else if (require_static && !allow_dynamic) {
        die(std::string(what) + " 是动态链接的: " + path + "\n" +
            "  请使用静态编译版本（build.sh 可自动获取），或加 --allow-dynamic 强制打包。");
    } else {
        b.elf_desc = std::string(elf_machine_name(info.machine)) + " 动态 ELF（!）";
    }
    b.machine = info.machine;
    return b;
}

void compress_blob(Blob& b, const std::string& mode, const char* what) {
    b.stored = b.raw;
    b.lz4 = false;
    if (mode == "none") return;

    std::vector<uint8_t> c = lz4_compress(b.raw.data(), b.raw.size());
    if (c.empty()) {
        if (mode == "lz4") die(std::string(what) + " LZ4 压缩失败");
        return;
    }
    if (c.size() + 64 < b.raw.size()) {
        b.stored = std::move(c);
        b.lz4 = true;
    } else if (mode == "lz4") {
        std::fprintf(stderr, "shpack: 警告: %s 压缩后没有收益，按原样存储\n", what);
    }
}

void print_blob(const char* what, const Blob& b) {
    const double ratio = b.raw.empty() ? 0.0
                                       : (100.0 * static_cast<double>(b.stored.size()) /
                                          static_cast<double>(b.raw.size()));
    std::printf("  %-8s %-10s %8s -> %8s  %5.1f%%  %s\n", what,
                b.lz4 ? "LZ4" : "store", human_size(b.raw.size()).c_str(),
                human_size(b.stored.size()).c_str(), ratio, b.elf_desc.c_str());
}

void fill_footer(Footer& f, uint32_t flags, uint64_t total, uint64_t bash_off, uint64_t bb_off,
                 uint64_t script_off, uint64_t cfg_off, uint64_t cfg_size, const Blob& bash,
                 const Blob& bb, const Blob& script, uint32_t cfg_count,
                 const uint8_t build_id[8]) {
    std::memset(&f, 0, sizeof(f));
    std::memcpy(f.magic, kMagic, sizeof(kMagic));
    f.version = kFormatVersion;
    f.header_size = kFooterSize;
    f.flags = flags;
    f.cfg_count = cfg_count;
    f.total_size = total;
    f.bash_off = bash_off;
    f.bash_stored = bash.stored.size();
    f.bash_raw = bash.raw.size();
    f.bb_off = bb_off;
    f.bb_stored = bb.stored.size();
    f.bb_raw = bb.raw.size();
    f.script_off = script_off;
    f.script_stored = script.stored.size();
    f.script_raw = script.raw.size();
    f.cfg_off = cfg_off;
    f.cfg_size = cfg_size;
    sha256(bash.raw.data(), bash.raw.size(), f.bash_sha);
    sha256(bb.raw.data(), bb.raw.size(), f.bb_sha);
    sha256(script.raw.data(), script.raw.size(), f.script_sha);
    std::memcpy(f.build_id, build_id, 8);
}

void footer_seal(Footer& f) {
    std::memset(f.footer_sha, 0, sizeof(f.footer_sha));
    uint8_t d[32];
    sha256(&f, sizeof(f), d);
    std::memcpy(f.footer_sha, d, 16);
}

void write_output(const std::string& path, const std::vector<uint8_t>& data) {
    const std::string tmp = path + ".tmp." + std::to_string(static_cast<long>(::getpid()));
    ::unlink(tmp.c_str());
    int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0755);
    if (fd < 0) die(errno_msg(errno, "创建 " + tmp));
    if (!write_all(fd, data.data(), data.size())) {
        const std::string e = errno_msg(errno, "写入 " + tmp);
        ::close(fd);
        ::unlink(tmp.c_str());
        die(e);
    }
    if (::fchmod(fd, 0755) != 0) {
        const std::string e = errno_msg(errno, "chmod " + tmp);
        ::close(fd);
        ::unlink(tmp.c_str());
        die(e);
    }
    if (::fsync(fd) != 0) {
        const std::string e = errno_msg(errno, "fsync " + tmp);
        ::close(fd);
        ::unlink(tmp.c_str());
        die(e);
    }
    ::close(fd);
    if (::rename(tmp.c_str(), path.c_str()) != 0) {
        const std::string e = errno_msg(errno, "重命名 " + tmp + " -> " + path);
        ::unlink(tmp.c_str());
        die(e);
    }
}

void random_id(uint8_t out[8]) {
    int fd = ::open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        if (::read(fd, out, 8) == 8) {
            ::close(fd);
            return;
        }
        ::close(fd);
    }
    const uint64_t v = static_cast<uint64_t>(::getpid()) ^
                       (static_cast<uint64_t>(::time(nullptr)) << 20);
    std::memcpy(out, &v, 8);
}

}  // namespace

int main(int argc, char** argv) {
    Options opt = parse_args(argc, argv);
    ensure_output_not_input(opt.output, {opt.stub, opt.bash, opt.busybox, opt.script});

    // 1. 读取并校验 stub
    Blob stub;
    {
        std::string err;
        if (!read_file(opt.stub, stub.raw, 64ull * 1024 * 1024, err)) die("stub: " + err);
        if (stub.raw.size() > 8 && std::memcmp(stub.raw.data(), "\x7f" "ELF", 4) != 0)
            die("stub 不是 ELF 文件: " + opt.stub);
        if (stub.raw.size() >= kFooterSize &&
            std::memcmp(stub.raw.data() + stub.raw.size() - kFooterSize + 0, kMagic, 8) == 0) {
            die("stub 本身已经是一个 shpack 产物，请使用未打包的 build/shpack-stub");
        }
        ElfInfo info;
        if (!elf_inspect(stub.raw, info, err)) die("stub: " + err);
        if (info.has_interp && !opt.allow_dynamic)
            die("stub 是动态链接的，产出的 ELF 将依赖目标机 ld.so；请用 make 生成静态 stub");
        stub.machine = info.machine;
        stub.elf_desc = std::string(elf_machine_name(info.machine)) +
                        (info.has_interp ? " 动态 ELF（!）" : " 静态 ELF");
    }

    // 2. 读取 bash / busybox / 脚本
    Blob bash = load_blob(opt.bash, "bash", true, opt.allow_dynamic);
    Blob bb = load_blob(opt.busybox, "busybox", true, opt.allow_dynamic);
    Blob script;
    {
        std::string err;
        if (!read_file(opt.script, script.raw, kMaxPayload, err)) die("script: " + err);
        if (script.raw.empty()) die("脚本是空文件: " + opt.script);
        script.elf_desc = "shell 脚本";
    }

    if (bash.machine != bb.machine) {
        die("bash 与 busybox 架构不一致: " + std::string(elf_machine_name(bash.machine)) +
            " vs " + elf_machine_name(bb.machine));
    }
    if (bash.machine != stub.machine) {
        die("stub 与 payload 架构不一致: stub=" + std::string(elf_machine_name(stub.machine)) +
            ", payload=" + elf_machine_name(bash.machine) +
            "（请用相同目标架构的交叉编译器构建 stub）");
    }

    // 3. 压缩
    compress_blob(bash, opt.compress, "bash");
    compress_blob(bb, opt.compress, "busybox");
    compress_blob(script, opt.compress, "script");

    // 4. 配置表
    std::string cfg;
    uint32_t cfg_count = 0;
    auto add_cfg = [&](const char* k, const std::string& v) {
        cfg.append(k);
        cfg.push_back('=');
        cfg.append(v);
        cfg.push_back('\0');
        ++cfg_count;
    };
    add_cfg(kCfgName, opt.name);
    add_cfg(kCfgMode, opt.mode);
    add_cfg(kCfgPathMode, opt.pathmode);
    add_cfg(kCfgCleanup, opt.cleanup ? "1" : "0");
    add_cfg(kCfgArgv0, opt.name);
    add_cfg(kCfgVersion, kToolVersion);
    for (const auto& e : opt.envs) {
        if (e.find('=') == std::string::npos) die("--env 需要 K=V 形式: " + e);
        add_cfg(kCfgEnv, e);
    }
    for (const auto& o : opt.bashopts) {
        if (o.empty()) die("--bash-opt 不能为空");
        add_cfg(kCfgBashOpt, o);
    }
    if (cfg.size() > kMaxConfig) die("配置表过大");
    if (cfg_count > 100000) die("配置项过多");

    // 5. 布局：stub | bash | busybox | script | cfg | footer
    const auto align16 = [](uint64_t v) { return (v + 15) & ~static_cast<uint64_t>(15); };
    std::vector<uint8_t> image = stub.raw;
    const auto place = [&](const std::vector<uint8_t>& bytes) -> uint64_t {
        image.resize(static_cast<size_t>(align16(image.size())), 0);
        const uint64_t off = image.size();
        image.insert(image.end(), bytes.begin(), bytes.end());
        return off;
    };
    const uint64_t bash_off = place(bash.stored);
    const uint64_t bb_off = place(bb.stored);
    const uint64_t script_off = place(script.stored);
    const uint64_t cfg_off = place(std::vector<uint8_t>(cfg.begin(), cfg.end()));
    image.resize(static_cast<size_t>(align16(image.size())), 0);

    const uint64_t total = image.size() + kFooterSize;
    uint32_t flags = 0;
    if (bash.lz4) flags |= kFlagBashLz4;
    if (bb.lz4) flags |= kFlagBusyboxLz4;
    if (script.lz4) flags |= kFlagScriptLz4;

    uint8_t build_id[8];
    random_id(build_id);

    Footer footer;
    fill_footer(footer, flags, total, bash_off, bb_off, script_off, cfg_off, cfg.size(), bash, bb,
                script, cfg_count, build_id);
    footer_seal(footer);

    const uint8_t* fbytes = reinterpret_cast<const uint8_t*>(&footer);
    image.insert(image.end(), fbytes, fbytes + kFooterSize);
    if (image.size() != total) die("内部错误：布局大小不一致");

    write_output(opt.output, image);

    // 6. 报告
    if (opt.verbose) {
        std::printf("shpack %s\n", kToolVersion);
        std::printf("  stub     %s\n", stub.elf_desc.c_str());
        std::printf("  脚本     %s\n", opt.script.c_str());
        std::printf("  配置     %u 项, %s\n", cfg_count, human_size(cfg.size()).c_str());
    }
    std::printf("已生成 %s (%s)\n", opt.output.c_str(), human_size(image.size()).c_str());
    std::printf("  组件      方式         原始       存储      比例  说明\n");
    print_blob("bash", bash);
    print_blob("busybox", bb);
    print_blob("script", script);
    std::printf("  build id %s\n", to_hex(build_id, 8).c_str());
    std::printf("  用法: %s [--help] [--] [脚本参数...]\n", opt.output.c_str());
    return 0;
}
