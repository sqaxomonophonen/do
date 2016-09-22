
all: mkatls default.atls

mkatls: mkatls.c
	$(CC) -std=gnu99 -Wall -g $^ -o $@ -lm

box.tbl: box.xcf
	./gimp2tblspec.sh $^

default.atls: mkatls lsl_reserved.rects ter-u18n.bdf box.tbl
	./mkatls default.atls lsl_reserved.rects main:ter-u18n.bdf box.tbl

default.png: mkatls lsl_reserved.rects ter-u18n.bdf box.tbl
	./mkatls default.png lsl_reserved.rects main:ter-u18n.bdf box.tbl

clean:
	rm -rf mkatls default.atls default.png box.tbl
