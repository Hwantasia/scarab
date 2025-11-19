#include "tea/tea_rename.h"
#include "tea/tea_decode.h"
#include "tea/tea_fetch.h"

#include "map_rename.h"
#include "core.param.h"
#include "globals/global_vars.h"
#include "globals/assert.h"

#include <string.h>
#include <stdlib.h>

/**
 * 전역 Rename/Issue 상태. NUM_CORES 기준으로 동적 할당되며 TEA 활성 시에만 사용된다.
 */
TEA_Rename_State* tea_rename_state = NULL;
TEA_Issue_Queue* tea_issue_queue  = NULL;

/**
 * @brief TEA Rename/Issue 자료구조를 최초 접근 시 할당한다.
 */
static void ensure_rename_structs_initialized(void) {
    if (tea_rename_state && tea_issue_queue) return;
    tea_rename_state = (TEA_Rename_State*)calloc(NUM_CORES, sizeof(TEA_Rename_State));
    tea_issue_queue  = (TEA_Issue_Queue*)calloc(NUM_CORES, sizeof(TEA_Issue_Queue));
    ASSERT(0, tea_rename_state && tea_issue_queue);
}

static void tea_reset_meta_pool(TEA_Rename_State* rs) {
    memset(rs->meta_pool, 0, sizeof(rs->meta_pool));
    memset(rs->meta_in_use, 0, sizeof(rs->meta_in_use));
}

/**
 * @brief 새 TEA 물리 레지스터를 할당하고 초기화한다.
 */
static inline int tea_alloc_phys(TEA_Rename_State* rs) {
    if (rs->free_count == 0)
        return TEA_PHYS_INVALID;
    int phys = rs->free_list[--rs->free_count];
    rs->phys_regs[phys].valid = FALSE; 
    rs->phys_regs[phys].refcount = 0;
    return phys;
}

/**
 * @brief 메인 스레드 RAT를 복사하여 Shadow RAT을 재구성한다.
 *        Poison bit을 사용하지 않고 Live-in이 모두 valid 하다고 가정한다.
 */
static void tea_shadow_rat_reset(uns proc_id, TEA_Rename_State* rs) {
    rs->free_count = 0;
    for (int phys = 0; phys < TEA_MAX_PHYS_REGS; ++phys) {
        rs->phys_regs[phys].valid = FALSE;
        rs->phys_regs[phys].refcount = 0;
    }
    Flag main_arch_valid[TEA_MAX_ARCH_REGS];
    map_snapshot_arch_rat(proc_id, NULL, main_arch_valid, TEA_MAX_ARCH_REGS);

    int next_phys = 0;
    for (int arch = 0; arch < TEA_MAX_ARCH_REGS; ++arch) {
        if (main_arch_valid[arch]) {
            ASSERTM(proc_id, next_phys < TEA_MAX_PHYS_REGS,
                    "Not enough TEA physical registers for snapshot!\n");
            int tea_phys = next_phys++;
            rs->map_table[arch] = tea_phys;
            rs->map_valid[arch] = TRUE;
            rs->phys_regs[tea_phys].valid = TRUE;
            rs->phys_regs[tea_phys].refcount = 0;
        } else {
            rs->map_table[arch] = TEA_PHYS_INVALID;
            rs->map_valid[arch] = FALSE;
        }
    }

    for (int phys = next_phys; phys < TEA_MAX_PHYS_REGS; ++phys) {
        rs->free_list[rs->free_count++] = phys;
    }

    rs->shadow_synced = TRUE;
    tea_reset_meta_pool(rs);
}

// ... (이하 tea_rename_stage 등의 로직은 이전과 동일) ...

static inline int tea_get_arch_slot(int arch_id) {
    if (arch_id < 0 || arch_id >= TEA_MAX_ARCH_REGS) return -1;
    return arch_id;
}

TEA_Op_Metadata* tea_meta_alloc(TEA_Rename_State* rs) {
    for (int i = 0; i < TEA_MAX_NODE; ++i) {
        if (!rs->meta_in_use[i]) {
            rs->meta_in_use[i] = TRUE;
            TEA_Op_Metadata* meta = &rs->meta_pool[i];
            memset(meta, 0, sizeof(*meta));
            meta->valid = TRUE;
            return meta;
        }
    }
    return NULL;
}

void tea_meta_release(TEA_Rename_State* rs_state, TEA_Op_Metadata* meta) {
    if (!meta)
        return;
    uns idx = (uns)(meta - rs_state->meta_pool);
    if (idx >= TEA_MAX_NODE)
        return;
    rs_state->meta_in_use[idx] = FALSE;
    memset(meta, 0, sizeof(*meta));
}

static void tea_issue_queue_init(TEA_Issue_Queue* iq) {
    iq->head = 0;
    iq->tail = 0;
    iq->count = 0;
    memset(iq->entries, 0, sizeof(iq->entries));
}

/**
 * @brief TEA Rename 상태를 초기화한다. (실제 Shadow RAT 스냅샷은 rename 단계에서 수행)
 */
void tea_init_rename(uns proc_id) {
    ASSERT(proc_id < NUM_CORES, "proc_id out of bounds\n");
    ensure_rename_structs_initialized();
    
    // 초기화 시점에는 map_snapshot 호출 시점이 아닐 수 있으므로 구조체만 0으로 설정
    TEA_Rename_State* rs = &tea_rename_state[proc_id];
    memset(rs, 0, sizeof(TEA_Rename_State));
    tea_issue_queue_init(&tea_issue_queue[proc_id]);
}

Flag tea_issue_queue_is_full(uns proc_id) {
    if (!tea_issue_queue) return TRUE;
    return tea_issue_queue[proc_id].count >= TEA_MAX_NODE;
}

/**
 * @brief TEA Decode 버퍼에서 uop를 가져와 Shadow RAT을 이용한 리네이밍을 수행한다.
 */
void tea_rename_stage(uns proc_id) {
    ASSERT(proc_id < NUM_CORES, "proc_id out of bounds\n");
    
    if (!tea_is_enabled()) return;
    if (!tea_contexts || !tea_contexts[proc_id]) return;
    
    TEA_Context* tctx = tea_contexts[proc_id];
    if (tctx->state == TEA_STATE_DISABLED || tctx->state == TEA_STATE_IDLE) return;

    ensure_rename_structs_initialized();
    TEA_Rename_State* rs = &tea_rename_state[proc_id];
    TEA_Decode_Buffer* dbuf = &tea_decode_buffer[proc_id];
    TEA_Issue_Queue* iq = &tea_issue_queue[proc_id];

    // 동기화 및 스냅샷 로직
    if (tea_thread_needs_rat_sync(proc_id)) {
        if (cycle_count < tctx->rat_sync_ready_cycle) return;
        
        // 여기서 map_snapshot_arch_rat을 사용하는 리셋 함수 호출
        tea_shadow_rat_reset(proc_id, rs);
        tea_thread_ack_sync(proc_id);
    }

    if (dbuf->num_ops == 0) return;

    int available_slots = TEA_MAX_NODE - iq->count;
    if (available_slots <= 0) return;

    int rename_width = (int)tea_configured_width();
    int rename_count = (dbuf->num_ops < available_slots) ? dbuf->num_ops : available_slots;
    if (rename_count > rename_width) rename_count = rename_width;

    int ops_consumed = 0;
    for (int k = 0; k < rename_count; ++k) {
        Op* op = dbuf->ops[k];
        if (!op || !op->inst_info || !op->table_info) continue;

        int required_phys = op->table_info->num_dest_regs;
        if (rs->free_count < required_phys) break; 

        TEA_Op_Metadata* meta = tea_meta_alloc(rs);
        if (!meta) break;

        // 소스 리네이밍
        uns num_src = op->table_info->num_src_regs;
        meta->num_src = num_src;
        for (uns s = 0; s < num_src; ++s) {
            int raw_arch = op->inst_info->srcs[s].id;
            int arch_id = tea_get_arch_slot(raw_arch);
            int phys = TEA_PHYS_INVALID;

            if (arch_id >= 0 && rs->map_valid[arch_id]) {
                phys = rs->map_table[arch_id];
            }
            // 스냅샷이 제대로 되었다면 모든 Arch Reg에 대해 TEA Phys가 할당되어 있어야 함
            // 만약 없다면 예외처리
            if (phys == TEA_PHYS_INVALID && arch_id >= 0) {
                 // Fallback: should not happen ideally if snapshot is correct
                 phys = tea_alloc_phys(rs);
                 rs->phys_regs[phys].valid = TRUE; 
                 rs->map_table[arch_id] = phys;
                 rs->map_valid[arch_id] = TRUE;
            }

            meta->src_phys_id[s] = phys;
            meta->src_ready[s] = (phys != TEA_PHYS_INVALID) ? rs->phys_regs[phys].valid : TRUE;
            
            if (phys != TEA_PHYS_INVALID) {
                rs->phys_regs[phys].refcount++;
                op->src_reg_id[s][REG_TABLE_TYPE_PHYSICAL] = phys;
            }
        }

        // 목적 리네이밍
        uns num_dst = op->table_info->num_dest_regs;
        meta->num_dst = num_dst;
        for (uns d = 0; d < num_dst; ++d) {
            int raw_arch = op->inst_info->dests[d].id;
            int arch_id = tea_get_arch_slot(raw_arch);
            
            int new_phys = TEA_PHYS_INVALID;
            if (arch_id >= 0) {
                new_phys = tea_alloc_phys(rs);
                rs->map_table[arch_id] = new_phys;
                rs->map_valid[arch_id] = TRUE;
                
                rs->phys_regs[new_phys].valid = FALSE; 
                rs->phys_regs[new_phys].producer_op = op;
            }

            meta->dst_phys_id[d] = new_phys;
            if (new_phys != TEA_PHYS_INVALID) {
                op->dst_reg_id[d][REG_TABLE_TYPE_PHYSICAL] = new_phys;
            }
        }

        meta->op = op;
        
        iq->entries[iq->tail].op = op;
        iq->entries[iq->tail].meta = meta;
        iq->tail = (iq->tail + 1) % TEA_MAX_NODE;
        iq->count++;
        
        ops_consumed++;
    }

    if (ops_consumed > 0) {
        if (ops_consumed < dbuf->num_ops) {
            memmove(dbuf->ops, dbuf->ops + ops_consumed, sizeof(Op*) * (dbuf->num_ops - ops_consumed));
        }
        dbuf->num_ops -= ops_consumed;
        if (dbuf->num_ops < 0) dbuf->num_ops = 0;
    }
}
