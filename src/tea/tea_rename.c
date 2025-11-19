#include "tea_rename.h"
#include "tea_decode.h"
#include "tea_fetch.h"
#include "map_rename.h"
#include "core.param.h"
#include "globals/global_vars.h"
#include <string.h>
#include <stdlib.h>

// 전역 포인터(런타임 NUM_CORES 기준 동적 할당)
TEA_Rename_State* tea_rename_state = NULL;
TEA_Issue_Queue*  tea_issue_queue  = NULL;

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

static void tea_shadow_rat_reset(uns proc_id, TEA_Rename_State* rs) {
    Flag arch_valid[TEA_MAX_ARCH_REGS];
    map_snapshot_arch_rat(proc_id, NULL, arch_valid, TEA_MAX_ARCH_REGS);

    rs->free_count = 0;
    for (int phys = 0; phys < TEA_MAX_PHYS_REGS; ++phys) {
        rs->phys_regs[phys].valid = FALSE;
        rs->phys_regs[phys].refcount = 0;
    }

    int next_phys = 0;
    for (int arch = 0; arch < TEA_MAX_ARCH_REGS; ++arch) {
        if (arch_valid[arch] && next_phys < TEA_MAX_PHYS_REGS) {
            rs->map_table[arch] = next_phys;
            rs->map_valid[arch] = TRUE;
            rs->phys_regs[next_phys].valid = TRUE;   // 커밋된 값이므로 즉시 사용 가능
            rs->phys_regs[next_phys].refcount = 0;
            next_phys++;
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

static inline int tea_alloc_phys(TEA_Rename_State* rs) {
    if (rs->free_count == 0)
        return TEA_PHYS_INVALID;
    int phys = rs->free_list[--rs->free_count];
    rs->phys_regs[phys].valid = FALSE;
    rs->phys_regs[phys].refcount = 0;
    return phys;
}

static inline int tea_get_arch_slot(int arch_id) {
    if (arch_id < 0 || arch_id >= TEA_MAX_ARCH_REGS)
        return -1;
    return arch_id;
}

static TEA_Op_Metadata* tea_meta_alloc(TEA_Rename_State* rs) {
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

static void tea_issue_queue_init(TEA_Issue_Queue* iq) {
    iq->head = 0;
    iq->tail = 0;
    iq->count = 0;
    memset(iq->entries, 0, sizeof(iq->entries));
}

/**
 * @brief TEA Rename 단계 초기화 함수. 시뮬레이션 시작 시 각 코어별로 호출.
 * @param proc_id 대상 코어 ID
 */
void tea_init_rename(uns proc_id) {
    ASSERT(proc_id < NUM_CORES, "proc_id out of bounds\n");
    ensure_rename_structs_initialized();
    TEA_Rename_State* rs = &tea_rename_state[proc_id];
    tea_shadow_rat_reset(proc_id, rs);
    tea_issue_queue_init(&tea_issue_queue[proc_id]);
}

/**
 * @brief TEA Rename (Map) 스테이지 함수. 디코드된 TEA uop들의 레지스터를 리네임하고 Issue 큐에 삽입.
 * @param proc_id 대상 코어 ID
 */
void tea_rename_stage(uns proc_id) {
    ASSERT(proc_id < NUM_CORES, "proc_id out of bounds\n");
    if (!tea_is_enabled()) {
        return;
    }
    if (!tea_contexts || !tea_contexts[proc_id]) {
        return;
    }
    TEA_Context* tctx = tea_contexts[proc_id];
    if (tctx->state == TEA_STATE_DISABLED || tctx->state == TEA_STATE_IDLE) {
        return;
    }

    ensure_rename_structs_initialized();
    TEA_Rename_State* rs = &tea_rename_state[proc_id];
    TEA_Decode_Buffer* dbuf = &tea_decode_buffer[proc_id];
    TEA_Issue_Queue* iq = &tea_issue_queue[proc_id];

    if (tea_thread_needs_rat_sync(proc_id)) {
        if (cycle_count < tctx->rat_sync_ready_cycle) {
            return; // 동기화 대기 중에는 Rename을 진행하지 않음
        }
        tea_shadow_rat_reset(proc_id, rs);
        tea_thread_ack_sync(proc_id);
    } else if (!rs->shadow_synced) {
        tea_shadow_rat_reset(proc_id, rs);
    }

    if (dbuf->num_ops == 0) {
        return;
    }

    int available_slots = TEA_MAX_NODE - iq->count;
    if (available_slots <= 0) {
        return; // Issue 큐가 가득 찼으므로 다음 사이클까지 대기
    }
    int rename_budget = dbuf->num_ops < available_slots ? dbuf->num_ops : available_slots;
    int width_limit = (int)tea_configured_width();
    if (rename_budget > width_limit) {
        rename_budget = width_limit;
    }

    int ops_consumed = 0;
    for (int k = 0; k < rename_budget; ++k) {
        Op* op = dbuf->ops[k];
        if (!op || !op->inst_info || !op->table_info) {
            continue;
        }

        TEA_Op_Metadata* meta = tea_meta_alloc(rs);
        if (!meta) {
            break;
        }

        uns num_src = op->table_info->num_src_regs;
        meta->num_src = num_src;
        for (uns s = 0; s < num_src; ++s) {
            int raw_arch = op->inst_info->srcs[s].id;
            int arch_id = tea_get_arch_slot(raw_arch);
            int phys = TEA_PHYS_INVALID;
            if (arch_id >= 0 && rs->map_valid[arch_id]) {
                phys = rs->map_table[arch_id];
            }
            if (phys == TEA_PHYS_INVALID && arch_id >= 0) {
                phys = arch_id;
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

        uns num_dst = op->table_info->num_dest_regs;
        meta->num_dst = num_dst;
        for (uns d = 0; d < num_dst; ++d) {
            int raw_arch = op->inst_info->dests[d].id;
            int arch_id = tea_get_arch_slot(raw_arch);
            int new_phys = (arch_id >= 0) ? tea_alloc_phys(rs) : TEA_PHYS_INVALID;
            if (arch_id >= 0) {
                int prev_phys = rs->map_valid[arch_id] ? rs->map_table[arch_id] : TEA_PHYS_INVALID;
                if (prev_phys != TEA_PHYS_INVALID) {
                    rs->phys_regs[prev_phys].valid = FALSE;
                }
                if (new_phys != TEA_PHYS_INVALID) {
                    rs->map_table[arch_id] = new_phys;
                    rs->map_valid[arch_id] = TRUE;
                } else {
                    rs->map_valid[arch_id] = FALSE;
                }
            }
            meta->dst_phys_id[d] = new_phys;
            if (new_phys != TEA_PHYS_INVALID) {
                op->dst_reg_id[d][REG_TABLE_TYPE_PHYSICAL] = new_phys;
            }
        }

        meta->op = op;
        meta->valid = TRUE;

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
