#!/bin/bash
# SPDX-License-Identifier: GPL-2.0
# Boots KERNEL (bzImage) under QEMU/KVM with the host root filesystem mounted
# read-only over 9p, and runs run_iavf_xdp_tests.sh --veth as root inside it.
# No host root or user namespaces needed.
#   run_in_qemu.sh /tmp/vk/arch/x86/boot/bzImage [loops]
set -eu
KERNEL=${1:?usage: $0 bzImage [loops]}
LOOPS=${2:-50}
HERE=$(cd "$(dirname "$0")" && pwd)
INIT=$(mktemp /tmp/iavf-xdp-init.XXXXXX)
trap 'rm -f "$INIT"' EXIT
cat > "$INIT" <<EOS
#!/bin/bash
export PATH=/usr/sbin:/usr/bin:/sbin:/bin
mount -t proc proc /proc; mount -t sysfs sys /sys
mount -t debugfs none /sys/kernel/debug
mount -t tmpfs tmpfs /run; mkdir -p /run/netns
ip link set lo up
bash $HERE/run_iavf_xdp_tests.sh --veth --loops $LOOPS
echo "=== GUEST RC=\$?"
echo o > /proc/sysrq-trigger
EOS
chmod +x "$INIT"
timeout 600 qemu-system-x86_64 -enable-kvm -cpu host -m 2048 -smp 4 \
	-nographic -no-reboot -kernel "$KERNEL" \
	-fsdev local,id=r,path=/,security_model=none,readonly=on \
	-device virtio-9p-pci,fsdev=r,mount_tag=/dev/root \
	-append "root=/dev/root rootfstype=9p rootflags=trans=virtio,version=9p2000.L,cache=loose ro console=ttyS0 init=$INIT panic=-1 loglevel=3" |
	tee /tmp/iavf-xdp-qemu.log | grep -E "PASS|FAIL|SKIP|==|BUG:|KASAN|WARNING:"
grep -q "GUEST RC=0" /tmp/iavf-xdp-qemu.log
