# Kapsel

Windows x64 便携虚拟机。解压后双击 `Kapsel.exe`，即可离线进入内置的 Debian 13 命令行。也可以通过同一个窗口从自选 ISO 启动图形虚拟机。

[下载 Windows x64 便携包](https://github.com/chaeoi/kapsel/releases/latest)

## 使用

将 ZIP 完整解压到有写入权限的目录。使用 Windows 10 2004 或更新版本的 x64 系统。

| 操作 | 用法 |
| --- | --- |
| 打开内置 Debian | 双击 `Kapsel.exe`。系统会以普通用户登录，提供 Bash、APT 和 sudo。 |
| 从 ISO 安装 | 点击「从 ISO 启动」，选择支持传统 BIOS 启动的 x86-64 ISO。图形窗口出现后，主窗口最小化。 |
| 进入 ISO 安装的系统 | 在主窗口的更多操作中选择「从 ISO 虚拟硬盘启动」。 |
| 设置虚拟硬件 | 「虚拟机设置」可调整 ISO 虚拟机的 CPU 数、内存和新盘容量。设置于下次启动生效。 |
| 关机 | 在客户机内关机、使用主窗口的「关机」，或关闭 ISO 图形窗口以向客户机发送电源事件。 |

内置 Debian 使用 1 个虚拟 CPU、512 MiB 内存和 4 GiB 虚拟磁盘。ISO 虚拟机默认使用 2 个虚拟 CPU、4 GiB 内存和 32 GiB 新盘。两种模式默认使用 TCG 和用户态 NAT。「优先使用 WHPX」适用于两种模式。打开此设置后，QEMU 会优先使用 Windows 虚拟机监控程序平台，初始化失败时回退到 TCG。 [WHPX 文档](https://www.qemu.org/docs/master/system/whpx.html)

### 文件与备份

- `data/debian.qcow2` 保存内置 Debian 的系统和用户文件。首次启动时由随包镜像生成。
- `data/iso.qcow2` 保存从外部 ISO 安装的系统。磁盘文件随实际写入增长。
- `settings.ini` 保存虚拟硬件和加速设置。`data/qemu.log` 记录最近一次虚拟机启动。

关机后可复制对应的 QCOW2 文件做备份。

### 官方安装镜像

| 镜像 | 大小 | 安装方式 |
| --- | --- | --- |
| Debian 13 amd64 [mini.iso](https://deb.debian.org/debian/dists/trixie/main/installer-amd64/current/images/netboot/mini.iso) | 约 64 MiB | 在安装过程中联网获取 Debian 软件包。 |
| Ubuntu 26.04.1 LTS amd64 [Mini ISO](https://cdimage.ubuntu.com/ubuntu-mini-iso/releases/26.04.1/release/ubuntu-mini-iso-26.04.1-mini-iso-amd64.iso) | 约 122 MiB | 联网下载所选 Ubuntu 安装镜像。 |

## 源码构建

在 Ubuntu 24.04 x86-64 上准备构建依赖，分别生成 QEMU 运行时和 Debian 磁盘，再组装便携包。Debian 构建使用独立挂载命名空间，需要 root 和可用的 loop 设备。

```sh
sudo apt-get install cmake ninja-build g++-mingw-w64-x86-64-win32 \
  binutils-mingw-w64-x86-64 pkg-config python3-venv zstd debootstrap \
  grub-pc-bin grub2-common qemu-system-x86 qemu-utils

build_root="$(mktemp -d)"
python3 tools/build_qemu.py --work-dir "$build_root/qemu" --output-dir "$build_root/runtime"
sudo tools/build_guest_image.sh "$build_root/debian.qcow2"
python3 tests/guest_smoke.py "$build_root/debian.qcow2"
python3 tools/package.py --qemu-runtime "$build_root/runtime" \
  --guest-image "$build_root/debian.qcow2" --output-dir "$build_root/artifacts"
```

| 路径 | 职责 |
| --- | --- |
| `src/app/` | Win32 主窗口、系统切换、硬件设置 |
| `src/terminal/` | 内置终端的输入、绘制和滚动历史 |
| `src/virtualization/` | 虚拟磁盘、QEMU 进程、串口和 QMP 管理 |
| `guest/debian/` | 内置 Debian 的软件包和启动配置 |
| `third_party/qemu/` | 上游版本锁、虚拟设备配置、Windows 集成补丁 |
| `packaging/windows/` | Windows 工具链、应用资源和运行清单 |
| `tools/`, `tests/` | 构建、打包、验收和发布 |

GitHub Actions 在 Linux 构建、测试离线镜像和 NAT，然后在 Windows 检查便携包。通过检查的 `main` 构建按 UTC 年月及当月序号发布，附带 ZIP、QEMU 源码归档和 SHA-256 校验文件。

### 上游组件

QEMU 11.1.3 从源码构建，仅生成 Windows x64 运行所需的虚拟机程序与磁盘工具。设备配置保留 Q35、VirtIO 磁盘和网卡、AHCI 光驱、VGA、USB 指针、串口与 ACPI。上游文件、补丁和编译配置见 QEMU 源码归档与仓库的 `third_party/qemu/`。运行时许可证随包提供。 [QEMU](https://www.qemu.org/) · [SeaBIOS](https://github.com/qemu/seabios)

内置系统来自 Debian 13 软件包仓库。镜像内的 `/usr/share/doc/` 保留软件包版权说明，便携包的 `guest/debian.packages` 记录二进制包与源码包版本。 [Debian 源码归档](https://snapshot.debian.org/)
