#include "Methods.hpp"

#include <miniz.h>
#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace pgm::control
{

namespace
{

constexpr std::size_t WIDTH = 448;
constexpr std::size_t HEIGHT = 224;

/// RGBA pixels as a PNG of RGB pixels, as the RTL simulator writes one.
std::vector<std::uint8_t> encodePng( std::span<std::uint8_t const> rgba, std::size_t width, std::size_t height )
{
  std::vector<std::uint8_t> rgb;
  rgb.reserve( width * height * 3 );
  for ( std::size_t at = 0; at + 3 < rgba.size(); at += 4 )
  {
    rgb.insert( rgb.end(), { rgba[at], rgba[at + 1], rgba[at + 2] } );
  }
  std::size_t size = 0;
  void* const png = tdefl_write_image_to_png_file_in_memory_ex(
      rgb.data(), static_cast<int>( width ), static_cast<int>( height ), 3, &size, MZ_DEFAULT_LEVEL, 0 );
  if ( png == nullptr )
  {
    return {};
  }
  std::vector<std::uint8_t> bytes( static_cast<std::uint8_t*>( png ), static_cast<std::uint8_t*>( png ) + size );
  mz_free( png );
  return bytes;
}

/// The most `video.screenshot` enlarges a pixel.
constexpr std::uint64_t MAX_SCALE = 8;

/// RGBA rows of `width` pixels with every pixel made `scale` by `scale`.
std::vector<std::uint8_t> enlarged( std::span<std::uint8_t const> rgba, std::size_t width, std::size_t scale )
{
  std::size_t const rowBytes = width * 4;
  std::vector<std::uint8_t> large;
  large.reserve( rgba.size() * scale * scale );
  for ( std::size_t row = 0; row + rowBytes <= rgba.size(); row += rowBytes )
  {
    std::size_t const rowStart = large.size();
    for ( std::size_t at = row; at < row + rowBytes; at += 4 )
    {
      for ( std::size_t i = 0; i < scale; ++i )
      {
        large.insert( large.end(),
                      rgba.begin() + static_cast<std::ptrdiff_t>( at ),
                      rgba.begin() + static_cast<std::ptrdiff_t>( at + 4 ) );
      }
    }
    // The row just made, again for each line below it.
    std::size_t const made = rowBytes * scale;
    for ( std::size_t i = 1; i < scale; ++i )
    {
      std::size_t const end = large.size();
      large.resize( end + made );
      std::copy_n( large.begin() + static_cast<std::ptrdiff_t>( rowStart ),
                   made,
                   large.begin() + static_cast<std::ptrdiff_t>( end ) );
    }
  }
  return large;
}

std::string base64( std::span<std::uint8_t const> bytes )
{
  static constexpr std::string_view ALPHABET = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string text;
  text.reserve( ( ( bytes.size() + 2 ) / 3 ) * 4 );
  for ( std::size_t at = 0; at < bytes.size(); at += 3 )
  {
    std::uint32_t const chunk = ( static_cast<std::uint32_t>( bytes[at] ) << 16U ) |
                                ( at + 1 < bytes.size() ? static_cast<std::uint32_t>( bytes[at + 1] ) << 8U : 0U ) |
                                ( at + 2 < bytes.size() ? bytes[at + 2] : 0U );
    text.push_back( ALPHABET[( chunk >> 18U ) & 63U] );
    text.push_back( ALPHABET[( chunk >> 12U ) & 63U] );
    text.push_back( at + 1 < bytes.size() ? ALPHABET[( chunk >> 6U ) & 63U] : '=' );
    text.push_back( at + 2 < bytes.size() ? ALPHABET[chunk & 63U] : '=' );
  }
  return text;
}

Error notLoaded()
{
  return Error{ .code = "not_loaded", .message = "No game is loaded" };
}

std::optional<machine::TileLayer> layerNamed( std::string const& name )
{
  if ( name == "text" )
  {
    return machine::TileLayer::TEXT;
  }
  if ( name == "background" )
  {
    return machine::TileLayer::BACKGROUND;
  }
  return std::nullopt;
}

/// An unsigned parameter that may be left out.
std::expected<std::uint64_t, Error>
optionalUnsigned( Json const& params, std::string_view name, std::uint64_t fallback )
{
  return params.contains( name ) ? requireUnsigned( params, name ) : fallback;
}

/// `image` answered as the debugger asked for it: written to `path` as a PNG,
/// or in the answer as a PNG, or as raw RGBA with `format` rgba.
Outcome answerImage( machine::Image const& image, Json const& params )
{
  Json result{ { "width", image.width }, { "height", image.height } };
  std::string format = "png";
  if ( params.contains( "format" ) )
  {
    auto const given = requireString( params, "format" );
    if ( !given || ( *given != "png" && *given != "rgba" ) )
    {
      return std::unexpected( given ? badRequest( "format is png or rgba" ) : given.error() );
    }
    format = *given;
  }
  if ( format == "rgba" )
  {
    result["rgba_base64"] = base64( image.rgba );
    return result;
  }
  auto const png =
      encodePng( image.rgba, static_cast<std::size_t>( image.width ), static_cast<std::size_t>( image.height ) );
  if ( png.empty() )
  {
    return std::unexpected( Error{ .code = "screenshot_failed", .message = "The image could not be encoded" } );
  }
  if ( params.contains( "path" ) )
  {
    auto const path = requireString( params, "path" );
    if ( !path )
    {
      return std::unexpected( path.error() );
    }
    std::ofstream file{ *path, std::ios::binary };
    file.write( reinterpret_cast<char const*>( png.data() ), static_cast<std::streamsize>( png.size() ) );
    if ( !file )
    {
      return std::unexpected(
          Error{ .code = "screenshot_failed", .message = fmt::format( "Cannot write {}", *path ) } );
    }
    result["path"] = *path;
    return result;
  }
  result["png_base64"] = base64( png );
  return result;
}

constexpr std::uint64_t MAX_TILES = 4096;

constexpr Param IMAGE_PATH{ .name = "path",
                            .type = "string",
                            .description = "Where to write the image as a PNG; left out, it is returned.",
                            .required = false };
constexpr Param IMAGE_FORMAT{ .name = "format",
                              .type = "string",
                              .description = "png (the default), or rgba for raw pixels in rgba_base64.",
                              .required = false };
constexpr Param LAYER{ .name = "layer", .type = "string", .description = "text or background." };

} // namespace

void addVideoMethods( Dispatcher& dispatcher, Emulator& emulator )
{
  dispatcher.add(
      "video.screenshot",
      info( "The last complete picture, 448 by 224, as a PNG: written to a path, or returned in the answer.",
            { { .name = "path",
                .type = "string",
                .description = "Where to write the PNG; left out, the PNG is returned.",
                .required = false },
              { .name = "scale",
                .type = "integer",
                .description = "Each pixel as scale by scale pixels, 1 to 8; 1 if left out.",
                .required = false } } ),
      [&emulator]( Json const& params ) -> Outcome
      {
        machine::Machine const* const machine = emulator.machine();
        if ( machine == nullptr )
        {
          return std::unexpected( Error{ .code = "not_loaded", .message = "No game is loaded" } );
        }
        auto const scale = optionalUnsigned( params, "scale", 1, 1, MAX_SCALE );
        if ( !scale )
        {
          return std::unexpected( scale.error() );
        }
        auto const factor = static_cast<std::size_t>( *scale );
        std::vector<std::uint8_t> const png =
            factor == 1 ? encodePng( machine->picture(), WIDTH, HEIGHT )
                        : encodePng( enlarged( machine->picture(), WIDTH, factor ), WIDTH * factor, HEIGHT * factor );
        if ( png.empty() )
        {
          return std::unexpected( Error{ .code = "screenshot_failed", .message = "The picture could not be encoded" } );
        }

        Json result{ { "width", WIDTH * factor },
                     { "height", HEIGHT * factor },
                     { "scale", factor },
                     { "frame", machine->picturesDrawn() } };
        if ( params.contains( "path" ) )
        {
          auto const path = requireString( params, "path" );
          if ( !path )
          {
            return std::unexpected( path.error() );
          }
          std::ofstream file{ *path, std::ios::binary };
          file.write( reinterpret_cast<char const*>( png.data() ), static_cast<std::streamsize>( png.size() ) );
          if ( !file )
          {
            return std::unexpected(
                Error{ .code = "screenshot_failed", .message = fmt::format( "Cannot write {}", *path ) } );
          }
          result["path"] = *path;
        }
        else
        {
          result["png_base64"] = base64( png );
        }
        return result;
      } );

  dispatcher.add( "video.registers",
                  info( "IGS023's 16 registers and its zoom table, with the scroll registers named." ),
                  [&emulator]( Json const& /*params*/ ) -> Outcome
                  {
                    machine::Machine const* const machine = emulator.machine();
                    if ( machine == nullptr )
                    {
                      return std::unexpected( notLoaded() );
                    }
                    auto const registers = machine->videoRegisters();
                    return Json{ { "registers", registers },
                                 { "zoom_table", machine->zoomTable() },
                                 { "background_scroll", { { "x", registers[3] }, { "y", registers[2] } } },
                                 { "text_scroll", { { "x", registers[6] }, { "y", registers[5] } } },
                                 { "line_counter", registers[7] },
                                 { "flags", registers[14] } };
                  } );

  dispatcher.add(
      "video.sprites",
      info( "The sprites of the list sprite DMA last copied from work RAM, which the picture shows from the next "
            "vertical blank, in list order: a later one draws over an earlier one." ),
      [&emulator]( Json const& /*params*/ ) -> Outcome
      {
        machine::Machine const* const machine = emulator.machine();
        if ( machine == nullptr )
        {
          return std::unexpected( notLoaded() );
        }
        Json sprites = Json::array();
        for ( machine::SpriteInfo const& sprite : machine->sprites() )
        {
          sprites.push_back( Json{ { "x", sprite.x },
                                   { "y", sprite.y },
                                   { "scale_x", sprite.scaleX },
                                   { "scale_y", sprite.scaleY },
                                   { "flip_x", sprite.flipX },
                                   { "flip_y", sprite.flipY },
                                   { "low_priority", sprite.lowPriority },
                                   { "palette", sprite.palette },
                                   { "mask_address", sprite.maskAddress },
                                   { "width", sprite.width },
                                   { "height", sprite.height } } );
        }
        return Json{ { "count", sprites.size() }, { "sprites", std::move( sprites ) } };
      } );

  dispatcher.add(
      "video.layers",
      info( "Which layers the picture is drawn with, to see what lies under one; sets those given, answers all "
            "three. Drawn pictures and screenshots follow it from the next line.",
            { { .name = "text", .type = "boolean", .description = "The text layer.", .required = false },
              { .name = "background", .type = "boolean", .description = "The background.", .required = false },
              { .name = "sprites", .type = "boolean", .description = "The sprites.", .required = false } } ),
      [&emulator]( Json const& params ) -> Outcome
      {
        machine::Machine* const machine = emulator.machine();
        if ( machine == nullptr )
        {
          return std::unexpected( notLoaded() );
        }
        machine::VideoLayers layers = machine->videoLayers();
        for ( auto [name, flag] : { std::pair{ "text", &layers.text },
                                    std::pair{ "background", &layers.background },
                                    std::pair{ "sprites", &layers.sprites } } )
        {
          if ( params.contains( name ) )
          {
            if ( !params.at( name ).is_boolean() )
            {
              return std::unexpected( badRequest( fmt::format( "{} must be true or false", name ) ) );
            }
            *flag = params.at( name ).get<bool>();
          }
        }
        machine->setVideoLayers( layers );
        return Json{ { "text", layers.text }, { "background", layers.background }, { "sprites", layers.sprites } };
      } );

  dispatcher.add(
      "video.tiles",
      info( "Tiles of the text layer (8 by 8) or the background (32 by 32) as an image, from the tile ROMs, in one "
            "of the layer's 32 palette groups. Transparent pixels are dark grey.",
            { LAYER,
              { .name = "first",
                .type = "integer",
                .description = "The first tile code; 0 if left out.",
                .required = false },
              { .name = "count",
                .type = "integer",
                .description = "How many tiles, at most 4096; 256 if left out.",
                .required = false },
              { .name = "columns",
                .type = "integer",
                .description = "Tiles to a row; 16 if left out.",
                .required = false },
              { .name = "palette",
                .type = "integer",
                .description = "The palette group, 0 to 31; 0 if left out.",
                .required = false },
              IMAGE_PATH,
              IMAGE_FORMAT } ),
      [&emulator]( Json const& params ) -> Outcome
      {
        machine::Machine const* const machine = emulator.machine();
        if ( machine == nullptr )
        {
          return std::unexpected( notLoaded() );
        }
        auto const name = requireString( params, "layer" );
        if ( !name )
        {
          return std::unexpected( name.error() );
        }
        auto const layer = layerNamed( *name );
        auto const first = optionalUnsigned( params, "first", 0 );
        auto const count = optionalUnsigned( params, "count", 256 );
        auto const columns = optionalUnsigned( params, "columns", 16 );
        auto const palette = optionalUnsigned( params, "palette", 0 );
        if ( !layer )
        {
          return std::unexpected( badRequest( "layer is text or background" ) );
        }
        for ( auto const* value : { &first, &count, &columns, &palette } )
        {
          if ( !*value )
          {
            return std::unexpected( value->error() );
          }
        }
        if ( *count == 0 || *count > MAX_TILES || *columns == 0 || *palette > 31 )
        {
          return std::unexpected( badRequest( "count is 1 to 4096, columns at least 1, palette 0 to 31" ) );
        }
        return answerImage( machine->tiles( *layer,
                                            static_cast<std::uint32_t>( *first ),
                                            static_cast<std::uint32_t>( *count ),
                                            static_cast<std::uint32_t>( *columns ),
                                            static_cast<std::uint32_t>( *palette ) ),
                            params );
      } );

  dispatcher.add(
      "video.tilemap",
      info( "A layer's whole tile map as an image, unscrolled: the text layer's 64 by 32 tiles (512 by 256), the "
            "background's 64 by 16 (2048 by 512), which is all its 4 KB of VRAM holds. Transparent pixels are dark "
            "grey.",
            { LAYER, IMAGE_PATH, IMAGE_FORMAT } ),
      [&emulator]( Json const& params ) -> Outcome
      {
        machine::Machine const* const machine = emulator.machine();
        if ( machine == nullptr )
        {
          return std::unexpected( notLoaded() );
        }
        auto const name = requireString( params, "layer" );
        if ( !name )
        {
          return std::unexpected( name.error() );
        }
        auto const layer = layerNamed( *name );
        if ( !layer )
        {
          return std::unexpected( badRequest( "layer is text or background" ) );
        }
        return answerImage( machine->tilemap( *layer ), params );
      } );
}

} // namespace pgm::control
