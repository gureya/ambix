#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/syscall.h>

volatile int sig = 0;

typedef struct threads_args {
    int* array;
    long size;
    long start;
    long stride;
} targs_t;


void* iterate_array(void * args_v) {
    targs_t *args = (targs_t *) args_v;
    long n_accesses = 0;
    int c;
    while(!sig) {
        for (long i = args->start; i < args->size + args->start; i+=args->stride) {
            if(sig) return (void *) n_accesses;
            c = args->array[i];
            n_accesses++;
        }
    }
    return (void *) n_accesses;
}

int main(int argc, char** argv) {
    if(argc != 4) {
        printf("USAGE: %s [GB] [n_threads] [run_time]\n", argv[0]);
        return 1;
    }
    long array_size = (long) atoi(argv[1]) * 1024 * 1024 * 1024 / sizeof(int);
    int n_threads = atoi(argv[2]);
    int run_time = atoi(argv[3]);
    long page_size = getpagesize();

    int* array = calloc(array_size, sizeof(int));

    for(long i=0; i < array_size; i+= (page_size / sizeof(int))) {
        array[i] = 1;
    }

    printf("Starting Benchmark\n");

    pthread_t threads[n_threads];

    for(int i=0; i < n_threads; i++) {
        targs_t args = { array, array_size,
            i * page_size / sizeof(int), n_threads * page_size / sizeof(int) };
        pthread_create(&threads[i], NULL, iterate_array, (void*) &args);
    }

    sleep(run_time);

    printf("Ending Benchmark\n");
    sig = 1;

    unsigned long total_accesses = 0;
    for(int i=0; i < n_threads; i++) {
        void* ret;
        pthread_join(threads[i], &ret);
        total_accesses += (long) ret;
    }

    printf("Throughput (k_accesses/s): %0.2f\n", ((double) total_accesses)/run_time/n_threads/1000);

    free(array);

    return 0;
}
