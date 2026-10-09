#pragma once

// The IGS025 of The Killing Blade and Dragon World 3, ported from
// rtl/igs025.sv at MiSTer core commit 6f757e4: two words, a command and its
// data, through which the game reads its id and its region's table, works the
// chip's checksum, and starts the IGS022's commands. The RTL builds each
// game's id and table in; here they are the I25 block's (cart::Igs025Table),
// which holds the same ones.
//
// igs025.sv is converted from MAME's src/mame/igs/igs025.cpp and
// pgmprot_igs025_igs022.cpp, copyright David Haywood and ElSemi, under the
// BSD-3-Clause licence, whose terms this port carries on: THIRD_PARTY.md has
// them.

#include "pgm/cart/PgmImage.hpp"

#include <cstdint>

namespace pgm::machine
{

class Igs025
{
public:
  /// `table` is the region's, from the cartridge's I25 block.
  explicit Igs025( cart::Igs025Table const& table );

  void reset();

  /// A read of word `offset`, 0 the command and 1 its data.
  std::uint16_t read( unsigned offset );
  /// The same read without its side effect, for the debugger.
  [[nodiscard]] std::uint16_t peek( unsigned offset ) const;
  /// A write of word `offset`; answers whether it starts an IGS022 command.
  bool write( unsigned offset, std::uint16_t value );

  template <class Archive>
  void serialize( Archive& archive )
  {
    archive( mCommand );
    archive( mReg );
    archive( mPtr );
    archive( mSwap );
    archive( mHold );
    archive( mHilo );
    archive( mHiloSelect );
  }

private:
  cart::Igs025Table mTable;
  std::uint16_t mCommand{};
  std::uint16_t mReg{};
  std::uint16_t mPtr{};
  std::uint16_t mSwap{};
  std::uint16_t mHold{};
  std::uint16_t mHilo{};
  std::uint8_t mHiloSelect{};
};

} // namespace pgm::machine
