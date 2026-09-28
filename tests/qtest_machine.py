"""Small socket-based runner for standalone CH32 register regressions."""
import argparse
from contextlib import AbstractContextManager
from pathlib import Path
import socket
import subprocess
import time
import uuid


class Machine(AbstractContextManager):
    def __init__(self, machine="ch32v307", guest=None):
        parser = argparse.ArgumentParser()
        parser.add_argument("--qemu", required=True, type=Path)
        parser.add_argument("--output", type=Path,
                            default=Path.home() / "tools/qemu-ch32v-tests" / uuid.uuid4().hex[:8])
        if guest:
            parser.add_argument("--cc", default="riscv-none-elf-gcc")
        self.guest = guest
        self.args = parser.parse_args()
        self.output = self.args.output.resolve()
        self.output.mkdir(parents=True, exist_ok=True)
        self.machine = machine
        self.resources = []

    def __enter__(self):
        try:
            self.log = (self.output / "qemu.log").open("w")
            self.resources.append(self.log)
            self.network, backend = socket.socketpair()
            self.resources.extend((self.network, backend))
            self.network.settimeout(2)
            self.uart_path = self.output / "uart.sock"
            self.bus_path = self.output / "qtest.sock"
            image = self.output / "idle.bin"
            image.write_bytes(bytes.fromhex("6f000000"))
            if self.guest:
                elf = self.output / "guest.elf"
                subprocess.run([self.args.cc, "-march=rv32imac", "-mabi=ilp32", "-nostdlib",
                                "-Wl,-Ttext=0x08000000,-N", "-Wl,-e,_start", "-o", str(elf),
                                str(self.guest)], check=True)
                cpu_args = ["-accel", "tcg", "-icount", "shift=0,align=off,sleep=off", "-kernel", str(elf)]
                machine = self.machine
            else:
                cpu_args = ["-accel", "qtest"]
                machine = f"{self.machine},flash-image={image}"
            self.process = subprocess.Popen([
                str(self.args.qemu.resolve()), "-M", machine, *cpu_args, "-display", "none", "-monitor", "none",
                "-nic", f"socket,fd={backend.fileno()}",
                "-serial", f"unix:{self.uart_path},server=on,wait=off",
                "-qtest", f"unix:{self.bus_path},server=on,wait=off"],
                stdout=self.log, stderr=subprocess.STDOUT, pass_fds=(backend.fileno(),))
            backend.close()
            deadline = time.monotonic() + 10
            while not (self.uart_path.exists() and self.bus_path.exists()):
                if self.process.poll() is not None or time.monotonic() > deadline:
                    raise RuntimeError((self.output / "qemu.log").read_text())
                time.sleep(.01)
            self.bus = self.connect(self.bus_path)
            self.uart = self.connect(self.uart_path)
            self.wire = self.bus.makefile("rwb", buffering=0)
            self.resources.append(self.wire)
            return self
        except BaseException:
            self.__exit__(None, None, None)
            raise

    def connect(self, path):
        sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.resources.append(sock)
        sock.settimeout(2)
        sock.connect(str(path))
        return sock

    def command(self, text):
        self.wire.write(text.encode() + b"\n")
        reply = self.wire.readline().decode().strip()
        assert reply.startswith("OK"), (text, reply)
        return reply.split()[1:]

    def write(self, address, value):
        self.command(f"writel {address:#x} {value:#x}")

    def read(self, address):
        return int(self.command(f"readl {address:#x}")[0], 0)

    def step(self, ns):
        self.command(f"clock_step {ns}")

    def __exit__(self, *exc):
        if hasattr(self, "process"):
            self.process.terminate()
            try:
                self.process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait()
        for resource in reversed(self.resources):
            resource.close()
        for name in ("uart.sock", "qtest.sock"):
            (self.output / name).unlink(missing_ok=True)
        print(f"Artifacts: {self.output}")
