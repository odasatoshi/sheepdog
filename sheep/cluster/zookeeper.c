/*
 * Copyright (C) 2011 Nippon Telegraph and Telephone Corporation.
 *
 * Copyright (C) 2012 Taobao Inc.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License version
 * 2 as published by the Free Software Foundation.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <zookeeper/zookeeper.h>
#include <pthread.h>

#include "cluster.h"
#include "event.h"
#include "work.h"
#include "util.h"
#include "rbtree.h"

#define SESSION_TIMEOUT 30000		/* millisecond */

#define BASE_ZNODE "/sheepdog"
#define QUEUE_ZNODE BASE_ZNODE "/queue"
#define MEMBER_ZNODE BASE_ZNODE "/member"
#define MASTER_ZNONE BASE_ZNODE "/master"

/* iterate child znodes */
#define FOR_EACH_ZNODE(parent, path, strs)			       \
	for (zk_get_children(parent, strs),		               \
		     (strs)->data += (strs)->count;		       \
	     (strs)->count-- ?					       \
		     snprintf(path, sizeof(path), "%s/%s", parent,     \
			      *--(strs)->data) : (free((strs)->data), 0); \
	     free(*(strs)->data))

enum zk_event_type {
	EVENT_JOIN_REQUEST = 1,
	EVENT_JOIN_RESPONSE,
	EVENT_LEAVE,
	EVENT_BLOCK,
	EVENT_UNBLOCK,
	EVENT_NOTIFY,
};

struct zk_node {
	struct list_head list;
	struct rb_node rb;
	struct sd_node node;
	bool callbacked;
	bool gone;
};

struct zk_event {
	uint64_t id;
	enum zk_event_type type;
	struct zk_node sender;
	enum cluster_join_result join_result;
	size_t msg_len;
	size_t nr_nodes;
	size_t buf_len;
	uint8_t buf[SD_MAX_EVENT_BUF_SIZE];
};

static struct sd_node sd_nodes[SD_MAX_NODES];
static size_t nr_sd_nodes;
static struct rb_root zk_node_root = RB_ROOT;
static pthread_rwlock_t zk_tree_lock = PTHREAD_RWLOCK_INITIALIZER;
static LIST_HEAD(zk_block_list);

static struct zk_node *zk_tree_insert(struct zk_node *new)
{
	struct rb_node **p = &zk_node_root.rb_node;
	struct rb_node *parent = NULL;
	struct zk_node *entry;

	while (*p) {
		int cmp;

		parent = *p;
		entry = rb_entry(parent, struct zk_node, rb);

		cmp = node_cmp(&new->node, &entry->node);
		if (cmp < 0)
			p = &(*p)->rb_left;
		else if (cmp > 0)
			p = &(*p)->rb_right;
		else
			/* already has this entry */
			return entry;
	}
	rb_link_node(&new->rb, parent, p);
	rb_insert_color(&new->rb, &zk_node_root);
	return NULL; /* insert successfully */
}

static struct zk_node *zk_tree_search_nolock(const struct node_id *nid)
{
	struct rb_node *n = zk_node_root.rb_node;
	struct zk_node *t;

	while (n) {
		int cmp;

		t = rb_entry(n, struct zk_node, rb);
		cmp = node_id_cmp(nid, &t->node.nid);

		if (cmp < 0)
			n = n->rb_left;
		else if (cmp > 0)
			n = n->rb_right;
		else
			return t; /* found it */
	}
	return NULL;
}

static inline struct zk_node *zk_tree_search(const struct node_id *nid)
{
	struct zk_node *n;

	pthread_rwlock_rdlock(&zk_tree_lock);
	n = zk_tree_search_nolock(nid);
	pthread_rwlock_unlock(&zk_tree_lock);
	return n;
}

/* zookeeper API wrapper */
static zhandle_t *zhandle;
static struct zk_node this_node;

static inline ZOOAPI int zk_delete_node(const char *path, int version)
{
	int rc;
	do {
		rc = zoo_delete(zhandle, path, version);
	} while (rc == ZOPERATIONTIMEOUT || rc == ZCONNECTIONLOSS);
	if (rc != ZOK)
		sd_eprintf("failed, path:%s, %s", path, zerror(rc));
	return rc;
}

static inline ZOOAPI void
zk_init_node(const char *path)
{
	int rc;
	do {
		rc = zoo_create(zhandle, path, "", 0, &ZOO_OPEN_ACL_UNSAFE, 0,
				NULL, 0);
	} while (rc == ZOPERATIONTIMEOUT || rc == ZCONNECTIONLOSS);

	if (rc != ZOK && rc != ZNODEEXISTS)
		panic("failed, path:%s, %s", path, zerror(rc));
}

static inline ZOOAPI int
zk_create_node(const char *path, const char *value, int valuelen,
	       const struct ACL_vector *acl, int flags, char *path_buffer,
	       int path_buffer_len)
{
	int rc;
	do {
		rc = zoo_create(zhandle, path, value, valuelen, acl,
				flags, path_buffer, path_buffer_len);
		if (rc != ZOK && rc != ZNODEEXISTS)
			sd_eprintf("failed, path:%s, %s", path, zerror(rc));
	} while (rc == ZOPERATIONTIMEOUT || rc == ZCONNECTIONLOSS);
	return rc;
}

/*
 * Create a znode after adding a unique monotonically increasing sequence number
 * to the path name.
 *
 * Note that the caller has to retry this function when this returns
 * ZOPERATIONTIMEOUT or ZCONNECTIONLOSS and the znode is not created.
 */
static inline ZOOAPI int
zk_create_seq_node(const char *path, const char *value, int valuelen,
		   char *path_buffer, int path_buffer_len)
{
	int rc;
	rc = zoo_create(zhandle, path, value, valuelen, &ZOO_OPEN_ACL_UNSAFE,
			ZOO_SEQUENCE, path_buffer, path_buffer_len);
	if (rc != ZOK)
		sd_iprintf("failed, path:%s, %s", path, zerror(rc));

	return rc;
}

static inline ZOOAPI int zk_get_data(const char *path, void *buffer,
				     int *buffer_len)
{
	int rc;
	do {
		rc = zoo_get(zhandle, path, 1, (char *)buffer,
			     buffer_len, NULL);
		if (rc != ZOK)
			sd_eprintf("failed, path:%s, %s", path, zerror(rc));
	} while (rc == ZOPERATIONTIMEOUT || rc == ZCONNECTIONLOSS);
	return rc;
}

static inline ZOOAPI int
zk_set_data(const char *path, const char *buffer, int buflen, int version)
{
	int rc;
	do {
		rc = zoo_set(zhandle, path, buffer, buflen, version);
	} while (rc == ZOPERATIONTIMEOUT || rc == ZCONNECTIONLOSS);
	if (rc != ZOK)
		panic("failed, path:%s, %s", path, zerror(rc));
	return rc;
}

static inline ZOOAPI int zk_node_exists(const char *path)
{
	int rc;
	do {
		rc = zoo_exists(zhandle, path, 1, NULL);
		if (rc != ZOK && rc != ZNONODE)
			sd_eprintf("failed, path:%s, %s", path, zerror(rc));
	} while (rc == ZOPERATIONTIMEOUT || rc == ZCONNECTIONLOSS);

	return rc;
}

static inline ZOOAPI void zk_get_children(const char *path,
					  struct String_vector *strings)
{
	int rc;
	do {
		rc = zoo_get_children(zhandle, path, 1, strings);
	} while (rc == ZOPERATIONTIMEOUT || rc == ZCONNECTIONLOSS);
	if (rc != ZOK)
		panic("failed, path:%s, %s", path, zerror(rc));
}

/* ZooKeeper-based queue give us an totally ordered events */
static int efd;
static int32_t queue_pos;

static bool zk_queue_peek(void)
{
	int rc;
	char path[MAX_NODE_STR_LEN];

	snprintf(path, sizeof(path), QUEUE_ZNODE "/%010"PRId32, queue_pos);

	rc = zk_node_exists(path);
	switch (rc) {
	case ZOK:
		return true;
	case ZNONODE:
		return false;
	default:
		panic("failed to check %s, %s", path, zerror(rc));
	}
}

/* return true if there is a node with 'id' in the queue. */
static bool zk_find_seq_node(uint64_t id, char *seq_path, int seq_path_len)
{
	int rc, len;

	for (int seq = queue_pos; ; seq++) {
		struct zk_event ev;

		snprintf(seq_path, seq_path_len, QUEUE_ZNODE"/%010"PRId32, seq);
		len = offsetof(typeof(ev), id) + sizeof(ev.id);
		rc = zk_get_data(seq_path, &ev, &len);
		switch (rc) {
		case ZOK:
			if (ev.id == id) {
				sd_dprintf("id %"PRIx64" is found in %s", id,
					   seq_path);
				return true;
			}
			break;
		case ZNONODE:
			sd_dprintf("id %"PRIx64" is not found", id);
			return false;
		default:
			panic("failed, %s", zerror(rc));
			break;
		}
	}
}

static void zk_queue_push(struct zk_event *ev)
{
	static bool first_push = true;
	int rc, len;
	char path[MAX_NODE_STR_LEN], buf[MAX_NODE_STR_LEN];

	len = offsetof(typeof(*ev), buf) + ev->buf_len;
	snprintf(path, sizeof(path), "%s/", QUEUE_ZNODE);
again:
	rc = zk_create_seq_node(path, (char *)ev, len, buf, sizeof(buf));
	switch (rc) {
	case ZOK:
		/* Success */
		break;
	case ZOPERATIONTIMEOUT:
	case ZCONNECTIONLOSS:
		if (!zk_find_seq_node(ev->id, buf, sizeof(buf)))
			/* retry if seq_node was not created */
			goto again;
		break;
	default:
		panic("failed, path:%s, %s", path, zerror(rc));
	}
	if (first_push) {
		int32_t seq;

		sscanf(buf, QUEUE_ZNODE "/%"PRId32, &seq);
		queue_pos = seq;
		eventfd_write(efd, 1);
		first_push = false;
	}

	sd_dprintf("create path:%s, queue_pos:%010"PRId32", len:%d",
		   buf, queue_pos, len);
}

static inline void *zk_event_sd_nodes(struct zk_event *ev)
{
	return (char *)ev->buf + ev->msg_len;
}

/* Change the join event in place and piggyback the nodes information. */
static void push_join_response(struct zk_event *ev)
{
	char path[MAX_NODE_STR_LEN];
	int len;

	ev->type = EVENT_JOIN_RESPONSE;
	ev->nr_nodes = nr_sd_nodes;
	memcpy(zk_event_sd_nodes(ev), sd_nodes,
	       nr_sd_nodes * sizeof(struct sd_node));
	queue_pos--;

	len = offsetof(typeof(*ev), buf) + ev->buf_len;
	snprintf(path, sizeof(path), QUEUE_ZNODE "/%010"PRId32, queue_pos);
	zk_set_data(path, (char *)ev, len, -1);
	sd_dprintf("update path:%s, queue_pos:%010"PRId32", len:%d",
		   path, queue_pos, len);
}

static void zk_queue_pop_advance(struct zk_event *ev)
{
	int rc, len;
	char path[MAX_NODE_STR_LEN];

	len = sizeof(*ev);
	snprintf(path, sizeof(path), QUEUE_ZNODE "/%010"PRId32, queue_pos);
	rc = zk_get_data(path, ev, &len);
	if (rc != ZOK)
		panic("failed to get data from %s, %s", path, zerror(rc));
	sd_dprintf("%s, type:%d, len:%d, pos:%"PRId32, path, ev->type, len,
		   queue_pos);
	queue_pos++;
}

static int zk_member_empty(void)
{
	struct String_vector strs;

	zk_get_children(MEMBER_ZNODE, &strs);
	return (strs.count == 0);
}

static inline void zk_tree_add(struct zk_node *node)
{
	struct zk_node *zk = xzalloc(sizeof(*zk));
	*zk = *node;
	pthread_rwlock_wrlock(&zk_tree_lock);
	if (zk_tree_insert(zk)) {
		free(zk);
		goto out;
	}
	/*
	 * Even node list will be built later, we need this because in master
	 * transfer case, we need this information to destroy the tree.
	 */
	sd_nodes[nr_sd_nodes++] = zk->node;
out:
	pthread_rwlock_unlock(&zk_tree_lock);
}

static inline void zk_tree_del_nolock(struct zk_node *node)
{
	rb_erase(&node->rb, &zk_node_root);
	free(node);
}

static inline void zk_tree_del(struct zk_node *node)
{
	pthread_rwlock_wrlock(&zk_tree_lock);
	zk_tree_del_nolock(node);
	pthread_rwlock_unlock(&zk_tree_lock);
}

static inline void zk_tree_destroy(void)
{
	struct zk_node *zk;
	int i;

	pthread_rwlock_wrlock(&zk_tree_lock);
	for (i = 0; i < nr_sd_nodes; i++) {
		zk = zk_tree_search_nolock(&sd_nodes[i].nid);
		if (zk)
			zk_tree_del_nolock(zk);
	}
	pthread_rwlock_unlock(&zk_tree_lock);
}

static inline void build_node_list(void)
{
	struct rb_node *n;
	struct zk_node *zk;

	nr_sd_nodes = 0;
	for (n = rb_first(&zk_node_root); n; n = rb_next(n)) {
		zk = rb_entry(n, struct zk_node, rb);
		sd_nodes[nr_sd_nodes++] = zk->node;
	}
	sd_dprintf("nr_sd_nodes:%zu", nr_sd_nodes);
}

static inline int zk_master_create(void)
{
	return zk_create_node(MASTER_ZNONE, "", 0, &ZOO_OPEN_ACL_UNSAFE,
			      ZOO_EPHEMERAL, NULL, 0);
}

static bool is_master(void)
{
	struct rb_node *n;
	struct zk_node *zk = NULL;

	if (!nr_sd_nodes) {
		if (zk_member_empty())
			return true;
		else
			return false;
	}

	for (n = rb_first(&zk_node_root); n; n = rb_next(n)) {
		zk = rb_entry(n, struct zk_node, rb);
		if (!zk->gone)
			break;
	}
	if (zk && node_eq(&zk->node, &this_node.node))
		return true;

	return false;
}

static void zk_queue_init(void)
{
	zk_init_node(BASE_ZNODE);
	zk_init_node(QUEUE_ZNODE);
	zk_init_node(MEMBER_ZNODE);
}

/* Calculate a unique 64 bit integer from this_node and the sequence number. */
static uint64_t get_uniq_id(void)
{
	static int seq;
	uint64_t id, n = uatomic_add_return(&seq, 1);

	id = fnv_64a_buf(&this_node, sizeof(this_node), FNV1A_64_INIT);
	id = fnv_64a_buf(&n, sizeof(n), id);

	return id;
}

static int add_event(enum zk_event_type type, struct zk_node *znode, void *buf,
		     size_t buf_len)
{
	struct zk_event ev;

	ev.id = get_uniq_id();
	ev.type = type;
	ev.sender = *znode;
	ev.buf_len = buf_len;
	if (buf)
		memcpy(ev.buf, buf, buf_len);
	zk_queue_push(&ev);
	return 0;
}

static void zk_watcher(zhandle_t *zh, int type, int state, const char *path,
		       void *ctx)
{
	struct zk_node znode;
	char str[MAX_NODE_STR_LEN], *p;
	int ret;

/* CREATED_EVENT 1, DELETED_EVENT 2, CHANGED_EVENT 3, CHILD_EVENT 4 */
	sd_dprintf("path:%s, type:%d", path, type);
	if (type == ZOO_CREATED_EVENT || type == ZOO_CHANGED_EVENT) {
		ret = sscanf(path, MEMBER_ZNODE "/%s", str);
		if (ret == 1) {
			int rc = zk_node_exists(path);
			if (rc != ZOK)
				panic("failed to check %s, %s", path,
				      zerror(rc));
		}
		/* kick off the event handler */
		eventfd_write(efd, 1);
	} else if (type == ZOO_DELETED_EVENT) {
		struct zk_node *n;

		ret = sscanf(path, MEMBER_ZNODE "/%s", str);
		if (ret != 1)
			return;
		p = strrchr(path, '/');
		p++;
		str_to_node(p, &znode.node);
		/* FIXME: remove redundant leave events */
		pthread_rwlock_rdlock(&zk_tree_lock);
		n = zk_tree_search_nolock(&znode.node.nid);
		if (n)
			n->gone = true;
		pthread_rwlock_unlock(&zk_tree_lock);
		if (n)
			add_event(EVENT_LEAVE, &znode, NULL, 0);
	}

}

/*
 * We plcaehode the enough space to piggyback the nodes information on join
 * response message so that every node can see the same membership view.
 */
static int add_join_event(void *msg, size_t msg_len)
{
	struct zk_event ev;
	size_t len = msg_len + sizeof(struct sd_node) * SD_MAX_NODES;

	assert(len <= SD_MAX_EVENT_BUF_SIZE);
	ev.id = get_uniq_id();
	ev.type = EVENT_JOIN_REQUEST;
	ev.sender = this_node;
	ev.msg_len = msg_len;
	ev.buf_len = len;
	if (msg)
		memcpy(ev.buf, msg, msg_len);
	zk_queue_push(&ev);
	return 0;
}

static int zk_join(const struct sd_node *myself,
		   void *opaque, size_t opaque_len)
{
	int rc;
	char path[MAX_NODE_STR_LEN];

	this_node.node = *myself;

	snprintf(path, sizeof(path), MEMBER_ZNODE "/%s", node_to_str(myself));
	rc = zk_node_exists(path);
	switch (rc) {
	case ZOK:
		sd_eprintf("Previous zookeeper session exist, shoot myself.");
		exit(1);
	case ZNONODE:
		/* success */
		break;
	default:
		panic("failed to check %s, %s", path, zerror(rc));
		break;
	}

	/* For concurrent nodes setup, we allow only one to continue */
	while (zk_member_empty()) {
		rc = zk_master_create();
		switch (rc) {
		case ZOK:
			/* I'm a master */
			goto out;
		case ZNODEEXISTS:
			/* wait */
			break;
		default:
			panic("failed to create master %s", zerror(rc));
		}
	}
out:
	return add_join_event(opaque, opaque_len);
}

static int zk_leave(void)
{
	int rc;
	char path[PATH_MAX];
	snprintf(path, sizeof(path), MEMBER_ZNODE"/%s",
			node_to_str(&this_node.node));
	add_event(EVENT_LEAVE, &this_node, NULL, 0);
	rc = zk_delete_node(path, -1);
	switch (rc) {
	case ZOK:
	case ZNONODE:
		/* success */
		break;
	default:
		panic("failed to delete %s, %s", path, zerror(rc));
	}
	return 0;
}

static int zk_notify(void *msg, size_t msg_len)
{
	return add_event(EVENT_NOTIFY, &this_node, msg, msg_len);
}

static void zk_block(void)
{
	add_event(EVENT_BLOCK, &this_node, NULL, 0);
}

static void zk_unblock(void *msg, size_t msg_len)
{
	add_event(EVENT_UNBLOCK, &this_node, msg, msg_len);
}

static void zk_handle_join_request(struct zk_event *ev)
{
	enum cluster_join_result res;

	sd_dprintf("sender: %s", node_to_str(&ev->sender.node));
	if (!is_master()) {
		/* Let's await master acking the join-request */
		queue_pos--;
		return;
	}

	res = sd_check_join_cb(&ev->sender.node, ev->buf);
	ev->join_result = res;
	push_join_response(ev);
	if (res == CJ_RES_MASTER_TRANSFER) {
		sd_eprintf("failed to join sheepdog cluster: "
			   "please retry when master is up");
		add_event(EVENT_LEAVE, &this_node, NULL, 0);
		exit(1);
	}
	sd_dprintf("I'm the master now");
}

static void watch_all_nodes(void)
{
	struct String_vector strs;
	struct zk_node znode;
	char path[MAX_NODE_STR_LEN];
	int len = sizeof(znode);

	if (zk_member_empty())
		return;

	FOR_EACH_ZNODE(MEMBER_ZNODE, path, &strs) {
		int rc = zk_get_data(path, &znode, &len);
		if (rc != ZOK)
			panic("failed to get data from %s", path);
	}
}

static void init_node_list(struct zk_event *ev)
{
	uint8_t *p = zk_event_sd_nodes(ev);
	size_t node_nr = ev->nr_nodes;
	int i;

	sd_dprintf("%zu", node_nr);
	for (i = 0; i < node_nr; i++) {
		struct zk_node zk;
		mempcpy(&zk.node, p, sizeof(struct sd_node));
		zk_tree_add(&zk);
		p += sizeof(struct sd_node);
	}

	watch_all_nodes();
}

static void zk_handle_join_response(struct zk_event *ev)
{
	int rc;
	char path[MAX_NODE_STR_LEN];

	sd_dprintf("JOIN RESPONSE");
	if (node_eq(&ev->sender.node, &this_node.node))
		/* newly joined node */
		init_node_list(ev);

	if (ev->join_result == CJ_RES_MASTER_TRANSFER)
		/*
		 * Sheepdog assumes that only one sheep is alive in
		 * MASTER_TRANSFER scenario. So only the joining sheep is
		 * supposed to return single node view to sd_join_handler().
		 */
		zk_tree_destroy();

	sd_dprintf("%s, %d", node_to_str(&ev->sender.node), ev->join_result);
	switch (ev->join_result) {
	case CJ_RES_SUCCESS:
	case CJ_RES_JOIN_LATER:
	case CJ_RES_MASTER_TRANSFER:
		snprintf(path, sizeof(path), MEMBER_ZNODE"/%s",
			 node_to_str(&ev->sender.node));
		if (node_eq(&ev->sender.node, &this_node.node)) {
			sd_dprintf("create path:%s", path);
			rc = zk_create_node(path, (char *)&ev->sender,
					    sizeof(ev->sender),
					    &ZOO_OPEN_ACL_UNSAFE,
					    ZOO_EPHEMERAL, NULL, 0);
			switch (rc) {
			case ZOK:
				/* success */
				break;
			case ZNODEEXISTS:
				sd_eprintf("%s already exists", path);
				break;
			default:
				panic("failed to create %s, %s", path,
				      zerror(rc));
				break;
			}
		} else {
			rc = zk_node_exists(path);
			switch (rc) {
			case ZOK:
			case ZNONODE:
				/* success */
				break;
			default:
				panic("failed to check %s, %s", path,
				      zerror(rc));
			}
		}

		zk_tree_add(&ev->sender);
		break;
	default:
		break;
	}

	build_node_list();
	sd_join_handler(&ev->sender.node, sd_nodes, nr_sd_nodes,
			ev->join_result, ev->buf);
}

static void kick_block_event(void)
{
	struct zk_node *block;

	if (list_empty(&zk_block_list))
		return;
	block = list_first_entry(&zk_block_list, typeof(*block), list);
	if (!block->callbacked)
		block->callbacked = sd_block_handler(&block->node);
}

static void block_event_list_del(struct zk_node *n)
{
	struct zk_node *ev, *t;

	list_for_each_entry_safe(ev, t, &zk_block_list, list) {
		if (node_eq(&ev->node, &n->node)) {
			list_del(&ev->list);
			free(ev);
		}
	}
}

static void zk_handle_leave(struct zk_event *ev)
{
	struct zk_node *n = zk_tree_search(&ev->sender.node.nid);

	if (!n) {
		sd_dprintf("can't find this leave node:%s, ignore it.",
			   node_to_str(&ev->sender.node));
		return;
	}
	block_event_list_del(n);
	zk_tree_del(n);
	build_node_list();
	sd_leave_handler(&ev->sender.node, sd_nodes, nr_sd_nodes);
}

static void zk_handle_block(struct zk_event *ev)
{
	struct zk_node *block = xzalloc(sizeof(*block));

	sd_dprintf("BLOCK");
	block->node = ev->sender.node;
	list_add_tail(&block->list, &zk_block_list);
	block = list_first_entry(&zk_block_list, typeof(*block), list);
	if (!block->callbacked)
		block->callbacked = sd_block_handler(&block->node);
}

static void zk_handle_unblock(struct zk_event *ev)
{
	struct zk_node *block;

	sd_dprintf("UNBLOCK");
	if (list_empty(&zk_block_list))
		return;
	block = list_first_entry(&zk_block_list, typeof(*block), list);
	if (block->callbacked)
		add_event(EVENT_NOTIFY, block, ev->buf, ev->buf_len);

	list_del(&block->list);
	free(block);
}

static void zk_handle_notify(struct zk_event *ev)
{
	sd_dprintf("NOTIFY");
	sd_notify_handler(&ev->sender.node, ev->buf, ev->buf_len);
}

static void (*const zk_event_handlers[])(struct zk_event *ev) = {
	[EVENT_JOIN_REQUEST]	= zk_handle_join_request,
	[EVENT_JOIN_RESPONSE]	= zk_handle_join_response,
	[EVENT_LEAVE]		= zk_handle_leave,
	[EVENT_BLOCK]		= zk_handle_block,
	[EVENT_UNBLOCK]		= zk_handle_unblock,
	[EVENT_NOTIFY]		= zk_handle_notify,
};

static const int zk_max_event_handlers = ARRAY_SIZE(zk_event_handlers);

static void zk_event_handler(int listen_fd, int events, void *data)
{
	eventfd_t value;
	struct zk_event ev;

	sd_dprintf("%d, %d", events, queue_pos);
	if (events & EPOLLHUP) {
		sd_eprintf("zookeeper driver received EPOLLHUP event,"
			   " exiting.");
		log_close();
		exit(1);
	}

	if (eventfd_read(efd, &value) < 0) {
		sd_eprintf("%m");
		return;
	}

	if (!zk_queue_peek())
		goto kick_block_event;

	zk_queue_pop_advance(&ev);
	if (ev.type < zk_max_event_handlers && zk_event_handlers[ev.type])
		zk_event_handlers[ev.type](&ev);
	else
		panic("unhandled type %d", ev.type);

	 /* Someone has created next event, go kick event handler. */
	if (zk_queue_peek()) {
		eventfd_write(efd, 1);
		return;
	}

kick_block_event:
	/*
	 * Kick block event only if there is no nonblock event. We perfer to
	 * handle nonblock event becasue:
	 *
	 * 1. Sheep assuems that unblock() and notify() is a transaction, so we
	 *    can only kick next block event after sd_notify_handler() is called
	 * 2. We should process leave/join event as soon as possible.
	 */
	kick_block_event();
}

static int zk_init(const char *option)
{
	char *hosts, *to, *p;
	int ret, timeout = SESSION_TIMEOUT;

	if (!option) {
		sd_eprintf("You must specify zookeeper servers.");
		return -1;
	}

	hosts = strtok((char *)option, "=");
	if ((to = strtok(NULL, "="))) {
		if (sscanf(to, "%u", &timeout) != 1) {
			sd_eprintf("Invalid paramter for timeout");
			return -1;
		}
		p = strstr(hosts, "timeout");
		*--p = '\0';
	}
	sd_dprintf("version %d.%d.%d, address %s, timeout %d",
		   ZOO_MAJOR_VERSION, ZOO_MINOR_VERSION, ZOO_PATCH_VERSION,
		   hosts, timeout);
	zhandle = zookeeper_init(hosts, zk_watcher, timeout, NULL, NULL, 0);
	if (!zhandle) {
		sd_eprintf("failed to connect to zk server %s", option);
		return -1;
	}

	zk_queue_init();

	efd = eventfd(0, EFD_NONBLOCK);
	if (efd < 0) {
		sd_eprintf("failed to create an event fd: %m");
		return -1;
	}

	ret = register_event(efd, zk_event_handler, NULL);
	if (ret) {
		sd_eprintf("failed to register zookeeper event handler (%d)",
			   ret);
		return -1;
	}

	return 0;
}

static void zk_update_node(struct sd_node *node)
{
	struct zk_node n = {
		.node = *node,
	};
	struct zk_node *t;

	sd_dprintf("%s", node_to_str(&n.node));

	pthread_rwlock_rdlock(&zk_tree_lock);
	t = zk_tree_search_nolock(&n.node.nid);
	if (t) {
		t->node = n.node;
		build_node_list();
	}
	pthread_rwlock_unlock(&zk_tree_lock);
}

static struct cluster_driver cdrv_zookeeper = {
	.name       = "zookeeper",

	.init       = zk_init,
	.join       = zk_join,
	.leave      = zk_leave,
	.notify     = zk_notify,
	.block      = zk_block,
	.unblock    = zk_unblock,
	.update_node = zk_update_node,
};

cdrv_register(cdrv_zookeeper);
