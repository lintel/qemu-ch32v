"""Run with: uv run --no-project python tests/check_timer_compare.py --qemu /path/to/qemu-system-riscv32"""

from qtest_machine import Machine


with Machine('ch32v307') as sim:
    write, read, step = sim.write, sim.read, sim.step
    timer = 0x40000400
    write(timer + 0x28, 7)
    write(timer + 0x2C, 65535)
    write(timer + 0x34, 20)
    write(timer + 0x0C, 3)
    write(timer, 1)
    step(19000)
    assert read(timer + 0x24) == 19
    assert not read(timer + 0x10) & 2
    step(1000)
    assert read(timer + 0x10) & 2
    write(timer + 0x10, 0xFFFD)
    assert not read(timer + 0x10) & 2
    write(timer + 0x24, 65530)
    write(timer + 0x34, 3)
    step(6000)
    assert read(timer + 0x24) == 0
    assert read(timer + 0x10) & 1
    step(3000)
    assert read(timer + 0x24) == 3 and read(timer + 0x10) & 2
    write(timer + 0x10, 0)
    step(500)
    write(timer + 0x34, 4)
    step(499)
    assert not read(timer + 0x10) & 2
    step(1)
    assert read(timer + 0x10) & 2
    print("PASS 1 MHz counter, compare deadline and counter wrap")
