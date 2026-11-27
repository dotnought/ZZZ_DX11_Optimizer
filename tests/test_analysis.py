# SPDX-License-Identifier: GPL-3.0-or-later
import csv
import json
from pathlib import Path
from tempfile import TemporaryDirectory
import unittest

from tools.analyze import analyze, frame_metrics, summary


def write_csv(path, header, rows):
    with path.open("w", encoding="utf-8", newline="") as stream:
        writer = csv.writer(stream)
        writer.writerow(header.split(","))
        writer.writerows(rows)


class AnalyzeTests(unittest.TestCase):
    def test_percentiles_and_slowest_frame_tail(self):
        result = frame_metrics([10.0] * 999 + [50.0])
        self.assertEqual(result["p999"], 10.0)
        self.assertEqual(result["0_1pct_low_fps"], 20.0)
        self.assertAlmostEqual(result["1pct_low_fps"], 1000 / 14)
        self.assertEqual(result["spikes_50ms"], 1)

    def test_combined_and_standalone_capture(self):
        with TemporaryDirectory() as temp:
            root = Path(temp)
            native = root / "native"
            native.mkdir()
            stem = "capture-fixture"
            (native / f"{stem}.json").write_text(json.dumps({
                "schema_version": 1, "frames": 2, "gpu_valid": 1,
                "gpu_attempts": 1, "gpu_partial_frames": 0,
            }))
            write_csv(native / f"{stem}-frames.csv",
                      "frame,present_hr,interval_ms,driver_present_ms,profiler_boundary_us,immediate_draw_calls,"
                      "deferred_draw_calls,dispatch_calls,rt_bind_calls,stall_calls,gpu_envelope_ms",
                      [(0, 0, 28, 2, 30, 50, 0, 2, 3, 1, "12.0"), (1, 0, 12, 1, 20, 30, 0, 1, 2, 0, "")])
            write_csv(native / f"{stem}-apis.csv",
                      "api,calls,timed_calls,stall_calls,amount,possible_repeats,total_sampled_us,max_sampled_us",
                      [("map_driver", 2, 2, 1, 0, 0, 2400, 2000)])
            write_csv(native / f"{stem}-gpu.csv",
                      "frame,work,shader_hash,rt_width,rt_height,view_format,depth_only,gpu_ms",
                      [(0, "draw", "0000000000000000", 3440, 1440, 28, 0, 6)])
            write_csv(native / f"{stem}-memory.csv",
                      "local_usage,local_budget,nonlocal_usage,nonlocal_budget", [(7, 8, "", "")])
            for name, values, folder in (
                ("vanilla", [20, 30], root / "stock"), ("profiler-l2", [12, 28], native)
            ):
                folder.mkdir(exist_ok=True)
                run = {
                    "scene": "Roscaelifer", "route": "route-a", "settings_id": "settings-a",
                    "condition": name, "game_version": "1", "driver": "1",
                    "cache": "warm", "presentmon": "presentmon.csv",
                }
                if name == "profiler-l2":
                    run["native"] = f"{stem}.json"
                (folder / "run.json").write_text(json.dumps(run))
                write_csv(folder / "presentmon.csv",
                          "ProcessID,SwapChainAddress,PresentRuntime,MsBetweenPresents,MsGPUBusy",
                          [(1, "0x1", "DXGI", value, 8) for value in values] + [(1, "0x2", "DXGI", 5, 0)])
            result = analyze(root)
            self.assertEqual(result["warnings"], [])
            self.assertEqual(len(result["runs"]), 2)
            sample = result["runs"][0]["native"]
            self.assertEqual(sample["top_one_percent_stall_frames"], 1)
            self.assertEqual(sample["memory_usage_to_budget"]["local"]["median"], 0.875)
            self.assertEqual(sample["gpu_workloads"][0]["captured_ms_per_sampled_frame"], 6)
            self.assertEqual(sample["frame_ms"]["mean"], 20)
            self.assertAlmostEqual(result["comparisons"][0]["conditions"]["profiler-l2"]["delta_fps_pct_vs_vanilla"], 25.0)
            self.assertIn("GPU coverage 1/1", summary(result))
            (native / "run.json").unlink()
            self.assertEqual(analyze(root)["runs"][-1]["run"]["condition"], "standalone")


if __name__ == "__main__":
    unittest.main()
