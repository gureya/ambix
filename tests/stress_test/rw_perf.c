#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

void write_array(char *array, size_t size, size_t page_size) {
    for(size_t i=0; i<size; i+=page_size/sizeof(char)) {
        array[i] = 1;
    }
}

void access_array(char *array, size_t size, size_t page_size) {
    char var;
    for(size_t i=0; i<size; i+=page_size/sizeof(char)) {
        var = array[i];
    }
}

int main(int argc, char **argv) {
    if (argc != 3) {
        return 1;
    }
    size_t page_size = sysconf(_SC_PAGESIZE);
    printf("Page Size is: %lu\n", page_size);

    size_t array_size = (size_t) atoi(argv[1]) * 1024 * 1024 * 1024;
    int n_runs = atoi(argv[2]);

    printf("Allocating array of size %lu\n", array_size);
    char *test_array = malloc(array_size);
    printf("End malloc\n");

    char input;
    while((input = getchar()) != 'e') {
        if (input == '\n') {
            continue;
        }
        clock_t start = clock();

        if (input == 'a') {
            for(int i=0; i < n_runs; i++) {
                access_array(test_array, array_size, page_size);
            }
        }
        else if (input == 'w') {
            for(int i=0; i < n_runs; i++) {
                write_array(test_array, array_size, page_size);
            }
        }

        double elapsed = (double)(clock() - start) * 1000.0 / CLOCKS_PER_SEC;
        printf("Time elapsed: %.4f ms\n", elapsed);
    }

    getchar();
    free(test_array);
    return 0;
}
