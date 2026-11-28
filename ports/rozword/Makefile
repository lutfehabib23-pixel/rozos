# rozword — rozOS-এর ভেতরে বানানো:  make  (TCC)   বা   make CC=gcc
# ধাপ ২৬খ-৪: FreeType-এর হেডার ft/-এ সাথে দেওয়া (PDF); লাইব্রেরি librozgfx-এর ভেতরেই।
CC = tcc
CFLAGS = -O2 -Ift
rozword: rozword.c
	$(CC) $(CFLAGS) rozword.c -lrozui -lrozgfx -lz -lm -o rozword
install: rozword
	cp rozword /usr/bin/rozword
clean:
	rm -f rozword
