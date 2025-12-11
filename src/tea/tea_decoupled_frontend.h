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
 * File         : tea_decoupled_frontend.h
 * Author       : TEA Implementation
 * Date         : 12/05/2025
 * Description  : Shadow FTQ for TEA thread - mirrors Main FTQ with synchronized timing
 *                Based on TEA paper Section IV-A and IV-D
 ***************************************************************************************/

#ifndef __TEA_DECOUPLED_FRONTEND_H__
#define __TEA_DECOUPLED_FRONTEND_H__

#include "globals/global_types.h"
#include "globals/assert.h"
#include "op.h"
#include "core.param.h"

#ifdef __cplusplus
extern "C" {
#endif

/**************************************************************************************/
/* Constants - Dynamic via core.param.def */

/* Maximum Ops per FT (Fetch Target) - matches Main FTQ */
#define TEA_MAX_OP_PER_FT 64

/**************************************************************************************/
/* TEA FT (Fetch Target) Structure */

typedef struct TEA_FT_struct {
    Op* ops[TEA_MAX_OP_PER_FT];     /* TEA Op pointers (cloned from Main) */
    uns op_count;                   /* Number of Ops in this FT */
    Addr start_pc;                  /* First instruction PC */
    Addr end_pc;                    /* Last instruction PC */
    Flag consumed;                  /* Has this FT been consumed by TEA Decode? */
} TEA_FT;

/**************************************************************************************/
/* TEA Shadow FTQ Structure */

typedef struct TEA_Shadow_FTQ_struct {
    TEA_FT* entries;               /* Dynamically allocated FT array */
    uns size;                      /* Total capacity (from TEA_SHADOW_FTQ_SIZE) */
    uns head;                      /* Head index for consumption */
    uns tail;                      /* Tail index for insertion */
    uns count;                     /* Current occupancy */
} TEA_Shadow_FTQ;

/**************************************************************************************/
/* Global Variables */

extern TEA_Shadow_FTQ* tea_shadow_ftqs;  /* Per-core Shadow FTQ array */

/**************************************************************************************/
/* Initialization and Reset */

/**
 * @brief Initialize Shadow FTQ for a core
 * @param proc_id Core ID
 */
void tea_shadow_ftq_init(uns proc_id);

/**
 * @brief Reset Shadow FTQ (on recovery or initialization)
 * @param proc_id Core ID
 */
void tea_shadow_ftq_reset(uns proc_id);

/**************************************************************************************/
/* FT Insertion - Called from decoupled_frontend.cc at BP prediction time */

/**
 * @brief Insert a new FT into Shadow FTQ (called when BP pushes to Main FTQ)
 * @param proc_id Core ID
 * @param main_ft_ptr Pointer to Main FT being pushed (void* for C/C++ compatibility)
 * 
 * This function clones each Op in the Main FT into TEA Ops, setting:
 * - tea_main_op_link: pointer to Main Op
 * - 100% branch prediction (oracle_info.pred = oracle_info.dir)
 */
void tea_shadow_ft_insert(uns proc_id, void* main_ft_ptr);

/**
 * @brief Add a single cloned Op to the current Shadow FT
 * @param proc_id Core ID
 * @param main_op Main Op to clone and add
 * 
 * Called from decoupled_frontend.cc for each Op in a Main FT.
 */
void tea_shadow_ft_add_op(uns proc_id, Op* main_op);

/**
 * @brief Notify Shadow FTQ that a new FT is about to start
 * @param proc_id Core ID
 * 
 * Called before adding Ops to prepare a new Shadow FT slot.
 */
void tea_shadow_ft_start(uns proc_id);

/**************************************************************************************/
/* FT Consumption - Called from TEA Decode stage */

/**
 * @brief Peek at the head FT without consuming
 * @param proc_id Core ID
 * @return Pointer to head FT, or NULL if empty
 */
TEA_FT* tea_shadow_ftq_peek(uns proc_id);

/**
 * @brief Consume (pop) the head FT
 * @param proc_id Core ID
 */
void tea_shadow_ftq_consume(uns proc_id);

/**
 * @brief Get number of FTs in Shadow FTQ
 * @param proc_id Core ID
 * @return Number of FTs
 */
uns tea_shadow_ftq_count(uns proc_id);

/**************************************************************************************/
/* Recovery */

/**
 * @brief Recover Shadow FTQ (flush all entries, free TEA Ops)
 * @param proc_id Core ID
 * 
 * Called on Main thread recovery to synchronize Shadow FTQ state.
 */
void tea_shadow_ftq_recover(uns proc_id);

/**
 * @brief Frontend-only partial flush (논문 Section IV-F)
 * @param proc_id Core ID
 * @param mispred_op The mispredicting Main Op
 * 
 * "Partially Flushing the Frontend: when the TEA thread runs so far ahead that
 * the main thread branch being flushed is in the frontend."
 * 
 * This function:
 * 1. Flushes Shadow FTQ entries younger than mispred_op
 * 2. Does NOT restore backend checkpoint
 * 3. Does NOT call bp_sched_recovery (avoids full pipeline flush)
 * 4. Returns TRUE if partial flush was performed
 */
Flag tea_frontend_partial_flush(uns proc_id, Op* mispred_op);

/**************************************************************************************/
/* Op Cloning Helper */

/**
 * @brief Clone a Main Op into a TEA Op with 100% prediction
 * @param proc_id Core ID
 * @param main_op Main Op to clone
 * @return Cloned TEA Op with tea_main_op_link set
 * 
 * Sets:
 * - tea_main_op_link = main_op
 * - tea_main_op_num = main_op->op_num
 * - tea_main_unique_num = main_op->unique_num
 * - oracle_info.pred = oracle_info.dir (100% accurate)
 * - oracle_info.pred_npc = oracle_info.npc
 * - mispred/misfetch/recover flags = FALSE
 */
Op* tea_clone_op_from_main(uns proc_id, Op* main_op);

/**************************************************************************************/

#ifdef __cplusplus
}
#endif

#endif /* __TEA_DECOUPLED_FRONTEND_H__ */
