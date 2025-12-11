#ifndef __TEA_FETCH_LOG_H__
#define __TEA_FETCH_LOG_H__

#include "globals/global_types.h"
#include "op.h"
#include "dependency_chain_cache.h"
#include <stdio.h>

/**
 * @brief Initializes the TEA fetch logger.
 * Creates tea_fetch.log in OUTPUT_DIR for tracking Block Cache lookups.
 */
void init_tea_fetch_log(void);

/**
 * @brief Closes the TEA fetch log file.
 */
void close_tea_fetch_log(void);

/**
 * @brief Logs a Block Cache lookup event from Shadow FTQ path.
 * 
 * @param proc_id Processor ID
 * @param cycle_count Current simulation cycle
 * @param lookup_pc PC used for Block Cache lookup (tea_op->inst_info->addr)
 * @param block_entry Block Cache entry (NULL if miss)
 * @param ft_index Index of Op within the FT
 * @param ft_op_count Total number of Ops in the FT
 * @param in_chain Whether Op passed dependency mask filtering
 */
void log_tea_block_cache_lookup(uns proc_id, Counter cycle_count,
                                  Addr lookup_pc,
                                  Dependency_Chain_Cache_Entry* block_entry,
                                  uns ft_index, uns ft_op_count,
                                  Flag in_chain);

/**
 * @brief Logs a Block Cache lookup event from fallback path.
 * 
 * @param proc_id Processor ID
 * @param cycle_count Current simulation cycle
 * @param fetch_addr Current fetch address
 * @param block_entry Block Cache entry (NULL if miss)
 */
void log_tea_fallback_fetch(uns proc_id, Counter cycle_count,
                             Addr fetch_addr,
                             Dependency_Chain_Cache_Entry* block_entry);

#endif // __TEA_FETCH_LOG_H__
