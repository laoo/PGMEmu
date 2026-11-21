#pragma once

#include "pgm/cart/Bios.hpp"
#include "pgm/cart/PgmImage.hpp"
#include "pgm/machine/Sound.hpp"
#include "pgm/machine/Time.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <vector>

namespace pgm::machine
{

/// The 68000's registers, as the debugger and `cpu.get_state` show them.
struct M68kState
{
  std::array<std::uint32_t, 8> d{};
  std::array<std::uint32_t, 8> a{};
  std::uint32_t pc{};
  std::uint16_t sr{};
  std::uint32_t usp{};
  std::uint32_t ssp{};
  bool stopped{};
  bool halted{};
};

/// The IGS027A's ARM7TDMI, as the debugger shows it.
struct Arm7Registers
{
  /// R0 to R15 as its current mode sees them; R15 is the pipeline's, 8 or 4
  /// past `pc`.
  std::array<std::uint32_t, 16> r{};
  std::uint32_t cpsr{};
  /// The current mode's SPSR; 0 in user and system modes, which have none.
  std::uint32_t spsr{};
  /// The address of the instruction it executes next.
  std::uint32_t pc{};
  bool fiq{};
  std::int64_t cycles{};
};

/// The processors a debugger can stop at.
enum class Cpu : std::uint8_t
{
  M68K,
  Z80,
  /// The IGS027A's ARM7TDMI.
  ARM7
};

/// The RAMs a debugger can write.
enum class Ram : std::uint8_t
{
  WORK,
  VIDEO,
  PALETTE,
  /// The Z80's.
  AUDIO
};

/// Why a run returned.
enum class StopReason : std::uint8_t
{
  /// It ran as long as it was asked to.
  COMPLETED,
  /// A processor reached a breakpoint (BreakpointHit). The 68000 has not
  /// executed that instruction.
  BREAKPOINT,
  /// The 68000 read or wrote a watched address; the instruction that did it
  /// has executed.
  WATCHPOINT,
  /// The condition it was given became true.
  CONDITION_MET,
  /// The condition did not become true in the time allowed.
  TIMEOUT,
  /// The 68000 halted (a double bus fault) and cannot go on.
  HALTED
};

/// A range of the 68000's address space whose data reads or writes stop a run.
struct Watchpoint
{
  std::uint32_t address{};
  std::uint32_t size{ 1 };
  bool read{};
  bool write{ true };
};

/// The access that stopped a run at a watchpoint.
struct WatchpointHit
{
  std::uint32_t address{};
  bool write{};
  /// What was read or written: a byte or a word.
  std::uint16_t value{};
  std::uint8_t bytes{};
  /// The instruction that made the access.
  std::uint32_t pc{};
};

/// The breakpoint that ended a run.
struct BreakpointHit
{
  Cpu cpu{};
  std::uint32_t address{};
  /// The Z80's or the ARM7's registers as they were at the breakpoint. That
  /// processor was not stopped there: it ran on to the 68000's time
  /// (docs/decisions/0023-breakpoints-on-the-caught-up-cpus.md).
  std::optional<Z80Registers> z80;
  std::optional<Arm7Registers> arm7;
};

/// An instruction the 68000 executed, when it began, and, when the trace
/// keeps them (Machine::setTraceRegisters), its registers as they were before
/// it; zeros otherwise.
struct TraceEntry
{
  std::uint32_t pc{};
  /// Master ticks since power-up.
  std::int64_t ticks{};
  std::array<std::uint32_t, 8> d{};
  /// A7 is the stack pointer in use.
  std::array<std::uint32_t, 8> a{};
  std::uint16_t sr{};
};

/// Which of IGS023's layers the picture is drawn with: all of them, unless a
/// debugger turns some off to see what lies under.
struct VideoLayers
{
  bool text{ true };
  bool background{ true };
  bool sprites{ true };
};

/// One of the two tile layers.
enum class TileLayer : std::uint8_t
{
  TEXT,
  BACKGROUND
};

/// A sprite of the list sprite DMA copied, as its five words say it.
struct SpriteInfo
{
  std::uint32_t x{};      // 11 bits
  std::uint32_t y{};      // 10 bits
  std::uint32_t scaleX{}; // 5 bits; 16 and above shrink
  std::uint32_t scaleY{};
  bool flipX{};
  bool flipY{};
  bool lowPriority{};
  std::uint16_t palette{};
  /// The word address of its masks in the B ROM.
  std::uint32_t maskAddress{};
  /// In 16-pixel units, and in lines.
  std::uint32_t width{};
  std::uint32_t height{};
};

/// A picture for a debugger: RGBA, row by row.
struct Image
{
  int width{};
  int height{};
  std::vector<std::uint8_t> rgba;
};

struct RunResult
{
  StopReason reason{};
  /// Master ticks (50 MHz) the run took.
  std::int64_t ticks{};
  /// Frame boundaries it crossed.
  std::int64_t frames{};
};

/// The PGM board: the 68000 and every device it reaches, running in emulated
/// time from power-up. docs/architecture.md describes how time is kept.
class Machine
{
public:
  /// Powers the board up with `bios` and, unless it is null, `cartridge`
  /// inserted, its game made region value `region` (cart::PgmImage::
  /// regionValue), or the one its image holds when none is given. Its reset
  /// line is held for the first 100 master ticks, as the RTL simulator's front
  /// end holds it; a reset() before the machine runs takes their place. Both
  /// must outlive the machine.
  Machine( cart::Bios const& bios,
           cart::PgmImage const* cartridge,
           std::optional<std::uint32_t> region = std::nullopt );
  ~Machine();

  Machine( Machine const& ) = delete;
  Machine& operator=( Machine const& ) = delete;
  Machine( Machine&& ) = delete;
  Machine& operator=( Machine&& ) = delete;

  /// Holds the reset line for `masterTicks` from now, running the raster on
  /// through it as the board does, then lets it go: the 68000 fetches its
  /// vectors as soon as the machine runs. This is the RTL simulator's
  /// `sim.reset`, which also returns with the time spent.
  void reset( std::int64_t masterTicks );

  /// Runs until `frames` frame boundaries have passed. A boundary is where the
  /// RTL simulator's `vblank` output rises: 11 master ticks after line 0 begins.
  RunResult runFrames( std::int64_t frames );

  /// Runs for at least `masterTicks`; it stops between instructions.
  RunResult runTicks( std::int64_t masterTicks );

  /// Runs until `condition` holds after an instruction, or `timeoutTicks` pass.
  RunResult runUntil( std::function<bool()> const& condition, std::int64_t timeoutTicks );

  /// Runs `instructions` of the 68000, or until `timeoutTicks` pass, which a
  /// 68000 that waits does: stopped with nothing to wake it, or held in reset.
  /// The entry into an interrupt is an instruction here, as it is in the
  /// trace: the 68000 takes it in place of the instruction it was about to
  /// execute.
  RunResult step( std::int64_t instructions, std::int64_t timeoutTicks );

  [[nodiscard]] Time now() const;
  /// Frame boundaries passed since power-up.
  [[nodiscard]] std::int64_t frame() const;
  [[nodiscard]] int line() const;
  [[nodiscard]] int dot() const;
  [[nodiscard]] bool vblank() const;
  [[nodiscard]] bool hblank() const;

  [[nodiscard]] M68kState m68kState() const;
  /// The Z80's registers, as of the last time the 68000 or the end of a run
  /// caught the sound side up.
  [[nodiscard]] Z80Registers z80Registers() const;
  [[nodiscard]] bool z80Halted() const;
  /// The ARM7TDMI's registers, as of the last time the 68000 or the end of a
  /// run caught it up; nothing on a board without one.
  [[nodiscard]] std::optional<Arm7Registers> arm7Registers() const;
  /// The ARM instruction at `address`, Thumb's when `thumb`, and its length in
  /// bytes; nothing on a board without an ARM7TDMI.
  [[nodiscard]] std::optional<std::string> disassembleArm7( std::uint32_t address, bool thumb, int& length ) const;
  /// The instruction at `address`, and its length in bytes.
  [[nodiscard]] std::string disassemble( std::uint32_t address, int& length ) const;
  /// The word at `address` as the 68000 would read it from ROM, work RAM,
  /// VRAM, the palette, IGS023's registers or the cartridge's protection, or 0
  /// elsewhere; it clocks no device.
  [[nodiscard]] std::uint16_t peek( std::uint32_t address ) const;
  /// `size` bytes of the 68000's address space from `address` on, read as
  /// peek() reads them. The space is 16 MB, and what runs past its end is
  /// left out.
  [[nodiscard]] std::vector<std::uint8_t> readBus( std::uint32_t address, std::size_t size ) const;
  /// Writes `bytes` into the 68000's address space from `address` on, as a
  /// debugger does: no bus cycle is made. False, and nothing written, when a
  /// byte falls where no RAM is mapped: outside work RAM, VRAM and the palette.
  bool writeBus( std::uint32_t address, std::span<std::uint8_t const> bytes );

  /// The whole board's state, as a save state holds it: everything a run
  /// changes, and nothing of the ROMs, the breakpoints or the listeners.
  [[nodiscard]] std::vector<std::uint8_t> saveState();
  /// Restores what saveState() wrote. A machine with other ROMs takes it but
  /// runs on wrongly; checking that is the caller's. False, and the machine
  /// unchanged, when `state` is not one this build wrote.
  bool loadState( std::span<std::uint8_t const> state );

  /// Instruction addresses of `cpu` a run stops at. The 68000 stops before
  /// the instruction executes. The Z80 and the ARM7 are caught up to the
  /// 68000 rather than run beside it, so the run stops after the 68000
  /// instruction during which they reached it; while either has breakpoints,
  /// they are caught up after every 68000 instruction. False, and nothing
  /// set, when the board has no ARM7.
  bool addBreakpoint( std::uint32_t address, Cpu cpu = Cpu::M68K );
  void removeBreakpoint( std::uint32_t address, Cpu cpu = Cpu::M68K );
  [[nodiscard]] std::set<std::uint32_t> const& breakpoints( Cpu cpu = Cpu::M68K ) const;
  /// The breakpoint that ended the last run.
  [[nodiscard]] std::optional<BreakpointHit> const& breakpointHit() const;
  /// Whether a breakpoint or a watchpoint is set on any processor: a run may
  /// then end early.
  [[nodiscard]] bool hasDebugPoints() const;

  /// Data accesses of the 68000 that stop a run, after the instruction that
  /// made them. Instruction fetches do not count. One watchpoint per address;
  /// adding another at the same address replaces it.
  void addWatchpoint( Watchpoint watchpoint );
  void removeWatchpoint( std::uint32_t address );
  [[nodiscard]] std::vector<Watchpoint> const& watchpoints() const;
  /// The access that ended the last run at a watchpoint.
  [[nodiscard]] std::optional<WatchpointHit> const& watchpointHit() const;

  /// How many instructions the trace keeps unless it is told otherwise, and
  /// the most it can be told to: 88 bytes each.
  static constexpr std::size_t TRACE_SIZE = 4096;
  static constexpr std::size_t MAX_TRACE_SIZE = std::size_t{ 1 } << 22U;
  /// Keeps the last `instructions` executed from now on, 1 to MAX_TRACE_SIZE;
  /// of those kept now, the newest that fit stay.
  void setTraceSize( std::size_t instructions );
  [[nodiscard]] std::size_t traceSize() const;
  /// Whether the trace keeps the registers before every instruction. It does
  /// not unless told to: copying them costs every run a few percent of its
  /// speed. A change empties the trace, so that every instruction in it has
  /// its registers or none has.
  void setTraceRegisters( bool kept );
  [[nodiscard]] bool traceRegisters() const;
  /// How many instructions the trace holds now.
  [[nodiscard]] std::size_t traceCount() const;
  /// The instruction executed `index` before the last one, 0 being the last;
  /// `index` is below traceCount().
  [[nodiscard]] TraceEntry traceAt( std::size_t index ) const;
  /// `count` instructions the 68000 executed, oldest first, the newest of
  /// them being traceAt( from ); fewer when the trace holds fewer.
  [[nodiscard]] std::vector<TraceEntry> trace( std::size_t count, std::size_t from = 0 ) const;

  /// The four input words of PGM.sv, IN0..IN3, with a set bit for a pressed
  /// button; IN3's low byte is the DIP switches. What the control protocol
  /// holds, and what the host's keyboard and pads hold, are set apart, and a
  /// button either presses is pressed.
  void setInputs( std::array<std::uint16_t, 4> const& pressed );
  void setHostInputs( std::array<std::uint16_t, 4> const& pressed );

  [[nodiscard]] std::span<std::uint8_t const> workRam() const;
  /// Writes `bytes` into work RAM from `offset` on, in 68000 byte order, as a
  /// debugger or `nvram.load` does; what does not fit is left out.
  void writeWorkRam( std::size_t offset, std::span<std::uint8_t const> bytes );
  [[nodiscard]] std::span<std::uint8_t const> videoRam() const;
  [[nodiscard]] std::span<std::uint8_t const> paletteRam() const;
  [[nodiscard]] std::span<std::uint8_t const> z80Ram() const;
  /// Writes `bytes` into `ram` from `offset` on, laid out as workRam(),
  /// videoRam(), paletteRam() and z80Ram() read: as a debugger does, with no
  /// bus cycle and nothing clocked. False, and nothing written, when the
  /// bytes run past the RAM's end.
  bool writeRam( Ram ram, std::size_t offset, std::span<std::uint8_t const> bytes );

  /// The last complete frame: 448 by 224 pixels, RGBA, row by row. It is
  /// complete when vertical blank begins, which is just before a frame boundary.
  [[nodiscard]] std::span<std::uint8_t const> picture() const;
  /// Pictures completed since power-up.
  [[nodiscard]] std::int64_t picturesDrawn() const;

  /// Which layers the picture is drawn with, from the next line drawn on.
  void setVideoLayers( VideoLayers layers );
  [[nodiscard]] VideoLayers videoLayers() const;
  /// IGS023's 16 registers, and its zoom table.
  [[nodiscard]] std::array<std::uint16_t, 16> videoRegisters() const;
  [[nodiscard]] std::array<std::uint16_t, 32> zoomTable() const;
  /// The sprites of the list sprite DMA last copied, which the picture shows
  /// from the next vertical blank.
  [[nodiscard]] std::vector<SpriteInfo> sprites() const;
  /// `count` tiles of a layer from tile code `first`, `columns` to a row, in
  /// palette group `palette` (0 to 31). Transparent pixels are dark grey.
  [[nodiscard]] Image tiles(
      TileLayer layer, std::uint32_t first, std::uint32_t count, std::uint32_t columns, std::uint32_t palette ) const;
  /// A layer's whole tile map, unscrolled: the text layer's 64 by 32 tiles,
  /// the background's 64 by 16, all of it that its 4 KB of VRAM holds.
  /// Transparent pixels are dark grey.
  [[nodiscard]] Image tilemap( TileLayer layer ) const;

  /// The ICS2115's output during the last run, at its own rate: a frame per
  /// 32 of its clocks per active voice, about 33 kHz with all 32.
  [[nodiscard]] std::span<AudioFrame const> audio() const;
  /// The rate audio() comes at now, in frames per second.
  [[nodiscard]] double audioRate() const;
  /// Called with audio() as every run ends; an empty function stops the calls.
  void setAudioListener( std::function<void( std::span<AudioFrame const> )> listener );
  /// Whether a listener is set: a run then reaches more than the caller.
  [[nodiscard]] bool audioListened() const;
  [[nodiscard]] std::array<Ics2115Voice, 32> const& ics2115Voices() const;
  [[nodiscard]] std::size_t ics2115ActiveVoices() const;

private:
  struct Parts;
  std::unique_ptr<Parts> mParts;
};

} // namespace pgm::machine
