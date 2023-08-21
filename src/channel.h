#pragma once

#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>

struct Channel {
    uint32_t* store;
    uint32_t previous;
    int capacity;
    int back;
    int front;

    int waiting;
    pthread_mutex_t lock;
    pthread_cond_t cond;
};

struct Channel* channel_make(int capacity);
void channel_push(struct Channel* q, uint32_t element);
uint32_t channel_pop_default(struct Channel* q);
int channel_pop_ornot(struct Channel* q, uint32_t* ret);
void channel_block_until_data(struct Channel* q);
uint32_t channel_pop_wait(struct Channel* q);