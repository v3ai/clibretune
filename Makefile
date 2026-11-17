# Thin POSIX wrapper around build.sh (all logic lives in build.sh/tools/).
all:
	./build.sh app

test:
	./build.sh test

clean:
	./build.sh clean

# full portability gate (UNIVERSAL_C.md): all compilers/standards/platforms
check:
	tools/check_universal.sh

check-quick:
	tools/check_universal.sh --quick

strict:
	STRICT=1 ./build.sh test
	STRICT=23 ./build.sh test

.PHONY: all test clean check check-quick strict
