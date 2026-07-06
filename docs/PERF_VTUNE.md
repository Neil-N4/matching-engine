# Perf and VTune Runbook

## Build For Profiling

Use a release build with symbols and frame pointers when collecting profiles:

```sh
cmake -S . -B build/profile \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DMATCHING_ENGINE_NATIVE=ON
cmake --build build/profile -j
```

For portable CI-style builds, disable native code generation:

```sh
cmake --preset release-portable
cmake --build --preset release-portable
```

## Linux perf

Record call graphs for the synthetic order-to-fill benchmark:

```sh
perf stat -d ./build/profile/engine_bench
perf record -F 999 -g -- ./build/profile/engine_bench
perf report
```

Useful counters for the matching hot path:

```sh
perf stat -e cycles,instructions,branches,branch-misses,cache-references,cache-misses \
  ./build/profile/engine_bench
```

## Intel VTune

Collect hotspots:

```sh
vtune -collect hotspots -result-dir vtune-hotspots -- ./build/profile/engine_bench
vtune -report summary -result-dir vtune-hotspots
```

Collect microarchitecture exploration:

```sh
vtune -collect uarch-exploration -result-dir vtune-uarch -- ./build/profile/engine_bench
vtune -report summary -result-dir vtune-uarch
```

## Interpreting Results

The default benchmark preloads one ask FIFO level and repeatedly submits market buys for one share. That isolates order-to-fill pointer mutation and avoids making every sample pay for best-price rescans. A separate scenario should be added when evaluating worst-case price-level churn.

