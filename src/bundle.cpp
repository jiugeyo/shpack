// SPDX-License-Identifier: MIT
#include "bundle.h"

#include <fcntl.h>
#include <stdio.h>
#include <cstring>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

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

constexpr char kBundleMagic[8] = {'S', 'H', 'P', 'K', 'B', 'N', 'D', '1'};

#pragma pack(push, 1)
struct BEntry {
    char arch[16];
    char kind[12];
    uint32_t flags;
    uint32_t machine;
    uint64_t raw_size;
    uint64_t stored_size;
    uint64_t offset;
    uint8_t sha[32];
};

struct BFooter {
    char magic[8];
    uint32_t version;
    uint32_t count;
    uint64_t total_size;
    uint64_t index_off;
    uint64_t index_size;
    uint8_t build_id[8];
    uint8_t footer_sha[16];
    uint8_t reserved[64];
};
#pragma pack(pop)

static_assert(sizeof(BEntry) == 92, "BEntry 大小固定");
static_assert(sizeof(BFooter) == 128, "BFooter 必须 128 字节");

std::string fixed_str(const char* p, size_t n) {
    size_t len = 0;
    while (len < n && p[len] != '\0') ++len;
    return std::string(p, len);
}

void copy_fixed(char* dst, size_t n, const std::string& s) {
    std::memset(dst, 0, n);
    const size_t len = s.size() < n ? s.size() : n - 1;
    std::memcpy(dst, s.data(), len);
}

void seal_footer(BFooter& f) {
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

Bundle::~Bundle() {
    if (fd_ >= 0) ::close(fd_);
}

bool Bundle::load(const std::string& file, bool required, std::string& err) {
    path = file;
    fd_ = ::open(file.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd_ < 0) {
        err = errno_msg(errno, "打开 " + file);
        return false;
    }
    struct stat st;
    if (::fstat(fd_, &st) != 0 || st.st_size < static_cast<off_t>(kBundleFooterSize)) {
        err = "文件太小，没有内嵌 bundle";
        if (required) err = file + ": " + err;
        return false;
    }
    file_size_ = static_cast<uint64_t>(st.st_size);

    BFooter f;
    if (!pread_all(fd_, file_size_ - kBundleFooterSize, &f, sizeof(f))) {
        err = "读取 bundle Footer 失败";
        return false;
    }
    if (std::memcmp(f.magic, kBundleMagic, sizeof(kBundleMagic)) != 0) {
        err = "没有内嵌 bundle（末尾找不到魔数）";
        if (required) err = file + ": " + err;
        return false;
    }
    if (f.version != kBundleVersion) {
        err = "bundle 版本不支持: " + std::to_string(f.version);
        return false;
    }
    if (f.count == 0 || f.count > kBundleMaxEntries) {
        err = "bundle 条目数量异常";
        return false;
    }
    if (f.total_size > file_size_ || f.total_size < kBundleFooterSize) {
        err = "bundle 大小字段异常";
        return false;
    }
    base = file_size_ - f.total_size;
    if (f.index_off + f.index_size > f.total_size - kBundleFooterSize) {
        err = "bundle 索引越界";
        return false;
    }
    const uint64_t need = static_cast<uint64_t>(f.count) * sizeof(BEntry);
    if (f.index_size != need) {
        err = "bundle 索引大小不符";
        return false;
    }

    uint8_t saved[16];
    std::memcpy(saved, f.footer_sha, sizeof(saved));
    std::memset(f.footer_sha, 0, sizeof(f.footer_sha));
    uint8_t digest[32];
    sha256(&f, sizeof(f), digest);
    if (std::memcmp(digest, saved, sizeof(saved)) != 0) {
        err = "bundle Footer 校验失败";
        return false;
    }

    std::vector<BEntry> raw(f.count);
    if (!pread_all(fd_, base + f.index_off, raw.data(), raw.size() * sizeof(BEntry))) {
        err = "读取 bundle 索引失败";
        return false;
    }
    entries_.clear();
    for (const auto& e : raw) {
        BundleEntryInfo info;
        info.arch = fixed_str(e.arch, sizeof(e.arch));
        info.kind = fixed_str(e.kind, sizeof(e.kind));
        info.flags = e.flags;
        info.machine = e.machine;
        info.raw_size = e.raw_size;
        info.stored_size = e.stored_size;
        info.offset = e.offset;
        std::memcpy(info.sha, e.sha, 32);
        if (info.arch.empty() || info.kind.empty() || info.stored_size == 0 ||
            info.offset + info.stored_size > f.total_size - kBundleFooterSize) {
            err = "bundle 条目损坏";
            return false;
        }
        entries_.push_back(std::move(info));
    }
    build_id = to_hex(f.build_id, 8);
    return true;
}

bool Bundle::has(const std::string& arch, const std::string& kind) const {
    for (const auto& e : entries_) {
        if (e.arch == arch && e.kind == kind) return true;
    }
    return false;
}

bool Bundle::read(const std::string& arch, const std::string& kind, std::vector<uint8_t>& out,
                  std::string& err) const {
    const BundleEntryInfo* hit = nullptr;
    for (const auto& e : entries_) {
        if (e.arch == arch && e.kind == kind) {
            hit = &e;
            break;
        }
    }
    if (hit == nullptr) {
        err = "bundle 中没有 " + arch + "/" + kind;
        return false;
    }
    if (hit->raw_size > kMaxPayload || hit->stored_size > kMaxPayload) {
        err = arch + "/" + kind + " 尺寸超限";
        return false;
    }
    std::vector<uint8_t> stored(static_cast<size_t>(hit->stored_size));
    if (!pread_all(fd_, base + hit->offset, stored.data(), stored.size())) {
        err = "读取 " + arch + "/" + kind + " 失败";
        return false;
    }
    out.clear();
    if (hit->lz4()) {
        out.resize(static_cast<size_t>(hit->raw_size));
        size_t got = 0;
        if (!lz4_decompress(stored.data(), stored.size(), out.data(), out.size(), &got) ||
            got != out.size()) {
            err = arch + "/" + kind + " 解压失败";
            return false;
        }
    } else {
        out = std::move(stored);
    }
    uint8_t digest[32];
    sha256(out.data(), out.size(), digest);
    if (std::memcmp(digest, hit->sha, sizeof(digest)) != 0) {
        err = arch + "/" + kind + " SHA-256 校验失败";
        return false;
    }
    return true;
}

std::vector<std::string> Bundle::arches() const {
    std::vector<std::string> out;
    for (const auto& e : entries_) {
        bool seen = false;
        for (const auto& a : out) {
            if (a == e.arch) {
                seen = true;
                break;
            }
        }
        if (!seen) out.push_back(e.arch);
    }
    return out;
}

bool write_bundle(const std::string& path, const std::vector<BundleItem>& items, std::string& err,
                  std::string& build_id_hex) {
    if (items.empty()) {
        err = "没有要写入 bundle 的组件";
        return false;
    }
    if (items.size() > kBundleMaxEntries) {
        err = "bundle 条目过多";
        return false;
    }

    std::vector<uint8_t> data;
    std::vector<BEntry> index;
    index.reserve(items.size());
    for (const auto& item : items) {
        if (item.arch.empty() || item.arch.size() >= sizeof(BEntry::arch)) {
            err = "架构名无效: " + item.arch;
            return false;
        }
        if (item.kind.empty() || item.kind.size() >= sizeof(BEntry::kind)) {
            err = "组件名无效: " + item.kind;
            return false;
        }
        if (item.raw.empty() || item.raw.size() > kMaxPayload) {
            err = "组件为空或过大: " + item.arch + "/" + item.kind;
            return false;
        }
        for (const auto& e : index) {
            if (fixed_str(e.arch, sizeof(e.arch)) == item.arch &&
                fixed_str(e.kind, sizeof(e.kind)) == item.kind) {
                err = "重复的 bundle 条目: " + item.arch + "/" + item.kind;
                return false;
            }
        }

        std::vector<uint8_t> stored = item.raw;
        uint32_t flags = 0;
        std::vector<uint8_t> c = lz4_compress(item.raw.data(), item.raw.size());
        if (!c.empty() && c.size() + 64 < item.raw.size()) {
            stored = std::move(c);
            flags |= 1u;
        }

        data.resize(static_cast<size_t>((data.size() + 15) & ~static_cast<uint64_t>(15)), 0);
        BEntry e{};
        copy_fixed(e.arch, sizeof(e.arch), item.arch);
        copy_fixed(e.kind, sizeof(e.kind), item.kind);
        e.flags = flags;
        e.machine = item.machine;
        e.raw_size = item.raw.size();
        e.stored_size = stored.size();
        e.offset = data.size();
        sha256(item.raw.data(), item.raw.size(), e.sha);
        data.insert(data.end(), stored.begin(), stored.end());
        index.push_back(e);
    }

    data.resize(static_cast<size_t>((data.size() + 15) & ~static_cast<uint64_t>(15)), 0);
    const uint64_t index_off = data.size();
    const uint64_t index_size = index.size() * sizeof(BEntry);
    const uint64_t total = index_off + index_size + kBundleFooterSize;

    BFooter f{};
    std::memcpy(f.magic, kBundleMagic, sizeof(kBundleMagic));
    f.version = kBundleVersion;
    f.count = static_cast<uint32_t>(index.size());
    f.total_size = total;
    f.index_off = index_off;
    f.index_size = index_size;
    random_id(f.build_id);
    seal_footer(f);
    build_id_hex = to_hex(f.build_id, 8);

    std::vector<uint8_t> image;
    image.reserve(static_cast<size_t>(total));
    image.insert(image.end(), data.begin(), data.end());
    const uint8_t* eb = reinterpret_cast<const uint8_t*>(index.data());
    image.insert(image.end(), eb, eb + index_size);
    const uint8_t* fb = reinterpret_cast<const uint8_t*>(&f);
    image.insert(image.end(), fb, fb + kBundleFooterSize);
    if (image.size() != total) {
        err = "内部错误：bundle 布局不一致";
        return false;
    }
    return write_file_atomic(path, image.data(), image.size(), 0644, err);
}

}  // namespace shpack
