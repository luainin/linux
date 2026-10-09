// SPDX-License-Identifier: GPL-2.0
/* Copyright(c) 2026 DeltaV StormControl. */

/* KUnit tests for the pure-logic helpers backing iavf's DROP/PASS-only
 * native XDP support. They need no adapter/netdev/hardware state, so they
 * run under a plain KUnit build - no VM, no VF, no SR-IOV required.
 */

#include <kunit/device.h>
#include <kunit/test.h>
#include <kunit/visibility.h>
#include <linux/filter.h>
#include <linux/etherdevice.h>
#include <net/xdp.h>

#include "iavf.h"
#include "iavf_txrx.h"

MODULE_IMPORT_NS(EXPORTED_FOR_KUNIT_TESTING);

/* Largest MTU that still fits one Rx buffer, per libeth's own sizing. */
#define IAVF_TEST_MAX_MTU \
	((int)LIBETH_RX_PAGE_LEN(LIBETH_XDP_HEADROOM) - LIBETH_RX_LL_LEN)

static void iavf_xdp_mtu_ok_test(struct kunit *test)
{
	KUNIT_EXPECT_TRUE(test, iavf_xdp_mtu_ok(0));
	KUNIT_EXPECT_TRUE(test, iavf_xdp_mtu_ok(1500));
	KUNIT_EXPECT_TRUE(test, iavf_xdp_mtu_ok(IAVF_TEST_MAX_MTU - 1));
	KUNIT_EXPECT_TRUE(test, iavf_xdp_mtu_ok(IAVF_TEST_MAX_MTU));

	KUNIT_EXPECT_FALSE(test, iavf_xdp_mtu_ok(IAVF_TEST_MAX_MTU + 1));
	KUNIT_EXPECT_FALSE(test, iavf_xdp_mtu_ok(9000));
}

/* MTU boundary table */
struct mtu_case { int mtu; bool ok; const char *name; };

static const struct mtu_case mtu_cases[] = {
	{ 0, true, "zero" },
	{ 68, true, "min-ipv4" },
	{ 576, true, "576" },
	{ 1500, true, "default" },
	{ 1500 + 8, true, "default+vlan-ish" },
	{ 2000, true, "2000" },
	{ IAVF_TEST_MAX_MTU - 1, true, "max-1" },
	{ IAVF_TEST_MAX_MTU, true, "max" },
	{ IAVF_TEST_MAX_MTU + 1, false, "max+1" },
	{ 4096, false, "4096" },
	{ 9000, false, "jumbo-9000" },
	{ 9216, false, "jumbo-9216" },
	{ 16000, false, "16000" },
};

static void mtu_case_desc(const struct mtu_case *c, char *desc)
{
	strscpy(desc, c->name, KUNIT_PARAM_DESC_SIZE);
}

KUNIT_ARRAY_PARAM(mtu, mtu_cases, mtu_case_desc);

static void iavf_xdp_mtu_param_test(struct kunit *test)
{
	const struct mtu_case *c = test->param_value;

	KUNIT_EXPECT_EQ(test, iavf_xdp_mtu_ok(c->mtu), c->ok);
}

/* A max-MTU frame including L2 overhead must fit the HW buffer exactly. */
static void iavf_xdp_mtu_fits_buffer_test(struct kunit *test)
{
	KUNIT_EXPECT_LE(test, IAVF_TEST_MAX_MTU + LIBETH_RX_LL_LEN,
			(int)LIBETH_RX_PAGE_LEN(LIBETH_XDP_HEADROOM));
	KUNIT_EXPECT_GT(test, IAVF_TEST_MAX_MTU + 1 + LIBETH_RX_LL_LEN,
			(int)LIBETH_RX_PAGE_LEN(LIBETH_XDP_HEADROOM));
}

static void iavf_xdp_mtu_default_ok_test(struct kunit *test)
{
	KUNIT_EXPECT_GE(test, IAVF_TEST_MAX_MTU, ETH_DATA_LEN);
}

/* libeth constant invariants */
static void iavf_xdp_headroom_test(struct kunit *test)
{
	KUNIT_EXPECT_GE(test, LIBETH_XDP_HEADROOM, XDP_PACKET_HEADROOM);
	KUNIT_EXPECT_GT(test, LIBETH_XDP_HEADROOM, LIBETH_SKB_HEADROOM);
}

static void iavf_xdp_headroom_aligned_test(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test, LIBETH_XDP_HEADROOM % sizeof(long), 0UL);
}

static void iavf_xdp_headroom_room_after_frame_test(struct kunit *test)
{
	/* adjust_head()/adjust_meta() room once struct xdp_frame is carved
	 * out of the front: encap programs need well over the ~24 bytes the
	 * regular skb headroom leaves.
	 */
	KUNIT_EXPECT_GE(test, (int)LIBETH_XDP_HEADROOM -
			(int)sizeof(struct xdp_frame), 128);
	KUNIT_EXPECT_LT(test, (int)LIBETH_SKB_HEADROOM -
			(int)sizeof(struct xdp_frame), 128);
}

static void iavf_xdp_page_len_shrinks_with_headroom_test(struct kunit *test)
{
	KUNIT_EXPECT_LE(test, LIBETH_RX_PAGE_LEN(LIBETH_XDP_HEADROOM),
			LIBETH_RX_PAGE_LEN(LIBETH_SKB_HEADROOM));
	KUNIT_EXPECT_EQ(test, LIBETH_RX_PAGE_LEN(LIBETH_XDP_HEADROOM) %
			LIBETH_RX_BUF_STRIDE, 0);
}

static void iavf_xdp_page_len_fits_page_test(struct kunit *test)
{
	KUNIT_EXPECT_LE(test, LIBETH_XDP_HEADROOM +
			LIBETH_RX_PAGE_LEN(LIBETH_XDP_HEADROOM),
			(unsigned long)(PAGE_SIZE << LIBETH_RX_PAGE_ORDER));
}

/* max_pkt_size clamp */
struct clamp_case {
	u32 max_frame, buf_len;
	bool xdp;
	u32 want;
	const char *name;
};

static const struct clamp_case clamp_cases[] = {
	{ 16128, 1536, true, 1536, "xdp-clamps-jumbo" },
	{ 16128, 1536, false, 16128, "no-xdp-untouched" },
	{ 1536, 1536, true, 1536, "xdp-equal" },
	{ 1000, 1536, true, 1000, "xdp-smaller-kept" },
	{ 1000, 1536, false, 1000, "no-xdp-smaller" },
	{ 9728, 3072, true, 3072, "xdp-mid" },
	{ 0, 1536, true, 0, "xdp-zero-max" },
	{ 16128, 0, true, 0, "xdp-zero-buf" },
	{ U32_MAX, 2048, true, 2048, "xdp-u32max" },
	{ U32_MAX, 2048, false, U32_MAX, "no-xdp-u32max" },
};

static void clamp_case_desc(const struct clamp_case *c, char *desc)
{
	strscpy(desc, c->name, KUNIT_PARAM_DESC_SIZE);
}

KUNIT_ARRAY_PARAM(clamp, clamp_cases, clamp_case_desc);

static void iavf_xdp_clamp_param_test(struct kunit *test)
{
	const struct clamp_case *c = test->param_value;

	KUNIT_EXPECT_EQ(test,
			iavf_xdp_clamp_max_frame(c->max_frame, c->buf_len,
						 c->xdp), c->want);
}

/* frame_runnable truth table: only eop && !discard && !rx_err runs */
static void iavf_xdp_runnable_truth_table_test(struct kunit *test)
{
	int i;

	for (i = 0; i < 8; i++) {
		bool eop = i & 1, discard = i & 2, err = i & 4;
		bool want = eop && !discard && !err;

		KUNIT_EXPECT_EQ_MSG(test,
				    iavf_xdp_frame_runnable(eop, discard, err),
				    want, "eop=%d discard=%d err=%d",
				    eop, discard, err);
	}
}

static void iavf_xdp_runnable_single_buf_test(struct kunit *test)
{
	KUNIT_EXPECT_TRUE(test, iavf_xdp_frame_runnable(true, false, false));
}

static void iavf_xdp_runnable_mid_frame_test(struct kunit *test)
{
	KUNIT_EXPECT_FALSE(test, iavf_xdp_frame_runnable(false, false, false));
}

static void iavf_xdp_runnable_tail_of_dropped_test(struct kunit *test)
{
	KUNIT_EXPECT_FALSE(test, iavf_xdp_frame_runnable(true, true, false));
}

static void iavf_xdp_runnable_hw_error_test(struct kunit *test)
{
	KUNIT_EXPECT_FALSE(test, iavf_xdp_frame_runnable(true, false, true));
}

/* iavf_run_xdp() with a fake program */
struct fake_ctx {
	struct net_device *dev;
	struct iavf_ring ring;
	struct bpf_prog *prog;
	struct xdp_buff xdp;
	struct xdp_rxq_info rxq;
	u8 *page;
};

static u32 fake_verdict;
static int fake_calls;
static const void *fake_seen_data, *fake_seen_end;

static unsigned int fake_bpf_func(const void *ctx, const struct bpf_insn *insn)
{
	const struct xdp_buff *x = ctx;

	fake_calls++;
	fake_seen_data = x->data;
	fake_seen_end = x->data_end;
	return fake_verdict;
}

static int fake_init(struct kunit *test)
{
	struct fake_ctx *f = kunit_kzalloc(test, sizeof(*f), GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, f);
	f->dev = alloc_etherdev(0);
	KUNIT_ASSERT_NOT_NULL(test, f->dev);
	f->prog = kunit_kzalloc(test, sizeof(*f->prog), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, f->prog);
	f->prog->aux = kunit_kzalloc(test, sizeof(*f->prog->aux), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, f->prog->aux);
	f->prog->bpf_func = fake_bpf_func;
	f->page = (u8 *)__get_free_page(GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, f->page);

	f->ring.netdev = f->dev;
	f->rxq.dev = f->dev;
	xdp_init_buff(&f->xdp, PAGE_SIZE, &f->rxq);
	xdp_prepare_buff(&f->xdp, f->page, LIBETH_XDP_HEADROOM, 100, true);

	fake_verdict = XDP_PASS;
	fake_calls = 0;
	fake_seen_data = NULL;
	fake_seen_end = NULL;
	test->priv = f;
	return 0;
}

static void fake_exit(struct kunit *test)
{
	struct fake_ctx *f = test->priv;

	free_netdev(f->dev);
	free_page((unsigned long)f->page);
}

static u32 run_with(struct kunit *test, u32 verdict)
{
	struct fake_ctx *f = test->priv;

	fake_verdict = verdict;
	return iavf_run_xdp(&f->ring, f->prog, &f->xdp);
}

static void iavf_run_xdp_pass_test(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test, run_with(test, XDP_PASS), (u32)XDP_PASS);
}

static void iavf_run_xdp_drop_test(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test, run_with(test, XDP_DROP), (u32)XDP_DROP);
}

static void iavf_run_xdp_aborted_test(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test, run_with(test, XDP_ABORTED), (u32)XDP_DROP);
}

static void iavf_run_xdp_tx_test(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test, run_with(test, XDP_TX), (u32)XDP_DROP);
}

static void iavf_run_xdp_redirect_test(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test, run_with(test, XDP_REDIRECT), (u32)XDP_DROP);
}

static void iavf_run_xdp_unknown_test(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test, run_with(test, 99), (u32)XDP_DROP);
	KUNIT_EXPECT_EQ(test, run_with(test, U32_MAX), (u32)XDP_DROP);
}

static void iavf_run_xdp_never_returns_other_test(struct kunit *test)
{
	u32 v, act;

	for (v = 0; v < 16; v++) {
		act = run_with(test, v);
		KUNIT_EXPECT_TRUE_MSG(test, act == XDP_PASS || act == XDP_DROP,
				      "verdict %u -> %u", v, act);
	}
}

static void iavf_run_xdp_calls_prog_once_test(struct kunit *test)
{
	run_with(test, XDP_PASS);
	KUNIT_EXPECT_EQ(test, fake_calls, 1);
}

static void iavf_run_xdp_sees_frame_test(struct kunit *test)
{
	struct fake_ctx *f = test->priv;

	run_with(test, XDP_PASS);
	KUNIT_EXPECT_PTR_EQ(test, fake_seen_data, (void *)f->xdp.data);
	KUNIT_EXPECT_PTR_EQ(test, fake_seen_end, (void *)f->xdp.data_end);
	KUNIT_EXPECT_EQ(test, (long)(fake_seen_end - fake_seen_data), 100L);
}

/* skb built from the xdp_buff */
static struct sk_buff *build(struct kunit *test)
{
	struct fake_ctx *f = test->priv;
	struct sk_buff *skb;

	/* the skb owns its own page reference, fake_exit() drops ours */
	get_page(virt_to_page(f->page));
	local_bh_disable();
	skb = iavf_xdp_build_skb(&f->xdp);
	local_bh_enable();
	KUNIT_ASSERT_NOT_NULL(test, skb);
	return skb;
}

static void iavf_xdp_skb_plain_test(struct kunit *test)
{
	struct fake_ctx *f = test->priv;
	struct sk_buff *skb;
	int i;

	for (i = 0; i < 100; i++)
		((u8 *)f->xdp.data)[i] = i;

	skb = build(test);
	KUNIT_EXPECT_EQ(test, skb->len, 100U);
	KUNIT_EXPECT_PTR_EQ(test, (void *)skb->data, (void *)f->xdp.data);
	KUNIT_EXPECT_EQ(test, (int)skb_headroom(skb),
			(int)LIBETH_XDP_HEADROOM);
	KUNIT_EXPECT_EQ(test, skb_metadata_len(skb), (u8)0);
	KUNIT_EXPECT_EQ(test, skb->data[0], (u8)0);
	KUNIT_EXPECT_EQ(test, skb->data[99], (u8)99);
	KUNIT_EXPECT_TRUE(test, skb->pp_recycle);
	kfree_skb(skb);
}

static void iavf_xdp_skb_head_pop_test(struct kunit *test)
{
	struct fake_ctx *f = test->priv;
	struct sk_buff *skb;

	((u8 *)f->xdp.data)[14] = 0xab;
	f->xdp.data += 14;	/* pop an L2 header */
	f->xdp.data_meta = f->xdp.data;

	skb = build(test);
	KUNIT_EXPECT_EQ(test, skb->len, 86U);
	KUNIT_EXPECT_EQ(test, skb->data[0], (u8)0xab);
	KUNIT_EXPECT_EQ(test, (int)skb_headroom(skb),
			(int)LIBETH_XDP_HEADROOM + 14);
	kfree_skb(skb);
}

static void iavf_xdp_skb_head_push_test(struct kunit *test)
{
	struct fake_ctx *f = test->priv;
	struct sk_buff *skb;

	f->xdp.data -= 50;	/* encap */
	f->xdp.data_meta = f->xdp.data;
	memset(f->xdp.data, 0x5a, 50);

	skb = build(test);
	KUNIT_EXPECT_EQ(test, skb->len, 150U);
	KUNIT_EXPECT_EQ(test, skb->data[0], (u8)0x5a);
	KUNIT_EXPECT_EQ(test, skb->data[49], (u8)0x5a);
	KUNIT_EXPECT_EQ(test, (int)skb_headroom(skb),
			(int)LIBETH_XDP_HEADROOM - 50);
	kfree_skb(skb);
}

static void iavf_xdp_skb_max_head_push_test(struct kunit *test)
{
	struct fake_ctx *f = test->priv;
	struct sk_buff *skb;
	int room = f->xdp.data - f->xdp.data_hard_start -
		   sizeof(struct xdp_frame);

	/* what bpf_xdp_adjust_head() allows */
	KUNIT_ASSERT_GT(test, room, 0);
	f->xdp.data -= room;
	f->xdp.data_meta = f->xdp.data;

	skb = build(test);
	KUNIT_EXPECT_EQ(test, skb->len, (unsigned int)(100 + room));
	KUNIT_EXPECT_EQ(test, (int)skb_headroom(skb),
			(int)LIBETH_XDP_HEADROOM - room);
	kfree_skb(skb);
}

static void iavf_xdp_skb_tail_trim_test(struct kunit *test)
{
	struct fake_ctx *f = test->priv;
	struct sk_buff *skb;

	f->xdp.data_end -= 40;

	skb = build(test);
	KUNIT_EXPECT_EQ(test, skb->len, 60U);
	kfree_skb(skb);
}

static void iavf_xdp_skb_tail_grow_test(struct kunit *test)
{
	struct fake_ctx *f = test->priv;
	struct sk_buff *skb;

	f->xdp.data_end += 20;

	skb = build(test);
	KUNIT_EXPECT_EQ(test, skb->len, 120U);
	kfree_skb(skb);
}

static void iavf_xdp_skb_empty_payload_test(struct kunit *test)
{
	struct fake_ctx *f = test->priv;
	struct sk_buff *skb;

	f->xdp.data_end = f->xdp.data;

	skb = build(test);
	KUNIT_EXPECT_EQ(test, skb->len, 0U);
	kfree_skb(skb);
}

static void iavf_xdp_skb_meta_test(struct kunit *test)
{
	struct fake_ctx *f = test->priv;
	struct sk_buff *skb;

	f->xdp.data_meta = f->xdp.data - 16;
	memset(f->xdp.data_meta, 0xcd, 16);

	skb = build(test);
	KUNIT_EXPECT_EQ(test, skb->len, 100U);
	KUNIT_EXPECT_EQ(test, skb_metadata_len(skb), (u8)16);
	KUNIT_EXPECT_EQ(test, ((u8 *)skb->data)[-1], (u8)0xcd);
	kfree_skb(skb);
}

static void iavf_xdp_skb_meta_max_test(struct kunit *test)
{
	struct fake_ctx *f = test->priv;
	struct sk_buff *skb;

	f->xdp.data_meta = f->xdp.data - 32;

	skb = build(test);
	KUNIT_EXPECT_EQ(test, skb_metadata_len(skb), (u8)32);
	kfree_skb(skb);
}

static void iavf_xdp_skb_no_meta_when_equal_test(struct kunit *test)
{
	struct fake_ctx *f = test->priv;
	struct sk_buff *skb;

	f->xdp.data_meta = f->xdp.data;

	skb = build(test);
	KUNIT_EXPECT_FALSE(test, skb_metadata_len(skb));
	kfree_skb(skb);
}

static void iavf_xdp_skb_shares_buffer_test(struct kunit *test)
{
	struct fake_ctx *f = test->priv;
	struct sk_buff *skb;

	skb = build(test);
	KUNIT_EXPECT_PTR_EQ(test, (void *)skb->head,
			    (void *)f->xdp.data_hard_start);
	KUNIT_EXPECT_GE(test, skb->truesize, (unsigned int)PAGE_SIZE);
	kfree_skb(skb);
}

static struct kunit_case iavf_txrx_test_cases[] = {
	KUNIT_CASE_PARAM(iavf_xdp_mtu_param_test, mtu_gen_params),
	KUNIT_CASE(iavf_xdp_mtu_ok_test),
	KUNIT_CASE(iavf_xdp_mtu_fits_buffer_test),
	KUNIT_CASE(iavf_xdp_mtu_default_ok_test),
	KUNIT_CASE(iavf_xdp_headroom_test),
	KUNIT_CASE(iavf_xdp_headroom_aligned_test),
	KUNIT_CASE(iavf_xdp_headroom_room_after_frame_test),
	KUNIT_CASE(iavf_xdp_page_len_shrinks_with_headroom_test),
	KUNIT_CASE(iavf_xdp_page_len_fits_page_test),
	KUNIT_CASE_PARAM(iavf_xdp_clamp_param_test, clamp_gen_params),
	KUNIT_CASE(iavf_xdp_runnable_truth_table_test),
	KUNIT_CASE(iavf_xdp_runnable_single_buf_test),
	KUNIT_CASE(iavf_xdp_runnable_mid_frame_test),
	KUNIT_CASE(iavf_xdp_runnable_tail_of_dropped_test),
	KUNIT_CASE(iavf_xdp_runnable_hw_error_test),
	KUNIT_CASE(iavf_run_xdp_pass_test),
	KUNIT_CASE(iavf_run_xdp_drop_test),
	KUNIT_CASE(iavf_run_xdp_aborted_test),
	KUNIT_CASE(iavf_run_xdp_tx_test),
	KUNIT_CASE(iavf_run_xdp_redirect_test),
	KUNIT_CASE(iavf_run_xdp_unknown_test),
	KUNIT_CASE(iavf_run_xdp_never_returns_other_test),
	KUNIT_CASE(iavf_run_xdp_calls_prog_once_test),
	KUNIT_CASE(iavf_run_xdp_sees_frame_test),
	KUNIT_CASE(iavf_xdp_skb_plain_test),
	KUNIT_CASE(iavf_xdp_skb_head_pop_test),
	KUNIT_CASE(iavf_xdp_skb_head_push_test),
	KUNIT_CASE(iavf_xdp_skb_max_head_push_test),
	KUNIT_CASE(iavf_xdp_skb_tail_trim_test),
	KUNIT_CASE(iavf_xdp_skb_tail_grow_test),
	KUNIT_CASE(iavf_xdp_skb_empty_payload_test),
	KUNIT_CASE(iavf_xdp_skb_meta_test),
	KUNIT_CASE(iavf_xdp_skb_meta_max_test),
	KUNIT_CASE(iavf_xdp_skb_no_meta_when_equal_test),
	KUNIT_CASE(iavf_xdp_skb_shares_buffer_test),
	{}
};

static struct kunit_suite iavf_txrx_test_suite = {
	.name = "iavf_txrx",
	.init = fake_init,
	.exit = fake_exit,
	.test_cases = iavf_txrx_test_cases,
};

/* Rx-path harness: drives the real iavf_clean_rx_irq() against a real
 * libeth page pool, a software descriptor ring and a fake bpf_prog.
 */
#define RX_COUNT	64

struct rx_ctx {
	struct device *dev;
	struct net_device *netdev;
	struct iavf_adapter *adapter;
	struct iavf_vsi vsi;
	struct iavf_q_vector qv;
	struct iavf_ring ring;
	struct bpf_prog *prog;
	u8 tail_reg[8];
};

static int rx_poll(struct napi_struct *napi, int budget)
{
	return 0;
}

static void rx_ctx_teardown(void *data)
{
	struct rx_ctx *c = data;

	napi_disable(&c->qv.napi);
	iavf_free_rx_resources(&c->ring);
	netif_napi_del(&c->qv.napi);
	free_netdev(c->netdev);
}

static int rx_init(struct kunit *test)
{
	struct rx_ctx *c = kunit_kzalloc(test, sizeof(*c), GFP_KERNEL);
	int ret;

	KUNIT_ASSERT_NOT_NULL(test, c);
	c->dev = kunit_device_register(test, "iavf-kunit-rx");
	KUNIT_ASSERT_FALSE(test, IS_ERR_OR_NULL(c->dev));
	c->adapter = kunit_kzalloc(test, sizeof(*c->adapter), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, c->adapter);
	mutex_init(&c->adapter->crit_lock);
	c->prog = kunit_kzalloc(test, sizeof(*c->prog), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, c->prog);
	c->prog->aux = kunit_kzalloc(test, sizeof(*c->prog->aux), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, c->prog->aux);
	c->prog->bpf_func = fake_bpf_func;

	c->netdev = alloc_etherdev(0);
	KUNIT_ASSERT_NOT_NULL(test, c->netdev);
	SET_NETDEV_DEV(c->netdev, c->dev);
	netif_napi_add(c->netdev, &c->qv.napi, rx_poll);
	napi_enable(&c->qv.napi);

	c->vsi.back = c->adapter;
	c->vsi.netdev = c->netdev;
	c->qv.adapter = c->adapter;
	c->qv.vsi = &c->vsi;
	c->qv.rx.ring = &c->ring;
	c->ring.count = RX_COUNT;
	c->ring.netdev = c->netdev;
	c->ring.vsi = &c->vsi;
	c->ring.q_vector = &c->qv;
	c->ring.tail = c->tail_reg;

	/* Buffers are created XDP-capable, as they are with a program attached */
	mutex_lock(&c->adapter->crit_lock);
	rcu_assign_pointer(c->adapter->xdp_prog, c->prog);
	ret = iavf_setup_rx_descriptors(&c->ring);
	rcu_assign_pointer(c->adapter->xdp_prog, NULL);
	rcu_assign_pointer(c->ring.xdp_prog, NULL);
	mutex_unlock(&c->adapter->crit_lock);
	if (ret) {
		netif_napi_del(&c->qv.napi);
		free_netdev(c->netdev);
	}
	KUNIT_ASSERT_EQ(test, ret, 0);

	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test, rx_ctx_teardown, c), 0);
	KUNIT_ASSERT_FALSE(test, iavf_alloc_rx_buffers(&c->ring,
						       IAVF_DESC_UNUSED(&c->ring)));

	fake_verdict = XDP_PASS;
	fake_calls = 0;
	test->priv = c;
	return 0;
}

static void rx_attach(struct rx_ctx *c, u32 verdict)
{
	fake_verdict = verdict;
	rcu_assign_pointer(c->ring.xdp_prog, c->prog);
}

/* Hardware writes back descriptor @n slots past next_to_clean. */
static void rx_post(struct rx_ctx *c, unsigned int n, unsigned int size,
		    bool eop, bool err)
{
	u16 idx = (c->ring.next_to_clean + n) % RX_COUNT;
	struct libeth_fqe *fqe = &c->ring.rx_fqes[idx];
	union iavf_rx_desc *d = IAVF_RX_DESC(&c->ring, idx);
	struct ethhdr *eth = page_address(fqe->page) + fqe->offset;
	u64 q = BIT_ULL(IAVF_RX_DESC_STATUS_DD_SHIFT) |
		((u64)size << IAVF_RXD_QW1_LENGTH_PBUF_SHIFT);

	memset(eth, 0, size);
	eth->h_proto = htons(ETH_P_IP);
	if (eop)
		q |= BIT_ULL(IAVF_RX_DESC_STATUS_EOF_SHIFT);
	if (err)
		q |= BIT_ULL(IAVF_RXD_QW1_ERROR_SHIFT);
	d->wb.qword1.status_error_len = cpu_to_le64(q);
}

static int rx_clean(struct rx_ctx *c, int budget)
{
	int ret;

	local_bh_disable();
	ret = iavf_clean_rx_irq(&c->ring, budget);
	local_bh_enable();
	return ret;
}

static void rx_no_prog_delivers_test(struct kunit *test)
{
	struct rx_ctx *c = test->priv;

	for (int i = 0; i < 3; i++)
		rx_post(c, i, 100, true, false);
	KUNIT_EXPECT_EQ(test, rx_clean(c, 64), 3);
	KUNIT_EXPECT_EQ(test, fake_calls, 0);
	KUNIT_EXPECT_NULL(test, c->ring.skb);
	KUNIT_EXPECT_EQ(test, c->ring.next_to_clean, 3);
}

static void rx_no_prog_multi_desc_test(struct kunit *test)
{
	struct rx_ctx *c = test->priv;

	rx_post(c, 0, 1024, false, false);
	KUNIT_EXPECT_EQ(test, rx_clean(c, 64), 0);
	KUNIT_EXPECT_NOT_NULL(test, c->ring.skb);

	rx_post(c, 0, 200, true, false);
	KUNIT_EXPECT_EQ(test, rx_clean(c, 64), 1);
	KUNIT_EXPECT_NULL(test, c->ring.skb);
	KUNIT_EXPECT_EQ(test, fake_calls, 0);
}

static void rx_pass_delivers_test(struct kunit *test)
{
	struct rx_ctx *c = test->priv;

	rx_attach(c, XDP_PASS);
	for (int i = 0; i < 5; i++)
		rx_post(c, i, 100, true, false);
	KUNIT_EXPECT_EQ(test, rx_clean(c, 64), 5);
	KUNIT_EXPECT_EQ(test, fake_calls, 5);
	KUNIT_EXPECT_NULL(test, c->ring.skb);
	KUNIT_EXPECT_EQ(test, c->ring.stats.packets, 5ULL);
}

static void rx_drop_counts_toward_budget_test(struct kunit *test)
{
	struct rx_ctx *c = test->priv;

	rx_attach(c, XDP_DROP);
	for (int i = 0; i < 40; i++)
		rx_post(c, i, 100, true, false);

	KUNIT_EXPECT_EQ(test, rx_clean(c, 16), 16);
	KUNIT_EXPECT_EQ(test, fake_calls, 16);
	KUNIT_EXPECT_EQ(test, rx_clean(c, 64), 24);
	KUNIT_EXPECT_EQ(test, fake_calls, 40);
	KUNIT_EXPECT_EQ(test, rx_clean(c, 64), 0);
	KUNIT_EXPECT_EQ(test, c->ring.stats.packets, 40ULL);
	KUNIT_EXPECT_EQ(test, c->ring.stats.bytes, 4000ULL);
}

static void rx_drop_budget_zero_test(struct kunit *test)
{
	struct rx_ctx *c = test->priv;

	rx_attach(c, XDP_DROP);
	rx_post(c, 0, 100, true, false);
	KUNIT_EXPECT_EQ(test, rx_clean(c, 0), 0);
	KUNIT_EXPECT_EQ(test, fake_calls, 0);
	KUNIT_EXPECT_EQ(test, c->ring.next_to_clean, 0);
}

static void rx_unsupported_verdicts_dropped_test(struct kunit *test)
{
	static const u32 verdicts[] = { XDP_ABORTED, XDP_TX, XDP_REDIRECT, 99 };
	struct rx_ctx *c = test->priv;

	for (int v = 0; v < ARRAY_SIZE(verdicts); v++) {
		rx_attach(c, verdicts[v]);
		rx_post(c, 0, 100, true, false);
		KUNIT_EXPECT_EQ(test, rx_clean(c, 64), 1);
		KUNIT_EXPECT_NULL(test, c->ring.skb);
	}
	KUNIT_EXPECT_EQ(test, fake_calls, (int)ARRAY_SIZE(verdicts));
}

static void rx_multi_desc_dropped_through_eop_test(struct kunit *test)
{
	struct rx_ctx *c = test->priv;

	rx_attach(c, XDP_PASS);
	rx_post(c, 0, 1024, false, false);
	rx_post(c, 1, 1024, false, false);
	rx_post(c, 2, 100, true, false);
	rx_post(c, 3, 100, true, false);	/* next, independent frame */

	/* The 3-descriptor frame is dropped without running the program
	 * and counts as one packet; the following frame runs normally.
	 */
	KUNIT_EXPECT_EQ(test, rx_clean(c, 64), 2);
	KUNIT_EXPECT_EQ(test, fake_calls, 1);
	KUNIT_EXPECT_FALSE(test, c->ring.xdp_discard);
	KUNIT_EXPECT_NULL(test, c->ring.skb);
}

static void rx_discard_survives_across_calls_test(struct kunit *test)
{
	struct rx_ctx *c = test->priv;

	rx_attach(c, XDP_PASS);
	rx_post(c, 0, 1024, false, false);
	KUNIT_EXPECT_EQ(test, rx_clean(c, 64), 0);
	KUNIT_EXPECT_TRUE(test, c->ring.xdp_discard);
	KUNIT_EXPECT_EQ(test, fake_calls, 0);

	rx_post(c, 0, 100, true, false);
	KUNIT_EXPECT_EQ(test, rx_clean(c, 64), 1);
	KUNIT_EXPECT_FALSE(test, c->ring.xdp_discard);
	KUNIT_EXPECT_EQ(test, fake_calls, 0);

	rx_post(c, 0, 100, true, false);
	KUNIT_EXPECT_EQ(test, rx_clean(c, 64), 1);
	KUNIT_EXPECT_EQ(test, fake_calls, 1);
}

static void rx_hw_error_never_runs_prog_test(struct kunit *test)
{
	struct rx_ctx *c = test->priv;

	rx_attach(c, XDP_PASS);
	rx_post(c, 0, 100, true, true);
	rx_post(c, 1, 100, true, false);
	KUNIT_EXPECT_EQ(test, rx_clean(c, 64), 2);
	KUNIT_EXPECT_EQ(test, fake_calls, 1);
	KUNIT_EXPECT_NULL(test, c->ring.skb);
}

static void rx_partial_frame_sets_discard_test(struct kunit *test)
{
	struct rx_ctx *c = test->priv;

	rx_attach(c, XDP_PASS);
	rx_post(c, 0, 1024, false, false);
	rx_clean(c, 64);
	KUNIT_ASSERT_TRUE(test, c->ring.xdp_discard);
}

static void rx_ring_wraps_test(struct kunit *test)
{
	struct rx_ctx *c = test->priv;
	unsigned int total = 0;

	rx_attach(c, XDP_DROP);
	for (int round = 0; round < 5; round++) {
		for (int i = 0; i < 50; i++)
			rx_post(c, i, 100, true, false);
		total += rx_clean(c, 64);
	}
	KUNIT_EXPECT_EQ(test, total, 250U);
	KUNIT_EXPECT_EQ(test, fake_calls, 250);
	KUNIT_EXPECT_LT(test, (int)c->ring.next_to_clean, RX_COUNT);
}

static void rx_prog_swap_midstream_test(struct kunit *test)
{
	struct rx_ctx *c = test->priv;

	rx_attach(c, XDP_DROP);
	rx_post(c, 0, 100, true, false);
	KUNIT_EXPECT_EQ(test, rx_clean(c, 64), 1);

	rcu_assign_pointer(c->ring.xdp_prog, NULL);
	rx_post(c, 0, 100, true, false);
	KUNIT_EXPECT_EQ(test, rx_clean(c, 64), 1);
	KUNIT_EXPECT_EQ(test, fake_calls, 1);

	rx_attach(c, XDP_DROP);
	rx_post(c, 0, 100, true, false);
	KUNIT_EXPECT_EQ(test, rx_clean(c, 64), 1);
	KUNIT_EXPECT_EQ(test, fake_calls, 2);
}

static struct kunit_case iavf_rx_test_cases[] = {
	KUNIT_CASE(rx_no_prog_delivers_test),
	KUNIT_CASE(rx_no_prog_multi_desc_test),
	KUNIT_CASE(rx_pass_delivers_test),
	KUNIT_CASE(rx_drop_counts_toward_budget_test),
	KUNIT_CASE(rx_drop_budget_zero_test),
	KUNIT_CASE(rx_unsupported_verdicts_dropped_test),
	KUNIT_CASE(rx_multi_desc_dropped_through_eop_test),
	KUNIT_CASE(rx_discard_survives_across_calls_test),
	KUNIT_CASE(rx_hw_error_never_runs_prog_test),
	KUNIT_CASE(rx_partial_frame_sets_discard_test),
	KUNIT_CASE(rx_ring_wraps_test),
	KUNIT_CASE(rx_prog_swap_midstream_test),
	{}
};

static struct kunit_suite iavf_rx_test_suite = {
	.name = "iavf_rx_path",
	.init = rx_init,
	.test_cases = iavf_rx_test_cases,
};

/* iavf_xdp_setup(): crit_lock handling and ring assignment */
struct setup_ctx {
	struct net_device *netdev;
	struct iavf_adapter *adapter;
	struct bpf_prog *prog;
};

static void setup_teardown(void *data)
{
	struct setup_ctx *c = data;

	free_netdev(c->netdev);
}

static int setup_init(struct kunit *test)
{
	struct setup_ctx *c = kunit_kzalloc(test, sizeof(*c), GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, c);
	c->netdev = alloc_etherdev(sizeof(struct iavf_adapter));
	KUNIT_ASSERT_NOT_NULL(test, c->netdev);
	KUNIT_ASSERT_EQ(test, kunit_add_action_or_reset(test, setup_teardown, c), 0);
	c->adapter = netdev_priv(c->netdev);
	c->adapter->netdev = c->netdev;
	c->adapter->num_active_queues = 2;
	c->adapter->rx_rings = kunit_kcalloc(test, 2, sizeof(struct iavf_ring),
					     GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, c->adapter->rx_rings);
	mutex_init(&c->adapter->crit_lock);
	c->prog = kunit_kzalloc(test, sizeof(*c->prog), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, c->prog);
	c->prog->aux = kunit_kzalloc(test, sizeof(*c->prog->aux), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, c->prog->aux);
	atomic64_set(&c->prog->aux->refcnt, 4);
	test->priv = c;
	return 0;
}

/* The netdev is not running, so no reset is scheduled; with a prog the
 * rings are only assigned on a program swap (no reset needed).
 */
static void setup_attach_detach_test(struct kunit *test)
{
	struct setup_ctx *c = test->priv;
	struct iavf_adapter *a = c->adapter;

	KUNIT_ASSERT_EQ(test, iavf_xdp_setup(a, c->prog, NULL), 0);
	KUNIT_EXPECT_PTR_EQ(test, rcu_access_pointer(a->xdp_prog), c->prog);
	KUNIT_EXPECT_FALSE(test, mutex_is_locked(&a->crit_lock));

	KUNIT_ASSERT_EQ(test, iavf_xdp_setup(a, NULL, NULL), 0);
	KUNIT_EXPECT_NULL(test, rcu_access_pointer(a->xdp_prog));
	KUNIT_EXPECT_NULL(test, rcu_access_pointer(a->rx_rings[0].xdp_prog));
	KUNIT_EXPECT_NULL(test, rcu_access_pointer(a->rx_rings[1].xdp_prog));
	KUNIT_EXPECT_FALSE(test, mutex_is_locked(&a->crit_lock));
}

static void setup_swap_updates_rings_test(struct kunit *test)
{
	struct setup_ctx *c = test->priv;
	struct iavf_adapter *a = c->adapter;
	struct bpf_prog *other = kunit_kzalloc(test, sizeof(*other), GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, other);
	other->aux = c->prog->aux;
	KUNIT_ASSERT_EQ(test, iavf_xdp_setup(a, c->prog, NULL), 0);
	KUNIT_ASSERT_EQ(test, iavf_xdp_setup(a, other, NULL), 0);
	KUNIT_EXPECT_PTR_EQ(test, rcu_access_pointer(a->xdp_prog), other);
	KUNIT_EXPECT_PTR_EQ(test, rcu_access_pointer(a->rx_rings[0].xdp_prog), other);
	KUNIT_EXPECT_PTR_EQ(test, rcu_access_pointer(a->rx_rings[1].xdp_prog), other);
}

/* hv_netvsc pushes its program from inside register_netdevice(), where
 * iavf_finish_config() already holds crit_lock: no trylock, no unlock.
 */
static void setup_registering_task_keeps_lock_test(struct kunit *test)
{
	struct setup_ctx *c = test->priv;
	struct iavf_adapter *a = c->adapter;

	mutex_lock(&a->crit_lock);
	WRITE_ONCE(a->register_task, current);
	KUNIT_ASSERT_EQ(test, iavf_xdp_setup(a, c->prog, NULL), 0);
	WRITE_ONCE(a->register_task, NULL);
	KUNIT_EXPECT_TRUE(test, mutex_is_locked(&a->crit_lock));
	KUNIT_EXPECT_PTR_EQ(test, rcu_access_pointer(a->xdp_prog), c->prog);
	mutex_unlock(&a->crit_lock);
}

static void setup_removed_adapter_test(struct kunit *test)
{
	struct setup_ctx *c = test->priv;
	struct iavf_adapter *a = c->adapter;

	a->state = __IAVF_REMOVE;
	KUNIT_EXPECT_EQ(test, iavf_xdp_setup(a, c->prog, NULL), -ENODEV);
	KUNIT_EXPECT_NULL(test, rcu_access_pointer(a->xdp_prog));
	KUNIT_EXPECT_FALSE(test, mutex_is_locked(&a->crit_lock));
}

static void setup_mtu_too_large_test(struct kunit *test)
{
	struct setup_ctx *c = test->priv;
	struct iavf_adapter *a = c->adapter;

	c->netdev->mtu = 9000;
	KUNIT_EXPECT_EQ(test, iavf_xdp_setup(a, c->prog, NULL), -EOPNOTSUPP);
	KUNIT_EXPECT_NULL(test, rcu_access_pointer(a->xdp_prog));
}

/* Rings not allocated yet (e.g. called before the first config): must not
 * touch rx_rings.
 */
static void setup_without_rings_test(struct kunit *test)
{
	struct setup_ctx *c = test->priv;
	struct iavf_adapter *a = c->adapter;

	a->rx_rings = NULL;
	KUNIT_EXPECT_EQ(test, iavf_xdp_setup(a, c->prog, NULL), 0);
	KUNIT_EXPECT_PTR_EQ(test, rcu_access_pointer(a->xdp_prog), c->prog);
	KUNIT_EXPECT_EQ(test, iavf_xdp_setup(a, NULL, NULL), 0);
}

static struct kunit_case iavf_setup_test_cases[] = {
	KUNIT_CASE(setup_attach_detach_test),
	KUNIT_CASE(setup_swap_updates_rings_test),
	KUNIT_CASE(setup_registering_task_keeps_lock_test),
	KUNIT_CASE(setup_removed_adapter_test),
	KUNIT_CASE(setup_mtu_too_large_test),
	KUNIT_CASE(setup_without_rings_test),
	{}
};

static struct kunit_suite iavf_setup_test_suite = {
	.name = "iavf_xdp_setup",
	.init = setup_init,
	.test_cases = iavf_setup_test_cases,
};

kunit_test_suites(&iavf_txrx_test_suite, &iavf_rx_test_suite, &iavf_setup_test_suite);

MODULE_DESCRIPTION("iavf XDP helper KUnit tests");
MODULE_LICENSE("GPL");
