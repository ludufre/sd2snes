FROM ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive
ENV TZ=UTC

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential \
        ca-certificates \
        curl \
        gawk \
        gcc-arm-none-eabi \
        libnewlib-arm-none-eabi \
        make \
        unzip \
        wget \
        zip \
    && rm -rf /var/lib/apt/lists/*

# Build and install Bisqwit's snescom + sneslink (assembler/linker for the SNES menu).
ARG SNESCOM_VER=1.8.1.1
RUN set -eux; \
    cd /tmp; \
    wget -q "https://bisqwit.iki.fi/src/arch/snescom-${SNESCOM_VER}.tar.gz"; \
    tar xzf "snescom-${SNESCOM_VER}.tar.gz"; \
    cd "snescom-${SNESCOM_VER}"; \
    # Strip x86-only flags so the build works on arm64 too.
    sed -i 's/-minline-stringops-dynamically//g' Makefile.sets; \
    # Be lenient with warnings from newer GCC; the upstream code was written for GCC 7-ish.
    make CXXFLAGS="-O2 -std=c++1z -fpermissive -w" \
         CPPFLAGS="-DVERSION=\"\\\"${SNESCOM_VER}\\\"\" -DUSE_PTHREADS=0 -DHASH_MAP=1 -g -pipe"; \
    install -m755 snescom sneslink /usr/local/bin/; \
    cd /; \
    rm -rf "/tmp/snescom-${SNESCOM_VER}" "/tmp/snescom-${SNESCOM_VER}.tar.gz"

WORKDIR /workspace

CMD ["bash", "build.sh"]
