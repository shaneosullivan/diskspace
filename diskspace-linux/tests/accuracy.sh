#!/bin/bash
# What the scanner claims, checked against what the filesystem says.
#
# Every case here is one the scanner could get wrong quietly - a hard link
# counted twice, a sparse file counted at its apparent size, a symlink followed
# out of the tree - where the number still looks plausible. The point is that a
# wrong answer fails loudly rather than being believed.
#
# Must run on a real filesystem. Under WSL that means somewhere in $HOME: a tree
# built on /mnt/c reports every sparse file as fully allocated and gives no inode
# identity, so half of this would fail for reasons that have nothing to do with
# the scanner.

set -u

DISKSCAN=${DISKSCAN:-$HOME/build/diskspace/diskscan}
ROOT=${TMPDIR:-/tmp}/diskspace-accuracy
PASS=0
FAIL=0

pass() { printf '  \033[32mPASS\033[0m  %-34s %s\n' "$1" "$2"; PASS=$((PASS + 1)); }
fail() { printf '  \033[31mFAIL\033[0m  %-34s %s\n' "$1" "$2"; FAIL=$((FAIL + 1)); }

check() { # name expected actual
  if [ "$2" = "$3" ]; then pass "$1" "$3"; else fail "$1" "got $3, expected $2"; fi
}

if [ ! -x "$DISKSCAN" ]; then
  echo "no diskscan at $DISKSCAN - build it first, or set DISKSCAN" >&2
  exit 2
fi

case "$(findmnt -no FSTYPE -T "${TMPDIR:-/tmp}" 2>/dev/null)" in
  9p|drvfs|cifs|"")
    echo "refusing to run on $(findmnt -no FSTYPE -T "${TMPDIR:-/tmp}"): sparse files and" >&2
    echo "inode identity are not reported faithfully there. Set TMPDIR somewhere real." >&2
    exit 2
    ;;
esac

# ---- the tree ---------------------------------------------------------------

rm -rf "$ROOT"
mkdir -p "$ROOT"/{plain,links,sparse,deep,odd}

# Six files of known size, each a whole number of blocks.
for i in 1 2 3 4 5 6; do
  dd if=/dev/zero of="$ROOT/plain/file$i.bin" bs=4096 count=$((i * 2)) status=none
done

# One file with three names. Two of them are extra links that must not be
# counted again.
dd if=/dev/zero of="$ROOT/links/original.bin" bs=4096 count=25 status=none
ln "$ROOT/links/original.bin" "$ROOT/links/second.bin"
ln "$ROOT/links/original.bin" "$ROOT/links/third.bin"

# A file that claims 100 MB and occupies nothing.
truncate -s 100M "$ROOT/sparse/hole.bin"

# A symlink pointing outside the tree entirely. Following it would pull in all
# of /usr; it must be recorded and left alone.
ln -s /usr "$ROOT/links/escape"

# A directory symlink loop. Following it would never terminate.
ln -s "$ROOT/deep" "$ROOT/deep/self"

# A name that is not valid UTF-8, which no Windows path can be and every Linux
# scanner meets eventually.
touch "$ROOT/odd/$(printf 'bad\xff\xfename')"

# A deep path, past the 260 characters that would end a naive Windows walk.
DEEP="$ROOT/deep"
for i in $(seq 1 30); do
  DEEP="$DEEP/directory-level-$i"
done
mkdir -p "$DEEP"
dd if=/dev/zero of="$DEEP/bottom.bin" bs=4096 count=1 status=none

CLUSTER=$(stat -f -c %S "$ROOT")
echo "tree built at $ROOT (block size $CLUSTER bytes)"
echo

# ---- what the filesystem says -----------------------------------------------

# du counts a directory's own blocks; the scanner reports only what is inside
# them, so the comparison has to subtract them.
du_bytes() { du -sx --block-size=1 "$1" | cut -f1; }
dir_bytes() { find "$1" -xdev -type d -printf '%b\n' | awk '{s+=$1} END {print s*512+0}'; }

EXPECTED_FILES=$(( $(du_bytes "$ROOT") - $(dir_bytes "$ROOT") ))

raw() { # key
  "$DISKSCAN" scan "$ROOT" --quiet --raw 2>/dev/null | sed -n "s/^$1=//p"
}

echo "counting hard links once"
ALLOCATED=$(raw allocated)
check "total matches du, less directories" "$EXPECTED_FILES" "$ALLOCATED"

DUPES=$(raw hardlink_duplicates)
check "extra hard links found" "2" "$DUPES"

DUPE_BYTES=$(raw hardlink_duplicate_bytes)
check "bytes not counted twice" "$((25 * 4096 * 2))" "$DUPE_BYTES"

# The sparse file must contribute essentially nothing, against a logical size of
# 100 MB. Anything over one block means its holes were counted.
SPARSE_ALLOC=$("$DISKSCAN" scan "$ROOT/sparse" --quiet --raw 2>/dev/null | sed -n 's/^allocated=//p')
if [ "$SPARSE_ALLOC" -le "$CLUSTER" ]; then
  pass "sparse file occupies almost nothing" "$SPARSE_ALLOC bytes for a 100 MB file"
else
  fail "sparse file occupies almost nothing" "$SPARSE_ALLOC bytes, expected <= $CLUSTER"
fi

SPARSE_LOGICAL=$("$DISKSCAN" scan "$ROOT/sparse" --quiet --raw 2>/dev/null | sed -n 's/^logical=//p')
check "and still reports its full size" "$((100 * 1024 * 1024))" "$SPARSE_LOGICAL"

echo
echo "not following what would escape or loop"
# /usr is well over a gigabyte. If the symlink was followed, the total shows it.
if [ "$ALLOCATED" -lt $((200 * 1024 * 1024)) ]; then
  pass "symlink out of the tree" "not followed"
else
  fail "symlink out of the tree" "total of $ALLOCATED suggests /usr was pulled in"
fi

# A followed directory loop does not terminate, so arriving here at all is most
# of the test.
DEEP_ENTRIES=$(raw nodes)
if [ -n "$DEEP_ENTRIES" ]; then
  pass "directory symlink loop" "terminated, $DEEP_ENTRIES entries"
else
  fail "directory symlink loop" "no result"
fi

echo
echo "names and depths"
UTF8_OK=$("$DISKSCAN" scan "$ROOT/odd" --quiet --raw 2>/dev/null | sed -n 's/^nodes=//p')
check "a name that is not valid UTF-8" "2" "$UTF8_OK"

DEEP_ALLOC=$("$DISKSCAN" scan "$ROOT/deep" --quiet --raw 2>/dev/null | sed -n 's/^allocated=//p')
if [ "$DEEP_ALLOC" -ge "$CLUSTER" ]; then
  pass "a path over 260 characters" "reached, $DEEP_ALLOC bytes"
else
  fail "a path over 260 characters" "got $DEEP_ALLOC, expected at least $CLUSTER"
fi

echo
echo "counting every link (--no-hardlinks)"
ALL_LINKS=$("$DISKSCAN" scan "$ROOT" --quiet --raw --no-hardlinks 2>/dev/null | sed -n 's/^allocated=//p')
check "total with the links counted" "$((EXPECTED_FILES + 25 * 4096 * 2))" "$ALL_LINKS"

echo
echo "mount points"
ROOT_IS_VOLUME=$("$DISKSCAN" check / 2>/dev/null | sed -n 's/^volume_root=//p')
check "/ is a whole volume" "1" "$ROOT_IS_VOLUME"
SUB_IS_VOLUME=$("$DISKSCAN" check "$ROOT" 2>/dev/null | sed -n 's/^volume_root=//p')
check "a folder on it is not" "0" "$SUB_IS_VOLUME"

echo
echo "undoing a delete"
# Run against the tree this suite just built, which has hard links, a sparse
# file, a deep path and a name that is not valid UTF-8 in it - so the check is
# not against something conveniently simple.
REVERSIBILITY=${REVERSIBILITY:-$(dirname "$DISKSCAN")/test-reversibility}
if [ -x "$REVERSIBILITY" ]; then
  if "$REVERSIBILITY" "$ROOT" >/tmp/reversibility.log 2>&1; then
    pass "removal and restore are exact inverses" "every figure"
  else
    fail "removal and restore are exact inverses" "see /tmp/reversibility.log"
  fi
else
  fail "removal and restore are exact inverses" "test-reversibility not built"
fi

echo
if [ "$FAIL" -eq 0 ]; then
  echo "all $PASS checks passed"
  rm -rf "$ROOT"
  exit 0
fi
echo "$FAIL of $((PASS + FAIL)) checks failed - tree left at $ROOT"
exit 1
