#pragma once

// The IGS023's sprite engine, ported from rtl/igs023_sprite.sv and
// rtl/igs023_buffer.sv at MiSTer core commit e898860 as far as they decide what
// is drawn, and the line buffer's erased pixel as at 6f757e4. Their timing is
// not ported: the RTL draws a frame's sprites from the list it copied at line
// 221 of the frame before, a line buffer at a time, and the result depends on
// that copy and on the ROMs alone. So the emulator draws all 224 lines at once
// from the copy.

#include "Sdram.hpp"

#include "pgm/machine/Machine.hpp"

#include <array>
#include <cstdint>
#include <span>

namespace pgm::machine
{

/// The sprite list as the DMA copies it out of work RAM: five words a sprite.
struct SpriteList
{
  static constexpr std::size_t MAX_SPRITES = 256;
  static constexpr std::size_t WORDS = 5;

  std::array<std::array<std::uint16_t, WORDS>, MAX_SPRITES> entries{};
  std::size_t count{};
};

/// What a sprite list entry's five words say.
[[nodiscard]] SpriteInfo decodeSprite( std::array<std::uint16_t, SpriteList::WORDS> const& words );

/// One line of the sprite layer, as the RTL's line buffer holds it: bit 11 set
/// where a sprite drew, bit 10 its priority, bits 9-5 its palette and bits 4-0
/// its colour.
using SpriteLine = std::array<std::uint16_t, 448>;
using SpriteFrame = std::array<SpriteLine, 224>;

/// What the line buffer holds where no sprite drew: igs023_buffer.sv erases
/// each line after showing it to a low-priority pixel of colour 0x3ff, which
/// is the backdrop.
constexpr std::uint16_t SPRITE_ERASED = 0xfff;

/// Copies the sprite list from work RAM as the RTL's DMA does: up to 256
/// entries, ending before the first whose size word is zero; when 255 entries
/// are read without one, the 256th is read but not counted.
SpriteList copySpriteList( std::span<std::uint8_t const> workRam );

/// Work RAM words the DMA reads for `list`, which decides how long it holds the bus.
std::size_t wordsCopied( SpriteList const& list );

/// Draws every line of the sprite layer for `list`, reading the mask (B) and
/// colour (A) ROMs from `sdram`.
void drawSprites( SpriteList const& list, Sdram const& sdram, SpriteFrame& frame );

} // namespace pgm::machine
