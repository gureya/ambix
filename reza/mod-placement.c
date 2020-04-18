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

#include "placement.h"

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Miguel Marques");
MODULE_DESCRIPTION("Memory Access Monitor");
MODULE_VERSION("0.3");
MODULE_INFO(vermagic, "5.6.3-patched SMP mod_unload modversions ");

#define DAEMON_NAME "placement_daemon"

struct sock *nl_sock;

addr_info_t *found_addrs;

struct task_struct **task_item;
int n_pids = 0;

unsigned long last_addr = 0;
int last_pid = 0;

int curr_pid = 0;
int n_to_find = 0;
int n_found = 0;

int first_pass = 1; // First pass in NVRAM tries to find optimal DRAM
                      //  candidates in NVRAM and later suboptimal ones (only one bit set)
int kept_pages = 0; // Used so that NVRAM find requests do not occur when it is empty

static int find_target_process(pid_t pid) {  // to find the task struct by process_name or pid
    if(n_pids > MAX_PIDS) {
        printk(KERN_INFO "PLACEMENT: Managed PIDs at capacity.");
        return -1;
    }
    for_each_process(task_item[n_pids]) {
        if (task_item[n_pids]->pid == pid) {
            n_pids++;
            return 0;
        }
    }
    return -1;
}

static int pte_callback_dram(pte_t *ptep, unsigned long addr, unsigned long next,
                        struct mm_walk *walk) {

    pte_t pte = *ptep;

    // If already found n pages, page is not present or page is not in DRAM node
    if((n_found >= n_to_find) || !pte_present(pte) || (pfn_to_nid(pte_pfn(pte)) != DRAM_NODE)) {
        if(n_found == n_to_find) { // found all + last
            last_addr = addr;
            n_found++;
        }
        return 0;
    }

    // if(!pte_young(pte) && !pte_dirty(pte)) {

    //     // TODO: Swap out
    //     found_addrs[n_found].addr = addr;
    //     found_addrs[n_found++].pid_retval = curr_pid;
    // }

    else if(!pte_young(pte) || !pte_dirty(pte)) {

        // Send to NVRAM
        found_addrs[n_found].addr = addr;
        found_addrs[n_found++].pid_retval = curr_pid;
    }

    else if(pte_young(pte)) {

        // Unset reference bit and move on
        pte_t old_pte = ptep_modify_prot_start(walk->vma, addr, ptep);
        pte = pte_mkold(old_pte); // unset modified bit
        ptep_modify_prot_commit(walk->vma, addr, ptep, old_pte, pte);
    }

    return 0;
}

static int pte_callback_nvram(pte_t *ptep, unsigned long addr, unsigned long next,
                        struct mm_walk *walk) {

    pte_t pte = *ptep;

    // If already found n pages, page is not present or page is not in NVRAM node
    if((n_found >= n_to_find) || !pte_present(pte) || (pfn_to_nid(pte_pfn(pte)) != NVRAM_NODE)) {
        if(n_found == n_to_find) { // found all + last
            last_addr = addr;
            n_found++;
        }
        return 0;
    }

    if(pte_young(pte) && pte_dirty(pte)) {
        // Send to DRAM
        found_addrs[n_found].addr = addr;
        found_addrs[n_found++].pid_retval = curr_pid;
    }

    else if(!first_pass) {
        if(pte_young(pte) || pte_dirty(pte)) {
            // Send to DRAM
            found_addrs[n_found].addr = addr;
            found_addrs[n_found++].pid_retval = curr_pid;
        }
        else {
            kept_pages = 1;
        }
    }

    return 0;
}

static int do_page_walk(int mode, int n) {
    struct mm_struct *mm;
    struct mm_walk_ops mem_walk_ops = {
            .pte_entry = pte_callback_dram,
        };

    if(mode == NVRAM_MODE) {
        first_pass = 1;
        kept_pages = 0;
        mem_walk_ops.pte_entry = pte_callback_nvram;
    }

    n_to_find = n;
    int n_cycles;
    for(n_cycles = 0; n_cycles < MAX_CYCLES; n_cycles++) {
        int i;
        
        // begin cycle at last_pid->last_addr
        mm = task_item[last_pid]->mm;
        curr_pid = task_item[last_pid]->pid;
        walk_page_range(mm, last_addr, MAX_ADDRESS, &mem_walk_ops, NULL);
        if(n_found >= n_to_find) {
            return 0;
        }

        for(i = last_pid+1; i < n_pids; i++) {

            mm = task_item[i]->mm;
            curr_pid = task_item[i]->pid;
            walk_page_range(mm, 0, MAX_ADDRESS, &mem_walk_ops, NULL);
            if(n_found >= n_to_find) {
                return 0;
            }
        }

        for(i = 0; i < last_pid-1; i++) {
            mm = task_item[i]->mm;
            curr_pid = task_item[i]->pid;
            walk_page_range(mm, 0, MAX_ADDRESS, &mem_walk_ops, NULL);
            if(n_found >= n_to_find) {
                return 0;
            }
        }

        // finish cycle at last_pid->last_addr
        mm = task_item[last_pid]->mm;
        curr_pid = task_item[last_pid]->pid;
        walk_page_range(mm, 0, last_addr+1, &mem_walk_ops, NULL);
        if(n_found >= n_to_find) {
            return 0;
        }

        if(mode == NVRAM_MODE) {
            first_pass = 0;
        }
    }
    if((mode == NVRAM_MODE) && !kept_pages) {
        return -2; // Signal ctl to stop sending NVRAM find requests.
    }
    return -1;  
}

static int bind_pid(pid_t pid) {
    if((pid <= 0) || (pid > MAX_PID_N)) {
        printk(KERN_INFO "PLACEMENT: Invalid pid value in bind command.\n");
        return -1;
    }
    if (!find_target_process(pid)) {
        printk(KERN_INFO "PLACEMENT: Could not bind pid=%d!\n", pid);
        return -1;
    }

    printk(KERN_INFO "PLACEMENT: Bound pid=%d!\n", pid);
    return 0;
}

static int unbind_pid(pid_t pid) {
    if((pid <= 0) || (pid > MAX_PID_N)) {
        printk(KERN_INFO "PLACEMENT: Invalid pid value in unbind command.\n");
        return -1;
    }

    // Find which task to remove
    int i;
    for(i = 0; i < n_pids; i++) {
        if(task_item[i]->pid == pid) {
            break;
        }
    }

    if(i == n_pids) {
        printk(KERN_INFO "PLACEMENT: Could not unbind pid=%d!\n", pid);
        return -1;
    }

    // Shift left all subsequent entries
    int j;
    for(j = i; j < n_pids; j++) {
        task_item[j] = task_item[j+1];
    }
    n_pids--;

    return 0;
}

/* Valid commands:

BIND [pid]
UNBIND [pid]
FIND [tier] [n]

*/
static void process_req(req_t *req) {
    int ret = -1;

    if(req != NULL) {
        n_found = 0;
        switch(req->op_code) {
            case FIND_OP:
                ret = do_page_walk(req->mode, req->pid_n);
                break;
            case BIND_OP:
                ret = bind_pid(req->pid_n);
                break;
            case UNBIND_OP:
                ret = unbind_pid(req->pid_n);
                break;

            default:
                printk(KERN_INFO "PLACEMENT: Unrecognized opcode!\n");
        }
    }

    found_addrs[n_found++].pid_retval = ret;
}


static void placement_nl_process_msg(struct sk_buff *skb) {

    struct nlmsghdr *nlmh;
    int sender_pid;
    struct sk_buff *skb_out;
    int msg_size;
    req_t *in_req;
    int res;

    // input
    nlmh = (struct nlmsghdr *) skb->data;

    in_req = (req_t *) NLMSG_DATA(nlmh);
    sender_pid = nlmh->nlmsg_pid;

    process_req(in_req);

    msg_size = sizeof(addr_info_t) * n_found;
    skb_out = nlmsg_new(msg_size, 0);
    if (!skb_out) {
        printk(KERN_ERR "Failed to allocate new skb.\n");
        return;
    }

    nlmh = nlmsg_put(skb_out, 0, 0, NLMSG_DONE, msg_size, 0);
    NETLINK_CB(skb_out).dst_group = 0; // unicast
    memcpy(NLMSG_DATA(nlmh), found_addrs, msg_size);

    if ((res = nlmsg_unicast(nl_sock, skb_out, sender_pid)) < 0) {
        printk(KERN_INFO "PLACEMENT: Error sending bak to user.\n");
    }

}


static int __init _on_module_init(void) {
    printk(KERN_INFO "PLACEMENT: Hello from module!\n");

    last_addr = 0;
    last_pid = 0;
    task_item = kmalloc(MAX_PIDS * sizeof(struct task_struct *), GFP_KERNEL);
    found_addrs = kmalloc(MAX_N_FIND * sizeof(addr_info_t), GFP_KERNEL);

    struct netlink_kernel_cfg cfg = {
        .input = placement_nl_process_msg,
    };

    nl_sock = netlink_kernel_create(&init_net, NETLINK_USER, &cfg);
    if (!nl_sock) {
        printk(KERN_ALERT "PLACEMENT: Error creating netlink socket.\n");
        return 1;
    }

    return 0;
}

static void __exit _on_module_exit(void) {
    pr_info("PLACEMENT: Goodbye from module!\n");
    netlink_kernel_release(nl_sock);

    kfree(task_item);
    kfree(found_addrs);
}

module_init(_on_module_init);
module_exit(_on_module_exit);