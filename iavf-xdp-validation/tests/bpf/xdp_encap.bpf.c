// SPDX-License-Identifier: GPL-2.0
/* Pushes push_len bytes of headroom (default 64, more than the ~24 B the
 * regular libeth skb headroom allows), stamps them, then pops them again so
 * the frame is unchanged. Fails (drop) if the driver left too little headroom.
 */
#include <linux/bpf.h>
#include <bpf/bpf_helpers.h>

const volatile int push_len = 64;

SEC("xdp")
int xdp_encap(struct xdp_md *ctx)
{
	void *data, *data_end;
	unsigned char *p;
	int i;

	if (bpf_xdp_adjust_head(ctx, -push_len))
		return XDP_DROP;

	data = (void *)(long)ctx->data;
	data_end = (void *)(long)ctx->data_end;
	p = data;
	if (p + 64 > (unsigned char *)data_end)
		return XDP_DROP;
	for (i = 0; i < 64; i++)
		p[i] = 0xe5;
	return bpf_xdp_adjust_head(ctx, push_len) ? XDP_DROP : XDP_PASS;
}

char _license[] SEC("license") = "GPL";
