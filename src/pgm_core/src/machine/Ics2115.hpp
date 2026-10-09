#pragma once

// The ICS2115 WaveFront synthesizer, ported from rtl/ics2115/*.sv at MiSTer
// core commit 6f757e4: 32 voices reading 8-bit samples (linear, u-law, or the
// board's byte-doubled "16-bit"), interpolated, enveloped, panned and mixed to
// stereo once per sample period; two timers; and the IRQ the Z80 runs on.
//
// Like the RTL, it processes the voices one after another after a sample tick,
// each at the time the RTL's sequencer would load it when its sample reads hit
// the cache; so a Z80 write reaches the voices later in the period, and not
// those before. A write applies at once, where the RTL queues it for a few
// cycles (docs/hardware/differences.md). The RTL's quirks are kept, among them
// the ones its comments record as measured on the board.
//
// Time is counted in pulses of ce_33m, the chip's clock (ClockEnables.hpp).

#include "Sdram.hpp"

#include "pgm/machine/Sound.hpp"

#include <array>
#include <cstdint>
#include <limits>
#include <vector>

namespace pgm::machine
{

/// Where the chip's sample fetches find the cartridge's samples (PGM.sv): from
/// byte `musicBase` of its address space up, when a cartridge is inserted; the
/// BIOS's below, and everywhere when none is.
struct SampleMapping
{
  bool cartridge{};
  std::uint32_t musicBase{};
};

class Ics2115
{
public:
  static constexpr std::size_t VOICES = 32;
  static constexpr std::int64_t NEVER = std::numeric_limits<std::int64_t>::max();

  Ics2115( Sdram const& sdram, SampleMapping samples );

  /// What the reset line does: everything but the time and the frames not yet
  /// taken goes back to its power-up state.
  void reset();

  /// Runs the chip until `pulses` pulses of ce_33m have passed since power-up.
  void advanceTo( std::int64_t pulses );

  /// The pulse at which the chip next acts by itself, a sample tick or a timer
  /// expiring, or NEVER while it is stopped. Until then only the Z80's
  /// accesses can change its IRQ.
  [[nodiscard]] std::int64_t nextEvent() const;

  /// The Z80's port reads and writes, `port` 0 to 3.
  std::uint8_t read( unsigned port );
  void write( unsigned port, std::uint8_t value );

  /// The IRQ line to the Z80.
  [[nodiscard]] bool irq() const;

  /// Moves the frames produced since the last call onto the end of `into`.
  void takeFrames( std::vector<AudioFrame>& into );

  /// The sample period now, in ce_33m pulses: 32 per active oscillator.
  [[nodiscard]] std::int64_t samplePeriod() const;

  [[nodiscard]] std::array<Ics2115Voice, VOICES> const& voices() const;
  /// How many voices are processed: register 0x0E plus one.
  [[nodiscard]] std::size_t activeVoices() const;

  /// Names its state for a save state (StateArchive.hpp).
  template <class Archive>
  void serialize( Archive& archive )
  {
    archive( mVoices );
    archive( mActiveOsc );
    archive( mOscSelect );
    archive( mRegSelect );
    archive( mSysCtl );
    archive( mIrqEnabled );
    archive( mIrqPending );
    archive( mTimerInt );
    archive( mLastIrqVoice );
    archive( mLowLatch );
    archive( mOscIrqPending );
    archive( mOscEnded );
    archive( mVolIrqPending );
    archive( mVolEnded );
    archive( mTimers );
    archive( mPulses );
    archive( mSampleCounter );
    archive( mPass );
    archive( mFrames );
  }

private:
  using Voice = Ics2115Voice;

  struct Timer
  {
    std::uint8_t preset{};
    /// Timer 0's 0x42, or 0x43 for timer 1, which also holds the IRQ enables.
    std::uint8_t scale{};
    std::uint32_t count{};  // 24 bits
    std::uint32_t period{}; // 24 bits
    bool running{};
  };

  /// One ce_33m pulse, with what happens on it.
  void pulse();
  /// What the pulse does while the chip runs: the sample counter and timers.
  void countPulse();
  /// A sample tick: a pass over the voices begins.
  void startPass();
  /// Processes the pass's next voice, and ends the pass with a frame after the
  /// last active one.
  void processNextVoice();
  /// Adds voice `index`'s contribution to the sample, and moves it on.
  void processVoice( std::size_t index, std::int32_t& left, std::int32_t& right );
  [[nodiscard]] std::int16_t sampleAt( Voice const& voice, std::uint32_t address ) const;

  [[nodiscard]] std::uint16_t registerValue() const;
  void writeVoice( std::uint8_t low, std::uint8_t high );
  void writeGlobal( std::uint8_t value );
  void setTimer( std::size_t timer, std::uint32_t period, bool running );

  /// The voice an IRQV read reports: the first with an IRQ pending, in round
  /// robin order after the last one reported.
  struct IrqvScan
  {
    bool found{};
    std::uint8_t voice{};
    bool osc{};
    bool vol{};
  };

  [[nodiscard]] IrqvScan irqvScan() const;

  Sdram const& mSdram;
  SampleMapping mSamples;

  std::array<Voice, VOICES> mVoices{};
  std::uint8_t mActiveOsc{};
  std::uint8_t mOscSelect{};
  std::uint8_t mRegSelect{};
  std::uint8_t mSysCtl{};
  std::uint8_t mIrqEnabled{};
  /// Bits 0 and 1: the timers' pending flags, which a read of their preset
  /// register acknowledges.
  std::uint8_t mIrqPending{};
  /// Bits 0 and 1: the timers' INT latches, which the same read clears.
  std::uint8_t mTimerInt{};
  std::uint8_t mLastIrqVoice{};
  std::uint8_t mLowLatch{};
  // The per-voice flags the RTL keeps beside the voice registers, one bit per
  // voice: IRQs pending, and voices an event has ended, whose IRQ re-asserts.
  std::uint32_t mOscIrqPending{};
  std::uint32_t mOscEnded{};
  std::uint32_t mVolIrqPending{};
  std::uint32_t mVolEnded{};
  std::array<Timer, 2> mTimers{};

  /// The pass over the voices that a sample tick began.
  struct Pass
  {
    bool active{};
    std::int64_t start{};
    std::size_t next{};
    std::int32_t left{};
    std::int32_t right{};
  };

  std::int64_t mPulses{};
  std::uint16_t mSampleCounter{};
  Pass mPass;
  std::vector<AudioFrame> mFrames;
};

} // namespace pgm::machine
