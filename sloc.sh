#!/usr/bin/env bash
allfilter() {
	grep -vF font0.c | grep -vF doc/
}
echo
echo ALL:
wc $(git ls-files | allfilter)
echo
echo OWN:
wc $(git ls-files | grep -vF stb_ | allfilter)
