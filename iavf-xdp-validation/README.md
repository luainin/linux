# Working on the linux-nilrt kernel (iavf XDP patch)

This directory lives on the `iavf-xdp-native-6.12` branch (based on
`nilrt/master/6.12`) next to the patch commit it validates. Commit 1 is the
patch that `meta-emerson-core` carries as
`recipes-kernel/linux/files/0001-iavf-add-native-XDP-support-for-DROP-and-PASS.patch`
(regenerate it from that single commit with `git format-patch -1 <sha>`);
this directory is not part of that patch. The Yocto build (kas container) is
only used to confirm the recipe still builds and to publish the patch. Do not
edit `build/tmp-glibc/work-shared/x64/kernel-source` directly.

## 1. Create the working clone (once)

The recipe pins the kernel by `SRCREV` in
`sources/meta-emerson-core/recipes-kernel/linux/linux-nilrt_git.bb`
(currently `91411b5d605ff59dbb43b35bbc9235c4a78849a5`, branch
`nilrt/master/6.12`).

```bash
cd ~/yocto-init
MIRROR=/mnt/data/yocto-client/downloads/git2/github.com.ni.linux.git
git clone --no-hardlinks --no-checkout $MIRROR linux-nilrt
cd linux-nilrt
git remote set-url origin https://github.com/ni/linux.git
git checkout -b iavf-xdp <SRCREV from the .bb file>
```

Clone the bitbake mirror, **not** `work-shared/.../kernel-source`: that tree
borrows objects from a container path (`/work/downloads/...`) via git
alternates and is broken when read from the host.

## 2. Apply the current patch

The patch is published on the `fixall` branch of meta-emerson-core. Read it
without touching that repo's working tree:

```bash
git -C /mnt/data/yocto-client/sources/meta-emerson-core \
  show origin/fixall:recipes-kernel/linux/files/0001-iavf-add-native-XDP-support-for-DROP-and-PASS.patch \
  > /tmp/xdp.patch
git am /tmp/xdp.patch
```

Applying cleanly on the pinned `SRCREV` also confirms the recipe's
`do_patch` will succeed.

## 3. Edit and check

Touch only the intended files:
`drivers/net/ethernet/intel/iavf/*`,
`drivers/net/ethernet/intel/libeth/rx.c`, `include/net/libeth/rx.h`.

```bash
git commit -a --amend            # keep it as ONE commit; imperative subject, no interface names
./scripts/checkpatch.pl --strict -g HEAD
```

Expected checkpatch output: only the "does MAINTAINERS need updating?"
notice for the new KUnit file. The commit author/Signed-off-by must match
(checkpatch errors otherwise).

## 4. Run the KUnit tests

Needs `gcc make bc libelf-dev libssl-dev qemu-system-x86_64` plus `flex`,
`bison` and `m4`. Without root, unpack the debs into a user dir:

```bash
mkdir -p /tmp/debs && cd /tmp/debs && apt-get download flex bison m4 libfl2 libfl-dev
T=$HOME/.local/kbuild-tools; mkdir -p $T; for d in *.deb; do dpkg -x $d $T; done
export PATH=$T/usr/bin:$PATH LD_LIBRARY_PATH=$T/usr/lib/x86_64-linux-gnu \
       BISON_PKGDATADIR=$T/usr/share/bison M4=$T/usr/bin/m4
```

(or `sudo apt install flex bison`). `IAVF` is a hidden Kconfig symbol selected
by `I40EVF`, so the kunitconfig must enable that, and `CONFIG_NET=y` first:

```bash
cat > /tmp/iavf.kunitconfig <<'EOF'
CONFIG_KUNIT=y
CONFIG_NET=y
CONFIG_PCI=y
CONFIG_PCI_MSI=y
CONFIG_NETDEVICES=y
CONFIG_ETHERNET=y
CONFIG_NET_VENDOR_INTEL=y
CONFIG_INET=y
CONFIG_BPF_SYSCALL=y
CONFIG_I40EVF=y
CONFIG_IAVF_KUNIT_TEST=y
EOF
python3 tools/testing/kunit/kunit.py run --arch=x86_64 \
  --kunitconfig=/tmp/iavf.kunitconfig --build_dir=/tmp/kunit-build \
  --make_options=-j$(nproc) 'iavf_txrx*'
```

UML (`--arch=um`) cannot be used: it has no PCI/Ethernet driver support.

Status: **verified** — 56/56 KUnit tests pass (35 cases, several
parametrized). The build also compiles the entire iavf driver and libeth with
the patch. Note: kunit.py may hang on the final QEMU reboot and hit its
timeout *after* printing the results; the pass/fail summary is still valid
(wrap in `timeout 120`).

Coverage (`iavf_txrx_kunit_test.c`):
- MTU boundary table (13 values) and libeth buffer-fit invariants
- libeth headroom/page-length invariants (adjust_head room > 128 B)
- `max_pkt_size` clamp table (10 cases) via `iavf_xdp_clamp_max_frame()`
- Run-or-skip truth table via `iavf_xdp_frame_runnable()` (EOP / discard / HW error)
- `iavf_run_xdp()` with a fake `bpf_prog`: PASS/DROP pass through; ABORTED,
  TX, REDIRECT and unknown verdicts map to DROP; program called once and sees
  the right data/data_end
- `iavf_xdp_build_skb()`: plain, head pop/push (max adjust_head), tail trim/grow,
  empty payload, metadata set/absent, page recycle flag, buffer sharing

`iavf_run_xdp()` and `iavf_xdp_build_skb()` are `VISIBLE_IF_KUNIT` /
`EXPORT_SYMBOL_IF_KUNIT`; they stay `static`-equivalent (not exported) when
KUnit is off.

### Sanitizer run (KASAN/UBSAN/lockdep/kmemleak)

`iavf-debug.kunitconfig` (repo root) is the base config plus
`DEBUG_KERNEL, KASAN, UBSAN, PROVE_LOCKING, DEBUG_ATOMIC_SLEEP, DEBUG_KMEMLEAK`.
Use a separate build dir and `--raw_output=all` so kernel splats are visible:

```
timeout 600 python3 tools/testing/kunit/kunit.py run --arch=x86_64 \
  --kunitconfig=$PWD/iavf-xdp-validation/iavf-debug.kunitconfig \
  --build_dir=/tmp/kunit-debug --make_options=-j$(nproc) \
  --raw_output=all 'iavf_txrx*' > /tmp/kunit-debug.log 2>&1
grep -E "BUG:|KASAN:|WARNING:|possible .*lock|UBSAN|Oops|not ok" /tmp/kunit-debug.log
```

Status: **verified** - 35/35 cases clean. The first run exposed a double
free in the test fixture (skb free drops the page ref); fixed.

### sparse (RCU/endian annotations)

No root needed: `apt-get download sparse && dpkg -x sparse*.deb ~/.local/kbuild-tools`,
then with the same PATH/LD_LIBRARY_PATH exports as above:

```
touch drivers/net/ethernet/intel/iavf/iavf_txrx.c
make O=/tmp/kunit-build ARCH=x86_64 C=2 CF="-D__CHECK_ENDIAN__" \
  drivers/net/ethernet/intel/iavf/iavf_txrx.o
```

Status: **verified** - no warnings on iavf_txrx/main/virtchnl and libeth rx.c
(confirms the `__rcu` annotations on the xdp_prog pointers are consistent).

On-target BPF test pack (veth self-test verified in QEMU/KVM; iavf hardware
run pending): see `tests/README.md`.

## 5. Publish the patch

```bash
git format-patch -1 -o /tmp/out
cp /tmp/out/0001-*.patch /mnt/data/yocto-client/new-xdp.patch
```

Then copy it over the recipe's patch and push **inside the kas container**
(all git operations on meta-emerson-core must be in ONE
`./yocto-init.py client shell` call: kas resets branch state at the start of
every call, and the container's default git identity is rejected by the org's
push policy):

```bash
cd /work/sources/meta-emerson-core
git config user.name "<your name>"; git config user.email "<your email>"
git fetch origin fixall && git checkout -B fixall origin/fixall
cp /work/new-xdp.patch recipes-kernel/linux/files/0001-iavf-add-native-XDP-support-for-DROP-and-PASS.patch
git add -A recipes-kernel/linux && git commit -m "..." && git push origin fixall
```

## 6. Confirm the Yocto build

```bash
./yocto-init.py client shell -- bitbake linux-nilrt -c compile -f
```

(`do_patch` must re-run to pick up a changed patch, e.g.
`-c cleansstate linux-nilrt` first.)

## Known blocker: eSDK on the host (not needed for KUnit)

Installing the eSDK on the host (`<installer>.sh -d ~/yocto-init/.esdk -y`)
extracts fine but its "Preparing build system" step fails with
`User namespaces are not usable by BitBake` because Ubuntu's AppArmor
restricts unprivileged user namespaces. Rebuilding the SDK does not change
this. It needs a host setting change (needs sudo):
`sudo sysctl -w kernel.apparmor_restrict_unprivileged_userns=0`
(lasts until reboot; use an AppArmor profile or sysctl.d entry to persist).
The SDK installed earlier under `/mnt/data/yocto-client/iq-esdk` was installed
inside the container and does not work from the host.

## Storm matrix on hardware

`tests/storm_gen.py` (sender, stdlib only) generates Achilles-style storms;
`tests/run_storm_matrix.sh <label> <size> <pps>` runs all eleven against the
DUT with the production `/DeltaV/ebpf` programs loaded by `dvinit-stormcontrol`
and prints master rx packets, busy CPU jiffies and SSH liveness. Results
(DUT 4 vCPU, 15 s each, 4 sender procs) are in `meta-emerson-core`
`recipes-kernel/linux/README-iavf-xdp.md`.
