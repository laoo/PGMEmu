# 0028: The page names its storage after itself

**Status:** accepted

## Context

The page is served at `laoo.github.io/PGMEmu`, and WsMcp's at `laoo.github.io/WsMcp`. A browser
keeps IndexedDB and `localStorage` by origin, and both pages are of one origin. Emscripten's IDBFS
names a mounted directory's database by the directory's path. Both pages mounted `/bios`, `/saves`
and `/states` ([0026](0026-the-page-is-plain-modules.md)), so they shared three databases: each
listed the other's boot ROMs, saves and states as its own, read them all into memory when it
opened, and offered to forget them.

## Decision

- The directories the page keeps are `/pgmemu/bios`, `/pgmemu/saves` and `/pgmemu/states`, and an
  image is written to `/pgmemu/roms` while it loads. The binding's own defaults are the same
  directories, so that a save state named without a directory, through the protocol's box, is
  kept where the page's are.
- Every name the page gives to the origin's storage begins with the project's: those
  directories, the images' database (`pgmemu-images`) and the `localStorage` keys (`pgmemu.keys`,
  `pgmemu.settings`).
- Nothing is moved from the databases the first published page wrote to. It was up for an hour;
  what it kept there is given to the page again, and forgotten from WsMcp's page, which lists it.

## Consequences

- Another page of the owner's on the same origin keeps clear of this one by naming its storage
  after itself in the same way. WsMcp's page still mounts the bare names.
- A page that embeds the module and wants its own files kept apart mounts other directories and
  names them to the protocol: `emu.set_bios`, and paths with a directory for `state.save` and
  `nvram.save`.
