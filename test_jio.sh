#!/usr/bin/env bash
set -e
cc -O0 -g -Wall allocator.c stb_ds.c jio.c test_jio.c -o _test_jio
DIR=__jiotestdir
rm -rf $DIR
mkdir $DIR
$RUNNER ./_test_jio $DIR
echo OK
ls -l $DIR
# to run with gdb or gf2:
# $ RUNNER="gdb --args" ./test_jio.sh
# $ RUNNER="gf2 --args" ./test_jio.sh
