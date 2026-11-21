// The keyboard and the gamepads as the board's controls, as the desktop
// application has them (src/pgm_app/src/InputMap.cpp): players 1 and 2 on the
// keyboard, each of the four players on the gamepad connected in its turn.
// Keys are bound by their place on the keyboard (KeyboardEvent.code), so that
// Z, X, C and V are in a row whatever the layout.

// A player's controls, in the order the page lists them.
export const CONTROLS = [ "Up", "Down", "Left", "Right", "Button 1", "Button 2", "Button 3", "Button 4", "Start", "Coin" ];

// How many players the keyboard is bound for, and how many the board has.
export const KEYBOARD_PLAYERS = 2;
const PLAYERS = 4;

// The desktop application's keys: player 1 on the arrows, Z, X, C, V, 1 and 5;
// player 2 on R, F, D, G, A, S, Q, W, 2 and 6, as MAME has them.
const DEFAULTS = [
  [ "ArrowUp", "ArrowDown", "ArrowLeft", "ArrowRight", "KeyZ", "KeyX", "KeyC", "KeyV", "Digit1", "Digit5" ],
  [ "KeyR", "KeyF", "KeyD", "KeyG", "KeyA", "KeyS", "KeyQ", "KeyW", "Digit2", "Digit6" ],
];

const STORAGE_KEY = "pgmemu.keys";

// How far a stick is pushed before it counts as a direction.
const STICK_THRESHOLD = 0.5;

// The standard mapping's buttons (https://w3c.github.io/gamepad/#remapping).
const PAD = { south: 0, east: 1, west: 2, north: 3, back: 8, start: 9, up: 12, down: 13, left: 14, right: 15 };

// Where a player's control is in the board's input words, IN0 to IN3 (PGM.sv):
// players 1 and 2 have the two bytes of IN0, players 3 and 4 those of IN1, each
// start, up, down, left, right and buttons 1 to 3 from its bit 0; IN2 holds the
// coins in its low nibble and the buttons 4 from bit 8.
function bitOf( player, control )
{
  const word = player >> 1;
  const shift = ( player & 1 ) * 8;
  switch ( control )
  {
  case 0: return [ word, 1 << ( 1 + shift ) ];
  case 1: return [ word, 1 << ( 2 + shift ) ];
  case 2: return [ word, 1 << ( 3 + shift ) ];
  case 3: return [ word, 1 << ( 4 + shift ) ];
  case 4: return [ word, 1 << ( 5 + shift ) ];
  case 5: return [ word, 1 << ( 6 + shift ) ];
  case 6: return [ word, 1 << ( 7 + shift ) ];
  case 7: return [ 2, 1 << ( 8 + player ) ];
  case 8: return [ word, 1 << shift ];
  default: return [ 2, 1 << player ];
  }
}

// The controls a gamepad holds, as a list of whether each of CONTROLS is held:
// its pad and left stick the directions, south, east, west and north buttons
// 1 to 4, start, and back for a coin.
function heldOn( pad )
{
  const pressed = ( index ) => pad.buttons[ index ]?.pressed ?? false;
  const x = pad.axes[ 0 ] ?? 0;
  const y = pad.axes[ 1 ] ?? 0;
  return [
    pressed( PAD.up ) || y < -STICK_THRESHOLD,
    pressed( PAD.down ) || y > STICK_THRESHOLD,
    pressed( PAD.left ) || x < -STICK_THRESHOLD,
    pressed( PAD.right ) || x > STICK_THRESHOLD,
    pressed( PAD.south ),
    pressed( PAD.east ),
    pressed( PAD.west ),
    pressed( PAD.north ),
    pressed( PAD.start ),
    pressed( PAD.back ),
  ];
}

export class Input
{
  constructor()
  {
    this.bindings = DEFAULTS.map( ( keys ) => [ ...keys ] );
    // The codes of the keys held down.
    this.held = new Set();
    // The binding the next key pressed becomes, and who is told.
    this.capturing = null;
    try
    {
      const kept = JSON.parse( localStorage.getItem( STORAGE_KEY ) ?? "null" );
      for ( let player = 0; player < KEYBOARD_PLAYERS; ++player )
      {
        if ( Array.isArray( kept?.[ player ] ) && kept[ player ].length === CONTROLS.length )
        {
          this.bindings[ player ] = kept[ player ].map( String );
        }
      }
    }
    catch
    {
      // No storage, or nothing readable in it: the defaults.
    }
  }

  save()
  {
    try
    {
      localStorage.setItem( STORAGE_KEY, JSON.stringify( this.bindings ) );
    }
    catch
    {
      // Kept for this visit only.
    }
  }

  reset()
  {
    this.bindings = DEFAULTS.map( ( keys ) => [ ...keys ] );
    this.save();
  }

  // The next key pressed becomes `player`'s `control`, and `done` is told;
  // Escape keeps it as it is.
  capture( player, control, done )
  {
    this.capturing = { player, control, done };
  }

  // Whether `code` is bound to a control.
  bound( code )
  {
    return this.bindings.some( ( keys ) => keys.includes( code ) );
  }

  // Takes a key going down; whether it was the board's.
  keyDown( event )
  {
    if ( this.capturing )
    {
      const { player, control, done } = this.capturing;
      this.capturing = null;
      if ( event.code !== "Escape" )
      {
        this.bindings[ player ][ control ] = event.code;
        this.save();
      }
      done();
      return true;
    }
    if ( event.ctrlKey || event.metaKey || event.altKey || !this.bound( event.code ) )
    {
      return false;
    }
    this.held.add( event.code );
    return true;
  }

  keyUp( event )
  {
    return this.held.delete( event.code );
  }

  // Lets go of every key: when the page loses the keyboard.
  releaseAll()
  {
    this.held.clear();
  }

  // Whether each control of each player is held, on the keyboard or on the
  // player's gamepad: a list of CONTROLS' length for each of the four.
  state()
  {
    const pads = this.gamepads();
    const state = [];
    for ( let player = 0; player < PLAYERS; ++player )
    {
      const keys = this.bindings[ player ] ?? [];
      const pad = pads[ player ] ? heldOn( pads[ player ] ) : [];
      state.push( CONTROLS.map( ( name, control ) => this.held.has( keys[ control ] ) || pad[ control ] === true ) );
    }
    return state;
  }

  // The controls held, as the words IN0 to IN3 with a bit set for each.
  words()
  {
    const words = [ 0, 0, 0, 0 ];
    this.state().forEach( ( controls, player ) =>
    {
      controls.forEach( ( held, control ) =>
      {
        if ( held )
        {
          const [ word, bit ] = bitOf( player, control );
          words[ word ] |= bit;
        }
      } );
    } );
    return words;
  }

  // The gamepads connected, in the order they were: the first is player 1's.
  gamepads()
  {
    return navigator.getGamepads ? [ ...navigator.getGamepads() ].filter( ( pad ) => pad?.connected ) : [];
  }
}
