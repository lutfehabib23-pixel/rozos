# rozword — rozOS-এর ভেতরে বানানো:  make  (TCC)   বা   make CC=gcc
CC = tcc
CFLAGS = -O2
rozword: rozword.c
	$(CC) $(CFLAGS) rozword.c -lrozui -lrozgfx -lm -o rozword
install: rozword
	cp rozword /usr/bin/rozword
clean:
	rm -f rozword
