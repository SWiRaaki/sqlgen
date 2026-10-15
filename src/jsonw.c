#include "jsonw.h"

static void indent(sg_jw *w) {
	sg_buf_putc(w->out, '\n');

	for (int i = 0; i < w->depth; ++i) {
		sg_buf_putc(w->out, '\t');
	}
}

static void string(sg_jw *w, const char *text) {
	sg_buf_putc(w->out, '"');

	for (const unsigned char *c = (const unsigned char *)text; *c; ++c) {
		switch (*c) {
		case '"':  sg_buf_puts(w->out, "\\\""); break;
		case '\\': sg_buf_puts(w->out, "\\\\"); break;
		case '\n': sg_buf_puts(w->out, "\\n");  break;
		case '\r': sg_buf_puts(w->out, "\\r");  break;
		case '\t': sg_buf_puts(w->out, "\\t");  break;
		default:
			if (*c < 0x20) {
				sg_buf_printf(w->out, "\\u%04x", *c);
			} else {
				sg_buf_putc(w->out, (char)*c);
			}
		}
	}

	sg_buf_putc(w->out, '"');
}

/* Separator, line break and key for the next member of the current container. */
static void member(sg_jw *w, const char *key) {
	if (w->depth > 0) {
		if (!w->first[w->depth]) {
			sg_buf_putc(w->out, ',');
		}

		if (w->flat[w->depth]) {
			if (!w->first[w->depth]) {
				sg_buf_putc(w->out, ' ');
			}
		} else {
			indent(w);
		}

		w->first[w->depth] = false;
	}

	if (key) {
		string(w, key);
		sg_buf_puts(w->out, ": ");
	}
}

void sg_jw_open(sg_jw *w, const char *key, char brace, bool flat) {
	bool parent_flat = w->depth > 0 && w->flat[w->depth];

	member(w, key);
	sg_buf_putc(w->out, brace);
	++w->depth;
	w->first[w->depth]   = true;
	w->flat[w->depth] = flat || parent_flat;
}

void sg_jw_close(sg_jw *w, char brace) {
	bool empty = w->first[w->depth];
	bool flat  = w->flat[w->depth];

	--w->depth;

	if (!empty && !flat) {
		indent(w);
	}

	sg_buf_putc(w->out, brace);
}

void sg_jw_text(sg_jw *w, const char *key, const char *value) {
	member(w, key);

	if (value) {
		string(w, value);
	} else {
		sg_buf_puts(w->out, "null");
	}
}

void sg_jw_bool(sg_jw *w, const char *key, bool value) {
	member(w, key);
	sg_buf_puts(w->out, value ? "true" : "false");
}

void sg_jw_int(sg_jw *w, const char *key, long long value) {
	member(w, key);
	sg_buf_printf(w->out, "%lld", value);
}

void sg_jw_null(sg_jw *w, const char *key) {
	member(w, key);
	sg_buf_puts(w->out, "null");
}

void sg_jw_names(sg_jw *w, const char *key, const sg_names *list) {
	sg_jw_open(w, key, '[', true);

	for (size_t i = 0; i < list->count; ++i) {
		sg_jw_text(w, NULL, list->items[i]);
	}

	sg_jw_close(w, ']');
}
