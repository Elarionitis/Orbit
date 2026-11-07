# Benchmarks

All figures come from real runs of the tools in this repository, and the commands to
reproduce them are below. Treat the absolute numbers with care: server and load generator
share one machine and talk over loopback, so there is no real network in these figures,
and the two compete for CPU.

## Environment

| | Linux | macOS |
|---|---|---|
| Machine | 6 vCPU QEMU/HVF virtual machine, on the laptop below | Apple M5, 10 cores, 16 GB |
| OS | Ubuntu 24.04, kernel 6.8 | macOS 27.2 |
| Compiler | GCC 13.3, Release | Apple Clang 21, Release |
| Poller | epoll | kqueue |
| Server threads | 3 | 4 |
| Generator threads | 3 | 3 |

Messages carry a 128-byte body. Each scenario runs on a fresh server for 8 measured
seconds after a 2-second warm-up.

## Results

Linux:

| Scenario | Published/s | Delivered/s | p50 | p99 | p99.9 | Lost |
|---|---:|---:|---:|---:|---:|---:|
| Max throughput, 100 clients, 10 rooms | 686,481 | 6,864,812 | 2.16 ms | 5.64 ms | 8.65 ms | 0 |
| Max throughput, 1,000 clients, 100 rooms | 526,014 | 5,260,145 | 6.82 ms | 17.83 ms | 21.50 ms | 0 |
| Paced 10k msg/s, 1,000 clients, 50 rooms | 10,000 | 199,998 | 0.08 ms | 0.20 ms | 0.30 ms | 0 |
| Paced 2k msg/s, 5,000 clients, 100 rooms | 2,000 | 100,003 | 0.12 ms | 0.27 ms | 0.40 ms | 0 |
| Paced 20k msg/s, WAL `fsync=interval` | 20,000 | 199,998 | 0.07 ms | 0.82 ms | 1.80 ms | 0 |
| Paced 5k msg/s, WAL `fsync=always` | 5,000 | 50,000 | 0.59 ms | 1.90 ms | 6.82 ms | 0 |

macOS:

| Scenario | Published/s | Delivered/s | p50 | p99 | p99.9 | Lost |
|---|---:|---:|---:|---:|---:|---:|
| Max throughput, 100 clients, 10 rooms | 64,922 | 649,217 | 23.59 ms | 66.06 ms | 318.77 ms | 0 |
| Max throughput, 1,000 clients, 100 rooms | 62,425 | 624,249 | 28.84 ms | 771.75 ms | 1778.38 ms | 0 |
| Paced 10k msg/s, 1,000 clients, 50 rooms | 10,000 | 199,998 | 0.16 ms | 0.69 ms | 4.46 ms | 0 |
| Paced 2k msg/s, 5,000 clients, 100 rooms | 2,000 | 99,986 | 0.35 ms | 1.77 ms | 2.29 ms | 0 |
| Paced 20k msg/s, WAL `fsync=interval` | 20,000 | 200,001 | 0.13 ms | 5.37 ms | 8.65 ms | 0 |
| Paced 5k msg/s, WAL `fsync=always` | 5,000 | 49,998 | 4.98 ms | 6.55 ms | 9.18 ms | 0 |

A single busy room peaks at 173,596 messages/s in and 17.4M deliveries/s out on Linux, with
100 clients all in one room, in closed loop at p99 14.4 ms.

As a cross-check on the generator, the server's own Prometheus counters read 695,726
published/s and 6,954,787 delivered/s during a run in which the generator reported
714,186 and 7,141,862.

### Reading these numbers

- **Max-throughput latency is queueing, not speed.** Those runs keep a window of messages
  in flight by design, so by Little's law latency there just reflects the window. The paced
  rows show the latency of a healthy system.
- **Linux is about ten times faster than macOS at peak.** Profiling on macOS puts roughly
  70% of server time inside the kernel's `sendmsg`, which points at the loopback path rather
  than the server code. The poller differs too, and no Linux profile was taken to confirm it.
- **`fsync=always` on the VM flatters the disk.** The virtual disk may acknowledge syncs
  sooner than real hardware. The macOS figure of about 5 ms uses `F_FULLFSYNC` on a real
  SSD, and is the more honest one for durable writes.
- **Saturation.** With 100 clients in one room, 3 server threads handled 100k msg/s paced
  at p99 2.82 ms, and were past saturation at 130k msg/s, where p50 jumped to 121 ms.
- **Zero loss holds below saturation only.** At an offered 170k msg/s the same setup could
  not keep up, the output limit disconnected 32 of the 100 clients as slow consumers, and
  the generator counted 5,013,112 deliveries lost. That is the overload policy working as
  designed: queues stay bounded and clients must reconnect. These generator clients do not
  reconnect, so that count includes deliveries to clients that had been dropped.

## Compared with the original server

The first version of Orbit used a thread per client and raw unframed TCP. This compares it
with the current server on the Linux VM, with 100 clients in one room.

| | Original | Orbit 2 |
|---|---:|---:|
| p50 / p99 at 10k msg/s | 1.21 / 1.97 ms | 0.19 / 0.90 ms |
| p50 / p99 at 50k msg/s | 7.32 / 45.27 ms | 0.27 / 0.82 ms |
| p50 / p99 at 100k msg/s | 20.37 / 270.54 ms | 0.37 / 2.82 ms |
| Messages keeping their sender, at 10k / 50k / 100k msg/s | 100% / 34% / 17% | 100% |
| Deliveries per second with one stalled client, at 10k msg/s | **0** | **1,000,007** |
| Threads with 5,000 clients | One per client, by design | 4 |

**The original was not slow at raw throughput,** and it would be misleading to claim
otherwise. On this single-room workload it kept up to about 130k msg/s, comparable with
Orbit, because it could use all six cores with 100 threads, and because it was taking a
shortcut. With no framing, one `recv` merges several messages into a single broadcast. That
is free batching, but it loses message boundaries, which is why most messages lose their
sender at high load.

The differences that matter are correctness, tail latency, and isolation. A single client
that stops reading fills its socket buffer, `send` blocks while holding the global mutex,
and delivery to everybody stops. Orbit disconnects that one client, and the rest are
unaffected: p99 was 0.56 ms during that run.

Notes on the method:

- Orbit's thread count was capped at 3, while the original ran as many threads as clients.
- Orbit's 6-thread results were worse here (p99 7.21 ms at 100k msg/s), as it then competes
  with the generator for six vCPUs.
- The two sides use different generators. For the original, the harness samples latency and
  skips lines cut by read boundaries, so its delivered counts read 3 to 11 percent low.
- The stalled client is not identical either: `legacy_load` leaves a socket unread, and the
  Orbit run used a short throwaway script that joined the room and never read.
- With 5,000 clients the original reached 3,150 threads nine seconds in. A second attempt
  never reached 5,000 threads, and the process was gone before the run ended. The cause was
  not investigated, so no figure is claimed for it at that scale.

## Durability

`CrashRecovery.EveryAcknowledgedMessageSurvivesSigkill` runs a real server process with
`fsync=always`, publishes at full speed, and sends `SIGKILL` at a random moment, five times
over. It then reads the log directly and checks that every acknowledged message is present
and unaltered. Recent sanitizer runs on Linux verified about 9,000 messages per test with
none lost. No test covers the other policies, which can lose a crash's queued tail, and
`SIGKILL` does not model power loss, so that guarantee rests on `fsync` being honest.

## Reproducing

```bash
cmake --preset release && cmake --build --preset release

# The main tables
scripts/run_benchmarks.py build/release --threads 3 --bench-threads 3 --duration 8

# The comparison: build commit f89a994 separately, then for each rate
c++ -std=c++20 -O2 -pthread scripts/legacy_load.cpp -o legacy_load
./legacy_load 8080 100 6 50000            # add a final 1 for a stalled client
./build/release/src/bench/orbit_bench --clients 100 --rooms 1 --rate 50000 --threads 3
```

On macOS, connection bursts are limited by `kern.ipc.somaxconn`, which defaults to 128.
