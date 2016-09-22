#!/bin/sh
if [ -z "$1" ] ; then
	echo "usage: $0 <input.xcf> [input.xcf]..."
	exit 1
fi
cat gimp2tblspec.scm | gimp -nci -b - $*
