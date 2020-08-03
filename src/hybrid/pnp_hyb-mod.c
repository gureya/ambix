/**
 * @file    placement.c
 * @author  Miguel Marques <miguel.soares.marques@tecnico.ulisboa.pt>
 * @date    12 March 2020
 * @version 0.3
 * @brief  Page walker for finding page table entries' R/M bits. Intended for the 5.6.3 Linux kernel.
 * Adapted from the code provided by Reza Karimi <r68karimi@gmail.com>
 * @see https://github.com/miguelmarques1904/pnp for a full description of the module.
 */

#pragma GCC diagnostic ignored "-Wdeclaration-after-statement"

#include <linux/delay.h>
#include <linux/init.h>  // Macros used to mark up functions e.g., __init __exit
#include <linux/kernel.h>  // Contains types, macros, functions for the kernel
#include <linux/kthread.h>
#include <linux/mempolicy.h>
#include <linux/module.h>  // Core header for loading LKMs into the kernel
#include <net/sock.h>
#include <linux/netlink.h>
#include <linux/skbuff.h>
#include <linux/mount.h>
#include <linux/sched.h>
#include <linux/sched/mm.h>
#include <linux/sched/signal.h>
#include <linux/seq_file.h>
#include <linux/shmem_fs.h>
#include <linux/signal.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

#include <linux/pagewalk.h>
#include <linux/mmzone.h> // Contains conversion between pfn and node id (NUMA node)

#include <linux/string.h>
#include "pnp.h"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Miguel Marques");
MODULE_DESCRIPTION("Bandwidth-aware page replacement");
MODULE_VERSION("0.3");
MODULE_INFO(vermagic, "5.6.14-patched SMP mod_unload modversions ");

struct sock *nl_sock;

addr_info_t *found_addrs;
addr_info_t *backup_addrs; // prevents a second page walk

struct task_struct **task_items;
struct nlmsghdr **nlmh_array;
int n_pids = 0;

volatile unsigned long last_addr_dram = 0;
volatile unsigned long last_addr_nvram = 0;
volatile int found_last = 0;

int last_pid_dram = 0;
int last_pid_nvram = 0;

int curr_pid = 0;
int n_to_find = 0;
int n_found = 0;
int n_backup = 0;



/*
-------------------------------------------------------------------------------

HELPER FUNCTIONS

-------------------------------------------------------------------------------
*/



static int find_target_process(pid_t pid) {  // to find the task struct by process_name or pid
    if (n_pids > MAX_PIDS) {
        pr_info("PLACEMENT: Managed PIDs at capacity.\n");
        return 0;
    }
    int i;
    for (i=0; i < n_pids; i++) {
        if (task_items[i]->pid == pid) {
            pr_info("PLACEMENT: Already managing given PID.\n");
            return 0;
        }
    }
    for_each_process(task_items[n_pids]) {
        if (task_items[n_pids]->pid == pid) {
            n_pids++;
            return 1;
        }
    }

    return 0;
}

static int update_pid_list(int i) {
    if (last_pid_nvram > i) {
        last_pid_nvram--;
    }
    else if (last_pid_nvram == i) {
        last_addr_nvram = 0;

        if (last_pid_nvram == (n_pids-1)) {
            last_pid_nvram = 0;
        }
    }

    // Shift left all subsequent entries
    int j;
    for (j = i; j < (n_pids - 1); j++) {
        task_items[j] = task_items[j+1];
    }

    n_pids--;

    return 0;
}

static int refresh_pids(void) {
    int i;

    for (i=0; i < n_pids; i++) {
        int pid = task_items[i]->pid;
        int found = 0;

        for_each_process(task_items[i]) {
            if (task_items[i]->pid == pid) {
                found = 1;
                break;
            }
        }

        if (!found) {
            update_pid_list(i);
        }

    }

    return 0;
}



/*
-------------------------------------------------------------------------------

CALLBACK FUNCTIONS

-------------------------------------------------------------------------------
*/



static int pte_callback_mem(pte_t *ptep, unsigned long addr, unsigned long next,
                        struct mm_walk *walk) {

    // If already found n pages, page is not present or page is not in DRAM node
    if ((ptep == NULL) || (n_found >= n_to_find) || !pte_present(*ptep) || !pte_write(*ptep) || !contains(pfn_to_nid(pte_pfn(*ptep)), DRAM_MODE)) {
        if ((n_found == n_to_find) && !found_last) { // found all + last
            last_addr_dram = addr;
            found_last = 1;
        }
        return 0;
    }

    // if (!pte_young(pte) && !pte_dirty(pte)) {

    //     // TODO: Maybe Swap out if mlocked
    //     found_addrs[n_found].addr = addr;
    //     found_addrs[n_found++].pid_retval = curr_pid;
    // }
    if (!pte_dirty(*ptep)) {

        // Send to NVRAM
        found_addrs[n_found].addr = addr;
        found_addrs[n_found++].pid_retval = curr_pid;
    }
    else {
        // Add to backup list
        if (!pte_young(*ptep) && (n_backup < n_to_find)) {
            backup_addrs[n_backup].addr = addr;
            backup_addrs[n_backup++].pid_retval = curr_pid;
        }

        pte_t old_pte = ptep_modify_prot_start(walk->vma, addr, ptep);
        *ptep = pte_mkold(old_pte); // unset modified bit
        *ptep = pte_mkclean(old_pte); // unset dirty bit
        ptep_modify_prot_commit(walk->vma, addr, ptep, old_pte, *ptep);

    }

    return 0;
}

static int pte_callback_nvram(pte_t *ptep, unsigned long addr, unsigned long next,
                        struct mm_walk *walk) {

    // If already found n pages, page is not present or page is not in NVRAM node
    if ((ptep == NULL) || (n_found >= n_to_find) || !pte_present(*ptep) || !pte_write(*ptep) || !contains(pfn_to_nid(pte_pfn(*ptep)), NVRAM_MODE)) {
        if ((n_found == n_to_find) && !found_last) { // found all + last
            last_addr_nvram = addr;
            found_last = 1;
        }
        return 0;
    }

    if (pte_dirty(*ptep)) {
        if (pte_young(*ptep)) {
            // Send to DRAM (priority)
            found_addrs[n_found].addr = addr;
            found_addrs[n_found++].pid_retval = curr_pid;
        }
        else {
            // Add to backup list
            if (n_backup < (n_to_find - n_found)) {
                backup_addrs[n_backup].addr = addr;
                backup_addrs[n_backup++].pid_retval = curr_pid;
            }
        }
    }

    return 0;
}

static int pte_callback_bal(pte_t *ptep, unsigned long addr, unsigned long next,
                        struct mm_walk *walk) {

    if ((ptep == NULL) || !pte_present(*ptep)) {
        return 0;
    }
    // TODO: test if mutex is needed
    if (pte_young(*ptep)) {
        if(!contains(pfn_to_nid(pte_pfn(*ptep)), DRAM_MODE)) {
            found_addrs[0].addr++;
        }
        else if(!contains(pfn_to_nid(pte_pfn(*ptep)), NVRAM_MODE)) {
            found_addrs[1].addr++;
    }

    return 0;
}



/*
-------------------------------------------------------------------------------

PAGE WALKERS

-------------------------------------------------------------------------------
*/



static int do_page_walk(struct mm_walk_ops mem_walk_ops, int last_pid, unsigned long last_addr) {
    struct mm_struct *mm;
    int i;
    //pr_info("PLACEMENT: lastaddr - %p\n", (void *) last_addr);
    // begin at last_pid->last_addr
    mm = task_items[last_pid]->mm;
    spin_lock(&mm->page_table_lock);
    curr_pid = task_items[last_pid]->pid;
    walk_page_range(mm, last_addr, MAX_ADDRESS, &mem_walk_ops, NULL);
    spin_unlock(&mm->page_table_lock);
    if (n_found >= n_to_find) {
        goto out;
    }

    for (i=last_pid+1; i<n_pids; i++) {

        mm = task_items[i]->mm;
        spin_lock(&mm->page_table_lock);
        curr_pid = task_items[i]->pid;
        walk_page_range(mm, 0, MAX_ADDRESS, &mem_walk_ops, NULL);
        spin_unlock(&mm->page_table_lock);
        if (n_found >= n_to_find) {
            return i;
        }
    }

    for (i = 0; i < last_pid-1; i++) {
        mm = task_items[i]->mm;
        spin_lock(&mm->page_table_lock);
        curr_pid = task_items[i]->pid;
        walk_page_range(mm, 0, MAX_ADDRESS, &mem_walk_ops, NULL);
        spin_unlock(&mm->page_table_lock);
        if (n_found >= n_to_find) {
            return i;
        }
    }

    // finish cycle at last_pid->last_addr
    mm = task_items[last_pid]->mm;
    spin_lock(&mm->page_table_lock);
    curr_pid = task_items[last_pid]->pid;
    walk_page_range(mm, 0, last_addr+1, &mem_walk_ops, NULL);
    spin_unlock(&mm->page_table_lock);

out:
    return last_pid;
}

static int dram_walk(int n) {
    struct mm_walk_ops mem_walk_ops = {.pte_entry = pte_callback_mem};

    n_to_find = n;
    n_backup = 0;
    found_last = 0;

    last_pid_dram = do_page_walk(mem_walk_ops, last_pid_dram, last_addr_dram);

    if (n_found >= n_to_find) {
        return 0;
    }
    else if (n_backup > 0) {
        int remaining = n_to_find - n_found;
        int i;

        for (i=0; (i < remaining) && (i < n_backup); i++) {
            found_addrs[n_found].addr = backup_addrs[i].addr;
            found_addrs[n_found++].pid_retval = backup_addrs[i].pid_retval;
        }

        if (n_found >= n_to_find) {
            return 0;
        }
    }
    return -1;
}

static int nvram_walk(int n) {
    struct mm_walk_ops mem_walk_ops = {.pte_entry = pte_callback_nvram};

    n_to_find = n;
    n_backup = 0;
    found_last = 0;

    last_pid_nvram = do_page_walk(mem_walk_ops, last_pid_nvram, last_addr_nvram);

    if (n_found >= n_to_find) {
        return 0;
    }
    else if (n_backup > 0) {
        int remaining = n_to_find - n_found;
        int i;

        for (i=0; (i < remaining) && (i < n_backup); i++) {
            found_addrs[n_found].addr = backup_addrs[i].addr;
            found_addrs[n_found++].pid_retval = backup_addrs[i].pid_retval;
        }

        if (n_found >= n_to_find) {
            return 0;
        }
    }
    return -1;
}

static int balance_walk(int n, int *mode) {
    struct mm_walk_ops mem_walk_ops = {.pte_entry = pte_callback_bal};

    found_addrs[0].addr = 0;
    found_addrs[1].addr = 0;

    int i;
    for (i=0; i < n_pids; i++) {

        mm = task_items[i]->mm;
        spin_lock(&mm->page_table_lock);
        curr_pid = task_items[i]->pid;
        walk_page_range(mm, 0, MAX_ADDRESS, &mem_walk_ops, NULL);
        spin_unlock(&mm->page_table_lock);
    }

    int dram_found = found_addrs[0].addr;
    int nvram_found = found_addrs[1].addr;

    if(dram_found > nvram_found) {
        *mode = DRAM_MODE;
        return dram_found - nvram_found;
    else {
        *mode = NVRAM_MODE;
        return nvram_found - dram_found;

}

static int switch_walk(int n) {
    struct mm_walk_ops mem_walk_ops = {.pte_entry = pte_callback_nvram};

    n_to_find = n;
    n_backup = 0;
    found_last = 0;

    last_pid_nvram = do_page_walk(mem_walk_ops, last_pid_nvram, last_addr_nvram);

    int nvram_found = n_found; // store the index of last nvram addr found
    if (nvram_found == 0) {
        return -1;
    }

    found_addrs[n_found].pid_retval = 0; // fill separator after

    n_to_find = n_found*2 + 1; // try to find the same amount of dram addrs
    n_found++;
    found_last = 0;
    n_backup = 0;

    mem_walk_ops.pte_entry = pte_callback_mem;
    last_pid_dram = do_page_walk(mem_walk_ops, last_pid_dram, last_addr_dram);
    int dram_found = n_found - nvram_found - 1;
    // found equal number of dram and nvram entries
    if (dram_found == nvram_found) {
        return 0;
    }
    else if ((dram_found < nvram_found) && (n_backup > 0)) {
        int remaining = nvram_found - dram_found;
        int i;

        for (i=0; (i < remaining) && (i < n_backup); i++) {
            found_addrs[n_found].addr = backup_addrs[i].addr;
            found_addrs[n_found++].pid_retval = backup_addrs[i].pid_retval;
        }

        if (dram_found == nvram_found) {
            return 0;
        }
    }
    // if it did not find an equal number then reconstruct array
    found_addrs[dram_found].pid_retval = 0; // fill separator in new space
    n_found = dram_found * 2 + 1; // discard last entries
    int i;
    for (i=dram_found+1; i<n_found; i++) {
        found_addrs[i].addr = found_addrs[nvram_found+1+i].addr;
        found_addrs[i].pid_retval = found_addrs[nvram_found+1+i].pid_retval;
    }
    return -1;
}



/*
-------------------------------------------------------------------------------

BIND/UNBIND FUNCTIONS

-------------------------------------------------------------------------------
*/



static int bind_pid(pid_t pid) {
    if ((pid <= 0) || (pid > MAX_PID_N)) {
        pr_info("PLACEMENT: Invalid pid value in bind command.\n");
        return -1;
    }
    if (!find_target_process(pid)) {
        pr_info("PLACEMENT: Could not bind pid=%d.\n", pid);
        return -1;
    }

    pr_info("PLACEMENT: Bound pid=%d.\n", pid);
    return 0;
}

static int unbind_pid(pid_t pid) {
    if ((pid <= 0) || (pid > MAX_PID_N)) {
        pr_info("PLACEMENT: Invalid pid value in unbind command.\n");
        return -1;
    }

    // Find which task to remove
    int i;
    for (i = 0; i < n_pids; i++) {
        if (task_items[i]->pid == pid) {
            break;
        }
    }

    if (i == n_pids) {
        pr_info("PLACEMENT: Could not unbind pid=%d.\n", pid);
        return -1;
    }

    update_pid_list(i);
    pr_info("PLACEMENT: Unbound pid=%d.\n", pid);
    return 0;
}



/*
-------------------------------------------------------------------------------

MESSAGE/REQUEST PROCESSING

-------------------------------------------------------------------------------
*/



/* Valid request commands:

BIND [pid]
UNBIND [pid]
FIND [tier] [n]

*/
static void process_req(req_t *req) {
    int ret = -1;
    n_found = 0;
    if (req != NULL) {
        switch (req->op_code) {
            case FIND_OP:
                refresh_pids();
                if (n_pids>0) {
                    switch (req->mode) {
                        case DRAM_MODE:
                            ret = dram_walk(req->pid_n);
                            break;
                        case NVRAM_MODE:
                            ret = nvram_walk(req->pid_n);
                            break;
                        case SWITCH_MODE:
                            ret = switch_walk(req->pid_n);
                            break;
                        case BALANCE_MODE:
                            int mode;
                            int n = balance_walk(req->pid_n, &mode);

                            if (mode == DRAM_MODE) {
                                ret = dram_walk(n);
                            else {
                                ret = nvram_walk(n);
                            found_addrs[n_found++].pid_retval = -mode;
                            break;
                        default:
                            pr_info("PLACEMENT: Unrecognized mode.\n");
                    }
                }
                break;
            case BIND_OP:
                ret = bind_pid(req->pid_n);
                break;
            case UNBIND_OP:
                ret = unbind_pid(req->pid_n);
                break;

            default:
                pr_info("PLACEMENT: Unrecognized opcode.\n");
        }
    }

    found_addrs[n_found++].pid_retval = ret;
}


static void placement_nl_process_msg(struct sk_buff *skb) {
    struct nlmsghdr *nlmh;
    int sender_pid;
    struct sk_buff *skb_out;
    req_t *in_req;
    int res;

    printk("PLACEMENT: Received message.\n");

    // input
    nlmh = (struct nlmsghdr *) skb->data;

    in_req = (req_t *) NLMSG_DATA(nlmh);
    sender_pid = nlmh->nlmsg_pid;

    process_req(in_req);

    int required_packets = (n_found / MAX_N_PER_PACKET) + ((n_found % MAX_N_PER_PACKET) != 0);
    skb_out = nlmsg_new(NLMSG_LENGTH(MAX_PAYLOAD) * required_packets, GFP_KERNEL);
    if (!skb_out) {
        pr_err("Failed to allocate new skb.\n");
        return;
    }

    int i;

    for (i=0; i < required_packets-1; i++) { // process all but last packet
        nlmh_array[i] = nlmsg_put(skb_out, 0, 0, 0, MAX_N_PER_PACKET * sizeof(addr_info_t), NLM_F_MULTI);
        memset(NLMSG_DATA(nlmh_array[i]), 0, MAX_PAYLOAD);
        memcpy(NLMSG_DATA(nlmh_array[i]), found_addrs + i*MAX_N_PER_PACKET, MAX_PAYLOAD);
    }
    int rem_size = (n_found % MAX_N_PER_PACKET) * sizeof(addr_info_t);

    nlmh_array[i] = nlmsg_put(skb_out, 0, 0, NLMSG_DONE, rem_size, 0);
    memcpy(NLMSG_DATA(nlmh_array[i]), found_addrs + i*MAX_N_PER_PACKET, rem_size);

    NETLINK_CB(skb_out).dst_group = 0; // unicast

    if (n_found == 1) {
        pr_info("PLACEMENT: Sending %d entry to ctl.\n", n_found);
    }
    else {
        pr_info("PLACEMENT: Sending %d entries to ctl in %d packets.\n", n_found, required_packets);
    }
    if ((res = nlmsg_unicast(nl_sock, skb_out, sender_pid)) < 0) {
            pr_info("PLACEMENT: Error sending response to ctl.\n");
    }
}



/*
-------------------------------------------------------------------------------

MODULE INIT/EXIT

-------------------------------------------------------------------------------
*/



static int __init _on_module_init(void) {
    pr_info("PLACEMENT-HYB: Hello from module!\n");

    task_items = kmalloc(sizeof(struct task_struct *) * MAX_PIDS, GFP_KERNEL);
    found_addrs = kmalloc(sizeof(addr_info_t) * MAX_N_FIND, GFP_KERNEL);
    backup_addrs = kmalloc(sizeof(addr_info_t) * MAX_N_FIND, GFP_KERNEL);
    nlmh_array = kmalloc(sizeof(struct nlmsghdr *) * MAX_PACKETS, GFP_KERNEL);

    struct netlink_kernel_cfg cfg = {
        .input = placement_nl_process_msg,
    };

    nl_sock = netlink_kernel_create(&init_net, NETLINK_USER, &cfg);
    if (!nl_sock) {
        pr_alert("PLACEMENT: Error creating netlink socket.\n");
        return 1;
    }

    return 0;
}

static void __exit _on_module_exit(void) {
    pr_info("PLACEMENT-HYB: Goodbye from module!\n");
    netlink_kernel_release(nl_sock);

    kfree(task_items);
    kfree(found_addrs);
    kfree(backup_addrs);
    kfree(nlmh_array);
}

module_init(_on_module_init);
module_exit(_on_module_exit);
