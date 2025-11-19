#include "tea_fetch.h"
#include "dependency_chain_cache.h"
#include "on_off_path_cache.h"
#include "globals/global_vars.h"
#include <stdlib.h>
#include <string.h>

// 전역 변수 정의
TEA_Context** tea_contexts = NULL;

static void tea_reset_fetch_queue(TEA_Fetch_Queue* fq) {
    fq->head = fq->tail = fq->count = 0;
    if (fq->name == NULL) {
        fq->name = strdup("TEA_Fetch_Queue");
    }
}

static inline int tea_fetch_queue_space(const TEA_Fetch_Queue* fq) {
    return TEA_FETCH_QUEUE_SIZE - fq->count;
}

static void tea_transition_state(TEA_Context* ctx, TEA_Thread_State next_state) {
    if (ctx->state == next_state) {
        return;
    }
    ctx->state = next_state;
    if (next_state == TEA_STATE_ACTIVE) {
        ctx->activated_cycle = cycle_count;
        ctx->shadow_rat_needs_sync = TRUE;
        ctx->rat_sync_ready_cycle = cycle_count + tea_rat_sync_latency_cycles();
    }
    if (next_state == TEA_STATE_IDLE) {
        ctx->trigger_pc = 0;
        ctx->shadow_rat_needs_sync = FALSE;
        ctx->rat_sync_ready_cycle = 0;
    }
}

static TEA_Context* tea_get_or_init_context(uns proc_id) {
    ASSERT(proc_id < NUM_CORES, "proc_id out of bounds\n");
    if (!tea_contexts) {
        tea_contexts = (TEA_Context**)calloc(NUM_CORES, sizeof(TEA_Context*));
        ASSERT(0, tea_contexts);
    }
    if (!tea_contexts[proc_id]) {
        tea_contexts[proc_id] = (TEA_Context*)calloc(1, sizeof(TEA_Context));
        ASSERT(proc_id, tea_contexts[proc_id]);
        tea_reset_fetch_queue(&tea_contexts[proc_id]->fetch_queue);
        tea_contexts[proc_id]->state = tea_is_enabled() ? TEA_STATE_IDLE : TEA_STATE_DISABLED;
        tea_contexts[proc_id]->shadow_rat_needs_sync = FALSE;
    }
    return tea_contexts[proc_id];
}

static Flag tea_enqueue_block_entry(uns proc_id, TEA_Context* tctx, Dependency_Chain_Cache_Entry* block_entry) {
    if (!block_entry || !block_entry->is_valid || block_entry->chain_length == 0) {
        return FALSE;
    }

    TEA_Fetch_Queue* fq = &tctx->fetch_queue;
    int space = tea_fetch_queue_space(fq);
    if (space == 0) {
        return FALSE;
    }

    Flag enqueued = FALSE;
    for (int ii = 0; ii < block_entry->chain_length && space > 0; ++ii) {
        Op* dst_op = &fq->entries[fq->tail];
        *dst_op = block_entry->chain[ii];
        dst_op->chain_bit = TRUE;  // TEA 전용 uop임을 명시

        fq->tail = (fq->tail + 1) % TEA_FETCH_QUEUE_SIZE;
        fq->count++;
        space--;
        enqueued = TRUE;
        tctx->ops_enqueued++;
    }

    if (enqueued) {
        tctx->last_fetch_pc = block_entry->h2p_branch_pc;
        tctx->blocks_fetched++;
        tctx->last_progress_cycle = cycle_count;
        if (tctx->state == TEA_STATE_IDLE || tctx->state == TEA_STATE_DRAINING) {
            tctx->trigger_pc = block_entry->h2p_branch_pc;
            tea_transition_state(tctx, TEA_STATE_ACTIVE);
        }
    }
    return enqueued;
}

/**
 * @brief TEA 모듈 초기화 함수. 시뮬레이션 시작 시 각 코어별로 호출됩니다.
 * @param proc_id 초기화할 코어 ID
 */
void tea_init(uns proc_id) {
    ASSERT(proc_id < NUM_CORES, "proc_id out of bounds\n");
    TEA_Context* ctx = tea_get_or_init_context(proc_id);
    ctx->state = tea_is_enabled() ? TEA_STATE_IDLE : TEA_STATE_DISABLED;
    ctx->shadow_rat_needs_sync = FALSE;
    ctx->blocks_fetched = 0;
    ctx->ops_enqueued = 0;
    ctx->activated_cycle = 0;
    ctx->last_progress_cycle = 0;
    ctx->trigger_pc = 0;
    ctx->last_fetch_pc = 0;
    ctx->rat_sync_ready_cycle = 0;
    tea_reset_fetch_queue(&ctx->fetch_queue);
}

/**
 * @brief TEA Fetch 스테이지 함수. 각 사이클마다 호출되어 TEA 스레드의 fetch 동작을 수행합니다.
 * @param proc_id 대상 코어 ID
 * @param current_fetch_addr 현재 메인 스레드의 fetch 대상 주소 (브랜치 예측기로부터 공유)
 */
void tea_fetch_stage(uns proc_id, Addr current_fetch_addr) {
    ASSERT(proc_id < NUM_CORES, "proc_id out of bounds\n");
    if (!tea_is_enabled()) {
        return;
    }
    TEA_Context* tctx = tea_get_or_init_context(proc_id);
    if (tctx->state == TEA_STATE_DISABLED) {
        return;
    }
    TEA_Fetch_Queue* fq = &tctx->fetch_queue;

    Dependency_Chain_Cache_Entry* block_entry = get_dependency_chain_block(proc_id, current_fetch_addr);
    Flag enqueued = tea_enqueue_block_entry(proc_id, tctx, block_entry);

    if (!enqueued && tctx->state == TEA_STATE_IDLE) {
        return;
    }

    if (tctx->state == TEA_STATE_ACTIVE && fq->count == 0 && (!block_entry || block_entry->chain_length == 0)) {
        tea_transition_state(tctx, TEA_STATE_DRAINING);
    } else if (tctx->state == TEA_STATE_DRAINING && fq->count == 0) {
        tea_transition_state(tctx, TEA_STATE_IDLE);
    }
}

Flag tea_thread_is_active(uns proc_id) {
    ASSERT(proc_id < NUM_CORES, "proc_id out of bounds\n");
    if (!tea_contexts || !tea_contexts[proc_id]) {
        return FALSE;
    }
    TEA_Context* ctx = tea_contexts[proc_id];
    return ctx->state == TEA_STATE_ACTIVE || ctx->state == TEA_STATE_DRAINING;
}

Flag tea_thread_needs_rat_sync(uns proc_id) {
    ASSERT(proc_id < NUM_CORES, "proc_id out of bounds\n");
    if (!tea_contexts || !tea_contexts[proc_id]) {
        return FALSE;
    }
    return tea_contexts[proc_id]->shadow_rat_needs_sync;
}

void tea_thread_ack_sync(uns proc_id) {
    ASSERT(proc_id < NUM_CORES, "proc_id out of bounds\n");
    if (!tea_contexts || !tea_contexts[proc_id]) {
        return;
    }
    tea_contexts[proc_id]->shadow_rat_needs_sync = FALSE;
    tea_contexts[proc_id]->rat_sync_ready_cycle = 0;
}
