#include "CpuWindow.hpp"

#include <imgui.h>
#include <spdlog/fmt/fmt.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

namespace pgm::app
{

namespace
{

constexpr char const* WINDOW = "CPU state";

/// How long a tab shows an answer before it asks again. The interface waits
/// for each answer, and the emulation thread answers only between two of its
/// frames; asked on every frame of the interface, the window would leave the
/// interface waiting out an emulated frame whenever the emulation runs behind.
constexpr double REFRESH_SECONDS = 0.5;

/// Whether it is time to ask again for what was last asked at `fetchedAt`.
bool due( double fetchedAt )
{
  return fetchedAt < 0.0 || ImGui::GetTime() - fetchedAt >= REFRESH_SECONDS;
}

bool succeeded( control::Json const& response )
{
  return response.at( "ok" ) == true;
}

/// A row of a table of registers: the register's name, then its value.
void row( std::string const& name, std::string const& value )
{
  ImGui::TableNextRow();
  ImGui::TableNextColumn();
  ImGui::TextUnformatted( name.c_str() );
  ImGui::TableNextColumn();
  ImGui::TextUnformatted( value.c_str() );
}

/// A field of a state, in at least `digits` hexadecimal digits.
std::string hex( control::Json const& field, int digits )
{
  return fmt::format( "{:0{}X}", field.get<std::uint32_t>(), digits );
}

char const* yesNo( control::Json const& field )
{
  return field.get<bool>() ? "yes" : "no";
}

/// After the value on a row: a letter a bit of `value`, from bit `top` down,
/// lit when the bit is set and dimmed when it is clear. The letters of one
/// call run together, apart from those of the last.
void drawFlags( std::uint32_t value, unsigned top, std::string_view letters )
{
  for ( std::size_t i = 0; i < letters.size(); ++i )
  {
    ImGui::SameLine( 0.0F, i == 0 ? -1.0F : 0.0F );
    if ( ( ( value >> ( top - i ) ) & 1U ) != 0 )
    {
      ImGui::Text( "%c", letters.at( i ) );
    }
    else
    {
      ImGui::TextDisabled( "%c", letters.at( i ) );
    }
  }
}

void drawM68k( control::Json const& state )
{
  row( "PC",
       fmt::format(
           "{:06X}  {}", state.at( "pc" ).get<std::uint32_t>(), state.at( "disasm" ).get_ref<std::string const&>() ) );
  auto const& d = state.at( "d" );
  for ( std::size_t i = 0; i < d.size(); ++i )
  {
    row( fmt::format( "D{}", i ), hex( d.at( i ), 8 ) );
  }
  auto const& a = state.at( "a" );
  for ( std::size_t i = 0; i < a.size(); ++i )
  {
    row( fmt::format( "A{}", i ), hex( a.at( i ), 8 ) );
  }
  auto const sr = state.at( "sr" ).get<std::uint32_t>();
  row( "SR", hex( state.at( "sr" ), 4 ) );
  drawFlags( sr, 15, "T" );
  drawFlags( sr, 13, "S" );
  ImGui::SameLine();
  ImGui::Text( "I%u", ( sr >> 8U ) & 7U );
  drawFlags( sr, 4, "XNZVC" );
  row( "USP", hex( state.at( "usp" ), 8 ) );
  row( "SSP", hex( state.at( "ssp" ), 8 ) );
  row( "Stopped", yesNo( state.at( "stopped" ) ) );
  row( "Halted", yesNo( state.at( "halted" ) ) );
}

void drawZ80( control::Json const& state )
{
  row( "PC", hex( state.at( "pc" ), 4 ) );
  row( "SP", hex( state.at( "sp" ), 4 ) );
  row( "AF", hex( state.at( "af" ), 4 ) );
  drawFlags( state.at( "af" ).get<std::uint32_t>(), 7, "SZYHXPNC" );
  row( "BC", hex( state.at( "bc" ), 4 ) );
  row( "DE", hex( state.at( "de" ), 4 ) );
  row( "HL", hex( state.at( "hl" ), 4 ) );
  row( "IX", hex( state.at( "ix" ), 4 ) );
  row( "IY", hex( state.at( "iy" ), 4 ) );
  row( "AF'", hex( state.at( "af_" ), 4 ) );
  row( "BC'", hex( state.at( "bc_" ), 4 ) );
  row( "DE'", hex( state.at( "de_" ), 4 ) );
  row( "HL'", hex( state.at( "hl_" ), 4 ) );
  row( "WZ", hex( state.at( "wz" ), 4 ) );
  row( "I", hex( state.at( "i" ), 2 ) );
  row( "R", hex( state.at( "r" ), 2 ) );
  row( "IM", fmt::format( "{}", state.at( "im" ).get<unsigned>() ) );
  row( "IFF1", state.at( "iff1" ).get<bool>() ? "1" : "0" );
  row( "IFF2", state.at( "iff2" ).get<bool>() ? "1" : "0" );
  row( "Halted", yesNo( state.at( "halted" ) ) );
}

void drawArm7( control::Json const& state )
{
  row( "PC",
       fmt::format(
           "{:08X}  {}", state.at( "pc" ).get<std::uint32_t>(), state.at( "disasm" ).get_ref<std::string const&>() ) );
  // R15 is the pipeline's, past PC.
  auto const& r = state.at( "r" );
  for ( std::size_t i = 0; i < r.size(); ++i )
  {
    row( fmt::format( "R{}", i ), hex( r.at( i ), 8 ) );
  }
  auto const cpsr = state.at( "cpsr" ).get<std::uint32_t>();
  row( "CPSR", hex( state.at( "cpsr" ), 8 ) );
  drawFlags( cpsr, 31, "NZCV" );
  drawFlags( cpsr, 7, "IFT" );
  row( "SPSR", hex( state.at( "spsr" ), 8 ) );
  row( "Mode", state.at( "mode" ).get_ref<std::string const&>() );
  row( "State", state.at( "thumb" ).get<bool>() ? "Thumb" : "ARM" );
  row( "FIQ line", state.at( "fiq" ).get<bool>() ? "up" : "down" );
  row( "Cycles", fmt::format( "{}", state.at( "cycles" ).get<std::int64_t>() ) );
}

} // namespace

CpuWindow::CpuWindow( Request request ) : mRequest{ std::move( request ) } {}

void CpuWindow::draw( bool& open )
{
  if ( !open )
  {
    return;
  }
  if ( ImGui::Begin( WINDOW, &open ) && ImGui::BeginTabBar( "CPUs" ) )
  {
    if ( ImGui::BeginTabItem( "68000" ) )
    {
      drawCpu( "m68k", mM68k, drawM68k );
      ImGui::EndTabItem();
    }
    if ( ImGui::BeginTabItem( "Z80" ) )
    {
      drawCpu( "z80", mZ80, drawZ80 );
      ImGui::EndTabItem();
    }
    if ( ImGui::BeginTabItem( "ARM7" ) )
    {
      drawCpu( "arm7", mArm7, drawArm7 );
      ImGui::EndTabItem();
    }
    ImGui::EndTabBar();
  }
  ImGui::End();
}

void CpuWindow::drawCpu( char const* cpu, Answer& answer, void ( *layOut )( control::Json const& state ) )
{
  if ( due( answer.fetchedAt ) )
  {
    answer.response = mRequest( "cpu.get_state", control::Json{ { "cpu", cpu } } );
    answer.fetchedAt = ImGui::GetTime();
  }
  control::Json const& response = answer.response;
  if ( !succeeded( response ) )
  {
    ImGui::TextUnformatted( response.at( "error" ).at( "message" ).get_ref<std::string const&>().c_str() );
    return;
  }
  ImGuiTableFlags const flags =
      ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit;
  if ( ImGui::BeginTable( "registers", 2, flags ) )
  {
    ImGui::TableSetupScrollFreeze( 0, 1 );
    for ( char const* heading : { "Register", "Value" } )
    {
      ImGui::TableSetupColumn( heading );
    }
    ImGui::TableHeadersRow();
    layOut( response.at( "result" ) );
    ImGui::EndTable();
  }
}

} // namespace pgm::app
