# Convenience wrapper over CMake. The build system proper is CMakeLists.txt.
# Override with GENERATOR=Ninja, JOBS=1, BUILD=<dir>.

BUILD     ?= build
GENERATOR ?= Unix Makefiles
TOOLCHAIN := cmake/wasm32-unknown-unknown.cmake
# No trailing comment here: make keeps the space before a `#`, and the port
# would end up padded. Must match the serve target in CMakeLists.txt.
PORT      := 8080
# The chat demo's WebSocket server, started alongside `serve`.
WS_PORT   := 8081

# make's own -jN cannot reach the generated build: its jobserver descriptors do
# not survive the cmake process in between. Pass a count explicitly instead.
JOBS ?= $(shell sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)

# macOS ships `open`, most Linux desktops `xdg-open`.
BROWSER := $(firstword $(foreach c,open xdg-open,$(shell command -v $(c) 2>/dev/null)))

.PHONY: all run serve wsd release clean

all: $(BUILD)/CMakeCache.txt
	@cmake --build $(BUILD) -j $(JOBS)

run: all
	@ctest --test-dir $(BUILD) --output-on-failure

serve: all
ifeq ($(BROWSER),)
	@echo "no browser opener found; visit http://localhost:$(PORT)/ yourself"
else
	@( sleep 1; $(BROWSER) "http://localhost:$(PORT)/" ) &
endif
	@BRAAM_WS_PORT=$(WS_PORT) node tools/wsd.mjs & \
	 trap "kill $$! 2>/dev/null" EXIT INT TERM; \
	 cmake --build $(BUILD) --target serve

# build/web/ as a zip, to unpack on a web server. Pack a clean tree: the web
# target copies into build/web/ and never deletes from it.
release: all
	@cmake --build $(BUILD) --target release

# The chat server on its own, for a browser that is already open.
wsd:
	@BRAAM_WS_PORT=$(WS_PORT) node tools/wsd.mjs

clean:
	@rm -rf $(BUILD)

$(BUILD)/CMakeCache.txt:
	@cmake -B $(BUILD) -G "$(GENERATOR)" -DCMAKE_TOOLCHAIN_FILE=$(TOOLCHAIN) $(CMAKE_ARGS)
