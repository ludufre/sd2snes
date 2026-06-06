# Xilinx ISE 14.7 WebPACK image - synthesizes the mk2 (Spartan-3) FPGA bootstrap
# core fpga_mini.bit. Heavy + only needed for the "full" self-contained build.
#
# The installer (~8GB, login-gated) is provided by the user, reassembled on the
# HOST into dist/xilinx/installer (part-1 .tar + parts 2-4 which are 7z), and the
# WebPACK license into dist/xilinx/Xilinx.lic. Both are bind-mounted/copied in at
# build time - never committed, never in the final sd2snes image.
#
# amd64-only base -> runs emulated on Apple Silicon (ISE is old and fragile under
# emulation; the tcmalloc preload that fixed Quartus is applied to the synth too).
ARG BUILD_PLATFORM=linux/amd64
FROM --platform=${BUILD_PLATFORM} stronglytyped/arm-none-eabi-gcc:latest

ENV DEBIAN_FRONTEND=noninteractive LANG=C.UTF-8 LC_ALL=C.UTF-8 TERM=xterm
ARG ISE_DIR=/opt/Xilinx

# host compiler (utils/rle) + ISE runtime libs (CLI synth: xst/ngdbuild/map/par/bitgen).
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential make ca-certificates \
        libstdc++6 libtcmalloc-minimal4 \
        libglib2.0-0 libgtk2.0-0 libxtst6 \
        libxmu6 libxi6 libxrender1 libxext6 libsm6 libice6 libxft2 \
        libfreetype6 libfontconfig1 libtinfo5 zlib1g \
    && rm -rf /var/lib/apt/lists/*

# Unattended ISE WebPACK install. The installer dir = extracted part-1 tree with
# the original parts 2-4 (.zip.xz) sitting next to it; batchxsetup reads those
# multi-part archives itself. Bind-mounted at /installer (not COPYed - it is ~8GB).
# `yes` pages through / accepts the EULA; TMPDIR keeps the installer's scratch off
# the read-only-ish mount.
COPY docker/ise14.7/install_config.txt /tmp/install_config.txt
RUN --mount=type=bind,source=dist/xilinx/installer,target=/installer,rw \
    cd /installer \
    && TMPDIR=/tmp yes | TMPDIR=/tmp ./bin/lin64/batchxsetup --batch /tmp/install_config.txt \
    && test -x "${ISE_DIR}/14.7/ISE_DS/ISE/bin/lin64/xst"

# ISE 14.7's bundled libstdc++ is too old for modern glibc; drop it so the tools
# use the system one (the documented fix, same as prepare.sh on the server).
RUN for f in "${ISE_DIR}"/14.7/ISE_DS/ISE/lib/lin/libstdc++.so.6 \
             "${ISE_DIR}"/14.7/ISE_DS/ISE/lib/lin64/libstdc++.so.6; do \
      [ -f "$f" ] && mv "$f" "$f.orig" || true; done; true

# xtclsh's startup runs `hwcosim_compile --print-boards` (ISim co-simulation),
# which has a shell bug here and dumps the board list to STDOUT - that pollutes
# the $(shell xtclsh xgenmapcmd.tcl) capture in common.mk and corrupts the map/par
# command line. hwcosim is irrelevant to synthesis, so stub it out (empty output).
RUN f="${ISE_DIR}/14.7/ISE_DS/ISE/sysgen/bin/lin64/hwcosim_compile"; \
    [ -f "$f" ] && printf '#!/bin/sh\nexit 0\n' > "$f" && chmod +x "$f"; true

# WebPACK license (HOSTID=ANY -> valid anywhere, incl. this container).
COPY dist/xilinx/Xilinx.lic /opt/Xilinx.lic
ENV XILINXD_LICENSE_FILE=/opt/Xilinx.lic

# synthesize the Spartan-3 bootstrap core -> fpga_mini.bit (mk2 path in
# verilog/common.mk: xst/ngdbuild/map/par/bitgen, then utils/rle). tcmalloc dodges
# the same emulated-glibc heap corruption that bit Quartus.
WORKDIR /build
COPY utils/   utils/
COPY verilog/ verilog/
RUN make -C utils >/dev/null
RUN cd verilog/sd2snes_mini \
    && LD_PRELOAD=/usr/lib/x86_64-linux-gnu/libtcmalloc_minimal.so.4 \
       make mk2 XILINX_HOME="${ISE_DIR}/14.7/ISE_DS" \
    && test -s fpga_mini.bit \
    && mkdir -p /opt/cfgware && cp fpga_mini.bit /opt/cfgware/fpga_mini.bit
