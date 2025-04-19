#!/bin/sh

blacklist() {
	grep -vF font0.c | grep -vF doc/ | grep -vF .git
}

third() {
	grep -F $1 stb_
}

ours() {
	third -v # not third
}

echo
echo 3RD PARTY:
wc -l $(git ls-files | third | blacklist)

echo
echo OURS:
wc -l $(git ls-files | ours | blacklist)
