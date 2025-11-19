#include "tea_backend.h"

#include "bp/bp.h"
#include "cmp_model.h"
#include "core.param.h"
#include "globals/assert.h"
#include "globals/global_vars.h"
#include "globals/utils.h"
#include "node_stage.h"
#include "tea_fetch.h"

/* Global backend state ----------------------------------------------------------- */
static TEA_Backend_State* tea_backend_states = NULL;
static Counter* tea_last_branch_unique = NULL;
static uns* tea_rs_usage_shadow = NULL;
static uns* tea_fu_usage_shadow = NULL;

#define TEA_MAX_PENDING_BRANCHES 16
typedef struct {
    Flag valid;
    Addr branch_pc;
} TEA_Pending_Branch;

static TEA_Pending_Branch** tea_pending_branches = NULL;

/* Forward declarations ----------------------------------------------------------- */
static Flag tea_issue_early_recovery(uns proc_id, Addr branch_pc);
static void tea_enqueue_pending_branch(uns proc_id, Addr branch_pc);
static Op* tea_find_branch_candidate(uns proc_id, Addr branch_pc);

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
    if (!tea_issue_early_recovery(proc_id, entry->op->inst_info->addr)) {
        tea_enqueue_pending_branch(proc_id, entry->op->inst_info->addr);
    }
}

void tea_node_stage_process_pending(uns proc_id) {
    if (!tea_pending_branches)
        return;
    TEA_Pending_Branch* queue = tea_pending_branches[proc_id];
    for (int i = 0; i < TEA_MAX_PENDING_BRANCHES; ++i) {
        if (!queue[i].valid)
            continue;
        if (tea_issue_early_recovery(proc_id, queue[i].branch_pc)) {
            queue[i].valid = FALSE;
        }
    }
}

/* Early recovery helpers -------------------------------------------------------- */
static Flag tea_issue_early_recovery(uns proc_id, Addr branch_pc) {
    if (!tea_is_enabled())
        return FALSE;
    Op* main_branch = tea_find_branch_candidate(proc_id, branch_pc);
    if (!main_branch)
        return FALSE;
    tea_last_branch_unique[proc_id] = MAX2(tea_last_branch_unique[proc_id], main_branch->unique_num);
    if (!(main_branch->oracle_info.recover_at_exec || main_branch->oracle_info.recover_at_decode))
        return TRUE;
    if (main_branch->oracle_info.recovery_sch)
        return TRUE;

    Bp_Recovery_Info* info = &cmp_model.bp_recovery_info[proc_id];
    bp_sched_recovery(info, main_branch, cycle_count, FALSE, FALSE, TEA_EARLY_RECOVERY_CYCLES);
    STAT_EVENT(proc_id, TEA_BRANCH_EARLY_FLUSH);
    return TRUE;
}

static void tea_enqueue_pending_branch(uns proc_id, Addr branch_pc) {
    TEA_Pending_Branch* queue = tea_pending_branches[proc_id];
    for (int i = 0; i < TEA_MAX_PENDING_BRANCHES; ++i) {
        if (queue[i].valid && queue[i].branch_pc == branch_pc)
            return;
    }
    for (int i = 0; i < TEA_MAX_PENDING_BRANCHES; ++i) {
        if (!queue[i].valid) {
            queue[i].valid = TRUE;
            queue[i].branch_pc = branch_pc;
            return;
        }
    }
    memmove(&queue[0], &queue[1], sizeof(TEA_Pending_Branch) * (TEA_MAX_PENDING_BRANCHES - 1));
    queue[TEA_MAX_PENDING_BRANCHES - 1].valid = TRUE;
    queue[TEA_MAX_PENDING_BRANCHES - 1].branch_pc = branch_pc;
}

static Op* tea_find_branch_candidate(uns proc_id, Addr branch_pc) {
    Node_Stage* main_node = &cmp_model.node_stage[proc_id];
    if (!main_node)
        return NULL;
    Op* candidate = NULL;
    for (Op* it = main_node->node_head; it; it = it->next_node) {
        if (it->table_info->cf_type == NOT_CF)
            continue;
        if (it->off_path)
            continue;
        if (it->inst_info->addr != branch_pc)
            continue;
        if (it->state == OS_DONE)
            continue;
        if (it->unique_num <= tea_last_branch_unique[proc_id])
            continue;
        candidate = it;
    }
    return candidate;
}
