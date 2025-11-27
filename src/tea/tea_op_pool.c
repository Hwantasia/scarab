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
 * File         : tea_op_pool.c
 * Author       : HPS Research Group
 * Date         : 11/25/2025
 * Description  : Dedicated Op Pool for TEA threads.
 *                Ensures TEA Ops have a stable lifetime separate from the main thread.
 ***************************************************************************************/

#include "tea_op_pool.h"

#include "globals/assert.h"
#include "globals/global_defs.h"
#include "globals/global_types.h"
#include "globals/global_vars.h"
#include "globals/utils.h"

#include "op_pool.h" // For op_pool_setup_op reuse if possible, or we implement our own

// TEA Op Pool Parameters
#define TEA_OP_POOL_ENTRIES_INC 128

/**************************************************************************************/
/* Global variables */

uns tea_op_pool_entries = 0;
uns tea_op_pool_active_ops = 0;
static Op* tea_op_pool_free_head;

/**************************************************************************************/
/* Prototypes */

static inline void tea_expand_op_pool(void);

/**************************************************************************************/
/* tea_init_op_pool: */

void tea_init_op_pool() {
    tea_reset_op_pool();
    tea_expand_op_pool();
}

/**************************************************************************************/
/* tea_reset_op_pool: */

void tea_reset_op_pool() {
    tea_op_pool_entries = 0;
    tea_op_pool_active_ops = 0;
    tea_op_pool_free_head = NULL;
    // Note: In a real reset scenario, we might want to free allocated blocks, 
    // but standard Scarab pools usually just reset counters or rely on OS cleanup at exit.
    // For now, we follow the simple reset pattern.
}

/**************************************************************************************/
/* tea_alloc_op: returns a pointer to the next available op */

Op* tea_alloc_op(uns proc_id) {
    Op* new_op;

    if (tea_op_pool_free_head == NULL) {
        tea_expand_op_pool();
    }

    new_op = tea_op_pool_free_head;
    ASSERT(0, !new_op->op_pool_valid);
    new_op->op_pool_valid = TRUE;

    // Use the main thread's setup function to initialize basic fields
    // This ensures compatibility with shared utility functions
    op_pool_setup_op(proc_id, new_op);

    // TEA-specific overrides can go here if needed
    // e.g., marking it as a TEA op explicitly if there's a flag for it

    tea_op_pool_active_ops++;
    tea_op_pool_free_head = new_op->op_pool_next;

    return new_op;
}

/**************************************************************************************/
/* tea_free_op: "frees" an op */

void tea_free_op(Op* op) {
    ASSERT(0, op);
    ASSERT(0, op->op_pool_valid);
    
    op->op_pool_valid = FALSE;
    tea_op_pool_active_ops--;

    // Clean up dynamic allocations if any (similar to free_op)
    if (op->sched_info) {
        free(op->sched_info);
        op->sched_info = NULL;
    }
    
    // Note: We don't call delete_store_hash_entry because TEA ops 
    // shouldn't be polluting the main thread's store hash.
    
    if (op->inst_info && op->inst_info->fake_inst) {
        free(op->inst_info);
        op->inst_info = NULL;
    }

    op->op_pool_next = tea_op_pool_free_head;
    tea_op_pool_free_head = op;
}

/**************************************************************************************/
/* tea_expand_op_pool: */

static inline void tea_expand_op_pool() {
    Op* new_pool = (Op*)calloc(TEA_OP_POOL_ENTRIES_INC, sizeof(Op));
    uns ii;

    for (ii = 0; ii < TEA_OP_POOL_ENTRIES_INC - 1; ii++) {
        new_pool[ii].op_pool_valid = FALSE;
        new_pool[ii].op_pool_next = &new_pool[ii + 1];
        new_pool[ii].op_pool_id = tea_op_pool_entries++;
        op_pool_init_op(&new_pool[ii]); // Reuse main init for basic fields
    }
    new_pool[ii].op_pool_valid = FALSE;
    new_pool[ii].op_pool_next = tea_op_pool_free_head;
    new_pool[ii].op_pool_id = tea_op_pool_entries++;
    op_pool_init_op(&new_pool[ii]);

    tea_op_pool_free_head = &new_pool[0];
}
