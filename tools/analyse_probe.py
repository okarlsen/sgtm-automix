#!/usr/bin/env python3
"""Summarise SGTM Host Probe logs.

Usage:
    python3 analyse_probe.py [LOG_DIR] [--last-minutes N] [--pid PID]

LOG_DIR defaults to ~/Library/Logs/SGTM Host Probe (macOS). With no --pid, every host process
found in the selected logs is reported separately.

For each host process it prints:
  * per instance: track name, wrapper, blocks, threads, block sizes, offline blocks, timeline jumps
  * how the host interleaves instances (realtime and offline separately): average run of
    consecutive blocks from one instance, and how many threads did the work
  * timeline skew between each pair of instances: when instance A finishes a block, how far
    ahead (+) or behind (-) instance B's timeline position is, in samples and milliseconds.
    This is the number the instance link (thread 3) has to cope with.
"""

import argparse
import csv
import glob
import os
import statistics
import sys
import time
from collections import defaultdict


def load(path):
    info = {"path": path, "blocks": [], "events": [], "track": "", "host": "", "wrapper": "", "pid": ""}
    with open(path, newline="") as f:
        rows = [line for line in f if not line.startswith("#")]
    for row in csv.DictReader(rows):
        if row["kind"] == "block":
            info["blocks"].append({
                "wall": int(row["wall_ns"]),
                "thread": row["thread"],
                "pos": int(row["timeline_samples"]) if row["has_position"] == "1" and int(row["timeline_samples"]) >= 0 else None,
                "local": int(row["local_samples"]),
                "n": int(row["block_size"]),
                "sr": float(row["sample_rate"] or 0),
                "offline": row["non_realtime"] == "1",
                "playing": row["playing"] == "1",
                "dropped": int(row["dropped_total"] or 0),
            })
        else:
            info["events"].append((row["kind"], row["info"]))
            if row["kind"] == "track":
                info["track"] = row["info"]
            if row["kind"] == "created":
                for part in row["info"].split():
                    key, _, value = part.partition("=")
                    if key in ("host", "wrapper", "pid"):
                        info[key] = value
    info["id"] = os.path.basename(path).rsplit("_", 1)[-1].removesuffix(".csv")[:6]
    return info


def position(b):
    """Timeline position, or None while the transport is stopped (hosts then repeat one position)."""
    return b["pos"] if (b["playing"] or b["offline"]) else None


def describe_instance(inst):
    blocks = inst["blocks"]
    sizes = [b["n"] for b in blocks]
    jumps = sum(1 for a, b in zip(blocks, blocks[1:])
                if None not in (position(a), position(b)) and position(b) != position(a) + a["n"])
    stopped = sum(1 for b in blocks if not (b["playing"] or b["offline"]))
    offline = sum(1 for b in blocks if b["offline"])
    threads = {b["thread"] for b in blocks}
    no_pos = sum(1 for b in blocks if b["pos"] is None)
    dropped = blocks[-1]["dropped"] if blocks else 0
    name = inst["track"] or "(no track name)"
    print(f"  [{inst['id']}] {name}: {inst['wrapper']}, {len(blocks)} blocks, "
          f"{len(threads)} thread(s), block size {min(sizes, default=0)}-{max(sizes, default=0)}, "
          f"{offline} offline, {stopped} with transport stopped, {jumps} timeline jumps while running"
          + (f", {no_pos} without timeline position" if no_pos else "")
          + (f", {dropped} DROPPED" if dropped else ""))


def scheduling(instances, offline):
    events = sorted(
        ((b["wall"], inst["id"], b["thread"]) for inst in instances for b in inst["blocks"] if b["offline"] == offline),
        key=lambda e: e[0])
    if not events:
        return
    runs, current = [], 1
    for a, b in zip(events, events[1:]):
        if a[1] == b[1]:
            current += 1
        else:
            runs.append(current)
            current = 1
    runs.append(current)
    threads = {e[2] for e in events}
    label = "offline" if offline else "realtime"
    print(f"  {label}: {len(events)} blocks on {len(threads)} thread(s); "
          f"average run of consecutive blocks from one instance: {statistics.mean(runs):.1f} "
          f"(longest {max(runs)})")


def skew(instances, offline):
    label = "offline" if offline else "realtime"
    for a in instances:
        for b in instances:
            if a is b:
                continue
            a_blocks = [x for x in a["blocks"] if x["offline"] == offline and position(x) is not None]
            b_blocks = [x for x in b["blocks"] if x["offline"] == offline and position(x) is not None]
            if not a_blocks or not b_blocks:
                continue
            sr = a_blocks[0]["sr"] or 48000.0
            diffs, j = [], 0
            for x in a_blocks:
                # Latest block B finished at or before A's block started.
                while j + 1 < len(b_blocks) and b_blocks[j + 1]["wall"] <= x["wall"]:
                    j += 1
                if b_blocks[j]["wall"] > x["wall"]:
                    continue
                y = b_blocks[j]
                diffs.append((position(y) + y["n"]) - (position(x) + x["n"]))
            if not diffs:
                continue
            ms = lambda s: 1000.0 * s / sr
            print(f"  {label} [{b['id']}] relative to [{a['id']}]: "
                  f"min {min(diffs):+d} ({ms(min(diffs)):+.1f} ms), "
                  f"median {int(statistics.median(diffs)):+d} ({ms(statistics.median(diffs)):+.1f} ms), "
                  f"max {max(diffs):+d} ({ms(max(diffs)):+.1f} ms) samples")


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("log_dir", nargs="?", default=os.path.expanduser("~/Library/Logs/SGTM Host Probe"))
    parser.add_argument("--last-minutes", type=float, help="only logs modified in the last N minutes")
    parser.add_argument("--pid", help="only this host process")
    args = parser.parse_args()

    paths = sorted(glob.glob(os.path.join(args.log_dir, "probe_*.csv")))
    if args.last_minutes:
        cutoff = time.time() - 60 * args.last_minutes
        paths = [p for p in paths if os.path.getmtime(p) >= cutoff]
    if not paths:
        sys.exit(f"No probe logs found in {args.log_dir}")

    by_pid = defaultdict(list)
    for p in paths:
        inst = load(p)
        if inst["blocks"] and (args.pid is None or inst["pid"] == args.pid):
            by_pid[inst["pid"]].append(inst)

    for pid, instances in by_pid.items():
        print(f"\nHost process {pid} ({instances[0]['host']}), {len(instances)} instance(s)")
        for inst in instances:
            describe_instance(inst)
        print(" Scheduling")
        scheduling(instances, offline=False)
        scheduling(instances, offline=True)
        print(" Timeline skew (+ = the other instance is ahead)")
        skew(instances, offline=False)
        skew(instances, offline=True)


if __name__ == "__main__":
    main()
