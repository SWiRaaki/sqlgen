#ifndef SQLGEN_TEMPLATE_H
#define SQLGEN_TEMPLATE_H

/**
 * @file template.h
 * @brief The template language: a set of templates rendered against a tree of values.
 *
 * ### Syntax
 * Everything is plain output except what starts with the *sigil*, `\` by default:
 *
 *     \(expr)                  the value of an expression
 *     \for (x in expr) { … \}  once per item of a list; `loop.index` (from 0), `loop.number`
 *                              (from 1), `loop.first`, `loop.last`, `loop.count`
 *     \if (expr) { … \}        with any number of `\elif (expr) { … \}` and one `\else { … \}`
 *     \align { … \}            pads the columns its lines mark with `\|` to their widest cell
 *     \define name(a, b) { … \}   a snippet; `\call name(x, y)` renders it
 *     \include("name")         renders another template of the set here
 *     \* … *\                  a comment; never output
 *
 * Blocks end only at the sigil followed by the block's closing bracket (`\}`), so brackets in the
 * output never end a block. Keywords are whole words: `\format` is plain text. After the sigil,
 * anything that is none of these forms is output unchanged, so `"\n"` needs no escaping; output
 * that must contain one of the forms literally can come from a string: `\("\\(")`.
 *
 * A line holding nothing but block tags, comments, includes and calls (and whitespace) produces no
 * output line, so a template can put each tag on a line of its own without leaving blank lines.
 *
 * Expressions: paths (`t.name`, `t.columns[0].name`), strings in double quotes (`\"`, `\\`, `\n`,
 * `\t` escaped), integers, `true`, `false`, `null`, `==`, `!=`, `<`, `<=`, `>`, `>=` (integers only),
 * `not`, `and`, `or`, parentheses,
 * and filters: `expr | name` or `expr | name(arg, …)`. A path naming no member is an error, not
 * an empty value. The built-in filters are `snake`, `pascal`, `camel`, `upper` (all taking a list
 * of words or a text, which is split into words), `lower`, `join(separator)`, `count`, `first`,
 * `last`, `at(index)` and `default(value)`; a caller adds its own.
 *
 * ### Header
 * A template may start with a header between a line `<<<` and a line `>>>`, one `key: value` per
 * line:
 *
 *     sigil:  \          the character starting every form
 *     expr:   ( )        the brackets around an expression
 *     block:  { }        the brackets around a block
 *     output: \(prefix)_\(t.name).h    the file this template produces
 *     each:   t in tables              produce one file per item, the item named t
 *     when:   embed_scripts            produce the file only when the expression is true
 *
 * A template with `output` produces files; one without is only used through `\include` and
 * `\define`. Every error names the template's file, line and column.
 */

#include "util.h"
#include "value.h"

typedef struct sg_template_set sg_template_set;

typedef struct {
	const char *name;    //!< Its name within the set, as `\include` names it.
	const char *text;
} sg_template_source;

/**
 * @brief A filter: returns a new value, or NULL after setting @p error to a message from malloc.
 */
typedef sg_value *(*sg_filter_fn)(const sg_value *input, sg_value *const *args, size_t count,
		void *user, char **error);

typedef struct {
	const char   *name;
	sg_filter_fn  fn;
	void         *user;
} sg_filter;

typedef struct {
	char   *path;    //!< Relative to the output directory.
	sg_buf  text;
} sg_rendered_file;

typedef SG_VEC(sg_rendered_file) sg_rendered;

/**
 * @brief Parse a whole set, so `\define` and `\include` work across its templates.
 * @param origin Put before each template's name in messages: its directory, or a label.
 * @return NULL, with every problem reported, when any template does not parse.
 */
sg_template_set *sg_templates_parse(const sg_template_source *sources, size_t count, const char *origin);

/**
 * @brief Render every template that has an `output`, against @p data (a map).
 * @return false, with the problem reported, on the first error; @p out then holds nothing.
 */
bool sg_templates_render(const sg_template_set *set, const sg_value *data, const sg_filter *filters,
		size_t filter_count, sg_rendered *out);

void sg_rendered_free(sg_rendered *files);
void sg_templates_free(sg_template_set *set);

#endif /* SQLGEN_TEMPLATE_H */
