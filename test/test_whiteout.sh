#!/bin/sh
# Unit test for docker2uxc's layer flattening + AUFS whiteout handling.
# Builds two synthetic gzip tar layers and asserts the merged rootfs is correct.
#
# The flatten loop and apply_whiteouts() below MUST stay in sync with docker2uxc.
set -eu

T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
cd "$T"

# --- layer 1: base tree --------------------------------------------------------
mkdir -p L1/a L1/b L1/opqdir
echo keep > L1/a/keep.txt
echo gone > L1/a/gone.txt
echo bx   > L1/b/x.txt
echo old1 > L1/opqdir/old1
echo old2 > L1/opqdir/old2
( cd L1 && tar -czf ../layer1.tar.gz . )

# --- layer 2: delete a/gone.txt, opaque opqdir + new file, add c/ --------------
mkdir -p L2/a L2/opqdir L2/c
: > L2/a/.wh.gone.txt
: > L2/opqdir/.wh..wh..opq
echo newopq > L2/opqdir/new.txt
echo cnew   > L2/c/new.txt
( cd L2 && tar -czf ../layer2.tar.gz . )

# --- flatten (mirrors docker2uxc) ---------------------------------------------
ROOTFS="$T/rootfs"; mkdir -p "$ROOTFS"
WORK=$T

apply_whiteouts() {
	_ld=$1; _rf=$2
	find "$_ld" -name '.wh..wh..opq' 2>/dev/null | while IFS= read -r m; do
		_rel=${m#"$_ld"/}; _dir=${_rel%/.wh..wh..opq}
		[ "$_dir" = "$_rel" ] && _dir=""
		[ -d "$_rf/$_dir" ] && find "$_rf/$_dir" -mindepth 1 -maxdepth 1 -exec rm -rf {} + 2>/dev/null || true
		rm -f "$m"
	done
	find "$_ld" -name '.wh.*' ! -name '.wh..wh..opq' 2>/dev/null | while IFS= read -r m; do
		_rel=${m#"$_ld"/}; _bn=${_rel##*/}; _dir=${_rel%/*}
		[ "$_dir" = "$_rel" ] && _dir=""
		_target=${_bn#.wh.}
		rm -rf "$_rf/${_dir:+$_dir/}$_target"
		rm -f "$m"
	done
}

for bp in layer1.tar.gz layer2.tar.gz; do
	if gzip -dc "$bp" | tar -t 2>/dev/null | grep -q '\(^\|/\)\.wh\.'; then
		ldir="$WORK/layer.$$"; rm -rf "$ldir"; mkdir -p "$ldir"
		gzip -dc "$bp" | tar -x -p -C "$ldir" 2>/dev/null || true
		apply_whiteouts "$ldir" "$ROOTFS"
		cp -a "$ldir/." "$ROOTFS/" 2>/dev/null || true
		rm -rf "$ldir"
	else
		gzip -dc "$bp" | tar -x -p -C "$ROOTFS" 2>/dev/null || true
	fi
done

# --- assertions ----------------------------------------------------------------
fail=0
must_exist()  { [ -e "$ROOTFS/$1" ] || { echo "FAIL: missing $1"; fail=1; }; }
must_absent() { [ ! -e "$ROOTFS/$1" ] || { echo "FAIL: should be gone: $1"; fail=1; }; }

must_exist  a/keep.txt
must_absent a/gone.txt          # regular whiteout
must_exist  b/x.txt
must_absent opqdir/old1         # opaque dir cleared lower layer
must_absent opqdir/old2
must_exist  opqdir/new.txt      # but layer-2 content kept
must_exist  c/new.txt
must_absent a/.wh.gone.txt      # markers themselves removed
must_absent opqdir/.wh..wh..opq

if [ "$fail" -eq 0 ]; then
	echo "PASS: whiteout flattening correct"
else
	echo "rootfs tree:"; find "$ROOTFS" | sed "s#$ROOTFS#  #"
	exit 1
fi
