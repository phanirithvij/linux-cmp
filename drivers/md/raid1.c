/*
 * raid1.c : Multiple Devices driver for Linux
 *
 * Copyright (C) 1999, 2000, 2001 Ingo Molnar, Red Hat
 *
 * Copyright (C) 1996, 1997, 1998 Ingo Molnar, Miguel de Icaza, Gadi Oxman
 *
 * RAID-1 management functions.
 *
 * Better read-balancing code written by Mika Kuoppala <miku@iki.fi>, 2000
 *
 * Fixes to reconstruction by Jakob Østergaard" <jakob@ostenfeld.dk>
 * Various fixes by Neil Brown <neilb@cse.unsw.edu.au>
 *
 * Changes by Peter T. Breuer <ptb@it.uc3m.es> 31/1/2003 to support
 * bitmapped intelligence in resync:
 *
 *      - bitmap marked during normal i/o
 *      - bitmap used to skip nondirty blocks during sync
 *
 * Additions to bitmap code, (C) 2003-2004 Paul Clements, SteelEye Technology:
 * - persistent bitmap code
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * You should have received a copy of the GNU General Public License
 * (for example /usr/src/linux/COPYING); if not, write to the Free
 * Software Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include "dm-bio-list.h"
#include <linux/raid/raid1.h>
#include <linux/raid/bitmap.h>

#define DEBUG 0
#if DEBUG
#define PRINTK(x...) printk(x)
#else
#define PRINTK(x...)
#endif

/*
 * Number of guaranteed r1bios in case of extreme VM load:
 */
#define	NR_RAID1_BIOS 256


static void unplug_slaves(mddev_t *mddev);

static void allow_barrier(conf_t *conf);
static void lower_barrier(conf_t *conf);

static void * r1bio_pool_alloc(gfp_t gfp_flags, void *data)
{
	struct pool_info *pi = data;
	r1bio_t *r1_bio;
	int size = offsetof(r1bio_t, bios[pi->raid_disks]);

	/* allocate a r1bio with room for raid_disks entries in the bios array */
	r1_bio = kzalloc(size, gfp_flags);
	if (!r1_bio)
		unplug_slaves(pi->mddev);

	return r1_bio;
}

static void r1bio_pool_free(void *r1_bio, void *data)
{
	kfree(r1_bio);
}

#define RESYNC_BLOCK_SIZE (64*1024)
//#define RESYNC_BLOCK_SIZE PAGE_SIZE
#define RESYNC_SECTORS (RESYNC_BLOCK_SIZE >> 9)
#define RESYNC_PAGES ((RESYNC_BLOCK_SIZE + PAGE_SIZE-1) / PAGE_SIZE)
#define RESYNC_WINDOW (2048*1024)

static void * r1buf_pool_alloc(gfp_t gfp_flags, void *data)
{
	struct pool_info *pi = data;
	struct page *page;
	r1bio_t *r1_bio;
	struct bio *bio;
	int i, j;

	r1_bio = r1bio_pool_alloc(gfp_flags, pi);
	if (!r1_bio) {
		unplug_slaves(pi->mddev);
		return NULL;
	}

	/*
	 * Allocate bios : 1 for reading, n-1 for writing
	 */
	for (j = pi->raid_disks ; j-- ; ) {
		bio = bio_alloc(gfp_flags, RESYNC_PAGES);
		if (!bio)
			goto out_free_bio;
		r1_bio->bios[j] = bio;
	}
	/*
	 * Allocate RESYNC_PAGES data pages and attach them to
	 * the first bio.
	 * If this is a user-requested check/repair, allocate
	 * RESYNC_PAGES for each bio.
	 */
	if (test_bit(MD_RECOVERY_REQUESTED, &pi->mddev->recovery))
		j = pi->raid_disks;
	else
		j = 1;
	while(j--) {
		bio = r1_bio->bios[j];
		for (i = 0; i < RESYNC_PAGES; i++) {
			page = alloc_page(gfp_flags);
			if (unlikely(!page))
				goto out_free_pages;

			bio->bi_io_vec[i].bv_page = page;
		}
	}
	/* If not user-requests, copy the page pointers to all bios */
	if (!test_bit(MD_RECOVERY_REQUESTED, &pi->mddev->recovery)) {
		for (i=0; i<RESYNC_PAGES ; i++)
			for (j=1; j<pi->raid_disks; j++)
				r1_bio->bios[j]->bi_io_vec[i].bv_page =
					r1_bio->bios[0]->bi_io_vec[i].bv_page;
	}

	r1_bio->master_bio = NULL;

	return r1_bio;

out_free_pages:
	for (i=0; i < RESYNC_PAGES ; i++)
		for (j=0 ; j < pi->raid_disks; j++)
			safe_put_page(r1_bio->bios[j]->bi_io_vec[i].bv_page);
	j = -1;
out_free_bio:
	while ( ++j < pi->raid_disks )
		bio_put(r1_bio->bios[j]);
	r1bio_pool_free(r1_bio, data);
	return NULL;
}

static void r1buf_pool_free(void *__r1_bio, void *data)
{
	struct pool_info *pi = data;
	int i,j;
	r1bio_t *r1bio = __r1_bio;

	for (i = 0; i < RESYNC_PAGES; i++)
		for (j = pi->raid_disks; j-- ;) {
			if (j == 0 ||
			    r1bio->bios[j]->bi_io_vec[i].bv_page !=
			    r1bio->bios[0]->bi_io_vec[i].bv_page)
				safe_put_page(r1bio->bios[j]->bi_io_vec[i].bv_page);
		}
	for (i=0 ; i < pi->raid_disks; i++)
		bio_put(r1bio->bios[i]);

	r1bio_pool_free(r1bio, data);
}

static void put_all_bios(conf_t *conf, r1bio_t *r1_bio)
{
	int i;

	for (i = 0; i < conf->raid_disks; i++) {
		struct bio **bio = r1_bio->bios + i;
		if (*bio && *bio != IO_BLOCKED)
			bio_put(*bio);
		*bio = NULL;
	}
}

static void free_r1bio(r1bio_t *r1_bio)
{
	conf_t *conf = mddev_to_conf(r1_bio->mddev);

	/*
	 * Wake up any possible resync thread that waits for the device
	 * to go idle.
	 */
	allow_barrier(conf);

	put_all_bios(conf, r1_bio);
	mempool_free(r1_bio, conf->r1bio_pool);
}

static void put_buf(r1bio_t *r1_bio)
{
	conf_t *conf = mddev_to_conf(r1_bio->mddev);
	int i;

	for (i=0; i<conf->raid_disks; i++) {
		struct bio *bio = r1_bio->bios[i];
		if (bio->bi_end_io)
			rdev_dec_pending(conf->mirrors[i].rdev, r1_bio->mddev);
	}

	mempool_free(r1_bio, conf->r1buf_pool);

	lower_barrier(conf);
}

static void reschedule_retry(r1bio_t *r1_bio)
{
	unsigned long flags;
	mddev_t *mddev = r1_bio->mddev;
	conf_t *conf = mddev_to_conf(mddev);

	spin_lock_irqsave(&conf->device_lock, flags);
	list_add(&r1_bio->retry_list, &conf->retry_list);
	conf->nr_queued ++;
	spin_unlock_irqrestore(&conf->device_lock, flags);

	wake_up(&conf->wait_barrier);
	md_wakeup_thread(mddev->thread);
}

/*
 * raid_end_bio_io() is called when we have finished servicing a mirrored
 * operation and are ready to return a success/failure code to the buffer
 * cache layer.
 */
static void raid_end_bio_io(r1bio_t *r1_bio)
{
	struct bio *bio = r1_bio->master_bio;

	/* if nobody has done the final endio yet, do it now */
	if (!test_and_set_bit(R1BIO_Returned, &r1_bio->state)) {
		PRINTK(KERN_DEBUG "raid1: sync end %s on sectors %llu-%llu\n",
			(bio_data_dir(bio) == WRITE) ? "write" : "read",
			(unsigned long long) bio->bi_sector,
			(unsigned long long) bio->bi_sector +
				(bio->bi_size >> 9) - 1);

		bio_endio(bio, bio->bi_size,
			test_bit(R1BIO_Uptodate, &r1_bio->state) ? 0 : -EIO);
	}
	free_r1bio(r1_bio);
}

/*
 * Update disk head position estimator based on IRQ completion info.
 */
static inline void update_head_pos(int disk, r1bio_t *r1_bio)
{
	conf_t *conf = mddev_to_conf(r1_bio->mddev);

	conf->mirrors[disk].head_position =
		r1_bio->sector + (r1_bio->sectors);
}

static int raid1_end_read_request(struct bio *bio, unsigned int bytes_done, int error)
{
	int uptodate = test_bit(BIO_UPTODATE, &bio->bi_flags);
	r1bio_t * r1_bio = (r1bio_t *)(bio->bi_private);
	int mirror;
	conf_t *conf = mddev_to_conf(r1_bio->mddev);

	if (bio->bi_size)
		return 1;
	
	mirror = r1_bio->read_disk;
	/*
	 * this branch is our 'one mirror IO has finished' event handler:
	 */
	update_head_pos(mirror, r1_bio);

	if (uptodate || conf->working_disks <= 1) {
		/*
		 * Set R1BIO_Uptodate in our master bio, so that
		 * we will return a good error code for to the higher
		 * levels even if IO on some other mirrored buffer fails.
		 *
		 * The 'master' represents the composite IO operation to
		 * user-side. So if something waits for IO, then it will
		 * wait for the 'master' bio.
		 */
		if (uptodate)
			set_bit(R1BIO_Uptodate, &r1_bio->state);

		raid_end_bio_io(r1_bio);
	} else {
		/*
		 * oops, read error:
		 */
		char b[BDEVNAME_SIZE];
		if (printk_ratelimit())
			printk(KERN_ERR "raid1: %s: rescheduling sector %llu\n",
			       bdevname(conf->mirrors[mirror].rdev->bdev,b), (unsigned long long)r1_bio->sector);
		reschedule_retry(r1_bio);
	}

	rdev_dec_pending(conf->mirrors[mirror].rdev, conf->mddev);
	return 0;
}

static int raid1_end_write_request(struct bio *bio, unsigned int bytes_done, int error)
{
	int uptodate = test_bit(BIO_UPTODATE, &bio->bi_flags);
	r1bio_t * r1_bio = (r1bio_t *)(bio->bi_private);
	int mirror, behind = test_bit(R1BIO_BehindIO, &r1_bio->state);
	conf_t *conf = mddev_to_conf(r1_bio->mddev);

	if (bio->bi_size)
		return 1;

	for (mirror = 0; mirror < conf->raid_disks; mirror++)
		if (r1_bio->bios[mirror] == bio)
			break;

	if (error == -ENOTSUPP && test_bit(R1BIO_Barrier, &r1_bio->state)) {
		set_bit(BarriersNotsupp, &conf->mirrors[mirror].rdev->flags);
		set_bit(R1BIO_BarrierRetry, &r1_bio->state);
		r1_bio->mddev->barriers_work = 0;
	} else {
		/*
		 * this branch is our 'one mirror IO has finished' event handler:
		 */
		r1_bio->bios[mirror] = NULL;
		if (!uptodate) {
			md_error(r1_bio->mddev, conf->mirrors[mirror].rdev);
			/* an I/O failed, we can't clear the bitmap */
			set_bit(R1BIO_Degraded, &r1_bio->state);
		} else
			/*
			 * Set R1BIO_Uptodate in our master bio, so that
			 * we will return a good error code for to the higher
			 * levels even if IO on some other mirrored buffer fails.
			 *
			 * The 'master' represents the composite IO operation to
			 * user-side. So if something waits for IO, then it will
			 * wait for the 'master' bio.
			 */
			set_bit(R1BIO_Uptodate, &r1_bio->state);

		update_head_pos(mirror, r1_bio);

		if (behind) {
			if (test_bit(WriteMostly, &conf->mirrors[mirror].rdev->flags))
				atomic_dec(&r1_bio->behind_remaining);

			/* In behind mode, we ACK the master bio once the I/O has safely
			 * reached all non-writemostly disks. Setting the Returned bit
			 * ensures that this gets done only once -- we don't ever want to
			 * return -EIO here, instead we'll wait */

			if (atomic_read(&r1_bio->behind_remaining) >= (atomic_read(&r1_bio->remaining)-1) &&
			    test_bit(R1BIO_Uptodate, &r1_bio->state)) {
				/* Maybe we can return now */
				if (!test_and_set_bit(R1BIO_Returned, &r1_bio->state)) {
					struct bio *mbio = r1_bio->master_bio;
					PRINTK(KERN_DEBUG "raid1: behind end write sectors %llu-%llu\n",
					       (unsigned long long) mbio->bi_sector,
					       (unsigned long long) mbio->bi_sector +
					       (mbio->bi_size >> 9) - 1);
					bio_endio(mbio, mbio->bi_size, 0);
				}
			}
		}
	}
	/*
	 *
	 * Let's see if all mirrored write operations have finished
	 * already.
	 */
	if (atomic_dec_and_test(&r1_bio->remaining)) {
		if (test_bit(R1BIO_BarrierRetry, &r1_bio->state)) {
			reschedule_retry(r1_bio);
			/* Don't dec_pending yet, we want to hold
			 * the reference over the retry
			 */
			return 0;
		}
		if (test_bit(R1BIO_BehindIO, &r1_bio->state)) {
			/* free extra copy of the data pages */
			int i = bio->bi_vcnt;
			while (i--)
				safe_put_page(bio->bi_io_vec[i].bv_page);
		}
		/* clear the bitmap if all writes complete successfully */
		bitmap_endwrite(r1_bio->mddev->bitmap, r1_bio->sector,
				r1_bio->sectors,
				!test_bit(R1BIO_Degraded, &r1_bio->state),
				behind);
		md_write_end(r1_bio->mddev);
		raid_end_bio_io(r1_bio);
	}

	if (r1_bio->bios[mirror]==NULL)
		bio_put(bio);

	rdev_dec_pending(conf->mirrors[mirror].rdev, conf->mddev);
	return 0;
}


/*
 * This routine returns the disk from which the requested read should
 * be done. There is a per-array 'next expected sequential IO' sector
 * number - if this matches on the next IO then we use the last disk.
 * There is also a per-disk 'last know head position' sector that is
 * maintained from IRQ contexts, both the normal and the resync IO
 * completion handlers update this position correctly. If there is no
 * perfect sequential match then we pick the disk whose head is closest.
 *
 * If there are 2 mirrors in the same 2 devices, performance degrades
 * because position is mirror, not device based.
 *
 * The rdev for the device selected will have nr_pending incremented.
 */
static int read_balance(conf_t *conf, r1bio_t *r1_bio)
{
	const unsigned long this_sector = r1_bio->sector;
	int new_disk = conf->last_used, disk = new_disk;
	int wonly_disk = -1;
	const int sectors = r1_bio->sectors;
	sector_t new_distance, current_distance;
	mdk_rdev_t *rdev;

	rcu_read_lock();
	/*
	 * Check if we can balance. We can balance on the whole
	 * device if no resync is going on, or below the resync window.
	 * We take the first readable disk when above the resync window.
	 */
 retry:
	if (conf->mddev->recovery_cp < MaxSector &&
	    (this_sector + sectors >= conf->next_resync)) {
		/* Choose the first operation device, for consistancy */
		new_disk = 0;

		for (rdev = rcu_dereference(conf->mirrors[new_disk].rdev);
		     r1_bio->bios[new_disk] == IO_BLOCKED ||
		     !rdev || !test_bit(In_sync, &rdev->flags)
			     || test_bit(WriteMostly, &rdev->flags);
		     rdev = rcu_dereference(conf->mirrors[++new_disk].rdev)) {

			if (rdev && test_bit(In_sync, &rdev->flags) &&
				r1_bio->bios[new_disk] != IO_BLOCKED)
				wonly_disk = new_disk;

			if (new_disk == conf->raid_disks - 1) {
				new_disk = wonly_disk;
				break;
			}
		}
		goto rb_out;
	}


	/* make sure the disk is operational */
	for (rdev = rcu_dereference(conf->mirrors[new_disk].rdev);
	     r1_bio->bios[new_disk] == IO_BLOCKED ||
	     !rdev || !test_bit(In_sync, &rdev->flags) ||
		     test_bit(WriteMostly, &rdev->flags);
	     rdev = rcu_dereference(conf->mirrors[new_disk].rdev)) {

		if (rdev && test_bit(In_sync, &rdev->flags) &&
		    r1_bio->bios[new_disk] != IO_BLOCKED)
			wonly_disk = new_disk;

		if (new_disk <= 0)
			new_disk = conf->raid_disks;
		new_disk--;
		if (new_disk == disk) {
			new_disk = wonly_disk;
			break;
		}
	}

	if (new_disk < 0)
		goto rb_out;

	disk = new_disk;
	/* now disk == new_disk == starting point for search */

	/*
	 * Don't change to another disk for sequential reads:
	 */
	if (conf->next_seq_sect == this_sector)
		goto rb_out;
	if (this_sector == conf->mirrors[new_disk].head_position)
		goto rb_out;

	current_distance = abs(this_sector - conf->mirrors[disk].head_position);

	/* Find the disk whose head is closest */

	do {
		if (disk <= 0)
			disk = conf->raid_disks;
		disk--;

		rdev = rcu_dereference(conf->mirrors[disk].rdev);

		if (!rdev || r1_bio->bios[disk] == IO_BLOCKED ||
		    !test_bit(In_sync, &rdev->flags) ||
		    test_bit(WriteMostly, &rdev->flags))
			continue;

		if (!atomic_read(&rdev->nr_pending)) {
			new_disk = disk;
			break;
		}
		new_distance = abs(this_sector - conf->mirrors[disk].head_position);
		if (new_distance < current_distance) {
			current_distance = new_distance;
			new_disk = disk;
		}
	} while (disk != conf->last_used);

 rb_out:


	if (new_disk >= 0) {
		rdev = rcu_dereference(conf->mirrors[new_disk].rdev);
		if (!rdev)
			goto retry;
		atomic_inc(&rdev->nr_pending);
		if (!test_bit(In_sync, &rdev->flags)) {
			/* cannot risk returning a device that failed
			 * before we inc'ed nr_pending
			 */
			rdev_dec_pending(rdev, conf->mddev);
			goto retry;
		}
		conf->next_seq_sect = this_sector + sectors;
		conf->last_used = new_disk;
	}
	rcu_read_unlock();

	return new_disk;
}

static void unplug_slaves(mddev_t *mddev)
{
	conf_t *conf = mddev_to_conf(mddev);
	int i;

	rcu_read_lock();
	for (i=0; i<mddev->raid_disks; i++) {
		mdk_rdev_t *rdev = rcu_dereference(conf->mirrors[i].rdev);
		if (rdev && !test_bit(Faulty, &rdev->flags) && atomic_read(&rdev->nr_pending)) {
			request_queue_t *r_queue = bdev_get_queue(rdev->bdev);

			atomic_inc(&rdev->nr_pending);
			rcu_read_unlock();

			if (r_queue->unplug_fn)
				r_queue->unplug_fn(r_queue);

			rdev_dec_pending(rdev, mddev);
			rcu_read_lock();
		}
	}
	rcu_read_unlock();
}

static void raid1_unplug(request_queue_t *q)
{
	mddev_t *mddev = q->queuedata;

	unplug_slaves(mddev);
	md_wakeup_thread(mddev->thread);
}

static int raid1_issue_flush(request_queue_t *q, struct gendisk *disk,
			     sector_t *error_sector)
{
	mddev_t *mddev = q->queuedata;
	conf_t *conf = mddev_to_conf(mddev);
	int i, ret = 0;

	rcu_read_lock();
	for (i=0; i<mddev->raid_disks && ret == 0; i++) {
		mdk_rdev_t *rdev = rcu_dereference(conf->mirrors[i].rdev);
		if (rdev && !test_bit(Faulty, &rdev->flags)) {
			struct block_device *bdev = rdev->bdev;
			request_queue_t *r_queue = bdev_get_queue(bdev);

			if (!r_queue->issue_flush_fn)
				ret = -EOPNOTSUPP;
			else {
				atomic_inc(&rdev->nr_pending);
				rcu_read_unlock();
				ret = r_queue->issue_flush_fn(r_queue, bdev->bd_disk,
							      error_sector);
				rdev_dec_pending(rdev, mddev);
				rcu_read_lock();
			}
		}
	}
	rcu_read_unlock();
	return ret;
}

/* Barriers....
 * Sometimes we need to suspend IO while we do something else,
 * either some resync/recovery, or reconfigure the array.
 * To do this we raise a 'barrier'.
 * The 'barrier' is a counter that can be raised multiple times
 * to count how many activities are happening which preclude
 * normal IO.
 * We can only raise the barrier if there is no pending IO.
 * i.e. if nr_pending == 0.
 * We choose only to raise the barrier if no-one is waiting for the
 * barrier to go down.  This means that as soon as an IO request
 * is ready, no other operations which require a barrier will start
 * until the IO request has had a chance.
 *
 * So: regular IO calls 'wait_barrier'.  When that returns there
 *    is no backgroup IO happening,  It must arrange to call
 *    allow_barrier when it has finished its IO.
 * backgroup IO calls must call raise_barrier.  Once that returns
 *    there is no normal IO happeing.  It must arrange to call
 *    lower_barrier when the particular background IO completes.
 */
#define RESYNC_DEPTH 32

static void raise_barrier(conf_t *conf)
{
	spin_lock_irq(&conf->resync_lock);

	/* Wait until no block IO is waiting */
	wait_event_lock_irq(conf->wait_barrier, !conf->nr_waiting,
			    conf->resync_lock,
			    raid1_unplug(conf->mddev->queue));

	/* block any new IO from starting */
	conf->barrier++;

	/* No wait for all pending IO to complete */
	wait_event_lock_irq(conf->wait_barrier,
			    !conf->nr_pending && conf->barrier < RESYNC_DEPTH,
			    conf->resync_lock,
			    raid1_unplug(conf->mddev->queue));

	spin_unlock_irq(&conf->resync_lock);
}

static void lower_barrier(conf_t *conf)
{
	unsigned long flags;
	spin_lock_irqsave(&conf->resync_lock, flags);
	conf->barrier--;
	spin_unlock_irqrestore(&conf->resync_lock, flags);
	wake_up(&conf->wait_barrier);
}

static void wait_barrier(conf_t *conf)
{
	spin_lock_irq(&conf->resync_lock);
	if (conf->barrier) {
		conf->nr_waiting++;
		wait_event_lock_irq(conf->wait_barrier, !conf->barrier,
				    conf->resync_lock,
				    raid1_unplug(conf->mddev->queue));
		conf->nr_waiting--;
	}
	conf->nr_pending++;
	spin_unlock_irq(&conf->resync_lock);
}

static void allow_barrier(conf_t *conf)
{
	unsigned long flags;
	spin_lock_irqsave(&conf->resync_lock, flags);
	conf->nr_pending--;
	spin_unlock_irqrestore(&conf->resync_lock, flags);
	wake_up(&conf->wait_barrier);
}

static void freeze_array(conf_t *conf)
{
	/* stop syncio and normal IO and wait for everything to
	 * go quite.
	 * We increment barrier and nr_waiting, and then
	 * wait until barrier+nr_pending match nr_queued+2
	 */
	spin_lock_irq(&conf->resync_lock);
	conf->barrier++;
	conf->nr_waiting++;
	wait_event_lock_irq(conf->wait_barrier,
			    conf->barrier+conf->nr_pending == conf->nr_queued+2,
			    conf->resync_lock,
			    raid1_unplug(conf->mddev->queue));
	spin_unlock_irq(&conf->resync_lock);
}
static void unfreeze_array(conf_t *conf)
{
	/* reverse the effect of the freeze */
	spin_lock_irq(&conf->resync_lock);
	conf->barrier--;
	conf->nr_waiting--;
	wake_up(&conf->wait_barrier);
	spin_unlock_irq(&conf->resync_lock);
}


/* duplicate the data pages for behind I/O */
static struct page **alloc_behind_pages(struct bio *bio)
{
	int i;
	struct bio_vec *bvec;
	struct page **pages = kzalloc(bio->bi_vcnt * sizeof(struct page *),
					GFP_NOIO);
	if (unlikely(!pages))
		goto do_sync_io;

	bio_for_each_segment(bvec, bio, i) {
		pages[i] = alloc_page(GFP_NOIO);
		if (unlikely(!pages[i]))
			goto do_sync_io;
		memcpy(kmap(pages[i]) + bvec->bv_offset,
			kmap(bvec->bv_page) + bvec->bv_offset, bvec->bv_len);
		kunmap(pages[i]);
		kunmap(bvec->bv_page);
	}

	return pages;

do_sync_io:
	if (pages)
		for (i = 0; i < bio->bi_vcnt && pages[i]; i++)
			put_page(pages[i]);
	kfree(pages);
	PRINTK("%dB behind alloc failed, doing sync I/O\n", bio->bi_size);
	return NULL;
}

static int make_request(request_queue_t *q, struct bio * bio)
{
	mddev_t *mddev = q->queuedata;
	conf_t *conf = mddev_to_conf(mddev);
	mirror_info_t *mirror;
	r1bio_t *r1_bio;
	struct bio *read_bio;
	int i, targets = 0, disks;
	mdk_rdev_t *rdev;
	struct bitmap *bitmap = mddev->bitmap;
	unsigned long flags;
	struct bio_list bl;
	struct page **behind_pages = NULL;
	const int rw = bio_data_dir(bio);
	int do_barriers;

	if (unlikely(!mddev->barriers_work && bio_barrier(bio))) {
		bio_endio(bio, bio->bi_size, -EOPNOTSUPP);
		return 0;
	}

	/*
	 * Register the new request and wait if the reconstruction
	 * thread has put up a bar for new requests.
	 * Continue immediately if no resync is active currently.
	 */
	md_write_start(mddev, bio); /* wait on superblock update early */

	wait_barrier(conf);

	disk_stat_inc(mddev->gendisk, ios[rw]);
	disk_stat_add(mddev->gendisk, sectors[rw], bio_sectors(bio));

	/*
	 * make_request() can abort the operation when READA is being
	 * used and no empty request is available.
	 *
	 */
	r1_bio = mempool_alloc(conf->r1bio_pool, GFP_NOIO);

	r1_bio->master_bio = bio;
	r1_bio->sectors = bio->bi_size >> 9;
	r1_bio->state = 0;
	r1_bio->mddev = mddev;
	r1_bio->sector = bio->bi_sector;

	if (rw == READ) {
		/*
		 * read balancing logic:
		 */
		int rdisk = read_balance(conf, r1_bio);

		if (rdisk < 0) {
			/* couldn't find anywhere to read from */
			raid_end_bio_io(r1_bio);
			return 0;
		}
		mirror = conf->mirrors + rdisk;

		r1_bio->read_disk = rdisk;

		read_bio = bio_clone(bio, GFP_NOIO);

		r1_bio->bios[rdisk] = read_bio;

		read_bio->bi_sector = r1_bio->sector + mirror->rdev->data_offset;
		read_bio->bi_bdev = mirror->rdev->bdev;
		read_bio->bi_end_io = raid1_end_read_request;
		read_bio->bi_rw = READ;
		read_bio->bi_private = r1_bio;

		generic_make_request(read_bio);
		return 0;
	}

	/*
	 * WRITE:
	 */
	/* first select target devices under spinlock and
	 * inc refcount on their rdev.  Record them by setting
	 * bios[x] to bio
	 */
	disks = conf->raid_disks;
#if 0
	{ static int first=1;
	if (first) printk("First Write sector %llu disks %d\n",
			  (unsigned long long)r1_bio->sector, disks);
	first = 0;
	}
#endif
	rcu_read_lock();
	for (i = 0;  i < disks; i++) {
		if ((rdev=rcu_dereference(conf->mirrors[i].rdev)) != NULL &&
		    !test_bit(Faulty, &rdev->flags)) {
			atomic_inc(&rdev->nr_pending);
			if (test_bit(Faulty, &rdev->flags)) {
				rdev_dec_pending(rdev, mddev);
				r1_bio->bios[i] = NULL;
			} else
				r1_bio->bios[i] = bio;
			targets++;
		} else
			r1_bio->bios[i] = NULL;
	}
	rcu_read_unlock();

	BUG_ON(targets == 0); /* we never fail the last device */

	if (targets < conf->raid_disks) {
		/* array is degraded, we will not clear the bitmap
		 * on I/O completion (see raid1_end_write_request) */
		set_bit(R1BIO_Degraded, &r1_bio->state);
	}

	/* do behind I/O ? */
	if (bitmap &&
	    atomic_read(&bitmap->behind_writes) < bitmap->max_write_behind &&
	    (behind_pages = alloc_behind_pages(bio)) != NULL)
		set_bit(R1BIO_BehindIO, &r1_bio->state);

	atomic_set(&r1_bio->remaining, 0);
	atomic_set(&r1_bio->behind_remaining, 0);

	do_barriers = bio->bi_rw & BIO_RW_BARRIER;
	if (do_barriers)
		set_bit(R1BIO_Barrier, &r1_bio->state);

	bio_list_init(&bl);
	for (i = 0; i < disks; i++) {
		struct bio *mbio;
		if (!r1_bio->bios[i])
			continue;

		mbio = bio_clone(bio, GFP_NOIO);
		r1_bio->bios[i] = mbio;

		mbio->bi_sector	= r1_bio->sector + conf->mirrors[i].rdev->data_offset;
		mbio->bi_bdev = conf->mirrors[i].rdev->bdev;
		mbio->bi_end_io	= raid1_end_write_request;
		mbio->bi_rw = WRITE | do_barriers;
		mbio->bi_private = r1_bio;

		if (behind_pages) {
			struct bio_vec *bvec;
			int j;

			/* Yes, I really want the '__' version so that
			 * we clear any unused pointer in the io_vec, rather
			 * than leave them unchanged.  This is important
			 * because when we come to free the pages, we won't
			 * know the originial bi_idx, so we just free
			 * them all
			 */
			__bio_for_each_segment(bvec, mbio, j, 0)
				bvec->bv_page = behind_pages[j];
			if (test_bit(WriteMostly, &conf->mirrors[i].rdev->flags))
				atomic_inc(&r1_bio->behind_remaining);
		}

		atomic_inc(&r1_bio->remaining);

		bio_list_add(&bl, mbio);
	}
	kfree(behind_pages); /* the behind pages are attached to the bios now */

	bitmap_startwrite(bitmap, bio->bi_sector, r1_bio->sectors,
				test_bit(R1BIO_BehindIO, &r1_bio->state));
	spin_lock_irqsave(&conf->device_lock, flags);
	bio_list_merge(&conf->pending_bio_list, &bl);
	bio_list_init(&bl);

	blk_plug_device(mddev->queue);
	spin_unlock_irqrestore(&conf->device_lock, flags);

#if 0
	while ((bio = bio_list_pop(&bl)) != NULL)
		generic_make_request(bio);
#endif

	return 0;
}

static void status(struct seq_file *seq, mddev_t *mddev)
{
	conf_t *conf = mddev_to_conf(mddev);
	int i;

	seq_printf(seq, " [%d/%d] [", conf->raid_disks,
						conf->working_disks);
	for (i = 0; i < conf->raid_disks; i++)
		seq_printf(seq, "%s",
			      conf->mirrors[i].rdev &&
			      test_bit(In_sync, &conf->mirrors[i].rdev->flags) ? "U" : "_");
	seq_printf(seq, "]");
}


static void error(mddev_t *mddev, mdk_rdev_t *rdev)
{
	char b[BDEVNAME_SIZE];
	conf_t *conf = mddev_to_conf(mddev);

	/*
	 * If it is not operational, then we have already marked it as dead
	 * else if it is the last working disks, ignore the error, let the
	 * next level up know.
	 * else mark the drive as failed
	 */
	if (test_bit(In_sync, &rdev->flags)
	    && conf->working_disks == 1)
		/*
		 * Don't fail the drive, act as though we were just a
		 * normal single drive
		 */
		return;
	if (test_bit(In_sync, &rdev->flags)) {
		mddev->degraded++;
		conf->working_disks--;
		/*
		 * if recovery is running, make sure it aborts.
		 */
		set_bit(MD_RECOVERY_ERR, &mddev->recovery);
	}
	clear_bit(In_sync, &rdev->flags);
	set_bit(Faulty, &rdev->flags);
	mddev->sb_dirty = 1;
	printk(KERN_ALERT "raid1: Disk failure on %s, disabling device. \n"
		"	Operation continuing on %d devices\n",
		bdevname(rdev->bdev,b), conf->working_disks);
}

static void print_conf(conf_t *conf)
{
	int i;
	mirror_info_t *tmp;

	printk("RAID1 conf printout:\n");
	if (!conf) {
		printk("(!conf)\n");
		return;
	}
	printk(" --- wd:%d rd:%d\n", conf->working_disks,
		conf->raid_disks);

	for (i = 0; i < conf->raid_disks; i++) {
		char b[BDEVNAME_SIZE];
		tmp = conf->mirrors + i;
		if (tmp->rdev)
			printk(" disk %d, wo:%d, o:%d, dev:%s\n",
				i, !test_bit(In_sync, &tmp->rdev->flags), !test_bit(Faulty, &tmp->rdev->flags),
				bdevname(tmp->rdev->bdev,b));
	}
}

static void close_sync(conf_t *conf)
{
	wait_barrier(conf);
	allow_barrier(conf);

	mempool_destroy(conf->r1buf_pool);
	conf->r1buf_pool = NULL;
}

static int raid1_spare_active(mddev_t *mddev)
{
	int i;
	conf_t *conf = mddev->private;
	mirror_info_t *tmp;

	/*
	 * Find all failed disks within the RAID1 configuration 
	 * and mark them readable
	 */
	for (i = 0; i < conf->raid_disks; i++) {
		tmp = conf->mirrors + i;
		if (tmp->rdev 
		    && !test_bit(Faulty, &tmp->rdev->flags)
		    && !test_bit(In_sync, &tmp->rdev->flags)) {
			conf->working_disks++;
			mddev->degraded--;
			set_bit(In_sync, &tmp->rdev->flags);
		}
	}

	print_conf(conf);
	return 0;
}


static int raid1_add_disk(mddev_t *mddev, mdk_rdev_t *rdev)
{
	conf_t *conf = mddev->private;
	int found = 0;
	int mirror = 0;
	mirror_info_t *p;

	for (mirror=0; mirror < mddev->raid_disks; mirror++)
		if ( !(p=conf->mirrors+mirror)->rdev) {

			blk_queue_stack_limits(mddev->queue,
					       rdev->bdev->bd_disk->queue);
			/* as we don't honour merge_bvec_fn, we must never risk
			 * violating it, so limit ->max_sector to one PAGE, as
			 * a one page request is never in violation.
			 */
			if (rdev->bdev->bd_disk->queue->merge_bvec_fn &&
			    mddev->queue->max_sectors > (PAGE_SIZE>>9))
				blk_queue_max_sectors(mddev->queue, PAGE_SIZE>>9);

			p->head_position = 0;
			rdev->raid_disk = mirror;
			found = 1;
			/* As all devices are equivalent, we don't need a full recovery
			 * if this was recently any drive of the array
			 */
			if (rdev->saved_raid_disk < 0)
				conf->fullsync = 1;
			rcu_assign_pointer(p->rdev, rdev);
			break;
		}

	print_conf(conf);
	return found;
}

static int raid1_remove_disk(mddev_t *mddev, int number)
{
	conf_t *conf = mddev->private;
	int err = 0;
	mdk_rdev_t *rdev;
	mirror_info_t *p = conf->mirrors+ number;

	print_conf(conf);
	rdev = p->rdev;
	if (rdev) {
		if (test_bit(In_sync, &rdev->flags) ||
		    atomic_read(&rdev->nr_pending)) {
			err = -EBUSY;
			goto abort;
		}
		p->rdev = NULL;
		synchronize_rcu();
		if (atomic_read(&rdev->nr_pending)) {
			/* lost the race, try later */
			err = -EBUSY;
			p->rdev = rdev;
		}
	}
abort:

	print_conf(conf);
	return err;
}


static int end_sync_read(struct bio *bio, unsigned int bytes_done, int error)
{
	r1bio_t * r1_bio = (r1bio_t *)(bio->bi_private);
	int i;

	if (bio->bi_size)
		return 1;

	for (i=r1_bio->mddev->raid_disks; i--; )
		if (r1_bio->bios[i] == bio)
			break;
	BUG_ON(i < 0);
	update_head_pos(i, r1_bio);
	/*
	 * we have read a block, now it needs to be re-written,
	 * or re-read if the read failed.
	 * We don't do much here, just schedule handling by raid1d
	 */
	if (test_bit(BIO_UPTODATE, &bio->bi_flags))
		set_bit(R1BIO_Uptodate, &r1_bio->state);

	if (atomic_dec_and_test(&r1_bio->remaining))
		reschedule_retry(r1_bio);
	return 0;
}

static int end_sync_write(struct bio *bio, unsigned int bytes_done, int error)
{
	int uptodate = test_bit(BIO_UPTODATE, &bio->bi_flags);
	r1bio_t * r1_bio = (r1bio_t *)(bio->bi_private);
	mddev_t *mddev = r1_bio->mddev;
	conf_t *conf = mddev_to_conf(mddev);
	int i;
	int mirror=0;

	if (bio->bi_size)
		return 1;

	for (i = 0; i < conf->raid_disks; i++)
		if (r1_bio->bios[i] == bio) {
			mirror = i;
			break;
		}
	if (!uptodate)
		md_error(mddev, conf->mirrors[mirror].rdev);

	update_head_pos(mirror, r1_bio);

	if (atomic_dec_and_test(&r1_bio->remaining)) {
		md_done_sync(mddev, r1_bio->sectors, uptodate);
		put_buf(r1_bio);
	}
	return 0;
}

static void sync_request_write(mddev_t *mddev, r1bio_t *r1_bio)
{
	conf_t *conf = mddev_to_conf(mddev);
	int i;
	int disks = conf->raid_disks;
	struct bio *bio, *wbio;

	bio = r1_bio->bios[r1_bio->read_disk];


	if (test_bit(MD_RECOVERY_REQUESTED, &mddev->recovery)) {
		/* We have read all readable devices.  If we haven't
		 * got the block, then there is no hope left.
		 * If we have, then we want to do a comparison
		 * and skip the write if everything is the same.
		 * If any blocks failed to read, then we need to
		 * attempt an over-write
		 */
		int primary;
		if (!test_bit(R1BIO_Uptodate, &r1_bio->state)) {
			for (i=0; i<mddev->raid_disks; i++)
				if (r1_bio->bios[i]->bi_end_io == end_sync_read)
					md_error(mddev, conf->mirrors[i].rdev);

			md_done_sync(mddev, r1_bio->sectors, 1);
			put_buf(r1_bio);
			return;
		}
		for (primary=0; primary<mddev->raid_disks; primary++)
			if (r1_bio->bios[primary]->bi_end_io == end_sync_read &&
			    test_bit(BIO_UPTODATE, &r1_bio->bios[primary]->bi_flags)) {
				r1_bio->bios[primary]->bi_end_io = NULL;
				rdev_dec_pending(conf->mirrors[primary].rdev, mddev);
				break;
			}
		r1_bio->read_disk = primary;
		for (i=0; i<mddev->raid_disks; i++)
			if (r1_bio->bios[i]->bi_end_io == end_sync_read &&
			    test_bit(BIO_UPTODATE, &r1_bio->bios[i]->bi_flags)) {
				int j;
				int vcnt = r1_bio->sectors >> (PAGE_SHIFT- 9);
				struct bio *pbio = r1_bio->bios[primary];
				struct bio *sbio = r1_bio->bios[i];
				for (j = vcnt; j-- ; )
					if (memcmp(page_address(pbio->bi_io_vec[j].bv_page),
						   page_address(sbio->bi_io_vec[j].bv_page),
						   PAGE_SIZE))
						break;
				if (j >= 0)
					mddev->resync_mismatches += r1_bio->sectors;
				if (j < 0 || test_bit(MD_RECOVERY_CHECK, &mddev->recovery)) {
					sbio->bi_end_io = NULL;
					rdev_dec_pending(conf->mirrors[i].rdev, mddev);
				} else {
					/* fixup the bio for reuse */
					sbio->bi_vcnt = vcnt;
					sbio->bi_size = r1_bio->sectors << 9;
					sbio->bi_idx = 0;
					sbio->bi_phys_segments = 0;
					sbio->bi_hw_segments = 0;
					sbio->bi_hw_front_size = 0;
					sbio->bi_hw_back_size = 0;
					sbio->bi_flags &= ~(BIO_POOL_MASK - 1);
					sbio->bi_flags |= 1 << BIO_UPTODATE;
					sbio->bi_next = NULL;
					sbio->bi_sector = r1_bio->sector +
						conf->mirrors[i].rdev->data_offset;
					sbio->bi_bdev = conf->mirrors[i].rdev->bdev;
				}
			}
	}
	if (!test_bit(R1BIO_Uptodate, &r1_bio->state)) {
		/* ouch - failed to read all of that.
		 * Try some synchronous reads of other devices to get
		 * good data, much like with normal read errors.  Only
		 * read into the pages we already have so they we don't
		 * need to re-issue the read request.
		 * We don't need to freeze the array, because being in an
		 * active sync request, there is no normal IO, and
		 * no overlapping syncs.
		 */
		sector_t sect = r1_bio->sector;
		int sectors = r1_bio->sectors;
		int idx = 0;

		while(sectors) {
			int s = sectors;
			int d = r1_bio->read_disk;
			int success = 0;
			mdk_rdev_t *rdev;

			if (s > (PAGE_SIZE>>9))
				s = PAGE_SIZE >> 9;
			do {
				if (r1_bio->bios[d]->bi_end_io == end_sync_read) {
					rdev = conf->mirrors[d].rdev;
					if (sync_page_io(rdev->bdev,
							 sect + rdev->data_offset,
							 s<<9,
							 bio->bi_io_vec[idx].bv_page,
							 READ)) {
						success = 1;
						break;
					}
				}
				d++;
				if (d == conf->raid_disks)
					d = 0;
			} while (!success && d != r1_bio->read_disk);

			if (success) {
				int start = d;
				/* write it back and re-read */
				set_bit(R1BIO_Uptodate, &r1_bio->state);
				while (d != r1_bio->read_disk) {
					if (d == 0)
						d = conf->raid_disks;
					d--;
					if (r1_bio->bios[d]->bi_end_io != end_sync_read)
						continue;
					rdev = conf->mirrors[d].rdev;
					atomic_add(s, &rdev->corrected_errors);
					if (sync_page_io(rdev->bdev,
							 sect + rdev->data_offset,
							 s<<9,
							 bio->bi_io_vec[idx].bv_page,
							 WRITE) == 0)
						md_error(mddev, rdev);
				}
				d = start;
				while (d != r1_bio->read_disk) {
					if (d == 0)
						d = conf->raid_disks;
					d--;
					if (r1_bio->bios[d]->bi_end_io != end_sync_read)
						continue;
					rdev = conf->mirrors[d].rdev;
					if (sync_page_io(rdev->bdev,
							 sect + rdev->data_offset,
							 s<<9,
							 bio->bi_io_vec[idx].bv_page,
							 READ) == 0)
						md_error(mddev, rdev);
				}
			} else {
				char b[BDEVNAME_SIZE];
				/* Cannot read from anywhere, array is toast */
				md_error(mddev, conf->mirrors[r1_bio->read_disk].rdev);
				printk(KERN_ALERT "raid1: %s: unrecoverable I/O read error"
				       " for block %llu\n",
				       bdevname(bio->bi_bdev,b),
				       (unsigned long long)r1_bio->sector);
				md_done_sync(mddev, r1_bio->sectors, 0);
				put_buf(r1_bio);
				return;
			}
			sectors -= s;
			sect += s;
			idx ++;
		}
	}

	/*
	 * schedule writes
	 */
	atomic_set(&r1_bio->remaining, 1);
	for (i = 0; i < disks ; i++) {
		wbio = r1_bio->bios[i];
		if (wbio->bi_end_io == NULL ||
		    (wbio->bi_end_io == end_sync_read &&
		     (i == r1_bio->read_disk ||
		      !test_bit(MD_RECOVERY_SYNC, &mddev->recovery))))
			continue;

		wbio->bi_rw = WRITE;
		wbio->bi_end_io = end_sync_write;
		atomic_inc(&r1_bio->remaining);
		md_sync_acct(conf->mirrors[i].rdev->bdev, wbio->bi_size >> 9);

		generic_make_request(wbio);
	}

	if (atomic_dec_and_test(&r1_bio->remaining)) {
		/* if we're here, all write(s) have completed, so clean up */
		md_done_sync(mddev, r1_bio->sectors, 1);
		put_buf(r1_bio);
	}
}

/*
 * This is a kernel thread which:
 *
 *	1.	Retries failed read operations on working mirrors.
 *	2.	Updates the raid superblock when problems encounter.
 *	3.	Performs writes following reads for array syncronising.
 */

static void raid1d(mddev_t *mddev)
{
	r1bio_t *r1_bio;
	struct bio *bio;
	unsigned long flags;
	conf_t *conf = mddev_to_conf(mddev);
	struct list_head *head = &conf->retry_list;
	int unplug=0;
	mdk_rdev_t *rdev;

	md_check_recovery(mddev);
	
	for (;;) {
		char b[BDEVNAME_SIZE];
		spin_lock_irqsave(&conf->device_lock, flags);

		if (conf->pending_bio_list.head) {
			bio = bio_list_get(&conf->pending_bio_list);
			blk_remove_plug(mddev->queue);
			spin_unlock_irqrestore(&conf->device_lock, flags);
			/* flush any pending bitmap writes to disk before proceeding w/ I/O */
			if (bitmap_unplug(mddev->bitmap) != 0)
				printk("%s: bitmap file write failed!\n", mdname(mddev));

			while (bio) { /* submit pending writes */
				struct bio *next = bio->bi_next;
				bio->bi_next = NULL;
				generic_make_request(bio);
				bio = next;
			}
			unplug = 1;

			continue;
		}

		if (list_empty(head))
			break;
		r1_bio = list_entry(head->prev, r1bio_t, retry_list);
		list_del(head->prev);
		conf->nr_queued--;
		spin_unlock_irqrestore(&conf->device_lock, flags);

		mddev = r1_bio->mddev;
		conf = mddev_to_conf(mddev);
		if (test_bit(R1BIO_IsSync, &r1_bio->state)) {
			sync_request_write(mddev, r1_bio);
			unplug = 1;
		} else if (test_bit(R1BIO_BarrierRetry, &r1_bio->state)) {
			/* some requests in the r1bio were BIO_RW_BARRIER
			 * requests which failed with -ENOTSUPP.  Hohumm..
			 * Better resubmit without the barrier.
			 * We know which devices to resubmit for, because
			 * all others have had their bios[] entry cleared.
			 */
			int i;
			clear_bit(R1BIO_BarrierRetry, &r1_bio->state);
			clear_bit(R1BIO_Barrier, &r1_bio->state);
			for (i=0; i < conf->raid_disks; i++)
				if (r1_bio->bios[i]) {
					struct bio_vec *bvec;
					int j;

					bio = bio_clone(r1_bio->master_bio, GFP_NOIO);
					/* copy pages from the failed bio, as
					 * this might be a write-behind device */
					__bio_for_each_segment(bvec, bio, j, 0)
						bvec->bv_page = bio_iovec_idx(r1_bio->bios[i], j)->bv_page;
					bio_put(r1_bio->bios[i]);
					bio->bi_sector = r1_bio->sector +
						conf->mirrors[i].rdev->data_offset;
					bio->bi_bdev = conf->mirrors[i].rdev->bdev;
					bio->bi_end_io = raid1_end_write_request;
					bio->bi_rw = WRITE;
					bio->bi_private = r1_bio;
					r1_bio->bios[i] = bio;
					generic_make_request(bio);
				}
		} else {
			int disk;

			/* we got a read error. Maybe the drive is bad.  Maybe just
			 * the block and we can fix it.
			 * We freeze all other IO, and try reading the block from
			 * other devices.  When we find one, we re-write
			 * and check it that fixes the read error.
			 * This is all done synchronously while the array is
			 * frozen
			 */
			sector_t sect = r1_bio->sector;
			int sectors = r1_bio->sectors;
			freeze_array(conf);
			if (mddev->ro == 0) while(sectors) {
				int s = sectors;
				int d = r1_bio->read_disk;
				int success = 0;

				if (s > (PAGE_SIZE>>9))
					s = PAGE_SIZE >> 9;

				do {
					rdev = conf->mirrors[d].rdev;
					if (rdev &&
					    test_bit(In_sync, &rdev->flags) &&
					    sync_page_io(rdev->bdev,
							 sect + rdev->data_offset,
							 s<<9,
							 conf->tmppage, READ))
						success = 1;
					else {
						d++;
						if (d == conf->raid_disks)
							d = 0;
					}
				} while (!success && d != r1_bio->read_disk);

				if (success) {
					/* write it back and re-read */
					int start = d;
					while (d != r1_bio->read_disk) {
						if (d==0)
							d = conf->raid_disks;
						d--;
						rdev = conf->mirrors[d].rdev;
						atomic_add(s, &rdev->corrected_errors);
						if (rdev &&
						    test_bit(In_sync, &rdev->flags)) {
							if (sync_page_io(rdev->bdev,
									 sect + rdev->data_offset,
									 s<<9, conf->tmppage, WRITE) == 0)
								/* Well, this device is dead */
								md_error(mddev, rdev);
						}
					}
					d = start;
					while (d != r1_bio->read_disk) {
						if (d==0)
							d = conf->raid_disks;
						d--;
						rdev = conf->mirrors[d].rdev;
						if (rdev &&
						    test_bit(In_sync, &rdev->flags)) {
							if (sync_page_io(rdev->bdev,
									 sect + rdev->data_offset,
									 s<<9, conf->tmppage, READ) == 0)
								/* Well, this device is dead */
								md_error(mddev, rdev);
						}
					}
				} else {
					/* Cannot read from anywhere -- bye bye array */
					md_error(mddev, conf->mirrors[r1_bio->read_disk].rdev);
					break;
				}
				sectors -= s;
				sect += s;
			}

			unfreeze_array(conf);

			bio = r1_bio->bios[r1_bio->read_disk];
			if ((disk=read_balance(conf, r1_bio)) == -1) {
				printk(KERN_ALERT "raid1: %s: unrecoverable I/O"
				       " read error for block %llu\n",
				       bdevname(bio->bi_bdev,b),
				       (unsigned long long)r1_bio->sector);
				raid_end_bio_io(r1_bio);
			} else {
				r1_bio->bios[r1_bio->read_disk] =
					mddev->ro ? IO_BLOCKED : NULL;
				r1_bio->read_disk = disk;
				bio_put(bio);
				bio = bio_clone(r1_bio->master_bio, GFP_NOIO);
				r1_bio->bios[r1_bio->read_disk] = bio;
				rdev = conf->mirrors[disk].rdev;
				if (printk_ratelimit())
					printk(KERN_ERR "raid1: %s: redirecting sector %llu to"
					       " another mirror\n",
					       bdevname(rdev->bdev,b),
					       (unsigned long long)r1_bio->sector);
				bio->bi_sector = r1_bio->sector + rdev->data_offset;
				bio->bi_bdev = rdev->bdev;
				bio->bi_end_io = raid1_end_read_request;
				bio->bi_rw = READ;
				bio->bi_private = r1_bio;
				unplug = 1;
				generic_make_request(bio);
			}
		}
	}
	spin_unlock_irqrestore(&conf->device_lock, flags);
	if (unplug)
		unplug_slaves(mddev);
}


static int init_resync(conf_t *conf)
{
	int buffs;

	buffs = RESYNC_WINDOW / RESYNC_BLOCK_SIZE;
	if (conf->r1buf_pool)
		BUG();
	conf->r1buf_pool = mempool_create(buffs, r1buf_pool_alloc, r1buf_pool_free,
					  conf->poolinfo);
	if (!conf->r1buf_pool)
		return -ENOMEM;
	conf->next_resync = 0;
	return 0;
}

/*
 * perform a "sync" on one "block"
 *
 * We need to make sure that no normal I/O request - particularly write
 * requests - conflict with active sync requests.
 *
 * This is achieved by tracking pending requests and a 'barrier' concept
 * that can be installed to exclude normal IO requests.
 */

static sector_t sync_request(mddev_t *mddev, sector_t sector_nr, int *skipped, int go_faster)
{
	conf_t *conf = mddev_to_conf(mddev);
	r1bio_t *r1_bio;
	struct bio *bio;
	sector_t max_sector, nr_sectors;
	int disk = -1;
	int i;
	int wonly = -1;
	int write_targets = 0, read_targets = 0;
	int sync_blocks;
	int still_degraded = 0;

	if (!conf->r1buf_pool)
	{
/*
		printk("sync start - bitmap %p\n", mddev->bitmap);
*/
		if (init_resync(conf))
			return 0;
	}

	max_sector = mddev->size << 1;
	if (sector_nr >= max_sector) {
		/* If we aborted, we need to abort the
		 * sync on the 'current' bitmap chunk (there will
		 * only be one in raid1 resync.
		 * We can find the current addess in mddev->curr_resync
		 */
		if (mddev->curr_resync < max_sector) /* aborted */
			bitmap_end_sync(mddev->bitmap, mddev->curr_resync,
						&sync_blocks, 1);
		else /* completed sync */
			conf->fullsync = 0;

		bitmap_close_sync(mddev->bitmap);
		close_sync(conf);
		return 0;
	}

	/* before building a request, check if we can skip these blocks..
	 * This call the bitmap_start_sync doesn't actually record anything
	 */
	if (!bitmap_start_sync(mddev->bitmap, sector_nr, &sync_blocks, 1) &&
	    !conf->fullsync && !test_bit(MD_RECOVERY_REQUESTED, &mddev->recovery)) {
		/* We can skip this block, and probably several more */
		*skipped = 1;
		return sync_blocks;
	}
	/*
	 * If there is non-resync activity waiting for a turn,
	 * and resync is going fast enough,
	 * then let it though before starting on this new sync request.
	 */
	if (!go_faster && conf->nr_waiting)
		msleep_interruptible(1000);

	raise_barrier(conf);

	conf->next_resync = sector_nr;

	r1_bio = mempool_alloc(conf->r1buf_pool, GFP_NOIO);
	rcu_read_lock();
	/*
	 * If we get a correctably read error during resync or recovery,
	 * we might want to read from a different device.  So we
	 * flag all drives that could conceivably be read from for READ,
	 * and any others (which will be non-In_sync devices) for WRITE.
	 * If a read fails, we try reading from something else for which READ
	 * is OK.
	 */

	r1_bio->mddev = mddev;
	r1_bio->sector = sector_nr;
	r1_bio->state = 0;
	set_bit(R1BIO_IsSync, &r1_bio->state);

	for (i=0; i < conf->raid_disks; i++) {
		mdk_rdev_t *rdev;
		bio = r1_bio->bios[i];

		/* take from bio_init */
		bio->bi_next = NULL;
		bio->bi_flags |= 1 << BIO_UPTODATE;
		bio->bi_rw = 0;
		bio->bi_vcnt = 0;
		bio->bi_idx = 0;
		bio->bi_phys_segments = 0;
		bio->bi_hw_segments = 0;
		bio->bi_size = 0;
		bio->bi_end_io = NULL;
		bio->bi_private = NULL;

		rdev = rcu_dereference(conf->mirrors[i].rdev);
		if (rdev == NULL ||
			   test_bit(Faulty, &rdev->flags)) {
			still_degraded = 1;
			continue;
		} else if (!test_bit(In_sync, &rdev->flags)) {
			bio->bi_rw = WRITE;
			bio->bi_end_io = end_sync_write;
			write_targets ++;
		} else {
			/* may need to read from here */
			bio->bi_rw = READ;
			bio->bi_end_io = end_sync_read;
			if (test_bit(WriteMostly, &rdev->flags)) {
				if (wonly < 0)
					wonly = i;
			} else {
				if (disk < 0)
					disk = i;
			}
			read_targets++;
		}
		atomic_inc(&rdev->nr_pending);
		bio->bi_sector = sector_nr + rdev->data_offset;
		bio->bi_bdev = rdev->bdev;
		bio->bi_private = r1_bio;
	}
	rcu_read_unlock();
	if (disk < 0)
		disk = wonly;
	r1_bio->read_disk = disk;

	if (test_bit(MD_RECOVERY_SYNC, &mddev->recovery) && read_targets > 0)
		/* extra read targets are also write targets */
		write_targets += read_targets-1;

	if (write_targets == 0 || read_targets == 0) {
		/* There is nowhere to write, so all non-sync
		 * drives must be failed - so we are finished
		 */
		sector_t rv = max_sector - sector_nr;
		*skipped = 1;
		put_buf(r1_bio);
		return rv;
	}

	nr_sectors = 0;
	sync_blocks = 0;
	do {
		struct page *page;
		int len = PAGE_SIZE;
		if (sector_nr + (len>>9) > max_sector)
			len = (max_sector - sector_nr) << 9;
		if (len == 0)
			break;
		if (sync_blocks == 0) {
			if (!bitmap_start_sync(mddev->bitmap, sector_nr,
					       &sync_blocks, still_degraded) &&
			    !conf->fullsync &&
			    !test_bit(MD_RECOVERY_REQUESTED, &mddev->recovery))
				break;
			if (sync_blocks < (PAGE_SIZE>>9))
				BUG();
			if (len > (sync_blocks<<9))
				len = sync_blocks<<9;
		}

		for (i=0 ; i < conf->raid_disks; i++) {
			bio = r1_bio->bios[i];
			if (bio->bi_end_io) {
				page = bio->bi_io_vec[bio->bi_vcnt].bv_page;
				if (bio_add_page(bio, page, len, 0) == 0) {
					/* stop here */
					bio->bi_io_vec[bio->bi_vcnt].bv_page = page;
					while (i > 0) {
						i--;
						bio = r1_bio->bios[i];
						if (bio->bi_end_io==NULL)
							continue;
						/* remove last page from this bio */
						bio->bi_vcnt--;
						bio->bi_size -= len;
						bio->bi_flags &= ~(1<< BIO_SEG_VALID);
					}
					goto bio_full;
				}
			}
		}
		nr_sectors += len>>9;
		sector_nr += len>>9;
		sync_blocks -= (len>>9);
	} while (r1_bio->bios[disk]->bi_vcnt < RESYNC_PAGES);
 bio_full:
	r1_bio->sectors = nr_sectors;

	/* For a user-requested sync, we read all readable devices and do a
	 * compare
	 */
	if (test_bit(MD_RECOVERY_REQUESTED, &mddev->recovery)) {
		atomic_set(&r1_bio->remaining, read_targets);
		for (i=0; i<conf->raid_disks; i++) {
			bio = r1_bio->bios[i];
			if (bio->bi_end_io == end_sync_read) {
				md_sync_acct(conf->mirrors[i].rdev->bdev, nr_sectors);
				generic_make_request(bio);
			}
		}
	} else {
		atomic_set(&r1_bio->remaining, 1);
		bio = r1_bio->bios[r1_bio->read_disk];
		md_sync_acct(conf->mirrors[r1_bio->read_disk].rdev->bdev,
			     nr_sectors);
		generic_make_request(bio);

	}

	return nr_sectors;
}

static int run(mddev_t *mddev)
{
	conf_t *conf;
	int i, j, disk_idx;
	mirror_info_t *disk;
	mdk_rdev_t *rdev;
	struct list_head *tmp;

	if (mddev->level != 1) {
		printk("raid1: %s: raid level not set to mirroring (%d)\n",
		       mdname(mddev), mddev->level);
		goto out;
	}
	/*
	 * copy the already verified devices into our private RAID1
	 * bookkeeping area. [whatever we allocate in run(),
	 * should be freed in stop()]
	 */
	conf = kzalloc(sizeof(conf_t), GFP_KERNEL);
	mddev->private = conf;
	if (!conf)
		goto out_no_mem;

	conf->mirrors = kzalloc(sizeof(struct mirror_info)*mddev->raid_disks,
				 GFP_KERNEL);
	if (!conf->mirrors)
		goto out_no_mem;

	conf->tmppage = alloc_page(GFP_KERNEL);
	if (!conf->tmppage)
		goto out_no_mem;

	conf->poolinfo = kmalloc(sizeof(*conf->poolinfo), GFP_KERNEL);
	if (!conf->poolinfo)
		goto out_no_mem;
	conf->poolinfo->mddev = mddev;
	conf->poolinfo->raid_disks = mddev->raid_disks;
	conf->r1bio_pool = mempool_create(NR_RAID1_BIOS, r1bio_pool_alloc,
					  r1bio_pool_free,
					  conf->poolinfo);
	if (!conf->r1bio_pool)
		goto out_no_mem;

	ITERATE_RDEV(mddev, rdev, tmp) {
		disk_idx = rdev->raid_disk;
		if (disk_idx >= mddev->raid_disks
		    || disk_idx < 0)
			continue;
		disk = conf->mirrors + disk_idx;

		disk->rdev = rdev;

		blk_queue_stack_limits(mddev->queue,
				       rdev->bdev->bd_disk->queue);
		/* as we don't honour merge_bvec_fn, we must never risk
		 * violating it, so limit ->max_sector to one PAGE, as
		 * a one page request is never in violation.
		 */
		if (rdev->bdev->bd_disk->queue->merge_bvec_fn &&
		    mddev->queue->max_sectors > (PAGE_SIZE>>9))
			blk_queue_max_sectors(mddev->queue, PAGE_SIZE>>9);

		disk->head_position = 0;
		if (!test_bit(Faulty, &rdev->flags) && test_bit(In_sync, &rdev->flags))
			conf->working_disks++;
	}
	conf->raid_disks = mddev->raid_disks;
	conf->mddev = mddev;
	spin_lock_init(&conf->device_lock);
	INIT_LIST_HEAD(&conf->retry_list);
	if (conf->working_disks == 1)
		mddev->recovery_cp = MaxSector;

	spin_lock_init(&conf->resync_lock);
	init_waitqueue_head(&conf->wait_barrier);

	bio_list_init(&conf->pending_bio_list);
	bio_list_init(&conf->flushing_bio_list);

	if (!conf->working_disks) {
		printk(KERN_ERR "raid1: no operational mirrors for %s\n",
			mdname(mddev));
		goto out_free_conf;
	}

	mddev->degraded = 0;
	for (i = 0; i < conf->raid_disks; i++) {

		disk = conf->mirrors + i;

		if (!disk->rdev) {
			disk->head_position = 0;
			mddev->degraded++;
		}
	}

	/*
	 * find the first working one and use it as a starting point
	 * to read balancing.
	 */
	for (j = 0; j < conf->raid_disks &&
		     (!conf->mirrors[j].rdev ||
		      !test_bit(In_sync, &conf->mirrors[j].rdev->flags)) ; j++)
		/* nothing */;
	conf->last_used = j;


	mddev->thread = md_register_thread(raid1d, mddev, "%s_raid1");
	if (!mddev->thread) {
		printk(KERN_ERR
		       "raid1: couldn't allocate thread for %s\n",
		       mdname(mddev));
		goto out_free_conf;
	}

	printk(KERN_INFO 
		"raid1: raid set %s active with %d out of %d mirrors\n",
		mdname(mddev), mddev->raid_disks - mddev->degraded, 
		mddev->raid_disks);
	/*
	 * Ok, everything is just fine now
	 */
	mddev->array_size = mddev->size;

	mddev->queue->unplug_fn = raid1_unplug;
	mddev->queue->issue_flush_fn = raid1_issue_flush;

	return 0;

out_no_mem:
	printk(KERN_ERR "raid1: couldn't allocate memory for %s\n",
	       mdname(mddev));

out_free_conf:
	if (conf) {
		if (conf->r1bio_pool)
			mempool_destroy(conf->r1bio_pool);
		kfree(conf->mirrors);
		safe_put_page(conf->tmppage);
		kfree(conf->poolinfo);
		kfree(conf);
		mddev->private = NULL;
	}
out:
	return -EIO;
}

static int stop(mddev_t *mddev)
{
	conf_t *conf = mddev_to_conf(mddev);
	struct bitmap *bitmap = mddev->bitmap;
	int behind_wait = 0;

	/* wait for behind writes to complete */
	while (bitmap && atomic_read(&bitmap->behind_writes) > 0) {
		behind_wait++;
		printk(KERN_INFO "raid1: behind writes in progress on device %s, waiting to stop (%d)\n", mdname(mddev), behind_wait);
		set_current_state(TASK_UNINTERRUPTIBLE);
		schedule_timeout(HZ); /* wait a second */
		/* need to kick something here to make sure I/O goes? */
	}

	md_unregister_thread(mddev->thread);
	mddev->thread = NULL;
	blk_sync_queue(mddev->queue); /* the unplug fn references 'conf'*/
	if (conf->r1bio_pool)
		mempool_destroy(conf->r1bio_pool);
	kfree(conf->mirrors);
	kfree(conf->poolinfo);
	kfree(conf);
	mddev->private = NULL;
	return 0;
}

static int raid1_resize(mddev_t *mddev, sector_t sectors)
{
	/* no resync is happening, and there is enough space
	 * on all devices, so we can resize.
	 * We need to make sure resync covers any new space.
	 * If the array is shrinking we should possibly wait until
	 * any io in the removed space completes, but it hardly seems
	 * worth it.
	 */
	mddev->array_size = sectors>>1;
	set_capacity(mddev->gendisk, mddev->array_size << 1);
	mddev->changed = 1;
	if (mddev->array_size > mddev->size && mddev->recovery_cp == MaxSector) {
		mddev->recovery_cp = mddev->size << 1;
		set_bit(MD_RECOVERY_NEEDED, &mddev->recovery);
	}
	mddev->size = mddev->array_size;
	mddev->resync_max_sectors = sectors;
	return 0;
}

static int raid1_reshape(mddev_t *mddev, int raid_disks)
{
	/* We need to:
	 * 1/ resize the r1bio_pool
	 * 2/ resize conf->mirrors
	 *
	 * We allocate a new r1bio_pool if we can.
	 * Then raise a device barrier and wait until all IO stops.
	 * Then resize conf->mirrors and swap in the new r1bio pool.
	 *
	 * At the same time, we "pack" the devices so that all the missing
	 * devices have the higher raid_disk numbers.
	 */
	mempool_t *newpool, *oldpool;
	struct pool_info *newpoolinfo;
	mirror_info_t *newmirrors;
	conf_t *conf = mddev_to_conf(mddev);
	int cnt;

	int d, d2;

	if (raid_disks < conf->raid_disks) {
		cnt=0;
		for (d= 0; d < conf->raid_disks; d++)
			if (conf->mirrors[d].rdev)
				cnt++;
		if (cnt > raid_disks)
			return -EBUSY;
	}

	newpoolinfo = kmalloc(sizeof(*newpoolinfo), GFP_KERNEL);
	if (!newpoolinfo)
		return -ENOMEM;
	newpoolinfo->mddev = mddev;
	newpoolinfo->raid_disks = raid_disks;

	newpool = mempool_create(NR_RAID1_BIOS, r1bio_pool_alloc,
				 r1bio_pool_free, newpoolinfo);
	if (!newpool) {
		kfree(newpoolinfo);
		return -ENOMEM;
	}
	newmirrors = kzalloc(sizeof(struct mirror_info) * raid_disks, GFP_KERNEL);
	if (!newmirrors) {
		kfree(newpoolinfo);
		mempool_destroy(newpool);
		return -ENOMEM;
	}

	raise_barrier(conf);

	/* ok, everything is stopped */
	oldpool = conf->r1bio_pool;
	conf->r1bio_pool = newpool;

	for (d=d2=0; d < conf->raid_disks; d++)
		if (conf->mirrors[d].rdev) {
			conf->mirrors[d].rdev->raid_disk = d2;
			newmirrors[d2++].rdev = conf->mirrors[d].rdev;
		}
	kfree(conf->mirrors);
	conf->mirrors = newmirrors;
	kfree(conf->poolinfo);
	conf->poolinfo = newpoolinfo;

	mddev->degraded += (raid_disks - conf->raid_disks);
	conf->raid_disks = mddev->raid_disks = raid_disks;

	conf->last_used = 0; /* just make sure it is in-range */
	lower_barrier(conf);

	set_bit(MD_RECOVERY_NEEDED, &mddev->recovery);
	md_wakeup_thread(mddev->thread);

	mempool_destroy(oldpool);
	return 0;
}

static void raid1_quiesce(mddev_t *mddev, int state)
{
	conf_t *conf = mddev_to_conf(mddev);

	switch(state) {
	case 1:
		raise_barrier(conf);
		break;
	case 0:
		lower_barrier(conf);
		break;
	}
}


static struct mdk_personality raid1_personality =
{
	.name		= "raid1",
	.level		= 1,
	.owner		= THIS_MODULE,
	.make_request	= make_request,
	.run		= run,
	.stop		= stop,
	.status		= status,
	.error_handler	= error,
	.hot_add_disk	= raid1_add_disk,
	.hot_remove_disk= raid1_remove_disk,
	.spare_active	= raid1_spare_active,
	.sync_request	= sync_request,
	.resize		= raid1_resize,
	.reshape	= raid1_reshape,
	.quiesce	= raid1_quiesce,
};

static int __init raid_init(void)
{
	return register_md_personality(&raid1_personality);
}

static void raid_exit(void)
{
	unregister_md_personality(&raid1_personality);
}

module_init(raid_init);
module_exit(raid_exit);
MODULE_LICENSE("GPL");
MODULE_ALIAS("md-personality-3"); /* RAID1 */
MODULE_ALIAS("md-raid1");
MODULE_ALIAS("md-level-1");
