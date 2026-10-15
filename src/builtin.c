#include "builtin.h"

#include <string.h>

const sg_builtin_set *sg_builtin_templates(const char *language) {
	return strcmp(language, sg_builtin_c.language) == 0 ? &sg_builtin_c : NULL;
}
