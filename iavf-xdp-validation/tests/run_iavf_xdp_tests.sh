#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
# On-target test pack for the iavf native-XDP patch.
#
#   run_iavf_xdp_tests.sh --iface eth5 [--peer-ip 10.0.0.2] [--loops 50]
#   run_iavf_xdp_tests.sh --veth          # self-test of this script on veth
#
# Needs root, ip, bpftool, and the BPF objects (run `make` first).
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
BPF=$HERE/bpf
IFACE= PEER_IP= LOOPS=50 VETH=0
PASS=0 FAIL=0 SKIP=0

while [ $# -gt 0 ]; do
	case $1 in
	--iface) IFACE=$2; shift 2;;
	--peer-ip) PEER_IP=$2; shift 2;;
	--loops) LOOPS=$2; shift 2;;
	--veth) VETH=1; shift;;
	*) echo "unknown arg $1" >&2; exit 2;;
	esac
done

ok()   { echo "PASS: $*"; PASS=$((PASS + 1)); }
bad()  { echo "FAIL: $*"; FAIL=$((FAIL + 1)); }
skip() { echo "SKIP: $*"; SKIP=$((SKIP + 1)); }
check() { local d=$1; shift; if "$@" >/dev/null 2>&1; then ok "$d"; else bad "$d"; fi; }
check_not() { local d=$1; shift; if "$@" >/dev/null 2>&1; then bad "$d"; else ok "$d"; fi; }

[ "$(id -u)" = 0 ] || { echo "must be root"; exit 2; }
for o in pass drop unsupported encap meta count; do
	[ -f "$BPF/xdp_$o.bpf.o" ] || { echo "missing $BPF/xdp_$o.bpf.o - run make"; exit 2; }
done

cleanup() {
	[ -n "$IFACE" ] && ip link set dev "$IFACE" xdp off 2>/dev/null
	rm -f /sys/fs/bpf/xdpt_* 2>/dev/null
	[ "$VETH" = 1 ] && { ip netns del xdpt-peer 2>/dev/null; ip link del xdpt0 2>/dev/null; }
}
trap cleanup EXIT

if [ "$VETH" = 1 ]; then
	ip netns add xdpt-peer
	ip link add xdpt0 type veth peer name xdpt1 netns xdpt-peer
	ip addr add 10.99.0.1/24 dev xdpt0
	ip link set xdpt0 up
	ip -n xdpt-peer addr add 10.99.0.2/24 dev xdpt1
	ip -n xdpt-peer link set xdpt1 up
	ip -n xdpt-peer link set lo up
	# veth only runs native XDP if the peer has an XDP prog or GRO is on
	ip netns exec xdpt-peer ethtool -K xdpt1 gro on 2>/dev/null
	IFACE=xdpt0 PEER_IP=10.99.0.2
fi
[ -n "$IFACE" ] || { echo "--iface or --veth required"; exit 2; }

DRIVER=$(ethtool -i "$IFACE" 2>/dev/null | awk '/^driver:/{print $2}')
echo "== interface $IFACE driver=${DRIVER:-unknown} peer=${PEER_IP:-none} loops=$LOOPS"

kmsg_mark() { dmesg | wc -l; }
MARK=$(kmsg_mark)

# bpftool handles BTF-defined maps; legacy iproute2 loaders reject them with EACCES.
attach()  {
	local pin=/sys/fs/bpf/xdpt_$1
	rm -f "$pin"
	bpftool prog load "$BPF/xdp_$1.bpf.o" "$pin" type xdp &&
		bpftool net attach xdpdrv pinned "$pin" dev "$IFACE" overwrite
}
detach()  { ip link set dev "$IFACE" xdp off; }
attached() { ip -d link show dev "$IFACE" | grep -q "prog/xdp"; }
pingp()   { ping -c 3 -W 1 -i 0.2 "$PEER_IP"; }

# 1. basic attach / detach / replace
detach 2>/dev/null
check "attach pass" attach pass
check "program visible" attached
check "replace pass->drop" attach drop
check "replace drop->pass" attach pass
check "detach" detach
check_not "program gone" attached

# 2. attach/detach/replace stress (NULL<->prog reset path, prog<->prog swap)
fails=0
for i in $(seq "$LOOPS"); do
	attach pass || fails=$((fails + 1))
	attach drop || fails=$((fails + 1))
	attach pass || fails=$((fails + 1))
	detach      || fails=$((fails + 1))
done
[ $fails = 0 ] && ok "attach/replace/detach x$LOOPS" || bad "stress had $fails failures"

# 3. verdict behaviour (needs a peer)
if [ -n "$PEER_IP" ]; then
	check "baseline ping" pingp
	attach pass; check "ping through XDP_PASS" pingp
	attach drop; check_not "ping blocked by XDP_DROP" pingp
	attach pass; check "ping works again after replace" pingp
	attach encap; check "adjust_head round trip (encap 64B) passes" pingp
	attach meta;  check "adjust_meta passes" pingp
	attach unsupported
	for v in 0 3 4 99; do
		bpftool map update name verdict key 0 0 0 0 value $v 0 0 0 2>/dev/null
		check_not "unsupported verdict $v is dropped" pingp
	done
	detach
	check "ping works after detach" pingp

	attach count
	pingp >/dev/null 2>&1
	max=$(bpftool -j map dump name stats_map 2>/dev/null | python3 -c '
import json, struct, sys
best = 0
for e in json.load(sys.stdin):
    for v in e["values"]:
        val = v["value"]
        if isinstance(val, dict):
            best = max(best, val["max_len"])
        else:
            best = max(best, struct.unpack("<4Q", bytes(int(x, 16) for x in val))[2])
print(best)' 2>/dev/null)
	[ -n "$max" ] && [ "$max" -gt 0 ] && ok "counting prog saw traffic (max_len=$max)" \
		|| bad "counting prog saw no traffic"
	detach
else
	skip "verdict/traffic tests (no --peer-ip)"
fi

# 4. MTU guard (iavf only)
if [ "$DRIVER" = iavf ]; then
	orig=$(cat /sys/class/net/$IFACE/mtu)
	attach pass
	check_not "MTU 9000 refused while program attached" ip link set dev "$IFACE" mtu 9000
	detach
	check "MTU 9000 allowed without program" ip link set dev "$IFACE" mtu 9000
	check_not "attach refused at MTU 9000" attach pass
	ip link set dev "$IFACE" mtu "$orig"
	[ "$(cat /sys/class/net/$IFACE/mtu)" = "$orig" ] && ok "MTU restored" || bad "MTU restore"
else
	skip "MTU guard (driver is ${DRIVER:-unknown}, not iavf)"
fi

# 5. kernel health
if [ -w /sys/kernel/debug/kmemleak ]; then
	echo scan > /sys/kernel/debug/kmemleak; sleep 5; echo scan > /sys/kernel/debug/kmemleak
	[ -z "$(cat /sys/kernel/debug/kmemleak)" ] && ok "kmemleak: no leaks" || { bad "kmemleak reports"; head -40 /sys/kernel/debug/kmemleak; }
else
	skip "kmemleak not available (CONFIG_DEBUG_KMEMLEAK / debugfs)"
fi
# Known, not iavf: on PREEMPT_RT pci-hyperv takes a sleeping lock in
# hv_compose_msi_msg() under the irq_desc raw lock, so every request_irq() on
# a Hyper-V VF splats (stock `ip link set <vf> up` too). Count those reports
# separately; anything else fails.
splats=$(dmesg | tail -n +$((MARK + 1)) | awk '
	function flush() { if (hdr != "") { if (known && hv) k++; else { u++; print hdr > "/dev/stderr" } } }
	/BUG:|WARNING:|KASAN|possible .*lock|UBSAN|Oops/ {
		flush(); hdr = $0; known = /sleeping function called from invalid context/; hv = 0; next }
	/ hv_compose_msi_msg\+/ { hv = 1 }
	END { flush(); printf "%d %d\n", u, k }')
set -- $splats
[ "${2:-0}" -gt 0 ] && echo "note: $2 known pci-hyperv RT splat(s) (hv_compose_msi_msg)"
if [ "${1:-0}" -gt 0 ]; then
	bad "kernel splat during tests"
else
	ok "no kernel splats in dmesg (besides known pci-hyperv RT)"
fi

echo "== passed=$PASS failed=$FAIL skipped=$SKIP"
[ "$FAIL" = 0 ]
