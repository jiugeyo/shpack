# 第三方组件与许可证

本仓库自身代码（`src/`、`scripts/`、`tests/`）使用 MIT 许可证，见 [LICENSE](LICENSE)。

`prebuilt/`、`dist/` 以及 builder 内嵌的 **GNU bash** 和 **BusyBox** 是第三方
GPL 程序：

| 组件 | 版本 | 许可证 | 来源 |
| --- | --- | --- | --- |
| GNU bash | 5.2.21 | GPL-3.0-or-later | Ubuntu 24.04 `bash-static` 二进制包 |
| BusyBox | 1.36.1 | GPL-2.0-only | Ubuntu 24.04 `busybox-static` 二进制包 |

覆盖架构：`amd64`、`arm64`、`armhf`、`i386`。

> **aarch64 二进制修改说明**：为了在 Android Termux（`untrusted_app` seccomp）
> 下运行，`prebuilt/aarch64/bash`、`prebuilt/aarch64/busybox`、aarch64 stub 和
> `dist/shpack-builder-aarch64` 做了少量指令级补丁：把 `set_robust_list` / `rseq`
> 的 `svc` 改为 `nop`，把 `setgid` / `setuid` 等改为返回 0。源码未修改，
> 补丁脚本见 [`scripts/patch-android.py`](scripts/patch-android.py)。

* 二进制取自 Ubuntu 官方仓库，aarch64 版本额外做了上述兼容补丁：
  * `http://archive.ubuntu.com/ubuntu/pool/universe/b/bash/`
  * `http://archive.ubuntu.com/ubuntu/pool/main/b/busybox/`
  * `http://ports.ubuntu.com/ubuntu-ports/pool/universe/b/bash/`（arm64/armhf）
  * `http://ports.ubuntu.com/ubuntu-ports/pool/main/b/busybox/`
* 上游源码：
  * bash: <https://ftp.gnu.org/gnu/bash/bash-5.2.tar.gz>
  * busybox: <https://busybox.net/downloads/busybox-1.36.1.tar.bz2>
* 许可证全文见 [`prebuilt/licenses/`](prebuilt/licenses/)：
  * [GPL-3.0](prebuilt/licenses/GPL-3.0.txt)
  * [GPL-2.0](prebuilt/licenses/GPL-2.0.txt)

如果你重新分发由本工具打包出的可执行文件（其中包含 bash 与 BusyBox），
请同时遵守 GPL 的源码提供义务：附上上述源码，或提供有效的书面要约。
`shpack` 只做封装，不会改变这些程序的许可证。

如果你用 `scripts/build-runtime.sh` 自行构建，请以你构建出的版本为准。
