#!/usr/bin/env python3
"""Tables of the rough check and the sanity run, from the JSON files of
bench_semantics_check.sh and of the sanity run (a directory, or a .tar.gz
of one).

Rough check: master_* and new_* files, the median of each build's runs per
benchmark; the branch's BM_AddOrder_NoMatch_V3 is matched with master's
BM_AddOrder_NoMatch. Sanity run: sanity_master_* and sanity_new_* files,
the median of three repetitions per benchmark, master next to the branch's
_V3 and _V1 versions, with the branch's per-iteration counter."""
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


def cv(xs):
    return 100 * st.stdev(xs) / st.mean(xs)


natural = lambda n: [int(t) if t.isdigit() else t for t in re.split(r"(\d+)", n)]
rough = collections.defaultdict(list)
sanity = collections.defaultdict(list)
counters = {}
for name, data in runs(Path(sys.argv[1])):
    parts = name.split("_")
    for b in data["benchmarks"]:
        if b["run_type"] != "iteration":
            continue
        ns = b["real_time"] * UNIT[b["time_unit"]]
        if parts[0] == "sanity":
            m = re.fullmatch(r"(BM_\w+?)(_V3|_V1)?(/\d+)?(/iterations:\d+)?", b["name"])
            version = {"_V3": "v3", "_V1": "v1", None: parts[1]}[m.group(2)]
            bench = m.group(1) + (m.group(3) or "")
            if parts[1] == "new" and version == "new":   # the single-threaded pair in bench_matching_engine
                version = "v3"
            sanity[(bench, version)].append(ns)
            for key in ("trades_per_iteration", "cancels_per_iteration"):
                if key in b and version == "v3":
                    counters[bench] = f"{key.split('_')[0]}: {b[key]:,.0f}"
        else:
            bench = re.sub(r"^BM_AddOrder_NoMatch_V3$", "BM_AddOrder_NoMatch", b["name"])
            rough[(bench, parts[0])].append(ns)

print("| Benchmark | master (ns) | branch (ns) | change | CV master | CV branch |")
print("|---|---:|---:|---:|---:|---:|")
for bench in sorted({n for n, _ in rough}, key=lambda n: (n.startswith("BM_Pipeline"), natural(n))):
    old, new = rough.get((bench, "master")), rough.get((bench, "new"))
    if old and new:
        print(f"| `{bench}` | {st.median(old):,.0f} | {st.median(new):,.0f} "
              f"| {100 * (st.median(new) / st.median(old) - 1):+.1f}% | {cv(old):.1f}% | {cv(new):.1f}% |")
    else:
        print(f"| `{bench}` | | {st.median(new):,.0f} | | | {cv(new):.1f}% |")

print()
print("| Benchmark | master (ns) | branch, v3 (ns) | branch, v1 (ns) | per iteration |")
print("|---|---:|---:|---:|---|")
fmt = lambda xs: f"{st.median(xs):,.1f}" if xs and st.median(xs) < 100 else (f"{st.median(xs):,.0f}" if xs else "")
for bench in dict.fromkeys(n for n, _ in sanity):   # in the order the programs run them
    print(f"| `{bench}` | {fmt(sanity.get((bench, 'master')))} | {fmt(sanity.get((bench, 'v3')))} "
          f"| {fmt(sanity.get((bench, 'v1')))} | {counters.get(bench, '')} |")
