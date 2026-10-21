#include "builtin.h"

#include <string.h>

static const sg_builtin_set *const SETS[] = { &sg_builtin_c, &sg_builtin_cpp, &sg_builtin_odin, &sg_builtin_markdown, &sg_builtin_mermaid };

const sg_builtin_set *sg_builtin_templates(const char *language) {
	for (size_t i = 0; i < sizeof SETS / sizeof *SETS; ++i) {
		if (strcmp(language, SETS[i]->language) == 0) {
			return SETS[i];
		}
	}

	return NULL;
}
