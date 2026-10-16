#include "template.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { MAX_NESTING = 32 };

/* The byte an `\|` leaves in an align block's output until the block pads its columns. */
#define MARK_BYTE '\x1f'

typedef struct {
	const char *file;
	unsigned    line;
	unsigned    col;
} pos;

/* ---- expressions ------------------------------------------------------------------------- */

typedef enum {
	E_PATH,
	E_STRING,
	E_INT,
	E_BOOL,
	E_NULL,
	E_FILTER,
	E_NOT,
	E_AND,
	E_OR,
	E_EQ,
	E_NE,
	E_LT,
	E_LE,
	E_GT,
	E_GE
} ekind;

typedef struct {
	char      *name;     /* a member name, or NULL for an index */
	long long  index;
} segment;

typedef struct expr expr;
typedef SG_VEC(expr *) exprs;

struct expr {
	ekind            kind;
	pos              at;
	char            *text;     /* E_STRING's value; E_FILTER's name */
	long long        integer;
	bool             boolean;
	SG_VEC(segment)  path;     /* E_PATH: the first segment is the name looked up */
	expr            *left;     /* E_FILTER's input, E_NOT's operand, a binary's left side */
	expr            *right;
	exprs            args;     /* E_FILTER */
};

/* ---- nodes ------------------------------------------------------------------------------- */

typedef enum {
	N_TEXT,
	N_EXPR,
	N_FOR,
	N_IF,
	N_ALIGN,
	N_CALL,
	N_INCLUDE,
	N_MARK
} nkind;

typedef struct node node;
typedef SG_VEC(node *) nodes;

typedef struct {
	expr  *cond;   /* NULL for the else branch */
	nodes  body;
} branch;

struct node {
	nkind            kind;
	pos              at;
	const char      *text;     /* N_TEXT: points into the template's source */
	size_t           len;
	expr            *value;    /* N_EXPR; N_FOR's list */
	char            *name;     /* N_FOR's variable; N_CALL's macro; N_INCLUDE's template */
	nodes            body;     /* N_FOR, N_ALIGN */
	SG_VEC(branch)   branches; /* N_IF */
	exprs            args;     /* N_CALL */
};

typedef SG_VEC(char *) strings;

typedef struct {
	char     *name;
	strings   params;
	nodes     body;
	pos       at;
} macro;

typedef struct {
	char             *name;
	char             *file;     /* for messages */
	char             *source;
	SG_VEC(size_t)    lines;    /* offset of every line's start */
	char              sigil;
	char              e_open;
	char              e_close;
	char              b_open;
	char              b_close;
	nodes             body;
	bool              has_output;
	nodes             output;
	char             *each_var;
	expr             *each;
	expr             *when;     /* produce the file only when this is true */
} tmpl;

struct sg_template_set {
	SG_VEC(tmpl)  templates;
	SG_VEC(macro) macros;
};

static void free_expr(expr *e);
static void free_nodes(nodes *list);

static void free_exprs(exprs *list) {
	for (size_t i = 0; i < list->count; ++i) {
		free_expr(list->items[i]);
	}

	free(list->items);
}

static void free_expr(expr *e) {
	if (!e) {
		return;
	}

	free(e->text);

	for (size_t i = 0; i < e->path.count; ++i) {
		free(e->path.items[i].name);
	}

	free(e->path.items);
	free_expr(e->left);
	free_expr(e->right);
	free_exprs(&e->args);
	free(e);
}

static void free_node(node *n) {
	free_expr(n->value);
	free(n->name);
	free_nodes(&n->body);

	for (size_t i = 0; i < n->branches.count; ++i) {
		free_expr(n->branches.items[i].cond);
		free_nodes(&n->branches.items[i].body);
	}

	free(n->branches.items);
	free_exprs(&n->args);
	free(n);
}

static void free_nodes(nodes *list) {
	for (size_t i = 0; i < list->count; ++i) {
		free_node(list->items[i]);
	}

	free(list->items);
	memset(list, 0, sizeof *list);
}

/* ---- positions and errors ---------------------------------------------------------------- */

static pos pos_of(const tmpl *t, size_t offset) {
	size_t lo = 0;
	size_t hi = t->lines.count;

	while (hi - lo > 1) {
		size_t mid = (lo + hi) / 2;

		if (t->lines.items[mid] <= offset) {
			lo = mid;
		} else {
			hi = mid;
		}
	}

	pos at = { t->file, (unsigned)lo + 1, (unsigned)(offset - t->lines.items[lo]) + 1 };
	return at;
}

static void fail(pos at, const char *fmt, ...) SG_PRINTF(2, 3);

static void fail(pos at, const char *fmt, ...) {
	va_list args;
	char    message[512];

	va_start(args, fmt);
	vsnprintf(message, sizeof message, fmt, args);
	va_end(args);
	sg_error(at.file, at.line, at.col, "%s", message);
}

/* ---- expression parser ------------------------------------------------------------------- */

typedef struct {
	const tmpl *t;
	const char *src;
	size_t      at;
	size_t      end;
	bool        failed;
} eparser;

static bool ident_start(char c) {
	return c == '_' || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

static bool ident_char(char c) {
	return ident_start(c) || (c >= '0' && c <= '9');
}

static void eskip(eparser *p) {
	while (p->at < p->end && (p->src[p->at] == ' ' || p->src[p->at] == '\t' || p->src[p->at] == '\n'
			|| p->src[p->at] == '\r')) {
		++p->at;
	}
}

static pos epos(const eparser *p) {
	return pos_of(p->t, p->at);
}

static void efail(eparser *p, const char *fmt, ...) SG_PRINTF(2, 3);

static void efail(eparser *p, const char *fmt, ...) {
	va_list args;
	char    message[512];

	if (p->failed) {
		return;
	}

	va_start(args, fmt);
	vsnprintf(message, sizeof message, fmt, args);
	va_end(args);

	pos at = epos(p);

	sg_error(at.file, at.line, at.col, "%s", message);
	p->failed = true;
}

/* The identifier at the cursor, without consuming it; its length, 0 when there is none. */
static size_t peek_ident(const eparser *p) {
	size_t n = 0;

	if (p->at < p->end && ident_start(p->src[p->at])) {
		while (p->at + n < p->end && ident_char(p->src[p->at + n])) {
			++n;
		}
	}

	return n;
}

static bool peek_word(eparser *p, const char *word) {
	eskip(p);

	size_t n = peek_ident(p);

	return n == strlen(word) && memcmp(p->src + p->at, word, n) == 0;
}

static bool accept(eparser *p, const char *symbol) {
	size_t n = strlen(symbol);

	eskip(p);

	if (p->end - p->at >= n && memcmp(p->src + p->at, symbol, n) == 0) {
		p->at += n;
		return true;
	}

	return false;
}

static expr *new_expr(eparser *p, ekind kind) {
	expr *e = sg_xmalloc(sizeof *e);

	memset(e, 0, sizeof *e);
	e->kind = kind;
	e->at   = epos(p);
	return e;
}

static expr *parse_expr(eparser *p);

static char *parse_string(eparser *p) {
	sg_buf out = {0};

	++p->at;

	while (p->at < p->end && p->src[p->at] != '"') {
		char c = p->src[p->at++];

		if (c == '\\' && p->at < p->end) {
			char e = p->src[p->at++];

			c = e == 'n' ? '\n' : (e == 't' ? '\t' : e);
		}

		sg_buf_putc(&out, c);
	}

	if (p->at >= p->end) {
		efail(p, "this string is never closed");
		sg_buf_free(&out);
		return NULL;
	}

	++p->at;
	return out.data ? out.data : sg_xstrdup("");
}

static expr *parse_primary(eparser *p) {
	eskip(p);

	if (p->at >= p->end) {
		efail(p, "an expression ends where a value was expected");
		return NULL;
	}

	char c = p->src[p->at];

	if (c == '(') {
		++p->at;

		expr *inner = parse_expr(p);

		if (inner && !accept(p, ")")) {
			efail(p, "')' expected");
		}

		return inner;
	}

	if (c == '"') {
		expr *e = new_expr(p, E_STRING);

		e->text = parse_string(p);
		return e->text ? e : (free_expr(e), NULL);
	}

	if ((c >= '0' && c <= '9') || (c == '-' && p->at + 1 < p->end && p->src[p->at + 1] >= '0'
			&& p->src[p->at + 1] <= '9')) {
		expr *e    = new_expr(p, E_INT);
		bool  neg  = c == '-';

		p->at += neg;

		while (p->at < p->end && p->src[p->at] >= '0' && p->src[p->at] <= '9') {
			e->integer = e->integer * 10 + (p->src[p->at++] - '0');
		}

		e->integer = neg ? -e->integer : e->integer;
		return e;
	}

	size_t n = peek_ident(p);

	if (n == 0) {
		efail(p, "'%c' where a value was expected", c);
		return NULL;
	}

	if (peek_word(p, "true") || peek_word(p, "false")) {
		expr *e = new_expr(p, E_BOOL);

		e->boolean = p->src[p->at] == 't';
		p->at += n;
		return e;
	}

	if (peek_word(p, "null")) {
		expr *e = new_expr(p, E_NULL);

		p->at += n;
		return e;
	}

	expr    *e     = new_expr(p, E_PATH);
	segment *first = SG_PUSH(e->path);

	first->name = sg_xstrndup(p->src + p->at, n);
	p->at += n;

	for (;;) {
		if (p->at < p->end && p->src[p->at] == '.') {
			++p->at;

			size_t m = peek_ident(p);

			if (m == 0) {
				efail(p, "a member name expected after '.'");
				free_expr(e);
				return NULL;
			}

			SG_PUSH(e->path)->name = sg_xstrndup(p->src + p->at, m);
			p->at += m;
		} else if (p->at < p->end && p->src[p->at] == '[') {
			++p->at;

			long long index = 0;
			size_t    from  = p->at;

			while (p->at < p->end && p->src[p->at] >= '0' && p->src[p->at] <= '9') {
				index = index * 10 + (p->src[p->at++] - '0');
			}

			if (p->at == from || !accept(p, "]")) {
				efail(p, "an index is a number in brackets: [0]");
				free_expr(e);
				return NULL;
			}

			SG_PUSH(e->path)->index = index;
		} else {
			return e;
		}
	}
}

static bool parse_list(eparser *p, exprs *out, const char *close) {
	eskip(p);

	if (accept(p, close)) {
		return true;
	}

	for (;;) {
		expr *e = parse_expr(p);

		if (!e) {
			return false;
		}

		*SG_PUSH(*out) = e;

		if (accept(p, close)) {
			return true;
		}

		if (!accept(p, ",")) {
			efail(p, "',' or '%s' expected", close);
			return false;
		}
	}
}

static expr *parse_pipe(eparser *p) {
	expr *e = parse_primary(p);

	while (e && accept(p, "|")) {
		eskip(p);

		size_t n = peek_ident(p);

		if (n == 0) {
			efail(p, "a filter name expected after '|'");
			free_expr(e);
			return NULL;
		}

		expr *f = new_expr(p, E_FILTER);

		f->text = sg_xstrndup(p->src + p->at, n);
		f->left = e;
		p->at  += n;
		e       = f;

		if (accept(p, "(") && !parse_list(p, &f->args, ")")) {
			free_expr(e);
			return NULL;
		}
	}

	return e;
}

static expr *parse_cmp(eparser *p) {
	expr *e = parse_pipe(p);

	if (!e) {
		return NULL;
	}

	eskip(p);

	pos   at   = epos(p);
	ekind kind = accept(p, "==") ? E_EQ : accept(p, "!=") ? E_NE : accept(p, "<=") ? E_LE
			: accept(p, ">=") ? E_GE : accept(p, "<") ? E_LT : accept(p, ">") ? E_GT : E_PATH;

	if (kind == E_PATH) {
		return e;
	}

	/* An error comparing points at the operator. */
	expr *b = new_expr(p, kind);

	b->at = at;

	b->left  = e;
	b->right = parse_pipe(p);
	return b->right ? b : (free_expr(b), NULL);
}

static expr *parse_not(eparser *p) {
	if (peek_word(p, "not")) {
		expr *e = new_expr(p, E_NOT);

		p->at  += 3;
		e->left = parse_not(p);
		return e->left ? e : (free_expr(e), NULL);
	}

	return parse_cmp(p);
}

static expr *parse_binary(eparser *p, const char *word, ekind kind, expr *(*operand)(eparser *)) {
	expr *e = operand(p);

	while (e && peek_word(p, word)) {
		expr *b = new_expr(p, kind);

		p->at   += strlen(word);
		b->left  = e;
		b->right = operand(p);
		e        = b;

		if (!b->right) {
			free_expr(e);
			return NULL;
		}
	}

	return e;
}

static expr *parse_and(eparser *p) {
	return parse_binary(p, "and", E_AND, parse_not);
}

static expr *parse_expr(eparser *p) {
	return parse_binary(p, "or", E_OR, parse_and);
}

static eparser eparser_at(const tmpl *t, size_t start, size_t end) {
	eparser p = { t, t->source, start, end, false };
	return p;
}

/* A whole span as one expression; NULL, reported, when it is not exactly one. */
static expr *parse_span(const tmpl *t, size_t start, size_t end) {
	eparser p = eparser_at(t, start, end);
	expr   *e = parse_expr(&p);

	eskip(&p);

	if (e && p.at < p.end) {
		efail(&p, "'%c' after the end of the expression", p.src[p.at]);
		free_expr(e);
		return NULL;
	}

	return e;
}

/* ---- lexer ------------------------------------------------------------------------------- */

typedef enum {
	T_TEXT,
	T_EXPR,
	T_FOR,
	T_IF,
	T_ELIF,
	T_ELSE,
	T_ALIGN,
	T_DEFINE,
	T_CALL,
	T_INCLUDE,
	T_CLOSE,
	T_MARK,
	T_COMMENT
} tkind;

typedef struct {
	tkind   kind;
	size_t  start;     /* T_TEXT: its span; others: where the tag starts */
	size_t  len;
	size_t  inner;     /* the span between the expression brackets */
	size_t  inner_end;
	size_t  name;      /* T_DEFINE, T_CALL: the name's span */
	size_t  name_len;
	int     depth;     /* how many \define bodies enclose it */
	bool    removed;   /* T_TEXT on a line that holds only tags */
} token;

typedef SG_VEC(token) tokens;

typedef struct {
	const tmpl *t;
	const char *src;
	size_t      at;
	size_t      end;
	tokens     *out;
	bool        failed;
} lexer;

static void lfail(lexer *l, size_t at, const char *fmt, ...) SG_PRINTF(3, 4);

static void lfail(lexer *l, size_t at, const char *fmt, ...) {
	va_list args;
	char    message[512];

	va_start(args, fmt);
	vsnprintf(message, sizeof message, fmt, args);
	va_end(args);

	pos where = pos_of(l->t, at);

	sg_error(where.file, where.line, where.col, "%s", message);
	l->failed = true;
}

/* Text tokens hold at most one line break, at their end, so lines can be judged token by token. */
static void push_text(lexer *l, size_t from, size_t to) {
	while (from < to) {
		const char *nl  = memchr(l->src + from, '\n', to - from);
		size_t      end = nl ? (size_t)(nl - l->src) + 1 : to;
		token      *t   = SG_PUSH(*l->out);

		t->kind  = T_TEXT;
		t->start = from;
		t->len   = end - from;
		from     = end;
	}
}

static void skip_blanks(lexer *l) {
	while (l->at < l->end && (l->src[l->at] == ' ' || l->src[l->at] == '\t')) {
		++l->at;
	}
}

/* At an opening expression bracket: the span to its matching closer, which strings cannot end. */
static bool bracketed(lexer *l, size_t *inner, size_t *inner_end) {
	const tmpl *t     = l->t;
	size_t      open  = l->at;
	int         depth = 1;

	if (l->at >= l->end || l->src[l->at] != t->e_open) {
		return false;
	}

	*inner = ++l->at;

	while (l->at < l->end) {
		char c = l->src[l->at];

		if (c == '"') {
			for (++l->at; l->at < l->end && l->src[l->at] != '"'; ++l->at) {
				l->at += l->src[l->at] == '\\';
			}
		} else if (c == t->e_open) {
			++depth;
		} else if (c == t->e_close && --depth == 0) {
			*inner_end = l->at++;
			return true;
		}

		++l->at;
	}

	lfail(l, open, "this '%c' is never closed", t->e_open);
	return false;
}

static bool expect_block(lexer *l, size_t tag, const char *what) {
	skip_blanks(l);

	if (l->at < l->end && l->src[l->at] == l->t->b_open) {
		++l->at;
		return true;
	}

	lfail(l, tag, "%s must be followed by '%c'", what, l->t->b_open);
	return false;
}

static const struct {
	const char *word;
	tkind       kind;
} KEYWORDS[] = {
	{ "for", T_FOR }, { "if", T_IF }, { "elif", T_ELIF }, { "else", T_ELSE }, { "align", T_ALIGN },
	{ "define", T_DEFINE }, { "call", T_CALL }, { "include", T_INCLUDE }, { NULL, T_TEXT }
};

/* After the sigil at @p tag: one form, pushed as a token; false when it is no form at all. */
static bool lex_form(lexer *l, size_t tag) {
	const tmpl *t = l->t;
	char        c = l->at < l->end ? l->src[l->at] : '\0';
	token       tok;

	memset(&tok, 0, sizeof tok);
	tok.start = tag;

	if (c == t->e_open) {
		if (!bracketed(l, &tok.inner, &tok.inner_end)) {
			return true;
		}

		tok.kind = T_EXPR;
	} else if (c == t->b_close) {
		++l->at;
		tok.kind = T_CLOSE;
	} else if (c == '|') {
		++l->at;
		tok.kind = T_MARK;
	} else if (c == '*') {
		for (++l->at; l->at + 1 < l->end && !(l->src[l->at] == '*' && l->src[l->at + 1] == t->sigil); ++l->at) {
		}

		if (l->at + 1 >= l->end) {
			lfail(l, tag, "this comment is never closed with '*%c'", t->sigil);
			l->at = l->end;
			return true;
		}

		l->at += 2;
		tok.kind = T_COMMENT;
	} else {
		size_t n = 0;

		while (l->at + n < l->end && ident_char(l->src[l->at + n])) {
			++n;
		}

		tok.kind = T_TEXT;

		for (size_t k = 0; KEYWORDS[k].word; ++k) {
			if (strlen(KEYWORDS[k].word) == n && memcmp(l->src + l->at, KEYWORDS[k].word, n) == 0) {
				tok.kind = KEYWORDS[k].kind;
			}
		}

		if (tok.kind == T_TEXT) {
			return false;
		}

		l->at += n;

		const char *word = KEYWORDS[0].word;

		for (size_t k = 0; KEYWORDS[k].word; ++k) {
			word = KEYWORDS[k].kind == tok.kind ? KEYWORDS[k].word : word;
		}

		if (tok.kind == T_DEFINE || tok.kind == T_CALL) {
			skip_blanks(l);
			tok.name = l->at;

			while (l->at < l->end && ident_char(l->src[l->at])) {
				++l->at;
			}

			tok.name_len = l->at - tok.name;

			if (tok.name_len == 0) {
				lfail(l, tag, "%c%s needs a name", t->sigil, word);
				return true;
			}
		}

		if (tok.kind != T_ELSE && tok.kind != T_ALIGN) {
			skip_blanks(l);

			if (!bracketed(l, &tok.inner, &tok.inner_end)) {
				if (!l->failed) {
					lfail(l, tag, "%c%s must be followed by '%c'", t->sigil, word, t->e_open);
				}

				return true;
			}
		}

		if (tok.kind != T_CALL && tok.kind != T_INCLUDE) {
			char what[32];

			snprintf(what, sizeof what, "%c%s", t->sigil, word);

			if (!expect_block(l, tag, what)) {
				return true;
			}
		}
	}

	tok.len = l->at - tag;
	*SG_PUSH(*l->out) = tok;
	return true;
}

static bool is_control(tkind kind) {
	return kind != T_TEXT && kind != T_EXPR && kind != T_MARK;
}

static bool blank(const char *s, size_t len) {
	for (size_t i = 0; i < len; ++i) {
		if (s[i] != ' ' && s[i] != '\t' && s[i] != '\r' && s[i] != '\n') {
			return false;
		}
	}

	return true;
}

static bool opens_block(tkind kind) {
	return kind == T_FOR || kind == T_IF || kind == T_ELIF || kind == T_ELSE || kind == T_ALIGN
			|| kind == T_DEFINE;
}

/* How many \define bodies enclose each token; a close belongs to the body it ends. */
static void mark_depths(tokens *list) {
	SG_VEC(tkind) open = {0};
	int           depth = 0;

	for (size_t i = 0; i < list->count; ++i) {
		token *tok = &list->items[i];

		tok->depth = depth;

		if (opens_block(tok->kind)) {
			*SG_PUSH(open) = tok->kind;
			depth += tok->kind == T_DEFINE;
		} else if (tok->kind == T_CLOSE && open.count) {
			depth -= open.items[--open.count] == T_DEFINE;
		}
	}

	free(open.items);
}

static bool has_newline(const char *s, size_t len) {
	return memchr(s, '\n', len) != NULL;
}

/*
 * A line holding only tags and whitespace produces nothing: its whitespace is dropped. What a
 * \define on the line holds is not output there, so it does not count as the line's content; its
 * own text is kept, except a line break that only ends the line the \define starts or ends on.
 */
static void drop_tag_lines(const char *src, tokens *list) {
	size_t first = 0;

	mark_depths(list);

	while (first < list->count) {
		size_t last = first;

		while (last < list->count && !(list->items[last].kind == T_TEXT
				&& src[list->items[last].start + list->items[last].len - 1] == '\n')) {
			++last;
		}

		size_t end     = last < list->count ? last + 1 : last;
		int    level   = first < list->count ? list->items[first].depth : 0;
		bool   control = false;
		bool   content = false;

		/* The line belongs to the outermost body it touches, so the line a \define ends on is
		 * judged by what follows the body, like the line it starts on by what precedes it. */
		for (size_t i = first; i < end; ++i) {
			level = list->items[i].depth < level ? list->items[i].depth : level;
		}

		for (size_t i = first; i < end; ++i) {
			const token *tok = &list->items[i];

			control = control || is_control(tok->kind);

			if (tok->depth <= level) {
				content = content || tok->kind == T_EXPR || tok->kind == T_MARK
						|| (tok->kind == T_TEXT && !blank(src + tok->start, tok->len));
			}
		}

		if (control && !content) {
			for (size_t i = first; i < end; ++i) {
				token *tok = &list->items[i];

				tok->removed = tok->kind == T_TEXT && (tok->depth <= level
						|| (blank(src + tok->start, tok->len) && has_newline(src + tok->start, tok->len)));
			}
		}

		first = end;
	}
}

static bool lex(const tmpl *t, size_t from, size_t to, tokens *out) {
	lexer  l    = { t, t->source, from, to, out, false };
	size_t text = from;

	while (l.at < l.end) {
		if (l.src[l.at] != t->sigil) {
			++l.at;
			continue;
		}

		size_t tag = l.at++;

		push_text(&l, text, tag);

		if (!lex_form(&l, tag)) {
			/* No form: the sigil stays in the text. */
			text = tag;
			continue;
		}

		text = l.at;
	}

	push_text(&l, text, l.end);
	drop_tag_lines(t->source, out);
	return !l.failed;
}

/* ---- parser ------------------------------------------------------------------------------ */

typedef struct {
	sg_template_set *set;
	tmpl            *t;
	tokens          *list;
	size_t           at;
	int              aligns;
	bool             failed;
} parser;

static node *new_node(parser *p, nkind kind, size_t offset) {
	node *n = sg_xmalloc(sizeof *n);

	memset(n, 0, sizeof *n);
	n->kind = kind;
	n->at   = pos_of(p->t, offset);
	return n;
}

static bool parse_nodes(parser *p, nodes *out, bool block, size_t opened);

static bool parse_block(parser *p, nodes *out, size_t opened) {
	return parse_nodes(p, out, true, opened);
}

static void pfail(parser *p, size_t offset, const char *fmt, ...) SG_PRINTF(3, 4);

static void pfail(parser *p, size_t offset, const char *fmt, ...) {
	va_list args;
	char    message[512];

	va_start(args, fmt);
	vsnprintf(message, sizeof message, fmt, args);
	va_end(args);

	pos at = pos_of(p->t, offset);

	sg_error(at.file, at.line, at.col, "%s", message);
	p->failed = true;
}

/* "x in expr" */
static bool parse_for_head(parser *p, const token *tok, node *n) {
	eparser e = eparser_at(p->t, tok->inner, tok->inner_end);

	eskip(&e);

	size_t len = peek_ident(&e);

	if (len == 0) {
		efail(&e, "a loop names its variable first: (x in list)");
		return false;
	}

	n->name = sg_xstrndup(e.src + e.at, len);
	e.at   += len;

	if (!peek_word(&e, "in")) {
		efail(&e, "'in' expected after the loop variable");
		return false;
	}

	e.at += 2;
	n->value = parse_span(p->t, e.at, tok->inner_end);
	return n->value != NULL;
}

/* After an if or elif branch: whitespace-only text, then an elif or else, continues the chain. */
static bool continues_chain(parser *p) {
	size_t i = p->at;

	while (i < p->list->count && p->list->items[i].kind == T_TEXT
			&& (p->list->items[i].removed || blank(p->t->source + p->list->items[i].start, p->list->items[i].len))) {
		++i;
	}

	if (i < p->list->count && (p->list->items[i].kind == T_ELIF || p->list->items[i].kind == T_ELSE)) {
		p->at = i;
		return true;
	}

	return false;
}

/* Comma-separated expressions filling a whole span; an empty span is no expressions. */
static bool parse_span_list(const tmpl *t, size_t start, size_t end, exprs *out) {
	eparser e = eparser_at(t, start, end);

	eskip(&e);

	while (e.at < e.end) {
		expr *x = parse_expr(&e);

		if (!x) {
			return false;
		}

		*SG_PUSH(*out) = x;
		eskip(&e);

		if (e.at < e.end && !accept(&e, ",")) {
			efail(&e, "',' expected between arguments");
			return false;
		}

		eskip(&e);
	}

	return true;
}

static bool parse_nodes(parser *p, nodes *out, bool block, size_t opened) {
	while (p->at < p->list->count) {
		const token *tok = &p->list->items[p->at++];

		switch (tok->kind) {
		case T_TEXT:
			if (!tok->removed) {
				node *n = new_node(p, N_TEXT, tok->start);

				n->text = p->t->source + tok->start;
				n->len  = tok->len;
				*SG_PUSH(*out) = n;
			}
			break;

		case T_COMMENT:
			break;

		case T_EXPR: {
			node *n = new_node(p, N_EXPR, tok->start);

			n->value = parse_span(p->t, tok->inner, tok->inner_end);

			if (!n->value) {
				free_node(n);
				return false;
			}

			*SG_PUSH(*out) = n;
			break;
		}

		case T_MARK:
			if (p->aligns == 0) {
				pfail(p, tok->start, "%c| marks a column, so it belongs inside %calign", p->t->sigil, p->t->sigil);
				return false;
			}

			*SG_PUSH(*out) = new_node(p, N_MARK, tok->start);
			break;

		case T_CLOSE:
			if (!block) {
				pfail(p, tok->start, "this %c%c closes no block", p->t->sigil, p->t->b_close);
				return false;
			}

			return true;

		case T_FOR: {
			node *n = new_node(p, N_FOR, tok->start);

			*SG_PUSH(*out) = n;

			if (!parse_for_head(p, tok, n) || !parse_block(p, &n->body, tok->start)) {
				return false;
			}

			break;
		}

		case T_IF: {
			node *n    = new_node(p, N_IF, tok->start);
			bool  last = false;

			*SG_PUSH(*out) = n;

			for (const token *at = tok;;) {
				branch *b = SG_PUSH(n->branches);

				if (at->kind != T_ELSE) {
					b->cond = parse_span(p->t, at->inner, at->inner_end);

					if (!b->cond) {
						return false;
					}
				}

				last = at->kind == T_ELSE;

				if (!parse_block(p, &b->body, at->start)) {
					return false;
				}

				if (last || !continues_chain(p)) {
					break;
				}

				at = &p->list->items[p->at++];
			}

			break;
		}

		case T_ELIF:
		case T_ELSE:
			pfail(p, tok->start, "this %c%s follows no %cif", p->t->sigil, tok->kind == T_ELIF ? "elif" : "else",
					p->t->sigil);
			return false;

		case T_ALIGN: {
			node *n = new_node(p, N_ALIGN, tok->start);

			*SG_PUSH(*out) = n;
			++p->aligns;

			bool ok = parse_block(p, &n->body, tok->start);

			--p->aligns;

			if (!ok) {
				return false;
			}

			break;
		}

		case T_DEFINE: {
			macro *m    = SG_PUSH(p->set->macros);
			char  *name = sg_xstrndup(p->t->source + tok->name, tok->name_len);

			m->name = name;
			m->at   = pos_of(p->t, tok->start);

			eparser e = eparser_at(p->t, tok->inner, tok->inner_end);

			for (eskip(&e); e.at < e.end; eskip(&e)) {
				size_t len = peek_ident(&e);

				if (len == 0) {
					efail(&e, "parameters are names separated by ','");
					return false;
				}

				*SG_PUSH(m->params) = sg_xstrndup(e.src + e.at, len);
				e.at += len;

				if (!accept(&e, ",") && (eskip(&e), e.at < e.end)) {
					efail(&e, "',' expected between parameters");
					return false;
				}
			}

			for (size_t i = 0; i + 1 < p->set->macros.count; ++i) {
				if (strcmp(p->set->macros.items[i].name, name) == 0) {
					pfail(p, tok->start, "%s is defined twice; first at %s:%u", name,
							p->set->macros.items[i].at.file, p->set->macros.items[i].at.line);
					return false;
				}
			}

			/* The macro array may grow while its body is parsed, so the body is filled through an index. */
			size_t index = p->set->macros.count - 1;
			nodes  body  = {0};

			if (!parse_block(p, &body, tok->start)) {
				free_nodes(&body);
				return false;
			}

			p->set->macros.items[index].body = body;
			break;
		}

		case T_CALL: {
			node *n = new_node(p, N_CALL, tok->start);

			n->name = sg_xstrndup(p->t->source + tok->name, tok->name_len);
			*SG_PUSH(*out) = n;

			if (!parse_span_list(p->t, tok->inner, tok->inner_end, &n->args)) {
				return false;
			}

			break;
		}

		case T_INCLUDE: {
			node *n = new_node(p, N_INCLUDE, tok->start);
			expr *e = parse_span(p->t, tok->inner, tok->inner_end);

			*SG_PUSH(*out) = n;

			if (!e || e->kind != E_STRING) {
				if (e) {
					pfail(p, tok->start, "%cinclude names a template as a string: %cinclude(\"header\")",
							p->t->sigil, p->t->sigil);
				}

				free_expr(e);
				return false;
			}

			n->name = sg_xstrdup(e->text);
			free_expr(e);
			break;
		}
		}

		if (p->failed) {
			return false;
		}
	}

	if (block) {
		pfail(p, opened, "this block is never closed with %c%c", p->t->sigil, p->t->b_close);
		return false;
	}

	return true;
}

/* ---- header and set ---------------------------------------------------------------------- */

typedef struct {
	size_t start;
	size_t end;
	bool   given;
} span;

static bool line_bounds(const char *src, size_t at, size_t len, size_t *end, size_t *next) {
	if (at >= len) {
		return false;
	}

	const char *nl = memchr(src + at, '\n', len - at);

	*next = nl ? (size_t)(nl - src) + 1 : len;
	*end  = nl ? (size_t)(nl - src) : len;

	if (*end > at && src[*end - 1] == '\r') {
		--*end;
	}

	return true;
}

static void trim(const char *src, size_t *from, size_t *to) {
	while (*from < *to && (src[*from] == ' ' || src[*from] == '\t')) {
		++*from;
	}

	while (*to > *from && (src[*to - 1] == ' ' || src[*to - 1] == '\t')) {
		--*to;
	}
}

static bool usable_char(char c) {
	return c > ' ' && c != '"' && c != 0x7f && !ident_char(c);
}

/* "( )" or "()": two different usable characters. */
static bool bracket_pair(const char *src, size_t from, size_t to, char *open, char *close) {
	size_t n = 0;
	char   found[2];

	for (size_t i = from; i < to; ++i) {
		if (src[i] == ' ' || src[i] == '\t') {
			continue;
		}

		if (n == 2 || !usable_char(src[i])) {
			return false;
		}

		found[n++] = src[i];
	}

	if (n != 2 || found[0] == found[1]) {
		return false;
	}

	*open  = found[0];
	*close = found[1];
	return true;
}

/* The header between "<<<" and ">>>"; @p body receives where the template itself starts. */
static bool parse_header(tmpl *t, size_t *body, span *output, span *each, span *when) {
	const char *src = t->source;
	size_t      len = strlen(src);
	size_t      end;
	size_t      next;
	bool        ok  = true;

	*body = 0;

	if (!line_bounds(src, 0, len, &end, &next)) {
		return true;
	}

	size_t a = 0;
	size_t b = end;

	trim(src, &a, &b);

	if (b - a != 3 || memcmp(src + a, "<<<", 3) != 0) {
		return true;
	}

	for (size_t at = next; ; at = next) {
		if (!line_bounds(src, at, len, &end, &next)) {
			sg_error(t->file, 1, 1, "this header is never closed with a line '>>>'");
			return false;
		}

		size_t from = at;
		size_t to   = end;

		trim(src, &from, &to);

		if (to - from == 3 && memcmp(src + from, ">>>", 3) == 0) {
			*body = next;
			break;
		}

		if (from == to) {
			continue;
		}

		const char *colon = memchr(src + from, ':', to - from);
		pos         where = pos_of(t, from);

		if (!colon) {
			sg_error(where.file, where.line, where.col, "a header line is 'key: value'");
			ok = false;
			continue;
		}

		size_t key_end = (size_t)(colon - src);
		size_t value   = key_end + 1;
		size_t vend    = to;

		trim(src, &from, &key_end);
		trim(src, &value, &vend);

		char *key = sg_xstrndup(src + from, key_end - from);

		if (strcmp(key, "sigil") == 0) {
			if (vend - value != 1 || !usable_char(src[value])) {
				sg_error(where.file, where.line, where.col, "sigil is one character that is no letter,"
						" digit, space or quote");
				ok = false;
			} else {
				t->sigil = src[value];
			}
		} else if (strcmp(key, "expr") == 0 || strcmp(key, "block") == 0) {
			bool  expr_key = key[0] == 'e';
			char *open     = expr_key ? &t->e_open : &t->b_open;
			char *close    = expr_key ? &t->e_close : &t->b_close;

			if (!bracket_pair(src, value, vend, open, close)) {
				sg_error(where.file, where.line, where.col, "%s is an opening and a closing character,"
						" different from each other: ( )", key);
				ok = false;
			}
		} else if (strcmp(key, "output") == 0 || strcmp(key, "each") == 0 || strcmp(key, "when") == 0) {
			span *s = key[0] == 'o' ? output : (key[0] == 'e' ? each : when);

			s->start = value;
			s->end   = vend;
			s->given = true;
		} else {
			sg_error(where.file, where.line, where.col, "unknown header key '%s'; known: sigil, expr,"
					" block, output, each, when", key);
			ok = false;
		}

		free(key);
	}

	char used[] = { t->e_open, t->e_close, t->b_open, t->b_close };

	for (size_t i = 0; ok && i < sizeof used; ++i) {
		if (used[i] == t->sigil) {
			sg_error(t->file, 1, 1, "the sigil '%c' is also one of the brackets", t->sigil);
			ok = false;
		}
	}

	if (ok && (each->given || when->given) && !output->given) {
		sg_error(t->file, 1, 1, "%s needs an output: the file it decides about", each->given ? "each" : "when");
		ok = false;
	}

	return ok;
}

static void index_lines(tmpl *t) {
	*SG_PUSH(t->lines) = 0;

	for (size_t i = 0; t->source[i]; ++i) {
		if (t->source[i] == '\n') {
			*SG_PUSH(t->lines) = i + 1;
		}
	}
}

static bool parse_template(sg_template_set *set, tmpl *t) {
	span   output = { 0, 0, false };
	span   each   = { 0, 0, false };
	span   when   = { 0, 0, false };
	size_t body   = 0;

	if (!parse_header(t, &body, &output, &each, &when)) {
		return false;
	}

	tokens list = {0};
	parser p    = { set, t, &list, 0, 0, false };
	bool   ok   = lex(t, body, strlen(t->source), &list) && parse_nodes(&p, &t->body, false, body);

	free(list.items);

	if (ok && output.given) {
		tokens names = {0};

		t->has_output = true;
		ok = lex(t, output.start, output.end, &names);

		for (size_t i = 0; ok && i < names.count; ++i) {
			const token *tok = &names.items[i];

			if (tok->kind != T_TEXT && tok->kind != T_EXPR) {
				pos at = pos_of(t, tok->start);

				sg_error(at.file, at.line, at.col, "output holds only text and %c%c…%c", t->sigil,
						t->e_open, t->e_close);
				ok = false;
			}
		}

		parser q = { set, t, &names, 0, 0, false };

		ok = ok && parse_nodes(&q, &t->output, false, output.start);
		free(names.items);
	}

	if (ok && each.given) {
		eparser e = eparser_at(t, each.start, each.end);
		size_t  n;

		eskip(&e);
		n = peek_ident(&e);

		if (n == 0) {
			efail(&e, "each is 'name in list'");
			return false;
		}

		t->each_var = sg_xstrndup(e.src + e.at, n);
		e.at += n;

		if (!peek_word(&e, "in")) {
			efail(&e, "'in' expected after the name");
			return false;
		}

		t->each = parse_span(t, e.at + 2, each.end);
		ok      = t->each != NULL;
	}

	if (ok && when.given) {
		t->when = parse_span(t, when.start, when.end);
		ok      = t->when != NULL;
	}

	return ok;
}

static void free_template(tmpl *t) {
	free(t->name);
	free(t->file);
	free(t->source);
	free(t->lines.items);
	free_nodes(&t->body);
	free_nodes(&t->output);
	free(t->each_var);
	free_expr(t->each);
	free_expr(t->when);
}

void sg_templates_free(sg_template_set *set) {
	if (!set) {
		return;
	}

	for (size_t i = 0; i < set->templates.count; ++i) {
		free_template(&set->templates.items[i]);
	}

	free(set->templates.items);

	for (size_t i = 0; i < set->macros.count; ++i) {
		macro *m = &set->macros.items[i];

		free(m->name);

		for (size_t j = 0; j < m->params.count; ++j) {
			free(m->params.items[j]);
		}

		free(m->params.items);
		free_nodes(&m->body);
	}

	free(set->macros.items);
	free(set);
}

sg_template_set *sg_templates_parse(const sg_template_source *sources, size_t count, const char *origin) {
	sg_template_set *set = sg_xmalloc(sizeof *set);
	bool             ok  = true;

	memset(set, 0, sizeof *set);

	/* Every template first, so their addresses stay put while macros refer to them. */
	for (size_t i = 0; i < count; ++i) {
		tmpl *t = SG_PUSH(set->templates);

		t->name    = sg_xstrdup(sources[i].name);
		t->file    = origin ? sg_path_join(origin, sources[i].name) : sg_xstrdup(sources[i].name);
		t->source  = sg_xstrdup(sources[i].text);
		t->sigil   = '\\';
		t->e_open  = '(';
		t->e_close = ')';
		t->b_open  = '{';
		t->b_close = '}';
		index_lines(t);
	}

	for (size_t i = 0; i < set->templates.count; ++i) {
		ok = parse_template(set, &set->templates.items[i]) && ok;
	}

	if (!ok) {
		sg_templates_free(set);
		return NULL;
	}

	return set;
}

/* ---- rendering --------------------------------------------------------------------------- */

typedef struct {
	const char     *name;
	const sg_value *value;
} binding;

typedef struct {
	const sg_template_set *set;
	const sg_value        *root;
	const sg_filter       *filters;
	size_t                 filter_count;
	SG_VEC(binding)        scope;
	int                    depth;
} renderer;

static bool render_nodes(renderer *r, const nodes *list, sg_buf *out);

static void bind(renderer *r, const char *name, const sg_value *value) {
	binding *b = SG_PUSH(r->scope);

	b->name  = name;
	b->value = value;
}

static const sg_value *lookup(const renderer *r, const char *name) {
	for (size_t i = r->scope.count; i > 0; --i) {
		if (strcmp(r->scope.items[i - 1].name, name) == 0) {
			return r->scope.items[i - 1].value;
		}
	}

	return sg_map_get(r->root, name);
}

static void path_text(const expr *e, size_t upto, sg_buf *out) {
	for (size_t i = 0; i < upto; ++i) {
		const segment *s = &e->path.items[i];

		if (s->name) {
			sg_buf_printf(out, "%s%s", i ? "." : "", s->name);
		} else {
			sg_buf_printf(out, "[%lld]", s->index);
		}
	}
}

static sg_value *eval(renderer *r, const expr *e);

static sg_value *eval_path(renderer *r, const expr *e) {
	const sg_value *at = lookup(r, e->path.items[0].name);

	if (!at) {
		fail(e->at, "unknown name '%s'", e->path.items[0].name);
		return NULL;
	}

	for (size_t i = 1; i < e->path.count; ++i) {
		const segment *s    = &e->path.items[i];
		sg_buf         text = {0};

		path_text(e, i, &text);

		if (s->name) {
			const sg_value *next = at->kind == SG_V_MAP ? sg_map_get(at, s->name) : NULL;

			if (at->kind != SG_V_MAP) {
				fail(e->at, "%s is %s, which has no member '%s'", text.data, sg_vkind_name(at->kind), s->name);
			} else if (!next) {
				fail(e->at, "%s has no member '%s'", text.data, s->name);
			}

			sg_buf_free(&text);

			if (!next) {
				return NULL;
			}

			at = next;
		} else {
			if (at->kind != SG_V_LIST || s->index < 0 || (size_t)s->index >= at->items.count) {
				fail(e->at, "%s[%lld] is out of range: %s holds %zu items", text.data, s->index, text.data,
						at->kind == SG_V_LIST ? at->items.count : 0);
				sg_buf_free(&text);
				return NULL;
			}

			sg_buf_free(&text);
			at = at->items.items[s->index];
		}
	}

	return sg_value_copy(at);
}

/* ---- built-in filters -------------------------------------------------------------------- */

static sg_value *filter_error(char **error, const char *fmt, ...) SG_PRINTF(2, 3);

static sg_value *filter_error(char **error, const char *fmt, ...) {
	va_list args;
	char    message[256];

	va_start(args, fmt);
	vsnprintf(message, sizeof message, fmt, args);
	va_end(args);
	*error = sg_xstrdup(message);
	return NULL;
}

typedef struct {
	sg_buf     *out;
	int         style;   /* 0 snake, 1 upper, 2 pascal, 3 camel */
	size_t      count;
} casing;

static void case_word(const char *word, void *user) {
	casing *c = user;

	if (c->count && (c->style == 0 || c->style == 1)) {
		sg_buf_putc(c->out, '_');
	}

	for (size_t i = 0; word[i]; ++i) {
		char ch = word[i];

		if (c->style == 1 || ((c->style == 2 || (c->style == 3 && c->count)) && i == 0)) {
			ch = ch >= 'a' && ch <= 'z' ? (char)(ch - 'a' + 'A') : ch;
		}

		sg_buf_putc(c->out, ch);
	}

	++c->count;
}

/* A list of words, or a text split into words, joined in a style. */
static sg_value *recase(const sg_value *input, int style, const char *name, char **error) {
	sg_buf out = {0};
	casing c   = { &out, style, 0 };

	if (input->kind == SG_V_TEXT) {
		sg_split_words(input->text, case_word, &c);
	} else if (input->kind == SG_V_LIST) {
		for (size_t i = 0; i < input->items.count; ++i) {
			if (input->items.items[i]->kind != SG_V_TEXT) {
				sg_buf_free(&out);
				return filter_error(error, "%s takes a text or a list of words; this list holds %s", name,
						sg_vkind_name(input->items.items[i]->kind));
			}

			sg_split_words(input->items.items[i]->text, case_word, &c);
		}
	} else {
		return filter_error(error, "%s takes a text or a list of words, not %s", name, sg_vkind_name(input->kind));
	}

	return sg_value_text_owned(out.data ? out.data : sg_xstrdup(""));
}

static bool scalar_text(const sg_value *v, sg_buf *out) {
	switch (v->kind) {
	case SG_V_TEXT: sg_buf_puts(out, v->text); return true;
	case SG_V_INT:  sg_buf_printf(out, "%lld", v->integer); return true;
	case SG_V_BOOL: sg_buf_puts(out, v->boolean ? "true" : "false"); return true;
	default:        return false;
	}
}

static sg_value *builtin(const char *name, const sg_value *input, sg_value *const *args, size_t count,
		char **error, bool *found) {
	static const char *const cases[] = { "snake", "upper", "pascal", "camel" };

	*found = true;

	for (int i = 0; i < 4; ++i) {
		if (strcmp(name, cases[i]) == 0) {
			return count ? filter_error(error, "%s takes no arguments", name) : recase(input, i, name, error);
		}
	}

	if (strcmp(name, "lower") == 0) {
		if (input->kind != SG_V_TEXT) {
			return filter_error(error, "lower takes a text, not %s", sg_vkind_name(input->kind));
		}

		char *text = sg_xstrdup(input->text);

		for (char *c = text; *c; ++c) {
			*c = *c >= 'A' && *c <= 'Z' ? (char)(*c - 'A' + 'a') : *c;
		}

		return sg_value_text_owned(text);
	}

	if (strcmp(name, "join") == 0) {
		if (count != 1 || args[0]->kind != SG_V_TEXT || input->kind != SG_V_LIST) {
			return filter_error(error, "join takes a list and one text argument, the separator: join(\", \")");
		}

		sg_buf out = {0};

		for (size_t i = 0; i < input->items.count; ++i) {
			sg_buf_puts(&out, i ? args[0]->text : "");

			if (!scalar_text(input->items.items[i], &out)) {
				sg_buf_free(&out);
				return filter_error(error, "join takes a list of texts, numbers or booleans");
			}
		}

		return sg_value_text_owned(out.data ? out.data : sg_xstrdup(""));
	}

	if (strcmp(name, "count") == 0) {
		switch (input->kind) {
		case SG_V_LIST: return sg_value_int((long long)input->items.count);
		case SG_V_MAP:  return sg_value_int((long long)input->members.count);
		case SG_V_TEXT: return sg_value_int((long long)strlen(input->text));
		default:        return filter_error(error, "count takes a list, a map or a text, not %s", sg_vkind_name(input->kind));
		}
	}

	if (strcmp(name, "first") == 0 || strcmp(name, "last") == 0) {
		if (input->kind != SG_V_LIST || input->items.count == 0) {
			return filter_error(error, "%s takes a list that is not empty", name);
		}

		return sg_value_copy(input->items.items[name[0] == 'f' ? 0 : input->items.count - 1]);
	}

	if (strcmp(name, "at") == 0) {
		if (count != 1 || args[0]->kind != SG_V_INT || input->kind != SG_V_LIST) {
			return filter_error(error, "at takes a list and one integer argument, the index: at(0)");
		}

		if (args[0]->integer < 0 || (size_t)args[0]->integer >= input->items.count) {
			return filter_error(error, "at(%lld) is out of range: the list holds %zu items", args[0]->integer,
					input->items.count);
		}

		return sg_value_copy(input->items.items[args[0]->integer]);
	}

	if (strcmp(name, "pluck") == 0) {
		if (count != 1 || args[0]->kind != SG_V_TEXT || input->kind != SG_V_LIST) {
			return filter_error(error, "pluck takes a list of maps and one text argument, the key: pluck(\"name\")");
		}

		sg_value *out = sg_value_list();

		for (size_t i = 0; i < input->items.count; ++i) {
			const sg_value *item   = input->items.items[i];
			const sg_value *member = item->kind == SG_V_MAP ? sg_map_get(item, args[0]->text) : NULL;

			if (!member) {
				sg_value_free(out);
				return filter_error(error, "pluck(\"%s\"): item %zu has no member '%s'", args[0]->text, i,
						args[0]->text);
			}

			sg_list_push(out, sg_value_copy(member));
		}

		return out;
	}

	if (strcmp(name, "flatten") == 0) {
		if (count || input->kind != SG_V_LIST) {
			return filter_error(error, "flatten takes a list of lists, and no arguments");
		}

		sg_value *out = sg_value_list();

		for (size_t i = 0; i < input->items.count; ++i) {
			const sg_value *item = input->items.items[i];

			if (item->kind != SG_V_LIST) {
				sg_value_free(out);
				return filter_error(error, "flatten takes a list of lists; item %zu is %s", i, sg_vkind_name(item->kind));
			}

			for (size_t j = 0; j < item->items.count; ++j) {
				sg_list_push(out, sg_value_copy(item->items.items[j]));
			}
		}

		return out;
	}

	if (strcmp(name, "contains") == 0) {
		if (count != 1 || input->kind != SG_V_LIST) {
			return filter_error(error, "contains takes a list and one argument, the value to look for");
		}

		bool held = false;

		for (size_t i = 0; i < input->items.count && !held; ++i) {
			held = sg_value_equal(input->items.items[i], args[0]);
		}

		return sg_value_bool(held);
	}

	if (strcmp(name, "default") == 0) {
		if (count != 1) {
			return filter_error(error, "default takes one argument, the value to use for null");
		}

		return sg_value_copy(input->kind == SG_V_NULL ? args[0] : input);
	}

	*found = false;
	return NULL;
}

static sg_value *eval_filter(renderer *r, const expr *e) {
	sg_value *input = eval(r, e->left);

	if (!input) {
		return NULL;
	}

	SG_VEC(sg_value *) args = {0};
	sg_value *result        = NULL;
	char     *error         = NULL;
	bool      ok            = true;

	for (size_t i = 0; ok && i < e->args.count; ++i) {
		sg_value *arg = eval(r, e->args.items[i]);

		ok = arg != NULL;

		if (ok) {
			*SG_PUSH(args) = arg;
		}
	}

	if (ok) {
		bool found = false;

		for (size_t i = 0; i < r->filter_count && !found; ++i) {
			if (strcmp(r->filters[i].name, e->text) == 0) {
				found  = true;
				result = r->filters[i].fn(input, args.items, args.count, r->filters[i].user, &error);
			}
		}

		if (!found) {
			result = builtin(e->text, input, args.items, args.count, &error, &found);
		}

		if (!found) {
			fail(e->at, "unknown filter '%s'", e->text);
		} else if (!result) {
			fail(e->at, "%s", error ? error : "the filter failed");
		}
	}

	free(error);
	sg_value_free(input);

	for (size_t i = 0; i < args.count; ++i) {
		sg_value_free(args.items[i]);
	}

	free(args.items);
	return result;
}

static sg_value *eval(renderer *r, const expr *e) {
	switch (e->kind) {
	case E_PATH:   return eval_path(r, e);
	case E_STRING: return sg_value_text(e->text);
	case E_INT:    return sg_value_int(e->integer);
	case E_BOOL:   return sg_value_bool(e->boolean);
	case E_NULL:   return sg_value_null();
	case E_FILTER: return eval_filter(r, e);
	case E_NOT: {
		sg_value *v = eval(r, e->left);
		bool      t = v && sg_value_truthy(v);

		sg_value_free(v);
		return v ? sg_value_bool(!t) : NULL;
	}
	case E_AND:
	case E_OR: {
		sg_value *left = eval(r, e->left);

		if (!left) {
			return NULL;
		}

		bool truth = sg_value_truthy(left);

		sg_value_free(left);

		/* Short-circuit: the right side is not even looked up when the left decides. */
		if (truth == (e->kind == E_OR)) {
			return sg_value_bool(truth);
		}

		sg_value *right = eval(r, e->right);
		bool      rt    = right && sg_value_truthy(right);

		sg_value_free(right);
		return right ? sg_value_bool(rt) : NULL;
	}
	case E_EQ:
	case E_NE: {
		sg_value *left  = eval(r, e->left);
		sg_value *right = left ? eval(r, e->right) : NULL;
		bool      same  = left && right && sg_value_equal(left, right);
		bool      ok    = left && right;

		sg_value_free(left);
		sg_value_free(right);
		return ok ? sg_value_bool(e->kind == E_EQ ? same : !same) : NULL;
	}
	case E_LT:
	case E_LE:
	case E_GT:
	case E_GE: {
		sg_value *left  = eval(r, e->left);
		sg_value *right = left ? eval(r, e->right) : NULL;
		bool      ok    = left && right && left->kind == SG_V_INT && right->kind == SG_V_INT;
		bool      truth = false;

		if (ok) {
			long long a = left->integer;
			long long b = right->integer;

			truth = e->kind == E_LT ? a < b : e->kind == E_LE ? a <= b : e->kind == E_GT ? a > b : a >= b;
		} else if (left && right) {
			fail(e->at, "<, <=, > and >= compare integers, not %s and %s", sg_vkind_name(left->kind),
					sg_vkind_name(right->kind));
		}

		sg_value_free(left);
		sg_value_free(right);
		return ok ? sg_value_bool(truth) : NULL;
	}
	}

	return NULL;
}

/* ---- nodes ------------------------------------------------------------------------------- */

/* Display width in characters: UTF-8 continuation bytes take no column. */
static size_t width(const char *s, size_t len) {
	size_t n = 0;

	for (size_t i = 0; i < len; ++i) {
		n += ((unsigned char)s[i] & 0xC0) != 0x80;
	}

	return n;
}

/* Pads each column a line marks to the widest cell in that column, plus one space. */
static void align_lines(const sg_buf *in, sg_buf *out) {
	SG_VEC(size_t) widths = {0};
	const char    *text   = in->data ? in->data : "";
	size_t         len    = in->len;

	for (int pass = 0; pass < 2; ++pass) {
		size_t at = 0;

		while (at < len) {
			const char *nl   = memchr(text + at, '\n', len - at);
			size_t      end  = nl ? (size_t)(nl - text) : len;
			size_t      cell = at;
			size_t      col  = 0;

			for (size_t i = at; i <= end; ++i) {
				if (i < end && text[i] != MARK_BYTE) {
					continue;
				}

				size_t w = width(text + cell, i - cell);

				if (i < end) {
					if (pass == 0) {
						while (widths.count <= col) {
							*SG_PUSH(widths) = 0;
						}

						widths.items[col] = w > widths.items[col] ? w : widths.items[col];
					} else {
						sg_buf_append(out, text + cell, i - cell);

						for (size_t pad = w; pad <= widths.items[col]; ++pad) {
							sg_buf_putc(out, ' ');
						}
					}
				} else if (pass == 1) {
					sg_buf_append(out, text + cell, i - cell);
				}

				cell = i + 1;
				++col;
			}

			if (pass == 1 && nl) {
				sg_buf_putc(out, '\n');
			}

			at = nl ? end + 1 : len;
		}
	}

	free(widths.items);
}

static const tmpl *find_template(const sg_template_set *set, const char *name) {
	size_t len = strlen(name);

	for (size_t i = 0; i < set->templates.count; ++i) {
		const char *have = set->templates.items[i].name;

		if (strcmp(have, name) == 0 || (strncmp(have, name, len) == 0 && strcmp(have + len, ".gtpl") == 0)) {
			return &set->templates.items[i];
		}
	}

	return NULL;
}

static bool print_value(const node *n, const sg_value *v, sg_buf *out) {
	if (scalar_text(v, out)) {
		return true;
	}

	fail(n->at, "the value is %s, which cannot be printed%s", sg_vkind_name(v->kind),
			v->kind == SG_V_NULL ? "; test it with \\if or give it a | default(...)" : "");
	return false;
}

static bool render_node(renderer *r, const node *n, sg_buf *out) {
	switch (n->kind) {
	case N_TEXT:
		sg_buf_append(out, n->text, n->len);
		return true;

	case N_MARK:
		sg_buf_putc(out, MARK_BYTE);
		return true;

	case N_EXPR: {
		sg_value *v  = eval(r, n->value);
		bool      ok = v && print_value(n, v, out);

		sg_value_free(v);
		return ok;
	}

	case N_FOR: {
		sg_value *list = eval(r, n->value);
		bool      ok   = list != NULL;

		if (list && list->kind != SG_V_LIST) {
			fail(n->at, "a loop goes over a list, not %s", sg_vkind_name(list->kind));
			ok = false;
		}

		for (size_t i = 0; ok && i < list->items.count; ++i) {
			sg_value *loop = sg_value_map();

			sg_map_set(loop, "index", sg_value_int((long long)i));
			sg_map_set(loop, "number", sg_value_int((long long)i + 1));
			sg_map_set(loop, "first", sg_value_bool(i == 0));
			sg_map_set(loop, "last", sg_value_bool(i + 1 == list->items.count));
			sg_map_set(loop, "count", sg_value_int((long long)list->items.count));

			bind(r, n->name, list->items.items[i]);
			bind(r, "loop", loop);
			ok = render_nodes(r, &n->body, out);
			r->scope.count -= 2;
			sg_value_free(loop);
		}

		sg_value_free(list);
		return ok;
	}

	case N_IF:
		for (size_t i = 0; i < n->branches.count; ++i) {
			const branch *b     = &n->branches.items[i];
			bool          taken = true;

			if (b->cond) {
				sg_value *v = eval(r, b->cond);

				if (!v) {
					return false;
				}

				taken = sg_value_truthy(v);
				sg_value_free(v);
			}

			if (taken) {
				return render_nodes(r, &b->body, out);
			}
		}

		return true;

	case N_ALIGN: {
		sg_buf inner = {0};
		bool   ok    = render_nodes(r, &n->body, &inner);

		if (ok) {
			align_lines(&inner, out);
		}

		sg_buf_free(&inner);
		return ok;
	}

	case N_INCLUDE: {
		const tmpl *t = find_template(r->set, n->name);

		if (!t) {
			fail(n->at, "no template named '%s' in this set", n->name);
			return false;
		}

		if (++r->depth > MAX_NESTING) {
			fail(n->at, "includes and calls nested more than %d deep", MAX_NESTING);
			return false;
		}

		bool ok = render_nodes(r, &t->body, out);

		--r->depth;
		return ok;
	}

	case N_CALL: {
		const macro *m = NULL;

		for (size_t i = 0; i < r->set->macros.count && !m; ++i) {
			m = strcmp(r->set->macros.items[i].name, n->name) == 0 ? &r->set->macros.items[i] : NULL;
		}

		if (!m) {
			fail(n->at, "no %s is defined", n->name);
			return false;
		}

		if (m->params.count != n->args.count) {
			fail(n->at, "%s takes %zu arguments, not %zu", n->name, m->params.count, n->args.count);
			return false;
		}

		if (++r->depth > MAX_NESTING) {
			fail(n->at, "includes and calls nested more than %d deep", MAX_NESTING);
			return false;
		}

		SG_VEC(sg_value *) args = {0};
		bool               ok   = true;

		for (size_t i = 0; ok && i < n->args.count; ++i) {
			sg_value *v = eval(r, n->args.items[i]);

			ok = v != NULL;

			if (ok) {
				*SG_PUSH(args) = v;
			}
		}

		size_t mark = r->scope.count;

		for (size_t i = 0; ok && i < args.count; ++i) {
			bind(r, m->params.items[i], args.items[i]);
		}

		ok = ok && render_nodes(r, &m->body, out);
		r->scope.count = mark;
		--r->depth;

		for (size_t i = 0; i < args.count; ++i) {
			sg_value_free(args.items[i]);
		}

		free(args.items);
		return ok;
	}
	}

	return false;
}

static bool render_nodes(renderer *r, const nodes *list, sg_buf *out) {
	for (size_t i = 0; i < list->count; ++i) {
		if (!render_node(r, list->items[i], out)) {
			return false;
		}
	}

	return true;
}

static bool safe_path(const char *path) {
	if (path[0] == '\0' || path[0] == '/') {
		return false;
	}

	for (const char *part = path; *part;) {
		size_t len = strcspn(part, "/");

		if (len == 0 || (len == 2 && part[0] == '.' && part[1] == '.') || (len == 1 && part[0] == '.')) {
			return false;
		}

		part += len + (part[len] == '/');
	}

	return true;
}

static bool render_file(renderer *r, const tmpl *t, sg_rendered *out) {
	sg_buf name = {0};
	sg_buf text = {0};

	if (!render_nodes(r, &t->output, &name) || !render_nodes(r, &t->body, &text)) {
		sg_buf_free(&name);
		sg_buf_free(&text);
		return false;
	}

	const char *path = name.data ? name.data : "";

	if (!safe_path(path)) {
		sg_error(t->file, 1, 1, "output '%s' must be a relative path inside the output directory", path);
		sg_buf_free(&name);
		sg_buf_free(&text);
		return false;
	}

	for (size_t i = 0; i < out->count; ++i) {
		if (strcmp(out->items[i].path, path) == 0) {
			sg_error(t->file, 1, 1, "output '%s' is produced twice", path);
			sg_buf_free(&name);
			sg_buf_free(&text);
			return false;
		}
	}

	sg_rendered_file *file = SG_PUSH(*out);

	file->path = name.data;
	file->text = text;

	if (!file->text.data) {
		file->text.data = sg_xstrdup("");
	}

	return true;
}

bool sg_templates_render(const sg_template_set *set, const sg_value *data, const sg_filter *filters,
		size_t filter_count, sg_rendered *out) {
	renderer r  = { set, data, filters, filter_count, { NULL, 0, 0 }, 0 };
	bool     ok = true;

	memset(out, 0, sizeof *out);

	for (size_t i = 0; ok && i < set->templates.count; ++i) {
		const tmpl *t = &set->templates.items[i];

		if (!t->has_output) {
			continue;
		}

		if (t->when) {
			sg_value *v = eval(&r, t->when);

			ok = v != NULL;

			bool produce = v && sg_value_truthy(v);

			sg_value_free(v);

			if (!produce) {
				continue;
			}
		}

		if (!t->each) {
			ok = render_file(&r, t, out);
			continue;
		}

		sg_value *list = eval(&r, t->each);

		ok = list != NULL;

		if (list && list->kind != SG_V_LIST) {
			sg_error(t->file, 1, 1, "each goes over a list, not %s", sg_vkind_name(list->kind));
			ok = false;
		}

		for (size_t j = 0; ok && j < list->items.count; ++j) {
			bind(&r, t->each_var, list->items.items[j]);
			ok = render_file(&r, t, out);
			--r.scope.count;
		}

		sg_value_free(list);
	}

	free(r.scope.items);

	if (!ok) {
		sg_rendered_free(out);
	}

	return ok;
}

void sg_rendered_free(sg_rendered *files) {
	for (size_t i = 0; i < files->count; ++i) {
		free(files->items[i].path);
		sg_buf_free(&files->items[i].text);
	}

	free(files->items);
	memset(files, 0, sizeof *files);
}
