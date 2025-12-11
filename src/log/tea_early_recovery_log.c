#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tea_early_recovery_log.h"
#include "globals/global_vars.h"
#include "globals/utils.h"
#include "op.h"
#include "debug/debug_print.h"
#include "debug/debug.param.h"

static FILE* tea_early_recovery_log_file = NULL;
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

void close_tea_early_recovery_log(void) {
    if (tea_early_recovery_log_file) {
        fclose(tea_early_recovery_log_file);
        tea_early_recovery_log_file = NULL;
    }
}

void init_tea_early_recovery_log(void) {
    if (tea_early_recovery_log_file == NULL) {
        tea_early_recovery_log_file = file_tag_fopen(OUTPUT_DIR, "tea_early_recovery", "w");
        if (tea_early_recovery_log_file == NULL) {
            perror("Error opening tea_early_recovery.log in output directory");
        } else {
            /* Write header */
            fprintf(tea_early_recovery_log_file, 
                "# TEA Early Recovery Log\n"
                "# Format: [Cycle] PC MainUnique MainOpNum TEAUnique TEAOpNum State Saved Bind FrontendOnly PCMatch ExecCycleMain ExecCycleTEA\n"
                "# State: FETCHED=0,IN_ROB=1,IN_RS=2,SLEEP=3,WAIT_FWD=4,LOW_PRIORITY=5,READY=6,TENTATIVE=7,SCHEDULED=8,MISS=9,WAIT_DCACHE=10,WAIT_MEM=11,DONE=12\n"
                "# Bind: Binding method used to match TEA Op to Main Op\n"
                "# FrontendOnly: 1 if frontend-only recovery, 0 if full recovery\n"
                "# PCMatch: 1 if PCs match, 0 if mismatch\n"
                "# Saved: Cycles saved by early recovery\n\n");
            fflush(tea_early_recovery_log_file);
            atexit(close_tea_early_recovery_log);
        }
    }
}

void log_tea_early_recovery(uns proc_id, Counter cycle_count, 
                             Op* main_branch, Op* tea_op,
                             const char* binding_type, Flag is_frontend_only) {
    if (!tea_early_recovery_log_file) {
        return;
    }
    
    /* Calculate saved cycles */
    Counter saved_cycles = 0;
    Counter tea_exec_cycle = tea_op->exec_cycle;
    
    if (tea_exec_cycle != MAX_CTR && 
        main_branch->exec_cycle != MAX_CTR && 
        main_branch->exec_cycle > tea_exec_cycle) {
        saved_cycles = main_branch->exec_cycle - tea_exec_cycle;
    }
    
    /* Check PC match */
    Flag pc_match = (main_branch->inst_info && tea_op->inst_info && 
                     main_branch->inst_info->addr == tea_op->inst_info->addr) ? 1 : 0;
    
    /* Log detailed information */
    fprintf(tea_early_recovery_log_file,
        "[C:%llu] PC:0x%llx MainU:%llu MainO:%llu TEAU:%llu TEAO:%llu "
        "State:%d(%s) Saved:%lld Bind:%s FO:%d PCMatch:%d "
        "ExecM:%llu ExecT:%llu\n",
        (unsigned long long)cycle_count,
        (main_branch->inst_info) ? (unsigned long long)main_branch->inst_info->addr : 0ULL,
        (unsigned long long)main_branch->unique_num,
        (unsigned long long)main_branch->op_num,
        (unsigned long long)tea_op->unique_num,
        (unsigned long long)tea_op->op_num,
        main_branch->state,
        get_state_name(main_branch->state),
        (long long)saved_cycles,
        binding_type,
        is_frontend_only,
        pc_match,
        (main_branch->exec_cycle != MAX_CTR) ? (unsigned long long)main_branch->exec_cycle : 0ULL,
        (tea_exec_cycle != MAX_CTR) ? (unsigned long long)tea_exec_cycle : 0ULL
    );
    
    fflush(tea_early_recovery_log_file);
}
