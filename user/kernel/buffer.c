/*
 * Buffer management
 */

#include "tux3.h"
#include "tux3_fork.h"

#ifndef trace
#define trace trace_on
#endif

/*
 * FIXME: Setting delta is not atomic with dirty for this buffer_head,
 */
#define BUFDELTA_AVAIL		1
#define BUFDELTA_BITS		order_base_2(BUFDELTA_AVAIL + TUX3_MAX_DELTA)
TUX3_DEFINE_STATE_FNS(unsigned long, buf, BUFDELTA_AVAIL, BUFDELTA_BITS,
		      BH_PrivateStart);

/*
 * FIXME: we should rewrite with own buffer management
 */

/*
 * FIXME: this is hack to save delta to linux buffer_head.
 * Inefficient, and this is not atomic with dirty bit change. And this
 * may not work on all arch (If set_bit() and cmpxchg() is not
 * exclusive, this has race).
 */
static void tux3_set_bufdelta(struct buffer_head *buffer, int delta)
{
	unsigned long state, old_state;

	delta = tux3_delta(delta);

	state = buffer->b_state;
	for (;;) {
		old_state = state;
		state = tux3_bufsta_update(old_state, delta);
		state = cmpxchg(&buffer->b_state, old_state, state);
		if (state == old_state)
			break;
	}
}

static void tux3_clear_bufdelta(struct buffer_head *buffer)
{
	unsigned long state, old_state;

	state = buffer->b_state;
	for (;;) {
		old_state = state;
		state = tux3_bufsta_clear(old_state);
		state = cmpxchg(&buffer->b_state, old_state, state);
		if (state == old_state)
			break;
	}
}

static int tux3_bufdelta(struct buffer_head *buffer)
{
	assert(buffer_dirty(buffer));
	while (1) {
		unsigned long state = buffer->b_state;
		if (tux3_bufsta_has_delta(state))
			return tux3_bufsta_get_delta(state);
		/* The delta is not yet set. Retry */
		cpu_relax();
	}
}

/* Can we modify buffer from delta */
int buffer_can_modify(struct buffer_head *buffer, unsigned delta)
{
	/* If true, buffer is still not stabilized. We can modify. */
	if (tux3_bufdelta(buffer) == tux3_delta(delta))
		return 1;
	/* The buffer may already be in stabilized stage for backend. */
	return 0;
}

/*
 * Caller must hold lock_page() or backend (otherwise, you may race
 * with buffer fork or clear dirty)
 */
void tux3_set_buffer_dirty_list(struct address_space *mapping,
				struct buffer_head *buffer, int delta,
				struct list_head *head)
{
	mark_buffer_dirty(buffer);

	if (!buffer->b_assoc_map) {
		spin_lock(&mapping->private_lock);
		BUG_ON(!list_empty(&buffer->b_assoc_buffers));
		list_move_tail(&buffer->b_assoc_buffers, head);
		buffer->b_assoc_map = mapping;
		/* FIXME: hack for save delta */
		tux3_set_bufdelta(buffer, delta);
		spin_unlock(&mapping->private_lock);
	}
}

void tux3_set_buffer_dirty(struct address_space *mapping,
			   struct buffer_head *buffer, int delta)
{
	struct list_head *head = tux3_dirty_buffers(mapping->host, delta);
	tux3_set_buffer_dirty_list(mapping, buffer, delta, head);
}

#define buffer_need_fork(b, d) (buffer_dirty(b) && !buffer_can_modify(b, d))

/*
 * Caller must hold lock_page() or backend (otherwise, you may race
 * with buffer fork or set dirty)
 */
void tux3_clear_buffer_dirty(struct buffer_head *buffer, unsigned delta)
{
	struct address_space *buffer_mapping = buffer->b_assoc_map;

	/* The buffer must not need to fork */
	assert(!buffer_need_fork(buffer, delta));

	if (buffer_mapping) {
		spin_lock(&buffer_mapping->private_lock);
		list_del_init(&buffer->b_assoc_buffers);
		buffer->b_assoc_map = NULL;
		tux3_clear_bufdelta(buffer);
		spin_unlock(&buffer_mapping->private_lock);

		clear_buffer_dirty(buffer);
	} else
		BUG_ON(!list_empty(&buffer->b_assoc_buffers));
}

/* This is called for the freeing block on volmap */
static void __blockput_free(struct sb *sb, struct buffer_head *buffer,
			    unsigned delta)
{
	/* FIXME: Untested. buffer was freed, so we would like to free cache */
	tux3_clear_buffer_dirty(buffer, delta);
	blockput(buffer);
}

void blockput_free(struct sb *sb, struct buffer_head *buffer)
{
	__blockput_free(sb, buffer, TUX3_INIT_DELTA);
}

void blockput_free_rollup(struct sb *sb, struct buffer_head *buffer)
{
	__blockput_free(sb, buffer, sb->rollup);
}

/* Copied from fs/buffer.c */
static void discard_buffer(struct buffer_head *buffer)
{
	/* FIXME: we need lock_buffer()? */
	lock_buffer(buffer);
	/*clear_buffer_dirty(buffer);*/
	buffer->b_bdev = NULL;
	clear_buffer_mapped(buffer);
	clear_buffer_req(buffer);
	clear_buffer_new(buffer);
	clear_buffer_delay(buffer);
	clear_buffer_unwritten(buffer);
	unlock_buffer(buffer);
}

/*
 * Invalidate buffer, this must be called from frontend like truncate.
 * Caller must hold lock_page(), and page->mapping must be valid.
 */
void tux3_invalidate_buffer(struct buffer_head *buffer)
{
	unsigned delta = tux3_inode_delta(buffer_inode(buffer));
	tux3_clear_buffer_dirty(buffer, delta);
	discard_buffer(buffer);
}

#include "buffer_writeback.c"
#include "buffer_fork.c"
