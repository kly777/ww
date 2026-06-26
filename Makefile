.PHONY: all clean dev release fmt test test-quick test-one

CXX      := g++
WINDRES  := windres
CXXFLAGS := -static -O3 -s -DRELEASE
LIBS     := -lole32 -luuid -lshell32 -lgdi32 -lmsimg32
SRC      := main.cpp snapshot_manager.cpp
BUILDDIR := build
RES      := version.rc

all: release

$(BUILDDIR):
	mkdir -p $(BUILDDIR)

$(BUILDDIR)/version.o: $(RES) version.h | $(BUILDDIR)
	$(WINDRES) -c 65001 $< $@

$(BUILDDIR)/ww_dev.exe: $(SRC) version.h | $(BUILDDIR)
	$(CXX) -std=c++23 -static -o $@ $(SRC) $(LIBS)

$(BUILDDIR)/ww.exe: $(SRC) $(BUILDDIR)/version.o version.h | $(BUILDDIR)
	$(CXX) -std=c++23 $(CXXFLAGS) -mwindows -o $@ $(SRC) $(BUILDDIR)/version.o $(LIBS)

test: $(BUILDDIR)/ww_dev.exe
	cd test && uv run pytest -v

test-quick: $(BUILDDIR)/ww_dev.exe
	cd test && uv run pytest -q

test-one: $(BUILDDIR)/ww_dev.exe
	cd test && uv run pytest -v -k "$(TEST)"

dev: $(BUILDDIR)/ww_dev.exe
	./$(BUILDDIR)/ww_dev.exe

release: $(BUILDDIR)/ww.exe

clean:
	rm -rf $(BUILDDIR)

fmt:
	clang-format -i $(SRC)
