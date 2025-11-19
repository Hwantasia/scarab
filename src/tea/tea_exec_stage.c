#include "tea_backend.h"

#include "globals/assert.h"
#include "globals/global_vars.h"

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

void tea_exec_stage_run(uns proc_id, TEA_Backend_State* backend, TEA_Rename_State* rs_state) {
    TEA_RS_State* state = &backend->rs;
    uns issue_budget = backend->issue_width;

    for (uns idx = 0; idx < state->size && issue_budget > 0; ++idx) {
        TEA_RS_Entry* slot = &state->entries[idx];
        if (!slot->in_use)
            continue;
        TEA_Node_Entry* entry = slot->node_entry;
        if (!entry || entry->issued)
            continue;
        if (!tea_operands_ready(rs_state, entry))
            continue;

        entry->issued = TRUE;
        entry->issue_cycle = cycle_count;
        entry->done_cycle = cycle_count + TEA_EXEC_LAT;
        issue_budget--;
    }

    uns busy_fu = 0;
    for (uns idx = 0; idx < state->size; ++idx) {
        TEA_RS_Entry* slot = &state->entries[idx];
        if (!slot->in_use)
            continue;
        TEA_Node_Entry* entry = slot->node_entry;
        if (!entry || !entry->issued)
            continue;
        if (entry->done_cycle > cycle_count) {
            busy_fu++;
            continue;
        }

        TEA_Op_Metadata* meta = entry->meta;
        if (!meta) {
            slot->in_use = FALSE;
            slot->node_entry = NULL;
            state->occupancy--;
            continue;
        }

        for (uns d = 0; d < meta->num_dst; ++d) {
            int phys = meta->dst_phys_id[d];
            if (phys != TEA_PHYS_INVALID)
                rs_state->phys_regs[phys].valid = TRUE;
        }
        for (uns s = 0; s < meta->num_src; ++s) {
            int phys = meta->src_phys_id[s];
            if (phys == TEA_PHYS_INVALID)
                continue;
            if (rs_state->phys_regs[phys].refcount > 0)
                rs_state->phys_regs[phys].refcount--;
            if (!rs_state->phys_regs[phys].valid && rs_state->phys_regs[phys].refcount == 0)
                rs_state->free_list[rs_state->free_count++] = phys;
        }

        if (entry->op)
            entry->op->state = OS_DONE;

        tea_backend_record_branch_completion(proc_id, entry);

        slot->in_use = FALSE;
        slot->node_entry = NULL;
        state->occupancy--;
    }

    tea_backend_update_rs_usage(proc_id, state->occupancy);
    tea_backend_update_fu_usage(proc_id, busy_fu);
}
