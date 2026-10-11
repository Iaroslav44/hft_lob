"""End-to-end checked MoldUDP64 ingestion with reproducible handshake stalls."""

import os
import random
import struct

import cocotb
from cocotb.triggers import Timer

SESSION = b"ABCDEFGHIJ"
MAX_SEQUENCE = (1 << 64) - 1


def add_order(order_id, timestamp, price, quantity, locate, buy=True):
    side = b"B" if buy else b"S"
    message = (b"A" + locate.to_bytes(2, "big") + b"\x12\x34" +
               timestamp.to_bytes(6, "big") + order_id.to_bytes(8, "big") +
               side + quantity.to_bytes(4, "big") + b"TEST    " +
               price.to_bytes(4, "big"))
    record = struct.pack("<QQIIHB5x", order_id, timestamp, price, quantity, locate, buy)
    return message, record


def mold_packet(sequence, messages=(), session=SESSION, control=None):
    count = len(messages) if control is None else control
    return (session + sequence.to_bytes(8, "big") + count.to_bytes(2, "big") +
            b"".join(len(message).to_bytes(2, "big") + message for message in messages))


def wire_beats(packet, bad_beats=()):
    """Attach upstream corruption markers to selected zero-based beat indices."""
    return [(int.from_bytes(packet[offset:offset + 8], "little"),
             (1 << len(packet[offset:offset + 8])) - 1, offset + 8 >= len(packet),
             offset // 8 in bad_beats)
            for offset in range(0, len(packet), 8)]


class IngressDriver:
    def __init__(self, dut, seed):
        self.dut = dut
        self.rng = random.Random(seed)
        self.force_ready = None
        self.expected = []
        self.words = []
        self.record_metadata = None
        self.held_output = None
        self.received = []
        self.accepted = 0
        self.duplicates = 0
        self.rejects = 0
        self.cycle_number = 0
        dut.clk.value = 0
        dut.rst_n.value = 0
        dut.s_axis_tvalid.value = 0
        dut.s_axis_tdata.value = 0
        dut.s_axis_tkeep.value = 0
        dut.s_axis_tlast.value = 0
        dut.s_axis_tuser.value = 0
        dut.m_axis_tready.value = 0
        dut.cfg_valid.value = 0
        dut.cfg_session.value = 0
        dut.cfg_next_sequence.value = 0

    async def cycle(self, beat=None, config=None, reset=False):
        dut = self.dut
        dut.clk.value = 0
        dut.rst_n.value = not reset
        dut.s_axis_tvalid.value = beat is not None
        dut.s_axis_tdata.value = beat[0] if beat else 0
        dut.s_axis_tkeep.value = beat[1] if beat else 0
        # An invalid LAST must never terminate the packet under reception.
        dut.s_axis_tlast.value = beat[2] if beat else self.rng.randrange(2)
        dut.s_axis_tuser.value = beat[3] if beat else 0
        ready = self.force_ready if self.force_ready is not None else self.rng.randrange(3) != 0
        dut.m_axis_tready.value = ready
        dut.cfg_valid.value = config is not None
        if config is not None:
            dut.cfg_session.value = int.from_bytes(config[0], "big")
            dut.cfg_next_sequence.value = config[1]
        await Timer(4, unit="ns")
        input_accepted = beat is not None and bool(int(dut.s_axis_tready.value))
        config_accepted = config is not None and bool(int(dut.cfg_ready.value))
        output_valid = bool(int(dut.m_axis_tvalid.value))
        output = None
        if output_valid:
            output = (int(dut.m_axis_tdata.value), int(dut.m_axis_tkeep.value),
                      int(dut.m_axis_tlast.value), int(dut.m_order_session.value),
                      int(dut.m_order_sequence.value), int(dut.m_order_packet_last.value))
        if reset:
            assert not output_valid and not int(dut.s_axis_tready.value)
            assert not int(dut.cfg_ready.value)
            self.held_output = None
            self.words.clear()
            self.record_metadata = None
        else:
            if self.held_output is not None:
                assert output_valid and output == self.held_output, "Output changed while stalled"
            self.held_output = output if output_valid and not ready else None
            if output_valid and ready:
                data, keep, last, session, sequence, packet_last = output
                assert keep == 0xFF, "Canonical records use all byte lanes"
                metadata = (session, sequence, packet_last)
                if not self.words:
                    self.record_metadata = metadata
                assert metadata == self.record_metadata, "Record metadata changed between words"
                self.words.append(data)
                assert last == (len(self.words) == 4), "Incorrect canonical record boundary"
                if last:
                    record = (b"".join(word.to_bytes(8, "little") for word in self.words),
                              *metadata)
                    assert self.expected, f"Unexpected record at cycle {self.cycle_number}: {record}"
                    assert record == self.expected.pop(0), f"Record mismatch at cycle {self.cycle_number}"
                    self.received.append(record)
                    self.words.clear()
                    self.record_metadata = None
        dut.clk.value = 1
        await Timer(6, unit="ns")
        if not reset:
            self.accepted += int(dut.accepted_pulse.value)
            self.duplicates += int(dut.duplicate_pulse.value)
            self.rejects += int(dut.decoder_reject.value)
        self.cycle_number += 1
        return input_accepted, config_accepted

    async def reset(self):
        self.expected.clear()
        for _ in range(3):
            await self.cycle(reset=True)
        await self.cycle()
        assert int(self.dut.recovery_needed.value) == 1
        assert int(self.dut.fault_code.value) == 1
        assert int(self.dut.idle.value) == 1

    async def configure(self, sequence, session=SESSION, competing_beat=None):
        assert int(self.dut.idle.value), "Administrative rearm requires drained input and output"
        accepted_input, accepted_config = await self.cycle(
            beat=competing_beat, config=(session, sequence))
        assert accepted_config and not accepted_input, "Configuration must win at idle"
        assert not int(self.dut.recovery_needed.value)
        assert int(self.dut.fault_code.value) == 0
        assert int(self.dut.expected_sequence.value) == sequence

    async def send_beats(self, beats):
        for beat in beats:
            for _ in range(self.rng.randrange(4)):
                await self.cycle()
            for _ in range(20000):
                accepted, _ = await self.cycle(beat)
                if accepted:
                    break
            else:
                raise AssertionError("Input handshake deadline exceeded")

    async def send(self, packet, bad_beats=()):
        await self.send_beats(wire_beats(packet, bad_beats))

    async def drain(self):
        for _ in range(50000):
            await self.cycle()
            if int(self.dut.idle.value) and not int(self.dut.m_axis_tvalid.value):
                assert not self.expected and not self.words, "Missing output at idle"
                return
        raise AssertionError("Drain deadline exceeded")

    def expect(self, record, sequence, packet_last, session=SESSION):
        self.expected.append((record, int.from_bytes(session, "big"), sequence, packet_last))

    def fault(self, code, expected_sequence):
        assert int(self.dut.recovery_needed.value)
        assert int(self.dut.fault_code.value) == code
        assert int(self.dut.expected_sequence.value) == expected_sequence


@cocotb.test(timeout_time=10, timeout_unit="ms")
async def mold_ingress_transactions(dut):
    seed = int(os.environ.get("HFT_RTL_SEED", "20261011"))
    dut._log.info("Mold ingress handshake seed: %d", seed)
    driver = IngressDriver(dut, seed)
    first, first_record = add_order(0, 0x010203040506, 0, 1, 0x1234)
    second, second_record = add_order(0xFEDCBA9876543210, 0xFFFFFFFFFFFF,
                                    0xF1234567, 0x87654321, 0xFEDC, False)
    packet = mold_packet(100, (first, b"S\x00\x01", b"", second))
    await driver.reset()
    await driver.configure(100, competing_beat=wire_beats(packet)[0])
    driver.expect(first_record, 100, 0)
    driver.expect(second_record, 103, 1)
    # The first beat stalled behind configuration must remain valid until accepted.
    accepted, _ = await driver.cycle(wire_beats(packet)[0])
    assert accepted, "Idle ingress did not accept the held first beat after configuration"
    await driver.send_beats(wire_beats(packet)[1:])
    await driver.drain()
    assert int(dut.expected_sequence.value) == 104
    assert driver.accepted == 1 and driver.duplicates == 0

    # Exact stale packets are discarded without affecting the committed sequence.
    await driver.send(packet)
    await driver.drain()
    assert driver.duplicates == 1 and driver.accepted == 1
    assert not int(dut.recovery_needed.value)
    assert int(dut.expected_sequence.value) == 104

    await driver.send(mold_packet(105, (first,)))
    await driver.drain()
    driver.fault(4, 104)
    assert int(dut.fault_sequence.value) == 105
    await driver.send(mold_packet(104, (first,)))
    await driver.drain()
    driver.fault(4, 104)

    # A checkpoint request during a partial packet must not rearm the controller.
    pending = wire_beats(mold_packet(104, (first,)))
    await driver.send_beats(pending[:1])
    assert not int(dut.idle.value)
    _, configured = await driver.cycle(config=(SESSION, 200))
    assert not configured
    driver.fault(4, 104)
    await driver.send_beats(pending[1:])
    await driver.drain()

    malformed_side = bytearray(second)
    malformed_side[19] = ord("X")
    malformed_quantity = bytearray(second)
    malformed_quantity[20:24] = b"\0\0\0\0"
    malformed_packets = [
        mold_packet(200, (first, bytes(malformed_side))),
        mold_packet(200, (first, bytes(malformed_quantity))),
        mold_packet(200, (first, b"A")),
        mold_packet(200, (first,))[:-1],
        SESSION + (200).to_bytes(8, "big") + b"\0\1\0",
        mold_packet(200, (first,)) + b"\0",
    ]
    for malformed in malformed_packets:
        await driver.configure(200)
        before = len(driver.received)
        await driver.send(malformed)
        await driver.drain()
        driver.fault(2, 200)
        assert len(driver.received) == before, "Rejected packet leaked an earlier valid A"

    # A missing interior lane rejects the entire packet.
    await driver.configure(200)
    malformed_keep = wire_beats(mold_packet(200, (first,)))
    data, keep, last, user = malformed_keep[1]
    malformed_keep[1] = (data, keep & ~0x04, last, user)
    await driver.send_beats(malformed_keep)
    await driver.drain()
    driver.fault(2, 200)

    # Upstream corruption quarantines a complete packet, including earlier A blocks.
    flagged_packet = mold_packet(200, (first, second))
    final_beat = (len(flagged_packet) - 1) // 8
    # Beat 3 follows the complete header, so the reported sequence remains defined.
    for bad_beat in (final_beat, 3):
        await driver.configure(200)
        before = len(driver.received)
        accepted_before = driver.accepted
        await driver.send(flagged_packet, bad_beats=(bad_beat,))
        await driver.drain()
        driver.fault(2, 200)
        assert int(dut.fault_sequence.value) == 200
        assert driver.accepted == accepted_before, "Corrupt packet advanced the checkpoint"
        assert len(driver.received) == before, "Upstream corruption leaked an order"

    # The packet validator diagnoses overflow before the session gate sees it.
    await driver.configure(MAX_SEQUENCE - 1)
    await driver.send(mold_packet(MAX_SEQUENCE - 1, (first, second)))
    await driver.drain()
    driver.fault(2, MAX_SEQUENCE - 1)

    await driver.configure(MAX_SEQUENCE)
    await driver.send(mold_packet(MAX_SEQUENCE, control=0))
    await driver.drain()
    assert not int(dut.recovery_needed.value)
    assert int(dut.expected_sequence.value) == MAX_SEQUENCE
    await driver.send(mold_packet(MAX_SEQUENCE, control=0xFFFF))
    await driver.drain()
    driver.fault(7, MAX_SEQUENCE)

    await driver.configure(400)
    accepted_before = driver.accepted
    await driver.send(mold_packet(400, control=0))
    await driver.drain()
    assert driver.accepted == accepted_before
    assert int(dut.expected_sequence.value) == 400
    assert not int(dut.recovery_needed.value)
    await driver.send(mold_packet(399, control=0))
    await driver.drain()
    assert driver.duplicates == 2
    driver.expect(first_record, 400, 1)
    await driver.send(mold_packet(400, (first,)))
    await driver.drain()
    await driver.send(mold_packet(401, control=0xFFFF))
    await driver.drain()
    driver.fault(7, 401)

    # All ten session bytes are significant, including the leading two.
    await driver.configure(500)
    await driver.send(mold_packet(500, (first,), session=b"K" + SESSION[1:]))
    await driver.drain()
    driver.fault(3, 500)

    await driver.configure(600)
    await driver.send(mold_packet(599, (first, second)))
    await driver.drain()
    driver.fault(5, 600)

    # The default 2048-byte store must quarantine an oversized opaque message.
    await driver.configure(600)
    await driver.send(mold_packet(600, (b"S" + bytes(2048),)))
    await driver.drain()
    driver.fault(2, 600)

    # Packet-last describes the original block, not the last surviving A record.
    await driver.configure(600)
    driver.expect(first_record, 600, 0)
    await driver.send(mold_packet(600, (first, b"S")))
    await driver.drain()
    assert int(dut.expected_sequence.value) == 602

    # Hold an unconsumed record, reject rearm, then reset the entire transaction.
    await driver.configure(500)
    driver.force_ready = False
    driver.expect(second_record, 500, 1)
    await driver.send(mold_packet(500, (second,)))
    for _ in range(20000):
        await driver.cycle()
        if int(dut.m_axis_tvalid.value):
            break
    else:
        raise AssertionError("Expected output was never offered under backpressure")
    for _ in range(8):
        _, configured = await driver.cycle(config=(SESSION, 900))
        assert not configured
    received_before = len(driver.received)
    await driver.reset()
    driver.force_ready = None
    await driver.drain()
    assert len(driver.received) == received_before
    await driver.configure(500)
    driver.expect(second_record, 500, 1)
    await driver.send(mold_packet(500, (second,)))
    await driver.drain()
    assert int(dut.expected_sequence.value) == 501
    assert driver.rejects == 0, "Packet validation must prevent malformed A replay"
