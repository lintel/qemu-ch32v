"""Run with: uv run --no-project python tests/check_clock.py --qemu /path/to/qemu-system-riscv32"""

from qtest_machine import Machine


with Machine('ch32v307,hse-frequency=16000000') as sim:
    write, read, step = sim.write, sim.read, sim.step
    assert read(0x1FFFF70C) & (1 << 14)
    write(0x4002102C, 1)
    assert read(0x40021004) & (1 << 17)
    write(0x40021004, 0x00290402)
    assert read(0x4002102C) & 1 == 0
    write(0x40021004, 0x002B0402)
    assert read(0x4002102C) & 1 == 1
    timer = 0x40000400
    write(timer + 0x2C, 65535)
    write(timer, 1)
    write(timer + 0x28, 0)
    for encoding, multiplier in enumerate((18, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 6.5, 15, 16)):
        write(0x40021004, (encoding << 18) | 0x30002)
        write(timer + 0x24, 0)
        step(1000)
        assert read(timer + 0x24) == int(8 * multiplier), encoding
    for encoding, multiplier in enumerate((2.5, 12.5, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 20)):
        write(0x4002102C, 0x10011 | (encoding << 8))
        write(0x40021004, 0x70002)
        write(timer + 0x24, 0)
        step(1000)
        assert read(timer + 0x24) == int(12 * multiplier), encoding
    write(0x4002102C, 1)
    write(0x40021004, 0x002B0402)
    print("PASS CH32V307 PLL/PLL2 encodings and divider-bit alias")
