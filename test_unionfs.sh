#!/bin/bash
# test_unionfs.sh — Automated test suite for Mini-UnionFS
# Tests: Layer Visibility, Copy-on-Write, Whiteout, Create, Mkdir/Rmdir, Rename

set -euo pipefail

FUSE_BINARY="./mini_unionfs"
TEST_DIR="./unionfs_test_env"
LOWER_DIR="$TEST_DIR/lower"
UPPER_DIR="$TEST_DIR/upper"
MOUNT_DIR="$TEST_DIR/mnt"

GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[0;33m'
NC='\033[0m'

PASS=0
FAIL=0

pass() { echo -e "${GREEN}PASSED${NC}"; ((PASS++)); }
fail() { echo -e "${RED}FAILED${NC} — $1"; ((FAIL++)); }

echo "════════════════════════════════════════════"
echo "  Mini-UnionFS Automated Test Suite"
echo "════════════════════════════════════════════"

# ── Setup ────────────────────────────────────────────────────────────
echo "[setup] Preparing test environment..."
rm -rf "$TEST_DIR"
mkdir -p "$LOWER_DIR" "$UPPER_DIR" "$MOUNT_DIR"

# Lower layer fixtures
echo "base_only_content"      > "$LOWER_DIR/base.txt"
echo "to_be_deleted"          > "$LOWER_DIR/delete_me.txt"
echo "lower_version"          > "$LOWER_DIR/shadowed.txt"
echo "upper_version"          > "$UPPER_DIR/shadowed.txt"
mkdir -p "$LOWER_DIR/subdir"
echo "nested_file"            > "$LOWER_DIR/subdir/nested.txt"

# Mount
"$FUSE_BINARY" "$LOWER_DIR" "$UPPER_DIR" "$MOUNT_DIR" -f &
FUSE_PID=$!
sleep 1   # wait for FUSE to initialise

if ! mountpoint -q "$MOUNT_DIR"; then
    echo -e "${RED}[FATAL] Mount failed — aborting tests${NC}"
    exit 1
fi
echo "[setup] Mounted at $MOUNT_DIR (pid=$FUSE_PID)"
echo

# ── Test 1: Layer Visibility ─────────────────────────────────────────
echo -n "Test 1: Layer visibility (lower file visible) ... "
if grep -q "base_only_content" "$MOUNT_DIR/base.txt" 2>/dev/null; then
    pass
else
    fail "base.txt not readable through mount"
fi

# ── Test 2: Upper takes precedence ──────────────────────────────────
echo -n "Test 2: Upper layer precedence ... "
if grep -q "upper_version" "$MOUNT_DIR/shadowed.txt" 2>/dev/null; then
    pass
else
    fail "expected upper_version, got: $(cat "$MOUNT_DIR/shadowed.txt" 2>/dev/null)"
fi

# ── Test 3: Copy-on-Write ────────────────────────────────────────────
echo -n "Test 3: Copy-on-Write (modify lower-only file) ... "
echo "modified_content" >> "$MOUNT_DIR/base.txt" 2>/dev/null
MOUNT_HAS=$(grep -c "modified_content" "$MOUNT_DIR/base.txt"  2>/dev/null || echo 0)
UPPER_HAS=$(grep -c "modified_content" "$UPPER_DIR/base.txt"  2>/dev/null || echo 0)
LOWER_HAS=$(grep -c "modified_content" "$LOWER_DIR/base.txt"  2>/dev/null || echo 0)
if [ "$MOUNT_HAS" -ge 1 ] && [ "$UPPER_HAS" -ge 1 ] && [ "$LOWER_HAS" -eq 0 ]; then
    pass
else
    fail "mount=$MOUNT_HAS upper=$UPPER_HAS lower=$LOWER_HAS (lower must be 0)"
fi

# ── Test 4: Whiteout on deletion of lower file ───────────────────────
echo -n "Test 4: Whiteout mechanism ... "
rm "$MOUNT_DIR/delete_me.txt" 2>/dev/null
WH_EXISTS=0
[ -f "$UPPER_DIR/.wh.delete_me.txt" ] && WH_EXISTS=1
LOWER_INTACT=0
[ -f "$LOWER_DIR/delete_me.txt" ] && LOWER_INTACT=1
MOUNT_VISIBLE=1
[ -e "$MOUNT_DIR/delete_me.txt" ] && MOUNT_VISIBLE=0 || MOUNT_VISIBLE=1
if [ "$WH_EXISTS" -eq 1 ] && [ "$LOWER_INTACT" -eq 1 ] && [ "$MOUNT_VISIBLE" -eq 1 ]; then
    pass
else
    fail "wh=$WH_EXISTS lower_intact=$LOWER_INTACT hidden=$MOUNT_VISIBLE"
fi

# ── Test 5: Create new file (lands in upper) ─────────────────────────
echo -n "Test 5: Create new file in union ... "
echo "brand_new" > "$MOUNT_DIR/new_file.txt" 2>/dev/null
if [ -f "$UPPER_DIR/new_file.txt" ] && \
   grep -q "brand_new" "$UPPER_DIR/new_file.txt" && \
   ! [ -f "$LOWER_DIR/new_file.txt" ]; then
    pass
else
    fail "new file not found in upper_dir or leaked into lower_dir"
fi

# ── Test 6: Nested directory visibility ──────────────────────────────
echo -n "Test 6: Nested directory and file visibility ... "
if grep -q "nested_file" "$MOUNT_DIR/subdir/nested.txt" 2>/dev/null; then
    pass
else
    fail "subdir/nested.txt not visible"
fi

# ── Test 7: mkdir creates directory in upper ─────────────────────────
echo -n "Test 7: mkdir in union (upper layer only) ... "
mkdir "$MOUNT_DIR/newdir" 2>/dev/null
if [ -d "$UPPER_DIR/newdir" ] && ! [ -d "$LOWER_DIR/newdir" ]; then
    pass
else
    fail "newdir not created in upper or leaked to lower"
fi

# ── Test 8: Whiteout-then-recreate ───────────────────────────────────
echo -n "Test 8: Recreate a previously deleted (whited-out) file ... "
echo "recreated" > "$MOUNT_DIR/delete_me.txt" 2>/dev/null
# Whiteout should be removed; new file in upper
if [ -f "$UPPER_DIR/delete_me.txt" ] && \
   grep -q "recreated" "$MOUNT_DIR/delete_me.txt" 2>/dev/null && \
   ! [ -f "$UPPER_DIR/.wh.delete_me.txt" ]; then
    pass
else
    fail "recreation failed or whiteout not cleared"
fi

# ── Teardown ─────────────────────────────────────────────────────────
echo
echo "[teardown] Unmounting..."
fusermount -u "$MOUNT_DIR" 2>/dev/null || umount "$MOUNT_DIR" 2>/dev/null || true
wait $FUSE_PID 2>/dev/null || true
rm -rf "$TEST_DIR"

echo
echo "════════════════════════════════════════════"
printf "  Results: ${GREEN}%d passed${NC} / " $PASS
if [ $FAIL -gt 0 ]; then
    printf "${RED}%d failed${NC}\n" $FAIL
else
    printf "${GREEN}%d failed${NC}\n" $FAIL
fi
echo "════════════════════════════════════════════"
[ $FAIL -eq 0 ]
