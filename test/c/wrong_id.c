/* Must not compile: a tag's id where a user's id is expected. */
#include "db.h"

int wrong_id(db_ctx *ctx, db_users_row *out);

int wrong_id(db_ctx *ctx, db_users_row *out) {
	return db_users_get_by_key(ctx, DB_TAG_ID(1), out);
}
