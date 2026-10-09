#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "machine/Ics2115.hpp"

#include <array>
#include <cstdint>
#include <vector>

using pgm::machine::AudioFrame;
using pgm::machine::Ics2115;
using pgm::machine::Sdram;

namespace
{

constexpr std::uint8_t OSC_LOOP = 0x08;
constexpr std::uint8_t OSC_BIDIR = 0x10;
constexpr std::uint8_t OSC_IRQ = 0x20;

/// The sample period with all 32 voices active, in the chip's clocks.
constexpr std::int64_t PERIOD = 1024;
/// Time enough after a sample tick for the voices to be processed and the
/// frame to be out: they take a little over half the period.
constexpr std::int64_t PASS = PERIOD * 3 / 4;

/// The chip, with `samples` as the BIOS's sample ROM, and the Z80's way of
/// writing its registers.
class Chip
{
public:
  explicit Chip( std::vector<std::uint8_t> samples = {} ) : mSamples{ std::move( samples ) }
  {
    mSdram.biosMusic = mSamples;
  }

  Ics2115& operator*()
  {
    return mIcs2115;
  }

  Ics2115* operator->()
  {
    return &mIcs2115;
  }

  /// A voice register of the selected voice, both bytes.
  void voice( std::uint8_t reg, std::uint16_t value )
  {
    mIcs2115.write( 1, reg );
    mIcs2115.write( 2, static_cast<std::uint8_t>( value ) );
    mIcs2115.write( 3, static_cast<std::uint8_t>( value >> 8U ) );
  }

  /// A global register, through the low byte.
  void global( std::uint8_t reg, std::uint8_t value )
  {
    mIcs2115.write( 1, reg );
    mIcs2115.write( 2, value );
  }

  std::uint16_t read( std::uint8_t reg )
  {
    mIcs2115.write( 1, reg );
    std::uint8_t const low = mIcs2115.read( 2 );
    return static_cast<std::uint16_t>( ( mIcs2115.read( 3 ) << 8U ) | low );
  }

  /// One byte of a register: the low one through port 2, the high through 3.
  std::uint8_t readByte( std::uint8_t reg, bool high )
  {
    mIcs2115.write( 1, reg );
    return mIcs2115.read( high ? 3 : 2 );
  }

  /// Runs the chip and returns what it produced.
  std::vector<AudioFrame> runTo( std::int64_t pulses )
  {
    mIcs2115.advanceTo( pulses );
    std::vector<AudioFrame> frames;
    mIcs2115.takeFrames( frames );
    return frames;
  }

  /// Sets voice 0 playing 8-bit linear samples from address 0 at `fc`, at
  /// full volume, panned hard right, with `conf` and loop end `end`.
  void play( std::uint16_t fc, std::uint8_t conf = 0, std::uint32_t end = 0x1fffffff )
  {
    global( 0x4f, 0 );
    voice( 0x00, static_cast<std::uint16_t>( conf << 8U ) );
    voice( 0x01, fc );
    voice( 0x04, static_cast<std::uint16_t>( end >> 13U ) );
    voice( 0x05, static_cast<std::uint16_t>( ( end >> 5U ) << 8U ) );
    voice( 0x09, 0xffff );
    voice( 0x0c, 0xff00 );
    global( 0x4d, 0x05 );
  }

private:
  std::vector<std::uint8_t> mSamples;
  Sdram mSdram;
  Ics2115 mIcs2115{ mSdram, {} };
};

/// What the right channel gives a sample at full volume, panned hard right.
std::int32_t fullRight( std::int32_t sample )
{
  constexpr std::int32_t loudest = 32704; // ((0x100 | 0xff) << 14) + 0xff) >> 8
  return ( sample * loudest ) >> 15;
}

} // namespace

TEST_CASE( "the chip stands still until 0x4D starts it, then gives a frame per 32 clocks per voice",
           "[machine][ics2115]" )
{
  Chip chip;
  REQUIRE( chip->nextEvent() == Ics2115::NEVER );
  REQUIRE( chip.runTo( 5000 ).empty() );

  chip.global( 0x4d, 0x05 );
  REQUIRE( chip->nextEvent() == 5000 + PERIOD );
  auto frames = chip.runTo( 5000 + ( 3 * PERIOD ) + PASS );
  REQUIRE( frames.size() == 3 );
  REQUIRE( frames[0].at == 5000 + PERIOD );
  REQUIRE( frames[2].at == 5000 + ( 3 * PERIOD ) );

  // Fewer active voices, a shorter period.
  chip.voice( 0x0e, 0x0f00 );
  REQUIRE( chip->samplePeriod() == PERIOD / 2 );
}

TEST_CASE( "a voice reads its samples, interpolates between them and scales them by its volume", "[machine][ics2115]" )
{
  Chip chip{ { 0x10, 0x20, 0x30, 0x40, 0xf0 } };

  SECTION( "a sample per frame" )
  {
    chip.play( 0x400 );
    auto const frames = chip.runTo( ( 5 * PERIOD ) + PASS );
    REQUIRE( frames.size() == 5 );
    REQUIRE( frames[0].right == fullRight( 0x1000 ) );
    REQUIRE( frames[1].right == fullRight( 0x2000 ) );
    REQUIRE( frames[3].right == fullRight( 0x4000 ) );
    REQUIRE( frames[4].right == fullRight( -0x1000 ) );
    REQUIRE( frames[0].left == 0 );
  }

  SECTION( "half a sample per frame" )
  {
    chip.play( 0x200 );
    auto const frames = chip.runTo( ( 2 * PERIOD ) + PASS );
    REQUIRE( frames[1].right == fullRight( 0x1800 ) );
  }
}

TEST_CASE( "a write early in a sample period reaches the voices processed after it, and not those before",
           "[machine][ics2115]" )
{
  Chip chip{ { 0x40 } };
  for ( int const v : { 0, 20 } )
  {
    chip.global( 0x4f, static_cast<std::uint8_t>( v ) );
    chip.voice( 0x00, 0x0000 ); // 8-bit samples
    chip.voice( 0x09, 0xffff );
    chip.voice( 0x0c, 0xff00 );
    chip.voice( 0x10, 0x0200 ); // stopped
  }
  chip.global( 0x4d, 0x05 );
  // The tick is at PERIOD; voice 0's turn comes a clock later, voice 20's
  // some 330 clocks later.
  chip.runTo( PERIOD + 16 );

  SECTION( "a later voice sounds in this frame" )
  {
    chip.global( 0x4f, 20 );
    chip.voice( 0x10, 0x0000 );
    auto const frames = chip.runTo( PERIOD + PASS );
    REQUIRE( frames.size() == 1 );
    REQUIRE( frames[0].right == fullRight( 0x4000 ) );
  }

  SECTION( "an earlier voice sounds from the next" )
  {
    chip.global( 0x4f, 0 );
    chip.voice( 0x10, 0x0000 );
    auto const frames = chip.runTo( ( 2 * PERIOD ) + PASS );
    REQUIRE( frames.size() == 2 );
    REQUIRE( frames[0].right == 0 );
    REQUIRE( frames[1].right == fullRight( 0x4000 ) );
  }
}

TEST_CASE( "u-law samples expand to 14 bits and a sign", "[machine][ics2115]" )
{
  Chip chip{ { 0x80, 0x00, 0xff } };
  // Format 11 is u-law too: the u-law bit takes precedence over the 16-bit one.
  auto const conf = static_cast<std::uint8_t>( GENERATE( 0x01, 0x03 ) );
  chip.play( 0x400, conf );
  auto const frames = chip.runTo( ( 3 * PERIOD ) + PASS );
  REQUIRE( frames[0].right == fullRight( 32124 ) );
  REQUIRE( frames[1].right == fullRight( -32124 ) );
  REQUIRE( frames[2].right == 0 );
}

TEST_CASE( "a bidirectional loop turns back at either end", "[machine][ics2115]" )
{
  Chip chip{ { 0x10, 0x20, 0x30, 0x40 } };
  chip.play( 0x400, OSC_LOOP | OSC_BIDIR, 3U << 9U );
  chip.voice( 0x03, 0x1000 ); // the loop starts at sample 1
  chip.voice( 0x0b, 0x1000 ); // and so does the oscillator
  auto const frames = chip.runTo( ( 6 * PERIOD ) + PASS );
  REQUIRE( frames.size() == 6 );
  std::array<std::int32_t, 6> const samples = { 0x2000, 0x3000, 0x4000, 0x3000, 0x2000, 0x3000 };
  for ( std::size_t i = 0; i < samples.size(); ++i )
  {
    REQUIRE( frames.at( i ).right == fullRight( samples.at( i ) ) );
  }
}

TEST_CASE( "a one-shot voice stops at its end and raises an IRQ the IRQV register reports", "[machine][ics2115]" )
{
  Chip chip{ { 0x10, 0x20, 0x30, 0x40 } };
  chip.global( 0x4a, 0xff );
  chip.play( 0x400, OSC_IRQ, 2U << 9U );

  chip.runTo( ( 2 * PERIOD ) + PASS );
  REQUIRE_FALSE( chip->irq() );

  chip.runTo( ( 3 * PERIOD ) + PASS );
  REQUIRE( chip->irq() );
  REQUIRE( ( chip->read( 0 ) & 0x82 ) == 0x82 );
  REQUIRE( ( chip->voices()[0].oscCtl & 1U ) != 0 );

  SECTION( "either byte of IRQV consumes the voice it reports" )
  {
    // The oscillator's flag clear, the volume's set: voice 0. The low byte
    // holds nothing, and reads as 1s.
    bool const high = GENERATE( false, true );
    REQUIRE( chip.readByte( 0x0f, high ) == ( high ? 0x60 : 0xff ) );
    REQUIRE_FALSE( chip->irq() );
    REQUIRE( ( chip->voices()[0].oscConf & 0x80U ) == 0 );
    // Nothing pending: the last voice reported, with all three flags set.
    REQUIRE( chip.readByte( 0x0f, true ) == 0xe0 );
  }

  SECTION( "the IRQ comes back each pass while the ended voice keeps it enabled" )
  {
    chip.readByte( 0x0f, true );
    chip.runTo( ( 4 * PERIOD ) + PASS );
    REQUIRE( chip->irq() );

    // Disabling it stops it coming back, once the last one is consumed.
    chip.voice( 0x00, 0x0000 );
    chip.readByte( 0x0f, true );
    chip.runTo( ( 5 * PERIOD ) + PASS );
    REQUIRE_FALSE( chip->irq() );
  }

  SECTION( "starting the voice again stops the IRQ coming back" )
  {
    chip.voice( 0x05, 0x3000 ); // an end it has not reached yet
    chip.voice( 0x10, 0x0000 );
    chip.readByte( 0x0f, true );
    chip.runTo( ( 4 * PERIOD ) + PASS );
    REQUIRE_FALSE( chip->irq() );
  }

  SECTION( "so does moving the voice, as the RTL has it" )
  {
    chip.voice( 0x0a, 0x0000 );
    chip.readByte( 0x0f, true );
    chip.runTo( ( 4 * PERIOD ) + PASS );
    REQUIRE_FALSE( chip->irq() );
  }
}

TEST_CASE( "timer 0 counts its preset times its scale, and raises its INT when 0x43 enables it", "[machine][ics2115]" )
{
  Chip chip;
  chip.global( 0x4d, 0x05 );
  chip.global( 0x42, 0x01 ); // a multiplier of 2, a shift of 4
  chip.global( 0x40, 0x01 ); // a preset of 2: 64 clocks

  chip.runTo( 65 );
  REQUIRE_FALSE( chip->irq() );
  REQUIRE( ( chip.read( 0x43 ) & 0x03U ) == 0x01 );

  // Enabling it after it fired delivers it.
  chip.global( 0x43, 0x08 );
  REQUIRE( chip->irq() );
  REQUIRE( chip->read( 0 ) == 0x81 );

  // Reading 0x43 acknowledges nothing.
  REQUIRE( ( chip.read( 0x43 ) & 0x03U ) == 0x01 );
  REQUIRE( chip->irq() );

  // Either byte of the preset acknowledges it: the pending flag and the INT.
  chip.readByte( 0x40, GENERATE( false, true ) );
  REQUIRE_FALSE( chip->irq() );
  REQUIRE( ( chip.read( 0x43 ) & 0x03U ) == 0 );

  chip.runTo( 65 + 64 );
  REQUIRE( chip->irq() );
}

TEST_CASE( "voice registers commit on their high byte, and read back their unimplemented bits as 1",
           "[machine][ics2115]" )
{
  Chip chip;
  chip.voice( 0x01, 0x12ff );
  REQUIRE( chip->voices()[0].oscFc == 0x12fe );
  REQUIRE( chip.read( 0x01 ) == 0x12ff );

  // A high byte alone writes a low byte of zero.
  chip->write( 1, 0x01 );
  chip->write( 3, 0x34 );
  REQUIRE( chip->voices()[0].oscFc == 0x3400 );

  chip.voice( 0x0c, 0x5a00 );
  REQUIRE( chip.read( 0x0c ) == 0x5fff );
  REQUIRE( chip.read( 0x4c ) == 0x0101 );
  REQUIRE( chip.read( 0x40 ) == 0x7878 );
}

TEST_CASE( "reset puts the voices and the registers back to power-up", "[machine][ics2115]" )
{
  Chip chip;
  chip.voice( 0x0c, 0x1000 );
  chip.global( 0x4d, 0x05 );

  chip->reset();

  REQUIRE( chip->voices()[0].volPan == 0x7f );
  REQUIRE( chip->nextEvent() == Ics2115::NEVER );
}
