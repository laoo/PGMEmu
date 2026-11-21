#include "pgm/control/Dispatcher.hpp"

#include "Methods.hpp"

#include <algorithm>
#include <cstdint>
#include <utility>

namespace pgm::control
{

namespace
{

Json errorResponse( std::uint64_t id, Error const& error )
{
  return Json{ { "id", id }, { "ok", false }, { "error", { { "code", error.code }, { "message", error.message } } } };
}

Json resultResponse( std::uint64_t id, Json result )
{
  return Json{ { "id", id }, { "ok", true }, { "result", std::move( result ) } };
}

} // namespace

Dispatcher::Dispatcher( Emulator& emulator )
{
  addEmuMethods( *this, emulator );
  addMemoryMethods( *this, emulator );
  addRunMethods( *this, emulator );
  addCpuMethods( *this, emulator );
  addTraceMethods( *this, emulator );
  addVideoMethods( *this, emulator );
  addAudioMethods( *this, emulator );
  addInputMethods( *this, emulator );
  addStateMethods( *this, emulator );
  addTestMethods( *this, emulator );
}

Json Dispatcher::handle( Json const& request ) const
{
  // The id is read first, so that every later failure can be answered under
  // it. Until it is known the protocol answers under 0, as the RTL simulator
  // does.
  if ( !request.is_object() )
  {
    return errorResponse( 0, badRequest( "Request must be a JSON object" ) );
  }

  // An id is a number: the hex strings parameters may be do not apply to it.
  auto const idField = request.find( "id" );
  auto const id = idField != request.end() && idField->is_number_integer()
                      ? requireUnsigned( request, "id" )
                      : std::unexpected( badRequest( "Missing or invalid field: id" ) );
  if ( !id )
  {
    return errorResponse( 0, id.error() );
  }

  auto const methodField = request.find( "method" );
  if ( methodField == request.end() || !methodField->is_string() )
  {
    return errorResponse( *id, badRequest( "Missing or invalid field: method" ) );
  }
  auto const& method = methodField->get_ref<std::string const&>();

  // `params` may be left out; a method that takes none sees an empty object.
  static Json const EMPTY_PARAMS = Json::object();
  auto const paramsField = request.find( "params" );
  bool const hasParams = paramsField != request.end();
  if ( hasParams && !paramsField->is_object() )
  {
    return errorResponse( *id, badRequest( "Field params must be an object" ) );
  }

  auto const handler = mHandlers.find( method );
  if ( handler == mHandlers.end() )
  {
    return errorResponse( *id, Error{ .code = "unknown_method", .message = "Unknown method: " + method } );
  }

  auto outcome = handler->second( hasParams ? *paramsField : EMPTY_PARAMS );
  if ( !outcome )
  {
    return errorResponse( *id, outcome.error() );
  }
  return resultResponse( *id, std::move( *outcome ) );
}

std::vector<std::string> Dispatcher::methodNames() const
{
  std::vector<std::string> names;
  names.reserve( mHandlers.size() );
  for ( auto const& [name, handler] : mHandlers )
  {
    names.push_back( name );
  }
  std::ranges::sort( names );
  return names;
}

std::vector<Method> Dispatcher::methods() const
{
  std::vector<Method> methods;
  methods.reserve( mInfo.size() );
  for ( auto const& [name, info] : mInfo )
  {
    methods.push_back( Method{ .name = name, .info = info } );
  }
  std::ranges::sort( methods, {}, &Method::name );
  return methods;
}

void Dispatcher::add( std::string_view name, MethodInfo info, Handler handler )
{
  mHandlers.emplace( name, std::move( handler ) );
  mInfo.emplace( name, std::move( info ) );
}

void Dispatcher::alias( std::string_view alias, std::string_view name )
{
  mHandlers.emplace( alias, mHandlers.at( std::string{ name } ) );
}

} // namespace pgm::control
