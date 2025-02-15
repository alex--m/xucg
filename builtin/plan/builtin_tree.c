/*
 * Copyright (C) Huawei Technologies Co., Ltd. 2019.  ALL RIGHTS RESERVED.
 * See file LICENSE for terms.
 */

#include <ucs/debug/log.h>
#include <ucs/debug/assert.h>
#include <ucs/debug/memtrack.h>
#include <uct/api/uct_def.h>

#include "builtin_plan.h"

#define MAX_PHASES (5)
#define ALLOC_SIZE(_ep_cnt) (sizeof(ucg_builtin_plan_t) + (MAX_PHASES * \
    (sizeof(ucg_builtin_plan_phase_t) + ((_ep_cnt) * sizeof(uct_ep_h)))))

ucs_config_field_t ucg_builtin_tree_config_table[] = {
    {"RADIX", "8", "Tree radix, for inter-node trees.\n",
     ucs_offsetof(ucg_builtin_tree_config_t, radix), UCS_CONFIG_TYPE_UINT},

    {"SOCKET_LEVEL_PPN_THRESH", "16",
     "Threshold for switching from 1-level to 2-level intra-node tree.\n",
     ucs_offsetof(ucg_builtin_tree_config_t, sock_thresh), UCS_CONFIG_TYPE_UINT},

    // TODO: add multi-root configuration

    {NULL}
};

static inline ucs_status_t ucg_builtin_tree_connect_phase(ucg_builtin_plan_phase_t *phase,
                                                          const ucg_builtin_tree_params_t *params,
                                                          ucg_step_idx_t step_index,
                                                          uct_ep_h **eps,
                                                          ucg_group_member_index_t *peers,
                                                          unsigned peer_cnt,
                                                          enum ucg_builtin_plan_method_type method,
                                                          enum ucg_plan_connect_flags coll_flags)
{
    unsigned idx;
    ucs_status_t status;

    int is_mock = params->coll_type->modifiers & UCG_GROUP_COLLECTIVE_MODIFIER_MOCK_EPS;

    ucs_assert(peer_cnt > 0);
flagless_retry:
    if ((peer_cnt == 1) || coll_flags) {
        status = ucg_builtin_single_connection_phase(params->ctx,
                peers[0], step_index, method, coll_flags, params->incast_cb,
                phase, is_mock);

#if ENABLE_DEBUG_DATA || ENABLE_FAULT_TOLERANCE
        if (status == UCS_OK) {
            phase->indexes = UCS_ALLOC_CHECK(sizeof(*peers), "tree root index");
            phase->indexes[0] = (coll_flags & UCG_PLAN_CONNECT_FLAG_AM_ROOT) ?
                    params->group_params->member_index : peers[0];
        }
#endif

        if ((status != UCS_ERR_UNREACHABLE) || (coll_flags == 0)) {
            return status;
        } else if (coll_flags) {
            /* retry - without the flags */
            coll_flags = 0;
            goto flagless_retry;
        }
    }

    phase->multi_eps     = *eps;
    phase->ep_cnt        = peer_cnt;
    phase->step_index    = step_index;
    phase->method        = method;
    *eps                += peer_cnt;

#if ENABLE_DEBUG_DATA || ENABLE_FAULT_TOLERANCE
    phase->indexes       = UCS_ALLOC_CHECK(peer_cnt * sizeof(*peers),
                                           "tree topology indexes");
#endif

    /* connect every endpoint, by group member index */
    for (idx = 0, status = UCS_OK; (idx < peer_cnt) && (status == UCS_OK); idx++, peers++) {
        status = ucg_builtin_connect(params->ctx, *peers, phase, idx, 0, NULL, is_mock);
    }
    return status;
}

ucs_status_t ucg_builtin_tree_connect(ucg_builtin_plan_t *tree,
                                      const ucg_builtin_tree_params_t *params,
                                      ucg_step_idx_t step_offset, unsigned ppn,
                                      uct_ep_h *first_ep,
                                      ucg_group_member_index_t *host_up,
                                      unsigned host_up_cnt,
                                      ucg_group_member_index_t *net_up,
                                      unsigned net_up_cnt,
                                      ucg_group_member_index_t *net_down,
                                      unsigned net_down_cnt,
                                      ucg_group_member_index_t *host_down,
                                      unsigned host_down_cnt)
{
    unsigned idx, flags;
    ucs_status_t status               = UCS_OK;
    enum ucg_collective_modifiers mod = params->coll_type->modifiers;
    uct_ep_h *iter_eps                = first_ep;
    ucg_builtin_plan_phase_t *phase   = tree->phss;
    ucg_step_idx_t *phs_cnt           = &tree->phs_cnt;
    phase                            += *phs_cnt;

    enum ucg_builtin_plan_method_type fanin_method, fanout_method;
    ucs_assert(host_up_cnt + host_down_cnt + net_up_cnt + net_down_cnt < UCG_BUILTIN_TREE_MAX_RADIX);
    ucs_assert(host_up_cnt + host_down_cnt + net_up_cnt + net_down_cnt > 0);
    // TODO: ucs_assert(up_cnt == (params->coll_type->root == params->me));

    enum ucg_builtin_plan_topology_type topo_type = params->plan_topo_type;
    switch (topo_type) {
    case UCG_PLAN_TREE_FANIN:
    case UCG_PLAN_TREE_FANIN_FANOUT:
        /* Create a phase for inter-node communication ("up the tree") */
        if (host_down_cnt) {
            if (mod & UCG_GROUP_COLLECTIVE_MODIFIER_AGGREGATE) {
                fanin_method = host_up_cnt ? UCG_PLAN_METHOD_REDUCE_WAYPOINT :
                                             UCG_PLAN_METHOD_REDUCE_TERMINAL;
            } else if (mod & UCG_GROUP_COLLECTIVE_MODIFIER_CONCATENATE) {
                fanin_method = host_up_cnt ? UCG_PLAN_METHOD_GATHER_WAYPOINT :
                                             UCG_PLAN_METHOD_GATHER_FOR_PAGG;
            } else {
                ucs_assert(host_up_cnt == 0);
                fanin_method = UCG_PLAN_METHOD_RECV_TERMINAL;
            }
            flags = UCG_PLAN_CONNECT_FLAG_AM_ROOT |
                    UCG_PLAN_CONNECT_FLAG_WANT_INCAST;
        } else {
            fanin_method = (ppn == 2) ? UCG_PLAN_METHOD_SEND_TERMINAL :
                                        UCG_PLAN_METHOD_SEND_TO_SM_ROOT;
            flags = UCG_PLAN_CONNECT_FLAG_WANT_INCAST;
        }

        if (host_up_cnt + host_down_cnt) {
            /* Add the host-level parent to the end of the host-array, so that
             * forwarding the message to the parent is all included in one step */
            if (host_up_cnt) host_down[host_down_cnt++] = host_up[0];
            status = ucg_builtin_tree_connect_phase(phase++, params, step_offset,
                    &iter_eps, host_down, host_down_cnt, fanin_method,
                    (ppn == 2) ? 0 : flags);
            (*phs_cnt)++;
            if (status != UCS_OK) {
                break;
            }
        }

        /* Create a phase for intra-node communication ("up the tree") */
        if (net_down_cnt) {
            if (mod & UCG_GROUP_COLLECTIVE_MODIFIER_AGGREGATE) {
                fanin_method = net_up_cnt ? UCG_PLAN_METHOD_REDUCE_WAYPOINT :
                                            UCG_PLAN_METHOD_REDUCE_TERMINAL;
            } else if (mod & UCG_GROUP_COLLECTIVE_MODIFIER_CONCATENATE) {
                fanin_method = net_up_cnt ? UCG_PLAN_METHOD_GATHER_WAYPOINT :
                        (mod & UCG_GROUP_COLLECTIVE_MODIFIER_SINGLE_DESTINATION) ?
                                UCG_PLAN_METHOD_GATHER_TERMINAL :
                                UCG_PLAN_METHOD_GATHER_A2A_ROOT;
            } else {
                ucs_assert(host_up_cnt == 0);
                fanin_method = UCG_PLAN_METHOD_RECV_TERMINAL;
            }
            flags = UCG_PLAN_CONNECT_FLAG_AM_ROOT |
                    UCG_PLAN_CONNECT_FLAG_WANT_INCAST;
        } else {
            fanin_method = UCG_PLAN_METHOD_SEND_TERMINAL;
            flags = UCG_PLAN_CONNECT_FLAG_WANT_INCAST;
        }

        if (net_up_cnt + net_down_cnt) {
            /* Add the network-level parent to the end of the host-array, so that
             * forwarding the message to the parent is all included in one step */
            if (net_up_cnt) net_down[net_down_cnt++] = net_up[0];
            status = ucg_builtin_tree_connect_phase(phase++, params, step_offset + 1,
                    &iter_eps, net_down, net_down_cnt, fanin_method, flags);
            (*phs_cnt)++;
        }

        if ((topo_type == UCG_PLAN_TREE_FANIN) || (status != UCS_OK)) {
            break;
        }

        /* If I have a host-level or network-level parent (it was added to
         * the end of the host-array) - now is the time to discard it by
         * decrementing the counter for each host-array */
        if (net_up_cnt) net_down_cnt--;
        if (host_up_cnt) host_down_cnt--;
        /* conditional break */
    case UCG_PLAN_TREE_FANOUT:
        /* Create a phase for inter-node communication ("up the tree") */
        if (net_down_cnt) {
            fanout_method = (mod & UCG_GROUP_COLLECTIVE_MODIFIER_BROADCAST) ?
                     (net_up_cnt ? UCG_PLAN_METHOD_BCAST_WAYPOINT :
                                   UCG_PLAN_METHOD_SEND_TERMINAL):
                     (net_up_cnt ? UCG_PLAN_METHOD_SCATTER_WAYPOINT:
                                   UCG_PLAN_METHOD_SCATTER_TERMINAL);
            flags = UCG_PLAN_CONNECT_FLAG_AM_ROOT |
                    UCG_PLAN_CONNECT_FLAG_WANT_BCAST;
        } else {
            fanout_method = UCG_PLAN_METHOD_RECV_TERMINAL;
            flags = UCG_PLAN_CONNECT_FLAG_WANT_BCAST;
        }

        if (net_up_cnt + net_down_cnt) {
            for (idx = 0; idx < net_down_cnt; idx++, net_up_cnt++) {
                if (net_up_cnt == UCG_BUILTIN_TREE_MAX_RADIX) {
                    return UCS_ERR_BUFFER_TOO_SMALL;
                }
                net_up[net_up_cnt] = net_down[idx];
            }
            status = ucg_builtin_tree_connect_phase(phase++, params, step_offset + 2,
                                                    &iter_eps, net_up, net_up_cnt,
                                                    fanout_method, flags);
            (*phs_cnt)++;
            if (status != UCS_OK) {
                break;
            }
        }

        /* Create a phase for intra-node communication ("down the tree") */
        if (host_down_cnt) {
            fanout_method = (mod & UCG_GROUP_COLLECTIVE_MODIFIER_BROADCAST) ?
                    (host_up_cnt ? UCG_PLAN_METHOD_BCAST_WAYPOINT :
                                   UCG_PLAN_METHOD_SEND_TERMINAL):
                    (host_up_cnt ? UCG_PLAN_METHOD_SCATTER_WAYPOINT:
                                   UCG_PLAN_METHOD_SCATTER_TERMINAL);
            flags = UCG_PLAN_CONNECT_FLAG_AM_ROOT |
                    UCG_PLAN_CONNECT_FLAG_WANT_BCAST;
        } else {
            fanout_method = UCG_PLAN_METHOD_RECV_TERMINAL;
            flags = UCG_PLAN_CONNECT_FLAG_WANT_BCAST;
        }

        if (host_up_cnt + host_down_cnt) {
            for (idx = 0; idx < host_down_cnt; idx++, host_up_cnt++) {
                if (host_up_cnt == UCG_BUILTIN_TREE_MAX_RADIX) {
                    return UCS_ERR_BUFFER_TOO_SMALL;
                }
                host_up[host_up_cnt] = host_down[idx];
            }
            status = ucg_builtin_tree_connect_phase(phase++, params, step_offset + 3,
                                                    &iter_eps, host_up, host_up_cnt,
                                                    fanout_method, (ppn == 2) ? 0 :
                                                                                flags);
            (*phs_cnt)++;
        }
        break;
    default:
        status = UCS_ERR_INVALID_PARAM;
        break;
    }

    if (status != UCS_OK) {
        free(tree);
        return status;
    }

    tree->ep_cnt = iter_eps - first_ep;

    return UCS_OK;
}

ucs_status_t ucg_builtin_tree_add_intra(const ucg_builtin_tree_params_t *params,
                                        unsigned *ppn,
                                        ucg_group_member_index_t my_idx,
                                        ucg_group_member_index_t *up,
                                        unsigned *final_up_cnt,
                                        ucg_group_member_index_t *down,
                                        unsigned *final_down_cnt,
                                        enum ucg_group_member_distance *master_phase)
{
    enum ucg_group_member_distance next_distance;
    unsigned up_cnt                               = 0;
    unsigned down_cnt                             = 0;
    ucg_group_member_index_t member_idx           = params->group_params->member_index;
    ucg_group_member_index_t socket_threshold     = params->config->sock_thresh;
    enum ucg_group_member_distance up_distance    = UCG_GROUP_MEMBER_DISTANCE_UNKNOWN;
    enum ucg_group_member_distance down_distance  = UCG_GROUP_MEMBER_DISTANCE_NONE;
    enum ucg_group_member_distance first_distance = UCG_GROUP_MEMBER_DISTANCE_NONE;

    /* Check how many processes are on each node */
    *final_down_cnt = ucg_group_count_ppx(params->group_params,
                                          UCG_GROUP_MEMBER_DISTANCE_HOST, ppn);

    /* If there's a small number of cores per socket - no use in 2-levels... */
    int is_single_level = (*ppn < socket_threshold);

    /* No information - use a flat tree (TODO: support multi-level trees too) */
    if (params->group_params->distance_type == UCG_GROUP_DISTANCE_TYPE_FIXED) {
        if (member_idx == 0) {
            *final_up_cnt = 0;
            ucg_builtin_prepare_rank_same_unit(params->group_params,
                                               UCG_GROUP_MEMBER_DISTANCE_UNKNOWN,
                                               down);
        } else {
            *final_down_cnt = 0;
            *final_up_cnt   = 1;
            *up             = 0;
        }
        return UCS_OK;
    }

    /* Go over potential parents, filling the per-phase member lists */
    for (member_idx = 0; member_idx < my_idx; member_idx++) {
        next_distance = params->group_params->distance_array[member_idx];

        /* If per-socket level is disabled - treat all local ranks the same */
        if (is_single_level && (next_distance == UCG_GROUP_MEMBER_DISTANCE_SOCKET)) {
            next_distance = UCG_GROUP_MEMBER_DISTANCE_HOST;
        }

        if (up_distance > next_distance) {
            /* Replace parent, possibly "demoting" myself */
            up_distance  = next_distance;
            *master_phase = next_distance - 1;
            up[0] = member_idx;
            up_cnt = 1;
            /**
             * Note: in the "multi-root" case, members #1,2,3... are likely
             * on the same host, and do not make for good tree roots. To
             * address this we change the root selection - inside the call
             * to @ref ucg_topo_fabric_calc .
             */
        }
    }

    /* Go over potential children, filling the per-phase member lists */
    for (member_idx++; member_idx < params->group_params->member_count; member_idx++) {
        next_distance = params->group_params->distance_array[member_idx];
        /* If per-socket level is disabled - treat all local ranks the same */
        if (is_single_level && (next_distance == UCG_GROUP_MEMBER_DISTANCE_SOCKET)) {
            next_distance = UCG_GROUP_MEMBER_DISTANCE_HOST;
        }

        /* Possibly add the next member to my list according to its distance */
        if ((next_distance > down_distance) &&
            (next_distance <= *master_phase) &&
            (next_distance < UCG_GROUP_MEMBER_DISTANCE_UNKNOWN)) {
            down_distance = next_distance;
            if (first_distance == UCG_GROUP_MEMBER_DISTANCE_NONE) {
                first_distance = down_distance;
            } else {
                first_distance = UCG_GROUP_MEMBER_DISTANCE_UNKNOWN;
            }
            down[down_cnt++] = down[0];
            down[0]          = member_idx;
            if (down_cnt == UCG_BUILTIN_TREE_MAX_RADIX) {
                goto limit_exceeded;
            }
        } else if (next_distance == first_distance) {
            down[down_cnt++] = member_idx;
            if (down_cnt == UCG_BUILTIN_TREE_MAX_RADIX) {
                goto limit_exceeded;
            }
        }
    }

    /* Make corrections for non-zero root */
    if (params->root != 0) {
        /* If the new root is my child - omit him */
        for (member_idx = 0; member_idx < down_cnt; member_idx++) {
            if (down[member_idx] == params->root) {
                down[member_idx] = down[--down_cnt];
                break;
            }
        }

        /* If I'm the new root - expect also a message from the "old" root (0) */
        if (my_idx == params->root) {
            up_cnt = 0;
            if (params->group_params->distance_array[0] < UCG_GROUP_MEMBER_DISTANCE_UNKNOWN) {
                down[down_cnt++] = 0;
            }
        }
    }

    *final_up_cnt = up_cnt;
    *final_down_cnt = down_cnt;
    return UCS_OK;

limit_exceeded:
    ucs_error("Internal PPN limit (%i) exceeded", UCG_BUILTIN_TREE_MAX_RADIX);
    return UCS_ERR_UNSUPPORTED;
}

static ucs_status_t ucg_builtin_tree_add_inter(const ucg_builtin_tree_params_t *params,
                                               ucg_group_member_index_t my_idx,
                                               ucg_group_member_index_t ppn,
                                               ucg_group_member_index_t *up,
                                               unsigned *up_cnt,
                                               ucg_group_member_index_t *down,
                                               unsigned *down_cnt)
{
    /* Calculate fabric peers up and down, assuming uniform distribution */
    ucg_group_member_index_t limit       = params->group_params->member_count;
    ucg_group_member_index_t radix       = params->config->radix;
    ucg_group_member_index_t inner_range = ppn;
    ucg_group_member_index_t outer_range = ppn * radix;
    ucg_group_member_index_t root, inner_idx, outer_idx;

    /* The OpenMPI default is to allocate "by node", so that each host gets
     * <PPN> consequtive rank numbers assigned to it. Alternatively, it is
     * possible to support different patterns, where there rank numbers are
     * assigned in a "round-robin" fashion. */
    int consecutive_ranks = 1;
    if (!consecutive_ranks) {
        limit = limit / ppn;
        ppn   = 1; /* from now on - ignore non-host-master members. */
    }

    /* Calculate the tree */
    do {
        for (outer_idx = 0; outer_idx < limit; outer_idx += outer_range) {
            root = (outer_range < limit) ? outer_idx : params->root;
            for (inner_idx = outer_idx;
                (inner_idx < outer_idx + outer_range) && (inner_idx < limit);
                 inner_idx += inner_range) {
                if (my_idx == inner_idx) {
                    if (my_idx == root) {
                        continue;
                    }
                    up[(*up_cnt)++] = root;
                    if (*up_cnt == UCG_BUILTIN_TREE_MAX_RADIX) {
                        goto limit_exceeded;
                    }
                } else if (my_idx == root) {
                    down[(*down_cnt)++] = inner_idx;
                    if (*down_cnt == UCG_BUILTIN_TREE_MAX_RADIX) {
                        goto limit_exceeded;
                    }
                }
            }
        }
        inner_range *= radix;
        outer_range *= radix;
    } while (outer_range < (limit * radix));
    return UCS_OK;

limit_exceeded:
    ucs_error("Internal PPN limit (%i) exceeded", UCG_BUILTIN_TREE_MAX_RADIX);
    return UCS_ERR_UNSUPPORTED;
}


static ucs_status_t ucg_builtin_tree_build(const ucg_builtin_tree_params_t *params,
                                           ucg_builtin_plan_t *tree)
{
    ucg_group_member_index_t net_up[UCG_BUILTIN_TREE_MAX_RADIX] = {0};
    ucg_group_member_index_t net_down[UCG_BUILTIN_TREE_MAX_RADIX] = {0};
    ucg_group_member_index_t host_up[UCG_BUILTIN_TREE_MAX_RADIX] = {0};
    ucg_group_member_index_t host_down[UCG_BUILTIN_TREE_MAX_RADIX] = {0};

    unsigned ppn = 0;
    unsigned net_up_cnt = 0;
    unsigned net_down_cnt = 0;
    unsigned host_up_cnt = 0;
    unsigned host_down_cnt = 0;

    /**
     * "Master phase" would be the highest phase this member would be the master of.
     * By the end of this function, the master_phase represents the type of node
     * in the topology tree - one of the following:
     *
     * UCG_GROUP_MEMBER_DISTANCE_NONE:
     *         no children at all, father is socket-master.
     *
     * UCG_GROUP_MEMBER_DISTANCE_SOCKET:
     *         socket-master, possible children on socket, father is host-master.
     *
     * UCG_GROUP_MEMBER_DISTANCE_HOST:
     *         socket-and-host-master, possible socket-master children
     *         (one on each of the rest of the sockets) and other host-masters,
     *         father is a (single) host-master.
     *
     * UCG_GROUP_MEMBER_DISTANCE_CLUSTER:
     *         fabric(-and-host-and-socket)-master, possible children of all
     *         types (but it's his own socket-and-host-master), fathers can be
     *         a list of fabric-masters in a multi-root formation (topmost
     *         phase of each collective is all-to-all between fabric-masters).
     */
    enum ucg_group_member_distance master = UCG_GROUP_MEMBER_DISTANCE_CLUSTER;
    tree->super.my_index                  = params->group_params->member_index;
    ucs_status_t status = ucg_builtin_tree_add_intra(params, &ppn,
                                                     tree->super.my_index,
                                                     host_up, &host_up_cnt,
                                                     host_down, &host_down_cnt,
                                                     &master);
    if (ucs_unlikely(status != UCS_OK)) {
        return status;
    }

    /* Network peers calculation */
    if ((master >= UCG_GROUP_MEMBER_DISTANCE_HOST) &&
        (ppn < params->group_params->member_count)) {
        host_up_cnt = 0; /* ignore fake parent of index 0 */
        status = ucg_builtin_tree_add_inter(params, tree->super.my_index, ppn,
                                            net_up, &net_up_cnt,
                                            net_down, &net_down_cnt);
        if (ucs_unlikely(status != UCS_OK)) {
            return status;
        }
    }

    /* Some output, for informational purposes */
    unsigned member_idx;
    for (member_idx = 0; member_idx < host_up_cnt; member_idx++) {
        ucs_info("%u's tree (host) parent #%u/%u: %u ", tree->super.my_index,
                member_idx + 1, host_up_cnt, host_up[member_idx]);
    }
    for (member_idx = 0; member_idx < net_up_cnt; member_idx++) {
        ucs_info("%u's tree (net) parent #%u/%u: %u ", tree->super.my_index,
                member_idx + 1, net_up_cnt, net_up[member_idx]);
    }
    for (member_idx = 0; member_idx < net_down_cnt; member_idx++) {
        ucs_info("%u's tree (net) child  #%u/%u: %u ", tree->super.my_index,
                member_idx + 1, net_down_cnt, net_down[member_idx]);
    }
    for (member_idx = 0; member_idx < host_down_cnt; member_idx++) {
        ucs_info("%u's tree (host) child  #%u/%u: %u ", tree->super.my_index,
                member_idx + 1, host_down_cnt, host_down[member_idx]);
    }

    /* fill in the tree phases while establishing the connections */
    return ucg_builtin_tree_connect(tree, params, 1, ppn,
                                    (uct_ep_h*)(&tree->phss[MAX_PHASES]),
                                    host_up, host_up_cnt, net_up, net_up_cnt,
                                    net_down, net_down_cnt, host_down, host_down_cnt);
}

ucs_status_t ucg_builtin_tree_create(ucg_builtin_group_ctx_t *ctx,
        enum ucg_builtin_plan_topology_type plan_topo_type,
        //const ucg_builtin_plan_topology_t *topology,
        const ucg_builtin_config_t *config,
        const ucg_group_params_t *group_params,
        const ucg_collective_type_t *coll_type,
        uct_incast_cb_t incast_cb,
        ucg_builtin_plan_t **plan_p)
{
    /* Allocate worst-case memory footprint, resized down later */
    ucg_builtin_plan_t *tree =
            (ucg_builtin_plan_t*)UCS_ALLOC_CHECK(ALLOC_SIZE(UCG_BUILTIN_TREE_MAX_RADIX), "buitin_tree");

    ucs_trace("recursive plan allocated %u phases and %u endpoints",
              MAX_PHASES, UCG_BUILTIN_TREE_MAX_RADIX);

    ucs_list_head_init(&tree->by_root);
    tree->phs_cnt = 0;
    tree->ep_cnt  = 0;

    /* tree discovery and construction, by phase */
    ucg_builtin_tree_params_t params = {
            .ctx            = ctx,
            .plan_topo_type = plan_topo_type,
            //.topology     = topology,
            .coll_type      = coll_type,
            .group_params   = group_params,
            .config         = &config->tree,
            .incast_cb      = incast_cb,
            .root           = 0
    };

    ucs_status_t status = ucg_builtin_tree_build(&params, tree);
    if (status != UCS_OK) {
        return status;
    }

    *plan_p = tree;
    return UCS_OK;
}

//ucs_status_t ucg_builtin_topo_tree_set_root(ucg_group_member_index_t root,
//                                            ucg_group_member_index_t my_index,
//                                            ucg_builtin_plan_t *plan,
//                                            ucg_builtin_plan_phase_t **first_phase_p,
//                                            unsigned *phase_count_p)
//{
//    ucs_assert(root != 0);
//
//    /* Check if I'm rank #0, the original root of the plan */
//    ucg_builtin_topo_tree_root_phase_t *root_phase;
//    /* Search for a previously prepared step - by root rank number */
//    ucs_list_for_each(root_phase, &plan->by_root, list) {
//        if (root_phase->root == root) {
//            goto phase_found;
//        }
//    }
//
//    /* Extend the tree to allow for additional endpoints */
//    ucg_builtin_plan_t *tree = ucs_realloc(plan,
//            ALLOC_SIZE(UCG_BUILTIN_TREE_MAX_RADIX), "nonzero_root");
//    if (tree == NULL) {
//        return UCS_ERR_NO_MEMORY;
//    }
//
//    /* Create a new descriptor for this phase */
//    root_phase = UCS_ALLOC_CHECK(sizeof(*root_phase), "ucg_builtin_root_phase");
//    ucs_list_add_head(&plan->by_root, &root_phase->list);
//    root_phase->root = root;
//
//    /* Build new phases in addition to the existing tree, for this new root */
//    const ucg_builtin_tree_params_t *params =
//            (ucg_builtin_tree_params_t*)&plan->phss[plan->phs_cnt-1];
//    ucs_status_t status = ucg_builtin_tree_build(params, root_phase, plan);
//    if (status != UCS_OK) {
//        return UCS_OK;
//    }
//
//    /* Reduce the allocation size according to actual usage */
//    tree = (ucg_builtin_plan_t*)ucs_realloc(tree, ALLOC_SIZE(tree->ep_cnt), "buitin_tree");
//    ucs_assert(tree != NULL); /* only reduces size - should never fail */
//
//phase_found:
//    *first_phase_p = &root_phase->phss[0];
//    *phase_count_p = root_phase->phs_cnt;
//    return UCS_OK;
//}
