#pragma once
#include "../compat.h"
/* Lock types already defined in compat.h */
#define lck_attr_alloc_init(name) ((void*)1)
#define lck_grp_alloc_init(name, attr) ((lck_grp_t*)1)
#define lck_grp_free(g) (void)g
#define lck_attr_free(a) (void)a

typedef int lck_attr_t;
typedef int decl_lck_mtx_data_t;
typedef int decl_lck_spin_data_t;

#define LCK_ATTR_NULL NULL
#define LCK_GRP_NULL  NULL
