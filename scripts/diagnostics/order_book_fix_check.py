#!/usr/bin/env python3
"""Median of each build's runs per benchmark, relative to base, from the JSON
files of order_book_fix_check.sh (a directory, or a .tar.gz of one)."""
import collections
import json
import re
import statistics as st
import sys
import tarfile
from pathlib import Path

UNIT = {"ns": 1.0, "us": 1e3, "ms": 1e6}


def runs(src):
    if src.suffix == ".gz":
        with tarfile.open(src) as tar:
            for m in tar.getmembers():
                if m.name.endswith(".json"):
                    yield Path(m.name).name, json.load(tar.extractfile(m))
    else:
        for f in sorted(src.glob("*.json")):
            yield f.name, json.loads(f.read_text())


vals = collections.defaultdict(list)
for name, data in runs(Path(sys.argv[1])):
    build = name.split("_")[0]
    for b in data["benchmarks"]:
        if b["run_type"] == "iteration":
            vals[(b["name"], build)].append(b["real_time"] * UNIT[b["time_unit"]])

print("| Benchmark | base (ns) | c1 | c2 | CV of base |")
print("|---|---:|---:|---:|---:|")
natural = lambda n: [int(t) if t.isdigit() else t for t in re.split(r"(\d+)", n)]
for bench in sorted({n for n, _ in vals}, key=lambda n: (n.startswith("BM_Pipeline"), natural(n))):
    m = {b: st.median(vals[(bench, b)]) for b in ("base", "c1", "c2")}
    base = vals[(bench, "base")]
    print(f"| `{bench}` | {m['base']:,.0f} | {100 * (m['c1'] / m['base'] - 1):+.1f}% "
          f"| {100 * (m['c2'] / m['base'] - 1):+.1f}% | {100 * st.stdev(base) / st.mean(base):.1f}% |")
