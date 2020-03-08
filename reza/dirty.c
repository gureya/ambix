/**
 * @file    dirty.c
 * @author  Miguel Marques <miguel.soares.marques@tecnico.ulisboa.pt>
 * @date    3 March 2020
 * @version 0.2
 * @brief  Page walker for finding page table entries' R/M bits. Intended for the 5.7 Linux kernel.
 * Adapted from the code provided by Reza Karimi <r68karimi@gmail.com>
 * @see https://github.com/miguelmarques1904/pnp for a full description of the module.
 */

#pragma GCC diagnostic ignored "-Wdeclaration-after-statement"

#include <linux/delay.h>
#include <linux/init.h>  // Macros used to mark up functions e.g., __init __exit
#include <linux/kernel.h>  // Contains types, macros, functions for the kernel
#include <linux/kthread.h>
#include <linux/mempolicy.h>
//#include <linux/mm.h> // Both included by pagewalk.h
//#include <linux/mm_types.h>
#include <linux/module.h>  // Core header for loading LKMs into the kernel
#include <linux/mount.h>
#include <linux/pagemap.h>
#include <linux/proc_fs.h> /* Necessary because we use the proc fs */
#include <linux/sched.h>
#include <linux/sched/mm.h>
#include <linux/sched/signal.h>
#include <linux/seq_file.h>
#include <linux/shmem_fs.h>
#include <linux/signal.h>
#include <linux/slab.h>
#include <linux/uaccess.h>

#include <asm-generic/memory_model.h>
#include <asm/pgtable.h>
#include <linux/pagewalk.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Miguel Marques");
MODULE_DESCRIPTION("Memory Access Monitor");
MODULE_VERSION("0.2");
MODULE_INFO(vermagic, "5.5.7-patchedv2 SMP mod_unload modversions ");

#define PROCFS_MAX_SIZE 2048
#define PROCFS_NAME "dirty"
#define DAEMON_NAME "dirty_daemon"
#define STAT_ARRAY_SIZE 1000000
#define P_NAME_MAX 100

static char *stat_array[STAT_ARRAY_SIZE];
static unsigned long stat_index = 0;
static unsigned long stat_count = 0;
struct task_struct *task_item;
static pid_t process_pid = -1;

static bool find_target_process(
    void) {  // to find the task struct by process_name or pid

  for_each_process(task_item) {
    if (process_pid == task_item->pid) {
      return true;
    }
  }
  return false;
}

static int pte_callback(pte_t *pte, unsigned long addr, unsigned long next,
                        struct mm_walk *walk) {
  if (!pte_present(*pte)) { // If it is not present
    return 0;
  }

  // if(pte_young(*pte)) {
  //   *pte = pte_mkold(*pte); // unset reference bit
  // }

  // if(pte_dirty(*pte)) {
  //   *pte = pte_mkclean(*pte); // unset dirty bit
  // }

  // convert pte to pfn to physical address

  char *addr = pte_val(*pte) & PTE_PFN_MASK

  stat_array[stat_index] = addr;

  if(stat_index++ > STAT_ARRAY_SIZE) {
    printk(KERN_INFO "DIRTY: max array_size reached. Resetting.\n");
    stat_index = 0;
  }

  return 0;

  /* ---------- playing with accessed bit -------- */
  // if (pte_young(*pte)) {  /
  //   stat_array[stat_array_index].count = 1;
  //   tmp_young++;
  //   *pte = pte_mkold(*pte);
  // } else {
  //   stat_array[stat_array_index].count = 0;
  //   tmp_old++;
  // }
}

static int do_page_walk(void) {
  struct vm_area_struct *mmap;
  struct mm_walk_ops mem_walk_ops = {
      .pte_entry = pte_callback,
      //.mm = task_item->mm,
  };
  mmap = task_item->mm->mmap;
  stat_index = 0;
  stat_count = 0;
  while (mmap != NULL) {
    walk_page_vma(mmap, &mem_walk_ops, NULL);
    mmap = mmap->vm_next;
  }
  stat_count = stat_index;
  return 0;
}

static struct task_struct *dirty_daemon_thread;
static int dirty_daemon(void *unused) {
  if (find_target_process()) {
    printk(KERN_INFO "DIRTY: the process found successfully!\n");
    do_page_walk();
    printk(KERN_INFO
           "DIRTY: page table scan done! %ld dirty pages found for pid=%d.\n",
           stat_count, process_pid);
    printk(KERN_INFO "DIRTY: run \"cat /proc/dirty\" to see dirty pages.\n");
  } else {
    printk(KERN_INFO
           "DIRTY: specify a valid pid, run \"echo [pid] > /proc/dirty\"\n");
  }

  return 0;
}

static int my_proc_list_show(struct seq_file *m, void *v) {
  unsigned long int i;
  for (i = 0; i < stat_count; i++) {
    seq_printf(m, "%s\n", (unsigned long)stat_array[i]);
  }
  return 0;
}

static ssize_t my_proc_write(struct file *file, const char __user *buffer,
                             size_t count, loff_t *f_pos) {
  char *tmp = kzalloc((count + 1), GFP_KERNEL);
  if (!tmp) return -ENOMEM;
  if (copy_from_user(tmp, buffer, count)) {
    kfree(tmp);
    return EFAULT;
  }

  long pid;
  if (kstrtol(tmp, 10, &pid) == 0) {
    process_pid = pid;
    dirty_daemon_thread = kthread_run(dirty_daemon, NULL, DAEMON_NAME);
  }
  return count;
}

static int my_proc_open(struct inode *inode, struct file *file) {
  return single_open(file, my_proc_list_show, NULL);
}

static struct file_operations my_fops = {.owner = THIS_MODULE,
                                         .open = my_proc_open,
                                         .release = single_release,
                                         .read = seq_read,
                                         .llseek = seq_lseek,
                                         .write = my_proc_write};

static int __init _on_module_init(void) {
  printk(KERN_INFO
         "DIRTY: Hello from the module, let's find those dirty pages!\n");

  // creating the /proc file
  struct proc_dir_entry *entry;
  entry = proc_create(PROCFS_NAME, 0777, NULL, &my_fops);
  if (!entry) {
    return -1;
  } else {
    printk(KERN_INFO "DIRTY: create proc file successfully\n");
  }
  return 0;
}

static void __exit _on_module_exit(void) {
  remove_proc_entry(PROCFS_NAME, NULL);
  pr_info("DIRTY: /proc/%s removed\n", PROCFS_NAME);
  pr_info("DIRTY: Goodbye from the module!\n");
}
module_init(_on_module_init);
module_exit(_on_module_exit);
