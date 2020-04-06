#ifndef _PLACEMENT_H
#define _PLACEMENT_H

#define MAX_PID_SIZE 7 // maximum PID value is 2^22
#define MAX_PIDS 20

// Find-related constants:
#define DRAM_MODE 0
#define NVRAM_MODE 1
#define MAX_N_FIND 256 //262144 // Find up to 1GB of pages
#define MAX_CYCLES 2


// Node definition:
#define DRAM_NODE 0
#define NVRAM_NODE 1

// Netlink:
#define NETLINK_USER 31
#define MAX_PAYLOAD 4096 // Theoretical max is 32KB - netlink header - padding

// Unix Domain Socket:
#define UDS_path "\0socket"
#define MAX_BACKLOG 5

// Comm-related OP codes:
#define FIND_OP 0
#define BIND_OP 1
#define UNBIND_OP 2

// Comm-related structures:
typedef struct addr_info {
    unsigned long addr;
    int pid_retval; // Stores pid info for FIND operation and BIND/UNBIND ok/nok
} addr_info_t;

typedef struct req {
    int op_code;
    int pid_n; // Stores pid for BIND/UNBIND and the number of pages for FIND
    int mode;
} req_t;

//Client-ctl comms:
#define PORT 8080

// Misc:
#define MAX_COMMAND_SIZE 80
#define DRAM_TARGET 0.9
#define DRAM_THRESH 0.05
#define MEMCHECK_INTERVAL 4

#endif