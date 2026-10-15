#ifndef SQLGEN_VALUE_H
#define SQLGEN_VALUE_H

/**
 * @file value.h
 * @brief The data templates read: a tree of null, booleans, integers, text, lists and maps.
 *
 * A tree owns everything below it; @ref sg_value_free releases a whole tree. Maps keep their keys
 * in insertion order, so anything iterating one does so in a stable order.
 */

#include <stdbool.h>
#include <stddef.h>

#include "util.h"

typedef enum {
	SG_V_NULL,
	SG_V_BOOL,
	SG_V_INT,
	SG_V_TEXT,
	SG_V_LIST,
	SG_V_MAP
} sg_vkind;

typedef struct sg_value sg_value;

typedef struct {
	char     *key;
	sg_value *value;
} sg_member;

struct sg_value {
	sg_vkind                kind;
	bool                    boolean;
	long long               integer;
	char                   *text;
	SG_VEC(sg_value *)      items;     //!< SG_V_LIST.
	SG_VEC(sg_member)       members;   //!< SG_V_MAP.
};

sg_value *sg_value_null(void);
sg_value *sg_value_bool(bool value);
sg_value *sg_value_int(long long value);

/** @brief A text value holding a copy of @p text; NULL gives null. */
sg_value *sg_value_text(const char *text);

/** @brief A text value taking ownership of @p text, which must come from malloc. */
sg_value *sg_value_text_owned(char *text);

sg_value *sg_value_list(void);
sg_value *sg_value_map(void);

/** @brief Append to a list, taking ownership of @p item; returns @p item. */
sg_value *sg_list_push(sg_value *list, sg_value *item);

/** @brief Set a member of a map, taking ownership of @p value; returns @p value. A key already
 *         present is replaced in place, keeping its position. */
sg_value *sg_map_set(sg_value *map, const char *key, sg_value *value);

/** @brief A member of a map, or NULL when it has none of that name (or is no map). */
sg_value *sg_map_get(const sg_value *map, const char *key);

/** @brief A deep copy. */
sg_value *sg_value_copy(const sg_value *value);

/**
 * @brief Whether a value counts as true in a condition: everything but null, false, 0, empty text,
 *        an empty list and an empty map.
 */
bool sg_value_truthy(const sg_value *value);

/** @brief Equality of two scalars by kind and content; lists and maps are never equal. */
bool sg_value_equal(const sg_value *left, const sg_value *right);

/**
 * @brief Build a tree from JSON text, through SQLite's json_tree. A JSON number with a fraction
 *        becomes text, since nothing a template prints needs arithmetic on it.
 * @return NULL when the text is not JSON.
 */
sg_value *sg_value_from_json(const char *json, size_t len);

/** @brief The name of a kind, for messages. */
const char *sg_vkind_name(sg_vkind kind);

void sg_value_free(sg_value *value);

#endif /* SQLGEN_VALUE_H */
