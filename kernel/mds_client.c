
#include <linux/wait.h>
#include <linux/sched.h>
#include "mds_client.h"
#include "mon_client.h"

#include "ceph_debug.h"
#include "super.h"
#include "messenger.h"
#include "decode.h"

/*
 * A cluster of MDS (metadata server) daemons is responsible for
 * managing the file system namespace (the directory hierarchy and
 * inodes) and for coordinating shared access to storage.  Metadata is
 * partitioning hierarchically across a number of servers, and that
 * partition varies over time as the cluster adjusts the distribution
 * in order to balance load.
 *
 * The MDS client is primarily responsible to managing synchronous
 * metadata requests for operations like open, unlink, and so forth.
 * If there is a MDS failure, we find out about it when we (possibly
 * request and) receive a new MDS map, and can resubmit affected
 * requests.
 *
 * For the most part, though, we take advantage of a lossless
 * communications channel to the MDS, and do not need to worry about
 * timing out or resubmitting requests.
 *
 * We maintain a stateful "session" with each MDS we interact with.
 * Within each session, we sent periodic heartbeat messages to ensure
 * any capabilities or leases we have been issues remain valid.  If
 * the session times out and goes stale, our leases and capabilities
 * are no longer valid.
 */

static void __wake_requests(struct ceph_mds_client *mdsc,
			    struct list_head *head);

const static struct ceph_connection_operations mds_con_ops;


/*
 * mds reply parsing
 */

/*
 * parse individual inode info
 */
static int parse_reply_info_in(void **p, void *end,
			       struct ceph_mds_reply_info_in *info)
{
	int err = -EIO;

	info->in = *p;
	*p += sizeof(struct ceph_mds_reply_inode) +
		sizeof(*info->in->fragtree.splits) *
		le32_to_cpu(info->in->fragtree.nsplits);

	ceph_decode_32_safe(p, end, info->symlink_len, bad);
	ceph_decode_need(p, end, info->symlink_len, bad);
	info->symlink = *p;
	*p += info->symlink_len;

	ceph_decode_32_safe(p, end, info->xattr_len, bad);
	ceph_decode_need(p, end, info->xattr_len, bad);
	info->xattr_data = *p;
	*p += info->xattr_len;
	return 0;
bad:
	return err;
}

/*
 * parse a normal reply, which may contain a (dir+)dentry and/or a
 * target inode.
 */
static int parse_reply_info_trace(void **p, void *end,
				  struct ceph_mds_reply_info_parsed *info)
{
	int err;

	if (info->head->is_dentry) {
		err = parse_reply_info_in(p, end, &info->diri);
		if (err < 0)
			goto out_bad;

		if (unlikely(*p + sizeof(*info->dirfrag) > end))
			goto bad;
		info->dirfrag = *p;
		*p += sizeof(*info->dirfrag) +
			sizeof(u32)*le32_to_cpu(info->dirfrag->ndist);
		if (unlikely(*p > end))
			goto bad;

		ceph_decode_32_safe(p, end, info->dname_len, bad);
		ceph_decode_need(p, end, info->dname_len, bad);
		info->dname = *p;
		*p += info->dname_len;
		info->dlease = *p;
		*p += sizeof(*info->dlease);
	}

	if (info->head->is_target) {
		err = parse_reply_info_in(p, end, &info->targeti);
		if (err < 0)
			goto out_bad;
	}

	if (unlikely(*p != end))
		goto bad;
	return 0;

bad:
	err = -EIO;
out_bad:
	pr_err("ceph problem parsing mds trace %d\n", err);
	return err;
}

/*
 * parse readdir results
 */
static int parse_reply_info_dir(void **p, void *end,
				struct ceph_mds_reply_info_parsed *info)
{
	u32 num, i = 0;
	int err;

	info->dir_dir = *p;
	if (*p + sizeof(*info->dir_dir) > end)
		goto bad;
	*p += sizeof(*info->dir_dir) +
		sizeof(u32)*le32_to_cpu(info->dir_dir->ndist);
	if (*p > end)
		goto bad;

	ceph_decode_need(p, end, sizeof(num) + 2, bad);
	ceph_decode_32(p, num);
	ceph_decode_8(p, info->dir_end);
	ceph_decode_8(p, info->dir_complete);
	if (num == 0)
		goto done;

	/* alloc large array */
	info->dir_nr = num;
	info->dir_in = kcalloc(num, sizeof(*info->dir_in) +
			       sizeof(*info->dir_dname) +
			       sizeof(*info->dir_dname_len) +
			       sizeof(*info->dir_dlease),
			       GFP_NOFS);
	if (info->dir_in == NULL) {
		err = -ENOMEM;
		goto out_bad;
	}
	info->dir_dname = (void *)(info->dir_in + num);
	info->dir_dname_len = (void *)(info->dir_dname + num);
	info->dir_dlease = (void *)(info->dir_dname_len + num);

	while (num) {
		/* dentry */
		ceph_decode_need(p, end, sizeof(u32)*2, bad);
		ceph_decode_32(p, info->dir_dname_len[i]);
		ceph_decode_need(p, end, info->dir_dname_len[i], bad);
		info->dir_dname[i] = *p;
		*p += info->dir_dname_len[i];
		dout("parsed dir dname '%.*s'\n", info->dir_dname_len[i],
		     info->dir_dname[i]);
		info->dir_dlease[i] = *p;
		*p += sizeof(struct ceph_mds_reply_lease);

		/* inode */
		err = parse_reply_info_in(p, end, &info->dir_in[i]);
		if (err < 0)
			goto out_bad;
		i++;
		num--;
	}

done:
	if (*p != end)
		goto bad;
	return 0;

bad:
	err = -EIO;
out_bad:
	pr_err("ceph problem parsing dir contents %d\n", err);
	return err;
}

/*
 * parse entire mds reply
 */
static int parse_reply_info(struct ceph_msg *msg,
			    struct ceph_mds_reply_info_parsed *info)
{
	void *p, *end;
	u32 len;
	int err;

	info->head = msg->front.iov_base;
	p = msg->front.iov_base + sizeof(struct ceph_mds_reply_head);
	end = p + msg->front.iov_len - sizeof(struct ceph_mds_reply_head);

	/* trace */
	ceph_decode_32_safe(&p, end, len, bad);
	if (len > 0) {
		err = parse_reply_info_trace(&p, p+len, info);
		if (err < 0)
			goto out_bad;
	}

	/* dir content */
	ceph_decode_32_safe(&p, end, len, bad);
	if (len > 0) {
		err = parse_reply_info_dir(&p, p+len, info);
		if (err < 0)
			goto out_bad;
	}

	/* snap blob */
	ceph_decode_32_safe(&p, end, len, bad);
	info->snapblob_len = len;
	info->snapblob = p;
	p += len;

	if (p != end)
		goto bad;
	return 0;

bad:
	err = -EIO;
out_bad:
	pr_err("ceph mds parse_reply err %d\n", err);
	return err;
}

static void destroy_reply_info(struct ceph_mds_reply_info_parsed *info)
{
	kfree(info->dir_in);
}


/*
 * sessions
 */
static const char *session_state_name(int s)
{
	switch (s) {
	case CEPH_MDS_SESSION_NEW: return "new";
	case CEPH_MDS_SESSION_OPENING: return "opening";
	case CEPH_MDS_SESSION_OPEN: return "open";
	case CEPH_MDS_SESSION_HUNG: return "hung";
	case CEPH_MDS_SESSION_CLOSING: return "closing";
	case CEPH_MDS_SESSION_RECONNECTING: return "reconnecting";
	default: return "???";
	}
}

static struct ceph_mds_session *get_session(struct ceph_mds_session *s)
{
	dout("mdsc get_session %p %d -> %d\n", s,
	     atomic_read(&s->s_ref), atomic_read(&s->s_ref)+1);
	atomic_inc(&s->s_ref);
	return s;
}

void ceph_put_mds_session(struct ceph_mds_session *s)
{
	dout("mdsc put_session %p %d -> %d\n", s,
	     atomic_read(&s->s_ref), atomic_read(&s->s_ref)-1);
	if (atomic_dec_and_test(&s->s_ref)) {
		ceph_con_destroy(&s->s_con);
		kfree(s);
	}
}

/*
 * called under mdsc->mutex
 */
struct ceph_mds_session *__ceph_lookup_mds_session(struct ceph_mds_client *mdsc,
						   int mds)
{
	struct ceph_mds_session *session;

	if (mds >= mdsc->max_sessions || mdsc->sessions[mds] == NULL)
		return NULL;
	session = mdsc->sessions[mds];
	dout("lookup_mds_session %p %d -> %d\n", session,
	     atomic_read(&session->s_ref), atomic_read(&session->s_ref)+1);
	get_session(session);
	return session;
}

static bool __have_session(struct ceph_mds_client *mdsc, int mds)
{
	if (mds >= mdsc->max_sessions)
		return false;
	return mdsc->sessions[mds];
}

/*
 * create+register a new session for given mds.
 * called under mdsc->mutex.
 */
static struct ceph_mds_session *register_session(struct ceph_mds_client *mdsc,
						 int mds)
{
	struct ceph_mds_session *s;

	s = kzalloc(sizeof(*s), GFP_NOFS);
	s->s_mdsc = mdsc;
	s->s_mds = mds;
	s->s_state = CEPH_MDS_SESSION_NEW;
	s->s_ttl = 0;
	s->s_seq = 0;
	mutex_init(&s->s_mutex);

	ceph_con_init(mdsc->client->msgr, &s->s_con,
		      ceph_mdsmap_get_addr(mdsc->mdsmap, mds));
	s->s_con.private = s;
	s->s_con.ops = &mds_con_ops;
	s->s_con.peer_name.type = cpu_to_le32(CEPH_ENTITY_TYPE_MDS);
	s->s_con.peer_name.num = cpu_to_le32(mds);

	spin_lock_init(&s->s_cap_lock);
	s->s_cap_gen = 0;
	s->s_cap_ttl = 0;
	s->s_renew_requested = 0;
	INIT_LIST_HEAD(&s->s_caps);
	s->s_nr_caps = 0;
	atomic_set(&s->s_ref, 1);
	INIT_LIST_HEAD(&s->s_waiting);
	INIT_LIST_HEAD(&s->s_unsafe);
	s->s_num_cap_releases = 0;
	INIT_LIST_HEAD(&s->s_cap_releases);
	INIT_LIST_HEAD(&s->s_cap_releases_done);
	INIT_LIST_HEAD(&s->s_cap_flushing);
	INIT_LIST_HEAD(&s->s_cap_snaps_flushing);

	dout("register_session mds%d\n", mds);
	if (mds >= mdsc->max_sessions) {
		int newmax = 1 << get_count_order(mds+1);
		struct ceph_mds_session **sa;

		dout("register_session realloc to %d\n", newmax);
		sa = kcalloc(newmax, sizeof(void *), GFP_NOFS);
		if (sa == NULL)
			return ERR_PTR(-ENOMEM);
		if (mdsc->sessions) {
			memcpy(sa, mdsc->sessions,
			       mdsc->max_sessions * sizeof(void *));
			kfree(mdsc->sessions);
		}
		mdsc->sessions = sa;
		mdsc->max_sessions = newmax;
	}
	mdsc->sessions[mds] = s;
	atomic_inc(&s->s_ref);  /* one ref to sessions[], one to caller */
	return s;
}

/*
 * called under mdsc->mutex
 */
static void unregister_session(struct ceph_mds_client *mdsc, int mds)
{
	dout("unregister_session mds%d %p\n", mds, mdsc->sessions[mds]);
	ceph_put_mds_session(mdsc->sessions[mds]);
	mdsc->sessions[mds] = NULL;
}

/*
 * drop session refs in request.
 *
 * should be last ref, or hold mdsc->mutex
 */
static void put_request_sessions(struct ceph_mds_request *req)
{
	if (req->r_session) {
		ceph_put_mds_session(req->r_session);
		req->r_session = NULL;
	}
	if (req->r_fwd_session) {
		ceph_put_mds_session(req->r_fwd_session);
		req->r_fwd_session = NULL;
	}
}

void ceph_mdsc_put_request(struct ceph_mds_request *req)
{
	dout("mdsc put_request %p %d -> %d\n", req,
	     atomic_read(&req->r_ref), atomic_read(&req->r_ref)-1);
	if (atomic_dec_and_test(&req->r_ref)) {
		if (req->r_request)
			ceph_msg_put(req->r_request);
		if (req->r_reply) {
			ceph_msg_put(req->r_reply);
			destroy_reply_info(&req->r_reply_info);
		}
		if (req->r_inode) {
			ceph_put_cap_refs(ceph_inode(req->r_inode),
					  CEPH_CAP_PIN);
			iput(req->r_inode);
		}
		if (req->r_locked_dir)
			ceph_put_cap_refs(ceph_inode(req->r_locked_dir),
					  CEPH_CAP_PIN);
		if (req->r_target_inode)
			iput(req->r_target_inode);
		if (req->r_dentry)
			dput(req->r_dentry);
		if (req->r_old_dentry) {
			ceph_put_cap_refs(ceph_inode(req->r_old_dentry->d_parent->d_inode),
					  CEPH_CAP_PIN);
			dput(req->r_old_dentry);
		}
		kfree(req->r_path1);
		kfree(req->r_path2);
		put_request_sessions(req);
		ceph_unreserve_caps(&req->r_caps_reservation);
		kfree(req);
	}
}

/*
 * lookup session, bump ref if found.
 *
 * called under mdsc->mutex.
 */
static struct ceph_mds_request *__lookup_request(struct ceph_mds_client *mdsc,
					     u64 tid)
{
	struct ceph_mds_request *req;
	req = radix_tree_lookup(&mdsc->request_tree, tid);
	if (req)
		ceph_mdsc_get_request(req);
	return req;
}

/*
 * Register an in-flight request, and assign a tid.  Link to directory
 * are modifying (if any).
 *
 * Called under mdsc->mutex.
 */
static void __register_request(struct ceph_mds_client *mdsc,
			       struct ceph_mds_request *req,
			       struct inode *dir)
{
	req->r_tid = ++mdsc->last_tid;
	if (req->r_num_caps)
		ceph_reserve_caps(&req->r_caps_reservation, req->r_num_caps);
	dout("__register_request %p tid %lld\n", req, req->r_tid);
	ceph_mdsc_get_request(req);
	radix_tree_insert(&mdsc->request_tree, req->r_tid, (void *)req);

	if (dir) {
		struct ceph_inode_info *ci = ceph_inode(dir);

		spin_lock(&ci->i_unsafe_lock);
		req->r_unsafe_dir = dir;
		list_add_tail(&req->r_unsafe_dir_item, &ci->i_unsafe_dirops);
		spin_unlock(&ci->i_unsafe_lock);
	}
}

static void __unregister_request(struct ceph_mds_client *mdsc,
				 struct ceph_mds_request *req)
{
	dout("__unregister_request %p tid %lld\n", req, req->r_tid);
	radix_tree_delete(&mdsc->request_tree, req->r_tid);
	ceph_mdsc_put_request(req);

	if (req->r_unsafe_dir) {
		struct ceph_inode_info *ci = ceph_inode(req->r_unsafe_dir);

		spin_lock(&ci->i_unsafe_lock);
		list_del_init(&req->r_unsafe_dir_item);
		spin_unlock(&ci->i_unsafe_lock);
	}
}

/*
 * Choose mds to send request to next.  If there is a hint set in the
 * request (e.g., due to a prior forward hint from the mds), use that.
 * Otherwise, consult frag tree and/or caps to identify the
 * appropriate mds.  If all else fails, choose randomly.
 *
 * Called under mdsc->mutex.
 */
static int __choose_mds(struct ceph_mds_client *mdsc,
			struct ceph_mds_request *req)
{
	struct inode *inode;
	struct ceph_inode_info *ci;
	struct ceph_cap *cap;
	int mode = req->r_direct_mode;
	int mds = -1;
	u32 hash = req->r_direct_hash;
	bool is_hash = req->r_direct_is_hash;

	/*
	 * is there a specific mds we should try?  ignore hint if we have
	 * no session and the mds is not up (active or recovering).
	 */
	if (req->r_resend_mds >= 0 &&
	    (__have_session(mdsc, req->r_resend_mds) ||
	     ceph_mdsmap_get_state(mdsc->mdsmap, req->r_resend_mds) > 0)) {
		dout("choose_mds using resend_mds mds%d\n",
		     req->r_resend_mds);
		return req->r_resend_mds;
	}

	if (mode == USE_RANDOM_MDS)
		goto random;

	inode = 0;
	if (req->r_inode) {
		inode = req->r_inode;
	} else if (req->r_dentry) {
		if (req->r_dentry->d_inode) {
			inode = req->r_dentry->d_inode;
		} else {
			inode = req->r_dentry->d_parent->d_inode;
			hash = req->r_dentry->d_name.hash;
			is_hash = true;
		}
	}
	dout("__choose_mds %p is_hash=%d (%d) mode %d\n", inode, (int)is_hash,
	     (int)hash, mode);
	if (!inode)
		goto random;
	ci = ceph_inode(inode);

	if (is_hash && S_ISDIR(inode->i_mode)) {
		struct ceph_inode_frag frag;
		int found;

		ceph_choose_frag(ci, hash, &frag, &found);
		if (found) {
			if (mode == USE_ANY_MDS && frag.ndist > 0) {
				u8 r;

				/* choose a random replica */
				get_random_bytes(&r, 1);
				r %= frag.ndist;
				mds = frag.dist[r];
				dout("choose_mds %p %llx.%llx "
				     "frag %u mds%d (%d/%d)\n",
				     inode, ceph_vinop(inode),
				     frag.frag, frag.mds,
				     (int)r, frag.ndist);
				return mds;
			}

			/* since this file/dir wasn't known to be
			 * replicated, then we want to look for the
			 * authoritative mds. */
			mode = USE_AUTH_MDS;
			if (frag.mds >= 0) {
				/* choose auth mds */
				mds = frag.mds;
				dout("choose_mds %p %llx.%llx "
				     "frag %u mds%d (auth)\n",
				     inode, ceph_vinop(inode), frag.frag, mds);
				return mds;
			}
		}
	}

	spin_lock(&inode->i_lock);
	cap = 0;
	if (mode == USE_AUTH_MDS)
		cap = ci->i_auth_cap;
	if (!cap && !RB_EMPTY_ROOT(&ci->i_caps))
		cap = rb_entry(rb_first(&ci->i_caps), struct ceph_cap, ci_node);
	if (!cap) {
		spin_unlock(&inode->i_lock);
		goto random;
	}
	mds = cap->session->s_mds;
	dout("choose_mds %p %llx.%llx mds%d (%scap %p)\n",
	     inode, ceph_vinop(inode), mds, 
	     cap == ci->i_auth_cap ? "auth " : "", cap);
	spin_unlock(&inode->i_lock);
	return mds;

random:
	mds = ceph_mdsmap_get_random_mds(mdsc->mdsmap);
	dout("choose_mds chose random mds%d\n", mds);
	return mds;
}


/*
 * session messages
 */
static struct ceph_msg *create_session_msg(u32 op, u64 seq)
{
	struct ceph_msg *msg;
	struct ceph_mds_session_head *h;

	msg = ceph_msg_new(CEPH_MSG_CLIENT_SESSION, sizeof(*h), 0, 0, NULL);
	if (IS_ERR(msg)) {
		pr_err("ceph create_session_msg ENOMEM creating msg\n");
		return ERR_PTR(PTR_ERR(msg));
	}
	h = msg->front.iov_base;
	h->op = cpu_to_le32(op);
	h->seq = cpu_to_le64(seq);
	return msg;
}

/*
 * send session open request.
 *
 * called under mdsc->mutex
 */
static int __open_session(struct ceph_mds_client *mdsc,
			  struct ceph_mds_session *session)
{
	struct ceph_msg *msg;
	int mstate;
	int mds = session->s_mds;
	int err = 0;

	/* wait for mds to go active? */
	mstate = ceph_mdsmap_get_state(mdsc->mdsmap, mds);
	dout("open_session to mds%d (%s)\n", mds,
	     ceph_mds_state_name(mstate));
	session->s_state = CEPH_MDS_SESSION_OPENING;
	session->s_renew_requested = jiffies;

	/* send connect message */
	msg = create_session_msg(CEPH_SESSION_REQUEST_OPEN, session->s_seq);
	if (IS_ERR(msg)) {
		err = PTR_ERR(msg);
		goto out;
	}
	ceph_con_send(&session->s_con, msg);

out:
	return 0;
}

/*
 * session caps
 */

/*
 * Free preallocated cap messages assigned to this session
 */
static void cleanup_cap_releases(struct ceph_mds_session *session)
{
	struct ceph_msg *msg;

	spin_lock(&session->s_cap_lock);
	while (!list_empty(&session->s_cap_releases)) {
		msg = list_first_entry(&session->s_cap_releases,
				       struct ceph_msg, list_head);
		ceph_msg_remove(msg);
	}
	while (!list_empty(&session->s_cap_releases_done)) {
		msg = list_first_entry(&session->s_cap_releases_done,
				       struct ceph_msg, list_head);
		ceph_msg_remove(msg);
	}
	spin_unlock(&session->s_cap_lock);
}

/*
 * Helper to safely iterate over all caps associated with a session.
 *
 * caller must hold session s_mutex
 */
static int iterate_session_caps(struct ceph_mds_session *session,
				 int (*cb)(struct inode *, struct ceph_cap *,
					    void *), void *arg)
{
	struct ceph_cap *cap, *ncap;
	struct inode *inode;
	int ret;

	dout("iterate_session_caps %p mds%d\n", session, session->s_mds);
	spin_lock(&session->s_cap_lock);
	list_for_each_entry_safe(cap, ncap, &session->s_caps, session_caps) {
		inode = igrab(&cap->ci->vfs_inode);
		if (!inode)
			continue;
		spin_unlock(&session->s_cap_lock);
		ret = cb(inode, cap, arg);
		iput(inode);
		if (ret < 0)
			return ret;
		spin_lock(&session->s_cap_lock);
	}
	spin_unlock(&session->s_cap_lock);

	return 0;
}

static int remove_session_caps_cb(struct inode *inode, struct ceph_cap *cap,
				   void *arg)
{
	struct ceph_inode_info *ci = ceph_inode(inode);
	dout("removing cap %p, ci is %p, inode is %p\n",
	     cap, ci, &ci->vfs_inode);
	ceph_remove_cap(cap);
	return 0;
}

/*
 * caller must hold session s_mutex
 */
static void remove_session_caps(struct ceph_mds_session *session)
{
	dout("remove_session_caps on %p\n", session);
	iterate_session_caps(session, remove_session_caps_cb, NULL);
	BUG_ON(session->s_nr_caps > 0);
	cleanup_cap_releases(session);
}

/*
 * wake up any threads waiting on this session's caps.  if the cap is
 * old (didn't get renewed on the client reconnect), remove it now.
 *
 * caller must hold s_mutex.
 */
static int wake_up_session_cb(struct inode *inode, struct ceph_cap *cap,
			      void *arg)
{
	struct ceph_mds_session *session = arg;

	spin_lock(&inode->i_lock);
	if (cap->gen != session->s_cap_gen) {
		pr_err("ceph failed reconnect %p %llx.%llx cap %p "
		       "(gen %d < session %d)\n", inode, ceph_vinop(inode),
		       cap, cap->gen, session->s_cap_gen);
		__ceph_remove_cap(cap, NULL);
	}
	wake_up(&ceph_inode(inode)->i_cap_wq);
	spin_unlock(&inode->i_lock);
	return 0;
}

static void wake_up_session_caps(struct ceph_mds_session *session)
{
	dout("wake_up_session_caps %p mds%d\n", session, session->s_mds);
	iterate_session_caps(session, wake_up_session_cb, session);
}

/*
 * Send periodic message to MDS renewing all currently held caps.  The
 * ack will reset the expiration for all caps from this session.
 *
 * caller holds s_mutex
 */
static int send_renew_caps(struct ceph_mds_client *mdsc,
			   struct ceph_mds_session *session)
{
	struct ceph_msg *msg;
	int state;

	if (time_after_eq(jiffies, session->s_cap_ttl) &&
	    time_after_eq(session->s_cap_ttl, session->s_renew_requested))
		pr_info("ceph mds%d session caps stale\n", session->s_mds);

	/* do not try to renew caps until a recovering mds has reconnected
	 * with its clients. */
	state = ceph_mdsmap_get_state(mdsc->mdsmap, session->s_mds);
	if (state < CEPH_MDS_STATE_RECONNECT) {
		dout("send_renew_caps ignoring mds%d (%s)\n",
		     session->s_mds, ceph_mds_state_name(state));
		return 0;
	}

	dout("send_renew_caps to mds%d (%s)\n", session->s_mds,
		ceph_mds_state_name(state));
	session->s_renew_requested = jiffies;
	msg = create_session_msg(CEPH_SESSION_REQUEST_RENEWCAPS, 0);
	if (IS_ERR(msg))
		return PTR_ERR(msg);
	ceph_con_send(&session->s_con, msg);
	return 0;
}

/*
 * Note new cap ttl, and any transition from stale -> not stale (fresh?).
 */
static void renewed_caps(struct ceph_mds_client *mdsc,
			 struct ceph_mds_session *session, int is_renew)
{
	int was_stale;
	int wake = 0;

	spin_lock(&session->s_cap_lock);
	was_stale = is_renew && (session->s_cap_ttl == 0 ||
				 time_after_eq(jiffies, session->s_cap_ttl));

	session->s_cap_ttl = session->s_renew_requested +
		mdsc->mdsmap->m_session_timeout*HZ;

	if (was_stale) {
		if (time_before(jiffies, session->s_cap_ttl)) {
			pr_info("ceph mds%d caps renewed\n", session->s_mds);
			wake = 1;
		} else {
			pr_info("ceph mds%d caps still stale\n",session->s_mds);
		}
	}
	dout("renewed_caps mds%d ttl now %lu, was %s, now %s\n",
	     session->s_mds, session->s_cap_ttl, was_stale ? "stale" : "fresh",
	     time_before(jiffies, session->s_cap_ttl) ? "stale" : "fresh");
	spin_unlock(&session->s_cap_lock);

	if (wake)
		wake_up_session_caps(session);
}

/*
 * send a session close request
 */
static int request_close_session(struct ceph_mds_client *mdsc,
				 struct ceph_mds_session *session)
{
	struct ceph_msg *msg;
	int err = 0;

	dout("request_close_session mds%d state %s seq %lld\n",
	     session->s_mds, session_state_name(session->s_state),
	     session->s_seq);
	msg = create_session_msg(CEPH_SESSION_REQUEST_CLOSE,
				 session->s_seq);
	if (IS_ERR(msg))
		err = PTR_ERR(msg);
	else
		ceph_con_send(&session->s_con, msg);
	return err;
}

/*
 * Called with s_mutex held.
 */
static int __close_session(struct ceph_mds_client *mdsc,
			 struct ceph_mds_session *session)
{
	if (session->s_state >= CEPH_MDS_SESSION_CLOSING)
		return 0;
	session->s_state = CEPH_MDS_SESSION_CLOSING;
	return request_close_session(mdsc, session);
}

/*
 * Trim old(er) caps.
 *
 * Because we can't cache an inode without one or more caps, we do
 * this indirectly: if a cap is unused, we prune its aliases, at which
 * point the inode will hopefully get dropped to.
 *
 * Yes, this is a bit sloppy.  Our only real goal here is to respond to
 * memory pressure from the MDS, though, so it needn't be perfect.
 */
static int trim_caps_cb(struct inode *inode, struct ceph_cap *cap, void *arg)
{
	struct ceph_mds_session *session = arg;
	struct ceph_inode_info *ci = ceph_inode(inode);
	int used, oissued, mine;

	if (session->s_trim_caps <= 0)
		return -1;

	spin_lock(&inode->i_lock);
	mine = cap->issued | cap->implemented;
	used = __ceph_caps_used(ci);
	oissued = __ceph_caps_issued_other(ci, cap);

	dout("trim_caps_cb %p cap %p mine %s oissued %s used %s\n",
	     inode, cap, ceph_cap_string(mine), ceph_cap_string(oissued),
	     ceph_cap_string(used));
	if (ci->i_dirty_caps)
		goto out;   /* dirty caps */
	if ((used & ~oissued) & mine)
		goto out;   /* we need these caps */

	session->s_trim_caps--;
	if (oissued) {
		/* we aren't the only cap.. just remove us */
		__ceph_remove_cap(cap, NULL);
	} else {
		/* try to drop referring dentries */
		spin_unlock(&inode->i_lock);
		d_prune_aliases(inode);
		dout("trim_caps_cb %p cap %p  pruned, count now %d\n",
		     inode, cap, atomic_read(&inode->i_count));
		return 0;
	}

out:
	spin_unlock(&inode->i_lock);
	return 0;
}

/*
 * Trim session cap count down to some max number.
 */
static int trim_caps(struct ceph_mds_client *mdsc,
		     struct ceph_mds_session *session,
		     int max_caps)
{
	int trim_caps = session->s_nr_caps - max_caps;

	dout("trim_caps mds%d start: %d / %d, trim %d\n",
	     session->s_mds, session->s_nr_caps, max_caps, trim_caps);
	if (trim_caps > 0) {
		session->s_trim_caps = trim_caps;
		iterate_session_caps(session, trim_caps_cb, session);
		dout("trim_caps mds%d done: %d / %d, trimmed %d\n",
		     session->s_mds, session->s_nr_caps, max_caps,
			trim_caps - session->s_trim_caps);
	}
	return 0;
}

/*
 * Allocate cap_release messages.  If there is a partially full message
 * in the queue, try to allocate enough to cover it's remainder, so that
 * we can send it immediately.
 *
 * Called under s_mutex.
 */
static int add_cap_releases(struct ceph_mds_client *mdsc,
			    struct ceph_mds_session *session,
			    int extra)
{
	struct ceph_msg *msg;
	struct ceph_mds_cap_release *head;
	int err = -ENOMEM;

	if (extra < 0)
		extra = mdsc->client->mount_args.cap_release_safety;

	spin_lock(&session->s_cap_lock);

	if (!list_empty(&session->s_cap_releases)) {
		msg = list_first_entry(&session->s_cap_releases,
				       struct ceph_msg,
				 list_head);
		head = msg->front.iov_base;
		extra += CEPH_CAPS_PER_RELEASE - le32_to_cpu(head->num);
	}

	while (session->s_num_cap_releases < session->s_nr_caps + extra) {
		spin_unlock(&session->s_cap_lock);
		msg = ceph_msg_new(CEPH_MSG_CLIENT_CAPRELEASE, PAGE_CACHE_SIZE,
				   0, 0, NULL);
		if (!msg)
			goto out_unlocked;
		dout("add_cap_releases %p msg %p now %d\n", session, msg,
		     (int)msg->front.iov_len);
		head = msg->front.iov_base;
		head->num = cpu_to_le32(0);
		msg->front.iov_len = sizeof(*head);
		spin_lock(&session->s_cap_lock);
		list_add(&msg->list_head, &session->s_cap_releases);
		session->s_num_cap_releases += CEPH_CAPS_PER_RELEASE;
	}

	if (!list_empty(&session->s_cap_releases)) {
		msg = list_first_entry(&session->s_cap_releases,
				       struct ceph_msg,
				       list_head);
		head = msg->front.iov_base;
		if (head->num) {
			dout(" queueing non-full %p (%d)\n", msg,
			     le32_to_cpu(head->num));
			list_move_tail(&msg->list_head,
				      &session->s_cap_releases_done);
			session->s_num_cap_releases -=
				CEPH_CAPS_PER_RELEASE - le32_to_cpu(head->num);
		}
	}
	err = 0;
	spin_unlock(&session->s_cap_lock);
out_unlocked:
	return err;
}

/*
 * flush all dirty inode data to disk.
 *
 * returns true if we've flushed through want_flush_seq
 */
static int check_cap_flush(struct ceph_mds_client *mdsc, u64 want_flush_seq)
{
	int mds, ret = 1;

	dout("check_cap_flush want %lld\n", want_flush_seq);
	mutex_lock(&mdsc->mutex);
	for (mds = 0; ret && mds < mdsc->max_sessions; mds++) {
		struct ceph_mds_session *session = mdsc->sessions[mds];

		if (!session)
			continue;
		get_session(session);
		mutex_unlock(&mdsc->mutex);

		mutex_lock(&session->s_mutex);
		if (!list_empty(&session->s_cap_flushing)) {
			struct ceph_inode_info *ci =
				list_entry(session->s_cap_flushing.next,
					   struct ceph_inode_info,
					   i_flushing_item);
			struct inode *inode = &ci->vfs_inode;

			spin_lock(&inode->i_lock);
			if (ci->i_cap_flush_seq <= want_flush_seq) {
				dout("check_cap_flush still flushing %p "
				     "seq %lld <= %lld to mds%d\n", inode,
				     ci->i_cap_flush_seq, want_flush_seq,
				     session->s_mds);
				ret = 0;
			}
			spin_unlock(&inode->i_lock);
		}
		mutex_unlock(&session->s_mutex);
		ceph_put_mds_session(session);

		if (!ret)
			return ret;
		mutex_lock(&mdsc->mutex);
	}

	mutex_unlock(&mdsc->mutex);
	dout("check_cap_flush ok, flushed thru %lld\n", want_flush_seq);
	return ret;
}

/*
 * called under s_mutex
 */
static void send_cap_releases(struct ceph_mds_client *mdsc,
		       struct ceph_mds_session *session)
{
	struct ceph_msg *msg;

	dout("send_cap_releases mds%d\n", session->s_mds);
	while (1) {
		spin_lock(&session->s_cap_lock);
		if (list_empty(&session->s_cap_releases_done))
			break;
		msg = list_first_entry(&session->s_cap_releases_done,
				 struct ceph_msg, list_head);
		list_del_init(&msg->list_head);
		spin_unlock(&session->s_cap_lock);
		msg->hdr.front_len = cpu_to_le32(msg->front.iov_len);
		dout("send_cap_releases mds%d %p\n", session->s_mds, msg);
		ceph_con_send(&session->s_con, msg);
	}
	spin_unlock(&session->s_cap_lock);
}

/*
 * requests
 */

/*
 * Create an mds request.
 */
struct ceph_mds_request *
ceph_mdsc_create_request(struct ceph_mds_client *mdsc, int op, int mode)
{
	struct ceph_mds_request *req = kzalloc(sizeof(*req), GFP_NOFS);

	if (!req)
		return ERR_PTR(-ENOMEM);

	req->r_started = jiffies;
	req->r_resend_mds = -1;
	INIT_LIST_HEAD(&req->r_unsafe_dir_item);
	req->r_fmode = -1;
	atomic_set(&req->r_ref, 1);  /* one for request_tree, one for caller */
	INIT_LIST_HEAD(&req->r_wait);
	init_completion(&req->r_completion);
	init_completion(&req->r_safe_completion);
	INIT_LIST_HEAD(&req->r_unsafe_item);

	req->r_op = op;
	req->r_direct_mode = mode;
	return req;
}

/*
 * return oldest (lowest) tid in request tree, 0 if none.
 *
 * called under mdsc->mutex.
 */
static u64 __get_oldest_tid(struct ceph_mds_client *mdsc)
{
	struct ceph_mds_request *first;
	if (radix_tree_gang_lookup(&mdsc->request_tree,
				   (void **)&first, 0, 1) <= 0)
		return 0;
	return first->r_tid;
}

/*
 * Build a dentry's path.  Allocate on heap; caller must kfree.  Based
 * on build_path_from_dentry in fs/cifs/dir.c.
 *
 * If @stop_on_nosnap, generate path relative to the first non-snapped
 * inode.
 *
 * Encode hidden .snap dirs as a double /, i.e.
 *   foo/.snap/bar -> foo//bar
 */
char *ceph_mdsc_build_path(struct dentry *dentry, int *plen, u64 *base,
			   int stop_on_nosnap)
{
	struct dentry *temp;
	char *path;
	int len, pos;

	if (dentry == NULL)
		return ERR_PTR(-EINVAL);

retry:
	len = 0;
	for (temp = dentry; !IS_ROOT(temp);) {
		struct inode *inode = temp->d_inode;
		if (inode && ceph_snap(inode) == CEPH_SNAPDIR)
			len++;  /* slash only */
		else if (stop_on_nosnap && inode &&
			 ceph_snap(inode) == CEPH_NOSNAP)
			break;
		else
			len += 1 + temp->d_name.len;
		temp = temp->d_parent;
		if (temp == NULL) {
			pr_err("ceph build_path_dentry corrupt dentry %p\n",
			       dentry);
			return ERR_PTR(-EINVAL);
		}
	}
	if (len)
		len--;  /* no leading '/' */

	path = kmalloc(len+1, GFP_NOFS);
	if (path == NULL)
		return ERR_PTR(-ENOMEM);
	pos = len;
	path[pos] = 0;	/* trailing null */
	for (temp = dentry; !IS_ROOT(temp) && pos != 0; ) {
		struct inode *inode = temp->d_inode;

		if (inode && ceph_snap(inode) == CEPH_SNAPDIR) {
			dout("build_path_dentry path+%d: %p SNAPDIR\n",
			     pos, temp);
		} else if (stop_on_nosnap && inode &&
			   ceph_snap(inode) == CEPH_NOSNAP) {
			break;
		} else {
			pos -= temp->d_name.len;
			if (pos < 0)
				break;
			strncpy(path + pos, temp->d_name.name,
				temp->d_name.len);
			dout("build_path_dentry path+%d: %p '%.*s'\n",
			     pos, temp, temp->d_name.len, path + pos);
		}
		if (pos)
			path[--pos] = '/';
		temp = temp->d_parent;
		if (temp == NULL) {
			pr_err("ceph build_path_dentry corrupt dentry\n");
			kfree(path);
			return ERR_PTR(-EINVAL);
		}
	}
	if (pos != 0) {
		pr_err("ceph build_path_dentry did not end path lookup where "
		       "expected, namelen is %d, pos is %d\n", len, pos);
		/* presumably this is only possible if racing with a
		   rename of one of the parent directories (we can not
		   lock the dentries above us to prevent this, but
		   retrying should be harmless) */
		kfree(path);
		goto retry;
	}

	*base = ceph_ino(temp->d_inode);
	*plen = len;
	dout("build_path_dentry on %p %d built %llx '%.*s'\n",
	     dentry, atomic_read(&dentry->d_count), *base, len, path);
	return path;
}

static int build_dentry_path(struct dentry *dentry,
			     const char **ppath, int *ppathlen, u64 *pino,
			     int *pfreepath)
{
	char *path;

	if (ceph_snap(dentry->d_parent->d_inode) == CEPH_NOSNAP) {
		*pino = ceph_ino(dentry->d_parent->d_inode);
		*ppath = dentry->d_name.name;
		*ppathlen = dentry->d_name.len;
		return 0;
	}
	path = ceph_mdsc_build_path(dentry, ppathlen, pino, 1);
	if (IS_ERR(path))
		return PTR_ERR(path);
	*ppath = path;
	*pfreepath = 1;
	return 0;
}

static int build_inode_path(struct inode *inode,
			    const char **ppath, int *ppathlen, u64 *pino,
			    int *pfreepath)
{
	struct dentry *dentry;
	char *path;

	if (ceph_snap(inode) == CEPH_NOSNAP) {
		*pino = ceph_ino(inode);
		*ppathlen = 0;
		return 0;
	}
	dentry = d_find_alias(inode);
	path = ceph_mdsc_build_path(dentry, ppathlen, pino, 1);
	dput(dentry);
	if (IS_ERR(path))
		return PTR_ERR(path);
	*ppath = path;
	*pfreepath = 1;
	return 0;
}

/*
 * request arguments may be specified via an inode *, a dentry *, or
 * an explicit ino+path.
 */
static int set_request_path_attr(struct inode *rinode, struct dentry *rdentry,
				  const char *rpath, u64 rino,
				  const char **ppath, int *pathlen,
				  u64 *ino, int *freepath)
{
	int r = 0;

	if (rinode) {
		r = build_inode_path(rinode, ppath, pathlen, ino, freepath);
		dout(" inode %p %llx.%llx\n", rinode, ceph_ino(rinode),
		     ceph_snap(rinode));
	} else if (rdentry) {
		r = build_dentry_path(rdentry, ppath, pathlen, ino, freepath);
		dout(" dentry %p %llx/%.*s\n", rdentry, *ino, *pathlen,
		     *ppath);
	} else if (rpath) {
		*ino = rino;
		*ppath = rpath;
		*pathlen = strlen(rpath);
		dout(" path %.*s\n", *pathlen, rpath);
	}

	return r;
}

/*
 * called under mdsc->mutex
 */
static struct ceph_msg *create_request_message(struct ceph_mds_client *mdsc,
					       struct ceph_mds_request *req,
					       int mds)
{
	struct ceph_msg *msg;
	struct ceph_mds_request_head *head;
	const char *path1 = 0;
	const char *path2 = 0;
	u64 ino1 = 0, ino2 = 0;
	int pathlen1 = 0, pathlen2 = 0;
	int freepath1 = 0, freepath2 = 0;
	int len;
	u16 releases;
	void *p, *end;
	int ret;

	ret = set_request_path_attr(req->r_inode, req->r_dentry,
			      req->r_path1, req->r_ino1.ino,
			      &path1, &pathlen1, &ino1, &freepath1);
	if (ret < 0) {
		msg = ERR_PTR(ret);
		goto out;
	}

	ret = set_request_path_attr(NULL, req->r_old_dentry,
			      req->r_path2, req->r_ino2.ino,
			      &path2, &pathlen2, &ino2, &freepath2);
	if (ret < 0) {
		msg = ERR_PTR(ret);
		goto out_free1;
	}

	len = sizeof(*head) +
		pathlen1 + pathlen2 + 2*(sizeof(u32) + sizeof(u64));

	/* calculate (max) length for cap releases */
	len += sizeof(struct ceph_mds_request_release) *
		(!!req->r_inode_drop + !!req->r_dentry_drop +
		 !!req->r_old_inode_drop + !!req->r_old_dentry_drop);
	if (req->r_dentry_drop)
		len += req->r_dentry->d_name.len;
	if (req->r_old_dentry_drop)
		len += req->r_old_dentry->d_name.len;

	msg = ceph_msg_new(CEPH_MSG_CLIENT_REQUEST, len, 0, 0, NULL);
	if (IS_ERR(msg))
		goto out_free2;

	head = msg->front.iov_base;
	p = msg->front.iov_base + sizeof(*head);
	end = msg->front.iov_base + msg->front.iov_len;

	head->mdsmap_epoch = cpu_to_le32(mdsc->mdsmap->m_epoch);
	head->op = cpu_to_le32(req->r_op);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2, 6, 29)
	head->caller_uid = cpu_to_le32(current_fsuid());
	head->caller_gid = cpu_to_le32(current_fsgid());
#else
	head->caller_uid = cpu_to_le32(current->fsuid);
	head->caller_gid = cpu_to_le32(current->fsgid);
#endif
	head->args = req->r_args;

	ceph_encode_filepath(&p, end, ino1, path1);
	ceph_encode_filepath(&p, end, ino2, path2);

	/* cap releases */
	releases = 0;
	if (req->r_inode_drop)
		releases += ceph_encode_inode_release(&p,
		      req->r_inode ? req->r_inode : req->r_dentry->d_inode,
		      mds, req->r_inode_drop, req->r_inode_unless, 0);
	if (req->r_dentry_drop)
		releases += ceph_encode_dentry_release(&p, req->r_dentry,
		       mds, req->r_dentry_drop, req->r_dentry_unless);
	if (req->r_old_dentry_drop)
		releases += ceph_encode_dentry_release(&p, req->r_old_dentry,
		       mds, req->r_old_dentry_drop, req->r_old_dentry_unless);
	if (req->r_old_inode_drop)
		releases += ceph_encode_inode_release(&p,
		      req->r_old_dentry->d_inode,
		      mds, req->r_old_inode_drop, req->r_old_inode_unless, 0);
	head->num_releases = cpu_to_le16(releases);

	BUG_ON(p > end);
	msg->front.iov_len = p - msg->front.iov_base;
	msg->hdr.front_len = cpu_to_le32(msg->front.iov_len);

	msg->pages = req->r_pages;
	msg->nr_pages = req->r_num_pages;
	msg->hdr.data_len = cpu_to_le32(req->r_data_len);
	msg->hdr.data_off = cpu_to_le16(0);

out_free2:
	if (freepath2)
		kfree((char *)path2);
out_free1:
	if (freepath1)
		kfree((char *)path1);
out:
	return msg;
}

/*
 * called under mdsc->mutex if error, under no mutex if
 * success.
 */
static void complete_request(struct ceph_mds_client *mdsc,
			     struct ceph_mds_request *req)
{
	if (req->r_callback)
		req->r_callback(mdsc, req);
	else
		complete(&req->r_completion);
}

/*
 * called under mdsc->mutex
 */
static int __prepare_send_request(struct ceph_mds_client *mdsc,
				  struct ceph_mds_request *req,
				  int mds)
{
	struct ceph_mds_request_head *rhead;
	struct ceph_msg *msg;
	int flags = 0;

	req->r_mds = mds;
	req->r_attempts++;
	dout("prepare_send_request %p tid %lld %s (attempt %d)\n", req,
	     req->r_tid, ceph_mds_op_name(req->r_op), req->r_attempts);

	if (req->r_request) {
		ceph_msg_put(req->r_request);
		req->r_request = NULL;
	}
	msg = create_request_message(mdsc, req, mds);
	if (IS_ERR(msg)) {
		req->r_reply = ERR_PTR(PTR_ERR(msg));
		complete_request(mdsc, req);
		return -PTR_ERR(msg);
	}
	req->r_request = msg;

	rhead = msg->front.iov_base;
	rhead->tid = cpu_to_le64(req->r_tid);
	rhead->oldest_client_tid = cpu_to_le64(__get_oldest_tid(mdsc));
	if (req->r_got_unsafe)
		flags |= CEPH_MDS_FLAG_REPLAY;
	if (req->r_locked_dir)
		flags |= CEPH_MDS_FLAG_WANT_DENTRY;
	rhead->flags = cpu_to_le32(flags);
	rhead->num_fwd = req->r_num_fwd;
	rhead->num_retry = req->r_attempts - 1;

	dout(" r_locked_dir = %p\n", req->r_locked_dir);

	if (req->r_target_inode && req->r_got_unsafe)
		rhead->ino = cpu_to_le64(ceph_ino(req->r_target_inode));
	else
		rhead->ino = 0;
	return 0;
}

/*
 * send request, or put it on the appropriate wait list.
 */
static int __do_request(struct ceph_mds_client *mdsc,
			struct ceph_mds_request *req)
{
	struct ceph_mds_session *session = NULL;
	int mds = -1;
	int err = -EAGAIN;

	if (req->r_reply)
		goto out;

	if (req->r_timeout &&
	    time_after_eq(jiffies, req->r_started + req->r_timeout)) {
		dout("do_request timed out\n");
		err = -EIO;
		goto finish;
	}

	mds = __choose_mds(mdsc, req);
	if (mds < 0 ||
	    ceph_mdsmap_get_state(mdsc->mdsmap, mds) < CEPH_MDS_STATE_ACTIVE) {
		dout("do_request no mds or not active, waiting for map\n");
		list_add(&req->r_wait, &mdsc->waiting_for_map);
		ceph_monc_request_mdsmap(&mdsc->client->monc,
					 mdsc->mdsmap->m_epoch+1);
		goto out;
	}

	/* get, open session */
	session = __ceph_lookup_mds_session(mdsc, mds);
	if (!session)
		session = register_session(mdsc, mds);
	dout("do_request mds%d session %p state %s\n", mds, session,
	     session_state_name(session->s_state));
	if (session->s_state != CEPH_MDS_SESSION_OPEN &&
	    session->s_state != CEPH_MDS_SESSION_HUNG) {
		if (session->s_state == CEPH_MDS_SESSION_NEW ||
		    session->s_state == CEPH_MDS_SESSION_CLOSING)
			__open_session(mdsc, session);
		list_add(&req->r_wait, &session->s_waiting);
		goto out_session;
	}

	/* send request */
	req->r_session = get_session(session);
	req->r_resend_mds = -1;   /* forget any previous mds hint */

	if (req->r_request_started == 0)   /* note request start time */
		req->r_request_started = jiffies;

	err = __prepare_send_request(mdsc, req, mds);
	if (!err) {
		ceph_msg_get(req->r_request);
		ceph_con_send(&session->s_con, req->r_request);
	}

out_session:
	ceph_put_mds_session(session);
out:
	return err;

finish:
	req->r_reply = ERR_PTR(err);
	complete_request(mdsc, req);
	goto out;
}

/*
 * called under mdsc->mutex
 */
static void __wake_requests(struct ceph_mds_client *mdsc,
			    struct list_head *head)
{
	struct ceph_mds_request *req, *nreq;

	list_for_each_entry_safe(req, nreq, head, r_wait) {
		list_del_init(&req->r_wait);
		__do_request(mdsc, req);
	}
}

/*
 * Wake up threads with requests pending for @mds, so that they can
 * resubmit their requests to a possibly different mds.  If @all is set,
 * wake up if their requests has been forwarded to @mds, too.
 */
static void kick_requests(struct ceph_mds_client *mdsc, int mds, int all)
{
	struct ceph_mds_request *reqs[10];
	u64 nexttid = 0;
	int i, got;

	dout("kick_requests mds%d\n", mds);
	while (nexttid <= mdsc->last_tid) {
		got = radix_tree_gang_lookup(&mdsc->request_tree,
					     (void **)&reqs, nexttid, 10);
		if (got == 0)
			break;
		nexttid = reqs[got-1]->r_tid + 1;
		for (i = 0; i < got; i++) {
			if (reqs[i]->r_got_unsafe)
				continue;
			if (((reqs[i]->r_session &&
			      reqs[i]->r_session->s_mds == mds) ||
			     (all && reqs[i]->r_fwd_session &&
			      reqs[i]->r_fwd_session->s_mds == mds))) {
				dout(" kicking tid %llu\n", reqs[i]->r_tid);
				put_request_sessions(reqs[i]);
				__do_request(mdsc, reqs[i]);
			}
		}
	}
}

void ceph_mdsc_submit_request(struct ceph_mds_client *mdsc,
			      struct ceph_mds_request *req)
{
	dout("submit_request on %p\n", req);
	mutex_lock(&mdsc->mutex);
	__register_request(mdsc, req, NULL);
	__do_request(mdsc, req);
	mutex_unlock(&mdsc->mutex);
}

/*
 * Synchrously perform an mds request.  Take care of all of the
 * session setup, forwarding, retry details.
 */
int ceph_mdsc_do_request(struct ceph_mds_client *mdsc,
			 struct inode *dir,
			 struct ceph_mds_request *req)
{
	int err;

	dout("do_request on %p\n", req);

	/* take CAP_PIN refs for r_inode, r_locked_dir, r_old_dentry */
	if (req->r_inode)
		ceph_get_cap_refs(ceph_inode(req->r_inode), CEPH_CAP_PIN);
	if (req->r_locked_dir)
		ceph_get_cap_refs(ceph_inode(req->r_locked_dir), CEPH_CAP_PIN);
	if (req->r_old_dentry)
		ceph_get_cap_refs(ceph_inode(req->r_old_dentry->d_parent->d_inode),
				  CEPH_CAP_PIN);

	/* issue */
	mutex_lock(&mdsc->mutex);
	__register_request(mdsc, req, dir);
	__do_request(mdsc, req);

	/* wait */
	if (!req->r_reply) {
		mutex_unlock(&mdsc->mutex);
		if (req->r_timeout) {
			err = wait_for_completion_timeout(&req->r_completion,
							  req->r_timeout);
			if (err > 0)
				err = 0;
			else if (err == 0)
				req->r_reply = ERR_PTR(-EIO);
		} else {
			wait_for_completion(&req->r_completion);
		}
		mutex_lock(&mdsc->mutex);
	}

	if (IS_ERR(req->r_reply)) {
		err = PTR_ERR(req->r_reply);
		req->r_reply = NULL;

		/* clean up */
		__unregister_request(mdsc, req);
		if (!list_empty(&req->r_unsafe_item))
			list_del_init(&req->r_unsafe_item);
		complete(&req->r_safe_completion);
	} else if (req->r_err) {
		err = req->r_err;
	} else {
		err = le32_to_cpu(req->r_reply_info.head->result);
	}
	mutex_unlock(&mdsc->mutex);

	dout("do_request %p done, result %d\n", req, err);
	return err;
}

/*
 * Handle mds reply.
 *
 * We take the session mutex and parse and process the reply immediately.
 * This preserves the logical ordering of replies, capabilities, etc., sent
 * by the MDS as they are applied to our local cache.
 */
static void handle_reply(struct ceph_mds_client *mdsc, struct ceph_msg *msg)
{
	struct ceph_mds_request *req;
	struct ceph_mds_reply_head *head = msg->front.iov_base;
	struct ceph_mds_reply_info_parsed *rinfo;  /* parsed reply info */
	u64 tid;
	int err, result;
	int mds;

	if (le32_to_cpu(msg->hdr.src.name.type) != CEPH_ENTITY_TYPE_MDS)
		return;
	if (msg->front.iov_len < sizeof(*head)) {
		pr_err("ceph_mdsc_handle_reply got corrupt (short) reply\n");
		return;
	}

	/* get request, session */
	tid = le64_to_cpu(head->tid);
	mutex_lock(&mdsc->mutex);
	req = __lookup_request(mdsc, tid);
	if (!req) {
		dout("handle_reply on unknown tid %llu\n", tid);
		mutex_unlock(&mdsc->mutex);
		return;
	}
	dout("handle_reply %p\n", req);
	mds = le32_to_cpu(msg->hdr.src.name.num);

	/* dup? */
	if ((req->r_got_unsafe && !head->safe) ||
	    (req->r_got_safe && head->safe)) {
		pr_warning("ceph got a dup %s reply on %llu from mds%d\n",
			   head->safe ? "safe" : "unsafe", tid, mds);
		mutex_unlock(&mdsc->mutex);
		goto out;
	}

	if (head->safe) {
		req->r_got_safe = true;
		__unregister_request(mdsc, req);
		complete(&req->r_safe_completion);

		if (req->r_got_unsafe) {
			/*
			 * We already handled the unsafe response, now do the
			 * cleanup.  No need to examine the response; the MDS
			 * doesn't include any result info in the safe
			 * response.  And even if it did, there is nothing
			 * useful we could do with a revised return value.
			 */
			dout("got safe reply %llu, mds%d\n", tid, mds);
			BUG_ON(req->r_session == NULL);
			list_del_init(&req->r_unsafe_item);

			/* last unsafe request during umount? */
			if (mdsc->stopping && !__get_oldest_tid(mdsc))
				complete(&mdsc->safe_umount_waiters);
			mutex_unlock(&mdsc->mutex);
			goto out;
		}
	}

	if (req->r_session && req->r_session->s_mds != mds) {
		ceph_put_mds_session(req->r_session);
		req->r_session = __ceph_lookup_mds_session(mdsc, mds);
	}
	if (req->r_session == NULL) {
		pr_err("ceph_mdsc_handle_reply got %llu, but no session for"
		       " mds%d\n", tid, mds);
		mutex_unlock(&mdsc->mutex);
		goto out;
	}
	BUG_ON(req->r_reply);

	if (!head->safe) {
		req->r_got_unsafe = true;
		list_add_tail(&req->r_unsafe_item, &req->r_session->s_unsafe);
	}

	mutex_unlock(&mdsc->mutex);

	mutex_lock(&req->r_session->s_mutex);

	/* parse */
	rinfo = &req->r_reply_info;
	err = parse_reply_info(msg, rinfo);
	if (err < 0) {
		pr_err("ceph_mdsc_handle_reply got corrupt reply mds%d\n", mds);
		goto out_err;
	}
	result = le32_to_cpu(rinfo->head->result);
	dout("handle_reply tid %lld result %d\n", tid, result);

	/*
	 * Tolerate 2 consecutive ESTALEs from the same mds.
	 * FIXME: we should be looking at the cap migrate_seq.
	 */
	if (result == -ESTALE) {
		req->r_direct_mode = USE_AUTH_MDS;
		req->r_num_stale++;
		if (req->r_num_stale <= 2) {
			mutex_unlock(&req->r_session->s_mutex);
			mutex_lock(&mdsc->mutex);
			put_request_sessions(req);
			__do_request(mdsc, req);
			mutex_unlock(&mdsc->mutex);
			goto out;
		}
	} else {
		req->r_num_stale = 0;
	}

	/* snap trace */
	if (rinfo->snapblob_len) {
		down_write(&mdsc->snap_rwsem);
		ceph_update_snap_trace(mdsc, rinfo->snapblob,
			       rinfo->snapblob + rinfo->snapblob_len,
			       le32_to_cpu(head->op) == CEPH_MDS_OP_RMSNAP);
		downgrade_write(&mdsc->snap_rwsem);
	} else {
		down_read(&mdsc->snap_rwsem);
	}

	/* insert trace into our cache */
	err = ceph_fill_trace(mdsc->client->sb, req, req->r_session);
	if (err == 0) {
		if (result == 0 && rinfo->dir_nr)
			ceph_readdir_prepopulate(req, req->r_session);
		ceph_unreserve_caps(&req->r_caps_reservation);
	}

	up_read(&mdsc->snap_rwsem);
out_err:
	if (err) {
		req->r_err = err;
	} else {
		req->r_reply = msg;
		ceph_msg_get(msg);
	}

	add_cap_releases(mdsc, req->r_session, -1);
	mutex_unlock(&req->r_session->s_mutex);

	/* kick calling process */
	complete_request(mdsc, req);
out:
	ceph_mdsc_put_request(req);
	return;
}



/*
 * handle mds notification that our request has been forwarded.
 */
static void handle_forward(struct ceph_mds_client *mdsc, struct ceph_msg *msg)
{
	struct ceph_mds_request *req;
	u64 tid;
	u32 next_mds;
	u32 fwd_seq;
	u8 must_resend;
	int err = -EINVAL;
	void *p = msg->front.iov_base;
	void *end = p + msg->front.iov_len;
	int from_mds, state;

	if (le32_to_cpu(msg->hdr.src.name.type) != CEPH_ENTITY_TYPE_MDS)
		goto bad;
	from_mds = le32_to_cpu(msg->hdr.src.name.num);

	ceph_decode_need(&p, end, sizeof(u64)+2*sizeof(u32), bad);
	ceph_decode_64(&p, tid);
	ceph_decode_32(&p, next_mds);
	ceph_decode_32(&p, fwd_seq);
	ceph_decode_8(&p, must_resend);

	mutex_lock(&mdsc->mutex);
	req = __lookup_request(mdsc, tid);
	if (!req) {
		dout("forward %llu dne\n", tid);
		goto out;  /* dup reply? */
	}

	state = mdsc->sessions[next_mds]->s_state;
	if (fwd_seq <= req->r_num_fwd) {
		dout("forward %llu to mds%d - old seq %d <= %d\n",
		     tid, next_mds, req->r_num_fwd, fwd_seq);
	} else if (!must_resend &&
		   __have_session(mdsc, next_mds) &&
		   (state == CEPH_MDS_SESSION_OPEN ||
		    state == CEPH_MDS_SESSION_HUNG)) {
		/* yes.  adjust our sessions, but that's all; the old mds
		 * forwarded our message for us. */
		dout("forward %llu to mds%d (mds%d fwded)\n", tid, next_mds,
		     from_mds);
		req->r_num_fwd = fwd_seq;
		put_request_sessions(req);
		req->r_session = __ceph_lookup_mds_session(mdsc, next_mds);
		req->r_fwd_session = __ceph_lookup_mds_session(mdsc, from_mds);
	} else {
		/* no, resend. */
		/* forward race not possible; mds would drop */
		dout("forward %llu to mds%d (we resend)\n", tid, next_mds);
		req->r_num_fwd = fwd_seq;
		req->r_resend_mds = next_mds;
		put_request_sessions(req);
		__do_request(mdsc, req);
	}
	ceph_mdsc_put_request(req);
out:
	mutex_unlock(&mdsc->mutex);
	return;

bad:
	pr_err("ceph_mdsc_handle_forward decode error err=%d\n", err);
}

/*
 * handle a mds session control message
 */
static void handle_session(struct ceph_mds_client *mdsc, struct ceph_msg *msg)
{
	u32 op;
	u64 seq;
	struct ceph_mds_session *session = NULL;
	int mds;
	struct ceph_mds_session_head *h = msg->front.iov_base;
	int wake = 0;

	if (le32_to_cpu(msg->hdr.src.name.type) != CEPH_ENTITY_TYPE_MDS)
		return;
	mds = le32_to_cpu(msg->hdr.src.name.num);

	/* decode */
	if (msg->front.iov_len != sizeof(*h))
		goto bad;
	op = le32_to_cpu(h->op);
	seq = le64_to_cpu(h->seq);

	mutex_lock(&mdsc->mutex);
	session = __ceph_lookup_mds_session(mdsc, mds);
	if (session && mdsc->mdsmap)
		/* FIXME: this ttl calculation is generous */
		session->s_ttl = jiffies + HZ*mdsc->mdsmap->m_session_autoclose;
	mutex_unlock(&mdsc->mutex);

	if (!session) {
		if (op != CEPH_SESSION_OPEN) {
			dout("handle_session no session for mds%d\n", mds);
			return;
		}
		dout("handle_session creating session for mds%d\n", mds);
		session = register_session(mdsc, mds);
	}

	mutex_lock(&session->s_mutex);

	dout("handle_session mds%d %s %p state %s seq %llu\n",
	     mds, ceph_session_op_name(op), session,
	     session_state_name(session->s_state), seq);

	if (session->s_state == CEPH_MDS_SESSION_HUNG) {
		session->s_state = CEPH_MDS_SESSION_OPEN;
		pr_info("ceph mds%d session came back\n", session->s_mds);
	}

	switch (op) {
	case CEPH_SESSION_OPEN:
		session->s_state = CEPH_MDS_SESSION_OPEN;
		renewed_caps(mdsc, session, 0);
		wake = 1;
		if (mdsc->stopping)
			__close_session(mdsc, session);
		break;

	case CEPH_SESSION_RENEWCAPS:
		renewed_caps(mdsc, session, 1);
		break;

	case CEPH_SESSION_CLOSE:
		unregister_session(mdsc, mds);
		remove_session_caps(session);
		wake = 1; /* for good measure */
		complete(&mdsc->session_close_waiters);
		kick_requests(mdsc, mds, 0);      /* cur only */
		break;

	case CEPH_SESSION_STALE:
		pr_info("ceph mds%d caps went stale, renewing\n",
			session->s_mds);
		spin_lock(&session->s_cap_lock);
		session->s_cap_gen++;
		session->s_cap_ttl = 0;
		spin_unlock(&session->s_cap_lock);
		send_renew_caps(mdsc, session);
		break;

	case CEPH_SESSION_RECALL_STATE:
		trim_caps(mdsc, session, le32_to_cpu(h->max_caps));
		break;

	default:
		pr_err("ceph_mdsc_handle_session bad op %d mds%d\n", op, mds);
		WARN_ON(1);
	}

	mutex_unlock(&session->s_mutex);
	if (wake) {
		mutex_lock(&mdsc->mutex);
		__wake_requests(mdsc, &session->s_waiting);
		mutex_unlock(&mdsc->mutex);
	}
	ceph_put_mds_session(session);
	return;

bad:
	pr_err("ceph_mdsc_handle_session corrupt message mds%d len %d\n", mds,
	       (int)msg->front.iov_len);
	return;
}


/*
 * called under session->mutex.
 */
static void replay_unsafe_requests(struct ceph_mds_client *mdsc,
				   struct ceph_mds_session *session)
{
	struct ceph_mds_request *req, *nreq;
	int err;

	dout("replay_unsafe_requests mds%d\n", session->s_mds);

	mutex_lock(&mdsc->mutex);
	list_for_each_entry_safe(req, nreq, &session->s_unsafe, r_unsafe_item) {
		err = __prepare_send_request(mdsc, req, session->s_mds);
		if (!err) {
			ceph_msg_get(req->r_request);
			ceph_con_send(&session->s_con, req->r_request);
		}
	}
	mutex_unlock(&mdsc->mutex);
}

/*
 * Encode information about a cap for a reconnect with the MDS.
 */
struct encode_caps_data {
	void **pp;
	void *end;
	int *num_caps;
};

static int encode_caps_cb(struct inode *inode, struct ceph_cap *cap,
			  void *arg)
{
	struct ceph_mds_cap_reconnect *rec;
	struct ceph_inode_info *ci;
	struct encode_caps_data *data = (struct encode_caps_data *)arg;
	void *p = *(data->pp);
	void *end = data->end;
	char *path;
	int pathlen, err;
	u64 pathbase;
	struct dentry *dentry;

	ci = cap->ci;

	dout(" adding %p ino %llx.%llx cap %p %lld %s\n",
	     inode, ceph_vinop(inode), cap, cap->cap_id,
	     ceph_cap_string(cap->issued));
	ceph_decode_need(&p, end, sizeof(u64), needmore);
	ceph_encode_64(&p, ceph_ino(inode));

	dentry = d_find_alias(inode);
	if (dentry) {
		path = ceph_mdsc_build_path(dentry, &pathlen, &pathbase, 0);
		if (IS_ERR(path)) {
			err = PTR_ERR(path);
			BUG_ON(err);
		}
	} else {
		path = NULL;
		pathlen = 0;
	}
	ceph_decode_need(&p, end, pathlen+4, needmore);
	ceph_encode_string(&p, end, path, pathlen);

	ceph_decode_need(&p, end, sizeof(*rec), needmore);
	rec = p;
	p += sizeof(*rec);
	BUG_ON(p > end);
	spin_lock(&inode->i_lock);
	cap->seq = 0;        /* reset cap seq */
	cap->issue_seq = 0;  /* and issue_seq */
	rec->cap_id = cpu_to_le64(cap->cap_id);
	rec->pathbase = cpu_to_le64(pathbase);
	rec->wanted = cpu_to_le32(__ceph_caps_wanted(ci));
	rec->issued = cpu_to_le32(cap->issued);
	rec->size = cpu_to_le64(inode->i_size);
	ceph_encode_timespec(&rec->mtime, &inode->i_mtime);
	ceph_encode_timespec(&rec->atime, &inode->i_atime);
	rec->snaprealm = cpu_to_le64(ci->i_snap_realm->ino);
	spin_unlock(&inode->i_lock);

	kfree(path);
	dput(dentry);
	(*data->num_caps)++;
	*(data->pp) = p;
	return 0;
needmore:
	return -ENOSPC;
}


/*
 * If an MDS fails and recovers, clients need to reconnect in order to
 * reestablish shared state.  This includes all caps issued through
 * this session _and_ the snap_realm hierarchy.  Because it's not
 * clear which snap realms the mds cares about, we send everything we
 * know about.. that ensures we'll then get any new info the
 * recovering MDS might have.
 *
 * This is a relatively heavyweight operation, but it's rare.
 *
 * called with mdsc->mutex held.
 */
static void send_mds_reconnect(struct ceph_mds_client *mdsc, int mds)
{
	struct ceph_mds_session *session;
	struct ceph_msg *reply;
	int newlen, len = 4 + 1;
	void *p, *end;
	int err;
	int num_caps, num_realms = 0;
	int got;
	u64 next_snap_ino = 0;
	__le32 *pnum_caps, *pnum_realms;
	struct encode_caps_data iter_args;

	pr_info("ceph reconnect to recovering mds%d\n", mds);

	/* find session */
	session = __ceph_lookup_mds_session(mdsc, mds);
	mutex_unlock(&mdsc->mutex);    /* drop lock for duration */

	if (session) {
		mutex_lock(&session->s_mutex);

		session->s_state = CEPH_MDS_SESSION_RECONNECTING;
		session->s_seq = 0;

		/* replay unsafe requests */
		replay_unsafe_requests(mdsc, session);

		/* estimate needed space */
		len += session->s_nr_caps *
			(100+sizeof(struct ceph_mds_cap_reconnect));
		pr_info("estimating i need %d bytes for %d caps\n",
		     len, session->s_nr_caps);
	} else {
		dout("no session for mds%d, will send short reconnect\n",
		     mds);
	}

	down_read(&mdsc->snap_rwsem);

retry:
	/* build reply */
	reply = ceph_msg_new(CEPH_MSG_CLIENT_RECONNECT, len, 0, 0, NULL);
	if (IS_ERR(reply)) {
		err = PTR_ERR(reply);
		pr_err("ceph send_mds_reconnect ENOMEM on %d for mds%d\n",
		       len, mds);
		goto out;
	}
	p = reply->front.iov_base;
	end = p + len;

	if (!session) {
		ceph_encode_8(&p, 1); /* session was closed */
		ceph_encode_32(&p, 0);
		goto send;
	}
	dout("session %p state %s\n", session,
	     session_state_name(session->s_state));

	/* traverse this session's caps */
	ceph_encode_8(&p, 0);
	pnum_caps = p;
	ceph_encode_32(&p, session->s_nr_caps);
	num_caps = 0;

	iter_args.pp = &p;
	iter_args.end = end;
	iter_args.num_caps = &num_caps;
	err = iterate_session_caps(session, encode_caps_cb, &iter_args);
	if (err == -ENOSPC)
		goto needmore;
	if (err < 0)
		goto out;
	*pnum_caps = cpu_to_le32(num_caps);

	/*
	 * snaprealms.  we provide mds with the ino, seq (version), and
	 * parent for all of our realms.  If the mds has any newer info,
	 * it will tell us.
	 */
	next_snap_ino = 0;
	/* save some space for the snaprealm count */
	pnum_realms = p;
	ceph_decode_need(&p, end, sizeof(*pnum_realms), needmore);
	p += sizeof(*pnum_realms);
	num_realms = 0;
	while (1) {
		struct ceph_snap_realm *realm;
		struct ceph_mds_snaprealm_reconnect *sr_rec;
		got = radix_tree_gang_lookup(&mdsc->snap_realms,
					     (void **)&realm, next_snap_ino, 1);
		if (!got)
			break;

		dout(" adding snap realm %llx seq %lld parent %llx\n",
		     realm->ino, realm->seq, realm->parent_ino);
		ceph_decode_need(&p, end, sizeof(*sr_rec), needmore);
		sr_rec = p;
		sr_rec->ino = cpu_to_le64(realm->ino);
		sr_rec->seq = cpu_to_le64(realm->seq);
		sr_rec->parent = cpu_to_le64(realm->parent_ino);
		p += sizeof(*sr_rec);
		num_realms++;
		next_snap_ino = realm->ino + 1;
	}
	*pnum_realms = cpu_to_le32(num_realms);

send:
	reply->front.iov_len = p - reply->front.iov_base;
	reply->hdr.front_len = cpu_to_le32(reply->front.iov_len);
	dout("final len was %u (guessed %d)\n",
	     (unsigned)reply->front.iov_len, len);
	ceph_con_send(&session->s_con, reply);

	if (session) {
		session->s_state = CEPH_MDS_SESSION_OPEN;
		__wake_requests(mdsc, &session->s_waiting);
	}

out:
	up_read(&mdsc->snap_rwsem);
	if (session) {
		mutex_unlock(&session->s_mutex);
		ceph_put_mds_session(session);
	}
	mutex_lock(&mdsc->mutex);
	return;

needmore:
	/*
	 * we need a larger buffer.  this doesn't very accurately
	 * factor in snap realms, but it's safe.
	 */
	num_caps += num_realms;
	newlen = len * ((100 * (session->s_nr_caps+3)) / (num_caps + 1)) / 100;
	pr_info("i guessed %d, and did %d of %d caps, retrying with %d\n",
	     len, num_caps, session->s_nr_caps, newlen);
	len = newlen;
	ceph_msg_put(reply);
	goto retry;
}


/*
 * compare old and new mdsmaps, kicking requests
 * and closing out old connections as necessary
 *
 * called under mdsc->mutex.
 */
static void check_new_map(struct ceph_mds_client *mdsc,
			  struct ceph_mdsmap *newmap,
			  struct ceph_mdsmap *oldmap)
{
	int i;
	int oldstate, newstate;
	struct ceph_mds_session *s;

	dout("check_new_map new %u old %u\n",
	     newmap->m_epoch, oldmap->m_epoch);

	for (i = 0; i < oldmap->m_max_mds && i < mdsc->max_sessions; i++) {
		if (mdsc->sessions[i] == NULL)
			continue;
		s = mdsc->sessions[i];
		oldstate = ceph_mdsmap_get_state(oldmap, i);
		newstate = ceph_mdsmap_get_state(newmap, i);

		dout("check_new_map mds%d state %s -> %s (session %s)\n",
		     i, ceph_mds_state_name(oldstate),
		     ceph_mds_state_name(newstate),
		     session_state_name(s->s_state));

		if (memcmp(ceph_mdsmap_get_addr(oldmap, i),
			   ceph_mdsmap_get_addr(newmap, i),
			   sizeof(struct ceph_entity_addr))) {
			/* notify messenger to close out old messages,
			 * socket. */
			ceph_con_close(&s->s_con);

			if (s->s_state == CEPH_MDS_SESSION_OPENING) {
				/* the session never opened, just close it
				 * out now */
				__wake_requests(mdsc, &s->s_waiting);
				unregister_session(mdsc, i);
			}

			/* kick any requests waiting on the recovering mds */
			kick_requests(mdsc, i, 1);
		} else if (oldstate == newstate) {
			continue;  /* nothing new with this mds */
		}

		/*
		 * send reconnect?
		 */
		if (newstate == CEPH_MDS_STATE_RECONNECT)
			send_mds_reconnect(mdsc, i);

		/*
		 * kick requests on any mds that has gone active.
		 *
		 * kick requests on cur or forwarder: we may have sent
		 * the request to mds1, mds1 told us it forwarded it
		 * to mds2, but then we learn mds1 failed and can't be
		 * sure it successfully forwarded our request before
		 * it died.
		 */
		if (oldstate < CEPH_MDS_STATE_ACTIVE &&
		    newstate >= CEPH_MDS_STATE_ACTIVE) {
			kick_requests(mdsc, i, 1);
			ceph_kick_flushing_caps(mdsc, s);
		}
	}
}



/*
 * leases
 */

/*
 * caller must hold session s_mutex, dentry->d_lock
 */
void __ceph_mdsc_drop_dentry_lease(struct dentry *dentry)
{
	struct ceph_dentry_info *di = ceph_dentry(dentry);

	ceph_put_mds_session(di->lease_session);
	di->lease_session = NULL;
}

static void handle_lease(struct ceph_mds_client *mdsc, struct ceph_msg *msg)
{
	struct super_block *sb = mdsc->client->sb;
	struct inode *inode;
	struct ceph_mds_session *session;
	struct ceph_inode_info *ci;
	struct dentry *parent, *dentry;
	struct ceph_dentry_info *di;
	int mds;
	struct ceph_mds_lease *h = msg->front.iov_base;
	struct ceph_vino vino;
	int mask;
	struct qstr dname;
	int release = 0;

	if (le32_to_cpu(msg->hdr.src.name.type) != CEPH_ENTITY_TYPE_MDS)
		return;
	mds = le32_to_cpu(msg->hdr.src.name.num);
	dout("handle_lease from mds%d\n", mds);

	/* decode */
	if (msg->front.iov_len < sizeof(*h) + sizeof(u32))
		goto bad;
	vino.ino = le64_to_cpu(h->ino);
	vino.snap = CEPH_NOSNAP;
	mask = le16_to_cpu(h->mask);
	dname.name = (void *)h + sizeof(*h) + sizeof(u32);
	dname.len = msg->front.iov_len - sizeof(*h) - sizeof(u32);
	if (dname.len != get_unaligned_le32(h+1))
		goto bad;

	/* find session */
	mutex_lock(&mdsc->mutex);
	session = __ceph_lookup_mds_session(mdsc, mds);
	mutex_unlock(&mdsc->mutex);
	if (!session) {
		pr_err("ceph handle_lease got lease but no session mds%d\n",
		       mds);
		return;
	}

	mutex_lock(&session->s_mutex);
	session->s_seq++;

	/* lookup inode */
	inode = ceph_find_inode(sb, vino);
	dout("handle_lease '%s', mask %d, ino %llx %p\n",
	     ceph_lease_op_name(h->action), mask, vino.ino, inode);
	if (inode == NULL) {
		dout("handle_lease no inode %llx\n", vino.ino);
		goto release;
	}
	ci = ceph_inode(inode);

	/* dentry */
	parent = d_find_alias(inode);
	if (!parent) {
		dout("no parent dentry on inode %p\n", inode);
		WARN_ON(1);
		goto release;  /* hrm... */
	}
	dname.hash = full_name_hash(dname.name, dname.len);
	dentry = d_lookup(parent, &dname);
	dput(parent);
	if (!dentry)
		goto release;

	spin_lock(&dentry->d_lock);
	di = ceph_dentry(dentry);
	switch (h->action) {
	case CEPH_MDS_LEASE_REVOKE:
		if (di && di->lease_session == session) {
			h->seq = cpu_to_le32(di->lease_seq);
			__ceph_mdsc_drop_dentry_lease(dentry);
		}
		release = 1;
		break;

	case CEPH_MDS_LEASE_RENEW:
		if (di && di->lease_session == session &&
		    di->lease_gen == session->s_cap_gen &&
		    di->lease_renew_from &&
		    di->lease_renew_after == 0) {
			unsigned long duration =
				le32_to_cpu(h->duration_ms) * HZ / 1000;

			di->lease_seq = le32_to_cpu(h->seq);
			dentry->d_time = di->lease_renew_from + duration;
			di->lease_renew_after = di->lease_renew_from +
				(duration >> 1);
			di->lease_renew_from = 0;
		}
		break;
	}
	spin_unlock(&dentry->d_lock);
	dput(dentry);

	if (!release)
		goto out;

release:
	/* let's just reuse the same message */
	h->action = CEPH_MDS_LEASE_REVOKE_ACK;
	ceph_msg_get(msg);
	ceph_con_send(&session->s_con, msg);

out:
	iput(inode);
	mutex_unlock(&session->s_mutex);
	ceph_put_mds_session(session);
	return;

bad:
	pr_err("ceph corrupt lease message\n");
}

void ceph_mdsc_lease_send_msg(struct ceph_mds_session *session,
			      struct inode *inode,
			      struct dentry *dentry, char action,
			      u32 seq)
{
	struct ceph_msg *msg;
	struct ceph_mds_lease *lease;
	int len = sizeof(*lease) + sizeof(u32);
	int dnamelen = 0;

	dout("lease_send_msg inode %p dentry %p %s to mds%d\n",
	     inode, dentry, ceph_lease_op_name(action), session->s_mds);
	dnamelen = dentry->d_name.len;
	len += dnamelen;

	msg = ceph_msg_new(CEPH_MSG_CLIENT_LEASE, len, 0, 0, NULL);
	if (IS_ERR(msg))
		return;
	lease = msg->front.iov_base;
	lease->action = action;
	lease->mask = cpu_to_le16(CEPH_LOCK_DN);
	lease->ino = cpu_to_le64(ceph_vino(inode).ino);
	lease->first = lease->last = cpu_to_le64(ceph_vino(inode).snap);
	lease->seq = cpu_to_le32(seq);
	put_unaligned_le32(dnamelen, lease + 1);
	memcpy((void *)(lease + 1) + 4, dentry->d_name.name, dnamelen);

	/*
	 * if this is a preemptive lease RELEASE, no need to
	 * flush request stream, since the actual request will
	 * soon follow.
	 */
	msg->more_to_follow = (action == CEPH_MDS_LEASE_RELEASE);

	ceph_con_send(&session->s_con, msg);
}

/*
 * Preemptively release a lease we expect to invalidate anyway.
 * Pass @inode always, @dentry is optional.
 */
void ceph_mdsc_lease_release(struct ceph_mds_client *mdsc, struct inode *inode,
			     struct dentry *dentry, int mask)
{
	struct ceph_dentry_info *di;
	struct ceph_mds_session *session;
	u32 seq;

	BUG_ON(inode == NULL);
	BUG_ON(dentry == NULL);
	BUG_ON(mask != CEPH_LOCK_DN);

	/* is dentry lease valid? */
	spin_lock(&dentry->d_lock);
	di = ceph_dentry(dentry);
	if (!di || !di->lease_session ||
	    di->lease_session->s_mds < 0 ||
	    di->lease_gen != di->lease_session->s_cap_gen ||
	    !time_before(jiffies, dentry->d_time)) {
		dout("lease_release inode %p dentry %p -- "
		     "no lease on %d\n",
		     inode, dentry, mask);
		spin_unlock(&dentry->d_lock);
		return;
	}

	/* we do have a lease on this dentry; note mds and seq */
	session = ceph_get_mds_session(di->lease_session);
	seq = di->lease_seq;
	__ceph_mdsc_drop_dentry_lease(dentry);
	spin_unlock(&dentry->d_lock);

	dout("lease_release inode %p dentry %p mask %d to mds%d\n",
	     inode, dentry, mask, session->s_mds);
	ceph_mdsc_lease_send_msg(session, inode, dentry,
				 CEPH_MDS_LEASE_RELEASE, seq);
	ceph_put_mds_session(session);
}

/*
 * drop all leases (and dentry refs) in preparation for umount
 */
static void drop_leases(struct ceph_mds_client *mdsc)
{
	int i;

	dout("drop_leases\n");
	mutex_lock(&mdsc->mutex);
	for (i = 0; i < mdsc->max_sessions; i++) {
		struct ceph_mds_session *s = __ceph_lookup_mds_session(mdsc, i);
		if (!s)
			continue;
		mutex_unlock(&mdsc->mutex);
		mutex_lock(&s->s_mutex);
		mutex_unlock(&s->s_mutex);
		ceph_put_mds_session(s);
		mutex_lock(&mdsc->mutex);
	}
	mutex_unlock(&mdsc->mutex);
}



/*
 * delayed work -- periodically trim expired leases, renew caps with mds
 */
static void schedule_delayed(struct ceph_mds_client *mdsc)
{
	int delay = 5;
	unsigned hz = round_jiffies_relative(HZ * delay);
	schedule_delayed_work(&mdsc->delayed_work, hz);
}

static void delayed_work(struct work_struct *work)
{
	int i;
	struct ceph_mds_client *mdsc =
		container_of(work, struct ceph_mds_client, delayed_work.work);
	int renew_interval;
	int renew_caps;
	u32 want_map = 0;

	dout("mdsc delayed_work\n");
	ceph_check_delayed_caps(mdsc, 0);

	mutex_lock(&mdsc->mutex);
	renew_interval = mdsc->mdsmap->m_session_timeout >> 2;
	renew_caps = time_after_eq(jiffies, HZ*renew_interval +
				   mdsc->last_renew_caps);
	if (renew_caps)
		mdsc->last_renew_caps = jiffies;

	for (i = 0; i < mdsc->max_sessions; i++) {
		struct ceph_mds_session *s = __ceph_lookup_mds_session(mdsc, i);
		if (s == NULL)
			continue;
		if (s->s_state == CEPH_MDS_SESSION_CLOSING) {
			dout("resending session close request for mds%d\n",
			     s->s_mds);
			request_close_session(mdsc, s);
			ceph_put_mds_session(s);
			continue;
		}
		if (s->s_ttl && time_after(jiffies, s->s_ttl)) {
			if (s->s_state == CEPH_MDS_SESSION_OPEN) {
				s->s_state = CEPH_MDS_SESSION_HUNG;
				pr_info("ceph mds%d session probably timed out,"
					" requesting mds map\n", s->s_mds);
			}
			want_map = mdsc->mdsmap->m_epoch + 1;
		}
		if (s->s_state < CEPH_MDS_SESSION_OPEN) {
			/* this mds is failed or recovering, just wait */
			ceph_put_mds_session(s);
			continue;
		}
		mutex_unlock(&mdsc->mutex);

		mutex_lock(&s->s_mutex);
		if (renew_caps)
			send_renew_caps(mdsc, s);
		else
			ceph_con_keepalive(&s->s_con);
		add_cap_releases(mdsc, s, -1);
		send_cap_releases(mdsc, s);
		mutex_unlock(&s->s_mutex);
		ceph_put_mds_session(s);

		mutex_lock(&mdsc->mutex);
	}
	mutex_unlock(&mdsc->mutex);

	if (want_map)
		ceph_monc_request_mdsmap(&mdsc->client->monc, want_map);

	schedule_delayed(mdsc);
}


void ceph_mdsc_init(struct ceph_mds_client *mdsc, struct ceph_client *client)
{
	mdsc->client = client;
	mutex_init(&mdsc->mutex);
	mdsc->mdsmap = kzalloc(sizeof(*mdsc->mdsmap), GFP_NOFS);
	init_completion(&mdsc->safe_umount_waiters);
	init_completion(&mdsc->session_close_waiters);
	INIT_LIST_HEAD(&mdsc->waiting_for_map);
	mdsc->sessions = NULL;
	mdsc->max_sessions = 0;
	mdsc->stopping = 0;
	init_rwsem(&mdsc->snap_rwsem);
	INIT_RADIX_TREE(&mdsc->snap_realms, GFP_NOFS);
	INIT_LIST_HEAD(&mdsc->snap_empty);
	spin_lock_init(&mdsc->snap_empty_lock);
	mdsc->last_tid = 0;
	INIT_RADIX_TREE(&mdsc->request_tree, GFP_NOFS);
	INIT_DELAYED_WORK(&mdsc->delayed_work, delayed_work);
	mdsc->last_renew_caps = jiffies;
	INIT_LIST_HEAD(&mdsc->cap_delay_list);
	spin_lock_init(&mdsc->cap_delay_lock);
	INIT_LIST_HEAD(&mdsc->snap_flush_list);
	spin_lock_init(&mdsc->snap_flush_lock);
	mdsc->cap_flush_seq = 0;
	INIT_LIST_HEAD(&mdsc->cap_dirty);
	mdsc->num_cap_flushing = 0;
	spin_lock_init(&mdsc->cap_dirty_lock);
	init_waitqueue_head(&mdsc->cap_flushing_wq);
	spin_lock_init(&mdsc->dentry_lru_lock);
	INIT_LIST_HEAD(&mdsc->dentry_lru);
}

/*
 * Wait for safe replies on open mds requests.  If we time out, drop
 * all requests from the tree to avoid dangling dentry refs.
 */
static void wait_requests(struct ceph_mds_client *mdsc)
{
	struct ceph_mds_request *req;
	struct ceph_client *client = mdsc->client;

	mutex_lock(&mdsc->mutex);
	if (__get_oldest_tid(mdsc)) {
		mutex_unlock(&mdsc->mutex);
		dout("wait_requests waiting for requests\n");
		wait_for_completion_timeout(&mdsc->safe_umount_waiters,
				    client->mount_args.mount_timeout * HZ);
		mutex_lock(&mdsc->mutex);

		/* tear down remaining requests */
		while (radix_tree_gang_lookup(&mdsc->request_tree,
					      (void **)&req, 0, 1)) {
			dout("wait_requests timed out on tid %llu\n",
			     req->r_tid);
			radix_tree_delete(&mdsc->request_tree, req->r_tid);
			ceph_mdsc_put_request(req);
		}
	}
	mutex_unlock(&mdsc->mutex);
	dout("wait_requests done\n");
}

/*
 * called before mount is ro, and before dentries are torn down.
 * (hmm, does this still race with new lookups?)
 */
void ceph_mdsc_pre_umount(struct ceph_mds_client *mdsc)
{
	dout("pre_umount\n");
	mdsc->stopping = 1;

	drop_leases(mdsc);
	ceph_check_delayed_caps(mdsc, 1);
	wait_requests(mdsc);
}

/*
 * wait for all write mds requests to flush.
 */
static void wait_unsafe_requests(struct ceph_mds_client *mdsc, u64 want_tid)
{
	struct ceph_mds_request *req;
	u64 next_tid = 0;
	int got;

	mutex_lock(&mdsc->mutex);
	dout("wait_unsafe_requests want %lld\n", want_tid);
	while (1) {
		got = radix_tree_gang_lookup(&mdsc->request_tree, (void **)&req,
					     next_tid, 1);
		if (!got)
			break;
		if (req->r_tid > want_tid)
			break;

		next_tid = req->r_tid + 1;
		if ((req->r_op & CEPH_MDS_OP_WRITE) == 0)
			continue;  /* not a write op */

		ceph_mdsc_get_request(req);
		mutex_unlock(&mdsc->mutex);
		dout("wait_unsafe_requests  wait on %llu (want %llu)\n",
		     req->r_tid, want_tid);
		wait_for_completion(&req->r_safe_completion);
		mutex_lock(&mdsc->mutex);
		ceph_mdsc_put_request(req);
	}
	mutex_unlock(&mdsc->mutex);
	dout("wait_unsafe_requests done\n");
}

void ceph_mdsc_sync(struct ceph_mds_client *mdsc)
{
	u64 want_tid, want_flush;

	dout("sync\n");
	mutex_lock(&mdsc->mutex);
	want_tid = mdsc->last_tid;
	want_flush = mdsc->cap_flush_seq;
	mutex_unlock(&mdsc->mutex);
	dout("sync want tid %lld flush_seq %lld\n", want_tid, want_flush);

	ceph_check_delayed_caps(mdsc, 1);

	wait_unsafe_requests(mdsc, want_tid);
	wait_event(mdsc->cap_flushing_wq, check_cap_flush(mdsc, want_flush));
}


/*
 * called after sb is ro.
 */
void ceph_mdsc_close_sessions(struct ceph_mds_client *mdsc)
{
	struct ceph_mds_session *session;
	int i;
	int n;
	struct ceph_client *client = mdsc->client;
	unsigned long started, timeout = client->mount_args.mount_timeout * HZ;

	dout("close_sessions\n");

	mutex_lock(&mdsc->mutex);

	/* close sessions */
	started = jiffies;
	while (time_before(jiffies, started + timeout)) {
		dout("closing sessions\n");
		n = 0;
		for (i = 0; i < mdsc->max_sessions; i++) {
			session = __ceph_lookup_mds_session(mdsc, i);
			if (!session)
				continue;
			mutex_unlock(&mdsc->mutex);
			mutex_lock(&session->s_mutex);
			__close_session(mdsc, session);
			mutex_unlock(&session->s_mutex);
			ceph_put_mds_session(session);
			mutex_lock(&mdsc->mutex);
			n++;
		}
		if (n == 0)
			break;

		if (client->mount_state == CEPH_MOUNT_SHUTDOWN)
			break;

		dout("waiting for sessions to close\n");
		mutex_unlock(&mdsc->mutex);
		wait_for_completion_timeout(&mdsc->session_close_waiters,
					    timeout);
		mutex_lock(&mdsc->mutex);
	}

	/* tear down remaining sessions */
	for (i = 0; i < mdsc->max_sessions; i++) {
		if (mdsc->sessions[i]) {
			session = get_session(mdsc->sessions[i]);
			unregister_session(mdsc, i);
			mutex_unlock(&mdsc->mutex);
			mutex_lock(&session->s_mutex);
			remove_session_caps(session);
			mutex_unlock(&session->s_mutex);
			ceph_put_mds_session(session);
			mutex_lock(&mdsc->mutex);
		}
	}

	WARN_ON(!list_empty(&mdsc->cap_delay_list));

	mutex_unlock(&mdsc->mutex);

	ceph_cleanup_empty_realms(mdsc);

	cancel_delayed_work_sync(&mdsc->delayed_work); /* cancel timer */

	dout("stopped\n");
}

void ceph_mdsc_stop(struct ceph_mds_client *mdsc)
{
	dout("stop\n");
	cancel_delayed_work_sync(&mdsc->delayed_work); /* cancel timer */
	if (mdsc->mdsmap)
		ceph_mdsmap_destroy(mdsc->mdsmap);
	kfree(mdsc->sessions);
}


/*
 * handle mds map update.
 */
void ceph_mdsc_handle_map(struct ceph_mds_client *mdsc, struct ceph_msg *msg)
{
	u32 epoch;
	u32 maplen;
	void *p = msg->front.iov_base;
	void *end = p + msg->front.iov_len;
	struct ceph_mdsmap *newmap, *oldmap;
	ceph_fsid_t fsid;
	int err = -EINVAL;

	ceph_decode_need(&p, end, sizeof(fsid)+2*sizeof(u32), bad);
	ceph_decode_copy(&p, &fsid, sizeof(fsid));
	if (ceph_fsid_compare(&fsid, &mdsc->client->monc.monmap->fsid)) {
		pr_err("ceph got mdsmap with wrong fsid\n");
		return;
	}
	ceph_decode_32(&p, epoch);
	ceph_decode_32(&p, maplen);
	dout("handle_map epoch %u len %d\n", epoch, (int)maplen);

	/* do we need it? */
	ceph_monc_got_mdsmap(&mdsc->client->monc, epoch);
	mutex_lock(&mdsc->mutex);
	if (mdsc->mdsmap && epoch <= mdsc->mdsmap->m_epoch) {
		dout("handle_map epoch %u <= our %u\n",
		     epoch, mdsc->mdsmap->m_epoch);
		mutex_unlock(&mdsc->mutex);
		return;
	}

	newmap = ceph_mdsmap_decode(&p, end);
	if (IS_ERR(newmap)) {
		err = PTR_ERR(newmap);
		goto bad_unlock;
	}

	/* swap into place */
	if (mdsc->mdsmap) {
		oldmap = mdsc->mdsmap;
		mdsc->mdsmap = newmap;
		check_new_map(mdsc, newmap, oldmap);
		ceph_mdsmap_destroy(oldmap);
	} else {
		mdsc->mdsmap = newmap;  /* first mds map */
	}
	mdsc->client->sb->s_maxbytes = mdsc->mdsmap->m_max_file_size;

	__wake_requests(mdsc, &mdsc->waiting_for_map);

	mutex_unlock(&mdsc->mutex);
	schedule_delayed(mdsc);
	return;

bad_unlock:
	mutex_unlock(&mdsc->mutex);
bad:
	pr_err("ceph error decoding mdsmap %d\n", err);
	return;
}

static struct ceph_connection *con_get(struct ceph_connection *con)
{
	struct ceph_mds_session *s = con->private;

	if (ceph_get_mds_session(s))
		return con;
	return NULL;
}

static void con_put(struct ceph_connection *con)
{
	struct ceph_mds_session *s = con->private;

	ceph_put_mds_session(s);
}

/*
 * if the client is unresponsive for long enough, the mds will kill
 * the session entirely.
 */
static void peer_reset(struct ceph_connection *con)
{
	struct ceph_mds_session *s = con->private;

	pr_err("ceph mds%d gave us the boot.  IMPLEMENT RECONNECT.\n",
	       s->s_mds);
}

static void dispatch(struct ceph_connection *con, struct ceph_msg *msg)
{
	struct ceph_mds_session *s = con->private;
	struct ceph_mds_client *mdsc = s->s_mdsc;
	int type = le16_to_cpu(msg->hdr.type);

	switch (type) {
	case CEPH_MSG_MDS_MAP:
		ceph_mdsc_handle_map(mdsc, msg);
		break;
	case CEPH_MSG_CLIENT_SESSION:
		handle_session(mdsc, msg);
		break;
	case CEPH_MSG_CLIENT_REPLY:
		handle_reply(mdsc, msg);
		break;
	case CEPH_MSG_CLIENT_REQUEST_FORWARD:
		handle_forward(mdsc, msg);
		break;
	case CEPH_MSG_CLIENT_CAPS:
		ceph_handle_caps(mdsc, msg);
		break;
	case CEPH_MSG_CLIENT_SNAP:
		ceph_handle_snap(mdsc, msg);
		break;
	case CEPH_MSG_CLIENT_LEASE:
		handle_lease(mdsc, msg);
		break;

	default:
		pr_err("ceph received unknown message type %d %s\n", type,
		       ceph_msg_type_name(type));
	}
	ceph_msg_put(msg);
}

const static struct ceph_connection_operations mds_con_ops = {
	.get = con_get,
	.put = con_put,
	.dispatch = dispatch,
	.peer_reset = peer_reset,
	.alloc_msg = ceph_alloc_msg,
	.alloc_middle = ceph_alloc_middle,
};




/* eof */
