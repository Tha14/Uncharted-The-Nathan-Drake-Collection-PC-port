#!/bin/bash
# swap.sh in|out BATCHDIR : copy BATCHDIR/u3data/... over overlay-r3 (originals saved to BATCHDIR.bak), or restore them
W="$HOME/mnt/Uncharted The Nathan Drake Collection - U3 Multiplayer/work"; S="$W/stage-assets"; B="$S/$2"; K="$S/$2.bak"
cd "$B" || exit 1
find u3data -type f | while read -r f; do
 dst="$W/overlay-r3/$f"
 if [ "$1" = in ]; then
  mkdir -p "$K/$(dirname "$f")" "$(dirname "$dst")"
  if [ -f "$dst" ]; then cp -p "$dst" "$K/$f"; else echo "$f" >> "$K.new"; fi
  cp "$f" "$dst"; echo "in  $f $(md5sum < "$dst" | cut -c1-8)"
 else
  if [ -f "$K/$f" ]; then cp -p "$K/$f" "$dst"; echo "restored $f $(md5sum < "$dst" | cut -c1-8)"
  elif grep -qxF "$f" "$K.new" 2>/dev/null; then rm -f "$dst"; echo "removed new $f"; fi
 fi
done
