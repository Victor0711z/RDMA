#include "scheduler.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>

enum { CONCURRENT_CHUNKS = 10000, CONCURRENT_THREADS = 8 };

typedef struct {
    scheduler_t *sched;
    _Atomic unsigned char *seen;
} concurrent_ctx_t;

static void *concurrent_worker(void *arg) {
    concurrent_ctx_t *ctx = (concurrent_ctx_t *)arg;
    for (;;) {
        int path;
        uint32_t chunk;
        if (scheduler_acquire(ctx->sched, &path, &chunk, 1024) == 0) {
            assert(chunk < CONCURRENT_CHUNKS);
            assert(atomic_fetch_add(&ctx->seen[chunk], 1) == 0);
            scheduler_on_ack(ctx->sched, path, chunk, 1024);
            continue;
        }
        if (scheduler_all_done(ctx->sched)) break;
        (void)scheduler_wait_for_work(ctx->sched, 10);
    }
    return NULL;
}

static void test_weighted_distribution(void) {
    scheduler_t s;
    const double weights[] = {2.0, 1.0};
    assert(scheduler_init(&s, 2, weights, 6, 8) == 0);

    int counts[2] = {0};
    for (uint32_t i = 0; i < 6; i++) {
        int path;
        uint32_t chunk;
        assert(scheduler_acquire(&s, &path, &chunk, 1024) == 0);
        assert(chunk == i);
        counts[path]++;
    }
    assert(counts[0] == 4);
    assert(counts[1] == 2);
    scheduler_destroy(&s);
}

static void test_failover_and_late_ack(void) {
    scheduler_t s;
    assert(scheduler_init(&s, 2, NULL, 4, 2) == 0);

    int owners[4];
    for (uint32_t i = 0; i < 4; i++) {
        uint32_t chunk;
        assert(scheduler_acquire(&s, &owners[i], &chunk, 1024) == 0);
        assert(chunk == i);
    }

    scheduler_mark_down(&s, 1);
    scheduler_on_ack(&s, 0, 0, 1024);
    scheduler_on_ack(&s, 0, 2, 1024);

    for (int i = 0; i < 2; i++) {
        int path;
        uint32_t chunk;
        assert(scheduler_acquire(&s, &path, &chunk, 1024) == 0);
        assert(path == 0);
        assert(chunk == 1 || chunk == 3);
        scheduler_on_ack(&s, path, chunk, 1024);
    }

    /* 原路径的迟到 ACK 不得重复计数。 */
    scheduler_on_ack(&s, 1, 1, 1024);
    assert(scheduler_all_done(&s));
    assert(scheduler_wait_terminal(&s, 1) == 1);
    scheduler_destroy(&s);
}

static void test_credit_spreads_work_across_qps(void) {
    scheduler_t s;
    assert(scheduler_init(&s, 2, NULL, 4, 1) == 0);

    int path;
    uint32_t chunk;
    assert(scheduler_acquire(&s, &path, &chunk, 1024) == 0 && path == 0);
    assert(scheduler_acquire(&s, &path, &chunk, 1024) == 0 && path == 1);
    /* 两个 QP 的 credit 都耗尽，必须等待 ACK。 */
    assert(scheduler_acquire(&s, &path, &chunk, 1024) != 0);
    assert(scheduler_wait_for_work(&s, 1) == 0);
    scheduler_on_ack(&s, 0, 0, 1024);
    assert(scheduler_wait_for_work(&s, 1) == 1);
    scheduler_destroy(&s);
}

static void test_concurrent_acquire_and_ack(void) {
    scheduler_t s;
    assert(scheduler_init(&s, 4, NULL, CONCURRENT_CHUNKS, 64) == 0);
    _Atomic unsigned char *seen = calloc(CONCURRENT_CHUNKS, sizeof(*seen));
    assert(seen != NULL);
    for (uint32_t i = 0; i < CONCURRENT_CHUNKS; i++) atomic_init(&seen[i], 0);

    concurrent_ctx_t ctx = { .sched = &s, .seen = seen };
    pthread_t tids[CONCURRENT_THREADS];
    for (int i = 0; i < CONCURRENT_THREADS; i++) {
        assert(pthread_create(&tids[i], NULL, concurrent_worker, &ctx) == 0);
    }
    for (int i = 0; i < CONCURRENT_THREADS; i++) pthread_join(tids[i], NULL);

    assert(scheduler_all_done(&s));
    for (uint32_t i = 0; i < CONCURRENT_CHUNKS; i++) assert(atomic_load(&seen[i]) == 1);
    free(seen);
    scheduler_destroy(&s);
}

int main(void) {
    test_weighted_distribution();
    test_failover_and_late_ack();
    test_credit_spreads_work_across_qps();
    test_concurrent_acquire_and_ack();
    puts("scheduler tests: OK");
    return 0;
}
