#pragma once

// Counts of the clock enables PGM.sv derives by jtframe_frac_cen (MiSTer core
// commit 6f757e4), as closed forms of the master tick count, so that a device
// can ask how many pulses of its clock have passed without the machine stepping
// it through every tick.
//
// jtframe_frac_cen adds n to an accumulator on every enabled input pulse and
// fires when it reaches m, so after k pulses it has fired floor(k * n / m)
// times. Its second and third outputs fire on the 0->1 transitions of bits 0
// and 1 of a count of the first: the 1st, 3rd, 5th... and the 2nd, 6th, 10th...

#include <cstdint>

namespace pgm::machine
{

/// ce_33m's rate in Hz.
inline constexpr double CE_33M_HZ = 50'000'000.0 * 615 / 908;

/// Pulses of ce_33m (the ICS2115's 33.865 MHz, 615/908 of the master clock)
/// after `masterTicks` ticks.
constexpr std::int64_t ce33mPulses( std::int64_t masterTicks )
{
  return masterTicks * 615 / 908;
}

/// Pulses of ce_8m (the Z80's 8.466 MHz): the 2nd, 6th, 10th... ce_33m.
constexpr std::int64_t ce8mPulses( std::int64_t masterTicks )
{
  return ( ce33mPulses( masterTicks ) + 2 ) / 4;
}

/// Pulses of the RTC's clock, named ce_32khz in PGM.sv: one ce_8m in 228.
constexpr std::int64_t rtcPulses( std::int64_t masterTicks )
{
  return ce8mPulses( masterTicks ) / 228;
}

} // namespace pgm::machine
