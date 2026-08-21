# Testing

## Local Default

The default test path has no external dependencies:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

This runs `matching_engine_tests`, a small assertion-based harness covering:

- slab-pool exhaustion and reuse
- SPSC full/empty behavior
- ITCH add/execute parser offsets
- L3 add/cancel/execute/match state transitions
- duplicate order rejection before matching
- OBI, micro-price, VPIN bucket accounting
- OFI and OFI-EMA queue-pressure accounting
- micro-price log-return, realized volatility, EWMA volatility, and quote spread widening from signal volatility
- atomic feature snapshots
- VPIN-adaptive quote widening
- long, short, and crossing position/PnL accounting
- risk-gate quote sizing, inventory limits, toxicity pause, drawdown kill switch, and notional limits

## GoogleTest

If GTest is installed, CMake also builds `matching_engine_gtests` and discovers each test case through CTest:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DMATCHING_ENGINE_REQUIRE_GTEST=ON
cmake --build build -j
ctest --test-dir build --output-on-failure
```

CI installs `libgtest-dev` and uses `MATCHING_ENGINE_REQUIRE_GTEST=ON`, so GTest coverage is mandatory there.

## Benchmark Smoke

Run:

```sh
./build/engine_bench
```

The standalone benchmark reports scenario-level median, p99, and throughput. It is a smoke/perf regression tool, not a substitute for hardware-pinned `perf` or VTune profiling.
