# path: scripts/test.sh
#!/usr/bin/env bash
#
# Automated tests for u235fs. Run as root:  sudo ./scripts/test.sh
# Exits non-zero on the first failed check.
#
set -euo pipefail

MODULE="u235fs"
KO="u235fs.ko"
MNT="/mnt/u235fs"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$HERE"

ok()   { echo "[OK] $1"; }
fail() { echo "[FAIL] $1" >&2; exit 1; }

cleanup() {
	set +e
	umount "$MNT" 2>/dev/null
	rmmod "$MODULE" 2>/dev/null
}
trap cleanup EXIT

[ "$(id -u)" -eq 0 ] || fail "must run as root (sudo)"

# --- build / load / mount ---------------------------------------------------
make >/dev/null || fail "module build"
ok "module builds"

insmod "$KO" || fail "insmod"
ok "module loads"

mkdir -p "$MNT"
mount -t u235fs none "$MNT" || fail "mount"
ok "filesystem mounts"

[ -f "$MNT/PARAMS" ] || fail "PARAMS missing after mount"
ok "PARAMS exists"

# --- PARAMS editable before start ------------------------------------------
printf 'mass=1500\nvolume=20\ninitial_frequency=2.5\nimpurities=3\nexternal_neutrons=10\nneutron_speed=2500\n' \
	| tee "$MNT/PARAMS" >/dev/null || fail "cannot write PARAMS before start"
grep -q 'mass=1500' "$MNT/PARAMS" || fail "PARAMS not updated"
grep -q 'initial_frequency=2.5' "$MNT/PARAMS" || fail "frequency not parsed"
ok "PARAMS editable before RAM"

# --- start simulation -------------------------------------------------------
touch "$MNT/RAM"
sleep 1
for f in TEMPERATURE DECAYED_NUCLEI DECAY_RATE ELAPSED_TIME STATE; do
	[ -f "$MNT/$f" ] || fail "state file $f missing after touch RAM"
done
ok "RAM starts simulation"

# --- ELAPSED_TIME grows -----------------------------------------------------
e1="$(grep real_seconds "$MNT/ELAPSED_TIME" | cut -d= -f2)"
sleep 2
e2="$(grep real_seconds "$MNT/ELAPSED_TIME" | cut -d= -f2)"
[ "$e2" -gt "$e1" ] || fail "ELAPSED_TIME did not increase ($e1 -> $e2)"
ok "ELAPSED_TIME increased"

# --- DECAYED_NUCLEI grows ---------------------------------------------------
d1="$(grep '^decayed_nuclei' "$MNT/DECAYED_NUCLEI" | cut -d= -f2)"
sleep 2
d2="$(grep '^decayed_nuclei' "$MNT/DECAYED_NUCLEI" | cut -d= -f2)"
awk -v a="$d1" -v b="$d2" 'BEGIN{exit !((b+0) > (a+0))}' \
	|| fail "DECAYED_NUCLEI did not increase ($d1 -> $d2)"
ok "DECAYED_NUCLEI increased"

# --- DECAY_RATE computed (positive) ----------------------------------------
r="$(grep '^decay_rate_per_second' "$MNT/DECAY_RATE" | cut -d= -f2)"
awk -v x="$r" 'BEGIN{exit !((x+0) > 0)}' || fail "DECAY_RATE not positive ($r)"
ok "DECAY_RATE computed"

# --- TEMPERATURE computed ---------------------------------------------------
t="$(grep '^temperature_celsius' "$MNT/TEMPERATURE" | cut -d= -f2)"
awk -v x="$t" 'BEGIN{exit !((x+0) > 0)}' || fail "TEMPERATURE not computed ($t)"
ok "TEMPERATURE computed"

# --- pause ------------------------------------------------------------------
rm "$MNT/RAM"
sleep 1
grep -q 'state=paused' "$MNT/STATE" || fail "STATE not paused after rm RAM"
p1="$(grep real_elapsed_seconds "$MNT/STATE" | cut -d= -f2)"
sleep 2
p2="$(grep real_elapsed_seconds "$MNT/STATE" | cut -d= -f2)"
[ "$p1" -eq "$p2" ] || fail "time advanced while paused ($p1 -> $p2)"
ok "simulation pauses and preserves values on rm RAM"

# --- resume -----------------------------------------------------------------
touch "$MNT/RAM"
sleep 2
grep -q 'state=running' "$MNT/STATE" || fail "did not resume after touch RAM"
p3="$(grep real_elapsed_seconds "$MNT/STATE" | cut -d= -f2)"
[ "$p3" -gt "$p2" ] || fail "did not continue from saved time ($p2 -> $p3)"
ok "simulation resumes from saved state"

# --- PARAMS locked ----------------------------------------------------------
if printf 'mass=1\n' | tee "$MNT/PARAMS" >/dev/null 2>&1; then
	fail "PARAMS writable after first start"
else
	ok "PARAMS locked after first start"
fi

# --- cleanup ----------------------------------------------------------------
umount "$MNT" || fail "umount"
rmmod "$MODULE" || fail "rmmod"
trap - EXIT
ok "cleanup works"

echo "ALL TESTS PASSED"
