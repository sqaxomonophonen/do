#!/usr/bin/env bash

# XXX temporary build script hack because desktop/web are built from the same
# source, but with very different configuration (#defines, etc) and artifacts
# (with *.o "collisions"), and I currently have to build both often in quick
# succession, so doing 2 full rebuilds is kinda slow. a solution could be to
# have different artifact/output directories, but then I'd have to "fix"
# cclean..

# .. besides, I think I want to migrate to a tsoding/nob.h-like solution at
# some point, but there's a bit of work in it:
#  - I want cclean integration
#  - nob.h has no process pool (only full sync/async), and do is getting big
#    enough that it's probably a good idea with a $(nproc) limit?
#  - webpack.bash must also be ported (replacing sha256sum with lonesha256.h?)

set -e
cd $(dirname $0)

if [ -z "$1" ] ; then
	echo "Usage: $0 <desktop|webpack|clean>..."
	echo "WARNING: does a bunch of mkdir/rmdir/cp/mv/rm on your"
	echo "build artifacts (and hopefully nothing else)"
	exit 1
fi

APREFIX=".buildhackartifactsnapshot"

mmv() { # "maybe move"; moves a file if it exists
	if [ -e "$1" ] ; then
		mv $1 $2
	fi
}

setbuild() {
	wantbuild="$1"
	if [ ! -z "$lastbuild" -a "$lastbuild" != "$wantbuild" ] ; then
		s0="${APREFIX}-${lastbuild}"
		mkdir $s0
		mv *.o $s0/
		mmv ./do $s0/
		mmv ./do.wasm $s0/
		mmv ./do.js $s0/
		s1="${APREFIX}-${wantbuild}"
		if [ -d "$s1" ] ; then
			mv $s1/* .
			rmdir $s1
		fi
	fi
}

while true ; do

	numbin=0
	if [ -x "./do" ] ; then
		lastbuild="desktop"
		numbin=$(($numbin + 1))
	elif [ -x "./do.wasm" ] ; then
		lastbuild="webpack"
		numbin=$(($numbin + 1))
	else
		lastbuild=""
	fi

	if [ "$numbin" -ge 2 ] ; then
		echo "found multiple binaries (do, do.wasm,..) don't know what to do..."
		exit 1
	fi

	case "$1" in
	desktop)
		setbuild "$1"
		cclean -x
		time gmake -f Makefile.linux.sdl3gl -j$(nproc)
		;;
	webpack)
		setbuild "$1"
		cclean -x
		./webpack.bash
		;;
	clean)
		rm -vrf do do.wasm do.js *.o ${APREFIX}-desktop ${APREFIX}-webpack
		;;
	"")
		exit 0
		;;
	esac
	shift
done
