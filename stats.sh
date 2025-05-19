#!/usr/bin/env bash

filter() {
	grep -vF stb_ | grep -vF doc/ | grep -vF font0.c | grep -vF .gitignore
}

echo -n "lines of code: "
cat $(git ls-files | filter) | wc -l
echo -n "asserts: "
git grep -nw assert | filter | wc -l
echo -n "bounds checked array accesses: "
git grep -nF arrchk | filter | wc -l
echo -n "XXXs: "
git grep -nw XXX | filter | wc -l
echo -n "FIXMEs: "
git grep -nw FIXME | filter | wc -l
echo -n "TODOs: "
git grep -nw TODO | filter | wc -l
