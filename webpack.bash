#!/usr/bin/env bash
set -e
cd $(dirname $0)
which emcc || ( echo "ERROR: no emcc; Emscripten is not set up? HINT: run $ . /path/to/emsdk_env.sh" ; exit 1 )
which gmake || ( echo "ERROR: no gmake; GNU Make is not installed?" ; exit 1 )
gmake -f Makefile.emscripten clean
OPT=-O3 gmake -f Makefile.emscripten -j$(nproc)
ART="do.wasm do.js do.css"
ls -l $ART
DOK=dok
mkdir -p $DOK
GEN="webpack.gen.h"
echo "// DO NOT EDIT/COMMIT: codegen'd on $(date +%F) with: $0" > $GEN
echo "#define LIST_OF_WEBPACK_FILES \\" >> $GEN
DO_WASM="XXXXXX"
for art in $ART ; do
	rname="$art"
	if [ "$art" = "do.js" ] ; then
		cp $art _tmp_${art}
		sed -i -e "s,do.wasm,/${DO_WASM},g" _tmp_${art}
		art="_tmp_${art}"
	fi

	sum=$(sha256sum $art | cut -d\  -f1)
	ext="${art##*.}"
	dst=$DOK/${sum}.${ext}
	if [ "$art" = "do.wasm" ] ; then
		DO_WASM="$dst"
	fi
	cp $art $dst
	echo "   X(\"${rname}\",\"${sum}.${ext}\") \\" >> $GEN
done
echo >> $GEN
echo "codegen'd $GEN"
rm -f _tmp_*
gmake -f Makefile.emscripten clean
