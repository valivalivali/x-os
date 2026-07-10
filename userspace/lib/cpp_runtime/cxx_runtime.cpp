// C++ runtime support for freestanding x-os environment
// Provides: operator new/delete, __cxa_guard_*, __cxa_pure_virtual, __cxa_atexit, __dso_handle

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <new>

// operator new (weak - may be overridden by library code)
__attribute__((weak)) void* operator new(size_t size) {
    return malloc(size);
}

__attribute__((weak)) void* operator new[](size_t size) {
    return malloc(size);
}

// operator delete
__attribute__((weak)) void operator delete(void* ptr) noexcept {
    free(ptr);
}

__attribute__((weak)) void operator delete[](void* ptr) noexcept {
    free(ptr);
}

__attribute__((weak)) void operator delete(void* ptr, size_t) noexcept {
    free(ptr);
}

__attribute__((weak)) void operator delete[](void* ptr, size_t) noexcept {
    free(ptr);
}

// __cxa_guard for static local variable initialization
// Layout: byte 0 = initialized flag, bytes 1-3 = padding, bytes 4-7 = mutex
static uint8_t __guard_mutex = 0;

extern "C" {

int __cxa_guard_acquire(uint64_t* guard) {
    uint8_t* initialized = (uint8_t*)guard;
    if (*initialized != 0)
        return 0; // already initialized
    // Simple spinlock (single-threaded OS, so this is fine)
    while (__sync_lock_test_and_set(&__guard_mutex, 1)) {
        // spin
    }
    if (*initialized != 0) {
        __sync_lock_release(&__guard_mutex);
        return 0;
    }
    return 1;
}

void __cxa_guard_release(uint64_t* guard) {
    uint8_t* initialized = (uint8_t*)guard;
    *initialized = 1;
    __sync_lock_release(&__guard_mutex);
}

void __cxa_guard_abort(uint64_t* guard) {
    (void)guard;
    // Called if initialization throws - we don't have exceptions
}

void __cxa_pure_virtual() {
    // Pure virtual function called - halt
    while (1) { }
}

// __cxa_atexit - just do nothing (no real atexit support needed)
static void* __dso_handle_value = nullptr;
void* __dso_handle = &__dso_handle_value;

typedef void (*dtor_func)(void*);
int __cxa_atexit(dtor_func func, void* arg, void* dso) {
    (void)func; (void)arg; (void)dso;
    return 0; // success - we just don't register destructors
}

// __cxa_thread_atexit - no-op (no threads)
int __cxa_thread_atexit(dtor_func func, void* arg, void* dso) {
    (void)func; (void)arg; (void)dso;
    return 0;
}

// new_handler support - return nullptr (no handler installed)
std::new_handler get_new_handler() noexcept {
    return nullptr;
}

// __throw_bad_alloc - abort since we have no exceptions
void __throw_bad_alloc() {
    while (1) { }
}

} // extern "C"
