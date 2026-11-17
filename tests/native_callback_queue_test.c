/* Tests the production queue with real libavutil allocations. */
#include <assert.h>
#include <limits.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "ffmpegkit_callback_queue.h"
#include "libavutil/mem.h"

static KitCallbackQueue queue;
static atomic_int counts[8];
static atomic_int producers_done;

static void drain(void)
{
    struct CallbackData *data;
    while ((data = kit_callback_queue_take(&queue)))
        kit_callback_queue_finish(&queue, data);
    assert(queue.count == 0);
    for (int i = 0; i < 8; i++)
        assert(atomic_load(&counts[i]) == 0);
}

static void *producer(void *arg)
{
    long id = (long)(intptr_t)arg;
    for (int i = 0; i < 10000; i++) {
        kit_callback_queue_log(&queue, id, 16, "stress");
        kit_callback_queue_statistics(&queue, id, i, 24, 0, i, i, 1, 1);
    }
    atomic_fetch_add(&producers_done, 1);
    return NULL;
}

static void *consumer(void *unused)
{
    struct CallbackData *data;
    (void)unused;
    while (atomic_load(&producers_done) < 4) {
        if ((data = kit_callback_queue_take(&queue))) {
            assert(atomic_load(&counts[data->sessionId]) > 0);
            kit_callback_queue_finish(&queue, data);
        }
    }
    return NULL;
}

int main(void)
{
    struct CallbackData *data;
    pthread_t workers[4], reader;
    char huge[KIT_CALLBACK_LOG_LIMIT * 3];
    kit_callback_queue_init(&queue, counts, 8);

    /* Overload is bounded and dropped records do not inflate in-transit counts. */
    for (int i = 0; i < KIT_CALLBACK_QUEUE_LIMIT; i++)
        assert(kit_callback_queue_log(&queue, 1, 16, "message"));
    assert(!kit_callback_queue_log(&queue, 1, 16, "overflow"));
    assert(atomic_load(&counts[1]) == KIT_CALLBACK_QUEUE_LIMIT);
    drain();

    /* One pending progress snapshot per full session ID, with the newest value. */
    for (int i = 0; i < 10000; i++)
        assert(kit_callback_queue_statistics(&queue, 2, i, 24, 1, i, i, 2, 1));
    assert(queue.count == 1 && atomic_load(&counts[2]) == 1);
    data = kit_callback_queue_take(&queue);
    assert(data->statisticsFrameNumber == 9999);
    assert(data->statisticsSize == 9999);
    kit_callback_queue_finish(&queue, data);

    memset(huge, 'x', sizeof(huge) - 1);
    huge[sizeof(huge) - 1] = 0;
    assert(kit_callback_queue_log(&queue, 0, 16, huge));
    data = kit_callback_queue_take(&queue);
    assert(data->logData.len == KIT_CALLBACK_LOG_LIMIT);
    kit_callback_queue_finish(&queue, data);

    /* Fault injection through the allocator used by the production code. */
    av_max_alloc(1);
    assert(!kit_callback_queue_log(&queue, 0, 16, "allocation failure"));
    assert(!kit_callback_queue_statistics(&queue, 0, 1, 1, 1, 1, 1, 1, 1));
    av_max_alloc(SIZE_MAX);
    drain();

    pthread_create(&reader, NULL, consumer, NULL);
    for (long i = 0; i < 4; i++)
        pthread_create(&workers[i], NULL, producer, (void *)(intptr_t)i);
    for (int i = 0; i < 4; i++)
        pthread_join(workers[i], NULL);
    pthread_join(reader, NULL);
    drain();
    pthread_mutex_destroy(&queue.mutex);
    puts("PASS: queue bounds, progress coalescing, allocation failure, concurrent accounting");
    return 0;
}
