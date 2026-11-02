#!/usr/bin/env python3
"""独立于 C++ launcher 的包格式校验器（用于交叉验证打包结果）。

用法: python3 tests/verify_package.py PKG [--expect-arch aarch64]
只使用标准库；LZ4 负载会跳过内容哈希（仍校验长度/偏移/ELF 头）。
"""
import hashlib
import struct
import sys

FOOTER = struct.Struct("<8sIIIIQ11Q32s32s32s8s16s16s")
assert FOOTER.size == 256

FLAG_LZ4 = [1 << 0, 1 << 1, 1 << 2]
MAGIC = b"SHPACK01"


def elf_machine(data):
    if len(data) < 20 or data[:4] != b"\x7fELF":
        return None
    return struct.unpack_from("<H", data, 18)[0]


NAMES = {62: "x86_64", 183: "aarch64", 3: "i386", 40: "ARM"}


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    path = sys.argv[1]
    expect = None
    if "--expect-arch" in sys.argv:
        expect = sys.argv[sys.argv.index("--expect-arch") + 1]

    raw = open(path, "rb").read()
    if len(raw) < 256:
        print("FAIL: 文件太小")
        return 1
    f = FOOTER.unpack(raw[-256:])
    (magic, version, header_size, flags, cfg_count, total, b_off, b_st, b_raw,
     bb_off, bb_st, bb_raw, s_off, s_st, s_raw, c_off, c_size, b_sha, bb_sha,
     s_sha, build_id, f_sha, _reserved) = f

    errors = []

    def check(cond, msg):
        if not cond:
            errors.append(msg)

    check(magic == MAGIC, f"魔数错误: {magic!r}")
    check(version == 1, f"版本错误: {version}")
    check(header_size == 256, f"header_size 错误: {header_size}")
    check(total == len(raw), f"total_size {total} != 文件大小 {len(raw)}")

    # Footer 自身哈希（footer_sha 位于偏移 224..240）
    body = bytearray(raw[-256:])
    body[224:240] = b"\x00" * 16
    check(hashlib.sha256(body).digest()[:16] == f_sha, "Footer 哈希不匹配")

    # 配置
    cfg = raw[c_off:c_off + c_size].split(b"\x00")
    cfg = [x for x in cfg if x]
    check(len(cfg) == cfg_count, f"配置项数量不符: {len(cfg)} != {cfg_count}")
    kv = {}
    for item in cfg:
        k, _, v = item.decode("utf-8", "replace").partition("=")
        kv.setdefault(k, []).append(v)

    print(f"包        : {path}")
    print(f"大小      : {len(raw)} 字节")
    print(f"版本      : {version}, build {build_id.hex()}")
    print(f"配置      : name={kv.get('name')} mode={kv.get('mode')} "
          f"path={kv.get('pathmode')} cleanup={kv.get('cleanup')}")
    if kv.get("env"):
        print(f"            env={kv['env']}")

    blobs = [
        ("bash", b_off, b_st, b_raw, b_sha, flags & FLAG_LZ4[0]),
        ("busybox", bb_off, bb_st, bb_raw, bb_sha, flags & FLAG_LZ4[1]),
        ("script", s_off, s_st, s_raw, s_sha, flags & FLAG_LZ4[2]),
    ]
    for name, off, st, rawsz, sha, lz4 in blobs:
        check(off + st <= len(raw) - 256, f"{name} 偏移越界")
        check(st == len(raw[off:off + st]), f"{name} 长度不符")
        data = raw[off:off + st]
        tag = "LZ4" if lz4 else "raw"
        line = f"  {name:<8} {tag:<3} off={off:<10} stored={st:<9} raw={rawsz}"
        if lz4:
            check(rawsz >= st, f"{name} 压缩后变大")
        else:
            check(hashlib.sha256(data).digest() == sha, f"{name} SHA-256 不匹配")
            m = elf_machine(data)
            if name in ("bash", "busybox"):
                check(m is not None, f"{name} 不是 ELF")
                if m is not None:
                    line += f" ELF={NAMES.get(m, m)}"
                    want = {"x86_64": 62, "aarch64": 183, "armv7": 40, "i386": 3}.get(expect)
                    if want is not None:
                        check(m == want, f"{name} 架构不是 {expect}: {m}")
        print(line)

    if errors:
        print("\nFAIL:")
        for e in errors:
            print("  -", e)
        return 1
    print("\nOK: 包结构、偏移、配置、Footer 校验全部通过")
    return 0


if __name__ == "__main__":
    sys.exit(main())
