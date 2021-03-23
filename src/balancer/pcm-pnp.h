#ifndef _PCM_PNP_H
#define _PCM_PNP_H

#define MAX_SOCKETS 2
#define PCM_FILE_NAME "memdata"
#define PCM_DELAY 1
#define PMM_MIXED 0

typedef struct memdata {
    float sys_dramReads, sys_dramWrites;
    float sys_pmmReads, sys_pmmWrites;
    float sys_pmmAppBW, sys_pmmMemBW;
} memdata_t;


#endif
