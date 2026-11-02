// SPDX-License-Identifier: MIT
#include "package.h"

#include <fcntl.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <utility>
#include <vector>

#include "format.h"
#include "lz4.h"
#include "sha256.h"
#include "util.h"

namespace shpack {
namespace {

struct Blob {
    std::vector<uint8_t> raw;
    std::vector<uint8_t> stored;
    bool lz4 = false;
    uint16_t machine = 0;
    std::string desc;
};

bool inspect_blob(Blob& b, const char* what, bool allow_dynamic, std::string& err) {
    if (b.raw.empty()) {
        err = std::string(what) + " 是空文件";
        return false;
    }
    ElfInfo info;
    if (!elf_inspect(b.raw, info, err)) {
        err = std::string(what) + ": " + err;
        return false;
    }
    if (info.has_interp && !allow_dynamic) {
        err = std::string(what) + " 是动态链接的，产物无法“到处运行”";
        return false;
    }
    b.machine = info.machine;
    b.desc = std::string(elf_machine_name(info.machine)) +
             (info.has_interp ? " 动态 ELF（!）" : " 静态 ELF");
    return true;
}

bool compress_blob(Blob& b, const std::string& mode, const char* what, std::string& err) {
    b.stored = b.raw;
    b.lz4 = false;
    if (mode == "none") return true;

    std::vector<uint8_t> c = lz4_compress(b.raw.data(), b.raw.size());
    if (c.empty()) {
        if (mode == "lz4") {
            err = std::string(what) + " LZ4 压缩失败";
            return false;
        }
        return true;
    }
    if (c.size() + 64 < b.raw.size()) {
        b.stored = std::move(c);
        b.lz4 = true;
    } else if (mode == "lz4") {
        std::fprintf(stderr, "shpack: 警告: %s 压缩后没有收益，按原样存储\n", what);
    }
    return true;
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

void random_id(uint8_t out[8]) {
    int fd = ::open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        const ssize_t n = ::read(fd, out, 8);
        ::close(fd);
        if (n == 8) return;
    }
    const uint64_t v = static_cast<uint64_t>(::getpid()) ^
                       (static_cast<uint64_t>(::time(nullptr)) << 20);
    std::memcpy(out, &v, 8);
}

}  // namespace

bool assemble_package(const PackageInputs& in, const PackageOptions& opt, PackageResult& out,
                      std::string& err) {
    if (opt.compress != "auto" && opt.compress != "none" && opt.compress != "lz4") {
        err = "--compress 只支持 auto|none|lz4";
        return false;
    }
    if (opt.pathmode != "busybox" && opt.pathmode != "system") {
        err = "--path-mode 只支持 busybox|system";
        return false;
    }
    if (opt.mode != "auto" && opt.mode != "dir" && opt.mode != "memfd") {
        err = "--mode 只支持 auto|dir|memfd";
        return false;
    }

    Blob stub, bash, bb, script;
    stub.raw = in.stub;
    bash.raw = in.bash;
    bb.raw = in.busybox;
    script.raw = in.script;

    if (stub.raw.size() < 4 || std::memcmp(stub.raw.data(), "\x7f" "ELF", 4) != 0) {
        err = "stub 不是 ELF 文件";
        return false;
    }
    if (stub.raw.size() >= kFooterSize &&
        std::memcmp(stub.raw.data() + stub.raw.size() - kFooterSize, kMagic, 8) == 0) {
        err = "stub 本身已经是一个 shpack 产物";
        return false;
    }
    if (!inspect_blob(stub, "stub", opt.allow_dynamic, err)) return false;
    if (!inspect_blob(bash, "bash", opt.allow_dynamic, err)) return false;
    if (!inspect_blob(bb, "busybox", opt.allow_dynamic, err)) return false;
    if (script.raw.empty()) {
        err = "脚本是空文件";
        return false;
    }
    if (bash.machine != bb.machine) {
        err = std::string("bash 与 busybox 架构不一致: ") + elf_machine_name(bash.machine) +
              " vs " + elf_machine_name(bb.machine);
        return false;
    }
    if (bash.machine != stub.machine) {
        err = std::string("stub 与 payload 架构不一致: stub=") +
              elf_machine_name(stub.machine) + ", payload=" + elf_machine_name(bash.machine);
        return false;
    }

    if (!compress_blob(bash, opt.compress, "bash", err)) return false;
    if (!compress_blob(bb, opt.compress, "busybox", err)) return false;
    if (!compress_blob(script, opt.compress, "script", err)) return false;

    std::string name = sanitize_component(opt.name, "app");
    std::string argv0 = opt.argv0.empty() ? name : opt.argv0;

    std::string cfg;
    uint32_t cfg_count = 0;
    auto add_cfg = [&](const char* k, const std::string& v) {
        cfg.append(k);
        cfg.push_back('=');
        cfg.append(v);
        cfg.push_back('\0');
        ++cfg_count;
    };
    add_cfg(kCfgName, name);
    add_cfg(kCfgMode, opt.mode);
    add_cfg(kCfgPathMode, opt.pathmode);
    add_cfg(kCfgCleanup, opt.cleanup ? "1" : "0");
    add_cfg(kCfgArgv0, argv0);
    add_cfg(kCfgVersion, kToolVersion);
    for (const auto& e : opt.envs) {
        if (e.find('=') == std::string::npos) {
            err = "--env 需要 K=V 形式: " + e;
            return false;
        }
        add_cfg(kCfgEnv, e);
    }
    for (const auto& o : opt.bashopts) {
        if (o.empty()) {
            err = "--bash-opt 不能为空";
            return false;
        }
        add_cfg(kCfgBashOpt, o);
    }
    if (cfg.size() > kMaxConfig) {
        err = "配置表过大";
        return false;
    }
    if (cfg_count > 100000) {
        err = "配置项过多";
        return false;
    }

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
    if (image.size() != total) {
        err = "内部错误：布局大小不一致";
        return false;
    }

    out.image = std::move(image);
    out.bash = BlobStat{bash.raw.size(), bash.stored.size(), bash.lz4, bash.desc};
    out.busybox = BlobStat{bb.raw.size(), bb.stored.size(), bb.lz4, bb.desc};
    out.script = BlobStat{script.raw.size(), script.stored.size(), script.lz4, script.desc};
    std::memcpy(out.build_id, build_id, 8);
    out.cfg_count = cfg_count;
    out.cfg_size = cfg.size();
    out.name = name;
    return true;
}

}  // namespace shpack
