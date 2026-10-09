#pragma once

// The sound CPU, tv80s in PGM.sv at MiSTer core commit 6f757e4, on
// floooh/chips' cycle-stepped z80.h (docs/decisions/0003-cpu-cores.md). One
// tick is one T-state, one pulse of ce_8m. z80.h stays inside Z80.cpp.

#include "StateArchive.hpp"

#include "pgm/machine/Sound.hpp"

#include <cstdint>
#include <memory>

namespace pgm::machine
{

/// What the Z80's pins reach. Each access is one call, in the T-state the
/// Z80 makes it.
class Z80Bus
{
public:
  virtual std::uint8_t read( std::uint16_t address ) = 0;
  virtual void write( std::uint16_t address, std::uint8_t value ) = 0;
  virtual std::uint8_t in( std::uint16_t port ) = 0;
  virtual void out( std::uint16_t port, std::uint8_t value ) = 0;
  /// The byte on the data bus during an interrupt acknowledge.
  virtual std::uint8_t acknowledge( std::uint16_t address ) = 0;

protected:
  Z80Bus() = default;
  ~Z80Bus() = default;
  Z80Bus( Z80Bus const& ) = default;
  Z80Bus& operator=( Z80Bus const& ) = default;
  Z80Bus( Z80Bus&& ) = default;
  Z80Bus& operator=( Z80Bus&& ) = default;
};

class Z80
{
public:
  /// Powered up, about to fetch from address 0 with every register zero.
  Z80();
  ~Z80();

  Z80( Z80 const& ) = delete;
  Z80& operator=( Z80 const& ) = delete;
  Z80( Z80&& ) = delete;
  Z80& operator=( Z80&& ) = delete;

  /// What tv80s's reset does: AF, AF' and SP to 0xFFFF, I, R and the
  /// interrupt state to zero, then a fetch from address 0. The other
  /// registers keep their values.
  void reset();

  /// One T-state. With `busRequested`, a T-state that begins an opcode fetch
  /// does not make it: the Z80 stops there, between two instructions, to
  /// grant its bus, and the answer is true. resume() makes the fetch when the
  /// bus is given back.
  bool tick( Z80Bus& bus, bool busRequested = false );

  /// Makes the fetch the Z80 stopped at, with the bus given back.
  void resume( Z80Bus& bus );

  /// Whether the Z80 is stopped for a bus request.
  [[nodiscard]] bool holding() const;

  /// The level of /INT and /NMI, true when asserted. NMI is taken on its edge.
  void setInterrupt( bool asserted );
  void setNmi( bool asserted );

  /// Whether the last tick was the first of an opcode fetch: the previous
  /// instruction is complete, and the Z80 is between instructions.
  [[nodiscard]] bool instructionBoundary() const;
  [[nodiscard]] bool halted() const;

  /// The registers. Those of an instruction still in flight may not have been
  /// written yet; at an instruction boundary they have.
  [[nodiscard]] Z80Registers registers() const;
  /// Loads the registers and starts fetching at their pc.
  void setRegisters( Z80Registers const& registers );

  /// Names its state for a save state (StateArchive.hpp): z80.h's, its pins,
  /// and what it was told of its interrupt lines and the bus.
  void serialize( StateWriter& archive );
  void serialize( StateReader& archive );

private:
  struct State;
  std::unique_ptr<State> mState;
};

} // namespace pgm::machine
