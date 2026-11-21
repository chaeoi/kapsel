import argparse
import json
import os
import subprocess
from pathlib import Path

from version import git, parse_version, select_version


def gh(*args):
    return subprocess.check_output(["gh", *args], text=True).strip()


def publish(version, output_dir):
    head = git("rev-parse", "HEAD")
    if select_version(head) != version:
        raise SystemExit("Version allocation changed after build. Rerun the workflow.")

    package = output_dir / f"Kapsel-{version}-windows-x64.zip"
    checksums = output_dir / "SHA256SUMS"
    source = output_dir / f"Kapsel-{version}-qemu-source.tar.xz"
    subprocess.run(["sha256sum", "--check", "SHA256SUMS"], cwd=output_dir, check=True)
    for asset in (package, source):
        if not asset.is_file():
            raise SystemExit(f"Missing release asset: {asset.name}")

    tag = f"refs/tags/{version}"
    exists = subprocess.run(["git", "show-ref", "--verify", "--quiet", tag]).returncode == 0
    if not exists:
        git("-c", "user.name=github-actions[bot]",
            "-c", "user.email=41898282+github-actions[bot]@users.noreply.github.com",
            "tag", "-a", version, head, "-m", f"Kapsel {version}")
        git("push", "origin", tag)

    repository = os.environ["GITHUB_REPOSITORY"]
    pages = json.loads(gh("api", "--paginate", "--slurp", f"repos/{repository}/releases?per_page=100"))
    releases = [release for page in pages for release in page]
    current = next((release for release in releases if release["tag_name"] == version), None)
    if current and not current["draft"]:
        print(f"Kapsel {version} is already published")
        return

    if not current:
        gh("release", "create", version, "--draft", "--verify-tag",
           "--title", f"Kapsel {version}", "--generate-notes")
    gh("release", "upload", version, str(package), str(source), str(checksums), "--clobber")
    number = parse_version(version)
    published = (
        parse_version(release["tag_name"]) for release in releases
        if not release["draft"] and not release["prerelease"]
    )
    latest = not any(released > number for released in published if released)
    gh("release", "edit", version, "--draft=false", f"--latest={str(latest).lower()}")
    print(f"Published Kapsel {version}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Publish a verified Kapsel build to GitHub.")
    parser.add_argument("version")
    parser.add_argument("output_dir", type=Path)
    args = parser.parse_args()
    publish(args.version, args.output_dir.resolve())
