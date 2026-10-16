#ifndef SQLGEN_SCHEMA_H
#define SQLGEN_SCHEMA_H

/**
 * @file schema.h
 * @brief Patch discovery, manifests, and applying the scripts to a scratch database.
 *
 * A patch is a directory holding a `patch.manifest`: the first line that is neither blank nor a
 * `#` comment is the patch's semantic version, and every further such line names a script in that
 * directory, in the order the scripts run. Patches run in order of version precedence, never in
 * directory order -- a glob puts 0.10.0 before 0.2.0.
 */

#include <sqlite3.h>

#include "config.h"
#include "model.h"
#include "semver.h"

typedef struct {
	char     *name;    //!< As the manifest lists it.
	char     *path;    //!< Joined to the patch directory.
	unsigned  line;    //!< Its line in the manifest.
} sg_script;

typedef struct {
	sg_semver           version;
	char               *dir;
	char               *manifest;
	SG_VEC(sg_script)   scripts;
	size_t              end;       //!< Index into the history: the schema once this patch has run.
} sg_patch;

/** @brief The schema as one statement left it. */
typedef struct {
	sg_origin  at;      //!< The statement; for the first snapshot, the system-tables script (line 0).
	sg_model   model;
} sg_snapshot;

/** @brief Records which script created a schema object, for @ref sg_origin. */
typedef struct {
	char       *type;      //!< "table", "view", "index" or "trigger".
	char       *name;
	sg_origin   origin;
	bool        seen;
} sg_object_origin;

typedef struct sg_patchset {
	char                       *systables;   //!< Borrowed from the config; NULL when there is none.
	SG_VEC(sg_patch)            patches;     //!< In precedence order.
	SG_VEC(sg_object_origin)    origins;     //!< Filled by @ref sg_schema_load.
	SG_VEC(sg_snapshot)         history;     //!< Filled by @ref sg_schema_load; see there.
	int                         version;     //!< The scratch database's schema_version, while loading.
} sg_patchset;

/**
 * @brief Find every patch the config names, read the manifests, and order the patches.
 * @return false when a pattern matches no patch, a manifest is malformed, a script it names is
 *         missing, or two patches have equal precedence. Every problem is reported.
 */
bool sg_patches_find(const sg_config *config, sg_patchset *out);

/**
 * @brief Apply the system tables and then every patch to @p db, recording origins.
 *
 * Each script runs inside its own transaction, statement by statement. A statement that fails is
 * reported at its `file:line:col`; a script that ends the transaction it runs in (a COMMIT, END or
 * ROLLBACK of its own) is refused, because a migration runner applying it the same way would lose
 * its atomicity. Loading stops at the first failing script, since later ones build on it.
 *
 * The schema's history is kept as it goes: a snapshot of the model once the system tables exist
 * (the empty schema when there are none), and another after every statement of a patch that
 * changes the schema, which `pragma schema_version` tells. Each patch records the snapshot it
 * ends on. Comparing two of them, with the steps between, is what says how a patch changed the
 * schema and which statement did it.
 *
 * With foreign keys enabled, `pragma foreign_key_check` runs once everything is loaded, so seed
 * rows that reference nothing are found here rather than at a deployment.
 *
 * Two rules come from the migration the generated code performs. Names starting with `_sys_` are
 * reserved for the tables it keeps (`_sys_patch`, `_sys_script`), so a script creating such an
 * object is refused at that statement. And the system-tables script runs before every migration,
 * so it is run a second time here and refused when that fails or changes the schema.
 */
bool sg_schema_load(sqlite3 *db, const sg_config *config, sg_patchset *patches);

/** @brief Attach the recorded origins to the model's tables and indexes. */
void sg_schema_attach_origins(const sg_patchset *patches, sg_model *model);

/** @brief Release everything. */
void sg_patchset_free(sg_patchset *patches);

#endif /* SQLGEN_SCHEMA_H */
