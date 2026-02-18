.PHONY: build install test check gate-local gate-fast gate-release deploy-local clean

PREFIX ?= /usr/local
BUILD_DIR ?= build

build:
	cmake -S . -B $(BUILD_DIR) -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_INSTALL_PREFIX=$(PREFIX)
	cmake --build $(BUILD_DIR)

install: build
	cmake --install $(BUILD_DIR)

test:
	ctest --test-dir $(BUILD_DIR) --output-on-failure

check: test

gate-local:
	./scripts/gate-local.sh

gate-fast:
	./scripts/gate-local.sh --quick

gate-release:
	STRICT_DAEMON_SMOKE=1 ./scripts/gate-local.sh --aur-smoke

deploy-local:
	STRICT_DAEMON_SMOKE=1 ./scripts/gate-local.sh --deploy-local

clean:
	rm -rf $(BUILD_DIR)
