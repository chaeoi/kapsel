#!/usr/bin/env bash
set -euo pipefail

repository=$(cd -- "$(dirname -- "$0")/.." && pwd)
if [[ $# != 1 ]]; then
    echo "Usage: sudo tools/build_guest_image.sh OUTPUT.qcow2" >&2
    exit 2
fi
if [[ $EUID != 0 ]]; then
    echo "The guest builder requires root for the isolated disk mount." >&2
    exit 1
fi
output=$(realpath -m -- "$1")
if [[ ! -d $(dirname "$output") ]]; then
    echo "Create the output directory first: $(dirname "$output")" >&2
    exit 1
fi
if [[ -z ${KAPSEL_MOUNT_NAMESPACE:-} ]]; then
    export KAPSEL_MOUNT_NAMESPACE=1
    exec unshare --mount --propagation private bash "$0" "$output"
fi

work=$(mktemp -d "${TMPDIR:-/tmp}/kapsel-guest.XXXXXX")
partition=
cleanup() {
    if mountpoint -q "$work/root"; then umount -R "$work/root"; fi
    if [[ -n $partition ]]; then losetup -d "$partition"; fi
    rm -rf -- "$work"
}
trap cleanup EXIT
mkdir "$work/root"

truncate -s 4G "$work/disk.raw"
sfdisk "$work/disk.raw" <<'PARTITIONS'
label: dos
start=2048, type=83, bootable
PARTITIONS
partition=$(losetup --show --find --partscan "$work/disk.raw")
for _ in {1..20}; do
    [[ -b ${partition}p1 ]] && break
    sleep .25
done
mkfs.ext4 -q -F -m 0 -L kapsel "${partition}p1"
mount "${partition}p1" "$work/root"
root=$work/root

cache=()
if [[ -n ${KAPSEL_DEBIAN_CACHE:-} ]]; then
    mkdir -p "$KAPSEL_DEBIAN_CACHE"
    cache=(--cache-dir="$KAPSEL_DEBIAN_CACHE")
fi
packages=$(paste -sd, "$repository/guest/debian/packages.txt")
keyring=${KAPSEL_DEBIAN_KEYRING:-}
if [[ -z $keyring ]]; then
    keyring_digest=9ea7778e443144ca490668737a8ab22dd3e748bb99e805e22ec055abeb3c7fac
    keyring_package=debian-archive-keyring_2025.1_all.deb
    curl --fail --silent --show-error --location --retry 3 \
        "https://deb.debian.org/debian/pool/main/d/debian-archive-keyring/$keyring_package" \
        -o "$work/$keyring_package"
    printf '%s  %s\n' "$keyring_digest" "$work/$keyring_package" | sha256sum --check
    dpkg-deb --extract "$work/$keyring_package" "$work/keyring"
    keyring=$work/keyring/usr/share/keyrings/debian-archive-keyring.gpg
fi
suite=trixie
mirror=https://deb.debian.org/debian
debootstrap --arch=amd64 --variant=minbase --foreign --include="$packages" \
    --keyring="$keyring" "${cache[@]}" "$suite" "$root" "$mirror"

printf '#!/bin/sh\nexit 101\n' > "$root/usr/sbin/policy-rc.d"
chmod +x "$root/usr/sbin/policy-rc.d"
# initramfs-tools and the kernel maintainer scripts need proc and sysfs in the image.
mount -t proc proc "$root/proc"
mount -t sysfs sysfs "$root/sys"
if ! DEBIAN_FRONTEND=noninteractive chroot "$root" /debootstrap/debootstrap --second-stage; then
    tail -100 "$root/debootstrap/debootstrap.log" >&2
    exit 1
fi
rm -f "$root/usr/sbin/policy-rc.d"

printf 'kapsel\n' > "$root/etc/hostname"
printf '127.0.0.1 localhost\n127.0.1.1 kapsel\n' > "$root/etc/hosts"
printf 'LABEL=kapsel / ext4 defaults,noatime 0 1\n' > "$root/etc/fstab"
cat > "$root/etc/apt/sources.list" <<'SOURCES'
deb https://deb.debian.org/debian trixie main
deb https://deb.debian.org/debian trixie-updates main
deb https://security.debian.org/debian-security trixie-security main
SOURCES
install -D "$repository/guest/debian/ethernet.network" "$root/etc/systemd/network/10-ethernet.network"
ln -snf /run/systemd/resolve/stub-resolv.conf "$root/etc/resolv.conf"
systemctl --root="$root" enable systemd-networkd.service systemd-resolved.service
systemctl --root="$root" disable systemd-networkd-wait-online.service
mkdir -p "$root/etc/systemd/system/serial-getty@ttyS0.service.d"
cp "$repository/guest/debian/serial-getty.conf" \
    "$root/etc/systemd/system/serial-getty@ttyS0.service.d/autologin.conf"
systemctl --root="$root" enable serial-getty@ttyS0.service
chroot "$root" useradd -m -s /bin/bash user
chroot "$root" passwd -l root
printf 'user ALL=(ALL) NOPASSWD: ALL\n' > "$root/etc/sudoers.d/local-console"
chmod 0440 "$root/etc/sudoers.d/local-console"
cat > "$root/home/user/.bashrc" <<'PROFILE'
[[ $- == *i* ]] || return
PS1='\[\e[1;36m\]\u@\h\[\e[0m\]:\[\e[1;34m\]\w\[\e[0m\]\$ '
alias ls='ls --color=auto'
PROFILE
chroot "$root" chown user:user /home/user/.bashrc

# Always install the drivers used by the built-in Q35 VM, even under a host chroot.
printf 'virtio_pci\nvirtio_blk\nvirtio_net\n' >> "$root/etc/initramfs-tools/modules"
printf 'MODULES=list\n' > "$root/etc/initramfs-tools/conf.d/kapsel"
printf 'ext4\n' >> "$root/etc/initramfs-tools/modules"
chroot "$root" update-initramfs -u -k all
DEBIAN_FRONTEND=noninteractive chroot "$root" apt-get clean
rm -f "$root/var/lib/systemd/random-seed"
# dpkg-query expands the package fields inside the format string.
# shellcheck disable=SC2016
chroot "$root" dpkg-query -W -f='${binary:Package}	${Version}	${source:Package}	${source:Version}\n' > "$work/debian.packages"
truncate -s 0 "$root/etc/machine-id"
find "$root/var/lib/apt/lists" -type f -delete

grub-install --target=i386-pc --boot-directory="$root/boot" \
    --modules='part_msdos ext2 biosdisk' "$partition"
cp "$repository/guest/debian/grub.cfg" "$root/boot/grub/grub.cfg"
sync -f "$root"
fstrim "$root"
umount -R "$root"
losetup -d "$partition"
partition=

qemu-img convert -f raw -O qcow2 -c "$work/disk.raw" "$work/debian.qcow2"
qemu-img check "$work/debian.qcow2"
install -m 0644 -o "${SUDO_UID:-0}" -g "${SUDO_GID:-0}" \
    "$work/debian.qcow2" "$output"
install -m 0644 -o "${SUDO_UID:-0}" -g "${SUDO_GID:-0}" \
    "$work/debian.packages" "${output%.qcow2}.packages"
