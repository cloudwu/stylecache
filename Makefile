all : cache.exe attrib.exe testintern.exe

cache.exe : attrib.c style.c
	gcc -Wall -g -o $@ $^ -DSTYLE_TEST_MAIN

attrib.exe : attrib.c style.c
	gcc -Wall -g -o $@ $^ -DATTRIB_TEST_MAIN

testintern.exe : test_intern.c style.c attrib.c
	gcc -Wall -g -o $@ $^

clean :
	rm -rf cache.exe attrib.exe testintern.exe
