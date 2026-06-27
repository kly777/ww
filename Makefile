.PHONY: all clean dev release fmt test test-quick test-one
.DELETE_ON_ERROR:

CXX      := g++
WINDRES  := windres

# --------------- Build config ---------------
SRC       := main.cpp snap_manager.cpp
RES       := version.rc

# Common: auto-dependency generation
CPPFLAGS := -MMD -MP
LIBS     := -lole32 -luuid -lshell32 -lgdi32 -lmsimg32

# Dev build: no optimization, no RELEASE define, console mode
BUILD_DEV     := build/dev
OBJS_DEV      := $(SRC:%.cpp=$(BUILD_DEV)/%.o)
DEPS_DEV      := $(OBJS_DEV:.o=.d)
CXXFLAGS_DEV  := -std=c++23 -static
LDFLAGS_DEV   := -static

# Release build: optimized, RELEASE define, windows subsystem + version resource
BUILD_REL     := build/release
OBJS_REL      := $(SRC:%.cpp=$(BUILD_REL)/%.o)
DEPS_REL      := $(OBJS_REL:.o=.d)
CXXFLAGS_REL  := -std=c++23 -static -O3 -s -DRELEASE
LDFLAGS_REL   := -static -s -mwindows

all: release

# ============ Pattern rules ============

# Dev .o
$(BUILD_DEV)/%.o: %.cpp | $(BUILD_DEV)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS_DEV) -c $< -o $@

# Release .o
$(BUILD_REL)/%.o: %.cpp | $(BUILD_REL)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS_REL) -c $< -o $@

# Windres (release only)
$(BUILD_REL)/version.o: $(RES) version.h | $(BUILD_REL)
	$(WINDRES) -c 65001 $< $@

# ============ Linking ============

$(BUILD_DEV)/ww_dev.exe: $(OBJS_DEV) | $(BUILD_DEV)
	$(CXX) $(LDFLAGS_DEV) -o $@ $^ $(LIBS)

$(BUILD_REL)/ww.exe: $(OBJS_REL) $(BUILD_REL)/version.o | $(BUILD_REL)
	$(CXX) $(LDFLAGS_REL) -o $@ $^ $(LIBS)

# ============ Phony targets ============

dev: $(BUILD_DEV)/ww_dev.exe
	./$(BUILD_DEV)/ww_dev.exe

release: $(BUILD_REL)/ww.exe

test: $(BUILD_DEV)/ww_dev.exe
	cd test && uv run pytest -v

test-quick: $(BUILD_DEV)/ww_dev.exe
	cd test && uv run pytest -q

test-one: $(BUILD_DEV)/ww_dev.exe
	cd test && uv run pytest -v -k "$(TEST)"

clean:
	$(RM) -rf build

fmt:
	clang-format -i $(SRC)

# ============ Build dir guards ============
$(BUILD_DEV):
	mkdir -p $(BUILD_DEV)

$(BUILD_REL):
	mkdir -p $(BUILD_REL)

# ============ Auto-dependency include ============
-include $(DEPS_DEV)
-include $(DEPS_REL)
