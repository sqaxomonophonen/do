#!/usr/bin/env bash

filter() {
	grep -vF stb_ | grep -vF sha1. | grep -vF lonesha256. | grep -vF doc/ | grep -vF font0.c | grep -vF .gitignore
}

echo -n "lines of code (wc): "
cat $(git ls-files | filter) | wc -l

clocloc=0
if command -v cloc >/dev/null 2>&1 ; then
	clocloc=$(cloc --csv $( git ls-files '*.c' '*.h' | grep -vF stb_ | grep -vF sha1. | grep -vF lonesha256. | grep -vF doc/ | grep -vF font0.c ) | grep SUM | cut -d, -f5)
fi


if [ $clocloc -gt 0 ] ; then
	echo "lines of code (cloc): $clocloc"
fi

count_asserts() {
	git grep -nw assert | filter | wc -l
}

echo -n "asserts: "
count_asserts
#echo -n "lines per assert (wc) "
#printf "%.2f" $( echo "$(cat $(git ls-files | filter) | wc -l) / $(count_asserts)" | bc -l )
#echo
if [ $clocloc -gt 0 ] ; then
	echo -n "lines per assert (cloc): "
	printf "%.2f" $(echo "$clocloc / $(count_asserts)" | bc -l)
	echo
fi
echo -n "bounds checked array accesses: "
git grep -nF arrchk | filter | wc -l
echo -n "XXXs: "
git grep -nw XXX | filter | wc -l
echo -n "FIXMEs: "
git grep -nw FIXME | filter | wc -l
echo -n "TODOs: "
git grep -nw TODO | filter | wc -l
echo -n "sdl3/gl impl lines of code: "
cat main_sdl3gl.c impl_gl.h impl_sdl3.h | wc -l
echo -n "emscripten(web) impl lines of code: "
cat main_emscripten.c main_emscripten_pre.js do.html impl_gl.h | wc -l
