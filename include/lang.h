#ifndef SQLGEN_LANG_H
#define SQLGEN_LANG_H

/**
 * @file lang.h
 * @brief What each output language adds to the templates: its filters, and its checks of a target.
 *
 * The languages are C, and two for documentation: Markdown and Mermaid, whose checks find nothing
 * to refuse -- any schema can be described.
 *
 * C's filters:
 *   - `cname`     a text or list of words as a C identifier: snake case, `_` appended to a C
 *                 keyword, `_` put before a leading digit;
 *   - `storage`   how a column is read and bound: "id" (a typed id), "int", "real", "text", "blob",
 *                 or "value" (numeric or ANY affinity, whose type varies by row);
 *   - `ctype`     a column's C type: its typed id, the target's type for its logical type, or the
 *                 type its storage gives (`int64_t`, `double`, `<prefix>_text`, `<prefix>_blob`,
 *                 `<prefix>_value`);
 *   - `idtype`    a table's typed id type, `<prefix>_<table>_<key column>`;
 *   - `cstring`   a text as one C string literal, quotes included;
 *   - `cstrings`  a text as a list of string literals, one per line, so long text spans lines;
 *   - `cbytes`    a text as lines of a byte array's initialiser, NUL-terminated, for text longer
 *                 than the 4095 bytes C99 promises a string literal may hold.
 */

#include "config.h"
#include "plan.h"
#include "template.h"
#include "value.h"

typedef struct {
	const sg_value  *root;     //!< The data the templates see.
	const sg_target *target;
} sg_lang_context;

enum { SG_LANG_MAX_FILTERS = 8 };

/**
 * @brief Fill @p out with the C filters; @p context must outlive their use.
 * @return How many were filled.
 */
size_t sg_lang_c_filters(sg_filter out[SG_LANG_MAX_FILTERS], sg_lang_context *context);

/**
 * @brief Whether the plan can be spelled in C for this target: every logical type from `types` a
 *        column uses must be mapped, and only on integer or real columns, since a C type standing
 *        for one is read and bound as an integer or a double.
 * @param index The target's position in `targets`, for messages.
 */
bool sg_lang_c_check(const sg_config *config, const sg_target *target, size_t index, const sg_plan *plan);

/**
 * @brief Markdown's filters:
 *   - `mdcell`   a text fit for a table cell: `|` escaped, line breaks as spaces;
 *   - `anchor`   a heading's text as the anchor GitHub and most renderers give it: lowercase,
 *                spaces as `-`, anything but letters, digits, `-` and `_` dropped.
 */
size_t sg_lang_markdown_filters(sg_filter out[SG_LANG_MAX_FILTERS], sg_lang_context *context);

/**
 * @brief Mermaid's filters:
 *   - `mmword`   a text as one word Mermaid accepts for an entity, attribute or type: every run of
 *                characters other than letters, digits and `_` becomes one `_`, and none ends it
 *                (`VARCHAR(20)` is `VARCHAR_20`, `UNSIGNED BIG INT` is `UNSIGNED_BIG_INT`);
 *                an empty text is `_`.
 */
size_t sg_lang_mermaid_filters(sg_filter out[SG_LANG_MAX_FILTERS], sg_lang_context *context);

/** @brief An output language: its filters, and what it checks of a target before rendering. */
typedef struct {
	const char *name;
	size_t    (*filters)(sg_filter out[SG_LANG_MAX_FILTERS], sg_lang_context *context);
	bool      (*check)(const sg_config *config, const sg_target *target, size_t index, const sg_plan *plan);
} sg_language;

/** @brief The language called @p name; NULL when sqlgen has none. */
const sg_language *sg_language_find(const char *name);

#endif /* SQLGEN_LANG_H */
