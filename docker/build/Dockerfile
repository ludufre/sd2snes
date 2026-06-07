# Local build image for the ludufre sd2snes fork (full firmware, no server).
#
# Compiles, in one container:
#   - MCU firmware  (firmware.img / .im3 / .stm)  -> arm-none-eabi-gcc
#   - SNES menu     (menu.bin / m3nu.bin)         -> Bisqwit snescom/sneslink
#   - ESP companion (esp32.bin / esp8266.bin)     -> PlatformIO
#
# The mk3/STM32 firmware embeds the FPGA bootstrap core (cfgware = fpga_mini.bi3),
# a Cyclone IV bitstream that only Quartus can synthesize. The fork never touches
# the FPGA, so we synthesize fpga_mini ONCE in a throwaway build stage (with
# Quartus) and copy just the small .bi3 into the final image - Quartus itself
# (~GBs) never ships in the final image. The runtime cores (fpga_base.bi3 ...)
# are unchanged and come from the official base zip in the full release zip.
#
# Build context = repo root (so the FPGA stage can COPY verilog/ + utils/);
# see .dockerignore. Base is amd64-only -> emulated on Apple Silicon.
ARG BUILD_PLATFORM=linux/amd64

# ============================================================================
# Stage 1 (fpga): synthesize the mk3 FPGA bootstrap core -> fpga_mini.bi3.
# Discarded except for the one .bi3 the final stage copies out.
# ============================================================================
FROM --platform=${BUILD_PLATFORM} stronglytyped/arm-none-eabi-gcc:latest AS fpga
ENV DEBIAN_FRONTEND=noninteractive LANG=C.UTF-8 LC_ALL=C.UTF-8
ARG QUARTUS_VER=23.1std.0.991
ARG QUARTUS_DIR=/opt/intelFPGA_lite/23.1std

# host compiler (for utils/rle) + Quartus runtime libs + the locale it expects.
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential make ca-certificates locales \
        libglib2.0-0 libtcmalloc-minimal4 libpng16-16 libfreetype6 fontconfig \
        libsm6 libxext6 libxrender1 libx11-6 libxft2 \
    && sed -i 's/^# *en_US.UTF-8/en_US.UTF-8/' /etc/locale.gen && locale-gen \
    && rm -rf /var/lib/apt/lists/*

# Quartus Prime Lite + Cyclone IV E support (the mini is an EP4CE15F17C8). The
# installer + cyclone .qdz are downloaded on the HOST (build-docker.sh) into
# dist/quartus/ - the old base image's TLS can't validate Intel's CDN cert chain,
# but the host can. They must sit together so the unattended run finds the .qdz.
COPY dist/quartus/ /tmp/quartus/
RUN cd /tmp/quartus \
    && chmod +x "QuartusLiteSetup-${QUARTUS_VER}-linux.run" \
    && "./QuartusLiteSetup-${QUARTUS_VER}-linux.run" --mode unattended --accept_eula 1 \
         --installdir "${QUARTUS_DIR}" \
         --disable-components quartus_help,quartus_update,modelsim_ase,modelsim_ae \
    && rm -rf /tmp/quartus
# Quartus bundles its own curl lib that clashes with the system one; neutralize
# just that. (We keep Quartus's bundled libstdc++ - swapping it for the system one
# corrupts quartus_map's heap: "realloc(): invalid old size".)
RUN find "${QUARTUS_DIR}" -name libccl_curl_drl.so -exec mv {} {}.disabled \; ; true

WORKDIR /build
COPY utils/   utils/
COPY verilog/ verilog/
# build the RLE compressor (rbf -> bi3), then synthesize fpga_mini and verify it.
RUN make -C utils >/dev/null
RUN cd verilog/sd2snes_mini \
    && LANG=en_US.UTF-8 LC_ALL=en_US.UTF-8 \
       LD_PRELOAD=/usr/lib/x86_64-linux-gnu/libtcmalloc_minimal.so.4 \
       make mk3 INTEL_BIN="${QUARTUS_DIR}/quartus/bin" QUARTUS_ROOTDIR="${QUARTUS_DIR}/quartus" \
    && test -s fpga_mini.bi3

# ============================================================================
# Stage 2: lean build image (arm-none-eabi-gcc + snescom + PlatformIO). NO Quartus.
# ============================================================================
ARG BUILD_PLATFORM=linux/amd64
FROM --platform=${BUILD_PLATFORM} stronglytyped/arm-none-eabi-gcc:latest

ARG SNESCOM_VER=1.8.1.1
ENV DEBIAN_FRONTEND=noninteractive
# PlatformIO (Click) aborts under the base image's ASCII locale; force UTF-8.
ENV LANG=C.UTF-8 LC_ALL=C.UTF-8

# host build tools (utils/, src/utils/), menu codegen (python3), snescom build
# deps, and packaging tools (gzip for the WebUI, zip for the release archives).
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential gawk make git curl wget unzip zip gzip xz-utils xxd \
        python3 python3-pip python3-venv ca-certificates \
    && rm -rf /var/lib/apt/lists/*

# context is the repo root, so reference the helper by its full path.
COPY docker/build/install-snescom.sh /tmp/install-snescom.sh
RUN bash /tmp/install-snescom.sh "${SNESCOM_VER}" && rm -f /tmp/install-snescom.sh

# The base image's Python is 3.6; PlatformIO's current esptool needs cryptography
# 45+ (Python 3.7+). Drop in a relocatable standalone Python (downloaded on the
# HOST into dist/python/ by build-docker.sh) just for the pio venv. The system
# python3 (3.6) is left alone - the menu codegen only needs a plain interpreter.
COPY dist/python/ /tmp/python/
RUN tar -xf /tmp/python/*.tar.gz -C /opt && rm -rf /tmp/python   # -> /opt/python

# PlatformIO in its own venv. Platform packages (xtensa toolchains, frameworks)
# download on the first `pio run` into /root/.platformio - mount a volume there
# to cache them across runs.
RUN /opt/python/bin/python3 -m venv /opt/pio \
    && /opt/pio/bin/pip install --no-cache-dir -U pip \
    && /opt/pio/bin/pip install --no-cache-dir platformio
ENV PATH="/opt/pio/bin:${PATH}"

# The base image keeps the ARM toolchain inside /work (and on PATH). We bind-mount
# the repo at /work, which would shadow it - so move it to /opt and re-point PATH.
RUN mv /work/gcc-arm-none-eabi-* /opt/arm-none-eabi
ENV PATH="/opt/arm-none-eabi/bin:${PATH}"

# the prebuilt mk3 cfgware (synthesized in stage 1); build-docker.sh drops it into
# the bind-mounted verilog/ before the mk3/STM32 firmware build.
COPY --from=fpga /build/verilog/sd2snes_mini/fpga_mini.bi3 /opt/cfgware/fpga_mini.bi3

WORKDIR /work
