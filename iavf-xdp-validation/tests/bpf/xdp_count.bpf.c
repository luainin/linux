// SPDX-License-Identifier: GPL-2.0
/* Counts frames and bytes seen, and the largest frame; drops UDP packets
 * to drop_port and passes everything else. Used for oversize-frame tests:
 * with a program attached the program must never see a frame larger than
 * one Rx buffer.
 */
#include <linux/bpf.h>
#include <linux/if_ether.h>
#include <linux/ip.h>
#include <linux/udp.h>
#include <linux/in.h>
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_endian.h>

const volatile __u16 drop_port = 9;

struct stats {
	__u64 frames;
	__u64 bytes;
	__u64 max_len;
	__u64 dropped;
};

struct {
	__uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
	__uint(max_entries, 1);
	__type(key, __u32);
	__type(value, struct stats);
} stats_map SEC(".maps");

SEC("xdp")
int xdp_count(struct xdp_md *ctx)
{
	void *data = (void *)(long)ctx->data;
	void *data_end = (void *)(long)ctx->data_end;
	struct ethhdr *eth = data;
	__u32 k = 0, len = data_end - data;
	struct stats *s = bpf_map_lookup_elem(&stats_map, &k);
	int verdict = XDP_PASS;

	if (!s)
		return XDP_PASS;
	s->frames++;
	s->bytes += len;
	if (len > s->max_len)
		s->max_len = len;

	if ((void *)(eth + 1) <= data_end && eth->h_proto == bpf_htons(ETH_P_IP)) {
		struct iphdr *ip = (void *)(eth + 1);

		if ((void *)(ip + 1) <= data_end && ip->protocol == IPPROTO_UDP) {
			struct udphdr *udp = (void *)ip + ip->ihl * 4;

			if ((void *)(udp + 1) <= data_end &&
			    udp->dest == bpf_htons(drop_port))
				verdict = XDP_DROP;
		}
	}
	if (verdict == XDP_DROP)
		s->dropped++;
	return verdict;
}

char _license[] SEC("license") = "GPL";
