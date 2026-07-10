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

## Tests

The project always builds a dependency-free CTest harness. If GTest is installed, CMake also builds the `matching_engine_gtests` target.

```sh
ctest --test-dir build --output-on-failure
```

CI installs GTest and configures with `MATCHING_ENGINE_REQUIRE_GTEST=ON`, so the richer GoogleTest suite is required on GitHub Actions.

## Run

```sh
./build/matching_engine
./build/engine_bench
```

If Google Benchmark is installed, the benchmark target links against it. Otherwise it builds a standalone latency harness that reports median, p99, and throughput for hot FIFO fills, add/cancel churn, ITCH decode, and alpha quote generation.

More detail:

- [Architecture](docs/ARCHITECTURE.md)
- [Testing](docs/TESTING.md)
- [Perf and VTune](docs/PERF_VTUNE.md)
