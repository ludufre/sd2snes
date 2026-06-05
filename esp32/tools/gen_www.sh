#!/usr/bin/env bash
# Regenerate src/www_assets.c from the WebUI sources in src/www/ (index.html,
# style.css, app.js) - each gzipped and embedded as a C array (via xxd).
# Run this after editing any of those files.
set -euo pipefail
here="$(cd "$(dirname "$0")/.." && pwd)"
cd "$here/src/www"
out="$here/src/www_assets.c"

echo "/* AUTO-GENERATED from www/{index.html,style.css,app.js} by tools/gen_www.sh - do not edit. */" > "$out"
sizes=""
for f in index.html style.css app.js; do
  gzip -9 -k -f "$f"
  xxd -i "$f.gz" | sed -e 's/^unsigned char/const unsigned char/' \
                       -e 's/^unsigned int/const unsigned int/' >> "$out"
  sizes="$sizes $f.gz=$(wc -c < "$f.gz")B"
done
echo "wrote src/www_assets.c ($sizes )"
