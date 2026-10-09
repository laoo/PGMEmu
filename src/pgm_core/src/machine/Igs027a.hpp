#pragma once

// The IGS027A: an ARM7TDMI with its own ROM and RAM, which the 68000 talks to
// through a latch and shared RAM. Ported from rtl/igs027a.sv, with the
// 68000's decode of address_translator.sv and the clocks of PGM.sv, at MiSTer
// core commit 6f757e4. The three board types differ in where things are, on
// either side, and in what raises the ARM's FIQ.
//
// The ARM runs on its own clock and is caught up to the 68000 whenever the
// 68000 reaches the latch or the shared RAM, and at the end of every run. It
// runs an instruction at a time, so what it does is seen up to an
// instruction's cycles late or early; nothing it does can interrupt the 68000.

#include "Protection.hpp"
#include "cpu/Arm7.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace pgm::machine
{

/// Where a board puts things, and how fast its ARM runs.
struct Igs027aBoard
{
  enum class Type : std::uint8_t
  {
    /// kovsh, photoy2k and the CAVE games: the internal ROM alone, a latch the
    /// ARM writes back into, and 64 bytes of shared RAM or none.
    TYPE1,
    /// kov2, martmast, ddp2...: an external ROM, 64 KB of shared RAM, and a
    /// latch whose write by the 68000 raises FIQ.
    TYPE2,
    /// dmnfrnt, theglad, svg...: two banks of shared RAM, one each side, and a
    /// write to its own address for FIQ.
    TYPE3
  };

  Type type{};
  /// The 68000's byte addresses of the latch, of its window onto the shared
  /// RAM, and, on type 3, of the write that raises FIQ; 0 for none.
  std::uint32_t latch{};
  std::uint32_t latchBytes{};
  std::uint32_t share{};
  std::uint32_t shareBytes{};
  std::uint32_t fiq{};
  /// The ARM's clock: `clockN` / `clockM` of the 50 MHz master clock.
  std::uint32_t clockN{ 2 };
  std::uint32_t clockM{ 5 };
};

class Igs027a final : public Protection, private cpu::Arm7Bus
{
public:
  /// `internalRom` is the 16 KB ROM inside the chip, as the cartridge holds
  /// it, its region already patched in; `externalRom` the cartridge's ARM ROM,
  /// empty on type 1. The external ROM must outlive the chip.
  Igs027a( Igs027aBoard board, std::vector<std::uint8_t> internalRom, std::span<std::uint8_t const> externalRom );

  [[nodiscard]] std::vector<std::uint8_t> pages() const override;
  [[nodiscard]] bool decodes( std::uint32_t address ) const override;
  std::uint16_t read( Time& time, std::uint32_t address, bool upper, bool lower ) override;
  void write( Time& time, std::uint32_t address, std::uint16_t value, bool upper, bool lower ) override;
  [[nodiscard]] std::uint16_t peek( std::uint32_t address ) const override;
  void reset( Time releasedAt ) override;
  void advanceTo( Time now ) override;
  void serialize( StateWriter& archive ) override;
  void serialize( StateReader& archive ) override;

  [[nodiscard]] Igs027a const* igs027a() const override;

  [[nodiscard]] cpu::Arm7 const& arm() const;
  /// The `size` bytes at `address` as the ARM would read them, without what
  /// reading some of them does.
  [[nodiscard]] std::uint32_t peekArm( std::uint32_t address, unsigned size ) const;

private:
  // The ARM's bus.
  std::uint32_t read( std::uint32_t address, unsigned size, unsigned access ) override;
  void write( std::uint32_t address, unsigned size, std::uint32_t value, unsigned access ) override;

  /// The 32-bit word the ARM reads at `address`, with what reading it does.
  std::uint32_t readWord( std::uint32_t address );
  /// The same word, without.
  [[nodiscard]] std::uint32_t wordAt( std::uint32_t address ) const;
  /// The internal and the external ROM's words at `address`.
  [[nodiscard]] std::uint32_t internalRomWord( std::uint32_t address ) const;
  [[nodiscard]] std::uint32_t externalRomWord( std::uint32_t address ) const;
  /// The index of the shared RAM's word for the ARM at `address`, in its bank.
  [[nodiscard]] std::size_t sharedIndexForArm( std::uint32_t address ) const;
  /// The index of the internal RAM's word at `address`, in type 3's second
  /// RAM when `second`.
  [[nodiscard]] std::size_t internalRamIndex( std::uint32_t address, bool& second ) const;
  /// Writes the bytes of `mask` of `value` to the word at `address`.
  void writeWord( std::uint32_t address, std::uint32_t value, std::uint32_t mask );
  /// The index of the shared RAM's word for the 68000 at its byte `address`,
  /// in its bank; `high` says which half of it the address is.
  [[nodiscard]] std::size_t sharedIndexFor68k( std::uint32_t address, bool& high ) const;
  [[nodiscard]] std::int64_t armCycles( Time time ) const;

  template <class Archive>
  void serializeState( Archive& archive );

  Igs027aBoard mBoard;
  std::vector<std::uint8_t> mInternalRom;
  std::span<std::uint8_t const> mExternalRom;
  cpu::Arm7 mArm;

  /// Type 1 and 2's RAM, 64 KB at 0x10000000 and again at 0x18000000; on type
  /// 3 the 256 KB at 0x18000000.
  std::vector<std::uint32_t> mRam;
  /// Type 3's 1 KB at 0x10000000.
  std::array<std::uint32_t, 0x100> mRam2{};
  /// The shared RAM: one 64 KB chip, or on type 3 two, the ARM on one and the
  /// 68000 on the other, `mBank` saying which is the ARM's.
  std::vector<std::uint32_t> mShared;
  std::uint32_t mBank{ 1 };
  /// The table at 0x50000000, with which the RTL XORs type 2's external ROM;
  /// the image's is decrypted whole, so it is RAM and nothing more.
  std::array<std::uint32_t, 0x100> mXorTable{};
  /// What the ARM wrote for the 68000, and the 68000 for the ARM.
  std::uint32_t mLatchToM68k{};
  std::uint32_t mLatchToArm{};
  /// Type 1's counter at 0x4000000C, one more each time it is read.
  std::uint32_t mCounter{ 1 };
  bool mFiq{};
  /// The ARM's clock pulses when its reset was let go.
  std::int64_t mPulsesAtStart{};
};

} // namespace pgm::machine
