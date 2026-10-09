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
5. [ ] VF reset and attach/detach during a storm (only a light ping flood so far).
6. [ ] Oversize-frame test on hardware; drop-flood under RT.
7. [ ] kmemleak/KASAN on hardware (needs a debug kernel build).
8. [ ] Multi-hour soak with storm + attach/detach churn.
9. [ ] Non-iavf VF, VF hot-remove/re-add, live migration with a program attached.
10. [ ] Audit stormcontrol programs for XDP_TX/REDIRECT use (unsupported: DROP/PASS only).
11. [ ] Turn the fork override into a normal patch in the layer; decide on the
    fixbootloader /bzImage change (saved diff only).
12. [ ] Upstream/netdev submission and Intel review; eSDK PR 85541 follow-up.
13. [ ] Documented rollback (fall back to xdpgeneric) and staged-rollout plan.
14. [ ] Re-run perf numbers VF-to-VF (earlier table measured hv_netvsc path only).
