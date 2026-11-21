#include "pgm/machine/Machine.hpp"

#include "Boards.hpp"
#include "Bus68k.hpp"
#include "ClockEnables.hpp"
#include "Ics2115.hpp"
#include "Igs023.hpp"
#include "Igs026.hpp"
#include "Igs027a.hpp"
#include "M68k.hpp"
#include "StateArchive.hpp"
#include "Z80.hpp"
#include "cpu/Arm7Disassembler.hpp"

#include <algorithm>
#include <optional>
#include <utility>
#include <vector>

namespace pgm::machine
{

namespace
{

constexpr std::int64_t POWER_ON_RESET_TICKS = 100;

/// Where the RTL simulator's `vblank` output rises, after line 0 begins: the
/// first pixel enable (5 master ticks), the output's two pixel delays and the
/// edge's own tick, 11 ticks in all.
constexpr Time FRAME_BOUNDARY_OFFSET = 11 * UNITS_PER_MASTER_TICK;

/// The first frame boundary strictly after `now`.
Time nextFrameBoundary( Time now )
{
  if ( now < FRAME_BOUNDARY_OFFSET )
  {
    return FRAME_BOUNDARY_OFFSET;
  }
  return ( ( ( ( now - FRAME_BOUNDARY_OFFSET ) / UNITS_PER_FRAME ) + 1 ) * UNITS_PER_FRAME ) + FRAME_BOUNDARY_OFFSET;
}

/// The next time the raster can change an interrupt: a line start or an hsync.
Time nextRasterEvent( Time now )
{
  Time const line = now / UNITS_PER_LINE * UNITS_PER_LINE;
  Time const hsync = line + ( HSYNC_START_DOT * UNITS_PER_DOT );
  return now < hsync ? hsync : line + UNITS_PER_LINE;
}

/// The level on the 68000's interrupt lines: 6 for vblank, over 4 for the
/// 62-line interrupt, as PGM.sv encodes them.
moira::u8 interruptLevel( Igs023 const& video )
{
  if ( video.irq6() )
  {
    return 6;
  }
  return video.irq4() ? 4 : 0;
}

std::span<std::uint8_t const> romOf( cart::PgmImage const* cartridge, cart::RomType type )
{
  if ( cartridge == nullptr )
  {
    return {};
  }
  auto const rom = cartridge->rom( type );
  return rom ? rom->data : std::span<std::uint8_t const>{};
}

std::uint32_t mappingOf( cart::PgmImage const* cartridge, cart::RomType type )
{
  if ( cartridge == nullptr )
  {
    return 0;
  }
  auto const rom = cartridge->rom( type );
  return rom ? rom->mapping : 0;
}

/// The region value the game is made: `chosen`, or the one its image holds.
std::uint32_t regionOf( cart::PgmImage const* cartridge, std::optional<std::uint32_t> chosen )
{
  if ( chosen )
  {
    return *chosen;
  }
  return cartridge == nullptr ? 0 : cartridge->ownRegion().value_or( 0 );
}

/// The region ASIC3 reports: the game's when its region block is ASIC3's, the
/// world otherwise. The RTL takes it from its region switches, which its
/// simulation sets to the world for orlegend.
std::uint8_t asic3Region( cart::PgmImage const* cartridge, std::uint32_t region )
{
  if ( cartridge == nullptr || !cartridge->regionInfo() ||
       cartridge->regionInfo()->scheme != cart::RegionScheme::ASIC3 )
  {
    return 0;
  }
  return static_cast<std::uint8_t>( region );
}

Sdram sdramOf( cart::Bios const& bios, cart::PgmImage const* cartridge )
{
  return Sdram{ .biosProgram = bios.program().data,
                .biosTiles = bios.tiles().data,
                .biosMusic = bios.music().data,
                .cartProgram = romOf( cartridge, cart::RomType::PRG ),
                .cartTiles = romOf( cartridge, cart::RomType::TLE ),
                .cartMusic = romOf( cartridge, cart::RomType::AUD ),
                .cartBRom = romOf( cartridge, cart::RomType::SPM ),
                .cartARom = romOf( cartridge, cart::RomType::SPC ) };
}

/// What a save state begins with, and the version of its layout, which
/// changes whenever a part's state does.
constexpr std::uint32_t STATE_MAGIC = 0x54534750; // "PGST"
constexpr std::uint32_t STATE_VERSION = 4;

/// An instruction as the trace keeps it: the time and Moira's registers as
/// they are, so that keeping one costs a run copies and no arithmetic. A
/// TraceEntry is made of it when one is asked for.
struct TraceRecord
{
  Time at{};
  /// D0-D7, then A0-A7.
  std::array<std::uint32_t, 16> r{};
  std::uint32_t pc{};
  moira::StatusRegister sr{};
};

/// The status register as the 68000 holds it, of the flags Moira keeps apart.
std::uint16_t statusWord( moira::StatusRegister const& sr )
{
  auto const bit = []( bool flag, unsigned at ) { return ( flag ? 1U : 0U ) << at; };
  return static_cast<std::uint16_t>( bit( sr.t1, 15 ) | bit( sr.s, 13 ) | ( ( sr.ipl & 7U ) << 8U ) | bit( sr.x, 4 ) |
                                     bit( sr.n, 3 ) | bit( sr.z, 2 ) | bit( sr.v, 1 ) | bit( sr.c, 0 ) );
}

TraceEntry entryOf( TraceRecord const& record )
{
  TraceEntry entry{
    .pc = record.pc, .ticks = masterTicks( record.at ), .d = {}, .a = {}, .sr = statusWord( record.sr )
  };
  std::copy_n( record.r.begin(), 8, entry.d.begin() );
  std::copy_n( record.r.begin() + 8, 8, entry.a.begin() );
  return entry;
}

} // namespace

struct Machine::Parts
{
  Parts( cart::Bios const& bios, cart::PgmImage const* cartridge, std::uint32_t region )
      : sdram{ sdramOf( bios, cartridge ) },
        video{ sdram,
               TileMapping{ .cartridge = cartridge != nullptr, .tileBase = mappingOf( cartridge, cart::RomType::TLE ) },
               workRam },
        ics2115{ sdram,
                 SampleMapping{ .cartridge = cartridge != nullptr,
                                .musicBase = mappingOf( cartridge, cart::RomType::AUD ) } },
        io{ z80, ics2115 }, asic3{ asic3Region( cartridge, region ) },
        protection{ cartridge == nullptr ? nullptr : makeProtection( *cartridge, region ) },
        bus{ RomSpace{ .sdram = &sdram,
                       .cartridge = cartridge != nullptr,
                       .cartBase = mappingOf( cartridge, cart::RomType::PRG ) },
             BusDevices{ .video = video, .io = io, .asic3 = asic3, .inputs = inputs, .protection = protection.get() },
             now,
             workRam },
        cpu{ bus, now }
  {
    cpu.watch( watch );
  }

  /// Runs until `until`, until `condition` holds after an instruction, or,
  /// when `instructions` is above zero, until that many have executed. Time
  /// running out is a timeout for a run that waited for either.
  RunResult run( Time until, std::function<bool()> const* condition, std::int64_t instructions = 0 );

  /// The instruction executed `index` before the last one.
  [[nodiscard]] TraceRecord const& traced( std::size_t index ) const
  {
    return traceRing[( traceNext + traceRing.size() - 1 - index ) % traceRing.size()];
  }

  /// The board's IGS027A, or null.
  [[nodiscard]] Igs027a* arm() const
  {
    return protection ? protection->igs027a() : nullptr;
  }

  /// Whether the Z80 or the ARM7 has breakpoints, so that they are caught up
  /// after every 68000 instruction to see whether they reached one.
  [[nodiscard]] bool sideBreakpoints() const
  {
    return !io.breakpoints().empty() || ( arm() != nullptr && !arm()->breakpoints().empty() );
  }

  /// Catches the sound side and the protection up to now. The first breakpoint
  /// either reached since the run began becomes the run's, if it has none.
  void catchUp()
  {
    io.advanceTo( now );
    if ( protection )
    {
      protection->advanceTo( now );
    }
    if ( hit )
    {
      return;
    }
    if ( auto const& sound = io.breakpointHit() )
    {
      hit = BreakpointHit{ .cpu = Cpu::Z80, .address = sound->pc, .z80 = sound, .arm7 = std::nullopt };
    }
    else if ( arm() != nullptr && arm()->breakpointHit() )
    {
      auto const& registers = *arm()->breakpointHit();
      hit = BreakpointHit{ .cpu = Cpu::ARM7, .address = registers.pc, .z80 = std::nullopt, .arm7 = registers };
    }
  }

  template <class Archive>
  void serialize( Archive& archive )
  {
    archive( now );
    archive( resetReleasedAt );
    archive( cpuStarted );
    archive( workRam );
    archive( inputs.pressed );
    video.serialize( archive );
    z80.serialize( archive );
    ics2115.serialize( archive );
    io.serialize( archive );
    asic3.serialize( archive );
    if ( protection )
    {
      protection->serialize( archive );
    }
    cpu.serialize( archive );
  }

  Time now{};
  /// The reset line is held until this time; the 68000 starts when it is let go.
  Time resetReleasedAt{};
  bool cpuStarted{};
  /// The breakpoint a run stopped at, which the next run executes rather than
  /// stopping at again.
  std::optional<std::uint32_t> resumeFrom;
  std::set<std::uint32_t> breakpoints;
  /// The breakpoint that ended the last run.
  std::optional<BreakpointHit> hit;
  WatchState watch;
  /// The last instructions executed, a ring: traceNext is where the next goes.
  std::vector<TraceRecord> traceRing = std::vector<TraceRecord>( Machine::TRACE_SIZE );
  std::size_t traceNext{};
  /// Whether an instruction's registers are kept with it.
  bool traceRegisters{};
  std::size_t traceCount{};
  /// The sound produced by the last run, and who hears it as the run ends.
  std::vector<AudioFrame> audio;
  std::function<void( std::span<AudioFrame const> )> audioListener;
  Sdram sdram;
  std::array<std::uint8_t, 0x20000> workRam{};
  InputPorts inputs;
  std::array<std::uint16_t, 4> protocolInputs{};
  std::array<std::uint16_t, 4> hostInputs{};

  void applyInputs()
  {
    for ( std::size_t i = 0; i < inputs.pressed.size(); ++i )
    {
      inputs.pressed.at( i ) = static_cast<std::uint16_t>( protocolInputs.at( i ) | hostInputs.at( i ) );
    }
  }

  Igs023 video;
  Z80 z80;
  Ics2115 ics2115;
  Igs026 io;
  Asic3 asic3;
  std::unique_ptr<Protection> protection;
  Bus68k bus;
  M68k cpu;
};

RunResult Machine::Parts::run( Time until, std::function<bool()> const* condition, std::int64_t instructions )
{
  bool const waits = condition != nullptr || instructions > 0;
  audio.clear();
  watch.hit.reset();
  hit.reset();
  io.clearBreakpointHit();
  if ( arm() != nullptr )
  {
    arm()->clearBreakpointHit();
  }
  bool const sideStops = sideBreakpoints();
  Time const start = now;
  std::int64_t const startFrame =
      start < FRAME_BOUNDARY_OFFSET ? 0 : ( ( start - FRAME_BOUNDARY_OFFSET ) / UNITS_PER_FRAME ) + 1;
  auto const result = [&]( StopReason reason )
  {
    // The sound side and the protection have only been caught up as far as
    // the 68000 last reached them; what they reach on the way is the run's
    // stop, unless the 68000 stopped for its own reason.
    catchUp();
    if ( hit && ( reason == StopReason::COMPLETED || reason == StopReason::TIMEOUT ) )
    {
      reason = StopReason::BREAKPOINT;
    }
    ics2115.takeFrames( audio );
    if ( audioListener )
    {
      audioListener( audio );
    }
    std::int64_t const endFrame =
        now < FRAME_BOUNDARY_OFFSET ? 0 : ( ( now - FRAME_BOUNDARY_OFFSET ) / UNITS_PER_FRAME ) + 1;
    return RunResult{ .reason = reason,
                      .ticks = masterTicks( now ) - masterTicks( start ),
                      .frames = endFrame - startFrame };
  };

  while ( now < until )
  {
    if ( sideStops )
    {
      catchUp();
      if ( hit )
      {
        return result( StopReason::BREAKPOINT );
      }
    }
    if ( !cpuStarted )
    {
      if ( now < resetReleasedAt )
      {
        now = std::min( resetReleasedAt, until );
        video.advanceTo( now );
        continue;
      }
      cpu.startEClock( now );
      cpu.reset();
      cpuStarted = true;
      continue;
    }

    video.advanceTo( now );
    if ( now < video.busHeldUntil() )
    {
      // Sprite DMA has the bus; the 68000 waits for it.
      now = std::min( video.busHeldUntil(), until );
      continue;
    }
    cpu.setIPL( interruptLevel( video ) );

    std::uint32_t const pc = cpu.getPC0();
    if ( !breakpoints.empty() && breakpoints.contains( pc ) && resumeFrom != pc )
    {
      resumeFrom = pc;
      hit = BreakpointHit{ .cpu = Cpu::M68K, .address = pc, .z80 = std::nullopt, .arm7 = std::nullopt };
      return result( StopReason::BREAKPOINT );
    }
    resumeFrom.reset();

    if ( cpu.stopped() && !video.irq6() && !video.irq4() )
    {
      // Nothing can wake a stopped 68000 before the raster raises an interrupt,
      // so time moves straight there rather than an instruction at a time.
      now = std::min( nextRasterEvent( now ), until );
      continue;
    }

    TraceRecord& record = traceRing[traceNext];
    record.at = now;
    record.pc = pc;
    if ( traceRegisters )
    {
      cpu.copyRegisters( record.r );
      record.sr = cpu.statusRegister();
    }
    traceNext = traceNext + 1 == traceRing.size() ? 0 : traceNext + 1;
    traceCount = std::min( traceCount + 1, traceRing.size() );
    cpu.execute();
    if ( cpu.isHalted() )
    {
      return result( StopReason::HALTED );
    }
    if ( watch.hit )
    {
      // Moira has moved its PC on by the time it writes; the instruction is
      // the one this loop started.
      watch.hit->pc = pc;
      return result( StopReason::WATCHPOINT );
    }
    if ( condition != nullptr && ( *condition )() )
    {
      return result( StopReason::CONDITION_MET );
    }
    if ( instructions > 0 )
    {
      --instructions;
      if ( instructions == 0 )
      {
        return result( StopReason::COMPLETED );
      }
    }
  }
  video.advanceTo( now );
  return result( waits ? StopReason::TIMEOUT : StopReason::COMPLETED );
}

Machine::Machine( cart::Bios const& bios, cart::PgmImage const* cartridge, std::optional<std::uint32_t> region )
    : mParts{ std::make_unique<Parts>( bios, cartridge, regionOf( cartridge, region ) ) }
{
  mParts->resetReleasedAt = POWER_ON_RESET_TICKS * UNITS_PER_MASTER_TICK;
  if ( mParts->protection )
  {
    mParts->protection->reset( mParts->resetReleasedAt );
  }
}

Machine::~Machine() = default;

void Machine::reset( std::int64_t masterTicks )
{
  Parts& parts = *mParts;
  parts.audio.clear();
  parts.io.reset( parts.now );
  parts.video.reset();
  parts.asic3.reset();
  parts.now += masterTicks * UNITS_PER_MASTER_TICK;
  if ( parts.protection )
  {
    parts.protection->reset( parts.now );
  }
  parts.video.advanceTo( parts.now );
  parts.resetReleasedAt = parts.now;
  parts.cpuStarted = false;
  parts.resumeFrom.reset();
}

RunResult Machine::runFrames( std::int64_t frames )
{
  Time until = mParts->now;
  for ( std::int64_t i = 0; i < frames; ++i )
  {
    until = nextFrameBoundary( until );
  }
  return mParts->run( until, nullptr );
}

RunResult Machine::runTicks( std::int64_t masterTicks )
{
  return mParts->run( mParts->now + ( masterTicks * UNITS_PER_MASTER_TICK ), nullptr );
}

RunResult Machine::runUntil( std::function<bool()> const& condition, std::int64_t timeoutTicks )
{
  return mParts->run( mParts->now + ( timeoutTicks * UNITS_PER_MASTER_TICK ), &condition );
}

RunResult Machine::step( std::int64_t instructions, std::int64_t timeoutTicks )
{
  if ( instructions <= 0 )
  {
    return RunResult{ .reason = StopReason::COMPLETED, .ticks = 0, .frames = 0 };
  }
  return mParts->run( mParts->now + ( timeoutTicks * UNITS_PER_MASTER_TICK ), nullptr, instructions );
}

Time Machine::now() const
{
  return mParts->now;
}

std::int64_t Machine::frame() const
{
  Time const now = mParts->now;
  return now < FRAME_BOUNDARY_OFFSET ? 0 : ( ( now - FRAME_BOUNDARY_OFFSET ) / UNITS_PER_FRAME ) + 1;
}

int Machine::line() const
{
  return Igs023::line( mParts->now );
}

int Machine::dot() const
{
  return Igs023::dot( mParts->now );
}

bool Machine::vblank() const
{
  return Igs023::vblank( mParts->now );
}

bool Machine::hblank() const
{
  return Igs023::hblank( mParts->now );
}

M68kState Machine::m68kState() const
{
  M68k const& cpu = mParts->cpu;
  M68kState state;
  for ( int i = 0; i < 8; ++i )
  {
    state.d.at( static_cast<std::size_t>( i ) ) = cpu.getD( i );
    state.a.at( static_cast<std::size_t>( i ) ) = cpu.getA( i );
  }
  state.pc = cpu.getPC0();
  state.sr = cpu.getSR();
  state.usp = cpu.getUSP();
  state.ssp = cpu.getISP();
  state.stopped = cpu.stopped();
  state.halted = cpu.isHalted();
  return state;
}

std::string Machine::disassemble( std::uint32_t address, int& length ) const
{
  return mParts->cpu.disassembleAt( address, length );
}

std::uint16_t Machine::peek( std::uint32_t address ) const
{
  return mParts->bus.peek( address );
}

std::vector<std::uint8_t> Machine::readBus( std::uint32_t address, std::size_t size ) const
{
  Bus68k const& bus = mParts->bus;
  size = address < Bus68k::ADDRESS_SPACE ? std::min<std::size_t>( size, Bus68k::ADDRESS_SPACE - address ) : 0;
  std::vector<std::uint8_t> bytes;
  bytes.reserve( size );
  for ( std::size_t i = 0; i < size; ++i )
  {
    auto const at = static_cast<std::uint32_t>( address + i );
    std::uint16_t const word = bus.peek( at );
    bytes.push_back( static_cast<std::uint8_t>( ( at & 1U ) == 0 ? word >> 8U : word ) );
  }
  return bytes;
}

bool Machine::writeBus( std::uint32_t address, std::span<std::uint8_t const> bytes )
{
  Bus68k& bus = mParts->bus;
  if ( address >= Bus68k::ADDRESS_SPACE || bytes.size() > Bus68k::ADDRESS_SPACE - address )
  {
    return false;
  }
  for ( std::size_t i = 0; i < bytes.size(); ++i )
  {
    if ( !bus.isRam( static_cast<std::uint32_t>( address + i ) ) )
    {
      return false;
    }
  }
  for ( std::size_t i = 0; i < bytes.size(); ++i )
  {
    bus.poke( static_cast<std::uint32_t>( address + i ), bytes[i] );
  }
  return true;
}

std::vector<std::uint8_t> Machine::saveState()
{
  StateWriter writer;
  std::uint32_t magic = STATE_MAGIC;
  std::uint32_t version = STATE_VERSION;
  writer( magic );
  writer( version );
  mParts->serialize( writer );
  return writer.bytes();
}

bool Machine::loadState( std::span<std::uint8_t const> state )
{
  // Read into a copy first, so that a state that turns out short or long
  // leaves this machine as it was.
  StateReader check{ state };
  std::uint32_t magic = 0;
  std::uint32_t version = 0;
  check( magic );
  check( version );
  if ( !check.good() || magic != STATE_MAGIC || version != STATE_VERSION )
  {
    return false;
  }
  std::vector<std::uint8_t> const before = saveState();
  mParts->serialize( check );
  if ( !check.good() || !check.atEnd() )
  {
    StateReader restore{ before };
    restore( magic );
    restore( version );
    mParts->serialize( restore );
    return false;
  }
  mParts->resumeFrom.reset();
  mParts->audio.clear();
  return true;
}

bool Machine::addBreakpoint( std::uint32_t address, Cpu cpu )
{
  switch ( cpu )
  {
  case Cpu::M68K:
    mParts->breakpoints.insert( address );
    return true;
  case Cpu::Z80:
    mParts->io.breakpoints().insert( address & 0xffffU );
    return true;
  case Cpu::ARM7:
    break;
  }
  if ( mParts->arm() == nullptr )
  {
    return false;
  }
  mParts->arm()->breakpoints().insert( address );
  return true;
}

void Machine::removeBreakpoint( std::uint32_t address, Cpu cpu )
{
  switch ( cpu )
  {
  case Cpu::M68K:
    mParts->breakpoints.erase( address );
    return;
  case Cpu::Z80:
    mParts->io.breakpoints().erase( address & 0xffffU );
    return;
  case Cpu::ARM7:
    break;
  }
  if ( mParts->arm() != nullptr )
  {
    mParts->arm()->breakpoints().erase( address );
  }
}

std::set<std::uint32_t> const& Machine::breakpoints( Cpu cpu ) const
{
  static std::set<std::uint32_t> const NONE;
  switch ( cpu )
  {
  case Cpu::M68K:
    return mParts->breakpoints;
  case Cpu::Z80:
    return mParts->io.breakpoints();
  case Cpu::ARM7:
    break;
  }
  return mParts->arm() != nullptr ? mParts->arm()->breakpoints() : NONE;
}

std::optional<BreakpointHit> const& Machine::breakpointHit() const
{
  return mParts->hit;
}

bool Machine::hasDebugPoints() const
{
  return !mParts->breakpoints.empty() || !mParts->watch.points.empty() || mParts->sideBreakpoints();
}

void Machine::addWatchpoint( Watchpoint watchpoint )
{
  watchpoint.address &= 0xffffffU;
  std::erase_if( mParts->watch.points, [&]( Watchpoint const& point ) { return point.address == watchpoint.address; } );
  mParts->watch.points.push_back( watchpoint );
}

void Machine::removeWatchpoint( std::uint32_t address )
{
  std::erase_if( mParts->watch.points,
                 [address]( Watchpoint const& point ) { return point.address == ( address & 0xffffffU ); } );
}

std::vector<Watchpoint> const& Machine::watchpoints() const
{
  return mParts->watch.points;
}

std::optional<WatchpointHit> const& Machine::watchpointHit() const
{
  return mParts->watch.hit;
}

void Machine::setTraceSize( std::size_t instructions )
{
  Parts& parts = *mParts;
  instructions = std::clamp<std::size_t>( instructions, 1, MAX_TRACE_SIZE );
  std::size_t const kept = std::min( parts.traceCount, instructions );
  std::vector<TraceRecord> ring( instructions );
  for ( std::size_t i = 0; i < kept; ++i )
  {
    ring[i] = parts.traced( kept - 1 - i );
  }
  parts.traceRing = std::move( ring );
  parts.traceCount = kept;
  parts.traceNext = kept % instructions;
}

std::size_t Machine::traceSize() const
{
  return mParts->traceRing.size();
}

void Machine::setTraceRegisters( bool kept )
{
  Parts& parts = *mParts;
  if ( parts.traceRegisters == kept )
  {
    return;
  }
  parts.traceRegisters = kept;
  std::ranges::fill( parts.traceRing, TraceRecord{} );
  parts.traceNext = 0;
  parts.traceCount = 0;
}

bool Machine::traceRegisters() const
{
  return mParts->traceRegisters;
}

std::size_t Machine::traceCount() const
{
  return mParts->traceCount;
}

TraceEntry Machine::traceAt( std::size_t index ) const
{
  return entryOf( mParts->traced( index ) );
}

std::vector<TraceEntry> Machine::trace( std::size_t count, std::size_t from ) const
{
  Parts const& parts = *mParts;
  count = from < parts.traceCount ? std::min( count, parts.traceCount - from ) : 0;
  std::vector<TraceEntry> entries;
  entries.reserve( count );
  for ( std::size_t i = count; i > 0; --i )
  {
    entries.push_back( entryOf( parts.traced( from + i - 1 ) ) );
  }
  return entries;
}

void Machine::setInputs( std::array<std::uint16_t, 4> const& pressed )
{
  mParts->protocolInputs = pressed;
  mParts->applyInputs();
}

void Machine::setHostInputs( std::array<std::uint16_t, 4> const& pressed )
{
  mParts->hostInputs = pressed;
  mParts->applyInputs();
}

void Machine::writeWorkRam( std::size_t offset, std::span<std::uint8_t const> bytes )
{
  auto& ram = mParts->workRam;
  if ( offset >= ram.size() )
  {
    return;
  }
  std::size_t const count = std::min( bytes.size(), ram.size() - offset );
  std::ranges::copy( bytes.first( count ), ram.begin() + static_cast<std::ptrdiff_t>( offset ) );
}

bool Machine::writeRam( Ram ram, std::size_t offset, std::span<std::uint8_t const> bytes )
{
  Parts& parts = *mParts;
  std::span<std::uint8_t> target;
  switch ( ram )
  {
  case Ram::WORK:
    target = parts.workRam;
    break;
  case Ram::VIDEO:
    target = parts.video.vramForDebugger();
    break;
  case Ram::PALETTE:
    target = parts.video.paletteForDebugger();
    break;
  case Ram::AUDIO:
    // The Z80 sees the bytes from the 68000's time on, not from where it was
    // last caught up to.
    parts.io.advanceTo( parts.now );
    target = parts.io.z80RamForDebugger();
    break;
  }
  if ( offset > target.size() || bytes.size() > target.size() - offset )
  {
    return false;
  }
  std::ranges::copy( bytes, target.begin() + static_cast<std::ptrdiff_t>( offset ) );
  return true;
}

std::span<std::uint8_t const> Machine::workRam() const
{
  return mParts->bus.workRam();
}

std::span<std::uint8_t const> Machine::videoRam() const
{
  return mParts->video.vram();
}

std::span<std::uint8_t const> Machine::paletteRam() const
{
  return mParts->video.palette();
}

std::span<std::uint8_t const> Machine::picture() const
{
  return mParts->video.frame();
}

std::int64_t Machine::picturesDrawn() const
{
  return mParts->video.framesCompleted();
}

Z80Registers Machine::z80Registers() const
{
  return mParts->z80.registers();
}

bool Machine::z80Halted() const
{
  return mParts->z80.halted();
}

void Machine::setVideoLayers( VideoLayers layers )
{
  mParts->video.setLayers( layers );
}

VideoLayers Machine::videoLayers() const
{
  return mParts->video.layers();
}

std::array<std::uint16_t, 16> Machine::videoRegisters() const
{
  return mParts->video.registers();
}

std::array<std::uint16_t, 32> Machine::zoomTable() const
{
  return mParts->video.zoomTable();
}

std::vector<SpriteInfo> Machine::sprites() const
{
  SpriteList const& list = mParts->video.spriteList();
  std::vector<SpriteInfo> sprites;
  sprites.reserve( list.count );
  for ( std::size_t i = 0; i < list.count; ++i )
  {
    sprites.push_back( decodeSprite( list.entries.at( i ) ) );
  }
  return sprites;
}

Image Machine::tiles(
    TileLayer layer, std::uint32_t first, std::uint32_t count, std::uint32_t columns, std::uint32_t palette ) const
{
  return mParts->video.tiles( layer, first, count, columns, palette );
}

Image Machine::tilemap( TileLayer layer ) const
{
  return mParts->video.tilemap( layer );
}

std::span<AudioFrame const> Machine::audio() const
{
  return mParts->audio;
}

double Machine::audioRate() const
{
  return CE_33M_HZ / static_cast<double>( mParts->ics2115.samplePeriod() );
}

void Machine::setAudioListener( std::function<void( std::span<AudioFrame const> )> listener )
{
  mParts->audioListener = std::move( listener );
}

bool Machine::audioListened() const
{
  return static_cast<bool>( mParts->audioListener );
}

std::array<Ics2115Voice, 32> const& Machine::ics2115Voices() const
{
  return mParts->ics2115.voices();
}

std::size_t Machine::ics2115ActiveVoices() const
{
  return mParts->ics2115.activeVoices();
}

std::optional<Arm7Registers> Machine::arm7Registers() const
{
  Igs027a const* const chip = mParts->arm();
  if ( chip == nullptr )
  {
    return std::nullopt;
  }
  return chip->registers();
}

std::optional<std::string> Machine::disassembleArm7( std::uint32_t address, bool thumb, int& length ) const
{
  Igs027a const* const chip = mParts->protection ? mParts->protection->igs027a() : nullptr;
  if ( chip == nullptr )
  {
    return std::nullopt;
  }
  if ( thumb )
  {
    length = 2;
    return cpu::disassembleThumb( address,
                                  static_cast<std::uint16_t>( chip->peekArm( address, 2 ) ),
                                  static_cast<std::uint16_t>( chip->peekArm( address + 2, 2 ) ) );
  }
  length = 4;
  return cpu::disassembleArm( address, chip->peekArm( address, 4 ) );
}

std::span<std::uint8_t const> Machine::z80Ram() const
{
  return mParts->io.z80Ram();
}

} // namespace pgm::machine
