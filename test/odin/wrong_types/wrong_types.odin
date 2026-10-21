/*
 * Must not compile for WRONG 1, 2 or 3 -- the types keep apart what does not belong together --
 * and must compile for WRONG 0, so a refusal is the case's own and not a broken package.
 */
package wrong_types

import db "../../cases/generate-odin/gen"

WRONG :: #config(WRONG, 0)

main :: proc() {
	when WRONG == 1 {
		/* A tag's id where a user's id is expected. */
		ctx: db.Context
		_, _ = db.users_get_by_key(&ctx, db.Tag_Id(1))
	} else when WRONG == 2 {
		/* An id read as its number without saying so. */
		id := db.Users_Serial(1)
		n: u64 = id
		_ = n
	} else when WRONG == 3 {
		/* A cursor of users read into a sessions row. */
		ctx: db.Context
		users, _ := db.users_list_by_role(&ctx, "admin")
		s: db.Sessions_Row
		s, _ = db.cursor_next(&users)
		_ = s
	} else {
		ctx: db.Context
		_, _ = db.users_get_by_key(&ctx, db.Users_Serial(1))
	}
}
