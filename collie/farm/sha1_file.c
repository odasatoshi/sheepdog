/*
 * Copyright (C) 2011 Taobao Inc.
 *
 * Liu Yuan <namei.unix@gmail.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License version
 * 2 as published by the Free Software Foundation.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

/*
 *   sha1_file provide us some useful features:
 *
 *   - Regardless of object type, all objects are all in deflated with zlib,
 *     and have a header that not only specifies their tag, but also size
 *     information about the data in the object.
 *
 *   - the general consistency of an object can always be tested independently
 *     of the contents or the type of the object: all objects can be validated
 *     by verifying that their hashes match the content of the file.
 */
#include <sys/types.h>
#include <sys/xattr.h>

#include "farm.h"
#include "util.h"

static void get_sha1(unsigned char *buf, unsigned len, unsigned char *sha1)
{
	struct sha1_ctx c;
	uint64_t offset = 0;
	uint32_t length = len;
	void *tmp = valloc(length);

	memcpy(tmp, buf, len);
	trim_zero_sectors(tmp, &offset, &length);

	sha1_init(&c);
	sha1_update(&c, (uint8_t *)&offset, sizeof(offset));
	sha1_update(&c, (uint8_t *)&length, sizeof(length));
	sha1_update(&c, tmp, length);
	sha1_final(&c, sha1);
	free(tmp);
}

static void fill_sha1_path(char *pathbuf, const unsigned char *sha1)
{
	int i;
	for (i = 0; i < SHA1_DIGEST_SIZE; i++) {
		static const char hex[] = "0123456789abcdef";
		unsigned int val = sha1[i];
		char *pos = pathbuf + i*2 + (i > 0);
		*pos++ = hex[val >> 4];
		*pos = hex[val & 0xf];
	}
}

static char *sha1_to_path(const unsigned char *sha1)
{
	static __thread char buf[PATH_MAX];
	const char *objdir;
	int len;

	objdir = get_object_directory();
	len = strlen(objdir);

	/* '/' + sha1(2) + '/' + sha1(38) + '\0' */
	memcpy(buf, objdir, len);
	buf[len] = '/';
	buf[len+3] = '/';
	buf[len+42] = '\0';
	fill_sha1_path(buf + len + 1, sha1);
	return buf;
}

#define CNAME	"user.farm.count"
#define CSIZE	sizeof(uint32_t)

static void get_sha1_file(char *name)
{
	uint32_t count;
	if (getxattr(name, CNAME, &count, CSIZE) < 0) {
		if (errno == ENODATA) {
			count = 1;
			if (setxattr(name, CNAME, &count, CSIZE, 0) < 0)
				panic("%m");
			return;
		} else
			panic("%m");
	}
	count++;
	if (setxattr(name, CNAME, &count, CSIZE, 0) < 0)
		panic("%m");
}

static int put_sha1_file(char *name)
{
	uint32_t count;

	if (getxattr(name, CNAME, &count, CSIZE) < 0) {
		if (errno == ENOENT) {
			fprintf(stderr, "sha1 file doesn't exist.\n");
			return -1;
		} else
			panic("%m");
	}

	count--;
	if (count == 0) {
		if (unlink(name) < 0) {
			fprintf(stderr, "%m\n");
			return -1;
		}
	} else {
		if (setxattr(name, CNAME, &count, CSIZE, 0) < 0)
			panic("%m");
	}
	return 0;
}

static int sha1_buffer_write(const unsigned char *sha1,
			     void *buf, unsigned int size)
{
	char *filename = sha1_to_path(sha1);
	int fd, ret = 0, len;

	fd = open(filename, O_WRONLY | O_CREAT | O_EXCL, 0666);
	if (fd < 0) {
		if (errno != EEXIST) {
			fprintf(stderr,
				"failed to open file %s with error: %m\n",
				filename);
			ret = -1;
		}
		goto err_open;
	}
	len = xwrite(fd, buf, size);
	if (len != size) {
		fprintf(stderr, "%m\n");
		close(fd);
		return -1;
	}

	close(fd);
	get_sha1_file(filename);
err_open:
	return ret;
}

bool sha1_file_exist(const unsigned char *sha1)
{
	return (access(sha1_to_path(sha1), R_OK) == 0);
}

int sha1_file_write(void *buf, size_t len, unsigned char *outsha1)
{
	unsigned char sha1[SHA1_DIGEST_SIZE];

	get_sha1(buf, len, sha1);
	if (sha1_buffer_write(sha1, buf, len) < 0)
		return -1;
	if (outsha1)
		memcpy(outsha1, sha1, SHA1_DIGEST_SIZE);
	return 0;
}

static int verify_sha1_file(const unsigned char *sha1,
			    void *buf, unsigned long len)
{
	unsigned char tmp[SHA1_DIGEST_SIZE];

	get_sha1(buf, len, tmp);
	if (memcmp((char *)tmp, (char *)sha1, SHA1_DIGEST_SIZE) != 0) {
		fprintf(stderr, "failed, %s != %s\n", sha1_to_hex(sha1),
			sha1_to_hex(tmp));
		return -1;
	}
	return 0;
}

void *sha1_file_read(const unsigned char *sha1, size_t *size)
{
	char *filename = sha1_to_path(sha1);
	int fd = open(filename, O_RDONLY);
	struct stat st;
	void *buf = NULL;

	if (fd < 0) {
		perror(filename);
		return NULL;
	}
	if (fstat(fd, &st) < 0) {
		fprintf(stderr, "%m\n");
		goto out;
	}

	buf = xmalloc(st.st_size);
	if (!buf)
		goto out;

	if (xread(fd, buf, st.st_size) != st.st_size) {
		free(buf);
		buf = NULL;
		goto out;
	}

	if (verify_sha1_file(sha1, buf, st.st_size) < 0) {
		free(buf);
		buf = NULL;
		goto out;
	}

	*size = st.st_size;
out:
	close(fd);
	return buf;
}

int sha1_file_try_delete(const unsigned char *sha1)
{
	char *filename = sha1_to_path(sha1);

	return put_sha1_file(filename);
}

static unsigned hexval(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return ~0;
}

int get_sha1_hex(const char *hex, unsigned char *sha1)
{
	int i;
	for (i = 0; i < SHA1_DIGEST_SIZE; i++) {
		unsigned int val = (hexval(hex[0]) << 4) | hexval(hex[1]);
		if (val & ~0xff)
			return -1;
		*sha1++ = val;
		hex += 2;
	}
	return 0;
}
