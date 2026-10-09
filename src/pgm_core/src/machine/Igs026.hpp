#pragma once

// The IGS026 glue between the 68000, the Z80 and the RTC, ported from
// rtl/igs026_x.sv at MiSTer core commit 6f757e4: the sound latches, the Z80's
// reset, bus request and NMI, the 68000's window onto the Z80's 64 KB of RAM,
// the Z80's I/O map onto the ICS2115 and the latches, and the RTC's serial port.
//
// It also keeps the sound side's time. The 68000 leads; the Z80 and the
// ICS2115 are caught up to it whenever the 68000 reaches this chip, and when a
// run ends. Nothing else connects the two sides, so what the 68000 sees is the
// same as if they had run alongside it.

#include "Ics2115.hpp"
#include "V3021.hpp"
#include "Z80.hpp"

#include "pgm/machine/Time.hpp"

#include <array>
#include <cstdint>
#include <span>

namespace pgm::machine
{

class Igs026
{
public:
  static constexpr std::size_t Z80_RAM_SIZE = 0x10000;

  Igs026( Z80& z80, Ics2115& ics2115 );

  /// What the board's reset line does: clears the latches and the Z80's NMI,
  /// and resets the ICS2115. The Z80 runs on, as its reset is a latch bit
  /// that the reset clears; its RAM and the RTC keep their contents.
  void reset( Time now );

  /// Runs the Z80 and the ICS2115 up to `now`.
  void advanceTo( Time now );

  /// A 68000 word read or write at byte address `address` in 0xC00000-0xC1FFFF.
  std::uint16_t read( Time now, std::uint32_t address, bool upper, bool lower );
  void write( Time now, std::uint32_t address, std::uint16_t value, bool upper, bool lower );

  /// The Z80's RAM, by Z80 address: what the RTL simulator's AUDIO_RAM is.
  [[nodiscard]] std::span<std::uint8_t const> z80Ram() const;

  /// Whether the Z80 is held in reset by the 68000 (register 0xC00008 bit 0).
  [[nodiscard]] bool z80InReset() const;

  /// Names its state for a save state (StateArchive.hpp).
  template <class Archive>
  void serialize( Archive& archive )
  {
    archive( mLatch );
    archive( mZ80Ram );
    archive( mZ80Nmi );
    mRtc.serialize( archive );
    archive( mZ80Pulses );
    archive( mIcs2115DueAt );
  }

private:
  class Z80Side;

  /// Whether the 68000 has asked for the Z80's bus: 0x45D3 in 0xC0000A.
  [[nodiscard]] bool busRequested() const;
  /// Whether the 68000 owns the Z80's RAM: it has asked for the bus, and the
  /// Z80 is in reset or has granted it.
  [[nodiscard]] bool busGranted() const;

  /// Lets a Z80 stopped for the bus go on, with the access it stopped at.
  void releaseBus();

  /// Brings the ICS2115 to the time of the Z80's tick `z80Pulses`, the ce_33m
  /// pulse it shares with that ce_8m pulse, and the Z80's /INT up to date.
  void syncIcs2115( std::int64_t z80Pulses );

  Z80& mZ80;
  Ics2115& mIcs2115;

  // latch[1], [2], [4], [5], [6] of the RTL, at 0xC00002 to 0xC0000C.
  std::array<std::uint16_t, 8> mLatch{};
  std::array<std::uint8_t, Z80_RAM_SIZE> mZ80Ram{};
  bool mZ80Nmi{};
  V3021 mRtc;

  /// Pulses of ce_8m the Z80 has been run to.
  std::int64_t mZ80Pulses{};
  /// The Z80 tick before which the ICS2115 next has to be brought up to date.
  std::int64_t mIcs2115DueAt{};
};

} // namespace pgm::machine
