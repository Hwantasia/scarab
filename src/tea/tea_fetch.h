#ifndef __TEA_FETCH_H__
#define __TEA_FETCH_H__

#include "globals/global_types.h"   // for Addr, uns, Flag types
#include "op.h"                    // for Op struct definition
#include "dependency_chain_cache.h"// for get_dependency_chain() and get_dependency_chain_block()
#include "bp/bp.h"                 // for branch predictor (if needed for fetch target)
#include "globals/assert.h"        // for ASSERT macro
#include "core.param.h"

static inline Flag tea_is_enabled(void) {
    return TEA_ENABLE;
}

static inline uns tea_configured_width(void) {
    return (TEA_WIDTH > 0) ? TEA_WIDTH : ISSUE_WIDTH;
}

static inline uns tea_rat_sync_latency_cycles(void) {
    return TEA_RAT_SYNC_LAT;
}

// 최대 TEA 페치 큐 크기 (한 번에 저장할 수 있는 명령어 수)
// - 기본적으로 최대 의존 체인 길이와 동일하게 설정
#define TEA_FETCH_QUEUE_SIZE   MAX_CHAIN_LENGTH

// TEA Fetch Queue 자료구조 정의
typedef struct TEA_Fetch_Queue_struct {
    Op*             entries[TEA_FETCH_QUEUE_SIZE];  // TEA 페치 큐의 명령어 포인터 배열
    int             head;       // 큐의 시작 인덱스
    int             tail;       // 큐의 끝 다음 인덱스
    int             count;      // 현재 큐에 있는 명령어 개수
    char            *name;      // 디버깅용 이름 (예: "TEA_Fetch_Queue")
} TEA_Fetch_Queue;

typedef enum {
    TEA_STATE_DISABLED = 0,   // 전역적으로 꺼진 상태
    TEA_STATE_IDLE,           // 활성화 대기
    TEA_STATE_ACTIVE,         // 체인 활성
    TEA_STATE_DRAINING        // 잔여 uop 비우는 중
} TEA_Thread_State;

// 전역 변수 선언 (각 코어별 TEA 상태)
typedef struct TEA_Context_struct {
    TEA_Fetch_Queue fetch_queue;   // TEA 전용 fetch 버퍼
    TEA_Thread_State state;        // 상태 머신
    Flag            shadow_rat_needs_sync; // 새 체인을 위해 Shadow RAT 동기화 필요
    Flag            terminated_by_miss;   // Block Cache miss로 인한 terminate/drain 중 (새 fetch 금지)
    Addr            trigger_pc;    // 현재 체인을 기동한 분기 PC
    Addr            last_fetch_pc; // 가장 최근에 enque한 fetch PC (디버깅용)
    Counter         blocks_fetched;// 누적 block enqueue 수 (리소스 모니터링용)
    Counter         ops_enqueued;  // 누적 uop enqueue 수
    Counter         activated_cycle; // 가장 최근 활성화된 사이클
    Counter         last_progress_cycle; // 마지막으로 큐가 채워진 사이클
    Counter         rat_sync_ready_cycle; // Shadow RAT 동기화가 완료되는 사이클
} TEA_Context;
extern TEA_Context** tea_contexts;                   // 각 코어별 TEA 컨텍스트 포인터 배열

// 함수 프로토타입 선언
#ifdef __cplusplus
extern "C" {
#endif
void tea_init(uns proc_id);
void tea_fetch_stage(uns proc_id, Addr current_fetch_addr);
Flag tea_thread_is_active(uns proc_id);
Flag tea_thread_needs_rat_sync(uns proc_id);
void tea_thread_ack_sync(uns proc_id);
void tea_reset_fetch_queue(TEA_Fetch_Queue* fq);
#ifdef __cplusplus
}
#endif

#endif /* __TEA_FETCH_H__ */
