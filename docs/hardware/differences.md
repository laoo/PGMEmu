# Where the emulator differs from the RTL, and the RTL from the board

The reference is the RTL ([0002](../decisions/0002-the-fpga-core-is-the-reference.md)). This
file lists every place where the emulator knowingly departs from it, and every behaviour of the
RTL the emulator copies although the board does not behave so. An entry goes in the commit that
makes it true and comes out in the commit that makes it false.

## RTL behaviour copied, though the board differs

| What | The RTL | The board | Where |
|---|---|---|---|
| ROM space without a cartridge | Reads the BIOS's SDRAM region: the program, zeros to 1 MB, the BIOS tiles at 1 MB and its samples at 3 MB. | Open bus. | `RomSpace` |
| VRAM size | 32 KB, but the background map is folded into its first 4 KB and 0x6000-0x6FFF onto the text map (`vram_phys`). PGMTest's `vram_write_test` fails on it, identically on the RTL and the emulator. | 32 KB, every byte its own. | `Igs023` |
| Text layer's fetch | Stops after 464 cycles of the 33 MHz clock whether or not all 57 tiles arrived; with the simulator's SDRAM latency the last tiles of a line can be left from an earlier fetch. The emulator always draws them fresh, so PGMTest's `fg_test` differs from the RTL in the rightmost column only. Probable cause, from reading `igs023_fg.sv`. | Fetches every tile. | not reproduced |
| RTC | Starts from zero rather than the date. Its "second" passes every 65536 pulses of a clock derived from the Z80's, about 1.77 s. | A V3021 with its own 32.768 kHz crystal. | `V3021` |
| Z80 at power-up | Runs from address 0 with every register zero: its reset is a latch bit that starts clear, so it is never reset until the BIOS holds it, and the simulator starts every flip-flop at zero. A reset then sets only what tv80's flip-flops hold (PC, AF, AF', SP, I, R, the interrupt state), and leaves BC to IY as they were. | Undefined until the BIOS resets it. | `Z80` |
| ICS2115's voices | `ics2115_osc.sv`'s model, quirks included: a volume loop turns back once and a plain one going up holds; 16-bit samples are the addressed byte twice. An IRQ of a voice that has ended comes back every pass while the voice keeps it enabled; starting the voice again stops it, and so does writing its position, which the RTL's author doubts the board does. The RTL's comments mark much of it as measured on a board. | Where it differs, not known here. | `Ics2115` |
| IGS022's commands | The 68000's write to the IGS025 that starts one waits until the IGS022 has finished it (`prot_dtack_n` in `PGM.sv`, which its own comment doubts). | The cartridge has no hold on DTACK; the game waits for the completion code in shared RAM. | `Igs022Igs025Board` |

## What the images hold, which the RTL does not load

The emulator runs a cartridge as its image holds it ([0014](../decisions/0014-images-as-retrohq-runs-them.md)).

| What | The image | The RTL |
|---|---|---|
| The CAVE games' internal ROM | RetroHQ's recreation of the undumped ROM in its September 2026 edition (564 bytes): the region from 0x20, written as a halfword. | The same recreation's June edition (592 bytes, `type1_cave_fixed.bin`), which writes 0xDD in its place and copies a byte to shared RAM the 68000 does not see. |
| ket's, espgal's and ddp3's program | Each `move.b d3,(-6,a0)` into sprite RAM is an `or.b`, for RetroHQ's hardware. | The program as dumped. |
| martmast's ARM programs | The external ROM's initialisation leaves shared RAM alone; the internal ROM does not check the external ROM's checksum. | As dumped. |
| External ARM ROMs | Decrypted whole. | Decrypted in part as loaded; type 2 XORs each read with a table the ARM writes. |

## The emulator, where it is not yet the RTL

| What | The emulator | The RTL | Until |
|---|---|---|---|
| VRAM contention | `igs023.sv`'s arbiter followed a master tick at a time: the lock while the text layer fetches and the background gets its head start, then the 68000's share of each eight-dot microcycle. The background's own reads in the first half of a microcycle are left out; they decide only whether a request waiting at a microcycle's boundary is started at its first dot. PGMTest's `vram_bench` takes the same time on both, to a chunk, in all 45 of its patterns tried. | The same, and those reads. | not planned |
| When the picture is read | A line at its start, sprites a frame at a time ([0011](../decisions/0011-video-is-drawn-by-line-and-by-frame.md)). | Dot by dot, sprites as line buffers free. | not planned |
| Palette RAM's port | Never taken from the picture. | One port: while the 68000 reads or writes palette RAM, the dot being drawn shows the colour it addresses. PGMTest's `system_basics` writes it on lines 100-120 and leaves scattered dots there. | not planned |
| Sprite line buffers at power-up | Start erased: where no layer draws, the backdrop shows. | Hold zeros until a line is first shown and erased, so the first frame shows sprite palette word 0 at high priority there. | not planned |
| A game's region | The one its image holds (ASIC3's default, the IGS025's default, what the ARM's internal ROM holds), or another of the image's, chosen when it is loaded (`emu.load_game`, `emu.set_region`). | A region byte from the MiSTer's switches, which 0xFF leaves as each chip's ROM has it: ASIC3 takes its low three bits, the IGS025 all of it, and the IGS027A has it read in place of its internal ROM's or shared RAM's own. The simulation sets it by the game's name, the world for orlegend, killbld and drgw3. Unchosen, the two agree for every image built from the workspace's sets. | — |
| The IGS027A's ARM | Runs whole instructions, each access one cycle of its clock, and is brought up to the 68000's time when the 68000 reaches the latch or the shared RAM: what the ARM does is seen up to an instruction's cycles early or late. Its writes land at once. | Interleaves with the 68000 a clock at a time; a write lands a cycle after the core makes it; cache misses stall the ARM, which then catches up, and stall the 68000 on the shared RAM. | not planned |
| IGS022's timing | Commands run whole when the IGS025 starts them; the 68000's write waits the ticks the RTL's engine would take if every ROM read hit `prot_cache.sv`. The DMA the engine runs at reset is done at once. | The engine's states one a master tick, ROM reads waiting on DDR when they miss; the reset's DMA takes some 8,200 ticks and more. | not planned |
| ICS2115's sequencing | The voices of a sample period one after another, each when the RTL's sequencer would load it if both its sample reads hit the cache: 24 master ticks apiece. A register write takes effect at once. | The same order, but a voice whose reads miss the cache waits for SDRAM, whose latency the simulator varies, and a write waits a few cycles for the voice it touches to leave the pipeline. A write that lands close to a voice's turn can reach it one sample earlier or later than here. | not planned |
| ICS2115's status bit 6 | Never set. | Set while a voice write is queued, a few cycles after each. | not planned |
| Z80 bus request | Granted before the Z80's next opcode fetch, which goes on when the bus comes back. | tv80s grants at the end of the current machine cycle, and starts the next one two of its ticks after the bus comes back. Modelled that way, PGMTest's ICS pages start the ICS2115 in step with the RTL, but orlegend's attract sound drifts from it after 18 s; as it is, orlegend's sound and the BIOS's are the RTL's sample for sample, and `z80_ics_test` and `ics2115_vol_pan` start the chip 21 and 29 samples early ([question](../open-questions.md)). | open |
| Z80 interrupt acknowledge | 0xFF from the ICS2115's ports, without a read's side effects. The BIOS's sound driver, which orlegend uses too, runs in interrupt mode 1 and ignores the byte. | Whatever the I/O decode puts on the bus. | — |
| ROM read timing | No delay, as on the board. | A 2,048-line cache in front of SDRAM freezes the 68000 on a miss and catches up after, its fetches served before any other of SDRAM's; latency varies with video and audio contention, and the simulator's SDRAM model adds random delays. | not planned ([0010](../decisions/0010-rom-timing.md)) |

## What comparing with the RTL shows today

`scripts/compare-with-rtl.py` runs the same requests on both. The simulation is the core at
MiSTer-devel's `6f757e4`.

- **The BIOS alone:** VRAM, palette RAM and the picture are identical at frames 60 and 600; work
  RAM differs in dead stack only. Its jingle, recorded from the reset to frame 800, is identical
  sample for sample, 441,754 frames of it; the Z80's RAM differs in dead stack only.
- **orlegend:** VRAM, palette RAM and the picture are identical at frames 300, 900, 1200 and
  1500; work RAM is identical at 300 and 900, and later differs in dead stack only.
- **orlegend's sound:** from the reset to frame 1200, 20 seconds of attract, identical sample
  for sample but for 2 frames after 18.5 s, where a write reaches a voice a sample apart; the
  Z80's RAM is identical.
- **PGMTest's sound pages:** `z80_ctrl`, `z80_sound_test`, `z80_ics_test` and
  `ics2115_vol_pan` match the RTL at frame 360 in the Z80's RAM (dead stack aside), VRAM,
  palette RAM and the picture, with their sounds played through `--press`. `z80_sound_test`'s
  sound is identical sample for sample; `z80_ics_test`'s and `ics2115_vol_pan`'s are identical
  but both begin 18 samples early, for the reason in the table above.
- **PGMTest's video pages:** `bg_test`, `sprite_test` and `video_timing` match the RTL picture
  pixel for pixel, and `video_timing` its work RAM too. `fg_test` differs in its rightmost column, for the reason in the table above.
  `system_basics` differs in rows 99-118, where the simulation shows the dots its palette
  writes leave, for the reason in the table above. The pages' work RAM, which holds what they measured, differs in a few
  bytes.
- **The Killing Blade and Dragon World 3:** their games first reach the IGS025 at frame 767. At
  frame 1100, past their start-up exchanges with it and the IGS022 and into their warning
  screens, VRAM, palette RAM and the picture are identical; work RAM differs in dead stack in
  drgw3, and in killbld only in one word above the stack pointer, at 0x81FF80.
- **Knights of Valour Super Heroes:** its game first reaches the IGS027A at frame 762. At frame
  1100 VRAM, palette RAM and the picture are identical; work RAM differs in dead stack and in five
  bytes of the game's at 0x81B0C4-0x81B0C9, each 4 lower than the RTL's.
- **Dead stack:** bytes below the stack pointer hold what interrupts pushed earlier, and differ
  wherever an interrupt arrived at a different instruction. `--ignore` leaves them out; orlegend's
  stack reaches down to 0x81F000, and the BIOS's sound driver's down from 0x3FE1 in the Z80's
  RAM.

Against the core before `6f757e4`, timing checkpoints in the BIOS's boot showed the emulator
running some loops about 0.03 % faster than the RTL, mostly while it copies ROM into the Z80's
RAM and verifies it. Interrupts still land at other instructions here and there, so dead stack
differs; the open question on [timing drift](../open-questions.md) keeps it in view.
