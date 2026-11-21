# 0026: The page is plain modules on pgm_web, and keeps its files in IndexedDB

**Status:** accepted

## Context

The core runs in a browser through `pgm_web` ([0025](0025-the-core-compiles-to-webassembly.md)),
which leaves the canvas, the sound and the keys to the page. A page has no directory to read the
BIOS from or keep saves in: it gets files from the person, one by one, and forgets them when it
is closed, unless it stores them itself. An image is 40 to 80 MB.

## Decision

- **The page is `web/`:** HTML, CSS and ES modules, with no framework, no bundler and no build
  step of their own. The `wasm` preset copies them beside `pgmemu.js` and `pgmemu.wasm`, so
  `build/wasm/web` is the whole site, served as static files.
- **A file is told by what it is.** A `.pgm` image begins `IGSPGM`. The BIOS is one of its three
  files by name, or a zip that holds one of them; the page reads the zip's directory to see. A
  MAME set's zip is refused, with a word about PGMBuilder: the page does not make images
  ([0021](0021-pgmbuilder-is-not-a-source-of-code.md)).
- **What it is given, it keeps.**
  - `/bios`, `/saves` and `/states` are mounted on Emscripten's IDBFS, and the page writes them
    back to IndexedDB after each change, every 30 seconds while a game runs, and when the page
    is hidden.
  - The images are kept as blobs in an IndexedDB store of the page's own (`web/images.js`), not
    on IDBFS, which reads every file it keeps into memory when the page opens. An image is
    written into the module's file system only while the core reads it, handed over rather than
    copied, and removed after: the core holds it whole from then on.
  - The page loads the last image again when it is opened. Its settings and keys are in
    `localStorage`.
- **The battery-backed RAM is kept, as the board's battery keeps it.** The page saves each
  game's work RAM with `nvram.save` and gives it back at power-up with `nvram.load`. It is kept
  by set and region, and so is a save state: Oriental Legend, given the RAM its other region
  left, stops at "INCORRECT VERSION".
- **The frontend is the desktop application's:**
  - the host's clock paces the frames at 59.19 Hz;
  - the picture is shown 4:3, and a vertical game's is turned a quarter anticlockwise
    ([0017](0017-vertical-games-by-image-flag.md));
  - players 1 and 2 have the application's default keys, and each of the four players the
    gamepad connected in its turn, with its default buttons;
  - a game's regions are chosen from a list, and choosing one powers the board up again;
  - the pause is the emulator's own flag, so `emu.pause`, a breakpoint or a watchpoint asked
    for through the protocol stops the page too.

  Keys are bound by their place on the keyboard (`KeyboardEvent.code`) rather than their symbol,
  so that Z, X, C and V are in a row on any layout.
- **The dots are not square, and the page has no shader.** The canvas holds the picture enlarged
  by whole dots, and the browser's smooth scaling fits that to the 4:3 shape, so that no column
  is drawn wider than its neighbour.
- **The sound is played by an AudioWorklet.** It resamples the ICS2115's frames from the rate
  they come at, which follows the number of voices the game plays, to the device's rate, and
  runs up to half a percent fast or slow to keep its queue near its target, as the desktop
  application's stream does.
- `?rom=URL`, given once or more, fetches the BIOS or an image and takes it as if it were opened.

## Consequences

- The page needs a server, even a local one: browsers do not run modules or worklets from
  `file:`.
- Files kept in one browser are not in another; the page's Forget buttons delete them.
- The page has no touch controls; a phone shows the game, but plays it only with a gamepad or a
  keyboard.
- Rewind, run-ahead, the shaders and the debugger's windows stay the desktop application's. The
  page has a box that sends one request of the control protocol and shows the answer.
