.PHONY: all clean dev release fmt

CXX      := g++
WINDRES  := windres
CXXFLAGS := -static -O3 -s -DRELEASE
LIBS     := -lole32 -luuid -lshell32 -lgdi32
SRC      := main.cpp
BUILDDIR := build
RES      := version.rc

all: release

$(BUILDDIR):
	mkdir -p $(BUILDDIR)

$(BUILDDIR)/version.o: $(RES) version.h | $(BUILDDIR)
	$(WINDRES) -c 65001 $< $@

$(BUILDDIR)/ww_dev.exe: $(SRC) version.h | $(BUILDDIR)
	$(CXX) -std=c++17 -static -o $@ $(SRC) $(LIBS)

$(BUILDDIR)/ww.exe: $(SRC) $(BUILDDIR)/version.o version.h | $(BUILDDIR)
	$(CXX) -std=c++17 $(CXXFLAGS) -mwindows -o $@ $(SRC) $(BUILDDIR)/version.o $(LIBS)

test: $(BUILDDIR)/ww.exe
	uv run pytest -v

dev: $(BUILDDIR)/ww_dev.exe
	./$(BUILDDIR)/ww_dev.exe

release: $(BUILDDIR)/ww.exe

clean:
	rm -rf $(BUILDDIR)

fmt:
	clang-format -i $(SRC)
