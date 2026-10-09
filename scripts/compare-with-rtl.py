#!/usr/bin/env python3
"""Runs a game on the emulator and on the RTL simulation side by side, and
compares their memory at chosen frames.

    scripts/compare-with-rtl.py [--game pgm] [--frames 60 600] [--regions WORK_RAM ...]

Both programs are driven over the same JSON-lines protocol
(docs/spec/control-protocol.md): one script, two servers, the same requests.
Each checkpoint reads every region from both and prints where they differ, as
address ranges. The exit status is 0 when every region matched at every
checkpoint.

The simulation runs at about 1.4 frames per second, so 600 frames take some
seven minutes. Its zips come from ../ROMS; the emulator's images from roms/,
built by scripts/make-pgm.sh. `pgm` is the BIOS alone.

--ignore REGION:START-END leaves a range of a region out of the comparison:
dead stack, for one, which holds whatever interrupts pushed and differs
wherever an interrupt arrived at a different instruction.

--program FILE replaces the BIOS's 68000 program with FILE in both, which is
how a PGMTest page (scripts/make-pgmtest.sh) is compared: the simulator's copy
is overwritten with memory.write after loading, the emulator is given FILE's
directory as its first BIOS source.

--press NAME@FRAME presses a control on both, as input.press does (two frames
held, two released), once FRAME frames have run; the names are the
simulator's: up, down, left, right, button1, start. It may be repeated.

--bisect, when a checkpoint differs in memory, looks for where the two first
parted, between the last checkpoint that matched (or the reset) and it: the
frame, then the line, then the instruction. The simulator's save states do not
reload exactly, so each step starts both again from the reset, and a bisection
costs some three times the frames to the difference. At the end it shows both
68000s and the emulator's last instructions.

--audio records the ICS2115's output on both from the reset to the last frame,
keeps both as WAVs in build/compare/, and compares them sample by sample after
aligning them: the two can disagree by a sample or so on where a frame ends.
"""

import argparse
import functools
import json
import math
import pathlib
import subprocess
import sys
import struct
import tempfile
import time
import wave
import zlib

ROOT = pathlib.Path(__file__).resolve().parent.parent
WORKSPACE = ROOT.parent
SIM_DIR = WORKSPACE / "Arcade-IGSPGM_MiSTer" / "sim"
# Progress is printed as it happens, also when the output is a file or a pipe.
print = functools.partial(print, flush=True)  # noqa: A001

MAX_READ = 0x100000
WRITE_CHUNK = 0x4000
PROGRAM_NAME = "pgm_p02s.u20"


class Server:
    """A JSON-lines server run as a child process."""

    def __init__(self, name, command, cwd, env=None):
        self.name = name
        self.next_id = 1
        self.process = subprocess.Popen(
            command, cwd=cwd, env=env, text=True,
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)

    def call(self, method, **params):
        request = {"id": self.next_id, "method": method, "params": params}
        self.next_id += 1
        self.process.stdin.write(json.dumps(request) + "\n")
        self.process.stdin.flush()
        line = self.process.stdout.readline()
        if not line:
            sys.exit(f"{self.name} ended while answering {method}")
        response = json.loads(line)
        if not response.get("ok"):
            sys.exit(f"{self.name}: {method} failed: {response.get('error')}")
        return response["result"]

    def read(self, region, size):
        data = bytearray()
        for address in range(0, size, MAX_READ):
            chunk = min(MAX_READ, size - address)
            result = self.call("memory.read", region=region, address=address, size=chunk)
            data += bytes.fromhex(result["data_hex"])
        return bytes(data)

    def write(self, region, data):
        for address in range(0, len(data), WRITE_CHUNK):
            self.call("memory.write", region=region, address=address,
                      data_hex=data[address:address + WRITE_CHUNK].hex())

    def close(self):
        self.process.stdin.close()
        self.process.wait(timeout=30)


def decode_png(data):
    """(width, height, rows of RGB bytes) of an 8-bit RGB or RGBA PNG."""
    assert data[:8] == b"\x89PNG\r\n\x1a\n"
    at, idat = 8, b""
    while at < len(data):
        length, kind = struct.unpack(">I4s", data[at:at + 8])
        body = data[at + 8:at + 8 + length]
        if kind == b"IHDR":
            width, height, depth, colour = struct.unpack(">IIBB", body[:10])
            assert depth == 8 and colour in (2, 6), "only 8-bit RGB and RGBA are read"
            channels = 3 if colour == 2 else 4
        elif kind == b"IDAT":
            idat += body
        at += 12 + length
    raw = zlib.decompress(idat)
    stride = width * channels
    rows, previous = [], bytearray(stride)
    for y in range(height):
        kind = raw[y * (stride + 1)]
        line = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for x in range(stride):
            left = line[x - channels] if x >= channels else 0
            up = previous[x]
            corner = previous[x - channels] if x >= channels else 0
            if kind == 1:
                line[x] = (line[x] + left) & 0xff
            elif kind == 2:
                line[x] = (line[x] + up) & 0xff
            elif kind == 3:
                line[x] = (line[x] + (left + up) // 2) & 0xff
            elif kind == 4:
                guess = left + up - corner
                pa, pb, pc = abs(guess - left), abs(guess - up), abs(guess - corner)
                line[x] = (line[x] + (left if pa <= pb and pa <= pc else up if pb <= pc else corner)) & 0xff
        previous = line
        rows.append(bytes(line[i] for i in range(stride) if i % channels < 3))
    return width, height, rows


def compare_pictures(mine, theirs):
    """Differing pixels, and the rows they lie in, of two decoded pictures."""
    (width, height, left), (_, _, right) = mine, theirs
    differing, rows = 0, []
    for y in range(height):
        count = sum(1 for x in range(width) if left[y][3 * x:3 * x + 3] != right[y][3 * x:3 * x + 3])
        if count:
            differing += count
            rows.append(y)
    return differing, rows


def differing_ranges(left, right):
    """(start, end) of every run of bytes that differ."""
    ranges = []
    start = None
    for address in range(len(left)):
        if left[address] != right[address]:
            if start is None:
                start = address
        elif start is not None:
            ranges.append((start, address))
            start = None
    if start is not None:
        ranges.append((start, len(left)))
    return ranges


def read_capture_stream(data):
    """(left, right) frames of the simulator's audio capture: packets of a
    44-byte header (sim_audio_capture.h) and a payload, of which those of type
    1 hold stereo 16-bit frames."""
    frames, at = [], 0
    while at + 44 <= len(data):
        magic, _, kind, size = struct.unpack_from("<IHHI", data, at)
        assert magic == 0x414D4750, f"no packet at {at}"
        if kind == 1:
            frames += list(struct.iter_unpack("<hh", data[at + 44:at + 44 + size]))
        at += 44 + size
    return frames


def read_wav(path):
    with wave.open(str(path), "rb") as wav:
        return list(struct.iter_unpack("<hh", wav.readframes(wav.getnframes())))


def write_wav(path, frames, rate):
    with wave.open(str(path), "wb") as wav:
        wav.setnchannels(2)
        wav.setsampwidth(2)
        wav.setframerate(rate)
        wav.writeframes(b"".join(struct.pack("<hh", *frame) for frame in frames))


def compare_audio(mine, theirs, max_lag=64, window=8192):
    """A summary of how two captures differ, at the lag that matches them best.
    The lag is chosen on a window from the simulator's first sound on."""
    start = next((i for i, frame in enumerate(theirs) if frame != (0, 0)), 0)

    def matches(lag):
        pairs = zip(mine[max(start + lag, 0):start + lag + window], theirs[start:start + window])
        return sum(1 for a, b in pairs if a == b)
    lag = max(range(-max_lag, max_lag + 1), key=lambda lag: (matches(lag), -abs(lag)))
    pairs = list(zip(mine[max(lag, 0):], theirs[max(-lag, 0):]))
    differing = [i for i, (a, b) in enumerate(pairs) if a != b]
    worst = max((abs(a[c] - b[c]) for a, b in pairs for c in (0, 1)), default=0)
    loud = max((abs(v) for frame in theirs for v in frame), default=0)
    rms = math.sqrt(sum((a[c] - b[c]) ** 2 for a, b in pairs for c in (0, 1)) / max(1, 2 * len(pairs)))
    return lag, len(pairs), differing, worst, loud, rms


REGION_SIZES = {"WORK_RAM": 0x20000, "VIDEO_RAM": 0x8000, "PALETTE_RAM": 0x2000, "AUDIO_RAM": 0x10000}


MASTER_TICKS_PER_LINE = 3200
LINES_PER_FRAME = 264
INSTRUCTION_STEP_TICKS = 16


class Pair:
    """The emulator and the simulation, started alike from the reset and run in
    step, with the same controls pressed at the same frames."""

    def __init__(self, args, scratch):
        bios_sources = ["--bios", args.bios]
        if args.program:
            override = pathlib.Path(scratch) / PROGRAM_NAME
            override.write_bytes(args.program.read_bytes())
            bios_sources = ["--bios", scratch] + bios_sources
        self.emulator = Server("emulator",
                               [args.emulator, "--server", *bios_sources, "--rom-dir", str(ROOT / "roms")],
                               cwd=ROOT)
        self.simulator = Server("simulator", ["./sim", "--server"], cwd=SIM_DIR,
                                env={"PGM_ROM_DIR": str(WORKSPACE / "ROMS"), "PATH": "/usr/bin:/bin"})
        self.simulator.call("sim.initialize", headless=True)
        for server in self.servers():
            server.call("sim.load_game", name=args.game)
        if args.program:
            self.simulator.write("BIOS_PROG_ROM", args.program.read_bytes())
        for server in self.servers():
            server.call("sim.reset", cycles=100)
        self.frame = 0
        self.presses = sorted((int(at), name) for name, at in (item.split("@") for item in args.press))

    def servers(self):
        return (self.emulator, self.simulator)

    def run_to(self, frame):
        """Runs both to `frame`, pressing what is due on the way: a press holds
        for two frames and releases for two. Answers each one's last run."""
        runs = {}
        while self.presses and self.presses[0][0] <= frame:
            at, name = self.presses.pop(0)
            for server in self.servers():
                if at > self.frame:
                    server.call("sim.run_frames", count=at - self.frame)
                server.call("input.press", name=name)
            self.frame = max(self.frame, at) + 4
        if frame > self.frame:
            runs = {server.name: server.call("sim.run_frames", count=frame - self.frame)
                    for server in self.servers()}
            self.frame = frame
        return runs

    def run_ticks(self, ticks):
        for server in self.servers():
            server.call("sim.run_cycles", count=ticks)

    def differences(self, regions, ignored):
        """The differing ranges of each region, those ignored left out."""
        found = {}
        for region in regions:
            mine = self.emulator.read(region, REGION_SIZES[region])
            theirs = self.simulator.read(region, REGION_SIZES[region])
            ranges = [(start, end) for start, end in differing_ranges(mine, theirs)
                      if not any(low <= start and end <= high for low, high in ignored.get(region, []))]
            if ranges:
                found[region] = (ranges, mine, theirs)
        return found

    def close(self):
        for server in self.servers():
            server.close()


def show_differences(found, limit):
    for region, (ranges, mine, theirs) in found.items():
        differing = sum(end - start for start, end in ranges)
        print(f"  {region}: {differing} bytes differ in {len(ranges)} ranges")
        for start, end in ranges[:limit]:
            print(f"    {start:06x}-{end - 1:06x}  emulator {mine[start:min(end, start + 8)].hex()}"
                  f"  simulator {theirs[start:min(end, start + 8)].hex()}")


def bisect(args, ignored, good, bad):
    """Finds the frame, the line and the instruction at which memory first
    differs between frame `good`, where it matched, and frame `bad`."""
    regions = args.regions
    with tempfile.TemporaryDirectory() as scratch:
        print(f"bisect: frames {good} to {bad}")
        pair = Pair(args, scratch)
        pair.run_to(good)
        before = good
        while pair.frame < bad:
            before = pair.frame
            pair.run_to(pair.frame + 1)
            if pair.differences(regions, ignored):
                break
        else:
            print("bisect: no frame differs on the way; the difference comes and goes")
            pair.close()
            return
        frame = pair.frame
        pair.close()
        print(f"bisect: frame {frame} is the first to differ")

        pair = Pair(args, scratch)
        pair.run_to(before)
        lines = 0
        while True:
            pair.run_ticks(MASTER_TICKS_PER_LINE)
            lines += 1
            if pair.differences(regions, ignored) or lines >= LINES_PER_FRAME * (frame - before):
                break
        pair.close()
        print(f"bisect: {lines} lines after frame {before}, line {lines % LINES_PER_FRAME} of its frame")

        pair = Pair(args, scratch)
        pair.run_to(before)
        pair.run_ticks(MASTER_TICKS_PER_LINE * (lines - 1))
        steps = 0
        found = {}
        while steps * INSTRUCTION_STEP_TICKS <= MASTER_TICKS_PER_LINE:
            pair.run_ticks(INSTRUCTION_STEP_TICKS)
            steps += 1
            found = pair.differences(regions, ignored)
            if found:
                break
        ticks = {server.name: server.call("sim.status")["total_ticks"] for server in pair.servers()}
        print(f"bisect: memory differs {steps * INSTRUCTION_STEP_TICKS} master ticks into that line; "
              f"total ticks emulator {ticks['emulator']}, simulator {ticks['simulator']}")
        show_differences(found, args.ranges)
        for server in pair.servers():
            state = server.call("cpu.get_state")
            print(f"  {server.name} 68000: pc {state['pc']:06x}  d " +
                  " ".join(f"{value:08x}" for value in state["registers"][:8]) + "\n" + " " * 21 + "a " +
                  " ".join(f"{value:08x}" for value in state["registers"][8:15]))
        print("  the emulator's last instructions (the simulator's pc is its prefetch address):")
        for entry in pair.emulator.call("debug.trace", count=12)["instructions"]:
            print(f"    {entry['pc']:06x}  {entry['disasm']}")
        pair.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--game", default="pgm")
    parser.add_argument("--frames", type=int, nargs="+", default=[60, 600],
                        help="frames after reset to compare at, ascending")
    parser.add_argument("--regions", nargs="+", default=["WORK_RAM", "VIDEO_RAM", "PALETTE_RAM"],
                        choices=sorted(REGION_SIZES))
    parser.add_argument("--emulator", default=str(ROOT / "build" / "release" / "src" / "pgm_cli" / "pgmemu-cli"))
    parser.add_argument("--bios", default=str(WORKSPACE / "ROMS" / "pgm.zip"))
    parser.add_argument("--ranges", type=int, default=12, help="differing ranges to list per region")
    parser.add_argument("--program", type=pathlib.Path, help="a BIOS program to run instead of the BIOS's own")
    parser.add_argument("--pictures", action="store_true",
                        help="also compare the pictures, and keep both as PNGs in build/compare/")
    parser.add_argument("--ignore", action="append", default=[], metavar="REGION:START-END",
                        help="a hex byte range of a region to leave out, end exclusive; may be repeated")
    parser.add_argument("--press", action="append", default=[], metavar="NAME@FRAME",
                        help="press a control at a frame; may be repeated")
    parser.add_argument("--audio", action="store_true",
                        help="also record the sound from the reset on, and compare it")
    parser.add_argument("--bisect", action="store_true",
                        help="find the frame, line and instruction where memory first differs")
    args = parser.parse_args()

    ignored = {}
    for item in args.ignore:
        region, span = item.split(":")
        start, end = (int(part, 16) for part in span.split("-"))
        ignored.setdefault(region, []).append((start, end))

    scratch = tempfile.TemporaryDirectory()
    pair = Pair(args, scratch.name)
    emulator, simulator = pair.servers()

    label = args.program.parent.parent.name if args.program else args.game
    outputs = ROOT / "build" / "compare"
    outputs.mkdir(parents=True, exist_ok=True)
    if args.audio:
        emulator.call("audio.capture_start", path=str(outputs / f"{label}-emulator.wav"))
        simulator.call("audio_capture.start", filename=str(outputs / f"{label}-simulator.pga"))

    identical = True
    matched = 0
    first_bad = None
    for frame in args.frames:
        started = time.monotonic()
        runs = pair.run_to(frame)
        if runs:
            print(f"frame {frame}: ran in {time.monotonic() - started:.0f} s; ticks emulator "
                  f"{runs['emulator']['ticks_executed']}, simulator {runs['simulator']['ticks_executed']}")
        else:
            print(f"frame {frame}:")
        found = pair.differences(args.regions, ignored)
        for region in args.regions:
            if region not in found:
                print(f"  {region}: identical")
        show_differences(found, args.ranges)
        identical &= not found
        if found and first_bad is None:
            first_bad = frame
        elif not found and first_bad is None:
            matched = frame

        if args.pictures:
            decoded = {}
            for server in (emulator, simulator):
                path = outputs / f"{label}-{frame}-{server.name}.png"
                server.call("video.screenshot", path=str(path))
                decoded[server.name] = decode_png(path.read_bytes())
            differing, rows = compare_pictures(decoded["emulator"], decoded["simulator"])
            identical &= differing == 0
            where = f" in rows {rows[0]}-{rows[-1]}" if rows else ""
            print(f"  picture: {'identical' if not differing else f'{differing} pixels differ{where}'}")

    if args.audio:
        rate = emulator.call("audio.capture_stop")["sample_rate"]
        simulator.call("audio_capture.stop")
        mine = read_wav(outputs / f"{label}-emulator.wav")
        theirs = read_capture_stream((outputs / f"{label}-simulator.pga").read_bytes())
        write_wav(outputs / f"{label}-simulator.wav", theirs, rate)
        lag, overlap, differing, worst, loud, rms = compare_audio(mine, theirs)
        identical &= not differing
        print(f"audio: emulator {len(mine)} frames, simulator {len(theirs)}, at {rate} Hz; aligned "
              f"{lag:+d}: " + ("identical" if not differing else
                              f"{len(differing)} of {overlap} differ from {differing[0]} on, by up to {worst} "
                              f"(rms {rms:.1f}; the loudest sample is {loud})"))

    pair.close()
    if args.bisect and first_bad is not None:
        bisect(args, ignored, matched, first_bad)
    return 0 if identical else 1


if __name__ == "__main__":
    sys.exit(main())
