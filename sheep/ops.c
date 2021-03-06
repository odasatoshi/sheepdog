/*
 * Copyright (C) 2011 Nippon Telegraph and Telephone Corporation.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License version
 * 2 as published by the Free Software Foundation.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */
#include <stdio.h>
#include <stdlib.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <mntent.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <pthread.h>

#include "sheep_priv.h"
#include "strbuf.h"
#include "trace/trace.h"
#include "util.h"

enum sd_op_type {
	SD_OP_TYPE_CLUSTER = 1, /* cluster operations */
	SD_OP_TYPE_LOCAL,       /* local operations */
	SD_OP_TYPE_PEER,          /* io operations */
	SD_OP_TYPE_GATEWAY,	/* gateway operations */
};

struct sd_op_template {
	const char *name;
	enum sd_op_type type;

	/* process request even when cluster is not working */
	bool force;

	/*
	 * process_work() will be called in a worker thread, and process_main()
	 * will be called in the main thread.
	 *
	 * If type is SD_OP_TYPE_CLUSTER, it is guaranteed that only one node
	 * processes a cluster operation at the same time.  We can use this for
	 * for example to implement distributed locking.  process_work()
	 * will be called on the local node, and process_main() will be called
	 * on every node.
	 *
	 * If type is SD_OP_TYPE_LOCAL, both process_work() and process_main()
	 * will be called on the local node.
	 *
	 * If type is SD_OP_TYPE_PEER, only process_work() will be called, and it
	 * will be called on the local node.
	 */
	int (*process_work)(struct request *req);
	int (*process_main)(const struct sd_req *req, struct sd_rsp *rsp, void *data);
};

static int stat_sheep(uint64_t *store_size, uint64_t *store_free,
		      uint32_t epoch)
{
	uint64_t used;

	if (sys->gateway_only) {
		*store_size = 0;
		*store_free = 0;
	} else {
		*store_size = md_get_size(&used);
		*store_free = *store_size - used;
	}
	return SD_RES_SUCCESS;
}

static int cluster_new_vdi(struct request *req)
{
	const struct sd_req *hdr = &req->rq;
	struct sd_rsp *rsp = &req->rp;
	uint32_t vid;
	int ret;
	struct vdi_iocb iocb = {
		.name = req->data,
		.data_len = hdr->data_length,
		.size = hdr->vdi.vdi_size,
		.base_vid = hdr->vdi.base_vdi_id,
		.create_snapshot = !!hdr->vdi.snapid,
		.nr_copies = hdr->vdi.copies ? hdr->vdi.copies : sys->nr_copies,
	};

	if (hdr->data_length != SD_MAX_VDI_LEN)
		return SD_RES_INVALID_PARMS;

	ret = vdi_create(&iocb, &vid);

	rsp->vdi.vdi_id = vid;
	rsp->vdi.copies = iocb.nr_copies;

	return ret;
}

static int post_cluster_new_vdi(const struct sd_req *req, struct sd_rsp *rsp,
				void *data)
{
	unsigned long nr = rsp->vdi.vdi_id;
	int ret = rsp->result;

	sd_dprintf("done %d %lx", ret, nr);
	if (ret == SD_RES_SUCCESS)
		set_bit(nr, sys->vdi_inuse);

	return ret;
}

static int vdi_init_tag(const char **tag, const char *buf, uint32_t len)
{
	if (len == SD_MAX_VDI_LEN + SD_MAX_VDI_TAG_LEN)
		*tag = buf + SD_MAX_VDI_LEN;
	else if (len == SD_MAX_VDI_LEN)
		*tag = NULL;
	else
		return -1;

	return 0;
}

static int cluster_del_vdi(struct request *req)
{
	const struct sd_req *hdr = &req->rq;
	uint32_t data_len = hdr->data_length;
	struct vdi_iocb iocb = {
		.name = req->data,
		.data_len = data_len,
		.snapid = hdr->vdi.snapid,
	};

	if (vdi_init_tag(&iocb.tag, req->data, data_len) < 0)
		return SD_RES_INVALID_PARMS;

	return vdi_delete(&iocb, req);
}

struct cache_deletion_work {
	uint32_t vid;
	struct work work;
};

static void cache_delete_work(struct work *work)
{
	struct cache_deletion_work *dw =
		container_of(work, struct cache_deletion_work, work);

	object_cache_delete(dw->vid);
}

static void cache_delete_done(struct work *work)
{
	struct cache_deletion_work *dw =
		container_of(work, struct cache_deletion_work, work);

	free(dw);
}

static int post_cluster_del_vdi(const struct sd_req *req, struct sd_rsp *rsp,
				void *data)
{
	unsigned long vid = rsp->vdi.vdi_id;
	struct cache_deletion_work *dw;
	int ret = rsp->result;

	if (!sys->enable_object_cache)
		return ret;

	dw = xzalloc(sizeof(*dw));
	dw->vid = vid;
	dw->work.fn = cache_delete_work;
	dw->work.done = cache_delete_done;

	queue_work(sys->deletion_wqueue, &dw->work);

	return ret;
}

/*
 * Look up vid and copy number from vdi name
 *
 * This must be a cluster operation.  If QEMU reads the vdi object
 * while sheep snapshots the vdi, sheep can return SD_RES_NO_VDI.  To
 * avoid this problem, SD_OP_GET_INFO must be ordered with
 * SD_OP_NEW_VDI.
 */
static int cluster_get_vdi_info(struct request *req)
{
	const struct sd_req *hdr = &req->rq;
	struct sd_rsp *rsp = &req->rp;
	uint32_t data_len = hdr->data_length;
	int ret;
	struct vdi_info info = {};
	struct vdi_iocb iocb = {
		.name = req->data,
		.data_len = data_len,
		.snapid = hdr->vdi.snapid,
	};

	if (vdi_init_tag(&iocb.tag, req->data, data_len) < 0)
		return SD_RES_INVALID_PARMS;

	ret = vdi_lookup(&iocb, &info);
	if (ret != SD_RES_SUCCESS)
		return ret;

	rsp->vdi.vdi_id = info.vid;
	rsp->vdi.copies = get_vdi_copy_number(info.vid);

	return ret;
}

static int remove_epoch(uint32_t epoch)
{
	int ret;
	char path[PATH_MAX];

	sd_dprintf("remove epoch %"PRIu32, epoch);
	snprintf(path, sizeof(path), "%s%08u", epoch_path, epoch);
	ret = unlink(path);
	if (ret && ret != -ENOENT) {
		sd_eprintf("failed to remove %s: %s", path, strerror(-ret));
		return SD_RES_EIO;
	}

	return SD_RES_EIO;
}

static int cluster_make_fs(const struct sd_req *req, struct sd_rsp *rsp,
			   void *data)
{
	int i, ret;
	uint32_t latest_epoch;
	uint64_t created_time;
	struct store_driver *driver;
	char *store_name = data;

	driver = find_store_driver(data);
	if (!driver)
		return SD_RES_NO_STORE;

	sd_store = driver;
	latest_epoch = get_latest_epoch();

	ret = sd_store->format();
	if (ret != SD_RES_SUCCESS)
		return ret;
	if (set_cluster_store(store_name) < 0)
		return SD_RES_EIO;

	ret = sd_store->init();
	if (ret != SD_RES_SUCCESS)
		return ret;

	sys->nr_copies = req->cluster.copies;
	sys->flags = req->flags;
	if (!sys->nr_copies)
		sys->nr_copies = SD_DEFAULT_COPIES;

	created_time = req->cluster.ctime;
	set_cluster_ctime(created_time);
	set_cluster_copies(sys->nr_copies);
	set_cluster_flags(sys->flags);

	for (i = 1; i <= latest_epoch; i++)
		remove_epoch(i);

	memset(sys->vdi_inuse, 0, sizeof(sys->vdi_inuse));
	clean_vdi_state();

	sys->epoch = 1;

	ret = log_current_epoch();
	if (ret)
		return SD_RES_EIO;

	if (have_enough_zones())
		sys->status = SD_STATUS_OK;
	else
		sys->status = SD_STATUS_HALT;

	return SD_RES_SUCCESS;
}

static int cluster_shutdown(const struct sd_req *req, struct sd_rsp *rsp,
			    void *data)
{
	sys->status = SD_STATUS_SHUTDOWN;
	return SD_RES_SUCCESS;
}

static int cluster_enable_recover(const struct sd_req *req,
				    struct sd_rsp *rsp, void *data)
{
	sys->disable_recovery = false;
	resume_suspended_recovery();
	return SD_RES_SUCCESS;
}

static int cluster_disable_recover(const struct sd_req *req,
				   struct sd_rsp *rsp, void *data)
{
	sys->disable_recovery = true;
	return SD_RES_SUCCESS;
}

static int cluster_get_vdi_attr(struct request *req)
{
	const struct sd_req *hdr = &req->rq;
	struct sd_rsp *rsp = &req->rp;
	uint32_t vid, attrid = 0;
	struct sheepdog_vdi_attr *vattr;
	struct vdi_iocb iocb = {};
	struct vdi_info info = {};
	int ret;

	vattr = req->data;
	iocb.name = vattr->name;
	iocb.tag = vattr->tag;
	iocb.snapid = hdr->vdi.snapid;
	ret = vdi_lookup(&iocb, &info);
	if (ret != SD_RES_SUCCESS)
		return ret;
	/*
	 * the current VDI id can change if we take a snapshot,
	 * so we use the hash value of the VDI name as the VDI id
	 */
	vid = fnv_64a_buf(vattr->name, strlen(vattr->name), FNV1A_64_INIT);
	vid &= SD_NR_VDIS - 1;
	ret = get_vdi_attr(req->data, hdr->data_length,
			   vid, &attrid, info.create_time,
			   !!(hdr->flags & SD_FLAG_CMD_CREAT),
			   !!(hdr->flags & SD_FLAG_CMD_EXCL),
			   !!(hdr->flags & SD_FLAG_CMD_DEL));

	rsp->vdi.vdi_id = vid;
	rsp->vdi.attr_id = attrid;
	rsp->vdi.copies = get_vdi_copy_number(vid);

	return ret;
}

static int local_release_vdi(struct request *req)
{
	uint32_t vid = req->rq.vdi.base_vdi_id;

	if (!vid) {
		sd_iprintf("Some VDI failed to release the object cache. "
			   "Probably you are running old QEMU.");
		return SD_RES_SUCCESS;
	}

	object_cache_flush_vdi(vid);
	object_cache_delete(vid);

	return SD_RES_SUCCESS;
}

static int local_get_store_list(struct request *req)
{
	struct strbuf buf = STRBUF_INIT;
	struct store_driver *driver;

	list_for_each_entry(driver, &store_drivers, list) {
		strbuf_addf(&buf, "%s ", driver->name);
	}
	req->rp.data_length = strbuf_copyout(&buf, req->data, req->data_length);

	strbuf_release(&buf);
	return SD_RES_SUCCESS;
}

static int local_read_vdis(const struct sd_req *req, struct sd_rsp *rsp,
			   void *data)
{
	return read_vdis(data, req->data_length, &rsp->data_length);
}

static int local_get_vdi_copies(const struct sd_req *req, struct sd_rsp *rsp,
			   void *data)
{
	rsp->data_length = fill_vdi_state_list(data);

	return SD_RES_SUCCESS;
}

static int local_stat_sheep(struct request *req)
{
	struct sd_rsp *rsp = &req->rp;
	uint32_t epoch = req->rq.epoch;

	return stat_sheep(&rsp->node.store_size, &rsp->node.store_free, epoch);
}

static int local_stat_recovery(const struct sd_req *req, struct sd_rsp *rsp,
					void *data)
{
	if (node_in_recovery())
		return SD_RES_NODE_IN_RECOVERY;

	return SD_RES_SUCCESS;
}

static int local_stat_cluster(struct request *req)
{
	struct sd_rsp *rsp = &req->rp;
	struct epoch_log *log;
	int i, max_logs;
	uint32_t epoch;

	if (req->vinfo == NULL) {
		sd_dprintf("cluster is not started up");
		goto out;
	}

	max_logs = req->rq.data_length / sizeof(*log);
	epoch = get_latest_epoch();
	for (i = 0; i < max_logs; i++) {
		size_t nr_nodes;

		if (epoch <= 0)
			break;

		log = (struct epoch_log *)req->data + i;
		memset(log, 0, sizeof(*log));
		log->epoch = epoch;
		log->ctime = get_cluster_ctime();
		nr_nodes = epoch_log_read_with_timestamp(epoch, log->nodes,
							 sizeof(log->nodes),
							 (time_t *)&log->time);
		if (nr_nodes == -1)
			nr_nodes = epoch_log_read_remote(epoch, log->nodes,
							 sizeof(log->nodes),
							 (time_t *)&log->time,
							 req->vinfo);
		assert(nr_nodes >= 0);
		assert(nr_nodes <= SD_MAX_NODES);
		log->nr_nodes = nr_nodes;

		log->disable_recovery = sys->disable_recovery;

		rsp->data_length += sizeof(*log);
		epoch--;
	}
out:
	switch (sys->status) {
	case SD_STATUS_OK:
		return SD_RES_SUCCESS;
	case SD_STATUS_WAIT_FOR_FORMAT:
		return SD_RES_WAIT_FOR_FORMAT;
	case SD_STATUS_WAIT_FOR_JOIN:
		return SD_RES_WAIT_FOR_JOIN;
	case SD_STATUS_SHUTDOWN:
		return SD_RES_SHUTDOWN;
	case SD_STATUS_HALT:
		return SD_RES_HALT;
	default:
		return SD_RES_SYSTEM_ERROR;
	}
}

static int local_get_obj_list(struct request *req)
{
	return get_obj_list(&req->rq, &req->rp, req->data);
}

static int local_get_epoch(struct request *req)
{
	uint32_t epoch = req->rq.obj.tgt_epoch;
	int nr_nodes, nodes_len;
	time_t timestamp;

	sd_dprintf("%d", epoch);

	nr_nodes =
		epoch_log_read_with_timestamp(epoch, req->data,
					req->rq.data_length - sizeof(timestamp),
					&timestamp);
	if (nr_nodes == -1)
		return SD_RES_NO_TAG;

	nodes_len = nr_nodes * sizeof(struct sd_node);
	memcpy((void *)((char *)req->data + nodes_len), &timestamp,
		sizeof(timestamp));
	req->rp.data_length = nodes_len + sizeof(time_t);
	return SD_RES_SUCCESS;
}

static int cluster_force_recover_work(struct request *req)
{
	struct vnode_info *old_vnode_info;
	uint32_t epoch = sys_epoch();

	/*
	 * We should manually recover the cluster when
	 * 1) the master is physically down (different epoch condition).
	 * 2) some nodes are physically down (same epoch condition).
	 * In both case, the nodes(s) stat is WAIT_FOR_JOIN.
	 */
	if (sys->status != SD_STATUS_WAIT_FOR_JOIN || req->vinfo == NULL)
		return SD_RES_FORCE_RECOVER;

	old_vnode_info = get_vnode_info_epoch(epoch, req->vinfo);
	if (!old_vnode_info) {
		sd_printf(SDOG_EMERG, "cannot get vnode info for epoch %d",
			  epoch);
		put_vnode_info(old_vnode_info);
		return SD_RES_FORCE_RECOVER;
	}

	if (req->rq.data_length <
	    sizeof(*old_vnode_info->nodes) * old_vnode_info->nr_nodes) {
		sd_eprintf("too small buffer size, %d", req->rq.data_length);
		return SD_RES_INVALID_PARMS;
	}

	req->rp.epoch = epoch;
	req->rp.data_length = sizeof(*old_vnode_info->nodes) *
		old_vnode_info->nr_nodes;
	memcpy(req->data, old_vnode_info->nodes, req->rp.data_length);

	put_vnode_info(old_vnode_info);

	return SD_RES_SUCCESS;
}

static int cluster_force_recover_main(const struct sd_req *req,
				      struct sd_rsp *rsp,
				      void *data)
{
	struct vnode_info *old_vnode_info, *vnode_info;
	int ret = SD_RES_SUCCESS;
	uint8_t c;
	uint16_t f;
	struct sd_node *nodes = data;
	size_t nr_nodes = rsp->data_length / sizeof(*nodes);

	if (rsp->epoch != sys->epoch) {
		sd_eprintf("epoch was incremented while cluster_force_recover");
		return SD_RES_FORCE_RECOVER;
	}

	ret = get_cluster_copies(&c);
	if (ret) {
		sd_printf(SDOG_EMERG, "cannot get cluster copies");
		goto err;
	}
	ret = get_cluster_flags(&f);
	if (ret) {
		sd_printf(SDOG_EMERG, "cannot get cluster flags");
		goto err;
	}

	sys->nr_copies = c;
	sys->flags = f;

	sys->epoch++; /* some nodes are left, so we get a new epoch */
	ret = log_current_epoch();
	if (ret) {
		sd_printf(SDOG_EMERG, "cannot update epoch log");
		goto err;
	}

	if (have_enough_zones())
		sys->status = SD_STATUS_OK;
	else
		sys->status = SD_STATUS_HALT;

	vnode_info = get_vnode_info();
	old_vnode_info = alloc_vnode_info(nodes, nr_nodes);
	start_recovery(vnode_info, old_vnode_info, true);
	put_vnode_info(vnode_info);
	put_vnode_info(old_vnode_info);
	return ret;
err:
	panic("failed in force recovery");
}

static int cluster_cleanup(const struct sd_req *req, struct sd_rsp *rsp,
				void *data)
{
	int ret;

	if (node_in_recovery())
		return SD_RES_NODE_IN_RECOVERY;

	if (sys->gateway_only)
		return SD_RES_SUCCESS;

	if (sd_store->cleanup)
		ret = sd_store->cleanup();
	else
		ret = SD_RES_NO_SUPPORT;

	return ret;
}

static int cluster_notify_vdi_add(const struct sd_req *req, struct sd_rsp *rsp,
				  void *data)
{
	if (req->vdi_state.old_vid)
		/* make the previous working vdi a snapshot */
		add_vdi_state(req->vdi_state.old_vid,
			      get_vdi_copy_number(req->vdi_state.old_vid),
			      true);

	if (req->vdi_state.set_bitmap)
		set_bit(req->vdi_state.new_vid, sys->vdi_inuse);

	add_vdi_state(req->vdi_state.new_vid, req->vdi_state.copies, false);

	return SD_RES_SUCCESS;
}

static int cluster_notify_vdi_del(const struct sd_req *req, struct sd_rsp *rsp,
				  void *data)
{
	uint32_t vid = *(uint32_t *)data;

	return objlist_cache_cleanup(vid);
}

static int cluster_delete_cache(const struct sd_req *req, struct sd_rsp *rsp,
				void *data)
{
	uint32_t vid = oid_to_vid(req->obj.oid);

	if (sys->enable_object_cache)
		object_cache_delete(vid);

	return SD_RES_SUCCESS;
}

static int cluster_recovery_completion(const struct sd_req *req,
				       struct sd_rsp *rsp,
				       void *data)
{
	static struct sd_node recovereds[SD_MAX_NODES], *node;
	static size_t nr_recovereds;
	static int latest_epoch;
	struct vnode_info *vnode_info;
	int i;
	uint32_t epoch = req->obj.tgt_epoch;

	node = (struct sd_node *)data;

	if (latest_epoch > epoch)
		return SD_RES_SUCCESS;

	if (latest_epoch < epoch) {
		sd_dprintf("new epoch %d", epoch);
		latest_epoch = epoch;
		nr_recovereds = 0;
	}

	recovereds[nr_recovereds++] = *node;
	xqsort(recovereds, nr_recovereds, node_cmp);

	sd_dprintf("%s is recovered at epoch %d", node_to_str(node), epoch);
	for (i = 0; i < nr_recovereds; i++)
		sd_dprintf("[%x] %s", i, node_to_str(recovereds + i));

	if (sys->epoch != latest_epoch)
		return SD_RES_SUCCESS;

	vnode_info = get_vnode_info();

	if (vnode_info->nr_nodes == nr_recovereds) {
		for (i = 0; i < nr_recovereds; ++i) {
			if (!node_eq(vnode_info->nodes + i, recovereds + i))
				break;
		}
		if (i == nr_recovereds) {
			sd_dprintf("all nodes are recovered, epoch %d", epoch);
			/* sd_store can be NULL if this node is a gateway */
			if (sd_store && sd_store->cleanup)
				sd_store->cleanup();
		}
	}

	put_vnode_info(vnode_info);

	return SD_RES_SUCCESS;
}

static void do_reweight(struct work *work)
{
	struct sd_req hdr;
	int ret;

	sd_init_req(&hdr, SD_OP_UPDATE_SIZE);
	hdr.flags = SD_FLAG_CMD_WRITE;
	hdr.data_length = sizeof(sys->this_node);

	ret = exec_local_req(&hdr, &sys->this_node);
	if (ret != SD_RES_SUCCESS)
		sd_eprintf("failed to update node size");
}

static void reweight_done(struct work *work)
{
	free(work);
}

static void reweight_node(void)
{
	struct work *rw = xzalloc(sizeof(*rw));

	rw->fn = do_reweight;
	rw->done = reweight_done;

	queue_work(sys->recovery_wqueue, rw);
}

static bool node_size_varied(void)
{
	uint64_t new, used, old = sys->this_node.space;
	double diff;

	if (sys->gateway_only)
		return false;

	new = md_get_size(&used);
	/* If !old, it is forced-out-gateway. Not supported by current node */
	if (!old) {
		if (new)
			return true;
		else
			return false;
	}

	diff = new > old ? (double)(new - old) : (double)(old - new);
	sd_dprintf("new %"PRIu64 ", old %"PRIu64", ratio %f", new, old,
		   diff / (double)old);
	if (diff / (double)old < 0.01)
		return false;

	sys->this_node.space = new;
	set_node_space(new);

	return true;
}

static int cluster_reweight(const struct sd_req *req, struct sd_rsp *rsp,
			    void *data)
{
	if (node_size_varied())
		reweight_node();

	return SD_RES_SUCCESS;
}

static int cluster_update_size(const struct sd_req *req, struct sd_rsp *rsp,
			       void *data)
{
	struct sd_node *node = (struct sd_node *)data;

	update_node_size(node);
	kick_node_recover();

	return SD_RES_SUCCESS;
}

static int local_md_info(struct request *request)
{
	struct sd_rsp *rsp = &request->rp;

	assert(request->rq.data_length == sizeof(struct sd_md_info));
	rsp->data_length = md_get_info((struct sd_md_info *)request->data);

	return rsp->data_length ? SD_RES_SUCCESS : SD_RES_UNKNOWN;
}

static int local_md_plug(const struct sd_req *req, struct sd_rsp *rsp,
			 void *data)
{
	char *disks = (char *)data;

	return md_plug_disks(disks);
}

static int local_md_unplug(const struct sd_req *req, struct sd_rsp *rsp,
			   void *data)
{
	char *disks = (char *)data;

	return md_unplug_disks(disks);
}

static int local_get_hash(struct request *request)
{
	struct sd_req *req = &request->rq;
	struct sd_rsp *rsp = &request->rp;

	if (!sd_store->get_hash)
		return SD_RES_NO_SUPPORT;

	return sd_store->get_hash(req->obj.oid, req->obj.tgt_epoch,
				  rsp->hash.digest);
}

/* Return SD_RES_INVALID_PARMS to ask client not to send flush req again */
static int local_flush_vdi(struct request *req)
{
	int ret = SD_RES_INVALID_PARMS;

	if (sys->enable_object_cache) {
		uint32_t vid = oid_to_vid(req->rq.obj.oid);
		ret = object_cache_flush_vdi(vid);
		if (ret != SD_RES_SUCCESS)
			return ret;
	}

	return ret;
}

static int local_discard_obj(struct request *req)
{
	uint64_t oid = req->rq.obj.oid;
	uint32_t vid = oid_to_vid(oid), zero = 0;
	int ret, idx = data_oid_to_idx(oid);

	sd_dprintf("%"PRIx64, oid);
	ret = write_object(vid_to_vdi_oid(vid), (char *)&zero, sizeof(zero),
			   SD_INODE_HEADER_SIZE + sizeof(vid) * idx, false);
	if (ret != SD_RES_SUCCESS)
		return ret;
	if (remove_object(oid) != SD_RES_SUCCESS)
		sd_eprintf("failed to remove %"PRIx64, oid);
	/*
	 * Return success even if remove_object fails because we have updated
	 * inode successfully.
	 */
	return SD_RES_SUCCESS;
}

static int local_flush_and_del(struct request *req)
{
	if (!sys->enable_object_cache)
		return SD_RES_SUCCESS;
	return object_cache_flush_and_del(req);
}

static int local_trace_ops(const struct sd_req *req, struct sd_rsp *rsp, void *data)
{
	int enable = req->data_length, ret;

	if (enable)
		ret = trace_enable();
	else
		ret = trace_disable();

	return ret;
}

static int local_trace_read_buf(struct request *request)
{
	struct sd_req *req = &request->rq;
	struct sd_rsp *rsp = &request->rp;
	int ret;

	ret = trace_buffer_pop(request->data, req->data_length);
	if (ret == -1)
		return SD_RES_AGAIN;

	rsp->data_length = ret;
	sd_dprintf("%u", rsp->data_length);
	return SD_RES_SUCCESS;
}

static int local_kill_node(const struct sd_req *req, struct sd_rsp *rsp,
			   void *data)
{
	sys->status = SD_STATUS_KILLED;

	return SD_RES_SUCCESS;
}

static int read_copy_from_replica(struct request *req, uint32_t epoch,
				  uint64_t oid, char *buf)
{
	struct request read_req = { };
	struct sd_req *hdr = &read_req.rq;
	struct sd_rsp *rsp = &read_req.rp;
	int ret;

	/* Create a fake gateway read request */
	sd_init_req(hdr, SD_OP_READ_OBJ);
	hdr->data_length = SD_DATA_OBJ_SIZE;
	hdr->epoch = epoch;

	hdr->obj.oid = oid;
	hdr->obj.offset = 0;
	hdr->obj.copies = get_req_copy_number(req);

	read_req.data = buf;
	read_req.op = get_sd_op(hdr->opcode);
	read_req.vinfo = req->vinfo;

	ret = gateway_read_obj(&read_req);

	if (ret == SD_RES_SUCCESS)
		untrim_zero_sectors(buf, rsp->obj.offset, rsp->data_length,
				    SD_DATA_OBJ_SIZE);

	return ret;
}

int peer_remove_obj(struct request *req)
{
	uint64_t oid = req->rq.obj.oid;

	objlist_cache_remove(oid);

	return sd_store->remove_object(oid);
}

int peer_read_obj(struct request *req)
{
	struct sd_req *hdr = &req->rq;
	struct sd_rsp *rsp = &req->rp;
	int ret;
	uint32_t epoch = hdr->epoch;
	struct siocb iocb;

	if (sys->gateway_only)
		return SD_RES_NO_OBJ;

	memset(&iocb, 0, sizeof(iocb));
	iocb.epoch = epoch;
	iocb.buf = req->data;
	iocb.length = hdr->data_length;
	iocb.offset = hdr->obj.offset;
	ret = sd_store->read(hdr->obj.oid, &iocb);
	if (ret != SD_RES_SUCCESS)
		goto out;

	rsp->data_length = hdr->data_length;
	rsp->obj.offset = 0;
	trim_zero_sectors(req->data, &rsp->obj.offset, &rsp->data_length);

	if (hdr->obj.copies)
		rsp->obj.copies = hdr->obj.copies;
	else
		rsp->obj.copies = get_obj_copy_number(hdr->obj.oid,
						      req->vinfo->nr_zones);
out:
	return ret;
}

static int do_create_and_write_obj(struct siocb *iocb, struct sd_req *hdr,
				   uint32_t epoch, void *data)
{
	iocb->buf = data;
	iocb->length = hdr->data_length;
	iocb->offset = hdr->obj.offset;

	return sd_store->create_and_write(hdr->obj.oid, iocb);
}

int peer_write_obj(struct request *req)
{
	struct sd_req *hdr = &req->rq;
	struct siocb iocb = { };
	uint64_t oid = hdr->obj.oid;

	iocb.epoch = hdr->epoch;
	iocb.buf = req->data;
	iocb.length = hdr->data_length;
	iocb.offset = hdr->obj.offset;

	return sd_store->write(oid, &iocb);
}

int peer_create_and_write_obj(struct request *req)
{
	struct sd_req *hdr = &req->rq;
	struct sd_req cow_hdr;
	uint32_t epoch = hdr->epoch;
	uint64_t oid = hdr->obj.oid;
	char *buf = NULL;
	struct siocb iocb;
	int ret = SD_RES_SUCCESS;

	memset(&iocb, 0, sizeof(iocb));
	iocb.epoch = epoch;
	iocb.length = get_objsize(oid);
	if (hdr->flags & SD_FLAG_CMD_COW) {
		sd_dprintf("%" PRIx64 ", %" PRIx64, oid, hdr->obj.cow_oid);

		buf = xvalloc(SD_DATA_OBJ_SIZE);
		if (hdr->data_length != SD_DATA_OBJ_SIZE) {
			ret = read_copy_from_replica(req, hdr->epoch,
						     hdr->obj.cow_oid, buf);
			if (ret != SD_RES_SUCCESS) {
				sd_eprintf("failed to read cow object");
				goto out;
			}
		}

		memcpy(buf + hdr->obj.offset, req->data, hdr->data_length);
		memcpy(&cow_hdr, hdr, sizeof(cow_hdr));
		cow_hdr.data_length = SD_DATA_OBJ_SIZE;
		cow_hdr.obj.offset = 0;
		trim_zero_sectors(buf, &cow_hdr.obj.offset,
				  &cow_hdr.data_length);

		ret = do_create_and_write_obj(&iocb, &cow_hdr, epoch, buf);
	} else
		ret = do_create_and_write_obj(&iocb, hdr, epoch, req->data);

	if (SD_RES_SUCCESS == ret)
		objlist_cache_insert(oid);
out:
	if (buf)
		free(buf);
	return ret;
}

static struct sd_op_template sd_ops[] = {

	/* cluster operations */
	[SD_OP_NEW_VDI] = {
		.name = "NEW_VDI",
		.type = SD_OP_TYPE_CLUSTER,
		.process_work = cluster_new_vdi,
		.process_main = post_cluster_new_vdi,
	},

	[SD_OP_DEL_VDI] = {
		.name = "DEL_VDI",
		.type = SD_OP_TYPE_CLUSTER,
		.process_work = cluster_del_vdi,
		.process_main = post_cluster_del_vdi,
	},

	[SD_OP_MAKE_FS] = {
		.name = "MAKE_FS",
		.type = SD_OP_TYPE_CLUSTER,
		.force = true,
		.process_main = cluster_make_fs,
	},

	[SD_OP_SHUTDOWN] = {
		.name = "SHUTDOWN",
		.type = SD_OP_TYPE_CLUSTER,
		.force = true,
		.process_main = cluster_shutdown,
	},

	[SD_OP_GET_VDI_ATTR] = {
		.name = "GET_VDI_ATTR",
		.type = SD_OP_TYPE_CLUSTER,
		.process_work = cluster_get_vdi_attr,
	},

	[SD_OP_FORCE_RECOVER] = {
		.name = "FORCE_RECOVER",
		.type = SD_OP_TYPE_CLUSTER,
		.force = true,
		.process_work = cluster_force_recover_work,
		.process_main = cluster_force_recover_main,
	},

	[SD_OP_CLEANUP] = {
		.name = "CLEANUP",
		.type = SD_OP_TYPE_CLUSTER,
		.force = true,
		.process_main = cluster_cleanup,
	},

	[SD_OP_NOTIFY_VDI_DEL] = {
		.name = "NOTIFY_VDI_DEL",
		.type = SD_OP_TYPE_CLUSTER,
		.force = true,
		.process_main = cluster_notify_vdi_del,
	},

	[SD_OP_NOTIFY_VDI_ADD] = {
		.name = "NOTIFY_VDI_ADD",
		.type = SD_OP_TYPE_CLUSTER,
		.force = true,
		.process_main = cluster_notify_vdi_add,
	},

	[SD_OP_DELETE_CACHE] = {
		.name = "DELETE_CACHE",
		.type = SD_OP_TYPE_CLUSTER,
		.process_main = cluster_delete_cache,
	},

	[SD_OP_COMPLETE_RECOVERY] = {
		.name = "COMPLETE_RECOVERY",
		.type = SD_OP_TYPE_CLUSTER,
		.force = true,
		.process_main = cluster_recovery_completion,
	},

	[SD_OP_GET_VDI_INFO] = {
		.name = "GET_VDI_INFO",
		.type = SD_OP_TYPE_CLUSTER,
		.process_work = cluster_get_vdi_info,
	},

	[SD_OP_LOCK_VDI] = {
		.name = "LOCK_VDI",
		.type = SD_OP_TYPE_CLUSTER,
		.process_work = cluster_get_vdi_info,
	},

	[SD_OP_REWEIGHT] = {
		.name = "REWEIGHT",
		.type = SD_OP_TYPE_CLUSTER,
		.process_main = cluster_reweight,
	},

	[SD_OP_UPDATE_SIZE] = {
		.name = "UPDATE_SIZE",
		.type = SD_OP_TYPE_CLUSTER,
		.process_main = cluster_update_size,
	},

	[SD_OP_ENABLE_RECOVER] = {
		.name = "ENABLE_RECOVER",
		.type = SD_OP_TYPE_CLUSTER,
		.process_main = cluster_enable_recover,
	},

	[SD_OP_DISABLE_RECOVER] = {
		.name = "DISABLE_RECOVER",
		.type = SD_OP_TYPE_CLUSTER,
		.process_main = cluster_disable_recover,
	},

	/* local operations */
	[SD_OP_RELEASE_VDI] = {
		.name = "RELEASE_VDI",
		.type = SD_OP_TYPE_LOCAL,
		.process_work = local_release_vdi,
	},

	[SD_OP_GET_STORE_LIST] = {
		.name = "GET_STORE_LIST",
		.type = SD_OP_TYPE_LOCAL,
		.force = true,
		.process_work = local_get_store_list,
	},

	[SD_OP_READ_VDIS] = {
		.name = "READ_VDIS",
		.type = SD_OP_TYPE_LOCAL,
		.force = true,
		.process_main = local_read_vdis,
	},

	[SD_OP_GET_VDI_COPIES] = {
		.name = "GET_VDI_COPIES",
		.type = SD_OP_TYPE_LOCAL,
		.force = true,
		.process_main = local_get_vdi_copies,
	},

	[SD_OP_GET_NODE_LIST] = {
		.name = "GET_NODE_LIST",
		.type = SD_OP_TYPE_LOCAL,
		.force = true,
		.process_main = local_get_node_list,
	},

	[SD_OP_STAT_SHEEP] = {
		.name = "STAT_SHEEP",
		.type = SD_OP_TYPE_LOCAL,
		.process_work = local_stat_sheep,
	},

	[SD_OP_STAT_RECOVERY] = {
		.name = "STAT_RECOVERY",
		.type = SD_OP_TYPE_LOCAL,
		.process_main = local_stat_recovery,
	},

	[SD_OP_STAT_CLUSTER] = {
		.name = "STAT_CLUSTER",
		.type = SD_OP_TYPE_LOCAL,
		.force = true,
		.process_work = local_stat_cluster,
	},

	[SD_OP_GET_OBJ_LIST] = {
		.name = "GET_OBJ_LIST",
		.type = SD_OP_TYPE_LOCAL,
		.process_work = local_get_obj_list,
	},

	[SD_OP_GET_EPOCH] = {
		.name = "GET_EPOCH",
		.type = SD_OP_TYPE_LOCAL,
		.process_work = local_get_epoch,
	},

	[SD_OP_FLUSH_VDI] = {
		.name = "FLUSH_VDI",
		.type = SD_OP_TYPE_LOCAL,
		.process_work = local_flush_vdi,
	},

	[SD_OP_DISCARD_OBJ] = {
		.name = "DISCARD_OBJ",
		.type = SD_OP_TYPE_LOCAL,
		.process_work = local_discard_obj,
	},

	[SD_OP_FLUSH_DEL_CACHE] = {
		.name = "DEL_CACHE",
		.type = SD_OP_TYPE_LOCAL,
		.process_work = local_flush_and_del,
	},

	[SD_OP_TRACE] = {
		.name = "TRACE",
		.type = SD_OP_TYPE_LOCAL,
		.force = true,
		.process_main = local_trace_ops,
	},

	[SD_OP_TRACE_READ_BUF] = {
		.name = "TRACE_READ_BUF",
		.type = SD_OP_TYPE_LOCAL,
		.force = true,
		.process_work = local_trace_read_buf,
	},

	[SD_OP_KILL_NODE] = {
		.name = "KILL_NODE",
		.type = SD_OP_TYPE_LOCAL,
		.force = true,
		.process_main = local_kill_node,
	},

	[SD_OP_MD_INFO] = {
		.name = "MD_INFO",
		.type = SD_OP_TYPE_LOCAL,
		.process_work = local_md_info,
	},

	[SD_OP_MD_PLUG] = {
		.name = "MD_PLUG_DISKS",
		.type = SD_OP_TYPE_LOCAL,
		.process_main = local_md_plug,
	},

	[SD_OP_MD_UNPLUG] = {
		.name = "MD_UNPLUG_DISKS",
		.type = SD_OP_TYPE_LOCAL,
		.process_main = local_md_unplug,
	},

	[SD_OP_GET_HASH] = {
		.name = "GET_HASH",
		.type = SD_OP_TYPE_LOCAL,
		.process_work = local_get_hash,
	},

	/* gateway I/O operations */
	[SD_OP_CREATE_AND_WRITE_OBJ] = {
		.name = "CREATE_AND_WRITE_OBJ",
		.type = SD_OP_TYPE_GATEWAY,
		.process_work = gateway_create_and_write_obj,
	},

	[SD_OP_READ_OBJ] = {
		.name = "READ_OBJ",
		.type = SD_OP_TYPE_GATEWAY,
		.process_work = gateway_read_obj,
	},

	[SD_OP_WRITE_OBJ] = {
		.name = "WRITE_OBJ",
		.type = SD_OP_TYPE_GATEWAY,
		.process_work = gateway_write_obj,
	},

	[SD_OP_REMOVE_OBJ] = {
		.name = "REMOVE_OBJ",
		.type = SD_OP_TYPE_GATEWAY,
		.process_work = gateway_remove_obj,
	},

	/* peer I/O operations */
	[SD_OP_CREATE_AND_WRITE_PEER] = {
		.name = "CREATE_AND_WRITE_PEER",
		.type = SD_OP_TYPE_PEER,
		.process_work = peer_create_and_write_obj,
	},

	[SD_OP_READ_PEER] = {
		.name = "READ_PEER",
		.type = SD_OP_TYPE_PEER,
		.process_work = peer_read_obj,
	},

	[SD_OP_WRITE_PEER] = {
		.name = "WRITE_PEER",
		.type = SD_OP_TYPE_PEER,
		.process_work = peer_write_obj,
	},

	[SD_OP_REMOVE_PEER] = {
		.name = "REMOVE_PEER",
		.type = SD_OP_TYPE_PEER,
		.process_work = peer_remove_obj,
	},
};

const struct sd_op_template *get_sd_op(uint8_t opcode)
{
	if (sd_ops[opcode].type == 0)
		return NULL;

	return sd_ops + opcode;
}

const char *op_name(const struct sd_op_template *op)
{
	return op->name;
}

bool is_cluster_op(const struct sd_op_template *op)
{
	return op->type == SD_OP_TYPE_CLUSTER;
}

bool is_local_op(const struct sd_op_template *op)
{
	return op->type == SD_OP_TYPE_LOCAL;
}

bool is_peer_op(const struct sd_op_template *op)
{
	return op->type == SD_OP_TYPE_PEER;
}

bool is_gateway_op(const struct sd_op_template *op)
{
	return op->type == SD_OP_TYPE_GATEWAY;
}

bool is_force_op(const struct sd_op_template *op)
{
	return !!op->force;
}

bool has_process_work(const struct sd_op_template *op)
{
	return !!op->process_work;
}

bool has_process_main(const struct sd_op_template *op)
{
	return !!op->process_main;
}

void do_process_work(struct work *work)
{
	struct request *req = container_of(work, struct request, work);
	int ret = SD_RES_SUCCESS;

	sd_dprintf("%x, %" PRIx64", %"PRIu32, req->rq.opcode, req->rq.obj.oid,
		   req->rq.epoch);

	if (req->op->process_work)
		ret = req->op->process_work(req);

	if (ret != SD_RES_SUCCESS) {
		sd_dprintf("failed: %x, %" PRIx64" , %u, %s",
			   req->rq.opcode, req->rq.obj.oid, req->rq.epoch,
			   sd_strerror(ret));
	}

	req->rp.result = ret;
}

int do_process_main(const struct sd_op_template *op, const struct sd_req *req,
		    struct sd_rsp *rsp, void *data)
{
	return op->process_main(req, rsp, data);
}

int sheep_do_op_work(const struct sd_op_template *op, struct request *req)
{
	return op->process_work(req);
}

static int map_table[] = {
	[SD_OP_CREATE_AND_WRITE_OBJ] = SD_OP_CREATE_AND_WRITE_PEER,
	[SD_OP_READ_OBJ] = SD_OP_READ_PEER,
	[SD_OP_WRITE_OBJ] = SD_OP_WRITE_PEER,
	[SD_OP_REMOVE_OBJ] = SD_OP_REMOVE_PEER,
};

int gateway_to_peer_opcode(int opcode)
{
	assert(opcode < ARRAY_SIZE(map_table));
	return map_table[opcode];
}
