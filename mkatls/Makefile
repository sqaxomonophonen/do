
all: mkatls default.atls

mkatls: mkatls.c
	$(CC) -std=gnu99 -Wall -g $^ -o $@ -lm

default.atls: mkatls lsl_reserved.rects ter-u18n.bdf
	./mkatls default.atls lsl_reserved.rects main:ter-u18n.bdf

default.png: mkatls lsl_reserved.rects ter-u18n.bdf
	./mkatls default.png lsl_reserved.rects main:ter-u18n.bdf

clean:
	rm -rf mkatls default.atls
