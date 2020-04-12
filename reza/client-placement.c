#include "placement.h"
#include "client-placement.h"

#include <stdio.h>
#include <stdlib.h>

#include <sys/socket.h>
#include <sys/un.h>

#include <errno.h>
#include <unistd.h>

int bind_uds(int addr_start, int addr_end) {
    // Unix domain socket
    struct sockaddr_un uds_addr;
    int unix_fd, w_ret;
    req_t bind_req;

    int pid = getpid();
    
    if((unix_fd = socket(AF_UNIX, SOCK_STREAM, 0)) == -1) {
        fprintf(stderr, "Error creating UD socket: %s\n", strerror(errno));
        return 0;
    }
    memset(&uds_addr, 0, sizeof(uds_addr));
    uds_addr.sun_family = AF_UNIX;

    strncpy(uds_addr.sun_path, UDS_path, sizeof(uds_addr.sun_path)-1);

    if(connect(unix_fd, (struct sockaddr*)&uds_addr, sizeof(uds_addr))) {
        fprintf(stderr, "Error connecting to server via UDS: %s\n", strerror(errno));
        return 0;
    }

    bind_req.op_code = BIND_OP;
    bind_req.pid_n = pid;

    if (w_ret = (write(unix_fd, &bind_req, sizeof(bind_req)) != sizeof(bind_req))) {
        if (w_ret == -1) {
            fprintf(stderr, "Error writing to UDS fd: %s\n", strerror(errno));
        }
        else {
            fprintf(stderr, "Unexpected amount of bytes written to UDS fd.\n");
        }

        return 0;
    }

    return 1;
}


int unbind_uds() {
    // Unix domain socket
    struct sockaddr_un uds_addr;
    int unix_fd, w_ret;
    req_t unbind_req;

    int pid = getpid();
    
    if((unix_fd = socket(AF_UNIX, SOCK_STREAM, 0)) == -1) {
        fprintf(stderr, "Error creating UD socket: %s\n", strerror(errno));
        return 0;
    }
    memset(&uds_addr, 0, sizeof(uds_addr));
    uds_addr.sun_family = AF_UNIX;

    strncpy(uds_addr.sun_path, UDS_path, sizeof(uds_addr.sun_path)-1);

    if(connect(unix_fd, (struct sockaddr*)&uds_addr, sizeof(uds_addr))) {
        fprintf(stderr, "Error connecting to server via UDS: %s\n", strerror(errno));
        return 0;
    }

    unbind_req.op_code = UNBIND_OP;
    unbind_req.pid_n = pid;

    if (w_ret = (write(unix_fd, &unbind_req, sizeof(unbind_req)) != sizeof(unbind_req))) {
        if (w_ret == -1) {
            fprintf(stderr, "Error writing to UDS fd: %s\n", strerror(errno));
        }
        else {
            fprintf(stderr, "Unexpected amount of bytes written to UDS fd.\n");
        }

        return 0;
    }

    return 1;
}