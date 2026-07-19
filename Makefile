# Convenience wrapper; the canonical build is CMake.
all:
	cmake -B build -DCMAKE_BUILD_TYPE=Release
	cmake --build build -j

test: all
	ctest --test-dir build --output-on-failure

clean:
	rm -rf build

.PHONY: all test clean
