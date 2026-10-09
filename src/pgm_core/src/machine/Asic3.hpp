#pragma once

// The ASIC3 protection of Oriental Legend, ported from rtl/pgm_asic3.sv at
// MiSTer core commit 6f757e4. The 68000 reaches it at 0xC04000-0xC0400F on
// every board, as PGM.sv decodes it whatever the cartridge: the first word
// selects a register, the others write it or read it back.
//
// pgm_asic3.sv is converted from MAME's src/mame/igs/pgmprot_orlegend.cpp,
// copyright Olivier Galibert and iq_132, under the BSD-3-Clause licence, whose
// terms this port carries on: THIRD_PARTY.md has them.

#include <array>
#include <cstdint>

namespace pgm::machine
{

class Asic3
{
public:
  /// `region` is the value the game reads as its region: 0 or 1 is the world.
  explicit Asic3( std::uint8_t region );

  void reset();
  [[nodiscard]] std::uint16_t read() const;
  /// A bus write at byte address `address`, whatever its strobes.
  void write( std::uint32_t address, std::uint16_t value );

  /// Names its state for a save state (StateArchive.hpp).
  template <class Archive>
  void serialize( Archive& archive )
  {
    archive( mRegister );
    archive( mLatch );
    archive( mX );
    archive( mHilo );
    archive( mHold );
  }

private:
  [[nodiscard]] std::uint16_t nextHold( std::uint16_t data ) const;

  std::uint8_t mRegion;
  std::uint8_t mRegister{};
  std::array<std::uint8_t, 3> mLatch{};
  std::uint8_t mX{};
  std::uint16_t mHilo{};
  std::uint16_t mHold{};
};

} // namespace pgm::machine
