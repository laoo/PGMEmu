#pragma once

// The 68000: Moira, connected to the board's bus and clock. Moira calls sync()
// before each bus cycle (MOIRA_PRECISE_TIMING), so every access reaches the bus
// at the emulated time the 68000 makes it, and devices that are caught up on
// access see the raster where the RTL's would be.

#include "Bus68k.hpp"

#include "pgm/machine/Time.hpp"

#include "Moira.h"

#include "pgm/machine/Machine.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace pgm::machine
{

/// The watchpoints the 68000's data accesses are checked against, and the
/// first access that hit one since `hit` was last cleared.
struct WatchState
{
  std::vector<Watchpoint> points;
  std::optional<WatchpointHit> hit;
};

class M68k : public moira::Moira
{
public:
  /// `time` is the machine's clock, which this CPU advances as it runs.
  M68k( Bus68k& bus, Time& time );

  /// Sets when the 68000's E clock started counting: the release of reset,
  /// which is when fx68k's E counter leaves zero.
  void startEClock( Time at );

  /// Checks every data access against `watch`'s watchpoints, which must
  /// outlive the CPU.
  void watch( WatchState& watch );

  /// Whether a STOP instruction is waiting for an interrupt.
  [[nodiscard]] bool stopped() const;

  /// D0-D7 then A0-A7, and the status register's flags, as Moira keeps them:
  /// what the trace copies before every instruction.
  void copyRegisters( std::array<std::uint32_t, 16>& to ) const
  {
    std::ranges::copy( reg.r, to.begin() );
  }

  [[nodiscard]] moira::StatusRegister const& statusRegister() const
  {
    return reg.sr;
  }

  /// The instruction at `address` as text, and its length in bytes.
  [[nodiscard]] std::string disassembleAt( std::uint32_t address, int& length ) const;

  /// Names its state for a save state (StateArchive.hpp): Moira's, which it
  /// keeps in plain fields, and the E clock's.
  template <class Archive>
  void serialize( Archive& archive )
  {
    archive( clock );
    archive( reg );
    archive( queue );
    archive( iCache );
    archive( budget );
    archive( irqMode );
    archive( ipl );
    archive( fcl );
    archive( fcSource );
    archive( exception );
    archive( loopModeDelay );
    archive( readBuffer );
    archive( writeBuffer );
    archive( flags );
    archive( mEClockOrigin );
  }

protected:
  void sync( int cycles ) override;

  /// Lengthens the interrupt acknowledge cycle to the E clock, as VPA does.
  void willInterrupt( moira::u8 level ) override;

  [[nodiscard]] moira::u8 read8( moira::u32 addr ) const override;
  [[nodiscard]] moira::u16 read16( moira::u32 addr ) const override;
  [[nodiscard]] moira::u32 read32( moira::u32 addr ) const override;
  [[nodiscard]] moira::u16 read16Dasm( moira::u32 addr ) const override;
  void write8( moira::u32 addr, moira::u8 val ) const override;
  void write16( moira::u32 addr, moira::u16 val ) const override;
  void write32( moira::u32 addr, moira::u32 val ) const override;

private:
  // Moira's client interface is const, as a read of memory looks from the
  // CPU's side; the bus it reaches is not.
  /// Records the access in mWatch if it is a data access a watchpoint covers.
  void noteAccess( std::uint32_t address, bool write, std::uint16_t value, std::uint8_t bytes ) const;

  Bus68k* mBus;
  WatchState* mWatch{};
  Time* mTime;
  Time mEClockOrigin{};
};

} // namespace pgm::machine
