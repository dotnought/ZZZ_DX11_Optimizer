# ZZZ DX11 Optimizer

Windows x64 DX11 capture tools for comparing Roscaelifer with Sixth Street and combat. The capture DLL instruments the existing XXMI wrapper. PresentMon provides the same external timing source in vanilla and ZZMI runs. Rendering optimizations begin once comparative captures identify an accessible bottleneck.

## First capture

[Download the Windows test package](https://github.com/dotnought/ZZZ_DX11_Optimizer/tree/profiling-milestone-1-build), extract the ZIP, and double-click **Start Here.exe**. The menu records Roscaelifer and Sixth Street with and without XXMI, guides the DLL swap, and restores stock afterward. See [the first-capture guide](docs/capture.md).

Send the generated `runs` folder for analysis. For local reports or the full benchmark matrix, see the [advanced reference](docs/advanced-capture.md).

The capture package uses XXMI's per-slot vertex-buffer checks. The batching experiment remains available in Git history. [Experimental source](https://github.com/dotnought/ZZZ_DX11_Optimizer/commit/3c8efc853fa794e45e1b80bdb8b6ba2e3a054ec6)

## Source and builds

The native module lives in `native/`. The patch in `patches/` applies to the pinned XXMI v1.3.3 source. `tools/` contains the Windows capture, install, build, and analysis tools. See [build instructions](docs/build.md) and [technical findings](docs/findings.md).

The capture DLL derives from GPL-3.0 XXMI/3DMigoto. This repository includes the profiling source, the source patch, build recipes, and license notices. PresentMon is distributed under its separate MIT license. See [third-party sources](docs/build.md#third-party-sources).
