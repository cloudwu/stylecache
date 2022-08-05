all : cache.exe attrib.exe testintern.exe

cache.exe : attrib.c style.c
	gcc -Wall -g -o $@ $^ -DSTYLE_TEST_MAIN

attrib.exe : attrib.c
	gcc -Wall -g -o $@ $^ -DATTRIB_TEST_MAIN

testintern.exe : test_intern.c intern_cache.h
	gcc -Wall -g -o $@ $<

clean :
	rm -rf cache.exe attrib.exe testintern.exe
