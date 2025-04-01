#!/usr/bin/env bash

blacklist() {
	grep -vF font0.c | grep -vF doc/ | grep -vF .git
}

3rd() {
	grep -F $1 stb_
}

ours() {
	3rd -v # not 3rd
}

echo
echo 3RD:
wc -l $(git ls-files | 3rd | blacklist)

echo
echo OWN:
wc -l $(git ls-files | ours | blacklist)
