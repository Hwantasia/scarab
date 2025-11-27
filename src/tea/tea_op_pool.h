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
 * File         : tea_op_pool.h
 * Author       : HPS Research Group
 * Date         : 11/25/2025
 * Description  : Header for tea_op_pool.c - Dedicated Op Pool for TEA threads
 ***************************************************************************************/

#ifndef __TEA_OP_POOL_H__
#define __TEA_OP_POOL_H__

#include "globals/global_types.h"
#include "op.h"

#ifdef __cplusplus
extern "C" {
#endif

/**************************************************************************************/
/* Global Variables */

extern uns tea_op_pool_entries;
extern uns tea_op_pool_active_ops;

/**************************************************************************************/
/* Prototypes */

void tea_init_op_pool(void);
void tea_reset_op_pool(void);
Op* tea_alloc_op(uns proc_id);
void tea_free_op(Op* op);

/**************************************************************************************/

#ifdef __cplusplus
}
#endif

#endif /* #ifndef __TEA_OP_POOL_H__ */
