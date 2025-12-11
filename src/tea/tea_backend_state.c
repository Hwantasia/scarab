#include "tea_backend.h"
#include <stdio.h>
#include "bp/bp.h"
#include "cmp_model.h"
#include "core.param.h"
#include "globals/assert.h"
#include "globals/global_vars.h"
#include "globals/utils.h"
#include "node_stage.h"
#include "tea_fetch.h"
#include "tea_rename.h"
#include "tea_decode.h"
#include "tea_decoupled_frontend.h"
#include "op_pool.h"
#include "map_rename.h"
#include "log/tea_early_recovery_log.h"

/* Global backend state ----------------------------------------------------------- */
static TEA_Backend_State* tea_backend_states = NULL;
static Counter* tea_last_branch_unique = NULL;
static uns* tea_rs_usage_shadow = NULL;
static uns* tea_fu_usage_shadow = NULL;

#define TEA_MAX_PENDING_BRANCHES 16
typedef struct {
    Flag valid;
    Addr branch_pc;
    Op* tea_op;  // [TEA Early Binding] TEA Op 직접 저장
    Counter tea_exec_cycle;
} TEA_Pending_Branch;

static TEA_Pending_Branch** tea_pending_branches = NULL;

/* Forward declarations ----------------------------------------------------------- */
static Flag tea_issue_early_recovery(uns proc_id, Op* tea_op);
static void tea_enqueue_pending_branch(uns proc_id, Op* tea_op);
static Op* find_main_op_fallback(uns proc_id, Addr branch_pc, Cf_Type cf_type);

/* Public helpers ----------------------------------------------------------------- */
void tea_backend_init_if_needed(uns proc_id) {
    if (!tea_backend_states) {
        tea_backend_states = (TEA_Backend_State*)calloc(NUM_CORES, sizeof(TEA_Backend_State));
        tea_last_branch_unique = (Counter*)calloc(NUM_CORES, sizeof(Counter));
        tea_rs_usage_shadow = (uns*)calloc(NUM_CORES, sizeof(uns));
        tea_fu_usage_shadow = (uns*)calloc(NUM_CORES, sizeof(uns));
        tea_pending_branches = (TEA_Pending_Branch**)calloc(NUM_CORES, sizeof(TEA_Pending_Branch*));
        ASSERT(0, tea_backend_states && tea_last_branch_unique && tea_pending_branches &&
                    tea_rs_usage_shadow && tea_fu_usage_shadow);
        for (uns ii = 0; ii < NUM_CORES; ++ii) {
            tea_pending_branches[ii] =
                (TEA_Pending_Branch*)calloc(TEA_MAX_PENDING_BRANCHES, sizeof(TEA_Pending_Branch));
            ASSERT(ii, tea_pending_branches[ii]);
        }
    }
    TEA_Backend_State* backend = &tea_backend_states[proc_id];
    if (!backend->rs.entries) {
        backend->issue_width = (tea_configured_width() == 0) ? ISSUE_WIDTH : tea_configured_width();
        backend->rs.size = TEA_RS_SIZE;
        backend->rs.entries = (TEA_RS_Entry*)calloc(backend->rs.size, sizeof(TEA_RS_Entry));
        ASSERT(proc_id, backend->rs.entries);
        backend->rs.occupancy = 0;
        backend->rob.free_list = NULL;
        backend->rob.head = backend->rob.tail = backend->rob.next_into_rs = NULL;
        backend->rob.count = 0;
        for (int idx = TEA_MAX_NODE - 1; idx >= 0; --idx) {
            backend->rob.entries[idx].next = backend->rob.free_list;
            backend->rob.free_list = &backend->rob.entries[idx];
        }
    }
}

TEA_Backend_State* tea_backend_get_state(uns proc_id) {
    tea_backend_init_if_needed(proc_id);
    return &tea_backend_states[proc_id];
}

uns tea_backend_rs_usage(uns proc_id) {
    return tea_rs_usage_shadow ? tea_rs_usage_shadow[proc_id] : 0;
}

uns tea_backend_fu_usage(uns proc_id) {
    return tea_fu_usage_shadow ? tea_fu_usage_shadow[proc_id] : 0;
}

/* Utility routines shared across stages ----------------------------------------- */
TEA_Node_Entry* tea_allocate_node_entry(TEA_Backend_State* backend) {
    if (!backend->rob.free_list)
        return NULL;
    TEA_Node_Entry* entry = backend->rob.free_list;
    backend->rob.free_list = entry->next;
    memset(entry, 0, sizeof(*entry));
    return entry;
}

void tea_release_node_entry(TEA_Backend_State* backend, TEA_Node_Entry* entry) {
    entry->next = backend->rob.free_list;
    backend->rob.free_list = entry;
}

void tea_backend_update_rs_usage(uns proc_id, uns occupancy) {
    if (tea_rs_usage_shadow)
        tea_rs_usage_shadow[proc_id] = occupancy;
}

void tea_backend_update_fu_usage(uns proc_id, uns busy_fu) {
    if (tea_fu_usage_shadow)
        tea_fu_usage_shadow[proc_id] = busy_fu;
}

void tea_backend_record_branch_completion(uns proc_id, TEA_Node_Entry* entry) {
    if (!entry || !entry->op || entry->op->table_info->cf_type == NOT_CF)
        return;
    
    Op* tea_op = entry->op;
    ASSERT(proc_id, tea_op);
    ASSERT(proc_id, tea_op->table_info);
    
    // [TEA Early Binding] TEA Op을 직접 전달
    if (!tea_issue_early_recovery(proc_id, tea_op)) {
        tea_enqueue_pending_branch(proc_id, tea_op);
    }
}

void tea_node_stage_process_pending(uns proc_id) {
    if (!tea_pending_branches)
        return;
    TEA_Pending_Branch* queue = tea_pending_branches[proc_id];
    for (int i = 0; i < TEA_MAX_PENDING_BRANCHES; ++i) {
        if (!queue[i].valid)
            continue;
        // [TEA Early Binding] Retry with TEA Op
        if (tea_issue_early_recovery(proc_id, queue[i].tea_op)) {
            queue[i].valid = FALSE;
        }
    }
}

/* Early recovery helpers -------------------------------------------------------- */
static Flag tea_issue_early_recovery(uns proc_id, Op* tea_op) {
    if (!tea_is_enabled()) return FALSE;
    ASSERT(proc_id, tea_op);
    ASSERT(proc_id, tea_op->inst_info);
    ASSERT(proc_id, tea_op->table_info);
    
    /* ============================================================
     * [Phase 4] Early Binding Priority Order:
     * 1. tea_main_op_link (Shadow FTQ 경로 - Clone 시 직접 연결)
     * 2. tea_main_op_candidate (기존 FTQ 검색 결과)
     * 3. find_main_op_fallback (ROB 검색 - 최후 수단)
     * ============================================================ */
    
    Op* main_branch = NULL;
    const char* binding_type = "None";
    
    // 1. Shadow FTQ 경로: tea_main_op_link 우선 사용
    if (tea_op->tea_main_op_link && tea_op->tea_main_op_link->op_pool_valid) {
        main_branch = tea_op->tea_main_op_link;
        binding_type = "ShadowFTQ";
    }
    
    // 2. 기존 FTQ 검색 결과 (fallback)
    if (!main_branch || !main_branch->op_pool_valid) {
        if (tea_op->tea_main_op_candidate && tea_op->tea_main_op_candidate->op_pool_valid) {
            main_branch = tea_op->tea_main_op_candidate;
            binding_type = "FTQ_Candidate";
        }
    }
    
    // 3. ROB 검색 (최후 수단)
    if (!main_branch || !main_branch->op_pool_valid) {
        main_branch = find_main_op_fallback(proc_id, 
                                            tea_op->inst_info->addr, 
                                            tea_op->table_info->cf_type);
        if (main_branch) {
            binding_type = "ROB_Fallback";
        }
    }
    
    if (!main_branch) return FALSE;
    
    // Main Op 유효성 검증
    ASSERT(proc_id, main_branch->op_pool_valid);
    ASSERT(proc_id, main_branch->inst_info);
    ASSERT(proc_id, main_branch->inst_info->addr == tea_op->inst_info->addr);
    
    if (main_branch->state == OS_DONE) return FALSE;
    if (main_branch->off_path) return FALSE;
    
    // Track last recovered branch
    tea_last_branch_unique[proc_id] = MAX2(tea_last_branch_unique[proc_id], main_branch->unique_num);
    
    if (!(main_branch->oracle_info.recover_at_exec || main_branch->oracle_info.recover_at_decode))
        return TRUE;
    if (main_branch->oracle_info.recovery_sch)
        return TRUE;

    /* ============================================================
     * [Phase 4] Frontend-only Recovery (논문 Section IV-F)
     * 
     * "Partially Flushing the Frontend: when the TEA thread runs 
     * so far ahead that the main thread branch being flushed is 
     * in the frontend."
     * 
     * Main Op State에 따른 Recovery 경로 결정:
     * - OS_FETCHED: Frontend만 flush (Checkpoint 불필요)
     * - Others: 기존 Checkpoint 기반 recovery
     * ============================================================ */
    
    Flag is_frontend_only = FALSE;
    if (main_branch->state == OS_FETCHED) {
        is_frontend_only = TRUE;
        STAT_EVENT(proc_id, TEA_BRANCH_FRONTEND_ONLY_RECOVERY);
    }

    // [DEBUG] Early Recovery logging
    if (DEBUG_CYCLE_START <= cycle_count && cycle_count <= DEBUG_CYCLE_STOP) {
      fprintf(stderr, "[TEA_EARLY_RECOVERY] C=%llu PC=0x%llx Main_Op=%llu State=%d Bind=%s FrontendOnly=%d\n",
              cycle_count, tea_op->inst_info->addr, 
              main_branch->op_num, main_branch->state, binding_type, is_frontend_only);
    }
    
    /* ============================================================
     * [Updated Feedback Implementation] Frontend Early Recovery
     * 
     * 논문 Section IV-F:
     * - PC/BP 히스토리 수정은 필수 (bp_sched_recovery 호출)
     * - Frontend-only인 경우 penalty=0으로 즉시 recovery
     * - TEA Fetch Queue와 Shadow FTQ도 flush
     * 
     * 사용자 피드백:
     * 1. 메인 파이프라인 리다이렉트 필요 → bp_sched_recovery 호출
     * 2. TEA Fetch Queue도 flush 필요
     * 3. Shadow FTQ 부분 플러시
     * ============================================================ */
    
    Bp_Recovery_Info* info = &cmp_model.bp_recovery_info[proc_id];
    
    if (is_frontend_only) {
        /* Frontend-only: PC/BP 히스토리 수정 + penalty=0 (즉시 recovery) */
        info->frontend_only_recovery = TRUE;  /* Set flag for partial FTQ flush */
        bp_sched_recovery(info, main_branch, cycle_count, FALSE, FALSE, 0);
        
        /* TEA Shadow FTQ 부분 플러시 (mispred보다 젊은 Op 제거) */
        tea_frontend_partial_flush(proc_id, main_branch);
        
        /* TEA Fetch Queue도 flush */
        if (tea_contexts && tea_contexts[proc_id]) {
            tea_reset_fetch_queue(&tea_contexts[proc_id]->fetch_queue);
        }
        
        STAT_EVENT(proc_id, TEA_BRANCH_FRONTEND_ONLY_RECOVERY);
    } else {
        /* Backend: 기존 checkpoint 기반 recovery */
        info->frontend_only_recovery = FALSE;
        bp_sched_recovery(info, main_branch, cycle_count, FALSE, FALSE, TEA_EARLY_RECOVERY_CYCLES);
    }
    
    STAT_EVENT(proc_id, TEA_BRANCH_EARLY_FLUSH);

    /* [TEA Logging] enable via DEBUG_TEA_EARLY_RECOVERY_LOG */
    if (DEBUG_TEA_EARLY_RECOVERY_LOG) {
        init_tea_early_recovery_log();
        log_tea_early_recovery(proc_id, cycle_count, main_branch, tea_op, binding_type, is_frontend_only);
    }
    

    return TRUE;
}

static void tea_enqueue_pending_branch(uns proc_id, Op* tea_op) {
    ASSERT(proc_id, tea_op);
    ASSERT(proc_id, tea_op->inst_info);
    
    TEA_Pending_Branch* queue = tea_pending_branches[proc_id];
    Addr branch_pc = tea_op->inst_info->addr;
    
    // 이미 Pending인지 확인
    for (int i = 0; i < TEA_MAX_PENDING_BRANCHES; ++i) {
        if (queue[i].valid && queue[i].tea_op == tea_op)
            return;  // 이미 있음
    }
    
    // 빈 슬롯 찾기
    for (int i = 0; i < TEA_MAX_PENDING_BRANCHES; ++i) {
        if (!queue[i].valid) {
            queue[i].valid = TRUE;
            queue[i].branch_pc = branch_pc;
            queue[i].tea_op = tea_op;
            queue[i].tea_exec_cycle = tea_op->exec_cycle;
            return;
        }
    }
    
    // 꽉 참 - FIFO 밀어내기
    memmove(&queue[0], &queue[1], sizeof(TEA_Pending_Branch) * (TEA_MAX_PENDING_BRANCHES - 1));
    queue[TEA_MAX_PENDING_BRANCHES - 1].valid = TRUE;
    queue[TEA_MAX_PENDING_BRANCHES - 1].branch_pc = branch_pc;
    queue[TEA_MAX_PENDING_BRANCHES - 1].tea_op = tea_op;
    queue[TEA_MAX_PENDING_BRANCHES - 1].tea_exec_cycle = tea_op->exec_cycle;
}

/* ROB Fallback 검색 (Early Binding 실패 시) */
static Op* find_main_op_fallback(uns proc_id, Addr branch_pc, Cf_Type cf_type) {
    Node_Stage* main_node = &cmp_model.node_stage[proc_id];
    if (!main_node)
        return NULL;
    
    Op* candidate = NULL;
    for (Op* it = main_node->node_head; it; it = it->next_node) {
        if (!it->inst_info || !it->table_info) continue;
        if (it->inst_info->addr != branch_pc) continue;
        if (it->table_info->cf_type != cf_type) continue;
        if (it->off_path) continue;
        if (it->state == OS_DONE) continue;
        
        // 가장 최근 Op 선택 (op_num이 큰 것)
        if (!candidate || it->op_num > candidate->op_num) {
            candidate = it;
        }
    }
    
    return candidate;
}

Flag tea_backend_is_idle(uns proc_id) {
    if (!tea_backend_states)
        return TRUE;
    TEA_Backend_State* backend = &tea_backend_states[proc_id];
    
    // Check ROB
    if (backend->rob.count > 0)
        return FALSE;

    // Check Issue Queue (if initialized)
    if (tea_issue_queue && tea_issue_queue[proc_id].count > 0)
        return FALSE;

    // Check Decode Buffer (if initialized)
    if (tea_decode_buffer && tea_decode_buffer[proc_id].num_ops > 0)
        return FALSE;

    return TRUE;
}
