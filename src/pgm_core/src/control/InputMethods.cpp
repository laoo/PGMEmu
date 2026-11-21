#include "Methods.hpp"

#include <spdlog/fmt/fmt.h>

#include <array>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace pgm::control
{

namespace
{

/// Player 1's controls, in the simulator's encoding of `input.get_state`: its
/// joystick_p1 bits, then start from bit 16. Button 4 and the coin are the
/// emulator's own; the simulator has no name for them.
struct Control
{
  std::string_view name;
  std::uint32_t bit;
};

constexpr std::uint32_t START = 1U << 16U;
constexpr std::uint32_t COIN = 1U << 20U;

constexpr std::array<Control, 11> CONTROLS{ { { .name = "right", .bit = 0x01 },
                                              { .name = "left", .bit = 0x02 },
                                              { .name = "down", .bit = 0x04 },
                                              { .name = "up", .bit = 0x08 },
                                              { .name = "button1", .bit = 0x10 },
                                              { .name = "btn1", .bit = 0x10 },
                                              { .name = "a", .bit = 0x10 },
                                              { .name = "button2", .bit = 0x20 },
                                              { .name = "button3", .bit = 0x40 },
                                              { .name = "button4", .bit = 0x80 },
                                              { .name = "start", .bit = START } } };

std::expected<std::uint32_t, Error> bitNamed( std::string_view name )
{
  if ( name == "coin" )
  {
    return COIN;
  }
  for ( Control const& control : CONTROLS )
  {
    if ( control.name == name )
    {
      return control.bit;
    }
  }
  return std::unexpected( Error{ .code = "invalid_input", .message = fmt::format( "Unknown input: {}", name ) } );
}

std::expected<std::uint32_t, Error> bitOf( Json const& params )
{
  auto const name = requireString( params, "name" );
  if ( !name )
  {
    return std::unexpected( name.error() );
  }
  return bitNamed( *name );
}

/// The controls `params[name]` lists, as one mask: an array of names, or a
/// single name.
std::expected<std::uint32_t, Error> bitsOf( Json const& params, std::string_view name )
{
  auto const field = params.find( name );
  if ( field != params.end() && field->is_string() )
  {
    return bitNamed( field->get_ref<std::string const&>() );
  }
  if ( field == params.end() || !field->is_array() )
  {
    return std::unexpected( badRequest( fmt::format( "Missing or invalid field: {}, a list of controls", name ) ) );
  }
  std::uint32_t bits = 0;
  for ( Json const& item : *field )
  {
    if ( !item.is_string() )
    {
      return std::unexpected( badRequest( fmt::format( "{} lists the names of controls", name ) ) );
    }
    auto const bit = bitNamed( item.get_ref<std::string const&>() );
    if ( !bit )
    {
      return std::unexpected( bit.error() );
    }
    bits |= *bit;
  }
  return bits;
}

/// The names of the controls in `buttons`, each once, under the name the
/// methods' descriptions give it.
Json namesOf( std::uint32_t buttons )
{
  Json names = Json::array();
  std::uint32_t named = 0;
  for ( Control const& control : CONTROLS )
  {
    if ( ( buttons & control.bit & ~named ) != 0 )
    {
      names.push_back( control.name );
      named |= control.bit;
    }
  }
  if ( ( buttons & COIN ) != 0 )
  {
    names.push_back( "coin" );
  }
  return names;
}

/// The longest `input.press` holds its controls, and lets them go, in frames.
constexpr std::uint64_t MAX_PRESS_FRAMES = 600;

/// The buttons as PGM.sv's IN0..IN3 words hold them: IN0's low byte is start,
/// up, down, left, right and buttons 1-3; IN2 has the coin in bit 0 and
/// button 4 in bit 8.
std::array<std::uint16_t, 4> inputWords( std::uint32_t buttons )
{
  auto const has = [buttons]( std::uint32_t bit ) { return ( buttons & bit ) != 0; };
  std::uint32_t in0 = 0;
  in0 |= has( START ) ? 0x01U : 0U;
  in0 |= has( 0x08 ) ? 0x02U : 0U;
  in0 |= has( 0x04 ) ? 0x04U : 0U;
  in0 |= has( 0x02 ) ? 0x08U : 0U;
  in0 |= has( 0x01 ) ? 0x10U : 0U;
  in0 |= ( buttons & 0x70U ) << 1U;
  std::uint32_t const in2 = ( has( COIN ) ? 0x01U : 0U ) | ( has( 0x80 ) ? 0x100U : 0U );
  return { static_cast<std::uint16_t>( in0 ), 0, static_cast<std::uint16_t>( in2 ), 0 };
}

Error notLoaded()
{
  return Error{ .code = "not_loaded", .message = "No game is loaded" };
}

} // namespace

void addInputMethods( Dispatcher& dispatcher, Emulator& emulator )
{
  // The buttons held through the protocol, shared by the methods below.
  auto buttons = std::make_shared<std::uint32_t>( 0 );

  // The machine a request acts on and the control it names, or why not.
  auto const target =
      [&emulator]( Json const& params ) -> std::expected<std::pair<machine::Machine*, std::uint32_t>, Error>
  {
    machine::Machine* const machine = emulator.machine();
    auto const bit = bitOf( params );
    if ( machine == nullptr || !bit )
    {
      return std::unexpected( machine == nullptr ? notLoaded() : bit.error() );
    }
    return std::pair{ machine, *bit };
  };
  auto const hold = [buttons]( machine::Machine& machine, std::uint32_t bit, bool pressed )
  {
    *buttons = pressed ? *buttons | bit : *buttons & ~bit;
    machine.setInputs( inputWords( *buttons ) );
  };

  for ( bool const pressed : { true, false } )
  {
    dispatcher.add( pressed ? "input.set" : "input.clear",
                    pressed
                        ? info( "Holds a control of player 1 down until input.clear.",
                                { { .name = "name",
                                    .type = "string",
                                    .description = "up, down, left, right, button1 to button4, start or coin." } } )
                        : info( "Lets a control of player 1 go.",
                                { { .name = "name",
                                    .type = "string",
                                    .description = "up, down, left, right, button1 to button4, start or coin." } } ),
                    [target, hold, pressed]( Json const& params ) -> Outcome
                    {
                      auto const found = target( params );
                      if ( !found )
                      {
                        return std::unexpected( found.error() );
                      }
                      hold( *found->first, found->second, pressed );
                      return Json::object();
                    } );
  }

  dispatcher.add(
      "input.press",
      info( "Presses controls of player 1 for some frames and releases them for some, running the machine through "
            "both: two frames each unless told otherwise. Answers as emu.run_frames does; a breakpoint or a "
            "watchpoint ends the press where it is reached, the controls let go.",
            { { .name = "name",
                .type = "string",
                .description = "One control: up, down, left, right, button1 to button4, start or coin. Or give "
                               "buttons.",
                .required = false },
              { .name = "buttons",
                .type = "array",
                .description = "Several controls pressed together, by the same names, in place of name.",
                .required = false },
              { .name = "frames",
                .type = "integer",
                .description = "Frames to hold them, 1 to 600; 2 if left out.",
                .required = false },
              { .name = "release_frames",
                .type = "integer",
                .description = "Frames to run after letting them go, 0 to 600; 2 if left out.",
                .required = false } } ),
      [&emulator, buttons]( Json const& params ) -> Outcome
      {
        machine::Machine* const machine = emulator.machine();
        if ( machine == nullptr )
        {
          return std::unexpected( notLoaded() );
        }
        auto const bits = params.contains( "buttons" ) ? bitsOf( params, "buttons" ) : bitOf( params );
        auto const frames = optionalUnsigned( params, "frames", 2, 1, MAX_PRESS_FRAMES );
        auto const releaseFrames = optionalUnsigned( params, "release_frames", 2, 0, MAX_PRESS_FRAMES );
        if ( !bits )
        {
          return std::unexpected( bits.error() );
        }
        if ( !frames || !releaseFrames )
        {
          return std::unexpected( !frames ? frames.error() : releaseFrames.error() );
        }
        // The controls already held stay held: the press is beside them, and
        // lets go only of what it pressed.
        machine->setInputs( inputWords( *buttons | *bits ) );
        machine::RunResult run = machine->runFrames( static_cast<std::int64_t>( *frames ) );
        machine->setInputs( inputWords( *buttons ) );
        if ( run.reason == machine::StopReason::COMPLETED && *releaseFrames > 0 )
        {
          machine::RunResult const released = machine->runFrames( static_cast<std::int64_t>( *releaseFrames ) );
          run = machine::RunResult{ .reason = released.reason,
                                    .ticks = run.ticks + released.ticks,
                                    .frames = run.frames + released.frames };
        }
        return runJson( run, *machine );
      } );

  dispatcher.add( "input.hold",
                  info( "Holds exactly the controls of player 1 listed, letting go of any other, until told "
                        "otherwise; an empty list lets go of all. Runs nothing. Answers the names held.",
                        { { .name = "buttons",
                            .type = "array",
                            .description = "Names: up, down, left, right, button1 to button4, start, coin." } } ),
                  [&emulator, buttons]( Json const& params ) -> Outcome
                  {
                    machine::Machine* const machine = emulator.machine();
                    if ( machine == nullptr )
                    {
                      return std::unexpected( notLoaded() );
                    }
                    auto const bits = bitsOf( params, "buttons" );
                    if ( !bits )
                    {
                      return std::unexpected( bits.error() );
                    }
                    *buttons = *bits;
                    machine->setInputs( inputWords( *buttons ) );
                    return Json{ { "held", namesOf( *buttons ) } };
                  } );

  dispatcher.add( "input.get_state",
                  info( "The controls held: as a mask in the RTL simulator's encoding, and by name." ),
                  [buttons]( Json const& /*params*/ ) -> Outcome
                  { return Json{ { "buttons", *buttons }, { "held", namesOf( *buttons ) } }; } );
}

} // namespace pgm::control
