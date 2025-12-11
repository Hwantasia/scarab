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
 * File         : ftq_search.h
 * Author       : TEA Implementation
 * Date         : 2025
 * Description  : FTQ search interface for TEA-Main Op Early Binding
 ***************************************************************************************/

#ifndef __FTQ_SEARCH_H__
#define __FTQ_SEARCH_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "globals/global_types.h"
#include "op.h"

/**
 * @brief FTQ에서 Main Op를 검색 (Early Binding용)
 * @param proc_id 코어 ID
 * @param pc 검색할 PC
 * @param cf_type 분기 타입
 * @return 매칭되는 Main Op, 없으면 NULL
 */
Op* find_op_in_ftq(uns proc_id, Addr pc, Cf_Type cf_type);

#ifdef __cplusplus
}
#endif

#endif /* __FTQ_SEARCH_H__ */
