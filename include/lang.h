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
 * @brief C++'s filters: `storage`, `cstring`, `cstrings` and `cbytes` as for C, and
 *   - `cppname`   as `cname`, with C++'s keywords as well;
 *   - `cpptype`   a column's C++ type: its typed id, `int64_t`, `double`, `std::string`,
 *                 `std::vector<unsigned char>` or `Value`; a logical type from `types` is spelt by
 *                 its storage;
 *   - `idtype`    a table's typed id type inside the prefix's namespace, `<Table><KeyColumn>`.
 */
size_t sg_lang_cpp_filters(sg_filter out[SG_LANG_MAX_FILTERS], sg_lang_context *context);

/**
 * @brief Whether the plan can be spelled in C++: every table and query becomes a namespace inside
 *        the prefix's, so no two may share a name, nor take one the runtime uses.
 */
bool sg_lang_cpp_check(const sg_config *config, const sg_target *target, size_t index, const sg_plan *plan);

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

/**
 * @brief Odin's filters:
 *   - `odinname`     snake case, `_` after a word Odin has taken (keywords, built-in types);
 *   - `odinparam`    as `odinname`, also clear of the names generated procedures give their own
 *                    parameters and locals (`ctx`, `allocator`, `stmt`, `rc`, …);
 *   - `storage`      as for C;
 *   - `odintype`     a column's Odin type: its typed id, `i64`, `f64`, `string`, `[]u8` or `Value`;
 *   - `idtype`       a table's typed id type, `<Table>_<Key_Column>`;
 *   - `odinstring`   a text as one Odin string literal, every byte that is not printable ASCII as `\xHH`;
 *   - `odinstrings`  a text as literals, one per line, to join with `+` into one constant.
 */
size_t sg_lang_odin_filters(sg_filter out[SG_LANG_MAX_FILTERS], sg_lang_context *context);

/**
 * @brief Whether the plan can be spelled in Odin: one package holds every name, so a query's
 *        procedures and types must not be a table's or the runtime's; the prefix, the package's
 *        name, must not be a word Odin has taken.
 */
bool sg_lang_odin_check(const sg_config *config, const sg_target *target, size_t index, const sg_plan *plan);

/** @brief An output language: its filters, and what it checks of a target before rendering. */
typedef struct {
	const char *name;
	size_t    (*filters)(sg_filter out[SG_LANG_MAX_FILTERS], sg_lang_context *context);
	bool      (*check)(const sg_config *config, const sg_target *target, size_t index, const sg_plan *plan);
} sg_language;

/** @brief The language called @p name; NULL when sqlgen has none. */
const sg_language *sg_language_find(const char *name);

#endif /* SQLGEN_LANG_H */
