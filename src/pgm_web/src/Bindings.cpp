// The core's binding to JavaScript, for a page in a browser
// (docs/decisions/0025-the-core-compiles-to-webassembly.md). It is thin on
// purpose: the page writes files into Emscripten's in-memory file system and
// loads them by path, runs frames, holds the controls, and takes the picture
// and the sound; every other capability is the control protocol, through
// handle().

#include "pgm/Emulator.hpp"
#include "pgm/control/Dispatcher.hpp"
#include "pgm/video/Screen.hpp"

#include <emscripten/bind.h>
#include <emscripten/val.h>

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace
{

/// The emulator a page drives. The BIOS is looked for in /bios until
/// `emu.set_bios` names its sources, and save states named without a
/// directory are kept in /states; the page may mount both on IndexedDB.
class WebEmulator
{
public:
  WebEmulator() : mEmulator{ settings() }, mDispatcher{ mEmulator } {}

  /// Loads the `.pgm` at `path` with the BIOS, as the region `region` names
  /// ("JAPN"), or as its image says when empty. Answers the error, or empty.
  std::string load( std::string const& path, std::string const& region )
  {
    auto const loaded =
        mEmulator.loadGameFromFile( path, region.empty() ? std::nullopt : std::optional<std::string_view>{ region } );
    return loaded ? std::string{} : loaded.error().message;
  }

  /// Runs a frame; whether it ran whole. A run that stops at a breakpoint or
  /// a watchpoint pauses the emulator, as the desktop application's does.
  bool runFrame()
  {
    pgm::machine::Machine* const machine = mEmulator.machine();
    if ( machine == nullptr )
    {
      return false;
    }
    pgm::machine::RunResult const run = machine->runFrames( 1 );
    if ( run.reason == pgm::machine::StopReason::BREAKPOINT || run.reason == pgm::machine::StopReason::WATCHPOINT )
    {
      mEmulator.setPaused( true );
    }
    return run.reason == pgm::machine::StopReason::COMPLETED;
  }

  /// Whether the page's own run is paused: by setPaused(), by `emu.pause`, or
  /// by a breakpoint or a watchpoint that runFrame() reached.
  [[nodiscard]] bool paused() const
  {
    return mEmulator.paused();
  }

  void setPaused( bool paused )
  {
    mEmulator.setPaused( paused );
  }

  /// The controls the page holds, as the board's input words IN0 to IN3
  /// (PGM.sv), a set bit for a control held; beside those the control
  /// protocol holds.
  void setInputs( unsigned in0, unsigned in1, unsigned in2, unsigned in3 )
  {
    if ( pgm::machine::Machine* const machine = mEmulator.machine() )
    {
      machine->setHostInputs( { static_cast<std::uint16_t>( in0 ),
                                static_cast<std::uint16_t>( in1 ),
                                static_cast<std::uint16_t>( in2 ),
                                static_cast<std::uint16_t>( in3 ) } );
    }
  }

  /// The last complete picture's RGBA, SCREEN_WIDTH by SCREEN_HEIGHT, a view
  /// of the module's memory valid until the next run or load.
  [[nodiscard]] emscripten::val picture() const
  {
    pgm::machine::Machine const* const machine = mEmulator.machine();
    if ( machine == nullptr )
    {
      return emscripten::val::null();
    }
    auto const pixels = machine->picture();
    return emscripten::val( emscripten::typed_memory_view( pixels.size(), pixels.data() ) );
  }

  /// Whether the game's monitor stood on its side: its picture is to be shown
  /// turned a quarter anticlockwise.
  [[nodiscard]] bool vertical() const
  {
    pgm::cart::PgmImage const* const cartridge = mEmulator.cartridge();
    return cartridge != nullptr && cartridge->orientation() == pgm::cart::Orientation::VERTICAL;
  }

  /// The last run's sound: interleaved left and right samples, 16-bit, at
  /// audioRate(); a view valid until the next call.
  [[nodiscard]] emscripten::val audio()
  {
    pgm::machine::Machine const* const machine = mEmulator.machine();
    if ( machine == nullptr )
    {
      return emscripten::val::null();
    }
    mSamples.clear();
    for ( pgm::machine::AudioFrame const& frame : machine->audio() )
    {
      mSamples.push_back( frame.left );
      mSamples.push_back( frame.right );
    }
    return emscripten::val( emscripten::typed_memory_view( mSamples.size(), mSamples.data() ) );
  }

  /// The rate audio() comes at now, in frames per second: the ICS2115's own,
  /// which follows the number of voices the game has it play.
  [[nodiscard]] double audioRate() const
  {
    pgm::machine::Machine const* const machine = mEmulator.machine();
    return machine == nullptr ? 0.0 : machine->audioRate();
  }

  /// A request of the control protocol, as one line of JSON; its answer.
  [[nodiscard]] std::string handle( std::string const& request ) const
  {
    auto const parsed = pgm::control::Json::parse( request, nullptr, false );
    return mDispatcher.handle( parsed.is_discarded() ? pgm::control::Json{} : parsed ).dump();
  }

private:
  static pgm::Settings settings()
  {
    pgm::Settings settings;
    settings.biosSources = { "/bios" };
    settings.romDirectory = "/roms";
    settings.stateDirectory = "/states";
    return settings;
  }

  pgm::Emulator mEmulator;
  pgm::control::Dispatcher mDispatcher;
  std::vector<std::int16_t> mSamples;
};

} // namespace

EMSCRIPTEN_BINDINGS( pgmemu )
{
  emscripten::constant( "SCREEN_WIDTH", static_cast<unsigned>( pgm::video::SCREEN_WIDTH ) );
  emscripten::constant( "SCREEN_HEIGHT", static_cast<unsigned>( pgm::video::SCREEN_HEIGHT ) );

  emscripten::class_<WebEmulator>( "PgmEmu" )
      .constructor<>()
      .function( "load", &WebEmulator::load )
      .function( "runFrame", &WebEmulator::runFrame )
      .function( "paused", &WebEmulator::paused )
      .function( "setPaused", &WebEmulator::setPaused )
      .function( "setInputs", &WebEmulator::setInputs )
      .function( "picture", &WebEmulator::picture )
      .function( "vertical", &WebEmulator::vertical )
      .function( "audio", &WebEmulator::audio )
      .function( "audioRate", &WebEmulator::audioRate )
      .function( "handle", &WebEmulator::handle );
}
