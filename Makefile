all : cache.exe attrib.exe

cache.exe : attrib.c style.c
	gcc -Wall -g -o $@ $^ -DSTYLE_TEST_MAIN

attrib.exe : attrib.c
	gcc -Wall -g -o $@ $^ -DATTRIB_TEST_MAIN


clean :
	rm -rf cache.exe attrib.exe
