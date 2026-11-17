#!/usr/bin/env python3
"""Install the opt-in muxer into the pinned FFmpeg 6 source, idempotently."""
import argparse
from pathlib import Path
import re
import shutil


def insert(path, marker, anchor, text):
    content = path.read_text()
    if marker in content:
        return
    if content.count(anchor) != 1:
        raise SystemExit(f"Unsupported FFmpeg source: missing unique anchor in {path}")
    path.write_text(content.replace(anchor, text + anchor))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("source", type=Path)
    args = parser.parse_args()
    source = args.source.resolve()
    version = (source / "libavformat/version_major.h").read_text()
    if not re.search(r"#define LIBAVFORMAT_VERSION_MAJOR\s+60\b", version):
        raise SystemExit("The native byte_segment patch requires FFmpeg 6/libavformat 60")
    insert(source / "configure", 'byte_segment_muxer_select=',
           'adts_muxer_select="mpeg4audio"', 'byte_segment_muxer_select="mp4_muxer"\n')
    insert(source / "libavformat/Makefile", 'CONFIG_BYTE_SEGMENT_MUXER',
           'OBJS-$(CONFIG_SEGMENT_MUXER)', 'OBJS-$(CONFIG_BYTE_SEGMENT_MUXER)        += byte_segment.o\n')
    insert(source / "libavformat/allformats.c", 'ff_byte_segment_muxer;',
           'extern const FFOutputFormat ff_segment_muxer;',
           'extern const FFOutputFormat ff_byte_segment_muxer;\n')
    shutil.copyfile(Path(__file__).resolve().parent / "patch/ffmpeg/byte_segment.c",
                    source / "libavformat/byte_segment.c")
    print("Installed native byte_segment muxer")


if __name__ == "__main__":
    main()
