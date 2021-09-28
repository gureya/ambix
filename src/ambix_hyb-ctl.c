#include "ambix.h"
#include "pcm-ambix.h"

#include <sys/socket.h>
#include <sys/select.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <linux/netlink.h>

#include <pthread.h>

#include <numaif.h>
#include <numa.h>
#include <math.h>
#include <syscall.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>
#include <errno.h>

///// BEGIN GUREYA'S ADDITIONS - VARIABLES & HEADER FILES! /////
#include <sys/time.h>
#include <assert.h>
#include "tpool.h"

size_t max_num_threads = 28;  //Total number of threads for the thread pool!
size_t active_num_threads = 1;  //Current number of threads used to migrate pages!

//Object data
struct tpool_work {
  thread_func_t func;
  void *arg;
  struct tpool_work *next;
};
typedef struct tpool_work tpool_work_t;

//Work queue implemented as a simple linked list!
struct tpool {
  tpool_work_t *work_first;
  tpool_work_t *work_last;
  pthread_mutex_t work_mutex;
  pthread_cond_t work_cond;
  pthread_cond_t working_cond;
  size_t working_cnt;
  size_t thread_cnt;
  bool stop;
};

tpool_t *tm;

//What each thread will process!
struct thread_data {
  void **cur_addr;
  int *cur_status;
  int *cur_nodes;
  int thread_page_count;
  unsigned long thread_no;
  int cur_pid;
};

///// END GUREYA'S ADDITIONS - VARIABLES & HEADER FILES! /////

int netlink_fd;

long page_size;  // in kB

struct sockaddr_nl src_addr, dst_addr;
struct nlmsghdr *nlmh_out;  //, *nlmh_in;

char *buffer;
int buf_size;

addr_info_t *candidates;

struct iovec iov_out, iov_in;
struct msghdr msg_out, msg_in;

volatile int exit_sig = 0;
volatile int switch_act = 1;
volatile int thresh_act = 1;

// In microseconds
int memcheck_interval = MEMCHECK_INTERVAL * 1000;
int clear_interval = CLEAR_DELAY * 1000;

pthread_t stdin_thread, socket_thread, memcheck_thread;
pthread_mutex_t comm_lock, placement_lock;

////// BEGIN GUREYA'S ADDITIONS - FUNCTIONS! ////

/*
 * A simple helper function to compute the time diff in ms
 */
unsigned long time_diff(struct timeval *start, struct timeval *stop) {
  unsigned long sec_res = stop->tv_sec - start->tv_sec;
  unsigned long usec_res = stop->tv_usec - start->tv_usec;
  return 1000000 * sec_res + usec_res;
}

/*
 * Simple helper for creating work objects
 */
static tpool_work_t* tpool_work_create(thread_func_t func, void *arg) {
  tpool_work_t *work;

  if (func == NULL)
    return NULL;

  work = malloc(sizeof(*work));
  work->func = func;
  work->arg = arg;
  work->next = NULL;
  return work;
}

/*
 * Simple helper for destroying work objects
 */
static void tpool_work_destroy(tpool_work_t *work) {
  if (work == NULL)
    return;
  free(work);
}

/*
 * Helper for pulling work from the Queue
 */
static tpool_work_t* tpool_work_get(tpool_t *tm) {
  tpool_work_t *work;

  if (tm == NULL)
    return NULL;

  work = tm->work_first;
  if (work == NULL)
    return NULL;

  if (work->next == NULL) {
    tm->work_first = NULL;
    tm->work_last = NULL;
  } else {
    tm->work_first = work->next;
  }

  return work;
}

/*
 * The worker function!
 */
static void* tpool_worker(void *arg) {
  tpool_t *tm = arg;
  tpool_work_t *work;

  while (1) {
    pthread_mutex_lock(&(tm->work_mutex));

    while (tm->work_first == NULL && !tm->stop)
      pthread_cond_wait(&(tm->work_cond), &(tm->work_mutex));

    if (tm->stop)
      break;

    work = tpool_work_get(tm);
    tm->working_cnt++;
    pthread_mutex_unlock(&(tm->work_mutex));

    if (work != NULL) {
      work->func(work->arg);
      tpool_work_destroy(work);
    }

    pthread_mutex_lock(&(tm->work_mutex));
    tm->working_cnt--;
    if (!tm->stop && tm->working_cnt == 0 && tm->work_first == NULL)
      pthread_cond_signal(&(tm->working_cond));
    pthread_mutex_unlock(&(tm->work_mutex));
  }

  tm->thread_cnt--;
  pthread_cond_signal(&(tm->working_cond));
  pthread_mutex_unlock(&(tm->work_mutex));
  return NULL;
}

/*
 * Pool create function!
 *
 * The minimum number of threads in the pool is 2!
 *
 */
tpool_t* tpool_create(size_t num) {
  tpool_t *tm;
  pthread_t thread;
  size_t i;

  if (num == 0)
    num = 2;

  tm = calloc(1, sizeof(*tm));
  tm->thread_cnt = num;

  pthread_mutex_init(&(tm->work_mutex), NULL);
  pthread_cond_init(&(tm->work_cond), NULL);
  pthread_cond_init(&(tm->working_cond), NULL);

  tm->work_first = NULL;
  tm->work_last = NULL;

  for (i = 0; i < num; i++) {
    pthread_create(&thread, NULL, tpool_worker, tm);
    pthread_detach(thread);
  }

  printf("\nThread pool has been created successfully\n");

  return tm;
}

/*
 * Pool destroy function!
 */
void tpool_destroy(tpool_t *tm) {
  tpool_work_t *work;
  tpool_work_t *work2;

  if (tm == NULL)
    return;

  pthread_mutex_lock(&(tm->work_mutex));
  work = tm->work_first;
  while (work != NULL) {
    work2 = work->next;
    tpool_work_destroy(work);
    work = work2;
  }
  tm->stop = true;
  pthread_cond_broadcast(&(tm->work_cond));
  pthread_mutex_unlock(&(tm->work_mutex));

  tpool_wait(tm);

  pthread_mutex_destroy(&(tm->work_mutex));
  pthread_cond_destroy(&(tm->work_cond));
  pthread_cond_destroy(&(tm->working_cond));

  free(tm);
}

/*
 * Adding work to the Queue!
 */
bool tpool_add_work(tpool_t *tm, thread_func_t func, void *arg) {
  tpool_work_t *work;

  if (tm == NULL)
    return false;

  work = tpool_work_create(func, arg);
  if (work == NULL)
    return false;

  pthread_mutex_lock(&(tm->work_mutex));
  if (tm->work_first == NULL) {
    tm->work_first = work;
    tm->work_last = tm->work_first;
  } else {
    tm->work_last->next = work;
    tm->work_last = work;
  }

  pthread_cond_broadcast(&(tm->work_cond));
  pthread_mutex_unlock(&(tm->work_mutex));

  return true;
}

/*
 * Waiting for processing to complete
 * This is a blocking function that only returns when there is no work!
 */
void tpool_wait(tpool_t *tm) {
  if (tm == NULL)
    return;

  pthread_mutex_lock(&(tm->work_mutex));
  while (1) {
    if ((!tm->stop && tm->working_cnt != 0)
        || (tm->stop && tm->thread_cnt != 0)) {
      pthread_cond_wait(&(tm->working_cond), &(tm->work_mutex));
    } else {
      break;
    }
  }
  pthread_mutex_unlock(&(tm->work_mutex));

  printf("\n All threads have finished their work, there is no more work\n");
}

/*
 * thread work function
 */
void worker(void *pdata) {
  int rc;
  struct thread_data *tn = pdata;

  //measure the time taken by move_pages system call by this thread!
  struct timeval tstart, tend;
  unsigned long exec_time;

  gettimeofday(&tstart, NULL);
  rc = move_pages(tn->cur_pid, tn->thread_page_count, tn->cur_addr,
                  tn->cur_nodes, tn->cur_status, 0);
  if (rc < 0) {
    perror("move_pages");
  }
  gettimeofday(&tend, NULL);

  exec_time = time_diff(&tstart, &tend);
  printf("thread_no: %ld\t my_page_count: %d \tmove_pages concluded in %ldms\n",
         tn->thread_no, tn->thread_page_count, exec_time / 1000);

  free(pdata);
}

////// END GUREYA'S ADDITIONS - FUNCTIONS! ////

/*
 -------------------------------------------------------------------------------

 NETLINK CONFIGURATION

 -------------------------------------------------------------------------------
 */

void configure_netlink_addr() {
  /* Source and destination addresses config */
  // source address
  memset(&src_addr, 0, sizeof(src_addr));
  src_addr.nl_family = AF_NETLINK;
  src_addr.nl_pid = getpid();
  src_addr.nl_groups = 0;  // unicast

  // destination address
  memset(&dst_addr, 0, sizeof(dst_addr));
  dst_addr.nl_family = AF_NETLINK;
  dst_addr.nl_pid = 0;  // kernel
  dst_addr.nl_groups = 0;  // unicast
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

/*
 -------------------------------------------------------------------------------

 HELPER FUNCTIONS

 -------------------------------------------------------------------------------
 */

int check_memdata(memdata_t *md) {
  if ((md == NULL)
      || !BETWEEN(
          md->sys_dramReads,
          0,
          DRAM_BW_MAX) || !BETWEEN(md->sys_dramWrites, 0, DRAM_BW_MAX)
          || !BETWEEN(md->sys_pmmReads, 0, NVRAM_BW_MAX) || !BETWEEN(md->sys_pmmWrites, 0, NVRAM_BW_MAX)
          || !BETWEEN(md->sys_pmmAppBW, 0, NVRAM_BW_MAX) || !BETWEEN(md->sys_pmmMemBW, 0, NVRAM_BW_MAX)) {
    return 0;
  }

  return 1;
}

memdata_t *read_memdata() {
  memdata_t *md = malloc(sizeof(memdata_t));

  FILE * in_file = fopen(PCM_FILE_NAME, "r");
  if (in_file == NULL) {
    fprintf(stderr, "Error opening memdata file.\n");
    return md;
  }
  if (fread(md, sizeof(memdata_t), 1, in_file) != 1) {
    fprintf(stderr, "Error reading memdata from file.\n");
  }

  fclose(in_file);

  return md;
}

time_t get_memdata_mtime() {
  struct stat st;
  if (stat(PCM_FILE_NAME, &st) != -1) {
    return st.st_mtime;
  }
  return 0;
}

long long free_space_node(int node, long long *sz) {
  long long node_fr = 0;
  *sz = numa_node_size64(node, &node_fr);
  return node_fr;
}

long long free_space_tot_bytes(int mode, long long *sz) {

  long long total_node_sz = 0;
  long long total_node_fr = 0;

  if (mode == DRAM_MODE) {
    for (int i = 0; i < n_dram_nodes; i++) {
      long long node_sz = 0;
      total_node_fr += free_space_node(DRAM_NODES[i], &node_sz);
      total_node_sz += node_sz;
    }
  } else {
    for (int i = 0; i < n_nvram_nodes; i++) {
      long long node_sz = 0;
      total_node_fr += free_space_node(NVRAM_NODES[i], &node_sz);
      total_node_sz += node_sz;
    }
  }

  *sz = total_node_sz;
  return total_node_fr;
}

float free_space_per(int node) {
  long long sz = 0;
  long long fr = free_space_node(node, &sz);
  return 1.0 * (sz - fr) / sz;
}

float free_space_tot_per(int mode, long long *sz) {
  long long fr = free_space_tot_bytes(mode, sz);
  return 1.0 * (*sz - fr) / *sz;
}

int free_space_pages(int node) {
  long long sz = 0;
  return free_space_node(node, &sz) / page_size;
}

int free_space_tot_pages(int mode) {
  long long sz = 0;
  return free_space_tot_bytes(mode, &sz) / page_size;
}

/*
 -------------------------------------------------------------------------------

 MIGRATION FUNCTIONS

 -------------------------------------------------------------------------------
 */

int do_migration(int mode, int n_found) {
  void **addr = malloc(sizeof(unsigned long) * n_found);
  int *dest_nodes = malloc(sizeof(int) * n_found);
  int *status = malloc(sizeof(int) * n_found);

  const int *node_list;
  int n_nodes;

  if (mode == DRAM_MODE) {
    node_list = NVRAM_NODES;
    n_nodes = n_nvram_nodes;
    active_num_threads = 1;  //Moving pages to NVRAM, use one thread!
  } else {
    node_list = DRAM_NODES;
    n_nodes = n_dram_nodes;
    active_num_threads = max_num_threads;  //Moving pages to DRAM, use maximum number of threads!
  }

  for (int i = 0; i < n_found; i++) {
    status[i] = -123;
  }

  int n_processed = 0;
  for (int i = 0; (i < n_nodes) && (n_processed < n_found); i++) {
    int curr_node = node_list[i];

    int n_avail_pages = free_space_pages(curr_node);

    int j = 0;
    for (; (j < n_avail_pages) && (n_processed + j < n_found); j++) {
      addr[n_processed + j] = (void *) candidates[n_processed + j].addr;
      dest_nodes[n_processed + j] = curr_node;
    }

    n_processed += j;
  }
  int n_migrated, i;
  int e = 0;  // counts failed migrations

  for (n_migrated = 0, i = 0; n_migrated < n_processed; n_migrated += i) {
    int curr_pid;
    curr_pid = candidates[n_migrated].pid_retval;

    for (i = 1;
        (candidates[n_migrated + i].pid_retval == curr_pid)
            && (n_migrated + i < n_processed); i++)
      ;

    void **addr_displacement = addr + n_migrated;
    int *dest_nodes_displacement = dest_nodes + n_migrated;

    //// start of gureya's code ///
    //measure the time taken by move_pages system call by this thread!
    //struct timeval tstart, tend;
    //unsigned long exec_time;

    size_t j;
    struct thread_data *pdata;
    int start, end;

    int thread_page_count = i / active_num_threads;
    int remainder_ = i % active_num_threads;

    //gettimeofday(&tstart, NULL);
    for (j = 0; j < active_num_threads; j++) {
      pdata = malloc(sizeof(struct thread_data));
      assert(pdata);
      start = j * thread_page_count;
      end = start + thread_page_count;

      // last thread process the remainder
      if (remainder_ != 0 && j == (active_num_threads - 1)) {
        end = end + remainder_;
        thread_page_count += remainder_;
      }
      pdata->thread_page_count = thread_page_count;
      pdata->thread_no = j;
      pdata->cur_addr = malloc(sizeof(char*) * thread_page_count);
      pdata->cur_status = malloc(sizeof(int*) * thread_page_count);
      pdata->cur_nodes = malloc(sizeof(int*) * thread_page_count);
      if (!pdata->cur_addr || !pdata->cur_status || !pdata->cur_nodes) {
        printf("Unable to allocate memory\n");
        exit(1);
      }

      pdata->cur_addr = addr_displacement + start;
      pdata->cur_nodes = dest_nodes_displacement + start;
      pdata->cur_status = status + start;
      pdata->cur_pid = curr_pid;

      /*printf("thread_no: %ld\tstart: %d\tend: %d\tthread_page_count: %d\n",
       pdata->thread_no, start, end, pdata->thread_page_count);*/

      tpool_add_work(tm, worker, (void*) pdata);
    }

    //wait for the threads to finish work!
    tpool_wait(tm);
    //gettimeofday(&tend, NULL);
    /// end of gureya's code! ///

    // For now I assume that move pages never fails!
    /*if (move_pages(curr_pid, (unsigned long) i, addr_displacement,
     dest_nodes_displacement, status, 0)) {
     // Migrate all and output addresses that could not migrate
     for (int j = 0; j < i; j++) {
     if (move_pages(curr_pid, 1, addr_displacement + j,
     dest_nodes_displacement + j, status, 0)) {
     printf("Error migrating addr: %ld, pid: %d\n",
     (unsigned long) *(addr_displacement + j), curr_pid);
     e++;
     }
     }
     }*/
  }

  free(addr);
  free(dest_nodes);
  free(status);
  return n_migrated - e;
}

int do_switch(int n_found) {
  void **addr_dram = malloc(sizeof(unsigned long) * n_found);
  int *dest_nodes_dram = malloc(sizeof(int) * n_found);
  void **addr_nvram = malloc(sizeof(unsigned long) * n_found);
  int *dest_nodes_nvram = malloc(sizeof(int) * n_found);
  int *status = malloc(sizeof(int) * n_found);

  //measure the time taken by move_pages system call by this thread!
  //struct timeval tstart, tend;
  //unsigned long exec_time;

  for (int i = 0; i < n_found; i++) {
    status[i] = -123;
  }

  int dram_migrated = 0;
  int nvram_migrated = 0;
  int dram_e = 0;  // counts failed migrations
  int nvram_e = 0;  // counts failed migrations

  int dram_free = 1;
  int nvram_free = 1;

  while ((((dram_migrated + dram_e) < n_found)
      || ((nvram_migrated + nvram_e) < n_found)) && (dram_free || nvram_free)) {
    // DRAM -> NVRAM
    int old_n_processed = dram_migrated + dram_e;
    int dram_processed = old_n_processed;

    for (int i = 0; (i < n_nvram_nodes) && (dram_processed < n_found); i++) {
      int curr_node = NVRAM_NODES[i];

      long long node_fr = 0;
      numa_node_size64(curr_node, &node_fr);
      int n_avail_pages = node_fr / page_size;

      int j = 0;
      for (; (j < n_avail_pages) && (j + dram_processed < n_found); j++) {
        addr_dram[dram_processed + j] = (void *) candidates[n_found + 1 + j]
            .addr;
        dest_nodes_nvram[dram_processed + j] = curr_node;
      }

      dram_processed += j;
    }
    if (old_n_processed < dram_processed) {
      // Send processed pages to NVRAM
      int n_migrated, i;
      dram_free = 1;

      for (n_migrated = 0, i = 0; n_migrated < dram_processed; n_migrated +=
          i) {
        int curr_pid;
        curr_pid = candidates[n_found + 1 + n_migrated].pid_retval;

        for (i = 1;
            (candidates[n_found + 1 + n_migrated + i].pid_retval == curr_pid)
                && (n_migrated + i < dram_processed); i++)
          ;
        void **addr_displacement = addr_dram + n_migrated;
        int *dest_nodes_displacement = dest_nodes_nvram + n_migrated;

        //// start of gureya's code ///
        active_num_threads = 1;
        //pages are being sent to NVRAM
        size_t j;
        struct thread_data *pdata;
        int start, end;

        int thread_page_count = i / active_num_threads;
        int remainder_ = i % active_num_threads;

        //gettimeofday(&tstart, NULL);
        for (j = 0; j < active_num_threads; j++) {
          pdata = malloc(sizeof(struct thread_data));
          assert(pdata);
          start = j * thread_page_count;
          end = start + thread_page_count;

          // last thread process the remainder
          if (remainder_ != 0 && j == (active_num_threads - 1)) {
            end = end + remainder_;
            thread_page_count += remainder_;
          }
          pdata->thread_page_count = thread_page_count;
          pdata->thread_no = j;
          pdata->cur_addr = malloc(sizeof(char*) * thread_page_count);
          pdata->cur_status = malloc(sizeof(int*) * thread_page_count);
          pdata->cur_nodes = malloc(sizeof(int*) * thread_page_count);
          if (!pdata->cur_addr || !pdata->cur_status || !pdata->cur_nodes) {
            printf("Unable to allocate memory\n");
            exit(1);
          }

          pdata->cur_addr = addr_displacement + start;
          pdata->cur_nodes = dest_nodes_displacement + start;
          pdata->cur_status = status + start;
          pdata->cur_pid = curr_pid;

          /*printf("thread_no: %ld\tstart: %d\tend: %d\tthread_page_count: %d\n",
           pdata->thread_no, start, end, pdata->thread_page_count);*/

          tpool_add_work(tm, worker, (void*) pdata);
        }

        //wait for the threads to finish work!
        tpool_wait(tm);
        //gettimeofday(&tend, NULL);
        /// end of gureya's code! ///

        //Commented Miguel's code!
        /*if (numa_move_pages(curr_pid, (unsigned long) i, addr_displacement,
         dest_nodes_displacement, status, 0)) {
         // Migrate all and output addresses that could not migrate
         for (int j = 0; j < i; j++) {
         if (numa_move_pages(curr_pid, 1, addr_displacement + j,
         dest_nodes_displacement + j, status, 0)) {
         printf("Error migrating DRAM/MEM addr: %ld, pid: %d\n",
         (unsigned long) *(addr_displacement + j), curr_pid);
         dram_e++;
         }
         }
         }*/
      }
    } else {
      dram_free = 0;
    }

    dram_migrated = dram_processed - dram_e;

    // NVRAM -> DRAM
    old_n_processed = nvram_migrated + nvram_e;
    int nvram_processed = old_n_processed;

    for (int i = 0; (i < n_dram_nodes) && (nvram_processed < n_found); i++) {
      int curr_node = DRAM_NODES[i];

      long long node_fr = 0;
      numa_node_size64(curr_node, &node_fr);
      int n_avail_pages = node_fr / page_size;

      int j = 0;
      for (; (j < n_avail_pages) && (j + nvram_processed < n_found); j++) {
        addr_nvram[nvram_processed + j] = (void *) candidates[nvram_processed
            + j].addr;
        dest_nodes_dram[nvram_processed + j] = curr_node;
      }

      nvram_processed += j;
    }

    if (old_n_processed < nvram_processed) {
      // Send processed pages to DRAM
      int n_migrated, i;
      nvram_free = 1;

      for (n_migrated = 0, i = 0; n_migrated < nvram_processed; n_migrated +=
          i) {
        int curr_pid;
        curr_pid = candidates[n_migrated].pid_retval;

        for (i = 1;
            (candidates[n_migrated + i].pid_retval == curr_pid)
                && (n_migrated + i < nvram_processed); i++)
          ;
        void **addr_displacement = addr_nvram + n_migrated;
        int *dest_nodes_displacement = dest_nodes_dram + n_migrated;

        //// start of gureya's code ///
        //measure the time taken by move_pages system call by this thread!
        //struct timeval tstart, tend;
        //unsigned long exec_time;

        active_num_threads = max_num_threads;
        //pages are being sent to DRAM
        size_t j;
        struct thread_data *pdata;
        int start, end;

        int thread_page_count = i / active_num_threads;
        int remainder_ = i % active_num_threads;

        //gettimeofday(&tstart, NULL);
        for (j = 0; j < active_num_threads; j++) {
          pdata = malloc(sizeof(struct thread_data));
          assert(pdata);
          start = j * thread_page_count;
          end = start + thread_page_count;

          // last thread process the remainder
          if (remainder_ != 0 && j == (active_num_threads - 1)) {
            end = end + remainder_;
            thread_page_count += remainder_;
          }
          pdata->thread_page_count = thread_page_count;
          pdata->thread_no = j;
          pdata->cur_addr = malloc(sizeof(char*) * thread_page_count);
          pdata->cur_status = malloc(sizeof(int*) * thread_page_count);
          pdata->cur_nodes = malloc(sizeof(int*) * thread_page_count);
          if (!pdata->cur_addr || !pdata->cur_status || !pdata->cur_nodes) {
            printf("Unable to allocate memory\n");
            exit(1);
          }

          pdata->cur_addr = addr_displacement + start;
          pdata->cur_nodes = dest_nodes_displacement + start;
          pdata->cur_status = status + start;
          pdata->cur_pid = curr_pid;

          /*printf("thread_no: %ld\tstart: %d\tend: %d\tthread_page_count: %d\n",
           pdata->thread_no, start, end, pdata->thread_page_count);*/

          tpool_add_work(tm, worker, (void*) pdata);
        }

        //wait for the threads to finish work!
        tpool_wait(tm);
        //gettimeofday(&tend, NULL);
        /// end of gureya's code! ///

        //Commented Miguel's code, for now assume that move_pages never fails!
        /*if (numa_move_pages(curr_pid, (unsigned long) i, addr_displacement,
         dest_nodes_displacement, status, 0)) {
         // Migrate all and output addresses that could not migrate
         for (int j = 0; j < i; j++) {
         if (numa_move_pages(curr_pid, 1, addr_displacement + j,
         dest_nodes_displacement + j, status, 0)) {
         printf("Error migrating NVRAM addr: %ld, pid: %d\n",
         (unsigned long) *(addr_displacement + j), curr_pid);
         nvram_e++;
         }
         }
         }*/
      }
    } else {
      nvram_free = 0;
    }

    nvram_migrated = nvram_processed - nvram_e;
  }

  free(addr_dram);
  free(addr_nvram);
  free(dest_nodes_dram);
  free(dest_nodes_nvram);
  free(status);

  return dram_migrated + nvram_migrated;
}

/*
 -------------------------------------------------------------------------------

 REQUEST PROCESSING FUNCTIONS

 -------------------------------------------------------------------------------
 */

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
  for (curr_nlmh = (struct nlmsghdr *) buffer; NLMSG_OK(curr_nlmh, len);
      curr_nlmh = NLMSG_NEXT(curr_nlmh, len)) {
    if (curr_nlmh->nlmsg_type == NLMSG_ERROR) {
      pthread_mutex_unlock(&comm_lock);
      return 0;
    }
    int payload_len = NLMSG_PAYLOAD(curr_nlmh, 0);
    memcpy(curr_pointer, (addr_info_t *) NLMSG_DATA(curr_nlmh), payload_len);
    curr_pointer += payload_len / sizeof(addr_info_t);
    i++;

  }
  pthread_mutex_unlock(&comm_lock);
  return 1;
}

int send_bind(int pid) {
  req_t req;
  addr_info_t *op_retval = malloc(sizeof(addr_info_t));

  req.op_code = BIND_OP;
  req.pid_n = pid;

  send_req(req, &op_retval);
  if (op_retval->pid_retval == 0) {
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
  if (op_retval->pid_retval == 0) {
    free(op_retval);
    return 1;
  }
  free(op_retval);
  return 0;
}

int send_find(int n_pages, int mode) {
  req_t req;

  req.op_code = FIND_OP;
  req.pid_n = n_pages;
  req.mode = mode;

  send_req(req, &candidates);

  int n_found = -1;

  while (candidates[++n_found].pid_retval > 0)
    ;

  if (n_found == 0) {
    return 0;
  }
  switch (mode) {
    case DRAM_MODE:
      return do_migration(DRAM_MODE, n_found);
      break;
    case NVRAM_MODE:
    case NVRAM_INTENSIVE_MODE:
    case NVRAM_WRITE_MODE:
      return do_migration(NVRAM_MODE, n_found);
      break;
    case SWITCH_MODE:
      return do_switch(n_found);
      break;
  }
  return 0;
}

/*
 -------------------------------------------------------------------------------

 PLACEMENT FUNCTIONS

 -------------------------------------------------------------------------------
 */

void *memcheck_placement(void *args) {
  long long dram_sz = 0;
  long long nvram_sz = 0;
  float dram_usage;
  float nvram_usage;
  int n_pages;
  time_t prev_memdata_lmod = 0;

  while (!exit_sig) {
    int n_migrated = 0;
    int switch_migrated = 0;
    int thresh_migrated = 0;
    int sleep_interval = memcheck_interval;

    if (thresh_act || switch_act) {
      dram_usage = free_space_tot_per(DRAM_MODE, &dram_sz);
      nvram_usage = free_space_tot_per(NVRAM_MODE, &nvram_sz);
      printf("Current DRAM Usage: %0.2f%%\n", dram_usage * 100);
      printf("Current NVRAM Usage: %0.2f%%\n", nvram_usage * 100);
    }

    if (switch_act) {
      time_t memdata_lmod = get_memdata_mtime();
      if (memdata_lmod == 0 || (memdata_lmod == prev_memdata_lmod)) {
        printf("MEMCHECK: Old or invalid memdata values. Ignoring...\n");
      } else {
        prev_memdata_lmod = memdata_lmod;
        memdata_t *md = read_memdata();
        if (!check_memdata(md)) {
          printf("MEMCHECK: Unexpected memdata values.\n");
        } else {
          float pmm_bw;
          if (PMM_MIXED) {
            pmm_bw = md->sys_pmmAppBW;
          } else {
            pmm_bw = md->sys_pmmWrites;
          }
          if (pmm_bw > NVRAM_BW_THRESH) {
            pthread_mutex_lock(&placement_lock);
            send_find(0, NVRAM_CLEAR);
            usleep(clear_interval);
            if (dram_usage >= DRAM_TARGET) {
              switch_migrated = send_find(MAX_N_SWITCH,
              SWITCH_MODE);
              if (switch_migrated > 0) {
                printf("DRAM<->NVRAM: Switched %d out of %ld pages.\n",
                       switch_migrated, MAX_N_SWITCH * 2);
              }
            } else {
              long long n_bytes = (DRAM_LIMIT - dram_usage) * dram_sz;
              n_pages = n_bytes / page_size;
              n_pages = fmin(n_pages, MAX_N_FIND);
              switch_migrated = send_find(n_pages,
              NVRAM_INTENSIVE_MODE);

              if (switch_migrated > 0) {
                printf("NVRAM->DRAM: Sent %d out of %d intensive pages.\n",
                       switch_migrated, n_pages);
                dram_usage = free_space_tot_per(DRAM_MODE, &dram_sz);
                nvram_usage = free_space_tot_per(NVRAM_MODE, &nvram_sz);
              }
            }

            pthread_mutex_unlock(&placement_lock);
          }
        }

        n_migrated += switch_migrated;
        free(md);
      }
    }

    if (thresh_act) {
      if ((dram_usage > DRAM_LIMIT) && (nvram_usage < NVRAM_TARGET)) {
        long long n_bytes = fmin((dram_usage - DRAM_TARGET) * dram_sz,
                                 (NVRAM_TARGET - nvram_usage) * nvram_sz);
        n_pages = n_bytes / page_size;
        n_pages = fmin(n_pages, MAX_N_FIND);
        pthread_mutex_lock(&placement_lock);
        thresh_migrated = send_find(n_pages, DRAM_MODE);
        pthread_mutex_unlock(&placement_lock);
        if (thresh_migrated > 0) {
          printf("DRAM->NVRAM: Migrated %d out of %d pages.\n", thresh_migrated,
                 n_pages);
        }
      } else if (!switch_act && (nvram_usage > NVRAM_LIMIT)
          && (dram_usage < DRAM_TARGET)) {
        long long n_bytes = fmin((nvram_usage - NVRAM_TARGET) * nvram_sz,
                                 (DRAM_TARGET - dram_usage) * dram_sz);
        n_pages = n_bytes / page_size;
        n_pages = fmin(n_pages, MAX_N_FIND);
        pthread_mutex_lock(&placement_lock);
        thresh_migrated = send_find(n_pages, NVRAM_MODE);
        pthread_mutex_unlock(&placement_lock);
        if (thresh_migrated > 0) {
          printf("NVRAM->DRAM: Migrated %d out of %d pages.\n", thresh_migrated,
                 n_pages);
        }
      }

      n_migrated += thresh_migrated;
    }

    if (n_migrated > 0) {
      sleep_interval *= 2;  // give time for bw to settle given the migrated pages
      if (switch_migrated > 0) {
        sleep_interval -= clear_interval;
      }
    }

    usleep(sleep_interval);
  }

  return NULL;
}

/*void *nvramWrChk_placement(void *args) {
 time_t prev_memdata_lmod = 0;
 while (!exit_sig) {
 int sleep_interval = nvramWrChk_interval;
 if (nvramWrChk_act) {
 time_t memdata_lmod = get_memdata_mtime();
 if (memdata_lmod == 0 || (memdata_lmod == prev_memdata_lmod)) {
 printf("NVRAMWRCHK: Old or invalid memdata values. Ignoring...\n");
 }
 else {
 prev_memdata_lmod = memdata_lmod;
 memdata_t *md = read_memdata();
 if (!check_memdata(md)) {
 printf("NVRAMWRCHK: Unexpected memdata values.\n");
 }
 else {
 float pmm_bw;
 if (PMM_MIXED) {
 // If mixed configuration (AD+MM), pcm cannot isolate PMM AD write BW, so use total AD BW
 pmm_bw = md->sys_pmmAppBW;
 }
 else {
 pmm_bw = md->sys_pmmWrites;
 }

 if ((pmm_bw > NVRAM_BW_WR_THRESH)) {

 int n_avail_pages = free_space_tot_pages(DRAM_MODE);
 int n_find = fmin(n_avail_pages, MAX_N_FIND);

 pthread_mutex_lock(&placement_lock);
 send_find(0, NVRAM_CLEAR_DIRTY);
 usleep(clearDirty_interval);
 int n_migrated = send_find(n_find, NVRAM_WRITE_MODE);
 pthread_mutex_unlock(&placement_lock);

 if (n_migrated > 0) {
 sleep_interval *= 3; // give time for bw to settle given the migrated pages
 printf("NVRAM->DRAM: Sent %d out of %d write-only pages.\n", n_migrated, n_find);
 }
 sleep_interval -= clearDirty_interval;
 }
 }
 free(md);
 }
 }

 usleep(sleep_interval);
 }

 return NULL;
 }*/

/*
 -------------------------------------------------------------------------------

 STDIN/SOCKET PROCESSING

 -------------------------------------------------------------------------------
 */

void *process_stdin(void *args) {
  char *command = malloc(sizeof(char) * MAX_COMMAND_SIZE);
  char *substring;
  long pid;

  printf("Available commands:\n"
         "\tbind [pid]\n"
         "\tunbind [pid]\n"
         "\tDEBUG: send [n] [dram|nvram|dramwr]\n"
         "\tDEBUG: switch [n]\n"
         "\tDEBUG: toggle [switch|thresh|all]\n"
         "\tDEBUG: clear\n"
         "\texit\n");

  while ((fgets(command, MAX_COMMAND_SIZE, stdin) != NULL)
      && strcmp(command, "exit\n")) {
    if ((substring = strtok(command, " ")) == NULL) {
      continue;
    }

    if (!strcmp(substring, "bind")) {
      if ((substring = strtok(NULL, " ")) == NULL) {
        fprintf(stderr, "Invalid argument for bind command.\n");
        continue;
      }
      pid = strtol(substring, NULL, 10);
      if ((pid > 0) && (pid < MAX_PID_N)) {
        if (send_bind((int) pid)) {
          printf("Bind request success (pid=%d).\n", (int) pid);
        } else {
          fprintf(stderr, "Bind request failed (pid=%d).\n", (int) pid);
        }
      } else {
        fprintf(stderr, "Invalid argument for bind command.\n");
      }
    }

    else if (!strcmp(substring, "unbind")) {
      if ((substring = strtok(NULL, " ")) == NULL) {
        fprintf(stderr, "Invalid argument for unbind command.\n");
        continue;
      }
      pid = strtol(substring, NULL, 10);
      if ((pid > 0) && (pid < MAX_PID_N)) {
        if (send_unbind((int) pid)) {
          printf("Unbind request success (pid=%d).\n", (int) pid);
        } else {
          fprintf(stderr, "Unbind request failed (pid=%d).\n", (int) pid);
        }
      } else {
        fprintf(stderr, "Invalid argument for unbind command.\n");
      }
    }

    else if (!strcmp(substring, "send")) {
      if ((substring = strtok(NULL, " ")) == NULL) {
        fprintf(stderr, "Invalid argument for send command.\n");
        continue;
      }
      long n = strtol(substring, NULL, 10);
      if ((substring = strtok(NULL, " ")) == NULL) {
        fprintf(stderr, "Invalid argument for send command.\n");
        continue;
      }

      int n_migrated = 0;

      if (!strcmp(substring, "dram\n")) {
        pthread_mutex_lock(&placement_lock);
        n_migrated = send_find((int) n, NVRAM_MODE);
        pthread_mutex_unlock(&placement_lock);
      } else if (!strcmp(substring, "nvram\n")) {
        pthread_mutex_lock(&placement_lock);
        n_migrated = send_find((int) n, DRAM_MODE);
        pthread_mutex_unlock(&placement_lock);
      } else if (!strcmp(substring, "dramwr\n")) {
        pthread_mutex_lock(&placement_lock);
        n_migrated = send_find((int) n, NVRAM_WRITE_MODE);
        pthread_mutex_unlock(&placement_lock);
      }

      else {
        fprintf(stderr, "Invalid argument for send command.\n");
        continue;
      }
      if (n_migrated > 0) {
        printf("stdin: Migrated %d out of %ld pages.\n", n_migrated, n);
      }
    }

    else if (!strcmp(substring, "switch")) {
      if ((substring = strtok(NULL, " ")) == NULL) {
        fprintf(stderr, "Invalid argument for switch command.\n");
        continue;
      }
      long n = strtol(substring, NULL, 10);
      n = fmin(n, MAX_N_SWITCH);
      pthread_mutex_lock(&placement_lock);
      int n_migrated = send_find((int) n, SWITCH_MODE);
      pthread_mutex_unlock(&placement_lock);
      if (n_migrated > 0) {
        printf("NVRAM<->DRAM: Switched %d out of %ld pages.\n", n_migrated,
               n * 2);
      }
    }

    else if (!strcmp(substring, "toggle")) {
      if ((substring = strtok(NULL, " ")) == NULL) {
        fprintf(stderr, "Invalid argument for toggle command.\n");
        continue;
      }
      if (!strcmp(substring, "switch\n")) {
        switch_act = 1 - switch_act;

        if (switch_act) {
          printf("Switch component turned ON\n");
        } else {
          printf("Switch component turned OFF\n");
        }
      } else if (!strcmp(substring, "thresh\n")) {
        thresh_act = 1 - thresh_act;

        if (thresh_act) {
          printf("Threshold component turned ON\n");
        } else {
          printf("Threshold component turned OFF\n");
        }
      } else if (!strcmp(substring, "all\n")) {
        switch_act = 1 - switch_act;
        thresh_act = 1 - thresh_act;

        if (switch_act) {
          printf("Switch component turned ON\n");
        } else {
          printf("Switch component turned OFF\n");
        }

        if (thresh_act) {
          printf("Threshold component turned ON\n");
        } else {
          printf("Threshold component turned OFF\n");
        }
      }
    }

    else if (!strcmp(substring, "clr\n") || !strcmp(substring, "clear\n")) {
      system("@cls||clear");
    }

    else {
      fprintf(stderr, "Unknown command.\n"
              "Available commands:\n"
              "\tbind [pid]\n"
              "\tunbind [pid]\n"
              "\tDEBUG: send [n] [dram|nvram|dramwr]\n"
              "\tDEBUG: switch [n]\n"
              "\tDEBUG: toggle [switch|thresh|all]\n"
              "\tDEBUG: clear\n"
              "\texit\n");

    }
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

  if ((unix_fd = socket(AF_UNIX, SOCK_STREAM, 0)) == -1) {
    fprintf(stderr, "Error creating UD socket: %s\n", strerror(errno));
    return NULL;
  }
  memset(&uds_addr, 0, sizeof(uds_addr));
  uds_addr.sun_family = AF_UNIX;

  strncpy(uds_addr.sun_path, UDS_path, sizeof(uds_addr.sun_path) - 1);
  unlink(UDS_path);  // unlink to avoid error in bind

  if (bind(unix_fd, (struct sockaddr*) &uds_addr, sizeof(uds_addr)) == -1) {
    fprintf(stderr, "Error binding UDS: %s\n", strerror(errno));
    return NULL;
  }

  if (listen(unix_fd, MAX_BACKLOG) == -1) {
    fprintf(stderr, "Error marking UDS as passive: %s\n", strerror(errno));
    return NULL;
  }

  while (!exit_sig) {
    sel_timeout.tv_sec = SELECT_TIMEOUT;

    FD_ZERO(&readfds);
    FD_SET(unix_fd, &readfds);
    sel = select(unix_fd + 1, &readfds, NULL, NULL, &sel_timeout);

    if (sel == -1) {
      fprintf(stderr, "Error in UDS select: %s.\n", strerror(errno));
      return NULL;
    } else if ((sel > 0) && FD_ISSET(unix_fd, &readfds)) {
      if ((acc = accept(unix_fd, NULL, NULL)) == -1) {
        fprintf(stderr, "Failed accepting incoming UDS connection: %s\n",
                strerror(errno));
        continue;
      }
      while ((rd = read(acc, &unix_req, sizeof(req_t))) == sizeof(req_t)) {
        switch (unix_req.op_code) {
          case BIND_OP:
            if (send_bind(unix_req.pid_n)) {
              printf("Bind request success (pid=%d).\n", unix_req.pid_n);
            } else {
              fprintf(stderr, "Bind request failed (pid=%d).\n",
                      unix_req.pid_n);
            }
            break;
          case UNBIND_OP:
            if (send_unbind(unix_req.pid_n)) {
              printf("Unbind request success (pid=%d).\n", unix_req.pid_n);
            } else {
              fprintf(stderr, "Unbind request failed (pid=%d).\n",
                      unix_req.pid_n);
            }
            break;
          default:
            fprintf(
                stderr,
                "Unexpected request OPcode from accepted UD socket connection");
        }
      }

      if (rd < 0) {
        fprintf(stderr, "Error reading from accepted UDS connection: %s\n",
                strerror(errno));
        return NULL;
      }

      else if (rd == 0) {
        close(acc);
      } else {
        fprintf(
            stderr,
            "Unexpected amount of bytes read from accepted UD socket connection.\n");
      }
    }
  }
  unlink(UDS_path);
  return NULL;
}

/*
 -------------------------------------------------------------------------------

 MAIN FUNCTION

 -------------------------------------------------------------------------------
 */

int main() {

  if ((netlink_fd = socket(PF_NETLINK, SOCK_RAW, NETLINK_USER)) == -1) {
    fprintf(
        stderr,
        "Could not create netlink socket fd: %s\nTry inserting kernel module first.\n",
        strerror(errno));
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

  // create the thread pool equal to maximum number of threads!
  tm = tpool_create(max_num_threads);

  if (bind(netlink_fd, (struct sockaddr *) &src_addr, sizeof(src_addr))) {
    printf("Error binding netlink socket fd: %s\n", strerror(errno));
    free(candidates);
    free(buffer);
    free(nlmh_out);
    return 1;
  }

  if (pthread_mutex_init(&comm_lock, NULL)) {
    fprintf(stderr, "Error creating communication mutex lock: %s\n",
            strerror(errno));
  }

  else if (pthread_mutex_init(&placement_lock, NULL)) {
    fprintf(stderr, "Error creating placement mutex lock: %s\n",
            strerror(errno));
  }

  else if (pthread_create(&stdin_thread, NULL, process_stdin, NULL)) {
    fprintf(stderr, "Error spawning stdin thread: %s\n", strerror(errno));
  }

  else if (pthread_create(&socket_thread, NULL, process_socket, NULL)) {
    fprintf(stderr, "Error spawning socket thread: %s\n", strerror(errno));
  }

  else if (pthread_create(&memcheck_thread, NULL, memcheck_placement, NULL)) {
    fprintf(stderr, "Error spawning memcheck placement thread: %s\n",
            strerror(errno));
  }

  else {
    pthread_join(stdin_thread, NULL);
    printf("Exiting ctl...\n");
    pthread_join(socket_thread, NULL);
    pthread_join(memcheck_thread, NULL);

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
