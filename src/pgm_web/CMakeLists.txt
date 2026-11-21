# The core for a browser: a JavaScript module, pgmemu.js and pgmemu.wasm, that
# a page imports (docs/decisions/0025-the-core-compiles-to-webassembly.md),
# put beside the page in web/ (docs/decisions/0026-the-page-is-plain-modules.md):
# build/wasm/web is the whole site.
add_executable( pgm_web src/Bindings.cpp )

set_target_properties( pgm_web PROPERTIES
  OUTPUT_NAME pgmemu
  SUFFIX ".js"
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/web" )

# A module the page instantiates when it wants, with the in-memory file
# system the BIOS and the image are written to, and IndexedDB's under the
# directories the page keeps. An image is tens of megabytes and the core holds
# it whole, so memory grows as it is asked for, up to the 2 GB every browser
# gives. NODEFS is for driving the module from Node, where it reads the files
# where they are.
target_link_options( pgm_web PRIVATE
  -lembind
  -lidbfs.js
  -lnodefs.js
  "-sMODULARIZE=1"
  "-sEXPORT_ES6=1"
  "-sEXPORT_NAME=createPgmEmu"
  "-sALLOW_MEMORY_GROWTH=1"
  "-sMAXIMUM_MEMORY=2GB"
  "-sENVIRONMENT=web,worker,node"
  "-sEXPORTED_RUNTIME_METHODS=FS,IDBFS,NODEFS" )

target_link_libraries( pgm_web PRIVATE pgm::core pgm::warnings )

# The page's own files, copied whenever one changes.
set( PGM_PAGE_FILES index.html style.css main.js input.js sound.js sound-worklet.js images.js )
set( PGM_PAGE_OUTPUTS )
foreach( file IN LISTS PGM_PAGE_FILES )
  add_custom_command(
    OUTPUT  "${CMAKE_BINARY_DIR}/web/${file}"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${PROJECT_SOURCE_DIR}/web/${file}" "${CMAKE_BINARY_DIR}/web/${file}"
    DEPENDS "${PROJECT_SOURCE_DIR}/web/${file}"
    VERBATIM )
  list( APPEND PGM_PAGE_OUTPUTS "${CMAKE_BINARY_DIR}/web/${file}" )
endforeach( )
add_custom_target( pgm_page ALL DEPENDS ${PGM_PAGE_OUTPUTS} )
