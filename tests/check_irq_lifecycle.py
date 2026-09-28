"""Run with --qemu /path/to/qemu-system-riscv32 --cc /path/to/riscv-gcc."""
from pathlib import Path
import struct
import time

from qtest_machine import Machine


with Machine(guest=Path(__file__).with_name("irq_lifecycle.S")) as sim:
    state = 0x20000000

    def wait(predicate):
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            assert sim.read(state + 32) == 0, "Guest entered unexpected exception"
            if predicate():
                return
            time.sleep(.01)
        raise AssertionError([sim.read(state + offset) for offset in range(0, 36, 4)])

    wait(lambda: sim.read(state + 28) == 0x1234)
    sim.write(0x40000014, 1)
    wait(lambda: sim.read(state + 8) != 0)
    assert sim.read(state + 8) & (1 << 12), "TIM2 active bit cleared before mret"
    assert sim.read(state + 12) == 0, "USART interrupted the active handler"
    sim.write(state, 1)
    wait(lambda: sim.read(state + 12) == 1)
    wait(lambda: sim.read(0xE000E304) & 0x00201000 == 0)
    sim.write(0x40000014, 1)
    wait(lambda: sim.read(state + 12) == 2)
    assert sim.read(state + 4) == 2
    print("PASS timer acknowledgement, pending USART delivery and repeated interrupts")

    packet = b"\xff" * 6 + b"\x02\0\0\0\0\x01\x88\xb5" + bytes(range(46))
    sim.network.sendall(struct.pack(">I", len(packet)) + packet)
    wait(lambda: sim.read(state + 16) == 1)
    assert sim.read(state + 20) & (1 << 13), "Ethernet active bit cleared before mret"
    sim.write(state, 2)
    wait(lambda: sim.read(0xE000E308) & (1 << 13) == 0)
    print("PASS Ethernet acknowledgement and active-bit clearing at mret")
