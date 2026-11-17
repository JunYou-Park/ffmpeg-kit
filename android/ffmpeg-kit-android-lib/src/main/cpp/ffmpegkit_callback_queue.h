/* FFmpegKit callback queue. LGPL-3.0-or-later. */
#ifndef FFMPEGKIT_CALLBACK_QUEUE_H
#define FFMPEGKIT_CALLBACK_QUEUE_H

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include "libavutil/bprint.h"

#define KIT_CALLBACK_QUEUE_LIMIT 1024
#define KIT_CALLBACK_LOG_LIMIT 4096
#define LogType 1
#define StatisticsType 2

struct CallbackData {
    int type;
    long sessionId;
    int logLevel;
    AVBPrint logData;
    int statisticsFrameNumber;
    float statisticsFps, statisticsQuality;
    int64_t statisticsSize;
    double statisticsTime, statisticsBitrate, statisticsSpeed;
    struct CallbackData *next;
};

typedef struct KitCallbackQueue {
    pthread_mutex_t mutex;
    struct CallbackData *head, *tail;
    unsigned count;
    atomic_int *in_transit;
    unsigned session_slots;
} KitCallbackQueue;

void kit_callback_queue_init(KitCallbackQueue *q, atomic_int *counts, unsigned slots);
int kit_callback_queue_log(KitCallbackQueue *q, long id, int level, const char *text);
int kit_callback_queue_statistics(KitCallbackQueue *q, long id, int frame, float fps,
                                 float quality, int64_t size, double time,
                                 double bitrate, double speed);
struct CallbackData *kit_callback_queue_take(KitCallbackQueue *q);
void kit_callback_queue_finish(KitCallbackQueue *q, struct CallbackData *data);

#endif
