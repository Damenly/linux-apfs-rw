// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2018 Ernesto A. Fernández <ernesto.mnd.fernandez@gmail.com>
 */

#include <linux/slab.h>
#include <linux/buffer_head.h>
#include <linux/mpage.h>
#include <linux/blk_types.h>
#include "apfs.h"

#define MAX_PFK_LEN	512

static int apfs_readpage(struct file *file, struct page *page)
{
	return mpage_readpage(page, apfs_get_block);
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 8, 0) /* Misses mpage_readpages() */

static void apfs_readahead(struct readahead_control *rac)
{
	mpage_readahead(rac, apfs_get_block);
}

#else /* LINUX_VERSION_CODE >= KERNEL_VERSION(5, 8, 0) */

static int apfs_readpages(struct file *file, struct address_space *mapping,
			  struct list_head *pages, unsigned int nr_pages)
{
	return mpage_readpages(mapping, pages, nr_pages, apfs_get_block);
}

#endif /* LINUX_VERSION_CODE >= KERNEL_VERSION(5, 8, 0) */

/**
 * apfs_create_dstream_rec - Create the data stream record for an inode
 * @inode: the vfs inode
 *
 * Does nothing if the record already exists.  TODO: support cloned files.
 * Returns 0 on success or a negative error code in case of failure.
 */
static int apfs_create_dstream_rec(struct inode *inode)
{
	struct super_block *sb = inode->i_sb;
	struct apfs_sb_info *sbi = APFS_SB(sb);
	struct apfs_inode_info *ai = APFS_I(inode);
	struct apfs_key key;
	struct apfs_query *query;
	struct apfs_dstream_id_key raw_key;
	struct apfs_dstream_id_val raw_val;
	int ret;

	if (inode->i_size || inode->i_blocks) /* Already has a dstream */
		return 0;

	apfs_init_dstream_id_key(ai->i_extent_id, &key);
	query = apfs_alloc_query(sbi->s_cat_root, NULL /* parent */);
	if (!query)
		return -ENOMEM;
	query->key = &key;
	query->flags |= APFS_QUERY_CAT | APFS_QUERY_EXACT;

	ret = apfs_btree_query(sb, &query);
	if (ret != -ENODATA) /* Either an error, or the record already exists */
		goto out;

	apfs_key_set_hdr(APFS_TYPE_DSTREAM_ID, ai->i_extent_id, &raw_key);
	raw_val.refcnt = cpu_to_le32(1);
	ret = apfs_btree_insert(query, &raw_key, sizeof(raw_key),
				&raw_val, sizeof(raw_val));
out:
	apfs_free_query(sb, query);
	return ret;
}
#define APFS_CREATE_DSTREAM_REC_MAXOPS	1

/**
 * apfs_create_crypto_rec - Create the crypto state record for an inode
 * @inode: the vfs inode
 *
 * Does nothing if the record already exists.  TODO: support cloned files.
 * Returns 0 on success or a negative error code in case of failure.
 */
static int apfs_create_crypto_rec(struct inode *inode)
{
	struct super_block *sb = inode->i_sb;
	struct apfs_sb_info *sbi = APFS_SB(sb);
	struct apfs_inode_info *ai = APFS_I(inode);
	struct apfs_key key;
	struct apfs_query *query;
	struct apfs_crypto_state_key raw_key;
	int ret;

	if (inode->i_size || inode->i_blocks) /* Already has a dstream */
		return 0;

	apfs_init_crypto_state_key(ai->i_extent_id, &key);
	query = apfs_alloc_query(sbi->s_cat_root, NULL /* parent */);
	if (!query)
		return -ENOMEM;
	query->key = &key;
	query->flags |= APFS_QUERY_CAT | APFS_QUERY_EXACT;

	ret = apfs_btree_query(sb, &query);
	if (ret != -ENODATA) /* Either an error, or the record already exists */
		goto out;

	apfs_key_set_hdr(APFS_TYPE_CRYPTO_STATE, ai->i_extent_id, &raw_key);
	if(sbi->s_dflt_pfk) {
		struct apfs_crypto_state_val *raw_val = sbi->s_dflt_pfk;
		unsigned key_len = le16_to_cpu(raw_val->state.key_len);
		ret = apfs_btree_insert(query, &raw_key, sizeof(raw_key),
					raw_val, sizeof(*raw_val) + key_len);
	} else {
		struct apfs_crypto_state_val raw_val;
		raw_val.refcnt = cpu_to_le32(1);
		raw_val.state.major_version = cpu_to_le16(APFS_WMCS_MAJOR_VERSION);
		raw_val.state.minor_version = cpu_to_le16(APFS_WMCS_MINOR_VERSION);
		raw_val.state.cpflags = 0;
		raw_val.state.persistent_class = cpu_to_le32(APFS_PROTECTION_CLASS_F);
		raw_val.state.key_os_version = 0;
		raw_val.state.key_revision = cpu_to_le16(1);
		raw_val.state.key_len = cpu_to_le16(0);
		ret = apfs_btree_insert(query, &raw_key, sizeof(raw_key),
					&raw_val, sizeof(raw_val));
	}
out:
	apfs_free_query(sb, query);
	return ret;
}
#define APFS_CREATE_CRYPTO_REC_MAXOPS	1

/**
 * apfs_dflt_key_class - Returns default key class for files in volume
 * @sb: volume superblock
 */
static unsigned apfs_dflt_key_class(struct super_block *sb)
{
	struct apfs_sb_info *sbi = APFS_SB(sb);

	if (!sbi->s_dflt_pfk)
		return APFS_PROTECTION_CLASS_F;

	return le32_to_cpu(sbi->s_dflt_pfk->state.persistent_class);
}

/**
 * apfs_create_crypto_rec - Adjust crypto state record refcount
 * @sb: volume superblock
 * @crypto_id: crypto_id to adjust
 * @delta: desired change in reference count
 *
 * This function is used when adding or removing extents, as each extent holds
 * a reference to the crypto ID. It should also be used when removing inodes,
 * and in that case it should also remove the crypto record (TODO).
 */
int apfs_crypto_adj_refcnt(struct super_block *sb, u64 crypto_id, int delta)
{
	struct apfs_sb_info *sbi = APFS_SB(sb);
	struct apfs_key key;
	struct apfs_query *query;
	struct apfs_crypto_state_val *raw_val;
	char *raw;
	int ret;

	if (!crypto_id)
		return 0;

	apfs_init_crypto_state_key(crypto_id, &key);
	query = apfs_alloc_query(sbi->s_cat_root, NULL /* parent */);
	if (!query)
		return -ENOMEM;
	query->key = &key;
	query->flags |= APFS_QUERY_CAT | APFS_QUERY_EXACT;

	ret = apfs_btree_query(sb, &query);
	if (ret)
		goto out;

	ret = apfs_query_join_transaction(query);
	if (ret)
		return ret;
	raw = query->node->object.bh->b_data;
	raw_val = (void *)raw + query->off;

	le32_add_cpu(&raw_val->refcnt, delta);

out:
	apfs_free_query(sb, query);
	return ret;
}
int APFS_CRYPTO_ADJ_REFCNT_MAXOPS(void)
{
	return 1;
}

/**
 * apfs_crypto_set_key - Modify content of crypto state record
 * @sb: volume superblock
 * @crypto_id: crypto_id to modify
 * @new_val: new crypto state data; new_val->refcnt is overridden
 *
 * This function does not alter the inode's default protection class field.
 * It needs to be done separately if the class changes.
 */
static int apfs_crypto_set_key(struct super_block *sb, u64 crypto_id, struct apfs_crypto_state_val *new_val)
{
	struct apfs_sb_info *sbi = APFS_SB(sb);
	struct apfs_key key;
	struct apfs_query *query;
	struct apfs_crypto_state_val *raw_val;
	char *raw;
	int ret;
	unsigned pfk_len;

	if (!crypto_id)
		return 0;

	pfk_len = le16_to_cpu(new_val->state.key_len);

	apfs_init_crypto_state_key(crypto_id, &key);
	query = apfs_alloc_query(sbi->s_cat_root, NULL /* parent */);
	if (!query)
		return -ENOMEM;
	query->key = &key;
	query->flags |= APFS_QUERY_CAT | APFS_QUERY_EXACT;

	ret = apfs_btree_query(sb, &query);
	if (ret)
		goto out;
	raw = query->node->object.bh->b_data;
	raw_val = (void *)raw + query->off;

	new_val->refcnt = raw_val->refcnt;

	ret = apfs_btree_replace(query, NULL /* key */, 0 /* key_len */,
				 new_val, sizeof(*new_val) + pfk_len);

out:
	apfs_free_query(sb, query);
	return ret;
}
#define APFS_CRYPTO_SET_KEY_MAXOPS	1

/**
 * apfs_crypto_get_key - Retrieve content of crypto state record
 * @sb: volume superblock
 * @crypto_id: crypto_id to modify
 * @val: result crypto state data
 * @max_len: maximum allowed value of val->state.key_len
 */
static int apfs_crypto_get_key(struct super_block *sb, u64 crypto_id, struct apfs_crypto_state_val *val,
			       unsigned max_len)
{
	struct apfs_sb_info *sbi = APFS_SB(sb);
	struct apfs_key key;
	struct apfs_query *query;
	struct apfs_crypto_state_val *raw_val;
	char *raw;
	int ret;
	unsigned pfk_len;

	if (!crypto_id)
		return -ENOENT;

	apfs_init_crypto_state_key(crypto_id, &key);
	query = apfs_alloc_query(sbi->s_cat_root, NULL /* parent */);
	if (!query)
		return -ENOMEM;
	query->key = &key;
	query->flags |= APFS_QUERY_CAT | APFS_QUERY_EXACT;

	ret = apfs_btree_query(sb, &query);
	if (ret)
		goto out;
	raw = query->node->object.bh->b_data;
	raw_val = (void *)raw + query->off;

	pfk_len = le16_to_cpu(raw_val->state.key_len);
	if(pfk_len > max_len) {
		ret = -ENOSPC;
		goto out;
	}

	memcpy(val, raw_val, sizeof(*val) + pfk_len);

out:
	apfs_free_query(sb, query);
	return ret;
}

static int apfs_write_begin(struct file *file, struct address_space *mapping,
			    loff_t pos, unsigned int len, unsigned int flags,
			    struct page **pagep, void **fsdata)
{
	struct inode *inode = mapping->host;
	struct super_block *sb = inode->i_sb;
	struct page *page;
	struct buffer_head *bh, *head;
	unsigned int blocksize, block_start, block_end, from, to;
	pgoff_t index = pos >> PAGE_SHIFT;
	sector_t iblock = (sector_t)index << (PAGE_SHIFT - inode->i_blkbits);
	int blkcount = (len + sb->s_blocksize - 1) >> inode->i_blkbits;
	struct apfs_max_ops maxops;
	int err;

	maxops.cat = APFS_CREATE_DSTREAM_REC_MAXOPS +
		     APFS_CREATE_CRYPTO_REC_MAXOPS +
		     APFS_UPDATE_INODE_MAXOPS() +
		     blkcount * APFS_GET_NEW_BLOCK_MAXOPS();
	maxops.blks = blkcount;

	err = apfs_transaction_start(sb, maxops);
	if (err)
		return err;

	err = apfs_create_dstream_rec(inode);
	if (err)
		goto out_abort;

	if(apfs_vol_is_encrypted(sb)) {
		err = apfs_create_crypto_rec(inode);
		if (err)
			goto out_abort;
	}

	page = grab_cache_page_write_begin(mapping, index,
					   flags | AOP_FLAG_NOFS);
	if (!page) {
		err = -ENOMEM;
		goto out_abort;
	}
	if (!page_has_buffers(page))
		create_empty_buffers(page, sb->s_blocksize, 0);

	/* CoW moves existing blocks, so read them but mark them as unmapped */
	head = page_buffers(page);
	blocksize = head->b_size;
	from = pos & (PAGE_SIZE - 1);
	to = from + min(inode->i_size - pos, (loff_t)len);
	for (bh = head, block_start = 0; bh != head || !block_start;
	     block_start = block_end, bh = bh->b_this_page, ++iblock) {
		block_end = block_start + blocksize;
		if (to > block_start && from < block_end) {
			if (!buffer_mapped(bh)) {
				err = __apfs_get_block(inode, iblock, bh,
						       false /* create */);
				if (err)
					goto out_put_page;
			}
			if (!buffer_uptodate(bh)) {
				get_bh(bh);
				lock_buffer(bh);
				bh->b_end_io = end_buffer_read_sync;
				submit_bh(REQ_OP_READ, 0, bh);
				wait_on_buffer(bh);
				if (!buffer_uptodate(bh)) {
					err = -EIO;
					goto out_put_page;
				}
			}
			clear_buffer_mapped(bh);
		}
	}

	err = __block_write_begin(page, pos, len, apfs_get_new_block);
	if (err)
		goto out_put_page;

	*pagep = page;
	return 0;

out_put_page:
	unlock_page(page);
	put_page(page);
out_abort:
	apfs_transaction_abort(sb);
	return err;
}

static int apfs_write_end(struct file *file, struct address_space *mapping,
			  loff_t pos, unsigned int len, unsigned int copied,
			  struct page *page, void *fsdata)
{
	struct inode *inode = mapping->host;
	struct super_block *sb = inode->i_sb;
	struct buffer_head *bh, *head;
	int ret, err;

	ret = generic_write_end(file, mapping, pos, len, copied, page, fsdata);
	if (ret < len) {
		/* XXX: handle short writes */
		err = -EIO;
		goto out_abort;
	}

	bh = head = page_buffers(page);
	do {
		if (buffer_dirty(bh))
			sync_dirty_buffer(bh);
		put_bh(bh);
		bh = bh->b_this_page;
	} while (bh != head);

	/* TODO: write all metadata for inodes at transaction commit instead? */
	err = apfs_flush_extent_cache(inode);
	if (err)
		goto out_abort;

	err = apfs_update_inode(inode, NULL /* new_name */);
	if (err)
		goto out_abort;

	err = apfs_transaction_commit(sb);
	if (!err)
		return ret;

out_abort:
	apfs_transaction_abort(sb);
	return err;
}

/* bmap is not implemented to avoid issues with CoW on swapfiles */
static const struct address_space_operations apfs_aops = {
	.readpage	= apfs_readpage,
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 8, 0)
	.readahead      = apfs_readahead,
#else
	.readpages      = apfs_readpages,
#endif
	.write_begin	= apfs_write_begin,
	.write_end	= apfs_write_end,
};

/**
 * apfs_inode_set_ops - Set up an inode's operations
 * @inode:	vfs inode to set up
 * @rdev:	device id (0 if not a device file)
 * @compressed:	is this a compressed inode?
 *
 * For device files, also sets the device id to @rdev.
 */
static void apfs_inode_set_ops(struct inode *inode, dev_t rdev, bool compressed)
{
	/* A lot of operations still missing, of course */
	switch (inode->i_mode & S_IFMT) {
	case S_IFREG:
		inode->i_op = &apfs_file_inode_operations;
		if (compressed)
			inode->i_fop = &apfs_compress_file_operations;
		else
			inode->i_fop = &apfs_file_operations;
		inode->i_mapping->a_ops = &apfs_aops;
		break;
	case S_IFDIR:
		inode->i_op = &apfs_dir_inode_operations;
		inode->i_fop = &apfs_dir_operations;
		break;
	case S_IFLNK:
		inode->i_op = &apfs_symlink_inode_operations;
		break;
	default:
		inode->i_op = &apfs_special_inode_operations;
		init_special_inode(inode, inode->i_mode, rdev);
		break;
	}
}

/**
 * apfs_inode_from_query - Read the inode found by a successful query
 * @query:	the query that found the record
 * @inode:	vfs inode to be filled with the read data
 *
 * Reads the inode record into @inode and performs some basic sanity checks,
 * mostly as a protection against crafted filesystems.  Returns 0 on success
 * or a negative error code otherwise.
 */
static int apfs_inode_from_query(struct apfs_query *query, struct inode *inode)
{
	struct apfs_inode_info *ai = APFS_I(inode);
	struct apfs_inode_val *inode_val;
	char *raw = query->node->object.bh->b_data;
	char *xval = NULL;
	int xlen;
	u32 rdev = 0, bsd_flags;
	bool compressed = false;

	if (query->len < sizeof(*inode_val))
		goto corrupted;

	inode_val = (struct apfs_inode_val *)(raw + query->off);

	ai->i_parent_id = le64_to_cpu(inode_val->parent_id);
	ai->i_extent_id = le64_to_cpu(inode_val->private_id);
	inode->i_mode = le16_to_cpu(inode_val->mode);
	ai->i_key_class = le32_to_cpu(inode_val->default_protection_class);
	ai->i_int_flags = le64_to_cpu(inode_val->internal_flags);

	ai->i_saved_uid = le32_to_cpu(inode_val->owner);
	i_uid_write(inode, ai->i_saved_uid);
	ai->i_saved_gid = le32_to_cpu(inode_val->group);
	i_gid_write(inode, ai->i_saved_gid);

	if (!S_ISDIR(inode->i_mode)) {
		/*
		 * Directory inodes don't store their link count, so to provide
		 * it we would have to actually count the subdirectories. The
		 * HFS/HFS+ modules just leave it at 1, and so do we, for now.
		 */
		set_nlink(inode, le32_to_cpu(inode_val->nlink));
	} else {
		ai->i_nchildren = le32_to_cpu(inode_val->nchildren);
	}

	inode->i_atime = ns_to_timespec64(le64_to_cpu(inode_val->access_time));
	inode->i_ctime = ns_to_timespec64(le64_to_cpu(inode_val->change_time));
	inode->i_mtime = ns_to_timespec64(le64_to_cpu(inode_val->mod_time));
	ai->i_crtime = ns_to_timespec64(le64_to_cpu(inode_val->create_time));

	inode->i_size = inode->i_blocks = 0;
	bsd_flags = le32_to_cpu(inode_val->bsd_flags);
	if ((bsd_flags & APFS_INOBSD_COMPRESSED) && !S_ISDIR(inode->i_mode)) {
		if (!apfs_compress_get_size(inode, &inode->i_size)) {
			/* TODO: correct block calculation in general */
			inode->i_blocks = (inode->i_size + 511) >> 9;
			compressed = true;
		}
	} else {
		xlen = apfs_find_xfield(inode_val->xfields,
					query->len - sizeof(*inode_val),
					APFS_INO_EXT_TYPE_DSTREAM, &xval);
		if (xlen >= sizeof(struct apfs_dstream)) {
			struct apfs_dstream *dstream = (struct apfs_dstream *)xval;

			inode->i_size = le64_to_cpu(dstream->size);
			inode->i_blocks = le64_to_cpu(dstream->alloced_size) >> 9;
		}
	}
	xval = NULL;

	rdev = 0;
	xlen = apfs_find_xfield(inode_val->xfields,
				query->len - sizeof(*inode_val),
				APFS_INO_EXT_TYPE_RDEV, &xval);
	if (xlen >= sizeof(__le32)) {
		__le32 *rdev_p = (__le32 *)xval;

		rdev = le32_to_cpup(rdev_p);
	}

	apfs_inode_set_ops(inode, rdev, compressed);
	return 0;

corrupted:
	apfs_alert(inode->i_sb,
		   "bad inode record for inode 0x%llx", apfs_ino(inode));
	return -EFSCORRUPTED;
}

/**
 * apfs_inode_lookup - Lookup an inode record in the catalog b-tree
 * @inode:	vfs inode to lookup
 *
 * Runs a catalog query for the apfs_ino(@inode) inode record; returns a pointer
 * to the query structure on success, or an error pointer in case of failure.
 */
static struct apfs_query *apfs_inode_lookup(const struct inode *inode)
{
	struct super_block *sb = inode->i_sb;
	struct apfs_sb_info *sbi = APFS_SB(sb);
	struct apfs_key key;
	struct apfs_query *query;
	int ret;

	apfs_init_inode_key(apfs_ino(inode), &key);

	query = apfs_alloc_query(sbi->s_cat_root, NULL /* parent */);
	if (!query)
		return ERR_PTR(-ENOMEM);
	query->key = &key;
	query->flags |= APFS_QUERY_CAT | APFS_QUERY_EXACT;

	ret = apfs_btree_query(sb, &query);
	if (!ret)
		return query;

	apfs_free_query(sb, query);
	return ERR_PTR(ret);
}

/**
 * apfs_test_inode - Check if the inode matches a 64-bit inode number
 * @inode:	inode to test
 * @cnid:	pointer to the inode number
 */
static int apfs_test_inode(struct inode *inode, void *cnid)
{
	u64 *ino = cnid;

	return apfs_ino(inode) == *ino;
}

/**
 * apfs_set_inode - Set a 64-bit inode number on the given inode
 * @inode:	inode to set
 * @cnid:	pointer to the inode number
 */
static int apfs_set_inode(struct inode *inode, void *cnid)
{
	apfs_set_ino(inode, *(u64 *)cnid);
	return 0;
}

/**
 * apfs_iget_locked - Wrapper for iget5_locked()
 * @sb:		filesystem superblock
 * @cnid:	64-bit inode number
 *
 * Works the same as iget_locked(), but can handle 64-bit inode numbers on
 * 32-bit architectures.
 */
static struct inode *apfs_iget_locked(struct super_block *sb, u64 cnid)
{
	return iget5_locked(sb, cnid, apfs_test_inode, apfs_set_inode, &cnid);
}

/**
 * apfs_iget - Populate inode structures with metadata from disk
 * @sb:		filesystem superblock
 * @cnid:	inode number
 *
 * Populates the vfs inode and the corresponding apfs_inode_info structure.
 * Returns a pointer to the vfs inode in case of success, or an appropriate
 * error pointer otherwise.
 */
struct inode *apfs_iget(struct super_block *sb, u64 cnid)
{
	struct apfs_sb_info *sbi = APFS_SB(sb);
	struct apfs_nxsb_info *nxi = APFS_NXI(sb);
	struct inode *inode;
	struct apfs_query *query;
	int err;

	inode = apfs_iget_locked(sb, cnid);
	if (!inode)
		return ERR_PTR(-ENOMEM);
	if (!(inode->i_state & I_NEW))
		return inode;

	down_read(&nxi->nx_big_sem);
	query = apfs_inode_lookup(inode);
	if (IS_ERR(query)) {
		err = PTR_ERR(query);
		goto fail;
	}
	err = apfs_inode_from_query(query, inode);
	apfs_free_query(sb, query);
	if (err)
		goto fail;
	up_read(&nxi->nx_big_sem);

	/* Allow the user to override the ownership */
	if (uid_valid(sbi->s_uid))
		inode->i_uid = sbi->s_uid;
	if (gid_valid(sbi->s_gid))
		inode->i_gid = sbi->s_gid;

	/* Inode flags are not important for now, leave them at 0 */
	unlock_new_inode(inode);
	return inode;

fail:
	up_read(&nxi->nx_big_sem);
	iget_failed(inode);
	return ERR_PTR(err);
}

#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 11, 0) /* No statx yet... */

int apfs_getattr(struct vfsmount *mnt, struct dentry *dentry,
		 struct kstat *stat)
{
	struct inode *inode = d_inode(dentry);

	generic_fillattr(inode, stat);
	stat->ino = apfs_ino(inode);
	return 0;
}


#elif LINUX_VERSION_CODE < KERNEL_VERSION(5, 12, 0)
int apfs_getattr(const struct path *path, struct kstat *stat,
		 u32 request_mask, unsigned int query_flags)
#else
int apfs_getattr(struct user_namespace *mnt_userns,
		 const struct path *path, struct kstat *stat, u32 request_mask,
		 unsigned int query_flags)
#endif
{
	struct inode *inode = d_inode(path->dentry);
	struct apfs_inode_info *ai = APFS_I(inode);

	stat->result_mask |= STATX_BTIME;
	stat->btime = ai->i_crtime;

	if (apfs_xattr_get(inode, APFS_XATTR_NAME_COMPRESSED, NULL, 0) >= 0)
		stat->attributes |= STATX_ATTR_COMPRESSED;

	stat->attributes_mask |= STATX_ATTR_COMPRESSED;

#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 12, 0)
	generic_fillattr(inode, stat);
#else
	generic_fillattr(mnt_userns, inode, stat);
#endif

	stat->ino = apfs_ino(inode);
	return 0;
}

/**
 * apfs_build_inode_val - Allocate and initialize the value for an inode record
 * @inode:	vfs inode to record
 * @qname:	filename for primary link
 * @val_p:	on return, a pointer to the new on-disk value structure
 *
 * Returns the length of the value, or a negative error code in case of failure.
 */
static int apfs_build_inode_val(struct inode *inode, struct qstr *qname,
				struct apfs_inode_val **val_p)
{
	struct apfs_inode_val *val;
	struct apfs_x_field xkey;
	int total_xlen, val_len;
	__le32 rdev = cpu_to_le32(inode->i_rdev);

	/* The only required xfield is the name, and the id if it's a device */
	total_xlen = sizeof(struct apfs_xf_blob);
	total_xlen += sizeof(xkey) + round_up(qname->len + 1, 8);
	if (inode->i_rdev)
		total_xlen += sizeof(xkey) + round_up(sizeof(rdev), 8);

	val_len = sizeof(*val) + total_xlen;
	val = kzalloc(val_len, GFP_KERNEL);
	if (!val)
		return -ENOMEM;

	val->parent_id = cpu_to_le64(APFS_I(inode)->i_parent_id);
	val->private_id = cpu_to_le64(apfs_ino(inode));

	val->mod_time = cpu_to_le64(timespec64_to_ns(&inode->i_mtime));
	val->create_time = val->change_time = val->access_time = val->mod_time;

	if (S_ISDIR(inode->i_mode))
		val->nchildren = 0;
	else
		val->nlink = cpu_to_le32(1);

	val->owner = cpu_to_le32(i_uid_read(inode));
	val->group = cpu_to_le32(i_gid_read(inode));
	val->mode = cpu_to_le16(inode->i_mode);

	/* The buffer was just allocated: none of these functions should fail */
	apfs_init_xfields(val->xfields, total_xlen);
	xkey.x_type = APFS_INO_EXT_TYPE_NAME;
	xkey.x_flags = APFS_XF_DO_NOT_COPY;
	xkey.x_size = cpu_to_le16(qname->len + 1);
	apfs_insert_xfield(val->xfields, total_xlen, &xkey, qname->name);
	if (inode->i_rdev) {
		xkey.x_type = APFS_INO_EXT_TYPE_RDEV;
		xkey.x_flags = 0; /* TODO: proper flags here? */
		xkey.x_size = cpu_to_le16(sizeof(rdev));
		apfs_insert_xfield(val->xfields, total_xlen, &xkey, &rdev);
	}

	*val_p = val;
	return val_len;
}

/*
 * apfs_inode_rename - Update the primary name reported in an inode record
 * @inode:	the in-memory inode
 * @new_name:	name of the new primary link (NULL if unchanged)
 * @query:	the query that found the inode record
 *
 * Returns 0 on success, or a negative error code in case of failure.
 */
static int apfs_inode_rename(struct inode *inode, char *new_name,
			     struct apfs_query *query)
{
	char *raw = query->node->object.bh->b_data;
	struct apfs_inode_val *new_val = NULL;
	int buflen, namelen;
	struct apfs_x_field xkey;
	int xlen;
	int err;

	if (!new_name)
		return 0;

	namelen = strlen(new_name) + 1; /* Count the null-termination */
	buflen = query->len;
	buflen += sizeof(struct apfs_x_field) + round_up(namelen, 8);
	new_val = kzalloc(buflen, GFP_KERNEL);
	if (!new_val)
		return -ENOMEM;
	memcpy(new_val, raw + query->off, query->len);

	/* TODO: can we assume that all inode records have an xfield blob? */
	xkey.x_type = APFS_INO_EXT_TYPE_NAME;
	xkey.x_flags = APFS_XF_DO_NOT_COPY;
	xkey.x_size = cpu_to_le16(namelen);
	xlen = apfs_insert_xfield(new_val->xfields, buflen - sizeof(*new_val),
				  &xkey, new_name);
	if (!xlen) {
		/* Buffer has enough space, but the metadata claims otherwise */
		apfs_alert(inode->i_sb, "bad xfields on inode 0x%llx",
			   apfs_ino(inode));
		err = -EFSCORRUPTED;
		goto fail;
	}

	/* Just remove the old record and create a new one */
	err = apfs_btree_replace(query, NULL /* key */, 0 /* key_len */,
				 new_val, sizeof(*new_val) + xlen);

fail:
	kfree(new_val);
	return err;
}
#define APFS_INODE_RENAME_MAXOPS	1

/**
 * apfs_create_dstream_xfield - Create the inode xfield for a new data stream
 * @inode:	the in-memory inode
 * @query:	the query that found the inode record
 *
 * Returns 0 on success, or a negative error code in case of failure.
 */
static int apfs_create_dstream_xfield(struct inode *inode,
				      struct apfs_query *query)
{
	char *raw = query->node->object.bh->b_data;
	struct apfs_inode_val *new_val;
	struct apfs_dstream dstream = {0};
	struct apfs_x_field xkey;
	struct apfs_inode_info *ai = APFS_I(inode);
	int xlen;
	int buflen;
	int err;

	buflen = query->len;
	buflen += sizeof(struct apfs_x_field) + sizeof(dstream);
	new_val = kzalloc(buflen, GFP_KERNEL);
	if (!new_val)
		return -ENOMEM;
	memcpy(new_val, raw + query->off, query->len);

	dstream.size = cpu_to_le64(inode->i_size);
	dstream.alloced_size = cpu_to_le64(inode->i_blocks << 9);
	if(apfs_vol_is_encrypted(inode->i_sb))
		dstream.default_crypto_id = cpu_to_le64(ai->i_extent_id);

	/* TODO: can we assume that all inode records have an xfield blob? */
	xkey.x_type = APFS_INO_EXT_TYPE_DSTREAM;
	xkey.x_flags = APFS_XF_SYSTEM_FIELD;
	xkey.x_size = cpu_to_le16(sizeof(dstream));
	xlen = apfs_insert_xfield(new_val->xfields, buflen - sizeof(*new_val),
				  &xkey, &dstream);
	if (!xlen) {
		/* Buffer has enough space, but the metadata claims otherwise */
		apfs_alert(inode->i_sb, "bad xfields on inode 0x%llx",
			   apfs_ino(inode));
		err = -EFSCORRUPTED;
		goto fail;
	}

	/* Just remove the old record and create a new one */
	err = apfs_btree_replace(query, NULL /* key */, 0 /* key_len */,
				 new_val, sizeof(*new_val) + xlen);

fail:
	kfree(new_val);
	return err;
}
#define APFS_CREATE_DSTREAM_XFIELD_MAXOPS	1

/**
 * apfs_inode_resize - Update the sizes reported in an inode record
 * @inode:	the in-memory inode
 * @query:	the query that found the inode record
 *
 * Returns 0 on success, or a negative error code in case of failure.
 */
static int apfs_inode_resize(struct inode *inode, struct apfs_query *query)
{
	char *raw;
	struct apfs_inode_val *inode_raw;
	char *xval;
	int xlen;
	int err;

	err = apfs_query_join_transaction(query);
	if (err)
		return err;
	raw = query->node->object.bh->b_data;
	inode_raw = (void *)raw + query->off;

	xlen = apfs_find_xfield(inode_raw->xfields,
				query->len - sizeof(*inode_raw),
				APFS_INO_EXT_TYPE_DSTREAM, &xval);
	if (!xlen && !inode->i_size) /* Empty file: no dstream needed yet */
		return 0;

	if (xlen) {
		struct apfs_dstream *dstream;

		if (xlen != sizeof(*dstream))
			return -EFSCORRUPTED;
		dstream = (struct apfs_dstream *)xval;

		/* TODO: count bytes read and written */
		dstream->size = cpu_to_le64(inode->i_size);
		dstream->alloced_size = cpu_to_le64(inode->i_blocks << 9);
		return 0;
	}
	/* This inode has no dstream xfield, so we need to create it */
	return apfs_create_dstream_xfield(inode, query);
}
#define APFS_INODE_RESIZE_MAXOPS	(1 + APFS_CREATE_DSTREAM_XFIELD_MAXOPS)

/**
 * apfs_update_inode - Update an existing inode record
 * @inode:	the modified in-memory inode
 * @new_name:	name of the new primary link (NULL if unchanged)
 *
 * Returns 0 on success, or a negative error code in case of failure.
 */
int apfs_update_inode(struct inode *inode, char *new_name)
{
	struct super_block *sb = inode->i_sb;
	struct apfs_sb_info *sbi = APFS_SB(sb);
	struct apfs_inode_info *ai = APFS_I(inode);
	struct apfs_query *query;
	struct buffer_head *bh;
	struct apfs_btree_node_phys *node_raw;
	struct apfs_inode_val *inode_raw;
	int err;

	query = apfs_inode_lookup(inode);
	if (IS_ERR(query))
		return PTR_ERR(query);

	err = apfs_inode_rename(inode, new_name, query);
	if (err)
		goto fail;

	err = apfs_inode_resize(inode, query);
	if (err)
		goto fail;

	/* TODO: just use apfs_btree_replace()? */
	err = apfs_query_join_transaction(query);
	if (err)
		goto fail;
	bh = query->node->object.bh;
	node_raw = (void *)bh->b_data;
	apfs_assert_in_transaction(sb, &node_raw->btn_o);
	inode_raw = (void *)node_raw + query->off;

	inode_raw->parent_id = cpu_to_le64(ai->i_parent_id);
	inode_raw->private_id = cpu_to_le64(ai->i_extent_id);
	inode_raw->mode = cpu_to_le16(inode->i_mode);
	inode_raw->owner = cpu_to_le32(i_uid_read(inode));
	inode_raw->group = cpu_to_le32(i_gid_read(inode));
	inode_raw->default_protection_class = cpu_to_le32(ai->i_key_class);
	inode_raw->internal_flags = cpu_to_le64(ai->i_int_flags);

	/* Don't persist the uid/gid provided by the user on mount */
	if (uid_valid(sbi->s_uid))
		inode_raw->owner = cpu_to_le32(ai->i_saved_uid);
	if (gid_valid(sbi->s_gid))
		inode_raw->group = cpu_to_le32(ai->i_saved_gid);

	inode_raw->access_time = cpu_to_le64(timespec64_to_ns(&inode->i_atime));
	inode_raw->change_time = cpu_to_le64(timespec64_to_ns(&inode->i_ctime));
	inode_raw->mod_time = cpu_to_le64(timespec64_to_ns(&inode->i_mtime));
	inode_raw->create_time = cpu_to_le64(timespec64_to_ns(&ai->i_crtime));

	if (S_ISDIR(inode->i_mode)) {
		inode_raw->nchildren = cpu_to_le32(ai->i_nchildren);
	} else {
		/* Orphaned inodes are still linked under private-dir */
		inode_raw->nlink = cpu_to_le32(inode->i_nlink ? : 1);
	}

fail:
	apfs_free_query(sb, query);
	return err;
}
int APFS_UPDATE_INODE_MAXOPS(void)
{
	return APFS_INODE_RENAME_MAXOPS + APFS_INODE_RESIZE_MAXOPS + 1;
}

/**
 * apfs_delete_inode - Delete an inode record and update the volume file count
 * @inode: the vfs inode to delete
 *
 * Returns 0 on success or a negative error code in case of failure.
 */
static int apfs_delete_inode(struct inode *inode)
{
	struct super_block *sb = inode->i_sb;
	struct apfs_superblock *vsb_raw = APFS_SB(sb)->s_vsb_raw;
	struct apfs_query *query;
	int ret;

	if (inode->i_size || inode->i_blocks) /* TODO: implement truncation */
		return -EOPNOTSUPP;

	query = apfs_inode_lookup(inode);
	if (IS_ERR(query))
		return PTR_ERR(query);
	ret = apfs_btree_remove(query);
	apfs_free_query(sb, query);

	apfs_assert_in_transaction(sb, &vsb_raw->apfs_o);
	switch (inode->i_mode & S_IFMT) {
	case S_IFREG:
		le64_add_cpu(&vsb_raw->apfs_num_files, -1);
		break;
	case S_IFDIR:
		le64_add_cpu(&vsb_raw->apfs_num_directories, -1);
		break;
	case S_IFLNK:
		le64_add_cpu(&vsb_raw->apfs_num_symlinks, -1);
		break;
	default:
		le64_add_cpu(&vsb_raw->apfs_num_other_fsobjects, -1);
		break;
	}
	return ret;
}
#define APFS_DELETE_INODE_MAXOPS	1

void apfs_evict_inode(struct inode *inode)
{
	struct super_block *sb = inode->i_sb;
	struct apfs_max_ops maxops;

	if (is_bad_inode(inode) || inode->i_nlink)
		goto out_clear;

	maxops.cat = APFS_DELETE_INODE_MAXOPS + APFS_DELETE_ORPHAN_LINK_MAXOPS();
	maxops.blks = 0;

	if (apfs_transaction_start(sb, maxops))
		goto out_report;
	if (apfs_delete_inode(inode))
		goto out_abort;
	if (apfs_delete_orphan_link(inode))
		goto out_abort;
	if (apfs_transaction_commit(sb))
		goto out_abort;
	goto out_clear;

out_abort:
	apfs_transaction_abort(sb);
out_report:
	apfs_warn(sb,
		  "failed to delete orphan inode 0x%llx\n", apfs_ino(inode));
out_clear:
	truncate_inode_pages_final(&inode->i_data);
	clear_inode(inode);
}

/**
 * apfs_insert_inode_locked - Wrapper for insert_inode_locked4()
 * @inode: vfs inode to insert in cache
 *
 * Works the same as insert_inode_locked(), but can handle 64-bit inode numbers
 * on 32-bit architectures.
 */
static int apfs_insert_inode_locked(struct inode *inode)
{
	u64 cnid = apfs_ino(inode);

	return insert_inode_locked4(inode, cnid, apfs_test_inode, &cnid);
}

/**
 * apfs_new_inode - Create a new in-memory inode
 * @dir:	parent inode
 * @mode:	mode bits for the new inode
 * @rdev:	device id (0 if not a device file)
 *
 * Returns a pointer to the new vfs inode on success, or an error pointer in
 * case of failure.
 */
struct inode *apfs_new_inode(struct inode *dir, umode_t mode, dev_t rdev)
{
	struct super_block *sb = dir->i_sb;
	struct apfs_superblock *vsb_raw = APFS_SB(sb)->s_vsb_raw;
	struct inode *inode;
	struct apfs_inode_info *ai;
	u64 cnid;
	struct timespec64 now;

	/* Updating on-disk structures here is odd, but it works for now */
	apfs_assert_in_transaction(sb, &vsb_raw->apfs_o);

	inode = new_inode(sb);
	if (!inode)
		return ERR_PTR(-ENOMEM);
	ai = APFS_I(inode);

	cnid = le64_to_cpu(vsb_raw->apfs_next_obj_id);
	le64_add_cpu(&vsb_raw->apfs_next_obj_id, 1);
	apfs_set_ino(inode, cnid);

#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 12, 0)
	inode_init_owner(inode, dir, mode);
#else
	inode_init_owner(&init_user_ns, inode, dir, mode);
#endif

	ai->i_saved_uid = i_uid_read(inode);
	ai->i_saved_gid = i_gid_read(inode);
	ai->i_parent_id = apfs_ino(dir);
	ai->i_extent_id = cnid;
	set_nlink(inode, 1);
	ai->i_nchildren = 0;
	if (apfs_vol_is_encrypted(sb) && S_ISREG(mode))
		ai->i_key_class = apfs_dflt_key_class(sb);
	else
		ai->i_key_class = 0;
	ai->i_int_flags = APFS_INODE_NO_RSRC_FORK;

	now = current_time(inode);
	inode->i_atime = inode->i_mtime = inode->i_ctime = ai->i_crtime = now;
	vsb_raw->apfs_last_mod_time = cpu_to_le64(timespec64_to_ns(&now));

	/* Symlinks are not yet supported */
	if (S_ISREG(mode))
		le64_add_cpu(&vsb_raw->apfs_num_files, 1);
	else if (S_ISDIR(mode))
		le64_add_cpu(&vsb_raw->apfs_num_directories, 1);
	else if (S_ISLNK(mode))
		le64_add_cpu(&vsb_raw->apfs_num_symlinks, 1);
	else
		le64_add_cpu(&vsb_raw->apfs_num_other_fsobjects, 1);

	if (apfs_insert_inode_locked(inode)) {
		/* The inode number should have been free, but wasn't */
		make_bad_inode(inode);
		iput(inode);
		return ERR_PTR(-EFSCORRUPTED);
	}

	/* No need to dirty the inode, we'll write it to disk right away */
	apfs_inode_set_ops(inode, rdev, false /* compressed */);
	return inode;
}

/**
 * apfs_create_inode_rec - Create an inode record in the catalog b-tree
 * @sb:		filesystem superblock
 * @inode:	vfs inode to record
 * @dentry:	dentry for primary link
 *
 * Returns 0 on success or a negative error code in case of failure.
 */
int apfs_create_inode_rec(struct super_block *sb, struct inode *inode,
			  struct dentry *dentry)
{
	struct apfs_sb_info *sbi = APFS_SB(sb);
	struct apfs_key key;
	struct apfs_query *query;
	struct apfs_inode_key raw_key;
	struct apfs_inode_val *raw_val;
	int val_len;
	int ret;

	apfs_init_inode_key(apfs_ino(inode), &key);
	query = apfs_alloc_query(sbi->s_cat_root, NULL /* parent */);
	if (!query)
		return -ENOMEM;
	query->key = &key;
	query->flags |= APFS_QUERY_CAT;

	ret = apfs_btree_query(sb, &query);
	if (ret && ret != -ENODATA)
		goto fail;

	apfs_key_set_hdr(APFS_TYPE_INODE, apfs_ino(inode), &raw_key);

	val_len = apfs_build_inode_val(inode, &dentry->d_name, &raw_val);
	if (val_len < 0) {
		ret = val_len;
		goto fail;
	}

	ret = apfs_btree_insert(query, &raw_key, sizeof(raw_key),
				raw_val, val_len);
	kfree(raw_val);

fail:
	apfs_free_query(sb, query);
	return ret;
}
int APFS_CREATE_INODE_REC_MAXOPS(void)
{
	return 1;
}

#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 12, 0)
int apfs_setattr(struct dentry *dentry, struct iattr *iattr)
#else
int apfs_setattr(struct user_namespace *mnt_userns,
		 struct dentry *dentry, struct iattr *iattr)
#endif
{
	struct inode *inode = d_inode(dentry);
	struct super_block *sb = inode->i_sb;
	struct apfs_max_ops maxops;
	int err;

	if (iattr->ia_valid & ATTR_SIZE && iattr->ia_size != inode->i_size)
		return -EOPNOTSUPP; /* TODO: implement truncation */

#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 12, 0)
	err = simple_setattr(dentry, iattr);
#else
	err = simple_setattr(mnt_userns, dentry, iattr);
#endif

	if(err)
		return err;

	maxops.cat = APFS_UPDATE_INODE_MAXOPS();
	maxops.blks = 0;

	/* TODO: figure out why ->write_inode() isn't firing */
	err = apfs_transaction_start(sb, maxops);
	if (err)
		return err;
	err = apfs_update_inode(inode, NULL /* new_name */);
	if (err)
		goto fail;
	err = apfs_transaction_commit(sb);
	if (err)
		goto fail;
	return 0;

fail:
	apfs_transaction_abort(sb);
	return err;
}

static int apfs_ioc_set_dflt_pfk(struct file *file, void __user *user_pfk)
{
	struct inode *inode = file_inode(file);
	struct super_block *sb = inode->i_sb;
	struct apfs_sb_info *sbi = APFS_SB(sb);
	struct apfs_nxsb_info *nxi = APFS_NXI(sb);
	struct apfs_wrapped_crypto_state pfk_hdr;
	struct apfs_crypto_state_val *pfk;
	unsigned key_len;

	if (__copy_from_user(&pfk_hdr, user_pfk, sizeof(pfk_hdr)))
		return -EFAULT;
	key_len = le16_to_cpu(pfk_hdr.key_len);
	if (key_len > MAX_PFK_LEN)
		return -EFBIG;
	pfk = kmalloc(sizeof(*pfk) + key_len, GFP_KERNEL);
	if (!pfk)
		return -ENOMEM;
	if (__copy_from_user(&pfk->state, user_pfk, sizeof(pfk_hdr) + key_len)) {
		kfree(pfk);
		return -EFAULT;
	}
	pfk->refcnt = cpu_to_le32(1);

	down_write(&nxi->nx_big_sem);

	if (sbi->s_dflt_pfk)
		kfree(sbi->s_dflt_pfk);
	sbi->s_dflt_pfk = pfk;

	up_write(&nxi->nx_big_sem);

	return 0;
}

static int apfs_ioc_set_dir_class(struct file *file, u32 __user *user_class)
{
	struct inode *inode = file_inode(file);
	struct apfs_inode_info *ai = APFS_I(inode);
	struct super_block *sb = inode->i_sb;
	struct apfs_max_ops maxops;
	u32 class;
	int err;

	if (get_user(class, user_class))
		return -EFAULT;

	ai->i_key_class = class;

	maxops.cat = APFS_UPDATE_INODE_MAXOPS();
	maxops.blks = 0;

	err = apfs_transaction_start(sb, maxops);
	if (err)
		return err;
	err = apfs_update_inode(inode, NULL /* new_name */);
	if (err)
		goto fail;
	err = apfs_transaction_commit(sb);
	if (err)
		goto fail;
	return 0;

fail:
	apfs_transaction_abort(sb);
	return err;
}

static int apfs_ioc_set_pfk(struct file *file, void __user *user_pfk)
{
	struct inode *inode = file_inode(file);
	struct super_block *sb = inode->i_sb;
	struct apfs_wrapped_crypto_state pfk_hdr;
	struct apfs_crypto_state_val *pfk;
	struct apfs_inode_info *ai = APFS_I(inode);
	struct apfs_max_ops maxops;
	unsigned key_len, key_class;
	int err;

	if (__copy_from_user(&pfk_hdr, user_pfk, sizeof(pfk_hdr)))
		return -EFAULT;
	key_len = le16_to_cpu(pfk_hdr.key_len);
	if (key_len > MAX_PFK_LEN)
		return -EFBIG;
	pfk = kmalloc(sizeof(*pfk) + key_len, GFP_KERNEL);
	if (!pfk)
		return -ENOMEM;
	if (__copy_from_user(&pfk->state, user_pfk, sizeof(pfk_hdr) + key_len)) {
		kfree(pfk);
		return -EFAULT;
	}
	pfk->refcnt = cpu_to_le32(1);

	maxops.cat = APFS_CRYPTO_SET_KEY_MAXOPS + APFS_UPDATE_INODE_MAXOPS();
	maxops.blks = 0;

	err = apfs_transaction_start(sb, maxops);
	if (err) {
		kfree(pfk);
		return err;
	}

	err = apfs_crypto_set_key(sb, ai->i_extent_id, pfk);
	if (err)
		goto fail;

	key_class = le32_to_cpu(pfk_hdr.persistent_class);
	if (ai->i_key_class != key_class) {
		ai->i_key_class = key_class;
		err = apfs_update_inode(inode, NULL /* new_name */);
		if (err)
			goto fail;
	}

	err = apfs_transaction_commit(sb);
	if (err)
		goto fail;
	kfree(pfk);
	return 0;

fail:
	apfs_transaction_abort(sb);
	kfree(pfk);
	return err;
}

static int apfs_ioc_get_class(struct file *file, u32 __user *user_class)
{
	struct inode *inode = file_inode(file);
	struct apfs_inode_info *ai = APFS_I(inode);
	u32 class;

	class = ai->i_key_class;
	if (put_user(class, user_class))
		return -EFAULT;
	return 0;
}

static int apfs_ioc_get_pfk(struct file *file, void __user *user_pfk)
{
	struct inode *inode = file_inode(file);
	struct super_block *sb = inode->i_sb;
	struct apfs_nxsb_info *nxi = APFS_NXI(sb);
	struct apfs_wrapped_crypto_state pfk_hdr;
	struct apfs_crypto_state_val *pfk;
	unsigned max_len, key_len;
	struct apfs_inode_info *ai = APFS_I(inode);
	int err;

	if (__copy_from_user(&pfk_hdr, user_pfk, sizeof(pfk_hdr)))
		return -EFAULT;
	max_len = le16_to_cpu(pfk_hdr.key_len);
	if (max_len > MAX_PFK_LEN)
		return -EFBIG;
	pfk = kmalloc(sizeof(*pfk) + max_len, GFP_KERNEL);
	if (!pfk)
		return -ENOMEM;

	down_read(&nxi->nx_big_sem);

	err = apfs_crypto_get_key(sb, ai->i_extent_id, pfk, max_len);
	if (err)
		goto fail;

	up_read(&nxi->nx_big_sem);

	key_len = le16_to_cpu(pfk->state.key_len);
	if (__copy_to_user(user_pfk, &pfk->state, sizeof(pfk_hdr) + key_len)) {
		kfree(pfk);
		return -EFAULT;
	}

	kfree(pfk);
	return 0;

fail:
	up_read(&nxi->nx_big_sem);
	kfree(pfk);
	return err;
}

long apfs_dir_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	void __user *argp = (void __user *)arg;

	switch (cmd) {
	case APFS_IOC_SET_DFLT_PFK:
		return apfs_ioc_set_dflt_pfk(file, argp);
	case APFS_IOC_SET_DIR_CLASS:
		return apfs_ioc_set_dir_class(file, argp);
	case APFS_IOC_GET_CLASS:
		return apfs_ioc_get_class(file, argp);
	default:
		return -ENOTTY;
	}
}

long apfs_file_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	void __user *argp = (void __user *)arg;

	switch (cmd) {
	case APFS_IOC_SET_PFK:
		return apfs_ioc_set_pfk(file, argp);
	case APFS_IOC_GET_CLASS:
		return apfs_ioc_get_class(file, argp);
	case APFS_IOC_GET_PFK:
		return apfs_ioc_get_pfk(file, argp);
	default:
		return -ENOTTY;
	}
}
