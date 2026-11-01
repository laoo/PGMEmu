cmake_minimum_required( VERSION 3.25 )

project( PGMEmu
  DESCRIPTION "Emulator of the IGS PolyGame Master arcade board, ported from its FPGA core"
  LANGUAGES   C CXX )

list( APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake" )

# No `VERSION` above: the version is the tag, and cmake/Version.cmake says why.
include( Version )
pgmResolveVersion( PGM_VERSION_STRING )
message( STATUS "PGMEmu version: ${PGM_VERSION_STRING}" )

set( CMAKE_CXX_STANDARD          23 )
set( CMAKE_CXX_STANDARD_REQUIRED ON )
set( CMAKE_CXX_EXTENSIONS        OFF )
set( CMAKE_EXPORT_COMPILE_COMMANDS ON )

option( PGM_BUILD_TESTS "Build the test suite"                     ON )
option( PGM_BUILD_APP   "Build the SDL3 desktop application"       ON )
option( PGM_WERROR      "Treat compiler warnings as errors"        OFF )

include( Warnings )
include( Optimisation )
include( Dependencies )

add_subdirectory( libextern )

add_subdirectory( src/pgm_core )
add_subdirectory( src/pgm_server )
add_subdirectory( src/pgm_cli )

# The application is the one target that needs SDL and ImGui, so a headless
# build, the suite and CI can leave both out.
if( PGM_BUILD_APP )
  add_subdirectory( src/pgm_app )
endif( )

if( PGM_BUILD_TESTS )
  enable_testing( )
  add_subdirectory( tests )
endif( )

include( Install )
