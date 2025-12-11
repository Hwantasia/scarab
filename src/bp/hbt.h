#ifndef __HBT_H__
#define __HBT_H__

#include "globals/global_types.h" // Addr, uns, Flag 등 기본 타입
#include "op.h"                   // Op 구조체

// ==========================================================
// HBT (Hard Branch Table) 관련 상수(매크로) 정의
// ==========================================================
#define HBT_SIZE 1024         // HBT 테이블의 전체 크기
#define HBT_CTR_BITS 3        // HBT 카운터의 비트 수
#define HBT_CTR_MAX ((1 << HBT_CTR_BITS) - 1) // HBT 카운터의 최댓값 (31)

// ==========================================================
// HBT 자료구조 정의
// ==========================================================
typedef struct {
  uns64 tag;
  uns32 counter;
} HbtEntry;

// ==========================================================
// 전역 변수 선언 (extern)
// 변수의 실체는 hbt.c에 있고, 여기서는 '이런 변수가 외부에 있다'고 알려주는 역할만 합니다.
// ==========================================================
extern HbtEntry hbt_table[HBT_SIZE];
extern uns64    retired_branch_count;

// ==========================================================
// 외부 공개 함수 원형(Prototype) 선언
// 다른 파일에서 호출할 수 있는 함수 목록입니다.
// ==========================================================

/**
 * @brief HBT 모듈을 초기화합니다. 시뮬레이션 시작 시 한 번 호출됩니다.
 */
void hbt_init(void);

/**
 * @brief 브랜치가 retire될 때 HBT의 상태를 업데이트합니다.
 * @param op Retire되는 브랜치 명령어 정보
 */
void hbt_update(Op* op);

/**
 * @brief 특정 주소(PC)의 브랜치가 'Hard-to-predict' 상태인지 확인합니다.
 * @param pc 확인할 브랜치의 주소
 * @return 'Hard' 상태이면 TRUE, 아니면 FALSE
 */
Flag hbt_is_hard_branch(Addr pc);


uns32 hbt_get_counter(Addr pc);

#endif /* #ifndef __HBT_H__ */