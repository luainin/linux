// SPDX-License-Identifier: GPL-2.0
/* Returns XDP_TX/XDP_REDIRECT/ABORTED/unknown depending on the "verdict"
 * map; iavf must turn every one of them into a drop.
 */
#include <linux/bpf.h>
#include <bpf/bpf_helpers.h>

struct {
	__uint(type, BPF_MAP_TYPE_ARRAY);
	__uint(max_entries, 1);
	__type(key, __u32);
	__type(value, __u32);
} verdict SEC(".maps");

SEC("xdp")
int xdp_unsupported(struct xdp_md *ctx)
{
	__u32 k = 0, *v = bpf_map_lookup_elem(&verdict, &k);

	return v ? *v : XDP_ABORTED;
}

char _license[] SEC("license") = "GPL";
