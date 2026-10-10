# iavf XDP backlog


Status key: [ ] todo, [x] done.

1. [x] Boot deadlock: crit_lock held across register_netdevice() while hv_netvsc
   notifier re-enters ndo_bpf. Fixed via `register_task` (fork 55e83d34fcd6).
2. [x] Slow VF reset (no-link VF) returned -EBUSY after 5 s although the program
   was committed; hv_netvsc then saw vf_setxdp failed and retries hit "already
   attached". Fixed (fork ab9b2a24060b). Stock iavf_change_mtu has the same
   -EBUSY pattern (not changed, stock behaviour).
3. [x] KUnit test for the register_task re-entry path (fork: iavf_xdp_setup suite, 74 KUnit tests total).
4. [x] NULL/size guard for rx_rings/num_active_queues in ndo_bpf if called before
   rings are allocated.
5. [x] VF reset and attach/detach during a storm: 51 detach/attach cycles + ring resizes (ethtool -G) under a 300k pps ip-unicast storm via VF-to-VF, 0 failures, no splats; attach logs 'reset still in progress, completing in background' (expected).
16. [x] Intermittent ~5 s rtnl-held stall on back-to-back detach/attach (~1 in 8 cycles). Root cause (ftrace): iavf_finish_config was queued ahead of reset_task on the ordered wq and blocked on rtnl_lock held by iavf_xdp_setup, which was waiting for that reset. Fixed in bcda5be4bf6f (rtnl_trylock + requeue). Verified on hardware: 150 detach/attach cycles, 0 slow; pack 24/0/1.
6. [x] Oversize frames: 1514 B @ 320k pps storm and 9000 B frames (sender MTU 9000) into MTU-1500 VF with native XDP: no splats; 9000 B never reach the VF (PF drops). MTU change with XDP loaded is rejected (ndo_change_mtu and hv_netvsc: 'mtu too large'; XDP attach at MTU 9000 -> EOPNOTSUPP). Drop-flood under RT: DUT stayed responsive, no splats. Real RT latency (cyclictest) not measured.
7. [x] kmemleak/KASAN/lockdep debug kernel (fork dcfb2688eafe, IAVF_DEBUG_KERNEL in the layer recipe): validation pack 25/25 incl. kmemleak (no leaks), and 11 attach/detach+ring-resize cycles under a 300k pps storm: 0 kmemleak reports, 0 KASAN/BUG/WARN from iavf XDP code. Pre-existing boot splats are NOT in XDP code: 'suspicious RCU usage' (RCU_NOCB, rcuc/0), and 'sleeping function called from invalid context' from iavf_request_misc_irq -> hv_compose_msi_msg (pci-hyperv takes a sleeping RT lock under irq_desc lock). The first splat turns lockdep off, so lockdep coverage is limited.
8. [ ] Multi-hour soak with storm + attach/detach churn.
9. [~] VF hot-remove/re-add (SR-IOV IovWeight 0 -> 100 on the Pri adapter, debug kernel, ping and XDP attached): VF removal and re-add are clean, 0 KASAN, 0 kmemleak; a DROP program on the master kept dropping on the synthetic path after removal and on the VF path after re-add, and traffic recovered on detach. Still untested: non-iavf VF and live migration.
10. [x] Audit stormcontrol programs for XDP_TX/REDIRECT use: done on the loaded xlated programs (xdp_entry + 7 filters); only ABORTED(0)/DROP(1)/PASS(2) returns, no redirect/adjust helper calls. Safe for DROP/PASS-only.
11. [-] Dropped by decision: the layer keeps building from the fork branch (SRC_URI override + SRCREV:x64); no layer patch is maintained. Fixbootloader change stays a saved diff only.
12. [~] Upstream plan written in UPSTREAM.md (rebase on net-next, split the series, reviewer points); not submitted. PR 85541 is Azure DevOps, not reachable via gh.
13. [x] Rollback and staged rollout: documented in the layer (README-iavf-xdp-rollout.md).
14. [x] Re-run perf numbers VF-to-VF (earlier table measured hv_netvsc path only).
17. [x] Senior review, PF-wedge focus (fork 385eb97c4c7c, 3ebea6f3c8dd): reset now requested under crit_lock; iavf_remove() releases a program still attached (hv_netvsc uses dev_xdp_propagate, untracked by the core); xdp_drops counts program DROP verdicts only (not HW-error/partial/alloc-fail frames). KUnit 84/84; mutation check: reverting each fix fails 4 tests.
18. [x] Decision: XDP attach/detach waits for crit_lock without a timeout (fail-closed). A VF error makes hv_netvsc roll back the master program, which would leave the port unfiltered; detach must never fail. Waits over 1 s log "XDP attach/detach: waited N ms for a reset in progress". Cost: rtnl can be held up to ~30 s behind a reset_task stuck waiting on the PF.
19. [ ] PF wedge on tbird05c ("Failed to communicate with PF", host Degraded 32787) after ethtool -G churn under storm with XDP attach/detach. Cause not attributed. Next: recover the host NIC port, then run the same churn on a stock kernel without XDP, with ftrace. With the VF dead, eth0 kept forwarding on the synthetic path and stormcontrol stayed attached (graceful).
20. [ ] Upstream consideration: rebuild Rx queues locally on attach/detach (like ixgbevf) instead of a full VIRTCHNL_OP_RESET_VF. Low production exposure (hot-add and program swaps take no reset).
21. [ ] Known gap, documented only: MTU 3431-3506 is accepted by hv_netvsc XDP but rejected by iavf (max 3430). On VF hot-add, hv_netvsc ignores the error, so the VF would be unfiltered. DeltaV uses MTU 1500.
