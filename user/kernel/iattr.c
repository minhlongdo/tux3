/*
 * Inode table attributes
 *
 * Original copyright (c) 2008 Daniel Phillips <phillips@phunq.net>
 * Portions copyright (c) 2006-2008 Google Inc.
 * Licensed under the GPL version 3
 *
 * By contributing changes to this file you grant the original copyright holder
 * the right to distribute those changes under any license.
 */

#include "tux3.h"

/*
 * Variable size attribute format:
 *
 *    immediate data: kind+version:16, bytes:16, data[bytes]
 *    immediate xattr: kind+version:16, bytes:16, atom:16, data[bytes - 2]
 */

unsigned atsize[MAX_ATTRS] = {
	[MODE_OWNER_ATTR] = 12,
	[CTIME_SIZE_ATTR] = 14,
	[DATA_BTREE_ATTR] = 8,
	[LINK_COUNT_ATTR] = 4,
	[MTIME_ATTR] = 6,
	[IDATA_ATTR] = 2,
	[XATTR_ATTR] = 4,
};

unsigned encode_asize(unsigned bits)
{
	unsigned need = 0;
	for (int kind = MIN_ATTR; kind < VAR_ATTRS; kind++)
		if ((bits & (1 << kind)))
			need += atsize[kind] + 2;
	return need;
}

int attr_check(void *attrs, unsigned size)
{
	void *limit = attrs + size;
	unsigned head;
	while (attrs < limit - 1)
	{
		attrs = decode16(attrs, &head);
		unsigned kind = head >> 12;
		if (kind < MIN_ATTR || kind >= MAX_ATTRS)
			return 0;
		if (attrs + atsize[kind] > limit)
			return 0;
		attrs += atsize[kind];
	}
	return 1;
}

void dump_attrs(struct inode *inode)
{
	//printf("present = %x\n", inode->present);
	for (int kind = 0; kind < 32; kind++) {
		if (!(tux_inode(inode)->present & (1 << kind)))
			continue;
		switch (kind) {
		case MODE_OWNER_ATTR:
			printf("mode 0%.6o uid %x gid %x ", inode->i_mode, inode->i_uid, inode->i_gid);
			break;
		case DATA_BTREE_ATTR:
			printf("root %Lx:%u ", (L)tux_inode(inode)->btree.root.block, tux_inode(inode)->btree.root.depth);
			break;
		case CTIME_SIZE_ATTR:
			printf("ctime %Lx size %Lx ", (L)tuxtime(inode->i_ctime), (L)inode->i_size);
			break;
		case MTIME_ATTR:
			printf("mtime %Lx ", (L)tuxtime(inode->i_mtime));
			break;
		case LINK_COUNT_ATTR:
			printf("links %u ", inode->i_nlink);
			break;
		case XATTR_ATTR:
			printf("xattr(s) ");
			break;
		default:
			printf("<%i>? ", kind);
			break;
		}
	}
	printf("\n");
}

void *encode_attrs(struct inode *inode, void *attrs, unsigned size)
{
	//printf("encode %u attr bytes\n", size);
	void *limit = attrs + size - 3;
	for (int kind = MIN_ATTR; kind < VAR_ATTRS; kind++) {
		if (!(tux_inode(inode)->present & (1 << kind)))
			continue;
		if (attrs >= limit)
			break;
		attrs = encode_kind(attrs, kind, tux_sb(inode->i_sb)->version);
		switch (kind) {
		case MODE_OWNER_ATTR:
			attrs = encode32(attrs, inode->i_mode);
			attrs = encode32(attrs, inode->i_uid);
			attrs = encode32(attrs, inode->i_gid);
			break;
		case CTIME_SIZE_ATTR:
			attrs = encode48(attrs, tuxtime(inode->i_ctime) >> TIME_ATTR_SHIFT);
			attrs = encode64(attrs, inode->i_size);
			break;
		case MTIME_ATTR:
			attrs = encode48(attrs, tuxtime(inode->i_mtime) >> TIME_ATTR_SHIFT);
			break;
		case DATA_BTREE_ATTR:;
			struct root *root = &tux_inode(inode)->btree.root;
			attrs = encode64(attrs, ((u64)root->depth << 48) | root->block);
			break;
		case LINK_COUNT_ATTR:
			attrs = encode32(attrs, inode->i_nlink);
			break;
		}
	}
	return attrs;
}

void *decode_attrs(struct inode *inode, void *attrs, unsigned size)
{
	//printf("decode %u attr bytes\n", size);
	u64 v64;
	u32 v32;
	struct xattr *xattr = tux_inode(inode)->xcache ? tux_inode(inode)->xcache->xattrs : NULL;
	void *limit = attrs + size;
	while (attrs < limit - 1) {
		unsigned head;
		attrs = decode16(attrs, &head);
		unsigned version = head & 0xfff, kind = head >> 12;
		if (version != tux_sb(inode->i_sb)->version) {
			attrs += atsize[kind];
			continue;
		}
		switch (kind) {
		case MODE_OWNER_ATTR:
			attrs = decode32(attrs, &v32);
			inode->i_mode = v32;
			attrs = decode32(attrs, &v32);
			inode->i_uid = v32;
			attrs = decode32(attrs, &v32);
			inode->i_gid = v32;
			break;
		case CTIME_SIZE_ATTR:
			attrs = decode48(attrs, &v64);
			attrs = decode64(attrs, &inode->i_size);
			inode->i_ctime = spectime(v64 << TIME_ATTR_SHIFT);
			break;
		case MTIME_ATTR:
			attrs = decode48(attrs, &v64);
			inode->i_mtime = spectime(v64 << TIME_ATTR_SHIFT);
			break;
		case DATA_BTREE_ATTR:
			attrs = decode64(attrs, &v64);
			tux_inode(inode)->btree = (struct btree){ .sb = tux_sb(inode->i_sb), .entries_per_leaf = 64, // !!! should depend on blocksize
				.ops = &dtree_ops,
				.root = { .block = v64 & (-1ULL >> 16), .depth = v64 >> 48 } };
			break;
		case LINK_COUNT_ATTR:
			attrs = decode32(attrs, &inode->i_nlink);
			break;
		case XATTR_ATTR:;
			// immediate xattr: kind+version:16, bytes:16, atom:16, data[bytes - 2]
			unsigned bytes, atom;
			attrs = decode16(attrs, &bytes);
			attrs = decode16(attrs, &atom);
			*xattr = (struct xattr){ .atom = atom, .size = bytes - 2 };
			unsigned xsize = sizeof(struct xattr) + xattr->size;
			assert((void *)xattr + xsize <= (void *)tux_inode(inode)->xcache + tux_inode(inode)->xcache->maxsize);
			memcpy(xattr->body, attrs, xattr->size);
			attrs += xattr->size;
			tux_inode(inode)->xcache->size += xsize;
			xattr = xcache_next(xattr); // check limit!!!
			break;
		default:
			return NULL;
		}
		tux_inode(inode)->present |= 1 << kind;
	}
	if (!(tux_inode(inode)->present & MTIME_BIT))
		inode->i_mtime = inode->i_ctime;
	return attrs;
}
