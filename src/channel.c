#include "channel.h"

#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>

// NOT a general-purpose queue! this is specifically for hardware
// IN/OUT instructions, where you can pop_default to get the last value
// that was there and pushing to a full queue just drops the value

struct Channel* channel_make(int capacity) {
    struct Channel* q = malloc(sizeof(struct Channel));
    q->store = malloc(sizeof(uint32_t) * capacity);
    q->previous = 0;
    q->capacity = capacity;
    q->back = -1;
    q->front = -1;

    q->waiting = 0;
    pthread_mutex_init(&q->lock, NULL);
    pthread_cond_init(&q->cond, NULL);
    return q;
}

void channel_push(struct Channel* q, uint32_t element) {
    pthread_mutex_lock(&q->lock);
    int newloc = (q->front + 1) % q->capacity;
    if (newloc != q->back) {
        q->store[newloc] = element;
        q->front = newloc;
        if (q->back == -1) {
            q->back = newloc;
        }
    }
    if (q->waiting) {
        pthread_cond_signal(&q->cond);
    }
    pthread_mutex_unlock(&q->lock);
    return;
}

uint32_t internal_pop(struct Channel* q) {
    uint32_t ret = q->store[q->back];
    if (q->back == q->front) {
        q->back = -1;
        q->front = -1;
    } else {
        q->back = (q->back + 1) % q->capacity;
    }
    return ret;
}

uint32_t channel_pop_default(struct Channel* q) {
    pthread_mutex_lock(&q->lock);
    uint32_t ret;
    if (q->back == -1) {
        ret = q->previous;
    } else {
        ret = internal_pop(q);
    }
    pthread_mutex_unlock(&q->lock);
    return ret;
}

int channel_pop_ornot(struct Channel* q, uint32_t* ret) {
    pthread_mutex_lock(&q->lock);
    int status;
    if (q->back == -1) {
        status = 0;  // returns bool
    } else {
        status = 1;
        *ret = internal_pop(q);
    }
    pthread_mutex_unlock(&q->lock);
    return status;
}

void channel_block_until_data(struct Channel* q) {
    pthread_mutex_lock(&q->lock);
    if (q->back == -1) {
        q->waiting = 1;
        pthread_cond_wait(&q->cond, &q->lock);
        q->waiting = 0;
    }
    pthread_mutex_unlock(&q->lock);
    return;
}

uint32_t channel_pop_wait(struct Channel* q) {
    pthread_mutex_lock(&q->lock);
    uint32_t ret;
    if (q->back == -1) {
        q->waiting = 1;
        pthread_cond_wait(&q->cond, &q->lock);
        q->waiting = 0;
    }

    ret = internal_pop(q);
    pthread_mutex_unlock(&q->lock);
    return ret;
}