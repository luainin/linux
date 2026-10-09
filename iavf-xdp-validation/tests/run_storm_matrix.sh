#!/bin/bash
# Storm matrix: sender runs storm_gen.py at the DUT while the DUT is sampled over its
# management NIC. Edit D (DUT), S (sender) and the MACs/IPs for your lab.
# usage: storm_run.sh <label> <size> <pps>
O="-o BatchMode=yes -o StrictHostKeyChecking=no"
D=root@10.223.30.164; S=root@10.223.31.16
L=$1; SIZE=$2; PPS=$3
cpu='awk "/^cpu /{print \$2+\$3+\$4+\$7+\$8, \$2+\$3+\$4+\$5+\$6+\$7+\$8}" /proc/stat'
rx='awk "/eth0:/{print \$3}" /proc/net/dev'
for m in eth-bcast arp-req arp-reply arp-cache ip-unicast ip-bcast udp-bcast ip-frag icmp tcp-syn tcp-zerowin; do
  a=$(ssh $O $D "$cpu; $rx" | tr '\n' ' ')
  ssh $O $S "python3 /home/root/storm_gen.py $m --iface eth0 --dst-ip 10.4.0.50 --dst-mac 00:15:5d:5c:bf:ae --size $SIZE --pps $PPS --seconds 15 --procs 4" >/tmp/gen.$m 2>&1 &
  sleep 3; ssh $O -o ConnectTimeout=8 $D 'echo alive' >/dev/null 2>&1 && alive=ok || alive=NOSSH
  wait
  b=$(ssh $O $D "$cpu; $rx" | tr '\n' ' ')
  aa=($a); bb=($b)
  sent=$(awk '{s+=$3} END{print s}' /tmp/gen.$m)
  echo "$L $m size=$SIZE sent=$sent rx=$(( bb[2]-aa[2] )) busy=$(( bb[0]-aa[0] )) total=$(( bb[1]-aa[1] )) ssh=$alive"
  sleep 2
done
ssh $O $D 'dmesg | grep -ci "kasan\|BUG:\|lockdep\|Call Trace\|hung task"; bpftool net | head -3'
