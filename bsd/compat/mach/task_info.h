#pragma once
#include "mach_types.h"
struct task_basic_info {
    int suspended;
    int resident_size;
    int virtual_size;
};
#define TASK_BASIC_INFO 1
