/* Atomic function stubs - provided separately to avoid conflicts with
 * static inline definitions in machine/atomic.h */
typedef unsigned int u_int;

void atomic_store_rel_int(volatile u_int *p, u_int v) {
    __asm__ __volatile__("" ::: "memory");
    *p = v;
}

int atomic_cmpset_int(volatile u_int *p, u_int old, u_int new) {
    return __sync_bool_compare_and_swap(p, old, new);
}
