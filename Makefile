# Convenience wrapper over CMake. The build system proper is CMakeLists.txt.
# Override with GENERATOR=Ninja, JOBS=1, BUILD=<dir>.

BUILD     ?= build
GENERATOR ?= Unix Makefiles
TOOLCHAIN := cmake/wasm32-unknown-unknown.cmake
PORT      := 8080          # must match the serve target in CMakeLists.txt

# make's own -jN cannot reach the generated build: its jobserver descriptors do
# not survive the cmake process in between. Pass a count explicitly instead.
JOBS ?= $(shell sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)

# macOS ships `open`, most Linux desktops `xdg-open`.
BROWSER := $(firstword $(foreach c,open xdg-open,$(shell command -v $(c) 2>/dev/null)))

.PHONY: all run serve clean

all: $(BUILD)/CMakeCache.txt
	@cmake --build $(BUILD) -j $(JOBS)

run: all
	@ctest --test-dir $(BUILD) --output-on-failure

serve: all
ifeq ($(BROWSER),)
	@echo "no browser opener found; visit http://localhost:$(PORT)/ yourself"
else
	@( sleep 1; $(BROWSER) http://localhost:$(PORT)/ ) &
endif
	@cmake --build $(BUILD) --target serve

clean:
	@rm -rf $(BUILD)

$(BUILD)/CMakeCache.txt:
	@cmake -B $(BUILD) -G "$(GENERATOR)" -DCMAKE_TOOLCHAIN_FILE=$(TOOLCHAIN) $(CMAKE_ARGS)
