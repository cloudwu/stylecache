all : cache.exe

cache.exe : attrib.c style.c
	gcc -Wall -g -o $@ $^ -DSTYLE_TEST_MAIN

clean :
	rm -rf cache.exe
