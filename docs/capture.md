# Windows capture guide

## Test the vertex-buffer optimization

If you already have a stock XXMI baseline, use **3** to install the new DLL, then launch ZZZ through XXMI and return to the same Roscaelifer spot, camera angle, and graphics settings. Keep the built-in costume checks enabled and personal mods off to match that baseline. Choose **7**, switch back to ZZZ, and wait for the finish sound or result banner. Send the new `runs` folder as a ZIP. Choice **6** restores stock after you close the game and launcher.

Choice **7** records one 45-second PresentMon capture tagged `vertex-batch`. The menu checks that the live game process has executed a batched check before recording. Consecutive, statically indexed vertex-buffer checks can share a binding query. After the first match, each remaining check queries its slot again so it sees any binding changes made by that match. Region hashing, deferred contexts, frame analysis, debug logging, and XXMI's built-in profiling use the per-slot path.

To use the per-slot path for choices **4** and **5**, set `BatchVertexChecks=0` under `[Optimization]` in the installed `zzz-profiler.ini` and restart ZZZ. The test DLL enables batching by default.

## First Windows capture

Extract the entire [Windows test ZIP](https://github.com/dotnought/ZZZ_DX11_Optimizer/tree/profiling-milestone-1-build) and double-click **Start Here.exe**. Keep the menu window open for all four runs with the game set to **DirectX 11**. Keep your resolution and graphics settings fixed. In each area, pick a spot and point the camera the same way for both recordings.

| Menu choice | What to do |
|---|---|
| **1** | Start ZZZ through its normal launcher with XXMI closed. Go to your Roscaelifer spot and record. |
| **2** | Go to your Sixth Street spot and record. |
| **3** | Close ZZZ and XXMI. Select your ZZMI folder in the picker to back up stock v1.3.3 and install the profiler. |
| **4** | Start ZZZ through XXMI. Return to the same Roscaelifer spot and record. |
| **5** | Return to the same Sixth Street spot and record. |
| **6** | Close ZZZ and XXMI. Restore the backed-up stock DLL. |
| **7** | Test vertex-buffer batching at the same Roscaelifer spot as the stock XXMI baseline. |

**To record:** Choose the number and press Enter. Switch straight back to ZZZ. The 45-second recording starts automatically once ZZZ has focus. Stay in the game until the finish sound or **ZZZ capture saved** banner. The banner disappears on its own. Switch back to the same menu window and pick the next option. If the run fails, the banner directs you to the error in the menu.

**To find the ZZMI folder for choice 3:** In XXMI Launcher, select **ZZMI** and click **Open Mods Folder**. Go up one folder from **Mods**. The folder you need contains both **d3dx.ini** and **d3d11.dll**. Note where it is, then close ZZZ and XXMI Launcher, including its tray icon. Choose **3**, select that folder, and confirm the swap.

**To launch for choices 4 and 5:** Disable your personal mods and leave the built-in ZZMI Core files alone. In XXMI Launcher with ZZMI selected, open **Settings → Advanced → Unsafe Mode**. This is the launcher's built-in option to allow third-party DLLs. If you prefer to leave it off, the first two recordings are still useful. After choice **6**, return it to its previous setting. [Launcher setting source](https://github.com/SpectrumQT/XXMI-Launcher/blob/4f5ef31cc237394568119f9feac2d99f44665466/src/xxmi_launcher/gui/windows/settings/frames/advanced_settings_frame.py#L99-L109)

**Right-click the `runs` folder beside Start Here.exe, choose Compress to ZIP file, and attach that ZIP.** Include any saved runs and the error text if a step fails. A screenshot of your graphics settings is helpful. The [advanced reference](advanced-capture.md) has the PowerShell commands, manual capture options, and full benchmark matrix.

## If something stops you

- **Version check failed.** The native build expects the stock **XXMI Libraries v1.3.3** DLL. Keep the vanilla runs and send the library version shown in the launcher.
- **PresentMon says access denied.** PresentMon needs ETW permissions. Its error is in `presentmon.log` in the run folder.
- **Only PresentMon data saved for choice 4 or 5.** Keep that run. Check the launcher setting, relaunch through XXMI with the profiler installed, and stay in ZZZ until the result message. If you installed the previous profiler, use **6** to restore stock, then **3** to install this build.
- **ZZZ won't launch with the profiler.** Close both apps, use choice **6** to restore stock, and send the launch error.
