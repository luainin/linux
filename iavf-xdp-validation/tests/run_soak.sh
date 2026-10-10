#!/bin/sh
# Soak test for iavf native XDP, run on the DUT as root while a sender floods
# the master interface. Repeatedly takes the XDP program off and back on (by
# default via the stormcontrol init script; hv_netvsc propagates it to the VF)
# and after every cycle checks the VF is up, the peer answers, and no new
# kernel splat appeared. With --master (the hv_netvsc interface) it also checks
# the VF data path carried traffic since the last cycle: ping alone can't tell,
# as hv_netvsc falls back to the synthetic path when the VF is down. Logs
# memory every cycle and runs kmemleak at the end.
#
#   run_soak.sh --vf eth5 --peer-ip 10.4.0.60 [--master eth0] [--minutes 60] [--interval 30]
#               [--off "<cmd>"] [--on "<cmd>"] [--log /tmp/soak.log]
VF= MASTER= PEER_IP= MINUTES=60 INTERVAL=30 LOG=/tmp/soak.log
OFF="/etc/init.d/dvinit-stormcontrol stop"
ON="/etc/init.d/dvinit-stormcontrol start"
while [ $# -gt 0 ]; do
	case $1 in
	--vf) VF=$2; shift 2;;
	--peer-ip) PEER_IP=$2; shift 2;;
	--master) MASTER=$2; shift 2;;
	--minutes) MINUTES=$2; shift 2;;
	--interval) INTERVAL=$2; shift 2;;
	--off) OFF=$2; shift 2;;
	--on) ON=$2; shift 2;;
	--log) LOG=$2; shift 2;;
	*) echo "unknown arg $1" >&2; exit 2;;
	esac
done
[ -n "$VF" ] && [ -n "$PEER_IP" ] || { echo "--vf and --peer-ip required" >&2; exit 2; }

log() { echo "$(date +%T) $*" | tee -a "$LOG"; }
mem() { awk '/^MemAvailable:|^Slab:|^Percpu:/{printf "%s%s ", $1, $2}' /proc/meminfo; }
vf_rx() { [ -n "$MASTER" ] && ethtool -S "$MASTER" | awk '/^ +vf_rx_packets:/{print $2}'; }
vf_up() { ip -o link show dev "$VF" | grep -q '[<,]UP[,>]' &&
	  [ "$(cat /sys/class/net/$VF/operstate)" != down ]; }
# New splats since line $1 of dmesg, ignoring the known pci-hyperv RT one
splats() {
	dmesg | tail -n +$(($1 + 1)) | awk '
	function flush() { if (hdr != "" && !(known && hv)) u++ }
	/BUG:|WARNING:|KASAN|possible .*lock|UBSAN|Oops|hung task/ {
		flush(); hdr = $0; known = /sleeping function called from invalid context/; hv = 0; next }
	/ hv_compose_msi_msg\+/ { hv = 1 }
	END { flush(); print u + 0 }'
}

: > "$LOG"
MARK=$(dmesg | wc -l)
end=$(($(date +%s) + MINUTES * 60))
cycles=0 fails=0 rx=$(vf_rx)
log "start vf=$VF peer=$PEER_IP minutes=$MINUTES interval=$INTERVAL $(mem)"
while [ "$(date +%s)" -lt "$end" ]; do
	cycles=$((cycles + 1))
	t0=$(date +%s%N)
	$OFF >/dev/null 2>&1; $ON >/dev/null 2>&1
	ms=$((($(date +%s%N) - t0) / 1000000))
	sleep 2
	st=ok
	vf_up || st="$st vf-down"
	ping -c 3 -W 1 -i 0.2 "$PEER_IP" >/dev/null 2>&1 || st="$st no-ping"
	n=$(splats "$MARK"); [ "$n" -gt 0 ] && st="$st splats=$n"
	if [ -n "$MASTER" ]; then
		r=$(vf_rx); [ "$r" -gt "$rx" ] || st="$st no-vf-rx"; rx=$r
	fi
	[ "$st" = ok ] || { st=${st#ok }; fails=$((fails + 1)); }
	log "cycle=$cycles toggle_ms=$ms status=$st $(mem)"
	sleep "$INTERVAL"
done

if [ -w /sys/kernel/debug/kmemleak ]; then
	echo scan > /sys/kernel/debug/kmemleak; sleep 5; echo scan > /sys/kernel/debug/kmemleak
	leaks=$(grep -c '^unreferenced object' /sys/kernel/debug/kmemleak)
else
	leaks=n/a
fi
log "done cycles=$cycles failed_cycles=$fails kmemleak=$leaks splats=$(splats "$MARK") $(mem)"
[ "$fails" = 0 ] && { [ "$leaks" = 0 ] || [ "$leaks" = n/a ]; }
