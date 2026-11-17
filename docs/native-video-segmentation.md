# Experimental native video segmentation

The Android library version on this branch is `6.0-native-video.2` (240602).
The new `byte_segment` muxer measures a complete MP4, including its container
metadata, before publishing it. Encoded packets are retained and remuxed at IDR
boundaries; this operation never decodes or re-encodes the video. The standard
MP4 and segment muxers are unchanged.

## Supported input and limits

- One H.264 video stream, AVCC or Annex B, **without B frames**, and optional AAC.
- Every video segment starts at an IDR. Open-GOP recovery points are not used.
- Valid timestamps and positive packet durations are required.
- Each complete MP4 is at most `byte_limit` (1 KiB–16 MiB).
- `max_segments` is 1–16384, default 1024. Retained encoded packet payload is
  bounded by twice `byte_limit` and 65536 packets. Candidate MP4 buffers, cached
  output, packet structures and muxer state need additional memory.
- Each output has its own MP4 header/trailer and requires no shared init file.
  The moov atom is at the end; faststart is not applied in this implementation.

At each IDR boundary the muxer measures the complete candidate MP4 through
seekable counting I/O. It tracks the highest written position, including header
overwrites, without retaining or copying the MP4 payload into a growing dynamic
output buffer. When a candidate no longer fits, the previous verified boundary
is selected and serialized once. Its real byte size must exactly match the
measured size before publication. This preserves the original boundary choices
and avoids another video encode; container metadata/packet traversal still
repeats during measurement. Packet objects are reused in a ring pool, and common
timestamp offsets are calculated once per stream per render.

If even one GOP cannot fit, the operation fails. It does not drop frames, cut
off the end of the source, or silently reduce quality. The caller can retry
encoding with shorter GOPs or a different quality profile. Frequent IDRs have
a compression-efficiency cost.

Audio packets are preserved once and in order. A common timestamp offset is
applied to both streams. AAC packets may straddle a video boundary; sample-exact
gapless playback and MP4 edit-list handling require receiver-side implementation
and Android validation.

## Invocation through the existing FFmpegKit API

An already encoded compatible video can be split using the existing argument-array
execution API. This path is exercised by the host integration tests:

```sh
ffmpeg -i compatible.mp4 -map 0:v:0 -map 0:a:0? -c copy \
  -f byte_segment -byte_limit 900000 -max_segments 128 \
  -segment_list /private/job/result.csv /private/job/part-%04d.mp4
```

The muxer also accepts Annex B H.264 for use after an encoder in the same job.
On a build with libx264, an example command is:

```sh
ffmpeg -i input.mp4 -map 0:v:0 -map 0:a:0? \
  -vf 'scale=480:-2,fps=24' \
  -c:v libx264 -profile:v baseline -pix_fmt yuv420p -preset veryfast \
  -crf 26 -bf 0 -g 24 -keyint_min 24 -sc_threshold 0 \
  -c:a aac -ac 1 -b:a 32k \
  -f byte_segment -byte_limit 900000 -max_segments 128 \
  -segment_list /private/job/result.csv /private/job/part-%04d.mp4
```

The quality settings are illustrative, not a visual-quality guarantee. The direct
libx264 encode command still needs a full Android build/device test. The caller
must subtract MMS headers, text and grouping metadata before supplying the video
byte limit. The muxer has no MMS/carrier-specific logic.

## Completion, metadata and ownership

Use a new empty, private directory per job. The output template must have a
numeric `%d` placeholder. The caller owns all files.

The optional CSV `segment_list` is written after all segment writes succeed.
Columns are `index,filename,bytes,start_us,end_us`, with CSV-escaped filenames.
Times are video boundaries on the timeline entering the muxer, which may have
been shifted by FFmpeg timestamp options. The final end time is the end of the
last video packet.

**Consume outputs only after a successful FFmpeg session.** Failure, cancellation,
file-count exhaustion, I/O error or process termination can leave partial files.
Discard the entire job directory in those cases. A failed manifest write can
leave a partial manifest; its presence alone is not proof of success. Segment
file-close errors are checked before accepting a segment.

No new JNI/AIDL method is introduced. MediaProcessingService must later add a
capability, job lifetime management and a multiple-result transfer contract before
Messaging can use the feature. This branch does not enable MMS sending.

## Native callback queue

The Android JNI callback delivery queue is bounded to 1024 waiting records.
Logs are limited to 4096 bytes per record and may be dropped during overload.
Pending statistics for the same session are coalesced to the latest snapshot.
Allocation failures are checked. The in-transit counter increments before a
record becomes visible to the consumer. JNI allocation/callback exceptions on
the attached callback thread are cleared after logging so delivery can continue.
These limits apply to the native queue, not the Java session log history.

## Building and verification

The Android FFmpeg build runs `tools/apply-native-video-patches.py` before
configure. Python 3 is required. The script is idempotent and requires
libavformat major 60 (the repository pins FFmpeg 6). It installs the new muxer
and its registration/build dependencies. Custom component-disable options may
exclude it. The build no longer checks out `ffmpegkit.c` over local modifications.

For host verification, apply the same source to a checkout of the pinned FFmpeg:

```sh
python3 tools/apply-native-video-patches.py /path/to/FFmpeg
```

Configure/build in a separate directory. `--disable-asm` avoids incompatibilities
between the old FFmpeg release and newer host assemblers:

```sh
/path/to/FFmpeg/configure --disable-everything --disable-doc --disable-asm \
  --enable-protocol=file --enable-demuxer=mov \
  --enable-muxer=byte_segment,mp4 --enable-decoder=h264,aac \
  --enable-parser=h264,aac --enable-bsf=h264_mp4toannexb \
  --enable-ffmpeg --enable-ffprobe
make -j4
```

From this repository, with system FFmpeg/FFprobe for fixtures and independent
decoding, run:

```sh
python3 tests/test_byte_segments.py /path/to/host-build/ffmpeg
python3 tests/test_byte_segments.py /path/to/host-build/ffmpeg --baseline /path/to/old/ffmpeg
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -I /path/to/host-build -I /path/to/FFmpeg \
  -I /path/to/FFmpeg/libavformat -I tools/patch/ffmpeg \
  tests/native_byte_counter_test.c -o /tmp/byte-counter-test
/tmp/byte-counter-test
cc -std=c11 -Wall -Wextra -Werror -g -fsanitize=address,undefined \
  -I android/ffmpeg-kit-android-lib/src/main/cpp \
  -I /path/to/host-build -I /path/to/FFmpeg \
  tests/native_callback_queue_test.c \
  android/ffmpeg-kit-android-lib/src/main/cpp/ffmpegkit_callback_queue.c \
  /path/to/host-build/libavutil/libavutil.a -pthread -lm -o /tmp/callback-test
/tmp/callback-test
```

Tests check complete-file sizes, independent decoding, identical decoded frames,
video/audio packet preservation, metadata ordering, silent video, Annex B,
single-file output, tiny budgets, oversized GOPs, B-frame rejection, output-count
caps, write failures, callback overload, progress coalescing, allocation failure
and concurrent accounting. Baseline comparison additionally checks byte-identical
MP4 files and unchanged segment boundaries, including exact-byte limits and a
limit one byte smaller. Counting I/O tests cover backwards overwrites, forwards
seeks and overflow. A variable-complexity fixture checks packet-pool wrap/growth.
On hosts where LeakSanitizer cannot run under the
execution supervisor, `ASAN_OPTIONS=detect_leaks=0` permits AddressSanitizer and
UndefinedBehaviorSanitizer checks; that does not constitute a leak check.

Host verification does not replace Android ABI/AAR builds or real-device
Service/Messaging integration tests. The Android SDK/NDK is needed for those.

For matched native-only performance measurements on Linux, the benchmark alternates
old/new binaries with identical inputs/limits, verifies output layouts and packet
counts outside the timed interval, and reports wall time, CPU time and peak RSS:

```sh
python3 tests/benchmark_byte_segments.py /path/to/old/ffmpeg \
  /path/to/new/ffmpeg /path/to/compatible-five-minute.mp4 --runs 5 --warmups 1
```

These measurements exclude video encoding and MMS. They must not be interpreted
as Android device speedups or end-to-end video-processing results.

The matched Linux host comparison against commit `8faf920` used the same
301.115-second, 46,822,197-byte fixture (7200 video and 6476 audio packets), one
warmup and five alternating timed runs per binary/limit. Every output set had
identical segment sizes and timeline boundaries, and preserved packet counts.
Values below are medians; RSS is measured in KiB:

| Byte limit | Wall time, old → new | CPU time, old → new | Peak RSS, old → new |
| ---: | ---: | ---: | ---: |
| 350000 | 0.3435 → 0.3109 s | 0.4082 → 0.4094 s | 15616 → 15616 |
| 900000 | 0.4061 → 0.3272 s | 0.4627 → 0.4188 s | 15616 → 15616 |
| 4000000 | 0.5054 → 0.3028 s | 0.5816 → 0.3875 s | 22480 → 18248 |

The clearest improvement was the 4 MB case: about 40% lower wall time, 33% lower
CPU time and 19% lower peak RSS. The smaller cases had overlapping timing ranges;
350 KB showed no CPU reduction. These are workload-specific native remux results,
not a general speedup guarantee. Byte-identical MP4 comparison was also checked
on the separate correctness fixtures, including exact-byte limits and ring growth.
The new muxer and counting I/O were additionally compiled with AddressSanitizer
and UndefinedBehaviorSanitizer and exercised through those correctness fixtures
(leak detection disabled on this host).

The matched Linux host comparison against commit `8faf920` used the same
301.115-second, 46,822,197-byte fixture (7200 video and 6476 audio packets), one
warmup and five alternating timed runs per binary/limit. Every output set had
identical segment sizes and timeline boundaries, and preserved packet counts.
Values below are medians; RSS is measured in KiB:

| Byte limit | Wall time, old → new | CPU time, old → new | Peak RSS, old → new |
| ---: | ---: | ---: | ---: |
| 350000 | 0.3435 → 0.3109 s | 0.4082 → 0.4094 s | 15616 → 15616 |
| 900000 | 0.4061 → 0.3272 s | 0.4627 → 0.4188 s | 15616 → 15616 |
| 4000000 | 0.5054 → 0.3028 s | 0.5816 → 0.3875 s | 22480 → 18248 |

The clearest improvement was the 4 MB case: about 40% lower wall time, 33% lower
CPU time and 19% lower peak RSS. The smaller cases had overlapping timing ranges;
350 KB showed no CPU reduction. These are workload-specific native remux results,
not a general speedup guarantee. Byte-identical MP4 comparison was also checked
on the separate correctness fixtures, including exact-byte limits and ring growth.
