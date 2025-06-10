#!/usr/bin/env bash
set -e
cd $(dirname $0)

# NOTE add new build sources here:
files=$( ls *.c *.h *.js *.css Makefile.emscripten MKinclude.* webpack.bash )

src_root=$(pwd)

buildname="do_web_build"
toolchain="upstream/emscripten"

cclean=$(which cclean || true)

if [ "$2" = "" ] ; then
	echo "Usage: $0 <newroot> <emsdk>"
	echo "Purpose is to allow emscripten builds on FreeBSD via Linux chroot (using debootstrap). Other setups not tested."
	echo "It chroots to <newroot> to build; <newroot> must have GNU Make installed."
	echo "<emsdk> must point at the emsdk root path under and relative to <newroot>"
	echo "emsdk must have been used to install+activate an emscripten version, which must be available at <emsdk>/$toolschain"
	echo "Copies source files to <newroot>/$buildname"
	echo "If you have cclean (https://github.com/sqaxomonophonen/cclean) it'll be used to delete .o files that should be rebuilt"
	echo "Copies artifacts back to source directory and chowns them to the owner of that directory."
	#echo "build sources: $files"
	exit 1
fi

newroot="$1"
emsdk="$2"

if [ ! -d "$newroot" ] ; then
	echo "$newroot: not a directory"
	exit 1
fi

if [ ! -d "$newroot/$emsdk" ] ; then
	echo "$newroot/$emsdk: not a directory"
	exit 1
fi

if [ ! -d "$newroot/$emsdk" ] ; then
	echo "$newroot/$emsdk: not a directory"
	exit 1
fi

if [ ! -x "$newroot/$emsdk/$toolchain/emcc" ] ; then
	echo "$newroot/$emsdk/$toolchain/emcc: not an executable (maybe you have to install an emscripten version using emsdk)"
	exit 1
fi

mkdir -p $newroot/$buildname

for src in $files ; do
	dst="$newroot/$buildname/$src"
	cmd=""
	if [ ! -f "$dst" ] ; then
		cmd="N"
	elif [ "$src" -nt "$dst" ] ; then
		cmd="U"
	fi
	if [ -z "$cmd" ] ; then
		continue
	fi
	echo "$cmd $src => $dst"
	cp $src $dst
done

if [ -n "$cclean" ] ; then
	pushd $newroot/$buildname
	$cclean -mx
	popd
fi
chroot $newroot /bin/bash -c "PATH=\"/$emsdk/$toolchain:\$PATH\" /$buildname/webpack.bash"

# this script expects to be run as root, so we don't know what owner the
# artifact files should have when copied back, however a good guess is the
# owner of the source directory:
mkdir -p dok
cp -a $newroot/$buildname/dok/* dok/
cp $newroot/$buildname/webpack.gen.h .

chown -R $(stat -f"%Su:%Sg" $src_root) dok/ webpack.gen.h # XXX stat usage is FreeBSD specific

echo OK
