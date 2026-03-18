# Build environment with z88dk and ZEsarUX
FROM ubuntu:22.04

ENV DEBIAN_FRONTEND=noninteractive
ENV ZESARUX_TAG=ZEsarUX-12.1

# Install dependencies for build + runtime + harness.
RUN apt-get update && apt-get install -y \
    build-essential \
    git \
    wget \
    curl \
    golang-go \
    python3 \
    python3-pip \
    perl \
    bison \
    flex \
    unzip \
    libncurses-dev \
    libssl-dev \
    libgmp-dev \
    libxml2-dev \
    xorg-dev \
    libpulse-dev \
    libsndfile1-dev \
    libasound2-dev \
    cmake \
    ca-certificates \
    && rm -rf /var/lib/apt/lists/*

# Install z88dk from official source archive (works across amd64/arm64).
RUN cd /tmp && \
    wget -q https://github.com/z88dk/z88dk/releases/download/v2.3/z88dk-src-2.3.tgz -O z88dk-src.tgz && \
    tar -xzf z88dk-src.tgz && \
    cd z88dk && \
    chmod +x build.sh && \
    ./build.sh && \
    make install PREFIX=/opt/z88dk && \
    test -x /opt/z88dk/bin/zcc && \
    cd /tmp && rm -rf z88dk-src.tgz z88dk

ENV PATH=/opt/z88dk/bin:/usr/local/bin:$PATH
ENV ZCCCFG=/opt/z88dk/share/z88dk/lib/config

# Build ZEsarUX from official GitHub source tag.
RUN cd /tmp && \
    wget -q https://github.com/chernandezba/zesarux/archive/refs/tags/${ZESARUX_TAG}.tar.gz -O zesarux.tar.gz && \
    tar -xzf zesarux.tar.gz && \
    cd zesarux-${ZESARUX_TAG}/src && \
    ./configure --enable-ssl --disable-caca --disable-aa --disable-cursesw --prefix /usr/local && \
    make clean && \
    make -j"$(nproc)" && \
    make install && \
    cd /tmp && rm -rf zesarux.tar.gz zesarux-${ZESARUX_TAG}

WORKDIR /workspace

# Verify tooling in the final image.
RUN /opt/z88dk/bin/zcc 2>&1 | grep -q "z88dk" && \
    command -v zcc > /dev/null && \
    zesarux --version > /dev/null && \
    echo "Toolchain verified"

CMD ["/bin/bash"]
