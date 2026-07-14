//! Runtime support for the no_std staticlib.
//! The allocator calls into the C runtime's malloc/free,
//! and the panic handler logs and loops.

use core::alloc::{GlobalAlloc, Layout};

#[global_allocator]
static ALLOCATOR: XosAllocator = XosAllocator;

struct XosAllocator;

unsafe extern "C" {
    fn malloc(size: usize) -> *mut u8;
    fn free(ptr: *mut u8);
}

unsafe impl GlobalAlloc for XosAllocator {
    unsafe fn alloc(&self, layout: Layout) -> *mut u8 {
        malloc(layout.size())
    }
    unsafe fn dealloc(&self, ptr: *mut u8, _layout: Layout) {
        free(ptr);
    }
}

#[panic_handler]
fn panic_handler(_info: &core::panic::PanicInfo) -> ! {
    loop {}
}
