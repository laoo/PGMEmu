#pragma once

// The IGS022 of The Killing Blade and Dragon World 3, ported from
// rtl/igs022.sv at MiSTer core commit 6f757e4: an engine that runs commands
// over 16 KB of RAM it shares with the 68000, among them DMA from its private
// 64 KB ROM, decrypted on the way. The IGS025 starts each command (Igs025.hpp).
//
// A command runs whole when it is started; what it would take the RTL's state
// machine is counted, one state a master tick, with every read of the ROM
// hitting prot_cache.sv, so that the 68000 can be kept waiting that long, as
// PGM.sv keeps it.
//
// igs022.sv is converted from MAME's src/mame/igs/igs022.cpp, copyright David
// Haywood and ElSemi, under the BSD-3-Clause licence, whose terms this port
// carries on: THIRD_PARTY.md has them.

#include <array>
#include <cstdint>
#include <span>

namespace pgm::machine
{

class Igs022
{
public:
  /// `rom` is the cartridge's I22 ROM, as it is stored: each 16-bit word low
  /// byte first. It must outlive the chip.
  explicit Igs022( std::span<std::uint8_t const> rom );

  /// The reset: shared RAM filled with 0xA55A, then the DMA the ROM describes
  /// at 0x100 run, as the RTL's engine runs it before it first goes idle. The
  /// engine's registers and stack are left as they were.
  void reset();

  /// Runs the command the 68000 left at shared RAM 0x200, as a trigger from
  /// the IGS025 does, and answers the master ticks the RTL's engine is busy
  /// with it, the trigger's own tick included.
  std::int64_t execute();

  /// A word of the shared RAM, by its index, 0 to 0x1FFF: byte address >> 1.
  [[nodiscard]] std::uint16_t read( std::uint32_t index ) const;
  void write( std::uint32_t index, std::uint16_t value, bool upper, bool lower );

  template <class Archive>
  void serialize( Archive& archive )
  {
    archive( mShared );
    archive( mRegs );
    archive( mStack );
    archive( mStackPtr );
  }

private:
  static constexpr std::size_t SHARED_WORDS = 0x2000;
  static constexpr std::size_t REGS = 768;
  static constexpr std::size_t STACK = 256;

  /// The engine's reads and writes, each counting the ticks its states take.
  std::uint16_t readShared( std::uint32_t index );
  void writeShared( std::uint32_t index, std::uint16_t value );
  /// The ROM's 16-bit word `index`, its address taken to 15 bits.
  std::uint16_t readRomWord( std::uint32_t index );
  /// The two ROM bytes from `address` on, as a word.
  std::uint16_t readRomBytes( std::uint32_t address );
  /// A byte of the ROM through the cache: a tick when the cache's 32-bit word
  /// is the one it was given the tick before, two when it is not.
  std::uint8_t romByte( std::uint32_t address );

  void push( std::uint32_t value );

  /// A DMA: `size` words from ROM word `source` to shared RAM word
  /// `destination`, decrypted by `mode` with `param`.
  struct Transfer
  {
    std::uint16_t source{};
    std::uint16_t destination{};
    std::uint16_t size{};
    std::uint8_t mode{};
    std::uint8_t param{};
  };

  void dma( Transfer const& transfer );
  /// The engine's write of its completion code to shared RAM 0x202.
  void setStatus( std::uint16_t status );

  std::span<std::uint8_t const> mRom;
  std::array<std::uint16_t, SHARED_WORDS> mShared{};
  std::array<std::uint32_t, REGS> mRegs{};
  std::array<std::uint32_t, STACK> mStack{};
  std::uint8_t mStackPtr{};

  /// The ticks the command running has taken so far.
  std::int64_t mTicks{};
  /// The 32-bit word of the ROM the cache was given last; between ROM reads
  /// the engine gives it the ROM's first.
  std::uint32_t mCacheWord{};
};

} // namespace pgm::machine
