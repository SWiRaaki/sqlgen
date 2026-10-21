#ifndef SQLGEN_BUILTIN_H
#define SQLGEN_BUILTIN_H

/**
 * @file builtin.h
 * @brief The template sets compiled into sqlgen, one per language.
 *
 * Their sources are the files under `templates/<language>/`; `tools/embed.c` turns them into C at
 * build time. `sqlgen templates <language> <dir>` writes them back out, to be copied and changed.
 */

#include <stddef.h>

#include "template.h"

typedef struct {
	const char               *language;
	const sg_template_source *templates;
	size_t                    count;
} sg_builtin_set;

extern const sg_builtin_set sg_builtin_c;
extern const sg_builtin_set sg_builtin_cpp;
extern const sg_builtin_set sg_builtin_odin;
extern const sg_builtin_set sg_builtin_markdown;
extern const sg_builtin_set sg_builtin_mermaid;

/** @brief The built-in set for @p language; NULL when there is none. */
const sg_builtin_set *sg_builtin_templates(const char *language);

#endif /* SQLGEN_BUILTIN_H */
