#!/usr/bin/env python3
"""Exercise the patched native muxer; system FFmpeg supplies test fixtures."""
import argparse
import csv
import json
from pathlib import Path
import subprocess
import tempfile


def run(args, success=True):
    result = subprocess.run([str(x) for x in args], capture_output=True, text=True)
    if success and result.returncode:
        raise AssertionError(f"Failed: {args}\n{result.stderr}")
    if not success and result.returncode == 0:
        raise AssertionError(f"Unexpected success: {args}")
    return result


def packets(path, kind):
    value = run(["ffprobe", "-v", "error", "-select_streams", kind,
                 "-show_packets", "-show_data_hash", "sha256",
                 "-show_entries", "packet=data_hash", "-of", "json", path])
    return [p["data_hash"] for p in json.loads(value.stdout)["packets"]]


def frames(path):
    value = run(["ffmpeg", "-v", "error", "-i", path, "-map", "0:v:0",
                 "-fps_mode", "passthrough", "-f", "framemd5", "-"])
    return [line.rsplit(",", 1)[1].strip() for line in value.stdout.splitlines()
            if line and not line.startswith("#")]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("ffmpeg", type=Path, help="FFmpeg 6 executable built with byte_segment")
    args = parser.parse_args()
    native = args.ffmpeg.resolve()
    with tempfile.TemporaryDirectory(prefix="byte-segment-tests-") as temp:
        root = Path(temp)
        source = root / "source.mp4"
        run(["ffmpeg", "-v", "error", "-f", "lavfi", "-i",
             "testsrc2=size=480x270:rate=24", "-f", "lavfi", "-i",
             "sine=frequency=600:sample_rate=22050", "-t", "12",
             "-c:v", "libx264", "-preset", "ultrafast", "-profile:v", "baseline",
             "-pix_fmt", "yuv420p", "-g", "24", "-keyint_min", "24",
             "-sc_threshold", "0", "-bf", "0", "-c:a", "aac", "-ac", "1",
             "-b:a", "32k", source])

        def split(name, input_file=source, limit=350000, more=(), success=True, video_hash=True):
            directory = root / name
            directory.mkdir()
            manifest = directory / "result.csv"
            pattern = directory / 'part-"%03d.mp4'
            run([native, "-v", "error", "-i", input_file, "-map", "0:v:0",
                 "-map", "0:a:0?", "-c", "copy", "-f", "byte_segment",
                 "-byte_limit", str(limit), "-segment_list", manifest,
                 *more, pattern], success=success)
            if not success:
                assert not manifest.exists(), "Failed jobs must not publish a completion manifest"
                return
            with manifest.open() as stream:
                entries = list(csv.DictReader(stream))
            assert entries
            videos, audios, decoded = [], [], []
            last_end = None
            for i, entry in enumerate(entries):
                path = Path(entry["filename"])
                assert int(entry["index"]) == i
                assert path.stat().st_size == int(entry["bytes"]) <= limit
                if last_end is not None:
                    assert int(entry["start_us"]) == last_end
                assert int(entry["end_us"]) > int(entry["start_us"])
                last_end = int(entry["end_us"])
                videos += packets(path, "v:0")
                audios += packets(path, "a:0")
                decoded += frames(path)  # Each file must decode independently.
            if video_hash:
                assert videos == packets(input_file, "v:0"), "Video packet loss/duplication/reordering"
            assert audios == packets(input_file, "a:0"), "Audio packet loss/duplication/reordering"
            assert decoded == frames(input_file), "Decoded frames changed or went missing"
            print(f"PASS: {name}, {len(entries)} files, {len(decoded)} identical decoded frames")
            return entries

        assert len(split("audio-video")) > 1
        split("annex-b", more=("-bsf:v", "h264_mp4toannexb"), video_hash=False)
        split("one-file", limit=4000000)
        silent = root / "silent.mp4"
        run(["ffmpeg", "-v", "error", "-i", source, "-an", "-c:v", "copy", silent])
        split("silent", input_file=silent)
        split("too-small", limit=1024, success=False)
        split("segment-cap", more=("-max_segments", "1"), success=False)

        long_gop = root / "long-gop.mp4"
        run(["ffmpeg", "-v", "error", "-i", source, "-an", "-c:v", "libx264",
             "-preset", "ultrafast", "-g", "1000", "-sc_threshold", "0", "-bf", "0", long_gop])
        split("oversized-gop", input_file=long_gop, success=False)
        bframes = root / "bframes.mp4"
        run(["ffmpeg", "-v", "error", "-i", source, "-an", "-c:v", "libx264",
             "-bf", "2", bframes])
        split("unsupported-bframes", input_file=bframes, success=False)

        # A write error must not be reported as a successful completed segment set.
        bad = root / "write-error"
        bad.mkdir()
        (bad / "part-000.mp4").symlink_to("/dev/full")
        run([native, "-v", "error", "-i", source, "-c", "copy", "-f", "byte_segment",
             "-byte_limit", "350000", "-segment_list", bad / "result.csv",
             bad / "part-%03d.mp4"], success=False)
        assert not (bad / "result.csv").exists()
        print("PASS: tiny budget, file cap, oversized GOP, unsupported B frames, output I/O failure")


if __name__ == "__main__":
    main()
