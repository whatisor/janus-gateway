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

# Ensure pkg-config can find libs installed to common prefixes
ENV PKG_CONFIG_PATH="/usr/lib/pkgconfig:/usr/local/lib/pkgconfig:/usr/lib/x86_64-linux-gnu/pkgconfig:${PKG_CONFIG_PATH}"

# Build and install libnice (recommended newer version)
RUN git clone --branch 0.1.22 --depth 1  https://gitlab.freedesktop.org/libnice/libnice /tmp/libnice \
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

# Build and install RNNoise (for HAVE_RNNOISE / post-processing)
RUN git clone https://github.com/xiph/rnnoise.git /tmp/rnnoise \
    && cd /tmp/rnnoise \
    && git checkout 70f1d256acd4b34a572f999a05c87bf00b67730d \
    && ./autogen.sh \
    && ./configure --prefix=/usr \
    && make -j"$(nproc)" \
    && make install \
    && ldconfig \
    && rm -rf /tmp/rnnoise

WORKDIR /work

# Usage (example):
#  docker build -t janus-dev --target dev .
#  docker run --rm -it \
#    -v $PWD:/work \
#    -v $PWD/out:/out \
#    janus-dev bash -lc "sh autogen.sh && ./configure --prefix=/opt/janus --enable-post-processing && make -j$(nproc) && make install DESTDIR=/out && make configs || true"

# Stage 2: runtime
FROM debian:bookworm-slim AS runtime
ENV DEBIAN_FRONTEND=noninteractive \
    JANUS_PREFIX=/opt/janus \
    LD_LIBRARY_PATH=/usr/lib:/usr/local/lib

RUN apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates libmicrohttpd12 libjansson4 libglib2.0-0 \
    libconfig9 libssl3 libogg0 libopus0 libsrtp2-1 libnice10 libcurl4 zlib1g libwebsockets17 \
    && rm -rf /var/lib/apt/lists/*

# Bring RNNoise shared library from dev stage (built from source)
# Try both common prefixes to be safe
COPY --from=dev /usr/lib/librnnoise.so* /usr/lib/
COPY --from=dev /usr/local/lib/librnnoise.so* /usr/local/lib/

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


