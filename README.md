# L3 Matching Engine and Alpha Pipeline

C++20 implementation of a Level 3 limit order book, NASDAQ ITCH 5.0 parser, lock-free telemetry path, online VPIN/imbalance/micro-price signals, and an adaptive Avellaneda-Stoikov market-making simulator.

The hot path avoids heap allocation by using fixed-capacity, cache-line-aligned data structures:

- `FixedSlabPool<T, N>` owns order nodes in pre-linked 64-byte chunks.
- `LockFreeSPSC<T, N>` moves engine events to the alpha thread using atomic head/tail sequences.
- The L3 book uses intrusive FIFO queues per price level plus fixed open-addressed tables for order and price lookup.

## Build

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

The default compiler flags include `-O3`, `-march=native`, `-fno-exceptions`, and `-fno-rtti` on GCC/Clang-compatible compilers.

## Run

```sh
./build/matching_engine
./build/engine_bench
```

If Google Benchmark is installed, the benchmark target links against it. Otherwise it builds a standalone latency harness with median and p99 reporting.

