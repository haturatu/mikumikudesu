SHELL := /bin/bash

.DEFAULT_GOAL := build

# ---------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------

PRESET ?= linux-debug
BUILD_DIR := build/$(PRESET)
BINARY := $(BUILD_DIR)/mikumikudesu

JOBS ?=
ARGS ?=
CMAKE_ARGS ?=
PREFIX ?= $(CURDIR)/install

TIDY_JOBS ?= 2

.PHONY: \
	help all configure build \
	debug release sanitize system \
	test run probe amd-check ci \
	format format-check shellcheck lint tidy \
	compdb install clean distclean presets

# ---------------------------------------------------------------------
# Build
# ---------------------------------------------------------------------

all: build

configure:
	cmake --preset "$(PRESET)" $(CMAKE_ARGS)

build: configure
	cmake --build --preset "$(PRESET)" --parallel $(JOBS)

debug:
	@$(MAKE) PRESET=linux-debug build

release:
	@$(MAKE) PRESET=linux-release build

sanitize:
	@$(MAKE) PRESET=linux-sanitize build

system:
	@$(MAKE) PRESET=linux-system-only build

# ---------------------------------------------------------------------
# Test / run
# ---------------------------------------------------------------------

test: build
	ctest --preset "$(PRESET)" --output-on-failure

run: build
	"./$(BINARY)" $(ARGS)

probe: build
	"./$(BINARY)" --probe --hidden $(ARGS)

amd-check:
	./scripts/check-linux-amd.sh

# Roughly mirrors the normal GitHub Actions build/test path.
ci:
	cmake --preset "$(PRESET)" \
		-DDAYO_WARNINGS_AS_ERRORS=ON \
		$(CMAKE_ARGS)
	cmake --build --preset "$(PRESET)" --parallel $(JOBS)
	ctest --preset "$(PRESET)" --output-on-failure

# ---------------------------------------------------------------------
# Development tools
# ---------------------------------------------------------------------

format:
	find src tests -type f \
		\( -name '*.cpp' -o -name '*.hpp' -o -name '*.h' \) \
		-print0 | xargs -0 -r clang-format -i
	gersemi --in-place \
		--definitions cmake/CompileHlsl.cmake -- .
	find scripts tests -type f -name '*.sh' \
		-print0 | xargs -0 -r shfmt -w -ln bash -i 2 -ci

format-check:
	find src tests -type f \
		\( -name '*.cpp' -o -name '*.hpp' -o -name '*.h' \) \
		-print0 | xargs -0 -r clang-format --dry-run --Werror
	gersemi --check --warnings-as-errors \
		--definitions cmake/CompileHlsl.cmake -- .
	find scripts tests -type f -name '*.sh' \
		-print0 | xargs -0 -r shfmt -ln bash -d -i 2 -ci

shellcheck:
	find scripts tests -type f -name '*.sh' \
		-print0 | xargs -0 -r shellcheck --severity=warning

lint: format-check shellcheck

tidy: configure
	@mapfile -t sources < <(find src tests -type f -name '*.cpp' -print); \
	run-clang-tidy \
		-p "$(BUILD_DIR)" \
		-j "$(TIDY_JOBS)" \
		-header-filter '(^|/)(src|tests)/' \
		-warnings-as-errors '*' \
		"$${sources[@]}"

# clangd / editors
compdb: configure
	ln -sfn "$(BUILD_DIR)/compile_commands.json" compile_commands.json

# ---------------------------------------------------------------------
# Install / cleanup
# ---------------------------------------------------------------------

install: build
	cmake --install "$(BUILD_DIR)" --prefix "$(PREFIX)"

clean:
	@if [[ -d "$(BUILD_DIR)" ]]; then \
		cmake --build --preset "$(PRESET)" --target clean; \
	fi

distclean:
	rm -rf build

presets:
	cmake --list-presets=all

# ---------------------------------------------------------------------
# Help
# ---------------------------------------------------------------------

help:
	@printf '%s\n' \
		'Usage: make [target] [VARIABLE=value]' \
		'' \
		'Build:' \
		'  build       Configure and build PRESET (default: linux-debug)' \
		'  debug       Build linux-debug' \
		'  release     Build linux-release' \
		'  sanitize    Build linux-sanitize' \
		'  system      Build linux-system-only' \
		'' \
		'Test / run:' \
		'  test        Build and run CTest' \
		'  run         Build and run mikumikudesu' \
		'  probe       Run Vulkan feature probe' \
		'  amd-check   Run full Linux/AMD smoke checks' \
		'  ci          Build/test with warnings-as-errors' \
		'' \
		'Development:' \
		'  format      Format C++, CMake, and shell files' \
		'  lint        Run formatter checks and ShellCheck' \
		'  tidy        Run clang-tidy' \
		'  compdb      Link compile_commands.json for clangd' \
		'' \
		'Other:' \
		'  install     Install into PREFIX (default: ./install)' \
		'  clean       Clean selected preset' \
		'  distclean   Remove all build directories' \
		'  presets     Show CMake presets' \
		'' \
		'Examples:' \
		'  make -j8' \
		'  make release JOBS=16' \
		'  make run ARGS="--asset model.pmx"' \
		'  make PRESET=linux-debug test' \
		'  CC=clang CXX=clang++ make tidy'
