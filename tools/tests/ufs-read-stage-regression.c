/* Host stubs around unmodified, extracted sweet_ufs_strategy()/ufs_copy_out().
 * Scheduling hooks model another reader, a full bounce-buffer log dump, and
 * a caller-page fault recursively reading the disk during the unlocked copy. */
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define UFS_BLOCK 4096u
#define PAGE_SIZE 4096u
#define STAGE_SIZE (256u << 10)
#define BOUNCE_SIZE (1u << 20)
#define DMA_BOUNCE 0x4000u
#define DMA_STAGE (DMA_BOUNCE + BOUNCE_SIZE - STAGE_SIZE)
#define DMA_SIZE (DMA_BOUNCE + BOUNCE_SIZE)
#define B_READ 1
#define Z_WAITOK 1
#define UFS_LK_STRATEGY 4

typedef struct buffer {
    uint64_t block;
    uint32_t count, resid;
    int flags, error, completed, mapped, map_failure;
    char *data;
} *buf_t;
static struct {
    char *dma;
    uint32_t secsize;
    uint64_t nblocks, first_block;
    bool writable, trace, direct;
    uint32_t cmd_serial, strat_count, strat_tid, strat_done, strat_step;
    uint32_t io_phase, direct_reads;
    uint64_t strat_blk, strat_vaddr, strat_t0, direct_bytes;
} su;
static char disk[2u << 20];
static pthread_mutex_t driver = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t schedule = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
static _Thread_local int lock_depth, primary;
static int paused, resume_copy, mode, live_allocations, fail_alloc, read_error;
static void sweet_ufs_strategy(buf_t);
static void ufs_copy_out(void *, const void *, uint32_t, volatile uint32_t *);
static void ufs_lock(int who) {
    assert(!lock_depth);
    assert(!pthread_mutex_lock(&driver));
    lock_depth = 1;
}
static void ufs_unlock(void) {
    assert(lock_depth == 1);
    lock_depth = 0;
    assert(!pthread_mutex_unlock(&driver));
}
#if EXPECT_PRIVATE
static void *kalloc_data(size_t size, int flags) {
    assert(!lock_depth);
    if (fail_alloc) return NULL;
    void *pointer = malloc(size);
    assert(pointer);
    live_allocations++;
    return pointer;
}
static void kfree_data(void *pointer, size_t size) {
    assert(!lock_depth && size && live_allocations > 0);
    live_allocations--;
    free(pointer);
}
#endif
#define buf_blkno(bp) ((bp)->block)
#define buf_count(bp) ((bp)->count)
#define buf_flags(bp) ((bp)->flags)
#define buf_setresid(bp, value) ((bp)->resid = (value))
#define buf_seterror(bp, value) ((bp)->error = (value))
static int buf_map(buf_t bp, caddr_t *destination) {
    if (bp->map_failure) return EIO;
    assert(!bp->mapped);
    bp->mapped = 1;
    *destination = bp->data;
    return 0;
}
static void buf_unmap(buf_t bp) { assert(bp->mapped); bp->mapped = 0; }
static void buf_biodone(buf_t bp) { assert(!bp->mapped); bp->completed++; }
static uint64_t mach_absolute_time(void) { return 1; }
static int current_thread(void) { return 1; }
static uint64_t thread_tid(int thread) { return 1; }
static void sweet_tracef(const char *format, ...) {}
static int ufs_read_blocks(uint64_t lba, uint32_t blocks) {
    assert(lock_depth && (lba + blocks) * UFS_BLOCK <= sizeof(disk));
    if (read_error) return read_error;
    memcpy(su.dma + DMA_BOUNCE, disk + lba * UFS_BLOCK, blocks * UFS_BLOCK);
    return 0;
}
static int ufs_read_blocks_dma(uint64_t lba, uint32_t blocks, void *out) {
    assert(lock_depth);
    if (read_error) return read_error;
    memcpy(out, disk + lba * UFS_BLOCK, blocks * UFS_BLOCK);
    return 0;
}
static int ufs_write_blocks(uint64_t lba, uint32_t blocks) {
    assert(lock_depth && (lba + blocks) * UFS_BLOCK <= sizeof(disk));
    memcpy(disk + lba * UFS_BLOCK, su.dma + DMA_BOUNCE, blocks * UFS_BLOCK);
    return 0;
}

#include "strategy.h"

static void ufs_copy_out(void *out, const void *in, uint32_t len,
    volatile uint32_t *progress) {
    assert(!lock_depth); /* A page fault must be able to issue another read. */
    if (primary && mode) {
        primary = 0;
        if (mode == 3) {
            char nested_data[8192];
            struct buffer nested = {.block = 64, .count = sizeof(nested_data),
                .flags = B_READ, .data = nested_data};
            sweet_ufs_strategy(&nested);
            assert(!nested.error && !nested.resid && nested.completed == 1);
            assert(!memcmp(nested_data, disk + 64 * su.secsize, sizeof(nested_data)));
        } else {
            assert(!pthread_mutex_lock(&schedule));
            paused = 1;
            assert(!pthread_cond_signal(&changed));
            while (!resume_copy) assert(!pthread_cond_wait(&changed, &schedule));
            assert(!pthread_mutex_unlock(&schedule));
        }
    }
    ufs_copy_out_impl(out, in, len, progress);
}

static void *first_reader(void *argument) {
    primary = 1;
    sweet_ufs_strategy(argument);
    return NULL;
}
static void reset(void) {
    assert(!live_allocations && !lock_depth);
    memset(&su, 0, sizeof(su));
    su.dma = calloc(1, DMA_SIZE);
    assert(su.dma);
    su.secsize = 512;
    su.nblocks = sizeof(disk) / UFS_BLOCK;
    su.writable = true;
    for (size_t i = 0; i < sizeof(disk); i++)
        disk[i] = (char)((i / UFS_BLOCK) * 19 + i % 251);
    paused = resume_copy = mode = fail_alloc = read_error = 0;
}
static void finish(void) { assert(!live_allocations); free(su.dma); }
static void interleaving(int scenario) {
    reset();
    mode = scenario;
    char a[8192], b[8192];
    struct buffer first = {.block = 0, .count = sizeof(a), .flags = B_READ, .data = a};
    struct buffer second = {.block = 64, .count = sizeof(b), .flags = B_READ, .data = b};
    pthread_t thread;
    if (scenario == 3) {
        primary = 1;
        sweet_ufs_strategy(&first);
    } else {
        assert(!pthread_create(&thread, NULL, first_reader, &first));
        assert(!pthread_mutex_lock(&schedule));
        while (!paused) assert(!pthread_cond_wait(&changed, &schedule));
        assert(!pthread_mutex_unlock(&schedule));
        if (scenario == 1) {
            sweet_ufs_strategy(&second);
            assert(!memcmp(b, disk + 64 * su.secsize, sizeof(b)));
        } else {
            ufs_lock(UFS_LK_STRATEGY);
            memset(su.dma + DMA_BOUNCE, 'L', BOUNCE_SIZE); /* log dump */
            ufs_unlock();
        }
        assert(!pthread_mutex_lock(&schedule));
        resume_copy = 1;
        assert(!pthread_cond_signal(&changed));
        assert(!pthread_mutex_unlock(&schedule));
        assert(!pthread_join(thread, NULL));
    }
    assert(!first.error && !first.resid && first.completed == 1);
    int intact = !memcmp(a, disk, sizeof(a));
    assert(intact == EXPECT_PRIVATE);
    printf("%s: %s: %s\n", EXPECT_PRIVATE ? "patched" : "original",
        scenario == 1 ? "overlapping reader" : scenario == 2 ? "log overwrite" :
        "recursive caller fault", intact ? "bytes preserved" : "corruption reproduced");
    finish();
}
static void ordinary_and_errors(void) {
    reset();
    char *data = malloc(2 * STAGE_SIZE + 123);
    assert(data);
    struct buffer bp = {.block = 1, .count = 2 * STAGE_SIZE + 123,
        .flags = B_READ, .data = data};
    sweet_ufs_strategy(&bp);
    assert(!bp.error && !bp.resid && bp.completed == 1);
    assert(!memcmp(data, disk + 512, bp.count));
    assert(!live_allocations);
    for (int failure = 0; failure < 5; failure++) {
        bp = (struct buffer){.count = 8192, .flags = B_READ, .data = data};
        if (failure == 0) bp.map_failure = 1;
        if (failure == 1) read_error = EIO;
        if (failure == 2) bp.block = sizeof(disk) / su.secsize;
        if (failure == 3) bp.count = 0;
        if (failure == 4) fail_alloc = 1;
        sweet_ufs_strategy(&bp);
        assert(bp.completed == 1 && !live_allocations);
        if (failure < 3) assert(bp.error);
        if (failure == 4 && EXPECT_PRIVATE) assert(bp.error == ENOMEM);
        read_error = fail_alloc = 0;
    }
    /* Unaligned writes must retain the neighboring bytes. */
    char before = disk[511], after = disk[512 + 5000];
    memset(data, 0x5a, 5000);
    bp = (struct buffer){.block = 1, .count = 5000, .data = data};
    sweet_ufs_strategy(&bp);
    assert(!bp.error && !bp.resid && !memcmp(disk + 512, data, 5000));
    assert(disk[511] == before && disk[512 + 5000] == after);
    su.writable = false;
    bp.completed = bp.error = 0;
    sweet_ufs_strategy(&bp);
    assert(bp.error == EROFS && bp.completed == 1);
    free(data);
    finish();
    puts("ordinary: multi-chunk/unaligned read, partial write, and error cleanup passed");
}
int main(void) {
    alarm(15);
    ordinary_and_errors();
    for (int scenario = 1; scenario <= 3; scenario++) interleaving(scenario);
    return 0;
}
