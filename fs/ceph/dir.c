// SPDX-License-Identifier: GPL-2.0
#include <linux/ceph/ceph_debug.h>

#include <linux/spinlock.h>
#include <linux/namei.h>
#include <linux/slab.h>
#include <linux/sched.h>
#include <linux/xattr.h>

#include "super.h"
#include "mds_client.h"
#include "crypto.h"

/*
 * Directory operations: readdir, lookup, create, link, unlink,
 * rename, etc.
 */

/*
 * Ceph MDS operations are specified in terms of a base ino and
 * relative path.  Thus, the client can specify an operation on a
 * specific inode (e.g., a getattr due to fstat(2)), or as a path
 * relative to, say, the root directory.
 *
 * Normally, we limit ourselves to strict inode ops (no path component)
 * or dentry operations (a single path component relative to an ino).  The
 * exception to this is open_root_dentry(), which will open the mount
 * point by name.
 */

const struct dentry_operations ceph_dentry_ops;

static bool __dentry_lease_is_valid(struct ceph_dentry_info *di);
static int __dir_lease_try_check(const struct dentry *dentry);

/*
 * Initialize ceph dentry state.
 */
static int ceph_d_init(struct dentry *dentry)
{
	struct ceph_dentry_info *di;
	struct ceph_mds_client *mdsc = ceph_sb_to_mdsc(dentry->d_sb);

	di = kmem_cache_zalloc(ceph_dentry_cachep, GFP_KERNEL);
	if (!di)
		return -ENOMEM;          /* oh well */

	di->dentry = dentry;
	di->lease_session = NULL;
	di->time = jiffies;
	dentry->d_fsdata = di;
	INIT_LIST_HEAD(&di->lease_list);

	atomic64_inc(&mdsc->metric.total_dentries);

	return 0;
}

/*
 * for f_pos for readdir:
 * - hash order:
 *	(0xff << 52) | ((24 bits hash) << 28) |
 *	(the nth entry has hash collision);
 * - frag+name order;
 *	((frag value) << 28) | (the nth entry in frag);
 */
#define OFFSET_BITS	28
#define OFFSET_MASK	((1 << OFFSET_BITS) - 1)
#define HASH_ORDER	(0xffull << (OFFSET_BITS + 24))
loff_t ceph_make_fpos(unsigned high, unsigned off, bool hash_order)
{
	loff_t fpos = ((loff_t)high << 28) | (loff_t)off;
	if (hash_order)
		fpos |= HASH_ORDER;
	return fpos;
}

static bool is_hash_order(loff_t p)
{
	return (p & HASH_ORDER) == HASH_ORDER;
}

static unsigned fpos_frag(loff_t p)
{
	return p >> OFFSET_BITS;
}

static unsigned fpos_hash(loff_t p)
{
	return ceph_frag_value(fpos_frag(p));
}

static unsigned fpos_off(loff_t p)
{
	return p & OFFSET_MASK;
}

static int fpos_cmp(loff_t l, loff_t r)
{
	int v = ceph_frag_compare(fpos_frag(l), fpos_frag(r));
	if (v)
		return v;
	return (int)(fpos_off(l) - fpos_off(r));
}

/*
 * make note of the last dentry we read, so we can
 * continue at the same lexicographical point,
 * regardless of what dir changes take place on the
 * server.
 */
static int note_last_dentry(struct ceph_fs_client *fsc,
			    struct ceph_dir_file_info *dfi,
			    const char *name,
		            int len, unsigned next_offset)
{
	char *buf = kmalloc(len+1, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;
	kfree(dfi->last_name);
	dfi->last_name = buf;
	memcpy(dfi->last_name, name, len);
	dfi->last_name[len] = 0;
	dfi->next_offset = next_offset;
	doutc(fsc->client, "'%s'\n", dfi->last_name);
	return 0;
}


static struct dentry *
__dcache_find_get_entry(struct dentry *parent, u64 idx,
			struct ceph_readdir_cache_control *cache_ctl)
{
	struct inode *dir = d_inode(parent);
	struct ceph_client *cl = ceph_inode_to_client(dir);
	struct dentry *dentry;
	unsigned idx_mask = (PAGE_SIZE / sizeof(struct dentry *)) - 1;
	loff_t ptr_pos = idx * sizeof(struct dentry *);
	pgoff_t ptr_pgoff = ptr_pos >> PAGE_SHIFT;

	if (ptr_pos >= i_size_read(dir))
		return NULL;

	if (!cache_ctl->folio || ptr_pgoff != cache_ctl->folio->index) {
		ceph_readdir_cache_release(cache_ctl);
		cache_ctl->folio = filemap_lock_folio(&dir->i_data, ptr_pgoff);
		if (IS_ERR(cache_ctl->folio)) {
			cache_ctl->folio = NULL;
			doutc(cl, " folio %lu not found\n", ptr_pgoff);
			return ERR_PTR(-EAGAIN);
		}
		/* reading/filling the cache are serialized by
		   i_rwsem, no need to use folio lock */
		folio_unlock(cache_ctl->folio);
		cache_ctl->dentries = kmap_local_folio(cache_ctl->folio, 0);
	}

	cache_ctl->index = idx & idx_mask;

	rcu_read_lock();
	spin_lock(&parent->d_lock);
	/* check i_size again here, because empty directory can be
	 * marked as complete while not holding the i_rwsem. */
	if (ceph_dir_is_complete_ordered(dir) && ptr_pos < i_size_read(dir))
		dentry = cache_ctl->dentries[cache_ctl->index];
	else
		dentry = NULL;
	spin_unlock(&parent->d_lock);
	if (dentry && !lockref_get_not_dead(&dentry->d_lockref))
		dentry = NULL;
	rcu_read_unlock();
	return dentry ? : ERR_PTR(-EAGAIN);
}

/*
 * When possible, we try to satisfy a readdir by peeking at the
 * dcache.  We make this work by carefully ordering dentries on
 * d_children when we initially get results back from the MDS, and
 * falling back to a "normal" sync readdir if any dentries in the dir
 * are dropped.
 *
 * Complete dir indicates that we have all dentries in the dir.  It is
 * defined IFF we hold CEPH_CAP_FILE_SHARED (which will be revoked by
 * the MDS if/when the directory is modified).
 */
static int __dcache_readdir(struct file *file,  struct dir_context *ctx,
			    int shared_gen)
{
	struct ceph_dir_file_info *dfi = file->private_data;
	struct dentry *parent = file->f_path.dentry;
	struct inode *dir = d_inode(parent);
	struct ceph_fs_client *fsc = ceph_inode_to_fs_client(dir);
	struct ceph_client *cl = ceph_inode_to_client(dir);
	struct dentry *dentry, *last = NULL;
	struct ceph_dentry_info *di;
	struct ceph_readdir_cache_control cache_ctl = {};
	u64 idx = 0;
	int err = 0;

	doutc(cl, "%p %llx.%llx v%u at %llx\n", dir, ceph_vinop(dir),
	      (unsigned)shared_gen, ctx->pos);

	/* search start position */
	if (ctx->pos > 2) {
		u64 count = div_u64(i_size_read(dir), sizeof(struct dentry *));
		while (count > 0) {
			u64 step = count >> 1;
			dentry = __dcache_find_get_entry(parent, idx + step,
							 &cache_ctl);
			if (!dentry) {
				/* use linear search */
				idx = 0;
				break;
			}
			if (IS_ERR(dentry)) {
				err = PTR_ERR(dentry);
				goto out;
			}
			di = ceph_dentry(dentry);
			spin_lock(&dentry->d_lock);
			if (fpos_cmp(di->offset, ctx->pos) < 0) {
				idx += step + 1;
				count -= step + 1;
			} else {
				count = step;
			}
			spin_unlock(&dentry->d_lock);
			dput(dentry);
		}

		doutc(cl, "%p %llx.%llx cache idx %llu\n", dir,
		      ceph_vinop(dir), idx);
	}


	for (;;) {
		bool emit_dentry = false;
		dentry = __dcache_find_get_entry(parent, idx++, &cache_ctl);
		if (!dentry) {
			dfi->file_info.flags |= CEPH_F_ATEND;
			err = 0;
			break;
		}
		if (IS_ERR(dentry)) {
			err = PTR_ERR(dentry);
			goto out;
		}

		spin_lock(&dentry->d_lock);
		di = ceph_dentry(dentry);
		if (d_unhashed(dentry) ||
		    d_really_is_negative(dentry) ||
		    di->lease_shared_gen != shared_gen ||
		    ((dentry->d_flags & DCACHE_NOKEY_NAME) &&
		     fscrypt_has_encryption_key(dir))) {
			spin_unlock(&dentry->d_lock);
			dput(dentry);
			err = -EAGAIN;
			goto out;
		}
		if (fpos_cmp(ctx->pos, di->offset) <= 0) {
			__ceph_dentry_dir_lease_touch(di);
			emit_dentry = true;
		}
		spin_unlock(&dentry->d_lock);

		if (emit_dentry) {
			doutc(cl, " %llx dentry %p %pd %p\n", di->offset,
			      dentry, dentry, d_inode(dentry));
			ctx->pos = di->offset;
			if (!dir_emit(ctx, dentry->d_name.name,
				      dentry->d_name.len, ceph_present_inode(d_inode(dentry)),
				      d_inode(dentry)->i_mode >> 12)) {
				dput(dentry);
				err = 0;
				break;
			}
			ctx->pos++;

			if (last)
				dput(last);
			last = dentry;
		} else {
			dput(dentry);
		}
	}
out:
	ceph_readdir_cache_release(&cache_ctl);
	if (last) {
		int ret;
		di = ceph_dentry(last);
		ret = note_last_dentry(fsc, dfi, last->d_name.name,
				       last->d_name.len,
				       fpos_off(di->offset) + 1);
		if (ret < 0)
			err = ret;
		dput(last);
		/* last_name no longer match cache index */
		if (dfi->readdir_cache_idx >= 0) {
			dfi->readdir_cache_idx = -1;
			dfi->dir_release_count = 0;
		}
	}
	return err;
}

static bool need_send_readdir(struct ceph_dir_file_info *dfi, loff_t pos)
{
	if (!dfi->last_readdir)
		return true;
	if (is_hash_order(pos))
		return !ceph_frag_contains_value(dfi->frag, fpos_hash(pos));
	else
		return dfi->frag != fpos_frag(pos);
}

static int ceph_readdir(struct file *file, struct dir_context *ctx)
{
	struct ceph_dir_file_info *dfi = file->private_data;
	struct inode *inode = file_inode(file);
	struct ceph_inode_info *ci = ceph_inode(inode);
	struct ceph_fs_client *fsc = ceph_inode_to_fs_client(inode);
	struct ceph_mds_client *mdsc = fsc->mdsc;
	struct ceph_client *cl = fsc->client;
	int i;
	int err;
	unsigned frag = -1;
	struct ceph_mds_reply_info_parsed *rinfo;

	doutc(cl, "%p %llx.%llx file %p pos %llx\n", inode,
	      ceph_vinop(inode), file, ctx->pos);
	if (dfi->file_info.flags & CEPH_F_ATEND)
		return 0;

	/* always start with . and .. */
	if (ctx->pos == 0) {
		doutc(cl, "%p %llx.%llx off 0 -> '.'\n", inode,
		      ceph_vinop(inode));
		if (!dir_emit(ctx, ".", 1, ceph_present_inode(inode),
			    inode->i_mode >> 12))
			return 0;
		ctx->pos = 1;
	}
	if (ctx->pos == 1) {
		u64 ino;
		struct dentry *dentry = file->f_path.dentry;

		spin_lock(&dentry->d_lock);
		ino = ceph_present_inode(dentry->d_parent->d_inode);
		spin_unlock(&dentry->d_lock);

		doutc(cl, "%p %llx.%llx off 1 -> '..'\n", inode,
		      ceph_vinop(inode));
		if (!dir_emit(ctx, "..", 2, ino, inode->i_mode >> 12))
			return 0;
		ctx->pos = 2;
	}

	err = ceph_fscrypt_prepare_readdir(inode);
	if (err < 0)
		return err;

	spin_lock(&ci->i_ceph_lock);
	/* request Fx cap. if have Fx, we don't need to release Fs cap
	 * for later create/unlink. */
	__ceph_touch_fmode(ci, mdsc, CEPH_FILE_MODE_WR);
	/* can we use the dcache? */
	if (ceph_test_mount_opt(fsc, DCACHE) &&
	    !ceph_test_mount_opt(fsc, NOASYNCREADDIR) &&
	    ceph_snap(inode) != CEPH_SNAPDIR &&
	    __ceph_dir_is_complete_ordered(ci) &&
	    __ceph_caps_issued_mask_metric(ci, CEPH_CAP_FILE_SHARED, 1)) {
		int shared_gen = atomic_read(&ci->i_shared_gen);

		spin_unlock(&ci->i_ceph_lock);
		err = __dcache_readdir(file, ctx, shared_gen);
		if (err != -EAGAIN)
			return err;
	} else {
		spin_unlock(&ci->i_ceph_lock);
	}

	/* proceed with a normal readdir */
more:
	/* do we have the correct frag content buffered? */
	if (need_send_readdir(dfi, ctx->pos)) {
		struct ceph_mds_request *req;
		int op = ceph_snap(inode) == CEPH_SNAPDIR ?
			CEPH_MDS_OP_LSSNAP : CEPH_MDS_OP_READDIR;

		/* discard old result, if any */
		if (dfi->last_readdir) {
			ceph_mdsc_put_request(dfi->last_readdir);
			dfi->last_readdir = NULL;
		}

		if (is_hash_order(ctx->pos)) {
			/* fragtree isn't always accurate. choose frag
			 * based on previous reply when possible. */
			if (frag == (unsigned)-1)
				frag = ceph_choose_frag(ci, fpos_hash(ctx->pos),
							NULL, NULL);
		} else {
			frag = fpos_frag(ctx->pos);
		}

		doutc(cl, "fetching %p %llx.%llx frag %x offset '%s'\n",
		      inode, ceph_vinop(inode), frag, dfi->last_name);
		req = ceph_mdsc_create_request(mdsc, op, USE_AUTH_MDS);
		if (IS_ERR(req))
			return PTR_ERR(req);

		err = ceph_alloc_readdir_reply_buffer(req, inode);
		if (err) {
			ceph_mdsc_put_request(req);
			return err;
		}
		/* hints to request -> mds selection code */
		req->r_direct_mode = USE_AUTH_MDS;
		if (op == CEPH_MDS_OP_READDIR) {
			req->r_direct_hash = ceph_frag_value(frag);
			__set_bit(CEPH_MDS_R_DIRECT_IS_HASH, &req->r_req_flags);
			req->r_inode_drop = CEPH_CAP_FILE_EXCL;
		}
		if (dfi->last_name) {
			int len = strlen(dfi->last_name);

			req->r_path2 = kzalloc(NAME_MAX + 1, GFP_KERNEL);
			if (!req->r_path2) {
				ceph_mdsc_put_request(req);
				return -ENOMEM;
			}
			memcpy(req->r_path2, dfi->last_name, len);

			err = ceph_encode_encrypted_dname(inode, req->r_path2, len);
			if (err < 0) {
				ceph_mdsc_put_request(req);
				return err;
			}
		} else if (is_hash_order(ctx->pos)) {
			req->r_args.readdir.offset_hash =
				cpu_to_le32(fpos_hash(ctx->pos));
		}

		req->r_dir_release_cnt = dfi->dir_release_count;
		req->r_dir_ordered_cnt = dfi->dir_ordered_count;
		req->r_readdir_cache_idx = dfi->readdir_cache_idx;
		req->r_readdir_offset = dfi->next_offset;
		req->r_args.readdir.frag = cpu_to_le32(frag);
		req->r_args.readdir.flags =
				cpu_to_le16(CEPH_READDIR_REPLY_BITFLAGS);

		req->r_inode = inode;
		ihold(inode);
		req->r_dentry = dget(file->f_path.dentry);
		err = ceph_mdsc_do_request(mdsc, NULL, req);
		if (err < 0) {
			ceph_mdsc_put_request(req);
			return err;
		}
		doutc(cl, "%p %llx.%llx got and parsed readdir result=%d"
		      "on frag %x, end=%d, complete=%d, hash_order=%d\n",
		      inode, ceph_vinop(inode), err, frag,
		      (int)req->r_reply_info.dir_end,
		      (int)req->r_reply_info.dir_complete,
		      (int)req->r_reply_info.hash_order);

		rinfo = &req->r_reply_info;
		if (le32_to_cpu(rinfo->dir_dir->frag) != frag) {
			frag = le32_to_cpu(rinfo->dir_dir->frag);
			if (!rinfo->hash_order) {
				dfi->next_offset = req->r_readdir_offset;
				/* adjust ctx->pos to beginning of frag */
				ctx->pos = ceph_make_fpos(frag,
							  dfi->next_offset,
							  false);
			}
		}

		dfi->frag = frag;
		dfi->last_readdir = req;

		if (test_bit(CEPH_MDS_R_DID_PREPOPULATE, &req->r_req_flags)) {
			dfi->readdir_cache_idx = req->r_readdir_cache_idx;
			if (dfi->readdir_cache_idx < 0) {
				/* preclude from marking dir ordered */
				dfi->dir_ordered_count = 0;
			} else if (ceph_frag_is_leftmost(frag) &&
				   dfi->next_offset == 2) {
				/* note dir version at start of readdir so
				 * we can tell if any dentries get dropped */
				dfi->dir_release_count = req->r_dir_release_cnt;
				dfi->dir_ordered_count = req->r_dir_ordered_cnt;
			}
		} else {
			doutc(cl, "%p %llx.%llx !did_prepopulate\n", inode,
			      ceph_vinop(inode));
			/* disable readdir cache */
			dfi->readdir_cache_idx = -1;
			/* preclude from marking dir complete */
			dfi->dir_release_count = 0;
		}

		/* note next offset and last dentry name */
		if (rinfo->dir_nr > 0) {
			struct ceph_mds_reply_dir_entry *rde =
					rinfo->dir_entries + (rinfo->dir_nr-1);
			unsigned next_offset = req->r_reply_info.dir_end ?
					2 : (fpos_off(rde->offset) + 1);
			err = note_last_dentry(fsc, dfi, rde->name,
					       rde->name_len, next_offset);
			if (err) {
				ceph_mdsc_put_request(dfi->last_readdir);
				dfi->last_readdir = NULL;
				return err;
			}
		} else if (req->r_reply_info.dir_end) {
			dfi->next_offset = 2;
			/* keep last name */
		}
	}

	rinfo = &dfi->last_readdir->r_reply_info;
	doutc(cl, "%p %llx.%llx frag %x num %d pos %llx chunk first %llx\n",
	      inode, ceph_vinop(inode), dfi->frag, rinfo->dir_nr, ctx->pos,
	      rinfo->dir_nr ? rinfo->dir_entries[0].offset : 0LL);

	i = 0;
	/* search start position */
	if (rinfo->dir_nr > 0) {
		int step, nr = rinfo->dir_nr;
		while (nr > 0) {
			step = nr >> 1;
			if (rinfo->dir_entries[i + step].offset < ctx->pos) {
				i +=  step + 1;
				nr -= step + 1;
			} else {
				nr = step;
			}
		}
	}
	for (; i < rinfo->dir_nr; i++) {
		struct ceph_mds_reply_dir_entry *rde = rinfo->dir_entries + i;

		if (rde->offset < ctx->pos) {
			pr_warn_client(cl,
				"%p %llx.%llx rde->offset 0x%llx ctx->pos 0x%llx\n",
				inode, ceph_vinop(inode), rde->offset, ctx->pos);
			return -EIO;
		}

		if (WARN_ON_ONCE(!rde->inode.in))
			return -EIO;

		ctx->pos = rde->offset;
		doutc(cl, "%p %llx.%llx (%d/%d) -> %llx '%.*s' %p\n", inode,
		      ceph_vinop(inode), i, rinfo->dir_nr, ctx->pos,
		      rde->name_len, rde->name, &rde->inode.in);

		if (!dir_emit(ctx, rde->name, rde->name_len,
			      ceph_present_ino(inode->i_sb, le64_to_cpu(rde->inode.in->ino)),
			      le32_to_cpu(rde->inode.in->mode) >> 12)) {
			/*
			 * NOTE: Here no need to put the 'dfi->last_readdir',
			 * because when dir_emit stops us it's most likely
			 * doesn't have enough memory, etc. So for next readdir
			 * it will continue.
			 */
			doutc(cl, "filldir stopping us...\n");
			return 0;
		}

		/* Reset the lengths to their original allocated vals */
		ctx->pos++;
	}

	ceph_mdsc_put_request(dfi->last_readdir);
	dfi->last_readdir = NULL;

	if (dfi->next_offset > 2) {
		frag = dfi->frag;
		goto more;
	}

	/* more frags? */
	if (!ceph_frag_is_rightmost(dfi->frag)) {
		frag = ceph_frag_next(dfi->frag);
		if (is_hash_order(ctx->pos)) {
			loff_t new_pos = ceph_make_fpos(ceph_frag_value(frag),
							dfi->next_offset, true);
			if (new_pos > ctx->pos)
				ctx->pos = new_pos;
			/* keep last_name */
		} else {
			ctx->pos = ceph_make_fpos(frag, dfi->next_offset,
							false);
			kfree(dfi->last_name);
			dfi->last_name = NULL;
		}
		doutc(cl, "%p %llx.%llx next frag is %x\n", inode,
		      ceph_vinop(inode), frag);
		goto more;
	}
	dfi->file_info.flags |= CEPH_F_ATEND;

	/*
	 * if dir_release_count still matches the dir, no dentries
	 * were released during the whole readdir, and we should have
	 * the complete dir contents in our cache.
	 */
	if (atomic64_read(&ci->i_release_count) ==
					dfi->dir_release_count) {
		spin_lock(&ci->i_ceph_lock);
		if (dfi->dir_ordered_count ==
				atomic64_read(&ci->i_ordered_count)) {
			doutc(cl, " marking %p %llx.%llx complete and ordered\n",
			      inode, ceph_vinop(inode));
			/* use i_size to track number of entries in
			 * readdir cache */
			BUG_ON(dfi->readdir_cache_idx < 0);
			i_size_write(inode, dfi->readdir_cache_idx *
				     sizeof(struct dentry*));
		} else {
			doutc(cl, " marking %llx.%llx complete\n",
			      ceph_vinop(inode));
		}
		__ceph_dir_set_complete(ci, dfi->dir_release_count,
					dfi->dir_ordered_count);
		spin_unlock(&ci->i_ceph_lock);
	}
	doutc(cl, "%p %llx.%llx file %p done.\n", inode, ceph_vinop(inode),
	      file);
	return 0;
}

static void reset_readdir(struct ceph_dir_file_info *dfi)
{
	if (dfi->last_readdir) {
		ceph_mdsc_put_request(dfi->last_readdir);
		dfi->last_readdir = NULL;
	}
	kfree(dfi->last_name);
	dfi->last_name = NULL;
	dfi->dir_release_count = 0;
	dfi->readdir_cache_idx = -1;
	dfi->next_offset = 2;  /* compensate for . and .. */
	dfi->file_info.flags &= ~CEPH_F_ATEND;
}

/*
 * discard buffered readdir content on seekdir(0), or seek to new frag,
 * or seek prior to current chunk
 */
static bool need_reset_readdir(struct ceph_dir_file_info *dfi, loff_t new_pos)
{
	struct ceph_mds_reply_info_parsed *rinfo;
	loff_t chunk_offset;
	if (new_pos == 0)
		return true;
	if (is_hash_order(new_pos)) {
		/* no need to reset last_name for a forward seek when
		 * dentries are sorted in hash order */
	} else if (dfi->frag != fpos_frag(new_pos)) {
		return true;
	}
	rinfo = dfi->last_readdir ? &dfi->last_readdir->r_reply_info : NULL;
	if (!rinfo || !rinfo->dir_nr)
		return true;
	chunk_offset = rinfo->dir_entries[0].offset;
	return new_pos < chunk_offset ||
	       is_hash_order(new_pos) != is_hash_order(chunk_offset);
}

static loff_t ceph_dir_llseek(struct file *file, loff_t offset, int whence)
{
	struct ceph_dir_file_info *dfi = file->private_data;
	struct inode *inode = file->f_mapping->host;
	struct ceph_client *cl = ceph_inode_to_client(inode);
	loff_t retval;

	inode_lock(inode);
	retval = -EINVAL;
	switch (whence) {
	case SEEK_CUR:
		offset += file->f_pos;
		break;
	case SEEK_SET:
		break;
	case SEEK_END:
		retval = -EOPNOTSUPP;
		goto out;
	default:
		goto out;
	}

	if (offset >= 0) {
		if (need_reset_readdir(dfi, offset)) {
			doutc(cl, "%p %llx.%llx dropping %p content\n",
			      inode, ceph_vinop(inode), file);
			reset_readdir(dfi);
		} else if (is_hash_order(offset) && offset > file->f_pos) {
			/* for hash offset, we don't know if a forward seek
			 * is within same frag */
			dfi->dir_release_count = 0;
			dfi->readdir_cache_idx = -1;
		}

		if (offset != file->f_pos) {
			file->f_pos = offset;
			dfi->file_info.flags &= ~CEPH_F_ATEND;
		}
		retval = offset;
	}
out:
	inode_unlock(inode);
	return retval;
}

/*
 * Handle lookups for the hidden .snap directory.
 */
struct dentry *ceph_handle_snapdir(struct ceph_mds_request *req,
				   struct dentry *dentry)
{
	struct ceph_fs_client *fsc = ceph_sb_to_fs_client(dentry->d_sb);
	struct inode *parent = d_inode(dentry->d_parent); /* we hold i_rwsem */
	struct ceph_client *cl = ceph_inode_to_client(parent);

	/* .snap dir? */
	if (ceph_snap(parent) == CEPH_NOSNAP &&
	    strcmp(dentry->d_name.name, fsc->mount_options->snapdir_name) == 0) {
		struct dentry *res;
		struct inode *inode = ceph_get_snapdir(parent);

		res = d_splice_alias(inode, dentry);
		doutc(cl, "ENOENT on snapdir %p '%pd', linking to "
		      "snapdir %p %llx.%llx. Spliced dentry %p\n",
		      dentry, dentry, inode, ceph_vinop(inode), res);
		if (res)
			dentry = res;
	}
	return dentry;
}

/*
 * Figure out final result of a lookup/open request.
 *
 * Mainly, make sure we return the final req->r_dentry (if it already
 * existed) in place of the original VFS-provided dentry when they
 * differ.
 *
 * Gracefully handle the case where the MDS replies with -ENOENT and
 * no trace (which it may do, at its discretion, e.g., if it doesn't
 * care to issue a lease on the negative dentry).
 */
struct dentry *ceph_finish_lookup(struct ceph_mds_request *req,
				  struct dentry *dentry, int err)
{
	struct ceph_client *cl = req->r_mdsc->fsc->client;

	if (err == -ENOENT) {
		/* no trace? */
		err = 0;
		if (!req->r_reply_info.head->is_dentry) {
			doutc(cl,
			      "ENOENT and no trace, dentry %p inode %llx.%llx\n",
			      dentry, ceph_vinop(d_inode(dentry)));
			if (d_really_is_positive(dentry)) {
				d_drop(dentry);
				err = -ENOENT;
			} else {
				d_add(dentry, NULL);
			}
		}
	}
	if (err)
		dentry = ERR_PTR(err);
	else if (dentry != req->r_dentry)
		dentry = dget(req->r_dentry);   /* we got spliced */
	else
		dentry = NULL;
	return dentry;
}

static bool is_root_ceph_dentry(struct inode *inode, struct dentry *dentry)
{
	return ceph_ino(inode) == CEPH_INO_ROOT &&
		strncmp(dentry->d_name.name, ".ceph", 5) == 0;
}

/*
 * Look up a single dir entry.  If there is a lookup intent, inform
 * the MDS so that it gets our 'caps wanted' value in a single op.
 */
static struct dentry *ceph_lookup(struct inode *dir, struct dentry *dentry,
				  unsigned int flags)
{
	struct ceph_fs_client *fsc = ceph_sb_to_fs_client(dir->i_sb);
	struct ceph_mds_client *mdsc = ceph_sb_to_mdsc(dir->i_sb);
	struct ceph_client *cl = fsc->client;
	struct ceph_mds_request *req;
	int op;
	int mask;
	int err;

	doutc(cl, "%p %llx.%llx/'%pd' dentry %p\n", dir, ceph_vinop(dir),
	      dentry, dentry);

	if (dentry->d_name.len > NAME_MAX)
		return ERR_PTR(-ENAMETOOLONG);

	if (IS_ENCRYPTED(dir)) {
		bool had_key = fscrypt_has_encryption_key(dir);

		err = fscrypt_prepare_lookup_partial(dir, dentry);
		if (err < 0)
			return ERR_PTR(err);

		/* mark directory as incomplete if it has been unlocked */
		if (!had_key && fscrypt_has_encryption_key(dir))
			ceph_dir_clear_complete(dir);
	}

	/* can we conclude ENOENT locally? */
	if (d_really_is_negative(dentry)) {
		struct ceph_inode_info *ci = ceph_inode(dir);
		struct ceph_dentry_info *di = ceph_dentry(dentry);

		spin_lock(&ci->i_ceph_lock);
		doutc(cl, " dir %llx.%llx flags are 0x%lx\n",
		      ceph_vinop(dir), ci->i_ceph_flags);
		if (strncmp(dentry->d_name.name,
			    fsc->mount_options->snapdir_name,
			    dentry->d_name.len) &&
		    !is_root_ceph_dentry(dir, dentry) &&
		    ceph_test_mount_opt(fsc, DCACHE) &&
		    __ceph_dir_is_complete(ci) &&
		    __ceph_caps_issued_mask_metric(ci, CEPH_CAP_FILE_SHARED, 1)) {
			__ceph_touch_fmode(ci, mdsc, CEPH_FILE_MODE_RD);
			spin_unlock(&ci->i_ceph_lock);
			doutc(cl, " dir %llx.%llx complete, -ENOENT\n",
			      ceph_vinop(dir));
			d_add(dentry, NULL);
			di->lease_shared_gen = atomic_read(&ci->i_shared_gen);
			return NULL;
		}
		spin_unlock(&ci->i_ceph_lock);
	}

	op = ceph_snap(dir) == CEPH_SNAPDIR ?
		CEPH_MDS_OP_LOOKUPSNAP : CEPH_MDS_OP_LOOKUP;
	req = ceph_mdsc_create_request(mdsc, op, USE_ANY_MDS);
	if (IS_ERR(req))
		return ERR_CAST(req);
	req->r_dentry = dget(dentry);
	req->r_num_caps = 2;

	mask = CEPH_STAT_CAP_INODE | CEPH_CAP_AUTH_SHARED;
	if (ceph_security_xattr_wanted(dir))
		mask |= CEPH_CAP_XATTR_SHARED;
	req->r_args.getattr.mask = cpu_to_le32(mask);

	ihold(dir);
	req->r_parent = dir;
	set_bit(CEPH_MDS_R_PARENT_LOCKED, &req->r_req_flags);
	err = ceph_mdsc_do_request(mdsc, NULL, req);
	if (err == -ENOENT) {
		struct dentry *res;

		res = ceph_handle_snapdir(req, dentry);
		if (IS_ERR(res)) {
			err = PTR_ERR(res);
		} else {
			dentry = res;
			err = 0;
		}
	}
	dentry = ceph_finish_lookup(req, dentry, err);
	ceph_mdsc_put_request(req);  /* will dput(dentry) */
	doutc(cl, "result=%p\n", dentry);
	return dentry;
}

/*
 * If we do a create but get no trace back from the MDS, follow up with
 * a lookup (the VFS expects us to link up the provided dentry).
 */
int ceph_handle_notrace_create(struct inode *dir, struct dentry *dentry)
{
	struct dentry *result = ceph_lookup(dir, dentry, 0);

	if (result && !IS_ERR(result)) {
		/*
		 * We created the item, then did a lookup, and found
		 * it was already linked to another inode we already
		 * had in our cache (and thus got spliced). To not
		 * confuse VFS (especially when inode is a directory),
		 * we don't link our dentry to that inode, return an
		 * error instead.
		 *
		 * This event should be rare and it happens only when
		 * we talk to old MDS. Recent MDS does not send traceless
		 * reply for request that creates new inode.
		 */
		d_drop(result);
		return -ESTALE;
	}
	return PTR_ERR(result);
}

static int ceph_mknod(struct mnt_idmap *idmap, struct inode *dir,
		      struct dentry *dentry, umode_t mode, dev_t rdev)
{
	struct ceph_mds_client *mdsc = ceph_sb_to_mdsc(dir->i_sb);
	struct ceph_client *cl = mdsc->fsc->client;
	struct ceph_mds_request *req;
	struct ceph_acl_sec_ctx as_ctx = {};
	int err;

	if (ceph_snap(dir) != CEPH_NOSNAP)
		return -EROFS;

	err = ceph_wait_on_conflict_unlink(dentry);
	if (err)
		return err;

	if (ceph_quota_is_max_files_exceeded(dir)) {
		err = -EDQUOT;
		goto out;
	}

	doutc(cl, "%p %llx.%llx/'%pd' dentry %p mode 0%ho rdev %d\n",
	      dir, ceph_vinop(dir), dentry, dentry, mode, rdev);
	req = ceph_mdsc_create_request(mdsc, CEPH_MDS_OP_MKNOD, USE_AUTH_MDS);
	if (IS_ERR(req)) {
		err = PTR_ERR(req);
		goto out;
	}

	req->r_new_inode = ceph_new_inode(dir, dentry, &mode, &as_ctx);
	if (IS_ERR(req->r_new_inode)) {
		err = PTR_ERR(req->r_new_inode);
		req->r_new_inode = NULL;
		goto out_req;
	}

	if (S_ISREG(mode) && IS_ENCRYPTED(dir))
		set_bit(CEPH_MDS_R_FSCRYPT_FILE, &req->r_req_flags);

	req->r_dentry = dget(dentry);
	req->r_num_caps = 2;
	req->r_parent = dir;
	ihold(dir);
	set_bit(CEPH_MDS_R_PARENT_LOCKED, &req->r_req_flags);
	req->r_mnt_idmap = mnt_idmap_get(idmap);
	req->r_args.mknod.mode = cpu_to_le32(mode);
	req->r_args.mknod.rdev = cpu_to_le32(rdev);
	req->r_dentry_drop = CEPH_CAP_FILE_SHARED | CEPH_CAP_AUTH_EXCL |
			     CEPH_CAP_XATTR_EXCL;
	req->r_dentry_unless = CEPH_CAP_FILE_EXCL;

	ceph_as_ctx_to_req(req, &as_ctx);

	err = ceph_mdsc_do_request(mdsc, dir, req);
	if (!err && !req->r_reply_info.head->is_dentry)
		err = ceph_handle_notrace_create(dir, dentry);
out_req:
	ceph_mdsc_put_request(req);
out:
	if (!err)
		ceph_init_inode_acls(d_inode(dentry), &as_ctx);
	else
		d_drop(dentry);
	ceph_release_acl_sec_ctx(&as_ctx);
	return err;
}

static int ceph_create(struct mnt_idmap *idmap, struct inode *dir,
		       struct dentry *dentry, umode_t mode, bool excl)
{
	return ceph_mknod(idmap, dir, dentry, mode, 0);
}

#if IS_ENABLED(CONFIG_FS_ENCRYPTION)
static int prep_encrypted_symlink_target(struct ceph_mds_request *req,
					 const char *dest)
{
	int err;
	int len = strlen(dest);
	struct fscrypt_str osd_link = FSTR_INIT(NULL, 0);

	err = fscrypt_prepare_symlink(req->r_parent, dest, len, PATH_MAX,
				      &osd_link);
	if (err)
		goto out;

	err = fscrypt_encrypt_symlink(req->r_new_inode, dest, len, &osd_link);
	if (err)
		goto out;

	req->r_path2 = kmalloc(CEPH_BASE64_CHARS(osd_link.len) + 1, GFP_KERNEL);
	if (!req->r_path2) {
		err = -ENOMEM;
		goto out;
	}

	len = ceph_base64_encode(osd_link.name, osd_link.len, req->r_path2);
	req->r_path2[len] = '\0';
out:
	fscrypt_fname_free_buffer(&osd_link);
	return err;
}
#else
static int prep_encrypted_symlink_target(struct ceph_mds_request *req,
					 const char *dest)
{
	return -EOPNOTSUPP;
}
#endif

static int ceph_symlink(struct mnt_idmap *idmap, struct inode *dir,
			struct dentry *dentry, const char *dest)
{
	struct ceph_mds_client *mdsc = ceph_sb_to_mdsc(dir->i_sb);
	struct ceph_client *cl = mdsc->fsc->client;
	struct ceph_mds_request *req;
	struct ceph_acl_sec_ctx as_ctx = {};
	umode_t mode = S_IFLNK | 0777;
	int err;

	if (ceph_snap(dir) != CEPH_NOSNAP)
		return -EROFS;

	err = ceph_wait_on_conflict_unlink(dentry);
	if (err)
		return err;

	if (ceph_quota_is_max_files_exceeded(dir)) {
		err = -EDQUOT;
		goto out;
	}

	doutc(cl, "%p %llx.%llx/'%pd' to '%s'\n", dir, ceph_vinop(dir), dentry,
	      dest);
	req = ceph_mdsc_create_request(mdsc, CEPH_MDS_OP_SYMLINK, USE_AUTH_MDS);
	if (IS_ERR(req)) {
		err = PTR_ERR(req);
		goto out;
	}

	req->r_new_inode = ceph_new_inode(dir, dentry, &mode, &as_ctx);
	if (IS_ERR(req->r_new_inode)) {
		err = PTR_ERR(req->r_new_inode);
		req->r_new_inode = NULL;
		goto out_req;
	}

	req->r_parent = dir;
	ihold(dir);

	if (IS_ENCRYPTED(req->r_new_inode)) {
		err = prep_encrypted_symlink_target(req, dest);
		if (err)
			goto out_req;
	} else {
		req->r_path2 = kstrdup(dest, GFP_KERNEL);
		if (!req->r_path2) {
			err = -ENOMEM;
			goto out_req;
		}
	}

	set_bit(CEPH_MDS_R_PARENT_LOCKED, &req->r_req_flags);
	req->r_mnt_idmap = mnt_idmap_get(idmap);
	req->r_dentry = dget(dentry);
	req->r_num_caps = 2;
	req->r_dentry_drop = CEPH_CAP_FILE_SHARED | CEPH_CAP_AUTH_EXCL |
			     CEPH_CAP_XATTR_EXCL;
	req->r_dentry_unless = CEPH_CAP_FILE_EXCL;

	ceph_as_ctx_to_req(req, &as_ctx);

	err = ceph_mdsc_do_request(mdsc, dir, req);
	if (!err && !req->r_reply_info.head->is_dentry)
		err = ceph_handle_notrace_create(dir, dentry);
out_req:
	ceph_mdsc_put_request(req);
out:
	if (err)
		d_drop(dentry);
	ceph_release_acl_sec_ctx(&as_ctx);
	return err;
}

static struct dentry *ceph_mkdir(struct mnt_idmap *idmap, struct inode *dir,
				 struct dentry *dentry, umode_t mode)
{
	struct ceph_mds_client *mdsc = ceph_sb_to_mdsc(dir->i_sb);
	struct ceph_client *cl = mdsc->fsc->client;
	struct ceph_mds_request *req;
	struct ceph_acl_sec_ctx as_ctx = {};
	struct dentry *ret;
	int err;
	int op;

	err = ceph_wait_on_conflict_unlink(dentry);
	if (err)
		return ERR_PTR(err);

	if (ceph_snap(dir) == CEPH_SNAPDIR) {
		/* mkdir .snap/foo is a MKSNAP */
		op = CEPH_MDS_OP_MKSNAP;
		doutc(cl, "mksnap %llx.%llx/'%pd' dentry %p\n",
		      ceph_vinop(dir), dentry, dentry);
	} else if (ceph_snap(dir) == CEPH_NOSNAP) {
		doutc(cl, "mkdir %llx.%llx/'%pd' dentry %p mode 0%ho\n",
		      ceph_vinop(dir), dentry, dentry, mode);
		op = CEPH_MDS_OP_MKDIR;
	} else {
		ret = ERR_PTR(-EROFS);
		goto out;
	}

	if (op == CEPH_MDS_OP_MKDIR &&
	    ceph_quota_is_max_files_exceeded(dir)) {
		ret = ERR_PTR(-EDQUOT);
		goto out;
	}
	if ((op == CEPH_MDS_OP_MKSNAP) && IS_ENCRYPTED(dir) &&
	    !fscrypt_has_encryption_key(dir)) {
		ret = ERR_PTR(-ENOKEY);
		goto out;
	}


	req = ceph_mdsc_create_request(mdsc, op, USE_AUTH_MDS);
	if (IS_ERR(req)) {
		ret = ERR_CAST(req);
		goto out;
	}

	mode |= S_IFDIR;
	req->r_new_inode = ceph_new_inode(dir, dentry, &mode, &as_ctx);
	if (IS_ERR(req->r_new_inode)) {
		ret = ERR_CAST(req->r_new_inode);
		req->r_new_inode = NULL;
		goto out_req;
	}

	req->r_dentry = dget(dentry);
	req->r_num_caps = 2;
	req->r_parent = dir;
	ihold(dir);
	set_bit(CEPH_MDS_R_PARENT_LOCKED, &req->r_req_flags);
	if (op == CEPH_MDS_OP_MKDIR)
		req->r_mnt_idmap = mnt_idmap_get(idmap);
	req->r_args.mkdir.mode = cpu_to_le32(mode);
	req->r_dentry_drop = CEPH_CAP_FILE_SHARED | CEPH_CAP_AUTH_EXCL |
			     CEPH_CAP_XATTR_EXCL;
	req->r_dentry_unless = CEPH_CAP_FILE_EXCL;

	ceph_as_ctx_to_req(req, &as_ctx);

	err = ceph_mdsc_do_request(mdsc, dir, req);
	if (!err &&
	    !req->r_reply_info.head->is_target &&
	    !req->r_reply_info.head->is_dentry)
		err = ceph_handle_notrace_create(dir, dentry);
	ret = ERR_PTR(err);
out_req:
	if (!IS_ERR(ret) && req->r_dentry != dentry)
		/* Some other dentry was spliced in */
		ret = dget(req->r_dentry);
	ceph_mdsc_put_request(req);
out:
	if (!IS_ERR(ret)) {
		if (ret)
			dentry = ret;
		ceph_init_inode_acls(d_inode(dentry), &as_ctx);
	} else {
		d_drop(dentry);
	}
	ceph_release_acl_sec_ctx(&as_ctx);
	return ret;
}

static int ceph_link(struct dentry *old_dentry, struct inode *dir,
		     struct dentry *dentry)
{
	struct ceph_mds_client *mdsc = ceph_sb_to_mdsc(dir->i_sb);
	struct ceph_client *cl = mdsc->fsc->client;
	struct ceph_mds_request *req;
	int err;

	if (dentry->d_flags & DCACHE_DISCONNECTED)
		return -EINVAL;

	err = ceph_wait_on_conflict_unlink(dentry);
	if (err)
		return err;

	if (ceph_snap(dir) != CEPH_NOSNAP)
		return -EROFS;

	err = fscrypt_prepare_link(old_dentry, dir, dentry);
	if (err)
		return err;

	doutc(cl, "%p %llx.%llx/'%pd' to '%pd'\n", dir, ceph_vinop(dir),
	      old_dentry, dentry);
	req = ceph_mdsc_create_request(mdsc, CEPH_MDS_OP_LINK, USE_AUTH_MDS);
	if (IS_ERR(req)) {
		d_drop(dentry);
		return PTR_ERR(req);
	}
	req->r_dentry = dget(dentry);
	req->r_num_caps = 2;
	req->r_old_dentry = dget(old_dentry);
	/*
	 * The old_dentry maybe a DCACHE_DISCONNECTED dentry, then we
	 * will just pass the ino# to MDSs.
	 */
	if (old_dentry->d_flags & DCACHE_DISCONNECTED)
		req->r_ino2 = ceph_vino(d_inode(old_dentry));
	req->r_parent = dir;
	ihold(dir);
	set_bit(CEPH_MDS_R_PARENT_LOCKED, &req->r_req_flags);
	req->r_dentry_drop = CEPH_CAP_FILE_SHARED | CEPH_CAP_XATTR_EXCL;
	req->r_dentry_unless = CEPH_CAP_FILE_EXCL;
	/* release LINK_SHARED on source inode (mds will lock it) */
	req->r_old_inode_drop = CEPH_CAP_LINK_SHARED | CEPH_CAP_LINK_EXCL;
	err = ceph_mdsc_do_request(mdsc, dir, req);
	if (err) {
		d_drop(dentry);
	} else if (!req->r_reply_info.head->is_dentry) {
		ihold(d_inode(old_dentry));
		d_instantiate(dentry, d_inode(old_dentry));
	}
	ceph_mdsc_put_request(req);
	return err;
}

static void ceph_async_unlink_cb(struct ceph_mds_client *mdsc,
				 struct ceph_mds_request *req)
{
	struct dentry *dentry = req->r_dentry;
	struct ceph_fs_client *fsc = ceph_sb_to_fs_client(dentry->d_sb);
	struct ceph_client *cl = fsc->client;
	struct ceph_dentry_info *di = ceph_dentry(dentry);
	int result = req->r_err ? req->r_err :
			le32_to_cpu(req->r_reply_info.head->result);

	if (!test_bit(CEPH_DENTRY_ASYNC_UNLINK_BIT, &di->flags))
		pr_warn_client(cl,
			"dentry %p:%pd async unlink bit is not set\n",
			dentry, dentry);

	spin_lock(&fsc->async_unlink_conflict_lock);
	hash_del_rcu(&di->hnode);
	spin_unlock(&fsc->async_unlink_conflict_lock);

	spin_lock(&dentry->d_lock);
	di->flags &= ~CEPH_DENTRY_ASYNC_UNLINK;
	wake_up_bit(&di->flags, CEPH_DENTRY_ASYNC_UNLINK_BIT);
	spin_unlock(&dentry->d_lock);

	synchronize_rcu();

	if (result == -EJUKEBOX)
		goto out;

	/* If op failed, mark everyone involved for errors */
	if (result) {
		int pathlen = 0;
		u64 base = 0;
		char *path = ceph_mdsc_build_path(mdsc, dentry, &pathlen,
						  &base, 0);

		/* mark error on parent + clear complete */
		mapping_set_error(req->r_parent->i_mapping, result);
		ceph_dir_clear_complete(req->r_parent);

		/* drop the dentry -- we don't know its status */
		if (!d_unhashed(dentry))
			d_drop(dentry);

		/* mark inode itself for an error (since metadata is bogus) */
		mapping_set_error(req->r_old_inode->i_mapping, result);

		pr_warn_client(cl, "failure path=(%llx)%s result=%d!\n",
			       base, IS_ERR(path) ? "<<bad>>" : path, result);
		ceph_mdsc_free_path(path, pathlen);
	}
out:
	iput(req->r_old_inode);
	ceph_mdsc_release_dir_caps(req);
}

static int get_caps_for_async_unlink(struct inode *dir, struct dentry *dentry)
{
	struct ceph_inode_info *ci = ceph_inode(dir);
	struct ceph_dentry_info *di;
	int got = 0, want = CEPH_CAP_FILE_EXCL | CEPH_CAP_DIR_UNLINK;

	spin_lock(&ci->i_ceph_lock);
	if ((__ceph_caps_issued(ci, NULL) & want) == want) {
		ceph_take_cap_refs(ci, want, false);
		got = want;
	}
	spin_unlock(&ci->i_ceph_lock);

	/* If we didn't get anything, return 0 */
	if (!got)
		return 0;

        spin_lock(&dentry->d_lock);
        di = ceph_dentry(dentry);
	/*
	 * - We are holding Fx, which implies Fs caps.
	 * - Only support async unlink for primary linkage
	 */
	if (atomic_read(&ci->i_shared_gen) != di->lease_shared_gen ||
	    !(di->flags & CEPH_DENTRY_PRIMARY_LINK))
		want = 0;
        spin_unlock(&dentry->d_lock);

	/* Do we still want what we've got? */
	if (want == got)
		return got;

	ceph_put_cap_refs(ci, got);
	return 0;
}

/*
 * rmdir and unlink are differ only by the metadata op code
 */
static int ceph_unlink(struct inode *dir, struct dentry *dentry)
{
	struct ceph_fs_client *fsc = ceph_sb_to_fs_client(dir->i_sb);
	struct ceph_client *cl = fsc->client;
	struct ceph_mds_client *mdsc = fsc->mdsc;
	struct inode *inode = d_inode(dentry);
	struct ceph_mds_request *req;
	bool try_async = ceph_test_mount_opt(fsc, ASYNC_DIROPS);
	struct dentry *dn;
	int err = -EROFS;
	int op;
	char *path;
	int pathlen;
	u64 pathbase;

	if (ceph_snap(dir) == CEPH_SNAPDIR) {
		/* rmdir .snap/foo is RMSNAP */
		doutc(cl, "rmsnap %llx.%llx/'%pd' dn\n", ceph_vinop(dir),
		      dentry);
		op = CEPH_MDS_OP_RMSNAP;
	} else if (ceph_snap(dir) == CEPH_NOSNAP) {
		doutc(cl, "unlink/rmdir %llx.%llx/'%pd' inode %llx.%llx\n",
		      ceph_vinop(dir), dentry, ceph_vinop(inode));
		op = d_is_dir(dentry) ?
			CEPH_MDS_OP_RMDIR : CEPH_MDS_OP_UNLINK;
	} else
		goto out;

	dn = d_find_alias(dir);
	if (!dn) {
		try_async = false;
	} else {
		path = ceph_mdsc_build_path(mdsc, dn, &pathlen, &pathbase, 0);
		if (IS_ERR(path)) {
			try_async = false;
			err = 0;
		} else {
			err = ceph_mds_check_access(mdsc, path, MAY_WRITE);
		}
		ceph_mdsc_free_path(path, pathlen);
		dput(dn);

		/* For none EACCES cases will let the MDS do the mds auth check */
		if (err == -EACCES) {
			return err;
		} else if (err < 0) {
			try_async = false;
			err = 0;
		}
	}

retry:
	req = ceph_mdsc_create_request(mdsc, op, USE_AUTH_MDS);
	if (IS_ERR(req)) {
		err = PTR_ERR(req);
		goto out;
	}
	req->r_dentry = dget(dentry);
	req->r_num_caps = 2;
	req->r_parent = dir;
	ihold(dir);
	req->r_dentry_drop = CEPH_CAP_FILE_SHARED | CEPH_CAP_XATTR_EXCL;
	req->r_dentry_unless = CEPH_CAP_FILE_EXCL;
	req->r_inode_drop = ceph_drop_caps_for_unlink(inode);

	if (try_async && op == CEPH_MDS_OP_UNLINK &&
	    (req->r_dir_caps = get_caps_for_async_unlink(dir, dentry))) {
		struct ceph_dentry_info *di = ceph_dentry(dentry);

		doutc(cl, "async unlink on %llx.%llx/'%pd' caps=%s",
		      ceph_vinop(dir), dentry,
		      ceph_cap_string(req->r_dir_caps));
		set_bit(CEPH_MDS_R_ASYNC, &req->r_req_flags);
		req->r_callback = ceph_async_unlink_cb;
		req->r_old_inode = d_inode(dentry);
		ihold(req->r_old_inode);

		spin_lock(&dentry->d_lock);
		di->flags |= CEPH_DENTRY_ASYNC_UNLINK;
		spin_unlock(&dentry->d_lock);

		spin_lock(&fsc->async_unlink_conflict_lock);
		hash_add_rcu(fsc->async_unlink_conflict, &di->hnode,
			     dentry->d_name.hash);
		spin_unlock(&fsc->async_unlink_conflict_lock);

		err = ceph_mdsc_submit_request(mdsc, dir, req);
		if (!err) {
			/*
			 * We have enough caps, so we assume that the unlink
			 * will succeed. Fix up the target inode and dcache.
			 */
			drop_nlink(inode);
			d_delete(dentry);
		} else {
			spin_lock(&fsc->async_unlink_conflict_lock);
			hash_del_rcu(&di->hnode);
			spin_unlock(&fsc->async_unlink_conflict_lock);

			spin_lock(&dentry->d_lock);
			di->flags &= ~CEPH_DENTRY_ASYNC_UNLINK;
			spin_unlock(&dentry->d_lock);

			if (err == -EJUKEBOX) {
				try_async = false;
				ceph_mdsc_put_request(req);
				goto retry;
			}
		}
	} else {
		set_bit(CEPH_MDS_R_PARENT_LOCKED, &req->r_req_flags);
		err = ceph_mdsc_do_request(mdsc, dir, req);
		if (!err && !req->r_reply_info.head->is_dentry)
			d_delete(dentry);
	}

	ceph_mdsc_put_request(req);
out:
	return err;
}

static int ceph_rename(struct mnt_idmap *idmap, struct inode *old_dir,
		       struct dentry *old_dentry, struct inode *new_dir,
		       struct dentry *new_dentry, unsigned int flags)
{
	struct ceph_mds_client *mdsc = ceph_sb_to_mdsc(old_dir->i_sb);
	struct ceph_client *cl = mdsc->fsc->client;
	struct ceph_mds_request *req;
	int op = CEPH_MDS_OP_RENAME;
	int err;

	if (flags)
		return -EINVAL;

	if (ceph_snap(old_dir) != ceph_snap(new_dir))
		return -EXDEV;
	if (ceph_snap(old_dir) != CEPH_NOSNAP) {
		if (old_dir == new_dir && ceph_snap(old_dir) == CEPH_SNAPDIR)
			op = CEPH_MDS_OP_RENAMESNAP;
		else
			return -EROFS;
	}
	/* don't allow cross-quota renames */
	if ((old_dir != new_dir) &&
	    (!ceph_quota_is_same_realm(old_dir, new_dir)))
		return -EXDEV;

	err = ceph_wait_on_conflict_unlink(new_dentry);
	if (err)
		return err;

	err = fscrypt_prepare_rename(old_dir, old_dentry, new_dir, new_dentry,
				     flags);
	if (err)
		return err;

	doutc(cl, "%llx.%llx/'%pd' to %llx.%llx/'%pd'\n",
	      ceph_vinop(old_dir), old_dentry, ceph_vinop(new_dir),
	      new_dentry);
	req = ceph_mdsc_create_request(mdsc, op, USE_AUTH_MDS);
	if (IS_ERR(req))
		return PTR_ERR(req);
	ihold(old_dir);
	req->r_dentry = dget(new_dentry);
	req->r_num_caps = 2;
	req->r_old_dentry = dget(old_dentry);
	req->r_old_dentry_dir = old_dir;
	req->r_parent = new_dir;
	ihold(new_dir);
	set_bit(CEPH_MDS_R_PARENT_LOCKED, &req->r_req_flags);
	req->r_old_dentry_drop = CEPH_CAP_FILE_SHARED | CEPH_CAP_XATTR_EXCL;
	req->r_old_dentry_unless = CEPH_CAP_FILE_EXCL;
	req->r_dentry_drop = CEPH_CAP_FILE_SHARED | CEPH_CAP_XATTR_EXCL;
	req->r_dentry_unless = CEPH_CAP_FILE_EXCL;
	/* release LINK_RDCACHE on source inode (mds will lock it) */
	req->r_old_inode_drop = CEPH_CAP_LINK_SHARED | CEPH_CAP_LINK_EXCL;
	if (d_really_is_positive(new_dentry)) {
		req->r_inode_drop =
			ceph_drop_caps_for_unlink(d_inode(new_dentry));
	}
	err = ceph_mdsc_do_request(mdsc, old_dir, req);
	if (!err && !req->r_reply_info.head->is_dentry) {
		/*
		 * Normally d_move() is done by fill_trace (called by
		 * do_request, above).  If there is no trace, we need
		 * to do it here.
		 */
		d_move(old_dentry, new_dentry);
	}
	ceph_mdsc_put_request(req);
	return err;
}

/*
 * Move dentry to tail of mdsc->dentry_leases list when lease is updated.
 * Leases at front of the list will expire first. (Assume all leases have
 * similar duration)
 *
 * Called under dentry->d_lock.
 */
void __ceph_dentry_lease_touch(struct ceph_dentry_info *di)
{
	struct dentry *dn = di->dentry;
	struct ceph_mds_client *mdsc = ceph_sb_to_fs_client(dn->d_sb)->mdsc;
	struct ceph_client *cl = mdsc->fsc->client;

	doutc(cl, "%p %p '%pd'\n", di, dn, dn);

	di->flags |= CEPH_DENTRY_LEASE_LIST;
	if (di->flags & CEPH_DENTRY_SHRINK_LIST) {
		di->flags |= CEPH_DENTRY_REFERENCED;
		return;
	}

	spin_lock(&mdsc->dentry_list_lock);
	list_move_tail(&di->lease_list, &mdsc->dentry_leases);
	spin_unlock(&mdsc->dentry_list_lock);
}

static void __dentry_dir_lease_touch(struct ceph_mds_client* mdsc,
				     struct ceph_dentry_info *di)
{
	di->flags &= ~(CEPH_DENTRY_LEASE_LIST | CEPH_DENTRY_REFERENCED);
	di->lease_gen = 0;
	di->time = jiffies;
	list_move_tail(&di->lease_list, &mdsc->dentry_dir_leases);
}

/*
 * When dir lease is used, add dentry to tail of mdsc->dentry_dir_leases
 * list if it's not in the list, otherwise set 'referenced' flag.
 *
 * Called under dentry->d_lock.
 */
void __ceph_dentry_dir_lease_touch(struct ceph_dentry_info *di)
{
	struct dentry *dn = di->dentry;
	struct ceph_mds_client *mdsc = ceph_sb_to_fs_client(dn->d_sb)->mdsc;
	struct ceph_client *cl = mdsc->fsc->client;

	doutc(cl, "%p %p '%pd' (offset 0x%llx)\n", di, dn, dn, di->offset);

	if (!list_empty(&di->lease_list)) {
		if (di->flags & CEPH_DENTRY_LEASE_LIST) {
			/* don't remove dentry from dentry lease list
			 * if its lease is valid */
			if (__dentry_lease_is_valid(di))
				return;
		} else {
			di->flags |= CEPH_DENTRY_REFERENCED;
			return;
		}
	}

	if (di->flags & CEPH_DENTRY_SHRINK_LIST) {
		di->flags |= CEPH_DENTRY_REFERENCED;
		di->flags &= ~CEPH_DENTRY_LEASE_LIST;
		return;
	}

	spin_lock(&mdsc->dentry_list_lock);
	__dentry_dir_lease_touch(mdsc, di);
	spin_unlock(&mdsc->dentry_list_lock);
}

static void __dentry_lease_unlist(struct ceph_dentry_info *di)
{
	struct ceph_mds_client *mdsc;
	if (di->flags & CEPH_DENTRY_SHRINK_LIST)
		return;
	if (list_empty(&di->lease_list))
		return;

	mdsc = ceph_sb_to_fs_client(di->dentry->d_sb)->mdsc;
	spin_lock(&mdsc->dentry_list_lock);
	list_del_init(&di->lease_list);
	spin_unlock(&mdsc->dentry_list_lock);
}

enum {
	KEEP	= 0,
	DELETE	= 1,
	TOUCH	= 2,
	STOP	= 4,
};

struct ceph_lease_walk_control {
	bool dir_lease;
	bool expire_dir_lease;
	unsigned long nr_to_scan;
	unsigned long dir_lease_ttl;
};

static int __dir_lease_check(const struct dentry *, struct ceph_lease_walk_control *);
static int __dentry_lease_check(const struct dentry *);

static unsigned long
__dentry_leases_walk(struct ceph_mds_client *mdsc,
		     struct ceph_lease_walk_control *lwc)
{
	struct ceph_dentry_info *di, *tmp;
	struct dentry *dentry, *last = NULL;
	struct list_head* list;
        LIST_HEAD(dispose);
	unsigned long freed = 0;
	int ret = 0;

	list = lwc->dir_lease ? &mdsc->dentry_dir_leases : &mdsc->dentry_leases;
	spin_lock(&mdsc->dentry_list_lock);
	list_for_each_entry_safe(di, tmp, list, lease_list) {
		if (!lwc->nr_to_scan)
			break;
		--lwc->nr_to_scan;

		dentry = di->dentry;
		if (last == dentry)
			break;

		if (!spin_trylock(&dentry->d_lock))
			continue;

		if (__lockref_is_dead(&dentry->d_lockref)) {
			list_del_init(&di->lease_list);
			goto next;
		}

		if (lwc->dir_lease)
			ret = __dir_lease_check(dentry, lwc);
		else
			ret = __dentry_lease_check(dentry);
		if (ret & TOUCH) {
			/* move it into tail of dir lease list */
			__dentry_dir_lease_touch(mdsc, di);
			if (!last)
				last = dentry;
		}
		if (ret & DELETE) {
			/* stale lease */
			di->flags &= ~CEPH_DENTRY_REFERENCED;
			if (dentry->d_lockref.count > 0) {
				/* update_dentry_lease() will re-add
				 * it to lease list, or
				 * ceph_d_delete() will return 1 when
				 * last reference is dropped */
				list_del_init(&di->lease_list);
			} else {
				di->flags |= CEPH_DENTRY_SHRINK_LIST;
				list_move_tail(&di->lease_list, &dispose);
				dget_dlock(dentry);
			}
		}
next:
		spin_unlock(&dentry->d_lock);
		if (ret & STOP)
			break;
	}
	spin_unlock(&mdsc->dentry_list_lock);

	while (!list_empty(&dispose)) {
		di = list_first_entry(&dispose, struct ceph_dentry_info,
				      lease_list);
		dentry = di->dentry;
		spin_lock(&dentry->d_lock);

		list_del_init(&di->lease_list);
		di->flags &= ~CEPH_DENTRY_SHRINK_LIST;
		if (di->flags & CEPH_DENTRY_REFERENCED) {
			spin_lock(&mdsc->dentry_list_lock);
			if (di->flags & CEPH_DENTRY_LEASE_LIST) {
				list_add_tail(&di->lease_list,
					      &mdsc->dentry_leases);
			} else {
				__dentry_dir_lease_touch(mdsc, di);
			}
			spin_unlock(&mdsc->dentry_list_lock);
		} else {
			freed++;
		}

		spin_unlock(&dentry->d_lock);
		/* ceph_d_delete() does the trick */
		dput(dentry);
	}
	return freed;
}

static int __dentry_lease_check(const struct dentry *dentry)
{
	struct ceph_dentry_info *di = ceph_dentry(dentry);
	int ret;

	if (__dentry_lease_is_valid(di))
		return STOP;
	ret = __dir_lease_try_check(dentry);
	if (ret == -EBUSY)
		return KEEP;
	if (ret > 0)
		return TOUCH;
	return DELETE;
}

static int __dir_lease_check(const struct dentry *dentry,
			     struct ceph_lease_walk_control *lwc)
{
	struct ceph_dentry_info *di = ceph_dentry(dentry);

	int ret = __dir_lease_try_check(dentry);
	if (ret == -EBUSY)
		return KEEP;
	if (ret > 0) {
		if (time_before(jiffies, di->time + lwc->dir_lease_ttl))
			return STOP;
		/* Move dentry to tail of dir lease list if we don't want
		 * to delete it. So dentries in the list are checked in a
		 * round robin manner */
		if (!lwc->expire_dir_lease)
			return TOUCH;
		if (dentry->d_lockref.count > 0 ||
		    (di->flags & CEPH_DENTRY_REFERENCED))
			return TOUCH;
		/* invalidate dir lease */
		di->lease_shared_gen = 0;
	}
	return DELETE;
}

int ceph_trim_dentries(struct ceph_mds_client *mdsc)
{
	struct ceph_lease_walk_control lwc;
	unsigned long count;
	unsigned long freed;

	spin_lock(&mdsc->caps_list_lock);
        if (mdsc->caps_use_max > 0 &&
            mdsc->caps_use_count > mdsc->caps_use_max)
		count = mdsc->caps_use_count - mdsc->caps_use_max;
	else
		count = 0;
        spin_unlock(&mdsc->caps_list_lock);

	lwc.dir_lease = false;
	lwc.nr_to_scan  = CEPH_CAPS_PER_RELEASE * 2;
	freed = __dentry_leases_walk(mdsc, &lwc);
	if (!lwc.nr_to_scan) /* more invalid leases */
		return -EAGAIN;

	if (lwc.nr_to_scan < CEPH_CAPS_PER_RELEASE)
		lwc.nr_to_scan = CEPH_CAPS_PER_RELEASE;

	lwc.dir_lease = true;
	lwc.expire_dir_lease = freed < count;
	lwc.dir_lease_ttl = mdsc->fsc->mount_options->caps_wanted_delay_max * HZ;
	freed +=__dentry_leases_walk(mdsc, &lwc);
	if (!lwc.nr_to_scan) /* more to check */
		return -EAGAIN;

	return freed > 0 ? 1 : 0;
}

/*
 * Ensure a dentry lease will no longer revalidate.
 */
void ceph_invalidate_dentry_lease(struct dentry *dentry)
{
	struct ceph_dentry_info *di = ceph_dentry(dentry);
	spin_lock(&dentry->d_lock);
	di->time = jiffies;
	di->lease_shared_gen = 0;
	di->flags &= ~CEPH_DENTRY_PRIMARY_LINK;
	__dentry_lease_unlist(di);
	spin_unlock(&dentry->d_lock);
}

/*
 * Check if dentry lease is valid.  If not, delete the lease.  Try to
 * renew if the least is more than half up.
 */
static bool __dentry_lease_is_valid(struct ceph_dentry_info *di)
{
	struct ceph_mds_session *session;

	if (!di->lease_gen)
		return false;

	session = di->lease_session;
	if (session) {
		u32 gen;
		unsigned long ttl;

		gen = atomic_read(&session->s_cap_gen);
		ttl = session->s_cap_ttl;

		if (di->lease_gen == gen &&
		    time_before(jiffies, ttl) &&
		    time_before(jiffies, di->time))
			return true;
	}
	di->lease_gen = 0;
	return false;
}

static int dentry_lease_is_valid(struct dentry *dentry, unsigned int flags)
{
	struct ceph_dentry_info *di;
	struct ceph_mds_session *session = NULL;
	struct ceph_mds_client *mdsc = ceph_sb_to_fs_client(dentry->d_sb)->mdsc;
	struct ceph_client *cl = mdsc->fsc->client;
	u32 seq = 0;
	int valid = 0;

	spin_lock(&dentry->d_lock);
	di = ceph_dentry(dentry);
	if (di && __dentry_lease_is_valid(di)) {
		valid = 1;

		if (di->lease_renew_after &&
		    time_after(jiffies, di->lease_renew_after)) {
			/*
			 * We should renew. If we're in RCU walk mode
			 * though, we can't do that so just return
			 * -ECHILD.
			 */
			if (flags & LOOKUP_RCU) {
				valid = -ECHILD;
			} else {
				session = ceph_get_mds_session(di->lease_session);
				seq = di->lease_seq;
				di->lease_renew_after = 0;
				di->lease_renew_from = jiffies;
			}
		}
	}
	spin_unlock(&dentry->d_lock);

	if (session) {
		ceph_mdsc_lease_send_msg(session, dentry,
					 CEPH_MDS_LEASE_RENEW, seq);
		ceph_put_mds_session(session);
	}
	doutc(cl, "dentry %p = %d\n", dentry, valid);
	return valid;
}

/*
 * Called under dentry->d_lock.
 */
static int __dir_lease_try_check(const struct dentry *dentry)
{
	struct ceph_dentry_info *di = ceph_dentry(dentry);
	struct inode *dir;
	struct ceph_inode_info *ci;
	int valid = 0;

	if (!di->lease_shared_gen)
		return 0;
	if (IS_ROOT(dentry))
		return 0;

	dir = d_inode(dentry->d_parent);
	ci = ceph_inode(dir);

	if (spin_trylock(&ci->i_ceph_lock)) {
		if (atomic_read(&ci->i_shared_gen) == di->lease_shared_gen &&
		    __ceph_caps_issued_mask(ci, CEPH_CAP_FILE_SHARED, 0))
			valid = 1;
		spin_unlock(&ci->i_ceph_lock);
	} else {
		valid = -EBUSY;
	}

	if (!valid)
		di->lease_shared_gen = 0;
	return valid;
}

/*
 * Check if directory-wide content lease/cap is valid.
 */
static int dir_lease_is_valid(struct inode *dir, struct dentry *dentry,
			      struct ceph_mds_client *mdsc)
{
	struct ceph_inode_info *ci = ceph_inode(dir);
	struct ceph_client *cl = mdsc->fsc->client;
	int valid;
	int shared_gen;

	spin_lock(&ci->i_ceph_lock);
	valid = __ceph_caps_issued_mask(ci, CEPH_CAP_FILE_SHARED, 1);
	if (valid) {
		__ceph_touch_fmode(ci, mdsc, CEPH_FILE_MODE_RD);
		shared_gen = atomic_read(&ci->i_shared_gen);
	}
	spin_unlock(&ci->i_ceph_lock);
	if (valid) {
		struct ceph_dentry_info *di;
		spin_lock(&dentry->d_lock);
		di = ceph_dentry(dentry);
		if (dir == d_inode(dentry->d_parent) &&
		    di && di->lease_shared_gen == shared_gen)
			__ceph_dentry_dir_lease_touch(di);
		else
			valid = 0;
		spin_unlock(&dentry->d_lock);
	}
	doutc(cl, "dir %p %llx.%llx v%u dentry %p '%pd' = %d\n", dir,
	      ceph_vinop(dir), (unsigned)atomic_read(&ci->i_shared_gen),
	      dentry, dentry, valid);
	return valid;
}

/*
 * Check if cached dentry can be trusted.
 */
static int ceph_d_revalidate(struct inode *dir, const struct qstr *name,
			     struct dentry *dentry, unsigned int flags)
{
	struct ceph_mds_client *mdsc = ceph_sb_to_fs_client(dentry->d_sb)->mdsc;
	struct ceph_client *cl = mdsc->fsc->client;
	int valid = 0;
	struct inode *inode;

	valid = fscrypt_d_revalidate(dir, name, dentry, flags);
	if (valid <= 0)
		return valid;

	inode = d_inode_rcu(dentry);

	doutc(cl, "%p '%pd' inode %p offset 0x%llx nokey %d\n",
	      dentry, dentry, inode, ceph_dentry(dentry)->offset,
	      !!(dentry->d_flags & DCACHE_NOKEY_NAME));

	mdsc = ceph_sb_to_fs_client(dir->i_sb)->mdsc;

	/* always trust cached snapped dentries, snapdir dentry */
	if (ceph_snap(dir) != CEPH_NOSNAP) {
		doutc(cl, "%p '%pd' inode %p is SNAPPED\n", dentry,
		      dentry, inode);
		valid = 1;
	} else if (inode && ceph_snap(inode) == CEPH_SNAPDIR) {
		valid = 1;
	} else {
		valid = dentry_lease_is_valid(dentry, flags);
		if (valid == -ECHILD)
			return valid;
		if (valid || dir_lease_is_valid(dir, dentry, mdsc)) {
			if (inode)
				valid = ceph_is_any_caps(inode);
			else
				valid = 1;
		}
	}

	if (!valid) {
		struct ceph_mds_request *req;
		int op, err;
		u32 mask;

		if (flags & LOOKUP_RCU)
			return -ECHILD;

		percpu_counter_inc(&mdsc->metric.d_lease_mis);

		op = ceph_snap(dir) == CEPH_SNAPDIR ?
			CEPH_MDS_OP_LOOKUPSNAP : CEPH_MDS_OP_LOOKUP;
		req = ceph_mdsc_create_request(mdsc, op, USE_ANY_MDS);
		if (!IS_ERR(req)) {
			req->r_dentry = dget(dentry);
			req->r_num_caps = 2;
			req->r_parent = dir;
			ihold(dir);

			req->r_dname = name;

			mask = CEPH_STAT_CAP_INODE | CEPH_CAP_AUTH_SHARED;
			if (ceph_security_xattr_wanted(dir))
				mask |= CEPH_CAP_XATTR_SHARED;
			req->r_args.getattr.mask = cpu_to_le32(mask);

			err = ceph_mdsc_do_request(mdsc, NULL, req);
			switch (err) {
			case 0:
				if (d_really_is_positive(dentry) &&
				    d_inode(dentry) == req->r_target_inode)
					valid = 1;
				break;
			case -ENOENT:
				if (d_really_is_negative(dentry))
					valid = 1;
				fallthrough;
			default:
				break;
			}
			ceph_mdsc_put_request(req);
			doutc(cl, "%p '%pd', lookup result=%d\n", dentry,
			      dentry, err);
		}
	} else {
		percpu_counter_inc(&mdsc->metric.d_lease_hit);
	}

	doutc(cl, "%p '%pd' %s\n", dentry, dentry, valid ? "valid" : "invalid");
	if (!valid)
		ceph_dir_clear_complete(dir);
	return valid;
}

/*
 * Delete unused dentry that doesn't have valid lease
 *
 * Called under dentry->d_lock.
 */
static int ceph_d_delete(const struct dentry *dentry)
{
	struct ceph_dentry_info *di;

	/* won't release caps */
	if (d_really_is_negative(dentry))
		return 0;
	if (ceph_snap(d_inode(dentry)) != CEPH_NOSNAP)
		return 0;
	/* valid lease? */
	di = ceph_dentry(dentry);
	if (di) {
		if (__dentry_lease_is_valid(di))
			return 0;
		if (__dir_lease_try_check(dentry))
			return 0;
	}
	return 1;
}

/*
 * Release our ceph_dentry_info.
 */
static void ceph_d_release(struct dentry *dentry)
{
	struct ceph_dentry_info *di = ceph_dentry(dentry);
	struct ceph_fs_client *fsc = ceph_sb_to_fs_client(dentry->d_sb);

	doutc(fsc->client, "dentry %p '%pd'\n", dentry, dentry);

	atomic64_dec(&fsc->mdsc->metric.total_dentries);

	spin_lock(&dentry->d_lock);
	__dentry_lease_unlist(di);
	dentry->d_fsdata = NULL;
	spin_unlock(&dentry->d_lock);

	ceph_put_mds_session(di->lease_session);
	kmem_cache_free(ceph_dentry_cachep, di);
}

/*
 * When the VFS prunes a dentry from the cache, we need to clear the
 * complete flag on the parent directory.
 *
 * Called under dentry->d_lock.
 */
static void ceph_d_prune(struct dentry *dentry)
{
	struct ceph_mds_client *mdsc = ceph_sb_to_mdsc(dentry->d_sb);
	struct ceph_client *cl = mdsc->fsc->client;
	struct ceph_inode_info *dir_ci;
	struct ceph_dentry_info *di;

	doutc(cl, "dentry %p '%pd'\n", dentry, dentry);

	/* do we have a valid parent? */
	if (IS_ROOT(dentry))
		return;

	/* we hold d_lock, so d_parent is stable */
	dir_ci = ceph_inode(d_inode(dentry->d_parent));
	if (dir_ci->i_vino.snap == CEPH_SNAPDIR)
		return;

	/* who calls d_delete() should also disable dcache readdir */
	if (d_really_is_negative(dentry))
		return;

	/* d_fsdata does not get cleared until d_release */
	if (!d_unhashed(dentry)) {
		__ceph_dir_clear_complete(dir_ci);
		return;
	}

	/* Disable dcache readdir just in case that someone called d_drop()
	 * or d_invalidate(), but MDS didn't revoke CEPH_CAP_FILE_SHARED
	 * properly (dcache readdir is still enabled) */
	di = ceph_dentry(dentry);
	if (di->offset > 0 &&
	    di->lease_shared_gen == atomic_read(&dir_ci->i_shared_gen))
		__ceph_dir_clear_ordered(dir_ci);
}

/*
 * read() on a dir.  This weird interface hack only works if mounted
 * with '-o dirstat'.
 */
static ssize_t ceph_read_dir(struct file *file, char __user *buf, size_t size,
			     loff_t *ppos)
{
	struct ceph_dir_file_info *dfi = file->private_data;
	struct inode *inode = file_inode(file);
	struct ceph_inode_info *ci = ceph_inode(inode);
	int left;
	const int bufsize = 1024;

	if (!ceph_test_mount_opt(ceph_sb_to_fs_client(inode->i_sb), DIRSTAT))
		return -EISDIR;

	if (!dfi->dir_info) {
		dfi->dir_info = kmalloc(bufsize, GFP_KERNEL);
		if (!dfi->dir_info)
			return -ENOMEM;
		dfi->dir_info_len =
			snprintf(dfi->dir_info, bufsize,
				"entries:   %20lld\n"
				" files:    %20lld\n"
				" subdirs:  %20lld\n"
				"rentries:  %20lld\n"
				" rfiles:   %20lld\n"
				" rsubdirs: %20lld\n"
				"rbytes:    %20lld\n"
				"rctime:    %10lld.%09ld\n",
				ci->i_files + ci->i_subdirs,
				ci->i_files,
				ci->i_subdirs,
				ci->i_rfiles + ci->i_rsubdirs,
				ci->i_rfiles,
				ci->i_rsubdirs,
				ci->i_rbytes,
				ci->i_rctime.tv_sec,
				ci->i_rctime.tv_nsec);
	}

	if (*ppos >= dfi->dir_info_len)
		return 0;
	size = min_t(unsigned, size, dfi->dir_info_len-*ppos);
	left = copy_to_user(buf, dfi->dir_info + *ppos, size);
	if (left == size)
		return -EFAULT;
	*ppos += (size - left);
	return size - left;
}



/*
 * Return name hash for a given dentry.  This is dependent on
 * the parent directory's hash function.
 */
unsigned ceph_dentry_hash(struct inode *dir, struct dentry *dn)
{
	struct ceph_inode_info *dci = ceph_inode(dir);
	unsigned hash;

	switch (dci->i_dir_layout.dl_dir_hash) {
	case 0:	/* for backward compat */
	case CEPH_STR_HASH_LINUX:
		return dn->d_name.hash;

	default:
		spin_lock(&dn->d_lock);
		hash = ceph_str_hash(dci->i_dir_layout.dl_dir_hash,
				     dn->d_name.name, dn->d_name.len);
		spin_unlock(&dn->d_lock);
		return hash;
	}
}

WRAP_DIR_ITER(ceph_readdir) // FIXME!
const struct file_operations ceph_dir_fops = {
	.read = ceph_read_dir,
	.iterate_shared = shared_ceph_readdir,
	.llseek = ceph_dir_llseek,
	.open = ceph_open,
	.release = ceph_release,
	.unlocked_ioctl = ceph_ioctl,
	.compat_ioctl = compat_ptr_ioctl,
	.fsync = ceph_fsync,
	.lock = ceph_lock,
	.flock = ceph_flock,
};

const struct file_operations ceph_snapdir_fops = {
	.iterate_shared = shared_ceph_readdir,
	.llseek = ceph_dir_llseek,
	.open = ceph_open,
	.release = ceph_release,
};

const struct inode_operations ceph_dir_iops = {
	.lookup = ceph_lookup,
	.permission = ceph_permission,
	.getattr = ceph_getattr,
	.setattr = ceph_setattr,
	.listxattr = ceph_listxattr,
	.get_inode_acl = ceph_get_acl,
	.set_acl = ceph_set_acl,
	.mknod = ceph_mknod,
	.symlink = ceph_symlink,
	.mkdir = ceph_mkdir,
	.link = ceph_link,
	.unlink = ceph_unlink,
	.rmdir = ceph_unlink,
	.rename = ceph_rename,
	.create = ceph_create,
	.atomic_open = ceph_atomic_open,
};

const struct inode_operations ceph_snapdir_iops = {
	.lookup = ceph_lookup,
	.permission = ceph_permission,
	.getattr = ceph_getattr,
	.mkdir = ceph_mkdir,
	.rmdir = ceph_unlink,
	.rename = ceph_rename,
};

const struct dentry_operations ceph_dentry_ops = {
	.d_revalidate = ceph_d_revalidate,
	.d_delete = ceph_d_delete,
	.d_release = ceph_d_release,
	.d_prune = ceph_d_prune,
	.d_init = ceph_d_init,
};
