# sqlgen -- one Makefile.
#
#   make                 build bin/release/sqlgen
#   make test            build, then run every test against it
#   make asan            the same build and tests under AddressSanitizer and UBSan
#   make CIPHER=1        link SQLCipher instead of plain SQLite (only `report --key` needs it)
#   make clean
#
# Each kind of build has its own directory under bin/, so switching between them never mixes
# objects compiled with different flags.

CC       ?= cc
BUILD    ?= release
CIPHER   ?= 0
SUFFIX   :=

SRC      := src/util.c src/semver.c src/json.c src/jsonw.c src/config.c src/schema.c src/introspect.c src/diff.c src/rules.c src/bytecode.c src/query.c src/plan.c src/value.c src/template.c src/builtin.c src/lang_c.c src/lang_doc.c src/generate.c src/model_json.c src/main.c
HDR      := $(wildcard include/*.h)

CSTD     := -std=c99
WARN     := -Wall -Wextra -pedantic -Wconversion -Wsign-conversion -Wshadow -Wstrict-prototypes \
            -Wmissing-prototypes -Wcast-qual -Wpointer-arith -Werror
CPPFLAGS := -D_POSIX_C_SOURCE=200809L -Iinclude -Ivendor/sqlite3
LIBS     := -lm

# The vendored file is the SQLCipher amalgamation, which is plain SQLite unless SQLITE_HAS_CODEC
# is defined. Its warnings are upstream's and are not ours to fix, so it gets no -Werror.
#   COLUMN_METADATA  sqlite3_column_table_name / _origin_name: where a view column comes from
#   THREADSAFE=0     sqlgen is single-threaded, so no mutexes and no -lpthread
#   OMIT_LOAD_EXTENSION  nothing is loaded at run time
#   EXPLAIN_COMMENTS describes each instruction's effect in explain; the query analysis reads it
SQLITE_DEFS := -DSQLITE_ENABLE_COLUMN_METADATA -DSQLITE_THREADSAFE=0 -DSQLITE_OMIT_LOAD_EXTENSION \
               -DSQLITE_DEFAULT_MEMSTATUS=0 -DSQLITE_ENABLE_EXPLAIN_COMMENTS

# SQLCipher refuses THREADSAFE=0 at compile time, so the cipher build takes the mutexes and pthreads.
ifeq ($(CIPHER),1)
SQLITE_DEFS := $(filter-out -DSQLITE_THREADSAFE=0,$(SQLITE_DEFS)) -DSQLITE_THREADSAFE=2
SQLITE_DEFS += -DSQLITE_HAS_CODEC -DSQLCIPHER_CRYPTO_OPENSSL -DSQLITE_TEMP_STORE=2 \
               -DSQLITE_EXTRA_INIT=sqlcipher_extra_init -DSQLITE_EXTRA_SHUTDOWN=sqlcipher_extra_shutdown
CPPFLAGS    += -DSQLITE_HAS_CODEC
LIBS        += -lcrypto -lpthread
SUFFIX      := -cipher
endif

ifeq ($(findstring asan,$(BUILD)),asan)
OPT      := -O1 -g -fno-omit-frame-pointer -fsanitize=address,undefined -fno-sanitize-recover=all
else
OPT      := -O2 -g
endif

OUT      := bin/$(BUILD)$(SUFFIX)
OBJ      := $(SRC:src/%.c=$(OUT)/%.o)
SQLITE_O := $(OUT)/sqlite3.o

.PHONY: all test asan clean

all: $(OUT)/sqlgen

$(OUT):
	@mkdir -p $@

$(SQLITE_O): vendor/sqlite3/sqlite3.c vendor/sqlite3/sqlite3.h Makefile | $(OUT)
	$(CC) $(OPT) $(SQLITE_DEFS) -w -c $< -o $@

$(OUT)/%.o: src/%.c $(HDR) Makefile | $(OUT)
	$(CC) $(CSTD) $(OPT) $(WARN) $(CPPFLAGS) -c $< -o $@

# The built-in templates: each file under templates/<language>/ is compiled in as a byte array.
LANGUAGES := c cpp markdown mermaid
BUILTIN_O := $(LANGUAGES:%=$(OUT)/builtin_%.o)

$(OUT)/embed: tools/embed.c | $(OUT)
	$(CC) $(CSTD) $(OPT) $(WARN) $< -o $@

.SECONDEXPANSION:
$(OUT)/builtin_%.c: $(OUT)/embed $$(sort $$(wildcard templates/$$*/*.gtpl)) templates/%
	$(OUT)/embed $@ $* $(sort $(wildcard templates/$*/*.gtpl))

$(OUT)/builtin_%.o: $(OUT)/builtin_%.c $(HDR)
	$(CC) $(CSTD) $(OPT) $(WARN) $(CPPFLAGS) -c $< -o $@

$(OUT)/sqlgen: $(OBJ) $(BUILTIN_O) $(SQLITE_O)
	$(CC) $(OPT) $^ -o $@ $(LIBS)

# Test helpers: unit tests for the version rules, strict JSON, templates and the configuration, and a tool that builds a database file from a
# script so `report` has something real to read.
$(OUT)/semver_test: test/semver_test.c $(OUT)/semver.o $(OUT)/util.o
	$(CC) $(CSTD) $(OPT) $(WARN) $(CPPFLAGS) $^ -o $@

$(OUT)/json_test: test/json_test.c $(OUT)/json.o $(OUT)/util.o
	$(CC) $(CSTD) $(OPT) $(WARN) $(CPPFLAGS) $^ -o $@

$(OUT)/template_test: test/template_test.c $(OUT)/template.o $(OUT)/value.o $(OUT)/util.o $(SQLITE_O)
	$(CC) $(CSTD) $(OPT) $(WARN) $(CPPFLAGS) $^ -o $@ $(LIBS)

$(OUT)/bytecode_test: test/bytecode_test.c $(OUT)/bytecode.o $(OUT)/introspect.o $(OUT)/util.o $(SQLITE_O)
	$(CC) $(CSTD) $(OPT) $(WARN) $(CPPFLAGS) $^ -o $@ $(LIBS)

# The schema the bytecode analysis is tested against: the real-schema case, in patch order.
BYTECODE_SCHEMA := test/cases/real-schema/sql/system.sql test/cases/real-schema/sql/0.1.0/users.sql \
                   test/cases/real-schema/sql/0.1.0/sessions.sql test/cases/real-schema/sql/0.1.0/linked_accounts.sql \
                   $(addprefix test/cases/real-schema/sql/0.2.0/,manga.sql manga_interest.sql source_manga.sql \
                   person.sql scan_group.sql chapter.sql stats_history.sql tag_canonical.sql tag_source.sql \
                   source_manga_author.sql source_manga_artist.sql source_manga_tag.sql reserved_accounts.sql)

$(OUT)/config_test: test/config_test.c $(OUT)/config.o $(OUT)/json.o $(OUT)/util.o $(SQLITE_O)
	$(CC) $(CSTD) $(OPT) $(WARN) $(CPPFLAGS) $^ -o $@ $(LIBS)

$(OUT)/mkdb: test/mkdb.c $(SQLITE_O)
	$(CC) $(CSTD) $(OPT) $(WARN) $(CPPFLAGS) $^ -o $@ $(LIBS)

# The C that test/cases/generate-c generates, compiled as strictly as anyone might, then driven
# against a real database by test/c/plan_test.c. test/c/wrong_id.c must not compile: typed ids of
# different tables do not mix.
GEN_C   := test/cases/generate-c/gen
GEN_CC  := $(CC) $(CSTD) $(OPT) -Wall -Wextra -pedantic -Wconversion -Wsign-conversion -Wshadow \
           -Wstrict-prototypes -Wmissing-prototypes -Wcast-qual -Werror -I$(GEN_C) $(CPPFLAGS)

# The C++ that test/cases/generate-cpp generates: built and run as C++11, the oldest it is written
# for, checked against every newer standard, and test/cpp/wrong_types.cpp refused case by case.
CXX      ?= c++
GEN_CPP  := test/cases/generate-cpp/gen
GEN_CXX  := $(CXX) $(OPT) -Wall -Wextra -pedantic -Wconversion -Wsign-conversion -Wshadow -Wold-style-cast \
            -Wcast-qual -Wzero-as-null-pointer-constant -Werror -I$(GEN_CPP) -Ivendor/sqlite3
CPP_STDS := c++14 c++17 c++20 c++23

test: $(OUT)/sqlgen $(OUT)/semver_test $(OUT)/json_test $(OUT)/template_test $(OUT)/config_test $(OUT)/mkdb \
		$(OUT)/bytecode_test
	$(OUT)/semver_test
	$(OUT)/json_test
	$(OUT)/template_test
	$(OUT)/bytecode_test $(BYTECODE_SCHEMA) > $(OUT)/bytecode.out
	@diff -u test/bytecode.expected $(OUT)/bytecode.out && echo "bytecode: ok"
	$(OUT)/config_test $(OUT)
	bash test/run.sh $(OUT)/sqlgen $(OUT)/mkdb $(if $(filter 1,$(CIPHER)),cipher,plain)
	$(GEN_CC) $(GEN_C)/*.c test/c/plan_test.c $(SQLITE_O) -o $(OUT)/plan_test $(LIBS)
	$(OUT)/plan_test
	@if $(GEN_CC) -c test/c/wrong_id.c -o /dev/null 2>/dev/null; then \
		echo "wrong_id: an id of one table was accepted for another"; exit 1; \
	else echo "wrong_id: refused, as it must be"; fi
	$(GEN_CXX) -std=c++11 $(GEN_CPP)/*.cpp test/cpp/plan_test.cpp $(SQLITE_O) -o $(OUT)/plan_test_cpp $(LIBS)
	$(OUT)/plan_test_cpp
	@for std in $(CPP_STDS); do for f in $(GEN_CPP)/*.cpp; do \
		$(GEN_CXX) -std=$$std -fsyntax-only $$f || exit 1; done; done; echo "c++: $(CPP_STDS) compile"
	$(GEN_CXX) -std=c++11 -DWRONG=0 -fsyntax-only test/cpp/wrong_types.cpp
	@for n in 1 2 3; do if $(GEN_CXX) -std=c++11 -DWRONG=$$n -fsyntax-only test/cpp/wrong_types.cpp 2>/dev/null; then \
		echo "wrong_types: case $$n compiled"; exit 1; fi; done; echo "wrong_types: every case refused, as it must be"

asan:
	$(MAKE) BUILD=asan test

clean:
	rm -rf bin
