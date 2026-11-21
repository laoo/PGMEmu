# 0027: The WebAssembly page is published on gh-pages, by hand

**Status:** accepted

## Context

The page in `web/` runs the emulator in a browser ([0026](0026-the-page-is-plain-modules.md)).
The owner wants it on GitHub Pages, published as the owner's other projects publish their sites:
built on the owner's machine and pushed to a branch of its own, not by a workflow.

## Decision

- `scripts/publish-site.sh` builds the `wasm` preset and composes `build/wasm/web` into
  `build/site`, a clone of the orphan `gh-pages` branch, with a `.nojekyll`. Pages serves that
  branch at `laoo.github.io/PGMEmu`.
- It composes by default, and pushes only with `--publish`.
- The branch holds one commit, amended at each publish and force-pushed with a lease, as every
  build rewrites the whole page. Its message names the commit of `main` it was built from.
- The branch shares no ancestor with `main`: no source file on it, no built page on `main`.
- The script fails when the page names a file that the build did not put beside it.

## Consequences

- Publishing needs Emscripten on the machine that publishes.
- A change to the page or the core reaches Pages only when it is published again.
- The page carries no ROM: a visitor opens their own BIOS and images, as on the desktop.
- The repository's Pages must be set, once, to serve the `gh-pages` branch.
