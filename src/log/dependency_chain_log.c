#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "dependency_chain_log.h"
#include "../globals/utils.h"
#include "../table_info.h" 
#include "../isa/isa.h" // NUM_REGS, Reg_Id 등을 위해 포함
#include "debug/debug.param.h"
#include "../debug/debug_print.h"

extern char* OUTPUT_DIR;
static FILE* dependency_chain_log_file = NULL;
static FILE* block_cache_log_file = NULL;

// ----- [ 개선사항 1: 레지스터 이름 배열 추가 ] -----
// disasm_reg와 동일한 방식으로 파일 내에 static 배열을 만듭니다.
#define REG(x) #x,
static const char* reg_names[NUM_REGS] = {
#include "../isa/x86_regs.def" // x86_regs.def 파일 경로에 맞게 수정
};
#undef REG

// 캐시된 Op 정보를 disasm하는 헬퍼 함수
static char* disasm_cached_op(Op* op) {
    static char buf[512];
    int i = 0;

    const char* opcode = Op_Type_str(op->table_info->op_type);
    if (op->table_info->op_type == OP_CF) {
        opcode = cf_type_names[op->table_info->cf_type];
    }
    i += sprintf(&buf[i], "%-8s ", opcode);

    for (int j = 0; j < op->table_info->num_dest_regs; j++) {
        i += sprintf(&buf[i], "r%u(%s)%s", op->inst_info->dests[j].id, reg_names[op->inst_info->dests[j].id], (j == op->table_info->num_dest_regs - 1) ? "" : ",");
    }
    if (op->table_info->num_dest_regs > 0 && op->table_info->num_src_regs > 0) {
        i += sprintf(&buf[i], " <- ");
    }
    for (int j = 0; j < op->table_info->num_src_regs; j++) {
        i += sprintf(&buf[i], "r%u(%s)%s", op->inst_info->srcs[j].id, reg_names[op->inst_info->srcs[j].id], (j == op->table_info->num_src_regs - 1) ? "" : ",");
    }
    if (op->table_info->mem_type == MEM_LD && op->oracle_info.mem_size > 0) {
      i += sprintf(&buf[i], " %d@%08llx", op->oracle_info.mem_size, op->oracle_info.va);
    }
    if (op->table_info->mem_type == MEM_ST && op->oracle_info.mem_size > 0) {
      i += sprintf(&buf[i], " %d@%08llx", op->oracle_info.mem_size, op->oracle_info.va);
    }
    
    buf[i] = '\0';
    return buf;
}

static void close_dependency_chain_log(void) {
    if (dependency_chain_log_file) {
        fclose(dependency_chain_log_file);
        dependency_chain_log_file = NULL; // 닫은 후 NULL로 설정
    }
    if (block_cache_log_file) {
        fclose(block_cache_log_file);
        block_cache_log_file = NULL; // 닫은 후 NULL로 설정
    }
}

void init_dependency_chain_log(void) {
    if (dependency_chain_log_file == NULL) {
        dependency_chain_log_file = file_tag_fopen(OUTPUT_DIR, "dependency_chain", "w");
        if (dependency_chain_log_file) atexit(close_dependency_chain_log);
    }
    if (block_cache_log_file == NULL) {
        block_cache_log_file = file_tag_fopen(OUTPUT_DIR, "block_cache", "w");
        if (block_cache_log_file) atexit(close_dependency_chain_log);
    }
}

void finalize_dependency_chain_log(void) {
    close_dependency_chain_log();
}

void log_dependency_chain_entry(uns proc_id, Dependency_Chain_Cache_Entry* entry, Counter cycle_count) {
    if (!dependency_chain_log_file || !entry || !entry->is_valid||
        !(cycle_count >= DEBUG_CYCLE_START && cycle_count <= DEBUG_CYCLE_STOP)) return;

    fprintf(dependency_chain_log_file, "--- [LOG] Dependency Chain for Core %u Cycle:%-4llu---\n", proc_id, cycle_count);
    fprintf(dependency_chain_log_file, "Index PC(H2P Branch PC): 0x%llx, OpNum: %llu, Chain Length: %u\n",
            entry->h2p_branch_pc, entry->h2p_branch_op_num, entry->chain_length);
    fprintf(dependency_chain_log_file, "------------------------------------------------------------------\n");

    for (int i = 0; i < entry->chain_length; ++i) {
        Op* op = &entry->chain[i];
        char* disasm_str = disasm_cached_op(op);
        fprintf(dependency_chain_log_file, "[PC: 0x%08llx] OpNum:%-10llu H2p:%s Disasm: %-45s\n",
            op->inst_info->addr, 
            op->op_num,
            op->oracle_info.hbt_pred_is_hard ? "O" : "X", 
            disasm_str);
    }
    fprintf(dependency_chain_log_file, "-------------------------- END LOG ---------------------------\n\n");
    fflush(dependency_chain_log_file);
}

void log_dependency_chain_block(uns proc_id, Dependency_Chain_Cache_Entry* entry, Counter cycle_count) {
    if (!block_cache_log_file || !entry || !entry->is_valid ||
        !(cycle_count >= DEBUG_CYCLE_START && cycle_count <= DEBUG_CYCLE_STOP)) return;

    fprintf(block_cache_log_file, "--- [LOG] Dependency Chain Block for Core %u Cycle:%-4llu---\n", proc_id, cycle_count);
    fprintf(block_cache_log_file, "Index PC(Block Starting PC): 0x%llx, OpNum: %llu\n",
            entry->h2p_branch_pc, entry->h2p_branch_op_num);

    // --- [ New code to print the bitmask ] ---
    char mask_str[65]; // 64 bits + null terminator
    // BUGFIX: Cap to 64 to prevent buffer overflow
    int mask_bits = entry->total_ops_in_block > 64 ? 64 : entry->total_ops_in_block;
    // Iterate from 0 to mask_bits to build the binary string
    for(int i = 0; i < mask_bits; ++i) {
        // Check if the i-th bit is set in the mask
        mask_str[i] = (entry->dependency_mask >> i) & 1 ? '1' : '0';
    }
    mask_str[mask_bits] = '\0'; // Add null terminator
    fprintf(block_cache_log_file, "Block Length: %-3u Dependency Mask: %s\n", 
            entry->total_ops_in_block, mask_str);
    // ---------------------------------------------
    
    fprintf(block_cache_log_file, "------------------------------------------------------------------\n");
    fprintf(block_cache_log_file, "Instructions in Block (Total: %u):\n", entry->chain_length);


    for (int i = 0; i < entry->chain_length; ++i) {
        Op* op = &entry->chain[i];
        char* disasm_str = disasm_cached_op(op);
        fprintf(block_cache_log_file, "[PC: 0x%08llx] OpNum:%-10llu H2p:%s Disasm: %-45s\n",
                op->inst_info->addr, 
                op->op_num,
                op->oracle_info.hbt_pred_is_hard ? "O" : "X", 
                disasm_str);
    }
    fprintf(block_cache_log_file, "-------------------------- END LOG ---------------------------\n\n");
    fflush(block_cache_log_file);
}

void log_full_cache_state(uns proc_id, Counter cycle_count) {
    if (!dependency_chain_log_file ||
        !(cycle_count >= DEBUG_CYCLE_START && cycle_count <= DEBUG_CYCLE_STOP)) {
        return;
    }

    fprintf(dependency_chain_log_file, "\n=============== [CACHE DUMP] for Core %u @ Cycle:%-6llu ===============\n", proc_id, cycle_count);

    // Assuming you are dumping the block_caches, not dependency_chain_caches
    Dependency_Chain_Cache_Entry* cache = block_caches[proc_id];

    for (int i = 0; i < BLOCK_CACHE_SIZE; i++) {
        Dependency_Chain_Cache_Entry* entry = &cache[i];

        if (entry->is_valid) {
            // --- [ Modified Part: Added Mask Info ] ---
            char mask_str[65];
            int mask_bits = entry->total_ops_in_block > 64 ? 64 : entry->total_ops_in_block;
            for(int k = 0; k < mask_bits; ++k) {
                mask_str[k] = (entry->dependency_mask >> k) & 1 ? '1' : '0';
            }
            mask_str[mask_bits] = '\0';

            fprintf(dependency_chain_log_file, "[Index %-4d] PC: 0x%08llx | OpNum: %-10llu | BlockLen: %-2u | Mask: %s\n",
                    i,
                    entry->h2p_branch_pc,
                    entry->h2p_branch_op_num,
                    entry->total_ops_in_block,
                    mask_str);
            // ---------------------------------------------

            fprintf(dependency_chain_log_file, "             ChainLen: %u\n", entry->chain_length);

            for (int j = 0; j < entry->chain_length; j++) {
                Op* op = &entry->chain[j];
                char* disasm_str = disasm_cached_op(op); // Ensure this helper exists
                fprintf(dependency_chain_log_file, "             |--> [%3d] PC: 0x%08llx | OpNum: %-10llu | H2P: %c | %s\n",
                        j,
                        op->inst_info->addr,
                        op->op_num,
                        op->oracle_info.hbt_pred_is_hard ? 'O' : 'X',
                        disasm_str);
            }
        }
    }
    fprintf(dependency_chain_log_file, "=========================== END CACHE DUMP ===========================\n\n");
    fflush(dependency_chain_log_file);
}