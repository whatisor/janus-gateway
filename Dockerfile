# Multi-stage env for Janus WebRTC Server (multistream branch)
# Stage 1: dev environment (no source copied; mount your repo and output)
FROM debian:bookworm AS dev

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates git build-essential autoconf automake libtool pkg-config \
    libmicrohttpd-dev libjansson-dev libssl-dev libglib2.0-dev \
    libconfig-dev libcurl4-openssl-dev libwebsockets-dev \
    libogg-dev libopus-dev \
    wget curl python3 meson ninja-build \
    && rm -rf /var/lib/apt/lists/*

# Build and install libnice (recommended newer version)
RUN git clone https://gitlab.freedesktop.org/libnice/libnice /tmp/libnice \
    && cd /tmp/libnice \
    && meson setup --prefix=/usr build \
    && ninja -C build \
    && ninja -C build install \
    && rm -rf /tmp/libnice

# Build and install libsrtp (2.x)
RUN cd /tmp \
    && wget -q https://github.com/cisco/libsrtp/archive/refs/tags/v2.6.0.tar.gz -O libsrtp.tar.gz \
    && tar xzf libsrtp.tar.gz && cd libsrtp-* \
    && ./configure --prefix=/usr --enable-openssl \
    && make -j"$(nproc)" shared_library \
    && make install \
    && rm -rf /tmp/libsrtp-*

WORKDIR /work

# Usage (example):
#  docker build -t janus-dev --target dev .
#  docker run --rm -it \
#    -v $PWD:/work \
#    -v $PWD/out:/out \
#    janus-dev bash -lc "sh autogen.sh && ./configure --prefix=/opt/janus && make -j$(nproc) && make install DESTDIR=/out && make configs || true"

# Stage 2: runtime
FROM debian:bookworm-slim AS runtime
ENV DEBIAN_FRONTEND=noninteractive \
    JANUS_PREFIX=/opt/janus \
    LD_LIBRARY_PATH=/usr/lib:/usr/local/lib

RUN apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates libmicrohttpd12 libjansson4 libglib2.0-0 \
    libconfig9 libssl3 libogg0 libopus0 libsrtp2-1 libnice10 libcurl4 zlib1g libwebsockets17 \
    && rm -rf /var/lib/apt/lists/*

# libnice and libsrtp runtime libs from dev image (built from source)
## No need to copy shared libs from dev: installed via apt in runtime

# Expect Janus installation to be bind-mounted at runtime (from /out/opt/janus)
# Example:
#  docker run --rm -it \
#    -v $PWD/out/opt/janus:/opt/janus \
#    -p 8088:8088 -p 8188:8188 -p 8089:8089 -p 7088:7088 -p 7089:7089 \
#    -p 8000-9000:8000-9000/udp \
#    janus-gateway:runtime

EXPOSE 8088 8089 8188 7088 7089 8000-9000/udp

WORKDIR /opt/janus

# Entry point: allow overrides via env
CMD ["/opt/janus/bin/janus", "-F", "/opt/janus/etc/janus"]


