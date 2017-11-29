/*
 * Copyright (c) 2016-2017, Linaro Ltd.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 * this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 * this list of conditions and the following disclaimer in the documentation
 * and/or other materials provided with the distribution.
 *
 * 3. Neither the name of the copyright holder nor the names of its contributors
 * may be used to endorse or promote products derived from this software without
 * specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */
#include <sys/stat.h>
#include <sys/types.h>
#include <assert.h>
#include <ctype.h>
#include <dirent.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#include <libxml/parser.h>
#include <libxml/tree.h>
#include "qdl.h"

static void xml_setpropf(xmlNode *node, const char *attr, const char *fmt, ...)
{
	xmlChar buf[128];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf((char*)buf, sizeof(buf), fmt, ap);
	xmlSetProp(node, (xmlChar*)attr, buf);
	va_end(ap);
}

static xmlNode *firehose_response_parse(const void *buf, size_t len, int *error)
{
	xmlNode *node;
	xmlNode *root;
	xmlDoc *doc;

	doc = xmlReadMemory(buf, len, NULL, NULL, 0);
	if (!doc) {
		fprintf(stderr, "failed to parse firehose packet\n");
		*error = -EINVAL;
		return NULL;
	}

	root = xmlDocGetRootElement(doc);
	for (node = root; node; node = node->next) {
		if (node->type != XML_ELEMENT_NODE)
			continue;
		if (xmlStrcmp(node->name, (xmlChar*)"data") == 0)
			break;
	}

	if (!node) {
		fprintf(stderr, "firehose packet without data tag\n");
		*error = -EINVAL;
		xmlFreeDoc(doc);
		return NULL;
	}

	for (node = node->children; node && node->type != XML_ELEMENT_NODE; node = node->next)
		;

	return node;
}

static void firehose_response_log(xmlNode *node)
{
	xmlChar *value;

	value = xmlGetProp(node, (xmlChar*)"value");
	printf("LOG: %s\n", value);
}

static int firehose_wait(int fd, int timeout)
{
	struct pollfd pfd;
	int ret;

	pfd.fd = fd;
	pfd.events = POLLIN;
	ret = poll(&pfd, 1, timeout);
	if (ret == 0)
		return -ETIMEDOUT;

	return ret < 0 ? ret : 0;;
}

static int firehose_read(int fd, int timeout, int (*response_parser)(xmlNode *node))
{
	char buf[4096];
	xmlNode *nodes;
	xmlNode *node;
	int error;
	char *msg;
	char *end;
	bool done = false;
	int ret = -ENXIO;
	int n;

	while (!done) {
		ret = firehose_wait(fd, timeout);
		if (ret < 0)
			return ret;

		n = read(fd, buf, sizeof(buf));
		if (n == 0) {
			continue;
		} else if (n < 0) {
			warn("failed to read");
			break;
		}
		buf[n] = '\0';

		if (qdl_debug)
			fprintf(stderr, "FIREHOSE READ: %s\n", buf);

		msg = buf;
		for (msg = buf; msg[0]; msg = end) {
			end = strstr(msg, "</data>");
			if (!end) {
				fprintf(stderr, "firehose response truncated\n");
				exit(1);
			}

			end += strlen("</data>");

			nodes = firehose_response_parse(msg, end - msg, &error);
			if (!nodes) {
				fprintf(stderr, "unable to parse response\n");
				return error;
			}

			for (node = nodes; node; node = node->next) {
				if (xmlStrcmp(node->name, (xmlChar*)"log") == 0) {
					firehose_response_log(node);
				} else if (xmlStrcmp(node->name, (xmlChar*)"response") == 0) {
					if (!response_parser)
						fprintf(stderr, "received response with no parser\n");
					else
						ret = response_parser(node);
					done = true;
				}
			}

			xmlFreeDoc(nodes->doc);
		}
	}

	return ret;
}

static int firehose_write(int fd, xmlDoc *doc)
{
	int saved_errno;
	xmlChar *s;
	int len;
	int ret;

	xmlDocDumpMemory(doc, &s, &len);

	if (qdl_debug)
		fprintf(stderr, "FIREHOSE WRITE: %s\n", s);

	ret = write(fd, s, len);
	saved_errno = errno;
	xmlFree(s);
	return ret < 0 ? -saved_errno : 0;
}

static int firehose_nop_parser(xmlNode *node)
{
	xmlChar *value;

	value = xmlGetProp(node, (xmlChar*)"value");
	return !!xmlStrcmp(value, (xmlChar*)"ACK");
}

static int firehose_nop(int fd)
{
	xmlNode *root;
	xmlNode *node;
	xmlDoc *doc;
	int ret;

	doc = xmlNewDoc((xmlChar*)"1.0");
	root = xmlNewNode(NULL, (xmlChar*)"data");
	xmlDocSetRootElement(doc, root);

	node = xmlNewChild(root, NULL, (xmlChar*)"nop", NULL);
	xml_setpropf(node, "value", "ping");

	ret = firehose_write(fd, doc);
	if (ret < 0)
		return ret;

	return firehose_read(fd, -1, firehose_nop_parser);
}

static size_t max_payload_size = 1048576;

static int firehose_configure(int fd)
{
	xmlNode *root;
	xmlNode *node;
	xmlDoc *doc;
	int ret;

	doc = xmlNewDoc((xmlChar*)"1.0");
	root = xmlNewNode(NULL, (xmlChar*)"data");
	xmlDocSetRootElement(doc, root);

	node = xmlNewChild(root, NULL, (xmlChar*)"configure", NULL);
	xml_setpropf(node, "MemoryName", "ufs");
	xml_setpropf(node, "MaxPayloadSizeToTargetInBytes", "%d", max_payload_size);
	xml_setpropf(node, "verbose", "%d", 0);
	xml_setpropf(node, "ZLPAwareHost", "%d", 0);

	ret = firehose_write(fd, doc);
	if (ret < 0)
		return ret;

	return firehose_read(fd, -1, firehose_nop_parser);
}

#define MIN(x, y) ((x) < (y) ? (x) : (y))
#define ROUND_UP(x, a) (((x) + (a) - 1) & ~((a) - 1))

static int firehose_program(int usbfd, struct program *program, int fd)
{
	unsigned num_sectors;
	struct stat sb;
	size_t chunk_size;
	xmlNode *root;
	xmlNode *node;
	xmlDoc *doc;
	void *buf;
	time_t t0;
	time_t t;
	int left;
	int ret;
	int n;

	num_sectors = program->num_sectors;

	if (fd >= 0) {
		fstat(fd, &sb);
		num_sectors = (sb.st_size + program->sector_size - 1) / program->sector_size;

		if (num_sectors > program->num_sectors) {
			fprintf(stderr, "[PROGRAM] %s truncated to %d\n",
				program->label,
				program->num_sectors * program->sector_size);
			num_sectors = program->num_sectors;
		}
	}

	buf = malloc(max_payload_size);
	if (!buf)
		err(1, "failed to allocate sector buffer");

	doc = xmlNewDoc((xmlChar*)"1.0");
	root = xmlNewNode(NULL, (xmlChar*)"data");
	xmlDocSetRootElement(doc, root);

	node = xmlNewChild(root, NULL, (xmlChar*)"program", NULL);
	xml_setpropf(node, "SECTOR_SIZE_IN_BYTES", "%d", program->sector_size);
	xml_setpropf(node, "num_partition_sectors", "%d", num_sectors);
	xml_setpropf(node, "physical_partition_number", "%d", program->partition);
	xml_setpropf(node, "start_sector", "%s", program->start_sector);
	if (program->filename)
		xml_setpropf(node, "filename", "%s", program->filename);

	ret = firehose_write(usbfd, doc);
	if (ret < 0) {
		fprintf(stderr, "[PROGRAM] failed to write program command\n");
		goto out;
	}

	ret = firehose_read(usbfd, -1, firehose_nop_parser);
	if (ret) {
		fprintf(stderr, "[PROGRAM] failed to setup programming\n");
		goto out;
	}

	t0 = time(NULL);

	lseek(fd, program->file_offset * program->sector_size, SEEK_SET);
	left = num_sectors;
	while (left > 0) {
		chunk_size = MIN(max_payload_size / program->sector_size, left);

		if (fd >= 0) {
			n = read(fd, buf, chunk_size * program->sector_size);
			if (n < 0)
				err(1, "failed to read");
		} else {
			n = 0;
		}

		if (n < max_payload_size)
			memset(buf + n, 0, max_payload_size - n);

		n = write(usbfd, buf, chunk_size * program->sector_size);
		if (n < 0)
			err(1, "failed to write");

		if (n != chunk_size * program->sector_size)
			err(1, "failed to write full sector");

		left -= chunk_size;
	}

	t = time(NULL) - t0;

	ret = firehose_read(usbfd, -1, firehose_nop_parser);
	if (ret) {
		fprintf(stderr, "[PROGRAM] failed\n");
	} else if (t) {
		fprintf(stderr,
			"[PROGRAM] flashed \"%s\" successfully at %ldkB/s\n",
			program->label,
			program->sector_size * num_sectors / t / 1024);
	} else {
		fprintf(stderr, "[PROGRAM] flashed \"%s\" successfully\n",
			program->label);
	}

out:
	xmlFreeDoc(doc);
	return ret;
}

static int firehose_apply_patch(int fd, struct patch *patch)
{
	xmlNode *root;
	xmlNode *node;
	xmlDoc *doc;
	int ret;

	printf("%s\n", patch->what);

	doc = xmlNewDoc((xmlChar*)"1.0");
	root = xmlNewNode(NULL, (xmlChar*)"data");
	xmlDocSetRootElement(doc, root);

	node = xmlNewChild(root, NULL, (xmlChar*)"patch", NULL);
	xml_setpropf(node, "SECTOR_SIZE_IN_BYTES", "%d", patch->sector_size);
	xml_setpropf(node, "byte_offset", "%d", patch->byte_offset);
	xml_setpropf(node, "filename", "%s", patch->filename);
	xml_setpropf(node, "physical_partition_number", "%d", patch->partition);
	xml_setpropf(node, "size_in_bytes", "%d", patch->size_in_bytes);
	xml_setpropf(node, "start_sector", "%s", patch->start_sector);
	xml_setpropf(node, "value", "%s", patch->value);

	ret = firehose_write(fd, doc);
	if (ret < 0)
		goto out;

	ret = firehose_read(fd, -1, firehose_nop_parser);
	if (ret)
		fprintf(stderr, "[APPLY PATCH] %d\n", ret);

out:
	xmlFreeDoc(doc);
	return ret;
}

static int firehose_set_bootable(int fd, int part)
{
	xmlNode *root;
	xmlNode *node;
	xmlDoc *doc;
	int ret;

	doc = xmlNewDoc((xmlChar*)"1.0");
	root = xmlNewNode(NULL, (xmlChar*)"data");
	xmlDocSetRootElement(doc, root);

	node = xmlNewChild(root, NULL, (xmlChar*)"setbootablestoragedrive", NULL);
	xml_setpropf(node, "value", "%d", part);

	ret = firehose_write(fd, doc);
	if (ret < 0)
		return ret;

	ret = firehose_read(fd, -1, firehose_nop_parser);
	if (ret) {
		fprintf(stderr, "failed to mark partition %d as bootable\n", part);
		return -1;
	}

	printf("partition %d is now bootable\n", part);
	return 0;
}

static int firehose_reset(int fd)
{
	xmlNode *root;
	xmlNode *node;
	xmlDoc *doc;
	int ret;

	doc = xmlNewDoc((xmlChar*)"1.0");
	root = xmlNewNode(NULL, (xmlChar*)"data");
	xmlDocSetRootElement(doc, root);

	node = xmlNewChild(root, NULL, (xmlChar*)"power", NULL);
	xml_setpropf(node, "value", "reset");

	ret = firehose_write(fd, doc);
	if (ret < 0)
		return ret;

	return firehose_read(fd, -1, firehose_nop_parser);
}

int firehose_run(int fd)
{
	int bootable;
	int ret;

	ret = firehose_wait(fd, 10000);
	if (ret < 0 && ret != -ETIMEDOUT)
		return ret;

	while (firehose_read(fd, 100, NULL) != -ETIMEDOUT)
		;

	ret = firehose_nop(fd);
	if (ret)
		return ret;

	ret = firehose_configure(fd);
	if (ret)
		return ret;

	ret = program_execute(fd, firehose_program);
	if (ret)
		return ret;

	ret = patch_execute(fd, firehose_apply_patch);
	if (ret)
		return ret;

	bootable = program_find_bootable_partition();
	if (bootable < 0)
		fprintf(stderr, "no boot partition found\n");
	else
		firehose_set_bootable(fd, bootable);

	firehose_reset(fd);

	return 0;
}
