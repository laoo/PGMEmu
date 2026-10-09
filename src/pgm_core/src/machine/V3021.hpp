#pragma once

// The V3021 serial real-time clock, ported from rtl/v3021.sv at MiSTer core
// commit 6f757e4.
//
// It is reached through one bit of IGS026 register 0xC00006: every bus cycle
// to that address is one clock edge of the chip's serial port, a write shifts
// a bit in and a read takes one out. The RTL's behaviour is kept where it
// departs from the datasheet (docs/hardware/): its "second" advances once per
// 65536 pulses of a clock PGM.sv derives from the Z80's, about every 1.77 s,
// and the clock starts from zero rather than from the host's time.

#include "pgm/machine/Time.hpp"

#include <array>
#include <cstdint>

namespace pgm::machine
{

class V3021
{
public:
  /// One bus cycle to the chip at `now`. `write` says whether it is a write,
  /// and `bit` is the data bit written; a read answers the bit read.
  bool access( Time now, bool write, bool bit );

  /// Names its state for a save state (StateArchive.hpp).
  template <class Archive>
  void serialize( Archive& archive )
  {
    archive( mRam );
    archive( mCounters );
    archive( mSecondsApplied );
    archive( mState );
    archive( mAddress );
    archive( mData );
    archive( mOut );
  }

private:
  /// The eight counters of the clock, BCD, in the order of the RAM addresses
  /// 2..9 they are copied from and to.
  struct Counters
  {
    std::uint8_t second{};
    std::uint8_t minute{};
    std::uint8_t hour{};
    std::uint8_t day{};
    std::uint8_t month{};
    std::uint8_t year{};
    std::uint8_t weekday{};
    std::uint8_t week{};
  };

  /// Applies every tick of the clock up to `now`.
  void advanceTo( Time now );
  void tickSecond();
  void loadCountersFromRam();
  [[nodiscard]] std::uint8_t lastDayOfMonth() const;

  std::array<std::uint8_t, 16> mRam{};
  Counters mCounters{ .month = 0x01, .weekday = 0x01 };
  std::int64_t mSecondsApplied{};
  std::uint8_t mState{};
  std::uint8_t mAddress{};
  std::uint8_t mData{};
  bool mOut{};
};

} // namespace pgm::machine
