#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <string.h>

#include <unistd.h>

#define RAM_SIZE 128 // in GB
#define WORKLOAD_PER 0.10

int access_array(int *array, size_t size, size_t page_size) {
    int var;
    for(size_t i=0; i<size; i+=page_size/sizeof(int)) {
	//printf("i=%lu\n", i);
        array[i] = 1;	
    }
}	

int main() {
    size_t page_size = sysconf(_SC_PAGESIZE);
    printf("Page Size is: %lu\n", page_size);

    size_t array_size = (size_t) RAM_SIZE * 1024 * 1024 * 1024 * WORKLOAD_PER;

    // make number divisible by array type
    size_t rem = array_size % sizeof(int);
    array_size -= rem;


    printf("Allocating array of size %lu\n", array_size);
    int* test_array = malloc(array_size);
    printf("End malloc\n");
    access_array(test_array, array_size/sizeof(int), page_size);

    char r[4];
    while(1) {
        printf("Test complete. Type \"exit\" to clean up and exit\n");
        scanf("%s", r);
        if(!strcmp(r, "exit")) {
            break;
        }
    }
    free(test_array);
    return 0;
}
