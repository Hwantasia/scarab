#ifndef __TEA_H2P_FETCH_LOG_H__
#define __TEA_H2P_FETCH_LOG_H__

#include "globals/global_types.h"
#include "op.h"

/**
 * @brief Initializes the TEA H2P branch fetch tracking log
 * Creates tea_h2p_fetch.out to track when H2P branches are fetched from TEA Fetch Queue
 */
void init_tea_h2p_fetch_log(void);

/**
 * @brief Closes the TEA H2P fetch log file
 */
void close_tea_h2p_fetch_log(void);

/**
 * @brief Logs when an H2P branch is fetched from TEA Fetch Queue
 * Shows where the corresponding Main Op is located at that moment
 * 
 * @param proc_id Processor ID
 * @param cycle_count Current simulation cycle
 * @param tea_op TEA H2P branch Op being fetched
 */
void log_tea_h2p_fetch(uns proc_id, Counter cycle_count, Op* tea_op);

#endif // __TEA_H2P_FETCH_LOG_H__
