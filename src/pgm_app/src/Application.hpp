#pragma once

#include "AudioOutput.hpp"
#include "CpuWindow.hpp"
#include "EmulationThread.hpp"
#include "Gamepads.hpp"
#include "GpuTexture.hpp"
#include "InputMap.hpp"
#include "InputWindow.hpp"
#include "ScreenRenderer.hpp"
#include "VideoWindow.hpp"

#include "pgm/Emulator.hpp"
#include "pgm/control/Dispatcher.hpp"
#include "pgm/server/McpHttpServer.hpp"
#include "pgm/server/McpServer.hpp"
#include "pgm/server/TcpLineServer.hpp"

#include <SDL3/SDL_gpu.h>
#include <SDL3/SDL_video.h>

#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace pgm::app
{

/// The desktop frontend: an SDL3 window rendered through SDL_GPU, with Dear
/// ImGui docked over it (docs/decisions/0008-the-renderer-is-sdl-gpu.md). It
/// reaches the emulator only through the dispatcher, as every other client does.
class Application
{
public:
  /// Opens the window and the GPU device and sets up ImGui. Answers why when
  /// any of them cannot be had. `settings` say where games and the BIOS are.
  static std::expected<std::unique_ptr<Application>, std::string> create( Settings settings );

  /// Serves the control protocol to other programs while the window is open:
  /// JSON-lines on TCP `linePort`, MCP over HTTP on `mcpPort`, each unless 0.
  /// Their requests run on the emulation thread, on the machine on screen.
  /// Throws std::runtime_error when a port cannot be had.
  void serve( std::uint16_t linePort, std::uint16_t mcpPort );

  /// Loads a game by set name or path, through the dispatcher, as an agent
  /// would; a failure is shown in the status window.
  void loadGame( std::string const& nameOrPath );

  /// Shows the dialog that opens a .pgm file. The file chosen is loaded on a
  /// later frame: the dialog answers on a thread of its own.
  void openFile();
  /// Shows the dialog that chooses the BIOS, pgm.zip; taken as openFile()'s.
  void chooseBios();
  /// Takes the BIOS from `path` from the next load on, and keeps it for the
  /// next start when `keep`.
  void setBios( std::filesystem::path const& path, bool keep );
  /// The BIOS kept from an earlier start, when the command line gave none.
  void restoreBios();
  /// Sets `key` among the application's settings, and writes them when they
  /// are kept.
  void keepSetting( std::string const& key, control::Json value );

  ~Application();

  Application( Application const& ) = delete;
  Application& operator=( Application const& ) = delete;
  Application( Application&& ) = delete;
  Application& operator=( Application&& ) = delete;

  /// Runs until the window is closed or Quit is chosen.
  void run();

private:
  Application( SDL_Window* window, SDL_GPUDevice* device, Settings settings );

  /// Answers a request of the control protocol, on the emulation thread.
  control::Json request( std::string const& method, control::Json params = control::Json::object() );

  /// Hands the keyboard to the emulation thread, and takes the last picture
  /// it completed.
  void updateEmulation();
  /// Loads the file the open dialog chose, if it has.
  void loadChosen();
  /// Names the loaded cartridge in the window's title, and learns which way
  /// up its monitor stood.
  void followGame();

  /// Lays out one frame of the user interface.
  void drawInterface();
  void drawMenuBar();
  /// The loaded game's regions, the one it runs as ticked; choosing another
  /// powers the board up again as that region.
  void drawRegionMenu();
  void drawDisplayMenu();
  void drawScreenWindow();
  void drawStatusWindow();
  void drawSoundWindow();

  /// Uploads what changed, then renders the interface into the swapchain.
  void renderFrame();

  SDL_Window* mWindow;
  SDL_GPUDevice* mDevice;
  std::unique_ptr<GpuTexture> mScreen;
  std::vector<std::uint8_t> mFrame;
  bool mFrameChanged{ true };
  /// Null when no audio device could be opened: the emulation then runs
  /// silent, paced by the clock alone.
  std::unique_ptr<AudioOutput> mAudio;
  std::unique_ptr<EmulationThread> mEmulation;
  std::unique_ptr<server::TcpLineServer> mLineServer;
  std::unique_ptr<server::McpServer> mMcp;
  std::unique_ptr<server::McpHttpServer> mMcpHttp;
  std::unique_ptr<CpuWindow> mCpu;
  std::unique_ptr<VideoWindow> mVideo;
  Gamepads mGamepads;
  /// Where the input map is kept, beside imgui.ini; empty when there is no
  /// such place, and the map is then not kept.
  std::filesystem::path mInputMapPath;
  InputMap mInputMap;
  std::unique_ptr<InputWindow> mInputWindow;
  std::unique_ptr<ScreenRenderer> mRenderer;
  /// Where the display settings are kept, beside imgui.ini; empty when they
  /// are not kept.
  std::filesystem::path mDisplayPath;
  DisplaySettings mDisplay;
  /// Whether the screen was drawn into the renderer's target this frame.
  bool mScreenShown{};
  std::string mImguiIniPath;
  bool mQuit{};
  bool mShowCpu{};
  bool mShowStatus{ true };
  bool mShowSound{};
  bool mShowInput{};
  bool mShowVideo{};
  bool mPaused{};
  /// Whether the screen window had focus when the interface was last drawn.
  bool mScreenFocused{};
  std::int64_t mPicturesShown{ -1 };
  std::string mLastError;
  /// The files the open dialogs chose, waiting to be taken.
  std::mutex mChosenMutex;
  std::optional<std::string> mChosen;
  std::optional<std::string> mChosenBios;
  /// Where the application's own settings are kept, the BIOS among them,
  /// beside imgui.ini; empty when they are not kept. They are read once, and
  /// written whole whenever one changes.
  std::filesystem::path mSettingsPath;
  control::Json mSettings = control::Json::object();
  /// Whether the last seconds are kept to be rewound through.
  bool mRewindKept{ true };
  /// Frames run ahead of each frame kept, 0 to MAX_RUN_AHEAD.
  int mRunAhead{};
  static constexpr int MAX_RUN_AHEAD = 3;
  /// Whether the emulator has been told where the BIOS is.
  bool mHaveBios{};
  /// The game the window's title names, and the frames until it is asked
  /// again what is loaded: a game may be loaded by an agent as well.
  std::optional<std::string> mFollowedGame;
  int mGameCheckIn{};
  /// Whether the screen is shown turned upright, 3:4: as the loaded game's
  /// monitor stood, until the Display menu turns it the other way.
  bool mVertical{};
  bool mShowImguiDemo{};
};

} // namespace pgm::app
