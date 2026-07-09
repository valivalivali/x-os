#pragma once
#include "../compat.h"
#ifndef assert
#define assert(x) do { if (!(x)) panic("assert: %s", #x); } while(0)
#endif
