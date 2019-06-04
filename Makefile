box: box.c
	gcc -g -Wall -pedantic box.c -o box

install: box
	cp box /usr/bin/box
