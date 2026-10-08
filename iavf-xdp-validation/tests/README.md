# iavf native-XDP on-target test pack

Exercises the XDP behaviour of the iavf patch
(`meta-emerson-core/recipes-kernel/linux/files/0001-iavf-add-native-XDP-support-for-DROP-and-PASS.patch`)
with real BPF programs. Complements the KUnit suite (see `../README.md`).

## Contents
- `bpf/*.bpf.c` - pass, drop, unsupported (TX/REDIRECT/ABORTED/unknown via a
  map), encap (adjust_head -64 then +64), meta (adjust_meta), count (frames,
  bytes, max frame length; drops UDP to a port).
- `run_iavf_xdp_tests.sh` - needs root; `--iface ETH [--peer-ip IP]` for real
  hardware, `--veth` for a self-contained self-test.
- `run_in_qemu.sh KERNEL [loops]` - runs the `--veth` self-test as root in a
  QEMU/KVM guest on the host rootfs (no host root needed).

## Build the BPF objects
Needs `clang` and libbpf headers. Without a libbpf-dev package, generate them
from the kernel clone:

    K=$(git rev-parse --show-toplevel); mkdir -p /tmp/bpfinc/bpf
    cp $K/tools/lib/bpf/bpf_{helpers,endian,tracing,core_read}.h /tmp/bpfinc/bpf/
    python3 $K/scripts/bpf_doc.py --header --file $K/include/uapi/linux/bpf.h \
        > /tmp/bpfinc/bpf/bpf_helper_defs.h
    make BPF_INC=/tmp/bpfinc

## Status
- **Verified (veth, KASAN+kmemleak+lockdep kernel, QEMU/KVM)**: 21 pass, 0 fail,
  1 skip (MTU guard is iavf-only). This validates the script and programs, not
  the iavf driver.
- **Not yet run on iavf** (needs an Intel SR-IOV VF, Hyper-V or VFIO): everything
  under `--iface`. On iavf the MTU-guard checks activate automatically.

## Manual checks still needed on hardware
- Oversize frames: send frames larger than one Rx buffer (e.g. peer MTU 9000,
  `ping -s 4000 -M do`) with a program attached at MTU 1500; `xdp_count`'s
  `max_len` must stay <= rx_buf_len (1536) - the HW must drop the rest.
- Drop flood under RT load: pktgen/`xdp-trafficgen` at line rate, watch
  latency (`cyclictest`) and that NAPI polls respect the budget.
- Reset while traffic flows: attach/detach during a flood, and
  `ethtool -L`/`ethtool -G` while attached.
- Re-run with `CONFIG_KASAN`, `CONFIG_DEBUG_KMEMLEAK`, `CONFIG_PROVE_LOCKING`.
- CPU comparison vs `xdpgeneric` (earlier prelim: ~11% -> ~2.4%).
