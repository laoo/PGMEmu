#pragma once

// An ARM7TDMI: the ARMv4T instruction sets, ARM and Thumb, with the core's
// three-stage pipeline and its cycle counts, and no coprocessor. It is the
// IGS027A's CPU (docs/decisions/0013-arm7tdmi-core.md). Written from ARM's
// ARM7TDMI Technical Reference Manual; what the manual leaves open follows
// the SingleStepTests ARM7TDMI suite (tests/cpu/Arm7SingleStepTest.cpp).
//
// Each memory access is one cycle, and so is each internal cycle: the RTL's
// core takes one of either per clock, its caches' stalls caught up after.

#include <array>
#include <cstdint>

namespace pgm::cpu
{

/// What the core reaches memory through.
class Arm7Bus
{
public:
  /// The kinds of an access, as the core signals them: SEQUENTIAL when the
  /// address follows the last one's (SEQ), CODE for an opcode fetch (OPC), LOCK
  /// for a swap's write. Their values are those the SingleStepTests suite uses.
  static constexpr unsigned SEQUENTIAL = 1;
  static constexpr unsigned CODE = 2;
  static constexpr unsigned LOCK = 8;

  /// Reads `size` bytes, 1, 2 or 4, at `address` as the core puts it on the
  /// bus, unaligned perhaps: the bus answers the aligned unit that holds it.
  virtual std::uint32_t read( std::uint32_t address, unsigned size, unsigned access ) = 0;
  /// Writes the low `size` bytes of `value`.
  virtual void write( std::uint32_t address, unsigned size, std::uint32_t value, unsigned access ) = 0;

protected:
  Arm7Bus() = default;
  ~Arm7Bus() = default;
  Arm7Bus( Arm7Bus const& ) = default;
  Arm7Bus& operator=( Arm7Bus const& ) = default;
  Arm7Bus( Arm7Bus&& ) = default;
  Arm7Bus& operator=( Arm7Bus&& ) = default;
};

/// Memory the core fetches opcodes from without the bus: a ROM, whose reads
/// do nothing that the bus would have to see. `bytes` holds `size` bytes, a
/// multiple of four, and outlives the core.
struct CodeWindow
{
  std::uint32_t start{};
  std::uint32_t size{};
  std::uint8_t const* bytes{};
};

/// Everything the core holds, as a save state takes it.
struct Arm7State
{
  /// R0 to R15 as user and system modes see them.
  std::array<std::uint32_t, 16> r{};
  /// R8 to R14 of FIQ mode.
  std::array<std::uint32_t, 7> fiq{};
  /// R13 and R14 of supervisor, abort, IRQ and undefined modes.
  std::array<std::uint32_t, 2> svc{};
  std::array<std::uint32_t, 2> abt{};
  std::array<std::uint32_t, 2> irq{};
  std::array<std::uint32_t, 2> und{};
  std::uint32_t cpsr{};
  /// The saved PSRs of FIQ, supervisor, abort, IRQ and undefined modes.
  std::array<std::uint32_t, 5> spsr{};
  /// The opcodes fetched and not yet executed: the one to execute next first.
  /// R15 is the address the next fetch reads.
  std::array<std::uint32_t, 2> pipeline{};
  /// Whether the next fetch follows the last access.
  bool fetchSequential{};
  /// The FIQ and IRQ lines, high when asserted.
  bool fiqLine{};
  bool irqLine{};
  /// Cycles run since the core was made.
  std::int64_t cycles{};
};

class Arm7
{
public:
  // The CPSR's fields.
  static constexpr std::uint32_t FLAG_N = 1U << 31U;
  static constexpr std::uint32_t FLAG_Z = 1U << 30U;
  static constexpr std::uint32_t FLAG_C = 1U << 29U;
  static constexpr std::uint32_t FLAG_V = 1U << 28U;
  static constexpr std::uint32_t DISABLE_IRQ = 1U << 7U;
  static constexpr std::uint32_t DISABLE_FIQ = 1U << 6U;
  static constexpr std::uint32_t THUMB = 1U << 5U;
  static constexpr std::uint32_t MODE_MASK = 0x1f;

  static constexpr std::uint32_t MODE_USER = 0x10;
  static constexpr std::uint32_t MODE_FIQ = 0x11;
  static constexpr std::uint32_t MODE_IRQ = 0x12;
  static constexpr std::uint32_t MODE_SUPERVISOR = 0x13;
  static constexpr std::uint32_t MODE_ABORT = 0x17;
  static constexpr std::uint32_t MODE_UNDEFINED = 0x1b;
  static constexpr std::uint32_t MODE_SYSTEM = 0x1f;

  explicit Arm7( Arm7Bus& bus );

  /// Lets opcode fetches within `window` skip the bus. Up to two windows; a
  /// fetch outside them all goes to the bus as before. Data accesses never
  /// use them.
  void addCodeWindow( CodeWindow window );

  /// The reset: supervisor mode, ARM state, both interrupts disabled, and
  /// the pipeline filled from address 0.
  void reset();

  /// Runs one instruction, or, when an enabled interrupt line is asserted,
  /// enters its handler instead.
  void step();

  void setFiq( bool asserted );
  void setIrq( bool asserted );

  [[nodiscard]] Arm7State const& state() const;
  /// Takes `state` as the core's, as a save state or a test sets it.
  void setState( Arm7State const& state );

  /// Register `index` as the current mode sees it.
  [[nodiscard]] std::uint32_t reg( unsigned index ) const;
  /// The address of the instruction executed next.
  [[nodiscard]] std::uint32_t pc() const;

  [[nodiscard]] std::int64_t cycles() const
  {
    return mState.cycles;
  }

private:
  // Registers and modes.
  /// The slot of `mState` that holds register `index` of `mode`.
  [[nodiscard]] std::uint32_t& bankSlot( std::uint32_t mode, unsigned index ) const;
  /// Whether register `index` of the current mode is the user bank's.
  [[nodiscard]] bool userRegister( unsigned index ) const;
  /// Loads mReg from mState's banks of the current mode, and stores it back
  /// into `mode`'s.
  void loadBank();
  void storeBank( std::uint32_t mode ) const;
  void setCpsr( std::uint32_t value );
  /// The SPSR of the current mode, or null in user and system modes.
  [[nodiscard]] std::uint32_t* spsr();
  [[nodiscard]] bool thumb() const;
  [[nodiscard]] bool conditionHolds( std::uint32_t condition ) const;
  void setNz( std::uint32_t result );

  // The pipeline and the bus.
  /// An opcode fetch: from a code window that holds `address`, or the bus.
  [[nodiscard]] std::uint32_t fetch( std::uint32_t address, unsigned size, unsigned access ) const;
  /// The fetch made in an instruction's first cycle: the next opcode into the
  /// pipeline, R15 on to the one after.
  void prefetch();
  /// R15 is written: the pipeline is filled again from its new value.
  void refill();
  void internalCycle();
  std::uint32_t readData( std::uint32_t address, unsigned size, unsigned access );
  void writeData( std::uint32_t address, unsigned size, std::uint32_t value, unsigned access );
  /// Writes `value` to register `index`, refilling the pipeline when it is R15.
  void writeRegister( unsigned index, std::uint32_t value );
  /// A load's or store's base written back.
  void writeBase( unsigned index, std::uint32_t value );

  void enterException( std::uint32_t mode, std::uint32_t vector, std::uint32_t returnAddress );

  // ARM state.
  void executeArm( std::uint32_t opcode );
  void dataProcessing( std::uint32_t opcode );
  void psrTransfer( std::uint32_t opcode );
  void multiply( std::uint32_t opcode );
  void multiplyLong( std::uint32_t opcode );
  void singleDataSwap( std::uint32_t opcode );
  void branchExchange( std::uint32_t opcode );
  void halfwordTransfer( std::uint32_t opcode );
  void singleDataTransfer( std::uint32_t opcode );
  void blockDataTransfer( std::uint32_t opcode );
  void branch( std::uint32_t opcode );
  void undefined();
  void softwareInterrupt();

  // Thumb state.
  void executeThumb( std::uint32_t opcode );
  void thumbShift( std::uint32_t opcode );
  void thumbAddSubtract( std::uint32_t opcode );
  void thumbImmediate( std::uint32_t opcode );
  void thumbAlu( std::uint32_t opcode );
  void thumbHighRegister( std::uint32_t opcode );
  void thumbPcRelativeLoad( std::uint32_t opcode );
  void thumbRegisterOffset( std::uint32_t opcode );
  void thumbSignedTransfer( std::uint32_t opcode );
  void thumbImmediateOffset( std::uint32_t opcode );
  void thumbHalfwordTransfer( std::uint32_t opcode );
  void thumbSpRelative( std::uint32_t opcode );
  void thumbLoadAddress( std::uint32_t opcode );
  void thumbAdjustSp( std::uint32_t opcode );
  void thumbPushPop( std::uint32_t opcode );
  void thumbMultiple( std::uint32_t opcode );
  void thumbConditionalBranch( std::uint32_t opcode );
  void thumbBranch( std::uint32_t opcode );
  void thumbLongBranch( std::uint32_t opcode );

  // Shared by both.
  struct Shifted
  {
    std::uint32_t value;
    bool carry;
  };

  [[nodiscard]] Shifted shiftByImmediate( unsigned type, std::uint32_t value, unsigned amount ) const;
  [[nodiscard]] Shifted shiftByRegister( unsigned type, std::uint32_t value, unsigned amount ) const;
  /// An ALU operation of the data-processing set; answers the result, and
  /// sets the flags when `setFlags`.
  std::uint32_t alu( unsigned operation, std::uint32_t first, Shifted second, bool setFlags );
  /// The internal cycles a multiply by `multiplier` takes.
  [[nodiscard]] static int multiplyCycles( std::uint32_t multiplier, bool isSigned );

  /// Loads and stores a block of registers, `list` from the lowest, from
  /// `base`; both instruction sets' block transfers come here.
  struct Block
  {
    unsigned base;
    std::uint16_t list;
    bool load;
    bool preIndex;
    bool up;
    bool writeBack;
    /// S: the user bank's registers, the base among them.
    bool userBank;
    /// Thumb's POP of R15, which drops bit 0 of what it loads.
    bool alignPc;
  };

  void transferBlock( Block const& block );
  /// LDRH's and LDRSB/LDRSH's value from what the bus answers at `address`.
  [[nodiscard]] static std::uint32_t loadedHalfword( std::uint32_t data, std::uint32_t address, bool isSigned );
  [[nodiscard]] static std::uint32_t loadedByte( std::uint32_t data, bool isSigned );
  [[nodiscard]] static std::uint32_t loadedWord( std::uint32_t data, std::uint32_t address );

  Arm7Bus* mBus;
  /// The canonical state. The banks of the current mode are stale while mReg
  /// holds them; storeBank() brings them up to date, and state() does.
  mutable Arm7State mState;
  /// R0 to R15 as the current mode sees them: the registers the core works
  /// on, copied in and out of mState's banks when the mode changes.
  std::array<std::uint32_t, 16> mReg{};
  /// Where fetches skip the bus; a window of no size is unused.
  std::array<CodeWindow, 2> mWindows{};
};

} // namespace pgm::cpu
