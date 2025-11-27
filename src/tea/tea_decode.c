#include "tea_decode.h"
#include "tea_fetch.h"
#include "core.param.h"
#include "globals/global_vars.h"
#include <string.h>
#include <stdlib.h>

// 전역 버퍼 포인터(런타임에 NUM_CORES 크기로 할당)
TEA_Decode_Buffer* tea_decode_buffer = NULL;

static void ensure_decode_buffers_initialized(void) {
    if (tea_decode_buffer)
        return;

    tea_decode_buffer = (TEA_Decode_Buffer*)calloc(NUM_CORES, sizeof(TEA_Decode_Buffer));
    ASSERT(0, tea_decode_buffer);

    int width = (int)tea_configured_width();
    for (uns cid = 0; cid < NUM_CORES; ++cid) {
        tea_decode_buffer[cid].capacity = width;
        tea_decode_buffer[cid].num_ops = 0;
        tea_decode_buffer[cid].ops = (Op**)calloc(tea_decode_buffer[cid].capacity, sizeof(Op*));
        ASSERT(cid, tea_decode_buffer[cid].ops);
    }
}

void tea_decode_init_if_needed(void) { ensure_decode_buffers_initialized(); }

/**
 * @brief TEA Decode 스테이지 함수. Fetch 큐에서 명령어를 가져와 디코딩/변환하여 Rename 단계로 전달합니다.
 * @param proc_id 대상 코어 ID
 */
void tea_decode_stage(uns proc_id) {
    ASSERT(proc_id < NUM_CORES, "proc_id out of bounds\n");
    if (!tea_is_enabled()) {
        return;
    }
    ensure_decode_buffers_initialized();
    TEA_Decode_Buffer* dbuf = &tea_decode_buffer[proc_id];

    // 항상 버퍼를 깨끗이 초기화 -> REMOVED to prevent dropping unconsumed Ops
    // dbuf->num_ops = 0;
    // if (dbuf->capacity > 0) {
    //     memset(dbuf->ops, 0, sizeof(Op*) * (size_t)dbuf->capacity);
    // }

    if (!tea_contexts || !tea_contexts[proc_id]) {
        return;
    }

    TEA_Context* tctx = tea_contexts[proc_id];
    if (tctx->state == TEA_STATE_DISABLED || tctx->state == TEA_STATE_IDLE) {
        return;
    }

    TEA_Fetch_Queue* fq = &tctx->fetch_queue;
    int decode_width = (int)tea_configured_width();
    if (decode_width > dbuf->capacity) {
        decode_width = dbuf->capacity;
    }

    while (dbuf->num_ops < decode_width && fq->count > 0) {
        Op* op = fq->entries[fq->head];
        op->decode_cycle = cycle_count;
        dbuf->ops[dbuf->num_ops++] = op;

        fq->head = (fq->head + 1) % TEA_FETCH_QUEUE_SIZE;
        fq->count--;
    }
}
