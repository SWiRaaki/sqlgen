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

/**
 * @brief One rule, with its level and parameters after the defaults and the file are applied.
 *
 * Every parameter the rule knows is present in @ref params, in the rule's own order, holding the
 * file's value or the default. A value is NULL only for a parameter without a default that the file
 * left unset or set to `null`; such a parameter is required, and the configuration is refused when
 * the rule is on without it.
 */
typedef struct {
	const char  *name;     //!< Static: one of the names sqlgen knows.
	sg_level     level;
	sg_pair_vec  params;
} sg_rule_config;

/**
 * @brief One entry of `targets`: a language, where its files go, and how it spells the plan.
 *
 * Which keys a target takes depends on its language: every target has `language`, `dir`,
 * `templates` and `confirm_clear`; C also `prefix` and `types`; C++ `prefix`; Odin `prefix` and
 * `link`; Markdown and Mermaid nothing more.
 */
typedef struct {
	char        *language;       //!< "c", "cpp", "odin", "markdown" or "mermaid".
	char        *dir;            //!< Where its files go; cleared on every generate.
	char        *prefix;         //!< C: put before every generated name; C++: the namespace; Odin: the package.
	char        *link;           //!< Odin: what the generated code links SQLite with (`foreign import`).
	char        *templates;      //!< A directory of templates replacing the built-in ones; NULL for those.
	bool         confirm_clear;  //!< Ask before clearing a directory that is not empty.
	sg_pair_vec  types;          //!< Logical type name -> the type this language spells it as.
} sg_target;

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

	bool  embed_scripts;              //!< Generate the scripts into the output, for migrating at run time.
	bool  typed_ids;                  //!< One distinct type per primary key, used by its foreign keys.
	SG_VEC(sg_target) targets;        //!< Absent from the file: one target per language, with every default.

	sg_pair_vec types_by_name;        //!< Column-name glob -> logical type name.
	sg_pair_vec types_by_declared;    //!< Declared type -> logical type name; wins over by_name.
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
 * @brief Read and validate a configuration given as text.
 * @param name Names it in messages; relative paths in it are resolved against its directory part.
 */
bool sg_config_load_text(const char *name, const char *text, size_t len, sg_config *out);

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

/** @brief A rule's parameter; NULL when it is unset or the rule has no such parameter. */
const char *sg_rule_param(const sg_rule_config *rule, const char *name);

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
