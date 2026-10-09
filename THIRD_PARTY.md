# Credits and third-party licences

PGMEmu is released under the GNU General Public License, version 2 only ([LICENSE](LICENSE)).
This file names the work it is derived from and the code it is built with, and carries the terms
that work asks to be carried. The binary packages hold this file and the licence of every library
linked into them, in `licenses/`.

## The hardware: the MiSTer core

The emulated hardware is a port of **Martin Donlon (Wickerwaka)**'s IGS PGM core for MiSTer,
https://github.com/MiSTer-devel/Arcade-IGSPGM_MiSTer, released under the GNU General Public
License, version 2. Each module's header names the RTL file it ports and the commit it was read
at. `e898860` is a commit of https://github.com/wickerwaka/Arcade-IGSPGM_MiSTer, where the core
was until June 2026; the move to MiSTer-devel rewrote its history, and it is `ddd88ca` there,
with the same RTL. The other commits are MiSTer-devel's. The test ROM the regression suite runs, PGMTest, is the
core's `testroms/`, built with the fix in [scripts/testroms.patch](scripts/testroms.patch).

## Code derived from MAME, under BSD-3-Clause

Three of the core's RTL files are converted from MAME's drivers, and the emulator's ports of them
carry their licence on:

| Emulator | RTL | MAME source | Copyright holders |
|---|---|---|---|
| `src/pgm_core/src/machine/Igs022.*` | `rtl/igs022.sv` | `src/mame/igs/igs022.cpp` | David Haywood, ElSemi |
| `src/pgm_core/src/machine/Igs025.*` | `rtl/igs025.sv` | `src/mame/igs/igs025.cpp`, `pgmprot_igs025_igs022.cpp` | David Haywood, ElSemi |
| `src/pgm_core/src/machine/Asic3.*` | `rtl/pgm_asic3.sv` | `src/mame/igs/pgmprot_orlegend.cpp` | Olivier Galibert, iq_132 |

```text
Copyright (c) the copyright holders named above

Redistribution and use in source and binary forms, with or without modification, are permitted
provided that the following conditions are met:

1. Redistributions of source code must retain the above copyright notice, this list of
   conditions and the following disclaimer.

2. Redistributions in binary form must reproduce the above copyright notice, this list of
   conditions and the following disclaimer in the documentation and/or other materials provided
   with the distribution.

3. Neither the name of the copyright holder nor the names of its contributors may be used to
   endorse or promote products derived from this software without specific prior written
   permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR
IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND
FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR
CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER
IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
```

## Libraries

Carried in `libextern/`, unmodified, with their licences beside them
([libextern/README.md](libextern/README.md)):

| Library | Copyright | Licence |
|---|---|---|
| [Moira](https://github.com/dirkwhoffmann/Moira), the 68000 | Dirk W. Hoffmann | MIT |
| [chips](https://github.com/floooh/chips)' `z80.h`, the Z80 | Andre Weissflog | zlib |

Fetched when the build is configured ([cmake/Dependencies.cmake](cmake/Dependencies.cmake)), and
linked into the programs:

| Library | Copyright | Licence |
|---|---|---|
| [SDL 3](https://github.com/libsdl-org/SDL) | Sam Lantinga | zlib |
| [Dear ImGui](https://github.com/ocornut/imgui) | Omar Cornut | MIT |
| [spdlog](https://github.com/gabime/spdlog), with its bundled [{fmt}](https://github.com/fmtlib/fmt) | Gabi Melman and contributors; Victor Zverovich | MIT |
| [nlohmann/json](https://github.com/nlohmann/json) | Niels Lohmann | MIT |
| [miniz](https://github.com/richgel999/miniz) | RAD Game Tools and Valve Software, Rich Geldreich | MIT |
| [cpp-httplib](https://github.com/yhirose/cpp-httplib) | yhirose | MIT |
| [CLI11](https://github.com/CLIUtils/CLI11) | University of Cincinnati | BSD-3-Clause |

Used by the test suite alone, and not distributed: [Catch2](https://github.com/catchorg/Catch2)
(Boost Software License 1.0), and the [SingleStepTests](https://github.com/SingleStepTests) CPU
suites, fetched by the `scripts/fetch-*.sh` scripts.

## What is not here

No ROM, BIOS or cartridge image of any kind is part of PGMEmu. The games are run from `.pgm`
images that [PGMBuilder](https://github.com/laoo/PGMBuilder), a separate program under its own
licence, makes from dumps the user owns. IGS, PolyGame Master and the games' titles are their
owners' trademarks; PGMEmu is not affiliated with them.
