#ifndef __TEA_EARLY_RECOVERY_LOG_H__
#define __TEA_EARLY_RECOVERY_LOG_H__

#include "globals/global_types.h"
#include "op.h"
#include <stdio.h>

/**
 * @brief Initializes the TEA early recovery logger.
 * Creates tea_early_recovery.log in OUTPUT_DIR.
 */
void init_tea_early_recovery_log(void);

/**
 * @brief Closes the TEA early recovery log file.
 */
void close_tea_early_recovery_log(void);

/**
 * @brief Logs detailed information about a TEA early recovery event.
 * 
 * @param proc_id Processor ID
 * @param cycle_count Current simulation cycle
 * @param main_branch Main thread branch Op
 * @param tea_op TEA thread Op
 * @param binding_type String describing how TEA Op was matched to Main Op
 * @param is_frontend_only Whether this is a frontend-only recovery
 */
void log_tea_early_recovery(uns proc_id, Counter cycle_count, 
                             Op* main_branch, Op* tea_op,
                             const char* binding_type, Flag is_frontend_only);

#endif // __TEA_EARLY_RECOVERY_LOG_H__
