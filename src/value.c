#include "value.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include <sqlite3.h>

static sg_value *make(sg_vkind kind) {
	sg_value *value = sg_xmalloc(sizeof *value);

	memset(value, 0, sizeof *value);
	value->kind = kind;
	return value;
}

sg_value *sg_value_null(void) {
	return make(SG_V_NULL);
}

sg_value *sg_value_bool(bool flag) {
	sg_value *value = make(SG_V_BOOL);

	value->boolean = flag;
	return value;
}

sg_value *sg_value_int(long long number) {
	sg_value *value = make(SG_V_INT);

	value->integer = number;
	return value;
}

sg_value *sg_value_text(const char *text) {
	return text ? sg_value_text_owned(sg_xstrdup(text)) : sg_value_null();
}

sg_value *sg_value_text_owned(char *text) {
	sg_value *value = make(SG_V_TEXT);

	value->text = text;
	return value;
}

sg_value *sg_value_list(void) {
	return make(SG_V_LIST);
}

sg_value *sg_value_map(void) {
	return make(SG_V_MAP);
}

sg_value *sg_list_push(sg_value *list, sg_value *item) {
	*SG_PUSH(list->items) = item;
	return item;
}

sg_value *sg_map_set(sg_value *map, const char *key, sg_value *value) {
	for (size_t i = 0; i < map->members.count; ++i) {
		if (strcmp(map->members.items[i].key, key) == 0) {
			sg_value_free(map->members.items[i].value);
			map->members.items[i].value = value;
			return value;
		}
	}

	sg_member *member = SG_PUSH(map->members);

	member->key   = sg_xstrdup(key);
	member->value = value;
	return value;
}

sg_value *sg_map_get(const sg_value *map, const char *key) {
	if (!map || map->kind != SG_V_MAP) {
		return NULL;
	}

	for (size_t i = 0; i < map->members.count; ++i) {
		if (strcmp(map->members.items[i].key, key) == 0) {
			return map->members.items[i].value;
		}
	}

	return NULL;
}

sg_value *sg_value_copy(const sg_value *value) {
	sg_value *copy = make(value->kind);

	copy->boolean = value->boolean;
	copy->integer = value->integer;
	copy->text    = sg_xstrdup(value->text);

	for (size_t i = 0; i < value->items.count; ++i) {
		sg_list_push(copy, sg_value_copy(value->items.items[i]));
	}

	for (size_t i = 0; i < value->members.count; ++i) {
		sg_map_set(copy, value->members.items[i].key, sg_value_copy(value->members.items[i].value));
	}

	return copy;
}

bool sg_value_truthy(const sg_value *value) {
	if (!value) {
		return false;
	}

	switch (value->kind) {
	case SG_V_NULL: return false;
	case SG_V_BOOL: return value->boolean;
	case SG_V_INT:  return value->integer != 0;
	case SG_V_TEXT: return value->text[0] != '\0';
	case SG_V_LIST: return value->items.count > 0;
	case SG_V_MAP:  return value->members.count > 0;
	}

	return false;
}

bool sg_value_equal(const sg_value *left, const sg_value *right) {
	if (!left || !right || left->kind != right->kind) {
		return false;
	}

	switch (left->kind) {
	case SG_V_NULL: return true;
	case SG_V_BOOL: return left->boolean == right->boolean;
	case SG_V_INT:  return left->integer == right->integer;
	case SG_V_TEXT: return strcmp(left->text, right->text) == 0;
	case SG_V_LIST:
	case SG_V_MAP:  return false;
	}

	return false;
}

const char *sg_vkind_name(sg_vkind kind) {
	switch (kind) {
	case SG_V_NULL: return "null";
	case SG_V_BOOL: return "a boolean";
	case SG_V_INT:  return "an integer";
	case SG_V_TEXT: return "text";
	case SG_V_LIST: return "a list";
	case SG_V_MAP:  return "a map";
	}

	return "null";
}

void sg_value_free(sg_value *value) {
	if (!value) {
		return;
	}

	free(value->text);

	for (size_t i = 0; i < value->items.count; ++i) {
		sg_value_free(value->items.items[i]);
	}

	free(value->items.items);

	for (size_t i = 0; i < value->members.count; ++i) {
		free(value->members.items[i].key);
		sg_value_free(value->members.items[i].value);
	}

	free(value->members.items);
	free(value);
}

typedef struct {
	sqlite3_int64  id;
	sg_value      *value;
} node_id;

/* json_tree lists every node after its parent, so nodes can be attached as they arrive. Their ids
 * only grow, so a parent is found by binary search. */
static sg_value *find_node(const node_id *nodes, size_t count, sqlite3_int64 id) {
	size_t lo = 0;
	size_t hi = count;

	while (lo < hi) {
		size_t mid = (lo + hi) / 2;

		if (nodes[mid].id == id) {
			return nodes[mid].value;
		}

		if (nodes[mid].id < id) {
			lo = mid + 1;
		} else {
			hi = mid;
		}
	}

	return NULL;
}

sg_value *sg_value_from_json(const char *json, size_t len) {
	sqlite3      *db   = NULL;
	sqlite3_stmt *stmt = NULL;
	sg_value     *root = NULL;
	SG_VEC(node_id) nodes = {0};

	if (len > INT_MAX || sqlite3_open_v2(":memory:", &db, SQLITE_OPEN_READWRITE, NULL) != SQLITE_OK
			|| sqlite3_prepare_v2(db, "select id, parent, key, type, atom from json_tree(?1) order by id",
					-1, &stmt, NULL) != SQLITE_OK) {
		sqlite3_close(db);
		return NULL;
	}

	sqlite3_bind_text(stmt, 1, json, (int)len, SQLITE_STATIC);

	while (sqlite3_step(stmt) == SQLITE_ROW) {
		const char *type = (const char *)sqlite3_column_text(stmt, 3);
		const char *atom = (const char *)sqlite3_column_text(stmt, 4);
		sg_value   *v;

		if (strcmp(type, "object") == 0) {
			v = sg_value_map();
		} else if (strcmp(type, "array") == 0) {
			v = sg_value_list();
		} else if (strcmp(type, "integer") == 0) {
			v = sg_value_int(sqlite3_column_int64(stmt, 4));
		} else if (strcmp(type, "true") == 0 || strcmp(type, "false") == 0) {
			v = sg_value_bool(type[0] == 't');
		} else if (strcmp(type, "null") == 0) {
			v = sg_value_null();
		} else {
			v = sg_value_text(atom ? atom : "");
		}

		if (sqlite3_column_type(stmt, 1) == SQLITE_NULL) {
			root = v;
		} else {
			sg_value *parent = find_node(nodes.items, nodes.count, sqlite3_column_int64(stmt, 1));

			if (!parent) {
				sg_value_free(v);
				continue;
			}

			if (parent->kind == SG_V_MAP) {
				sg_map_set(parent, (const char *)sqlite3_column_text(stmt, 2), v);
			} else {
				sg_list_push(parent, v);
			}
		}

		node_id *n = SG_PUSH(nodes);

		n->id    = sqlite3_column_int64(stmt, 0);
		n->value = v;
	}

	sqlite3_finalize(stmt);
	sqlite3_close(db);
	free(nodes.items);
	return root;
}
