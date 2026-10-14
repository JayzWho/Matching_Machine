#!/usr/bin/env python3
"""Plot completed ASLR and object-placement studies as independent launches.

Usage:
  python3 scripts/diagnostics/plot_launch_variance.py ASLR_STUDY LAYOUT_STUDY

Writes launch_variance.svg to the directory above LAYOUT_STUDY by default.
Matplotlib is used when installed; otherwise the same data are drawn with Cairo. Jitter is derived
from each launch identity, never from performance, and is deterministic.
"""
import argparse
from collections import defaultdict
import hashlib
import math
from pathlib import Path
import statistics

import summarize


BENCHMARK = "BM_MutexPipeline_Throughput/100000/iterations:3"
PANELS = (
    ("Original benchmark", "Process ASLR control", ("native_aslr", "native_noaslr"),
     ("Enabled", "Disabled")),
    ("Fixed object placement", "Object address modulo 64 bytes; ASLR enabled",
     ("offset_00", "offset_16", "offset_32", "offset_48"), ("0", "16", "32", "48")),
)
COLORS = ("#007F85", "#CA6D32", "#7560A5", "#5E7C3B")


def load_launches(directory, panel):
    # Shared validation rejects incomplete studies and malformed repetition sets.
    summarize.summarize(directory)
    grouped = defaultdict(list)
    for row in summarize.read_csv(directory / "samples.csv"):
        if row["benchmark"] == BENCHMARK:
            grouped[summarize.launch_key(row)].append(float(row["real_time_ns"]))
    conditions = PANELS[panel][2]
    if {key[2] for key in grouped} != set(conditions):
        raise ValueError(f"{directory}: expected exactly {conditions} for {BENCHMARK}")
    rows = []
    for (session, block, condition, binary), repetitions in sorted(grouped.items()):
        identity = f"{panel}/{session}/{block}/{condition}/{binary}"
        fraction = int.from_bytes(hashlib.sha256(identity.encode()).digest()[:8], "big") / (2**64 - 1)
        rows.append({"panel": panel, "study": str(directory), "condition": condition,
                     "session": session, "block": block, "binary": binary,
                     "repetitions": len(repetitions),
                     "median_ms": statistics.median(repetitions) / 1e6,
                     "jitter": (fraction - 0.5) * 0.30})
    return rows


def plot_matplotlib(rows, output, ymin, ymax, sessions):
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    from matplotlib.lines import Line2D

    plt.rcParams.update({"font.family": "DejaVu Sans", "font.size": 11,
                         "svg.hashsalt": "matching-machine-launch-variance"})
    figure, axes = plt.subplots(1, 2, figsize=(12, 6.2), sharey=True,
                               gridspec_kw={"width_ratios": [1, 1.25]})
    for panel, axis in enumerate(axes):
        title, subtitle, conditions, labels = PANELS[panel]
        panel_rows = [row for row in rows if row["panel"] == panel]
        for session in sessions:
            mine = [row for row in panel_rows if row["session"] == session]
            axis.scatter([conditions.index(row["condition"]) + row["jitter"] for row in mine],
                         [row["median_ms"] for row in mine], s=37, alpha=0.85,
                         color=COLORS[sessions.index(session)], edgecolors="white", linewidths=0.4,
                         zorder=3)
        ticks = []
        for index, (condition, label) in enumerate(zip(conditions, labels)):
            values = [row["median_ms"] for row in panel_rows if row["condition"] == condition]
            axis.hlines(statistics.median(values), index - 0.23, index + 0.23,
                        color="#20272D", linewidth=2.2, zorder=4)
            ticks.append(f"{label}\nn = {len(values)}")
        axis.set_xticks(range(len(conditions)), ticks)
        axis.set_xlim(-0.6, len(conditions) - 0.4)
        axis.set_ylim(ymin, ymax)
        axis.set_yticks(range(int(ymin), int(ymax) + 1, 5))
        axis.set_title(f"{'AB'[panel]}. {title}\n{subtitle}", loc="left", fontsize=11, pad=18)
        axis.grid(axis="y", color="#E1E6EA", zorder=0)
        axis.spines[["top", "right"]].set_visible(False)
        axis.tick_params(length=0, pad=9)
    axes[0].set_ylabel("100,000 orders: launch median (ms)", labelpad=12)
    figure.suptitle("Mutex pipeline launch variation", x=0.065, y=0.98, ha="left", fontsize=20)
    figure.text(0.065, 0.91, "Each dot = one process; median of 5 repetitions (3 workload iterations each)",
                color="#515D67", fontsize=11)
    handles = [Line2D([0], [0], marker="o", linestyle="", color=COLORS[i], label=f"Session {session}")
               for i, session in enumerate(sessions)]
    figure.legend(handles=handles, loc="lower left", bbox_to_anchor=(0.06, 0.07),
                  ncol=len(sessions), frameon=False)
    figure.text(0.065, 0.035, "Black bars: condition medians. Diagnostic placement uses a separate binary.",
                fontsize=10, color="#515D67")
    figure.subplots_adjust(left=0.09, right=0.975, bottom=0.22, top=0.76, wspace=0.15)
    figure.savefig(output / "launch_variance.svg", metadata={"Date": None})
    plt.close(figure)
    return {"renderer": "matplotlib", "version": matplotlib.__version__}


def plot_cairo(rows, output, ymin, ymax, sessions):
    import cairo
    width, height = 1200, 620

    def rgb(value):
        return tuple(int(value[i:i + 2], 16) / 255 for i in (1, 3, 5))

    def draw(context):
        context.set_source_rgb(1, 1, 1)
        context.paint()

        def text(x, y, content, size=12, color="#20272D", align="left", bold=False):
            context.select_font_face("DejaVu Sans", cairo.FONT_SLANT_NORMAL,
                                     cairo.FONT_WEIGHT_BOLD if bold else cairo.FONT_WEIGHT_NORMAL)
            context.set_font_size(size)
            extents = context.text_extents(content)
            if align == "center":
                x -= extents.width / 2 + extents.x_bearing
            elif align == "right":
                x -= extents.width + extents.x_bearing
            context.set_source_rgb(*rgb(color))
            context.move_to(x, y)
            context.show_text(content)

        def line(x1, y1, x2, y2, color, thickness=1):
            context.set_source_rgb(*rgb(color))
            context.set_line_width(thickness)
            context.move_to(x1, y1)
            context.line_to(x2, y2)
            context.stroke()

        text(65, 45, "Mutex pipeline launch variation", 25, bold=True)
        text(65, 76, "Each dot = one process; median of 5 repetitions (3 workload iterations each)",
             13, "#515D67")
        top, bottom = 166, 472
        panels = ((100, 490), (610, 1135))
        for panel, (left, right) in enumerate(panels):
            title, subtitle, conditions, labels = PANELS[panel]
            panel_rows = [row for row in rows if row["panel"] == panel]
            text(left, 116, f"{'AB'[panel]}. {title}", 16, bold=True)
            text(left, 141, subtitle, 12, "#515D67")

            def y(value):
                return bottom - (value - ymin) / (ymax - ymin) * (bottom - top)

            step = (right - left) / len(conditions)
            for tick in range(int(ymin), int(ymax) + 1, 5):
                line(left, y(tick), right, y(tick), "#E1E6EA")
                if panel == 0:
                    text(left - 12, y(tick) + 4, str(tick), 12, align="right")
            line(left, top, left, bottom, "#85929C")
            line(left, bottom, right, bottom, "#85929C")
            for index, (condition, label) in enumerate(zip(conditions, labels)):
                mine = [row for row in panel_rows if row["condition"] == condition]
                center = left + (index + 0.5) * step
                for row in mine:
                    context.set_source_rgba(*rgb(COLORS[sessions.index(row["session"])]), 0.85)
                    context.arc(center + row["jitter"] * step, y(row["median_ms"]), 4.2, 0, 2 * math.pi)
                    context.fill_preserve()
                    context.set_source_rgb(1, 1, 1)
                    context.set_line_width(0.5)
                    context.stroke()
                median = statistics.median(row["median_ms"] for row in mine)
                line(center - step * 0.23, y(median), center + step * 0.23, y(median), "#20272D", 2.2)
                text(center, bottom + 26, label, 13, align="center")
                text(center, bottom + 47, f"n = {len(mine)}", 11, "#515D67", align="center")
        context.save()
        context.translate(35, (top + bottom) / 2)
        context.rotate(-math.pi / 2)
        text(0, 0, "100,000 orders: launch median (ms)", 13, align="center")
        context.restore()
        for index, session in enumerate(sessions):
            x = 70 + index * 140
            context.set_source_rgb(*rgb(COLORS[index]))
            context.arc(x, 557, 4.2, 0, 2 * math.pi)
            context.fill()
            text(x + 12, 562, f"Session {session}", 12)
        text(65, 594, "Black bars: condition medians. Diagnostic placement uses a separate binary.",
             12, "#515D67")

    svg = cairo.SVGSurface(str(output / "launch_variance.svg"), width, height)
    draw(cairo.Context(svg))
    svg.finish()
    return {"renderer": "cairo", "version": cairo.cairo_version_string(),
            "python_binding_version": cairo.version}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("aslr_study", type=Path)
    parser.add_argument("layout_study", type=Path)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    directories = (args.aslr_study, args.layout_study)
    rows = [row for panel, directory in enumerate(directories) for row in load_launches(directory, panel)]
    if any(row["repetitions"] != 5 for row in rows):
        parser.error("this figure labels five repetitions and requires five per launch")
    sessions = sorted({row["session"] for row in rows})
    if len(sessions) > len(COLORS):
        parser.error("figure supports at most four session colors")
    ymin = math.floor(min(row["median_ms"] for row in rows) / 5) * 5
    ymax = math.ceil(max(row["median_ms"] for row in rows) / 5) * 5
    if ymax == ymin:
        ymax += 5
    output = args.output or args.layout_study.parent
    output.mkdir(parents=True, exist_ok=True)
    try:
        import matplotlib  # noqa: F401
    except ImportError:
        renderer = plot_cairo(rows, output, ymin, ymax, sessions)
    else:
        renderer = plot_matplotlib(rows, output, ymin, ymax, sessions)
    print(f"Wrote {len(rows)} independent launch scores to {output} ({renderer['renderer']})")


if __name__ == "__main__":
    main()
