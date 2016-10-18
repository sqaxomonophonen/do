CFLAGS=-std=gnu99 -Wall -g -O0

all: mkatls default.atls

dya.o: ../dya.c ../dya.h
	$(CC) $(CFLAGS) -c $< -o $@

mkatls.o: mkatls.c
	$(CC) $(CFLAGS) -I.. -c $<

mkatls: mkatls.o dya.o
	$(CC) $^ -o $@ -lm

builtin.tbl: builtin.xcf
	./gimp2tblspec.sh $^

boxselect.tbl: boxselect.xcf
	./gimp2tblspec.sh $^

DEFAULT_DEPS=mkatls default.progs default.cols lsl_reserved.rects ter-u18n.bdf ter-u12n.bdf builtin.tbl boxselect.tbl
DEFAULT_ARGS=default.progs default.cols lsl_reserved.rects main:ter-u18n.bdf subs:ter-u12n.bdf builtin.tbl boxselect.tbl

default.atls: $(DEFAULT_DEPS)
	./mkatls default.atls $(DEFAULT_ARGS)

default.png: $(DEFAULT_DEPS)
	./mkatls default.png $(DEFAULT_ARGS)

clean:
	rm -rf mkatls default.atls default.png builtin.tbl *.o
