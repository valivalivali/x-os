#pragma once
#include "mach_types.h"
struct host_basic_info {
    int max_cpus;
    int avail_cpus;
    int cpu_type;
    int cpu_subtype;
};
#define HOST_BASIC_INFO 1
