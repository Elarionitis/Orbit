#!/usr/bin/env python3
"""Runs the benchmark suite behind docs/BENCHMARKS.md and prints a Markdown table.

Each scenario starts a fresh server, so one run cannot warm or pollute the next.
Pass the build directory holding the release binaries, e.g.

    scripts/run_benchmarks.py build/release --threads 4 --bench-threads 3
"""
import argparse
import json
import os
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import time

# name, server flags, generator flags. Rates are chosen to sit under saturation, so
# that the latency columns describe a healthy system rather than a queue.
SCENARIOS = [
    ("Max throughput, 100 clients, 10 rooms", [],
     ["--clients", "100", "--rooms", "10", "--rate", "0"]),
    ("Max throughput, 1,000 clients, 100 rooms", [],
     ["--clients", "1000", "--rooms", "100", "--rate", "0", "--window", "4"]),
    ("Paced 10k msg/s, 1,000 clients, 50 rooms", [],
     ["--clients", "1000", "--rooms", "50", "--rate", "10000"]),
    ("Paced 2k msg/s, 5,000 clients, 100 rooms", [],
     ["--clients", "5000", "--rooms", "100", "--publishers", "500", "--rate", "2000"]),
    ("Paced 20k msg/s, WAL fsync=interval", ["DATA", "--fsync", "interval"],
     ["--clients", "100", "--rooms", "10", "--rate", "20000"]),
    ("Paced 5k msg/s, WAL fsync=always", ["DATA", "--fsync", "always"],
     ["--clients", "100", "--rooms", "10", "--rate", "5000"]),
]


def free_port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def wait_for_port(port, timeout=10):
    deadline = time.time() + timeout
    while time.time() < deadline:
        with socket.socket() as sock:
            if sock.connect_ex(("127.0.0.1", port)) == 0:
                return
        time.sleep(0.05)
    raise RuntimeError("server did not start")


def run(build_dir, name, server_flags, bench_flags, args):
    port = free_port()
    data_dir = tempfile.mkdtemp(prefix="orbit-bench-", dir=args.data_root)
    server_flags = [data_flag for flag in server_flags
                    for data_flag in (["--data-dir", data_dir] if flag == "DATA" else [flag])]
    server = subprocess.Popen(
        [os.path.join(build_dir, "src/server/orbit_server"), "--port", str(port),
         "--threads", str(args.threads), "--rate", "0", "--max-queued-bytes", "67108864",
         "--log-level", "warn", *server_flags])
    try:
        wait_for_port(port)
        result = subprocess.run(
            [os.path.join(build_dir, "src/bench/orbit_bench"), "--port", str(port),
             "--threads", str(args.bench_threads), "--duration", str(args.duration),
             "--warmup", "2", "--json", *bench_flags],
            capture_output=True, text=True, timeout=args.duration + 120)
        if result.stderr:
            print(f"# {name}: {result.stderr.strip()}", file=sys.stderr)
        return json.loads(result.stdout.strip().splitlines()[-1])
    finally:
        server.send_signal(signal.SIGTERM)
        server.wait(timeout=20)
        shutil.rmtree(data_dir, ignore_errors=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("build_dir")
    parser.add_argument("--threads", type=int, default=4, help="server threads")
    parser.add_argument("--bench-threads", type=int, default=3, help="generator threads")
    parser.add_argument("--duration", type=int, default=10, help="measured seconds per scenario")
    parser.add_argument("--data-root", default=None, help="where WAL scenarios write")
    args = parser.parse_args()

    print("| Scenario | Published/s | Delivered/s | p50 | p99 | p99.9 | Lost |")
    print("|---|---:|---:|---:|---:|---:|---:|")
    for name, server_flags, bench_flags in SCENARIOS:
        r = run(args.build_dir, name, server_flags, bench_flags, args)
        print(f"| {name} | {r['published_per_s']:,.0f} | {r['delivered_per_s']:,.0f} "
              f"| {r['p50_ms']:.2f} ms | {r['p99_ms']:.2f} ms | {r['p999_ms']:.2f} ms "
              f"| {r['lost'] + r['sequence_gaps']} |", flush=True)


if __name__ == "__main__":
    main()
