/*
 * Copyright (C) Huawei Technologies Co., Ltd. 2019-2020.  ALL RIGHTS RESERVED.
 * See file LICENSE for terms.
 */

#ifndef UCG_BUILTIN_OPS_H_
#define UCG_BUILTIN_OPS_H_

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include "../plan/builtin_plan.h"

#include <ucp/dt/dt.h>
#include <ucp/core/ucp_request.h>
#include <ucs/datastruct/ptr_array.h>

#ifndef HAVE_UCP_EXTENSIONS
#define UCT_COLL_DTYPE_MODE_BITS (0)
#endif

BEGIN_C_DECLS

/*
 * The built-in collective operations are composed of one or more steps.
 * In each step, we apply a method to a subgroup of peer processes.
 * Collectives are planned using "templates", and once the user
 * provides the details a step is "instantiated" from a suitable
 * template and the instance is executed. Often more than one instance
 * is created from the same template, and instances can run side-by-side.
 *
 * Methods are the basic algorithmic building blocks, like fan-in and
 * fan-out for trees, or the "Recursive K-ing" algorithm.
 * For example, Allreduce can either be done in two step,
 * fan-in and fanout, or in a single Recursive K-ing step.
 * Once the user requests an Allreduce operation - the selected
 * step templates are used to generate an instance
 * (or it is fetched from cache) and that instance is executed.
 */

extern ucg_plan_component_t ucg_builtin_component;


typedef union ucg_builtin_header_step {
    struct {
        ucg_coll_id_t  coll_id;
        ucg_step_idx_t step_idx;
    };
    uint16_t local_id;
} ucg_builtin_header_step_t;

typedef union ucg_builtin_header {
    struct {
        ucg_group_id_t group_id;
        ucg_builtin_header_step_t msg;
        ucg_offset_t remote_offset;
    };
    uint64_t header;
} ucg_builtin_header_t;

/*
 * The builtin operation
 */
enum ucg_builtin_op_step_flags {
    /* General characteristics */
    UCG_BUILTIN_OP_STEP_FLAG_LAST_STEP         = UCS_BIT(0),
    UCG_BUILTIN_OP_STEP_FLAG_SINGLE_ENDPOINT   = UCS_BIT(1),
    UCG_BUILTIN_OP_STEP_FLAG_SEND_STRIDED      = UCS_BIT(2),
    UCG_BUILTIN_OP_STEP_FLAG_SEND_VARIADIC     = UCS_BIT(3),
    UCG_BUILTIN_OP_STEP_FLAG_PIPELINED         = UCS_BIT(4),

    /* Alternative Methods for using the list of endpoints */
    UCG_BUILTIN_OP_STEP_FLAG_RECV_AFTER_SEND   = UCS_BIT(5),
    UCG_BUILTIN_OP_STEP_FLAG_RECV_BEFORE_SEND1 = UCS_BIT(6),
    UCG_BUILTIN_OP_STEP_FLAG_RECV1_BEFORE_SEND = UCS_BIT(7),

    /* Alternative Send types */
    UCG_BUILTIN_OP_STEP_FLAG_SEND_AM_SHORT     = UCS_BIT(8),
    UCG_BUILTIN_OP_STEP_FLAG_SEND_AM_BCOPY     = UCS_BIT(9),
    UCG_BUILTIN_OP_STEP_FLAG_SEND_AM_ZCOPY     = UCS_BIT(10),
    /* Note: AM_ZCOPY is like BCOPY with registered memory: it uses "eager"
     *       protocol, as opposed to PUT/GET (below) which use "rendezvous" */
    UCG_BUILTIN_OP_STEP_FLAG_SEND_PUT_ZCOPY    = UCS_BIT(11),
    UCG_BUILTIN_OP_STEP_FLAG_SEND_GET_ZCOPY    = UCS_BIT(12),
    UCG_BUILTIN_OP_STEP_FLAG_FRAGMENTED        = UCS_BIT(13),
    UCG_BUILTIN_OP_STEP_FLAG_WRITE_REMOTE_ADDR = UCS_BIT(14),

    UCG_BUILTIN_OP_STEP_FLAG_SWITCH_MASK       = UCS_MASK(15),

    /* Additional step information */
    UCG_BUILTIN_OP_STEP_FLAG_BCOPY_PACK_LOCK   = UCS_BIT(15),
    UCG_BUILTIN_OP_STEP_FLAG_TEMP_BUFFER_USED  = UCS_BIT(16),
    UCG_BUILTIN_OP_STEP_FLAG_PACKED_DTYPE_MODE = UCS_BIT(17),
    UCG_BUILTIN_OP_STEP_FLAG_AGGREGATED_PACKET = UCS_BIT(18)
}; /* Note: only 19 bits are allocated for this field in ucg_builtin_op_step_t */

enum ucg_builtin_op_step_comp_aggregation {
    UCG_BUILTIN_OP_STEP_COMP_AGGREGATE_NOP = 0,

    /* Aggregation of short (Active-)messages */
    UCG_BUILTIN_OP_STEP_COMP_AGGREGATE_WRITE,
    UCG_BUILTIN_OP_STEP_COMP_AGGREGATE_WRITE_OOO,
    UCG_BUILTIN_OP_STEP_COMP_AGGREGATE_GATHER,
    UCG_BUILTIN_OP_STEP_COMP_AGGREGATE_REDUCE,
    UCG_BUILTIN_OP_STEP_COMP_AGGREGATE_REDUCE_SWAP,

    /* Unpacking remote memory keys (for Rendezvous protocol) */
    UCG_BUILTIN_OP_STEP_COMP_AGGREGATE_REMOTE_KEY
}; /* Note: only 3 bits are allocated for this field in ucg_builtin_op_step_t */

enum ucg_builtin_op_step_comp_flags {
    UCG_BUILTIN_OP_STEP_COMP_FLAG_BATCHED_DATA    = UCS_BIT(0),
    UCG_BUILTIN_OP_STEP_COMP_FLAG_FRAGMENTED_DATA = UCS_BIT(1),
    UCG_BUILTIN_OP_STEP_COMP_FLAG_PACKED_LENGTH   = UCS_BIT(2),
    UCG_BUILTIN_OP_STEP_COMP_FLAG_PACKED_DATATYPE = UCS_BIT(3),
    UCG_BUILTIN_OP_STEP_COMP_FLAG_LONG_BUFFERS    = UCS_BIT(4),

    UCG_BUILTIN_OP_STEP_COMP_FLAG_MASK            = UCS_MASK(5)
}; /* Note: only 4 bits are allocated for this field in ucg_builtin_op_step_t */

enum ucg_builtin_op_step_comp_criteria {
    UCG_BUILTIN_OP_STEP_COMP_CRITERIA_SEND = 0,
    UCG_BUILTIN_OP_STEP_COMP_CRITERIA_SINGLE_MESSAGE,
    UCG_BUILTIN_OP_STEP_COMP_CRITERIA_MULTIPLE_MESSAGES,
    UCG_BUILTIN_OP_STEP_COMP_CRITERIA_MULTIPLE_MESSAGES_ZCOPY,
    UCG_BUILTIN_OP_STEP_COMP_CRITERIA_BY_FRAGMENT_OFFSET
}; /* Note: only 3 bits are allocated for this field in ucg_builtin_op_step_t */

enum ucg_builtin_op_step_comp_action {
    UCG_BUILTIN_OP_STEP_COMP_OP = 0,
    UCG_BUILTIN_OP_STEP_COMP_STEP,
    UCG_BUILTIN_OP_STEP_COMP_SEND
}; /* Note: only 2 bits are allocated for this field in ucg_builtin_op_step_t */

/* Definitions of several callback functions, used during an operation */
typedef struct ucg_builtin_op ucg_builtin_op_t;
typedef struct ucg_builtin_request ucg_builtin_request_t;
typedef void         (*ucg_builtin_op_init_cb_t)  (ucg_builtin_op_t *op,
                                                   ucg_coll_id_t coll_id);
typedef void         (*ucg_builtin_op_fini_cb_t)  (ucg_builtin_op_t *op);
typedef ucs_status_t (*ucg_builtin_op_optm_cb_t)  (ucg_builtin_op_t *op);

typedef struct ucg_builtin_zcomp {
    uct_completion_t           comp;
    ucg_builtin_request_t     *req;
} ucg_builtin_zcomp_t;

typedef struct ucg_builtin_op_step {
    enum ucg_builtin_op_step_flags            flags            :19;
    enum ucg_builtin_op_step_comp_flags       comp_flags       :5;
    enum ucg_builtin_op_step_comp_aggregation comp_aggregation :3;
    enum ucg_builtin_op_step_comp_criteria    comp_criteria    :3;
    enum ucg_builtin_op_step_comp_action      comp_action      :2;

    /* --- 4 bytes --- */

    uint8_t                    iter_ep;     /* iterator, somewhat volatile */
#define UCG_BUILTIN_OFFSET_PIPELINE_READY   ((ucg_offset_t)-1)
#define UCG_BUILTIN_OFFSET_PIPELINE_PENDING ((ucg_offset_t)-2)
    /* TODO: consider modifying "send_buffer" and removing iter_offset */

    uint8_t                    reserved;
    uint8_t                    ep_cnt;
    uint8_t                    batch_cnt;

    /* --- 8 bytes --- */

    ucg_builtin_plan_phase_t  *phase;
    uint8_t                   *send_buffer;
    union {
        size_t                 buffer_length;
        size_t                 dtype_length;
    };

    /* --- 32 bytes --- */

    ucg_builtin_header_t       am_header;
    void                      *uct_send;
    uint64_t                   fragments_total; /* != 1 for fragmented operations */
    uint32_t                   fragment_length; /* only for fragmented operations */
    ucg_offset_t               iter_offset;     /* iterator, somewhat volatile */

    /* --- 64 bytes --- */

    void                      *uct_progress;
    uct_iface_h                uct_iface;

    /* To enable pipelining of fragmented messages, each fragment has a counter,
     * similar to the request's overall "pending" counter. Once it reaches zero,
     * the fragment can be "forwarded" regardless of the other fragments.
     * This optimization is only valid for "*_WAYPOINT" methods. */
#define UCG_BUILTIN_FRAG_PENDING ((uint8_t)-1)
    volatile uint8_t          *fragment_pending;

    uint8_t                   *recv_buffer;
    int                       *var_counts;
    int                       *var_displs;
    uct_md_h                   uct_md;

    /* Send-type-specific fields */
    union {
        struct {
            uct_pack_callback_t  pack_full_cb;
            uct_pack_callback_t  pack_part_cb;
            uct_pack_callback_t  pack_single_cb;
        } bcopy;
        struct {
            uct_mem_h            memh;   /* Data buffer memory handle */
            uct_component_h      cmpt;   /* which component registered the memory */
            ucg_builtin_zcomp_t  zcomp;  /* completion context for UCT zcopy */
            uint64_t             raddr;  /* remote address (from previous step) */
            uct_rkey_bundle_t    rkey;   /* remote key (from previous step) */
        } zcopy;
    };
} UCS_S_PACKED UCS_V_ALIGNED(UCS_SYS_CACHE_LINE_SIZE) ucg_builtin_op_step_t;

enum ucg_builtin_op_flags {
    UCG_BUILTIN_OP_FLAG_BARRIER         = UCS_BIT(0),
    UCG_BUILTIN_OP_FLAG_REDUCE          = UCS_BIT(1),
    UCG_BUILTIN_OP_FLAG_ALLTOALL        = UCS_BIT(2),
    UCG_BUILTIN_OP_FLAG_SCATTER         = UCS_BIT(3),
    UCG_BUILTIN_OP_FLAG_GATHER_TERMINAL = UCS_BIT(4),
    UCG_BUILTIN_OP_FLAG_GATHER_WAYPOINT = UCS_BIT(5),
    UCG_BUILTIN_OP_FLAG_OPTIMIZE_CB     = UCS_BIT(6),
    UCG_BUILTIN_OP_FLAG_NON_CONTIGUOUS  = UCS_BIT(7),
    UCG_BUILTIN_OP_STEP_FLAG_FT_ONGOING = UCS_BIT(8),

    UCG_BUILTIN_OP_FLAG_SWITCH_MASK     = UCS_MASK(9),

    /* Various flags - only for non-contiguous cases */
    UCG_BUILTIN_OP_FLAG_VOLATILE_DT     = UCS_BIT(9),
    UCG_BUILTIN_OP_FLAG_SEND_PACK       = UCS_BIT(10),
    UCG_BUILTIN_OP_FLAG_SEND_UNPACK     = UCS_BIT(11),
    UCG_BUILTIN_OP_FLAG_RECV_PACK       = UCS_BIT(12),
    UCG_BUILTIN_OP_FLAG_RECV_UNPACK     = UCS_BIT(13)
};

/* Below are the flags relevant for step completion, a.k.a. op finalize stage */
#define UCG_BUILTIN_OP_FLAG_FINALIZE_MASK (UCG_BUILTIN_OP_FLAG_BARRIER     | \
                                           UCG_BUILTIN_OP_FLAG_ALLTOALL    | \
                                           UCG_BUILTIN_OP_FLAG_OPTIMIZE_CB | \
                                           UCG_BUILTIN_OP_FLAG_NON_CONTIGUOUS)

enum ucg_builtin_request_flags {
    UCG_BUILTIN_REQUEST_FLAG_HANDLE_OOO = UCS_BIT(0)
};

struct ucg_builtin_op {
    ucg_op_t                 super;
    ucg_builtin_op_step_t  **current;     /**< current step executed (for progress) */
    uint32_t                 flags;       /**< Flags for the op's init/finalize flows */
    uint32_t                 opt_cnt;     /**< optimization count-down */
    ucg_builtin_op_optm_cb_t optm_cb;     /**< optimization function for the operation */

    ucp_datatype_t           send_dt;     /**< Generic send datatype (if non-contig) */
    ucp_datatype_t           recv_dt;     /**< Generic receive datatype (if non-contig) */
    ucp_dt_state_t          *send_pack;   /**< send datatype - pack state */
    ucp_dt_state_t          *send_unpack; /**< send datatype - unpack state */
    ucp_dt_state_t          *recv_pack;   /**< recv datatype - pack state */
    ucp_dt_state_t          *recv_unpack; /**< recv datatype - unpack state */

    ucg_builtin_group_ctx_t *gctx;        /**< builtin-group context pointer */
    ucg_builtin_op_step_t    steps[];     /**< steps required to complete the operation */
} UCS_V_ALIGNED(UCS_SYS_CACHE_LINE_SIZE);

/*
 * For every instance of the builtin collective operation (op), we create allocate
 * a request to handle completion and interaction with the user (via API).
 */
struct ucg_builtin_request {
    volatile uint32_t         pending;      /**< number of step's pending messages */
    ucg_builtin_header_step_t expecting;    /**< Next packet expected (by header) */
    uint8_t                   flags;        /**< @ref ucg_builtin_request_flags */
    uint8_t                   am_id;        /**< Active Message Identifier */
    ucg_builtin_op_step_t    *step;         /**< indicator of current step within the op */
    ucg_builtin_op_t         *op;           /**< operation currently running */
    void                     *comp_req;     /**< completion status is written here */
    ucs_queue_elem_t          resend_queue; /**< membership in the resend queue */
};

/*
 * Incoming messages are processed for one of the collective operations
 * currently outstanding - arranged as a window (think: TCP) of slots.
 */
typedef struct ucg_builtin_comp_slot {
    ucg_builtin_request_t req;
    ucs_ptr_array_t       messages;
} UCS_V_ALIGNED(UCS_SYS_CACHE_LINE_SIZE) ucg_builtin_comp_slot_t;

typedef struct ucg_builtin_comp_desc {
    ucp_recv_desc_t      super;
    char                 padding[UCP_WORKER_HEADROOM_PRIV_SIZE];
    ucg_builtin_header_t header;
    char                 data[0];
} ucg_builtin_comp_desc_t;

typedef struct ucg_builtin_ctx {
    ucs_ptr_array_locked_t group_by_id;
    uint16_t               am_id;
    ucp_worker_h           worker;
    ucs_ptr_array_locked_t unexpected;
    ucg_builtin_config_t   config;
} ucg_builtin_ctx_t;


ucs_status_t ucg_builtin_step_create(ucg_builtin_plan_t *plan,
                                     ucg_builtin_plan_phase_t *phase,
                                     enum ucg_builtin_op_step_flags *flags,
                                     const ucg_collective_params_t *params,
                                     int8_t **current_data_buffer,
                                     ucg_op_reduce_full_f
                                     *selected_reduce_full_f,
                                     ucg_op_reduce_frag_f
                                     *selected_reduce_frag_f,
                                     int is_send_dt_contig,
                                     int is_recv_dt_contig,
                                     size_t send_dt_len,
                                     size_t recv_dt_len,
                                     uint32_t *op_flags,
                                     ucg_builtin_op_step_t *step,
                                     int *zcopy_step_skip);

ucs_status_t ucg_builtin_step_create_rkey_bcast(ucg_builtin_plan_t *plan,
                                                const ucg_collective_params_t *params,
                                                ucg_builtin_op_step_t *step);

ucs_status_t ucg_builtin_step_execute(ucg_builtin_request_t *req,
                                      ucg_builtin_header_t header);

ucs_status_t ucg_builtin_step_zcopy_prep(ucg_builtin_op_step_t *step,
                                         const ucg_collective_params_t *params);

ucs_status_t ucg_builtin_step_select_packers(const ucg_collective_params_t *params,
                                             size_t send_dt_len,
                                             int is_send_dt_contig,
                                             ucg_builtin_op_step_t *step);

ucs_status_t ucg_builtin_step_select_reducers(void *dtype, void *reduce_op,
                                              int is_contig, size_t dtype_len,
                                              int64_t dtype_cnt,
                                              ucg_builtin_config_t *config,
                                              ucg_op_reduce_full_f
                                              *selected_reduce_full_f,
                                              ucg_op_reduce_frag_f
                                              *selected_reduce_frag_f);


ucs_status_t ucg_builtin_op_create (ucg_plan_t *plan,
                                    const ucg_collective_params_t *params,
                                    ucg_op_t **op);

ucs_status_t ucg_builtin_op_consider_optimization(ucg_builtin_op_t *op,
                                                  ucg_builtin_config_t *config);

ucs_status_t ucg_builtin_op_trigger(ucg_op_t *op,
                                    ucg_coll_id_t coll_id,
                                    void *request);

void ucg_builtin_op_discard(ucg_op_t *op);

void ucg_builtin_op_finalize_by_flags(ucg_builtin_op_t *op);


void ucg_builtin_req_enqueue_resend(ucg_builtin_group_ctx_t *gctx,
                                    ucg_builtin_request_t *req);

int ucg_is_noncontig_allreduce(const ucg_group_params_t *group_params,
                               const ucg_collective_params_t *coll_params);

/*
 * Callback functions exported for debugging
 */
void ucg_builtin_print_reduce_cb_name(ucg_op_reduce_full_f reduce_cb);

void ucg_builtin_print_pack_cb_name(uct_pack_callback_t pack_single_cb);

void ucg_builtin_print_flags(ucg_builtin_op_step_t *step, uint32_t op_flags);

/*
 * Macros to generate the headers of all bcopy packing callback functions.
 */
typedef ssize_t (*packed_send_t)(uct_ep_h, uint8_t, uct_pack_callback_t, void*, unsigned);

static UCS_F_ALWAYS_INLINE void
ucg_builtin_step_get_local_address(ucg_builtin_op_step_t *step, int var_stride,
                                   uint8_t **buffer, size_t *length)
{
    if (var_stride) {
        size_t tmp_length = step->buffer_length;
        *buffer           = step->send_buffer +
                            step->var_displs[step->iter_offset] * tmp_length;
        *length           = step->var_counts[step->iter_offset] * tmp_length;
    } else {
        *length           = step->buffer_length;
        *buffer           = step->send_buffer + step->iter_offset;
    }
}

static UCS_F_ALWAYS_INLINE void
ucg_builtin_step_set_remote_address(ucg_builtin_op_step_t *step, uint8_t **ptr)
{
    uint8_t *sent    = step->send_buffer +
                      (step->iter_offset * step->buffer_length);
    *(uint64_t*)sent = (uint64_t)*ptr;
    *ptr             = sent;

    ucs_assert(step->iter_offset < step->ep_cnt);
    ucs_assert(step->buffer_length == (sizeof(uint64_t) +
                                       step->phase->md_attr->rkey_packed_size));
}

static UCS_F_ALWAYS_INLINE size_t
ucg_builtin_step_length(ucg_builtin_op_step_t *step,
                        const ucg_collective_params_t *params,
                        int is_send)
{
    size_t unpacked;
    int is_fragmented = (step->flags & UCG_BUILTIN_OP_STEP_FLAG_FRAGMENTED);
    int is_len_packed = (step->comp_flags &
                         UCG_BUILTIN_OP_STEP_COMP_FLAG_PACKED_LENGTH);

    if (is_fragmented || is_len_packed) {
        unpacked = step->dtype_length;
        if (is_len_packed) {
            unpacked = UCT_COLL_DTYPE_MODE_UNPACK_VALUE(unpacked);
        }

        return unpacked * (is_send ? params->send.count : params->recv.count);
    }

    return step->buffer_length;
}

/*
 * This number sets the number of slots available for collective operations.
 * Each operation occupies a slot, so no more than this number of collectives
 * can take place at the same time. The slot is determined by the collective
 * operation id (ucg_coll_id_t) - modulo this constant. Translating "coll_id"
 * to slot# happens on every incoming packet, so this constant is best kept
 * determinable at compile time, and set to a power of 2 (<= 64, to fit into
 * the resend bit-field).
 */
#define UCG_BUILTIN_MAX_CONCURRENT_OPS (16)

END_C_DECLS

#endif
