#include "jacs_internal.h"

void jacs_fiber_yield(jacs_ctx_t *ctx) {
    ctx->curr_fn = NULL;
    ctx->curr_fiber = NULL;
}

static void jacs_fiber_activate(jacs_activation_t *act) {
    act->fiber->activation = act;
    act->fiber->ctx->curr_fn = act;
    jacs_act_restore_regs(act);
}

void jacs_fiber_call_function(jacs_fiber_t *fiber, unsigned fidx, unsigned numargs) {
    jacs_ctx_t *ctx = fiber->ctx;
    const jacs_function_desc_t *func = jacs_img_get_function(&ctx->img, fidx);

    int numregs = func->num_regs_and_args & 0xf;

    jacs_activation_t *callee =
        jd_alloc(sizeof(jacs_activation_t) + sizeof(value_t) * (numregs + func->num_locals));
    memcpy(callee->locals, ctx->registers, numargs * sizeof(value_t));
    callee->pc = func->start >> 1;
    callee->caller = fiber->activation;
    callee->fiber = fiber;
    callee->func = func;
    callee->saved_regs = 0;

    // if fiber already activated, move the activation pointer
    if (fiber->activation)
        jacs_fiber_activate(callee);
    else
        fiber->activation = callee;
}

void jacs_fiber_set_wake_time(jacs_fiber_t *fiber, unsigned time) {
    fiber->wake_time = time;
}

void jacs_fiber_sleep(jacs_fiber_t *fiber, unsigned time) {
    jacs_fiber_set_wake_time(fiber, jacs_now(fiber->ctx) + time);
    jacs_fiber_yield(fiber->ctx);
}

static void free_fiber(jacs_fiber_t *fiber) {
    jacs_jd_clear_pkt_kind(fiber);
    jacs_ctx_t *ctx = fiber->ctx;
    if (ctx->fibers == fiber) {
        ctx->fibers = fiber->next;
    } else {
        jacs_fiber_t *f = ctx->fibers;
        while (f && f->next != fiber)
            f = f->next;
        if (!f)
            oops();
        f->next = fiber->next;
    }
    jd_free(fiber);
}

void jacs_fiber_return_from_call(jacs_activation_t *act) {
    if (act->caller) {
        jacs_fiber_activate(act->caller);
        jd_free(act);
    } else {
        jacs_fiber_t *fiber = act->fiber;
        if (fiber->pending) {
            DMESG("re-run fiber %d ", fiber->bottom_function_idx);
            fiber->pending = 0;
            act->pc = act->func->start >> 1;
        } else {
            DMESG("free fiber %d", fiber->bottom_function_idx);
            jacs_fiber_yield(fiber->ctx);
            free_fiber(fiber);
        }
    }
}

void jacs_fiber_free_all_fibers(jacs_ctx_t *ctx) {
    jacs_fiber_t *f = ctx->fibers;
    while (f) {
        ctx->fibers = f->next;
        jacs_jd_clear_pkt_kind(f);
        jacs_activation_t *act = f->activation;
        while (act) {
            jacs_activation_t *n = act->caller;
            jd_free(act);
            act = n;
        }
        jd_free(f);
        f = ctx->fibers;
    }
}

void jacs_fiber_start(jacs_ctx_t *ctx, unsigned fidx, unsigned numargs, unsigned op) {
    jacs_fiber_t *fiber;

    if (op != JACS_OPCALL_BG)
        for (fiber = ctx->fibers; fiber; fiber = fiber->next) {
            if (fiber->bottom_function_idx == fidx) {
                if (op == JACS_OPCALL_BG_MAX1_PEND1) {
                    if (fiber->pending) {
                        ctx->registers[0] = jacs_value_from_int(3);
                        // DMESG("fiber already pending %d", fidx);
                    } else {
                        fiber->pending = 1;
                        // DMESG("pend fiber %d", fidx);
                        ctx->registers[0] = jacs_value_from_int(2);
                    }
                } else {
                    ctx->registers[0] = jacs_zero;
                }
                return;
            }
        }

    DMESG("start fiber %d", fidx);

    fiber = jd_alloc(sizeof(*fiber));
    fiber->ctx = ctx;
    fiber->bottom_function_idx = fidx;

    jacs_fiber_call_function(fiber, fidx, numargs);

    fiber->next = ctx->fibers;
    ctx->fibers = fiber;

    jacs_fiber_set_wake_time(fiber, jacs_now(ctx));

    ctx->registers[0] = jacs_one;
}

void jacs_fiber_run(jacs_fiber_t *fiber) {
    jacs_ctx_t *ctx = fiber->ctx;
    if (ctx->error_code)
        return;

    jacs_fiber_sync_now(ctx);

    if (!jacs_jd_should_run(fiber))
        return;

    jacs_jd_clear_pkt_kind(fiber);
    fiber->role_idx = JACS_NO_ROLE;
    jacs_fiber_set_wake_time(fiber, 0);

    ctx->a = ctx->b = ctx->c = ctx->d = 0;

    ctx->curr_fiber = fiber;
    jacs_fiber_activate(fiber->activation);

    unsigned maxsteps = JACS_MAX_STEPS;
    while (ctx->curr_fn && --maxsteps)
        jacs_act_step(ctx->curr_fn);

    if (maxsteps == 0)
        jacs_panic(ctx, JACS_PANIC_TIMEOUT);
}

void jacs_panic(jacs_ctx_t *ctx, unsigned code) {
    if (!code)
        code = JACS_PANIC_REBOOT;
    if (!ctx->error_code) {
        ctx->error_pc = ctx->curr_fn ? ctx->curr_fn->pc : 0;
        if (code == JACS_PANIC_REBOOT) {
            DMESG("RESTART requested");
        } else {
            DMESG("PANIC %d at pc=%d", code, ctx->error_pc);
        }
        ctx->error_code = code;

        if (code != JACS_PANIC_REBOOT)
            for (jacs_activation_t *fn = ctx->curr_fn; fn; fn = fn->caller) {
                int idx = fn->func - jacs_img_get_function(&ctx->img, 0);
                DMESG("  pc=%d @ fun%d", (int)(fn->pc - (fn->func->start >> 1)), idx);
            }
    }
    jacs_fiber_yield(ctx);
}

value_t jacs_runtime_failure(jacs_ctx_t *ctx) {
    jacs_panic(ctx, JACS_PANIC_RUNTIME_FAILURE);
    return jacs_nan;
}

void jacs_fiber_sync_now(jacs_ctx_t *ctx) {
    jd_refresh_now();
    ctx->_now = now_ms;
}

static int jacs_fiber_wake_some(jacs_ctx_t *ctx) {
    if (ctx->error_code)
        return 0;
    uint32_t now = jacs_now(ctx);
    for (jacs_fiber_t *fiber = ctx->fibers; fiber; fiber = fiber->next) {
        if (fiber->wake_time && fiber->wake_time <= now) {
            jacs_jd_reset_packet(ctx);
            jacs_fiber_run(fiber);
            // we can't continue with the fiber loop - the fiber might be gone by now
            return 1;
        }
    }
    return 0;
}

void jacs_fiber_poke(jacs_ctx_t *ctx) {
    jacs_fiber_sync_now(ctx);
    while (jacs_fiber_wake_some(ctx))
        ;
}
