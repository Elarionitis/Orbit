# Nexus

Multi-threaded TCP Communication Server written in C++17.

## Features (Planned)
- Multi-threaded connection handling
- High-performance TCP message dispatching
- Production-ready, clean C++17 modular architecture

## Prerequisites
- Linux / macOS (POSIX APIs)
- C++17 compatible compiler (GCC, Clang)
- CMake 3.10+

## Building the Project
To compile both the server and client executables:
```bash
mkdir build
cd build
cmake ..
make
```

## Running the Executables
Once compiled, you can run the server and client executables:
- Server: `./build/src/server/nexus_server`
- Client: `./build/src/client/nexus_client`
