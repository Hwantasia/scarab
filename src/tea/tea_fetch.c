#include "tea_fetch.h"
#include "tea_op_pool.h"
#include "tea_backend.h"
#include "tea_decoupled_frontend.h"
#include "dependency_chain_cache.h"
#include "on_off_path_cache.h"
#include "core.param.h"          /* DEBUG_TEA_FETCH_LOG */
#include "globals/assert.h"
#include "globals/global_vars.h"
#include "statistics.h"
#include "ftq_search.h"
#include "log/tea_fetch_log.h"
#include <stdlib.h>
#include <string.h>

// 전역 변수 정의
TEA_Context** tea_contexts = NULL;
static Flag tea_fetch_log_inited = FALSE;

void tea_reset_fetch_queue(TEA_Fetch_Queue* fq) {
    // Free any existing ops in the queue
    while (fq->count > 0) {
        Op* op = fq->entries[fq->head];
        if (op) {
            tea_free_op(op);
        }
        fq->head = (fq->head + 1) % TEA_FETCH_QUEUE_SIZE;
        fq->count--;
    }
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
        ctx->terminated_by_miss = FALSE;
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
        tea_contexts[proc_id]->terminated_by_miss = FALSE;
    }
    return tea_contexts[proc_id];
}

/**
 * @brief Shadow FTQ에서 Op를 가져와 Block Cache의 dependency_mask로 필터링하여 TEA Fetch Queue에 enqueue
 * 
 * 논문 Section IV-D: "The TEA thread is initiated on a hit in the Block Cache.
 * The uops read out are rotated and sent directly to the shadow Rename stage."
 * 
 * @param proc_id Core ID
 * @param tctx TEA Context
 * @return 하나라도 enqueue되었으면 TRUE
 */
static Flag tea_enqueue_from_shadow_ftq(uns proc_id, TEA_Context* tctx) {
    if (tctx->terminated_by_miss) {
        return FALSE;
    }

    TEA_FT* tea_ft = tea_shadow_ftq_peek(proc_id);
    if (!tea_ft || tea_ft->op_count == 0) {
        return FALSE;
    }
    
    TEA_Fetch_Queue* fq = &tctx->fetch_queue;
    int space = tea_fetch_queue_space(fq);
    Flag enqueued = FALSE;

    /* ============================================================
     * Block Cache lookup must be done at Basic Block start PC.
     * - Block Cache is tagged by block_start_pc (dependency_chain_cache.c)
     * - Shadow FTQ delivers a sequential stream that may include non-dependent uops
     *   between dependent uops inside the same block.
     * - Therefore, once we hit a block, we must keep using that block_entry until
     *   we reach a CF terminator (new block starts at next op).
     * ============================================================ */

    Dependency_Chain_Cache_Entry* block_entry = NULL;
    Flag block_hit = FALSE;
    Flag empty_hit = FALSE;
    Addr current_block_start_pc = 0;
    uns pos_in_block = 0;

    for (uns i = 0; i < tea_ft->op_count && space > 0; i++) {
        /* Detect block boundary using original cf_type stream (survives ops[i]==NULL) */
        Flag start_new_block = (i == 0) || (tea_ft->op_cf_types[i - 1] != NOT_CF);
        if (start_new_block) {
            current_block_start_pc = tea_ft->op_pcs[i];
            block_entry = get_dependency_chain_block(proc_id, current_block_start_pc);
            block_hit = (block_entry && block_entry->is_valid && block_entry->h2p_branch_pc == current_block_start_pc);
            empty_hit = (!block_hit) ? is_empty_block_tag_hit(proc_id, current_block_start_pc) : FALSE;
            pos_in_block = 0;

            if (empty_hit) {
                STAT_EVENT(proc_id, TEA_BLOCK_CACHE_EMPTY_HIT);
            }

            if (!block_hit && !empty_hit) {
                /* Count miss once per block (not once per uop). */
                STAT_EVENT(proc_id, TEA_BLOCK_CACHE_MISS);

                /* Terminate semantics (TEA paper IV-G):
                 * - If TEA is already running (or we already enqueued something in this call),
                 *   a miss ends the current TEA episode: stop fetching new uops, let in-flight drain.
                 * - If TEA is idle and nothing has been enqueued yet, just skip this block.
                 */
                if (tctx->state != TEA_STATE_IDLE || enqueued) {
                    tctx->terminated_by_miss = TRUE;
                    STAT_EVENT(proc_id, TEA_TERMINATED_BY_BLOCK_CACHE_MISS);
                    tea_shadow_ftq_recover(proc_id);  /* Drop Shadow-FTQ backlog; keep in-flight TEA uops. */
                    break;
                }
            }
        } else {
            pos_in_block++;
        }

        Op* tea_op = tea_ft->ops[i];
        if (!tea_op || !tea_op->inst_info) {
            continue;
        }

        Addr op_pc = tea_op->inst_info->addr;
        Flag in_chain = FALSE;

        if (block_hit) {
            /* Block Cache Hit: dependency_mask bit position is the original in-block order */
            if (pos_in_block < block_entry->total_ops_in_block && pos_in_block < 64) {
                in_chain = ((block_entry->dependency_mask >> pos_in_block) & 1ULL) ? TRUE : FALSE;
            } else {
                in_chain = FALSE;
            }
        } else {
            /* Empty-hit or miss: never enqueue anything.
             * - Empty-hit: tag-store says this block is known-empty => skip without terminating.
             * - Miss: no tag and no data => TEA termination is handled at block boundary above.
             */
            in_chain = FALSE;
        }

        /* [TEA FETCH LOG] Per-uop decision with current block context */
        if (DEBUG_TEA_FETCH_LOG) {
            if (!tea_fetch_log_inited) {
                init_tea_fetch_log();
                tea_fetch_log_inited = TRUE;
            }
            log_tea_block_cache_lookup(proc_id, cycle_count, op_pc,
                                       block_hit ? block_entry : NULL, i, tea_ft->op_count,
                                       in_chain);
        }

        if (!in_chain) {
            tea_free_op(tea_op);
            tea_ft->ops[i] = NULL;
            continue;
        }

        /* Mark as chain instruction */
        tea_op->chain_bit = TRUE;

        /* Enqueue to TEA Fetch Queue */
        fq->entries[fq->tail] = tea_op;
        fq->tail = (fq->tail + 1) % TEA_FETCH_QUEUE_SIZE;
        fq->count++;
        space--;
        enqueued = TRUE;
        tctx->ops_enqueued++;
        STAT_EVENT(proc_id, TEA_OP_ENQUEUED);

        /* Remove from Shadow FT to prevent double processing */
        tea_ft->ops[i] = NULL;
    }
    
    /* Consume the Shadow FT if all Ops processed */
    Flag all_consumed = TRUE;
    for (uns i = 0; i < tea_ft->op_count; i++) {
        if (tea_ft->ops[i] != NULL) {
            all_consumed = FALSE;
            break;
        }
    }
    if (all_consumed) {
        tea_shadow_ftq_consume(proc_id);
    }
    
    if (enqueued) {
        tctx->blocks_fetched++;
        tctx->last_progress_cycle = cycle_count;
        if (tctx->state == TEA_STATE_IDLE || tctx->state == TEA_STATE_DRAINING) {
            if (tea_ft->start_pc != 0) {
                tctx->trigger_pc = tea_ft->start_pc;
            }
            tea_transition_state(tctx, TEA_STATE_ACTIVE);
        }
    }
    
    return enqueued;
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
        Op* dst_op = tea_alloc_op(proc_id);
        
        // Save identity fields that must be unique to this new TEA Op
        Op temp_identity;
        temp_identity.op_num = dst_op->op_num;
        temp_identity.unique_num = dst_op->unique_num;
        temp_identity.proc_id = dst_op->proc_id;
        temp_identity.op_pool_valid = dst_op->op_pool_valid;

        // Copy content from snapshot (this overwrites everything including IDs)
        *dst_op = block_entry->chain[ii]; 

        // Restore identity fields
        dst_op->op_num = temp_identity.op_num;
        dst_op->unique_num = temp_identity.unique_num;
        dst_op->proc_id = temp_identity.proc_id;
        dst_op->op_pool_valid = temp_identity.op_pool_valid;

        // Reset runtime flags that shouldn't be inherited from the cached snapshot
        dst_op->oracle_info.recovery_sch = FALSE;
        dst_op->oracle_info.mispred = FALSE;
        dst_op->oracle_info.misfetch = FALSE;
        dst_op->oracle_info.btb_miss = FALSE;
        dst_op->oracle_info.no_target = FALSE;
        dst_op->state = OS_FETCHED;
        dst_op->done_cycle = MAX_CTR;
        
        dst_op->chain_bit = TRUE;  // TEA 전용 uop임을 명시
        
        // [TEA Early Binding] FTQ에서 Main Op 검색
        if (dst_op->table_info && dst_op->inst_info) {
            Op* main_op = find_op_in_ftq(proc_id, 
                                         dst_op->inst_info->addr, 
                                         dst_op->table_info->cf_type);
            dst_op->tea_main_op_candidate = main_op;
            
            // Debugging ASSERT
            if (main_op) {
                ASSERT(proc_id, main_op->op_pool_valid);
                ASSERT(proc_id, main_op->inst_info);
                ASSERT(proc_id, main_op->inst_info->addr == dst_op->inst_info->addr);
            }
        } else {
            dst_op->tea_main_op_candidate = NULL;
        }
        
        fq->entries[fq->tail] = dst_op;

        fq->tail = (fq->tail + 1) % TEA_FETCH_QUEUE_SIZE;
        fq->count++;
        space--;
        enqueued = TRUE;
        tctx->ops_enqueued++;
        STAT_EVENT(proc_id, TEA_OP_ENQUEUED);
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
    ctx->terminated_by_miss = FALSE;
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
    TEA_Fetch_Queue* fq = &tctx->fetch_queue;
    
    // [DEBUG] Periodic TEA Fetch state logging (every 1000 cycles)
    static Counter last_log_cycle[MAX_NUM_PROCS] = {0};
    if (DEBUG_CYCLE_START <= cycle_count && cycle_count <= DEBUG_CYCLE_STOP) {
        if (cycle_count - last_log_cycle[proc_id] >= 1000) {
            fprintf(stderr, "[TEA_FETCH] C=%llu PC=0x%llx Q_cnt=%d State=%d NeedSync=%d SyncRdy=%llu\n",
                    cycle_count, current_fetch_addr, fq->count, tctx->state,
                    tea_thread_needs_rat_sync(proc_id), tctx->rat_sync_ready_cycle);
            last_log_cycle[proc_id] = cycle_count;
        }
    }
    
    if (tea_thread_needs_rat_sync(proc_id)) {
        return;
    }

    if (tctx->state == TEA_STATE_DISABLED) {
        return;
    }

    /* ============================================================
     * [Shadow FTQ Integration] Phase 3
     * 논문 Section IV-D: "Fetch addresses generated by the branch predictor
     * are sent to both the Block Cache and the I-cache"
     * 
     * 1. Shadow FTQ에서 Op를 가져와 Block Cache dependency_mask로 필터링
     * 2. Shadow FTQ가 비어있으면 기존 Block Cache 직접 접근 방식 사용
     * ============================================================ */
    
    Flag enqueued = FALSE;
    if (!tctx->terminated_by_miss) {
        /* 먼저 Shadow FTQ에서 fetch 시도 (논문 방식) */
        if (tea_shadow_ftq_count(proc_id) > 0) {
            enqueued = tea_enqueue_from_shadow_ftq(proc_id, tctx);
        }
        
        /* Shadow FTQ가 비어있으면 기존 Block Cache 직접 접근 (fallback) */
        if (!enqueued) {
            Dependency_Chain_Cache_Entry* block_entry = get_dependency_chain_block(proc_id, current_fetch_addr);
            Flag block_hit = (block_entry && block_entry->is_valid && block_entry->h2p_branch_pc == current_fetch_addr);
            Flag empty_hit = (!block_hit) ? is_empty_block_tag_hit(proc_id, current_fetch_addr) : FALSE;
            
            if (empty_hit) {
                STAT_EVENT(proc_id, TEA_BLOCK_CACHE_EMPTY_HIT);
            }

            // [TEA-Phase4] Log Block Cache Miss (empty-hit은 miss가 아님)
            if (!block_hit && !empty_hit) {
                STAT_EVENT(proc_id, TEA_BLOCK_CACHE_MISS);
            }

            enqueued = tea_enqueue_block_entry(proc_id, tctx, block_hit ? block_entry : NULL);
        }
    }

    if (!enqueued && tctx->state == TEA_STATE_IDLE) {
        return;
    }

    if (tctx->state == TEA_STATE_ACTIVE && fq->count == 0) {
        tea_transition_state(tctx, TEA_STATE_DRAINING);
    } else if (tctx->state == TEA_STATE_DRAINING && fq->count == 0 && tea_backend_is_idle(proc_id)) {
        if (tctx->terminated_by_miss && tea_shadow_ftq_count(proc_id) > 0) {
            tea_shadow_ftq_recover(proc_id);
        }
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
