#include "placement.h"


#include <sys/socket.h>
#include <sys/un.h>
#include <linux/netlink.h>

#include <pthread.h>

#include <numaif.h>
#include <numa.h>
#include <math.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>
#include <errno.h>

struct sockaddr_nl src_addr, dst_addr;
struct nlmsghdr *nlmh;
struct iovec iov;
int netlink_fd;
struct msghdr msg;

long page_size;

int do_NVRAM_DRAM = 1;

volatile int exit_sig = 0;

req_t req;
addr_info_t candidates[MAX_N_FIND]; // contains candidate pages that result from a find command
addr_info_t op_retval;

pthread_t stdin_thread, socket_thread, placement_thread;
pthread_mutex_t comm_lock;

void configure_iov_mhdr() {
    iov.iov_base = (void *)nlmh;
    iov.iov_len = nlmh->nlmsg_len;
    msg.msg_name = (void *) &dst_addr;
    msg.msg_namelen = sizeof(dst_addr);
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
}

void configure_hdr() {
    memset(nlmh, 0, NLMSG_SPACE(MAX_PAYLOAD));
    nlmh->nlmsg_len = NLMSG_SPACE(MAX_PAYLOAD);
    nlmh->nlmsg_pid = getpid();
    nlmh->nlmsg_flags = 0;
}

void configure_addrs() {
    // source address
    memset(&src_addr, 0, sizeof(src_addr));
    src_addr.nl_family = AF_NETLINK;
    src_addr.nl_pid = getpid();
    src_addr.nl_groups = 0; // unicast

    // destination address
    memset(&dst_addr, 0, sizeof(dst_addr));
    dst_addr.nl_family = AF_NETLINK;
    dst_addr.nl_pid = 0; // kernel
    dst_addr.nl_groups = 0; // unicast
}

int send_bind(int pid) {
    pthread_mutex_lock(&comm_lock);
    req.op_code = BIND_OP;
    req.pid_n = pid;

    memcpy(NLMSG_DATA(nlmh), &req, sizeof(req_t));
    sendmsg(netlink_fd, &msg, 0);

    recvmsg(netlink_fd, &msg, 0);

    memcpy(NLMSG_DATA(nlmh), &op_retval, sizeof(addr_info_t));
    if(!op_retval.pid_retval) {
        pthread_mutex_unlock(&comm_lock);
        return 0;
    }

    pthread_mutex_unlock(&comm_lock);
    return 1;
}

int send_unbind(int pid) {
    pthread_mutex_lock(&comm_lock);
    req.op_code = UNBIND_OP;
    req.pid_n = pid;

    memcpy(NLMSG_DATA(nlmh), &req, sizeof(req_t));
    sendmsg(netlink_fd, &msg, 0);

    recvmsg(netlink_fd, &msg, 0);

    memcpy(NLMSG_DATA(nlmh), &op_retval, sizeof(addr_info_t));
    if(!op_retval.pid_retval) {
        pthread_mutex_unlock(&comm_lock);
        return 0;
    }

    pthread_mutex_unlock(&comm_lock);
    return 1;
}

int send_find(int n_pages, int mode) {
    pthread_mutex_lock(&comm_lock);

    req.op_code = UNBIND_OP;
    req.pid_n = n_pages;
    req.mode = mode;

    memcpy(NLMSG_DATA(nlmh), &req, sizeof(req_t));
    sendmsg(netlink_fd, &msg, 0);

    recvmsg(netlink_fd, &msg, 0);

    memcpy(NLMSG_DATA(nlmh), &candidates, sizeof(addr_info_t) * n_pages);


    int n_found=0;
    int n_migrated=0;

    while(candidates[n_found++].pid_retval > 0);

    int dest_node = DRAM_NODE;
    if(mode == DRAM_MODE) {
        dest_node = NVRAM_NODE;

        if(candidates[n_found].pid_retval == -2) {
            do_NVRAM_DRAM = 0;
        }
    }

    void **addr = malloc(sizeof(unsigned long) * n_found);
    int *dest_nodes = malloc(sizeof(int *) * n_found);
    int *status = malloc(sizeof(int *) * n_found);

    for(int i=0; i<n_found; i++) {
        dest_nodes[i] = dest_node;
        status[i] = -123;
    }

    int j, curr_pid;
    for(int i=0; i < n_found; i+=j) {
        curr_pid=candidates[i].pid_retval;

        for(j=0; candidates[i+j].pid_retval == curr_pid; j++) {
            addr[j] = (void *) candidates[i+j].addr;
        }

        if(numa_move_pages(curr_pid, (unsigned long) j, addr, dest_nodes, status, 0)) {
            free(addr);
            free(dest_nodes);
            free(status);
            pthread_mutex_unlock(&comm_lock);
            return n_migrated;
        }
        n_migrated += j;
    }

    free(addr);
    free(dest_nodes);
    free(status);
    pthread_mutex_unlock(&comm_lock);
    return n_found;
}

void *decide_placement(void *args) {
    long long node_sz;
    long long node_fr = 1;
    float usage;
    int n_pages;

    while(!exit_sig) {
        node_sz = numa_node_size64(DRAM_NODE, &node_fr);
        usage = node_fr / node_sz;

        if(usage > (DRAM_TARGET+DRAM_THRESH)) {
            n_pages = ceil((usage - DRAM_TARGET) * node_sz / (page_size  / 1024));
            n_pages = fmin(n_pages, MAX_N_FIND);
            int n_migrated = send_find(n_pages, DRAM_MODE);

            if(n_migrated > 0) {
                do_NVRAM_DRAM = 1;
            }
            printf("DRAM-NVRAM: Migrated %d out of %d pages.\n", n_migrated, n_pages);
        }

        else if(do_NVRAM_DRAM && (usage < (DRAM_TARGET-DRAM_THRESH))) {
            n_pages = ceil((DRAM_TARGET - usage) * node_sz / (page_size  / 1024));
            n_pages = fmin(n_pages, MAX_N_FIND);
            int n_migrated = send_find(n_pages, NVRAM_MODE);
            printf("NVRAM-DRAM: Migrated %d out of %d pages.\n", n_migrated, n_pages);
        }

        sleep(MEMCHECK_INTERVAL);
    }

    return NULL;
}

void *process_stdin(void *args) {
    char *command = malloc(sizeof(char) * MAX_COMMAND_SIZE);
    char *substring;
    long pid;

    while((fgets(command, MAX_COMMAND_SIZE, stdin) != NULL) && strcmp(command, "exit")) {
        if((substring = strtok(command, " ")) == NULL) {
            continue;
        }
        
        if(!strcmp(substring, "bind")) {
            if((substring = strtok(NULL, " ")) == NULL) {
                fprintf(stderr, "Invalid argument for bind command.\n");
                continue;
            }
            pid = strtol(substring, NULL, 10);
            if((pid>0) && (pid<INT_MAX)) {
                if(send_bind((int) pid)) {
                    printf("Bind request success (pid=%d).\n", (int) pid);
                }
                else {
                    fprintf(stderr, "Bind request failed (pid=%d).\n", (int) pid);
                }
            }
            else {
                fprintf(stderr, "Invalid argument for bind command.\n");
            }
        }

        else if(!strcmp(substring, "unbind")) {
            if((substring = strtok(NULL, " ")) == NULL) {
                fprintf(stderr, "Invalid argument for unbind command.\n");
                continue;
            }
            pid = strtol(substring, NULL, 10);
            if((pid>0) && (pid<INT_MAX)) {
                if(send_unbind((int) pid)) {
                    printf("Unbind request success (pid=%d).\n", (int) pid);
                }
                else {
                    fprintf(stderr, "Unbind request failed (pid=%d).\n", (int) pid);
                }
            }
            else {
                fprintf(stderr, "Invalid argument for unbind command.\n");
            }
        }

        else {
            fprintf(stderr, "Unknown command.\nAvailable commands:\n\tbind [pid]\n\tunbind [pid]\n\texit\n");
        }

        //TODO: add console debug commands here

        // Testar modificar paginas "quentes"
        // Criar testes sinteticos.
        // 
    }
    exit_sig = 1;
    free(command);

    return NULL;
}

void *process_socket(void *args) {
    // Unix domain socket
    struct sockaddr_un uds_addr;
    int unix_fd, acc, rd;
    req_t unix_req;
    
    if((unix_fd = socket(AF_UNIX, SOCK_STREAM, 0)) == -1) {
        fprintf(stderr, "Error creating UD  fd: %s\n", strerror(errno));
        return NULL;
    }
    memset(&uds_addr, 0, sizeof(uds_addr));
    uds_addr.sun_family = AF_UNIX;

    strncpy(uds_addr.sun_path, UDS_path, sizeof(uds_addr.sun_path)-1);
    unlink(UDS_path); // unlink to avoid error in bind

    if (bind(unix_fd, (struct sockaddr*)&uds_addr, sizeof(uds_addr)) == -1) {
        fprintf(stderr, "Error binding UD socket fd: %s\n", strerror(errno));
        return NULL;
    }

    if (listen(unix_fd, MAX_BACKLOG) == -1) {
        fprintf(stderr, "Error marking UD socket fd as passive: %s\n", strerror(errno));
        return NULL;
    }

    while(!exit_sig) {
        if ((acc = accept(unix_fd, NULL, NULL)) == -1) {
            fprintf(stderr, "Failed accepting incoming UD socket fd connection: %s\n", strerror(errno));
            continue;
        }

        while( (rd = read(acc,&unix_req,sizeof(req_t))) == sizeof(req_t)) {
            switch(unix_req.op_code) {
                case BIND_OP:
                    if(send_bind(unix_req.pid_n)) {
                        printf("Bind request success (pid=%d).\n", unix_req.pid_n);
                    }
                    else {
                        fprintf(stderr, "Bind request failed (pid=%d).\n", unix_req.pid_n);
                    }
                    break;
                case UNBIND_OP:
                    if(send_unbind(unix_req.pid_n)) {
                        printf("Unbind request success (pid=%d).\n", unix_req.pid_n);
                    }
                    else {
                        fprintf(stderr, "Unbind request failed (pid=%d).\n", unix_req.pid_n);
                    }
                    break;
                default:
                    fprintf(stderr, "Unexpected request OPcode from accepted UD socket connection");
            }
        }

        if (rd < 0) {
            fprintf(stderr, "Error reading from accepted UD socket connection: %s\n", strerror(errno));
            return NULL;
        }

        else if (rd == 0) {
            close(acc);
        }
        else {
            fprintf(stderr, "Unexpected amount of bytes read from accepted UD socket connection.\n");
        }
    }

    return NULL;
}

int main() {

    if((netlink_fd = socket(PF_NETLINK, SOCK_RAW, NETLINK_USER)) == -1) {
        fprintf(stderr, "Could not create netlink socket fd: %s\nTry inserting kernel module first.\n", strerror(errno));
        return 1;
    }

    nlmh = malloc(NLMSG_SPACE(MAX_PAYLOAD));
    page_size = sysconf(_SC_PAGESIZE);

    configure_addrs();

    if(bind(netlink_fd, (struct sockaddr *) &src_addr, sizeof(src_addr))) {
        printf("Error binding netlink socket fd: %s\n", strerror(errno));
        free(nlmh);
        return 1;
    }
    configure_hdr();
    configure_iov_mhdr();

    if(pthread_mutex_init(&comm_lock, NULL)) {
        fprintf(stderr, "Error creating communication mutex lock: %s\n", strerror(errno));
        free(nlmh);
        return 1;
    }
    
    if(pthread_create(&stdin_thread, NULL, process_stdin, NULL)) {
        fprintf(stderr, "Error spawning stdin thread: %s\n", strerror(errno));
        free(nlmh);
        return 1;
    }

    if(pthread_create(&socket_thread, NULL, process_socket, NULL)) {
        fprintf(stderr, "Error spawning socket thread: %s\n", strerror(errno));
        free(nlmh);
        return 1;
    }

    if(pthread_create(&placement_thread, NULL, decide_placement, NULL)) {
        fprintf(stderr, "Error spawning placement thread: %s\n", strerror(errno));
        free(nlmh);
        return 1;
    }

    pthread_join(stdin_thread, NULL);
    pthread_join(socket_thread, NULL);
    pthread_join(placement_thread, NULL);

    pthread_mutex_destroy(&comm_lock);

    close(netlink_fd);

    free(nlmh);
    return 0;
}