#include "util.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

static unsigned g_errors;
static void   (*g_sink)(const char *line, void *user);
static void    *g_sink_user;

void sg_set_error_sink(void (*sink)(const char *line, void *user), void *user) {
	g_sink      = sink;
	g_sink_user = user;
}

static SG_NORETURN void out_of_memory(void) {
	fputs("sqlgen: out of memory\n", stderr);
	exit(70);
}

static void report(const char *kind, const char *file, unsigned line, unsigned col,
		const char *fmt, va_list args) {
	char head[1024];
	char body[2048];

	if (file && line && col) {
		snprintf(head, sizeof head, "%s:%u:%u: %s: ", file, line, col, kind);
	} else if (file && line) {
		snprintf(head, sizeof head, "%s:%u: %s: ", file, line, kind);
	} else if (file) {
		snprintf(head, sizeof head, "%s: %s: ", file, kind);
	} else {
		snprintf(head, sizeof head, "sqlgen: %s: ", kind);
	}

	vsnprintf(body, sizeof body, fmt, args);

	if (g_sink) {
		char line_text[sizeof head + sizeof body];

		snprintf(line_text, sizeof line_text, "%s%s", head, body);
		g_sink(line_text, g_sink_user);
	} else {
		fprintf(stderr, "%s%s\n", head, body);
	}
}

void sg_error(const char *file, unsigned line, unsigned col, const char *fmt, ...) {
	va_list args;

	va_start(args, fmt);
	report("error", file, line, col, fmt, args);
	va_end(args);
	++g_errors;
}

void sg_warning(const char *file, unsigned line, unsigned col, const char *fmt, ...) {
	va_list args;

	va_start(args, fmt);
	report("warning", file, line, col, fmt, args);
	va_end(args);
}

unsigned sg_error_count(void) {
	return g_errors;
}

void *sg_xmalloc(size_t size) {
	void *ptr = malloc(size ? size : 1);

	if (!ptr) {
		out_of_memory();
	}

	return ptr;
}

void *sg_xrealloc(void *ptr, size_t size) {
	void *grown = realloc(ptr, size ? size : 1);

	if (!grown) {
		out_of_memory();
	}

	return grown;
}

char *sg_xstrdup(const char *text) {
	return text ? sg_xstrndup(text, strlen(text)) : NULL;
}

char *sg_xstrndup(const char *text, size_t len) {
	char *copy = sg_xmalloc(len + 1);

	memcpy(copy, text, len);
	copy[len] = '\0';
	return copy;
}

char *sg_format(const char *fmt, ...) {
	sg_buf  buf = {0};
	va_list args;
	int     needed;

	va_start(args, fmt);
	needed = vsnprintf(NULL, 0, fmt, args);
	va_end(args);

	if (needed < 0) {
		out_of_memory();
	}

	buf.cap  = (size_t)needed + 1;
	buf.data = sg_xmalloc(buf.cap);

	va_start(args, fmt);
	vsnprintf(buf.data, buf.cap, fmt, args);
	va_end(args);

	return buf.data;
}

void *sg_grow(void *items, size_t *cap, size_t size) {
	size_t next = *cap ? *cap * 2 : 8;

	if (next < *cap || next > SIZE_MAX / size) {
		out_of_memory();
	}

	*cap = next;
	return sg_xrealloc(items, next * size);
}

static void buf_reserve(sg_buf *buf, size_t extra) {
	size_t needed = buf->len + extra + 1;

	if (needed < buf->len) {
		out_of_memory();
	}

	if (needed <= buf->cap) {
		return;
	}

	size_t next = buf->cap ? buf->cap : 64;

	while (next < needed) {
		if (next > SIZE_MAX / 2) {
			out_of_memory();
		}

		next *= 2;
	}

	buf->data = sg_xrealloc(buf->data, next);
	buf->cap  = next;
}

void sg_buf_append(sg_buf *buf, const char *bytes, size_t len) {
	buf_reserve(buf, len);
	memcpy(buf->data + buf->len, bytes, len);
	buf->len += len;
	buf->data[buf->len] = '\0';
}

void sg_buf_puts(sg_buf *buf, const char *text) {
	sg_buf_append(buf, text, strlen(text));
}

void sg_buf_putc(sg_buf *buf, char c) {
	sg_buf_append(buf, &c, 1);
}

void sg_buf_printf(sg_buf *buf, const char *fmt, ...) {
	va_list args;
	int     needed;

	va_start(args, fmt);
	needed = vsnprintf(NULL, 0, fmt, args);
	va_end(args);

	if (needed < 0) {
		out_of_memory();
	}

	buf_reserve(buf, (size_t)needed);

	va_start(args, fmt);
	vsnprintf(buf->data + buf->len, (size_t)needed + 1, fmt, args);
	va_end(args);

	buf->len += (size_t)needed;
}

void sg_buf_free(sg_buf *buf) {
	free(buf->data);
	buf->data = NULL;
	buf->len  = 0;
	buf->cap  = 0;
}

bool sg_read_file(const char *path, char **out, size_t *len) {
	FILE  *file = fopen(path, "rb");
	sg_buf buf  = {0};
	char   chunk[65536];
	size_t got;

	if (!file) {
		sg_error(path, 0, 0, "cannot be read: %s", strerror(errno));
		return false;
	}

	while ((got = fread(chunk, 1, sizeof chunk, file)) > 0) {
		sg_buf_append(&buf, chunk, got);
	}

	if (ferror(file)) {
		sg_error(path, 0, 0, "cannot be read: %s", strerror(errno));
		fclose(file);
		sg_buf_free(&buf);
		return false;
	}

	fclose(file);

	if (!buf.data) {
		buf.data = sg_xstrdup("");
	}

	*out = buf.data;
	*len = buf.len;
	return true;
}

bool sg_write_file(const char *path, const char *bytes, size_t len) {
	FILE *file = fopen(path, "wb");

	if (!file) {
		sg_error(path, 0, 0, "cannot be written: %s", strerror(errno));
		return false;
	}

	bool ok = fwrite(bytes, 1, len, file) == len;

	if (fclose(file) != 0) {
		ok = false;
	}

	if (!ok) {
		sg_error(path, 0, 0, "cannot be written: %s", strerror(errno));
	}

	return ok;
}

bool sg_create_file(const char *path, const char *bytes, size_t len) {
	int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0666);

	if (fd < 0) {
		sg_error(path, 0, 0, errno == EEXIST ? "already exists; it was left as it is"
				: "cannot be created: %s", strerror(errno));
		return false;
	}

	size_t done = 0;

	while (done < len) {
		ssize_t wrote = write(fd, bytes + done, len - done);

		if (wrote < 0) {
			if (errno == EINTR) {
				continue;
			}

			sg_error(path, 0, 0, "cannot be written: %s", strerror(errno));
			close(fd);
			return false;
		}

		done += (size_t)wrote;
	}

	if (close(fd) != 0) {
		sg_error(path, 0, 0, "cannot be written: %s", strerror(errno));
		return false;
	}

	return true;
}

bool sg_exists(const char *path) {
	struct stat info;

	return stat(path, &info) == 0;
}

bool sg_is_file(const char *path) {
	struct stat info;

	return stat(path, &info) == 0 && S_ISREG(info.st_mode);
}

bool sg_is_dir(const char *path) {
	struct stat info;

	return stat(path, &info) == 0 && S_ISDIR(info.st_mode);
}

char *sg_path_join(const char *dir, const char *name) {
	if (name[0] == '/' || !dir || dir[0] == '\0' || strcmp(dir, ".") == 0) {
		return sg_xstrdup(name);
	}

	size_t dlen = strlen(dir);

	return dir[dlen - 1] == '/' ? sg_format("%s%s", dir, name) : sg_format("%s/%s", dir, name);
}

char *sg_dirname(const char *path) {
	const char *slash = strrchr(path, '/');

	if (!slash) {
		return sg_xstrdup(".");
	}

	if (slash == path) {
		return sg_xstrdup("/");
	}

	return sg_xstrndup(path, (size_t)(slash - path));
}

void sg_line_col(const char *text, size_t offset, unsigned *line, unsigned *col) {
	unsigned l     = 1;
	size_t   start = 0;

	for (size_t at = 0; at < offset; ++at) {
		if (text[at] == '\n') {
			++l;
			start = at + 1;
		}
	}

	*line = l;
	*col  = (unsigned)(offset - start + 1);
}

static int lower(int c) {
	return c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c;
}

int sg_strcasecmp(const char *a, const char *b) {
	for (;; ++a, ++b) {
		int left  = lower((unsigned char)*a);
		int right = lower((unsigned char)*b);

		if (left != right || left == 0) {
			return left - right;
		}
	}
}

bool sg_contains_nocase(const char *haystack, const char *needle) {
	size_t hlen = strlen(haystack);
	size_t nlen = strlen(needle);

	if (nlen > hlen) {
		return false;
	}

	for (size_t at = 0; at + nlen <= hlen; ++at) {
		size_t i = 0;

		while (i < nlen && lower((unsigned char)haystack[at + i]) == lower((unsigned char)needle[i])) {
			++i;
		}

		if (i == nlen) {
			return true;
		}
	}

	return false;
}

static bool word_char(char c) {
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}

static bool upper_char(char c) {
	return c >= 'A' && c <= 'Z';
}

void sg_split_words(const char *name, void (*emit)(const char *word, void *user), void *user) {
	size_t start = 0;
	size_t len   = strlen(name);

	for (size_t at = 0; at <= len; ++at) {
		bool boundary = at == len || !word_char(name[at])
				|| (at > start && upper_char(name[at]) && !upper_char(name[at - 1]));

		if (!boundary) {
			continue;
		}

		if (at > start) {
			char *word = sg_xstrndup(name + start, at - start);

			for (char *c = word; *c; ++c) {
				*c = upper_char(*c) ? (char)(*c - 'A' + 'a') : *c;
			}

			emit(word, user);
			free(word);
		}

		start = at < len && word_char(name[at]) ? at : at + 1;
	}
}
