#include "bp/hbt.h"  
#include "globals/global_types.h" 
#include "op.h"                
#include "bp/bp_conf.h"         
#include "debug/debug.param.h"
#include "debug/debug_macros.h"
#include "globals/utils.h"        
#include "globals/global_vars.h"
#include <string.h>               

// .h 파일에 extern으로 선언된 전역 변수들의 실체를 정의
HbtEntry hbt_table[HBT_SIZE];
uns64    retired_branch_count = 0;

// HBT의 모든 카운터를 주기적으로 1씩 감소시키는 내부 함수
static void hbt_periodic_decrement(void) {
  for (int i = 0; i < HBT_SIZE; i++) {
    // 카운터가 0이 아니면 1만큼 감소 (Saturating at 0)
    if (hbt_table[i].counter > 0) {
      hbt_table[i].counter--;
    }
  }
}


// HBT 모듈 초기화 함수
void hbt_init(void) {
  // HBT 테이블 전체를 0으로 초기화
  memset(hbt_table, 0, sizeof(HbtEntry) * HBT_SIZE);
  retired_branch_count = 0;
  _DEBUG(0, DEBUG_HBT, "HBT module initialized.\n");
}

/**
 * @brief HBT 상태를 업데이트하는 함수. Branch가 retire될 때마다 호출됩니다.
 * @param op retire되는 브랜치 명령어 정보
 */
void hbt_update(Op* op) {
  // 1. 필요한 정보 추출: 브랜치 주소(PC)와 예측 실패 여부
  Addr pc      = op->inst_info->addr;
  Flag mispred = op->oracle_info.mispred | op->oracle_info.misfetch;

  // 2. HBT 테이블의 인덱스와 태그 계산
  uns32 index = pc % HBT_SIZE;
  uns64 tag   = pc / HBT_SIZE;
  HbtEntry* entry = &hbt_table[index];

  _DEBUG(0, DEBUG_HBT, "OpNum=%llu | HBT Update: PC=0x%llx, Mispred=%u, Index=%u, Tag=%llu\n",
         op->op_num, pc, mispred, index, tag);
  // 3. HBT Entry 탐색 및 할당
  // 현재 entry의 태그가 찾는 브랜치의 태그와 다른 경우
  if (entry->tag != tag) {
    _DEBUG(0, DEBUG_HBT, "OpNum=%llu | Allocating new entry at index %u for tag %llu (was tag %llu)\n",
          op->op_num, index, tag, entry->tag);
    // 카운터가 0이라면, 이 entry는 다른 브랜치가 사용하지 않는 '빈 공간'임
    // 따라서 이 공간을 현재 브랜치를 위해 새로 할당 (overwrite)
    if (entry->counter == 0) {
      entry->tag = tag;
      entry->counter = 0; // 카운터는 0으로 초기화
    } else {
      _DEBUG(0, DEBUG_HBT, "OpNum=%llu | Skipping update: index %u is occupied by tag %llu with counter %u\n",
      op->op_num, index, entry->tag, entry->counter);
      // 다른 'Hard' 브랜치가 아직 사용 중인 공간이므로, 이번에는 업데이트하지 않고 넘어감
      return; 
    }
  }

  // 4. 예측 실패 시 카운터 업데이트
  // entry->tag == tag 인 경우, 즉 현재 브랜치가 올바른 entry를 찾았을 때만 아래 로직 수행
  if (mispred) {
    _DEBUG(0, DEBUG_HBT, "OpNum=%llu | Mispredicted branch at index %u (tag %llu): Before counter %u\n",
          op->op_num, index, tag, entry->counter);
    
    // If newly allocated (counter was 0), initialize to 1.
    // Otherwise, increment.
    if (entry->counter == 0) {
        entry->counter = 1;
    } else {
        entry->counter = SAT_INC(entry->counter, HBT_CTR_MAX);
    }

    _DEBUG(0, DEBUG_HBT, "OpNum=%llu | Mispredicted branch at index %u (tag %llu): After counter %u\n",
           op->op_num, index, tag, entry->counter);
  }
  // ※ 예측 성공 시에는 아무것도 하지 않는 것이 HBT의 핵심 로직입니다.

  // 5. 주기적 감소 로직 트리거
  // 50k instructions마다 1씩 감소 (0.02 MPKI threshold)
  // inst_count는 전역 변수로 총 retired instruction 수를 나타냄 (global_vars.h)
  static Counter last_decay_inst_count = 0;
  if (*inst_count >= last_decay_inst_count + 50000) {
     _DEBUG(0, DEBUG_HBT, "Triggering periodic decrement at inst count = %llu\n", *inst_count);
    hbt_periodic_decrement();
    last_decay_inst_count = *inst_count;
  }
}

/**
 * @brief 특정 브랜치가 'Hard-to-predict' 상태인지 외부 모듈에 알려주는 함수
 * @param pc 확인할 브랜치의 주소
 * @return 'Hard' 상태이면 TRUE, 아니면 FALSE
 */
Flag hbt_is_hard_branch(Addr pc) {
  uns32 index = pc % HBT_SIZE;
  uns64 tag   = pc / HBT_SIZE;
  HbtEntry* entry = &hbt_table[index];

  // 해당 entry의 주인이 맞고(tag 일치), 카운터가 1보다 크면 'Hard'로 판단
  if (entry->tag == tag && entry->counter > 1) {
    return TRUE;
  }
  return FALSE;
}

uns32 hbt_get_counter(Addr pc) {
  uns32 index = pc % HBT_SIZE;
  uns64 tag   = pc / HBT_SIZE;
  HbtEntry* entry = &hbt_table[index];
  if (entry->tag == tag) {
    return entry->counter;
  } else {
    return 0; // mismatch면 0 반환
  }
}