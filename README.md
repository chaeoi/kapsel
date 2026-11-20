# Kapsel

适用于 Windows 10 2004 或更新版本及 Windows 11（x64）。运行支持传统 BIOS 引导的 x86-64 Linux ISO，例如 Alpine 或 Ubuntu。ISO 由用户提供。

1. 完整解压 ZIP 到有写入权限的目录，双击 `Kapsel.exe`。
2. 点「选择 ISO」，再点「从 ISO 启动」。首次启动会在程序旁创建 `vm.qcow2`，可在 Linux 安装程序里将系统装入这块虚拟硬盘。
3. 安装完成后关闭虚拟机，下次点「从硬盘启动」。系统和文件都保存在 `vm.qcow2`。备份或移动前先关闭虚拟机。

默认使用 **TCG + NAT**，普通用户权限即可启动。虚拟机分配 2 个虚拟 CPU、4 GiB 内存和一块按需增长的 64 GiB 虚拟硬盘。

勾选「优先使用 WHPX」后，QEMU 先尝试硬件加速，初始化失败时回退到 TCG。WHPX 需要宿主 Windows 已启用「Windows 虚拟机监控程序平台」。启动失败时可查看程序旁的 `qemu.log`。[QEMU 的 WHPX 文档](https://www.qemu.org/docs/master/system/whpx.html)

## 仓库结构

| 目录 | 内容 |
| --- | --- |
| `src/launcher/` | Win32 启动器、窗口资源和应用清单 |
| `packaging/windows/` | QEMU 下载锁定信息和 Windows 运行清单 |
| `tools/` | 打包、版本计算和发布脚本 |
| `tests/` | 版本分配测试 |
| `.github/workflows/` | GitHub Actions 构建与发布入口 |

## 本地构建

在 Linux 上准备 Python 3、MinGW-w64 x86-64、7-Zip 和 curl。在 Ubuntu 上对应的额外构建包是 `gcc-mingw-w64-x86-64-win32` 和 `p7zip-full`。

```sh
python3 -m unittest discover -s tests -v
python3 tools/package.py
```

脚本校验 QEMU 安装包后打包，输出 ZIP 的完整路径。默认在系统临时目录下生成 `Kapsel-0.0.0-dev-windows-x64.zip` 和 `SHA256SUMS`。可以用 `--output-dir` 指定输出位置，或用 `--installer` 复用已下载的安装包。

## 自动发布

GitHub Actions 为拉取请求构建开发包。推送到 `main` 后，工作流以提交时间的 UTC 年月和当月发布序号生成 `YYYY.M.N` 版本，例如 `2026.11.1`。构建通过后发布同版本的 Git 标签、Windows x64 ZIP 和 SHA-256 校验文件。同一个提交重新运行时沿用已分配的版本。

## QEMU 来源

便携包使用 Stefan Weil 的 QEMU 11.1.0 Windows x64 构建（2026-08-11）。上游许可证随包附在 `qemu/COPYING` 和 `qemu/COPYING.LIB`。[安装包与校验文件](https://qemu.weilnetz.de/w64/) · [对应源码提交](https://repo.or.cz/qemu/ar7.git/commit/e470268ff4) · [Windows 构建说明](https://qemu.weilnetz.de/w64/#build) · [SeaBIOS 源码](https://github.com/qemu/seabios)
