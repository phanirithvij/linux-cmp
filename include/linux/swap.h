#ifndef _LINUX_SWAP_H
#define _LINUX_SWAP_H

#include <linux/config.h>
#include <linux/spinlock.h>
#include <linux/linkage.h>
#include <linux/mmzone.h>
#include <linux/list.h>
#include <linux/sched.h>

#include <asm/atomic.h>
#include <asm/page.h>

#define SWAP_FLAG_PREFER	0x8000	/* set if swap priority specified */
#define SWAP_FLAG_PRIO_MASK	0x7fff
#define SWAP_FLAG_PRIO_SHIFT	0

static inline int current_is_kswapd(void)
{
	return current->flags & PF_KSWAPD;
}

/*
 * MAX_SWAPFILES defines the maximum number of swaptypes: things which can
 * be swapped to.  The swap type and the offset into that swap type are
 * encoded into pte's and into pgoff_t's in the swapcache.  Using five bits
 * for the type means that the maximum number of swapcache pages is 27 bits
 * on 32-bit-pgoff_t architectures.  And that assumes that the architecture packs
 * the type/offset into the pte as 5/27 as well.
 */
#define MAX_SWAPFILES_SHIFT	5
#define MAX_SWAPFILES		(1 << MAX_SWAPFILES_SHIFT)

/*
 * Magic header for a swap area. The first part of the union is
 * what the swap magic looks like for the old (limited to 128MB)
 * swap area format, the second part of the union adds - in the
 * old reserved area - some extra information. Note that the first
 * kilobyte is reserved for boot loader or disk label stuff...
 *
 * Having the magic at the end of the PAGE_SIZE makes detecting swap
 * areas somewhat tricky on machines that support multiple page sizes.
 * For 2.5 we'll probably want to move the magic to just beyond the
 * bootbits...
 */
union swap_header {
	struct {
		char reserved[PAGE_SIZE - 10];
		char magic[10];			/* SWAP-SPACE or SWAPSPACE2 */
	} magic;
	struct {
		char	     bootbits[1024];	/* Space for disklabel etc. */
		unsigned int version;
		unsigned int last_page;
		unsigned int nr_badpages;
		unsigned int padding[125];
		unsigned int badpages[1];
	} info;
};

 /* A swap entry has to fit into a "unsigned long", as
  * the entry is hidden in the "index" field of the
  * swapper address space.
  */
typedef struct {
	unsigned long val;
} swp_entry_t;

/*
 * current->reclaim_state points to one of these when a task is running
 * memory reclaim
 */
struct reclaim_state {
	unsigned long reclaimed_slab;
};

#ifdef __KERNEL__

struct address_space;
struct sysinfo;
struct writeback_control;
struct zone;

/*
 * A swap extent maps a range of a swapfile's PAGE_SIZE pages onto a range of
 * disk blocks.  A list of swap extents maps the entire swapfile.  (Where the
 * term `swapfile' refers to either a blockdevice or an IS_REG file.  Apart
 * from setup, they're handled identically.
 *
 * We always assume that blocks are of size PAGE_SIZE.
 */
struct swap_extent {
	struct list_head list;
	pgoff_t start_page;
	pgoff_t nr_pages;
	sector_t start_block;
};

/*
 * Max bad pages in the new format..
 */
#define __swapoffset(x) ((unsigned long)&((union swap_header *)0)->x)
#define MAX_SWAP_BADPAGES \
	((__swapoffset(magic.magic) - __swapoffset(info.badpages)) / sizeof(int))

enum {
	SWP_USED	= (1 << 0),	/* is slot in swap_info[] used? */
	SWP_WRITEOK	= (1 << 1),	/* ok to write to this swap?	*/
	SWP_ACTIVE	= (SWP_USED | SWP_WRITEOK),
					/* add others here before... */
	SWP_SCANNING	= (1 << 8),	/* refcount in scan_swap_map */
};

#define SWAP_CLUSTER_MAX 32

#define SWAP_MAP_MAX	0x7fff
#define SWAP_MAP_BAD	0x8000

/*
 * The in-memory structure used to track swap areas.
 */
struct swap_info_struct {
	unsigned int flags;
	int prio;			/* swap priority */
	struct file *swap_file;
	struct block_device *bdev;
	struct list_head extent_list;
	struct swap_extent *curr_swap_extent;
	unsigned old_block_size;
	unsigned short * swap_map;
	unsigned int lowest_bit;
	unsigned int highest_bit;
	unsigned int cluster_next;
	unsigned int cluster_nr;
	unsigned int pages;
	unsigned int max;
	unsigned int inuse_pages;
	int next;			/* next entry on swap list */
};

struct swap_list_t {
	int head;	/* head of priority-ordered swapfile list */
	int next;	/* swapfile to be used next */
};

/* Swap 50% full? Release swapcache more aggressively.. */
#define vm_swap_full() (nr_swap_pages*2 < total_swap_pages)

/* linux/mm/oom_kill.c */
extern void out_of_memory(gfp_t gfp_mask, int order);

/* linux/mm/memory.c */
extern void swapin_readahead(swp_entry_t, unsigned long, struct vm_area_struct *);

/* linux/mm/page_alloc.c */
extern unsigned long totalram_pages;
extern unsigned long totalhigh_pages;
extern long nr_swap_pages;
extern unsigned int nr_free_pages(void);
extern unsigned int nr_free_pages_pgdat(pg_data_t *pgdat);
extern unsigned int nr_free_buffer_pages(void);
extern unsigned int nr_free_pagecache_pages(void);

/* linux/mm/swap.c */
extern void FASTCALL(lru_cache_add(struct page *));
extern void FASTCALL(lru_cache_add_active(struct page *));
extern void FASTCALL(activate_page(struct page *));
extern void FASTCALL(mark_page_accessed(struct page *));
extern void lru_add_drain(void);
extern int lru_add_drain_all(void);
extern int rotate_reclaimable_page(struct page *page);
extern void swap_setup(void);

/* linux/mm/vmscan.c */
extern int try_to_free_pages(struct zone **, gfp_t);
extern int shrink_all_memory(int);
extern int vm_swappiness;

#ifdef CONFIG_NUMA
extern int zone_reclaim_mode;
extern int zone_reclaim_interval;
extern int zone_reclaim(struct zone *, gfp_t, unsigned int);
#else
#define zone_reclaim_mode 0
static inline int zone_reclaim(struct zone *z, gfp_t mask, unsigned int order)
{
	return 0;
}
#endif

#ifdef CONFIG_MIGRATION
extern int isolate_lru_page(struct page *p);
extern int putback_lru_pages(struct list_head *l);
extern int migrate_page(struct page *, struct page *);
extern void migrate_page_copy(struct page *, struct page *);
extern int migrate_page_remove_references(struct page *, struct page *, int);
extern int migrate_pages(struct list_head *l, struct list_head *t,
		struct list_head *moved, struct list_head *failed);
extern int fail_migrate_page(struct page *, struct page *);
#else
static inline int isolate_lru_page(struct page *p) { return -ENOSYS; }
static inline int putback_lru_pages(struct list_head *l) { return 0; }
static inline int migrate_pages(struct list_head *l, struct list_head *t,
	struct list_head *moved, struct list_head *failed) { return -ENOSYS; }
/* Possible settings for the migrate_page() method in address_operations */
#define migrate_page NULL
#define fail_migrate_page NULL
#endif

#ifdef CONFIG_MMU
/* linux/mm/shmem.c */
extern int shmem_unuse(swp_entry_t entry, struct page *page);
#endif /* CONFIG_MMU */

extern void swap_unplug_io_fn(struct backing_dev_info *, struct page *);

#ifdef CONFIG_SWAP
/* linux/mm/page_io.c */
extern int swap_readpage(struct file *, struct page *);
extern int swap_writepage(struct page *page, struct writeback_control *wbc);
extern int rw_swap_page_sync(int, swp_entry_t, struct page *);

/* linux/mm/swap_state.c */
extern struct address_space swapper_space;
#define total_swapcache_pages  swapper_space.nrpages
extern void show_swap_cache_info(void);
extern int add_to_swap(struct page *, gfp_t);
extern void __delete_from_swap_cache(struct page *);
extern void delete_from_swap_cache(struct page *);
extern int move_to_swap_cache(struct page *, swp_entry_t);
extern int move_from_swap_cache(struct page *, unsigned long,
		struct address_space *);
extern void free_page_and_swap_cache(struct page *);
extern void free_pages_and_swap_cache(struct page **, int);
extern struct page * lookup_swap_cache(swp_entry_t);
extern struct page * read_swap_cache_async(swp_entry_t, struct vm_area_struct *vma,
					   unsigned long addr);
/* linux/mm/swapfile.c */
extern long total_swap_pages;
extern unsigned int nr_swapfiles;
extern struct swap_info_struct swap_info[];
extern void si_swapinfo(struct sysinfo *);
extern swp_entry_t get_swap_page(void);
extern swp_entry_t get_swap_page_of_type(int type);
extern int swap_duplicate(swp_entry_t);
extern int valid_swaphandles(swp_entry_t, unsigned long *);
extern void swap_free(swp_entry_t);
extern void free_swap_and_cache(swp_entry_t);
extern sector_t map_swap_page(struct swap_info_struct *, pgoff_t);
extern struct swap_info_struct *get_swap_info_struct(unsigned);
extern int can_share_swap_page(struct page *);
extern int remove_exclusive_swap_page(struct page *);
struct backing_dev_info;

extern spinlock_t swap_lock;
extern int remove_vma_swap(struct vm_area_struct *vma, struct page *page);

/* linux/mm/thrash.c */
extern struct mm_struct * swap_token_mm;
extern unsigned long swap_token_default_timeout;
extern void grab_swap_token(void);
extern void __put_swap_token(struct mm_struct *);

static inline int has_swap_token(struct mm_struct *mm)
{
	return (mm == swap_token_mm);
}

static inline void put_swap_token(struct mm_struct *mm)
{
	if (has_swap_token(mm))
		__put_swap_token(mm);
}

static inline void disable_swap_token(void)
{
	put_swap_token(swap_token_mm);
}

#else /* CONFIG_SWAP */

#define total_swap_pages			0
#define total_swapcache_pages			0UL

#define si_swapinfo(val) \
	do { (val)->freeswap = (val)->totalswap = 0; } while (0)
/* only sparc can not include linux/pagemap.h in this file
 * so leave page_cache_release and release_pages undeclared... */
#define free_page_and_swap_cache(page) \
	page_cache_release(page)
#define free_pages_and_swap_cache(pages, nr) \
	release_pages((pages), (nr), 0);

#define show_swap_cache_info()			/*NOTHING*/
#define free_swap_and_cache(swp)		/*NOTHING*/
#define swap_duplicate(swp)			/*NOTHING*/
#define swap_free(swp)				/*NOTHING*/
#define read_swap_cache_async(swp,vma,addr)	NULL
#define lookup_swap_cache(swp)			NULL
#define valid_swaphandles(swp, off)		0
#define can_share_swap_page(p)			0
#define move_to_swap_cache(p, swp)		1
#define move_from_swap_cache(p, i, m)		1
#define __delete_from_swap_cache(p)		/*NOTHING*/
#define delete_from_swap_cache(p)		/*NOTHING*/
#define swap_token_default_timeout		0

static inline int remove_exclusive_swap_page(struct page *p)
{
	return 0;
}

static inline swp_entry_t get_swap_page(void)
{
	swp_entry_t entry;
	entry.val = 0;
	return entry;
}

/* linux/mm/thrash.c */
#define put_swap_token(x) do { } while(0)
#define grab_swap_token()  do { } while(0)
#define has_swap_token(x) 0
#define disable_swap_token() do { } while(0)

#endif /* CONFIG_SWAP */
#endif /* __KERNEL__*/
#endif /* _LINUX_SWAP_H */
