# Templates

sqlgen turns a schema into source code by rendering templates. Each language has a directory of
templates here (`c/`), compiled into sqlgen.

To change the output, export the built-in set, edit it, and point a target at your copy:

```sh
sqlgen templates c my-templates
```

```json
"targets": [{ "language": "c", "dir": "gen", "templates": "my-templates" }]
```

A set is every `*.gtpl` file in its directory. A template with an `output` in its header produces
files; one without is only used through `\include` and `\define`.

## Syntax

Everything is output as written, except what starts with `\`:

| Form | Meaning |
|---|---|
| `\(expr)` | the value of an expression |
| `\for (x in list) { … \}` | the block once per item |
| `\if (expr) { … \}` | the block when the expression is true |
| `\elif (expr) { … \}` | follows an `\if` or `\elif` |
| `\else { … \}` | follows an `\if` or `\elif` |
| `\align { … \}` | lines with `\|` marks, padded into columns |
| `\define name(a, b) { … \}` | a reusable snippet |
| `\call name(x, y)` | renders a snippet |
| `\include("name")` | renders another template of the set here |
| `\* … *\` | a comment |

A block ends only at `\}`. Braces in the output never end a block:

```
\for (c in t.columns) {
	if (x) { y(); }
\}
```

After `\`, anything that is none of these forms is plain output, so `"\n"`, `'\0'` and macro line
continuations need no escaping. Keywords are whole words: `\format` is plain text.

To output one of the forms literally, write it as a string: `\("\\(")` gives `\(`.

### Blank lines

A line holding only tags (`\for`, `\if`, `\}`, `\define`, `\call`, `\include`, comments) and
whitespace produces nothing, not even its line break. So this

```
struct row {
\for (c in t.columns) {
	int \(c.name);
\}
};
```

gives

```c
struct row {
	int id;
	int name;
};
```

A line with any output on it keeps its line break.

### Expressions

| Form | Example |
|---|---|
| path | `t.name`, `t.columns[0].name` |
| string | `"text"`, with `\"`, `\\`, `\n`, `\t` |
| integer | `42`, `-1` |
| constants | `true`, `false`, `null` |
| comparison | `==`, `!=`, and for integers `<`, `<=`, `>`, `>=` |
| logic | `not`, `and`, `or`, parentheses |
| filter | `t.words \| snake`, `xs \| join(", ")` |

A filter binds tighter than a comparison: `c | storage == "text"` compares the filter's result.

Conditions treat `null`, `false`, `0`, `""`, empty lists and empty maps as false, everything else
as true.

### Loops

Inside `\for`, `loop` describes the position:

| Name | Value |
|---|---|
| `loop.index` | 0, 1, 2, … |
| `loop.number` | 1, 2, 3, … |
| `loop.first` | true for the first item |
| `loop.last` | true for the last item |
| `loop.count` | the number of items |

```
(\for (c in t.columns) {\(c.name)\if (not loop.last) {, \}\})
```

gives `(id, name, email)`.

An inner loop's `loop` hides the outer one. To keep an outer index, pass it to a snippet:
`\call body(t, loop.index)`.

### Alignment

In an `\align` block, `\|` marks a column break. Each column is padded to its widest cell, plus one
space. Lines without marks pass unchanged.

```
\align {
\for (c in t.columns) {
	\(c | ctype)\|\(c.name);
\}
\}
```

gives

```c
	db_users_serial serial;
	db_text         username;
	int64_t         created_at;
```

### Snippets

`\define` creates a snippet for the whole set; it outputs nothing where it stands. `\call`
renders it with its parameters bound. Snippets see the names of the place they are called from.

```
\define field(c) {
	\(c | ctype) \(c.words | cname);
\}
\for (c in t.columns) {
\call field(c)
\}
```

A `\call` alone on its line vanishes like any tag, so a snippet meant as whole lines ends its
lines itself, as above. A snippet called inside a line must fit on one line.

## Header

A template may start with a header:

```
<<<
	output: \(prefix)_\(t.words | snake).h
	each:   t in tables
>>>
```

| Key | Meaning |
|---|---|
| `output` | the file to produce, relative to the target directory |
| `each` | one file per item: `name in list` |
| `when` | produce the file only when the expression is true |
| `sigil` | the character starting every form; default `\` |
| `expr` | the brackets around expressions; default `( )` |
| `block` | the brackets around blocks; default `{ }` |

`sigil`, `expr` and `block` exist for output languages that use `\` themselves:

```
<<<
	sigil:  @
	output: names.txt
>>>
@for (t in tables) {@(t.name)
@}
```

## Data

Templates see the generation plan. `sqlgen plan` prints it as JSON, so every name is visible
there. Beside it:

| Name | Value |
|---|---|
| `target` | the target: `language`, `dir`, `prefix`, `types` |
| `prefix` | the target's prefix |
| `generator` | `name` and `version` of sqlgen |
| `generated_at` | `YYYY-MM-DD HH:MM:SS UTC`; `SOURCE_DATE_EPOCH` sets it |
| `command` | the command that regenerates the files |

The plan:

| Name | Value |
|---|---|
| `hash` | a hash of every script; it changes whenever the schema does |
| `embed_scripts`, `typed_ids` | the `generate` settings |
| `custom_types` | the logical types from `types` that columns and queries use |
| `tables` | every table and view |
| `queries` | every query file, by file name |
| `scripts.systables` | the system tables script (`name`, `path`, `text`), or `null` |
| `scripts.patches` | each patch: `version`, `major`, `minor`, `patch`, `prerelease`, `build`, `scripts` |

A table:

| Name | Value |
|---|---|
| `name`, `sql_name` | as written, and as SQL needs it (quoted if necessary) |
| `words` | the name split into lowercase words: `["user", "tag"]` |
| `kind` | `table`, `view` or `virtual` |
| `without_rowid`, `junction` | true or false |
| `origin` | `script`, `line` and `patch` of its `create` statement |
| `id_column` | the column holding its typed id, or `null` |
| `select_list` | every column, comma-separated, ready for SQL |
| `columns` | see below |
| `statements` | see below |

A column:

| Name | Value |
|---|---|
| `name`, `sql_name`, `words` | as for tables |
| `type` | its logical type: the affinity, or a name from `types` |
| `custom` | true when `type` came from `types` |
| `affinity` | `integer`, `real`, `text`, `blob`, `numeric` or `any` |
| `nullable`, `writable`, `rowid` | true or false |
| `id_of` | the table whose typed id it holds, or `null` |

A statement:

| Name | Value |
|---|---|
| `kind` | `get`, `list`, `insert`, `update`, `delete`, `upsert`, `link` or `unlink` |
| `words` | its name: `["get", "by", "username"]` |
| `sql` | the SQL, with placeholders `?1`, `?2`, … |
| `params` | in placeholder order: `column`, `index` (into `columns`), `optional` |
| `key` | the columns it selects by |
| `returning` | the key columns an insert returns, for WITHOUT ROWID tables |

A query:

| Name | Value |
|---|---|
| `name`, `words` | from the file name: `user_sessions.sql` is `user_sessions` |
| `path` | the file |
| `doc` | its leading `--` comment lines that are no annotation, as a list of lines |
| `sql` | the statement as written, without its `;` |
| `writes` | true when it changes the database |
| `rows` | `none`, `one` (at most), `exactly_one` or `many` |
| `row_table` | the table whose columns it selects exactly, so its row type is that table's; or `null` |
| `row_words` | the words naming its row type: that table's, or its own |
| `params` | each parameter: `index`, its 1-based placeholder number, and the fields of a column |
| `columns` | each result column, with the fields of a column |

A parameter or result column has `name`, `words`, `type`, `custom`, `affinity`, `nullable` and
`id_of`, meaning what they mean for a column.

## Filters

For every language:

| Filter | Result |
|---|---|
| `snake` | `token_hash` |
| `pascal` | `TokenHash` |
| `camel` | `tokenHash` |
| `upper` | `TOKEN_HASH` |
| `lower` | the text in lowercase |
| `join(sep)` | the items of a list, joined |
| `count` | the length of a list, map or text |
| `first`, `last` | the first or last item of a list |
| `at(i)` | item `i` of a list |
| `default(x)` | `x` when the value is `null` |

The case filters take a text or a list of words; a text is split into words first, so `tokenHash`,
`token_hash` and `Token Hash` all give the same result.

For C:

| Filter | Result |
|---|---|
| `cname` | snake case, with `_` added to C keywords and before a leading digit |
| `storage` | how a column is read and bound: `id`, `int`, `real`, `text`, `blob` or `value` |
| `ctype` | a column's C type |
| `idtype` | a table's typed id type |
| `cstring` | a C string literal, quotes included |
| `cstrings` | a list of string literals, one per line of the text |
| `cbytes` | lines of a byte array initialiser, for text longer than 4095 bytes |

## Errors

Every error names the template, line and column:

```
my-templates/table.h.gtpl:12:9: error: t has no member 'nmae'
```

A path naming no member is an error, not an empty value. So is printing `null`: test it with
`\if`, or give it a `| default(...)`.
