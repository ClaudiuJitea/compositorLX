#!/usr/bin/env bash
# Keep the vendored C kernels in step with the macOS Compositor repo.
#   scripts/sync-kernels.sh /path/to/Compositor          report drift
#   scripts/sync-kernels.sh /path/to/Compositor --apply  copy the mac versions over (then re-apply the Linux shims)
# Linux shims: headers get extern "C" (wrap-extern-c.py); DitherPixels.c loses libdispatch/blocks (port-dither-kernel.py).
set -euo pipefail
mac="${1:?usage: $0 <mac-repo> [--apply]}"
apply="${2:-}"
here="$(cd "$(dirname "$0")" && pwd)"
src="$mac/Compositor/Rendering"
dst="$here/../vendor/compositor-rendering"
tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' EXIT
drift=0
for f in "$src"/*Pixels.[ch] "$src"/ContentFill.[ch]; do
  [ -f "$f" ] || continue
  b="$(basename "$f")"
  cp "$f" "$tmp/$b"
  case "$b" in
    *.h) python3 "$here/wrap-extern-c.py" "$tmp/$b" ;;
    DitherPixels.c) python3 "$here/port-dither-kernel.py" "$tmp/$b" ;;
  esac
  if [ ! -f "$dst/$b" ]; then echo "MISSING  $b"; drift=1
  elif ! diff -q "$tmp/$b" "$dst/$b" >/dev/null; then echo "DRIFT    $b ($(diff "$tmp/$b" "$dst/$b" | grep -c '^[<>]') lines)"; drift=1
  else continue; fi
  [ "$apply" = --apply ] && cp "$tmp/$b" "$dst/$b"
done
[ $drift = 0 ] && echo "kernels in sync"
exit 0
