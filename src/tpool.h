/*
 * tpool.h
 *
 *  Created on: Sep 21, 2021
 *      Author: david
 */

#include <stdbool.h>
#include <stddef.h>

#ifndef TPOOL_H_
#define TPOOL_H_

struct tpool;
typedef struct tpool tpool_t;

typedef void (*thread_func_t)(void *arg);

tpool_t* tpool_create(size_t num);
void tpool_destroy(tpool_t *tm);

bool tpool_add_work(tpool_t *tm, thread_func_t func, void *arg);
void tpool_wait(tpool_t *tm);

void worker(void *pdata);

#endif /* TPOOL_H_ */
