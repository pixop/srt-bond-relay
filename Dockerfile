FROM ubuntu:24.04 AS build

ARG DEBIAN_FRONTEND=noninteractive
ARG SRT_TAG=v1.5.5

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    ca-certificates \
    cmake \
    git \
    pkg-config \
    libssl-dev \
    zlib1g-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src

RUN git clone --depth 1 --branch "${SRT_TAG}" https://github.com/Haivision/srt.git

RUN cmake -S /src/srt -B /src/srt/build \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX=/opt/pixop-srt \
    -DENABLE_APPS=OFF \
    -DENABLE_SHARED=ON \
    -DENABLE_STATIC=OFF \
    -DENABLE_ENCRYPTION=ON \
    -DUSE_OPENSSL_PC=ON \
    -DENABLE_BONDING=ON \
    && cmake --build /src/srt/build --parallel \
    && cmake --install /src/srt/build

COPY . /src/srt-bond-relay

ENV PKG_CONFIG_PATH=/opt/pixop-srt/lib/pkgconfig:/opt/pixop-srt/lib64/pkgconfig

RUN cmake -S /src/srt-bond-relay -B /src/srt-bond-relay/build \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_PREFIX_PATH=/opt/pixop-srt \
    -DCMAKE_INSTALL_PREFIX=/usr/local \
    && cmake --build /src/srt-bond-relay/build --parallel \
    && cmake --install /src/srt-bond-relay/build

FROM ubuntu:24.04 AS runtime

ARG DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates \
    libssl3 \
    zlib1g \
    && rm -rf /var/lib/apt/lists/*

COPY --from=build /opt/pixop-srt /opt/pixop-srt
COPY --from=build /usr/local/bin/srt-bond-relay /usr/local/bin/srt-bond-relay

ENV LD_LIBRARY_PATH=/opt/pixop-srt/lib

ENTRYPOINT ["srt-bond-relay"]
