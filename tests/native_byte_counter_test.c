/* Exercise production counting I/O independently of the segmenter. */
#include <assert.h>
#include <stdio.h>
#include "byte_segment_io.h"

int main(void)
{
    ByteSegmentCounter counter = {0};
    assert(byte_segment_count_write(&counter, NULL, 100) == 100);
    assert(counter.position == 100 && counter.size == 100);
    assert(byte_segment_count_seek(&counter, 8, SEEK_SET) == 8);
    assert(byte_segment_count_write(&counter, NULL, 4) == 4);
    assert(counter.size == 100); /* Header overwrite is not append. */
    assert(byte_segment_count_seek(&counter, 20, SEEK_END) == 120);
    assert(counter.size == 100); /* A seek alone does not extend the file. */
    assert(byte_segment_count_write(&counter, NULL, 10) == 10);
    assert(counter.size == 130);
    assert(byte_segment_count_seek(&counter, -30, SEEK_CUR | AVSEEK_FORCE) == 100);
    assert(byte_segment_count_seek(&counter, 0, AVSEEK_SIZE) == 130);
    assert(counter.position == 100);
    assert(byte_segment_count_seek(&counter, -1, SEEK_SET) < 0);
    assert(byte_segment_count_seek(&counter, INT64_MAX, SEEK_CUR) < 0);
    assert(byte_segment_count_seek(&counter, INT64_MIN, SEEK_END) < 0);
    assert(byte_segment_count_write(&counter, NULL, -1) < 0);
    assert(counter.position == 100 && counter.size == 130);
    assert(byte_segment_count_seek(&counter, INT_MAX - 1, SEEK_SET) == INT_MAX - 1);
    assert(byte_segment_count_write(&counter, NULL, 2) < 0);
    assert(byte_segment_count_write(&counter, NULL, 1) == 1);
    assert(counter.size == INT_MAX);
    puts("PASS: counting I/O seeks, overwrites, high-water size, overflow guards");
    return 0;
}
