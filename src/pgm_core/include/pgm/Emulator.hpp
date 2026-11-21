#pragma once

#include "pgm/cart/Bios.hpp"
#include "pgm/cart/PgmImage.hpp"
#include "pgm/machine/Machine.hpp"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace pgm
{

/// Where the emulator finds what it is asked to load. Supplied by whoever
/// starts it, so that the core never decides where to look on its own.
struct Settings
{
  /// Directories and zip archives the BIOS files are taken from, the first
  /// that has a file winning (io::RomSources).
  std::vector<std::filesystem::path> biosSources;
  /// The directory `<name>.pgm` is looked for in when a game is loaded by name.
  std::filesystem::path romDirectory;
  /// Where `state.save` and `state.load` keep save states named without a
  /// directory, and `state.list` looks; the working directory when empty.
  std::filesystem::path stateDirectory;
};

/// Why a game could not be loaded. `kind` is what the control protocol reports
/// as an error code; `message` says which file and why.
struct LoadFailure
{
  enum class Kind : std::uint8_t
  {
    UNKNOWN_GAME,
    LOAD_FAILED,
    UNKNOWN_REGION
  };

  Kind kind{};
  std::string message;
};

/// A named block of the emulator's memory, as `memory.read` addresses it.
struct MemoryRegion
{
  std::string_view name;
  std::span<std::uint8_t const> bytes;
};

/// The emulated PGM as a whole: what is loaded into it, and, as later
/// milestones add them, the machine that runs it.
class Emulator
{
public:
  /// The name that loads the motherboard alone, with no cartridge. It is the
  /// name of the BIOS set in MAME and in the RTL simulator.
  static constexpr std::string_view BIOS_ONLY = "pgm";

  explicit Emulator( Settings settings );

  /// Loads `<romDirectory>/<name>.pgm` with the BIOS, or the BIOS alone when
  /// `name` is BIOS_ONLY. A name is a set name, not a path: it holds only
  /// letters, digits and underscores. `region`, a four-character code such as
  /// `JAPN`, makes the game that region; without it the game runs as its image
  /// says (cart::PgmImage::ownRegion). On failure what was loaded stays loaded.
  std::expected<void, LoadFailure> loadGameByName( std::string_view name,
                                                   std::optional<std::string_view> region = std::nullopt );

  /// Loads the `.pgm` at `path` with the BIOS, as loadGameByName() does.
  std::expected<void, LoadFailure> loadGameFromFile( std::filesystem::path const& path,
                                                     std::optional<std::string_view> region = std::nullopt );

  /// Powers the board up again with the loaded cartridge, its game made the
  /// region `region` names, as loading it with that region would. On failure
  /// the game runs on undisturbed.
  std::expected<void, LoadFailure> setRegion( std::string_view region );

  /// The short name of the loaded cartridge, BIOS_ONLY for the BIOS alone, or
  /// nothing before a game is loaded.
  [[nodiscard]] std::optional<std::string> gameName() const;

  /// The four-character code of the region the loaded game runs as: the one
  /// chosen, or the one its image holds. Nothing when the image has no
  /// regions, or holds a value its region table does not name.
  [[nodiscard]] std::optional<std::string> region() const;

  [[nodiscard]] cart::Bios const* bios() const;
  [[nodiscard]] cart::PgmImage const* cartridge() const;

  /// The board the loaded game runs on, powered up when it was loaded; null
  /// before a game is loaded.
  [[nodiscard]] machine::Machine* machine();
  [[nodiscard]] machine::Machine const* machine() const;

  [[nodiscard]] Settings const& settings() const;

  /// Takes the BIOS from `sources` from the next load on, as Settings'
  /// biosSources. What is loaded runs on undisturbed.
  void setBiosSources( std::vector<std::filesystem::path> sources );

  /// Every region that holds something now, in a stable order.
  [[nodiscard]] std::vector<MemoryRegion> memoryRegions() const;

  /// How many instructions the 68000's trace keeps (machine::Machine::
  /// setTraceSize): the machine's now, and that of every one powered up after
  /// it, which a game loaded or a region set is.
  [[nodiscard]] std::size_t traceSize() const;
  void setTraceSize( std::size_t instructions );
  /// Whether the trace keeps registers (machine::Machine::setTraceRegisters),
  /// likewise.
  [[nodiscard]] bool traceRegisters() const;
  void setTraceRegisters( bool kept );

  /// Whether the frontend's own run is paused. The emulator runs only when it
  /// is asked to; the desktop application runs it frame after frame, unless
  /// this is set. A breakpoint or a watchpoint that its run reaches sets it.
  [[nodiscard]] bool paused() const;
  void setPaused( bool paused );

private:
  [[nodiscard]] std::expected<cart::Bios, LoadFailure> loadBios() const;

  /// The agnostic id `region` spells, when `cartridge` can run as it.
  [[nodiscard]] static std::expected<std::uint32_t, LoadFailure> chooseRegion( cart::PgmImage const& cartridge,
                                                                               std::string_view region );

  /// Powers a new board up with what is loaded.
  void powerUp();

  /// Makes `bios` and `cartridge` the loaded game and powers a new board up
  /// with them, the game made region `region` when one is given.
  void install( cart::Bios bios, std::optional<cart::PgmImage> cartridge, std::optional<std::uint32_t> region );

  Settings mSettings;
  std::optional<cart::Bios> mBios;
  std::optional<cart::PgmImage> mCartridge;
  /// The agnostic id of the region chosen when the game was loaded.
  std::optional<std::uint32_t> mRegion;
  bool mPaused{};
  std::size_t mTraceSize{ machine::Machine::TRACE_SIZE };
  bool mTraceRegisters{};
  // Declared last, so that it goes before the ROMs it reads from.
  std::unique_ptr<machine::Machine> mMachine;
};

} // namespace pgm
