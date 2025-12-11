#ifndef __TEA_RENAME_H__
#define __TEA_RENAME_H__

#include "globals/global_types.h"
#include "globals/assert.h"
#include "core.param.h"          // MAX_ARCH_REGS 등 core 파라미터
#include "dependency_chain_cache.h" // MAX_CHAIN_LENGTH 사용
#include "op.h"

// [TEA 구현 지침]
// 별도의 TEA 파이프라인을 위해 독립적인 Rename 구조체를 정의한다.
// 메인 파이프라인과 공유하지 않는 TEA 전용 리소스를 정의한다.

// Scarab의 MAX_ARCH_REGS 사용 (x86-64 기준 보통 64~128)
#define TEA_MAX_ARCH_REGS   MAX_ARCH_REGS   
// TEA 전용 물리 레지스터 개수 (논문에 따라 충분히 크게 잡음)
#define TEA_MAX_PHYS_REGS   256             
#define TEA_PHYS_INVALID    (-1)
// 동시에 처리 가능한 TEA uOP의 최대 개수 (Issue Queue 크기)
#define TEA_MAX_NODE        MAX_CHAIN_LENGTH    

/**
 * @brief TEA 전용 물리 레지스터 상태 정보
 */
typedef struct {
    Flag valid;       /**< 데이터가 준비되었는지 여부 (Live-in은 즉시 TRUE) */
    Counter ready_cycle; /**< [TEA-Phase2] 데이터가 준비되는 시점 (Cycle) */
    uns  refcount;    /**< 해당 PR을 소스로 참조하는 카운트 (Free 시점 판단용) */
    Op* producer_op;  /**< (디버깅용) 값을 생성한 TEA Op. Live-in인 경우 NULL일 수 있음 */
} TEA_PhysReg_Info;

/**
 * @brief TEA uOP별 Rename 메타데이터
 * 원래 Op 구조체를 건드리지 않고 별도로 저장하여 메인 스레드 영향을 최소화
 */
typedef struct {
    Flag valid;
    Op* op;
    int  src_phys_id[MAX_SRCS];
    Flag src_ready[MAX_SRCS];
    int  dst_phys_id[MAX_DESTS];
    uns  num_src;
    uns  num_dst;
    Op*  main_op;  // [TEA Early Binding] FTQ\uc5d0\uc11c \ucc3e\uc740 Main Op (Fetch \uc2dc\uc810 \uc5f0\uacb0)
} TEA_Op_Metadata;

/**
 * @brief TEA Rename 단계의 핵심 상태 (Shadow RAT + Free List + PRF)
 */
typedef struct TEA_Rename_State_struct {
    // Shadow RAT: Arch Reg ID -> TEA Phys Reg ID
    int   map_table[TEA_MAX_ARCH_REGS];      
    Flag  map_valid[TEA_MAX_ARCH_REGS];      
    
    // TEA 전용 물리 레지스터 파일 (값은 저장하지 않고 상태만 관리)
    TEA_PhysReg_Info phys_regs[TEA_MAX_PHYS_REGS]; 
    
    // Free List 관리
    int   free_list[TEA_MAX_PHYS_REGS];
    int   free_count;
    
    // Op 메타데이터 풀 (동적 할당 부하를 줄이기 위한 풀링)
    TEA_Op_Metadata meta_pool[TEA_MAX_NODE];
    Flag  meta_in_use[TEA_MAX_NODE];

    Flag  shadow_synced; /**< 현재 스냅샷이 메인 스레드와 동기화 되었는지 여부 */
} TEA_Rename_State;

// 전역 포인터 (tea_rename.c에서 할당)
extern TEA_Rename_State* tea_rename_state;

// TEA Issue Queue Entry
typedef struct {
    Op* op;
    TEA_Op_Metadata* meta;
} TEA_Issue_Entry;

// TEA Issue Queue (Circular Buffer)
typedef struct TEA_Issue_Queue_struct {
    TEA_Issue_Entry entries[TEA_MAX_NODE];
    int head;
    int tail;
    int count;
} TEA_Issue_Queue;

extern TEA_Issue_Queue* tea_issue_queue;

// 함수 프로토타입
void tea_init_rename(uns proc_id);
void tea_rename_stage(uns proc_id);
Flag tea_issue_queue_is_full(uns proc_id);
TEA_Op_Metadata* tea_meta_alloc(TEA_Rename_State* rs);
void tea_meta_release(TEA_Rename_State* rs, TEA_Op_Metadata* meta);

/**
 * @brief TEA Op의 Metadata를 가져옴
 * @param op TEA Op
 * @return TEA_Op_Metadata 포인터, 없으면 NULL
 */
TEA_Op_Metadata* tea_get_op_metadata(Op* op);

#endif /* __TEA_RENAME_H__ */
