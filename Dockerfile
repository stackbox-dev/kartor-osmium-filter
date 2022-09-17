FROM ubuntu:20.04 as builder
WORKDIR /filter
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y osmium-tool libosmium2-dev g++ cmake libboost-all-dev
COPY . .
WORKDIR /filter/build
RUN cmake -DCMAKE_BUILD_TYPE=Release ..
ARG CONCURRENCY
RUN make -j${CONCURRENCY:-$(nproc)}

FROM ubuntu:20.04
COPY --from=builder /filter/build/src/osmium-filter /usr/local/bin/
RUN export DEBIAN_FRONTEND=noninteractive && apt update && \
    apt install -y osmium-tool
