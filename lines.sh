#!/bin/sh
wc -l $(git ls-files | grep -vF doc/keyboard_layout_gallery | grep -vF font0.c )
