#!/usr/bin/env python3
import argparse
import hashlib
import shutil
import subprocess
import tempfile
from pathlib import Path

from version import DEVELOPMENT_VERSION, parse_version

ROOT = Path(__file__).resolve().parents[1]
WINDOWS = ROOT / "packaging" / "windows"


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def package(version, runtime, image, output, vterm_source):
    for path in (runtime / "qemu-system-x86_64.exe", runtime / "qemu-img.exe",
                 runtime / "firmware" / "bios-256k.bin", image):
        if not path.is_file():
            raise SystemExit(f"Missing build input: {path}")

    with tempfile.TemporaryDirectory(prefix="kapsel-package-") as temporary:
        work = Path(temporary)
        build = work / "build"
        stage = work / "Kapsel"
        configure = ["cmake", "-S", str(ROOT), "-B", str(build), "-G", "Ninja",
                     f"-DCMAKE_TOOLCHAIN_FILE={WINDOWS / 'mingw-x64.cmake'}",
                     "-DCMAKE_BUILD_TYPE=Release", f"-DKAPSEL_VERSION={version}"]
        if vterm_source:
            configure.append(f"-DFETCHCONTENT_SOURCE_DIR_LIBVTERM={vterm_source}")
        subprocess.run(configure, check=True)
        subprocess.run(["cmake", "--build", build], check=True)
        subprocess.run(["cmake", "--install", build, "--prefix", stage, "--strip"], check=True)

        shutil.copytree(runtime, stage / "runtime")
        for filename in ("qemu-system-x86_64.exe.manifest", "kapsel.bmp"):
            shutil.copy2(WINDOWS / filename, stage / "runtime")
        (stage / "guest").mkdir()
        shutil.copy2(image, stage / "guest" / "debian.qcow2")
        sources = image.with_suffix(".packages")
        if sources.is_file():
            shutil.copy2(sources, stage / "guest" / "debian.packages")
        shutil.copy2(ROOT / "README.md", stage)

        output.mkdir(parents=True, exist_ok=True)
        name = f"Kapsel-{version}-windows-x64"
        archive = Path(shutil.make_archive(str(work / name), "zip", work, stage.name))
        destination = output / archive.name
        shutil.move(archive, destination)
        source = output / f"Kapsel-{version}-qemu-source.tar.xz"
        assets = [destination, source] if source.is_file() else [destination]
        (output / "SHA256SUMS").write_text(
            "".join(f"{sha256(asset)}  {asset.name}\n" for asset in assets), encoding="utf-8")
    return destination


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Build the Kapsel portable Windows x64 ZIP.")
    parser.add_argument("--qemu-runtime", type=Path, required=True)
    parser.add_argument("--guest-image", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path)
    parser.add_argument("--vterm-source", type=Path, help="Use an already verified libvterm source tree.")
    parser.add_argument("--version", default=DEVELOPMENT_VERSION)
    args = parser.parse_args()
    version = parse_version(args.version)
    if args.version != DEVELOPMENT_VERSION and (not version or version[2] > 65535):
        parser.error("--version must be YYYY.M.N with N from 1 to 65535, or 0.0.0-dev")
    output = args.output_dir or Path(tempfile.mkdtemp(prefix="kapsel-build-"))
    print(package(args.version, args.qemu_runtime.resolve(), args.guest_image.resolve(),
                  output.resolve(), args.vterm_source.resolve() if args.vterm_source else None))
