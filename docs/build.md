# Building the capture package

## Windows with Visual Studio

Install Git, the Desktop development with C++ workload, the **MSVC v143** toolset and a Windows 10/11 SDK. Visual Studio 2026 can use the v143 toolset when the corresponding optional components are installed. Run from an x64 Developer PowerShell with MSBuild available.

```powershell
.\tools\Build-Profiler.ps1
```

The build script checks the exact commit identity, applies the profiling patch, copies the two native sources and runs the upstream x64 Zip Release configuration. The result is `dist\bin\d3d11.dll` with a SHA-256 file. To test a locally built DLL, pass its path as `-ProfilerDll` to the installer. Keep the stock ZZMI importer files. The local build may produce additional upstream binaries as part of the solution build.

For the baseline executable, the test package includes the official PresentMon v2.6.0 CLI. The source repository can download and verify it with `.\tools\Get-PresentMon.ps1`. Python 3.10+ is required for report analysis. No Python package dependencies are required.

## Third-party sources

- [XXMI Libraries v1.3.3](https://github.com/SpectrumQT/XXMI-Libs-Package/tree/8bd31f74727f033b3d9049509e19da3563c8b98c) and [original 3DMigoto](https://github.com/bo3b/3Dmigoto) are GPL-3.0. The pin, profiler sources and patches are the source recipe for the DLL. The XXMI pin carries the upstream authors and license notices.
- The XXMI pin includes DirectXTK code, PCRE2, CRC32C, and a Nektra hook library. See the licenses in the pinned tree and the [corresponding Deviare-InProc source](https://github.com/nektra/Deviare-InProc/tree/e2e589f77bedf9b7ca21efcbc1eb33fb9a270b07) for the latter. The exact Nektra revision is identified by the [upstream binary update](https://github.com/SpectrumQT/XXMI-Libs-Package/commit/70e11c560049a37a10c65c06968ddaeef744f2cc). Building from the pin uses its bundled PCRE2 and Nektra libraries.
- [PresentMon v2.6.0](https://github.com/GameTechDev/PresentMon/tree/v2.6.0) carries an MIT license included alongside its executable. The package preserves the official executable without changes.

## Linux cross-build used for the test DLL

The test DLL was built with LLVM 19.1.7 and the Microsoft CRT and Windows SDK 10.0.26100 obtained with xwin 0.10.0 (VS 17 release channel). The cross recipe uses the separate `clang-cross.patch` to address Windows file names, MSVC language extensions and static linking. It uses Clang's synchronous C++ exception model because the pinned upstream code triggers an optimizer fault with LLVM 19's asynchronous model. Use the Visual Studio recipe for upstream's standard build settings.

With LLVM's `clang`, `llvm-rc` and `lld-link` available and the desktop x64 SDK splatted into a user-owned directory, run the following from the repo root. The recipe creates its working checkout and adds case aliases to the supplied SDK directory.

```sh
python3 tools/build-cross.py --sdk /path/to/splat --llvm-bin /usr/lib/llvm-19/bin --build-dir build/m1-cross
```

The DLL is `build/m1-cross/out/d3d11.dll`. A PE inspection of the test DLL matched the exported names and ordinals of the stock v1.3.3 DLL. PE inspection verifies the binary format and export surface. Actual loader behavior, GPU queries and ZZZ compatibility require Windows testing. Run the offline report tests with `python -m unittest discover -s tests -v`.
