#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>

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

char main(int argc, char **argv) {
    if (argc != 2) {
        return 1;
    }

    size_t page_size = sysconf(_SC_PAGESIZE);
    printf("Page Size is: %lu\n", page_size);

    size_t array_size = (size_t) atoi(argv[1]) * 1024 * 1024 * 1024;

    printf("Allocating array of size %lu\n", array_size);
    char *test_array = malloc(array_size);
    printf("End malloc\n");

    char input;
    while((input = getchar()) != 'e') {
        if (input == 'a') {
            access_array(test_array, array_size, page_size);
        }
        else if (input == 'w') {
            write_array(test_array, array_size, page_size);
        }
    }

    getchar();
    free(test_array);
    return 0;
}
