#!/usr/bin/env python3
import argparse
import hashlib
import json
import re
import shutil
import subprocess
import tempfile
from pathlib import Path

from version import DEVELOPMENT_VERSION, parse_version

ROOT = Path(__file__).resolve().parents[1]
LAUNCHER = ROOT / "src" / "launcher"
PACKAGING = ROOT / "packaging" / "windows"
MINGW = "x86_64-w64-mingw32-"
EMULATORS = ["qemu-system-x86_64.exe", "qemu-img.exe"]
FIRMWARE = ["bios-256k.bin", "vgabios-stdvga.bin", "kvmvapic.bin"]


def file_hash(path, algorithm):
    checksum = hashlib.new(algorithm)
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            checksum.update(block)
    return checksum.hexdigest()


def build_launcher(stage, work, version, components):
    numeric = (*components, 0)
    version_header = work / "version.h"
    version_header.write_text(
        "#define KAPSEL_FILE_VERSION " + ",".join(map(str, numeric)) + "\n"
        f'#define KAPSEL_VERSION "{version}"\n',
        encoding="utf-8",
    )
    manifest = (LAUNCHER / "kapsel.manifest.in").read_text(encoding="utf-8")
    (work / "kapsel.exe.manifest").write_text(
        manifest.replace("@VERSION@", ".".join(map(str, numeric))), encoding="utf-8"
    )
    resource = work / "kapsel.res.o"
    subprocess.run(
        [MINGW + "windres", "--codepage=65001", "-I", work, LAUNCHER / "kapsel.rc", resource],
        check=True,
    )
    subprocess.run(
        [MINGW + "gcc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-Os", "-s",
         "-municode", "-mwindows", "-o", stage / "Kapsel.exe",
         LAUNCHER / "kapsel.c", LAUNCHER / "appearance.c", resource,
         "-lcomdlg32", "-lcomctl32", "-lgdi32"],
        check=True,
    )


def copy_qemu(upstream, destination):
    destination.mkdir()
    libraries = {path.name.lower(): path.name for path in upstream.glob("*.dll")}
    pending = EMULATORS.copy()
    copied = set()
    while pending:
        name = pending.pop()
        if name in copied:
            continue
        binary = upstream / name
        table = subprocess.check_output([MINGW + "objdump", "-p", binary], text=True)
        for dependency in re.findall(r"DLL Name:\s+(\S+)", table):
            filename = libraries.get(dependency.lower())
            if filename:
                pending.append(filename)
        shutil.copy2(binary, destination / name)
        copied.add(name)

    (destination / "share").mkdir()
    for name in FIRMWARE:
        shutil.copy2(upstream / "share" / name, destination / "share" / name)
    for name in ("COPYING", "COPYING.LIB", "README.rst", "VERSION"):
        shutil.copy2(upstream / name, destination / name)
    shutil.copy2(PACKAGING / "qemu-system-x86_64.exe.manifest", destination)


def main():
    parser = argparse.ArgumentParser(description="Build the Kapsel Windows x64 portable ZIP.")
    parser.add_argument("--installer", type=Path, help="Use a local copy of the pinned QEMU installer.")
    parser.add_argument("--output-dir", type=Path, help="Output directory (defaults to a new system temp directory).")
    parser.add_argument("--version", default=DEVELOPMENT_VERSION, help="Release version in YYYY.M.N format.")
    args = parser.parse_args()
    components = (0, 0, 0) if args.version == DEVELOPMENT_VERSION else parse_version(args.version)
    if not components or components[2] > 65535:
        parser.error("--version must be YYYY.M.N with N from 1 to 65535, or 0.0.0-dev")

    dependency = json.loads((PACKAGING / "qemu.json").read_text(encoding="utf-8"))
    with tempfile.TemporaryDirectory(prefix="kapsel-") as temporary:
        work = Path(temporary)
        installer = args.installer.resolve() if args.installer else work / "qemu-setup.exe"
        if not args.installer:
            subprocess.run(["curl", "-fL", "--retry", "2", "-o", installer, dependency["url"]], check=True)
        if file_hash(installer, "sha512") != dependency["sha512"]:
            parser.exit(1, f"SHA-512 mismatch: {installer}\n")

        upstream = work / "upstream"
        subprocess.run(["7z", "x", "-y", "-bso0", f"-o{upstream}", installer], check=True)
        stage = work / "Kapsel"
        stage.mkdir()
        build_launcher(stage, work, args.version, components)
        copy_qemu(upstream, stage / "qemu")
        shutil.copy2(ROOT / "README.md", stage)

        name = f"Kapsel-{args.version}-windows-x64"
        package = Path(shutil.make_archive(str(work / name), "zip", work, stage.name))
        output = args.output_dir.resolve() if args.output_dir else Path(tempfile.mkdtemp(prefix="kapsel-build-"))
        output.mkdir(parents=True, exist_ok=True)
        shutil.move(package, output / package.name)
        (output / "SHA256SUMS").write_text(
            f"{file_hash(output / package.name, 'sha256')}  {package.name}\n", encoding="utf-8"
        )
    print(output / package.name)


if __name__ == "__main__":
    main()
