# Orbit

## Overview

Orbit is a small, local multi-client TCP chat application written in C++17. It consists of a server that accepts connections on `127.0.0.1` and a terminal client for joining and exchanging timestamped messages.

## Features

- Concurrent handling of connected clients
- Username-based chat messages with server-side timestamps
- Broadcasts to all connected clients except the sender
- Configurable server port (default: `8080`)
- Graceful client disconnect and server shutdown handling

## Project Structure

```text
.
├── CMakeLists.txt          # Top-level build configuration
├── src
│   ├── client              # Terminal client and its CMake target
│   ├── common              # Shared constants and timestamp utility
│   └── server              # TCP server and its CMake target
└── README.md
```

## Build Instructions

Prerequisites: CMake 3.10 or later, a C++17 compiler, and a POSIX-compatible system (macOS or Linux).

```bash
cmake -S . -B build
cmake --build build
```

The executables are created at `build/src/server/orbit_server` and `build/src/client/orbit_client`.

## Usage

Start the server in one terminal. The optional port must be between `1` and `65535`.

```bash
./build/src/server/orbit_server [port]
```

Then start one or more clients in separate terminals:

```bash
./build/src/client/orbit_client
```

Enter a username when prompted, then type messages at the `>` prompt. Type `/quit` or close the input stream to leave the chat. The client connects to `127.0.0.1:8080`; use the default server port when running it.

## Current Architecture

The server owns a listening socket and runs its accept loop on a dedicated thread. Each accepted connection is handled by a worker thread. The first message from a client is used as its username; later messages are timestamped and broadcast to the other connected clients. A mutex protects the connected-socket list, and shared utilities provide the default port, buffer size, and thread-safe timestamp formatting.

The terminal client runs a receive loop in a background thread while the main thread reads user input and sends messages.

## Future Improvements

- Add a defined message framing protocol for messages larger than the receive buffer.
- Allow the client address and port to be configured at launch.
- Add automated tests for connection lifecycle and broadcast behavior.
- Add structured logging and configurable server settings.
