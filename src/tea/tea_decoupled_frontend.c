/* Copyright 2020 HPS/SAFARI Research Groups
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

/***************************************************************************************
 * File         : tea_decoupled_frontend.c
 * Author       : TEA Implementation
 * Date         : 12/05/2025
 * Description  : Shadow FTQ for TEA thread - mirrors Main FTQ with synchronized timing
 ***************************************************************************************/

#include "tea_decoupled_frontend.h"
#include "tea_op_pool.h"
#include "tea_fetch.h"
#include "globals/global_vars.h"
#include "globals/assert.h"
#include "core.param.h"
#include "table_info.h"
#include "dependency_chain_cache.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/**************************************************************************************/
/* Global Variables */

TEA_Shadow_FTQ* tea_shadow_ftqs = NULL;

/**************************************************************************************/
/* Initialization and Reset */

void tea_shadow_ftq_init(uns proc_id) {
    if (!tea_shadow_ftqs) {
        tea_shadow_ftqs = (TEA_Shadow_FTQ*)calloc(NUM_CORES, sizeof(TEA_Shadow_FTQ));
        ASSERT(0, tea_shadow_ftqs);
    }
    
    TEA_Shadow_FTQ* ftq = &tea_shadow_ftqs[proc_id];
    
    /* Guard against double initialization - free existing entries first */
    if (ftq->entries) {
        /* Free all TEA Ops in existing FTs first */
        for (uns i = 0; i < ftq->size; i++) {
            TEA_FT* ft = &ftq->entries[i];
            for (uns j = 0; j < ft->op_count; j++) {
                if (ft->ops[j]) {
                    tea_free_op(ft->ops[j]);
                }
            }
        }
        free(ftq->entries);
        ftq->entries = NULL;
    }
    
    /* Use dynamic size from parameter (default 128) */
    ftq->size = TEA_SHADOW_FTQ_SIZE;
    ftq->entries = (TEA_FT*)calloc(ftq->size, sizeof(TEA_FT));
    ASSERT(proc_id, ftq->entries);
    
    ftq->head = 0;
    ftq->tail = 0;
    ftq->count = 0;
    
    /* Initialize all FT entries */
    for (uns i = 0; i < ftq->size; i++) {
        memset(&ftq->entries[i], 0, sizeof(TEA_FT));
    }
}

void tea_shadow_ftq_reset(uns proc_id) {
    if (!tea_shadow_ftqs) return;
    
    TEA_Shadow_FTQ* ftq = &tea_shadow_ftqs[proc_id];
    if (!ftq->entries) return;
    
    /* Free all TEA Ops in the FTQ */
    for (uns i = 0; i < ftq->size; i++) {
        TEA_FT* ft = &ftq->entries[i];
        for (uns j = 0; j < ft->op_count; j++) {
            if (ft->ops[j]) {
                tea_free_op(ft->ops[j]);
                ft->ops[j] = NULL;
            }
        }
        memset(ft, 0, sizeof(TEA_FT));
    }
    
    ftq->head = 0;
    ftq->tail = 0;
    ftq->count = 0;
}

/**************************************************************************************/
/* Op Cloning Helper - Creates TEA Op from Main Op with 100% prediction */

Op* tea_clone_op_from_main(uns proc_id, Op* main_op) {
    ASSERT(proc_id, main_op);
    ASSERT(proc_id, main_op->inst_info);
    
    /* Allocate TEA Op from TEA Op Pool */
    Op* tea_op = tea_alloc_op(proc_id);
    if (!tea_op) {
        return NULL;  /* Pool exhausted */
    }
    
    /* === Save TEA pool metadata before memcpy === */
    Counter saved_op_num = tea_op->op_num;
    Counter saved_unique_num = tea_op->unique_num;
    Counter saved_unique_per_proc = tea_op->unique_num_per_proc;
    Op* saved_pool_next = tea_op->op_pool_next;
    Flag saved_pool_valid = tea_op->op_pool_valid;
    uns saved_proc_id = tea_op->proc_id;
    
    /* Copy entire Op structure (overwrites everything!) */
    memcpy(tea_op, main_op, sizeof(Op));
    
    /* === Restore TEA pool metadata after memcpy === */
    tea_op->op_num = saved_op_num;
    tea_op->unique_num = saved_unique_num;
    tea_op->unique_num_per_proc = saved_unique_per_proc;
    tea_op->op_pool_next = saved_pool_next;
    tea_op->op_pool_valid = saved_pool_valid;
    tea_op->proc_id = saved_proc_id;
    
    /* === Reset runtime pointers to prevent RS/ROB/scheduler corruption === */
    tea_op->sched_info = NULL;
    tea_op->req = NULL;
    tea_op->next_node = NULL;
    tea_op->next_rdy = NULL;
    tea_op->in_rdy_list = FALSE;
    tea_op->in_node_list = FALSE;
    tea_op->state = OS_FETCHED;  /* Fresh state for TEA pipeline */
    
    /* Reset cycle timestamps for TEA pipeline */
    tea_op->issue_cycle = MAX_CTR;
    tea_op->map_cycle = MAX_CTR;
    tea_op->sched_cycle = MAX_CTR;
    tea_op->exec_cycle = MAX_CTR;
    tea_op->dcache_cycle = MAX_CTR;
    tea_op->done_cycle = MAX_CTR;
    tea_op->retire_cycle = MAX_CTR;
    
    /* [Feedback #4] Reset scheduling/execution state for clean TEA backend start */
    tea_op->rdy_cycle = cycle_count;          /* Ready from this cycle */
    tea_op->srcs_not_rdy_vector = 0;          /* All sources ready (will be set by TEA Rename) */
    
    /* === TEA-specific identity (link to Main Op) === */
    tea_op->tea_main_op_link = main_op;
    tea_op->tea_main_op_num = main_op->op_num;
    tea_op->tea_main_unique_num = main_op->unique_num;
    
    /* === 100% Perfect Branch Prediction === */
    if (main_op->table_info && main_op->table_info->cf_type != NOT_CF) {
        /* Direction: 100% accurate */
        tea_op->oracle_info.pred = main_op->oracle_info.dir;
        tea_op->oracle_info.pred_orig = main_op->oracle_info.dir;
        tea_op->oracle_info.late_pred = main_op->oracle_info.dir;
        
        /* Target: 100% accurate */
        tea_op->oracle_info.pred_npc = main_op->oracle_info.npc;
        tea_op->oracle_info.late_pred_npc = main_op->oracle_info.npc;
        
        /* No mispredictions in TEA thread */
        tea_op->oracle_info.mispred = FALSE;
        tea_op->oracle_info.misfetch = FALSE;
        tea_op->oracle_info.late_mispred = FALSE;
        tea_op->oracle_info.late_misfetch = FALSE;
        tea_op->oracle_info.btb_miss = FALSE;
        
        /* TEA Op should not schedule recovery itself */
        tea_op->oracle_info.recover_at_decode = FALSE;
        tea_op->oracle_info.recover_at_exec = FALSE;
        tea_op->oracle_info.recovery_sch = FALSE;
    }
    
    /* TEA Op Pool management - ensure it's marked as from TEA pool */
    tea_op->op_pool_valid = TRUE;
    
    /* === DEBUG LOG: Branch Op Fetch === */
    if (main_op->table_info && main_op->table_info->cf_type != NOT_CF) {
        if (DEBUG_TEA_SHADOW_FTQ) {
            const char* main_location = "UNKNOWN";
            switch (main_op->state) {
                case OS_FETCHED:   main_location = "FTQ"; break;
                case OS_IN_ROB:    main_location = "ROB"; break;
                case OS_IN_RS:     main_location = "RS"; break;
                case OS_READY:     main_location = "RS_Ready"; break;
                case OS_SCHEDULED: main_location = "EXEC"; break;
                case OS_DONE:      main_location = "DONE"; break;
                default:           main_location = "Other"; break;
            }
            
            fprintf(stderr, "[TEA_CLONE] C=%llu PC=0x%llx cf=%d | "
                    "TEA_op=%llu Main_op=%llu Main_unique=%llu | "
                    "Main_loc=%s Link=%s\n",
                    (unsigned long long)cycle_count,
                    (unsigned long long)main_op->inst_info->addr,
                    main_op->table_info->cf_type,
                    (unsigned long long)tea_op->op_num,
                    (unsigned long long)main_op->op_num,
                    (unsigned long long)main_op->unique_num,
                    main_location,
                    tea_op->tea_main_op_link == main_op ? "YES" : "NO");
        }
    }
    
    return tea_op;
}

/**************************************************************************************/
/* FT Insertion - Called when BP pushes FT to Main FTQ */

void tea_shadow_ft_insert(uns proc_id, void* main_ft_ptr) {
    /* ============================================================
     * [Feedback #4] tea_shadow_ft_insert 완성
     * 
     * 이 함수는 원래 void* main_ft_ptr을 받아 FT를 통째로 복사하는
     * 방식으로 설계되었으나, C++/C 인터페이스 복잡도 때문에
     * tea_shadow_ft_start() + tea_shadow_ft_add_op() 방식으로 대체됨.
     * 
     * 현재 decoupled_frontend.cc에서 사용하는 방법:
     * 1. FT push 시작 시: tea_shadow_ft_start(proc_id)
     * 2. 각 Op에 대해: tea_shadow_ft_add_op(proc_id, op)
     * 
     * 이 방식이 더 명확하고 BP 메타데이터(start_pc, end_pc)도
     * tea_shadow_ft_add_op에서 자동으로 업데이트됨.
     * 
     * 따라서 이 함수는 사용하지 않으며, 유지보수 목적으로만 존재함.
     * ============================================================ */
    
    if (!tea_is_enabled()) return;
    if (!tea_shadow_ftqs) return;
    if (!main_ft_ptr) return;
    
    /* Not implemented - use tea_shadow_ft_start() + tea_shadow_ft_add_op() instead */
    ASSERT(proc_id, 0 && "tea_shadow_ft_insert is deprecated - use tea_shadow_ft_start+add_op");
}

/**************************************************************************************/
/* Helper: Insert a single cloned Op into current FT */

void tea_shadow_ft_add_op(uns proc_id, Op* main_op) {
    if (!tea_is_enabled()) return;
    if (!tea_shadow_ftqs) return;
    if (!main_op) return;
    
    TEA_Shadow_FTQ* ftq = &tea_shadow_ftqs[proc_id];
    if (!ftq->entries || ftq->count == 0) return;
    
    /* Get the current (most recent) FT being filled */
    uns current_idx = (ftq->tail == 0) ? (ftq->size - 1) : (ftq->tail - 1);
    TEA_FT* tea_ft = &ftq->entries[current_idx];
    
    if (tea_ft->op_count >= TEA_MAX_OP_PER_FT) {
        /* FT is full - log warning and drop */
        if (DEBUG_TEA_SHADOW_FTQ) {
            fprintf(stderr, "[TEA_SHADOW_FTQ_WARN] C=%llu FT overflow: op_count=%u >= max=%u, dropping Op PC=0x%llx\n",
                    (unsigned long long)cycle_count, tea_ft->op_count, TEA_MAX_OP_PER_FT,
                    (unsigned long long)main_op->inst_info->addr);
        }
        return;
    }
    
    /* Clone the Main Op */
    Op* tea_op = tea_clone_op_from_main(proc_id, main_op);
    if (!tea_op) return;
    
    /* Add to FT */
    tea_ft->ops[tea_ft->op_count] = tea_op;
    tea_ft->op_count++;
    
    /* Update PC range */
    if (tea_ft->op_count == 1) {
        tea_ft->start_pc = main_op->inst_info->addr;
    }
    tea_ft->end_pc = main_op->inst_info->addr;
}

/**************************************************************************************/
/* FT Start - Prepares a new Shadow FT slot */

void tea_shadow_ft_start(uns proc_id) {
    if (!tea_is_enabled()) return;
    if (!tea_shadow_ftqs) return;
    
    TEA_Shadow_FTQ* ftq = &tea_shadow_ftqs[proc_id];
    if (!ftq->entries) return;
    
    /* Check if Shadow FTQ is full */
    if (ftq->count >= ftq->size) {
        /* Overwrite oldest entry - free its Ops first */
        TEA_FT* oldest = &ftq->entries[ftq->head];
        for (uns i = 0; i < oldest->op_count; i++) {
            if (oldest->ops[i]) {
                tea_free_op(oldest->ops[i]);
                oldest->ops[i] = NULL;
            }
        }
        oldest->op_count = 0;
        ftq->head = (ftq->head + 1) % ftq->size;
        ftq->count--;
    }
    
    /* Initialize new FT slot */
    TEA_FT* tea_ft = &ftq->entries[ftq->tail];
    memset(tea_ft, 0, sizeof(TEA_FT));
    tea_ft->consumed = FALSE;
    
    ftq->tail = (ftq->tail + 1) % ftq->size;
    ftq->count++;
}

/**************************************************************************************/
/* FT Consumption */

TEA_FT* tea_shadow_ftq_peek(uns proc_id) {
    if (!tea_shadow_ftqs) return NULL;
    
    TEA_Shadow_FTQ* ftq = &tea_shadow_ftqs[proc_id];
    if (!ftq->entries || ftq->count == 0) return NULL;
    
    return &ftq->entries[ftq->head];
}

void tea_shadow_ftq_consume(uns proc_id) {
    if (!tea_shadow_ftqs) return;
    
    TEA_Shadow_FTQ* ftq = &tea_shadow_ftqs[proc_id];
    if (!ftq->entries || ftq->count == 0) return;
    
    TEA_FT* ft = &ftq->entries[ftq->head];
    
    /* Free all Ops in this FT */
    for (uns i = 0; i < ft->op_count; i++) {
        if (ft->ops[i]) {
            tea_free_op(ft->ops[i]);
            ft->ops[i] = NULL;
        }
    }
    
    memset(ft, 0, sizeof(TEA_FT));
    
    ftq->head = (ftq->head + 1) % ftq->size;
    ftq->count--;
}

uns tea_shadow_ftq_count(uns proc_id) {
    if (!tea_shadow_ftqs) return 0;
    return tea_shadow_ftqs[proc_id].count;
}

/**************************************************************************************/
/* Recovery */

void tea_shadow_ftq_recover(uns proc_id) {
    /* Same as reset - flush all entries */
    tea_shadow_ftq_reset(proc_id);
    
    if (DEBUG_TEA_SHADOW_FTQ) {
        fprintf(stderr, "[TEA_SHADOW_FTQ_RECOVER] C=%llu proc_id=%u\n",
                (unsigned long long)cycle_count, proc_id);
    }
}

/**************************************************************************************/
/* Frontend-only Partial Flush (논문 Section IV-F) */

Flag tea_frontend_partial_flush(uns proc_id, Op* mispred_op) {
    if (!tea_shadow_ftqs || !mispred_op) return FALSE;
    
    TEA_Shadow_FTQ* ftq = &tea_shadow_ftqs[proc_id];
    if (!ftq->entries || ftq->count == 0) return FALSE;
    
    Counter mispred_unique = mispred_op->unique_num;
    uns flushed_ops = 0;
    uns flushed_fts = 0;
    
    /* ============================================================
     * 논문 Section IV-F:
     * "Partial flushes in the frontend are supported by adding a 
     * comparator before the flush signal for each pipeline stage 
     * to compare the timestamp of the instructions in that stage 
     * and the mispredicting branch."
     * 
     * 구현:
     * 1. Shadow FTQ를 순회하며 mispred_op보다 젊은(unique_num 큰) Op 제거
     * 2. 백엔드 체크포인트/ROB는 건드리지 않음
     * 3. Shadow RAT는 mispred_op 시점으로 복원 (여기서는 TEA 전용 RAT가 없으므로 생략)
     * ============================================================ */
    
    /* Iterate through all FTs in Shadow FTQ */
    uns idx = ftq->head;
    for (uns ft_idx = 0; ft_idx < ftq->count; ft_idx++) {
        TEA_FT* tea_ft = &ftq->entries[idx];
        Flag ft_modified = FALSE;
        
        for (uns i = 0; i < tea_ft->op_count; i++) {
            Op* tea_op = tea_ft->ops[i];
            if (!tea_op) continue;
            
            /* Compare using tea_main_unique_num (the Main Op's unique_num at clone time) */
            Counter tea_main_unique = tea_op->tea_main_unique_num;
            
            /* Flush Ops younger than mispred_op (larger unique_num) */
            if (tea_main_unique > mispred_unique) {
                tea_free_op(tea_op);
                tea_ft->ops[i] = NULL;
                flushed_ops++;
                ft_modified = TRUE;
            }
        }
        
        /* Compact the FT if modified */
        if (ft_modified) {
            uns write_idx = 0;
            for (uns i = 0; i < tea_ft->op_count; i++) {
                if (tea_ft->ops[i]) {
                    if (write_idx != i) {
                        tea_ft->ops[write_idx] = tea_ft->ops[i];
                        tea_ft->ops[i] = NULL;
                    }
                    write_idx++;
                }
            }
            tea_ft->op_count = write_idx;
            
            /* If FT is now empty, mark for removal */
            if (tea_ft->op_count == 0) {
                flushed_fts++;
            }
        }
        
        idx = (idx + 1) % ftq->size;
    }
    
    /* Remove empty FTs from tail */
    while (ftq->count > 0) {
        uns last_idx = (ftq->head + ftq->count - 1) % ftq->size;
        if (ftq->entries[last_idx].op_count == 0) {
            ftq->count--;
            ftq->tail = (ftq->tail == 0) ? ftq->size - 1 : ftq->tail - 1;
        } else {
            break;
        }
    }
    
    if (DEBUG_TEA_SHADOW_FTQ && flushed_ops > 0) {
        fprintf(stderr, "[TEA_FRONTEND_PARTIAL_FLUSH] C=%llu mispred_unique=%llu flushed_ops=%u flushed_fts=%u remaining=%u\n",
                (unsigned long long)cycle_count,
                (unsigned long long)mispred_unique,
                flushed_ops, flushed_fts, ftq->count);
    }
    
    return (flushed_ops > 0);
}
