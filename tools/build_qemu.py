#!/usr/bin/env python3
import argparse
import concurrent.futures
import hashlib
import json
import os
import re
import shutil
import subprocess
import tarfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
RECIPES = ROOT / "third_party" / "qemu"
TARGET = "x86_64-w64-mingw32"
MSYS2 = "https://repo.msys2.org/mingw/mingw64"


def run(*args, **kwargs):
    return subprocess.run(args, check=True, **kwargs)


def fetch(cache, item):
    target = cache / item["file"]
    if not target.exists():
        working = target.with_suffix(target.suffix + ".partial")
        run("curl", "--fail", "--silent", "--show-error", "--location", "--retry", "3",
            "--output", working, item["url"])
        working.replace(target)
    with target.open("rb") as stream:
        checksum = hashlib.sha256()
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            checksum.update(block)
    if checksum.hexdigest() != item["sha256"]:
        raise SystemExit(f"SHA-256 mismatch: {target}")
    return target


def prepare_sysroot(cache, work):
    packages = json.loads((RECIPES / "mingw-packages.json").read_text())
    for package in packages:
        package["url"] = f"{MSYS2}/{package['file']}"
    with concurrent.futures.ThreadPoolExecutor(max_workers=6) as workers:
        archives = list(workers.map(lambda package: fetch(cache, package), packages))

    sysroot = work / "mingw64"
    for archive in archives:
        run("tar", "--extract", "--zstd", "--file", archive, "--directory", work,
            "--exclude=.BUILDINFO", "--exclude=.MTREE", "--exclude=.PKGINFO")
    for pc in (sysroot / "lib" / "pkgconfig").glob("*.pc"):
        contents = re.sub(r"^prefix=.*$", f"prefix={sysroot}", pc.read_text(), flags=re.M)
        pc.write_text(contents)
    return sysroot


def build(work, output, jobs, source_output):
    source = json.loads((RECIPES / "source.json").read_text())
    source["file"] = f"qemu-{source['version']}.tar.xz"
    cache = work / "downloads"
    cache.mkdir(parents=True, exist_ok=True)
    qemu_dir = work / f"qemu-{source['version']}"
    archive = fetch(cache, source)
    if not qemu_dir.exists():
        run("tar", "-xJf", archive, "-C", work)
    for patch in sorted(RECIPES.glob("*.patch")):
        check = subprocess.run(("patch", "-p1", "--reverse", "--dry-run", "--force",
                                "--input", patch), cwd=qemu_dir, stdout=subprocess.DEVNULL)
        if check.returncode:
            run("patch", "-p1", "--input", patch, cwd=qemu_dir)

    slirp = json.loads((RECIPES / "slirp.json").read_text())
    slirp_archive = fetch(cache, slirp)
    slirp_dir = qemu_dir / "subprojects" / "slirp"
    if not slirp_dir.is_dir():
        slirp_dir.mkdir()
        run("tar", "-xzf", slirp_archive, "--strip-components=1", "-C", slirp_dir)

    toolchain = json.loads((RECIPES / "toolchain.json").read_text())
    toolchain_bin = work / toolchain["directory"] / "bin"
    toolchain_archive = fetch(cache, toolchain)
    if not toolchain_bin.is_dir():
        run("tar", "-xJf", toolchain_archive, "-C", work)
    shutil.copy2(RECIPES / "kapsel.mak",
                 qemu_dir / "configs" / "devices" / "x86_64-softmmu" / "kapsel.mak")
    sysroot = prepare_sysroot(cache, work)
    pkg_config = work / "mingw-pkg-config"
    pkg_config.write_text(
        '#!/bin/sh\nunset PKG_CONFIG_PATH\n'
        f'export PKG_CONFIG_LIBDIR="{sysroot}/lib/pkgconfig"\nexec pkg-config "$@"\n'
    )
    pkg_config.chmod(0o755)
    build_dir = work / "qemu-build"
    build_dir.mkdir(exist_ok=True)
    env = dict(os.environ, PKG_CONFIG=str(pkg_config),
               PATH=f"{toolchain_bin}{os.pathsep}{os.environ['PATH']}",
               CC=f"{TARGET}-clang", CXX=f"{TARGET}-clang++")
    run(qemu_dir / "configure", f"--cross-prefix={TARGET}-",
        "--target-list=x86_64-softmmu", "--without-default-features",
        "--without-default-devices", "--with-devices-x86_64=kapsel", "--enable-tools",
        "--enable-tcg", "--enable-whpx", "--enable-sdl", "--enable-pixman", "--enable-slirp",
        "--disable-fdt",
        "--disable-docs", "--disable-werror", "--disable-debug-info",
        f"--extra-cflags=-I{sysroot}/include",
        f"--extra-ldflags=-L{sysroot}/lib", cwd=build_dir, env=env)
    run("ninja", "-j", str(jobs), "qemu-system-x86_64.exe", "qemu-img.exe", cwd=build_dir, env=env)

    output.mkdir(parents=True, exist_ok=True)
    firmware = output / "firmware"
    firmware.mkdir(exist_ok=True)
    for name in ("bios-256k.bin", "vgabios-stdvga.bin", "kvmvapic.bin"):
        shutil.copy2(qemu_dir / "pc-bios" / name, firmware)
    for name in ("COPYING", "COPYING.LIB"):
        shutil.copy2(qemu_dir / name, output)
    toolchain_target = toolchain_bin.parent / TARGET
    libraries = {path.name.lower(): path for folder in (sysroot / "bin", toolchain_target / "bin")
                 for path in folder.glob("*.dll")}
    pending = [build_dir / "qemu-system-x86_64.exe", build_dir / "qemu-img.exe"]
    for path in (build_dir / "subprojects" / "slirp").glob("*.dll"):
        libraries[path.name.lower()] = path
    shutil.copy2(slirp_dir / "COPYRIGHT", output / "libslirp-COPYRIGHT")
    copied = set()
    while pending:
        binary = pending.pop()
        if binary.name.lower() in copied:
            continue
        imports = subprocess.check_output((f"{TARGET}-objdump", "-p", binary), text=True)
        for name in re.findall(r"DLL Name:\s+(\S+)", imports):
            if name.lower() in libraries:
                pending.append(libraries[name.lower()])
        released = output / binary.name
        shutil.copy2(binary, released)
        if released.suffix.lower() == ".exe":
            run(toolchain_bin / f"{TARGET}-strip", "--strip-unneeded", released)
        copied.add(binary.name.lower())
    if source_output:
        source_output.parent.mkdir(parents=True, exist_ok=True)
        with tarfile.open(source_output, "w:xz", preset=1) as bundle:
            for item in (archive, slirp_archive):
                bundle.add(item, arcname=f"downloads/{item.name}")
            bundle.add(RECIPES, arcname="third_party/qemu")
            bundle.add(Path(__file__), arcname="tools/build_qemu.py")
    licenses = sysroot / "share" / "licenses"
    shutil.copytree(licenses, output / "licenses", dirs_exist_ok=True)
    shutil.copy2(toolchain_target / "share" / "mingw32" / "COPYING.winpthreads.txt",
                 output / "licenses")
    shutil.copy2(qemu_dir / "pc-bios" / "README", firmware)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Build the Windows x64 QEMU runtime.")
    parser.add_argument("--work-dir", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--source-output", type=Path)
    parser.add_argument("--jobs", type=int, default=min(os.cpu_count() or 2, 6))
    args = parser.parse_args()
    build(args.work_dir.resolve(), args.output_dir.resolve(), args.jobs, args.source_output)
