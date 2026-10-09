#pragma once

#include "pgm/control/Dispatcher.hpp"

#include <functional>
#include <string>

namespace pgm::app
{

/// The CPUs' registers: a tab each for the 68000, the Z80 and the IGS027A's
/// ARM7, with the instruction the 68000 and the ARM7 execute next. It reads
/// them through `cpu.get_state`, as an agent would
/// (docs/decisions/0005-one-control-api.md), and asks again at most twice a
/// second while a tab is shown.
class CpuWindow
{
public:
  /// Answers a request of the control protocol: `method` with `params`.
  using Request = std::function<control::Json( std::string const& method, control::Json params )>;

  CpuWindow( Request request );

  /// Lays the window out while `open`, which its close button clears.
  void draw( bool& open );

private:
  /// What `cpu.get_state` last answered for a tab, and when it was asked.
  struct Answer
  {
    control::Json response;
    double fetchedAt{ -1.0 };
  };

  /// Has `layOut` fill a table of the registers of `cpu`, as `cpu.get_state`
  /// names it, from `answer`, which is asked for again when it is due; or
  /// says why there are none: no game is loaded, or the board has no such CPU.
  void drawCpu( char const* cpu, Answer& answer, void ( *layOut )( control::Json const& state ) );

  Request mRequest;
  Answer mM68k;
  Answer mZ80;
  Answer mArm7;
};

} // namespace pgm::app
