/* 
    Xen Store Daemon Test tool
    Copyright (C) 2005 Rusty Russell IBM Corporation

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
*/

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <fnmatch.h>
#include <stdarg.h>
#include <string.h>
#include <getopt.h>
#include <ctype.h>
#include <sys/time.h>
#include "utils.h"
#include "xs_lib.h"
#include "list.h"

#define XSTEST

static struct xs_handle *handles[10] = { NULL };

static unsigned int timeout_ms = 200;
static bool timeout_suppressed = true;
static bool readonly = false;
static bool print_input = false;
static unsigned int linenum = 0;

struct ringbuf_head
{
	uint32_t write; /* Next place to write to */
	uint32_t read; /* Next place to read from */
	uint8_t flags;
	char buf[0];
} __attribute__((packed));

static struct ringbuf_head *out, *in;
static unsigned int ringbuf_datasize;
static int daemon_pid;

/* FIXME: Mark connection as broken (close it?) when this happens. */
static bool check_buffer(const struct ringbuf_head *h)
{
	return (h->write < ringbuf_datasize && h->read < ringbuf_datasize);
}

/* We can't fill last byte: would look like empty buffer. */
static void *get_output_chunk(const struct ringbuf_head *h,
			      void *buf, uint32_t *len)
{
	uint32_t read_mark;

	if (h->read == 0)
		read_mark = ringbuf_datasize - 1;
	else
		read_mark = h->read - 1;

	/* Here to the end of buffer, unless they haven't read some out. */
	*len = ringbuf_datasize - h->write;
	if (read_mark >= h->write)
		*len = read_mark - h->write;
	return buf + h->write;
}

static const void *get_input_chunk(const struct ringbuf_head *h,
				   const void *buf, uint32_t *len)
{
	/* Here to the end of buffer, unless they haven't written some. */
	*len = ringbuf_datasize - h->read;
	if (h->write >= h->read)
		*len = h->write - h->read;
	return buf + h->read;
}

static int output_avail(struct ringbuf_head *out)
{
	unsigned int avail;

	get_output_chunk(out, out->buf, &avail);
	return avail != 0;
}

static void update_output_chunk(struct ringbuf_head *h, uint32_t len)
{
	h->write += len;
	if (h->write == ringbuf_datasize)
		h->write = 0;
}

static void update_input_chunk(struct ringbuf_head *h, uint32_t len)
{
	h->read += len;
	if (h->read == ringbuf_datasize)
		h->read = 0;
}

/* FIXME: We spin, and we're sloppy. */
static bool read_all_shmem(int fd __attribute__((unused)),
			   void *data, unsigned int len)
{
	unsigned int avail;
	int was_full;

	if (!check_buffer(in))
		barf("Corrupt buffer");

	was_full = !output_avail(in);
	while (len) {
		const void *src = get_input_chunk(in, in->buf, &avail);
		if (avail > len)
			avail = len;
		memcpy(data, src, avail);
		data += avail;
		len -= avail;
		update_input_chunk(in, avail);
	}

	/* Tell other end we read something. */
	if (was_full)
		kill(daemon_pid, SIGUSR2);
	return true;
}

static bool write_all_shmem(int fd __attribute__((unused)),
			    const void *data, unsigned int len)
{
	uint32_t avail;

	if (!check_buffer(out))
		barf("Corrupt buffer");

	while (len) {
		void *dst = get_output_chunk(out, out->buf, &avail);
		if (avail > len)
			avail = len;
		memcpy(dst, data, avail);
		data += avail;
		len -= avail;
		update_output_chunk(out, avail);
	}

	/* Tell other end we wrote something. */
	kill(daemon_pid, SIGUSR2);
	return true;
}

static bool read_all(int fd, void *data, unsigned int len);
static bool read_all_choice(int fd, void *data, unsigned int len)
{
	if (fd == -2)
		return read_all_shmem(fd, data, len);
	return read_all(fd, data, len);
}

static bool write_all_choice(int fd, const void *data, unsigned int len)
{
	if (fd == -2)
		return write_all_shmem(fd, data, len);
	return xs_write_all(fd, data, len);
}

/* We want access to internal functions. */
#include "xs.c"

static void __attribute__((noreturn)) usage(void)
{
	barf("Usage:\n"
	     "       xs_test [--readonly] [--no-timeout] [-x]\n"
	     "Reads commands from stdin, one per line:"
	     "  dir <path>\n"
	     "  read <path>\n"
	     "  write <path> <flags> <value>...\n"
	     "  setid <id>\n"
	     "  mkdir <path>\n"
	     "  rm <path>\n"
	     "  getperm <path>\n"
	     "  setperm <path> <id> <flags> ...\n"
	     "  shutdown\n"
	     "  watch <path> <token>\n"
	     "  waitwatch\n"
	     "  ackwatch <token>\n"
	     "  unwatch <path> <token>\n"
	     "  close\n"
	     "  start <node>\n"
	     "  abort\n"
	     "  introduce <domid> <mfn> <eventchn> <path>\n"
	     "  commit\n"
	     "  sleep <milliseconds>\n"
	     "  expect <pattern>\n"
	     "  notimeout\n"
	     "  readonly\n"
	     "  readwrite\n"
	     "  noackwrite <path> <flags> <value>...\n"
	     "  readack\n"
	     "  dump\n");
}

static int argpos(const char *line, unsigned int num)
{
	unsigned int i, len = 0, off = 0;

	for (i = 0; i <= num; i++) {
		off += len;
		off += strspn(line + off, " \t\n");
		len = strcspn(line + off, " \t\n");
		if (!len)
			return off;
	}
	return off;
}

static char *arg(const char *line, unsigned int num)
{
	static char *args[10];
	unsigned int off, len;

	off = argpos(line, num);
	len = strcspn(line + off, " \t\n");

	if (!len)
		barf("Can't get arg %u", num);

	free(args[num]);
	args[num] = malloc(len + 1);
	memcpy(args[num], line+off, len);
	args[num][len] = '\0';
	return args[num];
}

struct expect
{
	struct list_head list;
	char *pattern;
};
static LIST_HEAD(expects);

static char *command;

/* Trim leading and trailing whitespace */
static void trim(char *str)
{
	while (isspace(str[0]))
		memmove(str, str+1, strlen(str));

	while (strlen(str) && isspace(str[strlen(str)-1]))
		str[strlen(str)-1] = '\0';
}

static void output(const char *fmt, ...)
{
	char *str;
	struct expect *i;
	va_list arglist;

	va_start(arglist, fmt);
	vasprintf(&str, fmt, arglist);
	va_end(arglist);

	printf("%s", str);
	fflush(stdout);
	trim(str);
	list_for_each_entry(i, &expects, list) {
		if (fnmatch(i->pattern, str, 0) == 0) {
			list_del(&i->list);
			free(i);
			return;
		}
	}
	barf("Unexpected output %s\n", str);
}

static void failed(int handle)
{
	if (handle)
		output("%i: %s failed: %s\n",
		       handle, command, strerror(errno));
	else
		output("%s failed: %s\n", command, strerror(errno));
}

static void expect(const char *line)
{
	struct expect *e = malloc(sizeof(*e));

	e->pattern = strdup(line + argpos(line, 1));
	trim(e->pattern);
	list_add(&e->list, &expects);
}

static void do_dir(unsigned int handle, char *path)
{
	char **entries;
	unsigned int i, num;

	entries = xs_directory(handles[handle], path, &num);
	if (!entries) {
		failed(handle);
		return;
	}

	for (i = 0; i < num; i++)
		if (handle)
			output("%i:%s\n", handle, entries[i]);
		else
			output("%s\n", entries[i]);
	free(entries);
}

static void do_read(unsigned int handle, char *path)
{
	char *value;
	unsigned int len;

	value = xs_read(handles[handle], path, &len);
	if (!value) {
		failed(handle);
		return;
	}

	/* It's supposed to nul terminate for us. */
	assert(value[len] == '\0');
	if (handle)
		output("%i:%.*s\n", handle, len, value);
	else
		output("%.*s\n", len, value);
}

static void do_write(unsigned int handle, char *path, char *flags, char *data)
{
	int f;

	if (streq(flags, "none"))
		f = 0;
	else if (streq(flags, "create"))
		f = O_CREAT;
	else if (streq(flags, "excl"))
		f = O_CREAT | O_EXCL;
	else if (streq(flags, "crap"))
		f = 100;
	else
		barf("write flags 'none', 'create' or 'excl' only");

	if (!xs_write(handles[handle], path, data, strlen(data), f))
		failed(handle);
}

static void do_noackwrite(unsigned int handle,
			  char *path, const char *flags, char *data)
{
	struct xsd_sockmsg msg;

	/* Format: Flags (as string), path, data. */
	if (streq(flags, "none"))
		flags = XS_WRITE_NONE;
	else if (streq(flags, "create"))
		flags = XS_WRITE_CREATE;
	else if (streq(flags, "excl"))
		flags = XS_WRITE_CREATE_EXCL;
	else
		barf("noackwrite flags 'none', 'create' or 'excl' only");

	msg.len = strlen(path) + 1 + strlen(flags) + 1 + strlen(data);
	msg.type = XS_WRITE;
	if (!write_all_choice(handles[handle]->fd, &msg, sizeof(msg)))
		failed(handle);
	if (!write_all_choice(handles[handle]->fd, path, strlen(path) + 1))
		failed(handle);
	if (!write_all_choice(handles[handle]->fd, flags, strlen(flags) + 1))
		failed(handle);
	if (!write_all_choice(handles[handle]->fd, data, strlen(data)))
		failed(handle);
	/* Do not wait for ack. */
}

static void do_readack(unsigned int handle)
{
	enum xsd_sockmsg_type type;
	char *ret;

	ret = read_reply(handles[handle]->fd, &type, NULL);
	if (!ret)
		failed(handle);
	free(ret);
}

static void do_setid(unsigned int handle, char *id)
{
	if (!xs_bool(xs_debug_command(handles[handle], "setid", id,
				      strlen(id)+1)))
		failed(handle);
}

static void do_mkdir(unsigned int handle, char *path)
{
	if (!xs_mkdir(handles[handle], path))
		failed(handle);
}

static void do_rm(unsigned int handle, char *path)
{
	if (!xs_rm(handles[handle], path))
		failed(handle);
}

static void do_getperm(unsigned int handle, char *path)
{
	unsigned int i, num;
	struct xs_permissions *perms;

	perms = xs_get_permissions(handles[handle], path, &num);
	if (!perms) {
		failed(handle);
		return;
	}

	for (i = 0; i < num; i++) {
		char *permstring;

		switch (perms[i].perms) {
		case XS_PERM_NONE:
			permstring = "NONE";
			break;
		case XS_PERM_WRITE:
			permstring = "WRITE";
			break;
		case XS_PERM_READ:
			permstring = "READ";
			break;
		case XS_PERM_READ|XS_PERM_WRITE:
			permstring = "READ/WRITE";
			break;
		default:
			barf("bad perm value %i", perms[i].perms);
		}

		if (handle)
			output("%i:%i %s\n", handle, perms[i].id, permstring);
		else
			output("%i %s\n", perms[i].id, permstring);
	}
	free(perms);
}

static void do_setperm(unsigned int handle, char *path, char *line)
{
	unsigned int i;
	struct xs_permissions perms[100];

	strtok(line, " \t\n");
	strtok(NULL, " \t\n");
	for (i = 0; ; i++) {
		char *arg = strtok(NULL, " \t\n");
		if (!arg)
			break;
		perms[i].id = atoi(arg);
		arg = strtok(NULL, " \t\n");
		if (!arg)
			break;
		if (streq(arg, "WRITE"))
			perms[i].perms = XS_PERM_WRITE;
		else if (streq(arg, "READ"))
			perms[i].perms = XS_PERM_READ;
		else if (streq(arg, "READ/WRITE"))
			perms[i].perms = XS_PERM_READ|XS_PERM_WRITE;
		else if (streq(arg, "NONE"))
			perms[i].perms = XS_PERM_NONE;
		else
			barf("bad flags %s\n", arg);
	}

	if (!xs_set_permissions(handles[handle], path, perms, i))
		failed(handle);
}

static void do_shutdown(unsigned int handle)
{
	if (!xs_shutdown(handles[handle]))
		failed(handle);
}

static void do_watch(unsigned int handle, const char *node, const char *token)
{
	if (!xs_watch(handles[handle], node, token))
		failed(handle);
}

static void set_timeout(void)
{
	struct itimerval timeout;

	timeout.it_value.tv_sec = timeout_ms / 1000;
	timeout.it_value.tv_usec = (timeout_ms * 1000) % 1000000;
	timeout.it_interval.tv_sec = timeout.it_interval.tv_usec = 0;
	setitimer(ITIMER_REAL, &timeout, NULL);
}

static void disarm_timeout(void)
{
	struct itimerval timeout;

	timeout.it_value.tv_sec = 0;
	timeout.it_value.tv_usec = 0;
	setitimer(ITIMER_REAL, &timeout, NULL);
}

static void do_waitwatch(unsigned int handle)
{
	char **vec;
	struct timeval tv = {.tv_sec = timeout_ms/1000,
			     .tv_usec = (timeout_ms*1000)%1000000 };
	fd_set set;

	if (xs_fileno(handles[handle]) != -2) {
		/* Manually select here so we can time out gracefully. */
		FD_ZERO(&set);
		FD_SET(xs_fileno(handles[handle]), &set);
		disarm_timeout();
		if (select(xs_fileno(handles[handle])+1, &set,
			   NULL, NULL, &tv) == 0) {
			errno = ETIMEDOUT;
			failed(handle);
			return;
		}
		set_timeout();
	}

	vec = xs_read_watch(handles[handle]);
	if (!vec) {
		failed(handle);
		return;
	}

	if (handle)
		output("%i:%s:%s\n", handle, vec[0], vec[1]);
	else
		output("%s:%s\n", vec[0], vec[1]);
	free(vec);
}

static void do_ackwatch(unsigned int handle, const char *token)
{
	if (!xs_acknowledge_watch(handles[handle], token))
		failed(handle);
}

static void do_unwatch(unsigned int handle, const char *node, const char *token)
{
	if (!xs_unwatch(handles[handle], node, token))
		failed(handle);
}

static void do_start(unsigned int handle, const char *node)
{
	if (!xs_transaction_start(handles[handle], node))
		failed(handle);
}

static void do_end(unsigned int handle, bool abort)
{
	if (!xs_transaction_end(handles[handle], abort))
		failed(handle);
}

static void do_introduce(unsigned int handle,
			 const char *domid,
			 const char *mfn,
			 const char *eventchn,
			 const char *path)
{
	unsigned int i;
	int fd;

	/* This mechanism is v. slow w. valgrind running. */
	timeout_ms = 5000;

	/* We poll, so ignore signal */
	signal(SIGUSR2, SIG_IGN);
	for (i = 0; i < ARRAY_SIZE(handles); i++)
		if (!handles[i])
			break;

	fd = open("/tmp/xcmap", O_RDWR);
	/* Set in and out pointers. */
	out = mmap(NULL, getpagesize(), PROT_WRITE|PROT_READ, MAP_SHARED,fd,0);
	if (out == MAP_FAILED)
		barf_perror("Failed to map /tmp/xcmap page");
	in = (void *)out + getpagesize() / 2;
	close(fd);

	/* Tell them the event channel and our PID. */
	*(int *)((void *)out + 32) = getpid();
	*(u16 *)((void *)out + 36) = atoi(eventchn);

	if (!xs_introduce_domain(handles[handle], atoi(domid),
				 atol(mfn), atoi(eventchn), path)) {
		failed(handle);
		munmap(out, getpagesize());
		return;
	}
	output("handle is %i\n", i);

	/* Create new handle. */
	handles[i] = new(struct xs_handle);
	handles[i]->fd = -2;

	/* Read in daemon pid. */
	daemon_pid = *(int *)((void *)out + 32);
}

static void do_release(unsigned int handle, const char *domid)
{
	if (!xs_release_domain(handles[handle], atoi(domid)))
		failed(handle);
}

static int strptrcmp(const void *a, const void *b)
{
	return strcmp(*(char **)a, *(char **)b);
}

static void sort_dir(char **dir, unsigned int num)
{
	qsort(dir, num, sizeof(char *), strptrcmp);
}

static void dump_dir(unsigned int handle,
		     const char *node,
		     char **dir,
		     unsigned int numdirs,
		     unsigned int depth)
{
	unsigned int i;
	char spacing[depth+1];

	memset(spacing, ' ', depth);
	spacing[depth] = '\0';

	sort_dir(dir, numdirs);

	for (i = 0; i < numdirs; i++) {
		struct xs_permissions *perms;
		unsigned int j, numperms;
		unsigned int len;
		char *contents;
		unsigned int subnum;
		char **subdirs;
		char subnode[strlen(node) + 1 + strlen(dir[i]) + 1];

		sprintf(subnode, "%s/%s", node, dir[i]);

		perms = xs_get_permissions(handles[handle], subnode,&numperms);
		if (!perms) {
			failed(handle);
			return;
		}

		output("%s%s: ", spacing, dir[i]);
		for (j = 0; j < numperms; j++) {
			char buffer[100];
			if (!xs_perm_to_string(&perms[j], buffer))
				barf("perm to string");
			output("%s ", buffer);
		}
		free(perms);
		output("\n");

		/* Even directories can have contents. */
		contents = xs_read(handles[handle], subnode, &len);
		if (!contents) {
			if (errno != EISDIR)
				failed(handle);
		} else {
			output(" %s(%.*s)\n", spacing, len, contents);
			free(contents);
		}			

		/* Every node is a directory. */
		subdirs = xs_directory(handles[handle], subnode, &subnum);
		if (!subdirs) {
			failed(handle);
			return;
		}
		dump_dir(handle, subnode, subdirs, subnum, depth+1);
		free(subdirs);
	}
}

static void dump(int handle)
{
	char **subdirs;
	unsigned int subnum;

	subdirs = xs_directory(handles[handle], "/", &subnum);
	if (!subdirs) {
		failed(handle);
		return;
	}

	dump_dir(handle, "", subdirs, subnum, 0);
	free(subdirs);
}

static int handle;

static void alarmed(int sig __attribute__((unused)))
{
	if (handle) {
		char handlename[10];
		sprintf(handlename, "%u:", handle);
		write(STDOUT_FILENO, handlename, strlen(handlename));
	}
	write(STDOUT_FILENO, command, strlen(command));
	write(STDOUT_FILENO, " timeout\n", strlen(" timeout\n"));
	exit(1);
}

static void do_command(unsigned int default_handle, char *line)
{
	char *endp;

	if (print_input)
		printf("%i> %s", ++linenum, line);

	if (strspn(line, " \n") == strlen(line))
		return;
	if (strstarts(line, "#"))
		return;

	handle = strtoul(line, &endp, 10);
	if (endp != line)
		memmove(line, endp+1, strlen(endp));
	else
		handle = default_handle;

	command = arg(line, 0);
	if (!handles[handle]) {
		if (readonly)
			handles[handle] = xs_daemon_open_readonly();
		else
			handles[handle] = xs_daemon_open();
		if (!handles[handle])
			barf_perror("Opening connection to daemon");
	}

	if (!timeout_suppressed)
		set_timeout();
	timeout_suppressed = false;

	if (streq(command, "dir"))
		do_dir(handle, arg(line, 1));
	else if (streq(command, "read"))
		do_read(handle, arg(line, 1));
	else if (streq(command, "write"))
		do_write(handle,
			 arg(line, 1), arg(line, 2), arg(line, 3));
	else if (streq(command, "setid"))
		do_setid(handle, arg(line, 1));
	else if (streq(command, "mkdir"))
		do_mkdir(handle, arg(line, 1));
	else if (streq(command, "rm"))
		do_rm(handle, arg(line, 1));
	else if (streq(command, "getperm"))
		do_getperm(handle, arg(line, 1));
	else if (streq(command, "setperm"))
		do_setperm(handle, arg(line, 1), line);
	else if (streq(command, "shutdown"))
		do_shutdown(handle);
	else if (streq(command, "watch"))
		do_watch(handle, arg(line, 1), arg(line, 2));
	else if (streq(command, "waitwatch"))
		do_waitwatch(handle);
	else if (streq(command, "ackwatch"))
		do_ackwatch(handle, arg(line, 1));
	else if (streq(command, "unwatch"))
		do_unwatch(handle, arg(line, 1), arg(line, 2));
	else if (streq(command, "close")) {
		xs_daemon_close(handles[handle]);
		handles[handle] = NULL;
	} else if (streq(command, "start"))
		do_start(handle, arg(line, 1));
	else if (streq(command, "commit"))
		do_end(handle, false);
	else if (streq(command, "abort"))
		do_end(handle, true);
	else if (streq(command, "introduce"))
		do_introduce(handle, arg(line, 1), arg(line, 2),
			     arg(line, 3), arg(line, 4));
	else if (streq(command, "release"))
		do_release(handle, arg(line, 1));
	else if (streq(command, "dump"))
		dump(handle);
	else if (streq(command, "sleep")) {
		disarm_timeout();
		usleep(atoi(arg(line, 1)) * 1000);
	} else if (streq(command, "expect"))
		expect(line);
	else if (streq(command, "notimeout"))
		timeout_suppressed = true;
	else if (streq(command, "readonly")) {
		readonly = true;
		xs_daemon_close(handles[handle]);
		handles[handle] = NULL;
	} else if (streq(command, "readwrite")) {
		readonly = false;
		xs_daemon_close(handles[handle]);
		handles[handle] = NULL;
	} else if (streq(command, "noackwrite"))
		do_noackwrite(handle, arg(line,1), arg(line,2), arg(line,3));
	else if (streq(command, "readack"))
		do_readack(handle);
	else
		barf("Unknown command %s", command);
	fflush(stdout);
	disarm_timeout();

	/* Check expectations. */
	if (!streq(command, "expect")) {
		struct expect *i = list_top(&expects, struct expect, list);

		if (i)
			barf("Expected '%s', didn't happen\n", i->pattern);
	}
}

static struct option options[] = { { "readonly", 0, NULL, 'r' },
				   { "no-timeout", 0, NULL, 't' },
				   { NULL, 0, NULL, 0 } };

int main(int argc, char *argv[])
{
	int opt;
	char line[1024];

	while ((opt = getopt_long(argc, argv, "xrt", options, NULL)) != -1) {
		switch (opt) {
		case 'r':
			readonly = true;
			break;
		case 't':
			timeout_ms = 0;
			break;
		case 'x':
			print_input = true;
			break;
		}
	}

	if (optind + 1 == argc) {
		int fd = open(argv[optind], O_RDONLY);
		if (!fd)
			barf_perror("Opening %s", argv[optind]);
		dup2(fd, STDIN_FILENO);
	} else if (optind != argc)
		usage();
	

	/* The size of the ringbuffer: half a page minus head structure. */
	ringbuf_datasize = getpagesize() / 2 - sizeof(struct ringbuf_head);

	signal(SIGALRM, alarmed);
	while (fgets(line, sizeof(line), stdin))
		do_command(0, line);

	return 0;
}
