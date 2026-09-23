#!/usr/bin/env python3
"""Summarize native material-editor CSVs without hiding acceptance failures."""

import argparse
import csv
import json
import math
from pathlib import Path


def distribution(values):
    values = sorted(values)
    if not values:
        return {"samples": 0}
    return {"samples": len(values), "min_ms": values[0],
            "p50_ms": values[math.ceil(len(values) * .5) - 1],
            "p95_ms": values[math.ceil(len(values) * .95) - 1], "max_ms": values[-1]}


def rows(path):
    with path.open(encoding="utf-8-sig", newline="") as stream:
        return list(csv.DictReader(stream))


def summarize(directory):
    result = {"source": str(directory.resolve()), "fixtures": []}
    for path in sorted(directory.glob("*-Benchmark.csv")):
        data = rows(path)
        if not data:
            continue
        count = int(data[0]["nodes"])
        item = {"nodes": count, "cold": distribution([float(r["command_ms"]) for r in data if r["stage"] == "Cold"]),
                "repeat": distribution([float(r["command_ms"]) for r in data if r["stage"] == "Repeat"]),
                "fallbacks": sorted({int(r["fallbacks"]) for r in data}),
                "final_capture_cache": sorted({(int(r.get("final_capture_hits", r.get("cache_hits", 0))),
                                                 int(r.get("final_capture_misses", r.get("cache_misses", 0)))) for r in data})}
        slice_path = path.with_name(path.stem + "-Slices.csv")
        if slice_path.exists():
            slices = rows(slice_path)
            item["slices"] = distribution([float(r["slice_ms"]) for r in slices])
            for work in ("format", "routes"):
                item[work + "_slices"] = distribution([float(r["slice_ms"]) for r in slices if r["work"] == work])
        if count <= 300 and item["cold"]["samples"] >= 20 and item["repeat"]["samples"] >= 20:
            item["acceptance"] = {
                "cold_p95_1000ms": item["cold"]["p95_ms"] <= 1000,
                "repeat_p95_250ms": item["repeat"]["p95_ms"] <= 250,
                "slice_p95_8ms": item.get("slices", {}).get("p95_ms", math.inf) <= 8,
                "slice_max_50ms": item.get("slices", {}).get("max_ms", math.inf) <= 50,
            }
        else:
            item["acceptance"] = "stress or diagnostic only"
        result["fixtures"].append(item)
    for path in sorted(directory.glob("*-WirePaint.csv")):
        data = rows(path)
        measured = [r for r in data if int(r["sample"]) >= 0]
        times = distribution([float(r["policy_ms"]) for r in measured])
        result.setdefault("wire_paint", []).append({"file": path.name, "connections": sorted({int(r["connections"]) for r in measured}),
                                                    "timing": times, "p95_2ms": times.get("samples", 0) >= 20 and times.get("p95_ms", math.inf) <= 2})
    return result


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    args.output.write_text(json.dumps(summarize(args.directory), indent=2) + "\n", encoding="utf-8")
