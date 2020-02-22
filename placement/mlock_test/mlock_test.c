#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <string.h>
#include <time.h>
#include <errno.h>

#include <unistd.h>


extern int errno;

#define RAM_SIZE 128

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

    size_t array_size = (size_t) RAM_SIZE * 1024 * 1024 * 1024 * 0.3;

    // make number divisible by array type
    size_t rem = array_size % sizeof(int);
    array_size -= rem;


    printf("Allocating array of size %lu\n", array_size);
    int* test_array = malloc(array_size);
    printf("End malloc\n");
    access_array(test_array, array_size/sizeof(int), page_size);

    clock_t t;
    printf("Starting mlockall()\n");
    t = clock();
    if(mlockall(MCL_FUTURE)) {
        fprintf(stderr, "mlockall error: %s\n", strerror(errno));
        free(test_array);
        return 1;
    }
    t = clock() - t;
    printf("mlockall() complete: took %f seconds\n", ((double)t)/CLOCKS_PER_SEC);
    char r[4];
    while(1) {
        scanf("%s", r);
        if(!strcmp(r, "exit")) {
            break;
        }
    }
    if(munlockall()) {
        fprintf(stderr, "munlockall error: %s\n", strerror(errno));
        free(test_array);
        return 1;
    }
    free(test_array);
    return 0;
}
