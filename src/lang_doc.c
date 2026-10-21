#include "lang.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

static sg_value *fail(char **error, const char *name) {
	*error = sg_format("%s takes a text and no arguments", name);
	return NULL;
}

static sg_value *mdcell(const sg_value *in, sg_value *const *args, size_t count, void *user, char **error) {
	(void)args;
	(void)user;

	if (count || in->kind != SG_V_TEXT) {
		return fail(error, "mdcell");
	}

	sg_buf out = {0};

	for (const char *c = in->text; *c; ++c) {
		if (*c == '|') {
			sg_buf_puts(&out, "\\|");
		} else if (*c == '\n' || *c == '\r') {
			sg_buf_putc(&out, ' ');
		} else {
			sg_buf_putc(&out, *c);
		}
	}

	return sg_value_text_owned(out.data ? out.data : sg_xstrdup(""));
}

static sg_value *anchor(const sg_value *in, sg_value *const *args, size_t count, void *user, char **error) {
	(void)args;
	(void)user;

	if (count || in->kind != SG_V_TEXT) {
		return fail(error, "anchor");
	}

	sg_buf out = {0};

	for (const char *c = in->text; *c; ++c) {
		unsigned char u = (unsigned char)*c;

		if (isalnum(u) || *c == '-' || *c == '_') {
			sg_buf_putc(&out, (char)tolower(u));
		} else if (*c == ' ') {
			sg_buf_putc(&out, '-');
		}
	}

	return sg_value_text_owned(out.data ? out.data : sg_xstrdup(""));
}

static sg_value *mmword(const sg_value *in, sg_value *const *args, size_t count, void *user, char **error) {
	(void)args;
	(void)user;

	if (count || in->kind != SG_V_TEXT) {
		return fail(error, "mmword");
	}

	sg_buf out = {0};
	bool   gap = false;

	for (const char *c = in->text; *c; ++c) {
		unsigned char u = (unsigned char)*c;

		if (isalnum(u) || *c == '_') {
			if (gap && out.len) {
				sg_buf_putc(&out, '_');
			}

			sg_buf_putc(&out, *c);
			gap = false;
		} else {
			gap = true;
		}
	}

	return sg_value_text_owned(out.data ? out.data : sg_xstrdup("_"));
}

size_t sg_lang_markdown_filters(sg_filter out[SG_LANG_MAX_FILTERS], sg_lang_context *context) {
	out[0] = (sg_filter){ "mdcell", mdcell, context };
	out[1] = (sg_filter){ "anchor", anchor, context };
	return 2;
}

size_t sg_lang_mermaid_filters(sg_filter out[SG_LANG_MAX_FILTERS], sg_lang_context *context) {
	out[0] = (sg_filter){ "mmword", mmword, context };
	return 1;
}

/* A description refuses no schema. */
static bool describes_anything(const sg_config *config, const sg_target *target, size_t index,
		const sg_plan *plan) {
	(void)config;
	(void)target;
	(void)index;
	(void)plan;
	return true;
}

const sg_language *sg_language_find(const char *name) {
	static const sg_language LANGUAGES[] = {
		{ "c",        sg_lang_c_filters,        sg_lang_c_check },
		{ "cpp",      sg_lang_cpp_filters,      sg_lang_cpp_check },
		{ "odin",     sg_lang_odin_filters,     sg_lang_odin_check },
		{ "markdown", sg_lang_markdown_filters, describes_anything },
		{ "mermaid",  sg_lang_mermaid_filters,  describes_anything },
	};

	for (size_t i = 0; i < sizeof LANGUAGES / sizeof *LANGUAGES; ++i) {
		if (strcmp(LANGUAGES[i].name, name) == 0) {
			return &LANGUAGES[i];
		}
	}

	return NULL;
}
