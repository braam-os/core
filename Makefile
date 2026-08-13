# Convenience wrapper over CMake. The build system proper is CMakeLists.txt.

BUILD     ?= build
GENERATOR ?= Ninja
TOOLCHAIN := cmake/wasm32-unknown-unknown.cmake
PORT      := 8080          # must match the serve target in CMakeLists.txt

# macOS ships `open`, most Linux desktops `xdg-open`.
BROWSER := $(firstword $(foreach c,open xdg-open,$(shell command -v $(c) 2>/dev/null)))

# Ninja does its own job control and cannot read make's jobserver pipe.
unexport MAKEFLAGS

.PHONY: all run serve clean

all: $(BUILD)/CMakeCache.txt
	@cmake --build $(BUILD)

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
	@cmake -B $(BUILD) -G $(GENERATOR) -DCMAKE_TOOLCHAIN_FILE=$(TOOLCHAIN) $(CMAKE_ARGS)
