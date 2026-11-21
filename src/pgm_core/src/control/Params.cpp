#include "Methods.hpp"

#include <spdlog/fmt/fmt.h>

#include <charconv>
#include <optional>
#include <system_error>
#include <utility>

namespace pgm::control
{

namespace
{

/// The value of hex digits after `0x` or `$`, as a person writes an address.
std::optional<std::uint64_t> parseHex( std::string_view text )
{
  if ( text.starts_with( "0x" ) || text.starts_with( "0X" ) )
  {
    text.remove_prefix( 2 );
  }
  else if ( text.starts_with( '$' ) )
  {
    text.remove_prefix( 1 );
  }
  else
  {
    return std::nullopt;
  }
  std::uint64_t value = 0;
  auto const [end, error] = std::from_chars( text.data(), text.data() + text.size(), value, 16 );
  if ( text.empty() || error != std::errc{} || end != text.data() + text.size() )
  {
    return std::nullopt;
  }
  return value;
}

} // namespace

Error badRequest( std::string message )
{
  return Error{ .code = "bad_request", .message = std::move( message ) };
}

std::expected<std::string, Error> requireString( Json const& params, std::string_view name )
{
  auto const field = params.find( name );
  if ( field == params.end() || !field->is_string() )
  {
    return std::unexpected( badRequest( fmt::format( "Missing or invalid field: {}", name ) ) );
  }
  return field->get<std::string>();
}

std::expected<std::uint64_t, Error> requireUnsigned( Json const& params, std::string_view name )
{
  // Parsed text holds a non-negative integer as unsigned, a request built in
  // C++ from an `int` holds it as signed; the value is what is checked.
  auto const field = params.find( name );
  if ( field != params.end() && field->is_string() )
  {
    if ( auto const value = parseHex( field->get_ref<std::string const&>() ) )
    {
      return *value;
    }
  }
  else if ( field != params.end() && field->is_number_integer() &&
            ( field->is_number_unsigned() || field->get<std::int64_t>() >= 0 ) )
  {
    return field->get<std::uint64_t>();
  }
  return std::unexpected( badRequest( fmt::format( "Missing or invalid field: {}", name ) ) );
}

std::expected<std::uint64_t, Error> optionalUnsigned(
    Json const& params, std::string_view name, std::uint64_t fallback, std::uint64_t low, std::uint64_t high )
{
  if ( !params.contains( name ) )
  {
    return fallback;
  }
  auto const given = requireUnsigned( params, name );
  if ( given && ( *given < low || *given > high ) )
  {
    return std::unexpected( badRequest( fmt::format( "{} is {} to {}, not {}", name, low, high, *given ) ) );
  }
  return given;
}

Json paramsSchema( std::initializer_list<Param> params )
{
  Json properties = Json::object();
  Json required = Json::array();
  for ( Param const& param : params )
  {
    properties[std::string{ param.name }] = Json{ { "type", param.type }, { "description", param.description } };
    if ( param.required )
    {
      required.push_back( param.name );
    }
  }
  Json schema{ { "type", "object" }, { "properties", std::move( properties ) } };
  if ( !required.empty() )
  {
    schema["required"] = std::move( required );
  }
  return schema;
}

MethodInfo info( std::string_view description, std::initializer_list<Param> params )
{
  return MethodInfo{ .description = std::string{ description }, .params = paramsSchema( params ) };
}

} // namespace pgm::control
