#pragma once

// The 68000's address space: the chip selects of rtl/address_translator.sv and
// the order PGM.sv's data multiplexer gives them, at MiSTer core commit
// 6f757e4. A cartridge's protection takes its addresses before the board's
// own decode (Protection.hpp).

#include "Asic3.hpp"
#include "Igs023.hpp"
#include "Igs026.hpp"
#include "Protection.hpp"
#include "Sdram.hpp"

#include "pgm/machine/Time.hpp"

#include <array>
#include <cstdint>
#include <span>

namespace pgm::machine
{

/// The 68000's ROM space, 0x000000-0x7FFFFF, as the RTL fetches it from SDRAM:
/// the cartridge's program from its base up when one is inserted, and below it,
/// or everywhere without one, the BIOS's region of SDRAM, which reaches past
/// the 128 KB program into the BIOS's tiles at 1 MB and samples at 3 MB.
struct RomSpace
{
  Sdram const* sdram{};
  bool cartridge{};
  std::uint32_t cartBase{};

  /// The 68000 word at even byte address `address`. ROM files hold each word
  /// with its low byte first.
  [[nodiscard]] std::uint16_t word( std::uint32_t address ) const;
};

/// Inputs as the board's I/O ports read them, before inversion: a set bit is a
/// pressed button. The layout of each word is IN0..IN3 of PGM.sv.
struct InputPorts
{
  std::array<std::uint16_t, 4> pressed{};
};

/// The devices on the 68000's bus besides ROM and work RAM.
struct BusDevices
{
  Igs023& video;
  Igs026& io;
  Asic3& asic3;
  InputPorts const& inputs;
  /// The cartridge's protection; null for none.
  Protection* protection;
};

class Bus68k
{
public:
  /// `time` is the machine's clock: the time of every bus cycle, and what a
  /// device that keeps the 68000 waiting adds its wait states to.
  Bus68k( RomSpace rom, BusDevices devices, Time& time, std::span<std::uint8_t> workRam );

  /// A bus cycle now. Byte accesses are word cycles with one strobe; the
  /// written byte is on both halves of `value`, as the 68000 drives it.
  std::uint16_t read( std::uint32_t address, bool upper, bool lower );
  void write( std::uint32_t address, std::uint16_t value, bool upper, bool lower );

  /// A read for the debugger and the disassembler: no device is clocked by it.
  [[nodiscard]] std::uint16_t peek( std::uint32_t address ) const;

  [[nodiscard]] std::span<std::uint8_t const> workRam() const;

private:
  [[nodiscard]] bool isProtection( std::uint32_t address ) const;

  RomSpace mRom;
  Igs023& mVideo;
  Igs026& mIo;
  Asic3& mAsic3;
  InputPorts const& mInputs;
  Protection* mProtection;
  /// The 64 KB pages in which the protection may answer, by `address >> 16`.
  std::array<bool, 256> mProtectionPages{};
  Time& mTime;
  /// 128 KB of work RAM in the 68000's byte order; the sprite DMA reads it too.
  std::span<std::uint8_t> mWorkRam;
};

} // namespace pgm::machine
