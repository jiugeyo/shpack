#!/usr/bin/env python3
"""让静态 glibc 程序在 Android Termux（untrusted_app seccomp）下运行。

Android 的 app seccomp 策略会对 set_robust_list / rseq / set*id 返回 SIGSYS，
而 glibc 静态启动和 busybox 降权路径会调用它们。这里把对应 svc 指令改成：
  * set_robust_list / rseq：nop，让 glibc 走“内核不支持”分支；
  * setgid/setuid 等：mov x0,#0，假装成功（进程本来就没有特权）。

    scripts/patch-android.py FILE...
"""
import struct
import sys
from pathlib import Path

PT_LOAD = 1
PF_X = 1

# syscall number -> (mov x8, #N 编码, 替换 svc 的指令)
AARCH64 = {
    99:  (bytes.fromhex("680c80d2"), bytes.fromhex("1f2003d5")),  # set_robust_list
    293: (bytes.fromhex("a82480d2"), bytes.fromhex("1f2003d5")),  # rseq
    143: (bytes.fromhex("e81180d2"), bytes.fromhex("000080d2")),  # setregid
    144: (bytes.fromhex("081280d2"), bytes.fromhex("000080d2")),  # setgid
    145: (bytes.fromhex("281280d2"), bytes.fromhex("000080d2")),  # setreuid
    146: (bytes.fromhex("481280d2"), bytes.fromhex("000080d2")),  # setuid
    147: (bytes.fromhex("681280d2"), bytes.fromhex("000080d2")),  # setresuid
    149: (bytes.fromhex("a81280d2"), bytes.fromhex("000080d2")),  # setresgid
    159: (bytes.fromhex("e81380d2"), bytes.fromhex("000080d2")),  # setgroups
}
SVC = bytes.fromhex("010000d4")

X86_64 = {
    273: (bytes.fromhex("b811010000"), b"\x90\x90"),  # set_robust_list
    334: (bytes.fromhex("b84e010000"), b"\x90\x90"),  # rseq
    106: (bytes.fromhex("b86a000000"), b"\x31\xc0"),  # setgid -> xor eax,eax
    105: (bytes.fromhex("b869000000"), b"\x31\xc0"),  # setuid
}
SYSCALL = bytes.fromhex("0f05")


def elf_segments(data):
    if data[:4] != b"\x7fELF" or data[4] != 2 or data[5] != 1:
        return None
    e_machine = struct.unpack_from("<H", data, 18)[0]
    phoff = struct.unpack_from("<Q", data, 32)[0]
    phentsize = struct.unpack_from("<H", data, 54)[0]
    phnum = struct.unpack_from("<H", data, 56)[0]
    segs = []
    for i in range(phnum):
        off = phoff + i * phentsize
        p_type, p_flags = struct.unpack_from("<II", data, off)
        p_offset, p_vaddr, _, p_filesz = struct.unpack_from("<QQQQ", data, off + 8)
        if p_type == PT_LOAD and (p_flags & PF_X):
            segs.append((p_offset, p_filesz, p_vaddr))
    return e_machine, segs


def patch_aarch64(data, segs):
    changed = 0
    for seg_off, seg_len, _ in segs:
        end = min(seg_off + seg_len, len(data))
        for i in range(seg_off, end - 4, 4):
            w = data[i:i + 4]
            for sysno, (mov, repl) in AARCH64.items():
                if w != mov:
                    continue
                for j in range(i + 4, min(i + 4 * 64, end), 4):
                    if data[j:j + 4] == SVC:
                        data[j:j + 4] = repl
                        changed += 1
                        print(f"  aarch64 syscall {sysno}: patch @ {j:#x}")
                        break
    return changed


def patch_x86_64(data, segs):
    changed = 0
    for seg_off, seg_len, _ in segs:
        end = min(seg_off + seg_len, len(data))
        for i in range(seg_off, end - 5, 1):
            w = data[i:i + 5]
            for sysno, (mov, repl) in X86_64.items():
                if w != mov:
                    continue
                for j in range(i + 5, min(i + 64, end), 1):
                    if data[j:j + 2] == SYSCALL:
                        data[j:j + 2] = repl
                        changed += 1
                        print(f"  x86_64 syscall {sysno}: patch @ {j:#x}")
                        break
    return changed


def main(paths):
    if not paths:
        print(__doc__, file=sys.stderr)
        return 2
    total = 0
    for name in paths:
        p = Path(name)
        data = bytearray(p.read_bytes())
        info = elf_segments(data)
        if info is None:
            print(f"{name}: 不是 64 位 ELF，跳过", file=sys.stderr)
            continue
        machine, segs = info
        if machine == 183:
            n = patch_aarch64(data, segs)
        elif machine == 62:
            n = patch_x86_64(data, segs)
        else:
            print(f"{name}: 架构 {machine} 暂不支持，跳过", file=sys.stderr)
            continue
        if n:
            p.write_bytes(data)
        total += n
        print(f"{name}: {n} 处")
    return 0 if total else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
