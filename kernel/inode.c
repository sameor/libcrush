#include <linux/module.h>
#include <linux/fs.h>
#include <linux/smp_lock.h>
#include <linux/slab.h>

#include <linux/ceph_fs.h>

int ceph_inode_debug = 50;
#define DOUT_VAR ceph_inode_debug
#define DOUT_PREFIX "inode: "
#include "super.h"

const struct inode_operations ceph_symlink_iops;

int ceph_fill_inode(struct inode *inode, struct ceph_mds_reply_inode *info) 
{
	struct ceph_inode_info *ci = ceph_inode(inode);
	int i;

	inode->i_ino = le64_to_cpu(info->ino);
	inode->i_mode = le32_to_cpu(info->mode);
	inode->i_uid = le32_to_cpu(info->uid);
	inode->i_gid = le32_to_cpu(info->gid);
	inode->i_nlink = le32_to_cpu(info->nlink);
	inode->i_size = le64_to_cpu(info->size);
	inode->i_rdev = le32_to_cpu(info->rdev);
	inode->i_blocks = 1;
	inode->i_rdev = 0;

	insert_inode_hash(inode);

	dout(30, "new_inode ino=%lx by %d.%d sz=%llu mode %o\n", inode->i_ino,
	     inode->i_uid, inode->i_gid, inode->i_size, inode->i_mode);
	
	ceph_decode_timespec(&inode->i_atime, &info->atime);
	ceph_decode_timespec(&inode->i_mtime, &info->mtime);
	ceph_decode_timespec(&inode->i_ctime, &info->ctime);

	/* ceph inode */
	dout(30, "inode %p, ci %p\n", inode, ci);
	ci->i_layout = info->layout; 
	dout(30, "inode layout %p su %d\n", &ci->i_layout, ci->i_layout.fl_stripe_unit);

	if (le32_to_cpu(info->fragtree.nsplits) > 0) {
		//ci->i_fragtree = kmalloc(...);
		BUG_ON(1); // write me
	}
	ci->i_fragtree->nsplits = le32_to_cpu(info->fragtree.nsplits);
	for (i=0; i<ci->i_fragtree->nsplits; i++)
		ci->i_fragtree->splits[i] = le32_to_cpu(info->fragtree.splits[i]);

	ci->i_frag_map_nr = 1;
	ci->i_frag_map[0].frag = 0;
	ci->i_frag_map[0].mds = 0; // FIXME
	
	ci->i_nr_caps = 0;
	for (i=0; i<4; i++)
		ci->i_nr_by_mode[i] = 0;
	ci->i_cap_wanted = 0;
	
	ci->i_wr_size = 0;
	ci->i_wr_mtime.tv_sec = 0;
	ci->i_wr_mtime.tv_nsec = 0;

	inode->i_mapping->a_ops = &ceph_aops;

	switch (inode->i_mode & S_IFMT) {
	case S_IFIFO:
	case S_IFBLK:
	case S_IFCHR:
	case S_IFSOCK:
		dout(20, "%p is special\n", inode);
		init_special_inode(inode, inode->i_mode, inode->i_rdev);
		break;
	case S_IFREG:
		dout(20, "%p is a file\n", inode);
		inode->i_op = &ceph_file_iops;
		inode->i_fop = &ceph_file_fops;
		break;
	case S_IFLNK:
		dout(20, "%p is a symlink\n", inode);
		inode->i_op = &ceph_symlink_iops;
		break;
	case S_IFDIR:
		dout(20, "%p is a dir\n", inode);
		inc_nlink(inode);
		inode->i_op = &ceph_dir_iops;
		inode->i_fop = &ceph_dir_fops;
		break;
	default:
		derr(0, "BAD mode 0x%x S_IFMT 0x%x\n",
		     inode->i_mode, inode->i_mode & S_IFMT);
		return -EINVAL;
	}

	return 0;
}

struct ceph_inode_cap *ceph_find_cap(struct inode *inode, int want)
{
	struct ceph_inode_info *ci = ceph_inode(inode);

	int i;
	for (i=0; i<ci->i_nr_caps; i++) 
		if ((ci->i_caps[i].caps & want) == want) {
			dout(40, "find_cap found i=%d cap %d want %d\n", i, ci->i_caps[i].caps, want);
			return &ci->i_caps[i];
		}
	return 0;
}

static struct ceph_inode_cap *get_cap_for_mds(struct inode *inode, int mds)
{
	struct ceph_inode_info *ci = ceph_inode(inode);
	int i;
	for (i=0; i<ci->i_nr_caps; i++) 
		if (ci->i_caps[i].mds == mds) 
			return &ci->i_caps[i];
	return 0;
}


struct ceph_inode_cap *ceph_add_cap(struct inode *inode, int mds, u32 cap, u32 seq)
{
	struct ceph_inode_info *ci = ceph_inode(inode);
	int i;

	for (i=0; i<ci->i_nr_caps; i++) 
		if (ci->i_caps[i].mds == mds) break;
	if (i == ci->i_nr_caps) {
		if (i == ci->i_max_caps) {
			/* realloc */
			void *o = ci->i_caps;
			ci->i_caps = kmalloc(ci->i_max_caps*2*sizeof(*ci->i_caps), GFP_KERNEL);
			if (ci->i_caps == NULL) {
				ci->i_caps = o;
				derr(0, "add_cap enomem\n");
				return ERR_PTR(-ENOMEM);
			}
			memcpy(ci->i_caps, o, ci->i_nr_caps*sizeof(*ci->i_caps));
			if (o != ci->i_caps_static)
				kfree(o);
			ci->i_max_caps *= 2;
		}

		ci->i_caps[i].caps = 0;
		ci->i_caps[i].mds = mds;
		ci->i_caps[i].seq = 0;
		ci->i_caps[i].flags = 0;
		ci->i_nr_caps++;
	}

	dout(10, "add_cap inode %p (%lu) got cap %d %xh now %xh seq %d from %d\n",
	     inode, inode->i_ino, i, cap, cap|ci->i_caps[i].caps, seq, mds);
	ci->i_caps[i].caps |= cap;
	ci->i_caps[i].seq = seq;
	return &ci->i_caps[i];
}

int ceph_get_caps(struct ceph_inode_info *ci)
{
	int i;
	int have = 0;
	for (i=0; i<ci->i_nr_caps; i++)
		have |= ci->i_caps[i].caps;
	return have;
}


/*
 * 0 - ok
 * 1 - send the msg back to mds
 */
int ceph_handle_cap_grant(struct inode *inode, struct ceph_mds_file_caps *grant, struct ceph_mds_session *session)
{
	struct ceph_inode_cap *cap;
	struct ceph_inode_info *ci = ceph_inode(inode);
	int mds = session->s_mds;
	int seq = le32_to_cpu(grant->seq);
	int newcaps;

	dout(10, "handle_cap_grant inode %p ci %p mds%d seq %d\n", inode, ci, mds, seq);

	/* unwanted? */
	if (ceph_caps_wanted(ci) == 0) {
		dout(10, "wanted=0, reminding mds\n");
		grant->wanted = cpu_to_le32(0);
		return 1; /* ack */
	}

	/* new cap? */
	dout(10, "1\n");
	cap = get_cap_for_mds(inode, mds);
	dout(10, "2\n");
	if (!cap) {
		dout(10, "adding new cap inode %p for mds%d\n", inode, mds);
		cap = ceph_add_cap(inode, mds, le32_to_cpu(grant->caps), le32_to_cpu(grant->seq));
		return 0;
	} 

	/* revocation? */
	dout(10, "3\n");
	newcaps = le32_to_cpu(grant->caps);
	dout(10, "4\n");
	if (cap->caps & ~newcaps) {
		dout(10, "revocation: %d -> %d\n", cap->caps, newcaps);
		/* FIXME FIXME FIXME DO STUFF HERE */
		/* blindly ack for now: */
		cap->caps = newcaps;
		return 1; /* ack */
	}
	
	/* grant or no-op */
	dout(10, "5\n");
	if (cap->caps == newcaps) {
		dout(10, "no-op: %d -> %d\n", cap->caps, newcaps);
	} else {
		dout(10, "grant: %d -> %d\n", cap->caps, newcaps);
		cap->caps = newcaps;
	}
	return 0;	
}



/*
 * vfs methods
 */
int ceph_inode_getattr(struct vfsmount *mnt, struct dentry *dentry,
		       struct kstat *stat)
{
	dout(5, "getattr on dentry %p\n", dentry);
	return 0;
}



/*


static int ceph_vfs_setattr(struct dentry *dentry, struct iattr *iattr)
{
}

static int ceph_vfs_readlink(struct dentry *dentry, char __user * buffer,
			     int buflen)
{
}

static void *ceph_vfs_follow_link(struct dentry *dentry, struct nameidata *nd)
{
}

static void ceph_vfs_put_link(struct dentry *dentry, struct nameidata *nd, void *p)
{
}

static int
ceph_vfs_symlink(struct inode *dir, struct dentry *dentry, const char *symname)
{
}

static int
ceph_vfs_link(struct dentry *old_dentry, struct inode *dir,
	      struct dentry *dentry)
{
}

static int
ceph_vfs_mknod(struct inode *dir, struct dentry *dentry, int mode, dev_t rdev)
{
}
*/

const struct inode_operations ceph_symlink_iops = {
/*	.readlink = ceph_vfs_readlink,
	.follow_link = ceph_vfs_follow_link,
	.put_link = ceph_vfs_put_link,
	.getattr = ceph_vfs_getattr,
	.setattr = ceph_vfs_setattr,
*/
};
