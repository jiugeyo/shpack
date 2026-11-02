# shpack

**一个静态 ELF，内置 4 种架构的 bash/busybox：运行它，交互式把 shell 脚本打成自包含可执行文件。**

[![platform](https://img.shields.io/badge/platform-linux%20%7C%20android-blue)](#)
[![arch](https://img.shields.io/badge/target-x86__64%20%7C%20aarch64%20%7C%20armv7%20%7C%20i386-informational)](#)
[![license](https://img.shields.io/badge/license-MIT-green)](LICENSE)

`dist/shpack-builder` 是单文件、纯静态、无依赖的打包器（x86_64 主机，约 15 MB）。
把它拷到任意 Linux / WSL / Android(Termux) 上执行，它会在终端里问几个问题，
然后产出一个目标架构的**自包含 ELF**——目标机器不需要 bash、coreutils、动态库，
默认运行时也不往磁盘写任何东西。

```bash
$ ./shpack-builder
shpack-builder 交互式打包
脚本路径: deploy.sh
目标架构
  1) x86_64   2) aarch64   3) armv7   4) i386
选择 [2]: 2
...
✔ 已生成 deploy (3.4 MiB)
```

不需要 `git clone`，不需要交叉编译器，不需要 root。

---

## 1. 支持矩阵

| 目标架构 | 典型设备 | 运行时 bash/busybox | stub |
| --- | --- | --- | --- |
| `x86_64` | PC / 服务器 / WSL / x86 模拟器 | glibc 静态 | 静态 |
| `aarch64` | **Android arm64-v8a** / ARM64 Linux / Termux | glibc 静态 | 静态 |
| `armv7` | Android armeabi-v7a / 32 位 ARM Linux | glibc 静态（armhf） | 静态 |
| `i386` | 32 位 x86 Linux / x86 Android 模拟器 | glibc 静态 | 静态 |

主机侧 builder：

| 文件 | 主机 |
| --- | --- |
| `dist/shpack-builder` | x86_64 Linux / WSL |
| `dist/shpack-builder-aarch64` | aarch64 Linux / Android Termux |

> builder 本身是 ELF，不能直接在 Windows/macOS 上运行；Windows 用 WSL，
> macOS 用 Linux 容器。产物只面向 Linux 内核（Android 是 Linux）。

---

## 2. 快速开始

```bash
# 下载仓库后（或者只拿 dist/ 里的一个文件）
chmod +x dist/shpack-builder
./dist/shpack-builder

# 或者直接 CLI
./dist/shpack-builder -s deploy.sh -o deploy -a aarch64 --compress lz4

# 推到手机
adb push deploy /data/local/tmp/
adb shell chmod 755 /data/local/tmp/deploy
adb shell /data/local/tmp/deploy --help
```

CLI 常用参数：

```bash
shpack-builder -s SCRIPT -o OUT -a ARCH [选项]

  -a, --arch      x86_64 | aarch64 | armv7 | i386
  -n, --name      包名 / 运行时脚本文件名
  --compress      auto | lz4 | none | tar.zst
  --mode          auto | memfd | dir
  --path-mode     busybox | system
  --env K=V       注入环境变量（可重复）
  --bash-opt OPT  传给 bash 的选项（可重复）
  --no-cleanup    dir 模式保留临时目录
  --bundle PATH   使用外部 bundle（开发调试）
```

辅助命令：

```bash
./shpack-builder list                  # 列出内嵌架构与组件
./shpack-builder info                  # builder 版本 / bundle build id / 组件
./shpack-builder extract --arch aarch64 --out runtime
./shpack-builder --bundle build/shpack.bundle list
```

### 2.1 输出默认值

交互式向导里：

* **输出文件默认 = 输入脚本的文件名（含扩展名）**，例如 `deploy.sh`；
* **输出权限 = 输入脚本的权限**（`stat -c %a`），不会自动补 `+x`；
  脚本本身没有执行位时，输出会提示手动 `chmod +x`；
* 输出路径如果和输入脚本相同，会拒绝并让你重新输入，避免覆盖源码。

### 2.2 tar.zst 极限归档

压缩菜单第 4 项（或 `--compress tar.zst`）会：

1. 正常生成可执行 ELF（可执行位仍跟随脚本）；
2. 额外生成 `<output>.tar.zst`：tar 内放**未压缩 ELF**，再用 zstd
   `-22 --ultra --long=31` 极限压缩，得到最小上传体积。

实测 `hello`：

```
可执行 ELF     3.6 MB
tar.zst        1.7 MB
```

解压：`tar --zstd -xf app.tar.zst`（需要系统有 zstd）。这是**分发归档**，
解压出的 ELF 才能直接运行。builder 静态链接了 libzstd，不依赖外部 `zstd` 命令。

---

## 3. 产物怎么运行

目标 ELF = 静态 stub + 内置 bash + busybox + 你的脚本 + 配置 + Footer。

* `--mode auto`（默认）：优先 **memfd 纯内存**；内核不支持/SELinux 拒绝时回退临时目录；
* `--mode memfd`：强制纯内存（失败即报错，不落盘）；
* `--mode dir`：解包到私有临时目录，busybox applet 通过 PATH 软链提供，兼容性最好。

纯内存模式把 bash、busybox、脚本写入匿名文件（`memfd_create`，或 `/dev/shm`
创建后立即 `unlink`），用 `/proc/self/fd/N` 执行；270+ 个 applet 以 bash 函数
提供，`command_not_found_handle` 负责子 bash 的回落。退出后无残留。

| | memfd（默认） | dir |
| --- | --- | --- |
| 落盘 | 否 | 是 |
| applet 可见范围 | 当前 bash + 子 bash 回落 | 所有子进程 |
| 兼容性 | Linux 3.17+/Android 8+ | 几乎任何 Linux |

---

## 4. 单文件是怎么装下 4 个架构的

builder 的 ELF 尾部追加了一个 bundle：

```
[静态 builder 代码][bundle 数据][bundle 索引][128B Footer]
```

bundle 内含每个架构的 `stub` / `bash` / `busybox`，各自 LZ4 压缩并带 SHA-256：

```
$ ./shpack-builder list
builder bundle build: 548016ac19af53a7
内嵌架构 (4):
  x86_64    stub 814 KiB  bash 2.4 MiB  busybox 2.0 MiB
  aarch64   stub 712 KiB  bash 2.1 MiB  busybox 1.8 MiB
  armv7     stub 409 KiB  bash 1.3 MiB  busybox 1.2 MiB
  i386      stub 769 KiB  bash 2.5 MiB  busybox 2.0 MiB
```

运行 `pack` 时按目标架构取出对应三个组件，解压、校验、重新组装成目标 ELF。
builder 自身大小约 14~15 MB，拷贝一次即可反复使用。

bundle 格式（v1）：

| 字段 | 说明 |
| --- | --- |
| magic | `SHPKBLD1` |
| entry | arch[16] + kind[12] + flags + machine + raw/stored size + offset + SHA-256 |
| footer | magic/version/count/total/index 偏移/build id/自身哈希 |

`tests/verify_package.py` 可独立校验产物；`shpack-builder extract` 可把组件取出来。

---

## 5. Android

```bash
# PC/WSL 上打包
./dist/shpack-builder -s your.sh -o your-app -a aarch64

# 手机
adb push your-app /data/local/tmp/
adb shell chmod 755 /data/local/tmp/your-app
adb shell /data/local/tmp/your-app
```

* **arm64-v8a**：用 `-a aarch64`；**armeabi-v7a**：用 `-a armv7`；
* Termux 里可直接运行 `dist/shpack-builder-aarch64`，不需要电脑；
* App 内调用建议把产物放进 `jniLibs/arm64-v8a/libxxx.so`，由系统解压到
  `nativeLibraryDir` 后执行；
* 内存模式依赖 `memfd_create`（Android 8+）；更老设备或 SELinux 拒绝时
  `auto` 会回退 dir，`--mode memfd` 则明确失败。

详见 [docs/ANDROID.md](docs/ANDROID.md)。

---

## 6. 自己构建

依赖：`g++`、`make`、`libzstd-dev`（builder 的 tar.zst 归档），以及可选的交叉编译器。

```bash
make                                   # 构建 host packer/builder/bundler + 本机 stub
make ARCH=aarch64                      # 需要 aarch64-linux-gnu-g++
bash scripts/fetch-cross.sh --arch armv7   # 无 root 拿 armv7 交叉工具链
bash scripts/fetch-cross.sh --arch i386
make ARCH=armv7
make ARCH=i386

# 生成完整单文件 builder（4 架构）
scripts/make-builder.sh --arches "x86_64 aarch64 armv7 i386" --aarch64-host
```

`make-builder.sh` 的组件查找顺序：`build/<arch>` → `prebuilt/stub/<arch>`；
bash/busybox：`runtime/<arch>` → `prebuilt/<arch>`。因此**没有交叉编译器也能
打完整 builder**，仓库里的 `prebuilt/` 已经带了 4 架构组件和 stub。

运行时组件来源：

```bash
scripts/fetch-static.sh --arch x86_64  --out runtime/x86_64
scripts/fetch-static.sh --arch aarch64 --out runtime/aarch64
scripts/fetch-static.sh --arch armv7   --out runtime/armv7
scripts/fetch-static.sh --arch i386    --out runtime/i386
```

或从源码构建 musl 版（体积更小）：

```bash
scripts/get-toolchain.sh aarch64
scripts/build-runtime.sh --arch aarch64
```

---

## 7. 包格式（产物）

Footer 固定 256 字节，位于文件末尾；所有整数小端序。

| 偏移 | 大小 | 字段 |
| --- | --- | --- |
| 0 | 8 | magic `SHPACK01` |
| 8 | 4 | version = 1 |
| 12 | 4 | header_size = 256 |
| 16 | 4 | flags（bit0/1/2 = bash/busybox/script 使用 LZ4） |
| 20 | 4 | cfg_count |
| 24 | 8 | total_size |
| 32 | 8×3 | bash off / stored / raw |
| 56 | 8×3 | busybox off / stored / raw |
| 80 | 8×3 | script off / stored / raw |
| 104 | 8×2 | cfg off / size |
| 120 | 32×3 | 三个负载原始内容的 SHA-256 |
| 216 | 8 | build_id |
| 224 | 16 | Footer 自身哈希前 16 字节 |
| 240 | 16 | reserved |

配置表是 NUL 分隔的 `key=value`：`name`、`mode`、`pathmode`、`cleanup`、
`argv0`、`version`、可重复的 `env` / `bashopt`。

产物运行时错误码：100（包损坏）、101（SHA-256 失败）、102（无法准备运行时）、
125（fork）、127（exec bash）。

---

## 8. 测试

```bash
bash tests/run-tests.sh        # 82 项：产物运行/编码/模式/异常
bash tests/builder-tests.sh    # 22 项：4 架构打包 + 交互式 + extract + tar.zst
python3 tests/verify_package.py dist/... --expect-arch aarch64
```

已验证：

| 项目 | 结果 |
| --- | --- |
| x86_64 产物 | 82/82 测试通过；真实执行 |
| 单文件 builder | 4 架构打包 + 交互向导 + extract + tar.zst，22/22 通过 |
| aarch64/armv7/i386 产物 | 独立校验器通过（ELF class/machine/哈希/偏移正确） |
| aarch64 主机 builder | 交叉编译成功（静态 ELF） |
| armv7/i386 动态执行 | **未验证**：WSL1 不支持 32 位 ELF，qemu-user 也无法在 WSL1 定点映射 |
| aarch64 动态执行 | **未验证**：同上，无真机 |

> 有设备时请跑 `tests/android-smoke.sh`。

---

## 9. 仓库结构

```
shpack/
├── src/
│   ├── builder.cpp       单文件交互式 builder
│   ├── bundler.cpp       生成多架构 bundle
│   ├── bundle.cpp/.h     bundle 读写/校验
│   ├── package.cpp/.h    打包核心（packer 与 builder 共用）
│   ├── packer.cpp        文件版 packer
│   ├── launcher.cpp      目标产物运行时
│   └── lz4/sha256/util
├── prebuilt/             4 架构 bash/busybox + stub + host packer + 许可证
├── dist/                 shpack-builder（x86_64 / aarch64）
├── scripts/
│   ├── make-builder.sh   组装单文件 builder
│   ├── fetch-static.sh   下载官方静态 bash/busybox
│   ├── fetch-cross.sh    无 root 组装 armv7/i386 交叉工具链
│   ├── fetch-zstd.sh     获取 aarch64 静态 libzstd
│   └── build-runtime.sh  从源码构建 musl 运行时
├── tests/
│   ├── run-tests.sh      82 项产物测试
│   ├── builder-tests.sh  22 项 builder 测试
│   ├── verify_package.py 独立格式校验
│   └── android-smoke.sh  手机冒烟
├── examples/
├── docs/ANDROID.md
├── .github/workflows/    CI / Release
├── Makefile / build.sh / pack.sh / fix-perms.sh
├── THIRD_PARTY_NOTICES.md
└── LICENSE
```

---

## 10. 上传 GitHub 与发布

```bash
git init
git add .
# 关键：网页上传会丢可执行位，必须显式记录 mode
git add --chmod=+x build.sh pack.sh fix-perms.sh scripts/*.sh tests/*.sh \
  tests/scripts/*.sh examples/*.sh prebuilt/host/shpack \
  prebuilt/stub/*/shpack-stub dist/shpack-builder dist/shpack-builder-aarch64
git commit -m "shpack: single-file multi-arch shell packer"
git branch -M main
git remote add origin git@github.com:<you>/shpack.git
git push -u origin main

git tag v2.0.0 && git push origin v2.0.0    # 触发 Release
```

CI（`.github/workflows/build.yml`）会：

1. `make` 构建 host 工具与 x86_64 stub；
2. `scripts/make-builder.sh` 用 prebuilt 组装 4 架构 builder；
3. 跑 82 项产物测试 + 22 项 builder 测试；
4. 上传 `dist/`。

Release（`release.yml`）额外交叉编译 `shpack-builder-aarch64` 并附到 Release。

如果克隆后提示 `Permission denied`：

```bash
bash fix-perms.sh
git add --chmod=+x ... && git commit -m "fix: restore executable bits" && git push
```

---

## 11. 安全与许可

* 产物 Footer + 三个负载都有 SHA-256，运行时校验；不是签名，可信分发请另附校验和；
* 不要 setuid；自解压 + 执行做成 setuid 是危险设计；
* 本仓库代码 MIT；`prebuilt/`、`dist/` 内含的 GNU bash（GPLv3）与 BusyBox（GPLv2）
  许可见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) 和
  [`prebuilt/licenses/`](prebuilt/licenses/)。

---

## 12. 限制

* builder 是 ELF：Windows 需 WSL，macOS 需 Linux 容器；
* 目标产物只支持 Linux 内核（Android 可以）；
* 纯内存模式的 applet 是 bash 函数，`sh -c` / `xargs` 等非 bash 子进程看不到；
  需要完整 PATH 语义时用 `--mode dir`；
* `tar.zst` 是**额外归档**，解压需要 `zstd`；归档内是未压缩 ELF，适合上传分发；
* armv7/i386 产物未在真实 32 位环境执行验证，仅做了 ELF/格式校验；
* 极老内核（< 3.7）建议改用 musl 版本重新打包。
