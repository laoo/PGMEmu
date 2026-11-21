// The board's sound through Web Audio: an AudioWorklet fed the frames of
// each run, with the rate they come at. A browser starts sound only after the
// person does something on the page, so start() is called from the first
// click or key.

export class Sound
{
  constructor()
  {
    this.context = null;
    this.node = null;
    this.gain = null;
    this.queuedSeconds = 0;
    this.volume = 1;
    this.starting = null;
  }

  get started()
  {
    return this.node !== null;
  }

  // Opens the device; false where the browser has no Web Audio.
  start()
  {
    this.starting ??= this.open().catch( ( error ) =>
    {
      console.warn( "no sound:", error );
      return false;
    } );
    return this.starting;
  }

  async open()
  {
    this.context = new AudioContext();
    await this.context.audioWorklet.addModule( new URL( "./sound-worklet.js", import.meta.url ) );
    const node = new AudioWorkletNode( this.context, "pgmemu-sound", { numberOfInputs: 0, outputChannelCount: [ 2 ] } );
    node.port.onmessage = ( event ) =>
    {
      this.queuedSeconds = event.data;
    };
    this.gain = new GainNode( this.context, { gain: this.volume } );
    node.connect( this.gain ).connect( this.context.destination );
    await this.context.resume();
    this.node = node;
    return true;
  }

  // Queues `samples`, interleaved 16-bit left and right at `rate` frames a
  // second, copied: they are a view of the module's memory.
  push( samples, rate )
  {
    if ( !this.node || !samples || samples.length === 0 || !( rate > 0 ) )
    {
      return;
    }
    const floats = new Float32Array( samples.length );
    for ( let i = 0; i < samples.length; ++i )
    {
      floats[ i ] = samples[ i ] / 32768;
    }
    this.node.port.postMessage( { rate, samples: floats }, [ floats.buffer ] );
  }

  // Drops what is queued: after a pause, so that sound resumes at once.
  clear()
  {
    this.node?.port.postMessage( "clear" );
  }

  setVolume( volume )
  {
    this.volume = volume;
    if ( this.gain )
    {
      this.gain.gain.value = volume;
    }
  }
}
