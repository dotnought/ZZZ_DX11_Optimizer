# Advanced capture and benchmark reference

For the first four recordings, use [Start Here.exe](capture.md). The commands and settings below are for custom captures and repeat benchmarking.

## Start with a vanilla comparison

Extract the bundle to a writable folder. Keep ZZZ in DX11 for every run, using the same window mode and graphics preset. At 3440×1440, record the internal render resolution or scale, crowd setting, shadows, reflections, frame cap, VSync, game and driver versions. Disable frame generation and external frame caps for the uncapped runs. Label a separate fixed-60-FPS series for frame pacing and a separate reduced-render-resolution series for bottleneck classification.

Use a short repeatable route in each area. Choose a recognizable starting point, camera direction, traversal pace, and end point. Warm each route once before recording warm-cache runs. Record a consistent time of day and crowd state where practical. Capture area changes or first-load behavior in a separate cold-cache series.

Start the official game for vanilla and use XXMI for all other conditions. Open PowerShell in the bundle directory when the game has loaded the route. Set your actual settings ID, in-game version, driver and route name in the example.

```powershell
.\tools\Capture-PresentMon.ps1 -Scene Roscaelifer -Condition vanilla -SettingsId '3440x1440-dx11-uncapped-high' -GameVersion '<game version>' -Driver '<driver version>' -Route 'cafe-loop-v1'
```

Return to the game as soon as the tool tells you to switch. PresentMon starts automatically after eight seconds. Follow the route for 45 seconds. After the capture, the script prints the new run folder. Repeat with `-Scene SixthStreet` and its own route name. For a static-camera test, use different route labels from the corresponding traversal runs. If Windows denies PresentMon access to ETW, use a PowerShell session with the appropriate ETW permissions. [PresentMon CLI documentation](https://github.com/GameTechDev/PresentMon/blob/v2.6.0/README-ConsoleApplication.md)

## Install the native capture DLL

1. Confirm the launcher uses **XXMI Libraries v1.3.3**, the stable build this profiler is based on. Close the game and launcher.
2. From the bundle directory, replace the example path with the ZZMI importer directory containing `d3dx.ini`.

   ```powershell
   .\tools\Install-Profiler.ps1 -ImporterPath 'D:\XXMI Launcher\ZZMI'
   ```

3. Use the launcher's built-in **Allow 3rd-party 3DMigoto DLLs** setting for the ZZMI importer. The launcher labels this **Unsafe Mode**. It explicitly permits a custom DLL. If you prefer to keep signed-library enforcement, use the PresentMon-only baselines.
4. Check `zzz-profiler.ini` alongside the installed DLL. Set `Level=2` and `Seconds=45` for these captures. The DLL reads the file when the game first presents, so restart after a change. Keep the upstream hunting and profiling overlays off during benchmarking. Keep a copy of your graphics settings and mod list under the run directory.

The installer backs up the exact stock DLL and verifies its SHA-256. The native build is an unsigned experimental Windows x64 build. The launcher can replace it during a library update. If the stock version or digest differs, collect the external baselines for that version and report the installed XXMI version before using the native build. [XXMI v1.3.3 release](https://github.com/SpectrumQT/XXMI-Libs-Package/releases/tag/v1.3.3)

## Record one combined run

```powershell
.\tools\Capture-PresentMon.ps1 -Scene Roscaelifer -Condition profiler-l2 -SettingsId '3440x1440-dx11-uncapped-high' -GameVersion '<game version>' -Driver '<driver version>' -Route 'cafe-loop-v1' -ImporterPath 'D:\XXMI Launcher\ZZMI'
```

Switch back to the game when prompted. PresentMon starts automatically and the script triggers the native capture when the first PresentMon frames arrive. Allow both timed captures to finish while the game remains open. The native reporter can use up to four additional seconds to retrieve delayed queries. A completed run contains `run.json`, PresentMon CSV, native metadata JSON, and four native CSVs. The script finds the native capture from the same PID and level and copies it from `zzz-captures` beside the DLL. For a native-only run, press **Ctrl+Alt+F8** in the game without running the script. Press it again to stop early.

For the first analysis, capture vanilla and Level 2 in both areas. In the same session, capture the remaining conditions where time allows. Aim for three runs per condition and scene in the full matrix.

| Condition label | Game launch and configuration |
|---|---|
| `vanilla` | Official game launch with no XXMI and no mods |
| `xxmi-empty` | Stock ZZMI with its core files and no user mods |
| `xxmi-mods` | Stock ZZMI with a recorded representative set of user mods |
| `profiler-off` | Capture DLL and `Level=0`, with the same mod set as `xxmi-mods` |
| `profiler-l1` | Capture DLL with `Level=1`, with the same mod set |
| `profiler-l2` | Capture DLL with `Level=2`, with the same mod set |

For each scene, keep the settings ID and route label identical across conditions in a comparison. A reduced-resolution or capped run needs a different settings ID. Lower-resolution PresentMon baselines are the quickest check for a GPU throughput limit. If the native API and GPU data leave severe stalls unexplained, collect one Windows Performance Recorder trace with CPU Usage and GPU Activity over the same route for WPA correlation. The native rows include QPC and PID. [WPR documentation](https://learn.microsoft.com/windows-hardware/test/wpt/windows-performance-recorder)

## Capture configuration

| Setting | Behavior |
|---|---|
| `Level=0` or absent file | No active profiling |
| `Level=1` | Frame intervals, driver Present duration, API and workload counts, and DXGI memory budgets |
| `Level=2` | Level 1 plus sampled API timing, all selected stall timings, XXMI timings, and GPU query samples |
| `Level=3` | Up to 20 seconds of finer GPU segmentation using observed XXMI pixel/compute shader hashes |
| `Seconds` | Capture length, up to 120 seconds at Level 1/2 or 20 seconds at Level 3 |
| `GpuEveryNFrames` | GPU frame sample interval, default 4. Use the same value across comparable captures |
| `CpuSampleEvery` | Sampling denominator for frequent operations without a dedicated timer, default 16 |
| `StallMicroseconds` | Threshold for tagging a timed stall candidate, default 1000 µs |
| `ShaderHash` | Optional hexadecimal Level 3 GPU filter. Zero measures observed workloads broadly |

If the hotkey is bound elsewhere, close that tool for the capture. If you get only a PresentMon file, confirm the DLL was retained by the launcher, the config is beside that DLL, the game was restarted with the matching mode, and the display window stayed in the foreground. When the game does not launch or graphics behave differently, close both apps, restore stock, and record the error and mod set.

## Roll back

Close ZZZ and XXMI and run the installer with `-Restore` and the same `-ImporterPath`. Return the launcher's custom DLL option to its previous setting. You can remove `zzz-profiler.ini` and keep the capture directory for analysis. If the backup is missing or another app replaced the DLL, use XXMI's package repair instead of overwriting an unknown DLL.

## Reading the report

Average FPS is 1000 divided by mean frame interval in milliseconds. The 1% and 0.1% lows use 1000 divided by the mean of the slowest 1% and 0.1% of intervals, respectively, rounded up to at least one frame. Frame-time percentiles use nearest rank. Cross-condition numbers take the median of run-level measurements for the same scene, route, settings ID, versions, cache state, and PresentMon interval metric. Treat rates from a single run as preliminary.

PresentMon selection uses the most active DXGI swap chain for the recorded process. If the app showed multiple views or the chosen chain looks wrong, specify its expected address in the optional `swap_chain` field of that run's JSON.
