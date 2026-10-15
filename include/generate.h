#ifndef SQLGEN_GENERATE_H
#define SQLGEN_GENERATE_H

/**
 * @file generate.h
 * @brief Rendering the plan through each target's templates, and writing the result.
 *
 * Every target is rendered in memory before any directory is touched, so a template error leaves
 * every output directory as it was. Then each target's directory is cleared and refilled. A
 * directory that is not empty is only cleared after a confirmation: a prompt on a terminal, or
 * `--yes`; a target with `confirm_clear: false` never asks. Without a terminal and without `--yes`,
 * nothing is generated.
 *
 * The templates see the plan, exactly as `sqlgen plan` prints it, and beside it:
 *
 *     target        the target: language, dir, prefix, types
 *     prefix        the target's prefix
 *     generator     name and version of sqlgen
 *     generated_at  "YYYY-MM-DD HH:MM:SS UTC": now, or SOURCE_DATE_EPOCH when it is set, so a
 *                   build can make the output reproducible
 *     command       the command that regenerates the files
 */

#include "config.h"
#include "plan.h"

typedef struct {
	const char *config_arg;   //!< The configuration as the command line named it, for `command`.
	bool        yes;          //!< Clear directories without asking.
} sg_generate_options;

/**
 * @brief Generate every target.
 * @return false, with every problem reported, when anything could not be generated or written.
 */
bool sg_generate(const sg_config *config, const sg_plan *plan, const sg_generate_options *options);

/**
 * @brief Write a language's built-in templates into @p dir, to be copied and changed.
 * @return false, with the problem reported, when the language has none, or a file already exists.
 */
bool sg_export_templates(const char *language, const char *dir);

#endif /* SQLGEN_GENERATE_H */
