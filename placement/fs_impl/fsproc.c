#include <stdio.h>
#include <stdlib.h>
#include <numaif.h>

struct page {
    void *page; // page address
    int w_ratio; // write ratio
} page_t

struct page_list {
    page_t **pages; // list of pages from a process
    int pid; // pages owner
} page_list_t

page_list_t* pl;

int bind_pid(int pid) {
    fd = fopen("", "r");
}

int write_req(int pid, void *page) {
    
}

int read_req(int pid, void *page) {

}


int main(int argc, char** argv) {
    if(argc != 3) {
        printf("USAGE:")
    }
}
