
all: mkatls default.atls

mkatls: mkatls.c
	$(CC) -std=gnu99 -Wall -g $^ -o $@ -lm

box.tbl: box.xcf
	./gimp2tblspec.sh $^

DEFAULT_DEPS=mkatls lsl_reserved.rects ter-u18n.bdf box.tbl # garbage.txt
DEFAULT_ARGS=lsl_reserved.rects main:ter-u18n.bdf box.tbl # @textfile:gaaaarbage:garbage.txt

default.atls: $(DEFAULT_DEPS)
	./mkatls default.atls $(DEFAULT_ARGS)

default.png: $(DEFAULT_DEPS)
	./mkatls default.png $(DEFAULT_ARGS)

clean:
	rm -rf mkatls default.atls default.png box.tbl
