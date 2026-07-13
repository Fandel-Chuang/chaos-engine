FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update -qq && apt-get install -y -qq \
    cmake build-essential \
    liblua5.4-dev liburing-dev libbpf-dev \
    clang llvm \
    flatbuffers-compiler \
    libvulkan-dev libx11-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app
COPY . /app

RUN mkdir -p build && cd build \
    && cmake .. -DCMAKE_BUILD_TYPE=Release \
    && make -j$(nproc) chaos_server

EXPOSE 7777

CMD ["./build/bin/chaos_server"]
