// The headless command line is deliberately thin: it parses arguments, wires a
// transport to the dispatcher and hands over. Everything testable lives in
// pgm_core and pgm_server.

#include "pgm/Emulator.hpp"
#include "pgm/Version.hpp"
#include "pgm/cart/PgmImage.hpp"
#include "pgm/control/Batch.hpp"
#include "pgm/control/Describe.hpp"
#include "pgm/control/Dispatcher.hpp"
#include "pgm/machine/Machine.hpp"
#include "pgm/server/JsonLinesServer.hpp"
#include "pgm/server/McpServer.hpp"
#include "pgm/server/TcpLineServer.hpp"

#include <CLI/CLI.hpp>
#include <spdlog/fmt/fmt.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace
{

/// What --batch exits with when its script needs a game or a program that is
/// not there: what CTest takes for a skipped test.
constexpr int EXIT_SKIPPED = 77;

/// Runs the batch script at `path`, docs/spec/batch.md, and checks its sums
/// against the golden ones the script holds, or records them into it.
int runScript( pgm::Settings settings, std::filesystem::path const& path, bool record )
{
  std::ifstream file{ path };
  auto script = pgm::control::Json::parse( file, nullptr, false );
  if ( script.is_discarded() || !script.is_object() )
  {
    std::cerr << "pgmemu-cli: " << path.string() << " is not a JSON object\n";
    return 2;
  }
  auto const result = pgm::control::runBatch( std::move( settings ), script );
  switch ( result.outcome )
  {
  case pgm::control::BatchResult::Outcome::MISSING:
    std::cout << path.filename().string() << ": skipped: " << result.message << '\n';
    return EXIT_SKIPPED;
  case pgm::control::BatchResult::Outcome::FAILED:
    std::cerr << path.filename().string() << ": " << result.message << '\n';
    return 2;
  case pgm::control::BatchResult::Outcome::RAN:
    break;
  }

  if ( record )
  {
    script["golden"] = pgm::control::goldenOf( result );
    std::ofstream out{ path };
    out << script.dump( 2 ) << '\n';
    std::cout << path.filename().string() << ": recorded " << result.checkpoints.size() << " checkpoints\n";
    return out ? 0 : 2;
  }
  auto const differences = pgm::control::compareWithGolden(
      result, script.contains( "golden" ) ? script.at( "golden" ) : pgm::control::Json{} );
  for ( std::string const& difference : differences )
  {
    std::cout << path.filename().string() << ": " << difference << '\n';
  }
  if ( differences.empty() )
  {
    std::cout << path.filename().string() << ": " << result.checkpoints.size() << " checkpoints match\n";
  }
  return differences.empty() ? 0 : 1;
}

/// Runs `game` for `frames` frames unthrottled and prints how fast that went.
int benchmark( pgm::Settings settings, std::string const& game, std::int64_t frames, std::int64_t skip )
{
  pgm::Emulator emulator{ std::move( settings ) };
  if ( auto const loaded = emulator.loadGameByName( game ); !loaded )
  {
    std::cerr << "pgmemu-cli: " << loaded.error().message << '\n';
    return EXIT_SKIPPED;
  }
  pgm::machine::Machine& machine = *emulator.machine();
  machine.reset( 100 );
  machine.runFrames( skip );
  auto const started = std::chrono::steady_clock::now();
  machine.runFrames( frames );
  double const seconds = std::chrono::duration<double>( std::chrono::steady_clock::now() - started ).count();
  double const emulated = static_cast<double>( frames * pgm::machine::UNITS_PER_FRAME ) /
                          static_cast<double>( pgm::machine::UNITS_PER_SECOND );
  std::cout << fmt::format( "{}: {} frames in {:.2f} s, {:.0f} frames a second, {:.1f} times real time\n",
                            game,
                            frames,
                            seconds,
                            static_cast<double>( frames ) / seconds,
                            emulated / seconds );
  return 0;
}

/// Loads `game`, a set name or the path of a .pgm, before any request is
/// answered; false, with the reason on stderr, when it cannot be loaded.
bool loadAtStart( pgm::Emulator& emulator, std::string const& game )
{
  bool const isPath = game.find_first_of( "/.\\" ) != std::string::npos;
  auto const loaded = isPath ? emulator.loadGameFromFile( game ) : emulator.loadGameByName( game );
  if ( !loaded )
  {
    std::cerr << "pgmemu-cli: " << loaded.error().message << '\n';
    return false;
  }
  return true;
}

/// `answer` with the build's version written `devel`, as a transcript holds
/// it, so that any build checks a transcript alike.
std::string asRecorded( std::string answer )
{
  std::string const version{ pgm::versionString() };
  for ( std::size_t at = answer.find( version ); at != std::string::npos; at = answer.find( version, at ) )
  {
    answer.replace( at, version.size(), "devel" );
    at += 5;
  }
  return answer;
}

/// Answers the requests of the transcript at `path`, one a line, as JSON
/// lines or as MCP messages, and prints the answers; or, given `expected`,
/// compares them with its lines (docs/spec/control-protocol.md §1).
int runTranscript( pgm::control::Dispatcher const& dispatcher,
                   std::filesystem::path const& path,
                   bool mcp,
                   std::filesystem::path const& expected )
{
  std::ifstream requests{ path };
  if ( !requests )
  {
    std::cerr << "pgmemu-cli: cannot read " << path.string() << '\n';
    return 2;
  }
  pgm::server::JsonLinesServer const lines{ dispatcher };
  pgm::server::McpServer mcpServer{ dispatcher };
  std::vector<std::string> answers;
  std::string line;
  while ( std::getline( requests, line ) )
  {
    if ( !line.empty() && line.back() == '\r' )
    {
      line.pop_back();
    }
    if ( line.empty() || line.starts_with( '#' ) )
    {
      continue;
    }
    if ( !mcp )
    {
      answers.push_back( asRecorded( lines.handleLine( line ) ) );
    }
    else if ( auto answer = mcpServer.handleLine( line ) )
    {
      answers.push_back( asRecorded( std::move( *answer ) ) );
    }
  }

  if ( expected.empty() )
  {
    for ( std::string const& answer : answers )
    {
      std::cout << answer << '\n';
    }
    return 0;
  }
  std::ifstream file{ expected };
  std::vector<std::string> recorded;
  while ( std::getline( file, line ) )
  {
    if ( !line.empty() && line.back() == '\r' )
    {
      line.pop_back();
    }
    recorded.push_back( line );
  }
  for ( std::size_t i = 0; i < std::max( answers.size(), recorded.size() ); ++i )
  {
    std::string const got = i < answers.size() ? answers[i] : "(no answer)";
    std::string const want = i < recorded.size() ? recorded[i] : "(no answer)";
    if ( got != want )
    {
      std::cout << fmt::format(
          "{}: answer {} differs\n  expected: {}\n  got:      {}\n", path.filename().string(), i + 1, want, got );
      return 1;
    }
  }
  std::cout << fmt::format( "{}: {} answers match\n", path.filename().string(), answers.size() );
  return 0;
}

/// Prints what `emu.cartridge_info` would answer about the file at `path`.
int printInfo( std::filesystem::path const& path )
{
  auto const image = pgm::cart::PgmImage::read( path );
  if ( !image )
  {
    std::cerr << "pgmemu-cli: " << path.string() << ": " << image.error() << '\n';
    return 1;
  }
  std::cout << pgm::control::describe( *image ).dump( 2 ) << '\n';
  return 0;
}

int run( int argc, char** argv )
{
  CLI::App app{ "Headless IGS PGM emulator", "pgmemu-cli" };
  app.set_version_flag( "--version", std::string{ pgm::versionString() } );

  pgm::Settings settings;
  app.add_option( "--bios",
                  settings.biosSources,
                  "A directory or zip the BIOS files are taken from; repeat to search several, first first" )
      ->check( CLI::ExistingPath );
  app.add_option( "--rom-dir", settings.romDirectory, "Where emu.load_game finds <name>.pgm" )
      ->check( CLI::ExistingDirectory );
  app.add_option( "--state-dir",
                  settings.stateDirectory,
                  "Where state.save and state.load keep save states given by name; the working directory if not given" )
      ->check( CLI::ExistingDirectory );

  std::string serverSpec;
  auto* const serverFlag =
      app.add_option( "--server",
                      serverSpec,
                      "Answer JSON-lines requests: on stdin and stdout, or with tcp:PORT on 127.0.0.1:PORT until "
                      "the process is ended" )
          ->expected( 0, 1 );
  bool mcp = false;
  auto* const mcpFlag =
      app.add_flag( "--mcp", mcp, "Serve the Model Context Protocol on stdio, for an agent" )->excludes( serverFlag );
  std::filesystem::path batch;
  auto* const batchOption =
      app.add_option( "--batch", batch, "Run a batch script (docs/spec/batch.md) and check it against its golden sums" )
          ->check( CLI::ExistingFile )
          ->excludes( serverFlag )
          ->excludes( mcpFlag );
  bool record = false;
  app.add_flag( "--record", record, "With --batch: write the sums into the script as its golden ones" )
      ->needs( batchOption );
  std::string benchmarked;
  auto* const benchmarkOption =
      app.add_option( "--benchmark", benchmarked, "Run a game unthrottled for --frames frames, and say how fast" )
          ->excludes( serverFlag )
          ->excludes( mcpFlag )
          ->excludes( batchOption );
  std::int64_t frames = 3600;
  app.add_option( "--frames", frames, "With --benchmark: how many frames; a minute's worth if not given" )
      ->needs( benchmarkOption )
      ->check( CLI::PositiveNumber );
  std::int64_t skip = 0;
  app.add_option( "--skip", skip, "With --benchmark: frames to run before timing starts, to measure past the boot" )
      ->needs( benchmarkOption )
      ->check( CLI::NonNegativeNumber );
  std::filesystem::path info;
  app.add_option( "--info", info, "Describe a .pgm file and exit" )
      ->check( CLI::ExistingFile )
      ->excludes( serverFlag )
      ->excludes( mcpFlag );
  std::filesystem::path transcript;
  auto* const transcriptOption =
      app.add_option( "--transcript",
                      transcript,
                      "Answer the requests in a file, one a line (MCP messages with --mcp), and print the answers" )
          ->check( CLI::ExistingFile )
          ->excludes( serverFlag )
          ->excludes( batchOption )
          ->excludes( benchmarkOption );
  std::filesystem::path expected;
  app.add_option( "--expect", expected, "With --transcript: compare the answers with this file's lines instead" )
      ->check( CLI::ExistingFile )
      ->needs( transcriptOption );
  std::string game;
  app.add_option( "game",
                  game,
                  "With --server, --mcp or --transcript: a game to load before the first request, by set name (pgm "
                  "for the BIOS alone) or by the path of a .pgm" );

  CLI11_PARSE( app, argc, argv );

  // --server's value may be left out, so a game right after it is taken for
  // its value: one that is not tcp:PORT is the game.
  if ( !serverSpec.empty() && !serverSpec.starts_with( "tcp:" ) )
  {
    if ( !game.empty() )
    {
      std::cerr << "pgmemu-cli: --server: expected tcp:PORT, not " << serverSpec << '\n';
      return 1;
    }
    game = std::exchange( serverSpec, {} );
  }
  std::uint16_t port = 0;
  if ( !serverSpec.empty() )
  {
    std::string_view const digits = std::string_view{ serverSpec }.substr( 4 );
    auto const [end, error] = std::from_chars( digits.data(), digits.data() + digits.size(), port );
    if ( digits.empty() || error != std::errc{} || end != digits.data() + digits.size() )
    {
      std::cerr << "pgmemu-cli: --server: expected tcp:PORT, a port from 0 to 65535, not " << serverSpec << '\n';
      return 1;
    }
  }

  if ( !info.empty() )
  {
    return printInfo( info );
  }

  // stdout carries the protocol and nothing else, so every log line goes to
  // stderr; a client parsing stdout must never meet one.
  spdlog::set_default_logger( spdlog::stderr_color_mt( "pgmemu" ) );

  if ( !batch.empty() )
  {
    return runScript( std::move( settings ), batch, record );
  }
  if ( !benchmarked.empty() )
  {
    return benchmark( std::move( settings ), benchmarked, frames, skip );
  }

  bool const server = serverFlag->count() > 0;
  if ( !server && !mcp && transcript.empty() )
  {
    std::cerr << app.help();
    return 1;
  }

  pgm::Emulator emulator{ std::move( settings ) };
  pgm::control::Dispatcher const dispatcher{ emulator };
  if ( !game.empty() && !loadAtStart( emulator, game ) )
  {
    return 1;
  }
  if ( !transcript.empty() )
  {
    return runTranscript( dispatcher, transcript, mcp, expected );
  }
  if ( mcp )
  {
    pgm::server::McpServer transport{ dispatcher };
    spdlog::info( "pgmemu-cli {} serving MCP on stdio", pgm::versionString() );
    transport.serve( std::cin, std::cout );
    return 0;
  }
  if ( !serverSpec.empty() )
  {
    // Connections are served on threads of their own; the dispatcher answers
    // one request at a time.
    std::mutex oneAtATime;
    pgm::server::TcpLineServer const transport{ port,
                                                [&]( pgm::control::Json const& request )
                                                {
                                                  std::scoped_lock const lock{ oneAtATime };
                                                  return dispatcher.handle( request );
                                                } };
    spdlog::info( "pgmemu-cli {} serving JSON-lines on 127.0.0.1:{}", pgm::versionString(), transport.port() );
    for ( ;; )
    {
      std::this_thread::sleep_for( std::chrono::hours{ 1 } );
    }
  }
  pgm::server::JsonLinesServer const transport{ dispatcher };
  spdlog::info( "pgmemu-cli {} serving JSON-lines on stdio", pgm::versionString() );
  transport.serve( std::cin, std::cout );
  return 0;
}

} // namespace

// Nothing may escape main: an unhandled exception would abort without telling
// the user anything useful, and CLI11 reports argument problems by throwing.
// The handlers write to stderr directly rather than through spdlog, because
// logging may allocate or throw and would then be the second failure in a row.
int main( int argc, char** argv )
{
  try
  {
    return run( argc, argv );
  }
  catch ( std::exception const& error )
  {
    std::fprintf( stderr, "pgmemu-cli: %s\n", error.what() );
    return 2;
  }
  catch ( ... )
  {
    std::fputs( "pgmemu-cli: internal error: unknown exception\n", stderr );
    return 2;
  }
}
