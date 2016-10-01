
all: mkatls default.atls

mkatls: mkatls.c
	$(CC) -std=gnu99 -Wall -g $^ -o $@ -lm

box2.tbl: box2.xcf
	./gimp2tblspec.sh $^

DEFAULT_DEPS=mkatls lsl_reserved.rects ter-u18n.bdf ter-u12n.bdf box2.tbl default.cols
DEFAULT_ARGS=lsl_reserved.rects main:ter-u18n.bdf subs:ter-u12n.bdf box2.tbl default.cols

default.atls: $(DEFAULT_DEPS)
	./mkatls default.atls $(DEFAULT_ARGS)

default.png: $(DEFAULT_DEPS)
	./mkatls default.png $(DEFAULT_ARGS)

clean:
	rm -rf mkatls default.atls default.png box2.tbl
