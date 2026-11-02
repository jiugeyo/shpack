# Android 使用指南（arm64-v8a）

`shpack` 的 aarch64 产物是一个**纯静态 ELF**，不依赖 Android 的 `linker64`、
bionic libc、`/system/bin/sh` 或 toybox。默认情况下，bash、busybox 和脚本
都放进**匿名内存文件**执行，不会往 `/data`、`/sdcard` 写任何东西。

## 1. ABI 与系统版本

| Android ABI | 架构 | 构建方式 |
| --- | --- | --- |
| `arm64-v8a` | aarch64 | `./build.sh --arch aarch64` |
| `armeabi-v7a` | armv7（32 位） | `./build.sh --arch armv7` + armv7 静态 bash/busybox |
| `x86_64`（模拟器） | x86_64 | `./build.sh --arch x86_64` |

纯内存模式依赖 `memfd_create`（Linux 3.17+，Android 8 起普遍可用）。
更老的设备会自动回退到临时目录模式，功能不受影响，只是会落盘。

## 2. 部署位置：哪里可以执行？

| 位置 | 可执行 | 说明 |
| --- | --- | --- |
| `/data/local/tmp` | ✅（adb shell 域） | **最推荐**，内存/回退模式都能用 |
| Termux `$PREFIX` / `$HOME` | ✅ | Termux 的 app 域允许执行 |
| App 的 `nativeLibraryDir` | ✅ | 系统从 APK 的 `lib/arm64-v8a/` 解压，SELinux 放行 |
| App 的 `filesDir` / `cacheDir` | ⚠️ 多数被拒 | Android 10+ W^X/SELinux 限制 |
| `/sdcard`、`/storage/emulated/0` | ❌ | 通常 `noexec` |
| `/system`、`/vendor` | ❌ | 只读 |

典型流程：

```bash
adb push dist/hello-arm64 /data/local/tmp/hello
adb shell chmod 755 /data/local/tmp/hello
adb shell /data/local/tmp/hello --list
adb shell /data/local/tmp/hello arg1 arg2
```

### 2.1 在 App 内调用

最稳的方式是把产物当原生库打包：

```
app/src/main/jniLibs/arm64-v8a/libshpack.so   # 实际就是你的 ELF
```

安装后系统会解压到 `applicationInfo.nativeLibraryDir`（允许执行），然后：

```java
String exe = getApplicationInfo().nativeLibraryDir + "/libshpack.so";
new ProcessBuilder(exe, "--list").inheritIO().start().waitFor();
```

需要 `android:extractNativeLibs="true"`（或 AGP 的 `useLegacyPackaging true`），
文件名必须是 `lib*.so`。

### 2.2 Termux

```bash
cp your-app $HOME/
chmod +x $HOME/your-app
$HOME/your-app --version
```

Termux 的 `$TMPDIR` 可用于 `--mode dir` 回退。

## 3. 运行时行为（默认：纯内存）

1. 依次尝试 `memfd_create`（Android 8+）→ `/dev/shm` 匿名文件（若有）；
2. 把 bash、busybox、脚本写进匿名文件，只保留 fd；
3. 用 `/proc/self/fd/N` 执行 bash，脚本同样通过 fd 读取；
4. busybox 的 270+ 个 applet 用 shell 函数提供（`ls`、`sed`、`awk`……）；
5. 预检执行 `bash --version`，SELinux 拒绝时自动回退；
6. `auto` 回退顺序：内存 → 临时目录（`$SHPACK_TMPDIR`、`$TMPDIR`、
   `/data/local/tmp`、`/dev/shm`、`/tmp`、`$HOME/.cache`、cwd）；
7. 内存模式没有任何临时文件，退出即释放。

### 3.1 内存模式的 applet 说明

函数只对当前 bash 生效；子 bash 通过导出的 `command_not_found_handle`
回落。`sh -c` 或 `xargs` 等非 bash 子进程看不到函数，如果系统 `PATH`
里没有对应命令，请显式使用：

```bash
"$SHPACK_BUSYBOX" sed -n '1p' file     # 内存模式：busybox 是一个命令名
"$SHPACK_BUSYBOX_FD" ...                # 原始 /proc/self/fd 路径（通常不需要）
```

`--mode dir` 会用真实 PATH 软链，所有子进程都能用 applet，但会落盘。

## 4. 手机上的冒烟测试

```bash
adb push tests/android-smoke.sh /data/local/tmp/
adb push dist/hello-arm64 /data/local/tmp/hello
adb shell sh /data/local/tmp/android-smoke.sh /data/local/tmp/hello
```

脚本会检查：ELF 架构、`--version`/`--list`、参数与退出码、busybox applet、
`SHPACK_BUSYBOX`、`--extract`、临时目录清理，并打印当前运行模式。

## 5. 常见问题

**Q: `Permission denied` / `cannot execute`**
A: 多为 noexec 或 SELinux 拒绝。换到 `/data/local/tmp` 或 `chmod 755`；
`--verbose` 会显示内存模式失败原因和回退路径。

**Q: 内存模式没生效，提示回退 dir**
A: 设备内核 < 3.17、SELinux 禁止执行 `/proc/self/fd/*`，或 `/dev/shm`
不可执行。用 `--verbose` 查看预检错误；功能仍然可用，只是会落盘。

**Q: 子进程里 `sed: command not found`**
A: 内存模式 applet 是 bash 函数，`sh -c` 看不到。改用 `--mode dir`，
或在脚本里用 `"$SHPACK_BUSYBOX" sed ...`。

**Q: 需要 32 位设备**
A: `./build.sh --arch armv7`，并用 armv7 静态 bash/busybox
（`scripts/build-runtime.sh --arch armv7`）。

**Q: 体积太大**
A: 用 musl 版本通常能把 bash+busybox 从 4 MB 降到 2 MB 左右；
`--compress lz4` 对二进制有 35%~40% 压缩率。

**Q: 想确认产物真的静态**
A: `file your-app` 应含 `statically linked`；`readelf -l your-app | grep INTERP`
应无输出。

## 6. 已知边界

* 只支持 Linux 内核；iOS/macOS/Windows 不行；
* 内存模式要求 `/proc`（Android 一定有）；
* 极老设备（kernel < 3.7）不支持静态 glibc 产物，建议用 musl 重新构建；
* 不要在脚本里依赖 `$0` 的同目录资源。
