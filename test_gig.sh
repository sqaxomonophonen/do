#!/usr/bin/env bash
set -e
cc -O0 -g -Wall stb_divide.c stb_ds.c stb_sprintf.c utf8.c path.c mie.c io.c args.c allocator.c gig.c test_gig.c -o _test_gig -lm
DIR=__gigtestdir
rm -rf $DIR
mkdir $DIR
./_test_gig $DIR
