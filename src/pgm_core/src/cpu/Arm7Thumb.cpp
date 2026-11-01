// The ARM7TDMI's Thumb instruction set; see Arm7.hpp.

#include "Arm7.hpp"

namespace pgm::cpu
{

namespace
{

constexpr unsigned SP = 13;
constexpr unsigned LR = 14;
constexpr unsigned PC = 15;

constexpr bool bit( std::uint32_t value, unsigned index )
{
  return ( ( value >> index ) & 1U ) != 0;
}

constexpr std::uint32_t field( std::uint32_t value, unsigned low, unsigned width )
{
  return ( value >> low ) & ( ( 1U << width ) - 1U );
}

constexpr std::uint32_t signExtend( std::uint32_t value, unsigned bits )
{
  std::uint32_t const sign = 1U << ( bits - 1 );
  return ( value ^ sign ) - sign;
}

// The data-processing operations Thumb's maps onto.
constexpr unsigned OP_AND = 0x0;
constexpr unsigned OP_EOR = 0x1;
constexpr unsigned OP_SUB = 0x2;
constexpr unsigned OP_RSB = 0x3;
constexpr unsigned OP_ADD = 0x4;
constexpr unsigned OP_ADC = 0x5;
constexpr unsigned OP_SBC = 0x6;
constexpr unsigned OP_TST = 0x8;
constexpr unsigned OP_CMP = 0xa;
constexpr unsigned OP_CMN = 0xb;
constexpr unsigned OP_ORR = 0xc;
constexpr unsigned OP_MOV = 0xd;
constexpr unsigned OP_BIC = 0xe;
constexpr unsigned OP_MVN = 0xf;

} // namespace

void Arm7::executeThumb( std::uint32_t opcode )
{
  switch ( opcode >> 12U )
  {
  case 0x0:
  case 0x1:
    if ( field( opcode, 11, 2 ) == 3 )
    {
      thumbAddSubtract( opcode );
    }
    else
    {
      thumbShift( opcode );
    }
    return;
  case 0x2:
  case 0x3:
    thumbImmediate( opcode );
    return;
  case 0x4:
    if ( bit( opcode, 11 ) )
    {
      thumbPcRelativeLoad( opcode );
    }
    else if ( bit( opcode, 10 ) )
    {
      thumbHighRegister( opcode );
    }
    else
    {
      thumbAlu( opcode );
    }
    return;
  case 0x5:
    if ( bit( opcode, 9 ) )
    {
      thumbSignedTransfer( opcode );
    }
    else
    {
      thumbRegisterOffset( opcode );
    }
    return;
  case 0x6:
  case 0x7:
    thumbImmediateOffset( opcode );
    return;
  case 0x8:
    thumbHalfwordTransfer( opcode );
    return;
  case 0x9:
    thumbSpRelative( opcode );
    return;
  case 0xa:
    thumbLoadAddress( opcode );
    return;
  case 0xb:
    if ( field( opcode, 8, 4 ) == 0x0 )
    {
      thumbAdjustSp( opcode );
    }
    else if ( field( opcode, 9, 2 ) == 2 )
    {
      thumbPushPop( opcode );
    }
    else
    {
      undefined();
    }
    return;
  case 0xc:
    thumbMultiple( opcode );
    return;
  case 0xd:
    if ( field( opcode, 8, 4 ) == 0xf )
    {
      softwareInterrupt();
    }
    else
    {
      thumbConditionalBranch( opcode );
    }
    return;
  case 0xe:
    if ( bit( opcode, 11 ) )
    {
      undefined(); // ARMv5's BLX
    }
    else
    {
      thumbBranch( opcode );
    }
    return;
  default:
    thumbLongBranch( opcode );
    return;
  }
}

void Arm7::thumbShift( std::uint32_t opcode )
{
  unsigned const rd = field( opcode, 0, 3 );
  Shifted const shifted =
      shiftByImmediate( field( opcode, 11, 2 ), mReg[field( opcode, 3, 3 )], field( opcode, 6, 5 ) );
  prefetch();
  mReg[rd] = alu( OP_MOV, 0, shifted, true );
}

void Arm7::thumbAddSubtract( std::uint32_t opcode )
{
  std::uint32_t const operand = bit( opcode, 10 ) ? field( opcode, 6, 3 ) : mReg[field( opcode, 6, 3 )];
  std::uint32_t const first = mReg[field( opcode, 3, 3 )];
  prefetch();
  mReg[field( opcode, 0, 3 )] =
      alu( bit( opcode, 9 ) ? OP_SUB : OP_ADD, first, Shifted{ .value = operand, .carry = false }, true );
}

void Arm7::thumbImmediate( std::uint32_t opcode )
{
  unsigned const rd = field( opcode, 8, 3 );
  Shifted const operand{ .value = field( opcode, 0, 8 ), .carry = ( mState.cpsr & FLAG_C ) != 0 };
  prefetch();
  switch ( field( opcode, 11, 2 ) )
  {
  case 0:
    mReg[rd] = alu( OP_MOV, 0, operand, true );
    break;
  case 1:
    alu( OP_CMP, mReg[rd], operand, true );
    break;
  case 2:
    mReg[rd] = alu( OP_ADD, mReg[rd], operand, true );
    break;
  default:
    mReg[rd] = alu( OP_SUB, mReg[rd], operand, true );
    break;
  }
}

void Arm7::thumbAlu( std::uint32_t opcode )
{
  unsigned const rd = field( opcode, 0, 3 );
  std::uint32_t const source = mReg[field( opcode, 3, 3 )];
  std::uint32_t& target = mReg[rd];
  bool const carry = ( mState.cpsr & FLAG_C ) != 0;
  prefetch();
  auto const shift = [&]( unsigned type )
  {
    internalCycle();
    target = alu( OP_MOV, 0, shiftByRegister( type, target, source & 0xffU ), true );
  };
  switch ( field( opcode, 6, 4 ) )
  {
  case 0x0:
    target = alu( OP_AND, target, Shifted{ .value = source, .carry = carry }, true );
    break;
  case 0x1:
    target = alu( OP_EOR, target, Shifted{ .value = source, .carry = carry }, true );
    break;
  case 0x2:
    shift( 0 );
    break;
  case 0x3:
    shift( 1 );
    break;
  case 0x4:
    shift( 2 );
    break;
  case 0x5:
    target = alu( OP_ADC, target, Shifted{ .value = source, .carry = carry }, true );
    break;
  case 0x6:
    target = alu( OP_SBC, target, Shifted{ .value = source, .carry = carry }, true );
    break;
  case 0x7:
    shift( 3 );
    break;
  case 0x8:
    alu( OP_TST, target, Shifted{ .value = source, .carry = carry }, true );
    break;
  case 0x9: // NEG
    target = alu( OP_RSB, source, Shifted{ .value = 0, .carry = carry }, true );
    break;
  case 0xa:
    alu( OP_CMP, target, Shifted{ .value = source, .carry = carry }, true );
    break;
  case 0xb:
    alu( OP_CMN, target, Shifted{ .value = source, .carry = carry }, true );
    break;
  case 0xc:
    target = alu( OP_ORR, target, Shifted{ .value = source, .carry = carry }, true );
    break;
  case 0xd: // MUL
  {
    int const cycles = multiplyCycles( target, true );
    for ( int i = 0; i < cycles; ++i )
    {
      internalCycle();
    }
    target *= source;
    setNz( target );
    break;
  }
  case 0xe:
    target = alu( OP_BIC, target, Shifted{ .value = source, .carry = carry }, true );
    break;
  default:
    target = alu( OP_MVN, 0, Shifted{ .value = source, .carry = carry }, true );
    break;
  }
}

void Arm7::thumbHighRegister( std::uint32_t opcode )
{
  unsigned const rd = field( opcode, 0, 3 ) | ( bit( opcode, 7 ) ? 8U : 0U );
  unsigned const rs = field( opcode, 3, 3 ) | ( bit( opcode, 6 ) ? 8U : 0U );
  std::uint32_t const source = mReg[rs];
  std::uint32_t const destination = mReg[rd];
  prefetch();
  switch ( field( opcode, 8, 2 ) )
  {
  case 0:
    writeRegister( rd, rd == PC ? ( destination + source ) & ~1U : destination + source );
    break;
  case 1:
    alu( OP_CMP, destination, Shifted{ .value = source, .carry = false }, true );
    break;
  case 2:
    writeRegister( rd, rd == PC ? source & ~1U : source );
    break;
  default: // BX
    setCpsr( ( mState.cpsr & ~THUMB ) | ( bit( source, 0 ) ? THUMB : 0 ) );
    writeRegister( PC, source & ~1U );
    break;
  }
}

void Arm7::thumbPcRelativeLoad( std::uint32_t opcode )
{
  std::uint32_t const address = ( mReg[PC] & ~2U ) + ( field( opcode, 0, 8 ) << 2U );
  prefetch();
  std::uint32_t const data = readData( address, 4, 0 );
  internalCycle();
  mReg[field( opcode, 8, 3 )] = loadedWord( data, address );
}

void Arm7::thumbRegisterOffset( std::uint32_t opcode )
{
  unsigned const rd = field( opcode, 0, 3 );
  std::uint32_t const address = mReg[field( opcode, 3, 3 )] + mReg[field( opcode, 6, 3 )];
  unsigned const size = bit( opcode, 10 ) ? 1 : 4;
  prefetch();
  if ( !bit( opcode, 11 ) )
  {
    writeData( address, size, mReg[rd], 0 );
    return;
  }
  std::uint32_t const data = readData( address, size, 0 );
  internalCycle();
  mReg[rd] = size == 1 ? loadedByte( data, false ) : loadedWord( data, address );
}

void Arm7::thumbSignedTransfer( std::uint32_t opcode )
{
  unsigned const rd = field( opcode, 0, 3 );
  std::uint32_t const address = mReg[field( opcode, 3, 3 )] + mReg[field( opcode, 6, 3 )];
  prefetch();
  std::uint32_t value = 0;
  switch ( field( opcode, 10, 2 ) )
  {
  case 0: // STRH
    writeData( address, 2, mReg[rd], 0 );
    return;
  case 1: // LDSB
    value = loadedByte( readData( address, 1, 0 ), true );
    break;
  case 2: // LDRH
    value = loadedHalfword( readData( address, 2, 0 ), address, false );
    break;
  default: // LDSH
    value = loadedHalfword( readData( address, 2, 0 ), address, true );
    break;
  }
  internalCycle();
  mReg[rd] = value;
}

void Arm7::thumbImmediateOffset( std::uint32_t opcode )
{
  unsigned const rd = field( opcode, 0, 3 );
  unsigned const size = bit( opcode, 12 ) ? 1 : 4;
  std::uint32_t const address = mReg[field( opcode, 3, 3 )] + ( field( opcode, 6, 5 ) * size );
  prefetch();
  if ( !bit( opcode, 11 ) )
  {
    writeData( address, size, mReg[rd], 0 );
    return;
  }
  std::uint32_t const data = readData( address, size, 0 );
  internalCycle();
  mReg[rd] = size == 1 ? loadedByte( data, false ) : loadedWord( data, address );
}

void Arm7::thumbHalfwordTransfer( std::uint32_t opcode )
{
  unsigned const rd = field( opcode, 0, 3 );
  std::uint32_t const address = mReg[field( opcode, 3, 3 )] + ( field( opcode, 6, 5 ) << 1U );
  prefetch();
  if ( !bit( opcode, 11 ) )
  {
    writeData( address, 2, mReg[rd], 0 );
    return;
  }
  std::uint32_t const data = readData( address, 2, 0 );
  internalCycle();
  mReg[rd] = loadedHalfword( data, address, false );
}

void Arm7::thumbSpRelative( std::uint32_t opcode )
{
  unsigned const rd = field( opcode, 8, 3 );
  std::uint32_t const address = mReg[SP] + ( field( opcode, 0, 8 ) << 2U );
  prefetch();
  if ( !bit( opcode, 11 ) )
  {
    writeData( address, 4, mReg[rd], 0 );
    return;
  }
  std::uint32_t const data = readData( address, 4, 0 );
  internalCycle();
  mReg[rd] = loadedWord( data, address );
}

void Arm7::thumbLoadAddress( std::uint32_t opcode )
{
  std::uint32_t const base = bit( opcode, 11 ) ? mReg[SP] : mReg[PC] & ~2U;
  prefetch();
  mReg[field( opcode, 8, 3 )] = base + ( field( opcode, 0, 8 ) << 2U );
}

void Arm7::thumbAdjustSp( std::uint32_t opcode )
{
  std::uint32_t const offset = field( opcode, 0, 7 ) << 2U;
  prefetch();
  std::uint32_t& sp = mReg[SP];
  sp = bit( opcode, 7 ) ? sp - offset : sp + offset;
}

void Arm7::thumbPushPop( std::uint32_t opcode )
{
  bool const load = bit( opcode, 11 );
  auto list = static_cast<std::uint16_t>( field( opcode, 0, 8 ) );
  if ( bit( opcode, 8 ) )
  {
    list |= static_cast<std::uint16_t>( 1U << ( load ? PC : LR ) );
  }
  transferBlock( Block{ .base = SP,
                        .list = list,
                        .load = load,
                        .preIndex = !load,
                        .up = load,
                        .writeBack = true,
                        .userBank = false,
                        .alignPc = load && bit( opcode, 8 ) } );
}

void Arm7::thumbMultiple( std::uint32_t opcode )
{
  transferBlock( Block{ .base = field( opcode, 8, 3 ),
                        .list = static_cast<std::uint16_t>( field( opcode, 0, 8 ) ),
                        .load = bit( opcode, 11 ),
                        .preIndex = false,
                        .up = true,
                        .writeBack = true,
                        .userBank = false,
                        .alignPc = false } );
}

void Arm7::thumbConditionalBranch( std::uint32_t opcode )
{
  std::uint32_t const target = mReg[PC] + ( signExtend( field( opcode, 0, 8 ), 8 ) << 1U );
  bool const taken = conditionHolds( field( opcode, 8, 4 ) );
  prefetch();
  if ( taken )
  {
    writeRegister( PC, target );
  }
}

void Arm7::thumbBranch( std::uint32_t opcode )
{
  std::uint32_t const target = mReg[PC] + ( signExtend( field( opcode, 0, 11 ), 11 ) << 1U );
  prefetch();
  writeRegister( PC, target );
}

void Arm7::thumbLongBranch( std::uint32_t opcode )
{
  std::uint32_t const offset = field( opcode, 0, 11 );
  if ( !bit( opcode, 11 ) )
  {
    // The first half: the high part of the offset into LR.
    std::uint32_t const pc = mReg[PC];
    prefetch();
    mReg[LR] = pc + ( signExtend( offset, 11 ) << 12U );
    return;
  }
  std::uint32_t const next = mReg[PC] - 2;
  std::uint32_t const target = ( mReg[LR] + ( offset << 1U ) ) & ~1U;
  prefetch();
  mReg[LR] = next | 1U;
  writeRegister( PC, target );
}

} // namespace pgm::cpu
