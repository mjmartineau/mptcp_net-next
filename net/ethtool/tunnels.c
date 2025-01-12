// SPDX-License-Identifier: GPL-2.0-only

#include <linux/ethtool_netlink.h>
#include <net/udp_tunnel.h>

#include "bitset.h"
#include "common.h"
#include "netlink.h"

static const struct nla_policy
ethtool_tunnel_info_policy[ETHTOOL_A_TUNNEL_INFO_MAX + 1] = {
	[ETHTOOL_A_TUNNEL_INFO_UNSPEC]		= { .type = NLA_REJECT },
	[ETHTOOL_A_TUNNEL_INFO_HEADER]		= { .type = NLA_NESTED },
};

static_assert(ETHTOOL_UDP_TUNNEL_TYPE_VXLAN == ilog2(UDP_TUNNEL_TYPE_VXLAN));
static_assert(ETHTOOL_UDP_TUNNEL_TYPE_GENEVE == ilog2(UDP_TUNNEL_TYPE_GENEVE));
static_assert(ETHTOOL_UDP_TUNNEL_TYPE_VXLAN_GPE ==
	      ilog2(UDP_TUNNEL_TYPE_VXLAN_GPE));

static ssize_t
ethnl_tunnel_info_reply_size(const struct ethnl_req_info *req_base,
			     struct netlink_ext_ack *extack)
{
	bool compact = req_base->flags & ETHTOOL_FLAG_COMPACT_BITSETS;
	const struct udp_tunnel_nic_info *info;
	unsigned int i;
	size_t size;
	int ret;

	info = req_base->dev->udp_tunnel_nic_info;
	if (!info) {
		NL_SET_ERR_MSG(extack,
			       "device does not report tunnel offload info");
		return -EOPNOTSUPP;
	}

	size =	nla_total_size(0); /* _INFO_UDP_PORTS */

	for (i = 0; i < UDP_TUNNEL_NIC_MAX_TABLES; i++) {
		if (!info->tables[i].n_entries)
			return size;

		size += nla_total_size(0); /* _UDP_TABLE */
		size +=	nla_total_size(sizeof(u32)); /* _UDP_TABLE_SIZE */
		ret = ethnl_bitset32_size(&info->tables[i].tunnel_types, NULL,
					  __ETHTOOL_UDP_TUNNEL_TYPE_CNT,
					  udp_tunnel_type_names, compact);
		if (ret < 0)
			return ret;
		size += ret;

		size += udp_tunnel_nic_dump_size(req_base->dev, i);
	}

	return size;
}

static int
ethnl_tunnel_info_fill_reply(const struct ethnl_req_info *req_base,
			     struct sk_buff *skb)
{
	bool compact = req_base->flags & ETHTOOL_FLAG_COMPACT_BITSETS;
	const struct udp_tunnel_nic_info *info;
	struct nlattr *ports, *table;
	unsigned int i;

	info = req_base->dev->udp_tunnel_nic_info;
	if (!info)
		return -EOPNOTSUPP;

	ports = nla_nest_start(skb, ETHTOOL_A_TUNNEL_INFO_UDP_PORTS);
	if (!ports)
		return -EMSGSIZE;

	for (i = 0; i < UDP_TUNNEL_NIC_MAX_TABLES; i++) {
		if (!info->tables[i].n_entries)
			break;

		table = nla_nest_start(skb, ETHTOOL_A_TUNNEL_UDP_TABLE);
		if (!table)
			goto err_cancel_ports;

		if (nla_put_u32(skb, ETHTOOL_A_TUNNEL_UDP_TABLE_SIZE,
				info->tables[i].n_entries))
			goto err_cancel_table;

		if (ethnl_put_bitset32(skb, ETHTOOL_A_TUNNEL_UDP_TABLE_TYPES,
				       &info->tables[i].tunnel_types, NULL,
				       __ETHTOOL_UDP_TUNNEL_TYPE_CNT,
				       udp_tunnel_type_names, compact))
			goto err_cancel_table;

		if (udp_tunnel_nic_dump_write(req_base->dev, i, skb))
			goto err_cancel_table;

		nla_nest_end(skb, table);
	}

	nla_nest_end(skb, ports);

	return 0;

err_cancel_table:
	nla_nest_cancel(skb, table);
err_cancel_ports:
	nla_nest_cancel(skb, ports);
	return -EMSGSIZE;
}

static int
ethnl_tunnel_info_req_parse(struct ethnl_req_info *req_info,
			    const struct nlmsghdr *nlhdr, struct net *net,
			    struct netlink_ext_ack *extack, bool require_dev)
{
	struct nlattr *tb[ETHTOOL_A_TUNNEL_INFO_MAX + 1];
	int ret;

	ret = nlmsg_parse(nlhdr, GENL_HDRLEN, tb, ETHTOOL_A_TUNNEL_INFO_MAX,
			  ethtool_tunnel_info_policy, extack);
	if (ret < 0)
		return ret;

	return ethnl_parse_header_dev_get(req_info,
					  tb[ETHTOOL_A_TUNNEL_INFO_HEADER],
					  net, extack, require_dev);
}

int ethnl_tunnel_info_doit(struct sk_buff *skb, struct genl_info *info)
{
	struct ethnl_req_info req_info = {};
	struct sk_buff *rskb;
	void *reply_payload;
	int reply_len;
	int ret;

	ret = ethnl_tunnel_info_req_parse(&req_info, info->nlhdr,
					  genl_info_net(info), info->extack,
					  true);
	if (ret < 0)
		return ret;

	rtnl_lock();
	ret = ethnl_tunnel_info_reply_size(&req_info, info->extack);
	if (ret < 0)
		goto err_unlock_rtnl;
	reply_len = ret + ethnl_reply_header_size();

	rskb = ethnl_reply_init(reply_len, req_info.dev,
				ETHTOOL_MSG_TUNNEL_INFO_GET,
				ETHTOOL_A_TUNNEL_INFO_HEADER,
				info, &reply_payload);
	if (!rskb) {
		ret = -ENOMEM;
		goto err_unlock_rtnl;
	}

	ret = ethnl_tunnel_info_fill_reply(&req_info, rskb);
	if (ret)
		goto err_free_msg;
	rtnl_unlock();
	dev_put(req_info.dev);
	genlmsg_end(rskb, reply_payload);

	return genlmsg_reply(rskb, info);

err_free_msg:
	nlmsg_free(rskb);
err_unlock_rtnl:
	rtnl_unlock();
	dev_put(req_info.dev);
	return ret;
}

struct ethnl_tunnel_info_dump_ctx {
	struct ethnl_req_info	req_info;
	int			pos_hash;
	int			pos_idx;
};

int ethnl_tunnel_info_start(struct netlink_callback *cb)
{
	struct ethnl_tunnel_info_dump_ctx *ctx = (void *)cb->ctx;
	int ret;

	BUILD_BUG_ON(sizeof(*ctx) > sizeof(cb->ctx));

	memset(ctx, 0, sizeof(*ctx));

	ret = ethnl_tunnel_info_req_parse(&ctx->req_info, cb->nlh,
					  sock_net(cb->skb->sk), cb->extack,
					  false);
	if (ctx->req_info.dev) {
		dev_put(ctx->req_info.dev);
		ctx->req_info.dev = NULL;
	}

	return ret;
}

int ethnl_tunnel_info_dumpit(struct sk_buff *skb, struct netlink_callback *cb)
{
	struct ethnl_tunnel_info_dump_ctx *ctx = (void *)cb->ctx;
	struct net *net = sock_net(skb->sk);
	int s_idx = ctx->pos_idx;
	int h, idx = 0;
	int ret = 0;
	void *ehdr;

	rtnl_lock();
	cb->seq = net->dev_base_seq;
	for (h = ctx->pos_hash; h < NETDEV_HASHENTRIES; h++, s_idx = 0) {
		struct hlist_head *head;
		struct net_device *dev;

		head = &net->dev_index_head[h];
		idx = 0;
		hlist_for_each_entry(dev, head, index_hlist) {
			if (idx < s_idx)
				goto cont;

			ehdr = ethnl_dump_put(skb, cb,
					      ETHTOOL_MSG_TUNNEL_INFO_GET);
			if (!ehdr) {
				ret = -EMSGSIZE;
				goto out;
			}

			ret = ethnl_fill_reply_header(skb, dev, ETHTOOL_A_TUNNEL_INFO_HEADER);
			if (ret < 0) {
				genlmsg_cancel(skb, ehdr);
				goto out;
			}

			ctx->req_info.dev = dev;
			ret = ethnl_tunnel_info_fill_reply(&ctx->req_info, skb);
			ctx->req_info.dev = NULL;
			if (ret < 0) {
				genlmsg_cancel(skb, ehdr);
				if (ret == -EOPNOTSUPP)
					goto cont;
				goto out;
			}
			genlmsg_end(skb, ehdr);
cont:
			idx++;
		}
	}
out:
	rtnl_unlock();

	ctx->pos_hash = h;
	ctx->pos_idx = idx;
	nl_dump_check_consistent(cb, nlmsg_hdr(skb));

	if (ret == -EMSGSIZE && skb->len)
		return skb->len;
	return ret;
}
