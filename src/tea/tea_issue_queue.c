#include "tea_backend.h"

#include "globals/assert.h"
#include "globals/global_vars.h"
#include "statistics.h"

static TEA_RS_Entry* tea_find_free_rs_slot(TEA_RS_State* state) {
    for (uns idx = 0; idx < state->size; ++idx) {
        if (!state->entries[idx].in_use)
            return &state->entries[idx];
    }
    return NULL;
}

void tea_issue_queue_stage(uns proc_id, TEA_Backend_State* backend, TEA_Rename_State* rs_state,
                           uns width_limit) {
    (void)rs_state;
    TEA_RS_State* state = &backend->rs;
    TEA_Node_Entry* cursor = backend->rob.next_into_rs;
    uns moved = 0;

    if (cursor && state->occupancy >= state->size) {
        STAT_EVENT(proc_id, TEA_RS_FULL);
    }

    while (cursor && state->occupancy < state->size && moved < width_limit) {
        TEA_RS_Entry* slot = tea_find_free_rs_slot(state);
        if (!slot)
            break;
        slot->in_use = TRUE;
        slot->node_entry = cursor;
        state->occupancy++;

        cursor->in_rs = TRUE;
        if (cursor->op)
            cursor->op->state = OS_IN_RS;
        
        STAT_EVENT(proc_id, TEA_OP_ISSUED);

        cursor = cursor->next;
        backend->rob.next_into_rs = cursor;
        moved++;
    }

    tea_backend_update_rs_usage(proc_id, state->occupancy);
}
