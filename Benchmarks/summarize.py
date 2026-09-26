"""Summarise SourceControlBenchmark results.csv files into a Markdown table.

Usage: python summarize.py results.csv [more.csv ...]

Prints, per operation, the median milliseconds for each provider (and the sample count), in the provider order
GitLink, Git LFS 2, Git. Failed samples (ok=0) are excluded and counted separately.
"""
import csv
import statistics
import sys
from collections import defaultdict

OPERATIONS = [
    ("Init", "Init"),
    ("Connect.Cold", "Connect (cold)"),
    ("Connect.Warm", "Connect (warm)"),
    ("Status.100Files", "Status, 100 files"),
    ("Status.1000Files", "Status, 1000 files"),
    ("Status.Refresh", "Status, refresh"),
    ("MarkForAdd.100", "Mark for add, 100"),
    ("Revert.100", "Revert, 100"),
]
PROVIDER_ORDER = ["GitLink", "Git LFS 2", "Git"]


def main(paths):
    samples = defaultdict(list)   # (provider, op) -> [ms]
    failures = defaultdict(int)
    for path in paths:
        with open(path, newline="", encoding="utf-8") as f:
            for row in csv.DictReader(f):
                key = (row["provider"], row["operation"])
                if row["ok"] == "1":
                    samples[key].append(float(row["ms"]))
                else:
                    failures[key] += 1

    providers = [p for p in PROVIDER_ORDER if any(k[0] == p for k in samples)] + sorted(
        {k[0] for k in samples} - set(PROVIDER_ORDER))

    def fmt(ms):
        return f"{ms / 1000:.2f} s" if ms >= 1000 else f"{ms:.0f} ms"

    print("| Operation | " + " | ".join(providers) + " |")
    print("|---|" + "---:|" * len(providers))
    for op, label in OPERATIONS:
        cells = []
        for p in providers:
            values = samples.get((p, op), [])
            fails = failures.get((p, op), 0)
            if not values:
                cells.append("failed" if fails else "—")
                continue
            cell = f"{fmt(statistics.median(values))} (n={len(values)})"
            if fails:
                cell += f", {fails} failed"
            cells.append(cell)
        print(f"| {label} | " + " | ".join(cells) + " |")


if __name__ == "__main__":
    main(sys.argv[1:])
