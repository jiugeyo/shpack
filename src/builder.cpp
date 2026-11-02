// SPDX-License-Identifier: MIT
//
// shpack-builder：把 packer + 各架构 stub/bash/busybox 打包成单个静态 ELF。
// 自带交互式向导，也可以当普通 CLI 用：
//
//   ./shpack-builder                      # TTY 下进入交互向导
//   ./shpack-builder -s app.sh -o app -a aarch64
//   ./shpack-builder list
//   ./shpack-builder extract --arch aarch64 --out runtime
#include <sys/stat.h>
#include <sys/utsname.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include "bundle.h"
#include "format.h"
#include "package.h"
#include "sha256.h"
#include "util.h"

using namespace shpack;

namespace {

bool g_color = false;

std::string c(const char* code, const std::string& s) {
    if (!g_color) return s;
    return std::string("\033[") + code + "m" + s + "\033[0m";
}

std::string bold(const std::string& s) { return c("1", s); }
std::string green(const std::string& s) { return c("32", s); }
std::string dim(const std::string& s) { return c("2", s); }

bool tty() { return ::isatty(STDIN_FILENO) && ::isatty(STDOUT_FILENO); }

void usage() {
    std::printf(
        "%s %s - 单文件 shell 脚本打包器（内置多架构 bash/busybox）\n"
        "\n"
        "用法:\n"
        "  shpack-builder                         交互式向导（TTY）\n"
        "  shpack-builder pack -s SCRIPT -o OUT -a ARCH [选项]\n"
        "  shpack-builder list                    列出内嵌架构与组件\n"
        "  shpack-builder info                    显示 build id / 组件信息\n"
        "  shpack-builder extract --arch ARCH --out DIR\n"
        "  shpack-builder --version\n"
        "\n"
        "pack 选项:\n"
        "  -s, --script PATH       shell 脚本\n"
        "  -o, --output PATH       输出 ELF\n"
        "  -a, --arch ARCH         目标架构（见 list）\n"
        "  -n, --name NAME         包名 / 运行时脚本文件名\n"
        "  --compress MODE         auto | lz4 | none\n"
        "  --mode MODE             auto | memfd | dir\n"
        "  --path-mode MODE        busybox | system\n"
        "  --env K=V               追加环境变量（可重复）\n"
        "  --bash-opt OPT          传给 bash 的选项（可重复）\n"
        "  --no-cleanup            dir 模式保留临时目录\n"
        "  --bundle PATH           使用外部 bundle（开发调试）\n"
        "\n"
        "示例:\n"
        "  ./shpack-builder -s deploy.sh -o deploy -a aarch64\n"
        "  adb push deploy /data/local/tmp/ && adb shell chmod 755 /data/local/tmp/deploy\n",
        "shpack-builder", kToolVersion);
}

[[noreturn]] void die(const std::string& msg) {
    std::fprintf(stderr, "%s %s\n", c("31", "错误:").c_str(), msg.c_str());
    std::exit(1);
}

bool read_line(const std::string& label, const std::string& def, std::string& out) {
    std::printf("%s", label.c_str());
    if (!def.empty()) std::printf(" %s", dim("[" + def + "]").c_str());
    std::printf(": ");
    std::fflush(stdout);
    std::string line;
    if (!std::getline(std::cin, line)) return false;
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t'))
        line.pop_back();
    size_t start = 0;
    while (start < line.size() && (line[start] == ' ' || line[start] == '\t')) ++start;
    line = line.substr(start);
    if (line.size() >= 2 && ((line.front() == '"' && line.back() == '"') ||
                             (line.front() == '\'' && line.back() == '\''))) {
        line = line.substr(1, line.size() - 2);
    }
    out = line.empty() ? def : line;
    return true;
}

bool yes_no(const std::string& label, bool def) {
    for (;;) {
        std::string s;
        if (!read_line(label + " [y/N]", def ? "y" : "n", s)) return def;
        if (s == "y" || s == "Y" || s == "yes" || s == "YES") return true;
        if (s == "n" || s == "N" || s == "no" || s == "NO") return false;
        std::printf("请输入 y 或 n\n");
    }
}

int menu(const std::string& title, const std::vector<std::string>& items, int def) {
    std::printf("\n%s\n", bold(title).c_str());
    for (size_t i = 0; i < items.size(); ++i)
        std::printf("  %zu) %s\n", i + 1, items[i].c_str());
    for (;;) {
        std::string s;
        if (!read_line("选择", std::to_string(def + 1), s)) return def;
        char* end = nullptr;
        const long v = std::strtol(s.c_str(), &end, 10);
        if (end && *end == '\0' && v >= 1 && v <= static_cast<long>(items.size()))
            return static_cast<int>(v - 1);
        for (size_t i = 0; i < items.size(); ++i) {
            if (items[i] == s) return static_cast<int>(i);
        }
        std::printf("无效选择，请输入编号\n");
    }
}

std::string host_arch() {
    struct utsname u;
    if (::uname(&u) != 0) return "";
    const std::string arch = u.machine;
    if (arch == "x86_64" || arch == "amd64") return "x86_64";
    if (arch == "aarch64" || arch == "arm64") return "aarch64";
    if (arch == "i386" || arch == "i686" || arch == "x86") return "i386";
    if (arch.compare(0, 3, "arm") == 0) return "armv7";
    return arch;
}

std::string default_name(const std::string& script) {
    std::string n = base_name(script);
    if (n.size() > 3 && n.compare(n.size() - 3, 3, ".sh") == 0) n.resize(n.size() - 3);
    return sanitize_component(n, "app");
}

void print_stats(const std::string& output, const PackageResult& r) {
    std::printf("\n%s %s (%s)\n", green("✔ 已生成").c_str(), bold(output).c_str(),
                human_size(r.image.size()).c_str());
    std::printf("  包名    : %s\n", r.name.c_str());
    std::printf("  组件    : bash %s → %s%s, busybox %s → %s%s\n",
                human_size(r.bash.raw).c_str(), human_size(r.bash.stored).c_str(),
                r.bash.lz4 ? " (LZ4)" : "", human_size(r.busybox.raw).c_str(),
                human_size(r.busybox.stored).c_str(), r.busybox.lz4 ? " (LZ4)" : "");
    char hex[65];
    sha256_hex(r.image.data(), r.image.size(), hex);
    std::printf("  sha256  : %s\n", hex);
    std::printf("  build   : %s\n", to_hex(r.build_id, 8).c_str());
}

struct Request {
    std::string arch;
    std::string script;
    std::string output;
    std::string name;
    std::string mode = "auto";
    std::string compress = "auto";
    std::string pathmode = "busybox";
    std::vector<std::string> envs;
    std::vector<std::string> bashopts;
    bool cleanup = true;
};

bool build_one(Bundle& bundle, const Request& req, PackageResult& result, std::string& err) {
    if (!bundle.has(req.arch, "stub") || !bundle.has(req.arch, "bash") ||
        !bundle.has(req.arch, "busybox")) {
        err = "该 builder 未内嵌架构: " + req.arch;
        return false;
    }
    PackageInputs in;
    if (!bundle.read(req.arch, "stub", in.stub, err) ||
        !bundle.read(req.arch, "bash", in.bash, err) ||
        !bundle.read(req.arch, "busybox", in.busybox, err)) {
        return false;
    }
    if (!read_file(req.script, in.script, kMaxPayload, err)) return false;
    if (in.script.empty()) {
        err = "脚本是空文件: " + req.script;
        return false;
    }

    PackageOptions po;
    po.name = req.name.empty() ? default_name(req.script) : req.name;
    po.mode = req.mode;
    po.pathmode = req.pathmode;
    po.argv0 = po.name;
    po.compress = req.compress;
    po.cleanup = req.cleanup;
    po.envs = req.envs;
    po.bashopts = req.bashopts;
    if (!assemble_package(in, po, result, err)) return false;
    if (!write_file_atomic(req.output, result.image.data(), result.image.size(), 0755, err))
        return false;
    return true;
}

std::string format_entry_size(const Bundle& b, const std::string& arch, const std::string& kind) {
    for (const auto& e : b.entries()) {
        if (e.arch == arch && e.kind == kind) return human_size(e.raw_size);
    }
    return "-";
}

void cmd_list(const Bundle& b) {
    std::printf("builder bundle build: %s\n", b.build_id.c_str());
    const auto arches = b.arches();
    std::printf("内嵌架构 (%zu):\n", arches.size());
    for (const auto& a : arches) {
        std::printf("  %-8s  stub %-9s bash %-9s busybox %-9s\n", a.c_str(),
                    format_entry_size(b, a, "stub").c_str(),
                    format_entry_size(b, a, "bash").c_str(),
                    format_entry_size(b, a, "busybox").c_str());
    }
}

void cmd_info(const Bundle& b, const std::string& self) {
    std::printf("builder : %s\n", self.c_str());
    std::printf("version : %s\n", kToolVersion);
    std::printf("bundle  : %s\n", b.build_id.c_str());
    std::printf("base    : %llu\n", static_cast<unsigned long long>(b.base));
    cmd_list(b);
}

bool extract_one(const Bundle& b, const std::string& arch, const std::string& kind,
                 const std::string& out, std::string& err) {
    std::vector<uint8_t> data;
    if (!b.read(arch, kind, data, err)) return false;
    return write_file_atomic(out, data.data(), data.size(), 0755, err);
}

int cmd_extract(const Bundle& b, const std::vector<std::string>& args) {
    std::string arch, out, kind;
    for (size_t i = 0; i < args.size(); ++i) {
        const std::string& a = args[i];
        auto val = [&]() -> std::string {
            if (i + 1 >= args.size()) die("参数 " + a + " 缺少取值");
            return args[++i];
        };
        if (a == "--arch") arch = val();
        else if (a == "--out") out = val();
        else if (a == "--kind") kind = val();
        else die("extract 未知参数: " + a);
    }
    if (arch.empty() || out.empty()) die("用法: extract --arch ARCH --out DIR_OR_FILE [--kind K]");
    std::string err;
    if (!kind.empty()) {
        if (!extract_one(b, arch, kind, out, err)) die(err);
        std::printf("已写出 %s\n", out.c_str());
        return 0;
    }
    const std::string dir = out;
    if (::mkdir(dir.c_str(), 0755) != 0 && errno != EEXIST)
        die(errno_msg(errno, "创建 " + dir));
    for (const char* k : {"stub", "bash", "busybox"}) {
        if (!extract_one(b, arch, k, path_join(dir, k), err)) die(err);
        std::printf("  %s/%s\n", dir.c_str(), k);
    }
    return 0;
}

bool parse_pack_args(const std::vector<std::string>& args, Request& req) {
    bool have_script = false, have_output = false;
    for (size_t i = 0; i < args.size(); ++i) {
        const std::string& a = args[i];
        auto val = [&]() -> std::string {
            if (i + 1 >= args.size()) die("参数 " + a + " 缺少取值");
            return args[++i];
        };
        auto match = [&](const char* name) {
            const std::string n(name);
            return a == n || a.rfind(n + "=", 0) == 0;
        };
        if (match("-s") || match("--script")) {
            req.script = val();
            have_script = true;
        } else if (match("-o") || match("--output")) {
            req.output = val();
            have_output = true;
        } else if (match("-a") || match("--arch")) {
            req.arch = val();
        } else if (match("-n") || match("--name")) {
            req.name = val();
        } else if (match("--compress")) {
            req.compress = val();
        } else if (match("--mode")) {
            req.mode = val();
        } else if (match("--path-mode")) {
            req.pathmode = val();
        } else if (match("--env")) {
            req.envs.push_back(val());
        } else if (match("--bash-opt")) {
            req.bashopts.push_back(val());
        } else if (a == "--no-cleanup") {
            req.cleanup = false;
        } else if (a == "-h" || a == "--help") {
            usage();
            std::exit(0);
        } else {
            die("未知参数: " + a);
        }
    }
    if (!have_script || !have_output) die("pack 需要 -s 和 -o");
    if (req.name.empty()) req.name = default_name(req.script);
    return true;
}

bool interactive(Bundle& bundle) {
    const auto arches = bundle.arches();
    if (arches.empty()) {
        die("bundle 中没有可用架构");
    }

    std::printf("\n%s\n", bold("shpack-builder 交互式打包").c_str());
    std::printf("%s\n", dim("直接回车使用默认值；Ctrl-C 退出。").c_str());

    Request req;
    std::string script;
    for (;;) {
        if (!read_line("脚本路径", "", script)) return false;
        if (script.empty()) {
            std::printf("脚本路径不能为空\n");
            continue;
        }
        std::string err;
        std::vector<uint8_t> tmp;
        if (!read_file(script, tmp, kMaxPayload, err) || tmp.empty()) {
            std::printf("%s\n", (err.empty() ? "脚本为空" : err).c_str());
            continue;
        }
        std::printf("  %s\n", dim("大小 " + human_size(tmp.size())).c_str());
        break;
    }
    req.script = script;

    std::vector<std::string> arch_items;
    int def_arch = 0;
    const std::string host = host_arch();
    for (size_t i = 0; i < arches.size(); ++i) {
        arch_items.push_back(arches[i] + (arches[i] == host ? "  (本机)" : ""));
        if (arches[i] == host) def_arch = static_cast<int>(i);
    }
    const int ai = menu("目标架构", arch_items, def_arch);
    req.arch = arches[static_cast<size_t>(ai)];

    req.name = default_name(script);
    read_line("包名", req.name, req.name);
    req.name = sanitize_component(req.name, "app");

    std::string output = req.name;
    for (;;) {
        if (!read_line("输出文件", output, output)) return false;
        if (output.empty()) output = req.name;
        const std::string dir = dir_name(output);
        if (::access(dir.c_str(), W_OK | X_OK) != 0) {
            std::printf("目录不可写: %s\n", dir.c_str());
            continue;
        }
        if (::access(output.c_str(), F_OK) == 0 &&
            !yes_no("文件已存在，覆盖?", false)) {
            continue;
        }
        break;
    }
    req.output = output;

    const int mi = menu("运行模式（memfd 不落盘）",
                        {"auto（优先纯内存）", "memfd（强制纯内存）", "dir（落盘，兼容性最好）"}, 0);
    req.mode = mi == 0 ? "auto" : (mi == 1 ? "memfd" : "dir");

    const int ci = menu("压缩", {"auto（推荐）", "lz4", "none"}, 0);
    req.compress = ci == 0 ? "auto" : (ci == 1 ? "lz4" : "none");

    std::printf("\n%s\n", dim("环境变量（K=V），留空结束：").c_str());
    for (;;) {
        std::string kv;
        if (!read_line("  env", "", kv)) break;
        if (kv.empty()) break;
        if (kv.find('=') == std::string::npos) {
            std::printf("  需要 K=V 形式\n");
            continue;
        }
        req.envs.push_back(kv);
    }

    std::string opts;
    read_line("bash 选项（空格分隔，可留空）", "", opts);
    for (const auto& o : split(opts, ' ')) {
        if (!o.empty()) req.bashopts.push_back(o);
    }

    std::printf("\n%s\n", bold("确认").c_str());
    std::printf("  脚本   : %s\n", req.script.c_str());
    std::printf("  架构   : %s\n", req.arch.c_str());
    std::printf("  输出   : %s\n", req.output.c_str());
    std::printf("  名称   : %s\n", req.name.c_str());
    std::printf("  模式   : %s\n", req.mode.c_str());
    std::printf("  压缩   : %s\n", req.compress.c_str());
    if (!req.envs.empty()) {
        std::printf("  环境   :");
        for (const auto& e : req.envs) std::printf(" %s", e.c_str());
        std::printf("\n");
    }
    if (!yes_no("开始打包?", true)) return false;

    PackageResult result;
    std::string err;
    if (!build_one(bundle, req, result, err)) {
        std::printf("%s %s\n", c("31", "失败:").c_str(), err.c_str());
        return yes_no("再试一次?", true);
    }
    print_stats(req.output, result);
    std::printf("\n  下一步: %s\n",
                dim("adb push " + base_name(req.output) +
                    " /data/local/tmp/ && adb shell chmod 755 /data/local/tmp/" +
                    base_name(req.output))
                    .c_str());
    return yes_no("\n再打一个?", false);
}

}  // namespace

int main(int argc, char** argv) {
    g_color = tty() && ::getenv("NO_COLOR") == nullptr;

    std::string bundle_path;
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--bundle") {
            if (i + 1 >= argc) die("--bundle 缺少取值");
            bundle_path = argv[++i];
        } else if (a.rfind("--bundle=", 0) == 0) {
            bundle_path = a.substr(9);
        } else {
            args.push_back(a);
        }
    }

    if (!args.empty() && (args[0] == "-h" || args[0] == "--help")) {
        usage();
        return 0;
    }
    if (!args.empty() && args[0] == "--version") {
        std::printf("shpack-builder %s\n", kToolVersion);
        return 0;
    }

    // 没有 bundle 时，--help/--version 之外全部报错。
    Bundle bundle;
    std::string err;
    std::string self;
    if (bundle_path.empty()) {
        self = executable_path(argc > 0 ? argv[0] : nullptr);
        if (!bundle.load(self, false, err)) {
            std::fprintf(stderr, "%s %s\n", c("33", "提示:").c_str(),
                         "这个二进制没有内嵌 bundle（用 scripts/make-builder.sh 生成完整版）");
            if (!args.empty()) die(err);
            usage();
            return 1;
        }
    } else {
        self = bundle_path;
        if (!bundle.load(bundle_path, true, err)) die(err);
    }

    if (args.empty()) {
        if (!tty()) {
            usage();
            return 0;
        }
        while (interactive(bundle)) {
        }
        return 0;
    }

    const std::string cmd = args[0];
    if (cmd == "list" || cmd == "--list") {
        cmd_list(bundle);
        return 0;
    }
    if (cmd == "info") {
        cmd_info(bundle, self);
        return 0;
    }
    if (cmd == "extract") {
        return cmd_extract(bundle, std::vector<std::string>(args.begin() + 1, args.end()));
    }
    if (cmd == "interactive") {
        while (interactive(bundle)) {
        }
        return 0;
    }

    std::vector<std::string> pack_args;
    if (cmd == "pack") {
        pack_args.assign(args.begin() + 1, args.end());
    } else {
        pack_args = args;
    }
    Request req;
    parse_pack_args(pack_args, req);
    if (req.arch.empty()) req.arch = host_arch();
    if (!bundle.has(req.arch, "stub")) {
        std::fprintf(stderr, "%s 未内嵌架构 %s，可选：", c("31", "错误:").c_str(),
                     req.arch.c_str());
        for (const auto& a : bundle.arches()) std::fprintf(stderr, " %s", a.c_str());
        std::fprintf(stderr, "\n");
        return 1;
    }

    PackageResult result;
    if (!build_one(bundle, req, result, err)) die(err);
    print_stats(req.output, result);
    return 0;
}
