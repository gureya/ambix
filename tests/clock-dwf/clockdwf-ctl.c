#include "clockdwf.h"

#include <sys/socket.h>
#include <sys/select.h>
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

int netlink_fd;

long page_size;

struct sockaddr_nl src_addr, dst_addr;
struct nlmsghdr *nlmh_out;//, *nlmh_in;

char *buffer;
int buf_size;

addr_info_t *candidates;

struct iovec iov_out, iov_in;
struct msghdr msg_out, msg_in;

volatile int exit_sig = 0;

pthread_t stdin_thread, socket_thread, threshold_thread, switch_thread;
pthread_mutex_t comm_lock, placement_lock;

void configure_netlink_addr() {
    /* Source and destination addresses config */
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

void configure_netlink_outbound() {
    
    /* netlink message header config */
    nlmh_out->nlmsg_len = NLMSG_SPACE(MAX_PAYLOAD);
    nlmh_out->nlmsg_pid = getpid();
    nlmh_out->nlmsg_flags = 0;

    /* IO vector out config */
    iov_out.iov_base = (void *) nlmh_out;
    iov_out.iov_len = nlmh_out->nlmsg_len;

    /* message header outconfig */
    msg_out.msg_name = (void *) &dst_addr;
    msg_out.msg_namelen = sizeof(dst_addr);
    msg_out.msg_iov = &iov_out;
    msg_out.msg_iovlen = 1;
}

void configure_netlink_inbound() {

    /* IO vector in config */
    iov_in.iov_base = (void *) buffer;
    iov_in.iov_len = buf_size;
    
    /* message header in config */
    msg_in.msg_name = (void *) &dst_addr;
    msg_in.msg_namelen = sizeof(dst_addr);
    msg_in.msg_iov = &iov_in;
    msg_in.msg_iovlen = 1;
}

int send_req(req_t req, addr_info_t **out) {

    pthread_mutex_lock(&comm_lock);
    
    memset(NLMSG_DATA(nlmh_out), 0, MAX_PAYLOAD);
    memcpy(NLMSG_DATA(nlmh_out), &req, sizeof(req));
    sendmsg(netlink_fd, &msg_out, 0);

    //configure_netlink_inbound();
    memset(buffer, 0, buf_size);
    int len = recvmsg(netlink_fd, &msg_in, 0);

    addr_info_t *curr_pointer = *out;
    int i = 0;
    struct nlmsghdr * curr_nlmh;
    for (curr_nlmh = (struct nlmsghdr *) buffer; NLMSG_OK(curr_nlmh, len); curr_nlmh = NLMSG_NEXT(curr_nlmh, len)) {
        if(curr_nlmh->nlmsg_type == NLMSG_ERROR) {
            pthread_mutex_unlock(&comm_lock);
            return 0;
        }
        int payload_len = NLMSG_PAYLOAD(curr_nlmh, 0);
        memcpy(curr_pointer, (addr_info_t *) NLMSG_DATA(curr_nlmh), payload_len);
        curr_pointer += payload_len/sizeof(addr_info_t);
        i++;

    }
    //printf("Received a total of %d packets.\n", i);
    pthread_mutex_unlock(&comm_lock);
    return 1;
}

// void print_command(char *cmd) {
//   FILE *fp;
//   char buf[1024];

//   if ((fp = popen(cmd, "r")) == NULL) {
//     perror("popen");
//     exit(-1);
//   }

//   while(fgets(buf, sizeof(buf), fp) != NULL) {
//     printf("%s", buf);
//   }

//   if(pclose(fp))  {
//     perror("pclose");
//     exit(-1);
//   }
// }

// void print_node_allocations(int pid) {
//     char buf[1024];
//     snprintf(buf, sizeof(buf), "numastat -c %d", getpid());
//     printf("\x1B[32m");
//     print_command(buf);
//     printf("\x1B[0m");
// }

int send_bind(int pid) {
    req_t req;
    addr_info_t *op_retval = malloc(sizeof(addr_info_t));

    req.op_code = BIND_OP;
    req.pid_n = pid;

    send_req(req, &op_retval);
    if(op_retval->pid_retval == 0) {
        free(op_retval);
        return 1;
    }
    free(op_retval);
    return 0;
}

int send_unbind(int pid) {
    req_t req;
    addr_info_t *op_retval = malloc(sizeof(addr_info_t));

    req.op_code = UNBIND_OP;
    req.pid_n = pid;

    send_req(req, &op_retval);
    if(op_retval->pid_retval == 0) {
        free(op_retval);
        return 1;
    }
    free(op_retval);
    return 0;
}

int do_migration(int dest_node, int n_found) {
    int n_migrated = 0;

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
            break;
        }
        n_migrated += j;
    }
    free(addr);
    free(dest_nodes);
    free(status);
    return n_migrated;
}

int do_switch(int n_found) {
    int n_switched = 0;
    void **addr = malloc(sizeof(unsigned long));
    int *dest_node = malloc(sizeof(int *));
    int *status = malloc(sizeof(int *));
    int pid;

    status[0] = -123;

    for(int i=0; i<n_found; i++) {
        // alternate migrations to prevent reaching 100% usage in either node.
        pid = candidates[i+n_found+1].pid_retval;
        addr[0] = (void *) candidates[i+n_found+1].addr;
        dest_node[0] = NVRAM_NODE;
        if(numa_move_pages(pid, 1, addr, dest_node, status, 0)) {
            printf("Failed migration to nvram::%d:%p\n", pid, addr[0]);
            break;
        }
        pid = candidates[i].pid_retval;
        addr[0] = (void *) candidates[i].addr;
        dest_node[0] = DRAM_NODE;
        if(numa_move_pages(pid, 1, addr, dest_node, status, 0)) {
            printf("Failed migration to dram::%d:%p\n", pid, addr[0]);
            break;
        }
        n_switched++;
    }
    free(addr);
    free(dest_node);
    free(status);
    return n_switched;
}

int send_find(int n_pages, int mode) {
    req_t req;

    req.op_code = FIND_OP;
    req.pid_n = n_pages;
    req.mode = mode;

    
    send_req(req, &candidates);

    int n_found=-1;

    while(candidates[++n_found].pid_retval > 0);

    if(n_found == 0) {
        return 0;
    }
    int dest_node;
    switch(mode) {
        case DRAM_MODE:
            dest_node = NVRAM_NODE;
            return do_migration(dest_node, n_found);
            break;
        case NVRAM_MODE:
            dest_node = DRAM_NODE;
            return do_migration(dest_node, n_found);
            break;
        case SWITCH_MODE:
            return do_switch(n_found);
            break;
    }
    return 0;
}

void *switch_placement(void *args) {
    while(!exit_sig) {
        pthread_mutex_lock(&placement_lock);
        int n_switched = send_find(MAX_N_SWITCH, SWITCH_MODE);
        if(n_switched > 0) {
            printf("DRAM<->NVRAM: Switched %d out of %d pages.\n", n_switched, (int) MAX_N_SWITCH);
        }
        pthread_mutex_unlock(&placement_lock);
        sleep(SWITCH_INTERVAL);
    }

    return NULL;
}

void *threshold_placement(void *args) {
    long long node_sz;
    long long node_fr = 1;
    float usage;
    int n_pages;

    while(!exit_sig) {
        node_sz = numa_node_size64(DRAM_NODE, &node_fr);
        usage = 1.0 * (node_sz - node_fr) / node_sz;

        printf("Current DRAM Usage: %0.2f%%\n", usage*100);
        pthread_mutex_lock(&placement_lock);
        if(usage > (DRAM_TARGET+DRAM_THRESH_PLUS)) {
            int n_bytes = (usage - DRAM_TARGET) * node_sz;
            n_pages = ceil(n_bytes/page_size);
            n_pages = fmin(n_pages, MAX_N_FIND);
            int n_migrated = send_find(n_pages, DRAM_MODE);
            if(n_migrated > 0) {
                printf("DRAM->NVRAM: Migrated %d out of %d pages.\n", n_migrated, n_pages);
            }
        }

        else if(usage < (DRAM_TARGET-DRAM_THRESH_NEGATIVE)) {
            int n_bytes = (DRAM_TARGET - usage) * node_sz;
            n_pages = ceil(n_bytes/page_size);
            n_pages = fmin(n_pages, MAX_N_FIND);
            int n_migrated = send_find(n_pages, NVRAM_MODE);
            if(n_migrated > 0) {
                printf("NVRAM->DRAM: Migrated %d out of %d pages.\n", n_migrated, n_pages);
            }
        }
        pthread_mutex_unlock(&placement_lock);
        sleep(MEMCHECK_INTERVAL);
    }

    return NULL;
}

void *process_stdin(void *args) {
    char *command = malloc(sizeof(char) * MAX_COMMAND_SIZE);
    char *substring;
    long pid;

    printf("Available commands:\n"
            "\tbind [pid]\n"
            "\tunbind [pid]\n"
            "\texit\n");

    while((fgets(command, MAX_COMMAND_SIZE, stdin) != NULL) && strcmp(command, "exit\n")) {
        if((substring = strtok(command, " ")) == NULL) {
            continue;
        }
        
        if(!strcmp(substring, "bind")) {
            if((substring = strtok(NULL, " ")) == NULL) {
                fprintf(stderr, "Invalid argument for bind command.\n");
                continue;
            }
            pid = strtol(substring, NULL, 10);
            if((pid>0) && (pid<MAX_PID_N)) {
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
            if((pid>0) && (pid<MAX_PID_N)) {
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
            fprintf(stderr, "Unknown command.\n"
                "Available commands:\n"
                "\tbind [pid]\n"
                "\tunbind [pid]\n"
                "\texit\n");

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
    int unix_fd, sel, acc, rd;
    req_t unix_req;

    struct timeval sel_timeout;
    fd_set readfds;
    
    
    if((unix_fd = socket(AF_UNIX, SOCK_STREAM, 0)) == -1) {
        fprintf(stderr, "Error creating UD socket: %s\n", strerror(errno));
        return NULL;
    }
    memset(&uds_addr, 0, sizeof(uds_addr));
    uds_addr.sun_family = AF_UNIX;

    strncpy(uds_addr.sun_path, UDS_path, sizeof(uds_addr.sun_path)-1);
    unlink(UDS_path); // unlink to avoid error in bind

    if (bind(unix_fd, (struct sockaddr*)&uds_addr, sizeof(uds_addr)) == -1) {
        fprintf(stderr, "Error binding UDS: %s\n", strerror(errno));
        return NULL;
    }

    if (listen(unix_fd, MAX_BACKLOG) == -1) {
        fprintf(stderr, "Error marking UDS as passive: %s\n", strerror(errno));
        return NULL;
    }

    while(!exit_sig) {
        sel_timeout.tv_sec = SELECT_TIMEOUT;

        FD_ZERO(&readfds);
        FD_SET(unix_fd, &readfds);
        sel = select(unix_fd+1, &readfds, NULL, NULL, &sel_timeout);

        if (sel == -1) {
            fprintf(stderr, "Error in UDS select: %s.\n", strerror(errno));
            return NULL;
        } else if ((sel > 0) && FD_ISSET(unix_fd, &readfds)) {
            if ((acc = accept(unix_fd, NULL, NULL)) == -1) {
                fprintf(stderr, "Failed accepting incoming UDS connection: %s\n", strerror(errno));
                continue;
            }
            while( (rd = read(acc, &unix_req,sizeof(req_t))) == sizeof(req_t)) {
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
                fprintf(stderr, "Error reading from accepted UDS connection: %s\n", strerror(errno));
                return NULL;
            }

            else if (rd == 0) {
                close(acc);
            }
            else {
                fprintf(stderr, "Unexpected amount of bytes read from accepted UD socket connection.\n");
            }
        }
    }
    unlink(UDS_path);
    return NULL;
}

int main() {

    if((netlink_fd = socket(PF_NETLINK, SOCK_RAW, NETLINK_USER)) == -1) {
        fprintf(stderr, "Could not create netlink socket fd: %s\nTry inserting kernel module first.\n", strerror(errno));
        return 1;
    }
    page_size = sysconf(_SC_PAGESIZE);
    int packet_size = NLMSG_SPACE(MAX_PAYLOAD);
    buf_size = packet_size * MAX_PACKETS;

    candidates = malloc(sizeof(addr_info_t) * MAX_N_FIND);

    buffer = malloc(buf_size);

    nlmh_out = malloc(packet_size);

    configure_netlink_addr();
    configure_netlink_outbound();
    configure_netlink_inbound();

    if(bind(netlink_fd, (struct sockaddr *) &src_addr, sizeof(src_addr))) {
        printf("Error binding netlink socket fd: %s\n", strerror(errno));
        free(candidates);
        free(buffer);
        free(nlmh_out);
        return 1;
    }

    if(pthread_mutex_init(&comm_lock, NULL)) {
        fprintf(stderr, "Error creating communication mutex lock: %s\n", strerror(errno));
    }

    else if(pthread_mutex_init(&placement_lock, NULL)) {
        fprintf(stderr, "Error creating placement mutex lock: %s\n", strerror(errno));
    }
    
    else if(pthread_create(&stdin_thread, NULL, process_stdin, NULL)) {
        fprintf(stderr, "Error spawning stdin thread: %s\n", strerror(errno));
    }

    else if(pthread_create(&socket_thread, NULL, process_socket, NULL)) {
        fprintf(stderr, "Error spawning socket thread: %s\n", strerror(errno));
    }

    else if(pthread_create(&threshold_thread, NULL, threshold_placement, NULL)) {
        fprintf(stderr, "Error spawning thresold placement thread: %s\n", strerror(errno));
    }

    else if(pthread_create(&switch_thread, NULL, switch_placement, NULL)) {
        fprintf(stderr, "Error spawning switch thread: %s\n", strerror(errno));
    }

    else {
        pthread_join(stdin_thread, NULL);
        printf("Exiting ctl...\n");
        pthread_join(socket_thread, NULL);
        pthread_join(threshold_thread, NULL);
        pthread_join(switch_thread, NULL);

        pthread_mutex_destroy(&comm_lock);
        pthread_mutex_destroy(&placement_lock);

        close(netlink_fd);
        free(candidates);
        free(buffer);
        free(nlmh_out);
        return 0;
    }
    close(netlink_fd);
    free(candidates);
    free(buffer);
    free(nlmh_out);
    return 1;
}
