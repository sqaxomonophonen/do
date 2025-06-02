#!/usr/bin/env bash

filter() {
	grep -vF stb_ | grep -vF sha1. | grep -vF lonesha256. | grep -vF doc/ | grep -vF font0.c | grep -vF .gitignore
}

echo -n "lines of code: "
cat $(git ls-files | filter) | wc -l
echo -n "asserts: "
git grep -nw assert | filter | wc -l
echo "lines per assert $(( $(cat $(git ls-files | filter) | wc -l) / $(git grep -nw assert | filter | wc -l)  ))"
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
