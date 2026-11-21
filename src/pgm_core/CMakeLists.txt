add_library( pgm_core
  src/Emulator.cpp
  src/PortabilityChecks.cpp
  src/Version.cpp
  src/cart/Bios.cpp
  src/cart/PgmImage.cpp
  src/control/AudioMethods.cpp
  src/control/Batch.cpp
  src/control/CpuMethods.cpp
  src/control/Describe.cpp
  src/control/Dispatcher.cpp
  src/control/EmuMethods.cpp
  src/control/InputMethods.cpp
  src/control/MemoryMethods.cpp
  src/control/Params.cpp
  src/control/RunMethods.cpp
  src/control/StateMethods.cpp
  src/control/TestMethods.cpp
  src/control/TraceMethods.cpp
  src/control/VideoMethods.cpp
  src/cpu/Arm7.cpp
  src/cpu/Arm7Disassembler.cpp
  src/cpu/Arm7Thumb.cpp
  src/cpu/MoiraConfigCheck.cpp
  src/io/RomSources.cpp
  src/machine/Asic3.cpp
  src/machine/Boards.cpp
  src/machine/Bus68k.cpp
  src/machine/Ics2115.cpp
  src/machine/Igs022.cpp
  src/machine/Igs022Igs025Board.cpp
  src/machine/Igs023.cpp
  src/machine/Igs025.cpp
  src/machine/Igs027a.cpp
  src/machine/Igs026.cpp
  src/machine/M68k.cpp
  src/machine/Machine.cpp
  src/machine/SpriteEngine.cpp
  src/machine/V3021.cpp
  src/machine/Z80.cpp )

add_library( pgm::core ALIAS pgm_core )

target_include_directories( pgm_core PUBLIC include PRIVATE src )

target_compile_definitions( pgm_core PRIVATE PGM_VERSION="${PGM_VERSION_STRING}" )

# nlohmann_json is PUBLIC because the dispatcher's interface is JSON: a request
# goes in as one and a response comes out as one, whichever transport carries
# them. spdlog is PUBLIC as in NGA: every program built on the core logs through
# the one instance, and linking it twice is what Apple's linker warns about.
# The rest is PRIVATE: no header of the core includes it.
target_link_libraries( pgm_core
  PUBLIC  nlohmann_json::nlohmann_json spdlog::spdlog
  PRIVATE pgm::warnings miniz moira chips_z80 )
