/*
 * Copyright (C) 2011 Taobao Inc.
 * Copyright (C) 2013 Zelin.io
 *
 * Liu Yuan <namei.unix@gmail.com>
 * Kai Zhang <kyle@zelin.io>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License version
 * 2 as published by the Free Software Foundation.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include <pthread.h>

#include "farm.h"
#include "list.h"

static char farm_object_dir[PATH_MAX];
static char farm_dir[PATH_MAX];

static pthread_rwlock_t vdi_list_lock = PTHREAD_RWLOCK_INITIALIZER;
struct vdi_entry {
	char name[SD_MAX_VDI_LEN];
	uint64_t vdi_size;
	uint32_t vdi_id;
	uint32_t snap_id;
	uint8_t  nr_copies;
	struct list_head list;
};
static LIST_HEAD(last_vdi_list);

struct snapshot_work {
	struct trunk_entry entry;
	struct strbuf *trunk_buf;
	struct work work;
};
struct work_queue *wq;
static uatomic_bool work_error;

static struct vdi_entry *find_vdi(const char *name)
{
	struct vdi_entry *vdi;

	list_for_each_entry(vdi, &last_vdi_list, list) {
		if (!strcmp(vdi->name, name))
			return vdi;
	}
	return NULL;
}

static struct vdi_entry *new_vdi(const char *name, uint64_t vdi_size,
				 uint32_t vdi_id, uint32_t snap_id,
				 uint8_t nr_copies)
{
	struct vdi_entry *vdi;
	vdi = xmalloc(sizeof(struct vdi_entry));
	pstrcpy(vdi->name, sizeof(vdi->name), name);
	vdi->vdi_size = vdi_size;
	vdi->vdi_id = vdi_id;
	vdi->snap_id = snap_id;
	vdi->nr_copies = nr_copies;
	INIT_LIST_HEAD(&vdi->list);
	return vdi;
}

static void insert_vdi(struct sd_inode *new)
{
	struct vdi_entry *vdi;
	vdi = find_vdi(new->name);
	if (!vdi) {
		vdi = new_vdi(new->name,
			      new->vdi_size,
			      new->vdi_id,
			      new->snap_id,
			      new->nr_copies);
		list_add(&vdi->list, &last_vdi_list);
	} else if (vdi->snap_id < new->snap_id) {
		vdi->vdi_size = new->vdi_size;
		vdi->vdi_id = new->vdi_id;
		vdi->snap_id = new->snap_id;
		vdi->nr_copies = new->nr_copies;
	}
}

static int create_active_vdis(void)
{
	struct vdi_entry *vdi;
	uint32_t new_vid;
	list_for_each_entry(vdi, &last_vdi_list, list) {
		if (do_vdi_create(vdi->name,
				  vdi->vdi_size,
				  vdi->vdi_id, &new_vid,
				  false, vdi->nr_copies) < 0)
			return -1;
	}
	return 0;
}

static void free_vdi_list(void)
{
	struct vdi_entry *vdi, *next;
	list_for_each_entry_safe(vdi, next, &last_vdi_list, list)
		free(vdi);
}

char *get_object_directory(void)
{
	return farm_object_dir;
}

static int create_directory(const char *p)
{
	int ret = -1;
	struct strbuf buf = STRBUF_INIT;

	strbuf_addstr(&buf, p);
	if (xmkdir(buf.buf, 0755) < 0) {
		if (errno == EEXIST)
			fprintf(stderr, "Path is not a directory: %s\n", p);
		goto out;
	}

	if (!strlen(farm_dir))
		strbuf_copyout(&buf, farm_dir, sizeof(farm_dir));

	strbuf_addstr(&buf, "/objects");
	if (xmkdir(buf.buf, 0755) < 0)
		goto out;

	for (int i = 0; i < 256; i++) {
		strbuf_addf(&buf, "/%02x", i);
		if (xmkdir(buf.buf, 0755) < 0)
			goto out;

		strbuf_remove(&buf, buf.len - 3, 3);
	}

	if (!strlen(farm_object_dir))
		strbuf_copyout(&buf, farm_object_dir, sizeof(farm_object_dir));

	ret = 0;
out:
	if (ret)
		fprintf(stderr, "Fail to create directory: %m\n");
	strbuf_release(&buf);
	return ret;
}

static int get_trunk_sha1(uint32_t idx, const char *tag, unsigned char *outsha1)
{
	int nr_logs = -1, ret = -1;
	struct snap_log *log_buf, *log_free = NULL;
	struct snap_file *snap_buf = NULL;

	log_free = log_buf = snap_log_read(&nr_logs);
	if (nr_logs < 0)
		goto out;

	for (int i = 0; i < nr_logs; i++, log_buf++) {
		if (log_buf->idx != idx && strcmp(log_buf->tag, tag))
			continue;
		snap_buf = snap_file_read(log_buf->sha1);
		if (!snap_buf)
			goto out;
		memcpy(outsha1, snap_buf->trunk_sha1, SHA1_DIGEST_SIZE);
		ret = 0;
		goto out;
	}
out:
	free(log_free);
	free(snap_buf);
	return ret;
}

static int notify_vdi_add(uint32_t vdi_id, uint32_t nr_copies)
{
	int ret = -1;
	struct sd_req hdr;
	char *buf = NULL;

	sd_init_req(&hdr, SD_OP_NOTIFY_VDI_ADD);
	hdr.vdi_state.new_vid = vdi_id;
	hdr.vdi_state.copies = nr_copies;
	hdr.vdi_state.set_bitmap = true;

	ret = collie_exec_req(sdhost, sdport, &hdr, buf);

	if (ret)
		fprintf(stderr, "Fail to notify vdi add event(%"PRIx32", %d)\n",
			vdi_id, nr_copies);

	free(buf);
	return ret;
}

int farm_init(const char *path)
{
	int ret = -1;

	if (create_directory(path) < 0)
		goto out;
	if (snap_init(farm_dir) < 0)
		goto out;
	return 0;
out:
	if (ret)
		fprintf(stderr, "Fail to init farm.\n");
	return ret;
}

bool farm_contain_snapshot(uint32_t idx, const char *tag)
{
	unsigned char trunk_sha1[SHA1_DIGEST_SIZE];
	return (get_trunk_sha1(idx, tag, trunk_sha1) == 0);
}

static void do_save_object(struct work *work)
{
	void *buf;
	size_t size;
	struct snapshot_work *sw;
	unsigned char object_sha1[SHA1_DIGEST_SIZE];

	if (uatomic_is_true(&work_error))
		return;

	sw = container_of(work, struct snapshot_work, work);

	/* read object sha1 and check if exists in local path */
	if (sd_read_object_sha1(sw->entry.oid, sd_epoch, sw->entry.nr_copies,
				object_sha1) == 0 &&
	    sha1_file_exist(object_sha1)) {
		memcpy(sw->entry.sha1, object_sha1, SHA1_DIGEST_SIZE);
		return;
	}

	size = get_objsize(sw->entry.oid);
	buf = xmalloc(size);

	if (sd_read_object(sw->entry.oid, buf, size, 0, true) < 0)
		goto error;

	if (sha1_file_write(buf, size, sw->entry.sha1) < 0)
		goto error;

	free(buf);
	return;
error:
	free(buf);
	fprintf(stderr, "Fail to save object, oid %"PRIu64"\n",
		sw->entry.oid);
	uatomic_set_true(&work_error);
}

static void save_object_done(struct work *work)
{
	struct snapshot_work *sw = container_of(work, struct snapshot_work,
						work);

	if (uatomic_is_true(&work_error))
		goto out;

	strbuf_add(sw->trunk_buf, &sw->entry, sizeof(struct trunk_entry));
out:
	free(sw);
}

static int queue_save_snapshot_work(uint64_t oid, int nr_copies, void *data)
{
	struct snapshot_work *sw = xzalloc(sizeof(struct snapshot_work));
	struct strbuf *trunk_buf = data;

	sw->entry.oid = oid;
	sw->entry.nr_copies = nr_copies;
	sw->trunk_buf = trunk_buf;
	sw->work.fn = do_save_object;
	sw->work.done = save_object_done;
	queue_work(wq, &sw->work);

	return 0;
}

int farm_save_snapshot(const char *tag)
{
	unsigned char snap_sha1[SHA1_DIGEST_SIZE];
	unsigned char trunk_sha1[SHA1_DIGEST_SIZE];
	struct strbuf trunk_buf;
	void *snap_log = NULL;
	int log_nr, idx, ret = -1;
	uint64_t nr_objects = object_tree_size();

	snap_log = snap_log_read(&log_nr);
	if (!snap_log)
		goto out;

	idx = log_nr + 1;

	strbuf_init(&trunk_buf, sizeof(struct trunk_entry) * nr_objects);

	wq = create_work_queue("save snapshot", WQ_ORDERED);
	if (for_each_object_in_tree(queue_save_snapshot_work,
				    &trunk_buf) < 0)
		goto out;

	work_queue_wait(wq);
	if (uatomic_is_true(&work_error))
		goto out;

	if (trunk_file_write(nr_objects, (struct trunk_entry *)trunk_buf.buf,
			     trunk_sha1) < 0)
		goto out;

	if (snap_file_write(idx, trunk_sha1, snap_sha1) < 0)
		goto out;

	if (snap_log_write(idx, tag, snap_sha1) < 0)
		goto out;

	ret = 0;
out:
	strbuf_release(&trunk_buf);
	free(snap_log);
	return ret;
}

static void do_load_object(struct work *work)
{
	void *buffer = NULL;
	size_t size;
	struct snapshot_work *sw;

	if (uatomic_is_true(&work_error))
		return;

	sw = container_of(work, struct snapshot_work, work);

	buffer = sha1_file_read(sw->entry.sha1, &size);

	if (!buffer)
		goto error;

	if (sd_write_object(sw->entry.oid, 0, buffer, size, 0, 0,
			    sw->entry.nr_copies, true, true) != 0)
		goto error;

	if (is_vdi_obj(sw->entry.oid)) {
		if (notify_vdi_add(oid_to_vid(sw->entry.oid),
				   sw->entry.nr_copies) < 0)
			goto error;

		pthread_rwlock_wrlock(&vdi_list_lock);
		insert_vdi(buffer);
		pthread_rwlock_unlock(&vdi_list_lock);
	}

	free(buffer);
	return;
error:
	free(buffer);
	fprintf(stderr, "Fail to load object, oid %"PRIu64"\n", sw->entry.oid);
	uatomic_set_true(&work_error);
}

static void load_object_done(struct work *work)
{
	struct snapshot_work *sw = container_of(work, struct snapshot_work,
						work);

	free(sw);
}

static int queue_load_snapshot_work(struct trunk_entry *entry, void *data)
{
	struct snapshot_work *sw = xzalloc(sizeof(struct snapshot_work));

	memcpy(&sw->entry, entry, sizeof(struct trunk_entry));
	sw->work.fn = do_load_object;
	sw->work.done = load_object_done;
	queue_work(wq, &sw->work);

	return 0;
}

int farm_load_snapshot(uint32_t idx, const char *tag)
{
	int ret = -1;
	unsigned char trunk_sha1[SHA1_DIGEST_SIZE];

	if (get_trunk_sha1(idx, tag, trunk_sha1) < 0)
		goto out;

	wq = create_work_queue("load snapshot", WQ_DYNAMIC);
	if (for_each_entry_in_trunk(trunk_sha1, queue_load_snapshot_work,
				    NULL) < 0)
		goto out;

	work_queue_wait(wq);
	if (uatomic_is_true(&work_error))
		goto out;

	if (create_active_vdis() < 0)
		goto out;

	ret = 0;
out:
	free_vdi_list();
	return ret;
}
