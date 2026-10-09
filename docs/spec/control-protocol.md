# The control protocol

This is what a client sends to the emulator and what it gets back, whatever carries the bytes.
Why there is one protocol, and why it is the RTL simulator's, is
[0005](../decisions/0005-one-control-api.md). The simulator's own description,
`../Arcade-IGSPGM_MiSTer/docs/sim-server.md`, is the baseline. This document says where the
emulator matches it and what it adds.

## 1. Framing (JSON-lines)

`pgmemu-cli --server` reads requests on stdin and writes responses on stdout.

- One request is one line holding one JSON object. One response is one line holding one JSON
  object, written in the order the requests arrived and flushed after each.
- Blank lines are skipped and are not answered.
- stdout carries responses and nothing else. Logs go to stderr.
- Serving ends when stdin ends.

`pgmemu-cli --server tcp:PORT` serves the same lines on TCP instead, on 127.0.0.1 only, until the
process is ended; requests from all connections are answered one at a time. It is how a script
or an agent keeps one emulator across many short commands (`scripts/pgmemu.py`).

`pgmemu --server tcp:PORT` serves them from the desktop application while its window is
open. Each connection is a session of its own; requests from all of them, and from the window,
are answered in turn on the emulation thread, on the machine on screen
([0012](../decisions/0012-network-transports.md)).

### MCP

`pgmemu-cli --mcp` speaks the Model Context Protocol on stdio instead: JSON-RPC 2.0 messages, one
per line. It answers `initialize`, `ping`, `tools/list` and `tools/call`.

- Every method below is a tool, named with underscores for its dots: `emu.run_frames` is
  `emu_run_frames`. The `sim.` aliases are left out. A tool's description and input schema are
  the method's, from the dispatcher's method table.
- A tool's answer is the method's `result` as JSON text. A screenshot's PNG comes first, as an
  `image`, and is taken out of the text.
- A method's failure is a tool result with `isError` set, its text the error's code and message.
  Only a call that names no tool is a JSON-RPC error.

`pgmemu --mcp-http PORT` serves MCP's Streamable HTTP transport at `http://127.0.0.1:PORT/mcp`
while its window is open. A message is the body of a POST; a request's answer is the body of the
response, as `application/json`, and a notification is answered `202 Accepted` with none. The
server streams nothing, so a GET is answered `405`. A request whose `Origin` is not a page of
`localhost` or `127.0.0.1` is answered `403`.

## 2. Requests

```json
{"id":1,"method":"emu.status","params":{}}
```

| Field | Required | Meaning |
|---|---|---|
| `id` | yes | A non-negative integer, echoed in the response. The protocol does not require ids to be unique or increasing; a client that wants to match responses keeps them so. |
| `method` | yes | The method's name. |
| `params` | no | An object. A method that takes no parameters is given `{}` when it is left out. |

## 3. Responses

A request that succeeded:

```json
{"id":1,"ok":true,"result":{"version":"devel"}}
```

A request that failed:

```json
{"id":1,"ok":false,"error":{"code":"unknown_method","message":"Unknown method: emu.nonsense"}}
```

- Keys appear in the order shown, as the simulator writes them.
- `error.code` is stable and meant for programs; `error.message` is for people and may change.
- A request whose `id` cannot be read is answered under `id` 0. That covers text that is not
  JSON, JSON that is not an object, and an `id` that is missing, negative or not an integer.
- Every request is answered. A malformed one never ends the session.

## 4. Error codes

| Code | Meaning |
|---|---|
| `bad_request` | The request is malformed: not JSON, not an object, `id` or `method` missing or of the wrong type, `params` not an object, or a parameter missing, of the wrong type or out of its bounds. |
| `unknown_method` | No method of that name exists. |
| `unknown_game` | `emu.load_game` was given a name with no `<name>.pgm` in the ROM directory, or a name that is not a set name. |
| `load_failed` | A game could not be loaded: the file is not a valid `.pgm` ([pgm-format.md](pgm-format.md)), or the BIOS is missing or wrong. The message names the file and the fault. |
| `unknown_region` | A region was asked for that the game cannot run as: not a four-character code, not among the image's regions, or without the data its protection needs. The message lists the regions there are. |
| `no_cartridge` | The method needs a cartridge, and none is loaded. |
| `not_loaded` | The method needs a running machine, and no game is loaded. |
| `screenshot_failed` | The picture could not be encoded or written. |
| `invalid_signal` | A condition names a signal the emulator does not have (§6, `emu.run_until`). |
| `no_cpu` | The CPU asked for is not on the loaded board: the ARM7 of a game without an IGS027A. |
| `invalid_region` | No memory region of that name holds anything now. |
| `invalid_range` | The bytes asked for run past the end of the region. |
| `capture_running` | `audio.capture_start` while a capture is running. |
| `capture_not_running` | `audio.capture_stop` with no capture running. |
| `capture_failed` | The capture's file could not be written. |
| `invalid_input` | An `input.` method was given a name it does not know. |
| `state_failed` | A save state could not be written or read. |
| `state_mismatch` | A save state is not one of the game loaded, or not of this build's format. |
| `nvram_failed` | An NVRAM file could not be written, or is not 128 KB. |
| `debug_link_timeout` | The test ROM did not publish its debug link, or did not take the bytes, in the time given. |

`unknown_method`, `unknown_game`, `load_failed`, `invalid_region`, `invalid_signal`,
`invalid_input` and `bad_request` mean what the simulator means by them. `no_cartridge`, `not_loaded`,
`invalid_range`, `unknown_region`, `no_cpu` and the `capture_` codes are the emulator's own: the simulator does not check a
range, always has a machine, and captures sound by other methods (§6, `audio.capture_start`).

## 5. Names shared with the simulator

Every `sim.<name>` method the emulator implements is an alias of `emu.<name>`. The alias answers
exactly what the `emu.` method answers, so a script written against `./sim --server` runs
unchanged.

## 6. Methods

### `emu.status` (alias `sim.status`)

Takes no parameters.

```json
{"id":1,"ok":true,"result":{"version":"devel","game_name":"orlegend","region":"WRLD","total_ticks":844900,"frame":1}}
```

| Field | Meaning |
|---|---|
| `version` | The emulator's version: the release tag it was built from, or `devel`. |
| `game_name` | The short name of the loaded cartridge, `pgm` when the BIOS alone is loaded, or `null` before anything is. |
| `region` | The code of the region the game runs as: the one it was loaded or set with, or the one its image holds. `null` when the image has no regions, or holds a value its table does not name. |
| `total_ticks` | Master ticks (50 MHz) since the machine was powered up by loading the game; absent before a game is loaded. |
| `frame` | Frame boundaries passed since then (`emu.run_frames`); absent before a game is loaded. |

The simulator's other status fields (`running`, ...) are added as the state they describe comes
to exist, under the simulator's names.

### `emu.load_game` (alias `sim.load_game`)

Loads a game and the BIOS. Exactly one of `name` and `path` is given:

| Param | Meaning |
|---|---|
| `name` | A set name: `<name>.pgm` is taken from the ROM directory. `pgm` loads the BIOS alone. A name holds only letters, digits and underscores. |
| `path` | The path of a `.pgm` file. |
| `region` | Optional: the region the game runs as, one of the codes `emu.cartridge_info` lists under `region_info`, such as `JAPN`. Without it the game runs as its image holds: ASIC3's default, the IGS025's default, or what the ARM's internal ROM holds. |

```json
{"id":2,"method":"emu.load_game","params":{"name":"orlegend"}}
{"id":2,"ok":true,"result":{}}
```

Loading powers a new machine up: work RAM and VRAM are zero, the raster is at its first dot,
and the 68000 is held in reset for 100 master ticks, as the simulator's front end holds it. A
`sim.reset` before anything runs takes the place of those 100 ticks, so that the scripts written
for the simulator, which load and then reset, start the 68000 at the same moment.

The ROM directory and the BIOS sources are given when the emulator is started
(`pgmemu-cli --rom-dir DIR --bios PATH...`). The BIOS files are taken from the first BIOS source
that has each, so a directory with PGMTest's `pgm_p02s.u20` named before `pgm.zip` replaces the
program and keeps the rest.

Errors: `unknown_game`, `load_failed`, `unknown_region`, `bad_request`. A load that fails leaves
loaded what was loaded before it.

### `emu.set_region`

Powers the board up again with the loaded cartridge, its game made another region, as
`emu.load_game` with `region` would.

| Param | Meaning |
|---|---|
| `region` | One of the codes `emu.cartridge_info` lists under `region_info`. |

Errors: `unknown_region` (the BIOS alone, or nothing, is loaded, or the game has no such region),
`bad_request`. A region refused leaves the game running undisturbed.

### `emu.set_bios`

Says where the BIOS files are taken from for the loads that follow, in place of the BIOS sources
the emulator was started with. What is loaded runs on undisturbed.

| Param | Meaning |
|---|---|
| `sources` | Paths of directories or zips, one or more, such as that of `pgm.zip`; the first that has a file wins. |

Errors: `bad_request`, when `sources` is empty or names a path that does not exist.

### `emu.cartridge_info`

Describes the loaded cartridge. Takes no parameters. `pgmemu-cli --info FILE` prints the same
for a file.

```json
{
  "short_name": "orlegend",
  "long_name": "Oriental Legend / Xiyou Shi E Zhuan (ver. 126)",
  "manufacturer": "IGS",
  "year": "1997",
  "format_version": "0022",
  "hardware": "asic3",
  "orientation": "horizontal",
  "roms": [ { "type": "PRG", "mapping": 1048576, "size": 2097152, "crc32": "d5e93543" } ],
  "region_info": { "scheme": "asic3", "default_region": 0, "regions": [ { "id": "WRLD", "value": 0 } ] }
}
```

- `hardware` takes the names in [pgm-format.md §2.1](pgm-format.md#21-hardware).
- `orientation` is `vertical` for a game whose monitor stood on its side, which draws its picture
  lying with its top at the left: the frame is seen upright turned a quarter anticlockwise. It is
  the image's flag ([pgm-format.md §2.2](pgm-format.md#22-flags)).
- `region_info` is `null` when the image has no region block.
- `patch_type` and `patch_offset` appear for the `asic27` scheme, and `default_region` for the
  `asic3` scheme.

Errors: `no_cartridge`.

### `memory.list_regions`

Takes no parameters. Answers the names of the regions that hold something now, as an array.

| Region | Holds |
|---|---|
| `BIOS_PROG_ROM` | `pgm_p02s.u20`, 128 KB |
| `BIOS_TILE_ROM` | `pgm_t01s.rom`, 2 MB |
| `BIOS_MUSIC_ROM` | `pgm_m01s.rom`, 2 MB |
| `CART_PROG_ROM` | The cartridge's PRG |
| `CART_TILE_ROM` | TLE |
| `CART_MUSIC_ROM` | AUD |
| `CART_A_ROM` | SPC |
| `CART_B_ROM` | SPM |
| `CART_ARM_ROM` | EXT |
| `CART_ARM_INT_ROM` | INT (the emulator's own name) |
| `CART_IGS022_ROM` | I22 (the emulator's own name) |
| `WORK_RAM` | 128 KB of 68000 work RAM, in the 68000's byte order |
| `VIDEO_RAM` | 32 KB of IGS023 VRAM as the chip's 8-bit RAM holds it: the upper byte of each 68000 word at the odd address |
| `PALETTE_RAM` | 8 KB of palette RAM, in the 68000's byte order |
| `AUDIO_RAM` | The Z80's 64 KB, by Z80 address |

The RAM regions exist while a game is loaded. Their layouts are the simulator's, so that the same
read of either answers bytes that can be compared one for one.

Every ROM region holds its ROM as the `.pgm` file does ([pgm-format.md §3](pgm-format.md#3-the-entry-table)),
and address 0 is the ROM's first byte, whatever its mapping. These are the names the simulator's
`memory.read` accepts, and the two answer the same bytes for the same request. The simulator's
own `memory.list_regions` lists older names that its `memory.read` refuses; the emulator lists the
names that work.

### `memory.read`

| Param | Meaning |
|---|---|
| `region` | A name from `memory.list_regions`. |
| `address` | Byte offset within the region. |
| `size` | Number of bytes, at most 1048576 (1 MB). Larger reads are made in pieces. |

```json
{"id":3,"method":"memory.read","params":{"region":"CART_PROG_ROM","address":0,"size":4}}
{"id":3,"ok":true,"result":{"region":"CART_PROG_ROM","address":0,"data_hex":"82000000"}}
```

`data_hex` holds two lowercase hex digits per byte, in address order.

Errors: `invalid_region`, `invalid_range`, `bad_request`.

### `emu.reset` (alias `sim.reset`)

| Param | Meaning |
|---|---|
| `cycles` | Master ticks to hold the reset line for. |

Holds the reset line for `cycles` master ticks, running the raster on through them, and lets it
go. The 68000 fetches its vectors as soon as the machine runs. It answers `{}` once the ticks
have passed, as the simulator's does. Reset clears the IGS026's latches and the IGS023's
interrupts. It leaves RAM, the raster and the RTC alone, as the RTL does.

Errors: `not_loaded`, `bad_request`.

### `emu.run_frames` (alias `sim.run_frames`)

| Param | Meaning |
|---|---|
| `count` | Frame boundaries to run through. |

A frame boundary is where the simulator's `vblank` output rises: 11 master ticks after line 0
of the raster begins. A run stops between instructions, so it ends at most one instruction past
the boundary.

```json
{"id":4,"ok":true,"result":{"reason":"completed","ticks_executed":50687930,"frames_executed":60}}
```

| Field | Meaning |
|---|---|
| `reason` | `completed`; `breakpoint` when the 68000 reached a breakpoint first; `watchpoint` when it read or wrote a watched address; `halted` when it halted on a double bus fault. |
| `watchpoint` | With `reason` `watchpoint` only: the access, as `{"address", "access": "read" or "write", "value", "bytes", "pc"}`, `pc` being the instruction that made it. |
| `ticks_executed` | Master ticks the run took. |
| `frames_executed` | Frame boundaries it passed. |

Errors: `not_loaded`, `bad_request`.

### `emu.run_cycles` (alias `sim.run_cycles`)

| Param | Meaning |
|---|---|
| `count` | Master ticks (50 MHz) to run for, as the simulator counts them. |

Answers as `emu.run_frames` does.

### `emu.run_until` (alias `sim.run_until`)

| Param | Meaning |
|---|---|
| `condition` | When to stop, below. It is checked after each instruction. |
| `timeout_cycles` | Master ticks after which to give up; 1,000,000,000 (20 s of emulated time) when left out. |

Answers as `emu.run_frames` does, with `reason` `condition_met`, or `timeout` when the time ran
out first.

The conditions are the simulator's:

| `type` | Fields | Holds when |
|---|---|---|
| `cpu_pc_equals` | `value` | the 68000's next instruction is at `value` |
| `cpu_pc_in_range` | `start`, `end` (or `value`, `value2`) | `start` <= pc < `end` |
| `cpu_pc_out_of_range` | as above | pc < `start` or pc >= `end` |
| `signal_equals`, `signal_not_equals`, `signal_less_than`, `signal_less_equal`, `signal_greater_than`, `signal_greater_equal` | `signal`, `value` | the signal compares so with `value` |
| `and`, `or` | `children`: conditions | all hold, or any holds |
| `not` | `children`: one condition | it does not hold |

| Signal | Value |
|---|---|
| `vblank` | 1 in the first 40 lines of the raster |
| `hblank` | 1 in the first 192 dots of a line |
| `line` | The raster's line, 0 to 263, 0 being where vblank begins (the emulator's own) |
| `frame` | Frame boundaries passed since power-up (the emulator's own) |

The simulator also resolves names of RTL signals through Verilator. The emulator has no such
signals and answers `invalid_signal` for them.

Errors: `not_loaded`, `invalid_signal`, `bad_request`.

### `cpu.get_state`

Takes no parameters, or `cpu`: `m68k`, the default, `z80`, or `arm7` for the IGS027A's ARM7TDMI.

```json
{"id":5,"ok":true,"result":{"pc":4166,"registers":[0,4294967295,"..."],"disasm":"move.l  D2, -(A7)",
  "d":[0,4294967295,0,0,0,0,0,0],"a":[8394064,8401830,0,0,0,0,8519628,8519608],
  "sr":8196,"usp":0,"ssp":8519608,"stopped":false,"halted":false}}
```

| Field | Meaning |
|---|---|
| `pc` | The address of the instruction the 68000 executes next. |
| `registers` | The simulator's 17 longs, in fx68k's order: D0-D7, A0-A6, then the user and the supervisor stack pointer. |
| `disasm` | The instruction at `pc`. |
| `d`, `a` | The data and address registers; A7 is the stack pointer in use. |
| `sr`, `usp`, `ssp` | Status register and the two stack pointers. |
| `stopped`, `halted` | Whether a STOP is waiting for an interrupt, and whether the 68000 has halted. |

For the Z80 the answer is its registers, named as the SingleStepTests suite names them, and
whether it is in a HALT. They are as of the last time the sound side was brought up to the
68000's time, which is at the end of every run.

```json
{"id":5,"ok":true,"result":{"pc":1234,"sp":16384,"af":65535,"bc":0,"de":0,"hl":0,"ix":0,"iy":0,
  "af_":65535,"bc_":0,"de_":0,"hl_":0,"wz":0,"i":0,"r":17,"im":1,"iff1":true,"iff2":true,"halted":false}}
```

For the ARM7, as of the last time it was brought up to the 68000's time:

```json
{"id":5,"ok":true,"result":{"pc":460,"r":[268435495,0,"...",468],"cpsr":1610612755,"spsr":0,
  "mode":"supervisor","thumb":false,"fiq":false,"cycles":303790052,"disasm":"bl 0x00001438"}}
```

| Field | Meaning |
|---|---|
| `pc` | The address of the instruction it executes next. |
| `r` | R0 to R15 as its mode sees them; R15 is the pipeline's, 8 (ARM) or 4 (Thumb) past `pc`. |
| `cpsr`, `spsr` | The status registers; `spsr` is 0 in user and system modes, which have none. |
| `mode`, `thumb` | The mode's name (`user`, `fiq`, `irq`, `supervisor`, `abort`, `undefined`, `system`) and the instruction set. |
| `fiq` | Whether the FIQ line is up. |
| `cycles` | Its clock's cycles since its reset. |

Errors: `not_loaded`, `no_cpu`, `bad_request`.

### `cpu.disassemble`

| Param | Meaning |
|---|---|
| `address` | Where to start. |
| `count` | Instructions to disassemble, at most 1000. |
| `cpu` | `m68k`, the default, or `arm7`. |
| `thumb` | For the ARM7: Thumb instructions rather than ARM; if left out, the set it is in. |

Answers an array of `{"address", "length", "text"}`, one per instruction, read as the CPU would
read them, without clocking any device or setting off what reading some of them does.

Errors: `not_loaded`, `no_cpu`, `bad_request`.

### `debug.breakpoint.add`, `debug.breakpoint.remove`

| Param | Meaning |
|---|---|
| `address` | The instruction address. |

A run reaching a breakpoint stops before the instruction and answers `reason` `breakpoint`. The
next run executes that instruction rather than stopping at it again. `add` and `remove` answer
`{}`.

### `debug.breakpoint.list`

Answers the addresses of the breakpoints, ascending.

### `debug.watchpoint.add`, `debug.watchpoint.remove`, `debug.watchpoint.list`

A watchpoint stops a run after the 68000 instruction that reads or writes data in its range.
Instruction fetches and reads relative to the PC do not count; DMA does not either.

| Param | Meaning |
|---|---|
| `address` | The range's first address; one watchpoint per address, a second replacing the first. |
| `size` | Bytes in the range, 1 if left out. |
| `access` | `read`, `write` (the default) or `access` for both. |

`debug.watchpoint.remove` takes `address`. `debug.watchpoint.list` answers
`{"watchpoints":[{"address","size","access"}]}`.

### `debug.trace`

The last instructions the 68000 executed, oldest first: up to 4096 are kept, always.

```json
{"id":8,"ok":true,"result":{"instructions":[{"pc":1294,"ticks":5023441,"disasm":"bra.s   $50e"}]}}
```

`count` asks for how many, 32 if left out. `ticks` is when the instruction began.

### `state.save`, `state.load`, `state.list` (as the simulator has them)

`state.save` writes the whole machine's state to `filename`; `state.load` restores it into the
same game. A `filename` without a directory is in the state directory (`pgmemu-cli
--state-dir`, or the working directory). `state.list` answers the `.pgmstate` files there,
`{"states":["a.pgmstate"]}`.

A state holds everything a run changes, and none of the ROMs, breakpoints, watchpoints or the
trace. It is read back by the build that wrote it: the layout carries a version, and a state of
another version, or of another game, is refused with `state_mismatch`. Loading a state, then
running, gives exactly the frames, memory and sound that followed when it was saved.

Errors: `not_loaded`, `state_failed`, `state_mismatch`, `bad_request`.

### `nvram.save`, `nvram.load` (as the simulator has them)

The board keeps its 128 KB of work RAM on a battery. These write it to `filename` and read it
back, laid out as the RTL's NVRAM interface lays it out: each 16-bit word's low byte first.

Errors: `not_loaded`, `nvram_failed`, `bad_request`.

### `video.screenshot`

The last complete picture: 448 by 224 pixels, completed where vertical blank begins, so just
before every frame boundary.

| Param | Meaning |
|---|---|
| `path` | Where to write it as a PNG of RGB pixels, as the simulator writes one. Left out, the PNG comes back in the answer. |

```json
{"id":6,"ok":true,"result":{"width":448,"height":224,"frame":61,"path":"frame.png"}}
{"id":7,"ok":true,"result":{"width":448,"height":224,"frame":61,"png_base64":"iVBORw0..."}}
```

`frame` counts the pictures completed since power-up.

Errors: `not_loaded`, `screenshot_failed`.

### `audio.capture_start`

Starts recording the ICS2115's output, from the next run on, into a WAV file: 16-bit stereo, at
the chip's own rate, one frame per sample period (32 of its clocks per active voice, about
33 kHz with all 32). Nothing is resampled. A run adds what it produced when it ends.

| Param | Meaning |
|---|---|
| `path` | Where to write the WAV. |

```json
{"id":8,"ok":true,"result":{"path":"bios.wav"}}
```

The simulator's `audio_capture.start` records the same frames, but as its packet stream rather
than a WAV, so the two are not aliases; `scripts/compare-with-rtl.py --audio` reads both.

Errors: `not_loaded`, `capture_running`, `capture_failed`, `bad_request`.

### `audio.capture_stop`

Ends the capture, completes the file and answers what it holds. `sample_rate` is the rate the
frames came at, as their times give it.

```json
{"id":9,"ok":true,"result":{"path":"bios.wav","frames":497633,"sample_rate":33072}}
```

Errors: `capture_not_running`.

### `audio.voices`

The ICS2115's active voices and their registers, in the chip's own units: `osc_acc`,
`osc_start` and `osc_end` are 20.9 fixed-point sample addresses in the bank `osc_saddr` selects,
`vol_acc`, `vol_start` and `vol_end` are 26-bit envelope levels, of which the top 12 bits index
the volume table.

```json
{"id":10,"ok":true,"result":{"active":32,"voices":[{"osc_acc":0,"osc_fc":0,"osc_start":0,"osc_end":0,"osc_saddr":0,"osc_conf":2,"osc_ctl":0,"vol_acc":0,"vol_start":0,"vol_end":0,"vol_incr":0,"vol_pan":127,"vol_ctrl":1,"vol_mode":0}]}}
```

Errors: `not_loaded`.

### `input.set`, `input.clear` (as the simulator has them)

Holds a control of player 1 down, or lets it go, until the next change.

| Param | Meaning |
|---|---|
| `name` | `up`, `down`, `left`, `right`, `button1` (or `btn1`, `a`) and `start`, the simulator's names; and the emulator's own `button2`, `button3`, `button4` and `coin`. |

```json
{"id":11,"ok":true,"result":{}}
```

Errors: `not_loaded`, `invalid_input`, `bad_request`.

### `input.press` (as the simulator has it)

Holds a control for two frames, then lets it go for two: four frames run, and the answer is
`emu.run_frames`'s for all four. Takes `name`, as `input.set` does.

Errors: `not_loaded`, `invalid_input`, `bad_request`.

### `input.get_state` (as the simulator has it)

The controls held, as the simulator encodes them: its `joystick_p1` bits (right 0x01, left 0x02,
down 0x04, up 0x08, buttons 1 to 4 from 0x10), start at 0x10000, and the coin at 0x100000.

```json
{"id":12,"ok":true,"result":{"buttons":65552}}
```

### `video.registers`

IGS023's 16 registers in `registers`, its zoom table in `zoom_table`, and the scroll registers
named: `background_scroll` and `text_scroll` as `{"x","y"}`, `line_counter` and `flags`.

### `video.sprites`

The sprites of the list sprite DMA last copied from work RAM, at line 221: the picture shows
them from the next vertical blank. In list order; a later sprite draws over an earlier one.

```json
{"id":9,"ok":true,"result":{"count":13,"sprites":[{"x":0,"y":40,"scale_x":16,"scale_y":16,"flip_x":false,
  "flip_y":false,"low_priority":true,"palette":9,"mask_address":5601420,"width":28,"height":136}]}}
```

`width` is in 16-pixel units, `height` in lines, `mask_address` a word address in the B ROM.

### `video.layers`

Which of the text, background and sprite layers the picture is drawn with, to see what lies
under one. `text`, `background` and `sprites` are booleans; those given are set, and the answer
is all three. Lines drawn from then on, and so screenshots, follow it. It is a debugger's
setting, not the machine's: save states do not hold it, and the next game loaded draws all three.

### `video.tiles`

Tiles from the tile ROMs as an image: the text layer's, 8 by 8, or the background's, 32 by 32.

| Param | Meaning |
|---|---|
| `layer` | `text` or `background`. |
| `first` | The first tile code; 0 if left out. |
| `count` | How many, 1 to 4096; 256 if left out. |
| `columns` | Tiles to a row; 16 if left out. |
| `palette` | The layer's palette group, 0 to 31; 0 if left out. |
| `path`, `format` | As below. |

### `video.tilemap`

A layer's whole tile map as an image, unscrolled: the text layer's 64 by 32 tiles (512 by 256
pixels), and the background's 64 by 16 (2048 by 512), which is all its 4 KB of VRAM holds; the
chip reads the 16 rows over and over down its 2048 lines. Takes `layer`, `path` and `format`.

The images of `video.tiles` and `video.tilemap` show a transparent pixel dark grey. With `path`
an image is written there as a PNG; without it, it comes back as a PNG in `png_base64`, or, with
`format` `rgba`, as raw RGBA rows in `rgba_base64`. `width` and `height` come back either way.

Errors: `not_loaded`, `bad_request`, `screenshot_failed`.

### `test.status`

The block PGMTest's pages publish their state in, at the top of work RAM, 0x81F000: a 16-bit
magic naming the page, then words whose meaning is the page's own (its `TestStatus` in
`testroms/pages/` of the MiSTer core).

```json
{"id":11,"ok":true,"result":{"address":8515584,"magic":22100,"name":"VT","words":[22100,0,0,1,0,0,5,0,8192,0,0,0]}}
```

`words` asks for how many words, the magic included: 32 if left out, at most 2048. `name` is the
magic as two characters.

### `debug_link.start`, `debug_link.write`, `debug_link.read`, `debug_link.stop` (as the simulator has them)

PGMTest's debug link, as the simulator serves it: the test ROM publishes a block in work RAM,
marked `RFIF`, with a ring of bytes each way, and the link attaches to it and marks it active.
The ROM mailbox the board's PicoROM uses is not emulated, so `debug_link.start`'s `comms_addr`
is taken and not used.

- `debug_link.write {data_hex, timeout_cycles_per_byte}` runs the machine until the test ROM has
  published its block and the bytes fit in its ring.
- `debug_link.read {max_bytes, min_bytes, timeout_cycles}` runs it until at least `min_bytes`
  have come, or the time is up, and answers `{"data_hex", "available"}`: at most `max_bytes`, and
  how many more are waiting.
- `debug_link.stop` marks the block inactive.

Timeouts are master ticks, 2000000 if left out. Errors: `not_loaded`, `debug_link_timeout`,
`bad_request`.
