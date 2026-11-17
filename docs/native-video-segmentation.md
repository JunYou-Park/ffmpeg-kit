# Experimental native video segmentation

The Android library version on this branch is `6.0-native-video.1` (240601).
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

At each IDR boundary the muxer renders the buffered candidate MP4 in memory.
When it no longer fits, the previous verified candidate is published, and the
remaining GOP is measured as the beginning of the next segment. This avoids
another video encode, but it does repeat container muxing. Measure performance
on the target device before treating it as a speed improvement.

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
and concurrent accounting. On hosts where LeakSanitizer cannot run under the
execution supervisor, `ASAN_OPTIONS=detect_leaks=0` permits AddressSanitizer and
UndefinedBehaviorSanitizer checks; that does not constitute a leak check.

Host verification does not replace Android ABI/AAR builds or real-device
Service/Messaging integration tests. The Android SDK/NDK is needed for those.
