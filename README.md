<h1 align="center">Kapsel</h1>

<p align="center">Windows 上的便携式 Linux 虚拟机</p>

Kapsel 随包提供 QEMU。解压后即可从 Linux ISO 启动，安装后的系统和数据存放在同目录的 `vm.qcow2`。

## 下载

| 文件 | 下载 | 用途 |
| --- | --- | --- |
| Kapsel · Windows x64 | [最新发布](https://github.com/chaeoi/kapsel/releases/latest) | 下载 `Kapsel-<版本>-windows-x64.zip`。发布页同时提供 `SHA256SUMS`。 |
| Debian 13（Trixie）· amd64 | [官方 `mini.iso`（约 64 MiB）](https://deb.debian.org/debian/dists/trixie/main/installer-amd64/current/images/netboot/mini.iso) | 轻量安装介质，启动后联网获取系统软件包。 |
| Ubuntu 26.04.1 LTS · amd64 | [官方 Mini ISO（约 122 MiB）](https://cdimage.ubuntu.com/ubuntu-mini-iso/releases/26.04.1/release/ubuntu-mini-iso-26.04.1-mini-iso-amd64.iso) | 启动后联网下载所选版本的完整安装镜像。安装 Server 时可选择 Ubuntu Server (minimized)。 |

宿主机需要 Windows 10 2004 或更新版本（x64）。也可以使用其他支持传统 BIOS 启动的 x86-64 Linux ISO。

## 首次启动

1. 将便携包完整解压到有写入权限的目录，双击 `Kapsel.exe`。
2. 点击「选择 ISO」选中镜像，按需调整虚拟硬件，再点击「从 ISO 启动」。Kapsel 会在程序旁创建 `vm.qcow2`，供 Linux 安装程序使用。虚拟机窗口打开后，启动器会自动关闭。
3. 在 Linux 安装程序中选择虚拟硬盘。使用 Debian 时，可以在「软件选择」中取消桌面环境，保留「标准系统工具」以获得常用命令行工具。取消所有任务可进一步缩小安装占用。 [Debian 安装指南](https://www.debian.org/releases/trixie/amd64/apds02.en.html)
4. 安装完成后关闭虚拟机。下次打开 `Kapsel.exe` 时，点击「从硬盘启动」。

## 运行与数据

- **默认配置**为 2 个虚拟 CPU、4 GiB 内存、64 GiB 虚拟硬盘，使用 TCG 软件模拟和用户态 NAT。普通用户权限即可启动。虚拟 CPU 和内存设置会自动记住。首次启动前可以修改新盘容量，已有磁盘的容量在界面中只读显示。
- **硬件加速**可通过「优先使用 WHPX」启用。宿主机需启用 Windows 虚拟机监控程序平台。WHPX 初始化失败时，QEMU 回退到 TCG。 [QEMU 的 WHPX 文档](https://www.qemu.org/docs/master/system/whpx.html)
- **备份与迁移**时先关闭虚拟机，再复制 `vm.qcow2`。这个文件随实际写入增长。
- **启动异常**时查看程序旁的 `qemu.log`。

## 从源码构建

在 Linux 上准备 Python 3、MinGW-w64 x86-64、7-Zip 和 curl。在 Ubuntu 上可通过 `gcc-mingw-w64-x86-64-win32` 和 `p7zip-full` 安装编译及解包工具。

```sh
python3 -m unittest discover -s tests -v
python3 tools/package.py
```

打包脚本校验 QEMU 下载文件，生成 Windows x64 ZIP 和 `SHA256SUMS`。默认输出到系统临时目录。用 `--output-dir` 指定输出位置，用 `--installer` 复用已下载的 QEMU 安装包。

| 目录 | 内容 |
| --- | --- |
| `src/launcher/` | Win32 启动器和窗口资源 |
| `packaging/windows/` | QEMU 版本锁定和 Windows 运行清单 |
| `tools/` | 打包、版本编号和发布脚本 |
| `tests/` | 版本分配测试 |
| `.github/workflows/` | GitHub Actions 构建与发布入口 |

GitHub Actions 为拉取请求构建开发包。`main` 的发布版本按提交的 UTC 年月和当月序号生成，格式为 `YYYY.M.N`。同一提交重跑时沿用已有版本号。

## 上游组件

便携包使用 Stefan Weil 的 QEMU 11.1.0 Windows x64 构建（2026-08-11）。上游许可证位于解压目录中的 `qemu/COPYING` 和 `qemu/COPYING.LIB`。 [Windows 构建与校验文件](https://qemu.weilnetz.de/w64/) · [对应 QEMU 源码](https://repo.or.cz/qemu/ar7.git/commit/e470268ff4) · [SeaBIOS 源码](https://github.com/qemu/seabios)
