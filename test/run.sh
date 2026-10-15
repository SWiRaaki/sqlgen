#!/usr/bin/env bash
# Runs every case under test/cases/ and compares what sqlgen printed with what was expected.
#
#   bash test/run.sh <sqlgen> <mkdb> <plain|cipher>          run
#   UPDATE=1 bash test/run.sh <sqlgen> <mkdb> <plain|cipher> rewrite the expectations from the output
#
# A case is a directory holding:
#   args           the arguments, one line, as a shell would split them
#   build          optional; "plain" or "cipher" -- the case runs only against that build
#   setup          optional; run first with bash, with $MKDB and $SQLGEN set
#   after          optional; run afterwards with bash, with $SQLGEN set; its output is compared with
#                  expected.after (for checking files a run wrote, or a second run)
#   expected.out   standard output   (absent means: must be empty)
#   expected.err   standard error    (absent means: must be empty)
#   expected.code  the exit status   (absent means: 0)
#
# sqlgen runs inside the case directory, so every path in the expectations is relative to it.

set -u

# Generated files carry their generation time; a fixed one makes them comparable.
export SOURCE_DATE_EPOCH=1700000000

sqlgen=$(realpath "$1")
mkdb=$(realpath "$2")
build=${3:-plain}
here=$(cd "$(dirname "$0")" && pwd)
out="$here/out"
pass=0
fail=0
skip=0

rm -rf "$out"
mkdir -p "$out"

compare() {
	local expected=$1 actual=$2 label=$3

	if [ "${UPDATE:-0}" = 1 ]; then
		if [ -s "$actual" ] || [ "$label" = code ]; then
			cp "$actual" "$expected"
		else
			rm -f "$expected"
		fi
		return 0
	fi

	if [ -f "$expected" ]; then
		diff -u "$expected" "$actual" > "$actual.diff" && return 0
	elif [ "$label" = code ]; then
		[ "$(cat "$actual")" = 0 ] && return 0
		echo "exit status $(cat "$actual"), expected 0" > "$actual.diff"
	else
		[ ! -s "$actual" ] && return 0
		{ echo "expected no $label, got:"; cat "$actual"; } > "$actual.diff"
	fi

	return 1
}

for dir in "$here"/cases/*/; do
	name=$(basename "$dir")
	result="$out/$name"

	if [ -f "$dir/build" ] && [ "$(cat "$dir/build")" != "$build" ]; then
		skip=$((skip + 1))
		continue
	fi

	mkdir -p "$result"

	if [ -f "$dir/setup" ]; then
		(cd "$dir" && MKDB="$mkdb" SQLGEN="$sqlgen" bash setup) > "$result/setup.log" 2>&1 || {
			echo "FAIL $name (setup)"; cat "$result/setup.log"; fail=$((fail + 1)); continue
		}
	fi

	read -r -a args < "$dir/args"
	# No terminal on standard input, wherever the tests are started from, so nothing waits for an answer.
	(cd "$dir" && "$sqlgen" "${args[@]}") < /dev/null > "$result/out" 2> "$result/err"
	echo $? > "$result/code"

	if [ -f "$dir/after" ]; then
		(cd "$dir" && SQLGEN="$sqlgen" bash after) > "$result/after" 2>&1
	fi

	ok=1
	[ -f "$dir/after" ] && { compare "$dir/expected.after" "$result/after" "after" || ok=0; }
	compare "$dir/expected.out"  "$result/out"  "output" || ok=0
	compare "$dir/expected.err"  "$result/err"  "errors" || ok=0
	compare "$dir/expected.code" "$result/code" "code"   || ok=0

	if [ $ok = 1 ]; then
		pass=$((pass + 1))
	else
		fail=$((fail + 1))
		echo "FAIL $name"
		cat "$result"/*.diff
	fi
done

echo "$pass passed, $fail failed, $skip skipped (not for the $build build)"
[ $fail = 0 ]
