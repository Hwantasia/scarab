#include <stdio.h>
#include <stdlib.h>
#include "tea_h2p_fetch_log.h"
#include "globals/global_vars.h"
#include "globals/utils.h"
#include "debug/debug.param.h"
#include "op.h"

static FILE* tea_h2p_fetch_log_file = NULL;
extern char* OUTPUT_DIR;

/* State name helper */
static const char* get_state_name(Op_State state) {
    switch(state) {
        case OS_FETCHED: return "FETCHED";
        case OS_IN_ROB: return "IN_ROB";
        case OS_IN_RS: return "IN_RS";
        case OS_SLEEP: return "SLEEP";
        case OS_WAIT_FWD: return "WAIT_FWD";
        case OS_LOW_PRIORITY: return "LOW_PRIORITY";
        case OS_READY: return "READY";
        case OS_TENTATIVE: return "TENTATIVE";
        case OS_SCHEDULED: return "SCHEDULED";
        case OS_MISS: return "MISS";
        case OS_WAIT_DCACHE: return "WAIT_DCACHE";
        case OS_WAIT_MEM: return "WAIT_MEM";
        case OS_DONE: return "DONE";
        default: return "UNKNOWN";
    }
}

void close_tea_h2p_fetch_log(void) {
    if (tea_h2p_fetch_log_file) {
        fclose(tea_h2p_fetch_log_file);
        tea_h2p_fetch_log_file = NULL;
    }
}

void init_tea_h2p_fetch_log(void) {
    if (tea_h2p_fetch_log_file == NULL) {
        tea_h2p_fetch_log_file = file_tag_fopen(OUTPUT_DIR, "tea_h2p_fetch", "w");
        if (tea_h2p_fetch_log_file == NULL) {
            perror("Error opening tea_h2p_fetch.out");
        } else {
            /* Write header */
            fprintf(tea_h2p_fetch_log_file,
                "# TEA H2P Branch Fetch Tracking Log\n"
                "#\n"
                "# This log tracks when H2P branches are fetched from TEA Fetch Queue\n"
                "# and shows where the corresponding Main Op is located at that moment\n"
                "#\n"
                "# Format: [Cycle] TEA_PC TEA_OpNum TEA_UniqueNum Main_State Main_OpNum Main_UniqueNum Lead_Cycles\n"
                "#   - TEA_PC: H2P branch PC in TEA thread\n"
                "#   - TEA_OpNum/UniqueNum: TEA Op identifiers\n"
                "#   - Main_State: Where Main Op is (FETCHED, IN_ROB, IN_RS, SCHEDULED, DONE, etc.)\n"
                "#   - Main_OpNum/UniqueNum: Main Op identifiers\n"
                "#\n\n");
            fflush(tea_h2p_fetch_log_file);
            atexit(close_tea_h2p_fetch_log);
        }
    }
}

void log_tea_h2p_fetch(uns proc_id, Counter cycle_count, Op* tea_op) {
    if (!tea_h2p_fetch_log_file) {
        return;
    }
    
    /* Only log within debug cycle range */
    if (cycle_count < DEBUG_CYCLE_START || cycle_count > DEBUG_CYCLE_STOP) {
        return;
    }
    
    /* Only log if this is an H2P branch */
    if (!tea_op->oracle_info.hbt_pred_is_hard) {
        return;
    }
    
    /* Only log if this is a branch */
    if (!tea_op->table_info || tea_op->table_info->cf_type == NOT_CF) {
        return;
    }
    
    /* Find corresponding Main Op via tea_main_op_link */
    Op* main_op = tea_op->tea_main_op_link;
    
    const char* main_state_str = "NOT_LINKED";
    Counter main_op_num = 0;
    Counter main_unique_num = 0;
    // lead_cycles removed for clarity; focus on matching & state

    if (main_op && main_op->op_pool_valid) {
        main_state_str = get_state_name(main_op->state);
        main_op_num = main_op->op_num;
        main_unique_num = main_op->unique_num;
        
    }
    
    fprintf(tea_h2p_fetch_log_file,
        "[C:%llu] TEA_PC:0x%llx TEA_OpNum:%llu TEA_Unique:%llu "
        "Main_State:%s Main_OpNum:%llu Main_Unique:%llu\n",
        (unsigned long long)cycle_count,
        (unsigned long long)tea_op->inst_info->addr,
        (unsigned long long)tea_op->op_num,
        (unsigned long long)tea_op->unique_num,
        main_state_str,
        (unsigned long long)main_op_num,
        (unsigned long long)main_unique_num
    );
    
    fflush(tea_h2p_fetch_log_file);
}
