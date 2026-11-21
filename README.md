# PGMEmu

PGMEmu is an emulator of the IGS PolyGame Master (PGM) arcade board.

The reference is the MiSTer FPGA core by **Martin Donlon (Wickerwaka)**,
[Arcade-IGSPGM_MiSTer](https://github.com/MiSTer-devel/Arcade-IGSPGM_MiSTer). An LLM ported its
hardware to C++. All credit for understanding the board goes to him. PGMEmu is compared with a
Verilator simulation of the core.

- Every game that the MiSTer core supported in June 2026 runs, with its protection: the
  IGS027A's ARM7 of the later games, the IGS022/IGS025 of The Killing Blade and Dragon World 3,
  and the ASIC3 of Oriental Legend.
- It runs `.pgm` cartridge images made by [PGMBuilder](https://github.com/laoo/PGMBuilder). This
  is the format that RetroHQ's hardware runs.
- The window has shaders (sharp, scanlines, CRT), shows vertical games upright, maps the inputs
  of four players and their gamepads, and has rewind and run-ahead.
- Other programs and AI agents can control it, with or without the window.

## Games and the BIOS

**PGMEmu does not include a BIOS or any games.** You need your own dumps, as MAME sets.

- The BIOS is MAME's `pgm.zip`. PGMEmu reads it as it is.
- A game must be a `.pgm` image. [PGMBuilder](https://github.com/laoo/PGMBuilder) makes one from
  a set's zip.

IGS, PolyGame Master and the titles of the games are trademarks of their owners. PGMEmu is not
affiliated with them.

## Building

You need CMake 3.25 or later, Ninja and a C++23 compiler: Apple clang, GCC 14 or MSVC.

```sh
cmake --preset release && cmake --build --preset release && ctest --preset release
```

CMake downloads every dependency, SDL3 included, at a pinned version when it first configures. So
the first configure needs the network.

On Linux, SDL3 needs the development packages of the display and sound systems. On Ubuntu:

```sh
sudo apt install ninja-build libasound2-dev libpulse-dev libx11-dev libxext-dev libxrandr-dev \
  libxcursor-dev libxi-dev libxss-dev libxtst-dev libxkbcommon-dev libwayland-dev \
  libegl1-mesa-dev libdbus-1-dev libudev-dev
```

- To build without the window (and without SDL3 and ImGui), add `-DPGM_BUILD_APP=OFF`.
- On Windows, the `windows` preset builds with Visual Studio.

Each tagged release on GitHub has packages for macOS (Apple silicon), Linux (x64) and Windows
(x64). The macOS application is not notarized. The first time, open it with right-click > Open,
or run `xattr -dr com.apple.quarantine PGMEmu.app`.

## Running

There are two programs. Both print their options with `--help`.

A game is given by its set name or by the path of its `.pgm` file. A set name `orlegend` means
`orlegend.pgm` in the folder given with `--rom-dir`. The set name `pgm` runs the BIOS alone.

### The window: `pgmemu`

```sh
pgmemu
pgmemu --bios path/to/pgm.zip --rom-dir path/to/images orlegend
```

You can also drop a `.pgm` file onto the window, or use File > Open. Without `--bios`, the window
looks for `pgm.zip` next to the game, or one folder up. You can also choose it with File > Choose
BIOS. The window remembers it.

The keyboard is player 1 and player 2:

| Control | Player 1 | Player 2 |
|---|---|---|
| Joystick | Arrows | R, F, D, G |
| Buttons 1 to 4 | Z, X, C, V | A, S, Q, W |
| Start | 1 | 2 |
| Coin | 5 | 6 |

View > Input changes the keys, and maps gamepads for up to four players.

- Hold Backspace, or the left shoulder of a gamepad, to rewind up to 30 seconds.
- Emulation > Run-ahead shows each frame 1 to 3 frames ahead. So you see the effect of a control
  sooner. It is off by default.
- Emulation > Region starts the game again in another region.
- Display chooses the shader and the scaling.

### The command line: `pgmemu-cli`

`pgmemu-cli` has no window. It runs only on request.

```sh
pgmemu-cli --info orlegend.pgm                                   # describe an image
pgmemu-cli --server --bios pgm.zip --rom-dir images              # JSON lines on stdio
pgmemu-cli --benchmark orlegend --bios pgm.zip --rom-dir images  # how fast, without throttling
```

Useful options:

- `--state-dir DIR` is where `state.save` and `state.load` keep save states.
- `--batch FILE` runs a batch script and checks it against its recorded sums. `--record` writes
  the sums into the script. The format is in [docs/spec/batch.md](docs/spec/batch.md).
- `--benchmark GAME` runs a game as fast as it can for `--frames` frames (default 3600).
- `--transcript FILE` answers the requests in FILE and prints the answers. `--expect FILE`
  compares them with recorded answers instead.

## Remote control

Other programs can control a running emulator, with or without the window. They can load a game,
run it by frames, by cycles, by instructions or until a condition, press buttons, read and write
memory, read registers, disassemble code, set breakpoints on the 68000, the Z80 and the ARM7, set
watchpoints, search the history of what the 68000 executed, save and load states, look at the
sprites, layers and tiles, take screenshots and record the sound.

There is one set of methods. It is available in two ways:

- **JSON lines**, on stdio or TCP. Each request is one line of JSON. Each answer is one line.
- **MCP** (Model Context Protocol), on stdio or HTTP. Each method is an MCP tool, so an AI agent
  can use the emulator directly. Screenshots come back as images.

The methods are the same as those of the MiSTer core's Verilator simulation. So one script can
drive both, and compare them. The protocol is specified in
[docs/spec/control-protocol.md](docs/spec/control-protocol.md).

### JSON lines

```sh
pgmemu-cli --server tcp:7701 --bios pgm.zip --rom-dir images orlegend
```

The game after the flags is loaded before the first request. Then send requests from another
terminal:

```sh
echo '{"id":1,"method":"emu.run_frames","params":{"count":600}}' | nc -w 60 127.0.0.1 7701
```

```json
{"id":1,"ok":true,"result":{"reason":"completed","ticks_executed":506035225,"frames_executed":600}}
```

`scripts/pgmemu.py` is a small client. It sends one request and prints the result:

```sh
scripts/pgmemu.py emu.load_game name=orlegend
scripts/pgmemu.py emu.run_frames count=600
scripts/pgmemu.py video.screenshot path=shot.png
```

Without `tcp:PORT`, `--server` reads requests on stdin and writes answers on stdout.

### MCP

```sh
pgmemu-cli --mcp --bios pgm.zip --rom-dir images     # MCP on stdio
pgmemu --mcp-http 7702                               # the window, MCP on http://127.0.0.1:7702/mcp
```

To add it to Claude Code:

```sh
claude mcp add pgmemu -- /path/to/pgmemu-cli --mcp --bios /path/to/pgm.zip --rom-dir /path/to/images
claude mcp add --transport http pgmemu http://127.0.0.1:7702/mcp    # the window, when it is open
```

Other MCP clients take a configuration like this one:

```json
{"mcpServers":{"pgmemu":{"command":"/path/to/pgmemu-cli","args":["--mcp","--bios","/path/to/pgm.zip","--rom-dir","/path/to/images"]}}}
```

### The window

The window serves the same protocol while you watch it:

```sh
pgmemu --server tcp:7701 --mcp-http 7702 --bios pgm.zip --rom-dir images orlegend
```

All clients control the same emulator: the game on the screen. When a client loads a game, it
replaces the game in the window. A client can pause the window's run (`emu.pause`), and a
breakpoint that the window's run reaches pauses it too.

The servers listen on 127.0.0.1 only. They have no authentication. A client can make the emulator
write files, by the paths it gives to methods such as `video.screenshot` and `state.save`. Do not
expose the servers to a network.

## Tests

`ctest` runs the unit tests, the CPU test suites, recorded sessions of the control protocol, and a
regression suite of golden frames: the BIOS, every game and the pages of PGMTest. The tests that need ROMs, and the CPU suites, skip when
their data is not there. [CLAUDE.md](CLAUDE.md) says where they look for it, and how to compare
the emulator with the simulation of the core.

The design is in [docs/architecture.md](docs/architecture.md). The reasons for it are in
[docs/decisions/](docs/decisions/). [docs/README.md](docs/README.md) is the index.

## Licence

PGMEmu is released under the GNU General Public License, version 2 only ([LICENSE](LICENSE)).
This is the licence of the MiSTer core;
[docs/decisions/0004-licence-gpl-2.md](docs/decisions/0004-licence-gpl-2.md) says why.
[THIRD_PARTY.md](THIRD_PARTY.md) lists the work it is derived from and the libraries it is built
with, and carries their terms.
