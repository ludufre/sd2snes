#!/usr/bin/env bash
# Regenerate src/assets.cpp from the WebUI sources in src/www (index.html,
# style.css, app.js) - each gzipped and embedded as a PROGMEM byte array (the
# ESP8266 has little RAM, so assets live in flash and are served with send_P;
# PROGMEM is harmless on the ESP32). Run after editing the WebUI.
set -euo pipefail
here="$(cd "$(dirname "$0")/.." && pwd)"
src="$here/src/www"
out="$here/src/assets.cpp"

echo '/* AUTO-GENERATED from src/www by tools/gen_www.sh - do not edit. */' > "$out"
echo '#include <Arduino.h>' >> "$out"
echo '#include "assets.h"' >> "$out"
sizes=""
emit() {  # <srcfile> <symbol>
  local f="$1" sym="$2" len
  gzip -9 -c "$src/$f" > "/tmp/$f.gz"
  len="$(wc -c < "/tmp/$f.gz")"
  # Emit the declaration ourselves and let `xxd -i` (reading stdin) print just the
  # byte list - this avoids `xxd -n`, which older xxd builds (e.g. Ubuntu 18.04)
  # lack. Output is identical to the previous `xxd -i -n` + sed version.
  {
    echo "const unsigned char ${sym}[] PROGMEM = {"
    xxd -i < "/tmp/$f.gz"
    echo "};"
    echo "const unsigned int ${sym}_len = ${len};"
  } >> "$out"
  sizes="$sizes $f.gz=${len}B"
}
emit index.html index_html_gz
emit style.css  style_css_gz
emit app.js     app_js_gz
echo "wrote $out ($sizes )"
