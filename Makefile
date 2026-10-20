CC ?= gcc
CFLAGS ?= -std=c11 -Wall -Wextra -Werror -O2 -Iinclude -Isrc
SAN ?= -std=c11 -Wall -Wextra -Werror -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -Iinclude -Isrc

LIB_SRC = src/kiln.c src/codec.c src/json.c src/schema_json.c
GEN = tests/generated/point.c tests/generated/message.c

.PHONY: all test clean bench generate

all: build/libkiln.a build/gld-kilngen-c build/kiln-tests build/kiln-bench build/example

build:
	mkdir -p build

build/libkiln.a: $(LIB_SRC) include/kiln.h | build
	$(CC) $(CFLAGS) -c src/kiln.c -o build/kiln.o
	$(CC) $(CFLAGS) -c src/codec.c -o build/codec.o
	$(CC) $(CFLAGS) -c src/json.c -o build/json.o
	$(CC) $(CFLAGS) -c src/schema_json.c -o build/schema_json.o
	ar rcs $@ build/kiln.o build/codec.o build/json.o build/schema_json.o

build/libkiln-san.a: $(LIB_SRC) include/kiln.h | build
	$(CC) $(SAN) -c src/kiln.c -o build/kiln.san.o
	$(CC) $(SAN) -c src/codec.c -o build/codec.san.o
	$(CC) $(SAN) -c src/json.c -o build/json.san.o
	$(CC) $(SAN) -c src/schema_json.c -o build/schema_json.san.o
	ar rcs $@ build/kiln.san.o build/codec.san.o build/json.san.o build/schema_json.san.o

build/gld-kilngen-c: src/kilngen.c src/json.c src/json.h include/kiln.h | build
	$(CC) $(CFLAGS) -o $@ src/kilngen.c src/json.c

build/kiln-tests: tests/test_v2_cases.c build/libkiln-san.a | build
	$(CC) $(SAN) -o $@ tests/test_v2_cases.c -Lbuild -lkiln-san

build/test-generated: build/gld-kilngen-c build/libkiln-san.a tests/test_generated.c testdata/schema/point.json testdata/schema/message.json
	mkdir -p tests/generated
	./build/gld-kilngen-c --schema testdata/schema/point.json --out tests/generated
	./build/gld-kilngen-c --schema testdata/schema/message.json --out tests/generated
	$(CC) $(SAN) -Itests/generated -o $@ tests/test_generated.c tests/generated/point.c tests/generated/message.c -Lbuild -lkiln-san

build/kiln-bench: benches/bench.c build/libkiln.a | build
	$(CC) $(CFLAGS) -o $@ benches/bench.c -Lbuild -lkiln

build/example: examples/main.c build/libkiln.a | build
	$(CC) $(CFLAGS) -o $@ examples/main.c -Lbuild -lkiln

generate: build/gld-kilngen-c
	mkdir -p tests/generated
	./build/gld-kilngen-c --schema testdata/schema/point.json --out tests/generated
	./build/gld-kilngen-c --schema testdata/schema/message.json --out tests/generated

test: build/kiln-tests build/test-generated
	./build/kiln-tests
	./build/test-generated
	python3 tests/test_oracle.py

bench: build/kiln-bench
	./build/kiln-bench

clean:
	rm -rf build
