#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <string.h>
#include <time.h>
#include <errno.h>


extern int errno;


#define ARRAY_SIZE 10000000

int main() {
    int* test_list = malloc(sizeof(int) * ARRAY_SIZE);
    clock_t t;
    printf("Starting mlockall()\n");
    t = clock();
    if(mlockall(MCL_FUTURE)) {
        fprintf(stderr, "mlockall error: %s\n", strerror(errno));
        free(test_list);
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
        free(test_list);
        return 1;
    }
    free(test_list);
    return 0;
}