#include <linux/capability.h>
#include <linux/mman.h>
#include <linux/mm.h>
#include <linux/sched/user.h>
#include <linux/swap.h>
#include <linux/swapops.h>
#include <linux/pagemap.h>
#include <linux/pagevec.h>
#include <linux/mempolicy.h>
#include <linux/syscalls.h>
#include <linux/sched.h>
#include <linux/export.h>
#include <linux/rmap.h>
#include <linux/mmzone.h>
#include <linux/hugetlb.h>
#include <linux/memcontrol.h>
#include <linux/mm_inline.h>
#include <linux/page-flags.h>

#include "internal.h"

/*
 Provides a custom syscall that provides getters and setters for reference and dirty bit flags
*/

static int do_get_referenced(unsigned long vaddr)
{
	struct vm_area_struct * vma;

	VM_BUG_ON(offset_in_page(vaddr));

	vma = find_vma(current->mm, vaddr);
	if (!vma || vma->vm_start > vaddr) {
		return -ENOMEM;
    }

	return ((vma->vm_flags & PG_referenced != 0) ? 1 : 0);
}

static int do_get_modified(unsigned long vaddr)
{
	struct vm_area_struct * vma;

	VM_BUG_ON(offset_in_page(vaddr));

	vma = find_vma(current->mm, vaddr);
	if (!vma || vma->vm_start > vaddr) {
		return -ENOMEM;
    }

	return ((vma->vm_flags & PG_modified != 0) ? 1 : 0);
}

/* vma_get_rm: returns 0 if neither bit is set, 10 if only reference bit is set,
    1 if only modified bit is set, and 11 if both bits are set */
static int do_get_rm(unsigned long vaddr)
{
	struct vm_area_struct * vma, * prev;
	int is_ref, is_mod;

	VM_BUG_ON(offset_in_page(vaddr));

	vma = find_vma(current->mm, vaddr);
	if (!vma || vma->vm_start > vaddr) {
		return -ENOMEM;
    }

    is_ref = ((vma->vm_flags & PG_referenced != 0) ? 10 : 0);
    is_mod = ((vma->vm_flags & PG_modified != 0) ? 1 : 0);
	return is_ref + is_mod;
}

/* Getters */

SYSCALL_DEFINE1(get_page_rm, vaddr)
{
	return do_get_rm(vaddr, VM_LOCKED);
}
SYSCALL_DEFINE1(get_modified, vaddr)
{
	return do_get_modified(vaddr, VM_LOCKED);
}
SYSCALL_DEFINE1(get_referenced, vaddr)
{
	return do_get_referenced(start, len, VM_LOCKED);
}

/* Setters TBD*/

/* SYSCALL_DEFINE2(set_page_rm, unsigned long, vaddr, )
{
	return do_set_rm(start, len, VM_LOCKED);
}
SYSCALL_DEFINE2(set_modified, unsigned long, vaddr, )
{
	return do_set_modified(start, len, VM_LOCKED);
}
SYSCALL_DEFINE2(set_dirty, unsigned long, vaddr, )
{
	return do_set_dirty(start, len, VM_LOCKED);
} */


/* add to syscalls.h:

asmlinkage long sys_get_page_rm(unsigned long vaddr);
asmlinkage long sys_get_page_modified(unsigned long vaddr);
asmlinkage long sys_get_page_reference(unsigned long vaddr);

*/