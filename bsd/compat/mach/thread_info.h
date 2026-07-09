#pragma once
#include "mach_types.h"
#define THREAD_BASIC_INFO 1
struct thread_basic_info { int user_time; int system_time; };
