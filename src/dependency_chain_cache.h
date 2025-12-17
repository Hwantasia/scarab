#ifndef __DEPENDENCY_CC_H__
#define __DEPENDENCY_CC_H__

#include "globals/global_types.h"
#include "op.h"
#include "table_info.h"
#include <stdbool.h>

// =================================================================
// 상수 정의
// =================================================================
#define DEPENDENCY_CHAIN_CACHE_SIZE 1024
#define BLOCK_CACHE_SIZE            1024 // 새로 추가된 블록 캐시 크기
#define EMPTY_BLOCK_TAG_STORE_SIZE    256
#define MAX_CHAIN_LENGTH            64   // 체인의 최대 길이
#define MAX_LIVE_INS                32   // Live-in 목록의 최대 크기
// =================================================================
// 자료구조 정의
// =================================================================

#define MAX_ARCH_REGS                 64  // 추적할 아키텍처 레지스터 수
#define MAX_MEM_LIVE_INS              16  // [논문 반영] 메모리 의존성 버퍼 크기

typedef struct SourceList_struct {
    uint64_t reg_vector;                  // [논문 반영] Register Bit Vector
    Addr     addrs[MAX_MEM_LIVE_INS];     // [논문 반영] 16-entry 메모리 버퍼
    uns      addr_count;
} SourceList;

// An entry in the dependency chain cache
typedef struct Dependency_Chain_Cache_Entry_struct {
  Flag          is_valid;
  Addr          h2p_branch_pc;
  Counter       h2p_branch_op_num;
  uns           chain_length;
  Op            chain[MAX_CHAIN_LENGTH];
  uns           chain_pos[MAX_CHAIN_LENGTH];  // Option B: 원래 블록 내 위치 저장
  uint64_t     dependency_mask;     // 기본 블록 내 의존성 비트마스크
  uns          total_ops_in_block;  // 마스크와 함께 사용할 블록의 총 명령어 수
} Dependency_Chain_Cache_Entry;

typedef struct Block_Cache_Tag_Entry_struct {
    Flag is_valid;
    Addr block_start_pc;
} Block_Cache_Tag_Entry;

typedef enum {
    BW_IDLE,    // 유휴 상태
    BW_WALKING  // 분석 진행 중
} Backward_Walk_State;

typedef struct Backward_Walk_Engine_struct {
    Backward_Walk_State state;
    Counter             walk_cycles_remaining;
    Op* snapshot_buffer; // Changed from array to pointer
    int                 snapshot_op_count;
} Backward_Walk_Engine;

// 전역 변수 선언
extern Backward_Walk_Engine** bw_engines;

// =================================================================
// Function prototypes (원형) 선언
// =================================================================
void init_dependency_chain_cache(uns proc_id);
void reset_dependency_chain_cache(uns proc_id);
void add_dependency_chain(uns proc_id, Op* snapshot_buffer, int op_count);
void periodically_reset_caches(uns proc_id);
void cycle_backward_walk_engine(uns proc_id); 
Dependency_Chain_Cache_Entry* get_dependency_chain(uns proc_id, Addr pc);
Dependency_Chain_Cache_Entry* get_dependency_chain_block(uns proc_id, Addr pc);
Flag is_empty_block_tag_hit(uns proc_id, Addr block_start_pc);
extern Dependency_Chain_Cache_Entry** dependency_chain_caches;
extern Dependency_Chain_Cache_Entry** block_caches;

#endif // __DEPENDENCY_CHAIN_CACHE_H__
