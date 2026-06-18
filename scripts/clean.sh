# path: scripts/clean.sh
#!/usr/bin/env bash
#
# Safely tear everything down. Run as root for unmount/rmmod:
#   sudo ./scripts/clean.sh
#
set -euo pipefail

MODULE="u235fs"
MNT="/mnt/u235fs"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$HERE"

if [ "$(id -u)" -ne 0 ]; then
	echo "Note: not running as root; unmount/rmmod may be skipped." >&2
fi

if umount "$MNT" 2>/dev/null; then
	echo "unmounted $MNT"
else
	echo "$MNT not mounted (ok)"
fi

if rmmod "$MODULE" 2>/dev/null; then
	echo "removed module $MODULE"
else
	echo "module $MODULE not loaded (ok)"
fi

if [ -d "$MNT" ]; then
	rmdir "$MNT" 2>/dev/null && echo "removed $MNT" || true
fi

make clean >/dev/null 2>&1 || true
rm -f ./*.o ./*.ko ./*.mod ./*.mod.c modules.order Module.symvers ./.*.cmd 2>/dev/null || true
rm -f src/*.o src/*.mod* src/.*.cmd 2>/dev/null || true

echo "clean done"
