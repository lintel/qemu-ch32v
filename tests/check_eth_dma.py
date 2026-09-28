"""Run with: uv run --no-project python tests/check_eth_dma.py --qemu /path/to/qemu-system-riscv32"""
import struct
import time

from qtest_machine import Machine


with Machine('ch32v307') as sim:
    write, read, step = sim.write, sim.read, sim.step
    network = sim.network
    ethernet = 0x40028000
    tx_descriptor, rx_descriptor = 0x20001000, 0x20001020
    tx_buffer, rx_buffer = 0x20002000, 0x20003000
    for offset in (0x1048, 0x104C, 0x1050, 0x1054):
        assert read(ethernet + offset) == 0
        write(ethernet + offset, 0xFFFFFFFF)
        assert read(ethernet + offset) == 0
    packet = b"\xff" * 6 + b"\x02\0\0\0\0\x01\x88\xb5" + bytes(range(46))
    for offset in range(0, len(packet), 4):
        write(tx_buffer + offset, int.from_bytes(packet[offset:offset + 4], "little"))
    write(tx_descriptor, 0xB0100000)
    write(tx_descriptor + 4, len(packet))
    write(tx_descriptor + 8, tx_buffer)
    write(tx_descriptor + 12, tx_descriptor)
    write(rx_descriptor, 0x80000000)
    write(rx_descriptor + 4, 0x4000 | 2047)
    write(rx_descriptor + 8, rx_buffer)
    write(rx_descriptor + 12, rx_descriptor)
    write(ethernet + 0x1010, tx_descriptor)
    write(ethernet + 0x100C, rx_descriptor)
    assert read(ethernet + 0x1048) == read(ethernet + 0x104C) == 0
    write(ethernet + 0x1018, (1 << 13) | 2)
    write(ethernet + 0x1004, 0)
    step(1)
    wire_packet = network.recv(4096)
    while len(wire_packet) < 4 + len(packet):
        wire_packet += network.recv(4096)
    assert wire_packet == struct.pack(">I", len(packet)) + packet
    network.sendall(wire_packet)
    deadline = time.monotonic() + 2
    while read(rx_descriptor) & 0x80000000 and time.monotonic() < deadline:
        time.sleep(.01)
        step(1)
    assert not read(rx_descriptor) & 0x80000000
    assert read(ethernet + 0x1048) == tx_descriptor
    assert read(ethernet + 0x104C) == rx_descriptor
    assert read(ethernet + 0x1050) == tx_buffer
    assert read(ethernet + 0x1054) == rx_buffer
    write(tx_descriptor + 8, tx_buffer + 64)
    write(rx_descriptor + 8, rx_buffer + 64)
    assert read(ethernet + 0x1050) == tx_buffer
    assert read(ethernet + 0x1054) == rx_buffer
    print("PASS Ethernet current DMA descriptor and buffer registers")
