# Thin POSIX wrapper around build.sh
all:
	./build.sh app

test:
	./build.sh test

clean:
	./build.sh clean

strict:
	STRICT=1 ./build.sh test
	STRICT=23 ./build.sh test

.PHONY: all test clean strict
