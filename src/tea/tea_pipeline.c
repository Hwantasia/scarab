#include "tea_pipeline.h"

/**
 * TEA Backend Pipeline Overview
 *
 * The main Scarab pipeline follows MAP -> NODE (ROB) -> RS -> Ready List -> FU.
 * This file builds a TEA-specific replica of that backend so that helper-thread
 * μops experience the same structural bottlenecks and timing as main-thread μops.
 * The implementation purposely mirrors the coding patterns used in
 * node_stage.c / node_issue_queue.cc so future contributors can reason about TEA
 * activity using familiar interfaces.
 */

#include "core.param.h"
#include "globals/assert.h"
#include "globals/global_vars.h"
#include "statistics.h"
#include "op.h"
#include "tea_decode.h"
#include "tea_fetch.h"
#include "tea_rename.h"

#include <stdlib.h>
#include <string.h>

/* ---------- Per-stage data structures ---------- */

typedef struct TEA_Node_Entry_struct {
    Op* op;                     /**< Scarab Op allocated for the TEA uop            */
    TEA_Op_Metadata* meta;      /**< Shadow RAT bookkeeping for src/dst phys regs   */
    struct TEA_Node_Entry_struct* next;
    Flag in_rs;                 /**< Has the entry been inserted into TEA RS yet?   */
    Flag issued;                /**< Has the op been issued to the TEA execution units? */
    Counter ready_cycle;        /**< Earliest cycle when op may enter RS            */
    Counter done_cycle;         /**< Cycle when execution completes                 */
} TEA_Node_Entry;

typedef struct {
    TEA_Node_Entry entries[TEA_MAX_NODE];
    TEA_Node_Entry* free_list;
    TEA_Node_Entry* head;
    TEA_Node_Entry* tail;
    TEA_Node_Entry* next_into_rs;
    uns count;
} TEA_Node_Window;

typedef struct {
    Flag in_use;
    TEA_Node_Entry* node_entry;
} TEA_RS_Entry;

typedef struct {
    TEA_RS_Entry* entries;
    uns size;
    uns occupancy;
} TEA_RS_State;

typedef struct {
    TEA_Node_Window rob;
    TEA_RS_State    rs;
    uns             issue_width;
} TEA_Backend_State;

/* ---------- Module-wide state ---------- */
static TEA_Backend_State* tea_backend_states = NULL;
static uns* tea_rs_usage = NULL;
static uns* tea_fu_usage = NULL;

/* ---------- Helper forward declarations ---------- */
static void tea_backend_init_if_needed(uns proc_id);
static void tea_pipeline_fill_rob_from_map(uns proc_id, TEA_Backend_State* backend);
static void tea_pipeline_move_ops_into_rs(uns proc_id, TEA_Backend_State* backend, TEA_Rename_State* rs_state);
static void tea_pipeline_issue_ready_ops(uns proc_id, TEA_Backend_State* backend, TEA_Rename_State* rs_state);
static void tea_pipeline_wb_completed_ops(uns proc_id, TEA_Backend_State* backend, TEA_Rename_State* rs_state);
static void tea_pipeline_retire_ops(uns proc_id, TEA_Backend_State* backend, TEA_Rename_State* rs_state);

static void tea_release_phys_if_possible(TEA_Rename_State* rs_state, int phys_id) {
    if (phys_id == TEA_PHYS_INVALID)
        return;
    if (rs_state->phys_regs[phys_id].refcount > 0) {
        rs_state->phys_regs[phys_id].refcount--;
    }
    if (!rs_state->phys_regs[phys_id].valid && rs_state->phys_regs[phys_id].refcount == 0) {
        rs_state->free_list[rs_state->free_count++] = phys_id;
    }
}

static void tea_meta_release(TEA_Rename_State* rs_state, TEA_Op_Metadata* meta) {
    if (!meta)
        return;
    uns idx = (uns)(meta - rs_state->meta_pool);
    if (idx < TEA_MAX_NODE) {
        rs_state->meta_in_use[idx] = FALSE;
        memset(meta, 0, sizeof(*meta));
    }
}

static inline TEA_Node_Entry* tea_node_alloc_entry(TEA_Node_Window* rob) {
    if (!rob->free_list)
        return NULL;
    TEA_Node_Entry* entry = rob->free_list;
    rob->free_list = entry->next;
    memset(entry, 0, sizeof(*entry));
    return entry;
}

static inline void tea_node_release_entry(TEA_Node_Window* rob, TEA_Node_Entry* entry) {
    entry->next = rob->free_list;
    rob->free_list = entry;
}

static void tea_backend_init_if_needed(uns proc_id) {
    if (!tea_backend_states) {
        tea_backend_states = (TEA_Backend_State*)calloc(NUM_CORES, sizeof(TEA_Backend_State));
        tea_rs_usage = (uns*)calloc(NUM_CORES, sizeof(uns));
        tea_fu_usage = (uns*)calloc(NUM_CORES, sizeof(uns));
        ASSERT(0, tea_backend_states && tea_rs_usage && tea_fu_usage);
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
        for (int ii = TEA_MAX_NODE - 1; ii >= 0; --ii) {
            backend->rob.entries[ii].next = backend->rob.free_list;
            backend->rob.free_list = &backend->rob.entries[ii];
        }
    }
}

static void tea_pipeline_fill_rob_from_map(uns proc_id, TEA_Backend_State* backend) {
    if (!tea_issue_queue)
        return;
    TEA_Issue_Queue* iq = &tea_issue_queue[proc_id];
    if (iq->count == 0)
        return;

    while (iq->count > 0) {
        TEA_Issue_Entry issue_entry = iq->entries[iq->head];
        TEA_Node_Entry* rob_entry = tea_node_alloc_entry(&backend->rob);
        if (!rob_entry) {
            STAT_EVENT(proc_id, TEA_RS_FULL); // Repurpose to capture ROB pressure
            break;
        }

        iq->head = (iq->head + 1) % TEA_MAX_NODE;
        iq->count--;

        rob_entry->op = issue_entry.op;
        rob_entry->meta = issue_entry.meta;
        rob_entry->ready_cycle = cycle_count;
        rob_entry->in_rs = FALSE;
        rob_entry->issued = FALSE;
        rob_entry->done_cycle = 0;
        rob_entry->next = NULL;

        rob_entry->op->state = OS_IN_ROB;
        if (backend->rob.tail)
            backend->rob.tail->next = rob_entry;
        else
            backend->rob.head = rob_entry;
        backend->rob.tail = rob_entry;
        if (!backend->rob.next_into_rs)
            backend->rob.next_into_rs = rob_entry;
        backend->rob.count++;

        STAT_EVENT(proc_id, TEA_OP_ENQUEUED);
    }
}

static void tea_pipeline_move_ops_into_rs(uns proc_id, TEA_Backend_State* backend, TEA_Rename_State* rs_state) {
    TEA_Node_Entry* cursor = backend->rob.next_into_rs;
    while (cursor && backend->rs.occupancy < backend->rs.size) {
        TEA_RS_Entry* rs_slot = NULL;
        for (uns idx = 0; idx < backend->rs.size; ++idx) {
            if (!backend->rs.entries[idx].in_use) {
                rs_slot = &backend->rs.entries[idx];
                break;
            }
        }
        if (!rs_slot)
            break;

        rs_slot->in_use = TRUE;
        rs_slot->node_entry = cursor;
        backend->rs.occupancy++;

        cursor->in_rs = TRUE;
        cursor->op->state = OS_IN_RS;
        cursor = cursor->next;
        backend->rob.next_into_rs = cursor;
    }
    tea_rs_usage[proc_id] = backend->rs.occupancy;
}

static Flag tea_operands_ready(TEA_Rename_State* rs_state, TEA_Node_Entry* entry) {
    if (!entry || !entry->meta)
        return FALSE;
    TEA_Op_Metadata* meta = entry->meta;
    for (uns i = 0; i < meta->num_src; ++i) {
        int phys = meta->src_phys_id[i];
        if (phys == TEA_PHYS_INVALID)
            continue;
        if (!rs_state->phys_regs[phys].valid)
            return FALSE;
    }
    return TRUE;
}

static void tea_pipeline_issue_ready_ops(uns proc_id, TEA_Backend_State* backend, TEA_Rename_State* rs_state) {
    uns issue_budget = backend->issue_width;

    for (uns idx = 0; idx < backend->rs.size && issue_budget > 0; ++idx) {
        TEA_RS_Entry* rs_slot = &backend->rs.entries[idx];
        if (!rs_slot->in_use)
            continue;
        TEA_Node_Entry* entry = rs_slot->node_entry;
        if (!entry || entry->issued)
            continue;
        if (!tea_operands_ready(rs_state, entry))
            continue;

        entry->issued = TRUE;
        entry->op->state = OS_SCHEDULED;
        entry->done_cycle = cycle_count + TEA_EXEC_LAT;
        issue_budget--;
        STAT_EVENT(proc_id, TEA_OP_ISSUED);
    }
}

static void tea_pipeline_wb_completed_ops(uns proc_id, TEA_Backend_State* backend, TEA_Rename_State* rs_state) {
    uns busy_fu = 0;
    for (uns idx = 0; idx < backend->rs.size; ++idx) {
        TEA_RS_Entry* rs_slot = &backend->rs.entries[idx];
        if (!rs_slot->in_use)
            continue;
        TEA_Node_Entry* entry = rs_slot->node_entry;
        if (!entry || !entry->issued) {
            continue;
        }
        if (entry->done_cycle > cycle_count) {
            busy_fu++;
            continue;
        }

        TEA_Op_Metadata* meta = entry->meta;
        if (!meta) {
            rs_slot->in_use = FALSE;
            rs_slot->node_entry = NULL;
            backend->rs.occupancy--;
            continue;
        }
        for (uns d = 0; d < meta->num_dst; ++d) {
            int phys = meta->dst_phys_id[d];
            if (phys != TEA_PHYS_INVALID) {
                rs_state->phys_regs[phys].valid = TRUE;
            }
        }
        for (uns s = 0; s < meta->num_src; ++s) {
            tea_release_phys_if_possible(rs_state, meta->src_phys_id[s]);
        }

        entry->op->state = OS_DONE;
        rs_slot->in_use = FALSE;
        rs_slot->node_entry = NULL;
        backend->rs.occupancy--;
    }
    tea_rs_usage[proc_id] = backend->rs.occupancy;
    tea_fu_usage[proc_id] = busy_fu;
}

static void tea_pipeline_retire_ops(uns proc_id, TEA_Backend_State* backend, TEA_Rename_State* rs_state) {
    uns retire_budget = backend->issue_width; // Mirror NODE_RET_WIDTH for simplicity
    while (backend->rob.head && retire_budget > 0) {
        TEA_Node_Entry* head = backend->rob.head;
        if (head->op->state != OS_DONE)
            break;

        STAT_EVENT(proc_id, TEA_OP_COMPLETED);

        backend->rob.head = head->next;
        if (!backend->rob.head)
            backend->rob.tail = NULL;
        if (backend->rob.next_into_rs == head)
            backend->rob.next_into_rs = head->next;
        backend->rob.count--;

        tea_meta_release(rs_state, head->meta);
        head->op = NULL;
        head->meta = NULL;
        tea_node_release_entry(&backend->rob, head);

        retire_budget--;
    }
}

/* ---------- Public entry point ---------- */
void tea_pipeline_cycle(uns proc_id) {
    if (!tea_is_enabled())
        return;
    if (!tea_contexts || !tea_contexts[proc_id])
        return;

    TEA_Context* tctx = tea_contexts[proc_id];
    /* Only drive the backend when the TEA thread is alive; otherwise the supporting
       structures are not allocated yet. */
    if (tctx->state != TEA_STATE_ACTIVE && tctx->state != TEA_STATE_DRAINING)
        return;

    tea_backend_init_if_needed(proc_id);

    /* Frontend/rename stages still reuse the helpers defined earlier */
    tea_decode_stage(proc_id);
    tea_rename_stage(proc_id);

    if (!tea_rename_state || !tea_issue_queue)
        return;

    TEA_Backend_State* backend = &tea_backend_states[proc_id];
    TEA_Rename_State* rs_state = &tea_rename_state[proc_id];

    /* Backend ordering mirrors Scarab: fill ROB -> feed RS -> issue -> complete -> retire. */
    tea_pipeline_fill_rob_from_map(proc_id, backend);
    tea_pipeline_move_ops_into_rs(proc_id, backend, rs_state);
    tea_pipeline_issue_ready_ops(proc_id, backend, rs_state);
    tea_pipeline_wb_completed_ops(proc_id, backend, rs_state);
    tea_pipeline_retire_ops(proc_id, backend, rs_state);

    if (tctx->state == TEA_STATE_ACTIVE || tctx->state == TEA_STATE_DRAINING) {
        STAT_EVENT(proc_id, TEA_BACKEND_ACTIVE_CYCLE);
    }
}

/* ---------- Resource accounting helpers ---------- */
uns tea_backend_rs_usage(uns proc_id) {
    if (!tea_rs_usage)
        return 0;
    return tea_rs_usage[proc_id];
}

uns tea_backend_fu_usage(uns proc_id) {
    if (!tea_fu_usage)
        return 0;
    return tea_fu_usage[proc_id];
}
