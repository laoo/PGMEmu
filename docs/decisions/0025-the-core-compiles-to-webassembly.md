# 0025: The core compiles to WebAssembly

**Status:** accepted

## Context

The owner wants the emulator to run in a browser as well, as WsMcp, the owner's WonderSwan
emulator, does. The core was already kept free of SDL, threads, file dialogs and the network, so
that the suite, the CLI and the application share it.

## Decision

- `pgm_core` builds with Emscripten (`emcmake cmake --preset wasm`), with a thin binding,
  `pgm_web` (`src/pgm_web`), exported to JavaScript with embind: load an image, run a frame, hold
  the controls, take the RGBA picture and the sound, pause, and the dispatcher's `handle(json)`,
  so that the control protocol reaches a page as it reaches an agent.
- What the binding does not have, a page asks the protocol for: the BIOS's sources, the region,
  reset, save states, the battery-backed RAM. The binding has a method of its own only for what a
  page does every frame, and for loading, which gives an image by its path.
- Files reach the core through Emscripten's in-memory file system: a page writes the BIOS and an
  image there and loads them by path, as the CLI does.
- The core keeps to what Emscripten's libc++ has: no threads, no sockets, no `std::print`. It is
  built as Emscripten builds by default, without C++ exceptions; the core throws none and
  catches none.
- It is optimised as the release configuration is
  ([0024](0024-release-builds-are-o3-with-lto.md)).
- `pgm_server`, `pgm_cli`, the desktop application and the tests are not part of the WebAssembly
  build, and the libraries only they use are not fetched for it.

## Consequences

- A change to the core that needs a thread or a socket belongs in `pgm_server` or the
  application instead.
- The module is 32-bit: a `std::size_t` is 32 bits there, and the core holds an image whole, in
  memory that grows to 2 GB at most.
- CI does not build it ([0022](0022-ci-and-releases.md)); a change that breaks it is found when
  the page is next built.
- It is fast enough: under Node on an Apple M5 Pro it runs Oriental Legend at about 600 frames a
  second, Knights of Valour 2 at about 410 and DoDonPachi III at about 370, against the board's
  59.19, and its pictures are the native build's byte for byte.
