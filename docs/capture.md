# First Windows capture

Extract the entire [Windows test ZIP](https://github.com/dotnought/ZZZ_DX11_Optimizer/tree/profiling-milestone-1-build) and double-click **Start Here.exe**. Keep the menu window open for all four runs with the game set to **DirectX 11**. Keep your resolution and graphics settings fixed. In each area, pick a spot and point the camera the same way for both recordings.

| Menu choice | What to do |
|---|---|
| **1** | Start ZZZ through its normal launcher with XXMI closed. Go to your Roscaelifer spot and record. |
| **2** | Go to your Sixth Street spot and record. |
| **3** | Close ZZZ and XXMI. Select your ZZMI folder in the picker to back up stock v1.3.3 and install the profiler. |
| **4** | Start ZZZ through XXMI. Return to the same Roscaelifer spot and record. |
| **5** | Return to the same Sixth Street spot and record. |
| **6** | Close ZZZ and XXMI. Restore the backed-up stock DLL. |

**To record:** Choose the number and press Enter. When the tool says **Waiting**, Alt+Tab back to ZZZ, hold **Ctrl+Alt+F8** briefly, then stay in the game for 45 seconds. The tool prints **Saved** and the folder name when it finishes.

**To find the ZZMI folder for choice 3:** In XXMI Launcher, select **ZZMI** and click **Open Mods Folder**. Go up one folder from **Mods**. The folder you need contains both **d3dx.ini** and **d3d11.dll**. Note where it is, then close ZZZ and XXMI Launcher, including its tray icon. Choose **3**, select that folder, and confirm the swap.

**To launch for choices 4 and 5:** In XXMI Launcher with ZZMI selected, open **Settings → Advanced → Unsafe Mode**. This is the launcher's built-in option to allow third-party DLLs. If you prefer to leave it off, the first two recordings are still useful. After choice **6**, return it to its previous setting. [Launcher setting source](https://github.com/SpectrumQT/XXMI-Launcher/blob/4f5ef31cc237394568119f9feac2d99f44665466/src/xxmi_launcher/gui/windows/settings/frames/advanced_settings_frame.py#L99-L109)

**Right-click the `runs` folder beside Start Here.exe, choose Compress to ZIP file, and attach that ZIP.** Include any saved runs and the error text if a step fails. A screenshot of your graphics settings is helpful. The [advanced reference](advanced-capture.md) has the PowerShell commands, manual capture options, and full benchmark matrix.

## If something stops you

- **Version check failed.** The native build expects the stock **XXMI Libraries v1.3.3** DLL. Keep the vanilla runs and send the library version shown in the launcher.
- **PresentMon says access denied.** Use a Windows account with ETW access. The capture tool prints PresentMon's error so you can identify this case.
- **Only PresentMon data saved for choice 4 or 5.** Keep that run. Check the launcher setting, relaunch through XXMI with the profiler installed, and press the chord while the game has focus. Wait for the tool to print **Saved** before closing ZZZ.
- **ZZZ won't launch with the profiler.** Close both apps, use choice **6** to restore stock, and send the launch error.
