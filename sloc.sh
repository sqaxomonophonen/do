#!/usr/bin/env bash
echo
echo ALL:
wc -l $(git ls-files)
echo
echo OWN:
wc -l $(git ls-files | grep -vF stb_ | grep -vF font0.c )
