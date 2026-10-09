#pragma once

// The Killing Blade's and Dragon World 3's protection: an IGS025 the 68000
// gives commands, and the IGS022 it starts, which shares 16 KB of RAM with the
// 68000. Where each answers is address_translator.sv's decode, and the wait
// for an IGS022 command PGM.sv's prot_dtack_n, at MiSTer core commit 6f757e4.

#include "Igs022.hpp"
#include "Igs025.hpp"
#include "Protection.hpp"

#include "pgm/cart/PgmImage.hpp"

namespace pgm::machine
{

class Igs022Igs025Board final : public Protection
{
public:
  /// `igs025Address` is the byte address of the IGS025's first word.
  Igs022Igs025Board( std::span<std::uint8_t const> igs022Rom,
                     cart::Igs025Table const& igs025Table,
                     std::uint32_t igs025Address );

  [[nodiscard]] std::vector<std::uint8_t> pages() const override;
  [[nodiscard]] bool decodes( std::uint32_t address ) const override;
  std::uint16_t read( Time& time, std::uint32_t address, bool upper, bool lower ) override;
  void write( Time& time, std::uint32_t address, std::uint16_t value, bool upper, bool lower ) override;
  [[nodiscard]] std::uint16_t peek( std::uint32_t address ) const override;
  void reset( Time releasedAt ) override;
  void serialize( StateWriter& archive ) override;
  void serialize( StateReader& archive ) override;

private:
  [[nodiscard]] bool isIgs025( std::uint32_t address ) const;

  Igs022 mIgs022;
  Igs025 mIgs025;
  std::uint32_t mIgs025Address;
};

} // namespace pgm::machine
