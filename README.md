# shpack

**把 shell 脚本打包成「自带静态 bash + busybox」的单个 ELF，默认完全在内存中执行。**

[![platform](https://img.shields.io/badge/platform-linux%20%7C%20android-blue)](#)
[![arch](https://img.shields.io/badge/arch-x86__64%20%7C%20aarch64-informational)](#)
[![license](https://img.shields.io/badge/license-MIT-green)](LICENSE)

`shpack` 把一份 `.sh` 脚本、一份**纯静态** `bash`、一份**纯静态** `busybox`
封装成一个静态 ELF。目标机器不需要 bash、coreutils、`/bin/sh` 或任何动态库，
`chmod +x` 就能跑；默认把三个组件放进匿名内存文件，**不落盘**。

主要目标平台是 **Android arm64-v8a**，同时支持 x86_64 / aarch64 Linux
（Alpine、精简容器、CI 镜像、老系统等）。

```bash
$ ./deploy --help
$ adb push deploy /data/local/tmp/ && adb shell chmod 755 /data/local/tmp/deploy
$ adb shell /data/local/tmp/deploy --dry-run     # 全程不往磁盘写 bash/busybox
```

---

## 先澄清一个概念

shell 脚本不会被“编译成机器码”。`shpack` 做的是**自包含封装**：产物是一个
真正的静态 ELF，运行时由内核直接加载；bash、busybox、脚本作为 payload 存在
文件尾部，启动时解出并执行。

```
┌────────────────────────────────────┐
│ 静态 C++ stub（0.7~0.8 MB）        │ ← 内核执行的 ELF，无 PT_INTERP
├────────────────────────────────────┤
│ bash      （静态，LZ4 可选）       │
├────────────────────────────────────┤
│ busybox   （静态，LZ4 可选）       │
├────────────────────────────────────┤
│ 你的脚本 + 内存模式 preamble       │
├────────────────────────────────────┤
│ 配置表 + 256 字节 Footer           │
└────────────────────────────────────┘
```

脚本仍然由 bash 解释执行，行为与直接 `bash script.sh` 基本一致（见 §7 差异）。

---

## 1. 两种运行模式

| | `memfd`（默认 / auto 首选） | `dir`（兼容回退） |
| --- | --- | --- |
| bash / busybox / 脚本 | 匿名内存文件，`/proc/self/fd/N` | 私有临时目录 |
| 是否落盘 | **否**（memfd 或 `/dev/shm` 匿名文件） | 是 |
| busybox applet | 以 **bash 函数** 提供 | `PATH` 中的 270+ 个软链 |
| 子进程里的 applet | 子 bash 通过 `command_not_found_handle` 回落 | 所有进程都能用 |
| 兼容性 | 需要 `memfd_create`（Linux 3.17+/Android 8+）或 `/dev/shm` | 几乎任何 Linux |
| 清理 | 无残留，fd 关闭即释放 | 退出时 `rm -rf` |

`auto`（默认）先尝试纯内存，失败自动回退 `dir`；`--mode memfd` 强制纯内存，
`--mode dir` 强制落盘模式。

### 纯内存是怎么做到的

1. `bash`、`busybox`、脚本分别写入 `memfd_create` 的匿名文件（或 `/dev/shm`
   上创建后立刻 `unlink` 的匿名文件），只保留文件描述符；
2. `bash` 通过 `/proc/self/fd/N` 执行，脚本通过 `/proc/self/fd/M` 读取；
3. busybox 没有可放的目录，于是用一个启动 preamble 把 270+ 个 applet 定义成
   shell 函数：`ls() { ( exec -a busybox "$SHPACK_BUSYBOX_FD" "ls" "$@" ); }`；
   `exec -a` 是必需的——busybox 靠 `argv[0]` 识别 applet，而
   `/proc/self/fd/5` 的 basename 是 `5`；
4. 导出一个 `command_not_found_handle` 和 applet 列表，让**子 bash** 在系统
   `PATH` 里找不到命令时也能回落到 busybox；
5. 没有临时目录、没有符号链接，退出后没有任何残留。

> `/dev/shm` 回退是为了兼容没有 `memfd_create` 的内核（例如 WSL1），
> 它同样在 tmpfs 内存里，创建后立即 `unlink`，不会留下目录项。
> 该路径需要 `/dev/shm` 可执行；否则预检失败并回退 `dir`。

---

## 2. 快速开始

### 2.1 直接用仓库里的预编译组件（无需编译器）

仓库自带 `prebuilt/`：静态 bash/busybox、host packer、两个架构的 stub，全都有。
**克隆后可以直接打包**，不需要装交叉工具链，甚至不需要 `make`：

```bash
git clone https://github.com/<you>/shpack.git
cd shpack

./pack.sh -s examples/hello.sh -o hello --compress lz4
./pack.sh --arch aarch64 -s examples/hello.sh -o hello-arm64 --compress lz4

./hello --version
```

只有修改了 `src/` 才需要重新编译：

```bash
make                       # 本机 x86_64
make ARCH=aarch64          # arm64（需要 aarch64-linux-gnu-g++）
```

`dist/` 里还有已经打好的 `hello` / `hello-arm64`，可以直接
`adb push dist/hello-arm64 /data/local/tmp/`（仅供快速验证）。

> `prebuilt/host/shpack` 是 x86_64 程序。如果你在 arm64 Linux 上打包，
> 请先运行 `make` 构建本机 packer；目标 stub 和 bash/busybox 仍可复用。

### 2.2 从源码构建静态 bash/busybox

```bash
# 方式 A：从 Ubuntu 仓库提取（快，glibc 静态）
scripts/fetch-static.sh --arch x86_64  --out runtime/x86_64
scripts/fetch-static.sh --arch aarch64 --out runtime/aarch64

# 方式 B：从源码构建（musl，体积更小，Android 推荐）
scripts/get-toolchain.sh aarch64          # musl.cc 工具链，约 108 MB
scripts/build-runtime.sh --arch aarch64   # 产物在 runtime/aarch64/
```

`pack.sh` 会优先使用 `runtime/<arch>/`，没有时回退到 `prebuilt/<arch>/`。

### 2.3 Android arm64-v8a

```bash
./build.sh --arch aarch64                 # 交叉编译 stub
./pack.sh --arch aarch64 -s your.sh -o your-app --compress lz4

adb push your-app /data/local/tmp/
adb shell chmod 755 /data/local/tmp/your-app
adb shell /data/local/tmp/your-app --help
adb shell /data/local/tmp/your-app -- --dry-run
```

Termux 里直接 `./your-app` 即可（`$TMPDIR` 可执行）。更多细节见
[docs/ANDROID.md](docs/ANDROID.md)。

---

## 3. 构建

```bash
make                            # build/host/shpack + build/<arch>/shpack-stub
make ARCH=aarch64               # 交叉构建 arm64 stub
make ARCH=aarch64 CROSS_COMPILE=/opt/musl/bin/aarch64-linux-musl-
make clean
```

* **packer** 永远用宿主编译器构建（只在构建机运行）；
* **stub** 用目标架构编译器构建，强制 `-static`，无 `PT_INTERP`；
* `build.sh` 是包装脚本：`./build.sh --arch aarch64 --with-runtime`

---

## 4. 打包

```bash
./pack.sh -s script.sh -o app [选项]
```

| 参数 | 说明 |
| --- | --- |
| `-s, --script` | 脚本路径 |
| `-o, --output` | 输出 ELF（默认取脚本名） |
| `-n, --name` | 包名 / 运行时脚本文件名 |
| `-a, --arch` | `x86_64` / `aarch64`，决定 stub 与 prebuilt |
| `--stub/--bash/--busybox` | 显式指定组件 |
| `--compress auto\|none\|lz4` | 默认 auto（有收益才压缩） |
| `--mode auto\|memfd\|dir` | 运行时默认策略，默认 auto |
| `--path-mode busybox\|system` | dir 模式下 PATH 顺序 |
| `--env K=V` | 注入环境变量，可重复 |
| `--bash-opt OPT` | 传给 bash 的选项，可重复 |
| `--no-cleanup` | dir 模式保留临时目录 |

示例：

```bash
./pack.sh -s deploy.sh -o deploy \
  --name deploy --env DEPLOY_ENV=prod --compress lz4
```

---

## 5. 运行时

```bash
./app [shpack 选项] [--] [脚本参数...]
```

| 选项 | 说明 |
| --- | --- |
| `-h, --help` | shpack 帮助 |
| `-V, --version` | 版本 + build id |
| `--list` | 包内组件、压缩、大小 |
| `--extract DIR` | 释放 bash/busybox/脚本后退出（调试用） |
| `--keep` | 保留临时目录（仅 dir） |
| `--mode auto\|memfd\|dir` | 覆盖默认策略 |
| `--path busybox\|system` | dir 模式 PATH 顺序 |
| `--no-preflight` | 跳过执行预检 |
| `-v, --verbose` | 输出策略、目录、分段耗时 |
| `--` | 之后全部传给脚本 |

参数边界：遇到第一个非选项参数或 `--` 停止解析。

```bash
./app --help        # shpack 的帮助
./app -- --help     # 脚本自己的 --help
./app foo --help    # foo 之后属于脚本
```

### 环境变量

| 变量 | 含义 |
| --- | --- |
| `SHPACK_SELF` | 产物自身路径 |
| `SHPACK_DIR` | dir 模式解包目录；内存模式为空 |
| `SHPACK_BASH` | 内置 bash 路径（`/proc/self/fd/N` 或临时文件） |
| `SHPACK_BUSYBOX` | 可直接当命令用：dir 模式是路径，内存模式是 `busybox` |
| `SHPACK_BUSYBOX_FD` | busybox 的原始路径（`/proc/self/fd/N`），用于 `exec -a` 等场景 |
| `SHPACK_SCRIPT` | 脚本路径 |
| `SHPACK_MODE` | `memfd` / `dir` |
| `SHPACK_NAME` / `SHPACK_VERSION` | 包名 / 版本 |
| `SHPACK_ORIG_PATH` | 运行前 PATH |
| `SHPACK_TMPDIR` | dir 模式优先使用的目录 |

---

## 6. 退出码与信号

* 脚本退出码原样返回；
* `dir` 模式下 stub 等待子进程，`SIGTERM/SIGHUP` 转发给 bash，退出后清理；
* 内存模式直接 `execve` bash，信号/终端行为与普通脚本一致；
* stub 自身错误码：

| 退出码 | 含义 |
| --- | --- |
| 100 | 不是 shpack 产物 / Footer 损坏 / 文件被截断 |
| 101 | 负载 SHA-256 校验失败 |
| 102 | 无法准备运行时（内存与目录模式都失败） |
| 125 | fork 失败 |
| 127 | exec 内置 bash 失败 |

---

## 7. 限制（内存模式）

* applet 是 **bash 函数**，只对 bash 生效。`sh -c '...'`、`xargs` 直接
  `exec` 出来的子进程看不到这些函数；如果系统 `PATH` 里没有对应命令，请
  显式使用 `"$SHPACK_BUSYBOX" applet ...`；
* 子 bash（`bash -c`）通过导出的 `command_not_found_handle` 回落，但只在
  系统 `PATH` 找不到命令时触发；
* 依赖 `memfd_create` 或 `/dev/shm` 可执行；Android 上若被 SELinux 拒绝，
  `auto` 会回退 `dir`，也可以用 `--mode dir` 明确选择；
* 脚本里 `$0` 是 `/proc/self/fd/N`（内存模式）或临时路径（dir 模式），
  不要用 `$0` 定位同目录资源。

---

## 8. 包格式（v1）

所有整数小端序，Footer 固定 256 字节，位于文件末尾。

| 偏移 | 大小 | 字段 |
| --- | --- | --- |
| 0 | 8 | magic `SHPACK01` |
| 8 | 4 | version = 1 |
| 12 | 4 | header_size = 256 |
| 16 | 4 | flags（bit0/1/2 = 三个负载使用 LZ4） |
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
[`tests/verify_package.py`](tests/verify_package.py) 是不依赖 C++ 的独立解析器。

---

## 9. 测试

```bash
bash tests/run-tests.sh        # 81 项（x86_64）
python3 tests/verify_package.py dist/hello-arm64 --expect-arch aarch64
```

覆盖：ELF 静态属性、默认内存模式、dir 回退、无落盘、applet 函数、
子 bash、参数/退出码、stdin、`env -i`、TMPDIR、`--keep/--extract`、
损坏/截断检测、打包器静态校验、压缩、只读 cwd、并发、信号清理、
带空格路径、符号链接、大脚本、`bash -n` 等。

已验证的环境：

| 项目 | 结果 |
| --- | --- |
| x86_64 全量测试 | 81/81 通过 |
| 纯内存执行（memfd/shm 回退） | 通过（本机 WSL1 走 `/dev/shm` 匿名文件） |
| aarch64 产物结构 | 独立校验器通过（ELF=aarch64，哈希/偏移正确） |
| aarch64 源码构建 | `build-runtime.sh` 用交叉编译器跑通（静态 bash 2.0 MB + busybox 2.1 MB） |
| aarch64 真机执行 | **未验证**（无设备；WSL1 的 qemu-user 无法映射 aarch64 ET_EXEC） |

如果你有 arm64 设备，请运行：

```bash
adb push tests/android-smoke.sh /data/local/tmp/
adb push dist/hello-arm64 /data/local/tmp/hello
adb shell sh /data/local/tmp/android-smoke.sh /data/local/tmp/hello
```

---

## 10. 安全与许可

* Footer 有自身哈希，三个负载各有 SHA-256，运行时全部校验；这不是数字签名，
  需要可信分发请自行附 `sha256sum` / minisign；
* 不要 setuid；“自解压 + 执行”做成 setuid 是危险设计，shpack 不支持；
* 内存模式创建匿名文件并在使用后关闭，不留下路径；
* 本仓库代码 MIT；`prebuilt/` 与 `dist/` 中的 bash（GPLv3）、busybox（GPLv2）
  许可见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) 与
  [`prebuilt/licenses/`](prebuilt/licenses/)。

---

## 11. 上传到 GitHub

仓库已经按“开箱即用”整理：`prebuilt/`（约 11 MB，含 packer 和双架构 stub）
和 `dist/`（约 7 MB）可以直接提交，用户克隆后无需工具链即可打包。

```bash
git init
git add .
git commit -m "shpack: static self-contained shell script packer"
git branch -M main
git remote add origin git@github.com:<you>/shpack.git
git push -u origin main

# 发版：CI 会构建并附上产物
git tag v1.0.0
git push origin v1.0.0
```

内置的 GitHub Actions：

* `.github/workflows/build.yml`：push/PR 时构建 x86_64 + aarch64，
  下载运行时并跑完整测试，上传 `dist/` 作为 artifact；
* `.github/workflows/release.yml`：推送 `v*` tag 时创建 Release，
  自动附带 `dist/hello`、`dist/hello-arm64` 和校验和。

---

## 12. 目录结构

```
shpack/
├── src/                  C++ 实现（packer + 静态 stub）
│   ├── format.h          Footer / 配置格式
│   ├── launcher.cpp      运行时（memfd / dir / 预检 / 信号）
│   ├── packer.cpp        打包器
│   ├── lz4.cpp/.h        自实现 LZ4（零依赖）
│   ├── sha256.cpp/.h     SHA-256
│   └── util.cpp/.h       文件、ELF 静态检查
├── scripts/
│   ├── fetch-static.sh   从 Ubuntu 仓库取静态 bash/busybox
│   ├── build-runtime.sh  从源码构建 musl 静态 bash/busybox
│   └── get-toolchain.sh  下载 musl.cc 交叉工具链
├── prebuilt/             packer + 双架构 stub + 静态 bash/busybox + 许可证
├── dist/                 已打包示例 + SHA256SUMS
├── tests/                81 项端到端测试、独立格式校验、Android 冒烟脚本
├── examples/
├── docs/ANDROID.md
├── .github/workflows/   CI（构建/测试/Release）
├── Makefile / build.sh / pack.sh
├── THIRD_PARTY_NOTICES.md
├── LICENSE
└── README.md
```
