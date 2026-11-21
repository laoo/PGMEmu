// The page: the core built for WebAssembly (src/pgm_web), a canvas, Web Audio,
// the keyboard and gamepads, and the files it is given, kept in IndexedDB
// (docs/decisions/0026-the-page-is-plain-modules.md).

import createPgmEmu from "./pgmemu.js";
import { CONTROLS, KEYBOARD_PLAYERS, Input } from "./input.js";
import { Sound } from "./sound.js";
import { forgetImage, getImage, listImages, putImage } from "./images.js";

// The board's frame rate, some 59.19 Hz: 640 dots by 264 lines of the 10 MHz
// pixel clock. And the most frames run to catch up after the page stalled:
// the rest is dropped.
const FRAMES_PER_SECOND = 10_000_000 / ( 640 * 264 );
const MAX_FRAMES_OWED = 4;
// How often the saves are written to IndexedDB while a game runs.
const SAVE_EVERY_MS = 30_000;
// The picture is shown 4:3, as the board's monitor showed it.
const SHAPE = 4 / 3;
// The most the picture is enlarged dot for dot before the browser fits it.
const MAX_PRESCALE = 6;

// The directories kept in IndexedDB: the BIOS, each game's battery-backed
// RAM, and the save states. An image is written to ROMS only while it loads.
// They are under a directory named after the page, as IDBFS names a
// directory's database by its path, and every page of one origin, each
// project's on laoo.github.io, shares that origin's databases
// (docs/decisions/0028-the-page-names-its-storage-after-itself.md).
const ROOT = "/pgmemu";
const BIOS = `${ ROOT }/bios`;
const ROMS = `${ ROOT }/roms`;
const SAVES = `${ ROOT }/saves`;
const STATES = `${ ROOT }/states`;
const KEPT = [ BIOS, SAVES, STATES ];
// The motherboard's ROMs, as MAME's `pgm` set names them.
const BIOS_FILES = [ "pgm_p02s.u20", "pgm_t01s.rom", "pgm_m01s.rom" ];
// The set name that loads the motherboard alone, and what stands for it in
// the list of kept games.
const BIOS_ALONE = "pgm";
const BIOS_ALONE_CHOICE = ":bios";
// What the region codes of docs/spec/pgm-format.md §4.3 stand for.
const REGIONS = { WRLD: "World", HGKG: "Hong Kong", JAPN: "Japan", KREA: "Korea", TAWN: "Taiwan", CHNA: "China", USOA: "USA", SNGP: "Singapore" };

const SETTINGS_KEY = "pgmemu.settings";

const $ = ( id ) => document.getElementById( id );

// `last` is the image loaded last, and `regions` the region chosen for each.
const settings = { upright: true, whole: false, volume: 0.8, keepRam: true, last: "", regions: {} };
try
{
  Object.assign( settings, JSON.parse( localStorage.getItem( SETTINGS_KEY ) ?? "{}" ) );
}
catch
{
  // No storage: the defaults, for this visit.
}

function keepSettings()
{
  try
  {
    localStorage.setItem( SETTINGS_KEY, JSON.stringify( settings ) );
  }
  catch
  {
    // Kept for this visit only.
  }
}

function showMessage( text, error = false )
{
  const message = $( "message" );
  message.textContent = text;
  message.hidden = !text;
  message.classList.toggle( "error", error );
}

// The module, with the kept directories read back from IndexedDB.
const module = await createPgmEmu();
const { FS, IDBFS } = module;
const WIDTH = module.SCREEN_WIDTH;
const HEIGHT = module.SCREEN_HEIGHT;
FS.mkdir( ROOT );
FS.mkdir( ROMS );
for ( const directory of KEPT )
{
  FS.mkdir( directory );
  FS.mount( IDBFS, {}, directory );
}

// One synchronisation at a time: IDBFS does not take two at once.
let syncing = Promise.resolve();
function sync( fromIndexedDb )
{
  syncing = syncing.then( () => new Promise( ( resolve ) =>
  {
    FS.syncfs( fromIndexedDb, ( error ) =>
    {
      if ( error )
      {
        console.warn( "IndexedDB:", error );
      }
      resolve();
    } );
  } ) );
  return syncing;
}
await sync( true );

const emu = new module.PgmEmu();
const input = new Input();
const sound = new Sound();
sound.setVolume( settings.volume );

function request( method, params = {} )
{
  return JSON.parse( emu.handle( JSON.stringify( { id: 1, method, params } ) ) );
}

function filesIn( directory )
{
  return FS.readdir( directory ).filter( ( name ) => name !== "." && name !== ".." ).sort();
}

function exists( path )
{
  return FS.analyzePath( path ).exists;
}

// The game that runs: its set name, its title, the regions it can be and the
// one it is; null before anything is loaded. Read from the emulator, so that
// what the control protocol loads is followed too.
let game = null;
// The image the page loaded last, and the set it holds: the region chosen for
// that set is kept for that image.
let loaded = null;

function followGame()
{
  const status = request( "emu.status" ).result;
  if ( typeof status.game_name !== "string" )
  {
    game = null;
  }
  else
  {
    const info = request( "emu.cartridge_info" );
    const regions = info.ok ? ( info.result.region_info?.regions ?? [] ).map( ( region ) => region.id ) : [];
    game = {
      set: status.game_name,
      title: info.ok ? info.result.long_name : "PGM BIOS",
      board: info.ok ? info.result.hardware : "",
      regions,
      region: status.region ?? "",
    };
  }
  document.title = game ? `${ game.title } - PGMEmu` : "PGMEmu";
  $( "welcome" ).hidden = game !== null;
  for ( const id of [ "pause", "reset", "save-state", "load-state" ] )
  {
    $( id ).disabled = game === null;
  }
  const select = $( "region" );
  select.replaceChildren( new Option( game?.regions.length ? "Region" : "No regions", "" ) );
  for ( const code of game?.regions ?? [] )
  {
    select.add( new Option( `${ REGIONS[ code ] ?? code } (${ code })`, code ) );
  }
  select.value = game?.region ?? "";
  select.disabled = !game?.regions.length;
}

// --- The screen ---

const canvas = $( "screen" );
const context = canvas.getContext( "2d" );
// The picture as the board draws it, a dot a pixel.
const source = document.createElement( "canvas" );
source.width = WIDTH;
source.height = HEIGHT;
const sourceContext = source.getContext( "2d" );
const image = new ImageData( WIDTH, HEIGHT );
// Whether the picture is shown turned, and how many pixels a dot is drawn as.
let upright = false;
let prescale = 1;

// Draws the picture kept in `source`, each dot `prescale` pixels square. A
// vertical game draws lying with its top at the left; turned a quarter
// anticlockwise, as the desktop application turns it, a dot at (x, y) is
// shown at (y, WIDTH - x).
function paint()
{
  context.imageSmoothingEnabled = false;
  context.setTransform( ...( upright ? [ 0, -prescale, prescale, 0, 0, WIDTH * prescale ] : [ prescale, 0, 0, prescale, 0, 0 ] ) );
  context.drawImage( source, 0, 0 );
}

// Sizes the canvas to the stage: the 224 lines shaped 4:3, or 3:4 when turned,
// in whole multiples of the display's pixels when asked. The dots are not
// square, so the canvas holds the picture enlarged by whole dots, and the
// browser's own smooth scaling fits it: no column is wider than its neighbour.
function fit()
{
  const stage = $( "stage" ).getBoundingClientRect();
  const ratio = window.devicePixelRatio || 1;
  const across = HEIGHT * SHAPE;
  const [ unitWidth, unitHeight ] = upright ? [ HEIGHT, across ] : [ across, HEIGHT ];
  let scale = Math.min( stage.width / unitWidth, stage.height / unitHeight );
  if ( settings.whole )
  {
    scale = Math.max( 1, Math.floor( scale * ratio ) ) / ratio;
  }
  canvas.style.width = `${ unitWidth * scale }px`;
  canvas.style.height = `${ unitHeight * scale }px`;

  const dots = Math.min( MAX_PRESCALE, Math.max( 1, Math.ceil( scale * ratio ) ) );
  const [ width, height ] = upright ? [ HEIGHT * dots, WIDTH * dots ] : [ WIDTH * dots, HEIGHT * dots ];
  if ( canvas.width !== width || canvas.height !== height )
  {
    canvas.width = width;
    canvas.height = height;
    prescale = dots;
    paint();
  }
}
new ResizeObserver( fit ).observe( $( "stage" ) );

// Shows the last complete picture.
function draw()
{
  const pixels = emu.picture();
  if ( !pixels )
  {
    return;
  }
  image.data.set( pixels );
  sourceContext.putImageData( image, 0, 0 );
  const turned = settings.upright && emu.vertical();
  if ( turned !== upright )
  {
    upright = turned;
    fit();
  }
  paint();
}

// --- Running ---

let owed = 0;
let last = performance.now();
let framesThisSecond = 0;
let framesPerSecond = 0;

function showPaused()
{
  $( "pause" ).textContent = emu.paused() ? "Run" : "Pause";
}

function setPaused( paused )
{
  emu.setPaused( paused );
  showPaused();
  if ( paused )
  {
    sound.clear();
  }
}

// A frame did not run whole: a breakpoint or a watchpoint set through the
// protocol stopped it, which pauses the emulator, or the 68000 halted.
function stopped()
{
  if ( emu.paused() )
  {
    showMessage( "Stopped where the debugger asked; emu.status says where. Run goes on." );
  }
  else
  {
    showMessage( "The 68000 has halted. Reset starts it again.", true );
  }
  setPaused( true );
}

// The host's clock paces the board at its own rate, whatever the display's.
function tick( now )
{
  const elapsed = Math.min( ( now - last ) / 1000, 0.25 );
  last = now;
  if ( game && !emu.paused() )
  {
    owed = Math.min( owed + ( elapsed * FRAMES_PER_SECOND ), MAX_FRAMES_OWED );
    let ran = false;
    while ( owed >= 1 )
    {
      owed -= 1;
      emu.setInputs( ...input.words() );
      const whole = emu.runFrame();
      sound.push( emu.audio(), emu.audioRate() );
      ran = true;
      if ( !whole )
      {
        stopped();
        break;
      }
      ++framesThisSecond;
    }
    if ( ran )
    {
      draw();
    }
  }
  else
  {
    owed = 0;
  }
  requestAnimationFrame( tick );
}
requestAnimationFrame( tick );

setInterval( () =>
{
  framesPerSecond = framesThisSecond;
  framesThisSecond = 0;
  showStatus();
  showPaused();
}, 1000 );

// --- The battery-backed RAM and the states ---

// What the running game's files are named after: its set and its region. A
// game of another region is another board; Oriental Legend, given the RAM its
// other region left, refuses to start.
function gameKey()
{
  return game.region ? `${ game.set }.${ game.region }` : game.set;
}

// The board keeps its work RAM on a battery: the settings a game makes in its
// test mode and its high scores are there. The page keeps it for each game.
function ramFile()
{
  return `${ SAVES }/${ gameKey() }.nv`;
}

function stateFile()
{
  return `${ gameKey() }.pgmstate`;
}

// Gives the game that has just been powered up the RAM kept for it.
function restoreRam()
{
  if ( game && settings.keepRam && exists( ramFile() ) )
  {
    const answer = request( "nvram.load", { filename: ramFile() } );
    if ( !answer.ok )
    {
      showMessage( answer.error.message, true );
    }
  }
}

// Writes the running game's RAM into SAVES.
function writeRam()
{
  if ( game && settings.keepRam )
  {
    const answer = request( "nvram.save", { filename: ramFile() } );
    if ( !answer.ok )
    {
      showMessage( answer.error.message, true );
    }
  }
}

// Writes the RAM, and the kept directories to IndexedDB.
async function keepSaves()
{
  writeRam();
  await sync( false );
}
setInterval( keepSaves, SAVE_EVERY_MS );
document.addEventListener( "visibilitychange", () =>
{
  if ( document.visibilityState === "hidden" )
  {
    input.releaseAll();
    keepSaves();
  }
} );

// --- Files ---

// A file name the file system takes.
function safeName( name )
{
  return name.replace( /[/\\]/g, "_" ) || "game.pgm";
}

// Where the BIOS files are looked for: BIOS itself, for the three files as
// they are, then each zip kept there. The first that has a file wins, so a
// file given apart replaces the zip's.
function applyBios()
{
  const zips = filesIn( BIOS ).filter( ( name ) => name.toLowerCase().endsWith( ".zip" ) );
  request( "emu.set_bios", { sources: [ BIOS, ...zips.map( ( name ) => `${ BIOS }/${ name }` ) ] } );
}

// The names of a zip archive's entries, read from its central directory;
// none when `blob` is not a zip.
async function zipNames( blob )
{
  // The end record is the archive's last 22 bytes, or stands before a comment
  // of up to 64 KB.
  const tail = new DataView( await blob.slice( Math.max( 0, blob.size - 0x10015 ) ).arrayBuffer() );
  let end = tail.byteLength - 22;
  while ( end >= 0 && tail.getUint32( end, true ) !== 0x06054b50 )
  {
    --end;
  }
  if ( end < 0 )
  {
    return [];
  }
  const count = tail.getUint16( end + 10, true );
  const size = tail.getUint32( end + 12, true );
  const offset = tail.getUint32( end + 16, true );
  const bytes = new Uint8Array( await blob.slice( offset, offset + size ).arrayBuffer() );
  const directory = new DataView( bytes.buffer );
  const decoder = new TextDecoder();
  const names = [];
  let at = 0;
  for ( let i = 0; i < count && at + 46 <= bytes.length && directory.getUint32( at, true ) === 0x02014b50; ++i )
  {
    const nameLength = directory.getUint16( at + 28, true );
    names.push( decoder.decode( bytes.subarray( at + 46, at + 46 + nameLength ) ) );
    at += 46 + nameLength + directory.getUint16( at + 30, true ) + directory.getUint16( at + 32, true );
  }
  return names;
}

// Whether `blob` begins as a `.pgm` image does (docs/spec/pgm-format.md §2).
async function isImage( blob )
{
  const magic = new TextDecoder().decode( await blob.slice( 0, 6 ).arrayBuffer() );
  return magic === "IGSPGM";
}

// What follows a power-up the page asked for.
function started()
{
  followGame();
  restoreRam();
  setPaused( false );
  sound.clear();
  showMessage( "" );
  draw();
}

// Loads the image `blob`, kept as `name`. The image is in the module's file
// system only while the core reads it: the core holds it whole from then on.
async function load( name, blob )
{
  if ( filesIn( BIOS ).length === 0 )
  {
    settings.last = name;
    keepSettings();
    showMessage( "The BIOS is needed first: MAME's pgm.zip. Open it with BIOS, or drop it here." );
    return;
  }
  showMessage( `Loading ${ name }…` );
  writeRam();
  const path = `${ ROMS }/${ name }`;
  // Handed over, not copied: an image is tens of megabytes.
  FS.writeFile( path, new Uint8Array( await blob.arrayBuffer() ), { canOwn: true } );
  let error = emu.load( path, settings.regions[ name ] ?? "" );
  if ( error && settings.regions[ name ] )
  {
    // The region chosen for another build of the image; as the image says.
    delete settings.regions[ name ];
    error = emu.load( path, "" );
  }
  FS.unlink( path );
  if ( error )
  {
    showMessage( error, true );
    return;
  }
  settings.last = name;
  keepSettings();
  started();
  loaded = { name, set: game.set };
}

// Loads the image kept as `name`.
async function loadKept( name )
{
  const blob = await getImage( name ).catch( () => undefined );
  if ( !blob )
  {
    showMessage( `${ name } is not kept in this browser any more.`, true );
    return;
  }
  await load( name, blob );
}

function loadBiosAlone()
{
  writeRam();
  const answer = request( "emu.load_game", { name: BIOS_ALONE } );
  if ( !answer.ok )
  {
    showMessage( answer.error.message, true );
    return;
  }
  started();
}

// Keeps a file of the BIOS, and loads the game that waited for it.
async function keepBios( name, blob )
{
  FS.writeFile( `${ BIOS }/${ name }`, new Uint8Array( await blob.arrayBuffer() ) );
  applyBios();
  await sync( false );
  await showFiles();
  if ( game )
  {
    showMessage( "The BIOS is kept. The next game loaded runs with it." );
  }
  else if ( settings.last && ( await getImage( settings.last ).catch( () => undefined ) ) )
  {
    await loadKept( settings.last );
  }
  else
  {
    showMessage( "The BIOS is kept. Open a game's .pgm image." );
  }
}

// Takes a file given to the page: a BIOS file or the zip that holds them is
// kept in BIOS, an image is kept and loaded.
async function take( name, blob )
{
  const lower = name.toLowerCase();
  if ( BIOS_FILES.includes( lower ) )
  {
    await keepBios( lower, blob );
    return;
  }
  if ( await isImage( blob ) )
  {
    const kept = safeName( name );
    const keeping = putImage( kept, blob ).catch( ( error ) => console.warn( "the image is not kept:", error ) );
    await load( kept, blob );
    await keeping;
    await showFiles();
    return;
  }
  if ( lower.endsWith( ".zip" ) )
  {
    const names = await zipNames( blob );
    if ( names.some( ( entry ) => BIOS_FILES.includes( entry.toLowerCase() ) ) )
    {
      await keepBios( safeName( name ), blob );
      return;
    }
    showMessage( `${ name } holds no BIOS file. A game's MAME set is made a .pgm image with PGMBuilder first.`, true );
    return;
  }
  showMessage( `${ name } is neither a .pgm image nor the BIOS.`, true );
}

async function takeFiles( files )
{
  // The BIOS first, so that an image given with it loads at once.
  const given = [ ...files ].sort( ( a, b ) => Number( a.name.toLowerCase().endsWith( ".pgm" ) ) - Number( b.name.toLowerCase().endsWith( ".pgm" ) ) );
  for ( const file of given )
  {
    try
    {
      await take( file.name, file );
    }
    catch ( error )
    {
      showMessage( `Cannot take ${ file.name }: ${ error.message }`, true );
    }
  }
}

function megabytes( size )
{
  return `${ ( size / 1_048_576 ).toFixed( size < 10_485_760 ? 1 : 0 ) } MB`;
}

function listed( list, text, forget )
{
  const item = document.createElement( "li" );
  const label = document.createElement( "span" );
  label.textContent = text;
  const button = document.createElement( "button" );
  button.type = "button";
  button.textContent = "Forget";
  button.title = "Delete it from this browser";
  button.addEventListener( "click", async () =>
  {
    await forget();
    await showFiles();
  } );
  item.append( label, button );
  list.append( item );
}

async function showFiles()
{
  const images = await listImages().catch( () => [] );
  const kept = $( "kept" );
  kept.replaceChildren( new Option( "Kept games", "" ) );
  for ( const { name } of images )
  {
    kept.add( new Option( name, name ) );
  }
  if ( filesIn( BIOS ).length )
  {
    kept.add( new Option( "The BIOS alone", BIOS_ALONE_CHOICE ) );
  }
  kept.value = "";

  const list = $( "files" );
  list.replaceChildren();
  for ( const { name, size } of images )
  {
    listed( list, `${ name }, ${ megabytes( size ) }`, async () =>
    {
      await forgetImage( name );
      if ( settings.last === name )
      {
        settings.last = "";
        keepSettings();
      }
    } );
  }
  for ( const directory of KEPT )
  {
    for ( const name of filesIn( directory ) )
    {
      listed( list, `${ directory }/${ name }`, async () =>
      {
        FS.unlink( `${ directory }/${ name }` );
        if ( directory === BIOS )
        {
          applyBios();
        }
        await sync( false );
      } );
    }
  }
  if ( !list.childElementCount )
  {
    list.textContent = "Nothing yet.";
  }
}

// --- Status ---

function fact( list, name, value )
{
  const term = document.createElement( "dt" );
  term.textContent = name;
  const detail = document.createElement( "dd" );
  detail.textContent = value;
  list.append( term, detail );
}

function showStatus()
{
  const list = $( "status" );
  list.replaceChildren();
  if ( game )
  {
    const status = request( "emu.status" ).result;
    fact( list, "Game", game.title );
    fact( list, "Set", game.set );
    if ( game.board )
    {
      fact( list, "Board", game.board );
    }
    if ( game.region )
    {
      fact( list, "Region", REGIONS[ game.region ] ?? game.region );
    }
    fact( list, "Frame", String( status.frame ) );
    fact( list, "Speed", emu.paused() ? "paused" : `${ framesPerSecond } frames a second` );
    if ( emu.vertical() )
    {
      fact( list, "Monitor", settings.upright ? "vertical" : "vertical, shown lying" );
    }
  }
  else
  {
    fact( list, "Game", "none" );
  }
  const bios = filesIn( BIOS );
  fact( list, "BIOS", bios.length ? bios.join( ", " ) : "none" );
  fact( list, "Sound", sound.started ? `${ Math.round( sound.queuedSeconds * 1000 ) } ms queued` : "off until a click or key" );
  const pads = input.gamepads();
  $( "gamepads" ).textContent = pads.length ? `Gamepads: ${ pads.map( ( pad ) => pad.id ).join( "; " ) }` : "No gamepad; press one of its buttons to show it.";
}

// --- Keys ---

function keyName( code )
{
  const arrows = { ArrowUp: "↑", ArrowDown: "↓", ArrowLeft: "←", ArrowRight: "→" };
  return arrows[ code ] ?? code.replace( /^Key|^Digit/, "" );
}

function showKeys()
{
  const rows = CONTROLS.map( ( control, index ) =>
  {
    const row = document.createElement( "tr" );
    const name = document.createElement( "td" );
    name.textContent = control;
    row.append( name );
    for ( let player = 0; player < KEYBOARD_PLAYERS; ++player )
    {
      const cell = document.createElement( "td" );
      const bind = document.createElement( "button" );
      bind.type = "button";
      bind.textContent = keyName( input.bindings[ player ][ index ] );
      bind.addEventListener( "click", () =>
      {
        bind.textContent = "press a key…";
        input.capture( player, index, showKeys );
      } );
      cell.append( bind );
      row.append( cell );
    }
    return row;
  } );
  $( "keys" ).replaceChildren( ...rows );
}

// The controls held, on the keyboard or a gamepad, their keys lit.
function showHeld()
{
  const state = input.state();
  [ ...$( "keys" ).rows ].forEach( ( row, control ) =>
  {
    for ( let player = 0; player < KEYBOARD_PLAYERS; ++player )
    {
      row.cells[ player + 1 ].classList.toggle( "held", state[ player ][ control ] );
    }
  } );
}
setInterval( showHeld, 100 );

// --- Wiring ---

function isField( target )
{
  return target instanceof HTMLTextAreaElement || ( target instanceof HTMLInputElement && target.type !== "checkbox" && target.type !== "range" ) || target instanceof HTMLSelectElement;
}

window.addEventListener( "keydown", ( event ) =>
{
  sound.start();
  if ( !input.capturing && isField( event.target ) )
  {
    return;
  }
  if ( input.keyDown( event ) )
  {
    event.preventDefault();
  }
} );
window.addEventListener( "keyup", ( event ) =>
{
  if ( input.keyUp( event ) )
  {
    event.preventDefault();
  }
} );
window.addEventListener( "blur", () => input.releaseAll() );
window.addEventListener( "pointerdown", () => sound.start() );

for ( const id of [ "open", "bios" ] )
{
  $( id ).addEventListener( "change", async ( event ) =>
  {
    await takeFiles( event.target.files );
    event.target.value = "";
  } );
}
$( "kept" ).addEventListener( "change", async ( event ) =>
{
  const chosen = event.target.value;
  event.target.value = "";
  if ( chosen === BIOS_ALONE_CHOICE )
  {
    loadBiosAlone();
  }
  else if ( chosen )
  {
    await loadKept( chosen );
  }
} );

// Another region powers the board up again, as the desktop application's
// Region menu does; it is kept for the image.
$( "region" ).addEventListener( "change", ( event ) =>
{
  const region = event.target.value;
  if ( !region || !game )
  {
    return;
  }
  writeRam();
  const answer = request( "emu.set_region", { region } );
  if ( !answer.ok )
  {
    showMessage( answer.error.message, true );
    followGame();
    return;
  }
  if ( loaded?.set === game.set )
  {
    settings.regions[ loaded.name ] = region;
    keepSettings();
  }
  started();
} );

$( "pause" ).addEventListener( "click", () =>
{
  setPaused( !emu.paused() );
  showMessage( "" );
} );
$( "reset" ).addEventListener( "click", () =>
{
  const answer = request( "emu.reset", { cycles: 100 } );
  showMessage( answer.ok ? "" : answer.error.message, !answer.ok );
  setPaused( false );
} );
$( "save-state" ).addEventListener( "click", async () =>
{
  const answer = request( "state.save", { filename: stateFile() } );
  showMessage( answer.ok ? "State saved." : answer.error.message, !answer.ok );
  await sync( false );
  await showFiles();
} );
$( "load-state" ).addEventListener( "click", () =>
{
  const answer = request( "state.load", { filename: stateFile() } );
  showMessage( answer.ok ? "State loaded." : answer.error.message, !answer.ok );
  draw();
} );

$( "upright" ).checked = settings.upright;
$( "upright" ).addEventListener( "change", ( event ) =>
{
  settings.upright = event.target.checked;
  keepSettings();
  draw();
} );
$( "whole" ).checked = settings.whole;
$( "whole" ).addEventListener( "change", ( event ) =>
{
  settings.whole = event.target.checked;
  keepSettings();
  fit();
} );
$( "volume" ).value = String( settings.volume );
$( "volume" ).addEventListener( "input", ( event ) =>
{
  settings.volume = Number( event.target.value );
  sound.setVolume( settings.volume );
  keepSettings();
} );
$( "keep-ram" ).checked = settings.keepRam;
$( "keep-ram" ).addEventListener( "change", ( event ) =>
{
  settings.keepRam = event.target.checked;
  keepSettings();
} );
$( "reset-keys" ).addEventListener( "click", () =>
{
  input.reset();
  showKeys();
} );

// A request may load another game, save a state, or pause: the page follows.
$( "send" ).addEventListener( "click", async () =>
{
  const answer = emu.handle( $( "request" ).value );
  try
  {
    $( "answer" ).textContent = JSON.stringify( JSON.parse( answer ), null, 2 );
  }
  catch
  {
    $( "answer" ).textContent = answer;
  }
  followGame();
  showPaused();
  draw();
  await sync( false );
  await showFiles();
} );

const stage = $( "stage" );
stage.addEventListener( "dragover", ( event ) =>
{
  event.preventDefault();
  stage.classList.add( "dropping" );
} );
stage.addEventListener( "dragleave", () => stage.classList.remove( "dropping" ) );
stage.addEventListener( "drop", ( event ) =>
{
  event.preventDefault();
  stage.classList.remove( "dropping" );
  takeFiles( event.dataTransfer.files );
} );

applyBios();
followGame();
fit();
showKeys();
await showFiles();
showStatus();

// Files named in the address, ?rom=URL, once for each, are fetched and taken
// as if they were opened: the BIOS, an image, or both. Else the image loaded
// last, if it is still kept.
const urls = new URLSearchParams( location.search ).getAll( "rom" );
if ( urls.length )
{
  for ( const url of urls )
  {
    try
    {
      const response = await fetch( url );
      if ( !response.ok )
      {
        throw new Error( `${ response.status } ${ response.statusText }` );
      }
      const name = decodeURIComponent( new URL( url, location.href ).pathname.split( "/" ).pop() );
      await take( name, await response.blob() );
    }
    catch ( error )
    {
      showMessage( `Cannot fetch ${ url }: ${ error.message }`, true );
    }
  }
}
else if ( settings.last && filesIn( BIOS ).length )
{
  const blob = await getImage( settings.last ).catch( () => undefined );
  if ( blob )
  {
    await load( settings.last, blob );
  }
}
