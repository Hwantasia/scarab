#ifndef __TEA_RENAME_H__
#define __TEA_RENAME_H__

#include "globals/global_types.h"
#include "op.h"
#include "tea_decode.h"
#include "globals/assert.h"

// 최대 아키텍처 레지스터 개수와 최대 물리 레지스터 개수 (시뮬레이터 파라미터 기반)
// Scarab에서 아키텍처 레지스터는 64개 (예: x86-64)로 가정하고, 물리 레지스터는 그 몇 배 (예: 256)로 설정
#define TEA_MAX_ARCH_REGS   MAX_ARCH_REGS   /**< 아키텍처 레지스터 수 (Fill Buffer 추적 한계와 동일) */
#define TEA_MAX_PHYS_REGS   256             /**< TEA 전용 shadow PR 개수 */
#define TEA_PHYS_INVALID    (-1)
#define TEA_MAX_NODE        MAX_CHAIN_LENGTH    /**< 동시에 활발히 존재 가능한 TEA uop 상한 */

typedef struct {
    Flag valid;       /**< 최신 값을 보유하는지 */
    uns  refcount;    /**< 참조 중인 소스 개수 */
} TEA_PhysReg_Info;

typedef struct {
    Flag valid;
    Op*  op;
    int  src_phys_id[MAX_SRCS];
    Flag src_ready[MAX_SRCS];
    int  dst_phys_id[MAX_DESTS];
    uns  num_src;
    uns  num_dst;
} TEA_Op_Metadata;

// Shadow RAT (Register Alias Table) 및 관련 구조 정의
typedef struct TEA_Rename_State_struct {
    int   map_table[TEA_MAX_ARCH_REGS];      /**< 각 아키텍처 레지스터의 현재 물리 레지스터 매핑 */
    Flag  map_valid[TEA_MAX_ARCH_REGS];      /**< 해당 맵핑이 유효한지 (할당 여부 표시) */
    TEA_PhysReg_Info phys_regs[TEA_MAX_PHYS_REGS];
    int   free_list[TEA_MAX_PHYS_REGS];
    int   free_head;
    int   free_tail;
    int   free_count;
    Flag  shadow_synced;
    TEA_Op_Metadata meta_pool[TEA_MAX_NODE];
    Flag  meta_in_use[TEA_MAX_NODE];
} TEA_Rename_State;

// 런타임에 NUM_CORES 크기로 동적 할당
extern TEA_Rename_State* tea_rename_state;

// TEA Issue 큐 (Rename -> Issue 단계 사이에 보관되는 TEA uop들)
typedef struct {
    Op* op;
    TEA_Op_Metadata* meta;
} TEA_Issue_Entry;

typedef struct TEA_Issue_Queue_struct {
    TEA_Issue_Entry entries[TEA_MAX_NODE];
    int head;
    int tail;
    int count;
} TEA_Issue_Queue;
// 런타임에 NUM_CORES 크기로 동적 할당
extern TEA_Issue_Queue* tea_issue_queue;

void tea_init_rename(uns proc_id);
void tea_rename_stage(uns proc_id);

#endif /* __TEA_RENAME_H__ */
