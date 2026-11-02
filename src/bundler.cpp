// SPDX-License-Identifier: MIT
//
// 生成 builder 内嵌 bundle（不含 ELF 本身）。
//
//   shpack-bundler -o build/shpack.bundle
//     --arch x86_64  --stub build/x86_64/shpack-stub  --bash ... --busybox ...
//     --arch aarch64 --stub build/aarch64/shpack-stub --bash ... --busybox ...
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "bundle.h"
#include "format.h"
#include "util.h"

using namespace shpack;

namespace {

struct Group {
    std::string arch;
    std::string stub;
    std::string bash;
    std::string busybox;
};

[[noreturn]] void die(const std::string& msg) {
    std::fprintf(stderr, "shpack-bundler: 错误: %s\n", msg.c_str());
    std::exit(2);
}

void usage() {
    std::fprintf(stdout,
        "shpack-bundler %s - 生成可追加到 builder 的多架构 bundle\n"
        "\n"
        "用法:\n"
        "  shpack-bundler -o OUT --arch NAME --stub FILE --bash FILE --busybox FILE [更多 --arch ...]\n"
        "\n"
        "说明:\n"
        "  --arch 可重复；每个架构必须提供 stub/bash/busybox 三个静态 ELF。\n"
        "  产物是纯数据文件，用 cat builder bundle > shpack-builder 追加。\n",
        kToolVersion);
}

std::string need_value(int argc, char** argv, int& i, const std::string& opt) {
    const std::string a = argv[i];
    const std::string prefix = opt + "=";
    if (a.rfind(prefix, 0) == 0) return a.substr(prefix.size());
    if (i + 1 >= argc) die("参数 " + opt + " 缺少取值");
    return argv[++i];
}

std::vector<uint8_t> load_elf(const std::string& path, const char* what, uint16_t& machine) {
    std::vector<uint8_t> data;
    std::string err;
    if (!read_file(path, data, 64ull * 1024 * 1024, err)) die(std::string(what) + ": " + err);
    ElfInfo info;
    if (!elf_inspect(data, info, err)) die(std::string(what) + " (" + path + "): " + err);
    if (info.has_interp)
        die(std::string(what) + " 是动态链接的: " + path + "（bundle 只接受静态 ELF）");
    machine = info.machine;
    return data;
}

}  // namespace

int main(int argc, char** argv) {
    std::string output;
    std::vector<Group> groups;
    Group cur;
    bool want_help = false;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "-h" || a == "--help") {
            want_help = true;
        } else if (a == "-o" || a == "--output" || a.rfind("--output=", 0) == 0) {
            output = need_value(argc, argv, i, "--output");
        } else if (a == "--arch" || a.rfind("--arch=", 0) == 0) {
            const std::string v = need_value(argc, argv, i, "--arch");
            if (!cur.arch.empty() || !cur.stub.empty() || !cur.bash.empty() || !cur.busybox.empty()) {
                groups.push_back(cur);
                cur = Group{};
            }
            cur.arch = v;
        } else if (a == "--stub" || a.rfind("--stub=", 0) == 0) {
            cur.stub = need_value(argc, argv, i, "--stub");
        } else if (a == "--bash" || a.rfind("--bash=", 0) == 0) {
            cur.bash = need_value(argc, argv, i, "--bash");
        } else if (a == "--busybox" || a.rfind("--busybox=", 0) == 0) {
            cur.busybox = need_value(argc, argv, i, "--busybox");
        } else if (a == "--version") {
            std::printf("shpack-bundler %s\n", kToolVersion);
            return 0;
        } else {
            die("未知参数: " + a);
        }
    }
    if (want_help) {
        usage();
        return 0;
    }
    if (!cur.arch.empty() || !cur.stub.empty() || !cur.bash.empty() || !cur.busybox.empty())
        groups.push_back(cur);
    if (output.empty()) {
        usage();
        die("缺少 -o 输出路径");
    }
    if (groups.empty()) {
        usage();
        die("至少需要一个 --arch 组");
    }

    std::vector<BundleItem> items;
    for (const auto& g : groups) {
        if (g.arch.empty() || g.stub.empty() || g.bash.empty() || g.busybox.empty())
            die("每个 --arch 组都必须提供 stub/bash/busybox");
        uint16_t m_stub = 0, m_bash = 0, m_bb = 0;
        BundleItem s{"", "stub", load_elf(g.stub, "stub", m_stub), m_stub};
        BundleItem b{"", "bash", load_elf(g.bash, "bash", m_bash), m_bash};
        BundleItem x{"", "busybox", load_elf(g.busybox, "busybox", m_bb), m_bb};
        if (m_stub != m_bash || m_bash != m_bb)
            die("架构不一致: " + g.arch + "（stub/bash/busybox 的 e_machine 不同）");
        s.arch = b.arch = x.arch = g.arch;
        std::printf("  %-8s %-4s %s\n", g.arch.c_str(), elf_machine_name(m_stub),
                    human_size(s.raw.size() + b.raw.size() + x.raw.size()).c_str());
        items.push_back(std::move(s));
        items.push_back(std::move(b));
        items.push_back(std::move(x));
    }

    std::string err, build_id;
    if (!write_bundle(output, items, err, build_id)) die(err);
    std::printf("已生成 %s（%zu 个组件，build %s）\n", output.c_str(), items.size(),
                build_id.c_str());
    return 0;
}
