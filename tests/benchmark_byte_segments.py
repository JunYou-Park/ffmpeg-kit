#!/usr/bin/env python3
"""Compare two byte_segment binaries with matched inputs and validated outputs.

The timed interval is native packet remuxing and file publication only. Fixture
creation, output verification, packet counting and cleanup are outside timings.
Run on the same Linux host; these results do not measure video encoding or MMS.
"""
import argparse
import csv
import hashlib
import json
import os
from pathlib import Path
import random
import statistics
import subprocess
import sys
import tempfile
import time


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def probe_counts(ffprobe, source, concat=False):
    args = [ffprobe, "-v", "error"]
    if concat:
        args += ["-f", "concat", "-safe", "0"]
    args += ["-i", str(source), "-count_packets", "-show_entries",
             "stream=codec_type,codec_name,nb_read_packets", "-of", "json"]
    result = subprocess.run(args, capture_output=True, text=True, check=True)
    streams = json.loads(result.stdout)["streams"]
    counts = {}
    for stream in streams:
        kind = stream["codec_type"]
        if kind in counts:
            raise AssertionError(f"Expected at most one {kind} stream")
        counts[kind] = {"codec": stream["codec_name"],
                        "packets": int(stream["nb_read_packets"])}
    return counts


def validate_outputs(directory, byte_limit, ffprobe, expected_counts):
    with (directory / "result.csv").open(newline="") as stream:
        entries = list(csv.DictReader(stream))
    if not entries:
        raise AssertionError("No completed output segments")
    signature, paths = [], []
    previous_end = None
    for index, entry in enumerate(entries):
        path = Path(entry["filename"])
        size = int(entry["bytes"])
        start, end = int(entry["start_us"]), int(entry["end_us"])
        if int(entry["index"]) != index or path.parent != directory:
            raise AssertionError("Unexpected output index or path")
        if not 0 < path.stat().st_size == size <= byte_limit:
            raise AssertionError(f"Completed output exceeds byte limit: {path}")
        if end <= start or (previous_end is not None and start != previous_end):
            raise AssertionError("Noncontiguous or empty video timeline")
        previous_end = end
        signature.append((size, start, end))
        paths.append(path)
    if set(directory.glob("part-*.mp4")) != set(paths):
        raise AssertionError("Output files differ from the completion manifest")
    # One ffprobe process traverses the ordered files without decoding them.
    # Counting the input and every completed set also catches silent truncation.
    concat = directory / "verify.ffconcat"
    with concat.open("w") as stream:
        stream.write("ffconcat version 1.0\n")
        for path in paths:
            escaped = str(path).replace("'", "'\\''")
            stream.write(f"file '{escaped}'\n")
    counts = probe_counts(ffprobe, concat, concat=True)
    if counts != expected_counts:
        raise AssertionError(f"Packet counts changed: {counts} != {expected_counts}")
    return signature


def trial(binary, source, byte_limit, ffprobe, expected_counts, parent):
    with tempfile.TemporaryDirectory(prefix="trial-", dir=parent) as temp:
        directory = Path(temp)
        args = [str(binary), "-v", "error", "-i", str(source),
                "-map", "0:v:0", "-map", "0:a:0?", "-c", "copy",
                "-f", "byte_segment", "-byte_limit", str(byte_limit),
                "-segment_list", str(directory / "result.csv"),
                str(directory / "part-%04d.mp4")]
        with (directory / "stderr.log").open("wb") as error:
            start = time.perf_counter()
            process = subprocess.Popen(args, stdout=subprocess.DEVNULL, stderr=error)
            try:
                _, status, usage = os.wait4(process.pid, 0)
            except BaseException:
                process.kill()
                process.wait()
                raise
            elapsed = time.perf_counter() - start
            process.returncode = os.waitstatus_to_exitcode(status)
        if process.returncode:
            raise RuntimeError(f"{binary} failed ({process.returncode}):\n"
                               f"{(directory / 'stderr.log').read_text()}")
        signature = validate_outputs(directory, byte_limit, ffprobe, expected_counts)
        return {"wall_seconds": elapsed,
                "cpu_seconds": usage.ru_utime + usage.ru_stime,
                "peak_rss_kib": usage.ru_maxrss}, signature


def summarize(samples):
    return {"runs": len(samples),
            "median_wall_seconds": statistics.median(x["wall_seconds"] for x in samples),
            "median_cpu_seconds": statistics.median(x["cpu_seconds"] for x in samples),
            "median_peak_rss_kib": statistics.median(x["peak_rss_kib"] for x in samples),
            "wall_seconds_range": [min(x["wall_seconds"] for x in samples),
                                   max(x["wall_seconds"] for x in samples)]}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("baseline", type=Path)
    parser.add_argument("candidate", type=Path)
    parser.add_argument("source", type=Path, help="Compatible H.264/AAC MP4 fixture")
    parser.add_argument("--byte-limits", type=int, nargs="+", default=[350000, 900000, 4000000])
    parser.add_argument("--runs", type=int, default=5)
    parser.add_argument("--warmups", type=int, default=1)
    parser.add_argument("--seed", type=int, default=0)
    parser.add_argument("--ffprobe", default="ffprobe")
    parser.add_argument("--work-dir", type=Path, help="Parent directory for temporary outputs")
    args = parser.parse_args()
    if not sys.platform.startswith("linux") or not hasattr(os, "wait4"):
        parser.error("This benchmark uses Linux wait4 CPU/RSS accounting")
    if args.runs < 1 or args.warmups < 0 or not all(1024 <= n <= 16 * 1024 * 1024 for n in args.byte_limits):
        parser.error("Use positive runs, nonnegative warmups and supported byte limits")
    limits = list(dict.fromkeys(args.byte_limits))
    binaries = {"baseline": args.baseline.resolve(), "candidate": args.candidate.resolve()}
    source = args.source.resolve()
    binary_hashes = {name: sha256(path) for name, path in binaries.items()}
    source_hash = sha256(source)
    expected_counts = probe_counts(args.ffprobe, source)
    if "video" not in expected_counts:
        parser.error("A compatible video stream is required")
    data = {limit: {name: [] for name in binaries} for limit in limits}
    signatures = {}
    execution_order = []
    with tempfile.TemporaryDirectory(prefix="byte-segment-benchmark-", dir=args.work_dir) as temp:
        parent = Path(temp)
        for iteration in range(-args.warmups, args.runs):
            case_order = limits.copy()
            random.Random(args.seed + iteration).shuffle(case_order)
            for limit in case_order:
                names = ["baseline", "candidate"]
                if (iteration + limits.index(limit)) % 2:
                    names.reverse()
                for name in names:
                    sample, signature = trial(binaries[name], source, limit, args.ffprobe,
                                              expected_counts, parent)
                    if limit in signatures and signatures[limit] != signature:
                        raise AssertionError(f"Completed segment layout changed at limit {limit}")
                    signatures[limit] = signature
                    if iteration >= 0:
                        data[limit][name].append(sample)
                        execution_order.append({"iteration": iteration, "byte_limit": limit,
                                                "binary": name})
    if sha256(source) != source_hash or any(sha256(path) != binary_hashes[name]
                                           for name, path in binaries.items()):
        raise RuntimeError("A binary or input file changed during the benchmark")
    result = {"scope": "synthetic Linux host native remux only; excludes encoding and MMS",
              "source": {"path": str(source), "bytes": source.stat().st_size,
                         "sha256": source_hash, "streams": expected_counts},
              "binaries": {name: {"path": str(path), "sha256": binary_hashes[name]}
                           for name, path in binaries.items()},
              "runs": args.runs, "warmups": args.warmups, "seed": args.seed,
              "execution_order": execution_order,
              "cases": []}
    for limit in limits:
        baseline, candidate = (summarize(data[limit][name]) for name in binaries)
        result["cases"].append({"byte_limit": limit, "segments": len(signatures[limit]),
                                "baseline": baseline, "candidate": candidate,
                                "baseline_over_candidate_wall_ratio":
                                    baseline["median_wall_seconds"] / candidate["median_wall_seconds"],
                                "baseline_over_candidate_cpu_ratio":
                                    baseline["median_cpu_seconds"] / candidate["median_cpu_seconds"],
                                "samples": data[limit]})
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
