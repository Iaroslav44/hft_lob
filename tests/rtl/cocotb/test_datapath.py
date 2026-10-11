"""Independent cycle and frame scoreboards for the standalone RTL contracts."""

import os
import random
import struct

import cocotb
from cocotb.clock import Clock
from cocotb.triggers import ClockCycles, Timer, with_timeout
from cocotbext.axi import AxiStreamBus, AxiStreamFrame, AxiStreamMonitor, AxiStreamSource


def rng_for(dut, offset):
    seed = int(os.environ.get("HFT_RTL_SEED", "20261011")) + offset
    dut._log.info("Reference-model seed: %d", seed)
    return random.Random(seed)


def initialise(dut):
    dut.clk.value = 0
    dut.rst_n.value = 0
    for prefix in ("a", "b", "mold", "sbe", "net", "loop"):
        for signal in ("tdata", "tvalid", "tlast"):
            getattr(dut, f"{prefix}_axis_{signal}").value = 0
    for prefix in ("mold", "sbe", "net"):
        getattr(dut, f"{prefix}_axis_tkeep").value = 0


async def tick(dut):
    dut.clk.value = 0
    await Timer(5, unit="ns")
    dut.clk.value = 1
    await Timer(5, unit="ns")


class ArbiterModel:
    """Classify each ingress frame as unstarted, admitted or discarded."""

    def __init__(self):
        self.state = ["boundary", "boundary"]

    def step(self, beats, reset=False):
        if reset:
            self.state = ["boundary", "boundary"]
            return None
        chosen = None
        for lane in (0, 1):
            if self.state[lane] == "admitted":
                chosen = lane
                break
        if chosen is None:
            for lane in (0, 1):
                if beats[lane][0] and self.state[lane] == "boundary":
                    chosen = lane
                    break
        result = None
        for lane, (valid, data, last) in enumerate(beats):
            if valid:
                admitted = lane == chosen
                if admitted:
                    result = (data, last)
                self.state[lane] = "boundary" if last else (
                    "admitted" if admitted else "discarded")
        return result


@cocotb.test(timeout_time=100, timeout_unit="us")
async def arbiter_cycle_model(dut):
    initialise(dut)
    rng = rng_for(dut, 0)
    model = ArbiterModel()
    directed = [
        ((1, 0xA0, 0), (0, 0, 0)),
        ((1, 0xA1, 1), (1, 0xB0, 0)),
        ((0, 0, 0), (1, 0xB1, 1)),
        ((0, 0, 0), (1, 0xB2, 1)),
    ]
    for cycle in range(3000):
        reset = cycle == 0 or cycle % 197 == 0
        beats = directed[cycle - 1] if 1 <= cycle <= len(directed) else tuple(
            (rng.randrange(4) != 0, rng.getrandbits(64), rng.randrange(4) == 0)
            for _ in range(2)
        )
        dut.rst_n.value = not reset
        for prefix, (valid, data, last) in zip(("a", "b"), beats):
            getattr(dut, f"{prefix}_axis_tvalid").value = valid
            getattr(dut, f"{prefix}_axis_tdata").value = data
            getattr(dut, f"{prefix}_axis_tlast").value = last
        expected = model.step(beats, reset)
        await tick(dut)
        context = f"cycle={cycle}, inputs={beats}, states={model.state}"
        assert int(dut.out_axis_tvalid.value) == (expected is not None), context
        assert int(dut.out_axis_tlast.value) == (expected[1] if expected else 0), context
        if expected:
            assert int(dut.out_axis_tdata.value) == expected[0], context
        if reset:
            assert int(dut.out_axis_tdata.value) == 0, context


def pause_pattern(rng):
    while True:
        yield rng.randrange(4) == 0


@cocotb.test(timeout_time=200, timeout_unit="us")
async def axi_whole_frame_integrity(dut):
    initialise(dut)
    rng = rng_for(dut, 1)
    clock = cocotb.start_soon(Clock(dut.clk, 10, unit="ns").start())
    sources = [
        AxiStreamSource(AxiStreamBus.from_prefix(dut, f"{lane}_axis"),
                        dut.clk, dut.rst_n, reset_active_level=False)
        for lane in ("a", "b")
    ]
    monitor = AxiStreamMonitor(AxiStreamBus.from_prefix(dut, "out_axis"),
                              dut.clk, dut.rst_n, reset_active_level=False)
    for lane, source in enumerate(sources):
        source.set_pause_generator(pause_pattern(random.Random(rng.getrandbits(64))))
    await ClockCycles(dut.clk, 4)
    dut.rst_n.value = 1
    await ClockCycles(dut.clk, 2)
    submitted = set()
    for sequence in range(128):
        for lane, source in enumerate(sources):
            # Full 64-bit words avoid inventing byte qualifiers absent from the DUT.
            words = [((lane + 1) << 60) | (sequence << 16) | i
                     for i in range(rng.randrange(1, 10))]
            payload = b"".join(word.to_bytes(8, "little") for word in words)
            submitted.add(payload)
            await source.send(AxiStreamFrame(payload))
    for source in sources:
        await with_timeout(source.wait(), 100, "us")
    await ClockCycles(dut.clk, 4)
    received = set()
    while not monitor.empty():
        frame = bytes(monitor.recv_nowait().tdata)
        assert frame in submitted, f"Fragment, corruption or merged packet: {frame.hex()}"
        assert frame not in received, f"Duplicated frame: {frame.hex()}"
        received.add(frame)
    assert received, "The test must observe at least one admitted frame"
    assert monitor.current_frame is None, "Output ended in a partial frame"
    clock.cancel()


class HeaderModel:
    """Accumulate wire bytes independently of the DUT's field registers."""

    def __init__(self, kind):
        self.kind = kind
        self.width = 32 if kind == "mold" else 8
        self.required = {"mold": 20, "sbe": 8, "net": 42}[kind]
        self.metadata = (0, 0, 0)
        self.frame = bytearray()
        self.present = []
        self.checked = False
        self.accepted = 0
        self.rejected = 0

    def reset(self):
        self.metadata = (0, 0, 0)
        self.frame.clear()
        self.present.clear()
        self.checked = False

    def decode(self):
        b = self.frame
        if not all(self.present[:self.required]):
            return None
        if self.kind == "mold":
            return (int.from_bytes(b[:10], "big"), int.from_bytes(b[10:18], "big"),
                    int.from_bytes(b[18:20], "big"))
        if self.kind == "sbe":
            return struct.unpack_from("<HHH", b, 2)
        ip_length = int.from_bytes(b[16:18], "big")
        flags = int.from_bytes(b[20:22], "big")
        udp_length = int.from_bytes(b[38:40], "big")
        if (b[12:14] != b"\x08\x00" or b[14] != 0x45 or b[23] != 17 or
                flags & 0xBFFF or ip_length < 28 or
                not 8 <= udp_length <= ip_length - 20):
            return None
        return (int.from_bytes(b[26:30], "big"), int.from_bytes(b[30:34], "big"),
                udp_length)

    def step(self, beat):
        valid, data, keep, last = beat
        emitted = False
        if valid:
            if not self.checked:
                self.frame.extend(data.to_bytes(self.width, "little"))
                self.present.extend(bool(keep & (1 << i)) for i in range(self.width))
                if len(self.frame) >= self.required:
                    self.checked = True
                    decoded = self.decode()
                    if decoded is None:
                        self.rejected += 1
                    else:
                        self.metadata = decoded
                        self.accepted += 1
                        emitted = True
            if last:
                self.frame.clear()
                self.present.clear()
                self.checked = False
        return emitted, self.metadata


class HeaderTraffic:
    def __init__(self, kind, rng):
        self.kind = kind
        self.rng = rng
        self.width = 32 if kind == "mold" else 8
        self.pending = []

    def packet(self):
        rng = self.rng
        if self.kind == "mold":
            header = (rng.getrandbits(80).to_bytes(10, "big") +
                      rng.getrandbits(64).to_bytes(8, "big") +
                      rng.choice((0, 0xFFFF, rng.randrange(0x10000))).to_bytes(2, "big"))
        elif self.kind == "sbe":
            header = struct.pack("<HHHH", *(rng.randrange(0x10000) for _ in range(4)))
        else:
            header = bytearray(rng.getrandbits(8) for _ in range(42))
            header[12:14] = b"\x08\x00"
            header[14] = 0x45
            udp_length = rng.randrange(8, 65516)
            header[16:18] = (udp_length + 20).to_bytes(2, "big")
            header[20:22] = rng.choice((0, 0x4000)).to_bytes(2, "big")
            header[23] = 17
            header[38:40] = udp_length.to_bytes(2, "big")
            mutation = rng.randrange(9)
            if mutation == 1:
                header[12:14] = b"\x81\x00"
            elif mutation == 2:
                header[14] = 0x46
            elif mutation == 3:
                header[23] = 6
            elif mutation == 4:
                header[20:22] = rng.choice((1, 0x2000, 0x8000)).to_bytes(2, "big")
            elif mutation == 5:
                header[16:18] = rng.randrange(28).to_bytes(2, "big")
            elif mutation == 6:
                header[38:40] = rng.randrange(8).to_bytes(2, "big")
            elif mutation == 7:
                header[38:40] = (udp_length + 1).to_bytes(2, "big")
        payload = bytes(rng.getrandbits(8) for _ in range(rng.randrange(0, 400)))
        packet = bytes(header) + payload
        if rng.randrange(8) == 0:
            packet = packet[:rng.randrange(1, len(header))]
        beats = []
        for offset in range(0, len(packet), self.width):
            part = packet[offset:offset + self.width]
            keep = (1 << len(part)) - 1
            beats.append([1, int.from_bytes(part, "little"), keep,
                          offset + self.width >= len(packet)])
        if rng.randrange(4) == 0:
            missing = rng.randrange(len(header))
            beat_index, lane = divmod(missing, self.width)
            if beat_index < len(beats):
                beats[beat_index][2] &= ~(1 << lane)
        return beats

    def next(self):
        if self.rng.randrange(4) == 0:
            # Invalid beats deliberately include spurious LAST and arbitrary byte masks.
            return (0, self.rng.getrandbits(self.width * 8),
                    self.rng.getrandbits(self.width), self.rng.randrange(2))
        if not self.pending:
            self.pending = self.packet()
        return self.pending.pop(0)


@cocotb.test(timeout_time=200, timeout_unit="us")
async def parser_wire_models(dut):
    initialise(dut)
    rng = rng_for(dut, 2)
    models = {kind: HeaderModel(kind) for kind in ("mold", "sbe", "net")}
    traffic = {kind: HeaderTraffic(kind, random.Random(rng.getrandbits(64)))
               for kind in models}
    outputs = {
        "mold": (dut.mold_valid, (dut.mold_session, dut.mold_sequence, dut.mold_count)),
        "sbe": (dut.sbe_valid, (dut.sbe_template, dut.sbe_schema, dut.sbe_version)),
        "net": (dut.net_valid, (dut.net_src, dut.net_dst, dut.net_length)),
    }
    for cycle in range(10000):
        reset = cycle == 0 or cycle % 503 == 0
        dut.rst_n.value = not reset
        expected = {}
        for kind, model in models.items():
            if reset:
                model.reset()
                traffic[kind].pending.clear()
                beat = (0, 0, 0, 0)
                expected[kind] = (False, (0, 0, 0))
            else:
                beat = traffic[kind].next()
                expected[kind] = model.step(beat)
            for suffix, value in zip(("tvalid", "tdata", "tkeep", "tlast"), beat):
                getattr(dut, f"{kind}_axis_{suffix}").value = value
        await tick(dut)
        for kind, (valid_signal, fields) in outputs.items():
            actual = (bool(int(valid_signal.value)), tuple(int(field.value) for field in fields))
            assert actual == expected[kind], f"{kind}: cycle={cycle}, expected={expected[kind]}, actual={actual}"
    for kind, model in models.items():
        assert model.accepted > 0 and model.rejected > 0, f"{kind}: insufficient scenario coverage"


@cocotb.test(timeout_time=20, timeout_unit="us")
async def loopback_reset_contract(dut):
    initialise(dut)
    rng = rng_for(dut, 3)
    for _ in range(1000):
        data, valid, last, reset_n = rng.getrandbits(512), rng.randrange(2), rng.randrange(2), rng.randrange(2)
        dut.loop_axis_tdata.value = data
        dut.loop_axis_tvalid.value = valid
        dut.loop_axis_tlast.value = last
        dut.rst_n.value = reset_n
        await Timer(1, unit="ns")
        assert int(dut.loop_out_tdata.value) == data
        assert int(dut.loop_out_tlast.value) == last
        assert int(dut.loop_out_tvalid.value) == (valid and reset_n)
