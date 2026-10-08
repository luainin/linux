// SPDX-License-Identifier: GPL-2.0
/* Reserves 8 bytes of metadata and fills them; must not corrupt the packet. */
#include <linux/bpf.h>
#include <bpf/bpf_helpers.h>

SEC("xdp")
int xdp_meta(struct xdp_md *ctx)
{
	__u64 *m;

	if (bpf_xdp_adjust_meta(ctx, -(int)sizeof(*m)))
		return XDP_DROP;
	m = (void *)(long)ctx->data_meta;
	if ((void *)(m + 1) > (void *)(long)ctx->data)
		return XDP_DROP;
	*m = 0x1122334455667788ULL;
	return XDP_PASS;
}

char _license[] SEC("license") = "GPL";
