# path: scripts/demo.sh
#!/usr/bin/env bash
#
# Full demonstration of u235fs. Run as root:  sudo ./scripts/demo.sh
#
set -euo pipefail

MODULE="u235fs"
KO="u235fs.ko"
MNT="/mnt/u235fs"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$HERE"

cleanup() {
	set +e
	umount "$MNT" 2>/dev/null
	rmmod "$MODULE" 2>/dev/null
}
trap cleanup EXIT

hr() { echo "------------------------------------------------------------"; }
step() { hr; echo ">>> $*"; hr; }

# 1. root check
if [ "$(id -u)" -ne 0 ]; then
	echo "This script must be run as root (use sudo)." >&2
	exit 1
fi

# 2. build
step "1) Building module"
make

# 3. load
step "2) Loading module (insmod)"
insmod "$KO"
lsmod | grep "^${MODULE}" || true

# 4-5. mount
step "3) Mounting filesystem"
mkdir -p "$MNT"
mount -t u235fs none "$MNT"

# 6. list
step "4) Listing files after mount"
ls -la "$MNT"

# 7. show PARAMS
step "5) PARAMS (defaults)"
cat "$MNT/PARAMS"

# 8. change PARAMS before start
step "6) Changing PARAMS before RAM is created"
printf 'mass=1500\nvolume=20\ninitial_frequency=2.5\nimpurities=3\nexternal_neutrons=10\nneutron_speed=2500\n' \
	| tee "$MNT/PARAMS"
echo "--- PARAMS now ---"
cat "$MNT/PARAMS"

# 9. start simulation
step "7) Starting simulation (touch RAM)"
touch "$MNT/RAM"
sleep 1

# 10. state files appeared
step "8) Files after start"
ls -la "$MNT"

# 11. show several updates
step "9) Watching state update"
for i in 1 2 3; do
	sleep 1
	echo "----- sample #$i -----"
	echo "[ELAPSED_TIME]";   cat "$MNT/ELAPSED_TIME"
	echo "[DECAYED_NUCLEI]"; cat "$MNT/DECAYED_NUCLEI"
	echo "[DECAY_RATE]";     cat "$MNT/DECAY_RATE"
	echo "[TEMPERATURE]";    cat "$MNT/TEMPERATURE"
	echo "[STATE]";          cat "$MNT/STATE"
done

# 12-13. pause
step "10) Pausing simulation (rm RAM)"
before_pause="$(grep real_seconds "$MNT/ELAPSED_TIME" | cut -d= -f2)"
rm "$MNT/RAM"
sleep 2
echo "[STATE after pause]"
cat "$MNT/STATE"
after_pause="$(grep real_seconds "$MNT/ELAPSED_TIME" | cut -d= -f2)"
echo "real_seconds before pause: $before_pause, after waiting 2s paused: $after_pause"
echo "(values preserved, time frozen)"

# 14-15. resume
step "11) Resuming simulation (touch RAM)"
touch "$MNT/RAM"
sleep 2
echo "[STATE after resume]"
cat "$MNT/STATE"
resumed="$(grep real_elapsed_seconds "$MNT/STATE" | cut -d= -f2)"
echo "real_elapsed_seconds after resume: $resumed (continued, not restarted)"

# 16. PARAMS locked check
step "12) Verifying PARAMS is locked after first start"
if printf 'mass=1\n' | tee "$MNT/PARAMS" >/dev/null 2>&1; then
	echo "ERROR: PARAMS was writable after simulation start!" >&2
	exit 1
else
	echo "OK: writing PARAMS is rejected (EPERM)."
fi

# 17. unmount
step "13) Unmounting"
umount "$MNT"

# 18. unload
step "14) Unloading module"
rmmod "$MODULE"

trap - EXIT
hr
echo "Demo finished successfully."
