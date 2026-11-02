// SPDX-License-Identifier: MIT
//
// 文件版打包器：读取 stub/bash/busybox/脚本，组装成 shpack 产物。
// 单文件交互式版本见 builder.cpp。
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "format.h"
#include "package.h"
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
    std::string compress = "auto";
    std::string pathmode = "busybox";
    std::string mode = "auto";
    std::vector<std::string> envs;
    std::vector<std::string> bashopts;
    bool cleanup = true;
    bool allow_dynamic = false;
    bool verbose = false;
};

[[noreturn]] void die(const std::string& msg) {
    std::fprintf(stderr, "shpack: 错误: %s\n", msg.c_str());
    std::exit(2);
}

void ensure_output_not_input(const std::string& out,
                             const std::vector<std::string>& inputs) {
    struct stat so;
    if (::stat(out.c_str(), &so) != 0) return;
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
        "  --stub PATH       静态 launcher（make 生成 build/<arch>/shpack-stub）\n"
        "  --bash PATH       静态链接的 bash\n"
        "  --busybox PATH    静态链接的 busybox\n"
        "  --script PATH     shell 脚本\n"
        "  -o, --output PATH 输出 ELF 路径\n"
        "\n"
        "可选参数:\n"
        "  --name NAME       包名，同时作为运行时脚本文件名（默认取脚本文件名）\n"
        "  --env K=V         追加环境变量，可重复\n"
        "  --bash-opt OPT    追加 bash 选项（放在脚本路径之前），可重复\n"
        "  --path-mode M     busybox | system（默认 busybox）\n"
        "  --mode M          auto | memfd | dir（运行时默认策略，默认 auto）\n"
        "  --no-cleanup      运行结束后保留临时目录\n"
        "  --compress M      auto | none | lz4（默认 auto：仅有收益时压缩）\n"
        "  --allow-dynamic   跳过静态链接检查（仅用于调试）\n"
        "  -v, --verbose     输出更多信息\n"
        "  -h, --help        显示帮助\n"
        "  --version         显示版本\n"
        "\n"
        "单文件交互式 builder 请使用 shpack-builder。\n",
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
    if (opt.name.empty()) {
        std::string n = base_name(opt.script);
        if (n.size() > 3 && n.compare(n.size() - 3, 3, ".sh") == 0) n.resize(n.size() - 3);
        opt.name = n;
    }
    opt.name = sanitize_component(opt.name, "app");
    return opt;
}

void print_blob(const char* what, const BlobStat& b) {
    const double ratio = b.raw == 0 ? 0.0
                                    : (100.0 * static_cast<double>(b.stored) /
                                       static_cast<double>(b.raw));
    std::printf("  %-8s %-10s %8s -> %8s  %5.1f%%  %s\n", what, b.lz4 ? "LZ4" : "store",
                human_size(b.raw).c_str(), human_size(b.stored).c_str(), ratio, b.desc.c_str());
}

bool read_or_die(const std::string& path, std::vector<uint8_t>& out, uint64_t cap,
                 const char* what) {
    std::string err;
    if (!read_file(path, out, cap, err)) die(std::string(what) + ": " + err);
    return !out.empty();
}

}  // namespace

int main(int argc, char** argv) {
    Options opt = parse_args(argc, argv);
    ensure_output_not_input(opt.output, {opt.stub, opt.bash, opt.busybox, opt.script});

    PackageInputs in;
    read_or_die(opt.stub, in.stub, 64ull * 1024 * 1024, "stub");
    read_or_die(opt.bash, in.bash, kMaxPayload, "bash");
    read_or_die(opt.busybox, in.busybox, kMaxPayload, "busybox");
    read_or_die(opt.script, in.script, kMaxPayload, "script");

    PackageOptions po;
    po.name = opt.name;
    po.mode = opt.mode;
    po.pathmode = opt.pathmode;
    po.argv0 = opt.name;
    po.compress = opt.compress;
    po.cleanup = opt.cleanup;
    po.allow_dynamic = opt.allow_dynamic;
    po.envs = opt.envs;
    po.bashopts = opt.bashopts;

    PackageResult res;
    std::string err;
    if (!assemble_package(in, po, res, err)) die(err);

    mode_t omode = 0755;
    struct stat sst;
    if (::stat(opt.script.c_str(), &sst) == 0) {
        const mode_t m = sst.st_mode & 0777;
        if (m != 0) omode = m;
    }
    if (!write_file_atomic(opt.output, res.image.data(), res.image.size(), omode, err))
        die(err);

    if (opt.verbose) {
        std::printf("shpack %s\n", kToolVersion);
        std::printf("  脚本     %s\n", opt.script.c_str());
        std::printf("  权限     %03o（跟随脚本）\n", static_cast<unsigned>(omode));
        std::printf("  配置     %u 项, %s\n", res.cfg_count, human_size(res.cfg_size).c_str());
    }
    std::printf("已生成 %s (%s)\n", opt.output.c_str(), human_size(res.image.size()).c_str());
    std::printf("  组件      方式         原始       存储      比例  说明\n");
    print_blob("bash", res.bash);
    print_blob("busybox", res.busybox);
    print_blob("script", res.script);
    std::printf("  build id %s\n", to_hex(res.build_id, 8).c_str());
    std::printf("  用法: %s [--help] [--] [脚本参数...]\n", opt.output.c_str());
    return 0;
}
