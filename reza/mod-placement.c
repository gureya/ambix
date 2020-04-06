/**
 * @file    placement.c
 * @author  Miguel Marques <miguel.soares.marques@tecnico.ulisboa.pt>
 * @date    12 March 2020
 * @version 0.3
 * @brief  Page walker for finding page table entries' R/M bits. Intended for the 5.5.7 Linux kernel.
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
MODULE_INFO(vermagic, "5.5.7-patchedv2 SMP mod_unload modversions ");

#define DAEMON_NAME "placement_daemon"

struct sock *nl_sock;

static addr_info_t found_addrs[MAX_N_FIND];

//static unsigned long last_addr; //FIXME: use this to fix CLOCK behaviour

static struct task_struct *task_item[MAX_PIDS];
static int n_pids;

int curr_pid;
int n_to_find;
int n_found;

static int first_pass = 1;  // First pass in NVRAM tries to find optimal DRAM
                            //  candidates in NVRAM and later suboptimal ones (only one bit set)
static int kept_pages = 0;  // Used so that NVRAM find requests do not occur when it is empty

static int find_target_process(pid_t pid) {  // to find the task struct by process_name or pid
    if(n_pids > MAX_PIDS) {
        printk(KERN_INFO "PLACEMENT: Managed PIDs at capacity.");
        return -1;
    }
    for_each_process(task_item[n_pids]) {
        if (task_item->pid == pid) {
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
        return 0;
    }

    if(pte_young() && pte_dirty()) {
        // Send to DRAM
        found_addrs[n_found].addr = addr;
        found_addrs[n_found++].pid_retval = curr_pid;
    }

    else if(!first_pass) {
        if(pte_young() || pte_dirty()) {
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
    struct vm_area_struct *mmap;
    struct mm_walk_ops mem_walk_ops = {
            .pte_entry = pte_callback_dram,
        };

    if(mode == NVRAM_MODE) { // FIXME: send also r/m = 0 or check it and return appropriately. Ask this
        first_pass = 1;
        kept_pages = 0;
        mem_walk_ops = {
            .pte_entry = pte_callback_nvram,
        };
    }

    n_to_find = n;

    for(int n_cycles=0; n_cycles < MAX_CYCLES; n_cycles++) {
        for(int i=0; i<n_pids; i++) {
            mmap = task_item[i]->mm->mmap;
            curr_pid = task_item[i]->pid;

            while (mmap != NULL) {
                walk_page_vma(mmap, &mem_walk_ops, NULL);
                mmap = mmap->vm_next;

                if(n_found >= n_to_find) {
                    return 0;
                }
            }
        }

        if(mode == NVRAM_MODE) {
            first_pass = 0;
        }
    }
    if((mode == NVRAM_MODE) && kept_pages) {
        return -2; // Signal ctl to stop sending NVRAM find requests.
    }
    return -1;  
}

static int bind_pid(pid_t pid) {
    if (!find_target_process(pid)) {
        printk(KERN_INFO "PLACEMENT: Could not bind pid=%d!\n", pid);
        return 0;
    }

    printk(KERN_INFO "PLACEMENT: Bound pid=%d!\n", pid);
    return 1;
}

static int unbind_pid(pid_t pid) {
    // Find which task to remove
    for(int i=0; i<n_pids; i++) {
        if(task_item[i]->pid == pid) {
            break;
        }
    }

    if(i == n_pids) {
        printk(KERN_INFO "PLACEMENT: Could not unbind pid=%d!\n", pid);
        return 0;
    }

    // Shift left all subsequent entries
    for(int j=i; j<n_pids; j++) {
        task_item[j] = task_item[j+1];
    }
    n_pids--;

    return 1;
}

/* Valid commands:

BIND [pid]
UNBIND [pid]
FIND [tier] [n]

*/
static void process_req(req_t req) {
    n_found = 0;
    int ret = -1;
    switch(req.op_code) {
        case FIND_OP:
            ret = do_page_walk(req.mode, req.pid_n);
            break;
        case BIND_OP:
            ret = bind_pid((pid_t) req.pid_n));;
            break;
        case UNBIND_OP:
            ret = unbind_pid((pid_t) req.pid_n));
            break;

        default:
            printk(KERN_INFO "PLACEMENT: Unrecognized opcode!\n");
    }

    found_addrs[n_found++].pid_retval(ret);
}


static void placement_nl_process_msg(struct sk_buff *skb) {

    struct nlmsghdr *nlmh;
    int sender_pid;
    struct sk_buff *skb_out;
    int msg_size;
    req_t in_msg;
    int res;

    // input
    nlmh = (struct nlmsghdr *) skb->data;
    in_msg = (req_t) nlmsg_data(nlmh);
    sender_pid = nlmh->nlmsg_pid;

    process_req(in_msg);

    msg_size = sizeof(addr_info_t) * n_found;
    skb_out = nlmsg_new(msg_size, 0);
    if (!skb_out) {
        printk(KERN_ERR "Failed to allocate new skb\n");
        return;
    }

    nlmh = nlmsg_put(skb_out, 0, 0, NLMSG_DONE, msg_size, 0);
    NETLINK_CB(skb_out).dst_group = 0; // unicast
    memcpy(nlmsg_data(nlmh), &found_addrs, msg_size);

    if ((res = nlmsg_unicast(nl_sk, skb_out, sender_pid)) < 0) {
        printk(KERN_INFO "PLACEMENT: Error in output.\n");
    }
}


static int __init _on_module_init(void) {
    printk(KERN_INFO "PLACEMENT: Hello from the module\n");

    struct netlink_kernel_cfg cfg = {
        .input = placement_nl_process_msg,
    };

    nl_sk = netlink_kernel_create(&init_net, NETLINK_USER, &cfg);
    if (!nl_sk) {
        printk(KERN_ALERT "PLACEMENT: Error creating netlink socket.\n");
        return 1;
    }


}

static void __exit _on_module_exit(void) {
    pr_info("PLACEMENT: Goodbye from module!\n");
    netlink_kernel_release(nl_sk);

}
module_init(_on_module_init);
module_exit(_on_module_exit);
