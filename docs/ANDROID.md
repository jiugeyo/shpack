# Android 使用指南

`shpack-builder` 可以在 PC（WSL/Linux）或 Termux 里运行，产出 Android 可执行的
**纯静态 ELF**。目标机器不需要 Android 的 `linker64`、bionic libc、
`/system/bin/sh` 或 toybox。

## 1. ABI 对应

| Android ABI | 架构 | builder 参数 | 说明 |
| --- | --- | --- | --- |
| `arm64-v8a` | aarch64 | `-a aarch64` | 主流设备 |
| `armeabi-v7a` | armv7 | `-a armv7` | 32 位设备；静态 armhf 二进制，不依赖 Android 浮点 ABI |
| `x86_64` | x86_64 | `-a x86_64` | x86_64 模拟器 |
| `x86` | i386 | `-a i386` | 32 位 x86 模拟器 |

最低系统版本主要取决于内核：静态 glibc 产物要求 Linux 3.7+；
纯内存模式需要 `memfd_create`（Android 8+），更老设备自动回退 `dir`。

## 2. 两种用法

### 2.1 电脑上打包（推荐）

```bash
# x86_64 Linux / WSL
chmod +x dist/shpack-builder
./dist/shpack-builder -s your.sh -o your-app -a aarch64

adb push your-app /data/local/tmp/
adb shell chmod 755 /data/local/tmp/your-app
adb shell /data/local/tmp/your-app --help
```

交互式向导中，输出文件默认就是输入脚本名（如 `your.sh`），输出权限也跟随输入脚本；
打包完成一次后向导直接退出，不会询问继续打包。

也可以直接交互：

```bash
./dist/shpack-builder
```

### 2.2 手机 Termux 里打包

```bash
adb push dist/shpack-builder-aarch64 /data/data/com.termux/files/home/
# 在 Termux 中
chmod +x ~/shpack-builder-aarch64
~/shpack-builder-aarch64
```

builder 内置了 4 个架构的 bash/busybox，所以在手机上可以直接打 arm64/armv7 包。

## 3. 部署位置：哪里可以执行？

| 位置 | 可执行 | 说明 |
| --- | --- | --- |
| `/data/local/tmp` | ✅（adb shell 域） | **最推荐** |
| Termux `$PREFIX` / `$HOME` | ✅ | Termux 的 app 域允许 |
| App 的 `nativeLibraryDir` | ✅ | 系统从 APK `lib/<abi>/` 解压，SELinux 放行 |
| App 的 `filesDir` / `cacheDir` | ⚠️ 多数被拒 | Android 10+ W^X/SELinux |
| `/sdcard` | ❌ | 通常 noexec |
| `/system`、`/vendor` | ❌ | 只读 |

### App 内调用

把产物当作原生库打包：

```
app/src/main/jniLibs/arm64-v8a/libshpack.so   # 实际就是你的 ELF
```

```java
String exe = getApplicationInfo().nativeLibraryDir + "/libshpack.so";
new ProcessBuilder(exe, "--list").inheritIO().start().waitFor();
```

需要 `android:extractNativeLibs="true"`（或 AGP `useLegacyPackaging true`）。

## 4. 运行时行为

1. 默认 `auto`：先 `memfd_create`（Android 8+）或 `/dev/shm` 匿名文件；
2. bash、busybox、脚本全部在内存，`/proc/self/fd/N` 执行；
3. 270+ applet 以 bash 函数提供；
4. 预检 `bash --version`，SELinux 拒绝时回退临时目录
   （`$SHPACK_TMPDIR`、`$TMPDIR`、`/data/local/tmp`、`/tmp`、`$HOME/.cache`、cwd）；
5. 内存模式无残留；dir 模式退出时清理。

`--mode memfd` 强制纯内存（失败即报错），`--mode dir` 强制落盘。

### applet 注意

函数只对 bash 生效；`sh -c` / `xargs` 看不到。必要时：

```bash
"$SHPACK_BUSYBOX" sed -n '1p' file
```

`SHPACK_BUSYBOX` 在内存模式是命令名 `busybox`，其原始 fd 路径在
`SHPACK_BUSYBOX_FD`。

## 5. 手机冒烟测试

```bash
adb push dist/shpack-builder /data/local/tmp/
adb shell chmod 755 /data/local/tmp/shpack-builder
adb shell /data/local/tmp/shpack-builder -s /data/local/tmp/test.sh -o /data/local/tmp/app -a aarch64
adb shell /data/local/tmp/app --version
```

或使用 `tests/android-smoke.sh` 检查已有产物。

## 6. 常见问题

**Q: `Permission denied` / `cannot execute`**
A: noexec 或 SELinux。换 `/data/local/tmp`，`chmod 755`；`--verbose` 看内存模式失败原因。

**Q: 内存模式没生效**
A: 内核 < 3.17、SELinux 禁止执行 `/proc/self/fd/*`，或 `/dev/shm` 不可执行。
功能仍可用，只是落盘；需要强制失败用 `--mode memfd`。

**Q: 32 位设备**
A: `-a armv7`。arm64 产物不能跑在 32 位设备上。

**Q: x86 模拟器**
A: `-a i386`；x86_64 模拟器用 `-a x86_64`。

**Q: 体积太大**
A: `--compress lz4` 对二进制有 35%~40% 压缩率；用 musl 运行时还能再小
（`scripts/build-runtime.sh`）。

**Q: 确认产物真的静态**
A: `file app` 应含 `statically linked`；`readelf -l app | grep INTERP` 应无输出。

## 7. 已知边界

* 只支持 Linux 内核；iOS/macOS/Windows 不行；
* builder 本身要匹配主机架构（x86_64 PC 或 aarch64 Termux）；
* armv7/i386 在本仓库未做真机执行验证，只做了 ELF/格式校验；
* 不要在脚本里依赖 `$0` 的同目录资源。
