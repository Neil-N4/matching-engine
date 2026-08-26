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

Record call graphs for the benchmark suite:

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

The standalone benchmark prints one line per scenario:

- `hot_fifo_fill`: preloads one ask FIFO level and repeatedly submits market buys for one share. This isolates order-to-fill pointer mutation.
- `add_cancel_churn`: repeatedly adds and cancels unique order IDs across rotating prices. This stresses fixed hash-table deletion and best-level maintenance.
- `replace_churn`: repeatedly adds, replaces, and cancels unique order IDs across rotating prices. This stresses cancel-replace mutation, order-ID rewrites, and price-level migration without allocating a second order node.
- `delete_churn`: repeatedly adds and fully deletes unique order IDs across rotating prices. This isolates whole-order removal and slab-slot return.
- `time_in_force_ioc_fok`: adds a resting order, verifies FOK expiry leaves it untouched, then removes it through an IOC partial fill.
- `itch_add_decode`: parses NASDAQ ITCH add-order frames from a fixed message ring.
- `itch_replace_decode`: parses NASDAQ ITCH order-replace frames from a fixed message ring.
- `itch_delete_decode`: parses NASDAQ ITCH order-delete frames from a fixed message ring.
- `alpha_signal_ofi_vol_quote_risk`: computes VPIN/OBI/micro-price/OFI/volatility, an Avellaneda-Stoikov quote, position/PnL state, and a risk-gate decision.

Fast parser and alpha scenarios are batched internally before reporting per-op median and p99, which avoids timer granularity showing false `0ns` medians.
