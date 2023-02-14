#include <stdint.h>

#include "absl/flags/declare.h"

ABSL_DECLARE_FLAG(bool, sat_use_coarse_grain_queues);
ABSL_DECLARE_FLAG(uint64_t, sat_memory);
ABSL_DECLARE_FLAG(uint64_t, sat_reserve_memory);
ABSL_DECLARE_FLAG(uint64_t, sat_hugepage_memory);
ABSL_DECLARE_FLAG(uint32_t, sat_runtime);
ABSL_DECLARE_FLAG(int32_t, sat_memory_threads);
ABSL_DECLARE_FLAG(uint32_t, sat_invert_threads);
ABSL_DECLARE_FLAG(uint32_t, sat_check_threads);
ABSL_DECLARE_FLAG(bool, sat_test_cache_coherence);
ABSL_DECLARE_FLAG(uint32_t, sat_cache_increment_count);
ABSL_DECLARE_FLAG(uint32_t, sat_cache_line_size);
ABSL_DECLARE_FLAG(uint32_t, sat_cache_line_count);
ABSL_DECLARE_FLAG(bool, sat_test_cpu_frequency);
ABSL_DECLARE_FLAG(uint32_t, sat_cpu_frequency_threshold);
ABSL_DECLARE_FLAG(uint32_t, sat_cpu_frequency_round);
