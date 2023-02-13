#include <stdlib.h>

#include "absl/flags/declare.h"

ABSL_DECLARE_FLAG(bool, sat_use_coarse_grain_queues);
ABSL_DECLARE_FLAG(int64_t, sat_memory);
ABSL_DECLARE_FLAG(int64_t, sat_reserve_memory);
ABSL_DECLARE_FLAG(int64_t, sat_hugepage_memory);
