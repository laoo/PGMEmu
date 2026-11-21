---
name: pgmemu
description: Boot, run, inspect and debug games on PGMEmu, the IGS PGM arcade emulator in this repository, and compare it with the MiSTer core's RTL simulation. Use it to load a game or the BIOS headless, run frames, stop at breakpoints or watchpoints, read memory, registers, the sprite list or the sound voices, take screenshots, save and load states, drive PGMTest pages, or check the emulator against the RTL.
---

# Driving PGMEmu

PGMEmu answers a JSON control protocol (`docs/spec/control-protocol.md`, every method with its
parameters). Run it headless as a server that keeps its state, and send it one request per
command with `scripts/pgmemu.py`. Work from the repository root.

## Start it

Build once, then start the server in the background (Bash with `run_in_background`):

```sh
cmake --build --preset release
build/release/src/pgm_cli/pgmemu-cli --server tcp:7701 --bios ../ROMS/pgm.zip --rom-dir roms --state-dir build
```

Games are `roms/<set>.pgm` (build them with `scripts/make-pgm.sh` if `roms/` is empty); `pgm` is
the BIOS alone. A game may follow the flags, loaded before the first request: `... --state-dir
build orlegend`. When done, stop the one you started, by its port:
`pkill -f "pgmemu-cli --server tcp:7701"`.

If 7701 is taken, pick another port and pass it to every request as `--port PORT`, written out on
each call (zsh does not split a command kept in a variable).

A person may have the desktop application open instead, started with `--server tcp:7701`: then
the same requests drive the game on their screen, and you must not start a second server. The
window runs the game by itself between your requests: read what you need right after the run that
stops where you want it, or `emu.pause` it first (`emu.resume` when done). A breakpoint or a
watchpoint its own run reaches pauses it, and `emu.status` then says where it stopped.

To have the emulator as MCP tools in your own sessions instead, with screenshots shown as
images, register it for yourself (it is not committed, as each session would then run the binary
of its start, and rebuilds are frequent here):
`claude mcp add --scope local pgmemu -- "$PWD/build/release/src/pgm_cli/pgmemu-cli" --mcp --bios "$PWD/../ROMS/pgm.zip" --rom-dir "$PWD/roms"`.
Reconnect it (`/mcp`) after a rebuild.

## Send requests

```sh
scripts/pgmemu.py emu.load_game name=orlegend
scripts/pgmemu.py emu.run_frames count=600
scripts/pgmemu.py video.screenshot path=build/shot.png     # then Read build/shot.png to see it
```

Parameters are `key=value` (JSON values; `0x...` is hex) or one JSON object. Over MCP, or in
raw JSON, an integer may be a string `"0x1478"`. The answer is the result as JSON; an error goes to stderr with exit status 1. Numbers in answers are decimal:
`"pc": 1050780` is 0x10089c.

## What to ask for

| To | Methods |
|---|---|
| Load, reset, see where it is | `emu.load_game {name, region}`, `emu.reset {cycles: 100}`, `emu.status`, `emu.cartridge_info` |
| Region | `emu.set_region {region}`: a code from `emu.cartridge_info`'s `region_info`, such as `JAPN`; powers up anew |
| Run | `emu.run_frames {count}` (60 is a second), `emu.run_cycles {count}` (50 MHz master ticks), `emu.step {count}` (68000 instructions; answers the next `pc` and `disasm`), `emu.run_until {condition, timeout_cycles}` (a PC, a raster signal, or `{type: memory_equals, address, value, size}`) |
| Stop at code | `debug.breakpoint.add/remove/list {address, cpu}` (`m68k` by default, `z80`, `arm7`): the 68000 stops before that instruction; for the Z80 or the ARM7 the run stops just after, and the answer's `breakpoint.registers` are theirs at the breakpoint |
| Stop at data | `debug.watchpoint.add {address, size, access: read/write/access}`: a run stops after the instruction, and its answer says what was accessed and from where |
| See what ran | `debug.trace {count, from, registers}`: the last instructions, with disassembly. `debug.trace.find {pc, pc_start, pc_end, where, disasm_contains, from, limit}` searches them on the emulator's side. `debug.trace.configure {instructions, registers}` keeps more than the 4096 it keeps unless told (a frame is some tens of thousands), and the registers before each, which `registers` and `where` need |
| CPUs | `cpu.get_state` (68000), `cpu.get_state {cpu: z80}`, `cpu.get_state {cpu: arm7}` (the IGS027A's), `cpu.disassemble {address, count, cpu}` |
| Memory | `memory.read {address, size}` and `memory.write {address, data_hex}` take the 68000's own addresses when no region is named (work RAM is 0x800000-0x81FFFF, VRAM 0x900000, the palette 0xA00000), in 68000 byte order. With `region`, a name from `memory.list_regions`, `address` is an offset in it: the ROMs as their files hold them, and `AUDIO_RAM`, the Z80's |
| Picture | `video.screenshot {path, scale}`, `video.sprites`, `video.registers`, `video.layers {text, background, sprites}`, `video.tiles {layer, first, count, palette, path}`, `video.tilemap {layer, path}` |
| Sound | `audio.voices`, `audio.capture_start {path}` / `audio.capture_stop` (a WAV) |
| Input | `input.press {name}` (up, down, left, right, button1-4, start, coin: held two frames, released two), or `input.press {buttons: [...], frames, release_frames}` for several at once and for longer; `input.set` / `input.clear` hold one and let it go, `input.hold {buttons: [...]}` holds exactly those listed |
| State | `state.save {filename}`, `state.load {filename}`, `state.list`, `nvram.save/load` |
| PGMTest | `test.status` (a page's status block), `debug_link.start/write/read/stop` |
| The window | `emu.pause`, `emu.resume`: the game on screen stops running by itself |

A run answers `reason`: `completed`, `breakpoint`, `watchpoint`, `condition_met`, `timeout` or
`halted`, and with `breakpoint` an object saying which CPU and where. After a 68000 breakpoint,
the next run executes that instruction before stopping again; a breakpoint in code that runs every
frame stops every run within a frame, so remove it (`debug.breakpoint.remove`) to run on. A Z80
or ARM7 breakpoint in a loop does the same. While either has one, runs are about 40 percent
slower.

`video.sprites` answers `{count, sprites}` in list order, a later sprite drawn over an earlier
one. `x` (11 bits) and `y` (10 bits) are raw and wrap: x 1508 is off screen. `width` is in units of
16 pixels, `height` in lines, `scale_x` and `scale_y` are 16 for actual size, and `mask_address`
is a word address in the B ROM.

### Finding an address to stop at

Every game runs its vertical-blank handler each frame, and the BIOS finds it through work RAM:
its level-6 vector leads to `move.l $801478,-(A7); rts`. So, once a game runs, the long at
0x801478 (`memory.read address=0x801478 size=4`; work RAM reads in 68000 order) is the game's
handler, a good first breakpoint. Or run a few frames and read
`debug.trace` to see the code that is running, then `cpu.disassemble` around it.

The ROM regions (`BIOS_PROG_ROM`, `CART_PROG_ROM`) read in their files' order, each 16-bit word's
low byte first: `0000be0c` there is the long 0x00000cbe.

### Example: stop in a game and look at it

```sh
scripts/pgmemu.py emu.load_game name=orlegend
scripts/pgmemu.py emu.run_frames count=2400            # past the warning screen, into the attract mode
scripts/pgmemu.py memory.read address=0x801478 size=4   # the vblank handler: 0010089c
scripts/pgmemu.py debug.breakpoint.add address=0x10089c
scripts/pgmemu.py emu.run_frames count=60              # reason: breakpoint
scripts/pgmemu.py cpu.get_state
scripts/pgmemu.py video.sprites
scripts/pgmemu.py video.screenshot path=build/orlegend.png
scripts/pgmemu.py debug.breakpoint.remove address=0x10089c
```

orlegend shows a warning screen for its first 1000 frames or so; its attract mode is running by
frame 2400.

## Comparing with the RTL

The reference is the MiSTer core's Verilator simulation in `../Arcade-IGSPGM_MiSTer/sim`, which
speaks the same protocol. `scripts/compare-with-rtl.py` drives both and reports where memory,
pictures and sound differ:

```sh
scripts/compare-with-rtl.py --game orlegend --frames 300 --pictures
scripts/compare-with-rtl.py --frames 800 --regions AUDIO_RAM --audio
scripts/compare-with-rtl.py --program build/tools/pgmtest-<page>/pgm/pgm_p02s.u20 --frames 120 --press down@60
```

The simulation runs at about 1.4 frames a second: 600 frames take seven minutes, so start it in
the background. Bytes below a stack pointer (dead stack) differ wherever an interrupt arrived at
another instruction; leave them out with `--ignore WORK_RAM:1f000-20000`.
`docs/hardware/differences.md` lists what is known to differ, and why.

## Things that trip people up

- Time is counted in master ticks of 50 MHz. A frame is 844,800 of them.
- `video.screenshot` without `path` returns base64; give a path and Read the file to see it.
- `cpu.get_state {cpu: z80}` can catch the Z80 inside an instruction: its `pc` may point at an
  operand byte, not at an instruction. To stop at Z80 code, take an address from a disassembly of
  its RAM (`memory.read region=AUDIO_RAM`), not from that `pc`.
- A save state belongs to the build and the game that wrote it.
- Requests are answered one at a time; a long `emu.run_frames` holds the others back.
