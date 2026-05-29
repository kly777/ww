.PHONY: all clean dev release fmt

CXX      := g++
CXXFLAGS := -static -O3 -s -DRELEASE
LIBS     := -lole32 -luuid -lshell32 -lgdi32
SRC      := main.cpp
BUILDDIR := build


all: release

$(BUILDDIR):
	mkdir -p $(BUILDDIR)

$(BUILDDIR)/ww_dev.exe: $(SRC) | $(BUILDDIR)
	$(CXX) -std=c++17 -static -o $@ $(SRC) $(LIBS)

$(BUILDDIR)/ww.exe: $(SRC) | $(BUILDDIR)
	$(CXX) -std=c++17 $(CXXFLAGS) -mwindows -o $@ $(SRC) $(LIBS)

dev: $(BUILDDIR)/ww_dev.exe
	./$(BUILDDIR)/ww_dev.exe

release: $(BUILDDIR)/ww.exe

clean:
	rm -rf $(BUILDDIR)

fmt:
	clang-format -i $(SRC)
