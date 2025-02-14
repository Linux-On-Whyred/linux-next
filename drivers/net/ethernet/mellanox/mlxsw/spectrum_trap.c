// SPDX-License-Identifier: BSD-3-Clause OR GPL-2.0
/* Copyright (c) 2019 Mellanox Technologies. All rights reserved */

#include <linux/bitops.h>
#include <linux/kernel.h>
#include <linux/netlink.h>
#include <net/devlink.h>
#include <uapi/linux/devlink.h>

#include "core.h"
#include "reg.h"
#include "spectrum.h"
#include "spectrum_trap.h"

struct mlxsw_sp_trap_policer_item {
	struct devlink_trap_policer policer;
	u16 hw_id;
};

struct mlxsw_sp_trap_group_item {
	struct devlink_trap_group group;
	u16 hw_group_id;
	u8 priority;
};

#define MLXSW_SP_TRAP_LISTENERS_MAX 3

struct mlxsw_sp_trap_item {
	struct devlink_trap trap;
	struct mlxsw_listener listeners_arr[MLXSW_SP_TRAP_LISTENERS_MAX];
};

/* All driver-specific traps must be documented in
 * Documentation/networking/devlink/mlxsw.rst
 */
enum {
	DEVLINK_MLXSW_TRAP_ID_BASE = DEVLINK_TRAP_GENERIC_ID_MAX,
	DEVLINK_MLXSW_TRAP_ID_IRIF_DISABLED,
	DEVLINK_MLXSW_TRAP_ID_ERIF_DISABLED,
};

#define DEVLINK_MLXSW_TRAP_NAME_IRIF_DISABLED \
	"irif_disabled"
#define DEVLINK_MLXSW_TRAP_NAME_ERIF_DISABLED \
	"erif_disabled"

#define MLXSW_SP_TRAP_METADATA DEVLINK_TRAP_METADATA_TYPE_F_IN_PORT

static int mlxsw_sp_rx_listener(struct mlxsw_sp *mlxsw_sp, struct sk_buff *skb,
				u8 local_port,
				struct mlxsw_sp_port *mlxsw_sp_port)
{
	struct mlxsw_sp_port_pcpu_stats *pcpu_stats;

	if (unlikely(!mlxsw_sp_port)) {
		dev_warn_ratelimited(mlxsw_sp->bus_info->dev, "Port %d: skb received for non-existent port\n",
				     local_port);
		kfree_skb(skb);
		return -EINVAL;
	}

	skb->dev = mlxsw_sp_port->dev;

	pcpu_stats = this_cpu_ptr(mlxsw_sp_port->pcpu_stats);
	u64_stats_update_begin(&pcpu_stats->syncp);
	pcpu_stats->rx_packets++;
	pcpu_stats->rx_bytes += skb->len;
	u64_stats_update_end(&pcpu_stats->syncp);

	skb->protocol = eth_type_trans(skb, skb->dev);

	return 0;
}

static void mlxsw_sp_rx_drop_listener(struct sk_buff *skb, u8 local_port,
				      void *trap_ctx)
{
	struct devlink_port *in_devlink_port;
	struct mlxsw_sp_port *mlxsw_sp_port;
	struct mlxsw_sp *mlxsw_sp;
	struct devlink *devlink;
	int err;

	mlxsw_sp = devlink_trap_ctx_priv(trap_ctx);
	mlxsw_sp_port = mlxsw_sp->ports[local_port];

	err = mlxsw_sp_rx_listener(mlxsw_sp, skb, local_port, mlxsw_sp_port);
	if (err)
		return;

	devlink = priv_to_devlink(mlxsw_sp->core);
	in_devlink_port = mlxsw_core_port_devlink_port_get(mlxsw_sp->core,
							   local_port);
	skb_push(skb, ETH_HLEN);
	devlink_trap_report(devlink, skb, trap_ctx, in_devlink_port, NULL);
	consume_skb(skb);
}

static void mlxsw_sp_rx_acl_drop_listener(struct sk_buff *skb, u8 local_port,
					  void *trap_ctx)
{
	u32 cookie_index = mlxsw_skb_cb(skb)->cookie_index;
	const struct flow_action_cookie *fa_cookie;
	struct devlink_port *in_devlink_port;
	struct mlxsw_sp_port *mlxsw_sp_port;
	struct mlxsw_sp *mlxsw_sp;
	struct devlink *devlink;
	int err;

	mlxsw_sp = devlink_trap_ctx_priv(trap_ctx);
	mlxsw_sp_port = mlxsw_sp->ports[local_port];

	err = mlxsw_sp_rx_listener(mlxsw_sp, skb, local_port, mlxsw_sp_port);
	if (err)
		return;

	devlink = priv_to_devlink(mlxsw_sp->core);
	in_devlink_port = mlxsw_core_port_devlink_port_get(mlxsw_sp->core,
							   local_port);
	skb_push(skb, ETH_HLEN);
	rcu_read_lock();
	fa_cookie = mlxsw_sp_acl_act_cookie_lookup(mlxsw_sp, cookie_index);
	devlink_trap_report(devlink, skb, trap_ctx, in_devlink_port, fa_cookie);
	rcu_read_unlock();
	consume_skb(skb);
}

static void mlxsw_sp_rx_exception_listener(struct sk_buff *skb, u8 local_port,
					   void *trap_ctx)
{
	struct devlink_port *in_devlink_port;
	struct mlxsw_sp_port *mlxsw_sp_port;
	struct mlxsw_sp *mlxsw_sp;
	struct devlink *devlink;
	int err;

	mlxsw_sp = devlink_trap_ctx_priv(trap_ctx);
	mlxsw_sp_port = mlxsw_sp->ports[local_port];

	err = mlxsw_sp_rx_listener(mlxsw_sp, skb, local_port, mlxsw_sp_port);
	if (err)
		return;

	devlink = priv_to_devlink(mlxsw_sp->core);
	in_devlink_port = mlxsw_core_port_devlink_port_get(mlxsw_sp->core,
							   local_port);
	skb_push(skb, ETH_HLEN);
	devlink_trap_report(devlink, skb, trap_ctx, in_devlink_port, NULL);
	skb_pull(skb, ETH_HLEN);
	skb->offload_fwd_mark = 1;
	netif_receive_skb(skb);
}

#define MLXSW_SP_TRAP_DROP(_id, _group_id)				      \
	DEVLINK_TRAP_GENERIC(DROP, DROP, _id,				      \
			     DEVLINK_TRAP_GROUP_GENERIC_ID_##_group_id,	      \
			     MLXSW_SP_TRAP_METADATA)

#define MLXSW_SP_TRAP_DROP_EXT(_id, _group_id, _metadata)		      \
	DEVLINK_TRAP_GENERIC(DROP, DROP, _id,				      \
			     DEVLINK_TRAP_GROUP_GENERIC_ID_##_group_id,	      \
			     MLXSW_SP_TRAP_METADATA | (_metadata))

#define MLXSW_SP_TRAP_DRIVER_DROP(_id, _group_id)			      \
	DEVLINK_TRAP_DRIVER(DROP, DROP, DEVLINK_MLXSW_TRAP_ID_##_id,	      \
			    DEVLINK_MLXSW_TRAP_NAME_##_id,		      \
			    DEVLINK_TRAP_GROUP_GENERIC_ID_##_group_id,	      \
			    MLXSW_SP_TRAP_METADATA)

#define MLXSW_SP_TRAP_EXCEPTION(_id, _group_id)		      \
	DEVLINK_TRAP_GENERIC(EXCEPTION, TRAP, _id,			      \
			     DEVLINK_TRAP_GROUP_GENERIC_ID_##_group_id,	      \
			     MLXSW_SP_TRAP_METADATA)

#define MLXSW_SP_RXL_DISCARD(_id, _group_id)				      \
	MLXSW_RXL_DIS(mlxsw_sp_rx_drop_listener, DISCARD_##_id,		      \
		      TRAP_EXCEPTION_TO_CPU, false, SP_##_group_id,	      \
		      SET_FW_DEFAULT, SP_##_group_id)

#define MLXSW_SP_RXL_ACL_DISCARD(_id, _en_group_id, _dis_group_id)	      \
	MLXSW_RXL_DIS(mlxsw_sp_rx_acl_drop_listener, DISCARD_##_id,	      \
		      TRAP_EXCEPTION_TO_CPU, false, SP_##_en_group_id,	      \
		      SET_FW_DEFAULT, SP_##_dis_group_id)

#define MLXSW_SP_RXL_EXCEPTION(_id, _group_id, _action)			      \
	MLXSW_RXL(mlxsw_sp_rx_exception_listener, _id,			      \
		   _action, false, SP_##_group_id, SET_FW_DEFAULT)

#define MLXSW_SP_TRAP_POLICER(_id, _rate, _burst)			      \
	DEVLINK_TRAP_POLICER(_id, _rate, _burst,			      \
			     MLXSW_REG_QPCR_HIGHEST_CIR,		      \
			     MLXSW_REG_QPCR_LOWEST_CIR,			      \
			     1 << MLXSW_REG_QPCR_HIGHEST_CBS,		      \
			     1 << MLXSW_REG_QPCR_LOWEST_CBS)

/* Ordered by policer identifier */
static const struct mlxsw_sp_trap_policer_item
mlxsw_sp_trap_policer_items_arr[] = {
	{
		.policer = MLXSW_SP_TRAP_POLICER(1, 10 * 1024, 128),
	},
};

static const struct mlxsw_sp_trap_group_item mlxsw_sp_trap_group_items_arr[] = {
	{
		.group = DEVLINK_TRAP_GROUP_GENERIC(L2_DROPS, 1),
		.hw_group_id = MLXSW_REG_HTGT_TRAP_GROUP_SP_L2_DISCARDS,
		.priority = 0,
	},
	{
		.group = DEVLINK_TRAP_GROUP_GENERIC(L3_DROPS, 1),
		.hw_group_id = MLXSW_REG_HTGT_TRAP_GROUP_SP_L3_DISCARDS,
		.priority = 0,
	},
	{
		.group = DEVLINK_TRAP_GROUP_GENERIC(TUNNEL_DROPS, 1),
		.hw_group_id = MLXSW_REG_HTGT_TRAP_GROUP_SP_TUNNEL_DISCARDS,
		.priority = 0,
	},
	{
		.group = DEVLINK_TRAP_GROUP_GENERIC(ACL_DROPS, 1),
		.hw_group_id = MLXSW_REG_HTGT_TRAP_GROUP_SP_ACL_DISCARDS,
		.priority = 0,
	},
};

static const struct mlxsw_sp_trap_item mlxsw_sp_trap_items_arr[] = {
	{
		.trap = MLXSW_SP_TRAP_DROP(SMAC_MC, L2_DROPS),
		.listeners_arr = {
			MLXSW_SP_RXL_DISCARD(ING_PACKET_SMAC_MC, L2_DISCARDS),
		},
	},
	{
		.trap = MLXSW_SP_TRAP_DROP(VLAN_TAG_MISMATCH, L2_DROPS),
		.listeners_arr = {
			MLXSW_SP_RXL_DISCARD(ING_SWITCH_VTAG_ALLOW,
					     L2_DISCARDS),
		},
	},
	{
		.trap = MLXSW_SP_TRAP_DROP(INGRESS_VLAN_FILTER, L2_DROPS),
		.listeners_arr = {
			MLXSW_SP_RXL_DISCARD(ING_SWITCH_VLAN, L2_DISCARDS),
		},
	},
	{
		.trap = MLXSW_SP_TRAP_DROP(INGRESS_STP_FILTER, L2_DROPS),
		.listeners_arr = {
			MLXSW_SP_RXL_DISCARD(ING_SWITCH_STP, L2_DISCARDS),
		},
	},
	{
		.trap = MLXSW_SP_TRAP_DROP(EMPTY_TX_LIST, L2_DROPS),
		.listeners_arr = {
			MLXSW_SP_RXL_DISCARD(LOOKUP_SWITCH_UC, L2_DISCARDS),
			MLXSW_SP_RXL_DISCARD(LOOKUP_SWITCH_MC_NULL, L2_DISCARDS),
		},
	},
	{
		.trap = MLXSW_SP_TRAP_DROP(PORT_LOOPBACK_FILTER, L2_DROPS),
		.listeners_arr = {
			MLXSW_SP_RXL_DISCARD(LOOKUP_SWITCH_LB, L2_DISCARDS),
		},
	},
	{
		.trap = MLXSW_SP_TRAP_DROP(BLACKHOLE_ROUTE, L3_DROPS),
		.listeners_arr = {
			MLXSW_SP_RXL_DISCARD(ROUTER2, L3_DISCARDS),
		},
	},
	{
		.trap = MLXSW_SP_TRAP_DROP(NON_IP_PACKET, L3_DROPS),
		.listeners_arr = {
			MLXSW_SP_RXL_DISCARD(ING_ROUTER_NON_IP_PACKET,
					     L3_DISCARDS),
		},
	},
	{
		.trap = MLXSW_SP_TRAP_DROP(UC_DIP_MC_DMAC, L3_DROPS),
		.listeners_arr = {
			MLXSW_SP_RXL_DISCARD(ING_ROUTER_UC_DIP_MC_DMAC,
					     L3_DISCARDS),
		},
	},
	{
		.trap = MLXSW_SP_TRAP_DROP(DIP_LB, L3_DROPS),
		.listeners_arr = {
			MLXSW_SP_RXL_DISCARD(ING_ROUTER_DIP_LB, L3_DISCARDS),
		},
	},
	{
		.trap = MLXSW_SP_TRAP_DROP(SIP_MC, L3_DROPS),
		.listeners_arr = {
			MLXSW_SP_RXL_DISCARD(ING_ROUTER_SIP_MC, L3_DISCARDS),
		},
	},
	{
		.trap = MLXSW_SP_TRAP_DROP(SIP_LB, L3_DROPS),
		.listeners_arr = {
			MLXSW_SP_RXL_DISCARD(ING_ROUTER_SIP_LB, L3_DISCARDS),
		},
	},
	{
		.trap = MLXSW_SP_TRAP_DROP(CORRUPTED_IP_HDR, L3_DROPS),
		.listeners_arr = {
			MLXSW_SP_RXL_DISCARD(ING_ROUTER_CORRUPTED_IP_HDR,
					     L3_DISCARDS),
		},
	},
	{
		.trap = MLXSW_SP_TRAP_DROP(IPV4_SIP_BC, L3_DROPS),
		.listeners_arr = {
			MLXSW_SP_RXL_DISCARD(ING_ROUTER_IPV4_SIP_BC,
					     L3_DISCARDS),
		},
	},
	{
		.trap = MLXSW_SP_TRAP_DROP(IPV6_MC_DIP_RESERVED_SCOPE,
					   L3_DROPS),
		.listeners_arr = {
			MLXSW_SP_RXL_DISCARD(IPV6_MC_DIP_RESERVED_SCOPE,
					     L3_DISCARDS),
		},
	},
	{
		.trap = MLXSW_SP_TRAP_DROP(IPV6_MC_DIP_INTERFACE_LOCAL_SCOPE,
					   L3_DROPS),
		.listeners_arr = {
			MLXSW_SP_RXL_DISCARD(IPV6_MC_DIP_INTERFACE_LOCAL_SCOPE,
					     L3_DISCARDS),
		},
	},
	{
		.trap = MLXSW_SP_TRAP_EXCEPTION(MTU_ERROR, L3_DROPS),
		.listeners_arr = {
			MLXSW_SP_RXL_EXCEPTION(MTUERROR, L3_DISCARDS,
					       TRAP_TO_CPU),
		},
	},
	{
		.trap = MLXSW_SP_TRAP_EXCEPTION(TTL_ERROR, L3_DROPS),
		.listeners_arr = {
			MLXSW_SP_RXL_EXCEPTION(TTLERROR, L3_DISCARDS,
					       TRAP_TO_CPU),
		},
	},
	{
		.trap = MLXSW_SP_TRAP_EXCEPTION(RPF, L3_DROPS),
		.listeners_arr = {
			MLXSW_SP_RXL_EXCEPTION(RPF, L3_DISCARDS, TRAP_TO_CPU),
		},
	},
	{
		.trap = MLXSW_SP_TRAP_EXCEPTION(REJECT_ROUTE, L3_DROPS),
		.listeners_arr = {
			MLXSW_SP_RXL_EXCEPTION(RTR_INGRESS1, L3_DISCARDS,
					       TRAP_TO_CPU),
		},
	},
	{
		.trap = MLXSW_SP_TRAP_EXCEPTION(UNRESOLVED_NEIGH, L3_DROPS),
		.listeners_arr = {
			MLXSW_SP_RXL_EXCEPTION(HOST_MISS_IPV4, L3_DISCARDS,
					       TRAP_TO_CPU),
			MLXSW_SP_RXL_EXCEPTION(HOST_MISS_IPV6, L3_DISCARDS,
					       TRAP_TO_CPU),
			MLXSW_SP_RXL_EXCEPTION(DISCARD_ROUTER3, L3_DISCARDS,
					       TRAP_EXCEPTION_TO_CPU),
		},
	},
	{
		.trap = MLXSW_SP_TRAP_EXCEPTION(IPV4_LPM_UNICAST_MISS,
						L3_DROPS),
		.listeners_arr = {
			MLXSW_SP_RXL_EXCEPTION(DISCARD_ROUTER_LPM4, L3_DISCARDS,
					       TRAP_EXCEPTION_TO_CPU),
		},
	},
	{
		.trap = MLXSW_SP_TRAP_EXCEPTION(IPV6_LPM_UNICAST_MISS,
						L3_DROPS),
		.listeners_arr = {
			MLXSW_SP_RXL_EXCEPTION(DISCARD_ROUTER_LPM6, L3_DISCARDS,
					       TRAP_EXCEPTION_TO_CPU),
		},
	},
	{
		.trap = MLXSW_SP_TRAP_DRIVER_DROP(IRIF_DISABLED, L3_DROPS),
		.listeners_arr = {
			MLXSW_SP_RXL_DISCARD(ROUTER_IRIF_EN, L3_DISCARDS),
		},
	},
	{
		.trap = MLXSW_SP_TRAP_DRIVER_DROP(ERIF_DISABLED, L3_DROPS),
		.listeners_arr = {
			MLXSW_SP_RXL_DISCARD(ROUTER_ERIF_EN, L3_DISCARDS),
		},
	},
	{
		.trap = MLXSW_SP_TRAP_DROP(NON_ROUTABLE, L3_DROPS),
		.listeners_arr = {
			MLXSW_SP_RXL_DISCARD(NON_ROUTABLE, L3_DISCARDS),
		},
	},
	{
		.trap = MLXSW_SP_TRAP_EXCEPTION(DECAP_ERROR, TUNNEL_DROPS),
		.listeners_arr = {
			MLXSW_SP_RXL_EXCEPTION(DECAP_ECN0, TUNNEL_DISCARDS,
					       TRAP_EXCEPTION_TO_CPU),
			MLXSW_SP_RXL_EXCEPTION(IPIP_DECAP_ERROR,
					       TUNNEL_DISCARDS,
					       TRAP_EXCEPTION_TO_CPU),
			MLXSW_SP_RXL_EXCEPTION(DISCARD_DEC_PKT, TUNNEL_DISCARDS,
					       TRAP_EXCEPTION_TO_CPU),
		},
	},
	{
		.trap = MLXSW_SP_TRAP_DROP(OVERLAY_SMAC_MC, TUNNEL_DROPS),
		.listeners_arr = {
			MLXSW_SP_RXL_DISCARD(OVERLAY_SMAC_MC, TUNNEL_DISCARDS),
		},
	},
	{
		.trap = MLXSW_SP_TRAP_DROP_EXT(INGRESS_FLOW_ACTION_DROP,
					       ACL_DROPS,
					       DEVLINK_TRAP_METADATA_TYPE_F_FA_COOKIE),
		.listeners_arr = {
			MLXSW_SP_RXL_ACL_DISCARD(INGRESS_ACL, ACL_DISCARDS,
						 DUMMY),
		},
	},
	{
		.trap = MLXSW_SP_TRAP_DROP_EXT(EGRESS_FLOW_ACTION_DROP,
					       ACL_DROPS,
					       DEVLINK_TRAP_METADATA_TYPE_F_FA_COOKIE),
		.listeners_arr = {
			MLXSW_SP_RXL_ACL_DISCARD(EGRESS_ACL, ACL_DISCARDS,
						 DUMMY),
		},
	},
};

static struct mlxsw_sp_trap_policer_item *
mlxsw_sp_trap_policer_item_lookup(struct mlxsw_sp *mlxsw_sp, u32 id)
{
	struct mlxsw_sp_trap *trap = mlxsw_sp->trap;
	int i;

	for (i = 0; i < trap->policers_count; i++) {
		if (trap->policer_items_arr[i].policer.id == id)
			return &trap->policer_items_arr[i];
	}

	return NULL;
}

static struct mlxsw_sp_trap_group_item *
mlxsw_sp_trap_group_item_lookup(struct mlxsw_sp *mlxsw_sp, u16 id)
{
	struct mlxsw_sp_trap *trap = mlxsw_sp->trap;
	int i;

	for (i = 0; i < trap->groups_count; i++) {
		if (trap->group_items_arr[i].group.id == id)
			return &trap->group_items_arr[i];
	}

	return NULL;
}

static struct mlxsw_sp_trap_item *
mlxsw_sp_trap_item_lookup(struct mlxsw_sp *mlxsw_sp, u16 id)
{
	struct mlxsw_sp_trap *trap = mlxsw_sp->trap;
	int i;

	for (i = 0; i < trap->traps_count; i++) {
		if (trap->trap_items_arr[i].trap.id == id)
			return &trap->trap_items_arr[i];
	}

	return NULL;
}

static int mlxsw_sp_trap_cpu_policers_set(struct mlxsw_sp *mlxsw_sp)
{
	struct mlxsw_sp_trap *trap = mlxsw_sp->trap;
	char qpcr_pl[MLXSW_REG_QPCR_LEN];
	u16 hw_id;

	/* The purpose of "thin" policer is to drop as many packets
	 * as possible. The dummy group is using it.
	 */
	hw_id = find_first_zero_bit(trap->policers_usage, trap->max_policers);
	if (WARN_ON(hw_id == trap->max_policers))
		return -ENOBUFS;

	__set_bit(hw_id, trap->policers_usage);
	trap->thin_policer_hw_id = hw_id;
	mlxsw_reg_qpcr_pack(qpcr_pl, hw_id, MLXSW_REG_QPCR_IR_UNITS_M,
			    false, 1, 4);
	return mlxsw_reg_write(mlxsw_sp->core, MLXSW_REG(qpcr), qpcr_pl);
}

static int mlxsw_sp_trap_dummy_group_init(struct mlxsw_sp *mlxsw_sp)
{
	char htgt_pl[MLXSW_REG_HTGT_LEN];

	mlxsw_reg_htgt_pack(htgt_pl, MLXSW_REG_HTGT_TRAP_GROUP_SP_DUMMY,
			    mlxsw_sp->trap->thin_policer_hw_id, 0, 1);
	return mlxsw_reg_write(mlxsw_sp->core, MLXSW_REG(htgt), htgt_pl);
}

static int mlxsw_sp_trap_policer_items_arr_init(struct mlxsw_sp *mlxsw_sp)
{
	size_t elem_size = sizeof(struct mlxsw_sp_trap_policer_item);
	u64 arr_size = ARRAY_SIZE(mlxsw_sp_trap_policer_items_arr);
	struct mlxsw_sp_trap *trap = mlxsw_sp->trap;
	u64 free_policers = 0;
	u32 last_id;
	int i;

	for_each_clear_bit(i, trap->policers_usage, trap->max_policers)
		free_policers++;

	if (arr_size > free_policers) {
		dev_err(mlxsw_sp->bus_info->dev, "Exceeded number of supported packet trap policers\n");
		return -ENOBUFS;
	}

	trap->policer_items_arr = kcalloc(free_policers, elem_size, GFP_KERNEL);
	if (!trap->policer_items_arr)
		return -ENOMEM;

	trap->policers_count = free_policers;

	/* Initialize policer items array with pre-defined policers. */
	memcpy(trap->policer_items_arr, mlxsw_sp_trap_policer_items_arr,
	       elem_size * arr_size);

	/* Initialize policer items array with the rest of the available
	 * policers.
	 */
	last_id = mlxsw_sp_trap_policer_items_arr[arr_size - 1].policer.id;
	for (i = arr_size; i < trap->policers_count; i++) {
		const struct mlxsw_sp_trap_policer_item *policer_item;

		/* Use parameters set for first policer and override
		 * relevant ones.
		 */
		policer_item = &mlxsw_sp_trap_policer_items_arr[0];
		trap->policer_items_arr[i] = *policer_item;
		trap->policer_items_arr[i].policer.id = ++last_id;
		trap->policer_items_arr[i].policer.init_rate = 1;
		trap->policer_items_arr[i].policer.init_burst = 16;
	}

	return 0;
}

static void mlxsw_sp_trap_policer_items_arr_fini(struct mlxsw_sp *mlxsw_sp)
{
	kfree(mlxsw_sp->trap->policer_items_arr);
}

static int mlxsw_sp_trap_policers_init(struct mlxsw_sp *mlxsw_sp)
{
	struct devlink *devlink = priv_to_devlink(mlxsw_sp->core);
	const struct mlxsw_sp_trap_policer_item *policer_item;
	struct mlxsw_sp_trap *trap = mlxsw_sp->trap;
	int err, i;

	err = mlxsw_sp_trap_policer_items_arr_init(mlxsw_sp);
	if (err)
		return err;

	for (i = 0; i < trap->policers_count; i++) {
		policer_item = &trap->policer_items_arr[i];
		err = devlink_trap_policers_register(devlink,
						     &policer_item->policer, 1);
		if (err)
			goto err_trap_policer_register;
	}

	return 0;

err_trap_policer_register:
	for (i--; i >= 0; i--) {
		policer_item = &trap->policer_items_arr[i];
		devlink_trap_policers_unregister(devlink,
						 &policer_item->policer, 1);
	}
	mlxsw_sp_trap_policer_items_arr_fini(mlxsw_sp);
	return err;
}

static void mlxsw_sp_trap_policers_fini(struct mlxsw_sp *mlxsw_sp)
{
	struct devlink *devlink = priv_to_devlink(mlxsw_sp->core);
	const struct mlxsw_sp_trap_policer_item *policer_item;
	struct mlxsw_sp_trap *trap = mlxsw_sp->trap;
	int i;

	for (i = trap->policers_count - 1; i >= 0; i--) {
		policer_item = &trap->policer_items_arr[i];
		devlink_trap_policers_unregister(devlink,
						 &policer_item->policer, 1);
	}
	mlxsw_sp_trap_policer_items_arr_fini(mlxsw_sp);
}

static int mlxsw_sp_trap_groups_init(struct mlxsw_sp *mlxsw_sp)
{
	struct devlink *devlink = priv_to_devlink(mlxsw_sp->core);
	const struct mlxsw_sp_trap_group_item *group_item;
	struct mlxsw_sp_trap *trap = mlxsw_sp->trap;
	int err, i;

	trap->group_items_arr = kmemdup(mlxsw_sp_trap_group_items_arr,
					sizeof(mlxsw_sp_trap_group_items_arr),
					GFP_KERNEL);
	if (!trap->group_items_arr)
		return -ENOMEM;

	trap->groups_count = ARRAY_SIZE(mlxsw_sp_trap_group_items_arr);

	for (i = 0; i < trap->groups_count; i++) {
		group_item = &trap->group_items_arr[i];
		err = devlink_trap_groups_register(devlink, &group_item->group,
						   1);
		if (err)
			goto err_trap_group_register;
	}

	return 0;

err_trap_group_register:
	for (i--; i >= 0; i--) {
		group_item = &trap->group_items_arr[i];
		devlink_trap_groups_unregister(devlink, &group_item->group, 1);
	}
	kfree(trap->group_items_arr);
	return err;
}

static void mlxsw_sp_trap_groups_fini(struct mlxsw_sp *mlxsw_sp)
{
	struct devlink *devlink = priv_to_devlink(mlxsw_sp->core);
	struct mlxsw_sp_trap *trap = mlxsw_sp->trap;
	int i;

	for (i = trap->groups_count - 1; i >= 0; i--) {
		const struct mlxsw_sp_trap_group_item *group_item;

		group_item = &trap->group_items_arr[i];
		devlink_trap_groups_unregister(devlink, &group_item->group, 1);
	}
	kfree(trap->group_items_arr);
}

static bool
mlxsw_sp_trap_listener_is_valid(const struct mlxsw_listener *listener)
{
	return listener->trap_id != 0;
}

static int mlxsw_sp_traps_init(struct mlxsw_sp *mlxsw_sp)
{
	struct devlink *devlink = priv_to_devlink(mlxsw_sp->core);
	struct mlxsw_sp_trap *trap = mlxsw_sp->trap;
	const struct mlxsw_sp_trap_item *trap_item;
	int err, i;

	trap->trap_items_arr = kmemdup(mlxsw_sp_trap_items_arr,
				       sizeof(mlxsw_sp_trap_items_arr),
				       GFP_KERNEL);
	if (!trap->trap_items_arr)
		return -ENOMEM;

	trap->traps_count = ARRAY_SIZE(mlxsw_sp_trap_items_arr);

	for (i = 0; i < trap->traps_count; i++) {
		trap_item = &trap->trap_items_arr[i];
		err = devlink_traps_register(devlink, &trap_item->trap, 1,
					     mlxsw_sp);
		if (err)
			goto err_trap_register;
	}

	return 0;

err_trap_register:
	for (i--; i >= 0; i--) {
		trap_item = &trap->trap_items_arr[i];
		devlink_traps_unregister(devlink, &trap_item->trap, 1);
	}
	kfree(trap->trap_items_arr);
	return err;
}

static void mlxsw_sp_traps_fini(struct mlxsw_sp *mlxsw_sp)
{
	struct devlink *devlink = priv_to_devlink(mlxsw_sp->core);
	struct mlxsw_sp_trap *trap = mlxsw_sp->trap;
	int i;

	for (i = trap->traps_count - 1; i >= 0; i--) {
		const struct mlxsw_sp_trap_item *trap_item;

		trap_item = &trap->trap_items_arr[i];
		devlink_traps_unregister(devlink, &trap_item->trap, 1);
	}
	kfree(trap->trap_items_arr);
}

int mlxsw_sp_devlink_traps_init(struct mlxsw_sp *mlxsw_sp)
{
	int err;

	err = mlxsw_sp_trap_cpu_policers_set(mlxsw_sp);
	if (err)
		return err;

	err = mlxsw_sp_trap_dummy_group_init(mlxsw_sp);
	if (err)
		return err;

	err = mlxsw_sp_trap_policers_init(mlxsw_sp);
	if (err)
		return err;

	err = mlxsw_sp_trap_groups_init(mlxsw_sp);
	if (err)
		goto err_trap_groups_init;

	err = mlxsw_sp_traps_init(mlxsw_sp);
	if (err)
		goto err_traps_init;

	return 0;

err_traps_init:
	mlxsw_sp_trap_groups_fini(mlxsw_sp);
err_trap_groups_init:
	mlxsw_sp_trap_policers_fini(mlxsw_sp);
	return err;
}

void mlxsw_sp_devlink_traps_fini(struct mlxsw_sp *mlxsw_sp)
{
	mlxsw_sp_traps_fini(mlxsw_sp);
	mlxsw_sp_trap_groups_fini(mlxsw_sp);
	mlxsw_sp_trap_policers_fini(mlxsw_sp);
}

int mlxsw_sp_trap_init(struct mlxsw_core *mlxsw_core,
		       const struct devlink_trap *trap, void *trap_ctx)
{
	struct mlxsw_sp *mlxsw_sp = mlxsw_core_driver_priv(mlxsw_core);
	const struct mlxsw_sp_trap_item *trap_item;
	int i;

	trap_item = mlxsw_sp_trap_item_lookup(mlxsw_sp, trap->id);
	if (WARN_ON(!trap_item))
		return -EINVAL;

	for (i = 0; i < MLXSW_SP_TRAP_LISTENERS_MAX; i++) {
		const struct mlxsw_listener *listener;
		int err;

		listener = &trap_item->listeners_arr[i];
		if (!mlxsw_sp_trap_listener_is_valid(listener))
			continue;
		err = mlxsw_core_trap_register(mlxsw_core, listener, trap_ctx);
		if (err)
			return err;
	}

	return 0;
}

void mlxsw_sp_trap_fini(struct mlxsw_core *mlxsw_core,
			const struct devlink_trap *trap, void *trap_ctx)
{
	struct mlxsw_sp *mlxsw_sp = mlxsw_core_driver_priv(mlxsw_core);
	const struct mlxsw_sp_trap_item *trap_item;
	int i;

	trap_item = mlxsw_sp_trap_item_lookup(mlxsw_sp, trap->id);
	if (WARN_ON(!trap_item))
		return;

	for (i = MLXSW_SP_TRAP_LISTENERS_MAX - 1; i >= 0; i--) {
		const struct mlxsw_listener *listener;

		listener = &trap_item->listeners_arr[i];
		if (!mlxsw_sp_trap_listener_is_valid(listener))
			continue;
		mlxsw_core_trap_unregister(mlxsw_core, listener, trap_ctx);
	}
}

int mlxsw_sp_trap_action_set(struct mlxsw_core *mlxsw_core,
			     const struct devlink_trap *trap,
			     enum devlink_trap_action action)
{
	struct mlxsw_sp *mlxsw_sp = mlxsw_core_driver_priv(mlxsw_core);
	const struct mlxsw_sp_trap_item *trap_item;
	int i;

	trap_item = mlxsw_sp_trap_item_lookup(mlxsw_sp, trap->id);
	if (WARN_ON(!trap_item))
		return -EINVAL;

	for (i = 0; i < MLXSW_SP_TRAP_LISTENERS_MAX; i++) {
		const struct mlxsw_listener *listener;
		bool enabled;
		int err;

		listener = &trap_item->listeners_arr[i];
		if (!mlxsw_sp_trap_listener_is_valid(listener))
			continue;

		switch (action) {
		case DEVLINK_TRAP_ACTION_DROP:
			enabled = false;
			break;
		case DEVLINK_TRAP_ACTION_TRAP:
			enabled = true;
			break;
		default:
			return -EINVAL;
		}
		err = mlxsw_core_trap_state_set(mlxsw_core, listener, enabled);
		if (err)
			return err;
	}

	return 0;
}

static int
__mlxsw_sp_trap_group_init(struct mlxsw_core *mlxsw_core,
			   const struct devlink_trap_group *group,
			   u32 policer_id)
{
	struct mlxsw_sp *mlxsw_sp = mlxsw_core_driver_priv(mlxsw_core);
	u16 hw_policer_id = MLXSW_REG_HTGT_INVALID_POLICER;
	const struct mlxsw_sp_trap_group_item *group_item;
	char htgt_pl[MLXSW_REG_HTGT_LEN];

	group_item = mlxsw_sp_trap_group_item_lookup(mlxsw_sp, group->id);
	if (WARN_ON(!group_item))
		return -EINVAL;

	if (policer_id) {
		struct mlxsw_sp_trap_policer_item *policer_item;

		policer_item = mlxsw_sp_trap_policer_item_lookup(mlxsw_sp,
								 policer_id);
		if (WARN_ON(!policer_item))
			return -EINVAL;
		hw_policer_id = policer_item->hw_id;
	}

	mlxsw_reg_htgt_pack(htgt_pl, group_item->hw_group_id, hw_policer_id,
			    group_item->priority, group_item->priority);
	return mlxsw_reg_write(mlxsw_core, MLXSW_REG(htgt), htgt_pl);
}

int mlxsw_sp_trap_group_init(struct mlxsw_core *mlxsw_core,
			     const struct devlink_trap_group *group)
{
	return __mlxsw_sp_trap_group_init(mlxsw_core, group,
					  group->init_policer_id);
}

int mlxsw_sp_trap_group_set(struct mlxsw_core *mlxsw_core,
			    const struct devlink_trap_group *group,
			    const struct devlink_trap_policer *policer)
{
	u32 policer_id = policer ? policer->id : 0;

	return __mlxsw_sp_trap_group_init(mlxsw_core, group, policer_id);
}

static int
mlxsw_sp_trap_policer_item_init(struct mlxsw_sp *mlxsw_sp,
				struct mlxsw_sp_trap_policer_item *policer_item)
{
	struct mlxsw_sp_trap *trap = mlxsw_sp->trap;
	u16 hw_id;

	/* We should be able to allocate a policer because the number of
	 * policers we registered with devlink is in according with the number
	 * of available policers.
	 */
	hw_id = find_first_zero_bit(trap->policers_usage, trap->max_policers);
	if (WARN_ON(hw_id == trap->max_policers))
		return -ENOBUFS;

	__set_bit(hw_id, trap->policers_usage);
	policer_item->hw_id = hw_id;

	return 0;
}

static void
mlxsw_sp_trap_policer_item_fini(struct mlxsw_sp *mlxsw_sp,
				struct mlxsw_sp_trap_policer_item *policer_item)
{
	__clear_bit(policer_item->hw_id, mlxsw_sp->trap->policers_usage);
}

static int mlxsw_sp_trap_policer_bs(u64 burst, u8 *p_burst_size,
				    struct netlink_ext_ack *extack)
{
	int bs = fls64(burst) - 1;

	if (burst != (BIT_ULL(bs))) {
		NL_SET_ERR_MSG_MOD(extack, "Policer burst size is not power of two");
		return -EINVAL;
	}

	*p_burst_size = bs;

	return 0;
}

static int __mlxsw_sp_trap_policer_set(struct mlxsw_sp *mlxsw_sp, u16 hw_id,
				       u64 rate, u64 burst, bool clear_counter,
				       struct netlink_ext_ack *extack)
{
	char qpcr_pl[MLXSW_REG_QPCR_LEN];
	u8 burst_size;
	int err;

	err = mlxsw_sp_trap_policer_bs(burst, &burst_size, extack);
	if (err)
		return err;

	mlxsw_reg_qpcr_pack(qpcr_pl, hw_id, MLXSW_REG_QPCR_IR_UNITS_M, false,
			    rate, burst_size);
	mlxsw_reg_qpcr_clear_counter_set(qpcr_pl, clear_counter);
	return mlxsw_reg_write(mlxsw_sp->core, MLXSW_REG(qpcr), qpcr_pl);
}

int mlxsw_sp_trap_policer_init(struct mlxsw_core *mlxsw_core,
			       const struct devlink_trap_policer *policer)
{
	struct mlxsw_sp *mlxsw_sp = mlxsw_core_driver_priv(mlxsw_core);
	struct mlxsw_sp_trap_policer_item *policer_item;
	int err;

	policer_item = mlxsw_sp_trap_policer_item_lookup(mlxsw_sp, policer->id);
	if (WARN_ON(!policer_item))
		return -EINVAL;

	err = mlxsw_sp_trap_policer_item_init(mlxsw_sp, policer_item);
	if (err)
		return err;

	err = __mlxsw_sp_trap_policer_set(mlxsw_sp, policer_item->hw_id,
					  policer->init_rate,
					  policer->init_burst, true, NULL);
	if (err)
		goto err_trap_policer_set;

	return 0;

err_trap_policer_set:
	mlxsw_sp_trap_policer_item_fini(mlxsw_sp, policer_item);
	return err;
}

void mlxsw_sp_trap_policer_fini(struct mlxsw_core *mlxsw_core,
				const struct devlink_trap_policer *policer)
{
	struct mlxsw_sp *mlxsw_sp = mlxsw_core_driver_priv(mlxsw_core);
	struct mlxsw_sp_trap_policer_item *policer_item;

	policer_item = mlxsw_sp_trap_policer_item_lookup(mlxsw_sp, policer->id);
	if (WARN_ON(!policer_item))
		return;

	mlxsw_sp_trap_policer_item_fini(mlxsw_sp, policer_item);
}

int mlxsw_sp_trap_policer_set(struct mlxsw_core *mlxsw_core,
			      const struct devlink_trap_policer *policer,
			      u64 rate, u64 burst,
			      struct netlink_ext_ack *extack)
{
	struct mlxsw_sp *mlxsw_sp = mlxsw_core_driver_priv(mlxsw_core);
	struct mlxsw_sp_trap_policer_item *policer_item;

	policer_item = mlxsw_sp_trap_policer_item_lookup(mlxsw_sp, policer->id);
	if (WARN_ON(!policer_item))
		return -EINVAL;

	return __mlxsw_sp_trap_policer_set(mlxsw_sp, policer_item->hw_id,
					   rate, burst, false, extack);
}

int
mlxsw_sp_trap_policer_counter_get(struct mlxsw_core *mlxsw_core,
				  const struct devlink_trap_policer *policer,
				  u64 *p_drops)
{
	struct mlxsw_sp *mlxsw_sp = mlxsw_core_driver_priv(mlxsw_core);
	struct mlxsw_sp_trap_policer_item *policer_item;
	char qpcr_pl[MLXSW_REG_QPCR_LEN];
	int err;

	policer_item = mlxsw_sp_trap_policer_item_lookup(mlxsw_sp, policer->id);
	if (WARN_ON(!policer_item))
		return -EINVAL;

	mlxsw_reg_qpcr_pack(qpcr_pl, policer_item->hw_id,
			    MLXSW_REG_QPCR_IR_UNITS_M, false, 0, 0);
	err = mlxsw_reg_query(mlxsw_sp->core, MLXSW_REG(qpcr), qpcr_pl);
	if (err)
		return err;

	*p_drops = mlxsw_reg_qpcr_violate_count_get(qpcr_pl);

	return 0;
}
