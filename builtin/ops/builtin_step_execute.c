/*
 * Copyright (C) Huawei Technologies Co., Ltd. 2019.  ALL RIGHTS RESERVED.
 * See file LICENSE for terms.
 */

#include <string.h>
#include <ucs/arch/atomic.h>
#include <ucs/profile/profile.h>

#include "builtin_ops.h"
#include "builtin_comp_step.inl"

/******************************************************************************
 *                                                                            *
 *                         Operation Step Execution                           *
 *                                                                            *
 ******************************************************************************/

#define UCG_BUILTIN_ASSERT_SEND(step, send_type) \
    ucs_assert(((step)->flags & (UCG_BUILTIN_OP_STEP_FLAG_SEND_AM_SHORT  | \
                                 UCG_BUILTIN_OP_STEP_FLAG_SEND_AM_BCOPY  | \
                                 UCG_BUILTIN_OP_STEP_FLAG_SEND_AM_ZCOPY  | \
                                 UCG_BUILTIN_OP_STEP_FLAG_SEND_PUT_ZCOPY | \
                                 UCG_BUILTIN_OP_STEP_FLAG_SEND_GET_ZCOPY)) == \
                                 UCG_BUILTIN_OP_STEP_FLAG_SEND_ ## send_type);

ucs_status_t static UCS_F_ALWAYS_INLINE
ucg_builtin_step_dummy_send(ucg_builtin_request_t *req,
                            ucg_builtin_op_step_t *step,
                            uct_ep_h ep, uint8_t am_id,
                            ucg_builtin_header_t header,
                            int var_stride)
{
    return UCS_OK;
}

static UCS_F_ALWAYS_INLINE ucs_status_t
ucg_builtin_step_am_short_common(ucg_builtin_request_t *req,
                                 ucg_builtin_op_step_t *step,
                                 uct_ep_h ep, uint8_t am_id,
                                 ucg_builtin_header_t header,
                                 uint8_t *buffer, size_t length)
{
    uct_ep_am_short_func_t ep_am_short = step->uct_send;

    UCG_BUILTIN_ASSERT_SEND(step, AM_SHORT);

    return ep_am_short(ep, am_id, header.header, buffer, length);
}

static UCS_F_ALWAYS_INLINE ucs_status_t
ucg_builtin_step_am_short_one(ucg_builtin_request_t *req,
                              ucg_builtin_op_step_t *step,
                              uct_ep_h ep, uint8_t am_id,
                              ucg_builtin_header_t header,
                              int var_stride)
{
    size_t length;
    uint8_t *buffer;
    ucg_builtin_step_get_local_address(step, var_stride, &buffer, &length);

    return ucg_builtin_step_am_short_common(req, step, ep, am_id, header,
                                            buffer, length);
}

static UCS_F_ALWAYS_INLINE ucs_status_t
ucg_builtin_step_am_short_rkey(ucg_builtin_request_t *req,
                               ucg_builtin_op_step_t *step,
                               uct_ep_h ep, uint8_t am_id,
                               ucg_builtin_header_t header,
                               int var_stride)
{
    size_t length;
    uint8_t *buffer;
    ucg_builtin_step_get_local_address(step + 1, var_stride, &buffer, &length);

    ucg_builtin_step_set_remote_address(step, &buffer);

    return ucg_builtin_step_am_short_common(req, step, ep, am_id, header,
                                            buffer, length);
}

static UCS_F_ALWAYS_INLINE ucs_status_t
ucg_builtin_step_am_short_max(ucg_builtin_request_t *req,
                              ucg_builtin_op_step_t *step,
                              uct_ep_h ep, uint8_t am_id,
                              ucg_builtin_header_t am_iter,
                              int is_pipelined)
{
    ucs_status_t status;
    ucg_offset_t frag_size       = step->fragment_length;
    int is_packed                = ((step->flags &
                                     UCG_BUILTIN_OP_STEP_FLAG_PACKED_DTYPE_MODE)
                                    != 0);
    frag_size                    = ((frag_size) >>
                                    (UCT_COLL_DTYPE_MODE_BITS * is_packed));
    uint8_t *sbuf                = step->send_buffer;
    uint8_t *buffer_iter         = sbuf + step->iter_offset;
    size_t buffer_length         = frag_size * step->fragments_total;
    uint8_t *buffer_iter_limit   = sbuf + buffer_length - frag_size;
    am_iter.remote_offset        = (is_pipelined) ? step->iter_offset :
                                   am_iter.remote_offset + step->iter_offset;

    UCG_BUILTIN_ASSERT_SEND(step, AM_SHORT);
    ucs_assert(step->iter_offset != UCG_BUILTIN_OFFSET_PIPELINE_READY);
    ucs_assert(step->iter_offset != UCG_BUILTIN_OFFSET_PIPELINE_PENDING);
#ifdef HAVE_UCT_COLLECTIVES
    ucs_assert(frag_size == (is_packed ?
               UCT_COLL_DTYPE_MODE_UNPACK_VALUE(step->fragment_length) :
               step->fragment_length));
#endif

    /* send every fragment but the last */
    uct_ep_am_short_func_t ep_am_short = step->uct_send;
    if (ucs_likely(buffer_iter < buffer_iter_limit)) {
        do {
            status = ep_am_short(ep, am_id, am_iter.header, buffer_iter, frag_size);

            if (is_pipelined) {
                return status;
            }

            buffer_iter           += frag_size;
            am_iter.remote_offset += frag_size;
        } while ((status == UCS_OK) && (buffer_iter < buffer_iter_limit));

        /* send last fragment of the message */
        if (ucs_unlikely(status != UCS_OK)) {
            /* assuming UCS_ERR_NO_RESOURCE, restore the state for re-entry */
            if (!is_pipelined) {
                step->iter_offset = buffer_iter - frag_size - step->send_buffer;
            }

            return status;
        }
    }

    status = ep_am_short(ep, am_id, am_iter.header, buffer_iter,
                         sbuf + step->buffer_length - buffer_iter);
    step->iter_offset = (status == UCS_OK) ? 0 : buffer_iter - sbuf;
    return status;
}

static UCS_F_ALWAYS_INLINE ucs_status_t
ucg_builtin_step_am_bcopy_one(ucg_builtin_request_t *req,
                              ucg_builtin_op_step_t *step,
                              uct_ep_h ep, uint8_t am_id,
                              ucg_builtin_header_t header,
                              int var_stride)
{
    unsigned uct_flags;
    if (ucs_unlikely(step->flags & UCG_BUILTIN_OP_STEP_FLAG_BCOPY_PACK_LOCK)) {
        uct_flags = UCT_SEND_FLAG_PACK_LOCK;
    } else {
        uct_flags = 0;
    }

    UCG_BUILTIN_ASSERT_SEND(step, AM_BCOPY);
    ucs_assert(header.header != 0);

    uct_ep_am_bcopy_func_t ep_am_bcopy = step->uct_send;
    step->am_header                    = header;

    ssize_t len = ep_am_bcopy(ep, am_id, step->bcopy.pack_single_cb,
                              req, uct_flags);

    return (ucs_unlikely(len < 0)) ? (ucs_status_t)len : UCS_OK;
}

static UCS_F_ALWAYS_INLINE ucs_status_t
ucg_builtin_step_am_bcopy_max(ucg_builtin_request_t *req,
                              ucg_builtin_op_step_t *step,
                              uct_ep_h ep, uint8_t am_id,
                              ucg_builtin_header_t header,
                              int is_pipelined)
{
    ssize_t len;
    ucg_offset_t frag_size        = step->fragment_length;
    ucg_offset_t iter_limit       = step->buffer_length - frag_size;
    packed_send_t send_func       = step->uct_send;
    step->am_header               = header;

    if (is_pipelined) {
        step->am_header.remote_offset = step->iter_offset;
    }

    unsigned uct_flags;
    if (ucs_unlikely(step->flags & UCG_BUILTIN_OP_STEP_FLAG_BCOPY_PACK_LOCK)) {
        uct_flags = UCT_SEND_FLAG_PACK_LOCK;
    } else {
        uct_flags = 0;
    }

    UCG_BUILTIN_ASSERT_SEND(step, AM_BCOPY);
    ucs_assert(step->iter_offset != UCG_BUILTIN_OFFSET_PIPELINE_READY);
    ucs_assert(step->iter_offset != UCG_BUILTIN_OFFSET_PIPELINE_PENDING);
    ucs_assert(header.header != 0);

    /* check if this is not, by any chance, the last fragment */
    if (ucs_likely(step->iter_offset < iter_limit)) {
        /* send every fragment but the last */
        do {
            len = send_func(ep, am_id, step->bcopy.pack_full_cb, req, uct_flags);

            if (is_pipelined) {
                return ucs_unlikely(len < 0) ? (ucs_status_t)len : UCS_OK;
            }

            step->am_header.remote_offset += frag_size;
            step->iter_offset             += frag_size;
        } while ((len >= 0) && (step->iter_offset < iter_limit));

        if (ucs_unlikely(len < 0)) {
            step->am_header.remote_offset -= frag_size;
            step->iter_offset             -= frag_size;
            /*
             * TODO: step->am_header might be overwritten later, in
             *       the check_pending() call - need to prevent this!
             */

            return (ucs_status_t)len;
        }
    }

    /* Send last fragment of the message */
    len = send_func(ep, am_id, step->bcopy.pack_part_cb, req, uct_flags);
    if (ucs_unlikely(len < 0)) {
        return (ucs_status_t)len;
    }

    /* iter_offset can not set to be zero for pipelining */
    if (!is_pipelined) {
        step->am_header.remote_offset = step->iter_offset = 0;
    }

    return UCS_OK;
}

static UCS_F_ALWAYS_INLINE ucs_status_t
ucg_builtin_step_zcopy_common(ucg_builtin_request_t *req,
                              ucg_builtin_op_step_t *step,
                              uct_ep_h ep, uint8_t am_id,
                              ucg_builtin_header_t header,
                              uint8_t *buffer, size_t length,
                              unsigned type)
{
    uct_ep_am_zcopy_func_t ep_am_zcopy;
    uct_ep_put_zcopy_func_t ep_put_zcopy;
    uct_ep_get_zcopy_func_t ep_get_zcopy;

    uct_iov_t iov = {
            .buffer = buffer,
            .length = length,
            .memh   = step->zcopy.memh,
            .stride = 0,
            .count  = 1
    };

    ucg_builtin_zcomp_t *zcomp = &step->zcopy.zcomp;
    zcomp->req = req;

    ucs_status_t status;
    switch (type) {
    case UCG_BUILTIN_OP_STEP_FLAG_SEND_AM_ZCOPY:
        ep_am_zcopy = step->uct_send;
        status = ep_am_zcopy(ep, am_id, &header, sizeof(header),
                             &iov, 1, 0, &zcomp->comp);
        break;

    case UCG_BUILTIN_OP_STEP_FLAG_SEND_PUT_ZCOPY:
        ep_put_zcopy = step->uct_send;
        status = ep_put_zcopy(ep, &iov, 1, step->zcopy.raddr,
                              step->zcopy.rkey.rkey, &zcomp->comp);
        break;

    case UCG_BUILTIN_OP_STEP_FLAG_SEND_GET_ZCOPY:
        ep_get_zcopy = step->uct_send;
        status = ep_get_zcopy(ep, &iov, 1, step->zcopy.raddr,
                              step->zcopy.rkey.rkey, &zcomp->comp);
        break;

    default:
        return UCS_ERR_INVALID_PARAM;
    }

    return ucs_unlikely(status != UCS_INPROGRESS) ? status : UCS_OK;
}

static UCS_F_ALWAYS_INLINE ucs_status_t
ucg_builtin_step_am_zcopy_one(ucg_builtin_request_t *req,
                              ucg_builtin_op_step_t *step,
                              uct_ep_h ep, uint8_t am_id,
                              ucg_builtin_header_t header,
                              int var_stride)
{
    size_t length;
    uint8_t *buffer;
    ucg_builtin_step_get_local_address(step, var_stride, &buffer, &length);

    UCG_BUILTIN_ASSERT_SEND(step, AM_ZCOPY);

    return ucg_builtin_step_zcopy_common(req, step, ep, am_id, header, buffer, length,
                                         UCG_BUILTIN_OP_STEP_FLAG_SEND_AM_ZCOPY);
}

static UCS_F_ALWAYS_INLINE ucs_status_t
ucg_builtin_step_am_zcopy_rkey(ucg_builtin_request_t *req,
                               ucg_builtin_op_step_t *step,
                               uct_ep_h ep, uint8_t am_id,
                               ucg_builtin_header_t header,
                               int var_stride)
{
    size_t length;
    uint8_t *buffer;
    ucg_builtin_step_get_local_address(step + 1, var_stride, &buffer, &length);

    ucg_builtin_step_set_remote_address(step, &buffer);

    UCG_BUILTIN_ASSERT_SEND(step, AM_ZCOPY);

    return ucg_builtin_step_zcopy_common(req, step, ep, am_id, header, buffer, length,
                                         UCG_BUILTIN_OP_STEP_FLAG_SEND_AM_ZCOPY);
}

static UCS_F_ALWAYS_INLINE ucs_status_t
ucg_builtin_step_put_zcopy_one(ucg_builtin_request_t *req,
                               ucg_builtin_op_step_t *step,
                               uct_ep_h ep, uint8_t am_id,
                               ucg_builtin_header_t header,
                               int var_stride)
{
    size_t length;
    uint8_t *buffer;
    ucg_builtin_step_get_local_address(step, var_stride, &buffer, &length);

    UCG_BUILTIN_ASSERT_SEND(step, PUT_ZCOPY);

    return ucg_builtin_step_zcopy_common(req, step, ep, am_id, header, buffer, length,
                                         UCG_BUILTIN_OP_STEP_FLAG_SEND_PUT_ZCOPY);
}

static UCS_F_ALWAYS_INLINE ucs_status_t
ucg_builtin_step_get_zcopy_one(ucg_builtin_request_t *req,
                               ucg_builtin_op_step_t *step,
                               uct_ep_h ep, uint8_t am_id,
                               ucg_builtin_header_t header,
                               int var_stride)
{
    size_t length;
    uint8_t *buffer;
    ucg_builtin_step_get_local_address(step, var_stride, &buffer, &length);

    UCG_BUILTIN_ASSERT_SEND(step, GET_ZCOPY);

    return ucg_builtin_step_zcopy_common(req, step, ep, am_id, header, buffer, length,
                                         UCG_BUILTIN_OP_STEP_FLAG_SEND_GET_ZCOPY);
}

static UCS_F_ALWAYS_INLINE ucs_status_t
ucg_builtin_step_am_zcopy_max(ucg_builtin_request_t *req,
                              ucg_builtin_op_step_t *step,
                              uct_ep_h ep, uint8_t am_id,
                              ucg_builtin_header_t header,
                              int is_pipelined)
{
    ucs_status_t status;
    ucg_offset_t frag_size        = step->fragment_length;
    uint8_t *sbuf                 = step->send_buffer;
    void* iov_buffer_limit        = sbuf + step->buffer_length - frag_size;
    ucg_builtin_zcomp_t *zcomp    = &step->zcopy.zcomp;
    step->am_header.remote_offset = (is_pipelined) ? step->iter_offset :
                                    step->am_header.remote_offset;

    uct_iov_t iov = {
            .buffer = sbuf + step->iter_offset,
            .length = frag_size,
            .memh   = step->zcopy.memh,
            .stride = 0,
            .count  = 1
    };

    UCG_BUILTIN_ASSERT_SEND(step, AM_ZCOPY);
    ucs_assert(step->iter_offset != UCG_BUILTIN_OFFSET_PIPELINE_READY);
    ucs_assert(step->iter_offset != UCG_BUILTIN_OFFSET_PIPELINE_PENDING);

    /* check if this is not, by any chance, the last fragment */
    uct_ep_am_zcopy_func_t ep_am_zcopy = step->uct_send;
    if (ucs_likely(iov.buffer < iov_buffer_limit)) {
        /* send every fragment but the last */
        do {
            status = ep_am_zcopy(ep, am_id, &header, sizeof(header),
                                 &iov, 1, 0, &zcomp->comp);
            (zcomp++)->req = req;

            if (is_pipelined) {
                return status;
            }

            header.remote_offset += frag_size;
            iov.buffer = (void*)((uint8_t*)iov.buffer + frag_size);
        } while ((status == UCS_INPROGRESS) && (iov.buffer < iov_buffer_limit));

        if (ucs_unlikely(status != UCS_INPROGRESS)) {
            step->iter_offset = (uint8_t*)iov.buffer - sbuf - frag_size;
            step->am_header = header;
            return status;
        }
    }

    /* Send last fragment of the message */
    zcomp->req = req;
    iov.length = sbuf + step->buffer_length - (uint8_t*)iov.buffer;
    status     = ep_am_zcopy(ep, am_id, &header, sizeof(header),
                             &iov, 1, 0, &zcomp->comp);
    if (ucs_unlikely(status != UCS_INPROGRESS)) {
        step->iter_offset = (uint8_t*)iov.buffer - sbuf;
        step->am_header = header;
        return status;
    }

    step->am_header.remote_offset = 0;
    return UCS_OK;
}

/*
 * Below is a set of macros, generating most bit-field combinations of
 * step->flags in the switch-case inside @ref ucg_builtin_step_execute() .
 */

#define case_send_full(req, step, phase, _is_last, _is_1ep, _fixed_stride,\
                       _var_stride, _is_pipelined, _is_recv, _is_rs1, _is_r1s, \
                       _send_flag, _send_func)                                 \
   case ((_is_last      ? UCG_BUILTIN_OP_STEP_FLAG_LAST_STEP          : 0) |   \
         (_is_1ep       ? UCG_BUILTIN_OP_STEP_FLAG_SINGLE_ENDPOINT    : 0) |   \
         (_fixed_stride ? UCG_BUILTIN_OP_STEP_FLAG_SEND_STRIDED       : 0) |   \
         (_var_stride   ? UCG_BUILTIN_OP_STEP_FLAG_SEND_VARIADIC      : 0) |   \
         (_is_pipelined ? UCG_BUILTIN_OP_STEP_FLAG_PIPELINED          : 0) |   \
         (_is_recv      ? UCG_BUILTIN_OP_STEP_FLAG_RECV_AFTER_SEND    : 0) |   \
         (_is_rs1       ? UCG_BUILTIN_OP_STEP_FLAG_RECV_BEFORE_SEND1  : 0) |   \
         (_is_r1s       ? UCG_BUILTIN_OP_STEP_FLAG_RECV1_BEFORE_SEND  : 0) |   \
         _send_flag):                                                          \
                                                                               \
        is_zcopy = (_send_flag) & UCG_BUILTIN_OP_STEP_FLAG_SEND_AM_ZCOPY;      \
        if (_is_pipelined) {                                                   \
            frags_per_ep = step->fragments_total / step->ep_cnt;               \
            ucs_assert(!(step->fragments_total % step->ep_cnt));               \
        }                                                                      \
                                                                               \
        if ((_is_rs1 || _is_r1s) && (step->iter_ep == 0)) {                    \
            uint32_t new_cnt = step->iter_ep = _is_r1s ? 1 : phase->ep_cnt - 1;\
            if (_is_pipelined) {                                               \
                memset((void*)step->fragment_pending, new_cnt, frags_per_ep);  \
            }                                                                  \
            if (!is_zcopy) {                                                   \
                if (!_is_pipelined) {                                          \
                    frags_per_ep = step->fragments_total / step->ep_cnt;       \
                }                                                              \
                req->pending = new_cnt * frags_per_ep;                         \
            } /* Otherwise default init of ep_cnt*num_fragments is correct */  \
            break; /* Beyond the switch-case we fall-back to receiving */      \
        }                                                                      \
                                                                               \
        if (_is_recv && is_zcopy) {                                            \
            /* Both zcopy callbacks and incoming messages use pending, so ...*/\
            req->pending = 2 * step->fragments_total;                          \
        }                                                                      \
                                                                               \
        /* Perform one or many send operations, unless an error occurs */      \
        if (_is_1ep) {                                                         \
            status = _send_func (req, step, phase->single_ep, am_id, header,   \
                                 _is_pipelined | _var_stride);                 \
            if (ucs_unlikely(UCS_STATUS_IS_ERR(status))) {                     \
                goto step_execute_error;                                       \
            }                                                                  \
            if (!(_is_pipelined || _var_stride)) {                             \
                step->iter_offset = 0;                                         \
            }                                                                  \
        } else {                                                               \
            if ((_is_pipelined) && (ucs_unlikely(step->iter_offset ==          \
                                    UCG_BUILTIN_OFFSET_PIPELINE_PENDING))) {   \
                /* find a pending offset to progress */                        \
                unsigned frag_idx = 0;                                         \
                while ((frag_idx < frags_per_ep) &&                            \
                       (step->fragment_pending[frag_idx] ==                    \
                        UCG_BUILTIN_FRAG_PENDING)) {                           \
                    frag_idx++;                                                \
                }                                                              \
                ucs_assert(frag_idx < frags_per_ep);                           \
                step->iter_offset = frag_idx * step->fragment_length;          \
            }                                                                  \
                                                                               \
            ep_iter = ep_last = phase->multi_eps;                              \
            ep_iter += step->iter_ep;                                          \
            ep_last += phase->ep_cnt;                                          \
            if (_fixed_stride) {                                               \
                item_interval = step->buffer_length;                           \
            }                                                                  \
                                                                               \
            do {                                                               \
                status = _send_func (req, step, *ep_iter, am_id, header,       \
                                     _is_pipelined | _var_stride);             \
                if (ucs_unlikely(UCS_STATUS_IS_ERR(status))) {                 \
                    step->iter_ep = ep_iter - phase->multi_eps;                \
                    goto step_execute_error;                                   \
                }                                                              \
                                                                               \
                if (_fixed_stride) {                                           \
                    step->iter_offset += item_interval;                        \
                } else if (!(_is_pipelined || _var_stride)) {                  \
                    step->iter_offset    = 0;                                  \
                    header.remote_offset = 0; /* considering resend flow */    \
                }                                                              \
            } while (++ep_iter < ep_last);                                     \
                                                                               \
            if (_is_pipelined) {                                               \
                /* Reset the iterator for the next pipelined incoming packet */\
                step->iter_ep = _is_r1s ? 1 : phase->ep_cnt - 1;               \
                ucs_assert(_is_r1s + _is_rs1 > 0);                             \
                                                                               \
                /* Check if this invocation is a result of a resend attempt */ \
                unsigned idx = step->iter_offset / step->fragment_length;      \
                if (ucs_unlikely(step->fragment_pending[idx] ==                \
                        UCG_BUILTIN_FRAG_PENDING)) {                           \
                    step->fragment_pending[idx] = 0;                           \
                                                                               \
                    /* Look for other packets in need of resending */          \
                    for (idx = 0; idx < frags_per_ep; idx++) {                 \
                        if (step->fragment_pending[idx] ==                     \
                                UCG_BUILTIN_FRAG_PENDING) {                    \
                            /* Found such packets - mark for next resend */    \
                            step->iter_offset = idx * step->fragment_length;   \
                            status            = UCS_ERR_NO_RESOURCE;           \
                            goto step_execute_error;                           \
                        }                                                      \
                    }                                                          \
                } else {                                                       \
                    ucs_assert(step->fragment_pending[idx] == 0);              \
                }                                                              \
                step->iter_offset = UCG_BUILTIN_OFFSET_PIPELINE_READY;         \
            } else {                                                           \
                step->iter_ep = 0; /* Reset the per-step endpoint iterator */  \
                if (_fixed_stride) {                                           \
                    step->iter_offset = 0;                                     \
                }                                                              \
            }                                                                  \
        }                                                                      \
                                                                               \
        /* Potential completions (the operation may have finished by now) */   \
        if ((!_is_recv && !is_zcopy) || (req->pending == 0)) {                 \
            /* Nothing else to do - complete this step */                      \
            if (_is_last) {                                                    \
                ucg_builtin_comp_last_step_cb(req, UCS_OK);                    \
                return UCS_OK;                                                 \
            } else {                                                           \
                step->am_header = header;                                      \
                return ucg_builtin_comp_step_cb(req);                          \
            }                                                                  \
        }                                                                      \
        break;

#define  case_send_1ep(r, s, p,    _is_1ep, _fixed_stride, _var_stride, _is_pipelined, _is_recv, _is_rs1, _is_r1s, _send_flag, _send_func) \
        case_send_full(r, s, p, 0, _is_1ep, _fixed_stride, _var_stride, _is_pipelined, _is_recv, _is_rs1, _is_r1s, _send_flag, _send_func) \
        case_send_full(r, s, p, 1, _is_1ep, _fixed_stride, _var_stride, _is_pipelined, _is_recv, _is_rs1, _is_r1s, _send_flag, _send_func)

#define case_send_strides(r, s, p,    _fixed_stride, _var_stride, _is_pipelined, _is_recv, _is_rs1, _is_r1s, _send_flag, _send_func) \
            case_send_1ep(r, s, p, 0, _fixed_stride, _var_stride, _is_pipelined, _is_recv, _is_rs1, _is_r1s, _send_flag, _send_func) \
            case_send_1ep(r, s, p, 1, _fixed_stride, _var_stride, _is_pipelined, _is_recv, _is_rs1, _is_r1s, _send_flag, _send_func)

#define case_send_pipelined(r, s, p,       _is_pipelined, _is_recv, _is_rs1, _is_r1s, _send_flag, _send_func) \
          case_send_strides(r, s, p, 0, 0, _is_pipelined, _is_recv, _is_rs1, _is_r1s, _send_flag, _send_func) \
          case_send_strides(r, s, p, 1, 0, _is_pipelined, _is_recv, _is_rs1, _is_r1s, _send_flag, _send_func) \
          case_send_strides(r, s, p, 0, 1, _is_pipelined, _is_recv, _is_rs1, _is_r1s, _send_flag, _send_func)

#define    case_send_method(r, s, p,    _is_recv, _is_rs1, _is_r1s, _send_flag, _send_func) \
        case_send_pipelined(r, s, p, 0, _is_recv, _is_rs1, _is_r1s, _send_flag, _send_func) \
        case_send_pipelined(r, s, p, 1, _is_recv, _is_rs1, _is_r1s, _send_flag, _send_func)

#define        case_send(r, s, p,          _send_flag, _send_func) \
        case_send_method(r, s, p, 0, 0, 0, _send_flag, _send_func) \
        case_send_method(r, s, p, 1, 0, 0, _send_flag, _send_func) \
        case_send_method(r, s, p, 0, 1, 0, _send_flag, _send_func) \
        case_send_method(r, s, p, 0, 0, 1, _send_flag, _send_func)

/*
 * Executing a single step is the heart of the Builtin planner.
 * This function advances to the next step (some invocations negate that...),
 * sends and then recieves according to the instructions of this step.
 * The function returns the status, typically one of the following:
 * > UCS_OK - collective operation (not just this step) has been completed.
 * > UCS_INPROGRESS - sends complete, waiting on some messages to be recieved.
 * > otherwise - an error has occurred.
 *
 * For example, a "complex" case is when the message is fragmented, and requires
 * both recieveing and sending in a single step, like in REDUCE_WAYPOINT. The
 * first call, coming from @ref ucg_builtin_op_trigger() , will enter the first
 * branch ("step_ep" is zero when a new step is starting), will process some
 * potential incoming messages (arriving beforehand) - returning UCS_INPROGRESS.
 * Subsequent calls to "progress()" will handle the rest of the incoming
 * messages for this step, and eventually call this function again from within
 * @ref ucg_builtin_comp_step_cb() . This call will choose the second branch,
 * the swith-case, which will send the message and
 */
UCS_PROFILE_FUNC(ucs_status_t, ucg_builtin_step_execute, (req, header),
                 ucg_builtin_request_t *req, ucg_builtin_header_t header)
{
    int is_zcopy;
    ucs_status_t status;
    size_t item_interval;
    unsigned frags_per_ep;
    uct_ep_h *ep_iter, *ep_last;

    uint8_t am_id                   = req->am_id;
    ucg_builtin_op_step_t *step     = req->step;
    ucg_builtin_plan_phase_t *phase = step->phase;
    ucg_builtin_comp_slot_t *slot   = ucs_container_of(req, ucg_builtin_comp_slot_t, req);

    /* This step either starts by sending or contains no send operations */
    switch (step->flags & UCG_BUILTIN_OP_STEP_FLAG_SWITCH_MASK) {
    /* Single-send operations (only one fragment passed to UCT) */
    case_send(req, step, phase, 0, /* for recv-only steps */
              ucg_builtin_step_dummy_send)
    case_send(req, step, phase, UCG_BUILTIN_OP_STEP_FLAG_SEND_AM_SHORT,
              ucg_builtin_step_am_short_one)
    case_send(req, step, phase, UCG_BUILTIN_OP_STEP_FLAG_SEND_AM_BCOPY,
              ucg_builtin_step_am_bcopy_one)
    case_send(req, step, phase, UCG_BUILTIN_OP_STEP_FLAG_SEND_AM_ZCOPY,
              ucg_builtin_step_am_zcopy_one)
    case_send(req, step, phase, UCG_BUILTIN_OP_STEP_FLAG_SEND_PUT_ZCOPY,
              ucg_builtin_step_put_zcopy_one)
    case_send(req, step, phase, UCG_BUILTIN_OP_STEP_FLAG_SEND_GET_ZCOPY,
              ucg_builtin_step_get_zcopy_one)

    /* Remote key broadcasting operations */
    case_send(req, step, phase, UCG_BUILTIN_OP_STEP_FLAG_WRITE_REMOTE_ADDR,
              ucg_builtin_step_dummy_send)
    case_send(req, step, phase, UCG_BUILTIN_OP_STEP_FLAG_SEND_AM_SHORT |
                                UCG_BUILTIN_OP_STEP_FLAG_WRITE_REMOTE_ADDR,
              ucg_builtin_step_am_short_rkey)
    case_send(req, step, phase, UCG_BUILTIN_OP_STEP_FLAG_SEND_AM_BCOPY |
                                UCG_BUILTIN_OP_STEP_FLAG_WRITE_REMOTE_ADDR,
              ucg_builtin_step_am_bcopy_one) /* bcopy_one == bcopy_rkey */
    case_send(req, step, phase, UCG_BUILTIN_OP_STEP_FLAG_SEND_AM_ZCOPY |
                                UCG_BUILTIN_OP_STEP_FLAG_WRITE_REMOTE_ADDR,
              ucg_builtin_step_am_zcopy_rkey)

    /* Multi-send operations (using iter_ep and iter_offset for context) */
    case_send(req, step, phase, UCG_BUILTIN_OP_STEP_FLAG_FRAGMENTED,
              ucg_builtin_step_dummy_send)
    case_send(req, step, phase, UCG_BUILTIN_OP_STEP_FLAG_FRAGMENTED |
                                UCG_BUILTIN_OP_STEP_FLAG_SEND_AM_SHORT,
              ucg_builtin_step_am_short_max)
    case_send(req, step, phase, UCG_BUILTIN_OP_STEP_FLAG_FRAGMENTED |
                                UCG_BUILTIN_OP_STEP_FLAG_SEND_AM_BCOPY,
              ucg_builtin_step_am_bcopy_max)
    case_send(req, step, phase, UCG_BUILTIN_OP_STEP_FLAG_FRAGMENTED |
                                UCG_BUILTIN_OP_STEP_FLAG_SEND_AM_ZCOPY,
              ucg_builtin_step_am_zcopy_max)

    default:
        ucs_error("Invalid collective operation step: %u", step->flags);
        status = UCS_ERR_INVALID_PARAM;
        goto step_execute_error;
    }

    /* Initialize the users' request object, if applicable */
    return ucg_builtin_step_check_pending(slot, step, header);

    /************************** Error flows ***********************************/
step_execute_error:
    if (status == UCS_ERR_NO_RESOURCE) {
        /* Special case: send incomplete - enqueue for resend upon progress */
        if (step->flags & UCG_BUILTIN_OP_STEP_FLAG_PIPELINED) {
            step->fragment_pending[step->iter_offset / step->fragment_length] =
                    UCG_BUILTIN_FRAG_PENDING;
            step->iter_offset = UCG_BUILTIN_OFFSET_PIPELINE_PENDING;
        }

        /* Set the collective operation ID */
        step->am_header.msg.local_id = header.msg.local_id;

        /* Add this request to the resend-queue */
        ucg_builtin_req_enqueue_resend(req->op->gctx, req);

        return UCS_INPROGRESS;
    }

    /* Generic error - reset the collective and mark the request as completed */
    ucg_builtin_comp_last_step_cb(req, status);
    return status;
}
