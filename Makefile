.PHONY: build install test clean

PREFIX ?= /usr/local
BUILD_DIR ?= build

build:
	cmake -S . -B $(BUILD_DIR) -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_INSTALL_PREFIX=$(PREFIX)
	cmake --build $(BUILD_DIR)

install: build
	cmake --install $(BUILD_DIR)

test:
	ctest --test-dir $(BUILD_DIR) --output-on-failure

clean:
	rm -rf $(BUILD_DIR)
