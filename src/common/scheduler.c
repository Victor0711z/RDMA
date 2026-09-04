#include "scheduler.h"

#include <stdlib.h>
#include <string.h>

int scheduler_init(scheduler_t *s, int num_paths, const double *weights,
                    uint32_t total_chunks, uint32_t window_per_path) {
    if (num_paths <= 0 || num_paths > SCHED_MAX_PATHS) return -1;
    memset(s, 0, sizeof(*s));
    pthread_mutex_init(&s->lock, NULL);

    s->num_paths = num_paths;
    s->total_chunks = total_chunks;
    s->next_chunk_id = 0;
    s->chunks_done = 0;

    for (int i = 0; i < num_paths; i++) {
        s->paths[i].id = i;
        s->paths[i].weight = weights ? weights[i] : 1.0;
        if (s->paths[i].weight <= 0.0) s->paths[i].weight = 1.0;
        s->paths[i].vprogress = 0.0;
        s->paths[i].state = PATH_UP;
        s->paths[i].max_credit = window_per_path;
        s->paths[i].credit = window_per_path;
    }

    s->retry_cap = (int)total_chunks + 1;
    if (s->retry_cap < 1) s->retry_cap = 1;
    s->retry_queue = (uint32_t *)calloc((size_t)s->retry_cap, sizeof(uint32_t));
    s->retry_head = s->retry_tail = s->retry_count = 0;

    s->chunk_owner = (int8_t *)malloc((size_t)total_chunks > 0 ? total_chunks : 1);
    if (!s->retry_queue || !s->chunk_owner) {
        scheduler_destroy(s);
        return -1;
    }
    memset(s->chunk_owner, -1, (size_t)total_chunks > 0 ? total_chunks : 1);
    return 0;
}

void scheduler_destroy(scheduler_t *s) {
    free(s->retry_queue);
    free(s->chunk_owner);
    s->retry_queue = NULL;
    s->chunk_owner = NULL;
    pthread_mutex_destroy(&s->lock);
}

/* 调用方必须持有 s->lock */
static int pick_best_path_locked(scheduler_t *s) {
    int best = -1;
    double best_v = 0.0;
    for (int i = 0; i < s->num_paths; i++) {
        sched_path_t *p = &s->paths[i];
        if (p->state != PATH_UP) continue;
        if (p->credit == 0) continue;
        if (best == -1 || p->vprogress < best_v) {
            best = i;
            best_v = p->vprogress;
        }
    }
    return best;
}

int scheduler_acquire(scheduler_t *s, int *out_path, uint32_t *out_chunk, uint32_t chunk_bytes) {
    pthread_mutex_lock(&s->lock);

    int path_idx = pick_best_path_locked(s);
    if (path_idx < 0) {
        pthread_mutex_unlock(&s->lock);
        return -1;
    }

    uint32_t chunk_id;
    if (s->retry_count > 0) {
        chunk_id = s->retry_queue[s->retry_head];
        s->retry_head = (s->retry_head + 1) % s->retry_cap;
        s->retry_count--;
    } else if (s->next_chunk_id < s->total_chunks) {
        chunk_id = s->next_chunk_id++;
    } else {
        /* 没有更多分片需要分配了（即便这条路径还有 credit） */
        pthread_mutex_unlock(&s->lock);
        return -1;
    }

    sched_path_t *p = &s->paths[path_idx];
    p->credit--;
    p->inflight++;
    p->bytes_assigned += chunk_bytes;
    p->vprogress += (double)chunk_bytes / p->weight;
    s->chunk_owner[chunk_id] = (int8_t)path_idx;

    pthread_mutex_unlock(&s->lock);

    *out_path = path_idx;
    *out_chunk = chunk_id;
    return 0;
}

void scheduler_on_ack(scheduler_t *s, int path_idx, uint32_t chunk_id, uint32_t chunk_bytes) {
    pthread_mutex_lock(&s->lock);
    if (chunk_id < s->total_chunks && s->chunk_owner[chunk_id] == (int8_t)path_idx) {
        s->chunk_owner[chunk_id] = -1;
        sched_path_t *p = &s->paths[path_idx];
        if (p->inflight > 0) p->inflight--;
        p->credit++;
        if (p->credit > p->max_credit) p->credit = p->max_credit;
        p->bytes_acked += chunk_bytes;
        p->chunks_acked++;
        s->chunks_done++;
    }
    /* 否则：这是一个迟到的 / 针对已被故障转移分片的重复 ACK，直接忽略 */
    pthread_mutex_unlock(&s->lock);
}

void scheduler_mark_down(scheduler_t *s, int path_idx) {
    pthread_mutex_lock(&s->lock);
    if (path_idx < 0 || path_idx >= s->num_paths) {
        pthread_mutex_unlock(&s->lock);
        return;
    }
    sched_path_t *p = &s->paths[path_idx];
    if (p->state == PATH_DOWN) {
        pthread_mutex_unlock(&s->lock);
        return;
    }
    p->state = PATH_DOWN;

    for (uint32_t i = 0; i < s->total_chunks; i++) {
        if (s->chunk_owner[i] == (int8_t)path_idx) {
            s->chunk_owner[i] = -1;
            s->retry_queue[s->retry_tail] = i;
            s->retry_tail = (s->retry_tail + 1) % s->retry_cap;
            s->retry_count++;
            p->chunks_failed_over++;
        }
    }
    p->inflight = 0;
    p->credit = 0;

    pthread_mutex_unlock(&s->lock);
}

bool scheduler_all_done(scheduler_t *s) {
    pthread_mutex_lock(&s->lock);
    bool done = (s->chunks_done == s->total_chunks);
    pthread_mutex_unlock(&s->lock);
    return done;
}

bool scheduler_has_live_path(scheduler_t *s) {
    pthread_mutex_lock(&s->lock);
    bool live = false;
    for (int i = 0; i < s->num_paths; i++) {
        if (s->paths[i].state == PATH_UP) { live = true; break; }
    }
    pthread_mutex_unlock(&s->lock);
    return live;
}

void scheduler_snapshot(scheduler_t *s, sched_path_t *out, int max_paths, int *out_n) {
    pthread_mutex_lock(&s->lock);
    int n = s->num_paths < max_paths ? s->num_paths : max_paths;
    memcpy(out, s->paths, (size_t)n * sizeof(sched_path_t));
    pthread_mutex_unlock(&s->lock);
    *out_n = n;
}
