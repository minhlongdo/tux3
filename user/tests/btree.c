/*
 * Generic btree operations
 *
 * Original copyright (c) 2008 Daniel Phillips <phillips@phunq.net>
 * Portions copyright (c) 2006-2008 Google Inc.
 * Licensed under the GPL version 3
 *
 * By contributing changes to this file you grant the original copyright holder
 * the right to distribute those changes under any license.
 */

#include "tux3user.h"
#include "test.h"

#ifndef trace
#define trace trace_off
#endif

#include "balloc-dummy.c"
#include "kernel/btree.c"

static void clean_main(struct sb *sb, struct inode *inode)
{
	log_finish(sb);
	log_finish_cycle(sb, 1);
	free_map(inode->map);
	destroy_defer_bfree(&sb->deunify);
	destroy_defer_bfree(&sb->defree);
	tux3_clear_dirty_inode(sb->logmap);
	invalidate_buffers(sb->volmap->map);
	tux3_clear_dirty_inode(sb->volmap);
	put_super(sb);
	tux3_exit_mem();
}

struct uleaf { u32 magic, count; struct uentry { u16 key, val; } entries[]; };

struct uleaf_req {
	struct btree_key_range key;	/* key and count */
	u16 val;
};

static void uleaf_btree_init(struct btree *btree)
{
	struct sb *sb = btree->sb;
	btree->entries_per_leaf = (sb->blocksize - offsetof(struct uleaf, entries)) / sizeof(struct uentry);
}

static int uleaf_init(struct btree *btree, void *leaf)
{
	struct uleaf *uleaf = leaf;
	*uleaf = (struct uleaf){ .magic = 0xc0de };
	return 0;
}

static unsigned uleaf_free(struct btree *btree, void *leaf)
{
	struct uleaf *uleaf = leaf;
	return btree->entries_per_leaf - uleaf->count;
}

static int uleaf_sniff(struct btree *btree, void *leaf)
{
	struct uleaf *uleaf = leaf;
	return uleaf->magic == 0xc0de;
}

static int uleaf_can_free(struct btree *btree, void *leaf)
{
	struct uleaf *uleaf = leaf;
	return uleaf->count == 0;
}

static void uleaf_dump(struct btree *btree, void *data)
{
#if 0
	struct uleaf *leaf = data;
	__tux3_dbg("leaf %p/%i", leaf, leaf->count);
	struct uentry *entry, *limit = leaf->entries + leaf->count;
	for (entry = leaf->entries; entry < limit; entry++)
		__tux3_dbg(" %x:%x", entry->key, entry->val);
	__tux3_dbg(" (%x free)\n", uleaf_free(btree, leaf));
#endif
}

static tuxkey_t uleaf_split(struct btree *btree, tuxkey_t hint, void *vfrom, void *vinto)
{
	test_assert(uleaf_sniff(btree, vfrom));
	struct uleaf *from = vfrom, *into = vinto;
	unsigned at = from->count / 2;
	if (from->count && hint > from->entries[from->count - 1].key) // binsearch!
		at = from->count;
	unsigned tail = from->count - at;
	uleaf_init(btree, vinto);
	veccopy(into->entries, from->entries + at, tail);
	into->count = tail;
	from->count = at;
	return tail ? into->entries[0].key : hint;
}

static unsigned uleaf_seek(struct btree *btree, tuxkey_t key, struct uleaf *leaf)
{
	unsigned at = 0;
	while (at < leaf->count && leaf->entries[at].key < key)
		at++;
	return at;
}

static int uleaf_chop(struct btree *btree, tuxkey_t start, u64 len, void *vleaf)
{
	struct uleaf *leaf = vleaf;
	unsigned start_at, stop_at, count;
	tuxkey_t stop;

	/* Chop all range if len >= TUXKEY_LIMIT */
	stop = (len >= TUXKEY_LIMIT) ? TUXKEY_LIMIT : start + len;

	start_at = uleaf_seek(btree, start, leaf);
	stop_at = uleaf_seek(btree, stop, leaf);
	count = leaf->count - stop_at;
	vecmove(&leaf->entries[start_at], &leaf->entries[stop_at], count);
	leaf->count = start_at + count;
	return 1;
}

static int uleaf_merge(struct btree *btree, void *vinto, void *vfrom)
{
	struct uleaf *into = vinto;
	struct uleaf *from = vfrom;

	if (into->count + from->count > btree->entries_per_leaf)
		return 0;

	vecmove(&into->entries[into->count], from->entries, from->count);
	into->count += from->count;

	return 1;
}

static struct uentry *uleaf_resize(struct btree *btree, tuxkey_t key,
				   struct uleaf *leaf, unsigned one)
{
	test_assert(uleaf_sniff(btree, leaf));
	unsigned at = uleaf_seek(btree, key, leaf);
	if (at < leaf->count && leaf->entries[at].key == key)
		goto out;
	if (uleaf_free(btree, leaf) < one)
		return NULL;
	trace("expand leaf at 0x%x by %i", at, one);
	vecmove(leaf->entries + at + one, leaf->entries + at, leaf->count++ - at);
out:
	return leaf->entries + at;
}

static int uleaf_insert(struct btree *btree, struct uleaf *leaf, unsigned key, unsigned val)
{
	trace("insert 0x%x -> 0x%x", key, val);
	struct uentry *entry = uleaf_resize(btree, key, leaf, 1);
	if (!entry)
		return 1; // need to expand
	test_assert(entry);
	*entry = (struct uentry){ .key = key, .val = val };
	return 0;
}

static struct uentry *uleaf_lookup(struct uleaf *leaf, unsigned key)
{
	unsigned at;

	for (at = 0; at < leaf->count; at++) {
		if (leaf->entries[at].key == key)
			return &leaf->entries[at];
	}
	return NULL;
}

static int uleaf_write(struct btree *btree, tuxkey_t key_bottom,
		       tuxkey_t key_limit,
		       void *leaf, struct btree_key_range *key,
		       tuxkey_t *split_hint)
{
	struct uleaf_req *rq = container_of(key, struct uleaf_req, key);
	struct uleaf *uleaf = leaf;
	assert(key->len == 1);
	if (!uleaf_insert(btree, uleaf, key->start, rq->val)) {
		key->start++;
		key->len--;
		return BTREE_DO_RETRY;
	}

	*split_hint = key->start;
	return BTREE_DO_SPLIT;
}

static struct btree_ops ops = {
	.btree_init	= uleaf_btree_init,
	.leaf_init	= uleaf_init,
	.leaf_split	= uleaf_split,
	.leaf_merge	= uleaf_merge,
	.leaf_chop	= uleaf_chop,
	.leaf_pre_write	= noop_pre_write,
	.leaf_write	= uleaf_write,

	.leaf_sniff	= uleaf_sniff,
	.leaf_can_free	= uleaf_can_free,
	.leaf_dump	= uleaf_dump,
};

/* Test of new_leaf() and new_node() */
static void test01(struct sb *sb, struct inode *inode)
{
	struct btree *btree = &tux_inode(inode)->btree;
	int err;

	init_btree(btree, sb, no_root, &ops);
	err = alloc_empty_btree(btree);
	test_assert(!err);

	/* ->leaf_init() should be called */
	struct buffer_head *buffer = new_leaf(btree);
	test_assert(uleaf_sniff(btree, bufdata(buffer)));
	/* Test of uleaf_insert() */
	for (int i = 0; i < 7; i++)
		uleaf_insert(btree, bufdata(buffer), i, i + 0x100);
	for (int i = 0; i < 7; i++) {
		struct uentry *uentry = uleaf_lookup(bufdata(buffer), i);
		test_assert(uentry);
		test_assert(uentry->val == i + 0x100);
	}
	/* Test of uleaf_chop() */
	uleaf_chop(btree, 2, 3, bufdata(buffer));
	for (int i = 0; i < 7; i++) {
		struct uentry *uentry = uleaf_lookup(bufdata(buffer), i);
		if (2 <= i && i < 5) {
			test_assert(uentry == NULL);
		} else {
			test_assert(uentry);
			test_assert(uentry->val == i + 0x100);
		}
	}
	mark_buffer_dirty_non(buffer);
	uleaf_dump(btree, bufdata(buffer));
	blockput(buffer);

	clean_main(sb, inode);
}

static void btree_write_test(struct cursor *cursor, tuxkey_t key)
{
	int err;

	err = btree_probe(cursor, key);
	test_assert(!err);

	struct uleaf_req rq = {
		.key = {
			.start	= key,
			.len	= 1,
		},
		.val		= key + 0x100,
	};
	err = btree_write(cursor, &rq.key);
	test_assert(!err);

	block_t block = bufindex(cursor_leafbuf(cursor));
	release_cursor(cursor);

	/* probe added key: buffer should be same */
	err = btree_probe(cursor, key);
	test_assert(!err);
	struct buffer_head *leafbuf = cursor_leafbuf(cursor);
	test_assert(block == bufindex(leafbuf));
	struct uentry *entry = uleaf_lookup(bufdata(leafbuf), key);
	test_assert(entry);
	test_assert(entry->key == key);
	release_cursor(cursor);
}

/* btree_write() and btree_chop() test */
static void test02(struct sb *sb, struct inode *inode)
{
	struct btree *btree = &tux_inode(inode)->btree;
	int err;

	init_btree(btree, sb, no_root, &ops);
	err = alloc_empty_btree(btree);
	test_assert(!err);

	struct cursor *cursor = alloc_cursor(btree, 8); /* +8 for new depth */
	test_assert(cursor);

	/* At least add 1 depth */
	int keys = sb->entries_per_node * btree->entries_per_leaf + 1;
	/* Add keys to test tree_expand() until new depth */
	for (int key = 0; key < keys; key++)
		btree_write_test(cursor, key);
	test_assert(btree->root.depth == 2);
	/* Check key again after addition completed */
	for (int key = 0; key < keys; key++) {
		test_assert(btree_probe(cursor, key) == 0);
		struct buffer_head *leafbuf = cursor_leafbuf(cursor);
		struct uentry *entry = uleaf_lookup(bufdata(leafbuf), key);
		test_assert(entry);
		test_assert(entry->key == key);
		release_cursor(cursor);
	}
	/* Delte all */
	test_assert(btree_chop(btree, 0, TUXKEY_LIMIT) == 0);
	/* btree should have empty root */
	test_assert(btree->root.depth == 1);

	/* btree_probe() should return same path always */
	test_assert(btree_probe(cursor, 0) == 0);
	block_t root = bufindex(cursor->path[0].buffer);
	struct buffer_head *leafbuf = cursor_leafbuf(cursor);
	release_cursor(cursor);
	for (int key = 0; key < keys; key++) {
		test_assert(btree_probe(cursor, key) == 0);
		test_assert(root == bufindex(cursor->path[0].buffer));
		test_assert(leafbuf == cursor_leafbuf(cursor));
		/* This should be no key in leaf */
		struct uentry *entry = uleaf_lookup(bufdata(leafbuf), key);
		test_assert(entry == NULL);
		release_cursor(cursor);
	}

	free_cursor(cursor);

	clean_main(sb, inode);
}

/* btree_write() and btree_chop() test (reverse order) */
static void test03(struct sb *sb, struct inode *inode)
{
	struct btree *btree = &tux_inode(inode)->btree;
	int err;

	init_btree(btree, sb, no_root, &ops);
	err = alloc_empty_btree(btree);
	test_assert(!err);

	struct cursor *cursor = alloc_cursor(btree, 8); /* +8 for new depth */
	test_assert(cursor);

	/* Some depths */
	int keys = sb->entries_per_node * btree->entries_per_leaf * 100;

	for (int key = keys - 1; key >= 0; key--)
		btree_write_test(cursor, key);
	assert(btree->root.depth >= 5); /* this test expects more than 5 */

	/* Check key again after addition completed */
	for (int key = keys - 1; key >= 0; key--) {
		test_assert(btree_probe(cursor, key) == 0);
		struct buffer_head *leafbuf = cursor_leafbuf(cursor);
		struct uentry *entry = uleaf_lookup(bufdata(leafbuf), key);
		test_assert(entry);
		test_assert(entry->key == key);
		release_cursor(cursor);
	}
	/* Delete one by one for some keys from end */
	int left = sb->entries_per_node * btree->entries_per_leaf * 80;
	for (int key = keys - 1; key >= left; key--) {
		test_assert(btree_chop(btree, key, TUXKEY_LIMIT) == 0);

		int ret, check = 0;

		test_assert(btree_probe(cursor, check) == 0);
		do {
			struct buffer_head *leafbuf;

			leafbuf = cursor_leafbuf(cursor);
			while (uleaf_lookup(bufdata(leafbuf), check))
				check++;
			ret = cursor_advance(cursor);
			test_assert(ret >= 0);
		} while (ret);
		test_assert(check == key);
		release_cursor(cursor);
	}

	free_cursor(cursor);

	clean_main(sb, inode);
}

static void test04(struct sb *sb, struct inode *inode)
{
	struct btree *btree = &tux_inode(inode)->btree;
	int err;

	init_btree(btree, sb, no_root, &ops);
	err = alloc_empty_btree(btree);
	test_assert(!err);

	/* Insert_node test */
	struct cursor *cursor = alloc_cursor(btree, 1); /* +1 for new depth */
	test_assert(cursor);

	test_assert(!btree_probe(cursor, 0));
	for (int i = 0; i < sb->entries_per_node - 1; i++) {
		struct buffer_head *buffer = new_leaf(btree);
		trace("buffer: index %Lx", buffer->index);
		test_assert(!IS_ERR(buffer));
		mark_buffer_dirty_non(buffer);
		test_assert(btree_insert_leaf(cursor, 100 + i, buffer) == 0);
	}
	release_cursor(cursor);
	/* Insert key=1 after key=0 */
	test_assert(!btree_probe(cursor, 0));
	struct buffer_head *buffer = new_leaf(btree);
	test_assert(!IS_ERR(buffer));
	mark_buffer_dirty_non(buffer);
	test_assert(btree_insert_leaf(cursor, 1, buffer) == 0);
	/* probe same key with cursor2 */
	struct cursor *cursor2 = alloc_cursor(btree, 0);
	test_assert(!btree_probe(cursor2, 1));
	for (int i = 0; i <= cursor->level; i++) {
		test_assert(cursor->path[i].buffer == cursor2->path[i].buffer);
		test_assert(cursor->path[i].next == cursor2->path[i].next);
	}
	release_cursor(cursor);
	release_cursor(cursor2);
	free_cursor(cursor);
	free_cursor(cursor2);
	test_assert(!btree_chop(btree, 0, TUXKEY_LIMIT));

	clean_main(sb, inode);
}

static void clean_test05(struct sb *sb, struct inode *inode,
			 struct cursor *cursor, struct path_level *path)
{
	release_cursor(cursor);
	free_cursor(cursor);
	free(path);

	clean_main(sb, inode);
}

/* Test of cursor_redirect() */
static void test05(struct sb *sb, struct inode *inode)
{
	struct btree *btree = &tux_inode(inode)->btree;
	struct path_level *orig;
	int err;

	init_btree(btree, sb, no_root, &ops);
	err = alloc_empty_btree(btree);
	test_assert(!err);

	init_btree(btree, sb, no_root, &ops);
	err = alloc_empty_btree(btree);
	test_assert(!err);

	struct cursor *cursor = alloc_cursor(btree, 8); /* +8 for new depth */
	test_assert(cursor);

	/* Some depths */
	int keys = sb->entries_per_node * btree->entries_per_leaf * 100;
	for (int key = keys - 1; key >= 0; key--)
		btree_write_test(cursor, key);
	assert(btree->root.depth >= 5); /* this test expects more than 5 */

	test_assert(btree_probe(cursor, 0) == 0);
	orig = malloc(sizeof(*orig) * (cursor->level + 1));
	memcpy(orig, cursor->path, sizeof(*orig) * (cursor->level + 1));

	if (test_start("test05.1")) {
		/* Redirect full path */
		for (int i = 0; i <= cursor->level; i++) {
			set_buffer_clean(orig[i].buffer);
			get_bh(orig[i].buffer);
		}
		test_assert(cursor_redirect(cursor) == 0);
		for (int i = 0; i <= cursor->level; i++) {
			struct path_level *at = &cursor->path[i];

			/* Modify orignal buffer */
			memset(bufdata(orig[i].buffer), 0, sb->blocksize);
			blockput(orig[i].buffer);

			/* Redirected? */
			test_assert(orig[i].buffer != at->buffer);
			/* If not leaf, check ->next too */
			if (i < cursor->level)
				test_assert(orig[i].next != at->next);
		}
		release_cursor(cursor);

		/* Check key */
		for (int key = 0; key < keys; key++) {
			struct buffer_head *leafbuf;
			struct uentry *entry;

			test_assert(btree_probe(cursor, key) == 0);
			leafbuf = cursor_leafbuf(cursor);
			entry = uleaf_lookup(bufdata(leafbuf), key);
			test_assert(entry);
			test_assert(entry->key == key);
			release_cursor(cursor);
		}

		clean_test05(sb, inode, cursor, orig);
	}
	test_end();

	if (test_start("test05.2")) {
		/* Redirect partial path */
		for (int i = cursor->level / 2; i <= cursor->level ; i++) {
			set_buffer_clean(orig[i].buffer);
			get_bh(orig[i].buffer);
		}
		test_assert(cursor_redirect(cursor) == 0);
		for (int i = 0; i <= cursor->level; i++) {
			struct path_level *at = &cursor->path[i];

			/* Redirected? */
			if (i < cursor->level / 2) {
				test_assert(orig[i].buffer == at->buffer);
				test_assert(orig[i].next == at->next);
				continue;
			}

			/* Modify orignal buffer */
			memset(bufdata(orig[i].buffer), 0, sb->blocksize);
			blockput(orig[i].buffer);

			test_assert(orig[i].buffer != at->buffer);
			/* If not leaf, check ->next too */
			if (i < cursor->level)
				test_assert(orig[i].next != at->next);
		}
		release_cursor(cursor);

		/* Check key */
		for (int key = 0; key < keys; key++) {
			struct buffer_head *leafbuf;
			struct uentry *entry;

			test_assert(btree_probe(cursor, key) == 0);

			leafbuf = cursor_leafbuf(cursor);
			entry = uleaf_lookup(bufdata(leafbuf), key);
			test_assert(entry);
			test_assert(entry->key == key);
			release_cursor(cursor);
		}

		clean_test05(sb, inode, cursor, orig);
	}
	test_end();

	clean_test05(sb, inode, cursor, orig);
}

/* btree_chop() range chop (and adjust_parent_sep()) test */
static void test06(struct sb *sb, struct inode *inode)
{
	struct btree *btree = &tux_inode(inode)->btree;

	init_btree(btree, sb, no_root, &ops);

	/*
	 * Test below:
	 *
	 *         +----- (0, 8)---------+
	 *         |                     |
	 *    + (..., 2, 5) +        + (8, 12) +
	 *    |        |    |        |         |
	 * (dummy)   (3,4) (6,7)   (10,11)    (13,14)
	 *
	 * Make above tree and chop (7 - 10), then btree_chop() merges
	 * (6) and (11). And adjust_parent_sep() adjust (0,8) to (0,12).
	 *
	 * [(dummy) is to prevent merge nodes of (2,5) and (8,12)]
	 */

	/* Create leaves */
	struct buffer_head *leaf[4];
	int leaf_key[] = { 3, 6, 10, 13, };
	for (int i = 0; i < ARRAY_SIZE(leaf); i++) {
		leaf[i] = new_leaf(btree);
		test_assert(uleaf_sniff(btree, bufdata(leaf[i])));
		for (int j = leaf_key[i]; j < leaf_key[i] + 2; j++)
			uleaf_insert(btree, bufdata(leaf[i]), j, j + 0x100);
	}

	/* Create nodes */
	struct buffer_head *node[3];
	/* [left key, right key, left child, right child] */
	int node_key[3][4] = {
		{ 0, 8, 0, 0, }, /* child pointer is filled later */
		{ 2, 5, bufindex(leaf[0]), bufindex(leaf[1]), },
		{ 8, 12, bufindex(leaf[2]), bufindex(leaf[3]), },
	};
	for (int i = 0; i < ARRAY_SIZE(node); i++) {
		node[i] = new_node(btree);
		for (int j = 0; j < 2; j++) {
			struct bnode *bnode = bufdata(node[i]);
			struct index_entry *p = bnode->entries;
			bnode_add_index(bnode, p + j, node_key[i][2 + j],
					node_key[i][j]);
		}
	}
	/* fill node with dummy to prevent merge */
	for (int i = 0; i < sb->entries_per_node - 2; i++) {
		struct bnode *bnode = bufdata(node[1]);
		bnode_add_index(bnode, bnode->entries, 0, 100);
	}

	/* Fill child pointer in root node */
	struct bnode *root = bufdata(node[0]);
	root->entries[0].block = cpu_to_be64(bufindex(node[1]));
	root->entries[1].block = cpu_to_be64(bufindex(node[2]));
	/* Set root node to btree */
	btree->root = (struct root){ .block = bufindex(node[0]), .depth = 2 };

	for(int i = 0; i < ARRAY_SIZE(leaf); i++) {
		mark_buffer_dirty_non(leaf[i]);
		blockput(leaf[i]);
	}
	for(int i = 0; i < ARRAY_SIZE(node); i++) {
		mark_buffer_unify_non(node[i]);
		blockput(node[i]);
	}

	struct cursor *cursor = alloc_cursor(btree, 8); /* +8 for new depth */
	test_assert(cursor);

	/* Check keys */
	for (int i = 0; i < ARRAY_SIZE(leaf_key); i++) {
		test_assert(btree_probe(cursor, leaf_key[i]) == 0);
		struct buffer_head *leafbuf = cursor_leafbuf(cursor);
		for (int j = 0; j < 2; j++) {
			struct uentry *entry;
			entry = uleaf_lookup(bufdata(leafbuf), leaf_key[i] + j);
			test_assert(entry);
			test_assert(entry->key == leaf_key[i] + j);
		}
		release_cursor(cursor);
	}

	/* Chop (7 - 10) and check again */
	test_assert(btree_chop(btree, 7, 4) == 0);
	/* Check if adjust_parent_sep() changed key from 8 to 12 */
	test_assert(cursor_read_root(cursor) == 0);
	root = bufdata(cursor->path[cursor->level].buffer);
	test_assert(be64_to_cpu(root->entries[1].key) == 12);
	release_cursor(cursor);

	for (int i = 0; i < ARRAY_SIZE(leaf_key); i++) {
		test_assert(btree_probe(cursor, leaf_key[i]) == 0);
		struct buffer_head *leafbuf = cursor_leafbuf(cursor);
		for (int j = 0; j < 2; j++) {
			struct uentry *entry;
			entry = uleaf_lookup(bufdata(leafbuf), leaf_key[i] + j);
			if (7 <= leaf_key[i] + j && leaf_key[i] + j <= 10) {
				test_assert(entry == NULL);
			} else {
				test_assert(entry);
				test_assert(entry->key == leaf_key[i] + j);
			}
		}
		release_cursor(cursor);
	}

	free_cursor(cursor);

	clean_main(sb, inode);
}

static void clean_test07(struct sb *sb, struct inode *inode,
			 struct cursor *cursor)
{
	release_cursor(cursor);
	free_cursor(cursor);

	clean_main(sb, inode);
}

/* Test of insert_leaf() cursor adjust */
static void test07(struct sb *sb, struct inode *inode)
{
	struct btree *btree = &tux_inode(inode)->btree;

	init_btree(btree, sb, no_root, &ops);

	struct cursor *cursor = alloc_cursor(btree, 8); /* +8 for new depth */
	test_assert(cursor);

	/*
	 * After insert_leaf(), cursor must still be valid.
	 */

	/*
	 * Create path of following:
	 *
	 * +---------------+
	 * |0   | 100 | 200|     point child from entry in left half
	 * +---------------+
	 *    |
	 * ---+
	 * V
	 * +---------------+
	 * |0   |  10 | 20 |     point child from entry of right half
	 * +---------------+
	 *              |
	 * -------------+
	 * V
	 * +---------------+
	 * |               |
	 * +---------------+
	 */

	/* Create leaves */
	struct buffer_head *leaf[2];
	int leaf_key[] = { 3, 6, 10, 13, };
	for (int i = 0; i < ARRAY_SIZE(leaf); i++) {
		leaf[i] = new_leaf(btree);
		test_assert(uleaf_sniff(btree, bufdata(leaf[i])));
		for (int j = leaf_key[i]; j < leaf_key[i] + 2; j++)
			uleaf_insert(btree, bufdata(leaf[i]), j, j + 0x100);
	}

	test_assert(sb->entries_per_node == 3);	/* this test is assuming 3 */
	/* Create nodes */
	struct buffer_head *node[2];
	for (int i = 0; i < ARRAY_SIZE(node); i++) {
		node[i] = new_node(btree);
		struct bnode *bnode = bufdata(node[i]);
		/* fill node with dummy to make split node in insert_leaf() */
		for (int j = 0; j < sb->entries_per_node; j++) {
			struct index_entry *p = &bnode->entries[bcount(bnode)];
			if (i == 0)
				bnode_add_index(bnode, p, 0, j*100);
			else
				bnode_add_index(bnode, p, 0, j*10);
		}
	}

	/* Set next at left half */
	struct bnode *bnode;
	bnode = bufdata(node[0]);
	bnode->entries[0].block = cpu_to_be64(bufindex(node[1]));
	cursor_push(cursor, node[0], &bnode->entries[0 + 1]);
	/* Set next at right half */
	unsigned right = sb->entries_per_node / 2 + 1;
	bnode = bufdata(node[1]);
	bnode->entries[right].block = cpu_to_be64(bufindex(leaf[0]));
	cursor_push(cursor, node[1], &bnode->entries[right + 1]);
	/* push leaf */
	cursor_push(cursor, leaf[0], NULL);

	/* Set root node to btree */
	btree->root = (struct root){ .block = bufindex(node[0]), .depth = 2 };
	cursor_check(cursor);

	/* insert_leaf with keep == 0 */
	if (test_start("test07.1")) {
		int err = insert_leaf(cursor, 15, leaf[1], 0);
		test_assert(err == 0);
		cursor_check(cursor);

		clean_test07(sb, inode, cursor);
	}
	test_end();

	/* insert_leaf with keep == 1 */
	if (test_start("test07.2")) {
		int err = insert_leaf(cursor, 15, leaf[1], 1);
		test_assert(err == 0);
		cursor_check(cursor);

		clean_test07(sb, inode, cursor);
	}
	test_end();

	blockput(leaf[1]);

	clean_test07(sb, inode, cursor);
}

int main(int argc, char *argv[])
{
	struct dev *dev = &(struct dev){ .bits = 6 };
	init_buffers(dev, 1 << 20, 2);

	int err = tux3_init_mem();
	assert(!err);

	struct sb *sb = rapid_sb(dev);
	sb->super = INIT_DISKSB(dev->bits, 2048);
	setup_sb(sb, &sb->super);

	sb->volmap = tux_new_volmap(sb);
	assert(sb->volmap);
	sb->logmap = tux_new_logmap(sb);
	assert(sb->logmap);

	struct inode *inode = rapid_open_inode(sb, dev_errio, 0);
	assert(inode);

	test_init(argv[0]);

	/* Set fake backend mark to modify backend objects. */
	tux3_start_backend(sb);

	if (test_start("test01"))
		test01(sb, inode);
	test_end();

	if (test_start("test02"))
		test02(sb, inode);
	test_end();

	if (test_start("test03"))
		test03(sb, inode);
	test_end();

	if (test_start("test04"))
		test04(sb, inode);
	test_end();

	if (test_start("test05"))
		test05(sb, inode);
	test_end();

	if (test_start("test06"))
		test06(sb, inode);
	test_end();

	if (test_start("test07"))
		test07(sb, inode);
	test_end();

	tux3_end_backend();

	clean_main(sb, inode);

	return test_failures();
}
