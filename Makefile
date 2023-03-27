all : cache.exe attrib.exe testintern.exe testdl.exe

cache.exe : attrib.c style.c dirtylist.c
	gcc -Wall -g -o $@ $^ -DSTYLE_TEST_MAIN

attrib.exe : attrib.c style.c dirtylist.c
	gcc -Wall -g -o $@ $^ -DATTRIB_TEST_MAIN

testintern.exe : test_intern.c style.c attrib.c dirtylist.c
	gcc -Wall -g -o $@ $^

testdl.exe : dirtylist.c style.c attrib.c
	gcc -Wall -g -o $@ $^ -DDIRTYLIST_TEST_MAIN

clean :
	rm -rf *.exe
