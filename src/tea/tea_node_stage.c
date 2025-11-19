#include "tea_backend.h"

#include "globals/assert.h"
#include "globals/global_vars.h"

void tea_node_stage_fill(uns proc_id, TEA_Backend_State* backend, TEA_Rename_State* rs_state,
                         TEA_Issue_Queue* iq) {
    (void)rs_state;
    if (!iq || iq->count == 0)
        return;

    while (iq->count > 0) {
        TEA_Node_Entry* rob_entry = tea_allocate_node_entry(backend);
        if (!rob_entry)
            break;

        TEA_Issue_Entry issue_entry = iq->entries[iq->head];
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
        if (!backend->rob.head)
            backend->rob.head = rob_entry;
        backend->rob.tail = rob_entry;
        if (!backend->rob.next_into_rs)
            backend->rob.next_into_rs = rob_entry;
        backend->rob.count++;
    }
}

void tea_node_stage_retire(uns proc_id, TEA_Backend_State* backend, TEA_Rename_State* rs_state) {
    uns retire_budget = backend->issue_width;
    while (backend->rob.head && retire_budget > 0) {
        TEA_Node_Entry* head = backend->rob.head;
        if (!head->op || head->op->state != OS_DONE)
            break;

        retire_budget--;

        backend->rob.head = head->next;
        if (!backend->rob.head)
            backend->rob.tail = NULL;
        if (backend->rob.next_into_rs == head)
            backend->rob.next_into_rs = head->next;
        backend->rob.count--;

        tea_meta_release(rs_state, head->meta);
        head->op = NULL;
        head->meta = NULL;
        tea_release_node_entry(backend, head);
    }
}
