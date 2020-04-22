#include <stdio.h>
#include "client-placement.h"


int main() {
    if(!bind_uds()) {
        return 1;
    }
    printf("BIND OK\n");

    int a[200];

    for(int i=0; i<50; i++) {
        a[i] = 1;
    }

    getchar();

    printf("%d\n", a[0]);

    if(!unbind_uds()) {
        return 1;
    }

    getchar();
    
    printf("UNBIND OK\n");
    return 0;
}