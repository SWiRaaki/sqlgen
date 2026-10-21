/* Must not compile for WRONG 1, 2 or 3 -- the types keep apart what does not belong together --
 * and must compile for WRONG 0, so a refusal is the case's own and not a broken header. */
#include "db.hpp"

#if WRONG == 1
/* A tag's id where a user's id is expected. */
int wrong(db::Context &ctx, db::users::Row &out) {
	return db::users::get_by_key(ctx, db::TagId{ 1 }, out);
}
#elif WRONG == 2
/* An id read as its number without saying so. */
uint64_t wrong(db::UsersSerial id) {
	uint64_t n = id;
	return n;
}
#elif WRONG == 3
/* A cursor of users opened over sessions. */
int wrong(db::Context &ctx, db::UsersSerial id, db::Cursor<db::users::Row> &out) {
	return db::sessions::list_by_user(ctx, id, out);
}
#endif
