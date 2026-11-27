#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Analyze completed native captures and comparable PresentMon benchmark runs."""

import argparse
from collections import Counter, defaultdict
import csv
import json
import math
from pathlib import Path
import statistics
import sys


def read_json(path):
    return json.loads(path.read_text(encoding="utf-8-sig"))


def read_csv(path):
    with path.open(encoding="utf-8-sig", newline="") as stream:
        return list(csv.DictReader(stream))


def number(value):
    try:
        result = float(value)
    except (TypeError, ValueError):
        return None
    return result if math.isfinite(result) else None


def series(rows, column, positive=False):
    return [
        value for row in rows
        if (value := number(row.get(column))) is not None and (not positive or value > 0)
    ]


def distribution(values):
    if not values:
        return None
    ordered = sorted(values)
    size = len(ordered)

    def percentile(fraction):
        return ordered[max(0, math.ceil(size * fraction) - 1)]

    return {
        "n": size, "mean": statistics.fmean(ordered), "median": statistics.median(ordered),
        "p95": percentile(0.95), "p99": percentile(0.99), "p999": percentile(0.999),
        "max": ordered[-1],
    }


def frame_metrics(values):
    metrics = distribution(values)
    if not metrics:
        return None
    ordered = sorted(values, reverse=True)
    metrics.update({
        "fps": 1000 / metrics["mean"],
        "1pct_low_fps": 1000 / statistics.fmean(ordered[:math.ceil(len(ordered) * 0.01)]),
        "0_1pct_low_fps": 1000 / statistics.fmean(ordered[:math.ceil(len(ordered) * 0.001)]),
        "spikes_33ms": sum(x >= 1000 / 30 for x in ordered),
        "spikes_50ms": sum(x >= 50 for x in ordered),
    })
    return metrics


def presentmon(path, swap_chain=None):
    groups = defaultdict(list)
    for row in read_csv(path):
        mode = (row.get("PresentRuntime") or row.get("Runtime") or "").casefold()
        if mode == "dxgi":
            groups[(row["ProcessID"], row["SwapChainAddress"])].append(row)
    if swap_chain:
        groups = {key: rows for key, rows in groups.items() if key[1] == swap_chain}
    if not groups:
        raise ValueError(f"{path.name} contains no matching DXGI swap chain")
    key, rows = max(groups.items(), key=lambda group: len(group[1]))
    interval = next((name for name in ("MsBetweenPresents", "MsBetweenAppStart", "FrameTime")
                     if name in rows[0]), None)
    metrics = frame_metrics(series(rows, interval, positive=True))
    if not metrics:
        raise ValueError(f"{path.name} contains no positive frame intervals")
    return {
        "pid": key[0], "swap_chain": key[1], "other_swap_chains": len(groups) - 1,
        "interval_column": interval, "frame_ms": metrics,
        "cpu_busy_ms": distribution(series(rows, "MsCPUBusy") or series(rows, "CPUBusy")),
        "gpu_busy_ms": distribution(series(rows, "MsGPUBusy") or series(rows, "GPUBusy")),
        "gpu_time_ms": distribution(series(rows, "MsGPUTime") or series(rows, "GPUTime")),
        "present_ms": distribution(series(rows, "MsInPresentAPI")),
        "displayed_ms": distribution(series(rows, "DisplayedTime", positive=True)),
        "present_modes": Counter(row.get("PresentMode", "") for row in rows),
    }


def native_capture(path):
    meta = read_json(path)
    if meta["schema_version"] != 1:
        raise ValueError(f"Unsupported native capture schema in {path.name}")
    base = path.with_suffix("")

    def table(suffix):
        return read_csv(base.with_name(f"{base.name}-{suffix}.csv"))

    frames = table("frames")
    accepted = [row for row in frames if row["present_hr"] == "0"]
    budget, api, workload = table("memory"), table("apis"), table("gpu")
    if len(frames) != meta["frames"]:
        raise ValueError(f"{path.name} metadata and frame counts differ")
    per_api = defaultdict(Counter)
    for row in api:
        item = per_api[row["api"]]
        for field in ("calls", "timed_calls", "stall_calls", "amount", "possible_repeats"):
            item[field] += int(row[field])
        item["total_sampled_us"] += float(row["total_sampled_us"])
        item["max_sampled_us"] = max(item["max_sampled_us"], float(row["max_sampled_us"]))
    for item in per_api.values():
        if item["timed_calls"]:
            item["estimated_inclusive_us"] = item["total_sampled_us"] * item["calls"] / item["timed_calls"]
    api_ordered = sorted((dict(api=name, **row) for name, row in per_api.items()),
                         key=lambda row: row.get("estimated_inclusive_us", 0), reverse=True)
    per_group = defaultdict(lambda: defaultdict(float))
    for row in workload:
        group = (row["work"], row["shader_hash"], row["rt_width"], row["rt_height"],
                 row["view_format"], row["depth_only"])
        per_group[group][int(row["frame"])] += float(row["gpu_ms"])
    num_gpu = sum(row["gpu_envelope_ms"] != "" for row in frames)
    groups = []
    for key, group in per_group.items():
        groups.append({
            "work": key[0], "shader": key[1], "width": int(key[2]), "height": int(key[3]),
            "format": int(key[4]), "depth_only": key[5] == "1",
            "observed_ms": distribution(list(group.values())),
            "captured_ms_per_sampled_frame": sum(group.values()) / max(1, num_gpu),
        })
    groups.sort(key=lambda row: row["captured_ms_per_sampled_frame"], reverse=True)
    by_frame = []
    for row in accepted:
        interval = number(row["interval_ms"])
        if interval and interval > 0:
            by_frame.append((interval, row))
    by_frame.sort(key=lambda entry: entry[0], reverse=True)
    worst_one_percent = [row for _, row in by_frame[:math.ceil(len(by_frame) * 0.01)]]
    gpu_by_index = {int(row["frame"]): float(row["gpu_envelope_ms"])
                    for row in accepted if number(row["gpu_envelope_ms"]) is not None}
    memory = {}
    for locality in ("local", "nonlocal"):
        memory[locality] = distribution([
            usage / cap for row in budget
            if (usage := number(row[f"{locality}_usage"])) is not None
            and (cap := number(row[f"{locality}_budget"])) is not None and cap > 0
        ])

    return {
        "metadata": meta, "frame_ms": frame_metrics([x for x, _ in by_frame]),
        "non_s_ok_presents": len(frames) - len(accepted),
        "gpu_envelope_ms": distribution(list(gpu_by_index.values())),
        "present_ms": distribution(series(accepted, "driver_present_ms")),
        "profiler_boundary_us": distribution(series(accepted, "profiler_boundary_us")),
        "immediate_draw_calls": distribution(series(accepted, "immediate_draw_calls")),
        "deferred_draw_calls": distribution(series(accepted, "deferred_draw_calls")),
        "dispatch_calls": distribution(series(accepted, "dispatch_calls")),
        "rt_bind_calls": distribution(series(accepted, "rt_bind_calls")),
        "memory_usage_to_budget": memory, "api_by_estimated_inclusive_time": api_ordered,
        "gpu_workloads": groups,
        "top_one_percent_stall_frames": sum(int(row["stall_calls"]) > 0 for row in worst_one_percent),
        "top_one_percent_frames": len(worst_one_percent),
        "other_stall_frames": sum(int(row["stall_calls"]) > 0 for _, row in by_frame[len(worst_one_percent):]),
        "other_frames": len(by_frame) - len(worst_one_percent),
        "worst_frames": [row for _, row in by_frame[:20]],
        "limitation": "Inclusive API timers overlap. GPU envelopes include queue gaps. Workload segments cover observed immediate-context work and can contain untimed operations.",
    }


def comparable(manifest):
    if all(manifest.get(key) for key in ("scene", "settings_id", "game_version", "driver",
                                        "cache", "route", "condition")):
        return tuple(manifest[key] for key in ("scene", "settings_id", "game_version", "driver", "cache", "route"))
    return None


def comparisons(runs):
    groups = defaultdict(lambda: defaultdict(list))
    for row in runs:
        manifest = row["run"]
        key = comparable(manifest)
        if key and row.get("presentmon"):
            key += (row["presentmon"]["interval_column"],)
            groups[key][manifest["condition"]].append(row["presentmon"]["frame_ms"])
    results = []
    for key, conditions in sorted(groups.items()):
        baseline = conditions.get("vanilla", [])
        medians = {}
        for condition, summaries in conditions.items():
            medians[condition] = {
                "runs": len(summaries),
                **{field: statistics.median(row[field] for row in summaries)
                   for field in ("fps", "1pct_low_fps", "0_1pct_low_fps", "p99", "p999")},
            }
        for name, summary in medians.items():
            if baseline and name != "vanilla":
                summary["delta_fps_pct_vs_vanilla"] = 100 * (summary["fps"] / medians["vanilla"]["fps"] - 1)
            if "profiler-off" in medians and name in ("profiler-l1", "profiler-l2"):
                summary["delta_fps_pct_vs_profiler_off"] = 100 * (summary["fps"] / medians["profiler-off"]["fps"] - 1)
        results.append({"scene": key[0], "settings_id": key[1], "game_version": key[2],
                        "driver": key[3], "cache": key[4], "route": key[5],
                        "interval_column": key[6], "conditions": medians})
    return results


def summary(data):
    lines = ["# DX11 capture report", ""]
    for row in data["runs"]:
        meta, pm, native = row["run"], row.get("presentmon"), row.get("native")
        lines += [f"## {meta.get('scene', 'Unlabeled')} / {meta.get('condition', 'standalone')}", ""]
        for name, source in (("PresentMon", pm), ("Native", native)):
            if source and (f := source["frame_ms"]):
                lines.append(f"- {name} measured {f['n']} frames, {f['fps']:.2f} FPS, "
                             f"{f['1pct_low_fps']:.2f} FPS 1% low and {f['0_1pct_low_fps']:.2f} FPS 0.1% low. "
                             f"p99/p99.9 were {f['p99']:.2f}/{f['p999']:.2f} ms.")
        if pm and pm["gpu_busy_ms"]:
            lines.append(f"- PresentMon GPU busy median {pm['gpu_busy_ms']['median']:.2f} ms.")
        if native:
            m = native["metadata"]
            gpu = native["gpu_envelope_ms"]
            lines.append(f"- Native GPU coverage {m['gpu_valid']}/{m['gpu_attempts']} scheduled samples. "
                         f"{m['gpu_partial_frames']} samples hit the segment cap."
                         + (f" Median GPU envelope {gpu['median']:.2f} ms." if gpu else ""))
            lines.append(f"- Stall candidates occurred on {native['top_one_percent_stall_frames']}/"
                         f"{native['top_one_percent_frames']} slowest frames and "
                         f"{native['other_stall_frames']}/{native['other_frames']} other frames.")
            for api in native["api_by_estimated_inclusive_time"][:4]:
                if "estimated_inclusive_us" in api:
                    lines.append(f"- {api['api']} had {api['timed_calls']}/{api['calls']} timed calls, "
                                 f"{api['max_sampled_us'] / 1000:.2f} ms sampled max and "
                                 f"{api['stall_calls']} stall candidates.")
            for group in native["gpu_workloads"][:3]:
                lines.append(f"- Observed GPU {group['work']} group at {group['width']}×{group['height']} "
                             f"(DXGI format {group['format']}, shader {group['shader']}) accounted for "
                             f"{group['captured_ms_per_sampled_frame']:.3f} ms per sampled frame.")
        lines.append("")
    for group in data["comparisons"]:
        lines += [f"## Comparable runs for {group['scene']} / {group['settings_id']}", "",
                  "| Condition | Runs | FPS | 1% low | p99 ms | FPS Δ vs vanilla | FPS Δ vs profiler off |",
                  "|---|---:|---:|---:|---:|---:|---:|"]
        for name, result in sorted(group["conditions"].items()):
            def delta(field):
                return f"{result[field]:+.2f}%" if field in result else "—"
            lines.append(f"| {name} | {result['runs']} | {result['fps']:.2f} | {result['1pct_low_fps']:.2f} | "
                         f"{result['p99']:.2f} | {delta('delta_fps_pct_vs_vanilla')} | "
                         f"{delta('delta_fps_pct_vs_profiler_off')} |")
        lines.append("")
    if data["warnings"]:
        lines.extend(["## Input warnings", "", *(f"- {error}" for error in data["warnings"]), ""])
    return "\n".join(lines)


def analyze(root):
    runs, warnings = [], []
    used = set()
    manifests = list(sorted(root.rglob("run.json")))
    for path in manifests:
        try:
            manifest = read_json(path)
            row = {"run": manifest}
            if manifest.get("presentmon"):
                row["presentmon"] = presentmon(path.parent / manifest["presentmon"], manifest.get("swap_chain"))
            if manifest.get("native_missing"):
                warnings.append(f"{path.relative_to(root)} — native capture was missing")
            if manifest.get("native"):
                source = (path.parent / manifest["native"]).resolve()
                row["native"] = native_capture(source)
                used.add(source)
                if row.get("presentmon") and str(row["native"]["metadata"].get("pid", "")) not in (
                    "", row["presentmon"]["pid"]
                ):
                    warnings.append(f"{path.relative_to(root)} — native and PresentMon PIDs differ")
            runs.append(row)
        except (OSError, ValueError, KeyError, TypeError) as exc:
            warnings.append(f"{path.relative_to(root)} — {exc}")
    for path in sorted(root.rglob("capture-*.json")):
        if path.resolve() in used:
            continue
        try:
            runs.append({"run": {"scene": path.parent.name, "condition": "standalone"},
                         "native": native_capture(path)})
        except (OSError, ValueError, KeyError, TypeError) as exc:
            warnings.append(f"{path.relative_to(root)} — {exc}")
    return {"runs": runs, "comparisons": comparisons(runs), "warnings": warnings}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("root", type=Path, help="Directory of benchmark run directories or standalone captures")
    parser.add_argument("--output", type=Path, default=Path("report"), help="Output file stem")
    args = parser.parse_args()
    if not args.root.is_dir():
        parser.error("root must be a directory")
    data = analyze(args.root)
    if not data["runs"]:
        for warning in data["warnings"]:
            print(warning, file=sys.stderr)
        parser.error("no completed capture could be read")
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.with_suffix(".json").write_text(json.dumps(data, indent=2) + "\n", encoding="utf-8")
    args.output.with_suffix(".md").write_text(summary(data) + "\n", encoding="utf-8")
    print(f"Analyzed {len(data['runs'])} runs, with {len(data['warnings'])} input warnings.")


if __name__ == "__main__":
    main()
