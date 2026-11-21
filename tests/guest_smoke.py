#!/usr/bin/env python3
import argparse
import os
import selectors
import subprocess
import tempfile
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from threading import Thread


class Probe(BaseHTTPRequestHandler):
    def do_GET(self):
        self.send_response(200)
        self.end_headers()
        self.wfile.write(b"KAPSEL_NAT_OK\n")

    def log_message(self, *_):
        pass


def wait_for(vm, output, token, deadline_seconds=70):
    deadline = time.monotonic() + deadline_seconds
    with selectors.DefaultSelector() as events:
        events.register(vm.stdout, selectors.EVENT_READ)
        while token not in output and time.monotonic() < deadline:
            if not events.select(deadline - time.monotonic()):
                break
            data = os.read(vm.stdout.fileno(), 8192)
            if not data:
                break
            output.extend(data)
    if token not in output:
        raise RuntimeError(output.decode("utf-8", errors="replace"))


def boot(disk, arguments, command, marker):
    start = time.monotonic()
    args = ["qemu-system-x86_64", "-machine", "q35", "-accel", "tcg",
            "-smp", "1", "-m", "512", "-vga", "none", "-display", "none",
            "-parallel", "none", "-monitor", "none", "-serial", "stdio",
            "-drive", f"file={disk},format=qcow2,if=virtio", *arguments]
    with subprocess.Popen(args, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                          stderr=subprocess.STDOUT) as vm:
        try:
            output = bytearray()
            wait_for(vm, output, b"user@kapsel")
            print(f"Shell ready in {time.monotonic() - start:.2f} s", flush=True)
            output.clear()
            vm.stdin.write(command.encode() + b"\n")
            vm.stdin.flush()
            wait_for(vm, output, marker, 30)
            vm.stdin.write(b"sudo poweroff\n")
            vm.stdin.flush()
            if vm.wait(timeout=40):
                raise RuntimeError("QEMU exited with an error during guest shutdown")
        finally:
            if vm.poll() is None:
                vm.kill()


def main(image):
    with tempfile.TemporaryDirectory(prefix="kapsel-test-") as directory, \
            ThreadingHTTPServer(("127.0.0.1", 0), Probe) as server:
        disk = Path(directory) / "smoke.qcow2"
        subprocess.run(["qemu-img", "create", "-f", "qcow2", "-F", "qcow2",
                        "-b", image.resolve(), disk], check=True)
        thread = Thread(target=server.serve_forever, daemon=True)
        thread.start()
        try:
            boot(disk, ["-nic", "none"],
                 "printf 'kapsel-persistence\\n' > marker; printf 'OFFLINE_%s\\n' OK",
                 b"OFFLINE_OK")
            nat = ["-netdev", "user,id=nat",
                   "-device", "virtio-net-pci,netdev=nat,addr=0x3,romfile="]
            port = server.server_port
            command = ("grep -qx kapsel-persistence marker && "
                       f"timeout 20 bash -c 'until exec 3<>/dev/tcp/10.0.2.2/{port}; do sleep 1; done; "
                       "printf \"GET / HTTP/1.0\\r\\n\\r\\n\" >&3; cat <&3'")
            boot(disk, nat, command, b"KAPSEL_NAT_OK")
            print("Offline boot, disk persistence, DHCP/NAT, and clean shutdown verified.")
        finally:
            server.shutdown()


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Boot the built-in Debian image twice under TCG.")
    parser.add_argument("image", type=Path)
    main(parser.parse_args().image)
