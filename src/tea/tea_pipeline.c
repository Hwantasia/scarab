#include "tea_pipeline.h"

#include "tea_backend.h"
#include "tea_decode.h"
#include "tea_fetch.h"
#include "tea_rename.h"

#include "core.param.h"
#include "globals/assert.h"
#include "globals/global_vars.h"
#include "statistics.h"

void tea_pipeline_cycle(uns proc_id) {
    if (!tea_is_enabled())
        return;
    if (!tea_contexts || !tea_contexts[proc_id])
        return;

    TEA_Context* tctx = tea_contexts[proc_id];
    if (tctx->state != TEA_STATE_ACTIVE && tctx->state != TEA_STATE_DRAINING)
        return;

    tea_backend_init_if_needed(proc_id);

    tea_decode_stage(proc_id);
    tea_rename_stage(proc_id);

    TEA_Backend_State* backend = tea_backend_get_state(proc_id);
    TEA_Rename_State* rs_state = &tea_rename_state[proc_id];
    TEA_Issue_Queue* iq = &tea_issue_queue[proc_id];

    tea_node_stage_fill(proc_id, backend, rs_state, iq);
    tea_issue_queue_stage(proc_id, backend, rs_state, backend->issue_width);
    tea_exec_stage_run(proc_id, backend, rs_state);
    tea_node_stage_retire(proc_id, backend, rs_state);
    tea_node_stage_process_pending(proc_id);

    STAT_EVENT(proc_id, TEA_BACKEND_ACTIVE_CYCLE);
}
