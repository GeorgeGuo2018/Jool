#include "mod/common/send_packet.h"

#include <linux/version.h>

#include "mod/common/linux_version.h"
#include "mod/common/log.h"
#include "mod/common/icmp_wrapper.h"
#include "mod/common/packet.h"
#include "mod/common/route.h"

static unsigned int get_nexthop_mtu(struct packet *pkt)
{
#ifndef UNIT_TESTING
	return skb_dst(pkt->skb)->dev->mtu;
#else
	return 1500;
#endif
}

/*
 * Returns true if GSO fixed everything and MTU is no longer an issue.
 * Returns false if GSO did nothing and MTU needs to be addressed still.
 * No other outcomes.
 */
static bool handle_gso(struct packet *in)
{
	/*
	 * This is how I understand GSO:
	 *
	 * If gso_size > 0, then the skb is meant to be somehow "divided" (IP
	 * fragmented or TCP segmented) at some point regardless of DF and
	 * skb->ignore_df due to local reasons.
	 *
	 * (For example, one such reason might be that the packet was generated
	 * locally, made to length massively because of a large write() buffer,
	 * and meant to be divided as late as possible. Though Jool does not
	 * translate its own namespace's traffic, this can happen when
	 * forwarding traffic from some other namespace, because virtual
	 * interfaces do not respect MTU. Again, packets are meant to be divided
	 * as late as possible, which usually means "in the outgoing physical
	 * interface". If the packet is only traveling through namespaces, it
	 * can very well never be divided despite violating every MTU along the
	 * way.)
	 *
	 * Therefore, if GSO is intended to happen, Jool should usually not
	 * bounce Fragmentation Neededs back.
	 *
	 * The details on how this should be implemented, however, are a little
	 * dodgy to me. Should I pay attention to type flags other than
	 * SKB_GSO_TCPV4 and SKB_GSO_TCPV6? Why does SKB_GSO_UDP not care about
	 * the network layer's protocol? What if gso_size does not exceed the
	 * MTU of the next interface, but does exceed the MTU of some future
	 * interface? This code will probably need to evolve depending on future
	 * experience, and this first version tries to be as conservative as
	 * possible.
	 *
	 * For reference, this code works correctly on TCP packets traveling
	 * through only veth pair interfaces, and was prompted by this bug
	 * report:
	 * https://mail-lists.nic.mx/pipermail/jool-list/2018-July/000198.html
	 * Anything else might or might not work.
	 *
	 * Handle this code with care. Offloading is a very awkwardly convoluted
	 * topic.
	 *
	 * TODO https://www.kernel.org/doc/Documentation/networking/segmentation-offloads.txt
	 * This documentations talks about some SCTP quirk. I'm not yet sure if
	 * it affects us.
	 */
	return skb_is_gso(in->skb);
}

static verdict whine_if_too_big(struct xlation *state)
{
	struct packet *in = &state->in;
	struct packet *out = &state->out;
	unsigned int len;
	unsigned int mtu;

	if (handle_gso(in))
		return VERDICT_CONTINUE;
	if (pkt_l3_proto(in) == L3PROTO_IPV4 && !is_df_set(pkt_ip4_hdr(in)))
		return VERDICT_CONTINUE;

	len = pkt_len(out);
	mtu = get_nexthop_mtu(out);
	if (len > mtu) {
		/*
		 * We don't have to worry about ICMP errors causing this because
		 * the translate code already truncates them.
		 */
		log_debug("Packet is too big (len: %u, mtu: %u).", len, mtu);

		switch (pkt_l3_proto(out)) {
		case L3PROTO_IPV6:
			mtu = max(576u, mtu - 20u);
			break;
		case L3PROTO_IPV4:
			mtu = max(1280u, mtu + 20u);
			break;
		}

		return drop_icmp(state, JSTAT_PKT_TOO_BIG,
				ICMPERR_FRAG_NEEDED, mtu);
	}

	return VERDICT_CONTINUE;
}

verdict sendpkt_send(struct xlation *state)
{
	struct packet *out = &state->out;
	verdict result;
	int error;

	if (!route(state->jool.ns, out)) {
		kfree_skb(out->skb);
		return untranslatable(state, JSTAT_FAILED_ROUTES);
	}

	out->skb->dev = skb_dst(out->skb)->dev;
	log_debug("Sending skb.");

	result = whine_if_too_big(state);
	if (result != VERDICT_CONTINUE) {
		kfree_skb(out->skb);
		return result;
	}

#if LINUX_VERSION_AT_LEAST(3, 16, 0, 7, 2)
	out->skb->ignore_df = true; /* FFS, kernel. */
#else
	out->skb->local_df = true; /* FFS, kernel. */
#endif

	/* skb_log(out->skb, "Translated packet"); */

	/*
	 * Implicit kfree_skb(out->skb) here.
	 *
	 * At time of writing, RHEL hasn't yet upgraded to the messy version of
	 * dst_output().
	 */
#if LINUX_VERSION_AT_LEAST(4, 4, 0, 9999, 0)
	error = dst_output(state->jool.ns, NULL, out->skb);
#else
	error = dst_output(out->skb);
#endif
	if (error) {
		log_debug("dst_output() returned errcode %d.", error);
		return drop(state, JSTAT_DST_OUTPUT);
	}

	return VERDICT_CONTINUE;
}
