#!/usr/bin/env python3
"""Summarize completed diagnostic studies, keeping launches independent.

Usage: python3 scripts/diagnostics/summarize.py STUDY [STUDY ...]
       python3 scripts/diagnostics/summarize.py --format csv STUDY

Each launch is the median of its recorded repetitions (five by default).
CV uses sample standard deviation. IQR uses inclusive, linearly interpolated
quartiles. Studies are reported separately, never silently pooled. Layout lines
are joined to launches through command metadata, not the condition index in a
filename. Repeated address lines within one launch are not independent samples.
"""
import argparse
from collections import defaultdict
import csv
import json
from pathlib import Path
import re
import statistics as stats
import sys
import tarfile


def read_csv(path):
    with path.open(newline="") as handle:
        return list(csv.DictReader(handle))


def launch_key(row):
    return (int(row["session"]), int(row.get("block", row.get("round", ""))),
            row["condition"], row["binary"])


def cv(values):
    return stats.stdev(values) / stats.mean(values) * 100 if len(values) > 1 else None


def iqr_percent(values):
    quartiles = stats.quantiles(values, n=4, method="inclusive") if len(values) > 1 else values * 3
    return (quartiles[2] - quartiles[0]) / stats.median(values) * 100


def raw_files(directory):
    """Read diagnostic logs and metadata without extracting an archive."""
    files = {}

    def keep(name, data):
        name = Path(name).name
        if name in files and files[name] != data:
            raise ValueError(f"conflicting raw copies: {name}")
        files[name] = data

    archive_path = directory / "raw.tar.gz"
    if archive_path.exists():
        with tarfile.open(archive_path, "r:gz") as archive:
            for member in archive:
                if member.isfile() and (member.name.endswith(".log") or
                                        member.name.endswith(".command.json")):
                    keep(member.name, archive.extractfile(member).read())
    raw = directory / "raw"
    if raw.exists():
        for path in raw.iterdir():
            if path.is_file() and (path.name.endswith(".log") or path.name.endswith(".command.json")):
                keep(path.name, path.read_bytes())
    return files


def layout_rows(directory, launches):
    files = raw_files(directory)
    by_key = {launch_key(row): row for row in launches}
    observations = {}
    for name, data in files.items():
        if not name.endswith(".log"):
            continue
        lines = re.findall(r"^MM_LAYOUT\s+(.+)$", data.decode(errors="replace"), re.MULTILINE)
        if not lines:
            continue
        stem = name.removesuffix(".log")
        metadata_name = stem + ".command.json"
        if metadata_name not in files:
            raise ValueError(f"layout log has no command metadata: {name}")
        metadata = json.loads(files[metadata_name])
        if not metadata.get("complete"):
            raise ValueError(f"layout log belongs to an incomplete launch: {name}")
        # The filename provides only session/block identity. Condition names
        # always come from metadata, including after matrix reordering.
        identity = re.fullmatch(r"s(\d+)_b(\d+)_c\d+", stem)
        if not identity:
            raise ValueError(f"unrecognized diagnostic layout filename: {name}")
        session, block = map(int, identity.groups())
        matches = [key for key in by_key if key[:3] == (session, block, metadata["condition"])]
        if len(matches) != 1:
            raise ValueError(f"layout metadata does not identify exactly one launch: {name}")
        key = matches[0]
        if key in observations:
            raise ValueError(f"multiple layout logs identify the same launch: {key}")
        values = {field: set() for field in ("address", "mod64", "mod4096")}
        for line in lines:
            fields = dict(re.findall(r"(\w+)=([^\s]+)", line))
            for field in values:
                if field not in fields:
                    raise ValueError(f"missing {field} in layout log {name}")
                values[field].add(int(fields[field], 0))
            address = int(fields["address"], 0)
            if address % 64 != int(fields["mod64"]) or address % 4096 != int(fields["mod4096"]):
                raise ValueError(f"inconsistent address residues in layout log {name}")
        observations[key] = values

    grouped = defaultdict(list)
    for key, values in observations.items():
        grouped[key[2:]].append(values)
    rows = []
    for (condition, binary), launches_values in sorted(grouped.items()):
        combined = {field: set().union(*(v[field] for v in launches_values))
                    for field in ("address", "mod64", "mod4096")}
        rows.append({
            "kind": "layout", "condition": condition, "binary": binary,
            "layout_launches": len(launches_values),
            "unique_addresses": len(combined["address"]),
            "mod64_values": ";".join(map(str, sorted(combined["mod64"]))),
            "unique_mod64": len(combined["mod64"]),
            "unique_mod4096": len(combined["mod4096"]),
            "multi_mod64_launches": sum(len(v["mod64"]) > 1 for v in launches_values),
            "multi_mod4096_launches": sum(len(v["mod4096"]) > 1 for v in launches_values),
        })
    return rows


def summarize(directory):
    environment_path = directory / "env.json"
    environment = json.loads(environment_path.read_text()) if environment_path.exists() else {}
    if environment.get("complete") is False:
        raise ValueError(f"{directory}: study is incomplete; wait for recording and cleanup")
    expected = environment.get("design", {}).get("repetitions", 5)
    if not isinstance(expected, int) or expected < 1:
        raise ValueError(f"{directory}: invalid repetition count")
    samples = read_csv(directory / "samples.csv")
    launches = read_csv(directory / "launches.csv")
    launch_records = {launch_key(row): row for row in launches}
    if len(launch_records) != len(launches):
        raise ValueError(f"{directory}: duplicate launch records")
    if not samples or not launches:
        raise ValueError(f"{directory}: no complete measurements")
    for key, row in launch_records.items():
        if int(row.get("exit_status", "0")):
            raise ValueError(f"{directory}: failed launch {key}")

    repetitions = defaultdict(dict)
    for row in samples:
        key = launch_key(row)
        if key not in launch_records:
            raise ValueError(f"sample has no matching launch: {key}")
        group = repetitions[(key, row["benchmark"])]
        index = int(row["repetition"])
        if index in group:
            raise ValueError(f"duplicate repetition {index}: {key}, {row['benchmark']}")
        value = float(row["real_time_ns"])
        if not 0 < value < float("inf"):
            raise ValueError(f"invalid real time: {value}")
        group[index] = value

    grouped = defaultdict(list)
    for (key, benchmark), repetition_values in repetitions.items():
        if set(repetition_values) != set(range(expected)):
            raise ValueError(f"incomplete repetitions: {key}, {benchmark}; expected {expected}")
        values = list(repetition_values.values())
        grouped[(*key[2:], benchmark)].append((stats.median(values), cv(values)))
    missing = set(launch_records) - {key for key, _ in repetitions}
    if missing:
        raise ValueError(f"launches without samples: {sorted(missing)}")

    rows = []
    for (condition, binary, benchmark), entries in sorted(grouped.items()):
        medians = [value for value, _ in entries]
        inside_cv = [value for _, value in entries if value is not None]
        rows.append({
            "kind": "benchmark", "condition": condition, "binary": binary, "benchmark": benchmark,
            "launches": len(medians), "repetitions": expected,
            "median_ns": stats.median(medians), "min_ns": min(medians), "max_ns": max(medians),
            "spread_pct": (max(medians) / min(medians) - 1) * 100,
            "launch_cv_pct": cv(medians), "iqr_pct": iqr_percent(medians),
            "within_cv_median_pct": stats.median(inside_cv) if inside_cv else None,
            "within_cv_max_pct": max(inside_cv) if inside_cv else None,
        })

    telemetry = defaultdict(list)
    for row in launches:
        telemetry[(row["condition"], row["binary"])].append(row)
    for (condition, binary), records in sorted(telemetry.items()):
        def values(field):
            return [float(row[field]) for row in records]

        if any(float(row["wall_s"]) <= 0 for row in records):
            raise ValueError("launch wall time must be positive")
        foreign = [float(row["foreign_ms"]) / float(row["wall_s"]) for row in records]
        rows.append({
            "kind": "telemetry", "condition": condition, "binary": binary, "launches": len(records),
            "throttle_core": int(sum(values("throttle_core"))),
            "throttle_package": int(sum(values("throttle_package"))),
            "device_irqs": int(sum(values("device_irqs"))),
            "thermal_irqs": int(sum(values("irq_TRM"))),
            "suspended_s": sum(values("suspended_s")),
            "foreign_ms_per_s_median": stats.median(foreign),
            "foreign_ms_per_s_max": max(foreign),
            "foreign_above_5_ms_per_s": sum(value > 5 for value in foreign),
        })
    rows += layout_rows(directory, launches)
    return [{"study": str(directory), **row} for row in rows]


def display(value):
    if value is None:
        return "n/a"
    if isinstance(value, float):
        return f"{value:.6g}"
    return str(value).replace("|", "\\|").replace("\n", " ")


def markdown(rows):
    output = [
        "Launch scores are medians of repetitions; all times below are ns.",
        "Spread = (max / min − 1) × 100; CV uses sample standard deviation; "
        "IQR uses inclusive quartiles divided by the median. CV requires at least two observations.",
        "Layout counts use independent launches; repeated lines in a launch are deduplicated.", "",
    ]
    for study in dict.fromkeys(row["study"] for row in rows):
        output += [f"## {study}", ""]
        for kind in ("benchmark", "telemetry", "layout"):
            selected = [row for row in rows if row["study"] == study and row["kind"] == kind]
            if not selected:
                continue
            fields = [field for field in selected[0] if field not in ("study", "kind")]
            output += [f"### {kind.capitalize()}", "",
                       "| " + " | ".join(fields) + " |",
                       "| " + " | ".join("---" for _ in fields) + " |"]
            output += ["| " + " | ".join(display(row[field]) for field in fields) + " |" for row in selected]
            output.append("")
    return "\n".join(output)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directories", nargs="+", type=Path)
    parser.add_argument("--format", choices=("markdown", "csv"), default="markdown")
    args = parser.parse_args()
    try:
        rows = [row for directory in args.directories for row in summarize(directory)]
    except (OSError, ValueError, KeyError, tarfile.TarError) as exc:
        parser.error(str(exc))
    if args.format == "csv":
        fields = list(dict.fromkeys(field for row in rows for field in row))
        writer = csv.DictWriter(sys.stdout, fieldnames=fields, lineterminator="\n")
        writer.writeheader()
        writer.writerows(rows)
    else:
        print(markdown(rows))


if __name__ == "__main__":
    main()
