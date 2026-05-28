.PHONY: fmt run clean



ww.exe: main.cpp
	g++ -o ww.exe main.cpp -ldwmapi -lole32 -luuid

clean:
	rm -f ww.exe

run: ww.exe
	./ww.exe

fmt:
	clang-format -i main.cpp
