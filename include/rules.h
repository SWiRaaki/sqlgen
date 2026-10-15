#ifndef SQLGEN_RULES_H
#define SQLGEN_RULES_H

/**
 * @file rules.h
 * @brief Checks a schema model against the rules the configuration switches on.
 *
 * Every finding is printed like a compiler diagnostic and names its rule in brackets, so it says
 * what to switch off or reconfigure:
 *
 *     sql/0.1.0/sessions.sql:22:1: warning: sessions(user) references users, but no index of
 *     sessions starts with (user); deleting from users scans sessions [fk_indexed]
 *
 * A finding points at the statement that created its table. A model read from a live database has
 * no scripts, so its findings name the database file instead.
 *
 * Views and virtual tables are exempt from the rules about a table's own shape (`strict`,
 * `pk_shape`, `pk_naming`, `created_at`, `updated_at_needs_created_at`, `stamp_type`), and a
 * junction -- two integer columns, each a foreign key, unique together -- also from `pk_shape`,
 * `pk_naming` and `created_at`, since its pair is its identity. `prefix_origin` needs to know which
 * script created a table, so it does not apply to a live database.
 *
 * Independently of the rules, a `tables` entry naming no table in the schema is always an error:
 * it is a misspelling, and the options it sets would otherwise silently apply to nothing.
 */

#include <sqlite3.h>

#include "config.h"
#include "model.h"

typedef struct {
	sqlite3         *db;       //!< The database the model was read from; `fk_target` asks it.
	const sg_model  *model;
	const sg_config *config;
	const char      *source;   //!< Where findings without a script are reported; NULL for none.
	bool             strict;   //!< Report warnings as errors.
} sg_rules_input;

typedef struct {
	unsigned errors;
	unsigned warnings;
} sg_rules_result;

/**
 * @brief Run every rule that is on, print each finding, and count them.
 *
 * `fk_target` prepares `pragma foreign_key_check` per table, which is where SQLite reports a
 * foreign key whose parent columns are neither its primary key nor one of its unique keys
 * ("foreign key mismatch"); only preparing, so no table data is read. The model then names which
 * of the table's foreign keys it is.
 */
void sg_rules_run(const sg_rules_input *input, sg_rules_result *result);

#endif /* SQLGEN_RULES_H */
