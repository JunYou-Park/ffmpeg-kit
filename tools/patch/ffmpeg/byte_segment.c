/*
 * Copyright (c) 2026 FFmpegKit contributors
 *
 * This file is part of FFmpeg and is licensed under the GNU Lesser General
 * Public License, version 2.1 or later. See FFmpeg's COPYING.LGPLv2.1.
 */

/**
 * Experimental byte-bounded, independently playable MP4 segments.
 * Encoded packets are retained between IDRs and remuxed, never re-encoded.
 * A completed MP4, including its moov atom, is measured before publication.
 * Deliberately restricted to H.264 without B frames and optional AAC.
 */
#include "avformat.h"
#include "internal.h"
#include "mux.h"
#include "byte_segment_io.h"
#include "libavutil/avstring.h"
#include "libavutil/intreadwrite.h"
#include "libavutil/opt.h"
#include "libavutil/timestamp.h"

#define MAX_PACKETS 65536
#define MAX_SEGMENT_BYTES (16 * 1024 * 1024)

typedef struct ByteSegmentEntry {
    int64_t start_us, end_us;
    int bytes;
} ByteSegmentEntry;

typedef struct ByteSegmentContext {
    const AVClass *class;
    int64_t byte_limit;
    int max_segments;
    char *list;
    int video_index, nal_length_size;
    AVPacket **packets;
    int packet_count, packet_capacity, packet_head;
    int64_t packet_bytes;
    AVPacket *render_packet;
    int best_bytes, best_count;
    ByteSegmentEntry *entries;
    int entry_count;
    int64_t start_us, best_end_us;
    int64_t last_dts[2];
    int have_idr, failed;
} ByteSegmentContext;

static AVPacket *packet_at(const ByteSegmentContext *b, int index)
{
    return b->packets[(b->packet_head + index) & (b->packet_capacity - 1)];
}

static int interrupted(AVFormatContext *s)
{
    return s->interrupt_callback.callback &&
           s->interrupt_callback.callback(s->interrupt_callback.opaque);
}

/* Key flags alone can also mark recovery points in an open GOP. Require IDR. */
static int is_idr(const ByteSegmentContext *b, const AVPacket *pkt)
{
    size_t pos = 0;
    if (!b->nal_length_size) {
        /* x264's global headers and packets may still be Annex B before muxing. */
        for (pos = 0; pos + 3 < pkt->size; pos++)
            if (pkt->data[pos] == 0 && pkt->data[pos + 1] == 0 &&
                pkt->data[pos + 2] == 1 && (pkt->data[pos + 3] & 31) == 5)
                return 1;
        return 0;
    }
    while (pos + b->nal_length_size <= pkt->size) {
        unsigned size = 0;
        int i;
        for (i = 0; i < b->nal_length_size; i++)
            size = (size << 8) | pkt->data[pos++];
        if (!size || size > pkt->size - pos)
            return AVERROR_INVALIDDATA;
        if ((pkt->data[pos] & 31) == 5)
            return 1;
        pos += size;
    }
    return pos == pkt->size ? 0 : AVERROR_INVALIDDATA;
}

/* data == NULL measures a complete MP4 without retaining its encoded bytes. */
static int render(AVFormatContext *s, int count, uint8_t **data)
{
    ByteSegmentContext *b = s->priv_data;
    AVFormatContext *oc = NULL;
    AVPacket *copy = b->render_packet;
    ByteSegmentCounter counter = {0};
    int64_t offsets[2];
    uint8_t *discard = NULL;
    int ret, bytes, i;

    if (data)
        *data = NULL;
    if ((ret = avformat_alloc_output_context2(&oc, NULL, "mp4", NULL)) < 0)
        return ret;
    oc->interrupt_callback = s->interrupt_callback;
    /* Preserve a common A/V offset; do not independently zero each stream. */
    oc->avoid_negative_ts = AVFMT_AVOID_NEG_TS_DISABLED;
    for (i = 0; i < s->nb_streams; i++) {
        AVStream *st = ff_stream_clone(oc, s->streams[i]);
        if (!st) {
            ret = AVERROR(ENOMEM);
            goto end;
        }
        st->codecpar->codec_tag = 0;
        offsets[i] = av_rescale_q(b->start_us, AV_TIME_BASE_Q, s->streams[i]->time_base);
        if (offsets[i] == AV_NOPTS_VALUE) {
            ret = AVERROR(ERANGE);
            goto end;
        }
    }
    if (data) {
        if ((ret = avio_open_dyn_buf(&oc->pb)) < 0)
            goto end;
    } else {
        uint8_t *buffer = av_malloc(4096);
        if (!buffer) {
            ret = AVERROR(ENOMEM);
            goto end;
        }
        oc->pb = avio_alloc_context(buffer, 4096, 1, &counter, NULL,
                                   byte_segment_count_write, byte_segment_count_seek);
        if (!oc->pb) {
            av_free(buffer);
            ret = AVERROR(ENOMEM);
            goto end;
        }
        oc->pb->seekable = AVIO_SEEKABLE_NORMAL;
        /* Payload writes need only update a length; avoid staging their bytes. */
        oc->pb->direct = 1;
    }
    if ((ret = avformat_write_header(oc, NULL)) < 0)
        goto end;
    for (i = 0; i < count; i++) {
        AVPacket *pkt = packet_at(b, i);
        int64_t offset = offsets[pkt->stream_index];
        if (interrupted(s)) {
            ret = AVERROR_EXIT;
            goto end;
        }
        if ((ret = av_packet_ref(copy, pkt)) < 0)
            goto end;
        if (offset == AV_NOPTS_VALUE ||
            (offset > 0 && (copy->pts < INT64_MIN + offset || copy->dts < INT64_MIN + offset)) ||
            (offset < 0 && (copy->pts > INT64_MAX + offset || copy->dts > INT64_MAX + offset))) {
            ret = AVERROR(ERANGE);
            goto end;
        }
        copy->pts -= offset;
        copy->dts -= offset;
        av_packet_rescale_ts(copy, s->streams[pkt->stream_index]->time_base,
                            oc->streams[pkt->stream_index]->time_base);
        ret = av_interleaved_write_frame(oc, copy);
        if (ret < 0)
            goto end;
    }
    if ((ret = av_write_trailer(oc)) < 0)
        goto end;
    avio_flush(oc->pb);
    if ((ret = oc->pb->error) < 0)
        goto end;
    if (data) {
        bytes = avio_close_dyn_buf(oc->pb, data);
        oc->pb = NULL;
        ret = bytes;
    } else {
        ret = (int)counter.size;
    }
end:
    av_packet_unref(copy);
    if (oc && oc->pb) {
        if (data) {
            avio_close_dyn_buf(oc->pb, &discard);
            av_free(discard);
        } else {
            av_freep(&oc->pb->buffer);
            avio_context_free(&oc->pb);
        }
    }
    avformat_free_context(oc);
    return ret;
}

static int output_name(AVFormatContext *s, int index, char *name, int size)
{
    return av_get_frame_filename(name, size, s->url, index);
}

static int publish(AVFormatContext *s)
{
    ByteSegmentContext *b = s->priv_data;
    AVIOContext *pb = NULL;
    uint8_t *data = NULL;
    char name[1024];
    int ret, close_ret, i;
    if (!b->best_count || b->entry_count >= b->max_segments)
        return AVERROR(EFBIG);
    if (interrupted(s))
        return AVERROR_EXIT;
    if ((ret = output_name(s, b->entry_count, name, sizeof(name))) < 0)
        return ret;
    if (b->list && !strcmp(name, b->list))
        return AVERROR(EINVAL);
    ret = render(s, b->best_count, &data);
    if (ret <= 0 || ret != b->best_bytes || ret > b->byte_limit) {
        av_free(data);
        return ret < 0 ? ret : AVERROR_INVALIDDATA;
    }
    if ((ret = s->io_open(s, &pb, name, AVIO_FLAG_WRITE, NULL)) < 0) {
        av_free(data);
        return ret;
    }
    avio_write(pb, data, b->best_bytes);
    avio_flush(pb);
    ret = pb->error;
    close_ret = ff_format_io_close(s, &pb);
    if (ret >= 0)
        ret = close_ret;
    av_free(data);
    if (ret < 0)
        return ret;
    b->entries[b->entry_count++] = (ByteSegmentEntry) {
        b->start_us, b->best_end_us, b->best_bytes
    };
    av_log(s, AV_LOG_VERBOSE, "byte segment %d: %d bytes, %"PRId64"..%"PRId64" us\n",
           b->entry_count - 1, b->best_bytes, b->start_us, b->best_end_us);
    for (i = 0; i < b->best_count; i++) {
        AVPacket *pkt = packet_at(b, i);
        b->packet_bytes -= pkt->size;
        av_packet_unref(pkt);
    }
    b->packet_count -= b->best_count;
    b->packet_head = (b->packet_head + b->best_count) & (b->packet_capacity - 1);
    b->start_us = b->best_end_us;
    b->best_count = b->best_bytes = 0;
    return 0;
}

/* Called only at a closed video GOP boundary or EOF. */
static int checkpoint(AVFormatContext *s, int64_t end_us)
{
    ByteSegmentContext *b = s->priv_data;
    int bytes, ret;
    if (!b->packet_count)
        return 0;
    bytes = render(s, b->packet_count, NULL);
    if (bytes < 0)
        return bytes;
    if (bytes <= 0) {
        return AVERROR_INVALIDDATA;
    }
    if (bytes > b->byte_limit) {
        if (!b->best_count) {
            av_log(s, AV_LOG_ERROR, "One closed GOP exceeds byte_limit; use shorter GOPs or a different quality setting.\n");
            return AVERROR(EFBIG);
        }
        if ((ret = publish(s)) < 0)
            return ret;
        bytes = render(s, b->packet_count, NULL);
        if (bytes < 0)
            return bytes;
        if (bytes <= 0 || bytes > b->byte_limit) {
            return AVERROR(EFBIG);
        }
    }
    b->best_bytes = bytes;
    b->best_count = b->packet_count;
    b->best_end_us = end_us;
    return 0;
}

static int byte_segment_init(AVFormatContext *s)
{
    ByteSegmentContext *b = s->priv_data;
    char name[1024];
    int i;
    b->video_index = -1;
    if (s->nb_streams < 1 || s->nb_streams > 2 ||
        output_name(s, 0, name, sizeof(name)) < 0)
        return AVERROR(EINVAL);
    for (i = 0; i < s->nb_streams; i++) {
        AVCodecParameters *p = s->streams[i]->codecpar;
        if (!p->extradata || !p->extradata_size)
            return AVERROR(EINVAL);
        if (p->codec_type == AVMEDIA_TYPE_VIDEO && p->codec_id == AV_CODEC_ID_H264 &&
            b->video_index < 0 && !p->video_delay && p->extradata_size >= 7) {
            b->video_index = i;
            if (p->extradata[0] == 1)
                b->nal_length_size = (p->extradata[4] & 3) + 1;
            else if (AV_RB24(p->extradata) != 1 && AV_RB32(p->extradata) != 1)
                return AVERROR_INVALIDDATA;
        } else if (p->codec_type != AVMEDIA_TYPE_AUDIO || p->codec_id != AV_CODEC_ID_AAC) {
            return AVERROR(EINVAL);
        }
        b->last_dts[i] = AV_NOPTS_VALUE;
    }
    if (b->video_index < 0)
        return AVERROR(EINVAL);
    b->entries = av_calloc(b->max_segments, sizeof(*b->entries));
    b->render_packet = av_packet_alloc();
    if (!b->entries || !b->render_packet) {
        av_freep(&b->entries);
        av_packet_free(&b->render_packet);
        return AVERROR(ENOMEM);
    }
    return 0;
}

static int byte_segment_write_packet(AVFormatContext *s, AVPacket *pkt)
{
    ByteSegmentContext *b = s->priv_data;
    AVStream *st;
    int idr = 0, ret;
    int64_t time_us;
    if (b->failed)
        return b->failed;
    if (!pkt || !pkt->data || pkt->size <= 0 ||
        pkt->stream_index < 0 || pkt->stream_index >= s->nb_streams ||
        pkt->pts == AV_NOPTS_VALUE || pkt->dts == AV_NOPTS_VALUE || pkt->duration <= 0 ||
        pkt->pts > INT64_MAX - pkt->duration) {
        ret = AVERROR(EINVAL);
        goto fail;
    }
    st = s->streams[pkt->stream_index];
    if (b->last_dts[pkt->stream_index] != AV_NOPTS_VALUE &&
        pkt->dts < b->last_dts[pkt->stream_index]) {
        ret = AVERROR(EINVAL);
        goto fail;
    }
    b->last_dts[pkt->stream_index] = pkt->dts;
    time_us = av_rescale_q(pkt->pts, st->time_base, AV_TIME_BASE_Q);
    if (time_us == AV_NOPTS_VALUE) {
        ret = AVERROR(ERANGE);
        goto fail;
    }
    if (pkt->stream_index == b->video_index) {
        if (pkt->pts != pkt->dts || (idr = is_idr(b, pkt)) < 0) {
            ret = AVERROR_INVALIDDATA;
            goto fail;
        }
        if (!b->have_idr && !idr) {
            ret = AVERROR_INVALIDDATA;
            goto fail;
        }
        if (idr && b->have_idr && (ret = checkpoint(s, time_us)) < 0)
            goto fail;
        if (!b->have_idr) {
            b->start_us = time_us;
            b->have_idr = 1;
        }
    }
    if (b->packet_count >= MAX_PACKETS || pkt->size < 0 ||
        pkt->size > 2 * b->byte_limit - b->packet_bytes) {
        ret = AVERROR(EFBIG);
        goto fail;
    }
    if (b->packet_count == b->packet_capacity) {
        int capacity = FFMIN(MAX_PACKETS, FFMAX(128, b->packet_capacity * 2));
        AVPacket **packets = av_calloc(capacity, sizeof(*packets));
        int i;
        if (!packets) {
            ret = AVERROR(ENOMEM);
            goto fail;
        }
        for (i = 0; i < b->packet_capacity; i++)
            packets[i] = packet_at(b, i);
        av_free(b->packets);
        b->packets = packets;
        b->packet_capacity = capacity;
        b->packet_head = 0;
    }
    {
        int index = (b->packet_head + b->packet_count) & (b->packet_capacity - 1);
        if (!b->packets[index] && !(b->packets[index] = av_packet_alloc())) {
            ret = AVERROR(ENOMEM);
            goto fail;
        }
        if ((ret = av_packet_ref(b->packets[index], pkt)) < 0)
            goto fail;
    }
    b->packet_count++;
    b->packet_bytes += pkt->size;
    return 0;
fail:
    b->failed = ret;
    return ret;
}

/* CSV is a job-completion manifest, written only after all segments succeeded. */
static int write_manifest(AVFormatContext *s)
{
    ByteSegmentContext *b = s->priv_data;
    AVIOContext *pb = NULL;
    char name[1024];
    int ret, close_ret, i;
    if (!b->list)
        return 0;
    if ((ret = s->io_open(s, &pb, b->list, AVIO_FLAG_WRITE, NULL)) < 0)
        return ret;
    avio_printf(pb, "index,filename,bytes,start_us,end_us\n");
    for (i = 0; i < b->entry_count; i++) {
        const char *p;
        ByteSegmentEntry *entry = &b->entries[i];
        output_name(s, i, name, sizeof(name));
        avio_printf(pb, "%d,\"", i);
        for (p = name; *p; p++) {
            if (*p == '"')
                avio_w8(pb, '"');
            avio_w8(pb, *p);
        }
        avio_printf(pb, "\",%d,%"PRId64",%"PRId64"\n",
                    entry->bytes, entry->start_us, entry->end_us);
    }
    avio_flush(pb);
    ret = pb->error;
    close_ret = ff_format_io_close(s, &pb);
    if (ret >= 0)
        ret = close_ret;
    return ret;
}

static int byte_segment_write_trailer(AVFormatContext *s)
{
    ByteSegmentContext *b = s->priv_data;
    int64_t end_us = 0;
    int ret, i;
    if (b->failed)
        return b->failed;
    if (!b->have_idr || !b->packet_count)
        return AVERROR_INVALIDDATA;
    for (i = 0; i < b->packet_count; i++) {
        AVPacket *pkt = packet_at(b, i);
        if (pkt->stream_index == b->video_index)
            end_us = FFMAX(end_us, av_rescale_q(pkt->pts + pkt->duration,
                              s->streams[pkt->stream_index]->time_base, AV_TIME_BASE_Q));
    }
    if ((ret = checkpoint(s, end_us)) < 0 || (ret = publish(s)) < 0)
        return ret;
    return write_manifest(s);
}

static void byte_segment_deinit(AVFormatContext *s)
{
    ByteSegmentContext *b = s->priv_data;
    int i;
    for (i = 0; i < b->packet_capacity; i++)
        av_packet_free(&b->packets[i]);
    av_freep(&b->packets);
    av_packet_free(&b->render_packet);
    av_freep(&b->entries);
}

#define OFFSET(x) offsetof(ByteSegmentContext, x)
#define E AV_OPT_FLAG_ENCODING_PARAM
static const AVOption options[] = {
    { "byte_limit", "hard limit including the complete MP4 container", OFFSET(byte_limit), AV_OPT_TYPE_INT64, {.i64 = 900000}, 1024, MAX_SEGMENT_BYTES, E },
    { "max_segments", "maximum number of output files", OFFSET(max_segments), AV_OPT_TYPE_INT, {.i64 = 1024}, 1, 16384, E },
    { "segment_list", "completion manifest in CSV format", OFFSET(list), AV_OPT_TYPE_STRING, {.str = NULL}, 0, 0, E },
    { NULL },
};

static const AVClass byte_segment_class = {
    .class_name = "byte-bounded MP4 segment muxer",
    .item_name = av_default_item_name,
    .option = options,
    .version = LIBAVUTIL_VERSION_INT,
};

const FFOutputFormat ff_byte_segment_muxer = {
    .p.name = "byte_segment",
    .p.long_name = NULL_IF_CONFIG_SMALL("byte-bounded independent H.264/AAC MP4 segments"),
    .p.flags = AVFMT_NOFILE | AVFMT_GLOBALHEADER | AVFMT_TS_NEGATIVE,
    .p.video_codec = AV_CODEC_ID_H264,
    .p.audio_codec = AV_CODEC_ID_AAC,
    .p.priv_class = &byte_segment_class,
    .priv_data_size = sizeof(ByteSegmentContext),
    .init = byte_segment_init,
    .write_packet = byte_segment_write_packet,
    .write_trailer = byte_segment_write_trailer,
    .deinit = byte_segment_deinit,
};
