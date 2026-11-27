# ZZZ DX11 Optimizer

Windows x64 DX11 capture tools for comparing Roscaelifer with Sixth Street and combat. The capture DLL instruments the existing XXMI wrapper. PresentMon provides the same external timing source in vanilla and ZZMI runs. Rendering optimizations begin once comparative captures identify an accessible bottleneck.

## First capture

1. Unpack the test bundle. Read [installation and capture](docs/capture.md) before switching libraries.
2. Benchmark both routes with the game started normally in DX11. Use the bundled PresentMon and the run script to label the runs.
3. Install the profiler in the **ZZMI importer** folder with the original XXMI Libraries v1.3.3 DLL present. Repeat both routes at Level 2. Press **Ctrl+Alt+F8** at the start of each run.
4. Analyze the resulting runs with Python 3.10+.

   ```powershell
   py -3 tools/analyze.py runs --output runs-summary
   ```

The report ranks observed GPU workload groups and sampled API costs, describes stalls on slow frames, and compares repeated runs with the same scene, route, settings, versions, and cache state. Share the run folders and report files for bottleneck analysis.

## Source and builds

The native module lives in `native/`. The patch in `patches/` applies to the pinned XXMI v1.3.3 source. `tools/` contains the Windows capture, install, build, and analysis tools. See [build instructions](docs/build.md) and [technical findings](docs/findings.md).

The capture DLL derives from GPL-3.0 XXMI/3DMigoto. This repository includes the profiling source, the source patch, build recipes, and license notices. PresentMon is distributed under its separate MIT license. See [third-party sources](docs/build.md#third-party-sources).
