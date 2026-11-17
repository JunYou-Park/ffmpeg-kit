/* Byte-segment counting I/O. LGPL-2.1-or-later. */
#ifndef AVFORMAT_BYTE_SEGMENT_IO_H
#define AVFORMAT_BYTE_SEGMENT_IO_H

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include "avio.h"
#include "libavutil/error.h"

typedef struct ByteSegmentCounter {
    int64_t position;
    int64_t size;
} ByteSegmentCounter;

/* Match a seekable dynamic buffer, including overwrites of MP4 atom headers. */
static int byte_segment_count_write(void *opaque, uint8_t *data, int size)
{
    ByteSegmentCounter *counter = opaque;
    (void)data;
    if (size < 0)
        return AVERROR(EINVAL);
    if (size > INT_MAX - counter->position)
        return AVERROR(EFBIG);
    counter->position += size;
    if (counter->position > counter->size)
        counter->size = counter->position;
    return size;
}

static int64_t byte_segment_count_seek(void *opaque, int64_t offset, int whence)
{
    ByteSegmentCounter *counter = opaque;
    int64_t base, position;
    whence &= ~AVSEEK_FORCE;
    if (whence == AVSEEK_SIZE)
        return counter->size;
    switch (whence) {
    case SEEK_SET: base = 0; break;
    case SEEK_CUR: base = counter->position; break;
    case SEEK_END: base = counter->size; break;
    default: return AVERROR(EINVAL);
    }
    if ((offset > 0 && base > INT64_MAX - offset) ||
        (offset < 0 && base < INT64_MIN - offset))
        return AVERROR(EINVAL);
    position = base + offset;
    if (position < 0 || position > INT_MAX)
        return AVERROR(EINVAL);
    counter->position = position;
    return position;
}

#endif
