"""Run with: uv run --no-project python tests/check_usart.py --qemu /path/to/qemu-system-riscv32"""
import time

from qtest_machine import Machine


with Machine('ch32v307,usart1-echo=on') as sim:
    write, read, step = sim.write, sim.read, sim.step
    uart = sim.uart
    serial = 0x40013800
    write(serial + 8, 20)
    write(serial + 12, 0x203C)
    uart.sendall(b"Q")
    time.sleep(.02)
    step(1)
    assert read(serial) & 0x20
    assert read(serial + 4) == ord("Q")
    step(24998)
    assert not read(serial) & 0x10
    step(1)
    assert read(serial) & 0x10
    read(serial + 4)
    assert not read(serial) & 0x10
    write(serial, 0xFFBF)
    assert not read(serial) & 0x40
    write(serial + 4, ord("R"))
    step(24999)
    assert not read(serial) & 0x40
    step(1)
    assert read(serial) & 0x40
    assert uart.recv(1) == b"R"
    assert read(serial + 4) == ord("R")
    print("PASS UART baud timing, idle detection, flag clearing and local echo")
