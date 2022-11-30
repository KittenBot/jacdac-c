#include "jacs_internal.h"

#define LOGV JD_NOLOG
// #define LOGV(msg, ...) DMESG("JDI: " msg, ##__VA_ARGS__)

#define RESUME_USER_CODE 1
#define KEEP_WAITING 0

static bool handle_logmsg(jacs_fiber_t *fiber, bool print);

void jacs_jd_get_register(jacs_ctx_t *ctx, unsigned role_idx, unsigned code, unsigned timeout,
                          unsigned arg) {
    if (ctx->error_code)
        return;
    jd_device_service_t *serv = ctx->roles[role_idx]->service;
    if (serv != NULL) {
        jacs_regcache_entry_t *cached = jacs_regcache_lookup(&ctx->regcache, role_idx, code, arg);
        if (cached != NULL) {
            if (!timeout || timeout > JACS_MAX_REG_VALIDITY)
                timeout = JACS_MAX_REG_VALIDITY;
            // DMESG("cached cmd=%x %d < %d", code, cached->last_refresh_time + timeout,
            //      jacs_now(ctx));
            if (cached->last_refresh_time + timeout < jacs_now(ctx)) {
                jacs_regcache_free(&ctx->regcache, cached);
            } else {
                cached = jacs_regcache_mark_used(&ctx->regcache, cached);
                memset(&ctx->packet, 0, sizeof(ctx->packet));
                ctx->packet.service_command = cached->service_command;
                ctx->packet.service_size = cached->resp_size;
                ctx->packet.service_index = serv->service_index;
                ctx->packet.device_identifier = jd_service_parent(serv)->device_identifier;
                memcpy(ctx->packet.data, jacs_regcache_data(cached), cached->resp_size);
                // DMESG("cached reg %x sz=%d cmd=%d", code, cached->resp_size,
                // cached->service_command);
                return;
            }
        }
    }

    jacs_fiber_t *fib = ctx->curr_fiber;
    JD_ASSERT(fib != NULL);
    fib->role_idx = role_idx;
    fib->service_command = code;
    fib->pkt_kind = JACS_PKT_KIND_REG_GET;
    fib->pkt_data.reg_get.string_idx = arg;
    fib->pkt_data.reg_get.resend_timeout = 20;

    // DMESG("wait reg %x", code);
    jacs_fiber_sleep(fib, 0);
}

void jacs_jd_clear_pkt_kind(jacs_fiber_t *fib) {
    switch (fib->pkt_kind) {
    case JACS_PKT_KIND_SEND_PKT:
        jacs_free(fib->ctx, fib->pkt_data.send_pkt.data);
        break;
    default:
        break;
    }
    fib->pkt_kind = JACS_PKT_KIND_NONE;
}

void jacs_jd_send_cmd(jacs_ctx_t *ctx, unsigned role_idx, unsigned code) {
    if (ctx->error_code)
        return;
    if (JD_IS_SET(code)) {
        jacs_regcache_entry_t *cached = jacs_regcache_lookup(
            &ctx->regcache, role_idx, (code & ~JD_CMD_SET_REGISTER) | JD_CMD_GET_REGISTER, 0);
        if (cached != NULL)
            jacs_regcache_free(&ctx->regcache, cached);
    }

    const jacs_role_desc_t *role = jacs_img_get_role(&ctx->img, role_idx);
    jacs_fiber_t *fib = ctx->curr_fiber;
    JD_ASSERT(fib != NULL);

    if (role->service_class == JD_SERVICE_CLASS_DEVICE_SCRIPT_CONDITION) {
        jacs_fiber_sleep(fib, 0);
        LOGV("wake condition");
        jacs_jd_wake_role(ctx, role_idx);
        return;
    }

    fib->role_idx = role_idx;
    fib->service_command = code;

    unsigned sz = ctx->packet.service_size;
    fib->pkt_kind = JACS_PKT_KIND_SEND_PKT;
    fib->pkt_data.send_pkt.data = jacs_try_alloc(ctx, sz);
    if (fib->pkt_data.send_pkt.data != NULL) {
        fib->pkt_data.send_pkt.size = sz;
        memcpy(fib->pkt_data.send_pkt.data, ctx->packet.data, sz);
    }
    jacs_fiber_sleep(fib, 0);
}

void jacs_jd_send_logmsg(jacs_ctx_t *ctx, unsigned string_idx, unsigned localsidx,
                         unsigned num_args) {
    if (ctx->error_code)
        return;

    jacs_fiber_t *fib = ctx->curr_fiber;
    JD_ASSERT(fib != NULL);

    fib->role_idx = JACS_NO_ROLE;

    fib->pkt_kind = JACS_PKT_KIND_LOGMSG;
    fib->service_command = ctx->log_counter & 0xffff;
    ctx->log_counter++;
    fib->pkt_data.logmsg.string_idx = string_idx;
    fib->pkt_data.logmsg.num_args = num_args;
    fib->pkt_data.logmsg.localsidx = localsidx;

    if (handle_logmsg(fib, true) == RESUME_USER_CODE) {
        jacs_jd_clear_pkt_kind(fib);
    }
}

static void jacs_jd_set_packet(jacs_ctx_t *ctx, unsigned role_idx, unsigned service_command,
                               const void *payload, unsigned sz) {
    jd_packet_t *pkt = &ctx->packet;
    pkt->_size = (sz + 4 + 3) & ~3;
    pkt->flags = JD_FRAME_FLAG_COMMAND;
    jd_device_t *dev = jd_service_parent(ctx->roles[role_idx]->service);
    pkt->device_identifier = dev->device_identifier;
    pkt->service_size = sz;
    pkt->service_index = ctx->roles[role_idx]->service->service_index;
    pkt->service_command = service_command;
    if (payload)
        memcpy(pkt->data, payload, sz);
}

void jacs_jd_wake_role(jacs_ctx_t *ctx, unsigned role_idx) {
    for (jacs_fiber_t *fiber = ctx->fibers; fiber; fiber = fiber->next) {
        if (fiber->role_idx == role_idx) {
            fiber->role_wkp = 1;
        }
    }

    int runsome = 1;
    while (runsome) {
        runsome = 0;
        for (jacs_fiber_t *fiber = ctx->fibers; fiber; fiber = fiber->next) {
            if (fiber->role_wkp) {
                fiber->role_wkp = 0;
                jacs_fiber_run(fiber);
                runsome = 1;
                break; // can't go to next - fiber might be gone
            }
        }
    }
}

static int jacs_jd_reg_arg_length(jacs_ctx_t *ctx, unsigned command_arg) {
    JD_ASSERT(command_arg != 0);
    jd_packet_t *pkt = &ctx->packet;
    int slen = jacs_img_get_string_len(&ctx->img, command_arg);
    if (pkt->service_size >= slen + 1 && pkt->data[slen] == 0 &&
        memcmp(jacs_img_get_string_ptr(&ctx->img, command_arg), pkt->data, slen) == 0) {
        return slen + 1;
    } else {
        return 0;
    }
}

static jacs_regcache_entry_t *jacs_jd_update_regcache(jacs_ctx_t *ctx, unsigned role_idx,
                                                      unsigned command_arg) {
    jd_packet_t *pkt = &ctx->packet;

    int resp_size = pkt->service_size;
    uint8_t *dp = pkt->data;
    if (command_arg) {
        int slen = jacs_jd_reg_arg_length(ctx, command_arg);
        if (!slen)
            return NULL;
        dp += slen;
        resp_size -= slen;
    }

    jacs_regcache_entry_t *q =
        jacs_regcache_lookup(&ctx->regcache, role_idx, pkt->service_command, command_arg);
    if (q && q->resp_size != resp_size) {
        jacs_regcache_free(&ctx->regcache, q);
        q = NULL;
    }

    if (!q) {
        q = jacs_regcache_alloc(&ctx->regcache, role_idx, pkt->service_command, resp_size);
        q->argument = command_arg;
    }

    memcpy(jacs_regcache_data(q), dp, resp_size);
    q->last_refresh_time = jacs_now(ctx);

    return q;
}

static bool jacs_jd_pkt_matches_role(jacs_ctx_t *ctx, unsigned role_idx) {
    jd_packet_t *pkt = &ctx->packet;
    jd_device_service_t *serv = ctx->roles[role_idx]->service;
    return serv &&
           ((pkt->service_index == 0 && pkt->service_command == 0) ||
            serv->service_index == pkt->service_index) &&
           jd_service_parent(serv)->device_identifier == pkt->device_identifier;
}

static bool retry_soon(jacs_fiber_t *fiber) {
    jacs_fiber_sleep(fiber, 3);
    return KEEP_WAITING;
}

static bool role_missing(jacs_fiber_t *fiber) {
    jacs_ctx_t *ctx = fiber->ctx;
    jd_device_service_t *serv = ctx->roles[fiber->role_idx]->service;

    if (serv == NULL) {
        // role unbound, keep waiting, no timeout
        jacs_fiber_set_wake_time(fiber, 0);
        return 1;
    }

    return 0;
}

static bool handle_reg_get(jacs_fiber_t *fiber) {
    if (role_missing(fiber))
        return KEEP_WAITING;

    jacs_ctx_t *ctx = fiber->ctx;
    jd_packet_t *pkt = &ctx->packet;
    if (jd_is_report(pkt) && pkt->service_command &&
        pkt->service_command == fiber->service_command &&
        jacs_jd_pkt_matches_role(ctx, fiber->role_idx)) {
        jacs_regcache_entry_t *q =
            jacs_jd_update_regcache(ctx, fiber->role_idx, fiber->pkt_data.reg_get.string_idx);
        if (q) {
            q = jacs_regcache_mark_used(&ctx->regcache, q);
            return RESUME_USER_CODE;
        }
    }

    if (jacs_now(ctx) >= fiber->wake_time) {
        int arglen = 0;
        const void *argp = NULL;
        if (fiber->pkt_data.reg_get.string_idx) {
            arglen = jacs_img_get_string_len(&ctx->img, fiber->pkt_data.reg_get.string_idx);
            argp = jacs_img_get_string_ptr(&ctx->img, fiber->pkt_data.reg_get.string_idx);
        }

        jacs_jd_set_packet(ctx, fiber->role_idx, fiber->service_command, argp, arglen);
        if (jd_send_pkt(&ctx->packet) != 0) {
            LOGV("(re)send pkt FAILED cmd=%x", fiber->service_command);
            return retry_soon(fiber);
        } else {
            LOGV("(re)send pkt cmd=%x", fiber->service_command);
            if (fiber->pkt_data.reg_get.resend_timeout < 1000)
                fiber->pkt_data.reg_get.resend_timeout *= 2;
            jacs_fiber_sleep(fiber, fiber->pkt_data.reg_get.resend_timeout);
        }
    }
    return KEEP_WAITING;
}

static bool handle_send_pkt(jacs_fiber_t *fiber) {
    if (role_missing(fiber))
        return KEEP_WAITING;

    jacs_ctx_t *ctx = fiber->ctx;
    jacs_jd_set_packet(ctx, fiber->role_idx, fiber->service_command, fiber->pkt_data.send_pkt.data,
                       fiber->pkt_data.send_pkt.size);
    if (jd_send_pkt(&ctx->packet) == 0) {
        LOGV("send pkt cmd=%x sz=%d", fiber->service_command, ctx->packet.service_size);
        // jd_log_packet(&ctx->packet);
        return RESUME_USER_CODE;
    } else {
        LOGV("send pkt FAILED cmd=%x", fiber->service_command);
        return retry_soon(fiber);
    }
}

static bool handle_logmsg(jacs_fiber_t *fiber, bool print) {
    jacs_ctx_t *ctx = fiber->ctx;

    uint16_t low_log_counter = fiber->service_command;
    bool send_now = low_log_counter == (ctx->log_counter_to_send & 0xffff);
    if (!send_now && !print)
        return retry_soon(fiber);

    jd_packet_t *pkt = &ctx->packet;
    unsigned str_idx = fiber->pkt_data.logmsg.string_idx;
    unsigned sz = jacs_strformat(jacs_img_get_string_ptr(&ctx->img, str_idx),
                                 jacs_img_get_string_len(&ctx->img, str_idx), (char *)pkt->data + 2,
                                 JD_SERIAL_PAYLOAD_SIZE - 2,
                                 fiber->activation->locals + fiber->pkt_data.logmsg.localsidx,
                                 fiber->pkt_data.logmsg.num_args, 0);
    pkt->data[0] = low_log_counter & 0xff;     // log-counter
    pkt->data[1] = 0;                          // flags
    pkt->data[JD_SERIAL_PAYLOAD_SIZE - 1] = 0; // make sure to 0-terminate
    pkt->service_size = sz + 2;
    pkt->service_command = JD_DEVICE_SCRIPT_MANAGER_CMD_LOG_MESSAGE;
    pkt->service_index = ctx->cfg.mgr_service_idx;
    pkt->device_identifier = jd_device_id();
    pkt->_size = (pkt->service_size + 4 + 3) & ~3;
    pkt->flags = 0;

    if (print)
        DMESG("JSCR: %s", pkt->data + 2);

    if (!(ctx->flags & JACS_CTX_LOGGING_ENABLED))
        return RESUME_USER_CODE;

    if (send_now) {
        if (jd_send_pkt(&ctx->packet) == 0) {
            // LOGV("log sent");
            ctx->log_counter_to_send++;
            return RESUME_USER_CODE;
        } else {
            LOGV("send log FAILED");
            return retry_soon(fiber);
        }
    } else {
        return retry_soon(fiber);
    }
}

bool jacs_jd_should_run(jacs_fiber_t *fiber) {
    if (fiber->pkt_kind == JACS_PKT_KIND_NONE)
        return RESUME_USER_CODE;

    switch (fiber->pkt_kind) {
    case JACS_PKT_KIND_REG_GET:
        return handle_reg_get(fiber);

    case JACS_PKT_KIND_SEND_PKT:
        return handle_send_pkt(fiber);

    case JACS_PKT_KIND_LOGMSG:
        return handle_logmsg(fiber, false);

    default:
        JD_PANIC();
    }
}

static void jacs_jd_update_all_regcache(jacs_ctx_t *ctx, unsigned role_idx) {
    jacs_regcache_entry_t *q = NULL;
    jd_packet_t *pkt = &ctx->packet;

    if (jd_is_command(pkt))
        return;

    if (jd_is_event(pkt) && (pkt->service_command & JD_CMD_EVENT_CODE_MASK) == JD_EV_CHANGE) {
        jacs_regcache_age(&ctx->regcache, role_idx, jacs_now(ctx) - 10000);
        return;
    }

    for (;;) {
        q = jacs_regcache_next(&ctx->regcache, role_idx, pkt->service_command, q);
        if (!q)
            break;
        if (jacs_jd_update_regcache(ctx, q->role_idx, q->argument))
            break; // we only allow for one update
    }
}

static const char *jacs_jd_role_name(jacs_ctx_t *ctx, unsigned idx) {
    const jacs_role_desc_t *role = jacs_img_get_role(&ctx->img, idx);
    return jacs_img_get_string_ptr(&ctx->img, role->name_idx);
}

void jacs_jd_process_pkt(jacs_ctx_t *ctx, jd_device_service_t *serv, jd_packet_t *pkt) {
    if (ctx->error_code)
        return;

    memcpy(&ctx->packet, pkt, pkt->service_size + 16);
    pkt = &ctx->packet;

    unsigned numroles = jacs_img_num_roles(&ctx->img);

    // DMESG("pkt %d %x / %d", pkt->service_index, pkt->service_command, pkt->service_size);
    // jd_log_packet(&ctx->packet);

    for (unsigned idx = 0; idx < numroles; ++idx) {
        if (jacs_jd_pkt_matches_role(ctx, idx)) {
#if 0
            DMESG("wake pkt s=%d %x / %d r=%s", pkt->service_index, pkt->service_command,
                  pkt->service_size, ctx->roles[idx]->name);
#endif
            jacs_fiber_sync_now(ctx);
            jacs_jd_update_all_regcache(ctx, idx);
            jacs_jd_wake_role(ctx, idx);
        }
    }

    jacs_fiber_poke(ctx);
}

void jacs_jd_role_changed(jacs_ctx_t *ctx, jd_role_t *role) {
    if (ctx->flags & JACS_CTX_FREEING_ROLES)
        return;

    if (jacs_trace_enabled(ctx)) {
        unsigned sz = rolemgr_serialized_role_size(role);
        void *data = rolemgr_serialize_role(role);
        jacs_trace(ctx, JACS_TRACE_EV_ROLE_CHANGED, data, sz);
    }

    unsigned numroles = jacs_img_num_roles(&ctx->img);
    for (unsigned idx = 0; idx < numroles; ++idx) {
        if (ctx->roles[idx] == role) {
            jacs_regcache_free_role(&ctx->regcache, idx);
            jacs_jd_reset_packet(ctx);
            jacs_jd_wake_role(ctx, idx);
            break;
        }
    }
    jacs_fiber_poke(ctx);
}

void jacs_jd_reset_packet(jacs_ctx_t *ctx) {
    memset(&ctx->packet, 0xff, 32);
}

void jacs_jd_init_roles(jacs_ctx_t *ctx) {
    jacs_jd_free_roles(ctx); // free any previous roles
    unsigned numroles = jacs_img_num_roles(&ctx->img);
    for (unsigned idx = 0; idx < numroles; ++idx) {
        const jacs_role_desc_t *role = jacs_img_get_role(&ctx->img, idx);
        ctx->roles[idx] = jd_role_alloc(jacs_jd_role_name(ctx, idx), role->service_class);
        if (role->service_class == JD_SERVICE_CLASS_DEVICE_SCRIPT_CONDITION)
            ctx->roles[idx]->hidden = 1;
    }
    jd_role_force_autobind();
}

void jacs_jd_free_roles(jacs_ctx_t *ctx) {
    ctx->flags |= JACS_CTX_FREEING_ROLES;
    jd_role_free_all();
    ctx->flags &= ~JACS_CTX_FREEING_ROLES;
}

void jacs_set_logging(jacs_ctx_t *ctx, uint8_t logging) {
    if (logging)
        ctx->flags |= JACS_CTX_LOGGING_ENABLED;
    else {
        ctx->flags &= ~JACS_CTX_LOGGING_ENABLED;
        ctx->log_counter_to_send = ctx->log_counter;
    }
}

uint32_t jacs_global_flags;
void jacs_set_global_flags(uint32_t global_flags) {
    jacs_global_flags |= global_flags;
}
void jacs_reset_global_flags(uint32_t global_flags) {
    jacs_global_flags &= ~global_flags;
}
uint32_t jacs_get_global_flags(void) {
    return jacs_global_flags;
}
