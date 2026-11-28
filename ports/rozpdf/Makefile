# rozpdf — rozOS-এর ভেতরে বানানো:  make  (TCC)   বা   make CC=gcc
# FreeType-এর হেডার ft/-এ সাথে দেওয়া; লাইব্রেরি librozgfx-এর ভেতরেই। (প্যাকেজের বাইনারিতে Type1
# ফন্টের module-ও আছে; এখানকার বিল্ডে নেই — Type1 ফন্ট তখন অন্য ফন্টে দেখায়।)
CC = tcc
CFLAGS = -O2 -Ift
SRCS = rozpdf.c pdfparse.c pdfrender.c
rozpdf: $(SRCS) pdf.h
	$(CC) $(CFLAGS) $(SRCS) -lrozui -lrozgfx -lz -lm -o rozpdf
install: rozpdf
	cp rozpdf /usr/bin/rozpdf
clean:
	rm -f rozpdf
