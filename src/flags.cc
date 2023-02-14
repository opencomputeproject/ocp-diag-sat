#include "flags.h"

#include <stdint.h>

#include "absl/flags/flag.h"

ABSL_FLAG(
    bool, sat_use_coarse_grain_queues, false,
    "Whether to use coarse or fine grain lock queues during testing. By "
    "default fine grain lock queues will be used as they are more efficient.");

ABSL_FLAG(
    uint64_t, sat_memory, 0,
    "The amount of RAM to test in Megabytes. A value of 0 (the "
    "default) indicates that all free memory should be tested, minus a "
    "reserve for system processes. This reserve will be 15\% of all memory on "
    "systems with less than 2Gb of memory, and 5\% of all memory plus "
    "192Mb on larger systems.");

ABSL_FLAG(uint64_t, sat_reserve_memory, 0,
          "The minimum amount of RAM, in Megabytes, to reserve for other "
          "processes during the test if hugepages are not being used.");

ABSL_FLAG(uint64_t, sat_hugepage_memory, 0,
          "The minimum amount of hugepage RAM to test in Megabytes.");

ABSL_FLAG(uint32_t, sat_runtime, 20,
          "The desired duration of the stress test, in seconds.");

ABSL_FLAG(int32_t, sat_memory_threads, -1,
          "The number of memory copy threads to run. By default, this will "
          "equal the number of CPU cores.");

ABSL_FLAG(
    uint32_t, sat_invert_threads, 0,
    "The number of memory invert threads to run. None will be run by default.");

ABSL_FLAG(
    uint32_t, sat_check_threads, 0,
    "The number of memory check threads to run. None will be run by default.");

ABSL_FLAG(bool, sat_test_cache_coherence, false,
          "Whether to run the CPU Cache Coherence test, which verifies the "
          "CPU cache by incrementing counters from threads running on "
          "different CPU cores.");

ABSL_FLAG(uint32_t, sat_cache_increment_count, 1000,
          "The number of times that the shared counter should be incremented "
          "when verifying cache coherence. The default value is 1000.");

ABSL_FLAG(
    uint32_t, sat_cache_line_size, 0,
    "The size of an individual line in the CPU cache, in bytes. This is used "
    "for the cache coherence test and is automatically determined by default.");

ABSL_FLAG(uint32_t, sat_cache_line_count, 2,
          "The amount of cache-line-sized data structures to use for the cache "
          "coherence test. The default is 2.");
