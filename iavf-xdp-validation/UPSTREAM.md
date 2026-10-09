# Upstreaming plan (iavf native XDP, DROP/PASS only)

Status: not submitted. The Azure DevOps PR 85541 was abandoned (not needed).

## Before sending anything to netdev / Intel
1. Rebase onto current `net-next` (this tree is NI nilrt 6.12). Intel has been
   moving iavf to libeth/libie; re-check whether an upstream XDP conversion
   already exists and, if so, rebase onto or offer fixes to that instead of a
   parallel implementation.
2. Split the single squashed commit into a reviewable series, e.g.
   libeth headroom -> iavf Rx path (`iavf_run_xdp`) -> `ndo_bpf` / setup ->
   `register_task` (hv_netvsc VF registration) -> KUnit tests.
3. Run `scripts/checkpatch.pl --strict`, `sparse`, and `get_maintainer.pl`;
   Cc Intel Wired LAN (`intel-wired-lan@lists.osuosl.org`) and `netdev`.
4. Rebuild the KUnit suite (74 tests) and the on-target pack against the
   rebased tree; attach the results (pack 24-25 pass, storm matrix, KASAN and
   kmemleak runs) to the cover letter.

## Points reviewers will raise
- **DROP/PASS only**: no XDP_TX/REDIRECT, no `ndo_xdp_xmit`, no XDP Tx ring.
  Explain why (VF used under hv_netvsc, DROP/PASS covers the filter use case)
  and that other verdicts are treated as invalid and dropped.
- **`register_task`**: hv_netvsc runs its NETDEV_REGISTER notifier inside
  `register_netdevice()`, which calls the VF `ndo_bpf` while `iavf_finish_config`
  holds `crit_lock`. The patch skips the lock only for that task. Expect
  discussion; an alternative is deferring the program push.
- **Reset wait**: `iavf_xdp_setup` returns 0 and logs when the VF reset takes
  longer than 5 s, since the program is already committed (returning
  `-EBUSY` made hv_netvsc report failure and retry fail).
- **MTU**: changes are refused while a program is loaded and attach is refused
  above the single-buffer limit.
- Known unrelated splats seen on RT debug kernels (pci-hyperv
  `hv_compose_msi_msg` sleeping lock under `request_irq`) are not from this
  patch; mention them so they are not attributed to it.

## Not done
Live migration and a non-iavf VF with a program attached were not tested.
