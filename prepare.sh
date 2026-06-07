#!/usr/bin/env bash
#
# prepare.sh — provision the build SERVER for the sd2snes (ludufre PT-BR fork).
#
# Run this ONCE on a fresh server (from the Mac; it SSHes in and installs):
#   - dnf deps + ARM toolchain (arm-none-eabi-gcc-cs, EL9/EPEL) for the MCU firmware
#   - Bisqwit's snescom/sneslink (65816 assembler/linker) for the SNES menu
#   - Intel Quartus Prime Lite 23.1 + Cyclone IV E device support for the FPGA
#     cores (the FXPAK PRO / mk3 FPGA is an EP4CE15F17C8, "Cyclone IV E")
#   - QUARTUS_ROOTDIR in the server's environment
#   - (optional, mk2) the sd2snes-ise:14.7 runtime image for synthesizing the
#     Spartan-3 fpga_mini.bit. The ISE *install* (~18GB) + WebPACK license are
#     MANUAL (login-gated installer) — see docker/ise14.7/README.md.
#
# Idempotent: re-running skips anything already installed.
# After this, use ./build.sh to build.
#
# Env overrides:
#   SD2SNES_SERVER=root@172.29.8.103
#   QUARTUS_VER=23.1std.0.991      Quartus Lite build to install
#   QUARTUS_DIR=/opt/intelFPGA_lite/23.1std
#   QUARTUS_RUN_URL=...            full URL to QuartusLiteSetup-<ver>-linux.run
#   QUARTUS_CYCLONE_URL=...        full URL to cyclone-<ver>.qdz (Cyclone IV/V support)
#   SNESCOM_VER=1.8.1.1
#   SKIP_QUARTUS=1                 skip the (large/fragile) Quartus step
#   SKIP_ISE=1                     skip the optional mk2/ISE container step
#   SD2SNES_ISE_OPT=/root/ise-build/opt   host dir holding the ISE install

set -euo pipefail

SERVER="${SD2SNES_SERVER:-root@172.29.8.103}"
SNESCOM_VER="${SNESCOM_VER:-1.8.1.1}"
QUARTUS_VER="${QUARTUS_VER:-23.1std.0.991}"
QUARTUS_DIR="${QUARTUS_DIR:-/opt/intelFPGA_lite/23.1std}"
# Intel/Altera direct download links (no login). These DO move occasionally — if
# the download 404s, grab the files manually from the Intel/Altera download
# center and re-run with QUARTUS_RUN_URL=/QUARTUS_CYCLONE_URL= pointing at them.
QUARTUS_BASE="${QUARTUS_BASE:-https://downloads.intel.com/akdlm/software/acdsinst/23.1std/991/ib_installers}"
QUARTUS_RUN_URL="${QUARTUS_RUN_URL:-${QUARTUS_BASE}/QuartusLiteSetup-${QUARTUS_VER}-linux.run}"
QUARTUS_CYCLONE_URL="${QUARTUS_CYCLONE_URL:-${QUARTUS_BASE}/cyclone-${QUARTUS_VER}.qdz}"

log()  { printf '\033[1;34m==>\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33m!!\033[0m %s\n' "$*" >&2; }

# run a script on the server
rs() { ssh -o BatchMode=yes -o ConnectTimeout=30 "$SERVER" "set -euo pipefail; $*"; }

log "Provisioning ${SERVER}"

# ---------------------------------------------------------------------------
# 1. dnf packages: ARM toolchain + build tools + Quartus/ISE runtime libs.
#    Server is Oracle Linux 9 (EL9); the ARM cross-toolchain lives in EPEL.
# ---------------------------------------------------------------------------
log "Installing dnf packages (Oracle Linux 9 / EL9; root assumed)"
rs 'dnf install -y oracle-epel-release-el9 2>/dev/null || dnf install -y epel-release || true
    dnf config-manager --set-enabled ol9_codeready_builder ol9_developer_EPEL 2>/dev/null || true
    dnf install -y \
      gcc gcc-c++ make gawk glibc-devel which \
      ca-certificates curl wget git unzip zip rsync python3 \
      arm-none-eabi-gcc-cs arm-none-eabi-newlib arm-none-eabi-binutils-cs \
      libstdc++ libpng freetype fontconfig glib2 ncurses-libs \
      libSM libXext libXrender libX11 libXft'
# ---------------------------------------------------------------------------
# 2. snescom / sneslink (Bisqwit) — same recipe as the Dockerfile.
# ---------------------------------------------------------------------------
if rs 'command -v snescom >/dev/null 2>&1'; then
    log "snescom already installed — skipping"
else
    log "Building + installing snescom/sneslink ${SNESCOM_VER}"
    rs "cd /tmp; \
        wget -q 'https://bisqwit.iki.fi/src/arch/snescom-${SNESCOM_VER}.tar.gz'; \
        tar xzf 'snescom-${SNESCOM_VER}.tar.gz'; \
        cd 'snescom-${SNESCOM_VER}'; \
        sed -i 's/-minline-stringops-dynamically//g' Makefile.sets; \
        make CXXFLAGS='-O2 -std=c++1z -fpermissive -w' \
             CPPFLAGS='-DVERSION=\"\\\"${SNESCOM_VER}\\\"\" -DUSE_PTHREADS=0 -DHASH_MAP=1 -g -pipe'; \
        install -m755 snescom sneslink /usr/local/bin/; \
        cd /; rm -rf '/tmp/snescom-${SNESCOM_VER}' '/tmp/snescom-${SNESCOM_VER}.tar.gz'"
fi

# ---------------------------------------------------------------------------
# 3. Intel Quartus Prime Lite 23.1 + Cyclone IV E device support.
#    (Big download ~5GB; best-effort. The FPGA cores are pure hardware with no
#    strings — needed only to (re)build the .bi3 cores from verilog/.)
# ---------------------------------------------------------------------------
if [ "${SKIP_QUARTUS:-0}" = "1" ]; then
    warn "SKIP_QUARTUS=1 — not touching Quartus"
elif rs "[ -x '${QUARTUS_DIR}/quartus/bin/quartus_sh' ]"; then
    log "Quartus already present at ${QUARTUS_DIR} — skipping"
else
    log "Installing Quartus Prime Lite ${QUARTUS_VER} (this is large; ~5GB)"
    warn "If the downloads 404, fetch QuartusLiteSetup + cyclone .qdz manually from the"
    warn "Intel/Altera download center and re-run with QUARTUS_RUN_URL=/QUARTUS_CYCLONE_URL=."
    rs "mkdir -p /tmp/quartus-dl; cd /tmp/quartus-dl; \
        wget -q --show-progress -O QuartusLiteSetup.run  '${QUARTUS_RUN_URL}'; \
        wget -q --show-progress -O cyclone.qdz           '${QUARTUS_CYCLONE_URL}'; \
        chmod +x QuartusLiteSetup.run; \
        ./QuartusLiteSetup.run --mode unattended --accept_eula 1 \
            --installdir '${QUARTUS_DIR}' \
            --disable-components quartus_help,quartus_update; \
        cd /; rm -rf /tmp/quartus-dl"
fi

# ---------------------------------------------------------------------------
# 4. Put QUARTUS_ROOTDIR on the server's PATH/env (for non-interactive ssh too).
# ---------------------------------------------------------------------------
log "Setting QUARTUS_ROOTDIR in /etc/profile.d/sd2snes-quartus.sh"
rs "cat > /etc/profile.d/sd2snes-quartus.sh <<'EOF'
export QUARTUS_ROOTDIR=${QUARTUS_DIR}/quartus
export PATH=\$QUARTUS_ROOTDIR/bin:\$PATH
EOF
chmod 644 /etc/profile.d/sd2snes-quartus.sh"

# ---------------------------------------------------------------------------
# 5. Xilinx ISE 14.7 (OPTIONAL — only for the mk2 / Spartan-3 build).
#    The ISE *install* + WebPACK license are manual (see docker/ise14.7/README.md);
#    here we automate what we can: build the runtime image, and apply the
#    libstdc++ fix if an install is already present. Skip with SKIP_ISE=1.
# ---------------------------------------------------------------------------
ISE_OPT="${SD2SNES_ISE_OPT:-/root/ise-build/opt}"
if [ "${SKIP_ISE:-0}" = "1" ]; then
    warn "SKIP_ISE=1 — not touching the ISE container/install"
else
    log "Building the ISE runtime image sd2snes-ise:14.7"
    ssh -o BatchMode=yes "$SERVER" 'docker build -t sd2snes-ise:14.7 -' \
        < "$(dirname "$0")/docker/ise14.7/Dockerfile"
    if rs "[ -d '${ISE_OPT}/Xilinx/14.7/ISE_DS/ISE' ]"; then
        log "ISE install found at ${ISE_OPT} — applying libstdc++ fix (idempotent)"
        rs "for f in '${ISE_OPT}'/Xilinx/14.7/ISE_DS/ISE/lib/lin/libstdc++.so.6 \
                     '${ISE_OPT}'/Xilinx/14.7/ISE_DS/ISE/lib/lin64/libstdc++.so.6; do \
              [ -f \"\$f\" ] && mv \"\$f\" \"\$f.orig\" || true; done; true"
    else
        warn "No ISE install at ${ISE_OPT} — the mk2 build needs one. Follow the one-time"
        warn "steps in docker/ise14.7/README.md (reassemble installer + batchxsetup + WebPACK"
        warn "license). Until then, ./build.sh (default) translates mk3 only; --mk2 will fail."
    fi
fi

# ---------------------------------------------------------------------------
# 6. Verify.
# ---------------------------------------------------------------------------
log "Verifying toolchain"
rs 'echo -n "  arm-none-eabi-gcc: "; arm-none-eabi-gcc --version 2>/dev/null | head -1 || echo MISSING
    echo -n "  snescom:           "; command -v snescom >/dev/null && echo OK || echo MISSING
    echo -n "  quartus_sh:        "; '"'${QUARTUS_DIR}'"'/quartus/bin/quartus_sh --version 2>/dev/null | head -1 || echo "MISSING (FPGA cores can come from the official zip instead)"'
rs "echo -n '  sd2snes-ise:14.7:  '; docker image inspect sd2snes-ise:14.7 >/dev/null 2>&1 && echo OK || echo 'MISSING (mk2 only)'
    echo -n '  ISE install:       '; [ -x '${ISE_OPT}/Xilinx/14.7/ISE_DS/ISE/bin/lin64/xst' ] && echo OK || echo 'MISSING (mk2 only — see docker/ise14.7/README.md)'"

log "Done. The server is provisioned — run ./build.sh to build (add --mk2 for mk2)."
