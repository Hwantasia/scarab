#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tea_fetch_log.h"
#include "globals/global_vars.h"
#include "globals/utils.h"
#include "debug/debug.param.h"

static FILE* tea_fetch_log_file = NULL;
extern char* OUTPUT_DIR;

void close_tea_fetch_log(void) {
    if (tea_fetch_log_file) {
        fclose(tea_fetch_log_file);
        tea_fetch_log_file = NULL;
    }
}

void init_tea_fetch_log(void) {
    if (tea_fetch_log_file == NULL) {
        tea_fetch_log_file = file_tag_fopen(OUTPUT_DIR, "tea_fetch", "w");
        if (tea_fetch_log_file == NULL) {
            perror("Error opening tea_fetch.log in output directory");
        } else {
            /* Write header */
            fprintf(tea_fetch_log_file,
                "# TEA Fetch & Block Cache Lookup Log\n"
                "#\n"
                "# This log tracks Block Cache lookups to diagnose miss problems\n"
                "#\n"
                "# === BLOCK CACHE LOOKUP NOTE ===\n"
                "# Block Cache is tagged with block START PC.\n"
                "# TEA Shadow-FTQ path performs lookup at the block start, then\n"
                "# reuses that entry until the next CF terminator (new block).\n"
                "#\n"
                "# Format for Shadow FTQ lookups:\n"
                "# [SHADOW] Cycle PC FT_idx/FT_count Hit Block_Start_PC PC_Offset Mask InChain\n"
                "#   - PC: Current uop PC (for context)\n"
                "#   - FT_idx/FT_count: Op position within FT (e.g., 3/8)\n"
                "#   - Hit: 1 if the current block has a valid Block Cache entry\n"
                "#   - Block_Start_PC: h2p_branch_pc from cache entry (block start)\n"
                "#   - PC_Offset: PC - block_start_pc (shows position in block)\n"
                "#   - Mask: dependency_mask value (hex)\n"
                "#   - InChain: 1 if Op passed filtering, 0 if filtered out\n"
                "#\n"
                "# Format for Fallback lookups:\n"
                "# [FALLBACK] Cycle Fetch_PC Hit Block_Start_PC PC_Offset\n"
                "#\n\n");
            fflush(tea_fetch_log_file);
            atexit(close_tea_fetch_log);
        }
    }
}

void log_tea_block_cache_lookup(uns proc_id, Counter cycle_count,
                                  Addr lookup_pc,
                                  Dependency_Chain_Cache_Entry* block_entry,
                                  uns ft_index, uns ft_op_count,
                                  Flag in_chain) {
    if (!tea_fetch_log_file) {
        return;
    }
    
    /* Only log within debug cycle range */
    if (cycle_count < DEBUG_CYCLE_START || cycle_count > DEBUG_CYCLE_STOP) {
        return;
    }
    
    Flag hit = (block_entry && block_entry->is_valid) ? 1 : 0;
    Addr block_start_pc = 0;
    long long pc_offset = 0;
    uint64_t dep_mask = 0;
    
    if (hit) {
        block_start_pc = block_entry->h2p_branch_pc;
        pc_offset = (long long)lookup_pc - (long long)block_start_pc;
        dep_mask = block_entry->dependency_mask;
    }
    
    fprintf(tea_fetch_log_file,
        "[SHADOW] C:%llu PC:0x%llx FT:%u/%u Hit:%d BlockStart:0x%llx Offset:%lld Mask:0x%llx InChain:%d\n",
        (unsigned long long)cycle_count,
        (unsigned long long)lookup_pc,
        ft_index,
        ft_op_count,
        hit,
        (unsigned long long)block_start_pc,
        pc_offset,
        (unsigned long long)dep_mask,
        in_chain
    );
    
    fflush(tea_fetch_log_file);
}

void log_tea_fallback_fetch(uns proc_id, Counter cycle_count,
                             Addr fetch_addr,
                             Dependency_Chain_Cache_Entry* block_entry) {
    if (!tea_fetch_log_file) {
        return;
    }
    
    /* Only log within debug cycle range */
    if (cycle_count < DEBUG_CYCLE_START || cycle_count > DEBUG_CYCLE_STOP) {
        return;
    }
    
    Flag hit = (block_entry && block_entry->is_valid) ? 1 : 0;
    Addr block_start_pc = 0;
    long long pc_offset = 0;
    
    if (hit) {
        block_start_pc = block_entry->h2p_branch_pc;
        pc_offset = (long long)fetch_addr - (long long)block_start_pc;
    }
    
    fprintf(tea_fetch_log_file,
        "[FALLBACK] C:%llu FetchPC:0x%llx Hit:%d BlockStart:0x%llx Offset:%lld\n",
        (unsigned long long)cycle_count,
        (unsigned long long)fetch_addr,
        hit,
        (unsigned long long)block_start_pc,
        pc_offset
    );
    
    fflush(tea_fetch_log_file);
}
