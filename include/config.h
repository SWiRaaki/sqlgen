#ifndef SQLGEN_CONFIG_H
#define SQLGEN_CONFIG_H

/**
 * @file config.h
 * @brief The configuration file: read, validated and resolved.
 *
 * The file is strict JSON (RFC 8259), so any other tool can read it as well: @ref sg_json_check
 * refuses comments, unquoted keys, trailing commas and the rest of JSON5 with a position and a
 * reason, and only then do SQLite's JSON functions read the values. Every key is checked against
 * the keys sqlgen knows, and an unknown one is an error, so a misspelled option cannot be silently
 * ignored.
 *
 * Relative paths in the file are resolved against the directory holding it, so a run from
 * anywhere means the same thing.
 *
 * Every key has a default, so `{}` is a complete configuration. The defaults live once, in
 * config.c, and @ref sg_config_default_text spells out the same values; test/config_test.c checks
 * that the two agree.
 */

#include <stdbool.h>

#include "util.h"

/**
 * @brief The write functions a table opts into through `tables.<name>.writes`.
 */
enum {
	SG_WRITE_INSERT = 1 << 0,
	SG_WRITE_UPDATE = 1 << 1,
	SG_WRITE_DELETE = 1 << 2,
	SG_WRITE_UPSERT = 1 << 3,
	SG_WRITE_LINK   = 1 << 4,
	SG_WRITE_ALL    = (1 << 5) - 1
};

/** @brief A key mapped to a value, for the type maps. */
typedef struct {
	char *key;
	char *value;
} sg_pair;

typedef SG_VEC(sg_pair) sg_pair_vec;
typedef SG_VEC(char *)  sg_str_vec;

/**
 * @brief The options one table can set. Each `has_` flag says whether the entry set that key, which
 *        is what lets a named entry replace `"*"` key by key rather than wholesale.
 */
typedef struct {
	bool      has_writes;
	unsigned  writes;     //!< SG_WRITE_* flags.
} sg_table_options;

/** @brief One entry of `tables`. */
typedef struct {
	char             *name;      //!< A table name, or "*" for every table.
	sg_table_options  options;
} sg_table_config;

typedef enum {
	SG_LEVEL_OFF,
	SG_LEVEL_WARN,
	SG_LEVEL_ERROR
} sg_level;

/** @brief One rule, with its level after the defaults and the file are applied. */
typedef struct {
	const char *name;     //!< Static: one of the names sqlgen knows.
	sg_level    level;
	char       *params;   //!< The rule's object as JSON when the file gave one, else NULL.
} sg_rule_config;

/**
 * @brief The resolved configuration. Paths are already joined to the config file's directory.
 */
typedef struct {
	char *path;                       //!< The config file itself.
	char *dir;                        //!< Its directory.

	char *systables;                  //!< Script creating the system tables; NULL when there is none
	                                  //!< (absent or `null` in the file).
	sg_str_vec patch_patterns;        //!< Glob patterns naming patch directories.
	bool  double_quoted_strings;      //!< Accept "text" as a string literal, as legacy SQLite does.
	bool  foreign_keys;               //!< Enforce foreign keys while loading (seed rows are checked).

	char *output_dir;
	char *prefix;                     //!< Prefix for every generated identifier.
	bool  embed_scripts;
	bool  typed_ids;

	sg_pair_vec types_by_name;        //!< Column-name pattern -> output type.
	sg_pair_vec types_by_declared;    //!< Declared type -> output type.
	SG_VEC(sg_table_config) tables;   //!< As written; resolve with @ref sg_config_table.
	SG_VEC(sg_rule_config) rules;     //!< Every rule sqlgen knows, in a fixed order.
	sg_str_vec queries;               //!< Glob patterns naming query files.
} sg_config;

/**
 * @brief Read and validate the configuration at @p path.
 * @return false when it cannot be used; every problem found has been reported.
 */
bool sg_config_load(const char *path, sg_config *out);

/**
 * @brief The options in effect for @p table: the defaults, then the `"*"` entry's keys, then the
 *        keys of the entry naming @p table (ASCII case-insensitively, as SQLite names match).
 *
 * A named entry replaces `"*"` key by key: `"users": { "writes": ["update"] }` gives users only
 * update, whatever `"*"` says about writes, while any other key `"*"` sets still applies to users.
 */
sg_table_options sg_config_table(const sg_config *config, const char *table);

/** @brief Find a rule by name; NULL when sqlgen knows no such rule. */
const sg_rule_config *sg_config_rule(const sg_config *config, const char *name);

/**
 * @brief A configuration file stating every key with its default, as strict JSON.
 *
 * What `sqlgen init` writes, and what is written when no configuration was named and none exists.
 */
const char *sg_config_default_text(void);

/** @brief The file name looked for when none is given. */
#define SG_CONFIG_DEFAULT_PATH "sqlgen.json"

/** @brief Release everything the configuration holds. */
void sg_config_free(sg_config *config);

#endif /* SQLGEN_CONFIG_H */
