# ─── Stage 1: Build ───────────────────────────────────────────────────────────
FROM nvcr.io/nvidia/l4t-jetpack:r36.4.0 AS builder

ENV DEBIAN_FRONTEND=noninteractive

# Install build dependencies
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    cmake \
    git \
    pkg-config \
    libgstreamer1.0-dev \
    libgstreamer-plugins-base1.0-dev \
    libgstreamer-plugins-good1.0-dev \
    gstreamer1.0-plugins-base \
    gstreamer1.0-plugins-good \
    gstreamer1.0-plugins-bad \
    gstreamer1.0-plugins-ugly \
    gstreamer1.0-tools \
    libssl-dev \
    ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /build
COPY . .

RUN mkdir -p build && cd build && \
    cmake .. \
        -DCMAKE_BUILD_TYPE=Release \
        -DENABLE_JETSON=ON \
        -DENABLE_TEST_MODE=ON && \
    make -j$(nproc)

# ─── Stage 2: Runtime ─────────────────────────────────────────────────────────
FROM nvcr.io/nvidia/l4t-jetpack:r36.4.0

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
    gstreamer1.0-plugins-base \
    gstreamer1.0-plugins-good \
    gstreamer1.0-plugins-bad \
    gstreamer1.0-plugins-ugly \
    gstreamer1.0-tools \
    gstreamer1.0-rtsp \
    libssl3 \
    ca-certificates \
    nvidia-l4t-gstreamer \
    && rm -rf /var/lib/apt/lists/*

# Create app user
RUN useradd -r -s /bin/false streamserver

WORKDIR /app

# Copy built binary
COPY --from=builder /build/build/stream-server /usr/local/bin/stream-server

# Copy config and web files
COPY config.yaml /etc/stream-server/config.yaml
COPY web/ /app/web/

# Expose signaling port
EXPOSE 8080

# UDP ports for WebRTC media (ephemeral range subset)
EXPOSE 49152-49252/udp

# Run as non-root
USER streamserver

# Health check
HEALTHCHECK --interval=30s --timeout=5s --retries=3 \
    CMD curl -sf http://localhost:8080/ || exit 1

ENTRYPOINT ["stream-server"]
CMD ["--config", "/etc/stream-server/config.yaml"]
