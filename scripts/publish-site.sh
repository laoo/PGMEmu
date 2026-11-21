#!/usr/bin/env bash
# Builds the WebAssembly page and puts it on `gh-pages`, which GitHub Pages
# serves at laoo.github.io/PGMEmu
# (docs/decisions/0027-the-page-is-published-on-gh-pages-by-hand.md).
#
#   scripts/publish-site.sh             build and compose it into build/site, and stop
#   scripts/publish-site.sh --publish   the same, folded into the branch's one commit, force-pushed
#
# Composing is the default; what leaves this machine is asked for in words.
# Needs Emscripten (brew install emscripten). `gh-pages` holds one commit,
# amended every time and force-pushed with a lease: every build rewrites the
# whole page, and the sources' history has all of it. The branch is an orphan
# and shares no ancestor with `main`: it never holds a source file, and `main`
# never holds the built page. Its clone is build/site, made on the first run.
# Kept free of bash-4 features: macOS ships bash 3.2.
set -euo pipefail
root="$( cd "$( dirname "${BASH_SOURCE[0]}" )/.." && pwd )"
dest="$root/build/site"
built="$root/build/wasm/web"
branch=gh-pages

mode=compose
case "${1-}" in
  "") ;;
  --publish) mode=publish ;;
  *) echo "publish-site.sh: unknown argument $1" >&2; exit 1 ;;
esac

command -v emcmake >/dev/null || { echo "publish-site.sh: Emscripten is needed (brew install emscripten)" >&2; exit 1; }
# Emscripten's CMake and the libraries it builds are chatty; what they say is
# shown when the build fails.
log="$root/build/publish-site.log"
mkdir -p "$root/build"
( cd "$root" && emcmake cmake --preset wasm && cmake --build --preset wasm ) >"$log" 2>&1 || { cat "$log" >&2; exit 1; }

# Every file the page names is beside it: the scripts it imports, its
# stylesheet, the worklet and the module's .wasm. The names are read from the
# page's own files, so a file added to it and not to PGM_PAGE_FILES fails here.
for file in index.html pgmemu.js pgmemu.wasm; do
  [[ -f "$built/$file" ]] || { echo "publish-site.sh: $file is not in $built" >&2; exit 1; }
done
for name in $( grep -ho -E '(src|href)="[^":#]+"|from "\./[^"]+"|"\./[^"]+\.js"' "$built"/index.html "$built"/*.js |
               sed -E 's/^(src|href)=//; s/^from //; s/"//g; s|^\./||' | sort -u ); do
  [[ -f "$built/$name" ]] || { echo "publish-site.sh: the page names $name, which is not in $built" >&2; exit 1; }
done

if [[ ! -d "$dest/.git" ]]; then
  remote="$( git -C "$root" remote get-url origin )"
  mkdir -p "$dest"
  if git ls-remote --exit-code --heads "$remote" "$branch" >/dev/null 2>&1; then
    echo "cloning $branch of $remote into build/site"
    git clone --quiet --branch "$branch" --single-branch "$remote" "$dest"
  else
    echo "$branch does not exist on the remote yet; starting it here as an orphan"
    git -C "$dest" init --quiet --initial-branch="$branch"
    git -C "$dest" remote add origin "$remote"
  fi
fi
if [[ "$( git -C "$dest" branch --show-current )" != "$branch" ]]; then
  echo "publish-site.sh: build/site is not on $branch; that clone is the page's and nothing else's" >&2
  exit 1
fi

# The page whole, and nothing else: no Jekyll, which would hide nothing here
# but would take its time.
find "$dest" -mindepth 1 -maxdepth 1 ! -name .git -exec rm -rf {} +
cp "$built"/* "$dest"/
touch "$dest/.nojekyll"

if [[ "$mode" == compose ]]; then
  echo "composed only: serve it with python3 -m http.server -d build/site, then run with --publish"
  exit 0
fi

git -C "$dest" add -A
message="The WebAssembly page, built from $( git -C "$root" rev-parse --short HEAD )"
if git -C "$dest" rev-parse --verify --quiet HEAD >/dev/null; then
  git -C "$dest" commit --quiet --amend -m "$message"
else
  git -C "$dest" commit --quiet -m "$message"
fi
if git -C "$dest" rev-parse --abbrev-ref --symbolic-full-name '@{u}' >/dev/null 2>&1; then
  git -C "$dest" push --quiet --force-with-lease
else
  git -C "$dest" push --quiet -u origin "$branch"
fi
echo "published $( git -C "$dest" log -1 --format='%h %s' )"
