# PS5 payload SDK image for building ps5-freebsd-loader.
#
# Based on:
# - https://github.com/itsPLK/ps5-payload-manager/blob/main/Dockerfile.sdk
# - https://github.com/itsPLK/ps5-payload-manager/blob/main/build_deps.sh
FROM ubuntu:24.04

ARG SDK_REPOSITORY=https://github.com/ps5-payload-dev/sdk.git
ARG SDK_REF=master

ENV DEBIAN_FRONTEND=noninteractive

# Clang 18 and LLD 18 are required by the SDK. The native x86-64 tools are
# needed by the loader shellcode build.
RUN apt-get update && apt-get install -y --no-install-recommends \
    binutils \
    binutils-x86-64-linux-gnu \
    ca-certificates \
    clang-18 \
    cmake \
    gcc \
    gcc-x86-64-linux-gnu \
    git \
    libc6-dev-amd64-cross \
    lld-18 \
    llvm-18-dev \
    make \
    ninja-build \
    pkg-config \
    python3 \
    python3-pyelftools \
    sudo \
    unzip \
    wget \
    xxd \
    xz-utils \
    && rm -rf /var/lib/apt/lists/*

RUN ln -sf /usr/bin/clang-18 /usr/bin/clang && \
    ln -sf /usr/bin/clang++-18 /usr/bin/clang++ && \
    ln -sf /usr/bin/ld.lld-18 /usr/bin/ld.lld

ENV LLVM_CONFIG=/usr/bin/llvm-config-18

WORKDIR /opt

RUN git clone --depth 1 --branch "${SDK_REF}" "${SDK_REPOSITORY}" ps5-payload-sdk-source

WORKDIR /opt/ps5-payload-sdk-source
RUN make DESTDIR=/opt/ps5-payload-sdk install

WORKDIR /opt
COPY build_deps.sh /opt/build_deps.sh
RUN bash /opt/build_deps.sh

ENV PS5_PAYLOAD_SDK=/opt/ps5-payload-sdk
ENV PATH=/opt/ps5-payload-sdk/bin:${PATH}

WORKDIR /src
