# Convenience wrapper around the CMake build described in README.md.

CMAKE     ?= cmake
BUILD_DIR ?= build
JOBS      ?= $(shell nproc 2>/dev/null || echo 2)

.PHONY: all configure build windows clean

all: build

configure:
	$(CMAKE) -S . -B $(BUILD_DIR)

build: configure
	$(CMAKE) --build $(BUILD_DIR) -j$(JOBS)

# Cross-compile the Windows package (needs mingw-w64, cmake, curl and zip).
windows:
	./tools/build-windows.sh

clean:
	rm -rf $(BUILD_DIR)
