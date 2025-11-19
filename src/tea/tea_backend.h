#ifndef __TEA_BACKEND_H__
#define __TEA_BACKEND_H__

#include "globals/global_types.h"
#include "op.h"
#include "tea_rename.h"

#ifdef __cplusplus
extern "C" {
#endif

/* TEA backend data structures ----------------------------------------------------- */

typedef struct TEA_Node_Entry_struct {
    Op* op;                         /**< Pointer back to the Scarab Op snapshot              */
    TEA_Op_Metadata* meta;          /**< TEA rename metadata for this uop                    */
    struct TEA_Node_Entry_struct* next;
    Flag in_rs;                     /**< Has the entry entered the TEA RS?                   */
    Flag issued;                    /**< Has the entry been issued to TEA FUs?               */
    Counter ready_cycle;
    Counter issue_cycle;
    Counter done_cycle;
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

typedef struct TEA_Backend_State_struct {
    TEA_Node_Window rob;
    TEA_RS_State    rs;
    uns             issue_width;
} TEA_Backend_State;

/* Backend state helpers ----------------------------------------------------------- */

void tea_backend_init_if_needed(uns proc_id);
TEA_Backend_State* tea_backend_get_state(uns proc_id);

uns tea_backend_rs_usage(uns proc_id);
uns tea_backend_fu_usage(uns proc_id);

/* Stage level entry points -------------------------------------------------------- */

void tea_node_stage_fill(uns proc_id, TEA_Backend_State* backend, TEA_Rename_State* rs_state,
                         TEA_Issue_Queue* iq);
void tea_issue_queue_stage(uns proc_id, TEA_Backend_State* backend, TEA_Rename_State* rs_state,
                           uns width_limit);
void tea_exec_stage_run(uns proc_id, TEA_Backend_State* backend, TEA_Rename_State* rs_state);
void tea_node_stage_retire(uns proc_id, TEA_Backend_State* backend, TEA_Rename_State* rs_state);
void tea_node_stage_process_pending(uns proc_id);

/* Low-level helpers shared across stage implementations */
TEA_Node_Entry* tea_allocate_node_entry(TEA_Backend_State* backend);
void tea_release_node_entry(TEA_Backend_State* backend, TEA_Node_Entry* entry);
void tea_backend_update_rs_usage(uns proc_id, uns occupancy);
void tea_backend_update_fu_usage(uns proc_id, uns busy_fu);
void tea_backend_record_branch_completion(uns proc_id, TEA_Node_Entry* entry);

#ifdef __cplusplus
}
#endif

#endif /* __TEA_BACKEND_H__ */
