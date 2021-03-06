/**
 * \file
 * \brief SDMA (System Direct Memory Access) RPC bindings.
 */

/*
 * Copyright (c) 2016, ETH Zurich.
 * All rights reserved.
 *
 * This file is distributed under the terms in the attached LICENSE file.
 * If you do not find this file, copies can be found by writing to:
 * ETH Zurich D-INFK, Universitaetsstrasse 6, CH-8092 Zurich. Attn: Systems Group.
 */

#include <aos/aos.h>
#include <aos/aos_rpc.h>
#include <omap_timer/timer.h>
#include <sdma/sdma_rpc.h>

errval_t sdma_rpc_send_and_receive(uintptr_t* args, void* send_handler,
        void* rcv_handler)
{
    struct sdma_rpc* rpc = (struct sdma_rpc*) args[0];
    // 1. Set send handler.
    CHECK("sdma_rpc_send_and_receive: lmp_chan_register_send",
            lmp_chan_register_send(&rpc->lc, rpc->ws,
                    MKCLOSURE(send_handler, args)));

    // 3. Set receive handler.
    CHECK("sdma_rpc_send_and_receive: lmp_chan_register_recv",
            lmp_chan_register_recv(&rpc->lc, rpc->ws,
                    MKCLOSURE(rcv_handler, args)));

    // 4. Block until channel is ready to send.
    CHECK("sdma_rpc_send_and_receive: event_dispatch send",
            event_dispatch(rpc->ws));

    // 5. Block until channel is ready to receive.
    CHECK("sdma_rpc_send_and_receive: event_dispatch receive",
            event_dispatch(rpc->ws));

    return SYS_ERR_OK;
}

errval_t sdma_rpc_init(struct sdma_rpc* rpc, struct waitset* ws)
{
    rpc->ws = ws;

    // CHECK("sdma_rpc_init: aos_rpc_get_sdma_ep_cap",
    //         aos_rpc_get_sdma_ep_cap(get_init_rpc(), &cap_sdma_ep));

    // 2. Create local channel using SDMA driver as remote endpoint.
    CHECK("sdma_rpc_init: lmp_chan_accept",
            lmp_chan_accept(&rpc->lc, 100 * DEFAULT_LMP_BUF_WORDS, cap_sdma_ep));

    // 3. Marshal args.
    uintptr_t args = (uintptr_t) rpc;

    // 4. Allocate recv slot.
    CHECK("sdma_rpc_init: lmp_chan_alloc_recv_slot",
            lmp_chan_alloc_recv_slot(&rpc->lc));

    // 5. Send handshake request to SDMA driver and wait for ACK.
    debug_printf("BEFORE sdma_rpc_init: send and receive, rpc is at %p\n", rpc);
    CHECK("sdma_rpc_init: sdma_rpc_send_and_receive",
            sdma_rpc_send_and_receive(&args, sdma_rpc_handshake_send_handler,
                    sdma_rpc_handshake_recv_handler));
    debug_printf("AFTER sdma_rpc_init: send and receive\n");

    // By now we've successfully established the underlying LMP channel for RPC.
    rpc->request_pending = false;

    omap_timer_init();
    omap_timer_ctrl(true);

    return SYS_ERR_OK;
}

errval_t sdma_rpc_handshake_send_handler(void* void_args)
{
    uintptr_t* args = (uintptr_t*) void_args;

    struct sdma_rpc *rpc = (struct sdma_rpc*) args[0];
    errval_t err;
    size_t retries = 0;
    debug_printf("BEFORE handshake send\n");
    do {
        err = lmp_chan_send1(&rpc->lc, LMP_FLAG_SYNC, rpc->lc.local_cap,
                SDMA_RPC_HANDSHAKE);
        ++retries;
    } while (err_is_fail(err) && retries < 1);
    debug_printf("AFTER handshake send\n");

    if (err_is_fail(err)) {
        DEBUG_ERR(err, "Handshake retry limit exceeded");
    }

    return err;
}

errval_t sdma_rpc_handshake_recv_handler(void* void_args)
{
    uintptr_t* args = (uintptr_t*) void_args;

    struct sdma_rpc* rpc = (struct sdma_rpc*) args[0];
    struct lmp_recv_msg msg = LMP_RECV_MSG_INIT;;
    struct capref cap;

    errval_t err = lmp_chan_recv(&rpc->lc, &msg, &cap);
    if (err_is_fail(err) && lmp_err_is_transient(err)) {
        // Reregister.
        lmp_chan_register_recv(&rpc->lc, rpc->ws,
                MKCLOSURE((void*) sdma_rpc_handshake_recv_handler, args));
    }

    // We should have a response code.
    assert(msg.buf.msglen == 1);
    assert(SDMA_RPC_OK == msg.words[0]);

    rpc->lc.remote_cap = cap;

    // Return error from remote ops.
    if (err_is_fail(err)) {
        DEBUG_ERR(err, "handshake receive handler");
    }
    return err;
}

errval_t sdma_rpc_memcpy(struct sdma_rpc* rpc,
        struct capref dst,
        size_t dst_offset,
        struct capref src,
        size_t src_offset,
        size_t len)
{
	if (rpc->request_pending) {
		return SDMA_ERR_REQUEST_IN_PROGRESS;
	}

    uintptr_t args[5];
    args[0] = (uintptr_t) rpc;
    args[1] = (uintptr_t) SDMA_RPC_MEMCPY_SRC;
    args[2] = (uintptr_t) &src;
    args[3] = (uintptr_t) src_offset;
    args[4] = (uintptr_t) len;

    // debug_printf("MEMCPY: IPC timer reset to 0\n");
    // rpc->ipc_time = 0;
    CHECK("sdma_rpc_memcpy: sdma_rpc_send_and_receive (src, len)",
            sdma_rpc_send_and_receive(args, sdma_rpc_memcpy_send_handler,
                    sdma_rpc_response_recv_handler));

    args[1] = (uintptr_t) SDMA_RPC_MEMCPY_DST;
    args[2] = (uintptr_t) &dst;
    args[3] = (uintptr_t) dst_offset;

    CHECK("sdma_rpc_memcpy: sdma_rpc_send_and_receive (dst)",
            sdma_rpc_send_and_receive(args, sdma_rpc_memcpy_send_handler,
                    sdma_rpc_response_recv_handler));

    rpc->request_pending = true;

    // Set another receive handler, for when the remote memcpy has been
    // completed and the server acks back to inform us.
    CHECK("sdma_rpc_memcpy: lmp_chan_register_recv",
            lmp_chan_register_recv(&rpc->lc, rpc->ws,
                    MKCLOSURE((void*) sdma_rpc_response_recv_handler, &args)));

    return SYS_ERR_OK;
}

errval_t sdma_rpc_memset(struct sdma_rpc* rpc,
        struct capref dst,
        size_t dst_offset,
        size_t len,
        uint8_t val)
{
    if (rpc->request_pending) {
        return SDMA_ERR_REQUEST_IN_PROGRESS;
    }

    uintptr_t args[5];
    args[0] = (uintptr_t) rpc;
    args[1] = (uintptr_t) &dst;
    args[2] = (uintptr_t) dst_offset;
    args[3] = (uintptr_t) len;
    args[4] = (uintptr_t) val;

    // debug_printf("MEMSET: IPC timer reset to 0\n");
    // rpc->ipc_time = 0;
    CHECK("sdma_rpc_memset: sdma_rpc_send_and_receive",
            sdma_rpc_send_and_receive(args, sdma_rpc_memset_send_handler,
                    sdma_rpc_response_recv_handler));

    rpc->request_pending = true;

    // Set another receive handler, for when the remote memset has been
    // completed and the server acks back to inform us.
    CHECK("sdma_rpc_memset: lmp_chan_register_recv",
            lmp_chan_register_recv(&rpc->lc, rpc->ws,
                    MKCLOSURE((void*) sdma_rpc_response_recv_handler, &args)));

    return SYS_ERR_OK;
}

errval_t sdma_rpc_rotate(struct sdma_rpc* rpc,
        struct capref dst,
        size_t dst_offset,
        struct capref src,
        size_t src_offset,
        size_t width,
        size_t height)
{
    if (rpc->request_pending) {
        return SDMA_ERR_REQUEST_IN_PROGRESS;
    }

    uintptr_t args[6];
    args[0] = (uintptr_t) rpc;
    args[1] = (uintptr_t) SDMA_RPC_ROTATE_SRC;
    args[2] = (uintptr_t) &src;
    args[3] = (uintptr_t) src_offset;
    args[4] = (uintptr_t) width;
    args[5] = (uintptr_t) height;

    // debug_printf("ROTATE: IPC timer reset to 0\n");
    // rpc->ipc_time = 0;
    CHECK("sdma_rpc_rotate: sdma_rpc_send_and_receive (src, width, height)",
            sdma_rpc_send_and_receive(args, sdma_rpc_rotate_send_handler,
                    sdma_rpc_response_recv_handler));

    args[1] = (uintptr_t) SDMA_RPC_ROTATE_DST;
    args[2] = (uintptr_t) &dst;
    args[3] = (uintptr_t) dst_offset;

    CHECK("sdma_rpc_rotate: sdma_rpc_send_and_receive (dst)",
            sdma_rpc_send_and_receive(args, sdma_rpc_rotate_send_handler,
                    sdma_rpc_response_recv_handler));

    rpc->request_pending = true;

    // Set another receive handler, for when the remote rotate has been
    // completed and the server acks back to inform us.
    CHECK("sdma_rpc_rotate: lmp_chan_register_recv",
            lmp_chan_register_recv(&rpc->lc, rpc->ws,
                    MKCLOSURE((void*) sdma_rpc_response_recv_handler, &args)));

    return SYS_ERR_OK;
}

errval_t sdma_rpc_memcpy_send_handler(void* void_args)
{
    uintptr_t* args = (uintptr_t*) void_args;

    struct sdma_rpc *rpc = (struct sdma_rpc*) args[0];
    size_t code = (size_t) args[1];
    struct capref* cap = (struct capref*) args[2];
    size_t offset = (size_t) args[3];
    size_t len = (size_t) args[4];
    
    errval_t err;
    size_t retries = 0;
    // uint64_t timer_start = omap_timer_read();
    do {
        err = lmp_chan_send3(&rpc->lc, LMP_FLAG_SYNC, *cap, code, offset, len);
        ++retries;
    } while (err_is_fail(err) && retries < 5);
    // uint64_t timer_end = omap_timer_read();
    // rpc->ipc_time += timer_end - timer_start;
    if (err_is_fail(err)) {
        DEBUG_ERR(err, "memcpy RPC send error");
    }

    return err;
}

errval_t sdma_rpc_memset_send_handler(void* void_args)
{
    uintptr_t* args = (uintptr_t*) void_args;

    struct sdma_rpc *rpc = (struct sdma_rpc*) args[0];
    struct capref* cap = (struct capref*) args[1];
    size_t offset = (size_t) args[2];
    size_t len = (size_t) args[3];
    uint8_t val = (uint8_t) args[4];

    errval_t err;
    size_t retries = 0;
    // uint64_t timer_start = omap_timer_read();
    do {
        err = lmp_chan_send4(&rpc->lc, LMP_FLAG_SYNC, *cap, SDMA_RPC_MEMSET,
                offset, len, val);
        ++retries;
    } while (err_is_fail(err) && retries < 5);
    // uint64_t timer_end = omap_timer_read();
    // rpc->ipc_time += timer_end - timer_start;
    if (err_is_fail(err)) {
        DEBUG_ERR(err, "memset RPC send error");
    }

    return err;
}

errval_t sdma_rpc_rotate_send_handler(void* void_args)
{
    uintptr_t* args = (uintptr_t*) void_args;

    struct sdma_rpc *rpc = (struct sdma_rpc*) args[0];
    size_t code = (size_t) args[1];
    struct capref* cap = (struct capref*) args[2];
    size_t offset = (size_t) args[3];
    size_t width = (size_t) args[4];
    size_t height = (size_t) args[5];
    
    errval_t err;
    size_t retries = 0;
    // uint64_t timer_start = omap_timer_read();
    do {
        err = lmp_chan_send4(&rpc->lc, LMP_FLAG_SYNC, *cap, code, offset, width,
                height);
        ++retries;
    } while (err_is_fail(err) && retries < 5);
    // uint64_t timer_end = omap_timer_read();
    // rpc->ipc_time += timer_end - timer_start;
    if (err_is_fail(err)) {
        DEBUG_ERR(err, "rotate RPC send error");
    }

    return err;
}

errval_t sdma_rpc_response_recv_handler(void* void_args)
{
    uintptr_t* args = (uintptr_t*) void_args;

    struct sdma_rpc* rpc = (struct sdma_rpc*) args[0];
    struct lmp_recv_msg msg = LMP_RECV_MSG_INIT;;
    struct capref cap;

    errval_t err = lmp_chan_recv(&rpc->lc, &msg, &cap);
    if (err_is_fail(err) && lmp_err_is_transient(err)) {
        // Reregister.
        lmp_chan_register_recv(&rpc->lc, rpc->ws,
                MKCLOSURE((void*) sdma_rpc_response_recv_handler, args));
    }

    // We should have a response code and an error.
    assert(msg.buf.msglen == 2);

    err = (errval_t) msg.words[1];
    if (err_is_fail(err)) {
        DEBUG_ERR(err, "general-purpose response receive handler");
    }

    return err;
}

bool sdma_rpc_check_for_response(struct sdma_rpc* rpc)
{
	// This just peeks non-blockingly.
	errval_t err = check_for_event(rpc->ws);
	return err_is_ok(err);
}

errval_t sdma_rpc_wait_for_response(struct sdma_rpc* rpc)
{
    rpc->request_pending = false;

    // Block until channel is ready to receive.
    CHECK("sdma_rpc_wait_for_response: event_dispatch receive",
            event_dispatch(rpc->ws));

    // debug_printf("Time spent on IPC: %llu\n", rpc->ipc_time);


    return SYS_ERR_OK;
}