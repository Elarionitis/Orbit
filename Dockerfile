# Build stage: compiles the server against the same libraries the runtime image has.
FROM ubuntu:24.04 AS build
RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        build-essential ca-certificates cmake libssl-dev ninja-build \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY CMakeLists.txt CMakePresets.json ./
COPY cmake cmake
COPY src src
RUN cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DORBIT_BUILD_TESTS=OFF \
    && cmake --build build --target orbit_server orbit_client orbit_bench

# Runtime stage: no compiler, and an unprivileged user.
FROM ubuntu:24.04
RUN apt-get update \
    && apt-get install -y --no-install-recommends libssl3t64 \
    && rm -rf /var/lib/apt/lists/* \
    && useradd --system --create-home --uid 10001 orbit \
    && mkdir /data && chown orbit /data
COPY --from=build /src/build/src/server/orbit_server /usr/local/bin/
COPY --from=build /src/build/src/client/orbit_client /usr/local/bin/
COPY --from=build /src/build/src/bench/orbit_bench /usr/local/bin/
USER orbit
VOLUME /data
EXPOSE 8080 9090
ENTRYPOINT ["orbit_server"]
CMD ["--host", "0.0.0.0", "--data-dir", "/data", "--metrics-port", "9090", "--metrics-host", "0.0.0.0"]
