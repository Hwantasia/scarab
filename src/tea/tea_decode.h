#ifndef __TEA_DECODE_H__
#define __TEA_DECODE_H__

#include "globals/global_types.h"
#include "op.h"
#include "tea_fetch.h"    // TEA_Fetch_Queue and context
#include "globals/assert.h"

// 주의: ISSUE_WIDTH/NUM_CORES는 런타임 파라미터(변수)이므로
// 컴파일 타임 상수로 배열 크기에 사용할 수 없습니다.

// TEA Decode 단계 출력 버퍼 (Rename 단계로 넘길 준비가 된 uop 목록)
typedef struct TEA_Decode_Buffer_struct {
    Op** ops;      /**< Decode된 uop들의 포인터 배열(동적) */
    int  num_ops;  /**< 현재 버퍼에 있는 uop 개수 */
    int  capacity; /**< ops 배열 용량(일반적으로 ISSUE_WIDTH) */
} TEA_Decode_Buffer;

// 디코드 버퍼는 구현 파일에서 NUM_CORES 크기로 동적 할당됩니다.
extern TEA_Decode_Buffer* tea_decode_buffer;

// 함수 프로토타입
void tea_decode_stage(uns proc_id);
// 필요 시 초기화를 명시적으로 호출할 수 있도록 인터페이스도 제공합니다.
void tea_decode_init_if_needed(void);

#endif /* __TEA_DECODE_H__ */
