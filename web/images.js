// The `.pgm` images the page is given, kept in IndexedDB as blobs
// (docs/decisions/0026-the-page-is-plain-modules.md). They are not in the
// module's file system with the BIOS and the saves: IDBFS reads every file it
// keeps into memory when the page opens, and an image is tens of megabytes. A
// blob stays on disk until the game it holds is loaded.

const DATABASE = "pgmemu-images";
const STORE = "images";

let opening = null;

function open()
{
  opening ??= new Promise( ( resolve, reject ) =>
  {
    const request = indexedDB.open( DATABASE, 1 );
    request.onupgradeneeded = () => request.result.createObjectStore( STORE );
    request.onsuccess = () => resolve( request.result );
    request.onerror = () => reject( request.error );
  } );
  return opening;
}

// Runs `work` on the store in one transaction; answers what its request gives.
async function within( mode, work )
{
  const database = await open();
  return new Promise( ( resolve, reject ) =>
  {
    const transaction = database.transaction( STORE, mode );
    const request = work( transaction.objectStore( STORE ) );
    transaction.oncomplete = () => resolve( request.result );
    transaction.onerror = () => reject( transaction.error );
    transaction.onabort = () => reject( transaction.error );
  } );
}

// Keeps `blob` under `name`, in place of what was kept under it.
export function putImage( name, blob )
{
  return within( "readwrite", ( store ) => store.put( blob, name ) );
}

// The blob kept under `name`, or undefined.
export function getImage( name )
{
  return within( "readonly", ( store ) => store.get( name ) );
}

export function forgetImage( name )
{
  return within( "readwrite", ( store ) => store.delete( name ) );
}

// The names kept and their sizes in bytes, by name.
export async function listImages()
{
  const database = await open();
  return new Promise( ( resolve, reject ) =>
  {
    const images = [];
    const request = database.transaction( STORE, "readonly" ).objectStore( STORE ).openCursor();
    request.onsuccess = () =>
    {
      const cursor = request.result;
      if ( !cursor )
      {
        resolve( images );
        return;
      }
      images.push( { name: String( cursor.key ), size: cursor.value.size } );
      cursor.continue();
    };
    request.onerror = () => reject( request.error );
  } );
}
