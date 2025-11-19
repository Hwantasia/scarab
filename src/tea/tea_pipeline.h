#ifndef __TEA_PIPELINE_H__
#define __TEA_PIPELINE_H__

#include "globals/global_types.h"

void tea_pipeline_cycle(uns proc_id);
uns tea_backend_rs_usage(uns proc_id);
uns tea_backend_fu_usage(uns proc_id);

#endif /* __TEA_PIPELINE_H__ */
