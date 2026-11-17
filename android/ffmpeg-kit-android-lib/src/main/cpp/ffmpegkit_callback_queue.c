/* FFmpegKit callback queue. LGPL-3.0-or-later. */
#include "ffmpegkit_callback_queue.h"
#include "libavutil/mem.h"

void kit_callback_queue_init(KitCallbackQueue *q, atomic_int *counts, unsigned slots)
{
    pthread_mutex_init(&q->mutex, NULL);
    q->head = q->tail = NULL;
    q->count = 0;
    q->in_transit = counts;
    q->session_slots = slots;
}

/* Caller holds q->mutex. Count before publishing to the consumer. */
static int append(KitCallbackQueue *q, struct CallbackData *data)
{
    if (q->count >= KIT_CALLBACK_QUEUE_LIMIT)
        return 0;
    atomic_fetch_add(&q->in_transit[data->sessionId % q->session_slots], 1);
    if (q->tail)
        q->tail->next = data;
    else
        q->head = data;
    q->tail = data;
    q->count++;
    return 1;
}

int kit_callback_queue_log(KitCallbackQueue *q, long id, int level, const char *text)
{
    struct CallbackData *data = av_mallocz(sizeof(*data));
    int accepted;
    if (!data)
        return 0;
    data->type = LogType;
    data->sessionId = id;
    data->logLevel = level;
    av_bprint_init(&data->logData, 0, KIT_CALLBACK_LOG_LIMIT + 1);
    av_bprintf(&data->logData, "%.*s", KIT_CALLBACK_LOG_LIMIT, text);
    if (!av_bprint_is_complete(&data->logData)) {
        av_bprint_finalize(&data->logData, NULL);
        av_free(data);
        return 0;
    }
    pthread_mutex_lock(&q->mutex);
    accepted = append(q, data);
    pthread_mutex_unlock(&q->mutex);
    if (!accepted) {
        av_bprint_finalize(&data->logData, NULL);
        av_free(data);
    }
    return accepted;
}

int kit_callback_queue_statistics(KitCallbackQueue *q, long id, int frame, float fps,
                                 float quality, int64_t size, double time,
                                 double bitrate, double speed)
{
    struct CallbackData *data;
    int accepted = 0;
    pthread_mutex_lock(&q->mutex);
    /* Pending statistics for a session are snapshots, not a lossless event log. */
    for (data = q->head; data; data = data->next)
        if (data->type == StatisticsType && data->sessionId == id)
            break;
    if (!data && q->count < KIT_CALLBACK_QUEUE_LIMIT) {
        data = av_mallocz(sizeof(*data));
        if (data) {
            data->type = StatisticsType;
            data->sessionId = id;
            append(q, data);
        }
    }
    if (data) {
        data->statisticsFrameNumber = frame;
        data->statisticsFps = fps;
        data->statisticsQuality = quality;
        data->statisticsSize = size;
        data->statisticsTime = time;
        data->statisticsBitrate = bitrate;
        data->statisticsSpeed = speed;
        accepted = 1;
    }
    pthread_mutex_unlock(&q->mutex);
    return accepted;
}

struct CallbackData *kit_callback_queue_take(KitCallbackQueue *q)
{
    struct CallbackData *data;
    pthread_mutex_lock(&q->mutex);
    data = q->head;
    if (data) {
        q->head = data->next;
        if (!q->head)
            q->tail = NULL;
        data->next = NULL;
        q->count--;
    }
    pthread_mutex_unlock(&q->mutex);
    return data;
}

void kit_callback_queue_finish(KitCallbackQueue *q, struct CallbackData *data)
{
    if (!data)
        return;
    if (data->type == LogType)
        av_bprint_finalize(&data->logData, NULL);
    atomic_fetch_sub(&q->in_transit[data->sessionId % q->session_slots], 1);
    av_free(data);
}
