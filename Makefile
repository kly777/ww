.PHONY: fmt run clean

ww.exe: main.cpp
	g++ -o ww.exe main.cpp -lole32 -luuid -lshell32 -lgdi32

clean:
	rm -f ww.exe

run: ww.exe
	./ww.exe

fmt:
	clang-format -i main.cpp
