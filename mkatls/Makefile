
all: mkatls default.atls

mkatls: mkatls.c
	$(CC) -std=gnu99 -Wall -g $^ -o $@ -lm

default.atls: mkatls reserved.rects ter-u18n.bdf
	./mkatls default.atls reserved.rects main:ter-u18n.bdf

default.png: mkatls reserved.rects ter-u18n.bdf
	./mkatls default.png reserved.rects main:ter-u18n.bdf

clean:
	rm -rf mkatls default.atls
