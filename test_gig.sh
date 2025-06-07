#!/usr/bin/env bash
set -e
cc -O0 -g -Wall \
	stb_divide.c stb_ds.c stb_sprintf.c \
	allocator.c utf8.c path.c arg.c \
	mie.c \
	io.c \
	bufstream.c \
	jio.c \
	gig.c \
	test_gig.c \
	-o _test_gig \
	-lm
DIR=__gigtestbasedir
rm -rf $DIR
mkdir $DIR
$RUNNER ./_test_gig $DIR
#ls -lR $DIR
#ls -l $(find $DIR -type f)
# to run with gdb/gf2 or valgrind:
# $ RUNNER="gdb --args" ./test_gig.sh
# $ RUNNER="gf2 --args" ./test_gig.sh
# $ RUNNER="valgrind" ./test_gig.sh
