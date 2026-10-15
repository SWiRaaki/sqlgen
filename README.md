# sqlgen

Generates code from SQLite schemas. It applies the schema scripts to a scratch database,
introspects the result through SQLite's own interfaces, and (in the future) emits C, C++ and more.

```sh
make            # bin/release/sqlgen; needs only a C99 compiler
make test       # unit test and golden cases
make asan       # the same under AddressSanitizer and UBSan
make CIPHER=1   # SQLCipher, for `report --key`; links -lcrypto

bin/release/sqlgen model -c path/to/sqlgen.json
```
