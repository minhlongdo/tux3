/*
 * Map logical file extents to physical disk.
 *
 * Copyright (c) 2008-2014 Daniel Phillips
 * Copyright (c) 2008-2014 OGAWA Hirofumi
 */

/*
 * Locking order: Take care about memory allocation. (It may call our fs.)
 *
 * down_write(itree: btree->lock) (alloc_inum, save_inode, purge_inode)
 * down_read(itree: btree->lock) (open_inode)
 *
 * down_write(otree: btree->lock) (tux3_unify_orphan_add,
 *				   tux3_unify_orphan_del,
 *				   load_otree_orphan)
 *
 * down_write(inode: btree->lock) (btree_chop, filemap for write)
 * down_read(inode: btree->lock) (filemap for read)
 *
 * inode->i_mutex
 *     mapping->private_lock (front uses to protect dirty buffer list)
 *     tuxnode->hole_extents_lock (for inode->hole_extents,
 *				   i_ddc->dirty_holes is protected by ->i_mutex)
 *
 *     inode->i_lock
 *         tuxnode->lock (to protect tuxnode data)
 *             tuxnode->dirty_inodes_lock (for i_ddc->dirty_inodes,
 *					   Note: timestamp can be updated
 *					   outside inode->i_mutex)
 *
 * sb->forked_buffers (for sb->forked_buffers)
 *
 * This lock may be first lock except vfs locks (lock_super, i_mutex).
 * sb->delta_lock (change_begin, change_end) [only for TUX3_FLUSHER_SYNC]
 *
 * memory allocation: (blockread, blockget, kmalloc, etc.)
 *     FIXME: fill here, what functions/locks are used via memory reclaim path
 *
 * So, to prevent reentering into our fs recursively by memory reclaim
 * from memory allocation, lower layer wouldn't use __GFP_FS.
 */

#include "tux3.h"
#include "dleaf.h"

#ifndef trace
#define trace trace_on
#endif

enum map_mode {
	MAP_READ	= 0,	/* filemap for read */
	MAP_WRITE	= 1,	/* filemap for overwrite */
	MAP_REDIRECT	= 2,	/* filemap for redirected write
				 * (copy-on-write) */
	MAX_MAP_MODE,
};

#include "filemap_hole.c"

/* userland only */
void show_segs(struct block_segment seg[], unsigned segs)
{
	__tux3_dbg("%i segs: ", segs);
	for (int i = 0; i < segs; i++)
		__tux3_dbg("%Lx/%i ", seg[i].block, seg[i].count);
	__tux3_dbg("\n");
}

static int map_bfree(struct inode *inode, block_t block, unsigned count)
{
	struct sb *sb = tux_sb(inode->i_sb);

	switch (tux_inode(inode)->inum) {
	case TUX_BITMAP_INO:
	case TUX_COUNTMAP_INO:
		log_bfree_on_unify(sb, block, count);
		defer_bfree(sb, &sb->deunify, block, count);
		break;
	default:
		log_bfree(sb, block, count);
		defer_bfree(sb, &sb->defree, block, count);
		break;
	}

	return 0;
}

static void seg_free(struct btree *btree, block_t block, unsigned count)
{
	map_bfree(btree_inode(btree), block, count);
}

/*
 * FIXME: Use balloc_find() and balloc_modify(). Use multiple segment
 * allocation
 */
static int seg_find(struct btree *btree, struct dleaf_req *rq,
		    int space, unsigned seg_len, unsigned *alloc_len)
{
	struct sb *sb = btree->sb;
	struct block_segment *seg = rq->seg + rq->seg_idx;
	int maxsegs = min(space, rq->seg_max - rq->seg_idx);
	unsigned len = seg_len;
	/* If overwrite mode, set SEG_NEW to allocated seg */
	const int seg_state = rq->overwrite ? BLOCK_SEG_NEW : 0;
	int err, i, segs;

	assert(rq->seg_idx == rq->seg_cnt);

	err = balloc_find(sb, seg, maxsegs, &segs, &len);
	if (err) {
		assert(err != -ENOSPC);	/* frontend reservation bug */
		return err;
	}
	for (i = 0; i < segs; i++)
		seg[i].state = seg_state;

	rq->seg_cnt = rq->seg_idx + segs;
	*alloc_len = seg_len - len;

	return 0;
}

/*
 * Callback to allocate blocks to ->seg. dleaf is doing to write segs,
 * now we have to assign physical address to segs.
 */
static int seg_alloc(struct btree *btree, struct dleaf_req *rq, int new_cnt)
{
	struct sb *sb = btree->sb;
	struct block_segment *seg = rq->seg + rq->seg_idx;
	struct block_segment *limit = rq->seg + rq->seg_cnt;
	int err;

	if (new_cnt) {
		err = balloc_use(sb, seg, new_cnt);
		if (err)
			return err;	/* FIXME: error handling */

		while (new_cnt) {
			log_balloc(sb, seg->block, seg->count);
			new_cnt--;
			seg++;
		}
	}
	rq->seg_cnt -= limit - seg;

	/* FIXME: tell unused seg[] to balloc for reusing seg[] later */
	/* balloc_cache(sb, seg, limit - seg); */

	return 0;
}

static int seg_alloc_one(struct btree *btree, block_t start, unsigned count,
			 struct block_segment seg[1])
{
	struct sb *sb = btree->sb;
	unsigned need = count;
	int err, segs = 0;

	err = balloc_find(sb, seg, 1, &segs, &need);
	if (err)
		return err;
	if (need) {
		/* FIXME: tell unused seg[] to balloc for reusing seg[] later */
		/* balloc_cache(sb, seg, segs); */
		return 0;
	}
	assert(segs == 1);

	err = balloc_use(sb, seg, 1);
	if (err)
		return err;
	return segs;
}

int dtree_chop(struct btree *btree, tuxkey_t start, u64 len)
{
	if (has_direct_extent(btree)) {
		block_t block = btree->root.block;
		unsigned count = btree->root.count;
		/* FIXME: does not support hole_punch yet. */
		assert(len == TUXKEY_LIMIT);
		if (start < count) {
			if (start == 0)
				btree->root = no_root;
			else
				btree->root.count = start;
			tux3_mark_btree_dirty(btree);
			seg_free(btree, block + start, count - start);
		}
		return 0;
	}

	return btree_chop(btree, start, len);
}

/*
 * Map logical extent to physical extent in direct extent.
 *
 * return value:
 * < 0 - error
 *   0 - fallback to btree
 * 0 < - number of mapped seg[]
 */
static int map_direct(struct btree *btree, block_t start, unsigned count,
		      struct block_segment seg[], unsigned seg_max,
		      enum map_mode mode)
{
	int segs = 0;

	if (mode == MAP_READ) {
		/*
		 * Map direct extent. If mapping is only hole,
		 * btree stuff handles it.
		 */
		if (start < btree->root.count) {
			unsigned direct_count = btree->root.count;
			seg[0] = (struct block_segment){
				.block = btree->root.block + start,
				.count = count,
			};
			segs++;

			if (start + count > direct_count) {
				seg[0].count = direct_count - start;
				if (seg_max <= segs)
					return segs;

				/* Fill hole */
				seg[segs++] = (struct block_segment){
					.block = 0,
					.count = start + count - direct_count,
					.state = BLOCK_SEG_HOLE,
				};
			}
		}
	} else {
		/* FIXME: overwrite is not implemented yet */
		if (mode == MAP_WRITE)
			return 0;
		/* This doesn't fit to direct extent */
		if (start != 0 || count > MAX_DIRECT_COUNT)
			return 0;
		/* Rewriting existing extent partially */
		if (count < btree->root.count)
			return 0;

		segs = seg_alloc_one(btree, start, count, seg);
		if (segs <= 0)
			return segs;
		log_balloc(btree->sb, seg->block, seg->count);

		if (btree->root.count)
			seg_free(btree, btree->root.block, btree->root.count);

		btree->root = (struct root){
			.direct = 1,
			.count = seg->count,
			.block = seg->block,
		};
		tux3_mark_btree_dirty(btree);
	}

	return segs;
}

/* filemap() by using dleaf */
static int filemap2(struct inode *inode, block_t start, unsigned count,
		       struct block_segment seg[], unsigned seg_max,
		       enum map_mode mode)
{
	struct btree *btree = &tux_inode(inode)->btree;
	struct cursor *cursor = NULL;
	int err, segs = 0;

	assert(seg_max > 0);

	/*
	 * bitmap enters here recursively.
	 *
	 * tux3_flush_inode_internal() (flush bitmap)
	 *   flush_list()
	 *     filemap() (for flush)
	 *       balloc()
	 *         read bitmap
	 *           filemap() (for read)
	 *
	 * But bitmap is used (read/write) only from backend.
	 * So, no need to lock.
	 */
	if (tux_inode(inode)->inum != TUX_BITMAP_INO) {
		if (mode == MAP_READ)
			down_read(&btree->lock);
		else
			down_write(&btree->lock);
	}

	if (!has_root(btree)) {
		/* If there is no btree root, try direct extent */
		segs = map_direct(btree, start, count, seg, seg_max, mode);
		if (segs)
			goto out_unlock;

		/*
		 * FIXME: this should be merged to insert_leaf() or something?
		 */
		if (mode != MAP_READ) {
			err = btree_alloc_empty(btree);
			if (err) {
				segs = err;
				goto out_unlock;
			}
		}
	}
	if (has_root(btree)) {
		cursor = alloc_cursor(btree, 1); /* allows for depth increase */
		if (!cursor) {
			segs = -ENOMEM;
			goto out_unlock;
		}

		err = btree_probe(cursor, start);
		if (err) {
			segs = err;
			goto out_unlock;
		}
	}

	if (mode == MAP_READ) {
		if (has_root(btree)) {
			struct dleaf_req rq = {
				.key = {
					.start	= start,
					.len	= count,
				},
				.seg_max	= seg_max,
				.seg		= seg,
			};

			/* Read extents from data btree */
			err = btree_read(cursor, &rq.key);
			if (err) {
				segs = err;
				goto out_unlock;
			}
			segs = rq.seg_cnt;
			/*
			 * Read might be partial. (due to seg_max, or FIXME:
			 * lack of read for multiple leaves)
			 */
		} else {
			/* btree doesn't have root yet */
			segs = 1;
			seg[0].block = 0;
			seg[0].count = count;
			seg[0].state = BLOCK_SEG_HOLE;
		}
		assert(segs);
	} else {
		/* Write extents from data btree */
		struct dleaf_req rq = {
			.key = {
				.start	= start,
				.len	= count,
			},
			.seg_max	= seg_max,
			.seg		= seg,
			.overwrite	= mode != MAP_REDIRECT,
			.seg_find	= seg_find,
			.seg_alloc	= seg_alloc,
			.seg_free	= seg_free,
		};
		err = btree_write(cursor, &rq.key);
		if (err)
			segs = err;
		else
			segs = rq.seg_cnt;
	}

	if (cursor)
		release_cursor(cursor);
out_unlock:
	if (tux_inode(inode)->inum != TUX_BITMAP_INO) {
		if (mode == MAP_READ)
			up_read(&btree->lock);
		else
			up_write(&btree->lock);
	}
	if (cursor)
		free_cursor(cursor);

	return segs;
}

/*
 * Map logical extent to physical extent
 *
 * return value:
 * < 0 - error
 * 0 < - number of physical extents which were mapped
 */
static int filemap(struct inode *inode, block_t start, unsigned count,
		      struct block_segment seg[], unsigned seg_max,
		      enum map_mode mode)
{
	int segs;

	/*
	 * NOTE: hole extents are not protected by i_mutex on MAP_READ
	 * path. So, we shouldn't assume it is stable.
	 */

	if (mode == MAP_READ) {
		/* If whole region was hole, don't need to call filemap */
		if (tux3_is_hole(inode, start, count)) {
			assert(seg_max >= 1);
			seg[0].state = BLOCK_SEG_HOLE;
			seg[0].block = 0;
			seg[0].count = count;
			return 1;
		}
	}

	segs = filemap2(inode, start, count, seg, seg_max, mode);

	if (mode == MAP_READ) {
		/* Update seg[] with hole information */
		segs = tux3_map_hole(inode, start, count, seg, segs, seg_max);
	}

	return segs;
}

static int filemap_extent_io(enum map_mode mode, int rw, struct bufvec *bufvec);
int tux3_filemap_overwrite_io(int rw, struct bufvec *bufvec)
{
	enum map_mode mode = (rw & WRITE) ? MAP_WRITE : MAP_READ;
	return filemap_extent_io(mode, rw, bufvec);
}

int tux3_filemap_redirect_io(int rw, struct bufvec *bufvec)
{
	enum map_mode mode = (rw & WRITE) ? MAP_REDIRECT : MAP_READ;
	return filemap_extent_io(mode, rw, bufvec);
}

#ifdef __KERNEL__
#include <linux/mpage.h>
#include <linux/swap.h>		/* for mark_page_accessed() */
#include <linux/aio.h>		/* for kiocb */

static int filemap_extent_io(enum map_mode mode, int rw, struct bufvec *bufvec)
{
	struct inode *inode = bufvec_inode(bufvec);
	block_t block, index = bufvec_contig_index(bufvec);
	unsigned count = bufvec_contig_count(bufvec);
	int err;
	struct block_segment seg[10];

	/* FIXME: For now, this is only for write */
	assert(mode != MAP_READ);

	int segs = filemap(inode, index, count, seg, ARRAY_SIZE(seg), mode);
	if (segs < 0)
		return segs;
	assert(segs);

	for (int i = 0; i < segs; i++) {
		block = seg[i].block;
		count = seg[i].count;

		trace("extent 0x%Lx/%x => %Lx", index, count, block);

		err = blockio_vec(rw, bufvec, block, count);
		if (err)
			break;

		index += count;
	}

	return err;
}

static void seg_to_buffer(struct sb *sb, struct buffer_head *buffer,
			  struct block_segment *seg, int delalloc)
{
	switch (seg->state) {
	case BLOCK_SEG_HOLE:
		if (delalloc && !buffer_delay(buffer)) {
			map_bh(buffer, vfs_sb(sb), 0);
			set_buffer_new(buffer);
			set_buffer_delay(buffer);
			buffer->b_size = seg->count << sb->blockbits;
		}
		break;
	case BLOCK_SEG_NEW:
		assert(!delalloc);
		assert(seg->block);
		if (buffer_delay(buffer)) {
			/* for now, block_write_full_page() clear delay */
//			clear_buffer_delay(buffer);
			buffer->b_blocknr = seg->block;
			/*
			 * FIXME: do we need to unmap_underlying_metadata()
			 * for sb->volmap? (at least, check buffer state?)
			 * And if needed, is it enough?
			 */
			break;
		}
		set_buffer_new(buffer);
		/* FALLTHRU */
	default:
		map_bh(buffer, vfs_sb(sb), seg->block);
		buffer->b_size = seg->count << sb->blockbits;
		break;
	}
}

/* create modes: 0 - read, 1 - write, 2 - redirect, 3 - delalloc */
static int __tux3_get_block(struct inode *inode, sector_t iblock,
			    struct buffer_head *bh_result, int create)
{
	struct sb *sb = tux_sb(inode->i_sb);
	size_t max_blocks = bh_result->b_size >> sb->blockbits;
	enum map_mode mode;
	struct block_segment seg;
	int segs, delalloc;

	trace("==> inum %Lu, iblock %Lu, b_size %zu, create %d",
	      tux_inode(inode)->inum, (u64)iblock, bh_result->b_size, create);

	if (create == 3) {
		delalloc = 1;
		mode = MAP_READ;
	} else {
		delalloc = 0;
		mode = create;
	}
	assert(mode < MAX_MAP_MODE);

	segs = filemap(inode, iblock, max_blocks, &seg, 1, mode);
	if (segs < 0) {
		tux3_err(sb, "filemap failed: %d", segs);
		return -EIO;
	}
	assert(segs == 1);
	assert(seg.count <= max_blocks);
#if 1
	/*
	 * We doesn't use get_block() on write path in atomic-commit,
	 * so SEG_NEW never happen.  (FIXME: Current direct I/O
	 * implementation is using this path.)
	 */
	assert(seg.state != BLOCK_SEG_NEW /*|| (create && !delalloc) */);
#endif

	seg_to_buffer(sb, bh_result, &seg, delalloc);

	trace("<== inum %Lu, mapped %d, block %Lu, size %zu",
	      tux_inode(inode)->inum, buffer_mapped(bh_result),
	      (u64)bh_result->b_blocknr, bh_result->b_size);

	return 0;
}

/* Prepare buffer state for ->write_begin() to use as delalloc */
static int tux3_da_get_block(struct inode *inode, sector_t iblock,
			     struct buffer_head *bh_result, int create)
{
	/* FIXME: We should reserve the space */

	/* buffer should not be mapped */
	assert(!buffer_mapped(bh_result));
	/* If page is uptodate, buffer should be uptodate too */
	assert(!PageUptodate(bh_result->b_page) || buffer_uptodate(bh_result));

	/*
	 * If buffer is uptodate, we don't need physical address to
	 * read block. So, we don't need to find current physical
	 * address, just setup as SEG_HOLE for delalloc.
	 */
	if (buffer_uptodate(bh_result)) {
		struct sb *sb = tux_sb(inode->i_sb);
		static struct block_segment seg = {
			.state = BLOCK_SEG_HOLE,
			.block = 0,
			.count = 1,
		};
		assert(bh_result->b_size == sb->blocksize);

		seg_to_buffer(sb, bh_result, &seg, 1);

		trace("inum %Lu, mapped %d, block %Lu, size %zu",
		      tux_inode(inode)->inum, buffer_mapped(bh_result),
		      (u64)bh_result->b_blocknr, bh_result->b_size);

		return 0;
	}

	return __tux3_get_block(inode, iblock, bh_result, 3);
}

int tux3_get_block(struct inode *inode, sector_t iblock,
		   struct buffer_head *bh_result, int create)
{
	return __tux3_get_block(inode, iblock, bh_result, create);
}

struct buffer_head *__get_buffer(struct page *page, int offset)
{
	struct buffer_head *buffer = page_buffers(page);
	while (offset--)
		buffer = buffer->b_this_page;
	return buffer;
}

static struct buffer_head *get_buffer(struct page *page, int offset)
{
	struct buffer_head *buffer = __get_buffer(page, offset);
	get_bh(buffer);
	return buffer;
}

static struct buffer_head *__find_get_buffer(struct address_space *mapping,
					     pgoff_t index, int offset,
					     int need_uptodate)
{
	struct buffer_head *bh = NULL;
	struct page *page;

	page = find_get_page(mapping, index);
	if (page) {
		if (!need_uptodate || PageUptodate(page)) {
			spin_lock(&mapping->private_lock);
			if (page_has_buffers(page)) {
				bh = get_buffer(page, offset);
				assert(!need_uptodate || buffer_uptodate(bh));
			}
			spin_unlock(&mapping->private_lock);
		}
		page_cache_release(page);
	}
	return bh;
}

static struct buffer_head *find_get_buffer(struct address_space *mapping,
					   pgoff_t index, int offset)
{
	return __find_get_buffer(mapping, index, offset, 1);
}

struct buffer_head *peekblk(struct address_space *mapping, block_t iblock)
{
	struct inode *inode = mapping->host;
	pgoff_t index;
	int offset;

	index = iblock >> (PAGE_CACHE_SHIFT - inode->i_blkbits);
	offset = iblock & ((1 << (PAGE_CACHE_SHIFT - inode->i_blkbits)) - 1);

	return __find_get_buffer(mapping, index, offset, 0);
}

struct buffer_head *blockread(struct address_space *mapping, block_t iblock)
{
	struct inode *inode = mapping->host;
	gfp_t gfp_mask = mapping_gfp_mask(mapping) | __GFP_COLD; /* FIXME(?) */
	pgoff_t index;
	struct page *page;
	struct buffer_head *bh;
	int err, offset;

	index = iblock >> (PAGE_CACHE_SHIFT - inode->i_blkbits);
	offset = iblock & ((1 << (PAGE_CACHE_SHIFT - inode->i_blkbits)) - 1);

	bh = find_get_buffer(mapping, index, offset);
	if (bh)
		goto out;

	err = -ENOMEM;
	/* FIXME: don't need to find again. Just try to allocate and insert */
	page = find_or_create_page(mapping, index, gfp_mask);
	if (!page)
		goto error;

	if (!page_has_buffers(page))
		create_empty_buffers(page, tux_sb(inode->i_sb)->blocksize, 0);
	bh = get_buffer(page, offset);

	if (PageUptodate(page))
		unlock_page(page);
	else {
		err = mapping->a_ops->readpage(NULL, page);
		if (err)
			goto error_readpage;
		wait_on_page_locked(page);
		if (!PageUptodate(page)) {
			err = -EIO;
			goto error_readpage;
		}
	}
	page_cache_release(page);
	assert(buffer_uptodate(bh));

out:
	touch_buffer(bh);

	return bh;

error_readpage:
	put_bh(bh);
	page_cache_release(page);
error:
	return NULL;
}

struct buffer_head *blockget(struct address_space *mapping, block_t iblock)
{
	struct inode *inode = mapping->host;
	pgoff_t index;
	struct page *page;
	struct buffer_head *bh;
	void *fsdata;
	int err, offset;
	unsigned aop_flags = AOP_FLAG_UNINTERRUPTIBLE;

	index = iblock >> (PAGE_CACHE_SHIFT - inode->i_blkbits);
	offset = iblock & ((1 << (PAGE_CACHE_SHIFT - inode->i_blkbits)) - 1);

	/* Prevent reentering into our fs recursively by memory allocation. */
	if (!(mapping_gfp_mask(mapping) & __GFP_FS))
		aop_flags |= AOP_FLAG_NOFS;

	err = mapping->a_ops->write_begin(NULL, mapping,
					  iblock << inode->i_blkbits,
					  1 << inode->i_blkbits,
					  aop_flags, &page, &fsdata);
	if (err)
		return NULL;

	assert(page_has_buffers(page));

	bh = get_buffer(page, offset);
	/* Clear new, so the caller must initialize buffer. */
	clear_buffer_new(bh);
	/*
	 * FIXME: now all read is using ->readpage(), this means it
	 * reads whole page with lock_page(), i.e. read non-target
	 * block.  So, we have to hold to modify data to prevent race
	 * with ->readpage(). But we are not holding lock_page().
	 *
	 *          cpu0                            cpu1
	 *					bufferA = blockget()
	 *					modify data
	 *     blockread(bufferC)
	 *       readpage()
	 *         read bufferA <= lost modify
	 *         set_buffer_uptodate()
	 *         read bufferC
	 *         set_buffer_uptodate()
	 *                                      set_buffer_uptodate()
	 *
	 * So, this set uptodate before unlock_page. But, we should
	 * use submit_bh() or similar to read block, instead.
	 *
	 * FIXME: another issue of blockread/blockget(). If those
	 * functions was used for volmap, we might read blocks nearby
	 * the target block. But nearby blocks can be allocated for
	 * data pages, furthermore nearby blocks can be in-flight I/O.
	 *
	 * So, nearby blocks on volmap can be non-volmap blocks, and
	 * it would just increase amount of I/O size, and seeks.
	 *
	 * Like above said, we should use submit_bh() or similar.
	 */
	set_buffer_uptodate(bh);

	unlock_page(page);
	page_cache_release(page);

	touch_buffer(bh);

	return bh;
}

static int tux3_readpage(struct file *file, struct page *page)
{
	int err = mpage_readpage(page, tux3_get_block);
	assert(!PageForked(page));	/* FIXME: handle forked page */
	return err;
}

static int tux3_readpages(struct file *file, struct address_space *mapping,
			  struct list_head *pages, unsigned nr_pages)
{
	return mpage_readpages(mapping, pages, nr_pages, tux3_get_block);
}

#include "filemap_blocklib.c"

static void tux3_write_failed(struct address_space *mapping, loff_t to)
{
	struct inode *inode = mapping->host;

	if (to > inode->i_size) {
		/*
		 * write_{begin,end}() is protected by change_{begin,end},
		 * so there is no new blocks here on this page.
		 * No need to adjust the dtree.
		 *
		 * FIXME: right?
		 */
		truncate_pagecache(inode, inode->i_size);
	}
}

/* Use delalloc and check buffer fork. */
static int __tux3_file_write_begin(struct file *file,
				   struct address_space *mapping,
				   loff_t pos, unsigned len, unsigned flags,
				   struct page **pagep, void **fsdata,
				   int tux3_flags)
{
	int ret;

	ret = tux3_write_begin(mapping, pos, len, flags, pagep,
			       tux3_da_get_block, tux3_flags);
	if (ret < 0)
		tux3_write_failed(mapping, pos + len);
	return ret;
}

static int __tux3_file_write_end(struct file *file,
				 struct address_space *mapping,
				 loff_t pos, unsigned len, unsigned copied,
				 struct page *page, void *fsdata)
{
	int ret;

	ret = tux3_write_end(file, mapping, pos, len, copied, page, fsdata);
	if (ret < len)
		tux3_write_failed(mapping, pos + len);
	return ret;
}

/* Separate big write transaction to page chunk */
static int tux3_file_write_begin(struct file *file,
				 struct address_space *mapping,
				 loff_t pos, unsigned len, unsigned flags,
				 struct page **pagep, void **fsdata)
{
	return __tux3_file_write_begin(file, mapping, pos, len, flags, pagep,
				       fsdata,
				       TUX3_F_PAGEFORK | TUX3_F_SEP_DELTA);
}

static int tux3_file_write_end(struct file *file, struct address_space *mapping,
			       loff_t pos, unsigned len, unsigned copied,
			       struct page *page, void *fsdata)
{
	int ret;

	ret = __tux3_file_write_end(file, mapping, pos, len, copied, page,
				    fsdata);

	/* Separate big write transaction to small chunk. */
	assert(S_ISREG(mapping->host->i_mode));
	change_end_if_needed(tux_sb(mapping->host->i_sb));

	return ret;
}

#if 0 /* disabled writeback for now */
static int tux3_writepage(struct page *page, struct writeback_control *wbc)
{
	struct sb *sb = tux_sb(page->mapping->host->i_sb);
	change_begin(sb);
	int err = block_write_full_page(page, tux3_get_block, wbc);
	change_end(sb);
	return err;
}
#endif
#if 0
/* mpage_writepages() uses dummy bh, so we can't check buffer_delay. */
static int tux3_writepages(struct address_space *mapping,
			   struct writeback_control *wbc)
{
	return mpage_writepages(mapping, wbc, tux3_get_block);
}
#endif

static int tux3_disable_writepage(struct page *page,
				  struct writeback_control *wbc)
{
	/*
	 * FIXME: disable writeback for now. We would have to handle
	 * writeback for sync (e.g. by cache pressure).
	 * FIXME: we should use AOP_WRITEPAGE_ACTIVATE if for_reclaim?
	 * Or just set .writepage = NULL to page keep dirty and active?
	 */
	trace("writepage disabled for now (%d)", wbc->sync_mode);
	redirty_page_for_writepage(wbc, page);
#if 0
	if (wbc->for_reclaim)
		return AOP_WRITEPAGE_ACTIVATE;	/* Return with page locked */
#endif
	unlock_page(page);
	return 0;
}

static int tux3_disable_writepages(struct address_space *mapping,
				   struct writeback_control *wbc)
{
	/*
	 * FIXME: disable writeback for now. We would have to handle
	 * writeback for sync (e.g. by cache pressure)
	 */
	trace("writepages disabled for now (%d)", wbc->sync_mode);
	return 0;
}

#ifdef TUX3_DIRECT_IO
/*
 * Direct I/O is unsupport for now. Since this is for
 * non-atomic-commit mode, so this allocates blocks from frontend.
 */
static ssize_t tux3_direct_IO(int rw, struct kiocb *iocb,
			      const struct iovec *iov,
			      loff_t offset, unsigned long nr_segs)
{
	struct file *file = iocb->ki_filp;
	struct address_space *mapping = file->f_mapping;
	struct inode *inode = mapping->host;
	ssize_t ret;

	ret = blockdev_direct_IO(rw, iocb, inode, iov, offset, nr_segs,
				 tux3_get_block);
	if (ret < 0 && (rw & WRITE))
		tux3_write_failed(mapping, offset + iov_length(iov, nr_segs));
	return ret;
}
#endif

static sector_t tux3_bmap(struct address_space *mapping, sector_t iblock)
{
	sector_t blocknr;

	mutex_lock(&mapping->host->i_mutex);
	blocknr = generic_block_bmap(mapping, iblock, tux3_get_block);
	mutex_unlock(&mapping->host->i_mutex);

	return blocknr;
}

#include "filemap_mmap.c"

const struct address_space_operations tux_file_aops = {
	.readpage		= tux3_readpage,
	.readpages		= tux3_readpages,
//	.writepage		= tux3_writepage,
//	.writepages		= tux3_writepages,
	.writepage		= tux3_disable_writepage,
	.writepages		= tux3_disable_writepages,
	.write_begin		= tux3_file_write_begin,
	.write_end		= tux3_file_write_end,
	.bmap			= tux3_bmap,
	.set_page_dirty		= tux3_set_page_dirty_assert,
	.invalidatepage		= tux3_invalidatepage,
//	.releasepage		= ext4_releasepage,
#ifdef TUX3_DIRECT_IO
	.direct_IO		= tux3_direct_IO,
#endif
//	.migratepage		= buffer_migrate_page,	/* FIXME */
//	.is_partially_uptodate	= block_is_partially_uptodate,
//	.is_dirty_writeback	= buffer_check_dirty_writeback,
};

static int tux3_symlink_write_begin(struct file *file,
				    struct address_space *mapping,
				    loff_t pos, unsigned len, unsigned flags,
				    struct page **pagep, void **fsdata)
{
	return __tux3_file_write_begin(file, mapping, pos, len, flags, pagep,
				       fsdata, TUX3_F_PAGEFORK);
}

/* Copy of tux_file_aops, except ->write_begin/end */
const struct address_space_operations tux_symlink_aops = {
	.readpage		= tux3_readpage,
	.readpages		= tux3_readpages,
//	.writepage		= tux3_writepage,
//	.writepages		= tux3_writepages,
	.writepage		= tux3_disable_writepage,
	.writepages		= tux3_disable_writepages,
	.write_begin		= tux3_symlink_write_begin,
	.write_end		= __tux3_file_write_end,
	.bmap			= tux3_bmap,
	.set_page_dirty		= tux3_set_page_dirty_bug,
	.invalidatepage		= tux3_invalidatepage,
//	.releasepage		= ext4_releasepage,
#ifdef TUX3_DIRECT_IO
	.direct_IO		= tux3_direct_IO,
#endif
//	.migratepage		= buffer_migrate_page,	/* FIXME */
//	.is_partially_uptodate	= block_is_partially_uptodate,
//	.is_dirty_writeback	= buffer_check_dirty_writeback,
};

static int tux3_blk_readpage(struct file *file, struct page *page)
{
	int err = block_read_full_page(page, tux3_get_block);
	assert(!PageForked(page));	/* FIXME: handle forked page */
	return err;
}

/* Use delalloc and doesn't check buffer fork */
static int tux3_blk_write_begin(struct file *file,
				struct address_space *mapping,
				loff_t pos, unsigned len, unsigned flags,
				struct page **pagep, void **fsdata)
{
	return __tux3_file_write_begin(file, mapping, pos, len, flags, pagep,
				       fsdata, 0);
}

#if 0 /* disabled writeback for now */
static int tux3_blk_writepage(struct page *page, struct writeback_control *wbc)
{
	return block_write_full_page(page, tux3_get_block, wbc);
}
#endif

const struct address_space_operations tux_blk_aops = {
	.readpage		= tux3_blk_readpage,
//	.writepage		= tux3_blk_writepage,
//	.writepages		= tux3_writepages,
	.writepage		= tux3_disable_writepage,
	.writepages		= tux3_disable_writepages,
	.write_begin		= tux3_blk_write_begin,
	.bmap			= tux3_bmap,
	.set_page_dirty		= tux3_set_page_dirty_bug,
	.invalidatepage		= tux3_invalidatepage,
//	.migratepage		= buffer_migrate_page,		/* FIXME */
//	.is_partially_uptodate	= block_is_partially_uptodate,
//	.is_dirty_writeback	= buffer_check_dirty_writeback,
};

static int tux3_vol_get_block(struct inode *inode, sector_t iblock,
			      struct buffer_head *bh_result, int create)
{
	if (iblock >= tux_sb(inode->i_sb)->volblocks) {
		assert(!create);
		return 0;
	}
	map_bh(bh_result, inode->i_sb, iblock);
	return 0;
}

static int tux3_vol_readpage(struct file *file, struct page *page)
{
	return block_read_full_page(page, tux3_vol_get_block);
}

#if 0 /* disabled writeback for now */
static int tux3_vol_writepage(struct page *page, struct writeback_control *wbc)
{
	return block_write_full_page(page, tux3_vol_get_block, wbc);
}
#endif

/* Use tux3_vol_get_block() (physical map) and doesn't check buffer fork */
static int tux3_vol_write_begin(struct file *file,
				struct address_space *mapping,
				loff_t pos, unsigned len, unsigned flags,
				struct page **pagep, void **fsdata)
{
	return tux3_write_begin(mapping, pos, len, flags, pagep,
				tux3_vol_get_block, 0);
}

const struct address_space_operations tux_vol_aops = {
	.readpage		= tux3_vol_readpage,
//	.writepage		= tux3_vol_writepage,
	.writepage		= tux3_disable_writepage,
	.writepages		= tux3_disable_writepages,
	.write_begin		= tux3_vol_write_begin,
	.set_page_dirty		= tux3_set_page_dirty_bug,
	.invalidatepage		= tux3_invalidatepage,
//	.is_partially_uptodate  = block_is_partially_uptodate,
//	.is_dirty_writeback	= buffer_check_dirty_writeback,
};
#endif /* __KERNEL__ */
