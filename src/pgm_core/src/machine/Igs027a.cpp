#include "Igs027a.hpp"

#include "StateArchive.hpp"

#include <algorithm>

namespace pgm::machine
{

namespace
{

// The ARM's map, igs027a.sv's sel_* decode.
constexpr std::uint32_t INTERNAL_ROM_END = 0x4000;
constexpr std::uint32_t TYPE1_LATCH = 0x40000000;
constexpr std::uint32_t TYPE1_COUNTER = 0x4000000c;
constexpr std::uint32_t TYPE1_SHARE = 0x50800000;
constexpr std::uint32_t TYPE2_LATCH = 0x38000000;
constexpr std::uint32_t TYPE2_SHARE = 0x48000000;
constexpr std::uint32_t TYPE3_LATCH = 0x48000000;
constexpr std::uint32_t TYPE3_SHARE = 0x38000000;
constexpr std::uint32_t TYPE3_BANK = 0x40000018;
constexpr std::uint32_t XOR_TABLE = 0x50000000;
/// The external ROM, repeated every 8 MB through 0x08xxxxxx.
constexpr std::uint32_t EXTERNAL_ROM_AT = 0x08000000;
constexpr std::uint32_t EXTERNAL_ROM_MIRROR = 0x800000;
/// The vector fetch that takes type 3's FIQ down.
constexpr std::uint32_t FIQ_VECTOR = 0x1c;

constexpr std::size_t SHARE_WORDS = 0x4000;

/// Where `address`'s byte lands in its word, as a mask, for an access of
/// `size` bytes.
std::uint32_t laneMask( std::uint32_t address, unsigned size )
{
  if ( size == 4 )
  {
    return 0xffffffffU;
  }
  if ( size == 2 )
  {
    return ( address & 2U ) != 0 ? 0xffff0000U : 0x0000ffffU;
  }
  return 0xffU << ( ( address & 3U ) * 8 );
}

/// `value`, `size` bytes of it, in every lane, as the core drives its bus.
std::uint32_t replicate( std::uint32_t value, unsigned size )
{
  if ( size == 1 )
  {
    return ( value & 0xffU ) * 0x01010101U;
  }
  if ( size == 2 )
  {
    return ( value & 0xffffU ) * 0x00010001U;
  }
  return value;
}

} // namespace

Igs027a::Igs027a( Igs027aBoard board, std::vector<std::uint8_t> internalRom, std::span<std::uint8_t const> externalRom )
    : mBoard{ board }, mInternalRom{ std::move( internalRom ) }, mExternalRom{ externalRom }, mArm{ *this },
      mRam( board.type == Igs027aBoard::Type::TYPE3 ? 0x10000 : 0x4000 ),
      mShared( board.type == Igs027aBoard::Type::TYPE3 ? 2 * SHARE_WORDS : SHARE_WORDS )
{
  mInternalRom.resize( INTERNAL_ROM_END );
  // The ARM fetches its code straight from the ROMs. The vectors stay on the
  // bus: type 3's FIQ vector, fetched, takes its FIQ down (readWord()).
  constexpr std::uint32_t vectorsEnd = FIQ_VECTOR + 4;
  mArm.addCodeWindow( cpu::CodeWindow{
      .start = vectorsEnd, .size = INTERNAL_ROM_END - vectorsEnd, .bytes = mInternalRom.data() + vectorsEnd } );
  if ( mBoard.type != Igs027aBoard::Type::TYPE1 )
  {
    mArm.addCodeWindow( cpu::CodeWindow{
        .start = EXTERNAL_ROM_AT,
        .size = static_cast<std::uint32_t>( std::min<std::size_t>( mExternalRom.size(), EXTERNAL_ROM_MIRROR ) ),
        .bytes = mExternalRom.data() } );
  }
}

std::vector<std::uint8_t> Igs027a::pages() const
{
  std::vector<std::uint8_t> pages;
  for ( std::uint32_t const address : { mBoard.latch, mBoard.share, mBoard.fiq } )
  {
    if ( address != 0 )
    {
      pages.push_back( static_cast<std::uint8_t>( address >> 16U ) );
    }
  }
  return pages;
}

bool Igs027a::decodes( std::uint32_t address ) const
{
  auto const within = [address]( std::uint32_t start, std::uint32_t bytes )
  { return start != 0 && address >= start && address < start + bytes; };
  return within( mBoard.latch, mBoard.latchBytes ) || within( mBoard.share, mBoard.shareBytes ) ||
         within( mBoard.fiq, 2 );
}

std::uint16_t Igs027a::read( Time& time, std::uint32_t address, bool /*upper*/, bool /*lower*/ )
{
  advanceTo( time );
  return peek( address );
}

std::uint16_t Igs027a::peek( std::uint32_t address ) const
{
  if ( address - mBoard.latch < mBoard.latchBytes )
  {
    return static_cast<std::uint16_t>( ( address & 2U ) != 0 ? mLatchToM68k >> 16U : mLatchToM68k );
  }
  if ( address - mBoard.share < mBoard.shareBytes )
  {
    bool high = false;
    std::uint32_t const word = mShared.at( sharedIndexFor68k( address, high ) );
    return static_cast<std::uint16_t>( high ? word >> 16U : word );
  }
  return 0;
}

void Igs027a::write( Time& time, std::uint32_t address, std::uint16_t value, bool upper, bool lower )
{
  advanceTo( time );
  if ( address - mBoard.latch < mBoard.latchBytes )
  {
    mLatchToArm = ( address & 2U ) != 0 ? ( mLatchToArm & 0x0000ffffU ) | ( std::uint32_t{ value } << 16U )
                                        : ( mLatchToArm & 0xffff0000U ) | value;
    if ( mBoard.type == Igs027aBoard::Type::TYPE2 )
    {
      mFiq = true;
    }
  }
  else if ( address - mBoard.share < mBoard.shareBytes )
  {
    bool high = false;
    std::uint32_t& word = mShared.at( sharedIndexFor68k( address, high ) );
    std::uint32_t mask = ( upper ? 0xff00U : 0U ) | ( lower ? 0x00ffU : 0U );
    std::uint32_t data = value;
    if ( high )
    {
      mask <<= 16U;
      data <<= 16U;
    }
    word = ( word & ~mask ) | ( data & mask );
  }
  else if ( mBoard.type == Igs027aBoard::Type::TYPE3 )
  {
    mFiq = true; // the write to its own address
  }
  mArm.setFiq( mFiq );
}

void Igs027a::reset( Time releasedAt )
{
  mLatchToM68k = 0;
  mLatchToArm = 0;
  mCounter = 1;
  mFiq = false;
  mBank = 1;
  mPulsesAtStart = masterTicks( releasedAt ) * mBoard.clockN / mBoard.clockM;
  mArm.setState( cpu::Arm7State{} );
  mArm.reset();
}

void Igs027a::advanceTo( Time now )
{
  std::int64_t const target = armCycles( now );
  while ( mArm.cycles() < target )
  {
    if ( !mBreakpoints.empty() && !mBreakpointHit && mBreakpoints.contains( mArm.pc() ) )
    {
      mBreakpointHit = registers();
    }
    mArm.step();
  }
}

std::int64_t Igs027a::armCycles( Time time ) const
{
  std::int64_t const pulses = masterTicks( time ) * mBoard.clockN / mBoard.clockM;
  return pulses - mPulsesAtStart;
}

Igs027a const* Igs027a::igs027a() const
{
  return this;
}

Igs027a* Igs027a::igs027a()
{
  return this;
}

Arm7Registers Igs027a::registers() const
{
  cpu::Arm7State const& state = mArm.state();
  Arm7Registers registers;
  for ( unsigned i = 0; i < 16; ++i )
  {
    registers.r.at( i ) = mArm.reg( i );
  }
  registers.cpsr = state.cpsr;
  switch ( state.cpsr & cpu::Arm7::MODE_MASK )
  {
  case cpu::Arm7::MODE_FIQ:
    registers.spsr = state.spsr[0];
    break;
  case cpu::Arm7::MODE_SUPERVISOR:
    registers.spsr = state.spsr[1];
    break;
  case cpu::Arm7::MODE_ABORT:
    registers.spsr = state.spsr[2];
    break;
  case cpu::Arm7::MODE_IRQ:
    registers.spsr = state.spsr[3];
    break;
  case cpu::Arm7::MODE_UNDEFINED:
    registers.spsr = state.spsr[4];
    break;
  default:
    break;
  }
  registers.pc = mArm.pc();
  registers.fiq = state.fiqLine;
  registers.cycles = state.cycles;
  return registers;
}

std::set<std::uint32_t>& Igs027a::breakpoints()
{
  return mBreakpoints;
}

std::set<std::uint32_t> const& Igs027a::breakpoints() const
{
  return mBreakpoints;
}

std::optional<Arm7Registers> const& Igs027a::breakpointHit() const
{
  return mBreakpointHit;
}

void Igs027a::clearBreakpointHit()
{
  mBreakpointHit.reset();
}

cpu::Arm7 const& Igs027a::arm() const
{
  return mArm;
}

std::uint32_t Igs027a::peekArm( std::uint32_t address, unsigned size ) const
{
  std::uint32_t const word = wordAt( address );
  if ( size == 4 )
  {
    return word;
  }
  unsigned const shift = size == 2 ? ( address & 2U ) * 8 : ( address & 3U ) * 8;
  return ( word >> shift ) & ( size == 2 ? 0xffffU : 0xffU );
}

void Igs027a::serialize( StateWriter& archive )
{
  serializeState( archive );
}

void Igs027a::serialize( StateReader& archive )
{
  serializeState( archive );
}

template <class Archive>
void Igs027a::serializeState( Archive& archive )
{
  cpu::Arm7State state = mArm.state();
  archive( state );
  mArm.setState( state );
  archive( mRam );
  archive( mRam2 );
  archive( mShared );
  archive( mBank );
  archive( mXorTable );
  archive( mLatchToM68k );
  archive( mLatchToArm );
  archive( mCounter );
  archive( mFiq );
  archive( mPulsesAtStart );
}

// ---------------------------------------------------------------------------
// The ARM's side

std::uint32_t Igs027a::read( std::uint32_t address, unsigned size, unsigned /*access*/ )
{
  // The core fetches its code through its windows, so what comes here is
  // data: the internal RAM's mostly, then the ROMs', which nothing happens
  // to, but for type 3's FIQ vector. They go straight there.
  std::uint32_t word = 0;
  std::uint32_t const top = address >> 24U;
  if ( top == 0x10 || top == 0x18 )
  {
    bool second = false;
    std::size_t const index = internalRamIndex( address, second );
    word = second ? mRam2[index] : mRam[index];
  }
  else if ( address < INTERNAL_ROM_END &&
            ( mBoard.type != Igs027aBoard::Type::TYPE3 || ( address & ~3U ) != FIQ_VECTOR ) )
  {
    word = internalRomWord( address );
  }
  else if ( ( address >> 24U ) == 0x08 && mBoard.type != Igs027aBoard::Type::TYPE1 )
  {
    word = externalRomWord( address );
  }
  else
  {
    word = readWord( address );
  }
  if ( size == 4 )
  {
    return word;
  }
  unsigned const shift = size == 2 ? ( address & 2U ) * 8 : ( address & 3U ) * 8;
  return ( word >> shift ) & ( size == 2 ? 0xffffU : 0xffU );
}

void Igs027a::write( std::uint32_t address, unsigned size, std::uint32_t value, unsigned /*access*/ )
{
  std::uint32_t const mask = laneMask( address, size );
  std::uint32_t const lanes = replicate( value, size );
  std::uint32_t const top = address >> 24U;
  if ( top == 0x10 || top == 0x18 )
  {
    // The internal RAM, straight, as read() takes it.
    bool second = false;
    std::size_t const index = internalRamIndex( address, second );
    std::uint32_t& word = second ? mRam2[index] : mRam[index];
    word = ( word & ~mask ) | ( lanes & mask );
    return;
  }
  writeWord( address, lanes, mask );
}

std::uint32_t Igs027a::readWord( std::uint32_t address )
{
  std::uint32_t const word = wordAt( address );
  auto const type = mBoard.type;
  if ( type == Igs027aBoard::Type::TYPE1 && ( address & ~3U ) == TYPE1_COUNTER )
  {
    ++mCounter;
  }
  // Type 2's FIQ goes down when the ARM reads the latch, type 3's when it
  // fetches the FIQ vector.
  bool answered = false;
  if ( type == Igs027aBoard::Type::TYPE2 )
  {
    answered = ( address & ~3U ) == TYPE2_LATCH;
  }
  else if ( type == Igs027aBoard::Type::TYPE3 )
  {
    answered = ( address & ~3U ) == FIQ_VECTOR;
  }
  if ( answered && mFiq )
  {
    mFiq = false;
    mArm.setFiq( false );
  }
  return word;
}

std::uint32_t Igs027a::internalRomWord( std::uint32_t address ) const
{
  std::size_t const at = address & ( INTERNAL_ROM_END - 4 );
  return static_cast<std::uint32_t>( mInternalRom[at] | ( mInternalRom[at + 1] << 8U ) |
                                     ( mInternalRom[at + 2] << 16U ) | ( mInternalRom[at + 3] << 24U ) );
}

std::uint32_t Igs027a::externalRomWord( std::uint32_t address ) const
{
  // The image holds the external ROM decrypted whole, the RTL's XOR of each
  // read by the table at 0x50000000 included (docs/decisions/0014).
  std::size_t const at = address & ( EXTERNAL_ROM_MIRROR - 1 ) & ~3U;
  if ( at + 4 > mExternalRom.size() )
  {
    return 0;
  }
  return static_cast<std::uint32_t>( mExternalRom[at] | ( mExternalRom[at + 1] << 8U ) |
                                     ( mExternalRom[at + 2] << 16U ) | ( mExternalRom[at + 3] << 24U ) );
}

std::uint32_t Igs027a::wordAt( std::uint32_t address ) const
{
  auto const type = mBoard.type;
  if ( address < INTERNAL_ROM_END )
  {
    return internalRomWord( address );
  }
  std::uint32_t const top = address >> 24U;
  if ( top == 0x10 || top == 0x18 )
  {
    bool second = false;
    std::size_t const index = internalRamIndex( address, second );
    return second ? mRam2.at( index ) : mRam.at( index );
  }
  if ( ( address & 0xfffff000U ) == XOR_TABLE )
  {
    return mXorTable.at( ( address >> 2U ) & 0xffU );
  }
  if ( type == Igs027aBoard::Type::TYPE1 )
  {
    if ( ( address & 0xffffff00U ) == TYPE1_SHARE )
    {
      return mShared.at( sharedIndexForArm( address ) );
    }
    if ( ( address & 0xfffffff0U ) == TYPE1_LATCH )
    {
      return ( address & ~3U ) == TYPE1_COUNTER ? mCounter : mLatchToArm;
    }
    return 0;
  }
  std::uint32_t const latch = type == Igs027aBoard::Type::TYPE2 ? TYPE2_LATCH : TYPE3_LATCH;
  std::uint32_t const share = type == Igs027aBoard::Type::TYPE2 ? TYPE2_SHARE : TYPE3_SHARE;
  if ( ( address & ~3U ) == latch )
  {
    return mLatchToArm;
  }
  if ( ( address & 0xffff0000U ) == share )
  {
    return mShared.at( sharedIndexForArm( address ) );
  }
  if ( top == 0x08 )
  {
    return externalRomWord( address );
  }
  return 0;
}

void Igs027a::writeWord( std::uint32_t address, std::uint32_t value, std::uint32_t mask )
{
  auto const merge = [&]( std::uint32_t& word ) { word = ( word & ~mask ) | ( value & mask ); };
  auto const type = mBoard.type;
  std::uint32_t const top = address >> 24U;
  if ( top == 0x10 || top == 0x18 )
  {
    bool second = false;
    std::size_t const index = internalRamIndex( address, second );
    merge( second ? mRam2.at( index ) : mRam.at( index ) );
    return;
  }
  if ( ( address & 0xfffff000U ) == XOR_TABLE )
  {
    merge( mXorTable.at( ( address >> 2U ) & 0xffU ) );
    return;
  }
  if ( type == Igs027aBoard::Type::TYPE1 )
  {
    if ( ( address & 0xffffff00U ) == TYPE1_SHARE )
    {
      merge( mShared.at( sharedIndexForArm( address ) ) );
    }
    else if ( ( address & ~3U ) == TYPE1_LATCH )
    {
      // The answer, and the 68000's command taken where it was answered.
      merge( mLatchToM68k );
      mLatchToArm &= ~mask;
    }
    return;
  }
  std::uint32_t const latch = type == Igs027aBoard::Type::TYPE2 ? TYPE2_LATCH : TYPE3_LATCH;
  std::uint32_t const share = type == Igs027aBoard::Type::TYPE2 ? TYPE2_SHARE : TYPE3_SHARE;
  if ( ( address & ~3U ) == latch )
  {
    merge( mLatchToM68k );
  }
  else if ( ( address & 0xffff0000U ) == share )
  {
    merge( mShared.at( sharedIndexForArm( address ) ) );
  }
  else if ( type == Igs027aBoard::Type::TYPE3 && ( address & ~3U ) == TYPE3_BANK )
  {
    mBank = value & 1U;
  }
}

std::size_t Igs027a::sharedIndexForArm( std::uint32_t address ) const
{
  std::size_t const bank = mBoard.type == Igs027aBoard::Type::TYPE3 ? mBank : 0;
  return ( bank * SHARE_WORDS ) + ( ( address >> 2U ) & ( SHARE_WORDS - 1 ) );
}

std::size_t Igs027a::sharedIndexFor68k( std::uint32_t address, bool& high ) const
{
  std::uint32_t const halfword = ( address - mBoard.share ) >> 1U;
  // Type 1 puts the 68000's even halfwords in the high half of the ARM's
  // words, the others in the low.
  high = mBoard.type == Igs027aBoard::Type::TYPE1 ? ( halfword & 1U ) == 0 : ( halfword & 1U ) != 0;
  std::size_t const bank = mBoard.type == Igs027aBoard::Type::TYPE3 ? 1 - mBank : 0;
  return ( bank * SHARE_WORDS ) + ( ( halfword >> 1U ) & ( SHARE_WORDS - 1 ) );
}

std::size_t Igs027a::internalRamIndex( std::uint32_t address, bool& second ) const
{
  second = false;
  if ( mBoard.type == Igs027aBoard::Type::TYPE3 )
  {
    if ( ( address >> 24U ) == 0x10 )
    {
      second = true;
      return ( address >> 2U ) & 0xffU;
    }
    return ( address >> 2U ) & 0xffffU;
  }
  return ( address >> 2U ) & 0x3fffU;
}

} // namespace pgm::machine
