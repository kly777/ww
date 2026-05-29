.PHONY: fmt run clean release

ww_dev.exe: main.cpp
	g++ -o ww_dev.exe main.cpp -lole32 -luuid -lshell32 -lgdi32

release:
	g++ -mwindows -DRELEASE -O2 -s -o ww.exe main.cpp -lole32 -luuid -lshell32 -lgdi32

clean:
	rm -f ww_dev.exe ww.exe

dev: ww_dev.exe
	./ww_dev.exe

fmt:
	clang-format -i main.cpp
