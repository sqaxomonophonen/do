#!/usr/bin/env bash
set -e
cc -O0 -g -Wall \
	stb_divide.c stb_ds.c stb_sprintf.c \
	allocator.c utf8.c path.c arg.c \
	mie.c \
	io.c \
	gig.c \
	test_gig.c \
	-o _test_gig \
	-lm
DIR=__gigtestdir
rm -rf $DIR
mkdir $DIR
$RUNNER ./_test_gig $DIR
# to run with gdb or gf2:
# $ RUNNER="gdb --args" ./test_gig.sh
# $ RUNNER="gf2 --args" ./test_gig.sh
