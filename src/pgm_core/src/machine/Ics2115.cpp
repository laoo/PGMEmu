#include "Ics2115.hpp"

#include <algorithm>
#include <type_traits>

namespace pgm::machine
{

namespace
{

constexpr std::uint32_t MASK_29 = ( 1U << 29 ) - 1;
constexpr std::uint32_t MASK_26 = ( 1U << 26 ) - 1;
constexpr std::uint32_t MASK_24 = ( 1U << 24 ) - 1;

// osc_conf, register 0x00
constexpr std::uint8_t OSC_ULAW = 1U << 0;
constexpr std::uint8_t OSC_16BIT = 1U << 1;
constexpr std::uint8_t OSC_LOOP = 1U << 3;
constexpr std::uint8_t OSC_BIDIR = 1U << 4;
constexpr std::uint8_t OSC_IRQ = 1U << 5;
constexpr std::uint8_t OSC_INVERT = 1U << 6;
constexpr std::uint8_t OSC_IRQ_PENDING = 1U << 7;

// osc_ctl, register 0x10
constexpr std::uint8_t CTL_DONE = 1U << 0;
constexpr std::uint8_t CTL_STOP = 1U << 1;

// vol_ctrl, register 0x0D
constexpr std::uint8_t VOL_DONE = 1U << 0;
constexpr std::uint8_t VOL_STOP = 1U << 1;
constexpr std::uint8_t VOL_ROLLOVER = 1U << 2;
constexpr std::uint8_t VOL_LOOP = 1U << 3;
constexpr std::uint8_t VOL_BIDIR = 1U << 4;
constexpr std::uint8_t VOL_IRQ = 1U << 5;
constexpr std::uint8_t VOL_INVERT = 1U << 6;
constexpr std::uint8_t VOL_IRQ_PENDING = 1U << 7;

constexpr std::uint8_t SYS_CTL_RUN = 0x05;
constexpr std::uint8_t CHIP_REVISION = 0x01;
constexpr std::uint8_t DEFAULT_ACTIVE_OSC = 31;
constexpr std::uint8_t BOOT_IRQ_VOICE = 2;

// When a Z80 write still reaches voice v in the sample period that has just
// begun: up to -8 + 25 v master ticks after the tick. The sequencer of
// ics2115.sv loads one voice after another, each when the one before has been
// through the pipeline, which takes 22 to 24 ticks when both of its sample
// reads hit the per-voice cache in front of SDRAM, and longer by what SDRAM
// takes when one misses. A write takes a few cycles to land. The two numbers
// are fitted to the RTL simulation's output: they make orlegend's attract
// sound identical to it for 20 seconds, as anything from -9 to -7 does.
constexpr std::int64_t VOICE_FIRST_TICKS = -8;
constexpr std::int64_t VOICE_TICKS = 25;

/// The ce_33m pulses from a sample tick to voice v's turn.
constexpr std::array<std::int64_t, 32> VOICE_DUE = []
{
  std::array<std::int64_t, 32> due{};
  for ( std::size_t v = 0; v < due.size(); ++v )
  {
    std::int64_t const ticks = VOICE_FIRST_TICKS + ( VOICE_TICKS * static_cast<std::int64_t>( v ) );
    due.at( v ) = ticks <= 0 ? 0 : ( ( ticks * 615 ) + 907 ) / 908;
  }
  return due;
}();

constexpr Ics2115Voice DEFAULT_VOICE{ .oscConf = OSC_16BIT, .volPan = 0x7f, .volCtrl = VOL_DONE };

/// ics2115_tables.sv: amplitude by 12-bit volume index, four bits of exponent
/// over eight of mantissa.
constexpr std::array<std::uint16_t, 4096> VOLUME = []
{
  std::array<std::uint16_t, 4096> table{};
  for ( std::uint32_t i = 0; i < table.size(); ++i )
  {
    std::uint32_t const exponent = i >> 8U;
    std::uint32_t const mantissa = i & 0xffU;
    table.at( i ) = static_cast<std::uint16_t>(
        exponent == 0 ? mantissa >> 7U : ( ( ( 0x100U | mantissa ) << ( exponent - 1 ) ) + 0xffU ) >> 8U );
  }
  return table;
}();

/// ics2115_tables.sv: the pan law, attenuation by the pan value's top nibble.
constexpr std::array<std::uint16_t, 16> PAN = {
  0xfff, 508, 364, 304, 248, 200, 168, 140, 116, 96, 76, 56, 40, 28, 12, 0
};

/// ics2115_osc.sv: round(1024 * 2^(n/32)), the envelope's exponential steps.
constexpr std::array<std::uint32_t, 32> VOLUME_STEP_MANTISSA = { 1024, 1046, 1069, 1093, 1117, 1141, 1166, 1192,
                                                                 1218, 1244, 1272, 1300, 1328, 1357, 1387, 1417,
                                                                 1448, 1480, 1512, 1545, 1579, 1614, 1649, 1685,
                                                                 1722, 1760, 1798, 1838, 1878, 1919, 1961, 2004 };

/// ics2115_osc.sv's u-law expansion bases, by exponent.
constexpr std::array<std::int32_t, 8> ULAW_BASE = { 0, 132, 396, 924, 1980, 4092, 8316, 16764 };

std::int16_t ulawDecode( std::uint8_t code )
{
  std::uint32_t const inverted = ~static_cast<std::uint32_t>( code );
  std::uint32_t const exponent = ( inverted >> 4U ) & 7U;
  std::uint32_t const mantissa = inverted & 0xfU;
  std::int32_t const value =
      ( ULAW_BASE.at( exponent ) + static_cast<std::int32_t>( mantissa << ( exponent + 3 ) ) ) & 0x7fff;
  return static_cast<std::int16_t>( ( code & 0x80U ) != 0 ? value : -value );
}

/// The envelope's step per sample: linear in mode 2, otherwise 32 steps an
/// octave, starting 8 octaves up unless the mode is 0.
std::uint32_t volumeStep( std::uint8_t mode, std::uint8_t increment )
{
  if ( ( mode & 3U ) == 2 )
  {
    return static_cast<std::uint32_t>( increment ) << 10U;
  }
  std::uint32_t const e = ( mode & 3U ) == 0 ? increment : increment + 256U;
  return ( VOLUME_STEP_MANTISSA.at( e & 31U ) << ( e >> 5U ) ) >> 10U;
}

std::uint32_t bit( std::size_t voice )
{
  return 1U << voice;
}

} // namespace

Ics2115::Ics2115( Sdram const& sdram, SampleMapping samples ) : mSdram{ sdram }, mSamples{ samples }
{
  reset();
}

void Ics2115::reset()
{
  mVoices.fill( DEFAULT_VOICE );
  mActiveOsc = DEFAULT_ACTIVE_OSC;
  mOscSelect = 0;
  mRegSelect = 0;
  mSysCtl = 0;
  mIrqEnabled = 0;
  mIrqPending = 0;
  mTimerInt = 0;
  mLastIrqVoice = BOOT_IRQ_VOICE;
  mLowLatch = 0;
  mOscIrqPending = 0;
  mOscEnded = 0;
  mVolIrqPending = 0;
  mVolEnded = 0;
  mTimers = {};
  mSampleCounter = 0;
  mPass = {};
}

std::int64_t Ics2115::samplePeriod() const
{
  return ( std::int64_t{ mActiveOsc } + 1 ) * 32;
}

std::array<Ics2115Voice, Ics2115::VOICES> const& Ics2115::voices() const
{
  return mVoices;
}

std::size_t Ics2115::activeVoices() const
{
  return mActiveOsc + 1U;
}

std::int64_t Ics2115::nextEvent() const
{
  // A pass runs to its end whether or not the chip has been stopped since.
  std::int64_t const voice = mPass.active ? mPass.start + VOICE_DUE.at( mPass.next ) : NEVER;
  if ( ( mSysCtl & SYS_CTL_RUN ) != SYS_CTL_RUN )
  {
    return voice;
  }
  std::int64_t const period = samplePeriod();
  std::int64_t next = mPulses + ( mSampleCounter >= period - 1 ? 1 : period - mSampleCounter );
  for ( Timer const& timer : mTimers )
  {
    if ( timer.running )
    {
      next = std::min( next, mPulses + timer.count + 1 );
    }
  }
  return std::min( next, voice );
}

void Ics2115::advanceTo( std::int64_t pulses )
{
  while ( mPulses < pulses )
  {
    // Pulses before the next event only count; skip them in one step.
    std::int64_t const next = nextEvent();
    std::int64_t const quiet = std::min( next - 1, pulses ) - mPulses;
    if ( ( mSysCtl & SYS_CTL_RUN ) == SYS_CTL_RUN )
    {
      mSampleCounter = static_cast<std::uint16_t>( mSampleCounter + quiet );
      for ( Timer& timer : mTimers )
      {
        if ( timer.running )
        {
          timer.count -= static_cast<std::uint32_t>( quiet );
        }
      }
    }
    mPulses += quiet;
    if ( mPulses < pulses )
    {
      pulse();
    }
  }
}

void Ics2115::pulse()
{
  ++mPulses;
  if ( ( mSysCtl & SYS_CTL_RUN ) == SYS_CTL_RUN )
  {
    countPulse();
  }
  while ( mPass.active && mPass.start + VOICE_DUE.at( mPass.next ) <= mPulses )
  {
    processNextVoice();
  }
}

void Ics2115::countPulse()
{
  if ( mSampleCounter >= samplePeriod() - 1 )
  {
    mSampleCounter = 0;
    startPass();
  }
  else
  {
    ++mSampleCounter;
  }

  for ( std::size_t t = 0; t < mTimers.size(); ++t )
  {
    Timer& timer = mTimers.at( t );
    if ( !timer.running )
    {
      continue;
    }
    if ( timer.count == 0 )
    {
      mIrqPending = static_cast<std::uint8_t>( mIrqPending | ( 1U << t ) );
      // 0x43 bits 3 and 4 enable the timers' INTs.
      if ( ( mTimers[1].scale & ( 1U << ( 3 + t ) ) ) != 0 )
      {
        mTimerInt = static_cast<std::uint8_t>( mTimerInt | ( 1U << t ) );
      }
      timer.count = ( timer.period - 1 ) & MASK_24;
    }
    else
    {
      --timer.count;
    }
  }
}

void Ics2115::startPass()
{
  // A pass takes less than the shortest sample period, so this one has ended;
  // finishing it is only a safeguard.
  while ( mPass.active )
  {
    processNextVoice();
  }
  mPass = Pass{ .active = true, .start = mPulses };
}

void Ics2115::processNextVoice()
{
  processVoice( mPass.next, mPass.left, mPass.right );
  // As the sequencer does, it compares with the active voices as they are
  // after each voice.
  if ( mPass.next < mActiveOsc )
  {
    ++mPass.next;
    return;
  }
  mFrames.push_back( AudioFrame{ .at = mPass.start,
                                 .left = static_cast<std::int16_t>( std::clamp( mPass.left, -32768, 32767 ) ),
                                 .right = static_cast<std::int16_t>( std::clamp( mPass.right, -32768, 32767 ) ) } );
  mPass.active = false;
}

std::int16_t Ics2115::sampleAt( Voice const& voice, std::uint32_t address ) const
{
  // PGM.sv: the chip's 24-bit byte address, its bank in the top nibble, is read
  // a word at a time from the cartridge's samples above the music base, or
  // from the BIOS's.
  std::uint32_t const byteAddress = ( ( voice.oscSaddr & 0xfU ) << 20U ) | ( address & 0xfffffU );
  std::uint32_t const word = byteAddress >> 1U;
  std::uint32_t const musicBaseWord = ( mSamples.musicBase >> 1U ) & 0x7fffffU;
  std::uint32_t const sdramWord = mSamples.cartridge && word >= musicBaseWord
                                      ? Sdram::CART_MUSIC_AT + ( 2 * ( word - musicBaseWord ) )
                                      : Sdram::BIOS_MUSIC_AT + ( 2 * word );
  std::uint8_t const byte = mSdram.byte( sdramWord + ( byteAddress & 1U ) );

  // u-law takes precedence over 16-bit, so format 11 is u-law.
  if ( ( voice.oscConf & OSC_ULAW ) != 0 )
  {
    return ulawDecode( byte );
  }
  if ( ( voice.oscConf & OSC_16BIT ) != 0 )
  {
    // The board wires the 8-bit sample ROMs so that a 16-bit sample is the
    // addressed byte in both halves.
    return static_cast<std::int16_t>( ( byte << 8U ) | byte );
  }
  return static_cast<std::int16_t>( byte << 8U );
}

void Ics2115::processVoice( std::size_t index, std::int32_t& left, std::int32_t& right )
{
  Voice& v = mVoices.at( index );

  // Volume and pan, from the envelope as it stands.
  auto const volume = [&]( std::uint8_t pan )
  {
    std::int32_t const level =
        static_cast<std::int32_t>( v.volAcc >> 14U ) - static_cast<std::int32_t>( PAN.at( pan >> 4U ) );
    return level > 0 ? static_cast<std::int32_t>( VOLUME.at( static_cast<std::size_t>( level ) & 0xfffU ) ) : 0;
  };
  std::int32_t const leftVolume = volume( static_cast<std::uint8_t>( 255 - v.volPan ) );
  std::int32_t const rightVolume = volume( v.volPan );

  // The sample, interpolated between the two around the oscillator.
  std::uint32_t const current = ( v.oscAcc >> 9U ) & 0xfffffU;
  std::int32_t const sample1 = sampleAt( v, current );
  std::int32_t const sample2 = sampleAt( v, current + 1 );
  auto const fraction = static_cast<std::int32_t>( v.oscAcc & 0x1ffU );
  std::int32_t const sample = ( ( sample1 * 512 ) + ( ( sample2 - sample1 ) * fraction ) ) >> 9;

  if ( ( v.oscCtl & CTL_STOP ) == 0 )
  {
    left += ( sample * leftVolume ) >> 15;
    right += ( sample * rightVolume ) >> 15;
  }

  bool oscEvent = false;
  if ( ( v.oscCtl & ( CTL_STOP | CTL_DONE ) ) == 0 )
  {
    std::uint32_t const step = v.oscFc >> 1U;
    bool const backwards = ( v.oscConf & OSC_INVERT ) != 0;
    std::uint32_t const next = ( backwards ? v.oscAcc - step : v.oscAcc + step ) & MASK_29;
    std::int64_t const remaining =
        backwards ? static_cast<std::int64_t>( next ) - v.oscStart : static_cast<std::int64_t>( v.oscEnd ) - next;

    if ( remaining >= 0 )
    {
      v.oscAcc = next;
    }
    else
    {
      if ( ( v.oscConf & OSC_IRQ ) != 0 )
      {
        v.oscConf |= OSC_IRQ_PENDING;
        oscEvent = true;
      }
      if ( ( v.oscConf & OSC_LOOP ) != 0 )
      {
        bool const bidirectional = ( v.oscConf & OSC_BIDIR ) != 0;
        bool const reverse = bidirectional != ( ( v.oscConf & OSC_INVERT ) != 0 );
        if ( bidirectional )
        {
          v.oscConf ^= OSC_INVERT;
        }
        auto const overshoot = static_cast<std::uint32_t>( remaining );
        v.oscAcc = ( reverse ? v.oscEnd + overshoot : v.oscStart - overshoot ) & MASK_29;
      }
      else
      {
        v.oscCtl |= CTL_DONE;
      }
    }
  }

  // The envelope. A step of zero still checks the boundary: the BIOS ends a
  // voice by closing the window and waiting for DONE.
  bool volEvent = false;
  if ( ( v.volCtrl & ( VOL_DONE | VOL_STOP ) ) == 0 )
  {
    std::uint32_t const step = volumeStep( v.volMode, v.volIncr );
    bool const backwards = ( v.volCtrl & VOL_INVERT ) != 0;
    std::uint32_t const next = ( backwards ? v.volAcc - step : v.volAcc + step ) & MASK_26;
    std::int64_t const remaining =
        backwards ? static_cast<std::int64_t>( next ) - v.volStart : static_cast<std::int64_t>( v.volEnd ) - next;
    if ( remaining >= 0 )
    {
      v.volAcc = next;
    }
    else
    {
      v.volCtrl &= static_cast<std::uint8_t>( ~VOL_ROLLOVER );
      if ( ( v.volCtrl & VOL_IRQ ) != 0 )
      {
        v.volCtrl |= VOL_IRQ_PENDING;
        volEvent = true;
      }
      if ( ( v.volCtrl & VOL_LOOP ) != 0 )
      {
        // As the RTL has it: a bidirectional loop turns back once, at the
        // end, and never again; a plain loop going up stays where it got to.
        if ( ( v.volCtrl & VOL_BIDIR ) != 0 )
        {
          v.volCtrl |= VOL_INVERT;
        }
        else if ( backwards )
        {
          v.volAcc = ( v.volEnd - ( v.volStart - next ) ) & MASK_26;
        }
        else
        {
          v.volAcc = next;
        }
      }
      else
      {
        v.volCtrl |= VOL_DONE;
      }
    }
  }

  // The write-back sets the pending flags from this pass's events, and again
  // on every pass while a voice that an event ended keeps its IRQ enabled: the
  // IRQ is a level, measured so on the board. Whether a voice had ended is
  // judged as it was before this pass.
  auto const writeBack = [&]( std::uint32_t& pending, std::uint32_t& ended, bool event, bool done, bool enabled )
  {
    std::uint32_t const b = bit( index );
    if ( event || ( ( ended & b ) != 0 && enabled ) )
    {
      pending |= b;
    }
    if ( event && done )
    {
      ended |= b;
    }
    else if ( !done )
    {
      ended &= ~b;
    }
  };
  writeBack( mOscIrqPending, mOscEnded, oscEvent, ( v.oscCtl & CTL_DONE ) != 0, ( v.oscConf & OSC_IRQ ) != 0 );
  writeBack( mVolIrqPending, mVolEnded, volEvent, ( v.volCtrl & VOL_DONE ) != 0, ( v.volCtrl & VOL_IRQ ) != 0 );
}

bool Ics2115::irq() const
{
  // The line follows the pending flags, whatever the voices' enables now say.
  bool const voiceIrq = ( mOscIrqPending | mVolIrqPending ) != 0;
  return mTimerInt != 0 || ( ( mSysCtl & SYS_CTL_RUN ) == SYS_CTL_RUN && mIrqEnabled != 0 && voiceIrq );
}

Ics2115::IrqvScan Ics2115::irqvScan() const
{
  for ( std::size_t k = 1; k <= VOICES; ++k )
  {
    std::size_t const v = ( mLastIrqVoice + k ) % VOICES;
    if ( v > mActiveOsc )
    {
      continue;
    }
    bool const osc = ( mOscIrqPending & bit( v ) ) != 0;
    bool const vol = ( mVolIrqPending & bit( v ) ) != 0;
    if ( osc || vol )
    {
      return IrqvScan{ .found = true, .voice = static_cast<std::uint8_t>( v ), .osc = osc, .vol = vol };
    }
  }
  return {};
}

std::uint16_t Ics2115::registerValue() const
{
  // Unimplemented bits read as 1 and write-only registers as 0x78, as
  // measured on the board (ics2115.sv).
  if ( mRegSelect < 0x40 )
  {
    Voice const& v = mVoices.at( mOscSelect );
    auto const high = []( std::uint32_t value ) { return static_cast<std::uint16_t>( ( value << 8U ) | 0xffU ); };
    switch ( mRegSelect & 0x1fU )
    {
    case 0x00:
      return high( v.oscConf );
    case 0x01:
      return static_cast<std::uint16_t>( v.oscFc | 1U );
    case 0x02:
      return static_cast<std::uint16_t>( v.oscStart >> 13U );
    case 0x03:
      return high( ( v.oscStart >> 5U ) & 0xffU );
    case 0x04:
      return static_cast<std::uint16_t>( v.oscEnd >> 13U );
    case 0x05:
      return high( ( v.oscEnd >> 5U ) & 0xffU );
    case 0x06:
      return high( v.volIncr );
    case 0x07:
      return high( v.volStart >> 18U );
    case 0x08:
      return high( v.volEnd >> 18U );
    case 0x09:
      return static_cast<std::uint16_t>( v.volAcc >> 10U );
    case 0x0a:
      return static_cast<std::uint16_t>( v.oscAcc >> 13U );
    case 0x0b:
      return static_cast<std::uint16_t>( ( v.oscAcc << 3U ) | 7U );
    case 0x0c:
      return high( v.volPan | 0x0fU );
    case 0x0d:
      return high( v.volCtrl );
    case 0x0e:
      return high( 0xe0U | mActiveOsc );
    case 0x0f:
    {
      IrqvScan const scan = irqvScan();
      if ( !scan.found )
      {
        return high( 0xe0U | mLastIrqVoice );
      }
      return high( ( scan.osc ? 0U : 0x80U ) | ( scan.vol ? 0U : 0x40U ) | 0x20U | scan.voice );
    }
    case 0x10:
      return high( 0xfcU | ( v.oscCtl & 3U ) );
    case 0x11:
      return static_cast<std::uint16_t>( ( static_cast<std::uint32_t>( v.oscSaddr ) << 8U ) | 0x3fU );
    case 0x12:
      return high( 0xf0U | ( v.volMode & 0xfU ) );
    default:
      return 0xffff;
    }
  }

  auto const both = []( std::uint32_t value ) { return static_cast<std::uint16_t>( ( value << 8U ) | value ); };
  switch ( mRegSelect )
  {
  case 0x43:
    return both( ( mTimers[1].scale & 0xf8U ) | ( mIrqPending & 3U ) );
  case 0x4a:
    return 0x0202;
  case 0x4b:
  {
    IrqvScan const scan = irqvScan();
    return both( scan.found && scan.osc ? 0x80U | scan.voice : mLastIrqVoice );
  }
  case 0x4c:
    return both( CHIP_REVISION );
  case 0x4d:
    return both( mSysCtl );
  case 0x4f:
    return both( mOscSelect );
  case 0x46:
    return 0x4646;
  case 0x47:
  case 0x49:
  case 0x54:
  case 0x55:
    return 0x0000;
  case 0x48:
    return 0xc0c0;
  case 0x50:
    return 0x7474;
  case 0x51:
    return 0x4d4d;
  case 0x52:
    return 0x7d7d;
  case 0x53:
    return 0x8888;
  case 0x56:
    return 0x20a0;
  case 0x57:
    return 0x5050;
  default:
    return 0x7878;
  }
}

std::uint8_t Ics2115::read( unsigned port )
{
  switch ( port & 3U )
  {
  case 0:
  {
    // Bit 6, a voice write still queued, never shows: writes apply at once.
    if ( !irq() )
    {
      return 0;
    }
    bool const voicePending = ( mOscIrqPending | mVolIrqPending ) != 0;
    return static_cast<std::uint8_t>( 0x80U | ( mTimerInt != 0 ? 1U : 0U ) | ( voicePending ? 2U : 0U ) );
  }
  case 1:
    return mRegSelect;
  default:
    break;
  }

  bool const low = ( port & 3U ) == 2;
  std::uint16_t const value = registerValue();

  // The side effects follow the read, as the RTL applies them when the
  // strobe goes away. Either byte has them, as on the board: a 16-bit read of
  // IRQV consumes two voices.
  if ( mRegSelect < 0x40 && ( mRegSelect & 0x1fU ) == 0x0f )
  {
    // Reading IRQV consumes what it reported.
    IrqvScan const scan = irqvScan();
    if ( scan.found )
    {
      mLastIrqVoice = scan.voice;
      mOscIrqPending &= ~bit( scan.voice );
      mVolIrqPending &= ~bit( scan.voice );
      Voice& v = mVoices.at( scan.voice );
      if ( scan.osc )
      {
        v.oscConf &= static_cast<std::uint8_t>( ~OSC_IRQ_PENDING );
      }
      if ( scan.vol )
      {
        v.volCtrl &= static_cast<std::uint8_t>( ~VOL_IRQ_PENDING );
      }
    }
  }
  // Reading a timer's preset register acknowledges it, its pending flag and
  // its INT both; reading 0x43 does not.
  if ( mRegSelect == 0x40 || mRegSelect == 0x41 )
  {
    auto const timer = static_cast<std::uint8_t>( 1U << ( mRegSelect - 0x40U ) );
    mIrqPending &= static_cast<std::uint8_t>( ~timer );
    mTimerInt &= static_cast<std::uint8_t>( ~timer );
  }

  return static_cast<std::uint8_t>( low ? value : value >> 8U );
}

void Ics2115::write( unsigned port, std::uint8_t value )
{
  switch ( port & 3U )
  {
  case 1:
    mRegSelect = value;
    break;
  case 2:
    if ( mRegSelect >= 0x40 )
    {
      writeGlobal( value );
    }
    else if ( ( mRegSelect & 0x1fU ) != 0x0e )
    {
      // A voice register's low byte only waits for its high byte.
      mLowLatch = value;
    }
    break;
  case 3:
    if ( mRegSelect >= 0x40 )
    {
      break;
    }
    if ( ( mRegSelect & 0x1fU ) == 0x0e )
    {
      mActiveOsc = value & 0x1fU;
      break;
    }
    // The high byte writes both, so a high byte alone writes a low byte of 0.
    writeVoice( mLowLatch, value );
    mLowLatch = 0;
    break;
  default:
    break;
  }
}

void Ics2115::writeVoice( std::uint8_t low, std::uint8_t high )
{
  Voice& v = mVoices.at( mOscSelect );
  auto const set = []( auto& field, std::uint32_t value, unsigned shift, std::uint32_t mask )
  {
    using Field = std::remove_reference_t<decltype( field )>;
    field = static_cast<Field>( ( field & ~( mask << shift ) ) | ( ( value & mask ) << shift ) );
  };

  // The RTL applies the two bytes as two writes, the low one first, and
  // refreshes the IRQ flags beside the registers after each.
  auto const refreshIrq = [&]
  {
    std::uint32_t const b = bit( mOscSelect );
    if ( ( v.oscConf & ( OSC_IRQ | OSC_IRQ_PENDING ) ) == ( OSC_IRQ | OSC_IRQ_PENDING ) )
    {
      mOscIrqPending |= b;
    }
    // A volume IRQ is never pended by a write.

    // A voice stops counting as ended when it is started again, or when its
    // position is written. The second is the RTL's, by its author's own
    // account not known from the board; it keeps a reused voice from raising
    // an IRQ between being moved and started. Only the register's own number
    // counts, not its mirror at 0x20-0x3F.
    if ( ( v.oscCtl & CTL_DONE ) == 0 || mRegSelect == 0x0a || mRegSelect == 0x0b )
    {
      mOscEnded &= ~b;
    }
    if ( ( v.volCtrl & VOL_DONE ) == 0 || mRegSelect == 0x09 )
    {
      mVolEnded &= ~b;
    }
  };

  switch ( mRegSelect & 0x1fU )
  {
  case 0x01:
    set( v.oscFc, low & 0xfeU, 0, 0xff );
    break;
  case 0x02:
    set( v.oscStart, low, 13, 0xff );
    break;
  case 0x04:
    set( v.oscEnd, low, 13, 0xff );
    break;
  case 0x09:
    set( v.volAcc, low, 10, 0xff );
    set( v.volAcc, 0, 0, 0x3ff );
    break;
  case 0x0a:
    set( v.oscAcc, low, 13, 0xff );
    break;
  case 0x0b:
    set( v.oscAcc, low >> 3U, 0, 0x1f );
    break;
  default:
    break;
  }
  refreshIrq();

  switch ( mRegSelect & 0x1fU )
  {
  case 0x00:
    // The pending bit takes only together with the IRQ enable.
    v.oscConf = static_cast<std::uint8_t>( ( high & 0x7fU ) | ( ( high & ( high << 2U ) ) & 0x80U ) );
    break;
  case 0x01:
    set( v.oscFc, high, 8, 0xff );
    break;
  case 0x02:
    set( v.oscStart, high, 21, 0xff );
    break;
  case 0x03:
    set( v.oscStart, high, 5, 0xff );
    break;
  case 0x04:
    set( v.oscEnd, high, 21, 0xff );
    break;
  case 0x05:
    set( v.oscEnd, high, 5, 0xff );
    break;
  case 0x06:
    v.volIncr = high;
    break;
  case 0x07:
    v.volStart = static_cast<std::uint32_t>( high ) << 18U;
    break;
  case 0x08:
    v.volEnd = static_cast<std::uint32_t>( high ) << 18U;
    break;
  case 0x09:
    set( v.volAcc, high, 18, 0xff );
    break;
  case 0x0a:
    set( v.oscAcc, high, 21, 0xff );
    break;
  case 0x0b:
    set( v.oscAcc, high, 5, 0xff );
    break;
  case 0x0c:
    v.volPan = high;
    break;
  case 0x0d:
    v.volCtrl = high;
    break;
  case 0x10:
    v.oscCtl = high;
    break;
  case 0x11:
    v.oscSaddr = high;
    break;
  case 0x12:
    v.volMode = high;
    break;
  default:
    break;
  }
  refreshIrq();
}

void Ics2115::setTimer( std::size_t timer, std::uint32_t period, bool running )
{
  Timer& t = mTimers.at( timer );
  t.period = period & MASK_24;
  t.count = t.period;
  t.running = running;
}

void Ics2115::writeGlobal( std::uint8_t value )
{
  Timer& timer0 = mTimers[0];
  Timer& timer1 = mTimers[1];
  // Timer 0's period is its preset times 0x42's low five bits, timer 1's its
  // preset alone; both are shifted by the top three bits of their 0x42/0x43.
  auto const period0 = [&]
  { return ( ( timer0.scale & 0x1fU ) + 1U ) * ( timer0.preset + 1U ) << ( 4U + ( timer0.scale >> 5U ) ); };
  auto const period1 = [&] { return ( timer1.preset + 1U ) << ( 4U + ( timer1.scale >> 5U ) ); };

  switch ( mRegSelect )
  {
  case 0x40:
    timer0.preset = value;
    setTimer( 0, period0(), value != 0 && ( timer0.scale & 0x1fU ) != 0 );
    if ( value == 0 )
    {
      mIrqPending &= static_cast<std::uint8_t>( ~1U );
      mTimerInt &= static_cast<std::uint8_t>( ~1U );
    }
    break;
  case 0x41:
    timer1.preset = value;
    setTimer( 1, period1(), value != 0 && ( timer1.scale & 0x10U ) != 0 );
    if ( value == 0 )
    {
      mIrqPending &= static_cast<std::uint8_t>( ~2U );
      mTimerInt &= static_cast<std::uint8_t>( ~2U );
    }
    break;
  case 0x42:
    timer0.scale = value;
    setTimer( 0, period0(), timer0.preset != 0 && ( value & 0x1fU ) != 0 );
    break;
  case 0x43:
    timer1.scale = value;
    setTimer( 1, period1(), timer1.preset != 0 && ( value & 0x10U ) != 0 );
    // Enabling an INT whose timer has already fired delivers it.
    if ( ( value & 0x08U ) != 0 && ( mIrqPending & 1U ) != 0 )
    {
      mTimerInt |= 1U;
    }
    if ( ( value & 0x10U ) != 0 && ( mIrqPending & 2U ) != 0 )
    {
      mTimerInt |= 2U;
    }
    break;
  case 0x4a:
    mIrqEnabled = value;
    break;
  case 0x4d:
    mSysCtl = value & SYS_CTL_RUN;
    break;
  case 0x4f:
    mOscSelect = value & 0x1fU;
    break;
  default:
    break;
  }
}

void Ics2115::takeFrames( std::vector<AudioFrame>& into )
{
  into.insert( into.end(), mFrames.begin(), mFrames.end() );
  mFrames.clear();
}

} // namespace pgm::machine
