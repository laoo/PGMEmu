# How the optimised configurations are compiled: -O3 and link-time
# optimisation, for every target of the build, this project's and the fetched
# libraries' alike. Why, and what it was measured to give, is
# docs/decisions/0024-release-builds-are-o3-with-lto.md.
#
# Included before any target exists: the per-configuration IPO variables are
# read when a target is created, and the flags when a directory is.

# CMake's RelWithDebInfo is -O2 -g on GCC and clang. The emulator's hot loops
# gain from -O3's inlining and unrolling; the debug information stays.
if( NOT MSVC )
  foreach( lang IN ITEMS C CXX )
    string( REPLACE "-O2" "-O3" CMAKE_${lang}_FLAGS_RELWITHDEBINFO "${CMAKE_${lang}_FLAGS_RELWITHDEBINFO}" )
  endforeach( )
endif( )

# Link-time optimisation lets the compiler inline across the core's translation
# units, where its devices call one another on every bus cycle. Only where the
# toolchain supports it: a build that cannot is still a correct build.
include( CheckIPOSupported )
check_ipo_supported( RESULT PGM_IPO_SUPPORTED OUTPUT PGM_IPO_UNSUPPORTED_REASON LANGUAGES C CXX )
if( PGM_IPO_SUPPORTED )
  set( CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE        ON )
  set( CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELWITHDEBINFO ON )
else( )
  message( STATUS "Link-time optimisation is not available here: ${PGM_IPO_UNSUPPORTED_REASON}" )
endif( )
